# JamTaster Native ONNX Lab

This is an isolated feasibility implementation for replacing JamTaster's
end-user Python/PyTorch runtime with a private native C++ worker. It is not
part of Jam2's root CMake build, does not write into `release/`, and does not
change the working Python JamTaster integration.

The experiment answers four questions with measurements rather than an
integration commitment:

1. Can each pinned checkpoint be exported once and then run without Python?
2. Does native preprocessing produce equivalent model inputs and events?
3. Does CPU inference use the available cores and complete in acceptable time?
4. Does HTDemucs FT retain the stem quality of its four-member Python bag?

## Boundary

The committed experiment contains the C++ implementation, its adapted
MIT-licensed Demucs source, final ONNX models, provenance sidecars, and tests.
Models larger than GitHub's normal object limit are tracked with Git LFS. Only
local build/test material is ignored:

- `.deps/`: local ONNX Runtime and Eigen SDK packages used by this experiment.
- `outputs/`: reference tensors, native results, stems, and parity reports.
- `build/`: the standalone native build.

The one-off Python checkpoint converter lives at repository-level in
`tools/models/ConvertJamTasterCheckpoints.py`; no Python code is part of this native
experiment or its eventual runtime. Recorded conversion sizes and timings are
in `RESULTS.md`.

The minimal conversion-only Demucs Python fork is also isolated under
`tools/models/third_party/jamtaster_demucs_onnx`. It is never loaded by the
native executable and is needed only if the committed ONNX weights are
deliberately regenerated.

Regeneration requires a separate developer Python environment using
`tools/models/jamtaster_checkpoint_conversion_requirements.txt` plus the
existing JamTaster development runtime. There is intentionally no bootstrap or
end-user Python installation path in this experiment.

## Build

Provide ONNX Runtime 1.23.2 and Eigen 3.4 SDK roots, then build the standalone
experiment from the repository root:

```text
cmake -S experiments/jamtaster-native -B experiments/jamtaster-native/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJAMTASTER_ONNXRUNTIME_ROOT=<ort-sdk> -DJAMTASTER_EIGEN_ROOT=<eigen-source>
cmake --build experiments/jamtaster-native/build
ctest --test-dir experiments/jamtaster-native/build --output-on-failure
```

Inspect the resulting native environment:

```text
experiments/jamtaster-native/build/jamtaster_native_lab doctor --models experiments/jamtaster-native/models
experiments/jamtaster-native/build/jamtaster_native_lab inspect --model experiments/jamtaster-native/models/beat_this.onnx
```

The default ONNX Runtime session uses all hardware threads reported by the OS.
Pass `--threads N` to every native operation to make CPU scaling explicit and
repeatable.

## One-time exports

Each command loads the original checkpoint in the existing private development
component, exports its graph and weights, runs an ONNX Runtime parity probe,
and writes a SHA-256/provenance sidecar:

```text
python tools/models/ConvertJamTasterCheckpoints.py export beat-this
python tools/models/ConvertJamTasterCheckpoints.py export basic-pitch
python tools/models/ConvertJamTasterCheckpoints.py export chordmini
python tools/models/ConvertJamTasterCheckpoints.py export adtof
python tools/models/ConvertJamTasterCheckpoints.py export demucs-ft
```

`demucs-ft` produces four files in this fixed order:

1. `f7e0c4bc` - drums
2. `d12395a8` - bass
3. `92cfc3b6` - other
4. `04573f0d` - vocals

This matches the one-hot weighting in the official `htdemucs_ft.yaml`. The
native adapter runs all four members but takes each requested stem from its
corresponding fine-tuned member; it does not incorrectly average all outputs.

## Beat parity workflow

Generate the Python reference, run native CPU analysis, and compare raw logits
and events:

```text
python tools/models/ConvertJamTasterCheckpoints.py reference --stage beat --input sample.wav --output experiments/jamtaster-native/outputs/sample/python
experiments/jamtaster-native/build/jamtaster_native_lab beat --input sample.wav --model experiments/jamtaster-native/models/beat_this.onnx --threads 8 --dump-tensors experiments/jamtaster-native/outputs/sample/native --output experiments/jamtaster-native/outputs/sample/native.json
python tools/models/ConvertJamTasterCheckpoints.py compare --reference experiments/jamtaster-native/outputs/sample/python/reference.json --native experiments/jamtaster-native/outputs/sample/native.json --output experiments/jamtaster-native/outputs/sample/parity.json
```

Reports include model/provider identity, thread count, stage timings, event
precision/recall/F1, timing error, and raw-logit error. Short Jam2-generated
fixtures should be evaluated before 32-bar material and the full loopback song.

## Note, chord, and drum workflows

The remaining analysis models have full native audio-to-event commands; they
are not model-loading probes:

```text
experiments/jamtaster-native/build/jamtaster_native_lab basic-pitch --input bass.wav --model experiments/jamtaster-native/models/basic_pitch.onnx --min-midi 28 --max-midi 64 --threads 8 --output experiments/jamtaster-native/outputs/sample/bass.json
experiments/jamtaster-native/build/jamtaster_native_lab chords --input harmonic.wav --model experiments/jamtaster-native/models/chordmini_btc.onnx --threads 8 --output experiments/jamtaster-native/outputs/sample/chords.json
experiments/jamtaster-native/build/jamtaster_native_lab drums --input drums.wav --model experiments/jamtaster-native/models/adtof.onnx --thresholds 0.22,0.24,0.32,0.22,0.30 --threads 8 --output experiments/jamtaster-native/outputs/sample/drums.json
```

`basic-pitch` implements the upstream 22.05 kHz overlapping-window contract,
activation unwrapping, inferred onsets, onset decoding and sustained-note
recovery. `drums` implements ADTOF's 44.1 kHz STFT, 84-band logarithmic
filterbank and five-lane peak picker. `chords` implements ChordMini's 144-bin
log-frequency input, checkpoint normalization, 50% overlapping 108-frame
votes, Gaussian logit smoothing, categorical majority filtering and the full
170-class label vocabulary. Every report separates preprocessing, ONNX
inference and postprocessing time.

The conversion utility can generate the corresponding pinned Python references
for event-level comparison by selecting `--stage basic-pitch`, `--stage chords`
or `--stage drums`. Its existing `compare` command automatically selects MIDI
note-onset F1, 10 ms chord-frame agreement, or lane-aware drum-hit F1 from the
two report formats.

The fixed-window native ChordMini transform follows the checkpoint's frequency
grid but still needs corpus parity measurements against librosa's recursive CQT
before it can pass the production gate. ADTOF remains a local experiment until
its weight redistribution terms are resolved.

## Demucs workflow

After all four FT members have exported:

```text
experiments/jamtaster-native/build/jamtaster_native_lab demucs --input sample.wav --models "experiments/jamtaster-native/models/htdemucs_ft_0.onnx;experiments/jamtaster-native/models/htdemucs_ft_1.onnx;experiments/jamtaster-native/models/htdemucs_ft_2.onnx;experiments/jamtaster-native/models/htdemucs_ft_3.onnx" --output-dir experiments/jamtaster-native/outputs/sample/stems --threads 8 --seed 0 --output experiments/jamtaster-native/outputs/sample/demucs.json
```

The adapter follows the open-source `sevagh/demucs.onnx` boundary: native
STFT/iSTFT, segmentation, overlap-add and shift handling surround a core ONNX
graph. The small required MIT-licensed C++ adapter is committed under
`third_party/demucs_onnx` so its behavior is reviewable and reproducible.
JamTaster's version reproduces Python's contextual final-segment padding, fixes
STFT workspace and reflection padding, uses explicit half-spectrum real FFTs,
loads model files without a second in-memory copy, reports deterministic shift
seeds/offsets, and keeps progress reporting outside the inference library.
Source, revision and measured parity details are in `THIRD_PARTY_NOTICES.md`
and `RESULTS.md`.

## End-to-end native workflow

The `taste` command is the standalone native equivalent of the Python worker's
song-building pipeline. It separates and caches stems beneath the selected
project's `Analysis/sources/<sha256>/` directory, tracks tempo and downbeats,
analyzes chords, drums and bass, applies contextual repair, infers conservative
downbeat-aligned sections from measured tonal, groove, bass and energy changes,
and writes a Jam2-shaped JamJar with pitch-preserved stem
assets under `imported/`:

```text
experiments/jamtaster-native/build/jamtaster_native_lab taste --input sample.wav --project-root experiments/jamtaster-native/outputs/sample-project --name Sample_Song --models experiments/jamtaster-native/models --threads 8 --output experiments/jamtaster-native/outputs/sample-project/taste-result.json
```

Use `--force` to repeat every model stage, `--force --reuse-stems` to repeat
analysis/export without the expensive separation stage, `--analysis-only` to omit JamJar
rendering, `--bpm N` or `--meter N` to override the detected grid,
`--no-time-stretch` to retain source section lengths, and
`--no-arrangement-loop` to disable arrangement looping. A completed identical
source/name request returns the hash-owned saved result without loading models.

The context layer includes untouched Other-plus-Bass chord preparation,
beat-synchronous chroma fallback and evidence, soft key/bass/continuity chord
decoding, bass-root chord fusion, monophonic bass cleanup, repeated-cell drum
repair, four-four snare dynamics, rim-mode crosstalk suppression, sustained
ride classification, multimodal section changes, tracked-grid quantization,
and pitch-preserving Signalsmith stretching. Sections at or below 45 ms of
predicted internal grid residual use one whole-section stretch. Sections
above that threshold mirror the Python worker: each tracked bar receives an
independent stretch using its explicit seek/process/flush latency compensation,
and the exact-length bars are concatenated.
Section inference does not assume that an input contains a complete song,
intro or outro: its two-bar
measurement context may detect a boundary with fewer than four bars remaining,
while the four-bar spacing rule applies only between selected internal changes.
Forced re-exports remove obsolete JamTaster-owned WAVs only after the replacement
JamJar has been written successfully.

## Production gate

No Jam2 integration should change until:

- native and Python event accuracy remain equivalent on the known-truth corpus;
- native and Python Demucs stems have aligned lengths and no audible or measured
  quality regression;
- CPU timing and peak memory are recorded for short, 32-bar and full-song runs;
- Windows CPU plus macOS CPU/Core ML behavior has been checked; and
- every shipped model's weight redistribution terms are confirmed.

ADTOF remains local-only and is excluded from Git/LFS. Its PyTorch port has no
explicit licence file and the original repository uses CC BY-NC-SA 4.0, so its
converted weights must not be placed in a Jam2 distribution until that is
resolved.
