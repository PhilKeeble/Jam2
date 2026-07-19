# Jam2 Stress Tests

Use `stress` for targeted feature regression, controlled UDP impairment,
recovery assertions, timing cases, and deterministic full-mesh tests. Stress is
different from validation: it deliberately challenges a named behavior and
retains the raw technical evidence.

Run commands from the repository root. The default executable is
`release\jam2.exe`; override it with `--jam2 PATH`.

## Start With A Targeted Run

When audio-device options are omitted, stress uses deterministic headless
audio. A short two-peer smoke is:

```powershell
python tools\jam2_test.py stress --headless-audio --profile fast --scenario clean-control --scenario duplicate-2.0 --sample-rate 48000 --stream-ms 8000 --clean
```

For a matched non-silent PCM comparison, add
`--network-audio-format both`. Each selected base scenario runs once as PCM16
and once as PCM24 under the same native profile, duration, impairment, and
seed-derived plan. A format case fails if the native session silently uses the
other format or bidirectional audio packets are absent.

```powershell
python tools\jam2_test.py stress --headless-audio --profile fast --network-audio-format both --scenario clean-control --scenario jitter-20 --scenario loss-0.5 --scenario reorder-small --sample-rate 48000 --stream-ms 5000
```

The bare command runs the complete standard two-peer catalog for one profile,
which is intentionally much longer:

```powershell
python tools\jam2_test.py stress
```

Select a case by repeating `--scenario`. `--profile all` multiplies the selected
cases across `fast`, `moderate`, and `safe`; `--os-priority all` multiplies them
again across `off`, `high`, and `realtime`. Use these matrices deliberately.
`--stream-ms` is a minimum duration: a retained case is automatically extended
when its last native action, impairment injection, or recovery assertion needs
more time. Both the requested and effective durations are written to
`result.json`.

## Real-Device Stress

Supply both device IDs for the bounded two-physical-device completion smoke.

```powershell
python tools\jam2_test.py stress --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --scenario clean-control --scenario jitter-50 --stream-ms 12000 --scenario-cooldown-s 2
```

This uses normal native callback/device paths on both local interfaces while
the UDP impairment proxy remains on localhost. Confirm both devices support the
same sample rate first. Device IDs are machine-local.

For the mixed lifecycle case, supply exactly one device. The other peer uses
the deterministic headless callback while the real-device peer still follows
the normal create/join, control, UDP, callback, stats, and artifact paths:

```powershell
python tools\jam2_test.py stress --server-audio-device 16 --profile fast --scenario clean-control --sample-rate 44100 --stream-ms 8000
```

Use `--headless-audio-buffer-frames N` only for the synthetic callback size.
Network frame size and other tuning continue to come from the native profile
plus the case's sparse overrides.

The device examples omit `--clean` so they can be run after the complete
headless family without deleting its retained evidence. Use `--clean` once on
the first intended stress invocation when a fresh family root is wanted.

## Full-Mesh Stress

Providing `--mesh-peers` selects the deterministic headless mesh catalog.
Physical device options are rejected in mesh mode.

```powershell
python tools\jam2_test.py stress --mesh-peers 3 --profile fast --scenario mesh-3-clean --scenario mesh-3-edge-jitter --scenario mesh-3-authority-last --sample-rate 48000 --stream-ms 12000 --clean
```

Repeat `--mesh-peers` to select more than one size. For a selected peer count
`N`, the catalog provides:

- `mesh-N-clean`;
- `mesh-N-independent-drift`;
- `mesh-N-authority-peer2`;
- `mesh-N-edge-jitter` and `mesh-N-authority-last` when `N` is at least three.

`--mesh-base-port 0` reserves available ports automatically. A nonzero value
requests a consecutive explicit range and fails clearly if TCP/UDP ownership
cannot be established.

## Retained Two-Peer Scenarios

The current catalog is grouped below. The exact definitions, impairments,
native scheduled actions, and expected technical behavior live in
`tools/jam2test/scenarios.py`.

- Baselines: `clean-control`, `clean-fast-control`,
  `clean-moderate-control`, `clean-safe-control`.
- Asymmetric local-profile comparison: select the macro
  `asymmetric-profile-comparison` to run Fast-creator/Safe-joiner,
  Safe-creator/Fast-joiner, and Safe/Safe controls in both clean and
  bidirectional jitter/burst-pressure forms. Results record the creator-owned
  session profile separately from each peer's local profile and expose the
  latency, jitter, RTT, and playback-depth fields needed for comparison.
  Headless runs retain the harness's common synthetic callback size so Windows
  scheduler resolution does not distort the frame-based duration; the named
  profiles still supply their distinct prefill, rings, jitter/playout,
  adaptive-cushion, and drift values. Use physical devices for callback-buffer
  comparisons.
- Delay, jitter, loss, and ordering: `jitter-20`, `jitter-50`, `jitter-100`,
  `burst-pause-250`, `burst-pause-500`, `burst-pause-1500`,
  `transient-stall-recovery`, `transient-stall-250-recovery`, `loss-0.1`,
  `loss-0.5`, `loss-1.0`,
  `reorder-small`, `duplicate-2.0`, `corrupt-1.0`.
- UDP validation and bounded recovery: `near-wrap-sequence`, `malformed-udp`,
  `delayed-replay`, `forward-sequence-gap`, `extreme-sample-time`,
  `udp-short-flood`.
- Buffer/adaptive comparisons: `adaptive-on-pressure`,
  `adaptive-off-pressure`, `jitter-buffer-512-pressure`,
  `jitter-buffer-512-max3072-pressure`,
  `jitter-buffer-1024-max3072-pressure`,
  `jitter-buffer-2048-max3072-pressure`,
  `jitter-buffer-2048-max4096-pressure`, `prefill-768-pressure`,
  `jitter-buffer-512-adaptive-off-pressure`,
  `prefill-768-adaptive-off-pressure`, `jitter-buffer-1024-jitter-100`,
  `jitter-buffer-512-reorder-small`.

The two transient recovery cases inject exactly one bidirectional 120 ms or
250 ms stall. Their verdict requires the adaptive target to rise and fall, the
bounded audio-control recovery ratio to be observed, steady packet/mixer flow,
and the actual playback ring—not only its target counter—to return within four
network packets of the configured minimum.
- Metronome and grid: `metronome-shared-grid`, `metronome-leader-audio`,
  `metronome-listener-compensated`, `grid-authority-client-shared-grid`,
  `grid-authority-client-leader-audio`,
  `grid-authority-client-listener-compensated`, `grid-authority-concurrent`,
  `grid-stop-restart-shared-grid`, `grid-noop-running-controls`,
  `last-peer-departure-grid-restart`.
- Track and transport: `transport-grid-authority`, `transport-track-actions`,
  `transport-track-actions-joiner`, `transport-record-start-joiner`,
  `transport-track-sync-off`.
- Recorded timing pressure: `metronome-listener-compensated-metro-pulse`,
  `metronome-listener-compensated-pulse-jitter`,
  `metronome-listener-compensated-pulse-burst`,
  `metronome-listener-compensated-pulse-loss`.
  The `metro-pulse` verdict evaluates creator and joiner independently. Both
  must compensate their own click, include every remote path, converge on the
  measured average target, and satisfy the recorded steady average/peak
  landing bounds; one good side cannot hide a failure on the other.
- Runtime/tuning checks: `levels-low`, `sample-time-playout-off`,
  `playout-delay-3072`, `drift-max-5ppm`, `drift-smoothing-fast`,
  `socket-buffers`, `channels-1-to-1`, `runtime-controls`.

Convenience matrix names expand to several retained cases:

- `adaptive-off-vs-on`;
- `burst-loss`;
- `jitter-buffer-adaptive-pressure-matrix`;
- `jitter-buffer-audio-vs-prefill`.

`--include-audio-probes` adds recorded tone, pulse, and metronome comparisons:

- `audio-probe-clean-tone`, `audio-probe-jitter-tone`;
- `audio-probe-loss-server-to-client`,
  `audio-probe-loss-client-to-server`;
- `audio-probe-adaptive-on-pulse`, `audio-probe-adaptive-off-pulse`;
- `audio-probe-jitter-buffer-512-tone`, `audio-probe-prefill-768-tone`;
- `audio-probe-jitter-buffer-512-metronome`,
  `audio-probe-prefill-768-metronome`.

## Scheduling And Impairment

Frame-sensitive actions are emitted as typed native debug actions scheduled
against engine events and frame delays. Python timeouts bound process failure;
they are not used as the authoritative transport/metronome clock.

The UDP proxy owns controlled loss, delay, jitter, reorder, duplication,
corruption, bursts, and packet transforms outside the Jam2 process. Every run
records its seed and per-direction proxy counters so proxy behavior can be
compared with native receive/rejection counters. A clean zero-impairment case
is the appropriate baseline before interpreting an impaired run.

A server packet that arrives before the proxy has learned the client's
ephemeral endpoint is retained as
`server_to_client_unroutable_before_client`; it is not counted as deliberate
proxy loss and cannot satisfy or fail a loss/jitter verdict.

## Artifacts And Verdicts

Default artifacts are isolated below:

```text
tools/stress_logs/<invocation-id>/
```

An explicit `--output PATH` creates `PATH/<invocation-id>`. The invocation ID
is a UTC minute timestamp, with a numeric suffix only for a same-minute
collision. Without a custom output, `--clean` clears only
`tools/stress_logs`; with one, it clears the exact custom root.

Important files are:

- `invocation-manifest.json`: arguments, native profile descriptions, case
  states, and bounded artifact inventory;
- `results.csv`: flattened comparison data across selected cases;
- `format-comparison.json` and `format-comparison.csv`: paired raw/delta/
  percentage measurements when `--network-audio-format both` is selected;
- `<case>/result.json`: scenario verdict, raw proxy counters, technical
  measurements, and failures/observations;
- `<case>/peer-N/`: scenario, native manifest, stdout/stderr, CSV, and any
  recorded WAV artifacts for each peer.

Two-peer results normally separate `protocol_verdict`, `duration_verdict`, and
`audio_health_verdict`. Deliberately disruptive cases can report
`expected_impairment`; that means the named impairment behavior was observed,
not that the audio was subjectively good. Raw loss, jitter, RTT, queue,
underrun, drift, callback, grid, transport, and proxy measurements remain the
primary evidence.

The paired report includes exact header/payload/packet bytes, packet and byte
flow, bitrate, native process CPU, callback timing, jitter, RTT, loss/reorder/
late/missing/drop/underrun, drift, mix, and recorded-WAV measurements when the
selected scenario produces recording analysis. Short CPU differences are
reported as raw data and are not treated as a recommendation.

Exit code `0` means all selected cases passed or produced their expected
impairment. Exit code `1` means a case verdict failed. Exit code `2` means the
framework could not execute the requested plan, such as invalid arguments,
missing devices, bad paths, or process infrastructure failure.

Use [Validation](Validation.md) for the clean post-build gate and
[Benchmark](Benchmark.md) for real cross-machine measurements.
