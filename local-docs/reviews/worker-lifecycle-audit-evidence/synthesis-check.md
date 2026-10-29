# Final independent synthesis check

Checker: lifecycle_rules agent. Date: 2026-10-29.

Reviewed local-docs/reviews/worker-lifecycle-audit.md; worker-lifecycle-audit-evidence/reproduce.py; cancel_actual.cpp, owner_lock.cpp, external_probe.cpp; relevant actual Linux API source; and /tmp/fossilize-lifecycle-final-evidence/results.json.

**Verdict: approved; no required corrections.**

- All 13 retained results match their declared expected observations. Three exit1 cases are explicitly identified as product-expectation violations rather than passing product tests.
- Cancellation finding accurately says actual production functions with real exited child and synthetic recovery markers; actual replacement/event-loop/Steam occurrence is correctly limited to inspection, not claimed executed.
- Bootstrap uses exact helper mechanism plus independent reader-close negative control; stale-event race is explicitly modeled and conditional, with no natural occurrence claim.
- Interrupted wait, status overwrite, unrelated child reap, temporary-thread parent death, FD leak and inherited endpoint findings match the API probe code and source branches. The inherited endpoint finding does not invent a destructor reap/termination contract.
- Owner-lock finding uses actual lock and protocol memory; forceful death in the critical section is deliberately controlled, not attributed to ordinary driver crashes.
- Windows missing heartbeat, abandoned mutex and fail-open containment findings match the independently reviewed source. Native Windows execution is explicitly absent; nested outer-job fallback is retained.
- Once-only start and inherited-process-group kill preconditions, healthy cancellation, normal reaping, and ordinary current Linux parent-death cleanup are correctly recognized instead of reintroduced as blanket defects.
- Apple/Android, ordinary versus robust replay, GPU/Steam deployment, and full-build/test limitations are clear. No performance or user-issue frequency inference exceeds evidence.
- The report's audit is a code-path inventory plus focused probes, not a formal proof of all possible interleavings; its explicit evidence labels and limitations make that scope clear.

No new tests run for this final synthesis check; earlier independent checker probes and source review remain recorded in linux-checker.md.
