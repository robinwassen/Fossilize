# Change harness design rationale

The original 27 October 2026 proposal is retained in Git history. This document
keeps the design rationale; current operational authority is the
[issue workflow](../issue-workflow.md) and tracked
[implement-change-harness skill](../../.agents/skills/implement-change-harness/SKILL.md).
Task definitions and delivery evidence are maintained on GitHub, rather than here.

## Finite outcome and independent judgment

One explicitly selected issue moves through planning, implementation, independent
checking and one ready-for-review PR to this fork's `master`. The workflow stops
after publication and attachment. It does not discover another task, schedule
itself or merge a PR without explicit bound human approval. Product changes remain bounded by the selected requirement.

The coordinator owns canonical run state and publication. Fresh planner,
implementer and checker contexts separate implementation from judgment; a
coordinator cannot overrule a failed independent checker. Bounded repair, one
bounded replan and a preserved four-hour deadline make stopping conditions
explicit. Different models can share blind spots, so role separation still needs
assertion-bearing tests and independent review of their meaning.

## External memory and source isolation

Small structured state and sanitized reports live on the orphan
`harness-artifacts` ref. Source, committed tests and policy live on the
implementation branch. Detached planner/checker worktrees bind observations to
immutable input/candidate revisions. The repository lock coordinates local
worktrees; this is procedural isolation, not a security boundary or distributed
lock. Preserve dirty inspection views and unrelated user edits.

Artifact transactions use a private index, immutable objects and expected-old ref
updates. Each authoritative checkpoint is published without force and independently
confirmed at its bound push endpoint. Local commits, successful push commands and
old completed run records are distinct from confirmation of the current tip.
Interrupted publication retains a precise retry action, including terminal state
with a null active pointer. Evidence history is never merged into the source PR.

## Task and execution identity

Repository-qualified GitHub issue identity is independent of immutable run/storage
identity. Issues persist across runs; work units bound implementation/checking
inside a run; steps describe planned actions. Snapshots preserve task inputs for
checking and detect material changes before publication. Historical schema/legacy
allocation facts remain evidence, with explicit migrations rather than fallback
to removed task definitions or rewritten historical commits.

## Verification and delivery

The planner designs meaningful behavioral cases and gates. Implementers write and
run committed tests; fresh checkers inspect assertions, rerun required checks and
reject missing, weak or unavailable evidence. Exact candidate/base/plan envelopes
fence stale reports. Accepted work units require a separate final integration
check. Changed inputs invalidate acceptance and readiness.

Managed issue evidence records verified PR readiness while tasks remain open.
Actual PR links and explicit eligible closing references connect verification to
user-merged delivery. Run completion, work-unit acceptance, PR readiness and issue
delivery are separate facts. Uncertain creation/comment/attachment operations are
reconciled before retry; one source PR identity survives interruption.

These design choices apply external memory, bounded context and explicit stopping
conditions from [Loop Engineering](https://walkinglabs.github.io/learn-harness-engineering/en/lectures/lecture-13-loop-engineering/).
They are rationale, not evidence that any particular run or product behavior passed.
