# Jam2 Capture Plan

## Summary

Build `jam2-capture` as a separate utility for quickly creating audio files that can be loaded into `jam2-gui` for solo practice, shared tracks, stretching, pitch shifting, looping, or song sketching.

`jam2-capture` is not part of the live UDP jam path. It records external/local sources to files.

Primary capture modes:

- `record-input`: record an instrument/interface input.
- `record-loopback`: record what the OS/system is playing.

## Record Input Mode

Use this mode for solo idea recording, riffs, chord parts, or source material that should become a backing track or practice clip.

Windows behavior:

- Use ASIO capture for pro-audio interfaces.
- Reuse the existing Jam2 ASIO device enumeration, channel selection, sample-rate, and buffer-size code where practical.
- Record selected mono or stereo input channels to a file.
- Keep this separate from live `jam2` streaming.

macOS behavior:

- Use CoreAudio input capture.
- Support selected input channels where the CoreAudio backend allows it.

Linux behavior:

- Future option: PipeWire/ALSA input capture if Linux support is added.

Possible CLI shape:

```text
jam2-capture list-devices
jam2-capture record-input --audio-device 5 --input-channels 1 --sample-rate 44100 --output riff.wav
jam2-capture record-input --audio-device 5 --input-channels 1,2 --duration-ms 30000 --output idea.wav
```

Rules:

- WAV should be the first supported output format.
- MP3 export can be added later, but WAV is preferred for editing, stretching, and avoiding extra lossy artifacts.
- Print device, channel, sample rate, buffer size, duration, peak level, dropped/overrun counters, and output path.
- Do not require a peer, TCP connection, UDP socket, or live `jam2` session.
- Support optional sound-triggered recording so capture can arm first and start writing when input crosses a threshold.
- Support optional leading/trailing silence trim so captured ideas start close to the first played sound and end after the useful tail.

## Record Loopback Mode

Use this mode to capture system/app playback quickly so it can be imported into `jam2-gui` for practice and manipulation.

Windows behavior:

- Use WASAPI loopback capture from a selected render endpoint.
- This is for OS/app playback, not ASIO instrument input.

macOS behavior:

- Prefer documenting/supporting virtual audio devices such as BlackHole/Loopback first if direct system-output capture is not practical.
- Investigate newer macOS system-audio capture APIs only if OS version, permission, and deployment requirements are acceptable.

Linux behavior:

- Future option: PipeWire/PulseAudio monitor source capture.

Possible CLI shape:

```text
jam2-capture list-loopback-sources
jam2-capture record-loopback --source default --output reference.wav
jam2-capture record-loopback --source "Speakers" --duration-ms 60000 --output backing.wav
```

Rules:

- Record loopback output to WAV first.
- Make source names and active sample format visible.
- Report capture duration, peak level, underruns/drops where available, and output path.
- Keep loopback capture separate from ASIO input capture.
- Support the same optional sound-triggered recording and silence trim behavior as input capture.

## Sound Trigger And Trim

Add capture options that make quick idea recording easier:

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

Behavior:

- When trigger is enabled, capture starts in an armed state and does not write the file body until input level crosses the trigger threshold for the hold duration.
- Keep a pre-roll buffer so the first transient is not clipped when recording starts.
- Stop automatically, or mark the trim end, after audio stays below the tail threshold for the configured tail-silence duration.
- Leading and trailing silence trimming should be based on measured audio level, not manual timing.
- Print trigger threshold, trigger time, pre-roll used, detected tail point, and trimmed duration in final output.

## GUI Integration

`jam2-gui` can later expose capture actions that launch `jam2-capture` as a child process.

Useful GUI actions:

- Record instrument idea.
- Capture system audio.
- Stop capture.
- Import capture into the current song/session track library.
- Open the captured file in solo practice mode.
- Share the captured file with the peer over the GUI TCP control plane.

The GUI should treat captured files as normal shared-track assets after recording.

## Optional Capture Analysis

`jam2-capture` can optionally analyze captured WAV files after recording and write sidecar metadata for `jam2-gui`.

Possible CLI shape:

```text
jam2-capture record-input --audio-device 5 --input-channels 1 --output riff.wav --analyze on
jam2-capture record-loopback --source default --output backing.wav --analyze on
```

Behavior:

- After recording and trim are complete, run offline analysis on the final WAV.
- Analyze approximate BPM, key, and chord suggestions where practical.
- Write analysis to a sidecar HJSON file next to the WAV, such as `riff.hjson` or `backing.hjson`.
- Store values as suggestions that the GUI can display and the user can correct.
- `jam2-gui` should load the WAV and sidecar HJSON together when available.
- Corrected values in the GUI should become project metadata and should not overwrite the raw analysis unless the user explicitly exports updated metadata.

Essentia investigation:

- Download and inspect the Essentia source before deciding whether `jam2-capture` links the full library, a reduced custom build, or a small helper/wrapper.
- Identify only the algorithms needed for captured WAV analysis: tempo/BPM, key, chroma, and chord suggestions.
- Keep capture lightweight by avoiding unused Essentia features and optional dependencies.
- If linking Essentia into `jam2-capture` is too heavy, use a separate analyzer helper that reads WAV and writes HJSON.

Example sidecar metadata:

```hjson
track: {
  source_wav: "riff.wav"
  detected_bpm: 118.7
  accepted_bpm: null
  detected_key: "D major"
  accepted_key: null
}
chords: [
  { start_seconds: 0.0, end_seconds: 2.0, detected: "D", accepted: null }
  { start_seconds: 2.0, end_seconds: 4.0, detected: "A", accepted: null }
]
capture: {
  mode: "record-input"
  trigger_enabled: true
  trimmed_leading_ms: 240
  trimmed_trailing_ms: 870
}
```

## File Formats

First format:

- WAV PCM.

Future formats:

- MP3 export through an optional encoder if useful.
- FLAC export if smaller lossless files become useful.

Keep format support separate from the live UDP audio protocol.

## Things To Test

- ASIO input capture records the selected channel(s) correctly on Windows.
- CoreAudio input capture records the selected channel(s) correctly on macOS.
- WASAPI loopback captures system/app playback on Windows.
- Captured WAV files load into `jam2-gui` solo practice mode.
- Captured WAV files can be shared as backing-track/session assets over GUI TCP.
- Capture can run without starting a live jam.
- Capture failures report clear device/source/permission errors.
