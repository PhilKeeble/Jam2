# Jam2 Python Tooling and Headless Automation Refactor

## Purpose

This document defines how Jam2 keeps its Python benchmark, stress, impairment, coordination, artifact, and offline-analysis tooling while the GUI and audio engine are consolidated into one public executable.

It is a focused companion to:

- [refactor-plan.md](refactor-plan.md), the dependency-ordered worker plan.
- [refactor-modes.md](refactor-modes.md), the local/network and full-mesh design.
- [refactor-binaries.md](refactor-binaries.md), the single-application review.
- [refactor-efficiency.md](refactor-efficiency.md), the code and fast-path audit.
- [refactor-security.md](refactor-security.md), the lightweight security contract.

This is supporting design context. Implementation order and completion status
are defined only by [refactor-plan.md](refactor-plan.md).

Sections describing current or legacy tooling record the migration baseline.
They do not require preservation of legacy command aliases or text-control
interfaces.

## Decision Summary

Keep Python as the external test orchestrator and offline analysis layer.

The unified `jam2` executable must retain an explicit headless/debug automation
surface over the same shared session controller, `Engine`, and `NetworkSession`
used by the GUI. Startup-static and scheduled cases should be expressible
through a strictly validated scenario file ingested at startup. Tests that must
react to runtime state use a bounded, unversioned, inherited local pipe rather
than stdin or an always-listening network service.

This is a minimal replacement seam, not a second application API. Add a
scenario field, command, event, or artifact only when a retained automated case
needs it to replace an existing stdin command, Python wall-clock action,
duplicated native default, output scrape, or artifact-discovery step. Simple
cases continue to launch the ordinary public headless commands directly.

The resulting ownership split is:

| Owner | Responsibilities |
| --- | --- |
| Shared session controller, `Engine`, and `NetworkSession` | Shared GUI/headless create/join lifecycle, TCP bootstrap and membership, real/headless audio execution, deterministic test input, frame-accurate scheduling, recording, raw per-peer statistics, effective configuration, bounded events and snapshots |
| Unified executable debug/headless adapter | Minimal retained-case scenario parsing, conversion to existing typed application requests, structured event/artifact output needed by those cases, and optional bounded inherited-pipe transport for reactive cases |
| Python tooling | Process and machine orchestration, peer topology, impairment, retries, experiment matrices, artifact collection, repeated comparisons, offline WAV/CSV analysis |
| GUI | Normal user workflow and an optional thin diagnostic-bundle action; not a benchmark coordinator |

The local automation formats deliberately have no compatibility-version
sequence. Their identifiers are `jam2-debug-scenario`,
`jam2-debug-description`, and `jam2-automation`; Phase 10 replaces the
temporary `*-v1` argv-wrapper shape and supports only the current documented
shape. This does not remove version negotiation from the network-facing
control and UDP protocols, where independently running peers need it.

Automation output is likewise opt-in. A `debug run` process emits a native
process manifest, and `jam2_test.py` emits a Python invocation manifest for the
test command it owns. GUI launches and direct ordinary CLI use do not gain a
manifest writer or any new lifecycle behavior.

## Goals

- Preserve robust local stress and the complete two-host benchmark workflow
  after binary consolidation. Keep three/four-peer coverage in the shared
  stress/result model without requiring multi-host benchmark coordination.
- Exercise exactly the same engine and network implementations used by ordinary GUI sessions.
- Support local-only, create-jam, join-jam, and multi-peer full-mesh automation through the same public bootstrap.
- Preserve both real-device ASIO/CoreAudio tests and headless synthetic tests.
- Make scenarios reproducible through bounded declarative inputs and explicit numeric settings without maintaining legacy local schema versions.
- Keep the native automation schema no larger than the retained cases require;
  expand it only alongside a concrete case that cannot use an ordinary command.
- Schedule timing-sensitive actions against the engine frame clock rather than Python wall-clock sleeps.
- Produce raw artifacts that can be correlated across every peer in a mesh.
- Keep Python and debug-control work outside the real-time callback and packet fast path.
- Keep local automation simple and inspectable. Session keys and invite URLs may
  appear in local command lines, scenarios, logs, CSV, benchmark metadata, and
  artifacts because those surfaces are outside the application threat boundary.
- Retain raw measurements and explicit deltas rather than generating subjective playability scores or hidden recommendations.

## Non-Goals

- Do not move the benchmark case matrix or offline audio analysis into the real-time engine.
- Do not add a production room, relay, TURN, account, telemetry, or cloud benchmark service.
- Do not add a remotely exposed automation server to ordinary application startup.
- Do not mirror every GUI control, snapshot field, or internal method in the
  debug schema.
- Do not add arbitrary argv forwarding, raw state setters, test-only engine
  mutations, or operations that bypass normal session authority and peer
  control.
- Do not make Python part of the live audio callback or production packet-processing path.
- Do not create a separate benchmark audio engine whose behavior can diverge from the GUI engine.
- Do not make debug scenarios a bypass around peer authentication, source authorization, grid authority, or endpoint proof.
- Do not automatically choose a “best” tuning profile when raw measurements and deltas are sufficient.

## What the Current Tools Actually Exercise

The current Python layer is not one subsystem. It performs several separate jobs:

| Current area | Current role |
| --- | --- |
| [tools/jam2_test.py](tools/jam2_test.py) and [tools/jam2test/dispatch.py](tools/jam2test/dispatch.py) | Keep argument parsing in the thin entry script and route the `validate`, `stress`, `benchmark`, `connectivity`, and Phase 12 `fuzz` command families through package-owned invocation setup |
| [tools/jam2test/native.py](tools/jam2test/native.py) and [validation.py](tools/jam2test/validation.py) | Consume the native debug description, run static/reactive scenarios, and own the post-build validation suite |
| [tools/jam2test/stress.py](tools/jam2test/stress.py), [scenarios.py](tools/jam2test/scenarios.py), and [impairment.py](tools/jam2test/impairment.py) | Launch retained local/mesh cases, apply deterministic impairment, schedule typed native actions, and write verdicts/results |
| [tools/jam2test/benchmark.py](tools/jam2test/benchmark.py), [benchmark_control.py](tools/jam2test/benchmark_control.py), and [benchmark_suite.py](tools/jam2test/benchmark_suite.py) | Coordinate two machines, retain the comprehensive matrices, retry/correlate attempts, and stream bounded artifacts |
| [tools/jam2test/profiles.py](tools/jam2test/profiles.py) | Represents experiments as a native named base profile plus sparse overrides |
| [tools/jam2test/metrics.py](tools/jam2test/metrics.py), [results.py](tools/jam2test/results.py), and [audio_analysis.py](tools/jam2test/audio_analysis.py) | Reduce raw CSV and perform offline stem, tone, pulse, clipping, pop, and metronome analysis |
| [tools/jam2test/connectivity.py](tools/jam2test/connectivity.py) | Diagnoses STUN mapping and direct UDP reachability independently of the benchmark coordinator |
| [tools/upload_server.py](tools/upload_server.py) | Provides a temporary manual LAN log collector |

With a real audio device, Jam2 currently creates test tone/pulse samples inside the ASIO or CoreAudio callback. Python selects the mode but does not synthesize or forward those live samples. The generated input then follows the normal capture ring, packet, receive, playback, and recording paths. This is valuable and must remain true after engine extraction.

The current two-host benchmark control connection carries lifecycle messages and artifacts, not live audio. The Jam2 audio path remains direct UDP between the Jam2 processes.

## Pre-Phase 10 Limitations Preserved as Migration Baseline

The following sections describe the legacy baseline that Phase 10 migrated.
They remain here to explain the replacement requirements and parity decisions;
they are not descriptions of live wrapper files.

### The tooling is coupled to legacy command names and output text

The harness launches `listen`, `connect`, or `mesh`, rewrites the current invite URL, scrapes startup JSONL/stdout, sends text commands over stdin, and finds the newest CSV in an output directory.

These are historical compatibility mechanisms, not the desired final
automation contract. They are removed rather than retained as aliases once the
unified debug/session interfaces cover their useful workflows.

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

The current benchmark control state owns one active remote peer, which is the
required two-host benchmark topology, but it also bakes `server` and `client`
roles into control state, retries, artifacts, and aggregation. Phase 10 must
normalize machine, peer, suite, run, and attempt identities without weakening
that working two-host state machine. Supporting arbitrary three-/four-machine
benchmark coordination may remain a later extension rather than a core-refactor
gate.

### Native and Python profile definitions can drift

The `fast`, `moderate`, and `safe` values exist in native tuning-profile code and are repeated in Python. Python also sends the named profile plus many explicit overrides. This can hide native default changes and produce metadata that describes Python's expectation rather than the effective runtime configuration.

The native application must become the source of truth and emit the validated effective configuration for every run.

### Result aggregation is server/client shaped

The current primary metric reducer combines `server` and `client`. Mesh support produces a smaller aggregate. The universal network engine requires normalized local-peer and remote-peer identities so one result model works for one, two, three, or more remote peers.

### Some planned baseline conditions do not have a deterministic seam

The current Python proxy does not duplicate packets, and the current headless flow does not inject independent positive/negative device-clock drift per peer. Those baseline cases must be added through explicit test facilities or recorded as unavailable in the legacy baseline.

### Current artifact handling can perform unbounded cold-path work

The current benchmark upload reads complete archives into memory, and the
standalone upload server has no authentication or hard request-size limit.
Persisting invite URLs or session keys locally is permitted and is not treated
as an application vulnerability.

The retained automation contract still needs cold-path size, time,
concurrency, and cleanup bounds. Those protections do not require audio-path
cost.

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
            |       +-- inherited framed automation pipe
            |       +-- structured events and artifact manifest
            |       +-- typed session request conversion
            |
            +-- shared non-UI session controller
                    +-- TCP create/join bootstrap, membership, authorities
                    +-- persistent Engine
                    |       +-- real ASIO/CoreAudio or headless device
                    |       +-- deterministic native test source
                    |       +-- frame-clock command scheduler
                    |       +-- recording and raw snapshots
                    +-- optional universal NetworkSession
                            +-- one independent PeerStream per peer
                            +-- direct full-mesh UDP
```

There is one application-session, engine, and network implementation. “Debug”
changes how it is configured and controlled, not how audio, timing,
authorization, or network state is processed.

## Unified Executable Surface

The public shape preserves ordinary headless commands and one explicit
scenario-driven debug entry point:

```text
jam2
jam2 local [options]
jam2 network create [options]
jam2 network join <invite> [options]
jam2 debug run <scenario.json>
jam2 debug describe --json
```

- No arguments launch the GUI.
- `local`, `network create`, and `network join` remain usable directly by technical users.
- Ordinary headless commands focus on core audio/network startup and shutdown;
  tests use `debug run` only when they need declarative configuration,
  engine-frame scheduling, structured results, or event-driven control.
- The retained metronome, grid, transport, recording, and recovery cases move
  their existing stdin/wall-clock actions to typed scheduled scenario actions
  or the optional reactive channel; this does not require exposing every GUI
  action.
- `debug run` is opt-in and is never activated by a received peer message.
- `debug describe` reports the supported unversioned local format identifiers,
  operations, commands, test inputs, profiles, limits, network protocol
  versions, and output shape.
- All commands instantiate the same `Engine` and, when requested, the same `NetworkSession`.

The Python tools may call the direct headless commands for simple cases or generate a scenario file for deterministic scheduled cases.

## Scenario File Contract

### Purpose

A scenario file describes reproducible startup configuration and scheduled
actions. It may contain local invite or session-key material. It replaces long
fragile command lines and output scraping without becoming a second application
configuration system.

The contract covers only fields used by retained automation cases. It reuses
native runtime option/configuration types and validators instead of defining
parallel defaults or validation. An ordinary CLI invocation remains preferable
when a case needs only startup and process exit.

JSON is suitable because this is bounded startup/debug input outside the fast path and Python already produces it. A fixed schema and hard size/count/string/numeric limits matter more here than a binary encoding.

### Illustrative shape

The `jam2-debug-scenario` identifier is exact. The illustrative payload field
names below may be smaller in the implementation. A field is admitted only
with a named retained case that consumes it:

```json
{
  "schema": "jam2-debug-scenario",
  "run_id": "run-id",
  "operation": "network.create",
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
    "session_key": "optional-local-test-key"
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
- `network.create`: create a real network session and publish its invite as a
  declared structured result; request the reactive channel only when another
  process must receive it during the run.
- `network.join`: receive an invite from validated local scenario input or the
  explicitly requested reactive channel and join through the normal
  authenticated bootstrap path.

Retain the focused `validate.boundaries` and
`validate.controller-lifecycle` executable operations. They use the same native
validators/controllers and must appear consistently in the accepted scenario
schema, dispatch, and `debug describe` operation list. The three runtime
operations above are the minimum application-session operations, not an
exclusive list that deletes focused validators.

Deterministic local and multi-process stress still uses the public create/join bootstrap. A narrowly scoped inherited debug seam may override the candidate seen by each observer so impairment proxies can sit on individual UDP edges; it must not create a different receive, timing, mixing, authority, or membership implementation.

### Minimum retained-case coverage

The initial declarative and reactive contracts cover only automation already
retained in the repository that cannot remain reliable through an ordinary
startup command:

- Local, create, and join startup using the same real-device/headless audio,
  native profile/tuning, deterministic test-input, and session validators as
  ordinary commands.
- The bounded typed equivalents of current stdin actions used by retained
  metronome/grid, level, prepared-track play/stop/restart/sync, recording, and
  orderly-shutdown cases.
- Engine-frame or shared-grid scheduling for the current timing-sensitive
  actions, with requested/applied frame and rejection reporting.
- Lifecycle, invite, peer-active/disconnected, command completion, effective
  configuration, reason-specific error, and artifact-path events only where an
  existing controller currently waits for or scrapes that information.
- Explicit local fixtures and output roots needed by retained WAV, asset,
  recording, capture, and boundary cases, with the existing native validation
  and normal bounded worker/control paths.
- Final raw snapshots, CSV/recording paths, and hashes required by existing
  verdict and artifact-collection code; the schema does not mirror the whole
  application snapshot.

Python continues to own process termination/restart, topology, impairment,
timeouts, experiment matrices, and offline analysis. A new native field or
message requires a named retained case, an existing typed application request
or event path, and a reason that ordinary CLI startup plus structured final
output is insufficient.

Local fixtures are selected by the local scenario adapter and validated before
use. A scenario must not create a remote path-selection bypass or allow a peer
to name an arbitrary local file.

### Validation rules

- Require exactly the recognized unversioned schema identifier and reject the
  obsolete `*-v1` wrapper and every unknown identifier.
- Apply a small hard file-size limit before parsing.
- Bound every string, array, numeric value, scheduled command count, and output-path length.
- Reject unknown operation and command names.
- Validate all engine/tuning values through the same native configuration validator used by GUI/headless options.
- Resolve local output paths once, outside the callback, beneath the caller-selected output root.
- Reject contradictory settings before opening the audio device or UDP socket.
- Emit the effective validated configuration; never treat the requested file as proof of what ran.
- Treat scenario files and their local session material as ordinary caller-owned
  test artifacts. Network proof nonces remain generated by the application and
  are not scenario inputs.

## Reactive Local Automation Channel

Scenario files cover static startup and commands known in advance. Stress and recovery tests also need to react to events such as peer activation, disconnect, authority changes, queue saturation, or recording completion.

The unified executable should therefore offer a small local automation channel
only when a retained reactive case launches `debug run` with that channel
explicitly enabled. The parent controller creates the handles before launch.
Static `debug run`, `debug describe`, GUI startup, and ordinary `local` or
`network create/join` commands create and inherit no automation handles.

### Transport requirements

- Prefer a pair of anonymous inherited pipes: one bounded command stream into
  Jam2 and one bounded event stream back to the controller. Pass only the child
  ends and reject automation-handle configuration outside an explicitly
  reactive `debug run`.
- Do not use stdin/stdout for the reactive protocol, open a listening socket,
  or recreate the removed CLI stdin command loop.
- If platform constraints make inherited anonymous handles impractical, use
  only an OS-local IPC mechanism with caller ownership and an unpredictable
  per-run name; do not expose it on the LAN.
- Use bounded, length-prefixed frames identified as `jam2-automation` rather
  than unbounded newline accumulation or a local compatibility-version stack.
- Cap input/output frame size, queued frames, frames processed per turn, and incomplete-frame time.
- Separate structured protocol output from human stderr diagnostics.
- Close the channel and stop or continue according to explicit scenario policy when the controller disappears.

JSON payloads remain acceptable for this cold, local, human-rate control layer. The adapter must parse and validate them outside the engine and convert them into bounded typed `EngineCommand` values.

### Minimum message families

- Hello/capability publication and exact local-format identification.
- Invite delivery needed to connect retained create/join processes.
- Typed actions currently sent through stdin by retained cases, plus orderly
  shutdown. Python continues to own timeout/process cancellation; no separate
  native cancel message is admitted without a retained case that needs it.
- Lifecycle, peer-state, command-completion, and reason-specific error events on
  which retained controllers must react.
- Effective configuration and artifact/result publication needed to remove
  current output scraping and newest-file discovery.

Do not add a message family merely because an equivalent GUI action or snapshot
field exists. Static startup configuration stays in the scenario, and Python
keeps orchestration operations that do not require a native application action.

Automation commands that are intended to simulate GUI user actions must enter the same local command/request path as the GUI. They must not directly mutate remote-authority state or bypass normal peer control.

## Local Session Material

Local process arguments, scenarios, logs, clipboard contents, benchmark state,
and artifacts are outside Jam2's application threat boundary. Automation may
store and pass session keys or complete invite URLs through those surfaces when
that keeps create/join orchestration simple and inspectable. Network
challenge-response, authenticated framing, source authorization, and endpoint
proof remain mandatory; local visibility of a key does not weaken those remote
protocol requirements.

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
- Debug scenario, description, and automation format identifiers.
- Available test sources and command families.
- Active device/backend/rate/buffer/channel configuration.
- Control/UDP protocol versions and the current native security limits.

Python may define experimental override matrices, but it must not duplicate the native defaults as authoritative values.

## Python Architecture

The existing scripts should migrate incrementally into a small, dependency-free
repository-local framework. This is a bounded ownership refactor, not a rewrite
of the working scenario, impairment, analysis, benchmark-control, upload, or
result logic. It is justified because the current flat launchers mix argument
parsing, orchestration, validation, case definition, and artifact handling in
several large files.

### Unified Python command surface

Use one thin entrypoint with explicit subcommands rather than a generic flavour
argument:

```text
python tools/jam2_test.py validate [all|framework|product]
python tools/jam2_test.py stress [options]
python tools/jam2_test.py benchmark coordinator [options]
python tools/jam2_test.py benchmark agent [options]
python tools/jam2_test.py benchmark analyze <results>
python tools/jam2_test.py connectivity stun|direct [options]
python tools/jam2_test.py fuzz [all|control|udp|asset|wav] [options]
```

The entrypoint parses and dispatches only. A `tools/jam2test` package owns
capability discovery, process execution, artifacts, and separate validation,
stress, benchmark, connectivity, fuzzing, analysis, and network/impairment
modules.
Python command modules accept explicit configuration and return structured
results; they do not parse `sys.argv` or terminate the interpreter themselves.
Keep the package importable directly from the repository without installation
or new third-party dependencies.

Command responsibilities are deliberately different:

- Bare `validate` defaults to `all`: run the fast deterministic framework
  self-tests first and then the complete headless product-validation suite as
  the normal post-build baseline. `framework` and `product` remain selectable,
  and explicit device selections extend rather than silently replace headless
  coverage. The suite distinguishes pass, failed behavior, infrastructure
  error, and device/manual-only coverage that was not requested.
- Validation maps every public CLI option, supported debug operation/protocol
  field, and representative native-validator boundary to an automated case or
  an explicit device/manual-only classification. It checks parsing,
  representative valid and invalid boundaries, propagation into emitted
  effective configuration, clean local and multi-peer create/join workflows,
  scheduled controls, and artifact contracts. It does not exhaustively combine
  every numeric control, automate every GUI feature, or imply a public `mesh`
  command.
- `stress` owns targeted feature regression and improvement scenarios under
  controlled impairment, clock, lifecycle, and mesh conditions. It asserts the
  named adaptive/recovery/correctness behavior and retains raw evidence; it is
  neither the clean post-build baseline nor a comparative benchmark.
- `benchmark` owns repeatable measurement matrices and raw comparisons. A case
  may be invalid when it failed to establish or produce its required evidence,
  but network/audio quality is not converted into a release gate or subjective
  score.
- `connectivity` owns standalone STUN mapping and direct UDP reachability
  diagnostics for users. It remains usable without starting a benchmark or a
  Jam2 audio session.
- `fuzz` owns bounded mutation/generation, retained corpus replay, native
  parser/real-process execution, stable failure classification and minimization,
  and reproducible artifacts for the authenticated control, UDP, binary asset,
  and WAV surfaces. It is a test-only family, not a remotely reachable runtime
  service, and it distinguishes expected native rejection from crashes, hangs,
  resource-bound violations, or invariant failures.

Existing public scripts may be thin compatibility wrappers only during
migration. Remove them after command, case, result, and artifact parity is
demonstrated; do not retain permanent aliases. The temporary standalone upload
server remains separate unless a bounded replacement is explicitly requested.

Phase 10 migrates the retained cases that exist when the phase starts. The
additional late-join/restart/endpoint-migration, mixed-device, and adversarial
real-process cases assigned to Phase 11 are target coverage for the completed
tooling, not extra Phase 10 gates.

### Artifact roots and safe cleanup

Each command family has its own default root:

```text
tools/validate_logs/
tools/stress_logs/
tools/benchmark_logs/
tools/connectivity_logs/
tools/fuzz_logs/
```

Every invocation creates a new collision-resistant
`<UTC timestamp>_<run ID>` directory beneath its family root. An explicit
output-root option changes the parent location, not this family/run isolation.
The runner creates the final directory atomically and selects a new run ID on a
collision; it never reuses or chooses the newest directory implicitly.

Benchmark layout is suite-aware. The coordinator creates and distributes the
invocation and suite IDs, and both machines use:

```text
tools/benchmark_logs/<invocation>/suites/<suite-id>/machines/<machine-id>/
    cases/<case-id>/runs/<run-id>/attempts/<attempt-id>/...
```

Controller/agent logs, Jam2 stdout/stderr, native manifests, transfer logs,
results, WAV, and CSV use stable named paths within that tree. Uploaded
artifacts land in the matching coordinator machine/attempt subtree only after
their envelope identity is validated. The coordinator retains its own local
subtree and the uploaded agent subtree. The agent retains its local subtree
until the coordinator acknowledges the validated upload. Reconnects, retries,
and repeated cases never overwrite a prior attempt.

Default runs never delete earlier output. `--clean` intentionally removes all
old results beneath the selected command-family root before allocating the new
unique run directory. Thus `stress --clean` clears `stress_logs` but cannot
touch `benchmark_logs`, `validate_logs`, `connectivity_logs`, or `fuzz_logs`;
each other command, including `fuzz`, behaves equivalently within its own
family.

Top-level `--clean` means only this safe pre-run family cleanup. The current
benchmark-agent option that deletes local artifacts after a successful upload
must be renamed `--delete-after-upload`; it may delete only after the
coordinator acknowledges and correlates the validated upload.

An explicit output option selects a parent directory, not a shared destination:
the framework still creates the separate family folder beneath it. Before
recursive cleanup, resolve the canonical family path and verify that it is the
expected direct child of that parent. Refuse a path that resolves to the output
parent itself, the repository root, or outside the selected family folder, and
do not follow a symlink/reparse escape. Recreate only the selected family root
and then create the new run directory.

Output parents have a 2048-character framework bound. Before a live Windows
benchmark starts, also project the normalized native attempt path and reject an
output root that would leave insufficient room beneath the legacy native
file-path budget; the error tells the operator to choose a shorter `--output`
root instead of retrying cases that cannot create WAV/CSV files.

Temporary legacy wrappers may translate their old `--logs` arguments into the
new parent/family/run model, but must obey the same family isolation rather than
deleting the common parent supplied by the caller.

### Scenario catalog

- Defines case names, one-variable tuning variants, signals, durations, repeats, and expected hard invariants.
- Produces bounded `jam2-debug-scenario` JSON in the one currently supported
  local format.
- Stores explicit impairment parameters and seeds.
- May contain local session keys or invite URLs where convenient.
- Does not copy native profile defaults; it requests a native profile name and
  sparse explicit overrides. Benchmark catalogs may retain many experimental
  combinations across frame size, buffering, jitter, adaptation, drift,
  sockets, metronome, priority, signal direction, and other supported tuning
  controls.

### Local controller

- Launches ordinary `jam2 local`/`network create`/`network join` processes for
  simple cases and `jam2 debug run` only for cases requiring declarative
  scheduling, structured results, or reactive control.
- Selects real devices or headless audio independently per peer.
- Creates local/create/join topology inputs over the universal mesh engine.
- Starts per-edge impairment where requested.
- Passes create/join session material through scenarios, arguments, or the
  inherited automation pipe only when a reactive scenario requests it.
- Waits on structured lifecycle events instead of scraping human text.
- Applies reactive commands and bounded timeouts.
- Collects one native process manifest per `debug run` peer. For an ordinary
  CLI peer, records arguments, exact stdout/stderr paths, process result, and
  collected files in the Python invocation manifest instead of imposing a
  native automation manifest on that command.

### Two-host benchmark coordinator and agent

The current robust two-host benchmark is required behavior, not a transitional
script to replace with a simpler fire-and-forget runner. Its server/client
naming should evolve toward one coordinator and one lightweight agent per
machine while retaining the working control, upload, and state-management
semantics:

- The coordinator owns suite, case, run, attempt, and peer/topology identity;
  selects cases, profiles, signals, durations, and repeats; and publishes an
  idempotent current state.
- The agent reconnects safely, accepts only the active attempt, launches its
  local Jam2 peer, retains artifacts until acknowledgement, retries bounded
  transfers, and never associates stale artifacts with a newer attempt.
- Case retry/exhaustion, completion acknowledgement, post-case grace, optional
  cleanup, and final `all_done` acknowledgement remain explicit and bounded.
- Artifact upload remains outside the audio path, uses bounded streaming rather
  than whole-archive memory reads, and correlates both machines' manifests,
  recordings, CSV, analysis, and raw diagnostics before aggregation.
- Every result records stable machine and peer IDs, actual endpoint/network
  metadata, requested base profile and sparse overrides, and the native emitted
  effective configuration from each side.
- The initial required workflow remains two hosts. Normalized identities and a
  reusable agent model prevent server/client-shaped results and permit later
  extension, but three- or four-host benchmarking is not a Phase 10 gate.

Build this by generalizing the current benchmark control and artifact code
rather than replacing its proven state machine or introducing a new external
dependency.

Preserving the comprehensive many-profile, many-repeat workflow is an
implementation requirement, but running that long matrix is not a Phase 10
closeout gate. Close Phase 10 with one short live two-host smoke: one native
base profile, one repeat, and two or three short representative cases that
finish in a few minutes. The subset must prove negotiation, create/join,
recorded WAV/CSV and native/Python manifests, bounded packaging and upload,
upload acknowledgement/result correlation, final `all_done`, and clear log
retention on both machines in the normalized tree. The comprehensive matrix
remains available for deliberate performance work.

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

This is target coverage across the remaining refactor. Phase 10 migrates the
cases already retained by the current tools; Phase 11 adds the missing
late-join/restart/endpoint-migration, mixed-device, and adversarial lifecycle
cases. The list must not be read as silently moving those Phase 11 additions
into the Phase 10 gate.

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

Artifacts are addressed by their manifest and exact run-relative path, never by
directory recency. The command-family roots and unique invocation directories
defined above are part of the contract: validation, stress, benchmark, and
connectivity output cannot overwrite one another, and consecutive runs of the
same family remain independently inspectable.

There are two owned manifest layers rather than one global application
manifest:

- Each `jam2 debug run` process publishes a native manifest. It is
  authoritative for source/build identity, executable/platform, local peer
  identity, recognized local automation format identifiers, control/UDP
  protocol versions, validated effective audio/profile/tuning/session
  configuration, test input, native lifecycle/end reason, and exact native
  artifact paths/hashes.
- Every `jam2_test.py validate|stress|benchmark|connectivity|fuzz` invocation
  publishes a Python invocation manifest. It is authoritative for command
  family, invocation/suite/case/run/attempt and machine/peer identities,
  topology, impairments and seeds, retries/transfers, process return state,
  omissions/classifications, analysis/results, and references to native
  manifests and collected artifacts when applicable.

Python records ordinary CLI launches and their exact arguments,
stdout/stderr, return state, and collected files in its invocation manifest.
If a case must prove native effective configuration, lifecycle publication, or
native artifact identity, it uses `debug run` and consumes that process's
native manifest. GUI and direct ordinary CLI users do not receive an automation
manifest side effect.

CSV/stat output must support:

- One local-engine row stream.
- One aggregate `NetworkSession` view.
- One normalized view per remote peer/edge.
- Stable peer identity independent of endpoint.
- Queue capacities, high-water marks, and drop/rejection counters.
- Callback, packet scheduler, per-peer clock/drift/playout, mix, metronome, transport, and lifecycle measurements required by [refactor-plan.md](refactor-plan.md).

Python should consume a debug process's published native manifest rather than
choose the newest file in a directory, and should link it from the Python
invocation manifest.

## Test Layers

The command families describe why a run exists, while the layers below describe
where it executes. Clean deterministic framework and product checks belong to
`validate`; controlled feature challenges and recovery assertions belong to
`stress`; cross-host tuning comparisons belong to `benchmark`; reachability
diagnosis belongs to `connectivity`; and bounded mutation/corpus execution
belongs to `fuzz`. Headless, real-device, local, and multi-host execution do not
blur those result contracts.

### Focused protocol and component validation

Use independent Python codecs, malformed-input generators, deterministic process
scenarios, emitted counters, and artifact analysis for packet framing, replay,
horizons, parsers, bounded queues, authority, and timing behavior. Exercise the
real Jam2 executable so a passing check demonstrates externally visible native
behavior rather than only a separately compiled test harness. Keep small
in-process Python checks for the independent generators themselves; do not add
CMake/CTest scaffolding.

### Bounded native fuzzing

Phase 12 adds `tools/jam2test/fuzz.py` and focused helpers behind the thin
dispatcher command:

```text
python tools/jam2_test.py fuzz [all|control|udp|asset|wav]
```

Python owns input generation, explicit seed/iteration/time/resource settings,
corpus selection, process isolation, result classification, minimization, and
artifacts. Native code remains authoritative for decoding, authentication,
state transitions, checked arithmetic, bounds, and rejection reasons. Use the
existing opt-in debug runner or a narrowly declared native debug operation;
never add an ordinary GUI handle, stdin mutation path, public network listener,
or Python parser whose success substitutes for exercising native code.

Every invocation creates `tools/fuzz_logs/<invocation-id>` and records the
target, master and derived seeds, corpus/input hashes, iteration counts, native
build and protocol identity, command/scenario, limits, elapsed time, exit/end
state, rejection/crash/hang/invariant classification, stable signature, and any
minimized reproduction. `--clean` removes only the canonical fuzz family root.
The implemented baseline caps individual inputs, native parser allocations by
target/input bounds, process lifetime, iterations, captured output, retained
failures, minimization work, and total invocation time. An OS-enforced memory
sandbox is optional future hardening rather than a Phase 12 requirement.

Targets are the final Phase 12 formats: length-prefixed authenticated control
and strict model validation; compact authenticated UDP for both PCM16 and
PCM24; binary asset framing/state; and the narrow PCM16 WAV parser. The retained
seed and mutation corpus covers valid inputs plus representative truncation,
coalescing, excess, authentication, arithmetic, replay, and malformed cases.
Expected bounded validation errors are normal outcomes. Preserve and minimize
crashes, hangs, resource-limit failures, inconsistent state/counters, or other
explicit native invariants without changing their signature. Broader
message-type/state-machine corpora and ASan/UBSan runs may be added later but
are not Phase 12 completion work. The resumed Phase 12 run relies on its already
retained successful bounded smoke and does not schedule another fuzz campaign.

### Headless deterministic integration

Use headless peers for repeatable lifecycle, packet, mesh, CPU, memory, bandwidth, mix, command, and deterministic clock tests. These runs do not claim real device/driver coverage.

### Real-device local stress

Use native callback test input with ASIO/CoreAudio devices and controlled localhost impairment. Compare the same case headless and real-device to isolate callback, driver, device buffer, OS priority, and hardware-clock effects.

For Phase 12, `stress --network-audio-format both` expands each selected native
case into matched PCM16 and PCM24 executions and writes
`format-comparison.json` and `format-comparison.csv` in the invocation root.
The pair is complete only when both formats passed with matching topology,
profile, duration, impairment, and flow, and each result declares the expected
native format, bytes/sample, packet/header/payload sizes, packet rate, bitrate,
and received audio. Ordinary case failures remain failures rather than being
hidden by comparison generation.

### Multi-host direct benchmark

Run one normal engine per real machine over the actual direct network, with the Python control plane used only for orchestration and artifact movement. These runs validate real network, hardware, and cross-machine clocks.

`benchmark coordinator|agent --network-audio-format both` uses the same paired
dimension and retains the full per-side manifests, CSV/WAV files, upload
acknowledgements, correlated results, and final `all_done` state. The coordinator
comparison adds non-silent WAV peak/RMS/pop/clipping data when observed; it does
not invent audio-analysis results for stress runs that did not record WAVs.
Phase 12's local short tone comparison is retained at
`tools/benchmark_logs/20260715T213324Z_99a9b776`, while the pre-change physical
reference remains at `tools/benchmark_logs/20260715T153015Z_4fd412e0`.

The required Phase 10 workflow is two hosts. Preserve its full case/repeat
state machine, reconnect/retry behavior, upload acknowledgement, stale-attempt
rejection, and correlated artifacts while changing server/client terminology to
coordinator/agent and requested profiles to native-base-plus-override matrices.
Validation at Phase 10 closeout uses only the short one-profile, one-repeat,
two-or-three-case live smoke defined above; it must not require the long
comprehensive benchmark matrix.

### Connectivity diagnosis

Keep STUN mapping stability and exchanged-token direct UDP probes available as
short standalone diagnostics. Report the observed local/public endpoints,
mapping consistency, probes sent/received, elapsed time, and explicit failure
reason. Do not require an audio device, benchmark agent, or running Jam2 session
and do not infer a subjective connection-quality score.

### Real-session diagnostic capture

An optional GUI action may package raw diagnostics from an ordinary jam. This supplements controlled tests with field evidence but is not a replacement for reproducible scenarios.

## Diagnostic Bundle and Upload Direction

The application may later expose a small explicit diagnostic-bundle action:

- Stop the real-time audio/network work before transferring a bundle.
- Include the effective configuration, raw CSV, errors, and manifest by default.
- Make recordings an explicit opt-in because of size and audio privacy.
- Make the bundle contents explicit before transfer; local session keys and
  invite URLs do not require redaction.
- Use bounded streaming, incremental hashing, size/time/concurrency limits, cancellation, and temporary-file cleanup.
- Never block leaving a jam indefinitely.

The GUI should not become a general upload server. Automated benchmark artifact collection remains a coordinator/agent responsibility. The temporary standalone Python uploader can remain for controlled manual use until a bounded replacement is explicitly requested.

## Security Rules for Debug Automation

- The automation layer exists only under an explicit debug/headless command.
- No ordinary GUI or peer message can enable it.
- Automation handles exist only when the parent explicitly creates them for a
  reactive `debug run`; reject them for every other command and reject partial
  command/event handle pairs.
- Prefer inherited local handles; do not listen on a LAN socket.
- Bound scenario bytes, frame bytes, queue depth, command count, strings, arrays, numbers, output paths, and processing per turn.
- Parse, authenticate where applicable, and validate before constructing an `EngineCommand`.
- Keep automation parsing, file access, formatting, and artifact hashing off the callback and packet scheduler.
- Do not expose a direct “set remote state” command; exercise normal coordinator/authority messages.
- Treat scenario files as untrusted local input and report errors at the adapter boundary.
- Do not expose a remote network mutation that bypasses normal authentication,
  authorization, or endpoint proof. Local session keys in automation data are
  permitted.
- Stream large artifacts instead of reading complete archives into memory.
- Cap artifact bytes, files, duration, concurrency, and retained disk use.
- Make the local automation format identifiers and all capacity limits visible
  in the native debug-run manifest.

## Performance Rules

- Test-source generation remains fixed-shape and allocation-free inside the callback.
- Frame scheduling uses prevalidated bounded commands and fixed-capacity handoff storage.
- Scenario parsing and Python communication never run in the callback.
- Structured snapshot formatting and CSV/file work remain on non-real-time workers.
- Debug mode reports its own command/event queue occupancy and overflow.
- The automation channel must not change packet cadence when idle.
- Benchmark the same engine with and without automation events to expose supervisory overhead.

## Relationship to the Authoritative Plan

This document defines the target automation behavior and explains why it is
needed. It does not assign phases, gates, or completion state. The remaining
implementation order, completed work, useful validation commands, and optional
wire experiments are maintained only in
[refactor-plan.md](refactor-plan.md).

## Target Capability Summary

- Python validation, stress, two-host benchmark, connectivity, and bounded
  fuzzing tooling
  remains supported after binary consolidation through one thin
  `jam2_test.py` command surface and separately owned modules.
- Bare validation is the deterministic headless post-build baseline, with
  explicit optional real-device extensions and visible manual/device-only gaps.
- Stress remains the controlled feature-regression and recovery surface;
  benchmark remains a many-profile measurement workflow rather than a quality
  gate; connectivity remains a standalone direct-path diagnostic.
- GUI and debug/headless runs instantiate the same shared session controller,
  `Engine`, and `NetworkSession` implementations.
- Local, create, join, and multi-peer full-mesh scenarios are automatable through the same public bootstrap.
- Supported lifecycle, metronome/grid, prepared-track, recording, track-take, gain/mute, tuning, asset, stats, error, and recovery paths are reachable through normal typed application actions.
- Declarative scenarios use one unversioned bounded local format; local session
  material is permitted.
- Reactive automation is local-only, opt-in, bounded, and inactive during ordinary GUI startup.
- Native automation manifests exist only for `debug run`; Python command
  manifests own orchestration evidence, and ordinary GUI/direct CLI behavior
  is unchanged.
- Debug commands cannot bypass authentication, source authorization, endpoint proof, grid authority, or transport revision rules.
- Real-device test input remains inside the ASIO/CoreAudio callback path.
- Headless tests are labeled and do not claim hardware coverage.
- Frame-sensitive actions use engine/grid scheduling rather than Python sleep timing.
- Native profiles/effective configuration are the source of truth.
- Two-, three-, and four-peer runs use one normalized peer/result model.
- Per-edge impairment and independent drift cases are explicit and reproducible.
- Raw CSV, recordings, manifests, and repeat-level results remain available.
- Results report hard measurements and deltas without subjective scores or hidden recommendations.
- Artifact transfer is bounded and streamed outside real-time work.
- Phase 10 closes with a few-minute live two-host smoke while the comprehensive
  multi-profile benchmark remains available for deliberate runs.
- Legacy scraping, text-control, and mode adapters are removed rather than retained as aliases.

## Decision Log

| Date | Decision | Reason | Consequence |
| --- | --- | --- | --- |
| 2026-07-13 | Retain Python tooling after single-binary consolidation | It provides flexible process/machine orchestration, impairment, repeated experiment matrices, artifact movement, and offline analysis without entering the audio callback | Python migrates to a stable debug/headless adapter instead of being removed |
| 2026-07-13 | Use the same native engine for GUI, direct headless commands, and debug scenarios | A separate benchmark engine could pass while production behavior regresses | Debug mode changes configuration/control only, never audio/network implementation |
| 2026-07-13 | Use declarative scenario files plus an optional inherited reactive channel | Startup files are reproducible, while recovery tests need event-driven control | Deterministic cases use the real create/join coordinator; reactive cases remain local, bounded, and opt-in |
| 2026-07-15 | Limit native automation to concrete retained-case needs | A full mirror of the GUI/application API would duplicate surfaces and expand maintenance/security cost without replacing a demonstrated tooling problem | Ordinary commands remain the default; each new schema field or message must replace a named stdin, timing, scraping, defaults, or artifact-discovery dependency |
| 2026-07-15 | Unify Python tooling behind validation, stress, benchmark, and connectivity commands | The current flat launchers obscure ownership and mix clean regression, impairment, measurement, and diagnosis while still containing valuable working logic | A thin `jam2_test.py` dispatcher uses focused modules; validation becomes the post-build baseline, existing behavior migrates incrementally, and temporary wrappers are removed after parity |
| 2026-07-15 | Add bounded `fuzz` as a fifth isolated command family after final Phase 12 wire/asset formats land | Mutation should exercise the native formats that will ship while remaining reproducible, resource-bounded, and absent from ordinary runtime surfaces | `jam2_test.py fuzz` uses focused package modules, native debug execution, retained/minimized corpora, manifests, and `tools/fuzz_logs/<invocation-id>` with family-local cleanup |
| 2026-07-15 | Preserve the robust two-host benchmark and many-profile matrices | Cross-host state, retries, uploads, and effective tuning evidence are core measurement capabilities rather than obsolete server/client details | Coordinator/agent naming and normalized identities replace role-shaped results, while native base profiles plus sparse overrides retain broad experiments without duplicated defaults or a Phase 10 multi-host requirement |
| 2026-07-15 | Keep local automation formats unversioned | The temporary local adapter has no external compatibility promise and only the desired current behavior needs support | Replace `*-v1` with the three stable unversioned identifiers; retain versioning only for network protocols between independent peers |
| 2026-07-15 | Keep manifests opt-in and split ownership | Ordinary GUI/direct CLI operation should not gain test-only output behavior, while native and orchestration facts have different authorities | `debug run` emits the native process manifest and `jam2_test.py` emits the command invocation manifest |
| 2026-07-15 | Use a short two-host smoke for Phase 10 closeout | The comprehensive benchmark takes too long for a migration proof | Preserve the full workflow, but close with one profile, one repeat, and two or three short cases that exercise upload and retain clear logs on both machines |
| 2026-07-14 | Treat local session-key exposure as outside the application boundary | A compromised host can already observe the process and audio; local automation should remain simple and inspectable | Keys and invites may appear in arguments, scenarios, logs, clipboard contents, benchmark state, and artifacts while network authentication remains mandatory |
| 2026-07-13 | Keep impairment and offline analysis in Python | They are test/cold-path responsibilities and benefit from rapid iteration | Production audio stays direct and small; proxy overhead remains measurable |
| 2026-07-13 | Schedule timing-sensitive actions using engine frames/events | Python wall-clock sleeps add supervisory jitter and cannot prove exact application time | Engine reports requested/applied timing while Python handles failure timeouts only |

## Remaining Tooling Context

- The debug scenario, event, effective-configuration, and native-manifest
  formats cover the minimum retained-case needs rather than arbitrary argv or
  the complete typed application surface.
- Native profile/configuration reporting replaces duplicated Python defaults.
- A native frame scheduler and deterministic test sources support timing cases
  without Python sleep timing.
- Local, impairment, metronome, lifecycle, two-/three-/four-peer, and
  multi-machine workflows share one normalized result model.
- Human-output scraping, newest-file discovery, legacy mode adapters, and the
  runtime stdin command loop have been removed now that structured events and
  manifests cover their useful information.
