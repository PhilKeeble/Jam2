# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root. Completed refactor history, evidence, and supporting reviews live in [refactor-plan.md](refactor-plan.md) and its linked refactor documents.

## Tuner

Add aubio for live audio tuning (guitar / bass / singing etc), allow them to see note in real time in performance mode of their input and then open up to see a more in depth tuner view helping them get in position

## Essentia

Consider essentia for things like beat detection.

Workflow consideration:
- I start on the looper view and I record some free form idea 
- BPM follows the idea that I just put in 
- Generate can follow the style and mood and chords and key of what i just played to then populate ideas around it 

Could also be used for having a section and then generate a B section from it that follows on nicely, still having complexity for the different ideas it might introduce, but help with A and B sections 

## Arrangement

Some way to move forward and back through sections or with time, so that the performance mode can follow and looper banks can be tied to sections, allowing 4 sections in one song 

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
