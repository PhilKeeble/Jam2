# Jam2 Server wire format review

Implemented on `UDP-Multiplex`. This records decisions actually made during
implementation, their consequences, unresolved limits, and validation results.
The earlier discussion is in `UDP-Multiplex-WireFormat.md`.

**Current implementation: experimental Server application wire v3, outer
multiplex v2.** The final typed-coverage implementation/results section supersedes
the earlier application layouts where stated. The earlier v2 efficiency and
independent-ordering changes remain in place. Measurements and limitations below
are retained as review history, not silently rewritten.

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

**Correction from the subsequent source review:** asset ACKs use the priority
ingress queue and control byte counters, but `muxSend` exempts the entire asset
channel from rate limiting when `assetBytesPerSecond == 0`, including ACKs.
The earlier hypothesis that the 65,536 B/s control budget caps uncapped WAV
throughput at about 780 kB/s per receiver was incorrect. That arithmetic would
apply if every 86-byte IPv4 ACK were charged to that budget, but this code does
not do so. The observed WAV throughput does not establish an ACK-rate ceiling.
Individual ACKs still cost 58 multiplex bytes plus 28 modeled IPv4/UDP bytes,
so reducing their packet count remains worth measuring. Header/HMAC changes
remain deferred as requested. Other possible
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

**Peer identity on the current wire:** the application identity is a random
128-bit peer token, represented as 32 hex characters at the Qt/JSON boundary.
The existing runtime numeric peer ID is the nonzero uint64 obtained by reading
the first eight token bytes in big-endian order; it is derived, not separately
assigned by the creator. Typed binary identity fields carry the raw 16 bytes
(with the existing optional/empty markers where applicable). Membership
entries send the token once plus the endpoint and reconstruct `peer_id`, so
they do not transmit a redundant eight-byte numeric ID. There is no compact
creator-assigned u8/u16 peer-handle dictionary in this experiment.

Ordinary control records infer their sender from their authenticated channel.
The handshake transcript binds the peer token and derives per-connection,
directional application keys; dispatch supplies that channel's authenticated
token to the controller. The 32-byte application record header therefore has
no peer-ID field. Likewise, the 42-byte multiplex prefix carries session,
introduction, logical-stream and sequence IDs, not a repeated peer token.
The receiving socket/expected remote endpoint select the link, then the frame's
introduction, stream and HMAC are checked. Endpoints are routes and may change
through repair; they are not the application identity. Explicit references to
other peers, such as authority, participants or recording source, use the
typed token fields. Cold JSON bootstrap messages still use textual tokens;
generic document values are not all rewritten into binary identity fields.
The unchanged native HELLO/ACK payload carries the derived eight-byte numeric
ID in its existing little-endian encoding; normal native packet headers do not
gain a peer field. This describes existing behavior; no protocol change was
made for this identity review.

### Read-only latency and multiplex efficiency review, 2026-10-27

The request to investigate the remaining GUI discrepancy was extended with an
explicit instruction not to change code. No application or native test source
was changed, no executable was rebuilt, and the public Release SHA-256 stayed
`90DE9828042DC69EB83F93DB3235A58A438BEC0A67B7736B2D35BC98F6D79A54`.
This section records source inspection, retained-log analysis and new runs of
the existing native latency driver. The current build cache has
`BUILD_TESTING=OFF`; an attempted CTest invocation found no registered tests and
is not counted as validation. The already-built native CTest driver was then
invoked directly with its normal public executable and real Go directory
arguments. All three ABBA runs passed the existing, unchanged latency budgets.

| New run | Direct GUI median (ms) | Server GUI median (ms) | Server minus Direct (ms) | Direct p95 / Server p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| 1 | 6.0 | 6.7 | +0.7 | 8.3 / 9.0 |
| 2 | 6.7 | 6.0 | -0.7 | 9.3 / 8.3 |
| 3 | 6.8 | 6.3 | -0.5 | 9.3 / 8.1 |

Each run starts Direct/Server/Server/Direct four-peer jams. Together these are
12 jam starts, six per mode, with 576 correlated path observations and 192
fresh receiver snapshots per mode. They are not 576 independent trials. The
mean of each jam's GUI mean was Direct 6.6616 ms and Server 6.3167 ms. This
does not establish a fixed Server advantage either; it does refute treating
the earlier +0.7 ms GUI median as a reproducible fixed transport surcharge.
The raw data and 16 app log/CSV sets per run are in
`artifacts/wire-efficiency/latency-sampling-abba-{1,2,3}/`, with `native-run.log`
containing each native driver's result. Derived data are
`latency-sampling-abba-summary.json` and
`latency-cached-versus-fresh-review.json`.

For the new run that reproduced the +0.7 ms GUI median, cached GUI mean ring
depth was Direct 2.995 ms versus Server 3.511 ms (+0.516 ms). Fresh engine ring
snapshots instead averaged Direct 3.732 ms versus Server 3.672 ms (-0.060 ms).
The mean GUI mixer difference supplied the remaining approximately +0.057 ms
of the GUI mean-total difference (+0.572 ms; a median difference need not equal
a mean difference). This is the key evidence for a sampling discrepancy:
the positive cached ring offset is absent in the fresh ring samples. The
callback-consumption counters also look similar in that run: 69.84% Direct
versus 70.50% Server of requested source frames were consumed with less than
5 ms readable depth. Mean sampled mixer worker gap maxima were 1.974/1.954 ms
and callback intervals approximately 2.6672 ms in both modes.

The other two new runs had fresh Server ring means lower by 0.166 and 0.236 ms.
Their cached GUI ring differences were -0.718 and -0.913 ms. In the older
three-model runs, every mode had exactly 24 unchanged adjacent GUI snapshots
and 32 changed pairs out of 56 receiver-adjacent pairs: the one-second test
polls were repeatedly reusing the existing two-second diagnostics. The source
samples instantaneous per-peer mixer/jitter depth plus shared playback depth
after receive processing, then retains that snapshot for its GUI interval.
It adds interval RTT/2 and a constant output-path estimate. This is not a
sample-age measurement and not a time-weighted queue occupancy average.
Packet production (64 frames = 1.333 ms), mixing and output consumption
(128 frames = 2.667 ms) change these instantaneous depths. Sampling different
positions in those cycles, with differing startup phase, is the strongest
explanation for the GUI discrepancy. The fixed overhead interpretation is
unsupported; the exact contribution of each phase or any residual scheduling
cost is not causally isolated by these observations.

Diagnostic interpretation matters: `playback_depth_avg_frames` in per-peer CSV
rows is sampled from the PeerStream's playback sink, which is the per-peer
mixer queue in mesh mode; it is not the shared output ring. The ring counters
named `depth_under_2ms`, `depth_under_5ms` and `depth_under_10ms` are exclusive
bins (<2, 2–5, 5–10 ms), not cumulative thresholds. The new below-5 calculation
adds the first two bins and uses differences of cumulative counters over
4000–13000 ms. Its denominator is requested ring-source frames, not elapsed
time. The three runs recorded 133–397 missing ring-source frames per mode per
run, over approximately 3.073 million observed frames (0.0043–0.0129%), zero
playback-dropped frames and callback mean intervals 2.66718–2.66724 ms. The
underrun counts are not silently treated as zero or acoustic measurements.

To resolve per-packet receive-to-consumption timing, rather than infer it from
these snapshots, the next measurement needs sample timestamps/ages, aligned
queue components and callback/packet phase, ideally interval distributions or
tagged sample age. No such instrumentation was added under the read-only
instruction. This investigation does not establish actual socket-to-speaker
equivalence, hardware ASIO latency or WAN/Wi-Fi behavior.

The following are additional efficiency candidates, not implemented changes:

1. **An in-order reliable receive fast path.** `ReliableUdpStream::receive`
   currently inserts every valid fragment into an ordered map with an owned
   byte vector, even when its sequence is exactly the next expected value,
   immediately delivers it, then erases the entry. Delivering this common case
   directly would avoid the temporary map node, vector allocation and one
   payload copy. Existing out-of-order storage, duplicate ACK behavior, sequence
   bounds and application-owned input buffering must remain correct. This is
   the clearest small CPU/allocation optimization before replacing the whole
   bounded window with preallocated slots.
2. **Transport ACK aggregation, especially WAV.** Application dispatch receipts
   already coalesce; UDP fragment ACKs are still individual authenticated
   datagrams. Eight current IPv4 ACKs cost 8*86 = 688 bytes. A hypothetical
   authenticated ACK with a contiguous base and eight-byte selective bitmap
   could cost 94 bytes including IPv4/UDP, approximately 86% fewer ACK bytes for
   that batch. This is illustrative arithmetic, not a measured gain. ACKs
   could flush at the end of a receive batch without adding a sparse-traffic
   timer. Missing fragments must never be cumulatively acknowledged; stream
   separation, duplicate/retransmitted ACK handling and version negotiation
   require loss/reordering tests. Application dispatch/commit confirmations
   remain separate and unchanged in meaning.
3. **Reuse worker scratch storage and use timer deadlines.** The worker
   constructs fresh socket/lease vectors in `run`, a link vector in
   `readSockets`, and an ordered link vector in `timers`, including idle turns.
   Retaining capacity could remove repeated allocations. Its fixed 10 ms idle
   polling could instead wait until the next actual retransmission,
   registration, budget or repair deadline, while retaining immediate command
   and ingress wakeups. This primarily targets idle CPU and allocations; it
   must not introduce later servicing of time-sensitive control messages.
4. **Retain socket readiness and reduce repeated route-lease work.** Runtime
   receive probes sockets round-robin with nonblocking `recvfrom`; the wait
   helper discards the ready set before the reader rescans. Returning readiness
   could avoid empty receive syscalls. Each send/receive also loads an atomic
   shared route pointer; retaining a safe lease across a batch may reduce
   reference-count work. A shared-pointer atomic is not guaranteed lock-free.
   For two to four peers these gains may be small or lose against added
   bookkeeping, so measure syscall/probe counts first. Socket lifetime,
   handover exclusivity, round-robin fairness and repair must be preserved.
5. **Reduce control-path copies and repeated retry encoding.** Ingress copies
   into a fixed queue, copies out, constructs a QByteArray, authenticates and
   copies reliable payload into ordered/application storage. Validating spans
   and retaining only ownership-required copies could reduce this work.
   Retries also reconstruct and authenticate an otherwise identical outer
   datagram; a bounded encoded-frame cache could trade memory for CPU under
   loss. Neither is evidence that classification is the remaining GUI delay.
6. **MTU and window experiments after the smaller changes.** The fragment
   payload is 1024 bytes: a full data datagram is 1082 UDP bytes / 1110 modeled
   IPv4 bytes. On a verified 1500-byte IPv4 path, 1414 payload bytes would fit
   the same envelope, reducing an illustrative 24,614-byte record from 25
   fragments to 18. This must be negotiated against real path MTU; it is not a
   safe blind Internet default. The sender window also caps at 32 fragments,
   approximately 32 KiB, so at 100 ms RTT the full-window payload ceiling is
   roughly 328 kB/s before loss and application overhead. Larger windows could
   help high-bandwidth/high-RTT WAV transfers, but can also build queues that
   harm audio. They need explicit bounds, stats and mixed-load tests.

The current native audio FIFO bypass, preallocated ingress queues, cached
numeric routes, event-turn batching and independent reliable reply ordering
are already present. Wake signaling already coalesces OS notifications while
pending, so “one wake syscall per fragment” would be an incorrect premise.
The shorter peer handles and common typed TCP/UDP codec are agreed design
work, not additional implemented savings. Header/HMAC consolidation remains
deferred. The source review supports measuring the candidates above, not
claiming all will improve performance or that they explain a fixed 0.7 ms cost.

### Authorized UDP handler optimization implementation

Scope is `UDP-Multiplex`, Jam2 Server control/WAV traffic only. Native audio,
Direct/STUN TCP control, peer names/handles, retry caches, MTU/window changes,
readiness APIs and deadline-driven idle waits are outside this change. Baseline
Release WAV CTest passed in 16.40 s; 4,194,348 bytes reached three recipients in
5872 ms (2,142,888 B/s aggregate), with zero ingress drops. Its executable hash
was `070BB38FD7CCADCF212C3035F7627E63E9383A1A2CC185C08E62DF8612D6955D`.
The generated build cache had been changed to the Visual Studio generator;
`compile.cmd` requires Ninja. The incompatible cache and CMakeFiles were
preserved under `build/cache-before-multiplex-optimizations/`, and the normal
MSVC Release command regenerated the build. No source was changed for that.

Implemented for measurement:

- In-order `ReliableUdpStream` receive delivers a borrowed payload directly,
  advances the contiguous sequence and drains any already-owned reordered
  successors. Only gaps allocate map/vector storage. Asynchronous application
  input retains its required owning copy. Direct-in-order and reorder-insertion
  counters expose the distinction. This class is used by multiplex streams,
  not native audio or TCP.
- Multiplex envelope version is now **2**. New authenticated kind **8** is an
  ACK window. The unchanged 42-byte prefix's sequence holds the highest
  contiguous received fragment; a zero-length payload means no selective bits.
  An eight-byte big-endian bitmap optionally acknowledges sequences base+1
  through base+64, bit 0 through bit 63. Base zero is allowed only with a
  nonempty selective bitmap. Length, HMAC, session, introduction, stream,
  ordering domain and sent-sequence range are checked. An invalid window is
  rejected before freeing any pending fragment. This is a transport receipt,
  not an application dispatch or domain-commit acknowledgement.
- ACKs flush in the current worker turn after its receive batch, without a
  timer or sparse-traffic waiting period. Primary and independent control keep
  separate ACK windows; asset ACKs remain on the asset channel and priority
  ingress queue. A failed send retains pending ACK state for another poll.
  Close/Open ACKs remain immediate. The data packet budget still excludes ACKs,
  as before when ACKs were emitted by receive callbacks.
- Worker socket/lease/read-link/ordered-link vectors retain capacity across
  turns. A typed diagnostic config can disable reuse for native CTest A/B
  comparisons. Workspace growth turns and service totals are exposed.
- The worker borrows its ingress slot until parsing finishes. Queue count
  includes that slot so the producer cannot overwrite it; RAII releases it on
  normal, stale-packet and exception paths. Parsing/HMAC use a bounded view
  rather than making another whole-packet QByteArray and payload copy.
  Out-of-order data and asynchronous GUI delivery still own their bytes.
  Outbound encoding also accepts a payload span and reserves its final packet
  capacity, avoiding an intermediate fragment QByteArray. Authentication tags
  and header size are unchanged; encoded retry packets are not cached.

The envelope change is intentionally not backward compatible with v1. All
participants in this experimental Server jam need updated builds; native UDP
proof and the Go directory protocol are unchanged. Old builds can establish
the physical route but their incompatible multiplex cannot open/authenticate
the control stream and times out. There is no mixed-v1/v2 downgrade path here.
Normal Data/ACK fields are otherwise unchanged; kind 4 individual ACK remains
available to the measurement reference. Options disabling fast in-order or
aggregated ACK behavior exercise the same production implementation in native
benchmarks; normal sessions enable both.

Initial native control/loss and four-peer pressure/fresh-workspace CTests all
passed (28.91 s grouped). Reused workspace grew on 4 of 701 worker turns;
fresh workspace grew on all 679 turns. Their total service times, 57,455 versus
55,923 us over different turn counts, do not establish a CPU speedup by
themselves. Reuse removes growth after stabilization without changing queue
bounds. Pressure measured zero ingress drops, 1308/1310 coalesced ACKs, 2023
direct in-order fragments and approximately 2.87 MB of known avoided ingress/
decode copies in either configuration. Actual allocation counts are not
instrumented; workspace-growth turns are a structural measurement.

The native `jam2_udp_handler_performance` CTest rotates five ablations over
seven samples each, using 2 MiB payloads with ordered and deterministic
loss/duplication/reordering traffic. It passed in 3.87 s. Reference/combined
median processing times were 46,822.9/40,470.1 us ordered (13.6% lower) and
59,584.7/50,282.8 us impaired (15.6% lower). Ordered ACK packets fell 2048 to 79,
modeled IPv4 bytes 2,449,408 to 2,280,074 (6.9% lower total), and reorder
insertions 2048 to zero. Under impairment, ACKs fell 2254 to 720 and data
retransmissions 225 to 127; selective feedback avoided needless repeat data
in this deterministic workload. In-order-only and view-only timing differences
were about 1% ordered and below 1% impaired, so those timings need modest
interpretation despite deterministic allocation/copy reductions. The benchmark
reference uses the new shared encoder and HMAC-view implementation; it is a
conservative component ablation, not a byte-identical older executable. It
does not include real sockets or the ingress queue; four-peer tests cover them.

The first real WAV implementation regressed to 7181, 7096 and 7757 ms despite
approximately halving ACK-heavy control ingress (creator 13,001 to 7077 in
the first run), zero ingress drops and no asset retransmissions. It is not
accepted as a useful throughput result merely because the CTest passed.
Source inspection found that fewer ACK wakeups can leave send credit unused:
the worker polls only two data fragments per stream, then may sleep despite
the existing congestion window allowing more data. The correction continues
with another immediate worker turn only after sending the full two-fragment
budget and while queued data still has congestion/window credit. A blocked
send or exhausted credit ends continuation. The per-turn budget, congestion
window limit, retransmission policy and 10 ms idle wait are unchanged. A
repoll counter makes this behavior observable. Native tests cover available,
exhausted, empty and would-block send credit. Real socket validation and the
retention decision follow; retry caches and other proposed optimizations
remain deferred. Logs and raw failed-iteration peer artifacts are under
`artifacts/multiplex-handler-optimizations/`.

#### Final measurements and retention decision

Retain all four authorized changes. ACK aggregation provides the largest
measured processing/network reduction. Keep the immediate credit-driven worker
continuation with it: aggregation without that correction caused a real WAV
throughput regression. The other changes remove identifiable allocation or
copy work with bounded ownership, although an individual elapsed-time benefit
is not established for every workload. Retry packet caching, deadline-based
idle waits, socket readiness/route-lease changes, MTU/window changes and
header/HMAC consolidation remain deferred.

The final benchmark removes a redundant fixture-only authentication pass when
selecting packet impairments. The actual production decoder still authenticates
every delivered datagram. Seven rotated samples per variant and workload gave:

| Variant | Ordered median us | Loss/duplicate/reorder median us |
| --- | ---: | ---: |
| Individual ACK / owning decode / map receive reference | 34,091.1 | 45,734.2 |
| In-order receive only | 34,420.8 | 46,149.0 |
| Borrowed decode only | 34,009.1 | 45,011.3 |
| ACK aggregation only | 28,728.2 | 36,996.5 |
| Combined | 28,574.1 | 36,733.0 |

Combined elapsed processing time is 16.2% lower ordered and 19.7% lower
impaired in this run. These are wall-clock processing measurements with a
simulated network clock, not socket delivery latency or isolated CPU cycle
counts. The slightly slower in-order-only results (about 1%) do not establish
a timing win; retain that path for avoiding 2048 temporary map/vector
insertions and payload copies in the ordered workload. Out-of-order fragments
still require owned storage. Borrowed decoding removes another payload copy;
the ingress lease and span encoder remove additional copies outside this
component benchmark. Required producer queue and asynchronous application
ownership copies remain.

A second final benchmark run confirmed the packet counts and gave combined
ordered/impaired medians 27,422.1/36,366.0 us versus reference
33,936.8/45,344.7 us (19.2%/19.8% lower). In-order-only was approximately
unchanged in that repeat, reinforcing the limit on its timing claim.

Ordered ACK datagrams fell 2048 to 79 (96.1% fewer). Including unchanged data
packets and 28 bytes of modeled IPv4/UDP overhead per packet, total bytes fell
2,449,408 to 2,280,074 (6.9%). Under this impairment pattern, total bytes fell
2,716,874 to 2,481,610 (8.7%) and data retransmissions 225 to 127. Feedback
timing changes the sequence of later sends, so this retransmission reduction
is workload-specific, not a general loss-recovery guarantee. Sparse traffic
has no intentional aggregation delay: its ACK flushes in the current serviced
worker turn, subject to existing queue/scheduler/send availability. A lost ACK
is recovered by a duplicate-data ACK or a later cumulative/selective window;
failed sends retain pending ACK state. All ACK fields remain authenticated.
RTT samples for fragments acknowledged together share the ACK arrival time;
the existing Karn rule and RTO limits remain in force.

Corrected real four-peer unthrottled WAV transfers took 5776 and 5710 ms versus
5872 ms baseline: approximately 1.6% and 2.8% shorter. These few localhost runs
show recovery of the regression, not a statistically established WAN speedup.
The last run verified the same 4,194,348-byte WAV at all three recipients,
2,203,685 B/s aggregate and zero ingress drops. Receiving peers sent
1529/1408/1363 ACKs and coalesced 2782/2896/2932 additional receipts. Each had
zero reorder insertions. The creator used 5986 immediate credit repolls.
Approximately 39.4 MB of known ingress/decode copying was avoided across the
three receivers; this is memory-copy work, not bytes saved on the network.
The WAV star topology, fragment size, congestion limits and audio path remain
unchanged.

Final four-peer pressure A/B measured 4 workspace-growth turns out of 309 with
reuse, versus 312 of 312 without reuse. Service totals were 35,461/39,014 us;
the differing turn counts and scheduler phase prevent claiming an isolated
CPU percentage improvement. Both verified identical application data, zero
ingress drops, 2023 direct fragments and 2,873,165 known avoided ingress/decode
copy bytes. These counters cover specific removed operations, not all memory
allocations or copies in the transport. Retained vector capacity is bounded
by the links serviced; existing session admission and ingress bounds apply.

Release validation used the normal elevated `compile.cmd --in-dev-shell`
MSVC workflow and `release/jam2.exe`, SHA-256
`841E814879CB83F8E4877AECE91CB1349C2169FB5F4CED8D7DFB14019B6B8883`.
The 14-test scoped run passed 13 cases: control/core units, native handler
performance, four-peer shared content, recovery, sharing races, WAV, both
application startup cases, receive pressure, fresh-buffer comparison, peer
rotation and partial admission. Listening tools failed once at the existing
restart epoch/grid assertion. The assertion requires both cached
`transport_grid_ready` and fresh `metronome_epoch_valid` to be true, without
an additional wait at that point. Its original output did not identify which
condition failed. No ingress drops or invalid ACK windows were observed in
the retained peer evidence. A stale 200 ms grid-status snapshot is a plausible
explanation, not a confirmed cause. Product behavior, assertion and timeout
were not changed to make it pass. Failure-only before/after snapshot capture
was added, followed by four successful isolated reruns; their valid grid
mapping errors stayed within the existing 128-frame bound. The original
intermittent failure remains an explicit unresolved validation limitation.

A further grouped rerun of control units, handler performance, core units and
listening tools passed all four in 41.45 s. Listening therefore passed five
reruns in total, including the grouped sequence preceding the original
failure. All 15 distinct scoped cases, including the audio comparison below,
have a passing latest run; that does not erase or explain the earlier failure.

The separate four-peer Direct/Server audio comparison passed in 61.74 s.
It used ABBA startup order, two independently started jams per mode and 192
displayed path observations per mode (correlated cached samples, not 192
independent experiments). Direct/Server median estimates were 6.6/6.3 ms and
p95 9.0/8.1 ms. Server-minus-Direct was -0.3/-0.9 ms against the existing
1.333/2.667 ms median/p95 budgets. Both modes had median playback/output
components of 2.667 ms. Process CPU was 148.8%/160.0% of one core; this
between-mode observation does not isolate these optimizations' CPU effect.
No fixed added audio latency appeared in this run; the test still measures
queue-based estimates with fake audio, not physical socket-to-speaker latency.
No audio codec, DSP, callback or runtime implementation was changed.

Detailed logs, including the initial WAV regression and the original listening
failure, are retained under `artifacts/multiplex-handler-optimizations/`.
Only directly relevant CTests were run; no full suite or coverage collection.

## Control-plane coverage inventory (2026-10-27, review only)

This inventory reviews the current `UDP-Multiplex` working tree, including the
uncommitted handler optimizations above. It adds documentation only. No protocol,
GUI, audio, TCP/STUN implementation or test behavior is changed by this audit.
The objective is fewer datagrams and bytes per useful action and per completed
distribution, while preserving deterministic reconciliation, bounded queues,
reliable important actions and prompt sparse/time-sensitive dispatch.

### Coverage result and meaning of "typed"

- There are **51 named application message schemas**, plus three special binary
  operations: AssetChunk, Receipt and diagnostic Binary. All explicit normal
  message names in `ControlMessageValidation.cpp` have a schema. The additional
  `debug.lifecycle.disconnect` is internal: the UDP server closes the connection
  locally instead of encoding it. Listening validation also delegates to its
  owner; asset-stream metadata has its own transfer validation path.
- **49 of the 51 named schemas have field-specific binary payloads throughout.**
  `song.set.song` and `looper.track.batch.offer.tracks` are the two document
  fields. They are bounded binary documents, not JSON strings on the wire.
  Song updates additionally have serial-bound generic deltas and a typed rename
  fast path. Full schema coverage therefore does not mean all nested content
  has a fixed typed layout, or that every model operation has a C++ command type.
- Ordinary application APIs still accept/return `QJsonObject`. The compiled
  schemas convert at the serialization boundary. Model-to-controller typed
  intents remain a separate improvement; do not call this an already complete
  end-to-end C++ typed interface.
- Four cold `hello.*` records and the rendezvous protocol remain JSON. Existing
  native UDP proof/clock/metronome/transport messages already have binary layouts
  and remain in their existing runtime. They are not missing application opcodes.
- Direct/STUN use the existing TCP codec. This audit inventories their migration
  requirements; it does not enable the experimental codec in those modes.

Evidence: [schemas and codec](app/application/ServerControlWire.cpp),
[operation assignments/API](app/application/ServerControlWire.hpp),
[application validation/source policy](app/application/ControlMessageValidation.cpp),
[client selection](app/application/ControlClient.cpp),
[server selection](app/application/ControlServer.cpp).

### Complete application operation register

Numbers below are decimal **u16 operation IDs**, not multiplex packet kinds.
`P` means primary reliable application ordering. `I` means the independent
reliable control ordering when available; on an asset channel the same schema
uses that asset stream's primary ordering. Both retain transport ACK/retry.
`D` means successful dispatch requires an application receipt, normally
cumulative; `-` means no positive dispatch receipt. Rejections can still produce
an explicit result. Neither symbol means execution, readiness or file validation.

Every named schema also has optional `revision:i64` and
`authority_token:identity16` slots. Presence masks are u8 for <=8 total slots,
otherwise u16. Present Booleans share a separate packed bitmap. Absent values
consume no value bytes. Domain validators decide required fields and tighter
limits; codec representability alone does not establish domain validity.

`text` = u16 UTF-8 byte length + bytes, at most 16,384 bytes before narrower
domain checks. `id16` = presence/empty marker plus 16 binary bytes when nonempty;
UUID syntax is enforced for UUID fields. `hash32` = 32 binary bytes.
`frame` = empty/nonempty marker plus u64 when present/nonempty, constrained to
the supported nonnegative i64 range. It restores a string at the Qt boundary.
Enums are u8 IDs; floating values are finite IEEE-754 f64, not reduced precision.

| ID | Message | Payload beyond common slots | Order | Dispatch |
| ---: | --- | --- | :---: | :---: |
| 1 | session.contract | protocol u8; audio format/profile enums; sample rate u32; frame size u16; coordinator id16 | P | D |
| 2 | session.membership | page index/count u32; coordinator/editor/arrangement id16; editor/arrangement revisions i64; typed peer list | P | D |
| 3 | session.heartbeat | sequence u32 | I | - |
| 4 | session.heartbeat.ack | sequence u32 | I | - |
| 5 | session.udp.introduce | peer/nonce id16 | P | D |
| 6 | session.udp.recover | peer id16 | P | D |
| 7 | session.connection.failure | a/b id16; detail text | P | D |
| 8 | session.endpoint.update | endpoint text | P | D |
| 9 | session.end | detail text | P | D |
| 10 | session.error | message text | P | D |
| 16 | jam.sync.set | six Boolean policy slots; generated-ideas enum | P | D |
| 17 | jam.sync.request | same policy slots | P | D |
| 18 | jam.metronome.state.set | enabled Boolean | P | D |
| 19 | jam.metronome.state.request | enabled Boolean | P | D |
| 24 | jam.listening.request | L below | P | D |
| 25 | jam.listening.prepare | L below | P | D |
| 26 | jam.listening.ready | L below | I | - |
| 27 | jam.listening.failed | L below | I | - |
| 28 | jam.listening.commit | L below | P | D |
| 29 | jam.listening.cancel | L below | P | - |
| 30 | jam.listening.state | L below | P | D |
| 31 | jam.listening.reset | L below | P | D |
| 32 | jam.listening.applied | actual frame | I | - |
| 40 | beat.set | section u8; lane enum; beat u16; text | P | D |
| 41 | beat.hit | section u8; beat u16; lane u8; text | P | D |
| 42 | beat.division | section u8; beat u16; division u8 | P | D |
| 43 | music.division | section u8; beat u16; division u8 | P | D |
| 44 | music.step | section u8; beat u16; step u8; lane enum; text | P | D |
| 45 | grid.resize | section u8; lane enum; beats u16 | P | D |
| 46 | song.set | bounded song checkpoint/delta; scope enum; arrangement/base revisions i64; authoritative/playing Booleans | P | D |
| 47 | practice.references.render | request id16; signature hash32; five render Booleans; voicing/kit u32 | P | D |
| 56 | bank.request | bank u8; target beat as frame | P | D |
| 57 | bank.prepare | bank u8; switch id16; target beat as frame | P | D |
| 58 | bank.ready | bank u8; switch id16 | I | - |
| 59 | bank.cancel | bank u8; switch id16 | P | - |
| 60 | bank.switch | bank u8; switch id16; target beat as frame | P | D |
| 64 | looper.track.share.request | batch id16 | P | D |
| 65 | looper.track.batch.offer | batch id16; bounded tracks document | P | D |
| 66 | looper.track.batch.complete | batch id16; track count u32; applied Boolean; reason text | P | D |
| 67 | looper.recording.state | phase enum; state revision u32; bank u8; lane id/name text; countdown u16; bars u8; group/source id16 | I | - |
| 68 | looper.recording.group.start | group id16; typed participant list; bars u8; countdown/target beats as frame | P | D |
| 69 | looper.recording.group.finish | group id16 | P | D |
| 70 | looper.recording.group.recover.request | group id16 | P | D |
| 71 | looper.recording.group.recover | group id16 | P | D |
| 72 | looper.recording.resync.request | no additional fields | P | D |
| 73 | looper.recording.resync.state | playing Boolean; active bank u8 | P | D |
| 80 | looper.asset.request | typed hash list; arrangement revision i64 | P | D |
| 81 | looper.asset.start | hash32; file bytes u32; chunk size u16 | P | D |
| 82 | looper.asset.ack | bound transfer handle u32; cumulative written chunks u16 | P | - |
| 83 | looper.asset.done | bound transfer handle u32; chunks u16 | P | D |
| 84 | looper.asset.unavailable | hash32; reason text | P | D |
| 96 | AssetChunk (binary) | bound transfer handle u32; index u16; remaining bytes are WAV file data | P | - |
| 97 | Receipt (binary) | message ID u64; stage u8; rejection adds reason code u8 + text | matching domain | - |
| 98 | Binary (diagnostic) | bounded nonempty opaque bytes, <=65,536 | P | - |

There are 41 positive-dispatch-receipt schemas and seven independent-control
schemas among the 51. A diagnostic Binary record is an explicitly named test/
diagnostic operation, not an escape carrying unknown product messages as JSON.
Unknown operations/fields fail instead of silently bypassing typed coverage.

`L` has ten possible fields: fixed-layout settings, bank u8, fixed-layout tempo,
target/epoch frames, epoch position/source seconds f64, playing Boolean, reason
text, changed-groups u8. With the common fields it uses a u16 presence mask.
Settings = speed i32 + pitch i32 + focus flag/preset byte + three f64 values;
a non-`custom` preset uses a bounded text escape. Tempo = rate i32 + anchor u64
+ anchor position f64. The schema registry includes the legacy transaction
forms, but Server's active listening path uses authoritative state selection
and applied reports; do not infer that every slider edit exchanges all nine
listening opcodes. Preserve late-application/recording protection semantics.

Peer/participant/hash lists have a u8 count and an explicit maximum of 64 items;
memberships are paged at 64, not a 64-peer application-wide cap. Peer entries
send a binary token plus endpoint text and derive the existing numeric peer ID
locally. Participant entries send 16-byte tokens. Hash-list entries send 32-byte
hashes. The recording group currently has its own 64-participant validation
limit. Do not turn any per-message limit into a new global mesh cap during a
future shared-codec migration.

### Remaining document content and actual shared edit paths

| Area | Present encoding/ownership | Gap and proposed treatment |
| --- | --- | --- |
| Track name | `song.set`, typed RenameTrack patch proved against exact base/next song serials | Wire path already compact. A typed model intent can later avoid constructing/comparing a complete song; it is not another missing wire opcode. |
| Track region/placement/loop fields | GUI region owner changes `LooperLaneRegion`, calls arrangement synchronization, publishes `song.set`; generic bounded delta | Strong candidate for one atomic typed region patch/intent. Keep signed sentinels, content identity, revision and deletion/replacement checks. Do not independently merge fields into an invalid region. |
| Clear WAV, remove/reorder lanes/sections, arrangement steps | Current model edits publish snapshots; structural array changes can use replacement | Inventory compact structural patches next. Address stable content/lane identity, index shifts and deterministic reconciliation before adding commands. Initial state/recovery still needs checkpoints. |
| Song/section title, timing/meter/grid-lock changes | Song graph/delta, not a dedicated model command for every edit | Candidates after measuring bytes/packets and change frequency. Keep unrelated changes and applicable sync scopes intact. |
| Track contribution manifest | `ShareOffer.tracks` bounded numeric-key CBOR, optionally compressed | Strong typed nested-list candidate: recording UUID16, hash32, validated bank/lane target, UTF-8 name, sample rate, frame/loop/reference metadata with presence bits. Current UUID/hash values inside this document remain strings. |
| Snapshot lane hashes/IDs and bank timing | Numeric property keys and five canonical signed frame strings already packed; many remaining values remain document types | Consider typed known lane/timing structures or explicit bounded transforms. Four play/accent mask fields are decimal u64 strings and are not handled by `snapshotFrameKey()`. Preserve full unsigned precision. Do not force arbitrary lane/bank IDs into UUID16. |
| Generated recipes/extensions | Unknown keys retain bounded text-key/value representation | Keep an explicit bounded extension representation. A fixed schema for every arbitrary recipe would add complexity and reject legitimate data. |

Important correction to the earlier candidate list: **lane gain, mute and solo
are local mix choices, not shared edit traffic**. Their GUI actions regenerate
the local mix without publishing an arrangement, and `LooperProject::toJson(true)`
omits `gain_db`, `muted`, `solo`, local paths and other local-only properties.
Likewise peer listening levels call the local runtime, not a control send.
Do not add typed network messages for these controls under this efficiency work.
Their names appearing in the document key dictionary is not proof of wire usage.

Evidence: [sync-compatible model serialization](app/gui/LooperProject.cpp),
[GUI region, local mix, sharing and snapshot call sites](app/gui/MainWindow.cpp),
[concurrent edit reconciliation](app/gui/ConcurrentLooperMerge.cpp),
[track workspace state](app/gui/TrackWorkspaceController.hpp).

### Routing, fan-out and confirmation inventory

Audio uses the existing peer mesh. Application control and WAV are routed through
the creator in this experiment; changing serialization does not change topology.
An edit from one joiner normally goes joiner -> creator, then its authoritative
result goes creator -> the other participants, including the origin where needed
for reconciliation. A creator edit skips the first leg. Membership pages are
observer-specific because routes may differ; they are not blindly identical
broadcasts. A document delta/transfer handle is also bound to a particular stream
baseline, so encoded bytes cannot be blindly reused across recipients.

Policy/metronome requests need the authoritative set/state result. Editor actions
receive creator-assigned ordering/revisions. Arrangement proposals reconcile in
the existing owner and return authoritative snapshots. Bank/group/listening
actions retain their transaction/revision and next-valid-boundary rules. Snapshot
catch-up repairs state; it must not replay obsolete starts after recording or a
partition. Source authorization remains separate from codec field validity.

Four confirmation layers must remain distinguishable:

1. Multiplex ACK window: authenticated receipt/storage of reliable fragments.
2. Application Receipt: accepted/rejected dispatch at that channel boundary.
3. Domain reply/state: readiness, authoritative decision or actual application.
4. WAV completion: disk write progress, final hash/WAV validation and batch apply.

`looper.asset.ack` is cumulative progress **after the file worker writes a
chunk**, not a duplicate transport ACK. It maintains the four-chunk application
window. `asset.done` is sent by the sender once chunks are acknowledged; its
dispatch receipt does not prove the receiver finished final file verification.
The receiving transfer/batch owner performs that verification and publishes its
outcome. Removing any of these messages needs a replacement for its function.

Packet-count examples are isolated exchange arithmetic, not packet captures:

- A minimal heartbeat has five payload bytes plus a 32-byte application record.
  Its IPv4 data datagram costs 123 bytes. With a matching reply and two separate
  empty-window transport ACKs, the isolated exchange is four datagrams/418 bytes.
  It has no application dispatch receipts. Normal heartbeats are infrequent;
  do not optimize them as if they run at audio packet rate.
- A <=1024-byte authenticated record that needs a standalone successful receipt
  can entail four datagrams: Data, transport ACK, Receipt, transport ACK. The
  success receipt is 41 application bytes/127 IPv4 bytes. Coalescing ACKs and
  cumulative successes already reduces these counts in bursts.
- With four total peers, a joiner proposal plus three authoritative deliveries
  creates four delivery legs before confirmations. Separate receipt/ACK traffic
  can multiply the datagram count. This is why per-action/fan-out accounting
  matters more than reducing a scalar payload by one byte.
- A full WAV record is 24,614 application bytes -> 25 data fragments with a
  38-byte final payload. Packing consecutive already-queued records could
  amortize those small tails: at most about one data datagram per chunk,
  approximately 4% of data packets but only roughly 0.3% of total IPv4 data bytes
  in a long, suitably queued transfer, before ACK effects. The file worker and
  5 ms producer scheduling may mean the next chunk is not available in time.
  This opportunity needs actual tail-occupancy measurement; never wait for it.

Evidence: [creator routing/membership](app/application/SharedSessionController.cpp),
[dispatch/coalescing](app/application/ServerControlWire.cpp),
[file progress and validation](app/gui/AssetTransferService.cpp),
[fragment enqueue/poll](libs/jam2-core/src/reliable_udp_stream.cpp).

### Other wire/control traffic accounted for

| Surface | Current format | Coverage decision |
| --- | --- | --- |
| `hello.challenge`, `hello.proof`, `hello.ok`, `hello.error` | Bounded framed JSON; challenge/proof binds chosen Server wire version into authentication transcript | Genuine cold-format gap. A future typed handshake must preserve nonce freshness, direction/channel keys, proof binding, rejection bounds and admission limits. Low steady-state packet benefit; smaller fields do not remove the challenge/proof round trips. |
| Directory registration/admission/introduction/repair | JSON v1 datagrams to the Go server | Separate protocol, not an unknown application-message fallback. Binary conversion requires coordinated server/client work and is outside the current Jam2-only branch implementation. Cold/recovery traffic, plus normal registration keepalives. |
| Multiplex Open/OpenAck/Data/Ack/Close/CloseAck/PeerLeave/AckWindow | Existing fixed prefix/authenticated v2 envelope | Already typed. ACK batching and borrowed payload/worker reuse are implemented above. Keep Open/Close identity and retirement/replay rules. |
| Native UDP Hello/HelloAck/Ping/Pong/MetronomeState/Bye/TransportState | Existing typed JAM2 packet layouts, native protocol v2 | Seven non-audio kinds already accounted for. Leave in the existing optimized runtime, including clock/epoch/sample-time behavior. Do not move these onto an ordered application stream as part of codec coverage. |
| Native UDP Audio | Existing PCM media packet path | Explicitly excluded. No format/DSP/buffer/scheduler changes. |
| STUN discovery | Existing standard STUN messages | Discovery only; retain the standard protocol, outside application codec conversion. |
| Internal disconnect, local automation, logs, plugin/worker IPC, persistence | Internal actions or separate local formats | Excluded from network typed coverage. Do not create packets to encode local-only actions. |

Directory message names found in the client implementation are:
`jam_create`, `jam_create_challenge`, `jam_created`, `jam_pause`, `jam_paused`,
`jam_join`, `jam_recover`, `jam_member`, `jam_wait`, `jam_join_request`,
`jam_admit`, `jam_admit_ack`, `jam_remove`, `jam_remove_ack`, `jam_reject`,
`jam_reject_ack`, `jam_pair`, `jam_pair_ack`, `jam_prepare`, `jam_candidate`,
`jam_registered`, `jam_offer`, `jam_ready`, `jam_go`, `jam_outcome`,
`jam_connected`, `jam_failed`, `jam_error`, `jam_leave` (29 names).
This inventories Jam2's client-side surface, not every possible server-only
message. It does not change directory authentication or claim the server is typed.

### Proposed changes ranked for this goal

These are review recommendations, **not implemented or approved changes**.
No new byte/CPU percentage is claimed without an ablation and a real workload.

| Priority | Proposed work | Why it fits / acceptance condition |
| --- | --- | --- |
| 1 | Exhaustive per-operation native codec/validation fixtures and per-action traffic measurements | Make all 54 operations explicit and catch field drift before migration. Measure complete fan-out exchanges, not just payload lengths. |
| 1 | Typed nested track-offer manifest | Removes repeated text UUID/hash/value representation at sharing time, with one cohesive typed structure. Compare compressed existing documents, sparse/full manifests and 1024-byte boundary crossings. Keep stable arbitrary lane IDs and optional reference metadata. |
| 1 | Atomic typed region/placement edits; then explicit asset clear/structural edits where measured useful | Target real shared traffic that still goes through song documents. Prefer a small serial-bound patch first if it avoids a second reconciliation engine. Model intents can later remove full JSON graph construction. Require deletion/replacement and concurrent independent-edit convergence. |
| 1 experiment | Fill only an unsent reliable tail with already-queued bytes | Removes packets without a batching timer or more per-packet framing. Measure tail availability, data/ACK datagrams and action delay; preserve stream/domain separation and already-assigned sequence ownership. Packing can make records share a lost fragment or split a record across fragments; test those completion delays explicitly. |
| 2 | Typed known snapshot lane/timing structures, including full-width play/accent masks | Improves checkpoint density/type fidelity; compressed documents already save much of the repetitive data. Keep bounded extension fields and full precision. Rank by actual checkpoint traffic rather than synthetic rename count. |
| 2 | Co-packet already-ready dispatch results/domain responses | Review where immediate flushes prevent combining bytes already ready for the same ordering. Do not wait for a response, cross ordering domains or remove explicit rejection/application meaning. Tail packing may capture part of this gain already. |
| Later | Compact peer handles/endpoint representation, typed cold handshake | Valid size opportunities, mostly control metadata/startup. Preserve stable IDs, route changes, reconnect generations and explicitly coordinated compatibility. Names/handles remain separately unimplemented work. |
| Deferred | ACK piggybacking, header/HMAC redesign, encoded retry caching, MTU/window changes, socket readiness/deadline work | Not required for typed payload coverage and not silently included. Revisit only with measured traffic/CPU evidence and separate authorization. |

Do not add live gain/mute/solo messages, compress every tiny edit, delay sparse
actions to fill packets, strip semantic receipts indiscriminately, or remove
snapshots/recovery. Typed encoding alone does not prevent WAV upload from
saturating an uplink. Current uncapped WAV mode has no aggregate mesh egress
pacing guarantee: every creator copy still consumes that link's capacity.
Explicit aggregate upload pacing/fairness, if requested later, is separate
congestion work and must be measured with simultaneous audio. This audit does
not claim that a smaller format guarantees unaffected audio on a saturated link.

### Native-test coverage inventory and proposed acceptance

Existing native test ownership:

| Boundary | Existing CTest(s) / evidence | Remaining coverage needed |
| --- | --- | --- |
| Codec and channel | `jam2_server_wire_units`: authentication/replay/tamper; song delta/rename/base failure; packed enums/flags/frame types; asset binding/final short chunk; receipt/rejection/queue/independent safety | No exhaustive schema-driven fixture register for all 51 named messages plus three specials. Add valid/min/max/optional-field and malformed/source-policy cases per operation. Literal message-name presence is not an execution-coverage metric. |
| Encoded workload cost | `jam2_server_wire_performance`: rename, checkpoint/packing variants, WAV codec and edit bursts | Add region/structural edits, typed manifest variants and known timing fields; compare both bytes and fragmented datagrams against current production codec. |
| Reliable stream/mux | `jam2_udp_control_transport_units`, `jam2_udp_multiplex_units`, `jam2_udp_handler_performance` | Extend same owned production implementation for tail packing, gap/duplicate/retry ownership, ACK loss, would-block, queue bounds and sparse-send timing. |
| Four-peer shared state/files | `jam2_udp_four_shared_content_integration`, `jam2_udp_four_recovery_integration`, `jam2_udp_four_sharing_races_integration`, `jam2_udp_four_unthrottled_wav_integration` | Explicit per-action packet/byte/fan-out assertions for added typed paths; atomic regions; delete versus rename/region; replacement content; catch-up; cancelled/failed sharing; final file hashes. Reuse actual GUI/model owners. |
| Control alongside audio | `jam2_udp_four_receive_pressure_integration`, application startup/local tests, `jam2_four_mode_audio_latency_integration` | Simultaneous realistic editing plus WAV fan-out and deterministic loss/reordering. Compare receive/classification queue delays and drops, native loss/jitter, callback/buffer/xrun data and control completion distributions. Keep the existing audio implementation. |
| Listening/late execution | `jam2_udp_four_listening_tools_integration` | Preserve recording protection, late boundaries and applied reports. Resolve the previously recorded intermittent epoch/grid failure before broad mode migration; passing reruns did not establish its cause. |

The test source has direct named fixtures for 13 of 51 schemas in
`ServerControlWireTests.cpp`, plus special-operation/channel tests. That count
is a source-inventory observation, **not 25% test coverage**: GUI integrations
exercise additional messages and source name searches miss generated forms.
Conversely, existing broad integration passes do not prove every optional field
and invalid enum/length/source case has a native assertion.

For each candidate, record action count, recipients, emitted/superseded records,
UDP Data/ACK datagrams, payload/framing/IPv4 bytes, retries, fill ratio/tail bytes,
queue depths/waits, dispatch/application completion and CPU/service totals.
Measure two-peer and exactly-four-peer workflows; normal sparse edits and bursts;
initial join/checkpoint/reconnect; recording and WAV fan-out. Packet counts must
cover all recipients and both directions. Current wire-byte counters are not a
complete packet capture: they omit some outer headers/retries and cannot by
themselves establish whole-jam congestion savings. New instrumentation should be
bounded technical counters outside real-time callbacks, with no coverage build.

No CTests were rerun for this documentation-only audit. Prior test results above
remain historical evidence; no implementation improvement is validated here.

### Conditions for eventually sharing the interface across modes

Keep operation/domain types and codec ownership separate from delivery framing.
The existing `ControlConnection` and codec boundary are useful seams; avoid a
second authority or reconciliation implementation. Replace generic model intents
in the relevant model/controller/service owner, not by growing `MainWindow`.

Today codec selection and some validation shortcuts are coupled to
`ReliableUdp`/`multiplex_`. Future TCP adoption must select a negotiated codec
capability explicitly, retain validation exactly once at its real boundary, and
keep mode-independent source/authorization checks. TCP can carry length-delimited
typed records using its existing reliable byte stream. Independent ordering is
currently a multiplex facility; a single TCP stream retains its head-of-line
behavior unless separately redesigned. Shared payload encoding does not make
transport behavior identical.

Version/capability negotiation, key/replay separation, existing Direct/STUN
discovery/route handling, asset-channel ownership and current snapshots/WAV
semantics need explicit compatibility tests before migration. Preserve the seven
native non-audio runtime packet kinds alongside the unchanged Audio path. This
inventory is the basis for selecting changes on `UDP-Multiplex`, not authorization
to enable the new codec in other modes or change their connection topology.

## Typed coverage and packet-packing implementation and results

The user approved inventory changes 1-4. Scope remains Server control/WAV on
`UDP-Multiplex`. The pre-change Release codec/WAV CTests passed (17.44 s total).
The 4,194,348-byte WAV reached three verified recipients in 5628 ms, aggregate
2,235,793 B/s. Baseline codec: 1000 typed renames used 79,362 authenticated bytes;
the 1-MiB WAV codec workload used 3425 overhead bytes. Logs are retained under
`artifacts/typed-coverage-optimizations/`.

Implementation choices under measurement:

- Application wire version **3**, still inside outer multiplex version **2**.
  Handshake proofs bind the application version; independent-control key-domain
  derivation follows that version. Updated experimental peers are required.
  TCP and the native JAM2 protocol are unchanged. New goldens retain the exact
  authenticated layout, with version/tag bytes updated intentionally.
- Native fixtures enumerate all 51 schemas and three special operations. They
  exercise exact round trips, every truncation offset, trailing bytes, unknown
  fields, wrong revision types, opcode/authentication/replay, owning validation
  and source policy. Asset progress belongs to the asset owner rather than the
  normal control-message validator; existing native asset/integration checks
  retain that boundary. A missing authoritative revision in the first new policy
  fixture was corrected; product validation was not relaxed.
- Track manifests use a compiled 16-field entry schema, a u16 entry count bounded
  at 1536, u16 presence masks and one Boolean bitmap byte. Known UUID/hash values
  become binary, frame strings become signed i64, and arbitrary lane IDs remain
  bounded UTF-8. Optional empty reference signatures remain distinct. The current
  bounded document representation is retained for extension/noncanonical values.
  Both candidates use the existing compression policy; the encoder selects typed
  bytes only if smaller after compression. This compares two representations at
  sharing time and can increase CPU; benchmark results decide whether it is useful.
  The document field's flags are bit 0 compressed, bit 1 typed manifest. The typed
  bit is legal only for `ShareOffer.tracks`; other/reserved bits fail.
- A serial-bound atomic region patch (kind 5) carries bank/lane u16, one loop
  Boolean byte and four signed i64 frame values. The encoder proves the complete
  reconstructed song is identical, so unrelated edits/content replacement fall
  back to the existing delta. Decoder checks region bounds before advancing its
  baseline. This is serialization, not a second conflict-reconciliation engine.
- A serial-bound single-bank lane splice (kind 6) carries bank/prefix/remove u16
  plus a bounded inserted-lane document array. It preserves unchanged prefixes/
  suffixes; complete-song reconstruction is also proved. It targets insert/delete
  without replacing every lane. Section changes, reorders and combined unrelated
  changes retain generic encoding. Full checkpoints remain available for recovery.
- Reliable enqueue may fill only the unsequenced queue tail from already-ready
  bytes. Sequenced pending/retry frames are immutable. No batching timer, window,
  pacing or packet-size change is added. Fixed fragment capacity is reserved;
  native options/config support an unpacked comparison. Stats expose appended
  tail bytes and fragments avoided. Cross-record loss dependencies and sparse
  immediate sends are tested; no claim of unchanged completion distributions is
  made merely from byte conservation.

No change to local gain/mute/solo semantics, WAV star topology, audio
codec/DSP/callback/runtime, other connection modes, retry caching or HMAC/header
consolidation is included.

### Retention decision and measured value

**Keep the fixtures, conditional typed manifests, atomic region/lane-splice
patches, and unsent-fragment packing.** These are incremental refinements of the
existing codec/reliable stream, not new network actions or a replacement authority
model. They do not finish migration of the model API from QJsonObject to C++
domain command types, and they do not enable typed TCP control yet.

Seven rotated samples per reference/typed variant used the same production codec
and exact decode/convergence checks. Manifest runs used 20 offers; edit runs used
200 snapshots including their initial checkpoint. The table reports application
record bytes, including authentication/framing. Data fragment counts assume
separate writes and 1024-byte fragmentation before tail packing; modeled IPv4
data bytes add 86 per datagram. These codec counts exclude ACKs/receipts, actual
fan-out, loss and socket scheduling.

| Workload | Reference / typed application bytes | Reference / typed data fragments | Reference / typed median us |
| --- | ---: | ---: | ---: |
| 1-track offers | 4420 / 3000 | 20 / 20 | 613.1 / 771.0 |
| 4-track offers | 8220 / 5760 | 20 / 20 | 1585.4 / 1788.3 |
| 32-track offers | 43680 / 31060 | 60 / 40 | 11436.2 / 12616.9 |
| 128-track offers | 170640 / 113420 | 180 / 120 | 45631.6 / 48185.3 |
| Region edits, 64 lanes | 34900 / 23488 | 203 / 203 | 8805.5 / 8125.5 |
| Lane insertion/removal, 64 lanes | 628218 / 29202 | 800 / 203 | 367047 / 9321.2 |

The manifest fixtures save 29-34% application bytes, and larger offers cross fewer
fragment boundaries (one-third fewer data fragments in these two cases). Small
offers remain one data packet each: they save bytes, not packets. Selecting the
smaller compressed representation costs CPU, around 7.9 us extra per single-track
offer here, and about 128 us extra per 128-track offer. The encoder always retains
the document candidate when it is smaller/equal or the typed schema cannot
losslessly represent the entry. Real one-track offers can therefore remain
documents. The fallback counter combines these size and representation reasons;
it is not a count solely of unknown extensions. File-transfer phase frequency and
byte/packet savings justify keeping this conditional path; no CPU win is claimed.

Region patches save 32.7% application bytes and about 7.7% processing time in this
fixture, but not data packets because both representations fit a fragment.
Lane splice saves 95.4% application bytes and reduces data fragments 800 -> 203
(74.6%). The large processing gain comes from avoiding repeated replacement
encoding of unchanged lanes; it is specific to the insertion/removal workload,
not a claim for arbitrary edits. Same-size reorders, section structure changes,
asset-clear commands and model-level typed intents remain generic/current paths.

The packing native benchmark uses authenticated production UdpStreamConnection,
seven rotated samples, exact application byte verification and controlled
loss/duplicates/reordering. It never waits to accumulate more writes:

| Already-queued write workload | Unpacked / packed Data | Unpacked / packed ACK | Modeled IPv4 bytes |
| --- | ---: | ---: | ---: |
| 2 MiB, 24614-byte writes, ordered | 2130 / 2095 | 92 / 91 | 2288244 / 2285148 |
| Same, impaired | 2262 / 2175 | 754 / 720 | 2492478 / 2481610 |
| 64 KiB, 32-write bursts of 73 bytes, ordered | 898 / 85 | 43 / 29 | 146462 / 75340 |
| Same, impaired | 955 / 87 | 307 / 32 | 180509 / 78594 |

Aligned 64-KiB writes showed no packing opportunity and no packet saving. Their
small timing differences do not establish a CPU benefit. Ordered WAV-sized writes
saved only 35 data packets/0.14% modeled total bytes in this harness, and processing
time was approximately unchanged (27509/27720 us). The dense small-write case
saved 90.5% Data packets and 48.6% modeled total bytes, with 3565/1159 us processing.
It intentionally queues 32 writes per turn: existing application batching already
reduces some real scalar bursts, so do not extrapolate this percentage to normal
editing. Under impairment, feedback timing changes later sends/loss opportunities;
do not interpret its retransmission reduction as a universal loss-rate benefit.

The exact-four-peer socket pressure A/B used the same required content/heartbeat
counts with native-shaped traffic and malformed-packet classification. Packed /
unpacked creator control ingress was **397 / 1188 packets**, with 814 avoided
fragments and 51,590 tail bytes packed across the four transports; unpacked
counters were zero. Both had zero ingress drops and exact application bytes.
Creator worker service totals were 28,253/34,398 us over 286/329 turns. Sampled
classification means were 1372/346 ns and maxima 25800/1300 ns: the packed run
does not establish faster per-packet classification. Native packet counts and
worker schedules differ, so neither service totals nor sampled timings are an
isolated whole-app CPU percentage comparison.

Real four-peer 4,194,348-byte WAV runs took **5740, 5730 and 5683 ms** versus the
5628-ms pre-change baseline. These are approximately 1-2% slower localhost samples,
not a claimed WAV throughput improvement or proof of exact performance equality.
All three recipients verified the files and had zero ingress drops. The last run
reported 49 avoided creator fragments/22,455 tail bytes packed. Current file
producer/window scheduling leaves most tails without another ready chunk, so the
real WAV packing gain is small. Keep the path for queued control bursts and
available tails; do not increase window/pacing or delay sparse writes to chase
the theoretical WAV packing maximum.

### Validation, failed assertions and remaining limits

The normal elevated MSVC Release compile succeeded, using `release/jam2.exe`:
SHA-256 `7BF55A24F6F95C1C670017180B3EF753610554612D5B86EB4FEB75EA8C38628D`.
All **14 directly relevant CTest cases have passing latest runs**:

- `jam2_server_wire_units`, `jam2_server_wire_performance`;
- `jam2_udp_control_transport_units`, `jam2_udp_multiplex_units`,
  `jam2_udp_handler_performance`;
- `jam2_looper_project_units` (existing atomic-region, deletion/rename/replacement
  reconciliation cases);
- `jam2_udp_four_receive_pressure_integration`,
  `jam2_udp_four_unpacked_fragments_performance`;
- `jam2_udp_four_shared_content_integration`, `jam2_udp_four_recovery_integration`,
  `jam2_udp_four_sharing_races_integration`,
  `jam2_udp_four_unthrottled_wav_integration`;
- `jam2_udp_four_listening_tools_integration`,
  `jam2_four_mode_audio_latency_integration`.

The first eight scoped cases passed in 52.83 s. The subsequent ten-case group
passed nine in 228.43 s: the new shared-content assertion required the first
originating GUI region proposal to use the fast path, although all four peers had
already converged to the correct atomic values. Its origin had no typed-region
count. An outgoing song baseline is independent of received authoritative
snapshots; an edit can carry accumulated changes and legitimately use generic
encoding. The exact field causing that initial fallback was not separately
instrumented. The corrected scenario checks two consecutive real painted region
edits against full snapshots at all four peers, then requires the origin's typed
counter after the established-baseline edit. No product rule, assertion on model
values, or timing budget was weakened. This real workflow passed in 76.90 s,
including later sharing/saving/reopening/recording stages. The final region was
start/stop/source-start/source-end = 128/368/16/256 with the same WAV hash.
The matching WAV rerun passed in 15.77 s; the two-case rerun took 92.67 s.

The codec fixture register also tests optional common fields, each represented
field's absence/type, bounded extensions, 1536-entry manifests, malformed typed
flags/counts, invalid region flags, unbound patches and simultaneous unrelated
changes. Previous application-version records with recomputed valid HMACs are
rejected. File/memory ownership, retry immutability, would-block, cross-record
fragment gaps/reordering and immediate sparse transmission are covered. Exhaustive
operation coverage is not exhaustive enumeration of every possible value or
network schedule. Full snapshots and owning validators remain required.

The audio comparison passed in 61.93 s: Direct/Server medians **6.6/6.7 ms** and
p95 **8.9/9.0 ms**, differences +0.1/+0.1 ms within existing 1.333/2.667-ms budgets.
ABBA order used two independently started jams per mode and 192 correlated
displayed path observations per mode. It remains a fake-audio queue-estimate
comparison, not an acoustic/hardware latency measurement. No fixed added audio
delay is established by this run, and no audio implementation was changed.
Listening passed in 20.08 s. The older intermittent grid assertion recorded above
remains historically unexplained; this run did not reproduce it.

Logs and preserved failed-assertion/audio artifacts are under
`artifacts/typed-coverage-optimizations/`. Some existing early-exit test paths print
"four-peer test failed; retained per-peer artifacts" despite success/exit 0 because
their cleanup success marker is unset; CTest status and explicit assertions are
the validation result. That pre-existing diagnostic quirk was not modified here.
No full test suite or source coverage collection was run. These changes do not
provide uplink saturation protection; aggregate upload pacing and the other
deferred transport/header changes remain separate work.

## Compact transport and evidence-based recovery: implementation and results

Scope remains Jam2 Server on UDP-Multiplex. Direct/STUN TCP, live audio encoding,
audio callbacks, WAV pacing and the 1024-byte reliable fragment ceiling remain
unchanged. The scoped Release validation and measurements are recorded below.

- Mux version 3 establishes a 16-bit connection handle in authenticated full
  Open/OpenAck frames. Established data/ACK packets use a 16-byte fixed prefix plus the
  existing 16-byte HMAC tag, saving 26 bytes against the 58-byte full envelope.
  Sequence numbers remain 64-bit. Per-direction cached keys bind the handle to
  the full session, introduction and stream generation. Handles are not reused
  within a link; exhaustion falls back to full framing. The application HMAC is
  retained. This experimental version requires matching builds.
  Open/close/leave and standalone native intent receipts retain full framing.
- Application version 4 defines stable peer identities inline once per ordered
  codec context, then uses a marker and uint16 reference (3 versus 17 bytes).
  The bounded table holds 4096 identities, then uses full identities. This is a
  dictionary bound, not a jam peer limit. Primary and independent ordering
  domains have separate dictionaries. Failed records do not commit mappings.
  Supersedable scalar assignments may reference an established handle, but a
  previously unseen identity uses a non-defining literal. Otherwise coalescing
  could remove its first definition and leave surviving records with unknown
  or differently numbered handles. A regression covers interleaved cells and
  later ordered definitions. Existing mappings remain usable by scalar edits.
- Ready reverse data may carry a 9/17-byte ACK prefix when it fits within the
  existing fragment ceiling. Primary/independent ACKs can share one 24-byte
  ACK-pair payload. No batching delay is introduced; sparse ACKs flush in the
  same worker turn. Separate asset connections are not combined across streams.
- Every reliable control/WAV fragment uses the same recovery policy. Three
  newly acknowledged higher sequences provide gap evidence; after a configurable
  10000-us reordering grace, one early retransmission is permitted per fragment.
  Duplicate ACKs do not add evidence. Ordinary RTO recovery remains active for
  every fragment, including isolated traffic and a lost fast retransmission.
  This is not a sparse/busy mode split. Existing lifetime and congestion-window
  reduction rules remain. Reordering can still cause an unnecessary early retry.
- Native transport intent receipts echo the accepted 28-byte intent through the
  authenticated mux. They stop identical per-peer native repeats only after the
  runtime accepts/schedules or deliberately suppresses the action under its
  existing recording policy. Grid/clock readiness deferrals are not acknowledged.
  A changed intent starts delivery again; no peer gates the other peers. Native
  packet layout remains unchanged. Runtime reattachment can restart event
  counters, so receipts are conservatively disabled on an already-used physical
  link until a fresh link epoch, retaining normal repeats there. This avoids
  confusing a delayed receipt with a new intent having restarted counters.

New counters expose compact savings, ACK pairs/piggybacks, selective gap reports,
fast retransmissions and native intent receipts/suppressed repeats. These are
application datagram byte counts; IP/UDP and link-layer overhead are additional.

### Initial component measurements and retention decision

Seven interleaved Release samples per component variant use the same production
stream/parser, fixed payloads, 1-ms simulated poll ticks and deterministic
loss/duplication/reordering. Simulated completion time is distinct from measured
host execution time; neither is a prediction of Internet transfer time.

| Workload / reference | Selected result | Meaning |
| --- | --- | --- |
| Ordered 2-MiB bulk, 2280074 modeled IPv4 bytes | 2224772 with compact framing | 55302 bytes saved, 2.43%; 2127 datagrams unchanged |
| Ordered 64-KiB queued small writes, 75340 bytes | 72376 compact | 3.93% smaller; existing tail packing retained |
| Impaired 2-MiB bulk, 5.464001-s simulated completion | 1.777001 s selective repair | 67.5% shorter; retries 127 to 124; not a loss-free latency claim |
| Impaired 64-KiB burst, 122001-us simulated completion | 39001 us selective repair | 68.0% shorter; four retries in both variants |
| 1000 simultaneous bidirectional control/readiness exchanges, 8000 packets / 1072000 bytes | ACK combining: 5000 / 856000 | 37.5% fewer packets, 20.1% fewer bytes |
| Same exchanges | ACK combining + compact: 5000 / 726000 | 32.3% fewer bytes overall; 1000 pairs and 2000 piggybacks |
| 200 repeated two-peer identity records, 16000 application bytes | 10428 with peer references | 34.8% smaller; unchanged 200 unpacked fragments |

The duplex workload intentionally makes both ordering domains/reverse data ready
together; it establishes the opportunity, not its frequency in normal use.
The real four-peer WAV/heartbeat pressure run used zero ACK pairs/piggybacks.
The one-way asset benchmark likewise shows no ACK-combining gain. Keep the
opportunistic code because it removes packets when work is already ready without
holding sparse traffic. Do not add a collection timer to inflate this result.

Measured duplex host medians were 31.94 ms reference, 24.41 ms ACK combining,
23.61 ms all transport, for 1000 exchanges. Individual microbenchmark CPU
differences are scheduler/machine dependent. The peer-reference encode/decode
median was 782.6 versus 729.3 us per 200 records. Primary gains claimed here are
bytes, datagrams and deterministic loss recovery, not general CPU acceleration.

The initial exact-four-peer pressure A/B used 36738 fewer envelope bytes in 1413
compact packets; total control/asset datagram bytes were 64391/862892 optimized
versus 74365/884922 reference. These are separate live socket runs with different
worker schedules, not identical packet traces. The native receipt scenario
suppressed 60/60 requested repeats after acceptance on all three edges, sent
changed intents immediately, and rejected all three intentionally stale receipts.
The reference sent all 60 repeats. One full IntentAck costs 86 application bytes
(114 including IPv4/UDP), compared with a 64-byte native intent (92 including
IPv4/UDP): a receipt pays for itself after avoiding two repeats. Its benefit is
therefore conditional on repeats remaining; a one-shot intent adds a receipt.

Retain all four optimizations, including peer references. The fast-reorder
configuration range is 0..2000000 us (default 10000); zero explicitly removes the
grace. Ordinary RTO taking precedence disables subsequent evidence-based retry
for that fragment, preserving backoff. No change to ACK authentication, both
MAC layers, MTU/fragment ceiling, WAV pacing or audio data was needed.

### Mux-v3 / peer-reference-v4 field details

Mux integers below are big endian. The native transport payload remains its
existing little-endian layout and is echoed byte-for-byte, without translation.

| Compact offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `J2MX` magic |
| 4 | 1 | mux version 3 |
| 5 | 1 | bit7 compact; bit6 zero; bits5:4 channel (1 control, 2 asset, 3 independent); bits3:0 kind |
| 6 | 2 | nonzero connection handle |
| 8 | 8 | data sequence or ACK cumulative base |
| 16 | variable, <=1024 | payload, inferred from UDP datagram length |
| end-16 | 16 | truncated HMAC-SHA256 over prefix and payload |

Data/ACK kinds supported in compact framing are 3 Data, 4 Ack, 8 AckWindow,
9 AckPair and 10 DataAck. Invalid reserved bits, unknown kinds/channels, zero
handles, wrong context keys and truncated/oversized payloads are rejected.
The key derivation is HMAC-SHA256 with the existing 16-byte jam key over
`jam2-mux-compact-v3` followed by session/introduction/stream (three u64 values)
and one direction byte (1 initiator, 2 acceptor). The full 32-byte derived key is
cached. Direction and fresh stream generation prevent reflection/retired-stream
reuse even if a compact handle were presented on a different connection.

Open/OpenAck retain the full 42-byte prefix plus 16-byte tag, with an optional
2-byte proposed/confirmed handle payload. An empty OpenAck declines compact
framing and the sender continues using full envelopes. Close/CloseAck/PeerLeave
also stay full. Thus setup adds four payload bytes per negotiated stream;
subsequent compact datagrams recover that cost immediately. Handle exhaustion
uses full framing, without wrapping or reassigning a live handle. No socket or
session peer limit follows from this field.

AckWindow uses the sequence slot as contiguous base; an optional 8-byte bitmap
marks base+1 through base+64. AckPair puts the primary base in that same slot;
payload is primary bitmap/u64, independent base/u64, independent bitmap/u64.
Both windows are validated before either changes sender state. DataAck keeps
the data sequence in the slot and prefixes data with flags/u8 (0 or 1), reverse
ACK base/u64 and, if bit0 is set, bitmap/u64. Prefix lengths are 9/17 bytes;
the complete payload must fit 1024 bytes. A full fragment therefore retains a
separate ACK. Retries retain original application bytes/sequence; an ACK prefix
can reflect current receive state and is not cached in the fragment.

IntentAck kind 11 uses full Control framing with reserved stream 1 and sequence
zero, plus the complete 28-byte native payload. Only an exact match to the last
sent intent in the active physical epoch marks it acknowledged. It has no
reliable-stream sequence or ACK-of-ACK; if lost, the next normal intent repeat
causes another receipt. No extra timeout or sparse-traffic policy is introduced.
Native intent repeats retain their existing 20-ms schedule and expiry rules.
This is acceptance/scheduling evidence, not proof of audible execution or a
quorum commit. Existing bounded engine-command submission can still fail; no
receipt is issued on such a failure. Existing authority event de-duplication
means this does not add a new engine-command retry/rollback mechanism.

Peer identity markers are 0 empty, 1 full 16-byte identity (learn next handle
if unseen and below 4096), 2 uint16 handle, 3 full non-defining literal. Handles
start at 1 in each direction/ordering domain and reset with its codec. Duplicate
full definitions do not allocate a second handle. Dictionaries commit only
after complete encode/decode success. An unknown/zero reference fails decoding.
UUID transaction identifiers, nonces, SHA-256 hashes and native audio peer IDs
do not use this dictionary. A peer-list entry initially costs 17 bytes rather
than its previous raw 16; each subsequent reference saves 13 bytes for that
list entry (14 where the previous optional identity cost 17). Names/display
labels and cross-mode typed-control migration remain separate work.

The initial choice is fixed u16 for both connection and application peer
references. Connection handles identify successive logical stream generations,
so they are not limited by the number of simultaneous peers. Application peer
handles use the same simple fixed-width representation with a 4096-entry memory
bound and literal overflow, preserving experimental large-mesh support. A u8
peer reference would save one additional byte per reference and could still use
literal overflow; it is a possible later refinement if measurements justify a
different dictionary layout. No decimal digit encoding or string peer token is
transmitted for a compact reference.

### Test audit during this stage

The first ten-case scoped group passed nine cases in 231.21 s. Rotation/loss
failed a combined stale-receipt assertion that required three probes to arrive
after one injection despite mux loss. That initial assertion did not report
separate accepted/rejected counters, so its exact subcondition cannot be
reconstructed. The test now asserts zero false acceptance separately and retries
stale probes until three invalid receipts have actually arrived.

A traced rotation rerun then found a second fixture error: it treated three
received receipt packets as three distinct acknowledged peers. The trace showed
three IntentAck emissions: packet 33325 delivered, 33326 dropped (divisible by
19), 33327 delivered and duplicated (divisible by 23). Thus a peer could remain
unacknowledged although the aggregate packet counter reached three. Native
repeats correctly continued to that peer, violating the fixture's premature
expectation of universal suppression. The fixture now checks explicit sent /
acknowledged state on every link; these flags and receipt-epoch eligibility are
also exposed in JSON diagnostics. Packet counters remain packet counters,
including duplicates. Final rerun results follow below.

The corrected rotation workflow passed in 20.77 s. Byte counters include full
IntentAck reception (including authenticated but stale receipts). A suppressed
native send reports logical success to the runtime so its existing repeat
schedule can advance; the multiplex native packet counter counts actual socket
sends, and the separate suppression counter accounts for avoided sends. Older
per-peer runtime send counters may therefore describe logical send attempts.

Successful codec decoding commits dictionary changes even if the model then
rejects the action and sends a rejection result. This preserves dictionary
agreement with the sender; a model rejection does not remove an ordered record.
Malformed/failed codec decoding commits nothing. Component reference variants
are ablations in the current binary, not a historic executable. The application
codec benchmark still models full 58-byte mux envelopes to isolate application
size changes; compact-envelope/IP costs are measured by the handler benchmark.

### Final audio comparison

The final public Release executable is `release/jam2.exe`, SHA-256
`D1DF503AD770DC4B17350461724E4E0BB9CDFA975E16ADCD7A5F95BDF7CE0F7C`.
The serial Direct/Server ABBA comparison passed in 62.19 s. It starts two
independent four-peer jams per mode, observes all twelve directed paths, and
collects 192 correlated displayed path observations per mode.

| Metric | Direct | Server | Server minus Direct |
| --- | --- | --- | --- |
| Displayed total median | 6.4 ms | 6.4 ms | 0.0 ms |
| Displayed total p95 | 8.3 ms | 7.9 ms | -0.4 ms |
| Mixer component median | 0.270833 ms | 0.062500 ms | -0.208333 ms |
| Playback queue median | 3.14583 ms | 3.43750 ms | +0.29167 ms |
| Output component median | 2.66667 ms | 2.66667 ms | 0.0 ms |

Medians of individual components need not add to the median total. Network and
jitter medians were both zero in this loopback run. Aggregate process CPU was
152.627/160.147 percent of one core (Direct/Server); this single comparison does
not isolate the optimization's CPU cost. Existing absolute median/p95 delta
budgets remain 1.33333/2.66667 ms plus 0.1-ms display resolution. Passing those
budgets is not proof of sub-millisecond equivalence on hardware. This run shows
no added Server queue-estimate latency; no live-audio encoding, callback,
buffer, socket wait, or DSP implementation was changed in this stage.

Raw audio observations and engine/transport snapshots are preserved under
`artifacts/transport-final-efficiencies/final-audio-*.jsonl`. Component A/B
results reproduced their packet/byte/completion counts in the final run;
duplex medians were 33.58 ms reference, 25.29 ms ACK combining and 23.59 ms all
transport. Peer-reference medians were 778.9/740.0 us per 200 records. These
measure host execution, not a network timing guarantee.

### Final scoped validation and remaining measurement limits

The normal elevated MSVC Release build succeeded. All **14 directly relevant
CTest cases passed on the final public executable in one serial 308.91-s run**:

- `jam2_server_wire_units`, `jam2_server_wire_performance`;
- `jam2_udp_control_transport_units`, `jam2_udp_multiplex_units`,
  `jam2_udp_handler_performance`;
- `jam2_four_mode_audio_latency_integration`;
- `jam2_udp_four_listening_tools_integration`,
  `jam2_udp_four_shared_content_integration`,
  `jam2_udp_four_recovery_integration`,
  `jam2_udp_four_sharing_races_integration`,
  `jam2_udp_four_unthrottled_wav_integration`;
- `jam2_udp_four_receive_pressure_integration`,
  `jam2_udp_four_wire_reference_performance`,
  `jam2_udp_four_peer_rotation_integration`.

Coverage includes distinct later-fragment evidence, duplicate SACKs, reorder
grace, lost early repair using the ordinary timer, an isolated fragment using
that same timer, would-block ownership, long grace versus RTO backoff, atomic
ACK-pair validation, sparse immediate ACKs, full-framing negotiation fallback,
compact context/direction/handle/tamper/truncation rejection, dictionary rollback,
4096-entry exhaustion with literal overflow, and scalar coalescing without lost
definitions. Four-peer tests include all three native intent receipt edges
(including leaf-to-leaf), stale receipts, loss/duplicates, changed intents,
per-peer suppression, rejected duplicate reader attachment, safe receipt
disablement after runtime restart, actual GUI playback acceptance, recording
and sharing workflows, logical-stream recovery and retained physical sockets.

The final real pressure ablation recorded control/asset bytes sent of
63529/862828 optimized versus 76569/885038 reference. It used 1376 compact
packets, saving 35776 envelope bytes, with zero pair/piggyback opportunities in
that particular run. The GUI workflow observed four receipt packets and 93
suppressed repeats; its assertions separately required all three peer flags.
The impaired rotation case observed zero accepted stale probes, seven accepted
receipt packets total, five invalid receipts and 60 suppressed repeats. These
are packet counters, so duplicates can increase received counts beyond peers.

The 4-MiB WAV scenario sent 4194348 bytes to each of three recipients. Its final
transfer measurement was **6455 ms / 1949348 aggregate raw bytes/s**, compared
with **5675 ms / 2217276 bytes/s** in the pre-stage baseline (13.7% longer elapsed,
12.1% lower throughput). Each is one live workflow run, with independently
scheduled native audio/control traffic and candidate impairment. They are not
identical packet traces or repeated throughput samples. The test passed its
existing workflow assertions; this data does **not** establish a WAV throughput
gain, nor isolate a cause for the difference. Retention is justified by exact
byte/packet savings, controlled component ablations and recovery behavior, not
by that single WAV comparison. WAV pacing and fragment size were not changed.
A matched repeated end-to-end throughput experiment remains necessary before
claiming real-network transfer speed improvements.

No full test suite, source coverage, hardware/acoustic audio test, Internet MTU
probe or new WAN congestion simulation was run. No uplink-saturation protection
is added. Rare engine-command submission failure and the runtime-restart receipt
limitation described above remain. Compact framing and dictionary changes require
matching experimental Server builds (application v4, mux v3); native audio stays
v2 and the legacy TCP format is unchanged. All work remains on UDP-Multiplex.

Final logs and retained raw measurements are in
`artifacts/transport-final-efficiencies/final-validation.log`, with baseline,
initial ablations, failed-assertion logs, the confirming route trace and audio
JSONL alongside it. `git diff --check` passed. No commit or merge was performed.

## Authority beacon on/off experiment and accelerated cadence results

The first stage tests the existing packet/cadence before changing either. Scope
is Server UDP on this branch; no audio encoding, audio callback, resampler or
other mode's behavior is changed. Tests compare normal 20-ms delivery, one
initial mapping followed by no further delivery, and no beacons from startup.
The latter two distinguish clock maintenance from the initial epoch mapping.

The existing nearest-bar phase calculation is exposed as a small, allocation-free
core helper used by the Server receive branch. Direct/STUN retain their existing
inline calculation. The accelerated test uses that production helper, authority
state machine, initial clock mapping, raw/musical frame conversion and actual
runtime compensation function/defaults (250-ms bound, 750-ms smoothing, 1-ms
deadband, 40-ms/s slew). It injects four clock counters and advances virtual time;
it does not run faster audio callbacks or create a second DSP implementation.

Simulated scenarios include equal clocks, +50/-50/+100-ppm peers against a
zero-ppm authority, delay jitter, asymmetric delay, 128-frame counter snapshots,
loss/ten-second outage and a clock-rate sign change. Each represents one virtual
hour. Ground-truth phase error is calculated from the injected continuous clocks,
separately from the runtime's RTT-derived observation. Fixed-rate clock models
cannot establish actual device oscillator behavior or real audible alignment.

The four-process Server test filters only native AuthorityState packets in the
existing test UDP translator. Assignments/proposals, transport intents and audio
continue normally. This models complete beacon loss and recovery without a new
product switch. The sender still emits packets toward that translator, so this
integration cannot measure CPU/uplink savings from actually disabling sends.
Packet compaction and a longer production interval remain pending the results.

### Accelerated results and interval sweep

The first on/off experiment passed. An additional test-only cadence sweep runs
the same unchanged phase/correction code at 20, 100, 250 and 1000 ms, followed by
bootstrap-only and fully disabled delivery. Seven deterministic scenarios times
six delivery policies simulate 42 one-hour four-clock runs in 0.93 s of Release
execution. This is virtual timestamp/state-machine advancement, not accelerated
audio rendering or a multi-device oscillator measurement. No production sender
interval or packet layout was changed. Phase samples are taken each virtual
second; maxima are maxima of those samples, not every simulated instant.

For a clean 50-ppm peer relative to the authority:

| Delivery | Error at 10 minutes | Error at 1 hour | Delivered beacons/hour/recipient |
| --- | --- | --- | --- |
| Every 20 ms | 1.02083 ms | 1.02083 ms | 180000 |
| Every 100 ms | 1.02083 ms | 1.02083 ms | 36000 |
| Every 250 ms | 1.02083 ms | 1.02083 ms | 14400 |
| Every 1000 ms | 1.06250 ms | 1.06250 ms | 3600 |
| Initial mapping, then off | 30 ms | 180 ms | 1 |
| Off from startup | No mapping | No mapping | 0 |

The approximately 1-ms residual follows the existing deadband/rounded correction
steps; the test does not claim all observations are accurate to one millisecond.
Equal clocks with a clean symmetric path stayed aligned with bootstrap only.
The injected +/-50/+100-ppm clocks diverged without further observations, as
predicted by their explicit clock rates. Ongoing synchronization is therefore
worth retaining under this model. A 100/250-ms interval looks promising for a
real transport trial: 80%/92% fewer AuthorityState packets respectively. These
percentages apply only to that beacon kind. Existing 100-ms assignments,
listener-compensated peer-phase messages, pings and other control traffic remain.

With equal clocks but 0..5-ms extra one-way jitter, the 20-ms case's first peer
had approximately 2.60-ms absolute p95 error; the 100/250-ms variants measured
2.31/2.52 ms and the 1000-ms variant 4 ms. The higher cadence does not remove
RTT/2 estimation bias or queued-packet timing noise. The synthetic deterministic
jitter sequence is not a representative Wi-Fi distribution. Asymmetry of 8-ms
forward/2-ms reverse introduces a 3-ms initial mapping bias. Callback snapshots
quantized to 128 frames also add phase-estimation error. Loss/outage and clock
sign-change scenarios are recorded in the raw output. Slower intervals can delay
startup/recovery; this simulation sends its first beacon immediately and does
not measure a randomly timed join or actual scheduler wakeups.

### Existing long-session correction bound exposed by the experiment

At 100 ppm, free-running phase difference reaches 360 ms after one hour. The
current compensation bounds the requested target to +/-250 ms relative to its
initial mapping; with the 1-ms deadband it settles around 249 ms of correction.
The remaining simulated phase error reaches **111 ms**, even with 20-ms beacons.
All tested periodic cadences encounter this same bound. The 50-ppm peer needs
only 180 ms of correction over that hour and remains near the deadband.

This follows from the production correction function with a non-rebased initial
offset. It is a limitation of treating accumulated clock drift as a bounded
phase offset; it is not evidence that a slower interval caused the error. This
test does not include the full device/resampler system, so it does not establish
that an actual one-hour hardware jam will exhibit exactly this error. No bound,
slew, deadband, resampler or correction policy was changed to make this pass.
Review a continuously maintained clock mapping separately before relying on
indefinite high-drift sessions. Increasing beacon frequency alone cannot solve
this modeled saturation.

### Test failures and evidence retained during this experiment

The initial Release compile found a Windows `max` macro collision in the new
test's `std::max` call. Parenthesizing the standard function fixed the test
compile; no product behavior changed. The initial accelerated and four-process
experiments then passed in 0.42 s and 16.89 s respectively.

The first final scoped run passed three of five tests in 117.55 s. The tightened
new test required all twelve incoming-audio breakdowns but had not enabled
diagnostics when creating/joining its GUI jams. All four peers were active, had
over 45000 delivered native packets, and were running fake audio, but their
breakdown field read `Waiting for live incoming-audio measurements.` The fixture
now explicitly requests diagnostics, as the existing latency comparison does.
Its twelve-path measurement assertion is retained. This was a fixture setup
error, not proof that audio stopped when authority packets were filtered.

The unchanged Server listening test also failed its existing instant readiness
assertion after playback/listening edits. Observed mapping errors were 0, -55
and -167 frames in the first three inspected peers; the third had a valid epoch
but was not grid-ready. The existing readiness bound is twice the negotiated
64-frame network packet size, i.e. 128 frames, so 167 exceeded it by 39 frames
(0.8125 ms at 48 kHz). The artifact and raw logs are preserved. Similar transient
grid assertions are recorded earlier in this review, before this experiment.
That history does not establish this occurrence's cause. The assertion and
product settings were not changed; an isolated rerun is recorded below.

The accelerated output is also exported as `simulation-peers.csv` and
`simulation-four-peer-summary.csv` in `artifacts/grid-beacon-experiment/`.
The latter derives maximum pairwise error at checkpoints from all four clocks,
including the authority's zero reference, rather than treating three follower
errors as independent jam runs. These are deterministic comparisons, not
statistical confidence intervals or repeated physical-device measurements.

### Final native validation and next design decision

The final normal MSVC Release build succeeded. The production executable remains
`release/jam2.exe`, SHA-256
`C907BEA388267D9BC2DA5DBEE6E2C102FF76B9A04A7E2C31D921ED2F64FF5051`.
All five directly relevant native CTests have passing latest runs:

| CTest | Latest result |
| --- | --- |
| `jam2_server_grid_beacon_accelerated` | Passed, 0.90 s in scoped run (0.93 s build gate) |
| `jam2_udp_four_grid_beacon_integration` | Passed, 18.52 s after explicit diagnostics setup |
| `jam2_four_mode_audio_latency_integration` | Passed, 61.88 s |
| `jam2_udp_four_listening_tools_integration` | Passed unchanged on isolated rerun, 20.47 s |
| `jam2_udp_four_peer_rotation_integration` | Passed, 20.23 s |

The four-process fixture checks exactly three remote peers and three measurable
incoming fake-audio paths at every peer (all twelve directed paths). With
beacons disabled at startup, only the authority maps its epoch; all three
followers remain unmapped while audio continues. Enabling beacons allows all
four to become ready. After six seconds of normal delivery, filtering for six
seconds preserves the already-mapped fake-clock grid while audio delivery
continues and the forwarded-beacon counter remains unchanged. This short
same-machine result does not establish absence of clock drift. A subsequent
tempo change (guaranteed to differ from saved startup preferences) invalidates
the followers' epochs until beacons are restored; all four then recover.

The unchanged listening rerun observed errors 0/+58/+79/-49 frames, all within
the 128-frame readiness bound. Its earlier -167-frame failure remains a
transient, unisolated cause; a later pass does not erase that evidence or prove
hardware timing correctness. Neither the readiness threshold nor compensation
settings were relaxed. The first five-case scoped run's two failures and their
separate dispositions are recorded above, not reported as an all-green group.

The Direct/Server audio comparison used ABBA, two four-peer starts per mode,
and 192 correlated displayed path observations per mode. Direct/Server totals
were 7.0/6.5-ms median and 9.4/8.1-ms p95. Differences -0.5/-1.3 ms passed the
existing 1.33333/2.66667-ms budgets plus display resolution. These remain
fake-audio queue estimates, not acoustic latency measurements or evidence that
this experiment improved audio latency.

Recommendation: retain ongoing clock observations. Prototype a separate small
timing beacon with revision/sample-counter information, keeping full settings
and epoch changes reliable and immediately available for joining/reset peers.
Then test 100/250-ms steady-state intervals with immediate change/recovery
updates. Those designs are recommendations for the next stage, not implemented
defaults. The slower interval must be tested against acquisition time, loss,
readiness, asymmetry and transient jitter as well as steady drift. Review the
long-session correction bound separately; a leaner or more frequent packet does
not fix it.

Production stays at 20 ms with the current 56-byte authority payload and native
36-byte header. No WAV pacing, audio-stream packet, callback, resampler or
Direct/STUN behavior changed. Direct/STUN retain their previous inline phase
calculation; Server uses the equivalent shared helper so simulation exercises
the production path. This temporary duplication preserves branch/mode scope
until a broader clock/control migration is explicitly undertaken.

No full test suite, source coverage or hardware-device experiment was run.
Logs, failed-assertion artifacts, raw state snapshots, audio observations and
per-peer/four-peer CSV summaries are under `artifacts/grid-beacon-experiment/`.
`git diff --check` passed. Existing unrelated workspace edits were preserved;
no commit or merge was performed.

## One-second and one-minute authority beacon trials and results

The next experiment adds 60000-ms delivery alongside the existing 1000-ms and
20-ms controls. Production still emits the original packet at 20 ms. The native
four-process trials downsample fresh AuthorityState datagrams in the Server
test translator, without queuing an old timestamp. Separate per-edge clocks
allow the first packet immediately, then only a fresh packet at/after each
interval. Other native packets, assignments and audio are passed unchanged.
This tests receivers with sparse observations, not sender CPU/uplink savings.

Accelerated four-clock tests additionally cover a late join just after a beacon,
an epoch reset, and losing the first beacon after the reset. Measurements now
include maximum phase error on every 10-ms simulation tick and within the first
ten minutes, rather than only once-per-second samples that could hide the
minute interval's sawtooth. Per-second samples remain for percentiles. Acquisition
and reacquisition wait are explicit; no new immediate-reset packet is invented
to conceal sparse-cadence recovery delay. All choices/results will be recorded
here after the scoped Release tests. No audio or other-mode behavior is changed.

### Initial one-second / one-minute measurements

The extended accelerated test passed in 1.57 s. It now advances ten deterministic
scenarios through seven delivery policies, each with exactly four clocks over
one virtual hour. The extra scenarios are late join, ordered epoch reset and
loss of the first post-reset beacon. A packet retains its original revision and
epoch in the harness so an in-flight pre-reset observation cannot acquire a new
epoch accidentally. Stale observations are rejected by the real authority state
machine. A final reporting addition measures simultaneous pairwise phase errors
at every 10-ms tick as well as per-peer error; its rerun is recorded below.

| Clean relative clock rate | 20-ms maximum, first 10 min | 1-s maximum, first 10 min | 60-s maximum, first 10 min |
| --- | --- | --- | --- |
| +50 ppm peer versus authority | 1.02083 ms | 1.06250 ms | 4.00000 ms |
| +100 ppm peer versus authority | 1.02083 ms | 1.10417 ms | 7.00000 ms |

These are phase errors against modeled physical clock time, rather than the
runtime's last observed target error. The minute interval permits a sawtooth:
at 50/100 ppm another 3/6 ms accumulates during each 60-second observation gap.
With one-in-17 beacon loss and a ten-second outage, maxima over the first ten
minutes were 1.5625/2.10417 ms at 1 s versus 7/13 ms at 60 s for the +50/+100-ppm
peers. The existing 250-ms total correction bound still dominates the +100-ppm
one-hour endpoint; these ten-minute measurements avoid confusing that separate
limit with cadence performance.

Late join and epoch reset occur at virtual time 1.010 s, just after a scheduled
1-s observation and after the initial minute observation. The next delivered
sample arrives 0.992 s later at a 1-s cadence or 58.992 s later at 60 s. Dropping
the first post-reset beacon extends recovery to 1.992 s and 118.992 s. During
these waits the follower remains unmapped; its phase error is unavailable, not
zero. These tests intentionally do not add immediate join/change beacons.
Bootstrap-only mode still permits a fresh initial mapping after each explicit
epoch reset, then disables continuing observations, as distinct from disabling
all authority delivery from startup.

The actual four-process 1-s trial passed in 9.66 s. It observed nine newly
forwarded authority datagrams across three follower edges during its three-second
steady window, and recovered all followers after a tempo reset in 1377 ms. The
20-ms on/off baseline passed in 18.83 s. The real 60-s trial runs for at least
one complete interval and then waits for the next scheduled epoch observation;
it is not an accelerated sleep or a delivery of an old queued packet.

Assuming the original 120-byte IPv4/UDP packet size, an actual sender change
would reduce this beacon kind from 50 packets/s / 6000 B/s per recipient to
1 packet/s / 120 B/s, or 1 packet/min / 2 B/s. Those are modeled AuthorityState
savings of 98% and 99.9667%. The translator-based real-process tests still
receive the sender's 20-ms emissions before filtering, so do not establish
actual uplink or sender-CPU savings. The separate 10-Hz assignments and other
control/audio traffic are unchanged and excluded from those percentages.

### Final four-peer comparison and validation

The simultaneous four-clock maxima below are measured every 10 ms during the
first ten virtual minutes, after all peers are mapped. Clocks are 0/+50/-50/
+100 ppm; each cell is the largest instantaneous difference between any pair,
including the authority. Unmapped periods are recorded separately and never
treated as zero phase error. The first-ten-minute window precedes saturation
of the existing 250-ms correction bound.

| Scenario | 20-ms beacon | 1-s beacon | 60-s beacon |
| --- | --- | --- | --- |
| Clean symmetric path | 2.04167 ms | 2.16667 ms | 10.9792 ms |
| Equal clocks with injected 0..5-ms jitter | 3.12500 ms | 5.00000 ms | 5.00000 ms |
| 128-frame clock-counter snapshots plus drift | 3.35417 ms | 4.43750 ms | 12.3333 ms |
| Drift, one-in-17 loss and ten-second outage | 3.52083 ms | 3.64583 ms | 19.9792 ms |

These are deterministic synthetic conditions, not measured distributions of
device drift or Wi-Fi delay. Per-second percentiles are exported too; they do
not replace the every-tick maximum when assessing long-interval sawtooth error.
The +100-ppm one-hour clamp still raises worst pairwise error to approximately
112/112.042/114.979 ms for 20 ms/1 s/60 s in the clean case. That separate limit
has not been fixed or excluded from the raw output.

The actual four-process Server trials passed, with all twelve incoming audio
paths measured during their steady windows:

| Delivery | CTest duration | Time from tempo-change command until all four grid-ready |
| --- | --- | --- |
| 20-ms on/off baseline | 18.83 s | 349 ms after explicit beacon restoration |
| 1-s delivery | 9.66 s | 1377 ms, automatic next periodic observations |
| 60-s delivery | 125.59 s | 57625 ms, automatic next periodic observations |

The baseline's restoration is explicit after verifying that blocked packets
prevent mapping, so its 349 ms is not a pure cadence-matched recovery latency.
All recovery timings include GUI commands, runtime scheduling, state publication
and snapshot observation; they are not UDP RTTs. The 60-s trial forwarded one
additional beacon on each of the three authority edges during its 62-second
steady window, then retained invalid follower epochs until the next periodic
beacons after the tempo change. No first/reset beacon was forced through the
minute filter. All four followers/authority observations were grid-ready at the
end, with zero reported target mapping error in that run. This fake-clock result
does not establish zero real-device phase error between those observations.

The real tests pass full settings assignments independently at their existing
cadence. Receiving those settings alone did not supply the fresh sample-clock
observation needed to recover the new epoch. Initial acquisition is optimistic
in these socket trials because the per-edge filter allows its first packet
immediately. The accelerated late-join test covers a peer arriving just after
a periodic transmission. Missing the first recovery beacon was tested in the
accelerated test, not by waiting two real minutes in an additional socket trial.

All four directly relevant Release CTests have passing latest runs:
`jam2_server_grid_beacon_accelerated` (final 1.57 s),
`jam2_udp_four_grid_beacon_integration`,
`jam2_udp_four_grid_beacon_1s_integration` and
`jam2_udp_four_grid_beacon_60s_integration`. The three socket cases passed together
in 154.08 s. No assertion/threshold was relaxed after a failure in this stage.
The final simulation contains 70 one-hour four-clock runs and 210 follower
summaries, plus 70 simultaneous pairwise summaries. Other connection modes,
audio encoding, callbacks, WAV pacing and product sender behavior are unchanged.
The public executable hash remains
`C907BEA388267D9BC2DA5DBEE6E2C102FF76B9A04A7E2C31D921ED2F64FF5051`.

Conclusion: 1 s remains close to 20 ms for steady tracking in these cases;
60 s produces a larger drift envelope and long join/reset/loss recovery waits.
An immediate, reliably delivered fresh epoch observation on important changes
would need to be designed and tested separately before choosing either as a
production steady-state interval. Quantized actions while a follower remains
unmapped were not exercised in this cadence trial; an all-ready result after
recovery does not establish that actions submitted during the gap were retained.
No interval is selected as the new default by this work.

Logs, raw socket snapshots, `simulation-peers.csv` and
`simulation-four-peer.csv` are retained under `artifacts/grid-beacon-1s-60s/`.
Only scoped Release tests were run; no full suite, coverage or hardware test.
All changes remain on UDP-Multiplex. `git diff --check` passed; no commit or
merge was performed.

## One-second Server authority beacon implementation and validation

Server UDP now targets one second between steady AuthorityState publications.
Direct/STUN retain their previous 20-ms emission branch. The outer grid-service
turn remains 20 ms because proposals, 100-ms assignments and peer-phase traffic
still use it. One second is a minimum steady elapsed interval with normal
service/scheduler granularity, not a hard real-time transmit guarantee.

An allocation-free `AuthorityBeaconSchedule` records the last attempted
publication's revision, epoch and active-peer count. A change requests a fresh
observation at the next grid-service turn instead of waiting the full second.
The first publication is immediately due. Only Server uses this gate. A new
active count helps joining peers acquire timing promptly; an unobserved peer
replacement that leaves the count identical still receives the next periodic
beacon. Full-grid packets are rebuilt with a fresh engine sample timestamp;
no old frame or timestamp is queued for later delivery.

The schedule advances after a broadcast attempt, independently of peer ACKs or
send success, matching the existing best-effort snapshot semantics. One failed
peer cannot hold healthy peers at a rapid retry rate. A lost/failed event beacon
recovers through the next periodic observation; this is not a new reliably
acknowledged epoch-transfer protocol. The existing correction bound is unchanged.
Structured jam diagnostics expose `grid_authority_beacon_interval_us`,
`grid_authority_beacon_batches` (publication attempts) and
`grid_authority_beacon_packets` (successful native sends across peers).

### Exact unchanged packet layout

This is native protocol version 2, type 6 `MetronomeState`, kind 3
`AuthorityState`. It shares the selected UDP socket with other multiplex traffic
but is not wrapped in the reliable `J2MX` stream envelope or application-v4
record. All multi-byte fields here are little endian. UDP payload is exactly
92 bytes: native header 36 plus metronome payload 56. IPv4 plus UDP adds 28 bytes
(120 total); IPv6 plus UDP adds 48 (140 total), excluding options/extensions and
link-layer overhead.

| Header offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `JAM2` magic |
| 4 | 1 | native version 2 |
| 5 | 1 | packet type 6 |
| 6 | 2 | payload length, 56 |
| 8 | 8 | session ID |
| 16 | 4 | packet sequence, checked by metronome replay window |
| 20 | 8 | current sender engine sample frame |
| 28 | 8 | SipHash-2-4 authentication tag over header/payload with tag field zeroed |

Payload offsets below are relative to byte 36:

| Payload offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | signed BPM; sign encodes enabled/disabled |
| 4 | 8 | grid revision |
| 12 | 8 | authority epoch sample frame |
| 20 | 1 | marker, message kind, metronome mode, running/reset and tempo-pulse flags |
| 21 | 1 | beats per bar |
| 22 | 1 | subdivision |
| 23 | 1 | written beat unit |
| 24 | 16 | 128-step play mask |
| 40 | 16 | 128-step accent mask |

The flag byte is bit7 marker; bits1:0 kind; bits3:2 mode; bit4 running;
bit5 epoch-reset; bit6 indicates tempo-pulse-units equal to 3. AuthorityState uses kind 3 and
normally leaves its reset flag clear; ordered assignments carry reset intent.
Most payload fields remain repeated settings, particularly the 32 mask bytes.
No strings, JSON, CBOR, schema names or compression are involved in this packet.
This change reduces cadence only, leaving field sizes, authentication, parser,
versions and the live audio packet format intact. Packet compaction remains a
separate future change rather than being silently included here.

### Initial validation on the production one-second cadence

The normal MSVC Release build and deterministic schedule fixtures passed.
Fixtures cover first publication, no early steady publication, exact deadline,
revision/epoch/active-count refresh before the steady deadline, no post-refresh
burst and saturating timestamp arithmetic. The four-process production check
passed in 19.67 s, reporting five snapshot-observed publication batches and
18 authority datagrams across the three edges during its six-second steady
window. Snapshot counters publish asynchronously, so the batch delta need not
be perfectly co-timed with the translator's packet-count window. This confirms
the product sender cadence, not just receiver-side filtering.

The Direct/Server ABBA latency comparison passed in 61.90 s and explicitly
asserted 20000/1000000-us intervals in those modes. Direct/Server median totals
were 6.5/6.3 ms, p95 totals 9.1/7.9 ms (deltas -0.2/-1.2 ms). These are 192
correlated fake-audio displayed path observations per mode across two starts,
using unchanged latency budgets. They show no added Server queue-estimate
latency in this run, not an acoustic latency improvement. Server listening
passed in 22.16 s. Shared-content, recovery and impaired-route checks continue
below; no full suite is requested or run.

### Listener Compensated phase-metadata lifetime

All seven initially scoped tests passed in 263.01 s, including shared content,
recovery and impaired rotation. A subsequent code audit found that the Listener
Compensated consumer expires peer phase metadata after 500 ms. Applying that
unchanged expiry to the authority's new 1-s broadcast would falsely remove the
authority from contributing peers between ordinary successful beacons. Those
initial Shared Grid cases did not exercise this condition.

For Server authority metadata only, expiry now uses two beacon intervals plus
the existing 500-ms margin: **2500000 us** at the chosen cadence. Non-authority
peer phases still arrive every 100 ms and retain 500000-us expiry. Direct/STUN
retain their original 500000-us check for every peer. Revision and endpoint
checks still reject stale-grid or inactive-source metadata. Holding authority
epoch metadata longer can retain it for up to 2.5 s after observations cease;
this is an explicit control-metadata lifetime, not an added audio delay. Missing
multiple observations can still make it stale, as intended.

Diagnostics add `grid_authority_phase_freshness_us`,
`grid_authority_phase_age_us` and `grid_authority_phase_fresh`. Deterministic
fixtures test the old 500-ms boundary, the exact 2.5-s endpoint, expiry, zero
receive time and a backwards clock. The four-peer test invokes the actual
`metronome.mode` combo to select Listener Compensated, then observes six seconds
of one-second publications, requiring fresh authority metadata throughout and
an observed age greater than 500 ms. This demonstrates crossing the previous
expiry, rather than only checking immediately after a packet arrives. The
Direct/Server audio comparison also asserts mode-specific expiry values.

### Final validation and retained choices

The final normal MSVC Release build succeeded. `release/jam2.exe` SHA-256 is
`F9C3FCDC2CC6DCA8309EA7886B5CE969C5F3E4A45693E1B84C2ACC50C3805FF8`.
The real four-peer test, including the actual Listener Compensated mode combo,
passed in 25.88 s on this executable. It recorded 18 steady authority datagrams
across three links and an authority metadata age of 965812 us while remaining
fresh throughout the six-second listener observation. This crosses the old
500000-us expiry. All twelve incoming fake-audio paths remained measurable.

The final three-case group passed in 82.37 s: accelerated schedule/freshness
fixtures and simulation (1.55 s), Direct/Server audio comparison (61.66 s), and
Server listening integration (19.15 s). The audio test explicitly verifies
Direct/Server interval values 20000/1000000 us and phase-metadata validity
windows 500000/2500000 us. Median totals were 5.9/5.7 ms and p95 totals 8.2/7.9 ms;
deltas -0.2/-0.3 ms passed unchanged budgets. It uses two four-peer jam starts
per mode and 192 correlated path observations per mode. No hardware/acoustic
equivalence is claimed.

Before the mode-specific expiry audit, all seven scoped cases passed together
in 263.01 s: accelerated, production four-peer beacon, audio comparison, Server
listening, shared content (65.21 s), recovery (73.13 s), and impaired rotation
(19.36 s). The final expiry-only follow-up reran its four directly affected
cases described above; it did not repeat the already passing shared-content,
recovery and rotation cases. There were no test failures in this implementation
stage. The broader earlier intermittent readiness limitation remains recorded
above and is not declared solved by these passes.

Retain the one-second Server steady cadence, event-triggered fresh observations,
mode-specific metadata expiry and raw diagnostics. An event observation can be
lost; next-period recovery and revision validation remain necessary. The 2.5-s
metadata lifetime is not a change to clock correction gain, smoothing, deadband,
slew, audio buffering or sample-rate conversion. The existing 250-ms accumulated
correction limit remains. Full settings/masks are still repeated; the native
packet remains version 2 and 92 UDP-payload bytes, with no authentication change.

Logs/raw observations are under `artifacts/grid-beacon-production-1s/`. No full
suite, source coverage, alternate public binary, commit or merge was used.
All work stays on UDP-Multiplex; Direct/STUN emission and expiry branches,
live audio formats/callbacks and WAV pacing retain their previous behavior.
`git diff --check` passed and unrelated workspace edits were preserved.

## Grid assignment repetition experiment and results

User-requested comparison: assignment delivery disabled, 1 s, 60 s and 600 s,
against the current 100-ms background repeat. Production sender/format and the
one-second authority beacon remain unchanged. The accelerated test uses the
production `SessionAuthority` ordering/acceptance/duplicate/rejection logic and
`AuthorityBeaconSchedule`. It measures revision/authority convergence and fresh
beacon acceptance, not simulated sound quality or audio clock drift.

The producer already sets its assignment deadline immediately due when it orders
a changed revision. Interval variants preserve that behavior; otherwise a
pure periodic filter would incorrectly make every ordinary change wait ten
minutes. Tests separately lose the first changed assignment to expose the repeat
interval's recovery cost. Late join does not invent a new assignment revision
or an immediate targeted response. Both disabling all startup assignments and
allowing only the initial publication are included to distinguish acquisition
from steady maintenance. New revisions, late join, lost join assignment,
authority handoff and delayed old assignments are exercised with four states
over one virtual hour per case. Unresolved waits are reported as -1, not zero.

The four-process Server fixture drops only native Assignment packets, leaving
the one-second AuthorityState beacons, proposals, other control messages and
audio untouched. It measures all twelve incoming fake-audio paths, blocks
startup, restores assignments, disables steady repeats, then tests a new epoch
while assignments remain blocked. It also samples real 1-s and 60-s delivery.
The test translator preserves changed-revision publications and forwards fresh
packets, never queued old timestamps. Its per-edge first delivery is immediate;
late-join timing is therefore established by the global-producer virtual test,
not extrapolated from that optimistic translator startup behavior.

These are receive-side packet-loss/thinning experiments: the product still
emits 100-ms assignments toward the translator. Actual upload/CPU savings are
not measured. A true 600-s interval is exercised using virtual timestamps;
there is no ten-minute wall-clock sleep or claim of hardware validation.

### Accelerated assignment results

Seven scenarios times six delivery policies simulate 42 one-hour four-peer
sessions. The final scoped run passed in 0.19 s. The table uses the third
follower; all receiver/authority outcomes and duplicate/stale counters are
exported in `assignment-simulation.csv`. Ready here means current revision and
authority accepted, plus an accepted beacon for that revision (or being the
assigned publisher). It is a state-machine observation at 10-ms virtual service
ticks, not a rendered sound/phase measurement.

| Background assignment repeat | Stable datagrams/hour, three recipients | Late-join ready wait | Ready wait after first changed assignment is lost |
| --- | --- | --- | --- |
| 100 ms, current | 108000 | 1.010 s | 1.010 s |
| 1 s | 10800 | 1.010 s | 1.010 s |
| 60 s | 180 | 59.010 s | 60.010 s |
| 600 s | 18 | 599.010 s | 600.010 s |
| Initial publication only | 3 | Unresolved for late join | Unresolved after change |
| Fully disabled | 0 | Cannot acquire initially | Cannot acquire initially |

The 100-ms and 1-s ready results coincide for this chosen event phase because
the follower also needs the next one-second clock beacon. They are not a bound
for every possible network schedule. Packet arrival for the lost revised
assignment itself was 0.102/1.002/60.002/600.002 s. The ready metric includes the
beacon and virtual service boundaries, not just assignment delivery. Late join
is at virtual 1.010 s. Losing its first assignment increased ready waits to
2.010/119.010/1199.010 s for 1/60/600-s repeats, respectively.

Without loss, a newly ordered revision was published immediately and its
assignment arrived in 2 ms at every periodic cadence; ready followed at the
10-ms service boundary. This preserves the current producer's existing
deadline-reset behavior. It does not imply two-millisecond real GUI recovery.
All periodic policies converged after ownership handoff; delayed old revisions
were rejected without rollback. The initial-only case retained an unchanged
grid but failed to distribute changed revisions, ownership or a late join.
Fully disabled receivers rejected the authority beacons with unknown revision
instead of silently learning unauthorized grid ownership from them.

Stable delivery has exactly one useful accepted assignment per follower in
this model. Subsequent current-revision duplicates were 35999, 3599, 59 and 5
per follower at 100 ms, 1 s, 60 s and 600 s. These duplicate counters demonstrate
redundancy for an unchanged, already-known grid; they do not justify discarding
the delivery/recovery mechanism for changes or missing state.

### Four-process Server measurements

The actual four-peer assignment experiment passed in 80.70 s:

- With all assignments filtered from startup, followers had no valid mapped
  epoch even though one-second authority beacons and all twelve audio paths
  continued.
- After restoring assignments, all four acquired the same valid epoch. Turning
  repeats off for three seconds retained that unchanged epoch with beacons
  continuing. A subsequent tempo/epoch change remained divergent for the
  observation window while assignments were still blocked.
- Restoring 1-s assignment delivery recovered convergence in 1186 ms. Its
  nominal three-second steady observation counted nine assignment datagrams
  across the three edges, versus 96 at the current cadence. Observation windows
  are minimum loop durations and can overshoot while collecting four snapshots;
  these counts are not matched-duration throughput benchmarks.
- The 60-s delivery window counted six datagrams across three edges over a
  nominal 62 seconds (initial allowed copies and one repeat). The unchanged
  epoch and measurable incoming audio paths persisted. A new changed revision
  then bypassed the interval filter and converged in 104 ms, demonstrating why
  ordinary change handling must be distinguished from background repetition.

The real trial did not wait ten minutes or inject a lost changed assignment at
the 600-s cadence. Those recovery costs are established by virtual timestamps
and the production state machine. The native translator allows a new edge's
first packet promptly, so it cannot establish worst-case periodic late-join
latency; the global broadcast schedule in the accelerated test covers that.

### Validation, fixture correction and retained policy

The initial accelerated run failed in the reordered-old/fully-disabled case:
the fixture injected a valid old coordinator assignment despite promising that
all assignments were disabled. The real state machine correctly accepted that
first known revision, violating the fixture's contradictory zero-assignment
assertion. The fixture now omits that injection only in the fully-disabled case.
Reordering remains exercised for the periodic and initial-only policies; no
product ordering or validation rule was changed. The failed log is retained.

The normal MSVC Release build succeeded. All five scoped CTests passed together
in 129.36 s: `jam2_server_grid_assignment_accelerated`,
`jam2_server_grid_beacon_accelerated`,
`jam2_udp_four_grid_assignment_integration`,
`jam2_udp_four_grid_beacon_integration` and
`jam2_udp_four_peer_rotation_integration`. The last two retain existing native
clock/listener-mode and loss/route-rotation checks for the modified test fixture.
No full suite, source coverage or real-device audio test was run.

The public `release/jam2.exe` is unchanged, SHA-256
`F9C3FCDC2CC6DCA8309EA7886B5CE969C5F3E4A45693E1B84C2ACC50C3805FF8`.
Production stays at 100-ms assignment repeats and one-second authority beacons.
No audio, control wire layout, reliability policy, other connection mode or WAV
pacing was changed. Logs/raw socket snapshots/CSV are preserved under
`artifacts/grid-assignment-experiment/`. `git diff --check` passed; no commit or
merge was performed.

Interpretation: assignment state is necessary, but repeating already-accepted
unchanged state at 10 Hz is not necessary for clock maintenance. A one-second
repeat is a lower-cost periodic alternative in these cases (90% fewer stable
assignment datagrams). Minute/ten-minute repeats save 99.8333%/99.9833% of this
message kind but expose long waits for lost updates or joining peers. An
acknowledged on-change/on-join assignment exchange is a more direct route to
eliminating redundant repeats while retaining delivery. That is a future design,
not implemented here. Behaviour of user actions submitted during an unknown or
obsolete assignment gap, including recording deadlines, is not established by
this state-delivery experiment and must not be inferred from audio still flowing.


## Event-driven Server grid assignments with applied receipts

Scope: UDP-Multiplex branch, Jam2 Server datagram control path only. This
supersedes the retained 100-ms Server assignment policy in the preceding
experiment. Direct/STUN still send their existing 56-byte MetronomeState
assignments every 100 ms. One-second Server AuthorityState observations and
100-ms listener PeerPhase messages are separate and unchanged. Audio packets,
callbacks, pacing, DSP, WAV pacing and transport-intent receipts are unchanged.

The bootstrap coordinator now sends the latest assignment independently to each
active peer on a new ordered grid revision or initial/re-established endpoint.
A fresh authenticated runtime HELLO requests its current assignment again,
including when membership and the physical multiplex survive a runtime restart.
Proof resets, inactive endpoints and membership removal discard pending delivery
state. Reconnection creates a fresh random 64-bit application token.

Logical control-stream reauthentication on a retained multiplex explicitly
requests grid recovery without resetting UDP proof, audio sockets or the
engine. The request uses a fresh nonzero random token, retried every one second
until a validated assignment echoes it. The coordinator delivers only to that
requester, using the request nonce as the applied-delivery token. Matching
repeated requests preserve the current delivery/deadline instead of restarting
its retry backoff; stale requests use the normal metronome replay gate. The
request has no state payload: the answer is always the latest authoritative
snapshot. This also works when no grid revision has been acquired yet; the
coordinator ignores a request until it has a grid and the requester retries.
Request count, pending state and retry interval are exposed in the same
structured stats. A lost request or response cannot leave logical reconnection
silently relying on a periodic blind assignment broadcast. No peer
is a barrier for another peer's action or acknowledgement. Only the latest
snapshot is retained: a newer revision supersedes an older pending delivery;
these are state snapshots rather than a log of commands requiring replay.
The network/control runtime owns this delivery state. These native typed grid
packets retain the existing raw control routing on the same per-peer UDP
multiplex socket. They do not wait for an ordered GUI/WAV stream hole to close.
Shared CPU load, socket pressure and network congestion can still delay them.

Each peer receives a distinct token for its delivery. Retransmissions use that
token and immutable assignment contents, with fresh native sequence numbers so
the normal metronome replay gate permits recovery without reopening old packet
replays. Retry deadlines start at 100 ms, then 200/400/800 ms, capped at one
second while that authenticated endpoint remains active. The network loop
checks them on existing grid-service turns (nominally 20 ms); deadlines are not
a promise of exact wall-clock wakeup under load. Failed socket sends also wait
for the retry deadline. There is no retry count limit for an active peer that
has not applied the latest state. Inactive peers stop receiving retries and
acquire current state on reactivation. Settled peers receive no assignment
refresh traffic. The retry bounds and delivery counters are exposed through
structured network snapshots, together with each target peer's revision,
attempt count and applied status; global pending/applied counts count active
remote peers at the coordinator, not a session quorum.

Wire choice (fixed native JAM2 v2 authenticated header stays 36 bytes):

| Packet | Type | Payload | Header timing slot | Total UDP application bytes |
| --- | --- | --- | --- | --- |
| Server grid assignment | 9 | existing 56-byte typed grid record + LE u64 delivery token | assigned authority peer ID | 100 |
| Grid assignment applied | 10 | LE u64 applied revision | delivery token | 44 |
| Grid assignment recovery request | 11 | none | fresh request token | 36 |

The new assignment's first 56 bytes retain kind=Assignment (2). Its token must
be nonzero. All three packet kinds have exact lengths checked by the native parser;
the whole header and payload remain authenticated. No JSON, allocation-based
schema or additional audio framing is involved. The 8-byte token distinguishes
application deliveries across reconnects and protects completion from stale
ACKs for the same revision. An ACK is accepted only from the active authenticated
endpoint to which that token was sent, for that delivery's exact revision and
token. Matching duplicate ACKs are harmless; old/wrong/unsent tokens cannot
complete a delivery. There is no peer-wide sequence or quorum dependency.

One applied receipt proves receipt and application together; there is no second
receipt-only packet. The receiver first validates coordinator authority and
revision through SessionAuthority, stores the applicable shared settings,
and applies the ordered grid once. Recording isolation or a newer pending
local proposal defers the receipt. The control loop then waits for valid clock
mapping and for the existing engine snapshot to match runtime metronome
pattern, mode, enabled state, leader-click role and epoch before sending the receipt. A full
engine command queue or an unconsumed command therefore cannot produce an
early success ACK. This confirms observable engine state, not that a click
has already been rendered or heard, and is not a distributed simultaneous-start
barrier. Local click masks and local enable policy retain existing ownership.
A peer without usable clock mapping cannot report application yet; fresh
AuthorityState observations remain necessary to establish that mapping.

A lost applied receipt causes a retransmitted current assignment and another
receipt after the same application checks. Duplicate assignments do not
execute the epoch-reset action again. Same-revision retries must have identical
assignment contents; a different payload for the already-accepted revision is
ignored. Old revisions and unauthorized sources retain normal rejection rules.
The sender caches one canonical 56-byte snapshot per revision, not complete
retry datagrams; its token is per target and fresh sequence/authentication is
constructed for each attempt. AuthorityState can update live clock observations
without mutating that pending snapshot. This change adds no cached-packet retry
scheme for other control or WAV traffic.

Compatibility limitation: Server peers must use this updated branch build on
both ends. There is no negotiation/fallback to blind legacy assignments for a
mixed Server session; old builds reject the new native packet kinds, updated
Server runtimes reject legacy kind-2 assignments. The generic parser recognizes
the new kinds in all builds, but only a supplied Server datagram transport
activates emission/application/receipt handling. Other connection modes retain
their original send policy and wire bytes.

Costs and expected savings: a loss-free already-mapped revision costs one
100-byte assignment plus one 44-byte applied receipt per target, then no steady
assignment packets. IPv4+UDP adds 28 bytes to each, for 200 bytes per target
(240 with IPv6+UDP). This replaces 10 x 120 IPv4 bytes per second per target for
an unchanged legacy assignment. A logical recovery adds one 36-byte request (64 bytes including IPv4/UDP),
and requests repeat only while pending. Initial mapping may need the next one-second
clock observation and can cause retries before the applied receipt. Slow,
isolated or lossy targets continue paying their bounded retry traffic; the
healthy targets remain quiet. This saves only the assignment component, not
all control-plane packets. Native CSV grid_assignments_sent previously counted
broadcast attempts; Server now counts targeted attempts. Use the new structured
grid_assignment_packets for successful targeted sends and grid_assignment_retries
for retry attempts when comparing modes.

### Validation and measured results

The final normal MSVC Release build passed. Ten directly relevant CTests passed
serially in 163.72 seconds; no full suite or coverage collection was run:

- jam2_udp_control_transport_units (18.61 s)
- jam2_udp_multiplex_units (0.05 s)
- jam2_core_boundary_units (0.02 s)
- jam2_server_grid_beacon_accelerated (1.53 s)
- jam2_grid_assignment_delivery_units (0.01 s)
- jam2_application_boundary_units (0.03 s)
- jam2_udp_four_grid_beacon_integration (26.01 s)
- jam2_udp_four_grid_assignment_integration (35.75 s)
- jam2_four_mode_audio_latency_integration (61.83 s)
- jam2_udp_four_peer_rotation_integration (19.88 s)

The new native unit target tests zero/unsent/wrong/stale ACK rejection,
matching duplicate ACKs, retry deadlines/backoff/cap, cancellation/reset,
supersession and reconnect token changes. Fixed packet sizes, authentication
of assignment tokens and request nonces, truncation and invalid payloads are
covered alongside unchanged 92-byte legacy assignments. Its deterministic
four-peer case exercises the production SessionAuthority and delivery tracker:
one lost assignment, two lost applied ACKs and one delayed application converge
with ten initial assignment attempts, one application per follower and zero
assignment repeats at a one-hour idle timestamp. Old revisions cannot roll the
state back and old same-revision connection tokens cannot complete reconnection.
The runtime-host boundary test verifies request coalescing/one-time consumption,
cleanup/reset, stopped-worker rejection and independence from UDP proof resets.

The real exactly-four-process Server fixture uses release/jam2.exe, the real Go
signaller, fake tone input and a native UDP translator for targeted packet loss.
It verifies:

- Blocking startup assignments leaves followers uninitialized despite arriving
  clock beacons and all twelve measured audio paths. Restoring delivery acquires
  the current assignment and three applied receipts.
- Four nominal three-second settled windows (startup, control reconnect,
  ACK-loss recovery and rejoin) each observe ZERO new assignment packets and
  ZERO new applied ACK packets. The production sender's successful packet
  counter also stays constant. These are actual sender/emission observations,
  unlike the preceding interval-thinning experiment. The observation loops can
  slightly exceed three seconds while collecting snapshots.
- Logical control reconnect retains peer identity, the same revision and every
  physical audio socket. Its first recovery request is deliberately lost;
  the requester retries and completes a fresh applied handshake. No audio
  proof reset is used.
- Blocking applied ACKs leaves three coordinator deliveries pending even though
  the actual follower engine state has acquired the revision. Retries do not
  repeat the epoch reset. A newer revision supersedes the unacknowledged one;
  releasing ACK loss completes only the latest delivery and traffic becomes
  quiet again.
- Dropping the first three changed assignments recovers through retries. In
  the final run the complete application/readiness observation took 1303 ms.
  This includes the next independent one-second clock observation, engine/GUI
  observations and snapshot polling; it is not the assignment RTT or a 1.355-s
  retry timer. The first retry deadline remains 100 ms.
- Blocking assignments to one peer still lets the other two apply and ACK;
  the coordinator reports two applied and one pending. Unblocking restores it.
- A real local input take on one follower enables recording isolation. That
  peer retains its old grid revision and emits no success receipt while the
  healthy peers apply the changed revision. The deferred counter increments;
  after stopping the take it acquires and acknowledges the latest assignment.
  This fixture disables synchronized recording so the existing whole-group
  recording protection does not reject the creator's edit before runtime.
- Explicit leave removes the inactive peer from pending delivery. Healthy peers
  can change the grid while it is absent. Rejoining with the same invitation
  acquires current state and restores all twelve measured audio paths.

The four-process assignment workflow additionally switches to the supported
creator leader-audio mode and confirms its new revision through applied
receipts. The ACK condition includes the engine leader-click role as well as
pattern/mode/enabled/epoch. The existing clock/beacon and route-rotation
workflows also pass, preserving
one-second Server observations and existing loss/ownership behavior.

Audio validation uses the unchanged four-peer ABBA mode comparison: Direct,
Server, Server, Direct, with 64-frame packets, 128-frame fake callbacks at
48 kHz. Each mode contributes 192 path observations (two runs x eight snapshot
sets x twelve directed paths). These are correlated observations on one
machine, not 192 independent device trials.

| GUI latency estimate | Direct | Server | Server minus Direct |
| --- | --- | --- | --- |
| Median total | 6.3 ms | 6.4 ms | +0.1 ms |
| p95 total | 8.3 ms | 8.0 ms | -0.3 ms |
| Median network component | 0 ms | 0 ms | 0 ms |
| Median playback ring | 2.813 ms | 2.958 ms | +0.146 ms |
| Median output callback | 2.667 ms | 2.667 ms | 0 ms |

Both differences pass the unchanged absolute budgets of one packet (1.333 ms)
at the median and one callback (2.667 ms) at p95, plus 0.1-ms GUI display
resolution. This run does not show an audio-latency regression; the small
differences are not evidence of a latency improvement. The earlier scoped
run measured Server minus Direct at -0.1 ms median and -0.2 ms p95; the
median changed sign in the final run, so these runs do not establish a
repeatable increase. No real-device,
Wi-Fi, physical sound-to-sound or cross-machine clock measurement was made.

Aggregate four-GUI-process CPU during the audio observation windows was
130.234% of one core for Direct and 160.602% for Server. This includes the whole
GUI/fake-audio/network modes, not only assignment processing. There is no CPU
before/after toggle benchmark for this feature, so the difference cannot be
attributed to the new assignment policy or used to claim a CPU saving. Raw
per-run CPU/wall times are retained. The confirmed efficiency gain is removal
of unnecessary settled assignment datagrams.

### Findings during implementation and remaining choices

Four intermediate failures were investigated and their evidence retained:

1. The first rejoin assertion checked twelve GUI audio paths immediately after
   the grid applied. Snapshots showed the same current revision, three applied
   receipts and active endpoints, but the slower GUI audio diagnostics still
   showed zero/two paths on some peers. The fixture now waits for its required
   measured paths before asserting them; no product audio code was changed.
2. The first recording case used synchronized recording. Existing whole-group
   protection correctly held all four peers and rejected the creator's tempo
   edit, so no new revision or pending assignment existed. The fixture now
   selects a local recording to exercise one isolated receiver; recording
   protection itself is unchanged.
3. The logical reconnect check initially found no new assignment: the existing
   multiplex correctly retains its proven UDP path and therefore emits no new
   native HELLO. Event-driven delivery needed an explicit grid-only request.
   Type 11 plus the runtime/controller request seam resolves that gap without
   changing audio endpoint proof or socket ownership. The final CTest also
   loses its first request to verify recovery.
4. An added GUI leader-handoff case assumed a joiner could become the leader
   audio source through its metronome button. That assumption is invalid:
   MainWindow::setMetronomeEnabled explicitly suppresses local leader audio on
   followers (followerUsingLeaderAudio). Disabling GUI metronome synchronization
   and toggling off/on do not remove that role restriction; the creator remains
   publisher. The fixture now exercises the supported creator leader mode and
   engine-role receipt. No product GUI routing or audio behavior was changed.
   Native SessionAuthority can reconcile source-claim handoffs, but the current
   GUI exposes only the creator as leader-audio source. That existing product
   limitation remains for later review; native handoffs retain state-machine
   coverage rather than an unsupported GUI source-claim test.

There is no periodic repair packet after an applied ACK. An actual endpoint
reconnect/runtime HELLO or logical reauthentication requests another handshake.
A hypothetical silent loss of receiver state without any reconnect, runtime
HELLO or revision change is not detected by an unchanged-revision beacon;
future code that deliberately discards grid state must request recovery too.
Coordinator process restart/revision persistence is still the existing session
lifecycle contract; this change does not introduce coordinator failover,
checkpoint persistence or revision-number recovery for a replaced coordinator.

Future efficiency work should be justified by real traces: optionally piggyback
an applied revision/token when a suitable control packet is already leaving,
or use measured RTT to tune retries within explicit numeric bounds. A separate
received-but-not-applied status could reduce repeats during long isolation,
but adds packets/state and is deliberately omitted. Current applied receipts
provide both required guarantees in one packet, and the one-second retry cap
bounds isolated-peer traffic. No acknowledgement aggregation or new cache of
retry datagrams was introduced for these native grid packets.

Final public binary SHA-256:
07241DDFBA3E01CD37A0894F4FB627790C853A897B1C4B481104BF60A69AAE09.
Build output, final CTest log, raw assignment/beacon states and audio
JSONL/CSV are preserved in artifacts/grid-assignment-ack/. Intermediate failed
logs/snapshots are retained alongside them. All work remains on UDP-Multiplex;
no commit, merge or other branch operation was performed.


## Unified multiplex transport and mandatory peer names

Goal scope: UDP-Multiplex only; all public network create/join modes migrate to
one current multiplex and typed control implementation. Direct/STUN contact the
forwarded creator directly and do not register or ping a Jam2 Server. The
creator coordinates peer-to-peer candidate discovery, simultaneous punching and
fresh sockets/attempt identities on failure, preserving healthy links. Its
forwarded listening port remains fixed. STUN remains endpoint discovery only.
Audio packet shape, callback/DSP fast path, direct full mesh and WAV star
transfer/pacing are retained. TCP and older multiplex/legacy codec paths have
been removed after replacement workflows passed. No TCP fallback remains.

Before edits, the existing Release CTest jam2_four_mode_audio_latency_integration
passed in 61.85 s at commit 231bed6 (full commit recorded in artifacts). Each
mode had 192 path observations. Direct/TCP median total was 6.6 ms, p95 8.1 ms;
Server/multiplex median 6.4 ms, p95 8.3 ms. Median/p95 differences -0.2/+0.2 ms
passed the unchanged one-packet/one-callback budgets. This is a correlated
four-process single-host fake-audio estimate, not physical sound-to-sound or
Wi-Fi validation. Baseline build output and full CTest details are preserved
in artifacts/unified-udp-multiplex/. No alternate public binary was copied;
all tests use release/jam2.exe, and no other branch is modified.

Names: the user explicitly permits duplicates. Names never replace stable peer
IDs, authentication tokens or compact numeric handles. Input must normalize to
NFC and contain 1-8 Unicode letter scalar values, with at most 32 UTF-8 bytes.
NFC allows composed accented letters; remaining combining marks, spaces,
numbers, punctuation, markup, controls, bidi controls, zero-width joiners and
invisible Hangul letter fillers are rejected. Raw input is bounded before
normalization. UTF-8 received from the network must roundtrip exactly and must
already be canonical NFC; replacement characters, overlong UTF-8, encoded
surrogates and noncanonical sequences are rejected. This counts letters/scalars,
not UTF-8 bytes or UTF-16 code units. Scripts needing separate combining marks
that NFC cannot compose are therefore unsupported by the letters-only policy.
Names can be visually similar, and duplicate names intentionally remain visually
identical; inspectable stable peer IDs remain distinct.

The name is bound into the authenticated join transcript. Creator-validated
membership maps the name to its stable peer token/ID for all participants.
Typed membership emits a one-byte name length and name bytes only when that
binding is first defined or changes; later entries use zero length and the
existing stream binding. Reconnects initialize fresh codec bindings. No name
is carried in audio packets or repeatedly attached to ordinary control actions.
A name may change on a later explicit join using the same app identity; the
creator's authoritative binding updates then. Active authenticated names are
not a free-form rename operation. UI create/join adds mandatory name controls,
CLI uses --name, and input/handshake/membership parsing share one validator.
The GUI will render names without Peer N fallbacks; unbound/incomplete membership
is not presented as a named established peer. Validation and GUI tests are
being added alongside migration.

### Direct/STUN bootstrap implemented and checked

Direct and STUN now use the same typed authenticated logical control/asset streams as Server. Discovery is the only mode choice: Server uses its rendezvous; Direct joins the forwarded creator; STUN first discovers the creator socket endpoint and then uses the Direct introduction path. There are no periodic server/STUN packets in a Direct/STUN jam. The creator's selected UDP listening socket is fixed. Its established A-B links share that socket, with receive routes selected by source endpoint. A physical socket has one reader; when the runtime owns it, bootstrap/control datagrams are handed to the worker and native audio bytes go straight to the native receiver. B-C links use dedicated sockets and failed pairs rotate them without disturbing healthy pairs.

The binary creator introduction envelope has a fixed 32-byte prefix: magic J2PB (4), current multiplex version (1), kind (1), body length (2, big endian), session (8), sender token (16). A 16-byte truncated HMAC-SHA256 authenticates the complete prefix/body with the jam key; the key itself is never sent. Each body starts with a 16-byte request nonce. Kind-specific fields use fixed shapes: introduction ID (8), attempt (1), partner token (16), endpoint IPv4+port (6), and boolean flags (1), only where needed. The largest accepted bootstrap datagram is 128 bytes. Unknown kinds, stale attempts, wrong session/version/tag, invalid endpoints and trailing bytes are rejected. Endpoint fields are literal IPv4, matching this implementation's existing IPv4 punching path; IPv6 introductions are not implemented.

Introductions repeat at 500 ms, use a 4-second phase attempt bound, and permit ten attempts with new sockets/IDs. Direct creator-join attempts use a 3-second attempt bound and ten attempts, within a 45-second overall bound. Pending unauthenticated creator joins are capped at eight; failed bootstrap authentication work is capped at 64 per 10-second window. These are resource bounds, not a session-size limit. An optional creator session limit still applies. Same-public-address pairs try private candidates on alternating attempts and public candidates on the others. Both native HELLO directions must be proved before activation. Completion is retried and acknowledged.

Release native checks passed: peer-name units (0.03 s), bootstrap codec/coordinator units (0.03 s), Direct four-peer transport (9.73 s), STUN four-peer transport (9.71 s). The latter tests authenticate duplicate names under distinct IDs, deliver unchanged native packets on all twelve directed audio paths, block initial B-C socket attempts, and then break/recover one established pair. The STUN fixture additionally asserts exactly one discovery request and an unchanged creator port. These are deterministic local socket/NAT-injection tests, not evidence that every real router permits punching.

All public names are mandatory and canonical before authentication. The name is included in the authentication transcript. Asset authentication reuses the already authenticated control binding instead of resending a name. Membership's per-channel name slot is a one-byte UTF8 length plus at most 32 bytes on first definition; a zero length subsequently reuses the binding. Ordinary actions and native audio carry IDs/handles, not names. Duplicate display names are deliberately allowed; stable IDs remain distinct. A different token whose derived native peer ID collides with an existing member is rejected.

### Common control stack and TCP retirement

The native four-peer session-controller workflow passed before TCP removal (2.11 s) and again after removal (2.08 s). It uses the production controller, transports and runtime sources and checks typed membership, duplicate-name bindings, acknowledged heartbeats, logical control-stream reauthentication and fresh membership, asset-channel binding, departure/rejoin, session end and cancellation. The real four-peer Direct GUI startup/action workflow passed in 62.04 s. Its initial failures were a Unicode test fixture written through a non-Unicode shell and missing update-dialog automation metadata; both were corrected without changing update behavior. The latter metadata has its own native updater test assertion.

NativeTcpTransport and the TCP connector/listener/reservation implementation have been deleted. ControlClient/ControlServer now authenticate only multiplex logical streams and dispatch only ControlWire records. The old authenticated JSON/large-JSON TCP codec, its counters and native TCP lifecycle diagnostic were retired. The new native controller lifecycle CTest replaces that TCP-specific validation scenario. Current control fuzzing uses the actual typed codec. The prior TCP performance reference is retained as measured artifacts, not an executable fallback in this branch.

One constant, ControlWireVersion 4, selects the current multiplex, creator introductions, control authentication and typed application records. This is a deliberate compatibility break for mandatory names and common discovery; previous control versions are rejected. Authentication no longer negotiates a separate Server-wire version field. Its transcript binds the common control version, session, both nonces, token, endpoint, logical channel and canonical name. Native audio remains protocol 2 and the invite URL container remains v1; neither is a fallback control stack. All participants in this branch must run the matching current control version.

The cold join challenge/proof remains bounded JSON inside an authenticated logical stream. Ordinary application records, memberships and WAV transfer metadata/chunks use the typed binary codec. This keeps one low-frequency authentication implementation while the recurring control traffic retains its compact format. Names are not resent on each edit or audio packet. A name binding is retained per authenticated channel; a newly authenticated channel must receive its initial membership definitions again. Duplicate names can consequently produce identical visible labels; that is the requested policy, and their peer IDs and GUI controls remain distinct.

The 1-second authority beacon, change/join/recovery-driven acknowledged grid assignments, typed conflict/application logic, ACK aggregation/selective recovery, independent bounded replies and efficient WAV transfer now apply in all discovery modes. --no-stun on a creator explicitly selects Direct discovery. A STUN session sends initial discovery on its actual listening socket and never depends on that service for established traffic.

Post-migration latency comparison passed in 64.11 s: Direct median 6.4 ms / p95 8.7 ms; Server median 6.5 ms / p95 8.0 ms. There were 192 correlated path observations per mode (two runs, eight samples, twelve paths), not 192 independent experiments. Server-minus-Direct was +0.1 ms median and -0.7 ms p95, within the unchanged one-packet/one-callback limits. The prior TCP Direct result was 6.6/8.1 ms. This does not establish a causal 0.2 ms improvement or hardware/WiFi performance. CPU in this run was materially higher for both modes (Direct 357.954%, Server 373.502% of one core across four processes) than the initial baseline; that discrepancy remains under investigation and is not being dismissed as unrelated.

After removal, native peer-name, bootstrap/coordinator, Direct four-peer, STUN four-peer, ordered/independent impaired control streams and typed-codec CTests passed. The separate replacement-asset fixture initially used a payload smaller than the minimum WAV document; the typed codec correctly rejected its transfer metadata. That fixture was updated to a valid-sized payload and passed its native Release rerun. Completion receipts lost for an introduction now cause another bounded fresh attempt, followed by an observable failure at ten attempts, rather than silently abandoning completion. A native virtual-time test covers that terminal path.

Remaining protocol limitations: creator introduction endpoints are IPv4; discovery resolution can block its worker during DNS, while STUN request/retry waits are asynchronous. Invalid bootstrap HMAC work is globally bounded, so a sufficiently busy invalid source can defer valid join processing until the next 10-second window. Possession of the shared jam key proves session membership, not a separately certified human identity; display names are labels, not authorization tokens. These are review points, not new identity/account or relay features.

### Common listening-control cleanup and deadline correction

The application no longer selects listening behavior with a Server-binary flag: every network session uses the same authoritative desired-state model. The old TCP prepare/all-peer-readiness/commit path was removed from the owner. Independent setting groups merge; conflicting assignments use authority receive order; local recording/content preparation can defer application without holding other peers. Native tests were updated to assert that common behavior rather than the retired all-peer barrier, while retaining four-peer duplicate/stale-order and clock-continuity checks.

That migration exposed a pre-existing scheduling defect in the typed model: treating every desired-state update as a join checkpoint could replace a still-future advertised target with an earlier local beat, even though its tempo anchor retained the original target. The owner now honors future targets exactly. Only an expired target moves to a future local beat, with the existing source-position catch-up. The four-peer native test checks that different raw engine ages map to the same advertised musical frame; this is a control scheduling fix, not an audio-packet or audio-callback change.

The public four-peer CLI recovery/count-in workflow passed in 45.97 s with the actual multiplex NAT fixture. It cuts an existing creator mapping, proves a fresh socket mapping appears, and then exercises shared transport scheduling. The prior fixture's native endpoint override no longer selected the multiplex routes. Its first replacement also pumped forwarding only between blocking automation reads, producing artificial 200-500 ms RTT; forwarding now runs independently at a 1 ms test timer.

Worker service counters are accumulated elapsed time inside a transport worker's processing section, excluding socket waits. The 18-25 ms figures were per-peer cumulative maxima near the end of approximately 14-second runs, not per-packet delay, all-peer totals, or exact OS CPU time. The isolated audio repeat passed (Direct median/p95 5.6/8.0 ms, Server 6.6/8.1 ms; 192 observations each); CPU remained high in the GUI benchmark (350.210/355.498% of one core across four applications). Worker service totals stayed small, and a thread sample showed GUI-thread work dominating that sampled process. The exact GUI cost has not yet been isolated. Raw snapshots/CSV copies are retained under artifacts/unified-udp-multiplex/no-tcp-audio-raw.

Name dictionaries share the existing 4096-entry per-channel identity cache bound. After cache exhaustion, literal identities/names remain representable on memberships; this is not an application-wide peer cap. Operational compact streams must confirm the same nonzero handle; silent full-framing downgrade is rejected. Full-frame codec ablations are current-version diagnostic tests, not compatibility with an older session protocol.

The unused listening prepare/ready/commit wire operations have also been retired; there are now 48 named typed operations plus the three special asset/receipt/binary operations. Their old opcode numbers are not reassigned to a different meaning. Native fixtures now cover the current common domain and reject retired operations rather than preserving a second network behavior model.

The existing send/receive socket-buffer options now configure the multiplex-owned bootstrap and every fresh peer socket. Previously the runtime applied them to its separate native socket, which was not the socket carrying multiplex traffic. Defaults remain OS defaults; there is no hidden buffer-size increase. Requested sizes, actual per-link and bootstrap OS sizes, and the count of multiplex-owned physical sockets are exposed in diagnostics. The four-peer Direct/STUN fixture requests 32768/65536 bytes, checks the OS readback, verifies the creator shares one listening socket, and checks that rotated sockets retain tuning. Zero/negative explicit buffer requests are rejected. OS readback can differ from the requested size.

### Current native and GUI validation, socket ownership cleanup

The socket-buffer configuration now passed the real four-peer Direct test: the creator owns one physical multiplex socket and each joiner owns three, with requested send/receive sizes retained on the punched links. The focused nine-test Release run also passed names, bootstrap, STUN four-peer, controller lifecycle, control transport, typed codec, multiplex core, listening tools and asset lifecycle. Direct four-peer GUI listening and WAV sharing passed; the same listening workflow passed with external rendezvous.

Removed the ineffective old debug topology/endpoint overrides rather than preserving an option which the multiplex router would overwrite. The metronome impairment fixture now applies its existing deterministic impairment scheduler to packets crossing the actual dynamically punched NAT mappings, preserving the exact physical source/destination endpoints. Bootstrap and typed streams retain their own dedicated fault tests; this metronome fixture continues to impair native runtime traffic as before. No production audio packet, callback or pacing change is involved.

NetworkSession can now own either a native socket (retained for useful low-level diagnostics) or a routed datagram transport. Public multiplex sessions construct the latter directly, so the runtime no longer creates an unused extra OS socket. Receiver attachment remains transactional; constructor rejection, replacement, move and cleanup are covered by the native multiplex CTest. Startup and CSV socket diagnostics use the actual transport endpoint and OS buffer sizes. This is resource ownership and diagnostic correctness, not an audio queue or timing adjustment. The native multiplex ownership tests and four-peer metronome clean/loss/reordering/burst-loss workflows passed with this ownership model.

The common bank/recording owners no longer expose an all-peer barrier policy left over from the TCP mode: locally prepared bank changes can proceed and only ready armed peers enter a recording group. Readiness and application receipts remain observable; delayed peers recover or reschedule according to the shared desired-state rules. Existing real four-peer GUI workflows cover the interaction, and the bank-owner native checks cover missing, duplicate, stale and departed peer receipts.

The direct creator also reaps failed unadmitted bootstrap links, instead of keeping failed map entries indefinitely. Active physical links which never complete membership admission expire after an explicit 30,000 ms default (Config.pendingAdmissionTimeoutMs, bounded 1-60,000 ms for diagnostic injection). The limit and expiry count are exposed in multiplex diagnostics. This is separate from the logical stream authentication deadline of 20 seconds; it bounds clients which prove the socket but never open/authenticate control. The four-peer Direct/STUN fixture injects 32 valid-MAC unproved joins, checks the pending cap of eight and cleanup, and exercises the active-but-unadmitted expiry with a 1,000 ms configured deadline before retrying the real join. Admitted members and healthy links are excluded from this cleanup. Both the Direct and STUN four-peer Release fixtures passed (14.16 s and 14.14 s).

The first pending-cap fixture used tokens whose leading 64 bits were zero. Those are correctly rejected before allocating a link, so the assertion expecting eight pending links failed. The probe now uses distinct nonzero stable peer IDs; this was a fixture correction, not a change to the nonzero-ID requirement.

### Final measurements and verification notes

The final unchanged-budget ABBA audio comparison passed in 64.66 s after direct routed-transport construction: Direct median/p95 6.7/9.3 ms, Server 6.7/9.0 ms. Each mode has 192 directed-path observations: two runs, eight time samples per run, twelve directed paths across exactly four peers. These are correlated samples, not 192 independent experiments. Server minus Direct was 0.0 ms median and -0.3 ms p95. The earlier TCP baseline was Direct 6.6/8.1 ms and Server 6.4/8.3 ms; run-to-run scheduling/buffer occupancy still varies, so these data establish the existing comparison budget rather than a sample-exact physical latency guarantee. Audio bytes, full-mesh fan-out, callback/frame sizes and pacing remain unchanged. The displayed totals are production GUI estimates reconstructed from RTT/2, jitter/mixer queue frames, playback ring frames and output callback latency, not acoustic loopback measurements.

Final aggregate four-process CPU was 335.855% (Direct) and 341.383% (Server), compared with the earlier 153.62%/147.868% baseline. This higher GUI benchmark CPU is repeatable in the current runs. Its exact cause is unresolved: small worker service counters do not explain or rule out time elsewhere in the runtime, GUI, graphics, OS or instrumentation. No CPU improvement or equivalence is claimed. Raw final snapshots, CSVs and logs are retained in artifacts/unified-udp-multiplex/final-owned-socket-audio-raw, with the corresponding CTest log alongside it. The earlier raw observations remain available for comparison.

Worker service is specifically measured after the worker acquires its main mutex and before its socket wait. It includes command/ingress processing, socket work owned by that worker, timers, read-route publication and preparation of its wait set. It excludes the initial mutex wait and waitForAny, and excludes work performed by the separate native runtime receiver. It can include scheduler preemption during the measured section; it is not an OS CPU counter. The quoted approximately 18/25 ms totals were the largest per-peer cumulative counters observed near the end of approximately 14-second Direct/Server runs, not per-packet delays or whole-application service time.

The current focused native owner run passed the codec/performance, name, bootstrap, runtime/controller lifecycle, routed receive, listening, recording, bank, workspace, GUI model/widget, preferences, asset lifecycle and fuzz/help checks. Two harness executables initially had not been built in the selected network/GUI target; the normal Release unit build now includes them. The public CLI name checks passed (1.68 s), covering absent/empty, digit, oversized, whitespace and markup input before transport/audio startup across Direct, STUN and Server. The broad native diagnostic boundary test initially rejected its old valid-membership fixture because that fixture omitted the newly mandatory name. It now supplies the same valid duplicate name on all 64 entries; production validation remains strict. Its successful rerun and the subsequent recovery/interruption results are recorded below.

### Follow-up from final interruption/recovery checks

Direct/STUN admission, the port/coordinator harness, public four-peer CLI recovery (45.88 s), and native boundaries (95.01 s) passed. The WAV interruption workflow found an actual state-composition failure: after cancelling an unpublished WAV, all four models removed its bytes but restored the lane's earlier default name. Pending peer proposals were merged only against the applied authority snapshot, which could not express cancellation back to that same applied empty-WAV value. Staging now retains each source's latest proposed snapshot per scope within the current authority revision/validation chain. A later edit from that source uses that pending snapshot as its merge base, so cancellation is an observable change while other peers' independent edits remain composed. The cache is discarded on revision/generation changes or rejected validation; it is limited to current pending proposers/scopes, not a permanent history. Native checks exercise cancellation, another peer's change to another lane, generation/revision boundaries and rejected staging; the existing exact four-peer interruption workflow retains its lane-name assertion.

The grid idle check also failed after a logical reconnect. Retained snapshots show that the production assignment counter and applied-peer state stayed unchanged throughout that three-second window, while the fixture also checked translator-level assignment/ACK counters. This is consistent with an in-flight duplicate receipt rather than a new periodic producer transmission; the original assertion did not print the individual counter difference, so that attribution is not yet confirmed. The fixture now waits for 200 ms of observed quiescence (bounded by five seconds) before the unchanged strict three-second zero-traffic window, and reports every counter in a failure. No production retry or beacon interval was changed.

The detailed grid rerun disproved the in-flight-traffic hypothesis: translated assignments, ACKs and the producer counter were all unchanged (4/4, 4/4, 23/23), with three peers acknowledged and zero pending. The failed boolean was clock readiness: one fake-audio peer's instantaneous mapping error was 140 frames, beyond the existing 128-frame readiness tolerance, while its applied revision was current. The temporary settling phase has been removed. The idle check now explicitly verifies current revisions, all three application receipts, no pending recovery and strictly zero new assignment/ACK/producer packets for three seconds. Clock readiness is checked separately with its original tolerance and bounded recovery requirement. No clock tolerance, audio timing or production beacon/retry policy was relaxed. This records a fake multi-process scheduling limitation instead of reporting it as ACK traffic.

The complete four-peer WAV interruption regression now passed in 38.78 s; the acknowledged-grid workflow passed in 37.46 s with separate clock readiness recovery and unchanged idle traffic assertions. The pending proposal cache stores only Track proposals (one per current source), since generated-Idea scopes are atomic replacements and do not need this extra base.

Cleanup also removed the legacy KeepCurrent conflict selector and all mode/engine-running checks which selected between old and current reconciliation. All network Track edits now share the deterministic later-received conflict rule, atomic WAV/region groups, independent-field merging and deletion/replacement handling. Generated-Idea scopes share atomic later-authority receive order; invalid structure still rejects. Native model fixtures retain these current rules and remove expectations for the retired behavior.

Validated peer names are retained by full stable token for the lifetime of the jam, including after departure, so delayed completion/disconnect messages can still name the peer. Each binding also records its numeric ID; current GUI ID lookups still prefer active members. Starting another jam clears the directory, and a rejoin can update the binding. This stores one small validated name per distinct admitted identity seen in that jam, with no application-wide peer cap; it is not an account or permanent history. Human-facing peer log labels now use names; structured diagnostics retain raw IDs/tokens for debugging. Native four-controller lifecycle coverage checks departure lookup and reset boundaries.

The last model cleanup exposed one unit expectation for the retired accepted-first list ordering. The current rule retains distinct concurrent imports, uses the later proposed order, and appends retained earlier additions deterministically. The native expectation now checks that rule; the production rule was already used in multiplex mode. The current-only model CTest passed in 0.04 s. Asset progress/interruption log labels now resolve names through the owning context as well, with a native service assertion that a re-request identifies Boris rather than its internal peer token.

### Completion audit for the unified transport change

The final code remains on `UDP-Multiplex`; the main branch was not changed. No TCP jam-control implementation, old multiplex decoder, legacy reconciliation selector, old endpoint-override path or obsolete controller-lifecycle diagnostic operation remains. Useful raw native-socket diagnostics and explicit same-version performance ablations remain; they are not alternate public network protocols. Invitations retain their URL container syntax, while all peers must use the current wire/schema implementation.

| Requirement | Current implementation / authoritative evidence |
| --- | --- |
| One current transport and typed control behavior in all public modes | `SharedSessionController` always creates `UdpMultiplexTransport`; Direct/STUN native exactly-four-peer workflows and Server four-peer GUI workflows passed. The current-only codec covers 48 named operations plus asset chunk/receipt/binary records; native codec and performance CTests passed. |
| Direct/STUN reach the forwarded creator, with no external rendezvous connection | Direct binary bootstrap and `PeerIntroductionCoordinator`; Direct/STUN four-peer native tests use no Go service, assert no rendezvous use, and verify zero STUN requests in Direct / one creator discovery request in STUN. |
| Creator introductions, fresh failed sockets and healthy-edge preservation | Direct/STUN tests block the first B-C attempt and a later established pair, check fresh ports, preserve healthy ports and the creator port, and deliver all twelve native paths. The public four-process CLI workflow also cuts an established creator mapping and passes fresh-mapping recovery. |
| Lightweight, authenticated and bounded admission | Fixed/bounded bootstrap parser, session/source/attempt binding and MAC checks; 32 valid-MAC unproved joins exercise the eight-pending cap, failure cleanup and active unadmitted expiry. Bad-key work is rate bounded. These limits do not cap admitted mesh size. |
| Mandatory names mapped to stable IDs | `PeerName` validates canonical NFC, 1-8 letter scalars and at most 32 UTF-8 bytes. Native name/codec/controller tests, real CLI invalid-input checks in all three modes, actual GUI create/join validation and four distinct IDs with duplicate Alex names passed. Full-token directory lookup survives departure and clears for a new jam. |
| Names visible in GUI peer controls, popups and transfer logs | Common controller lookups feed performance/mixer/RTT/incoming-delay/recording and connection labels. GUI model/widget and real session-dialog workflows passed. Native asset service checks verify name-based log labels. Raw structured IDs remain for debugging. |
| Preserve audio format/receive path/pacing and WAV star topology | Native routing tests assert unchanged audio bytes/fan-out, four-peer metronome clean/loss/reorder/burst tests passed, and unchanged-budget ABBA latency comparison passed. Shared assets still traverse separate reliable creator/joiner logical channels; WAV exact-byte, interruption and sharing-race workflows passed. No audio DSP/callback or payload format was changed. |
| Common deterministic state behavior and reliable application acknowledgement | Current-only model tests cover independent edits, later conflicts, atomic regions/content, deletion/replacement and pending cancellation. Grid workflow checks dropped assignments/receipts, logical reconnect, recording isolation, rejoin and strict zero idle assignment/ACK packets. Clock readiness retains its separate original tolerance. |
| TCP baseline and reviewable limitations | Commit 231bed6 and its native Release TCP comparison logs are retained in `artifacts/unified-udp-multiplex`. Baseline, final latency, correlated sample counts, higher unresolved GUI CPU, IPv4/NAT/shared-key limitations, wire fields, bounds and choices are documented in this file. |

The last Release native batch passed all seven selected checks: Track workspace (0.14 s), asset transfer (0.17 s), Direct four-peer shared content (46.92 s), four-peer WAV interruption (38.11 s), actual four-peer name/session dialogs (62.52 s), Server four-peer shared content (83.55 s), and Server sharing races (9.36 s). The final named four-controller lifecycle passed in 2.10 s and current-only merge model in 0.04 s. Their logs are `final-common-content-names-test.log`, `final-named-controller-lifecycle-test.log` and `final-common-merge-name-build-test.log` under the retained artifact folder.

Public examples now include mandatory names and current UDP reachability rules. The debug scenario schema matches the supported operations, requires `network.name` for create/join, documents native Unicode validation, and removes the retired topology override and controller-lifecycle scenario. Earlier design documents are marked as historical where their Server-only rollout / TCP-retention assumptions have been superseded.

Validation used the normal MSVC optimized Release build and `release/jam2.exe`, with directly relevant native CTests only. No full-suite or source-coverage run was requested or performed. These fake-audio/loopback and deterministic network impairment results do not certify every real router, operating system or audio device. In particular, no improvement/equivalence in whole-GUI CPU is claimed; the higher measured CPU remains an explicit review item rather than being attributed to an unverified cause.

### CPU recheck requested after the earlier gaming workload

The user reported playing games during the earlier measurements and requested a fresh run. No product code or benchmark settings were changed for this recheck. The current Release executable was tested with the same native CTest and four-run ABBA order (two runs per mode, four GUI peers per run, eight time samples and 192 correlated directed-path observations per mode). It passed in 62.49 s.

| Mode | Original baseline CPU | Earlier high run CPU | Current recheck CPU | Current GUI latency median / p95 |
| --- | ---: | ---: | ---: | ---: |
| Direct | 153.620% | 335.855% | 156.319% | 6.6 / 8.2 ms |
| Jam2 Server | 147.868% | 341.383% | 165.085% | 6.5 / 8.0 ms |

CPU values sum the four Jam2 processes' OS CPU time, expressed as a percentage of one logical core. They do not directly include the game's CPU time. Direct's individual current runs were 159.124% and 153.522%; Server's were 171.879% and 158.265%. The current weighted aggregates are approximately 1.8% and 11.6% above the original baselines respectively, rather than the earlier 119% / 131% increases. The large doubling did not reproduce under the current conditions. This supports environmental contention as a possibility; it does not establish that the game caused it, since no controlled same-binary gaming-on/off comparison was performed. The earlier high run also preceded the last metadata/name-label cleanup, so attributing the difference solely to either code or environment would be premature.

Concurrent Windows total-processor sampling recorded 65 samples across 32 logical processors: mean host CPU 9.763%, maximum 26.116%. This is whole-machine CPU utilization and is a different scale from the per-core Jam2 aggregate. It shows no whole-machine CPU saturation during this recheck; GPU load and individual-core contention were not measured. The audio comparison remained inside its unchanged budget (Server minus Direct: -0.1 ms median, -0.2 ms p95).

The CTest log, total-host samples, persistent-background-process CPU deltas, tested binary SHA-256 and raw per-peer logs/CSVs/snapshots are retained as `user-cpu-recheck-*` under `artifacts/unified-udp-multiplex`. This supersedes treating the earlier doubling as an established application CPU regression; a smaller Server-versus-original-baseline difference remains measured, with cause unassigned.
