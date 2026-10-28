# State, artifact publication and recovery contract

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
checking, closing and publishing (plus initializing/complete); status is distinct. Current
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
Never merge/push artifacts with the source PR. Each required records checkpoint
is published separately as described below. Independent clones do not share this
repository-local lock or allocation authority.

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
subtask dependencies. Initial plans, replans and source resets select the first
dependency-ready unit regardless of array order; implementer dispatch also checks
that all dependencies have accepted. It does not decide semantic plan adequacy.
An implementation rejection at final checking invalidates every unit approval and
restarts with the first dependency-ready unit; renewed unit checks precede another
separate final check.
`publication` gates describe the coordinator's external sequence below, rather than checks that
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

Finding IDs must be unique both within one report and across durable findings; a
duplicate report is rejected without advancing the checkpoint.
`affected_task` permanently attributes each finding to the root (task-wide) or a
registered unit in the report's current plan. Missing attribution in old or new
reports means root-wide; explicit null, empty, unknown, retired or unplanned keys
are invalid. Use root-wide scope for uncertain or multiple-unit effects. This is
ownership of a concrete finding, not a waiver: subtask acceptance blocks on all
unresolved durable findings for its own unit, its transitive dependencies or the
root, while explicitly unrelated unit findings remain open for their owner's
repair. Accepted reports cannot introduce any unresolved finding. Final acceptance
and every publication guard continue to require all findings resolved.

Replans retain prior validated unit declarations under `provenance` entries named
`superseded_plan` (version and subtasks); full old plans remain in their reports.
Finding attribution is preserved when plans change. A finding whose unit is absent
from the new plan conservatively blocks every unit, rather than disappearing.
Current dependency relationships determine applicable findings for remaining units;
replans still invalidate every prior approval. No resolution or replan action may
reassign an existing finding to a narrower task.

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
active checker. It retains `affected_task`; an optional conflicting attribution is
rejected. `resume` records resolved-prerequisite `observation`/`next_action`
for an environment decision; it cannot waive mandatory evidence or exhausted bounds.

Initial implementation plus three repairs per unit and one replan are defaults.
Forward-work invocations (`dispatch`, `command-intent`, `observe-source`,
`reserve`, `publish-intent` and every `publish-observation`) pause before progress
when the deadline or a recorded budget is exhausted. Valid late reports and worker
observations are retained as facts, then leave the run awaiting a budget decision;
even late final acceptance cannot authorize publication. The pause saves the exact
operation, status and next action. An authorized extension with an unexpired
deadline restores that step, preserving live-worker fences and completed reports.
Repair/replan counters are also checked at dispatch. Repeated unchanged rejection
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

## PR description

Use these explicit level-two Markdown headings in every harness PR description.
Write plain language that a reader familiar only with the project can understand.
Keep each section proportional to the change; headings do not require long text.

```markdown
## Problem
Describe the affected workflow and observable problem.

## What changes
Explain the behavior before and after the change.

## Why this matters
Explain why solving the problem matters for this project.

## Why this approach
Explain the chosen shape and scope, including a meaningful tradeoff when relevant.

## Verification and limits
State what was actually checked and what remains unverified.

## Task references
List the root and any subtask keys with links to their canonical definitions.
```

Do not include private machine configuration, full transcripts or inaccessible
evidence links. The description must make sense without reading the run history.

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


## Canonical backlog closeout (FOS-000026)

After all required units have independently accepted, the next action is durable
closeout rather than final checking. A parent remains incomplete while any required
child/criterion is open. Every unit must have one tracked canonical definition;
artifact-only legacy subtasks need explicit migration preserving permanent keys,
titles, original criteria/dependencies and the registry's allocation meaning.
The helper verifies the canonical inventory at closeout; it does not edit Markdown.
It conservatively invalidates all closeout targets after rejection or source/base/
plan change. Independently completed unrelated work outside this root is untouched.

1. `closeout-intent` payload:
   `{"targets":[{"key":"FOS-000026","location":"local-docs/reviews/harness-backlog.md","evidence":"Independent unit checker passed required gates; concise actual counts and limits.","reference":"https://github.com/robinwassen/Fossilize/compare/master...codex/FOS-000026-backlog-completion"}]}`.
   Include the root and every required unit exactly once. All units must have
   accepted, with applicable base/plan and no unresolved findings. The helper
   persists their acceptance provenance and the exact pre-bookkeeping source SHA.
2. In one direct child commit, edit only target canonical files and these metadata
   lines inside each target definition (add/replace a status line in the same place):
   `**Status:** Done (verified proposed branch state; not landed on master).`
   and `**Completion evidence:** <exact evidence> <exact reference>`.
   Preserve every other byte, including blank lines, original acceptance,
   dependencies, titles, registry allocations and unrelated definitions. This
   restricted project grammar deliberately does not implement arbitrary Markdown.
   A stable compare reference avoids a self-referential SHA or invented early PR.
3. `closeout-record` takes `{"candidate_sha":"FULL_BOOKKEEPING_SHA"}`. It requires
   the clean matching branch and one direct bookkeeping commit containing actual
   Done/evidence/reference bytes. It clears exact-commit unit/final approvals and
   enters checker-only `subtask` renewal. Dispatch/acknowledge/report as usual,
   without implementer dispatch or repair-budget consumption. Renewed acceptance
   of every unit enters separate `final`; prepared closeout survives successful
   renewal and is required for final acceptance and every publication operation.
4. Rejection or later tuple changes persist `stage=cleanup`, the original targets,
   reason and an explicit pending cleanup action. Interruption keeps active state.
   Remove stale Done metadata and completion evidence from all affected targets,
   commit the honest incomplete source, then `closeout-cleanup` with
   `{"candidate_sha":"FULL_CLEANUP_SHA","observation":"Stale Done removed; required work remains open."}`.
   It verifies no targeted Done marker remains, retains invalidated provenance,
   clears approvals, and returns to repair/replan. A fresh implementer and checks
   resolve the concrete finding; only then repeat intent/bookkeeping/renewal once.
   An environment rejection first needs the existing prerequisite `resume` action.

Reopen with `show` after interruption. Pending -> persist intent; intent with no
commit -> make the bookkeeping commit; intent with the clean direct child already
committed -> record that same SHA without duplicating edits. Prepared -> resume
checker-only renewal/final. Cleanup -> reconcile tracked source before dispatch.
Dirty/wrong/unrelated candidates cannot be adopted as bookkeeping. Every action
uses the usual artifact tip/revision CAS and append-only report/log transaction.

Existing active schema-2 records lacking `closeout` are read as pending; the next
mutation persists that field without changing immutable requirements, reports,
worker facts, history or budgets. No new run, import, deadline or fabricated
acceptance is needed. Complete historical records remain byte-for-byte untouched.
For self-application, first accept the implementation candidate using the existing
run. Invoke the resulting helper from that candidate in the same implementation
worktree, persist closeout intent, commit FOS-000026's actual status/evidence,
record the new candidate, renew unit checks and obtain separate final acceptance.
The stable compare link remains valid after PR creation; actual PR URL belongs in
publication records/description. Publish exactly once under the existing sequence;
no subsequent closeout invocation or automatic merge occurs.


## Artifact publication (FOS-000027)

This policy supersedes the original local-only proposal. Initialization, every
state transition/report/evidence addition, permanent reservation, interruption or
reconciliation, closeout, source-publication observation and terminal commit are
required artifact checkpoints. The helper commits records locally first, then
makes one bounded non-force push of `CHECKPOINT_SHA:refs/heads/harness-artifacts`.
It independently reads the actual remote ref before retry and after push. Its
transport attempt has a 60-second total deadline and at most 20 seconds per network
command, with terminal prompting disabled. Network errors are sanitized.

`allocations.json.artifact_publication` binds policy version, `origin`, declared
owner/repository, exact artifact ref, SHA-256 of the actual single push endpoint,
and sanitized semantic privacy review identity/summary. This journal-level policy
survives clearing the active pointer. Fetch, push and confirmation all use the
bound push endpoint, including when origin has a different fetch URL. Hosted URL
repository paths must match the declared repository; local file transports bind
the explicitly configured endpoint. Multiple push URLs, changed endpoints, wrong
repositories/refs or upstream destinations fail closed. No credential or raw URL
is copied into publication records. Historical repository identity is retained.

Every result includes `artifact_publication.status` (`confirmed` or `incomplete`),
checkpoint SHA, destination, independently observed remote SHA, outcome and exact
retry API envelope. CLI exits 3 for an incomplete mutation/publication (JSON is
still returned); malformed/stale requests exit 2. `show` exits zero for inspection
and reports incompleteness explicitly. Only `workflow_complete=true` certifies
both terminal local records and confirmation of the exact current artifact tip.
A complete historical state by itself describes its old workflow, not evidence
that the new policy was applied retrospectively.

Forward mutations require reconciliation of the previous checkpoint. During an
outage the helper can still retain report/acknowledgement/liveness/budget facts
locally, but cannot dispatch work, allocate keys, authorize source publication or
finish while the prior tip is pending. Restore transport/authentication or remove
the rejecting remote hook, inspect `show`, then retry only the recorded operation:

```sh
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation publish-artifacts --payload /tmp/empty.json --expected-tip FULL_CURRENT_TIP --expected-revision CURRENT_REVISION
```

Here `/tmp/empty.json` contains `{}`. The corresponding API is
`Harness(repo).apply("publish-artifacts", {}, tip, revision)`. When active.json is
null, use revision 0. This retry creates no acknowledgement commit, attempt, PR or
allocation. Already confirmed tips are idempotent. An ancestor on the remote means
only ancestor coverage: the current checkpoint remains incomplete until exact-tip
confirmation. Command success, caller-provided SHA and an intervening remote
advance cannot certify the requested current tip.

For an observed remote descendant, the helper fetches into
`refs/harness/observed/<observed-sha>`, validates all ancestors (including permitted
legacy schema-1 records and an empty orphan bootstrap root), JSON/run/pointer and
allocation identities, publication policy, retained local records and append-only
evidence. Automatic adoption is deliberately conservative: existing run-state
bytes must be identical, additions may retain evidence and extend allocation
reservations/high-water. Any evolved run state requires explicit reconciliation
between writers; the helper does not decide whether changed budgets, worker facts,
acceptances or PR state are compatible. A safe descendant advances the local ref
under CAS, returns incomplete with `remote-advance-adopted`, and requires `show`
and a fresh invocation at that tip/revision. `show` never adopts an artifact tip.
Divergence, malformed history, dropped reservations or altered records retains both
tips and a fetch/compare/coordinate action. Never force push, silently discard a
writer's records, or automatically merge distributed state. Cross-machine key
allocation remains separately coordinated; local flock is not a distributed lock.

### Existing-history adoption and privacy

After implementation acceptance, invoke the helper from the candidate worktree,
in the same run. Before the first production push, inspect **all unique historical
record blobs**, including reports and representative log payloads, for small,
sanitized allowed harness records. Combine structural all-ancestor validation,
credential-pattern audit and semantic review. Pattern absence alone is not privacy
proof. Do not print secrets into review output. If disallowed content is found,
retain history locally and report its exact blocked scope for a decision; never
silently redact/rewrite, force push or publish that history.

Record a short sanitized review summary, then use:

```json
{"reviewed_tip":"FULL_REVIEWED_ARTIFACT_TIP","review_summary":"All unique historical records reviewed semantically; describe allowed contents and audit scope concisely."}
```

Call `apply("adopt-artifacts", payload, tip, active_revision)` or CLI action
`adopt-artifacts` with the usual payload/CAS flags. For null-active history, use
revision 0 and include `repository` containing `target_remote=origin` and the
actual `target_repository`. The adoption commit changes only the allocation
journal policy; original requirements, completed states, current worker/PR facts,
reports, budgets and ancestry remain intact. No old publication is fabricated and
no deadline is extended. Schema-1 bootstrap import additionally requires the same
`reviewed_tip`/`review_summary` when it binds publication in its import transaction.
A pending policy-less tip blocks normal forward operations until explicit adoption.

### Finite terminal ordering

Source PR lookup/create-once/attachment and any required CI follow their existing
sequence. The last local artifact transaction already contains terminal run state
and `active.json=null` atomically; the helper then publishes and verifies that
exact commit. Failure returns `workflow_complete=false`, even though the local
historical state says complete or the PR is attached. Restart discovers the global
journal policy and pending tip despite the null pointer. A following initialize
cannot conceal it. `publish-artifacts` reconciles that same terminal checkpoint
without rerunning accepted work or duplicating a PR. Read-only status and retry
produce no self-acknowledgement commit, so publication terminates finitely.
