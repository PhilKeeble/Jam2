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
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --clean
```

Run the safe profile:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile safe --clean
```

Compare all three profiles for one scenario:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile all --scenario jitter-50 --clean
```

Run one scenario quickly:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --scenario clean-control
```

Validate recovery from one 120 ms bidirectional delivery stall. This case
requires packet flow, mixer occupancy, adaptive padding, and the adaptive
target to be recovering during the final five seconds:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --os-priority realtime --scenario transient-stall-recovery --stream-ms 22000 --logs tools\stress_logs_phase7_udp_recovery --clean
```

Run short stress passes:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --stream-ms 10000 --clean
```

Run headless mesh stress without ASIO/CoreAudio devices:

```powershell
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile all --clean
```

Run one mesh size/profile quickly:

```powershell
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 4 --stream-ms 10000 --clean
```

Run the metronome timing scenarios with WAV recording/analysis:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --scenario metronome-shared-grid --scenario metronome-leader-audio --scenario metronome-symmetric-delay --scenario metronome-listener-compensated --clean
```

Run the focused shared-grid Stop/Start revision and epoch case with headless audio:

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --scenario grid-stop-restart-shared-grid --stream-ms 12000 --clean
```

Verify that repeated unchanged metronome controls neither replace a running
authority epoch nor lose the absolute beat position when the second process
adopts it:

```powershell
python tools\run_stress_local.py --headless-audio --sample-rate 48000 --scenario grid-noop-running-controls --stream-ms 15000 --clean
```

Run the focused peer-initiated recording reset and automatic authority-departure recovery cases:

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --scenario transport-record-start-joiner --scenario last-peer-departure-grid-restart --stream-ms 12000 --clean
```

The ordinary shared-grid verdicts compare the final periodic row before harness shutdown. This avoids treating the intentional survivor authority recovery, after the harness closes its first peer, as an active-jam disagreement. The departure case separately validates that final recovery transition.

Include CLI/session/error validation checks:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --include-validation --clean
```

Include targeted recorded tone/pulse probes for audible artifact analysis under stress:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --include-audio-probes --clean
```

Useful args:

- `--mode normal|mesh`: normal runs one `network create` plus one `network join` with real or headless audio; mesh runs the same public create/join bootstrap with multiple headless peers and direct UDP edges.
- `--server-audio-device`: local creator-side audio device id.
- `--client-audio-device`: local joiner-side audio device id.
- `--sample-rate`: sample rate for both local interfaces.
- `--profile fast|moderate|safe|all`: choose one profile or run each selected scenario against all three. Default: `fast`.
- `--scenario NAME`: run only one scenario. Repeat for multiple scenarios.
- `--os-priority off|high|realtime|all`: pass the selected OS scheduling mode to both local Jam2 processes. Use `all` to run every selected scenario once with no explicit priority request, once with high priority, and once with realtime priority for direct comparison.
- `--stream-ms N`: stream duration per scenario. Default: `30000`.
- `--scenario-cooldown-s N`: recovery time between scenario processes. Default: `2.0`; set it to `0` only when deliberately measuring immediate reopen behavior.
- `--include-validation`: run short non-audio/error-path validation checks.
- `--include-audio-probes`: add targeted recorded tone/pulse probes for audible analysis.
- `--validation-stream-ms N`: stream duration for validation pair tests. Default: `5000`.
- `--logs PATH`: output folder. Default: `tools\stress_logs`.
- `--clean`: delete the output folder before running.
- `--seed N`: deterministic proxy impairment seed.
- `--mesh-peers N`: mesh peer count for `--mode mesh`; repeat for multiple. Default mesh counts are 2, 3, 4, and 8.
- `--mesh-base-port PORT`: first localhost UDP port used by mesh stress. Default: `0`, which reserves available ports automatically; a nonzero value requests consecutive explicit ports and fails clearly if any cannot be bound.
- `--headless-audio-buffer-frames N`: synthetic callback size used by headless cases. Default: `1024`, large enough for reliable wall-clock pacing with ordinary Windows timer scheduling while the network frame size remains controlled by the selected profile.

Mesh scenarios include clean/authority cases plus `mesh-N-independent-drift` for per-peer synthetic clocks and `mesh-N-edge-jitter` for one impaired edge with all other edges direct.

Main outputs:

- `tools\stress_logs\stress_summary.txt`
- `tools\stress_logs\stress_results.csv`
- `tools\stress_logs\stress_results.json`
- per-peer mesh stdout/stderr/stats/recording folders when `--mode mesh` is used
- per-scenario `recording\*.wav` folders for metronome scenarios
- per-probe `recording\*.wav` and `audio_probe_analysis` data when `--include-audio-probes` is used
- `tools\stress_logs\validation_results.json` when `--include-validation` is used

Stress results separate three decisions:

- `protocol_verdict` checks the behavior the scenario intended to exercise, such as duplicate, replay, malformed-packet, or sample-time rejection.
- `duration_verdict` requires every participating process—and every recording made by a headless case—to cover at least 90% of the requested stream duration.
- `audio_health_verdict` checks clean transport-validation cases for capacity drops, playback overruns, dropped or late frames, unexpected missing frames, jitter-buffer drops, and underruns. Forward-gap and extreme-sample-time cases allow only the two deliberately rejected audio packets' worth of missing frames.
- `audio_health_observations` records callback gaps beyond twice the expected period and bounded forced jitter releases. These remain prominent measurements but do not by themselves claim an audible discontinuity when playback counters are otherwise clean.

The overall `verdict` fails if any applicable decision fails. Scenarios that intentionally inject ordinary loss, jitter, or blackouts retain `audio_health_verdict=not_evaluated`, because discontinuities are part of those cases and need scenario-specific interpretation. The raw counters remain in CSV and JSON for comparison.

The headless duration and callback checks are independent of peer count. Two-person and multi-peer cases use the same public TCP bootstrap and the same `NetworkSession`, `PeerStream`, and mixer implementation.

Run the dependency-free verdict checks directly with Python:

```powershell
python -m unittest discover -s tools -p "test_*.py"
```

The zero-delay proxy path forwards packets immediately after observation or transformation. Its timed heap is used only when a scenario actually requests delay, jitter, reordering, or burst pauses, preventing the validation harness itself from manufacturing a startup packet burst.

Audio probe coverage:

- `audio-probe-clean-tone`: symmetric tone without injected impairment.
- `audio-probe-jitter-tone`: symmetric tone under high ordered jitter.
- `audio-probe-loss-server-to-client`: server tone, client silence, high loss.
- `audio-probe-loss-client-to-server`: client tone, server silence, high loss.
- `audio-probe-adaptive-on-pulse`: symmetric pulse under jitter/burst pressure.
- `audio-probe-adaptive-off-pulse`: same pulse pressure with adaptive cushion disabled.

## Two-Host Benchmarks

`tools\run_benchmark_server.py` runs the creator side of the benchmark suite. It coordinates lifecycle state and client artifact upload over one direct TCP control connection, records server stems, waits for client artifacts, then writes the final summary/CSV/JSON. Jam2 UDP audio still travels directly between the two unified Jam2 processes and is never relayed by the benchmark control connection.

The server waits indefinitely for the first TCP benchmark client before publishing any case by default, so the creator machine can be started first and the client can join when ready.

Each offered run carries a `suite_id`, `case_id`, `run_index`, and `attempt_id`. If the client TCP connection drops before the client reports that its Jam2 child finished, the server abandons that attempt and retries the same logical run with a new `attempt_id`. The server and client clean that run's local artifact folder before retrying, so stale files do not collide with the new attempt. If TCP drops after the client finished Jam2, the server waits for the client to reconnect and upload the already completed artifacts.

Jam2 itself rejects a run if the active audio device sample rate differs from the requested `--sample-rate`, so a device that silently starts at another rate becomes a clear child-process failure instead of misleading benchmark warnings.

Run this on the creator/server machine:

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

Run one short end-to-end case for control/upload testing:

```powershell
python tools\run_benchmark_server.py --server-audio-device 5 --sample-rate 44100 --signals tone-440 --no-metronome-cases --stream-ms 5000 --case fast_os_priority_off_tone-440 --clean
```

Useful server args:

- `--server-audio-device`: creator-side audio device id.
- `--sample-rate`: audio sample rate.
- `--bind-control HOST:PORT`: TCP lifecycle control bind. Default: `0.0.0.0:49000`. This intentionally matches the default Jam2 UDP audio port number; TCP control and UDP audio are separate sockets.
- `--bind-http HOST:PORT`: optional HTTP diagnostic bind for `current.json`. Disabled by default and not used for uploads.
- `--initial-client-timeout-s N`: optional timeout for the initial TCP client wait. Default `0` waits indefinitely.
- `--client-upload-timeout-s N`: active-case timeout while waiting for accept/connect/finish/upload. Default `0` waits indefinitely.
- `--post-listener-upload-grace-s N`: optional timeout for client artifact upload after the creator-side Jam2 process exits. Default `0` waits indefinitely. The flag name is retained for script compatibility.
- `--case-retry-limit N`: retry count after client TCP disconnects before the Jam2 child has finished. Default `3`; `0` retries indefinitely.
- `--finish-grace-s N`: wait for the client to acknowledge `all_done` after the last case. Default `300`.
- `--logs PATH`: output folder. Default: `tools\benchmark_logs`.
- `--stream-ms N`: stream duration per case. Default: `30000`.
- `--repeats N`: repeat count per case. Default: `1`.
- `--signals silence,tone-440,pulse-1s`: symmetric injected input signals for non-metronome cases. When `tone-440` is included, the suite also adds a small number of directional tone cases.
- `--no-metronome-cases`: skip metronome-only benchmark cases.
- `--list-cases`: print the fixed benchmark case list and exit.
- `--clean`: delete the output folder before running.

## Two-Host Benchmark Client

`tools\run_benchmark_client.py` connects to the benchmark TCP control plane, runs the joiner-side Jam2 process for each offered case, records client stems, analyzes local WAVs, and uploads the artifacts back on the same TCP connection. The client reconnects the control channel if it drops; every run and upload carries a suite/case/run/attempt identity so stale results are rejected instead of being matched to a later case. If the server retries a run with a new attempt while the client still has an old Jam2 child running, the client terminates the stale child and follows the new offer.

Run this on the joiner/client machine:

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
- `--client-audio-device`: joiner-side audio device id.
- `--sample-rate`: audio sample rate.
- `--logs PATH`: local output folder. Default: `tools\benchmark_logs`.
- `--poll-ms N`: control-loop wait interval while idle. Default: `500`.
- `--timeout-s N`: stop waiting after N seconds. `0` waits forever.
- `--finish-idle-s N`: optional fallback exit if the server disappears after completed work. Default `0` waits for `all_done`.
- `--post-upload-pause-s N`: wait after each upload before accepting the next offered case. Default `5`.
- `--case-timeout-s N`: kill a Jam2 child after this many seconds. Default `0` derives from stream/wait time.
- `--use-published-audio-host`: use the Jam2 URL host exactly as published by the server instead of rewriting it to the `--server` host.
- `--clean`: remove local artifacts after upload.
- `--network-profile auto|wired|wifi|unknown`: client network metadata.

Benchmark outputs on the server:

- `tools\benchmark_logs\benchmark_summary.txt`
- `tools\benchmark_logs\benchmark_results.csv`
- `tools\benchmark_logs\benchmark_results.json`
- per-case server/client `recording\*.wav`
- per-case server/client `analysis.json`
- per-case server/client stdout/stderr/stats artifacts

Useful result tags:

- `control_disconnect`: TCP control disconnected during the active case. If this appears with valid artifacts, the upload still completed.
- `active_sample_rate_mismatch` or `*_active_sample_rate_differs`: a device did not run at the requested sample rate. Current Jam2 builds should fail hard before streaming in this case.
- `jitter_buffer_dropped_packets`, `jitter_buffer_dropped_frames`, `missing_audio_frames`, `playback_dropped_frames`, `underrun_high`: raw audio-path instability measurements from the Jam2 child stats.
- `their-input_tone_frequency_mismatch`: the remote received recording did not contain the expected 440 Hz tone. Check sample-rate tags first, then network/dropout stats.

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
- `tools\jam2_benchmark_suite.py`: fixed benchmark case definitions.
- `tools\jam2_audio_analysis.py`: WAV/stem analysis helpers.
- `tools\jam2_harness.py`: Jam2 process and CSV collection helpers.
- `tools\jam2_metrics.py`: stress CSV summarizing and result CSV writing.
- `tools\udp_stress_proxy.py`: localhost UDP impairment proxy.
