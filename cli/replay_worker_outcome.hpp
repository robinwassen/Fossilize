/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <cstdint>
#include <climits>
#include "replay_timeout.hpp"
#include "replay_reconciliation.hpp"

// Legacy markers still carry scheduler cursors. Only complete, bounded
// numeric fields can also supply candidate evidence for a skipped item.
inline bool replay_crash_cursor(const char *fields, int &resume, Fossilize::Hash &hash)
{
	using Fossilize::ReplayEvent::Numbers;
	Numbers nums{fields, fields + std::strlen(fields)};
	uint64_t index;
	if (!nums.next(index) || index > INT_MAX || nums.p == nums.end || *nums.p++ != ' ')
		return false;
	hash = 0;
	unsigned digits = 0;
	for (; nums.p != nums.end; nums.p++)
	{
		unsigned char c = *nums.p;
		unsigned digit;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
		else return false;
		if (++digits > 16)
			return false;
		hash = (hash << 4) | digit;
	}
	if (!digits)
		return false;
	resume = int(index);
	return true;
}

// Internal controller state. This is deliberately separate from optional shared
// progress and from the mutable range used to launch the next attempt.
enum class WorkerDisposition { Complete, Retry, Unresolved, Failed };

struct ReplayWorkerOutcome
{
	ReplayTimeoutHistory timeout;
	Fossilize::ReplayEvent::State events;
	std::array<Fossilize::ReplayEvent::State, 4> previous_events = {};
	unsigned previous_event_count = 0, omitted_event_attempts = 0;
	void begin_events(unsigned worker)
	{
		if (events.offered)
		{
			if (previous_event_count < previous_events.size()) previous_events[previous_event_count++] = events;
			else omitted_event_attempts++;
		}
		events.begin(worker, attempts);
	}
	struct Range
	{
		unsigned begin = 0, end = 0;
		unsigned confirmed_end = 0;
		uint64_t skipped_hash = 0;
		unsigned skipped_index = 0;
	};
	Range assigned[3]; // graphics, compute, ray tracing; half-open
	unsigned attempts = 0, failed_attempts = 0;
	uint64_t termination = 0;
	unsigned native_error = 0;
	bool initialized = false;
	bool unknown_remainder = false;
	bool complete = false;
	bool controlled_failure = false;
	unsigned failure_mask = 0;
	bool orderly_reported = false;
	bool orderly_drained = false;

	struct Attempt
	{
		Fossilize::ReplayAssignment ranges;
		unsigned number = 0;
		WorkerDisposition disposition = WorkerDisposition::Unresolved;
	};
	struct CrashMarker
	{
		unsigned resume = 0;
		Fossilize::Hash hash = 0;
		bool conflicting = false;
	};
	Fossilize::ReplayReconciliation *reconciliation = nullptr;
	Fossilize::ReplayAssignment original;
	Attempt active;
	std::array<Attempt, 4> previous_attempts = {};
	unsigned previous_attempt_count = 0, omitted_attempts = 0;
	CrashMarker markers[3];
	bool dependency_crash = false, crash_reported = false;

	void assign(unsigned gb, unsigned ge, unsigned cb, unsigned ce, unsigned rb, unsigned re)
	{
		if (initialized)
			return;
		original = {{{gb, ge}, {cb, ce}, {rb, re}}};
		for (unsigned i = 0; i < original.size(); i++)
		{
			assigned[i].begin = assigned[i].confirmed_end = original[i].begin;
			assigned[i].end = original[i].end;
		}
		initialized = true;
	}

	bool begin_attempt(unsigned worker, const Fossilize::ReplayAssignment &ranges)
	{
		if (attempts == UINT32_MAX)
			return false;
		if (active.number)
		{
			if (previous_attempt_count < previous_attempts.size())
				previous_attempts[previous_attempt_count++] = active;
			else
				omitted_attempts++;
		}
		attempts++;
		active = {ranges, attempts, WorkerDisposition::Unresolved};
		for (auto &marker : markers)
			marker = {};
		dependency_crash = false;
		crash_reported = false;
		timeout.begin(attempts);
		begin_events(worker);
		return true;
	}

	void accept_frame(const char *payload, size_t length)
	{
		if (length < 6 || std::memcmp(payload, "1 ITEM", 6))
		{
			events.accept(payload, length);
			return;
		}

		Fossilize::ReplayTerminalFact fact;
		if (!reconciliation || !events.hello || events.final ||
		    !Fossilize::replay_decode_terminal(payload, length, fact) ||
		    !reconciliation->accept(unsigned(events.worker), active.number, original, active.ranges, fact))
			events.lost = true;
	}

	void culprit(unsigned kind, int resume, uint64_t hash)
	{
		if (!hash || resume <= 0)
			return;
		auto &marker = markers[kind];
		if (!marker.hash)
		{
			marker.resume = unsigned(resume);
			marker.hash = hash;
		}
		else if (marker.hash != hash || marker.resume != unsigned(resume))
			marker.conflicting = true;
	}

	bool skipped() const
	{
		return assigned[0].skipped_hash || assigned[1].skipped_hash || assigned[2].skipped_hash;
	}

	WorkerDisposition finish_controlled()
	{
		timeout.current.recovery = ReplayRecovery::Failed;
		controlled_failure = true;
		complete = false;
		update_coverage();
		return active.disposition = WorkerDisposition::Failed;
	}

	WorkerDisposition finish(bool clean, bool can_retry, int graphics, int compute, int raytracing)
	{
		if (native_error) { clean = false; can_retry = false; }
		if (clean && (orderly_reported || !events.successful())) { clean = false; can_retry = false; }
		if (clean)
		{
			complete = update_coverage() && !skipped();
			timeout.current.recovery = complete ? ReplayRecovery::Complete : ReplayRecovery::Unresolved;
			return active.disposition = complete ? WorkerDisposition::Complete : WorkerDisposition::Unresolved;
		}

		failed_attempts++;
		const int progress[] = { graphics, compute, raytracing };
		bool remaining = false;
		for (unsigned i = 0; i < 3; i++)
		{
			auto range = active.number ? active.ranges[i] : original[i];
			if (range.begin == range.end)
				continue;
			if (progress[i] < 0 || unsigned(progress[i]) < range.begin || unsigned(progress[i]) > range.end)
				can_retry = false;
			else
				remaining |= unsigned(progress[i]) < range.end;
		}
		if (can_retry && crash_reported && !dependency_crash)
			mark_skipped(progress);
		if (can_retry && remaining)
		{
			timeout.current.recovery = ReplayRecovery::Retry;
			return active.disposition = WorkerDisposition::Retry;
		}
		update_coverage();
		timeout.current.recovery = ReplayRecovery::Unresolved;
		return active.disposition = WorkerDisposition::Unresolved;
	}

	void refresh_coverage() { update_coverage(); }

private:
	void mark_skipped(const int *progress)
	{
		if (!reconciliation)
			return;
		for (unsigned i = 0; i < 3; i++)
		{
			const auto &marker = markers[i];
			if (!marker.hash || marker.conflicting || marker.resume != unsigned(progress[i]))
				continue;
			Fossilize::ReplayTerminalFact fact{unsigned(events.worker), active.number, Fossilize::replay_pipeline_tags[i],
			                                  marker.resume - 1, marker.hash, Fossilize::ReplayTerminal::CrashSkipped};
			if (reconciliation->accept(fact.worker, fact.attempt, original, active.ranges, fact) &&
			    !assigned[i].skipped_hash)
			{
				assigned[i].skipped_hash = fact.hash;
				assigned[i].skipped_index = fact.index;
			}
		}
	}

	bool update_coverage()
	{
		bool satisfied = true;
		unknown_remainder = false;
		for (unsigned i = 0; i < 3; i++)
		{
			if (original[i].begin == original[i].end)
				continue;
			if (!reconciliation)
			{
				satisfied = false;
				unknown_remainder = true;
				continue;
			}
			auto coverage = reconciliation->coverage(i, original[i]);
			assigned[i].confirmed_end = coverage.confirmed_end;
			unknown_remainder |= coverage.unknown;
			satisfied &= coverage.satisfied;
		}
		return satisfied;
	}
};
