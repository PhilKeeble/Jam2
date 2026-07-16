# Jam2 Validation

`validate` is the clean deterministic post-build baseline. It checks the Python
framework and the externally visible native product contracts without running
the long stress or benchmark matrices.

Run commands from the repository root. The default executable is
`release\jam2.exe`; select another build with `--jam2 PATH`.

## Full Baseline

After building, run:

```powershell
python tools\jam2_test.py validate
```

This is equivalent to `validate all`. It performs the framework self-tests and
the deterministic headless product suite. Product validation includes five
10-second, four-process production-flow cases, so allow roughly 100 seconds on
a typical development machine.

Use a separate output parent when retaining evidence for a particular change:

```powershell
python tools\jam2_test.py validate all --clean
```

The resulting root is `tools/validate_logs/<invocation-id>`, not `tools`
itself. With `--output PATH`, it is
`PATH/validate_logs/<invocation-id>`.

## Selections

```powershell
python tools\jam2_test.py validate framework
python tools\jam2_test.py validate product
python tools\jam2_test.py validate all
```

- `framework` runs the dependency-free Python unit suite. The dispatcher still
  interrogates `jam2 debug describe` first so the manifest and coverage map
  describe the selected native executable.
- `product` runs native public-surface, schema, boundary, lifecycle, local,
  reactive-channel, handle-isolation, and clean mesh checks.
- `all` runs both and is the default completion baseline.

## Current Product Coverage

The product selection verifies:

- public `jam2` help/command surfaces and rejection of the former public
  `listen`, `connect`, and `mesh` aliases at both root and network levels;
- parity among `debug describe`, the unversioned scenario schema, admitted
  operations, runtime fields, and typed actions;
- native numeric/boundary and controller-lifecycle validators, including UDP
  v2 PCM16/PCM24 golden bytes/tags and exact sizes, authenticated binary asset
  fragmentation/coalescing and state bounds, the production 30000-ms/five-miss
  heartbeat policy, and shortened debug expiry;
- authenticated real-process TCP fragmentation, admission, authorization,
  nested remote-model, asset/WAV, endpoint, and bounded-flood hardening;
- one clean local effective-configuration run;
- rejection of obsolete `*-v1`, unknown, wrongly typed, and out-of-range local
  automation input;
- bounded inherited reactive commands/events and both controller-loss policies;
- rejection of automation handles by GUI, ordinary CLI, describe, and static
  debug launches;
- clean two-, three-, and four-peer create/join runs through the universal
  direct-mesh engine, including continued packet/mix progress for two survivors
  after an ordinary third peer leaves.
- five same-platform public-command runs using one creator and three joiners,
  launched only through `jam2 network create` and `jam2 network join` with no
  inherited debug automation channel: clean silence, bidirectional tone/pulse
  injection, shared-grid metronome timing, leader-audio distribution, and
  listener-compensated `metro-pulse` timing. Shared-grid requires one common
  authority/revision plus a tight five-millisecond mapped-grid and click-interval
  bound on every peer. Listener-compensated requires each listener's locally
  rendered click to remain within the looser 80-millisecond best-effort bound
  against the mixture of remote metro pulses. Leader-audio requires one
  authoritative click source, silent local metronomes on all listeners, and
  a continuous 440 Hz leader signal plus timing-correct embedded clicks in
  every listener's received-audio stem. Each case records and analyzes 10 seconds on all four peers,
  checks all six direct mesh edges in both directions, and retains each peer's
  stdout, stderr, CSV, five WAV stems, and recording sidecar.

`coverage-map.json` classifies every public CLI option and scenario field as
automated or explicitly device/manual-only. Unclassified surfaces fail the
validation run rather than disappearing from the report.

## Optional Real Device

Add one short native-device extension while retaining the full headless
baseline:

```powershell
python tools\jam2_test.py validate all --real-device 5 --clean
```

The extension runs a short local tone scenario and verifies that the native
manifest reports the requested device. It currently uses the baseline 48 kHz
configuration, so confirm support first:

```powershell
.\release\jam2.exe test-device 5 --sample-rate 48000
```

This is a single-device extension. It does not claim mixed-device,
cross-machine, GUI, or broader lifecycle coverage.

## Artifacts And Results

Without `--output`, each invocation is created under:

```text
tools/validate_logs/<invocation-id>/
```

Key files include:

- `invocation-manifest.json`: selection, case states, return code, omissions,
  native profiles, coverage reference, and bounded artifact hashes;
- `debug-description.json`: authoritative native automation/profile
  description consumed by the framework;
- `coverage-map.json`: public CLI and scenario-field coverage classification;
- per-case stdout, stderr, scenarios, native manifests, CSV, events, and
  process results where applicable.

`--clean` removes only the selected parent's complete `validate_logs` family
before creating the new unique invocation. Stress, benchmark, and connectivity
roots are never removed by validation cleanup.

## Console Output

Validation does not mirror child-process logs to the console. It streams one
start line and one result line per case, then one summary, for example:

```text
[RUN ] 13/17 public-cli-network-clean
[PASS] 13/17 public-cli-network-clean (15.1s) peers=4 active=3 sent=90409 recv=89929
[RUN ] 14/17 public-cli-network-tone
[PASS] 14/17 public-cli-network-tone (16.2s) tone=440.4-440.4Hz pulses=9-10
[RUN ] 15/17 public-cli-metronome-shared-grid
[PASS] 15/17 public-cli-metronome-shared-grid (15.1s) clicks=20-22 max_interval_error=2.0ms max_grid_error=2.7ms
[RUN ] 16/17 public-cli-metronome-leader-audio
[PASS] 16/17 public-cli-metronome-leader-audio (16.0s) leader_clicks=20-20 max_interval_error=0.1ms tone=440.4-440.4Hz source_peers=1
[RUN ] 17/17 public-cli-metronome-metro-pulse
[PASS] 17/17 public-cli-metronome-metro-pulse (16.0s) steady_matches=12-12 max_error=22.4ms
[SUMMARY] PASS 17/17 (99.0s) artifacts=C:\path\to\Jam2\tools\validate_logs\<invocation-id>
```

Failures name the peer and reason where possible and include the relevant case
artifact directory. Setup failures use `INFRASTRUCTURE-ERROR` rather than
presenting an incomplete run as a product assertion failure. `validate all`
reports 18 baseline cases because it adds the framework self-test; an optional
`--real-device` adds one more case.

Manifest states and exit codes distinguish outcomes:

- `state: "passed"`, exit `0`: all selected checks passed;
- `state: "failed"`, exit `1`: a framework or product assertion failed;
- `state: "infrastructure-error"`, exit `2`: the requested run could not be
  executed or inspected reliably.

The manifest's `omissions` field remains explicit. Headless validation does not
claim GUI interaction or physical audio behavior unless `--real-device` was
supplied.

Use [Stress Tests](StressTests.md) for controlled impairment/recovery and
[Benchmark](Benchmark.md) for two-machine device/network measurements.
