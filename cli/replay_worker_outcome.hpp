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
#include "replay_timeout.hpp"

// Internal controller state. This is deliberately separate from optional shared
// progress and from the mutable range used to launch the next attempt.
enum class WorkerDisposition { Complete, Retry, Unresolved, Failed };

struct ReplayWorkerOutcome
{
	ReplayTimeoutHistory timeout;
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

	WorkerDisposition finish_controlled()
	{
		timeout.current.recovery = ReplayRecovery::Failed;
		controlled_failure = true;
		complete = false;
		unknown_remainder = !orderly_drained;
		// Drained means processed, not a successfully replayed prefix.
		// A bounded category summary cannot certify individual item success.
		return WorkerDisposition::Failed;
	}

	void assign(unsigned gb, unsigned ge, unsigned cb, unsigned ce, unsigned rb, unsigned re)
	{
		if (initialized)
			return;
		assigned[0].begin = assigned[0].confirmed_end = gb; assigned[0].end = ge;
		assigned[1].begin = assigned[1].confirmed_end = cb; assigned[1].end = ce;
		assigned[2].begin = assigned[2].confirmed_end = rb; assigned[2].end = re;
		initialized = true;
	}

	void culprit(unsigned kind, int resume, uint64_t hash)
	{
		auto &r = assigned[kind];
		if (hash && resume > 0 && unsigned(resume - 1) >= r.begin && unsigned(resume - 1) < r.end)
		{
			// One culprit is sufficient to retain unsuccessful required work. Keep
			// the first rather than letting a later attempt erase the evidence.
			if (!r.skipped_hash)
			{
				r.skipped_hash = hash;
				r.skipped_index = unsigned(resume - 1);
			}
		}
	}

	bool skipped() const
	{
		return assigned[0].skipped_hash || assigned[1].skipped_hash || assigned[2].skipped_hash;
	}

	WorkerDisposition finish(bool clean, bool can_retry, int graphics, int compute, int raytracing)
	{
		if (native_error) { clean = false; can_retry = false; }
		if (clean)
		{
			for (auto &r : assigned) r.confirmed_end = r.skipped_hash ? r.skipped_index : r.end;
			unknown_remainder = false;
			complete = !skipped();
			timeout.current.recovery = complete ? ReplayRecovery::Complete : ReplayRecovery::Unresolved;
			return complete ? WorkerDisposition::Complete : WorkerDisposition::Unresolved;
		}
		failed_attempts++;
		int progress[] = { graphics, compute, raytracing };
		bool remaining = false;
		for (unsigned i = 0; i < 3; i++)
		{
			auto &r = assigned[i];
			if (r.begin == r.end) continue;
			if (progress[i] < 0 || unsigned(progress[i]) < r.confirmed_end || unsigned(progress[i]) > r.end)
				can_retry = false;
			else
			{
				unsigned known = unsigned(progress[i]);
				if (r.skipped_hash && known > r.skipped_index) known = r.skipped_index;
				if (known > r.confirmed_end) r.confirmed_end = known;
				remaining = remaining || unsigned(progress[i]) < r.end;
			}
		}
		if (can_retry && remaining)
		{
			timeout.current.recovery = ReplayRecovery::Retry;
			return WorkerDisposition::Retry;
		}
		// A failed final attempt cannot certify a successful assignment. Retain
		// uncertainty even if a cursor happened to reach the end of its range.
		unknown_remainder = !can_retry || remaining || !skipped();
		timeout.current.recovery = ReplayRecovery::Unresolved;
		return WorkerDisposition::Unresolved;
	}
};
