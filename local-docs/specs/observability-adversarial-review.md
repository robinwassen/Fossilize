# Adversarial review of the observability standard

27 October 2026. Three independent sub-agents reviewed the nested-bullet standard, then rechecked the revised wording. Their scopes were CLI conventions, library/capture boundaries, and concurrency/outcome accounting. They read the project guidance and documentation; none inspected implementation or edited files. The primary agent adjudicated their findings and revised the standard.

This tests the proposed principles against counterexamples. It is not a runtime test, a current-implementation compliance audit, or proof that every future edge case is covered.

## Accepted findings and corrections

| Gap in the original bullets | Counterexample | Correction |
|---|---|---|
| Exact counts assumed complete evidence | A worker completes unknown work before losing its report. | Report known counts and unknown affected scope; never fabricate precision. |
| Coverage assumed an enumerable total | Database discovery stops after 80 items, with unread scope unknown. | Report discovery completeness separately; 80 accounted-for items cannot establish full coverage. |
| Causality did not explicitly cover dependencies and cancellation | One invalid shader blocks 500 pipelines; user cancellation intentionally kills workers. | Distinguish originating faults, blocked/unattempted work and intentional cancellation. |
| Event-order independence did not explicitly cover stale attempts | Attempt 2 succeeds before a delayed failure report from attempt 1 arrives. | Reconcile by logical work and attempt; duplicates cannot inflate counts and stale attempts cannot overwrite authoritative outcomes. |
| Optional sinks lacked an interaction contract | A callback throws, blocks, reenters under a lock or retains borrowed data. | Define callback threading, lifetime and reentrancy; contain diagnostic failures and bound blocking. |
| Finalization could hide errors in destruction | An accepted async write fails only at flush/close. | Explicit finalization exposes errors before nonthrowing destruction; capture status does not depend on logs. |
| Structured-result availability was ambiguous | A converter writes its payload to stdout, then appends a JSON summary. | Explicit machine mode/destination keeps reports parseable on success and controlled failure without mixing payloads or prose. |
| Final-result delivery was an absolute promise | Cancellation occurs while the report destination is blocked or full. | Bound reporting and cleanup; attempt an authoritative result, and treat missing/truncated evidence honestly. |
| Schema versioning alone did not preserve contracts | A reason identifier changes meaning without a version increment. | Preserve identifier, field, unit and exit-status semantics; document breaking changes. |
| CLI and child signal semantics were insufficiently separated | A child receives SIGABRT, while the controller continues successfully. | Apply termination conventions to the CLI itself; child signals remain evidence in the aggregate outcome. |
| Durability and diagnostic loss were underspecified | Close succeeds but power-loss durability was never established; sampled logs miss failures. | State the promised persistence assurance, keep outcome accounting independent of sampled logs and expose diagnostic loss. |

Smaller clarifications add tool/library version, event severity/time, actionable support-boundary explanations and restraint around sensitive payloads. Severity remains separate from aggregate success. These additions require neither remote telemetry nor an observability SDK.

## Scenario rechecks

Each counterexample now has a determinate, honest outcome under the revised principles:

- **Lost worker:** preserve known range and completed work; mark unresolved counts unknown and required coverage incomplete.
- **Discovery failure:** distinguish 80 discovered items from the unknown unread scope; incomplete required coverage returns nonzero.
- **Dependency failure:** one shader validation fault may affect 500 consumers; do not report 501 independent compiler faults. Category counts may overlap.
- **Stale retry:** timeout attempt 1, success attempt 2, then late failure attempt 1 gives one successful final item plus timeout history, independent of delivery order or duplicates.
- **Cancellation:** retain completed/persisted results, identify intentional termination of remaining work and follow platform cancellation conventions.
- **Failing callback:** diagnostic failure does not escape a Vulkan interception boundary or indefinitely stall capture; capture status remains available independently.
- **Late I/O error:** successful queue acceptance followed by failed close produces an incomplete recording outcome through explicit finalization.
- **Machine-mode failure:** available result channel contains structured failure data; exported payload is not contaminated. A broken channel is reported through remaining available evidence, without an impossible delivery guarantee.
- **Logging overload:** sampled/dropped diagnostics do not decide correctness; disclose diagnostic loss and any unavailable accounting.
- **Opaque driver persistence:** unknown driver-cache durability does not invalidate a contract that never promised it; failure to deliver a promised output does.

All three reviewers re-read the revised standard and reported their material findings addressed, with no new material contradiction in their review scopes. This is bounded adversarial review, not unanimous proof of completeness.

## Decisions retained and details deferred

- Retained six top-level rules, nested bullet form, aggregate exit statuses, explicit unsupported outcomes, quiet-mode failure summaries, and separation of replay/persistence/reuse evidence.
- Kept `1`/`2` as a suggested CLI baseline rather than a universal numeric mandate. No exit code per detailed cause is needed.
- Deferred callback ABI, exception-versus-result API choices, wire schemas, queue sizes, exact synchronization/durability guarantees, redaction mechanics and platform-specific cancellation mappings. These require later specifications, not more top-level principles.

## Sources and limits

[GNU libc](https://sourceware.org/glibc/manual/latest/html_node/Exit-Status.html) supports broad exit statuses and distinguishes exit from termination status. [Pigweed CLI style](https://pigweed.dev/style/cli.html) supports stream separation and stable exit-code contracts. [CLI Guidelines](https://clig.dev/) supports explicit machine-readable output, interface stability and prompt interruption; the exact result-delivery contract is a project design choice.

[OpenTelemetry error handling](https://opentelemetry.io/docs/specs/otel/error-handling/) provides analogous guidance for containing callback and background diagnostic failures. [C++ Core Guidelines C.36/C.37](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rc-dtor-fail) supports nonfailing destructors. Neither source mandates a particular Fossilize API or SDK. Accounting, dependency and retry corrections follow from the counterexamples and the project's stated properties.
