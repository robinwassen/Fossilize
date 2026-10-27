# Local state and recovery contract

## Authority and invocation

Python 3 standard library, Git and POSIX `flock` are runtime requirements. Run from
the implementation worktree (or pass `--repo`). The helper acquires the same
`harness-run.lock` in the repository's common Git directory from every worktree.
It fails promptly on contention. Never delete the lock file or bypass a live
owner. An existing bootstrap coordinator holding it must finish its write and
release it before helper handoff. Worker liveness is independent of lock liveness.

```sh
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation show
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation initialize --payload /tmp/init.json --expected-tip none --expected-revision 0
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation dispatch --payload /tmp/dispatch.json --expected-tip FULL_ARTIFACT_SHA --expected-revision 1
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation detached-worktree /tmp/new-checker FULL_CANDIDATE_SHA
```

Every mutation uses a JSON file (`--payload -` reads stdin), exact artifact tip and
expected state revision from the last result. A stale tip/revision fails without
advancing the ref. `show` reads committed blobs; it never adopts uncommitted files.
The equivalent importable API is `Harness(repo).show()` and
`Harness(repo).apply(action, payload, expected_tip, expected_revision)`.
`Harness.tuple_fields(state['attempts'][-1])` builds a report envelope.
`detached_worktree(repo, path, sha)` creates a new path only; it never resets or
cleans an existing worktree. CLI wraps it in the repository lock.

An initialization payload (replace SHAs/paths and original acceptance):

```json
{
  "task_key": "FOS-000025",
  "run_id": "2026-10-27-example",
  "task_location": "local-docs/specs/basic-change-harness-plan.md#fos-000025--implement-the-change-harness-skill",
  "original": [{"id": "AC1", "requirement": "Exact original requirement"}],
  "repository": {
    "input_sha": "FULL_INPUT_SHA",
    "base_sha": "FULL_TARGET_MASTER_SHA",
    "target_remote": "origin",
    "target_repository": "robinwassen/Fossilize",
    "target_branch": "master",
    "branch": "codex/FOS-000025-example",
    "implementation_worktree": "/absolute/implementation"
  },
  "limits": {"repairs": 3, "replans": 1, "command_timeout": 600},
  "ci_policy": "local"
}
```

`initialize` requires the root's active registration and canonical definition in
committed local-docs. The same active identity resumes idempotently; another root
or run is rejected. Completed identities cannot be reused. The default deadline
is invocation time plus four hours; optionally supply an absolute Unix `deadline`.
Use `ci_policy=required_after_pr` only when declared before work.

## Artifact transaction and schema

Schema 2 requires immutable root/run/task/original/repository identity, monotonic
revision, phase/operation/status/next action, plans, units, attempts, verification,
findings, budgets, provenance and publishing. Phases are planning, implementing,
checking and publishing (plus initializing/complete); status is distinct. Current
state is `runs/<root>/<run>/state.json`; `active.json` points to it until completion.
Completion and clearing the active pointer occur in the same commit. Failed or
incomplete work keeps the pointer.

The helper constructs a complete records tree with a private temporary index,
`hash-object`, `write-tree`, `commit-tree`, then `update-ref` with expected old tip.
The artifact branch starts orphaned and has no source ancestors. Every adopted
ancestor is checked for regular, bounded UTF-8 records only. Permitted paths are
`active.json`, `allocations.json`, and each run's state, `reports/<name>.json`, or
`evidence/<name>.json|txt|log`. Individual records are at most 1 MiB; no symlinks,
binary/NUL data, traversal or arbitrary source paths. Reports/evidence are
append-only. A failed write/tree/commit/CAS leaves the old checkpoint authoritative;
unreachable Git objects may remain until normal Git garbage collection.

A separate artifact worktree is an inspection view. Ref updates intentionally do
not rewrite its index/files. Before refreshing it, inspect its status and preserve
unexpected edits separately; use a new detached view rather than discarding edits.
Never merge/push artifacts with the source PR. Local-only records cannot recover a
lost machine; independent clones do not share this lock or allocation authority.

## Plans and permanent keys

A planner report's `plan` uses:

- `version`, exact `original`, `scope`, `non_goals`, `implementation`;
- `cases`: objects with unique `id`, `assertions`, `location`, and optionally
  `units` containing applicable split keys (final checking always covers all);
- `gates`: unique `id`, `kind` (`subtask`, `final`, `both`, `publication`, `optional`),
  nonempty `criteria` IDs, `method`, `cwd`, `prerequisites`, `expected`;
- `subtasks`: empty for an unsplit root, otherwise objects with unique registered
  `key`, permanent `title`, `definition`, `dependencies`, `criteria`.

The helper validates criterion coverage, registrations, reservations and acyclic
subtask dependencies. It does not decide semantic plan adequacy. `publication`
gates describe the coordinator's external sequence below, rather than checks that
could already have passed before a PR exists. Include review of publication
readiness in final gates as appropriate. Keep the original detailed planner report
as a separate artifact when normalizing its fields into this compact schema.

`reserve` takes `title` and branch-qualified canonical `location`. It reserves
strictly above the union of registry rows, canonical current/archived definitions,
journal entries and journal high-water. Interrupted reservations remain issued.
Then the coordinator updates and commits the tracked registry before publishing
a subtask definition in a plan. Reads/reservations never edit the registry.

The Markdown scanner recognizes this project's ledger table, keyed headings and
numbered bold keyed definitions; prose references are not definitions. It verifies
one canonical location/title per key and explicit `depends on` clauses in definition
metadata. Other backlog syntaxes require an explicit reviewed parser extension;
do not claim arbitrary Markdown or cross-machine allocation support. Retired
entries contribute to high-water and cannot be reused as active subtasks.

## Attempts, reports and bounds

`dispatch` takes `role`, unused `attempt_id`, `requested` and `actual` model/effort
objects and the observed `catalog` mapping model names to supported effort arrays.
Different requested/actual choices require `fallback_reason`. Record the actual
runtime catalog; the helper cannot query or authenticate it. It stores intent
before launch. `acknowledge` takes that attempt ID and actual `worker_id`.

`report` must carry every exact identity-envelope field from roles.md, plus:

- planner: the complete `plan` (initial version 1, then increment by one);
- implementer: `result_candidate_sha`, nonempty `test_mapping` and `commands`;
- checker: `verdict`, findings and exact evidence below. Rejection needs findings
  and `next_action`; acceptance cannot contain unresolved findings.

Supply additional small text/JSON artifacts in `records`, a mapping of permitted
full run evidence/report paths to text or JSON values. Accepted/rejected role
reports themselves are committed automatically at `reports/<attempt_id>.json`.
For checker acceptance, each mandatory gate for the operation needs `gate`,
`result=passed`, `exit_code=0`, `command`, `cwd`, `environment`, existing `log` path,
`criteria`, `candidate_sha`, `base_sha`, `plan_version`, `attempt_id`. The report
also needs nonzero `discovered` and `executed` test-ID arrays, empty `skipped`, and
`test_mapping` mapping each applicable planned case to `location` and executed
`tests` IDs. Missing/failed/unsupported/incomplete/not-run evidence blocks acceptance.
Optional diagnostics retain their own outcomes. Logs and declared discovery are
procedural evidence, not authenticated proof that a command ran or tests are good.
The checker independently establishes meaning and completeness.

`reconcile` records `attempt_id`, `liveness` (live/unknown/terminated), concrete
`observation`, `next_action`. Intent without worker acknowledgement is ambiguous;
only confirmed termination unlocks a replacement. Old reports remain fenced.
`resolve-finding` takes `finding_id` and concrete `evidence`, only during a fresh
active checker. `resume` records resolved-prerequisite `observation`/`next_action`
for an environment decision; it cannot waive mandatory evidence or exhausted bounds.

Initial implementation plus three repairs per unit and one replan are defaults.
`dispatch` checks deadline/counters on invocation. Repeated unchanged rejection
also stops early; concrete `new_evidence` in a report distinguishes a new probe.
`budget-extension` needs recorded human `authorization`, `reason`, and increased
`deadline`/`repairs`/`replans` as applicable. No automatic timeout watcher exists.
`command-intent` takes `command`, `cwd`, positive `timeout` at most the configured
command timeout; persist it before the coordinator executes that command using a
real process deadline. The helper does not execute shell text or cancel processes.

`observe-source` takes committed `candidate_sha` and optionally `base_sha`. It
requires a clean matching implementation branch/worktree, invalidates unit/final
acceptance and returns to implementation. It preserves prior publication facts.
Changed HEAD/dirty/untracked work cannot publish under an older final acceptance.

## Publication observations

After separate final acceptance, fetch target master and compare accepted base.
If moved, integrate through implementation and recheck. `publish-intent` takes
exact `target_remote`, `target_repository`, `target_branch`, `branch` from state.
It validates clean matching local HEAD, current observed remote base and accepted
candidate/base/plan before persisting intent. It does not push or call GitHub.

Use normal non-force `git push origin <branch>`. `publish-observation` takes:

1. `event=push`, `remote_sha`: helper independently reads `ls-remote` and validates
   the candidate head. Failure/conflict preserves source; never force push.
2. `event=lookup`, `all_states=true`, `unambiguous=true`, `matches=[]` or a singleton
   exact PR object: `target_repository`, `target_branch`, `branch`, `state=open`,
   `draft=false`, `url`. Closed/merged/multiple/wrong matches block progress.
3. Only an exact empty lookup permits `event=create-dispatched`. Commit this
   *before* the coordinator's one GitHub create call; stage returns to lookup.
   Always reconcile using step 2, including after response loss. Never infer
   absence from a failed or ambiguous lookup.
4. With a recorded matching open PR, call Codex `attach_artifact`, then record
   `event=attachment`, matching `url`, `success` and useful outcome. A failed
   attachment leaves only attachment pending. Success completes and clears active
   atomically. Under predeclared required-after-PR CI, record `event=ci`,
   `result=passed`, `evidence` before completion.

Before each publication observation the helper rechecks local candidate, remote
base and (after push) remote head. This covers observed commits only; target refs
can move after the observation. GitHub lookup/create/attachment observations must
come from the actual tools; the helper has no network/API transport and cannot
validate a fabricated PR object. Preserve the one-PR identity across recovery.

## Explicit bootstrap import

Legacy schema 1 is rejected by `show` and ordinary mutations. Inspect it read-only
with `git show harness-artifacts:runs/<root>/<run>/state.json`. After the live lock
owner releases the common-dir lock, use `import-bootstrap` with expected legacy
tip/revision, the initialization identity/original fields, and a normalized `plan`
matching the existing planner report/version. Do not invent a plan if absent.
If an actual legacy implementation report is already committed, optionally supply
`implementation_report` with its full run report path. Its role/attempt/input/plan
identity and clean candidate HEAD must match known legacy facts. The candidate is
imported as unverified, and the known implementation count consumes its budget;
it still requires fresh independent subtask and final checks. No legacy fenced
attempt or test evidence is fabricated.

Import verifies root/run/location/branch/input/base/original acceptance consistency,
normalizes abbreviated commit SHAs, keeps the old state verbatim in provenance,
keeps old report blobs and Git ancestry, and reconciles registry-location
placeholders. The oldest state commit timestamp starts the old four-hour budget;
migration never grants a new one. No old test execution or acceptance is fabricated.
Legacy worker facts stay in provenance, not in new fenced attempts.

Import stops at `bootstrap-reconciliation`. `reconcile` then requires concrete
runtime `observation`, `terminated_workers` listing every known legacy worker,
and `next_action`. Confirm completed/terminated actual workers, including a
previously running implementer, before continuing. Preserve its report as a fact;
new fenced checks provide current tuple-bound results. If any worker outcome
is unknown, stop for intervention. This is an explicit schema migration and
procedural handoff, not retrospective certification of incomplete old records.
