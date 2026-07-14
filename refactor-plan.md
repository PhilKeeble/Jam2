# Jam2 Refactor Plan

## Purpose

This is the dependency-ordered implementation plan for completing the Jam2 v1
refactor. It groups related work so the current implementation is corrected and
made measurable before its behavior is moved into the final architecture.

The detailed findings, rationale, and validation matrices remain in:

- [AGENTS.md](AGENTS.md), which takes precedence over this plan.
- [refactor-efficiency.md](refactor-efficiency.md).
- [refactor-security.md](refactor-security.md).
- [refactor-modes.md](refactor-modes.md).
- [refactor-binaries.md](refactor-binaries.md).
- [refactor-python.md](refactor-python.md).
- [PLAN.md](PLAN.md).

## Work Rules

- Validate every phase with Python stress tests where practical and use focused
  manual checks only where Python is not useful.
- Keep the exact validation commands under each phase after they are run so the
  scenarios and saved artifacts remain available for regression comparisons.
  Device stress uses IDs `5` and `16` at 44100 Hz. Refactor artifacts use
  top-level folders such as `tools\stress_logs_phase2_udp` or
  `tools\stress_logs_phase2_mesh`, never subfolders of the normal
  `tools\stress_logs` directory.
- At the end of every phase, compile from the repository root with
  `cmd.exe /d /c "call compile.cmd --in-dev-shell"`, then run the relevant
  Python stress scenarios and inspect their saved metrics. Fix failures or
  regressions and repeat compilation and affected scenarios until validation
  passes. Then mark the phase complete and report what changed, the validation
  outcome, and whether any focused manual testing is still needed from the user.
- Preserve existing benchmark artifacts as historical protocol and behavior
  evidence. Re-run representative cases when they are useful; do not require a
  new exhaustive baseline before correcting known defects.
- Do not add CMake/CTest test scaffolding. Python is the primary process-level
  validation, impairment, measurement, and artifact-analysis layer.
- Keep UDP v1 bytes, PCM24, packet cadence, and configured latency behavior
  stable until the optional protocol-experiment phase.
- Keep architectural, timing, and wire-format changes isolated from one another.
- Keep raw measurements and explicit failure reasons. Do not add subjective
  playability scores or hidden recommendations.

Status markers:

- `[ ]` not started.
- `[~]` in progress.
- `[x]` implementation item complete; at phase level, compilation and required
  automated validation have also passed.
- `[v]` phase implementation complete; compilation or validation remains pending.
- `[!]` blocked, with the reason recorded in the work log.

## Current Status

Last plan update: 2026-07-14.

- `[x]` Phase 1: stabilize and improve the current model.
- `[x]` Phase 2: extract the persistent local engine.
- `[x]` Phase 3: extract the mature one-peer network path.
- `[x]` Phase 4: generalize to universal direct full mesh.
- `[x]` Phase 5: timing, metronome, and transport authority.
- `[ ]` Phase 6: GUI lifecycle and single-application integration.
- `[ ]` Phase 7: tooling migration and legacy retirement.
- `[ ]` Phase 8: optional measured protocol experiments.

Current implementation focus: Phase 6 GUI lifecycle and single-application integration.

## Product and Architecture Invariants

1. Two people connecting directly remains the primary and best-tested workflow.
2. Three/four-person sessions remain direct full mesh. No relay or TURN audio
   path is introduced; STUN remains endpoint discovery only.
3. The current mature `listen/connect` path is the behavioral source for the
   future one-peer `PeerStream`. The current experimental mesh receive/mix loop
   is not promoted as the replacement engine.
4. Every remote audio peer is an independent clock domain and is corrected
   before mixing.
5. The audio callback does not allocate, log, throw, lock, block, perform file
   I/O, parse network input, or call GUI code.
6. Network, audio, control, model, asset, file, and mix storage is bounded and
   exposes capacity, high-water, rejection, and drop behavior.
7. Raw CPU, bandwidth, packet-rate, timing, buffering, loss, jitter, drift,
   resampler, and xrun measurements remain visible.
8. Application control is dispatched only after authentication and explicit
   source/message-family authorization.
9. Remote assets are hash-addressed. Remote metadata never selects a local
   filesystem path.
10. V1 traffic is not claimed to be confidential; invited peers share the
    session trust boundary.

## Phase 1: Stabilize and Improve the Current Model

Complete the identified work against the current mature `listen/connect`
implementation before extracting it.

### Correctness and ownership

- `[x]` Preserve a single playback-ring read-index owner using a bounded
  producer-request/consumer-apply drop handoff.
- `[x]` Expose requested, applied, coalesced, pending, and maximum-batch drop
  measurements.
- `[x]` Make sequence handling wrap-safe in every receive path.
- `[x]` Add fixed replay, bounded forward-gap, sample-time-horizon,
  ping-correlation, and per-wake packet-work behavior.
- `[x]` Replace expected invalid-datagram exceptions with explicit parse results
  and reason-specific counters.
- `[x]` Correct `UdpSocket` move-assignment lifetime handling.

### Control and network security

- `[x]` Replace newline accumulation with bounded length-prefixed control
  framing, connection limits, deadlines, output high-water marks, and bounded
  work per event-loop turn.
- `[x]` Implement mutual challenge-response without transmitting the master
  session key.
- `[x]` Authenticate sequenced control frames and bind dispatch to authenticated
  connection identity.
- `[x]` Apply explicit authorization by message family and source role.
- `[x]` Validate all remote JSON types, counts, strings, numeric values,
  revisions, and checked arithmetic before mutation.
- `[x]` Generate keys, peer tokens, and nonces with OS-backed randomness and
  remove secrets from routine stored artifacts.

### Assets and remote files

- `[x]` Make transfers requested-only, hash-addressed, bounded, streamed, and
  incrementally verified.
- `[x]` Commit only validated assets atomically from same-directory temporary
  files and clean partial state on every failure.
- `[x]` Ignore remote filesystem paths and derive cache paths locally from
  validated hashes.
- `[x]` Isolate a strict bounded PCM16 RIFF/WAVE parser.
- `[x]` Keep file, hash, parsing, stretch, and prepared-mix work on bounded
  non-real-time workers.

### UDP and fast-path efficiency

- `[x]` Validate exact UDP type, flags, reserved bytes, session,
  authentication, and type-specific payload size before dispatch.
- `[x]` Resolve endpoints once and retain numeric endpoint identities.
- `[x]` Use caller-owned fixed UDP receive/transmit and PCM storage.
- `[x]` Authenticate without copying the complete packet.
- `[x]` Pack and decode PCM24 directly into bounded storage.
- `[x]` Replace mature-path dynamic reorder/jitter storage with fixed-capacity
  indexed storage and visible occupancy/capacity drops.
- `[x]` Use one deadline-aware network wait that rechecks send deadlines during
  receive bursts.
- `[x]` Bulk-copy ring spans where ownership permits.
- `[x]` Preserve UDP v1 wire bytes and configured latency behavior.

### Compatibility mesh containment

- `[x]` Enforce peer caps at the connection boundary and close rejected peers.
- `[x]` Require bounded authenticated endpoint proof before activating an
  advertised endpoint.
- `[x]` Bound current pending-mix storage and reject stale or excessive-future
  values.
- `[x]` Do not add mature jitter, drift, resampling, or timing features to the
  compatibility mesh engine.

### Python validation suite

- `[x]` Separate scenario definitions, orchestration, impairment, result
  processing, and artifact handling into reusable modules while preserving
  current commands.
- `[x]` Treat native emitted effective configuration as authoritative rather
  than duplicating profile defaults in Python.
- `[x]` Add deterministic duplication, corruption, replay, malformed traffic,
  near-wrap sequences, extreme timestamps, floods, and per-direction
  impairment.
- `[x]` Keep versioned, secret-free manifests and structured raw results with
  exact settings, seeds, peer sides, artifacts, and technical failure reasons.
- `[x]` Preserve existing local-stress and two-host benchmark commands while
  preparing adapters for the unified executable.

## Phase 2: Extract the Persistent Local Engine

- `[x]` Create one Qt-free `Engine` owning audio-device lifecycle, callback frame
  clock, local capture/playback handoffs, monitoring, metronome, transport,
  prepared tracks, recording, and technical statistics.
- `[x]` Define typed immutable configuration, bounded commands, fixed-shape
  snapshots, bounded events, stop, and join interfaces.
- `[x]` Make local operation the base engine state and networking an optional
  attachment.
- `[x]` Tag or epoch capture data against the authoritative callback frame
  clock; discard stale capture when networking attaches.
- `[x]` Keep current CLI/headless modes working as adapters over the extracted
  engine.
- `[x]` Preserve real-device and headless operation.

### Validation commands

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --scenario clean-control --stream-ms 10000 --logs tools\stress_logs_phase2_udp --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 2 --stream-ms 10000 --logs tools\stress_logs_phase2_mesh --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile moderate --scenario clean-control --scenario metronome-shared-grid --stream-ms 10000 --logs tools\stress_logs_phase2_udp_headless --clean
```

## Phase 3: Extract the Mature One-Peer Network Path

- `[x]` Create `PeerStream` from the corrected `listen/connect` reorder, jitter,
  playout, missing-frame, drift, resampling, RTT, and statistics behavior.
- `[x]` Create `NetworkSession` for the UDP socket, packet scheduling,
  control/bootstrap state, peer identity, and immutable session contract.
- `[x]` Attach one `PeerStream` for the normal two-person case.
- `[x]` Separate creator/joiner bootstrap concepts from steady-state audio-peer
  behavior.
- `[x]` Keep `listen` and `connect` as compatibility adapters.
- `[x]` Carry hardened framing, authentication, authorization, replay,
  endpoint, and bounded-storage components forward rather than reimplementing
  them.

Validation commands:

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --scenario clean-control --scenario jitter-50 --scenario loss-0.5 --scenario reorder-small --scenario duplicate-2.0 --scenario near-wrap-sequence --scenario delayed-replay --scenario forward-sequence-gap --scenario extreme-sample-time --scenario metronome-shared-grid --scenario runtime-controls --stream-ms 18000 --logs tools\stress_logs_phase3_udp --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile moderate --scenario clean-control --scenario metronome-shared-grid --scenario metronome-leader-audio --stream-ms 16000 --include-audio-probes --logs tools\stress_logs_phase3_udp_headless --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile moderate --scenario metronome-leader-audio --scenario audio-probe-jitter-buffer-512-metronome --scenario audio-probe-prefill-768-metronome --stream-ms 16000 --logs tools\stress_logs_phase3_udp_headless_recheck --clean
```

## Phase 4: Generalize to Universal Direct Full Mesh

- `[x]` Use the same `NetworkSession` and `PeerStream` implementation for one or
  many remote peers.
- `[x]` Encode local audio once and fan out the packet to each active direct
  peer.
- `[x]` Give each peer independent reorder, jitter, clock mapping, drift,
  resampling, and raw statistics.
- `[x]` Map each peer onto the local frame timeline before mixing.
- `[x]` Add bounded fixed-slot mixing with peer-contribution tracking,
  missing-peer silence, deadline release, wide accumulation, and one saturation
  step.
- `[x]` Support peer add/remove/endpoint changes without restarting the audio
  device or unaffected streams.
- `[x]` Retire the old mesh audio loop after the universal path replaces its
  useful topology behavior.

Validation commands:

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --scenario clean-control --scenario jitter-50 --scenario loss-0.5 --scenario reorder-small --scenario runtime-controls --stream-ms 12000 --logs tools\stress_logs_phase4_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 2 --mesh-peers 3 --mesh-peers 4 --stream-ms 16000 --logs tools\stress_logs_phase4_mesh --clean
```

## Phase 5: Timing, Metronome, and Transport Authority

- `[x]` Separate bootstrap coordinator, audio peer, grid authority, and
  arrangement authority.
- `[x]` Give each grid an authority peer, revision, mapped epoch, and explicit
  state.
- `[x]` Let the peer initiating a grid revision become its authority.
- `[x]` Ensure `leader-audio` click injection comes from exactly that authority
  peer.
- `[x]` Map shared-grid and listener-compensated behavior relative to the
  authority stream.
- `[x]` Resolve concurrent proposals through ordered revisions and support
  joining a running grid at a safe musical boundary.
- `[x]` Make transport actions source-identified, revisioned, and scheduled
  against the engine frame clock.

Validation commands:

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --scenario metronome-shared-grid --scenario metronome-leader-audio --scenario metronome-listener-compensated --scenario grid-authority-client-shared-grid --scenario grid-authority-client-leader-audio --scenario grid-authority-client-listener-compensated --scenario grid-authority-concurrent --scenario transport-grid-authority --scenario runtime-controls --stream-ms 12000 --logs tools\stress_logs_phase5_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 2 --mesh-peers 3 --mesh-peers 4 --scenario mesh-2-clean --scenario mesh-3-authority-last --scenario mesh-4-authority-last --stream-ms 12000 --logs tools\stress_logs_phase5_mesh --clean
```

## Phase 6: GUI Lifecycle and Single-Application Integration

- `[ ]` Integrate the persistent engine directly through typed commands, events,
  and snapshots.
- `[ ]` Make no-argument `jam2` launch the GUI while documented subcommands keep
  headless and diagnostic operation.
- `[ ]` Implement Local, Start Jam, Join Jam, Leave Jam, reconnect, and explicit
  device-restart transitions without restarting the application.
- `[ ]` Remove mode-oriented GUI behavior and expose session, peer, authority,
  timing, and raw diagnostic state directly.
- `[ ]` Keep engine/network/audio workers independent of the Qt event thread.
- `[ ]` Remove child-process discovery/launch, stdin, loopback control, and JSONL
  state reconstruction once unused.
- `[ ]` Consolidate packaging around one public executable.

## Phase 7: Tooling Migration and Legacy Retirement

- `[ ]` Add a bounded explicit debug/headless adapter over the same `Engine` and
  `NetworkSession` used by the GUI.
- `[ ]` Add versioned declarative scenarios, effective-configuration output,
  structured events, artifact manifests, and frame-accurate scheduling where
  needed.
- `[ ]` Migrate Python from legacy command/output adapters to the unified
  interface.
- `[ ]` Generalize orchestration and result identities for two-, three-, and
  four-peer sessions, per-edge impairment, and independent headless drift.
- `[ ]` Add TCP control, authorization, model-boundary, asset-transfer, and WAV
  adversarial validation against real unified Jam2 processes; keep these as
  Python process scenarios rather than CMake/CTest targets.
- `[ ]` Remove duplicated legacy packet loops, obsolete compatibility control,
  stale command parsing, and stale documentation after migration.
- `[ ]` Retain useful device, connection, stress, benchmark, recording, and
  diagnostic workflows.

## Phase 8: Optional Measured Protocol Experiments

- `[ ]` Compare PCM16 and PCM24 only if measured bandwidth or mesh scaling data
  justifies it.
- `[ ]` Consider a smaller versioned UDP header only as a separate compatibility
  experiment.
- `[ ]` Consider raw binary asset chunks separately from the engine refactor.
- `[ ]` Consider encryption or stronger peer isolation only if the deployment
  threat model changes.
- `[ ]` Do not introduce relays, TURN audio, rooms, accounts, or production
  platform infrastructure.

## Work Log

Add concise entries as implementation proceeds:

### 2026-07-14 - Phase 5 timing, metronome, and transport authority

- Added coordinator-ordered grid revisions with endpoint-derived authority,
  explicit running/stopped/authority-missing state, safe-bar epoch mapping, and
  no automatic authority election.
- Made shared-grid and listener compensation follow only the assigned stream;
  leader-audio now renders and injects from exactly one assigned peer across
  one-peer and mesh sessions.
- Source-identified transport now carries its event counter and grid revision,
  maps requested targets onto the receiving engine clock, and remains controlled
  by the separate arrangement authority.
- Added authority, mapping, leader-injection, and transport frame statistics plus
  client/concurrent/third-peer/fourth-peer Python scenarios. MSVC built
  successfully; all nine UDP scenarios and all 2/3/4-peer mesh scenarios passed
  in the Phase 5 artifact folders above.

### 2026-07-14 - Phase 4 universal direct full mesh

- Generalized `NetworkSession` to stable multi-peer ownership, endpoint-gated
  lifecycle changes, one-time packet encoding, and direct fan-out with no peer
  count cap.
- Added bounded `PeerMixer` local-timeline queues, per-peer resampling and
  gain/mute controls, contribution/deadline tracking, missing-peer silence,
  wide accumulation, single saturation, shared adaptive/output control, and
  detailed per-peer/aggregate CSV statistics.
- Replaced the legacy mesh decode and `pending_mix` path with the same mature
  `PeerStream` used by one-peer sessions. Python now verifies distinct peer
  identities, all expected active edges, mixer contributors/output, and zero
  capacity/output drops for 2/3/4-peer sessions.
- MSVC/Ninja built successfully; the final build reported no work to do. The
  five-scenario headless UDP regression suite and all 2/3/4-peer mesh scenarios
  passed with artifacts in the Phase 4 folders above.

### 2026-07-14 - Phase 3 mature one-peer network path

- Added `PeerStream` with fixed-capacity reorder/jitter, sample-time playout,
  missing-frame handling, adaptive cushion, drift ratio, RTT, replay, horizons,
  and per-peer technical statistics.
- Added `NetworkSession` ownership for the UDP socket, immutable PCM24
  contract, stable compatibility peer IDs, endpoint gate, bootstrap metadata,
  and rational packet schedule.
- `listen` and `connect` now bootstrap as compatibility adapters and attach the
  same role-free one-peer stream to Engine capture/playback APIs. UDP v1 bytes,
  PCM24, packet cadence, and timing policy remain unchanged.
- Appended peer/session identity and contract fields to CSV. The MSVC/Ninja
  build succeeded, the 11-scenario UDP suite passed, and the targeted headless
  recheck passed 3/3 with valid identity/contracts and zero reorder/jitter
  capacity drops.

### 2026-07-14 — Phase 2 persistent local engine

- Added a Qt-free `jam2::Engine` in `jam2-core`. It owns real/headless audio
  stream lifetime, the callback frame clock, capture/playback rings, local
  monitoring, metronome and transport controls, prepared tracks, jam recording,
  track takes, and fixed-shape technical snapshots.
- Added validated retained startup configuration, bounded command/scheduled
  command/event storage with capacity and rejection/drop statistics, explicit
  stop/join lifecycle, and a non-real-time supervisor boundary that reports
  command and worker failures without adding work to audio callbacks.
- Made local audio the base state. Network capture/playback is an explicit
  generation-tagged attachment acknowledged at a callback boundary; stale
  capture is discarded there, its authoritative local frame epoch is exposed,
  and detached local operation neither fills the capture ring nor records
  artificial playback underruns.
- Moved the synthetic headless device into the core engine and made both real
  backends advance the authoritative frame clock once per callback, including
  input-only operation. The callback attachment check remains fixed-work,
  allocation-free, lock-free, and logging-free.
- Converted `local`, `listen`, `connect`, and compatibility `mesh` paths to
  legacy adapters over Engine-owned resources. The mature network algorithms,
  UDP v1 bytes, PCM24 payloads, packet cadence, and zero-based session packet
  sample-time behavior were left unchanged for Phase 3 extraction.
- Added capture attachment state, generation, epoch, stale-discard, and
  playback-attachment data to CLI diagnostics and structured status output;
  bounded Engine queue data is exposed in snapshots and final text statistics.
### 2026-07-13 — Phase 1 implementation pass

- Corrected playback-ring ownership, UDP parsing/sequence/replay/horizon/work
  bounds, fixed mature reorder/jitter storage, endpoint resolution, PCM24
  storage, and compatibility-mesh pending-mix containment without changing UDP
  v1 bytes.
- Replaced remote newline JSON control with bounded mutually authenticated,
  sequenced frames and source-aware dispatch; added message/model bounds and
  secret scrubbing.
- Reworked looper assets into requested-only paced streaming with incremental
  hashing, same-directory temporary files, strict PCM16 inspection, and atomic
  hash-addressed commit. Waveform reads now stream through the shared bounded
  WAV inspector.
- Added a bounded two-task GUI file executor. Prepared rendering is coalesced,
  memory-capped, strict-parser based, one-lane-at-a-time, and atomically
  streamed; imports, project open/save, metadata, previews, cache checks, and
  asset validation/commit run outside the GUI event thread.
- Added native mesh endpoint activation proof using bounded authenticated
  Ping/Pong challenges correlated to the observed candidate UDP source. Audio,
  grid, and transport traffic remain gated until proof succeeds, with explicit
  state and rejection counters.
- Added independent Python UDP v1 fixtures and deterministic malformed,
  duplicate, corrupt, replay, near-wrap, gap, timestamp, and flood scenarios;
  manifests/results record redacted native effective configuration.
- Split reusable scenario planning and result/audio verdict processing out of
  the runner while retaining the existing command surface.
- Kept real-process TCP/model/asset/WAV adversarial scenario expansion in the
  tooling-migration phase, where the validator can target the same unified
  executable rather than adding a second temporary automation surface.
- No build, compilation, or runtime validation was run, as required by
  `AGENTS.md`. Phase 1 is marked implementation-complete with user-run
  validation pending; broader final-architecture validation remains in Phase 7.

### 2026-07-13 — Local invitation output

- Keep the complete invitation URL and session key visible in native stdout and
  the GUI's local engine-output panel. These values are the deliberate manual
  handoff used to join a direct session; hiding them makes the primary workflow
  unusable.
- Redaction remains limited to Python manifests and other artifacts intended for
  comparison, archiving, or upload. Those artifacts are not invitation paths.

### 2026-07-13 — User validation follow-up

- Reviewed the two latest `release/logs` GUI CSVs. Across roughly 220 seconds
  both sides recorded zero sequence loss, playback drops, playback-ring
  underruns, late audio frames, reorder capacity drops, jitter capacity drops,
  and prepared-track underruns. The more timing-stressed side recorded five
  callback gaps above twice the expected interval at the 32-frame device
  setting, so device callback scheduling is the current lead for the reported
  light crackle; no speculative audio-path change was made.
- Reviewed the Phase 1 UDP stress artifacts. The two one-shot validation
  transforms fired after only eight observed audio packets, before both native
  receive paths were reliably active, so only one side recorded the expected
  rejection. Moved those transforms later while retaining the two-sided
  rejection requirement rather than weakening the verdict.
- Reviewed the Phase 1 mesh artifacts. The 3- and 4-peer cases failed when a
  hard-coded `50000` base selected Windows-excluded UDP port `50002`; mesh
  traffic on the surviving peers remained clean. Mesh stress now reserves
  available loopback ports automatically, while explicit ports are checked and
  fail with a direct bind error.
- Prevented a local metronome authority takeover from being overwritten by an
  in-flight remote UDP state packet. Receipt counters remain visible even when
  authoritative local state correctly ignores the remote update.
- Added bounded, source-aware recording contributions to Track Sync. A joiner's
  completed recording is offered by id/hash, requested and WAV/hash validated
  by the host, placed into the intended empty lane or appended on collision,
  and then included in the host-authoritative snapshot. An armed host lane is
  treated as reserved so simultaneous local/remote takes remain separate.
- No compilation or native/Python scenario execution was performed; the user
  remains the build and runtime validator under `AGENTS.md`.

### 2026-07-13 - Python validation verdict tightening

- Split real-process stress decisions into protocol correctness, effective run
  duration, and audio health. The overall verdict now fails on any applicable
  decision while retaining every audio-health failure reason and the raw
  counters in JSON/CSV.
- Made clean transport-validation cases reject capacity drops, playback
  overruns, dropped/late/missing frames, jitter-buffer drops, and underruns.
  Callback gaps and bounded forced jitter releases remain explicit technical
  observations without claiming an audible discontinuity on their own. The
  deliberate gap and extreme timestamp cases allow only their two rejected
  audio packets' frame count.
- Added a configurable recovery interval between scenarios so consecutive
  real-device cases do not implicitly assume immediate driver reopen is clean.
- Added reusable headless duration and callback checks. Headless runs use an
  explicit 1024-frame synthetic callback by default so wall-clock pacing is not
  based on sub-millisecond sleeps under ordinary Windows timer scheduling; the
  selected profile still controls the network frame size.
- Kept mesh validation limited to evidence that survives the universal-engine
  refactor: per-process duration/recording coverage, callback timing, raw packet
  and rejection counters, capacity use, and audio continuity. No lasting
  contract was added for the current mesh startup topology or all-tone mix.
- Re-evaluated the user's existing artifacts without launching Jam2. All six
  UDP cases still passed their protocol-specific checks, but each now failed
  audio health for the previously hidden callback/continuity faults. Existing
  mesh recordings covered only about 4.9% of requested duration; the 3/4-peer
  runs also surfaced their stale-sample rejections instead of passing.
- Added twelve dependency-free Python tests covering split verdicts,
  duration coverage, deliberate rejection allowances, impairment-case scope,
  recording coverage, forced jitter release reporting, and stale-sample
  reporting, plus immediate/duplicate/delayed proxy scheduling. They pass
  directly under Python and are not registered with CMake/CTest.
- The user's stricter rerun showed all intended protocol checks passing, while
  the listener jitter queue repeatedly crossed 1,024 frames to 1,088 frames.
  Dropping the oldest valid packet created a sample-time hole, which playout
  replaced with silence before adaptive playback dropped the excess; this
  amplified one transient queue overflow into 74k-122k missing frames.
- Changed max-depth handling to force-release the oldest valid in-order jitter
  packet instead. This keeps the queue bounded without inventing a sample-time
  hole, and exposes `jitter_buffer_forced_releases` in native and Python
  artifacts so burst pressure remains measurable.
- The next user rerun confirmed zero unintended playback drops and zero
  unintended missing frames; the two deliberate rejection cases inserted
  exactly one 64-frame packet per side. It also isolated excessive listener-side
  forced releases to the harness's zero-delay proxy path, which queued every
  packet in a timed heap and could release a startup batch after native drain.
- Zero-delay, duplicate-only, corruption-only, and header-transform proxy
  traffic now forwards immediately in receive order. The timed heap remains for
  scenarios that actually request delay, jitter, reordering, or burst pauses.
- A clean proxy rerun then produced zero forced releases, jitter/capacity drops,
  playback drops, or unintended missing frames, with duplicate validation
  passing fully and proxy jitter maxima reduced to about 5 ms. The remaining
  one-shot gap/timestamp underruns were traced to the reorder layer waiting 18
  packets after an authenticated rejected packet.
- Rejected packets at the expected sequence now count that sequence as lost and
  advance immediately. A rejected implausible forward gap records a bounded
  one-packet recovery hint: the expected packet is still accepted if it arrives,
  otherwise the immediately following plausible sequence confirms the loss.
  This preserves ordinary reorder handling without imposing a 26 ms stall.
- The user's final focused rerun passed all six protocol, duration, and audio
  decisions. Clean, duplicate, delayed-replay, forward-gap, extreme-timestamp,
  and short-packet-flood cases all recorded zero underrun time, playback drops,
  late audio frames, forced jitter releases, and capacity drops.
- Forward-gap and extreme-timestamp injection each rejected exactly one packet
  per peer, inserted exactly one 64-frame silence span per peer, and recovered
  with a reorder high-water mark of zero. The 2,048 malformed short packets were
  all rejected without interrupting audio.
- Clean control recorded two callback intervals above twice the expected device
  period and delayed replay recorded four. These remain raw observations, not
  failures, because neither coincided with an underrun, drop, missing frame, or
  queue-capacity event.
- No native compilation or Jam2 scenario execution was performed.

```text
Date:
Phase/task:
Files changed:
Behavior changed:
Behavior preserved:
Python validation added/updated:
User-run build/runtime evidence:
Known limitations or follow-up:
```
