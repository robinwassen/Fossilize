# Linux robust controller lifecycle audit

Baseline: `/home/robin/dev/fossilize`, HEAD `2325a40846cceb9e053cc5b26aad4c030e07e1a2`; relevant source previously compared equal to upstream `556da9ff7d7616704dda515756219ab1dab693ec`. Read project index, descriptor, pillars, verification guide before probes. No production edits. Source inspection plus small extracted-helper probes only; no GPU or Steam integration exercised.

## Path matrix

| Event/path | Implementation / result | Assessment |
|---|---|---|
| Controller prepare metadata/databases | hpp976–1111 opens/exports metadata and counts workload before forking | Errors return failure before workers; no explicit preparation deadline |
| Controller signal/event setup | hpp1125–1233 resets dispositions, blocks SIGCHLD/INT/TERM, installs signalfd, periodic timer, optional sentinel socket | Normal arrangement supports serialized lifecycle handling; errors before workers return failure |
| Initial range partition / empty range | hpp1238–1253 and 513–518 | Fixed range per slot, empty slot no process; finite job, not persistent service pool |
| Create pipes/fork | hpp521–529 | Pipe2/fork failure leaves created FDs unclosed until process exit; ordinary process-scoped leak, no proven persistent orphan |
| Parent initialization of child | hpp530–551 stores PID, synchronously writes faulty-module blacklist, closes redundant ends, increments active count, adds pipe to epoll | Blocking startup handshake finding A; epoll-add failure returns with child, eventually parent death protection applies |
| Child early setup | hpp553–623 closes inherited sensitive descriptors, arms PDEATHSIG + race check, restores signal mask, redirects stdin/stdout/stderr | Correct normal parent-death backstop; erroneous bool return finding C on setup failure |
| Child loads blacklist | hpp1650–1664 blocks fgets until input EOF | Paired with startup handshake; no watchdog armed yet |
| Driver device initialization | cpp2025–2047 sends BEGIN_HEARTBEAT before device init | Covers device init hang; preparation before this marker not bounded by heartbeat |
| Task reuse / completion | cpp1747–1818 persistent thread loop; cpp4459–4482 synchronizes then joins | Tasks reuse workers; normal completed range exits; no routine replacement or work stealing |
| Worker protocol | hpp143–278,281–298 parse crash/markers/heartbeat from pipe | Read is blocking; safe with current pipe readiness, unsafe with stale slot event, finding B |
| Heartbeat watchdog | hpp300–332,1295–1311 | 10s once armed, resets on heartbeat; deliberately ignores expiry while controller-stopped |
| Compilation timeout | cpp928–973, hpp1589–1602 | Counts no-completion intervals and signals compile thread, then recovery path; distinct from worker heartbeat |
| Crash cleanup watchdog | hpp158–181,1284–1292,1511–1534 | CRASH installs 3s fallback, parent can kill while emergency driver teardown hangs |
| Recoverable death | hpp335–413,937–969 | waitpid reaps old PID, flushes pipe, advances ranges from markers, replacement only if remaining work and enough progress metadata |
| Unrecoverable death / no markers | hpp371–390 | Stops that slot; records dirty counter when shared control exists. Master may still report complete/success, finding D |
| Normal worker exit | hpp352–354 | Removes FDs, decrements active count, no replacement |
| SIGCHLD coalescing / lost signal resilience | hpp937–969,1353–1367 | waitpid(-1,WNOHANG) loop plus 1s periodic reap; no ordinary unreaped-zombie claim |
| Throttle downward | hpp746–815,831–848 | SIGSTOP selected workers; allocations retained intentionally, resource policy rather than orphan defect |
| Resume | hpp849–869 | Rearms heartbeat then SIGCONT. Crash cleanup timer not similarly rearmed; can kill paused crash recovery, usually bounded fallback rather than leak |
| Static RUNNING_TARGET=0 | hpp875–880 | Can intentionally leave all workers paused until caller changes target/cancels; not defect by itself |
| Sentinel EOF | hpp909–934 | EOF normally SIGKILLs every tracked child; DETACH deliberately disables this behavior. Error recv except EAGAIN only closes socket, no kill; could matter on hard socket errors but EOF usual path works |
| SIGINT/SIGTERM controller | hpp1335–1342,896–907 | Explicit SIGKILL including stopped workers, expect_kill prevents usual replacement; continues event loop to reap |
| SIGKILL/controller crash | hpp566–574 | Kernel PDEATHSIG kills workers after setup. Pre-registration race handled by getppid comparison. Does not prove all process/thread caller contexts are correct (external audit handles that) |
| Controller loop fatal error | hpp1260–1265,1324–1327 etc | Returns failure without explicit kill/reap; process exit normally triggers PDEATHSIG. Could leave brief adopted zombies but no sustained active orphan proven |
| Master finalization | hpp1382–1429 | active_processes==0 marks progress complete then optional module-identifier merge and success; no workers retained after normal end |
| Worker final teardown | hpp1703–1730 and cpp destructor | Driver teardown after thread joins remains covered by existing heartbeat timer; when no markers, watchdog death can become unrecoverable |

## Candidate A: controller can deadlock sending fault blacklist if replacement dies before reading

**Conditional defect; helper-level reproduction.** At hpp537 `send_faulty_modules_and_close(input_fds[1])` is synchronous and precedes closing the controller's own `input_fds[0]` at539. It can fill the pipe once sufficient faulty module hashes accumulate. If child died or stalled before consuming stdin, the controller's read-end reference prevents broken-pipe detection, so write blocks indefinitely. SIGINT/TERM are already blocked for signalfd; event loop is not running during this call, so cancellation/control EOF/watchdogs cannot be processed. Other workers may continue running while their controller is stuck. PDEATHSIG does not help while controller remains alive.

Trigger: enough accumulated faulty module entries to fill pipe, and child killed/OOM/setup allocation failure/hang before or during blacklist read. First spawn has empty blacklist; this concerns later recovery with accumulated errors. Not evidence of common user reports.

Probe: `/tmp/fossilize-linux-controller-probes/blacklist_pipe.cpp` contains unchanged production `write_all` and `send_faulty_modules_and_close`, same close ordering and blocked SIGTERM. 4096 16-digit hashes; worker exits before read; controlled supervisor tests and kills/reaps only own controller. Build `g++ -std=c++17 -O0 -Wall -Wextra .../blacklist_pipe.cpp -o .../blacklist_pipe`; output `returned_before_SIGTERM=0 returned_after_SIGTERM=0 controller_exited=0` (probe exit0 means expected deadlock mechanism observed). No whole binary execution.

Fix direction: close unused read end before write; bounded/nonblocking handshake managed in event loop, startup deadline, handle transfer errors.

## Candidate B: stale epoll events can be dispatched against replacement slot descriptors

**Static conditional race + modeled helper reproduction; not naturally reproduced end-to-end.** epoll event data stores process slot index (hpp542–545; timer event indices158–181,300–316), not PID/generation/fd. `poll_children_process_states` restarts slot immediately at hpp962. `epoll_wait` has already returned an array; another old pipe-ready event in that array can subsequently call current `proc.process_once()` at1314–1317. That reads current replacement `crash_fd`, which may have no bytes and is blocking. Controller then cannot service cancellation until replacement writes/closes it. Old watchdog event can analogously act on new timer descriptor (read1302); generation protection is absent.

Feasible precondition to independently check: one epoll batch includes SIGCHLD and old pipe readiness; SIGCHLD handled first, recovery restarts slot; old pipe event follows. Parent-loop source permits ordering, but probe models this ordering rather than inducing actual recovery. New worker typically sends BEGIN_HEARTBEAT fairly soon; pathological preparation can make delay unbounded.

Probe `/tmp/fossilize-linux-controller-probes/stale_pipe.cpp` extracts unchanged `ProcessProgress::process_once`, caches actual old epoll event, replaces slot descriptor, dispatches cached event. Supervisor observes `stale_event_dispatch_returned=0 (0 means blocked on replacement pipe)` and kills/reaps own controller. This proves blocking mechanism under modeled precondition, not natural event ordering occurrence.

Fix direction: generation-tag epoll registrations, reject stale events; make pipes/timerfd nonblocking and drain EAGAIN safely.

## Candidate C: child setup failures return success into controller code

**Static proof, fault not injected.** `start_child_process` returns bool. In fork child, failed `pthread_sigmask` at577 or `dup2` at606/608 executes `return EXIT_FAILURE` (integer1 -> true). Caller at1249 treats it as success, proceeds running controller loop from fork child after closing controller signal/epoll/timer FDs; with additional slots remaining it can fork its own additional children. This is incorrect control flow for failure. Ordinary valid descriptors make dup2 failure rare; EINTR or injected syscall failure makes branch reachable. `_exit(EXIT_FAILURE)` is appropriate in child. No claim that current reports originate here.

## Candidate D: terminal completion loses failed/cancelled distinction

**Static proof; established observability gap, no orphan inference.** Unrecoverable worker death returns false and removes slot (hpp371–390), just like normal completion; after all slots disappear master sets progress_complete (1382–83) and returns EXIT_SUCCESS (1429). Explicit cancellation SIGKILL also leads same success path. Shared counters may disclose failures if caller reads them; raw exit status does not. This violates a strong completion/completeness contract but does not show workers persist. Should link existing observability backlog rather than duplicate findings if already recorded.

## Claims rejected / qualified

- Successful workers are not routinely respawned to fill idle CPUs; only recovery replaces them.
- Worker processes present after throttle can be intentionally stopped. That is not itself orphaning.
- Zombies are dead processes; they do not keep compiling or retain ordinary user address-space memory.
- Current controller does explicitly kill stopped workers on cancellation and reaps via two mechanisms.
- Current PDEATHSIG setup means generic old reports of indefinite active children surviving controller death cannot simply be asserted current.
- Startup rollback FD omissions alone do not establish persistent orphaning because master normally exits and kernel kills children.
- Fixed ranges and no stealing are performance design tradeoffs, not lifecycle defects by themselves.
- Memory retained under SIGSTOP is resource policy; no hard RSS bound promised here.
- Linux fork/exec parent-thread semantics and Windows paths need the other auditors, not inferred from this source.

## Candidate E: cancellation can respawn an already-failed worker

**Strong conditional defect; unchanged full helper + real OS wait status reproduced.** `shutdown_processes` sets every tracked slot's expect_kill=true and calls SIGKILL(hpp896–907). However `process_shutdown` only suppresses retry if wait status specifically says SIGKILL AND expect_kill (356–359). If a worker has already exited with code2 and recovery markers, but remains unreaped when a SIGTERM or control-EOF cancellation event is handled first, sending SIGKILL succeeds for the zombie but does not alter its exit status. When reaped, exit2 with markers+remaining work returns true (396–412); `poll_children_process_states` immediately starts a replacement (962), despite cancellation. No controller-global shutdown flag blocks that spawn, and new slot expect_kill remains true but is not checked in start_child_process; throttle logic skips expected-kill slots, so replacement can run to end. Sentinel EOF has already been closed, so no subsequent EOF cancels it again.

Probe `/tmp/fossilize-linux-controller-probes/cancel_respawn.cpp` extracts the entire unchanged process_shutdown method. Real child exits2; waitid WEXITED|WNOWAIT deterministically ensures exited-unreaped status; cancel sets flag and SIGKILLs; waitpid returns exit2; markers2 of end20 model ordinary recoverable crash. Output `cancel_kill_result=0 actual_exit_code=2 expect_kill=1 restart_requested=1`. Probe0 means defect observed. Parent dispatcher actual restart is source-proven, not executed by this helper. External-owner agent assigned independent checker.

Fix direction: sticky cancellation state before killing, checked unconditionally before any restart; do not use observed termination cause as sole cancellation guard.

## Additional normal-thread audit

`ThreadedReplayer::tear_down_threads` (cpp1888–1899) sets shutting_down, notifies, then unconditional joins. It cannot interrupt a thread stuck in driver code. In robust Linux mode the post-BEGIN_HEARTBEAT parent watchdog is a separate process and can SIGKILL hung teardown; thus join itself is not enough to prove leaked robust workers. In standalone mode there is no parent watchdog and join may be unbounded. That is a limitation of selecting nonrobust execution, not by itself an established contract violation. Early normal_process failures after worker creation can also enter join through destructor without executing explicit sync_worker_memory_context timeout. Do not claim configured compilation no-progress timeout guarantees standalone teardown deadline.
