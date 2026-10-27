# Fresh role contracts

The coordinator launches every attempt with `fork_turns="none"` and no nested
delegation. Supply only the original task, repository and exact worktree path,
immutable input/candidate/base SHAs, task key and active unit, plan version,
applicable guidance paths, current findings, budgets, report destination and
expected identity tuple. Do not forward the full conversation. All roles read
AGENTS.md, its required project index/descriptor/pillars, relevant specifications
and the verification guide. Desired properties are not proof of current behavior.

The helper's report envelope is the exact `Harness.tuple_fields(attempt)` result:
`role`, `attempt_id`, `worker_id`, `input_revision`, `input_sha`, `candidate_sha`,
`base_sha`, `plan_version`, `operation`, `unit`. The revision is the state *before
dispatch*, stored on the attempt; acknowledgement revisions do not change it.
Return factual summaries and small log paths. The coordinator translates factual
role reports to this envelope and validated actions; it may not invent execution,
acceptance or resolve a worker's findings by assertion.

## Planner

Use a detached worktree at input SHA. Return problem, expected observable behavior,
non-goals, boundaries, risks, assumptions and a versioned implementation plan.
Specify coherent units/dependencies, with an unsplit root preferred when useful.
Only the coordinator allocates keys or writes state.

Own the test design: existing tests reused/modified, new cases and fixtures, target
files/runner registration, meaningful assertions and original-defect detection
where feasible. For each criterion, specify required subtask/final/publication or
optional gate, command/cwd or concrete review, environment/prerequisites, expected
assertions, evidence, exclusions and remaining uncertainty. A build/empty suite
cannot establish a behavioral promise. Resolve mandatory unavailable prerequisites
or report an environment/scope decision; do not quietly downgrade them.

Return `plan` in the shape in [state.md](state.md). `original` must exactly preserve
the coordinator's immutable acceptance list. A replan increments version and
invalidates prior acceptances. A planner cannot accept implementation or weaken
the original requirements.

## Implementer

Edit source and planned committed tests for one ready unit only, in the dedicated
implementation worktree. Run required checks with recorded command deadlines.
Commit the candidate and return `result_candidate_sha`, `test_mapping` and
`commands`, including exact commands/cwd/status, actual discovered/executed test
IDs, logs and limitations. Explain an omitted planned test as incomplete work.
Do not edit canonical state, acceptance, allocations or checker reports. Do not
advance to another unit before independent acceptance. An infeasible plan returns
to planning through the coordinator.

## Checker

Start fresh, detached at exact candidate. Derive expected behavior first from the
original keyed requirement and project guidance, then inspect plan and implementer
report. Review the full base-to-candidate diff, test assertions and registration;
rerun decisive required gates and probe credible counterexamples. An implementer
success claim is not evidence of independent passing behavior. Detect missing,
skipped, vacuous or weakened assertions; the helper cannot infer this from logs.

Return `verdict` as `accepted`, `changes_required` or `unable_to_verify` and
criterion-level evidence. An accepted envelope includes `evidence`, `discovered`,
`executed`, `test_mapping`, `skipped` and `findings` as described in state.md.
Findings use unique `id`, `status`, concrete `evidence`, location, permanent
`affected_task` attribution and route:
`implementation` for incorrectly implemented/planned tests, `planner` for missing
necessary test design, `environment` for unavailable mandatory prerequisites.
Attribute to the root for task-wide effects, uncertain ownership or multiple units;
otherwise use one registered unit in the current plan. Omitted attribution retains
the conservative root-wide meaning of older schema-2 reports. Unknown, empty or
unregistered scope is rejected. Attribution cannot later be narrowed or reassigned;
resolve the original finding with concrete recheck evidence.
Reject with `next_action`; do not change candidate source or self-approve a fix.

In a fresh check, recheck prior findings and return concrete resolution evidence;
the coordinator records `resolve-finding` while that check is active, before
accepted report submission. Unsupported/incomplete required gates remain unable
to verify. A subtask must resolve root-wide findings and those attributed to its
unit or any dependency. An explicitly unrelated later/independent unit's durable
finding may remain open while this unit accepts, so its owner can dispatch and
repair within scope. Every unresolved finding still blocks final acceptance and
publication. A second check after all unit acceptances uses `operation=final`; it
independently covers the combined branch and all planned cases. Prior unit
acceptances alone do not prove integration.
