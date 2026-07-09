# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root.

## Shared Song Timing Ideas

The GUI already has shared song grids and local track playback. A possible future pass is tighter timing between the song grid, metronome, and any shared backing track.

### Option 2: GUI Shared Grid Timing

Keep track playback in the GUI for now, but give the GUI a real musical timebase that the Track, Chord, Beat, and Metronome views can all reference.

Expected benefits:

- Track playback can start on the next beat, bar, or explicit grid position instead of immediately when the button handler runs.
- Chord and beat views can show a small current-beat marker tied to the same grid as the metronome.
- The metronome does not have to be audible for the grid to exist; an off or muted metronome can still provide timing for visuals and scheduled track starts.
- Standalone mode can create a local GUI grid at startup, then reset the epoch when BPM, beats-per-bar, or division changes.
- Jam mode can follow the engine shared-grid model so both players can start the same local track at the same musical grid position.

Possible implementation shape:

- Add a small GUI song-clock model with BPM, beats-per-bar, division, epoch, running state, and a monotonic-clock-to-grid conversion.
- In standalone mode, initialize the GUI song clock when the GUI starts or when the first timing-dependent view is created.
- On BPM or metronome pattern changes, establish a new epoch explicitly rather than silently stretching old grid time.
- In jam mode, prefer the engine metronome epoch and pattern when available; do not invent a second competing jam grid.
- Add a `track.play.scheduled` control message that carries track position plus target grid beat or target session time, while keeping current immediate `track.play` behavior as the fallback.
- Add current-beat visual markers to chord and beat views without changing their editing model.
- Add optional beat/subdivision snap for Loop Start and Loop End only after audible track position is corrected.

Open decisions:

- Whether track starts should default to next beat, next bar, or immediate unless a lock option is enabled.
- Whether standalone grid should always run from GUI startup or only after the user enables metronome/grid timing.
- How much late-message tolerance is acceptable before a peer skips to the scheduled track position instead of starting late.
- Whether grid lock is global or a per-track option.

Measurement and stats:

- Expose hard timing data such as scheduled start target, actual local start time, estimated output queue delay, grid phase error, and peer start delta where practical.
- Do not add subjective playability scores.

Limitations:

- This improves musical coordination and visual consistency, but Qt track playback remains a separate audio stream.
- It is not sample-accurate with the engine output path and still depends on estimating output queue delay.
- The UDP audio protocol should not need to change for this option; GUI TCP control messages are the likely place for scheduled track commands.

Do not implement this without first deciding the exact GUI song-clock model and how scheduled starts should behave when a peer receives a command late.

## Network Audio Format Experiments

The current live UDP audio path uses mono PCM24 packets, while recording stems are written as PCM16 WAV files. A future tuning pass can add an explicit experimental PCM16 network mode to compare Wi-Fi behavior and audible quality against the current PCM24 default.

Goals:

- Keep PCM24 as the default network format unless measurements show a clear reason to change it.
- Add an explicit runtime option such as `--network-audio-format pcm24|pcm16`.
- Measure whether PCM16's smaller packets reduce Wi-Fi burst impact, late packets, jitter-buffer drops, missing audio frames, playback underruns, or RTT spikes.
- Check whether PCM16 causes any audible quality loss for direct instrument monitoring, metronome mixing, send-level changes, and recorded comparison runs.
- Expose the active network format, bytes per sample, packet payload bytes, and estimated audio bitrate in machine-readable stats/CSV.

Scope limits:

- Do not add a codec framework, negotiation layer, compression, or automatic quality switching for the first experiment.
- Keep packet parsing fixed-shape and allocation-light.
- Require both peers to use the same explicit format for the experiment; fail clearly on mismatch rather than guessing.

## Simple Looper / Multi-Track View

The current Track view is single-track. A future pass could turn it into a small grid-locked looper for stacking scratch parts during writing or a jam, without becoming a general DAW.

Intended use:

- Let players quickly stack parts such as guitar, bass, vocals, percussion, and scratch guide tracks.
- Use local input or loopback captures as new track items.
- Keep all items aligned to the shared song grid so verse, chorus, or idea sections can be tried while jamming.
- Support switching between small groups such as A and B, where each group is a different stack of looped parts for comparison.

Possible model:

- A looper project has a small number of groups, for example A and B at first.
- Each group has up to four visible lanes.
- Each lane can contain multiple track items that play at the same time on the shared timeline.
- Each track item stores WAV metadata, start grid position or start frame, gain, mute state, solo state, speed, pitch, loop enable, loop range, and file/cache identity.
- A group has a shared loop region, grid reference, and playback state.
- The global playhead is shared across lanes, with stacked waveforms drawn against the same time axis.

Basic controls:

- Add WAV, add last input capture, add last loopback capture, remove item.
- Per-lane or per-item mute, solo, level, and basic name/color.
- Existing speed and pitch controls may be reused through the current Signalsmith-based processing path.
- Global play, stop, seek, loop region, and optional snap-to-grid for starts and loop points.
- Group buttons such as A and B for switching between alternate stacks during a jam.

Scope limits:

- Do not add arbitrary clip editing, automation, plugins, comping, destructive trimming, or a full DAW timeline.
- Do not add accounts, hosted storage, rooms, relay audio, or cloud project sync.
- Keep the first useful version PCM WAV only.
- Prefer explicit numeric/grid data over subjective arrangement suggestions.
- Keep the UI focused on controlled testing and quick idea capture rather than production editing.

Interaction with Option 2:

- This feature should use the GUI shared-grid timing model from Option 2.
- Track items and groups should be positioned by grid/sample data, not by independent wall-clock starts.
- Group switching should be scheduled on a beat or bar when grid timing is enabled.
- Chord and beat views can use the same current-beat marker as the looper playhead.
- In jam mode, scheduled group starts should use the shared grid so both players can switch ideas together if they have the same local track assets.

Interaction with Option 3:

- A GUI-only looper should use one mixed playback device, not one `QAudioSink` per lane, to avoid each lane having a separate buffered output clock.
- If the looper becomes central to Jam2, engine-side track mixing becomes the cleaner long-term path.
- The data model should be designed so each lane/item can later map to an engine local track source without changing song files.
- Engine-side playback would provide better timing, shared metronome alignment, output recording, and stats for multi-track stacks.
- Standalone use should not require a peer; if moved engine-side later, the engine should support a local-only audio mode with network disabled.

Measurement and stats:

- Expose hard data such as group, lane count, active item count, playhead frame, output queue delay, scheduled group switch time, actual switch time, per-lane level, muted/solo state, underruns, and dropped/late scheduled starts.
- CSV or structured status output should remain suitable for comparing timing and alignment across runs.

Open decisions:

- Whether A/B groups are fixed two groups or a small configurable count.
- Whether mute/solo applies to lanes, individual items, or both in the first version.
- Whether captures are inserted at the audible playhead, next beat, next bar, or selected grid position.
- Whether group switching stops old audio immediately, crossfades, or switches only on the next loop boundary.
- Whether jam-mode peers share only metadata/control or also offer missing WAV files through the existing GUI TCP file transfer path.

## Engine-Side Shared Track Mix Source

`jam2-gui` can currently load, play, process, and share WAV files locally through Qt audio and the GUI TCP control plane. A possible future engine-side track source would mix a local backing track through the same ASIO/CoreAudio device path as the live jam output and metronome.

### Option 3: Engine Audio-Clock Track Playback

Move backing track playback out of the Qt `QAudioSink` path and into the `jam2` engine output mix. The GUI would still handle file selection, file transfer, metadata, and user controls, but the engine would own the track playout clock.

Expected benefits:

- Track, metronome, remote peer audio, local monitor, and output recording all share the same ASIO/CoreAudio callback clock.
- The track playhead can be reported as an engine sample frame instead of a Qt read-ahead cursor.
- Track start, stop, loop, and seek can be scheduled against the same sample-time model already used by the shared metronome.
- Jam recordings can include the local track stem or at least report exact track frame timing alongside metronome and remote audio.
- Output latency, underruns, decode failures, and current track frame can be exposed through the existing stats/status path.

Likely command shape:

```text
track load <path-or-cache-id>
track play <start-frame>
track stop
track level <0..1>
track level +0.05
track level -0.05
```

Additional possible commands:

```text
track seek <frame>
track loop on|off
track loop <start-frame> <end-frame>
track start-at <engine-sample-time> <track-frame>
track unload
```

Rules and constraints:

- The track source should remain local-only by default and should not be sent over the live UDP audio stream.
- Mix the track into local output through the same ASIO/CoreAudio device as remote peer audio and metronome.
- Keep track level independent from remote peer playback level and metronome level.
- Decode and file I/O must stay outside the real-time callback.
- Prebuffer decoded audio or use a callback-safe handoff.
- Report track loaded/playing state, track level, underruns, decode errors, and current playback frame in machine-readable status and CSV where useful.
- Let `jam2-gui` continue handling TCP file transfer, file readiness, shared countdowns, and synchronized playback commands.
- Keep the first version PCM WAV only unless a wider decoder solves a concrete local need.
- Do not add rooms, relays, accounts, or a hosted track path.
- Do not send mixed backing track audio over UDP unless explicitly requested later.

Possible implementation phases:

- Phase 1: engine can load a PCM16 WAV, predecode it off the callback thread, mix it locally, and expose current frame/status.
- Phase 2: GUI controls route through engine commands instead of direct `QAudioSink` playback.
- Phase 3: scheduled start uses engine sample time and shared metronome epoch for beat-aligned starts.
- Phase 4: optional recording/stats additions for track stem, track underruns, and exact start-frame comparisons.

Open decisions:

- Whether track audio should be recorded in output recordings by default, as a separate stem, or not at all.
- Whether the engine should accept file paths directly or only cache IDs prepared by the GUI.
- Whether speed and pitch controls remain GUI-only for now or require callback-safe engine resampling/time-stretching.
- Whether Option 2's GUI song clock should be built first and then reused to schedule engine track starts.

Measurement and stats:

- Expose current track frame, scheduled start sample time, actual first mixed sample time, loop start/end frames, track underruns, decode/prebuffer errors, and track level.
- Keep CSV output suitable for comparing timing runs.

Limitations:

- This is a larger architectural change than Option 2.
- It touches engine commands, status/stats, GUI control flow, and callback-safe audio mixing.
- It should not be started until the smaller GUI playhead/loop-position behavior is stable.

## Linux Audio Backend

Consider Linux support after the Windows ASIO and macOS CoreAudio paths are stable. Linux should be treated as another host-native low-latency backend, not as a Docker or container target.

Backend approach:

- Start with ALSA direct hardware access for the smallest dependency footprint and most inspectable timing behavior.
- Use ALSA `snd_pcm` capture/playback devices in full-duplex mode where possible.
- Configure the requested sample rate, period size, buffer size, and signed 32-bit PCM if supported by the device.
- Run a dedicated audio service thread around `poll`, `snd_pcm_wait`, or mmap-style ALSA access, then hand audio to the existing capture/playback rings.
- Keep the real-time-sensitive ALSA loop free of allocation, logging, exceptions, locks on the hot path, and blocking work unrelated to device I/O.
- Add JACK or PipeWire support only if direct ALSA testing shows a concrete need.

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
- Prefer one full-duplex hardware device.
- If separate input/output devices are used later, expose the clocking and drift consequences clearly.
- Real validation must happen on Linux with the actual audio driver stack and hardware; build success alone is not meaningful latency validation.
