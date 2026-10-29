# Worker lifecycle audit

29 October 2026. Research and implementation review; no production fixes.

**Conclusion:** the process-isolation architecture is reasonable, but there are demonstrable lifecycle defects. In particular, a cancellation/recovery race can request new work after cancellation, and the external API can forget a live process after an interrupted wait. Other findings affect exit ownership, dead-owner recovery, and failure-path containment. These are evidence about this implementation, not an attribution of historical Steam reports or a measurement of their frequency.

## Scope, baseline and proof standard

- Local baseline: `2325a40846cceb9e053cc5b26aad4c030e07e1a2`; initially clean checkout.
- Upstream `master` checked on 29 October: `556da9ff7d7616704dda515756219ab1dab693ec`. The lifecycle sources had previously been compared byte-for-byte to that revision; upstream remained at that revision during this audit. Runtime evidence records individual source hashes.
- Reviewed: external launcher/API, progress wrapper, Linux and Windows robust controllers, child startup/bootstrap, thread pool, work completion, crash/timeout recovery, throttling, caller/controller termination, IPC lifetime, reaping, and final teardown. The capture-layer recorder is outside this replay-worker scope.
- Linux execution: focused probes against unmodified external implementation and actual included controller functions; supplementary extracted-helper/mechanism probes. Windows: independent source/OS-semantic checks only. No Steam installation changes, games, real driver compilation, native Windows execution, or full CMake/CTest run.
- Apple/Android builds define `NO_ROBUST_REPLAYER` in `cli/CMakeLists.txt:46`; their ordinary replay cannot be treated as the same robust process supervisor. They were not built or executed. Shared ordinary-thread behavior was reviewed; platform compatibility is not certified.
- **Reproduced** means the stated API or mechanism was exercised. **Source-proven conditional defect** means an exact failure branch and governing OS semantics establish the consequence, but the prerequisite was not observed naturally. **Coverage gap** means a desired reliability event has no implementation; it is not evidence that the triggering failure actually occurred. **Modeled race** is explicitly weaker than a full-controller reproduction.

The user requested a research agent, path inspectors and independent checkers. One agent researched OS rules and inspected Windows; a second inspected the Linux controller; a third inspected the external owner/API. The research agent independently checked the controller; the external inspector checked dead-owner locking, cancellation and Windows findings. The primary agent independently rebuilt/reran external probes and called actual production cancellation/recovery functions. Retained reports record qualifications and rejected claims.

## Rules used

The criteria are ownership, transactional startup, cancellation that prevents further starts, phase-specific deadlines, pause/resume accounting, IPC that cannot indefinitely block supervision, truthful terminal state, recovery with forward progress, and explicit resource ownership. These are design criteria grounded in OS semantics, not an assumption that every application must use a particular framework.

Primary references include [Linux parent-death signal semantics](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html), [waitpid](https://man7.org/linux/man-pages/man2/waitpid.2.html), [pipes](https://man7.org/linux/man-pages/man7/pipe.7.html), [epoll event-cache pitfalls](https://man7.org/linux/man-pages/man7/epoll.7.html), [fork](https://man7.org/linux/man-pages/man2/fork.2.html), [signal safety](https://man7.org/linux/man-pages/man7/signal-safety.7.html), [Windows job objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects), and [abandoned mutex ownership](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject). Detailed criteria: [research notes](worker-lifecycle-audit-evidence/rules.md).

## Highest priority confirmed findings

### Cancellation can request replacement of an already-failed worker

**Reproduced production-function decision; replacement call chain proven by inspection.**

Sequence:

1. A worker crashes, reports recoverable progress, and exits with code 2; its status has not yet been collected.
2. Cancellation marks the slot `expect_kill=true` and sends SIGKILL. A signal cannot change the exit status of an already-exited child.
3. Reaping returns the original exit code 2.
4. `process_shutdown()` suppresses retry only when the status specifically says SIGKILL and `expect_kill` is set. With code 2 and remaining work, it requests retry.
5. `poll_children_process_states()` immediately starts the replacement. No persistent controller stopping state gates that start.

Source: `cli/fossilize_replay_linux.hpp:334`, `:896`, `:937`. The path is possible for SIGINT/SIGTERM and control-connection EOF; the latter connection may already be closed when replacement starts. A later explicit whole-tree kill or controller death remains a fallback.

The primary probe includes the actual production `.cpp`, calls actual `shutdown_processes()` and `process_shutdown()`, and uses `waitid(..., WNOWAIT)` on a real exited child. Recovery marker values are controlled test state. Observed: `production_shutdown_called=1 child_already_exited=2 expect_kill=1 replacement_requested=1`. Two independent agents checked the branch/caller chain. Negative control cancelling a live worker yields SIGKILL and no restart.

This does **not** demonstrate the whole event-loop race occurring during a Steam run. It proves the wrong recovery decision for a reachable cancellation/exit ordering.

### Interrupted wait can claim success and forget a running process

**Reproduced against the unmodified ExternalReplayer implementation, independently rebuilt/rerun.**

`ExternalReplayer::Impl::wait()` blocks on a child-tree sentinel. A handled signal without `SA_RESTART` interrupts the read. The code logs the read failure and proceeds instead of retrying. After approximately 100 ms of nonblocking waitpid polling, it synthesizes success and resets its tracked PID even though the child remains alive.

Source: `fossilize_external_replayer_linux.hpp:408–489`.

Observed: `wait_status=0 child_alive=1 handle=-1`. This violates the public blocking-wait contract and loses the API's means to manage that child. The probe individually kills/reaps the child afterwards. Ordinary host-process death still has parent-death cleanup; this is a live-owner ownership failure, not proof of permanent survival after host death.

### Dead owner of the shared diagnostic lock can block lifecycle handling

**Reproduced against the unmodified API and actual shared-lock implementation; independent agent inspection/rerun.**

The shared FIFO uses a nonrobust futex lock with no owner-death recovery or timeout. If the controller dies while holding it, the lock remains acquired. `poll_progress()` can observe/reap the dead child and then block on that same lock. Calling `wait()` also pumps progress first, so the dead lock can obstruct completion handling.

Source: `platform/futex_wrapper_linux.hpp:42–58`, `fossilize_external_replayer_linux.hpp:226–303`, `:408–415`.

A controlled external helper maps the actual protocol block, sets `progress_started`, acquires the production lock, then exits 37. The calling process blocks in progress polling; `/proc/.../wchan` identifies a futex wait. A scoped supervisor ends only its own test process. This simulates forceful controller death inside a real critical section; it does not show that real driver crashes commonly strike that section.

## Other demonstrated ownership and accounting defects

| Finding | Evidence and limit |
|---|---|
| A temporary launching thread kills replay while the host stays alive | `PR_SET_PDEATHSIG` follows the thread that forked, not the whole process. Public `start()` has no launching-thread lifetime restriction. Actual API probe starts from a temporary `std::thread`; joining it leaves the host alive but child killed by SIGKILL. Source: external Linux `:953–980`. This is premature death rather than leaked workers. |
| Completion polling overwrites the child's exit status | `is_process_complete()` first reaps exit 37, then calls `poll_progress()`, which attempts another wait and replaces the result with `-ECHILD`. Actual API probe returns -10. Source: external Linux `:372–405` and `:239–252`. Applies when the child died without publishing `progress_complete`. |
| `wait()` can reap an unrelated host child | Its initial `poll_progress()` may reap replay and reset PID to -1. The subsequent `waitpid(pid,...)` then becomes `waitpid(-1,...)`. With both children already exited, probe returns unrelated exit 42 instead of replay exit 37; the host then gets ECHILD for its unrelated child. Both exits synchronized using WNOWAIT. Source: external Linux `:415`, `:451`. |
| Partial startup leaks descriptors | Injecting failure of the second pipe leaves the first pipe's two local FDs open even after object destruction. Actual API/linker syscall injection; observed `leaked=2`. Socket/fork failure branches have similar source omissions. Source: external Linux `:940–950`, `:1005–1008`. No running-worker leak is inferred from this alone. |
| Another launched instance can retain an ownership sentinel | A second instance inherits the first instance's host-side control endpoint across exec. Closing the first instance's endpoint does not produce EOF until the second process releases its inherited copy. Actual two-instance helper probe confirms this. Source: external Linux startup `:940–1002`. Destructor termination is not explicitly promised, so this is a demonstrated sentinel/inheritance weakness, not a fabricated destructor contract. |

## Controller failure branches and conditional gaps

| Path | Finding | Evidence strength |
|---|---|---|
| Blacklist bootstrap transfer | Controller synchronously sends accumulated failing-module hashes before closing its own redundant pipe reader and before entering supervision. A dead/stalled replacement plus enough hashes fills the pipe; its own reader prevents EPIPE. Blocked SIGTERM/control EOF cannot be serviced while the controller is writing. | Unchanged production-helper reproduction plus independent corrective negative control. Requires a sufficiently large accumulated blacklist and a replacement that stops consuming input; no frequency established. External SIGKILL can still end the controller. Linux `:529–551`. |
| Stale event after slot replacement | epoll entries identify slots, not generations. Reap/restart can replace a slot while other old events remain in the returned batch; an old event can read a new blocking pipe. | Source-feasible race plus modeled blocking-read probe; **not** a naturally reproduced full-controller race. Independent checker validated against epoll's event-cache warning. Linux `:937–969`, `:1268–1367`. |
| Child setup error | Fork-child branches return `EXIT_FAILURE` from a function returning bool. On this platform that is true, so a failed child can continue controller code instead of terminating. | Exact source-proven conditional branch; no injected pthread_sigmask/dup2 failure in this audit. Linux `:576–578`, `:605–609`. |
| Pre-device preparation | Watchdog begins only upon BEGIN_HEARTBEAT, sent before Vulkan device initialization. Earlier bootstrap/archive preparation has no comparable worker deadline. | Source coverage gap. Do not misreport Linux device initialization as unprotected: its heartbeat starts before device creation. |
| Terminal outcome | Unrecoverable worker loss, and ordinary cancellation, can lead to master progress_complete and success after the active count reaches zero. | Source behavior, already related to observability backlog FOS-000012/FOS-000013. Separately classify completion and successful coverage. No orphan inference. Linux `:371–390`, `:1382–1429`. |

## Windows-specific source findings

Independent inspector and second checker reviewed these paths; **none was executed on Windows**.

- **Missing general worker watchdog:** workers emit BEGIN_HEARTBEAT/HEARTBEAT, but the controller does not recognize them or maintain a heartbeat deadline. The only controller timer begins after CRASH. Thus compilation's batch timeout does not cover hung initialization, other preparation, or final driver destruction. This is a source-proven coverage gap, not proof of an actual driver hang. Source: Windows controller `:117–245`, `:908–925`; worker `:1233–1242`.
- **Abandoned IPC mutex mishandled:** WAIT_ABANDONED grants ownership, but code only releases on WAIT_OBJECT_0. A survivor can retain the mutex and block other participants after a thread dies holding it. Requires owner-thread death in the critical section; native execution not performed. Source: external Windows `:155–165`, controller Windows `:149–239`.
- **Containment setup fails open:** job creation/configuration/assignment failures log and continue. The process can launch without guaranteed kill-on-close containment. A working outer job can still protect workers if only an inner job fails; no blanket orphan claim. Source: external Windows `:671–718`; controller Windows `:633–637`, `:765–781`.
- **Unchecked resume and I/O startup results:** ResumeThread failures can leave suspended processes reported as started; some pipe errors have no terminal supervisor transition. These are concrete conditional branches, with triggering API failures not reproduced. Source: external Windows `:702–729`; controller Windows `:625–673`, `:961–964`.
- Partial startup handle rollback is also incomplete. Most controller-local leaks end at process exit; this alone does not establish persistent worker escape.

## Lifecycle coverage and rejected claims

| Lifecycle event | Reviewed behavior / conclusion |
|---|---|
| Creation and partial startup | FDs, handles, shared maps, fork/CreateProcess, child initialization and bootstrap checked; rollback and failure-control findings above. |
| Normal work reuse | Threads repeatedly consume tasks; robust process ranges assigned once. A normally completed process exits and is reaped; no routine replacement to fill idle CPUs. |
| Normal completion and finalization | Threads join, caches/device tear down, child status is collected, controller finalizes outputs. API status/reap defects and completion/coverage distinctions above. |
| Crash before recovery information | Slot abandoned; no fabricated recovery index. Overall terminal report may hide incompleteness. |
| Crash with recovery information | Resume remaining ranges, known failing modules passed to replacement; cancel/restart and bootstrap/event-race findings apply. |
| Compilation timeout | Separate no-completion timeout during batch synchronization; not a whole-run or universal per-pipeline deadline. |
| Linux driver init/final teardown hang | After device-init heartbeat registration, the supervisor's heartbeat watchdog is a backstop. Unconditional thread join alone is not proof of a robust-worker leak. |
| Intentional stop/resume | SIGSTOP retains allocations; SIGCONT resumes same PID and rearms heartbeat. RUNNING_TARGET=0 may deliberately pause all workers. Policy, not an orphan defect. |
| Cancellation of healthy running/stopped workers | Explicit SIGKILL plus reap normally works. Negative control confirms no replacement for a genuinely SIGKILL-terminated child. |
| Cancellation overlapping recoverable exit | Confirmed wrong recovery decision above. |
| Controller death | Current Linux children register parent-death SIGKILL and check for the startup race. Generic historical claims that this backstop is absent are rejected. Windows jobs normally provide containment when setup succeeds. |
| Caller destruction / thread exit / process death | These are distinct events. Public header assigns reaping responsibility to caller unless wait is used; no destructor-alone reap guarantee. Thread-scoped Linux protection and inherited sentinel weakness above. |
| Reaping / coalesced SIGCHLD | Linux drains waitpid and polls periodically; no generic permanent-zombie conclusion from signal coalescing. Zombies are dead and do not compile. |
| Repeated start or disallowed group kill | start explicitly once-only; kill prohibited with inherited process group. Violating those preconditions is not a supported-path product defect. |
| Standalone replay and unsupported robust platforms | Shared thread teardown checked; there is no separate robust supervisor in ordinary mode. Missing isolation there is not an accidental orphan claim. Apple/Android native behavior remains unverified. |
| DETACH | Disables control-EOF cancellation, but does not undo Linux parent-death protection. Full daemonization is not established by the public wording; classified ambiguous rather than confirmed defect. |

Postfork allocation/environment work in a multithreaded host also deserves hardening review under async-signal-safety rules; no concrete held-lock deadlock was reproduced, so it is retained as a risk rather than a confirmed hang. Memory bounds, fixed partitions and work stealing are separate resource/scheduling questions, not proof of lifecycle failure.

## Verification and retained evidence

Run from repository root with a new output directory:

```sh
python3 local-docs/reviews/worker-lifecycle-audit-evidence/reproduce.py \
  --out /tmp/fresh-fossilize-lifecycle-evidence
```

The runner builds fresh probe executables against the current source, records compiler/kernel/source hashes and exact build/run commands, checks expected observations, and retains all stdout/stderr. Three probes deliberately return 1 when the product expectation is violated; the runner distinguishes that from unexpected execution failure. A matched defect is **not** a passing product regression suite.

Final retained run: **13 of 13 expected observations matched**, including two negative controls. These include deliberately reproduced defects, so this is research evidence rather than a passing product suite. Compiler: GCC 13.3.0, C++14, `-O0 -g -pthread`; Linux 7.0.0-38-generic.

The evidence directory contains the source/API probes, extracted-helper controls, independent inspection/check reports, and a snapshot of the final run. No binaries or generated build outputs are committed. Timing observations of deliberately blocked helpers are mechanism evidence, not game-performance benchmarks. No CMake/CTest, sanitizers, GPU/Steam runtime, Windows or cross-platform conformance claim is made.

## Recommended order of follow-up

First prevent all starts/restarts after cancellation and correct wait ownership/terminal status handling. Next ensure owner death cannot strand shared IPC locks or block the lifecycle event loop, and align process ownership with thread/process lifetime. Then make startup transactional, restrict inherited descriptors, reject stale events, and add phase deadlines and Windows failure handling. Any implementation should add deterministic regression cases for the accepted scenarios and preserve the healthy cancellation/reap negative controls. This audit creates no new backlog allocations and does not change existing FOS keys.
