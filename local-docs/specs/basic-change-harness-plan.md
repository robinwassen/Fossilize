# Basic change harness: proposed skill design

Date: 2026-10-27. Status: implementation in progress under FOS-000025.

## Outcome and boundaries

Create a discoverable personal skill named `implement-change-harness`. It runs one
identified root task at a time in this repository, with sequential, independently
verifiable subtasks when necessary. It produces one verified implementation branch
and one pull request to `master` for the whole task. It does not merge the PR,
schedule itself, discover another task, or implement unrelated improvements.

The parent agent coordinates. Planning, implementation and adversarial checking
each run in a fresh subagent. The coordinator manages durable state and publishing;
it does not replace a failed checker with its own judgment.

## FOS-000025 — Implement the change harness skill

Canonical task definition. Status: in progress. Implement the workflow specified
below as a tracked, discoverable skill with a standard-library state/Git helper,
meaningful automated tests and independently checked evidence. Preserve the
planner/implementer/checker responsibilities and per-subtask verification gates.
Acceptance requires tested state transitions, recovery and publication safeguards,
a validated skill package, and one PR to robinwassen/Fossilize master. Use this
three-role method to implement the harness itself; record the bootstrap run on
harness-artifacts before dispatching agents. Product C++ changes are out of scope.

AGENTS.md and local-docs are shared in Git.

## Lessons applied from the requested lecture

The lecture argues for external memory, separation of implementation and judgment,
explicit stopping conditions, bounded context, and starting with a small system.
Those become committed state, independent checker attempts, evidence gates, fresh
agent contexts, and a sequential workflow. These are design choices, not a claim
that multiple agents guarantee correctness. Different models can share blind spots.

Do not adopt scheduling or an endless research loop: this change workflow has a
finite outcome. Do not use PR counts or passing test counts as quality measures.

Source: https://walkinglabs.github.io/learn-harness-engineering/en/lectures/lecture-13-loop-engineering/

## Repository layout and branches

- Implementation: `codex/<root-key>-<short-description>` from the recorded target
  `master` commit, in a dedicated worktree. Preserve the user's checkout and edits.
- Artifacts: `harness-artifacts`, created as an orphan branch with an empty root
  tree. Subsequent commits contain only harness records, never a source-tree copy.
  Use a separate artifacts worktree. Never merge this branch into `master` or the
  implementation branch. Verify an existing branch before adopting it.
- Planner: a detached worktree at the planning input commit.
- Checker: a fresh detached worktree at the exact candidate commit for each check.
  Keep test outputs out of tracked source files and use separate build directories.
- All worktrees are in the same Git repository; this is procedural isolation and
  context separation, not a security sandbox. Agents still share filesystem access.

Artifacts branch contents:

```text
active.json                              # one active run pointer, or null
allocations.json                         # permanent issued/reserved key journal
runs/<root-key>/<run-id>/state.json       # canonical current run state
runs/<root-key>/<run-id>/reports/...      # planner/implementer/checker reports
runs/<root-key>/<run-id>/evidence/...     # small logs and evidence metadata
```

JSON is proposed instead of YAML to use Python's standard library and eliminate a
parser dependency. Both plans remain structured fields inside state.json. Git
history preserves previous plans and transitions; reports retain detailed findings.
Avoid storing full transcripts, build trees, binaries or secret-bearing raw logs.

Default PR destination for this checkout: `robinwassen/Fossilize`, base `master`,
using `origin`. `upstream` points to ValveSoftware/Fossilize; do not select it merely
because it is upstream. Record repository identity explicitly at initialization.
Publishing artifacts is a separate policy: default local-only, with its lack of
cross-machine recovery made explicit. Pushing implementation is required for a PR.

## Task keys and canonical definitions

The root uses its existing permanent backlog key. New root tasks must first be
registered under the project rules. Every split-out subtask also receives a new
project-wide key; array positions and run IDs are not task keys.

For Fossilize, allocate `FOS-` followed by at least six digits, above every previously
issued number. Inspect the registry and all current/archived backlog definitions.
Register allocations in local-docs/backlog-keys.md before creating definitions.
Never recycle keys. Reserve interrupted allocations permanently rather than fill gaps.

The existing root backlog remains canonical; state stores its key, location and
input snapshot. Harness-created subtasks have their canonical definition in the
artifacts state, with a branch-qualified location in the registry. Other mentions
are references, not additional definitions. Retire or preserve keys on replanning;
do not reinterpret an old subtask key as a different task.

The tracked registry is the project allocation authority. Agents propose
decomposition; the coordinator allocates keys under the repository lock and
includes registry updates with the implementation branch. Reconcile allocations
from unmerged harness runs before allocating; a branch snapshot alone may be stale.
Worktrees read the tracked guidance from their input revision, with explicit paths
to newer required guidance when necessary. Independent clones/machines are outside
v1's serialization guarantee; concurrent remote allocations require coordination.

Preserve a minimal permanent allocation journal/high-water mark on the artifacts
branch as well as the tracked project registry. Before allocating, reconcile the union
of both records and all backlog definitions; never trust only one counter. Write
the reservation durably first, then update the tracked registry, then publish the
subtask definition. Interrupted reservations remain issued forever. The journal
contains key/location/allocation metadata, not copies of the project guidance.
Conflicting meanings for one key require reconciliation rather than renumbering.
The ledger at the time of this proposal ends at FOS-000024; re-read it at allocation time rather
than encoding the next number in the skill.

## Agent contracts and bounded context

Use `fork_turns="none"` on every phase/attempt. Supply repository/worktree paths,
root key, current subtask, immutable input commit, plan version, acceptance criteria,
relevant guidance paths and the expected report format. Agents read only the
additional files needed for their task. All three agents must read applicable
AGENTS.md instructions and the required project index, descriptor and pillars.
Read relevant specifications without claiming desired properties are proven behavior.
Before planning or running verification, read the testing guide referenced by
AGENTS.md: for this project, local-docs/verification.md. Make this guide available
to all three roles from the tracked project documentation. Follow its links
selectively; do not copy its entire test inventory into every worker prompt.

Do not pass the full parent transcript. Reports are concise summaries with evidence
paths. A retry gets unresolved findings and relevant decisions rather than its
predecessor's entire conversation. No nested delegation in v1.

### Planner

Read the root request, backlog definition, relevant code and project guidance.
Return the problem, expected behavior, explicit non-goals, affected boundaries,
implementation plan, verification plan, risks and assumptions.

The planner owns test planning, including which tests the suite must be extended
with for each subtask. Specify the existing tests to reuse or modify, new test
cases and fixtures, intended test files/targets, relevant runner registration,
and the behavior and assertions each test must exercise. For regression fixes,
plan evidence that the test detects the original defect where feasible. If no
suite extension is needed, explain how existing coverage establishes the required
behavior. Record these decisions in the versioned verification plan before
implementation; the checker does not take over test planning.

Each subtask must have a coherent outcome, dependencies, intended changes and
acceptance evidence that can be obtained before advancing. A test-file edit and its
implementation normally belong together. Prefer no decomposition for a small fix.
An original reproducer or baseline failure is included where feasible.

Verification covers positive behavior, relevant failure/unsupported cases,
regressions, scope boundaries and environment requirements. Commands include their
working directories and expected results. Where runtime checks are not practical,
specify the review evidence and the remaining uncertainty. Do not invent hardware
access or downgrade a required hardware check to optional after it fails.

The planner selects and justifies the important checks for each subtask using the
project testing guide and the actual changed behavior. Record a verification gate
for each acceptance criterion: the risk/behavior addressed, check method, exact
command/cwd or review procedure, fixture, prerequisites/environment, expected
assertions, and evidence to retain. Classify checks as required for this subtask,
required at final integration, or optional diagnostic checks. Explain exclusions
when an apparently relevant testing category is intentionally omitted. Existing
project-mandated checks remain mandatory; planner judgment cannot remove them.

Prefer the smallest checks that establish the required behavior, with relevant
negative/regression cases. For example, a CPU-only database fix can require an
assertion-bearing persistence-failure regression and relevant system tests without
requiring a real Steam game. A claim about real driver compilation needs suitable
driver evidence; null-device replay does not establish it. An empty CTest run or
successful build cannot satisfy a behavioral test gate. Interactive, expensive or
environment-mutating checks follow the guide's scope and authorization boundaries.

Resolve unavailable prerequisites during planning where possible. If a mandatory
gate cannot run, record the limitation and route it for an environment or scope
decision. Optional checks may remain unavailable with an explanation. Subtask
completion requires all its mandatory gates to pass; defer only explicitly planned
whole-task checks to final integration. The checker may require additional checks
for concrete uncovered risks, routing acceptance/plan changes through the planner.

The coordinator validates the plan's completeness and unique keys mechanically.
Planner and implementation output cannot silently weaken the original task.

### Implementer

Take at most one ready subtask. Implement its source changes and planned test-suite
additions or modifications, register tests with the relevant runner where needed,
and run the required checks. Iterate locally, then commit a candidate including
the tests and return the SHA, changes, results and limitations. Map each planned
test to its implemented location and actual execution evidence; explain omissions
as incomplete work rather than silently skipping them.
Only this role edits implementation source and committed tests; it cannot certify acceptance or edit
canonical state, acceptance requirements or verifier reports.

Do not proceed to the next subtask until the checker accepts this one. A rejected
candidate returns to implementation with actionable findings. An infeasible plan
returns to the planner through the coordinator; unrelated discoveries are recorded
as out-of-scope observations rather than automatically implemented.

### Checker

Start fresh at the candidate SHA. First derive expected behavior from the original
task and project constraints; then consult the plans and implementer's factual
report. Independently inspect the full relevant diff, rerun decisive checks and
probe credible counterexamples. Check for unrelated changes and weakened tests.

Validate that the planned test-suite extensions were correctly implemented and
actually run. Inspect test assertions, fixtures, failure injection and runner
registration, and confirm the tests exercise the intended behavior rather than
passing vacuously or testing only a mock. Independently run the required gates at
the candidate SHA, verify that the intended tests were discovered/executed, and
retain actual results. Check evidence that regression tests detect the original
defect where required by the plan. An implementer's success report, a build, an
empty test run or skipped tests cannot substitute for that evidence.

Incorrect or missing implementation of an already planned test returns to the
implementer. Missing coverage in the plan returns to the planner for a versioned
update, then to the implementer and a fresh checker. The checker reports the gap
and evidence; the planner owns the resulting test design and gate changes.

Return `accepted`, `changes_required`, or `unable_to_verify`, with criterion-level
evidence, findings and residual uncertainty. Findings include file/location,
concrete failure scenario and relevance to acceptance. Acceptance is not a vote or
a lack of findings. It requires evidence for every required criterion.

The checker may construct disposable probes in its own worktree; any proposed
source or test fix goes back to the implementer and is checked again. It does not
modify the candidate branch. A check verifies a specific candidate/base/plan tuple.
It also assesses whether the planner selected sufficient gates for the original
requirements. Passing a weak plan is not acceptance. Missing necessary coverage
returns to planning; the implementer cannot waive a gate or redefine its result.

After all subtasks are accepted, run a fresh whole-task check on the combined
branch. Earlier subtask acceptance alone does not establish integration correctness.

## Model and effort policy

Initial, configurable proposal; validate against the live runtime's model catalog:

| Role | Model | Effort | Reason |
|---|---|---|---|
| Coordinator | Current parent model | Inherited | Small routing/state context |
| Planner | gpt-6.1-sol | high | Bound scope and choose meaningful evidence |
| Implementer | gpt-6.1-sol | medium | Focused, sequential changes |
| Checker | gpt-6.1-sol | high | Counterexamples and independent verification |

Raise implementation effort for C++ lifetime, concurrency, Vulkan compatibility,
serialization or unclear failure behavior. Permit a stronger available model for
an unresolved reasoning problem, within the recorded budget. Do not automatically
select the most expensive effort, or a cheaper checker because its report is short.
Model diversity is optional; independent evidence matters more than model names.

Store requested and actual model/effort on every attempt. Validate support before
dispatch; do not silently substitute unavailable models. Use an explicitly recorded
supported fallback or record a capability blocker. Do not change global Codex config.

Official reference: https://learn.chatgpt.com/docs/agent-configuration/subagents

## Durable state contract

Use a versioned schema with these required field groups:

| Group | Contents |
|---|---|
| Identity | schema version, state revision, run ID, root key, canonical task location, input snapshot |
| Repository | repo identity/common Git directory, target remote/repository/master SHA, implementation branch/worktree and candidate SHA |
| Progress | phase, operation, status, active subtask key, next action, stop/blocker reason |
| Plan | version, original acceptance requirements, scope/non-goals, implementation plan, verification plan including per-subtask test-suite extensions and required gates |
| Subtasks | unique keys, canonical definitions, dependencies, criteria, status, accepted SHA and evidence references |
| Attempts | role, attempt ID, worker ID, input SHA/plan/revision, model/effort, timestamps, result/report references |
| Verification | exact candidate and base SHA, plan version, planned-test-to-implementation mapping, discovered/executed tests, commands/cwd, environment, exit/result, log reference, criterion coverage |
| Findings | unique finding IDs, affected task key, evidence, resolution and recheck result |
| Limits | implementation/repair/replan counts, command timeouts, run time budget, usage when available |
| Publishing | destination, head/base, persisted intent, remote head, PR identity/URL, attach outcome |

Keep phase separate from status. Three agent phases are `planning`, `implementing`
and `checking`. Administrative phases are `initializing` and `publishing`, followed
by `complete`. Status expresses `ready`, `running`, `interrupted`,
`awaiting_decision`, `failed` or `complete`; the checking operation distinguishes
subtask from final verification. A blocked run records its exact resumption action.

Evidence distinguishes passed, failed, unsupported, incomplete and not-run results.
Unsupported or incomplete mandatory verification cannot produce acceptance.

## State transitions and recovery

1. Acquire a repository-wide OS lock in the common Git directory; inspect active
   run, branches, worktrees, tools, model support and local guidance. Resume an
   existing run instead of starting another root task.
2. Initialize durable identity and record the target base. Persist each dispatch
   intent before starting its worker; persist worker identity immediately after
   creation. Validate the report before recording a result or advancing.
3. Planner -> implementer -> checker. On rejection, return the same subtask to
   implementation. On acceptance, select the next ready subtask. After the last
   subtask, perform final checking, then publishing.
4. The coordinator is the only writer of canonical state. A helper validates legal
   transitions and expected state revision, atomically writes state, and commits
   state plus reports to harness-artifacts. The Git ref is the durable checkpoint;
   uncommitted artifact files do not advance a phase.
5. On resume, reconcile durable intent with actual agents/processes, source HEAD,
   dirty/untracked files, artifact commits, evidence and remote PR state. Preserve
   partial work. Do not blindly restart a command, reset a worktree, or adopt an
   unrecorded commit as accepted.
6. A stale lock is not proof a worker stopped. Confirm prior writers terminated
   before dispatching replacements. Uncertainty becomes awaiting_decision. Tag
   reports with attempt IDs and input revisions; reject late/stale results. The
   dispatch-before-worker-ID crash window requires explicit reconciliation and
   must be covered by a recovery test.
   Record an unacknowledged dispatch as an ambiguous outcome, not an absent worker.
   Use the runtime's actual worker cancellation/status facilities. If they cannot
   establish termination, stop for intervention before starting a replacement.
7. Changed source invalidates prior final acceptance. A changed plan invalidates
   affected acceptance. If target master advances, refresh the integration baseline
   under a recorded policy and rerun affected checks before publishing; conflicts
   return to implementation. Do not claim a proof against future target commits.

Proposed bounds: initial candidate plus at most three repair rounds per subtask,
one replan per run, with separate command and run timeouts recorded before work.
Repeated identical failures without new evidence stop early. Budget exhaustion
records unfinished work and the next action; it never masquerades as success.
Infrastructure errors do not automatically become code failures. Resume requires
an explicit budget extension when the configured budget is exhausted.

## Publishing and completion

Require all planned subtasks accepted, no unresolved acceptance findings, a final
checker acceptance for the current candidate/base/plan, and all required checks
passed. Required CI belongs in that gate if it can run before PR creation; if the
host requires a PR to run CI, distinguish PR opened from CI-verified completion
and record that policy before execution.

Persist PR creation intent, push the implementation branch, verify its remote SHA,
then look up an existing PR by exact target repository, base and head before
creating one. An ambiguous network failure triggers lookup, not immediate retry.
Include closed and merged PRs in that lookup; never silently reopen or replace a
closed PR. Reconcile the discovered outcome with the run's identity and state.
Record the URL and attach it to the current Codex chat with attach_artifact.
An attachment failure retries attachment without creating another PR.

One PR covers the root and all subtasks. Never create a subtask PR. Default to a
ready-for-review PR after verification; use a draft only if the task requests it
or a recorded repository policy requires it. No automatic merge or force push.
Complete means verified and PR opened, not merged. A publication failure retains
the verified candidate and resumes publishing without reimplementing it.

Plain-language PR description:

- Problem: describe the affected user/workflow and the observable problem.
- Difference: explain behavior before and after the change.
- Reason: explain why this matters in terms of the project's purpose.
- Design: explain why this approach and scope were chosen, including a meaningful
  alternative or tradeoff when relevant, without a transcript of deliberation.
- Verification and limits: say what was checked and where uncertainty remains.
- References: list root/subtask keys with links to the tracked backlog and sufficient standalone context.

Do not put private machine configuration, full run transcripts or inaccessible evidence
links into the public PR. Supply sufficient public explanation and verification.

## Skill package and implementation sequence

Proposed destination: ~/.codex/skills/implement-change-harness/.

```text
SKILL.md                       # purpose, invariants, coordinator routing
agents/openai.yaml             # discovery metadata
references/roles.md             # three bounded role contracts
references/state.md             # schema, transitions, recovery, publication rules
scripts/harness_state.py        # validated state and artifact-branch operations
scripts/test_harness_state.py   # meaningful recovery/invariant tests in temp repos
```

Keep role-specific material out of unrelated workers' prompts. Prefer one small
standard-library helper over a daemon, database, scheduler or custom agent runtime.
The helper manages deterministic mechanics; Codex handles agent orchestration and
project-specific implementation/verification. No pretending the skill alone is a
durable background service: it resumes when invoked again after a session ends.

Implementation order: (1) settle the policy decisions below; (2) implement and
test schema/Git/checkpoint mechanics; (3) write coordinator and role contracts;
(4) validate skill metadata; (5) simulate worker reports and interrupted runs in
temporary repositories; (6) run one small real task after creation is requested.
Do not open a real PR merely to test the skill's publication code.

## Adversarial validation scenarios

- A second invocation cannot start a different root task while one is active.
- Root/subtask key collisions, absent registrations, retired-key reuse, dangling
  dependencies and cyclic dependencies are rejected.
- Each interruption boundary resumes without skipping verification, discarding
  partial source changes, duplicating workers, or duplicating a PR.
- Stale/late worker reports and verdicts for the wrong SHA/base/plan are rejected.
- The implementation agent reports success but the original reproducer still fails;
  the checker rejects it. A deliberately weakened test does not satisfy acceptance.
- A planned test is omitted, unregistered, skipped or vacuous; the checker rejects
  the candidate despite a passing build or suite exit. A missing test design goes
  back to the planner; an incorrectly implemented planned test goes to implementation.
- A later subtask breaks an earlier one; final whole-task checking rejects it.
- A required GPU/driver check is unavailable; the result stays incomplete.
- PR creation succeeds but the connection fails before state records the URL;
  resume finds the one existing PR. Remote push/branch conflicts preserve work.
- master changes or a user edits source after checking; stale acceptance cannot
  publish the changed candidate as verified.
- Artifact writes fail or state is malformed; no phase advances or success claim.
- Artifact branch contains only records, and source PRs contain no harness records
  or private machine configuration. Original checkout remains untouched.
- Repeated rejection, exhausted budgets and unclear worker liveness stop with a
  precise recoverable state rather than looping or declaring success.

These are proposed tests, not tests already performed. Test helper mechanics with
temporary repositories and mocked external effects, plus one end-to-end exercise
of role behavior when the skill exists. Syntax validation alone is insufficient.

An independent design-review subagent identified the surviving-worker and ambiguous
dispatch windows, the allocation-ledger durability gap, stale approvals after replans,
the moving-base publication window, and the need for a single authoritative Git
checkpoint. The proposal incorporates those corrections. The review is a design
exercise; none of these recovery guarantees have yet been implemented or tested.

## Policy decisions and current limitations

Recommended v1 choices: JSON; repository-local single active root; artifacts local
by default; origin/master as this repository's destination; PR opened as the normal
terminal outcome; fresh sequential agents; bounded retries; supported model fallback
only when recorded. Model availability and tool capabilities are checked per run.

Clarify artifact remote retention if recovery across machines is needed. A shared
artifacts branch and a shared key authority require distributed coordination beyond
the proposed local lock. CI-only-after-PR repositories need a separate CI-pending
state. Decide whether waiting for CI is part of the desired terminal outcome.

No skill-level promise of hostile-agent containment, autonomous wakeups after
process death, exhaustive test coverage, or correctness from model diversity.
