# Jam2 Plan

## Things To Test

- Validate real two-peer live audio on separate machines over LAN, one-Wi-Fi, and both-Wi-Fi setups.
- Run longer sessions with real audio hardware to confirm drift correction does not slowly underrun, overrun, or sound unstable.
- Compare drift correction on/off and different `--drift-deadband-ppm` values using CSV stats.
- Validate the current ASIO path with a real vendor ASIO driver, not only ASIO4ALL.
- Validate CoreAudio behavior on macOS with real full-duplex hardware.
- Test stable streaming at practical frame sizes, especially `64`, `128`, and `256`.
- Test runtime controls in real sessions: `stats`, `metro on`, `metro off`, `bpm <number>`, `quit`, and Ctrl+C.
- Confirm manual port-forward mode and STUN-assisted mode both work across real networks.
- Confirm wrong session/key packets and packets from unexpected endpoints are ignored and counted.
- Use benchmark/adaptive logs to compare profiles by packet loss, jitter, playback depth, underruns, overruns, drift ppm, resampler ratio, and estimated one-way latency.
- Validate metronome behavior in real sessions after shared-timeline refinement: startup alignment, BPM changes, on/off propagation, and first-beat accenting.

## Things To Refine

- Tighten metronome shared-timeline alignment. Current behavior exchanges metronome state and generates local clicks, but does not yet provide a true shared epoch/phase model.
- Add runtime controls for metronome level so users can adjust click volume during a jam, not only through startup `--metronome-level`.
- Add runtime remote playback level so users can raise or lower what they hear from the other peer during a jam without changing the outgoing audio stream.
- Add engine support for GUI orchestration: allow `jam2 listen` to accept an explicit session id/key while preserving current headless behavior where omitted session args generate a new session.
- Add machine-readable startup/status output suitable for a controller process, so `jam2-gui` does not need to scrape human CLI text.
- Tune the drift correction loop against real hardware clocks, including smoothing, deadband, max correction, and audible behavior during long runs.
- Improve receive/playout behavior beyond the current small reorder buffer and FIFO playback ring. Future playout should become sample-time aware so packet drops or long bursts do not permanently shift remote audio timing.
- Keep OS-level packet scheduling and send pacing under review if real-session stats show avoidable burstiness.
- Continue refining stats and CSV output only where it exposes hard tuning data. Avoid subjective playability scores or hidden recommendations.
- Keep CLI/runtime controls numeric and inspectable. Defaults can be conservative, but aggressive tuning should remain explicit.
- Preserve the current product scope: two peers, direct UDP, no rooms, no relay/TURN audio path, no accounts, and no GUI layer unless explicitly chosen later.

## Potential Future Plans

### Runtime Mix Controls

Add runtime commands and audio-control state for levels that need to change during a live jam.

Possible CLI/runtime shape:

```text
--metronome-level <0..1>
--remote-level <0..1>

metro level <0..1>
metro level +0.05
metro level -0.05
remote level <0..1>
remote level +0.05
remote level -0.05
```

Rules:

- Metronome level should remain atomic and callback-safe, matching the existing metronome on/off/BPM control style.
- Remote playback level should affect only local monitoring of the peer's audio, not the outgoing audio sent to the peer.
- Apply remote playback gain in the audio callback after remote playback is popped/resampled and before metronome mixing.
- Report requested and final metronome/remote levels in startup output, final output, and CSV stats.
- Keep local remote-playback level independent per user; do not synchronize it over the network by default.

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

### GUI-Oriented Engine Hooks

Add small engine hooks that let a separate GUI process control `jam2` without merging GUI/control-plane behavior into the audio engine.

Possible CLI shape:

```text
--session-id <hex-or-u64>
--session-key <hex-16-byte-key>
--machine-readable-startup on|off
```

Rules:

- If `--session-id` and `--session-key` are provided to `jam2 listen`, use them for the printed/generated `jam2://` URL and UDP authentication.
- If either session argument is omitted, preserve current headless behavior and generate a fresh session id/key inside `jam2 listen`.
- Machine-readable startup output should expose connection URL, local/public endpoint, selected device, active sample rate, active buffer size, selected channels, and any startup error in a stable parseable shape.
- Runtime stdin commands remain the first control interface for `jam2-gui`: metronome on/off/BPM/level, remote playback level, stats, and quit.
- Do not add TCP, shared beat documents, GUI state, or lead-cue handling to the `jam2` audio engine.

### Metronome Modes

Add an explicit metronome behavior option so real sessions can compare the tradeoffs:

```text
--metronome-mode shared-grid
--metronome-mode leader-audio
--metronome-mode symmetric-delay
--metronome-mode listener-compensated
```

Candidate behavior:

- `shared-grid`: both peers generate local clicks from the same BPM, beat index, and session/sample-time epoch. This should be the default low-latency direction: both players trust the same grid, while remote audio is still heard late by network, playback, and device latency. This is best for known forms, count-ins, and equal peers who can play to the grid rather than react only to delayed incoming audio.
- `leader-audio`: the peer that starts or owns the metronome mixes the click into their outgoing audio stream. The other peer hears the leader's audio and click together, including the same packet loss, jitter, and playout delay. This is simpler and useful for leader/follower testing, but it makes the follower play to a delayed leader reference and the leader hears the follower roughly another one-way latency later.
- `symmetric-delay`: both peers intentionally delay playout against a common target so received audio can line up more closely with the local grid. This overlaps with delay correction and trades immediacy for tighter received-audio/click alignment. The target must cover one-way latency plus jitter and playback cushion, and applied delay must be visible as hard stats.
- `listener-compensated`: each peer shifts their local click so the incoming remote audio lands on that listener's click. This is useful as an experiment, but it is not recommended as the default because it creates a round-trip timing problem: each listener may hear remote audio aligned locally, while the two players are no longer truly playing to one shared grid.

Design constraints for metronome modes:

- Do not drive metronome phase from packet arrival timing. Click generation should stay local and stable unless a selected mode explicitly changes the playout timeline.
- Stats should expose metronome mode, grid source, epoch sample time, local/remote beat index, playout delay frames/ms, adaptive added latency, metronome phase error frames/ms, missing audio frames inserted, late audio frames dropped, and alignment-valid state.

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

### Sample-Time-Aware Playout

Move remote playback from FIFO-only behavior toward an explicit mapping:

```text
remote packet sample_time + playout_delay_frames -> local output sample position
```

Rules:

- Missing sample ranges should become silence or controlled concealment.
- Late frames should be counted or dropped rather than permanently shifting the stream.
- Packet arrival timing should inform diagnostics and buffer pressure, not directly redefine musical time.
- This should support metronome alignment and future adaptive cushion without hiding timing jumps.

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

### Adaptive Playback Cushion

Consider an explicit experimental mode for Wi-Fi burst handling before committing to a full jitter buffer. The goal is to keep a low normal playback target, such as `768` or `1024` frames, while temporarily allowing playback depth to grow when diagnostics show burst pressure or near-empty playback depth.

Possible CLI shape:

```text
--adaptive-playback-cushion on|off
--adaptive-playback-target-frames 768
--adaptive-playback-min-frames 768
--adaptive-playback-max-frames 2048
--adaptive-playback-release-ppm 1000
```

Rules:

- Default behavior remains fixed numeric prefill/max depth.
- Raise cushion quickly only after clear burst or low-depth evidence.
- Release extra latency slowly so players do not hear sudden timing jumps.
- Avoid sudden frame drops except when playback depth is far beyond the configured ceiling.
- Adaptive playback cushion should adjust an explicit `playout_delay_frames` target, not rely on accidental FIFO depth changes.
- If adaptive delay changes, metronome alignment should either move with the declared playout mapping or report alignment as invalid during recovery.
- Stats must expose current adaptive target, target raise/release events, burst-mode events, time above target, and estimated added latency.
- Keep thresholds numeric and inspectable; do not add subjective playability scoring.

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

Add the missing diagnostic stats below to make benchmark output better at explaining which part of the system should be tuned next.

Packet send timing:

- Add `send_interval_min_ms`, `send_interval_avg_ms`, and `send_interval_max_ms` to show whether jam2 is pacing outgoing audio packets evenly.
- Add `send_schedule_error_min_ms`, `send_schedule_error_avg_ms`, and `send_schedule_error_max_ms` to compare actual send time against intended send time.
- Add `send_catchup_events` and `send_catchup_max_packets` to detect local packet bursts caused by the send loop falling behind.
- Add `socket_send_error_count` and, where measurable without blocking the hot path, `socket_send_delay_max_ms`.
- Reason: separates bursts created locally by send pacing or scheduling from bursts caused later by Wi-Fi/network behavior.

Receive loop timing:

- Add `receive_loop_gap_min_ms`, `receive_loop_gap_avg_ms`, and `receive_loop_gap_max_ms`.
- Add `receive_burst_packets_max` and `receive_packets_per_loop_max` to show how many packets are drained after a gap.
- Add `late_packet_age_min_ms`, `late_packet_age_avg_ms`, and `late_packet_age_max_ms` if packet sample/send timing makes this reliable.
- Reason: distinguishes packets arriving late from jam2 reading packets late because the receive loop was not scheduled.

Audio callback cadence:

- Add `audio_callback_interval_min_ms`, `audio_callback_interval_avg_ms`, and `audio_callback_interval_max_ms`.
- Add `audio_callback_interval_p99_ms` if a lightweight histogram or bucketed approximation is practical.
- Add `audio_callback_gap_over_expected_count`, `audio_callback_gap_over_1_5x_count`, and `audio_callback_gap_over_2x_count`.
- Add `audio_callback_processing_time_max_ms` only if it can be measured without allocations, logging, locks, or blocking inside the real-time callback.
- Reason: shows whether audio driver/OS callback stalls line up with audible dropouts, ring underruns, or packet bursts.

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

Sample-time-aware playout:

- Add `expected_remote_sample_time`, `last_received_sample_time`, and `last_played_remote_sample_time` after playout tracks remote sample time explicitly.
- Add `remote_sample_lag_frames` and `remote_sample_lag_ms`.
- Add `missing_sample_ranges`, `missing_audio_frames_inserted`, `late_audio_frames_dropped`, `playout_delay_frames`, `playout_delay_ms`, `playout_delay_error_frames`, and `playout_delay_error_ms`.
- Reason: needed to make packet drops, late packets, adaptive cushion, jitter buffering, and metronome alignment measurable instead of inferred from FIFO depth.

System context:

- Add active process priority, active stream thread priority or QoS, MMCSS state/profile on Windows, timer resolution state on Windows, CPU count, and power mode where available.
- Add optional platform-specific Wi-Fi/link information only if it can be gathered cheaply and clearly labeled.
- Reason: explains why two benchmark runs with the same jam2 settings may behave differently and helps identify OS or hardware conditions worth changing.

### Stereo Stream Mode

Consider supporting both mono and stereo network audio payloads as a protocol/handshake feature, not only a local audio-device option.

Open decisions:

- Add an explicit `--stream-channels mono|stereo` option, or infer from `--input-channels`.
- Decide whether peers must match stream channel count exactly.
- Decide whether mono/stereo mismatch should be rejected at handshake or allowed with deterministic downmix/upmix.
- Keep payload format fixed-size and efficient for each mode.
- Expose bitrate and packet payload size clearly in stats.

Conservative first approach:

- Default remains mono.
- Handshake includes stream channel count.
- Reject mismatched channel count initially with a clear error.
- Add automatic mono/stereo conversion only later if real testing shows it is worth the complexity.

### Tuning Profiles

Add named profiles only after enough real two-machine test data exists to choose useful numeric values.

Possible CLI shape:

```text
--profile stable
--profile low-latency
--profile aggressive
```

Rules:

- Profiles should expand to explicit numeric values for frame size, audio buffer size, playback prefill, ring size, and drift correction limits.
- Print the actual numeric values after applying a profile.
- Explicit user-provided numeric flags should override profile defaults.
- Do not hide tuning behavior behind subjective labels without exposing hard data.

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
