# Shared base-palette research

This note records the design reasoning behind the workbench's reusable
instrument and drum foundations. The templates are synthesis targets, not
claims of acoustic realism or attempts to clone proprietary presets.

## Directional listening references

The three links in `Bugs.md` were used as directional targets at their noted
timestamps:

- `miTyMfxxWCo`, around 1:30: dense heavy synth layering;
- `lFodQSX2i0c`, opening track and around 8:00: trap low end and a contrasting
  dance/e-kit balance;
- `VagWXGotYRg`, first half and around 28:00: hard modern-metal drums and
  synth/metal integration.

They inform the Heavy Synth, long sine-reinforced 808 kick, and hard acoustic
kick/snare replacements. No audio is copied or sampled.

## Synthesis decisions

- DaisySP's variable-shape, variable-saw, FM, formant, VOSIM, Z, additive and
  physical-string primitives are kept as the small source vocabulary. See the
  [official DaisySP namespace reference](https://electro-smith.github.io/DaisySP/namespacedaisysp.html)
  and [VariableShapeOscillator source reference](https://daisy.audio/DaisySP/variableshapeosc_8h/).
- Glide is a real native pitch transition, with a bounded 0–1.5 second time,
  rather than a name applied to an ordinary bass patch. The control follows
  the conventional previous-note-to-current-note behavior described in the
  [Ableton instrument reference](https://www.ableton.com/en/manual/live-instrument-reference/).
- The arpeggiated audition turns held chord tones into a timed sequence, so
  pluck length and masking can be judged in their intended use. Rate and gate
  are the key audible dimensions described in the
  [Ableton MIDI effect reference](https://www.ableton.com/en/live-manual/11/live-midi-effect-reference/).
- Piano-like, guitar-like, harp-like and violin-like templates focus on the
  exciter, string/body and damping relationships used by physical models. The
  acoustic labels remain explicitly “-like”.

## Mix architecture

The base palette separates source identity from treatment:

1. Choose an independent base for chords, melody, bass and support.
2. Choose Acoustic, Electronic or Latin drums.
3. Optionally apply a style treatment. It changes filtering, envelope, drive,
   motion, space, kit bus settings and a small number of defining drum pieces;
   it never changes the selected musical pattern.
4. Replace any individual drum piece from a base kit or researched candidate.
5. Use `Designed style mix` to render the saved custom lineup over the current
   profile's unchanged deterministic performance.

Each base advertises its intended frequency slot. These are design boundaries,
not automatic mix recommendations or subjective scores.
