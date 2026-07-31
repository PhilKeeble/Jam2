# Jam2 Symbolic Generation Audit

## Goal

Validate and refine the current production generator one style at a time across
all 27 profiles. Each review uses several deterministic generations per native
form and complexity level, while treating the complete arrangement as the unit
of musical judgement.

This pass covers:

- cross-seed variation without erasing profile identity;
- chord vocabulary, harmonic rhythm, tonal motion, and form;
- melody, bass, supporting-role, and drum behaviour;
- phrase and section development over the complete native form;
- interaction among lanes;
- comparison of generated drums with relevant real-drummer MIDI evidence.

Instrument timbre, synthesis quality, and mix balance are explicitly outside
this audit.

## Evidence rules

1. Exact and transposition-normalized duplicates are reported separately.
   Reusing a style-native progression or groove is not itself a failure.
2. Pairwise similarity is exposed as raw data by lane. It is not converted into
   an opaque quality score.
3. Magenta Groove MIDI Dataset comparisons are direct only where its label and
   population reasonably match the Jam2 profile. Related labels are marked as
   proxies, not presented as proof of profile authenticity.
4. Dataset similarity is descriptive. Jam2's researched profile specification,
   native form, and musical coherence remain authoritative.
5. A rule changes only when a reproducible generation, corpus outlier, or
   research contradiction identifies a concrete problem.
6. Each changed profile is regenerated and reassessed before moving on.

## Reproducible corpus

The production `validate.music-full-form-corpus` diagnostic currently provides:

- every public and experimental profile;
- every native form;
- complexities 1, 4, and 8;
- four deterministic seeds per profile/form/complexity cell;
- complete recipes, chord voicings, lane events, drum-grid hits, and exact
  expressive drum events.

The initial corpus therefore contains 888 complete symbolic ideas. Four seeds
per fixed profile/form/complexity cell are the primary within-profile comparison
set; the other forms and complexity levels test whether variation remains
coherent across the complete profile boundary.

## Audit outputs

`tools/music_research/analyze_jam2_generation.py` writes:

- machine-readable profile, group, lane, and evidence measurements;
- a style-ordered Markdown review board;
- exact raw and normalized duplicate pairs;
- progression, groove, motif, mode, tonic, and arrangement-role coverage;
- pairwise Jaccard similarity distributions for harmony, melody, bass,
  supporting roles, and drum placement;
- event-density, pitch-range, section-change, kick/bass alignment, and
  melody/support collision measurements;
- research-contract violations;
- bounded Magenta comparisons with source-relationship labels.

Generated reports remain under `artifacts/music-research/`.

## Style review order

The review proceeds through the catalogue in this order so closely related
profiles can be compared together:

1. Pop
2. Rock
3. Jazz
4. Modal Jam
5. Blues
6. J-Pop / Anisong
7. Country
8. Electronic
9. R&B / Soul
10. Funk
11. Hip-Hop / Trap
12. Reggae
13. Bossa Nova
14. Modern Progressive Metalcore

## Working log

### 2026-07-31 — Baseline framework

- Confirmed 27 profiles across 14 catalogue review groups and 74 native forms.
- Confirmed the existing 888-idea corpus contains the complete symbolic recipe,
  not only drum-grid data.
- Kept the earlier drum analyzer as the detailed performance-event source and
  began a separate full-arrangement analyzer instead of overloading drum
  repetition metrics with harmonic and melodic assumptions.
- Established direct/proxy/unsupported evidence labels for Magenta comparison;
  sparse modern electronic, Trap, Metal, odd-meter, and atmospheric material
  will not be represented as directly covered by GMD.

