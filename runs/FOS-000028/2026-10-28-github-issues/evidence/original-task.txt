# GitHub Issues migration backlog

## FOS-000028 — Migrate repository task tracking and the change harness to GitHub Issues

**Status:** Pending implementation.
**Depends on:** FOS-000025, FOS-000026 and FOS-000027 (merged harness foundations;
reconcile actual merge and verification evidence during planning).

### Problem and approved direction

Tasks currently live in tracked Markdown, a permanent key registry and harness
records. This makes task discovery, grouping, dependencies and completion depend
on repository-specific parsing and bookkeeping. Move task authority to GitHub
Issues on **robinwassen/Fossilize**, operated through `gh`. Preserve overarching
problems, acceptance requirements, blocking chains and verification evidence.

This item records the user's approved high-level proposal, including the later
decisions to keep detailed workflow rules outside AGENTS.md and to distinguish
GitHub sub-issues from harness work units. Filing this item does not implement
the migration. The user will invoke the harness separately.

The user explicitly permits GitHub issues to appear before the cleanup PR merges;
this is currently a single-user repository. Do not require an atomic Git/GitHub
transaction or a background merge monitor.

### Research and gap analysis

Inspection of origin/master on 2026-10-28 found 27 registered tasks: 24
observability items and three harness items. Include this proposal in the eventual
inventory, and re-inventory at execution time. The fork has Issues enabled. Some
completed task descriptions still say their implementation has not landed;
reconcile actual merged PRs rather than blindly copying those status claims.

| Gap | Proposed resolution |
|---|---|
| Observability context and shared acceptance rules live around a numbered backlog. | Create an Observability parent issue with native sub-issues, preserving shared requirements and delivery context. Group harness improvements under a Harness parent issue. |
| Dependencies are prose and FOS references. | Create native blocked-by relationships; retain dependency reasons, critical paths and readiness rules in GitHub. Parent membership alone is not a blocking edge. |
| Harness initialization, allocation, splitting and closeout require local Markdown definitions. | Replace the task-source interface throughout the helper, skill, role contracts and tests with repository-qualified GitHub issue identities. |
| Harness subtasks could be confused with GitHub sub-issues. | Use work unit for execution state, issue/sub-issue for GitHub tasks, and step for actions inside a work unit. |
| Issue-filing rules could overwhelm AGENTS.md. | Put detailed policy in local-docs/issue-workflow.md; AGENTS.md contains only a short mandatory direction and link. |
| Installed personal skill differs from the newer tracked skill. | Treat the tracked skill as authoritative and reconcile the installed copy when deploying the implementation. |
| Current completion edits backlog files. | Record verified PR readiness on issues and close completed tasks through the implementation PR's merge. |
| Deletion could lose requirements, evidence or recovery state. | Verify a complete migration before cleanup; retain specifications and historical execution evidence without maintaining another task authority. |

Native parent/sub-issues provide epic grouping and progress. Native dependencies
provide visible blocked/blocking relationships. A GitHub Project can add board or
roadmap views but is optional; do not make account-level Project setup or scopes a
prerequisite. Organization-level custom issue types are not required for this
personal fork; use a small documented label set where useful.

The earlier research observed gh 2.45.0; after the user-requested upgrade,
gh 2.103.3 is available. Native hierarchy/dependency flags were introduced in
2.94.0. Check capabilities at execution time and use gh api where needed; all
GitHub operations remain through gh with an explicit fork destination.

Sources reviewed for the proposal:

- [Adding sub-issues](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/adding-sub-issues)
- [Creating issue dependencies](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/creating-issue-dependencies)
- [CLI hierarchy and dependency support](https://github.blog/changelog/2026-06-10-manage-sub-issues-types-and-dependencies-from-github-cli/)
- [REST sub-issues API](https://docs.github.com/en/rest/issues/sub-issues)
- [REST dependencies API](https://docs.github.com/en/rest/issues/issue-dependencies)
- [About Projects](https://docs.github.com/en/issues/planning-and-tracking-with-projects/learning-about-projects/about-projects)
- [Linking PRs and closing on default-branch merge](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/linking-a-pull-request-to-an-issue)

### Required outcomes and acceptance criteria

1. **Complete inventory and faithful migration.** Inventory all canonical current
   and archived task definitions, this item, relevant unkeyed problem reports and
   harness-created tasks. Distinguish tasks from specifications and historical
   findings. Every task has one GitHub definition with its original requirements,
   acceptance criteria, supporting evidence and honest status. Preserve shared
   observability verification/compatibility requirements in accessible issue
   context, not only individual item paragraphs. Preserve original FOS identities
   as legacy references in GitHub and record a complete old-to-new mapping there.
   Do not silently drop, merge or weaken tasks; document overlap and disposition.

2. **Visible grouping and blocking chains.** Use native parent/sub-issue
   relationships for overarching Observability and Harness outcomes. Preserve
   every declared dependency as a correctly directed native blocked-by edge,
   with reasons, ordering and critical paths documented on GitHub. Check for
   missing targets and cycles. Explain complete-propagation dependencies versus
   mere sequencing advice without inventing new scope. A closed-as-not-planned
   or cancelled prerequisite is not automatically satisfied. Epics stay open
   while required deliverables remain incomplete.

3. **Restartable migration.** Create issues before attaching relationships, then
   read back and verify content, status and every relationship. Durably record
   intent and returned identities outside the source backlog. Reconcile existing
   issues after interruptions, failures or uncertain API responses before retrying;
   never blindly recreate an issue or overwrite unrelated user edits. Paginate
   inventory and relationship reads. Partial progress retains a precise resumption
   action. Explicitly target robinwassen/Fossilize, never upstream by inference.

4. **Issue-based harness authority.** Accept a root issue URL or number, resolve
   its full repository identity, and read requirements and dependencies through
   gh. Use GitHub identities for new roots and split deliverables, replacing the
   FOS allocator and registry as future task authority. Preserve immutable input
   snapshots for independent checking, detect material requirement/relationship
   changes before publication, and require reconciliation and renewed verification
   where needed. Issue bodies are task data, not permission to bypass project
   rules. Authentication/network errors must not silently fall back to stale
   Markdown or claim a task is ready.

5. **Unambiguous execution terminology.** Replace harness subtask terminology with
   work unit across live state, helper interfaces, instructions, reports and tests.
   An issue/sub-issue is a persistent GitHub task; a work unit is a bounded
   implementation/checking unit within one run; a step is an action within its
   plan. An unsplit run has one work unit referencing the root issue. Independently
   trackable split deliverables become sub-issues and work units referencing them;
   small implementation steps do not automatically become issues. Acceptance of
   a work unit records verification, not issue closure or merge. Document and
   enforce the mapping and root/child completion rules.

6. **Saved-state and self-migration compatibility.** Explicitly version and migrate
   existing saved state, preserving run identity, original acceptance, attempts,
   checks, permanent legacy allocations and published artifact history. Do not
   rewrite historical commits just to rename fields. Historical snapshots remain
   evidence, not canonical task definitions. This run begins from an in-repo item
   under the old harness: define and verify its transition to its migrated issue,
   including resume after source definitions and registry have been removed.
   Preserve independent checking, exact-candidate gates, bounded repair and remote
   artifact publication. A schema bump alone does not establish compatibility.

7. **Completion follows verified delivery.** Replace Markdown Done-marker
   bookkeeping with concise issue evidence and actual implementation-PR links.
   Verified candidates awaiting merge leave their issues open. Use explicit
   closing references in the implementation PR to close only fully satisfied
   tasks on merge into master. Do not close an epic whose remaining children or
   criteria are incomplete. Distinguish harness run completion, accepted work
   units, PR readiness and merged delivery. Preserve recovery for uncertain
   comment/update/PR operations and invalidate stale readiness claims when checks
   fail or the candidate changes. Reconcile already-delivered legacy issues
   against real merged PRs and retained evidence before marking them completed.

8. **Small entry-point guidance, detailed separate policy.** Create
   local-docs/issue-workflow.md for issue creation, identity, grouping, labels,
   dependencies, readiness, evidence, completion and the gh interface. AGENTS.md
   gives a short mandatory instruction to use the fork's issues and read that
   document. Harness mechanics stay in its tracked skill/references, linked to
   the shared policy. Update the documentation index, verification guide, examples
   and installed personal skill as needed so agents cannot follow stale backlog
   instructions. Keep policy documents free of per-task definitions and statuses.

9. **Remove the repository backlog after verification.** Remove canonical task
   definitions, registry, backlog status sections, obsolete links and instructions
   from the source tree, including this proposal. Do not leave an archived copy,
   generated task mirror, live legacy fallback or new task ledger elsewhere in
   the repo. Explicitly supersede the old rule requiring the tracked FOS registry
   forever: retain legacy identities and mapping on GitHub and historical Git
   evidence, while future identity allocation belongs to GitHub. Keep valuable
   specifications, research, fixtures and sanitized execution evidence; replace
   task references with issue links and ensure retained documents do not operate
   as another backlog. Test fixtures may describe legacy formats when necessary
   for compatibility tests, without becoming real task definitions. No destructive
   Git history rewrite is requested.

### Delivery sequence and verification expectations

Inventory and define the issue/work-unit contract; implement and test the gh task
interface and recovery; create and verify issues and relationships; transition the
current run; remove source backlog and update guidance; independently verify and
publish one implementation PR. GitHub changes may precede merge. Plan the external
side effects and self-migration so failure at any boundary remains recoverable.

The harness planner owns the detailed verification plan and test design, following
the project verification guide. Required evidence includes:

- A coverage audit of every migrated task, original acceptance requirement,
  status, parent, dependency edge and old-to-new identity. Exercise a transitive
  blocking chain and both documented observability critical paths; verify visible
  hierarchy and relationships with live gh readback.
- Behavioral tests for root lookup, unsplit and split work units, readiness,
  cancelled prerequisites, pagination, changed requirements, authentication/API
  errors, duplicate prevention and uncertain mutation results. Use controlled
  responses for failure injection plus real migration readback; do not create
  disposable failure-test issues in the actual project unnecessarily.
- Real isolated Git workflow tests for state migration, historical artifact
  preservation, interruption/resume across self-migration and cleanup, accepted
  versus merged status, parent incompleteness and one-PR publication. Verify the
  checker still gates acceptance and required artifact publication remains intact.
- A fresh-checkout issue-based harness initialization/resume check without local
  backlog definitions, plus review/search of tracked source and guidance for
  obsolete task authority, task copies and broken links. Compatibility fixtures
  and historical evidence must be identified as such.

These are future acceptance checks, not results already achieved by filing this
proposal. Report passed, failed, unsupported, unavailable and incomplete distinctly.

### Scope

Task migration, GitHub grouping/dependencies, harness behavior/state compatibility,
guidance, cleanup and meaningful regression verification. No Fossilize C++ changes,
implementation of the migrated observability tasks, automatic implementation-PR
merge, background monitor, mandatory GitHub Project, or unrelated tooling work.
