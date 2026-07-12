# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root.

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

## refactoring

The protocol used to sync peers etc might be json, convert to a binary packed format for more efficient transport

- Future follow-up: after the local binary GUI-engine socket stabilizes, consider converting peer/song `ControlServer`/`ControlClient` JSON messages to a similarly fixed binary protocol where it helps latency or allocation pressure. Keep song/file-transfer semantics readable until there is a concrete measured reason to move them.

Consider moving the binaries into 1 application so that its easier for the process between them and easier for it to work like one unit all together



## Linux Audio Backend (never implement this without explicit approval)

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
