# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root.

## Shared Song Timing Ideas

The GUI already has shared song grids and local track playback. A possible future pass is tighter timing between the song grid, metronome, and any shared backing track.

Ideas to discuss before implementation:

- Show a small current-beat indicator over chord and beat views.
- Let both players start from the same song-grid position at the same agreed session time.
- Consider a per-track option for whether playback should lock to the metronome.

Do not implement this without first deciding the exact timing model and how it should be measured.

## Engine-Side Shared Track Mix Source

`jam2-gui` can currently load, play, process, and share WAV files locally through Qt audio and the GUI TCP control plane. A possible future engine-side track source would mix a local backing track through the same ASIO/CoreAudio device path as the live jam output and metronome.

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

- The track source should remain local-only by default and should not be sent over the live UDP audio stream.
- Mix the track into local output through the same ASIO/CoreAudio device as remote peer audio and metronome.
- Keep track level independent from remote peer playback level and metronome level.
- Decode and file I/O must stay outside the real-time callback.
- Prebuffer decoded audio or use a callback-safe handoff.
- Report track loaded/playing state, track level, underruns, decode errors, and current playback frame in machine-readable status and CSV where useful.
- Let `jam2-gui` continue handling TCP file transfer, file readiness, shared countdowns, and synchronized playback commands.

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
