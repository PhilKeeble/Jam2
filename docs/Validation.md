# Automated Validation

Jam2 product validation is implemented in C++ and run by CTest. The native
catalogue owns application behavior, GUI controls, exactly-four-peer direct
jams, fake audio, synchronization, WAV transfer and persistence, network
security, metronome/epoch timing, plugins, and JamTaster behavior. Python is
not a second product-validation authority.

## Windows commands

A normal distribution build remains test-free:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell"
```

Run only the CTests directly relevant to a change while developing. Every
feature, behavior change and bug fix must add or update relevant native C++
tests registered with CTest. All builds and tests use optimized Release.

Select the relevant suite:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests unit"
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests gui"
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests performance"
```

Run one registered CTest exactly:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests gui --test-name jam2_four_gui_agent_integration"
```

Run the full suite only when the user explicitly requests it, including before
distribution:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests-full"
```

`--tests-full` builds the optimized `/O2` Release targets, stages the one public
`release/jam2.exe`, and runs the complete hardware-independent CTest catalogue
once. No source-coverage collector or alternate build configuration is used.

A full-suite run is a report, not authorization to fix failures. For each
failure, report the expected behavior, observed result, measurement method,
and assertion or threshold. Distinguish confirmed causes from hypotheses and
whether the evidence suggests a test issue, product issue, or environmental
limitation. The user decides whether the test or product code needs changing.

## macOS commands

The equivalent command shapes are:

```bash
bash ./compile.sh
bash ./compile.sh --tests unit
bash ./compile.sh --tests gui --test-name jam2_four_gui_agent_integration
bash ./compile.sh --tests-full
```

On macOS, builds and tests also use optimized Release. Run `--tests-full` only
when the user explicitly requests the full CTest catalogue. The same reporting
rule applies; no source coverage is collected.

Windows completion does not imply CoreAudio/Cocoa completion. The exact later
Apple parity work is listed in `TEST-MACOS.md`.

## Suites

The selected suite names are:

- `unit`: bounded model, parser, lifecycle, numeric, persistence, and service
  contracts;
- `plugin`: deterministic isolated-plugin and input-backend contracts;
- `gui`: widget, modal, application, and exactly-four-peer GUI workflows;
- `jam-sync`: policy/revision behavior and four-peer reconciliation;
- `shared-content`: project, WAV, asset-transfer, interruption, and exact-byte
  convergence;
- `performance`: fake-audio transport, recording, prepared playback, and the
  metronome/epoch impairment matrix;
- `network`: typed-control/security boundaries and authenticated UDP adversarial cases;
  and
- `full`: every default hardware-independent CTest.

GUI tests run offscreen unless `--show-gui` is supplied to `gui` or the full
gate. Normal and selected test commands build only the requested target set.

## Results and temporary files

All automated runtime state is rooted under `build/test-artifacts`. Selected
and full test commands remove that exact directory before starting and again
after complete success. Failures retain their test artifacts there for diagnosis.

The maintained implementation/review history is in `TEST-PLAN.md`,
`TEST-LOG.md`, and `TEST-REVIEW.md`. Cross-machine and long soak campaigns are
manual product testing and are intentionally not part of this automated gate.

## Retained Python tooling

`tools/jam2_test.py` remains for two-machine benchmarking, offline result
analysis, connectivity diagnostics, and bounded fuzz orchestration. Its former
`validate` and `stress` command families were retired after their product
contracts moved into C++/CTest. Python unit tests continue to validate the
retained Python tools themselves; they do not validate Jam2 product behavior.

## Windows WASAPI checks

Run `compile.cmd --in-dev-shell --tests wasapi` from the repository root in the
normal elevated build environment. This builds optimized Release and runs only
the WASAPI-labelled native CTests: conversion/clock/duplex units, real startup
and Settings dialog actions/persistence, and an exactly-four-peer audio-flow
integration with different device periods, independent clocks, loss, jitter,
reordering and duplication. Existing audio/CLI/calibration tests remain relevant
when changing the shared processing path.

Hardware validation is opt-in. Set `JAM2_WASAPI_HARDWARE_PROFILE` to the absolute
path of a local JSON profile before configuring/building:

```json
{
  "schema": "jam2-wasapi-hardware-v1",
  "output_endpoint_id": "<opaque WASAPI output ID from list-devices>",
  "input_endpoint_id": "<optional opaque WASAPI input ID>",
  "asio_clsid": "<optional ASIO driver CLSID>"
}
```

The hardware CTest uses `release/jam2.exe` for 48/44.1 kHz output, a 15-second
split-device capture run, and optional ASIO regression. It checks advancing
callbacks, recording frame counts, queue bounds and errors, retains transcripts
and WAVs, and checks cleanup after a partially failed device start. With no
profile it skips. These checks require no speakers; they do not measure acoustic
or cable round-trip latency. Mono/surround routing is verified deterministically
in the native conversion tests.

## macOS private aggregate checks

Run `./compile.sh --tests coreaudio` on macOS for the optimized Release build
and relevant native CTests. The GUI tests exercise the real Local, Start Jam,
Join Jam and Settings controls using deterministic device metadata, including
Apply/Save/Cancel and persistent UIDs. Portable core tests validate single and
split channel layouts, output-device input-channel offsets, disabled input
streams, optional input and invalid/missing selections. Runtime configuration
and restart checks run with the existing four-peer lifecycle integration.

Hardware testing is opt-in. Obtain physical UIDs from
`release/Jam2.app/Contents/MacOS/jam2 list-devices`, then run:

```sh
export JAM2_COREAUDIO_OUTPUT_UID='<output device UID>'
export JAM2_COREAUDIO_INPUT_UID='<different input device UID>'
./compile.sh --tests coreaudio
```

The macOS-only `jam2_coreaudio_aggregate_integration` checks process-private
aggregate creation, output clock/input drift configuration, advancing native
callbacks, physical channel identities, playback-only operation and cleanup
after stop and a deliberately failed startup. Without both environment
variables, this hardware test skips. It opens the specified hardware at 48 kHz
with its current buffer size; ensure those devices support that sample rate.
Microphone permission for the native test runner may need granting on macOS.

Windows can run `compile.cmd --in-dev-shell --tests coreaudio` to build and test
the shared GUI, preferences, planning and runtime handoff. This does not compile
or validate the native CoreAudio backend. On macOS, finish with GUI checks of
single-device, split-device and None-input startup, an actual capture/playback
run, Stop/restart, missing/unplugged-device errors, and Settings Apply rollback.
Confirm unrelated applications can still use their devices and that no Jam2
aggregate remains after stop or process exit.

Implementation references: Apple's [aggregate device API](https://developer.apple.com/documentation/coreaudio/audiohardwarecreateaggregatedevice(_:_:)),
[private aggregate key](https://developer.apple.com/documentation/coreaudio/kaudioaggregatedeviceisprivatekey),
[input drift compensation](https://developer.apple.com/documentation/coreaudio/kaudiosubdevicedriftcompensationkey),
and [per-IOProc stream usage](https://developer.apple.com/documentation/coreaudio/audiohardwareioprocstreamusage).
