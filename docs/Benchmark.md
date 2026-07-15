# Jam2 Benchmarks

Use `benchmark` for repeatable two-machine measurements over the real direct
Jam2 network path. Python coordinates cases and moves artifacts after each run;
it never relays Jam2 audio.

Run all commands from the repository root. The default executable is
`release\jam2.exe`; use `--jam2 PATH` to select another build.

## Before Running

- Build Jam2 and confirm each audio device supports the requested sample rate
  with `jam2 test-device <id> --sample-rate <hz>`.
- Give the machines distinct `--machine-id` values. IDs may contain letters,
  numbers, dots, underscores, and hyphens.
- On the coordinator, allow TCP `49000` for Python benchmark control and both
  TCP and UDP `49001` for the normal Jam2 create/join path. The ports are
  independently configurable.
- Use a short `--output` parent on Windows. Native attempt paths are checked
  before the suite starts so generated CSV and WAV names remain within the
  Windows path budget.
- Start the coordinator first, then the agent.

`--network-profile auto|wired|wifi|unknown` records metadata only. It never
changes native tuning.

## Short Two-Machine Run

This is the short workflow used to prove negotiation, normal create/join,
recording, manifests, upload acknowledgement, result correlation, and final
`all_done` without running the comprehensive matrix.

Coordinator, using LAN address `192.168.1.50` and audio device `5`:

```powershell
python tools\jam2_test.py benchmark coordinator --machine-id studio-a --audio-device 5 --sample-rate 44100 --profile fast --case fast_silence --case fast_tone-440 --case fast_pulse-1s --signals silence,tone-440,pulse-1s --stream-ms 5000 --repeats 1 --control 0.0.0.0:49000 --audio-bind 0.0.0.0:49001 --public-audio-host 192.168.1.50 --initial-agent-timeout-s 120 --case-timeout-s 45 --upload-timeout-s 120 --case-retry-limit 1 --finish-grace-s 30 --output C:\j2c --clean
```

Agent, using audio device `16`:

```powershell
python tools\jam2_test.py benchmark agent --machine-id studio-b --audio-device 16 --sample-rate 44100 --coordinator 192.168.1.50:49000 --connect-timeout-s 120 --case-timeout-s 45 --output C:\j2a --clean
```

Do not use `--delete-after-upload` when the purpose of the run requires logs on
both machines. That option removes an agent attempt only after the coordinator
has acknowledged its validated upload.

Use `--headless-audio` instead of `--audio-device` when testing only the
orchestration and network workflow. Headless runs do not claim device, driver,
hardware-clock, or callback coverage.

## Cases And Matrices

List the cases produced by the current native profiles and retained matrix:

```powershell
python tools\jam2_test.py benchmark coordinator --machine-id catalog --list-cases --output build\benchmark-catalog
```

The listing records each case ID, native base profile, sparse override object,
signal, and repeat count. Select cases by repeating `--case`. The coordinator
owns the case plan offered to the agent.

Useful selection controls:

- `--profile fast|moderate|safe|all` filters by native base profile. The default
  is `all`.
- `--signals silence,tone-440,pulse-1s` selects a comma-separated signal subset.
- `--no-metronome-cases` removes metronome-only cases.
- `--stream-ms N` sets each selected case duration.
- `--repeats N` preserves each repeat as a separate run identity.
- `--case NAME` selects an exact catalog case and may be repeated.

The comprehensive workflow is intentionally retained. Omitting `--case` with
`--profile all` exercises the broad profile, sparse tuning, directional tone,
metronome, Wi-Fi diagnostic, and OS-priority matrices. It can take a long time,
especially with multiple repeats, and is not the routine smoke test.

## Coordinator And Agent Behavior

Every offered attempt has a coordinator-issued invocation ID, suite ID, case
ID, run index, and attempt ID. The agent accepts only the current bounded offer,
runs the normal `network.join` operation, packages its attempt, and streams it
over the benchmark TCP control connection. The coordinator validates the
envelope and peer result identity before extracting it.

If a case does not complete, `--case-retry-limit` bounds coordinator retries.
Reconnects and repeated offers retain their identity, while stale attempts and
uploads are rejected. `--initial-agent-timeout-s`, `--case-timeout-s`,
`--upload-timeout-s`, `--connect-timeout-s`, and `--finish-grace-s` bound the
relevant waits.

## Artifacts

Without `--output`, artifacts are placed below:

```text
tools/benchmark_logs/<invocation-id>/
```

With `--output C:\j2c`, the family folder is still included:

```text
C:/j2c/benchmark_logs/<invocation-id>/
```

The normalized attempt tree is:

```text
suites/<suite-id>/machines/<machine-id>/cases/<case-id>/
  runs/run-<number>/attempts/<attempt-id>/
```

The coordinator retains its own machine subtree and the validated uploaded
agent subtree. The agent retains its local subtree unless
`--delete-after-upload` was explicitly used. Named files include:

- root `invocation-manifest.json` and role-specific `coordinator.log` or
  `agent.log`;
- root `transfer.log` with upload lifecycle events;
- per-attempt `scenario.json`, `native-manifest.json`, `peer-result.json`,
  `jam2.stdout.log`, and `jam2.stderr.log`;
- per-attempt `csv/*.csv` and `recording/*.wav` plus `recording.json`;
- coordinator-side `correlated-result.json`.

The invocation manifest records bounded artifact paths, byte counts, and
SHA-256 hashes. A successful short run has `state: "passed"`, `return_code: 0`,
and `all_done_acknowledged: true` on both machines. Each coordinator result
must have `verdict: "complete"`, `agent_artifacts_received: true`, and two
distinct peer/machine records.

`--clean` removes only the selected output parent's entire `benchmark_logs`
family before allocating a new invocation. It does not clean validation,
stress, or connectivity artifacts.

## Offline Analysis

Analyze a coordinator invocation containing `correlated-result.json` files:

```powershell
python tools\jam2_test.py benchmark analyze C:\j2c\benchmark_logs\<invocation-id> --output C:\j2-analysis --clean
```

The analyzer creates another isolated benchmark invocation containing
`analysis.json`, `analysis.csv`, and its own invocation manifest. It preserves
case- and repeat-level raw measurements; it does not produce a subjective
playability score or silently choose a profile.

For controlled local impairment and feature recovery, use
[Stress Tests](StressTests.md). For the deterministic post-build baseline, use
[Validation](Validation.md).
