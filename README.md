# Jam2 Quick Run

Build:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Check devices on each host:

```powershell
.\build\jam2.exe list-devices
.\build\jam2.exe test-device 0 --sample-rate 48000
.\build\jam2.exe meter-device 0 --sample-rate 48000 --buffer-size 512 --duration-ms 3000
```

Host A:

```powershell
.\build\jam2.exe listen --audio-device 16 --sample-rate 44100 --audio-buffer-size 128 --frame-size 128 --playback-prefill-frames 1536 --playback-ring-frames 8192 --playback-max-frames 4096 --stats-warmup-ms 3000 --stats-interval-ms 5000 --log-stats logs --metronome on --bpm 120
```

Host B, paste the `jam2://...` URL from Host A:

```powershell
.\build\jam2.exe connect "jam2://v1?endpoint=127.0.0.1:49000&session=55f9e711a1c6b358&key=10eee9ddd63f5f43014378bdfd0ccc8f" --audio-device 5 --sample-rate 44100 --audio-buffer-size 128 --frame-size 128 --playback-prefill-frames 1536 --playback-ring-frames 8192 --playback-max-frames 4096 --stats-warmup-ms 3000 --stats-interval-ms 5000 --log-stats logs --metronome on --bpm 120
```

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

Latency and stability tuning:

- If audio pops, clicks, or drops out, increase `--playback-prefill-frames` first. Try `2048`.
- If latency is too high and the CSV shows `playback_ring_underruns=0`, reduce `--playback-prefill-frames`. Try `1024`.
- If playback latency grows over time, keep `--playback-max-frames` set so old audio is dropped instead of allowing delay to grow.
- If `playback_ring_overruns` is non-zero, keep `--playback-ring-frames` larger than `--playback-max-frames`; `8192` ring with `4096` max is the current stable choice.
- If `resampler_ratio` is pinned at the drift limit for long runs, inspect `drift_ppm` before changing `--drift-max-correction-ppm`. The current default `500` ppm is enough for the tested Windows/Mac devices.
- If `sequence_lost` or `reordered_lost` increases, the network is dropping or delaying packets beyond the small reorder window. Increase prefill before lowering frame size.
- Lower `--frame-size` and `--audio-buffer-size` only after the current profile is stable. Smaller values reduce latency but are more sensitive to jitter.

Runtime commands:

```text
stats
bpm 140
metro off
metro on
quit
```

Automated matrix testing:

Edit test cases in:

```text
tools/test_matrix.json
```

The default matrix covers aggressive through safe profiles from `64/64` up to `512/512`. Each test runs for `120000ms` by default; increase `--runs` to measure variance across repeated runs.

Start the listen/server side first:

```powershell
python tools/run_matrix_server.py --server-audio-device 16 --host 0.0.0.0 --port 8000 --runs 3
```

Then start the connect/client side on the other host:

```bash
python tools/run_matrix_client.py --server http://WINDOWS_IP:8000 --client-audio-device 0
```

After each test, the client uploads its `stats.csv` back to the server. When the server finishes the matrix it writes combined results and analysis automatically:

```text
tools/logs/combined_stats.csv
tools/logs/analysis.csv
```

The server publishes the current `jam2://` URL as raw JSON at:

```text
http://WINDOWS_IP:8000/current.json
```

Logs are written under:

```text
tools/logs/<test-id>/server/run_NN/
tools/logs/<test-id>/client/run_NN/
```

Each run directory contains `stdout.txt`, `stderr.txt`, and `stats.csv` when the app generated a CSV.

If needed, combine matrix CSVs manually:

```powershell
python tools/collect_matrix_csv.py --side all
```

Or collect one side only:

```powershell
python tools/collect_matrix_csv.py --side server
python tools/collect_matrix_csv.py --side client
```

The combined CSV is written to `tools/logs/combined_stats.csv`, or `combined_stats_server.csv` / `combined_stats_client.csv` for one side.

Rank the matrix profiles:

```powershell
python tools/analyze_matrix_csv.py --input tools/logs/combined_stats.csv
```

This writes:

```text
tools/logs/analysis.csv
```

The analyzer ranks profiles by stability first, then latency. By default, any sequence loss, reordered loss, playback underrun, playback overrun, or dropped playback frame marks a profile unstable. To tolerate small glitches during experiments:

```powershell
python tools/analyze_matrix_csv.py --max-loss 2 --max-playback-underruns 256
```
