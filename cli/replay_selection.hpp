// Private replay selection: discovery, policy exclusions and execution are separate facts.
#pragma once
#include "fossilize_db.hpp"
#include <algorithm>
#include <unordered_set>
#include <limits>

namespace Fossilize
{
enum class ReplayExclusionPolicy { Whitelist, Blacklist, Range, HashSelection, ReplayV1Hint, OptionalInput };
struct ReplaySelectionExclusion
{
	DiscoveryKey key;
	ReplayExclusionPolicy policy;
};
struct ReplaySelectionRange
{
	unsigned requested_begin = 0, requested_end = ~0u;
	unsigned effective_begin = 0, effective_end = 0;
};
struct ReplaySelectionReport
{
	DatabasePreparationReport preparation;
	bool recovered_prefix_requested = false;
	bool preparation_succeeded = false;
	bool scope_satisfied = false;
	bool hash_found = true;
	bool selected_total_known = false;
	bool optional_inputs_excluded = false;
	Hash requested_hash = 0;
	ReplaySelectionRange ranges[3];
	std::vector<Hash> stable_hashes[3]; // Selected before hints; indices preserve robust resume.
	std::vector<DiscoveryKey> selected, dispatch;
	std::vector<ReplaySelectionExclusion> excluded;
	std::vector<DiscoveryInput> optional_unavailable;
	// Describes this optimization only; no physical compilation/cache-hit assertion.
	const char *hint_policy = "replay-v1/device-mode/pipelineCacheUUID";
};
static const ResourceTag replay_pipeline_tags[3] = {
	RESOURCE_GRAPHICS_PIPELINE, RESOURCE_COMPUTE_PIPELINE, RESOURCE_RAYTRACING_PIPELINE
};

inline bool replay_inputs_unavailable(const DatabasePreparationReport &preparation)
{
	if (preparation.inputs.empty())
		return false;
	for (auto &input : preparation.inputs)
	{
		if (input.required || input.state == DiscoveryState::Complete || input.state == DiscoveryState::RecoveredPrefix)
			return false;
	}
	return true;
}

inline void configure_replay_inputs(DatabaseInterface &db, size_t count, const std::vector<unsigned> &optional)
{
	for (size_t i = 0; i < count; i++)
	{
		unsigned index = unsigned(db.has_sub_databases() ? i + 1 : 0);
		bool required = std::find(optional.begin(), optional.end(), unsigned(i)) == optional.end();
		db.set_input_required(index, required);
	}
}

inline ReplaySelectionReport select_replay_scope(DatabaseInterface &db, const unsigned *starts,
                                                const unsigned *ends, Hash hash, bool prefix,
                                                const std::unordered_set<Hash> *hints = nullptr,
                                                bool preparation_succeeded = true)
{
	ReplaySelectionReport report;
	report.preparation = db.get_preparation_report();
	report.preparation_succeeded = preparation_succeeded;
	report.recovered_prefix_requested = prefix;
	report.scope_satisfied = report.preparation.satisfies_required_scope(prefix);
	report.requested_hash = hash;
	report.hash_found = !hash;
	for (auto &input : report.preparation.inputs)
	{
		if (!input.required && input.state != DiscoveryState::Complete)
			report.optional_unavailable.push_back(input);
	}
	for (auto &entry : report.preparation.excluded)
	{
		auto policy = entry.policy == DiscoveryExclusion::Blacklist ?
		              ReplayExclusionPolicy::Blacklist : ReplayExclusionPolicy::Whitelist;
		report.excluded.push_back({entry.key, policy});
	}
	// A failed optional-only backend is excluded as an input, including every
	// known key observed before failure. Retained keys are never dispatched from
	// an interface whose prepare() failed. Original discovery uncertainty survives.
	if (!preparation_succeeded || replay_inputs_unavailable(report.preparation))
	{
		report.optional_inputs_excluded = !report.preparation.inputs.empty();
		for (auto &input : report.preparation.inputs)
			report.optional_inputs_excluded &= !input.required;
		report.scope_satisfied &= report.optional_inputs_excluded && report.hash_found;
		if (report.optional_inputs_excluded)
		{
			report.optional_unavailable = report.preparation.inputs;
			for (auto key : report.preparation.discovered)
				report.excluded.push_back({key, ReplayExclusionPolicy::OptionalInput});
		}
		for (unsigned i = 0; i < 3; i++)
		{
			report.ranges[i].requested_begin = starts[i];
			report.ranges[i].requested_end = ends[i];
		}
		report.selected_total_known = report.scope_satisfied;
		return report;
	}
	for (unsigned i = 0; i < 3; i++)
	{
		auto tag = replay_pipeline_tags[i];
		size_t count = 0;
		if (!db.get_hash_list_for_resource_tag(tag, &count, nullptr) || count > std::numeric_limits<unsigned>::max())
		{
			report.scope_satisfied = false;
			continue;
		}
		std::vector<Hash> universe(count);
		if (!db.get_hash_list_for_resource_tag(tag, &count, universe.data()))
		{
			report.scope_satisfied = false;
			continue;
		}
		std::sort(universe.begin(), universe.end());
		universe.erase(std::unique(universe.begin(), universe.end()), universe.end());
		auto &range = report.ranges[i];
		range.requested_begin = starts[i];
		range.requested_end = ends[i];
		range.effective_end = std::min<unsigned>(unsigned(universe.size()), ends[i]);
		range.effective_begin = std::min(range.effective_end, starts[i]);
		for (unsigned j = 0; j < universe.size(); j++)
		{
			DiscoveryKey key{tag, universe[j]};
			bool include = hash ? key.hash == hash : j >= range.effective_begin && j < range.effective_end;
			if (!include)
			{
				report.excluded.push_back({key, hash ? ReplayExclusionPolicy::HashSelection : ReplayExclusionPolicy::Range});
				continue;
			}
			if (hash)
			{
				report.hash_found = true;
				range.effective_begin = j;
				range.effective_end = j + 1;
			}
			report.stable_hashes[i].push_back(key.hash);
			report.selected.push_back(key);
			if (hints && hints[tag].count(key.hash))
				report.excluded.push_back({key, ReplayExclusionPolicy::ReplayV1Hint});
			else
				report.dispatch.push_back(key);
		}
	}
	report.scope_satisfied &= report.hash_found;
	report.selected_total_known = report.scope_satisfied;
	return report;
}
}
