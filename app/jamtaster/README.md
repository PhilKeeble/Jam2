# JamTaster

JamTaster is Jam2's bundled native WAV-analysis engine. The public application
remains `jam2`; analysis is executed by the private `jamtaster-worker` helper so
a model/runtime failure cannot bring down a live Jam2 process.

The release contains the worker, ONNX Runtime and the model weights. It does not
use Python, pip, PyTorch, CUDA, an installer, a downloader, or a mutable runtime.
Inference is CPU-only and uses the machine's available hardware threads.

## Runtime boundary

Jam2 writes a protocol-1 JSON request and starts one worker process. The worker
emits newline-delimited progress, result and error objects. Supported actions
are `detect_bpm`, `split_stems`, `analyze_all` and `convert_song`.

The internal command is:

```text
jamtaster-worker worker --request <request.json> --models <model-directory>
```

This command is an implementation boundary, not a second public Jam2 CLI.

## Song-local results

Results are keyed by the source WAV's SHA-256 and live with the owning song:

```text
<song>/analysis/index.json
<song>/analysis/sources/<sha256>/tempo.json
<song>/analysis/sources/<sha256>/stems/
<song>/analysis/sources/<sha256>/analysis.json
<song>/analysis/sources/<sha256>/converted/<song>/
```

Deleting an unsaved song therefore deletes its analysis. Analysing is local and
does not send a peer command. Only applying results changes normal Jam2 project
state; existing Jam Sync and WAV-sharing settings then replicate those ordinary
tempo, section, note and imported-lane changes.

## Native pipeline

The worker runs the four-model HTDemucs FT ensemble, Beat This, ChordMini,
Basic Pitch and ADTOF through ONNX Runtime. Native post-processing aligns the
grid, restores drum patterns and dynamics, uses bass/chord/groove context, finds
meaningful bar-aligned section changes, and exports up to twelve sections with
an enabled arrangement. Exported section WAVs use Signalsmith Stretch and live
under the JamJar's normal `imported/` directory.

Section inference does not assume that the input is a complete song and does
not use a separate song-form model. Sections use neutral names such as
`Section A` and are grouped from sustained tonal, groove and energy changes.

## Source layout

- `*.cpp` / `*.hpp`: native inference, DSP, post-processing and worker protocol.
- `third_party/demucs_onnx`: locally adapted MIT-licensed Demucs ONNX C++ code.
- `models`: production ONNX artifacts and provenance sidecars.
- `tests`: deterministic native DSP, export and post-processing tests.
- `tools/models`: one-off Python conversion tools; these are never shipped or
  invoked by Jam2.

Model and source licensing details are in `MODEL_NOTICES.md`. The ADTOF weight
redistribution terms still require resolution before publishing a release; its
local artifact remains ignored until that decision is made.

## Validation

The normal Jam2 build compiles the worker and stages it with the application.
On Windows, follow the repository build rule and then run:

```powershell
ctest --test-dir build --output-on-failure
```

Release validation should also submit a protocol-1 request to the staged worker
and verify that every `asset_path` in the generated JamJar names an existing,
valid WAV.
