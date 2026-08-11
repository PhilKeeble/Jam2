# JamTaster Native Feasibility Results

These are local Windows CPU measurements from 2026-08-11. Audio fixtures and
detailed reports remain in the ignored `outputs/` directory. The final ONNX
artifacts and portable provenance sidecars are configured for Git LFS.

## Checkpoint conversion

The one-time converter used the existing private JamTaster development runtime.
Conversion-only packages were isolated from the end-user path.

| Model | ONNX artifacts | Total size | Last export time | Export probe |
| --- | ---: | ---: | ---: | --- |
| Beat This `final0` | 1 | 79.2 MiB | 4.82 s | maximum absolute error `1.10e-5` |
| Basic Pitch 0.4.0 | 1 | 0.2 MiB | 0.14 s | official upstream ONNX copied unchanged |
| ChordMini BTC | 1 | 12.5 MiB | 3.06 s | maximum absolute error `5.72e-6` |
| HTDemucs FT | 4 | 664.8 MiB | 15.23 s | native audio parity below |

The seven distributable artifacts total 756.6 MiB (0.739 GiB). All pass ONNX
validation and load with ONNX Runtime's CPU execution provider. ADTOF is a
separate 1.7 MiB local experiment and is excluded from Git/LFS because its
weight-redistribution terms remain unresolved.

## Beat This native parity

The 29.4-second Jam2-generated `Calm_Satellite` drum fixture produced 49 beats
and 13 downbeats in both Python and native C++. At a 20 ms event tolerance,
beat and downbeat precision, recall and F1 were all `1.0`.

- Beat-logit mean absolute error: `0.0192`.
- Downbeat-logit mean absolute error: `0.0130`.
- Maximum matched event displacement: `20 ms` (one beat).
- Native inference: `1.379 s` at 1 thread, `0.776 s` at 2, `0.488 s` at 4,
  and `0.392 s` at 8.
- Native preprocessing: approximately `0.323 s`; total fell from `1.701 s` at
  1 thread to `0.714 s` at 8 threads.

This verifies both CPU-only execution and useful multicore scaling.

## HTDemucs native parity

The known-truth fixture is an 8-second, 48 kHz Jam2 mix made from its original
drums, bass, chords and melody lanes. `other` truth is chords plus melody.

| Stem | Native correlation | Native SDR | Python correlation | Python SDR |
| --- | ---: | ---: | ---: | ---: |
| drums | `0.98879` | `15.55 dB` | `0.98940` | `15.75 dB` |
| bass | `0.98008` | `12.44 dB` | `0.97276` | `11.42 dB` |
| other | `0.99055` | `16.79 dB` | `0.99109` | `17.02 dB` |

For an exact-boundary comparison, native C++ and the conversion fork used the
same 44.1 kHz input and the same four shift offsets. Native-versus-PyTorch SDR
was `69.50 dB` for drums, `67.56 dB` for bass and `73.64 dB` for other, with
correlations above `0.9999999`. The near-silent vocals output measured
`49.61 dB`; correlation is not meaningful for that near-zero target.

The final native 48 kHz run used 8 CPU threads:

- preprocessing/resampling: `0.184 s`;
- four-member ONNX inference and DSP: `12.381 s`;
- mono WAV writing/resampling: `0.819 s`;
- total: approximately `13.38 s`.

The adapted PyTorch boundary took `7.66 s` for inference on the same host. The
native path trades some speed for eliminating Python, Torch and environment
management from the shipped runtime; further native optimization remains
possible after integration is justified.

## Adapted Demucs corrections

Parity work found and fixed concrete issues in the upstream C++ adapter:

- final short segments retain the preceding context used by nested Python
  `TensorChunk` padding before centre-trimming;
- model-input padding excludes unrelated STFT workspace padding;
- reflection padding follows PyTorch's boundary-excluding semantics;
- real FFT processing uses the positive-frequency half-spectrum explicitly;
- deterministic shift seeds and actual offsets are reported;
- output stems retain the exact source-frame count after sample-rate conversion;
- models load directly from disk without a second full in-memory copy; and
- library inference no longer prints unsolicited console output.

The unit suite includes PyTorch complex-bin reference values and a native
STFT-to-iSTFT reconstruction guard. The MSVC build completes without warnings,
and `jamtaster_native_units` passes.

## Native analysis models

Native audio-to-event implementations exist for Basic Pitch, ChordMini BTC and
ADTOF rather than only ONNX load probes. Each reports preprocessing, inference
and postprocessing separately. Full-song CPU timings are recorded below. Event
parity across the known-truth corpus remains a production-gate task; in
particular, the fixed-window ChordMini transform must still be compared with
the pinned librosa CQT across more than one song.

## Californication full-song CPU timing

The native experiment analyzed the same 113.268-second, 48 kHz loopback WAV as
the existing Python/CUDA JamTaster report, using ONNX Runtime CPU with eight
threads. Model outputs and detailed JSON reports remain under the ignored
`outputs/californication-native/` directory.

| Stage | Native CPU | Existing Python/CUDA |
| --- | ---: | ---: |
| HTDemucs FT separation and stem writing | `122.607 s` | `6.265 s` |
| Beat/downbeat analysis | `2.913 s` | `0.497 s` |
| Chord analysis | `1.829 s` | `3.676 s` including chord-source preparation |
| ADTOF drum analysis | `2.989 s` | `1.225 s` |
| Basic Pitch bass analysis | `2.250 s` | `0.784 s` |
| Comparable stages, sequential | `132.588 s` | `12.447 s` |
| Complete native pipeline excluding saved separation | `21.868 s` | `17.602 s` |
| Complete native pipeline, forced separation | `119.592 s` | `17.602 s` |

Native Demucs consisted of `2.645 s` preprocessing, `108.527 s` inference/DSP,
and `11.435 s` output conversion/writing. It ran at `1.08x` the source duration
and is the clear CPU bottleneck. The four independent analysis stages could run
in parallel after separation, giving a measured-stage critical path of about
`125.596 s`. Native structure inference, contextual repair and JamJar export
were added in the subsequent wrapper run described below.

The native reports contained 181 beats, 45 downbeats, 715 raw drum hits, 321
raw bass notes and 58 chord segments. Median detected beat spacing was `0.620 s`
(`96.77 BPM`). Chord output followed the expected broad Am/F and C/G/Dm song
patterns, but this run used the raw native `other` stem: native other-plus-bass
preparation/EQ and CQT parity are still required before making an accuracy
claim. Drum output reproduced the already-known ride-to-crash confusion and
therefore also still needs the contextual repair layer.

## Californication native wrapper run

The final standalone `taste` wrapper processed the same 113.268-second source
on Windows with ONNX Runtime CPU and eight threads. A forced run completed in
`119.592 s`; separation took `97.724 s`, leaving `21.868 s` for the final
chroma/context-enabled analysis and export stages. The earlier pre-chroma
saved-stem wrapper pass completed in `20.123 s`. A fully completed cache hit
returned in approximately `104 ms`.

| Wrapper stage | Time |
| --- | ---: |
| Beat and downbeat tracking | `3.247 s` |
| ChordMini plus beat-synchronous chroma | `3.188 s` |
| ADTOF core/candidates and energy shaping | `5.972 s` |
| Basic Pitch and bass recovery | `2.214 s` |
| Multimodal structure inference | `2.523 s` |
| Context post-processing | `0.162 s` |
| Signalsmith/JamJar export | `4.202 s` |

The result contains 181 beats, 45 downbeats, a detected `96.774 BPM` stored as
Jam2's whole `97 BPM`, 181 beat-synchronous chroma evidence cells, 61 final
chord segments, 855 drum events and 290 bass notes. After removing assumed
four-bar phrase bonuses and endpoint-length constraints, evidence-only
multimodal change detection produced five contiguous, downbeat-aligned sections
of 44, 40, 48, 32 and 12 beats (11, 10, 12, 8 and 3 bars). The last three
boundaries are valid for an excerpt and are not treated as an inferred outro.

The final evidence-only, corrected local-warp cached-stem rerun completed in
`21.962 s`: beat tracking took `3.144 s`, chords `3.203 s`, drums `6.027 s`,
bass `2.162 s`, structure inference `2.603 s`, context post-processing
`0.162 s`, and export `4.306 s`.

The JamJar has five enabled arrangement steps and 20 imported lanes (four
stems per section). All 20 asset paths were resolved through Windows
extended-length IO, their SHA-256 values matched the JamJar metadata, and each
48 kHz WAV had exactly `section_beats * 60 / 97` frames. The generated JamJar
was 616,912 bytes, below Jam2's 4 MiB limit.

The first native per-bar implementation used uncompensated low-level processors
and introduced warm-up gaps; retaining one streaming processor across changing
ratios removed the gaps but moved content relative to the grid. The corrected
path mirrors the `python-stretch` binding: banks above 45 ms predicted residual
use a fresh Signalsmith instance per tracked bar with explicit
`seek()`/`process()`/`flush()` latency compensation, followed by exact-length
concatenation. Lower-residual banks retain a single whole-section stretch.

On the regenerated Californication drum stems, all 39 internal bar joins had
non-silent audio in their first 50 ms. There were zero silent or near-silent
joins. Every tracked bar boundary maps exactly to its target 97 BPM grid
position.

The intermediate native `exact()` implementation was also rejected because its
per-bar tail flush repeatedly approached zero and produced an audible restart.
After porting `python-stretch`'s explicit latency handling, the first seven
matching Californication drum joins had mean/max waveform jumps of
`0.0233`/`0.0880`, compared with Python's `0.0303`/`0.0826`. Relative to nearby
sample-to-sample motion, native averaged `1.41x` and Python `2.43x`; native bar
tails no longer collapse toward zero.
