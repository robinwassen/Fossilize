# Applicability review and evidence

27 October 2026. This is a scenario-based review of the proposed contracts, not a runtime test or an audit of current compliance. No implementation changes or GPU workloads were needed to test whether the principles give unambiguous answers. Existing logging helpers, error taxonomy and logging configuration were not reviewed; narrowly scoped worker/lifecycle code was inspected to establish architectural constraints. Some lifecycle functions contain diagnostics incidentally; these were not used as a design template.

## Project context and verified constraints

Read `AGENTS.md`, `README.md`, `local-docs/README.md`, `project-descriptor.md`, `project-pillars.md`, and `known-issues/steam-shader-replay-observability.md` in the repository checkout.

The project has three observability boundaries: an embeddable library, a Vulkan capture layer within another application's process, and command-line tools. The external-replayer library interface adds an embedding boundary around a separately running CLI. `.foz` descriptions are inputs to driver compilation, not proof of a populated or reusable driver cache.

Verified in the local checkout:

- Linux controller partitions graphics, compute and ray-tracing ranges across children: `cli/fossilize_replay_linux.hpp:1239`. Child creation uses `fork()` at line 529. Per-worker shutdown/restart is handled at lines 937–968; progress-based resumption is at lines 396–413. This allows one worker to fail/restart while others have independent outcomes.
- Windows similarly partitions ranges across child processes at `cli/fossilize_replay_windows.hpp:885`; process creation uses `CreateProcessA` at line 625. The controller sets one replay worker thread per child at line 763. Linux does so at line 988. Normal replay also has a thread pool (`cli/fossilize_replay.cpp:873`); these are alternative execution paths, not a claim that every run nests full thread pools in every child.
- Linux timeout handling sends SIGABRT (`cli/fossilize_replay_linux.hpp:1589–1602`). The deadline is based on lack of completed work in the wait loop (`cli/fossilize_replay.cpp:930–975`), rather than necessarily measuring one pipeline's total compilation time. Windows robust timeout handling also uses crash recovery (`cli/fossilize_replay_windows.hpp:1203`). A timeout and spontaneous fault must therefore remain distinct even if recovery mechanisms coincide.
- CLI dispatch selects controller, child or normal replay (`cli/fossilize_replay.cpp:4770`). The external-replayer API exposes process waiting/completion (`fossilize_external_replayer.hpp:180–192`). Library callers need an operation outcome in addition to termination status.

**Conclusion:** multiple independently affected ranges and multiple observed failure categories in one run are architecturally possible. This is established by control flow, not by reproducing simultaneous failures. Parallelism and recovery are supported modes, not promises that every invocation uses them.

## Scenario checks

The following outcomes are required by the proposed rules. Reason identifiers are illustrative, not a proposed exhaustive taxonomy or wire schema.

| Scenario | Required observable result | Rules |
|---|---|---|
| 100 selected pipelines: 70 succeed, 10 verified reusable, 5 unsupported, 3 fail after timeouts, 2 fail validation, 10 remain unfinished | Final accounting totals 100; unsupported, timeout and validation reasons remain distinct; coverage is incomplete and exit is nonzero under the default full-coverage policy. Reason counts describe affected items and need not be additive if one item has several reasons. | 2–4 |
| Retry a timeout and ultimately complete every required item and output | Exit 0; timeout/retry history remains available; each item is counted once in final coverage. Worker failure is not automatically permanent run failure. | 2–4 |
| Restart continues after skipping an offending pipeline | Skipped required pipeline remains failed or unsupported as justified; successful later ranges do not erase the loss. Exit nonzero unless an explicit policy excludes that requirement. | 2–4 |
| Two workers report in reverse order | Same aggregate outcome, reason counts and exit status in either order. Neither last nor first error determines the whole result. | 3–4 |
| Worker dies before identifying its active pipeline | Record worker/range and observed termination; exact resource and root cause remain unknown. Unreconciled work is unfinished, not falsely assigned a crash cause or counted successful. | 2, 4 |
| Ten-second no-progress deadline leads to SIGABRT | Trigger is deadline/no progress; signal is mechanism; driver fault is unproven; record scope, deadline and retry/final effect. | 4–5 |
| Vulkan creations succeed but required cache/database output cannot be written | Execution facts remain successful; persistence is failed; whole operation is unsuccessful. A warm replay alone does not establish cache reuse. | 2, 6 |
| Recorder accepts queued work, then database writing fails | Initial acceptance stays distinct from final recording outcome; completion/status exposes recording loss; application's Vulkan result is preserved wherever safe. | 1, 6 |
| Quiet replay encounters failures while stdout exports command data | Stdout remains valid data; compact unsuccessful result survives quiet mode on the chosen diagnostic/result channel, with all categories and counts. | 3, 5 |
| Parent is killed before a final result can be delivered | Consumer sees abnormal termination or missing final result and classifies completion as unknown/incomplete. No promise of a final log from SIGKILL. | 4 |
| Optional diagnostic sink fails; alternatively required structured result cannot be delivered | Optional diagnostic failure does not turn successful Vulkan work into application failure; required CLI report delivery failure makes the CLI contract unsuccessful. Neither case invents output durability. | 1, 6 |
| Steam schedules another replay of unchanged input | Fossilize identifies its inputs, options, observed work/reuse and persistence; Steam supplies scheduling rationale. Unknown driver-cache reuse is explicit. | 5–6 |

The mixed example satisfies the accounting invariant: `70 + 10 + 5 + 3 + 2 + 10 = 100`. Reordering worker events leaves that partition unchanged; a successful retry moves an item's final outcome while preserving its attempt history. These checks expose no need for an exit code per reason or an exhaustive combination of failure codes.

## Check against the initiating issue

The local known issue reports hidden timeouts, timeout/crash ambiguity, exit zero despite unfinished work, and unclear persistence/reuse. Rules 3 and 5 preserve an actionable outcome in quiet integrations; rule 4 separates deadline triggers from signals and diagnosis; rule 2 prevents normal process termination from claiming complete coverage; rule 6 separates replay, persistence and reuse evidence. The rules do not claim that Steam discarded the entire cache or that Fossilize can observe every driver-cache decision.

These checks establish that the principles are applicable and can later become acceptance criteria. They do not establish that the current implementation meets them. Numeric schemas, ABI migration and implementation tasks are intentionally left to a later specification.

A subsequent [adversarial review](observability-adversarial-review.md) expanded these checks to unknown totals/counts, stale and duplicate reports, dependency blocking, cancellation, callback failures and finalization. The standard was revised accordingly; the known-total examples here remain illustrative.
