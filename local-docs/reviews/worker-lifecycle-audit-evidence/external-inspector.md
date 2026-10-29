# ExternalReplayer Linux lifecycle audit

Baseline: HEAD 2325a40846cceb9e053cc5b26aad4c030e07e1a2. Source unmodified. Research date 2026-10-29. Read local-docs README, descriptor, pillars and verification guide; inspected build inventory CMakeLists, cli/CMakeLists, test/CMakeLists, CMakePresets, populated submodules. No Steam changes or full GPU verification.

## Contract exclusions
- Header fossilize_external_replayer.hpp:174 says start may only be called once. Repeated start overwrites resources, but is invalid caller usage; do NOT report it as a supported lifecycle defect.
- Header :177-179 assigns caller responsibility for reaping unless wait used; is_process_complete also reaps. Destructor closes descriptors/mapping but does not itself wait, so destructor-alone zombies are not a proven contract violation.
- Header :193-195 explicitly prohibits kill with inherit_process_group=true. That mode makes killpg(pid) unsuitable by design; do NOT flag it as supported kill failure.
- start true only establishes fork, not exec success. Child exec errors subsequently reported; absence of synchronous exec handshake is a design choice unless stronger startup contract demanded.

## Path matrix
| Event | Implementation | Assessment |
|---|---|---|
| Construct | Impl defaults pid/fds=-1, shm=nullptr | Valid unstarted state, but poll_progress/resource getters dereference shm if caller invokes before start (API precondition not explicit). |
| shm_open failure | start889-894 false | No new descriptor, object destructible. |
| ftruncate/mmap/fcntl failure | start899-931 false | fd retained for destructor; shm namespace name not unlinked before these failures. mmap failure leaves MAP_FAILED destructor munmap call harmless fail. Named-shm leak is static. |
| shm_unlink failure | 934-938 false | Mapping/fd destructor closes, name remains if unlink truly failed. |
| first pipe failure | 940-942 false | Prior mapping cleaned by destructor. |
| second pipe failure | 944-946 false | First pipe local endpoints leaked. Reproduced. |
| socketpair failure | 948-950 false | Both preceding pipes local endpoints leaked. Static. |
| fork failure | 1005-1008 false | All local pipe/socketpair endpoints leaked. Static. |
| successful parent fork branch | 954-968 | Stores pid, kill readiness read end, control sender, child-tree death read end; closes corresponding child ends and shm fd. |
| child init prctl failure / raced parent death | 971-980 _exit | Terminates child; parent sees asynchronous failed child. |
| child create session failure | 987-993 exit | Readiness pipe closes at exit, kill call can fail ESRCH; child reaped via owner API. |
| exec failure | 817-825 exit(errno) | start already true; poll/wait discover failure. Postfork exit uses libc rather than _exit and allocations/environment calls; multithread host deadlock risk not reproduced here. |
| successful exec | child inherits intended shm/control/death endpoints | Unrelated host FDs also inherited, no whitelist/close-on-exec for host-owned endpoints; concurrent-instance sentinel interference reproduced. |
| normal poll alive | 226-303 | Reads shared atomics and FIFO. FIFO uses nonrobust cross-process lock (parent separately proves dead-owner lock hang). |
| poll child died before complete | 239-252 | Reaps and caches exit then reset_pid; same call may still report Running or ResultNotReady; next call Error. |
| is_process_complete child alive | 382-387 false | Standard waitpid WNOHANG. |
| is_process_complete child died before progress_complete | 382 then399 | Reaps correctly then poll_progress reaps again, overwrites cached status with -ECHILD. Reproduced. |
| wait child alive | 415 then442 blocking read | Waits sentinel; EINTR not retried, then 100ms waitpid retry can falsely succeed/forget live child. Reproduced. |
| wait child already dead before first poll | 415 reaps/reset;451 waitpid(-1) | Potential unrelated child reap; static and shares double-reap ordering root. |
| wait normal sentinel EOF | 451-489 | WNOHANG +100ms status poll, then reset descriptors. Child_fd inherits across replay descendant processes intentionally to wait process tree. |
| caller SIGCHLD reaps externally | 246-252,477-482 | Synthesizes -ECHILD, documented-ish limitation; cannot recover actual return status. |
| kill | 497-516 | Waits startup readiness pipe then group SIGKILL; caller still must wait/reap. Read EINTR makes kill returnfalse without retry. |
| destructor | 149-162 | Closes descriptors, shared mapping. Closing control sender normally triggers master's sentinel cleanup; no blocking wait. |
| creator thread exits | 972 PDEATHSIG | Kills replayer though host and owner object remain alive; no header main-thread restriction. Reproduced. |
| whole host death | PDEATHSIG and sentinel | Intended immediate controller kill plus worker PDEATHSIG chain in CLI. Cannot infer installed Steam embedding code version. |
| DETACH | header314 and controller control command | Stops EOF-triggered cleanup only; cannot undo kernel PDEATHSIG, so host death still kills replayer. Semantic mismatch if consumers expect full daemonization; classify unclear contract, not proven defect. |
| later unrelated exec/fork from host | no CLOEXEC on host endpoints | Can retain sentinel, inherited descriptors, change ownership detection. Reproduced with second supported instance. |

## Confirmed probes

Artifacts: /tmp/external-owner-audit-probes/probe.cpp, executable probe, results.json. Compile command from repo:

```
g++ -std=c++14 -O0 -g -pthread -I. -Ikhronos /tmp/external-owner-audit-probes/probe.cpp fossilize_external_replayer.cpp path.cpp -Wl,--wrap=pipe -o /tmp/external-owner-audit-probes/probe
```

Helpers are a controlled external executable selected via the public external_replayer_arguments API. They ignore Vulkan work; these prove API/OS lifecycle mechanics, not frequency in Steam or real GPU-driver hangs. All spawned live helper PIDs are killed and reaped individually by the harness.

### E1: wait interrupted can lose ownership and claim success while child lives (high)
Run `probe wait-interrupt`. SIGALRM uses handler without SA_RESTART and interrupts read(child_fd). Code442 only logs EINTR and advances. waitpid returns0 for live helper; 100ms expires, code473 assigns success0, then reset_pid forgets still-living PID. Output:

```
wait_status=0 child_alive=1 handle=-1
Fossilize ERROR: Failed to wait for child process to end.
Fossilize WARN: waitpid loop timed out.
```

No API precondition violated: wait contract182 promises blocking until complete. A native host signal is valid. Live child checked kill(pid,0), then individually killed/reaped. This path directly explains a class of unmanaged surviving replay subprocess after API says completion, but no claim that Steam takes this exact path.

### E2: spawning-thread death kills active replay despite live owning process (medium/high)
Run `probe thread`. Calls public start from std::thread, keeps ExternalReplayer in main, allows child startup100ms, exits thread and joins, waits child exit. Output `host_alive=1 child_signaled=1 signal=9`. Linux PDEATHSIG follows the thread that forked, not entire process. No API thread-lifetime constraint in header. Incorrect premature lifetime coupling; does not cause workers to survive.

### E3: completed-process polling destroys exit status (medium)
Run `probe status`. Helper exits37 without progress_complete; harness waits for zombie via /proc without reaping; calls is_process_complete. Output `done=1 status=-10 expected=37`. First waitpid382 gets37; poll_progress399 performs second waitpid239, ECHILD sets status=-10 and synthesized=true. Happens for early replay error/cancellation, not normal complete marker. Proven against actual production source.

### E4: failed startup leaks descriptors (medium/low)
Run `probe pipe-failure`. Linker wrap fails second pipe with EMFILE (deliberate controlled syscall failure). start returnsfalse; object destroyed. Output `start=0 fd_before=4 fd_after=6 leaked=2`. First pipe endpoints local lost by start return. Similar socket/fork partial failures static, not separately run.

### E5: concurrent instance inherits ownership sentinel of earlier instance (medium, contract qualification)
Run `probe inherited-control`. A helper reads passed control fd until EOF; B helper stays alive. B inherits host sender endpoint of A because it is neither closed in child nor CLOEXEC. Destroy A object to close its sender: helperA remains live. Kill/reap B, helperA immediately getsEOF and exits. Output `after_A_destroyed_A_alive=1 after_B_killed_A_exited=1`.

Proves cross-instance FD interference in supported multiple-object use. Destructor does not explicitly promise to terminate, so report as ownership-sentinel weakness, not standalone destructor contract violation. Same leakage affects unrelated inherited pipe endpoints; FD hygiene can interfere with other embedding app lifecycle protocols too.

## Independent checker: parent's dead-owner shared-lock finding
Read production futex lock platform/futex_wrapper_linux.hpp42-57 and external poll294, controller parse190 etc. Lock has no robust owner metadata, deadline, owner-death detection or recovery. Parent helper sets progress_started=1 and takes real futex then exits37. Independently ran `/tmp/fossilize-lifecycle-root-probes/owner-lock` after inspecting source. Output: `child_exit_observed_before_poll=1 poll_returned=0 subject_alive=1`. Verdict VALID and reproduced: API poll blocks after its own child exit observation, disabling caller progress/cancel flows. Synthetic-helper scope: forceful controller death while inside ring critical section recreates this lock state; does not show real drivers frequently hit it. Master event-loop hang while host thread dies holding lock is statically valid, but whole host death should instead trigger PDEATHSIG, so don't conflate those.
