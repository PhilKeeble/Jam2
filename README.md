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
.\build\jam2.exe listen --audio-device 16 --audio-buffer-size 64 --sample-rate 44100 --frame-size 64 --playback-prefill-frames 64 --metronome on --bpm 120 --no-stun
```

Host B, paste the `jam2://...` URL from Host A:

```powershell
.\build\jam2.exe connect "jam2://v1?endpoint=127.0.0.1:49000&session=55f9e711a1c6b358&key=10eee9ddd63f5f43014378bdfd0ccc8f" --audio-device 5 --audio-buffer-size 64 --sample-rate 44100 --frame-size 64 --playback-prefill-frames 64 --metronome on --bpm 120
```

Runtime commands:

```text
stats
bpm 140
metro off
metro on
quit
```
