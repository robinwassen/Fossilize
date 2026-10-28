# State, issue authority and publication

## Invocation and ownership

Python3 standard library, Git, gh and POSIX flock are required. Run the helper
from the implementation worktree, or pass --repo. Every mutation uses a JSON
payload, exact artifact tip and expected state revision from the previous result.
The common Git directory's harness-run.lock coordinates every local worktree;
never delete the lock or bypass a live owner. Worker liveness is separate.

```sh
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation show
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation initialize --payload /tmp/init.json --expected-tip none --expected-revision 0
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation dispatch --payload /tmp/dispatch.json --expected-tip FULL_ARTIFACT_SHA --expected-revision 1
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation detached-worktree /tmp/new-checker FULL_CANDIDATE_SHA
```

Harness(repo).apply(action,payload,expected_tip,expected_revision) is equivalent.
show reads committed records and never adopts dirty inspection files. A stale
CAS/revision fails without changing the ref. detached_worktree creates a new path
only; it never resets an existing worktree. The coordinator is the sole canonical
state writer. A worker needs explicit permission for the restricted records-only
interface, which appends active-run evidence and byte-preserves every state.json,
active.json, allocations.json and prior report/evidence record.

## Initialization and task source

An initialization payload uses root_issue (full URL or numeric issue number),
run_id, repository and optional limits/ci_policy:

```json
{
  "root_issue": "https://github.com/robinwassen/Fossilize/issues/NUMBER",
  "run_id": "unique-run-token",
  "repository": {
    "input_sha": "FULL_INPUT_SHA",
    "base_sha": "FULL_TARGET_MASTER_SHA",
    "target_remote": "origin",
    "target_repository": "robinwassen/Fossilize",
    "target_branch": "master",
    "branch": "codex/issue-N-description",
    "implementation_worktree": "/absolute/implementation"
  },
  "limits": {"repairs": 3, "replans": 1, "command_timeout": 600},
  "ci_policy": "local"
}
```

Replace NUMBER/N and SHAs/paths. Numeric identity always resolves in the explicit
fork. gh resolves repository/server/node identity and rejects PR records. New-run
original acceptance is AC1 containing the exact root issue body; a supplied
original must match. Migrated runs retain their existing original list exactly.
Current requirements never come from Markdown. Authentication/API/network errors
fail closed, with no stale-task fallback.

The immutable issue snapshot includes every deliverable's title/body, full
identity, state/reason, parent, children and recursive blocked-by graph. Reads
paginate; missing nodes/cycles fail. Internal deliverables may remain open while
accepted units coordinate their dependency order. External prerequisites require
closed/completed; not-planned/cancelled is unsatisfied. Parent membership creates
no implicit edge. Titles are conservatively material; labels, timestamps and
ordinary comments are not readiness authority.

Re-read material snapshots before checker dispatch and each publication boundary.
reconcile-input takes exact live snapshot plus concrete observation, preserves the
old snapshot as provenance, clears approvals and requires renewed work/checks.
Never silently adopt changed requirements. The same active root/run resumes;
another root/run or reused completed storage identity is rejected.

## Schema3 and artifact records

Live state uses schema_version=3, immutable task_key/run_id storage identity,
root_issue, issue_snapshot, exact original acceptance, repository identity,
revision, phase/operation/status/next_action, plan, work_units, active_work_unit,
attempts, verification, findings, limits, closeout, publishing, final_acceptance
and provenance. A new storage token is issue-N. A migrated legacy token stays
unchanged; it is a storage path, never future task allocation authority.

Phases are planning, implementing, checking, closing, publishing and complete
(plus initializing); status is distinct. Artifact records are active.json,
allocations.json and runs/<storage>/<run>/state.json, reports/<name>.json or
evidence/<name>.json|txt|log. allocations.json preserves historical permanent
legacy allocation/publication facts; current workflows allocate no FOS keys.
Reports/evidence are append-only, regular bounded UTF-8 records, at most1MiB each.
Traversal, arbitrary source paths, symlinks, binary/NUL content and malformed JSON
are rejected. Every ancestor is validated. Immutable blob/tree caching and batched
Git object/index operations preserve those per-object checks.

A private index plus hash-object/write-tree/commit-tree/update-ref expected-old CAS
creates records checkpoints on the separate orphan harness-artifacts ref. Failed
writes/trees/commits/CAS retain the old authoritative checkpoint. Unreachable
objects may remain until ordinary Git garbage collection. A separate artifact
worktree is only an inspection view; ref updates never reset its files/index.
Preserve dirty/untracked contents, using a fresh detached view when necessary.
Never merge or push artifact history as source changes.

## Plans, work units and reports

A plan contains version, exact original, scope, non_goals, implementation, cases,
gates and work_units. cases have unique id, assertions, location and optional
work_units selecting applicable issue identities. Gates have unique id, kind
(work_unit/final/both/publication/optional), criteria IDs, method, cwd,
prerequisites and expected assertions. Required gates cover every original
criterion; semantic adequacy remains the independent checker's responsibility.

work_units=[] means one execution unit references the root issue. Split rows
contain key/issue (the same full child URL), title, definition, dependencies and
criteria. Every child must have the native root parent, unique identity and native
deliverable prerequisite mapping; together their criteria cover the root. The
helper selects a dependency-ready unit independent of array order. New issues
and native edges are created through journaled external operations before plan
recording; small steps alone do not create issues.

dispatch takes role, unused attempt_id, requested/actual model+effort, observed
catalog and fallback_reason where needed. Record intent before launch, then
acknowledge the returned worker_id. Unknown/live workers fence replacement;
reconcile needs attempt_id, liveness (live/unknown/terminated), concrete observation
and next_action. Confirm termination before a fresh replacement attempt.

Reports carry the exact tuple in roles.md. Planner reports include the versioned
plan. Implementer reports include committed result_candidate_sha, nonempty
 test_mapping and actual commands. Checker reports include verdict, findings,
evidence, discovered/executed nonzero unique IDs, empty mandatory skipped and
planned-case mappings to executed tests. Each required gate evidence entry has
gate, result=passed, exit_code=0, command/cwd/environment/log/criteria and exact
candidate_sha/base_sha/plan_version/attempt_id. Its log must be an existing small
artifact record. Records may be supplied with the action; canonical state cannot
be overwritten through that interface. Reports themselves are committed unchanged.

Findings have unique IDs, status/evidence/route and permanent affected_task issue
scope. Missing legacy attribution remains root-wide. Unit acceptance blocks on
its own, transitive prerequisite and root findings; unrelated later-unit findings
remain for their owner. Unknown/orphaned scope is conservative. Replans preserve
prior attribution/provenance; no resolution narrows scope. resolve-finding requires
concrete recheck evidence during a fresh active checker. Final acceptance and
publication require every finding resolved and every work unit accepted.

## Bounds and changes

Default bounds are initial implementation plus3 repairs per unit, one replan and
four hours from initialization; command timeout is600 seconds. command-intent
records command/cwd/timeout before a real external process deadline. The helper
does not execute shell text, launch workers or run a timeout watcher. Record actual
commands and outcomes; no build/empty suite substitutes for assertion-bearing tests.

Budget exhaustion pauses forward work and preserves the exact operation/action.
Late reports and worker observations remain facts, then require a budget decision;
they never authorize late publication. budget-extension needs explicit human
authorization/reason and increased limits with an unexpired deadline. Migration
never resets the deadline or counts. Repeated unchanged rejection also stops;
new_evidence distinguishes a new probe. Unsupported required checks remain open.

observe-source requires a clean matching branch/worktree at committed candidate
(and optional base), clears approvals and invalidates stale issue readiness.
Changed HEAD, dirty/untracked files, plan/base/input drift or checker rejection
cannot publish against old acceptance. A final implementation rejection clears
all unit approvals and returns to dependency-ready implementation, followed by
renewed unit and separate final checking.

## Issue closeout and one PR

After all units independently accept, closeout-intent takes targets containing
issue, concise passing evidence and the exact source compare URL:
https://github.com/robinwassen/Fossilize/compare/<base>...<branch>.
Targets are the root and required unit issues exactly once. Open issue state,
current acceptance and no incomplete uncovered child are required. If earlier
units accepted a different candidate, the action first schedules checker-only
renewal of every unit at the combined candidate, with no implementation-budget
consumption; repeat closeout-intent after those checks. The helper
prepares eligible closing_references, an exact candidate/base/plan/input binding
and a readiness generation identified by the committed intent revision. Its marker
binds that generation as well as the tuple. Replaying closeout-intent with the
exact saved targets retains the same generation after interruption; changed
in-flight evidence requires explicit supersession. Renewed verification creates
a new generation even at an unchanged candidate. Journal one
managed readiness comment per target with that binding/marker/evidence/reference.
Derive its journal operation key from the saved marker and target issue identity,
so restart replays the same operation and later generations have distinct keys; then
closeout-record with exact candidate_sha reads comments and prepares separate
final checking. It creates no source bookkeeping commit or premature issue closure.

After rejection/input/source changes, journal a marked superseding readiness
comment containing "superseded" on each target, then closeout-cleanup with concrete
observation. Cleanup restores the saved phase/operation, including planner/replan
routing for missing-design rejection. Unrelated human comments remain untouched.
Renew affected checks;
accepted work units, PR readiness, run completion and merged delivery are distinct.

PR descriptions use these explicit level-two headings: Problem; What changes;
Why this matters; Why this approach; Verification and limits; Task references.
Write for a reviewer without conversation history. Include eligible full-URL
Closes references only for fully satisfied tasks; incomplete epics stay open.

After separate final acceptance, fetch target master and compare accepted base.
publish-intent requires exact target_remote/target_repository/target_branch/branch,
clean matching candidate, unchanged live inputs, resolved findings and exact final
candidate/base/plan. Then use a normal non-force source push and publish-observation:

1. event=push with remote_sha: helper independently verifies remote head.
2. event=lookup with all_states=true, unambiguous=true, matches=[] or one exact
   open non-draft PR object (target_repository, target_branch, branch, state,
   draft, url). Closed/merged/multiple/wrong PRs require a decision. The helper
   also reads the actual gh PR head/base/state and closingIssuesReferences;
   it must close exactly the fully verified targets.
3. Exact empty lookup permits event=create-dispatched before the one create call.
   Always look up again after success or response loss. Never blindly recreate.
4. A matching PR enters issue-evidence. Journal one actual-PR comment per target
   containing PR URL, exact candidate and marker
   <!-- harness-pr:<run_id>:<candidate_sha> -->. Record event=issue-evidence;
   the helper paginates/readbacks those exact facts before attach.
5. Call attach_artifact, then event=attachment with matching url and success.
   Failure leaves attachment only pending. Predeclared required_after_pr CI needs
   event=ci, result=passed and evidence before completion. Completion records an
   authoritative successful required-check baseline for later approved delivery.

Each observation rechecks accepted source/base/head and live task input. Ref races
remain observable facts, not a distributed transaction. Reconcile the same PR
identity rather than creating another. After attachment/required CI, terminal
state and active.json=null commit atomically, then exact artifact publication
must confirm before workflow_complete=true.

## Approved delivery after PR publication

After exact PR-ready terminal publication, present the attached PR URL and concise
independent verification/limits. Request approval to squash-merge and wrap up once;
honor explicit approval already given for the same repository, PR and accepted
head. Decline/defer leaves the PR open. PR creation is never approval. A flag is
an assertion of actual human approval, not authority inferred from issue text.

The coordinator invokes one command (replace all placeholders with recorded facts):

```sh
python3 -B .agents/skills/implement-change-harness/scripts/harness_state.py --repo /absolute/implementation wrap-up --approve --root-issue https://github.com/robinwassen/Fossilize/issues/NUMBER --run-id RUN --approved-repository robinwassen/Fossilize --approved-pr https://github.com/robinwassen/Fossilize/pull/PR --approved-head FULL_ACCEPTED_SHA --accepted-revision TERMINAL_REVISION
```

`Harness(repo).wrap_up(approval)` provides the same supported transition. Selection
uses the exact completed root/run, even with active.json=null. Accepted revision
is the original PR-ready terminal revision; resumption retains it while delivery
revisions increase. A different active run blocks delivery before mutation.
The same command remains runnable from the retained implementation worktree,
which may detach safely at the accepted candidate; no worktree is removed.

The command holds the existing common-directory lock and uses the same artifact
CAS, bound push endpoint and integrity checks. It validates independent unit/final
reports and mandatory gates, attachment, eligible closeout targets, current material
issue graph, exact actual PR repositories/head/base, required successful checks and
merge conditions. Historical PR-ready runs without a required-check baseline need
review and renewed readiness through the supported publication workflow; no baseline
or acceptance is invented. Changed requirements/head/base/checks require a decision.

Every intent, dispatch, returned identity and readback has a durable checkpoint
with exact-tip publication before progression. Restart the same command after
interruption or response loss; authoritative reads reconcile merged PRs, absent
branches, completed tickets and marked comments before any retry. An uncertain
merge/comment is never blindly redispatched. All Git/gh subprocesses have finite
deadlines (at most 60 seconds each; artifact transport has its own bounded deadline)
and the command checks a 600-second overall progression deadline.

After squash merge, independently fetch the intended target from the bound push
endpoint and require reachability, exactly one accepted-base parent, a distinct
merge identity and the accepted candidate tree. Keep the real merge SHA separately
from original candidate and all original acceptance. Conditional remote deletion
uses expected-SHA lease; local update-ref deletion also requires the accepted SHA.
Dirty/changed/unexpectedly occupied source worktrees require a decision. Unrelated
worktrees, user edits, artifact inspection files and unfinished parents survive.
Only independently verified closeout targets may close as completed; cancelled
issues require a decision. Paginated marked comment readback proves exactly one
concise delivery comment per target with PR/candidate/merge/verification limits.
Expected completed auto-closure does not invalidate unchanged material requirements.

The supported terminal transition changes only delivery/PR bookkeeping, next action
and monotonic revision plus new append-only delivery evidence. It preserves plans,
acceptance, reports, history, allocations and original budgets. The active pointer
stays null. Delivery records the original accepted candidate separately from merge.

CLI prints one compact JSON object including `summary`, scope, real identities,
checkpoint and `resume_command`: `delivered` (exit0), `incomplete` (exit3, resume the
same command), or `decision-needed` (exit2, specific reason). A local delivered
record without exact confirmed orphan publication still returns incomplete.
No ad-hoc scripts, direct state surgery or agent-managed mechanical steps are needed.

## Artifact publication and recovery

Every mutation publishes its authoritative checkpoint separately to the orphan
origin harness-artifacts ref, then independently reads the bound push endpoint.
A successful push alone does not prove exact current-tip coverage. The journal's
artifact_publication policy binds version, origin, declared repository/ref, SHA256
of the single actual push endpoint and sanitized semantic privacy-review identity.
Hosted endpoint paths must match the declared repository. Multiple push URLs,
endpoint changes, wrong repositories/refs and upstream destinations fail closed;
credentials and raw endpoints never enter records. Different fetch/push URLs are
handled by using the bound push endpoint consistently.

Every result carries artifact_publication.status=confirmed/incomplete, checkpoint,
observed remote SHA, outcome and precise retry API envelope. CLI exit3 indicates
incomplete publication (JSON still returns); invalid/stale input exits2. show is
read-only and reports pending status. Only workflow_complete=true certifies both
terminal local state and exact confirmed remote tip. Pending terminal artifacts
still block another initialization despite the null active pointer.

Use publish-artifacts with {}, exact current tip and active revision (0 if null)
to verify/retry that checkpoint. It creates no self-acknowledgement commit, worker,
PR or allocation. Forward work pauses behind pending artifacts; safety facts may
be retained locally. An observed remote ancestor covers only that ancestor.

A remote descendant is fetched to refs/harness/observed/<sha>, every ancestor and
record/policy/identity is validated, and durable local facts must survive. Automatic
adoption permits only identical existing state plus safe append-only evidence and
retained legacy allocation facts. Evolved worker/budget/approval/source state needs
explicit coordination; divergence/malformed/dropped records retain both tips and
precise reconciliation. Adoption returns incomplete and requires show/new CAS.
Never force-push, retarget, overwrite or silently merge competing evidence.

For policy-less existing history, semantically review all unique historical record
blobs, supported by structural and credential-pattern auditing. Pattern absence
alone is not privacy proof. adopt-artifacts needs reviewed_tip and review_summary;
null-active adoption also needs explicit repository identity. It changes only the
publication policy, not old run/report bytes, original criteria, budgets or ancestry.
Disallowed history remains local for a decision; never silently rewrite/redact it.

## Explicit saved-state and self-migration

Ordinary operations reject schema1/2 active state. import-bootstrap explicitly
imports schema1 using its original identity/acceptance, normalized historical plan,
known worker/report facts and oldest-record start time; no old acceptance is
fabricated. The private legacy compatibility engine has no CLI or normal task
mutation authority. Historical fixtures alone opt into its old API.

migrate-state takes legacy_root, run_id, exact original, complete legacy-to-issue
mapping and exact live snapshot. It migrates schema2 to3 under the same lock/CAS,
retaining storage/run paths, all ancestor commits, completed-state/report/evidence
bytes, permanent allocations, original deadline/budgets, findings and provenance.
Committed historical source/registry union is imported as evidence only. Live plan
fields/operations/work-unit identities normalize without changing semantic plan
version; original acceptance and historical reports remain verbatim.

Idle migration clears approvals. Active migration permits only one acknowledged
implementer, with its exact original tuple supplied as bridge. The helper stores
that envelope verbatim and accepts its original report once, unchanged, translating
only live execution state. Modified/replayed/other old envelopes fail. No checker
approval carries forward. Any pending schema1 worker uncertainty must be resolved
with concrete termination facts before forward work.

For self-application, commit and test candidateA while definitions remain; complete
live issue/content/native-graph readback; pause and give the coordinator candidate,
root mapping, snapshot and exact action. Only the coordinator invokes candidate
migrate-state on the existing run and confirms publication/bridge. Then resume
that same implementer, remove source task authority, update guidance/package, run
checks and commit candidateB. Return the original bridge envelope. Do not start a
new run, fake a repair, replace the worker or delete definitions before confirmation.

After fetching source plus orphan artifacts into a fresh clone, rebind-execution
requires exact root_issue/run_id/old_common_dir and candidate_sha. It validates the
same logical repository/destination/run/task while explicitly updating local
common-dir/worktree location, retaining prior location as provenance. Ordinary
show/mutations never silently disable execution identity checks.
