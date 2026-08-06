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
2. Choose Acoustic, AbletonRock32, Ableton808, Electronic or Latin drums.
3. Optionally apply a style treatment. It changes filtering, envelope, drive,
   motion, space, kit bus settings and a small number of defining drum pieces;
   it never changes the selected musical pattern.
4. Replace any individual drum piece from a base kit or researched candidate.
5. Use `Designed style mix` to render the saved custom lineup over the current
   profile's unchanged deterministic performance.

Each base advertises its intended frequency slot. These are design boundaries,
not automatic mix recommendations or subjective scores.

AbletonRock32 is the capture-matched exception to the broad reusable bases. It
uses the experiment's bounded per-piece modal/noise banks (12 resonators and
four filtered-noise bands maximum), omits synth character layers, and uses
shell-only tom bodies with explicit head strikes. Muted metal pieces use the
bounded onset-softening control; its zero default
keeps every existing shared kit unchanged. The capture-matched configurations
remain local to `experiments/synth-ab`. The lab also exposes a bounded
source/model layer level independent of piece level. AbletonRock32 uses it to
mute the clap-prone open-hat model and explicit transient while retaining the
smooth modal/noise tail. Its tom detail banks reduce the long fixed fundamental
while retaining the two short head-impact noise bands. Long diffuse tom noise
tails are deliberately omitted so the model strike and revised tone stay clear.

Ableton808 applies the same capture-matched isolation to the supplied 808 Core
Kit. It is a fifth, independent base rather than a change to the shared
Electronic Kit. Its voices use deterministic phase and delay where the attack
time is part of the identity, high-passed short resonators for the rim, explicit
per-band velocity curves, and a dry compressor fitted from the complete
four-pattern suite. Its ride remains one voice whose ghost, normal and accent
behaviour comes only from velocity.
