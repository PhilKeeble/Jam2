# Jam2 MVP Implementation Plan

## Summary

Build a lightweight two-person CLI app for ultra-low-latency music streaming over direct UDP. The MVP targets 24-bit mono audio between exactly two peers, with no relay servers, no rooms, and no GUI. Connection setup uses public STUN only to discover the listener's public UDP endpoint; the app prints a `jam2:` connection string that the second peer uses to connect directly. Manual port-forwarding mode remains available for users who do not want STUN or whose NAT does not cooperate.

Primary goals:

- Lowest practical one-way audio latency.
- Fixed, efficient binary UDP protocol.
- Clear CLI connection state and hard technical stats.
- Windows support via ASIO only.
- macOS support via CoreAudio only.
- Tunable runtime options for buffer sizes, packet sizes, jitter buffer, drift correction, and delay-correction experiments.

## Tech Stack

- Language: C++20.
- Build system: host-native CMake for Windows and macOS builds.
- Windows audio: ASIO SDK only; no WASAPI backend for MVP.
- macOS audio: CoreAudio AudioUnit/HAL.
- Networking: native UDP sockets with platform wrappers.
- STUN: minimal client implementation for Binding Request/Response.
- Crypto/auth: lightweight keyed packet authentication using a small dependency such as libsodium, or a compact BLAKE3 keyed MAC if dependency footprint is preferred.
- Resampling/drift correction: dedicated lightweight resampler, initially simple linear or cubic interpolation, replaceable later with higher-quality implementation.
- CLI parsing: small dependency such as CLI11, or minimal custom parser if dependency count should stay low.

Build workflow:

- Windows: MSVC, CMake, Ninja, and the ASIO SDK when the ASIO backend is implemented.
- macOS: Xcode Command Line Tools, CMake, and Ninja.
- Docker is not part of the normal workflow because ASIO/CoreAudio latency and callback behavior must be tested on host OS audio drivers and hardware.
- Local Windows ASIO SDK path for this workspace: `C:\Tools\ASIO-SDK_2.3.4\ASIOSDK`. CMake should use this as the fallback path when `ASIO_SDK_DIR` is not set.

## Connection Model

The app has two primary connection flows.

STUN-assisted listener flow:

```bash
jam2 listen --stun stun.l.google.com:19302 --device "ASIO Fireface USB"
```

The listener:

- Opens a UDP socket.
- Sends a STUN Binding Request.
- Discovers its public UDP endpoint.
- Generates a random session id and authentication key.
- Prints a single connection string.

Example output:

```text
Mode: listen
STUN: stun.l.google.com:19302
Local UDP bind: 0.0.0.0:49000
Public endpoint: 203.0.113.10:49152
Connection string:
jam2://v1?endpoint=203.0.113.10:49152&session=...&key=...

Waiting for peer...
```

Connector flow:

```bash
jam2 connect "jam2://v1?endpoint=203.0.113.10:49152&session=...&key=..." --device "ASIO Fireface USB"
```

The connector:

- Parses the `jam2:` string.
- Sends authenticated UDP handshake packets to the listener endpoint.
- Starts audio streaming after handshake confirmation.

Manual port-forward flow:

```bash
jam2 listen --no-stun --public-endpoint 203.0.113.10:49000 --device "ASIO Fireface USB"
jam2 connect "jam2://v1?endpoint=203.0.113.10:49000&session=...&key=..." --device "ASIO Fireface USB"
```

Rules:

- No relay support.
- No rendezvous/signaling server.
- Only one active peer at a time.
- Listener locks to the first authenticated peer.
- Packets from other endpoints are ignored and counted.
- STUN is used only for endpoint discovery and is never in the audio path.

## Lightweight Protocol

Use a fixed binary UDP protocol with separate control and audio packet types.

Packet types:

- `HELLO`
- `HELLO_ACK`
- `AUDIO`
- `PING`
- `PONG`
- `METRONOME_STATE`
- `BYE`

Common header fields:

- Magic/version.
- Packet type.
- Flags.
- Session id.
- Sequence number.
- Local sample timestamp.
- Monotonic send timestamp.
- Payload length.
- Authentication tag.

Audio payload:

- 24-bit signed PCM.
- Mono.
- Little-endian packed 3-byte samples.
- Default sample rate: `48000`.
- Default frame size: `128` samples.
- Supported frame sizes: `32`, `64`, `128`, `256`.

Default audio mode:

- Fastest possible remote playback.
- No software monitoring requirement.
- Users are expected to direct-monitor locally.

Packet validation:

- Reject wrong magic/version.
- Reject wrong session id.
- Reject failed authentication tag.
- Reject unexpected endpoint after session lock.
- Track but do not crash on malformed, late, duplicate, or out-of-order packets.

## CLI Options

Required or commonly used:

```text
listen
connect <jam2-url>
--device <name-or-id>
--list-devices
--stun <host:port>
--no-stun
--public-endpoint <ip:port>
--bind <ip:port>
--sample-rate 48000
--frame-size 32|64|128|256
--jitter-us <microseconds>
--stats-interval-ms <milliseconds>
--log-stats <path.csv>
```

Tuning options:

```text
--socket-send-buffer <bytes>
--socket-recv-buffer <bytes>
--drift-correction on|off
--delay-correction off|symmetric
--delay-correction-target-us <microseconds>
--debug-input-meter
--debug-loopback-local
--debug-record-input <path.wav>
--debug-record-recv <path.wav>
```

Metronome options:

```text
--metronome off|on
--bpm <number>
```

Runtime stdin commands:

```text
stats
stats reset
bpm <number>
metro on
metro off
quit
```

## Implementation Stages

Current implementation checkpoint:

- Stage 1 skeleton/CLI is partially complete: host-native CMake/MSVC/Ninja build, `--help`, command dispatch, and device listing entry point exist.
- Stage 2 is complete for the MVP slice: UDP socket wrapper, `jam2://v1` URL generation/parsing, authenticated `HELLO`/`HELLO_ACK`, peer lock, and local two-process handshake validation.
- Stage 3 is complete for the MVP slice: minimal STUN Binding Request/Response parsing, XOR-MAPPED-ADDRESS extraction, retry/timeout behavior, `--stun`, `--no-stun`, and `--public-endpoint`.
- Stage 4 Windows ASIO slice is partially complete: ASIO registry enumeration, driver probe, input meter, buffer/callback validation, and duplex ASIO stream with callback-safe ring handoff. macOS/CoreAudio is not implemented yet.
- Stage 5 is partially complete: fixed AUDIO packets, packed 24-bit mono PCM helpers, timed UDP packet exchange, RTT and receive-delay stats in milliseconds, coarse AUDIO interarrival variance stats, sequence/loss tracking, preallocated capture/playback rings, one-sided ASIO-to-UDP integration on localhost, explicit playback prefill gating, and ring depth/prefill reporting in frames and milliseconds. Remaining Stage 5 work is real two-peer live audio validation, adaptive jitter buffering, tighter packet pacing, and longer stability testing.

### Stage 1: Project Skeleton and CLI

Create the C++20 project structure, CMake build, logging, CLI parsing, platform abstraction boundaries, and common types.

Deliverables:

- ~~`jam2 listen`~~
- ~~`jam2 connect`~~
- ~~`--help`~~
- ~~`--list-devices`~~
- common config model
- structured terminal output

No real audio streaming required yet. Basic command dispatch is implemented; remaining polish is structured config/output cleanup.

### Stage 2: UDP Session and `jam2:` URL

Implement:

- ~~UDP socket abstraction.~~
- ~~Session id/key generation.~~
- ~~`jam2:` URL generation and parsing.~~
- ~~Authenticated `HELLO` / `HELLO_ACK`.~~
- ~~Peer lock after first valid authenticated handshake.~~
- ~~Clear ignored-packet counters.~~

Acceptance:

- ~~Two local processes can handshake over `127.0.0.1`.~~
- ~~Listener prints connection state transitions.~~
- ~~Wrong session/key packets are rejected.~~

### Stage 3: STUN Endpoint Discovery

Implement minimal STUN client:

- ~~Binding Request.~~
- ~~Binding Response parsing.~~
- ~~XOR-MAPPED-ADDRESS extraction.~~
- ~~Timeout and retry handling.~~
- ~~`--stun`, `--no-stun`, and `--public-endpoint`.~~

Acceptance:

- ~~Listener can print discovered public endpoint.~~
- ~~Manual endpoint mode works without contacting STUN.~~
- ~~STUN failure gives a clear CLI error and suggests manual mode.~~

### Stage 4: Audio Device Backends

Implement platform audio backends:

- ~~Windows: ASIO device enumeration, input/output open, buffer-size selection.~~
- macOS: CoreAudio device enumeration, input/output open, buffer-size selection.
- ~~Mono capture path.~~
- ~~Remote playback path.~~
- ~~No allocations or logging in real-time audio callbacks.~~

Acceptance:

- ~~Device list shows usable device names/ids.~~
- ~~Selected device opens at requested sample rate where supported.~~
- ~~Input meter can show signal level for debugging.~~
- Remaining: CoreAudio backend and real hardware validation beyond ASIO4ALL.

### Stage 5: 24-bit UDP Audio Streaming

Implement:

- ~~Capture-to-network ring buffer.~~
- ~~Network packetizer.~~
- ~~AUDIO packet send/receive.~~
- Jitter buffer.
- ~~Receive-to-output ring buffer.~~
- ~~24-bit mono PCM packing/unpacking.~~
- Configurable frame size and jitter target.

Acceptance:

- Two peers can stream mono audio over LAN.
- Stats show packet rate, bitrate, loss, jitter, buffer depth, underruns, and overruns.
- Audio path remains stable for at least 10 minutes on a clean LAN.
- Completed so far: localhost authenticated AUDIO packet exchange, RTT/receive-delay stats in ms, coarse AUDIO interarrival variance stats in ms, one-sided ASIO-to-UDP ring integration, visible ring underrun/overrun counters, `--playback-prefill-frames` startup gating, and ring depth/prefill stats in both frames and ms.

### Stage 6: Drift Correction

Implement:

- Remote sample timestamp tracking.
- Local playout position tracking.
- Drift estimate in ppm.
- Adaptive resampler ratio.
- `--drift-correction on|off`.

Acceptance:

- Long sessions do not slowly underrun/overrun due to clock mismatch.
- CLI reports drift ppm and current resampler ratio.
- Disabling drift correction is possible for comparison testing.

### Stage 7: Stats and CSV Logging

Implement hard technical stats only:

- Peer endpoint.
- Connection mode: STUN/manual.
- Sample rate.
- Frame size.
- Packet rate.
- Send/receive bitrate.
- Packet loss count and percent.
- Late packet count.
- Dropped packet count.
- Out-of-order packet count.
- Jitter min/avg/max.
- RTT.
- Jitter buffer depth.
- Underruns/overruns.
- Drift ppm.
- Resampler ratio.
- Audio callback xruns.
- Input peak/RMS when enabled.

Acceptance:

- Terminal stats refresh at configured interval.
- `stats reset` clears counters.
- CSV output is suitable for comparing tuning runs.

### Stage 8: Metronome Sync

Implement local metronome generation:

- Control packets send metronome state.
- Peers generate click locally.
- Runtime BPM edits without restarting.
- `metro on`, `metro off`, and `bpm <number>` commands.

Acceptance:

- Both peers hear locally generated metronome aligned to the shared session timeline.
- BPM can change while connected.
- Metronome audio is not streamed as normal audio payload.

### Stage 9: Experimental Delay Correction

Implement optional delay-correction mode:

```text
--delay-correction off
--delay-correction symmetric
--delay-correction-target-us <microseconds>
```

Behavior:

- Default remains fastest possible remote playback.
- Symmetric mode intentionally delays playback to align peers to a shared timing target.
- The app reports the applied correction as hard data.

Acceptance:

- Delay correction can be enabled/disabled from startup config.
- Applied delay is visible in stats.
- No automatic playability scoring or subjective recommendations are added.

## Testing Plan

Connection tests:

- Parse valid and invalid `jam2:` URLs.
- Reject wrong session ids and wrong auth keys.
- Lock to first authenticated peer.
- Ignore unauthenticated packets and packets from unexpected endpoints.
- Verify local UDP handshake between two processes.

STUN tests:

- Parse STUN Binding Responses.
- Extract XOR-MAPPED-ADDRESS.
- Handle timeout, malformed response, and unsupported address family.
- Confirm `--no-stun --public-endpoint` bypasses STUN.

Protocol tests:

- Encode/decode all packet types.
- Validate header sizes and payload lengths.
- Validate 24-bit PCM packing/unpacking.
- Detect duplicate, late, lost, and out-of-order packets.

Audio tests:

- Enumerate ASIO/CoreAudio devices.
- Open selected device by id/name.
- Confirm no allocations in audio callback paths where practical.
- Verify stable streaming on LAN at frame sizes `64` and `128`.
- Verify debug input meter and optional recording.

Performance tests:

- Measure packet send interval stability.
- Measure jitter buffer underruns/overruns under simulated jitter/loss.
- Compare drift correction on/off in long-running sessions.
- Export CSV stats for tuning runs.

Manual acceptance scenarios:

- STUN listener prints usable `jam2:` URL.
- Connector establishes direct UDP session from pasted URL.
- Manual port-forward mode works with provided public endpoint.
- Wrong peer traffic is ignored.
- Runtime BPM changes without restarting.
- App exits cleanly with `quit` or Ctrl+C.

## Assumptions and Defaults

- MVP supports only two peers.
- MVP supports only Windows and macOS.
- Windows audio backend is ASIO only.
- macOS audio backend is CoreAudio only.
- Default sample rate is `48000`.
- Default audio format is packed 24-bit mono PCM.
- Default frame size is `128` samples.
- Default mode is fastest possible remote playback.
- Drift correction is enabled by default.
- Delay correction is off by default.
- STUN default is configurable and may start as `stun.l.google.com:19302`.
- Public STUN is used only for endpoint discovery, never audio.
- No relay/TURN support is included.
- No hosted rendezvous/signaling server is included.
- No subjective playability score is included.
- Stats must expose hard technical data for tuning.
- Local Windows test environment has ASIO4ALL installed for initial ASIO enumeration and rough streaming tests; vendor ASIO drivers remain preferred for serious latency testing.
