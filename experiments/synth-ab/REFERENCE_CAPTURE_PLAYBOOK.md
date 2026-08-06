# Reference capture and synth-matching playbook

This is the reusable input and iteration contract for matching reference sounds
in the Jam2 sound-design workbench. It records what was learned while building
`AbletonRock32`, and it is intended to remain useful when the work moves from
drums to melodic instruments.

## Principles learned from AbletonRock32 and Ableton808

1. Match the physical stages of the sound separately: excitation/attack, body,
   resonant tail, noise/texture, and room or stereo field.
2. Correct the underlying model before adding layers. Extra noise can hide an
   incorrect attack briefly, but it usually makes the result washier and less
   identifiable.
3. Noise is not automatically realism. Add a band only when a corresponding
   feature can be heard in the reference, then compare with that band muted.
4. Keep transient pitch and timbre stable across velocity unless the reference
   clearly changes articulation. A conspicuous pitch-changing transient reads
   as a clap or synthetic layer.
5. Judge normalized isolated hits and level-matched full-kit phrases separately.
   A tonally pure or long sound can appear louder even when its peak is not.
6. Repeated hits are essential. They expose round robin, random phase, velocity
   mapping, harsh resonances, and tails that accumulate in real patterns.
7. Capture different articulations separately. For example, ride tip and ride
   cup should be separate reference files even if the final kit maps them to
   normal and accent velocities.
8. Use ablation renders while tuning: model only, attack only, resonators only,
   noise only, and complete sound. Keep a layer only when it closes an audible
   gap.
9. The optional Synth Character Layer is not a default requirement for matched
   acoustic sounds. Use it only when the source has an audible synthetic
   component that the physical model cannot express.
10. Preserve a complete-kit reference groove at its natural relative levels.
    Isolated one-shots cannot reveal whether a matched piece sits correctly in
    the kit.
11. For very short resonators, phase and sub-millisecond delay can matter as
    much as frequency. High-pass the individual band when a truncated sinusoid
    creates false sub-bass rather than changing the whole piece's colour stage.
12. Fit ghost, normal and accent gains from the repeated sections separately
    from the continuous velocity curve. Sampler velocity groups are often not a
    single power law.
13. Compare complete-pattern crest factor and section-relative RMS after the
    isolated voices match. Use a kit-only bus compressor only when the context
    reference demonstrates that density; keep room and stereo treatment
    separate.

## Recording format

Use this format for authoritative isolated references:

- WAV, 48 kHz, 24-bit PCM.
- Mono for the core reference. In Ableton, use **Convert to Mono** so the two
  channels are summed; do not export only the left channel.
- No normalization, limiting, clipping, dithering, mastering chain, automatic
  fades, or time warping.
- Keep the instrument/device processing that defines the sound, but document it.
- Leave the MIDI-provided two-second pre-roll intact.
- Export each instrument or articulation as its own stem.
- Preserve the full release tail. Silence after the final tail is harmless and
  can be trimmed automatically.

Optional stereo material is still valuable, but it serves a different purpose:

- Export stereo full-kit grooves or complete melodic phrases to describe the
  intended panorama, ambience, chorus, delay, and reverb.
- If a stereo effect is fundamental to an instrument, provide both a dry mono
  core and the complete stereo patch when possible. This lets the synthesis and
  spatial/effects problems be measured independently.

## Standard drum capture contract

Use the files in `capture-midi/drums-v1`. Every articulation file contains:

- a two-second pre-roll at 120 BPM;
- one 17-step velocity ladder: 1, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88,
  96, 104, 112, 120, and 127;
- 12 ghost hits at velocity 24;
- 12 soft hits at velocity 48;
- 12 medium hits at velocity 80;
- 12 hard hits at velocity 112.

The 12 repeats reveal sampler round robin and randomization. The ladder reveals
velocity switches and continuous changes without requiring hundreds of hits.

Spacing is based on the expected tail:

| Class | Interval | Approximate file length | Pieces |
| --- | ---: | ---: | --- |
| Short | 1.0 s | 1:07 | kick, snare, hats closed, toms, cross-stick, clap |
| Long | 4.0 s | 4:22 | open hat, crash, ride tip, ride cup |

If the source has a tail longer than four seconds, extend only that MIDI clip's
spacing before export and note the change. Do not shorten or gate the source to
fit this contract.

### Ableton workflow

1. Set the Live set to 120 BPM and disable Warp for rendered reference audio.
2. Use `00-kit-map-check.mid` first. Confirm that every note triggers the named
   articulation; transpose the relevant capture clip if the device is not GM
   mapped.
3. Put each capture MIDI file on the track containing its corresponding device
   or Drum Rack pad. Do not layer multiple kit pieces on one exported stem.
4. Disable master/return processing unless it is an inseparable part of the
   source. Record the exceptions in the delivery notes.
5. Export **All Individual Tracks**, WAV, 48 kHz, 24-bit, mono, Normalize off,
   Dither off. The tails may contain silence; that is acceptable.
6. Also export a stereo full-kit groove, tom fill, repeated-ride phrase, and
   open/closed-hat phrase at the kit's intended mix levels.

Deliver the MIDI files used, the isolated WAV stems, the stereo context phrases,
and the metadata below in one kit folder.

When a capture clip has been transposed to fit a non-GM rack, the edited note can
be recovered directly from the Ableton Live Set with:

```powershell
python experiments/synth-ab/tools/inspect_ableton_drum_map.py "path/to/project.als"
```

Do not silently apply that project-specific map to the generic capture pack.
Store it with the corresponding kit and use it for that kit's musical comparison
patterns.

## Metadata to include with every source

- Kit, preset, sample-library, or device name and version.
- MIDI note map and the name of each articulation.
- Exact instrument/device chain, return sends, and master-chain state.
- Whether round robin, humanization, velocity randomization, or random phase is
  active.
- Any altered capture spacing or note length.
- Short perceptual notes, especially articulation changes such as "ride cup only
  on accents" or "open hat is intentionally muted and smooth."
- For melodic sounds: playable range, mono/poly mode, voice count, glide,
  unison, tuning, and controller assignments.

## Iteration procedure

For each sound:

1. Trim leading/trailing silence for analysis while retaining an untouched copy.
2. Segment hits from their MIDI schedule and measure peak, RMS, decay times,
   spectral centroid, strong modes, noise distribution, and velocity response.
3. Establish the simplest model that supplies the sound's main identity.
4. Match attack, body, tail, and texture in that order. Re-check earlier stages
   after every material layer change.
5. Compare consistent-velocity one-shots and repeated-hit passages at matched
   loudness. Never use peak level alone as the similarity criterion.
6. Render explicit ablations for uncertain layers and keep the simpler version
   when the difference is not useful.
7. Test the complete kit or arrangement against the context references without
   normalizing individual pieces.
8. When the performance MIDI is available, render those exact note times and
   velocities instead of approximating the groove with an internal pattern.
9. Record engine limitations. Add resonators, noise bands, envelopes, modulation,
   or spatial processing only for reference features that the current engine
   demonstrably cannot reproduce.

## Extension for melodic instrument replication

Melodic sources require the same attack/body/tail decomposition, plus pitch,
duration, expression, and polyphony dimensions. Before matching a melodic patch,
capture these independently:

- **Pitch map:** isolated notes across the playable range, ideally every minor
  third; at minimum C and F-sharp in every octave. This reveals key tracking,
  multisample zones, formants, and register-dependent envelopes.
- **Velocity map:** velocities 20, 50, 80, 110, and 127 on representative low,
  middle, and high notes, with eight repeats when phase or round robin varies.
- **Duration map:** 125 ms staccato, one-second medium, and four-second held notes,
  followed by enough silence to preserve the release.
- **Sustain map:** long notes that expose oscillator drift, loop points, beating,
  filter movement, and evolving modulation.
- **Articulations:** separate files for pluck/sustain, muted/open, pick/finger,
  legato/retrigger, or any keyswitch-selected behavior.
- **Expression:** slow, documented sweeps of modulation wheel, aftertouch,
  expression, pitch bend, macro controls, and sustain pedal when they change the
  identity of the patch.
- **Polyphony:** intervals and triads in low, middle, and high registers, followed
  by a repeated musical phrase. This exposes voice allocation, phase reset,
  unison, nonlinear drive, and effects that cannot be inferred from one note.
- **Spatial layer:** a dry mono core plus the complete stereo patch or effects
  returns, when available.

The checked-in [`capture-midi/melodic-v1`](capture-midi/melodic-v1/README.md)
pack provides a universal first pass plus separate modules for these dimensions.
Use the complete file for broad keyboard instruments, or only the relevant
modules for specialist basses, mono leads, pads, and plucks. After the first
analysis, a shorter instrument-specific follow-up may still be needed: a long
evolving pad needs more sustain and release time than the universal file, while
a short pluck benefits more from repeated-note and velocity detail.

The current Jam2 generation corpus uses MIDI 28-55 for bass, 36-72 for chords,
54-81 for melody, and 48-77 for support. The universal chromatic capture covers
MIDI 28-84 to include the union plus melodic headroom. Its aligned 16-bar
musical context supplies separate chord, melody, bass, support, and General MIDI
drum files plus one five-track type-1 MIDI arrangement.

For melodic patches, the intended stereo render is normally the primary
reference because unison, chorus, spatial modulation, delay, and reverb can be
part of the instrument rather than incidental ambience. Add a dry or mono
diagnostic render when the patch exposes one without changing its intended
sound. Keep the patch/device name, role, Live Set or device rack, mono/poly and
glide settings, voice count, unison/phase behaviour, effect order, relevant
controllers, articulations, keyswitches, and MPE usage with the audio exports.
