# Jam2 Future Plan

This file tracks future work that is not already implemented. User-facing documentation lives in `docs/`, with only `README.md` and this plan kept at the repository root. Completed refactor history, evidence, and supporting reviews live in [refactor-plan.md](refactor-plan.md) and its linked refactor documents.

## Essentia

Consider essentia for things like beat detection.

Workflow consideration:
- I start on the looper view and I record some free form idea 
- BPM follows the idea that I just put in 
- Generate can infer the harmony, key/collection, groove, phrasing, and possible
  style/profile of what I just played, then populate related ideas around it.
  Any inferred profile should be visible and overridable rather than relying on
  the removed global Mood model.

Could also be used for having a section and then generate a B section from it that follows on nicely, still having complexity for the different ideas it might introduce, but help with A and B sections 

## Arrangement

The first four chord/beat/lyric sections now map directly to looper Banks A-D when
reference WAVs are generated. Keep that simple fixed mapping as the foundation for
an arrangement system.

Future arrangement work:

- Store an explicit sequence such as `A x2 -> B x2 -> A x2`, separate from the
  section content and the audio in each bank.
- Advance banks only on an exact section or bar boundary derived from the shared
  transport. Do not use an approximate wall-clock timer.
- Show hard state in Performance view: current bank, next bank, repetitions
  remaining, and the scheduled transition frame.
- Start with manual sequence editing and start/stop controls. Avoid hidden
  arrangement generation or automatic recommendations.
- Keep custom recorded/imported lanes alongside generated reference lanes when a
  bank is revisited or its section references are regenerated.
- Keep A-D as the first implementation limit. Expanding beyond four banks should
  be a separate decision rather than adding another mapping layer now.

## Musical Idea Generation Framework

`THEORY-PLAN.md` records the completed research and the approved implementation
design for the version-6 idea generator. The researched catalog and redesign
have been implemented; listening-led refinement remains normal follow-up work,
with Modern Progressive Metalcore intentionally marked experimental.

The following are hard product plans. Research must determine their smallest
accurate, coherent implementation; it must not decide whether to omit them:

- Replace the current implied `N/4` and 4/8/12/16-bar restrictions with explicit
  meter, beat-unit, beat-grouping, compound-subdivision, click/accent, and
  profile-native phrase/form-length support. Generate and edit the meters and
  lengths approved for each style directly rather than approximating them as
  4/4.
- Add a dedicated bass line with its own style-aware rhythm, pitch movement,
  phrase development, editing/view representation, recipe data, diagnostics,
  teaching explanation, and reference rendering.
- Add role-aware supporting musical lines where the selected profile uses them,
  including melodic harmony, countermelody, call-and-response, pads, riffs, and
  other researched roles. Keep these distinct from both the main melody and
  chordal comping.
- Add profile-level long-range form and arrangement/energy planning so harmony,
  melody, bass, supporting lines, comping, drums, timbre, and section boundaries
  develop together over the idea's native span.
- Expand compact local synthesis, articulation, percussion voices, and per-lane
  timing as required to represent the approved styles honestly. Parameters and
  generated choices must remain deterministic, inspectable, and visible in
  teaching details and raw diagnostics.
- Completed in the production drum rework: use the local Google Magenta Groove
  MIDI Dataset as an evidence source for
  a dedicated audit of Jam2's actual generated drum tracks, not only for drum
  timbre velocity calibration. Compare style-matched distributions and
  relationships for lane/piece usage, hit density, ghost/normal/accent velocity
  bands, repeated-hit variation, syncopation, microtiming, open/closed-hat
  behaviour, kick/snare/ride relationships, and fill placement. Report the raw
  Jam2-versus-dataset measurements per profile, inspect outliers musically, and
  use the evidence to improve how each kit piece is generated and used. Keep
  deterministic Jam2 rules and profile intent authoritative: the dataset is a
  reference population, not a target to copy or an opaque learned generator.
  The checked-in analysis, complementary Drum Groove Corpora timing evidence,
  888-idea structural corpus, phrase-aware drummer implementation, and raw
  results are recorded in `DRUM-REWORK.md` and `DRUMKIT-RESEARCH.md`. The
  `2026-07-31` final validation at the top of `DRUM-REWORK.md` records clean
  exact parity, the complete `1,701`-render Lab audit, the final rational-knee
  audio corpus, regressions, boundary validation, and the required
  `release/jam2.exe` build. Listening-led refinement remains normal follow-up
  work through the retained Drum Kit Lab.
- Make Idea Details teaching-first, with an expandable Detailed Analysis view
  for the exact recipe, theoretical decisions, timings, and synthesis choices.

Phase 1B also includes a narrowly bounded Modern Progressive Metal / Metalcore
feasibility study centred on Spiritbox, ERRA, and Nick Broomhall. Do not treat it
as coverage of all Metal. Public inclusion depends on whether compact local
synthesis can make original low-tuned riffs, mute/open articulation, distortion,
bass reinforcement, modern drums, and clean/ambient contrast musically
convincing without samples or a broad production platform.

Keep automatic continuation from a user-authored A section into A-prime or B as
separate future work. Phase 1B should still research the relevant style and
section-flow relationships so that future feature has a grounded specification.

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
