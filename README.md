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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`jam2-gui` and `jam2-capture` are enabled by default for new CMake build directories and are written to `release` beside the `jam2` binary. If an existing build directory was configured before those targets were added, reconfigure with `-DJAM2_BUILD_GUI=ON -DJAM2_BUILD_CAPTURE=ON` or use a fresh build directory.

The GUI needs Qt 6 Widgets, Network, and Multimedia development packages. If CMake cannot find Qt automatically, pass its install prefix:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJAM2_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64
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

Static two-host benchmark:

The Python benchmark runner uses fixed profile/signal cases with no dynamic case generation. Each case records dry stems, uploads server/client artifacts to the benchmark server, analyzes the WAVs and stats, and writes compact pass/warn/fail outputs. Benchmark logs default to `tools/benchmark_logs`; local stress logs default to `tools/stress_logs`.

Start the listen/server side first:

```powershell
python tools/run_benchmark_server.py --server-audio-device 16 --sample-rate 44100 --bind-http 0.0.0.0:8000 --clean
```

Then start the connect/client side on the other host:

```bash
python tools/run_benchmark_client.py --server http://WINDOWS_IP:8000 --client-audio-device 0 --sample-rate 44100 --network-profile wifi --clean
```

List benchmark cases without launching Jam2:

```powershell
python tools/run_benchmark_server.py --server-audio-device 16 --sample-rate 44100 --list-cases
```

Run a short tone benchmark pass. This includes symmetric tone cases plus targeted server-to-client and client-to-server tone cases for the baseline profiles:

```powershell
python tools/run_benchmark_server.py --server-audio-device 16 --sample-rate 44100 --signals tone-440 --no-metronome-cases --stream-ms 10000 --clean
```

The benchmark writes:

```text
tools/benchmark_logs/benchmark_summary.txt
tools/benchmark_logs/benchmark_results.csv
tools/benchmark_logs/benchmark_results.json
```

Each case folder contains server/client `stats.csv`, `stdout.txt`, `stderr.txt`, `recording/*.wav`, `recording/recording.json`, and `analysis.json`.

Tone coverage includes symmetric `tone-440`, `tone-server-to-client`, and `tone-client-to-server` cases without applying every direction to every profile.

Local stress tests can also add targeted recorded tone/pulse probes:

```powershell
python tools/run_stress_local.py --server-audio-device 16 --client-audio-device 0 --sample-rate 44100 --include-audio-probes --clean
```

Those probes write WAV stems and `audio_probe_analysis` data under `tools/stress_logs` for clean, jitter, directional-loss, and adaptive on/off pressure cases.

The server publishes the current `jam2://` URL as raw JSON at:

```text
http://WINDOWS_IP:8000/current.json
```

Re-print an existing benchmark JSON as a compact table:

```powershell
python tools/analyze_benchmark_results.py tools/benchmark_logs/benchmark_results.json
```
