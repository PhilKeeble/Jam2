# Jam2 Capture Roadmap

## Current State

`jam2-capture` exists as a separate CMake app in `apps/jam2-capture`.

Implemented first pass:

- Builds as C++20/CMake target `jam2-capture`.
- Writes the executable to repo-root `release`.
- Reuses the existing Jam2 host audio backend and device enumeration.
- Supports `list-devices`.
- Supports `record-input` for selected device/input channels, sample rate, buffer size, duration, and output path.
- Supports Windows WASAPI `list-loopback-sources`.
- Supports Windows WASAPI `record-loopback` from default or numeric render endpoints.
- Supports macOS 14.2+ CoreAudio process-tap `list-loopback-sources` and `record-loopback --source default`.
- Writes mono PCM16 WAV files.
- Sums captured input/loopback audio to mono before WAV output.
- Supports trigger, hold, pre-roll, leading silence trim, and trailing silence trim on input and loopback capture.
- Writes a `.wav.json` capture sidecar and can print a `capture.summary` JSON line for GUI parsing.
- Supports a stdin `stop` command for clean shorter captures on the main capture paths.
- Prints device, sample rate, buffer size, duration, frames written, peak level, capture ring overruns, and output path.
- Does not require a peer, TCP connection, UDP socket, or live `jam2` session.
- Keeps capture outside the live UDP audio path.
- `jam2-gui` Track tab can launch `jam2-capture record-input` or `record-loopback`, configure trigger/pre-roll/trim, refresh loopback sources, stop capture, parse the capture summary, load sidecar metadata, import the captured WAV as current track metadata, and share imported WAVs over the GUI TCP control plane.

Current CLI shape:

```text
jam2-capture list-devices
jam2-capture record-input --audio-device 5 --input-channels 1 --sample-rate 44100 --buffer-size 128 --duration-ms 30000 --output riff.wav
jam2-capture list-loopback-sources
jam2-capture record-loopback --source default --duration-ms 30000 --output system.wav
jam2-capture record-loopback --source default --duration-ms 30000 --trigger on --pre-roll-ms 250 --trim-trailing-silence on --output system.wav
```

## Near-Term Capture Work

- Validate ASIO input capture on Windows with real hardware.
- Validate CoreAudio input capture on macOS with real hardware.
- Add clearer failure messages for invalid devices, invalid channels, unavailable sample rates, driver open failures, and output path errors.
- Add richer sidecar metadata where useful: source device name/id, selected channels, timestamp, and backend-specific drop/xrun counters.

## Sound Trigger And Trim

Implemented CLI options:

```text
--trigger off|on
--trigger-threshold-db -45
--trigger-hold-ms 50
--pre-roll-ms 250
--tail-silence-db -50
--tail-silence-ms 1000
--trim-leading-silence on|off
--trim-trailing-silence on|off
```

Current rules:

- Triggered capture starts armed and writes only after input level crosses the threshold for the hold duration.
- A pre-roll buffer is preserved so the first transient is not clipped.
- Leading/trailing trim uses measured level.
- Capture summary reports pre-roll used, trimmed leading/trailing frames, final duration, peak, and output path.

Future:

- Optional auto-stop after tail silence if fixed duration is too awkward for quick idea capture.
- Add explicit trigger/tail timestamps if needed for debugging capture behavior.

## Loopback Capture Follow-Up

Windows WASAPI loopback and macOS 14.2+ CoreAudio process-tap loopback exist in the first pass.

Remaining platform work:

- macOS older than 14.2: use virtual devices such as BlackHole/Loopback through `record-input`.
- macOS 14.2+: add per-process filtering later if needed; current implementation records the default system mix.
- Linux: future PipeWire/PulseAudio monitor source if Linux support is added.

Rules:

- Record loopback output to WAV.
- Make source names and active sample format visible.
- Report duration, peak level, underruns/drops where available, and output path.
- Keep loopback capture separate from ASIO/CoreAudio input capture.

## Optional Capture Analysis

Add after the Essentia investigation pass.

Planned CLI:

```text
jam2-capture record-input --audio-device 5 --input-channels 1 --output riff.wav --analyze on
jam2-capture record-loopback --source default --output backing.wav --analyze on
```

Behavior:

- Analyze final WAV offline after recording and trim.
- Detect approximate BPM, key, and chord suggestions where practical.
- Write analysis sidecar metadata next to the WAV.
- Treat analysis values as suggestions that the GUI can display and users can correct.
- Corrected GUI values become project metadata and should not overwrite raw analysis unless explicitly exported.

## File Formats

Current:

- WAV PCM16 mono.

Future:

- Stereo or selected-channel-count WAV if needed.
- 24-bit or float WAV if useful for higher-quality capture.
- MP3 export only through an optional encoder if useful.
- FLAC export if smaller lossless files become useful.

Keep file-format support separate from the live UDP audio protocol.

## Test Checklist

- `jam2-capture list-devices` matches `jam2 list-devices`.
- ASIO input capture records the selected channel(s) on Windows.
- CoreAudio input capture records the selected channel(s) on macOS.
- Captured WAV files open in common audio tools.
- Captured WAV files import into `jam2-gui` Track tab.
- Capture can run without starting a live jam.
- Capture failures report clear device/source/path errors.
- Windows loopback captures system/app playback.
- macOS 14.2+ loopback captures system/app playback with CoreAudio process taps.
