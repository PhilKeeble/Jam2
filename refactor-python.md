# Jam2 Python Tooling and Headless Automation Refactor

## Purpose

This document defines how Jam2 keeps its Python benchmark, stress, impairment, coordination, artifact, and offline-analysis tooling while the GUI and audio engine are consolidated into one public executable.

It is a focused companion to:

- [refactor-plan.md](refactor-plan.md), the dependency-ordered worker plan.
- [refactor-modes.md](refactor-modes.md), the local/network and full-mesh design.
- [refactor-binaries.md](refactor-binaries.md), the single-application review.
- [refactor-efficiency.md](refactor-efficiency.md), the code and fast-path audit.
- [refactor-security.md](refactor-security.md), the lightweight security contract.

This is a design and migration plan. No tooling or application implementation is marked complete by this document.

## Decision Summary

Keep Python as the external test orchestrator and offline analysis layer.

The unified `jam2` executable must retain an explicit headless/debug automation surface over the same `Engine` and `NetworkSession` used by the GUI. Static and scheduled cases should be expressible through a strictly validated scenario file ingested at startup. Tests that must react to runtime state should use a bounded, versioned, inherited local automation channel rather than an always-listening network service.

The resulting ownership split is:

| Owner | Responsibilities |
| --- | --- |
| C++ `Engine` and `NetworkSession` | Real/headless audio execution, deterministic test input, frame-accurate scheduling, local/create/join/static lifecycle, recording, raw per-peer statistics, effective configuration, bounded events and snapshots |
| Unified executable debug/headless adapter | Scenario parsing, secret-free startup configuration, conversion to typed engine commands, structured event/artifact output, bounded local automation transport |
| Python tooling | Process and machine orchestration, peer topology, impairment, retries, experiment matrices, artifact collection, repeated comparisons, offline WAV/CSV analysis |
| GUI | Normal user workflow and an optional thin diagnostic-bundle action; not a benchmark coordinator |

## Goals

- Preserve robust local stress, two-host benchmark, and future three/four-peer benchmark workflows after binary consolidation.
- Exercise exactly the same engine and network implementations used by ordinary GUI sessions.
- Support local-only, create-jam, join-jam, and explicit static full-mesh automation.
- Preserve both real-device ASIO/CoreAudio tests and headless synthetic tests.
- Make scenarios reproducible through versioned declarative inputs and explicit numeric settings.
- Schedule timing-sensitive actions against the engine frame clock rather than Python wall-clock sleeps.
- Produce raw artifacts that can be correlated across every peer in a mesh.
- Keep Python and debug-control work outside the real-time callback and packet fast path.
- Keep session secrets out of command lines, persisted scenario files, logs, CSV, benchmark metadata, and uploaded artifacts.
- Retain raw measurements and explicit deltas rather than generating subjective playability scores or hidden recommendations.

## Non-Goals

- Do not move the benchmark case matrix or offline audio analysis into the real-time engine.
- Do not add a production room, relay, TURN, account, telemetry, or cloud benchmark service.
- Do not add a remotely exposed automation server to ordinary application startup.
- Do not make Python part of the live audio callback or production packet-processing path.
- Do not create a separate benchmark audio engine whose behavior can diverge from the GUI engine.
- Do not make debug scenarios a bypass around peer authentication, source authorization, grid authority, or endpoint proof.
- Do not automatically choose a “best” tuning profile when raw measurements and deltas are sufficient.

## What the Current Tools Actually Exercise

The current Python layer is not one subsystem. It performs several separate jobs:

| Current area | Current role |
| --- | --- |
| [tools/jam2_harness.py](tools/jam2_harness.py) | Launches Jam2 processes, watches structured startup lines, sends text runtime commands, and locates CSV artifacts |
| [tools/run_stress_local.py](tools/run_stress_local.py) | Defines scenarios, launches local peers, schedules controls, applies impairment, analyzes recordings, and writes verdicts/results |
| [tools/udp_stress_proxy.py](tools/udp_stress_proxy.py) | Applies deterministic two-endpoint delay, jitter, loss, reorder, and burst behavior |
| [tools/run_benchmark_server.py](tools/run_benchmark_server.py) and [run_benchmark_client.py](tools/run_benchmark_client.py) | Coordinate two machines, launch one Jam2 side each, retry cases, and move artifacts |
| [tools/jam2_benchmark_control.py](tools/jam2_benchmark_control.py) | Implements the current one-client benchmark lifecycle and artifact control connection |
| [tools/jam2_profiles.py](tools/jam2_profiles.py) and [jam2_benchmark_suite.py](tools/jam2_benchmark_suite.py) | Define tuning variants and benchmark cases |
| [tools/jam2_metrics.py](tools/jam2_metrics.py) | Reduces raw CSV into explicit technical summaries |
| [tools/jam2_audio_analysis.py](tools/jam2_audio_analysis.py) | Performs offline stem, tone, pulse, clipping, pop, and metronome analysis |
| [tools/upload_server.py](tools/upload_server.py) | Provides a temporary manual LAN log collector |
| [tools/connection_test.py](tools/connection_test.py) | Diagnoses STUN mapping and direct UDP reachability independently of the app |

With a real audio device, Jam2 currently creates test tone/pulse samples inside the ASIO or CoreAudio callback. Python selects the mode but does not synthesize or forward those live samples. The generated input then follows the normal capture ring, packet, receive, playback, and recording paths. This is valuable and must remain true after engine extraction.

The current two-host benchmark control connection carries lifecycle messages and artifacts, not live audio. The Jam2 audio path remains direct UDP between the Jam2 processes.

## Current Limitations to Preserve as Baseline Gaps

### The tooling is coupled to legacy command names and output text

The harness launches `listen`, `connect`, or `mesh`, rewrites the current invite URL, scrapes startup JSONL/stdout, sends text commands over stdin, and finds the newest CSV in an output directory.

These are compatibility mechanisms, not the desired final automation contract. They must remain available until the replacement produces accepted side-by-side results.

### Mesh stress is not equivalent to normal stress

The current mesh runner:

- Runs only clean static localhost peer lists.
- Uses headless audio for every peer.
- Uses the same test signal for all peers.
- Does not apply the Python impairment proxy.
- Does not provide independently simulated peer clocks.
- Does not exercise real device callback/xrun behavior.
- Uses reduced aggregate and per-peer measurements compared with the mature two-person path.

It is useful for current process, packet, fan-out, and mix evidence, but it is not the quality baseline for the universal `PeerStream` implementation.

### The benchmark coordinator is one-client shaped

The current benchmark control state owns one active remote peer. It cannot coordinate an arbitrary two-, three-, or four-peer full mesh, assign per-peer roles, or collect N correlated artifact sets.

### Native and Python profile definitions can drift

The `fast`, `moderate`, and `safe` values exist in native tuning-profile code and are repeated in Python. Python also sends the named profile plus many explicit overrides. This can hide native default changes and produce metadata that describes Python's expectation rather than the effective runtime configuration.

The native application must become the source of truth and emit the validated effective configuration for every run.

### Result aggregation is server/client shaped

The current primary metric reducer combines `server` and `client`. Mesh support produces a smaller aggregate. The universal network engine requires normalized local-peer and remote-peer identities so one result model works for one, two, three, or more remote peers.

### Some planned baseline conditions do not have a deterministic seam

The current Python proxy does not duplicate packets, and the current headless flow does not inject independent positive/negative device-clock drift per peer. Those baseline cases must be added through explicit test facilities or recorded as unavailable in the legacy baseline.

### Current artifact handling can expose secrets or unbounded cold-path work

The current benchmark can persist the invite URL/session key in case state and metadata. The current benchmark upload reads complete archives into memory, and the standalone upload server has no authentication or hard request-size limit.

These are not acceptable properties of the retained automation contract. The fixes are cold-path bounds and secret separation and do not require audio-path cost.

## Target Architecture

```text
Python controller
    |
    +-- scenario matrix and repeat/seed selection
    +-- local or remote machine agents
    +-- per-edge impairment processes
    +-- artifact collection and offline analysis
    |
    +-- launches the same unified jam2 executable
            |
            +-- debug/headless adapter (explicit command only)
            |       |
            |       +-- bounded scenario parser
            |       +-- inherited secret/control channel
            |       +-- structured events and artifact manifest
            |       +-- typed EngineCommand conversion
            |
            +-- shared persistent Engine
                    |
                    +-- real ASIO/CoreAudio or headless device
                    +-- deterministic native test source
                    +-- frame-clock command scheduler
                    +-- recording and raw snapshots
                    +-- optional universal NetworkSession
                            |
                            +-- create/join/static lifecycle
                            +-- one independent PeerStream per peer
                            +-- direct full-mesh UDP
```

There is one engine implementation. “Debug” changes how it is configured and controlled, not how audio, timing, authorization, or network state is processed.

## Unified Executable Surface

The final spelling can be selected during Phase 8, but the public shape must preserve ordinary headless commands and add one explicit scenario-driven debug entry point:

```text
jam2
jam2 local [options]
jam2 network create [options]
jam2 network join <invite> [options]
jam2 network static [options]
jam2 debug run <scenario.json>
jam2 debug describe --json
```

- No arguments launch the GUI.
- `local`, `network create`, `network join`, and `network static` remain usable directly by technical users.
- `debug run` is opt-in and is never activated by a received peer message.
- `debug describe` reports supported scenario schema versions, commands, test inputs, profiles, limits, protocol versions, and output schema without exposing secrets.
- All commands instantiate the same `Engine` and, when requested, the same `NetworkSession`.

The Python tools may call the direct headless commands for simple cases or generate a scenario file for deterministic scheduled cases.

## Scenario File Contract

### Purpose

A scenario file describes non-secret, reproducible startup configuration and scheduled actions. It replaces long fragile command lines and output scraping without becoming a second application configuration system.

JSON is suitable because this is bounded startup/debug input outside the fast path and Python already produces it. A fixed schema and hard size/count/string/numeric limits matter more here than a binary encoding.

### Illustrative shape

The exact names are provisional, but the responsibilities should remain recognizable:

```json
{
  "schema": "jam2-debug-scenario-v1",
  "run_id": "non-secret-run-id",
  "operation": "network.static",
  "audio": {
    "backend": "headless",
    "device_id": null,
    "sample_rate": 48000,
    "buffer_frames": 64,
    "input_channels": [1],
    "output_channels": [1]
  },
  "network": {
    "bind": "127.0.0.1:50000",
    "peer_ids": ["peer-b", "peer-c"],
    "secret_source": "inherited-control"
  },
  "tuning": {
    "profile": "fast",
    "overrides": {
      "frame_size": 64,
      "jitter_buffer_frames": 512
    }
  },
  "test_input": {
    "kind": "tone",
    "frequency_hz": 440.0,
    "level": 0.125,
    "phase": 0.0
  },
  "capture": {
    "duration_ms": 30000,
    "stats_interval_ms": 5000,
    "record_stems": true,
    "output_root": "artifacts/run-id/peer-a"
  },
  "commands": [
    {
      "after_event": "network.active",
      "delay_frames": 48000,
      "command": "metronome.start"
    }
  ]
}
```

### Required operation types

- `local`: persistent engine with no UDP socket.
- `network.create`: create a real network session and emit an invite through the protected automation channel.
- `network.join`: receive an invite through the protected automation channel and join through the normal authenticated bootstrap path.
- `network.static`: use explicit test membership for deterministic local/multi-process stress while retaining the normal universal per-peer audio engine.

Static membership is a test/bootstrap convenience. It must not create a simplified receive, timing, mixing, or authority implementation.

### Required functional coverage

The scenario and reactive command contracts must be able to exercise supported functionality without adding test-only engine mutations:

- Real-device and headless engine start, stop, cancellation, and failure paths.
- Create, join, static membership, leave, reconnect, late join, peer restart, and endpoint change.
- Every metronome mode, BPM/pattern change, grid start/stop, authority change, and missing-authority behavior.
- Prepared-track load, play, quantized restart, seek, loop, level, stop, and invalid-fixture handling.
- Jam recording and track-take arm/start/stop/cancel lifecycle.
- Local monitor, send, remote, metronome, per-peer gain, and mute controls.
- Supported runtime tuning changes and rejected immutable-session changes.
- Shared asset request/transfer/application through the normal bounded peer-control and worker path using explicit local fixtures and expected hashes.
- Periodic/final snapshots, CSV, recordings, artifact manifests, queue saturation, and reason-specific errors.
- Controlled disconnect, packet impairment, stale/duplicate commands, and recovery behavior.

Local fixtures are selected by the local scenario adapter and validated before use. A scenario must not create a remote path-selection bypass or allow a peer to name an arbitrary local file.

### Validation rules

- Require a recognized schema identifier and reject unsupported versions.
- Apply a small hard file-size limit before parsing.
- Bound every string, array, numeric value, scheduled command count, and output-path length.
- Reject unknown operation and command names.
- Validate all engine/tuning values through the same native configuration validator used by GUI/headless options.
- Resolve local output paths once, outside the callback, beneath the caller-selected output root.
- Reject contradictory settings before opening the audio device or UDP socket.
- Emit the effective validated configuration; never treat the requested file as proof of what ran.
- Do not include session keys, invite secrets, peer proof material, or upload credentials in the persisted scenario.

## Reactive Local Automation Channel

Scenario files cover static startup and commands known in advance. Stress and recovery tests also need to react to events such as peer activation, disconnect, authority changes, queue saturation, or recording completion.

The unified executable should therefore offer a small local automation channel only when `debug run` explicitly requests it.

### Transport requirements

- Prefer inherited stdin/stdout handles or an inherited local pipe so no listening socket is opened.
- If platform constraints require a local endpoint, bind only an OS-local IPC mechanism with caller ownership and an unpredictable per-run name; do not expose it on the LAN.
- Use bounded, versioned, length-prefixed frames rather than unbounded newline accumulation.
- Cap input/output frame size, queued frames, frames processed per turn, and incomplete-frame time.
- Separate structured protocol output from human stderr diagnostics.
- Close the channel and stop or continue according to explicit scenario policy when the controller disappears.

JSON payloads remain acceptable for this cold, local, human-rate control layer. The adapter must parse and validate them outside the engine and convert them into bounded typed `EngineCommand` values.

### Minimum message families

- Hello/capability and schema negotiation.
- One-time secret delivery for create/join/static coordination.
- Start, stop, cancel, and orderly shutdown.
- Typed local user actions such as metronome, grid, transport, recording, peer gain/mute, and tuning changes.
- Lifecycle and peer-state events.
- Fixed-shape snapshots and reason-specific errors.
- Effective configuration and artifact-manifest publication.

Automation commands that are intended to simulate GUI user actions must enter the same local command/request path as the GUI. They must not directly mutate remote-authority state or bypass normal peer control.

## Secret Handling

The scenario file and artifacts use only a non-secret `run_id` and stable test peer IDs.

For automated create/join/static cases:

1. Python creates or receives secret material in memory.
2. It delivers the secret through an inherited automation handle after the child starts, or asks one child to create the session and forwards the returned invite in memory to the joining children.
3. The executable copies the secret into bounded session-owned memory and acknowledges only a non-secret identifier.
4. Neither side writes the secret to the scenario file, command line, routine stdout/stderr, CSV, metadata, result JSON, archive, or error text.
5. The artifact collector scans outputs for known secret values before accepting a run.

The exact in-memory handoff may change by platform, but persisting the secret as a convenience is not an accepted fallback.

## Native Test Facilities Required by Python

### Deterministic capture-path test source

Keep the current principle that test input is produced in the native audio callback path. Extend it only where a concrete mesh or timing test requires it:

- Silence.
- Tone with explicit frequency, level, and starting phase.
- Pulse with explicit period, width, level, and phase.
- Metronome/grid-derived pulse.
- Optional bounded per-peer signal tag using distinct explicit tone or pulse parameters.

The same source contract must work with real devices and the headless device. Real-device injection measures callback/driver scheduling while replacing physical ADC input. Headless injection measures engine/network scaling without claiming hardware coverage.

### Frame-accurate command scheduler

Python `time.sleep()` is not an acceptable timing source for metronome, transport, recording, or exact recovery comparisons.

The engine/debug adapter should support commands triggered by:

- A specific `LocalEngineFrame`.
- A frame offset after an accepted local lifecycle event.
- A future grid revision/bar scheduled through the normal authority flow.
- A bounded wall-clock timeout only for supervisory failure handling.

Every scheduled command should report requested frame, applied frame, difference, source, and rejection reason.

### Effective configuration and capabilities

The native executable is the source of truth for named profiles and supported numeric limits.

Python should query or receive:

- Named profiles and all effective values.
- Supported sample/frame formats and frame sizes.
- Debug schema and automation protocol versions.
- Available test sources and command families.
- Active device/backend/rate/buffer/channel configuration.
- Control/UDP protocol and security-limit versions.

Python may define experimental override matrices, but it must not duplicate the native defaults as authoritative values.

## Python Architecture

The existing scripts can migrate incrementally; a broad framework rewrite is unnecessary. The eventual responsibilities should be separated clearly enough that two-peer and mesh cases share the same runner and result model.

### Scenario catalog

- Defines case names, one-variable tuning variants, signals, durations, repeats, and expected hard invariants.
- Produces versioned scenario JSON.
- Stores explicit impairment parameters and seeds.
- Does not contain session secrets.
- Does not copy native profile defaults; it requests a profile name and explicit overrides.

### Local controller

- Launches two or more unified `jam2 debug run` processes.
- Selects real devices or headless audio independently per peer.
- Creates static/create/join topology inputs.
- Starts per-edge impairment where requested.
- Delivers secrets over inherited handles.
- Waits on structured lifecycle events instead of scraping human text.
- Applies reactive commands and bounded timeouts.
- Collects one artifact manifest per peer.

### Multi-machine coordinator and agent

The current server/client naming should evolve toward one coordinator plus one lightweight agent per machine:

- The coordinator owns suite/run/attempt identity and the peer/topology plan.
- An agent can launch one or more local Jam2 peers when a test requires it.
- The same agent protocol supports two, three, or four machines.
- Every peer reports stable peer ID, machine ID, role, actual endpoint state, and artifact manifest.
- A reconnect never associates stale artifacts with a newer attempt.
- Coordinator/agent traffic remains outside the audio path.

This can be built by generalizing the current benchmark control code rather than introducing a new external dependency.

### Offline analysis

Keep WAV and CSV analysis in Python because it is cold-path work, easy to inspect, and likely to evolve more rapidly than the engine.

Analysis should:

- Preserve raw CSV, recordings, scenario, effective configuration, and manifest.
- Normalize results by `run_id`, `machine_id`, `local_peer_id`, and `remote_peer_id`.
- Report absolute values and explicit deltas.
- Retain repeat-level results rather than only an aggregate.
- Apply named hard correctness thresholds transparently.
- Avoid subjective scores or inferred profile recommendations.

## Full-Mesh Stress and Impairment

### Required topology coverage

- Two peers through the universal full-mesh engine.
- Three peers with all edges clean.
- Four peers with all edges clean.
- One impaired edge while unrelated edges remain clean.
- One impaired peer whose edges use explicit independent conditions.
- Late join, leave, restart, and endpoint migration.
- One real-device peer with remaining peers headless on one machine.
- Real-device peers on separate machines for actual hardware/network validation.

### Impairment ownership

Controlled packet delay, loss, duplication, reorder, and burst behavior should remain external to the product audio engine for process-level tests. This keeps the production packet path honest and allows the proxy's own packet counts to be compared with Jam2's observations.

For local full mesh, use one explicitly modeled impairment edge per peer pair or a test-only virtual network that retains edge identity. This is test infrastructure and is not a product relay/TURN path.

Every proxy run should include a zero-impairment proxy baseline so Python scheduling/proxy overhead is visible.

Independent positive/negative clock drift is better implemented through a deterministic native headless/fake-clock test seam than by delaying UDP packets. Packet delay is not device-clock drift.

## Evidence and Artifact Contract

Every peer run should publish a final manifest containing at least:

- Non-secret suite, run, attempt, machine, local-peer, and role identifiers.
- Source revision or exact working-tree identity.
- Executable/build identity and platform.
- Audio backend, device identity, requested and active rate/buffer/channels.
- Effective profile and every numeric override.
- Debug scenario, automation, control, and UDP protocol versions.
- Topology and peer count without session secrets.
- Test input parameters.
- Impairment parameters and random seed for every affected edge.
- Start/end reason and process return state.
- Paths and hashes for raw CSV, stdout/stderr diagnostics, recordings, analysis, and result files.
- Secret-scan result.

CSV/stat output must support:

- One local-engine row stream.
- One aggregate `NetworkSession` view.
- One normalized view per remote peer/edge.
- Stable non-secret peer identity independent of endpoint.
- Queue capacities, high-water marks, and drop/rejection counters.
- Callback, packet scheduler, per-peer clock/drift/playout, mix, metronome, transport, and lifecycle measurements required by [refactor-plan.md](refactor-plan.md).

Python should consume the published manifest rather than choose the newest file in a directory.

## Test Layers

### Native unit/component tests

Use CTest/native harnesses for packet codecs, rings, framing, replay, horizons, timing maps, drift/resampler bounds, mix scheduling, authority, parsers, and fuzz seams. Python process tests should not be the only way to exercise these primitives.

### Headless deterministic integration

Use headless peers for repeatable lifecycle, packet, mesh, CPU, memory, bandwidth, mix, command, and deterministic clock tests. These runs do not claim real device/driver coverage.

### Real-device local stress

Use native callback test input with ASIO/CoreAudio devices and controlled localhost impairment. Compare the same case headless and real-device to isolate callback, driver, device buffer, OS priority, and hardware-clock effects.

### Multi-host direct benchmark

Run one normal engine per real machine over the actual direct network, with the Python control plane used only for orchestration and artifact movement. These runs validate real network, hardware, and cross-machine clocks.

### Real-session diagnostic capture

An optional GUI action may package raw diagnostics from an ordinary jam. This supplements controlled tests with field evidence but is not a replacement for reproducible scenarios.

## Diagnostic Bundle and Upload Direction

The application may later expose a small explicit diagnostic-bundle action:

- Stop the real-time audio/network work before transferring a bundle.
- Include the effective configuration, raw CSV, errors, and non-secret manifest by default.
- Make recordings an explicit opt-in because of size and audio privacy.
- Scrub secrets and verify the bundle before transfer.
- Use bounded streaming, incremental hashing, size/time/concurrency limits, cancellation, and temporary-file cleanup.
- Never block leaving a jam indefinitely.

The GUI should not become a general upload server. Automated benchmark artifact collection remains a coordinator/agent responsibility. The temporary standalone Python uploader can remain for controlled manual use until a bounded replacement is explicitly requested.

## Security Rules for Debug Automation

- The automation layer exists only under an explicit debug/headless command.
- No ordinary GUI or peer message can enable it.
- Prefer inherited local handles; do not listen on a LAN socket.
- Bound scenario bytes, frame bytes, queue depth, command count, strings, arrays, numbers, output paths, and processing per turn.
- Parse, authenticate where applicable, and validate before constructing an `EngineCommand`.
- Keep automation parsing, file access, formatting, and artifact hashing off the callback and packet scheduler.
- Do not expose a direct “set remote state” command; exercise normal coordinator/authority messages.
- Treat scenario files as untrusted local input and report errors at the adapter boundary.
- Never persist or echo session keys, invite secrets, proof nonces, or upload tokens.
- Stream large artifacts instead of reading complete archives into memory.
- Cap artifact bytes, files, duration, concurrency, and retained disk use.
- Make debug-protocol version and all capacity limits visible in the effective run manifest.

## Performance Rules

- Test-source generation remains fixed-shape and allocation-free inside the callback.
- Frame scheduling uses prevalidated bounded commands and fixed-capacity handoff storage.
- Scenario parsing and Python communication never run in the callback.
- Structured snapshot formatting and CSV/file work remain on non-real-time workers.
- Debug mode reports its own command/event queue occupancy and overflow.
- The automation channel must not change packet cadence when idle.
- Benchmark the same engine with and without automation events to expose supervisory overhead.

## Refactor Phase Integration

### Phase 0: baseline and contract

- Preserve the current Python stress and benchmark results as legacy evidence.
- Record exactly which cases use real devices, headless audio, a proxy, or a real network.
- Stop storing session keys in routine benchmark state/artifacts.
- Define scenario schema v1, structured event/artifact schema v1, effective-configuration output, and non-secret run identity.
- Record missing deterministic seams: packet duplication, independent peer drift, reactive frame scheduling, and impaired mesh edges.
- Add native CTest foundations without removing current process tools.

Gate: existing tools produce a traceable baseline, and the successor automation contract is documented before engine interfaces are frozen.

### Phase 1: correctness and lightweight bounds

- Apply bounded parsing, secret scrubbing, artifact size/time limits, and streamed upload behavior to retained tooling where it is exposed during the refactor.
- Keep benchmark/automation credentials separate from Jam2 session secrets.
- Preserve raw rejection/capacity data without per-packet logging.

Gate: retained tools cannot silently persist session keys or accept unbounded frames/artifacts.

### Phase 2: wire-compatible fast-path changes

- Keep the scenario, signal, device, topology, seed, and artifact schemas fixed while measuring each packet/ring optimization.
- Use the same Python runner to compare each isolated change with Phase 0 evidence.
- Preserve native debug instrumentation for packet rate, bytes, CPU, allocations where available, copied bytes, scheduler gaps, queue depth, and rejection counts.
- Do not change the automation protocol as a side effect of a UDP fast-path implementation change.

Gate: every accepted optimization has traceable before/after artifacts from an otherwise identical scenario.

### Phase 3: engine extraction

- Put effective configuration, native test sources, fixed-shape snapshots, bounded commands/events, recording, and technical stats in the shared `Engine`.
- Add the frame-clock scheduler seam required by deterministic scenarios.
- Keep scenario JSON and Python types outside the engine.

Gate: legacy CLI and a thin test adapter can drive the same extracted engine without a second audio implementation.

### Phase 4: one-peer parity

- Run legacy and replacement one-peer paths through identical Python scenarios.
- Preserve directional tone/pulse, metronome, impairment, device, and profile comparisons.
- Emit normalized per-peer evidence even when only one remote peer exists.

Gate: Python can compare legacy and replacement artifacts without topology-specific metric code hiding differences.

### Phase 5: universal full mesh

- Generalize orchestration from server/client to arbitrary peer IDs.
- Add clean two/three/four-peer cases and independently impaired edges.
- Add deterministic per-peer headless drift and late-join/restart cases.
- Retain real-device and real-network mesh cases where machines are available.

Gate: the same scenario/result model covers one or multiple remote peers and exposes every edge independently.

### Phase 6: timing and authority

- Schedule metronome/grid/transport actions through frame/event triggers.
- Exercise creator, first joiner, third peer, and fourth peer as grid initiator.
- Ensure automation submits normal local requests rather than injecting accepted remote state.
- Record requested/applied frames, authority peer, revision, mapping error, and recovery.

Gate: timing tests are reproducible and prove the real authority path rather than a debug bypass.

### Phase 7: GUI lifecycle

- Reuse the same engine commands/events as debug/headless operation.
- Optionally add explicit local diagnostic-bundle creation without making the GUI a benchmark coordinator.
- Verify GUI activity does not change debug/headless engine behavior beyond measurable UI load.

Gate: GUI and headless adapters are thin clients of the same engine contract.

### Phase 8: single executable

- Ship one `jam2` executable with GUI default, ordinary headless subcommands, `debug run`, and capability/schema description.
- Implement bounded scenario ingestion and the inherited local reactive channel.
- Remove child-engine key arguments while retaining Python process orchestration.
- Preserve headless operation on Windows and the callable executable inside the macOS bundle.

Gate: local stress and multi-host Python runners use the unified executable and the same engine as the GUI.

### Phase 9: migrate and retire compatibility

- Move local stress, benchmark coordinator/agents, connection diagnostics, profiles, metrics, and artifact collection to the new contract.
- Compare migrated results with Phase 0 legacy evidence.
- Remove stdout scraping, newest-file discovery, unbounded text stdin, legacy mode loops, and obsolete local control only after replacement acceptance.
- Keep the versioned scenario and local automation contracts as supported technical interfaces.

Gate: no supported tool depends on a retired engine loop or compatibility protocol, and robust Python automation remains available.

### Phase 10: optional wire experiments

Python continues to provide the comparison matrix and raw artifact correlation for optional PCM/header/control experiments. Debug automation does not justify or require a production wire-format change.

## Acceptance Checklist

- `[ ]` Python stress and benchmark tooling remains supported after binary consolidation.
- `[ ]` GUI and debug/headless runs instantiate the same `Engine` and `NetworkSession` implementations.
- `[ ]` Local, create, join, and static full-mesh scenarios are automatable.
- `[ ]` Supported lifecycle, metronome/grid, prepared-track, recording, track-take, gain/mute, tuning, asset, stats, error, and recovery paths are reachable through normal typed application actions.
- `[ ]` Declarative scenarios are versioned, bounded, and contain no session secrets.
- `[ ]` Reactive automation is local-only, opt-in, bounded, and inactive during ordinary GUI startup.
- `[ ]` Debug commands cannot bypass authentication, source authorization, endpoint proof, grid authority, or transport revision rules.
- `[ ]` Real-device test input remains inside the ASIO/CoreAudio callback path.
- `[ ]` Headless tests are labeled and do not claim hardware coverage.
- `[ ]` Frame-sensitive actions use engine/grid scheduling rather than Python sleep timing.
- `[ ]` Native profiles/effective configuration are the source of truth.
- `[ ]` Two-, three-, and four-peer runs use one normalized peer/result model.
- `[ ]` Per-edge impairment and independent drift cases are explicit and reproducible.
- `[ ]` Raw CSV, recordings, manifests, and repeat-level results remain available.
- `[ ]` Results report hard measurements and deltas without subjective scores or hidden recommendations.
- `[ ]` Session secrets are absent from arguments, scenarios, logs, CSV, metadata, archives, uploads, and errors.
- `[ ]` Artifact transfer is bounded and streamed outside real-time work.
- `[ ]` Legacy automation paths are removed only after accepted replacement comparisons.

## Decision Log

| Date | Decision | Reason | Consequence |
| --- | --- | --- | --- |
| 2026-07-13 | Retain Python tooling after single-binary consolidation | It provides flexible process/machine orchestration, impairment, repeated experiment matrices, artifact movement, and offline analysis without entering the audio callback | Python migrates to a stable debug/headless adapter instead of being removed |
| 2026-07-13 | Use the same native engine for GUI, direct headless commands, and debug scenarios | A separate benchmark engine could pass while production behavior regresses | Debug mode changes configuration/control only, never audio/network implementation |
| 2026-07-13 | Use declarative scenario files plus an optional inherited reactive channel | Startup files are reproducible, while recovery tests need event-driven control | Static cases need no control service; reactive cases remain local, bounded, and opt-in |
| 2026-07-13 | Keep secrets out of persisted scenarios and process arguments | Benchmark artifacts and process inspection must not disclose the Jam2 session key | Secrets use an inherited in-memory handoff and artifacts use non-secret run IDs |
| 2026-07-13 | Keep impairment and offline analysis in Python | They are test/cold-path responsibilities and benefit from rapid iteration | Production audio stays direct and small; proxy overhead remains measurable |
| 2026-07-13 | Schedule timing-sensitive actions using engine frames/events | Python wall-clock sleeps add supervisory jitter and cannot prove exact application time | Engine reports requested/applied timing while Python handles failure timeouts only |

## Recommended First Tooling Work

Do not begin by rewriting all scripts.

1. Capture the current `listen/connect` and mesh artifact shapes and identify secret exposure.
2. Define the v1 scenario, event, effective-configuration, and artifact-manifest schemas.
3. Add non-secret run/attempt/peer identity to current artifacts.
4. Make the native executable report effective profile/configuration so Python stops treating duplicated defaults as authoritative.
5. Add the bounded frame-scheduler/test-source seams to the extracted engine when Phase 3 begins.
6. Implement the unified executable debug adapter in Phase 8.
7. Migrate one clean two-peer scenario end to end and compare it with the Phase 0 baseline.
8. Migrate impairment and metronome scenarios.
9. Generalize the runner/result model to three/four peers.
10. Remove legacy scraping/control paths only after the migrated suites pass.
