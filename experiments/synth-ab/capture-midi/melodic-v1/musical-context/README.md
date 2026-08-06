# Style Mixer profile MIDI

The genre folders contain the exact deterministic eight-bar performances used
by the Synth A/B web app's **Generated style phrase** audition. They are
generated from the native profile seed audit at complexity 4, including the
same notes, chord voicings, velocities, note lengths, lane timing, drum fills,
swing, microtiming, and production drum velocities.

The folder layout uses stable Jam2 identifiers:

```text
musical-context/
  pop/
    pop_loop/
    pop_sectional/
  rock/
    rock_riff_modal/
    ...
```

There are currently 14 genre/style folders and 27 profile folders. Each
profile contains:

- `01-chords.mid` — channel 1
- `02-melody.mid` — channel 2
- `03-bass.mid` — channel 3
- `04-support.mid` — channel 4, only when that role is present in the Style Mixer
- `05-drums.mid` — channel 10
- `06-full-arrangement.mid` — the same active roles as aligned named tracks
- `profile.json` — seed, tempo, meter, key/mode, form, groove, fingerprints,
  active roles, and file inventory

The original six MIDI files directly in `musical-context` remain the neutral
16-bar sound-selection arrangement. The genre/profile folders are the exact
web-app performances to use when designing a mix for a particular profile.

## Ableton workflow

Import either the individual stems at the same start point or the full
arrangement. Allow Ableton to import the MIDI tempo and time signature if you
want playback to line up with the web audition automatically. The files contain
no program changes, volume, pan, or device selection, so they do not overwrite
the instruments being auditioned.

Jam2's 7/8 Jazz Fusion profile expresses its tempo as 156 eighth-note pulses.
Its MIDI therefore uses the time-equivalent DAW tempo of 78 quarter notes per
minute. `profile.json` records both values.

The drum file uses this stable rack map:

| Piece | MIDI note |
| --- | ---: |
| Kick | 36 |
| Cross-stick | 37 |
| Snare | 38 |
| Closed hat | 42 |
| Floor tom | 43 |
| Open hat | 46 |
| Mid tom | 47 |
| Crash | 49 |
| High tom | 50 |
| Ride | 51 |

## Regeneration

After rebuilding the Synth A/B native executable, refresh its exact performance
audit and then regenerate the MIDI from the repository root:

```powershell
$env:PATH = (Resolve-Path release).Path + [IO.Path]::PathSeparator + $env:PATH
experiments\synth-ab\build\jam2_sound_lab.exe experiments\synth-ab\site --audit-only
python experiments\synth-ab\tools\generate_profile_context_midi.py
python experiments\synth-ab\tools\generate_profile_context_midi.py --check
```

`profile-context-manifest.json` records every style/profile, note count, note
range, duration, and MIDI SHA-256 hash.
