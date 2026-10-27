# Sweep evidence

Research against `556da9ff7d7616704dda515756219ab1dab693ec`, Linux, 27 October 2026.

- [Raw observations](results.json): original command arguments, process statuses, stdout and stderr. Negative status `-6` is Python's representation of termination by SIGABRT. A positive child code `2` elsewhere is internal protocol, not a proposed public invocation status.
- [Probe source](probe.cpp): rejecting database mock, recorder acceptance/finalization experiment, `/dev/full` file helpers and outer-valid archive generation.
- [Reproduction runner](reproduce.py): direct isolated compilation and the same experiments, including the existing `fossilize-test`. Does not modify repository source. Requires Linux, `g++`, Python 3, populated repository dependencies and `/dev/full`.
- [Build/test metadata](metadata.json): original revision, compiler and baseline system-test exit.

```sh
python3 local-docs/reviews/observability-sweep-evidence/reproduce.py \
  --repo /absolute/path/to/fossilize \
  --out /tmp/fossilize-observability-reproduction
```

Run from the repository root, replace the repository placeholder with its absolute path, and use a fresh output directory. The runner suppresses core dumps and runs produced tools inside that directory. It compiles with assertions enabled, `-O0`, no CMake-generated settings and no SPIR-V validation. These are Linux probes; no production GPU or Vulkan loader is needed for null-device tests. It deliberately records defects rather than treating observed false-success statuses as a passing conformance suite. Process IDs, timings, allocator statistics and thread counts may differ between runs.

The original `fossilize-test` was separately compiled with the same core objects and returned 0. That establishes that these probes can expose gaps despite the current system test passing; it does not establish full project correctness. CMake and cross-platform test suites were not run because CMake is absent.

The standard is a requested design target, not current implementation guarantees. See the [sweep](../observability-sweep.md) for ordering, source links, adversarial acceptance criteria and runtime/source-only distinctions. The raw observations retain the original scratch paths under `/tmp/fossilize-observability-probes`; those temporary binaries are not included. The retained sources here are sufficient to rebuild them.
