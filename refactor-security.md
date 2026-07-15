# Jam2 v1 Lightweight Network Security Review

## Purpose

This report reviews Jam2's network-exposed control, UDP audio, project, asset-transfer, and WAV-processing paths as they exist near the v1 functionality target. It records the risks found, the smallest useful mitigations, their expected cost, and where they belong in the broader refactor.

The goal is not to turn Jam2 into a hosted platform. The target remains a
small, direct, short-lived session between controlled peers, with two people as
the primary case and three/four-person direct mesh as the expected extension.
Larger meshes remain experimental and have no application-wide peer cap; a jam
creator may optionally configure a limit for that session.

No code was changed and no fuzzing, sanitizer run, build, or live penetration test was performed for this review. The findings are from static source inspection. No confirmed remote-code-execution path was found, but static inspection alone cannot prove that malformed network data or WAV input is memory-safe.

The detailed findings describe the original review baseline. They remain as
threat and design context, not as current implementation status or a separate
phase/gate system. Current status and remaining work live only in
[refactor-plan.md](refactor-plan.md).

## Governing Constraints

The security work must preserve the repository's product rules:

- No rooms, relay/TURN audio path, accounts, certificate-management UI, or broad security platform.
- No allocation, locks, logging, exceptions, file work, or blocking operations in the real-time audio callback.
- Keep fixed, bounded, inspectable protocol shapes.
- Prefer a small local implementation over a dependency unless the dependency removes substantial correctness risk.
- Expose hard counters for rejected traffic, capacity, timeouts, replay, and endpoint state.
- Inspect packet-loop and callback timing when a fast-path change could affect
  them; measurements are engineering evidence rather than hard acceptance gates.

Security is treated here primarily as containment:

1. A malformed or hostile packet must not create unbounded memory, CPU, disk, file-descriptor, or queue work.
2. Data must not reach application handlers until its connection and source are authenticated and authorized for that message type.
3. Remote values must not become allocation sizes, loop bounds, local paths, or timeline positions without explicit limits.
4. A peer-advertised endpoint must not receive continuous audio until it proves it owns the observed UDP source.

## Threat Boundary

### In scope

- An unauthenticated host reaches a TCP or UDP port while a jam is active.
- A scanner or buggy client opens connections or sends fragmented, oversized, malformed, duplicated, reordered, or replayed data.
- A user connects to a malicious host or an on-path party modifies unencrypted TCP control traffic.
- An authenticated invited peer sends messages outside its intended role.
- An authenticated or buggy peer sends extreme sequence numbers, timestamps, grid sizes, project values, asset sizes, or WAV structures.
- A valid peer advertises a third party's UDP endpoint.
- A corrupt transferred WAV reaches the parser and prepared-mix worker.

### Deliberately limited for v1

- Peers who receive the invite are generally trusted not to extract and redistribute the shared session key.
- The application does not attempt account-level identity, revocation, bans, or persistence across sessions.
- The application does not attempt traffic-flow privacy or anonymity.
- Direct connectivity remains mandatory; restrictive NAT failure is reported rather than relayed.
- Local command lines, logs, clipboard contents, scenarios, benchmark state,
  and artifacts are outside the application threat boundary and may contain
  session keys or invite URLs.

The short socket-exposure window reduces likelihood, but it is not a reliable control by itself. Automated scans can hit a newly opened port immediately, and the same limits are valuable against accidental bugs from trusted peers.

## Current Security Posture

### Useful protections already present

- UDP receive storage is capped at the expected maximum datagram size.
- UDP decoding checks the header minimum, magic/version, session ID, exact total payload length, and SipHash tag before the packet reaches normal handlers.
- Audio and control packet handlers contain several type-specific payload-size checks.
- STUN is used for discovery and is not part of the audio data path.
- The WAV chunk walk has basic file-size boundary checks before reading chunk payloads.
- Asset completion checks a SHA-256 content hash before accepting the complete transfer.

These are meaningful foundations. The main v1 risks are not an obviously writable network buffer; they are unbounded resource use, insufficient message authority, endpoint abuse, unchecked numeric/model expansion, and weak containment around transferred files.

### Overall assessment

| Area | Current assessment | Primary concern |
| --- | --- | --- |
| TCP control admission/framing | High priority | Unbounded unauthenticated connections and receive buffers |
| TCP authentication | High priority | Raw key transmission, no server proof, and pre-auth client dispatch bug |
| Control authorization | High priority | Authenticated peers are not restricted by source or message role |
| Project/model JSON | High priority | Remote values can drive unbounded allocations and numeric conversions |
| Asset transfer | High priority | Full-file/base64 copies and unbounded size/chunk/disk work |
| WAV processing | Medium-high priority | Remote file reaches a custom parser/DSP worker without strict format/resource validation |
| UDP parser | Good base, medium hardening | Fixed/authenticated format exists; replay, horizons, and work budgets are incomplete |
| Mesh endpoint activation | Medium-high priority | Advertised endpoint is trusted without proof of UDP source ownership |
| Secrets/privacy | Medium priority | Shared key exposure surfaces and plaintext control/audio |
| Socket RAII | Correctness priority | Move assignment ends object lifetime and then writes the ended object |

## Detailed Findings

### 1. TCP admission and framing are unbounded

Relevant sources:

- [app/application/ControlServer.cpp](app/application/ControlServer.cpp)
- [app/application/ControlClient.cpp](app/application/ControlClient.cpp)

`ControlServer::listen` binds to `QHostAddress::Any`. That is necessary for direct remote control, but every interface can accept traffic for the duration of the session.

The server currently accepts every pending TCP connection into `peers_` before authentication. There is no hard pending-connection cap, authentication deadline, incomplete-frame deadline, or aggregate connection cap. A remote host can therefore consume sockets and peer objects without knowing the session key.

Both control endpoints append `readAll()` to a `QByteArray` and wait for a newline. A sender can omit the newline and grow the buffer indefinitely. A valid line and parsed JSON object also have no explicit maximum size. The server broadcast path calls `write` for every authenticated peer without a queued-output high-water policy.

Likely result: memory/file-descriptor exhaustion or GUI event-loop starvation rather than code execution.

Lightweight correction:

- Use a four-byte network-order length prefix for handshake frames and a small fixed authenticated header after the handshake.
- Reject a length above the declared maximum before reserving or appending payload storage.
- Keep a fixed receive state per connection: header bytes received, declared payload length, payload bytes received, and deadline.
- Add a small pending-unauthenticated cap, bounded failed-key work,
  authentication deadline, incomplete-frame deadline, and output high-water
  mark. An optional creator-selected session peer limit is enforced after valid
  authentication; there is no application-wide authenticated-peer cap.
- Limit frames processed per event-loop turn and resume later so one peer cannot monopolize the GUI/control thread.
- Close the offending connection and increment a reason-specific counter when a hard limit is violated.

Suggested starting limits, to be named constants and validated against real project sizes:

| Limit | Starting point | Reason |
| --- | ---: | --- |
| Maximum control JSON payload | 64 KiB | Far above ordinary human-paced control messages; small enough for simple fixed bounds |
| Pending unauthenticated TCP peers | 8 | Covers expected concurrent connection attempts without making admission unbounded |
| Authentication deadline | 5 seconds | Reclaims scanners and stalled handshakes |
| Incomplete-frame deadline | 5 seconds | Prevents slow indefinite frame accumulation |
| Per-peer queued output high-water | 256 KiB | Four maximum frames; close a peer that cannot consume control state |
| Frames handled per event-loop turn | 32 | Bounds monopolization while allowing normal bursts |

When the creator configures a session peer limit, enforce it in the shared
session controller rather than only in GUI presentation. Without that setting,
authenticated membership is limited by actual machine resources while every
connection and per-peer state remains independently bounded.

If a measured valid song/project snapshot exceeds the ordinary control-message maximum, give that message family a separate bounded chunked transfer/state machine. Do not raise the limit for every control message merely to accommodate one large payload.

### 2. The control client dispatches messages before authentication

Relevant source:

- [app/application/ControlClient.cpp](app/application/ControlClient.cpp)

While `authenticated_` is false, the client specially handles `hello.ok` and `hello.error`, but any other valid JSON object falls through to `onMessage`. A malicious endpoint can send `session.settings`, `mesh.peer_list`, song, metronome, or asset messages before authentication succeeds.

This is a concrete state-machine bug, not only a hardening preference.

Lightweight correction:

- Before authentication, accept only the exact next handshake message for the current handshake state.
- Reject and close on every other type, duplicate step, malformed proof, or unexpected field shape.
- Do not install or invoke the normal application-message callback until mutual authentication completes.
- Add a deterministic test proving that every application message is ignored/rejected before the authenticated state.

Cost: a branch per handshake frame and no steady-state audio cost.

### 3. The master session key is sent directly and the client does not authenticate the server

Relevant sources:

- [app/application/ControlServer.cpp](app/application/ControlServer.cpp)
- [app/application/ControlClient.cpp](app/application/ControlClient.cpp)

The client currently sends the session key as a JSON string in the initial TCP hello. TCP is not encrypted. A passive observer can obtain the key, while a malicious endpoint can return `hello.ok` without proving that it knows the key.

For the v1 threat boundary, full TLS and certificate identity would add more deployment and UI complexity than value. A small challenge-response protocol is a better balance.

Recommended control handshake:

1. Server sends the session ID, protocol version, and a fresh server nonce in a bounded challenge frame.
2. Client sends a fresh client nonce, its claimed/bootstrap token, its UDP candidate, and a keyed proof over the session ID, version, both nonces, and the supplied fields.
3. Server validates the proof, assigns/binds the stable peer identity for this connection, and returns its keyed proof over the same transcript and assigned identity.
4. Both sides derive separate client-to-server and server-to-client control-frame keys from the master session key and transcript.
5. Application frames begin at sequence one and are accepted only after the final proof is valid.

Use HMAC-SHA-256 or an equivalently reviewed keyed construction and truncate authenticated frame tags to 128 bits. SHA-256 is already used in the GUI, so this need not introduce a large dependency. Domain-separate control, UDP, and any future asset keys rather than using identical key material directly for every protocol.

An example authenticated control envelope is:

```text
u32 frame_bytes
u8  version
u8  message_class
u16 flags_must_be_zero
u64 connection_sequence
u8  authentication_tag[16]
u8  payload[frame_bytes - fixed_header]
```

The tag must cover the fixed header with a zeroed tag field, payload, session identity, direction, and bound connection/peer identities. Exact bytes require golden protocol tests; native structs must not be sent.

This protects the key from direct transmission and prevents ordinary on-path modification/replay of control frames. It does not encrypt message contents, and because invited peers share the master key it does not provide cryptographic isolation from a deliberately malicious invited peer who can observe another connection. Authorization still has to be enforced in application logic.

Expected cost: at most one extra handshake round trip, a few SHA-256 operations during join, approximately 28-32 bytes of framing per control message, and one MAC verification per human-paced message. It does not touch the audio callback or UDP audio bandwidth.

### 4. Authentication is not authorization

Relevant sources:

- [app/application/ControlServer.cpp](app/application/ControlServer.cpp)
- [app/gui/MainWindow.cpp](app/gui/MainWindow.cpp)

After authentication, `ControlServer` invokes `onMessage` without the source peer. Creator-side and joiner-side messages both enter `MainWindow::handleControlMessage`. The receiver therefore cannot reliably enforce which connection or role may send coordinator snapshots, membership, session errors, grid actions, project state, or assets.

For example, a joiner can submit a coordinator-shaped `mesh.peer_list`; the receiving path clears its current peer table, accepts the supplied endpoints, and restarts the current mesh engine. Similar concerns apply to settings, song state, and asset messages.

Lightweight correction:

- Carry the authenticated stable peer ID and connection direction into every control dispatch.
- Never trust a `source_peer_id`, role, or endpoint identity supplied in the JSON payload when the connection already establishes it.
- Define a compact authorization table by message family.
- Separate requests/proposals from authoritative ordered snapshots. A peer may request a grid change; only the coordinator may issue the accepted grid revision.
- Reject an otherwise valid but unauthorized message before it mutates models, starts workers, writes files, or causes network reconfiguration.
- Send responses to the requesting peer when appropriate instead of broadcasting all responses to every peer.

Minimum authorization matrix:

| Message family | Accepted source | Rule |
| --- | --- | --- |
| Session contract/settings snapshot | Bootstrap coordinator | Joiners reject the same shape from other sources |
| Membership/peer-list snapshot | Bootstrap coordinator | Each advertised peer is also subject to endpoint proof |
| Grid/transport proposal | Any authenticated peer | Receiver overwrites source with connection identity; coordinator orders it |
| Accepted grid/transport revision | Current coordinator/authority as defined by the protocol | Revision and source role must match |
| Collaborative song/track edit | Any authenticated peer | Validate and order/rebroadcast it; local Sync off suppresses outbound actions and ignores inbound application |
| Asset request | Authenticated peer | Hash must be referenced by accepted project state and response is targeted |
| Asset start/chunk/done | The peer from which that hash was explicitly requested | Reject unsolicited or overlapping transfer state |
| Heartbeat/acknowledgement | Bound authenticated peer | Sequence and source come from the authenticated connection; it cannot name or remove another peer |
| Leave request | Bound authenticated peer | Removes only the requesting peer |
| Session end | Bootstrap coordinator | Joiners reject attempts by any other peer to end the jam |

Cost: a source lookup and a few comparisons per control message. It has no packet-rate audio cost.

The TCP control plane uses a bounded heartbeat outside every real-time path. The
creator sends a check-in every 30000 ms; five consecutive misses expire the
approximately 150-second joiner grace period. Reauthentication and a valid
heartbeat inside that window resume the session. Expiry returns a GUI joiner to
Local or terminates a headless `network join` command with an explicit reason.
An authenticated coordinator-only End Jam bypasses the grace period. Missing an
ordinary peer removes only that peer and never ends the creator's session.
Control heartbeat interval/miss/age counters stay visible in native stats and
configuration; UDP-only engine/debug cases have no such lifecycle policy.
Deterministic native debug scenarios may use an explicit shorter interval and
threshold, while production continues to assert the 30000-ms/five-miss default.

### 5. Remote JSON can drive unbounded model growth and unsafe numeric conversion

Relevant sources:

- [app/gui/MainWindow.cpp](app/gui/MainWindow.cpp)
- [app/gui/BeatGridModel.cpp](app/gui/BeatGridModel.cpp)
- [app/gui/LooperProject.cpp](app/gui/LooperProject.cpp)
- [app/gui/SharedTrackController.cpp](app/gui/SharedTrackController.cpp)

Remote values are often converted with permissive `toInt`/`toDouble` defaults and then passed into models. `grid.resize`, for example, applies only a lower bound and resizes several vectors to the remote beat count. Song sections, lanes, lyrics, text fields, names, IDs, project arrays, frame positions, gains, and durations do not share one strict schema/limit layer.

Likely result: memory/CPU exhaustion, inconsistent model state, integer overflow, or non-finite values entering later calculations.

Lightweight correction:

- Give every network message type an explicit validator before model mutation.
- Check JSON type before conversion; do not rely on permissive default conversion.
- Require integers where integers are intended and range-check before narrowing.
- Reject NaN/infinity and bound every floating-point control.
- Define maximum counts for sections, beats per section, lanes, events,
  requested hashes, and concurrent transfers. Bound peer-list entries per
  control frame/page and validate each entry, but page membership rather than
  imposing an application-wide total peer limit.
- Define maximum UTF-8/UTF-16 lengths for IDs, names, chord cells, beat cells, lyrics, error strings, and paths.
- Use checked addition/multiplication for frame, byte, sample, and allocation calculations.
- Require monotonic/versioned state where stale data must not overwrite current state.
- Increment rejection counters by message type and reason without logging each rejected packet in a flood.

The limits should be product-derived and centralized in one protocol/model-limits header. They should not be scattered GUI magic numbers.

### 6. Asset transfer can consume unbounded memory, CPU, and disk

Relevant source:

- [app/gui/AssetTransferService.cpp](app/gui/AssetTransferService.cpp)

The sender reads the entire WAV, copies portions, base64-encodes each chunk, wraps the encoded text in JSON, and broadcasts through the generic control sender. The receiver trusts `file_bytes` enough to reserve from it, appends decoded chunks to one growing `QByteArray`, hashes only at completion, and writes the completed buffer directly to the final path.

There is no common hard asset-size limit, transfer timeout, chunk-count limit, decoded-chunk limit, aggregate disk limit, concurrency limit, or requirement that the transfer was requested. Repeated asset requests can also force repeated hashing, reading, base64 work, and broadcasts.

SHA-256 currently verifies that the received bytes match the sender-supplied hash. It does not make the sender trustworthy or make the WAV safe to parse.

Lightweight correction:

- Derive the maximum asset bytes from the maximum supported PCM16 duration, channel count, and a small bounded WAV-header allowance.
- Accept an incoming transfer only for a validated 64-character lowercase hexadecimal hash that is pending because accepted project state referenced it.
- Allow only a small fixed number of transfers; for v1, one active inbound transfer application-wide and one active outbound file source is a simple starting policy.
- Validate declared bytes, chunk size, expected chunk count, index, decoded bytes per chunk, cumulative bytes, and completion count before writing.
- Read the sender file incrementally; do not retain the complete asset merely to send it.
- Write incoming bytes incrementally to a temporary file while updating `QCryptographicHash`.
- Abort and remove the temporary file on timeout, disconnect, order error, excess bytes, hash mismatch, or disk failure.
- Commit with `QSaveFile` or an equivalent same-directory atomic replace only after size/hash/WAV validation succeeds.
- Avoid duplicate simultaneous requests for the same hash and cache a validated file's size/hash result for the session where safe.
- Target asset responses to the requesting peer rather than broadcasting them.

Base64 remained bounded through Phase 11. Phase 12 now replaces that chunk body
format completely with raw authenticated binary frames on the existing control
plane; it does not retain a base64 compatibility decoder. Each frame has an
exact bounded prefix and payload, is tied to an already requested transfer and
authenticated source, and validates transfer identity, index/offset, payload
length, cumulative bytes, chunk count and state transition with checked
arithmetic before allocation or disk I/O. Metadata stays in the existing strict
structured control model. Streaming, incremental hashing, temporary-file
cleanup, strict WAV validation and atomic commit remain mandatory, and bounded
chunk scheduling must not starve heartbeat or ordinary control frames.

The implemented current format requires a fixed 24 KiB declared chunk size and
exact 24 KiB non-final chunks, permits only a smaller final chunk, and derives a
hard maximum chunk count from the maximum asset size. The receiver enforces an
eight-chunk queue and high-water backpressure. Peer disconnect, session stop,
and Track Sync-off cancel the applicable queued, active, pending, and inbound
work without discarding unrelated peers' transfers. Deterministic boundary
coverage verifies exact size/count/order/offset/source/hash behavior,
fragmentation/coalescing with ordinary control frames, and cancellation.

Cost: the same O(file bytes) hashing work, small bounded per-chunk state, and
much lower peak memory. A full 24 KiB chunk uses 24,656 authenticated framed
bytes versus 32,768 base64 characters for the former data field alone, at least
a 24.76% transfer reduction before removed JSON overhead.

### 7. Remote project paths must not select local files

Relevant sources:

- [app/gui/MainWindow.cpp](app/gui/MainWindow.cpp)
- [app/gui/LooperProject.cpp](app/gui/LooperProject.cpp)
- [app/gui/PreparedMixRenderer.cpp](app/gui/PreparedMixRenderer.cpp)

`normalizeLooperAssetPaths` replaces a lane's `asset_path` only when `asset_hash` is non-empty. A remote lane with no validated hash can therefore retain an absolute or relative path. Prepared-mix code resolves and attempts to open that path.

No automatic exfiltration of arbitrary file content was confirmed in the reviewed path, but remote metadata should never be able to choose a local file for parsing or processing.

Lightweight correction:

- Ignore all remote `asset_path` values.
- Require a valid content hash for every remotely referenced asset.
- Construct the path locally as `<fixed-session-cache>/<validated-hash>.wav`.
- Canonicalize the cache root and verify the final path remains beneath it before open/commit.
- Keep user-selected local project paths as a separate trusted-local model field that is never populated from remote state.

Cost: a hash-format check and local path construction per asset reference.

### 8. WAV parsing needs strict format and resource containment

Relevant source:

- [app/gui/PreparedMixRenderer.cpp](app/gui/PreparedMixRenderer.cpp)

The custom parser reads the complete file before applying the five-minute processed-output limit. It recognizes RIFF/WAVE and walks chunks with basic bounds, but it does not fully validate the RIFF declared size, PCM format tag, supported channel maximum, byte rate, block alignment, data alignment, duplicate critical chunks, chunk-count/header-work bounds, or all arithmetic before allocations and DSP work. It can also retain/copy large WAV data per lane.

No clear out-of-bounds read was confirmed in the reviewed 64-bit code. The
larger practical risk is resource exhaustion or unexpected numeric state
reaching the stretch/mix worker. Finite deterministic malformed and boundary
coverage is required for core hardening because this parser receives
network-originated bytes. Phase 12 also retains an initial bounded native
mutation corpus through the refactored Python tooling; broader fuzz coverage
and sanitizer orchestration are optional future supplements.

Lightweight correction for a deliberately narrow v1 WAV format:

- Accept only RIFF/WAVE, uncompressed PCM format tag 1, 16-bit samples, the session sample rate, and explicitly supported mono/stereo channel counts.
- Check the filesystem size against the product-derived maximum before reading the file.
- Validate RIFF size, chunk headers, padding, chunk count, cumulative header bytes, unique required `fmt`/`data` chunks, block alignment, byte rate, data-size alignment, and frame count using checked arithmetic.
- Reject trailing/duplicate/unsupported structures according to one documented policy rather than partially interpreting them.
- Do not allocate output until validated frame counts and processing-speed bounds establish the maximum.
- Avoid duplicating the same WAV bytes for multiple lanes that reference the same hash.
- Keep parse, hash, file I/O, stretch, and mix work on a bounded non-real-time worker.
- Retain the Phase 12 bounded native `jam2_test.py fuzz wav` target as an
  additional regression tool; expanded campaigns and sanitizers are optional.

If future versions accept broad codecs or complex container variants, a
sandboxed decoder process would become more attractive. It is unnecessary for
the narrow PCM16 v1 format if the local parser is bounded, isolated, and covered
by deterministic malformed-input regressions; optional fuzzing can add further
confidence without gating the core refactor.

### 9. UDP has good authentication foundations but incomplete abuse bounds

Relevant sources:

- [libs/jam2-core/src/protocol.cpp](libs/jam2-core/src/protocol.cpp)
- [libs/jam2-core/src/udp_socket.cpp](libs/jam2-core/src/udp_socket.cpp)
- [app/cli/NetworkRuntime.cpp](app/cli/NetworkRuntime.cpp)

The current fixed UDP header and 64-bit SipHash tag provide session integrity at low overhead. Audio remains plaintext. The receive buffer is fixed to the expected maximum datagram size, which sharply limits direct packet-size abuse.

Remaining issues:

- Unknown packet types and invalid flag/reserved combinations are not rejected centrally before dispatch.
- Authentication currently copies the complete packet and throws exceptions for routine invalid traffic.
- Replay handling is incomplete/inconsistent across message types.
- Sequence comparisons need modular wrap handling.
- Very large authenticated sequence gaps can cause excessive advancement/work if drain logic iterates toward the supplied value.
- Remote sample times and timestamps need past/future acceptance horizons before entering reorder, jitter, or mix storage.
- Ping/pong values are not consistently bound to a currently outstanding challenge, so replayed/forged authenticated responses can distort RTT/timing observations.
- A receive burst can consume too much scheduler time unless packet work is budgeted and send deadlines are rechecked.
- Current map-based pending/reorder/mix storage is dynamic and can be stressed by an authenticated peer's sequence/timeline choices.

Lightweight correction:

- Perform cheap fixed checks first: size, magic, version, known type, allowed flags/reserved zero, session, and exact type-specific payload length.
- Verify the existing tag directly over packet spans without allocation.
- Return an explicit parse result and counter rather than throw for ordinary invalid datagrams.
- Maintain a fixed 64- or 128-packet replay bitmap per peer/stream, using wrap-safe sequence distance.
- Reject stale and excessive-future sample/timeline values before they reach storage.
- Bound sequence-gap advancement; a huge jump must cause a bounded reject/resync decision, never a loop proportional to the gap.
- Use fixed indexed reorder/jitter/mix storage with visible capacity/high-water/drop counters.
- Limit datagrams processed per wake/batch and re-evaluate the next audio send deadline between batches.
- Put an unpredictable nonce in ping requests, retain a small fixed outstanding set, and accept exactly one matching pong from the bound peer/source.

Expected steady cost is a few comparisons and bitmap operations per packet. At approximately 750 packets/sec per peer, even tens of simple operations per packet are negligible compared with socket, PCM conversion, map, and resampler work. Fixed storage and allocation-free authentication may reduce overall cost.

### 10. Advertised mesh endpoints are activated without source proof

Relevant sources:

- [app/application/SharedSessionController.cpp](app/application/SharedSessionController.cpp)
- [libs/jam2-core/src/udp_socket.cpp](libs/jam2-core/src/udp_socket.cpp)

The current control flow accepts a peer-advertised `udp_endpoint`, distributes it, and restarts the mesh engine around the supplied endpoint. A malicious authenticated peer can advertise a third party's address, causing Jam2 peers to direct UDP traffic toward that address. A hostname can also cause repeated resolution in the current packet path.

Lightweight correction:

- Treat every control-advertised endpoint as an unverified candidate.
- Resolve/normalize it once outside the packet loop and keep a numeric address key.
- Send only a small, rate-limited number of authenticated challenge probes to an unverified candidate; never send audio to it.
- Activate an edge only after the correct response arrives from the observed UDP source and is bound to the authenticated peer/session/challenge.
- If the observed endpoint changes, return the edge to probing and repeat proof before audio resumes.
- Expose candidate, probing, authenticated, compatible, active, stale, and failed state with a reason.

Expected cost: a few small packets and at most roughly one extra round trip during edge activation. All mesh edges can probe in parallel. There is no steady-state bandwidth or audio-latency increase.

### 11. Trusted invited peers share one non-confidential session boundary

The UDP SipHash tag authenticates possession of the shared session key; it does not encrypt audio. Current TCP control and asset content are also plaintext. A peer with the group key can potentially forge another peer's UDP packets if identity is not additionally bound, and any observer on the traffic path can hear/read content.

For the stated controlled-peer v1 use case:

- Keep a shared session master key.
- Derive protocol-specific keys and bind stable peer/source identities wherever possible.
- State clearly that v1 direct traffic is authenticated for session safety but
  is not confidential.
- Require users to invite only trusted participants. Traffic encryption,
  pairwise public-key exchange, and per-edge cryptographic isolation are not
  Jam2 product or refactor goals.

Pairwise UDP keys would require at least per-destination final authentication even when audio payload encoding is shared. The raw cryptographic CPU would still be small at Jam2's bitrate, but negotiation, identity, recovery, testing, and fan-out complexity are not justified for this v1 balance.

### 12. Session secrets require strong generation, not local redaction

Relevant sources:

- [libs/jam2-core/src/common.cpp](libs/jam2-core/src/common.cpp)
- CLI/GUI process-launch and benchmark metadata paths

Session IDs and keys must come from a cryptographically strong operating-system
source. The master key necessarily appears in the invite URL and may also appear
in local process arguments, logs/tool output, clipboard history, scenarios, and
benchmark metadata. Those local surfaces are explicitly outside the
application threat boundary.

Lightweight correction:

- Add one Qt-free `secure_random_bytes(span)` wrapper backed by the operating-system CSPRNG on Windows and macOS, with explicit failure reporting.
- Generate session keys, nonces, peer bootstrap tokens, and challenge values only through that wrapper.
- Keep invite creation, joining, diagnostics, and automation simple and
  inspectable; local keys do not require redaction.
- Continue to avoid transmitting the raw master key in the network control
  handshake; use challenge-response and derived protocol keys.
- Do not attempt elaborate in-memory secret wiping through copied Qt strings for v1; reduce unnecessary copies/lifetimes instead.

OS random generation happens only at session/handshake time and has no measurable audio cost.

### 13. UDP socket move assignment violates object-lifetime rules

Relevant source:

- [libs/jam2-core/src/udp_socket.cpp](libs/jam2-core/src/udp_socket.cpp)

`UdpSocket::operator=(UdpSocket&&)` explicitly invokes `this->~UdpSocket()` and then writes `handle_` without reconstructing the object. That is C++ object-lifetime undefined behavior.

No remote trigger was established, but socket ownership is a security and reliability boundary. Replace this with a private `close()`/RAII-handle reset followed by `std::exchange`, or use a scoped socket-handle type whose move assignment closes the old handle without ending the containing object's lifetime.

Cost: none.

## Recommended Lightweight Security Set

The following set gives the best risk reduction without materially increasing audio/network cost.

### Target controls for the refactored v1 network path

1. Fixed length-prefixed control framing with a hard payload maximum.
2. Small hard limits for pending unauthenticated connections, failed-key work,
   per-peer buffers, output queues, frames per event turn, and
   handshake/incomplete-frame time; enforce an optional creator-selected
   session peer limit without an application-wide authenticated-peer cap.
3. Strict pre-authentication state machines on both client and server.
4. Mutual challenge-response without transmitting the raw master key, followed by sequenced authenticated control frames.
5. Authenticated source identity and an explicit per-message authorization matrix.
6. Central strict schemas and product-derived bounds for all remote JSON/model values.
7. Requested-only, size/chunk/time/concurrency-bounded streaming asset transfer to a temporary file with incremental hashing and atomic commit.
8. Content-hash-only remote asset references and locally constructed canonical paths.
9. A narrow, strictly validated PCM16 WAV parser running on a bounded worker,
   plus deterministic malformed/boundary coverage and the retained Phase 12
   bounded native fuzz baseline.
10. Exact UDP version/type/length validation for the one current compact layout,
    allocation-free authentication, replay windows, wrap-safe sequences,
    sample-time horizons, bounded gap handling, and per-wake work budgets.
11. Authenticated UDP endpoint proof before audio activation.
12. OS-backed secrets/nonces plus domain-separated control/UDP key derivation.

### Explicit product non-goals

- TLS certificates, certificate pinning UI, or a private certificate authority.
- UDP encryption/AEAD and audio confidentiality.
- Pairwise public-key identities or per-edge secret negotiation.
- Accounts, passwords, rooms, bans, IP reputation, or intrusion-detection features.
- Relay/TURN audio paths.
- A sandboxed codec service while only the bounded PCM16 WAV subset is accepted.

These items address privacy, malicious invited peers, persistent identity, or
broad public-service operation. Jam2 instead assumes short direct sessions
among trusted invited participants and focuses its security work on
authentication, integrity, authorization, bounded resource use, and robustness
against malformed traffic and common implementation bugs.

## Computational and Network Cost

### Expected normal two-person session impact

| Control | CPU | Memory | Bandwidth/latency |
| --- | --- | --- | --- |
| Connection/frame/size/time caps | Effectively zero | Fixed small state; lower worst case | None |
| Schema and authorization checks | A few comparisons per human-paced message | None beyond validators | None |
| Challenge-response | A few hashes at join | Tens/hundreds of bytes | At most one extra RTT during join |
| Authenticated TCP frame MAC | One SHA-256-based MAC per control message | Small sequence/key state | About 28-32 bytes per control message |
| UDP replay/horizon/type checks | A few integer/bitmap operations per packet | Roughly 16-64 bytes per peer/stream | No packet growth and no playout delay |
| Endpoint proof | Only during join/change | Small challenge state | A few probe packets; about one RTT before edge activation |
| Incremental asset hash/write | Same O(n) work as whole-file hash | Much lower peak memory | Same with base64; lower after raw chunks |
| Strict WAV checks | Negligible beside file I/O/DSP | Prevents invalid allocations | None |
| Phase 12 fuzz/sanitizer tests | Zero in release builds | Zero in release builds | None |
| OS CSPRNG | Session/handshake only | Negligible | None |

At a representative 750 UDP packets/sec per remote peer, a 20-operation validation/replay path is about 15,000 simple operations/sec per peer. Three remote peers would be about 45,000 operations/sec, which should be below measurement noise compared with socket calls, PCM packing, resampling, and the current dynamic maps. This is an order-of-magnitude illustration, not a substitute for the plan's before/after profiling.

Expected user-visible result:

- Audio bandwidth: unchanged.
- Audio playout latency: unchanged.
- Packet-loop CPU: likely unchanged or lower once copy/allocation/exception paths are removed.
- Control bandwidth: trivially higher for small authenticated headers; asset bandwidth lower if raw chunks are adopted.
- Peak control/asset memory: materially lower and bounded.
- Join time: potentially one additional RTT for mutual proof and one parallel RTT for endpoint proof.
- Implementation complexity: moderate, concentrated in reusable framing/validation/state-machine components rather than the audio callback.

## Implementation Shape

### Keep security work out of the real-time callback

```text
TCP/control thread
    bounded deframer -> handshake/MAC -> source authorization -> schema validator
        -> bounded EngineCommand

UDP/network thread
    fixed receive buffer -> cheap header checks -> SipHash -> replay/horizon checks
        -> fixed PeerStream slot -> local-timeline output

asset worker
    authorized request -> bounded stream -> temp file + incremental SHA-256
        -> strict WAV validation -> atomic commit -> prepared-mix worker

audio callback
    consumes only already validated, bounded, prepared audio/control state
```

### Central reusable components

Prefer small components with no GUI model mutation inside them:

- `ControlFrameDecoder`: fixed receive state, maximum length, sequence, MAC result, and explicit failure reason.
- `ControlHandshake`: explicit client/server states, nonces, transcript proof, peer binding, and deadline.
- `ControlAuthorization`: message family plus authenticated source/role to allow/reject.
- `RemoteMessageValidator`: exact field types, counts, lengths, numeric ranges, and revision rules.
- `AssetTransfer`: requested hash, expected/received bytes, next chunk, deadline, incremental hash, temporary file, and final status.
- `WavPcm16Parser`: bounded header/format metadata and explicit parse result independent of rendering.
- `ReplayWindow`: fixed wrap-safe sequence bitmap with accept/duplicate/old/ahead result.
- `EndpointProbe`: candidate, challenge, observed source, attempts/deadline, and state.
- `secure_random_bytes`: platform CSPRNG boundary.

Each ordinary malformed-input path should return a small result enum. Exceptions remain appropriate at top-level file/socket/worker boundaries for exceptional system failures, not for normal unauthenticated traffic.

## Relationship to the Authoritative Plan

This review supplies threat context, target properties, and useful adversarial
scenarios. It does not assign phases, gates, or completion state. Those are
maintained only in [refactor-plan.md](refactor-plan.md).

## Useful Security Scenarios

### TCP framing and admission

- Fragment every header/payload boundary, including one byte per read.
- Deliver multiple frames in one read.
- Declare zero, exact maximum, and maximum-plus-one payload lengths.
- Omit payload completion until the deadline.
- Open more pending unauthenticated connections than allowed, repeat invalid
  keys, and exceed a configured optional session peer limit.
- Fill a peer's output queue beyond the high-water mark.
- Verify current/peak connection, buffer, queue, timeout, and rejection counters.
- Verify memory and file-descriptor use remain bounded.

### Authentication and authorization

- Send every application message before authentication; none reaches `MainWindow`, models, engine commands, asset state, or files.
- Use wrong session, wrong proof, repeated nonce, altered transcript field, wrong frame tag, duplicate sequence, old sequence, and wrong direction.
- Connect to a fake server that cannot prove the session key; client must reject it.
- Send each coordinator-only message from a normal peer.
- Put a forged `source_peer_id`, role, or endpoint owner in an otherwise valid payload; connection identity wins.
- Verify authorization failures do not rebroadcast or partially mutate state.

### JSON/model limits

- Boundary and boundary-plus-one tests for every count/string/range constant.
- Negative, fractional, too-large, NaN/infinite, missing, null, and wrong-type numeric fields.
- Huge grid beat counts, peer lists, lanes, lyrics, text cells, events, and requested-hash arrays.
- Checked frame/byte arithmetic near integer limits.
- Stale/duplicate/future revisions.

### Assets and WAVs

- Unsolicited transfer, invalid hash, negative/oversized declared bytes, invalid chunk size/count, out-of-order/duplicate chunk, excess cumulative bytes, early/late completion, timeout, disconnect, hash mismatch, and disk failure.
- More simultaneous transfers and requests than allowed.
- Verify temporary files are removed and existing valid assets are not truncated on failure.
- RIFF size mismatch, truncated/oversized/padded chunks, excessive chunk count, missing/duplicate `fmt` or `data`, non-PCM tag, unsupported channels/rate/bits, invalid byte rate/block alignment, misaligned data, excessive frames, and arithmetic limits.
- Retain the Phase 12 bounded native control, binary-asset, and WAV fuzz targets
  as supplementary regression tools; exhaustive corpora and sanitizers are
  optional.

### UDP and endpoint state

- Wrong magic/version/session/key/tag/type/payload length and every compact
  header truncation/extension boundary.
- Sequence wrap, duplicate, reorder, replay outside window, and huge forward gap.
- Sample/timestamp just inside/outside past and future horizons.
- Burst beyond the per-wake budget while verifying send/callback deadline counters remain stable.
- Pong with no request, wrong nonce, duplicate pong, expired pong, and pong from wrong peer/source.
- Advertise a third-party endpoint; only bounded probes may be sent and audio must remain disabled.
- Change observed endpoint; edge returns to probing before audio resumes.

### Random generation and release overhead

- Verify CSPRNG failure produces a clear session-creation failure rather than a weak fallback.
- Record packet-loop CPU, allocations, copied bytes, send gaps, callback gaps, and UDP rejection counters before/after.
- Record normal control join time, frame overhead, asset peak memory, and transfer bytes.
- Run the Phase 12 fuzz command only through opt-in local debug execution with
  explicit input/process/iteration/time/output and native allocation bounds;
  release builds gain no listening fuzz service, ordinary runtime mutation
  path, or sanitizer cost. OS-level memory sandboxing is optional future work.

## Target Security Properties

- Unauthenticated traffic has aggregate admission/work limits, and each
  authenticated peer cannot grow packet, control, model, asset, mix, or disk
  state without declared per-peer bounds.
- The client cannot dispatch application data before authenticating the server/session.
- Every control mutation has an authenticated source and explicit authorization rule.
- The master key is not transmitted directly in the network control hello;
  local arguments, logs, clipboard contents, scenarios, and artifacts may
  contain it.
- Remote project state cannot select an arbitrary local path.
- Transferred assets are requested, streamed, incrementally verified, strictly parsed, and atomically committed.
- Invalid UDP input uses fixed work/storage; replay, gap, timestamp, ping, and endpoint state are bounded and measured.
- No audio is sent to an endpoint that has not proved the authenticated observed UDP source.
- No security control enters or blocks the real-time callback.
- Raw two-person and three/four-person packet/callback measurements remain
  useful evidence when investigating security-related timing changes.
- The documentation states that v1 traffic is not encrypted and that invited peers share the session trust boundary.

## Final Recommendation

Keep the bounds and state-machine corrections in shared typed control,
`PeerStream`, `NetworkSession`, and session-controller boundaries rather than
adding a separate security subsystem. The highest-value controls are limits,
source-aware authorization, streaming file handling, replay/timeline bounds,
and endpoint proof. They prevent both deliberate abuse and ordinary peer bugs
while adding effectively no steady audio latency or bandwidth.

Do not add TLS, encrypted UDP, pairwise cryptographic identity, accounts, or
relay infrastructure. Jam2's model is short direct sessions among trusted
invited users; authentication, integrity, authorization, and bounded handling
remain required even though confidentiality is not a product goal.
