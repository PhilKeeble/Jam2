# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root. 

## Sound Design

Add sounds in with good mixes 
Work on the chord generation to make it more rhythmically interesting, like the midi codex generated for me for the initial melodic references that used stuttered chords after held chords etc 

## Release

Link DLLs on windows so its one bin
Make docker compose that will build on windows / mac / linux, ship back into folder like Jam2gether-Windows etc and then have just the clean binary in there with the docker compose handling getting those base into jam2 in dist/ or something

## Wav analysis

back on the menu, test in lab to see how accurate it is ?

  A realistic implementation sequence would be:

  1. WAV import → Demucs stems → BPM/key/chord analysis → review → JamJar.
  2. Add bass and melody transcription.
  3. Add classified drum-lane transcription.
  4. Add loopback capture as another input source.
  5. Add optional lyric transcription.

Looks like maybe not worth the effort? 

## Guitar Pro Import

Guitar pro has complex writings that could be exportd to midi, and then that midi could feed jam2?

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
