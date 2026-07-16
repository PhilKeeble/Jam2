# Jam2 v1 Code, Protocol, Data-Flow, and Efficiency Review

## Purpose

This report reviews Jam2 as it approaches a v1-style release. It covers:

- The current GUI, engine, audio, network, recording, mesh, and control flows.
- Correctness and session-safety concerns that should be addressed before broad refactoring.
- Avoidable work in the UDP and audio fast paths.
- The custom UDP packet format and possible wire-efficiency changes.
- Large translation units and practical module boundaries.
- Stale or duplicated flows left by earlier development stages.
- The relationship between stabilization, efficiency work, and consolidating the two shipped applications.

This was a static source review. No compilation, runtime profiling, or
audio-device testing was performed. Quantified gains below are arithmetic
estimates from the original packet shapes and tuning profiles, not benchmark
results. Compare them with Jam2's raw statistics and retained benchmark tooling
when useful; they are not hard completion gates. Sections describing current
code paths record that original audit baseline, while implementation status and
order live only in [refactor-plan.md](refactor-plan.md).

## Executive Assessment

Jam2 is structurally close to a credible v1. The core real-time design is generally sound:

- ASIO/CoreAudio callbacks use preallocated scratch storage.
- Capture and playback normally cross thread boundaries through bounded rings.
- Recording samples are handed to writer threads rather than written in callbacks.
- Prepared-track file loading stays outside the callback and playback is command-driven.
- The live audio protocol is already a compact binary UDP protocol rather than JSON or a dynamic schema.
- Raw timing, packet, buffer, drift, and callback measurements are already treated as product features.

The principal weaknesses are concentrated outside the core callback implementation:

1. A playback-ring ownership violation can race the audio callback.
2. Mesh receive/mixing storage is allocation-heavy and not sufficiently bounded or peer-aware.
3. The packet loop performs thousands of small allocations per second in the aggressive profile.
4. Endpoint conversion and possibly name resolution happen at packet rate.
5. The mesh loop can add an unnecessary wait large enough to miss a fast-profile send interval.
6. Remote TCP control and asset synchronization need explicit input and memory bounds.
7. Engine and GUI behavior are concentrated in two very large source files.
8. Process, stdin, loopback TCP, JSONL, and GUI state form several overlapping local-control paths.
9. The Python integration and benchmark tools need stronger independent protocol generators and adversarial real-process coverage.

The recommended release strategy is:

1. Correct concurrency, boundedness, sequence, and control-input risks.
2. Expand focused Python real-process validation and retain comparable two-peer and mesh evidence.
3. Optimize the existing UDP implementation without changing the wire format.
4. Extract a Qt-free engine behind typed interfaces.
5. Ship one public executable named `jam2`: no arguments launch the GUI, while supported subcommands retain benchmarking and optional headless use.
6. Consider PCM16 or a smaller header only as separate measured protocol experiments.

Consolidating the binaries should be viewed primarily as a maintainability, packaging, lifecycle, and user-flow improvement. It is not expected to materially lower audio latency on its own because the existing GUI/process boundary is outside the real-time callback.

## Current Architecture and Data Flow

### Live two-peer audio

```text
Audio device callback
    -> capture SPSC ring
    -> network packet loop
    -> PCM24 pack
    -> authenticated UDP packet
    -> remote UDP socket
    -> authenticate and parse
    -> sequence reorder and jitter storage
    -> PCM24 unpack
    -> playback SPSC ring
    -> remote gain/resampling/mixing in callback
    -> audio device output
```

The network packet loop also carries ping/pong RTT probes, metronome state, transport state, and session shutdown packets on the same authenticated UDP protocol.

### Mesh audio

Each engine sends one encoded local packet to every remote endpoint. Received peer packets are authenticated and tracked per peer, then accumulated by sample time into a shared pending mix before being released to the playback ring.

This keeps audio direct and relay-free, but CPU, inbound bitrate, outbound bitrate, packet processing, and mix work scale with the number of remote peers. A four-person mesh gives every process three outgoing copies and three independent incoming streams.

### GUI to local engine

The current GUI launches the engine as a child process and uses a combination of:

- Command-line configuration.
- stdin text commands.
- A framed binary loopback TCP control connection.
- JSONL status on stdout.
- stderr and process lifecycle notifications.

The GUI reconstructs engine state from all of these plus locally cached UI state. The binary control protocol is already appropriate for frequent meters and clock snapshots; the process boundary itself is the source of most duplication.

### GUI to remote GUI

Remote GUI control uses authenticated newline-delimited JSON over TCP for:

- Session settings.
- Metronome settings.
- Song and arrangement state.
- Mesh membership coordination.
- Looper asset requests and base64 chunks.

This control path is outside the live audio path. Replacing all of it with binary messages would not directly improve audio latency. Binary framing is most worthwhile for assets and other fixed, frequent message shapes; readable JSON remains reasonable for human-paced song metadata.

### Recording and prepared tracks

The callback pushes recording samples into preallocated queues consumed by file-writer threads. Prepared mixes are loaded into fixed slots outside the callback and controlled through a bounded command queue. These are good boundaries and should survive engine extraction.

## Release-Priority Correctness and Safety Findings

### 1. Playback-ring consumer ownership

The playback ring is effectively designed as a single-producer/single-consumer structure:

- The network thread produces playback samples.
- The audio callback consumes playback samples.

When playback exceeds `playback_max_frames`, the network thread also calls `drop_oldest()`, which writes the consumer/read index. The callback can concurrently update the same index after a normal pop. One update can overwrite the other, causing incorrect depth or replay/drop behavior.

Recommended correction:

- Preserve one owner for the read index.
- Let the producer publish a requested-drop count atomically.
- Have the callback/consumer apply the request before its next pop, or introduce a rigorously designed overwrite ring whose ownership and accounting explicitly support producer-side eviction.
- Expose requested drops, applied drops, maximum requested batch, and any coalesced requests in stats/CSV.

This is a correctness fix, not merely an optimization.

### 2. Mesh mixing and storage

Mesh mixing currently uses an ordered map keyed only by sample time, with a dynamically sized sample vector as the value. This has several risks:

- Map and vector allocation occurs in the packet loop.
- Storage does not explicitly record which peers contributed to a timeslot.
- A packet arriving after a timeslot was released can recreate an old key.
- Very large future sample times can retain storage longer than intended.
- The mix cursor and per-peer startup epochs require explicit rules when peers start, restart, or join at different times.
- Progressive saturated addition can make clipped results dependent on peer iteration/arrival order.

Recommended correction:

- Use a fixed number of mix slots indexed by normalized playout time or a coordinator-defined stream epoch.
- Store membership-generation contribution metadata with each slot. Size or
  replace that metadata only when membership changes outside packet processing;
  do not impose a compile-time peer-count ceiling.
- Reject packets behind the released cursor.
- Reject or clamp packets beyond a configured future horizon.
- Release at a deadline even when peers are missing, recording the missing contributor count.
- Accumulate in a wider fixed integer type and saturate once at release for deterministic mixing.
- Expose active slots, maximum slots, late-after-release packets, future-window rejects, missing peer contributions, and capacity drops.

### 3. Sequence wraparound and replay handling

Sequence comparisons use ordinary unsigned greater-than/less-than in several receive paths. That fails when a 32-bit sequence wraps. At 750 packets per second, wrap takes roughly 66 days of uninterrupted audio, so it is not common but is straightforward to make correct.

Recommended correction:

- Use modular signed-distance comparisons for 32-bit packet sequences.
- Unit-test transitions across `0xffffffff -> 0`.
- Maintain bounded replay/duplicate windows for packet types where sequence identity matters.
- Keep audio loss, duplicate, reordered, late, and replay counts separate.

### 4. Remote TCP input and asset bounds

The remote control client/server append incoming TCP data until a newline is observed. Asset transfers declare a size, reserve memory, append decoded base64 chunks, and retain the complete asset in memory before hashing and writing it.

Recommended v1 bounds:

- Maximum JSON line length and total undecoded receive buffer.
- Maximum pending unauthenticated connections and failed-authentication work.
- Optional creator-selected session peer limit, with no application-wide
  authenticated-peer maximum.
- Maximum project message size.
- Maximum asset bytes, chunk bytes, and chunk count.
- Exactly 64 hexadecimal characters for SHA-256 identifiers.
- Non-negative sizes that fit local integer and filesystem limits.
- One explicitly identified transfer state per connection, or a small fixed transfer limit.
- Abort and clear state on disconnect, unexpected transfer start, wrong hash, wrong index, excessive decoded data, or timeout.
- Stream asset chunks to a temporary file while updating SHA-256 incrementally; atomically publish the file only after length and hash validation.
- Apply TCP backpressure instead of queueing an entire large asset through repeated socket writes at once.

### 5. Session admission limits

When a creator configures a session peer limit, a valid join above that limit
must be rejected cleanly and must not continue receiving peer lists, songs, or
assets. Without that option, the application does not impose an authenticated
mesh size cap. Pending unauthenticated connections and repeated invalid-key work
remain independently bounded to avoid resource exhaustion.

Recommended correction:

- Address the error to the offending connection rather than broadcasting it.
- Close that connection after the error frame is flushed.
- Do not add it to authenticated membership or include it in broadcasts.
- Count configured-session-limit rejections, pending-authentication rejections,
  authentication failures, and current authenticated/control peers.

### 6. UDP validation and observability

The UDP decoder verifies framing, session, payload length, and authentication, which is a good base. It should additionally provide:

- Explicit allowed packet types.
- Required flags/reserved values for protocol v1.
- Exact or bounded payload size by type.
- Clear parse-result categories instead of exceptions for expected hostile/malformed datagrams.
- Separate counters for short packet, version, type, flags, session, size, authentication, replay, and endpoint failures.

This improves both efficiency and the raw diagnostic value required by the project rules.

### 7. TCP authentication properties

The peer TCP connection sends the session key in the initial JSON authentication message. The connection is authenticated but not encrypted; control content and transferred assets remain observable to a passive network observer.

A lightweight improvement is challenge-response authentication:

1. Server sends a random nonce.
2. Client returns session id plus a keyed authenticator over the nonce and protocol context.
3. Server compares the authenticator and never receives the raw key.

This does not provide confidentiality. The limitation should be documented
accurately rather than implying encrypted transport. Traffic encryption is not
a Jam2 product/refactor goal: invited participants are trusted, while the
lightweight authentication, integrity, and authorization controls remain
necessary for session safety without adding TLS deployment complexity.

## Quantified Fast-Path Baseline

### Packet rates and current network traffic

Jam2 transmits mono PCM24, so raw audio is always:

```text
48,000 samples/sec * 3 bytes = 144,000 payload bytes/sec
```

Using the 48-byte Jam2 header and 28 bytes for IPv4 plus UDP:

| Profile | Frames/packet | Packets/sec | Datagram bytes | Traffic/direction | Header bytes/sec |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fast | 64 | 750 | 268 | ~201 KB/s / 1.61 Mbit/s | 36 KB/s |
| Moderate | 128 | 375 | 460 | ~172.5 KB/s / 1.38 Mbit/s | 18 KB/s |
| Safe | 256 | 187.5 | 844 | ~158.3 KB/s / 1.27 Mbit/s | 9 KB/s |

These figures exclude Ethernet/Wi-Fi framing and control packets. Larger packets reduce header and packet-processing cost but increase packetization time and loss impact.

### Approximate allocation rate

For a normal live packet with the default jitter path, the current implementation can perform approximately:

- Transmit: PCM payload vector plus final authenticated packet vector.
- Receive: socket receive vector, authentication copy, decoded PCM vector, reorder map node, and jitter map node.

That is up to seven packet-related heap allocations per local send/receive cycle:

| Profile | Approximate allocations/sec/process in a two-peer session |
| --- | ---: |
| Fast | ~5,250 |
| Moderate | ~2,625 |
| Safe | ~1,313 |

This is a code-path estimate. Silence, disabled jitter, control traffic, map state, allocator implementation, and future storage changes affect the measured number. Mesh receive-side allocations scale approximately with the number of remote peers.

The byte volumes involved are small for a modern CPU. The concern is packet-rate allocator entry, cache disruption, and rare slow allocations causing packet-loop gaps.

## High-Value Efficiency Improvements and Expected Gains

### 1. Caller-owned UDP and PCM buffers

Change the packet APIs from returning vectors to filling caller-owned spans or fixed packet objects.

Target flow:

```text
preallocated transmit buffer
    -> write header with zero auth field
    -> pack PCM directly after header
    -> authenticate in place
    -> send span

preallocated receive buffer
    -> receive datagram view
    -> validate/authenticate view
    -> decode directly into a fixed reorder/jitter slot
```

Expected gain:

- Removes most of the estimated 5,250 allocations/sec at the fast two-peer profile.
- Removes allocator variance and repeated construction/destruction.
- Reduces cache churn and packet ownership complexity.
- Average end-to-end latency will probably remain similar.
- Worst packet-loop gaps and small-mesh scaling are the measurements most likely to improve.

This is the highest-confidence broad fast-path optimization.

### 2. Authenticate without copying the packet

The decoder currently copies the full packet so the stored authentication field can be zeroed before SipHash verification.

At the fast profile, the copied audio datagrams total approximately:

```text
240 Jam2 bytes/packet * 750 packets/sec = 180 KB/sec
```

The copy bandwidth is negligible. Avoiding 750 allocations/sec per remote peer is the meaningful gain.

Implement incremental hashing across:

1. Bytes before the tag.
2. Eight logical zero bytes.
3. Bytes after the tag through the payload.

Expected gain:

- Small steady-state CPU improvement.
- One fewer allocation per received packet.
- Lower packet-loop timing variance.
- No network bandwidth change.

### 3. Decode PCM directly into fixed packet storage

PCM24 unpack currently returns a newly allocated decoded vector. Decode into the final reorder/jitter slot instead.

Expected gain:

- Removes 750 decoded-vector allocations/sec per remote peer in the fast profile.
- Avoids an intermediate dynamic object and move.
- Makes maximum queued audio memory explicit.
- Small average CPU improvement; greater benefit to tail timing and mesh.

### 4. Resolve endpoints once and keep numeric keys

Every send currently reconstructs the socket address. Numeric endpoints repeat textual conversion, and a hostname can reach name resolution from the packet loop. Every receive also formats the source address back into text.

Use a `ResolvedEndpoint` containing the native numeric address, port, and compact comparison key. Resolve only when a peer is added or its endpoint changes.

Expected gain:

- Removes up to 1,500 send/receive address conversions per second per peer in the fast profile.
- Modest CPU reduction for numeric addresses.
- Potentially removes severe blocking stalls when a hostname would otherwise trigger resolver work.
- Makes mesh peer lookup cheaper and allocation-free.

This should be implemented early because blocking name resolution in a packet loop has a much larger worst case than its usual cached behavior suggests.

### 5. Fixed-capacity reorder, jitter, and mesh slots

Replace ordered maps with storage sized from explicit configuration:

- Sequence-indexed reorder slots.
- Deadline-indexed jitter slots.
- Peer-aware mesh timeslots.

Expected gain:

- Removes approximately two map-node allocations per received packet in the normal jitter path, or about 1,500 allocations/sec per peer at fast settings.
- Replaces tree lookup with constant-time slot lookup.
- Provides fixed memory use and inspectable overflow behavior.
- Likely a low-single-digit network-thread CPU improvement for two peers.
- More important for deterministic timing and three/four-peer scaling than for average two-peer latency.

### 6. One deadline-aware network wait

The mesh loop can wait up to 1 ms in the socket receive call and then sleep another 1 ms when no packet was returned. A fast-profile audio packet is scheduled every:

```text
64 / 48,000 = 1.333 ms
```

A nearly 2 ms idle period can therefore miss a send deadline and cause the loop to transmit catch-up packets together.

Replace this with one wait bounded by the nearest required action:

- Audio send deadline.
- Ping deadline.
- Metronome/transport send deadline.
- Stats deadline.
- Shutdown request.

After waking, drain available datagrams nonblocking and return to deadline evaluation.

Expected gain:

- Removes up to roughly 1 ms of avoidable delay in affected loop iterations.
- Reduces departure jitter and catch-up bursts.
- May improve remote receive-jitter measurements.
- Does not change physical network propagation time or configured playout delay.

This is the optimization most likely to produce an immediately visible timing-stat improvement.

### 7. Bulk-copy audio rings

The ring currently performs `% capacity` for every sample. Normal capture and playback flow accounts for roughly:

```text
callback -> capture ring:  48,000 index operations/sec
capture ring -> network:   48,000
network -> playback ring:  48,000
playback ring -> callback: 48,000
total:                    ~192,000/sec
```

Copy at most two contiguous spans for each push/pop: from the current position to the end of the array, then from the beginning for the remainder.

Expected gain:

- Removes most runtime modulo/division operations from ring movement.
- Improves contiguous memory access.
- Probably less than 1% of total CPU on a modern desktop.
- Directly improves callback headroom, so it remains worthwhile after the ownership bug is corrected.

Do not combine this optimization with an unreviewed ownership change.

### 8. Explicit parse results instead of expected exceptions

Malformed, unauthenticated, wrong-session, and wrong-size UDP datagrams are routine untrusted input rather than exceptional program failures.

Use a result such as:

```text
PacketParseResult {
    status,
    parsed header/view when valid
}
```

Expected gain:

- Removes exception construction/unwinding for rejected traffic.
- Usually negligible in a healthy authenticated session.
- Materially improves behavior under malformed traffic or fuzz testing.
- Enables precise rejection counters without string matching.

### 9. Move blocking GUI work off the event thread

The GUI performs synchronous device child-process probes, full-file reads, hashing, copies, prepared-mix work, and asset serialization in user-triggered paths.

Expected gain:

- Does not reduce audio latency while the engine remains in another process.
- Prevents multi-second UI stalls and improves cancellation/progress reporting.
- Becomes more important after binary consolidation, when an unresponsive GUI and engine share one process.
- Streaming asset handling also reduces peak memory from roughly whole-file-plus-base64 copies to a small fixed chunk window.

## UDP Wire-Format Review

### What is already good

- Fixed 48-byte header.
- Explicit magic and version.
- Explicit little-endian encoding rather than sending native C++ structs.
- Session identifier and keyed authentication tag.
- Fixed PCM24 audio representation.
- Sequence and sample-time fields suitable for loss, reorder, and scheduling diagnostics.
- UDP payload remains below typical MTU sizes for current profiles.

### Fields and costs

The common header carries magic, version, type, flags, session id, sequence, sample time, send time, payload length, authentication tag, and reserved bytes.

Not every packet type needs every field:

- Audio needs sequence, sample time, session identification, type/version, and authentication.
- Ping/pong needs a timing token but not audio sample time.
- Metronome/transport needs scheduled timing fields but uses a much lower packet rate.
- UDP already supplies a datagram length, although an internal length remains useful for strict validation and future extensions.

### Smaller header estimate

Reducing the header from 48 to 32 bytes saves:

| Profile | Saving/direction | Approximate total IPv4/UDP reduction |
| --- | ---: | ---: |
| Fast | 12 KB/sec | ~6.0% |
| Moderate | 6 KB/sec | ~3.5% |
| Safe | 3 KB/sec | ~1.9% |

This is real but not transformative. It also requires a wire-version decision and touches every packet type.

The recommendation above governed the earlier fast-path phases. Phase 12 now
owns the one final replacement: audit which fields each packet type actually
uses, combine the mutually exclusive audio sample-time and ping timing-token
slots where safe, remove unused flags/reserved bytes, and select the smallest
fixed explicitly encoded header that preserves required timing, session,
sequence/replay, strict-size, and authentication behavior. Do not force a
particular byte count by deleting a justified field. Retain one current parser
and encoder only; reject stale versions clearly and remove the old layout rather
than maintaining compatibility.

### PCM16 network format

PCM16 reduces raw audio payload from 144 KB/sec to 96 KB/sec, saving 48 KB/sec per direction per remote peer regardless of packet size.

Including the current header and IPv4/UDP overhead:

| Profile | PCM24 traffic | PCM16 traffic | Reduction |
| --- | ---: | ---: | ---: |
| Fast | ~201 KB/sec | ~153 KB/sec | ~24% |
| Moderate | ~172.5 KB/sec | ~124.5 KB/sec | ~28% |
| Safe | ~158.3 KB/sec | ~110.3 KB/sec | ~30% |

For a four-person mesh, each process sends three copies:

```text
Fast PCM24 outbound: ~603 KB/sec / 4.82 Mbit/sec
Fast PCM16 outbound: ~459 KB/sec / 3.67 Mbit/sec
```

PCM16 is the only proposed wire change with a large predictable bandwidth gain. It could reduce Wi-Fi airtime, packet serialization, and burst pressure, but it does not reduce packet frequency, physical propagation time, or configured buffering.

Phase 12 decision:

- Implement both PCM16 and PCM24 as session-wide direct-network formats.
- Let the creator select Audio Quality once and have every joiner adopt the
  authenticated session value automatically, preserving encode-once fan-out.
- Keep internal processing and PCM16 Track/Looper/recording WAV behavior
  unchanged; only network pack/unpack differs.
- Default Start Jam to PCM16 after matched measurements and manual listening
  found no audible difference while preserving the explicit PCM24 choice.
  Headless/CLI runs continue to use their declared native format.
- Record format, header and payload bytes, bytes/sample, packet rate and bitrate
  in stats, CSV, native/Python manifests, stress results, and benchmarks.
- Compare matched non-silent recordings plus clean and impaired two/three/four-
  peer packet, callback, jitter, late/missing/drop/underrun, drift, queue, CPU,
  and bitrate data before deciding which default is preferable.

Phase 12 measured result on the 64-frame fast profile:

- The compact current protocol uses a 36-byte header. PCM24 therefore produces
  a 228-byte audio datagram and PCM16 produces a 164-byte audio datagram at the
  same packet rate of about 750 packets/second.
- Matched stress measured a 28.06% PCM16 send-bitrate reduction. The physical
  Windows/macOS non-silent tone/pulse benchmark measured a 28.07% packet-size
  and send-bitrate reduction, with both remote WAVs present for all four cases
  and no detected pop or clipping event.
- The retained pre-change physical benchmark used a 48-byte header and
  240-byte PCM24 datagrams. Compact PCM24 is 5% smaller without changing its
  192-byte payload; compact PCM16 is 31.67% smaller than that retained packet.
- CPU differences in the short comparison changed with run order and are not
  evidence of a CPU improvement. Packet frequency and configured latency are
  intentionally unchanged. Both formats passed manual listening, and PCM16 was
  selected as the GUI default for its measured bandwidth saving.

The final physical-device audit also found that adaptive recovery's requested
playback ratio was being overwritten by the outer runtime loop and that release
stopped when the target counter reached its minimum rather than when the real
device ring recovered. The mixer owns that ratio and continues until the actual
ring is within four packets of the target. The final reconciliation removed
hidden 5,000-ppm callback/core caps, made the configured numeric control
authoritative, and set the native profiles to an explicit 20,000 ppm. The
44.1-kHz physical PCM16 250-ms stall rerun recovered both rings from roughly
1,500 frames to 416 frames or below, shed 1,117-1,244 frames, retained packet
and mixer flow, and recorded no playback-underrun time. Headless audio now uses
the same bounded playback-ratio behavior instead of ignoring the control.

### Peer JSON

Converting all remote GUI JSON to binary is not a high-value audio optimization. Most messages are human-paced and do not cross the audio callback.

Recommended division:

- Keep song/arrangement metadata readable unless profiling shows a problem.
- Use explicit binary framing or raw framed chunks for large asset data to remove base64's roughly 33% size expansion and full-document copies.
- Consider fixed binary messages for frequent clock, metronome, transport, and membership snapshots only if their measured rate warrants it.
- Apply strict lengths and versions to both JSON and binary formats.

Phase 12 adopts the raw-chunk part of this division. Keep human-paced metadata
strictly validated and structured, but replace base64 asset bodies completely
with bounded authenticated binary frames on the existing control plane. Preserve
requested/source-bound transfer state, worker-thread streaming, checked
length/count/offset arithmetic, incremental hashing, strict WAV validation,
temporary-file cleanup and atomic commit. Do not add a second listener or a
whole-file buffer, and keep heartbeat/control frames schedulable during a large
transfer.

The implemented frame carries one fixed 24 KiB non-final chunk at a time; only
the final chunk may be smaller. A full chunk occupies 24,656 authenticated
framed bytes versus 32,768 base64 characters for the replaced data field alone,
which is at least a 24.76% wire reduction before the removed JSON field overhead
is counted. The fixed chunk size and derived maximum count also prevent a sender
from turning one permitted asset into an excessive number of tiny frames.

## Original Code Organization Review

The size and ownership findings in this section are the pre-split audit
baseline. The implemented application now has one `app` target, shared
application/session owners, extracted CLI services, page construction,
mixer/metronome/track workflow components, a non-widget Track workspace owner,
an independent control-message router, and an asset-transfer context/service.
`MainWindow` remains the presentation and user-intent coordinator rather than
the owner of protocol parsing, asset state machines, project/track resources,
or boundary validation.

### Engine entry point

The CLI engine entry point is over 8,000 lines and currently combines:

- Command-line parsing and validation.
- Scheduling and OS-priority configuration.
- Session handshake and STUN flow.
- Normal and mesh network loops.
- Reorder, jitter, drift, compensation, and transport behavior.
- Statistics formatting and CSV production.
- stdin command parsing.
- GUI-control socket framing.
- Audio-device setup and recording lifecycle.
- CLI diagnostic commands.

Recommended Qt-free modules:

- Engine configuration and validation.
- Resolved UDP transport and packet codec.
- Peer/session state.
- Reorder, jitter, playout, and drift control.
- Mesh mix scheduler.
- Metronome and transport synchronization.
- Runtime commands, events, and snapshots.
- Statistics aggregation/CSV formatting.
- Recording and prepared-source supervision.

The CLI front end should become argument parsing, mode selection, and text/structured presentation over the same engine API used by the GUI.

### GUI window

`MainWindow.cpp` is over 7,000 lines and combines UI construction with session coordination, process supervision, binary local control, remote JSON control, mesh membership, track recording, waveform behavior, project persistence, asset transfer, and diagnostic presentation.

Recommended GUI boundaries:

- Session and mesh coordinator.
- Engine adapter/controller.
- Mixer and stats view model.
- Metronome/transport controller.
- Track and recording workflow.
- Project persistence.
- Asset transfer service.
- Page/widget construction and presentation.

`MainWindow` should coordinate pages and user intent, not own protocol parsing or file-transfer state machines.

### Duplicated and transitional flows

The following are not necessarily dead code today, but become removable after engine integration:

- Child executable discovery and engine-path UI.
- Argument construction and internal reparsing.
- Child startup/replacement/kill escalation.
- Local loopback TCP server and engine client.
- stdin fallback for GUI controls.
- JSONL runtime status parsing.
- Duplicate state conversion among text commands, binary opcodes, JSON objects, and widgets.
- Mesh engine restarts when membership changes.
- Release staging of a second executable.

Device listing, probing, metering, local/headless audio, and benchmark modes should remain supported. They are valuable technical diagnostics, not stale production code.

## Single-Executable Direction

The selected target is one public executable named `jam2`:

- `jam2` with no subcommand launches the GUI.
- `jam2 list-devices`, device probes/meters, `local`, `network create`,
  `network join`, and explicit debug/diagnostic commands remain supported
  CLI/headless entry points.
- `listen`, `connect`, `mesh`, and public static-membership commands are removed
  without compatibility aliases; every network session uses universal mesh.
- Benchmark and stress tooling uses the unified public/debug surface.
- The GUI calls the engine API directly rather than launching its own executable.
- On Windows, CLI mode attaches to the invoking terminal while normal GUI launch remains console-free.
- On macOS, document the callable executable inside the application bundle for headless use.

Recommended engine boundary:

```text
EngineConfig
    immutable validated startup/session/device/tuning configuration

Engine
    start(config)
    requestStop()
    join()
    submit(command)
    snapshot()
    pollEvent()

EngineCommand
    fixed/bounded runtime operations, including peer add/remove/update

EngineSnapshot
    fixed-shape lifecycle, audio, timing, queue, and recording data

PeerSnapshot page/event stream
    bounded per-call peer data published outside real-time work; no global
    peer-count ceiling

EngineEvent
    bounded errors, lifecycle changes, peer changes, transport, and recording completion
```

Qt objects, JSON objects, dialogs, filesystem work, and callbacks into widgets should not enter the engine or real-time layer.

The binary-specific review is preserved in
[refactor-binaries.md](refactor-binaries.md). Both documents use the selected
one-public-executable design: the GUI is primary and CLI/headless/debug
operation remains available in `jam2`.

## Technical Design Areas

These groups preserve the audit's technical reasoning. They do not define
implementation order, status, or completion gates; those live only in
[refactor-plan.md](refactor-plan.md).

### Current-model correctness and bounds

1. Correct playback-ring ownership.
2. Make sequence handling wrap-safe.
3. Bound and validate remote control and asset traffic.
4. Bound pending authentication and enforce an optional creator-selected
   session limit at admission.
5. Make mesh mixing bounded, peer-aware, and deterministic.
6. Add precise rejection/drop counters.

Do not mix these fixes with a packet-header change.

### Python validation and comparable evidence

1. Add independent Python protocol, malformed-input, impairment, and artifact checks against real Jam2 processes; do not add CMake/CTest scaffolding.
2. Capture fast/moderate/safe two-peer baselines.
3. Capture two-, three-, and four-peer headless mesh baselines.
4. Record allocations/packet, network-loop CPU, packet-loop gaps, send intervals, callback gaps, jitter, loss, underruns, drift, and bitrate.

### Wire-compatible fast-path optimization

1. Resolve endpoints once and use numeric keys.
2. Introduce caller-owned receive/transmit buffers.
3. Authenticate without copying.
4. Decode PCM directly into fixed slots.
5. Replace ordered packet maps with bounded indexed storage.
6. Replace double waiting with deadline-aware polling.
7. Bulk-copy ring spans after the ownership correction.
8. Compare material changes with retained historical evidence when useful.

### Engine and peer-path extraction

1. Move shared configuration and validation behind typed structures.
2. Extract normal and mesh network behavior into reusable engine components.
3. Preserve audio, scheduling, metronome, transport, recording, priority, and stats semantics.
4. Keep the unified public headless and debug commands operating over the
   extracted engine.
5. Add dynamic mesh membership without restarting the audio device.

### Unified application

1. Make no-argument `jam2` launch the GUI.
2. Replace GUI child-process launch with the engine API.
3. Replace local TCP/stdin/JSONL state with typed commands, events, and snapshots.
4. Move blocking GUI file/device work to bounded worker tasks.
5. Remove second-binary staging and obsolete compatibility code.
6. Update packaging, documentation, benchmark paths, and shutdown tests.

### Phase 12 measured wire work

1. Add creator-selected session PCM16/PCM24 with automatic joiner adoption and
   matched benchmark/stress dimensions using actual non-silent audio.
2. Replace the current header once with the audited compact explicitly encoded
   layout and remove the obsolete layout without a compatibility parser.
3. Replace only asset chunk bodies with authenticated bounded binary frames;
   retain structured control/project metadata.
4. Implement these in the completed core/session/asset ownership boundaries;
   retain the existing bounded native fuzz baseline while using matched stress
   and benchmark runs as the Phase 12 performance and behavior evidence.

## Useful Validation Scenarios

### Protocol

- Golden encoded bytes and SipHash tags for every packet type.
- Truncated, extended, wrong-version, unknown-type, wrong-session, wrong-key,
  modified-tag, wrong-payload, and format-specific exact-size cases for the
  compact current header.
- Sequence wrap, gaps, duplicates, reorder recovery, late packets, and replay.
- Output-buffer-too-small and slot-capacity failures without allocation or exceptions.

### Ring and callback handoff

- Producer/consumer wraparound.
- Full and empty transitions.
- Concurrent requested-drop application.
- No lost read-index updates.
- Exact overrun, underrun, and drop accounting.
- Recording/prepared-source queue saturation without blocking the callback.

### Mesh

- Two, three, and four peers.
- Offset startup epochs.
- Missing, late, duplicated, and reordered peer packets.
- Peer add, remove, endpoint update, and restart without audio-device restart.
- Old and excessive-future sample times.
- Fixed-slot exhaustion.
- Deterministic mixing and clipping independent of arrival order.
- Per-peer and aggregate raw CPU, bitrate, packet-rate, jitter, and drop data.

### Remote control and assets

- TCP fragmentation and multiple frames in one read.
- Missing newline and oversized line.
- Authentication failure, pending-authentication rejection, and configured
  session-limit rejection.
- Excessive declared asset size.
- Invalid hash, wrong chunk order, duplicate chunk, excess decoded bytes, timeout, disconnect, and disk-write failure.
- Successful streaming transfer without retaining the complete base64 and decoded asset in memory.

### Unified application

- No-argument GUI startup.
- Every documented CLI subcommand.
- GUI start, join, local perform, disconnect, reconnect, and shutdown.
- Device failure, UDP failure, worker-thread exception, and bounded join timeout.
- Metronome and transport remain engine-sample-time authoritative.
- Existing Python stress and two-host benchmark workflows remain usable.

## Measurements Worth Retaining

When useful for diagnosing a material fast-path change, retain before/after
results for:

- Allocations per audio packet and allocations/sec.
- Bytes copied per packet.
- Packet-loop CPU time and total process CPU.
- Send interval minimum, average, maximum, and high-percentile gaps.
- Receive-loop gaps.
- Callback interval/gap counters and xruns.
- Packet loss, duplicates, reorder, late packets, and jitter.
- Playback depth, missing frames, drops, underruns, and overruns.
- Drift ppm and resampler ratio.
- Per-peer and aggregate packet rate and bitrate.
- Fixed storage occupancy, high-water marks, and capacity drops.
- Startup and shutdown time.

Stable design properties:

- No regression in audio correctness, authentication, session rejection, metronome/transport timing, recording alignment, or raw diagnostics.
- No unbounded packet, mix, control, or asset storage.
- No allocation, locks, logging, exceptions, blocking, or file work in the audio callback.
- Two-person behavior remains the simplest and best-tested path.
- Three/four-peer mesh remains direct, measurable, and supported without hidden relays or automatic recommendations.
- A microbenchmark improvement does not by itself outweigh worse callback or
  packet-loop tail timing; investigate the full technical evidence and audible
  behavior.

## Design Context Summary

- Preserve playback-ring ownership, bounded peer-aware mixing, wrap-safe
  sequencing, remote-control/asset limits, and precise technical counters.
- Keep endpoint resolution, packet buffers, authentication, PCM conversion,
  reorder/jitter storage, network waiting, and ring copies predictable and
  allocation-light.
- Complete the typed shared application boundary, remove process-compatibility
  paths, and split the engine and GUI monoliths along ownership boundaries.
- Retain the implemented session-wide PCM16/PCM24 choice, compact 36-byte UDP
  header, and authenticated binary asset chunks as the one current protocol;
  do not restore the removed wire or base64 compatibility paths.

The largest predictable bandwidth gain is PCM16. The largest predictable packet-processing gain is eliminating packet-rate allocation and endpoint conversion. The change most likely to improve visible timing measurements immediately is replacing the double wait with deadline-aware network scheduling. The single-binary refactor offers the largest maintainability and workflow gain, but should follow stabilization rather than being used as a substitute for it.
