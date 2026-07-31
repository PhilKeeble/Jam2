# Metronome

Jam2 includes a shared metronome for timing experiments and practical playing.
Shared-grid and listener-compensated clicks are generated locally. In
leader-audio mode, exactly one selected peer injects its click into its normal
UDP audio stream and the other peers suppress their local renderer.

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

The GUI exposes the same controls in its runtime/metronome area. It also has
explicit written beat-unit (`/2`, `/4`, `/8`, or `/16`), tempo-pulse, beats per
bar, and click-division controls. Generated ideas set them from their researched
meter.

The written beat unit describes notation and grid positions. Tempo pulse says
how many written units one displayed BPM pulse spans. Simple and odd meters use
one; compound 6/8, 9/8, and 12/8 use three eighth-note units per dotted-quarter
pulse. At 120 BPM, a written eighth in compound meter therefore lasts one third
of a 120 BPM pulse. The click is enabled at researched group starts by default,
so 12/8 clicks four dotted-quarter pulses rather than all twelve eighths.

Both timing fields are applied through the authenticated UDP grid authority and
used by the audio clock, scheduling grid, quantized recording schedules, and
bar-duration calculations. Click enablement, selected click hits, accents,
sound, and level remain local renderer settings. The notation denominator never
silently rescales a profile's researched BPM range.

## Modes

| Mode | Use |
| --- | --- |
| `shared-grid` | Default mode. Both peers generate the click from the shared timing grid. |
| `leader-audio` | Useful for comparing click timing against the listener's audio path. |
| `listener-compensated` | Every peer shifts its own local click toward the average audible phase of all active remote peers. With two peers, each side follows the other's received audio timing. |

Use `shared-grid` first. Compare the other modes only when collecting timing data or validating metronome behavior.

Listener compensation does not give the creator or grid authority special
rendering control. Every participant publishes its mapped grid phase and uses
the fresh playout phase of every active remote stream. Each participant then
adjusts only its own click toward that group average; the shared grid and audio
remain otherwise unchanged. CSV stats expose the base, target, applied offset,
averaged latency, contributing-peer count, clamp events, and stale events.

The UDP grid is created with the session and keeps running independently of the
audible click and backing-track transport. In shared-grid and
listener-compensated modes, click On/Off and pattern accents are local only. In
leader-audio mode, starting the click transfers grid authority to the starter
so that only that peer injects click audio, but it retains the existing epoch
and musical phase. Otherwise authority remains with the bootstrap coordinator.
A BPM or tempo-pulse change is an explicit hard reset because it changes the
duration of the timing unit: pending/playing backing and recording transport
stop and a fresh ordered epoch is established. Meter, written-unit, division,
mode, click enablement, hit selection, and accents do not reset the epoch. A
late or rejoining peer maps the current epoch onto its local engine without
restarting it. TCP carries no competing metronome epoch or settings model.

Grid-aligned lane recording waits for the next safe beat of the existing epoch,
then performs the configured number of count-in bars. The backing source,
recording take, and track-relative GUI markers all start together after that
count-in, with source frame zero displayed as `1.1`. This presentation origin
does not alter the continuous UDP epoch. With the backing stopped, the song
markers remain still even though the scheduling clock continues.

## Tuning Notes

- Keep metronome level low enough that it does not mask the remote instrument.
- If timing feels unstable, inspect stats before changing metronome mode.
- For repeatable metronome comparisons, record jam stems and compare the generated WAVs and CSV logs.
- Metronome timing and impairment scenarios are documented in
  [Stress Tests](StressTests.md), with cross-machine measurements in
  [Benchmark](Benchmark.md).
