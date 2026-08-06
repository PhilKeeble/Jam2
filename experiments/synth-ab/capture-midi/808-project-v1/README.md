# DrumSamples 808 mapped pattern MIDI

These patterns use the note mapping recovered from the MIDI clips in
`DrumSamples.als`. They are project-specific and intentionally do not use the
generic GM mapping from `drums-v1`.

## Recovered mapping

| Exported piece | ALS track | MIDI note | Ableton name |
| --- | --- | ---: | --- |
| Kick | Kick | 36 | C1 |
| Cross-stick | cross stick | 37 | C#1 |
| Snare | Snare | 38 | D1 |
| Clap | percussion - clap | 39 | D#1 |
| Ride | ride | 41 | F1 |
| Closed hat | Closed Hats | 42 | F#1 |
| Floor/low tom | Low Tom | 43 | G1 |
| Mid tom | Mid Tom | 45 | A1 |
| Open hat | Open Hats | 46 | A#1 |
| High tom | Hi Tom | 47 | B1 |
| Crash/cymbal | Cymbals | 51 | D#2 |

The ALS `ride` clip still has the inherited name `Jam2 Ride cup / bell capture
v1`, but this 808 kit has one ride articulation on note 41. Low, normal, and
high velocities represent ghost, normal, and accent ride hits; there is no
separate tip/cup mapping for this kit.

## Pattern sets

The quickest route is `comparison-suite`: place its 11 stem MIDI files on the
11 retained isolated tracks at bar 1, then export once. It contains all four
patterns with two silent bars between them. The patterns begin at bars 1, 11,
21, and 31, and the complete suite ends at bar 39 (38 bars / 76 seconds).

Use `comparison-suite/full-kit.mid` instead when working on one track containing
the complete 808 rack.

The `patterns` directory also provides every groove separately. Each directory
contains an eight-bar, 120 BPM `full-kit.mid` plus 11 sample-aligned files under
`stems`:

- `01-straight-808-pop`: overall balance, clap/snare layering, hats, and a short
  tom fill.
- `02-syncopated-electro`: denser kick, sixteenth hats, and transient
  layering.
- `03-ride-groove`: ghost, normal, and accent response on the single ride voice,
  plus repeated-hit smoothness and accumulated metallic tails.
- `04-tom-fill-context`: six bars of half-time context followed by a two-bar tom
  fill.

Use `full-kit.mid` on one track that contains the complete 808 rack. To render
individual audio stems from the existing isolated Ableton tracks, put each MIDI
file from `stems` on the correspondingly named track at the same start point.
All stem clips are exactly eight bars, including pieces with no events in a
particular pattern.

Please export both:

- one mono or stereo full-kit mix per pattern at the kit's intended relative
  levels; and
- All Individual Tracks as 48 kHz, 24-bit WAV with Normalize and Dither off.

The complete mix is the primary reference for balance and tail buildup. The
individual tracks let each synthesized piece be diagnosed when the mix differs.

Regenerate or validate the pack from the repository root:

```powershell
python experiments/synth-ab/tools/generate_808_pattern_midi.py
python experiments/synth-ab/tools/generate_808_pattern_midi.py --check
```

`manifest.json` records the recovered mapping, single-ride velocity use, event
counts, and SHA-256 hash of every generated file.
