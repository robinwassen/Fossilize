# Worker lifecycle audit evidence

See [main report](../worker-lifecycle-audit.md). Research date: 29 October 2026.

`reproduce.py` builds into a new directory supplied via `--out`; no production edits, Steam execution, or GPU work. `external_probe.cpp` and `owner_lock.cpp` link the unmodified ExternalReplayer implementation. `cancel_actual.cpp` includes the production replay source with its main renamed and dead sections discarded; it calls actual cancellation/recovery functions with controlled recovery state and real process exit ordering. Other C++ files model selected supervisor mechanisms using extracted production methods; they are not full controller runs. Controls distinguish cancellation of a live worker from an already-exited worker and remove the redundant-reader bootstrap failure mechanism.

The `*-inspector.md`, `*-checker.md`, `external-extra-check.md` and `rules.md` files retain agent research, path matrices, independent verdicts and evidence qualifications. Their `/tmp/...` references are provenance from the original investigation; the retained probe source names and runner here provide the durable reproduction path. Final-run `results.json`, `metadata.json`, and `build.json` retain measured observations, source hashes and exact commands. Build output paths are historical scratch paths and should be replaced by a new `--out` directory when reproducing.

Expected defect observations do not establish a passing product suite. Windows findings are source-only. No observed issue is attributed to a particular Steam report or installed client version.
