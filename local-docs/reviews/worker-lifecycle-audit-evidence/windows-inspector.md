# Windows worker lifecycle audit and independent checks

Date 2026-10-29; local revision 2325a40846cceb9e053cc5b26aad4c030e07e1a2. Files: cli/fossilize_replay_windows.hpp; fossilize_external_replayer_windows.hpp; shared ThreadedReplayer logic and public ExternalReplayer contract. No native Windows execution; all verdicts below are static source/OS-semantic evidence. Research criteria: /tmp/fossilize-lifecycle-rules.md.

## Path coverage

- External launcher: create/map shared memory; create named mutex; build arguments/environment/stdio; create/configure kill-on-close job; CreateProcess suspended; assign job; resume; progress poll; completion check; blocking wait; explicit kill; object destruction.
- Controller: create/configure inner job; import shared memory; parse/export DB metadata; split ranges; spawn children; build process/pipe/timer wait set; parse messages; normal exit/recovery; crash-cleanup timer; replacement spawn; final identifier merge; completion flag.
- Worker: import shared resources; initialize replayer; read ignored-module bootstrap input; duplicate stdout IPC handle; install SEH/abort handling; parse/create/compile; timeout/crash reporting and emergency teardown; normal thread joining/cache/device teardown; exit.

## Independently checked candidates

### W1 — Parent-death containment setup fails open: VALID conditional defect

Evidence: external launcher lines 671–686 logs CreateJobObject or SetInformationJobObject failure and continues; lines 714–718 logs AssignProcessToJobObject failure and continues. Controller lines 765–781 and worker spawn lines 633–637 do the same. Both process spawns return success afterwards. Destructor only closes process and job handles (external lines 83–94). Closing process handles does not terminate their processes.

Concrete branch: external job creation/configuration/assignment fails; process creation succeeds; caller dies; no configured containing job is guaranteed; controller can continue working after its caller exits. If inner worker job setup similarly fails, controller death can leave workers alive. **Caveat:** failure of the inner job alone does not prove escape when the inherited outer job remains functional. Standard nested-job inheritance can still contain them. Direct --master-process has no Fossilize outer-job fallback.

Primary sources: https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects ; https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject ; https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle . Underlying API failure not reproduced, so do not claim normal-path orphaning.

### W2 — ResumeThread failures ignored: VALID conditional startup defect

External lines 702–729 and controller lines 625–673: create suspended, attempt ResumeThread (return ignored), close thread handle and report success. ResumeThread -1 signals failure; >1 means still suspended. There is no subsequent startup deadline for a child that remains suspended. This can leave an apparently started worker alive without progress until explicit kill or owner death. Native failure not demonstrated. In the controller, a nonempty ignored-module bootstrap list can additionally block startup while sending to a child that never runs; this latter consequence depends on pipe behavior and list size and is not claimed proven here.

Source: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-resumethread . Do not imply ResumeThread normally fails with the valid fresh handle.

### W3 — Windows heartbeat emitted but never supervised: VALID source-proven missing deadline

Worker lines 1233–1242 emit BEGIN_HEARTBEAT and HEARTBEAT. Controller parse lines 117–245 recognizes neither, falling through to unexpected-message logging. ProcessProgress has no watchdog timer field. Controller lines 924–925 waits infinitely; its only timer is armed after CRASH at lines 119–138. Shared compilation timeout is enforced only while sync_worker_memory_context waits for batch completion (cli/fossilize_replay.cpp 928–973). Consequently hung Vulkan initialization, parsing outside that wait, or final driver destruction is not bounded by the Windows controller even with --timeout-seconds. This establishes absence of watchdog coverage; it does not establish a real driver hang occurred. Bounded cleanup after a received CRASH remains implemented.

### W4 — Abandoned IPC mutex ownership not released: VALID source-proven recovery defect

External poll_progress lines 155–165 and controller parse lines 149–154,169–174,190–195,211–216,234–239 enter critical sections only on WAIT_OBJECT_0. WAIT_ABANDONED is not an ordinary failed acquisition: caller owns mutex. It skips ReleaseMutex. Concrete schedule: external polling thread dies while owning mutex; next controller parse thread receives WAIT_ABANDONED, keeps mutex and continues; later external polling thread blocks in infinite mutex wait. Reverse caller/controller roles likewise possible. Repeated acquisitions by the same surviving owner recursively increase/decrease ownership but leave original acquisition held, so subsequent polls do not repair it. Could also block supervisor handling of its worker cleanup timer once supervisor enters a shared-mutex wait.

Source: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject ; https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-releasemutex . Trigger is death of a thread while holding mutex; not proven naturally occurring. Recommended response must also validate shared ring consistency rather than blindly treating abandonment as normal.

## Additional findings/limits

### W5 — Worker pipe I/O failure is not a terminal transition: VALID conditional supervision gap

Controller lines 961–964 calls process_once and discards false. process_once lines 249–273 resets event then returns false on ResetEvent/GetOverlappedResult/kick failure. No process cancellation/restart/deadline follows. If process remains alive and cannot communicate, only an eventual process exit or previously armed crash timer can resolve it; W3 means no normal watchdog. A failed event reset may instead keep signaled state and cause spinning. API failure prerequisites not reproduced.

### W6 — Startup handle rollback incomplete: VALID leak paths, mostly process-lifetime bounded

Child spawn lines 569–629 has multiple returns after pipe creation without closing prior handles. External start opens NUL then returns on environment/create-process failure without closing this local handle (lines 635–709). Members such as shared mapping/mutex/job are covered by eventual ExternalReplayer destructor; local NUL is not. Controller process exit eventually closes its leaked handles; avoid overstating these as persistent running-worker leaks. If controller has already created children and later spawn/event/I/O startup fails, returning out of main eventually closes its successful job and kills children; W1 can undermine that fallback.

### W7 — Progress-only polling does not independently notice dead controller: CONTRACT/OBSERVABILITY GAP, not blanket worker leak

External poll_progress reads shared state but never tests process signaled state; a controller that exits before progress_started can leave it returning ResultNotReady repeatedly. Public API exposes is_process_complete and wait; CLI correctly calls is_process_complete every iteration (cli/fossilize_replay.cpp 3857). Therefore not a CLI endless loop. A caller that uses only progress polling must also inspect exit; public PollResult semantics/documentation should be checked before calling this misuse or guaranteed bug.

## Protections verified by inspection / rejected blanket claims

- Normal successful worker exits are collected and not replaced. Crash replacement resumes only when recovery indices exist and remaining work exists (controller process_shutdown 278–346).
- Suspended creation followed by job association before resume is a sound normal-path containment pattern.
- Job handles are created with null security attributes, so they are non-inheritable; children do not automatically retain the kill-on-close handle. Blanket claim that inherited job handles always defeat cleanup is unsupported here.
- External kill() calls TerminateProcess(controller), not TerminateJobObject; under normal successful inner-job setup, controller death closes its job and kills its workers. Calling it a normal-path children-survive bug would be wrong. When job setup fails, W1 applies.
- INFINITE wait() is documented blocking API, so absence of a timeout on that API alone is not a defect. Missing supervisor coverage across driver phases is W3.
- start() is publicly documented once-only. Repeated start resource leakage is outside contract and should not be reported as supported-path defect.
- Windows supports no Linux RUNNING_TARGET/DETACH protocol here: send_message returns false; no SIGSTOP/SIGCONT lifecycle equivalence should be assumed.

## Evidence status

Source/semantics prove the listed branch handling and missing events. No Windows binary was compiled or executed, no real API failure rate established, no claim about Steam deployment version, and no historical user report attributed to these findings. Highest confidence actionable Windows issues: W3 coverage gap and W4 abandoned mutex handling. W1/W2/W5 are concrete failure paths whose triggering API failures remain conditional.
