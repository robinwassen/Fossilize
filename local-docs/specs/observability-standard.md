# Fossilize observability standard

Proposed · 27 October 2026 · Applies to the C++ library, Vulkan capture layer and CLI tools, independently of current logging. **A caller must be able to determine what was requested, what happened, what remains incomplete, and what results were preserved—without parsing prose or enabling debug logs.**

- **1. Return outcomes; let the host own presentation.**
  - Library outcomes expose stable reason identifiers, affected scope and underlying error details where known; a boolean is insufficient when unsupported, failed and incomplete work require different responses.
  - Async acceptance means queued. Provide explicit completion/finalization that reports background and write/close errors before nonthrowing destruction; capture completeness must remain queryable without a logging sink.
  - Diagnostics use an optional caller-controlled sink. Define callback threading, lifetime and reentrancy constraints; contain sink errors and avoid unbounded blocking or recursive reporting.
  - Recoverable errors must not make the library unilaterally write to standard streams or terminate its host. Capture reports recording loss separately from the application's Vulkan result and preserves application continuity wherever safe.

- **2. Define success against an explicit scope and policy.**
  - Report selected inputs, filters and compatibility policy. State whether discovery and the workload total are complete; unknown scope must never appear as zero work or 100% coverage.
  - Account for each discovered selected item once as succeeded, verified reusable, unsupported, failed, or unfinished/unknown; identify deliberate exclusions separately. Explain dependency-blocked or cancelled work without attributing a fault to an unattempted item.
  - Distinguish execution completion, coverage and required-output persistence. Finishing a queue or surviving a worker crash is not proof of success.
  - Unsupported work remains visible; excluding it from success requirements requires an explicit, reported policy. Recovery permits success only after every required work and output obligation is satisfied.

- **3. Use exit status for the aggregate outcome, reason codes for detail.**
  - CLI exit `0` means the declared contract was satisfied; partial, failed, cancelled or indeterminate required work returns nonzero. A small documented set is sufficient: `1` for unsuccessful work and `2` for invalid invocation is a suitable baseline.
  - Do not assign an exit code to every reason, return an error count, or select whichever worker failed last. Report all observed categories with known affected counts; mark unknown counts and overlapping categories explicitly.
  - Preserve platform cancellation/termination conventions for the CLI itself; report child signals as evidence in its aggregate outcome. Library operation results are independent of host process exits.

- **4. Correlate concurrent work and preserve causality.**
  - Associate records with run, input/resource, worker and attempt as applicable. Distinguish originating failures from downstream effects and attempt failures from final item outcomes.
  - Reconcile by logical work and attempt despite duplicate, delayed or reordered reports; stale attempts must not overwrite authoritative outcomes or double-count work. Lost reports produce explicit uncertainty, never invented exact counts or success.
  - Record trigger, termination mechanism, recovery action and final effect separately. A controller timeout followed by SIGABRT establishes a timeout, not a spontaneous driver crash; unknown attribution stays unknown.
  - Attempt one authoritative final result during orderly shutdown, with bounded reporting and cleanup. Missing or truncated final results leave the outcome incomplete/unknown for consumers lacking other authoritative evidence.

- **5. Make output composable, bounded and actionable.**
  - Stdout carries requested command data; stderr carries human diagnostics and progress. Offer an explicit structured-result mode/destination independent of verbosity; keep it parseable on controlled failure and success, separate from payloads, prose and terminal controls.
  - Human and structured output express the same facts. Version schemas and preserve the meanings of reason identifiers, fields, units and exit statuses; document breaking changes.
  - Quiet suppresses routine chatter, not a compact unsuccessful outcome, affected scope and reason summary. Explain known support boundaries and useful recovery actions without inventing advice.
  - Baseline context includes tool/library version, operation/phase, duration, relevant device/driver and deadline settings, plus raw Vulkan/OS errors where known. Events include severity and time; severity does not determine run success.
  - Aggregate repeated diagnostics with bounded examples; retain authoritative outcome accounting independently of sampled logs and expose diagnostic loss. Expensive tracing is opt-in; avoid unsolicited shader contents, secrets or sensitive paths in baseline reports.

- **6. State what persistence and reuse evidence actually proves.**
  - Distinguish object creation, output write/close completion, any promised durability guarantee, and observed cache reuse. Identify input/cache and compatibility context; successful replay alone proves neither durable output nor a driver-cache hit.
  - Report failed or unknown persistence against the declared output obligation. Required result/output delivery failure makes the operation unsuccessful; optional logging loss is separately visible where possible and must not change Vulkan behavior.
  - Fossilize reports observable facts; integrators such as Steam own scheduling reasons and retain/present the result, including uncertainty about reuse.

Conventions: [GNU C Library exit-status guidance](https://sourceware.org/glibc/manual/latest/html_node/Exit-Status.html), [Pigweed CLI style](https://pigweed.dev/style/cli.html), and [CLI Guidelines](https://clig.dev/). [OpenTelemetry error handling](https://opentelemetry.io/docs/specs/otel/error-handling/) informs diagnostic-failure containment; adopting its SDK is not required. Outcome accounting and concurrency rules are project-specific design decisions. See the separate adversarial review for counterexamples and rationale.
