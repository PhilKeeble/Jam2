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

The browser provides:

- all 26 researched profiles plus the experimental modern-metal profile;
- synchronized candidate stems for each generated role;
- scene-wide and per-role Jam2, DaisySP, and hybrid source selection;
- live level, tone, drive, and space audition controls;
- an Instrument + Kit Lab that changes the real native Jam2/Daisy source
  architecture and returns a newly rendered WAV;
- locally persisted profile choices and refinement notes;
- expandable raw synthesis parameters.

The live browser controls are directional post-processing tests and default to
a neutral tone, drive, and space path. They do not rewrite the authoritative
WAV or silently change Jam2. Candidate synthesis is changed in the C++
renderer, regenerated, and remains inspectable.

## Instrument + Drum Kit Lab

Open the workbench normally, then select `Instrument + Kit Lab` in the sidebar.
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
- semantic character macros, bounded mutations, A/B/C snapshots, and
  listening notes.

Auditions include a single note, a register/velocity sequence, two polyphonic
chords, and the profile's actual generated four-bar phrase. `Designed style
mix` schedules the custom role and the other currently active designed stems
from one Web Audio clock, so it does not introduce per-element start delays.
Drum voices
have a dedicated per-piece Kit Lab covering kick, snare, closed/open hats,
high/mid/floor toms, crash, ride, cross-stick, shaker, and hand percussion. Every profile
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
per-piece contract, and their tom, cross-stick, shaker, hand-drum, wood-block,
tambourine, hand-clap, crash, ride, and short shell-snare voices use dedicated models.
None of the 972
researched piece patches uses the generic modal, particle, or legacy
one-size-fits-all cymbal source. Piece pitch, tone, decay,
noise/snap/metal colour, pitch/FM sweep, and level remain explicit. Researched
patches additionally expose:

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
Every profile defines all twelve sounds even when its generated groove does not
normally use a particular lane. A selected-piece audition inserts one clearly
reported test hit only when needed, so dormant kit sounds never audition as
unexplained silence. The separate `One-shot` test always renders exactly one
selected hit without depending on the style pattern. `Ghost / normal / accent
ladder` exposes the semantic velocity mapping, and `Deterministic repeated
hits` checks bounded variation and machine-gun behaviour before the selected
piece is heard in its profile groove or complete kit.

The Drum Kit Lab has four deliberately separate comparison paths:

- `Render & play` / `Replay` auditions the edited kit alone.
- `Jam2 drums only` plays the original Jam2 drum render of the exact same
  deterministic profile pattern.
- `Edited drums + Jam2 backing` places the edited kit against the original
  Jam2 chords, melody, bass, and support stems.
- `Jam2 drums + Jam2 backing` is the fixed all-Jam2 A/B reference.

`Designed style mix` remains available separately. It renders every role from
its currently active browser patch or kit before starting them on one Web
Audio clock. Unvisited roles use their researched defaults. Loading A/B/C
makes that snapshot active for later designed mixes; loading an empty snapshot
restores the researched default. The Jam2-backed comparisons do not read those
saved Daisy/design patches, so drum evaluation cannot silently change its
backing instrumentation. Every mixed audition uses gain `1.16` for drums,
`0.82` for a normal backing role, and `0.68` for support. The drum/non-drum
relationship is therefore exactly `+3 dB`; isolated renders are unaffected.

`Save preset for Jam2 review` writes the current versioned design to
`presets/workbench-presets.json`. Each profile/role has one inspectable record
containing the research context, notes, source choices, blends, and raw
parameters. Candidate records also name the research family and source
references. The raw panel includes the last render's realised per-hit MIDI
velocity, model excitation, output gain, brightness/decay/drive response, and
objective signal/envelope/band-energy diagnostics. Saving here does not change
Jam2; it creates the handoff used for
listening, polishing, and later implementation. A complete record can also be
copied from the raw JSON panel.

The local service rejects non-local browser origins, requests larger than
64 KiB, unknown profiles or roles, and out-of-range synthesis parameters.
Rendered audition WAVs are cached under `site/lab-renders` with a bounded
96-file limit.

Use `build.cmd --catalog-only` to render only the workbench catalogue or
`build.cmd --showcase-only` to render only the DaisySP capability page.
Use `build.cmd --audit-only` to regenerate `site\seed-audit.json` without
rendering audio. The audit records every fixed profile seed, the complete
research recipe, and the concrete four-bar MIDI voicings and lane events used
by the experiment.

## DaisySP capability page

`site\showcase.html` isolates oscillator shapes, FM, additive synthesis,
wavefolding, filters, and six compact source-versus-designed voices. It is a
diagnostic companion to the profile catalogue rather than a style comparison.

## Audition policy

- Each profile uses a fixed seed and a four-bar excerpt.
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
- Working preferences and notes stay in browser `localStorage`. Only the
  explicit `Save preset for Jam2 review` action writes a preset record inside
  this experiment; there is no remote service or download workflow.

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
replace each kit's source, tuning, room, or hand-percussion identity, and ride
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
