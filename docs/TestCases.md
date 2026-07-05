# Jam2 Python Test Tools

Run commands from the repository root:

```powershell
cd C:\Users\Phil\Documents\GitHub\Jam2
```

The default Jam2 binary path is `release\jam2.exe`. Override it with `--jam2` if needed.

## Local Stress Testing

`tools\run_stress_local.py` runs two local Jam2 processes through a localhost UDP impairment proxy. Use this when you want fast, repeatable tests with two real local audio interfaces and no real network involved.

Default profile is aggressive: `32/64/768`.

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --clean
```

Run the old safe profile:

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
- `tools\stress_logs\validation_results.json` when `--include-validation` is used

## Two-Host Matrix Tuning

`tools\run_matrix_server.py` coordinates two-host profile testing. Run this on the listen/server machine.

Adaptive profile search:

```powershell
python tools\run_matrix_server.py --server-audio-device 5 --sample-rate 44100 --host 0.0.0.0 --port 8000 --clean
```

Benchmark mode:

```powershell
python tools\run_matrix_server.py --server-audio-device 5 --sample-rate 44100 --host 0.0.0.0 --port 8000 --benchmark --clean
```

No STUN:

```powershell
python tools\run_matrix_server.py --server-audio-device 5 --sample-rate 44100 --no-stun --clean
```

Useful args:

- `--server-audio-device`: server/listen-side audio device id.
- `--sample-rate`: audio sample rate.
- `--host`: HTTP control server bind address. Default: `0.0.0.0`.
- `--port`: HTTP control server port. Default: `8000`.
- `--logs PATH`: output folder. Default: `tools\logs`.
- `--clean`: delete the output folder before running.
- `--no-stun`: use local/manual endpoint behavior.
- `--network-profile auto|wired|wifi|unknown`: tolerance metadata.
- `--drift-probes`: add drift deadband probe runs after stable profile detection.
- `--benchmark`: run benchmark mode instead of adaptive search.

## Two-Host Matrix Client

`tools\run_matrix_client.py` polls the matrix server and runs the connect-side tests. Run this on the client/connect machine.

```powershell
python tools\run_matrix_client.py --server http://192.168.1.50:8000 --client-audio-device 16 --sample-rate 44100
```

Clean local logs after upload:

```powershell
python tools\run_matrix_client.py --server http://192.168.1.50:8000 --client-audio-device 16 --sample-rate 44100 --clean
```

Useful args:

- `--server`: matrix server URL, for example `http://192.168.1.50:8000`.
- `--client-audio-device`: client/connect-side audio device id.
- `--sample-rate`: audio sample rate.
- `--logs PATH`: local output folder. Default: `tools\logs`.
- `--poll-ms N`: polling interval. Default: `500`.
- `--timeout-s N`: stop waiting after N seconds. `0` waits forever.
- `--server-gone-grace-s N`: exit after server disappears following completed runs. Default: `10`.
- `--clean`: remove uploaded local run logs.
- `--network-profile auto|wired|wifi|unknown`: client network metadata.

## Collect Matrix CSV

`tools\collect_matrix_csv.py` combines per-run `stats.csv` files into one CSV.

```powershell
python tools\collect_matrix_csv.py --logs tools\logs
```

Server side only:

```powershell
python tools\collect_matrix_csv.py --logs tools\logs --side server
```

Custom output:

```powershell
python tools\collect_matrix_csv.py --logs tools\logs --output artifacts\matrix_combined.csv
```

Useful args:

- `--logs PATH`: matrix log folder. Default: `tools\logs`.
- `--output PATH`: output CSV path. Default is inside the log folder.
- `--side server|client|all`: side filter. Default: `all`.

## Analyze Matrix CSV

`tools\analyze_matrix_csv.py` summarizes collected matrix CSVs and applies threshold filters.

Analyze from logs:

```powershell
python tools\analyze_matrix_csv.py --logs tools\logs
```

Analyze an existing combined CSV:

```powershell
python tools\analyze_matrix_csv.py --input tools\logs\combined_stats.csv
```

Write analysis output:

```powershell
python tools\analyze_matrix_csv.py --input tools\logs\combined_stats.csv --output artifacts\matrix_analysis.csv
```

Example threshold run:

```powershell
python tools\analyze_matrix_csv.py --logs tools\logs --max-loss-percent 0.05 --max-playback-underrun-events-per-second 0.10 --min-playback-depth-ms 2.5
```

Useful args:

- `--logs PATH`: collect logs before analysis.
- `--input PATH`: existing combined CSV.
- `--output PATH`: output analysis CSV.
- `--max-loss N`: maximum total packet loss.
- `--max-loss-percent N`: maximum packet loss percent.
- `--max-loss-per-second N`: maximum loss rate.
- `--max-playback-underruns N`: maximum underrun frames.
- `--max-playback-underruns-per-second N`: maximum underrun frame rate.
- `--max-playback-underrun-events-per-second N`: maximum underrun event rate.
- `--max-playback-overruns N`: maximum playback overruns.
- `--max-playback-dropped-frames-per-second N`: maximum dropped playback frame rate.
- `--min-playback-depth-ms N`: minimum acceptable playback depth.
- `--min-runs N`: minimum runs required for a profile.

## Support Modules

These files are imported by the runnable tools and are not normally run directly:

- `tools\matrix_common.py`: shared paths, JSON writing, log helpers.
- `tools\jam2_profiles.py`: shared stress profile definitions.
- `tools\jam2_harness.py`: Jam2 process and CSV collection helpers.
- `tools\jam2_metrics.py`: stress CSV summarizing and result CSV writing.
- `tools\udp_stress_proxy.py`: localhost UDP impairment proxy.
