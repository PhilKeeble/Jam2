# Metronome

Jam2 includes a shared metronome for timing experiments and practical playing. The metronome is generated locally; the UDP audio stream does not carry a mixed metronome track.

## Controls

CLI startup options:

```text
--metronome on|off
--bpm n
--metronome-level n
--metronome-mode shared-grid|leader-audio|listener-compensated
```

Runtime commands:

```text
metro on
metro off
metro mode shared-grid
metro level 0.15
metro level +0.05
bpm 140
```

The GUI exposes the same controls in its runtime/metronome area.

## Modes

| Mode | Use |
| --- | --- |
| `shared-grid` | Default mode. Both peers generate the click from the shared timing grid. |
| `leader-audio` | Useful for comparing click timing against the listener's audio path. |
| `listener-compensated` | Experimental mode for comparing listener-side compensation behavior. |

Use `shared-grid` first. Compare the other modes only when collecting timing data or validating metronome behavior.

Every fresh Start after Stop creates a new ordered grid revision and epoch, so both connected peers begin again at `1.1`. A late or rejoining peer does not restart that running grid: it maps the authority's original epoch and elapsed frame position onto its local engine, then joins the current absolute bar and beat. Repeating an unchanged On, BPM, pattern, or mode control also leaves the current revision and authority epoch intact. If the current grid authority leaves while the metronome is running, the surviving coordinator immediately orders a fresh running epoch instead of leaving a stopped clock with an On control. Authenticated TCP metronome messages update the remote GUI presentation; the native UDP authority state alone applies the run state, mode, pattern, and mapped epoch to the remote engine so it cannot create a competing local proposal.

Grid-aligned lane recording keeps the current running position while it waits for the next safe whole-bar boundary, performs the configured count-in on that grid, and then publishes one Track Sync `RecordStart` target. It does not submit a duplicate Start or reset to `1.1` when recording is armed. At the take boundary every Track-Sync-enabled peer resets its visible/track epoch to `1.1`, restarts any prepared tracks, and the recording peer begins capture. A peer with Track Sync disabled neither publishes nor applies that coordinated reset.

## Tuning Notes

- Keep metronome level low enough that it does not mask the remote instrument.
- If timing feels unstable, inspect stats before changing metronome mode.
- For repeatable metronome comparisons, record jam stems and compare the generated WAVs and CSV logs.
- Metronome timing and impairment scenarios are documented in
  [Stress Tests](StressTests.md), with cross-machine measurements in
  [Benchmark](Benchmark.md).
