---
name: implement-change-harness
description: Implement one identified repository change with fresh planner, implementer and independent checker agents, durable Git checkpoints, bounded repair, and one verified pull request. Use when this finite three-role workflow is requested; it does not select a backlog item or schedule future work.
---

# Implement change harness

Take one explicitly identified root requirement through planning, implementation,
independent checking and one ready-for-review PR. Stop after recording and attaching
that PR; do not merge it or start another task. Preserve the original acceptance.

The parent is the coordinator and sole canonical state writer. Use the helper in
[scripts/harness_state.py](scripts/harness_state.py) for state transitions and Git
checkpoints. It is a local POSIX tool, not a worker runtime or security boundary.
Read [references/state.md](references/state.md) before initialization or recovery.
Read [references/roles.md](references/roles.md) when constructing a role prompt or
processing its report. Read applicable AGENTS.md and its required documentation;
all roles read the project's verification guide before selecting/running checks.

1. Inspect active state under the common Git directory lock. Resume the existing
   root; never start a second active run. Preserve the user's checkout and partial
   work. Verify an existing artifact branch before adopting it. Explicitly import
   bootstrap records; never treat an incomplete old schema as current acceptance.
2. Record immutable original requirements, repository/destination/base, scope,
   budgets, command deadlines and CI policy. Default here is origin,
   robinwassen/Fossilize, master; artifacts remain local-only. No mandatory hosted
   CI is declared for a skill-only change unless project policy says otherwise.
3. Dispatch a fresh planner with `collaboration.spawn_agent`, `fork_turns="none"`.
   Check the actual model catalog, persist dispatch intent, launch once, then
   checkpoint the returned worker identity. Use fresh attempts for repairs, too.
   If launch/acknowledgement is uncertain, use `collaboration.list_agents` and
   `collaboration.interrupt_agent`; require confirmed termination before replacing.
4. Validate the planner's versioned implementation and test plan, then dispatch
   one fresh implementer for the first ready work unit. An unsplit root is the
   unit itself, with no second canonical definition. For a split, durably reserve
   permanent keys, register them under project rules, then define the subtasks.
5. At the committed candidate, dispatch a fresh checker in a new detached
   worktree. Require actual required-gate execution and planned-test mappings.
   Route incorrect/missing planned tests to implementation, missing test design
   to the planner, and unavailable mandatory prerequisites to a decision. Only
   the checker accepts. The coordinator never overrules a failed checker.
6. After every unit accepts, dispatch a separate fresh final checker over the
   combined branch. Changed source, plan or target base requires renewed checks.
   Enforce repair/replan/deadline bounds; stop with a precise resumption action.
7. Fetch origin/master and compare the accepted base. Persist publication intent,
   write the PR body with the required [section headings](references/state.md#pr-description),
   push the implementation branch without force, verify its remote SHA, then look
   up PRs across all states by exact repository/base/head. Persist creation dispatch
   before a create call. After success or uncertainty, look up again; never blindly
   retry create. Closed/merged/multiple matches need a decision. Record the open
   PR and call `attach_artifact`; retry only attachment when it fails.

All planner/implementer/checker attempts use fresh bounded context and no nested
delegation. Use subagents, not new user-owned chats. Record requested and actual
model/effort, including any explicit supported fallback; do not change global
configuration. Default planner/checker: gpt-6.1-sol/high; implementer:
gpt-6.1-sol/medium, raised with a recorded reason for difficult correctness work.
Different models are optional; independent evidence is mandatory.

Keep reports and small sanitized logs on the orphan `harness-artifacts` branch;
keep skill/source/tests on the implementation branch. Never merge/push artifact
history as part of the source PR. Report passed, failed, unsupported, incomplete
and not-run distinctly. JSON completeness cannot establish semantic test quality:
the actual checker reads assertions, runs counterexamples and checks the original
requirements. This skill cannot authenticate a hostile coordinator or recover
across machines without separately coordinated artifact/key retention.
