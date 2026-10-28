# Observability and diagnosis sweep

27 October 2026 · Source revision `556da9ff7d7616704dda515756219ab1dab693ec` · Historical research. Current task requirements and verification gates are maintained on GitHub.

## Recommendation

Start by repairing lost outcomes at their source, then propagate them through workers and controllers, and only then make the final report authoritative. More log lines alone cannot satisfy the standard. In particular, neither a drained queue, a clean child exit, the existing progress counters, nor the replayer-cache marker currently proves successful required work.

Implementation requirements, shared acceptance rules, native dependencies and critical paths are maintained in the [Observability outcome](https://github.com/robinwassen/Fossilize/issues/8). This document preserves the investigation and its limitations; it is not another task definition or status ledger.

## Scope, evidence and limitations

Applied the user-supplied [observability standard](../specs/observability-standard.md), SHA-256 `5acddd5528480a30941c987dc967e39ab05cb35a098e0b01faceaec73a4cd24f`. The hash identifies the original supplied document; the linked repository copy is the maintained standard. Its six rules are referenced as §1–§6. Read the local documentation index, descriptor, pillars, known issue, applicability review and adversarial review. The earlier reviews supplied scenarios, not proof about this checkout.

Swept first-party library/database code, capture dispatch and lifetime paths, external replay API and shared transport, Linux and Windows controllers, replay threads, device setup and feature filters, all 12 CLI target sources, build definitions and existing tests. Inspection depth was greatest at failure propagation and finalization boundaries. Vendored SPIR-V tools, RapidJSON, Vulkan headers, volk and miniz were not independently audited. An assertion originating inside RapidJSON was traced to Fossilize's unchecked access at its own parsing boundary.

**Runtime evidence:** isolated Linux `g++` debug builds, no GPU, `--null-device`, SPIR-V validation not compiled in, synthetic archives and a rejecting database mock. CMake configuration was attempted but failed because `cmake` is unavailable. Manual compilation succeeded for replay, convert, merge, list, rehash, prune, the probe and the existing `fossilize-test`; that system test returned 0. Windows/Android behavior, real driver failures, timeouts, cancellation races, cache hits and power-loss durability were not runtime verified. The direct builds are probes, not a replacement for the supported build matrix.

Reproducible source/runner and raw results are in [observability-sweep-evidence](observability-sweep-evidence/README.md). The research records are now tracked project documentation. The investigation changed no product source or shared ignore rules.

### Reproduced failures

| ID | Experiment | Observed result | What this establishes |
|---|---|---|---|
| E1 | Reject database `prepare()` after recorder initialization | `record_sampler()` returns true; one diagnostic; no writes | Acceptance does not establish recording completion; prepare failure has no returned completion result. |
| E2 | Prepare succeeds; database rejects every write | Acceptance true; three rejected writes; zero callback diagnostics | Background write results can disappear entirely. Three is the observed number of writes in this fixture, not three failed logical samplers. |
| E3 | Write five bytes to `/dev/full` through each shared file helper | Both helpers return true | Buffered acceptance is mistaken for output completion because close errors are ignored. |
| E4 | Convert a valid stream entry to a `.foz` symlink targeting `/dev/full` | Exit 0, empty stderr | The late database write failure reaches command-level false success. Conversion does not need to understand the entry's JSON. |
| E5 | Replay one compute entry containing `{}` | Normal replay exits 1 through trivial crash handling; master replay exits 0 after “Cannot continue” | A schema assertion terminates a worker and the controller loses the unsuccessful outcome. |
| E6 | Same input with `--progress --quiet-slave` | Exit 0; progress says overall 0/1 and dirty crashes 1 | Existing progress already exposes some loss; its existence does not fix the exit contract or preserve the original cause. |
| E7 | Replay compute entry containing `{"version":999}` | Normal and master replay exit 0 after version rejection | Controlled parse/support failures are also lost; this is independent of crashes and assertions. |
| E8 | Same version-rejected entry through list connectivity, rehash and prune | All exit 0; list prints an empty dependency row; prune reports 1→0 compute entries | Utility parse failures can be mistaken for usable output or intentional exclusion. |
| E9 | Remove the final byte of a stream entry, then list that resource type | Exit 0; empty stdout; sliced-file warning | Salvage is supported, but its incomplete discovery is not exposed as a caller-queryable outcome. This is not evidence that salvage itself should be removed. |

## Adversarial recheck and rejected shortcuts

This was a self-adversarial code and probe review, not an independent reviewer or sub-agent audit. The retained reproduction runner was then executed from a fresh directory: all 13 case exit statuses reproduced, the recorder/file-helper observations matched exactly, and the existing system test again returned 0. Source-reference targets were checked against the current files. For each material finding I checked the caller, alternative modes and possible existing evidence before proposing work.

| Challenge | Rechecked evidence / resulting constraint |
|---|---|
| “Progress counters and crash hashes already solve this.” | They exist and are useful. E6 still returns zero. Counters are partly optional, skips mix causes, retries duplicate activity and the ring is lossy. Keep them; add an authoritative outcome. |
| “Every child failure should make the run fail forever.” | Rejected. Recovery can satisfy the full contract. Separate failed attempts from final items; skipped required culprits/unknown ranges prevent success unless explicitly excluded. |
| “Fix controller exits and replay is done.” | Rejected. E7 has clean process exits after controlled parse rejection; void creation routines lose Vulkan failures. Worker producers must change too. |
| “The crash fixture merely exposes an assertion, not a real bug.” | It does expose an unchecked Fossilize schema boundary. E7 separately reproduces false success without assertions, and E5 proves controller behavior after worker loss. No driver crash was claimed. |
| “A warning about truncation is sufficient; fail all truncated captures.” | Rejected as a blanket policy. Stream salvage is intentional. Return its scope/completeness and let explicit policy define whether the salvaged prefix satisfies the requested operation. |
| “Any failed optimization or error-level log means failure.” | Rejected. A documented original-module fallback can satisfy a copy/transform policy. Severity and operation success must remain separate. |
| “Buffered fwrite success proves output is safe.” | E3/E4 disprove close completion; even checked close does not prove power-loss durability or driver cache reuse. |
| “Replay-cache entries are proven compiled pipelines.” | Creation dispatch writes markers without checking creation success. Marker membership proves membership under the old producer behavior, not durable compilation. No real-driver reproduction was claimed. |
| “Turn on more logs or adopt OpenTelemetry first.” | Rejected. Neither repairs discarded return values, close errors, causality or coverage. No SDK, remote collector or upload is required. |
| “Guarantee a final result from every crash.” | Rejected. Signal safety, SIGKILL, blocked storage and torn writes constrain delivery. Missing final evidence is incomplete/unknown. |
| “Wrap a callback and guarantee it never blocks.” | Rejected. Arbitrary synchronous code cannot be safely interrupted. Use documented sink constraints or bounded queued delivery and truthful diagnostic-loss reporting. |
| “This proves the original Steam cache-loss theory.” | Rejected. These probes use no GPU and do not measure Steam scheduling, driver-cache durability or reuse. They establish Fossilize outcome propagation defects. |
