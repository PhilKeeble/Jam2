# Jam2 Single-Application Refactor Review

## Executive Assessment

Consolidating `jam2-gui` and the `jam2` engine into one application is likely worthwhile for maintainability, state sharing, startup reliability, packaging, and user flow. It should not be treated as a direct audio-latency optimization: the current GUI/process boundary is outside the real-time audio callback, so removing it mainly simplifies control and observability.

The safest direction would be to:

- Keep the audio callback, rings, UDP worker, scheduling priorities, sample-time metronome model, and packet timing semantics intact.
- Extract the current engine from the CLI entry point behind a small Qt-free C++ API before integrating it into the GUI.
- Run audio and network work on dedicated threads rather than the Qt event thread.
- Replace local process, stdin, TCP, and JSONL communication with typed commands and fixed-shape snapshots.
- Retain public headless, automation, and diagnostic commands in the same
  `jam2` executable, with the no-argument GUI remaining the primary use case.
- Keep mesh support. Optimize the normal experience for two peers, while making small sessions of three or four peers straightforward and retaining the ability to connect more peers where hardware and network capacity allow.
- Treat peer-control protocol changes and UDP fast-path optimization as separate, measurable work.

The largest risk is not having two components in one process. It is safely extracting the audio, network, timing, recording, and statistics code from the 8,000-plus-line CLI entry point without changing its real-time behavior.

Sections describing the "current" two-process architecture record the original
review baseline. They explain what the refactor is removing and are not current
status; [refactor-plan.md](refactor-plan.md) is the status authority.

## Original Review Architecture (Historical)

Jam2 currently uses three distinct communication paths:

| Path | Original review format | Relevance |
| --- | --- | --- |
| Engine-to-engine UDP | Fixed 48-byte binary header and packed PCM24 payload | Critical audio and timing path |
| GUI-to-local engine | Binary framed TCP controls/meters, stdin commands, and JSONL stdout status | Local control and observability |
| GUI-to-remote GUI | Newline-delimited JSON over authenticated TCP | Session settings, song grid, looper assets, and mesh coordination |

The live audio UDP protocol is already binary. Its header contains packet type, flags, session id, sequence, sample time, send time, payload length, and an authentication tag. Audio samples are packed as three-byte PCM24 values. Changing peer-control JSON would therefore not make received audio immediately ingestible; audio is already received through a separate binary path.

The local GUI-engine control path has also partly moved away from JSON. It has a fixed binary frame header and numeric command opcodes for controls, meters, clock state, transport state, and recording events. JSON remains in engine startup/status output and the remote GUI control plane.

The post-refactor implementation is different: one `jam2` application owns the
engine in process; UDP protocol v2 uses one explicitly encoded 36-byte header
and session-wide PCM16 or PCM24; and requested Track/Looper chunk bodies use
bounded authenticated binary control frames. The table above remains only the
baseline that motivated those changes.

## Benefits of Consolidation

### Lifecycle and Packaging

The GUI currently has to locate an adjacent engine executable, construct arguments, launch it, monitor stdout and stderr, open a loopback control server, wait for the engine to connect, and escalate through quit, terminate, and kill during shutdown.

A single application would remove:

- Missing or mismatched engine executable failures.
- Command-line generation and reparsing between internal components.
- Child-process startup and replacement races.
- Local TCP bind, connection, framing, and reconnect failures.
- States where the child is running but its control connection is unavailable.
- JSONL buffering and stdout parsing for runtime state.
- macOS bundle copying and release staging of a second executable.
- Forced child termination as part of ordinary shutdown.

Startup could become `Engine::start(config)`, with shutdown handled through an explicit stop request and bounded thread joins owned by RAII objects.

### State Sharing

The GUI currently reconstructs engine state from JSON status, binary control snapshots, acknowledgements, process state, exit callbacks, and locally cached UI values. This creates multiple representations of connection, metronome, transport, recording, track, and local-engine state.

An in-process engine could expose:

- One immutable start configuration.
- Atomic numeric runtime controls for simple gains and toggles.
- A bounded typed command queue for infrequent operations.
- A fixed-shape technical statistics snapshot.
- Explicit lifecycle and connection states.
- A bounded event queue for errors, recording completion, and transport changes.

This would reduce serialization, duplicated conversions, and state-reconciliation branches without putting Qt objects in the engine.

### User Flow

A unified application could simplify normal operation:

- The user runs one `jam2` application and never selects an engine executable.
- Starting, joining, or hosting a mesh changes the engine session without launching a child process.
- Local perform mode becomes an engine with networking disabled, rather than a distinct CLI/application lifecycle.
- Disconnecting can leave local metronome and track work active when desired.
- Device and network errors can be presented as typed events rather than parsed console text.
- The release contains one application to install, sign, bundle, and diagnose.

The default UI should remain centered on a direct two-person jam. Mesh controls can progressively expose three/four-person and larger sessions without making the common two-peer flow more complicated.

## Efficiency Considerations

### Expected Gains from One Process

Consolidation removes low-frequency control overhead:

- Child-process creation.
- Argument construction and parsing.
- Stdin text parsing for GUI actions.
- Loopback TCP framing, copies, socket wakeups, and reconnect logic.
- JSON creation, UTF-8 conversion, pipe transfer, line buffering, parsing, and string-key lookup for engine statistics.
- Duplicate conversions between strings, JSON values, command opcodes, and UI state.

These changes can reduce incidental CPU work and improve responsiveness, but they are unlikely to materially reduce end-to-end audio latency because none of this work belongs in the real-time callback.

### Original UDP Fast-Path Findings (Now Implemented)

The original binary UDP implementation performed avoidable per-packet work:

- Packet encoding returns a newly allocated `std::vector`.
- PCM24 packing returned another allocated vector.
- Packet authentication during decoding copies the full packet so the authentication field can be zeroed.
- PCM24 unpacking allocated a vector of decoded samples.
- Expected malformed network input is reported through exceptions in the receive path.

Potential improvements include:

- Preallocated transmit and receive packet buffers.
- Encoding into caller-owned spans.
- Authentication without copying the packet.
- Direct PCM16/PCM24 unpacking into a preallocated audio block or ring reservation.
- Fixed-capacity receive, reorder, and mesh-mix storage where practical.
- Explicit parse-result codes for invalid packets instead of exceptions for routine rejection.

These changes now live in the Qt-free core and are measured independently for
two-peer and mesh sessions. Phase 12 additionally replaced the old UDP layout
once and added the creator-selected PCM16/PCM24 session format; no old parser or
codec-selection compatibility branch remains.

### Peer JSON

Converting GUI-to-GUI JSON to a binary protocol would reduce allocation and message size, but most of those messages are controls or human-paced edits. Their cost is unlikely to affect live audio.

Binary framing may be useful for frequent metronome or transport state, mesh membership snapshots, and asset chunks. Song and arrangement metadata can remain readable until measurements show a concrete need. If converted, use an explicit fixed binary envelope with opcode and payload length; do not send native C++ structs directly because padding, endianness, versioning, and untrusted lengths still require validation.

## Redundant and Removable Code

A precise dead-code count requires compiler analysis and runtime coverage, so these figures are estimates based on static inspection.

### Process-Boundary Code

Approximately 600–1,000 lines appear primarily attributable to the local process boundary:

- `Jam2Process` and engine executable-path handling.
- GUI child launch, replacement, exit, and forced-shutdown logic.
- The GUI local control TCP server and frame parser.
- The engine GUI-control socket thread, framing, acknowledgements, and snapshots.
- GUI-specific JSONL status production and parsing.
- Release staging and bundle copying for the second executable.
- UI options for engine path, GUI-control socket, and extra CLI arguments.

Some state and statistics behavior must remain behind typed interfaces, so not every line in those regions would simply disappear.

### Duplicate Controls and CLI Compatibility

The engine supports both stdin text commands and binary GUI-control commands
that ultimately update the same runtime state. The public CLI remains, but the
legacy runtime stdin loop, GUI compatibility output, and duplicate mode parsers
can be removed while retaining the underlying behavior as typed commands.
Public network startup is limited to `network create` and `network join`;
`listen`, `connect`, `mesh`, and static-membership aliases do not need backward
compatibility.

Ordinary headless commands stay focused on core audio/network startup and
shutdown. Metronome, grid, transport, recording, and other scheduled automation
actions belong to the explicit typed debug scenario/pipe contract rather than a
runtime stdin loop or broad interactive option surface.

Device listing, device tests, metering, benchmark support, and headless audio
are useful diagnostics rather than obvious dead code. They remain supported by
the one public executable through the shared application interfaces.

### Mesh Is Retained Scope

Mesh is not a removal candidate. Its coordinator, peer-list, per-peer timing/statistics, mixing, and reconnection behavior must be included in the extracted engine design.

The current project rules describing Jam2 as strictly two-person are out of date. The intended product shape is:

- Two people remain the primary and simplest workflow.
- Three or four peers are an expected small-group use case.
- Larger direct full-mesh sessions remain available without an
  application-wide peer cap, subject to the machine's actual CPU, bandwidth,
  packet-rate, and device limits. A creator may optionally configure a limit
  for that jam.
- Audio remains direct UDP with no relay/TURN audio path.
- Per-peer and aggregate raw measurements should expose how mesh cost scales.

Mesh increases the importance of preallocation, bounded storage, per-peer statistics, and avoiding packet-sized allocation/copy work. It also means the engine interface must support peer membership changes without relying on child-process restarts.

### Likely Reduction

Keeping mesh and useful diagnostic support, approximately 1,100–2,000 lines could plausibly be removed through consolidation and retirement of local process/CLI compatibility. The more valuable outcome is decomposing the large CLI and GUI translation units into components with clear ownership.

## Risks

### GUI Interference

The engine must never run its audio or UDP loop on the Qt GUI thread. Painting, layouts, modal dialogs, asset sync, file work, and timers can stall unpredictably.

Keep separate execution domains for:

- The host audio callback controlled by ASIO/CoreAudio.
- UDP send/receive and mesh processing.
- Recording and file work.
- Engine supervision/control.
- The Qt GUI event loop.

Communication with the callback must remain atomic, lock-free, or wait-free, with no allocation, logging, exceptions, locks, or blocking calls in the callback.

### Loss of Process Isolation

With one process, an audio backend crash or memory error ends the entire application, and a deadlocked engine cannot be handled by killing a child. This increases the importance of explicit ownership, stop tokens, bounded joins, top-level thread exception handling, and preventing callbacks into destroyed GUI objects.

### Scheduling Regressions

The engine currently applies platform-specific scheduling and priority behavior. Extraction must preserve it rather than relying on default Qt thread scheduling. Comparisons should include callback gaps, receive-loop gaps, jitter, loss, playback depth, underruns, overruns, drift, resampler ratio, and actual platform priority/QoS data.

### Metronome and Transport Regressions

The GUI must not become the musical clock. The robust model remains:

- Engine sample frames are authoritative.
- Metronome audio is generated locally.
- Peer messages carry sample-time epochs and patterns.
- Network timing projects remote events onto the local engine timeline.
- GUI timers only display state and submit commands.

For mesh, per-peer clock observations and coordinator behavior must remain equivalent before considering changes to timing algorithms.

### Refactor Breadth

Moving the CLI code directly into `MainWindow` would increase complexity. The engine should first become a Qt-free component with stable configuration, commands, events, and snapshots. The GUI should then replace `QProcess` with that component.

### Test Harness Compatibility

The Python stress and benchmark tools launch `jam2` through its public
headless/debug surface. Removing that interface would discard valuable
regression coverage. GUI, ordinary headless commands, and debug automation must
therefore remain adapters over the same application session, `Engine`, and
`NetworkSession` implementations in the one shipped executable.

## Recommended Target Architecture

```text
Qt GUI thread
    |
    | typed commands, events, immutable snapshots
    v
Shared application session controller
    |-- TCP bootstrap, authentication, membership, and authorities
    |-- Engine
    |     |-- ASIO/CoreAudio device and real-time callback
    |     |-- recording/file worker
    |     `-- statistics snapshot publisher
    `-- NetworkSession
          `-- UDP peers, scheduling, timing, and mixing
```

The engine boundary should remain small:

- `EngineConfig`: complete immutable start/session configuration.
- `Engine::start(config)`.
- `Engine::requestStop()` and `Engine::join()`.
- `Engine::submit(Command)` using bounded storage.
- Atomic runtime controls for appropriate simple values.
- `EngineSnapshot Engine::snapshot()` with fixed-shape local technical data,
  plus bounded per-call/paged peer snapshots published outside real-time work
  so diagnostics do not impose a global peer-count ceiling.
- A bounded event queue for lifecycle, network, recording, and error events.
- Explicit peer add/remove/update operations suitable for mesh membership changes.

Qt types, widgets, JSON objects, dialogs, and GUI callbacks should not enter the engine or core library.

## Remaining Architecture Context

The final application uses one shared non-UI session controller for GUI,
ordinary headless commands, and explicit debug automation. `Engine` owns audio,
callback time, local media, and technical state. `NetworkSession` owns UDP peer
streams and mixing. The shared controller owns TCP bootstrap, authentication,
membership, the immutable session contract, and musical authority coordination.

Typed commands, events, and snapshots replace `Jam2Process`, in-process CLI
invocation, local loopback control, the runtime stdin command loop, and JSONL
state reconstruction. A dedicated bounded inherited pipe may be used by debug
automation; it is not the former CLI stdin protocol.

Useful historical stress artifacts can be compared when a change needs them,
but they are engineering evidence rather than mandatory numeric gates. The
authoritative implementation order and completion status live only in
[refactor-plan.md](refactor-plan.md).

## Decision Summary

Consolidation is worth considering as a maintainability and product-coherence refactor. It should not be sold as a guaranteed latency improvement. The most credible network-efficiency opportunity is reducing allocations and copies in the existing binary UDP path, particularly as mesh peer count grows.

The refactor is safest if engine extraction, GUI integration, UDP optimization, and peer-control protocol conversion remain separate measured stages. This preserves the ability to compare behavior and identify regressions rather than changing application structure, timing, and wire formats simultaneously.
