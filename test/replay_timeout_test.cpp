#include "diagnostic_test.hpp"
#include "../cli/replay_timeout.hpp"
#include "../cli/replay_worker_outcome.hpp"
#include <climits>

static DiagnosticChecks checks;
static void deadlines()
{
	ReplayDeadline work, heartbeat, cleanup;
	work.arm(100, replay_seconds_ns(2)); heartbeat.arm(100, replay_seconds_ns(10));
	cleanup.arm(100, replay_seconds_ns(3));
	checks.require(!work.expired(100 + replay_seconds_ns(2) - 1), "M16/strict-monotonic-boundary");
	checks.require(work.expired(100 + replay_seconds_ns(2)), "M16/exact-deadline");
	uint64_t late = 100 + replay_seconds_ns(4) + 123;
	checks.require(work.trigger(late).elapsed_ns == replay_seconds_ns(4) + 123, "M16/measured-overshoot-not-threshold");
	work.arm(late, replay_seconds_ns(2));
	checks.require(cleanup.expired(late) && !heartbeat.expired(late) && !work.expired(late), "M16/work-progress-only-resets-work");
	heartbeat.arm(late, replay_seconds_ns(10));
	checks.require(cleanup.start_ns == 100 && work.start_ns == late, "M16/heartbeat-only-resets-heartbeat");
	work.arm(late, 0); checks.require(!work.expired(UINT64_MAX), "M16/zero-disables-deadline");
	checks.require(replay_seconds_ns(UINT_MAX) == uint64_t(UINT_MAX) * 1000000000ull, "M16/unsigned-seconds-no-overflow");
	checks.require(cleanup.elapsed(99) == 0, "M16/backward-clock-no-underflow");
}
static void delivery()
{
	ReplayTimeoutHistory history; history.begin(1);
	auto &e = history.current;
	for (unsigned i = 1; i <= 3; i++)
	{
		ReplayDeadline d; d.arm(10, replay_seconds_ns(i));
		auto trigger = d.trigger(10 + replay_seconds_ns(i) + 71);
		e.record(ReplayTimeoutOrigin(i), trigger);
		// The adapter is called only after the retained cause exists.
		auto adapter = [&]() { checks.require(e.triggers[i - 1].present && e.triggers[i - 1].elapsed_ns == trigger.elapsed_ns, "M16/cause-before-adapter"); return i == 1 ? 22u : 0u; };
		e.request(ReplayTermination(i), ReplayDelivery::Requested, 0);
		unsigned error = adapter();
		e.request(ReplayTermination(i), error ? ReplayDelivery::Failed : ReplayDelivery::Accepted, error);
	}
	checks.require(e.requests[0].delivery == ReplayDelivery::Failed && e.requests[0].native_error == 22 && !e.native_observed,
	               "M16/delivery-error-distinct-from-native-effect");
	e.native_observed = true; e.native_status = 9; e.recovery = ReplayRecovery::Retry;
	for (unsigned i = 0; i < 100; i++) e.request(ReplayTermination::KillSignal, ReplayDelivery::Failed, 5);
	checks.require(e.request_count == 4 && e.dropped_requests > 0 && e.requests[0].native_error == 22, "M16/bounded-first-delivery-evidence");
	for (unsigned i = 2; i < 100; i++) history.begin(i);
	checks.require(history.previous_count == 4 && history.dropped_attempts > 0 && history.previous[0].native_status == 9 &&
	               history.previous[0].triggers[0].present && history.previous[0].requests[0].native_error == 22,
	               "M16/bounded-attempts-preserve-first-cause-error-and-native-effect");
}
static void unknown()
{
	ReplayWorkerOutcome r; r.assign(0, 2, 0, 0, 0, 0); r.timeout.begin(1);
	r.timeout.current.native_observed = true; r.timeout.current.native_status = 9;
	checks.require(r.finish(false, false, -1, -1, -1) == WorkerDisposition::Unresolved && r.unknown_remainder && !r.skipped(), "M16/external-kill-unknown-item-and-work");
	for (auto &t : r.timeout.current.triggers) checks.require(!t.present, "M16/external-kill-is-not-timeout-or-oom");
	ReplayDeadline cleanup; cleanup.arm(100, replay_seconds_ns(3));
	checks.require(!cleanup.expired(100 + replay_seconds_ns(2)), "M16/spontaneous-fault-no-timeout-before-cleanup-deadline");
	ReplayWorkerOutcome known; known.assign(0, 2, 0, 0, 0, 0); known.culprit(0, 1, 0xabc);
	checks.require(known.skipped() && known.assigned[0].skipped_hash == 0xabc, "M16/independent-item-evidence-retained");
}
static void protocol()
{
	ReplayAttemptEvidence e;
	const char *invalid[] = {"TIMEOUT", "TIMEOUT 1 2000000000", "TIMEOUT 1 2000000000 1", "TIMEOUT 2 10000000000 11000000000", "TIMEOUT 1 -1 999", "TIMEOUT 1 18446744073709551616 18446744073709551616", "TIMEOUT 1 2000000000 2500000000 suffix", "TIMEOUT 1 2000000001 2500000000", "TERMINATE 1 2 22"};
	for (auto text : invalid) { replay_parse_timeout(e,text); checks.require(!e.triggers[0].present && !e.request_count, "M16/strict-extension-no-invented-cause"); }
	checks.require(!replay_parse_timeout(e,"GRAPHICS 2 abc"), "M16/legacy-grammar-remains-separate");
	replay_parse_timeout(e,"TIMEOUT 1 2000000000 2500000000");
	replay_parse_timeout(e,"TIMEOUT 1 3000000000 3500000000");
	checks.require(e.triggers[0].threshold_ns == 2000000000ull && e.triggers[0].elapsed_ns == 2500000000ull, "M16/duplicate-cannot-overwrite-origin");
	replay_parse_timeout(e,"TERMINATE 1 0 0"); replay_parse_timeout(e,"TERMINATE 1 2 22");
	checks.require(e.request_count == 1 && e.requests[0].native_error == 22, "M16/strict-delivery-raw-error");
}
int main(int argc, char **argv)
{
	std::string name = argc > 1 ? argv[1] : "";
	if (name == "deadlines" || name == "reset") deadlines();
	else if (name == "delivery") delivery();
	else if (name == "unknown") unknown();
	else if (name == "protocol") protocol();
	else return 2;
	printf("Executed M16/unit-%s failures=%d\n", name.c_str(), checks.failures);
	return checks.failures ? 1 : 0;
}
