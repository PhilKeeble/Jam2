# Jam2 Engine

`jam2` is one unified GUI and command-line application. With no arguments it opens the GUI; its subcommands use the same engine and network implementation for headless operation, diagnostics, and automated testing.

## Commands

```text
jam2 list-devices
jam2 test-device <id> [--sample-rate n]
jam2 meter-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n]
jam2 local [audio options]
jam2 network create [--profile fast|moderate|safe] [options]
jam2 network join <jam2-url> [--profile fast|moderate|safe] [options]
jam2 debug describe --json
jam2 debug run <scenario.json>
```

Use `list-devices` first on each host, then use the local device id in the GUI or CLI.

## CLI Help

Use `-h` or `--help` for the command menu, then apply it to a command or network/debug subcommand for its accepted options and ranges:

```powershell
.\release\jam2.exe -h
.\release\jam2.exe local -h
.\release\jam2.exe network -h
.\release\jam2.exe network create -h
.\release\jam2.exe network join -h
.\release\jam2.exe debug run -h
```

`jam2 help` is also accepted for the root menu. Help exits without opening an audio device, socket, STUN request, or scenario file.

## Basic Create And Join

Host:

```powershell
.\release\jam2.exe network create --bind 0.0.0.0:49000 --profile fast --audio-device 0 --input-channels 1 --output-channels 1,2
```

Client:

```powershell
.\release\jam2.exe network join "jam2://..." --profile fast --audio-device 0 --input-channels 1 --output-channels 1,2
```

The creator's TCP coordinator authenticates the joiner, checks the immutable sample-rate/frame-size contract, and distributes direct UDP candidates and current membership. Device ids and channel selections remain local to each machine.

## Important Options

| Option | Purpose |
| --- | --- |
| `--profile fast\|moderate\|safe` | Applies a named set of numeric tuning values. Explicit numeric flags override the selected profile. |
| `--audio-device` | Selects the host audio device id. |
| `--sample-rate` | Sets device and stream sample rate. Keep both peers matching. |
| `--audio-buffer-size` | Sets the host audio callback size in frames. |
| `--headless-clock-drift-ppm` | Test-only synthetic device clock offset (`-5000..5000` ppm); requires headless audio. |
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

## Runtime Control

Normal product interaction does not depend on an interactive stdin protocol.
Configure initial headless audio/network state with command-line options and
use `--stream-ms` or normal process signals for bounded shutdown. The GUI
submits typed commands directly to its in-process engine. The local stress
harness currently uses a small transitional headless line adapter for Phase 8
regressions; Phase 10 replaces it with the bounded native automation contract.

## Stats Output

Use stats while testing and tuning. The engine reports raw measurements such as packet loss, jitter, RTT, bitrate, playback depth, underruns, overruns, drift ppm, resampler ratio, jitter-buffer drops, adaptive cushion movement, requested/applied playback drops, receive loop gaps, audio callback gaps, and prepared-source frame/underrun/busy counters. Transport diagnostics retain the authenticated source peer, event counter, grid revision, typed action, source frame, requested target, and locally applied target.

Use CSV logs for comparisons between runs:

```powershell
.\release\jam2.exe network create --stats enabled --stats-interval-ms 1000 --log-stats logs
```

See [Diagnosing](Diagnosing.md) and [Profiles](Profiles.md) for practical tuning guidance.
