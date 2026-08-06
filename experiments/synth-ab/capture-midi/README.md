# Standard reference-capture MIDI

`drums-v1` is the reproducible MIDI pack for capturing the next acoustic or
electronic reference kit. See
[`../REFERENCE_CAPTURE_PLAYBOOK.md`](../REFERENCE_CAPTURE_PLAYBOOK.md) for the
recording settings, Ableton workflow, and the reasoning behind the schedule.

The pack uses General MIDI percussion notes on MIDI channel 10. Run
`00-kit-map-check.mid` before recording. If a device uses a different map,
transpose that articulation's MIDI clip in Ableton; do not change its velocity
or timing schedule.

Regenerate the checked-in files from the repository root with:

```powershell
python experiments/synth-ab/tools/generate_drum_capture_midi.py
```

Validate them without writing with:

```powershell
python experiments/synth-ab/tools/generate_drum_capture_midi.py --check
```

`manifest.json` is machine-readable and records the exact notes, durations,
hit counts, timing classes, and SHA-256 hash of every MIDI file.

Project-specific note maps and musical comparison patterns are kept separately
from the generic capture files. The mapping recovered from `DrumSamples.als` and
four eight-bar pattern sets for the electronic kit are in
[`808-project-v1`](808-project-v1/README.md).

`melodic-v1` is the reproducible pack for melodic instrument capture and mix
auditioning. It contains a complete full-spectrum capture, focused diagnostic
modules, five aligned musical-role stems, and a type-1 full arrangement. See
[`melodic-v1/README.md`](melodic-v1/README.md) for ranges, Ableton placement,
export settings, and the extra patch information needed for replication.

Regenerate or validate it with:

```powershell
python experiments/synth-ab/tools/generate_melodic_capture_midi.py
python experiments/synth-ab/tools/generate_melodic_capture_midi.py --check
```
