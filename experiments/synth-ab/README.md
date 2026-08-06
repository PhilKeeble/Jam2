# Jam2 sound-design workbench

This experiment is an isolated, research-led environment for developing the
sound palette used by generated Jam2 ideas. It renders the same deterministic
Jam2 musical performance through:

- the exact current Jam2 reference renderer;
- a profile-aware voice assembled from DaisySP building blocks;
- a hybrid of the Jam2 and Daisy renders.

It is separate from the Jam2 application and is not part of the root CMake
build. Generated WAVs and downloaded dependencies remain under this experiment
and are ignored by Git.

Reference instruments should be recorded using the reusable
[`REFERENCE_CAPTURE_PLAYBOOK.md`](REFERENCE_CAPTURE_PLAYBOOK.md). The
standardized drum MIDI files under [`capture-midi`](capture-midi/README.md)
capture velocity response, repeated-hit variation, and complete release tails
for acoustic and electronic kits. The playbook also records the measurement and
ablation procedure to reuse for future melodic-instrument matching.

## Run on Windows

Double-click `build.cmd`, or run:

```bat
experiments\synth-ab\build.cmd
```

Run `open-workbench.cmd` after building. It starts a local server bound only to
`127.0.0.1` and opens the sound-design workbench in the default browser. Close
the separate server window when the listening session is finished.

Do not open `site\index.html` directly. Browsers commonly silence `file://`
media when it is routed through Web Audio, which the live tone, drive, and
space controls require.

The browser now has three focused pages:

- `Base instruments` creates named pitched sounds from the factory templates;
- `Base drums` creates named piece options inside the Acoustic and Electronic
  kit families;
- `Style mixer` assigns those library sounds to roles and plays the resulting
  lineup over an unchanged generated pattern.

The live browser controls are directional post-processing tests and default to
a neutral tone, drive, and space path. They do not rewrite the authoritative
WAV or silently change Jam2. Candidate synthesis is changed in the C++
renderer, regenerated, and remains inspectable.

## Base Instruments, Base Drums and Style Mixer

Open the workbench normally, then choose one of the three pages in the sidebar.
Sound synthesis controls appear only on the two base pages. Style Mixer is an
arrangement page: it contains the profile, role lineup, sound selectors and
mix playback, but no oscillator, envelope, drum-model, snapshot or raw-patch
controls.

Both base pages also expose an `Audition profile for generated pattern`
selector. It changes only the notes and groove used by the generated-style
audition; it does not attach that profile to the base sound or alter the saved
sound parameters.

The save boundaries are deliberate:

- Base edits are drafts until `Save as new sound` or `Update selected sound`
  is pressed. Named sounds live in browser `localStorage`.
- A saved instrument can be assigned to any pitched role. Names such as bass,
  pad or lead describe the starting sound and do not restrict placement.
- A saved drum option belongs to one kit family and one piece. Style Mixer can
  start from any complete kit family and replace each piece independently with
  a factory piece or any compatible named option.
- Style Mixer edits persist per profile and role in browser `localStorage`.
  Arrangements reference the named library sounds, so updating a sound is
  reflected when that arrangement is next loaded or played.
- `Save complete style mix` creates a self-contained project handoff in
  `presets/style-mixes.json`. Each profile snapshot contains every resolved
  pitched parameter, the complete resolved drum kit and bus, per-piece source
  choices, active layer flags, native-Jam2 selections, and the exact mixer
  gains. It does not depend on browser sound names after it is written.
- Every Style Mixer role can instead select `Current Jam2 native style sound`.
  This uses the exact generated Jam2 reference stem for that role and profile,
  and it can be combined independently with experimental roles in the same
  sample-aligned lineup.

The page sends bounded patch requests only to the local server on
`127.0.0.1`. The server invokes the experiment's native DaisySP renderer and
returns a mono PCM16 WAV; synthesis does not run in JavaScript and nothing is
written into Jam2.

The pitched lab covers every parameter in the experiment's polyphonic voice
path for chords, melody, bass, and supporting lines:

- Jam2 native plus all nine Daisy source architectures and four filter paths;
- independent source A and source B selection, equal-power blend, transpose,
  and fine tuning;
- source-specific oscillator, FM, additive, formant, VOSIM, Z-oscillator, and
  physical-string controls;
- amplitude and filter envelopes;
- transient, noise, wavefold, drive, and cabinet character;
- vibrato, tremolo, chorus, and delay;
- deliberately wide bipolar character macros and bounded mutations. Macro
  extremes reach across oscillator shape/layers, FM ratio and index, additive
  family, formants, Z spectral mode, physical-string response, filter and
  envelope contour, wavefold/drive/noise, glide/modulation and chorus/delay
  while remaining clamped to native renderer limits.

The Base Instruments page provides 13
shared foundations spanning sub and gliding basses, electric-style bass,
analogue poly, FM keys, atmosphere pad, arpeggiator pluck, modern and heavy
synth leads, and piano-, guitar-, harp- and violin-like compact models. In the
Style Mixer, every pitched role can independently use any factory or named
sound. The selected profile's bounded envelope, filter, drive, movement and
space treatment is applied automatically.

Drums have two shared foundations: Acoustic and Electronic. Their complete
piece sets are the same canonical kits promoted into Jam2. The
experiment-only native renderer instead accepts up to 12 independently tuned
modal bands and four independently filtered noise bands per piece. Frequency,
level, attack, decay, velocity response, articulation gain and room send are
bounded and preserved in the render result for inspection. A separate bounded
0–100 ms onset-softening envelope can reduce an underlying metal
model's initial burst without changing its decay; it defaults to zero for all
existing kits. Base
Drums edits one selected piece at a time and can save multiple
named alternatives—for example several Electronic kicks. Style Mixer starts
from one complete family and lets each lane use the selected kit, a factory
piece from another family, or a named piece option. The chosen instruments,
kit and pieces persist per profile/role and `Designed style mix` renders that
exact lineup over the unchanged style pattern. See
`BASE_PALETTE_RESEARCH.md` for the source and mix-boundary notes.

The Electronic kit's twelve pieces retain their fitted velocity and repeat
responses. The ride is one velocity-controlled voice, not a cup/rim
articulation split. The reference-pattern audition accepts exact bounded MIDI
events for repeatable source/generated comparisons.

Auditions include a single note, a register/velocity sequence, two polyphonic
chords, and the profile's actual generated eight-bar phrase. `Designed style
mix` schedules the custom role and the other currently active designed stems
from one Web Audio clock, so it does not introduce per-element start delays.
Drum voices
have a dedicated per-piece Kit Lab covering kick, snare, closed/open hats,
high/mid/floor toms, crash, ride, and cross-stick. Every profile
exposes three coherent complete-kit candidates (81 kits across the current 27
profiles), with the feedback focus loaded on every page load. A
second selector can replace only the currently selected piece with its
equivalent from either alternate kit; any manual edit clearly changes the
complete-kit selection to `Custom`.

Loop-Centred Pop and Sectional Pop intentionally share Hybrid Section Lift as
their recommended kit, because their distinction is musical generation rather
than drum timbre. The audit permits this one named reuse and still rejects
accidental candidate convergence elsewhere.

Manual patches can use the Jam2 native sound, legacy profile voice, Daisy
analogue/synthetic kick or snare, square or ring-mod metal, modal or particle
resonance, procedural cymbals, or a sample-aligned two-source blend. The
researched candidates are more constrained: source suitability is a hard
per-piece contract, and their tom, cross-stick, wood-block, hand-clap, crash,
ride, and short shell-snare voices use dedicated models.
None of the 972
researched piece patches uses the generic modal, particle, or legacy
one-size-fits-all cymbal source. Piece pitch, tone, decay,
noise/snap/metal colour, pitch/FM sweep, and level remain explicit. Researched
patches additionally expose:

- an independent source/model layer level, leaving modal/noise detail banks at
  the piece level so a model transient can be removed without losing its tail;
- independent beater/stick/rim/click/brush/clap transient construction;
- independent wire/dust/particle/air/metal-wash texture;
- per-voice drive, internal source rate, bit depth, reconstruction low-pass,
  and dynamic reconstruction response;
- per-piece shared-room send plus explicit kit room return, size, and damping;
- deterministic ghost/normal/accent velocity bands with separate model
  excitation and output curves, plus bounded brightness/decay/drive response;
- named choke groups and fade time, currently used for open/closed hats.
Each piece also has an independent optional synth character layer. It uses any
of the nine pitched Daisy architectures and exposes MIDI pitch, level, gate,
attack, decay, sustain, release, noise mix, and low-pass cutoff. The synth is
triggered sample-aligned with its drum piece, added after the two-drum
equal-power blend, and processed by the same complete-kit bus.
The optional layer remains available where a piece calls for it. The Acoustic
kit uses its per-piece modal/noise detail banks for measured shell,
metal and broadband components instead. Its open hat mutes the Daisy model and
explicit transient completely, then uses only slower-attack detail bands for a
smooth, fixed-pitch wash. Its toms retain the shell-model strike and body but
reduce the long, stationary fundamental while retaining only short filtered
noise bands for useful head texture, without an added diffuse noise tail.
Every profile defines all twelve sounds even when its generated groove does not
normally use a particular lane. A selected-piece audition inserts one clearly
reported test hit only when needed, so dormant kit sounds never audition as
unexplained silence. The separate `One-shot` test always renders exactly one
selected hit without depending on the style pattern. `Ghost / normal / accent
ladder` exposes the semantic velocity mapping, and `Deterministic repeated
hits` checks bounded variation and machine-gun behaviour before the selected
piece is heard in its profile groove or complete kit.

`Designed style mix` renders every arranged role before starting the complete
lineup on one Web Audio clock. Jam2-native selections load their current
reference stems on that same clock; unassigned roles use researched defaults.
Every mixed audition uses gain `1.16` for drums,
`0.82` for a normal backing role, and `0.68` for support. The drum/non-drum
relationship is therefore exactly `+3 dB`; isolated renders are unaffected.

The local service rejects non-local browser origins, requests larger than
64 KiB, unknown profiles or roles, and out-of-range synthesis parameters.
Rendered audition WAVs are cached under `site/lab-renders` with a bounded
96-file limit.

Use `build.cmd --catalog-only` to render only the workbench catalogue or
`build.cmd --showcase-only` to render only the DaisySP capability page.
Catalogue rendering uses up to eight parallel worker processes by default.
Override that with `--jobs N`, for example
`build.cmd --catalog-only --jobs 12`.

Add `--profile PROFILE_ID` or `--style STYLE_ID` to regenerate only selected
catalogue entries while retaining all other profiles already present in the
manifest and audio directory. Options can be repeated and their selections are
combined, for example
`build.cmd --catalog-only --jobs 8 --style pop --profile rock_riff_modal`.
Without either filter, catalogue generation still replaces and regenerates the
complete catalogue.

Use `build.cmd --audit-only` to regenerate `site\seed-audit.json` without
rendering audio. The audit records every fixed profile seed, the complete
research recipe, and the concrete eight-bar MIDI voicings and lane events used
by the experiment.

## DaisySP capability page

`site\showcase.html` isolates oscillator shapes, FM, additive synthesis,
wavefolding, filters, and six compact source-versus-designed voices. It is a
diagnostic companion to the profile catalogue rather than a style comparison.

## Audition policy

- Each profile uses a fixed seed and an eight-bar excerpt.
- Candidate stems receive identical notes, velocities, durations, meter,
  groove, and articulation.
- Hybrid stems preserve the shared sample-accurate event onset with no
  artificial source delay. Different attack envelopes remain part of each
  layer's timbre. Hybrid bass uses the Jam2 low fundamental below 185 Hz and
  Daisy mid/high colour so two full-band fundamentals do not create accidental
  beating.
- Output is RMS-matched with a peak ceiling for useful comparisons.
- Daisy drum candidates use profile-aware voice construction plus a disclosed
  drive, low-pass, and modest peak-control bus. The raw threshold, ratio, and
  release values are shown in each drum role's technical data.
- The current Jam2 render remains available as a legitimate candidate.
- Profile candidates are deliberately compact synthesis targets, not claims
  of recorded acoustic realism or clones of commercial presets.
- Daisy designs deliberately distribute roles across variable-shape,
  variable-saw, FM, additive, physical-string, sine, phase-reset formant,
  VOSIM, and Z-oscillator sources instead of routing most profiles through one
  generic oscillator.
- Named instrument sounds, drum-piece options and working style arrangements
  stay in browser `localStorage`. Explicit complete-style snapshots are written
  only to the local experiment's `presets/style-mixes.json` handoff file; there
  is no remote library or upload workflow.

`tools\audit_wavs.py` reports file/scene signal health and Jam2/Daisy envelope
timing. `tools\audit_timbres.py` reports raw source/filter coverage, spectral
bands, centroid, rolloff, flatness, zero-crossing, difference ratio, and crest
factor for the pitched Daisy designs. These are diagnostics, not quality
scores.

`tools\audit_drum_candidates.py` renders the complete researched kit matrix.
Its full scope checks all 81 complete candidates, all 972 candidate one-shots,
and semantic velocity ladders and repeated-hit auditions for all twelve
recommended pieces in every profile: 1,701 renders in total. Structural gates
enforce intended identity and suitable source families for every piece,
purposeful within-profile alternatives, and non-aliased recommendations across
profiles. Rendered gates check instrument-specific duration and spectral
relationships, including short wooden cross-sticks, ordered tom pitches,
short shell-led snares, separately identifiable crash and ride behaviour, and
the ride's edge/bow, bow, and centre/bell velocity articulations. Reports and
WAVs are written only to the ignored `.research-cache`.

The candidate generator also enforces conservative presence safeguards learned
from listening review: style-sensitive closed-hat level/attack floors, minimum
closed-hat envelope duration and normal velocity bands, usable cross-stick
ghost/normal bands, longer wood/rim energy and flatter output curves, and a
cap on the pitched part of normal/accent ride articulations. These do not
replace each kit's source, tuning, or room identity, and ride
ghosts bypass the pitched-ring reduction.

Except for the explicitly native feedback-focused Reggae kit, every researched
snare combines the Jam2 shell/wire body with Daisy synthetic impact. Shell-led
brush, rim, traditional, and modern designs use different reinforcement
amounts; machine-led designs retain a smaller shell anchor. Rock and Metal
reverse that relationship into a driven synthetic-smack lead with
candidate-specific shell blend and close-mic filtering.

The Reggae feedback focus is `Jam2 Roots + Warm Crash`: every piece except the
crash uses an isolated native Jam2 drum lane at unity, and its crash is an
exact copy from Warm Rhythm-Box Roots. The two original complete alternatives
remain available.

The ride model damps successively higher inharmonic modes more strongly and
shortens their decay, with reduced upper noise-band wash. The principal
bow/bell mode and semantic edge-bow, bow, and centre-bell mapping remain
intact; ride-led acoustic styles receive slightly stronger damping.

Electronic-metal-underlay kicks use the Daisy synthetic/analogue drum pair
without an additional FM synth tail. The audit rejects `fm2` on researched
kicks; intentional sine-fundamental reinforcement remains available to
Trap/808 and other explicitly sub-led designs.

`tools\analyze_groove_midi.py` analyses the licensed Magenta Groove MIDI
Dataset for velocity and groove evidence. The paired GMD WAVs all represent
the same Roland TD-11/e-kit capture, so they are not genre-timbre targets; the
dataset is primarily evidence for generation, articulation, timing, dynamics,
and inter-piece relationships.

## Pinned dependency

- DaisySP: `599511b740f8f3a9b8db72a0642aa45b8a23c3a3`

DaisySP uses a permissive licence. Its licence is copied into `site/licenses`
when the renderer runs.
