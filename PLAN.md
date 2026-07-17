# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root. Completed refactor history, evidence, and supporting reviews live in [refactor-plan.md](refactor-plan.md) and its linked refactor documents.

## Loopback Recording Level Investigation

Investigate why GUI WASAPI loopback recordings can be quieter than the audio heard through the selected render endpoint. PCM16 conversion should preserve normalized signal level, so first verify the current mono downmix, which averages every endpoint channel and may attenuate audio when only some channels contain signal or when channels partially cancel.

- Record known-level test signals with identical stereo, left-only, right-only, and opposite-polarity channel content.
- Test stereo and multichannel render endpoints, including interfaces where only outputs 1-2 carry audio.
- Compare source and recorded peak/RMS levels in dBFS to quantify any reduction.
- Expose or log the selected endpoint, channel count, channel mask, mix format, valid bits per sample, and captured peak dBFS outside the capture path.
- If channel averaging is confirmed, add an explicit, inspectable loopback channel selection or downmix rule rather than automatic normalization.
- Keep the saved format PCM16 during this investigation so bit depth and channel routing remain separate variables.

From mac looks like that it is stero recording going to mono, if input channel is 1,2 on focusrite then mono reording will be very quickm need to look into that.

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
jam2 network create --audio-backend alsa --audio-device hw:2,0 --sample-rate 48000 --audio-buffer-size 128
```

Rules:

- Keep Linux builds host-native through CMake.
- Do not make PulseAudio the low-latency backend.
- Expose actual ALSA period size, buffer size, sample format, channel count, input/output latency frames, underruns, overruns, and xrun recoveries in stats.
- Prefer one full-duplex hardware device.
- If separate input/output devices are used later, expose the clocking and drift consequences clearly.
- Real validation must happen on Linux with the actual audio driver stack and hardware; build success alone is not meaningful latency validation.
