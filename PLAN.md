# Jam2 Plan

## Things To Test

- Validate real two-peer live audio on separate machines over LAN, one-Wi-Fi, and both-Wi-Fi setups.
- Run longer sessions with real audio hardware to confirm drift correction does not slowly underrun, overrun, or sound unstable.
- Compare drift correction on/off and different `--drift-deadband-ppm` values using CSV stats.
- Validate the current ASIO path with a real vendor ASIO driver, not only ASIO4ALL.
- Validate CoreAudio behavior on macOS with real full-duplex hardware.
- Test stable streaming at practical frame sizes, especially `64`, `128`, and `256`.
- Test runtime controls in real sessions: `stats`, `status`, `metro on`, `metro off`, `metro mode <mode>`, `metro level <number>`, `remote level <number>`, `bpm <number>`, `quit`, and Ctrl+C.
- Confirm manual port-forward mode and STUN-assisted mode both work across real networks.
- Confirm wrong session/key packets and packets from unexpected endpoints are ignored and counted.
- Use benchmark/adaptive logs to compare profiles by packet loss, jitter, playback depth, underruns, overruns, drift ppm, resampler ratio, and estimated one-way latency.
- Validate metronome behavior in real sessions after shared-timeline refinement: startup alignment, BPM changes, on/off propagation, and first-beat accenting.
- Validate sample-time-aware playout in real sessions: missing sample insertion, late frame drops, playout delay error, and whether packet drops no longer permanently shift remote timing.
- Compare `shared-grid`, `leader-audio`, `symmetric-delay`, and `listener-compensated` metronome modes in real sessions using raw alignment and playout stats.
- Validate adaptive playback cushion in wired, one-Wi-Fi, and both-Wi-Fi sessions using target changes, padding frames, time above/under target, underruns, drops, and added latency.

## Things To Refine

- Tune the drift correction loop against real hardware clocks, including smoothing, deadband, max correction, and audible behavior during long runs.
- Keep OS-level packet scheduling and send pacing under review if real-session stats show avoidable burstiness.
- Continue refining stats and CSV output only where it exposes hard tuning data. Avoid subjective playability scores or hidden recommendations.
- Keep CLI/runtime controls numeric and inspectable. Defaults can be conservative, but aggressive tuning should remain explicit.
- Preserve the current product scope: two peers, direct UDP, no rooms, no relay/TURN audio path, no accounts, and no GUI layer unless explicitly chosen later.

## Potential Future Plans

### Local Shared Track Mix Source

Add an engine-side local playback source that `jam2-gui` can control for shared backing tracks.

Possible runtime command shape:

```text
track load <path-or-cache-id>
track play <start-frame>
track stop
track level <0..1>
track level +0.05
track level -0.05
```

Rules:

- The track source is local-only by default and should not be sent over the live UDP audio stream.
- Mix the track into local output through the same ASIO/CoreAudio device as remote peer audio and metronome.
- Keep track level independent from remote peer playback level and metronome level.
- Decode/file I/O must stay outside the real-time callback; prebuffer decoded audio or use a callback-safe handoff.
- Report track loaded/playing state, track level, underruns, decode errors, and current playback frame in machine-readable status and CSV where useful.
- Let `jam2-gui` handle TCP file transfer, file readiness, shared countdowns, and synchronized playback commands.

### Jam Output Recording

Add optional recording inside `jam2` for the local jam output mix. This is separate from `jam2-capture`, which records external/local sources for import.

Possible runtime command shape:

```text
record output start <path.wav>
record output stop
record output status
record stems start <folder>
record stems stop
record stems status
```

Rules:

- Record the local output mix before it reaches ASIO/CoreAudio output.
- Include remote peer audio, metronome, and local shared track playback when those sources are active.
- Do not rely on OS loopback capture for jam output, especially on Windows ASIO where system loopback may not see the signal.
- Keep file I/O out of the real-time callback by using a callback-safe handoff to a writer thread.
- WAV should be the first supported output format.
- Support optional stem recording so users can import the jam into a DAW and remove or rebalance parts later.
- First useful stem set: `mix.wav`, `my-input.wav`, `their-input.wav`, `inputs-mix.wav`, `metronome.wav`, and `shared-track.wav` when active.
- `my-input.wav` should record the local captured input before it is sent to the peer.
- `their-input.wav` should record the decoded remote peer audio as heard locally after receive/playout processing.
- `inputs-mix.wav` should record local input plus remote peer audio without metronome or shared backing track.
- `mix.wav` should record the full local monitoring mix, including inputs, metronome, and shared track when active.
- Stems must stay sample-aligned so they line up correctly in a DAW.
- Missing/inactive stems should be omitted or written as silence according to an explicit option.
- Report recording state, output path, written frames, dropped writer frames, and file errors in machine-readable status/final stats.

### Custom Metronome And Rhythm Training

Add future metronome controls that go beyond a fixed quarter-note click.

Possible CLI/runtime shape:

```text
--time-signature 4/4
--metronome-pattern <pattern>
--metronome-subdivision quarter|eighth|sixteenth|triplet

metro time 7/8
metro pattern <pattern>
metro subdivision sixteenth
```

Rules:

- Support time signatures such as `4/4`, `3/4`, `6/8`, `7/8`, and other explicit numerator/denominator values.
- Support configurable accent patterns, including first-beat accent, offbeat accents, clave-like patterns, and user-defined beat/subdivision accents.
- Support different click sounds or levels for strong accent, weak accent, and subdivision clicks.
- Keep patterns explicit and numeric/textual rather than inferred.
- Expose active time signature, pattern, subdivision, beat index, bar index, and accent index in stats.
- Keep click generation callback-safe: no allocation, logging, file I/O, locks, or blocking in the audio callback.
- Maybe explore whether `jam2-gui` Beat View/drum-grid patterns should be reusable as metronome accent/rhythm-training patterns. Do not assume this is required until real use shows it is useful.
- Preserve simple default behavior: 4/4 quarter-note click with first-beat accent.

### Experimental Delay Correction

Add optional delay correction only as an explicit mode:

```text
--delay-correction off
--delay-correction symmetric
--delay-correction-target-us <microseconds>
```

Rules:

- Default remains fastest possible remote playback.
- Symmetric mode intentionally delays playback to align peers to a shared timing target.
- Applied delay must be visible in stats.
- No automatic playability scoring or subjective recommendations.

### Future: Small Full-Mesh Group Mode

Consider an experimental mode for 3 people first, with 4 people only after the 3-peer path is stable. The goal is to preserve the current lowest-latency direct UDP design by using a small full mesh instead of a relay, TURN audio path, room server, or central mixer.

Topology:

- Each participant sends captured mono audio directly to every other participant.
- Each participant receives one independent remote stream per peer.
- Each client mixes remote streams locally into the mono playback output.
- The listener may coordinate the initial peer list, but it must not become an audio relay or hosted mixer.

Latency rules:

- Do not wait for all peers' packets to align before playback.
- Keep each remote peer on independent reorder buffering, playback depth, drift correction, underrun handling, and stats.
- If one peer is late or unstable, that peer should contribute silence, drops, or drift correction independently while stable peers continue at their lowest usable latency.
- Mixing remote mono streams should happen in the audio callback from already-prefilled per-peer playback rings, using preallocated scratch buffers and no allocation, logging, locks, throws, or blocking operations.

Expected tradeoffs:

- Upload and receive bandwidth scale with `N - 1`.
- CPU cost for summing 2 or 3 remote mono streams should be negligible compared with network jitter, audio device latency, packet scheduling, and drift correction.
- Four-peer mode increases packet rate and uplink pressure enough that Wi-Fi or weak uplinks may require larger playback prefill values.
- The worst individual peer link can affect what that peer hears and contributes, but should not force additional delay onto other stable peer streams.

Possible CLI shape:

```text
jam2 listen --max-peers 3
jam2 connect "<jam2-url>"
```

Rules:

- Keep mono PCM as the first network format.
- Require all peers to match sample rate and frame size.
- Prefer 3-peer LAN validation before internet testing or 4-peer experiments.
- Expose per-peer hard stats: endpoint, packet loss, jitter, RTT, bitrate, playback depth, underruns, overruns, drift ppm, and resampler ratio.
- Do not add rooms, accounts, GUI, subjective playability scores, or hidden recommendations.

### Future: Tuning Profiles

Add named profiles only after enough real two-machine test data exists to choose useful numeric values.

Possible CLI shape:

```text
--profile stable
--profile low-latency
--profile aggressive
```

Rules:

- Profiles should expand to explicit numeric values for frame size, audio buffer size, playback prefill, ring size, and drift correction limits.
- The app must print the actual numeric values after applying a profile.
- Explicit user-provided numeric flags should override profile defaults.
- Do not hide tuning behavior behind subjective labels without exposing the hard data.

### Full Jitter Buffer

The current receive path has a small reorder buffer that can recover packets arriving slightly out of order. A full jitter buffer would intentionally maintain a target playout depth and schedule packets by sequence/sample time.

Possible CLI shape:

```text
--jitter-buffer-frames 0
--jitter-buffer-frames 256
--jitter-buffer-max-frames 1024
```

Rules:

- Keep this optional; it may help 64-frame packet mode, but it adds latency.
- Default should remain explicit and conservative.
- Stats must show target depth, actual depth, reordered recovered, reordered lost, dropped frames, underruns, and added latency.
- Do not add hidden adaptive behavior until fixed numeric behavior is well understood.

### OS Scheduling And Priority Experiments

Add OS scheduling and cadence stats even before changing priorities, so benchmark CSVs can show whether bursts are likely caused by local scheduling stalls, audio callback gaps, network jitter, or playback-buffer pressure. Priority changes should then be explicit startup options for testing whether those measured local stalls can be reduced. Active settings should be reported in startup output, periodic CSV rows, and final stats.

Possible CLI shape:

```text
--process-priority normal|high|realtime
--stream-thread-priority normal|high|realtime|mmcss
--timer-resolution default|1ms
--os-scheduling-stats off|on
```

Windows behavior:

- `--process-priority high` should request `HIGH_PRIORITY_CLASS`.
- `--process-priority realtime` should request `REALTIME_PRIORITY_CLASS` as an explicit experimental mode for controlled testing.
- `--stream-thread-priority high` should request an elevated stream/network thread priority such as `THREAD_PRIORITY_HIGHEST`.
- `--stream-thread-priority realtime` should request a realtime thread priority only when explicitly selected.
- `--stream-thread-priority mmcss` should register the stream thread with MMCSS, preferably using a suitable low-latency audio task profile such as `Pro Audio` or `Audio`.
- `--timer-resolution 1ms` should request 1 ms timer resolution, report success/failure, and release it on shutdown.

macOS behavior:

- Provide equivalent opt-in scheduling experiments where practical for the non-realtime stream/network thread.
- Test thread QoS classes such as user-initiated or user-interactive for stream/network work.
- Consider a real-time thread policy only if it can be applied narrowly and safely outside CoreAudio's own callback management.
- Do not fight CoreAudio's audio callback scheduling; keep callback work simple and non-blocking.
- Report which QoS or scheduling policy was requested and which was actually applied.

Stats to add for comparison:

- Add these stats even if process/thread priority options are not implemented yet.
- Include the fields in CSV output so benchmark analysis can identify where improvement is likely before testing priority changes.
- Requested and active process priority.
- Requested and active stream thread priority or QoS.
- MMCSS requested/active state and task profile on Windows.
- Timer resolution requested/active state on Windows.
- Stream loop wake lateness min/avg/max.
- Count of stream loop wakeups later than `1 ms`, `2 ms`, and one packet interval.
- UDP send schedule lateness min/avg/max.
- Receive loop gap min/avg/max.
- Audio callback interval min/avg/max.
- Audio callback gaps over expected callback interval, `1.5x`, and `2x`.
- Largest observed local scheduling stall in milliseconds.

Benchmark goals:

- Compare normal, high, MMCSS/QoS, realtime, and timer-resolution variants against the same network/audio profile.
- Use the added stats to separate local scheduling stalls from Wi-Fi/network jitter, audio callback stalls, and playback-buffer pressure.
- Keep realtime priority out of defaults and automation unless explicitly requested for the run.

### Additional Low-Latency Diagnostics

Continue adding diagnostic stats that make benchmark output better at explaining which part of the system should be tuned next.

Remaining packet/send timing:

- Add `socket_send_error_count` and, where measurable without blocking the hot path, `socket_send_delay_max_ms`.

Remaining receive loop timing:

- Add `late_packet_age_min_ms`, `late_packet_age_avg_ms`, and `late_packet_age_max_ms` if packet sample/send timing makes this reliable.

Remaining audio callback cadence:

- Add `audio_callback_interval_p99_ms` if a lightweight histogram or bucketed approximation is practical.
- Add `audio_callback_processing_time_max_ms` only if it can be measured without allocations, logging, locks, or blocking inside the real-time callback.

Playback depth shape:

- Add playback depth percentiles or bucketed approximations: `playback_depth_p01_ms`, `playback_depth_p05_ms`, `playback_depth_p50_ms`, `playback_depth_p95_ms`, and `playback_depth_p99_ms`.
- Add `time_under_target_depth_percent`, `time_over_target_depth_percent`, `longest_under_target_ms`, and `longest_over_target_ms` once a fixed or adaptive target exists.
- Add `playback_depth_near_empty_events` and `playback_depth_near_full_events`.
- Reason: min/avg/max and current 2/5/10 ms buckets are useful, but percentiles and target-relative time make it clearer whether a profile is barely stable or has real cushion.

Packet loss and reordering shape:

- Add `packet_loss_burst_events`, `packet_loss_burst_max_packets`, `consecutive_late_packet_max`, and `reorder_depth_max_packets`.
- Add `out_of_order_gap_max_packets` if it differs from the existing reorder-distance stat once the full jitter buffer is designed.
- Reason: total packet loss is less useful than loss burst shape for audio. These stats help decide whether to raise frame size, add jitter buffering, or increase playback cushion.

RTT shape:

- Add RTT percentiles or bucketed approximations: `rtt_p50_ms`, `rtt_p95_ms`, and `rtt_p99_ms`.
- Add `rtt_jitter_ms`.
- Keep any one-way estimate clearly labeled as an estimate.
- Reason: RTT average hides spikes. Percentiles help decide whether delay correction, adaptive cushion, or network changes are likely to help.

System context:

- Add active process priority, active stream thread priority or QoS, MMCSS state/profile on Windows, timer resolution state on Windows, CPU count, and power mode where available.
- Add optional platform-specific Wi-Fi/link information only if it can be gathered cheaply and clearly labeled.
- Reason: explains why two benchmark runs with the same jam2 settings may behave differently and helps identify OS or hardware conditions worth changing.

### Aggregate Device Helpers

Aggregation may help users who do not have one full-duplex hardware device, but it should remain outside the core path until direct full-duplex testing is stable.

Notes:

- Windows users can often aggregate WDM devices through ASIO4ALL, but Jam2 should not try to script ASIO4ALL's control panel.
- macOS aggregate devices require sub-device UIDs, clock source selection, drift compensation choices, input/output enablement, naming, and cleanup.
- Aggregates can hide resampling and clock-drift behavior, which may make latency/stability stats harder to interpret.
- The recommended path remains one full-duplex ASIO/CoreAudio device where possible.

Possible future commands:

```text
jam2 list-aggregate-candidates
jam2 create-aggregate --name Jam2Aggregate --input-device <id> --output-device <id>
jam2 delete-aggregate --name Jam2Aggregate
```

Only add these after the direct full-duplex path is stable on real two-machine tests.

### Future: Linux Audio Backend

Consider Linux support after the Windows ASIO and macOS CoreAudio paths are stable. Linux should be treated as another host-native low-latency backend, not as a Docker or container target.

Backend approach:

- Start with ALSA direct hardware access for the smallest dependency footprint and most inspectable timing behavior.
- Use ALSA `snd_pcm` capture/playback devices in full-duplex mode where possible, configured for the requested sample rate, period size, buffer size, and signed 32-bit PCM if the device supports it.
- Run a dedicated non-real-time audio service thread around `poll`/`snd_pcm_wait` or mmap-style ALSA access, then hand audio to the existing capture/playback rings.
- Keep the real-time-sensitive ALSA loop free of allocation, logging, exceptions, locks on the hot path, and blocking work unrelated to device I/O.
- Add JACK or PipeWire support only if direct ALSA testing shows a concrete need. JACK can be useful on pro-audio Linux setups because it provides a graph callback model and central device clocking, but it adds runtime setup expectations. PipeWire is common on modern desktops, but its native API and session behavior are broader than Jam2 needs for a first Linux slice.

Possible CLI shape:

```text
jam2 list-devices
jam2 test-device <id> --sample-rate 48000 --audio-backend alsa
jam2 listen --audio-backend alsa --audio-device hw:2,0 --sample-rate 48000 --audio-buffer-size 128
```

Rules:

- Keep Linux builds host-native through CMake.
- Do not make PulseAudio the low-latency backend.
- Expose actual ALSA period size, buffer size, sample format, channel count, input/output latency frames, underruns, overruns, and xrun recoveries in stats.
- Prefer one full-duplex hardware device. If separate input/output devices are used later, expose the clocking and drift consequences clearly.
- Real validation must happen on Linux with the actual audio driver stack and hardware; build success alone is not meaningful latency validation.

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
