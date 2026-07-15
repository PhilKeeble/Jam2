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
the deterministic headless product suite.

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
- native numeric/boundary and controller-lifecycle validators, including the
  production 30000-ms/five-miss heartbeat policy and shortened debug expiry;
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
