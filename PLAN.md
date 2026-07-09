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

## Small Full-Mesh Group Mode

Consider an experimental mode for multiple people. The goal is to preserve the current direct UDP design by using a small full mesh instead of a relay, TURN audio path, room server, or central mixer.

Topology:

- Each participant sends captured mono audio directly to every other participant.
- Each participant receives one independent remote stream per peer.
- Each client mixes remote streams locally into the mono playback output.
- A listener may coordinate the initial peer list, but must not become an audio relay or hosted mixer.

Latency rules:

- Do not wait for all peers' packets to align before playback.
- Keep each remote peer on independent reorder buffering, playback depth, drift correction, underrun handling, and stats.
- If one peer is late or unstable, that peer should contribute silence, drops, or drift correction independently while stable peers continue at their lowest usable latency.
- Mixing remote mono streams should happen in the audio callback from already-prefilled per-peer playback rings, using preallocated scratch buffers and no allocation, logging, locks, throws, or blocking operations.

Expected tradeoffs:

- Upload and receive bandwidth scale with `N - 1`.
- CPU cost for summing two or three remote mono streams should be small compared with network jitter, audio device latency, packet scheduling, and drift correction.
- Four-peer mode increases packet rate and uplink pressure enough that Wi-Fi or weak uplinks may require larger playback prefill or jitter-buffer values.
- The worst individual peer link should affect that peer's contribution, but should not force additional delay onto stable peer streams.

Possible CLI shape:

```text
jam2 listen --max-peers 3
jam2 connect "<jam2-url>"
```

Rules:

- Keep mono PCM as the first network format.
- Require all peers to match sample rate and frame size.
- Prefer three-peer LAN validation before internet testing or four-peer experiments.
- Expose per-peer hard stats: endpoint, packet loss, jitter, RTT, bitrate, playback depth, underruns, overruns, drift ppm, and resampler ratio.
- Do not add rooms, accounts, GUI room layers, subjective playability scores, or hidden recommendations.

GUI 

Gui should be done in a star topology rather than full mesh like UDP. All peers connect to the one listener in TCP and follow that session, where peer B could make a change and push it to peer A (the leader) and then the other peers would replicate off of peeer A for the change. No need to track and order, just whoever last made the change wins if people make changes in a view at once. The star topology llows the UDP to follow the more complex mesh model whilst the TCP star can be more simple and relieve some networking pressure whilst alowing all peers to have a 2 way TCP link.

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
