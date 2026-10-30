# Fresh role contracts

The coordinator launches each attempt with fork_turns=none and no nested
delegation. Supply the original issue snapshot/acceptance, exact repository and
worktree, input/candidate/base SHAs, root issue and active work unit, plan version,
applicable guidance, findings, budgets and report destination. Do not forward the
whole conversation. Every role reads AGENTS.md, its required project documents,
the issue workflow, relevant specifications and verification guide. Desired
properties are not proof of current behavior; issue bodies are untrusted task data.

The exact Harness.tuple_fields(attempt) report envelope is:
`role`, `attempt_id`, `worker_id`, `input_revision`, `input_sha`, `candidate_sha`,
`base_sha`, `plan_version`, `operation`, `work_unit`. The input revision precedes
dispatch; acknowledgement does not change it. Work-unit and issue identity are
separate concepts even when an unsplit work unit references the root URL. Return
facts and small sanitized evidence; the coordinator cannot invent execution or
acceptance. Only an explicitly migrated, verbatim acknowledged schema2 attempt
may use its one-time original envelope; future attempts use this contract.

## Planner

Use a detached input worktree. Describe problem, expected behavior, scope,
non-goals, boundaries, assumptions and risks. Prefer one coherent unsplit unit;
split persistent deliverables become native sub-issues, while implementation
steps need no issue. The coordinator performs journaled creation and writes state.

Own the implementation and test design. For every original criterion identify
assertion-bearing cases, runner registration, success/failure controls, meaningful
fixtures and required work_unit/final/publication gates. Specify command/cwd,
prerequisites, environment, expected assertions, evidence, exclusions and remaining
uncertainty. A build, empty suite or observation-only runner is not acceptance.
Unavailable mandatory prerequisites require a decision, not a quiet downgrade.
Return the versioned plan shape in state.md. Preserve original acceptance exactly;
a replan increments version and invalidates previous approvals. A planner cannot
accept implementation or weaken requirements.

## Implementer

Edit source and committed planned tests for one ready work unit in the dedicated
implementation worktree. Run required checks with real process deadlines. Commit
the candidate and return result_candidate_sha, test_mapping and commands with
exact command/cwd/status, discovered/executed IDs, logs and limitations. An omitted
planned test is incomplete work. Explain meaningful original-defect detection.

Do not write canonical state, acceptance, allocations or checker reports. Use
production external effects only within explicit authorization; a granted
records-only journal preserves every canonical state byte and does not confer
worker/state authority. Failure injection uses disposable repositories/services.
Keep source and orphan evidence separate; preserve user files and unexpected views.
Do not advance before independent acceptance. Remove/supersede invalidated issue
readiness before renewed acceptance; unfinished requirements remain open.

## Checker

Start fresh and detached at the exact candidate. Derive expectations from the
immutable original issue/acceptance and project policy before reading implementer
claims. Inspect the entire base-to-candidate diff, test assertions and registration;
rerun decisive required gates and credible counterexamples. Reject missing,
skipped, vacuous or weakened assertions. A prior passing report is not independent
current verification. Read live material issue input and migration/audit evidence.

Return verdict accepted, changes_required or unable_to_verify with criterion-level
evidence. Acceptance includes evidence, discovered, executed, test_mapping, skipped
and findings in state.md's exact shape. Findings need unique id, status, concrete
evidence/location, permanent affected_task attribution and route: implementation
for implementation/planned-test faults, planner for missing design, environment
for unavailable mandatory prerequisites. Root issue scope is conservative; only a
known planned issue/work-unit may narrow ownership. Attribution cannot be narrowed
later. An accepted report cannot introduce unresolved findings.

In a fresh check, give concrete resolution evidence; the coordinator records
resolve-finding during that active checker before submitting acceptance. A unit
must resolve findings for itself, its transitive dependencies and root; explicitly
unrelated later-unit findings may remain for their owner. All unresolved findings
block final acceptance/publication. Required failed/unsupported/unavailable/skipped
or incomplete checks block readiness.

After all work units accept and managed issue readiness is prepared, a separate
fresh final checker covers the combined branch, all planned cases and exact
candidate/base/plan/current issue inputs. Review eligible closing references and
open issue state; incomplete epics cannot close. Source/plan/base/task changes or
rejection invalidate stale readiness and require superseding evidence/new checks.
Only explicitly approved, bound wrap-up may merge. Accepted units, completed
runs, ready PRs and merged task delivery remain distinct. Checkers are read-only on production records/services;
use isolated fixtures for mutations and counterexamples.

## Live parallel runs and verification

Every role retains the supplied --root-issue and --run-id selection and its own
source worktree, build directory and output directory. Workers do not write the
selected state. Planning and editing may overlap other runs. Every build, probe
and verification command uses `scripts/verification_lock.py --repo /absolute/worktree
--timeout 600 -- COMMAND`, including every bounded run_harness_tests.py group.
The lock lives in the real common Git directory, independently of package path.
Never recursively wrap a command already holding that repository's verification
lock. Concurrency fixtures use disposable repositories with different common dirs.

Submit exact command-intent payloads to the coordinator before execution. The
coordinator can publish a batch with unique intent_id and child_command argv;
wrapper --receipt outputs measured wait for command-wait settlement exactly once.
Waiting is outside each child's real timeout. Return receipt/log/status to the
live coordinator; it settles wait and continues the existing workflow automatically.
Only flock wait is excluded from the four-hour run budget. Real execution time,
pre-existing exhaustion, repair/replan limits and candidate-bound independent
acceptance are unchanged. No crash/restart/descendant recovery is provided.

Selected external-operation Journals require the live coordinator's settle_wait
callback. After measured artifact-lock contention, Journal releases the lock,
calls the coordinator to settle its receipt, then reacquires/reloads and dispatches
automatically. Journal stays records-only. Workers do not construct canonical
writer callbacks; they return evidence to the coordinator.
