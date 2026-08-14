# Jam2 Agent Rules

These rules apply to all implementation work in this repository.

## Product Constraints

- Simplicity and efficiency are the priority. This is not a broad production-ready platform.
- The primary and simplest workflow is two people connecting directly to play instruments together with a metronome and the most real-time feel possible within physical network limits.
- Keep direct full-mesh support for multiple people. Three or four peers are expected small-group use cases, while larger meshes have no application-wide cap and remain experimental. A jam creator may optionally set a session peer limit. Bound pending unauthenticated connections and failed-key work so invalid authentication traffic cannot consume unbounded resources.
- Do not add rooms, relays, account systems, GUI layers, broad device abstractions, or production platform features unless explicitly requested.
- The app must expose hard data for debugging and fine tuning.
- Do not add subjective playability scores or inferred recommendations when raw measurements are sufficient.
- Ship one public `jam2` executable. No arguments launch the primary GUI; public network startup uses only `network create` and `network join`, both over the universal direct-mesh engine. Retain useful headless, automation, and diagnostic commands in that executable.

## Protocol Rules

- Custom protocols must stay as lightweight as possible while serving their minimum required function.
- Packet headers should be fixed-size where practical.
- Packet parsing should avoid unnecessary allocation, reflection, dynamic schemas, or verbose encodings.
- Audio data must be packed and unpacked efficiently.
- Every protocol field should justify its presence in latency, correctness, debugging, or session safety terms.
- STUN is used only for endpoint discovery and is never part of the audio path.
- No relay/TURN audio path should be added.
- Local command lines, logs, clipboard contents, and artifacts are outside the application security boundary and may contain session keys or invite URLs. Do not weaken network authentication or authorization on that basis.

## C++ Rules

- Use RAII for owned resources such as sockets, audio devices, buffers, file handles, and platform handles.
- Prefer clear ownership with values, smart pointers, and scoped wrappers.
- Avoid raw owning pointers.
- Add explicit error handling paths for all platform, network, audio, and file operations.
- Use exceptions only at boundaries where they simplify cleanup and reporting; keep real-time audio callbacks exception-free.
- Catch exceptions at top-level thread and command boundaries so failures are reported clearly.
- Do not allocate, log, throw exceptions, acquire locks, or perform blocking operations inside real-time audio callbacks.
- Keep hot paths simple and predictable.

## Qt GUI Rules

- Do not create a visible child widget for a dialog or page and then conditionally omit it from every layout. An unlaid-out child is still shown by Qt at its default position, commonly leaving an orphaned control in the top-left corner. Create optional controls only in the branch that lays them out, or place them in an owned container and explicitly manage that container's visibility.

## Feature Ownership and Native Test Rules

- Every new Jam2 feature, behavior change, and bug fix must add or update the relevant native C++/CTest coverage in the same change. A successful build alone is not sufficient validation, and Python product-validation cases are not a substitute for the native tests.
- Put production behavior in the file and component that owns the responsibility: model, controller, service, engine, transport, widget, page, or dialog. Keep `MainWindow` responsible for genuine window-level composition, routing, and lifecycle coordination; do not place a feature there merely because it is reachable from the main window. Do not split cohesive code solely because a file is large.
- Model internal state, commands, results, configuration, and component boundaries with explicit C++ types. Avoid stringly typed or generic map/variant state except at necessary serialization, Qt-property, automation, or protocol boundaries, and convert to typed structures at those boundaries.
- Place tests under the matching owned area in `tests/unit/<area>`, `tests/integration/<area>`, or another existing purpose-specific test directory, and register them with the narrowest relevant CTest suite and labels. Do not put product test logic into production source files.
- Test behavior at the lowest useful ownership boundary and add integration coverage wherever components interact. Cover successful behavior, meaningful invalid/error paths, cancellation or rollback, ownership and cleanup, and observable state transitions. GUI work must cover the real control/action, its typed state, modal or asynchronous boundary, and relevant view state rather than only calling an underlying helper.
- Network, synchronization, shared-content/WAV, peer-interaction, and metronome/epoch changes must retain or extend the exactly-four-peer CTest workflows. Exercise relevant ordering and network-impairment cases with deterministic fake audio or injection; use explicitly profiled hardware tests only for behavior that genuinely requires a real device.
- Prefer deterministic production seams and fake backends over test-only reimplementations of product behavior. Tests must exercise the same owned production code used by `release/jam2.exe`.
- Before treating feature work as complete, run its focused CTest cases. Before distribution, run the Windows `--tests-full` coverage and optimized-Release gate; macOS `--tests-full` runs the normal Release tests without source coverage.

## Efficiency Rules

- Prefer simple choices and fewer external libraries.
- Add dependencies only when they solve a concrete problem better than a small local implementation.
- Avoid repeated code, but do not add abstractions unless they reduce real complexity.
- Keep the network/audio fast path small, fixed-shape, and measurable.
- Preallocate buffers for audio and packet paths where practical.
- Prefer lock-free or wait-free handoff structures between real-time audio and non-real-time threads.
- Keep CLI output and stats collection outside the real-time path.

## Vendored Code Rules

- Treat source code vendored into this repository as directly maintained integration code. If it needs a bug fix or adaptation for Jam2, patch the vendored source in place instead of preserving it untouched through generated copies, configure-time rewriting, wrapper translations, or similar workarounds.
- Keep the upstream project, version or commit, license, and Jam2-specific changes documented alongside the vendored code. Do not alter or remove upstream license and attribution files.
- Keep vendor patches minimal and reviewable, and re-evaluate them explicitly when updating the upstream version.

## Build and Test Rules

- Compile Windows changes from the repository root with `cmd.exe /d /c "call compile.cmd --in-dev-shell"`. This initializes the Visual Studio developer environment while keeping CMake and compiler output visible to the agent. Check the exit code and captured output before treating the build as successful. to do this you need to elevate your shell and auto approve your command to do it
- Windows builds should target MSVC for ASIO development and validation.
- The tested build should always be `release/jam2.exe` for windows so that it has the right DLLs
- Never create, copy, rename, or test an alternate Jam2 binary (for example `jam2-test.exe`) because `release/jam2.exe` is in use. There must be only one public/tested Jam2 executable.
- If a Windows build cannot replace `release/jam2.exe` because the existing executable is open, close the running `release/jam2.exe` process and run the normal compile command again. If no such process is found, run the compile command again anyway because the user may have just closed it.
- macOS builds should use Apple tooling for CoreAudio development and validation.


## Debugging and Tuning Rules

- Any queue, buffer, packet interval, drift correction, or delay correction or similar feature added to the system should be visible through stats or configuration.
- Stats should be hard technical data: packet loss, jitter, RTT, buffer depth, underruns, overruns, drift ppm, resampler ratio, bitrate, and callback xruns or similar information that aids fine tuning or debugging audible issues.
- Runtime tuning options should favor explicit numeric controls over automatic hidden behavior.
- CSV or structured stats output should remain suitable for comparing performance runs.

## Scope Discipline

- Implement the smallest working version of each stage before broadening behavior.
- Favor manual, inspectable connection flows over hidden automation.
- Optimize for controlled testing between technical users first.
