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
both Windows passes. A missing profile, plugin, or device is a failure rather
than an automatic pass. Device IDs are the numeric IDs printed by Jam2's audio
device listing command; input channels are one-based.

The macOS profile uses the same schema, with a `.vst3` bundle path and the
CoreAudio device ID required by the Apple implementation. Complete and verify
that platform path by following `TEST-MACOS.md`.
