# Capture

`jam2-capture` records mono PCM16 WAV files. It is separate from the live UDP audio protocol and is used for collecting input takes, loopback/system audio, and shared track material.

## Commands

```text
jam2-capture list-devices
jam2-capture list-loopback-sources
jam2-capture record-input --audio-device 5 --input-channels 1 --sample-rate 44100 --buffer-size 128 --duration-ms 30000 --output riff.wav
jam2-capture record-loopback --source default --duration-ms 30000 --output system.wav
```

Input capture uses the Jam2 host audio backend and mixes selected input channels to mono. Loopback capture uses WASAPI loopback on Windows or CoreAudio process taps on macOS 14.2 or newer.

On older macOS versions, route system audio through a virtual device such as BlackHole or Loopback and capture it with `record-input`.

## Trigger And Trim

Useful recording options:

```text
--trigger on|off
--trigger-threshold-db -45
--trigger-hold-ms 50
--pre-roll-ms 250
--tail-silence-db -55
--tail-silence-ms 500
--trim-leading-silence on|off
--trim-trailing-silence on|off
--summary-json on|off
```

Example:

```powershell
.\release\jam2-capture.exe record-input --audio-device 0 --input-channels 1 --trigger on --trigger-threshold-db -45 --pre-roll-ms 250 --trim-leading-silence on --trim-trailing-silence on --summary-json on --output captures\idea.wav
```

When `--summary-json on` is used, capture can write a `.wav.json` sidecar and print a `capture.summary` JSON line for tools or the GUI.

## GUI Use

The GUI can launch capture, import the resulting WAV metadata, and load the file into the Track tab. Track sharing is handled by the GUI TCP control plane, not by `jam2-capture`.
