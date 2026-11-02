#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

// Private replay/controller evidence. Durations are monotonic nanoseconds;
// thresholds are durations, never timestamps or counts of scheduler wakeups.
enum class ReplayTimeoutOrigin : unsigned { NoProgress = 1, Heartbeat = 2, Cleanup = 3 };
enum class ReplayTermination : unsigned { AbortSignal = 1, KillSignal = 2, WindowsRecovery = 3, TerminateProcess = 4 };
enum class ReplayDelivery : unsigned { Requested = 0, Accepted = 1, Failed = 2 };
enum class ReplayRecovery : unsigned { None, CrashCleanup, Retry, Complete, Unresolved, Failed };

static inline uint64_t replay_monotonic_ns()
{
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
static inline uint64_t replay_seconds_ns(unsigned seconds)
{
	// All unsigned CLI values fit in uint64_t nanoseconds, including UINT_MAX.
	return uint64_t(seconds) * 1000000000ull;
}
// Epoll payloads use arithmetic bit fields, never union member layout.
static inline uint64_t replay_event_token(uint32_t id, unsigned attempt = 0)
{ return (uint64_t(attempt) << 32) | id; }
static inline uint32_t replay_event_id(uint64_t token) { return uint32_t(token); }
static inline unsigned replay_event_attempt(uint64_t token) { return unsigned(token >> 32); }

struct ReplayTimeoutTrigger
{
	bool present = false;
	uint64_t threshold_ns = 0, elapsed_ns = 0;
};
struct ReplayDeadline
{
	uint64_t start_ns = 0, threshold_ns = 0;
	bool armed = false;
	void arm(uint64_t now, uint64_t threshold) { start_ns = now; threshold_ns = threshold; armed = threshold != 0; }
	uint64_t elapsed(uint64_t now) const { return now >= start_ns ? now - start_ns : 0; }
	bool expired(uint64_t now) const { return armed && elapsed(now) >= threshold_ns; }
	ReplayTimeoutTrigger trigger(uint64_t now) const { return {true, threshold_ns, elapsed(now)}; }
};
struct ReplayTerminationRequest
{
	ReplayTermination mechanism = ReplayTermination::AbortSignal;
	ReplayDelivery delivery = ReplayDelivery::Requested;
	unsigned native_error = 0;
};
struct ReplayAttemptEvidence
{
	unsigned attempt = 0;
	std::array<ReplayTimeoutTrigger, 3> triggers = {};
	std::array<ReplayTerminationRequest, 4> requests = {};
	unsigned request_count = 0, dropped_requests = 0;
	bool native_observed = false;
	uint64_t native_status = 0;
	ReplayRecovery recovery = ReplayRecovery::None;
	bool record(ReplayTimeoutOrigin origin, const ReplayTimeoutTrigger &trigger)
	{
		auto &slot = triggers[unsigned(origin) - 1];
		if (slot.present || !trigger.present || !trigger.threshold_ns || trigger.elapsed_ns < trigger.threshold_ns) return false;
		slot = trigger;
		return true;
	}
	void request(ReplayTermination mechanism, ReplayDelivery delivery, unsigned error)
	{
		// A returning adapter updates only its outstanding request. Preserve
		// earlier failures rather than replacing them with a later success.
		if (delivery != ReplayDelivery::Requested && request_count &&
		    requests[request_count - 1].mechanism == mechanism &&
		    requests[request_count - 1].delivery == ReplayDelivery::Requested)
		{
			requests[request_count - 1].delivery = delivery;
			requests[request_count - 1].native_error = error;
		}
		else if (request_count < requests.size()) requests[request_count++] = {mechanism, delivery, error};
		else if (dropped_requests != std::numeric_limits<unsigned>::max()) dropped_requests++;
	}
};
struct ReplayTimeoutHistory
{
	ReplayAttemptEvidence current;
	std::array<ReplayAttemptEvidence, 4> previous = {};
	unsigned previous_count = 0, dropped_attempts = 0;
	void begin(unsigned attempt)
	{
		if (current.attempt)
		{
			if (previous_count < previous.size()) previous[previous_count++] = current;
			else if (dropped_attempts != std::numeric_limits<unsigned>::max()) dropped_attempts++;
		}
		current = {};
		current.attempt = attempt;
	}
};
static inline const char *replay_timeout_name(ReplayTimeoutOrigin origin)
{
	return origin == ReplayTimeoutOrigin::NoProgress ? "work-no-progress" :
	       origin == ReplayTimeoutOrigin::Heartbeat ? "heartbeat" : "post-crash-cleanup";
}

// Strict, bounded, decimal extension grammar. Old messages are unchanged and
// absence of an extension supplies no evidence about a fault's origin.
static inline bool replay_parse_numbers(const char *text, const char *prefix, uint64_t *values, unsigned count)
{
	size_t n = strlen(prefix);
	if (strncmp(text, prefix, n) != 0 || strlen(text) > 128) return false;
	text += n;
	for (unsigned i = 0; i < count; i++)
	{
		if (*text < '0' || *text > '9') return false;
		uint64_t value = 0;
		do
		{
			unsigned digit = unsigned(*text++ - '0');
			if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
			value = value * 10 + digit;
		} while (*text >= '0' && *text <= '9');
		values[i] = value;
		if (i + 1 < count && *text++ != ' ') return false;
	}
	return *text == '\0' || (*text == '\n' && text[1] == '\0');
}
static inline bool replay_parse_timeout(ReplayAttemptEvidence &evidence, const char *cmd)
{
	uint64_t v[3];
	if (replay_parse_numbers(cmd, "TIMEOUT ", v, 3))
	{
		// Only dispatcher work deadlines travel over the child pipe. Controller
		// watchdog/cleanup evidence comes from its own monotonic clock.
		if (v[0] == 1 && v[1] && v[1] <= replay_seconds_ns(~0u) && v[1] % 1000000000ull == 0 && v[2] >= v[1])
			evidence.record(ReplayTimeoutOrigin::NoProgress, {true, v[1], v[2]});
		return true;
	}
	if (replay_parse_numbers(cmd, "TERMINATE ", v, 3))
	{
		if ((v[0] == 1 || v[0] == 3) && v[1] <= 2 && v[2] <= std::numeric_limits<unsigned>::max() &&
		    (v[1] == 2 ? v[2] != 0 : v[2] == 0) && evidence.triggers[0].present)
			evidence.request(ReplayTermination(v[0]), ReplayDelivery(v[1]), unsigned(v[2]));
		return true;
	}
	return strncmp(cmd, "TIMEOUT", 7) == 0 || strncmp(cmd, "TERMINATE", 9) == 0;
}
