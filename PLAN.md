# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root. 

## Sound Design

Add sounds in with good mixes 
Work on the chord generation to make it more rhythmically interesting, like the midi codex generated for me for the initial melodic references that used stuttered chords after held chords etc 

## Release

Link DLLs on windows so its one bin
Make docker compose that will build on windows / mac / linux, ship back into folder like Jam2gether-Windows etc and then have just the clean binary in there with the docker compose handling getting those base into jam2 in dist/ or something

maybe binary doesnt have validate etc in it and thats only debug builds? on release i spawn container for windows and mac and then build debug, run all validation checks, then pass on to build release and package them up into dist/ for easy release ?

website and youtube videos 
discord server for community feedback / testing / logs etc?
Make a jamjar collection repo to store full songs and jams 

## macOS Follow-ups

- Fix loopback recording on macOS. Validate the CoreAudio capture and permission paths, keep the UI action disabled when capture is unavailable, and add native CTest coverage plus a profiled hardware check on macOS.

## refinement pass

look for redundant code and dependencies 
look for old schema and backwards compatability
look for places we can refactor to make code splits cleaner 

## Deferred latency tuning

- After the base audio/protocol code refinement sweep is complete, perform a measured jam-profile settings sweep. Keep user-facing numeric buffers/delays as the stability levers rather than hiding additional automatic latency.
- Defer 32-frame network-packet experiments until that settings sweep; retain the current 64-frame protocol packet while code-path behavior is being established.
- Defer recovery/reorder/jitter-threshold tuning until the settings sweep, with loss/reordering impairment evidence and audio-quality validation before changing it.
- Keep the common full-mesh path for two peers. Later measure whether a special two-peer path would materially help before considering any implementation.

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
