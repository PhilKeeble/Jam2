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

## Efficiency Rules

- Prefer simple choices and fewer external libraries.
- Add dependencies only when they solve a concrete problem better than a small local implementation.
- Avoid repeated code, but do not add abstractions unless they reduce real complexity.
- Keep the network/audio fast path small, fixed-shape, and measurable.
- Preallocate buffers for audio and packet paths where practical.
- Prefer lock-free or wait-free handoff structures between real-time audio and non-real-time threads.
- Keep CLI output and stats collection outside the real-time path.

## Build and Test Rules

- Compile Windows changes from the repository root with `cmd.exe /d /c "call compile.cmd --in-dev-shell"`. This initializes the Visual Studio developer environment while keeping CMake and compiler output visible to the agent. Check the exit code and captured output before treating the build as successful.
- Windows builds should target MSVC for ASIO development and validation.
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
