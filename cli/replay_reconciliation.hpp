#pragma once

#include "replay_event.hpp"
#include "replay_selection.hpp"
#include <memory>

namespace Fossilize
{
enum class ReplayTerminal : uint8_t
{
	Unknown, Created, CacheHint, Unsupported, Dependency, ReadFailed, ParseFailed, CreateFailed,
	ValidationFailed, OtherFailed, CrashSkipped, NoExecution, Blacklisted, Count
};

inline bool replay_terminal_satisfied(ReplayTerminal status)
{
	return status == ReplayTerminal::Created || status == ReplayTerminal::CacheHint ||
	       status == ReplayTerminal::NoExecution;
}

inline unsigned replay_pipeline_kind(ResourceTag tag)
{
	for (unsigned i = 0; i < 3; i++)
		if (tag == replay_pipeline_tags[i])
			return i;
	return 3;
}

struct ReplayIndexRange
{
	unsigned begin = 0, end = 0;

	bool contains(unsigned index) const { return begin <= index && index < end; }
	bool contains(ReplayIndexRange range) const { return begin <= range.begin && range.begin <= range.end && range.end <= end; }
};

using ReplayAssignment = std::array<ReplayIndexRange, 3>;

struct ReplayTerminalFact
{
	unsigned worker = 0, attempt = 0;
	ResourceTag tag = RESOURCE_COUNT;
	unsigned index = 0;
	Hash hash = 0;
	ReplayTerminal status = ReplayTerminal::Unknown;
};

inline size_t replay_terminal_frame(char *out, size_t size, const ReplayTerminalFact &fact)
{
	char payload[112];
	int len = std::snprintf(payload, sizeof(payload), "1 ITEM %u %u %u %u %llu %u",
	                       fact.worker, fact.attempt, unsigned(fact.tag), fact.index,
	                       (unsigned long long)fact.hash, unsigned(fact.status));
	return len > 0 && size_t(len) < sizeof(payload) ? ReplayEvent::frame(out, size, payload) : 0;
}

inline bool replay_decode_terminal(const char *payload, size_t size, ReplayTerminalFact &fact)
{
	constexpr char prefix[] = "1 ITEM ";
	if (size < sizeof(prefix) || std::memcmp(payload, prefix, sizeof(prefix) - 1))
		return false;
	ReplayEvent::Numbers nums{payload + sizeof(prefix) - 1, payload + size};
	uint64_t worker, attempt, tag, index, hash, status;
	if (!nums.number(worker) || !nums.next(attempt) || !nums.next(tag) || !nums.next(index) ||
	    !nums.next(hash) || !nums.next(status) || nums.p != nums.end ||
	    worker > UINT32_MAX || !attempt || attempt > UINT32_MAX || tag >= RESOURCE_COUNT ||
	    index > UINT32_MAX || !status || status >= unsigned(ReplayTerminal::Count) ||
	    status == unsigned(ReplayTerminal::CrashSkipped))
		return false;
	fact = {unsigned(worker), unsigned(attempt), ResourceTag(tag), unsigned(index), hash, ReplayTerminal(status)};
	return true;
}

class ReplayReconciliation
{
public:
	struct Item
	{
		// Keep the latest two distinct observations and the latest observed
		// failure, even when it precedes both retained detailed observations.
		uint32_t attempt[2] = {};
		uint32_t failure_attempt = 0;
		uint16_t mask[2] = {};
		uint16_t failure_mask = 0;
		bool history_omitted = false;

		ReplayTerminal status() const
		{
			for (unsigned i = 1; i < unsigned(ReplayTerminal::Count); i++)
				if (mask[0] == 1u << i)
					return ReplayTerminal(i);
			return ReplayTerminal::Unknown;
		}

		void observe(unsigned number, ReplayTerminal terminal)
		{
			uint16_t bit = uint16_t(1u << unsigned(terminal));
			if (!replay_terminal_satisfied(terminal) && number >= failure_attempt)
			{
				if (number > failure_attempt)
					failure_mask = 0;
				failure_attempt = number;
				failure_mask |= bit;
			}
			for (unsigned i = 0; i < 2; i++)
			{
				if (number == attempt[i])
				{
					mask[i] |= bit;
					return;
				}
				if (number > attempt[i])
				{
					std::swap(number, attempt[i]);
					std::swap(bit, mask[i]);
					if (!number)
						return;
				}
			}
			history_omitted = true;
		}
	};
	static_assert(sizeof(Item) == 20, "Replay reconciliation item storage");
	static_assert(unsigned(ReplayTerminal::Count) <= 16, "Terminal masks must cover every status");
	using Counts = std::array<uint64_t, unsigned(ReplayTerminal::Count)>;

	// Retain only the selected hashes and their offsets after discovery.
	void initialize(const ReplaySelectionReport &report)
	{
		selected_total_known = report.selected_total_known;
		discovery_complete = report.preparation.discovery_complete;
		recovered_prefix = report.scope_satisfied && report.recovered_prefix_requested &&
			std::any_of(report.preparation.inputs.begin(), report.preparation.inputs.end(),
			            [](const DiscoveryInput &input) { return !input.filter && input.state == DiscoveryState::RecoveredPrefix; });
		optional_scope = report.scope_satisfied && !report.optional_unavailable.empty();

		for (unsigned i = 0; i < 3; i++)
		{
			auto &items = selected[i];
			auto &range = report.ranges[i];
			if (!selected_total_known && !discovery_complete &&
			    !report.requested_hash && (range.requested_begin != 0 || range.requested_end != ~0u))
			{
				// Unread hashes could change the absolute indices of observed keys.
				items = {};
				continue;
			}
			const auto &hashes = report.stable_hashes[i];
			items.begin = range.effective_begin;
			items.count = hashes.size();
			items.hashes.reset(items.count ? new Hash[items.count] : nullptr);
			items.states.reset(items.count ? new Item[items.count] : nullptr);
			if (items.count)
				std::copy(hashes.begin(), hashes.end(), items.hashes.get());
		}
	}

	ReplayAssignment assignment(unsigned worker = 0, unsigned workers = 1) const
	{
		ReplayAssignment ranges = {};
		if (!workers || worker >= workers)
			return ranges;
		for (unsigned i = 0; i < 3; i++)
		{
			uint64_t count = selected[i].size();
			ranges[i] = {selected[i].begin + unsigned(count * worker / workers),
			             selected[i].begin + unsigned(count * (uint64_t(worker) + 1) / workers)};
		}
		return ranges;
	}

	const Item *item(ResourceTag tag, unsigned index, Hash hash) const
	{
		unsigned kind = replay_pipeline_kind(tag);
		if (kind >= 3)
			return nullptr;
		const auto &items = selected[kind];
		if (index < items.begin || index - items.begin >= items.size() ||
		    items.hashes[index - items.begin] != hash)
			return nullptr;
		return &items.states[index - items.begin];
	}

	bool accept(unsigned worker, unsigned attempt, const ReplayAssignment &original,
	            const ReplayAssignment &active, const ReplayTerminalFact &fact)
	{
		unsigned kind = replay_pipeline_kind(fact.tag);
		if (!attempt || fact.worker != worker || fact.attempt != attempt || kind >= 3 ||
		    !original[kind].contains(active[kind]) || !active[kind].contains(fact.index) ||
		    fact.status <= ReplayTerminal::Unknown || fact.status >= ReplayTerminal::Count)
			return false;
		const Item *found = item(fact.tag, fact.index, fact.hash);
		if (!found)
			return false;
		selected[kind].states[fact.index - selected[kind].begin].observe(attempt, fact.status);
		return true;
	}

	struct Coverage
	{
		unsigned confirmed_end;
		bool unknown = false;
		bool satisfied = true;
	};

	Coverage coverage(unsigned kind, ReplayIndexRange range) const
	{
		Coverage result{range.begin};
		if (kind >= 3 || range.begin < selected[kind].begin || range.end < range.begin ||
		    uint64_t(range.end) - selected[kind].begin > selected[kind].size())
			return {range.begin, true, false};
		const auto &items = selected[kind];
		for (unsigned index = range.begin; index < range.end; index++)
		{
			auto status = items.states[index - items.begin].status();
			result.unknown |= status == ReplayTerminal::Unknown;
			result.satisfied &= replay_terminal_satisfied(status);
			if (index == result.confirmed_end && status != ReplayTerminal::Unknown && status != ReplayTerminal::CrashSkipped)
				result.confirmed_end++;
		}
		return result;
	}

	Counts counts() const
	{
		Counts result = {};
		for (const auto &items : selected)
			for (size_t i = 0; i < items.size(); i++)
				result[unsigned(items.states[i].status())]++;
		return result;
	}

	size_t item_capacity() const { return selected[0].size() + selected[1].size() + selected[2].size(); }
	size_t storage_bytes() const { return sizeof(*this) + (sizeof(Hash) + sizeof(Item)) * item_capacity(); }
	size_t conflicted_items() const
	{
		size_t conflicts = 0;
		for (const auto &items : selected)
			for (size_t i = 0; i < items.size(); i++)
			{
				uint16_t mask = items.states[i].mask[0];
				conflicts += (mask & (mask - 1)) != 0;
			}
		return conflicts;
	}

	size_t omitted_item_histories() const
	{
		size_t omitted = 0;
		for (const auto &items : selected)
			for (size_t i = 0; i < items.size(); i++)
				omitted += items.states[i].history_omitted;
		return omitted;
	}

	const char *scope() const
	{
		if (!selected_total_known) return "incomplete";
		if (recovered_prefix && optional_scope) return "recovered-prefix+optional-inputs";
		if (recovered_prefix) return "recovered-prefix";
		if (optional_scope) return "optional-inputs";
		return "full";
	}

	bool selected_total_known = false;
	bool discovery_complete = false;

private:
	struct Selection
	{
		std::unique_ptr<Hash[]> hashes;
		std::unique_ptr<Item[]> states;
		size_t count = 0;
		unsigned begin = 0;
		size_t size() const { return count; }
	};
	Selection selected[3];
	bool recovered_prefix = false, optional_scope = false;
};
}
