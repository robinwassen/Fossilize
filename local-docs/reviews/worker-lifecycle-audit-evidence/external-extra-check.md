# Final external lifecycle probe and independent checker appendix

Baseline HEAD2325a40846cceb9e053cc5b26aad4c030e07e1a2, Linux probes, Windows static only. No production changes.

## E6: wait can reap an unrelated host child and return that child's status — VALID, REPRODUCED

Source `/tmp/external-owner-audit-probes/probe.cpp`, mode `wait-steals-child`. Same exact compile command in external-owner report. Public ExternalReplayer starts controlled helper that exits37 without publishing progress_complete. Host additionally forks its own unrelated child exiting42. Harness uses `waitid(P_PID, child, ..., WEXITED|WNOWAIT)` on BOTH before calling replay.wait(): both statuses are ready, neither has been reaped. No timing guess or race lottery.

Call chain:
- wait415 calls poll_progress.
- poll_progress239 reaps replay exit37; reset_pid244 sets pid=-1 and closes child sentinel.
- wait451 then calls waitpid(-1,...,WNOHANG), reaping unrelated child42.
- wait487 poll sees pidnegative; wait returns cached unrelated42.
- Host later waitpid(unrelated,...,WNOHANG) returns -1/ECHILD proving ownership theft.

Observed:
```
wait_status=42 expected_replay=37 unrelated_waitpid=-1 unrelated_errno=10 expected_unrelated=42
```
Exit1 deliberately denotes observed failed expectation. Probe killed/reaped only exact own PIDs if needed; both were already reaped by subject code. This is broader than lost replay status E3: an unrelated application subprocess's status is consumed. Requires target child already exited before wait initial poll and no progress_complete marker (early error, cancellation, helper). Ordinary successful completed marker skips initial waitpid, so do not imply every wait steals.

## W3: Windows missing general heartbeat supervision — VALID STATIC GAP

Independently read actual source: worker emits BEGIN_HEARTBEAT and HEARTBEAT at cli/fossilize_replay_windows.hpp1233-1242. Parser117-245 recognizes neither. ProcessProgress struct has timer_handle only, armed by CRASH119-138. Wait set at908-925 contains process, pipe, existing crash timer and infinite timeout. Thus living worker stalled before CRASH outside shared batch completion timeout has no active supervisor deadline. Shared batch timeout only active cpp928-973; final teardown and init can escape it.

Classification: source-proven coverage gap, not proven real hang, not API wait contract violation. Header explicitly documents blocking wait; a watchdog across initialization/destruction is a desired reliability property, not currently a documented guarantee. Explicit kill and successful job close remain possible fallback; do not claim Windows can never kill a hung worker. No Windows compilation/native execution performed.

## W4: Windows abandoned mutex handling — VALID STATIC DEFECT

Independently checked external poll155-165 and supervisor parse149 etc. Each enters critical section and ReleaseMutex only on WAIT_OBJECT_0. Microsoft documents WAIT_ABANDONED grants ownership to current thread, requiring consistency check and eventual release: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject . Return80 therefore leaves ownership acquired but unreleased.

Microsoft ReleaseMutex documentation confirms recursive acquisitions require one release each: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-releasemutex . Therefore a later call on same thread acquiring/releases once does not repair missing release; other processes/threads remain blocked until holder thread exits. Dead controller -> host receives abandoned ownership; a later different host thread can hang. Host polling thread dying while owner host stays alive -> controller can acquire abandoned ownership and hold it, blocking future host polling. Whole host death with functional kill-on-close job kills controller, so don't present that alone as surviving-controller scenario.

Classification: concrete wrong handling of OS result, conditional on owner-thread termination while mutex held. Static source plus authoritative OS semantics, no native Windows reproduction. Ring state may be inconsistent, so treating abandonment as ordinary success without validation is not a sufficient fix.

## Independent controller cancellation-race check — VALID, method-level reproduction

Inspected `/tmp/fossilize-linux-controller-probes/cancel_respawn.cpp`, compared unchanged process_shutdown method with current source. Ran binary:
```
cancel_kill_result=0 actual_exit_code=2 expect_kill=1 restart_requested=1
```
Zombie exit2 already available via WNOWAIT when shutdown marks expect_kill and kill(SIGKILL) succeeds; signal cannot alter alreadyexited status. Guard only suppresses retry for WIFSIGNALED/SIGKILL, so valid recovery markers cause true retry. Controller reaper962 starts replacement; start_child_process508 has no cancellation check. expect_kill remains true but merely prevents later pause/resume, not initial execution. No global sticky stopping flag gates fork. Thus cancellation can spawn a replacement when crash and cancellation overlap. Proof scope: actual extracted production recovery decision + real child wait status + full-source caller analysis; not full controller event-loop concurrency reproduction. Existing healthy-worker shutdown normally suppresses replacement, so no blanket claim that all cancellation fails.
