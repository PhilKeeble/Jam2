# Jam2 Engine

`jam2` is one unified GUI and command-line application. With no arguments it opens the GUI; its subcommands use the same engine and network implementation for headless operation, diagnostics, and automated testing.

On Windows, opening `jam2.exe` starts the GUI without creating a console window.
CLI commands attach to the terminal that launched them and preserve redirected
input, output, and error streams. They do not create a new console when launched
by an automation tool or another process without a terminal.

Windows treats this executable as a GUI application, so interactive shells may
return their prompt before a CLI command finishes. In interactive Command Prompt,
use `start "" /wait jam2.exe <command>`. In PowerShell, use
`Start-Process -FilePath .\release\jam2.exe -ArgumentList '--help' -NoNewWindow -Wait -PassThru`
when explicit waiting or the process exit code is needed (`.ExitCode` on the
returned process). Batch files and tools that launch and wait on a process handle
can continue to use the same executable. Bounded headless use remains available,
for example `jam2.exe local --headless-audio on --stream-ms 1000`.

macOS keeps its existing `Jam2.app` GUI launch and command-line behavior.

## Commands

```text
jam2 list-devices
jam2 test-device <id>
jam2 local [audio options]
jam2 network create --name Alice [--profile fast|moderate|safe] [options]
jam2 network join <jam2-url> --name Alex [--profile fast|moderate|safe] [options]
jam2 debug describe --json
jam2 debug run <scenario.json>
```

Use `list-devices` first on each host, then `test-device <id>` to report the
device's current sample rate and support for 44100/48000 Hz and
32/64/128/256-frame buffers. The GUI's **Test Device** buttons call the same
core operation.

## Windows WASAPI

`list-devices` lists ASIO drivers first, then WASAPI output and input endpoints,
including their channel counts and opaque stable IDs. Select a WASAPI **output**
with `--audio-device <id>`. `--wasapi-input <input-id-or-endpoint-id>` selects a
separate capture endpoint; omitting it or using `none` starts playback only.
WASAPI opens all capture channels and routes mono to front stereo/mono output.
Exclusive mode is not used.

```text
jam2 local --audio-device <wasapi-output-id> --wasapi-input none --audio-buffer-size 0 --stream-ms 3000 --stats enabled
jam2 network join <jam2-url> --name Alex --audio-device <wasapi-output-id> --wasapi-input <wasapi-input-id>
```

WASAPI always requests the automatic minimum device period; use diagnostics for
the actual period. `--audio-buffer-size` only limits its internal processing block
(0 uses 128 frames), without changing the device period. Windows shared-mode
sample-rate conversion keeps the engine/network rate unchanged when an endpoint
uses another mix rate. The output clock paces the engine. Independent input-clock
correction uses a bounded preallocated queue with numeric controls:

- `--wasapi-capture-target-ms` (default 20, range 1..500).
- `--wasapi-capture-capacity-ms` (default 200, at least twice the target, maximum 2000).
- `--wasapi-capture-max-ppm` (default 2000, range 0..5000; 0 disables correction).

Queue depth, correction ppm, capture underruns/overruns, discontinuities, invalid
timestamps and render padding appear in structured stats and CSV. `local --stats
enabled` prints the final device/queue measurements. Capture timestamps and
render padding contribute to recording-alignment estimates; Windows' reported
stream maximum is retained separately. Use physical calibration for actual
round-trip latency. Endpoint loss stops the stream with an error; Jam2 does not
silently select another microphone or output.

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
.\release\jam2.exe network create --name Alice --bind 0.0.0.0:40440 --profile fast --audio-device 0 --input-channels 1 --output-channels 1,2
```

Client:

```powershell
.\release\jam2.exe network join "jam2://..." --name Alex --profile fast --audio-device 0 --input-channels 1 --output-channels 1,2
```

The creator's multiplex control coordinator authenticates the joiner, supplies the immutable
sample-rate/frame-size contract, and distributes direct UDP candidates and
current membership. Device ids, channel selections, callback buffer size, and
receive/playout tuning remain local to each machine.

## Important Options

| Option | Purpose |
| --- | --- |
| `--name <letters>` | Required create/join name: 1 to 8 Unicode letters after NFC normalization; duplicate names are allowed. |
| `--profile fast\|moderate\|safe` | Applies a named set of numeric tuning values. Explicit numeric flags override the selected profile. |
| `--audio-device` | Selects the host audio device id. |
| `--sample-rate` | Creator-owned session sample rate. A joiner receives it from the session contract. |
| `--audio-buffer-size` | Sets the host audio callback size in frames. |
| `--headless-clock-drift-ppm` | Test-only synthetic device clock offset (`-5000..5000` ppm); requires headless audio. |
| `--frame-size` | Sets audio frames per UDP packet. |
| `--input-channels` | Selects one or more input channels mixed to mono. Multiple channels use independent adaptive peak-noise floors (+14 dB open / +8 dB close hysteresis) and sample-ramped activity weights with a 4 ms attack and 45 ms release. The normalized weighted average preserves one sounding channel at unity gain without block-boundary switching or multi-channel clipping. |
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
| `--stats` | Enables diagnostic collection and final/on-demand human output. |
| `--stats-interval-ms` | Sets structured CSV sampling; it does not print recurring human stats. |
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
.\release\jam2.exe network create --name Alice --stats enabled --stats-interval-ms 1000 --log-stats logs
```

See [Diagnosing](Diagnosing.md) and [Profiles](Profiles.md) for practical tuning guidance.
