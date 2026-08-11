# JamTaster component

JamTaster is Jam2's optional, isolated WAV-analysis component. Production code lives here because it is an application feature rather than a development experiment.

## Runtime boundary

Jam2 starts one private worker process per analysis request and communicates with JSON lines. Python, Torch, native model libraries and model weights never load into `jam2.exe`. A failed worker cannot corrupt the live Jam2 process.

Shared runtime material is versioned outside the repository:

```text
Windows: %LOCALAPPDATA%/Jam2/components/jamtaster/1.0.0/
macOS:   ~/Library/Application Support/Jam2/components/jamtaster/1.0.0/
```

Per-WAV results belong to the owning song:

```text
<song>/analysis/index.json
<song>/analysis/sources/<sha256>/tempo.json
<song>/analysis/sources/<sha256>/stems/
<song>/analysis/sources/<sha256>/analysis.json
<song>/analysis/sources/<sha256>/converted/<song>/
```

Deleting an unsaved song therefore deletes its analysis. Applied WAVs are materialized under the normal `imported/` folder.

Installation, health checks, worker progress, and the `analysis/` cache are
strictly local. They never create peer commands. Only an explicit Apply action
changes the ordinary Jam2 song/track models, after which the existing Jam Sync
and track-sharing preferences distribute those normal project changes.

## Development installation

The Settings page invokes this automatically from a source checkout. The equivalent command is:

```powershell
py -3.10 app\jamtaster\worker\JamTaster.py install
```

This source-only installer creates the private runtime and downloads pinned models into the same application-data location used by a release. Jam2 automatically selects a CPU-only or CUDA package on Windows from the detected NVIDIA GPU and driver. macOS uses the native PyTorch package and never installs CUDA; Health reports whether Apple's MPS backend is present while analysis keeps the conservative CPU path. End users receive or download a prebuilt component and never need Python, pip, a virtual environment, or an accelerator choice.

All development-installer pip commands are executed by the Python executable
inside the versioned component runtime. Package upgrade/uninstall messages
therefore refer only to that private runtime and never to the user's system
Python installation. The native UI consumes structured installation milestones
for its progress bar while retaining raw pip/model output only in Jam2's logs.

## Worker requests

The stable interface remains the single `JamTaster.py` argparse entry point:

```powershell
python JamTaster.py worker --request request.json
```

Protocol 1 supports `detect_bpm`, `split_stems`, `analyze_all` and `convert_song`. Requests name an existing WAV and its owning project root. Progress, results and errors are emitted as JSON lines.

Song sections are inferred locally from sustained, bar-aligned changes in the
detected chord cadence, drum groove, bass pattern, and stem energy. The result
uses neutral `Section A`, `Section B`, and subsequent names; there is no separate
song-form model or runtime dependency.

## Release package

After installing and checking a complete component on its target operating system:

```powershell
python app\jamtaster\packaging\build_component.py `
  --component-root <component-root> `
  --output artifacts\jamtaster
```

The builder creates a platform-specific `tar.gz` and a small release manifest containing its exact byte size and SHA-256. Windows artifacts have separate `-cpu` and `-cuda` suffixes; macOS artifacts use the native platform package and contain no CUDA libraries. Jam2 detects the appropriate package, verifies the archive and publishes the component atomically. If an automatically selected accelerator fails at job runtime, that request is retried once on CPU. Builds must be produced separately for Windows x86-64 CPU/CUDA, macOS arm64 and macOS x86-64 where supported, then signed/notarized as part of the normal release pipeline.

Model redistribution licences must be audited before publishing a complete component archive.

## Fast tests

The unit tests do not download or execute the ML models:

```powershell
<component-python> -m unittest discover -s app\jamtaster\worker\tests -v
```
