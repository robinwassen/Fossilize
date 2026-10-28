# Backlog key registry

This is the project-wide allocation ledger for backlog item keys. Keys are independent of file, order and implementation status. Never delete an issued key or assign it to another logical item. Retain retired entries with their last location and any replacement keys. References to an allocated key may occur in many files; its canonical definition belongs to one item.

Format: `FOS-` followed by at least six decimal digits. Allocate the next number above the highest ever issued, including retired keys; do not fill gaps. The highest number currently issued is **28**; the next allocation is **FOS-000029**. Update this allocation summary and append the registry row before creating a new item. Serialize concurrent allocations and recheck for collisions when combining changes.

“Allocated” below records key issuance, not implementation completion. The backlog owns implementation status and acceptance evidence. On moves, update the canonical location without changing the key. On removal, change the allocation state to “Retired” and preserve the row.

| Key | Item | Canonical location | Allocation state |
|---|---|---|---|
| `FOS-000001` | Preserve precise context in existing diagnostics | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000002` | Make utility parse failure affect the command result | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000003` | Check shared file-helper flush/close completion | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000004` | Retain unrecoverable child loss in the controller | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000005` | Separate replay-thread work completion from item success | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000006` | Stop replay-cache markers from certifying failed creation | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000007` | Record timeout origin before invoking crash recovery | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000008` | Reject malformed resource envelopes as controlled input errors | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000009` | Carry reason data from the place that detects it | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000010` | Explain unsupported features and blocked dependencies | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000011` | Expose discovery and selection completeness | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000012` | Reconcile final logical items across recovery attempts | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000013` | Publish one versioned replay result and derive exits from it | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000014` | Retain authoritative results independently of lossy progress events | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000015` | Report cancellation and bound orderly shutdown reporting | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000016` | Add database finalization that can report failure | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000017` | Expose recorder background completion and recording loss | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000018` | Expose typed library operation reasons beyond the recorder | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000019` | Publish capture completeness without changing application results | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000020` | Make library presentation optional and define callback safety | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000021` | Propagate required-output failures and preserve merge evidence | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000022` | Describe reuse evidence and compatibility honestly | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000023` | Migrate remaining CLI contracts and internal consumers | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000024` | Bound diagnostics without sampling away outcomes | [Observability backlog](reviews/observability-sweep.md) | Allocated |
| `FOS-000025` | Implement the change harness skill | [Harness proposal](specs/basic-change-harness-plan.md#fos-000025--implement-the-change-harness-skill) | Allocated |
| `FOS-000026` | Include backlog completion in the implementation PR | [Harness backlog](reviews/harness-backlog.md#fos-000026--include-backlog-completion-in-the-implementation-pr) | Allocated |
| `FOS-000027` | Commit and publish harness artifacts to the remote orphan branch | [Harness backlog](reviews/harness-backlog.md#fos-000027--commit-and-publish-harness-artifacts-to-the-remote-orphan-branch) | Allocated |
| `FOS-000028` | Migrate repository task tracking and the change harness to GitHub Issues | [Migration backlog](reviews/github-issues-migration.md#fos-000028--migrate-repository-task-tracking-and-the-change-harness-to-github-issues) | Allocated |
