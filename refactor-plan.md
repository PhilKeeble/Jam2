# Jam2 v1 Refactor Worker Plan

## Purpose

This is the working execution plan for the Jam2 v1 stabilization, efficiency, lightweight security, mode-consolidation, and single-application refactor.

It is intended to be handed to an implementation agent so the agent can:

- Understand the target architecture and non-negotiable behavior.
- Find the relevant source and supporting documentation.
- Work in dependency order rather than treating the refactor as one rewrite.
- Know what must be measured before and after each change.
- Stop at explicit parity and acceptance gates.
- Record what is implemented, what was validated, and what remains pending.

Creating this plan does not mark any implementation phase complete.

## Governing Documents

Read these before beginning or resuming implementation:

1. [AGENTS.md](AGENTS.md) contains the repository rules and takes precedence over this plan.
2. [refactor-efficiency.md](refactor-efficiency.md) contains the full code, protocol, data-flow, correctness, and quantified efficiency review.
3. [refactor-modes.md](refactor-modes.md) contains the local/network lifecycle, full-mesh, timing, and authority design.
4. [refactor-binaries.md](refactor-binaries.md) contains the GUI/engine process-boundary and single-application review.
5. [refactor-security.md](refactor-security.md) contains the network/control/asset/WAV threat review, lightweight security set, overhead estimates, and validation matrix.
6. [refactor-python.md](refactor-python.md) defines the retained Python orchestration, scenario-driven headless/debug automation, artifact, and tooling-migration contract.
7. [PLAN.md](PLAN.md) contains the broader product roadmap.

Supporting operational documentation:

- [docs/Engine.md](docs/Engine.md)
- [docs/Gui.md](docs/Gui.md)
- [docs/Mesh.md](docs/Mesh.md)
- [docs/Metronome.md](docs/Metronome.md)
- [docs/Profiles.md](docs/Profiles.md)
- [docs/TestCases.md](docs/TestCases.md)
- [docs/Diagnosing.md](docs/Diagnosing.md)
- [docs/ConnectionTest.md](docs/ConnectionTest.md)

Where the older binary review differs from the later reviews, use these later decisions:

- Headless and CLI operation remains a supported public interface inside the unified `jam2` executable; it is not only a private development harness.
- Python stress and benchmark orchestration remains supported through the bounded scenario/debug contract in [refactor-python.md](refactor-python.md); it is not removed with the second binary.
- The current mature `listen/connect` packet behavior is the parity baseline.
- The present experimental `mesh` loop is not the replacement engine. Its useful membership, fan-out, and per-peer-stat concepts are inputs to a generalized mature engine.
- The end state is one persistent local engine with an optional full-mesh network session.
- The v1 security target is bounded, authenticated, source-authorized direct sessions among controlled peers; it is not accounts, relays, certificate infrastructure, or encrypted media.
- Security limits and validation belong at the existing control, file-worker, `PeerStream`, and `NetworkSession` boundaries rather than in a separate service or the audio callback.

## Work Rules

### Repository and build discipline

- Inspect the current worktree before editing and preserve unrelated user changes.
- Do not compile or run build/test commands unless the user explicitly asks. This is required by [AGENTS.md](AGENTS.md).
- An implementation can be marked code-complete while build or runtime validation remains pending, but it cannot pass its phase gate until the requested results are available.
- Keep MSVC/ASIO as the Windows implementation and validation target. Keep Apple tooling/CoreAudio as the macOS target.
- Do not combine architectural, wire-format, and tuning changes in one measured step.
- Do not remove a compatibility path until its replacement has passed the stated gate.

### Status notation

Use these markers when maintaining this document:

- `[ ]` not started.
- `[~]` in progress.
- `[x]` implemented and validated.
- `[v]` implementation complete, validation pending.
- `[!]` blocked; add the exact reason and evidence in the work log.

Only change a phase to `[x]` after all exit-gate items are satisfied. If code is written but the user has not yet compiled or returned runtime results, use `[v]` on the applicable tasks and leave the phase incomplete.

### Change isolation

Each measured change should answer one clear question. In particular:

- Do not change the UDP header while extracting the engine.
- Do not change PCM depth while implementing full mesh.
- Do not replace the process boundary while proving one-peer packet parity.
- Do not retune buffer defaults to conceal a timing regression.
- Do not mix the current mesh loop into the mature path wholesale.
- Do not split files mechanically before ownership boundaries are understood.
- Do not combine TCP control-handshake/framing changes with a UDP audio wire-format change.
- Do not make an unverified advertised endpoint active merely because its TCP control connection authenticated.
- Do not keep an unsafe legacy framing/authentication path after the hardened peer control path is the accepted replacement.

## Current Status

Last plan update: 2026-07-13.

- `[ ]` Phase 0: baseline evidence and native test foundation.
- `[ ]` Phase 1: correctness, lightweight security, limits, and observability.
- `[ ]` Phase 2: wire-compatible fast-path primitives.
- `[ ]` Phase 3: persistent local engine extraction.
- `[ ]` Phase 4: mature one-peer `PeerStream` parity.
- `[ ]` Phase 5: universal `NetworkSession` and full mesh.
- `[ ]` Phase 6: timing, metronome, and transport authority.
- `[ ]` Phase 7: GUI local/network lifecycle.
- `[ ]` Phase 8: single-binary integration.
- `[ ]` Phase 9: compatibility migration and legacy retirement.
- `[ ]` Phase 10: optional measured wire experiments.

Current implementation phase: none started.

Current validation state: documentation review only; no build or runtime validation was performed while creating this plan.

## Product and Architecture Invariants

Every phase must preserve these constraints:

1. Two people connecting directly remains the primary and best-tested workflow.
2. Three/four-person use remains direct full mesh.
3. No relay or TURN audio path is introduced. STUN remains endpoint discovery only.
4. Raw CPU, bandwidth, packet-rate, timing, buffering, loss, jitter, drift, and xrun data remain exposed.
5. The audio callback does not allocate, log, throw, lock, block, perform file I/O, or call GUI code.
6. Packet/audio storage is bounded, preallocated where practical, and has visible capacity/drop behavior.
7. Every remote audio peer is an independent clock domain.
8. Every peer is equal in the UDP audio data plane.
9. Session bootstrap authority, grid authority, and arrangement authority are separate concepts.
10. The peer that starts a metronome grid becomes authority for that grid revision.
11. `leader-audio` click injection comes from exactly that authority peer.
12. Starting, joining, leaving, or changing peers does not unnecessarily restart the local audio device.
13. Current `listen/connect` timing, buffering, drift, metronome, transport, recording, priority, and statistics behavior is preserved until a measured replacement is accepted.
14. No remotely derived packet, control, model, asset, file, mix, or timeline state grows without a declared hard bound.
15. Application control messages are dispatched only after authentication and explicit source/message-family authorization.
16. Remote assets are referenced by validated content hash; remote metadata never selects a local filesystem path.
17. An advertised UDP endpoint receives no audio until an authenticated challenge proves the observed source owns that endpoint.
18. Security parsing, hashing, file work, and cryptography remain outside the real-time callback.
19. v1 direct audio/control/assets are not claimed to be confidential; invited peers share the session trust boundary.

## V1 Security Contract

The detailed rationale and limits are in [refactor-security.md](refactor-security.md). Implementation must preserve this minimum contract:

- TCP control uses bounded length-prefixed framing, fixed connection/buffer/output limits, authentication and incomplete-frame deadlines, and bounded frames per event-loop turn.
- Client and server use a strict mutual challenge-response state machine; the master session key is not sent as a control field.
- Authenticated control frames carry a monotonic connection sequence and keyed tag. The receiver binds source identity from the connection rather than trusting payload claims.
- Every remote message type has explicit field types, numeric/string/count limits, and an authorization rule before it mutates application or engine state.
- Asset transfers are requested, size/chunk/time/concurrency bounded, streamed through incremental hashing to a temporary file, strictly parsed, and atomically committed.
- UDP v1 keeps its existing authenticated fixed wire bytes during the core refactor, while parsing becomes allocation-free and gains fixed replay, wrap, gap, timeline, ping, and work-budget safeguards.
- Control-advertised endpoints remain candidates until an authenticated response arrives from the observed UDP source.
- Session keys, peer tokens, and nonces come from an OS-backed CSPRNG and are excluded from routine logs, benchmark artifacts, CSV, and errors.
- TLS, UDP encryption, pairwise public-key identities, accounts, bans, and relay infrastructure are deferred unless the user expands the threat model.

## Target Architecture

```text
jam2 application
    |
    +-- GUI or headless command adapter
    |       |
    |       +-- typed commands
    |       +-- bounded events
    |       +-- fixed-shape snapshots
    |
    +-- persistent local Engine
            |
            +-- audio device and callback frame clock
            +-- local capture/playback handoffs
            +-- metronome and grid renderer
            +-- prepared tracks and transport
            +-- recording
            +-- technical statistics
            +-- optional NetworkSession
                    |
                    +-- bounded authenticated control and authorization
                    +-- session contract, membership, and endpoint proof
                    +-- one encoded local packet fanned out to peers
                    +-- PeerStream A: reorder/jitter/clock/drift/resampler
                    +-- PeerStream B: reorder/jitter/clock/drift/resampler
                    +-- PeerStream N: reorder/jitter/clock/drift/resampler
                    +-- bounded local-timeline mix scheduler
```

The conceptual runtime states are:

```text
local engine only
local engine + network session with zero peers
local engine + network session with one peer
local engine + network session with multiple peers
```

`listen` and `connect` become create/join bootstrap actions. `mesh` becomes the normal network topology rather than a separate engine mode.

## Source Map

### Current engine and mode implementation

| Source | Current responsibility | Primary phases |
| --- | --- | --- |
| [apps/jam2-cli/main.cpp](apps/jam2-cli/main.cpp) | CLI parsing, audio runtime, local/listen/connect/mesh loops, handshake, jitter/playout, drift, metronome, transport, stats, commands, GUI-control socket | 0-6, 8-9 |
| `run_audio_packet_exchange` in `main.cpp` | Mature two-person packet, playout, drift, and timing behavior | 0, 2, 4 |
| `run_local` in `main.cpp` | Immediate local engine and supervisor behavior | 0, 3 |
| `run_listen` and `run_connect` in `main.cpp` | Two-person bootstrap and compatibility handshake | 0, 4, 9 |
| `run_mesh` in `main.cpp` | Experimental static peer fan-out/mix behavior | 0-1, 5, 9 |
| `sync_audio_control` in `main.cpp` | Network/command-to-audio control synchronization | 3-6 |
| `mix_leader_click_into_packet` in `main.cpp` | Mature leader-audio injection behavior | 4, 6 |

### Core library

| Source | Current responsibility | Primary phases |
| --- | --- | --- |
| [libs/jam2-core/include/jam2/audio_ring.hpp](libs/jam2-core/include/jam2/audio_ring.hpp) and [audio_ring.cpp](libs/jam2-core/src/audio_ring.cpp) | Capture/playback ring and statistics | 0-3 |
| [libs/jam2-core/include/jam2/protocol.hpp](libs/jam2-core/include/jam2/protocol.hpp) and [protocol.cpp](libs/jam2-core/src/protocol.cpp) | Fixed UDP header, authentication, sequence tracking, PCM24 pack/unpack | 0-2, 4-6, 10 |
| [libs/jam2-core/include/jam2/udp_socket.hpp](libs/jam2-core/include/jam2/udp_socket.hpp) and [udp_socket.cpp](libs/jam2-core/src/udp_socket.cpp) | Socket lifetime, bind/send/receive, endpoints | 1-5 |
| [libs/jam2-core/include/jam2/common.hpp](libs/jam2-core/include/jam2/common.hpp) and [common.cpp](libs/jam2-core/src/common.cpp) | Session/key parsing and current random session/key generation | 0-1, 3-5 |
| [libs/jam2-core/include/jam2/audio_device.hpp](libs/jam2-core/include/jam2/audio_device.hpp) and platform implementations | Device discovery, duplex stream, callback control/timing | 0, 3, 7-8 |
| [libs/jam2-core/include/jam2/metronome.hpp](libs/jam2-core/include/jam2/metronome.hpp) and [metronome.cpp](libs/jam2-core/src/metronome.cpp) | Local click/grid behavior | 0, 3, 6 |
| [libs/jam2-core/include/jam2/prepared_track_source.hpp](libs/jam2-core/include/jam2/prepared_track_source.hpp) | Prepared-track playback | 0, 3, 6 |
| [libs/jam2-core/include/jam2/output_recorder.hpp](libs/jam2-core/include/jam2/output_recorder.hpp) | Output recording | 0, 3, 6 |
| [libs/jam2-core/include/jam2/track_take_recorder.hpp](libs/jam2-core/include/jam2/track_take_recorder.hpp) | Track-take recording | 0, 3, 6 |
| [libs/jam2-core/include/jam2/gui_control_protocol.hpp](libs/jam2-core/include/jam2/gui_control_protocol.hpp) | Existing engine/GUI control framing | 1, 3, 7-9 |
| [libs/jam2-core/CMakeLists.txt](libs/jam2-core/CMakeLists.txt) | Core source registration | 0, 3-8 |

Prefer extending the existing Qt-free core unless a separate engine library produces a concrete dependency or build benefit. Do not create extra libraries merely to mirror conceptual boxes.

### GUI and control plane

| Source | Current responsibility | Primary phases |
| --- | --- | --- |
| [apps/jam2-gui/MainWindow.cpp](apps/jam2-gui/MainWindow.cpp) and [MainWindow.hpp](apps/jam2-gui/MainWindow.hpp) | Session UI, process lifecycle, mode split, leader settings, mesh membership restarts, metronome controls, remote model dispatch, asset transfer, stats display | 0-1, 5-9 |
| `MainWindow::startJam` | Current listen/connect/mesh branching | 0, 7 |
| `MainWindow::restartMeshEngineFromPeerList` | Current process restart on membership change | 0, 5, 7 |
| [apps/jam2-gui/Jam2Process.cpp](apps/jam2-gui/Jam2Process.cpp) | Child-engine process wrapper | 0, 7-9 |
| [apps/jam2-gui/ControlServer.cpp](apps/jam2-gui/ControlServer.cpp) | Creator-side star control, admission, framing, authentication, source binding, and peer dispatch | 0-1, 5-9 |
| [apps/jam2-gui/ControlClient.cpp](apps/jam2-gui/ControlClient.cpp) | Joiner-side control framing, mutual authentication, and authenticated dispatch | 0-1, 5-9 |
| [apps/jam2-gui/SessionController.cpp](apps/jam2-gui/SessionController.cpp) | GUI session control state | 3, 6-8 |
| [apps/jam2-gui/SharedTrackController.cpp](apps/jam2-gui/SharedTrackController.cpp) | Shared song/track control | 3, 6-8 |
| [apps/jam2-gui/PreparedMixRenderer.cpp](apps/jam2-gui/PreparedMixRenderer.cpp) | Current WAV parsing and prepared mix work | 0-1, 3, 7-8 |
| [apps/jam2-gui/LooperProject.cpp](apps/jam2-gui/LooperProject.cpp) and [BeatGridModel.cpp](apps/jam2-gui/BeatGridModel.cpp) | Remote project/grid model loading and allocation-driving values | 0-1, 6-8 |
| [apps/jam2-gui/PlaybackGrid.cpp](apps/jam2-gui/PlaybackGrid.cpp) | GUI playback grid representation | 3, 6-8 |
| [apps/jam2-gui/GuiLoopbackRecorder.cpp](apps/jam2-gui/GuiLoopbackRecorder.cpp) | GUI-side recording behavior | 3, 7-8 |
| [apps/jam2-gui/CMakeLists.txt](apps/jam2-gui/CMakeLists.txt) | GUI source registration and final executable integration | 7-9 |

### Existing measurement and diagnostic tools

| Source | Use |
| --- | --- |
| [tools/run_stress_local.py](tools/run_stress_local.py) | Same-machine normal/mesh scenarios, network impairments, validation, audio probes |
| [tools/udp_stress_proxy.py](tools/udp_stress_proxy.py) | Controlled delay, jitter, loss, reorder, duplication, and burst impairment |
| [tools/run_benchmark_server.py](tools/run_benchmark_server.py) | Two-host benchmark coordination and server-side engine runs |
| [tools/run_benchmark_client.py](tools/run_benchmark_client.py) | Client-side benchmark execution and artifact upload |
| [tools/jam2_benchmark_suite.py](tools/jam2_benchmark_suite.py) | Benchmark profile/case definitions including directional and metronome cases |
| [tools/jam2_profiles.py](tools/jam2_profiles.py) | Named tuning profiles used by tools |
| [tools/jam2_metrics.py](tools/jam2_metrics.py) | Structured result summaries |
| [tools/jam2_audio_analysis.py](tools/jam2_audio_analysis.py) | Recorded audio analysis |
| [tools/analyze_benchmark_results.py](tools/analyze_benchmark_results.py) | Static benchmark result aggregation |
| [tools/connection_test.py](tools/connection_test.py) | Endpoint/STUN/direct-connect diagnostics |

There is currently no CMake/CTest native test registration in the reviewed tree. Phase 0 must establish it rather than relying only on Python process-level tools.

The current tooling gaps, target scenario/automation interface, ownership split, and migration gates are defined in [refactor-python.md](refactor-python.md).

## Measurement Contract

### Baseline identities

Every stored comparison should identify:

- Source revision or an exact description of the working tree.
- Platform, OS version, CPU, and power mode.
- Audio API, device, sample rate, and device buffer size.
- Jam2 profile and every numeric override.
- Control/UDP protocol version and every security capacity/timeout/horizon constant.
- Network type and topology.
- Peer count and peer role in the scenario.
- Signal/case name, duration, repeat index, and random seed.
- Whether the run is current legacy, intermediate compatibility, or replacement implementation.
- Output paths for raw stdout/stderr, CSV, recordings, metadata, and analysis.

Do not compare runs whose sample rate, device buffer, frame size, impairment seed, or topology differs without labeling that difference.

### Required raw metrics

Record these when applicable:

#### Real-time audio

- Callback count.
- Callback interval minimum, average, maximum, and available percentiles.
- Callback gap/xrun count.
- Capture and playback ring depth, high-water mark, overrun, underrun, and explicit-drop counts.
- Recording/prepared-source queue depth and saturation.

#### Packet loop

- Packets and bytes sent/received per second.
- Send interval minimum, average, maximum, and available percentiles.
- Receive-loop and scheduler gap counts/durations.
- Catch-up send count and maximum catch-up batch.
- Packet-loop CPU and whole-process CPU.
- Allocations per packet and allocations per second when profiling is available.
- Bytes copied per packet when profiling is available.
- Parse/auth/type/flags/session/replay/past-horizon/future-horizon/work-budget rejection counts.
- Datagrams processed per wake/batch and deadline recheck count.

#### Per remote peer

- RTT and jitter.
- Loss, duplicate, reorder, recovered reorder, late, replay, and malformed counts.
- Reorder/jitter occupancy and capacity drops.
- Remote-to-local clock estimate.
- Raw and smoothed drift ppm.
- Applied resampler ratio and clamp/deadband/slew activity.
- Playout target, actual depth, underrun, missing-frame, future-window reject, and late-after-release counts.
- Bitrate and packet rate.
- Replay-window occupancy/advance, largest accepted/rejected gap, and ping challenge match/expiry counts.
- Endpoint candidate/probe attempts, observed source, activation duration, migration, and rejection reason.

#### Mesh/mix

- Active peer count.
- Active and maximum mix slots.
- Missing peer contributions by peer and total.
- Mix deadline releases.
- Capacity drops and excessive-future rejects.
- Clipping/saturation count.
- Mix CPU and per-peer resampler CPU where available.

#### Timing and musical state

- Grid revision and authority peer ID.
- Authority epoch and local mapped epoch.
- Phase error before and after correction.
- Compensation amount, bounds, deadband, and slew.
- Stale/missing authority-reference state.
- Transport event source, event ID, grid revision, scheduled frame, applied frame, and error.
- Leader-audio click source count.

#### Lifecycle and control

- Audio-device start/restart/stop count.
- Network attach/detach duration.
- Peer handshake and activation duration.
- Peer add/remove duration.
- Startup and shutdown duration.
- Command/event queue depth and overflow.
- Current/peak pending and authenticated TCP peers.
- Per-peer control receive-buffer and queued-output high-water marks.
- Control frame size, authentication, deadline, sequence/replay, source-role, schema, and capacity rejection counts by reason.
- Control handshake duration and failure stage without exposing keys/nonces in routine output.
- Active/pending asset transfers, declared/received bytes, chunk count, duration, temporary-file cleanup, and rejection reason.
- WAV parser format/size/resource rejection counts and prepared-worker failure reason.

### Comparison rules

- Preserve raw results; summaries do not replace CSV and recordings.
- Use repeated runs for timing-sensitive comparisons.
- Establish the acceptable baseline range before judging a replacement.
- Report absolute values and deltas. Do not reduce them to a subjective playability score.
- Clean-run correctness counters expected to remain zero must remain zero.
- A buffer-default increase is not an acceptable way to pass a latency or underrun regression unless it is a separately approved tuning change.
- Any queue/storage implementation must show configured capacity, high-water mark, and overflow/drop behavior.
- Security rejection counters must be rate-safe; malformed traffic must not cause per-packet logging or unbounded diagnostic storage.
- Routine artifacts must use a non-secret run/session identifier and must not record the master key.
- Performance-sensitive phase promotion requires user review of the recorded comparison, even when automated tests pass.

## Baseline Scenario Matrix

Phase 0 should capture at least this matrix, using existing tools where possible:

| Topology | Profiles | Signals | Network conditions |
| --- | --- | --- | --- |
| Current `listen/connect`, two peers | fast, moderate, safe | silence, directional tone, pulse, metronome modes | clean, loss, jitter, reorder, duplicate, burst |
| Current `listen/connect`, two peers | fast plus drift variants | tone/pulse | positive and negative simulated drift |
| Current `mesh`, two peers | fast, moderate, safe | silence, tone, pulse | clean plus representative impairment |
| Current `mesh`, three peers | fast and one conservative profile | silence, independent tones/pulses | clean; one impaired peer |
| Current `mesh`, four peers | fast and one conservative profile | silence, independent tones/pulses | clean; one impaired peer |
| Local lifecycle | applicable local settings | metronome, prepared track, recording | no UDP |

The current mesh results document what exists; they do not define the replacement's receive-quality target. Two-peer replacement parity is judged against `listen/connect`.

### Security component matrix

Run these as deterministic component/fuzz tests rather than exposing a live service to uncontrolled traffic:

| Surface | Required cases |
| --- | --- |
| TCP admission/framing | Fragmented/coalesced frames, missing payload, maximum/max-plus-one length, pending/authenticated cap, deadline, output high-water, frames-per-turn budget |
| Control authentication | Wrong/replayed proof, fake server, pre-auth application message, altered transcript, wrong-direction/tag/sequence frame |
| Control authorization/schema | Every message family from every role, forged payload source, wrong JSON types, boundary/max-plus-one counts/strings/numbers, stale revision |
| Asset state | Unsolicited/oversized/overlapping transfer, invalid hash, wrong chunk/order/count, timeout/disconnect, excess bytes, hash/disk/atomic-commit failure |
| WAV parser | Truncation, RIFF/chunk/format/alignment/count/size errors, unsupported format/channels, excessive frames, arithmetic boundaries, generated fuzz corpus |
| UDP | Type/flags/length/auth/replay/wrap/large-gap/horizon/work-budget cases, unmatched/expired pong, third-party endpoint candidate and observed-source proof |

Sanitizers and fuzzers are test-only and add no release cost. Builds/runs still require the user's explicit request under [AGENTS.md](AGENTS.md).

## Phase 0: Baseline Evidence and Native Test Foundation

### Objective

Freeze current observable behavior and create deterministic unit/component tests before refactoring it.

### Entry conditions

- This worker plan and the governing documents listed above are available.
- The baseline and successor automation contracts in [refactor-python.md](refactor-python.md) are available.
- The user has selected when and where builds/benchmark runs will occur.

### Tasks

- `[ ]` Record the exact baseline configuration and working-tree identity.
- `[ ]` Capture the baseline scenario matrix above.
- `[ ]` Preserve representative raw CSV, logs, metadata, and recordings.
- `[ ]` Add CMake/CTest registration for Qt-free native tests.
- `[ ]` Add golden encode/decode/authentication tests for every UDP packet type.
- `[ ]` Add malformed/truncated/oversized/version/type/flags/session/key/tag/payload tests.
- `[ ]` Add sequence gap, duplicate, reorder, replay, and `0xffffffff -> 0` wrap tests.
- `[ ]` Add ring wrap/full/empty/overrun/underrun/concurrent-drop ownership tests.
- `[ ]` Add deterministic jitter/reorder release tests.
- `[ ]` Add drift-controller and resampler-bound tests.
- `[ ]` Add metronome/grid projection and transport scheduling tests around current behavior.
- `[ ]` Record deterministic reproductions for the current pre-auth dispatch, unbounded framing/model/asset, endpoint-trust, and secret-artifact gaps where the current code has a usable seam; mark them as Phase 1 known failures rather than accepted behavior.
- `[ ]` Define golden vectors and a test contract for the Phase 1 bounded control deframer, mutual handshake, authenticated frame sequence/tag, and source-role authorization matrix without changing current runtime behavior.
- `[ ]` Define boundary tables for every remote JSON/model count, string, numeric, asset, path, and narrow PCM16 WAV field that Phase 1 must enforce.
- `[ ]` Add current UDP decoder fuzz coverage and define deterministic replay-window, bounded-gap, sample-time-horizon, ping-correlation, packet-work-budget, and endpoint-proof vectors for Phase 1/4/5.
- `[ ]` Identify the extraction seams required to fuzz the control decoder, remote-message validators, asset state machine, and WAV parser independently in Phase 1.
- `[ ]` Add test-only sanitizer/fuzz configurations where supported; do not enable their cost in release builds.
- `[ ]` Capture a secret-artifact baseline covering logs, CSV, benchmark metadata, errors, and process arguments so each exposure can be removed and rechecked in its owning phase.
- `[ ]` Document which important behaviors currently have no deterministic test seam.

### Relevant sources

- `apps/jam2-cli/main.cpp`, especially `run_audio_packet_exchange`, all four mode functions, and statistics emission.
- `libs/jam2-core/src/protocol.cpp`.
- `libs/jam2-core/src/audio_ring.cpp`.
- `libs/jam2-core/src/metronome.cpp`.
- `libs/jam2-core/src/common.cpp` and `udp_socket.cpp`.
- `apps/jam2-gui/ControlServer.cpp`, `ControlClient.cpp`, `MainWindow.cpp`, `LooperProject.cpp`, `BeatGridModel.cpp`, and `PreparedMixRenderer.cpp`.
- Root and core CMake files.
- `tools/run_stress_local.py` and the two-host benchmark suite.

### Validation

- Existing process-level scenarios have reproducible metadata and raw artifacts.
- Native tests run through CTest when the user authorizes the build/test run.
- Tests fail for deliberately invalid packet/ring behavior and pass for valid golden cases.
- Existing valid/invalid behavior is captured without treating a known exposure as acceptable parity.
- Available fuzz/sanitizer harnesses run independently from audio hardware when the user authorizes them; missing seams have exact Phase 1 extraction tasks and cases.
- Baselines contain enough raw fields to evaluate later phases.

### Exit gate

- `[ ]` User accepts the captured `listen/connect` baselines as the one-peer reference.
- `[ ]` Baseline limitations are documented rather than silently ignored.
- `[ ]` Native protocol/ring/sequence test foundation is passing.
- `[ ]` Current security gaps have reproducible evidence/test vectors and every desired Phase 1 correction has an explicit acceptance case; tests for protections that already exist are passing.

## Phase 1: Correctness, Lightweight Security, Limits, and Observability

### Objective

Correct unsafe ownership/comparison behavior and establish the lightweight v1 security boundary before performance or architecture changes. Implement each subsection as an isolated, reviewable change; Phase 1 is not one large protocol rewrite.

### Tasks

#### Playback ring ownership

- `[ ]` Preserve a single owner of the playback read index.
- `[ ]` Replace producer-side read-index mutation with a bounded requested-drop handoff, or another rigorously reviewed ownership design.
- `[ ]` Expose requested, applied, coalesced, and maximum-batch drops.
- `[ ]` Test callback/network concurrency and wraparound.

#### Packet sequencing and validation

- `[ ]` Implement modular wrap-safe sequence distance.
- `[ ]` Keep loss, duplicate, reordered, recovered, late, and replay counters separate.
- `[ ]` Validate allowed packet types, flags/reserved values, and exact/bounded payload sizes.
- `[ ]` Return explicit parse statuses for expected invalid datagrams rather than throwing.
- `[ ]` Add a raw rejection counter for every parse/auth/session/endpoint reason.
- `[ ]` Define/test the fixed replay-window, bounded-gap, sample-time-horizon, ping-challenge, and packet-work-budget primitives that mature `PeerStream` will own in Phase 4.

#### Socket ownership and secrets

- `[ ]` Replace `UdpSocket` move assignment's explicit self-destruction with a valid RAII handle reset/exchange.
- `[ ]` Add a Qt-free OS-backed `secure_random_bytes` boundary with explicit failure handling for session keys, peer tokens, and nonces.
- `[ ]` Derive domain-separated control connection keys; define/test the UDP key-derivation contract for Phase 4/5 without changing the current UDP compatibility path before its parity gate.
- `[ ]` Redact the master key from routine logs, CSV, benchmark metadata, errors, and other stored artifacts.
- `[ ]` Re-run the Phase 0 secret-artifact scan and preserve only explicitly documented invite/user-copy exposure.

#### TCP control admission, framing, and authentication

- `[ ]` Replace newline accumulation with bounded length-prefixed handshake/control framing; reject an excessive declared length before allocation.
- `[ ]` Add hard pending/authenticated peer caps, authentication and incomplete-frame deadlines, per-peer receive/output high-water marks, and a frames-per-event-turn budget.
- `[ ]` Fix the client pre-authentication dispatch bug: only the exact next handshake message is accepted before authentication and no application callback can run.
- `[ ]` Implement mutual nonce challenge-response without sending the raw master key in the hello transcript.
- `[ ]` Derive separate connection-direction keys and authenticate every application frame with a monotonic sequence and fixed tag.
- `[ ]` Apply bounded output/backpressure behavior; close a peer that cannot consume control state rather than grow queued bytes indefinitely.
- `[ ]` Use an explicit control-protocol version and fail clearly on incompatibility.
- `[ ]` Implement and pass the Phase 0 fragmentation/cap/deadline/pre-auth/proof/tag/sequence/tamper/fake-server test vectors.

#### Control authorization and remote schemas

- `[ ]` Preserve authenticated source peer ID and connection direction in every server/client dispatch.
- `[ ]` Define a message-family authorization table; distinguish peer proposals from coordinator/authority snapshots.
- `[ ]` Ignore/overwrite payload-claimed source, role, or endpoint owner with authenticated connection identity.
- `[ ]` Validate exact JSON field types and centralized product-derived count/string/numeric limits before model or engine mutation.
- `[ ]` Reject non-finite values and perform checked narrowing, addition, and multiplication for bytes, samples, frames, and allocation sizes.
- `[ ]` Bound sections, beats, lanes, peer lists, events, requested hashes, text/lyrics/names/IDs, and every other remotely driven collection.
- `[ ]` Add monotonic revision checks wherever stale state must not overwrite current state.
- `[ ]` Implement and pass the Phase 0 source-role matrix and every boundary/max-plus-one schema case.

#### Asset, path, and WAV containment

- `[ ]` Derive a hard maximum asset byte count from the supported PCM16 duration/channel/format contract.
- `[ ]` Accept only validated 64-character lowercase SHA-256 identifiers that are referenced by accepted state and explicitly pending/requested.
- `[ ]` Bound asset chunk bytes/count/order, cumulative bytes, transfer time, request duplication, concurrency, and aggregate disk use.
- `[ ]` Clear/abort transfer state and remove temporary files on disconnect or any protocol/file/hash failure.
- `[ ]` Stream outgoing files and incoming assets incrementally; do not retain complete base64 and decoded copies.
- `[ ]` Hash while writing to a same-directory temporary file and atomically commit only after exact size, hash, and WAV validation succeeds.
- `[ ]` Target asset responses to the requesting peer instead of broadcasting them.
- `[ ]` Ignore all remote `asset_path` values; construct and verify a canonical path beneath the fixed cache root from the validated hash.
- `[ ]` Isolate a strict PCM16 RIFF/WAVE parser and validate file/RIFF/chunk limits, format tag, channels, rate, bits, byte rate, block alignment, data alignment, frame count, and arithmetic before allocation/DSP.
- `[ ]` Keep parse/hash/file/stretch/mix work on bounded non-real-time workers.
- `[ ]` Implement the isolated asset/control/WAV fuzz harnesses and pass the deterministic transfer/parser failure matrix under test-only sanitizers when the user authorizes the run.

#### Membership and current mesh containment

- `[ ]` Reject an over-cap peer on its own connection and close it after the error is sent.
- `[ ]` Ensure a rejected peer receives no membership, song, or asset broadcasts.
- `[ ]` Treat advertised endpoints only as candidates; current compatibility code must not silently broaden transmission beyond configured peers.
- `[ ]` If current mesh must remain usable during the refactor, add minimal bounded storage, stale/future rejection, and deterministic saturation safeguards.
- `[ ]` Avoid investing in a second mature receive engine inside `run_mesh`; the proper mixer belongs to Phase 5.

### Relevant sources

- `audio_ring.hpp/.cpp` and ring use in `apps/jam2-cli/main.cpp`.
- `protocol.hpp/.cpp`, `udp_socket.hpp/.cpp`, `common.hpp/.cpp`, and all receive paths.
- `ControlServer.cpp`, `ControlClient.cpp`, and control/asset/model handling in `MainWindow.cpp`.
- `BeatGridModel.cpp`, `LooperProject.cpp`, `SharedTrackController.cpp`, and `PreparedMixRenderer.cpp`.
- Current `run_mesh` pending-mix storage and peer-cap GUI handling.

### Measurements

- Ring requested/applied drop counts and depth correctness under stress.
- Rejection counters under malformed/fuzzed datagrams.
- Current/peak pending/authenticated peers, control receive/output high-water, handshake duration, deadline/cap/backpressure rejection, and normal frame overhead.
- Control authentication/sequence/source-role/schema rejection counts with no pre-auth or unauthorized state mutation.
- Maximum asset worker memory, temporary/disk bytes, transfer duration, and cleanup/rejection counts.
- WAV validation time and parser/DSP worker memory for boundary-valid files.
- Current mesh slot/entry high-water marks and bounded reject/drop counts.
- Packet-loop CPU, allocations, copied bytes, send gaps, callback gaps, audio bandwidth, and timing deltas versus Phase 0.

### Exit gate

- `[ ]` Deterministic correctness tests pass.
- `[ ]` No packet/control/model/asset/file/mix-driven storage or work is unbounded.
- `[ ]` The playback ring has explicit, tested index ownership.
- `[ ]` Invalid traffic is classified without expected-path exceptions.
- `[ ]` No application message is dispatched before mutual authentication, and every accepted mutation has an authenticated/authorized source.
- `[ ]` The raw master key is absent from the control hello and routine stored artifacts.
- `[ ]` Remote asset state cannot select a local path; asset transfer/parse/commit is bounded and failure-safe.
- `[ ]` The accepted security controls do not change UDP v1 wire bytes, audio bandwidth, or configured playout latency.
- `[ ]` Baseline audio behavior has not regressed.

## Phase 2: Wire-Compatible Fast-Path Primitives

### Objective

Remove packet-rate allocation, avoidable copies, address conversion, and scheduler delay without changing the v1 UDP header or PCM24 wire representation.

### Ordered tasks

1. `[ ]` Introduce a resolved numeric endpoint object and compact comparison key.
2. `[ ]` Resolve a hostname only when a peer is added or its endpoint changes.
3. `[ ]` Add caller-owned/fixed-capacity UDP transmit and receive buffers.
4. `[ ]` Pack PCM24 directly after the packet header in the transmit buffer.
5. `[ ]` Authenticate packet spans without copying the whole datagram to zero the tag.
6. `[ ]` Decode PCM24 directly into final fixed packet storage.
7. `[ ]` Implement the tested fixed replay-window and checked wrap/gap/horizon primitives without changing current playout policy before Phase 4.
8. `[ ]` Replace packet-loop double waiting with one deadline-aware socket wait.
9. `[ ]` Drain ready datagrams nonblocking in bounded batches and re-evaluate send deadlines between batches.
10. `[ ]` Add bounded indexed reorder/jitter storage where its ownership is already stable.
11. `[ ]` Bulk-copy ring data in at most two contiguous spans after Phase 1 ownership is proven.
12. `[ ]` Compare each change separately with the Phase 0 baseline.

If fixed reorder/jitter storage would require duplicating logic that is about to become `PeerStream`, define and test the storage primitive here but integrate it with mature playout in Phase 4.

### Relevant sources

- `protocol.hpp/.cpp` vector-returning packet and PCM APIs.
- `udp_socket.hpp/.cpp` address and receive/send APIs.
- Normal and mesh network loops in `apps/jam2-cli/main.cpp`.
- `audio_ring.hpp/.cpp`.

### Required measurements

- Allocations per packet and allocations/sec.
- Packet bytes copied.
- Endpoint conversion/resolution calls in the packet loop.
- Replay/horizon/gap decision cost and bytes of fixed state per peer.
- Packets per receive batch, budget yields, and send-deadline rechecks.
- Network-loop CPU and total process CPU.
- Send interval distribution, maximum scheduler gap, and catch-up batches.
- Callback timing and ring counters.
- Packet loss, jitter, late, missing, and underrun counters.

### Expected direction

- Most packet-path heap allocations removed.
- No DNS or string endpoint formatting in the hot packet loop.
- Up to roughly 1 ms of avoidable mesh-loop delay removed where the old receive-wait-plus-sleep behavior occurred.
- Average physical network latency may remain similar; tail loop timing and multi-peer scaling are the important gains.

### Exit gate

- `[ ]` UDP v1 bytes remain compatible with golden tests.
- `[ ]` Fast-path storage is bounded and expected packet processing does not throw.
- `[ ]` Malformed/replayed/excessive-work traffic cannot allocate or monopolize an unbounded receive loop.
- `[ ]` No regression in the accepted one-peer audio baseline.
- `[ ]` Before/after raw results are recorded for every accepted optimization.

## Phase 3: Persistent Local Engine Extraction

### Objective

Create a Qt-free persistent `Engine` and clear ownership boundaries while legacy mode behavior still operates.

The engine-facing test-source, frame-scheduling, effective-configuration, command/event, and snapshot seams required by retained Python automation are defined in [refactor-python.md](refactor-python.md).

### Target engine ownership

- Audio device and RAII stream lifetime.
- Authoritative callback `LocalEngineFrame` clock.
- Capture/playback/recording handoff buffers.
- Metronome and current grid renderer.
- Prepared-track source and transport application.
- Recording lifetime and writer boundaries.
- Runtime tuning/configuration.
- Technical statistics and fixed-shape snapshots.
- Optional network-capture tap and future `NetworkSession` attachment.

### Boundary types

Define typed, Qt-free structures for:

- `EngineConfig` and validated derived configuration.
- `EngineCommand` using bounded payload/storage.
- `EngineEvent` for bounded lifecycle, error, peer, transport, and recording events.
- `EngineSnapshot` for fixed-shape technical state.
- Explicit lifecycle state and error results.
- Authenticated source/authority metadata on every command that originates remotely.

Qt objects, JSON objects, widgets, dialogs, filesystem UI work, and callbacks into GUI objects do not enter the engine or real-time layer.

### Tasks

- `[ ]` Extract common configuration parsing/validation from mode execution.
- `[ ]` Move audio device, metronome, track, recording, control, and stats ownership into `Engine`.
- `[ ]` Keep audio callback frame time authoritative; do not substitute a GUI or supervisor timer.
- `[ ]` Make network capture explicitly disabled while local-only.
- `[ ]` On network activation, discard stale capture and establish a documented frame boundary.
- `[ ]` Keep legacy `local`, `listen`, `connect`, and `mesh` entry points working through the extracted engine.
- `[ ]` Split the monolithic CLI file only along the ownership boundaries created by the extraction.
- `[ ]` Permit remote input to become an `EngineCommand` only after control authentication, source authorization, and schema validation outside the engine/audio callback.
- `[ ]` Keep packet parsing, control MAC work, asset/WAV parsing, hashing, and filesystem/DSP workers outside the real-time callback with bounded command/event handoffs.
- `[ ]` Preserve Phase 1 security capacities, rejection counters, and protocol state during extraction rather than recreating GUI-owned unbounded buffers.
- `[ ]` Catch worker/command exceptions at boundaries and convert them into bounded error events.
- `[ ]` Make shutdown order explicit and bounded.

### Suggested source organization

Names can be adjusted to existing conventions, but the responsibilities should become independently testable:

```text
engine_config
engine
engine_command
engine_snapshot
network_transport
packet_codec
peer_stream        (Phase 4)
network_session    (Phase 5)
mix_scheduler      (Phase 5)
grid_sync          (Phase 6)
transport_sync     (Phase 6)
stats
```

Prefer these inside `jam2-core` unless a separate library demonstrably simplifies Qt linkage or platform ownership.

### Validation

- Local device/metronome/track/recording behavior matches baseline.
- Legacy network modes still pass their Phase 0 scenarios.
- Enabling network capture never sends samples accumulated while disabled.
- Local -> network-capable -> local transitions do not corrupt ring or frame counters.
- Runtime errors are visible without exceptions escaping worker top-level boundaries.
- Rejected remote control/file input cannot partially enqueue an engine command or mutate engine state.
- Security parsing and worker saturation do not block the callback or grow command/event queues beyond their visible capacity.

### Exit gate

- `[ ]` One persistent local engine owns all local audio state.
- `[ ]` Legacy mode behavior remains available and measured.
- `[ ]` The engine API is usable without Qt or a child process.
- `[ ]` Real-time callback constraints remain satisfied.
- `[ ]` The Phase 1 security contract and counters remain intact at the new ownership boundaries.

## Phase 4: Mature One-Peer `PeerStream` Parity

### Objective

Turn the current mature `listen/connect` receive behavior into one reusable remote-peer component without losing its quality or diagnostics.

This is the primary parity gate for the entire refactor.

### Behavioral source

Use `run_audio_packet_exchange` as the behavioral source for:

- Wrap-safe sequence tracking and reorder recovery.
- Bounded jitter and sample-time release.
- Missing-frame insertion/accounting.
- Late/duplicate/drop classification.
- Playback prefill and maximum behavior.
- Adaptive cushion.
- RTT and jitter observations.
- Remote sample-time slope and drift estimation.
- Bounded resampler control.
- Metronome state/phase observations.
- Leader-audio packet injection.
- Transport messages.
- Network-thread priority.
- Full detailed statistics.

Do not use `run_mesh` as the receive, clock, drift, or playout template.

### PeerStream responsibilities

- Stable peer ID independent of endpoint.
- Authenticated and versioned stream handshake.
- Sample-rate, frame-size, and network-format compatibility validation.
- Current endpoint and endpoint-change state.
- Per-peer sequence/replay state.
- Fixed replay window and bounded forward-gap policy.
- Per-peer fixed reorder and jitter storage.
- Accepted past/future sample-time horizons.
- `RemoteStreamFrame(peer)` to `LocalEngineFrame` mapping.
- Per-peer drift estimator and resampler.
- Per-peer playout target/deadline state.
- Outstanding ping challenges and correlated RTT observations.
- Per-wake/per-peer packet-work budget and reason-specific rejection counters.
- Per-peer audio block output on the local timeline.
- Per-peer raw technical snapshot.

### Tasks

- `[ ]` Extract the mature receive pipeline into `PeerStream`.
- `[ ]` Define explicit local, remote, monotonic, and grid time types or clearly named wrappers/conversions.
- `[ ]` Introduce stable peer identity.
- `[ ]` Generalize listener/connect handshake into symmetric per-edge authentication and compatibility state.
- `[ ]` Derive the UDP protocol key from the session master with explicit domain/version separation while retaining the UDP v1 packet layout.
- `[ ]` Integrate fixed wrap-safe replay, bounded huge-gap handling, and mapped sample-time past/future horizons before reorder/jitter storage.
- `[ ]` Correlate every accepted pong with one live unpredictable ping challenge from the bound peer/source and expire/consume challenges deterministically.
- `[ ]` Apply bounded packet batches/work per peer and recheck send/playout deadlines so an authenticated peer cannot starve scheduling.
- `[ ]` Retain join-initiated UDP probing and observed-source endpoint handling.
- `[ ]` Preserve priority and deadline scheduling.
- `[ ]` Run the replacement with exactly one remote peer.
- `[ ]` Produce side-by-side legacy and replacement outputs for every one-peer parity case.

### One-peer parity matrix

- Clean fast, moderate, and safe profiles.
- Silence, tone, pulse, bidirectional audio, and directional audio.
- Loss, jitter, reorder, duplicate, and burst impairment.
- Positive and negative clock drift.
- Adaptive cushion on/off and configured bounds.
- Playback maximum/drop behavior.
- Shared-grid, leader-audio, and current listener-compensated behavior.
- Prepared-track transport and recording.
- Startup, incompatible handshake, remote departure, and shutdown.
- Duplicate/old replay, sequence wrap, huge authenticated sequence gap, old/future sample time, and burst beyond the work budget.
- Unsolicited/wrong/expired/duplicate pong and packets arriving from a source not bound to the peer.

### Exit gate

- `[ ]` All deterministic `PeerStream` tests pass.
- `[ ]` Clean-run correctness counters match accepted expectations.
- `[ ]` Buffering, drift, timing, metronome, transport, recording, priority, and raw stats behavior is at parity with `listen/connect`.
- `[ ]` Replay/gap/horizon/ping/work-budget safeguards use fixed state and do not change accepted clean-path playout timing.
- `[ ]` User accepts the side-by-side one-peer results.

Hard stop: do not implement general full mesh until this gate passes.

## Phase 5: Universal `NetworkSession` and Full Mesh

### Objective

Expand the proven one-peer component into the only network audio engine, where two people are the smallest full mesh.

Use the arbitrary-peer scenario, per-edge impairment, stable peer identity, and normalized artifact requirements in [refactor-python.md](refactor-python.md) when validating this phase.

### NetworkSession responsibilities

- Session ID/key and immutable session contract.
- Stable local peer ID and peer registry.
- Bounded authenticated control connections and source/role authorization.
- UDP socket, candidate discovery, authenticated observed-source endpoint proof, and migration state.
- Per-edge handshake and state.
- Encode local audio once per packet interval.
- Fan out the same authenticated audio packet to every active peer where authentication format permits; otherwise share all pre-auth encoded bytes and perform only required per-peer finalization.
- One independent `PeerStream` for every remote peer.
- Bounded local-timeline mix scheduler.
- Dynamic add/remove/update without audio-device restart.
- Per-peer and aggregate technical snapshots.

### Required clock and mix model

```text
remote packet sample time
    -> that peer's clock mapper
    -> that peer's jitter/playout state
    -> that peer's drift resampler
    -> LocalEngineFrame block
    -> bounded mix slot
```

Never mix packets from different peers by comparing raw sender `sample_time`. Each peer begins independently, drifts independently, and can restart independently.

Do not apply one resampler ratio after the aggregate mix. Each independent remote device clock needs correction before mixing.

### Tasks

- `[ ]` Implement the peer registry and stable identity/endpoint separation.
- `[ ]` Bind control connection, stable peer identity, UDP key context, and authorized message roles in one session registry entry.
- `[ ]` Implement encode-once fan-out.
- `[ ]` Create/remove independent `PeerStream` instances dynamically.
- `[ ]` Implement bounded mix slots indexed by normalized local playout time.
- `[ ]` Track a peer contribution bitset/generation per slot.
- `[ ]` Reject released-past and excessive-future contributions.
- `[ ]` Release at a clear deadline with silence only for missing peers.
- `[ ]` Accumulate in a wide fixed type and saturate once at release.
- `[ ]` Add per-peer gain/mute without changing timing ownership.
- `[ ]` Make `Bye` remove one peer rather than stop or restart the entire session.
- `[ ]` Add membership changes without restarting the local audio device.
- `[ ]` Treat membership-advertised endpoints as candidates and resolve/normalize them once outside the packet loop.
- `[ ]` Send only a small rate-limited authenticated challenge to a candidate; activate audio only after a matching response arrives from the observed UDP source.
- `[ ]` Return an endpoint to probing on observed-source change; never continue audio to an unproved migrated endpoint.
- `[ ]` Ensure one peer cannot claim/remove another peer or supply a source/role identity through payload fields.
- `[ ]` Preserve direct-only STUN/NAT behavior and expose every edge state/rejection reason.

### Connectivity states to expose per edge

```text
candidate
probing
authenticated
compatible
active
stale
failed with reason
```

Do not report a peer as audio-connected merely because its TCP control connection succeeded.

### Validation matrix

- Two peers through the universal path, compared again with Phase 4.
- Three and four clean peers.
- Independently simulated positive and negative drift per remote peer.
- One impaired peer while unaffected peers remain stable.
- Offset startup epochs and late joins.
- Peer restart with a new endpoint/sample epoch.
- Missing contributor silence isolated to that peer.
- Late-after-release and excessive-future rejection.
- Deterministic clipping independent of packet arrival order.
- Peer join/leave without device restart or stale audio.
- Direct edge failure behind restrictive NAT reported accurately.
- Third-party advertised endpoint receives only the bounded candidate probes and never audio.
- Wrong-source/wrong-peer/replayed/expired endpoint challenge responses cannot activate an edge.
- Endpoint migration pauses that edge, reproves the observed source, and does not disturb other peers or the audio device.

### Exit gate

- `[ ]` Two-person universal-network behavior retains Phase 4 parity.
- `[ ]` Three/four peer streams maintain independent drift and queue stability.
- `[ ]` Mix storage is fixed/bounded and deterministic.
- `[ ]` Peer membership changes do not restart the device.
- `[ ]` Every per-edge failure and capacity limit is visible in raw stats.
- `[ ]` No audio is transmitted to an unproved candidate and no payload-claimed identity overrides the authenticated registry.

## Phase 6: Timing, Metronome, and Transport Authority

### Objective

Replace permanent listener/connector/coordinator timing roles with explicit versioned musical authority while retaining equal UDP audio peers.

The frame/event-triggered automation rules in [refactor-python.md](refactor-python.md) govern reproducible authority, metronome, and transport scenarios; debug control must not bypass the real authority path.

### Separate roles

1. Bootstrap/session coordinator: membership, immutable contract, ordering of conflicting control requests.
2. Audio peer: equal direct sender/receiver in the full mesh.
3. Grid authority: peer that owns the current metronome grid revision.
4. Arrangement authority: source of accepted song/prepared-track transport changes.

The creator can initially remain bootstrap and arrangement coordinator. That does not make it permanent grid authority or an audio relay.

### Configuration split

#### Immutable session contract

- Protocol and application compatibility version.
- Sample rate.
- Network frame size.
- Network sample representation.
- Channel/stream layout required by the wire protocol.
- Session identity/authentication context.

#### Versioned musical session state

- Grid revision and authority peer ID.
- Running/stopped state.
- BPM/pattern/mode.
- Authority epoch and projection reference.
- Arrangement/transport revisions.

#### Local tuning

- Device and channels.
- Device buffer size.
- Capture/playback ring capacities.
- Jitter/adaptive/drift bounds.
- Socket buffers and OS priority.
- Local/remote gains and peer mutes.
- Logging and CSV paths.

Do not continue treating all creator GUI settings as mandatory leader settings.

### Grid authority flow

1. Any GUI submits a start-grid request with its stable peer ID.
2. The bootstrap coordinator orders competing requests.
3. It assigns the next grid revision and accepted authority peer ID.
4. The authority chooses a measurable future epoch with sufficient RTT lead.
5. The accepted state is broadcast to all peers.
6. The authority sends ongoing UDP grid state directly to all peers.
7. Followers map the authority epoch through that authority's `PeerStream`.
8. Stale revisions are ignored.

### Tasks

- `[ ]` Remove permanent `listener_side` timing decisions from the packet engine.
- `[ ]` Remove fixed process-start `grid_coordinator` behavior.
- `[ ]` Define versioned `GridState` with authority peer ID.
- `[ ]` Implement coordinator ordering and rebroadcast of grid changes.
- `[ ]` Authorize proposal versus accepted-snapshot message families from the authenticated connection role; never trust a payload-claimed source peer.
- `[ ]` Implement shared-grid mapping and bounded correction relative to the authority stream.
- `[ ]` Implement leader-audio injection by exactly the authority peer.
- `[ ]` Suppress follower-local click in leader-audio mode.
- `[ ]` Reframe listener compensation as non-authority compensation relative to the authority's mapped playback stream.
- `[ ]` Expose stale/missing authority-reference state.
- `[ ]` Define explicit authority-disconnect behavior; default to `authority_missing` and user-initiated takeover rather than hidden election.
- `[ ]` Add source peer ID, globally unambiguous event ID, and grid revision to transport actions.
- `[ ]` Reject stale, duplicate, wrong-authority, and excessive-future grid/transport revisions before scheduling or model mutation.
- `[ ]` Define new-peer alignment to a future bar without resetting existing peers.

### Validation matrix

- Creator starts each metronome mode.
- First joiner starts each mode.
- Third/fourth peer starts each mode.
- Concurrent start requests resolve to one revision and authority.
- BPM/pattern changes create a clean future revision.
- New peer joins a running grid at a future bar.
- Shared-grid phase correction stays within configured limits.
- Leader-audio click exists in exactly one outgoing stream.
- Muting the leader peer has documented/visible click consequences.
- Authority disconnect exposes missing state and explicit takeover works.
- Transport actions from multiple sources cannot collide or apply twice.
- Authenticated non-coordinator sends coordinator snapshot; authenticated non-authority sends authority-only update; both are rejected without partial state change.
- Forged payload source ID cannot override the connection-bound peer ID.

### Exit gate

- `[ ]` Network topology no longer determines musical authority.
- `[ ]` All GUIs converge on one grid revision/authority.
- `[ ]` Metronome/transport raw timing data demonstrates bounded mapping and correction.
- `[ ]` Leader-audio behavior works for any selected peer in two-, three-, and four-peer sessions.
- `[ ]` Every accepted musical-state mutation has a valid authenticated source role and monotonic revision/event identity.

## Phase 7: GUI Local/Network Lifecycle

### Objective

Make the application operate locally by default and attach/detach the proven network session through Start Jam, Join Jam, and Leave Jam.

### Lifecycle

#### Application start

- UI and local engine can exist before a device is active.
- Starting the device begins the authoritative callback frame clock.
- Local metronome, tracks, recording, and controls work with no UDP socket.

#### Start Jam

- Use the active local sample rate to form the immutable session contract.
- Bind UDP and perform manual/STUN endpoint discovery.
- Create session ID/key, local peer ID, coordinator control state, and invite.
- Attach `NetworkSession` with zero peers.
- Do not restart the audio device.

#### Join Jam

- Obtain the immutable contract before activating audio exchange.
- Compare sample rate/frame/format explicitly.
- If sample rate differs, require an explicit engine restart or reject while keeping the application/project alive.
- Bind/discover the local endpoint, join membership, and authenticate/probe every required direct edge.
- Mutually authenticate the coordinator control connection before accepting session/project state; never send the raw master key in a hello field.
- Add `PeerStream` objects as edges become active.

#### Leave Jam

- Stop network packetization.
- Send bounded best-effort departure state.
- Remove peers and drain/discard residual remote playback.
- Close network/control sockets.
- Keep local device, metronome, tracks, recording/project state, and application alive.

### Tasks

- `[ ]` Replace mode selection with local default plus Start Jam, Join Jam, and Leave Jam transitions.
- `[ ]` Remove the user-facing mesh checkbox after universal network parity.
- `[ ]` Stop calling `restartMeshEngineFromPeerList` for membership changes.
- `[ ]` Display coordinator, grid authority, arrangement authority, and per-edge states separately.
- `[ ]` Display immutable contract mismatches clearly.
- `[ ]` Move device probes, file reads/hashing, prepared-mix work, asset serialization, and other blocking GUI operations to bounded workers.
- `[ ]` Route peer control through the hardened framing/authentication/authorization/schema layer; typed local engine calls do not replace or bypass remote security.
- `[ ]` Move asset transfer out of `MainWindow` into the bounded requested-only streaming/temp/hash/WAV/atomic-commit service established in Phase 1.
- `[ ]` Display concise raw control/asset/endpoint rejection and capacity data without per-packet GUI logging.
- `[ ]` Keep all engine interaction typed and asynchronous.
- `[ ]` Define GUI behavior for engine/network worker failure and bounded shutdown.

### Validation matrix

- App/device start into local operation.
- Local metronome/track/recording before networking.
- Local -> create network session with zero peers.
- Local -> join session.
- Peer joins/leaves while local audio continues.
- Third/fourth peer changes without device restart.
- Leave -> local with no stale network audio.
- Join a different session without app restart.
- Explicit sample-rate mismatch restart/rejection.
- GUI remains responsive during device probes and large asset work.
- Fake coordinator/pre-auth/unauthorized control data cannot start a join, replace membership/settings, mutate project/grid state, or start an asset/file worker.
- Valid maximum-size asset transfer keeps GUI responsive and peak memory within the declared bound; every invalid/aborted transfer removes temporary state.

### Exit gate

- `[ ]` The normal UI exposes local and network lifecycle, not engine implementation modes.
- `[ ]` Start/join/leave and peer membership do not unnecessarily restart audio.
- `[ ]` GUI control does not block or enter the real-time layer.
- `[ ]` All required technical states remain visible.
- `[ ]` GUI lifecycle exposes no bypass around the Phase 1 control/asset/WAV security contract.

## Phase 8: Single-Binary Integration

### Objective

Host the proven engine in the same public executable as the GUI while preserving headless commands and failure isolation through threads and RAII.

The scenario-file, local reactive channel, secret-handoff, effective-configuration, and artifact contracts for the unified executable are defined in [refactor-python.md](refactor-python.md).

### Public command shape

The exact spelling can be finalized during implementation, but the intended shape is:

```text
jam2
jam2 local [options]
jam2 network create [options]
jam2 network join <jam2-url> [options]
jam2 network static --session-id ... --session-key ... --peers ...
```

- No arguments launches the GUI.
- Headless local and network operation remains public.
- `network static` supports deterministic direct stress/benchmark cases.

### Tasks

- `[ ]` Integrate `Engine` into the GUI executable through dedicated non-GUI threads.
- `[ ]` Replace child-process commands with typed `EngineCommand` submission.
- `[ ]` Replace stdout/JSONL/local TCP state with bounded events and snapshots.
- `[ ]` Preserve CLI/headless adapters over the same engine API.
- `[ ]` Remove child-process/session-key argument exposure and ensure unified startup/diagnostics never print the master key.
- `[ ]` Preserve bounded non-real-time worker containment for network-originated JSON, assets, WAV parsing, and prepared DSP after process-level fault isolation is removed.
- `[ ]` Catch exceptions at engine/network/worker/command top-level boundaries.
- `[ ]` Implement bounded cancellation and shutdown joins.
- `[ ]` Remove `QProcess` and second-binary staging only after behavior passes.
- `[ ]` Update packaging and platform startup behavior.

### Measurements

- Callback/network timing versus the pre-integration universal engine.
- GUI event-loop responsiveness.
- Whole-process CPU and memory.
- Command/event queue occupancy and overflow.
- Startup, network attach/detach, and shutdown time.
- Device/network/worker failure recovery.
- Malformed control/asset/WAV worker failure containment and secret-artifact scan.

### Exit gate

- `[ ]` GUI and every public headless command use the same engine implementation.
- `[ ]` No audio/timing regression versus Phase 7.
- `[ ]` GUI work cannot block the audio callback or network scheduler.
- `[ ]` Shutdown and failure paths are bounded and report clear errors.
- `[ ]` Single-binary integration does not weaken remote framing/authentication/authorization, file containment, or key handling.

## Phase 9: Compatibility Migration and Legacy Retirement

### Objective

Move tooling and documentation to the new lifecycle, then remove duplicate implementations only after accepted parity.

Follow the migration order and compatibility-removal gates in [refactor-python.md](refactor-python.md); retaining supported Python automation is an exit requirement, not optional cleanup.

### Tasks

- `[ ]` Temporarily map `listen` to network create.
- `[ ]` Temporarily map `connect` to network join.
- `[ ]` Temporarily map `mesh` to network static or equivalent peer-list testing.
- `[ ]` Migrate local stress tooling to the new commands and structured status.
- `[ ]` Migrate two-host benchmark tooling.
- `[ ]` Migrate connection diagnostics.
- `[ ]` Compare migrated tool results with preserved legacy baselines.
- `[ ]` Remove the old two-peer loop only after the one-peer/full-mesh replacement is accepted.
- `[ ]` Remove the experimental mesh loop only after multi-peer acceptance.
- `[ ]` Remove obsolete local TCP/stdin/JSONL/process-control compatibility.
- `[ ]` Remove every legacy peer-control path that can dispatch pre-authentication data, use unbounded newline framing, transmit the raw key, or omit source authorization.
- `[ ]` Update stress/benchmark tools to use non-secret run/session identifiers and verify keys are absent from persisted artifacts.
- `[ ]` Update all user and technical documentation.
- `[ ]` Document the v1 trust boundary accurately: direct traffic is authenticated/bounded but not encrypted, and invited peers share session trust.
- `[ ]` Decide with the user when command aliases can be removed.

### Exit gate

- `[ ]` No supported benchmark, diagnostic, GUI, or headless workflow depends on the old loops.
- `[ ]` Replacement results remain traceable to Phase 0 baselines.
- `[ ]` Duplicate mode implementations and process-boundary compatibility code are gone.
- `[ ]` No supported tool or application workflow depends on the retired unsafe framing/authentication/key-exposure paths.

## Phase 10: Optional Measured Wire Experiments

### Objective

Evaluate wire changes only after the architecture is stable and only when final measurements show a concrete need.

These experiments are not required for the core refactor.

### PCM16 experiment

- `[ ]` Add PCM16 as an explicit matched-peer format, not a silent fallback.
- `[ ]` Include network sample format in the immutable handshake.
- `[ ]` Record bytes/sample, payload size, bitrate, and format in stats.
- `[ ]` Compare instrument/tone recordings and directional audio quality.
- `[ ]` Compare Wi-Fi airtime-related behavior, burst pressure, jitter, late packets, and underruns.

Expected bandwidth reduction from the review is approximately 24-30% depending on packet profile. It does not reduce propagation time or packet frequency.

### Smaller UDP header experiment

- `[ ]` Define a new explicit protocol version with a small fixed common prefix and fixed type-specific fields.
- `[ ]` Retain explicit byte encoding; never send native structs.
- `[ ]` Add golden bytes and strict version rejection.
- `[ ]` Compare bandwidth and packet CPU against v1.

The estimated 48-byte to 32-byte header saving is roughly 2-6% depending on frame size. Do not adopt it without measured value exceeding compatibility and complexity cost.

### Selective control/asset binary framing

- `[ ]` Replace base64 asset payloads with bounded raw framed chunks if transfer measurements justify it.
- `[ ]` Consider fixed binary messages only for frequent, fixed-shape membership, clock, metronome, or transport data.
- `[ ]` Keep readable human-paced song/arrangement metadata unless profiling demonstrates a problem.
- `[ ]` Version and strictly bound every framing format; raw/binary payloads retain authenticated source, sequence, authorization, and transfer-state checks.

Media/control encryption or pairwise peer keys are separate threat-model experiments, not implied by binary framing. Do not add them without explicit user approval and independent CPU, bandwidth, join-time, fan-out, recovery, and interoperability measurements.

### Exit gate

- `[ ]` Each accepted experiment has independent before/after raw data.
- `[ ]` Peers fail clearly on unsupported formats/versions.
- `[ ]` No experiment weakens authentication, diagnostics, or timing behavior.
- `[ ]` Optional raw asset framing preserves all Phase 1 size/chunk/time/request/hash/temp/atomic-commit protections.

## Cross-Phase Acceptance Checklist

Before declaring the overall refactor complete:

- `[ ]` Two-person direct sessions remain the simplest and best-tested path.
- `[ ]` Two-person audio behavior derives from the mature `listen/connect` implementation.
- `[ ]` The current experimental mesh receive/timing loop did not replace mature behavior.
- `[ ]` Three/four peers use the same `PeerStream` implementation as two peers.
- `[ ]` Every remote peer has independent clock mapping and drift correction before mix.
- `[ ]` Mix and packet storage is bounded and exposes occupancy/capacity drops.
- `[ ]` Local operation requires no UDP socket.
- `[ ]` Start/join/leave and peer changes do not unnecessarily restart the audio device.
- `[ ]` Any peer can become grid authority by starting the metronome.
- `[ ]` Leader-audio has exactly one click source.
- `[ ]` Coordinator, grid authority, and arrangement authority are distinct and visible.
- `[ ]` Direct edge/NAT failures are reported per peer without relay fallback.
- `[ ]` GUI and headless commands share one engine implementation and one executable.
- `[ ]` The retained Python suites use the bounded scenario/debug interface in [refactor-python.md](refactor-python.md) and still cover local, create, join, static mesh, real-device, and headless workflows.
- `[ ]` Real-time callback rules are satisfied.
- `[ ]` All queues, transfers, and packet horizons are bounded.
- `[ ]` TCP admission, framing, authentication, sequence, receive/output queues, deadlines, and per-turn work are bounded and reason-counted.
- `[ ]` No application message reaches models, files, workers, or the engine before authentication and source/message-family authorization.
- `[ ]` Remote values pass strict centralized schema/count/string/numeric checks before allocation or narrowing.
- `[ ]` Assets are requested/hash-addressed, streamed, incrementally verified, strictly parsed, and atomically committed beneath a fixed local cache root.
- `[ ]` UDP replay, huge gaps, sample-time horizons, ping correlation, per-wake work, and endpoint proof use fixed visible state.
- `[ ]` No audio is sent to an endpoint before authenticated observed-source proof.
- `[ ]` Session secrets come from the OS CSPRNG and are absent from routine logs, CSV, benchmark metadata, errors, and process arguments.
- `[ ]` Documentation makes no v1 confidentiality or malicious-invited-peer-isolation claim.
- `[ ]` Raw technical stats/CSV remain sufficient for run comparison.
- `[ ]` Legacy paths are removed only after accepted replacements exist.
- `[ ]` Optional wire changes were measured separately.

## Work Log Template

Append concise entries here as work progresses. Do not erase earlier evidence.

```text
Date:
Phase/task:
Working-tree or revision identity:
Files changed:
Behavior intentionally changed:
Behavior intentionally preserved:
Protocol/security versions and limits:
Tests added/updated:
Build performed by:
Build result/artifact:
Runtime scenarios:
Raw result paths:
Before/after measurements:
Security rejection/capacity/fuzz/sanitizer evidence:
Known limitations:
Gate status:
Next task:
```

## Decision Log

Record decisions that affect later phases so they are not repeatedly inferred from code.

| Date | Phase | Decision | Reason/evidence | Consequence |
| --- | --- | --- | --- | --- |
| 2026-07-13 | Plan | Use mature `listen/connect` behavior as one-peer parity baseline | It contains the complete reorder, jitter, playout, adaptive, drift, timing, priority, and leader-audio behavior | Current mesh is not promoted unchanged |
| 2026-07-13 | Plan | Use persistent local engine plus optional full-mesh `NetworkSession` | Simplifies lifecycle and makes two peers the smallest ordinary mesh | Listen/connect remain bootstrap concepts only |
| 2026-07-13 | Plan | Correct each peer clock before mixing | Remote devices drift independently | One aggregate post-mix resampler is not acceptable |
| 2026-07-13 | Plan | Let the metronome initiator own that grid revision | Musical authority should follow explicit GUI action rather than topology | Requires versioned authority and coordinator request ordering |
| 2026-07-13 | Plan | Defer wire-format experiments | Architecture, timing, and wire changes must remain independently measurable | PCM24 and the current fixed header remain during core refactor |
| 2026-07-13 | Security | Use lightweight bounded authenticated control rather than TLS/accounts | Sessions are short-lived, direct, and among controlled peers; resource and state containment has the highest value | Add fixed framing, challenge-response, frame MAC/sequence, source authorization, and strict schemas without a security platform |
| 2026-07-13 | Security | Treat v1 confidentiality and malicious invited-peer isolation as out of scope | Audio/control/assets are direct and peers share the invite key | Document plaintext traffic; defer AEAD, certificates, and pairwise public-key identity unless the threat model changes |
| 2026-07-13 | Security | Require observed-source endpoint proof before audio activation | A control-advertised endpoint can otherwise redirect continuous UDP toward a third party | Candidate probes are bounded and add join time only; active audio cost is unchanged |
| 2026-07-13 | Security | Keep transferred media hash-addressed and accept only narrow validated PCM16 WAV | Remote paths and broad/unbounded file parsing add avoidable attack and resource surface | Stream to temp, validate/hash, atomically commit, and fuzz the parser outside real-time work |

## Resume Instructions for an Implementation Agent

When this document is provided in a later task:

1. Read [AGENTS.md](AGENTS.md) and all governing refactor documents, including [refactor-security.md](refactor-security.md) and [refactor-python.md](refactor-python.md).
2. Inspect the current worktree; do not assume this status section is newer than the code.
3. Find the first incomplete phase and read its entry conditions, source areas, tasks, measurements, and exit gate.
4. Check the work log and decision log for newer evidence.
5. Confirm whether the user requested implementation, diagnosis, documentation, build, or runtime validation.
6. Work only as far as the authorized phase/task and preserve unrelated changes.
7. Keep code-complete and validated-complete states distinct.
8. Do not compile unless the user explicitly requests it; otherwise provide exact validation work that remains for the user.
9. Update task markers, the current-status section, work log, and decision log when material work or evidence is completed.
10. Stop at every hard gate, especially one-peer `PeerStream` parity, and present raw comparison evidence before proceeding.
11. Treat every network-originated length, count, numeric value, source identity, endpoint, timeline value, hash, and file as untrusted until the phase-specific bounded validator accepts it.

## Recommended First Implementation Task

Begin with Phase 0, not engine extraction:

1. Establish native CTest scaffolding.
2. Add protocol golden/malformed tests.
3. Add ring ownership and sequence-wrap tests.
4. Record current control/security gaps, define the Phase 1 golden/boundary vectors, and add the available asset/WAV/UDP-abuse fuzz harness seams without changing runtime behavior.
5. Prepare the existing Python tools to record a clearly identified current `listen/connect` baseline without persisting the session key.
6. Ask the user to compile/run the agreed baseline and return the artifacts.

The first production corrections after that evidence exists should be playback-ring ownership and the TCP pre-authentication/framing/admission bounds, implemented as separate measured changes. The ring issue affects mature-path comparison reliability; the control issues are immediate network-exposure correctness and resource bounds that should exist before engine extraction.
