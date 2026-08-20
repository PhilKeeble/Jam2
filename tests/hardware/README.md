# Jam2 hardware test profile

Real audio-device and VST3 testing is deliberately opt-in. The normal suites,
including `--tests-full` without a profile, use fake audio and never open a
physical device.

Create a machine-local JSON file outside the tracked source tree:

```json
{
  "schema": "jam2-hardware-profile-v1",
  "plugin_path": "C:/Program Files/Common Files/VST3/Example.vst3",
  "audio_device_id": 5,
  "input_channel": 2,
  "buffer_frames": 32
}
```

On Windows, run only the hardware extension with:

```text
compile.cmd --tests hardware --hardware-profile build/hardware-profile.json
```

Supplying the same option to `--tests-full` includes the hardware extension in
the normal Release test pass. Supplying it to the separate Windows `--coverage`
command also makes the profiled hardware and plugin coverage mandatory. A
missing profile, plugin, or device is a failure rather than an automatic pass.
Device IDs are the numeric IDs printed by Jam2's audio device listing command;
input channels are one-based.

The macOS profile uses the same schema, with a `.vst3` bundle path and the
CoreAudio device ID required by the Apple implementation. Complete and verify
that platform path by following `TEST-MACOS.md`.

The complete macOS CoreAudio/CoreMIDI profile uses schema v2:

```json
{
  "schema": "jam2-hardware-profile-v2",
  "plugin_path": "/Library/Audio/Plug-Ins/VST3/Gateway.vst3",
  "instrument_plugin_path": "/Library/Audio/Plug-Ins/VST3/Surge XT.vst3",
  "audio_device_id": 0,
  "input_channel": 1,
  "second_input_channel": 2,
  "buffer_frames": 64,
  "unsupported_buffer_frames": 1,
  "midi_device_name": "Xjam"
}
```

Schema v2 retains schema v1 for existing Windows profiles and adds an explicit
second real input, a device-profiled unsupported buffer, a VST3 instrument, and
a CoreMIDI input selector. The hardware MIDI case is intentionally interactive:
it prints `MIDI_CAPTURE_READY` before requiring note-on, note-off, and continuous
control messages. It then prints `MIDI_MUTED_READY` to prove that another real
message is consumed while the routed output remains exactly muted, followed by
`MIDI_INSTRUMENT_READY` before requiring the real controller -> isolated
instrument -> audio-device callback path to produce wet signal. Each phase
exits as soon as those product signals are observed; its 60-second limits are
hardware-interaction deadmen, not performance thresholds. The audio-effect case
likewise exits after live input is measured inside the isolated worker,
completed effect processing, bypassed delayed-dry signal, and recovered effect
processing are observed. The effect's wet peak remains raw diagnostic data; it
is not required to be nonzero because a valid utility plug-in may intentionally
start silent. Run these cases only while an operator is ready to provide
continuous audio and use the profiled controller.
