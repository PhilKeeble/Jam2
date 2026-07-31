# Jam2 Theory and Style Generation Pass

## Objective

Iterate through every Jam2 style and profile until the production generator
reliably creates complete, stylistically appropriate ideas across harmony,
melody, bass, supporting roles, drums, and form.

Variation must remain bounded by profile identity. A profile may correctly use
a small number of core harmonic or drum backbones, but generations must develop
those backbones through musical choices such as phrase answers, ghost notes,
kick placement, articulation, fills, inversions, approaches, register, motif
transformation, supporting-role entry, and section contrast. Random change is
not a substitute for authored variation.

Complexity must introduce researched musical concepts in ways that suit the
profile and the chosen form. Higher levels must not merely scatter more notes,
chords, chromatic events, or fills.

Sound design and mix quality are outside this pass.

## Authoritative local research

- `THEORY-PLAN.md`: style/profile research, harmonic and melodic vocabulary,
  native forms, complexity concepts, variation contract, and validation plan.
- `DRUMKIT-RESEARCH.md`: profile drum identities and GMD evidence limitations.
- `DRUM-REWORK.md`: performance-event architecture, phrase-aware drummer rules,
  and measured drum corpus.
- `GENERATION-AUDIT.md`: reproducible corpus method and evidence boundaries for
  this pass.

## Acceptance model

For each profile:

1. Generate at least several fixed-seed examples for every native form.
2. Compare examples at a fixed form and complexity for raw and
   transposition-normalized duplication.
3. Confirm that recurring backbones retain profile identity while their
   expression and development vary musically.
   Each profile should draw from multiple researched backbone families where
   the style supports them. An idea may introduce a related backbone at a
   section boundary, then use ghost notes, pickups, articulation, added hits,
   orchestration, and fills as bounded performance variation. One immutable
   backbone with cosmetic changes is too narrow; unrelated bar-by-bar rerolling
   loses stylistic and song-level identity.
   There is no target count. Existing backbones must be validated for style,
   function, and coherent development. Add another only when research exposes
   a specific missing pocket, harmonic route, song subtype, or section
   function—not to enlarge a library.
4. Compare matched seeds at low, middle, and high complexity. Record which
   concepts enter, where they occur, and why they suit the profile.
5. Check chord vocabulary, harmonic rhythm, melody, bass, support, drums,
   phrase boundaries, section roles, and full-form trajectory.
   Treat the requested duration as one continuous plan. The selected length
   must inform the number, role, placement, development, return, transition,
   and ending of its sections; a longer request cannot merely append more
   copies of a short plan. Measure and inspect four-bar block recurrence, but
   do not equate repetition with failure:
   recurring material must be transformed, answered, reorchestrated, or given
   a new formal role where the profile calls for development. A chain of
   independently generated four-bar loops with section labels is not a valid
   long-form arrangement.
6. Compare drums with direct GMD style evidence where available; mark related
   populations as proxies and use complementary evidence for genuine gaps.
7. Correct concrete generator failures, regenerate the profile, and repeat the
   same measurements.
8. Retain raw measurements and examples. Do not collapse judgement into an
   inferred quality score.
9. Audit every applied complexity operation in context. Confirm its harmonic
   destination is eligible, its placement serves a phrase/section role, its
   stated resolution occurs, and melody, bass, and any active support part
   follow the altered harmony. A symbolic label or randomly shifted chord is
   not evidence that the technique was used musically.

## Working log

### 2026-07-31 — Pass established

- Confirmed the scope covers all 27 profiles and all generated musical lanes.
- Confirmed that closeness within a profile is expected; exact reuse of the
  complete idea or transposition-only reuse is the primary duplication failure.
- Confirmed drum variation should draw from multiple profile-native backbone
  families where possible, select coherently at the idea or section level, and
  elaborate them rather than choose arbitrary patterns bar by bar.
- Began a full-arrangement analyzer for the existing 888-idea production
  corpus.
- Began a matched-seed corpus mode so complexity 1, 4, and 8 can be compared as
  related realizations instead of unrelated random draws.

### 2026-07-31 — Full baseline and Pop iteration 1

- Generated 1,332 complete ideas: all 27 profiles, every native form,
  complexities 1/4/8, and six matched seeds per form.
- Found no complete raw duplicates and no complete
  transposition-normalized duplicates. This rules out literal whole-idea
  cloning but does not establish musical quality.
- Added separate measurements for authored groove-family coverage, performed
  core-bar backbones, detailed bar variants, fill placement, section melodic
  recurrence, and matched-complexity tempo/motif identity.
- Clarified the drum target: each profile needs multiple researched backbone
  families where possible. A song selects coherently from that library and may
  use a related section backbone; local performance variation must not become a
  new structural pattern every bar.
- Clarified that backbone counts are not targets for either harmony or drums.
  The pass keeps the existing libraries unless style research identifies a
  concrete missing musical function.
- Pop baseline recorded a median eight distinct non-ghost structural bar
  variants in a 16-bar `pop_loop` form and about 22–23 in a 24-bar
  `pop_sectional` form. This is descriptive, not a quality threshold: many
  variants are valid when they remain profile-native and develop coherently.
  The count must be reviewed alongside pocket identity, phrase placement,
  section relationships, and real-performance evidence. First-to-final melodic
  event recurrence was near zero and likewise requires musical inspection
  rather than an automatic failure.
- Pop already has multiple selectable authored families across songs
  (`pop-straight-eighth`, `pop-four-floor`, `pop-syncopated-kick`, and
  `pop-half-time`, constrained per profile). Structural-variant counts will not
  be used to suppress their authored drummer gestures merely for being high.
- Current Pop breadth is descriptive only: `pop_loop` exposes four groove
  families and five harmonic progression backbones in the measured corpus;
  `pop_sectional` exposes three groove and three harmonic backbones. The task is
  to validate those choices and how they develop, not expand them to a number.
- Added a coherent sectional Pop arrival rule: the B/arrival may change from
  half-time to straight eighth, straight eighth to four-floor, or four-floor to
  straight eighth, while a return restores the primary family.
- Isolated tempo selection from complexity-dependent random decisions so a
  matched seed keeps its pulse while complexity develops other musical layers.
- Direct GMD Pop evidence argues against treating bar uniqueness as a defect:
  its median unique core-bar ratio is `1.0`; exact Pop subsets range from the
  more repetitive `pop/groove7` to highly changing `pop` and `pop/soft`
  performances. Jam2 Pop remains based on authored profile families while its
  kick, ghost, timekeeper, and fill gestures create comparable human variation.
- Found a separate Pop arrangement issue: compound 6/8 and 12/8 bass previously
  attacked every written eighth in every bar. The revised rule anchors the
  dotted-quarter pulse by default, retains continuous eighths for an active
  profile variation, and introduces bounded pickup motion into Sectional Pop
  build/arrival roles.
- Stabilised the seed's motif candidate against foundation harmony and isolated
  chord-rhythm, bass/support, and tempo random streams. In the 216-idea Pop
  iteration, `pop_loop` preserved tempo and motif across all 72 matched
  complexity transitions; `pop_sectional` preserved tempo throughout, with
  motif changes confined to comparisons where the available progression plan
  changed.
- Added explicit full-duration evidence: adjacent four-bar exact recurrence and
  similarity for harmony, melody, bass, support, and drums; section recurrence;
  role-density changes; and first-to-final relationships. These are inspection
  aids, not scores, and will be interpreted against each profile's loop/form
  research.
- Added a complexity-operation contract covering functional eligibility,
  claimed resolution, phrase/section placement, and coordinated melody, bass,
  and support responses. Tightened authored theory target selection so
  secondary dominants lead to explicit destinations, modal IV/iv colour only
  claims a tonic resolution when one follows, backdoor/tritone dominants target
  tonic, and a one-chord pitch shift is no longer mislabelled as temporary
  modulation.

### 2026-07-31 — Pop full-form and theory-operation refinement

- Corrected two systemic complexity-coordination faults exposed by the Pop
  corpus. Inserting an applied chord could invalidate an iterator before its
  written resolution was recorded; the target is now captured before insertion.
  Profile-specific bass motion could also replace a new chord root or written
  inversion at the moment of harmonic change; every newly written harmony now
  receives its root/slash-bass landing before profile-native motion resumes.
- Corrected the optional hook double so lowering by an octave never uses a
  numeric MIDI clamp that changes pitch class. The former lower-bound clamp
  could turn a valid B-flat double into C.
- The regenerated 216-idea Pop corpus now has zero hard generation,
  research-contract, duplicate, or applied-theory failures. All 590 measured
  `pop_sectional` and 216 `pop_loop` theory operations have an eligible target,
  realised resolution where required, and coordinated melody/bass/support
  response. An inversion is correctly defined by its bass and does not require
  a simultaneous melody onset.
- Added explicit requested-length evidence. It checks exact section and drummer
  phrase-plan coverage, every section boundary's lead-in fill, landing accent,
  bass landing, harmonic change and melodic connection, drummer energy
  trajectory, and the final chord/melody/fill relationship. It remains
  descriptive: an open loop ending is not failed merely for avoiding tonic.
- Every measured Pop form covers its exact requested length. Across 432 section
  boundaries, all have a prepared fill, landing accent, and bass landing.
  Median melodic boundary motion is about two semitones and the median gap is
  zero beats, so the sections connect as one performance instead of restarting
  as isolated blocks. Loop Pop drummer energy follows `0,1` over eight bars or
  `0,1,2,1` over sixteen; Sectional Pop rises through A/Lift/B and releases on
  the 32-bar Return.
- Musical inspection nevertheless found that the original Sectional Pop Lift
  did not fully satisfy its research claim: it rotated the selected loop, but
  average melody/drum activity barely distinguished Lift from A and there was
  no directed latter-half preparation. The revised eight-bar Lift retains the
  selected song backbone in its first half, then uses the researched
  predominant/dominant `ii–IV–V–V` preparation. Melody space closes
  progressively, the Lift and B enter higher, and the Return releases density.
  In the regenerated corpus average melody events per bar rise from about
  `4.5–4.6` in A to about `4.8–5.0` in Lift/B, then fall to about `2.5–2.9` in
  the Return; the change is a consequence of the form rule, not a numeric
  target.
- Matched-complexity identity remains controlled. `pop_loop` preserves seed,
  tempo, variation, progression, groove and motif throughout all 72
  transitions. `pop_sectional` preserves seed, tempo, variation and groove
  throughout; all 14 motif-cell changes occur only among the 17 comparisons
  where the complexity-eligible progression plan itself changes.

### 2026-07-31 — Rock iteration 1

- Generated and inspected 288 complete Rock ideas: all native forms for
  Riff/Modal, Shuffle/Blues Rock, and Punk/Garage, complexities 1/4/8, and
  twelve matched seeds per form/complexity cell. The regenerated audit has zero
  complete duplicates, research-contract failures, or theory-operation
  failures.
- The theory audit now treats silence correctly. Melody is not forced to add an
  onset over every applied chord; a rest is a valid arrangement choice.
  However, any melody or support note sounding through an altered harmony or
  its resolution must belong to the realised chord, and bass still has to land
  on the written root/inversion. This prevents the validation pass itself from
  making sparse Rock/Blues phrasing artificially busy.
- Direct GMD Rock evidence contains 341 performances. Jam2's measured Rock
  density lies inside that broad population rather than copying its median;
  the authored Rock, Shuffle, and Punk families retain their distinct meters,
  backbones, fills, ghost notes, timekeeper changes, and kick answers.
- Full-form inspection exposed two genuine section-map errors. The 10-bar 5/4
  Riff/Modal form had been divided into five generic two-bar labels, obscuring
  the researched asymmetric module. It now follows `2+2+3+3`: Riff A, displaced
  A-prime, three-bar Expansion, and three-bar Return. The 12-bar Punk form was
  named A-B-A but emitted generic A-A-prime-B roles; it now emits A,
  stop/re-entry B, and returning A-prime. These maps drive phrase energy, fills,
  motif role, and transition evidence rather than changing labels alone.
- Every Rock section and drummer phrase plan covers its exact requested length.
  All measured boundaries have a lead-in fill, landing accent, and bass
  landing. Riff/Modal uses modal/minor root families and straight/half-time
  grooves; Shuffle/Blues Rock uses the shared twelve-bar tonic/IV/turnaround
  schema and shuffle/straight-Blues families; Punk/Garage keeps its restricted
  I/IV/V or flat-VII vocabulary, fast straight drive, eighth-note bass, and
  concise 8/12/16-bar arcs.

### 2026-07-31 — Jazz iteration 1 and key-aware notation

- Generated and inspected 360 complete Jazz ideas across Swing/Standards,
  Bebop, and Fusion, every native form, complexities 1/4/8, and matched seeds.
  The final audit has zero complete duplicates, generation-contract findings,
  or applied-theory findings.
- Sustained pads now revoice at written chord changes, and any prior
  monophonic support note is truncated when a new support event begins. Melody
  sustains are likewise truncated at a harmonic boundary when the old pitch is
  not a common tone. These rules preserve intentional sustain without allowing
  stale notes to contradict an applied chord or its resolution.
- The last nineteen apparent Jazz theory failures were auditor errors rather
  than generator errors. The research parser had treated `m7b5` as an ordinary
  minor seventh and `dim7` as a diminished triad plus a minor seventh. It now
  matches Jam2's production parser: `{0,3,6,10}` for half-diminished and
  `{0,3,6,9}` for diminished seventh. Re-auditing the same 360 rendered recipes
  cleared all three Jazz profiles.
- Corrected a cross-style notation inconsistency exposed while reviewing the
  generated lanes. Enharmonic preference is now derived from tonic plus the
  resolved mode's relative-major key signature, rather than tonic pitch class
  alone. G natural minor therefore uses flats, C-sharp natural minor uses
  sharps, D Dorian follows C major, and F Mixolydian follows B-flat major
  throughout generated melody, bass, support, tonic labels, and pitch-class
  chord construction.
- The generation key picker now exposes twelve canonical pitch classes without
  flat/sharp duplicates. The chord-grid context menu separately follows the
  requested A-first chromatic order (`A A# B C C# D D# E F F# G G#`);
  generated output still respells those pitch classes for its musical key.
- The Chord Tones row now spells chord members by diatonic degree rather than
  selecting a chromatic sharp/flat list from the root text. It therefore shows
  `F7` as `F A C Eb`, `Cm` as `C Eb G`, and preserves theoretical spellings
  such as `F#maj7 = F# A# C# E#`.
- The production MSVC release build and complete boundary-validation scenario
  pass with explicit regression coverage for the canonical key list and
  G-minor/C-sharp-minor lane spelling, plus extended-chord tone spelling.
- The first clean contract result was not accepted as sufficient musical
  evidence. Direct comparison with the Jazz brief exposed four broader
  generator errors: Bebop still had only one harmony per bar, every Fusion
  melody/chord lane inherited triplet subdivision merely because its parent
  style was Jazz, Swing two-feel still walked every quarter, and Fusion's
  generic section map did not guarantee the defining vamp/changes contrast.
- Bebop's loop-length functional backbones now move twice per bar while its
  native twelve-bar Blues schema remains at its authored form rate. The
  asymmetric twenty-bar chorus follows `4+5+4+7` (head, sequential answer,
  bridge, return/break/tag), and the 32-bar chorus has explicit head,
  transformed head, bridge, and return/break roles. Its regenerated median is
  two harmony events and four walking-bass attacks per 4/4 bar; complexity
  approaches now sit a semitone from every actual next half-bar or bar target,
  not only from bar-line targets.
- Swing/Standards now distinguishes walking from two-feel in the symbolic
  bass lane. `jazz-two-feel` and brush-ballad cores use sustained root/fifth
  half notes; eligible higher-complexity pickups occur only immediately before
  a written change. The regenerated corpus spans median bass densities from
  two attacks per bar in strict two-feel material through four in walking
  material instead of labelling one quarter-note rule as both.
- Fusion now uses straight four-way musical subdivisions, kick-aligned
  electric-bass attacks, and chord-tone riff motion rather than triplet Jazz
  defaults and roots on every written beat. `fusion-16` is an eight-bar vamp
  plus eight-bar changes section; `fusion-14` is `4+6+4`; `fusion-10` is
  `2+2+4+2`. A selected modal backbone supplies the vamp and receives a
  functional contrast; a selected functional backbone is reserved for the
  changes while a mode-compatible two-root vamp supplies A and the return.
- Profile-specific head density replaces the generic subdivision probability.
  Across the final corpus, median attacks per beat are approximately
  `0.87–0.99` for Swing forms, `1.25–1.34` for Bebop forms, and `0.61–0.95`
  for Fusion forms. Fusion changes sections are normally sparser than the riff
  vamp, preserving the researched fragmented-riff/longer-answer contrast
  instead of producing continuous pseudo-solo lines.
- Complexity selection now rejects a proposed applied dominant, diminished
  approach, modal interchange, backdoor dominant, or tritone substitute when
  the exact proposed chord is already sounding at the insertion point. This
  removed a nominal Fusion `G7 → Cm7` operation that had redundantly inserted
  `G7` over an existing `G7`; complexity records now describe audible changes.
- Final Jazz evidence: 360 ideas; zero raw or transposition-normalized complete
  duplicates; zero generation-contract or applied-theory failures; exact
  requested-length coverage for every section and drummer phrase; bass
  landings at every measured section boundary. GMD's 101 labelled Jazz files
  are direct evidence for Swing/Standards and only a declared proxy for Bebop
  and especially straight/odd Fusion. Swing median drum density (`12.40`
  hits/bar) sits near the GMD Jazz median (`11.47`); Bebop intentionally
  retains denser ride-led interaction, while Fusion comparisons are interpreted
  per beat/meter and not forced toward the Swing population.

### 2026-07-31 — Modal Jam iteration 1 and mode-derived extensions

- Generated and inspected 216 complete Modal Jam ideas: Modal Groove and
  Atmospheric Modal, every native form, complexities 1/4/8, and twelve matched
  seeds per form/complexity cell. The final corpus has zero raw or
  transposition-normalized complete duplicates, zero research-contract
  findings, and zero applied-theory findings.
- The first corpus contradicted the profile brief despite passing basic
  validity checks. Atmospheric harmony changed once per bar, bass followed
  every upper root, and the supposed drone restarted at each chord. Modal
  Groove likewise had no real ostinato relationship. The final planner treats
  non-tonic structures as upper colours over `/I`, keeps the bass and drone on
  the tonic pedal, and lets Modal Groove answer that pedal only at meter-group
  starts with fifth or characteristic-degree motion.
- Fixed the systemic source of the missing pedals: `romanDegree()` did not
  strip an ASCII flat before classifying `bII`, `bVI`, or `bVII`. The upper
  structures were therefore mistaken for tonic-degree tokens and never
  received their pedal bass. Flat and sharp prefixes are now normalized before
  degree classification.
- Atmospheric harmony now changes once per broad form section rather than once
  per bar. Its 12-bar form is three four-bar fields; 16 bars uses four
  four-bar fields; 20 bars uses four five-bar fields. Median harmony and bass
  attack density are each `0.25` events per bar, while one continuous drone
  spans the idea unless a sparse high-complexity response deliberately
  interrupts it. A monophonic support lane no longer alternates a drone and pad
  merely because both roles are listed in the profile.
- The asymmetric 10-bar Modal Groove form now has researched roles rather than
  five generic labels: Pedal A, Rhythmic Answer, Colour Reveal, Displacement,
  and Return. In 4/4 its bass uses tonic plus a mid-bar answer; in 5/4 and 7/8
  it follows the declared `3+2` or `3+2+2` group starts. The final corpus
  median is two bass attacks per bar, with sixteen normalized bass
  realizations across the profile rather than root-following every upper
  chord.
- Removed the generic inversion and parallel-mode interchange operations from
  Modal Jam. They could drop the pedal or relabel an already-native Dorian IV
  as “borrowed” without making an audible harmonic change. Modal complexity now
  develops the native collection through rhythmic mutation, characteristic
  degrees, collection-preserving extensions, sparse dialogue, and
  orchestration instead of importing a generic functional-harmony operation.
- Added a true mode-derived extension operation at complexity level 3. It
  stacks thirds through the active mode, verifies that the starting triad is
  itself diatonic, and selects the exact supported seventh or ninth quality.
  For example, in D Lydian the tonic becomes `Dmaj9`, while II over the pedal
  becomes `E9/D`; in F-sharp Phrygian the tonic becomes `F#m7` rather than
  receiving an out-of-collection major ninth. Unsupported ninth structures
  fall back to their correct diatonic seventh rather than inventing an
  incorrect symbol.
- Explicit qualities remain literal exceptions. Existing dominant, altered,
  borrowed, diminished, suspended, secondary-function, and already-extended
  symbols are not recomputed from the mode. This separates “extend this
  diatonic chord” from “realize this deliberately authored `V7`, `7b9`,
  borrowed iv, or substitute dominant.”
- Updated the symbolic auditor so ninths and slash basses are represented in
  chord pitch-class sets. A mode-derived extension is checked against the
  active modal collection rather than requiring every sounding melodic tone
  to be one of the chord's stacked thirds; stable modal tensions and the tonic
  drone are therefore accepted, while an extension leaving the declared mode
  is a hard failure.
- Corpus inspection found that some otherwise valid modal melodies never
  stated the characteristic degree. The melody planner now targets that degree
  at the first suitable upper-colour change in each section. The analyzer makes
  at least one such statement a Modal Jam contract: all 216 final samples pass.
  Dorian natural 6, Mixolydian flat 7, Aeolian flat 6, Phrygian flat 2, and
  Lydian sharp 4 are therefore audible identities rather than metadata labels.
- GMD has no suitable Atmospheric Modal population. Its Jazz-labelled subset
  is retained only as a human-kit proxy for Modal Groove, not as a style target.
  Modal Groove's median `11.49` drum hits per bar is close to that broad
  population's `11.47`, but the authored modal groove families, meter grouping,
  velocity roles, phrase development, and fills remain authoritative.
- Every Modal Jam form and drummer phrase plan covers its exact requested
  duration. All measured boundaries have a prepared fill, landing accent, and
  bass landing. Matched seeds preserve tempo, variation, progression, groove,
  motif cell, and drum performance across complexity; higher levels develop
  the same identity instead of rerolling its backbone.
- Regression evidence: nine Python analyzer tests pass; the required MSVC
  release build succeeds; and the full boundary-validation scenario passes.
  Boundary cases explicitly prove a continuous Phrygian tonic pedal beneath
  `bII/I` and the distinct `Dmaj9` versus `E9/D` Lydian extension result.

### 2026-07-31 - Blues iteration 1

- Generated and inspected 216 complete Blues ideas across Dominant/Major Blues
  and Minor Blues, all six native profile/form combinations, complexities
  1/4/8, and twelve matched seeds per form/complexity cell. The final corpus
  has zero raw or transposition-normalized complete duplicates, zero
  research-contract findings, and zero applied-theory failures.
- Replaced the former generic progression treatment with explicit open and
  closed 8-, 12-, and 16-bar Blues routes. Dominant Blues now varies slow
  change, quick change, tonic close, open dominant, and a complexity-bounded
  Jazz-Blues route. Minor Blues distinguishes modal minor movement from a
  functional `iiø-V-i` turnaround, which is unavailable at foundational
  complexity. Open and closed endings occur across the seed population rather
  than every idea ending on V.
- The resolved tonal collection now matches the selected profile. Dominant
  Blues uses a collection containing both minor and major third inflection;
  Minor Blues varies Minor Blues, Natural Minor, and Dorian where the selected
  progression supports them. Every final dominant sample states both the minor
  and major third, while every minor sample states the minor third and flat
  seventh.
- Reworked the form map as call, response, and closing spans rather than a
  generic sequence of equally labelled blocks. The opening two-bar call's
  onset identity is recalled in later form lines with bounded mutation and
  chord-aware repitching. All final samples retain at least 83 percent of the
  opening call slots at the measured return, with a median of complete onset
  recall; pitches remain free to answer the current harmony.
- Melody density now breathes between lead calls and answers instead of being
  forced at every chord change. High-complexity support enters only in response
  space, not under the lead-call half of each four-bar line. Every applicable
  advanced sample contains that response role without turning the whole form
  into continuous counterpoint.
- Compound-meter Minor Blues bass now attacks the four dotted-quarter anchors
  of each 12/8 bar. Bounded higher-complexity pickups may increase the median to
  about 4.67 attacks per bar; foundational complexity remains exactly four and
  never introduces an off-pulse attack. Dominant walking/shuffle motion uses
  roots, fifths, and eligible sixth colour; minor motion uses root, fifth,
  flat-seven, and an explicit Dorian natural sixth only when Dorian is active.
- Generic inversions and unrelated harmonic substitutions no longer spread
  through Blues call lines. Applied dominants, passing diminished chords, and
  the dominant profile's backdoor option are confined to the composed closing
  span. Minor applied dominants tonicize i rather than treating modal v as a
  temporary tonic. Complexity 8 uses a bounded one- or two-operation
  turnaround, and all recorded operations have a real target and coordinated
  resolution.
- Direct GMD Blues evidence is only four performances, all on a triplet-sixth
  grid and with unusually dense layered timekeeping. It is retained as useful
  but narrow human-performance evidence, not a density target. Jam2 therefore
  keeps its authored shuffle, straight, and compound families, ride/hat
  articulation, four-bar phrase fills, and profile-specific kick/snare
  coordination instead of copying the GMD median of roughly 25 hits per bar.
- Inspection exposed floor/high/mid tom fill notes carrying a ghost marker and
  velocities as low as 14-17. Fill toms are now promoted semantically from
  `g` to `x` in the generated grid before performance realization, so their
  normal-stroke velocity and inspectable pattern state agree. Across the final
  corpus there are 2,274 tom hits, zero ghost-state toms, and the lowest
  rendered tom velocity is 48.
- Diversity remains profile-led rather than arbitrary: each profile has 108
  distinct normalized melody realizations and 36 normalized drum sequences
  across its 108 samples, with no complete duplicates. Harmony deliberately
  recurs where seeds select the same researched Blues schema; fills, ghost
  notes, articulation, bass approaches, melody answers, and endings vary
  within that identity.
- Final artifacts are
  `artifacts/music-research/blues-iteration-1/full-form-corpus.json`,
  `symbolic-audit.json`, `blues-review.md`, and `style-review.md`. Twelve Python
  analyzer tests pass, the required MSVC release build succeeds, and every
  practice-generation boundary passes, including mode-derived extension and
  performance-state consistency. The complete boundary scenario still reports
  one unrelated pre-existing headless master-output failure
  (`audible_peak_ppm=0`); the generator-related case that previously exposed
  the tom state/velocity mismatch is now green.
