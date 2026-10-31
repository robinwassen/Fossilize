#pragma once

#include "fossilize.hpp"
#include <array>
#include <cstdlib>
#include <cstdint>
#include <mutex>

// Private replay result, independent of optional shared progress. Counts are
// failure observations (possibly several per item), never queue completions.
// Keep bounded first evidence for each reason rather than retaining every blob.
enum class ReplayFailure : unsigned { Read, Parse, Create, Unsupported, Validation, Setup, Dependency, Count };
class ReplayItemOutcome
{
public:
	struct Evidence
	{
		uint64_t count = 0;
		Fossilize::ResourceTag tag = Fossilize::RESOURCE_COUNT;
		Fossilize::Hash hash = 0;
		VkResult result = VK_SUCCESS;
	};
	using Snapshot = std::array<Evidence, unsigned(ReplayFailure::Count)>;
	void fail(ReplayFailure reason, Fossilize::ResourceTag tag = Fossilize::RESOURCE_COUNT,
	          Fossilize::Hash hash = 0, VkResult result = VK_SUCCESS)
	{
		std::lock_guard<std::mutex> holder(lock);
		auto &e = evidence[unsigned(reason)];
		if (!e.count) { e.tag = tag; e.hash = hash; e.result = result; }
		e.count++;
	}
	Snapshot snapshot() const { std::lock_guard<std::mutex> holder(lock); return evidence; }
	unsigned failure_mask() const
	{
		unsigned mask = 0; auto copy = snapshot();
		for (unsigned i = 0; i < copy.size(); i++) if (copy[i].count) mask |= 1u << i;
		return mask;
	}
	bool succeeded() const
	{
		for (const auto &e : snapshot()) if (e.count) return false;
		return true;
	}
	int exit_status() const { return succeeded() ? EXIT_SUCCESS : EXIT_FAILURE; }
private:
	mutable std::mutex lock;
	Snapshot evidence = {};
};
