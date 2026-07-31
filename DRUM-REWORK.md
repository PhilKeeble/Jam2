# Jam2 Production Drum Rework

## Status

This document is the durable brief and working log for the production drum
rework requested after the Drum Kit Lab listening passes.

The end state is not merely a new collection of drum sounds. Jam2 must generate
coherent, stylish drum performances that feel deliberately played or
deliberately programmed, use the accepted researched kit for the selected
profile, remain deterministic and inspectable, and sit at the accepted level in
the Jam2 reference mix.

## Final validation — 2026-07-31 Europe/London

The requested production drum rework is implemented and its final validation
set is clean:

- The exact production/Lab parity report covers all `27` profiles and all
  twelve pieces. All `313/313` directly comparable renders pass. The other
  eleven pieces are the selected Reggae kit's `jam2-native` lanes, which
  deliberately call the production renderer and are explicitly reported as
  shared-native rather than compared through a second implementation.
- The exact-engine candidate audit contains `1,701` unique renders: all `81`
  candidates, `972` one-shots, and every recommended piece's velocity ladder
  and repeated-hit sequence. It has zero renderer errors, semantic findings,
  structural findings, or cross-kit relationship findings.
- The native Reggae audit path now distinguishes exact generated performance
  excerpts from deliberately authored one-shot/ladder/repeat grids. Artificial
  auditions clear the stale production event list and render their explicit
  twelve-lane pattern; representative profile auditions retain the exact
  version-7 performance.
- Closed-hat identity checks are calibrated to the exact production voices.
  Bright and intentionally damped/rhythm-box hats are accepted only when the
  combined spectrum, low-ring bound, decay, and transient crest remain
  hat-like. This avoids reintroducing the earlier bell/gong failure while no
  longer treating every dark hat as a full-band modern cymbal.
- The regenerated structural corpus contains `888` ideas: every one of the
  `27` profiles, all `74` native forms, complexities `1`, `4`, and `8`, and
  four deterministic seeds. Median hits/bar remain
  `12.500 / 12.542 / 12.517`; median unique core-bar ratios remain
  `0.917 / 0.917 / 0.917`; exact two-bar core recurrence remains
  `0.000 / 0.000 / 0.000`; and median phrase/fill boundary count remains
  `4 / 4 / 4`.
- The final release-rendered audio corpus contains all `74` drum stems and all
  `74` Jam2 backing mixes. Drum stems have median peak `0.573899`, median RMS
  `-22.518 dBFS`, and maximum peak `0.955688`. Mixes have median peak
  `0.400421`, median RMS `-23.731 dBFS`, and maximum peak `0.519958`.
  Neither corpus contains a PCM sample at or above the `0.979` safety
  threshold.
- The positive-slope rational knee removed the Reggae plateau. Across every
  stem, the longest run of one identical high-level PCM value is eight samples
  (`0.167 ms` at 48 kHz); the longest run at or above `0.95` is six samples.
  The renderer still reports `96,449` float samples above the linear knee
  across eight forms, but it shapes rather than clamps them.
- The promoted production JSON exactly matches a fresh extraction of all `27`
  recommended Lab kits when the generated timestamp is excluded. Experimental
  melodic synthesis remains unpromoted.
- All `12` drum-research/signal regression tests pass. Production boundary
  validation returns `ok: true`. The required elevated MSVC build succeeds to
  the single public binary `release/jam2.exe`; its SHA-256 is
  `0EBBBCB2C89C74E6520869517001436C13367F42A7CEEDBCCFBE4EC583423979`.
  `release/licenses/DaisySP-LICENSE.txt` is staged beside the release.

Authoritative generated evidence is under
`artifacts/drum-research/jam2-corpus/`,
`experiments/synth-ab/.research-cache/drum-candidate-audit/report-full.json`,
and `experiments/synth-ab/site/drum-production-parity.json`.

## Resume checkpoint — 2026-07-30 23:45 Europe/London

This is the historical checkpoint used to resume the final pass. At that time,
the feature implementation was substantially complete. Do not restart the
research, kit promotion, lane migration, or drummer planner. The remaining work
listed below was completed by the final validation above.

### Completed and validated

- Production recipe version `7`, the twelve-lane beat schema, legacy Tom to Mid
  Tom migration, profile-native phrase planning, complexity-independent drummer
  rules, exact performance events, three-tom fills, velocity bands,
  microtiming, repeat groups, cymbal/hat/ride articulation, and deterministic
  diagnostics are implemented.
- One recommended twelve-piece researched kit is embedded for each of all `27`
  profiles. The Drum Kit Lab retains its alternatives and editable controls;
  experimental chord, melody, bass, and support synthesis has not been promoted.
- The newest structural corpus completed `888` ideas over all `27` profiles,
  all `74` native forms, complexities `1`, `4`, and `8`, and four deterministic
  seeds. Its complexity medians are:

| Complexity | Hits/bar | Unique core bars | Exact two-bar core recurrence | Phrase/fill boundaries |
|---:|---:|---:|---:|---:|
| 1 | 12.500 | 0.917 | 0.000 | 4 |
| 4 | 12.542 | 0.917 | 0.000 | 4 |
| 8 | 12.517 | 0.917 | 0.000 | 4 |

- The newest audio corpus completed all `74` native-form drum stems and all
  `74` Jam2 backing mixes. The first post-`+8 dB` measurement pass found:
  median drum-stem peak `0.573899`, median stem RMS `-22.518 dBFS`, maximum
  mastered-mix peak `0.522766`, median mastered-mix RMS `-23.716 dBFS`, and
  zero mastered-mix samples at the safety ceiling.
- The managed drum lane starts at `0 dB`; regeneration preserves an existing
  user fader value. The accepted internal recipe `+3 dB` remains part of kit
  voicing, and the requested additional `+8 dB` is baked after the drum bus.
- The Drum Kit Lab now preserves the shortened idea's valid fingerprints and
  uses exact production version-7 events for profile and selected-piece groove
  auditions. A post-build Sectional Pop audit reported `40` exact events,
  `35` microtimed events, `4` fill events, and non-empty production
  articulation on all `40` rendered hits.
- The production release build and boundary suite passed before the two
  unbuilt source-only corrections listed below. All `12` Python
  research/signal regression tests passed.

### Source changes pending at that checkpoint (now built)

Two small corrections were present in source but had not yet been rebuilt or
accepted by the final gates:

1. `experiments/synth-ab/src/SoundDesignMain.cpp` now passes the exact
   production articulation into both parity-render paths. The previous parity
   comparator replaced it with generic `ghost/normal/accent`. That caused the
   only two comparable failures: Funk `dry-ghost-pocket` kick and Bebop
   `simmons-colour` high tom. Before this correction, `311` of `313`
   directly comparable profile/piece cases passed; all other residuals were
   at or below the `0.2%` float round-trip tolerance. Reggae's eleven
   `jam2-native` lanes are reported separately because the Lab deliberately
   calls the production Jam2 lane renderer for those lanes rather than
   approximating them through `ResearchDrumEngine`.
2. `app/gui/PracticeReferenceRenderer.cpp` now uses a rational soft knee above
   `0.78` instead of a float-precision `tanh` asymptote. The first `+8 dB`
   corpus was healthy except for the deliberately loud native Reggae stem:
   its two forms accounted for `84,699` of `96,477` soft-limited float
   samples and `10,340` PCM samples at or above `0.979`; the longest
   ceiling-adjacent run was `120` samples (`2.5 ms`). The rational curve
   remains linear below `0.78`, approaches `0.98`, and retains a positive
   slope so hot native-kit overlaps do not become identical-sample plateaus.
   This is a systemic output-stage correction, not a change to the accepted
   Reggae kit or groove.

The first resumed parity reruns also exposed a pinned DaisySP defect rather
than another profile mismatch. `SyntheticBassDrum::Init` did not initialize
`transient_env_` or `transient_env_lp_`, so the first click/noise transient
could depend on heap contents. Exactly one synthetic-layer kick failed per
parity run, but the affected profile moved between Blues Minor and Boom-Bap.
`cmake/PrepareDaisySPDrums.cmake` now produces a build-local corrected
translation unit for both production and the Lab, verifies the exact pinned
source anchor, and leaves the vendored DaisySP source untouched. This
correction must be included in the resumed rebuild before parity is accepted.

### Work that remained at that checkpoint

1. Rebuild the Lab and rerun `--verify-drum-parity`. The expected result is
   zero failures among all `313` directly comparable cases, with the eleven
   production-native Reggae lanes explicitly identified as shared-native
   rather than falsely compared through a different renderer.
2. Run the complete Drum Kit Lab `full` audit: `81` candidates, `972`
   one-shots, and velocity/repeat/identity checks for `1,701` renders. Inspect
   any finding musically; do not suppress a genuine piece-identity,
   pitch-order, decay, articulation, silence, or overload failure.
3. Rebuild production and regenerate the `74` audio forms with the rational
   soft knee. Re-run the stem and mix audits. Confirm no non-finite audio, no
   mastered-mix ceiling contact, no long ceiling-adjacent drum-stem runs, and
   no new profile outlier. The `888` structural results need not change.
4. Replace provisional audio figures later in this document with the final
   rerendered measurements and record the final parity/full-audit result in
   both drum research documents.
5. Run all Python drum-research tests, production boundary validation, and the
   required elevated Windows MSVC build. Verify that the tested binary is
   exactly `release/jam2.exe` and that
   `release/licenses/DaisySP-LICENSE.txt` is staged.

### Historical resume commands

From the repository root:

```bat
cd experiments\synth-ab
call build.cmd --audit-only
set PATH=C:\Qt\6.10.3\msvc2022_64\bin;%PATH%
build\jam2_sound_lab.exe site --verify-drum-parity
cd ..\..
python experiments\synth-ab\tools\audit_drum_candidates.py --root experiments\synth-ab --scope full
```

After the production rebuild:

```bat
release\jam2.exe debug run tools\scenarios\music-full-form-corpus.json
python tools\drum_research\analyze_jam2_corpus.py artifacts\drum-research\jam2-corpus\full-form-corpus.json --output artifacts\drum-research\jam2-corpus\full-form-analysis.json
python tools\drum_research\audit_rendered_audio.py artifacts\drum-research\jam2-corpus\audio\drum-stems --output artifacts\drum-research\jam2-corpus\drum-stem-audio-audit.json
python tools\drum_research\audit_rendered_audio.py artifacts\drum-research\jam2-corpus\audio\full-form-corpus --output artifacts\drum-research\jam2-corpus\mix-audio-audit.json
```

The final required build command remains:

```bat
cmd.exe /d /c "call compile.cmd --in-dev-shell"
```

Important generated evidence is under
`artifacts/drum-research/jam2-corpus/`; the latest Lab parity report is
`experiments/synth-ab/site/drum-production-parity.json`. The final validation
section above records that every checkpoint item now passes.

## User intent

- Promote the feedback-selected drum sounds for every profile from the
  experiment into Jam2.
- Keep the Drum Kit Lab available for later editing and candidate comparison.
- Do not promote the experimental melodic, chord, bass, or support sounds.
- Expand the single generic tom into high, mid, and floor toms so fills can be
  orchestrated naturally.
- Use the local Google Magenta Groove MIDI Dataset as the primary evidence for
  real drummer timing, dynamics, coordination, repetition, groove development,
  and fills.
- Add complementary research evidence where Magenta has weak style or meter
  coverage.
- Combine performance evidence with the existing style, drum-machine,
  synthesis, reference-track, and listening research in
  `DRUMKIT-RESEARCH.md`.
- Remove global idea complexity from drum generation. Complexity may continue
  to affect harmonic and melodic roles, but it must not act as a probability
  knob for random extra drum hits.
- Preserve a drummer's own profile-appropriate expressive vocabulary. Removing
  the global complexity control does not mean making every drum part simple:
  it means selecting and pacing kick alternatives, ghosts, articulations,
  cymbal changes, fills, substitutions, linear cells, double-kick figures, and
  other techniques for musical reasons established by the style and phrase.
- Make every generated drum track musically convincing for its profile without
  asking the user to understand or tune a drummer-complexity control.
- Preserve the accepted kit voicing, including its existing internal `+3 dB`
  recipe gain, then bake an additional `+8 dB` into the finished generated
  drum stem. Keep the managed lane at `0 dB` so the user retains the full
  upward fader range.
- Record measurements, limitations, decisions, and validation results as the
  work proceeds.

## Product boundary

Jam2 remains a small, deterministic practice and collaboration application.
This work does not add:

- a trained generative model at runtime;
- cloud inference or a network dependency;
- a general DAW drum editor;
- sample-library management;
- a user-facing matrix of obscure drummer parameters;
- automatic subjective quality scores;
- experimental melodic synthesis in the production application.

The dataset work produces compact, authored, inspectable rules and calibrated
distributions that run locally.

## Evidence hierarchy

No source is allowed to answer a question it does not measure reliably. Source
licensing is recorded for provenance, but this work does not copy, bundle, or
redistribute source MIDI/audio and does not transplant recognisable patterns.
The production result is Jam2's original deterministic rule system informed by
aggregate measurements and musical analysis.

| Priority | Source | Appropriate evidence | Important limitations |
|---|---|---|---|
| 1 | Local Google Magenta Groove MIDI Dataset v1.0.0 | Human onset timing, MIDI velocity, kit-piece relationships, repeated hits, drummer differences, beats, fills, tempo and labelled style | 1,138 of 1,150 performances are 4/4; sparse modern electronic, Trap, Metal and odd-meter coverage; one electronic-kit capture is not a production-timbre reference |
| 2 | Existing Jam2 style and drum synthesis research | Profile intent, drum-machine behaviour, synthesis architecture, modern reference listening, accepted kit identity | Does not by itself prove realistic performance distributions |
| 3 | Drum Groove Corpora | Professional microtiming comparison across a broader drummer population, including recorded popular music and Heavy Metal | Primarily onset/microtiming evidence; reduced kit vocabulary; not a replacement for Magenta velocity and fill data |
| 4 | Freesound Loop Dataset subsets | Electronic loop vocabulary, layer density, pattern and genre conventions in House, Techno, Breakbeat, Drum and Bass and related styles | Audio loops do not provide trustworthy performed MIDI velocity; onset transcription and source quality must be verified |
| 5 | Slakh2100 | Drum relationship to bass, guitar and full-arrangement sections in aligned multitrack MIDI | General MIDI arrangements and synthetic rendering are weak evidence for human microtiming, dynamics, profile labels, or drum sound |
| 6 | Focused primary references and manual transcription | Underrepresented modern Metal/Metalcore, Trap, electronic, odd-meter and profile-specific behaviour | Use principles and measurements; do not reproduce copyrighted performances |

Google E-GMD is not a second symbolic groove corpus. It re-records the original
GMD sequences through 43 kits and is useful only if a later audio/transcription
question needs those renders.

All external corpus files remain outside the repository and application.
Research notes identify their provenance and limitations. Jam2 stores neither
the source material nor direct pattern copies.

The user explicitly confirmed on 2026-07-30 that these corpora are research
references only. Licence review is therefore not a gate on aggregate analysis:
no third-party MIDI or audio is copied into Jam2, no source performance is
transplanted, and no recognisable source pattern is emitted. Provenance and
licence remain recorded solely so the evidence can be audited.

## Known local Magenta coverage

The local metadata currently contains:

- `1,150` performances from `10` anonymised drummers;
- `503` beats and `647` fills;
- `18` top-level style families;
- `1,138` performances in 4/4, five in 6/8, five in 3/4, one in 5/4, and one
  in 5/8;
- substantial Rock, Funk, Jazz, Latin, Hip-hop, Soul, Afro-Cuban, Punk,
  New Orleans, Country, Pop, Reggae, Gospel and Afrobeat material;
- useful sublabels including Rock half-time, Jazz-Funk, Jazz Fusion, Motown,
  Brazilian Baião, Samba, Bossa, Disco and Breakbeat.

These figures must be reproduced by the checked-in analysis tool rather than
remaining hand-counted notes.

## Current Jam2 problems to replace

### Performance generation

The current generator:

1. selects one authored two-bar `GrooveDef`;
2. repeats its first and second bars;
3. uses the global `recipe.complexity` value to raise probability tables for
   extra kicks, snare ghosts, cymbal substitutions and fills;
4. adds at most a small number of generic advanced cells;
5. writes every acoustic tom event into one `Tom` lane.

This can increase density without producing a stronger musical reason for the
change. Fill selection is mostly one final-beat special case, and common
acoustic fills are two hits on the same generic tom.

### Performance representation

The visible beat grid represents `g`, `x`, and `a`, but generated recipes do
not currently preserve a complete realised drum performance containing exact
per-hit velocity, timing offset, articulation, repeat context, and fill role.
The renderer therefore has too much responsibility for inventing performance.

### Sound rendering

Production Jam2 currently maps broad patch-name substrings to a small integer
kit and renders one compact hard-coded model per instrument. It does not
contain the lab's per-piece source blends, synthesis layers, transient and
texture components, velocity response, choke relationships, kit bus, or exact
profile defaults.

### Persistence

Beat lanes are saved as positional arrays without lane names or a lane-schema
version. Inserting new tom lanes directly would remap old Cross-stick, Shaker,
and Hand Percussion data onto the wrong instruments.

### Baseline mix

The accepted Drum Kit Lab comparison uses drum gain `1.16` against `0.82` for
a normal backing role, which is approximately `+3.01 dB`. Production Jam2
originally expressed that through the recipe. Subsequent in-app listening
showed that generated drums still needed substantially more presence. The
final requested policy retains that engine voicing and adds `+8 dB` after the
kit bus, not by striking every drum harder and changing its timbre.

## Target performance architecture

### Style/profile groove vocabulary

Each profile owns a bounded vocabulary containing:

- one or more core pockets;
- compatible kick grammars;
- backbeat, cross-stick, rim, clap, or snare roles;
- timekeeping voice and articulation rules;
- phrase-length variation plans;
- transition and fill families;
- density boundaries;
- expected interaction with bass, riffs, skank, comping, or other relevant
  musical roles.

The profile selects intrinsically appropriate behaviour. Bebop, Trap, Funk,
Metal and Bossa may be rhythmically detailed without requiring high global
complexity. Sparse Pop, Reggae or atmospheric playing remains intentional
rather than incomplete.

### Internal drummer/performance character

A generated idea may deterministically select a small internal performance
character within profile-safe bounds. This is not initially a user-facing
control.

Possible measured axes include:

- ahead, centred, or behind the beat by limb/voice;
- tight versus loose timing dispersion;
- economical versus active kick vocabulary;
- restrained versus expressive ghost-note use;
- stable versus articulated timekeeping;
- fill length and lead-in preference.

These axes must remain coherent over the full idea. Independent per-hit random
humanisation is not an acceptable drummer model.

### Phrase-aware generation

Generation operates over musical spans rather than isolated beats:

- establish the core pocket;
- create bounded A/A-prime variation rather than repeating a two-bar pattern
  unchanged;
- recognise two-, four-, and eight-bar boundaries;
- prepare transitions before a fill;
- orchestrate the fill across physically plausible limbs and tom movement;
- land into the next section or return cleanly to the pocket;
- avoid a crash, fill, or embellishment merely because a probability passed.

Every idea requires an explicit performance arc proportional to its actual
length. A short idea may establish, vary, and turn around within a few bars; a
32-bar part requires several developments and recoveries. In either case the
part must preserve recognisable pocket identity while developing comparably to
the other musical roles:

- bars 1-4 establish the drummer, pulse and primary orchestration;
- later phrases introduce measured A-prime kick, ghost, articulation, or
  timekeeping changes;
- fills and transitions occur naturally and often enough to articulate form,
  while their length and intensity remain style-appropriate;
- section or phrase peaks may change cymbal voice, register, density, or fill
  vocabulary;
- recovery phrases simplify after strong events so expression does not become
  continuous crowding;
- the final phrase can prepare a loop, cadence, continuation, or next section
  deliberately.

These are functional roles, not fixed bar-number templates. The chosen form,
meter, style, profile, tempo, and deterministic seed decide their placement.

### Physical coordination

Rules must prevent or explicitly justify impossible or implausible
simultaneous actions:

- closed and open hi-hat are mutually coordinated;
- an open hat can be choked by the following closed/pedal action;
- ride and two-handed hat/snare/tom activity respect available hands;
- fills leave and return to the timekeeping voice coherently;
- tom paths favour playable movement while allowing deliberate reversals;
- double-kick vocabulary is profile-specific and does not consume a hand;
- electronic styles may intentionally exceed acoustic-kit constraints where
  their programmed identity calls for it.

### Velocity and articulation

`g`, `x`, and `a` remain the readable grid states, but each generated hit must
resolve deterministically into inspectable performance data:

- MIDI velocity;
- microtiming offset;
- articulation;
- phrase/fill role;
- repeat index or local repetition context;
- velocity/excitation class.

Velocity must use measured, piece-specific and style-bounded distributions.
Consecutive hits are correlated: accents, alternating hands, crescendos,
decrescendos and rebound behaviour replace independent uniform randomness.

Velocity also drives the accepted synthesis semantics:

- ghost/normal/accent output and excitation curves;
- brightness, decay and drive modulation;
- Ride edge/bow/bell emphasis;
- snare shell/wire/transient balance;
- hat articulation and duration;
- tom attack and shell response.

## Target 12-piece schema

The production layout is:

1. Kick
2. Snare
3. Closed HH
4. Open HH
5. Ride
6. Crash
7. High Tom
8. Mid Tom
9. Floor Tom
10. Cross-stick / Rim
11. Shaker
12. Hand Percussion

The schema migration must:

- identify legacy ten-lane data explicitly;
- map legacy `Tom` to `Mid Tom`;
- preserve every later legacy lane by identity rather than index;
- initialise High Tom and Floor Tom as empty;
- save a new schema version or stable lane IDs;
- retain rendering support for legacy generated recipes where needed;
- add round-trip and malformed-input boundary tests.

Do not leave both a production generic Tom and three new toms merely to avoid
writing a migration. The saved representation must become explicit and safe.

## Target sound architecture

Extract a shared drum-only production module used by both Jam2 and the Drum Kit
Lab. It contains:

- the 12-piece instrument identity;
- kit and piece parameter types;
- Jam2 procedural sources;
- the minimal pinned DaisySP subset used by accepted drums;
- source blending;
- the optional Trap sine fundamental;
- transients, textures and colour stages;
- choke relationships;
- semantic velocity response;
- room sends and drum bus;
- stable kit IDs and revisions;
- deterministic rendering and diagnostics.

The accepted catalogue currently maps all 27 profiles to their feedback-focus
kits. There are 23 named kit families and 26 distinct resolved parameter sets:
Pop Loop and Pop Sectional are identical, while the shared Jazz and Soul/R&B
families have profile-specific resolved parameters.

The experiment retains:

- all three candidates per profile;
- editable controls and candidate override;
- audition modes and research diagnostics;
- experimental melodic sound design.

Jam2 initially ships only the accepted resolved drum defaults. Future lab
changes require an explicit promotion/revision step.

## Mix contract

- Preserve the existing internal `+3 dB` recipe gain because it is part of the
  accepted kit/bus voicing.
- Apply an additional `+8 dB` makeup after the complete researched kit bus.
- Leave generated managed drum lanes at `0 dB`, preserving all `+12 dB` of
  user fader headroom.
- Keep samples below `0.78` linear and smoothly limit louder post-makeup
  transients toward `0.98`; never hard-clip the rendered stem.
- Preserve an existing user's manual managed-lane gain when regenerating.
- Establish common master headroom before PCM conversion so the relative lift
  cannot create hidden clipping.
- Report pre-makeup peak, output peak/RMS, limited-sample count, full-mix peak,
  over-unity sample count, and every applied gain.

## Analysis deliverables

The checked-in analysis must produce inspectable tables, not a learned runtime
model or a subjective score.

At minimum, measure by source, style, drummer, beat/fill class, tempo band, kit
piece and metrical position:

- hit density and silence;
- velocity distributions and quantiles;
- ghost/normal/accent candidate bands;
- onset deviation and dispersion;
- consecutive-hit velocity and timing deltas;
- kick/snare/hat/ride co-occurrence;
- open/closed/pedal-hat transitions where available;
- ride bow/bell use where available;
- tom direction, span, density and fill placement;
- crash placement and post-fill landing;
- fill length, lead-in, ending and return behaviour;
- two-, four-, and eight-bar variation;
- drummer-level differences and within-drummer consistency.

Jam2's current and revised outputs must be passed through the same feature
extractor. Comparisons report raw distributions and differences per profile.
Dataset similarity is evidence for inspection, not an automatic quality score
or an optimisation target.

## Implementation sequence

1. Preserve this brief and establish reproducible source manifests.
2. Implement dataset parsing, canonical drum mapping, analysis and reports.
3. Render/export the current Jam2 baseline through the same event analysis.
4. Define evidence-backed profile performance specifications.
5. Add the versioned 12-piece schema and legacy migration.
6. Add explicit deterministic drum-performance recipe data.
7. Replace complexity probability tables with phrase-aware profile generation.
8. Extract the shared production drum engine and accepted catalogue.
9. Integrate stable kit selection and the production mix contract.
10. Run profile-wide event, signal, compatibility and listening audits.
11. Refine only from identified evidence or listening failures.
12. Update this log, `DRUMKIT-RESEARCH.md`, relevant product documentation, and
    complete the required experiment and Windows release builds.

## Acceptance criteria

The goal is complete only when:

- global melodic/harmonic complexity does not change generated drum decisions;
- all profiles generate an intentional core groove and coherent phrase-level
  development;
- audits across every supported generated idea length demonstrate development
  beyond repeated two-bar material, scaled to the available form and including
  purposeful A-prime changes, transitions, controlled fill pacing,
  peak/recovery behaviour, and an intentional ending or loop return;
- ghost notes, accents, timing and repeated hits use deterministic measured
  behaviour;
- style-appropriate fills use high, mid and floor toms where appropriate;
- non-tom fills remain idiomatic for Jazz, electronic, Trap, Reggae, Bossa and
  other relevant profiles;
- old ten-lane projects load without lane corruption;
- new projects round-trip the explicit lane schema;
- all 27 profiles select the accepted researched drum sound;
- the unfinished melodic experiment remains outside production Jam2;
- the Drum Kit Lab still builds and edits candidates against the shared drum
  renderer;
- produced recipes identify the kit ID/revision and exact performance;
- raw diagnostics expose timing, velocity, density, fill and signal data;
- comparative audits cover all 27 profiles and identify source-coverage
  limitations honestly;
- generated-track review inspects both raw event reports and audible Jam2
  renders for every profile, including groove realism, repetition, expression,
  fill interest, style fit, density, and long-form development;
- generated stems and full mixes have no unintended clipping;
- the accepted drum presence is retained against Jam2 backing;
- the required elevated MSVC build produces and validates
  `release/jam2.exe`.

## Working log

### 2026-07-30 — Goal established

- Created the production drum-rework goal.
- Confirmed that no additional user decision is required before research and
  implementation.
- Confirmed the initial source boundary: Magenta for human performance,
  complementary sources only for their reliable evidence, existing Jam2
  research for style and sound identity.
- Confirmed that drum complexity will be removed without changing the
  harmonic/melodic meaning of the existing idea-complexity setting.
- Confirmed that the experiment remains the editable workbench and unfinished
  melodic content is not promoted.

### 2026-07-30 — Reproducible Magenta ingestion baseline

- Added `tools/drum_research/analyze_gmd.py`, a standard-library-only parser
  accepting either the extracted GMD directory or official archive.
- Parsed all `1,150 / 1,150` local MIDI performances with zero errors.
- Preserved the full researched Roland mapping instead of reducing it to
  kick/snare/hat: high, mid and floor tom heads/rims; snare head/rim and
  cross-stick; hat bow/edge/pedal; crash bow/edge variants; and Ride
  bow/edge/bell remain distinct in the evidence.
- The output reports raw velocity distributions, overlapping candidate
  semantic bands, straight and triplet timing residuals, repeated-hit velocity
  and timing deltas, metrical positions, selected-grid co-occurrence, hat
  transitions, tom directions, articulation counts, density, and breakdowns
  by primary/exact style, drummer, beat/fill, and tempo band.
- Rejected the first naive grid classifier because a six-step triplet grid has
  more candidate locations and therefore won a raw mean-error comparison too
  easily. The corrected classifier ignores positions common to both grids,
  requires materially discriminating events and a clear triplet majority, and
  still reports residuals against both grids so the convenience classification
  cannot hide the measurements.
- Added five parser/grid regression tests; all pass.
- The first evidence output is local under
  `artifacts/drum-research/gmd-evidence.json`. It is intentionally ignored as a
  generated artifact; reviewed conclusions will be recorded here and in
  `DRUMKIT-RESEARCH.md`.
- Initial checks demonstrate large style differences that prohibit one shared
  velocity rule: median Pop snare velocity is approximately `101`, Rock `75`,
  Jazz `48`, Hip-hop `100`, and Reggae `38` in this corpus. These figures
  describe the source population and are not yet Jam2 defaults.
- Fill tom transitions contain `702` descending, `417` ascending, and `1,740`
  same-register consecutive movements. The high same-register count is an
  early warning against reducing realistic fills to a mandatory high-to-low
  sweep; duration, metrical position, drummer, style, articulation and local
  repetition must be separated before designing fill families.

### 2026-07-30 — Current Jam2 long-form baseline

- Added a reusable `validate.music-full-form-corpus` scenario and generated the
  current production corpus through `release/jam2.exe`.
- The baseline contains `888` ideas: all `27` profiles, all `74` native forms,
  complexities `1`, `4`, and `8`, and four deterministic samples per cell.
  Forms range from `8` to `32` bars. It also contains `74` complete audible
  mixes, one per native form.
- Added `tools/drum_research/analyze_jam2_corpus.py` and three regression tests.
  The audit reports raw hit density, bar uniqueness, exact two- and four-bar
  recurrence, adjacent/phrase-boundary changes, state/lane use, and existing
  kick/ghost/cymbal/fill/advanced-cell counts. Cymbal-only differences are
  deliberately excluded from the core-pocket development measurement.
- The baseline proves that global complexity currently changes drum behaviour:

| Complexity | Median hits/bar | Median unique core-bar ratio | Median exact two-bar core recurrence | Median fills/idea |
|---:|---:|---:|---:|---:|
| 1 | 12.479 | 0.433 | 0.400 | 1 |
| 4 | 12.833 | 0.536 | 0.273 | 2 |
| 8 | 13.250 | 0.733 | 0.091 | 2 |

- Existing technique counts rise mechanically with global complexity. Mean
  kick mutations are `2.743 / 4.334 / 5.696`, ghost mutations
  `1.419 / 2.642 / 3.635`, cymbal mutations `2.655 / 4.537 / 5.889`, and fills
  `1.074 / 1.838 / 2.476` at complexities `1 / 4 / 8`. Advanced rhythmic cells
  are absent below complexity 5 and have median `2` at complexity 8.
- Longer forms currently become proportionally more repetitive. Median unique
  core-bar ratio falls from `0.750` for eight-bar ideas to `0.562` at 16 bars,
  `0.417` at 24 bars, and `0.375` at 32 bars. This is direct evidence for
  length-relative phrase planning rather than merely adding a few more random
  mutations to a repeated two-bar base.
- Bossa, Metal, Contemporary Country, Reggae, Bebop and Swing currently show
  the highest median exact two-bar core recurrence among profile aggregates.
  Some repetition is stylistically correct, so the rework must distinguish
  pocket identity from identical orchestration and expression rather than
  optimising recurrence toward zero.
- Generated reports live under `artifacts/drum-research/` and remain outside
  source control. Reviewed evidence and design consequences are preserved in
  the research documents.

## Implementation and measured iteration — 2026-07-30

### Representation, performance, and compatibility

- Beat lane schema `2` now contains the canonical twelve pieces. Legacy
  ten-lane projects migrate old Tom to Mid Tom and preserve every following
  lane by identity; High and Floor Tom start empty.
- Generator recipe version `7` stores exact drum events with tick, stable lane
  ID, velocity, offset, articulation, role, repeat group, and fill flag.
- Drum generation is independent of melodic/harmonic complexity. A fixed-seed
  regression proves complexity `1` and `8` have the same beat fingerprint, hit
  count, and complete performance-event sequence.
- Every form develops its profile groove over its actual length and places
  phrase/section transitions. Acoustic fills traverse High, Mid, and Floor
  Tom; Jazz, Trap, electronic, Bossa, and Reggae use distinct fill families.
- Exact velocity comes from the accepted piece's ghost, normal, or accent band.
  Repeated hits vary deterministically inside that band.
- Ride resolves edge/bow/bell, hats resolve tip/shoulder/edge/open states, and
  the remaining pieces retain inspectable physical or programmed
  articulations. The corpus never exceeds three simultaneous drum voices.

### Accepted kit promotion

- `tools/drum_research/extract_researched_kits.py` is the explicit boundary
  from the Lab manifest to one accepted twelve-piece production kit for each
  of the `27` profiles. No experimental melodic synthesis is promoted.
- Production IDs are profile-scoped and revisioned
  (`profile:candidate:rN`). This prevents shared Jazz and Soul/R&B family names
  with profile-resolved parameters from loading an arbitrary hash-table entry.
- Primary and secondary sources are validated against the destination lane.
  Boundary validation renders ghost, normal, and accent probes for every
  non-native piece and rejects silence, non-finite samples, unsupported
  mappings, missing identities, and invalid velocity bands.
- The Lab now links the production catalogue module/resource while retaining
  its three editable candidates and pinned DaisySP design renderer. A fresh
  promotion extraction matches the embedded catalogue for all profiles when
  its generation timestamp is ignored.

Production and the Lab now compile the same `ResearchDrumEngine.cpp` and the
same pinned DaisySP drum subset. There is no separate mathematical
approximation between the two sound paths. The production build uses DaisySP
commit `599511b740f8f3a9b8db72a0642aa45b8a23c3a3`; its MIT notices are shipped
with the release.

### Fixed-seed corpus results

The version-7 corpus contains `888` ideas covering all `27` profiles, all `74`
native forms, complexities `1`, `4`, and `8`, and four seeds per cell.

| Measurement | Complexity 1 | Complexity 4 | Complexity 8 |
|---|---:|---:|---:|
| Median hits per bar | 12.500 | 12.656 | 12.500 |
| Median unique core-bar ratio | 0.917 | 0.917 | 0.917 |
| Median exact two-bar core recurrence | 0.000 | 0.000 | 0.000 |
| Median phrase/fill boundaries per idea | 4 | 4 | 4 |

This replaces baseline recurrence of `0.400 / 0.273 / 0.091` and
complexity-governed fill counts. Different corpus seeds cause the small
aggregate differences; the direct same-seed regression proves independence.

Across `200,203` realised events:

- ghost velocity median is `26` (`p05 18`, `p95 32`);
- normal velocity median is `75` (`p05 54`, `p95 84`);
- accent velocity median is `120` (`p05 101`, `p95 125`);
- same-state rapid-repeat absolute velocity delta has median `4`, `p95 13`;
- residual offset has median `0 ms`, `p05 -3 ms`, `p95 +5 ms`, while
  style backbeat placement extends the complete range to `-13..+28 ms`;
- `488` ideas use an acoustic three-tom fill; other fill families are counted
  separately rather than forced through toms.

GMD's wider residual onset distributions are not copied literally. They mix
deliberate feel, motor variation, grid-selection error, electronic-kit
latency, and passages whose intended subdivision is imperfectly represented
by one grid. Jam2 expresses large systematic feel through swing and lane
placement, then uses a smaller per-hit residual.

### Mix, diagnostics, and validation

- The accepted internal `+3 dB` recipe voicing remains unchanged. A new
  post-bus `+8 dB` makeup stage raises the completed generated drum stem while
  the managed lane begins at `0 dB`.
- The makeup stage is linear below `0.78`, uses a rational soft knee with a
  positive slope above it, and approaches a `0.98` PCM ceiling. The renderer
  reports pre-makeup peak, post-makeup peak/RMS, limited samples, frames,
  event count, and gain.
- Regeneration preserves any existing manual lane gain, including values above
  the new unity default.
- `tools/drum_research/audit_rendered_audio.py` reports PCM peak, RMS, crest,
  DC, and samples at the renderer's `0.98` safety clamp.
- The pre-makeup `74`-form figures (`0.149..0.862` drum peak, median `0.464`)
  are retained as the comparison baseline. The final post-makeup corpus
  completed all `74` forms with the rational knee: stem median peak
  `0.573899`, median RMS `-22.518 dBFS`, maximum peak `0.955688`, and zero
  PCM samples at or above `0.979`. The final mixes have median peak
  `0.400421`, median RMS `-23.731 dBFS`, maximum peak `0.519958`, and zero
  ceiling contact. The longest identical high-level stem run is eight samples,
  so the earlier Reggae plateau is removed.
- Required Windows MSVC release build succeeds to `release/jam2.exe`;
  production boundary validation passes after the mix change; all `12`
  Python analysis/signal tests pass; and the Lab audit-only build links and
  writes all `27` seed audits.

### Exact Lab performance auditions

- The Lab's shortened profile excerpt is re-fingerprinted without discarding
  the production recipe, so it no longer falls back to the simplified display
  grid.
- Complete-kit and selected-piece profile auditions consume the exact Jam2
  version-7 events: tick, swing treatment, millisecond offset, MIDI velocity,
  repeat group, articulation, and fill flag.
- One-shot, velocity-ladder, and repeated-hit modes remain explicit diagnostic
  grids. The render result identifies `jam2-v7-performance` versus
  `diagnostic-audition-grid` so the distinction is visible.
- A direct Sectional Pop render reported `40` production events, `35`
  microtimed events, and `4` fill events for fixed seed `1511909652`.
  The post-articulation build also confirmed non-empty production articulation
  on every one of those `40` events, including `firm`, `controlled`,
  `rebound`, `center-rimshot`, `ghost-center`, `tip`, `shoulder`,
  `edge-accent`, `open`, and `fully-open`.
