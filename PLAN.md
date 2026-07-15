# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root. Completed refactor history, evidence, and supporting reviews live in [refactor-plan.md](refactor-plan.md) and its linked refactor documents.

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

## Practice Ideas (Visual Only)

Consider adding lightweight visual practice generation to the existing Chord View and Beat View. This should remain a reference for musicians to play themselves, not an automatic accompaniment, assessment, or training platform. Users can already use the views for ear training, and can use the looper to record drum, chord, bass, or melody parts before playing another part over them.

Possible chord and soloing exercises:

- Generate chord progressions from explicit constraints such as key, mode, allowed chord qualities, progression length, harmonic rhythm, and whether chords must remain in the selected key.
- Use weighted musical rules, repetition limits, phrase structure, and controlled mutation so exercises are coherent without becoming fully predictable.
- Expose the random seed and exact variation controls so an exercise can be recreated, saved, shared, or deliberately changed.
- Allow individual chords, beats, bars, or sections to be locked while the remaining material is regenerated or mutated.
- Provide visual practice modes such as current/next chord reveal, guide-tone or target-note prompts, limited note sets, rhythmic motifs, call-and-response bars, and explicit transposition between loops.

Possible drum exercises:

- Generate visual patterns for selected grooves and subdivisions using stable anchors plus controlled kick, snare, cymbal, tom, and fill variation.
- Support exercises such as keeping several lanes fixed while one changes, omitting a lane for the player to supply, progressively adding lanes, displaced patterns, fill windows, and visual gap-click patterns.
- Consider extending beat steps beyond the current binary hit state so accents and ghost notes can be represented. One possible interaction is right-clicking a step to select a distinct ghost-note state, stored and drawn differently from a normal hit, for example a different square style or an `o` marker. The exact states, interaction, storage, and visual treatment require further discussion.
- Improve the moving marker to identify the exact active subdivision, not only the current beat, before relying on Beat View for detailed reading exercises.

Scope and performance limits:

- Keep the first version visual only. Do not add generated drum samples, chord synthesis, automatic listening, pitch detection, correctness scoring, subjective ratings, or inferred recommendations.
- Keep generation deterministic, local, fast, and outside every real-time audio callback. A small rule-based generator is preferable to a broad training framework or dependency.
- Reuse the existing metronome, song saving, view synchronization, and looper workflows where practical.
- Do not measure or score the musician's performance in the initial feature. If performance analysis is ever considered, discuss its reliability, latency, CPU cost, audio-path isolation, device/input requirements, and real-time safety before implementation.
- Treat richer beat states, multi-section exercise playback, mixed subdivisions, and audible accompaniment as separate future decisions rather than requirements for the first visual generator.

### Optional Audible Reference Rendering

After the visual generator is useful on its own, consider basic audible references that help the musician hear the generated material. This remains ear-based practice: the musician decides whether what they are playing matches the reference, and Jam2 does not listen, grade, score, or report correctness.

Possible uses:

- Click or otherwise audition a recognized chord to hear a short, basic synthetic reference for its notes and quality.
- Render a generated chord progression so the musician can hear the harmony before recording or playing over it.
- Render a generated beat pattern so the musician can hear and internalize the feel of the groove while learning it.
- Map any future normal, ghost, and accent beat states to explicit reference levels or simple sound variants without treating those levels as performance measurements.

Prefer offline rendering through the existing Track and looper workflow rather than real-time synthesis of an evolving arrangement:

- Derive event timing from the same BPM, beat, subdivision, bar, and metronome-grid rules used by the visual exercise.
- Generate bounded mono PCM16 WAV assets outside the real-time callback and at the active engine sample rate.
- Place chord references in one Track lane and drum references in another so they can be muted, soloed, level-adjusted, looped, replaced, or recorded over independently.
- Keep the metronome separate from the rendered WAVs so the live grid click can remain audible over the chord and drum reference lanes and can be adjusted or disabled independently.
- Re-render explicitly when the exercise, BPM, subdivision, voicing, groove, or requested loop length changes; do not hide automatic timing conversion or correction.
- Let the audio callback consume only prepared track data through a bounded playback path. Do not parse chords, allocate voices, generate waveforms, access files, or construct patterns in the callback.

Initial sound scope should remain deliberately basic:

- Use a small fixed chord vocabulary, equal-tempered note frequencies, simple spread voicings, bounded voice counts, short attack/release envelopes, and conservative gain normalization.
- Prefer structured generator-produced chords. If manually entered chord text is supported, recognize only a documented set of chord spellings and report unsupported text rather than guessing.
- Use a small synthetic drum set such as kick, snare, closed/open hi-hat, and tom before considering bundled samples. Realistic multisampled instruments, effects, MIDI, articulation systems, and broad chord-symbol interpretation are outside the initial reference feature.
- Expose the reference render duration, sample rate, generated frame count, lane level, and resulting WAV path where useful for inspection.
- Bound exercise length, rendered frames, simultaneous chord voices, drum events, memory use, and output size. If rendering can become noticeable, keep it cancelable and off the GUI and audio threads.

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
jam2 network create --audio-backend alsa --audio-device hw:2,0 --sample-rate 48000 --audio-buffer-size 128
```

Rules:

- Keep Linux builds host-native through CMake.
- Do not make PulseAudio the low-latency backend.
- Expose actual ALSA period size, buffer size, sample format, channel count, input/output latency frames, underruns, overruns, and xrun recoveries in stats.
- Prefer one full-duplex hardware device.
- If separate input/output devices are used later, expose the clocking and drift consequences clearly.
- Real validation must happen on Linux with the actual audio driver stack and hardware; build success alone is not meaningful latency validation.
