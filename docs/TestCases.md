# Jam2 Python Test Tools

Run commands from the repository root:

```powershell
cd C:\Users\Phil\Documents\GitHub\Jam2
```

The default Jam2 binary path is `release\jam2.exe`. Override it with `--jam2` if needed.

## Local Stress Testing

`tools\run_stress_local.py` runs two local Jam2 processes through a localhost UDP impairment proxy. Use this for fast repeatable tests with two local audio interfaces and no real network involved.

Default profile is `fast`. Use `--profile all` to run each selected stress case against `fast`, `moderate`, and `safe`.

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --clean
```

Run the safe profile:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --profile safe --clean
```

Compare all three profiles for one scenario:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --profile all --scenario jitter-50 --clean
```

Run one scenario quickly:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --scenario clean-control
```

Run short stress passes:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --stream-ms 10000 --clean
```

Run the metronome timing scenarios with WAV recording/analysis:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --scenario metronome-shared-grid --scenario metronome-leader-audio --scenario metronome-symmetric-delay --scenario metronome-listener-compensated --clean
```

Include CLI/session/error validation checks:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --include-validation --clean
```

Include targeted recorded tone/pulse probes for audible artifact analysis under stress:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 48000 --include-audio-probes --clean
```

Useful args:

- `--server-audio-device`: local listen-side audio device id.
- `--client-audio-device`: local connect-side audio device id.
- `--sample-rate`: sample rate for both local interfaces.
- `--profile fast|moderate|safe|all`: choose one profile or run each selected scenario against all three. Default: `fast`.
- `--scenario NAME`: run only one scenario. Repeat for multiple scenarios.
- `--os-priority off|high|realtime|all`: pass the selected OS scheduling mode to both local Jam2 processes. Use `all` to run every selected scenario once with no explicit priority request, once with high priority, and once with realtime priority for direct comparison.
- `--stream-ms N`: stream duration per scenario. Default: `30000`.
- `--include-validation`: run short non-audio/error-path validation checks.
- `--include-audio-probes`: add targeted recorded tone/pulse probes for audible analysis.
- `--validation-stream-ms N`: stream duration for validation pair tests. Default: `5000`.
- `--logs PATH`: output folder. Default: `tools\stress_logs`.
- `--clean`: delete the output folder before running.
- `--seed N`: deterministic proxy impairment seed.

Main outputs:

- `tools\stress_logs\stress_summary.txt`
- `tools\stress_logs\stress_results.csv`
- `tools\stress_logs\stress_results.json`
- per-scenario `recording\*.wav` folders for metronome scenarios
- per-probe `recording\*.wav` and `audio_probe_analysis` data when `--include-audio-probes` is used
- `tools\stress_logs\validation_results.json` when `--include-validation` is used

Audio probe coverage:

- `audio-probe-clean-tone`: symmetric tone without injected impairment.
- `audio-probe-jitter-tone`: symmetric tone under high ordered jitter.
- `audio-probe-loss-server-to-client`: server tone, client silence, high loss.
- `audio-probe-loss-client-to-server`: client tone, server silence, high loss.
- `audio-probe-adaptive-on-pulse`: symmetric pulse under jitter/burst pressure.
- `audio-probe-adaptive-off-pulse`: same pulse pressure with adaptive cushion disabled.

## Two-Host Static Benchmarks

`tools\run_benchmark_server.py` runs the listen/server side of the static benchmark suite. It coordinates lifecycle state and client artifact upload over a direct TCP control connection, records server stems, waits for client artifacts, then writes the final summary/CSV/JSON. An HTTP `current.json` diagnostic endpoint is available only when explicitly enabled.

The server waits indefinitely for the first TCP benchmark client before publishing any case by default, so the listener machine can be started first and the client can join when ready.

Run this on the listen/server machine:

```powershell
python tools\run_benchmark_server.py --server-audio-device 5 --sample-rate 44100 --clean
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
- `--bind-control HOST:PORT`: TCP lifecycle control bind. Default: `0.0.0.0:49000`. This intentionally matches the default Jam2 UDP audio port number; TCP control and UDP audio are separate sockets.
- `--bind-http HOST:PORT`: optional HTTP diagnostic bind for `current.json`. Disabled by default and not used for uploads.
- `--initial-client-timeout-s N`: optional timeout for the initial TCP client wait. Default `0` waits indefinitely.
- `--post-listener-upload-grace-s N`: optional timeout for client artifact upload after the server-side Jam2 process exits. Default `0` waits indefinitely.
- `--finish-grace-s N`: wait for the client to acknowledge `all_done` after the last case. Default `300`.
- `--logs PATH`: output folder. Default: `tools\benchmark_logs`.
- `--stream-ms N`: stream duration per case. Default: `30000`.
- `--repeats N`: repeat count per case. Default: `1`.
- `--signals silence,tone-440,pulse-1s`: symmetric injected input signals for non-metronome cases. When `tone-440` is included, the suite also adds a small number of directional tone cases.
- `--no-metronome-cases`: skip metronome-only benchmark cases.
- `--list-cases`: print the static case list and exit.
- `--clean`: delete the output folder before running.

## Two-Host Benchmark Client

`tools\run_benchmark_client.py` connects to the benchmark TCP control plane, runs the connect-side Jam2 process for each offered case, records client stems, analyzes local WAVs, and uploads the artifacts back on the same TCP connection. The client reconnects the control channel if it drops; every run and upload carries a suite/case/run/attempt identity so stale results are rejected instead of being matched to a later case.

Run this on the client/connect machine:

```powershell
python tools\run_benchmark_client.py --server 192.168.1.50 --client-audio-device 16 --sample-rate 44100
```

Clean local client artifacts after successful upload:

```powershell
python tools\run_benchmark_client.py --server 192.168.1.50 --client-audio-device 16 --sample-rate 44100 --clean
```

Useful client args:

- `--server`: benchmark server host, for example `192.168.1.50`. The old diagnostic HTTP URL form also works.
- `--control`: optional TCP control endpoint. By default this uses the server host with TCP port `49000`, for example `192.168.1.50:49000`.
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
