# UDP Multiplex Wire Format: Discussion Draft

> Current branch status: Direct, STUN and Jam2 Server share the current typed UDP multiplex. TCP and older multiplex fallback paths have been removed, and create/join requires a validated peer name. Earlier rollout restrictions and TCP-retention choices below are historical. See [REVIEW-WIRE-FORMAT.md](REVIEW-WIRE-FORMAT.md#unified-multiplex-transport-and-mandatory-peer-names) for the implemented behavior, limits and native test evidence.

Date: 2026-10-25  
Repository/branch: Jam2, `UDP-Multiplex`  
Status: design notes for the next session; no wire-format or topology implementation authorised by this document.

## Purpose

Design a compact, typed control protocol for Jam2's multiplexed UDP connections.
Reduce JSON, repeated metadata, parsing work and unnecessary ordering dependencies.
Define the meaning of each action alongside its bytes: who may issue it, who
decides it, what confirms success, and how conflicts, duplicates and recovery work.

The user's immediate interest is the wire format and operation contracts.
A completely distributed control mesh remains an architectural option to discuss,
rather than a prerequisite for this work.

Audio processing and the existing audio packet design must remain protected.
Control and assets continue to share the already punched physical socket for each
peer pair. Separate logical message classes do not require additional hole punches.

Related context: [UDP-Multiplex-Implementation.md](UDP-Multiplex-Implementation.md)
and the original [UDP-Muliplex.md](UDP-Muliplex.md).

## Current implementation baseline

The present implementation adapts the existing authenticated control/asset
protocol to ordered reliable UDP streams:

- Multiplex packets have a 42-byte prefix and 16-byte authentication tag:
  58 bytes before their payload and UDP/IP headers.
- Reliable fragments currently carry at most 1,024 payload bytes.
- Control and assets use separate logical channels.
- Their payloads retain the existing authenticated framing; control messages
  usually retain JSON, with compression/chunking for large JSON messages.
- Reliability currently provides ordered bytes within each logical stream.
  Audio does not wait for these acknowledgements.
- Asset transfer also has application chunk/window and final validation state.

Source owners:

- [UdpStreamConnection.hpp](app/application/UdpStreamConnection.hpp)
- [UdpStreamConnection.cpp](app/application/UdpStreamConnection.cpp)
- [reliable_udp_stream.hpp](libs/jam2-core/include/jam2/reliable_udp_stream.hpp)
- [ControlProtocol.hpp](app/application/ControlProtocol.hpp)
- [ControlMessageValidation.cpp](app/application/ControlMessageValidation.cpp)
- [GuiControlMessageRouter.cpp](app/gui/GuiControlMessageRouter.cpp)

These are observations about the current implementation, not target sizes or
requirements to preserve every existing layer.

## Keep transport, authority and asset distribution separate

There are three independent decisions:

1. **Delivery topology:** which established peer links carry messages.
2. **Decision authority:** who accepts edits and establishes their revisions.
3. **Asset distribution:** which holders supply WAV bytes to which receivers.

A full mesh of physical connections can support direct asset transfers while
the creator remains the authority for shared arrangement and timing decisions.
It is not necessary to decentralise all decisions to remove creator-mediated
WAV forwarding.

For six peers and a 20 MB WAV created by B:

- Star delivery: B uploads 20 MB to creator A; A uploads 80 MB to four others.
- Direct delivery from B: B uploads 100 MB to the other five peers.
- Both move approximately 100 MB of application payload, excluding overhead.
  Direct fan-out moves the upload bottleneck to B.
- If A created the WAV, direct fan-out still leaves A uploading five copies.
- Multi-source chunk distribution could spread that upload work among holders.

Consequently, mesh delivery is not automatically lower total traffic or better
for a recording peer with a weak uplink. A future chunk distributor could choose
sources, resume ranges and verify completed content by hash.

Creator departure would also require an explicit membership, authority and
timing handover policy. Mesh transport alone does not provide that behaviour.

## Typed actions rather than GUI state dumps

Ordinary changes should describe an operation, for example:

- `RenameTrack`
- `SetClipRegion`
- `AddTrack`
- `RemoveTrack`
- `SetMusicalStep`
- `PrepareRecording`
- `CommitRecordingStart`
- `AnnounceAsset`

These names are illustrative. Numeric type assignments and exact schemas have
not been selected.

Use operation deltas for ordinary edits. Reserve full snapshots/checkpoints
primarily for joining, recovery and convergence checks. Replacing a whole
arrangement merely to change one field can cost much more than the packet header.

Explicitly classify shared versus local state. Device settings, buffer choices
and personal monitor controls should remain local where that is their intended
meaning. A protocol should not broadcast every GUI change indiscriminately.

Keep commands typed inside the application as well as on the wire. Converting
binary messages straight back into generic JSON maps throughout the handlers
would retain much of the parsing/allocation and validation ambiguity.

## Compact encoding proposal

Prefer explicit binary layouts for small, common operations:

- Numeric operation types and field identifiers.
- Short wire handles mapped to stable project/object identities.
- Binary hashes: SHA-256 is 32 bytes, rather than 64 hexadecimal characters.
- Integer timeline frame positions.
- Explicit numeric units, bounds and precision for parameter values.
- Bounded, length-prefixed UTF-8 strings for names, lyrics and other text.
- Flags or presence masks for genuinely optional information.

Specify byte order, signedness, valid ranges and length limits. Never transmit
the raw memory layout of a C++ struct: padding and platform representations are
not the protocol.

A short handle is a wire optimisation, not a replacement for stable identity.
Handle mappings must be available during joining/recovery and must not be reused
while delayed operations could still refer to an earlier binding.

### Illustrative MoveClip proposal

For a direct proposal, the authenticated link can imply sender and session:

| Field | Suggested representation | Bytes |
|---|---|---:|
| Type, flags, body length | u8 + u8 + u16 | 4 |
| Operation counter | u64 | 8 |
| Clip wire handle | u32 | 4 |
| Expected clip revision | u64 | 8 |
| New start frame | u64 | 8 |
| **Operation record** | | **32** |

This is not a complete datagram format. Authentication, packet numbers,
transport headers, optional ACK data and UDP/IP headers are additional.
Forwarded operations must preserve their original issuer; a commit may carry
an accepted revision and other result information.

The example illustrates the potential size of a specific operation, not a
benchmark or a final choice of widths.

### Packet envelope and evolution

Review which identifiers belong once in authenticated connection setup, once
per packet, or in a particular operation. Avoid repeating session, sender,
object and revision fields where established context already provides them.

An envelope will still need the appropriate link binding, packet identity,
message boundaries and authentication/replay guarantees. Removing nested legacy
framing must preserve those guarantees explicitly.

Agree protocol versions/capabilities before exchanging the new encoding.
Define handling for unknown optional extensions and unsupported mandatory
operations. Do not silently interpret an unknown operation as another kind.

Keep packets bounded and account for the complete envelope when selecting
payload limits. Avoid IP fragmentation and define bounded application
fragmentation/reassembly where large messages need it.

For larger structured snapshots, compare a typed binary snapshot format with
numeric-key CBOR. CBOR is standardised and compact, but still needs a strict
Jam2 schema. Merely changing generic JSON maps into generic binary maps does
not establish typed operation semantics.

Human-readable decoded logs can remain JSON or text independently of the wire.

## Three different confirmations

### 1. Transport receipt

The authenticated packet or reliable fragment arrived and entered transport
handling. This supports loss detection, retransmission and congestion control.

It does not mean that a GUI operation was accepted, a WAV validated, or a
scheduled action executed.

Consider ACK ranges or a small bitmap to acknowledge multiple packets together.
ACKs can share outgoing control packets where appropriate, with a bounded
standalone ACK delay when there is no return traffic.

### 2. Operation result

The decision owner accepted or rejected the proposed operation.

An authoritative `Commit` can serve both as the response to the proposer and
as the update distributed to other peers. Avoid adding a redundant separate
application acknowledgement when the commit already proves acceptance.

Rejections should be typed, for example stale revision, invalid region, missing
object or wrong authority. Include enough current revision/value information
for the sender to reconcile its optimistic UI or request recovery.

### 3. Readiness or application status

A peer has completed a meaningful application stage, such as:

- Validated the complete WAV against its hash and format constraints.
- Prepared the corresponding local mix.
- Armed a recording participant.
- Applied a scheduled transport or recording action.

Only request this status when the operation needs it. Routine edits should not
require every peer to acknowledge multiple application phases.

## Initial action catalogue

| Family | Delivery semantics | Confirmation | Initial decision owner |
|---|---|---|---|
| Shared parameter/drag preview | Latest value supersedes older previews; coalesce queued values | Usually none | Peer assigned that interaction |
| Committed track or clip edit | Reliable, deduplicated; preserve relevant dependencies | Commit or typed rejection | Arrangement authority, initially creator |
| Track addition/removal | Reliable and deduplicated | Commit or typed rejection | Arrangement authority |
| Musical cell/step edit | Reliable; order conflicting edits to the same target | Commit or typed rejection | Shared-content authority |
| Membership or ownership change | Reliable and ordered | Authoritative membership revision | Creator/coordinator |
| Playback, bank switch, group recording | Reliable and deadline-aware | Ready, commit/cancel, application status where required | Timing coordinator |
| WAV announcement | Reliable metadata, independent of file completion | Accepted arrangement operation | Arrangement authority |
| WAV content | Reliable resumable chunks/ranges, separately paced | Received ranges and final verification result | Serving peer and receiving peer |
| Recovery/checkpoint exchange | Reliable, bounded, explicit revision coverage | Checkpoint acceptance/missing-state response | Relevant state authority |

These are proposed contracts to refine, not descriptions of already implemented
binary messages. Start from the existing validators/router to inventory all
current action types, including listening state, recording recovery and session
repair messages.

Parameter preview is only appropriate for shared parameters. Local movement can
remain immediate, with coalesced remote previews and a reliable final value.
An obsolete preview should not be retransmitted after the final commit.

## Ownership and conflict policy

Initially retain one decision owner for each object or operation domain.
The creator can remain the shared arrangement authority even when messages or
files travel directly between peers.

Later, tracks could have assigned owners. Ownership changes must themselves
follow a committed membership/authority rule; peers cannot independently claim
ownership and expect their decisions to converge.

A normal edit proposal contains:

> operation identity + target identity + expected revision + requested change

The owner validates it against current state and returns a commit or rejection.

Proposed rules to specify and test:

- **Different objects:** preserve independent changes.
- **Different independent fields:** merge when those fields have not changed
  since the proposal's base revision and the resulting state remains valid.
- **Same field:** start with an explicit policy such as accept one operation
  and reject the stale competing proposal. The owner's committed result is
  what all receivers apply.
- **Related fields:** move/trim/loop boundaries form an atomic region change;
  independently merging numbers must not manufacture an invalid clip.
- **Delete versus metadata edit:** removal of the same WAV content defeats a
  delayed rename or placement edit of that content.
- **Delete versus content replacement:** distinguish replacement bytes/content
  revision from a metadata edit. An old removal must not silently delete a new
  recording that reused the track.
- **Late asset completion:** fulfils an existing asset reference; never creates
  a removed track by itself.

Consider revisions for logical field groups, or owner-side last-change
revisions for those groups. Rejecting every operation against a stale global
arrangement revision would unnecessarily conflict independent edits.

The recently repaired rename/delete race is a concrete regression example:
a rename could be accepted while the sender still edited from the preceding
revision, causing its later removal to conflict and restore the WAV. An
accompanying Idea clear could also be rejected despite only track metadata
having changed.

If all peers eventually become equal decision makers, the protocol needs
deterministic conflict rules independent of arrival order, operation identities,
causal/dependency information and deletion retention. Logical counters plus a
stable peer tie-break can order concurrent updates without depending on wall
clocks. Convergence alone does not define the musically appropriate result.

## Duplicate handling and recovery

A retry reuses the original operation identity. If the result was lost, the
owner returns the recorded result rather than applying the action again.

Operation identities can use the authenticated peer identity plus a monotonic
operation counter. Define counter lifetime, non-reuse, bounds and reconnect
behaviour; do not assume packet numbers alone identify application operations.

Bound result/history retention. Checkpoints need enough revision and
deduplication coverage to recover safely after old history is discarded.
An unknown old operation must not blindly become a new action because its
result record was pruned.

Recovering or newly joining peers need:

- Stable object/handle mappings and membership/ownership state.
- Accepted object revisions and a checkpoint or missing operations.
- Deletion information sufficient to prevent resurrection by stale peers.
- Asset manifests and local availability information.
- Clear behaviour when recovery history is no longer available.

Retain the intended invitation lifecycle: leaving/restarting a jam in the same
application session can reuse the existing session ID and key. This discussion
does not propose a new per-restart runtime identifier.

## Timed actions

Playback, bank switching and group recording need more than eventual agreement.

Specify target position on the agreed session timeline, participating peers,
readiness deadline, accepted revision and late-arrival handling. Define whether
a missing participant causes cancellation, rescheduling or an explicitly
permitted partial group.

A delayed recording-start retransmission must not execute immediately after
its intended start time. Define what happens when a commit was issued but a
participant receives it late or loses connectivity.

Transport receipt, ready-to-start and actual application are separate states.
Do not describe this as guaranteed simultaneous execution across a partition.

## Scheduling and ordering

Keep audio deadline handling separate from reliable control and bulk queues.

Proposed scheduling goals:

- Protect audio deadlines.
- Reserve a small, bounded budget for urgent control.
- Send ordinary reliable edits without waiting behind unrelated bulk work.
- Use remaining capacity for WAV transfers, with congestion-aware pacing.
- Account for aggregate upload across all peer connections, not only each
  connection independently.

Separate logical channels still share CPU, socket queues and physical network
capacity. A mesh does not remove Wi-Fi airtime or uplink limits.

Consider independently reliable messages or dependency groups rather than one
globally ordered control byte stream. Membership changes require ordering;
independent track operations often do not. A missing parent/object creation
still needs explicit dependency handling, bounded buffering or recovery.

Batch small already-queued operations where useful without waiting to fill a
packet. Sharing a socket does not require putting control inside audio packets
or delaying audio to assemble a control batch.

Review transport ACKs, asset chunk windows and application completion results
by their actual purpose. Remove redundancy only where equivalent backpressure,
receipt or validation semantics remain.

## Migration and validation

Keep the command model independent of encoding and topology.

A possible migration is:

1. Inventory existing messages and define their typed command/result contracts.
2. Establish the operation catalogue, ownership rules and conflict tests.
3. Design binary encoding and version/capability negotiation for Server mode.
4. Retain a JSON/TCP adapter for existing STUN/Direct wire compatibility.
5. Measure against the existing path before changing topology or scheduling.
6. Explore direct/multi-source WAV delivery and distributed ownership as
   separate experiments.

No change to existing TCP flows is required merely to prototype a binary
Server-mode encoding. Both adapters should use the same typed validation and
application semantics.

Future implementation needs native tests for encoding/decoding, invalid lengths
and values, byte order, unsupported versions, duplicates, lost results, stale
proposals, conflicting region edits, delete versus late asset completion,
counter/handle lifetime, recovery checkpoints and deadline handling.

Retain exactly-four-peer workflows with deterministic fake audio and injected
loss, duplication, reordering and interrupted transfers. Include concurrent
edits, a slow peer, reconnect and late join. Verify converged shared state,
retained recordings/files and continued audio, not only successful decoding.

Measure bytes and packets per action, parsing/allocation cost, control queue
delay, proposal-to-commit latency, scheduled-action lateness, ACK/retransmission
overhead and per-peer/aggregate asset throughput alongside audio timing.

## Decisions to pick up next session

- Which operations and fields are shared versus local?
- Retain creator authority initially, or assign any per-track owners now?
- Exact operation/result catalogue and atomic field groups.
- Expected-revision conflict policy for each operation.
- Packet receipt versus commit versus readiness requirements.
- Fixed layouts, string bounds, handle allocation and snapshot encoding.
- Packet numbering, ACK representation, replay and deduplication lifetimes.
- Dependency/ordering scopes and bounded recovery.
- Timed-action late/partial-group policies.
- Version negotiation and the boundary between new UDP and existing TCP formats.
- Which measurements define a worthwhile improvement before implementation?

The immediate design artifact should be an operation catalogue where every action
declares **payload, issuer, decision owner, ordering, priority, expiry,
acknowledgement, conflict rule and recovery behaviour**.

## References

- [QUIC ACK frame design, RFC 9000 section 19.3](https://www.rfc-editor.org/rfc/rfc9000.html#section-19.3)
- [QUIC stream delivery, RFC 9000 section 2.2](https://www.rfc-editor.org/rfc/rfc9000.html#section-2.2)
- [Transport acknowledgement versus application processing, RFC 9221 section 5.2](https://www.rfc-editor.org/rfc/rfc9221.html#section-5.2)
- [UDP usage and congestion guidance, RFC 8085](https://www.rfc-editor.org/rfc/rfc8085.html)
- [CBOR encoding, RFC 8949](https://www.rfc-editor.org/rfc/rfc8949.html)
- [Local-first software and application conflict semantics](https://www.inkandswitch.com/essay/local-first/)
