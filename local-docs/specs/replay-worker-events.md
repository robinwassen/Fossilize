# Private worker reason transport

Issue [#18](https://github.com/robinwassen/Fossilize/issues/18) adds private scalar
facts alongside the legacy child protocol. This is not a public structured-result
API, a complete item ledger or a general mixed-version interoperability guarantee.
Public declarations and SharedControlBlock version/layout remain unchanged.

Every rich frame is printable ASCII `FZ18 <decimal-payload-byte-count>:<payload>\n`.
The fixed decoder and serializer hold fewer than 512 bytes per frame (including
the line terminator); no incoming length causes allocation. Decimal integers
are checked for overflow. LF is the only terminator; NUL, CR and non-ASCII bytes
invalidate the record. Malformed or oversized input is discarded through LF;
subsequent frames can be retained, with sticky loss/unsupported flags. EOF
invalidates an unfinished record and records missing HELLO/FINAL uncertainty.

Startup writes `1 OFFER <version-bitmask> <worker> <attempt>` on the existing
stdin pipe, before optional legacy shader hashes and EOF. Linux contains EPIPE
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
