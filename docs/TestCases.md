# Jam2 Python Test Tools

Run commands from the repository root:

```powershell
cd C:\Users\Phil\Documents\GitHub\Jam2
```

The default Jam2 binary path is `release\jam2.exe`. Override it with `--jam2` if needed.

## Local Stress Testing

`tools\run_stress_local.py` runs two local Jam2 processes through a localhost UDP impairment proxy. Use this for fast repeatable tests with two local audio interfaces and no real network involved.

Default profile is aggressive: `32/64/768`.

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --clean
```

Run the safe profile:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile safe --clean
```

Compare aggressive and safe for one scenario:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile both --scenario jitter-50 --clean
```

Run one scenario quickly:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --scenario clean-control
```

Run short stress passes:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --stream-ms 10000 --clean
```

Run the metronome timing scenarios with WAV recording/analysis:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --scenario metronome-shared-grid --scenario metronome-leader-audio --scenario metronome-symmetric-delay --scenario metronome-listener-compensated --clean
```

Include CLI/session/error validation checks:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --include-validation --clean
```

Useful args:

- `--server-audio-device`: local listen-side audio device id.
- `--client-audio-device`: local connect-side audio device id.
- `--sample-rate`: sample rate for both local interfaces.
- `--profile aggressive|safe|both`: choose profile family. Default: `aggressive`.
- `--scenario NAME`: run only one scenario. Repeat for multiple scenarios.
- `--stream-ms N`: stream duration per scenario. Default: `30000`.
- `--include-validation`: run short non-audio/error-path validation checks.
- `--validation-stream-ms N`: stream duration for validation pair tests. Default: `5000`.
- `--logs PATH`: output folder. Default: `tools\stress_logs`.
- `--clean`: delete the output folder before running.
- `--seed N`: deterministic proxy impairment seed.

Main outputs:

- `tools\stress_logs\stress_summary.txt`
- `tools\stress_logs\stress_results.csv`
- `tools\stress_logs\stress_results.json`
- per-scenario `recording\*.wav` folders for metronome scenarios
- `tools\stress_logs\validation_results.json` when `--include-validation` is used

## Two-Host Static Benchmarks

`tools\run_benchmark_server.py` runs the listen/server side of the static benchmark suite. It publishes each fixed benchmark case over HTTP, runs the listener, records server stems, waits for client artifacts, then writes the final summary/CSV/JSON.

Run this on the listen/server machine:

```powershell
python tools\run_benchmark_server.py --server-audio-device 5 --sample-rate 44100 --bind-http 0.0.0.0:8000 --clean
```

List the benchmark cases without launching Jam2:

```powershell
python tools\run_benchmark_server.py --server-audio-device 5 --sample-rate 44100 --list-cases
```

Run a short tone benchmark pass. This includes symmetric tone cases plus targeted server-to-client and client-to-server tone cases for the baseline profiles:

```powershell
python tools\run_benchmark_server.py --server-audio-device 5 --sample-rate 44100 --signals tone-440 --no-metronome-cases --stream-ms 10000 --clean
```

Useful server args:

- `--server-audio-device`: server/listen-side audio device id.
- `--sample-rate`: audio sample rate.
- `--bind-http HOST:PORT`: HTTP control/artifact server bind. Default: `0.0.0.0:8000`.
- `--logs PATH`: output folder. Default: `tools\benchmark_logs`.
- `--stream-ms N`: stream duration per case. Default: `30000`.
- `--repeats N`: repeat count per case. Default: `1`.
- `--signals silence,tone-440,pulse-1s`: symmetric injected input signals for non-metronome cases. When `tone-440` is included, the suite also adds a small number of directional tone cases.
- `--no-metronome-cases`: skip metronome-only benchmark cases.
- `--list-cases`: print the static case list and exit.
- `--clean`: delete the output folder before running.

## Two-Host Benchmark Client

`tools\run_benchmark_client.py` polls the benchmark server, runs the connect-side Jam2 process for each published case, records client stems, analyzes local WAVs, and uploads the artifacts back to the server.

Run this on the client/connect machine:

```powershell
python tools\run_benchmark_client.py --server http://192.168.1.50:8000 --client-audio-device 16 --sample-rate 44100
```

Clean local client artifacts after successful upload:

```powershell
python tools\run_benchmark_client.py --server http://192.168.1.50:8000 --client-audio-device 16 --sample-rate 44100 --clean
```

Useful client args:

- `--server`: benchmark server URL, for example `http://192.168.1.50:8000`.
- `--client-audio-device`: client/connect-side audio device id.
- `--sample-rate`: audio sample rate.
- `--logs PATH`: local output folder. Default: `tools\benchmark_logs`.
- `--poll-ms N`: polling interval. Default: `500`.
- `--timeout-s N`: stop waiting after N seconds. `0` waits forever.
- `--clean`: remove local artifacts after upload.
- `--network-profile auto|wired|wifi|unknown`: client network metadata.

Benchmark outputs on the server:

- `tools\benchmark_logs\benchmark_summary.txt`
- `tools\benchmark_logs\benchmark_results.csv`
- `tools\benchmark_logs\benchmark_results.json`
- per-case server/client `recording\*.wav`
- per-case server/client `analysis.json`
- per-case server/client stdout/stderr/stats artifacts

Tone coverage:

- `tone-440`: both peers inject tone.
- `tone-server-to-client`: server injects tone, client injects silence.
- `tone-client-to-server`: client injects tone, server injects silence.

## Analyze Benchmark Results

The benchmark server writes the main CSV/text/JSON outputs automatically. `tools\analyze_benchmark_results.py` is only a lightweight helper for re-printing or exporting a compact table from an existing `benchmark_results.json`.

```powershell
python tools\analyze_benchmark_results.py tools\benchmark_logs\benchmark_results.json
```

Write a regenerated summary and flattened CSV:

```powershell
python tools\analyze_benchmark_results.py tools\benchmark_logs\benchmark_results.json --text artifacts\benchmark_summary.txt --csv artifacts\benchmark_results.csv
```

## Support Modules

These files are imported by the runnable tools and are not normally run directly:

- `tools\jam2_tooling.py`: shared paths, JSON writing, CSV copy, and log helpers.
- `tools\jam2_profiles.py`: shared profile definitions.
- `tools\jam2_benchmark_suite.py`: static benchmark case definitions.
- `tools\jam2_audio_analysis.py`: WAV/stem analysis helpers.
- `tools\jam2_harness.py`: Jam2 process and CSV collection helpers.
- `tools\jam2_metrics.py`: stress CSV summarizing and result CSV writing.
- `tools\udp_stress_proxy.py`: localhost UDP impairment proxy.
