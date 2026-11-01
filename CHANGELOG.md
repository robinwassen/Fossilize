# Changelog

This records changes in this fork; GitHub Issues remain the authority for tasks
and their status. Follow the [changelog writing guideline](local-docs/changelog-guidelines.md).

## Unreleased

Entries are reviewed with their implementation PR; inclusion in an open PR does
not establish merged delivery or a release.

- **[Observability]** Recorded distinct replay timeout causes before recovery and retained termination results so interrupted work can be diagnosed without mistaking signals for causes. ([#16](https://github.com/robinwassen/Fossilize/issues/16))

- **[Observability]** Versioned replay hints by creation policy and device mode and exposed rejected writes so legacy or unattempted work cannot silently suppress replay. ([#15](https://github.com/robinwassen/Fossilize/issues/15))

- **[Observability]** Retained replay item failures across threads and child processes so drained queues no longer report failed required work as successful. ([#14](https://github.com/robinwassen/Fossilize/issues/14))

- **[Harness]** Added a shared changelog and required entry review so contributors can see what changed and why it matters. ([#61](https://github.com/robinwassen/Fossilize/issues/61))

- **[Worker lifecycle]** Made Linux replay children terminate on signal mask or stdin/stdout setup failure so failed workers cannot execute controller code. ([#56](https://github.com/robinwassen/Fossilize/issues/56))

## Completed changes

These backfilled entries are grouped by topic, with earlier changes first within
each topic; no release version or release date is implied.

- **[Observability]** Improved Vulkan and cache error logs to preserve error codes and affected resources so failures are easier to diagnose. ([#10](https://github.com/robinwassen/Fossilize/issues/10))
- **[Observability]** Fixed list, rehash and prune commands to report failure when resource parsing fails so incomplete processing cannot appear successful. ([#11](https://github.com/robinwassen/Fossilize/issues/11))
- **[Observability]** Fixed file write helpers to report write, flush and close errors so callers can detect incomplete output. ([#12](https://github.com/robinwassen/Fossilize/issues/12))
- **[Harness]** Added a resumable change workflow with independent checks so interrupted implementation work can continue with its progress and review evidence preserved. ([#34](https://github.com/robinwassen/Fossilize/issues/34))
- **[Harness]** Included verified backlog completion in implementation PRs so merged changes update task status without a separate follow-up. ([#35](https://github.com/robinwassen/Fossilize/issues/35))
- **[Harness]** Added remote publication of change workflow records so plans, reports and recovery checkpoints remain available beyond the local checkout. ([#36](https://github.com/robinwassen/Fossilize/issues/36))
- **[Harness]** Moved task tracking to GitHub Issues so contributors can find requirements, progress and blocking dependencies in one place. ([#37](https://github.com/robinwassen/Fossilize/issues/37))
- **[Harness]** Added one-command delivery after explicit approval so merging, branch cleanup and task closeout can resume after interruptions. ([#39](https://github.com/robinwassen/Fossilize/issues/39))

The entry for [#35](https://github.com/robinwassen/Fossilize/issues/35) records
historical backlog behavior, subsequently replaced by GitHub issue tracking in
[#37](https://github.com/robinwassen/Fossilize/issues/37); the old backlog mechanism
is not current contributor guidance.
