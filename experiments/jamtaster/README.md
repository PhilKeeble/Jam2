# JamTaster

JamTaster is a pure-Python experiment that turns one Jam2 loopback recording into a copy-ready Jam2 song. The first POC accepts only the format Jam2 records: mono, uncompressed, PCM16 WAV.

It separates four stems (`drums`, `bass`, `other`, `vocals`), analyses timing, form, chords, core drum hits and bass notes, then writes the detected form into Jam2 sections, up to Jam2's 12-section limit. Consecutive spans with the same SongFormer label are merged, so three adjacent verse spans become one continuous Verse; a later verse after a chorus remains distinct. Sections begin and end on tracked downbeats and contain complete bars.

By default, JamTaster uses Signalsmith Stretch through its Python binding to pitch-preserve every stem onto the exact whole-BPM Jam2 grid. It measures internal beat residuals after endpoint alignment. Banks above the explicit local-warp threshold are stretched at tracked bar anchors using identical boundaries for all four stems; steadier banks retain the simpler one-pass stretch. Both source and exported drift, every bar factor, and the local residual curve remain in the report.

## Setup

Run all commands through the one public interface from the repository root:

```powershell
python experiments\jamtaster\JamTaster.py setup
python experiments\jamtaster\JamTaster.py models fetch
python experiments\jamtaster\JamTaster.py doctor
```

`setup --cpu` installs CPU-only PyTorch. The default setup installs CUDA 12.8 PyTorch in `experiments/jamtaster/.venv`; neither choice changes system Python. Package, Torch, Hugging Face, model and temporary-work caches all stay inside the experiment and are ignored by Git.

The POC deliberately requires Python 3.10, matching SongFormer's tested environment and the current Jam2 development machine.

Model fetching is deliberately separate because the research weights are large. JamTaster pins the SongFormer, ChordMini and ADTOF source revisions. The weights and fetched source are local runtime material and are not redistributed by this repository.

## Taste a recording

```powershell
python experiments\jamtaster\JamTaster.py taste C:\recordings\song.wav --name "Song Name"
```

Useful explicit controls include:

```text
--device auto|cpu|cuda
--sections auto|single
--bpm 123.0
--meter auto|2..12
--no-arrangement-loop
--no-time-stretch
--demucs-model htdemucs_ft
--beat-model final0
--drum-thresholds 0.12,0.16,0.12,0.05,0.20
--drum-candidate-scale 0.30
--drum-division 4
--drum-ghost-energy-ratio 0.50
--drum-accent-energy-ratio 0.85
--drum-repair-min-repeats 3
--drum-repair-neighborhood-bars 8
--no-drum-repair
--bass-min-midi 28 --bass-max-midi 60
--drift-warning-ms 100
--local-drift-warning-ms 45
--local-warp-threshold-ms 45
```

The accepted BPM is rounded to the whole-BPM value stored by Jam2. Each exported bank is then stretched to exactly `beats * 60 / BPM` seconds, rounded once to the WAV sample grid; all four stems receive the same target frame count. The raw detected BPM, source duration, stretch factor and residual frame-grid error remain in the reports. `--no-time-stretch` is an explicit diagnostic opt-out. `--sections single` is useful for a short loop, skips unnecessary song-form inference, and fails if the full recording would exceed Jam2's 512-beat section limit.

Drum notes use Jam2's native `g`, `x` and `a` states. ADTOF confidence supplies core hits; band-limited transient energy and beat/downbeat position retain dynamics. Low-threshold events are admitted only with repeated-bar, verified transient-tom, or high-confidence snare-cluster evidence. Persistent rim grooves are identified separately so a cross-stick sample is not expanded into simultaneous tom, kick and hi-hat notes. Every candidate, repair, suppression and dynamics rule is retained in `analysis.json`.

Chord and bass events are quantized through the recording's tracked beat intervals rather than by repeatedly adding the rounded project BPM. This prevents small source-tempo offsets accumulating into late notes. ChordMini receives the untouched `other + bass` analysis mix: GuitarSet testing rejected HPSS and EQ as primary model inputs. A context decoder then combines raw beat-synchronous chroma, a soft key estimate, bass-root support, neighboring beats, and aligned repetitions of the same section. Raw model events and every contextual change are retained. Sparse bass recovery consults the full mix only when the separated line is irregular and confines recovered notes to the register established by the bass stem; a conservative monophonic pass resolves simultaneous candidates and overlaps without inventing notes.

## Output

For `--name "Song Name"`, JamTaster creates:

```text
experiments/jamtaster/
  songs/
    Song_Name/
      Song_Name.jamjar
      imported/
        jamtaster-a-verse-drums-<stable-id>.wav
        ...
  reports/
    Song_Name/
      analysis.json
      manifest.json
      progress.json
```

Copy the entire `Song_Name` directory into the `songs` directory next to the target `jam2.exe`. The copy-ready folder contains only native Jam2 content. In particular, JamTaster does not create or use `generated`, `prepared`, `recorded`, `received` or `jam_recordings`.

Generated arrangements are saved as enabled. A current Jam2 build arms the first arrangement row when the song opens, so the arrangement follows the next normal transport Play without reopening the arrangement editor. This local startup preference is intentionally not sent in Track Sync snapshots.

JamTaster refuses to overwrite an existing song folder. Names use the same portable-slug rules as Jam2, including Windows reserved names.

`analysis.json` records every stage's wall time and real-time factor. `progress.json` is updated after each completed stage so an interrupted or failed research run still retains useful timing evidence.

## Known-truth benchmark

The benchmark command makes a short mono full mix from an existing native Jam2 generated bank and retains its recipe, source stems, beats, chords, drum events and bass events as ground truth:

```powershell
python experiments\jamtaster\JamTaster.py benchmark release\songs\Calm_Satellite\Calm_Satellite.jamjar --name Calm_Satellite_8_Bars --bank 0 --bars 8
python experiments\jamtaster\JamTaster.py taste experiments\jamtaster\reports\benchmarks\Calm_Satellite_8_Bars\input\fullmix.wav --name Calm_Satellite_Tasted --sections single --bpm 98 --meter 4
python experiments\jamtaster\JamTaster.py score experiments\jamtaster\reports\benchmarks\Calm_Satellite_8_Bars\truth.json experiments\jamtaster\reports\Calm_Satellite_Tasted\analysis.json
```

The score includes stem SI-SDR/correlation, tempo error, beat/downbeat timing, chord symbol/root agreement, drum hit/state/repair accuracy (overall, by family, and inside/outside authored fill regions), bass pitch/onset agreement, and the stage timings from the tasted run.

The retained POC measurements and keep/refine decisions are summarized in `BENCHMARKS.md`.

## Labelled reference datasets

All dataset audio lives under repository-root `datasets/`, which is ignored as a whole by Git. Inventory and benchmark operations remain subcommands of the single Python interface:

```powershell
python experiments\jamtaster\JamTaster.py datasets inventory
python experiments\jamtaster\JamTaster.py datasets benchmark guitarset --split dev --limit 10 --device cuda
python experiments\jamtaster\JamTaster.py datasets benchmark filobass --split heldout --limit 5 --device cuda
python experiments\jamtaster\JamTaster.py datasets benchmark mdb-drums --split heldout --limit 7 --device cuda
```

Track identifiers are assigned to deterministic development and held-out splits by SHA-256. Rules are changed only from development results; held-out reports are validation. GuitarSet supplies chord/beat/note truth, FiloBass supplies aligned bass MIDI, MDB Drums and Groove MIDI supply drum events, dynamics and timing. HarmonixSet is annotations-only until matching local audio is provided. Stem-only corpora are deliberately not reported as transcription-accuracy references. See `DATASETS.md` for local layouts and provenance.

## Model roles

- `demucs-infer` / `htdemucs_ft`: local four-stem separation.
- Beat This: local beat and downbeat tracking.
- SongFormer: local PyTorch/safetensors functional structure analysis. Hugging Face is used only to distribute files; this is not a hosted inference call, GGUF, or language model.
- ChordMini BTC: local chord recognition on the `other + bass` analysis mix.
- ADTOF-pytorch: local five-class drum transcription, mapped onto Jam2's named drum lanes.
- Basic Pitch: local bass-stem note transcription with an explicit pitch range.
- Signalsmith Stretch via `python-stretch`: native pitch-preserving whole-section or tracked-bar alignment, orchestrated by Python and trimmed to exact target frame counts.

SongFormer's functional labels are retained as section names. Consecutive equal labels are merged before downbeat snapping; non-consecutive repetitions and distinct intro/outro material remain chronological arrangement steps. When fewer than four segments exist, the remaining required Jam2 sections are normal empty banks.

## Tests

The fast tests use the isolated environment (including Signalsmith and NumPy) but do not run or download ML models:

```powershell
experiments\jamtaster\.venv\Scripts\python.exe -m unittest discover -s experiments\jamtaster\tests -v
```
