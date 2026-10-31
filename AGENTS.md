# Agent guidance

Before working on this project, read:

1. [Local documentation index](local-docs/README.md).
2. [Project descriptor](local-docs/project-descriptor.md) for the project’s scope and components.
3. [Desired project properties](local-docs/project-pillars.md) for design and review principles.

Read specifications under `local-docs/specs/` when relevant to the task.

Use these documents to guide implementation, investigation and review. The pillars are desired properties; distinguish them from verified current behavior. Preserve the distinction between supported, unsupported, failed and incomplete work when discussing results.

`AGENTS.md` and `local-docs/` are shared project guidance tracked in Git. Keep them available to collaborators and agent worktrees; do not exclude them through local or shared ignore rules. Keep generated build outputs, temporary run artifacts and private machine configuration separate from this documentation.

## Task tracking

Use [robinwassen/Fossilize Issues](https://github.com/robinwassen/Fossilize/issues) as task authority. Before creating, implementing or reviewing a task, read the [issue workflow](local-docs/issue-workflow.md).

## Verification

Before planning or running verification, read the [testing and verification guide](local-docs/verification.md). Select checks appropriate to the change and report what was actually verified, including limitations.

## Changelog

Update `CHANGELOG.md` with each change, following the
[changelog writing guideline](local-docs/changelog-guidelines.md).
