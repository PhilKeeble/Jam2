# Jam2 Quick Run

Repository layout:

- `libs/jam2-core`: shared low-latency audio, protocol, STUN, socket, and utility code.
- `apps/jam2-cli`: current `jam2` command-line audio engine.
- `apps/jam2-gui` and `apps/jam2-capture`: Qt controller app and standalone WAV capture utility.
- `tests`: CMake/CTest tests for shared code and app behavior.
- `artifacts`: preserved local benchmark logs and release binaries.
- `release`: app binaries produced by CMake for easy distribution.

Build:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`jam2-gui` and `jam2-capture` are enabled by default for new CMake build directories and are written to `release` beside the `jam2` binary. If an existing build directory was configured before those targets were added, reconfigure with `-DJAM2_BUILD_GUI=ON -DJAM2_BUILD_CAPTURE=ON` or use a fresh build directory.

The GUI needs Qt 6 Widgets, Network, and Multimedia development packages. If CMake cannot find Qt automatically, pass its install prefix:

```powershell
cmake -S . -B build -G Ninja -DJAM2_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64
```

Check devices on each host:

```powershell
.\release\jam2.exe list-devices
.\release\jam2.exe test-device 0 --sample-rate 48000
.\release\jam2.exe meter-device 0 --sample-rate 48000 --buffer-size 512 --duration-ms 3000
```

Capture a quick WAV from an input device:

```powershell
.\release\jam2-capture.exe list-devices
.\release\jam2-capture.exe record-input --audio-device 0 --input-channels 1 --sample-rate 44100 --buffer-size 128 --duration-ms 30000 --output captures\riff.wav
.\release\jam2-capture.exe list-loopback-sources
.\release\jam2-capture.exe record-loopback --source default --duration-ms 30000 --output captures\system.wav
.\release\jam2-capture.exe record-input --audio-device 0 --input-channels 1 --trigger on --trigger-threshold-db -45 --pre-roll-ms 250 --trim-leading-silence on --trim-trailing-silence on --summary-json on --output captures\idea.wav
```

Capture output is PCM16 mono WAV. Each capture can also write a `.wav.json` sidecar and print a `capture.summary` JSON line for the GUI.

On macOS, `record-loopback` uses CoreAudio process taps on macOS 14.2 or newer. Older macOS users should route system audio through a virtual device such as BlackHole and capture it with `record-input`.

The GUI Track tab can refresh loopback sources, import captured WAV metadata, play a local WAV, and share the WAV to the authenticated peer over the GUI TCP control plane. Speed is currently simple local playback-rate control; pitch-preserving stretch is left for the Signalsmith dependency pass.

Host A:

```powershell
.\release\jam2.exe listen --audio-device 16 --sample-rate 44100 --audio-buffer-size 128 --frame-size 128 --playback-prefill-frames 1536 --playback-ring-frames 8192 --playback-max-frames 4096 --stats enabled --stats-warmup-ms 3000 --stats-interval-ms 5000 --log-stats logs --metronome on --bpm 120
```

Host B, paste the `jam2://...` URL from Host A:

```powershell
.\release\jam2.exe connect "jam2://v1?endpoint=127.0.0.1:49000&session=55f9e711a1c6b358&key=10eee9ddd63f5f43014378bdfd0ccc8f" --audio-device 5 --sample-rate 44100 --audio-buffer-size 128 --frame-size 128 --playback-prefill-frames 1536 --playback-ring-frames 8192 --playback-max-frames 4096 --stats enabled --stats-warmup-ms 3000 --stats-interval-ms 5000 --log-stats logs --metronome on --bpm 120
```

When `listen` generates the session id/key itself, it also prints a full `connect` command with matching stream/tuning options; replace only the client `--audio-device` value.

Current stable profile:

```text
--sample-rate 44100
--audio-buffer-size 128
--frame-size 128
--playback-prefill-frames 1536
--playback-ring-frames 8192
--playback-max-frames 4096
--stats-warmup-ms 3000
```

Network-efficient profile:

```text
--sample-rate 44100
--audio-buffer-size 64
--frame-size 256
--playback-prefill-frames 1536
--playback-ring-frames 8192
--playback-max-frames 4096
--stats-warmup-ms 3000
```

Latency and stability tuning:

- If audio pops, clicks, or drops out, increase `--playback-prefill-frames` first. Try `2048`.
- If latency is too high and the CSV shows `playback_ring_underruns=0`, reduce `--playback-prefill-frames`. Try `1024`.
- If playback latency grows over time, keep `--playback-max-frames` set so old audio is dropped instead of allowing delay to grow.
- If `playback_ring_overruns` is non-zero, keep `--playback-ring-frames` larger than `--playback-max-frames`; `8192` ring with `4096` max is the current stable choice.
- If `resampler_ratio` is pinned at the drift limit for long runs, inspect `drift_ppm` before changing `--drift-max-correction-ppm`. The current default `500` ppm is enough for the tested Windows/Mac devices.
- If `sequence_lost` or `reordered_lost` increases, the network is dropping or delaying packets beyond the small reorder window. Increase prefill before lowering frame size.
- Lower `--frame-size` and `--audio-buffer-size` only after the current profile is stable. Smaller values reduce latency but are more sensitive to jitter.

Key arguments:

| Argument | What it does | When to tweak |
| --- | --- | --- |
| `--audio-device` | Selects the host audio device by ID from `list-devices`. | Change when switching interface/driver. |
| `--sample-rate` | Sets stream and device sample rate. | Keep both hosts matching; `44100` is the current tested value. |
| `--audio-buffer-size` | Sets hardware callback size in frames. | Lower for less device latency; raise if the driver/callback path is unstable. |
| `--frame-size` | Sets audio frames per UDP packet. | Lower for lower packet interval; raise to reduce packet rate/reorder pressure. |
| `--playback-prefill-frames` | Initial playback cushion before output starts. | Raise for pops/dropouts; lower only when underruns are zero and latency is too high. |
| `--playback-ring-frames` | Playback ring capacity/headroom. | Raise if bursts cause overruns; capacity alone does not add latency unless depth grows. |
| `--playback-max-frames` | Maximum retained playback depth before old frames are dropped. | Keep set to prevent latency creep over time. |
| `--stats-warmup-ms` | Excludes startup from drift/jitter/depth tuning stats. | Raise if startup transients are still skewing CSV results. |
| `--stats enabled\|disabled` | Enables live stats counters, final stats output, and the interactive `stats` command. | Leave disabled for lean live sessions; enable for debugging. |
| `--stats-interval-ms` | Prints periodic stats and writes sparse periodic CSV rows when logging. | Use during manual tuning and adaptive analysis. |
| `--log-stats` | Writes CSV stats to a folder. Requires `--stats enabled`. | Enable for comparing adaptive or manual runs. |
| `--metronome-level` | Sets local click volume from `0` to `1`. | Start lower if the click masks the remote instrument. |
| `--remote-level` | Sets local peer playback volume from `0` to `1`. | Lower the peer locally without changing what you send. |
| `--metronome-mode` | Selects `shared-grid`, `leader-audio`, `symmetric-delay`, or `listener-compensated`. | Use `shared-grid` as the default; compare modes with stats during timing tests. |
| `--sample-time-playout` | Enables or disables sample-time-aware receive scheduling. | Leave on for normal testing so drops and late packets do not permanently shift playback timing. |
| `--playout-delay-frames` | Declares the target sample-time playout delay. Defaults to `--playback-prefill-frames`. | Set explicitly when comparing delay, jitter, or metronome alignment experiments. |
| `--adaptive-playback-cushion` | Enables explicit adaptive playout target changes. | Use for Wi-Fi or burst testing; final stats show every raise/release and padding event. |
| `--adaptive-playback-target-frames` | Sets the starting adaptive playout target. | Keep near the normal stable prefill for a run. |
| `--adaptive-playback-min-frames` / `--adaptive-playback-max-frames` | Bounds adaptive target movement. | Use numeric bounds to keep added latency controlled. |
| `--adaptive-playback-release-ppm` | Controls how slowly the adaptive target releases extra cushion. | Lower values hold added latency longer; higher values release faster. |
| `--drift-deadband-ppm` | Keeps playback at exact 1.0 ratio while smoothed drift is within this ppm. | Raise if tiny corrections sound rough; lower if long-run drift needs earlier correction. |
| `--drift-max-correction-ppm` | Caps playback resampler correction. | Change only if `resampler_ratio` is pinned and `drift_ppm` is believable. |
| `--input-channels` | Selects one or more input channels mixed to mono. | Use `1,2,3,4` for multi-input devices; invalid device channels error out. |
| `--output-channels` | Selects one or more output channels receiving duplicated mono. | Use when headphones/monitor outputs are not 1/2. |
| `--socket-send-buffer` / `--socket-recv-buffer` | Requests OS UDP socket buffer sizes. | Use when diagnosing packet drops or OS buffering differences. |
| `--stream-ms` | Ends the stream after a fixed duration. | Use for repeatable adaptive or manual tests. |
| `--wait-ms` | Limits how long handshake waits. | Leave unset for manual sessions; set for automation. |

Runtime commands:

```text
stats
status
bpm 140
metro off
metro on
metro mode shared-grid
metro level 0.15
metro level +0.05
remote level 0.75
remote mute
remote unmute
quit
```

Automated adaptive tuning:

The Python tuner starts with aggressive settings and backs off until it finds a mostly stable profile for the exact two-host setup. It raises frame size before audio buffer size, skips adaptive candidates where `frame_size < audio_buffer_size`, and does not test 32-frame packetization or 512-frame audio buffers. The largest packetization mode remains 256 frames to keep audio UDP packets below typical MTU.

The adaptive search is staged to avoid spending repeated runs on clearly bad settings:

- Probe: one 30 second run per candidate. Obvious failures such as excessive packet loss rate, excessive playback underrun rate, missing client upload, missing final stats, or playback depth below 2.5 ms are rejected immediately. Tiny playback underrun, packet-loss, and dropped-frame rates are kept as warnings so audibly clean low-latency paths can continue.
- Prefill jump: if a probe, edge check, or confirmation run fails only because playback depth is too low, the tuner estimates the missing frames from the CSV depth minimum and sample rate, then queues the next matching prefill value instead of waiting for the normal ladder to reach it.
- Edge check: promising candidates get two more 30 second runs. A candidate can pass with 2/3 good runs if aggregate packet-loss, underrun, dropped-frame, overrun, and depth metrics are within the mostly-stable limits.
- Drift check: default adaptive runs keep `--drift-correction on --drift-deadband-ppm 25` and report measured drift. Add `--drift-probes` to spend extra time testing one measured wider deadband after a physical profile is already stable.
- Confirmation: the best short-run result gets three 60 second runs. A rare non-catastrophic burst does not invalidate the profile if the aggregate rates remain inside the mostly-stable limits.
- Aggressive recommendation: the tuner also prints a lowest-latency mostly-clean profile. This allows more playback underruns, tiny packet loss, dropped-frame rates, and depth dips that may be inaudible. It still rejects incomplete runs, playback overruns, severe jitter, and severe depth collapse.

Always pass `--network-profile` on both scripts so the logs and acceptance thresholds match the actual connection. Use `wired` for Ethernet, `wifi` for Wi-Fi, and `unknown` if you cannot tell. If either side is on Wi-Fi, the combined run is treated as Wi-Fi. The scripts can still auto-detect with `--network-profile auto`, but explicit values are preferred for repeatable test data.

Start the listen/server side first:

```powershell
python tools/run_matrix_server.py --server-audio-device 16 --sample-rate 44100 --host 0.0.0.0 --port 8000 --network-profile wired --clean
```

Then start the connect/client side on the other host:

```bash
python tools/run_matrix_client.py --server http://WINDOWS_IP:8000 --client-audio-device 0 --sample-rate 44100 --network-profile wifi --clean
```

Targeted benchmark mode:

Add `--benchmark` on the server side to run a bounded connection benchmark instead of stopping at the first confirmed adaptive recommendation:

```powershell
python tools/run_matrix_server.py --benchmark --server-audio-device 16 --sample-rate 44100 --host 0.0.0.0 --port 8000 --network-profile wired --clean
```

Use the same client command as the adaptive tuner. Benchmark mode runs each profile for `3 x 30s`, starts with this baseline spine, then queues a small number of targeted neighbor profiles based on the measured failure mode:

```text
32/64/768
64/64/768
32/128/1024
64/128/1024
128/128/1024
128/128/1536
128/256/2048
128/256/4096
```

The fields are `audio_buffer_size/frame_size/playback_prefill_frames`. Capture underruns push the benchmark toward larger audio buffers, packet loss or jitter pushes toward larger frame sizes, and playback underruns, drops, overruns, or low depth push toward larger prefill. Clean profiles queue lower-latency boundary checks. The benchmark writes:

If one run is an isolated large burst compared with the other runs for that profile, benchmark mode excludes that run from the aggregate and runs a replacement sample. The original run logs remain on disk, and the benchmark CSV records `benchmark_attempt_count`, `benchmark_clean_runs`, and `benchmark_discarded_burst_runs`. The benchmark exits with failure if any baseline spine profile cannot collect three non-burst runs within the retry limit.

```text
tools/logs/benchmark.csv
tools/logs/benchmark.json
tools/logs/benchmark_summary.txt
```

When burst runs are discarded, `combined_stats.csv` remains the raw record and benchmark mode also writes `benchmark_clean_combined_stats.csv` for analysis without the discarded burst rows.

After each test, the client uploads `stats.csv`, `stdout.txt`, and `stderr.txt` back to the server. When the tuner finishes it writes combined results, analysis, a stable recommendation, and an aggressive low-latency recommendation:

```text
tools/logs/combined_stats.csv
tools/logs/analysis.csv
tools/logs/analysis.json
tools/logs/recommendation.txt
```

The server publishes the current `jam2://` URL as raw JSON at:

```text
http://WINDOWS_IP:8000/current.json
```

Logs are written under:

```text
tools/logs/<candidate-id>/server/run_NN/
tools/logs/<candidate-id>/client/run_NN/
```

Each run directory contains `stdout.txt`, `stderr.txt`, and `stats.csv` when the app generated a CSV.

If needed, combine CSVs manually:

```powershell
python tools/collect_matrix_csv.py --side all
```

Or collect one side only:

```powershell
python tools/collect_matrix_csv.py --side server
python tools/collect_matrix_csv.py --side client
```

The combined CSV is written to `tools/logs/combined_stats.csv`, or `combined_stats_server.csv` / `combined_stats_client.csv` for one side.

Analyze adaptive or manual logs:

```powershell
python tools/analyze_matrix_csv.py --input tools/logs/combined_stats.csv
```

Or point the analyzer at a log directory directly:

```powershell
python tools/analyze_matrix_csv.py --logs tools/logs
```

This writes:

```text
tools/logs/analysis.csv
```

The analyzer ranks profiles by stability first, then latency. By default, any sequence loss, reordered loss, playback underrun, playback overrun, or dropped playback frame marks a profile unstable. The adaptive server invokes it with confirmation rules requiring at least three runs, at least 2.5 ms playback depth, and only tiny playback underrun rates. Depth below 5 ms is still reported as a warning in the adaptive output because it means the ring came close to empty. To apply the adaptive stable rules manually:

Newer CSVs include underrun severity fields in addition to raw underrun counts: `playback_ring_underrun_time_percent`, `playback_ring_underrun_burst_max_ms`, `playback_ring_underrun_event_max_ms`, `playback_dropped_time_ms`, and playback-depth bucket percentages under 2 ms, 5 ms, and 10 ms. The analyzer uses these severity fields when present so tiny, scattered underruns are not treated the same as long audible gaps.

The analyzer's stdout label is `CSV-ranked profile`. The adaptive tuner prints separate `stable recommendation` and `aggressive low-latency recommendation` lines and writes both to `tools/logs/recommendation.txt`; use those as the automation result. Each recommendation includes a full server `listen` command and a client `connect "<paste jam2://...>"` command using the detected client audio device from uploaded logs.

```powershell
python tools/analyze_matrix_csv.py --input tools/logs/combined_stats.csv --min-runs 3 --min-playback-depth-ms 2.5 --max-loss-percent 0.05 --max-loss-per-second 0.10 --max-playback-underruns-per-second 3 --max-playback-underrun-events-per-second 0.10 --max-playback-dropped-frames-per-second 8
```

To tolerate small glitches during experiments:

```powershell
python tools/analyze_matrix_csv.py --max-loss 2 --max-playback-underruns 256
```
