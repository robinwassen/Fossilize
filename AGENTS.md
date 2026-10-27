# Agent guidance

Before working on this project, read:

1. [Local documentation index](local-docs/README.md).
2. [Project descriptor](local-docs/project-descriptor.md) for the project’s scope and components.
3. [Desired project properties](local-docs/project-pillars.md) for design and review principles.

Read specifications under `local-docs/specs/` when relevant to the task.

Use these documents to guide implementation, investigation and review. The pillars are desired properties; distinguish them from verified current behavior. Preserve the distinction between supported, unsupported, failed and incomplete work when discussing results.

`AGENTS.md` and `local-docs/` are shared project guidance tracked in Git. Keep them available to collaborators and agent worktrees; do not exclude them through local or shared ignore rules. Keep generated build outputs, temporary run artifacts and private machine configuration separate from this documentation.

## Backlog item keys

- Every item has one permanent, project-wide unique `FOS-` key with at least six digits; list numbers only indicate order.
- Before adding items, register keys in the [registry](local-docs/backlog-keys.md), increasing beyond the highest ever issued. Check all backlogs, including archives, for collisions and coordinate concurrent allocations.
- Never reuse or change keys. Keep registry entries forever; update locations on moves and retire removed items with their last location/replacement. Splits get new keys; retain the parent key or retire it with replacement links. Merges retain one key or get a new one; retire superseded keys.
- Use keys in dependencies, implementation references, acceptance evidence and status updates. On backlog edits, verify one canonical definition per key, registered allocations and valid cross-file dependencies.


## Verification

Before planning or running verification, read the [testing and verification guide](local-docs/verification.md). Select checks appropriate to the change and report what was actually verified, including limitations.
