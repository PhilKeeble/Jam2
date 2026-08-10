# JamTaster POC benchmark notes

Measured on 2026-08-10 with an NVIDIA GeForce RTX 4090 and the pinned CUDA 12.8 environment. Runtime JSON remains under `reports/` (ignored by Git); this file keeps the decision summary.

## Fixture

`Calm_Satellite` bank A was generated natively by Jam2 at 98 BPM in 4/4. The benchmark command mixed its first eight bars (19.592 seconds) from the authored chord, drum, melody and bass assets. Jam2's generated recipe supplied exact beat/downbeat, chord, drum and bass truth. Source assets also supplied the four separation targets (`other` is chord plus melody; `vocals` is silence).

All event F1 figures use explicit tolerances: 70 ms beats, 100 ms downbeats/bass, and 50 ms drums.

| Run | Change | Pipeline s | RTF | Beat F1 | Beat MAE ms | Chord exact | Chord root | Drum F1 | Drum F1, truth velocity >= 40 | Bass F1 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| I2 | Baseline after report fix | 16.670 | 0.851 | 0.985 | 7.10 | 0.000 | 0.059 | 0.498 | n/a | 0.941 |
| I3 | Conservative chord+bass fusion; `0.12,0.16,0.22,0.12,0.20` drums | 16.685 | 0.852 | 0.985 | 7.10 | 0.867 | 0.926 | 0.541 | n/a | 1.000 |
| I4 | More aggressive snare/tom/hat/cymbal thresholds | 16.430 | 0.839 | 0.985 | 7.10 | 0.923 | 0.923 | 0.545 | 0.633 | 1.000 |
| I5 | Stabilized chords; balanced `0.12,0.16,0.22,0.05,0.20` drums | 16.673 | 0.851 | 0.985 | 7.10 | 0.916 | 0.916 | 0.564 | 0.667 | 1.000 |

I5 was the selected pre-dynamics baseline. The later 32-bar work below supersedes its drum defaults.

I5 drum-family F1 was kick 0.870, snare 0.722, hi-hat 0.340 and tom 0.429. The five-class ADTOF model did not recover the fixture's two cymbal events. The all-event drum score includes very quiet authored ghost notes; the velocity-banded score does not erase them from raw truth and simply reports an additional explicit view.

The I2 separation scores were 12.43 dB SI-SDR for drums, 13.54 dB bass and 20.04 dB other. Correlations were 0.975, 0.987 and 0.996 respectively. With silent vocal truth, the separated vocal lane measured -43.71 dBFS RMS instead of SI-SDR.

SongFormer took 7.26 seconds on the short fixture and produced a single verse plus invalid-division warnings internally. Structure was not scored because an eight-bar loop has no intro/verse/chorus/bridge ground truth. It remains enabled for full songs, where its result can be evaluated by inspecting the generated form.

## 32-bar drum dynamics and fill work

Two native Jam2 grooves were expanded to 32 bars. `Wandering_Nova` is a dense 4/4 Anisong Rock groove with 668 grid notes and eight authored fill regions. `Running_Crater` bank B is a sparse 2/4 Bossa groove with 76 grid notes, using only kick and cross-stick/rim; it deliberately exercises a very different failure mode.

| Fixture/run | Pipeline s | Hit F1 | Precision | Recall | State accuracy | Repair precision | Fill F1 | Fill recall | Non-fill F1 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Wandering I1, velocity-discarding baseline | 33.380 | 0.949 | 0.942 | 0.957 | 0.714 | 0.500 | 0.351 | n/a | 0.984 |
| Wandering I7, selected | 13.353 | 0.967 | 0.966 | 0.969 | 0.841 | 0.900 | 0.694 | 0.806 | 0.983 |
| Wandering I8, exact-grid export | 13.450 | 0.970 | 0.967 | 0.973 | 0.837 | 0.864 | 0.722 | 0.839 | 0.984 |
| Bossa I1, tom/rim confused | 11.382 | 0.343 | 0.252 | 0.539 | 0.293 | 0.029 | n/a | n/a | 0.357 |
| Bossa I6, selected rim mode | 11.117 | 0.806 | 0.931 | 0.711 | 0.889 | n/a | n/a | n/a | n/a |

The selected dense-groove pass raised fill F1 from 0.351 to 0.694 and state accuracy from 0.714 to 0.841 without weakening the ordinary groove. Verified transient-tom repairs were the reliable fill mechanism; broad low-threshold fill repair was removed. A narrow tom-over-hi-hat de-duplication removed double-labelled fill onsets. Four-four snare states use the tracked beat/downbeat position because ADTOF exports constant MIDI velocity and snare-band energy is badly contaminated by simultaneous hats and kicks.

The sparse Bossa fixture caught an overfit: its rim sample activates ADTOF's tom, hi-hat and kick classes simultaneously. Persistent tom-like activity with more than twice the conventional snare count now enters an explicit rim mode. It maps the sustained class to Jam2's cross-stick lane, retains only the dominant repeating kick position, removes same-onset lane duplicates and disables speculative repair. Demucs remains the limiting stage for this fixture: its separated drum stem measured -2.20 dB SI-SDR and 0.611 correlation. Even direct ADTOF analysis of the untouched truth drum stem produced duplicated lanes, confirming that this case is not repair-induced.

`--sections single` now skips SongFormer. This reduced the 65.085-second Wandering run from 33.38 seconds to about 13.35 seconds; drum analysis itself took about 1.73 seconds, including transient corroboration.

The selected drum defaults are thresholds `0.12,0.16,0.12,0.05,0.20`, candidate scale `0.30`, 4-way subdivision, ghost/accent energy ratios `0.50/0.85`, and three supporting bars for repetition repair.

## California acceptance run

Input: `release/songs/White_Solstice/recorded/california-loopback.wav`, mono PCM16, 48 kHz, 113.268 seconds.

The final dynamics-and-repair run took 24.211 seconds (0.214 RTF). Major stages were Demucs 7.236 s, Beat This 0.623 s, SongFormer 8.119 s, ChordMini 3.916 s, drum analysis 1.231 s, drum repair 0.009 s, Basic Pitch 1.048 s and JamJar export 0.117 s.

Beat This detected 96.774 BPM; the accepted native Jam2 grid is 97 BPM in 4/4. SongFormer returned intro, three verse spans, chorus and outro. JamTaster selected:

- A / Bookend: 20 beats, reused for intro and outro.
- B / Verse: 40 beats, reused three times.
- C / Chorus: 32 beats.
- D: unused.

The arrangement is A, B x3, C, A. The analysis contained 68 chord segments, 849 drum hits and 321 bass notes. Of the drum notes, 22 were repetition repairs and six were independently verified transient-tom repairs. Beat/downbeat-relative dynamics produced 84 accented and 229 ghost snares across the full analysis instead of accumulating position error from the rounded 97 BPM clock. Fixed-grid/audio drift (audio is deliberately not stretched) is +248.866 ms for A, +237.732 ms for B and +146.186 ms for C.

The copy-ready `California_Drums_Final` output contains one 335,177-byte JamJar and 12 mono PCM16 imported assets. Final validation found zero missing files, hash mismatches, WAV metadata mismatches, forbidden lane paths or unexpected top-level entries.

## Bar-boundary and exact-grid follow-up

The I8 Wandering run verified the new export path against the known 32-bar truth. Its authored 118 BPM source already occupied exactly 128 beats, so Signalsmith correctly remained an exact-copy path. Export took 0.079 seconds and the remaining WAV-frame rounding error was -0.007 ms. The drum metrics in the table show no regression from changing the section/export path.

`California_Grid_Aligned` repeated the full-song acceptance run with complete tracked-downbeat sections, exact-grid stretching and the arrangement saved enabled. The selected banks remained A/bookend 20 beats (2.36-14.98 s), B/verse 40 beats (40.12-65.10 s), and C/chorus 32 beats (84.80-104.74 s). Every end is a tracked downbeat and every length is a complete number of 4/4 bars.

Signalsmith shortened A by 2.0117%, B by 0.9608%, and C by 0.7386%, without changing pitch. Source drift of +248.866, +237.732 and +146.186 ms became exported drift of -0.009, +0.003 and -0.002 ms respectively. Each bank's four stems has one identical exact frame count. The arrangement remains A, B x3, C, A and is armed automatically when opened by the updated Jam2.

This run took 25.354 seconds (0.224 RTF): Demucs 6.974 s, Beat This 0.590 s, SongFormer 8.022 s, ChordMini 3.838 s, drum analysis 1.178 s, drum repair 0.009 s, Basic Pitch 1.107 s and JamJar export including 12 Signalsmith stretches 1.701 s. The 335,200-byte JamJar and 12 imported PCM16 assets passed path, hash, WAV metadata and exact-frame validation with zero failures.

## Chord and bass placement iterations

The harmonic pass used the same generated Bossa, Wandering and Calm fixtures rather than tuning only against California. Two proposed recovery methods were discarded: pYIN reduced Bossa bass F1 to 0.231 and added 9.13 seconds; unrestricted Basic Pitch on the full mix reduced it to 0.249 by transcribing chord overtones. A full-mix recovery constrained to the register already established by the separated bass stem was retained. A regularity gate was added after a Calm regression exposed that a correct one-note-per-bar line must not be treated as missing content.

| Fixture/run | Pipeline s | Chord exact | Chord root | Bass F1 | Bass precision | Bass recall | Bass MAE ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| Bossa pre-harmonic baseline I6 | 11.117 | 0.032 | 0.032 | 0.427 | 0.760 | 0.297 | 39.0 |
| Bossa pYIN experiment I1 | 20.360 | 0.116 | 0.242 | 0.231 | 0.300 | 0.188 | n/a |
| Bossa unrestricted full-mix I2 | 11.792 | 0.115 | 0.318 | 0.249 | 0.190 | 0.359 | 47.6 |
| Bossa selected constrained I3 | 11.890 | 0.437 | 0.453 | 0.505 | 0.774 | 0.375 | 45.2 |
| Wandering held-out I9 | 13.517 | 0.865 | 0.882 | 0.802 | 0.898 | 0.725 | n/a |
| Calm held-out I7 | 10.529 | 0.929 | 0.929 | 0.941 | 0.889 | 1.000 | 7.1 |

The Bossa chord fallback is a local beat-synchronous chroma/template classifier used only when ChordMini covers less than half the song. Bass-root fusion is restricted to simple triads and seventh chords; retaining extensions such as inferred ninths raised root agreement but harmed exact Jam2 chord symbols. On ordinary ChordMini-covered material the fallback remains off.

Musical export now maps every chord and bass onset through Beat This's individual source beat intervals before snapping to Jam2's quarter-beat cells. The Bossa I3 exported bass residual averaged 30.84 ms with a 95.38 ms maximum, without the cumulative end-of-section offset caused by sampling a uniform 124 BPM source clock. A synthetic irregular-beat regression test verifies exact cell placement.

## California full-form acceptance run

`California_JamTaster_Full_Form` is the final A-L-capable export of the 113.268-second loopback. It took 26.738 seconds (0.236 RTF): Demucs 7.096 s, Beat This 0.532 s, SongFormer 8.082 s, ChordMini 3.846 s, drum analysis 1.200 s, drum repair 0.010 s, Basic Pitch 0.831 s, and JamJar export plus 24 Signalsmith stretches 3.230 s.

SongFormer returned six chronological segments and all six were retained: A intro (20 beats), B verse (40), C verse (40), D verse (32), E chorus (32), and F outro (12). Their tracked-downbeat boundaries are 2.36, 14.98, 40.12, 65.10, 84.80, 104.74 and 112.20 seconds. The enabled arrangement is A, B, C, D, E, F, each once. The 621,061-byte JamJar contains six matching looper banks and 24 imported PCM16 stem assets.

Beat This again measured 96.774 BPM and the Jam2 grid is 97 BPM in 4/4. Signalsmith reduced per-bank source drift of +248.866, +397.732, +237.732, -93.814, +146.186 and +37.320 ms to residual exported drift between -0.009 and +0.007 ms. Analysis retained 69 chord segments, 852 drum hits and 312 bass notes. ChordMini coverage was 99.96%, so the chroma fallback stayed off; bass-stem density was sufficient, so full-mix bass recovery also stayed off.

## Context and labelled-corpus iteration

All accuracies and F1 values below are proportions on a `0.0` to `1.0` scale. These reference runs exercise the analyser directly on the labelled task audio; they do not include Demucs separation error unless explicitly described as a Jam2 full-mix fixture.

The installed inventory contains 360 GuitarSet audio/JAMS pairs (285 development, 75 held out), 48 FiloBass audio/aligned-MIDI pairs (38/10), 23 MDB Drums audio/subclass pairs (16/7), and the local Groove MIDI corpus with 1,090 WAV and 1,150 MIDI files. HarmonixSet has annotations but no bundled matching audio, so it is not counted as an accuracy corpus. Dataset identifiers use a deterministic SHA-256 80/20 split.

### Chords

GuitarSet development input experiments rejected HPSS as ChordMini's primary feed: mean root accuracy fell from 0.391 to 0.241 on the five-track comparison. Broad 65-7200 Hz EQ was less destructive but still reduced root accuracy from 0.391 to 0.383. Raw `other + bass` therefore remains the model input. Adding HPSS as a second chroma layer changed five-track root accuracy from 0.403 (raw chroma context) to 0.399 and added 7.46 seconds to the 65-second Wandering fixture, so it was removed.

On five untouched GuitarSet held-out excerpts, raw ChordMini exact/root accuracy was 0.319/0.519 and raw-chroma context produced 0.321/0.523. The corpus includes isolated guitar solos, where absolute chord recognition is predictably much weaker than on accompaniment excerpts; the small held-out gain is the relevant postprocessing comparison.

The final context stage uses a soft chroma-derived key, bass-root support, ChordMini evidence, neighboring beats and aligned repetitions of equal structure labels. It retains raw and decoded events. On the 32-bar Wandering Jam2 fixture it improved the current raw post-fusion chord result from exact/root 0.832/0.892 to 0.865/0.912. Context processing itself took about 0.024 seconds.

### Bass

On five FiloBass development songs, conservative monophonic cleanup raised Basic Pitch note F1 from 0.4275 to 0.4455. Fragment thresholds from 35 to 100 ms produced the same result; the gain came from resolving simultaneous candidates and trimming overlaps, not deleting more short notes. The retained 55 ms floor is therefore conservative rather than claimed as tuned.

On five untouched held-out songs, mean F1 rose from 0.4155 to 0.4421 and every song improved. Precision rose from 0.3855 to 0.4271, recall from 0.4520 to 0.4591, and matched-note duration overlap stayed effectively unchanged (0.8732 to 0.8727).

### Drums

MDB Drums showed that raw ADTOF is already high-recall and that candidate repair trades precision for gaps. On ten development tracks raw F1 was 0.8001, repair alone 0.7921, cymbal context without repair 0.8395, and the retained repair plus cymbal context 0.8301. On all seven untouched held-out tracks the corresponding values were 0.7054, 0.6930, 0.7646 and 0.7504. The ride/crash pattern rule is therefore retained: development Crash F1 rose from 0.655 to 0.733 and Ride from 0.000 to 0.283.

Several attempts to tighten candidate repair improved MDB precision but failed the authored-fill regression. Five-repeat repair reduced Wandering drum F1 from 0.970 to 0.966. Confidence and tom-dominance filters reduced it to about 0.960. Those filters were rejected and the selected three-repeat/fill repair retained because it preserves audible groove continuity, fill F1 and dynamics; `--no-drum-repair` remains the explicit accuracy-oriented comparison. Cymbal context is independent and remains enabled in either case.

### Local timing and combined form

The previous California endpoint-aligned export still had internal maximum beat residuals of 72.5 ms (A), 105.8 (B), 102.0 (C), 24.5 (D), 103.6 (E) and 29.9 (F). This confirms the reported Section E drift: an exact endpoint did not imply locally aligned beats.

`California_Context_Bar_Aligned_Combined` uses tracked-bar Signalsmith processing above 45 ms. Consecutive SongFormer verse labels were merged first, producing four enabled arrangement steps: intro, one 28-bar verse, chorus and outro. Intro/verse/chorus use 5/28/8 independently measured bar segments; the steady outro remains one-pass. All 16 imported assets passed existence/hash validation, and all four stems in each bank have identical exact frame counts. The complete real-song pipeline took 24.984 seconds.
