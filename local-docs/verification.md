# Testing and verification

Run command examples from the repository root unless stated otherwise. Source/build paths in this guide are repository-relative; absolute placeholder paths must be resolved before use.

Inventory checked against first-party source and the local Steam installation on 27 October 2026. Select methods appropriate to the change; expensive, interactive and real-game checks are separate from unit/regression tests. Recheck this inventory when build targets, flags or harnesses change. A built target, an empty CTest run, a benchmark and an assertion-bearing test are different evidence.

## Preparation and reporting

- Inspect `CMakeLists.txt`, `cli/CMakeLists.txt`, `layer/CMakeLists.txt`, `test/CMakeLists.txt` and `CMakePresets.json`. Check `git submodule status --recursive`; initialize required dependencies with `git submodule update --init --recursive` when setting up a build. Full CLI builds require populated SPIR-V dependencies and compatible Python/build tools.
- Use separate build directories for compiler, architecture, Debug/Release, sanitizer and feature configurations. Use isolated working/output directories: several tests create/delete fixed-name files. Preserve original captures/caches and never run conflicting harness instances in the same directory.
- Record revision/local changes, compiler/build options, exact commands, process status and artifacts. GPU/Steam evidence also records device/driver/loader, input identity, relevant compilation environment, deadlines, cache state, AppID and Steam/Proton/runtime versions. Avoid collecting unrelated secrets.
- Report passed, failed, unsupported, skipped/unavailable and incomplete separately. Zero exit/progress 100% currently does not establish complete replay or persistence; assert relevant output and coverage facts. Future guarantees in the issues are not current implementation guarantees.
- At this inventory check, `g++`, `vulkaninfo` and Steam were present; CMake/CTest/Ninja were not on PATH. Earlier direct-build probes and `fossilize-test` passed their recorded executions, not a full current CMake/CTest matrix. Documented commands below are procedures, not claims they have all been run here.

## Change-harness skill checks

The repository skill has a separate standard-library Python suite; CTest does not
discover it. The full discovered suite is mandatory, scheduled in bounded groups.
From the exact committed implementation/checker candidate, create a fresh directory
outside source and use the supported runner (each invocation has its own real
600-second process deadline):

```sh
export HARNESS_EVIDENCE_DIR="$(mktemp -d /tmp/fossilize-harness-check.XXXXXX)"
timeout --signal=TERM --kill-after=5s 600s env PYTHONDONTWRITEBYTECODE=1 python3 -B .agents/skills/implement-change-harness/scripts/run_harness_tests.py --inventory --output "$HARNESS_EVIDENCE_DIR/inventory.json"
for group in regression-test_harness_artifacts regression-test_harness_closeout regression-test_harness_issue_artifacts regression-test_harness_issue_closeout regression-test_harness_migration regression-test_harness_state regression-test_issue_guidance regression-test_issue_migration regression-test_issue_source delivery-ordinary delivery-w09 delivery-w11-0 delivery-w11-1 delivery-w11-2 delivery-w11-3 regression-extra; do
  timeout --signal=TERM --kill-after=5s 600s env PYTHONDONTWRITEBYTECODE=1 python3 -B .agents/skills/implement-change-harness/scripts/run_harness_tests.py --run-group "$group" --inventory "$HARNESS_EVIDENCE_DIR/inventory.json" --output "$HARNESS_EVIDENCE_DIR/$group.json" || break
done
timeout --signal=TERM --kill-after=5s 600s env PYTHONDONTWRITEBYTECODE=1 python3 -B .agents/skills/implement-change-harness/scripts/run_harness_tests.py --verify-results "$HARNESS_EVIDENCE_DIR"
timeout --signal=TERM --kill-after=5s 600s python3 /home/robin/.codex/skills/.system/skill-creator/scripts/quick_validate.py .agents/skills/implement-change-harness
```

For parallel harness work, explicitly select `--root-issue URL --run-id RUN` in
harness_state.py and retain separate source/build/output paths. Every command in
the block above (and every build/probe) is a child of the package's
`scripts/verification_lock.py --repo /absolute/worktree --timeout 600 -- COMMAND`.
The wrapper replaces the outer timeout in these examples: lock waiting is outside
the child's600-second deadline. It uses one persistent common-Git-directory lock,
independent of package/worktree location. The live coordinator waits and continues
automatically. Never nest wrappers on the same common directory. Publish command
intents first; use unique intent_id/child_command argv and wrapper receipt paths,
then coordinator command-wait records measured contention once before the next
budget gate. Execution time and repair limits are unchanged. Artifact updates use
a separate short lock and reload only the selected run transaction against the
latest tree. See the skill's references/state.md for receipt settlement.

Inventory uses actual full `unittest` discovery of `test_*.py`, without running
fixtures. Every discovered ID has exactly one owner, including a catch-all for
new modules. Four distinct W11 unittest IDs partition the complete explicit
publication scenario inventory; each independently checks runtime checkpoint
capture against that inventory. W09 retains its full effect-boundary matrix.
The original 113 methods are accounted for by a checked compatibility manifest;
the original W11 method maps to its four shards and inventory/rejection control.
The verifier freshly discovers all tests and requires exact disjoint executed
and scenario unions, current candidate/attempt/digest, completed passing statuses,
and every group result. Missing, skipped, failed, stale, duplicate, unfinished or
unexecuted results fail. Use a new directory after changing candidate or retrying
a failed attempt. Independent groups may run in at most three isolated processes
concurrently; their tempfile repositories/services are separate.

Discovery alone, one passing shard and historical reports do not establish a
complete pass. Ordinary full discovery remains available for exploration:
`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s .agents/skills/implement-change-harness/scripts -p 'test_*.py' -v`.
It is too large for a single mandatory 600-second gate; the complete bounded union
above replaces that gate without removing assertions or increasing any deadline.
The validator path is this host's installed skill-creator tool; resolve it on
another host. PyYAML belongs to that validator, not the helper runtime. Record
exact commands, timings, nonzero discovered/executed IDs and case/scenario mappings.
A passing Python suite does not prove actual role conduct, live GitHub behavior
or C++/Vulkan/Steam behavior.

`test_issue_source.py` and `test_issue_migration.py` use a subprocess fake gh service
for identities, paginated reads, complete requirement/graph migration, uncertainty,
authentication, edit conflicts and durable external-operation records.
`test_harness_migration.py` exercises schema3 issue runs, split work units, material
input drift, old-state migration, the exact active-attempt bridge and source-free
fresh-clone resumption. `test_harness_issue_closeout.py` distinguishes open PR
readiness from simulated user-merged delivery; `test_harness_issue_artifacts.py`
executes live-schema artifact publication counterexamples.

The retained T01–T17/C01–C08/P01–P12 assertions are explicit historical compatibility
fixtures in `test_harness_state.py`, `test_harness_closeout.py` and
`test_harness_artifacts.py`. Their private fixture-only legacy engine has no CLI
or normal task authority. Current equivalents preserve stale-worker/report fences,
required evidence, scoped findings, budgets, separate final checking, actual PR
identity, non-force exact publication, dirty views, divergent writers and terminal
null-pointer retry. Legacy Markdown/allocator examples exist only as compatibility
fixtures; current operations never require them.

Live read-only migration audits additionally compare complete original requirements,
shared acceptance, honest merge evidence, native hierarchy and all dependency edges
with the immutable historical input and published evidence. Independent checker
review and actual source-PR/artifact publication remain separate gates. Keep small
sanitized outputs outside source; do not maintain a generated task mirror.

## Canonical builds and CTest

From the repository root:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel
ctest --preset linux-debug
```

Repeat with `linux-release`; native Windows equivalents are `windows-debug`/`windows-release` in a prepared compiler environment with Ninja. The preset file declares schema 10 and CMake minimum 4.0, distinct from the older root CMakeLists minimum. Check dependency compatibility as well. Without presets:

```sh
cmake -S . -B out/build/verify-debug -DCMAKE_BUILD_TYPE=Debug -DFOSSILIZE_TESTS=ON
cmake --build out/build/verify-debug --parallel
ctest --test-dir out/build/verify-debug -N
ctest --test-dir out/build/verify-debug --output-on-failure
```

For multi-configuration generators, build with `--config Debug` and test with `-C Debug`. Focus with `ctest --test-dir <build> -V -R '<test-regex>'`; repeat stress cases with `--repeat until-fail:20`. Check the discovered test count before claiming coverage.

| Registered CTest name | Executable | Scope |
|---|---|---|
| `fossilize-system-test` | `test/fossilize-test` | CPU-only recording/serialization/replay through test interfaces, hashing/handles, databases/concurrency/metadata, filters, logging and identifiers. No real driver compilation. |
| `varint-system-test` | `test/varint-test` | CPU-only varint encoding/decoding. |
| `application-info-filter-test` | `test/application-info-filter-test` | CPU-only application/engine/environment filters and bucket rules. |
| `object-cache-test` | `test/object-cache-test` | CPU-only object-cache behavior. |
| `feature-filter-test` | `test/feature-filter-test` | Synthetic feature/SPIR-V support cases; built with CLI support, no physical GPU required. |
| `linux-futex-test` / `spinlock-futex-test` | `test/futex-test` | Non-Windows concurrent lock stress; Apple uses the spinlock name. |
| `layer-test` | `test/layer-test` | Optional real Vulkan layer integration; see below. |

A full CLI/layer build also registers the diagnostics regression tests for
[issue #10](https://github.com/robinwassen/Fossilize/issues/10):

| Registered CTest name | Scope |
|---|---|
| `replay-diagnostic-test` | Actual replay pipeline creation and cache-export branches with deterministic Vulkan/CRT failure injection, compile-required retries, exact errno retention and cleanup checks. |
| `device-diagnostic-test` | Actual device initialization with fake loader/dispatch, distinct results at every checked setup stage, safe destruction, successful/null-device and support-limit controls. |
| `capture-diagnostic-<mode>-<kind>-<outcome>` (45 tests) | Exported layer entrypoints through a fake loader: normal, QA, paranoid, eager and pipeline-binary recording; graphics/compute/raytracing; rejection, successful recording and controlled Vulkan failure. Each process has its own output directory and spam counters. |
| `diagnostic-compatibility-test` | Preserved source interfaces, dependency-complete known-hash FOZ fixture/readback and real `fossilize-replay --null-device` CLI invocation. |

These are CPU-only assertions, including explicit checks that survive `NDEBUG`.
New runners inherit the project exception/sanitizer flags and have process
timeouts. The layer caller in `test/capture_diagnostic_test.cpp` loads the
library by path with `dlopen`/`LoadLibrary`; it can be built against baseline
headers, run with the baseline library, and then run unchanged with the candidate
library as limited Vulkan layer C ABI evidence. The caller in
`test/diagnostic_compatibility_test.cpp` can likewise be compiled against baseline
headers and linked to candidate libraries for source compatibility evidence.
This does not establish a general C++ binary ABI guarantee. Preserve those
executables and exact baseline/candidate identities outside source when checking
compatibility. Human diagnostics intentionally add operation/result context and
correct pipeline names; there is no new stable log parsing contract or result
schema. Cache close failures become visible without changing aggregate CLI exit
policy. These tests do not demonstrate driver compilation or durable cache output.

The utility parse regressions for [issue #11](https://github.com/robinwassen/Fossilize/issues/11)
register 19 additional `utility-parse-<tool>-<case>` tests whenever CLI support
is enabled. They invoke the actual list/rehash/prune executables through portable
CMake `execute_process` calls (30-second child deadlines, 90-second CTest limits),
using GPU-free valid outer FOZ fixtures. They check incompatible versions, invalid
JSON, valid populated/empty inputs, deterministic valid-then-rejected ordering,
list dependency failures and verified empty dependencies, ordinary list/size
compatibility, and prune rejection both before and after an application callback.
Readback reparses emitted records and checks hashes, payload semantics and compute
reference closure. Empty inputs have zero resource-work records; rehash preserves
its existing synthesized default application context, whose exact hash, empty
application/features objects and decoded null context are checked separately. Explicit checks remain active in Release. The helper and
utility paths can be overridden separately to run the same failure assertions
against unchanged baseline executables. The applicable Linux runtime matrix is
Debug/Release with exceptions OFF/ON, CLI and layer ON; portable registration is
not evidence of native Windows execution, which requires a Windows host.

The intentional command-result change is limited to parser failure in
`fossilize-list --connectivity`, `fossilize-rehash` and `fossilize-prune`: they now
stop immediately, identify the rejected tag/hash and incomplete scope, and return
`EXIT_FAILURE`. Log severity does not determine the outcome. Plain list and
`--size` remain enumeration and do not validate resource JSON. Connectivity fills
all create info before printing; any fill failure emits no connectivity rows,
including rows for earlier valid items. A successfully parsed empty dependency
set still prints its ordinary empty row. Rehash can retain previously accepted
resources after recorder teardown; its failed output is an incomplete prefix.
Callbacks accepted from a later failing multi-object blob are not rolled back.
Prune may retain previously copied accepted application metadata, but stops before
copying the rejected metadata and before selecting/copying pipelines: it promises
neither a pipeline prefix nor dependency closure after failure. No output from a
failed invocation should be treated as a complete result. These changes do not
redesign asynchronous persistence error reporting. Opt/disasm migration and the
explicitly permitted optimizer fallback policy remain future scope; these tests
make no claim to verify those tools or the complete observability standard.

A normal Linux CLI/layer-enabled build with SPIR-V validation registers 174 tests before optional layer integration, including 14 Linux fork-child cases and 28 replay-controller cases retained from [issue #13](https://github.com/robinwassen/Fossilize/issues/13), plus 32 replay item-outcome cases. Discover the current platform-specific inventory with CTest before reporting coverage; this Linux count does not establish native Windows execution. The original core/feature/lock subset is six on Linux and five on Windows. Explain platform/configuration differences. Direct execution is possible from an isolated working directory using absolute executable paths, but CTest supplies important environment settings for the layer test.

The replay outcome regressions for [issue #14](https://github.com/robinwassen/Fossilize/issues/14)
register `replay-outcome-{state,parse-read,creation,compile-required-loop,support,validation,setup-dependency,worker-queue,matrix}`,
four `replay-outcome-entry-*` private production-entry checks, fifteen
`replay-outcome-cli-*` fixtures and four Linux `replay-outcome-controller-*`
order/progress checks. Tests use explicit checks in Release, controlled Vulkan
producers, rejecting databases, the real SPIR-V validator, checked FOZ readback,
real workers and CLI processes. Run the full CPU suite in Debug/Release with
exceptions OFF/ON and `FOSSILIZE_REPLAYER_SPIRV_VAL=ON`; discover actual IDs with
`ctest --show-only=json-v1`. Native Windows execution is explicitly excluded from
the issue's Linux verification run at the user's request; portable source and
registration do not establish Windows runtime behavior. Windows private controller
order tests cover absent progress; inherited anonymous shared-memory order tests
are Linux-only, while shipped CLI/ExternalReplayer tests exercise named progress
sharing on Windows when run there.

`ReplayItemOutcome` is private bounded evidence: independent reason counters and
one first tag/hash/raw Vulkan result per category, protected by a mutex. Categories
are read, parse, creation, unsupported, validation, setup and dependency failure;
counts are overlapping observations, not unique logical item totals. Initial
objects, shader callbacks, worker parsing/creation and device/run setup retain
unsuccessful outcomes without requiring shared progress. Queue completion still
counts every processed job, including failed processing. Required unsupported or
blacklisted work and skipped compile-required work are unsuccessful; a successful
compile-required fallback remains successful, while a failed required loop
iteration cannot be erased by a later success. Explicit ranges/whitelists exclude
work from the requested scope; failed or invalid creation cannot add successful
replay-cache/validation-whitelist markers; issue #15 further isolates replay hints
by policy/device mode and prevents zero-iteration hints as described below.

Normal replay now returns nonzero for retained required failures. Current paired
children/controllers add an internal `REPLAY_FAILED <reason-mask> <drained>`
terminal pipe record for orderly failures; the nonzero OS status remains required.
Controllers retain the reason set, drain orderly siblings and avoid crash counters
or restart attempts for controlled item failures. Drained assignment ranges do
not certify a successful prefix. Missing/truncated terminal proof remains an
unresolved worker result under the existing recovery policy. Lossy optional
progress-ring forwarding cannot erase the pipe/OS result. Public ExternalReplayer
declarations, SharedControlBlock layout/version, existing progress/resume messages
and cached wait/query status semantics remain unchanged; tests assert those
specific contracts. This is not a general binary ABI or mixed-version child
protocol guarantee. Controllers reject a missing explicitly selected hash before their zero-child
completion shortcut, including empty archives. The `--progress` wrapper resolves
`--pipeline-hash` through
existing range options so missing hashes cannot silently replay a different scope.
Direct child CLI forces one thread and consumes controller-filtered metadata;
standalone child whitelist arguments are not a supported selection interface.

The E7 oracle is `test/replay_outcome_cli.cmake` with `CASE=e7` (and `CASE=good`
for its clean control), built fixture generator, real `REPLAY`, isolated `WORK`
and optional `BASELINE_REPLAY`. Against unchanged base Debug/Release executables,
the version-999 record must reproduce false-success zero in normal, child,
master and progress modes; candidate exits must be nonzero and clean controls
zero on both. The old Release `{}` controller fixture now expects orderly parser
failure nonzero, while Debug assertion-triggered E5/E6 worker-loss checks remain.
These CPU results establish neither actual driver compilation, hardware support,
cache output persistence nor whole-standard observability compliance. Harness
Python component tests are a separate scope when harness helpers change.

The `*-library-only-*` presets disable **both CLI and tests**. They are build/package checks, not test coverage; their test presets/workflows must not be reported as a passing suite. To test the core without CLI/SPIR-V dependencies:

```sh
cmake -S . -B out/build/verify-core -DCMAKE_BUILD_TYPE=Debug \
  -DFOSSILIZE_CLI=OFF -DFOSSILIZE_VULKAN_LAYER=OFF -DFOSSILIZE_TESTS=ON
cmake --build out/build/verify-core --parallel
ctest --test-dir out/build/verify-core --output-on-failure
```

Relevant configuration matrix: GCC/Clang; Debug/Release; `FOSSILIZE_ENABLE_EXCEPTIONS=ON/OFF`; CLI ON/OFF; layer ON/OFF; native Linux/Windows and supported pointer sizes. GCC/Clang normally disable exceptions; MSVC differs. `.travis.yml`/`appveyor.yml` contain historical matrices, not proof those toolchains or hosted jobs still operate.

**Package/install smoke:** run `cpack --preset linux-release` (or matching Windows preset), stage with `cmake --install <build> --prefix <private-stage>`, inspect package contents and run staged CLI/layer discovery checks. Unix layer manifests embed the **configure-time** install prefix; configure that to the staging location rather than assuming an install-time override rewrites JSON. `cmake --workflow --preset linux-debug` runs configure/build/test/package. Never incidentally install a test layer into system or Steam locations.

## Instrumentation, analysis and failure testing

- **ASan:** separate build with `FOSSILIZE_SANITIZE_ADDRESS=ON`, then build/run tests. **TSan:** separate build with `FOSSILIZE_SANITIZE_THREADS=ON`. Do not combine them. Inspect effective compile/link flags: test/utility targets do not all apply identical target options. Report driver/instrumentation incompatibility separately from product failures.
- **UBSan:** no dedicated project switch. Configure a separate GCC/Clang build with `-fsanitize=undefined -fno-sanitize-recover=all` in compilation and appropriate executable/shared link flags. Run tests and verify instrumentation actually covers changed code.
- **Static analysis/warnings:** MSVC has `FOSSILIZE_MSVC_ANALYZE=ON`. For Clang analysis/clang-tidy, generate `compile_commands.json` with `CMAKE_EXPORT_COMPILE_COMMANDS=ON` and analyze changed first-party translation units. Optional Valgrind runs can inspect CPU harnesses. Record tooling/configuration and adjudicate findings; there is no universal first-party static-analysis/Valgrind suite.
- **Coverage:** separate GCC `--coverage` or Clang/LLVM instrumentation build, including linking, followed by tests and matching coverage tooling. No top-level coverage target/threshold is currently registered. Line/branch coverage complements behavioral assertions.
- **Deterministic negative tests:** rejecting database mocks, controlled Vulkan dispatch, short-write/flush/close injection, incompatible/malformed/truncated/checksum-invalid input, lost IPC/full rings, worker death, cancellation and stale/duplicate/reordered attempts. Use fake clocks/barriers and process watchdogs, not timing races. `/dev/full` is a Linux integration case; retain portable injected equivalents. Assert host survival and correct errors in Debug/Release.
- **Fuzz/property testing:** isolated parser/database/varint/message harnesses with bounded sizes/runtime/memory, fixed seeds/corpora and sanitizers. There is no registered first-party fuzz target today. Preserve discoveries as deterministic fixtures and register new harnesses with the test runner/CI.
- **Existing research runner:** `python3 local-docs/reviews/observability-sweep-evidence/reproduce.py --repo /absolute/repo --out /tmp/fresh-fossilize-probes`. Requires a new output directory, Linux/g++ and populated dependencies; directly compiles probes without CMake. It records historical defects E1–E9, including false-success exits, and runs the existing system test. It is an observation runner, **not** a passing observability conformance suite; follow the issues' assertion-bearing acceptance requirements.
- **Exploratory unstable-driver stress:** the disabled `SIMULATE_UNSTABLE_DRIVER` macro in `cli/fossilize_replay.cpp` injects random crashes/deadlocks during creation/teardown. No CMake switch exists. Use a disposable diagnostic build; nondeterministic stress is not a regression oracle or an ordinary Steam deployment build.


Approved delivery regression coverage lives in
`.agents/skills/implement-change-harness/scripts/test_delivery_wrapup.py` (W01–W14), with W15 runner controls in
`test_harness_runner.py`. Run every bounded group and the exact-union verifier
above. The fixture uses
subprocess fake gh and real disposable Git/bare remotes to produce actual squash
commits and inspect tree/topology/ref identities. Tests assert approval/acceptance
fences, live drift, conditional cleanup, dirty/unrelated preservation, completed
terminal-run bookkeeping, ticket/comment evidence, explicit mutation-boundary
process-death recovery and exact publication failures. Fixture service response
loss and artifact publication seams require fresh helper instances on retry.
The CLI happy path invokes one approved wrap-up command and emits compact results.
Repeated matrix reads execute the same persistent fake-gh entrypoint in-process
to avoid interpreter startup overhead; mutation calls still use subprocess gh
and real Git. CLI/service-response-loss cases retain subprocess reads as well.
These tests do not mutate production PRs/issues or prove live GitHub protection
behavior. No C++ build, GPU or Steam verification applies to harness-only changes.

## GPU-independent CLI, database and embedding checks

Use a small known valid fixture, including required dependencies, and capture stdout, stderr and status separately:

```sh
out/build/verify-debug/cli/fossilize-replay --null-device --num-threads 1 /absolute/input.foz
out/build/verify-debug/cli/fossilize-replay --null-device --master-process --num-threads 2 /absolute/input.foz
out/build/verify-debug/cli/fossilize-replay --null-device --progress --num-threads 2 --quiet-slave /absolute/input.foz
```

Null-device exercises parsing/setup/thread/controller paths with dummy Vulkan implementations. It does not establish real support, driver compilation or cache reuse. Apple/Android compile out robust master/progress modes (`NO_ROBUST_REPLAYER`).

- **CLI contracts:** per tool, cover valid/empty input, invalid invocation, support limits, missing dependencies, corrupt archives and failed output delivery. Inspect each help/parser first: not every tool has conventional `--help`; `fossilize-bench` has no argument parser and executes its workload. Do not blindly run every executable with `--help` as a smoke suite.
- **Database semantic round trips:** convert FOZ↔directory/ZIP where supported, merge overlapping inputs, list sizes/connectivity, rehash and prune **copies**. Compare expected `(tag, hash)` sets, decoded payloads and dependency closure, not necessarily byte-identical compressed files. Verify intact prefixes and failed finalization. `fossilize-synth` plus explicit SPIR-V fixtures can create cases; `fossilize-opt`/`fossilize-disasm` exercise transformation/fallback/inspection. Assert unsupported and partial paths rather than assuming whole-archive support from zero exit.
- **Library/API integration:** compile a small consumer using recorder, custom `StateCreatorInterface`/`DatabaseInterface`, log callbacks and `ExternalReplayer` start/wait/progress/fault queries. Test no sink, ownership/lifetime, queued versus completed work and exceptions configuration. Old-source compile/run fixtures prove claimed source compatibility; prebuilt/protocol fixtures are separate evidence for claimed binary/wire compatibility.
- **Benchmark:** run the absolute `cli/fossilize-bench` path from a disposable directory; it creates/removes `.test.*` FOZ/ZIP files and measures synthetic record/read/compression/checksum performance. It is not CTest-registered and can log replay failure without unsuccessful exit. Compare time/CPU/RSS/I/O under identical fixture/build/hardware conditions; do not treat timings or exit alone as correctness.

## Real driver replay, validation and cache tests

Record `vulkaninfo --summary` where available. GPU checks need a working Vulkan loader/ICD/device; validation needs the installed Khronos validation layer. `--spirv-val` requires `FOSSILIZE_REPLAYER_SPIRV_VAL=ON` at build time. Replay copied common/application captures together:

```sh
out/build/verify-debug/cli/fossilize-replay --num-threads 1 --spirv-val --enable-validation \
  /absolute/common.foz /absolute/application.foz
out/build/verify-debug/cli/fossilize-replay --master-process --num-threads 2 --timeout-seconds 60 \
  /absolute/common.foz /absolute/application.foz
```

Repeat relevant modes with one/multiple workers and quiet workers. The timeout is **no-progress based**, not necessarily per-pipeline elapsed time; 60 seconds is an example test setting. Isolate an offender with `--pipeline-hash` or `--graphics-pipeline-range`, `--compute-pipeline-range`, `--raytracing-pipeline-range` (end exclusive), retaining the same input identity/order. On supported Linux builds, `--disable-signal-handler` permits debugger/core investigation. Debugger/profiler/system-call tracing is opt-in diagnosis; record overhead and distinguish controller timeout from spontaneous faults.

Use explicit private outputs for `--on-disk-pipeline-cache`, validation cache/whitelist/blacklist, module identifiers, `--replayer-cache` and `--enable-pipeline-stats`. Exercise initial creation, repeat replay, incompatible context and persistence failures. Compile-required policy flags and `--loop` give additional cases; loops are repeated work, not extra coverage. Successful replay, saved output, marker membership, a faster repeat and application-cache feedback prove different things. Never infer power-loss durability or opaque driver-cache hits from replay success.

Specialized hardware tools, **not CTest tests**:

```sh
out/build/verify-debug/cli/fossilize-validate-cache-roundtrip \
  --fossilize-replay /absolute/repo/out/build/verify-debug/cli/fossilize-replay
out/build/verify-debug/cli/fossilize-feature-sifter
```

The roundtrip executable is built on non-Windows/non-Android platforms; it creates synthetic pipelines/cache, replays in another process and checks feedback. Explore `--robustness`, `--dxvk`, `--vkd3d-proton`, `--pipeline-binary-key` on capable devices. It currently parses human logs and does not reject every mixed hit/miss case; inspect the all-hits policy/evidence. Feature-sifter checks feature/extension effects on pipeline cache/binary keys, with `--block-extension`, `--dxvk`, `--vkd3d-proton` variants, requiring suitable driver support. Neither certifies all hardware.

`cli/validate_roundtrip_sandbox.sh` is an interactive vkd3d-proton `gears` cache experiment, including descriptor-buffer/heap variants. **Do not run it unmodified on the normal account:** it hard-codes driver/demo paths and deletes `~/.cache/nvidia` and `~/.cache/mesa_shader_cache`. Adapt its phases to an isolated account/cache setup or scoped preserved outputs. It is not an automated regression test, and broad cache deletion is not a general testing prerequisite.

## Capture layer and application continuity

The optional real Vulkan CTest needs loader/development libraries and a usable device:

```sh
cmake -S . -B out/build/verify-layer -DCMAKE_BUILD_TYPE=Debug \
  -DFOSSILIZE_TESTS=ON -DFOSSILIZE_VULKAN_LAYER=ON -DFOSSILIZE_TEST_RUNTIME_LAYER=ON
cmake --build out/build/verify-layer --parallel
ctest --test-dir out/build/verify-layer -V -R '^layer-test$'
```

CTest sets layer discovery/enabling, dump prefix, application filter and `BLOCK_ENV`. `layer_test.cpp` is partly inspection-oriented: it lists files and shells out to remove the dump-prefix glob. Use a dedicated build path without shell-special characters and no valuable matching files. Assert expected artifacts separately when a change requires stronger guarantees than successful harness execution.

For an ordinary Vulkan application/demo, create a fresh scratch directory, then:

```sh
VK_LAYER_PATH=/absolute/repo/out/build/verify-layer/layer \
VK_INSTANCE_LAYERS=VK_LAYER_fossilize \
FOSSILIZE_DUMP_PATH=/absolute/scratch/capture \
  /absolute/vulkan-application
```

`FOSSILIZE=1` alone enables an already discovered implicit manifest; it does not locate an uninstalled build. `VK_LAYER_PATH` plus explicit enabling handles build-tree discovery; it replaces explicit search paths, so preserve validation-layer directories when combining layers. Loader diagnostics and Linux `/proc/<pid>/maps` can prove the loaded library. Modern implicit-search variables are version-dependent (Khronos references below).

Compare a repeatable sequence with capture disabled/enabled: Vulkan results/handles, rendering, startup/frame timing, CPU/RSS/I/O, resource sets and replayability. Include unwritable destinations, failed preparation and unsupported recording while asserting application continuity. Test `FOSSILIZE_DUMP_SYNC=1` separately because it changes recording timing/overhead. `FOSSILIZE_DUMP_SIGSEGV=1` changes signal handling and is an emergency experiment. `FOSSILIZE_PRECOMPILE_QA=1` probes compilation-required/fallback behavior and changes pipeline creation behavior; identify it as a separate QA mode.

`test/multi-instance-and-device-test` is CLI-dependent, built but **not CTest-registered**. Run its absolute path under the capture environment from an isolated directory. It launches 64 threads/devices/instances and on Linux forks additional processes; wait for all descendants. The source expects four application-version groups with 10,000 sampler variants each, but ends with `TODO: Automatically check here`; device-init failure can silently end a thread. Exit zero is stress-execution evidence only. Add device-success and artifact-content assertions for acceptance; avoid running this heavy stress alongside a game.

## Steam: using the local build in a real game workflow

This is manual integration evidence, distinct from automated tests. A replayer replacement tests the CLI/Steam process protocol, not automatically a locally changed capture layer or Steam's embedding library. This documentation does not authorize or claim that client files have been replaced, Steam restarted or a game run as part of a documentation task.

1. Select one AppID and repeatable startup/gameplay sequence. Save launch settings and original relevant cache/input state. Locate its Steam library, collect an untouched baseline, and identify the **actual** replay executable, working directory and arguments from `logs/shader_log.txt` and the live process. Copy inputs/dependencies for standalone reproduction. The local known issue concerns AppID 1222670 (The Sims 4), if available.
2. Build a normal native Release replayer matching Steam's selected architecture/runtime. Check executable architecture/dynamic-library requirements and copied-workload replay in the relevant runtime. A host binary running outside the Steam container is not proof it runs inside. Do not deploy random-fault or sanitizer builds as ordinary replacements.
3. Fully exit Steam and its replay processes. Prefer the Linux wrapper below; otherwise temporarily replace only the observed active binary with a preserved original. Game-only `%command%` options are too late to configure Steam's separate pre-launch replay process.
4. Launch Steam with the setup and prove the local binary ran: private wrapper marker plus executable identity/hash (`/proc/<pid>/exe` on Linux), arguments, inputs and resulting artifacts. A launch that schedules no replay is **integration not exercised**, not success. Do not delete all shader caches to force scheduling; any deliberate cache reset is a separate scoped experiment with originals preserved.
5. Compare untouched/local/restored-cache/warm-repeat runs, controlling workload, driver and policy. Record timeouts/failures/coverage, persistence/reuse evidence, processing duration and game startup/gameplay behavior. Steam “completed”, current exit zero and shorter startup are not full coverage or cache-hit proof. Start with unchanged arguments/deadlines; changed deadlines are a separately labelled experiment.
6. Restore the original environment/binary, restart normally and verify rollback. Recheck identity each run because Steam updates can replace client files. Retain scoped evidence only.

### Linux wrapper hook: preferred method

`cli/fossilize_replay.cpp` checks `FOSSILIZE_REPLAY_WRAPPER`; Linux dispatch uses `execvp` with the original arguments and sets `FOSSILIZE_REPLAY_WRAPPER_ORIGINAL_APP`. The wrapper must clear the recursion variable. Windows explicitly does not support this hook in current source; robust wrapper dispatch is compiled out on Apple/Android.

Create an executable script at a private absolute path outside Steam's installation:

```sh
#!/bin/sh
unset FOSSILIZE_REPLAY_WRAPPER
exec /absolute/repo/out/build/linux-release/cli/fossilize-replay "$@"
```

After fully exiting Steam, launch the client from a terminal:

```sh
FOSSILIZE_REPLAY_WRAPPER=/absolute/scratch/fossilize-local-wrapper steam
```

Verify the variable reaches replay, including runtime/container boundaries; an already-running client need not inherit it. Preserve arguments, working directory, inherited descriptors, stdout and stderr: Steam uses shared-memory/control descriptors and progress communication. Use `exec`; avoid backgrounding/pipelines or redirecting protocol stdout. Optional provenance goes to a separate private side file, not a full environment dump. Do not append diagnostic/controller flags without testing forwarding/child behavior.

The installed local `ubuntu12_64/fossilize_replay` hook was exercised during this inventory with `--help` and an argument containing spaces forwarded to a temporary receiver. Original-app identification and clearing the recursion variable were verified. This proves hook dispatch/argument forwarding only, **not** descriptor preservation, runtime compatibility, Steam scheduling or real game replay. Recheck after client updates. A failed/unsupported wrapper can fall back to the original executable, so require positive provenance. Rollback: fully restart Steam without the variable.

### Temporary bundled-binary drop-in: fallback

Discover the active path from process/logs. Locally observed candidates under `~/.steam/debian-installation` include `ubuntu12_64/fossilize_replay`, `steamrt64/fossilize_replay`, and `ubuntu12_32`/`steamrt32` counterparts. Presence does not prove selection; Flatpak/other layouts differ. Do not replace every candidate or assume the build's hyphenated filename equals Steam's underscored one.

With Steam/replay children stopped, resolve the actual regular-file target, verify architecture/runtime compatibility, and use a backup path that does not exist. This block is a **template after discovery**, not automatic deployment:

```sh
(
  set -eu
  test_replayer=/absolute/discovered/Steam/path/fossilize_replay
  local_replayer=/absolute/repo/out/build/linux-release/cli/fossilize-replay
  saved_replayer="${test_replayer}.before-local-test"
  test -f "$test_replayer"
  test ! -L "$test_replayer"
  test ! -e "$saved_replayer"
  file "$test_replayer" "$local_replayer"
  sha256sum "$test_replayer" "$local_replayer"
  cp -a -- "$test_replayer" "$saved_replayer"
  install -m 755 -- "$local_replayer" "$test_replayer"
  cmp -- "$local_replayer" "$test_replayer"
)
```

Printing `file` output does not enforce compatibility; verify it before executing replacement. Keep the original until rollback is verified. After testing, stop Steam, check whether an update changed the target, restore with `cp -a -- <saved-original> <actual-target>`, and verify `cmp`/hash before normal relaunch. Do not blindly overwrite a newer updated Steam binary with an older backup. No global cache deletion is required.

**Steam-game capture is separate:** enable the build manifest in the game's environment, account for runtime path visibility/architecture and existing layers, use a private dump path, prove the mapped library and compare application continuity. Game launch options can affect this game-process layer test. Do not replace `libVkLayer_steam_fossilize.so` by filename alone: layer identity, manifest, exported entry points and embedding expectations require independent checks.

## Shared file-helper completion

[Issue #12](https://github.com/robinwassen/Fossilize/issues/12) adds
`file-helper-<string|buffer>-<open|short-write|flush|close|first-error|success|filesystem>`
and, on Linux, `file-helper-<string|buffer>-dev-full`. Every scenario has explicit
checks active in Release, isolated output and a finite CTest deadline. The per-call
internal adapter tests operation order, raw errors (including unknown zero), first
failure retention, cleanup errno clobbering and exactly one close per opened
handle. It uses no global injection hooks. `file-helper-legacy-compatibility`
uses only the original declarations and exact bool function-pointer types;
`file-helper-legacy-completion` checks both Linux `/dev/full` failure paths.
For original-defect controls, compile that same committed compatibility caller
with the unchanged input `cli/file.hpp` and `cli/file.cpp` in an external directory.
Its normal invocation must pass; `--dev-full-string` and `--dev-full-buffer` must
each fail against the original implementation and pass against the candidate.
Also compile the caller against the unchanged input header and link candidate
`file.cpp`, so the new header cannot mask a source compatibility regression.

The two original bool signatures and symbols are retained. Additive
`write_string_to_file_result` and `write_buffer_to_file_result` expose success,
first failing operation and immediately captured raw errno. Bool failures restore
that first errno after cleanup. Strings retain `strlen` termination and native
text newline translation (`w`); buffers retain exact binary bytes (`wb`). Empty
writes truncate existing files and still flush and close. Explicit completion
checks write, flush and close even when an earlier stage failed; close consumes
the handle even if it fails. No allocation or exception is required in this path.
Callbacks for the internal stdio-style adapter must not throw.

Source compatibility evidence covers the two legacy functions and this preserved
caller. Exact symbols do not establish a general C++ binary ABI guarantee for
static cli-utils or a prebuilt-consumer claim. No wire/file format or machine
output schema changes. Normal writes and initial failures retain their behavior;
formerly false-success flush/close outcomes intentionally return false. Completion
means the stdio/OS interface accepted flush and close, **not power-loss durability**,
atomic replacement or rollback of partial output. No fsync is added. Linux tests
and portable injected close failures do not establish native Windows execution;
record native platform evidence or its unavailable/incomplete status separately.

## Additional platforms, support-data and dependency checks

- **Windows:** native builds/CTest plus real replay/controller/ExternalReplayer/layer tests as applicable. Linux coverage does not test CreateProcess/JobObjects, Windows exceptions, named mappings or cancellation. Use native output paths/loader setup; a Linux Steam ELF replacement is not a Windows procedure.
- **Apple:** core/CLI builds, spinlock tests and available Vulkan portability setup separately; robust replay is disabled. Do not assume Linux Steam/signal procedures apply.
- **Android:** `android_build.sh [Debug|Release]` uses NDK/Ninja and requires `ANDROID_NDK` plus SDK/Gradle setup as applicable. It builds arm64-v8a and optionally the layer APK through `FOSSILIZE_LAYER_APK=ON`; it does **not** execute tests. Use ADB to push/run applicable binaries on a suitable device, configure layer enabling/private writable dump paths, inspect `adb logcat -s Fossilize`, retrieve artifacts and replay. Inspect current script/device requirements; native desktop success does not test Android loader/permissions/signals.
- **Vulkan support-data consistency:** run `extract_vulkan_extensions.py --xml /absolute/pinned/vk.xml --completed-work completed.json` and `extract_vulkan_extensions_sifter.py --xml /absolute/pinned/vk.xml`, saving stdout in scratch files. These report/generate support information, not full automatic extension correctness. Review differences against pinned headers/registry, compile, add positive/negative filter/serialization cases and verify hardware-dependent claims. Do not redirect blindly over source.
- **Dependency changes:** use the dependency's documented test/corpus/build workflow (SPIRV-Tools, SPIRV-Cross, RapidJSON, volk/miniz as applicable) plus Fossilize integration. Top-level CTest does not run every vendored suite; do not label dependency coverage as first-party behavioral verification.

External references: [Khronos layer discovery](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md), [loader environment variables](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md), [CMake presets/workflows](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html), [upstream Linux wrapper](https://github.com/ValveSoftware/Fossilize/blob/master/cli/fossilize_replay_linux.hpp). Local source is authoritative for this checkout; external documentation does not establish a particular installed runtime supports a feature.

## Linux fork-child setup termination

[Issue #56](https://github.com/robinwassen/Fossilize/issues/56) registers 14
`fork-child-*` CTests on Linux with CLI/tests enabled: sigmask, stdout dup2 and
stdin dup2 failures at the first initial slot, last initial slot and recovery;
healthy replay, healthy recovery, parent death, prctl failure and parent-race
controls. The fixture includes the production replay translation unit and calls
`run_master_process`, its actual launcher, shutdown parser and recovery loop.
Only this executable wraps syscalls; the shipped CLI has no injection interface.
Each test has private output, Release-active checks, a 12-second observer deadline,
three seconds for emergency cleanup and a 30-second CTest deadline.

Two known compute pipelines and their shader/layout dependencies are recorded
and read back before replay. Healthy workers execute the null device and update
production shared counters. Recovery sends zero cursors and zero hashes from a fully configured first
worker before its controlled nonzero exit. The real replacement then replays
both pipelines, without claiming prefix reuse or real GPU crash recovery. Atomic
pipe records assert the exact injected child/site/generation, setup ordering,
protocol EOF, controller-owned births, terminal worker status and exactly one
controller reap. Initial and adjacent faults wait at their exact injection
site until the original controller successfully reaps the healthy sibling.
Records assert reap, barrier release and injection ordering; worker exit alone
does not release the barrier. A subreaper checks complete descendant collection; emergency
process-group cleanup runs even when assertions fail. Parent-death readiness is
acknowledged after real PDEATHSIG/mask/redirect setup, before killing the controller.
Inherited stdio, atexit and destructor sentinels distinguish fatal `_exit` from
ordinary worker `exit`. A faulted child attempting a controller fork/epoll operation
is reported and terminated to bound regression controls.

The three fatal setup branches use `_exit(EXIT_FAILURE)`. Adjacent prctl failure
and parent-race branches already use `_exit`; both have controls. Quiet stderr
redirection remains best effort. `exit(run_slave_process(...))` remains the normal
terminal worker path after runtime execution. Parent pre-fork Boolean returns
remain valid. No public API, binary layout or protocol changes are introduced.
The issue #13 controller returns `EXIT_FAILURE` for lost required work and
leaves `progress_complete` zero. These tests preserve that fail-fast policy
and its cleanup. Healthy and healthy-recovery controls require success, complete
progress and two actually parsed/completed pipelines. Initial faults retain one
completed sibling pipeline; recovery faults complete neither pipeline.

Run the full CPU CTest suite in Debug/Release with exceptions OFF/ON. In Debug
exceptions OFF also repeat `ctest --test-dir BUILD --output-on-failure
--no-tests=error -R '^fork-child-' --repeat until-fail:3`. For before-fix controls,
copy the committed candidate source/tests to an isolated directory, replace only
`cli/fossilize_replay_linux.hpp` with master revision
`9804598fba84b747aeecd66c0e873f3f48c4c3cd`, and build the fixture. Retain all
other candidate files, including issue #13 outcome and cleanup changes.
The three `fork-child-initial-first-{sigmask,stdout-dup2,stdin-dup2}` tests must
fail for observed controller fallthrough/worker-owned fork or wrong worker status,
with one intended injection; healthy and healthy-recovery must still pass.
Configuration/build errors and deadlines are not original-defect evidence.

This is CPU-only Linux process evidence, without real driver or Steam claims.
Windows verification is explicitly excluded by the user's #56 run instruction;
no native Windows execution is claimed. Robust master replay is unsupported on
Apple/Android in the current build.

## Robust replay worker outcome

Issue [#13](https://github.com/robinwassen/Fossilize/issues/13) separates a worker's
restart disposition from its retained required-work outcome. Robust controllers
return `EXIT_FAILURE` immediately after an unrecoverable loss, then terminate and
reap remaining children. Immutable assigned graphics/compute/ray-tracing ranges,
observed termination, known skipped culprit, completed prefix and unknown
remainder survive retry-start changes and optional progress forwarding. A clean
retry can succeed after a failed attempt when no required culprit was skipped;
a skipped culprit remains unsuccessful even when its tail completes. Cancelled
required work is unresolved and unsuccessful; existing child termination
mechanisms remain in use.

`replay-controller-{clean,zero,empty-archive,lost,lost-progress,mixed-success-first,mixed-loss-first,
recover,skip-graphics,skip-compute,skip-raytracing,skip-graphics-final,
skip-compute-final,skip-raytracing-final,skip-graphics-progress,
skip-graphics-final-progress,partial,loss-after-retry,held-sibling,
signal,status-error,outcome-state}` use explicit checks active under `NDEBUG`.
`zero` preserves empty selected ranges on the populated known-hash fixture;
`empty-archive` additionally reads back zero resources for every tag in a valid
empty FOZ and runs that archive through the production master.
The dedicated test executable includes the production controller with adapters
compiled only in its translation unit. Actual fork/CreateProcess, pipes,
epoll/native waits, shutdown parsing, restarts and cleanup remain in use.
Barriers select both terminal report orders and hold a still-live sibling;
process status and retained state are the oracle. Per-attempt artifacts record
ranges and item completions, and kernel checks establish reaping/termination.
Progress-enabled controls preserve the legacy clean/dirty crash-counter meanings
independently of final required-work failure; expected sibling cleanup is not
counted as another crash. The state/protocol case also verifies unchanged current message literals and
shared-ring forwarding, unknown native status, and empty-kind recovery.

`replay-controller-cli-{normal,master,progress}` preserve successful null-device
invocations on a dependency-complete known-hash fixture. `replay-controller-cli-
{e5,e6}` use a readback-checked FOZ containing compute `{}`. Debug requires the
actual CLI's unsuccessful exit, including `--progress --quiet-slave`. To reproduce the
original defect, invoke `test/replay_controller_cli.cmake` with `GENERATOR`,
`REPLAY`, `WORK`, `CASE=e5` (then `e6`) and `BASELINE_REPLAY` set to an unchanged
input Debug executable. The runner checks baseline false-success zero separately
from candidate status one. Release exercises a distinct parser-rejection boundary without losing a worker.
At the issue #13 delivery, this fixture exited zero despite rejected required
work; issue [#14](https://github.com/robinwassen/Fossilize/issues/14) supersedes
that behavior with status one for the orderly item failure. The current
`ExternalReplayer::Progress` control still requires exactly one compute parse
failure and skip, zero completed compute pipelines and zero clean/dirty worker
deaths. This Release case verifies item failure, not worker loss. Mandatory
deterministic real-controller loss cases require status one in every
configuration. The E5/E6 worker-loss defect and baseline contrast remain
established by Debug evidence.

`replay-controller-cli-compatibility` exercises `ExternalReplayer::Options{}`,
`start`, `wait`, `is_process_complete`, and cached status for clean and failed
controllers. Kernel terminal-state barriers cover both query-first and wait-first
completion, including a process already reaped by the initial progress poll.
Progress pumping after a known terminal status does not reap it a second time
or overwrite the unsuccessful result with `-ECHILD`. Compile
`test/replay_controller_compatibility_test.cpp` with the
input public headers first on its include path and link candidate libraries for
old-header source compatibility evidence. Existing diagnostic compatibility
fixtures retain recorder/database and ordinary CLI checks. Issue #13 preserved
public declarations, shared progress layout/version and worker grammar while
making unresolved robust replay return nonzero, including through
`ExternalReplayer::wait`. Issue #14 preserves those public declarations and
shared progress layout/version, plus existing progress/resume message semantics,
and adds the internal `REPLAY_FAILED <reason-mask> <drained>` terminal record for
orderly item failures described above. These compatibility checks establish
preserved caller source compatibility, not a general binary ABI, driver/GPU,
output durability, full item-ledger or whole observability-standard guarantee.
Apple/Android `NO_ROBUST_REPLAYER` paths remain outside this contract.

Register the same portable controller cases on native Windows. Native Windows
execution for the initial issue #13 run was explicitly waived by the user and
remains unverified; Linux results do not establish Windows process/IPC behavior.
Record subsequent platform/configuration evidence separately.


## Replay creation hints

Issue [#15](https://github.com/robinwassen/Fossilize/issues/15) narrows
`--replayer-cache <path>` to optional creation hints in
`path.replay-v1.<null|vulkan>.<pipelineCacheUUID>`. The UUID encoding is retained.
The concurrent database writes `.N.foz` fragments and reads a harvested `.foz`
base; ordinary explicit harvesting (for example `fossilize-merge-db`) is still
required before fragments can suppress later selection. UUID-only legacy files
and unknown policy namespaces are ignored and left untouched, intentionally
causing a one-time replay when switching from legacy hints. No automatic migration
or stronger provenance is assigned to old entries.

A v1 hint means every requested creation iteration succeeded, at least one
iteration ran, and no validation error callback was observed for that item, in
the selected device mode and pipeline cache UUID context. A permitted
compile-required probe followed by successful fallback completes one iteration;
a skipped or failed fallback does not. Failure remains sticky across `--loop`
iterations. Zero iterations retain the existing invocation status but cannot
write pipeline creation or pipeline validation hints (shader setup may still
write its own validation whitelist entries). Validation whitelists remain independent
observed-no-validation-error hints under their existing success gate, including
prior explicit/implicit entries; they neither select compiled output nor prove
that validation actually ran. Hints never establish driver-output persistence,
flush/close durability, successful hardware compilation in null mode, or an
observed driver cache hit. Cache export/finalization is a separate contract.

`write_entry` rejection is visible as `Replay/marker write rejected`, with tag
and hash and the unavailable optional hint. It does not change a successful
Vulkan creation into a compilation failure or change the optional-output CLI
success policy. The Boolean only reports write acceptance; `flush()` has no
result and supplies no durability evidence.

Nine registered `replay-cache-{creation-rerun,success-policy,loop,compile-required,
write-failure,legacy-provenance,null-provenance,whitelist,legacy-invocation}` cases
extend the production-including outcome fixture. They assert real recorder FOZ
hashes/payloads, actual worker creation attempts and outcomes for all three
pipeline kinds, teardown/reopen, explicit fragment harvesting and actual
suppression by the successful control. Release-active checks cover final dispatch,
loop/fallback counters and flags, retained library handles and cleanup, independent
per-item state, zero attempts, rejecting adapters and a missing-parent filesystem
failure, untouched legacy base/fragments, unknown policy namespaces, and independent
whitelist use. The Vulkan-mode provenance check retains production device options
but privately adapts device initialization to null dispatch; it is CPU control-flow
evidence, not physical Vulkan/driver evidence. Ordinary CLI cache and validation
whitelist invocations remain registered alongside existing compatibility tests.

Run the full CPU CTest matrix in Debug/Release with exceptions OFF/ON and SPIR-V
validation ON, retaining discovered/executed IDs. For original-defect controls,
copy the committed fixture and candidate source to an isolated tree, replace only
`cli/fossilize_replay.cpp` with input revision
`a6b22d65badc61907060028514dc5e86c5ce6c3e`, and build `replay-outcome-test`.
`cache-write-failure`, `cache-legacy-provenance` and `cache-loop` must fail explicit
`M15` runtime assertions (rejected-write visibility, current-policy namespace and
zero-iteration gate). Build failures, deadlines and crashes are not defect
reproduction. The unchanged input already contains issue #14 creation-failure
gating: its `creation` control must pass and is not a newly fixed #15 defect.
Preserved old-header caller compilation/linking and the existing diagnostic and
controller/outcome compatibility tests provide source/invocation evidence, not a
general binary ABI guarantee. Native Windows execution is explicitly excluded by
the user's run instruction and remains unverified; no GPU or Steam claim follows
from this Linux CPU matrix.


## Replay timeout causality (issue #16)

`--timeout-seconds` measures the monotonic duration since the dispatcher last
observed a completion in the memory context it is waiting for; zero disables
that work deadline. It does not identify an individual pipeline's elapsed compile
time. Linux heartbeat supervision retains its existing 10-second threshold;
post-crash cleanup retains its existing 3-second threshold on Linux and Windows.
Each trigger stores the configured threshold and actual elapsed duration in
nanoseconds, including overshoot. Work progress, heartbeat and cleanup have
independent start times. Cleanup notices cannot extend an existing cleanup deadline.

Private controller state retains the current attempt plus the first four prior
attempts, with an explicit count of omitted attempts. Each attempt has three
trigger slots and at most four termination requests, plus an omission count.
The original trigger, signal/termination request, adapter acceptance or raw native
error, observed native terminal status, recovery disposition and required-work
outcome are separate facts. An accepted syscall is not proof of termination.
An unexplained SIGKILL has unknown cause; it does not establish timeout or OOM.
Multiple active worker threads do not establish an individual culprit; independently
reported resource evidence remains usable. A later successful retry retains the
earlier attempt's evidence without making that timeout permanent work failure.

Run the registered `replay-timeout-unit-*` and Linux `replay-timeout-controller-*`
cases with `ctest --test-dir BUILD --output-on-failure --no-tests=error -R
'^replay-timeout-'`, then repeat with `--repeat until-fail:3`. Run the full CPU
suite in separate Debug/Release, exceptions OFF/ON configurations as above. All
build/test/probe commands require the published-intent verification lock described
in Preparation. Preserve CTest JSON discovery, actual executed IDs and logs.

Portable units use explicit fake times and termination adapters. The Linux
fixture includes the production dispatcher/controller, creates and reads back a
known dependency-complete FOZ, uses real creation threads, fork, pipes, epoll,
signal handlers, shutdown and kernel reaping, and substitutes only private test
Vulkan/syscall/clock adapters. Pipe barriers order creation, cause delivery,
abort, hung teardown and escalation. No wall-clock sleep determines event order;
an emergency watchdog only bounds broken fixtures. Direct ordinary CLI controls
also exercise timeout and failed abort delivery without a controller. Mutation
controls must compile and fail explicit `M16/` assertions; compilation failure,
watchdog expiry and crashes are not original-defect evidence.

The internal child protocol adds bounded, strictly decimal `TIMEOUT 1
<threshold-ns> <elapsed-ns>` and `TERMINATE <mechanism> <delivery> <native-error>`
records. Only the child's work-no-progress origin is accepted from the pipe;
heartbeat/cleanup origins come from controller clocks. Old CRASH, resource,
heartbeat and REPLAY_FAILED records keep their meaning; an absent or malformed
extension leaves cause unknown. Public source declarations and shared progress
version/layout are unchanged. Old-header ExternalReplayer callers and ordinary
CLI controls remain compatibility gates; no general prebuilt C++ binary ABI
claim is made. No test injection options or hooks are installed in the CLI.

Windows uses the portable deadline/evidence model for no-progress recovery and
its existing cleanup TerminateProcess path; it gains no heartbeat watchdog.
Native Windows execution for issue #16 is explicitly user-waived and unverified.
Portable test registration and Linux evidence do not establish native Windows,
GPU, driver compilation, Steam presentation or cache persistence behavior.
