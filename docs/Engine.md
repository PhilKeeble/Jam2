# Jam2 Engine

`jam2` is the command-line audio engine used by both the GUI and headless testing. It handles device access, UDP audio, STUN endpoint discovery, metronome timing, drift correction, stats, and CSV logging.

## Commands

```text
jam2 list-devices
jam2 test-device <id> [--sample-rate n]
jam2 meter-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n]
jam2 listen [--profile fast|moderate|safe] [options]
jam2 connect <jam2-url> [--profile fast|moderate|safe] [options]
```

Use `list-devices` first on each host, then use the local device id in the GUI or CLI.

## Basic Listen And Connect

Host:

```powershell
.\release\jam2.exe listen --bind 0.0.0.0:49000 --profile fast --audio-device 0 --input-channels 1 --output-channels 1,2
```

Client:

```powershell
.\release\jam2.exe connect "jam2://..." --profile fast --audio-device 0 --input-channels 1 --output-channels 1,2
```

Both peers must match sample rate and frame size. Device ids and channel selections are local to each machine.

## Important Options

| Option | Purpose |
| --- | --- |
| `--profile fast\|moderate\|safe` | Applies a named set of numeric tuning values. Explicit numeric flags override the selected profile. |
| `--audio-device` | Selects the host audio device id. |
| `--sample-rate` | Sets device and stream sample rate. Keep both peers matching. |
| `--audio-buffer-size` | Sets the host audio callback size in frames. |
| `--frame-size` | Sets audio frames per UDP packet. |
| `--input-channels` | Selects one or more input channels mixed to mono. |
| `--output-channels` | Selects one or more output channels for duplicated mono playback. |
| `--playback-prefill-frames` | Initial playback cushion before audio output begins. |
| `--playback-ring-frames` | Playback ring capacity. |
| `--playback-max-frames` | Maximum retained playback depth before old audio is dropped. |
| `--sample-time-playout` | Enables sample-time-aware receive scheduling. |
| `--playout-delay-frames` | Target sample-time playout delay. |
| `--jitter-buffer-frames` | Adds explicit jitter-buffer delay before playback release. |
| `--jitter-buffer-max-frames` | Caps jitter-buffer span. |
| `--adaptive-playback-cushion` | Allows explicit adaptive playout target changes. |
| `--drift-correction` | Enables or disables playback clock drift correction. |
| `--drift-smoothing` | Smooths measured drift. |
| `--drift-deadband-ppm` | Avoids tiny correction changes inside a ppm deadband. |
| `--drift-max-correction-ppm` | Caps resampler correction. |
| `--socket-send-buffer` / `--socket-recv-buffer` | Requests OS UDP socket buffer sizes. |
| `--stats` | Enables live and final stats. |
| `--stats-interval-ms` | Prints periodic stats. |
| `--log-stats` | Writes stats CSV files to a folder. |
| `--record-jam-folder` | Records local jam stems for later inspection. |

## Runtime Commands

While `jam2 listen` or `jam2 connect` is running, stdin accepts:

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

## Stats Output

Use stats while testing and tuning. The engine reports raw measurements such as packet loss, jitter, RTT, bitrate, playback depth, underruns, overruns, drift ppm, resampler ratio, jitter-buffer drops, adaptive cushion movement, receive loop gaps, and audio callback gaps.

Use CSV logs for comparisons between runs:

```powershell
.\release\jam2.exe listen --stats enabled --stats-interval-ms 1000 --log-stats logs
```

See [Diagnosing](Diagnosing.md) and [Profiles](Profiles.md) for practical tuning guidance.
