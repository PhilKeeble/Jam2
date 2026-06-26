# Jam2 Quick Run

Build:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Check devices on each host:

```powershell
.\build\jam2.exe --list-devices
.\build\jam2.exe probe-device 0 --sample-rate 48000
.\build\jam2.exe meter-device 0 --sample-rate 48000 --buffer-size 512 --duration-ms 3000
```

Host A:

```powershell
.\build\jam2.exe listen --audio-device 0 --audio-buffer-size 512 --sample-rate 48000 --frame-size 128 --playback-prefill-frames 512 --metronome on --bpm 120
```

Host B, paste the `jam2://...` URL from Host A:

```powershell
.\build\jam2.exe connect "jam2://..." --audio-device 0 --audio-buffer-size 512 --sample-rate 48000 --frame-size 128 --playback-prefill-frames 512 --metronome on --bpm 120
```

Runtime commands:

```text
stats
bpm 140
metro off
metro on
quit
```
