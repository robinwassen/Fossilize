# Independent check: Linux robust controller audit

Checker: lifecycle_rules agent, separate from linux_controller_audit inspector. Date 2026-10-29; baseline 2325a40846cceb9e053cc5b26aad4c030e07e1a2. Read report and actual source independently; reran both inspector probes. No production changes or full Fossilize/Steam execution.

## A — Bootstrap pipe can block supervisor: ACCEPTED with explicit prerequisites

Read source lines 506–551 and helper lines 44–58,415–426. Parent sets pid/crash_fd, calls synchronous send_faulty_modules_and_close, and only afterwards closes its own input_fds[0], increments active count, and registers crash pipe. Signals SIGINT/SIGTERM are blocked before spawn. A child that exits before reading no longer supplies a reader, but controller's own read reference keeps writes from failing EPIPE. Finite pipe fills with accumulated blacklist; supervisor blocks in write, outside event loop, so normal control/signal handling and watchdogs cannot execute. PDEATHSIG cannot help while controller remains alive.

Independent rerun inspector blacklist_pipe: returned_before_SIGTERM=0 returned_after_SIGTERM=0 controller_exited=0; exit0. I constructed negative control /tmp/fossilize-linux-controller-probes/check-blacklist-close-reader.cpp: same extracted helper, but close controller's redundant reader before send and ignore SIGPIPE so EPIPE can be returned. It reported returned_before_SIGTERM=1 returned_after_SIGTERM=1; exit0 means blocked-send mechanism disappears. controller_exited=0 in the negative print is just immediate waitpid scheduling; arrival of the completion byte already proves helper returned. This is mechanism-level positive/negative evidence, not whole-program fault injection.

Qualification: blacklist starts empty; sufficient accumulated failures plus a dead/stalled replacement reader are required. Worker death after fork is a reachable external event, but large blacklist accumulation and real Steam occurrence are not established. An external SIGKILL/process-group kill still terminates the blocked controller. Do not claim all cancellation methods fail.

## B — Cached old epoll event can affect replacement slot: ACCEPTED conditional race, not end-to-end reproduced

Actual registrations encode only slot index and event-kind bits; polling reaps/restarts a slot immediately while processing an already-returned event batch. Subsequent old event resolves current slot, and process_once reads current blocking fd. I reran stale_pipe: stale_event_dispatch_returned=0, exit0. Source-extracted process_once and helper model are accurate. However probe deliberately constructs the replacement-before-dispatch schedule, rather than obtaining it through the full controller. It proves blocking read under that schedule; it does not demonstrate natural recovery frequency or that SIGCHLD always comes first.

Attempted falsification: closing/deleting old epoll registration does not erase events already returned in the user buffer. Kernel documentation expressly warns about this event-cache closure pitfall, and recommends marking removed structures: https://man7.org/linux/man-pages/man7/epoll.7.html (event cache section). SIGCHLD need not be the first trigger: periodic timer also calls reap/restart before other entries in the returned batch. Generation-tagging or delayed replacement would address stale identity; nonblocking I/O would prevent the demonstrated blocking read. An old watchdog timer entry can similarly be applied to a replacement's timer, but its exact behavior depends on whether replacement timer already exists. Keep that second variant conditional.

## C — Fork child setup failure returns bool true into controller: ACCEPTED source-proven erroneous branch

Signature bool at506; failed pthread_sigmask returns EXIT_FAILURE at578; failed dup2 returns EXIT_FAILURE at607/609. EXIT_FAILURE==1 on this platform, converted to true. Both initial spawn caller and recovery caller interpret true as success, so child can return into controller code with its controller FDs already closed. Initial spawn may then create later slots itself. No contract justifies returning from the child setup branch; it needs terminal child exit. Runtime syscall failure not injected. The exact production impact varies by failing call and remaining slots, so do not claim every branch necessarily forks extra workers or survives indefinitely.

## D — Dirty/cancelled worker disappearance can lead to successful master completion: ACCEPTED source behavior, categorize separately

process_shutdown marks slot gone before classifying failure; unrecoverable paths return no-restart. Once active count reaches zero master writes progress_complete and returns EXIT_SUCCESS. Cancellation's expected SIGKILL also no-restarts and reaches same final success. Shared failure counters partially disclose details; successful master exit is not full requested replay proof. This is completion semantics/observability, not evidence of worker leakage. Existing observability backlog may already cover it.

## Other inspected claims

- Reaping loop plus periodic fallback exists; generic 'SIGCHLD coalescing leaves zombies forever' rejected.
- Worker PDEATHSIG registration and immediate parent-ID check correctly cover ordinary controller death after child startup; generic orphaning claim rejected for that case.
- Paused processes retaining memory and fixed partitions are intended policy/scheduling, not proof of lifecycle bugs.
- Missing reader/failed-bootstrap output can matter even before watchdog registration; this is A rather than duplicate finding.
- Startup FD rollback omissions are real but process exit normally releases FDs and kills children; no independent persistent active-orphan proof from those omissions alone.

All accepted impacts are scoped to documented schedules/preconditions; no empirical attribution to historical user issues or Steam deployment is made.

## E — Cancellation may restart an already-failed worker: ACCEPTED, independently reproduced recovery decision

This is stronger than the modeled stale-event case. I independently generated /tmp/fossilize-linux-controller-probes/check-cancel-restart.cpp containing unchanged production shutdown_processes and process_shutdown, with only IPC/epoll cleanup stubs and minimal state. A real owned child exits2; waitid WNOWAIT synchronizes that it exited without reaping it; shutdown_processes sets expect_kill and kills the still-owned PID; waitpid obtains actual exit2; process_shutdown returns true with remaining progress range. Output: cancelled=1 actual_exit=2 restart_requested=1 active_after_reap=0. Probe exit0.

Falsification/negative control: check-cancel-live.cpp instead cancels a live child. Output cancelled=1 actual_exit=-9 restart_requested=0 active_after_reap=0, exit0. Thus normal SIGKILL cancellation is correctly handled; the gap specifically concerns an exit status fixed before cancellation.

Source proof: expected-kill guard at process_shutdown lines356–360 is conditional on WIFSIGNALED && WTERMSIG==SIGKILL, rather than expect_kill alone; existing exit2 bypasses it, recovers from valid markers, and returns true. poll_children_process_states lines962 immediately calls start_child_process on true. start_child_process has no shutdown gate. A sentinel-EOF cancellation already closes the control FD, so it supplies no later event to cancel the replacement. Worker and controller parent-death protections do not fire while those parents remain alive. Remaining work may therefore continue after cancellation, bounded only by workload completion or a later kill; no unsupported claim of infinite execution is required.

The helper proves actual OS exit-status behavior and exact production recovery decision; it does not itself spawn the production replacement or run Steam. Root's separate complete-source harness can supply that stronger integration level.
