# Jam2 Server wire format review

Implemented on `UDP-Multiplex`. This records decisions actually made during
implementation, their consequences, unresolved limits, and validation results.
The earlier discussion is in `UDP-Multiplex-WireFormat.md`.

## Scope

Only Jam2 Server control and WAV protocol traffic is in scope. Direct/STUN retain
their TCP format and behavior. Live audio packets, DSP, buffering, clock
synchronization, and audio engine scheduling are unchanged. WAV distribution
retains the recording-peer -> creator -> other-peers star.

## Transport retained

The existing authenticated multiplex envelope and reliable ordered streams are
retained. The outer prefix is 42 bytes plus a 16-byte HMAC tag. A reliable
fragment carries at most 1,024 bytes. UDP/IPv4 adds 28 bytes without IP options.

**Head-of-line blocking remains:** if fragment N is missing, later fragments in
that logical stream can be received and acknowledged but their bytes cannot be
delivered to the application until N is recovered. A priority assigned to an
application message cannot make it overtake bytes already queued in that stream.
Control and WAV have separate logical streams, but share the physical socket,
worker, and network capacity. There is no finite action-latency guarantee during
loss or a partition. A transport ACK proves transport receipt, not successful
application or execution.

This isolates the encoding experiment from a simultaneous reliability rewrite.
Possible later experiments are independently reliable bounded messages, separate
ordering scopes, compact ACK ranges/bitmaps, and measured aggregate upload pacing.
They need independent replay, dependency, congestion, recovery, and fairness tests.

## Reconciliation contract

Merge independent changes. For irreconcilable changes to the same value, the
creator's later validated receive/dispatch order wins, not sender wall-clock
timestamps. Loss and retransmission can change that order across links. Retries
must not count as new edits or reverse an already superseded result. Determinism
means all peers apply the creator's ordered decisions; it does not mean the same
winner under every possible network schedule.

Deletion beats metadata edits of the same content in either order. A stale delete
must not remove replacement recording content. Related clip region fields are
atomic; field-by-field merging must not manufacture an invalid region.

Unresponsive peers must not veto progress. Recording protection suppresses
conflicting remote transport application locally while continuing network receipt
and acknowledgements. Catch-up uses current authoritative state, not replay of
obsolete start/stop commands. Late actions need an action-specific next valid
boundary; a delayed stop/cancel must never turn into a new start.

## Build environment

The first required compile command found an existing Visual Studio-generator
cache, incompatible with `compile.cmd`'s Ninja generator. The old build was
preserved at `cmake-build-wire-format-baseline`; the normal command then built
the unmodified optimized MSVC Release application successfully. This is build
workspace maintenance, not a product change.

## Implemented encoding boundary

`ServerControlWire` owns numeric operation schemas, presence masks, authenticated
binary records, stream-local snapshot deltas, WAV transfer bindings, and dispatch
receipts. It activates only for the reliable UDP connection used by Server mode.
The existing TCP codec is not modified.

The cold challenge/proof handshake remains JSON for now. Server peers must agree
`server_wire=1`; the selected version is also appended to the authenticated
transcript, preventing a silent format downgrade. Existing nonce-derived direction
keys and the outer authenticated stream binding are retained. Binary handshake
encoding remains a possible later optimization, not a claimed current saving.

The authenticated record is `[length:u32][operation:u16][version:u8][flags:u8]
[sequence:u64][tag:16][payload]`, all integers big-endian. Length excludes its own
four bytes. The 16-byte tag is truncated HMAC-SHA-256 over the entire record with
the tag bytes zeroed, including length. Flags currently allow only bit 0 for
application fragmentation. Application sequence numbers restart only with fresh
authenticated keys. There is no encryption added by this experiment.

Ordinary records carry an operation-specific u32 presence mask followed by
positional fields. The first authenticated record sequence is also the
stream-local application message ID, avoiding a redundant eight-byte counter in
every record. This is not the outer UDP packet/fragment sequence. A result carries
that ID explicitly because it refers to traffic in the opposite direction.
IDs/tokens are 16 binary bytes
(the existing peer token has 16 bytes; it is not shortened to its derived 8-byte
audio peer ID). SHA-256 fields are 32 bytes. Optional binary identities and frame
strings preserve empty versus absent values with a one-byte marker. UTF-8 text
has a u16 length and a 16,384-byte bound, preserving the existing 4,096 UTF-16-unit
cell limit. Existing domain validators still enforce narrower per-field limits.

Structured snapshot fields use bounded CBOR at the Qt serialization boundary;
their nested property names are not yet assigned numeric IDs. Common messages
use fixed numeric schemas with no transmitted property names. CBOR is checked for
depth and item count before materialization. Compression is used only when smaller
and its declared expanded size is limited to 4 MiB. This uses Qt already present
in the project, not an additional library.

Successive song messages use a typed recursive patch (unchanged, replacement,
object edits, same-length array edits) bound to an exact stream-local base serial.
Structural array insertions/deletions use replacement. A missing base fails the
logical channel and forces fresh authentication/checkpoint recovery. These patches
optimize serialization; they are not conflict-resolution decisions. The existing
GUI model still uses its JSON-shaped application boundary. Moving domain handling
entirely to typed commands is additional work, not accomplished by binary encoding
alone.

Large operations are split into 32-KiB application parts with total length and
offset, then independently authenticated and reliably fragmented. Parts occupy
consecutive authenticated record sequences, so offset/part-size identifies the
first record and its application identity without another transmitted counter.
Output is bounded to 5 MiB, while no write exceeds the existing 256-KiB stream
queue. A five-millisecond output pump submits at most 16 records per turn. These
values and current queue/receipt counts must be exposed in diagnostics. Output
remains FIFO and does not yet permit urgent operations to overtake checkpoint
parts. The pump interval can contribute scheduling delay when backpressured.

## WAV encoding implemented

Transfer start carries a monotonically assigned u32 handle bound to this
authenticated logical asset stream, the full hash, file length, and fixed chunk
size. A chunk is `[handle:u32][index:u16][PCM WAV file bytes]`; offset is derived
from index times the agreed chunk size and data length from the record boundary.
This saves 42 bytes versus the previous 48-byte per-chunk payload header. The
authenticated record remains 32 bytes in either format; no second per-chunk
message-ID counter is transmitted. Progress and DONE metadata also reference
the four-byte transfer handle. Their chunk count is u16, saving 30 payload bytes
versus the old binary SHA-256 plus u32 count.
Final short chunks are checked against the exact remaining length. Handles are
not reused within a stream, and exhaustion fails instead of wrapping. Replaced
handles reject delayed chunks. Fresh authentication prevents old stream packets
from entering a new binding.

The application boundary reconstructs the existing chunk structure for the
unchanged file worker, preserving its hash/order/source checks and four-chunk
write-acknowledgement window. This adds a small boundary copy; it does not claim
zero-copy file delivery. File topology, live audio, and chunk/window sizes are
unchanged. Final validated-file confirmation remains distinct from dispatch and
progress acknowledgement.

## Initial validation

The new native Release `jam2_server_wire_units` CTest passed. It checks authenticated
records, replay/tamper rejection, malformed booleans, unknown operations/fields,
checkpoint/delta convergence, rejected-encode rollback, WAV transfer replacement,
exact final chunk sizes, receipt stages, and queue drain under backpressure.

The native performance CTest uses the actual codecs for 1,000 renames of one lane
in a 64-lane model, checking decoded equality on every action. Final measurements
below supersede the initial implementation's measurements. Timings are observations,
not portable pass thresholds; the byte assertion detects regression to full-snapshot
renames.

## Reconciliation and progress implemented

The existing looper merge has an explicit policy, defaulting to its legacy
keep-current rule. Only Server mode selects latest-received conflicts. Independent
fields merge; clip region and content fields are atomic groups. Metadata based on
old bytes cannot mutate replacement content. Native cases cover both policies,
both rename/delete orders, replacement content, and inverted-loop prevention.

Generated-Idea proposals are bulk replacements of their declared scope. Server
accepts a later valid scope even when an intervening Idea or timing change makes
its base stale; the scope compositor retains unrelated tracks and, for chord-only
or beat-only operations, the other content scope. A whole-Idea replacement is
atomic at the Idea level rather than an attempt to splice generated sections
from competing generations. Native checks retain the stricter legacy rejection
policy and exercise the explicit Server policy. Reconciliation still needs a
base in the retained 16-revision authority history. An older unknown base is
rejected and refreshed from authoritative state; it is not silently resubmitted
as a new intention. Extending history or adding durable typed mutation identities
is a separate cost/recovery tradeoff.

Server listening controls carry an explicit changed-group mask: speed, pitch, and
the related focus settings. This preserves intent when a slider is moved back to
its previously applied value while another request is in flight. The creator
merges untouched groups and commits later received values for groups changed by
both peers. Fixed binary listening settings and tempo layouts replace nested
property names; membership entries, participant IDs, and asset request hashes are
also packed directly rather than sent as text or generic maps.

Desired authoritative listening state is now separate from local preparation and
application. The creator can accept and distribute changes even while its own
recording is protected. Each peer prepares independently; a slow or recording peer
retains only the latest committed selection, then catches up after protection ends.
The existing 750-ms lead is retained; late application moves to a future local
beat with at least 200 ms of preparation lead. The original authoritative tempo
anchor is retained and the source position is advanced to the later application
point. This is a control decision using existing engine commands, not an audio
engine, clock protocol, DSP, codec, or buffering change. It deliberately permits
temporary differences between peers while they prepare; actual application is
observable separately from committed desired state.

Server bank launch uses local readiness rather than an all-peer barrier. Recording
groups may start with ready participants while another armed peer is unavailable.
A late Server recording start moves its count-in to a future bar and preserves
the requested count-in length. Default non-Server readiness behavior is unchanged.
The recording/bank policies are implemented in their owned native components,
with the GUI passing the strategy-selected policy.

The native four-controller listening test includes a recording creator, two
applying peers, one recording peer, concurrent speed and pitch edits, delayed
application, duplicate state, and explicit speed reset. The looper, listening,
recording-workflow and bank unit tests passed in optimized Release.

The first four-real-process UDP application run failed because the new schema
omitted the existing Track Sync `applied`/`reason` result fields. The failure was
confirmed in authenticated application logs; both fields are now represented.
Artifacts were preserved under `artifacts/wire-format-initial-failure` before the
next standard compile command recreated its test workspace. The subsequent native
four-peer Server shared-content CTest passed in 81.99 s and the four-peer transport
CTest passed in 2.84 s. These are correctness workflow durations, not isolated
control latency measurements.

Restricted execution initially prevented the Go signaller from starting and
prevented two native file-worker tests from opening temporary WAVs. The same
scoped tests passed when run with the required elevated execution. No assertion
was relaxed to address these execution-environment failures.

## Result semantics and bounded failure handling

Dispatch results carry the original stream-local u64 message ID and an explicit
stage: dispatched or rejected. Semantic/source validation can return an invalid
value or unauthorized rejection with bounded explanatory text before application
dispatch. This is not conflated with an arrangement commit, readiness, file-worker
acceptance, or actual timed execution. Heartbeats, asset chunks/progress, readiness
and other status messages do not require redundant dispatch receipts. Receipt
frames are not acknowledged at the application level.

Per-channel outstanding dispatch results are bounded to 128. Current/past queue
counts, unresolved results on stream retirement, encoding rejects, typed
rejections, and enqueue-to-result latency sample count/sum/maximum are exposed.
Latency includes local output queueing, transport, remote dispatch, reverse
queueing and return transport; it is not a one-way network-delay measurement.

Bank-switch and recording-group commands retain their existing domain identities
and readiness/status exchanges, with reliable delivery and dispatch results on
the relevant commands. They do not gain a universal remote-engine-executed ACK.
Listening application has the explicit revision/frame status described below;
WAV batch application has the existing batch result. Adding execution results to
other action types needs action-specific definitions for late, protected,
superseded and cancelled outcomes rather than treating one generic ACK as proof
that everything happened.

Output capacity is checked against the actual encoded size before committing a
snapshot base or transfer binding. A slow Server peer whose bounded broadcast
queue/result window fills has its logical control stream recovered, causing the
existing authentication path to send current authoritative state. Other peer
links continue. This trades that peer's transient updates for explicit state
recovery; it does not silently drop the final authoritative snapshot. Application
message IDs and snapshot bases are scoped to a freshly authenticated stream.
Across recovery, authoritative checkpoints reconcile unknown outcomes rather than
automatically replaying old user intentions. The existing UUID-based WAV batch
and recording-group identities retain their domain deduplication behavior.

Large document assembly has a separate ten-second progress deadline in addition
to individual record framing deadlines. Decompression uses the already vendored
miniz inflater with an exact bounded output buffer, checks full input consumption
and exact output length, and rejects a stream which expands beyond its declared
length. Merely checking qCompress's four-byte size hint would not provide that
allocation bound. No vendored source or additional dependency was introduced.

The real four-peer Server listening workflow exposed a sync re-enable bug: no new
slider edit meant no changed-group intent, so the creator failed to republish
its selection after private editing had been allowed. Re-enable now explicitly
commits the creator's current selection, and a native regression covers it.
Existing default TCP policy and message validation remain unchanged; Server-only
changed-group fields are enabled explicitly at their validation boundary.

The re-enable fix also has an ordering requirement: queue the authoritative sync
policy before the listening state it enables. Publishing state first caused
receivers to correctly suppress it under the previous policy. Publication is
deferred one event-loop turn with a guard against superseding selections. Both
the native asynchronous-boundary case and the subsequent real four-peer Server
listening workflow passed; that workflow took 20.00 s. The unchanged legacy TCP
listening workflow also passed (39.45 s). Those complete workflow durations have
different transport impairment paths and are not an isolated latency comparison.

## Representative byte layouts

These sizes include the 32-byte authenticated application record, exclude the
outer multiplex envelope and UDP/IP, and assume no optional authority token.

| Record | Fields after authentication header | Bytes |
|---|---|---:|
| Heartbeat | presence u32, heartbeat sequence u32 | 40 |
| Listening request | presence u32, fixed settings, changed groups u8 | 78 |
| Listening state | presence, settings, bank u8, tempo, target, epoch, source position, playing, revision | 141 |
| WAV start | presence, SHA-256, file bytes u32, chunk size u16, handle u32 | 78 |
| WAV progress or DONE | presence u32, handle u32, chunks u16 | 42 |
| Full WAV chunk | handle u32, index u16, 24,576 file bytes | 24,614 |
| Dispatch success | referenced first-record sequence u64, stage u8 | 41 |

The settings examples use the six-byte preset name `custom`. Settings encode
speed ppm i32, pitch cents i32, enabled u8, bounded UTF-8 preset, and three finite
f64 values for frequency Hz, gain dB, and Q. Tempo encodes rate ppm i32, anchor
frame u64, and anchor position f64. No floating-point parameter is silently
quantized. Related focus values are one reconciliation group.

Flags, enums, numeric ranges and trailing bytes are checked. A native golden
vector fixes the heartbeat payload, complete big-endian header and HMAC bytes.
SHA-256 is exactly 32 bytes without an empty marker; optional 16-byte identities
use a marker to preserve empty versus absent values. Membership transmits each
16-byte peer token once and derives its existing numeric peer ID locally.

Each reliable datagram adds 58 multiplex bytes and normally 28 UDP/IPv4 bytes;
each standalone transport ACK costs another 86 UDP/IPv4 bytes. A 24,614-byte WAV
record still takes 25 current 1,024-byte reliable fragments. Reducing its header
does not eliminate a datagram at this chunk size. A dispatch result itself can
also consume a separate datagram and transport ACK. These costs are why progress
and status messages do not receive redundant application receipts.

WAV validation/atomic application remains in the existing file worker. Existing
`looper.track.batch.complete` with `applied=true` confirms a successfully installed
batch; false plus a reason reports supersession or failure. A DONE dispatch
receipt only confirms dispatch of DONE, not successful WAV validation. There is
no extra per-file validation ACK added for transfers already covered by the batch
result. Standalone repair transfers retain their existing completion semantics;
a dedicated per-file final-result exchange is a possible later extension if its
extra traffic is justified.

## Remaining tradeoffs to review

- Ordered delivery still causes head-of-line blocking. Urgent control cannot
  overtake an already queued large control document. Asset traffic is a separate
  stream, but shared worker/socket/uplink contention is still possible.
- The outer multiplex session/introduction/stream identifiers and its HMAC remain
  unchanged. The nested directional application HMAC is also retained. Removing
  duplication requires an explicit authentication/replay redesign, not merely
  stripping headers. Initial capability negotiation still uses a small JSON
  handshake.
- Snapshot patches address positions in a stream-local serialized baseline.
  Stable object IDs remain in the model; there is no new per-track wire-handle
  registry. Nested snapshot/track-manifest properties still use schema-validated
  CBOR names. More specialized typed operations and compact numeric snapshot
  schemas could reduce CPU/bytes further, but would expand the application-model
  migration.
- The snapshot serializer can evaluate a complete replacement while choosing a
  smaller patch. Large models therefore still have traversal/allocation costs
  even when very few bytes are sent. The four-MiB bound is an encoded-document
  limit, not a claim that a Qt object graph occupies only four MiB of heap.
- Stream-local result IDs are not durable mutation IDs across application/session
  restart. Unknown outcomes recover through authoritative state; old intentions
  are not automatically resubmitted as new edits. UUID-based recording/batch
  operations retain their existing explicit deduplication identities.
- Content replacement is distinguished by existing WAV identity/hash. Identical
  bytes intentionally share content identity; a separate recording-generation
  token would be needed to distinguish every semantic re-recording of identical
  bytes. That is not claimed by the current metadata merge.
- WAV chunks retain the four-chunk file-worker window and current pacing and
  reliable-stream congestion control. No range resume, new congestion controller,
  or peer-assisted distribution is included. Creator upload remains the star's
  fan-out bottleneck. First experiments after this work could compare batching,
  chunk/window sizes and ACK aggregation using controlled RTT/loss and upload
  limits, while retaining raw queue and result-latency measurements.

## Final codec comparison

The final native Release performance workload includes both directions of WAV
metadata, progress acknowledgements, and data encoding/decoding. It checks exact
decoded equality and file-byte equality. The renamed-track workload checks all
1,000 resulting models; it includes the initial Server checkpoint.

| Workload | Encoding | Authenticated bytes | Encode and decode time |
|---|---|---:|---:|
| 1,000 renames in a 64-lane model | Direct | 17,093,783 | 547.692 ms |
| Same renames | STUN | 17,093,783 | 519.411 ms |
| Same renames | Server | 147,079 | 286.049 ms |
| 1-MiB file, 43 chunks, metadata and progress | Direct | 1,058,653 | 11.006 ms |
| Same file workload | STUN | 1,058,653 | 10.729 ms |
| Same file workload | Server | 1,052,136 | 11.735 ms |

For this model, renames use 99.14% fewer authenticated bytes. File framing and
metadata overhead falls from 10,077 to 3,560 bytes, or 64.67% less overhead; total
file-transfer bytes fall by only about 0.62% because the file data dominates.
WAV codec CPU time was higher in this single Server sample;
the retained application-boundary chunk reconstruction still copies data. No WAV
CPU speedup or Internet throughput improvement is claimed from these numbers.

These are production-codec comparisons in an optimized native CTest. Direct and
STUN use the same existing TCP codec. Handshake, application dispatch-result
records, multiplex/UDP/IP/Ethernet headers, retransmissions, disk validation and
network/file-worker windows are excluded from this isolated codec comparison.
Complete application workflows separately exercise the real reliable transport,
loss/reordering/duplication, file workers, and four-peer state convergence.

Actual listening application is reported with a Server-only status carrying the
authoritative revision and the receiver's raw engine frame. It is sent only after
existing engine publication checks confirm application. It does not require an
additional application ACK; transport reliability still applies. Repeated current
state can recover the recorded application result without executing it twice.
The creator exposes per-peer application reports. Frame numbers are local raw
engine positions, not a claim of identical wall-clock execution across a partition.

The Server diagnostic adds per-peer/per-channel wire counters and queue/result
latencies, as well as aggregate totals. Queue/result limits are labelled per
channel so aggregate values are not mistaken for a single channel's cap. The new
diagnostic fields are emitted only in Server mode; TCP diagnostic shapes retain
their existing fields.

Aggregate queue/receipt counts sum active channels. Aggregate queue high-water
and result-latency maximum are maxima of individual channel observations, not a
simultaneous total-queue high-water measurement. Aggregate last-result identity
is only a maximum of stream-local counters; use the per-channel entry to correlate
an operation. Retired-channel result windows become unresolved counts rather than
being labelled applied, rejected or lost.

## Small-field widths and compatibility

Section/bank/step/division identifiers, the listening changed-group mask, count-in
bars and protocol-version metadata use u8. Beat positions/counts use u16. These
wire widths exceed existing domain limits; encoding rejects overflow rather than
truncating. Small identity/hash arrays use a u8 count with the existing 64-entry
per-message bound. This is not a new total jam peer limit; membership already
uses bounded multipart messages.

The retained 512-MiB WAV file bound fits u32. The fixed 24-KiB chunk size fits
u16, and the largest allowed file requires 21,846 chunks, so indices and progress
counts also fit u16. These choices depend on the present file/chunk bounds.
Increasing those bounds requires reviewing these widths and changing the
experimental protocol version if its layout changes.

Version 1 requires exact operation schemas. An unknown operation, presence bit,
flag, version, trailing byte, malformed UTF-8, non-finite floating-point value or
invalid bound is rejected. There is no general TLV skip/extension mechanism or
negotiated JSON fallback. Every Server participant must run this experimental
format; Direct/STUN peers continue using their existing format. The numeric
assignments and positional field order are defined in
`app/application/ServerControlWire.hpp` and `ServerControlWire.cpp`.

| Operation IDs | Assignments in ascending order |
|---|---|
| 1–10 | contract, membership, heartbeat, heartbeat reply, introduce, repair, pair failure, endpoint, end, error |
| 16–19 | sync set, sync request, metronome set, metronome request |
| 24–32 | listening request, prepare, ready, failed, commit, cancel, state, reset, applied |
| 40–47 | cell set, hit, beat division, music division, music step, resize, song, reference render |
| 56–60 | bank request, prepare, ready, cancel, switch |
| 64–73 | share request, offer, complete, recording state, group start, finish, recover request, recover, resync request, resync state |
| 80–84 | asset request, start, progress, DONE, unavailable |
| 96–98 | asset chunk, dispatch result, existing opaque diagnostic binary |

Every ordinary schema appends optional revision i64 and authority identity slots.
The u32 presence mask identifies fields in schema order; present fields are
concatenated without alignment padding. A rejected result adds a u8 rejection
code and bounded UTF-8 reason after its referenced sequence and stage. There are
no transmitted field names on these ordinary operations.

The four-byte mask is retained even for small operations to make omitted versus
present fields explicit and keep one straightforward positional parser. Booleans
are individual u8 values, not packed value bitmaps. Some existing domain enums
(recording phase, sync scope, preset/profile names) remain bounded text values.
Packing boolean values and assigning numeric enum values could save further
bytes, but need stable enum assignments and compatibility tests. Optional
authority/revision fields are sent only when present; there is no per-message
wall-clock timestamp, new global ID, or redundant chunk offset/length/hash.

## Scoped proposal validation race

The four-peer sharing-race CTest initially failed its 90-second convergence wait.
All four peers had removed the generated reference WAV and retained the custom
WAV; the failed assertion was the generated Idea metadata remaining on some
peers. Two back-to-back scoped proposals could cancel each other's asynchronous
file validation. Merely retaining the composed creator model fixed the creator
but did not publish the cancelled earlier scope to the other peers.

Server proposals now retain one composed pending model, bound to the current
authority revision and validation generation. Independent track fields reconcile
against this pending model as well as committed history. Failed validation,
generation changes and authority advancement prevent stale staging from becoming
a later base. The staging owner also retains the bounded set of affected scopes;
successful publication includes each affected scope using existing scoped
messages. This can emit two small consecutive deltas rather than one when an
Idea edit and track edit coalesce. Receivers retain their existing scoped
composition, and sync-policy filtering still applies. This is not a new atomic
multi-scope transaction or a change to TCP behavior.

The native staging regression covers scope retention, superseding completion,
rejection, authority advancement and cancellation. The unchanged real four-peer
sharing-race assertion subsequently passed in 7.92 s. The initial failure and its
per-peer artifacts are retained in `artifacts/wire-format-sharing-race-failure`.

Existing asset-service log estimates such as `binary_body_bytes` and
`authenticated_chunk_wire_bytes` describe the retained legacy application
boundary, not the compact Server record. For actual Server record bytes and
header savings use `udp_multiplex.binary_control` counters. Transport byte and
retransmission counters remain separate; neither estimate includes every layer.

## Release validation record

The 19-test scoped Release run passed in 315.19 seconds. Its captured native
CTest output, including the raw codec measurements used above, is in
`artifacts/wire-format-19-ctests.log`.

| Area | Passed CTests |
|---|---|
| Binary/authentication/framing | `jam2_server_wire_units`, `jam2_udp_control_transport_units`, `jam2_udp_multiplex_units` |
| Native owned state | `jam2_looper_project_units`, `jam2_listening_tools_units`, `jam2_shared_bank_launch_units`, `jam2_track_recording_workflow_units`, `jam2_track_workspace_support_units`, `jam2_asset_transfer_units` |
| Codec comparison | `jam2_server_wire_performance` |
| Exactly-four-peer Server workflows | `jam2_udp_four_shared_content_integration`, `jam2_udp_four_listening_tools_integration`, `jam2_udp_four_sharing_races_integration`, `jam2_udp_four_recovery_integration`, `jam2_udp_four_unthrottled_wav_integration`, `jam2_udp_four_peer_rotation_integration`, `jam2_udp_four_application_integration` |
| Exactly-four-peer legacy TCP regressions | `jam2_four_shared_content_integration`, `jam2_four_listening_tools_integration` |

Builds used the repository-root `compile.cmd --in-dev-shell` workflow, optimized
MSVC Release, and the normal `release/jam2.exe` with its runtime DLLs. Native
CTest targets also used Release. No alternate Jam2 executable, Debug build,
instrumentation, source coverage or full-suite run was used. The existing Go
signaller executable was consumed read-only; no sibling repository was modified.

The final generated-Idea conflict-policy adjustment was followed by another
successful normal Release compile and the Server sharing-race CTest (8.39 s).
The directly affected merge/staging units and both four-peer shared-content
workflows then passed against that final build: Server shared content 84.99 s,
TCP shared content 32.11 s, native merge 0.03 s, and native staging 0.10 s.
That captured run is `artifacts/wire-format-final-policy-ctests.log`.

The separate `jam2_udp_four_peer_integration` transport/WAV fixture also passed
against the final build in 2.82 s. It exercises four real authenticated peers
through the existing deterministic UDP impairment fixture and reconstructs WAV
chunks with the production asset format. In total, 20 distinct scoped native
Release CTests passed; affected cases were rerun after the final policy change.
