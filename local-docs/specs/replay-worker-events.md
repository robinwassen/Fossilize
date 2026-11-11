# Private worker reason transport

Issue [#18](https://github.com/robinwassen/Fossilize/issues/18) adds private scalar
facts alongside the legacy child protocol. This is not a public structured-result
API, a complete item ledger or a general mixed-version interoperability guarantee.
Public declarations and SharedControlBlock version/layout remain unchanged.

Every rich frame is printable ASCII `FZ18 <decimal-payload-byte-count>:<payload>\n`.
The fixed decoder and serializer hold fewer than 1024 bytes per frame (including
the line terminator); no incoming length causes allocation. Decimal integers
are checked for overflow. LF is the only terminator; NUL, CR and non-ASCII bytes
invalidate the record. Malformed or oversized input is discarded through LF;
subsequent frames can be retained, with sticky loss/unsupported flags. EOF
invalidates an unfinished record and records missing HELLO/FINAL uncertainty.

Startup writes `1 OFFER <version-bitmask> <worker> <attempt>` on the existing
stdin pipe, before optional legacy shader hashes and EOF. Both slave startup paths
read explicit bytes with a fixed line cap and require LF before selecting an
offer; EOF prefixes, NUL, CR, non-ASCII, junk and oversized lines fail before
HELLO or replay. Windows stdin uses binary mode to preserve invalid bytes.
Legacy hash lines are bounded hexadecimal values; the first remains available
when no offer is present. Linux contains EPIPE
for both writes without changing the caller's SIGPIPE disposition, mask or
pre-existing pending signal; a closed reader remains a native delivery failure
with unknown worker origin. Bit 1 offers version
1. A child selects an offered version with `1 HELLO <worker> <attempt>` on its
output pipe before replay. No extra blocking exchange is needed. Missing,
duplicate, unsupported or mismatched selections cannot certify a rich result.
An absent offer preserves the first legacy shader hash and direct slave behavior;
that invocation has no claim of negotiated reason coverage. Old CRASH, resource,
validation, heartbeat, TIMEOUT/TERMINATE and REPLAY_FAILED records retain their
meanings and optional shared-ring forwarding.

The version 1 event payload is `1 EVENT phase reason domain known raw tag hash
worker attempt begin end offset threshold elapsed`, all decimal. `raw` is signed
64-bit; other fields are unsigned 64-bit. Stable phases are parse=1, create=2,
timeout=3. Stable reasons are generic parse=1, invalid JSON=2, incompatible
version=3, malformed envelope=4, invalid shader=5, invalid record=6, creation
failure=16, work no progress=32, controller heartbeat=33, controller cleanup=34.
Raw domains are Vulkan=1, RapidJSON=2, native OS=3. Unknown IDs are retained opaque
with unsupported context; they never become known categories or success.

Known bits are raw=1, tag=2, hash=4, worker=8, attempt=16, range=32, parser offset=64,
duration=128. Unknown fields must encode zero, without setting their bit. Known
zero values and empty half-open ranges remain distinct from absent values.
Offsets are input bytes; threshold and elapsed are monotonic nanosecond durations.
Syntax failures alone carry RapidJSON code/offset. Other controlled parser errors
carry stable reasons without invented raw codes. Parser captures are scoped TLS;
Impl dependency detection preserves the first origin, public reentrant calls
have separate boundaries, explicit nested captures retain their own results,
and later valid calls clear a reused receiver. No shader payload is copied into events.

Actual parse and creation detectors emit before final reporting, independently
of verbosity. The healthy dispatcher emits no-progress threshold/elapsed before
termination. Controller heartbeat/cleanup clocks and native delivery/status facts
remain separately available in the private timeout evidence model; a signal is
not itself a cause. An individual hash is absent for no-progress events; legacy
crash resource evidence may independently identify one. Assigned ranges and the
controller's worker/attempt are controller-known context in ReplayWorkerOutcome,
not fabricated worker-reported event fields.

A worker serializes up to 16 first event frames under its ordinary-thread mutex;
additional observations are counted as omitted. Each bounded frame is a complete
pipe write (below Linux PIPE_BUF); Windows message fragments are fed to the same
length-aware stream decoder, including ERROR_MORE_DATA. Asynchronous crash handlers
retain their small legacy writes and never enter event locks or allocate. The
controller stores 16 events for the current attempt and the first four prior
attempts, with omitted-attempt counts. This is bounded evidence, not exact logical
item accounting. Separate legacy failure summaries remain authoritative.

Orderly shutdown attempts `1 FINAL <failure-0-or-1> <omitted-event-count>` after
producer threads drain. Rich successful completion requires HELLO, one valid FINAL,
no observed failure/loss/unsupported context, and independently successful native
termination. Empty assignments negotiate and finalize without initializing Vulkan, preserving
the controller's original no-device zero-work behavior. FINAL cannot override a
nonzero or abnormal process status. Missing final evidence preserves received
origins and leaves unresolved work unsuccessful. A later successful retry retains
the earlier attempt's evidence.

Linux verification uses the registered replay-event unit/producer/controller
cases, the existing timeout/controller/outcome/diagnostic regressions and actual
input-header callers. Run the complete CPU inventory in Debug/Release with
exceptions OFF/ON; run bounded codec/parser/producer cases under ASan+UBSan.
Native Windows execution was explicitly waived for this issue and remains
unverified. Portable registration and Windows source changes are not Windows,
GPU, driver-cache, persistence or Steam runtime evidence.

## Support and dependency details (issue #19)

The paired private protocol additionally accepts `1 DETAIL 1` records. The first
number is the negotiated protocol version and the second is the detail schema
version. The remaining decimal fields are `phase reason domain known raw tag hash
worker attempt support-kind support-known required available support-raw parent-tag
parent-hash parent-status parent-category parent-omitted identifier-hex`. `identifier-hex` is a
bounded ASCII identifier encoded as lowercase hexadecimal, or `-` when absent.
The fixed frame capacity is now 1024 bytes, including framing and LF, and the
identifier holds at most 159 bytes. A frame remains below Linux PIPE_BUF; the
stream decoder never allocates from an incoming length. Existing EVENT 1 payloads
and their known bits retain their meaning. A previous decoder treats DETAIL as
unsupported context; rich mixed-version interoperability is not promised.

Support phase=4/reason=48 carries a caller-owned support value. Kinds are feature=1,
extension=2, property=3, unknown sType=4, SPIR-V capability=5, malformed input=6 and
constraint=7. Kind zero means no rejection. Support-known bits are required=1,
available=2 and raw=4; unset values encode zero. A known zero is distinct from an
unknown value. Feature and extension checks report required/available booleans;
property checks report the recorded requirement/value as required and the device
or replay boundary value as available. The identifier defines a maximum, minimum,
mask or alignment relation, not always a less-than comparison. Input-only
constraints name their relation and compare its input values. Capability
and unknown-sType checks retain their raw numeric IDs. Identifiers name existing
Vulkan fields, extensions, SPIR-V capabilities or constraints; they do not imply
an extension can be enabled on the current device. Malformed shader input is
reported as parse failure by replay, rather than Unsupported.

Dependency phase=5/reason=49 names the consumer using tag/hash and its first
blocking parent using parent-tag/hash. Parent status is unknown=0, missing=1,
failed=2 or read failed=3. Parent-category zero means unavailable; otherwise it is
one plus the existing ReplayFailure category index (read, parse, create,
unsupported, validation, setup, dependency). Parent-omitted counts additional observed blocking references after the first;
it is independent of omitted event frames. A blocked consumer has no fabricated
Vulkan result. Initial static parsing, shader resolution, derivative-base and
pipeline-library scheduling retain source hashes before handle replacement.
Lazy maintenance4 creation failures name the static parent, and its consumer is
blocked before the pipeline create call.

`1 COUNTS 1 read parse create unsupported validation setup dependency` carries
seven independent unsigned attempt-local observation counts at orderly shutdown.
It is sent separately from sampled detail and before the unchanged FINAL record.
A failing count cannot be erased by FINAL success. Counts overlap by design. Cached static-parent failures retain one originating
observation per source identity within the attempt; these are not counts of every
repeated native static-create invocation. Dependent consumers are observed
separately. These counts are not a reconciled logical-item ledger across worker
processes or retries.
Missing COUNTS leaves these counts unknown; missing FINAL retains uncertainty.
The existing 16-event and four-prior-attempt retention limits still apply, with
explicit omitted-event counts. Source resource caches hold operational failure
status separately from these bounded diagnostic examples; the bounded 64-origin
sample is only a fallback for parent category context, never an accounting or
queue-completion mechanism.

FeatureFilter preserves its original bool entrypoints and adds value-output
overloads. Each call clears its receiver and owns its identifier bytes. Lexical
thread-local receiver scopes isolate concurrent checks and restore nested calls;
there is no shared mutable last-error or new callback sink. Parser dependency
captures are private lexical adapters; queued work retains scalar identities, not
pointers into a capture or allocator. Public StateCreatorInterface virtual
signatures and shared-progress layout/version remain unchanged. Old-header
compile/link/run checks describe the preserved source and bool behavior boundary,
not a general prebuilt C++ ABI guarantee.

Dependency prose recommends a common cache only when an input resolver confirms that the referenced resource is absent. A missing handle-map entry alone is unresolved; a known null handle is failed. Present unreadable inputs retain read-failed status, and known originating parse/create failures remain attached to the parent.

## Ordinary support/dependency text

Ordinary replay retains at most 32 semantic warning causes per worker attempt,
with two examples per cause subject to eight examples globally. Repeated hashes
and differing requested limits coalesce by predicate; enum/capability/sType IDs
and generic constraint resource contexts remain distinct. Each retained cause
has fixed per-resource counters; an overflow counter counts additional replay
observations, not distinct omitted causes. Counters saturate explicitly at
`UINT64_MAX`, reporting lower bounds rather than wrapping. These are attempt
observations (including repeats), independent of structured category counts and
bounded event retention, never a global unique-item ledger.

Collection uses fixed arrays and a mutex, without per-observation diagnostic
allocation or output. Up to 64 source support contexts can explain blocked
consumers; context not retained remains explicitly unknown. Source and parent
hashes and known required/available values appear only in bounded examples.
After producers drain, finalization emits one report of at most 42 warning lines
and 32768 bytes: one header, 32 causes, eight examples and one overflow line.
Finalization and destruction share an idempotent guard, including standalone
replay without an event writer. Empty and success-only attempts emit no report.
Quiet workers retain stderr suppression. Hard exit/crash handlers do not flush
or certify completion; reporting is best effort on ordinary shutdown.

Private replay dependency captures suppress only replaced missing/invalid-parent
parser diagnostics. Detailed FeatureFilter calls suppress unsupported detector
prose in favor of the report; bool-only callers preserve legacy diagnostics.
Unrelated parse, validation, OOM and driver errors retain ordinary reporting;
the bound applies to the support/dependency report, not all stderr.
