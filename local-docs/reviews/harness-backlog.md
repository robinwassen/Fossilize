# Harness backlog

## FOS-000026 — Include backlog completion in the implementation PR

**Status:** Done (verified proposed branch state; not landed on master).
**Completion evidence:** Independent unit checker passed 34/34 Python/Git tests with no skips, skill metadata validation, canonical-preservation review and historical PR #2 provenance checks. No C++/GPU/Steam coverage claimed; separate exact-candidate renewal and final review remain required. https://github.com/robinwassen/Fossilize/compare/master...codex/FOS-000026-backlog-completion

**Depends on:** FOS-000025 (implementation merged in [PR #2](https://github.com/robinwassen/Fossilize/pull/2)).

### Problem

The harness records a completed run on `harness-artifacts`, but does not update
its canonical backlog items. A delivered task can therefore still say “in
progress,” as FOS-000025 currently does. Run completion is not reflected in the
backlog that future agents and contributors read.

### Required outcome

Add backlog completion as the final bookkeeping step on the implementation
branch, included in the same implementation PR as the task and its subtasks.
After the PR merges, the Done markers reach `master` with the implementation.
On an unmerged branch, Done describes that branch's verified proposed state;
it must not claim that the change has already landed on `master`.

This supersedes the earlier suggestion to wait for merge and then publish a
separate closeout change. Do not add a merge monitor, scheduler, direct write to
`master`, or separate closeout PR.

### Acceptance criteria

- Update the skill, role/state guidance and helper only as needed to make this
  a concrete final step. The planner decides the appropriate implementation and
  necessary test-suite additions using the [verification guide](../verification.md).
- Mark the root and each completed subtask Done in their canonical backlog
  definitions only after the applicable acceptance criteria and required
  verification pass. A parent stays incomplete while any required subtask or
  criterion remains open. Failed, unsupported, unavailable or incomplete required
  checks cannot justify Done.
- Preserve permanent keys, canonical definitions, dependencies and original
  acceptance criteria. Leave unfinished items open. Do not change allocation
  state in the permanent key registry to represent implementation completion.
- Retain concise verification evidence and an honest implementation-PR reference
  with the status update. Do not invent a PR number or claim a PR exists before
  creation. The planner must choose a reference that works with the publication
  order; a stable branch/compare reference is acceptable until an actual PR URL
  is known. Do not create an early PR solely to obtain a number.
- Include the bookkeeping changes in the final independently reviewed candidate.
  If source or backlog edits occur after review, invalidate/repeat the affected
  checks under the existing exact-commit rules. Publishing an unreviewed status
  commit must not bypass the checker or produce duplicate PRs.
- A checker rejection or later change that invalidates completion must also
  invalidate the corresponding Done claim. Interruption/resume must preserve
  unfinished closeout work rather than report successful bookkeeping that never
  happened.
- One implementation PR carries the root, subtasks and backlog updates. The
  harness never merges it automatically. Merging it is sufficient to update the
  backlog on `master`, with no subsequent closeout invocation required.
- Apply the resulting completion rule to this item in its implementation PR.
  Reconcile the stale FOS-000025 status only against its actual merged PR and
  retained verification evidence; do not mark other pending backlog work Done.

### Verification expectations

The planner owns the verification plan and test design; the implementer writes
and runs the planned tests; the checker independently validates their assertions
and execution. Cover a completed root, completed and incomplete subtasks, failed
mandatory verification, checker rejection after a proposed Done marker, and
interruption/resume before closeout is recorded. Verify that the reviewed
implementation PR includes the intended backlog edits and preserves keys and
acceptance text. Use behavioral tests for changed helper mechanics and realistic
isolated workflow checks for coordinator instructions; do not rely only on text
matching. Do not claim these future checks have already passed.

### Scope

Harness workflow, canonical backlog status/evidence, and meaningful verification.
No Fossilize C++ changes, automatic merge, background polling, retrospective
claims of unperformed verification, or unrelated backlog implementation.


## FOS-000027 — Commit and publish harness artifacts to the remote orphan branch

**Status:** Done (verified proposed branch state; not landed on master).
**Completion evidence:** Independent unit checker passed 51/51 Python/Git tests with no skips, metadata validation, original-criteria review and allocation-history counterexamples. Required exact-candidate renewal and separate final review follow bookkeeping; no C++/GPU/Steam coverage claimed. https://github.com/robinwassen/Fossilize/compare/master...codex/FOS-000027-artifact-publication

**Depends on:** FOS-000025 (existing harness checkpoint and publication mechanics).

### Problem and reviewed evidence

Harness state, reports, evidence and allocation records remain in the local
`harness-artifacts` orphan history. A pushed implementation branch and an open PR
therefore do not make the run records available remotely.

Review on 2026-10-28 found that the installed `implement-change-harness` skill
explicitly selects local-only artifacts. Its `scripts/harness_state.py`
`checkpoint()` already writes a complete records tree with a temporary index,
creates a commit with `commit-tree`, and advances `refs/heads/harness-artifacts`
with an expected-tip `update-ref`. The publication workflow pushes the
implementation branch, but has no equivalent artifact publication step.
`git ls-remote --heads origin harness-artifacts` returned no branch, while the
local artifact tip was `5c0c6e5` (FOS-000026 publication revision 33).

The artifact inspection worktree can show modified/deleted records because ref
updates intentionally leave its index/files untouched; `references/state.md`
documents this. Its apparent dirtiness is not proof those records lack commits.
Inspect the committed tree rather than blindly staging that worktree or resetting
it. The original [proposal](../specs/basic-change-harness-plan.md) also specifies
local-only retention; this item deliberately changes that policy.

### Required outcome and acceptance criteria

- Make committing and publishing the allowed harness records to
  `origin`'s `refs/heads/harness-artifacts` part of the normal workflow. Record the
  repository/remote identity explicitly, keep the branch orphaned and separate
  from source history, and update the skill, state guidance and helper as needed.
- Define and enforce publication checkpoints covering initialization, state
  transitions and reports/evidence, permanent allocation changes, interruption
  and terminal run state. Distinguish locally committed from remotely confirmed
  records; retain a precise retry action when publication is incomplete. A run
  cannot claim successful artifact publication or full workflow completion while
  required records exist only locally.
- Publish the authoritative committed ref, including existing valid local run
  history. Preserve the records-only tree and existing validation. Do not stage
  stale inspection files, copy source trees, or include build outputs, binaries,
  private configuration, secrets or full transcripts. Preserve unexpected local
  edits when inspecting or refreshing a worktree.
- Use explicit non-force pushes to the recorded artifact destination. Verify the
  remote ref against the checkpoint being published; command success alone is
  insufficient. Never merge artifact history into the implementation PR or
  `master`, and never push to `upstream` merely because it exists.
- Handle an absent remote branch, already published checkpoints, remote advances
  and divergent history without overwriting either writer's work. Validate any
  adopted remote artifact history. Reconcile remote state before continuing;
  unresolved conflicts retain local records and a concrete recovery action.
  A repository-local lock must not be described as cross-machine coordination.
- Network/authentication/rejection failures and ambiguous push outcomes preserve
  the local checkpoint and remain recoverable. Resume checks the actual remote
  ref before retrying, without rerunning accepted implementation work, duplicating
  a PR or dropping allocation reservations. Define terminal publication ordering
  so the final state/active-pointer update is also published, without an endless
  cycle of commits acknowledging their own publication.

### Verification expectations

Use the [verification guide](../verification.md) to select meaningful behavioral
checks in isolated repositories with a bare remote. Cover first publication,
subsequent checkpoints, existing local history, idempotent retry, push rejection,
remote divergence, and interruption before/after a successful push but before its
acknowledgement. Fetch the artifact branch into a fresh clone and assert the
expected state, reports/evidence, allocation journal and terminal active pointer
are present in committed records. Check orphan ancestry, source/artifact separation
and preservation of stale inspection-worktree edits. Assert failed publication is
reported as incomplete and resume completes publication without recreating a PR.
These are future acceptance checks, not checks already performed for this item.

### Scope

Harness artifact persistence/publication, recovery, documentation and regression
coverage. No Fossilize C++ changes, automatic merge, background scheduler or claim
that remote retention alone solves distributed task-key allocation.
