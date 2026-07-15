# Jam2 Refactor Plan

## Purpose

This is the authoritative dependency-ordered implementation and status plan for
completing the Jam2 v1 refactor. The supporting refactor documents provide
design rationale and technical context, but they do not define phases,
completion gates, or status.

The detailed findings, rationale, target behavior, and useful test context
remain in:

- [AGENTS.md](AGENTS.md), which takes precedence over this plan.
- [refactor-efficiency.md](refactor-efficiency.md).
- [refactor-security.md](refactor-security.md).
- [refactor-modes.md](refactor-modes.md).
- [refactor-binaries.md](refactor-binaries.md).
- [refactor-python.md](refactor-python.md).
- [PLAN.md](PLAN.md).

## Work Rules

- Compile substantive C++ changes and run relevant Python stress tests where
  they are useful. These checks provide engineering evidence, not hard numeric
  completion gates. Investigate obvious failures or regressions, then use
  focused manual checks for behavior that Python cannot judge usefully.
- Keep exact commands under the phase after they are run so the scenarios and
  saved artifacts remain available for historical regression comparisons.
  Device stress uses IDs `5` and `16` at 44100 Hz. Refactor artifacts use
  top-level folders such as `tools\stress_logs_phase2_udp` or
  `tools\stress_logs_phase2_mesh`, never subfolders of the normal
  `tools\stress_logs` directory.
- Compile from the repository root with
  `cmd.exe /d /c "call compile.cmd --in-dev-shell"`. Inspect the exit code,
  compiler output, relevant stress results, and historical logs before deciding
  that a change looks sound. Phase completion means its implementation work is
  present and reviewed; audible acceptance remains a focused user check.
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
- `[~]` implementation or mandatory closeout audit in progress.
- `[x]` implementation item complete and verified by the mandatory closeout
  audit; compilation or passing tests alone are not sufficient.
- `[!]` blocked, with the reason recorded in the work log.

## Mandatory Phase Closeout Procedure

Finishing an implementation pass is not the same as completing a phase. Use
the following procedure every time a phase appears finished:

1. Keep the phase under `Remaining Work` and describe it as implementation
   complete with closeout pending. Do not call it complete or move it to
   `Completed Work` yet.
2. Re-read `AGENTS.md`, the complete phase in this plan, every supporting
   refactor document listed under `Purpose`, and relevant work-log/manual-
   regression entries. Review each document one by one against the actual
   implementation, even if it was reviewed before implementation began.
3. Build a requirements-to-code audit from those documents. Inspect the real
   call paths, state and resource ownership, retained compatibility code,
   boundedness, statistics, and automation coverage. Passing tests do not prove
   an architectural or ownership requirement is complete.
4. Split any broad checkbox whose clauses have different completion states.
   Keep completed clauses checked, add every missing clause as a concrete
   unchecked item in the appropriate phase, and implement it. Nothing found in
   closeout may be silently omitted; an intentional deferral must name the
   later phase that owns it and explain the boundary in the work log.
5. Reconcile known manual bugs with the audited requirements. A known defect in
   behavior owned by the phase prevents completion until fixed or explicitly
   assigned to a later phase whose scope genuinely owns the fix.
6. Compile substantive C++ work with `compile.cmd`, run the relevant Python
   stress/validation scenarios, enhance durable automation where it can usefully
   cover the change, and inspect the resulting logs and measurements. Record
   exact commands and artifact locations under the phase.
7. Repeat the document and source audit after the missing work and defects are
   addressed. Confirm that replacement call sites are active and obsolete code
   has been removed before marking removal or decomposition work complete.
8. Only after this second audit may every satisfied item become `[x]`, the phase
   move to `Completed Work`, and the completion report say `Phase N complete`.
   The work log must summarize the per-document audit outcome, implementation,
   automated evidence, remaining focused manual checks, and any explicitly
   deferred work.

Before step 8, user-facing updates must say "implementation pass complete;
closeout audit pending", not "phase complete".

## Product and Architecture Invariants

1. Two people connecting directly remains the primary and best-tested workflow.
2. Three/four-person sessions remain the expected small-group direct full-mesh
   use case. Larger meshes have no application-wide peer cap and remain
   experimental. A creator may optionally set a session peer limit. Pending or
   failed authentication is bounded so invalid-key traffic cannot consume
   unbounded connection state.
3. No relay or TURN audio path is introduced; STUN remains endpoint discovery
   only.
4. The former mature `listen/connect` path was the behavioral source for the
   reusable one-peer `PeerStream`; the former experimental mesh receive/mix loop
   was not promoted as the replacement engine.
5. Every remote audio peer is an independent clock domain and is corrected
   before mixing.
6. The audio callback does not allocate, log, throw, lock, block, perform file
   I/O, parse network input, or call GUI code.
7. Network, audio, control, model, asset, file, and mix storage is bounded per
   message, queue, transfer, stream, or timeslot and exposes capacity,
   high-water, rejection, and drop behavior. Membership collections may grow
   only during non-real-time admission/removal and do not create a hidden peer
   cap.
8. Raw CPU, bandwidth, packet-rate, timing, buffering, loss, jitter, drift,
   resampler, and xrun measurements remain visible.
9. Application control is dispatched only after authentication and explicit
   source/message-family authorization.
10. Remote assets are hash-addressed. Remote metadata never selects a local
   filesystem path.
11. V1 traffic is intentionally not confidential. Authentication, integrity,
    and authorization protect session safety, but invited peers share the
    session trust boundary and must be trusted. Traffic encryption and pairwise
    cryptographic isolation are not refactor goals. Local command lines, logs,
    clipboard contents, and artifacts are outside the application threat
    boundary and may contain session keys or invite URLs.
12. One public `jam2` executable provides the primary GUI plus supported
    headless, diagnostic, and automation commands. Public network startup uses
    only `network create` and `network join`, both over the universal mesh path.

## Completed Work

Last plan reconciliation: 2026-07-15.

Historical status note: Phases 1-7 were marked complete before the mandatory
closeout procedure existed. Phase 11 must apply that procedure retroactively
and reopen any unsupported or partial item; these summaries do not waive the
final cross-document and source audit.

- `[x]` Phase 1: stabilize and improve the current model.
- `[x]` Phase 2: extract the persistent local engine.
- `[x]` Phase 3: extract the mature one-peer network path.
- `[x]` Phase 4: generalize to universal direct full mesh.
- `[x]` Phase 5: timing, metronome, and transport authority.
- `[x]` Phase 6: deliver the unified GUI/CLI executable and lifecycle.
- `[x]` Phase 7: expand unified-path automation and regression coverage.
- `[x]` Phase 8: complete the shared session controller and typed application
  boundary.

### Phase 1: Stabilize and Improve the Current Model

Complete the identified work against the current mature `listen/connect`
implementation before extracting it.

#### Correctness and ownership

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

#### Control and network security

- `[x]` Replace newline accumulation with bounded length-prefixed control
  framing, pending-connection limits, deadlines, output high-water marks, and
  bounded work per event-loop turn.
- `[x]` Implement mutual challenge-response without transmitting the master
  session key.
- `[x]` Authenticate sequenced control frames and bind dispatch to authenticated
  connection identity.
- `[x]` Apply explicit authorization by message family and source role.
- `[x]` Validate all remote JSON types, counts, strings, numeric values,
  revisions, and checked arithmetic before mutation.
- `[x]` Generate keys, peer tokens, and nonces with OS-backed randomness and
  retain explicit technical diagnostics around generated session state.

#### Assets and remote files

- `[x]` Make transfers requested-only, hash-addressed, bounded, streamed, and
  incrementally verified.
- `[x]` Commit only validated assets atomically from same-directory temporary
  files and clean partial state on every failure.
- `[x]` Ignore remote filesystem paths and derive cache paths locally from
  validated hashes.
- `[x]` Isolate a strict bounded PCM16 RIFF/WAVE parser.
- `[x]` Keep file, hash, parsing, stretch, and prepared-mix work on bounded
  non-real-time workers.

#### UDP and fast-path efficiency

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

#### Compatibility mesh containment

- `[x]` Bound pending unauthenticated connections and close failed
  authentication attempts.
- `[x]` Require bounded authenticated endpoint proof before activating an
  advertised endpoint.
- `[x]` Bound current pending-mix storage and reject stale or excessive-future
  values.
- `[x]` Do not add mature jitter, drift, resampling, or timing features to the
  compatibility mesh engine.

#### Python validation suite

- `[x]` Separate scenario definitions, orchestration, impairment, result
  processing, and artifact handling into reusable modules while preserving
  current commands.
- `[x]` Emit effective native configuration and compare it with Python profile
  expectations so drift is visible.
- `[x]` Add deterministic duplication, corruption, replay, malformed traffic,
  near-wrap sequences, extreme timestamps, floods, and per-direction
  impairment.
- `[x]` Keep versioned manifests and structured raw results with exact settings,
  seeds, peer sides, artifacts, and technical failure reasons.
- `[x]` Preserve existing local-stress and two-host benchmark commands while
  preparing adapters for the unified executable.

### Phase 2: Extract the Persistent Local Engine

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

#### Historical validation commands

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --scenario clean-control --stream-ms 10000 --logs tools\stress_logs_phase2_udp --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 2 --stream-ms 10000 --logs tools\stress_logs_phase2_mesh --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile moderate --scenario clean-control --scenario metronome-shared-grid --stream-ms 10000 --logs tools\stress_logs_phase2_udp_headless --clean
```

### Phase 3: Extract the Mature One-Peer Network Path

- `[x]` Create `PeerStream` from the corrected `listen/connect` reorder, jitter,
  playout, missing-frame, drift, resampling, RTT, and statistics behavior.
- `[x]` Create `NetworkSession` for the UDP socket, packet scheduling, peer
  identity, and immutable session contract.
- `[x]` Attach one `PeerStream` for the normal two-person case.
- `[x]` Separate creator/joiner bootstrap concepts from steady-state audio-peer
  behavior.
- `[x]` Keep `listen` and `connect` as temporary extraction adapters, then
  retire their public commands and duplicate packet loops once the unified
  create/join path is exercised.
- `[x]` Carry hardened framing, authentication, authorization, replay,
  endpoint, and bounded-storage components forward rather than reimplementing
  them.

#### Historical validation commands

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --scenario clean-control --scenario jitter-50 --scenario loss-0.5 --scenario reorder-small --scenario duplicate-2.0 --scenario near-wrap-sequence --scenario delayed-replay --scenario forward-sequence-gap --scenario extreme-sample-time --scenario metronome-shared-grid --scenario runtime-controls --stream-ms 18000 --logs tools\stress_logs_phase3_udp --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile moderate --scenario clean-control --scenario metronome-shared-grid --scenario metronome-leader-audio --stream-ms 16000 --include-audio-probes --logs tools\stress_logs_phase3_udp_headless --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile moderate --scenario metronome-leader-audio --scenario audio-probe-jitter-buffer-512-metronome --scenario audio-probe-prefill-768-metronome --stream-ms 16000 --logs tools\stress_logs_phase3_udp_headless_recheck --clean
```

### Phase 4: Generalize to Universal Direct Full Mesh

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

#### Historical validation commands

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --scenario clean-control --scenario jitter-50 --scenario loss-0.5 --scenario reorder-small --scenario runtime-controls --stream-ms 12000 --logs tools\stress_logs_phase4_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 2 --mesh-peers 3 --mesh-peers 4 --stream-ms 16000 --logs tools\stress_logs_phase4_mesh --clean
```

### Phase 5: Timing, Metronome, and Transport Authority

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

#### Historical validation commands

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --scenario metronome-shared-grid --scenario metronome-leader-audio --scenario metronome-listener-compensated --scenario grid-authority-client-shared-grid --scenario grid-authority-client-leader-audio --scenario grid-authority-client-listener-compensated --scenario grid-authority-concurrent --scenario transport-grid-authority --scenario runtime-controls --stream-ms 12000 --logs tools\stress_logs_phase5_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --mesh-peers 2 --mesh-peers 3 --mesh-peers 4 --scenario mesh-2-clean --scenario mesh-3-authority-last --scenario mesh-4-authority-last --stream-ms 12000 --logs tools\stress_logs_phase5_mesh --clean
```

### Phase 6: Unified GUI/CLI Executable and Lifecycle

- `[x]` Make no-argument `jam2` launch the GUI while documented subcommands keep
  headless and diagnostic operation.
- `[x]` Implement Local, Start Jam, Join Jam, Leave Jam, reconnect, and explicit
  device-restart transitions without restarting the application.
- `[x]` Remove the user-facing mode selector and static mesh topology workflow.
- `[x]` Keep engine/network/audio workers independent of the Qt event thread.
- `[x]` Remove child-process execution while preserving the existing engine
  lifecycle during the first in-process integration.
- `[x]` Consolidate packaging around one public executable.

#### Historical validation commands

```powershell
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --scenario clean-control --scenario metronome-shared-grid --scenario runtime-controls --stream-ms 12000 --logs tools\stress_logs_phase6_udp_headless --clean
```

### Phase 7: Unified-Path Automation and Regression Coverage

- `[x]` Add bounded `debug describe` and `debug run` entry points to the unified
  executable.
- `[x]` Add versioned scenario identification, effective-configuration output,
  structured startup events, and retained result artifacts.
- `[x]` Generalize orchestration and result identities for two-, three-, and
  four-peer sessions, per-edge impairment, and independent headless drift.
- `[x]` Expand Python protocol, authorization, model, asset, WAV, UDP, authority,
  profile-equivalence, and recovery checks around real unified Jam2 behavior.
- `[x]` Retire the public static topology and the duplicate legacy audio packet
  loops after universal create/join and mesh validation.
- `[x]` Retain useful device, connection, stress, benchmark, recording, and
  diagnostic workflows.

#### Historical validation commands

```powershell
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --os-priority realtime --scenario clean-control --stream-ms 10000 --logs tools\stress_logs_phase7_udp_recovery_clean --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --scenario jitter-50 --scenario loss-0.5 --scenario reorder-small --scenario duplicate-2.0 --scenario near-wrap-sequence --scenario delayed-replay --scenario forward-sequence-gap --scenario extreme-sample-time --scenario malformed-udp --scenario udp-short-flood --scenario metronome-shared-grid --scenario runtime-controls --stream-ms 12000 --include-validation --validation-stream-ms 5000 --logs tools\stress_logs_phase7_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --os-priority realtime --mesh-peers 2 --mesh-peers 3 --mesh-peers 4 --scenario mesh-2-clean --scenario mesh-3-authority-last --scenario mesh-3-independent-drift --scenario mesh-4-authority-last --stream-ms 12000 --logs tools\stress_logs_phase7_mesh --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --os-priority realtime --mesh-peers 3 --scenario mesh-3-edge-jitter --stream-ms 12000 --logs tools\stress_logs_phase7_mesh_edge --clean
python tools\run_stress_local.py --server-audio-device 5 --client-audio-device 16 --sample-rate 44100 --profile fast --os-priority realtime --scenario transient-stall-recovery --stream-ms 22000 --logs tools\stress_logs_phase7_udp_recovery --clean
```

### Phase 8: Complete the Shared Session Controller and Typed Application Boundary

- `[x]` Add one shared non-UI session controller used by GUI, ordinary headless
  commands, and debug automation.
- `[x]` Keep `Engine` responsible for audio-device lifecycle, callback time,
  local media, controls, and technical snapshots; keep `NetworkSession`
  responsible for UDP peers, packet scheduling, and peer mixing; keep the
  shared controller responsible for TCP bootstrap, authentication, and
  membership.
- `[x]` Make `Engine::submit`, events, and snapshots the real application path
  rather than an unused interface beside compatibility accessors.
- `[x]` Support a creator-selected optional session peer limit and reject valid
  joins above that limit. Do not impose an application-wide authenticated-peer
  maximum.
- `[x]` Allow membership, mixer contribution metadata, and diagnostic peer
  views to grow only during non-real-time admission/removal. Keep every
  `PeerStream` and mix timeslot bounded while avoiding a compile-time peer-count
  ceiling.
- `[x]` Retain a small pending-unauthenticated cap, authentication deadlines,
  bounded failed-key work, and immediate cleanup of failed connections.
- `[x]` Publish coordinator and per-edge UDP proof/stream state through typed
  snapshots for direct GUI display.
- `[x]` Make one typed session lifecycle owner coordinate `ApplicationRuntime`
  and the TCP session controller for Local, Create, Join, Leave, reconnect,
  membership readiness, and network attachment. Remove duplicated lifecycle
  flags and transition policy from `MainWindow`.
- `[x]` Replace decisions based on human-readable transport status strings with
  typed lifecycle, transport, and failure events. Cover initial connection
  refusal, authentication failure, established disconnect, retry, and manual
  refresh through the same bounded reconnect state machine.
- `[x]` Consolidate the effective immutable session contract, including the
  current leader startup/settings handshake, under the shared controller so a
  join launch does not depend on a second GUI-owned contract and readiness
  state.
- `[x]` Make the shared controller the authority-coordination owner. Parse,
  validate, and apply grid and arrangement authority tokens and revisions from
  membership pages and routed updates; preserve the authenticated source
  identity and reject stale, malformed, or unauthorized revisions before
  mutation or rebroadcast.
- `[x]` Publish correct coordinator, grid authority, arrangement authority,
  revision, lifecycle, and failure state through typed snapshots on every peer,
  then add focused controller and real-process coverage for join-in-progress,
  non-coordinator authority changes, initial connect failure, disconnect, and
  reconnect.

#### Manual regressions to clear before Phase 8 completion

- `[x]` Make metronome start immediate and deterministic when requested; do not
  let a stale or future grid epoch from an earlier membership state delay it to
  a later bar. Cover starting with no remote peers and after a peer leaves.
- `[x]` Let any peer synchronize shared-track play, stop, and restart through a
  source-identified revisioned transport target. When Sync track controls is
  off, keep that peer's local controls private and disregard incoming peer
  track actions. Asset preparation must not cause peers to choose different
  later bars. Preserve each source's event sequence across leave/rejoin, and do
  not consume a repeated event until the re-established UDP edge can map it.
- `[x]` Restore recording count-in to show `WAITING FOR NEXT BAR...` until the
  next metronome bar, then count exactly the configured count-in beats in time
  with that grid (`4 3 2 1` for one 4/4 bar) and begin capture at the following
  boundary.
- `[x]` Show only `Remote Peers X` in the top-right status, counting remote
  authenticated TCP members whose UDP edge is active. Keep membership, grid,
  arrangement, and per-edge detail in typed snapshots and technical logs, and
  do not repeatedly log an unchanged session snapshot on periodic updates.
- `[x]` Preserve complete fixed-size network capture packets when the device
  callback is smaller than the packet, and establish a bounded sender-time
  baseline on late network attachment/rejoin. Confirm on real 32-frame ASIO
  devices that remote audio no longer becomes sparse/bitcrushed after rejoin.
- `[x]` When the adaptive playback target releases after a transient latency
  increase, actually return buffered playback depth to the target through the
  bounded drop handoff and expose the requested/applied correction. Add a
  transient-spike recovery case that checks the queue settles again.
- `[x]` On last-peer departure, retain or leave the creator session according
  to the selected lifecycle while clearing departed-peer stream and stale grid
  scheduling state so local metronome and transport actions do not wait on the
  old mesh.
- `[x]` Keep manually stopped recorded takes bar-aligned by scheduling Stop at
  the next whole bar. Present a continuous transport-anchored position in the
  Track waveform and Looper toolbar using the configured time signature rather
  than a beat-stepped, hard-coded 4/4 marker.
- `[x]` On explicit Leave, stop the metronome in the surviving persistent local
  engine as well as resetting its controls, so the audible and displayed states
  agree without publishing a stop action to peers that remain in the jam.

#### Shared-controller foundation validation commands already run

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell"
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --scenario runtime-controls --stream-ms 16000 --logs tools\stress_logs_phase8_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --os-priority realtime --mesh-peers 2 --mesh-peers 3 --scenario mesh-2-clean --scenario mesh-3-authority-last --stream-ms 12000 --logs tools\stress_logs_phase8_mesh --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --stream-ms 6000 --include-validation --validation-stream-ms 3000 --logs tools\stress_logs_phase8_validation --clean
.\release\jam2.exe debug run tools\scenarios\boundary-validation.json
```

#### Typed lifecycle and manual-regression implementation evidence

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell"
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario transport-grid-authority --stream-ms 12000 --logs tools\stress_logs_phase8_departure_transport --clean
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario transport-track-actions --stream-ms 16000 --logs tools\stress_logs_phase8_transport_actions --clean
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario metronome-shared-grid --scenario last-peer-departure-grid-restart --stream-ms 12000 --logs tools\stress_logs_phase8_metronome_departure_final --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --stream-ms 6000 --include-validation --validation-stream-ms 3000 --logs tools\stress_logs_phase8_typed_validation_final --clean
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario transient-stall-recovery --stream-ms 16000 --logs tools\stress_logs_phase8_adaptive_recovery --clean
.\release\jam2.exe debug run tools\scenarios\lifecycle-headless-smoke.json
.\release\jam2.exe debug run tools\scenarios\boundary-validation.json
.\release\jam2.exe debug run tools\scenarios\controller-lifecycle-validation.json
python -m unittest discover -s tools -p "test_*.py"
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --stream-ms 6000 --include-validation --validation-stream-ms 3000 --logs tools\stress_logs_phase8_typed_validation_closeout_final --clean
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario transport-track-actions --stream-ms 16000 --logs tools\stress_logs_phase8_transport_actions_closeout --clean
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario metronome-shared-grid --scenario last-peer-departure-grid-restart --stream-ms 12000 --logs tools\stress_logs_phase8_metronome_departure_closeout --clean
python tools\run_stress_local.py --mode normal --sample-rate 48000 --headless-audio --scenario transient-stall-recovery --stream-ms 16000 --logs tools\stress_logs_phase8_adaptive_recovery_closeout --clean
cmd.exe /d /c "call compile.cmd --in-dev-shell"
.\release\jam2.exe debug run tools\scenarios\boundary-validation.json
python tools\run_stress_local.py --mode normal --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --profile fast --os-priority realtime --scenario clean-control --stream-ms 10000 --logs tools\stress_logs_phase8_audio_regression_fixed --clean
python tools\run_stress_local.py --mode normal --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --profile fast --os-priority realtime --scenario transport-track-actions-joiner --stream-ms 20000 --logs tools\stress_logs_phase8_symmetric_track_joiner_fixed --clean
python tools\run_stress_local.py --mode normal --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --profile fast --os-priority realtime --scenario transport-track-sync-off --stream-ms 20000 --logs tools\stress_logs_phase8_track_sync_off --clean
python -m unittest tools.test_jam2_results
cmd.exe /d /c "call compile.cmd --in-dev-shell"
.\release\jam2.exe debug run tools\scenarios\boundary-validation.json
python -m unittest tools.test_jam2_results
python tools\run_stress_local.py --mode normal --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --profile fast --os-priority realtime --scenario grid-authority-client-shared-grid --scenario transport-track-actions-joiner --stream-ms 20000 --logs tools\stress_logs_phase8_rejoin_clock_countin_fix --clean
python tools\run_stress_local.py --mode normal --headless-audio --headless-audio-buffer-frames 256 --sample-rate 44100 --profile fast --os-priority realtime --scenario grid-authority-client-shared-grid --stream-ms 20000 --logs tools\stress_logs_phase8_rejoin_clock_countin_fix_final --clean
```

The focused controller, transport, metronome, last-peer-departure, transient
recovery, lifecycle, and boundary cases passed. The final validation run passed
all 35 cases. The two-pass document/source closeout found and removed the last
GUI lifecycle mirrors and corrected shared-track asset-readiness/late-join
coordination. The user confirmed every focused manual check: immediate
metronome behavior; recording count-in; simplified remote-peer display;
ordinary and post-rejoin two-way shared-track controls, including Sync-off
isolation; clean real-device remote audio after rejoin; bar-aligned recording
Stop with continuous matching markers; and explicit-Leave metronome shutdown.
The final document and source audit found no remaining Phase 8-owned gap.

## Remaining Work

### Phase 9: Remove Compatibility Architecture and Split Application Ownership

#### Required single-application source layout

Phase 9 must replace the historical sibling `jam2-gui` and `jam2-cli`
application identities with this source layout:

```text
app/
  CMakeLists.txt
  main.cpp
  application/
  cli/
  gui/
  platform/

libs/
  jam2-core/
  third_party/
```

`app/cli` is a command-line frontend inside the one `jam2` application, not a
second application, executable, library target, or lifecycle. It owns argument
parsing, help, diagnostics, headless-command adaptation, and console/structured
presentation. Shared runtime configuration, session control, and services
belong in `app/application` so GUI, CLI, and debug automation consume the same
interfaces. `app/gui` owns Qt presentation and workflows, while `app/platform`
owns Windows/macOS application resources and integration.

Keep `libs/jam2-core` as the Qt-free engine/network/audio library. Keep
`libs/third_party` unchanged beside `libs/jam2-core`; do not move it under
`app`, into `jam2-core`, or into another `src` tree.

- `[x]` Remove `Jam2Process` argument construction, in-process CLI invocation,
  global stdout/stderr stream replacement, embedded global callbacks, and
  `EngineCompatibilityView` after Phase 8 consumers use typed interfaces.
- `[~]` Remove the legacy runtime stdin command loop, local GUI loopback control,
  JSONL state reconstruction, machine-readable startup compatibility, and text
  scraping that no retained user or automation path needs.
- `[x]` Keep `network create` and `network join` as the only public network
  startup commands; remove any remaining internal `listen`, `connect`, `mesh`,
  static-membership adapters or stale option parsing without preserving user
  compatibility.
- `[x]` Keep useful non-network diagnostics such as device listing/testing,
  meters, local/headless audio, connection diagnostics, recording, benchmark,
  and explicit debug commands in the same executable.
- `[~]` Keep ordinary headless commands focused on core audio/network startup
  and shutdown without an interactive stdin command loop. Phase 10 owns the
  replacement typed contract for scheduled automation.
- `[ ]` First perform a behavior-preserving source/CMake migration from
  `apps/jam2-gui` and `apps/jam2-cli` to the required top-level `app` tree.
  Make `app/CMakeLists.txt` own the sole `jam2` executable target, move platform
  resources under `app/platform`, and remove `apps` after it is empty. Do not
  combine this path migration with protocol, audio-path, dependency, or runtime
  behavior changes.
- `[ ]` After the structural migration builds and its public entry points pass,
  reduce the current `apps/jam2-cli/main.cpp` content under `app/cli` to a CLI
  frontend over shared application/runtime components. Extract argument
  parsing/help/dispatch, the universal network runner, stats/CSV presentation,
  and recording/prepared-source supervision into files with explicit
  ownership. Do not create a separate CLI executable or library target.
- `[ ]` Extract GUI page/widget construction and presentation under `app/gui`
  from the current `MainWindow.cpp`; `MainWindow` should assemble pages and
  coordinate user intent rather than construct and implement every page itself.
- `[ ]` Extract a mixer/stats view model and a metronome/transport controller
  under `app/gui`, consuming typed engine/session snapshots and submitting typed
  commands without duplicating lifecycle state.
- `[ ]` Extract the track/recording workflow, project persistence coordination,
  and asset-transfer service under `app/gui`; protocol parsing and file transfer
  state machines must not remain owned by the window.
- `[ ]` Leave `ApplicationRuntime` and `SharedSessionController` as the single
  engine/network/session lifecycle owners under `app/application`, with shared
  runtime options and services there rather than under either frontend. Keep
  extracted CLI, GUI, and debug components as adapters over them.
- `[ ]` Preserve `libs/jam2-core` and `libs/third_party` at their existing
  paths. Keep `jam2-core` Qt-free and keep `third_party` as its sibling rather
  than treating either directory as application source.
- `[ ]` Remove stale options, unused symbols, transitional names, and duplicated
  documentation after all extracted replacement call sites are active, then
  update active CMake/documentation source paths and audit both former monoliths
  against the detailed ownership boundaries in `refactor-efficiency.md`.

#### Manual regressions to clear before Phase 9 completion

- `[ ]` Feed GUI volume meters from a consumed interval peak with visible decay,
  not the lifetime maximum statistics accumulators.
- `[ ]` Replace the alert-style Jam Ready message box with a normal invite
  dialog that does not play the Windows notification sound and provides a
  reusable Copy URL button.
- `[ ]` Ensure the extracted track/transport view model presents the typed
  shared playing state consistently after arrangement and asset synchronization.
- `[ ]` Make Track/Looper clip move and edge-crop drags use a stable timeline
  coordinate system. Hit-test edit handles before click-to-seek, seek only for
  an otherwise unused timeline click, and freeze the view scale for the drag so
  moving playhead/grid markers cannot change mouse-to-frame mapping or jitter
  the edited region.

#### Compatibility-removal validation commands already run

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell"
.\release\jam2.exe -h
.\release\jam2.exe network -h
.\release\jam2.exe network create -h
.\release\jam2.exe network join -h
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --scenario jitter-50 --scenario loss-0.5 --scenario reorder-small --scenario duplicate-2.0 --stream-ms 8000 --logs tools\stress_logs_phase9_udp_headless --clean
python tools\run_stress_local.py --mode mesh --sample-rate 48000 --profile fast --os-priority realtime --mesh-peers 2 --mesh-peers 3 --scenario mesh-2-clean --scenario mesh-3-clean --stream-ms 8000 --logs tools\stress_logs_phase9_mesh_headless --clean
python tools\run_stress_local.py --headless-audio --headless-audio-buffer-frames 256 --sample-rate 48000 --profile fast --os-priority realtime --scenario clean-control --stream-ms 5000 --include-validation --validation-stream-ms 3000 --logs tools\stress_logs_phase9_validation --clean
```

### Phase 10: Complete the Native Automation Contract

- `[ ]` Replace the `debug run` argv wrapper with the smallest bounded,
  versioned declarative contract needed to retire existing automation reliance
  on stdin text commands, Python wall-clock action timing, duplicated native
  defaults, human-output scraping, and newest-file discovery. Reuse the same
  typed application requests and native validators as GUI and ordinary
  headless paths; do not mirror the full GUI, accept arbitrary argv, or expose
  internal state mutation.
- `[ ]` Admit scenario fields and operations only when a retained automated
  case needs them. Cover the startup audio/create/join configuration,
  profile/tuning selection, deterministic test input, local fixture/capture and
  artifact outputs, and typed scheduled actions already exercised by those
  cases. Keep ordinary `local` and `network create/join` commands as the simpler
  path for tests that do not need deterministic scheduling or structured
  automation results.
- `[ ]` Add an optional dedicated inherited local length-prefixed automation
  channel only for a `debug run` scenario that explicitly requests reactive
  commands/events. The parent controller creates and passes the handles; static
  `debug run`, `debug describe`, GUI, and ordinary CLI/network commands create
  none. Prefer a pair of anonymous inherited command/event pipes, reject handle
  configuration outside reactive `debug run`, do not reuse stdin/stdout, and do
  not expose a LAN automation listener. Use an unpredictable OS-local named IPC
  endpoint only if a platform constraint makes inherited anonymous handles
  impractical.
- `[ ]` Schedule frame-sensitive metronome, grid, transport, recording, and
  recovery actions through the native engine frame scheduler rather than Python
  wall-clock sleeps.
- `[ ]` Make native profile definitions and validation the single source of
  truth consumed by GUI, CLI, debug descriptions, and Python; remove duplicated
  Python defaults.
- `[ ]` Publish authoritative manifests containing run/machine/peer identity,
  source/build identity, effective configuration, topology, impairments,
  lifecycle result, and artifact paths/hashes. Local secrets may remain visible
  in arguments, scenarios, logs, and artifacts.
- `[ ]` Remove newest-file discovery and human-output scraping from Python once
  structured events and manifests cover those uses.
- `[ ]` Generalize the two-host benchmark controller into coordinator plus
  lightweight per-machine agents using normalized arbitrary peer identities.

Checks to run when useful:

- Compile and run declarative local, create/join, scheduled-control, two/three/
  four-peer, impairment, and multi-machine-compatible dry-run scenarios. Compare
  useful results with retained logs without imposing hard metric gates.
- Verify ordinary and static-debug launches inherit no automation handles, the
  reactive channel closes cleanly with its controller, and every admitted
  schema field/message maps to a named retained test case.

### Phase 11: Complete Hardening, Lifecycle Coverage, and Final Core Audit

- `[ ]` Apply the mandatory closeout procedure retroactively to Phases 1-7,
  reviewing every supporting refactor document against their real call paths,
  ownership, removal claims, tests, and known regressions. Reopen and implement
  any partial or unsupported item rather than grandfathering its `[x]` status.
- `[ ]` Centralize reusable control authorization, remote-model validation,
  connection admission, asset-transfer, and failure-reason policy outside GUI
  mutation code while keeping all work off the real-time callback.
- `[ ]` Add real-process Python scenarios for TCP fragmentation and admission,
  authentication/source rejection, remote model boundaries, asset/WAV failures,
  endpoint proof, and bounded malformed/flood behavior.
- `[ ]` Keep frame decoders, validators, asset state, and WAV parsing reachable
  through finite deterministic Python-generated malformed and boundary corpora
  required by the retained real-process scenarios. Phase 11 does not require a
  mutation/generative fuzzing framework, corpus minimizer, or sanitizer
  orchestration; those more complex optional components belong to Phase 12.
- `[ ]` Add lifecycle scenarios for late join, peer leave, peer restart,
  coordinator reconnect, endpoint migration, authority disappearance/takeover,
  and continued audio for unaffected peers.
- `[ ]` Add a mixed test with one real-device peer and remaining local headless
  peers; retain separate-machine testing as the final check for real network and
  independent hardware clocks.
- `[ ]` Review raw two-, three-, and four-peer CPU, packet, callback, jitter,
  drift, queue, underrun, and recovery data against retained refactor logs when
  investigating material changes; do not turn those numbers into hard gates or
  subjective scores.
- `[ ]` Perform a final unused-code and documentation audit against every
  supporting refactor document, then request focused user validation of GUI
  lifecycle and audible two-way audio before declaring the core refactor done.

Checks to run when useful:

- Compile and run the relevant lifecycle, malformed-input, mixed-device, and
  full-mesh suites. Record exact commands and log locations, then record the
  focused manual checks still requested from the user.

### Phase 12: Optional Protocol Experiments and Fuzzing Components

- `[ ]` Compare PCM16 and PCM24 only if measured bandwidth or mesh scaling data
  justifies it.
- `[ ]` Consider a smaller versioned UDP header only as a separate compatibility
  experiment.
- `[ ]` Consider raw binary asset chunks separately from the engine refactor.
- `[ ]` Consider dedicated Python mutation/generative fuzzing modules for the
  length-prefixed authenticated control protocol, UDP packet parsing/replay and
  endpoint-proof state, remote model/asset messages, and narrow WAV parsing.
  Keep deterministic boundary fixtures in Phase 11 rather than making this
  optional fuzzing infrastructure a core-refactor completion requirement.
- `[ ]` If optional fuzzing is implemented, use reproducible seeds, retained
  corpus replay, failure minimization, strict process/input/time/resource
  bounds, and isolated real-process or parser-harness execution. Sanitizer runs
  may supplement it where supported but remain test-only and non-gating.

## Work Log

Add concise entries as implementation proceeds:

### 2026-07-15 - Remove encryption and redundant non-feature Phase 12 items

- Removed the prospective encryption/stronger-peer-isolation item from Phase
  12. Jam2 intentionally prioritizes low-complexity direct performance over
  traffic confidentiality and requires users to invite only trusted
  participants sharing the session trust boundary.
- Kept authentication, integrity, explicit authorization, endpoint proof,
  replay protection, and bounded malformed-input handling as required session
  safety controls; the absence of confidentiality does not weaken them.
- Removed the redundant Phase 12 checkbox forbidding relays, TURN audio, rooms,
  accounts, and production platform infrastructure. Those remain standing
  repository/product constraints, not work that Phase 12 could complete.
- Updated `refactor-security.md` and `refactor-efficiency.md` to describe
  encryption, pairwise cryptographic identity, and confidentiality as explicit
  non-goals rather than work deferred until a hypothetical threat-model change.
  No implementation changed.

### 2026-07-15 - Move complex Python fuzzing components to optional Phase 12

- Kept finite deterministic malformed/boundary corpora and real-process TCP,
  authentication, model, asset/WAV, endpoint-proof, and bounded flood scenarios
  in Phase 11 because they directly validate required core security properties.
- Removed dedicated mutation/generative fuzz-framework, corpus-minimization, and
  sanitizer-orchestration work from Phase 11 completion. Phase 12 now owns
  optional Python fuzzing modules for control and UDP protocols, endpoint/replay
  state, remote model/assets, and narrow WAV parsing.
- Optional fuzzing must use reproducible seeds, retained corpus replay, failure
  minimization, strict resource/time bounds, and isolated process or parser
  harnesses. Sanitizers remain test-only and non-gating.
- Updated `refactor-security.md` so its required core guidance calls for finite
  deterministic malformed-input coverage while identifying fuzz/sanitizer
  infrastructure as optional Phase 12 work. No test implementation changed.

### 2026-07-15 - Phase 10 native-automation scope clarification

- Narrowed `debug run` from a prospective mirror of the application surface to
  a minimal native seam that must replace a named retained dependency on stdin
  text commands, Python wall-clock action timing, duplicated native defaults,
  output scraping, or newest-file artifact discovery. Ordinary `local` and
  `network create/join` remain the default for simple cases.
- Each admitted schema field/message must have a concrete retained case and map
  to an existing typed application request, event, validator, or artifact. The
  seam may not accept arbitrary argv, raw state setters, test-only engine
  mutations, remote-authority bypasses, or a full copy of GUI controls and
  snapshots.
- Reactive IPC is optional even within `debug run`. Its parent creates a pair
  of inherited local command/event handles only for an explicitly reactive
  scenario; static debug runs, `debug describe`, GUI, and ordinary commands
  receive none. Anonymous inherited pipes are preferred, with unpredictable
  OS-local named IPC only as a platform fallback and never a LAN listener.
- Updated `refactor-python.md` ownership, goals/non-goals, retained-case
  coverage, transport/message limits, controller responsibilities, security
  rules, and decision log to match this narrower Phase 10 contract. This was a
  documentation decision only; no automation implementation changed.

### 2026-07-15 - Phase 9 single-application source-layout decision

- Confirmed that `apps/jam2-cli` is not a separate build target: its source is
  compiled into the sole `jam2` target currently defined under
  `apps/jam2-gui`. The sibling application names therefore describe history,
  not the intended architecture.
- Phase 9 now requires one top-level `app` tree split into `application`, `cli`,
  `gui`, and `platform` ownership. CLI behavior remains a supported frontend of
  `jam2`, but it must not become a second executable, library target, or runtime
  lifecycle. Shared configuration and services belong to `app/application`.
- The behavior-preserving path/CMake migration must precede monolith
  decomposition so structural and ownership regressions can be isolated.
  `libs/jam2-core` remains the Qt-free core, and `libs/third_party` must remain
  unchanged beside it. No source files were moved by this planning update.

### 2026-07-15 - Phase 8 final closeout

- The user confirmed all remaining focused checks: the connector's first
  Play/Stop/Restart after rejoin, Sync-off isolation, continuous matching
  markers and bar-aligned recording Stop, explicit-Leave metronome shutdown,
  and clean real-device remote audio after rejoin. The count-in,
  `Remote Peers X`, immediate metronome behavior, and ordinary symmetric track
  controls had already been confirmed.
- Re-read `AGENTS.md`, the complete Phase 8 plan and work log,
  `refactor-efficiency.md`, `refactor-security.md`, `refactor-modes.md`,
  `refactor-binaries.md`, `refactor-python.md`, `PLAN.md`, and the current bug
  list. The implementation retains the direct full-mesh, two-peer-first model,
  one public executable, raw technical diagnostics, bounded authentication
  work, per-peer fixed-capacity media paths, and callback safety rules. No
  relay, account, dependency, wire-format, or PCM-format change was introduced.
- Re-audited the actual source boundary: GUI, ordinary `network create/join`,
  and debug automation converge on `SharedSessionController`,
  `ApplicationRuntime`, `Engine`, and `NetworkSession`. The controller owns
  lifecycle, authenticated membership, contract, authority, and reconnect
  policy; the engine owns audio/local media and its frame clock; the network
  session owns bounded UDP streams and mixing. Obsolete compatibility accessors
  and duplicated GUI lifecycle policy are absent from the active Phase 8 path.
- Existing controller, real-process, transport, metronome/departure, adaptive
  recovery, lifecycle, boundary, and Python evidence remains applicable. The
  final native boundary validation passed 30/30 and the required MSVC build
  exited zero with `BUILD SUCCEEDED`; `release/jam2.exe` matched the built
  artifact byte-for-byte. The user explicitly selected manual validation for
  the final GUI/audio checks, so no additional Python run was required.
- The newly reported crop jitter is a separate Phase 9 track-presentation bug:
  edit drags currently interact with click-to-seek and a changing view scale.
  Phase 9 now explicitly requires edit-handle hit testing before seek and a
  stable mouse-to-frame mapping for the duration of a drag. The sticky volume
  meter also remains Phase 9 view-model work. Phase 10 still owns the expanded
  native automation boundary, Phase 11 owns the retroactive cross-phase
  security/hardening audit, and Phase 12 owns optional protocol fuzzing; none is
  concealed Phase 8 work.
- The second post-fix document/source audit found no remaining Phase 8-owned
  defect or incomplete item. Every Phase 8 item is now `[x]`, and the phase has
  moved to `Completed Work`.

### 2026-07-15 - Phase 8 manual regression follow-up

- The user confirmed the restored recording count-in, simplified
  `Remote Peers X` presentation, immediate metronome, and ordinary two-way
  shared-track control. The focused reconnect and recording-display edge cases
  remain open for manual verification.
- In the latest CSVs, the first connector session ended at transport event 3,
  the rejoined connector restarted its counter at 1, and the persistent creator
  retained the same source peer's replay high-water mark. The creator therefore
  rejected the first three rejoin actions and only accepted later counters,
  explaining why connector Play sometimes reached the other side and sometimes
  did not. Transport event IDs now live in persistent `Jam2RuntimeHost` state
  and stay monotonic across a network worker reset. Receive processing also
  defers replay acceptance until the re-established UDP edge has RTT mapping,
  allowing the bounded repeated event to be scheduled instead of consumed.
- Manual recording Stop now targets the next whole musical bar. Track and
  Looper markers use continuous engine transport position and the configured
  beats per bar instead of stepping once per beat against an unrelated modulo,
  which removes the deterministic visual drift. Explicit Leave now disables
  the surviving engine's local metronome after network detachment so it cannot
  keep clicking behind reset controls.
- Re-audited the follow-up against every supporting refactor document. The
  `(authenticated peer id, persistent local counter)` event identity satisfies
  the mesh transport ordering model without adding a wire field; replay and
  readiness checks remain on the bounded network path. Recording targets remain
  engine-frame authoritative, while GUI timers only interpolate presentation.
  No allocation, locking, logging, file work, protocol change, dependency, or
  relay path was added to the real-time callback. The sticky meter item remains
  explicitly assigned to Phase 9's mixer/stats view-model work.
- The native boundary scenario passed all 30 cases, including whole-bar record
  stop and transport-event monotonicity across network reset. The required
  `cmd.exe /d /c "call compile.cmd --in-dev-shell"` MSVC validation exited zero
  with `BUILD SUCCEEDED`, and the fixed `release/jam2.exe` matches the build
  artifact byte-for-byte. Per the user's request, these GUI/audio regressions
  are left for manual validation and no additional Python tests are required
  for this pass.

### 2026-07-15 - Phase 8 rejoin transport clock and recording count-in

- Audited the three latest real-device CSVs in `release/logs`. The rejoined
  connector published a TrackRestart with raw target `12061237`, but its local
  marker epoch became `12035075` while the creator mapped the same event to raw
  target/epoch `12615242`. The connector therefore displayed a 26,162-frame
  (593 ms at 44.1 kHz) transport-phase error even though the scheduled audio
  actions remained clock-mapped. `restartPreparedTrackQuantized` now maps the
  future raw restart target to its matching future musical frame instead of
  attaching the button-press musical frame.
- The same rejoin reset the connector's grid proposal counter to one while the
  persistent creator retained that peer identity's prior request history. It
  sent 8,717 stale proposals and the creator rejected all 8,717. Grid request
  IDs now live in persistent `Jam2RuntimeHost` state and remain monotonic across
  a network worker reset/rejoin.
- Restored the engine-clock recording sequence requested by the user. Arming a
  take against a stopped metronome waits for the engine's new valid epoch; one
  grid snapshot then defines the next-bar countdown boundary and the following
  recording boundary. The UI remains on `WAITING FOR NEXT BAR...`, displays
  integer `4 3 2 1` on the corresponding metronome beats for the default one-bar
  4/4 count-in, and starts the take at the exact scheduled engine frame.
- The prior sparse/bitcrushed fault did not recur in these logs. Both connector
  runs had zero sequence loss, capture underruns, and playback underruns, with
  raw drift of 6.34 and 8.71 ppm. The only material active-session disturbance
  was one 105-107 ms jitter/stall immediately before the first leave: the peers
  reported 6,488/6,425 missing and late-after-release mixer frames, and the
  connector dropped 3,008 output frames while recovering. That bounded stall
  can explain a brief light crackle, but it is not continuing packet loss or a
  PCM/bit-depth fault. The retained 13.359 s prepared mix had no clipping or
  hard adjacent-sample discontinuity.
- The required elevated MSVC build passed. Boundary validation passed all 28
  cases, including future raw/musical clock mapping, exact `4 3 2 1`, and grid
  proposal monotonicity across reset; Python result tests passed 13/13. The
  joiner transport scenario passed. The shared-grid rerun passed in
  `tools/stress_logs_phase8_rejoin_clock_countin_fix_final` with consensus grid
  revision 2, alignment valid on both sides, zero beat delta, zero packet loss,
  zero missing frames, and zero playback-underrun time. The metrics reader now
  tests and retains the last established periodic alignment when a creator's
  final post-disconnect row correctly clears its live peer mapping.
- Phase 8 remains open for focused real-device confirmation that connector-
  initiated Play/Restart stays visually aligned after leave/rejoin, the restored
  recording count-in follows the audible clicks and begins cleanly, remote audio
  has no recurring crackle, and the simplified `Remote Peers X` display is the
  desired presentation.

### 2026-07-14 - Phase 8 symmetric track control and reconnect audio regression

- The user confirmed the immediate metronome behavior. Shared-track semantics
  were corrected so any authenticated peer may originate source-identified
  Play, Stop, or Restart events. Replay counters are now per source. Recording
  transport remains arrangement-authority-only. A peer with Sync track controls
  disabled applies local controls without publishing them and authenticates but
  disregards incoming peer track actions.
- Replaced the dense top-right topology summary with `Remote Peers X`. The value
  counts non-local authenticated membership entries only while their typed UDP
  edge is Active; revision and per-edge measurements remain in snapshots and
  technical logs.
- Audited the latest real jam logs. Before the reconnect, peer
  `jam2_stats_20260714_233610_632_pid25324.csv` sent 19,489 packets in 28.293 s
  (about 689 packets/s), had zero capture-ring underruns, and measured raw drift
  of 38.8 ppm. After reconnect,
  `jam2_stats_20260714_233706_753_pid25324.csv` sent only 71,688 packets in
  169.202 s (about 424 packets/s), reported 10,738,816 capture-ring underrun
  frames, and rejected all 116,535 incoming packets as future sample times.
  The receiving creator `jam2_stats_20260714_233601_924_pid43624.csv` had zero
  sequence loss but measured -385,263 ppm raw drift, hit the 0.9995 resampler
  limit, and substituted 1,610,080 playback-ring underrun frames in 50,315
  events (36.51 s, 15.628% of the run). This explains the sparse, repetitive
  bitcrushed sound as an application capture/timeline fault, not packet loss or
  PCM24 quality.
- Fixed `Engine::popNetworkCapture` so a 64-frame packet never consumes and
  discards a partial 32-frame ASIO callback. Fixed `PeerStream` so the first
  authenticated packet after late attachment establishes its sender-time
  baseline while later jumps still use the bounded ten-second rejection
  horizon. The prepared/local track path did not use the faulty network capture
  handoff, which is why its audio remained clean.
- The required elevated MSVC build passed. Boundary validation now covers exact
  64-from-32 capture before and after reattachment, late sender-time baselining,
  post-baseline future rejection, symmetric transport authorization, per-source
  replay rejection, and record authority. Python result tests passed 12/12.
  Clean audio passed with zero loss and zero underrun time in
  `tools/stress_logs_phase8_audio_regression_fixed`; joiner-originated Play,
  Stop, and Restart passed in
  `tools/stress_logs_phase8_symmetric_track_joiner_fixed`; independent sync-off
  behavior passed in `tools/stress_logs_phase8_track_sync_off`.
- Phase 8 remains open for the user's real-device confirmation of remote audio
  after rejoin, two-way GUI track control with sync enabled/disabled, and the
  simplified Remote Peers status presentation.

### 2026-07-14 - Phase 8 automated closeout and second audit

- Re-read `AGENTS.md` and the complete Phase 8 checklist, work log, and manual
  bugs. The implementation keeps the direct lightweight mesh, hard technical
  state, fixed/bounded hot paths, one executable, and the required MSVC/Python
  validation workflow; no dependency, relay, account, or subjective scoring
  surface was added.
- Reconciled `refactor-binaries.md` and `refactor-modes.md` against the live GUI
  and headless call paths. Both use `SharedSessionController` with
  `ApplicationRuntime`, `Engine`, and `NetworkSession`; the controller owns the
  immutable contract, membership, reconnect policy, and authority revisions.
  Removed the remaining GUI creator/connection/coordinator mirrors found by the
  second audit.
- Reconciled `refactor-efficiency.md` and `refactor-security.md`. Controller and
  readiness work remains cold-path, authenticated source identity is retained,
  stale/unauthorized authority and readiness revisions are rejected, and
  existing admission, frame, queue, asset, peer-stream, and mixer bounds remain
  visible. Phase 9 still owns the GUI/CLI source decomposition, Phase 11 owns
  the final cross-phase hardening audit, and Phase 12 owns optional protocol
  fuzzing components.
- Reconciled `refactor-python.md` and `PLAN.md`. Added executable-based real
  controller coverage rather than CTest or a mock, retained raw recovery peak
  data while making the settling verdict use end-of-window occupancy, and made
  no optional wire/PCM/fast-path experiment. Phase 10 still owns replacement of
  the temporary headless line adapter with the bounded inherited native pipe.
- Shared Play now publishes a fresh arrangement revision, waits for creator and
  every current peer to finish asset preparation, and lets the arrangement
  authority publish one source-identified target with at least 200 ms lead.
  Stop cancels an outstanding barrier, and late/reconnected peers force a fresh
  readiness revision instead of choosing their own later bar.
- The required elevated MSVC build passed. The controller lifecycle scenario
  passed ten typed cases, including refusal, authentication failure, bounded
  exhaustion, established disconnect/reconnect, manual refresh, late join, and
  non-coordinator authority. Boundary and 16 Python unit checks passed; the
  final clean run and all 35 validation cases passed in
  `tools/stress_logs_phase8_typed_validation_closeout_final`. Focused transport,
  metronome/departure, and adaptive-recovery results are in the corresponding
  `*_closeout` folders recorded under Phase 8.
- Automated/controller-owned Phase 8 clauses are closeout-audited. Phase 8 is
  intentionally still under `Remaining Work`: the user still needs to confirm
  immediate metronome behavior, shared-track play/stop/restart and controls,
  and topology counts/revision/log presentation in the GUI.

### 2026-07-14 - Phase 8 typed lifecycle and manual-regression continuation

- Bound `ApplicationRuntime` and the TCP controller under the shared typed
  lifecycle path, added typed transport/failure events and bounded reconnect
  state, removed the second GUI-owned settings/readiness handshake, and exposed
  lifecycle/failure/authority data in shared snapshots.
- Moved membership-page grid/arrangement parsing, monotonic revision checks,
  authenticated source validation, and late-join authority state into the
  shared controller. GUI topology now reports explicit total/remote counts and
  independent membership/grid/arrangement revisions without unchanged-summary
  log spam.
- Gave metronome revisions a fresh 200-500 ms local epoch, cancelled stale
  prepared commands when authority departs, retained the creator after the last
  remote peer leaves, and added a durable departure/restart regression.
- Added distinct revisioned track play/stop/restart actions, ensured an
  immediate target is published at least once, and exposed source, counter,
  grid revision, action, requested target, and applied target in CSV. Both peers
  observed action set `{restart, stop, play}` with counter `3` in
  `tools/stress_logs_phase8_transport_actions`.
- Verified adaptive release applies the requested bounded playback drop and
  returns to the configured target in
  `tools/stress_logs_phase8_adaptive_recovery`. Final focused metronome and
  departure evidence is in `tools/stress_logs_phase8_metronome_departure_final`;
  all 34 validation cases passed in
  `tools/stress_logs_phase8_typed_validation_final`.
- Reopened Phase 9's stdin-removal markers because the Phase 8 stress coverage
  temporarily uses a small headless line adapter. Phase 10 owns its replacement
  with the bounded native automation contract and subsequent removal.
- Phase 8 remains open. Focused shared-controller coverage for established
  disconnect/reconnect/manual refresh and GUI confirmation of shared-track
  control consistency remain, followed by the mandatory two-pass closeout
  audit. No Phase 8 item was promoted to `[x]` from these implementation-pass
  results alone.

### 2026-07-14 - Mandatory phase closeout discipline

- Added a required two-pass document/source reconciliation after implementation
  and after any resulting fixes. A phase can no longer be declared complete
  from its shortened checklist, compilation, or stress results alone.
- Required broad partially implemented items to be split, all discovered work
  to remain visible, known phase-owned defects to block completion, exact
  validation evidence to be retained, and only closeout-audited phases to move
  into `Completed Work`.

### 2026-07-14 - Manual Phase 8/9 regression triage

- Recorded delayed metronome start, stale post-departure grid scheduling,
  unsynchronized shared-track transport, ambiguous/repeated session snapshots,
  sticky meters, invite-dialog sound/copy usability, and incorrect recording
  count-in presentation as required Phase 8/9 work.
- In `jam2_stats_20260714_172425_838_pid44232.csv` and
  `jam2_stats_20260714_172432_942_pid38200.csv`, the transient raised the adaptive
  target to about 1,530 frames. The target counter returned to 256, but playback
  depth remained around 1,500 frames with no requested/applied playback drop,
  confirming that the latency control value recovered without draining the
  accumulated queue.
- The latest 21:11 pair was otherwise stable and returned to the normal
  256-frame target; it also showed creator membership moving from one remote
  peer to zero when the joiner left.

### 2026-07-14 - Phase 9 compatibility removal groundwork

- Added `ApplicationRuntime` as the typed owner of the persistent local engine
  and universal network worker; GUI and session control no longer invoke an
  internal CLI or reconstruct state from captured process output.
- Removed `Jam2Process`, embedded global callbacks, stream replacement, runtime
  stdin control, GUI loopback framing, machine-startup/JSONL compatibility,
  `EngineCompatibilityView`, stale helpers/options, and the unused GUI control
  protocol. GUI actions now submit fixed-shape `EngineCommand` values directly.
- Kept the fixed polling snapshot allocation-free by separating cold device and
  recording metadata, while retaining typed engine/network diagnostics.
- Build and public help checks succeeded. Clean plus impaired create/join,
  two/three-peer full mesh, lifecycle, boundary, admission, and debug validation
  passed; artifacts are retained in the Phase 9 folders named above.
- Phase 9 remains open: `apps/jam2-cli/main.cpp` and `MainWindow.cpp` still need
  the explicit CLI, page/presentation, mixer/stats, metronome/transport,
  track/recording, project, and asset-transfer ownership splits listed above.

### 2026-07-14 - Phase 8 shared session controller groundwork

- Added a shared non-UI controller used by GUI, headless, and debug paths for
  TCP bootstrap/authentication, paged membership, a basic immutable contract,
  and per-edge state.
- Routed live GUI controls through `Engine` commands/events/snapshots, exposed
  typed `NetworkSession` snapshots, and retained bounded per-peer streams and
  mixer slots without a global peer-count ceiling.
- Added the optional creator peer limit, an eight-connection unauthenticated
  cap, authentication deadlines, and a bounded failed-authentication work rate.
- Build succeeded. Focused UDP and 2/3-peer mesh runs passed, the boundary
  scenario passed, and all 33 validation cases passed. Those checks established
  the controller foundation but did not cover the remaining lifecycle and
  authority gaps; their artifacts are retained in the Phase 8 folders above.
- Phase 8 remains open: complete lifecycle ownership, typed transport failures,
  reconnect behavior, effective session-contract ownership, authority parsing
  and validation, and authoritative snapshots/tests are listed above.

### 2026-07-14 - Refactor document reconciliation

- Made this file the only authority for refactor phases, completion status, and
  remaining work; supporting refactor documents now provide design context
  without their own phase or gate systems.
- Grouped implemented Phases 1-7 under `Completed Work` and corrected their
  descriptions so transitional GUI/CLI and Python architecture is not claimed
  as finished.
- Added remaining core Phases 8-11 for the shared session controller, typed
  application boundary, compatibility removal/source decomposition, native
  automation contract, hardening, lifecycle coverage, and final audit.
- Moved optional measured wire/protocol experiments to the final Phase 12.
- Resolved product directives around optional creator peer limits, bounded
  unauthenticated admission, one public GUI/CLI executable, create/join-only
  networking, permitted local key exposure, removal of the runtime stdin loop,
  shared session ownership, and evidence-based non-gating validation.

### 2026-07-14 - Transient stall recovery

- Prevented adaptive silence from outranking queued peer audio, made adaptive
  release accumulate fractional frames and temporarily drain at the configured
  ppm, and resynchronized stale frames already replaced by mixer deadlines.
- Full queues now evict oldest stale audio rather than rejecting all new audio;
  detached shutdown sinks no longer create false padding or capacity drops.
- Added `transient-stall-recovery`, a one-shot bidirectional 120 ms proxy stall
  with final-window queue, padding, packet-flow, capacity, and target-release
  assertions.
- Build succeeded. Clean control passed. The recovery case passed with 125 ms
  jitter, zero loss/capacity drops/missing frames/playback drops/underrun time,
  final-window occupancy below 14%, and both adaptive targets releasing.

### 2026-07-14 - Phase 7 tooling migration and legacy retirement

- Added the bounded versioned `debug` adapter, unified command discovery,
  structured lifecycle/boundary events, exact artifact paths, and migrated
  stress/benchmark launch commands to the public `network create`/`join`
  bootstrap.
- Added stable multi-peer identities, independent synthetic clocks, and named
  per-edge impairment proxies through an inherited debug-only candidate seam.
- Removed the public static topology, old listen/connect packet loops, separate
  CLI target, and stale mode dispatch. The creator now coordinates authenticated
  TCP membership while every audio edge uses the same direct UDP full mesh.
- Serialized and acknowledged dynamic membership updates, fixed unchanged-peer
  reconciliation, and scoped finite stress clocks to start on the first proven
  edge for public create/join sessions.
- Enhanced mesh verdicts to validate established membership/active/mixer
  high-water values while retaining final teardown counts for diagnosis.
- Final retained results: all 13 UDP scenarios and all 32 validation checks
  passed; all four 2/3/4-peer mesh scenarios passed; the three-peer impaired-edge
  scenario passed. All mesh audio probes passed with zero packet loss and zero
  mixer capacity drops.

### 2026-07-14 - Phase 6 GUI lifecycle and single application

- Consolidated Windows packaging into `jam2.exe`: no arguments launch the Qt
  GUI with the existing logo, while documented CLI/debug arguments run headless.
- Replaced the GUI child process and loopback state reconstruction with an
  in-process typed bridge and persistent Engine; unchanged devices survive
  Local/Start/Join/Leave and unaffected peer streams survive membership edits.
- The GUI process runs High priority and the UDP packet worker alone runs Time
  Critical with MMCSS Pro Audio for realtime mode. MSVC and the three-scenario
  Phase 6 headless suite passed.

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
  the then-current secret scrubbing. The 2026-07-14 reconciliation later moved
  local key exposure outside the application threat boundary.
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
  manifests/results recorded redacted native effective configuration under the
  policy in effect at that time.
- Split reusable scenario planning and result/audio verdict processing out of
  the runner while retaining the existing command surface.
- Kept real-process TCP/model/asset/WAV adversarial scenario expansion in the
  tooling-migration phase, where the validator can target the same unified
  executable rather than adding a second temporary automation surface.
- No build, compilation, or runtime validation was run under the `AGENTS.md`
  rule in effect at that time. Phase 1 was marked implementation-complete with
  user-run validation pending.

### 2026-07-13 — Local invitation output

- Keep the complete invitation URL and session key visible in native stdout and
  the GUI's local engine-output panel. These values are the deliberate manual
  handoff used to join a direct session; hiding them makes the primary workflow
  unusable.
- At that time, redaction was limited to Python comparison/archive artifacts.
  The 2026-07-14 reconciliation supersedes that restriction: local artifacts
  may contain session keys or invite URLs.

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
