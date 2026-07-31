# Jam2 Musical Idea Generator Research and Implementation Plan

## Status and purpose

This document is the controlling plan for replacing Jam2's current musical idea
generation with a more tonal, stylistically grounded, and structurally
interesting system.

**Current status: Phase 1B detailed research and cross-style synthesis are
complete, the catalog and scope were discussed and approved, and the version-6
generator redesign has been implemented. Modern Progressive Metalcore remains
explicitly experimental pending normal listening and sound-design refinement.**

The first deliverable was deep musical research using the twelve current public
style labels as its starting catalog. The research findings, sources,
disagreements, profile proposals, and original form designs must be added to
this document. The work should be expansive enough to represent the music
honestly, but compact enough that each retained profile is well evidenced,
musically distinct, and useful. Those findings must then be discussed and
agreed before implementation began.

The research is not trying to reach a style-count benchmark. It must discover
how many styles Jam2 can realistically represent accurately within its compact
local framework while keeping the results interesting across seeds and
complexity levels and making each style a worthwhile learning tool.

The objective is not to make the output theoretically busier. It is to make
simple ideas more musical:

- Notes and chords must belong to the declared tonal language unless a
  deliberate, explained exception is being used.
- Harmony, melody, groove, bass movement, supporting lines, timbre, motif,
  phrase shape, and loop ending must be conceived together.
- An idea must have audible development across its appropriate span rather than
  being copies of a short resolving loop. Sixteen bars is a useful default, not
  a required native length for every style or profile.
- Styles must be grounded in evidence and coherent musical practice rather than
  broad stereotypes.
- Complexity must control vocabulary and difficulty, not whether the result has
  a satisfying form.
- Idea Details should teach the musical concepts used in the generated result
  well enough that a user can try applying the same principles in a jam.
- Research should preserve the possibility of generating a related A-prime or B
  section from user-authored material, even though that continuation feature is
  not part of the initial redesign.

## Binding instructions for the implementing agent

These instructions apply to every phase of this plan.

1. Read `AGENTS.md` and this entire document before doing any work.
2. Preserve unrelated working-tree changes. Do not overwrite or reformat
   unrelated files.
3. Complete Phase 1 by adding the actual research to this document before
   changing generator code.
4. Do not start Phase 2 or later until the completed research has been discussed
   with the user and the final style/profile catalog and required redesign
   specification have been accepted at the Phase 1B checkpoint.
5. Distinguish evidence, musical interpretation, and implementation choice.
   Never present an ear-authored rule as a corpus finding.
6. Prefer primary sources, peer-reviewed research, public academic corpora,
   authoritative transcriptions, documented performance analysis, and
   representative listening. Secondary explanations may locate or contextualise
   stronger sources and may be used conservatively when stronger evidence is
   unavailable.
7. Cite the material evidence used for research conclusions close enough to the
   finding that a reader can follow it. Use ordinary citations and explain
   limitations when they materially affect a decision. Ordinary inline or
   linked citations are sufficient; fixed metadata fields are not required.
8. Do not copy complete melodies, drum performances, or identifiable
   arrangements into Jam2. Extract general musical principles and author new
   templates.
9. Do not add a runtime machine-learning system, online dependency, large
   dataset, or new third-party library. Research informs a compact deterministic
   local generator.
10. Keep all generated decisions seed-deterministic and expose them in Idea
    Details. Explicit test seeds remain test/diagnostic inputs.
11. Use listening throughout research and implementation because parseability,
    randomness, and chord validity are not proxies for musical quality. Do not
    create a formal acceptance gate that leaves work waiting for the user to
    listen; record and address musical observations as they arise in the normal
    discussion.
12. When a broad style cannot be represented honestly by one grammar, define
    coherent internal profiles. Create a separate profile because its musical
    grammar differs, not merely because its instruments, production, or sounds
    differ. Timbral and production differences may still be important profile
    attributes or variation axes. Keep the catalog compact where the evidence
    permits and keep the public workflow simple.
13. Any deliberate chromatic note must have a declared role, location,
    destination or collection-based justification. Foundational complexity must
    never produce an unexplained exception.
14. Build Windows changes from the repository root using elevated
    `cmd.exe /d /c "call compile.cmd --in-dev-shell"` and test only
    `release/jam2.exe`, as required by `AGENTS.md`.
15. Prefer an honest, high-quality profile catalog over an arbitrary breadth
    target. There is no fixed profile or blueprint count: add material when it
    captures a genuine, relevant difference and can be supported well.
16. Treat the research requirements as musical questions rather than quotas.
    Record when a dimension, form strategy, evidence type, or tonal concept is
    not applicable to a profile instead of forcing it to appear.

## Confirmed product decisions

The following decisions are already agreed and should not be reopened during
implementation unless research produces a concrete contradiction.

- Use the twelve current public style names as the initial research set:
  Pop, Indie, Rock, Jazz, Modal Vamp, Blues, Anime / J-Pop, Country, EDM,
  R&B / Soul, Funk, and Hip-Hop / Trap.
- The final public catalog is not required to contain exactly those twelve.
  Research must evaluate whether each label is musically coherent and useful
  for Jam2. It may recommend retaining, narrowing, renaming, merging, splitting,
  replacing, or removing a starting style, and may recommend an additional
  style when the evidence and Jam2 use case justify it. Candidate changes are
  discussed provisionally in Phase 1A and approved for implementation only at
  the Phase 1B final checkpoint.
- Remove the current global **Mood/Character** model and its Generate-dialog
  selector for new generation.
- Replace Mood with a seed-derived, style-guided **variation plan**. Its axes
  may describe energy, intensity, density, openness, tension, articulation,
  motion, emotional colour, performance feel, or a style-specific tendency.
  The available axes and their musical effect must come from the selected
  style/profile rather than a universal list.
- A variation may resemble a mood where that is musically useful. For example,
  a romantic or yearning leaning may be appropriate for some Pop or R&B
  profiles, while sparse, menacing, rolling, or hypnotic may be more meaningful
  distinctions for Trap.
- Variation plans are not required to be subgenres. They are bounded ways for a
  seed to take an otherwise coherent style profile in a particular direction.
- Keep internal variants automatic rather than adding a Variant control to the
  primary Generate dialog. Record the chosen profile and variation plan in Idea
  Details.
- Add `Scale / mode` with `Auto` plus compatible manual overrides. The list is
  filtered by the selected style; an explicit mode with Random Style constrains
  which styles are eligible.
- Treat the current eight complexity levels as provisional. Research must
  propose the clearest number and ordering of levels for teaching distinct
  musical tools. The levels should share a broad conceptual progression across
  the catalog, while each profile applies those tools in its own idiomatic way.
- A complexity level unlocks an eligible technique; it does not force that
  technique into every seed. Foundational complexity uses simple voicings and
  the profile's conservative native pitch collection: diatonic for ordinary
  major/minor tonal profiles, or an explicitly declared modal, blues, or other
  essential collection where removing it would make the style inaccurate.
  Later levels may introduce collection-preserving extensions and inversions,
  directed tension such as secondary dominants, modal interchange, chromatic
  approaches, substitutions, modulation, rhythmic devices, or analogous
  profile-native tools in an order established by research.
- Include a four-phrase arc, two eight-bar periods, and a continuous evolving
  loop among the structural strategies evaluated by the research. They are
  useful shared models, not an exhaustive list or a requirement that every
  style or profile support all three. Native forms and other well-supported
  structures may be preferable. Research determines compatibility.
- Use a profile's native form length where it has one. For example, a 12-bar
  blues may be fixed at 12 bars. Do not preserve the current 4/8/12/16 choices
  or introduce another fixed global whitelist. `Auto` is the normal default
  when a profile has several researched lengths.
- Treat form length as a compatibility choice rather than a request to stretch
  material. `Auto` chooses among the selected profile's supported native
  lengths using its researched weights. An explicit manual length filters the
  eligible profiles and forms. If the selected style has no honest profile for
  that length, show the incompatibility rather than silently truncating,
  padding, or changing the music. A profile with one defensible native length
  may lock that length clearly in the Generate workflow.
- Replace the current implied `N/4` time model with explicit, teachable meter
  support. Phase 1B must research and specify at least numerator, denominator or
  beat unit, beat grouping/accent, compound subdivision, click behaviour,
  half-time/double-time interpretation, and whether any retained profile needs
  section-local or changing meter. The engine and UI must support the approved
  profile-native meters rather than forcing them through a 4/4 approximation.
- Prefer endings that lead from the final bar back into bar 1. A closed ending
  may exist when research supports it, but must not be the universal default.
- Apply the redesign to the whole approved catalog rather than leaving some
  retained styles on the old generator.
- Provide a fixed-seed listening suite in addition to automated theory tests so
  outputs can be compared and discussed consistently without making formal
  listening sign-off a blocking gate.
- Keep the profile catalog deliberately compact at the first research
  checkpoint, while allowing additional profiles when a style contains a
  genuinely different harmony, groove, melody, form, or performance grammar.
  Do not impose a numerical cap.
- Use songs and recordings as evidence for how a style works, then extract
  general principles and create original jam-ready material. Jam2 is not
  required to generate condensed copies of complete songs.
- In styles whose characteristic melody is predominantly vocal, generate an
  instrumental melody with plausible vocal phrasing, including pickups,
  breathing space, repetition, range, and melisma-compatible space where
  appropriate.
- Make the default Idea Details popup primarily a practical teaching surface.
  Use correct musical terms, explain them in accessible language, point to
  where they occur in the current idea, explain why they fit the profile, show
  the simple foundation and the authentic techniques introduced at relevant
  complexity steps up to the selected level, and suggest how the user could
  apply the concept while building or jamming over their own section.
  Provide a **Detailed Analysis** control that expands the raw technical
  decisions for diagnosis. Keep the same analysis available in structured
  diagnostic logs or artifacts, and keep academic source detail in this
  research document rather than crowding the default teaching view.
- Research section-to-section flow as well as self-contained loops. A future
  continuation feature should primarily analyse what the user's A section is
  actually doing and how it could be followed. The style catalog may inform
  groove and general practice, but the continuation must not be rigidly locked
  to a possibly uncertain style classification. Any inferred style/profile
  should be visible and overridable.
- Add bass as a dedicated musical line rather than only an implied chord root.
  Also add explicit role-aware support for melodic harmony, countermelody,
  response, pad, riff, and other supporting lines separately from chordal
  comping where profiles need them. Phase 1B must research their musical
  grammars and specify the smallest clear data, view, recipe, editing, teaching,
  and rendering model; these are required redesign capabilities, not optional
  future notes.
- Research characteristic timbre and production behaviour that materially
  affects style recognition, including the percussion voices and articulations
  needed by approved profiles. Specify and implement compact, inspectable
  synthesis targets using feasible parameters such as sound family, register,
  articulation, envelope, brightness, filtering, saturation, noise, modulation,
  and space. These timbre and percussion improvements are required redesign
  work, but must not disguise an inaccurate musical grammar or expand Jam2 into
  a broad production platform.

## Current implementation audit

The research phase must verify this audit against the code before relying on it.
Before judging which styles Jam2 can represent, also record the current
capabilities and realistic local extension points for:

- Meter, beat and bar representation, compound subdivision, swing, and
  half-time/double-time interpretation.
- Chord identity, voicing, articulation, and the relationship between the chord
  view and generated events.
- Independent bass, melodic harmony, countermelody, and other supporting-line
  data.
- Available synthesis voices and inspectable oscillator, envelope, filter,
  articulation, saturation, noise, modulation, and spatial parameters.
- Form length, bank/section relationships, recipe compatibility, Idea Details,
  structured diagnostic output, and fixed-seed rendering.

### Verified Phase 1A capability baseline

The Phase 1A code audit verified the following baseline. This describes the
current implementation; it is not an endorsement of the current musical rules.

| Dimension | Current support | Consequence for research |
|---|---|---|
| Public styles and internal grammar | `PracticeIdeaGenerator.cpp` has twelve fixed style records. Each record contains a tempo range, three patch labels, and six manually authored progressions. Each style also has five authored two-bar groove families. There is no profile layer and no evidence model behind these tables. | The existing labels and patterns are useful prototypes, but no current row should be treated as a researched style definition. A profile must become a first-class musical grammar rather than another patch or progression label. |
| Length, meter, and subdivision | Generate offers only 4, 8, 12, or 16 bars. It inherits a global `beatsPerBar`, which the recipe renders as `N/4`; a denominator, compound-beat grouping, and section-local meter are not represented. Drum divisions support 1, 2, 3, 4, 6, and 8 steps per beat; chord/melody divisions support 1, 2, 3, and 4. | Triplet and compound-feeling subdivisions can already be scheduled, but calling them true 6/8, 12/8, or 2/4 would be misleading. The redesign must add explicit meter semantics and research-driven native form lengths without a fixed global bar-count whitelist. |
| Harmony and voicing | Chords, slash-chord bass notes, per-beat chord rhythm, close/spread/voice-led rendering, sevenths, extensions, and several chromatic operations are present. The current tonal-realisation bugs listed below make some results invalid. | The event model can support much of the proposed harmonic research after its tonal foundation is corrected. Slash chords are not a substitute for an independent bass part. |
| Melody and supporting lines | One monophonic melody lane supports onset, hold, rest, pitch, velocity, and per-beat subdivision. Current melody candidates use generic four-bar contours and chord-tone scoring. There is no separate harmony, response, countermelody, riff, or supporting-line lane. | Vocal-like instrumental melody is feasible. Profile-specific phrase rhythm, motif development, melodic-harmonic independence, riffs, vocal harmony, and call-and-response need new researched rules; polyphonic support needs an explicit data extension. |
| Bass | The only explicit bass information is an optional slash-chord note that is folded into chord rendering. | Dedicated bass is the single highest-leverage required extension: it materially affects Jazz, Country, Funk, R&B/Soul, Hip-Hop/Trap, Reggae, Bossa Nova, and several Electronic profiles. Phase 1B must define its grammar, data shape, view, recipe, editing, teaching, and renderer. |
| Drum groove and feel | Seven kit lanes are available: kick, snare, closed/open hi-hat, tom, crash, and ride. Authored two-bar patterns support triplets, sixteenths, global swing, snare offset, timing variation, velocity variation, and generic fills. | This is a useful compact groove engine, but it lacks profile-specific long-range development, per-lane microtiming, and characteristic percussion such as clap, rim/cross-stick, shaker, conga, cowbell, and clave. |
| Timbre and synthesis | Chord and melody rendering use compact sine, triangle, saw, pulse, and FM-style patches with layers, detune, harmonic colour, drive, filtering/brightness, envelope, and delay. Drum rendering maps patch-name substrings to four rough synthetic kit families. Melody patches are deliberately constrained to warm sine/triangle-like sounds. | Electronic and compact keyboard timbres are plausible, but current patch names overstate the accuracy of guitar, acoustic-piano, brass/reed, drum-machine, and style-specific drum sounds. Research can propose small inspectable synthesis and articulation extensions, not a production platform. |
| Form, sections, and banks | Song/reference data can hold multiple named sections. The first four map naturally to Banks A–D; later sections remain reference-only. The generator currently fills one generated section and its displayed form names do not control the material deeply. | A future A-to-A-prime/B continuation fits the existing section/bank model. The missing work is musical inference and section-role planning, not a new room or project architecture. |
| Recipe, details, and diagnostics | The audited legacy recipe recorded the seed, selected tables, harmony edits, groove timing, melody events, patches, and fingerprints. Idea Details showed the raw `generationRecipeDetails` text in one read-only text box. | Existing data shapes were useful research inputs, but the implemented redesign uses one strict version-6 recipe and rejects all earlier generated metadata instead of migrating it. |
| Validation | Existing checks cover bounds, schema validity, determinism, event density, chord parsing, seed variety, and some strong-beat chord-tone behaviour. | Tests do not currently establish tonal membership, exception resolution, phrase function, motif identity, style fit, native form, bass/line independence, timbral target, or audible loop quality. |

The audit therefore does **not** impose a “styles the current code can already
fake” limit. It identifies a compact extension boundary. The preliminary
highest-leverage candidates for Phase 1B assessment are:

1. An independent bass lane, view, recipe model, and renderer.
2. One or more supporting pitched lines with declared musical roles.
3. Explicit meter denominator/beat unit and grouping plus unrestricted
   research-driven native form lengths.
4. Profile-level long-range form, arrangement/energy, and section-role data.
5. Per-lane groove timing plus an inspectable expansion of profile-required
   percussion and synthesis articulation.
6. A teaching-first Idea Details summary with expandable raw analysis.

All six are approved redesign outcomes. Phase 1B must define the smallest
coherent implementation of each without reducing a style to unsupported labels
or growing Jam2 into a general production environment.

This capability audit informs research feasibility; it does not prevent the
research from defining the Jam2 engine, view, and synthesis enhancements needed
to deliver the approved hard scope accurately. The following generator problems
have already been identified.

- The style, progression, groove, tempo, and mood tables were manually authored.
  No supporting corpus import, citations, or research notes were found in the
  repository or the commit that introduced the current catalog.
- A mood can replace the mode after a progression has been selected. This can
  make a valid template incompatible with its realised roots and qualities.
- Roman-numeral accidentals are applied relative to an already altered modal
  scale. In C natural minor, the current calculation can turn `bVI` from A-flat
  into G and `bIII` from E-flat into D.
- Minor-mode detection relies partly on the display name beginning with a
  lowercase `i`. Progressions beginning `ii` or `iii` can therefore select a
  minor collection accidentally.
- Complexity 1 disables advanced theory operations but does not protect against
  these baseline tonal errors.
- The final bar is generally overwritten with a tonic chord, including cases
  where the style or native form calls for a turnaround.
- Harmony is largely a progression repeated modulo its length. One four-bar
  segment is rotated in a sixteen-bar idea, but phrase-level harmonic function
  is not genuinely planned.
- Melody form names such as `A-B-C-A'` do not fully control the generated
  material. The same four-bar arc and a small set of rotations/inversions are
  used regardless of what the label claims.
- Motifs are mostly continuous random contours reharmonised one note at a time.
  Their rhythm and interval identity are not deliberately developed across the
  whole form.
- Drum grooves begin with useful authored two-bar patterns but then repeat,
  receiving generic random variations and four-bar fills rather than
  form-aware development.
- Global moods use generic associations and groove-ID substring matching.
  Their meaning is not equally valid across all styles.
- Existing tests emphasise schema validity, bounds, density, chord parsing,
  seed variety, and strong-beat chord tones. They do not prove tonal membership,
  exception resolution, phrase function, audible motif identity, style fit, or
  loop quality.

# Phase 1 — Deep musical research

## Phase gate

Phase 1 changes documentation and may add disposable local analysis artifacts,
but it does not change generator behaviour. Its lasting deliverable is the
completed research section in this file.

### Phase 1A — Provisional research-catalog checkpoint

First survey the current starting styles and plausible additions discovered
during that survey, define candidate repertoire boundaries, and propose a
compact public catalog and profile set. Present that candidate catalog to the
user with the reason each style is useful to Jam2, the reason each profile needs
a distinct musical grammar, its preliminary evidence strength, and its
deliberate exclusions.

Approval at this checkpoint means that a candidate is worth detailed research;
it is not final approval for implementation. Detailed evidence may still
justify adding, merging, reframing, or removing styles or profiles. Bring any
material revision back to the user rather than treating the provisional catalog
as an immutable quota.

The code audit and evidence survey produce the following provisional catalog.
“Research” means the profile is worth detailed Phase 1B study, not that its
implementation is already approved.

| Candidate public style | Provisional profiles | Why Jam2 can represent and teach it | Meaningful musical distinction | Evidence confidence or open issue | Phase 1A decision |
|---|---|---|---|---|---|
| Pop | Start with **Contemporary Pop**. Test whether loop-centred and more sectional/dance-oriented practice require separate grammars or only form, groove, and variation families. | Chord loops, vocal-like hooks, backbeats, section energy, and compact synths all fit Jam2. It is a useful foundation for teaching melody/harmony coordination and section flow. | A profile split is justified only if detailed research finds different phrase, form, and layer relationships—not merely a ballad, dance patch, or energy change. | **High evidence, broad label.** Large melody, harmony, groove, and form corpora exist; repertoire boundaries still need narrowing. | **Research; retain and narrow.** |
| Indie (starting label, not proposed as a public style) | No general Indie profile. Reassign defensible material to Rock, Pop, or Modal Jam profiles and retain indie-like timbre only as a variation where appropriate. | Jam2 can produce some recognisable indie-associated sounds and grooves, but the label does not supply one stable musical grammar. | Scholarship describes indie as a motley family and finds timbre/production to be a primary differentiator; a public generator row would often be Rock or Pop rules plus a patch. | **High confidence in the coherence problem.** Particular substyles such as shoegaze, jangle, art rock, and indie folk could be studied separately later, but most depend on articulation/production Jam2 does not yet model well. | **Do not advance as one public style.** |
| Rock | **Riff/Modal Rock**, **Shuffle/Blues Rock**, and **Punk/Garage**. Alternative or indie-derived ideas enter only where they fit one of these musical grammars. | Power/root motion, modal mixture, riffs, backbeats, shuffles, vocal-like melody, and compact section forms are teachable in the grid. | The profiles differ in pitch collection, riff behaviour, drum feel, articulation, phrase density, and form. Punk is not just a distorted patch: its fast emphatic riffs, restricted pitch/vocal shapes, and concise forms are a coherent learning grammar. | **High musical evidence; medium-high current fit.** Convincing guitar/bass articulation and riff representation need work. | **Research; retain, absorb the coherent part of Indie, and expose Punk/Garage internally.** |
| Modern Metal feasibility candidate | Narrow the study to **Modern Progressive Metal / Metalcore**, using Spiritbox, ERRA, and Nick Broomhall's modern riff writing/production as central references. Do not treat this as coverage of the wider Metal family. | Jam2 could teach low-register pedal/riff construction, syncopation and metric displacement, breakdown and riff-module form, clean/heavy contrast, melodic lead or vocal-like hooks, bass/drum reinforcement, and ambient supporting layers. | In this practice, distortion, palm muting, pick attack, gating, low tuning, cabinet filtering, stereo layering, bass support, and drum transients change how pitch tension and rhythmic weight are perceived. The musical grammar and sound design therefore have to be researched and tested together. | **Strong analytical basis; uncertain Jam2 fit.** Current oscillators and patch-name synthesis cannot do this justice. Phase 1B must prototype a compact synthetic plucked-string/attack, mute/open articulation, amp/distortion/cabinet chain, low-register bass interaction, modern metal drum/percussion palette, and clean/ambient contrast, then compare it by listening with the reference set. | **Add to Phase 1B as a feasibility study. Promote it to a public style only if the musical and synthetic results remain recognisable and satisfying without samples, copied riffs, or a large production system.** |
| Jazz | **Swing/Standards**, **Bebop**, and **Jazz Fusion**. Treat ballad/two-feel as a feel or form family unless research proves a separate grammar; route jazz blues through compatible Blues forms rather than duplicating it. | Jam2 can teach swing and straight subdivisions, seventh/extended harmony, voice leading, head-like melody, improvisational vocabulary, comping, electric/modal/funk interaction, and chorus or riff-based form. | Swing/Standards centres head, chorus form, comping, and walking/two-feel bass; Bebop raises harmonic rhythm, chromatic approach language, melodic density, and interactive comping; Fusion changes groove, instrumentation/timbre, bass/riff relationship, modal/static harmony, and form enough to justify separate research. | **Strong Swing/Bebop corpora; Fusion evidence needs focused work.** Honest output requires dedicated bass/supporting lines, richer comping, researched timbres, and unrestricted native form lengths/meters. | **Research; retain with broader but controlled scope.** |
| Modal Jam | **Groove Vamp** (especially Dorian/Mixolydian) and **Atmospheric Pedal** (especially Lydian and selected darker collections). | This is a valuable teaching and jamming category even though it is not one historical genre. Jam2 can make the mode audible through pedals, characteristic degrees, constrained harmony, motif, register, and groove. | The two profiles differ in tonal centre reinforcement, permitted chord motion, rhythmic activity, phrase direction, and use of colour tones—not just in mode name or pad choice. | **Good theory foundation; label is an implementation choice.** Phase 1B must prevent a generic “all modes use the same contour” grammar. | **Research; retain but rename from Modal Vamp.** |
| Blues | **Twelve-Bar Major/Dominant Blues** and **Minor Blues**. Treat straight, shuffle, slow, quick-change, turnaround, and supported eight-bar practice as form/groove families rather than automatic new profiles. | Native schemas, blue-note vocabulary, call-and-response, turnaround logic, shuffle/straight feel, and 8/12-bar form are exceptionally teachable in Jam2. | Major/dominant and minor blues have materially different tonal relationships and melody/harmony treatment; groove and turnaround variants can remain inside them. | **High evidence and high fit.** Research must preserve melody/harmony friction and avoid forcing every final bar to tonic. | **Research; retain.** |
| J-Pop / Anisong | **Anisong Pop-Rock** and **Idol/Dance J-Pop** provisionally. | Jam2 can teach energetic vocal-line melody, extended/chromatic harmony, sectional contrast, key movement, supporting calls/harmony, and active groove. | Anisong evidence shows section-boundary modulation and chromatic-mediant practice; idol material also foregrounds multi-singer lead/harmony/call arrangement and varied dance/pop instrumentation. | **Strong recent evidence, medium-high fit.** A 100-song anisong corpus and professionally authored idol-style corpus exist, but the umbrella is eclectic and needs careful boundaries. | **Research; retain and rename from Anime / J-Pop.** |
| Country | **Traditional Honky-Tonk/Two-Step** and **Contemporary Country-Pop/Rock**. Train-beat, boom-chick, shuffle, and waltz/6-8 where supported are native groove/form families. | Simple but directed harmony, alternating bass, vocal melody, turnaround, train/boom-chick feels, and section form are clear teaching material. | Corpus work finds a distinctive three-chord emphasis and avoidance of minor tonic, while recent practice moves toward pop-like loops; that historical grammar change justifies provisional profiles. | **Strong harmony evidence, good groove data, high fit after bass support.** Guitar/pedal-steel articulation must remain honestly simplified. | **Research; retain.** |
| Electronic | **House**, **Techno**, **Breakbeat**, and **Synthwave** provisionally; Phase 1B may reframe the public name or move Synthwave if the umbrella becomes incoherent. | Jam2 already has grid-precise drums, layered oscillators, filtering, delay, pedals/loops, and timing controls. It can teach how texture, rhythm, timbre, and layer entry create motion even with restricted harmony. | EDM scholarship explicitly treats rhythm, meter, texture, and timbre as primary and documents buildup/drop processes. The profiles differ in beat architecture, bass/riff relation, phrase layering, and formal energy, not only patch choice. | **High evidence for shared electronic principles; profile-specific evidence still needed.** Current single-section rendering cannot yet express buildup/drop or accumulative form honestly. | **Research; retain but rename from EDM.** |
| R&B / Soul | **Classic/Motown Soul** and **Contemporary R&B/Neo-Soul**. | Extended voicings, bass-led voice leading, vocal-like melody, response/harmony, swing, pocket, and compact keyboard timbres fit the intended framework. | Historical evidence supports a move from earlier eighth-note swing toward contemporary sixteenth-note swing; neo-soul also uses deliberate microtiming displacement. Soul-dominant sonorities and bass-controlled voice leading require more than generic Jazz chords. | **Strong and improving evidence; medium-high fit.** Per-lane timing, independent bass, and supporting voices are important dependencies. | **Research; retain.** |
| Funk | Begin with one **Static-Vamp/Pocket Funk** profile. Treat disco-funk as cross-style groove/variation unless Phase 1B finds enough distinct grammar for a second profile. | Jam2 can teach “the one,” cyclic syncopation, dominant/static harmony, interlocking rhythm, motif, articulation, and controlled variation. | Identity is carried by the counterpoint of bass, drums, clipped comping, and riff layers over a stable cycle; more chord changes do not mean more authentic funk. | **Strong rhythmic theory; high potential fit but bass is essential.** Existing chord-and-drum output alone would be a caricature. | **Research; retain compactly.** |
| Hip-Hop / Trap | **Boom-Bap** and **Trap**. Treat lo-fi as a production/feel variation unless research finds an independent musical grammar. | Loop length, sparse/oscillating harmony, instrumental hook, drum architecture, timing, and 808/bass motion fit a deterministic grid generator. | Hip-hop phrase research separates repetitive, oscillating, and expansional beat harmony; Trap changes drum subdivision, backbeat/tactus, bass role, density, and timbre enough to warrant its own profile. Rap pitch should not be imitated as if it were a sung vocal melody; generate an instrumental hook unless the selected practice uses singing. | **Strong Hip-Hop phrase evidence; Trap needs deeper profile research.** A dedicated pitched 808/bass lane is a major dependency. | **Research; retain.** |
| Reggae | Begin with one **Roots Reggae** profile containing researched one-drop, rockers, and steppers groove families. Defer Ska unless detailed evidence and current-framework fit justify it separately. | The relationship among bass line, drum placement, offbeat guitar/organ comping, rests, vocal-like melody, and dub-influenced space is highly recognisable and an excellent jamming lesson. | One-drop, rockers, and steppers change kick/snare drive but can share a broader roots grammar; the defining bass/comping relationship separates the style from Rock or Funk. | **Medium evidence, medium-high potential fit.** Practitioner and academic descriptions agree on the core groove families, but Phase 1B needs a stronger representative repertoire analysis. Bass, short skank articulation, cross-stick, and optional percussion are dependencies. | **Add to Phase 1B research.** |
| Bossa Nova | Begin with one narrowly bounded **Bossa Songbook** profile rather than a broad “Latin” style. | It offers strong teaching value through independent bass and syncopated chord attacks, colourful voice-led harmony, subtle vocal-like melody, and compact ensemble interaction. | Research describes João Gilberto's guitar model as reducing samba into bass pulse plus flexible syncopated chord rhythm; melody phrasing and harmony interact without simply behaving like swung Jazz. | **Medium-high evidence, medium current fit.** Honest teaching needs explicit 2/4-style grouping, independent bass/comping, appropriate percussion restraint, and careful repertoire boundaries. | **Research; add with required meter, bass, comping, and percussion support.** |

### Phase 1A evidence foundation

The survey used sources to decide what deserves detailed research, not to turn
preliminary observations into finished generator rules:

- Cross-style melody, groove, and form can be grounded in the
  [Billboard Melodic Music Dataset](https://transactions.ismir.net/articles/10.5334/tismir.168),
  whose 371 manually transcribed lead melodies are also split into 1,133
  section examples; the [Groove MIDI Dataset](https://magenta.tensorflow.org/datasets/groove),
  which contains 1,150 expressive drum performances, timing, velocity, tempo,
  meter, fills, and broad style labels; and the
  [SALAMI structure overview](https://transactions.ismir.net/articles/10.5334/tismir.54),
  which documents expert hierarchical form annotations. These are useful
  evidence pools, not balanced definitions of every style.
- Pop and Rock research can build from the public
  [RS 5x20/RS200 corpus work](https://mtosmt.org/issues/mto.11.17.1/mto.11.17.1.temperley.html),
  the documented interaction of harmony and form, and
  [verse-prechorus-chorus teleology](https://www.mtosmt.org/issues/mto.22.28.3/mto.22.28.3.nobile.html).
  These sources also warn that later form can be driven by texture, timbre, and
  voice rather than chord function alone.
- The recommendation not to preserve Indie as one generator grammar is
  supported by scholarship describing it as a
  [motley family differentiated primarily by timbre](https://www.mtosmt.org/issues/mto.12.18.2/mto.12.18.2.blake.html).
  Conversely, the proposed Punk/Garage research profile has documented
  [riff, power-chord, tempo, drum, vocal, and concise-form conventions](https://www.mtosmt.org/issues/mto.19.25.1/mto.19.25.1.pearson.php).
- Modern Metal is a feasibility study precisely because its notes cannot be
  separated from its sound. Research on
  [contemporary Metal riff types and time feels](https://mtosmt.org/ojs/index.php/mto/article/view/648)
  and the
  [distortion/intelligibility paradox](https://www.taylorfrancis.com/chapters/oa-edit/10.4324/9781315742816-6/distortion-paradox-mark-mynett)
  supports that concern. The narrow listening and production references begin
  with Spiritbox's documented combination of
  [low-tuned riffing and ambient guitar layers](https://www.guitarworld.com/features/spiritbox-mike-stringer-eternal-blue-interview),
  ERRA's account of adapting instrument and technique to
  [song-specific modern Metalcore riffs](https://rocksound.tv/features/erra-cure-the-album-story),
  and Nick Broomhall's
  [Thick Riff process](https://www.guitarworld.com/features/nick-broomhall-thick-riff-thursday)
  of identifying the instrumental causes of a reference track's emotional
  response. These artist references guide analysis and listening; Jam2 must
  still author original riffs and synthesis.
- Jazz has unusually useful public data: the
  [Weimar Jazz Database/Jazzomat project](https://jazzomat.hfm-weimar.de/dbformat/dboverview.html)
  provides solo-transcription evidence, while the
  [Jazz Structure Dataset](https://transactions.ismir.net/articles/10.5334/tismir.131)
  documents head, solo-chorus, and return structures. Their focus also shows
  why a chord-loop-only Jazz label would be inadequate.
- Blues form and tonal practice are supported by research on
  [mode, harmony, and dissonance treatment](https://mtosmt.org/issues/mto.10.16.3/mto.10.16.3.stoia.php),
  [twelve-bar vocal/instrumental phrase placement](https://www.mtosmt.org/issues/mto.20.26.4/mto.20.26.4.stoia.html),
  and a broader corpus study of
  [strophic and contrasting-section blues forms](https://mtosmt.org/issues/mto.25.31.1/mto.25.31.1.carter.php).
- J-Pop/Anisong now has unusually direct evidence. A 2026 study of 100 anime
  openings documents
  [chromatic modulation tied to verse-prechorus-chorus boundaries](https://mtosmt.org/issues/mto.26.32.1/mto.26.32.1.li.php).
  The [IdolSongsJp corpus](https://huggingface.co/datasets/imprt/idol-songs-jp/blob/main/README.md)
  adds fifteen original songs by professional writers with expert chord/key
  annotation and separate lead, harmony, and call tracks. It is valuable
  evaluation evidence but its licence explicitly rules out generative-AI
  training. Jam2 will neither train on nor copy its musical material; Phase 1B
  may use its published aggregate findings and lawful analytical comparison.
- Country harmony can start from
  [de Clercq's 200-song corpus study](https://academic.oup.com/edited-volume/41992/chapter-abstract/371464385),
  which finds country between common-practice and Rock norms, with strong
  three-chord emphasis and avoidance of minor tonic. Phase 1B must complement
  that harmony evidence with performance and groove analysis.
- Electronic research can build from Butler's
  [layered account of EDM rhythm and meter](https://www.mtosmt.org/issues/mto.01.7.6/mto.01.7.6.butler.html)
  and work on
  [continuous buildup and drop processes](https://mtosmt.org/issues/mto.21.27.2/mto.21.27.2.smith.html).
  These sources strongly support making rhythm, texture, timbre, and layer
  process first-class rather than decorating generic four-chord loops.
- R&B/Soul and Funk have evidence for both harmony and feel:
  [Soul-dominant corpus behaviour](https://mtosmt.org/issues/mto.26.32.2/mto.26.32.2.fink.html),
  [historical eighth- versus sixteenth-swing tendencies in R&B](https://www.midside.com/presentations/declercq_2024_belmont_text.pdf),
  [neo-soul beat-bin microtiming](https://academic.oup.com/mts/article/45/2/181/7234305),
  and analysis of
  [cyclic dotted-span counter-rhythm in Funk and related groove music](https://mtosmt.org/issues/mto.16.22.2/mto.16.22.2.cohn.php).
- Hip-Hop phrase research explicitly distinguishes beat-loop length from the
  number and direction of harmonies and identifies
  [repetitive, oscillating, and expansional beat types](https://mtosmt.org/issues/mto.20.26.2/mto.20.26.2.adams.html).
  This supports a loop-aware grammar and cautions against evaluating Hip-Hop as
  ordinary functional chord progression.
- Reggae advances provisionally because existing academic/practitioner evidence
  consistently identifies one-drop, rockers, and steppers and the central role
  of bass, while GMD supplies some style-labelled expressive drum material.
  The evidence is not yet broad enough to freeze exact rules; this is a Phase 1B
  research task.
- Bossa Nova advances narrowly because research describes a coherent
  [bass-pulse and syncopated-chord guitar reduction of samba](https://www.scielo.br/j/cint/a/3TTy5RCKtKQDJLghWzb833b/)
  and distinctive
  [speech-like melodic timing](https://www.mtosmt.org/issues/mto.24.30.4/mto.24.30.4.stover.php).
  This is a stronger Jam2 teaching target than a broad, culturally and
  rhythmically incoherent “Latin” label.

### Candidates screened but not advanced as public Phase 1B styles

- **Ambient, Disco, Lo-Fi, and Synth-Pop** currently make more sense as
  profiles or variation/timbre families within Modal Jam, Pop, Electronic,
  Funk, or Hip-Hop than as automatic new public rows.
- Traditional, classic, extreme, Doom, Thrash, Death, Black, Djent, and other
  broad **Metal** families remain outside the current public proposal. Phase 1B
  studies only the narrowly bounded Modern Progressive Metal / Metalcore
  candidate above; it must not silently generalise its findings to the whole
  Metal family.
- **Gospel** is historically and musically important but internally broad.
  Much of the near-term teachable harmony, groove, response, and supporting
  voice work overlaps R&B/Soul, while convincing ensemble/voice-leading
  representation depends on the proposed supporting lines. Re-evaluate it
  after that capability is specified instead of folding it casually into Soul.
- **Ska, Salsa, Afro-Cuban, Afrobeat, Samba, and broad “Latin” or “World”**
  labels are not rejected as music; they are deferred because their bass,
  percussion, interlocking ensemble, articulation, and repertoire boundaries
  require more representation than the initial compact framework can presently
  promise. Roots Reggae and Bossa Nova are narrower, more defensible first
  research candidates.
- **Folk** is too broad to define one grammar. Specific folk practices may later
  justify research, while generic singer-songwriter material is currently
  better represented through Pop, Country, or Rock.

### Phase 1A approved catalog

The approved public research catalog currently contains **thirteen** labels
because that is where the survey landed, not because thirteen is a target:

`Pop`, `Rock`, `Jazz`, `Modal Jam`, `Blues`, `J-Pop / Anisong`, `Country`,
`Electronic`, `R&B / Soul`, `Funk`, `Hip-Hop / Trap`, `Reggae`, and
`Bossa Nova`.

Phase 1B also includes one **public-style feasibility candidate**:
`Modern Progressive Metal / Metalcore`. It is not counted as an approved public
label yet. Phase 1B may promote it at the final checkpoint only if original
musical blueprints and compact synthetic rendering can represent its riff,
timing, tension, articulation, and clean/heavy contrast honestly.

The approved change from the starting set is to remove `Indie` as one public
grammar, route its defensible musical ideas into researched Rock/Pop/Modal
profiles, add Reggae and Bossa Nova, and rename three labels more honestly.
Jazz research covers Swing/Standards, Bebop, and Fusion. The internal profile
list is deliberately provisional. Phase 1B should
merge a profile when its difference reduces to timbre or variation, and should
split one only when evidence reveals a genuinely different relationship among
harmony, melody/riff, bass, groove, form, and supporting layers.

**This checkpoint was approved by the user on 2026-07-29. Phase 1B detailed
research may proceed; generator implementation remains blocked until the final
Phase 1B checkpoint.**

### Phase 1B — Detailed research

After the provisional checkpoint, complete the detailed profile research,
original blueprints, cross-style synthesis, and implementation-fit assessment.
The final Phase 1 checkpoint approves the resulting public catalog, internal
profiles, and detailed implementation specification for the required redesign
capabilities.

Before Phase 2 begins:

- Every retained style brief must be complete enough to support an accurate and
  interesting Jam2 representation.
- Material research conclusions must cite their supporting evidence, with
  important limitations stated where they affect the recommendation.
- The accepted internal profiles and their style-specific variation axes must
  be listed.
- Each profile must have enough original examples to demonstrate its native
  form and the structural strategies that genuinely fit it.
- Conflicts, omissions, inapplicable requirements, and weak evidence must be
  called out rather than papered over.
- The complete findings must be discussed with the user.

## Research method

### 1. Define the repertoire before extracting rules

For each style, state the repertoire period, performance practice, production
family, and contrasting musical practices needed to make the Jam2 label
coherent. Do not collect demographic or geographic metadata unless it is
musically necessary to distinguish the repertoire. Broad catalog terms must not
silently combine incompatible grammars.

For example, research must decide whether Jazz means swing standards,
small-group modal jazz, jazz-funk, or some controlled combination; whether EDM
profiles are House, Techno, Synthwave, or Breakbeat; and whether Hip-Hop / Trap
contains separate loop and drum grammars for Boom-Bap, Lo-Fi, and Trap.

### 2. Evaluate whether Jam2 can represent it honestly

For each starting style and candidate addition, evaluate:

- Whether its recognisable identity can be expressed through the layers Jam2
  can generate now or could reasonably support in its compact framework: tonal
  language, harmony, bass line, melody or riff, supporting line, groove,
  comping, timing, articulation, timbre, form, and controlled variation.
- Whether its important distinctions are musical and generative rather than
  mostly dependent on production, instrumentation, vocal persona, lyrics, or
  sound design that Jam2 does not model.
- Whether the generator can produce more than a narrow stereotype while
  remaining recognisable across seeds.
- Whether low complexity can teach a clear foundation and higher complexity can
  add authentic, explainable vocabulary.
- Whether there is enough reliable evidence to define conservative rules and
  disclose meaningful limitations.
- Whether the public label is understandable, coherent, and useful to someone
  learning how to build a jam in that musical practice.

Retain or add a style when this case is convincing. Merge, reframe, defer, or
remove it when Jam2 could represent only its name or surface sound. There is no
target number on either outcome.

### 3. Triangulate evidence

Use complementary evidence where it is available and relevant. Useful types
include:

- Corpus or dataset statistics.
- Music-theory or performance-practice scholarship.
- Manual analysis/listening of representative recordings or authoritative
  transcriptions.

No source count can guarantee good research. Triangulate material claims when
possible and be explicit when a conclusion rests on a narrower basis.
Commercial charts, prominent-song lists, and canonical recommendations may
help discover repertoire, especially where formal research is limited, but
must not be treated as an unbiased statistical average. Balance them across
relevant periods, production families, and contrasting musical practices as
appropriate.

Do not infer a complete style from a list of common chord progressions. Study
the interaction among musical layers and the location of events within phrases.

### 4. Analyse relevant dimensions consistently across profiles

For each internal profile, investigate the dimensions below that materially
affect its representation. State when a dimension is unknown, unsupported, or
not musically applicable rather than inventing a rule:

- Native tempo range and whether half-time/double-time perception matters.
- Meter, subdivision, swing, microtiming, backbeat placement, and velocity
  tendencies.
- Typical loop, phrase, period, or native-form length.
- Harmonic rhythm and chord-change locations.
- Tonal centres, modes, blues/modal mixtures, chord qualities, inversions,
  pedals, bass movement, and characteristic exceptions.
- Bass-line role, range, rhythm, repetition, root and inversion treatment,
  pedals, approach notes, passing motion, fills, and relationship to kick,
  chords, and phrase boundaries.
- Cadence or boundary strength at the profile's musically significant
  positions, which may include 2-, 4-, 8-, 12-, or 16-bar boundaries.
- How an ending sets up the next loop.
- Melodic range, density, pickup behaviour, rhythmic cells, contour,
  repetition, sequence, target tones, phrase peaks, and rests.
- Drum anchors, kick/snare relationship, cymbal/hat vocabulary, fills, ghost
  notes, limb feasibility, and multi-bar development.
- Chord-comping articulation and its relationship to drums and melody.
- Melodic harmony, countermelody, response, pad, riff, or other supporting-line
  roles where applicable; their range, spacing, motion, density, entrances,
  target tones, and collision-avoidance relationship with the main melody.
- Characteristic sound families and feasible synthesis traits, including
  register, attack and release shape, sustain, brightness, filtering, noise,
  saturation, modulation, stereo or spatial treatment, and effects when they
  are genuinely style-bearing.
- Which timbral distinctions can be approximated honestly with Jam2's compact
  synthesis and which depend on instruments, performers, samples, or production
  practices outside the intended framework.
- Which traits remain essential at low complexity.
- Which techniques are optional at higher complexity and how they resolve.
- Appropriate style-guided variation axes and invalid combinations.

### 5. Produce original native-form and long-form blueprints

For every profile provisionally approved for detailed study, author enough
abstract, original blueprints to show how its native form and compatible
longer-form strategies work. Sixteen bars is the default when the profile has
no stronger native length; use 4-, 8-, 12-, or other justified lengths when
they are a better musical fit. A blueprint contains functional or
tonic-relative harmony where applicable, phrase roles, boundary or cadence
roles, motif transformations, bass and supporting-line roles, groove
development, feasible timbral targets, and the transition from its final bar
back to bar 1. It is not a copied transcription.

Across the retained catalog, the examples should demonstrate:

- Low-complexity material that is simple but musically complete.
- Higher-complexity material whose additional vocabulary is controlled rather
  than merely denser.
- Open loop endings where they are idiomatic, as well as any well-supported
  closed or sectional endings.
- A clear explanation of why its phrase flow is idiomatic.

Do not multiply blueprints to satisfy a count. Add one when it teaches or tests
a genuinely distinct native form, structural strategy, variation path, or
complexity behaviour.

### 6. Convert observations into bounded specifications

For every proposed rule, record:

- Evidence supporting it.
- Styles/profiles to which it applies.
- Whether it is required, preferred, optional, or prohibited.
- Its seed-controlled range.
- Its interaction with mode, complexity, form, and other layers.
- An automated invariant where one is possible.
- What must still be judged through listening.

### 7. Research section continuation and contrast

For each profile where the evidence supports it, document how an established A
section commonly continues as A-prime or contrasts with a B section. Study:

- Which features normally remain stable, such as tonic, tempo, meter, pocket,
  instrumentation role, or motif identity.
- Which features may change, such as harmonic region, harmonic rhythm, cadence
  strength, register, density, subdivision, articulation, melodic range, or
  texture.
- Transition bars, pickups, fills, breakdowns, lifts, and return preparation.
- How much contrast remains recognisably part of the same piece.
- How a contrasting section can return convincingly to A.

This research should produce reusable relationship rules, not require the
initial redesign to implement automatic continuation. A future continuation
system should infer observable musical properties from the supplied section,
show any style/profile inference and its uncertainty, permit an override, and
use the catalog as guidance rather than forcing the material into a named
style.

### 8. Research a shared, style-dependent complexity ladder

Evaluate the current eight-level model rather than assuming it is the best
teaching structure. Propose the number of stages and their order from the
musical tools that produce useful, audible, and explainable differences.

The final ladder should:

- Give each stage a broadly consistent concept across the catalog while letting
  each profile realise that concept in an idiomatic way.
- Begin with the profile's simple, conservative foundation: clear tonal or
  collection membership, simple voicings, an intelligible motif, an idiomatic
  core groove, and a complete native form.
- Test collection-preserving colour such as extensions, inversions,
  suspensions, register changes, and resolved melodic decoration as an early
  expansion.
- Test directed harmonic tension such as secondary dominants, applied chords,
  or a profile-native analogue at a later stage without forcing functional
  harmony into styles that do not use it.
- Investigate where modal interchange, chromatic approach, substitution,
  modulation, rhythmic displacement, denser subdivision, advanced bass motion,
  supporting lines, and timbral development belong.
- Treat every unlocked technique as permitted and seed-weighted, not mandatory
  in every generated idea.
- Keep the style recognisable at every stage. A higher level adds musical tools
  and performance difficulty; it does not replace the profile grammar or merely
  increase note density.
- Define what the teaching popup says was available, what was actually selected
  for this seed, and how the selected tools were applied.

If research recommends replacing the eight-level scale, update the current
research recipe and controls together; no historical generated-recipe mapping
is required.

## Approved profile research briefs

The headings below reflect the Phase 1A checkpoint: thirteen approved public
research labels plus one conditional feasibility candidate. They are still
research hypotheses rather than quotas. Phase 1B must evaluate the usefulness
and coherence of every public label and profile as well as the music associated
with it. Findings may recommend that a label be retained, reframed, merged,
split, replaced, removed, or added. Research must be written here rather than
kept only in chat or temporary notes.

Every full brief must cover, where relevant: repertoire boundary and exclusions;
harmony/tonality; melody or riff language; groove, meter, subdivision, and
microtiming; bass; supporting lines; native phrase and section form; timbre,
synthesis, articulation, and percussion; variation axes; style-dependent
complexity; teaching content; A-to-A-prime/A-to-B implications; implementation
fit and honest limitations; cited evidence; proposed compact profiles; and
original non-song-derived blueprints. A dimension may be marked inapplicable,
but it must not disappear silently.

### Pop

Research Western contemporary, straight, dance/disco-influenced, half-time, and
romantic/anthemic possibilities without treating one four-chord loop as the
style. Establish how melody, production-oriented repetition, harmonic
ambiguity, pre-chorus-like lift, and loop endings create form.

**Findings.** Bound the public style to English-language, Western-chart
contemporary Pop from roughly 1990 onward, while allowing earlier disco, synth,
rock, and R&B influence as groove or arrangement families. Do not claim that
this represents every music marketed as Pop. The McGill Billboard and newer
corpus literature show that loops are common but not one fixed four-chord
schema: recent songs may sustain ambiguous or “hybrid” tonics, and a large
majority of observed loops are song-specific rather than members of one tiny
stock list
([Duinker](https://www.mtosmt.org/issues/mto.19.25.4/mto.19.25.4.duinker.html);
[White and Quinn](https://easychair.org/publications/preprint/vRwsT)).
Verse–prechorus–chorus direction can arise from harmonic rhythm, dominant or
ascending motion, but also from register, melodic ascent, layer addition,
rhythmic activity, and timbral expansion while the same loop continues
([Nobile](https://mtosmt.org/issues/mto.22.28.3/mto.22.28.3.nobile.html)).
The melody must therefore be a vocal-like lead with pickups, repeated hook
rhythms, selective chord-tone targeting, controlled non-chord tones, and
phrase-level contour; it must not arpeggiate every chord. The
[BiMMuDa](https://transactions.ismir.net/articles/10.5334/tismir.168) lead
melodies and section labels are appropriate validation material.

Use a mostly 4/4 straight or lightly swung grid, with 6/8 or 12/8 available only
to a supported ballad family. Backbeat, four-on-floor, syncopated kick, and
half-time are groove families, not separate harmonic styles. Bass normally
reinforces roots and inversions but may add octave, approach, anticipatory, or
hook motion when it does not compete with the lead. Supporting lines should be
sparse hook doubles, response figures, pads, or chordal pulses. Native
generation should favour 8- or 16-bar sections and 16–32-bar A/A-prime,
A/B, or verse/prechorus-like arcs, but permit asymmetric phrase additions and
subtractions. “More complex” Pop should mean stronger voice leading, inversions,
secondary colour, motif transformation, or coordinated section lift—not
continuous chromatic chords or busier notes.

**Proposed profiles and variation axes.**

- `pop_loop` — **Loop-Centred Pop**: one two- to eight-bar harmonic cycle whose
  formal direction comes mainly from melody, bass, groove, texture, register,
  and ending treatment. Seed axes: tonal-centre clarity, loop start phase,
  harmonic rhythm, straight/half-time/four-floor groove, hook syncopation,
  bass independence, layer activation, and open/closed loop ending.
- `pop_sectional` — **Sectional/Lift Pop**: an A section that genuinely prepares
  an A-prime or B through harmonic, melodic, or textural intensification. Seed
  axes: lift mechanism, pre-B length, cadence strength, common-loop versus
  contrasting-loop B, melodic range expansion, supporting-response density,
  and return strategy.

Dance/disco, romantic/anthemic, synth-pop, and acoustic presentation remain
compatible groove/timbre/form families inside these profiles unless later
evidence shows a different compositional grammar.

**Original form blueprints.**

- `pop_loop_16`: four bars `vi–IV–I–V` repeated four times, but with original
  two-bar hook cell `x` transformed as `x, x′, y, x″`; bass changes from roots
  to selected inversions in bars 9–12; the last pass withholds the final melody
  resolution and thins the drums. This tests development over a constant loop,
  not the originality of the common progression.
- `pop_lift_24`: A1 eight bars of `I–V/vi–vi–IV`; an eight-bar lift that shortens
  melodic rests, raises register, and increases harmonic rhythm through
  `ii–IV–V`; B eight bars begins away from tonic with `IV–I6–vi–V` and answers
  A's hook in a wider contour. An alternate seed keeps A's harmony under all
  three sections so the same formal job is achieved entirely by layers,
  register, bass, and melody.

### Indie reassignment conclusion

Phase 1A found that `Indie` is not coherent enough to remain one public
generative grammar. Phase 1B must route supported loose-room, garage, motorik,
pedal/modal, textural, and performance practices into the relevant Rock, Pop,
Modal Jam, or other profiles, and record any practice that cannot be represented
honestly. It does not require a separate full style brief unless new evidence
overturns that conclusion.

Phase 1B confirms that reassignment. Scholarship treats Indie as a motley family
whose boundaries are frequently carried by timbre and production rather than
one harmony/groove/form system
([Blake](https://www.mtosmt.org/issues/mto.12.18.2/mto.12.18.2.blake.html)).
Route garage attack and concise loose performance to `rock_punk_garage`;
motorik or repetitive guitar/bass modules to `rock_riff_modal` or an Electronic
groove family; pedal/modal and atmospheric practices to `modal_atmospheric`;
and loop-centred or intimate songwriting to the appropriate Pop profile.
Jangle, shoegaze wall-of-sound, lo-fi room colour, fragile vocal delivery, and
extreme guitar texture may be timbre variations only when Jam2 can render them
honestly. They are not substitute chord progressions. No separate `indie`
stable ID or blueprint is recommended.

### Rock

Research modal mixture, `bVII`, `bIII`, power-chord practice, plagal motion,
riff-centred harmony, straight and shuffle feels, and the difference between a
riff loop and a functional chord progression.

**Findings.** Bound this label to guitar/bass/drum-centred Rock practice from
the mid-1960s onward, not soft Pop with a guitar patch and not the separately
tested modern-metal production grammar. Corpus research finds ♭VII and IV–I
motion unusually important relative to common-practice syntax, supporting
Aeolian/Mixolydian mixture and plagal closure rather than universal V–I
([de Clercq and Temperley](https://mtosmt.org/issues/mto.11.17.1/mto.11.17.1.temperley.html)).
Power chords are root/fifth sonorities whose major/minor identity can remain in
the melody or surrounding parts. Riff-centred music needs a pitch-and-rhythm
object that may imply several roots or stay over a pedal; converting every riff
attack into a chord change destroys the grammar.

All profiles need a dedicated bass that can double the riff, reinforce roots,
hold a pedal, or connect changes, and a drum layer that can lock kick attacks to
the riff without making every kick identical. Melody should behave like a sung
line—pentatonic/mode-aware, phrase-breathed, and selectively independent of
power-chord quality. Supporting lines are secondary guitar riffs, sustained
tones, response licks, or simple doubles, not automatic four-part harmony.
Straight eighths, swung/shuffled eighths, half-time, and faster eighth-note
drive need separate timing families. Most sections are 4, 8, or 16 bars, but
riff modules may cross bars or form asymmetric 3+3+2, 6-, 10-, or 12-bar
groups. Section contrast may retain a riff while changing pedal, register, or
drum time; change riff; or move from riff to broader chordal chorus.

**Proposed profiles and variation axes.**

- `rock_riff_modal` — **Riff/Modal Rock**: pedals, power chords, Mixolydian,
  Aeolian, minor pentatonic, ♭VII/♭III/IV roots, plagal or open endings. Axes:
  riff-versus-chord dominance, pedal strength, modal collection, syncopation,
  riff/chord call-response, bass doubling/independence, straight/half-time
  drums, and open/plagal closure.
- `rock_shuffle_blues` — **Shuffle/Blues Rock**: swung or triplet subdivision,
  dominant/power mixtures, boogie bass/guitar cells, I/IV/V or riff-based blues
  syntax, vocal-like pentatonic lead. Axes: shuffle ratio, straight-versus-swing
  interpolation, boogie activity, quick change, turnaround type, riff unison,
  and dynamic build. Native Blues forms route through the shared Blues form
  catalog rather than being copied here.
- `rock_punk_garage` — **Punk/Garage**: fast straight subdivision, emphatic
  power-chord or compact riff motion, short phrases, backbeat drive, limited
  fills, narrow hook vocabulary, and concise repetition. These are documented
  compositional and performance conventions, not merely distortion
  ([Pearson](https://www.mtosmt.org/issues/mto.19.25.1/mto.19.25.1.pearson.php)).
  Axes: tempo band, downstroke/eighth-note continuity, chord-versus-riff unit,
  stop/start breaks, chant-like versus wider melody, phrase compression, and
  garage looseness.

**Original form blueprints.**

- `rock_pedal_10`: a two-bar low-tonic rhythmic riff in Mixolydian is grouped
  `2+2+3+3`; the latter modules answer with ♭VII and IV power roots while the
  bass alternates unison and pedal. A-prime preserves the attack pattern but
  shifts two accents across the bar and opens the melody to scale degree 6.
- `rock_shuffle_12`: a native twelve-bar quick-change frame with a two-beat
  boogie cell, sparse vocal-like call in bars 1–2 and instrumental answer in
  3–4, IV-register lift in 5–6, and a two-bar V–IV turnaround that can either
  loop or close. It shares tonal validation with Blues but uses heavier riff
  doubling and Rock drum/timbre rules.
- `punk_14`: `4+4+2+4` bars: two iterations of an original three-power-root
  verse cell, a two-bar all-lane stop/re-entry, and a four-bar broader-register
  refrain. Complexity adds anticipations, an inner response, and phrase
  elision; it never turns into chromatic Jazz harmony.

### Jazz

Research three deliberately broad but distinct territories: Swing/Standards,
Bebop, and Fusion. Study phrase length, ii–V motion, turnarounds, guide tones,
comping rhythm, ride language, motif development, harmonic rhythm, bass roles,
and the limits of generating convincing jazz with Jam2's compact
representation. Do not flatten Fusion into swing with electric timbres or
Bebop into greater note density.

**Findings.** Bound Swing/Standards to small-combo treatment of Great American
Songbook and swing-era/hard-bop song forms; Bebop to 1940s–60s small-combo head
and improvisational language over faster functional changes; and Fusion to
selected late-1960s onward electric, straight-eighth/sixteenth, modal, Rock- and
Funk-interacting practice. This deliberately excludes free Jazz, big-band
arranging, New Orleans/trad, Latin Jazz as a whole, and the claim that one
generated “solo” reproduces real ensemble improvisation.

The [Jazz Structure Dataset](https://transactions.ismir.net/articles/10.5334/tismir.131)
supports explicit head, solo-chorus, and head-return roles; the
[Jazzomat glossary](https://jazzomat.hfm-weimar.de/dbformat/glossary.html)
separates composition type from feel and describes swing as ride pattern,
hi-hat 2/4, walking bass, variable uneven eighths, and irregular comping/drum
accents. It also treats ballad as slow two-beat/swing rather than an independent
rhythm style. Jam2 should generate an original head-like instrumental melody,
not a wall of pseudo-improvisation: clear motifs, guide-tone targeting, rests,
pickup/extension phrases, and idiomatic chromatic approaches unlocked at later
stages. Swing ratio must vary with tempo and lane; Jazzomat's timing tools
explicitly model swing ratio, loudness shape, and variance
([example](https://jazzomat.hfm-weimar.de/tutorials/benjamin/tutorial_3_rhythm.html)).

Harmony requires seventh chords as normal units, functional ii–V and
turnaround families where profile-appropriate, applied dominants,
tonicizations, substitutions, and voice-led extensions as optional later
tools—not a random chord-quality suffixer. Comping must be a rhythmically sparse
support lane with shell/guide-tone and rootless options. Bass is structural:
two-feel, walking quarter line, pedal/ostinato, or syncopated electric riff.
The [FiloBass study](https://arxiv.org/abs/2311.02023) reinforces the need to
model jazz bass against beats, downbeats, chord symbols, and form rather than
derive it from block-chord roots. Drums require ride/hat, comping accents,
brushes where feasible, and section/fill awareness; random “humanize” is not
swing.

Fusion is compositionally distinct. Published Scofield research documents
single-chord or pendular fusion segments, modal/functional juxtaposition,
upper structures, third-related key movement, riff themes, and contrasts
between lyrical long phrases and fragmented pentatonic/fourth-based themes
([International Society for Jazz Research](https://jazzresearch.org/en/jazz-research/jazzforschung-43-2011/)).
It therefore needs electric bass/riff interlock, straight or flexibly inflected
eighth/sixteenth groove, synth/electric-piano/guitar timbres, and optional mixed
or asymmetric meter; it is not Bebop harmony over Rock drums.

**Proposed profiles and variation axes.**

- `jazz_swing_standards` — **Swing/Standards**: songbook-like AABA/ABAC and
  original 12/16/32-bar chorus forms; medium swing, two-feel, and ballad
  families. Axes: two-feel/walk, tempo-dependent swing, shell/rootless comping,
  harmonic rhythm, turnaround strength, head density, guide-tone emphasis,
  brush/stick palette, and final-tag choice.
- `jazz_bebop` — **Bebop**: faster functional movement, chain ii–V or cycle
  motion, angular “worm”-like heads, chromatic approaches/enclosures, walking
  bass, sparse interactive comping, and chorus-based form. Axes: harmonic
  rhythm, approach-note class, phrase elision, syncopation, substitution depth,
  head/unison density, break placement, and tempo. Complexity must preserve
  melodic direction rather than reward maximum notes.
- `jazz_fusion` — **Fusion**: electric straight-eighth/sixteenth or mixed-meter
  groove, modal/pedal and functional contrast, bass or unison riffs, lyrical or
  fragmented themes, and timbral development. Axes: vamp-versus-changes
  balance, Funk/Rock weighting, meter/grouping, bass independence, unison-riff
  amount, upper-structure colour, synth/electric-guitar support, and open-ended
  versus returning form.

**Original form blueprints.**

- `swing_abac_32`: four original eight-bar phrases. A establishes a tonic
  guide-tone motif over `Imaj7–vi7–ii7–V7`; B sequences its rhythm through a
  secondary ii–V; A-prime reharmonizes one sustained melody note; C delays the
  last resolution with a two-bar turnaround. Two-feel may become walking at B.
- `bebop_20`: a four-bar angular head cell, its five-bar sequential answer,
  four-bar contrasting bridge through two tonicizations, and seven-bar return
  with a two-bar drum break/tag. The asymmetric `4+5+4+7` design tests phrase
  awareness; chromatic approaches remain attached to target notes and changes.
- `fusion_14`: a seven-beat bass/riff cycle heard as `3+2+2`, four repetitions
  grouped into A, then six bars of straight 4/4 lyrical harmony, then a
  compressed two-cycle return. The B melody expands A's fourth-based cell; it
  does not paste an unrelated solo over the vamp.

### Modal Jam

Research Dorian, Mixolydian, Lydian, Phrygian, and Aeolian vamps separately.
Form must come from pedal movement, register, motif, rhythm, texture, and
controlled harmonic colour rather than false functional cadences.

**Findings.** This is an explicitly pedagogical Jam2 category, not a claim that
all modal music is one historical style. A modal centre should be established
by tonic pedal/ostinato, recurrence and metrical position; colour comes from the
mode's characteristic degree, slow harmonic rhythm, suspended or quartal
voicings, and melody. Reviews of modal-jazz scholarship consistently identify
slow harmonic rhythm, pedal harmony, limited functional progression, sus/slash
sonorities, and prominent fourths
([Stover](https://mtosmt.org/issues/mto.12.18.1/mto.12.18.1.stover.html)).
The modal tonic in the bass can let diatonic quartal structures move above it
without implying ordinary root progressions
([Berklee/Pease](https://online.berklee.edu/takenote/harmonic-considerations-modal-harmony/)).

Each collection needs its own eligibility rules: Dorian foregrounds natural 6
against minor 3; Mixolydian foregrounds ♭7 against major 3; Lydian foregrounds
♯4 while avoiding repeated cadential IV-to-I reinterpretation; Phrygian
foregrounds ♭2 and must control register/semitone density; Aeolian foregrounds
♭6/♭7 without importing a raised-leading-tone dominant unless a deliberate
tonal-mixture stage is selected. The melody should revisit the tonic and
characteristic degree through an evolving motif, not run the scale. One or two
colour chords may shift above a continuing pedal; root movement that creates
V–I or relative-major closure is constrained.

Groove Vamp may use straight eighth/sixteenth, swung, Funk-, Rock-, or
electronic-compatible rhythm families but owns its modal pitch/bass rules.
Atmospheric Pedal uses longer attacks, space, overlapping support, slow contour,
and timbral evolution. Bass is a pedal, ostinato, or limited modal cell;
supporting material is quartal/sus comping, drone, countermelody fragment, or
pad. Form comes from motif mutation, register, density, pedal displacement,
rhythmic phase, and texture. Native lengths may be a short 2–8-bar jam cycle or
an evolving 12–32-bar arc with very few chord changes.

**Proposed profiles and variation axes.**

- `modal_groove` — **Groove Vamp**: Dorian and Mixolydian primary; Aeolian and
  Phrygian when their characteristic pitch and register rules are active. Axes:
  mode, pedal/ostinato, one- versus two-centre colour, groove family,
  characteristic-degree emphasis, bass-cell syncopation, motif phase,
  comping/riff response, and open/return ending.
- `modal_atmospheric` — **Atmospheric Pedal**: Lydian, Dorian, Aeolian, and
  selected Phrygian. Axes: mode, drone register, harmonic-colour rate, pad
  spacing, pulse clarity, melodic range, countermelody amount, timbral motion,
  and degree of unresolved ending.

**Original form blueprints.**

- `modal_dorian_16`: D pedal throughout four four-bar phrases; an original
  two-note pickup motif first reaches F, then exposes B natural in phrase two;
  phrase three shifts the upper voicing from `Dm11` colour to `G/D` without
  treating G as a dominant; phrase four removes attacks and returns the motif
  one octave lower. Bass adds only a late `D–A–C–D` response.
- `modal_lydian_15`: `5+5+5` bars over an F pedal. A sparse motif first withholds
  B natural, A-prime makes it the sustained high point against `G/F`, and B
  moves the pedal briefly to E before returning without a V–I cadence. Density,
  register, and pad spectrum provide the arc.

### Blues

Respect native 8-, 12-, and 16-bar forms, slow/quick change, minor blues,
shuffle and straight feels, turnaround placement, blues pitch collections, and
the intentional major/minor mixture of melody and dominant-seventh harmony.

**Findings.** Bound the style to accompaniment and original instrumental
call-and-response derived from twentieth-century African American Blues
practice, with electric/urban, shuffle, straight, slow, and selected
Jazz-influenced variants. Do not treat “sad minor Pop,” Blues Rock timbre, and
all Gospel/Jazz syntax as Blues. Twelve bars are a native schema but not a
mandatory global length: eight-, twelve-, and sixteen-bar strophes and
contrasting sections all occur. Corpus work documents a wider family of
strophic and contrasting forms
([Carter](https://mtosmt.org/issues/mto.25.31.1/mto.25.31.1.carter.php)),
while twelve-bar practice coordinates vocal and instrumental phrase placement
rather than filling every bar uniformly
([Stoia](https://www.mtosmt.org/issues/mto.20.26.4/mto.20.26.4.stoia.html)).

Major/dominant Blues intentionally permits dominant-seventh I, IV, and V
sonorities without treating I7 as an applied dominant. Melody may combine minor
and major pentatonic resources, ♭3/3 inflection, ♭5, and ♭7 against those chords;
the resulting friction is stylistic evidence, not a validation error
([Stoia on mode and dissonance](https://mtosmt.org/issues/mto.10.16.3/mto.10.16.3.stoia.php)).
Minor Blues instead anchors minor tonic/iv, permits selected dominant V or
modal v, and uses different chord-tone and cadence weighting. Melody follows a
vocal-like call with rests and an instrumental answer; bends, scoops, grace
approaches, and variable intonation should eventually be represented as
articulation metadata, while the initial renderer may approximate them
conservatively.

Groove families are triplet shuffle, slow 12/8, straight eighth, and selected
Funk/Latin-influenced straight variants only when supported. Bass roles include
root/fifth pulse, shuffle/boogie cell, walking connection, and sparse sustained
support. Guitar/keyboard comping may use short chord attacks, boogie dyads, or
responses. The last two bars need explicit turnaround policies: V–IV, ii–V,
chromatic/diatonic bass turnaround, stop-time, tonic close, or open dominant;
the generator must not force tonic at every final bar. Complexity can add
quick-change, substitutions, chromatic approach, richer turnaround, and
cross-chorus motif development without erasing the form.

**Proposed profiles and variation axes.**

- `blues_dominant` — **Major/Dominant Blues**: native 8/12/16-bar schemas;
  slow/quick change; straight/shuffle/12-8; dominant/power/boogie accompaniment;
  mixed major/minor melody. Axes: form schema, IV timing, feel, stop-time,
  comping cell, bass role, call/answer spacing, blue-note intensity, turnaround,
  and loop/close ending.
- `blues_minor` — **Minor Blues**: minor tonic/iv, selected V7 or modal v,
  minor-pentatonic and colour-note melody, slower or straight/shuffle grooves,
  and more sustained harmony. Axes: form length, functional-versus-modal
  dominant, chromatic descent, bass sparsity, response density, turnaround, and
  degree of Jazz extension.

**Original form blueprints.**

- `blues_dom_12`: quick-change bars `I7 | IV7 | I7 | I7 | IV7 | IV7 | I7 |
  I7 | V7 | IV7 | I7 | V7`; an original two-bar call returns rhythmically in
  bars 5–6 at a new pitch level, with instrumental answers in the intervening
  space. Seeded alternatives replace bar 12 with tonic close or stop-time and
  alter only eligible turnaround bars.
- `blues_minor_16`: `4+4+4+4` bars: minor-tonic call, iv answer, return with a
  descending chromatic bass colour, and V7-to-i or modal-v open turnaround.
  Melody first withholds scale degree 2, then uses it as a phrase extension;
  complexity adds voice-led `iiø7–V7` only in the functional variant.

### J-Pop / Anisong

Define the intended repertoire carefully. Research longer harmonic journeys,
royal-road-related motion, secondary dominants, bass lines, phrase lift,
high-energy pop-rock/dance grooves, melodic sequencing, pickups, and cadence
avoidance without reducing the style to `IV–V–iii–vi`.

**Findings.** Bound this Jam2 label to mainstream Japanese idol/dance Pop and
television-anime opening/ending Pop-Rock from roughly the 1990s onward. Exclude
city pop as a whole, Vocaloid as a production/vocal system, game music,
traditional Japanese music, and the implication that all Japanese Pop shares
one grammar. The “royal road” family is genuinely useful but must be treated as
a family of partial-tonic/circle-related motions, not a preset synonymous with
the style
([Ramage](https://academic.oup.com/mts/article-abstract/45/2/238/7224663)).

The direct 100-song anisong study finds that chromatic mediants and modulations
are strongly associated with verse–prechorus–chorus boundaries; local
secondary dominants also intensify diatonic targets
([Li](https://mtosmt.org/issues/mto.26.32.1/mto.26.32.1.li.php)).
This supports a sectional planner with longer harmonic routes, phrase-end
tonicization, chromatic-mediant section shifts, and return-key planning. These
are seed-weighted options, never a requirement to modulate. Harmony may use
diatonic seventh chords, `IV–V–iii–vi` relatives, circle sequences, inversions,
applied dominants, modal interchange, and selected common-tone/mediant section
links. Every chromatic chord must have a local voice-leading and formal role.

Melody is the instrumental analogue of the sung lead: energetic pickups,
eighth/sixteenth rhythmic cells, sequence, repeated-note hooks, wide but
singable peaks, phrase extension across chord changes, and section-specific
range. It must not merely outline the busy harmony. The professionally authored
[IdolSongsJp](https://huggingface.co/datasets/imprt/idol-songs-jp/blob/main/README.md)
corpus is unusually relevant because it separates lead, harmony, and call
parts. Jam2 may use its published aggregate evidence and lawful listening
comparison, but must not train on or reproduce the licensed songs. Idol/Dance
therefore needs optional parallel/contrary harmony and short call responses;
Anisong Pop-Rock needs guitar/keyboard riffs and counterlines that clear the
lead's register.

Both profiles primarily use explicit 4/4 with straight subdivisions, but may
use supported 6/8, 12/8, local meter changes, anticipatory chord changes, or
asymmetric phrase extensions when reference evidence supports the generated
family. Bass is active: roots/inversions, diatonic runs, anticipations, and
section-lift motion coordinated with kick. Timbre is bright, layered, and
articulation-rich—drums, electric bass, piano/synth, guitar, pads, and lead
colours—but profile identity must survive a neutral patch test.

**Proposed profiles and variation axes.**

- `jpop_anisong_rock` — **Anisong Pop-Rock**: high-energy sectional arc,
  Pop/Rock rhythm section, longer functional/chromatic harmony, melodic peaks,
  riffs/counterlines, and optional boundary modulation. Axes: harmonic-route
  length, secondary-dominant density, modal mixture, section-key relation,
  guitar/synth balance, bass motion, pickup/extension amount, melodic range,
  and A-to-B lift mechanism.
- `jpop_idol_dance` — **Idol/Dance J-Pop**: dance/Pop groove, lead/harmony/call
  arrangement, bright layered hooks, sectional contrast, and chromatic colour
  at phrase or section boundaries. Axes: four-floor/backbeat blend, call
  density, harmony-voice interval/contrary motion, synth/guitar balance,
  harmonic rhythm, sequence length, bass anticipation, and final lift/tag.

**Original form blueprints.**

- `anisong_28`: A eight bars begins `I–V/vi–vi–IV` with a two-bar pickup motif;
  an eight-bar lift uses `ii–V/iii–iii–vi` and sequentially raises the motif; B
  twelve bars begins in a common-tone ♭III-related key, establishes it rather
  than jumping randomly, then returns through a pivot in the last two bars.
  An alternate seed stays in one key and creates the same lift through bass,
  register, harmonic rhythm, and drums.
- `idol_24`: three eight-bar groups. Lead hook `x` receives a one-bar call only
  in its rests; A-prime adds a diatonic third/sixth harmony on the cadence; B
  converts the hook rhythm into a supporting synth figure while the lead sings
  a wider answer over a circle-derived progression. The final two bars may tag
  or point forward rather than close.

### Country

Research traditional boom-chick/two-step, train beat, shuffle, and country-rock
profiles; I/IV/V syntax; secondary dominants; bass alternation; vocal-like
melodic phrasing; pickups; and turnaround practice.

**Findings.** Bound Traditional to honky-tonk, two-step, truck-driving/train,
shuffle, and waltz-derived small-band Country from roughly the 1940s–70s, and
Contemporary to selected 1990s onward Nashville Country-Pop/Rock. Exclude
Bluegrass ensemble simulation, Western Swing's full Jazz grammar, Americana and
Folk as wholes, and production-only “country” signifiers. A 200-song corpus
places Country harmony between common-practice and Rock, with unusually strong
I/IV/V emphasis and avoidance of minor tonic
([de Clercq](https://academic.oup.com/edited-volume/41992/chapter-abstract/371464385)).
That justifies a major-key default, but not a three-chord hard gate:
secondary/applied dominants, `ii`, `vi`, inversions, bass-line harmonization,
and modal/Blues colour enter where the profile and level support them.

Traditional melody should sound sung: major pentatonic and chord-tone anchors,
Mixolydian/Blues inflection, pickups, repeated notes, thirds/sixths, compact
range, phrase-end falls, and room for instrumental fills. Country-guitar
pedagogy explicitly joins shuffle, swing, honky-tonk and boogie grooves with
pentatonic/hybrid scales, intros, turnarounds, endings, bends and ensemble
playing
([Berklee](https://online.berklee.edu/courses/country-guitar)).
Jam2 should encode bend/slide targets and duration even if its first synthetic
render is a restrained approximation. Pedal-steel-like support is a held or
moving harmony voice with contrary/oblique bend intent; it must not be a generic
pad. Short guitar/fiddle-like response phrases occupy vocal rests.

Bass is mandatory: alternating root/fifth for boom-chick/two-step, walk-up/down
to structural roots, train-beat pulse, waltz root plus upper support, or
Pop/Rock root/octave syncopation. Percussive Arts Society teaching identifies
train beat and slow country waltz as distinct classic drum-set grooves
([PAS](https://pas.org/pas-blog/country-music-grooves-variations-on-the-classics/)).
The meter model must therefore support 2/4 or 4/4 two-beat grouping, 3/4 waltz,
and selected 6/8/12/8 rather than relabelling every pattern 4/4. Contemporary
Country may use Pop loops and Rock backbeats, but must retain vocal phrasing,
country bass/guitar response, and at least one non-patch grammatical marker.

**Proposed profiles and variation axes.**

- `country_honky_tonk` — **Traditional Honky-Tonk/Two-Step**: I/IV/V-directed
  harmony, alternating bass, boom-chick, train, shuffle, and waltz families;
  pickup melody and response fills. Axes: meter/groove, slow/fast two-feel,
  secondary-dominant amount, bass alternation/walk, turnaround, response
  instrument, bend/double-stop amount, stop-time, and tag ending.
- `country_contemporary` — **Contemporary Country-Pop/Rock**: Pop-like loops or
  sectional harmony, Rock backbeat, vocal hook, electric/acoustic response,
  root/octave or connecting bass, and restrained country articulation. Axes:
  Pop-loop versus I/IV/V direction, straight/half-time/6-8 feel, guitar drive,
  bass syncopation, prechorus lift, vocal-range expansion, harmony support, and
  country-marker strength.

**Original form blueprints.**

- `country_two_step_16`: four four-bar phrases over `I | I | IV | I`,
  `I | V | I | V`, then a varied return. Alternating root/fifth bass and
  boom-chick establish two-beat grouping; a two-beat pickup call receives
  short bent/double-stop answers; the final two bars choose V turnaround,
  walk-down tag, or tonic stop.
- `country_waltz_12`: four three-bar vocal phrases in explicit 3/4, each with
  bass on beat 1 and light upper chord attacks on 2/3. The third phrase
  tonicizes IV with `V/IV`; the support line approaches a sustained third by
  oblique bend while the lead rests.
- `country_modern_24`: eight-bar loop-centred A, four-bar lift with faster bass
  approach motion, eight-bar broader chorus, four-bar turnaround. A and chorus
  share a hook rhythm but differ in contour and guitar response; a neutral
  timbre render must still distinguish it from generic Pop through phrase,
  bass, and fill placement.

### Electronic

Treat House, Techno, Synthwave, and Breakbeat as potentially different internal
profiles. Determine how harmonic stasis, bass pattern, layer accumulation,
filter/energy contour, kick architecture, and drop/build implication create
medium- and long-range form without forcing tonal cadences.

**Findings.** Use `Electronic` as a public navigation label for three explicitly
defined club/beat grammars, not as a claim that all electronic music is one
genre. House, Techno, and Breakbeat survive the distinct-grammar test.
Synthwave does not yet: its retro synth palette, arpeggios, gated drums, and
cinematic/Pop form can be expressed as timbre, bass, and arrangement families
inside Pop, Rock, Modal Jam, House, or Techno. Keep a `synthwave` timbre/form
tag, but do not create a profile whose rules are only “use an analogue patch.”

EDM analysis shows meter emerging from interacting layers, including patterns
that can first sound metrical and later be reinterpreted
([Butler](https://mtosmt.org/issues/mto.01.7.6/mto.01.7.6.butler.html)).
Recent narrative-grammar work describes a stable “core,” DJ-compatible entry
and exit, and break routines that destabilize and rebuild that core
([Grosz et al.](https://journals.sagepub.com/doi/10.1177/10298649251321709)).
Buildup/drop research identifies continuous changes—layer addition, filter
sweep, crescendo, rising gesture—and breakdowns that can foreground a new
melody which later becomes a countermelody
([Smith](https://mtosmt.org/issues/mto.21.27.2/mto.21.27.2.smith.html);
[Snyder](https://www.cambridge.org/core/journals/twentieth-century-music/article/breakdowns-and-the-aesthetic-of-disorientation-in-festivalhouse-music/A0748DBC5425594BE0F95D9AE0676DDC)).
Jam2 therefore needs event-level layer activation and continuous parameter
curves; a static 16-bar render with a final fill is inadequate.

House uses explicit 4/4 four-floor kick, offbeat/open hats, 2/4 clap/snare,
syncopated bass, sparse chord/stab or vocal-like hook, and roughly 120–130 BPM
as a conservative centre
([house overview](https://en.wikipedia.org/wiki/House_music)). Techno also
often uses four-floor 4/4 but makes rhythm, synthetic timbre, repetition,
spectral motion, percussion transformation, and phase the primary form-bearing
materials; harmony can be absent, one-centre, modal, or a short stab cycle.
Breakbeat uses syncopated non-four-floor kick/snare architecture and prominent
bass; it must generate original kit patterns and synthetic sounds rather than
copy or ship recorded breaks. All three need step-precise timing plus
profile-specific swing/microtiming, sidechain-like amplitude interaction,
filter/envelope automation, and independent kick/sub/bass management.

**Proposed profiles and variation axes.**

- `electronic_house` — **House**: four-floor core, offbeat hats, clap/snare,
  syncopated bass, chord/vocal-like hooks, and additive break/build/drop form.
  Axes: bass syncopation, chord-stab rhythm, swing, kick continuity, hat/opening,
  harmonic stasis, layer-entry cadence, filter/energy curve, break depth, and
  hook/counterhook.
- `electronic_techno` — **Techno**: four-floor or closely related pulse,
  limited pitch, percussion/riff cycles, timbral process, metric layering, and
  accumulative form. Axes: tonal-centre clarity, ostinato length/phase,
  polymetric layer, kick density, percussion mutation, spectral brightness,
  resonance/drive, automation shape, subtraction, and re-entry.
- `electronic_breakbeat` — **Breakbeat**: original syncopated kick/snare cycle,
  straight or swung subdivisions, bass-led low end, chopped-pattern implication
  without samples, and phrase-level edits. Axes: break length, kick/snare
  syncopation, ghost-note density, swing, bass lock/answer, fill/stutter,
  half/double-time hearing, pad/stab amount, and cut/re-entry structure.

**Original form blueprints.**

- `house_arc_32`: eight-bar core introduction in which kick, hat, bass, then
  stab enter; eight-bar A with a two-bar vocal-like hook; eight-bar break/build
  that removes kick and turns the hook into a support line while filter,
  subdivision, and register rise; eight-bar return with one bass variation and
  a clean DJ-compatible exit option.
- `techno_process_24`: a three-beat percussion ostinato phases against 4/4
  across six four-bar groups. Pitch remains one pedal plus one neighbour;
  formal change comes from phase audibility, resonance, noise bandwidth,
  subtraction, and re-entry. The final group either realigns or deliberately
  remains open.
- `breakbeat_20`: original two-bar kick/snare cycle grouped `4+4+4+4+4`; each
  group changes only one role—ghost layer, bass answer, hat subdivision, full
  cut, then recombination—so variation is audible and attributable rather than
  random event churn.

### R&B / Soul

Research Soul, Motown, Neo-Soul, ballad, and funk-soul distinctions; extended
chords; voice leading; backdoor/plagal motion; pocket; laid-back timing;
melodic anticipation; melisma-compatible space; and phrase-end behaviour.

**Findings.** Keep two historical/production territories rather than presenting
R&B/Soul as timelessly uniform. Classic/Motown Soul covers selected 1960s–70s
vocal-group and rhythm-section practice; Contemporary R&B/Neo-Soul covers
selected 1990s onward slow-to-midtempo, Hip-Hop/Jazz/Gospel-influenced practice.
Exclude Gospel as a whole, Disco/Funk where their independent groove grammar is
primary, and any attempt to synthesize a literal singer.

Classic Soul identity is ensemble counterpoint: strong backbeat/tambourine,
melodic and syncopated electric bass, short guitar/keyboard roles, lead
phrasing, and call-and-response. Jamerson research describes bass that moves
melodically while remaining locked to the drums
([EBSCO overview](https://www.ebsco.com/research-starters/history/james-jamerson)),
and Motown summaries consistently identify tambourine backbeat, prominent
melodic bass, distinctive chord/melodic structures, and vocal response
([Motown overview](https://en.wikipedia.org/wiki/Motown)). “Soul dominant”
eleventh-type sonorities have context-sensitive conventional versus non-
resolving behaviour; bass motion and metrical strength affect interpretation
([Fink](https://mtosmt.org/issues/mto.26.32.2/mto.26.32.2.fink.html)).
Jam2 must tag these sonorities rather than misclassify every one as V.

Contemporary R&B/Neo-Soul favours spacious groove, seventh/ninth/eleventh and
sus/add sonorities, inversions/slash bass, chromatic or diatonic planing,
backdoor/plagal motion, and smooth common-tone/stepwise voice leading. Extended
voicing is not complexity by itself; the bass and upper voices define the
actual sonority. Research on reverse extensions shows bass notes below a fixed
upper harmony can create related chord families
([Brøvig-Hanssen](https://journals.library.columbia.edu/index.php/currentmusicology/article/view/10139)).
Timing is lane-specific: historical R&B moved from more eighth-note toward
sixteenth-note swing
([de Clercq](https://www.midside.com/presentations/declercq_2024_belmont_text.pdf)),
and beat-bin research shows repeated onset pairs can occupy deliberately
stretched positions rather than one global swing value
([Danielsen](https://academic.oup.com/mts/article/45/2/181/7234305)).

Lead melody must leave breath and melisma-compatible duration, use anticipation
and delayed resolution, and preserve a memorable phrase. Supporting roles are
essential: diatonic/extended vocal-like harmony in selected phrases, short
calls, horn/keyboard response, pad, and inner voice. Bass ranges from hook-like
Motown motion to sparse roots, inversions, approaches, and behind/ahead attacks.
Timbres should include electric bass, compact electric piano/organ, muted or
clean guitar, warm pad/strings, horn-like stabs, tambourine/clap, and dry/roomy
drums, with honest synthetic labels.

**Proposed profiles and variation axes.**

- `soul_classic_motown` — **Classic/Motown Soul**: directed major/minor
  harmony, soul-dominant colour, melodic bass, strong backbeat/tambourine,
  chordal interlock, vocal-like lead, call/response. Axes: bass activity,
  tambourine/backbeat layer, straight/eighth-swing feel, call density,
  soul-dominant resolution, horn/support activation, stop-time, and sectional
  lift.
- `rnb_contemporary_neosoul` — **Contemporary R&B/Neo-Soul**: sparse
  sixteenth-pocket, extended voice-led harmony, independent slash bass,
  anticipated/delayed melody, vocal harmony/calls, and timbral space. Axes:
  sixteenth swing, per-lane offset, harmonic rhythm, extension/planing amount,
  bass inversion/approach, lead density, harmony/call amount, pad/keys spacing,
  and loop versus sectional form.

**Original form blueprints.**

- `soul_16`: four four-bar phrases over an original I/vi/IV/soul-dominant
  route. Bass begins root-led, answers the lead in phrase two, and connects IV
  back to I in phrase four; tambourine enters at A-prime; a two-note horn answer
  occupies only the lead's rest. The dominant may resolve or remain plagal
  according to its tagged family.
- `neosoul_12`: three four-bar groups over slowly moving upper voicings while
  the bass traces `1–7–6–♭6`; one melody pickup is delayed differently at each
  return. A-prime replaces one block attack with contrary inner-voice motion;
  B removes bass for two beats then re-enters on an inversion. Per-lane timing
  remains a named recipe parameter, not random jitter.

### Funk

Prioritise the one, static dominant/minor vamps, bass/drum interlock, clipped
comping, syncopated cells, linear/disco/half-time possibilities, and form
through orchestration and rhythmic mutation rather than unnecessary harmony.

**Findings.** One compact profile is sufficient: late-1960s–70s
James-Brown/P-Funk-derived static-vamp and pocket practice, with later
straight-sixteenth influence available as variation. Exclude Disco when
four-floor/string/Pop form is primary, Jazz-Funk/Fusion when improvisational
harmony/form is primary, and “Funky” as a generic syncopation adjective.
Identity arises from a cyclic ensemble of differentiated parts: emphasized
downbeat “one,” bass, drum, clipped guitar/keys, and riff/horn responses.
Research describes anacrustic bass/drum events whose tiny placement changes
alter the projected groove
([Butterfield](https://mtosmt.org/issues/mto.06.12.4/mto.06.12.4.butterfield.php)),
cyclic counter-rhythms that resist a simplistic on/off syncopation measure
([Cohn](https://mtosmt.org/issues/mto.16.22.2/mto.16.22.2.cohn.php)), and
repeatable lane-specific microdeviations in early Funk
([Taylor](https://www.gmth.de/zeitschrift/artikel/1224.aspx)).

Harmony should usually be one dominant-seventh/ninth, minor-seventh, or
minor-pentatonic centre, with one contrast chord or chromatic approach only
where it serves the cycle. Complexity does not earn extra changes. Bass owns a
syncopated motif with root/downbeat return, rests, octave/fifth, chromatic
approach, ghost/dead-note intent, and controlled fills. Kick converses with bass;
snare/backbeat, hi-hat sixteenths, ghost notes, and pickup fills have independent
velocity/timing. Guitar/keys use short muted attacks in complementary spaces.
The instrumental lead is a short riff, chant-like/vocal call analogue, or
horn-like response rather than a continuous melody over every subdivision.

Native form is a two- or four-bar groove developed across 8–32 bars through
activation, subtraction, break, register, call, timbral mutation, and one
disciplined variation at a time. Every part must expose its anchor and mutable
events so variation cannot destroy the interlock. Timbre needs dry punchy
drums, round/plucked bass with optional envelope colour, muted clean guitar,
clav/organ-like keys, and short horn-like stabs; microtiming must remain
visible per lane.

**Proposed profile and variation axes.**

- `funk_static_pocket` — **Static-Vamp/Pocket Funk**. Axes: dominant/minor
  centre, one emphasis/rest, bass cell and fill probability, bass–kick lock,
  straight/swung sixteenth feel, lane timing, ghost-note level, comping
  complement, riff/call role, break placement, and optional disco/half-time
  groove family. Disco-funk stays a cross-style variation unless its form and
  layer system later prove independently necessary.

**Original form blueprints.**

- `funk_16`: one original two-bar D7sus/dominant groove repeated as four
  four-bar phrases. Phrase 1 establishes bass, kick, hat, and clipped chord
  cells; phrase 2 adds a two-note horn response; phrase 3 removes the downbeat
  bass once but leaves the drum “one” audible; phrase 4 mutates one bass pickup
  and ends with a full-band break/re-entry. A validation pass rejects any seed
  where all active lanes repeatedly strike the same sixteenth slots.
- `funk_minor_10`: `4+4+2` bars over a minor-seventh centre; a bass cell and
  guitar counter-cell exchange one attack on the second phrase, then a two-bar
  stop-time answer leads back to the cycle. Higher complexity alters timing,
  approach pitch, and orchestration before it considers a second harmony.

### Hip-Hop / Trap

Research Boom-Bap, Lo-Fi, and Trap separately where appropriate. Study
one-/two-harmony loops, sample-like phrase lengths, bass-centred tonality,
half-time grids, hat rolls, kick/808 movement, negative space, motivic
repetition, and phrase changes created by activation or subtraction.

**Findings.** Retain Boom-Bap and Trap as distinct profiles. Treat Lo-Fi as a
production, tempo, harmony, and feel family over Boom-Bap unless the user later
wants its listening-function/aesthetic context as a public category. Academic
work describes Lo-Fi Hip-Hop's looped piano/guitar/synth “sample” and production
identity
([Oxford](https://academic.oup.com/book/58670/chapter/485385463));
production-value scholarship likewise warns that its meaning often lies in the
medium/imperfection rather than a new note grammar
([Harper](https://www.cambridge.org/core/journals/organised-sound/article/lofi-today/73B4DDB240C2B2DC0A0E64249AB44325)).
Jam2 will make original sample-*like* phrases and synthetic degradation; it
will not copy or bundle recordings.

Hip-Hop beat phrases are not ordinary functional progressions. Adams
distinguishes repetitive, oscillating, and expansional harmonic beat types and
shows phrase can arise from harmony, syntax, motive, metre, texture, timbre, and
lyrics
([Adams](https://mtosmt.org/issues/mto.20.26.2/mto.20.26.2.adams.html)).
Because Jam2 has no rapper, Boom-Bap should leave a broad midrange/rhythmic
space and generate a restrained instrumental hook—sample-like chord/melody
fragment, bass motif, or response—not a busy sung melody pretending to be rap.
Kick, snare, hat, bass, and musical loop need independent swing, velocity, and
offset; classic break research confirms that swing/microtiming is embedded in
Hip-Hop's breakbeat canon
([Frane](https://doi.org/10.1525/mp.2017.34.3.291)).
Berklee's production overview supports eighth/sixteenth hats with lazy swing
and foregrounded beatmaking choices
([Souder](https://www.berklee.edu/berklee-now/news/essential-features-of-hip-hop-production-tempo-instrumentation-rhythmic-feel-and-sonic-density)).

Trap needs a separate low-end model: half-time snare perception, sparse
syncopated kick, pitched long-decay 808/sub notes, slides, sixteenth through
thirty-second/triplet hat rolls, and strategic negative space. Triplet flow,
stable bass, and harmony materially affect perceived meter
([Manabe](https://mtosmt.org/issues/mto.19.25.1/mto.19.25.1.manabe.html)).
Research on the TR-808 shows its usable spectral/register range can constrain
the entire song key; “transpose anywhere” is therefore not sonically neutral
([Deruty](https://vbn.aau.dk/en/publications/harmonic-and-transposition-constraints-arising-from-the-use-of-th/)).
The bass lane needs tuned fundamental, pitch glide, retrigger/legato, decay,
distortion, and kick-conflict controls plus register validation.

Both profiles favour 1–4-bar beat cycles and 8/16-bar phrase groups, but allow
odd loop lengths and 12/20/24/32-bar arcs. Form comes from beat cuts, layer
activation, hook mutation, bass/drum replacement, fill, and occasional B-loop,
not mandatory Pop cadences. Complexity should expose phrase expansion,
polyrhythmic swing, selective hat subdivision, 808 approach/slide, or
support-line transformation one concept at a time.

**Proposed profiles and variation axes.**

- `hiphop_boom_bap` — **Boom-Bap**: punchy kick/snare, swung hat or original
  break-like cycle, sample-like harmonic/melodic fragment, independent bass,
  rap-space-aware instrumental hook, and loop edits. Axes: repetitive/
  oscillating/expansional beat type, 1–4/odd loop length, swing by lane,
  kick/snare placement, ghost layer, bass relation, hook density, filter/grit,
  cut/re-entry, and Lo-Fi variation.
- `hiphop_trap` — **Trap**: half-time drum frame, pitched 808/sub, sparse kick,
  fast straight/triplet hat gestures, dark or ambiguous compact musical loop,
  and activation/subtraction form. Axes: perceived tempo, hat base subdivision,
  roll placement/rate, snare/rim family, kick–808 relationship, slide/decay,
  key/register, loop density, lead/bell/pad colour, negative space, and beat
  switch.

**Original form blueprints.**

- `boom_bap_16`: an original three-chord, two-bar electric-piano fragment is
  filtered and rhythmically re-cut into a four-bar loop; four loop passes add
  bass, remove the downbeat kick once, answer the hook, then cut all but drums
  for two beats. Melody/rest validation preserves a notional rap lane even
  though Jam2 renders no voice.
- `trap_12`: three four-bar groups over one minor-centred two-note pad cell. A
  establishes half-time snare and a two-note 808; A-prime adds one triplet hat
  roll and an 808 approach slide; B removes hats for two beats, changes kick
  placement and raises the instrumental hook while keeping the 808 in its
  validated spectral range. More complexity may alter one roll or slide
  grammar, never carpet every bar with 32nd notes.

### Reggae

Research one-drop, rockers, and steppers where the evidence supports distinct
profiles. Study offbeat guitar/keyboard roles, bass-led harmony, drum placement,
syncopation, phrase length, dub-informed space and timbre, melodic restraint,
and section contrast without treating all Jamaican popular music as one
grammar.

**Findings.** Keep one narrowly bounded **Roots Reggae** profile centred on
Jamaican late-1960s–mid-1980s roots-band practice. One-drop, rockers, and
steppers are groove families, not separate compositional profiles. Exclude Ska,
Rocksteady, Dancehall, Dub as a standalone studio genre, and all later Reggae
fusion; allow restrained dub-informed space/echo as an arrangement axis.

One-drop places its defining combined kick/rim/snare weight around beat 3 of a
4/4 bar and leaves beat 1 open; it is not a Rock backbeat on 2 and 4. Steppers
uses four-floor kick and remains a documented bedrock roots groove
([Modern Drummer](https://www.moderndrummer.com/article/february-2019-reggae-101-the-steppers-beat/)).
Sources disagree when they reduce “rockers” to one exact kick pattern; treat it
as the more driving roots family with kick on/around beat 1 and additional
syncopated support while preserving the central rim/snare feel. Freeze several
reference-validated patterns, not one invented universal formula. This
disagreement is recorded as a conservative implementation choice.

The defining ensemble relationship is stronger than any chord list: short
damped guitar/keyboard skank and organ bubble emphasize offbeats while a low,
warm, often melodic bass line occupies strong beats and the spaces left by the
upper parts. The UK Oak teaching sequence independently identifies skank,
one-drop, organ bubble, offbeat emphasis, and prominent bass as the core
interlock
([Oak National Academy](https://www.thenational.academy/teachers/programmes/music-secondary-ks4-eduqas/units/rhythms-and-conventions-of-latin-and-caribbean-music/lessons/the-development-of-reggae)).
Harmony may use simple major/minor diatonic loops, I/IV/V motion, modal
subtonic, and occasional dominant colour; the bass may imply inversions and is
not restricted to roots. Melody is vocal-like, spacious, pentatonic/diatonic,
pickup-aware, and supports call/response or short horn/organ figures.

Explicit 4/4 is primary, with straight-to-lightly-lilted eighth/sixteenth
subdivision controlled per lane. Form favours two- or four-bar riddim cycles
developed across 8–32 bars through bass variation, skank/bubble alternation,
percussion, fills, echo throws, mutes, and dropouts. Dub space means timed echo,
filtering, and subtraction around an intact groove—not random reverb on every
part. Required timbres are deep plucked bass, short muted guitar/keyboard,
organ-like bubble, rim/cross-stick, compact kick, hat, shaker/hand percussion,
and optional short horn-like response.

**Proposed profile and variation axes.**

- `reggae_roots` — **Roots Reggae**. Axes: one-drop/rockers/steppers family;
  straight/lilt subdivision; bass melodic activity and pickup; skank single/
  double attack; organ-bubble density; chord-loop length; vocal-like hook/call;
  rim/hat variation; percussion; dub dropout/echo amount; and open/closed
  section ending.

**Original form blueprints.**

- `roots_one_drop_16`: a four-bar `I–IV–I–V`-derived cycle with combined
  kick/rim on 3, beat-1 kick space, short offbeat skank, complementary organ
  bubble, and an original bass melody that anticipates only the third chord.
  Four passes introduce a vocal-like hook, horn response, two-beat dub dropout,
  then bass variation and return.
- `roots_steppers_12`: three four-bar passes retain the same harmonic/bass cell
  while four-floor kick increases drive. A-prime removes half the skanks rather
  than adding notes; B sends one response through a bounded dotted echo and
  closes with bass/drum intact so the effect cannot erase the groove.

### Bossa Nova

Research the relationship among samba-derived rhythmic cells, guitar/piano
accompaniment, bass motion, extended tonal harmony, melodic placement, two- and
four-bar pattern evolution, native song forms, and restrained timbre. Determine
the explicit meter, beat-unit, grouping, anticipation, and click model Jam2
needs rather than forcing bossa into a generic straight-pop grid.

**Findings.** Keep one **Bossa Songbook** profile bounded to the late-1950s–60s
João Gilberto/Antônio Carlos Jobim-centred song practice and close small-
ensemble continuations. Do not label generic Jazz with a Latin preset, Samba,
MPB, Latin Jazz, or all Brazilian music as Bossa Nova. Scholarship describes
Gilberto's guitar as a reduction of samba's surdo-like bass pulse and
tamborim-derived syncopated upper rhythm, but explicitly warns that Bossa is
more varied than one fixed pattern
([Murphy/Reily summary](https://www.scielo.br/j/cint/a/3TTy5RCKtKQDJLghWzb833b/)).
Recent transcription research found eighteen accompaniment patterns in only
four early recordings
([Silva et al.](https://abrapem.org/wp-content/uploads/2024/09/219.-Padroes-de-acompanhamento-na-bossa-nova-ao-violao-de-Joao-Gilberto-em-quatro-faixas-de-seus-dois-primeiros-LPs.pdf)).
Jam2 therefore needs a small pattern grammar with contextual transitions and
melody/chord-boundary awareness, not one “bossa clave.”

Represent the groove in explicit binary meter: default notated 2/4 with two
main pulses, 16th-note timeline, bass on structural pulses, and syncopated chord
attacks that may anticipate or tie across a bar; accept equivalent 4/4/cut-time
source notation while preserving grouping and click. Berklee's Brazilian bass
curriculum explicitly spans 2/4, 4/4, and cut time
([Berklee](https://college.berklee.edu/courses/ilbs-262)).
Comping and bass must be independent lanes even when one guitar timbre renders
them together. Bass begins with roots/fifths and selected approaches/inversions;
upper voicings use 6/9, maj7/9, min7/9, dominant alterations, diminished
passing colour, secondary ii–V, chromatic bass motion, and modulations only when
voice-led and formally placed.

Melody is restrained, narrow-to-moderate, anacrustic, speech-like, and capable
of floating against steady accompaniment. Research on Brazilian “fluid meter”
describes Bossa's colloquial narrow-range melody and anacrustic, prosody-led
timing rather than mere quantized syncopation
([Ulhôa/Stover](https://www.mtosmt.org/issues/mto.24.30.4/mto.24.30.4.stover.php)).
Because Jam2 renders an instrumental analogue, use phrase-specific anticipation,
delay, duration, appoggiatura, and rests—not a blanket late-melody offset.
Support is sparse piano/guitar comping colour, a quiet counterline, or gentle
response; percussion is restrained shaker, cross-stick/brush, soft kick/surdo
analogue, and optional light cymbal.

Native form follows the selected original song blueprint: 16/24/32 bars and
AABA/ABAC/through-composed variants are candidates, not a whitelist. Complexity
adds inversions, extensions, secondary ii–V, chromatic bass, substitution,
phrase displacement, and counterline one at a time while keeping the intimate
balance and two-pulse rhythmic identity.

**Proposed profile and variation axes.**

- `bossa_songbook` — **Bossa Songbook**. Axes: 2/4 versus equivalent 4/4
  notation/grouping; accompaniment pattern family and transition; harmonic
  rhythm; tonal route; bass root/fifth versus chromatic/approach motion;
  extension/alteration; melody anticipation/fluidity; comping density;
  percussion restraint; counterline; and AABA/ABAC/continuous form.

**Original form blueprints.**

- `bossa_abac_32`: four eight-bar groups in 2/4. A presents a narrow pickup
  motif over `I6/9–V/ii–ii9–V13`; B sequences its rhythm while bass descends by
  step; A-prime changes upper voices rather than roots; C briefly tonicizes
  `iii`, returns via a voice-led ii–V, and leaves a soft tag. Comping selects
  related two-bar patterns based on melody attacks.
- `bossa_18`: `5+5+4+4` bars. An original speech-like motif begins before each
  phrase but changes duration rather than always shifting late; bass alternates
  root/fifth until the third group introduces one chromatic approach;
  counterline appears only in the final lead rest. This tests native asymmetric
  phrase support without losing the binary pulse.

### Modern Progressive Metal / Metalcore feasibility study

This is a conditional candidate, not an approved public style. Keep the scope
narrowly centred on the compositional and production practices represented by
Spiritbox, ERRA, and Nick Broomhall's public riff-writing work. Research
low-register riff grammar, syncopation and metric displacement, riff-module and
breakdown form, clean/heavy contrast, melodic hooks, bass/drum locking,
ambient/supporting layers, and especially the way distortion, articulation,
gating, double tracking, register, and percussion change perceived tension.
Prototype requirements must remain compact and local: no samples, copied riffs,
large amp platform, or promise to cover metal's many subgenres. Recommend
public inclusion only if Jam2 can make an original result that is both
compositionally and sonically honest.

**Findings and feasibility verdict.** The candidate is coherent only at the
narrow level requested. Garza's contemporary-metal analysis identifies six
riff types—straight/open division, breakdown, long-duration, pedal-tone,
weak-beat syncopation, and tremolo—and shows that cymbal and drum patterns can
make the same guitar rhythm project different time feels
([Garza](https://www.mtosmt.org/issues/mto.21.27.1/mto.21.27.1.garza.php)).
Half-time metal backbeats commonly leave kick placement free to coordinate with
guitar rather than to mark a generic Rock pattern
([Hannan](https://academic.oup.com/mts/article/44/1/121/6445145)).
Metalcore also supports compound AABA-like grouping in which breakdowns have a
formal role rather than being random “heavy” inserts
([Hudson](https://mtosmt.org/issues/mto.21.27.1/mto.21.27.1.hudson.pdf)).

The reference boundary is modern progressive/atmospheric Metalcore represented
by Spiritbox's low tunings, selective production-led writing, and clean/heavy
layer contrast
([Stringer interview](https://www.musicradar.com/news/spiritbox-interview-mike-stringer);
[rig documentation](https://www.premierguitar.com/videos/rig-rundown/spiritbox)),
ERRA's song-specific balance of simpler low riffs, technical material, melody,
and atmosphere
([ERRA album account](https://rocksound.tv/features/erra-cure-the-album-story)),
and Nick Broomhall's practice of identifying which instrumental choices cause
a reference's emotional response rather than copying its notes
([Broomhall](https://www.guitarworld.com/features/nick-broomhall-thick-riff-thursday)).
Exclude traditional Heavy Metal, Thrash, Death, Black, Doom, Djent as a whole,
and every implication that these three references define all Metalcore.

Pitch grammar should use low tonic pedals; perfect-fifth/octave reinforcement;
minor pentatonic, Aeolian, Phrygian/Phrygian-dominant colour only when grounded
by a characteristic degree; chromatic upper/lower-neighbour attacks; dissonant
semitone/tritone accents; and clean sections with wider chord extensions.
Rhythm is the primary riff identity: rests, chokes, mute/open alternation,
over-the-bar syncopation, groupings such as `3+3+2`, and selective straight
open-note release. Bass usually reinforces low guitar rhythm but must be its own
register/spectral lane; drums need kick/riff coordination, snare-defined
full/half-time, cymbal-defined pulse, tom transitions, and restrained blast or
double-kick-like density only where this narrow profile supports it. Melody is
a singable clean-vocal/lead-guitar analogue in open sections and a sparse
instrumental hook in heavy space; Jam2 should not synthesize screaming as its
melody.

The sound concern is decisive. Research on the distortion paradox shows the
production tradeoff between heaviness and intelligibility
([Mynett](https://www.taylorfrancis.com/chapters/oa-edit/10.4324/9781315742816-6/distortion-paradox-mark-mynett)).
Low tuning alone will sound like a dull oscillator. A minimum honest renderer
needs an excitation/decay model with pick transient and pitch-dependent string
decay; palm-muted/open/choked articulations; non-linear preamp distortion;
high-pass/low-pass and cabinet-like resonant filtering; controlled gate;
double-tracked micro-difference rather than a copied mono voice; separate
midrange bass distortion plus clean sub/fundamental; tight kick/snare/cymbal
transients; and clean chorus/delay/reverb layers. Register must be constrained
by usable spectral output, not transposed blindly.

**Verdict:** approve one profile as a *research-complete experimental
candidate*, but do **not** yet promote it to the normal public catalog. The
composition model fits Jam2 after explicit grouping and articulation are added;
the current renderer does not. Promotion requires the original tests below to
pass a user listening checkpoint against the declared reference qualities.
This is an honest gate on a known sonic dependency, not a demand for samples or
a general amp simulator.

**Proposed profile and variation axes.**

- `metal_modern_progressive` — **Modern Progressive Metalcore**. Axes: riff
  type; perceived full/half/double time; pulse grouping; pedal amount;
  mute/open/choke articulation; consonant/dissonant attack colour; unison
  versus independent bass; kick-lock strength; clean/heavy section ratio;
  melodic-hook prominence; ambient-layer density; breakdown placement; and
  metric-preserving versus metric-displacing variation. Odd or changing meters
  are allowed when the riff grouping warrants them, not injected to advertise
  complexity.

**Original form blueprints and sound-design test targets.**

- `metal_riff_12`: three original four-bar riff modules. A uses a low pedal and
  attack grouping `3+3+2` over 4/4; A-prime retains pitches but moves two chokes
  and lets the cymbal reveal a different pulse; B uses open fifths and a
  singable lead over half-time drums. This isolates whether articulation and
  time feel create meaningful development.
- `metal_contrast_18`: heavy A `6` bars, clean B `8` bars, compressed return
  `4` bars. The clean section keeps A's intervallic motif in augmentation over
  add9/sus voicings; the return removes one eighth from the final riff module
  and notates the local meter explicitly. This tests native asymmetric form and
  motivic continuity rather than arbitrary juxtaposition.
- Render each blueprint in four diagnostic states: dry excitation; articulated
  but clean; mono distorted/cab-filtered; and final doubled heavy/clean
  arrangement. Log fundamental/register, articulation, envelope, distortion
  drive, filter/cab parameters, gate thresholds, stereo differences, bass split,
  drum transient settings, and peak/RMS levels. Failure conditions are
  indistinct mute/open attacks, pitch collapse below the chosen register,
  masking of kick/bass/lead, fake chorus-like mono doubling, or “heaviness” that
  depends only on extra loudness.

## Cross-style research synthesis

This synthesis contains:

- A proposed public-style and internal-profile catalog with stable IDs, concise
  definitions, and explanations of all changes from the starting catalog.
- A profile-to-supported-mode matrix, plus a concise public-style summary where
  that is useful. Do not imply that every profile under a public style supports
  the same modes.
- A profile-to-meter, subdivision, and perceived-time matrix that defines the
  required explicit Jam2 meter model, click/accent behaviour, and UI choices.
- A profile-to-native-length and form-strategy matrix.
- A progression-family catalog with conventional analysis and exception tags.
- A groove-family catalog with anchors, allowed transformations, and timing
  policy.
- A bass-line grammar catalog describing rhythmic roles, pitch eligibility,
  kick interaction, phrase development, and style-native movement.
- A supporting-line grammar catalog distinguishing chordal comping, melodic
  harmony, countermelody, call-and-response, pads, and riffs where applicable.
- A feasible timbre and synthesis palette for each profile, with characteristic
  traits, bounded variation, teaching value, and honest limitations.
- A motif grammar comparison across styles.
- A section-continuation and contrast grammar showing reusable A-to-A-prime,
  A-to-B, and return relationships where supported.
- The proposed shared complexity ladder, each profile's idiomatic realization
  of its stages, and how the current eight controls map to that researched model.
- The approved style-guided variation axes and their bounded ranges.
- A list of existing progressions/grooves to retain, correct, replace, or
  remove.
- Open research disputes and the chosen conservative defaults.

### Final Phase 1B catalog recommendation

The research supports all thirteen approved public labels, but not every
provisional profile. It recommends **26 normal internal profiles** because each
changes at least two of harmony/tonality, melody/riff, bass, groove, form, or
supporting-line behaviour. This is a result, not a benchmark. `Indie` remains
reassigned; `Lo-Fi` and `Synthwave` remain cross-profile production families.
The one modern-metal profile remains experimental and outside the normal public
catalog until its sound test passes.

| Public style | Stable internal profiles | Compact public meaning | Change from starting/provisional catalog |
|---|---|---|---|
| Pop | `pop_loop`, `pop_sectional` | Contemporary Western loop-centred and sectional/lift Pop | Retain; split by form-bearing grammar, not mood or patch. |
| Rock | `rock_riff_modal`, `rock_shuffle_blues`, `rock_punk_garage` | Guitar/bass/drum Rock built from riffs, modal/power-root motion, shuffle/Blues relation, or concise Punk/Garage drive | Retain; absorbs only Indie practices that match these grammars. |
| Jazz | `jazz_swing_standards`, `jazz_bebop`, `jazz_fusion` | Small-combo Swing/Standards, Bebop, and selected electric Fusion | Retain and broaden carefully; ballad/two-feel are families, not profiles. |
| Modal Jam | `modal_groove`, `modal_atmospheric` | Pedal/vamp-based mode learning through groove or atmosphere | Rename from Modal Vamp; explicitly pedagogical, not one historical genre. |
| Blues | `blues_dominant`, `blues_minor` | Native-form dominant/major and minor Blues | Retain; groove and 8/12/16-bar variants remain families. |
| J-Pop / Anisong | `jpop_anisong_rock`, `jpop_idol_dance` | Selected modern Japanese anime Pop-Rock and idol/dance Pop | Rename and narrow; profiles differ in section/harmony and supporting-voice arrangement. |
| Country | `country_honky_tonk`, `country_contemporary` | Traditional honky-tonk/two-step and contemporary Country-Pop/Rock | Retain; add explicit 3/4 and compound-time families. |
| Electronic | `electronic_house`, `electronic_techno`, `electronic_breakbeat` | Three beat/layer/process grammars under a navigation umbrella | Rename from EDM; remove provisional Synthwave profile and keep its tag. |
| R&B / Soul | `soul_classic_motown`, `rnb_contemporary_neosoul` | Classic/Motown ensemble Soul and contemporary R&B/Neo-Soul | Retain; historical timing, bass, harmony, and support differences justify split. |
| Funk | `funk_static_pocket` | Static-vamp, interlocking pocket Funk | Retain one compact profile; Disco remains cross-style. |
| Hip-Hop / Trap | `hiphop_boom_bap`, `hiphop_trap` | Original sample-like Boom-Bap and pitched-808 Trap beat grammars | Retain; Lo-Fi remains a Boom-Bap production/feel family. |
| Reggae | `reggae_roots` | Roots Reggae with one-drop, validated rockers variants, and steppers | Add; do not generalise to Ska, Dancehall, or all Jamaican music. |
| Bossa Nova | `bossa_songbook` | Intimate Bossa songbook practice with independent bass/comping and fluid lead | Add; replaces any temptation to use a broad “Latin” row. |
| Experimental, not normal public catalog | `metal_modern_progressive` | Narrow modern progressive Metalcore composition/sound test | Research-complete candidate; public promotion waits for the specified listening checkpoint. |

### Tonal-collection and exception matrix

`Primary` means safe at the earliest applicable level; `later` means a
style-supported colour unlocked by complexity; `no` means do not select it as
the tonic collection for that profile. Local borrowed chords do not silently
change the declared tonic collection.

| Profile | Primary tonic collections | Later or bounded colour | Required exception/avoidance |
|---|---|---|---|
| `pop_loop` | Ionian, Aeolian; hybrid/ambiguous major-relative-minor centre | Mixolydian, Dorian, modal mixture, applied chords | Melody and harmony may imply different tonic readings; do not force a final I. |
| `pop_sectional` | Ionian, Aeolian | same-key mixture, secondary dominants, bounded section tonicization/modulation | Chromatic change must serve lift/contrast and include a return policy. |
| `rock_riff_modal` | Mixolydian, Aeolian, minor pentatonic; Ionian | Dorian, Blues collection, Phrygian colour | Power chords have unspecified third; preserve pedal/riff identity. |
| `rock_shuffle_blues` | dominant/major Blues, Mixolydian, Blues collections | minor-Blues colour and Rock modal mixture | Route native Blues schemas through Blues rules; do not “correct” ♭3/3 friction. |
| `rock_punk_garage` | Ionian, Mixolydian, Aeolian, minor pentatonic | restricted modal mixture | Prefer a small pitch/root vocabulary; chromatic density is not level. |
| `jazz_swing_standards` | major/minor tonal, Blues | tonicization, modal sections, substitutions | Seventh-chord/guide-tone analysis; no global scale pasted over changes. |
| `jazz_bebop` | major/minor tonal, Blues | chromatic approach, altered dominant, substitution | Chromatic notes require targets; preserve chord-scale and voice-leading context. |
| `jazz_fusion` | Dorian, Mixolydian, Aeolian, major/minor tonal | Lydian, pentatonic, altered/upper structures, multi-centre sections | Modal/static and functional sections are explicitly tagged. |
| `modal_groove` | Dorian, Mixolydian; bounded Aeolian/Phrygian | one/two-centre modal colour | Characteristic degree and tonic pedal required; suppress accidental functional V–I. |
| `modal_atmospheric` | Lydian, Dorian, Aeolian; bounded Phrygian | controlled pedal displacement | Same contour cannot represent every mode; avoid relative-key drift. |
| `blues_dominant` | dominant/major Blues with mixed pentatonics | selected substitutions/tonicizations | I7/IV7 are not automatically V/IV; melody–harmony dissonance is intentional. |
| `blues_minor` | minor Blues, Aeolian/minor pentatonic | Dorian 6, V7 or modal v, iiø–V later | Do not import major/dominant schema unchanged. |
| `jpop_anisong_rock` | Ionian, Aeolian | applied dominants, royal-road relatives, mixture, bounded section modulation | Every section key has entry/exit or deliberate open-end logic. |
| `jpop_idol_dance` | Ionian, Aeolian | same colour set with less compulsory modulation | Supporting harmony eligibility follows lead and singer-like range. |
| `country_honky_tonk` | Ionian, Mixolydian/major pentatonic | Blues inflection, V/x, brief vi/ii | Minor tonic is non-default; simple major does not mean no direction. |
| `country_contemporary` | Ionian, Aeolian only when evidence-family supports it | Pop-loop ambiguity, mixture, applied chords | At least one phrase/bass/fill marker beyond timbre must remain country-native. |
| `electronic_house` | minor/major tonal, Dorian/Aeolian, ambiguous loop | borrowed colour, short vocal-derived progression | Harmony may be absent; form validation cannot demand a cadence. |
| `electronic_techno` | unpitched, one-centre, Dorian/Phrygian/Aeolian or chromatic cell | controlled neighbour/second centre | Pitch scarcity is valid; do not infer full chords from percussion spectra. |
| `electronic_breakbeat` | minor/major, modal or ambiguous bass centre | sampled-like chromatic planing made from original material | Bass register/timbre may carry tonic more strongly than chords. |
| `soul_classic_motown` | major/minor tonal, Blues/Soul mixture | soul dominant, applied chords, plagal/backdoor colour | Tag soul-dominant behaviour; melodic bass may redefine inversion. |
| `rnb_contemporary_neosoul` | major/minor tonal, Dorian/Aeolian, ambiguous extended loop | planing, slash bass, backdoor, chromatic common-tone motion | Name upper structure and bass separately before assigning root/function. |
| `funk_static_pocket` | dominant/Mixolydian, Dorian/minor pentatonic | chromatic approach or one contrast centre | Static harmony is normative; no complexity pressure for progression. |
| `hiphop_boom_bap` | minor/major/modal/ambiguous loop | original sample-like planing and pitch shift | Allow unpitched/tonally incomplete beats; reserve rap space. |
| `hiphop_trap` | Aeolian, Phrygian colour, minor pentatonic, ambiguous sparse centre | harmonic-minor/leading-tone colour, 808 approach slides | Validate key against usable 808 register; no arbitrary full-range transpose. |
| `reggae_roots` | major/minor tonal, Mixolydian/Aeolian | dominant/Blues colour and bass-implied inversion | Bass may be tonal lead; preserve upper-part offbeat role. |
| `bossa_songbook` | major/minor tonal | secondary ii–V, altered dominants, diminished passing, modulation | Lead, bass, and upper voicings are separately analysed; not “Jazz scale + Latin beat.” |
| `metal_modern_progressive` | Aeolian, Phrygian, minor pentatonic, low pedal | Phrygian dominant colour, chromatic neighbour, dissonant semitone/tritone accent, clean add/sus colour | Register and distortion affect pitch validity; do not generalise beyond candidate. |

### Explicit meter, subdivision, click, and native-form model

The generator recipe must store the written meter `(numerator, denominator)`,
beat unit, grouping/accent array, subdivision family, per-lane timing policy,
perceived-time ratio (`half`, `normal`, `double`, or profile-specific), and any
local meter changes. `4/4`, `12/8`, and `3/4` are not interchangeable labels for
the same step count. The click derives from beat unit and grouping, offers a
subdivision click when requested, and visibly marks local changes. Tempo ranges
below are broad initial weighting centres, not hard gates; reference analysis
and listening in Phase 2 may widen them.

| Profile | Meter/grouping and timing centre | Perceived time and click | Native unit and form weighting |
|---|---|---|---|
| `pop_loop` | 4/4 straight/light swing; bounded 6/8 or 12/8 family | 70–130 BPM; normal or half-time; quarter click, compound dotted-quarter when selected | 2–8-bar loop; 8/16-bar section; 16–32-bar layer/melody arc |
| `pop_sectional` | 4/4 primary; supported 6/8/12/8; rare explicit phrase extension | 65–140; section may change perceived time without rewriting meter | 8-bar-like phrases with 4–12-bar alternatives; A–lift–B, A/A′, verse/prechorus-like 16–32+ |
| `rock_riff_modal` | 4/4 with explicit `3+3+2` or other attack grouping; bounded odd/local meter | 80–150; full/half; click follows stable beat while riff grouping is overlaid | 1–4-bar riff modules; 6/8/10/12/16+ sections; riff/chord A–B |
| `rock_shuffle_blues` | 4/4 triplet shuffle or 12/8; straight family explicit | 60–180; quarter or dotted-quarter click | 2-bar cell; Rock section or shared native 8/12/16-bar Blues form |
| `rock_punk_garage` | 4/4 straight eighth/sixteenth; stop/start | 140–220; strong normal/double drive | 1–4-bar cell; concise 6/8/10/12/14/16-bar section |
| `jazz_swing_standards` | 4/4 swing/two-feel; 3/4 and ballad compound family when selected | 60–240; tempo-dependent swing; quarter/half or 3-beat click | 8-bar phrase; 12-bar Blues, 16-bar, 32-bar AABA/ABAC and tags |
| `jazz_bebop` | 4/4 swing primary; occasional explicit 3/4 or local phrase displacement | 150–320; fast swing ratio narrows; quarter/half click | 4/8-bar head phrase; 12/16/20/32-bar original chorus and breaks |
| `jazz_fusion` | 4/4 straight 8/16 plus 3/4, 5/4, 7/8 and explicit additive grouping | 80–180; full/half/double may change by section | 1–4-bar vamp/riff; asymmetric 7/10/14+ modules; vamp/changes contrast |
| `modal_groove` | 4/4 straight/swing; profile-compatible odd grouping allowed | 60–140; groove supplies perceived time | 1–8-bar vamp; 8–32-bar motif/register/layer arc |
| `modal_atmospheric` | 4/4, 3/4, 6/8, 5/4 or free-looking events over explicit pulse | 40–110; sparse click, optional subdivision only for editing | 4–16-bar pedal spans; 12–32+ evolving arc |
| `blues_dominant` | 4/4 straight/shuffle or 12/8 | 60–180; quarter/dotted-quarter click | Native 8/12/16-bar schema; strophic call/answer and turnaround |
| `blues_minor` | 4/4 straight/shuffle or 12/8 | 50–130; normal/slow compound | Native 8/12/16-bar schema; sustained minor arc and turnaround |
| `jpop_anisong_rock` | 4/4 straight primary; 6/8/12/8 and explicit local/asymmetric changes supported | 100–190; normal/half/double section contrast | 4–12-bar phrase; 16–32+ sectional journey with optional modulation |
| `jpop_idol_dance` | 4/4 straight/four-floor primary; supported compound family | 105–175; normal/double-energy | 8-bar-like groups; 16–32+ A/A′/B with calls and tags |
| `country_honky_tonk` | 2/4 or 4/4 two-beat; 3/4 waltz; 6/8/12/8 and shuffle explicit | 65–185; click follows two-beat, three-beat, or compound grouping | 4-bar phrase; 12/16/24/32 forms, tags and turnarounds |
| `country_contemporary` | 4/4 primary; 6/8/12/8 and selected 3/4 | 65–155; normal/half | 4–8-bar loop/phrase; 16–32+ sectional Pop/Country arc |
| `electronic_house` | 4/4 four-floor; straight or bounded sixteenth swing | 118–132 centre; unwavering quarter click, optional 8/16 edit click | 1–4-bar core; 8-bar layer phrase; 16–64+ core/break/build/return |
| `electronic_techno` | 4/4 primary with polymetric ostinati; explicit odd cycle length | 120–150 centre; quarter click remains visible under phase | 1–5-beat/cycle cell; 4–16-bar process span; 16–64+ accumulative form |
| `electronic_breakbeat` | 4/4 primary; straight/swing; syncopated 1–4-bar breaks | 80–175 broad; half/double readings explicit | 1–4-bar break/bass loop; 8–32+ edit/break/re-entry arc |
| `soul_classic_motown` | 4/4 straight/eighth swing; selected 12/8 ballad | 75–140; quarter/dotted-quarter click | 2–4-bar ensemble cell; 8/16-bar section; 16–32+ call/response form |
| `rnb_contemporary_neosoul` | 4/4 sixteenth swing/pocket; 6/8/12/8 ballad family | 50–115; per-lane offsets around stable click | 1–4-bar harmonic/pocket cell; 8–24+ sparse sectional/loop arc |
| `funk_static_pocket` | 4/4 straight/swung sixteenth; bounded half-time/disco family | 80–125; quarter click, sixteenth editing grid | 1–4-bar interlock; 8–32-bar activation/break/mutation arc |
| `hiphop_boom_bap` | 4/4 with lane-specific swing; odd 3/5/7-beat or bar loop allowed when explicit | 70–108; stable backbeat with local timing offsets | 1–4/odd loop; 8/12/16/20/24/32 beat phrase via cuts/layers |
| `hiphop_trap` | 4/4 half-time frame; straight, triplet, 16/32 hat subgrids | 55–90 heard also as 110–180; show both written and perceived rate | 1–4-bar loop; 8/12/16/24+ activation/beat-switch form |
| `reggae_roots` | 4/4; straight-to-lilted eighth/sixteenth by lane | 65–100; quarter click with beat 1 visible despite one-drop space | 2–4-bar riddim; 8–32-bar bass/skank/dropout development |
| `bossa_songbook` | 2/4 binary default; source-compatible 4/4/cut time with preserved two-pulse grouping; 16th timeline | 70–155 according to declared beat unit; click accents two-pulse grouping | 2–4-bar comping relation; 16/18/24/32+ AABA/ABAC/asymmetric song form |
| `metal_modern_progressive` | 4/4 with additive attack grouping plus explicit 5/8, 7/8, 9/8 or local changes when riff-derived | 65–180; full/half/double shown independently of notation | 1–4-bar riff; asymmetric 6/10/12/18+ heavy/clean/breakdown modules |

### Shared timing rule

Swing, shuffle, and “human” feel are not one global percentage. Each lane stores
an anchor grid, subdivision mapping, deterministic micro-offset curve, velocity
shape, and allowed variance. The seed may choose among researched templates and
small bounded deviations; it must not independently jitter every event. Timing
recipes remain visible in Idea Details and diagnostics.

### Progression and tonal-motion family catalog

These families are parameterized grammars, not lists of literal song
progressions. A profile selects only compatible families, then a seed chooses
roots, durations, inversions, voicings, exception tags, and form placement.

| Family ID | Compatible profiles | Core grammar | Required tags/constraints |
|---|---|---|---|
| `loop_pop_diatonic` | Pop, contemporary Country, J-Pop, House, Boom-Bap | 2–8-bar diatonic or hybrid-tonic loop with rotations and varied harmonic rhythm | `tonic_reading`, `loop_boundary`, `open_or_closed`; no mandatory final I |
| `sectional_functional_lift` | Sectional Pop, J-Pop, Country, Soul | A establishes/prolongs; lift increases directed motion/activity; B confirms, evades, or reframes tonic | `lift_mechanism`, `cadence_strength`, `return_route` |
| `rock_modal_roots` | Riff/Modal Rock, Punk/Garage, selected Fusion | power-root motion among I/♭VII/IV/♭III/♭VI or modal relatives | `third_unspecified`, `pedal`, `plagal/open_end`; reject classical-function relabelling |
| `riff_implied_harmony` | Rock, Fusion, Funk, Metal | riff attacks/pedals imply colour; chord events occur only at structural roots/voicings | store riff and chord layers separately; `implied_root_confidence` |
| `jazz_songbook_functional` | Swing/Standards | tonic prolongation, predominant–dominant motion, turnarounds, tonicization across a native chorus | seventh-quality, guide tones, harmonic rhythm, cadence class |
| `jazz_bebop_chain` | Bebop | faster ii–V/cycle motion, applied dominants, substitutions attached to targets | every chromatic root has `target` and `substitution_type`; melody approach targets agree |
| `jazz_fusion_contrast` | Fusion | static/pendular modal segment contrasted with functional, multi-centre, or upper-structure segment | segment-level tonal model; no random chord-density escalation |
| `modal_pedal_colour` | Modal Jam, selected Fusion/Electronic | modal tonic pedal under one or more diatonic/quartal/sus colours | `mode`, `characteristic_degree`, `pedal_continuity`; suppress accidental V–I |
| `blues_native_schema` | Both Blues profiles, Shuffle/Blues Rock, Jazz when routed | explicit 8/12/16-bar slot schema with quick/slow change and turnaround alternatives | dominant-tonic exception, melody-friction exception, `turnaround`, `loop_or_close` |
| `jpop_circle_chromatic` | Both J-Pop profiles | royal-road relatives, circle sequence, applied targets, modal mixture, optional section modulation | `local_target`, `boundary_role`, key-entry/exit; never a mandatory IV–V–iii–vi |
| `country_three_chord_directed` | Traditional Country, selected contemporary | I/IV/V emphasis, plagal/authentic return, V/x, bass walk, tag | major-tonic default, `pickup`, `turnaround`; not limited to three chords |
| `soul_directed_plagal` | Classic Soul | tonal motion with melodic bass, plagal/backdoor and tagged soul-dominant colour | bass-inversion analysis; soul dominant not always V |
| `rnb_voice_led_upper_bass` | Contemporary R&B/Neo-Soul | slow upper-voicing motion plus independent slash/reverse-extension bass | store `upper_structure`, `bass_pitch`, common-tone/planing path separately |
| `static_vamp` | Funk, Modal Groove, Techno, selected Fusion/Reggae | one centre/chord with riff, bass, and orchestration carrying form | no pressure to add roots; optional one contrast centre only |
| `hiphop_beat_phrase` | Boom-Bap, Trap | repetitive, oscillating, or expansional 1–4/odd loop whose phrase can be timbral/rhythmic | `beat_type`, `loop_phase`, `rap_space`, `cut_points`; functional cadence optional |
| `reggae_bass_led_loop` | Roots Reggae | simple upper harmony whose independent bass may anticipate, connect, or imply inversions | bass is analysed first-class; skank duration/position separate |
| `bossa_voice_led_tonal` | Bossa | 6/7/9-based tonal motion, secondary ii–V, chromatic bass, diminished/altered passing and bounded modulation | melody attack and comping-pattern compatibility; bass/upper voices separate |
| `metal_low_pedal` | Experimental Metal | low pedal, fifth/octave, modal/chromatic attacks, clean add/sus contrast | articulation, register, spectral validity, and riff grouping are part of harmony |

### Groove-family catalog

| Family ID | Profiles | Immutable anchors | Seeded transformations and timing policy |
|---|---|---|---|
| `pop_backbeat` | Pop, contemporary Country/J-Pop | clear 2/4 backbeat relationship and lead space | kick syncopation, hats, half-time, fills, bounded straight/light swing |
| `pop_four_floor` | Pop, Idol/Dance J-Pop | quarter kicks and section pulse | clap/hat/stab variation, kick subtraction only at formal events |
| `rock_eighth_drive` | Riff Rock, Punk/Garage | bass/drum/guitar share stable eighth-level drive | riff-locked kick, stop/start, half-time, limited lane looseness |
| `rock_blues_shuffle` | Shuffle Rock, Blues | triplet/shuffle hierarchy and backbeat | tempo-aware ratio, boogie cell, straight-shuffle family switch only by recipe |
| `jazz_swing` | Swing/Standards, Bebop | ride cycle, hat 2/4, bass feel and asymmetric comping | tempo-dependent swing, comping/drum accents, brush/stick, bounded interaction |
| `jazz_two_feel_ballad` | Swing/Standards | half-note bass support and slow swing/brush identity | walking transition, sparse comping, fills at structural boundaries |
| `fusion_straight` | Fusion | electric bass/drum pocket and straight 8/16 or declared additive pulse | ghost notes, Funk/Rock weighting, meter grouping, riff lock |
| `blues_straight_12_8` | Blues, Shuffle Rock | declared straight 4/4 or compound 12/8 anchors | stop-time, slow/quick feel, turnaround fills; never silently swap subdivision |
| `country_boom_chick` | Traditional Country | alternating bass/low attack and upper chord/backbeat two-feel | walk-ups, shuffle, stop/tag; grouping displayed as 2/4 or 4/4 two-beat |
| `country_train` | Traditional Country | continuous train-like snare/brush subdivision and firm bass pulse | accents, kick, ghosting, fills; velocity shape is essential |
| `country_waltz_compound` | Both Country profiles | explicit 3/4 or 6/8/12/8 grouping | slow/fast waltz, compound ballad, bass/chord placement follows meter |
| `house_core` | House | 4/4 quarter kick, offbeat hat, 2/4 clap/snare relationship | swing, bass/stab syncopation, break/build layer removal and automation |
| `techno_process` | Techno | stable kick/pulse or declared non-kick core | polymetric ostinato, phase, timbral/percussion mutation; click stays stable |
| `breakbeat_syncopated` | Breakbeat | original repeating kick/snare identity | ghost edits, swing, half/double reading, stutter/cut at phrase points |
| `soul_backbeat` | Classic Soul | backbeat/tambourine and bass/drum ensemble identity | eighth swing, extra snare/kick, call activation; lane timing bounded |
| `rnb_sixteenth_pocket` | Contemporary R&B/Neo-Soul | sparse backbeat/subdivision frame | lane-specific swing/offset, ghost attacks, anticipation, negative space |
| `funk_interlock` | Funk | “one,” bass–kick relation, complementary comping, cyclic cell | one-event mutations, ghost/dead notes, per-lane timing; reject collision-heavy seeds |
| `boom_bap_break_like` | Boom-Bap | kick/snare identity and repeatable original break-like loop | lazy lane swing, ghost notes, odd loop/cut, Lo-Fi degradation after timing |
| `trap_half_time` | Trap | half-time snare frame, pitched 808 role, fast hat grid availability | sparse kick, straight/triplet rolls, slide/decay, negative space |
| `reggae_one_drop` | Roots Reggae | beat-1 kick space, combined central kick/rim weight, offbeat upper roles | hat lilt, bass anticipation, fills that do not resolve like Rock |
| `reggae_rockers` | Roots Reggae | reference-validated driving variant preserving central roots feel | multiple named kick variants; no single disputed universal pattern |
| `reggae_steppers` | Roots Reggae | four-floor kick plus roots rim/hat/skank relationship | rim variation, bass/skank subtraction, dub edits |
| `bossa_binary` | Bossa | two-pulse bass plus syncopated upper pattern over 16th timeline | contextual pattern transitions, anticipations, phrase-specific lead fluidity |
| `metal_riff_time` | Experimental Metal | guitar/bass attack grouping plus snare/cymbal-defined perceived time | kick lock, choke/open change, half/full/double reinterpretation, local meter |

### Dedicated bass grammar

Bass is a real editable/rendered lane with note, duration, articulation,
velocity, timing, role, and relationship metadata. It is never reconstructed
only from the displayed chord root. Collision checks consider kick, melody,
support, register, and low-frequency decay.

| Style/profile group | Rhythmic role | Pitch eligibility and phrase development | Kick/harmony interaction |
|---|---|---|---|
| Pop/J-Pop/contemporary Country | root/inversion support, octave pulse, approach, anticipatory hook | chord roots/5ths/3rds, declared inversions, diatonic/chromatic approaches; simplify under dense lead and develop at section lift | may lock selected kicks or answer them; avoid continuous unison |
| Riff Rock/Punk/Metal | riff unison, low pedal, structural root, connecting release | riff pitches, fifth/octave, modal neighbours, chromatic attacks; alternate doubling and independence by module | kick-lock strength is explicit; bass has separate register/spectrum and may sustain where guitar chokes |
| Shuffle Rock/Blues | root/fifth, boogie/shuffle cell, walk/turnaround | dominant/minor schema tones, sixth/♭7 colour, chromatic approach at eligible turnaround slots | reinforces shuffle pulse; fills stay out of vocal call and turnaround anchors |
| Swing/Bebop | two-feel, walking quarter, pedal, pickup into next root | chord tones on structural beats, scale/chromatic approaches, enclosure into target, contour/range and repeat constraints | bass establishes pulse independently of sparse kick; line targets form/chord boundaries |
| Fusion | ostinato/riff, syncopated electric line, pedal, functional connector | modal/pentatonic or chord-targeted according to section model; motif transformations and fills | strong bass–drum pocket with selective kick lock; may lead harmony |
| Modal Jam | pedal, ostinato, limited modal response | tonic/fifth plus characteristic or safe mode tones; rare pedal displacement; avoid functional root tour | reinforces centre; kick may support cell but must not turn every note into root |
| Traditional Country | alternating root/fifth, two-step, walk-up/down, waltz support | triad/root/fifth, passing scale tones, V/x approach; phrase-end walk/tag | “boom” role may be bass instrument or guitar; coordinate without double-trigger mud |
| House/Techno/Breakbeat | syncopated synth bass, sub pulse, ostinato, neighbour cell | declared tonal centre/mode; register-safe octave/approach; automation and note-length are first-class | sidechain-like envelope/space, kick conflict policy, phase relation |
| Classic Soul | melodic counterline that still supports harmony | chord tones, scale/chromatic approaches, inversions, pickups; phrase answers and section lift | locks selected drum anchors but creates forward motion between them |
| Contemporary R&B/Neo-Soul | sparse root/inversion, reverse extension, approach, delayed/anticipated attack | analyse bass separately from upper voicing; chromatic step and common-tone paths | per-lane timing and sub duration visible; preserve negative space |
| Funk | primary syncopated motif with downbeat return, rests, ghost/dead intent | root, octave/fifth, pentatonic/modal colour, chromatic pickup; one-event mutations and fills | explicit bass–kick anchor/counter relationship; collision validator is structural |
| Boom-Bap | independent bass motif or low element of original sample-like loop | sparse tonal/ambiguous root, approach and octave; may enter after first phrase | relation to kick selected as lock, offset, or answer; leave rap and kick space |
| Trap | long-decay pitched 808/sub with retrigger, legato and glide | key/register-validated roots, approaches, octave and bounded slides; decay shapes phrase | kick may layer onset or alternate; detect low-frequency overlap and preserve transient |
| Reggae | tonal lead: sustained/melodic line with pickups and rests | roots, inversions, fifth/octave, diatonic/pentatonic approaches; vary one pickup or cadence per section | occupies strong-beat space left by skank; selective kick lock, never generic Rock bass |
| Bossa | two-pulse root/fifth foundation, inversion and quiet chromatic approach | chord root/5th/3rd, stepwise/half-step approaches and voice-led descent; restraint before complexity | bass pulse grounds syncopated comping; soft percussion must not mask attacks |

### Supporting-line grammar

Each generated supporting lane has exactly one named role at a time. A role
specifies source material, allowed register, collision/priority policy,
activation span, and relationship to the lead. “Harmony” is not a free extra
melody.

| Role ID | Musical job | Primary profile use | Generation constraints |
|---|---|---|---|
| `support_comping` | chordal shell, stab, skank, clipped chord, pad attack, or guitar/keys pattern | Jazz, Bossa, Reggae, Funk, Soul/R&B, Pop, Country, Electronic | derived from harmony plus profile rhythm; voice-led; obey lead/bass register; articulation is part of role |
| `support_lead_harmony` | parallel/contrary/oblique line harmonizing selected lead phrases | J-Pop Idol, Soul/R&B, Pop, Country, selected Jazz | only on tagged lead spans; interval eligibility follows harmony and vocal-like range; cadence crossings checked |
| `support_countermelody` | independent but subordinate motif filling long lead space or creating B contrast | Pop/J-Pop, Jazz/Fusion, Modal, Bossa, Electronic, Metal clean sections | derived from lead motif or complementary rhythm; never continuous; collision and salience budget enforced |
| `support_call_response` | short answer after a lead call | Blues, Soul, Funk, Reggae, Country, Pop | begins in actual lead rest or controlled overlap; 1–2 cells; pitch/contour reference without exact copy |
| `support_pad_drone` | sustained centre, colour, texture, or slow voice-leading | Modal, Pop, R&B, Electronic, Metal clean, selected J-Pop | slow attacks/releases, common-tone preference, no low-bass duplication; automation bounded |
| `support_riff` | secondary rhythmic pitch object or unison/answer riff | Rock, Fusion, Funk, Electronic, Metal | independent riff representation; shared attack relation declared; may not be inferred as chord changes |
| `support_horn_stab` | compact punctuation, lift, or ensemble answer | Soul, Funk, Reggae, Jazz/Fusion | short attacks in lead space; 2–4 voice compact voicing optional; synthetic limitation disclosed |
| `support_hook_double` | selective octave/unison/timbral reinforcement | Pop, J-Pop, Rock, Hip-Hop/Trap, Electronic, Metal | only salient hook events; small deterministic timing/timbre difference when doubled |

The view may expose several supporting lanes, but the first implementation needs
one editable active supporting line plus chordal comping represented separately.
Recipes and data structures must allow more later without changing their
semantics.

### Timbre, articulation, percussion, and synthesis palette

The smallest coherent local renderer is not a General MIDI imitation set. It is
a reusable collection of compact engines:

1. subtractive wavetable/oscillator voice with pitch/filter/amplitude envelopes,
   noise, unison, drive, and modulation;
2. plucked-string/excitation voice with damping, pick position/brightness,
   pitch-dependent decay, mute/open/choke, slide/bend, and optional resonant
   body;
3. struck/tine/organ/keys voice using simple additive/FM/resonator components;
4. bass voice capable of clean fundamental plus independently driven midrange,
   glide and legato;
5. synthesized drum/percussion voices with transient, body/noise, pitch, decay,
   damping, velocity layers, choke groups, and per-piece tuning;
6. bounded effects—cabinet/formant filtering, saturation/distortion, chorus,
   delay/echo, reverb, gate, EQ/filter, and envelope-driven amplitude
   interaction.

Every recipe stores the actual engine and numeric parameters, not only an
instrument name. Presets are style-weighted starting regions with bounded
variation. The neutral-patch validation asks whether the note/rhythm/form
grammar remains recognisable; the styled render asks whether synthesis adds the
expected articulation and balance without falsely claiming acoustic realism.

| Profile group | Characteristic target | Feasible compact approach | Honest limitation / teaching value |
|---|---|---|---|
| Pop | clean/bright lead, pad, pluck, piano/keys, electric/acoustic-like bass, tight kit, clap | subtractive/pluck/keys engines, layered but sparse hooks, tuned synthetic kit, chorus/delay | Can teach register, envelope, layering and hook doubling; does not recreate named commercial productions. |
| Rock/Punk | picked/muted/open guitar-like attacks, bass, acoustic-kit impact | plucked excitation into drive/cab filter; mute/choke; separate bass; transient-rich kit | Power/riff grammar must survive simplified guitar; bends, feedback and realistic amp interaction remain approximate. |
| Jazz Swing/Bebop | piano/guitar comping, upright-like bass, ride/hat/brush kit, horn-like lead | keys/pluck, damped bass with attack/noise, modal cymbal/noise synthesis, restrained resonant lead | Synthetic brushes/cymbals/horns are approximations; timing, voice leading and roles carry the lesson. |
| Jazz Fusion | electric piano, synth, electric bass, clean/driven guitar, flexible kit | FM/tine keys, subtractive lead, pluck/drive, expressive bass, ghost-capable kit | Supports electric groove and timbral contrast; not a complete Fusion production studio. |
| Modal Jam | drone/pad, organ/keys, clean lead, ostinato bass, restrained/electronic drums | subtractive/additive sustained voices, evolving filters, delay/reverb, neutral groove palettes | Timbre is deliberately cross-style; modal identity must come from pitch/pedal/motif. |
| Blues | electric/acoustic-like guitar/keys, round bass, shuffle/straight kit, organ/harmonica-like colour | pluck with bend/slide metadata, keys/organ, bass, rim/kit; resonant mono lead | Convincing continuous bends and vocal grain are limited; popup teaches target/inflection rather than claiming exact performance. |
| J-Pop/Anisong | bright layered guitar/synth/keys, active bass, crisp kit, lead/harmony/call colours | Pop engines plus pluck/drive, fast envelopes, selective doubles and counterlines | Arrangement and section motion must carry identity; no attempt to synthesize Japanese vocals. |
| Country | acoustic/electric-like pluck, muted bass, dry kit, fiddle/pedal-steel-like response | pluck/body, bend/slide/volume-swell articulation, alternating bass, train/waltz kit | Fiddle and pedal steel are explicitly “-like”; bend targets and voice-leading remain inspectable. |
| House/Techno/Breakbeat | kick architecture, hats/claps, synth bass/stabs, noise/riser, evolving filter/drive | dedicated drum synth, subtractive/FM voices, automation, sidechain-like envelope, bounded FX | Strong fit; no recorded break or proprietary drum-machine sample. Sound parameters are excellent teaching data. |
| Soul/R&B | electric piano/organ, melodic bass, muted guitar, warm pad/strings, tambourine/clap, horn-like response | keys/additive, bass split, pluck, short resonant stabs, lane timing, warm saturation | Vocal and horn ensemble realism limited; harmony, bass, pocket and response remain authentic teaching targets. |
| Funk | dry kit/ghost notes, punchy or envelope bass, muted guitar, clav/organ, short horn stabs | bass with pluck/envelope option, clipped pluck, FM/clav, short stabs, tight transient kit | High fit once articulation/timing are separate; profile rejects reliance on a “wah” patch alone. |
| Boom-Bap/Lo-Fi | punchy break-like kit, original sample-like keys/chop, bass, filter/grit/tape-like motion | original rendered phrase re-cut internally, drum synthesis, per-lane swing, filter/saturation, bounded pitch drift/noise | Never uses copied samples; popup distinguishes compositional loop from degradation effect. |
| Trap | tuned long 808/sub, sharp hats/rims/claps, sparse bell/pluck/pad, kick transient | sine/triangle fundamental plus drive, glide/legato/decay, high-rate hat synth/choke, sparse subtractive/FM voices | Key/register and speaker translation matter; diagnostics expose fundamental, decay and kick overlap. |
| Reggae | deep round bass, short skank, organ bubble, rim/cross-stick, light percussion, dub echo | bass split, choked pluck/keys, organ additive voice, rim/shaker synth, send delay/filter | Synthetic horn/guitar realism limited; bass/offbeat/dub role separation is the primary lesson. |
| Bossa | nylon-like pluck combining bass/chord, quiet keys, soft bass, cross-stick/brush/shaker, intimate lead | plucked excitation/body with independent lanes, keys, soft transient percussion, restrained room | Guitar tone is approximate, but contextual bass/chord pattern and lead timing are directly teachable. |
| Experimental Metal | articulate low pluck, clean/ambient layer, distorted/cab-filtered double, bass split, tight modern kit | the four-stage diagnostic renderer specified in the Metal brief | Current engine cannot claim success. Public promotion depends on intelligible articulation, low-end separation, and user listening. |

Percussion recipes must expose piece, synthesis engine, tuning, transient/body
mix, decay, choke group, velocity curve, timing, and phrase role. Style packs
are small weighted recipes over those engines, not audio sample libraries.

### Motif grammar and section continuation

Every lead/riff motif records rhythm, relative contour/intervals, anchor tones,
range, rests, pickup, phrase role, harmonic fit, and salience. Transformations
operate on one or more named dimensions: exact/varied repetition, sequence,
rhythmic displacement, augmentation/diminution, interval expansion/contraction,
fragmentation, extension, register shift, inversion where idiomatic, call/
answer, reharmonization, and reassignment to a supporting role.

| Profile family | Motif centre | Favoured development | Avoid |
|---|---|---|---|
| Pop/Country/J-Pop/Soul | vocal-like hook with pickup, rests and phrase peak | repetition with one changed ending, sequence, range lift, call/harmony, B-section answer | chord-by-chord arpeggiation, nonstop lead, arbitrary virtuosity |
| Riff Rock/Punk/Metal | attack/rhythm plus pedal/root/interval identity | accent shift, choke/open change, transposition to structural root, augmentation in clean/B section | treating every attack as chord, random chromatic fill |
| Swing/Bebop | head-like cell tied to guide tones and phrase | sequence through changes, chromatic approach to target, rhythmic displacement, extension/elision | scale running without target, density-only “bebop” |
| Fusion/Funk | bass/unison/riff cell and complementary response | phase, fragmentation, register, call/answer, orchestration, contrast with lyrical theme | all lanes doubling continuously |
| Modal Jam | characteristic-degree motif over centre/pedal | withhold/reveal colour degree, register, space, rhythmic phase, limited sequence | generic scalar contour or functional cadence |
| Blues | vocal-like call and instrumental answer | pitch inflection, response, transposition to IV, chorus-level ending/turnaround change | correcting blue-note friction, filling call rests |
| Electronic | short bass/stab/riff/percussion identity | activation, filter/envelope, phase, subtraction, support-role reassignment | demanding tonal melody or cadence from every seed |
| Hip-Hop/Trap | original sample-like fragment or sparse instrumental hook | re-cut, pitch/register shift, omission, beat cut, answer, beat-switch reinterpretation | imitating rap flow as sung lead, copied sample contour |
| Reggae | spacious lead hook plus bass motif | call/response, bass pickup change, skank/bubble subtraction, dub echo at one phrase | busy lead over melodic bass, Rock-style fill resolution |
| Bossa | narrow speech-like pickup phrase | duration/placement nuance, gentle sequence, harmony-aware rest/counterline | global “late” offset or rigid repeated syncopation |

Research supports the later user-authored continuation feature, but it should
infer from the actual A section before consulting the catalog. The catalog
provides candidate operations and priors, never a forced style label.

1. Analyze A's metre/grouping, tempo/perceived time, tonal centre or pitch
   collection, harmony/loop type, harmonic rhythm, bass role, groove anchors,
   lead/riff motif, phrase/form boundaries, active support roles, register,
   density, timbre, and ending implication.
2. Produce several scored but inspectable continuation plans: `A′` preserves
   identity with one or two transformations; `B` changes one or more structural
   dimensions while retaining a motif/voice-leading/groove link; `return`
   prepares A without merely copying its last bar.
3. Reject plans whose required profile capabilities conflict with A. A style
   prior may suggest a Blues turnaround, Pop lift, modal register arc, Funk
   subtraction, electronic break/build, Jazz bridge, Bossa contrasting phrase,
   or Metal clean/heavy change only when A supplies compatible evidence.

| Relationship ID | Reusable operation | Strong profile uses |
|---|---|---|
| `continue_motif_variation` | same metre/harmony/groove; alter motif ending, register, density, or support | all profiles; safest A-to-A′ default |
| `continue_harmonic_lift` | increase harmonic rhythm/direction or change compatible loop while preserving hook rhythm | sectional Pop, J-Pop, Country, Soul, Bossa, Jazz |
| `continue_textural_lift` | retain harmony; add/remove bass, drums, support, register, or timbral energy | Pop, Electronic, Modal, Hip-Hop, Reggae, Metal |
| `continue_riff_answer` | new or transformed riff answers A and preserves attack/interval identity | Rock, Fusion, Funk, Metal |
| `continue_mode_pedal_arc` | retain centre; reveal colour degree, change upper structures/register or displace pedal | Modal, Fusion, Techno |
| `continue_native_form_slot` | complete the next structural slot in an identified schema | Blues, Jazz chorus, Bossa AABA/ABAC, Country tag |
| `continue_break_subtract` | remove anchors selectively, foreground hook/counterline, then rebuild | Electronic, Hip-Hop, Funk, Reggae, Pop |
| `continue_key_region` | bounded tonicization/modulation with explicit pivot/common-tone/direct-entry and return/open plan | J-Pop, Jazz, Bossa, sectional Pop; rare elsewhere |
| `continue_time_reframe` | keep attack material but change snare/cymbal/bass context to full/half/double or additive grouping | Metal, Fusion, Rock, Trap, Electronic |

The future feature must return its chosen relationship and evidence in detailed
analysis. It should offer A′ and B choices rather than assert that one is
objectively “best.”

### Research-approved shared complexity ladder

Keep eight levels for control and recipe compatibility, but replace the old
assumption that complexity is just denser melody or more chords. The levels are
an ordered curriculum of *available tools*. A selected level includes earlier
tools; the seed may use zero or a small number of newly unlocked tools. Every
profile remains recognisable at level 1 and level 8.

1. **Core grammar.** Profile-native metre, groove, tonic collection, simple
   voicing/articulation, bass role, clear motif and native phrase. Harmony is
   normally diatonic; documented core exceptions such as dominant Blues,
   Mixolydian Rock, static Funk, unpitched Techno, or one-drop rhythm are
   allowed because removing them would falsify the style.
2. **Voicing and connection.** Inversions, seventh/6/add/sus or profile-native
   extensions are allowed but not forced; smoother upper-voice and bass
   connection; one restrained accompaniment variation.
3. **Directed colour.** Secondary/applied dominants, tonicization, modal
   borrowed colour, chromatic approach, blue-note inflection, or the closest
   profile-native analogue. Every colour has a target or stated non-functional
   role.
4. **Rhythmic development.** Syncopation, subdivision variation, phrase
   anticipation/delay, deterministic lane timing, ghost/dead articulation, or
   one metric-layer operation; density remains bounded.
5. **Expanded tonal/riff vocabulary.** Modal interchange, backdoor/plagal
   colour, substitutions, planing, altered extensions, richer riff interval,
   bass-led reharmonization, or profile-native timbral/pitch process.
6. **Independent dialogue and form.** Countermelody, lead harmony, call/
   response development, role reassignment, stronger A′/B contrast, break/
   build, or cross-phrase motif transformation.
7. **Large-scale tonal and metric tools.** Bounded modulation, multi-centre
   form, odd/additive or changing meter, polymetric phase, metric displacement,
   advanced turnaround/substitution, or equivalent structural process.
8. **Integrated mastery.** A coherent seed-weighted combination of selected
   earlier tools with longer-range recall, transformation, restraint, and a
   style-native return/ending. This level does not require every tool and is not
   a maximum-note mode.

| Profile | Levels 1–3: foundation, voicing, directed colour | Levels 4–5: rhythm and expanded vocabulary | Levels 6–8: dialogue, structure, advanced integration |
|---|---|---|---|
| `pop_loop` | triad/add/sus loop, inversions, one applied/borrowed target | hook displacement, bass approach, hybrid-tonic or mixture colour | support hook, layer-led A′/B, longer loop recall without compulsory modulation |
| `pop_sectional` | simple A/B relation, voice-led voicing, directed lift chord | anticipation, harmonic-rhythm change, modal/plagal colour | countermelody/harmony, key-region lift, asymmetric phrase and integrated return |
| `rock_riff_modal` | core pedal/power riff, open third, modal root colour | attack displacement, mute/open, richer modal/chromatic neighbour | secondary riff, odd/local meter, clean/chordal contrast and motif return |
| `rock_shuffle_blues` | core shuffle/boogie and Blues-root motion | stop-time, call displacement, turnaround colour | developed responses, native-form reharm where eligible, cross-chorus integration |
| `rock_punk_garage` | compact power roots and straight drive | anticipations, stop/start, limited modal colour | response riff and phrase compression/extension; oddity only if it preserves directness |
| `jazz_swing_standards` | seventh shells, ii–V/turnaround, inversions/extensions | tempo-aware swing variation, chromatic approach, substitutions | interactive comping/support, reharmonized bridge/tag, modulation and chorus recall |
| `jazz_bebop` | guide-tone changes and clear head before added approaches | syncopation/elision, enclosures, altered dominant/substitution | break/dialogue, denser harmonic route, asymmetric chorus and integrated target logic |
| `jazz_fusion` | modal or functional electric core, extensions, bass/riff | ghosted pocket, upper structures, richer riff/pentatonic colour | lyrical/riff dialogue, mixed meter, multi-centre contrast and return |
| `modal_groove` | tonic pedal, characteristic degree, simple quartal/sus colour | motif phase, bass syncopation, second modal colour | counterline, additive metre, pedal displacement and long-range colour reveal |
| `modal_atmospheric` | drone, sparse motif, slow colour voicing | phrase fluidity, register/timbre process, bounded dissonance | countermelody, asymmetric arc, second centre and integrated unresolved return |
| `blues_dominant` | native schema, dominant/major-minor mixture, simple call | shuffle/straight nuance, blue approach, richer turnaround | developed response, substitutions/stop-time, multi-chorus motif memory |
| `blues_minor` | native minor schema, iv and selected V/v | timing/inflection, chromatic bass, iiø–V where selected | countermelody/response, contrasting slot, advanced turnaround and chorus arc |
| `jpop_anisong_rock` | diatonic seventh route, active bass, pickup hook; applied target | denser sequence/syncopation, mixture/circle colour | harmony/counterline, boundary modulation/local meter, long sectional recall |
| `jpop_idol_dance` | bright diatonic route, simple lead/call, inversions | dance syncopation, applied colour, bass anticipation | singer-like harmony/calls, section key/layer lift, integrated multi-role return |
| `country_honky_tonk` | I/IV/V, alternating bass, pickup/fill; V/x allowed | train/shuffle/waltz nuance, walk and bend/double-stop colour | harmony/response, richer tag/turnaround, metre/form contrast without losing simplicity |
| `country_contemporary` | Pop/Country loop, root/octave bass, vocal hook | backbeat/6-8 variation, mixture and guitar response | harmony/support, sectional lift/modulation, cross-section hook transformation |
| `electronic_house` | four-floor core, simple bass/stab, diatonic/modal loop | swing, automation, bass/stab mutation and colour | break/build counterhook, polymetric layer or key colour, long process return |
| `electronic_techno` | kick/pulse, one-centre ostinato, simple spectral motion | phase/syncopation, percussion and neighbour/timbre expansion | subtraction dialogue, polymeter/local grouping, integrated accumulative process |
| `electronic_breakbeat` | original break core and bass centre | ghost/swing/edit, re-cut motif and chromatic colour | counterhook/break, half/double or odd loop, long beat-switch/recombination |
| `soul_classic_motown` | tonal triad/seventh, melodic bass, simple call; soul colour | eighth feel, bass/response development, applied/plagal motion | harmony/horn dialogue, sectional lift/key region, ensemble recall and tag |
| `rnb_contemporary_neosoul` | seventh/9 voicings allowed, sparse bass/lead, smooth connection | lane pocket, slash/reverse-extension bass, planing/backdoor colour | vocal-like harmony/counterline, asymmetric B/key region, integrated timing/voice-leading arc |
| `funk_static_pocket` | one-centre interlock and “one,” simple bass/comp cells | ghost/dead timing, one-event mutation, chromatic pickup | call/horn dialogue, break/metric phase, long-range groove recall; no chord-count inflation |
| `hiphop_boom_bap` | original loop, bass, kick/snare and restrained hook | lane swing, re-cut/ghost edit, sample-like pitch/colour | response/beat cut, odd/expansional loop, integrated A/B beat transformation |
| `hiphop_trap` | minor/sparse loop, half-time frame, register-safe 808 | hat/roll gesture, kick–808 shift/slide, tonal neighbour | counterhook/beat switch, metric reframe, long decay/space/form integration |
| `reggae_roots` | one validated groove, bass-led loop, skank/bubble and simple hook | lilt, bass pickup, double-skank/dub colour | call/horn response, dropout/echo B, groove-family contrast with intact identity |
| `bossa_songbook` | binary bass/comping, 6/7/9 voicings, narrow pickup lead | contextual pattern shift, chromatic bass, secondary ii–V/alteration | counterline, modulation/asymmetric phrase, fluid lead and form-wide return |
| `metal_modern_progressive` | low pedal/fifth riff, mute/open, kick lock and simple clean hook | attack displacement, dissonant neighbour, articulation/tone contrast | ambient/counterline, local meter/clean-heavy form, integrated metric/riff recall |

For version-6 generation, the existing UI value `1…8` maps directly to the same
numbered researched stage; no fake precision is gained by renumbering. Recipes
store the requested stage, available tools, selected tools, and style-specific
realization. Earlier generated recipes are rejected rather than migrated or
silently regenerated under the new
meaning.

### Variation-axis contract

The axes listed in each individual brief are the approved seed vocabulary.
Implementation must type them rather than hiding them in arbitrary random
branches:

- categorical axes select a compatible family, role, mode, form, ending, or
  transformation;
- bounded numeric axes control density, probability, swing/offset, velocity,
  range, drive, decay, filter, echo, or automation inside profile-reviewed
  limits;
- ordered axes such as `sparse → active` or `root support → melodic bass` use a
  small declared scale with musical descriptions;
- dependent axes expose compatibility—for example a hat-roll rate exists only
  for a Trap family, walking bass only for a compatible Jazz feel, and a
  modulation return only after a section-key change;
- the recipe records the candidate options, seeded choice, numeric value, and
  any constraint that clipped or rejected it.

Random style/profile selection first chooses a public style, then a profile
using explicit user-visible weights, then compatible mode, metre, form,
progression/groove, roles, and timbres. It does not choose “mood” and retrofit
that choice across incompatible styles.

### Legacy catalog disposition

The current six-progressions/five-grooves-per-style catalog is useful as a
source of audition material, not as the new specification. Every current
family must be re-encoded under profile, mode, native form, meter, bass, and
role metadata; no old array is retained merely to preserve a count.

| Current area | Retain or route | Correct or replace | Remove as a governing rule |
|---|---|---|---|
| Pop progressions/grooves | Keep common loop motions, straight, four-floor, syncopated-kick and half-time ideas as `pop_loop`/`pop_sectional` families | Add hybrid-tonic/open-end analysis, section lift, bass/support and actual form; treat Disco as cross-style | Fixed one-chord-per-bar repetition, unconditional last-bar tonic, patch/mood-defined form |
| Indie | Route modal roots/pedal to Rock/Modal, motorik to Rock/Electronic, garage to Punk/Garage, intimate loop ideas to Pop | Revalidate every routed pattern in destination grammar | Public `indie` ID, six progression/five groove quota, timbre as grammar |
| Rock | Keep I–♭VII–IV, minor descent, shuffle, straight/half-time and syncopated attack material | Encode power roots/riffs separately from chords; add Punk/Garage, bass and articulation; route native Blues forms | Generic block-chord realization of every riff and mandatory tonic ending |
| Jazz | Keep ii–V, turnarounds, backdoor/circle candidates, swing/two-feel/brush and straight Jazz-Funk seeds | Split three profiles; repair root/quality/target analysis; make swing tempo/lane-aware; add walking bass and comping | One Jazz row where “Jazz-Funk” is only one of five drum patches and every melody shares one scale |
| Modal Vamp | Keep Dorian/Mixolydian/Lydian/Phrygian/Aeolian source ideas and sparse/tom/ride pulses | Rename/split profiles; pedal-tag upper colours; characteristic-degree melody and non-functional validation | Generic four-chord loop per mode, accidental relative-key cadence |
| Blues | Keep slow/quick 12-bar schemas, selected 8-bar/minor/Jazz variants, straight/shuffle seeds | Separate dominant/minor profiles; validate 8/16 forms; preserve blue-note exceptions and endings; add call/answer/bass | Any global “last bar = tonic,” one Blues scale over both profiles, forced Jazz substitutions |
| Anime/J-Pop | Keep circle/royal-road-related and applied-dominant candidates plus drive/dance/half-time seeds | Rename/split profiles; audit malformed slash/degree spelling; add sectional modulation, lead harmony/calls and active bass | Treating IV–V–iii–vi as definition or complexity as extra chromatic chords |
| Country | Keep I/IV/V, V/x, boom-chick/train/two-step/shuffle and Country-Rock seeds | Split profiles; put vi-loop families mainly in contemporary; add explicit 3/4/6/8, alternating bass, pickup/fill roles | One global 4/4 bar model and minor/mood mode overrides |
| EDM | Keep House, Techno and Breakbeat rhythmic seed material; route Disco-House; retain Synthwave only as tag | Rename/split; make layer process, bass, automation, phrase and kick architecture first-class | One shared tonal progression list, `edm_synthwave` profile, static full-section render |
| R&B/Soul | Keep Motown, laid-back, Neo-Soul, ballad and Funk-Soul source grooves plus tonal/plagal candidates | Split historical profiles; replace global swing/snare offset with lane timing; separate upper harmony and bass | Mood-selected mode/cadence and one generic electric-piano treatment |
| Funk | Keep static I7/i7, I–IV contrast and “one”/linear/sync/disco/half-time source cells | Make static pocket primary; move ii–V to Jazz/Fusion unless specifically evidenced; add bass/comp interlock and mutation rules | More chords/notes as complexity, collision-prone independent variation |
| Hip-Hop/Trap | Keep Boom-Bap/Lo-Fi/Trap rhythmic source cells and compact minor-loop candidates | Split profiles; Lo-Fi tag; original sample-like beat type; pitched 808/glide/register and high-rate hat representation | One combined BPM/melody/harmony grammar and patch-only 808 |
| Global mood and complexity transforms | Retain descriptive UI language only as optional non-generative metadata if useful | Replace with profile axes and the eight-stage tool curriculum | Mood-to-mode override; automatic final chord recolouring; globally injecting iv, diminished, backdoor, tritone substitute or modulation without profile/target logic |

Specific current defects already identified by the code audit remain mandatory
repairs: slash/secondary-dominant parsing must distinguish inversion from
target; Roman accidentals and chord qualities must be mode-aware; chord pitches
must preserve spelling/function; melody validation must recognize declared
non-chord/blue/modal tones; native form must not be expanded by modulo-copying a
short array; and the renderer must not rewrite every non-Blues final event to
tonic.

### Smallest coherent hard-scope redesign

The research does not support implementing harmony first and postponing the
other roles. The smallest version that can represent the approved catalog
honestly contains:

- a profile registry with evidence-facing names, compatibility tables, typed
  axes, weighted families, complexity realizations, and stable IDs;
- explicit meter/grouping/subdivision/perceived-time and local-change data used
  by planner, grid, click, renderer, recipe, import/export, and diagnostics;
- profile-native section/form plans with arbitrary bar/beat spans, phrase
  boundaries, structural roles, ending types, layer activation, and automation;
- separate harmony/comping, melody/riff, bass, supporting-line, drum/percussion,
  and automation lanes; each event has duration, velocity, articulation,
  deterministic timing, source role, and explanation;
- a tonal model that represents collection, scale-degree spelling, chord upper
  structure, independent bass, function/non-function tag, exception/target,
  section key, and modulation route;
- the compact synthesis engines and percussion parameters described above,
  with usable-register and collision checks;
- an expanded musical editor that makes bass and one supporting line visible
  and editable alongside melody, harmony/comping and drums, with arbitrary
  meter/length navigation;
- a teaching-first Idea Details popup: style/profile and what it is; the core
  groove/form/tonal idea; how to try it in a jam; the selected complexity tools
  and how each changed this seed; layer listening tips; and concise limitations.
  A **Detailed Analysis** action expands the raw recipe/event/form/timing/
  synthesis/validation data for review and logs;
- one strict version-6 research recipe with no earlier-recipe migration,
  Mood/Character records, or fallback generator;
- deterministic structural/tonal tests and fixed-seed listening exports that
  report hard data rather than a subjective score.

Automatic continuation from a user-authored A section is still outside this
first redesign. Its analysis fields and continuation relationships are
preserved now so it does not require another foundational model later.

### Evidence limits, open disputes, and conservative defaults

| Topic | Evidence limit or dispute | Conservative Phase 1B default |
|---|---|---|
| Modern Metal sound | Composition is well supported; the present renderer has not passed the four-stage low-register articulation/listening test. | Keep `metal_modern_progressive` experimental and out of the normal public catalog until that test and user discussion. |
| Roots Reggae rockers | Practitioner/secondary sources agree it is more driving than one-drop but conflict on one exact kick formula. | Store several named, cited, listening-validated variants under one groove family; never publish one as universal. |
| Jazz Fusion breadth | Fusion spans Jazz-Rock, Jazz-Funk, modal electric and many later hybrids. | Keep one selected electric groove/modal-functional profile; exclude repertoire that needs a different ensemble/form grammar and revisit after listening. |
| Contemporary Country breadth | Current Country-Pop/Rock overlaps Pop/Rock strongly and minor-tonic use varies by repertoire. | Require a phrase, bass, fill, form, or articulation marker beyond patch; weight major higher without banning evidence-supported minor. |
| Pop/J-Pop umbrella boundaries | Both public labels cover eclectic production and historical periods. | Use the stated periods/practices and two grammar profiles each; route cross-style timbres rather than cloning profiles. |
| Bossa patterns and notation | Performers vary accompaniment; sources use 2/4, 4/4, or cut-time representations. | Preserve binary two-pulse grouping and contextual pattern grammar; show written meter/beat unit rather than claim one notation is uniquely correct. |
| Tempo ranges | Published corpora and sources are uneven across profiles; tempo interacts with beat unit and perceived time. | Treat matrix values as weighting centres, log actual reference-derived updates in Phase 2, and permit outliers with explicit family support. |
| Acoustic/horn/voice realism | Compact synthesis cannot reproduce singers, brass sections, pedal steel, fiddle, brushes, nylon guitar, or amplified guitar exactly. | Use honest “-like” labels, prioritize articulation/role/register, expose parameters, and require grammar to survive neutral timbre. |
| Lo-Fi and Synthwave status | Both have recognisable aesthetics, but current evidence does not show a necessary independent multi-layer grammar inside Jam2. | Keep typed production/timbre/form tags; revisit only if a future brief identifies distinct bass/groove/form/support rules. |
| Profile count | Twenty-six is larger than the current catalog but far below an arbitrary breadth exercise. | Keep every profile because its grammar differs materially; merge during Phase 2 only if implemented rules collapse to the same relationships. |

When a source and listening observation disagree, record both and choose the
sparser, less stereotyped rule until further evidence supports a stronger one.
No weakly evidenced feature becomes a universal rule merely because it is easy
to code.

## Phase 1B final research and catalog checkpoint

Present the completed research in a form that makes these decisions easy to
review:

1. Which starting public style labels remain appropriate for Jam2?
2. Should any public styles be renamed, narrowed, merged, replaced, removed, or
   added?
3. Are the proposed internal profiles coherent and sufficiently distinct?
4. Which variation axes should each profile expose to the seed?
5. Which modes and tonal exceptions are valid for each profile?
6. What shared complexity stages best teach distinct musical tools, how does
   each profile realise them, and how should existing eight-level inputs map if
   the scale changes?
7. What explicit meter, subdivision, click/accent, and time-feel model is
   required to support every approved profile honestly?
8. Which native lengths, structural strategies, and ending types should each
   profile weight?
9. Which legacy progression and groove families should be removed?
10. Do the original native-form and long-form blueprints provide suitable
    implementation targets based on the research and musical observations to
    date?
11. Are the researched section-continuation relationships useful foundations
   for a later A-to-B generation feature?
12. What dedicated bass, supporting-line, view/editing, timbre, synthesis, and
    percussion model is the smallest coherent implementation of the approved
    hard scope?

### Phase 1B recommendation for user discussion

1. **Public labels:** retain the thirteen-label research catalog: Pop, Rock,
   Jazz, Modal Jam, Blues, J-Pop / Anisong, Country, Electronic, R&B / Soul,
   Funk, Hip-Hop / Trap, Reggae, and Bossa Nova.
2. **Changes:** remove Indie as a public grammar; keep the approved renames;
   retain Reggae and Bossa Nova; keep Lo-Fi and Synthwave as typed variation
   families; do not yet promote modern Metal from experimental status.
3. **Profiles:** approve the 26 stable normal profiles in the final catalog
   table. They are compact relative to the repertoire and each changes multiple
   musical relationships rather than only a patch.
4. **Variation:** approve the profile axes in each brief under the typed,
   dependency-aware variation-axis contract. They are seed choices, not UI
   requirements to expose every parameter at once.
5. **Tonality:** approve the profile-specific matrix and its declared Blues,
   power-chord, modal, Soul, static-harmony, bass-led, and unpitched exceptions.
6. **Complexity:** retain eight levels but adopt the shared tool curriculum and
   profile-specific realizations. A level unlocks possibilities and reports
   which were selected; it does not force density.
7. **Time:** approve explicit numerator/denominator, beat unit, grouping,
   subdivision, per-lane timing, perceived-time, click and local-meter data.
   Remove the present global N/4 and swing-percentage assumptions.
8. **Form:** approve arbitrary profile-native spans and the matrix weightings.
   `4/8/12/16/32` remain useful choices where native, not a whitelist.
9. **Legacy material:** retain and route only the source ideas identified in the
   disposition table. Remove fixed family-count tests and correct the forced
   cadence, mode/mood, Roman/slash, riff-as-chord, and modulo-form behaviours.
10. **Blueprints:** the original brief blueprints are suitable Phase 2 targets
    because they cover loop development, native schemas, asymmetric forms,
    section contrast, meter, bass, support, and timbre without copying songs.
    Phase 2 should add concrete event-level fixtures rather than more
    near-duplicate prose examples.
11. **Continuation:** preserve the analysis fields and candidate relationships;
    they are a strong foundation. Keep automatic user-A continuation outside
    the first redesign and infer from A before applying style priors.
12. **Hard capability scope:** approve the inseparable model listed under
    “Smallest coherent hard-scope redesign,” including dedicated bass, one
    editable supporting line plus comping, arbitrary meter/form, local
    synthesis/percussion, and teaching/details diagnostics.

The only material catalog decision intentionally left open is whether the
experimental modern Metal profile should later become a normal public option.
The research recommends deciding that from the specified original sound test
and listening discussion, not from the notation alone.

Do not implement until these questions have agreed answers recorded in this
document. This is the final implementation-catalog approval; it is distinct
from the earlier provisional approval to perform detailed research.

# Phase 2 — Convert research into a musical specification

Phase 2 begins only after the Phase 1B final research and catalog checkpoint.

## Style profile model

Maintain a complete research/specification record for each accepted profile
containing:

- Stable profile ID and public parent style.
- Evidence summary and supporting citations.
- Supported tonic collections/modes.
- Tempo and perceived-time ranges.
- Supported meters, subdivisions, swing/time-feel policies, and any
  half-time/double-time interpretation.
- Compatible progression and groove families.
- Bass-line roles and movement grammar.
- Supporting-line roles and their relationship to melody and comping.
- Feasible timbre palettes and bounded synthesis parameters.
- Native phrase/form lengths.
- Compatible structural strategies and their weights.
- Harmonic-rhythm and cadence policies.
- Comping articulation rules.
- Motif rhythm, contour, range, density, and transformation grammar.
- Drum anchors, timing feel, phrase development, and fill policy.
- Style-specific variation axes and legal combinations.
- Complexity-gated techniques.
- Plain-language teaching concepts and pointers from those concepts to concrete
  generated decisions.
- Section-continuation relationships retained for possible future use.
- Required dedicated bass, harmony, countermelody, response, riff, pad, and
  comping relationships together with their view/editing representation.

At the Phase 1B checkpoint, tag each researched capability as either:

- **Required redesign capability:** required in the version-6 runtime profile
  and redesign. Explicit meter/native lengths, dedicated bass, role-aware
  supporting lines, and profile-required timbre/percussion always use this tag.
- **Future capability:** preserved in the research/specification for later
  user-authored continuation or another newly discovered feature outside the
  approved hard scope.

Build the compact authored runtime profile from the required redesign
capabilities. Do not add dormant recipe or runtime fields solely to reserve a
future design. The runtime profile must bind every enabled musical layer to one
coherent plan; at minimum, the generator must not choose a progression and
groove independently merely because both share a broad style label.

## Research-approved complexity ladder

Convert the approved Phase 1 complexity findings into a compact cumulative
ladder. Each stage defines:

- The shared musical concept being made available.
- The profile-specific operations that can express that concept authentically.
- Preconditions, destinations, resolution rules, and interactions with mode,
form, groove, melody, bass, voicing, supporting lines, and timbre.
- Seed-controlled weights, including the valid choice not to use an unlocked
  technique in a particular idea.
- Teaching text for the general concept and the concrete realization selected
  for the current idea.

A selected stage permits the tools at that stage and the stages below it; it
does not require all of them to appear. When a shared concept has no honest
realization in a profile, record that limitation rather than inserting an
unidiomatic device. Preserve deterministic generation; if the stage model
changes, update the version-6 schema instead of retaining historical mappings.

## Joint style, profile, mode, time, and length selection

Resolve request compatibility before choosing musical material:

1. Begin with profiles allowed by an explicit public style, or the whole
   approved catalog for Random Style.
2. Intersect that set with any explicit mode, meter/time model, and form-length
   constraints. `Auto` values do not constrain the set at this stage.
3. If no profile remains, report the unsupported combination clearly. Do not
   replace a manual choice or silently change the public style.
4. Select an eligible internal profile seed-deterministically using its approved
   weights.
5. Resolve each `Auto` value from that profile's supported, mutually compatible
   choices, then choose its progression, groove, form, variation, and other
   material from the same coherent plan.

Idea Details and Detailed Analysis should show the input constraints, the
eligible profile decision, and every automatically resolved value.

## Style-guided deterministic variation plan

Remove the universal Mood/Character choice from new generation. After style
profile selection, derive a `VariationPlan` from the seed.

The plan should contain only axes supported by that profile. Example axis types
include:

- Energy or intensity.
- Rhythmic density and syncopation.
- Harmonic motion versus pedal/stasis.
- Openness versus tight articulation.
- Brightness, warmth, tension, yearning, menace, calm, or romantic colour where
  supported by the repertoire.
- Straight, laid-back, pushed, swung, machine-tight, or loose timing feel.
- Melodic activity, register, space, and phrase lift.
- Textural build, subtraction, or contrast.

Each axis must map to explicit bounded changes in harmony, motif, groove,
articulation, tempo, or synthesis. It must not silently replace the selected
mode or unlock chromatic techniques above the selected complexity.

## Idea Details teaching and detailed analysis

The default popup must prioritise what a musician can learn and try:

- Explain the selected style/profile and its most important concepts in
  accessible, technically correct language.
- Point to how those concepts appear in the current generated idea rather than
  displaying a generic style essay.
- Give practical suggestions for playing over the idea or applying the same
  harmony, motif, groove, bass, articulation, or form concepts in a new jam.
- Explain the simple foundation first, then identify the authentic techniques
  introduced at relevant complexity steps up to the selected level. Complexity
  is an expansion of vocabulary and difficulty, not a quality score.

The default view should remain concise. A **Detailed Analysis** control expands
the complete inspectable data, including the stable profile, seed, variation
axes and values, mode and collection, form and boundary roles, harmonic
decisions, tonal exceptions and resolutions, motif transformations, groove,
bass or supporting-line roles and timbral decisions where those roles are
present in the approved runtime plan, and the concrete choices each axis
influenced. Expose the same underlying analysis in structured diagnostic logs
or review artifacts so technical evaluation does not depend on copying teaching
text from the popup.

## Tonal representation

- Give Roman numerals one conventional tonic-relative interpretation.
- Measure unaltered degrees against the major reference scale and apply written
  accidentals exactly once.
- Store chord quality explicitly rather than deriving it ambiguously from the
  current mode.
- Give each progression family an explicit pitch collection/mode and supported
  modes.
- Tag every style-native exception, such as a blues dominant seventh, raised
  minor-key leading tone, borrowed minor iv, or rock `bVII`.
- Separate collection membership from chord membership.
- Define preparation/resolution requirements for melodic non-chord tones.
- Reject an invalid or unsupported style/mode/profile combination instead of
  silently changing it.

`Auto` chooses among modes supported by the selected profile. Manual mode
override filters the available profiles/progressions to compatible ones.

## Meter and time representation

If Phase 1B approves time-model enhancements, represent meter, beat unit,
subdivision, swing/time feel, and perceived half-time or double-time explicitly
where each distinction changes generation, teaching, playback, or diagnostics.
Keep the implementation no broader than the retained profiles require.

`Auto` chooses among the selected profile's supported time models. A manual
choice filters eligible profiles and grooves. Do not relabel an incompatible
4/4 pattern as compound meter, force a native compound or triple-meter practice
onto 4/4, or change the requested time model silently.

## Long-form planner

Create a phrase plan before generating individual chords or notes. It controls
all musical layers. The strategies below are a shared structural vocabulary;
only use a strategy and length when the approved profile supports them.

### Four-phrase arc

- Phrase 1: establish the profile and motif without a full cadence.
- Phrase 2: related answer or development with a different boundary function.
- Phrase 3: contrast, tension, register/texture shift, or harmonic departure.
- Phrase 4: return and prepare the loop restart.

### Two eight-bar periods

- First period: antecedent with open or midpoint arrival.
- Second period: consequent that develops rather than copies the first.
- The final bar normally turns toward bar 1; if a tonic arrival occurs, avoid
  duplicating static tonic harmony across bars 16 and 1.

### Continuous evolving loop

- Avoid artificial four-bar cadences.
- Develop through harmonic rhythm, bass, motif, register, density, subdivision,
  texture, and groove.
- Place a clear long-range peak or contrast and a controlled reduction or
  pickup before the restart.

Adapt the strategies deliberately to the chosen native length, including 4, 8,
12, 16, or any other researched bar count where appropriate. Do not truncate,
stretch, or modulo-repeat a progression merely to reach a standard length.

Length selection must follow the same compatibility principle as manual mode
selection. `Auto` may choose a profile-native length; a manual length narrows
the eligible profiles and structural strategies. Reject or clearly surface an
unsupported combination rather than silently substituting another form.

## Future section continuation compatibility

Automatic continuation of a user-authored bank is a later possibility, not a
required part of the initial redesign. Preserve the researched continuation
relationships so a later system can:

- Analyse the supplied section's observable key or collection, harmonic
  motion, groove, density, motif, phrasing, and boundary behaviour.
- Propose a related A-prime or contrasting B section from those observations.
- Use compatible profile knowledge for groove and general musical practice
  without requiring an exact style classification.
- Display any inferred style/profile and uncertainty, allow the user to
  override it, and keep the concrete reasoning inspectable.
- Preserve important identity while applying an idiomatic amount of contrast
  and preparing a useful return.

## Required musical layers, views, and timbres

Dedicated bass, supporting pitched lines, their editing/view representation,
profile-required timbres, and characteristic percussion are part of the
approved redesign. Phase 1B must specify a compact implementation that
separates:

- Chord identity and voicing from the bass line sounding beneath it.
- Main melody from parallel harmony, countermelody, response, pad, or riff
  support.
- Musical-role selection from the timbre used to render that role.
- Profile-defining timbre from seed-driven colour variation.

The research must decide how many supporting lines are needed, when each role
is applicable, how they are exposed without cluttering the primary workflow,
and how every generated or edited event is stored, taught, diagnosed, and
rendered. Do not assume every bass note is a chord root or every supporting tone
is a block chord. Synthesis and percussion recipes must remain compact,
deterministic, inspectable, and realistic for Jam2's local audio engine.

## Harmony planner

- Select a profile-compatible progression family and instantiate it according
  to the profile's phrase roles or native continuous-development behaviour.
- Use statement, answer, contrast, return, and loop-back variants only where the
  profile and selected form support those roles. For static, oscillating,
  riff-centred, or continuously evolving profiles, use researched changes in
  activation, subtraction, register, bass, harmonic rhythm, texture, or tension
  instead of imposing functional phrase roles.
- Make bass movement and inversions serve the multi-bar line.
- Permit internal tonic chords when stylistically appropriate, but do not force
  tonic onto every fourth bar.
- Ensure advanced operations occur at meaningful locations and reach the
  destination described in the recipe.
- Follow the approved complexity ladder. Keep the foundational stage within its
  declared conservative collection and simple voicing grammar; unlock
  collection-preserving colour, directed tension, mixture, substitution, or
  other tools only at the researched stages and only in profile-native ways.

## Motif and melody planner

- Generate a profile-compatible motif unit with recognisable interval and
  rhythmic identity. Its length may be shorter than a bar or span several bars
  when research supports that phrase, riff, pickup, or loop behaviour.
- In vocal-centred profiles, let the instrumental melody follow plausible
  vocal phrase behaviour, including pickups, repetition, breathing space,
  singable range, and room for profile-appropriate sustained or melismatic
  gestures.
- Develop it according to the phrase plan through controlled repetition,
  answer, sequence, displacement, truncation, augmentation/diminution where
  appropriate, register transfer, rests, and cadence alteration.
- Use chord and collection constraints as hard eligibility rules before
  candidate scoring.
- Make strong structural notes fit a declared role: chord tone, supported
  extension or stable tension, collection-based target, prepared suspension, or
  explained profile-native exception.
- At foundational complexity, keep notes within the profile's declared
  diatonic, modal, blues, or other native collection. Weak-position passing and
  neighbour tones must follow the profile's conservative preparation and
  resolution rules. Do not prohibit an essential blues or modal pitch merely
  because it is not part of a major/minor diatonic scale.
- At higher complexity, tie enclosures, chromatic approaches, guide tones,
  extensions, and altered notes to declared targets.
- Score motif recognisability, phrase contrast, contour, range, breathing
  space, groove alignment, cadence direction, and final-bar-to-bar-1
  continuity.
- Do not claim an `A/B/C` form unless the actual event relationships satisfy
  that form.

## Groove planner

- Preserve useful authored two-bar cores only after they have passed research
  review.
- Protect essential kick, backbeat, ride/hat, and pedal anchors.
- Generate phrase-level A/A-prime/B/turnaround or continuous-development
  behaviour from the shared form plan.
- Let all complexity levels receive musical variation. Complexity changes the
  available vocabulary and frequency, not the existence of development.
- Keep maximum hit growth, active-limb, cymbal-overlap, timing, and velocity
  bounds inspectable.
- Make fills, cymbal changes, rolls, ghost notes, and kick alternatives respond
  to phrase roles and profile practice rather than a generic four-bar timer.
- Coordinate chord attacks and melody gaps with the groove instead of
  generating each layer independently.

# Phase 3 — Implementation sequence

Implement in the following order so tonal and structural invariants exist before
style content is expanded.

1. **Research recipe and request model**
   - Add stable mode/profile/form/variation fields for generator version 6 and
     the researched explicit meter/time-model fields.
   - Encode the approved complexity ladder directly in the version-6 recipe.
   - Remove Mood/Character records, controls, transforms, labels, and test APIs
     completely.
   - Reject all earlier generated recipe versions. There is no compatibility
     mapping, historical mood round-trip, or fallback generation path.
   - New generation writes only version 6.
2. **Tonal core**
   - Replace the ambiguous degree calculation.
   - Add pitch-collection, chord-quality, exception, and resolution types.
   - Add exhaustive unit/boundary validation before migrating profiles.
3. **Profile catalog**
   - Encode the final profiles and required redesign capabilities approved at
     the Phase 1B checkpoint.
   - Bind progressions, grooves, tempo, modes, variation axes, and form weights.
   - Keep only genuinely future continuation or out-of-scope research out of
     recipes and runtime structures until a later implementation needs it.
   - Remove unsupported or misleading legacy material.
4. **Long-form harmony**
   - Build phrase plans and form-aware harmony for all supported lengths.
   - Add explicit ending and loop-back roles.
5. **Motif and melody**
   - Replace cosmetic form labels with motif-to-phrase development.
   - Add hard tonal eligibility and resolution validation.
6. **Groove and comping**
   - Apply the shared phrase plan to drums and chord articulation.
   - Retain density and physical-playability limits.
7. **Meter, musical layers, views, and synthesis**
   - Implement the researched explicit meter/time model and unrestricted
     profile-native length representation.
   - Add dedicated bass plus the role-aware harmony, countermelody, response,
     pad, riff, or other supporting lines approved for individual profiles.
   - Add their compact editing/view representation, recipe fields, diagnostics,
     teaching data, and reference rendering.
   - Add the profile-required timbre, articulation, percussion voices, and
     per-lane timing behaviour defined by Phase 1B.
8. **Idea Details and documentation**
   - Make the default popup teach what the idea is doing, how to jam with or
     reuse its concepts, and how the selected complexity expanded it.
   - Add a **Detailed Analysis** control for the full inspectable profile,
     variation, form, cadence, tonal exception, motif, groove, and any generated
     layer or timbral plan.
   - Emit the same raw analysis through structured diagnostic logs or review
     artifacts.
   - Update user documentation and the research citations and decision notes.

Keep the implementation concentrated in the existing generator, recipe/dialog,
and boundary-validation areas unless research demonstrates that a small local
module split reduces real complexity.

# Phase 4 — Validation and listening

## Automated tonal and structural tests

Add deterministic coverage for:

- All twelve tonics and every supported mode.
- Every approved style/profile/mode combination.
- Every approved profile/meter/subdivision/time-feel combination.
- Every supported native length and its significant phrase boundaries.
- Every approved complexity stage and its cumulative eligibility rules.
- Conventional degree realisation, including regressions such as C-minor
  `bVI = Ab`, `bIII = Eb`, Mixolydian `bVII`, and Phrygian `bII`.
- Chord-tone and collection membership.
- Explicit style-native exception tags.
- Preparation and resolution of non-chord tones.
- Secondary-dominant, borrowed-chord, substitution, and modulation targets.
- Native-length selection and the absence of modulo truncation.
- Phrase-role/cadence-role agreement with generated harmony.
- Final-bar-to-bar-1 loop function.
- Motif similarity for related phrases and measurable contrast for B material.
- Profile-compatible progression/groove/mode/tempo choices.
- Seed determinism, bounded density, limb feasibility, and valid event shapes.
- Version-6 recipe/control-message round trips and rejection of every earlier
  generated-recipe version.

Tests must not require every style to use functional cadences. Static,
oscillating, modal, funk, hip-hop, and electronic profiles need
profile-appropriate structural assertions.

## Fixed-seed listening suite

Add a bounded diagnostic path through the existing `jam2` executable and test
tooling, not a second executable.

The suite should:

- Render a representative set of original previews covering every approved
  public style, its important profiles, native lengths, and compatible
  structural strategies. Determine the case count after the profile catalog is
  approved; do not force an incompatible form or add weak cases to meet a
  quota.
- Use predetermined seeds derived mechanically from stable case IDs containing
  the public style, profile, native length, structural strategy, and case
  purpose rather than from mutable display order or cherry-picked successful
  outputs.
- Cover simple, intermediate, and advanced complexity behaviour across the
  suite, with focused cases for profile-specific transitions and techniques.
- Cover every supported manual mode across the full suite and add focused cases
  where a profile has a unique tonal language.
- Render a balanced 48 kHz audition mix plus recipe JSON for each case.
- Emit an index and review CSV containing the style, profile, seed, mode,
  complexity, form, variation plan, and checklist fields.

Use this checklist during ongoing listening and discussion:

- No unexplained wrong note or chord.
- Convincing tonal centre or deliberate tonal ambiguity.
- Recognisable and coherent style/profile.
- Audible motif identity and purposeful development.
- Distinct long-range phrase flow.
- Harmony, groove, and melody that sound conceived together.
- Bass movement and any supporting line reinforce the groove, harmony, and
  melody without unnecessary collisions.
- Timbres and articulations support the profile recognisably within Jam2's
  synthesis limits without substituting surface sound for musical accuracy.
- Appropriate simplicity at low complexity.
- Controlled rather than arbitrary colour at high complexity.
- A convincing transition from the final bar to bar 1, or a convincing closed
  ending when the case explicitly calls for one.
- No repeated full cadence every four bars unless the approved profile
  explicitly requires that effect.

An observed musical-quality failure is a real failure even when all automated
invariants pass. Record its seed, diagnosis, responsible rule, and correction
in this document or the suite review artifact. Do not block unrelated progress
while waiting for formal listening acceptance.

## Build and product validation

After implementation:

1. Inspect the dirty worktree and preserve unrelated changes.
2. Build from the repository root with elevated
   `cmd.exe /d /c "call compile.cmd --in-dev-shell"`.
3. If `release/jam2.exe` cannot be replaced because it is running, close that
   process and rerun the normal command.
4. Confirm the command exit code and compiler output.
5. Run the deterministic Jam2 validation against `release/jam2.exe`.
6. Run the fixed-seed musical listening suite.
7. Report automated results and unresolved listening observations separately.

# Completion criteria

This plan is complete only when:

- All material Phase 1 findings and their supporting citations are present in
  this file and the final catalog has been discussed.
- Every retained style has a defensible internal profile catalog.
- The global Mood/Character generator has been replaced by inspectable
  style-guided seed variation.
- Foundational-complexity output has no unexplained tonal exceptions.
- The approved complexity ladder exposes broadly consistent musical concepts
  across the catalog while every profile realizes them idiomatically and no
  unlocked technique is forced into every seed.
- Every retained structural strategy generates the materially different,
  audible structure it claims, and profiles use only compatible strategies.
- Output develops across its chosen native span and normally loops through a
  purposeful final-bar lead-in where an open loop is appropriate.
- Every required generated layer shares one form and arrangement plan, including
  dedicated bass and any applicable harmony, countermelody, response, riff,
  pad, or comping role.
- Approved profile-native meters, beat groupings, click behaviour, subdivisions,
  and form lengths are represented directly rather than approximated through
  the old `N/4` and 4/8/12/16 restrictions.
- Profile-required timbres, articulations, percussion voices, and per-lane
  timing behaviour are compact, deterministic, inspectable, and audible.
- The default Idea Details view teaches what the generated idea is doing, how
  to jam with or reuse its concepts, and how complexity changed it; Detailed
  Analysis and structured diagnostics expose the complete raw plan.
- User-authored section continuation remains clearly separated as future work;
  the approved meter, dedicated-layer, view, timbre, and percussion scope is
  implemented without unused fields for unrelated deferred features.
- Earlier generated-recipe versions are rejected; manual projects without
  generated metadata continue to load.
- Automated validation passes.

# Research and decision log

Add dated entries here throughout the work. Do not rely on chat history as the
only record of why the catalog changed.

| Date | Phase | Style/profile | Finding or decision | Evidence | Consequence |
|---|---|---|---|---|---|
| 2026-07-28 | Planning | All | Replace global Mood with style-guided deterministic variation and research the current style catalog before implementation. | User decision and current implementation audit. | Phase 1 is mandatory and implementation-gated. |
| 2026-07-29 | Planning | Catalog | Treat the twelve current styles as a starting set rather than a required final catalog. Evaluate retaining, changing, removing, and adding styles during research. | User direction. | Phase 1A provisionally approves candidates for detailed study; Phase 1B approves the final implementation catalog and may revise that provisional set. |
| 2026-07-29 | Planning | Profiles and forms | Keep profiles compact and high quality without a numerical cap; add one only for a genuine musical-grammar difference. Use native lengths and research-determined form compatibility rather than forcing sixteen bars or every strategy. | User direction. | Research and listening coverage will be evidence-led instead of quota-led. |
| 2026-07-29 | Planning | Teaching and melody | Make Idea Details a practical teaching surface, and use vocal-derived instrumental phrasing in vocal-centred profiles. | User direction. | Profile research must include accessible concept explanations and applicable melodic phrase practice. |
| 2026-07-29 | Planning | Section flow | Research A-to-A-prime and A-to-B relationships for a possible future continuation feature. Follow the supplied material first and use style inference as visible, overridable guidance. | User direction. | Profiles will record section-flow relationships without adding continuation to the initial implementation scope. |
| 2026-07-29 | Planning | Bass, support, and timbre | Research dedicated bass movement, melodic harmony/supporting lines, and feasible style-bearing synthetic timbres. | User direction. | Superseded by the approved Phase 1A hard-scope decision below: these become required redesign capabilities with compact views, editing, recipes, teaching, diagnostics, and rendering. |
| 2026-07-29 | Planning | Scope and inspection | Make length selection compatibility-based and Idea Details teaching-first with expandable raw analysis and matching structured diagnostics. | User direction and context-free plan audit. | Phase 2 encodes the final researched catalog and required redesign capabilities; only user-authored continuation and newly discovered out-of-scope features remain future work. |
| 2026-07-29 | Planning | Complexity | Research a shared progression of musical tools whose concrete realization is profile-dependent; treat techniques as unlocked rather than forced and reconsider the current eight-level count. | User direction. | Phase 1 will propose the stage count, ordering, profile mappings, and teaching content. |
| 2026-07-29 | Planning | Evidence and listening | Use ordinary citations without a formal source ledger, and use listening as ongoing evidence without a user-acceptance gate. | User direction. | The initial source list and mandatory per-case acceptance requirement were removed. |
| 2026-07-29 | Planning | Time and feasibility | Research meter, subdivision, time feel, and native form length while auditing current time, synthesis, layer, view, recipe, and diagnostic capabilities. | User direction and plan audit. | Superseded by the approved Phase 1A hard-scope decision below: explicit meter/grouping and unrestricted researched lengths are required redesign capabilities. |
| 2026-07-29 | Phase 1A | Capability baseline | The present engine has useful chord, monophonic melody, drum-grid, timing, seed, recipe, and compact synthesis primitives, but no profile model, independent bass/supporting lines, explicit meter denominator/grouping, or genuine long-range form planner. | Direct audit of `PracticeIdeaGenerator`, `GenerationRecipe`, `BeatGridModel`, `PracticeIdeaDialogs`, `PracticeReferenceRenderer`, and their validation paths. | The missing meter, form, bass, supporting-line, view, timbre, and percussion capabilities must be researched and implemented compactly. |
| 2026-07-29 | Phase 1A | Approved catalog and hard scope | Approve thirteen research labels: Pop, Rock, Jazz, Modal Jam, Blues, J-Pop / Anisong, Country, Electronic, R&B / Soul, Funk, Hip-Hop / Trap, Reggae, and Bossa Nova. Merge Indie ideas into coherent Rock, Pop, or Modal profiles; broaden Jazz research to Swing/Standards, Bebop, and Fusion; approve the three proposed renames. | User approval following the Phase 1A academic/corpus survey and Jam2 capability review. | Phase 1B may begin. Explicit meter/native lengths, dedicated bass, role-aware harmony/countermelody/supporting lines, profile-level form, and profile-required timbre/percussion are hard redesign plans, not optional deferrals. |
| 2026-07-29 | Phase 1A addendum | Modern Progressive Metal / Metalcore | Add a narrow Phase 1B feasibility study centred on Spiritbox, ERRA, and Nick Broomhall rather than attempting to represent the full Metal family. Treat sound design and musical grammar as one research problem because distortion, articulation, tuning, bass, and production materially change perceived tension and weight. | User direction; contemporary Metal time-feel/riff and production scholarship; artist production and songwriting references. | Prototype and evaluate compact original riff generation plus synthetic plucked attack, mute/open articulation, amp/distortion/cabinet behaviour, low-register bass, modern drums/percussion, and ambient contrast. Promote it to the public catalog only if the result is musically and sonically honest. |
| 2026-07-29 | Phase 1B | Final profile catalog | Detailed research supports thirteen public styles containing 26 normal internal profiles. Lo-Fi and Synthwave remain typed production/timbre families; Indie remains reassigned. | Individual briefs, cited corpora/performance research, cross-style harmony/groove/bass/form comparison. | Present this catalog at the final Phase 1B checkpoint; do not implement before discussion. |
| 2026-07-29 | Phase 1B | Time, form, and layers | Honest representation requires explicit meter/beat unit/grouping, arbitrary native spans, profile form, dedicated bass, chordal comping, one editable role-aware supporting line, and per-lane timing. | Cross-profile meter/form matrices; Jazz, Bossa, Country, Reggae, Funk, R&B, Hip-Hop, Electronic and Metal evidence. | Treat these as one smallest coherent redesign rather than optional follow-ups. |
| 2026-07-29 | Phase 1B | Complexity | Retain eight levels as an ordered curriculum of unlocked musical tools, with profile-native exceptions and realizations; no stage forces every newly available technique. | User direction reconciled with all detailed profile grammars. | Map existing `1…8` inputs directly, version the model, and log available versus selected techniques. |
| 2026-07-29 | Phase 1B | Legacy generator | Current progression/groove material can seed several new families, but fixed counts, Mood-to-mode/cadence transforms, modulo form, forced tonic endings, generic riff-as-chord treatment, and global swing are incompatible with the research. | Direct code audit plus Phase 1B synthesis. | Route or correct named legacy ideas and remove the governing global rules/count tests in Phase 2. |
| 2026-07-29 | Phase 1B | Modern Progressive Metal / Metalcore | The narrow compositional grammar is defensible, but public inclusion cannot be justified from notation while the current renderer lacks intelligible low-register articulation and production contrast. | Metal time-feel/form scholarship, distortion research, and Spiritbox/ERRA/Broomhall references. | Keep one research-complete experimental profile; decide public promotion only after the specified original four-stage sound test and user listening discussion. |
| 2026-07-29 | Implementation | Version-6 generator | Implemented the approved 13-style/26-profile catalog, experimental Metal profile, profile-native forms and meter, shared complexity curriculum, independent bass/support roles, ten percussion lanes, style-filtered tonal tools, seed-derived profile variation, typed production families, numeric synthesis/articulation/timing/automation recipes, five-layer reference rendering, and teaching-first Idea Details with Detailed Analysis. Removed Mood/Character records and all earlier generated-recipe compatibility. | Release build plus deterministic boundary, controller-lifecycle, local-headless, and network-lifecycle validation. | The generator redesign is available for normal listening-led refinement. Metal remains explicitly experimental; user-authored A-to-B inference remains separate future work. |
| 2026-07-29 | Correction | Tempo, meter, and naming | A 12/8 Shuffle / Blues Rock seed exposed that a hidden legacy intensity offset and meter multiplier could turn the researched 60–180 range into values as high as 291 BPM, while the clock treated every written eighth as a full BPM pulse. | Reproduction of the reported 282 BPM result, code-path audit, and 256-seed 12/8 regression. | BPM is now sampled directly inside the selected profile range; written beat unit and tempo pulse are separate; compound meters use a three-eighth dotted-quarter pulse and grouped click; titles contain tonic, mode, profile, and meter facts only; Mood/Character generation and versions 1–5 are absent from the active recipe path. |
