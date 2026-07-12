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

## UDP Packet Fast-Path Efficiency Review

Review the existing binary UDP audio path independently of any GUI/engine consolidation. Measure first, then remove avoidable packet-rate allocation and copying where the results justify it.

- Instrument or benchmark packet encode, authentication, PCM24 pack/unpack, receive parsing, reorder handling, and mesh mixing for two peers and representative three/four-peer sessions.
- Measure CPU time, allocations per packet, bytes copied per packet, packet-loop gaps, callback gaps, jitter, late packets, loss, playback underruns, and aggregate/per-peer mesh cost.
- Investigate caller-owned preallocated transmit/receive buffers and direct PCM24 conversion into preallocated audio blocks or ring reservations.
- Investigate computing or verifying the authentication tag without copying the complete packet solely to clear the tag field.
- Replace exceptions for ordinary malformed or unauthenticated datagrams with explicit parse results if measurements or profiling show a meaningful benefit.
- Keep the current fixed binary wire format and observable behavior unchanged during the first optimization pass so results can be compared directly.
- Keep all storage bounded and expose any new buffer capacity or drop counters through technical stats and CSV output.

Acceptance criteria:

- No regression in authentication, session validation, sequence tracking, audio correctness, metronome/transport timing, or malformed-packet rejection.
- Benchmark results record before/after measurements rather than assuming that fewer allocations improve real-world latency.
- Optimizations improve or preserve packet processing and callback timing for both the primary two-person flow and small mesh sessions.

## Application Refactoring

Consider consolidating the GUI and audio engine into one end-user application, and selectively replacing peer/song JSON control messages with fixed binary messages where measurements justify it. Preserve the primary two-person flow while retaining mesh for small groups and larger direct sessions.

See [refactor.md](refactor.md) for the current architecture review, tradeoffs, risks, efficiency opportunities, and recommended implementation sequence.

## Loopback Recording Level Investigation

Investigate why GUI WASAPI loopback recordings can be quieter than the audio heard through the selected render endpoint. PCM16 conversion should preserve normalized signal level, so first verify the current mono downmix, which averages every endpoint channel and may attenuate audio when only some channels contain signal or when channels partially cancel.

- Record known-level test signals with identical stereo, left-only, right-only, and opposite-polarity channel content.
- Test stereo and multichannel render endpoints, including interfaces where only outputs 1-2 carry audio.
- Compare source and recorded peak/RMS levels in dBFS to quantify any reduction.
- Expose or log the selected endpoint, channel count, channel mask, mix format, valid bits per sample, and captured peak dBFS outside the capture path.
- If channel averaging is confirmed, add an explicit, inspectable loopback channel selection or downmix rule rather than automatic normalization.
- Keep the saved format PCM16 during this investigation so bit depth and channel routing remain separate variables.

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
