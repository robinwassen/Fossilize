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
