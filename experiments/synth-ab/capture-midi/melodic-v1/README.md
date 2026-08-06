# Melodic reference-capture MIDI v1

This pack has two jobs:

1. `replication` exposes enough of one melodic patch to reproduce its sound in
   the synth lab.
2. `musical-context` lets candidate chord, melody, bass, support, and drum
   sounds be auditioned together before they are adopted by a Jam2 style.

All files are deterministic, 120 BPM, 4/4, and 480 PPQ. They contain no program
changes, mixer volume, or pan messages, so importing them does not replace the
selected Ableton instrument or set its mix level.

## Replication files

Start with `replication/00-full-spectrum.mid` when the instrument is genuinely
playable across a wide range. It combines all seven tests and takes about 9:35:

| File | What it reveals |
| --- | --- |
| `01-chromatic-range.mid` | Every semitone from MIDI 28 through 84, including key tracking, multisample zones, and register changes |
| `02-velocity-response.mid` | Velocities 20, 50, 80, 110, and 127, repeated four times at low, middle, and high notes |
| `03-duration-release.mid` | 125 ms, one-second, and four-second gates plus four seconds of release space |
| `04-repeated-notes.mid` | Phase reset, round robin, retrigger behaviour, and voice stealing |
| `05-polyphony-chords.mid` | Intervals, triads, sevenths, inversions, and open voicings in several registers |
| `06-legato-glide.mid` | Detached notes, overlaps, mono priority, glide, and legato envelopes |
| `07-optional-expression.mid` | Mod wheel, channel aftertouch, expression, and sustain pedal; use only when the patch responds to these controls |

The range is based on the current generated Jam2 corpus, not the full MIDI
keyboard. Its observed ranges are:

| Role | MIDI range | Scientific pitch | Ableton label |
| --- | ---: | --- | --- |
| Bass | 28-55 | E1-G3 | E0-G2 |
| Chords | 36-72 | C2-C5 | C1-C4 |
| Melody | 54-81 | F#3-A5 | F#2-A4 |
| Support | 48-77 | C3-F5 | C2-F4 |
| Complete capture | 28-84 | E1-C6 | E0-C5 |

Ableton displays MIDI octaves one lower than the scientific pitch labels Jam2
uses. The MIDI note number is authoritative.

For a bass, do not worry if its intended upper range sounds unnatural; the
bass-role range is the important part. For a monophonic lead or bass, the chord
file is still useful because it reveals note priority and voice stealing, but
it is not expected to sound like polyphony. A narrow specialist patch can be
rendered from only the relevant module files.

## Musical context files

The `musical-context` files form one 16-bar, 32-second A-minor/C-major
arrangement:

- `01-chords.mid`: sustained chords, stabs, inversions, and open voicings on
  channel 1.
- `02-melody.mid`: a repeated hook, answer phrases, and a register lift on
  channel 2.
- `03-bass.mid`: roots, fifths, eighth-note drive, and approach notes on
  channel 3.
- `04-support.mid`: quiet answers, arpeggios, and chord-tone layers on channel
  4.
- `05-drums.mid`: a General MIDI groove on channel 10.
- `06-full-arrangement.mid`: the same material as a type-1 MIDI file with five
  named, aligned tracks.

For an existing Ableton project, put the five stem files at the same start
point on five tracks. This is usually easier than importing the full
arrangement because each track can already contain the instrument being
auditioned. If the drum rack is not General MIDI, transpose/remap only the drum
notes; do not move the events in time.

`musical-context` also contains one folder per Style Mixer genre, with nested
folders for all 27 current profiles. Those files are different from the neutral
arrangement above: they reproduce the exact deterministic eight-bar performance
heard by the web app for that profile, including timing and drum-performance
detail. See [`musical-context/README.md`](musical-context/README.md) and
`musical-context/profile-context-manifest.json` for the complete inventory.

## What to export

For each melodic patch being replicated:

1. Put the patch on a clean Ableton track and render the complete capture, or
   the applicable replication modules, from the clip start through the final
   silence.
2. Export the patch in its intended stereo form. Stereo chorus, unison,
   reverb, delay, and panning can be part of a melodic sound's identity, so
   stereo is the primary reference here rather than a problem.
3. If the instrument has a meaningful dry or mono output, also export that as
   a diagnostic reference. Do not collapse the intended stereo patch to mono
   merely to make a second file.
4. Use 48 kHz, 24-bit PCM WAV, Normalize off, Dither off, and no master-bus
   processing unless that processing is deliberately part of the reference.
5. Leave automation, velocity sensitivity, random/round-robin behaviour, and
   the instrument's own effects enabled. Disable unrelated mix-bus effects.

For mix selection, export the five context stems and one stereo full mix at
their intended relative levels. All context renders must have the same start
and end time, including silent sections.

## Information to keep with each patch

The audio is substantially more useful when accompanied by:

- the instrument/patch name and whether it is intended for chords, melody,
  bass, support, or several roles;
- the Ableton Live Set or saved device rack, including effect-chain order;
- whether the patch is mono/poly, its voice count, glide mode/time, unison,
  oscillator phase/randomness, and oversampling/quality setting;
- any articulations, keyswitches, macros, modulation-wheel use, aftertouch,
  sustain behaviour, or MPE dimensions that are part of normal playing;
- a note naming the primary reference (`stereo-wet`, `stereo-dry`, or
  `mono-dry`) and any master processing intentionally retained.

If a patch changes articulation through keyswitches or macros, export each
normal articulation separately with the same relevant MIDI modules. If it has
MPE or other per-note expression, provide the Ableton clip/Live Set as well;
the v1 files use conventional channel MIDI and cannot encode every MPE gesture.

## Regeneration and validation

From the repository root:

```powershell
python experiments/synth-ab/tools/generate_melodic_capture_midi.py
python experiments/synth-ab/tools/generate_melodic_capture_midi.py --check
python experiments/synth-ab/tools/generate_profile_context_midi.py
python experiments/synth-ab/tools/generate_profile_context_midi.py --check
```

`manifest.json` records the exact duration, note/velocity range, event count,
track names, MIDI channels, and SHA-256 hash of every file.
`musical-context/profile-context-manifest.json` provides the equivalent record
for every genre/profile performance.
