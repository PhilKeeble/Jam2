# Jam2 Server wire format review

Implemented on `UDP-Multiplex`. This records decisions actually made during
implementation, their consequences, unresolved limits, and validation results.
The earlier discussion is in `UDP-Multiplex-WireFormat.md`.

**Current implementation: experimental Server wire v2.** The final section,
"Measured efficiency changes (v2)", supersedes the v1 field layouts and the
earlier all-control-in-one-ordering limitation below. Earlier measurements and
limitations are retained as the review history, not silently rewritten.

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

## Manual two-peer log review, 26 October 2026

Reviewed `release/logs/jam2_gui_20261026_212839_299_pid20560.log` and its
`jam2_stats_20261026_212933_522_pid20560.csv`. This was the joiner's view of a
two-peer Server session, authenticated at 21:29:33.503 UTC and ended by the
creator at 21:36:48.992 UTC. The selected route was LAN. It does not establish
Internet/loss performance or expose the creator's application-result state.

Across 217 periodic multiplex snapshots, there were no reported invalid mux
packets, mux send errors or reliable retransmissions. Main-control authentication,
framing, sequence/tag, validation, authorization and output-reject counters were
zero at shutdown; heartbeat misses were zero. Both binary channels reported zero
encoding, queue, operation and rejection-result errors. The last periodic sample,
at 21:36:47.323 UTC, had zero pending/unresolved dispatch results and empty queues.
That sample precedes shutdown by about 1.65 seconds; it is not a terminal binary
receipt audit after channel teardown.

Main-control enqueue-to-dispatch-result latency was 31.945 ms mean, 38.523 ms
maximum across 13 results. Asset-channel dispatch results were 3.756 ms mean,
4.045 ms maximum across two results. These include application scheduling and
return delivery; they are not transport RTT or timed-execution latency. Per-channel
application queue high-water values were 1,969 bytes for main control and 24,614
bytes for assets. The reported transport queues were empty in the last sample.

A 734,160-byte WAV was received and accepted in 543 ms; a 13,649,552-byte WAV
was sent in 6,272 ms and its one-track batch completed. An older pending batch
was superseded at 21:31:00.067 while its active WAV transfer was deliberately
retained; the later logs show acceptance and arrangement application. This was
not a failed or abandoned transfer.

**Listening-state retry gap (subsequently fixed below):** at 21:29:33.595, initial content/transport changes
cancelled a scheduled listening commit. The cancellation is confirmed by the
log. Code inspection shows that `ListeningToolsController::cancel` clears the
pending commit/deferred message without automatically retaining a retry in
`deferredServerSelection_`. A repeated selection of the same authoritative
revision also does not restart an idle cancelled selection. Consequently, a
valid desired selection can remain unapplied after this kind of cancellation
until another state-changing event supplies a new selection. Later listening
revisions in this run did commit, ending with revision 12 at normal speed.
The available log does not include the initial desired/applied settings or the
creator's per-peer execution reports, so an actual settings divergence in this
run is not established. A deterministic cancellation/retry regression is needed
before treating this path as verified. No product fix was made during this
log review.

At 21:36:48.977 a logical close briefly scheduled reconnect; 15 ms later the
creator-left event ended the jam. There was only one completed main-control
connection and no mid-session disconnect. This looks like shutdown event ordering,
not evidence of a connection failure during playing. The diagnostic field
`same_socket_recovery_attempts=2` is a configured limit, not two observed retries.

## Listening cancellation retry fix

The subsequent native regression reproduced the manual-log issue before the fix:
`jam2_listening_tools_units` failed with "Server cancellation stranded the latest
authoritative listening selection". The deterministic four-controller case has
one healthy peer, a content-delayed creator and joiner, and a recording-protected
joiner. It checks recovery without any new edit or retransmitted selection.

Server listening control now distinguishes a local content/transport interruption
from cancellation of the authoritative operation. On interruption before commit,
it cancels the obsolete local preparation, retains the latest typed selection,
and keeps the selected GUI values. It does not broadcast an authoritative cancel
or report a terminal preparation failure for this transient condition. Content
readiness and section transitions also gate this path in Server mode. The existing
controller poll retries once content, bank and recording protection allow it;
late application uses the existing future-beat scheduling. The original revision
and tempo anchor are retained; actual application is reported normally. No new
wire field, protocol version, peer barrier, application revision, audio-engine
change or extra network retry loop was introduced.

The pending slot remains bounded to the latest selection. Newer revisions replace
it; old duplicates cannot restore superseded work, and an already applied revision
does not execute twice. Explicit authority cancellation clears a deferred retry
even during recording protection. Disabled listening sync prevents retry execution;
session reset clears the retained state. Genuine render/command failures and
preparation timeouts retain their existing failure handling rather than gaining
an automatic unbounded retry policy. Legacy TCP behavior is unchanged.

`server_content_deferrals` counts local preparation/commit interruptions retained
for retry and resets with the controller session. `server_deferred_state` exposes
whether a retained selection is waiting. This reuses the existing poll, content
readiness and recording gates; it does not add a delay queue or hidden backoff.
`server_applied_revision` exposes the revision actually applied through the shared
operation path, starting at zero before application. The older generic applied
revision starts with the GUI's default settings and cannot by itself prove that
an initial network selection executed.

Native coverage also exercises interrupted render and staged-load ownership,
deletion of abandoned files, stale worker generations and staging events, newer
revision supersession, explicit cancellation, disabled sync/reset, and terminal
render failure. The expanded optimized Release unit CTest and real four-peer
Server and legacy listening checks pass against the rebuilt application.
The four-process Server workflow additionally requires all three joiners to finish
applying their initial authoritative listening revision after startup song/WAV
sync, before any user slider edit can mask a stranded initial selection.

One integration run passed that new startup check but failed later at the existing
"Stop cancels waiting Play while retaining the newest shared speed" assertion.
Expected: all four peers stopped with applied/musical speed 930,000 ppm. Observed:
three reached revision 8 at 930,000 ppm; one remained applied at revision 7,
820,000 ppm and reported "listening audio swap was not applied by the engine".
The test polls real-process state for up to 60 seconds; it does not infer success
from the selected GUI value. A normal prepared-track load appeared in that peer's
log shortly before the missed swap. Interference with staged-source publication
is a hypothesis, not an established cause. This is a different failure path from
the reproduced content-change cancellation, and the run reported no content
deferrals at that later operation. The assertion and publication-grace threshold
were not relaxed. Per-peer artifacts are preserved at
`artifacts/listening-retry-integration-failure`. The final Release build is being
rerun unchanged to check reproducibility and the TCP/shared-content regressions.
That unchanged rerun passed all four scoped CTests in 144.72 s: Server listening
19.66 s, Server shared content 86.27 s, legacy TCP listening 38.50 s, and native
listening units 0.28 s. The record is `artifacts/listening-retry-final-ctests.log`.
The intermittent later swap failure remains a recorded follow-up; a passing rerun
does not establish its cause or prove that path free of races.

## Requested Direct/Server audio-latency comparison

`jam2_four_mode_audio_latency_integration` runs the same public Release application
with four participants and real synthetic PCM in each mode. It uses loopback
endpoints, the existing real Go signaller for Server, and isolated factory
preferences. Every joiner must receive the same fast, 48-kHz, 64-frame, PCM24
contract. The existing GUI automation backend uses 128-frame headless callbacks;
the test asserts this on all four participants in both modes. It does not change
that backend or any audio path/tuning implementation.

The GUI's incoming-audio number is an estimate: RTT/2 plus jitter-buffer, mixer
queue, playback-ring and output-path frames converted to milliseconds. The pill
rounds to whole milliseconds. The benchmark reads the production GUI breakdown,
keeps its component frame counts and 0.1-ms network/total values, and verifies
the total against those components. It does not substitute the CLI's different
one-way estimate or claim a measured physical microphone-to-speaker delay.

There are four runs in Direct/Server/Server/Direct order. Each waits for all twelve
directed audio paths to become measurable, warms up for three seconds, then takes
eight one-second samples from all four receivers: 192 path observations per mode.
This bounds the experiment and reduces a fixed run-order bias; observations from
one process or cached telemetry interval are not statistically independent trials.
The raw observations and median/p95 component summaries are retained in native
CTest output as well as the test's JSONL artifact.

The preselected equivalence gate is an absolute median difference no larger than
one packet (64/48,000 seconds = 1.333 ms) and an absolute p95 difference no larger
than one headless callback (128/48,000 seconds = 2.667 ms), with 0.1 ms added for
display resolution. These are explicit regression budgets, not proof of zero
mode overhead, universal Internet/Wi-Fi equivalence, or statistical equivalence
within half a millisecond. The exact measured differences must accompany a pass.
This is an idle/normal-control-traffic baseline; it does not establish equivalence
under a saturated WAV upload or an impaired uplink.

The debug `gui-agent` create/join commands now accept an optional strictly boolean
`diagnostics` field so this test can enable the existing live measurements. The
default remains false for existing automation. Normal GUI/CLI mode behavior is
unchanged. The latency CTest exercises enabled diagnostics in both modes and a
malformed-field rejection before startup. Earlier fixture setup attempts exposed
two existing conventions: Direct invites omit a Server strategy parameter, and
only receiving joiners log receipt of the creator's contract. Those checks now
follow the existing conventions; the latency budgets were not adjusted.

The optimized Release latency CTest passed in 61.80 s with these pooled GUI
estimate observations (192 per mode, correlated as described above):

| Mode | Median incoming estimate | p95 incoming estimate | Median mixer queue | Median playback ring | Output path |
|---|---:|---:|---:|---:|---:|
| Direct | 6.7 ms | 9.1 ms | 0.375 ms | 3.458 ms | 2.667 ms |
| Jam2 Server | 6.7 ms | 9.3 ms | 0.604 ms | 2.875 ms | 2.667 ms |

Server minus Direct was 0.0 ms at the median and +0.2 ms at p95, within the
unchanged budgets. Both modes' median displayed RTT/2 and instantaneous jitter
depth were 0.0 ms on loopback; displayed zero does not establish physically zero
network delay. Component medians do not sum to the median total. The sample did
not reproduce a 1–2-ms median increase in Server mode, but cannot attribute the
manual Wi-Fi observation to contention or rule out such an increase on a different
device, link, workload or scheduler. The matching controlled result is the
scope of the equivalence claim.

Raw values are retained in `artifacts/mode-audio-latency.jsonl`, and the complete
native CTest output in `artifacts/mode-audio-latency-ctest.log`. Audio/core/CLI
implementations and audio parameters were not adjusted to obtain this result.

After the debug diagnostics option was added, the final default-automation
regressions also passed against `release/jam2.exe`: native listening units
0.26 s, four-peer Server listening 20.64 s, and four-peer TCP listening 38.62 s.
The record is `artifacts/listening-retry-final-defaults-ctests.log`. Together with
the four-peer shared-content run and new latency benchmark, five distinct scoped
native Release CTests passed for this follow-up. No full-suite or coverage run
was performed. The separately recorded intermittent staged-source failure is
still unresolved; these passes are not presented as a fix for that failure.

## Further control-plane efficiency audit

These are opportunities identified in the current code, not implemented changes
or measured future savings. The next gains are likely to be packet-count and
application-processing reductions rather than another universal 99% payload cut.

- `Channel::flush` submits each framed record separately to the reliable sender.
  Bundle already-ready small records into bounded writes so they can share a
  fragment. Keep urgent actions immediately flushable; do not introduce a blanket
  batching delay. A standalone 41-byte dispatch-success record costs 127 bytes
  with multiplex/UDP/IPv4 framing, plus an 86-byte standalone transport ACK,
  excluding link-layer overhead. Eliminating a packet can outweigh shaving a few
  bytes from its payload.
- Dispatch confirmations are cumulative in consumption but currently emitted
  individually. Coalescing consecutive successes and attaching confirmation to
  existing return traffic can reduce reverse-path packets. Explicit rejections,
  domain commits, actual application results and file validation must retain their
  distinct meanings. Transport ACK ranges/bitmaps or piggybacking require a
  separate control-transport protocol change; they are not an application-only
  encoding tweak.
- Existing slider debounce can be complemented by action-specific coalescing
  before enqueue for superseded, unsent absolute-value updates. Do this before
  assigning authenticated sequences and snapshot baselines. Removing already
  encoded records would create sequence/base holes. Relative edits, recording
  actions, deletion and transactional boundaries must not be generically dropped.
- Explicit typed track/cell edit operations could avoid building, traversing and
  diffing a full application snapshot for each small edit. Keep checkpoints for
  joining/recovery. This is a domain-model boundary migration, with stable identity,
  revision and conflict tests; a serialization patch is not itself an edit intent.
- Reduce current CPU work: precompute schema field validation instead of building
  temporary allowed-field sets for every encode; avoid redundant semantic checking
  of the same validated Server message at two dispatch layers; measure a minimum
  payload size before attempting compression. `qCompress` is currently attempted
  even when the serializer ultimately chooses the small uncompressed patch. Keep
  source/authority checks and hard decompression limits intact. Numeric CBOR field
  IDs, packed flags and bounded numeric domain enums offer further checkpoint and
  small-field savings, but require versioned layouts and compatibility tests.
- Independent ordering for urgent actions versus checkpoints could improve tail
  latency under loss/backlog. This is a reliability/dependency redesign: adding a
  priority flag to the existing ordered FIFO cannot overtake already queued or
  missing bytes. Preserve deterministic creator decisions and explicit dependency
  barriers before allowing operations to overtake one another.

Start with bounded small-record/success-result coalescing and the avoidable
serialization/validation work, then evaluate specialized edit operations. Measure
packet counts, on-wire bytes including ACKs, queue age, dispatch/apply p50/p95/p99,
CPU and retransmission volume under controlled loss/RTT/uplink limits. Header or
nested-HMAC consolidation is a later authentication/binding/replay design review;
tags and counters must not simply be removed. All proposals remain restricted to
Server control/WAV traffic, retain the WAV star, and do not change live audio.


HMAC / header consolidation might be worth doing (TBD, waiting on results)

## Measured efficiency changes (v2)

Implemented only on `UDP-Multiplex`, using the reliable-UDP Server codec gate.
Direct/STUN still use their original authenticated TCP messages. The live media
path and WAV star distribution are unchanged. There is no header or tag
consolidation: application records remain 32 bytes including length, sequence and
16-byte tag; the outer multiplex prefix/tag remain 42+16 bytes. No ACK range,
transport ACK piggyback or shortened counter experiment is included.

### Version and payload packing

The experimental handshake now requires `server_wire=2`, also bound into the
proof transcript as `jam2-server-wire-2`. A v1 experimental client cannot silently
connect using an incompatible v2 layout. There is no downgrade fallback. Legacy
TCP negotiation is unaffected.

Ordinary operations use a presence mask sized from the compiled positional schema:
one byte for at most eight fields, otherwise two bytes. Common revision/authority
slots count toward that width. All current schemas fit 16 bits. In schema order,
only present Boolean fields consume bits in a separate Boolean bitmap immediately
after the presence mask, least significant bit first, rounded to whole bytes.
Other present field values then follow in schema order. Unused presence/Boolean
bits are rejected. Absence and false remain distinct. Numbers remain network byte
order, and floating values retain their original IEEE-754 precision.

Text domain enums have one-byte IDs, starting at one in the following order;
zero and unknown IDs are invalid. Conversion occurs only at the protocol boundary.

| Field domain | IDs in order |
|---|---|
| Grid lane | chord, target, beat, lyric, melody, bass, support |
| Recording phase | idle, armed, ready, waiting, count-in, recording, complete, finalizing |
| Song scope | tracks, idea.full, idea.chords, idea.beats |
| Audio contract format | pcm16-mono, pcm24-mono |
| Contract profile | fast, moderate, safe |
| Generated ideas policy | off, chords, beats, full |

The audio-format field is control-plane contract metadata; audio packet encoding
is not changed. Future enum extensions require coordinated experimental peers.
Listening settings combine focus enable in bit 7 with a preset tag in bits 0..6:
tag 1 means `custom`, tag 0 carries the existing bounded UTF-8 string, and other
tags are rejected. Other preset names therefore remain lossless. Speed/pitch and
the three focus floating values retain their exact previous ranges/precision.

CBOR checkpoint/manifest property names use the stable one-based `documentKeys()`
table in `ServerControlWire.cpp`. Unknown recipe/application properties retain
text keys. Generic object-delta keys use a one-byte ID, with zero followed by a
bounded UTF-8 key as the escape. Unknown numeric IDs, invalid key types, and
duplicate resolved aliases are rejected. This global dictionary is versioned with
the protocol; it is not dynamically negotiated or sent per message.

Snapshot `source_frames`, `start_frame`, `stop_frame`, `loop_start_frame`, and
`loop_end_frame` canonical signed-i64 decimal strings become CBOR integers under
their numeric field IDs. The decoder restores decimal strings before the model
sees them, including values above double's exact-integer range and -1 sentinels.
Noncanonical strings, overflowing strings, and application numeric values use
text-key escapes to preserve their exact original JSON types. This avoids an
implicit schema conversion of user-defined recipe fields.

### Processing and typed rename encoding: retained

Allowed-field sets are cached with each static schema. Source/authority and WAV
sample-rate validation still execute at authenticated wire dispatch. The Server
session callback consumes that already-validated message synchronously rather
than traversing it again. TCP retains its existing validation boundary. This does
not remove domain reconciliation, transaction/readiness checks or WAV validation.

The first numeric-document prototype was rejected: repeatedly converting the full
graph for each patch candidate, expanded-size check and diagnostic increased the
1,000-rename workload to about 1,461 ms, versus the 288-ms v1 baseline. The retained
code lazily builds replacement candidates. Patches with at most two changes and
at most 256 bytes can skip the full-subtree size comparison. This saves CPU but
does not guarantee the globally smallest encoding of every tiny subtree.

Expanded-song bounds use the last exact checkpoint size plus conservative patch
growth. A replacement resets that bound. If the conservative bound crosses 4 MiB,
the encoder/decoder recompute the exact bounded document size and reject actual
oversize. This avoids full serialization on every delta while retaining the size
limit. Byte-growth bounds are deliberately conservative and may trigger an exact
check earlier than necessary. Parsing still enforces nesting/item limits and exact
bounded decompression. Diagnostics now call the saving
`bytes_avoided_vs_last_checkpoint`; its baseline is the last full replacement's
uncompressed CBOR document length, minus the current operation payload length
(positive differences only). It excludes authentication/framing and does not
recompress a hypothetical checkpoint on every edit. It is a raw-size comparison,
not a measurement of bytes saved against the previous compressed on-wire frame.

There is a typed `RenameTrack` patch (kind 4): bank u16, lane u16, UTF-8 name with
u16 byte length. It lives inside the existing checked base/next snapshot serial
pair. The encoder proves that applying exactly that rename to the previous
snapshot reconstructs the entire new song. Any simultaneous unrelated edit,
deletion, lane replacement, reorder or structural change falls back to the
generic bounded patch/replacement. An unbound/stale serial cannot use indices to
rename a different lane. Creator reconciliation remains unchanged: this compact
wire edit does not override deletion or bypass the authority's semantic rules.

**Typed-edit limitation:** this is a serialization-boundary fast path, not a full
GUI/domain intent migration. The current GUI still supplies its model snapshot;
the codec avoids the recursive generic patch for the common single rename. The
benchmark includes model JSON copy/update, codec, authentication and decode, but
does not measure rebuilding the GUI's complete `songToJson` graph. Direct typed
intents from the model owner could remove that remaining work later. Gain, region,
mute and structural edits retain the generic patch. This bounded first stage is
kept because its measured wire saving is substantial without introducing a second
authority/reconciliation implementation.

Compression is attempted only for document payloads of at least 128 bytes and
used only when smaller. The cutoff is visible in diagnostics, along with attempts
and skips. Trying compression on all 999 small typed renames saved no bytes and
cost extra CPU, so that variant is rejected. This threshold is an explicit
engineering tradeoff, not a proven optimum for every future payload.

### Small writes and successful dispatch receipts: retained

Already-ready small authenticated records share a write of at most 1,024 bytes;
individual record HMACs, sequences and length boundaries remain intact. A lone
large record keeps its existing fragmentation. Absolute scalar edit bursts are
flushed on a zero-delay event-loop callback. Transaction commands, checkpoints,
WAV operations and other barriers flush immediately. Existing blocked-output
retry remains 5 ms. A zero-delay callback has no fixed millisecond sleep, but is
still subject to GUI scheduling; it is not a hard dispatch-latency guarantee.

Success receipts dispatched in the same event-loop turn collapse to the highest
operation ID. Explicit typed rejection results are emitted individually and
cumulatively cover earlier successes. A later success cannot replace a rejection.
No receipt acknowledges another receipt. Local enqueue, authenticated dispatch,
domain commit/application and WAV hash verification remain different events.
This does not piggyback an ACK on an existing record or change transport ACKs.

### Unsent absolute assignments: retained with a narrow whitelist

Only `beat.set`, `music.step`, `beat.division`, and `music.division` may supersede
an unsent assignment for the same type/section/lane/beat/step. Different keys
remain independent. A primary non-whitelisted operation drains the pending assignments
and acts as a barrier. A pending assignment is encoded/validated but receives no
authenticated sequence, dispatch ID or snapshot baseline until it is drained.
Replacing it therefore makes no sequence/base hole and allocates no phantom
receipt. The 128-outstanding-operation and 5-MiB application bounds include these
intents. New queue/coalescing/write/receipt counters are visible in wire stats.
Retained assignments stay in their original relative arrival/revision order:
when X1 is superseded by X3 around another cell's Y2, the output is Y2 then X3.
Replacing X1 in its old position would send revision 3 before revision 2 and could
make the receiver drop the independent Y2 update as stale. The native test covers
this explicitly. `unsent_assignment_delay_samples`, `sum_us` and `max_us` expose
the local wait from the surviving assignment's enqueue until sequence allocation.
It includes event-loop scheduling/barrier draining, not subsequent transport wait
or network dispatch. A sparse-edit test verifies it leaves on the next callback
without waiting for a second edit. Busy-GUI delay still needs real-run measurement;
the native no-GUI harness is not presented as that measurement.

This deliberately excludes relative edits, recording, listening transactions,
policy updates, deletions and song proposals/checkpoints. In particular, merging
partial listening group requests or dropping a stale peer snapshot generically
could lose another independent change. Existing slider/domain debounce and
creator reconciliation still own those cases. Coalescing reduces already-queued
bursts; it does not discard records already sequenced or handed to the transport.

### Independent ordering: retained for self-contained replies/status only

Outer multiplex channel 3 carries a second reliable ordering within the same
authenticated Control stream ID and physical UDP link. Only Data and Ack kinds
are legal there; Open/Close, stream retirement and authentication belong to the
existing Control channel 1. Asset channel 2 is unchanged. No additional socket or
signaller/relay path is created. Stream replacement invalidates both orderings.

Each direction derives its independent application key with the existing
HMAC-SHA256 primitive: HMAC(direction-key, `jam2-independent-control-v2`). Separate
keys and sequences prevent a valid record from being replayed between orderings;
the unchanged outer HMAC already binds channel, session, introduction and stream.
This is ordering-domain separation, not consolidation/removal of nested tags.

The whitelist is heartbeat/reply, listening ready/failed/applied, bank ready and
recording state. Existing owners check explicit transaction IDs, revisions,
participants and recording state revisions. Source validation remains mandatory.
Commands, commits, cancellations, edits, contracts and sync policy stay in primary
order. Therefore a rename cannot overtake its checkpoint, nor can a transaction
command overtake policy/model prerequisites. The independent parser rejects even
a correctly authenticated dependent opcode. Replays, bad tags, wrong-ordering
records and bounded-input violations are tested.

Independent application/transport input/output each have a 16-KiB bound, with
4,096-byte application records and a 10-second no-byte-progress frame deadline.
Ordinary primary bounds remain unchanged. Up to two independent packets may use
an existing per-control poll budget; when primary data waits and budget is at
least two, one slot is reserved for primary. It never increases the link worker's
existing control/asset packet budget. Separate ACK/congestion/retransmission state
can change aggregate fairness within that budget; this is not a new shared
congestion controller. The path and limits are exposed through wire diagnostics.

**Ordering limitation:** readiness/liveness can bypass a lost checkpoint; Start,
Stop, Commit, bank switches and policy/model changes still cannot bypass it. Moving
those commands safely requires explicit cross-ordering dependency identities and
recovery semantics. A partition or saturated physical uplink still has no finite
completion guarantee. This implementation does not claim otherwise. Oversized
whitelisted status records are rejected/backpressured at the smaller bound rather
than silently changing ordering. Local transmission backpressure remains a
separate condition from a successful dispatch result.

### Measurements and decisions

The pre-change Release baseline and subsequent ablations are retained under
`artifacts/wire-efficiency-baseline/`. Tests exercise the production codec and
ControlClient/ControlServer/UdpStreamConnection used by `release/jam2.exe`.
No alternate public Jam2 binary or instrumented build is used.

The 64-lane, 1,000 sequential rename benchmark initially measured v1 at 147,079
authenticated bytes and 287.83 ms; Direct at 17,093,783 bytes and 552.44 ms, STUN
the same bytes and 527.11 ms. An early v2 retained-path run measured about 79,362
bytes and 32.65 ms. Disabling just the typed rename gives about 148,293 bytes and
37.99 ms; numeric field IDs save about 111 bytes against text keys on this single
compressed initial checkpoint. Numeric packing has a modest compressed benefit
on this repetitive fixture; most gain comes from repeated edits and processing.
CPU timings are local workload totals, not network latency or GUI apply time.

In the 1-MiB WAV codec workload (43 chunks plus both directions' metadata), v1
overhead was 3,560 bytes and v2 is 3,425 bytes, compared with TCP's 10,077. This
additional saving is only 135 bytes per MiB; raw PCM dominates. Chunk size,
transfer window, hashes, sample content and star topology remain unchanged.

For 32 event-loop turns of 32 assignments to the same cell (1,024 attempted
assignments), the deterministic small-write model measured:

| Variant | Dispatched edits | Data + transport ACK datagrams | IPv4 bytes incl. ACKs |
|---|---:|---:|---:|
| Unbatched, every dispatch success | 1,024 | 4,096 | 435,904 |
| Small writes batched | 1,024 | 2,176 | 270,784 |
| Also cumulative successes | 1,024 | 192 | 59,488 |
| Also superseded unsent assignments | 32 | 128 | 13,632 |

These counts are calculated from actual production Channel write sizes: every
small write fits one reliable 1,024-byte fragment, plus its unchanged 58-byte
multiplex envelope, 28-byte UDP/IPv4 header and one 86-byte ACK. They exclude
Ethernet/Wi-Fi, IPv6, handshake, loss/retransmission, pacing and intervening
event-loop scheduling. They are not packet captures or an Internet throughput
claim. The last row sends fewer semantic operations intentionally; it has the
same final cell value, not 1,024 separately observable intermediate states.

The deterministic loss experiment suppresses primary Data for 250 ms while
retaining ordinary delay, drops, duplicates and reordering. An early independent
run delivered BankReady in 30 ms of injected network time while the dependent edit
waited 870 ms for checkpoint recovery. The final paired single-order comparison
and final native/four-peer/audio validation results are recorded below after
completion. This is a controlled impairment result, not a measured WAN percentile.

The final Release codec run (`artifacts/wire-efficiency/codec-final-ctest.log`)
measured 79,362 bytes / 32.15 ms for 1,000 typed renames; disabling the fast path
gave 148,293 bytes / 37.66 ms. Relative to the recorded v1 baseline, this is 46.0%
fewer authenticated bytes and 88.8% less codec/authentication workload time. The
concurrent Direct/STUN totals were 524.16/518.26 ms and 17,093,783 bytes each.
These are workload comparisons, not a claim that every control opcode improves
by this percentage. Compressing all small typed edits used the same 79,362 bytes
but 36.72 ms instead of 32.15 ms, supporting the retained compression cutoff.

The additional fresh-checkpoint ablation covers 100 independently bound 64-lane
snapshots, including decode and exact model convergence:

| Property representation | Compression | Authenticated bytes | Encode/decode time |
|---|---|---:|---:|
| Text keys/values | Off | 1,429,590 | 100.66 ms |
| Numeric keys/frame fields | Off | 729,690 | 111.84 ms |
| Text keys/values | Level 1 | 66,060 | 109.50 ms |
| Numeric keys/frame fields | Level 1 | 54,897 | 115.16 ms |

Numeric checkpoint fields are retained: 49.0% fewer bytes before compression and
16.9% fewer after compression on this fixture. The compressed variant costs about
5.2% more checkpoint CPU (about 0.057 ms per checkpoint here). It does not imply
the same saving on random hashes or arbitrary recipe data. The failed eager
numeric implementation is not retained; the lazy/typed update path is essential
to avoiding that earlier CPU regression.

The final native burst timings were 13.61/14.61/11.25/5.29 ms for variants 0..3.
Batching alone has a small CPU cost here, retained for the packet reduction.
Cumulative receipts reduce calculated IPv4 bytes by 86.4% while preserving all
1,024 dispatches; the same-cell coalescing case reduces them by 96.9% with only
32 surviving assignments. Under the native harness, assignment enqueue-to-drain
means were 92/99/98/1 microseconds and maxima 188/160/177/2 microseconds. Earlier
assignments in an uncoalesced 32-message burst include the time to enqueue the
remaining messages. The coalesced case measures its surviving final assignment.
These values are neither GUI-thread percentiles nor sparse-input real-device
latencies; the real-run counters exist to measure those separately.

The first complete integration round passed all eight selected Release CTests:
UDP control transport 18.62 s, exactly-four-peer normal/rotation transport
2.77/18.97 s, Server listening 19.85 s, shared-content 81.90 s, recovery 71.79 s,
sharing races 9.90 s and matched-mode audio latency 61.90 s. The full record is
`artifacts/wire-efficiency/integration-first-ctest.log`. The paired deterministic
ordering experiment measured BankReady at 870 ms with single ordering and 30 ms
with independent ordering; the dependent edit arrived at 870 ms in both cases.
Both variants experienced four deliberately suppressed primary datagrams.

In that audio comparison, Direct's incoming estimate median/p95 were 6.6/8.2 ms,
Server's 7.7/9.4 ms: differences +1.1/+1.2 ms, within the existing unchanged
packet/callback budgets plus 0.1-ms display resolution. Playback-ring median was
2.667 ms Direct and 4.354 ms Server; output path remained 2.667 ms in both, and
displayed network/jitter medians remained zero. Ring occupancy accounts for the
largest observed component difference; medians of components do not sum to the
median total. Exact causation is unproven. Callback phase and host scheduling are
hypotheses, not an established explanation or evidence that mode overhead is zero.

This single CTest contains four independent jam/process starts in ABBA mode order,
two per mode, with four peers and 12 directed receive paths. Eight one-second
observations per path/run produce 192 pooled observations per mode. They are
correlated: paths share processes, repeated samples share a run, and diagnostics
may refresh only every two seconds. The per-run total medians were Direct 6.5,
Server 7.4, Server 7.9, Direct 6.7 ms. Consequently 192 observations do not mean
192 independent mode trials, and this pass is not statistical proof of equivalent
physical audio latency. Raw rows are in
`artifacts/wire-efficiency/audio-latency-first.jsonl`. The final rebuilt run is
reported separately rather than discarding an inconvenient earlier result.

The final rebuilt audio comparison also passed in 61.90 s. Direct measured
median/p95 6.8/9.3 ms and Server 7.0/9.5 ms, differences +0.2/+0.2 ms. Playback
ring medians were 3.854 ms Direct and 4.021 ms Server; network/jitter remained
displayed zero and both output paths remained 2.667 ms. Per-run total medians in
ABBA order were 6.7, 6.9, 7.0 and 7.5 ms, using the native test's upper-median
quantile convention. The earlier +1.1-ms median difference
therefore did not reproduce at that magnitude. It is still inappropriate to
declare the difference purely scheduler noise or claim physically identical
audio latency. Together these two CTest invocations contain eight jam/process
starts (four per mode) and 384 correlated observations per mode. Both separate
results are retained, not pooled to hide run variation. The final raw data is
`artifacts/wire-efficiency/audio-latency-final.jsonl`.

### Play/Stop source invalidation: reproduced and corrected in the control owner

The final pre-fix integration selection passed nine of ten tests, but Server
listening failed after 77.15 s. After Stop/Play/Stop around a new 0.93 speed
selection, three peers reported applied revision 8 at 930,000 ppm; one remained
at revision 7 and 820,000 ppm, reporting `listening audio swap was not applied
by the engine`. The test polls all four real GUI states until its unchanged
60-second deadman expires. It requires idle phase, applied speed and musical
rate both 930,000 ppm, pitch 500 cents, focus enabled at 900 Hz, and global
transport stopped. This is an application-state convergence failure, not merely
a display-rounding or test-threshold failure.

The failing peers' final wire diagnostics showed zero invalid multiplex packets,
encoding rejects, queue rejects, dispatch rejects and pending dispatch receipts.
That does not prove every network timing was irrelevant, but it distinguishes
successful authenticated dispatch from a failed local source application.
The same failure signature is preserved from before the efficiency work in
`artifacts/listening-retry-integration-failure/`. The new failure and per-peer logs
are retained in `artifacts/wire-efficiency/listening-final-failure/` and
`artifacts/wire-efficiency/integration-final-ctest.log`.

The confirmed controller defect was a time-of-check hole. Scheduled preparation
was invalidated/retried on changed content only while `engine_frame < target`.
The engine's existing prepared-source unload cancels queued listening swaps.
If source invalidation occurred at/after that target before the GUI observed the
matching source cookie, the GUI skipped content recovery, waited the unchanged
200-ms publication grace, and treated the missing source as terminal. Desired
Server revision 8 survived at the authority, but the affected peer fell back to
its previously applied GUI selection without scheduling recovery. The precise
instruction interleaving of the original real-time run was not captured; the
late-invalidation condition itself is now deterministic in a native test.

`testServerContentInvalidationAfterBoundary` reproduced the terminal-failure path
against the previous production controller: the new native CTest failed before
the fix (`listening-late-boundary-red-ctest.log`). The test advances a scheduled
peer past target plus publication grace with no matching source cookie, then
invalidates its content. Three other controller peers retain their successfully
applied authoritative state. It also covers explicit invalidation while the
content signature remains identical, asynchronous file ownership, and the fresh
source's matching-cookie application/result. This uses the owned production
controller with deterministic engine/context injection, not sleeps to hit a race.

The content owner now notifies `ListeningToolsController::sourceWillChange()`
before clearing the prepared source. MainWindow only forwards that lifecycle
event, guarded by Jam2 Server strategy. The controller retains the latest shared
selection and cancels its obsolete preparation even after the planned boundary;
it does not call the ordinary passed-boundary finalizer recursively. The periodic
content check also handles late invalidation when a shared Server selection
exists. Private listening, disabled sync and TCP retain their existing paths.

When current content becomes ready and recording/transport gates permit, the
existing poll starts a new render generation, stages a fresh source cookie and
chooses a future sensible boundary. It reuses the same authoritative revision,
does not publish a new creator decision, does not replay healthy peers, and emits
`jam.listening.applied` only after actual cookie/tempo confirmation. An explicit
cancellation or newer revision still supersedes it. Genuine render/staging
failures remain reported rather than being retried indefinitely. There is no
new retry timer, grace extension, audio buffer, PCM/media packet or engine/callback
implementation change.

New Server diagnostics `server_source_invalidations` and
`server_post_boundary_deferrals` distinguish explicit source lifecycle events and
late deferrals from the existing general content-deferral count. Deterministic
tests verify cleanup of the obsolete file, no retry while content is unavailable,
fresh generation/cookie ownership, original-revision acknowledgement, healthy
four-peer behavior and legacy/private-mode isolation. The native test is green
after the fix (0.29 s). Two unchanged real four-peer Server GUI workflow runs then
passed consecutively, 19.87 and 21.11 s; records are
`listening-source-fix-native-final-ctest.log` and
`listening-source-fix-repeat-ctest.log` under `artifacts/wire-efficiency/`.

The matched audio comparison on the final source-fix build passed in 61.86 s:
Direct median/p95 6.7/9.1 ms; Server 7.7/9.4 ms; differences +1.0/+0.3 ms.
Per-run upper medians in ABBA order were 6.7, 7.9, 7.0 and 6.7 ms. Thus a roughly
1-ms median increase did recur. Across the three comparisons during this work,
the pooled median differences are +1.1, +0.2 and +1.0 ms, all within the unchanged
test budgets. It is not valid to dismiss the difference as proven random noise
or claim zero Server overhead. There are now 12 independent jam starts, six per
mode, with 576 correlated observations per mode. That is still a small number of
independent mode trials. Raw post-fix rows are retained in
`artifacts/wire-efficiency/audio-latency-post-fix.jsonl`. The test measures the GUI
estimate with fake audio on loopback, not physical acoustic input-to-output delay;
isolating the recurring difference would require focused receive/mix/callback
timing measurements. No audio-path changes or threshold relaxations were made.

Final source-fix validation passed all six directly selected Release CTests:
four-peer shared content 79.93 s, matched-mode audio latency 61.86 s, four-peer
TCP listening 38.30 s, four-peer Server listening 21.25 s, wire units 0.06 s and
wire performance 1.68 s. The Server GUI workflow has passed three consecutive
runs after the fix, including the two repeated runs above. Native controller
coverage additionally passed at 0.29 s, including explicit late invalidation,
four-peer catch-up and private/TCP isolation. Full output is in
`artifacts/wire-efficiency/source-fix-final-validation-ctest.log`. Earlier UDP
transport/loss, four-peer rotation, recovery and sharing-race passes remain in
the earlier integration records; those transport implementations did not change
during this controller fix. The earlier failures and negative native reproduction
remain archived. No full-suite or coverage run was performed.

| Efficiency experiment | Decision and boundary |
|---|---|
| Small-record batching and cumulative successes | Keep; zero-delay event-loop turn for scalar bursts/results, immediate primary command barriers; no transport ACK redesign |
| Avoidable processing | Keep cached validation, lazy replacement/bound accounting, single Server dispatch validation and 128-byte compression cutoff; reject eager numeric serialization and compress-every-small-edit variants |
| Typed edits | Keep the measured single-rename wire fast path; full model-owner intent migration and other typed track edits remain future work |
| Unsent supersession | Keep only the four scalar assignment types; preserve surviving revision order and barriers; no generic dropping of snapshots/transactions |
| Independent ordering | Keep bounded self-contained replies/status; model/policy/Start/Stop/Commit remain ordered together until explicit cross-ordering dependencies are designed |
| Packed flags, enums and numeric checkpoint fields | Keep v2 layouts and bounded escapes; coordinated v2 test peers are required |
| Header/HMAC consolidation | Deferred as requested; sizes, tag strength and nested authentication remain intact |

The independent path requires the native v2 Server UDP transport on both ends;
there is no per-peer feature-bit negotiation or compatibility fallback for a
third-party v2 transport lacking channel 3. The optional transport API is also
used by native ablation fixtures to keep all records on one ordering. Those
fixtures are not a negotiated production interoperability mode.

## Investigation of the recurring Server audio-delay estimate

The extra delay is visible in buffering, including fresh engine reads, not just
in rounding the GUI pill. This investigation added diagnostic test capture and a
native wait benchmark only. Production `release/jam2.exe` remained byte-identical
to the source-invalidation fix (SHA-256
`CB635AB7F15839A4358C1DEB25E29DA514523ED9EE1C504049D8527DB249E7F2`). No audio packet,
callback, engine, buffer setting, playback algorithm or wait implementation was
changed to obtain these results.

### Retained stats and queue evidence

`ModeAudioLatencyIntegration.cpp` now retains each peer's existing CSV/log output
before successful temporary roots are deleted, and writes complete per-sample
engine/transport snapshots alongside the original GUI estimates. The two further
ABBA comparisons passed their original unchanged budgets in 61.68/61.98 s:

| Diagnostic comparison | Direct median / p95 | Server median / p95 | Server minus Direct |
|---|---:|---:|---:|
| Round 4 | 6.6 / 8.1 ms | 6.9 / 9.2 ms | +0.3 / +1.1 ms |
| Round 5, complete retained stats | 6.0 / 8.4 ms | 6.9 / 9.3 ms | +0.9 / +0.9 ms |

Together with the three earlier comparisons, median differences were +1.1, +0.2,
+1.0, +0.3 and +0.9 ms. These comprise 20 jam/process starts, ten per mode, and
960 correlated path observations per mode. They still do not represent 960
independent trials or physical acoustic latency measurements.

Round 5 retained all 16 peers' CSVs and logs, the original GUI rows, and 128 fresh
snapshots under `artifacts/wire-efficiency/audio-investigation-round5/`. Round 4's
complete GUI rows/CTest output and the first two runs' CSVs are under
`audio-investigation-round4/`; the build script cleaned the remaining successful
temporary roots, so the next comparison used standalone CTest to retain all data.

All 16 final CSV configuration rows agree: fast profile/48 kHz/64-frame packets,
128-frame headless callback, 64-frame prefill, 4,096-frame playback-ring capacity
and 1,024-frame playback maximum. Thus differing configured buffer sizes do not
explain this comparison. Output-path contribution stayed 128 frames (2.667 ms).

Fresh ring reads averaged 174.266 frames (3.631 ms) in Direct and 200.047 frames
(4.168 ms) in Server: +25.781 frames, or +0.537 ms. The contemporaneous cached GUI
ring values differed by +0.593 ms in their means; their pooled medians differed
more. The GUI is a periodically sampled estimate, and medians of mixed components
or differently shaped distributions cannot be added/subtracted as a causal
decomposition. Across the first three comparisons, arithmetic mean ring increases
were +1.021, +0.364 and +0.782 ms, while mixer-queue mean differences were small.

Callback-consumption counters also confirm the queue-level difference. For each
peer, take the first periodic CSV row at/after 4 s and the last at/before 13 s,
then compute the delta of `depth_under_2ms_frames + depth_under_5ms_frames` divided
by the delta of `depth_observed_frames`. Averaging those per-peer percentages
gives 71.05% Direct versus 30.23% Server below 5 ms. These counters classify the
readable source depth before each pop and weight requested source frames,
including missing frames; they are not continuous-time averages or independent
GUI samples. Resampled source-frame counts need not equal device output frames.

The mean of sampled mixer **rolling service-gap peaks** was 1,965 us Direct and
2,348 us Server, with observed maxima 1,976/2,481 us in those samples. This field
is not an average duration per worker invocation. Mean sampled receive-loop gap
averages were approximately 0.397/0.470 ms. Mean callback intervals were both
2.6672 ms, and capture-ready dispatch averages were approximately 14 us in both.
This points toward receive/deadline servicing rather than a larger callback or
slower capture-ready notification. Derived summaries are retained as
`csv-summary.json`, `wake-summary.json` and `cushion-summary.json`.

Adaptive buffering is active in both modes. Targets sometimes rise following
shortages and release again; there was no observed delivery-gap hold or
delivery-gap target raise in these samples. The 30-second delivery-gap hold is
therefore not a supported explanation here. Existing padding/underrun/release
counters and the mixer algorithm explain how changed delivery cadence can leave
different output-ring occupancy even with identical configured capacities.

For all eight Server run/receiver groups in Round 5, control bytes sent and
received were constant across the measurement window. Budget-deferral counters
were also already present from startup rather than advancing during the samples,
and audio queue drops were zero. Consequently ongoing encoded control traffic
or batching was not consuming bandwidth during this measured increase. Startup
timing can still influence buffering; this is not an ablation of every startup
effect of the new encoding.

### Measured Windows wait-path penalty

Direct normally receives in the runtime network worker. Server first receives
in its multiplex socket worker, puts the unchanged datagram in its audio FIFO,
and wakes that runtime worker. There is an extra scheduling handoff. In addition,
the existing wait APIs handle deadlines differently:

- Without a capture-wake handle, Direct `UdpSocket::recv_from_for` passes the
  microsecond timeout to `select`.
- Server `UdpMultiplexTransport::receiveFor` uses `RealtimeWakeSignal::waitFor`.
  On Windows this computes `(timeout_us + 999) / 1000` and calls
  `WSAWaitForMultipleEvents`, rounding a sub-millisecond budget upward.
- Direct's capture-event wait also uses the coarser event API. Therefore the
  distinction is which path is used, not a claim that every Direct receive wait
  is more precise.

The runtime requests waits of at most 1,000 us and can request smaller waits to
meet its next audio-send deadline. Timeout API rounding plus host scheduling can
therefore postpone the next receive/mix/send cycle beyond its requested budget,
changing packet batching and cushion/release decisions. These methods predate
the current wire-efficiency changes; the audio receive/wait code is unchanged.

The new `jam2_udp_receive_wait_performance` CTest exercises these exact production
APIs, rotating four methods over 48 samples per requested deadline, both without
an explicit timer-period request and with a scoped Windows 1-ms timer request.
It does not reimplement the waits or modify their behavior. Under the controlled
1-ms timer condition, a 250-us request measured:

| Existing wait method | Mean | Median | p95 |
|---|---:|---:|---:|
| Direct socket/select | 983 us | 982 us | 995 us |
| Queued transport/event | 1,511 us | 1,555 us | 2,070 us |

The event wait adds about 528 us in the mean and 1,075 us at p95 in this empty-wait
experiment. For 500-us requests, means were 986/1,470 us; for 750-us requests,
991/1,560 us. These are useful measured scheduling penalties of the same order as
the extra ring occupancy, not a promise that `select` meets a 250-us deadline.
Both APIs remain subject to Windows scheduling.

The default-timer standalone condition measured roughly 15-ms waits and is not
used as an estimate of the running Qt GUI's latency. The benchmark's explicit
1-ms timer condition is controlled and reported; the GUI's exact effective timer
resolution was not independently queried. Socket-data/capture notifications can
also wake a real wait before its timeout, whereas the benchmark intentionally
measures empty timeout completion. The test passed in 23.20 s, and existing native
UDP/wake units passed in 0.03 s. Records are
`receive-wait-timer-ablation-ctest.log` and
`udp-units-after-wait-profile-ctest.log` under `artifacts/wire-efficiency/`.

**Diagnosis and remaining attribution:** a real receive-wait scheduling penalty
exists in the current Windows Server path, alongside its extra worker handoff.
The actual jam stats show larger service gaps and higher ring occupancy while
control traffic is idle. This is a concrete mechanism for additional software
buffering, rather than evidence that the binary control payload itself adds an
audio delay. The evidence does not yet prove what fraction of the complete
end-to-end offset belongs to timeout rounding versus the handoff, startup phase
and adaptive buffering. Isolating that fraction requires a controlled Server-only
deadline-preserving wait experiment and the same CSV/callback measurements.
That would change audio transport scheduling, so it is separate from this
control-only implementation and has not been applied as an unsolicited fix.

### Authorized Server receive-path experiment

The subsequent request explicitly authorizes testing a closer match to Direct:
one runtime network worker reads the established Server UDP sockets, returns
native `JAM2` packets in its existing receive buffer, and classifies `J2MX` and
directory traffic into bounded ingress queues for the control worker. Audio
packet bytes, authentication, DSP, jitter and playback policies are unchanged.
Direct/STUN do not opt into this transport or its receiver lifecycle hooks.
This experiment stays on `UDP-Multiplex` and in Jam2 Server mode.

The control worker owns discovery/candidate reads before runtime attachment.
Attachment waits for an exclusive handover (2 s failure bound); detachment
returns reads to that worker. Pre-attachment native media is discarded and
counted rather than building a stale audio FIFO. Runtime reads hold a dedicated
reader guard, independent of control parsing; shutdown obtains the same guard
before closing sockets. Immutable route leases keep socket storage alive across
membership changes and reject removed routes. The Server send path uses those
same leases, removing contention with the control worker's model mutex.

Classification checks the source endpoint and fixed magic bytes; authentication
and reliable-stream processing remain with their existing processors. There
are two preallocated 128-packet queues (1500-byte slots): control/directory/ACK
priority and bulk asset data. The control worker drains at most 64 from each per
turn. Full queues discard without ACK so reliable streams retransmit; WAV still
uses the recording-peer/creator/other-peer star. The runtime checks sockets in
round-robin order and yields after 64 non-native packets per call, bounding work
before other runtime duties. Kernel socket FIFO ordering still applies, so
bulk packets ahead of audio can still cost classification work.

Runtime waits are capped at 1000 us. With no capture wake, the new multi-socket
wait preserves the microsecond `select` deadline; on Windows capture-event
waits and more than `FD_SETSIZE` sockets retain the existing event wait fallback.
OS scheduling can exceed requested waits. Classification timing is sampled
every 64 packets to limit observation cost. Queue depths/high-water/drops,
queue residence time, native delivery/discards, work-budget yields and reader
handovers are exposed in existing structured multiplex diagnostics. These
bounds and the 2 s handover limit are explicit implementation choices, not
claims of hard real-time scheduling. The ingress copy and short queue mutex
apply to control/WAV only; atomic route leases and the reader guard still add
some runtime overhead.

The automation agent can select the previous queued-worker model for an A/B
comparison in the same Release executable. Normal Server startup selects the
runtime model. The native four-peer pressure case combines native packets,
control bursts and typed WAV transfers and checks handover/reattachment. A
six-run Direct/queued/runtime/runtime/queued/Direct comparison retains existing
latency budgets and records four-process CPU time over each sample window.
Results and retention decision follow after validation.

The malformed-prefix burst test initially exposed a premature return on
`receiveFor(..., 0)`: the prototype classified only one pass over the sockets
because the wait deadline was already reached. The corrected path drains
already available datagrams up to the 64-packet budget, including on a zero-wait
call. The deadline bounds waiting, not a promise to finish packet processing in
1000 us. The red test and green rerun are retained as
`receive-classification-budget-{red,green}.log`. The green native four-peer
pressure run took 6.02 s: creator native delivery 1356, control ingress 1367,
asset ingress 792, zero ingress drops, combined high-water 21/256 packets,
maximum ingress residence 935 us. Sampled classification averaged 744 ns and
peaked at 6900 ns. Timing starts after `recvfrom` and covers endpoint/magic
classification, counters and any ingress enqueue; it excludes socket reads,
waiting, and downstream authentication/processing. Maxima are sampled maxima,
not measurements of every packet. This is a loopback fake-media transport test;
the public GUI/engine comparisons use real native authenticated audio packets.

The initial six-run comparison passed in 93.03 s: median total GUI estimates
Direct/runtime/queued were 6.6/6.6/7.7 ms, p95 8.0/8.3/9.3 ms, and four-process
CPU 301.324/310.011/330.777 percent of one logical core. This was the prototype
before the zero-wait draining correction. The first corrected-path comparison
passed in 93.14 s: medians 5.6/6.5/7.2 ms, p95 8.1/8.0/9.5 ms, CPU
285.730/311.927/318.037 percent. Thus the second run still has a +0.9 ms Server
median offset relative to Direct; passing the existing budget does not establish
exact equivalence or attribute the remaining difference. Another repeat and
the real GUI WAV/recovery checks are pending.

CPU is Windows `GetProcessTimes` user+kernel time summed over the four app
processes, divided by actual wall time over the eight snapshot/sample windows
(about 9 s including command overhead). 300% means three logical cores across
the four processes, not 300% of the machine. The test driver and Go directory
server are excluded. Each comparison starts two independent four-peer jams per
mode, recording eight samples of twelve directed paths per jam: 192 correlated
GUI observations/mode and 64 fresh engine/transport snapshots/mode. GUI latency
breakdowns can reuse their existing 2 s diagnostic cache. These are software
estimates with deterministic headless callbacks, not acoustic measurements or
192 independent trials. OS scheduling, startup phase and adaptive buffering
remain potential contributors. The queued reference runs its retained receive
path in the same executable, including common added diagnostics; it is not a
byte-identical copy of an older executable. Source/configuration now also expose
the 2000 ms handover timeout in structured diagnostics.

The next six-run comparison also passed (93.01 s): Direct/runtime/queued medians
5.8/6.3/7.1 ms, p95 8.9/8.0/9.0 ms, CPU 300.562/318.545/335.195 percent of one
core. Across the two comparisons after zero-wait correction, runtime versus
queued median improved 0.7 and 0.8 ms, p95 improved 1.5 and 1.0 ms, and CPU
fell 1.92% and 4.97% relative to queued. Server minus Direct medians remained
+0.9/+0.5 ms. Raw artifacts are `receive-models-round{1,2,3}/` and corresponding
CTest logs; aggregate analysis is `receive-model-comparison-summary.json`.

The periodic CSV rows (4000 through 13000 ms of each engine run, 40 rows/mode
per comparison) provide another view. For comparisons 2/3, mean sampled mixer
worker gap maxima were runtime 1.934/1.955 ms, Direct 1.994/2.047 ms, queued
2.259/2.331 ms. Mean receive-loop gaps were runtime 0.377/0.396 ms, Direct
0.402/0.420 ms, queued 0.474/0.482 ms. Callback intervals stayed approximately
2.6672 ms. These are averages of existing periodic diagnostics, not a new
distribution of individual worker gaps. Fresh snapshot mean ring occupancy
was runtime 162.906/188.500 frames, Direct 174.141/168.438, queued
194.031/192.594. The newer path improves servicing, but startup/buffer phase
variation and the GUI cache prevent claiming identical latency from these runs.

**Full-size multiplex regression found during validation:** the real four-peer
unthrottled WAV test failed initial reconciliation three times. This case delays
native HELLO/ACK proof for 4 s; artificial multiplex loss is disabled. Audio
continued, while the fourth peer missed one primary fragment and its sender
hit the existing 15 s acknowledgement timeout. Bounded fixture header tracing
proved eleven sends of that same fragment were forwarded, while subsequent
fragments were ACKed. The prototype had reused `NetworkSession`'s native audio
receive storage (36 + 256*3 = 804 bytes), although the current multiplex maximum
is 42 + 1024 + 16 = 1082 bytes. `UdpSocket` deliberately treats oversized UDP
receives as discarded datagrams (Windows `WSAEMSGSIZE`, POSIX `MSG_TRUNC`), so
every retry of a full fragment was discarded before classification. This was a
prototype integration bug, not evidence of network loss or WAV star-topology
failure. Failed runs and the trace are preserved in `runtime-wav-*-failure/`.

The fix allocates one 1500-byte receive buffer on transport attachment, owned
by `NetworkSession` and used directly by the Server runtime reader. Direct
retains its native 804-byte buffer and socket path. There is no native packet
copy or FIFO added. Allocation happens before receiver registration; failed
registration leaves the existing transport intact. Detachment/destruction
release the storage. The Server API now rejects an undersized caller buffer
explicitly, and its required size is exposed in structured diagnostics. A
native real-UDP regression covers 804, 805, 1082 and 1500-byte datagrams; it
failed before the fix and passed afterward (0.09 s), with red/green logs
retained. Four-peer transport coverage also now attaches readers before the
third/fourth peers join, exercising immutable route replacement during receive.
The fixture's optional `JAM2_TRACE_CANDIDATE_MUX` records at most 1024 packet
headers and writes them on teardown; it is test-only observation. Real WAV,
recovery and final-size-buffer performance validation follow.

The full-size-buffer regression fix passed the previously failing public-app
WAV test in 16.03 s. A 4,194,348-byte file reached all three recipients with
verified contents in 5879 ms (aggregate application payload 2,140,337 B/s).
The delayed native proof fixture withheld 54 proof packets; no usable audio
path was falsely failed. Every receiver continued direct native delivery.
Per-peer ingress high-water was 8/7/8/7 packets, drops 0/0/0/0, maximum queue
residence 137/299/317/206 us, sampled classification mean 455/281/287/282 ns
and sampled maximum 2700/2600/4200/2500 ns. Three subsequent large arrangement
changes also converged while audio continued; proposal traffic was 4965 bytes.
The control worker's maximum service times were 358/384/346/357 us. Its
approximately 11.2 ms loop-gap maxima include the existing 10 ms idle wait;
ingress and outbound commands wake it immediately rather than waiting for
that timeout. The record is `runtime-wav-buffer-green.log`.

Limits retained intentionally: priority hints are classified before HMAC but
authentication is unchanged in the control worker. A faulty authenticated
peer can still create contention on its UDP FIFO or fill the bounded ingress
queues. The 64-packet budget is per receive call; the existing runtime may
consume up to 64 native datagrams per wake, so a constructed mixed flood can
cause more classification work over a whole wake. This is not a global CPU
quota or a hard latency guarantee. The native flood test proves the call
budget, not immunity to every sustained overload pattern. Native media before
attachment is intentionally not retained; normal native proof retries must
complete after handover. The old 512-packet native FIFO storage remains in the
transport for the selectable queued reference, unused by the runtime model.

Asset ACKs remain in the priority/control budget at the existing 65,536 B/s
default. With a 58-byte ACK plus 28 bytes modeled IPv4/UDP per 1024-byte data
fragment, that budget alone permits roughly 780 kB/s bulk payload per receiver
before other control traffic. The measured three-recipient aggregate is
consistent with this ceiling, but the test does not isolate the ceiling from
congestion windows, file work and scheduler costs. Raising or separately
budgeting ACK traffic could improve WAV throughput; that is a subsequent
experiment. Header/HMAC changes remain deferred as requested. Other possible
follow-ups are a measured SPSC replacement for the short ingress mutex,
readiness-guided socket probes for larger meshes, and a budget across an entire
runtime wake if sustained mixed floods prove important. No such follow-up is
silently included here.

**Final validation and retention:** keep the runtime reader as the Jam2 Server
default on `UDP-Multiplex`. The final Release executable SHA-256 is
`90DE9828042DC69EB83F93DB3235A58A438BEC0A67B7736B2D35BC98F6D79A54`.
The isolated comparison after the 1500-byte-buffer correction passed in 93.12 s:

| Receive model | Median GUI estimate (ms) | p95 (ms) | Four-app CPU (% of one core) |
| --- | ---: | ---: | ---: |
| Direct | 5.9 | 8.1 | 322.838 |
| Server runtime reader | 6.6 | 8.0 | 328.659 |
| Server queued reference | 7.6 | 9.4 | 335.385 |

That is a 1.0 ms median reduction, 1.4 ms p95 reduction and 2.01% relative CPU
reduction versus queued Server. The final runtime median is still 0.7 ms above
Direct, while p95 is 0.1 ms below it. Across the three comparisons after the
zero-wait correction, median improvements versus queued were 0.7/0.8/1.0 ms;
the Direct offsets were 0.9/0.5/0.7 ms. This supports retaining the architecture
and its simpler native delivery path; it does not establish zero Server
overhead. The final-size-buffer comparison contains two independent jam starts
per mode and 192 correlated GUI observations per mode. Its raw rows, fresh
snapshots and 24 app CSV/log sets are in `receive-models-round4/`; the matching
CTest log is `receive-models-round4-ctest.log`. Earlier comparisons retain the
prototype's smaller receive buffer, so they must not be represented as four
identical-build repetitions. Audio packet representation and DSP are unchanged.

All 11 scoped Release CTests passed in 260.70 s: UDP control/transport units,
four-peer listening, shared content, recovery, sharing races, unthrottled WAV,
both public-app audio/bootstrap cases, pressure and candidate rotation. The
separate exactly-four-peer partial-edge failure case passed in 40.28 s. Along
with the final receive-model performance test, this is 13 distinct passing
CTest cases, without running the full suite. The pressure fixture's callback
cleanup was rebuilt and rechecked (5.67 s) after the grouped run. Logs are
`runtime-receive-final-scoped-ctest.log`, `runtime-receive-partial-ctest.log` and
`runtime-receive-final-pressure-ctest.log`.

The second final WAV pass took 5824 ms (2,160,550 B/s aggregate), had zero
ingress drops, per-peer high-water at most 11 packets, maximum residence
318 us and sampled classification means 260–516 ns. Final native pressure
likewise had zero drops; the grouped run high-water was 28 packets, maximum
residence 602 us and sampled classification mean/max 660/7100 ns. These
measurements support prompt categorization and control-worker wakeup under
the exercised loads. They do not measure acoustic latency during saturated
WAV transfer, WAN/Wi-Fi contention, hardware ASIO timing or large experimental
meshes. Liveness timestamps written by the runtime reader are atomic; worker
age calculations guard against a newer timestamp than their current cycle's
clock sample to avoid unsigned-age underflow. Further attribution of the
remaining GUI/Direct offset remains a measured follow-up, not something this
experiment has proved away.

**Follow-up: computation time versus the remaining 0.7 ms GUI median.** The
final comparison's component means (rather than adding component medians)
were Direct/Server: playback ring 2.6693/3.3288 ms, mixer queue
0.7745/0.4008 ms, output fallback 2.6667/2.6667 ms. Displayed network and jitter
contributions were zero in both modes on loopback. Thus the cached GUI samples
have +0.6595 ms ring occupancy and -0.3737 ms mixer occupancy in Server mode.
The mean total estimates differ by +0.2927 ms (6.1198 versus 6.4125), while the
pooled median totals differ by +0.7 ms. Component medians do not add to the
median total, and the median difference is not a measured per-packet transport
penalty. Derived component data are retained in
`receive-model-final-latency-components.json`.

Classification timing covers only the short processing section after socket
read, excluding kernel delivery/waiting, runtime scheduling, mixer servicing
and output callback timing. At 48 kHz, a 64-frame packet spans 1.333 ms and a
128-frame callback 2.667 ms; processing a packet in microseconds does not imply
that its samples are consumed immediately. The observed GUI difference is in
buffer occupancy. Why that occupancy differs is still unresolved. Fresh ring
snapshots from the same final run averaged 173.859 frames Direct versus
178.125 Server, only +4.266 frames / +0.0889 ms, much smaller than the cached
GUI ring difference. The production connection diagnostics refresh on their
existing 2 s schedule, while automation reads fresh engine snapshots about
once per second; they sample different instants. Startup/packet/callback phase
and periodic sampling can explain an apparent offset in principle, but the
current data do not prove that explanation or rule out a residual scheduling
cost. Distinguishing these requires aligned timing of native receive, runtime
processing, mixer handoff and output consumption, together with uncached
component samples. The classifier microbenchmark alone cannot supply that
attribution. No audio or scheduling code was changed for this follow-up.
