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

Run one selected suite while developing:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests unit"
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests gui"
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests performance"
```

Run one registered CTest exactly:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests gui --test-name jam2_four_gui_agent_integration"
```

Run the pre-distribution Release gate only after focused implementation work is
done:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests-full"
```

`--tests-full` builds the normal `/O2` Release targets, stages the one public
`release/jam2.exe`, and runs the complete hardware-independent CTest catalogue
once. It does not collect source coverage.

After implementing major features, audit which maintained source and functions
the native catalogue exercises with the separate Windows coverage command:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --coverage"
```

`--coverage` builds and runs the instrumented MSVC catalogue, exports the native
coverage reports, and checks the reviewed maintained-function inventory. It
does not rerun the optimized catalogue; use `--tests-full` for that behavioral
gate. Because instrumentation replaces the staged executable while collecting,
the command restores a normal `/O2` `release/jam2.exe` before returning, even
when collection or the audit fails. `--test-name` can focus collection for
diagnosis; it writes a report and requires that test to pass, but only an
unfocused `--coverage` run enforces the complete maintained-source inventory.
`--hardware-profile` makes the explicitly profiled hardware and plugin coverage
mandatory during the full audit.

## macOS commands

The equivalent command shapes are:

```bash
bash ./compile.sh
bash ./compile.sh --tests unit
bash ./compile.sh --tests gui --test-name jam2_four_gui_agent_integration
bash ./compile.sh --tests-full
```

On macOS, `--tests-full` builds the normal Release targets and runs the complete
CTest catalogue once. It does not collect source coverage and never invokes the
Windows PowerShell coverage scripts.

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
- `network`: TCP/security boundaries and authenticated UDP adversarial cases;
  and
- `full`: every default hardware-independent CTest.

GUI tests run offscreen unless `--show-gui` is supplied to `gui` or the full
gate. Normal and selected test commands build only the requested target set.

## Results and temporary files

All automated runtime state is rooted under `build/test-artifacts`. A selected,
full, or coverage command removes that exact directory before starting and
again after complete success; failures retain coverage artifacts under
`build/coverage/test-artifacts` for diagnosis. Windows `--coverage` reports are
written under `build/coverage`, including the instrumented CTest log, summary,
source inventory, and function-level CSVs. Normal Windows and macOS test runs
do not create code-coverage reports.

The maintained implementation/review history is in `TEST-PLAN.md`,
`TEST-LOG.md`, and `TEST-REVIEW.md`. Cross-machine and long soak campaigns are
manual product testing and are intentionally not part of this automated gate.

## Retained Python tooling

`tools/jam2_test.py` remains for two-machine benchmarking, offline result
analysis, connectivity diagnostics, and bounded fuzz orchestration. Its former
`validate` and `stress` command families were retired after their product
contracts moved into C++/CTest. Python unit tests continue to validate the
retained Python tools themselves; they do not validate Jam2 product behavior.
