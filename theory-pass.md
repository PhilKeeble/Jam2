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

### 2026-07-31 - J-Pop / Anisong iteration 1

- Generated and inspected 180 complete ideas: 108 Anisong Rock ideas across
  its 18-, 24-, and 32-bar forms, and 72 Idol / Dance J-Pop ideas across its
  16- and 24-bar forms. Every form uses complexities 1/4/8 and twelve matched
  seeds per form/complexity cell. The final corpus has zero raw or
  transposition-normalized complete duplicates, zero research-contract
  findings, and zero applied-theory failures.
- The baseline was valid but musically generic. Every idea resolved as Major;
  bass attacked every eighth note; Anisong Lift and B sections were mostly
  rotations of one loop; support toggled mechanically; long-range hook recall
  was weak; and high complexity sprayed eight to sixteen unrelated chord
  operations through one form. The final pass treats these as coordinated
  profile and form choices rather than independent density controls.
- Both profiles now select researched Major and Natural-Minor families.
  Anisong adds Aeolian drive and minor-circle/dominant-return backbones; the
  original major circle, partial-tonic, descending-bass, and chromatic-colour
  families remain. The final population contains 74 Major and 34
  Natural-Minor Anisong ideas, plus 48 Major and 24 Natural-Minor Idol ideas.
  These counts describe deterministic seed coverage, not a quota.
- The Anisong Lift is a directed pre-arrival route rather than a rotated loop.
  Major routes can use `ii - V/iii - iii - vi - ii - IV - V - V`; minor
  routes use `iv - bVII - bIII - bVI - iiø - V7 - V7 - V7`, shortened to the
  native section length. Full 32-bar forms explicitly return home, while
  shorter forms may end open when their selected route supports it.
- Advanced Anisong can select one prepared chromatic-mediant B region. The
  preceding dominant targets the local tonic; the entire B harmony and melody
  use the local centre; a full Return receives a reciprocal home dominant and
  an explicit home-tonic arrival. Nineteen of 36 complexity-8 Anisong ideas
  select this option. Complexity operations cannot overwrite its entry,
  return preparation, or home arrival.
- This section-key policy follows corpus evidence rather than claiming that
  modulation defines the style. Joy Li's 100-anisong corpus reports frequent
  chromatic modulation and section-level tonal planning, while Ramage's
  partial-tonic account supports treating `IV-V-iii-vi` as one flexible
  family rather than the whole genre. Sources:
  `https://mtosmt.org/issues/mto.26.32.1/mto.26.32.1.li.php` and
  `https://academic.oup.com/mts/article-abstract/45/2/238/7224663`.
- Harmonic complexity is now bounded by profile and musical role. Foundation
  ideas apply no chromatic operations; complexity 4 applies one or two;
  complexity 8 applies three or four in Anisong and two or three in Idol,
  including any selected section-key region. Extensions remain derived from
  the active mode, while authored dominants, borrowed qualities, and altered
  functions stay literal. The descending route's `V/vii` is correctly the
  dominant triad in first inversion, not an applied dominant of scale degree
  seven.
- Melody now stores the opening two-bar onset hook and recalls it in Idol A'
  and the full Anisong Return with bounded mutation and chord-aware
  repitching. Every measured return preserves at least half of the opening
  onset template; final-corpus minima are 0.67 for Idol and 0.78 for
  full-form Anisong, with medians near 0.9 or higher. Density and register
  still develop through Lift, B, arrival, and return roles, so recall does not
  mean literal pitch copying.
- Idol now has explicit Hook A, Hook A', and optional B / Final Tag roles.
  A short group response punctuates each completed four-bar lead phrase even
  when the final lead subdivision is occupied; selected lead harmony and hook
  doubles are confined to eligible bars rather than following every note.
  The external IdolSongsJp corpus is used only as supporting role evidence for
  separately authored lead, harmony, and call parts, not as copied material:
  `https://huggingface.co/datasets/imprt/idol-songs-jp/blob/main/README.md`.
- Anisong bass now rotates roots, fifths, and chord thirds, with phrase-edge
  chromatic approaches only at eligible complexity. Idol keeps a more stable
  root/fifth dance foundation. Median attacks fell from the baseline's
  mechanical eight per bar to 6.0 for Anisong and 5.125 for Idol; final
  per-idea maxima are 6.417 and 5.75 respectively. Kick overlap remains high
  without forcing every offbeat answer.
- The length-aware form planner now treats a custom eight-bar Idol request as
  one valid Hook A section. It adds Hook A' only when bars remain, and a B /
  Final Tag only beyond sixteen bars. This fixed a boundary-discovered case
  where a custom eight-bar recipe incorrectly advertised a second eight-bar
  section starting at bar nine.
- GMD has no direct J-Pop or Anisong label. Its Rock population is retained
  only as a human-drummer proxy for Anisong and its seven Dance files as a
  weak proxy for Idol; neither density is treated as a target. Jam2's
  profile-authored drummer backbones, velocity bands, articulation,
  development cells, and fills remain authoritative. Final median drum
  densities are 11.229 hits/bar for Anisong and 14.979 for Idol.
- Final diversity is profile-led: Anisong has seven progression IDs, four
  grooves, 108 distinct normalized melodies, 104 normalized bass lines, and
  36 drum sequences across 108 ideas; Idol has four progression IDs, two
  grooves, 72 distinct normalized melodies, 72 bass lines, and 24 drum
  sequences across 72 ideas. No complete idea duplicates another inside its
  fixed comparison cell.
- Final artifacts are
  `artifacts/music-research/jpop-iteration-1/full-form-corpus.json`,
  `symbolic-audit.json`, `jpop-review.md`, and `style-review.md`. Fourteen
  Python analyzer tests pass, the required MSVC release build succeeds, and
  the complete boundary-validation scenario passes with no failures.

### 2026-07-31 - Country iteration 1

- Generated and inspected 216 complete Country ideas: 108 Honky-Tonk /
  Two-Step ideas and 108 Contemporary Country ideas, covering all six native
  forms, complexities 1/4/8, and twelve matched seeds per form/complexity
  cell. The accepted corpus has zero raw or transposition-normalized complete
  duplicates, zero research-contract findings, and zero applied-theory or
  coordinated-lane failures.
- The baseline was structurally valid but did not distinguish meter or profile
  deeply enough. Every sample resolved as Major; the Country waltz and 6/8
  ballad could select ordinary four-beat families; traditional harmony mostly
  rotated generic loops; bass attacked three or four times per bar regardless
  of two-feel; advanced support became nearly continuous; and complexity 8
  applied as many as twelve unrelated chord operations to one idea.
- Honky-Tonk now draws from Major, Mixolydian, and Major Pentatonic melody
  collections over explicit 12-, 16-, and 24-bar I/IV/V routes. The authored
  routes include tonic and open-dominant tags, an optional directed
  `I - V/ii - ii - V` family, and a waltz B section that tonicizes IV before
  returning through ii-V. `V/ii` remains Major because its leading tone is
  functional, while all fifteen final Mixolydian samples explicitly state the
  flat seventh in the opening sung call.
- Contemporary Country retains seven researched backbones spanning I-IV-V,
  I-V-IV, I-vi-IV-V, vi-IV-I-V, and applied-dominant motion, plus a bounded
  minor-Pop family. The final population contains 93 Major and 15
  Natural-Minor ideas. Sectional lifts now become a directed predominant /
  dominant phrase rather than another loop rotation, and every named Return
  closes on the home tonic.
- Meter now selects the drummer family before any variation is applied.
  `country-waltz-24` always uses the three-beat waltz family;
  `country-pop-6-8` always uses the compound family; incompatible forms cannot
  select either. The waltz defect found during inspection—simultaneous snare
  and cross-stick on beats two and three—was removed. Its audible side-stick
  anchors now use normal/accent bands, its fills identify as
  `country-waltz-*`, and the compound form uses `country-compound-*` fills
  that preserve the broad dotted-quarter pulse.
- The Country drummer keeps its two-bar backbone and section-boundary fills
  but uses fewer non-boundary kick, ghost, timekeeper, and development
  gestures. This raised median Honky-Tonk backbone reuse from about 0.33 to
  0.44 while retaining nine structural bar variants and thirteen performed
  variants in a typical complete idea. Contemporary Country retains twelve
  structural and fifteen performed variants, with all measured fills at
  structural phrase or section boundaries.
- The local GMD evidence has 29 Country-labelled performances, 26 classified
  on a straight-sixteenth grid and three on a triplet grid. It supports stable
  adjacent-bar identity, articulated hat/snare interaction, fills, and
  velocity variation, but it is weak form evidence: its median active length
  is one bar and only a small tail contains long performances. Its absolute
  MIDI velocities also reflect the single captured electronic kit and are not
  treated as a Jam2 loudness target. Jam2 therefore uses GMD to bound pocket
  continuity and performer behavior while retaining the researched
  profile/form grammar and drum-kit velocity semantics.
- Foundation Country bass is exactly two attacks per bar: beats 1/3 in 4/4,
  beats 1/3 in 3/4, both beats in 2/4, and the two dotted-quarter anchors in
  6/8. Higher complexity adds a connector only at a written change or
  phrase edge. Inspection found 246 connectors with the correct pitch class
  in the wrong octave; the planner now chooses the next target register first
  and places a semitone or whole-tone approach around it. All 1,834 labelled
  connectors in the accepted corpus resolve by one or two semitones.
- The opening two-bar sung-call rhythm is stored and reharmonized in A-prime,
  arrival, or Return sections rather than rerolled. Every measured Honky-Tonk
  return preserves at least 75 percent of the opening onset template and every
  Contemporary return at least 83 percent; both medians are complete onset
  recall. Four-bar endings reserve a short response slot. Foundation ideas
  remain lead-only, complexity 4 adds sparse guitar/fiddle-like answers, and
  only advanced Contemporary Country may add a bounded arrival hook double.
- Harmonic complexity is now profile-bounded: foundation applies no optional
  operations, complexity 4 applies one or two, and complexity 8 applies at
  most three in Honky-Tonk or four in Contemporary Country. Generic `maj9`
  stacking was replaced by profile-native I/IV sixths and V7 in traditional
  Country, or add9, m7, and V7 colours in Contemporary Country. Borrowed iv
  must reach I, applied dominants and diminished connectors have an explicit
  destination, and operations cannot overwrite the form route.
- Inversion review exposed a wider systemic problem: 51 of the initial 131
  Country inversions increased the surrounding bass motion, often changing a
  repeated root into an arbitrary slash-bass leap. Inversion candidates now
  have to shorten the preceding/next bass route, and a final pass removes one
  if a later inserted setup chord makes it cease to help. This rule applies to
  every style and will be rechecked as the remaining styles are regenerated;
  all 130 Country inversions in the accepted corpus pass the final-route
  contract.
- Final diversity remains style-led rather than quota-led. Contemporary
  Country has seven progression IDs, three grooves, 108 distinct normalized
  melodies, 98 bass lines, 68 support lines, and 36 drum sequences. Honky-Tonk
  has three directed progression families, four grooves, 108 melodies, 88
  bass lines, 42 support lines, and 36 drum sequences. Recurrence belongs to
  the researched backbone; complete ideas do not collapse to duplicates.
- Final artifacts are
  `artifacts/music-research/country-iteration-1/full-form-corpus.json`,
  `symbolic-audit.json`, `country-review.md`, and `style-review.md`. Seventeen
  Python analyzer tests pass, the required MSVC release build succeeds, and
  the complete boundary-validation scenario passes. Fixed C++ cases cover
  waltz and compound routing, two-feel bass anchors, Mixolydian flat-seven
  identity, meter-specific fills, non-doubled side-stick, bounded Country
  chord colours, functional inversions, and stepwise bass connectors.

### 2026-07-31 - Electronic iteration 1

- Generated and inspected 252 complete Electronic ideas: 72 House ideas over
  its 16- and 32-bar forms, 108 Techno ideas over its 16-bar, 32-bar, and
  fifteen-bar 5/4 process forms, and 72 Breakbeat ideas over its 16- and
  24-bar forms. Every form uses complexities 1/4/8 and twelve matched seeds
  per form/complexity cell. The accepted corpus has zero raw or
  transposition-normalized complete duplicates, zero research-contract
  findings, and zero applied-theory or coordinated-lane failures.
- The baseline was diverse but contradicted the researched grammar. All three
  profiles inherited a dense chord-following lead at roughly five attacks per
  bar; its rhythm was rerolled continuously. The musical grid randomly
  alternated eighth and sixteenth resolution between beats, so an authored
  programmed cell could not recur exactly. House complexity applied hundreds
  of one-off inversions and borrowed chords, Techno received generic harmonic
  operations despite its one-centre brief, Breakbeat had only one skeleton,
  and machine drums mutated too many ordinary bars.
- Electronic profiles now use a stable sixteenth address space. House and
  Breakbeat preserve a two-bar opening pitch/rhythm cell; Techno phases a
  three-beat process cell against both 4/4 and 5/4. Later states may toggle
  one deterministic slot, while a cell that straddles a section boundary is
  treated as an intentional splice between two process states. Full-form
  contracts reject unrelated cell rerolls.
- House now has five harmonic backbones: three Major Pop/House rotations, a
  Natural-Minor loop, and a Dorian `i7-IV7-i7-bVII` vamp. Its lead median is
  1.89 attacks per bar rather than roughly five; bass is exactly two offbeat
  attacks per bar and never masks the quarter kick. The sparse House groove
  and denser Disco-House groove remain distinct. Disco-House now applies
  bounded sixteenth swing, and its median density of roughly 21 hits per bar
  sits near the five-file GMD Disco proxy without treating that proxy as a
  House definition.
- Techno retains four researched pitch plans: single centre, tonic/drop
  pedal, Dorian pedal, and Phrygian neighbour. Foundation uses one pitch
  class; developed ideas use exactly the centre plus one declared modal
  colour. Generic inversions, borrowed chords, and cadence pressure are
  disabled. Bass attacks once per written beat, including all five beats in
  the 5/4 form. The two drum backbones distinguish straight machine pulse
  from a three-over-four timekeeper layer.
- Techno complexity is now process-led and inspectable. Complexity 4 reveals
  the modal neighbour, mutates one cell slot, and adds a process-boundary
  riff. Complexity 8 extends the mutation across later states, adds a
  sustained centre only in the subtracted contrast state, and uses support
  and automation as a dialogue rather than adding chords. The deliberate
  repeated tonic is exempt from the generic four-identical-note melody rule;
  its own centre-plus-neighbour and cell-recurrence contracts are stricter and
  more stylistically relevant.
- Breakbeat now draws from six harmony/centre families, including a Dorian
  loop, and from two original drum skeletons: straight and bounded swung.
  Bass keeps four attacks per bar with at least one syncopated onset and
  answers rather than duplicates the broken kick. Lead density fell to a
  median 1.94 attacks per bar. Phrase edits, ghost detail, dropout, and
  chopped fills alter the preserved skeleton without turning it into
  four-floor House.
- GMD is used with explicit limits. The exact `dance/breakbeat` label contains
  only two performances, both classified on the triplet grid; it supports the
  swung family and human timing/velocity detail but its 7.81-hit median is not
  imposed on an electronic programmed break. House uses the five-file
  `dance/disco` population only as a proxy. Techno has no direct GMD label and
  uses the seven-file Dance population only as broad timekeeper evidence.
  None of these small populations is treated as form, harmony, or production
  ground truth.
- Electronic harmonic operations are now boundary colours rather than
  quotas. Foundation applies none; complexity 4 allows at most one in House
  or Breakbeat; complexity 8 allows at most three in House and two in
  Breakbeat, only at a phrase/process hand-off. Techno allows none. Borrowed
  subdominants resolve to the claimed centre and syncopated House bass is
  audited over the full harmonic span instead of being falsely rejected for
  entering after the kick.
- The complexity ledger was corrected systemically. It no longer selects
  `planing` merely because a profile is Electronic, no longer mistakes the
  bar number `7` in a chord-plan string for a seventh chord, and no longer
  claims counterpoint/call-response roles just because any support event
  exists. Inherited style backbones are not reported as newly introduced
  bass approaches or extensions. Electronic complexity now exposes actual
  displacement, modal-neighbour, riff-mutation, support, return, metric, and
  integrated-arrangement events.
- Diversity remains grammar-led. The accepted corpus contains five House
  progression IDs, two grooves, 60 normalized melody realizations, 38 bass
  realizations, and 24 drum sequences; four Techno progression IDs, two
  grooves, 85 melodies, 18 intentionally repetitive bass realizations, and
  36 drum sequences; and six Breakbeat progression IDs, two grooves, 70
  melodies, 44 bass lines, and 24 drum sequences. Matched complexity keeps
  seed, tempo, progression, groove, and motif identity stable while adding
  the appropriate concepts.
- Final artifacts are
  `artifacts/music-research/electronic-iteration-1/full-form-corpus.json`,
  `symbolic-audit.json`, `electronic-review.md`, and `style-review.md`.
  Eighteen Python analyzer tests pass, the required MSVC release build
  succeeds, and the complete boundary-validation scenario passes. Fixed C++
  coverage checks House offbeat bass and stable grid/cell, Techno 5/4
  beat-pulse bass and centre-plus-neighbour cell, Breakbeat syncopated bass,
  and the evidence-based complexity ledger.

### 2026-07-31 - R&B / Soul iteration 1

- Generated and inspected 216 complete R&B / Soul ideas: 108 Classic /
  Motown Soul ideas and 108 Contemporary R&B / Neo-Soul ideas. All six native
  forms use complexities 1/4/8 and twelve matched seeds per form/complexity
  cell. The accepted corpus has zero raw or transposition-normalized complete
  duplicates, zero research-contract findings, and zero applied-theory or
  coordinated-lane failures.
- The baseline had ample raw diversity but the wrong musical balance.
  Classic Soul lead/support medians were 4.44/2.75 attacks per bar and
  Neo-Soul was 4.29/1.69, so both behaved like continuously generated
  instrumental counterpoint rather than a vocal call with selective ensemble
  answers. The profiles applied 386 and 306 optional chord operations,
  drummer backbones changed on most bars, and 31 of 72 matched Neo-Soul
  complexity transitions rerolled the underlying progression.
- Progression selection is now complexity-independent. Classic Soul retains
  four directed families: `Imaj7-vi7-ii7-V7`, `ii7-V7-Imaj7`, Dorian
  `i7-IV7`, and major-to-minor plagal motion. Neo-Soul retains five families:
  the functional and descending seventh routes, Dorian minor vamp, backdoor
  dominant route, and minor-plagal descending-bass route. `rnb-minor4` now
  resolves as Dorian in both profiles so its major IV7 and declared melodic
  collection agree; the other current families resolve as Major.
- The opening two-bar vocal-call rhythm is stored as part of the seeded
  identity. A-prime and Return reharmonize that onset template with at most
  one bounded rhythmic edit rather than rerolling a new lead. Across every
  measurable final idea, Classic Soul recalls 83 percent to 100 percent of
  opening onsets and Neo-Soul recalls 67 percent to 100 percent; both medians
  are complete onset recall. Whole-section pitch recurrence remains low
  because the same rhythm is deliberately reharmonized and redirected.
- Lead writing now leaves a four-bar phrase-end breath. Classic Soul uses
  concise forward calls and has a final median of 2.67 attacks per bar.
  Neo-Soul uses fewer and longer notes, delayed pickups, and common-tone
  space, with a final median of 2.17. Compound-time density is judged against
  four dotted-quarter pulses rather than twelve written eighths, avoiding a
  false “continuous lead” finding for one onset per broad pulse.
- Support is ensemble-directed instead of generic. Foundation Classic Soul
  answers each four-bar vocal unit with a compact band response; complexity 4
  may add one selected lower harmony at a section lift; complexity 8 uses a
  two-note horn answer in the final vocal breath. Neo-Soul is lead/bass-only
  at foundation, adds lower vocal harmony only in the opening of A-prime or
  Return at complexity 4, and at complexity 8 coordinates one B-section pad
  with a two-note inner-line answer. Final support medians are 0.40 and 0.25
  attacks per bar, with no exact lead/support unison collisions.
- Classic Soul bass preserves a four-pulse melodic route through roots,
  thirds, fifths, written inversions, and harmonic arrivals. In 12/8 those
  attacks occur only at the four dotted-quarter anchors, not on all twelve
  written eighths. Neo-Soul retains one long structural bass statement per
  bar at foundation, adds a sparse offbeat semitone pickup into every second
  harmonic arrival at developed levels, and at complexity 8 may remove the
  bass for the first B bar before re-entering with a bounded whole-step-under
  reverse-ninth colour. A required chromatic resolution or inversion always
  takes precedence over the dropout/colour.
- Compound meter now has authored profile grooves. Classic uses
  `soul-12-8`, with a broad backbeat and compound pickup shape; Neo-Soul uses
  `rnb-12-8`, with a sparser late pocket. Straight forms cannot select either,
  and compound forms cannot select the ordinary Motown, Funk-Soul, laid-back,
  or sixteenth families.
- The drummer keeps a recognizable two-bar skeleton while retaining
  performer detail. Non-boundary kick, ghost, and timekeeper mutation rates
  were reduced rather than suppressing fills or dynamics. Median backbone
  reuse rose from 0.18 to 0.44 in Classic Soul and from 0.08 to 0.38 in
  Neo-Soul; the typical complete ideas still contain ten and 9.5 structural
  bar variants, fourteen and thirteen performed variants, and one or more
  structurally placed fills.
- GMD supplies 63 directly labelled Soul performances and is useful evidence
  for stable backbeat/tambourine continuity, human velocity and timing,
  ghost interaction, and fill placement. Its 16.51-hit-per-bar median comes
  from the captured electronic-kit population and is not imposed as an
  absolute density or loudness target. Neo-Soul uses that population only as
  a bounded proxy because modern Neo-Soul is underrepresented; its sparser
  8.31-hit Jam2 median is retained intentionally.
- Harmonic complexity is bounded to no optional operations at foundation,
  one at complexity 4, and at most two at complexity 8. The first developed
  Soul voice-leading operation is selected from inversion, parallel
  subdominant colour, or a mode-derived seventh/extension and persists when
  complexity rises. Advanced complexity may add an applied dominant at a
  structural destination. Seven Neo-Soul inversions are deliberately removed
  at complexity 8 because the added applied dominant changes the surrounding
  bass route and makes the old inversion cease to improve voice leading; this
  is a musical correction, not an uncontrolled concept turnover.
- Final diversity remains grammar-led. Classic Soul has four progression
  IDs, three meter-appropriate grooves, 108 normalized melodies, 84 bass
  lines, 99 support lines, and 36 drum sequences. Neo-Soul has five
  progression IDs, five grooves, 108 melodies, 86 bass lines, 69 support
  lines, and 36 drum sequences. Matched complexity keeps seed, tempo,
  progression, groove, and motif-cell identity stable.
- Final artifacts are
  `artifacts/music-research/rnb-soul-iteration-5/full-form-corpus.json`,
  `symbolic-audit.json`, `rnb-soul-review.md`, and `style-review.md`.
  Nineteen Python analyzer tests pass, the required MSVC release build
  succeeds, and the complete boundary-validation scenario passes. Fixed C++
  cases cover compound Soul groove/pulse bass/call response and matched
  Neo-Soul complexity, sparse pickups, B-section pad/counterline, and
  reverse-extension bass behavior.

### 2026-07-31 - Funk iteration 1

- Generated and inspected 108 complete Static / Pocket Funk ideas over an
  eight-bar pocket, sixteen-bar activation arc, and ten-bar minor pocket
  exchange. Every native form uses complexities 1/4/8 and twelve matched
  seeds per form/complexity cell. The accepted corpus has zero raw or
  transposition-normalized complete duplicates, zero research-contract
  findings, and zero applied-theory or coordinated-lane failures.
- The baseline was varied but did not behave like an ensemble pocket. Its
  lead attacked roughly 8.06 times per bar, bass attacked six times per bar
  because a generic Funk branch doubled the authored cell, support averaged
  two attacks per bar, and the drummer changed nearly every structural bar.
  It also allowed `ii7-V7` and chromatic chord routes at low complexity and
  applied 48 generic chord operations. Those mechanisms rewarded activity
  and chord count rather than “the one,” interlock, breath, and return.
- Harmony is now limited to four researched vamp routes: static dominant I7,
  static minor i7, I7-IV7, and i7-flat-VII7. Major-centred routes declare
  Mixolydian; minor-centred routes declare Dorian so the collection agrees
  with dominant/seventh colour. The ten-bar minor exchange accepts only the
  two minor routes. Adjacent identical chord-plan events are compacted into
  sustained harmonic spans rather than falsely presenting a new chord every
  bar.
- Complexity deliberately applies no optional chord operations in Funk.
  Foundation establishes the same selected one- or two-centre vamp used at
  higher levels. Complexity 4 introduces a single chromatic bass pickup,
  one bounded riff-attack exchange, a bass-downbeat subtraction in the break,
  and a short horn answer. Complexity 8 adds a final ensemble answer and
  long-range return while keeping progression, groove, tempo, variation
  plan, and motif identity stable across all 72 matched-complexity
  transitions.
- The lead is now a short two-bar riff/call rather than a continuous solo.
  Its opening attack map is stored and recalled by later form states; a
  developed state may exchange or remove at most one slot. Across the final
  corpus, two-bar onset recall is at least 67 percent and has a median of
  complete recall. Median lead density fell from 8.06 to 2.44 attacks per
  bar, leaving audible space for bass, clipped comping, drums, and answers.
- Bass now states the root on the one and answers with a syncopated
  root/fifth cell at four attacks per bar. The old duplicate generic Funk
  event was removed. Developed levels replace one late cell event at every
  four-bar hand-off with a chromatic pickup that resolves on the following
  one, and the Break / Stop state omits one bass downbeat rather than adding
  more notes. Final median bass density is 3.94 attacks per bar.
- Support is silent at foundation. Complexity 4 uses only two-note horn stabs
  in selected riff breaths; complexity 8 converts the last note of the final
  answer into a distinct ensemble call/response role. The final support
  median is 0.25 attacks per bar and never exceeds 0.5, so it remains
  punctuation rather than another generated lead.
- The chord lane now uses clipped, complementary sixteenth attacks while
  leaving the root downbeat available to bass and kick. Forms explicitly
  describe Pocket, Response/Exchange, Break or Stop, and Return states.
  Development is activation, subtraction, one-event exchange, and re-entry,
  not unrelated four-bar loops or chord-count inflation.
- Five researched drum backbones remain available: The One, Syncopated
  Pocket, Linear Funk, Disco-Funk, and Half-Time Funk. Non-structural kick,
  ghost, timekeeper, and development gestures were reduced, while fills stay
  at section/phrase boundaries. Median backbone reuse rose from 0.06 to 0.39
  without collapsing the five families; a typical idea retains six
  structural bar variants, nine performed variants, and fully structural
  fill placement.
- GMD supplies 160 directly labelled Funk performances. Its median density is
  15.18 hits per bar, useful as evidence for an active but repeated
  timekeeper, ghost detail, human velocity/timing, and coordinated kick/snare
  variation. Jam2's accepted median is 15.15 hits per bar, but this proximity
  is descriptive rather than an optimization score; Half-Time and
  Disco-Funk retain their intentionally different densities. The fixed
  contract additionally requires a kick on the one in at least three
  quarters of bars.
- Diversity remains grammar-led. The accepted corpus contains four
  progression IDs, five groove IDs, 108 normalized melodies, 24 bass
  realizations, 48 support realizations, and 36 drum sequences. The repeated
  bass and harmony signatures are the intended shared pocket backbones;
  complete generations remain distinct through key, route, groove, riff,
  form, performance detail, and bounded development.
- Final artifacts are
  `artifacts/music-research/funk-iteration-5/full-form-corpus.json`,
  `symbolic-audit.json`, `funk-review.md`, and `style-review.md`. Twenty
  Python analyzer tests pass, the required MSVC release build succeeds, and
  the complete boundary-validation scenario passes. The fixed C++ case
  verifies matched-complexity identity, zero chord operations, chromatic bass
  pickup, horn/ensemble answers, opening-cell recall, break subtraction, and
  the drummer's downbeat-one anchor.

### 2026-07-31 - Hip-Hop / Trap iteration 1

- Generated and inspected 216 complete ideas: 108 Boom-Bap and 108 Trap
  ideas. Each profile uses three native forms, complexities 1/4/8, and twelve
  matched seeds per form/complexity cell. Trap gained a researched twelve-bar
  form alongside its sixteen- and twenty-four-bar forms. The accepted corpus
  has zero raw or transposition-normalized complete duplicates, zero
  research-contract findings, and zero applied-theory or coordinated-lane
  failures.
- The baseline was diverse but behaved like generic chord-led instrumental
  writing. Boom-Bap and Trap leads attacked about 3.13 and 2.83 times per
  bar, both basses were generic one-root-per-bar lines, support averaged
  roughly two attacks per bar, and the drummer changed most structural bars.
  Trap also applied 204 generic chord operations and changed progression or
  motif identity across matched complexity. Those mechanisms contradicted
  repetitive/oscillating beat phrases, rap space, independent Boom-Bap bass,
  and sustained 808 form.
- Boom-Bap now draws from five original beat-phrase families: static minor,
  oscillating minor, a longer minor sample-like cell, a major seventh cell,
  and a Dorian cell. Trap draws from eight sparse minor families, including
  static, tonic-flat-six oscillation, Phrygian neighbour, descending, and
  bounded three- or four-chord routes. Adjacent identical events are
  compacted into true sustained harmonic centres. Major, Dorian, Natural
  Minor, and Phrygian declarations now agree with the chosen route.
- Optional chord operations are deliberately disabled for both profiles.
  Complexity acts on the beat phrase and coordinated performance instead:
  Boom-Bap adds one chromatic bass pickup and sparse answer/return roles;
  Trap adds an audible 808 approach slide, one explicit roll, negative-space
  hat subtraction, kick re-spacing, and a later hook double. All 72 matched
  transitions per profile preserve seed, tempo, progression, groove,
  variation plan, motif cell, and drum identity.
- Both profiles now use a stable sixteenth-note musical address grid. Their
  opening four-bar instrumental cell, or five-bar cell in the odd Boom-Bap
  form, is retained as the seeded identity. Later sections may remove or
  exchange at most one onset rather than rerolling a continuous melody.
  Final lead medians are 1.25 attacks per bar in Boom-Bap and 1.06 in Trap,
  leaving deliberate space for a vocal or rap line while retaining bounded
  instrumental character.
- Boom-Bap harmony is rendered as short sample-like chops and occasional
  recuts; a two-beat musical cut exposes the drums before the loop returns.
  Its bass states a short root and syncopated fifth/octave answer at about
  1.88 attacks per bar, independently of the kick. Developed ideas add one
  chromatic pickup at the loop hand-off rather than converting the bass into
  a walking or chord-doubling line.
- Trap bass is now an explicit 808 performance in MIDI 28-47. Foundation
  sustains the root and uses a bounded octave retrigger; developed ideas add
  a semitone approach with `808-slide` articulation and form-safe duration.
  Final bass density is 1.25 attacks per bar at foundation and 1.5 at
  developed levels. Across the accepted corpus the lane contains 1,872 long
  root events, 468 octave retriggers, and 312 resolving slides rather than
  generic root stabs.
- Support is empty at foundation. Complexity 4 adds exactly one short
  `riff` answer per idea; complexity 8 adds one `hook_double` as a structural
  return or raised-state marker. Final medians are about 0.067 attacks per
  bar in Boom-Bap and 0.063 in Trap, and no support event collides in exact
  unison with the lead.
- The Boom-Bap drummer retains the hard two-and-four backbeat and swung,
  break-like kick/hat identity while using short turnarounds at structural
  boundaries. Trap retains a beat-three half-time snare, applies one
  six-step roll at the Roll/Slide boundary, clears hats for the first two
  beats of its negative-space state, and moves one opening kick to reframe
  that state. Ordinary-bar mutation rates were reduced without suppressing
  fills, dynamics, microtiming, or performed variants.
- GMD contributes 95 directly labelled Hip-Hop performances. Its
  19.68-hit-per-bar median is useful evidence for human backbeat timing,
  velocity, ghost detail, and break-derived continuity; Jam2 Boom-Bap remains
  intentionally sparser at roughly 14.07 hits per bar. GMD is only a proxy
  for Trap and is not used to erase programmed sixteenth rolls, half-time
  negative space, or 808-led form.
- Diversity remains grammar-led. Boom-Bap contains five progression IDs, two
  grooves, 108 normalized melodies, 55 bass lines, 30 support lines, and 36
  drum sequences. Trap contains eight progression IDs, three grooves, 107
  normalized melodies, 40 bass lines, 32 support lines, and 36 drum
  sequences. Repeated harmony, support absence, and shared 808 cells are
  intentional profile backbones rather than complete-generation duplication.
- Final artifacts are
  `artifacts/music-research/hiphop-trap-iteration-5/full-form-corpus.json`,
  `symbolic-audit.json`, `hiphop-trap-review.md`, and `style-review.md`.
  Twenty-one Python analyzer tests pass, the required MSVC release build
  succeeds, and the complete boundary-validation scenario passes. Fixed C++
  cases cover matched-complexity Boom-Bap recall/backbeat/pickup behavior and
  Trap half-time framing, 808 register/duration/slide, roll, negative-space,
  kick re-spacing, and sparse role activation.

### 2026-07-31 - Reggae iteration 1

- Generated and inspected 108 complete Roots Reggae ideas over the new
  twelve-bar steppers activation form, sixteen-bar riddim arc, and
  twenty-four-bar Roots arc. Every form uses complexities 1/4/8 and twelve
  matched seeds. The accepted corpus has zero raw or
  transposition-normalized complete duplicates, zero research-contract
  findings, and zero applied-theory or coordinated-lane failures.
- The 72-idea baseline had enough raw variation but the wrong ensemble
  hierarchy. Melody attacked about 2.94 times per bar and was rerolled across
  sections, bass played a generic root/fifth/flat-seven note on every beat,
  kick and bass overlapped on roughly 95 percent of bass attacks, complexity
  applied 260 optional chord operations, and advanced support became a
  near-continuous second lead. The arrangement labels did not encode the
  researched bass/skank/bubble/dropout relationships.
- Roots Reggae retains four simple, explicit riddim routes: `I-IV-V-I`,
  `I-V-vi-IV`, minor `i-flat-VII-flat-VI-flat-VII`, and Mixolydian
  `I-flat-VII-IV-I`. Their Major, Natural Minor, and Mixolydian declarations
  now agree with the chosen route. Optional chord operations are disabled:
  the existing routes already supply appropriate diatonic, dominant, and
  subtonic colour, while complexity is expressed through performance and
  form.
- The new twelve-bar form always uses the steppers drummer backbone and moves
  from full riddim to skank subtraction and bounded response/return. The
  sixteen-bar form explicitly states Riddim, Bass/Answer, two-beat Dub
  Dropout, and Return. The twenty-four-bar form develops the same identity
  over three eight-bar states. Short generic eight-bar requests receive a
  valid compact two-state form rather than malformed zero-length sections.
- The musical address grid is now a stable sixteenth grid, while every skank
  attack is placed on the eighth-note offbeat. The previous use of grid index
  one would have become a sixteenth-note displacement when the stable grid
  was introduced; the implementation uses the grid midpoint explicitly.
  Steppers A-prime removes alternate skanks, and the dub state removes upper
  chord and melody attacks for two beats while bass and drums remain intact.
- The lead is now a four-bar vocal-like call with a maximum of two ordinary
  attacks per bar, a bounded final arrival, a 57-76 MIDI tessitura, and
  maximum seven-semitone motion. Later states recall its onset map with at
  most one normal edit, except for the intentional dub subtraction. Median
  density is 1.58 attacks per bar rather than 2.94, and the accepted corpus
  still contains 107 normalized melodic realizations from 108 ideas.
- Bass now uses a two-bar melodic root/chord-tone identity at two attacks per
  bar. Developed levels add one semitone pickup at each four-bar hand-off,
  raising density only to 2.25. Long rounded notes, midpoint answers, and
  syncopated pickups reduced median kick/bass onset overlap from roughly 0.95
  to 0.24 without weakening bass leadership.
- Organ bubble is a foundational complementary comping role at roughly two
  pulses per bar. Complexity 4 replaces one selected bubble with a short
  organ- or horn-like call response; complexity 8 adds one final upper
  response at the dub/return boundary. The roles remain
  `support_comping`, `call_response`, and `countermelody`, with no exact
  lead/support unison collisions.
- One-drop, Rockers, and Steppers remain distinct drummer backbones. One-drop
  is contractually checked for open beat-one kick space and a beat-three kick;
  Steppers preserves four-floor drive; Rockers remains the conservative
  driving family rather than one claimed universal kick formula. Ordinary
  mutation rates were reduced, giving median backbone reuse 0.50 while a
  typical full idea still has eight structural and eleven performed bar
  variants with structural fills.
- GMD supplies 20 directly labelled Reggae performances. Its 13.33-hit
  median is useful evidence for lilted hat/rim/percussion timing, velocity,
  and human fill detail. Jam2 remains intentionally sparser at 8.0 hits per
  bar because the profile is a restrained Roots interlock and GMD's
  electronic-kit population is not an absolute density or mix target.
- Diversity remains grammar-led. The final corpus contains four progression
  IDs, three groove IDs, 107 normalized melodies, 28 bass realizations, 83
  support realizations, and 36 drum sequences. Repeated bass and harmony
  signatures are the selected riddim backbones; complete ideas remain
  distinct through route, groove family, call, form, pickup, response, and
  performance detail.
- Final artifacts are
  `artifacts/music-research/reggae-iteration-8/full-form-corpus.json`,
  `symbolic-audit.json`, and `reggae-review.md`. Twenty-two Python analyzer
  tests pass, the required MSVC release build succeeds, and the complete
  boundary-validation scenario passes. The fixed C++ case checks matched
  identity, zero chord operations, bass pickup, bubble/response roles,
  offbeat skank, dub silence, opening-call recall, one-drop placement, and
  steppers-form routing.

### 2026-07-31 - Bossa Nova iteration 1

- Generated and inspected 108 complete Bossa Nova Songbook ideas over
  thirty-two-bar AABA, thirty-two-bar ABAC, and asymmetric eighteen-bar
  forms. Every form uses complexities 1/4/8 and twelve matched seeds. The
  accepted corpus has zero raw or transposition-normalized complete
  duplicates, zero research-contract findings, and zero applied-theory or
  coordinated-lane failures.
- The baseline exposed a generic Jazz/Latin interpretation rather than the
  narrower researched Bossa grammar. Melody attacked about 2.9 times per
  bar, matched complexity changed the progression in 25 of 72 transitions,
  the eighteen-bar form was incorrectly divided into three six-bar blocks,
  bass and kick attacked together throughout, and advanced support became
  a recurring second line. Chord rhythm used one largely universal pattern
  instead of a small contextual upper-part vocabulary.
- All five researched songbook routes remain available: two-six-five-one,
  one-six-two-five, minor two-five-one, backdoor motion, and a directed
  three-six-two-five cycle. Major and Natural Minor declarations agree with
  the route. Progression, groove, tempo, variation plan, and motif identity
  are now stable across all 72 matched-complexity transitions.
- Complexity no longer chooses a different harmonic backbone. Foundation
  uses the selected route unchanged; complexity 4 permits one directed
  voice-leading colour and complexity 8 permits two. Inversion, passing
  diminished, secondary dominant, backdoor dominant, and tritone
  substitution remain available only within that bounded budget. Melody and
  bass are coordinated with every altered chord and resolution; all 108
  applied operations pass the functional and lane audit.
- The thirty-two-bar forms explicitly describe AABA and ABAC theme,
  contrast, return, and tag relationships. The eighteen-bar form is now
  genuinely 5+5+4+4: a five-bar call, related five-bar answer, four-bar
  turn, and four-bar return/tag. Its two-bar theme recall is indexed from
  each section start, so the odd first-section length no longer displaces
  the returned attack map.
- The upper chord lane uses a seeded three-family binary grammar of
  anticipations and answers on the sixteenth grid. Families persist over
  two-bar spans and make bounded section-aware changes. Upper voicings never
  collapse onto the structural bass pulse, preserving the independent
  bass/comp relationship without claiming one universal Bossa clave.
- Melody is a restrained vocal-like phrase with at most two ordinary attacks
  per bar, longer selected durations, bounded theme recall, MIDI 57-77
  tessitura, and maximum seven-semitone motion. The repeated-pitch fallback
  now selects a nearby guide or scale tone after register correction.
  Median density is 1.44 attacks per bar, while all 108 ideas retain distinct
  normalized melodic realizations.
- Bass retains exactly two events per bar. The root or written inversion owns
  the first pulse; a delayed fifth answers at beat two-and-a-half, between
  the structural kick and upper voicing attacks. Developed ideas replace a
  selected phrase-edge answer with one chromatic approach resolving into the
  next section. The reported kick/bass overlap remains 1.0 because the
  drummer has one kick per bar and that downbeat kick intentionally shares
  the root; the independent second bass attack has no matching kick.
- Support is silent at foundation. Complexity 4 adds one quiet inner
  guide-tone colour at new formal regions; complexity 8 adds one
  `countermelody` in the final lead breath. Median support density is 0.094
  attacks per bar with no exact lead/support unison collisions, so the lane
  remains occasional orchestration rather than a second accompaniment.
- Bossa Core and Bossa Sparse remain distinct percussion backbones. A typical
  idea has nine structural and twenty performed bar variants, 0.688 median
  backbone reuse, and fully structural fill placement. Jam2's restrained
  six-hit-per-bar median is not inflated to match the two unusually long GMD
  recordings labelled `latin/brazilian-bossa`, whose 22.73-hit median is
  treated only as descriptive evidence for human percussion timing and
  detail. The binary accompaniment research takes precedence over the
  corpus's unstable triplet-grid summary.
- Diversity remains grammar-led. The accepted corpus contains five
  progression IDs, two groove IDs, 108 normalized melodies, 105 bass
  realizations, 36 support realizations, and 36 drum sequences. Repeated
  two-pulse and accompaniment relationships are deliberate style identity;
  complete ideas remain distinct through route, key, form, upper-pattern
  family, melody, voice leading, and drummer performance.
- Final artifacts are
  `artifacts/music-research/bossa-iteration-6/full-form-corpus.json`,
  `symbolic-audit.json`, and `bossa-review.md`. Twenty-three Python analyzer
  tests pass, the required MSVC release build succeeds, and the complete
  boundary-validation scenario passes. The fixed C++ case checks matched
  identity, 5+5+4+4 form, bounded theory, two-pulse delayed bass, developed
  phrase approach, sparse guide/counterline roles, binary off-pulse upper
  voicings, restrained melody density, and odd-form theme recall.

### 2026-07-31 - Modern Progressive Metalcore iteration 1

- Generated and inspected 72 complete ideas over the twelve-bar heavy module
  and eighteen-bar heavy/clean contrast form. Both forms use complexities
  1/4/8 and twelve matched seeds. The accepted corpus has zero raw or
  transposition-normalized complete duplicates, zero research-contract
  findings, and zero applied-theory or coordinated-lane failures.
- The baseline retained the intended four pitch routes but did not organise
  them into the researched arrangement. The eighteen-bar form was six generic
  three-bar blocks instead of 6+8+4; the separate lead attacked about five
  times per bar in both heavy and clean space; complexity 8 generated roughly
  3.7 continuous support attacks per bar; and near-zero drummer-backbone reuse
  made most riff bars unrelated. Bass and guitar did lock to kick, but without
  enough module identity or clean/heavy contrast.
- Four narrow pitch backbones remain available: low Phrygian pedal and flat
  two, Aeolian roots, heavy-to-clean extended colours, and chromatic
  neighbour pedal. Their Phrygian and Natural Minor declarations agree with
  the selected route. Optional chord operations are disabled because
  complexity belongs to grouping, articulation, riff coordination, section
  role, and orchestration rather than generic harmonic decoration.
- The twelve-bar form is now Heavy A, Displaced A-prime, and Open
  B/Breakdown in three four-bar modules. A-prime retains the riff pitch/attack
  identity but moves selected chokes. B opens the chord articulation, switches
  to the related half-time drummer state, and gives the clean-vocal/lead
  analogue more space.
- The eighteen-bar form is now the researched 6+8+4 arc: six bars of grouped
  Heavy A, eight bars of Clean B augmentation, and a four-bar compressed heavy
  return. It retains written 9/8 with 3+3+3 pulse grouping because the current
  engine has one meter per idea. The compressed return makes the intended
  metric change audible by omitting the actual final riff attack and leaving
  a choke/breath, regardless of which heavy backbone supplied the final bar.
- Heavy chord-riff attacks are generated from the authored kick pattern and
  use `palm-muted`, `open-accent`, and selected `gated-choke` articulations.
  Clean/open sections stop copying every kick and instead use sustained
  attacks on the written larger pulses. Bass reinforces every authored guitar
  attack in its own low split lane. Median kick/bass overlap is about 0.80:
  heavy attacks intentionally lock, while the clean/open section creates
  independence.
- The separate melody is no longer a second busy riff. Heavy sections use a
  sparse two-bar hook whose attack map returns in Displaced A-prime or the
  compressed return with at most one bounded edit. Clean/open sections force
  two structural phrase anchors per bar, use longer selected durations, and
  allow a restrained singable contour. MIDI is bounded to 55-79 with
  seven-semitone maximum motion and repeated-pitch correction. Final median
  density is 1.72 attacks per bar rather than about 5.0; 71 of 72 ideas retain
  distinct normalized melodic realizations.
- Support is now role- and section-specific. The eighteen-bar foundation uses
  only four two-bar-spaced clean-section `pad` entries. Complexity 4 adds one
  `hook_double` at the displaced or compressed riff state; complexity 8 adds
  one `countermelody` in the final clean/open lead breath. Median support
  density is 0.195 attacks per bar with no exact lead/support unison
  collisions, replacing the previous continuous high-complexity layer.
- The primary drummer is selected from Grouped Modern Metal or Breakdown
  Half-Time; Clean/Heavy Contrast is now a sectional drummer role rather than
  a possible whole-idea backbone. Ordinary mutation was reduced from five
  kick and five development gestures per eight bars to one of each, with one
  timekeeper gesture and structural fills. Median backbone reuse rose from
  zero to 0.278 while a typical idea still contains eleven structural and
  thirteen performed bar variants, preserving composed transitions without
  turning every bar into a new riff.
- GMD Rock is only a proxy: its 341 performances and 15.13-hit median inform
  human timing, velocity, backbeat, and fill continuity, not Metalcore kick
  grammar. Jam2's accepted 10.39-hit median reflects half-time and clean
  states as well as grouped heavy sections. Magenta's sparse modern-Metal and
  odd-meter coverage, the processed Drum Groove Corpora aggregate, and the
  focused Garza/Hannan/Hudson plus Spiritbox/ERRA/Broomhall research are kept
  in their proper evidence roles; no corpus density is treated as a target.
- Diversity remains grammar-led. The accepted corpus contains four
  progression IDs, two primary heavy groove IDs plus the internal clean
  sectional family, 71 normalized melodies, 24 bass realizations, 25 support
  realizations, and 24 drum sequences. Matched complexity preserves
  progression, primary groove, tempo, variation plan, motif identity, and
  drummer sequence across all 48 transitions; it develops only the bounded
  layers described above.
- Final artifacts are
  `artifacts/music-research/metal-iteration-4/full-form-corpus.json`,
  `symbolic-audit.json`, and `metal-review.md`. Twenty-four Python analyzer
  tests pass, the required MSVC release build succeeds, and the complete
  boundary-validation scenario passes. The fixed C++ case checks
  matched-complexity identity, 6+8+4 form, zero chord operations, pad/double/
  counterline activation, heavy/open articulations, kick lock, bass
  reinforcement, clean-lead prominence, and final-module attack subtraction.

### 2026-07-31 - Final all-style regression and cross-style audit

- Regenerated the complete production-generator matrix after every
  style-specific refinement: all 27 profiles, every declared form,
  complexities 1/4/8, and six matched seeds per cell. The accepted
  `final-all-styles-iteration-2` corpus contains 1,386 complete ideas over
  231 fixed comparison cells and 462 matched-complexity seed groups.
- The first full regression found four edge cases rather than a broad style
  regression. Two high-complexity Minor Blues ideas let the answer half of
  too many form lines become denser than the call. Two twelve-bar Neo-Soul
  ideas exceeded the researched vocal-like ceiling by only one onset in a
  bar. The generator now tracks Blues call and answer attacks directly and
  suppresses an answer once it reaches the call count; Neo-Soul now has a
  deterministic three-onset-per-bar ceiling. A complete fresh regeneration
  has zero research-contract findings across every profile.
- There are zero identical complete-idea pairs among 3,465 within-cell
  comparisons, both in raw form and after pitch-transposition normalization.
  This is not diversity for its own sake: individual lanes deliberately
  retain researched backbones where the style calls for them, while melody,
  bass detail, drummer performance, route, section role, or long-form
  development keeps the complete realization distinct.
- Matched comparisons use the same seed at complexities 1, 4, and 8. All 924
  transitions preserve tempo, the authored drummer performance, and the
  arrangement variation plan. Harmonic backbones change in only 39
  transitions where an enabled researched chord operation is actually
  applied; motif cells change in only 25 transitions where the profile's
  developed grammar calls for transformation. This keeps complexity focused
  on musically coordinated additions rather than unrelated regeneration.
- Every applied chord operation is checked for its local function and for
  melody/bass/support coordination. Style contracts additionally cover
  form, density, phrase breathing, harmonic rhythm, section return,
  accompaniment role, drum flow, fill placement, and style-specific
  complexity behaviour. The final matrix has zero contract failures and zero
  analyzer findings.
- GMD relationships remain explicitly bounded in the generated review.
  Blues, Bossa, Country, Breakbeat, Funk, Boom-Bap, Swing, Pop, Reggae, Soul,
  and core Rock use direct labelled subsets where available. House, Techno,
  Trap, Bebop, Fusion, J-Pop, Metal, Modal Groove, Neo-Soul, and Shuffle Rock
  use clearly marked proxies; Atmospheric Modal remains unsupported by GMD.
  Dataset timing, velocity, coordination, density, and fill summaries are
  descriptive evidence, never automatic style or quality scores. Existing
  profile research takes precedence where GMD coverage is sparse or
  semantically mismatched.
- The accepted evidence is
  `artifacts/music-research/final-all-styles-iteration-2/full-form-corpus.json`,
  `symbolic-audit.json`, and `all-style-review.md`. The audit excludes timbre
  and mix as required. Twenty-four analyzer unit tests pass, the required
  MSVC release build succeeds, and the full boundary-validation suite passes
  against `release/jam2.exe`.

### 2026-07-31 - Independent all-style holdout pass

- Ran an additional independent holdout after the accepted all-style matrix,
  using a new seed namespace and eight unseen matched seeds per profile/form/
  complexity cell. This is 1,848 new complete ideas across all 27 profiles,
  14 styles, 231 comparison cells, and 616 matched-complexity groups. It is a
  genuinely new generator sample rather than a second reading of the earlier
  1,386 ideas.
- The first holdout uncovered two narrow long-form edge cases. Four House
  realizations allowed a two-bar process mutation to add an eighth attack to
  an already full identity cell, pushing the repeated hook just beyond its
  researched lead-density ceiling. Four Atmospheric Modal realizations
  reached the Colour Reveal in a high register where the ordinary A5 melody
  ceiling rejected B5 or Bb5; fallback chord tones then prevented the melody
  from stating the mode-defining degree.
- House process mutation is now bounded to seven attacks in every two-bar
  cycle. It can replace or subtract a slot without accumulating a continuous
  general-purpose lead over a thirty-two-bar form. Atmospheric Modal now
  treats the first attack of the authored Colour Reveal as a structural
  mode-defining anchor and permits that one sparse anchor the nearest pitch up
  to C6, avoiding a gratuitous register jump while guaranteeing the Dorian,
  Lydian, Phrygian, Mixolydian, or Aeolian characteristic degree.
- Regenerated all 1,848 holdout ideas from the corrected release engine. The
  accepted holdout has zero research-contract failures, zero functional or
  coordinated-lane theory failures, and zero raw or transposition-normalized
  complete duplicates across 6,468 within-cell pairs. The least-diverse
  profile in this larger sample still has 46 normalized melodies and 16
  complete drummer sequences.
- All 1,232 complexity transitions preserve tempo, authored drummer
  performance, and the arrangement variation plan. Only 34 transitions alter
  the progression and 20 alter the motif cell, in each case through the
  profile's selected researched development grammar rather than a new random
  backbone.
- Added fixed C++ boundary coverage for the formerly failing House seed and
  Atmospheric Dorian seed. The final MSVC release build succeeds, all 24
  analyzer unit tests pass, and the full boundary-validation suite passes
  against `release/jam2.exe`.
- Holdout evidence is
  `artifacts/music-research/final-all-styles-holdout-1/full-form-corpus.json`,
  `symbolic-audit.json`, and `all-style-review.md`. The reproducible scenario
  is `tools/scenarios/music-generation-all-style-holdout.json`.
