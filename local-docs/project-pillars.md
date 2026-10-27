# Fossilize: desired project properties

Draft principles for the library, capture layer and CLI tools. These describe desired behavior, not guarantees about the current implementation.

## 1. Correctness

**Promise:** Record and replay supported Vulkan state faithfully. Make representation and compatibility limits explicit; do not silently approximate unsupported state or present incomplete work as successful.

**Violation:** Silent alteration or corruption, or unsupported work reported as successfully processed.

## 2. Failure containment

**Promise:** Application continuity takes priority over capture completeness. When recording fails or encounters unsupported state, preserve the application's Vulkan behavior wherever safely possible and clearly mark the recording incomplete. Contain recoverable worker failures and preserve valid existing data.

**Violation:** A recording failure unnecessarily terminates or changes the application's operation; one worker failure unnecessarily brings down the controller; interrupted work is presented as complete.

## 3. Efficiency

**Promise:** Complete useful work with bounded, measurable CPU, memory, I/O and application overhead. Parallelize where beneficial and reuse compatible prior work where available. Evaluate capture overhead and replay performance separately.

**Violation:** Avoidable repeated work, unbounded resource growth, or throughput improvements that unacceptably degrade the running application. Keeping every worker busy is not itself a success criterion.

## 4. Diagnosability

**Promise:** Provide small, actionable baseline diagnostics by default, with expensive tracing opt-in. Make outcomes, known unsupported cases, failure categories, affected work, completeness and recovery actions visible. Distinguish observed facts from inference and explicitly identify unknown context.

Provide human-readable logs and structured records expressing the same facts. Unsupported work must be distinguishable from both successful processing and crashes; identify the unsupported feature or compatibility boundary when known.

**Violation:** An observed support limitation disappears silently, or a routine failure requires a debugger to determine its category, affected work or recovery outcome.

## Enabler: regression verification

Important behavioral promises should have repeatable automated checks. Fixes should include regression coverage where practical; hardware-dependent guarantees should identify the environments in which they are verified.

## Using these principles

Protect application continuity when capture cannot be completed faithfully; report the loss of completeness. Observability enables actionable user reports, measured engineering and automated analysis. Structured output supports optional aggregation without requiring automatic collection or uploading.

Specific resource budgets, support policies and diagnostic schemas belong in subsequent specifications.
