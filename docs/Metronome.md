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

## Tuning Notes

- Keep metronome level low enough that it does not mask the remote instrument.
- If timing feels unstable, inspect stats before changing metronome mode.
- For repeatable metronome comparisons, record jam stems and compare the generated WAVs and CSV logs.
- The benchmark tools include metronome timing scenarios documented in [Test Cases](TestCases.md).
