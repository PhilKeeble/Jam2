# Jam2 Local and Network Mode Consolidation Review

## Purpose

This report evaluates consolidating Jam2's current `local`, `listen`, `connect`, and `mesh` engine modes into one persistent local engine with an optional network session.

The intended product model is:

- The audio device, local metronome, prepared tracks, recording, transport clock, controls, and technical statistics belong to one local engine.
- Starting or joining a jam attaches networking to that engine instead of replacing it with another process or mode.
- All network audio uses one direct full-mesh implementation.
- A two-person jam is a mesh containing one remote peer, with no alternate audio engine.
- Peers are equal in the UDP audio data plane.
- Starting and joining remain distinct bootstrap workflows, not permanent engine roles.
- Musical authority is assigned explicitly by the GUI/control plane. In `leader-audio`, the peer that starts the metronome becomes the leader for that grid epoch.

This is a static source review. Nothing was compiled or run. It should be considered alongside [refactor-binaries.md](refactor-binaries.md) and [refactor-efficiency.md](refactor-efficiency.md).

The current-mode descriptions below record the pre-consolidation baseline.
They explain which mature behavior had to be preserved and are not the current
public command set or implementation status. Status and phase order live only
in [refactor-plan.md](refactor-plan.md).

The final implementation reconciliation confirms one role-independent
`NetworkSession` data plane keyed by stable peer IDs and pre-resolved numeric
UDP endpoints. Creator/joiner remains bootstrap metadata; endpoint route
failures are isolated to the affected edge and return it to bounded probing
rather than terminating the whole runtime. A fatal socket-wide failure remains
an explicit runtime error.

## Executive Assessment

The proposed consolidation is a strong target and fits Jam2 better than the current four-mode history. It would simplify lifecycle, testing, state ownership, peer scaling, and the single-executable refactor.

The important qualification is that the existing `mesh` loop should not simply replace `listen` and `connect`.

`listen` and `connect` use the same relatively mature two-peer packet engine, which contains:

- Sequence reorder recovery.
- A bounded jitter horizon.
- Sample-time-aware playout and missing-frame handling.
- Adaptive playback cushion logic.
- Clock-drift measurement and resampler control.
- Detailed packet, buffer, callback, and compensation statistics.
- Leader-audio click injection.
- Shared-grid and listener-compensated timing behavior.
- Network-thread priority setup.

The experimental `mesh` mode is a separate implementation. It adds fan-out, per-peer counters, and local mixing, but omits or simplifies several of those mature behaviors. Making it the universal path unchanged would make two-person sessions easier to describe while making their technical behavior worse.

The safest design is therefore:

1. Keep one persistent local `Engine` and sample-frame clock.
2. Extract the mature two-peer receive behavior into a reusable `PeerStream` component.
3. Run one `PeerStream` for every remote peer, including the ordinary one-peer case.
4. Drift-correct and release each remote stream independently onto the local engine timeline.
5. Mix the released peer blocks into one playback stream.
6. Keep session creation/joining, membership coordination, and musical authority outside the audio peer implementation.
7. Retire the legacy `listen`, `connect`, and experimental `mesh`
   implementations after their useful behavior is carried into the universal
   path; do not retain public compatibility aliases.

This yields one local engine and one network engine without treating the original experimental mesh loop as the architectural foundation.

## Current Modes

### `local`

`local` starts the audio device and all local facilities without creating an audio UDP socket:

- Audio input/output callback.
- Local monitor and mixer.
- Metronome and engine sample-frame clock.
- Prepared-track playback.
- Track take and jam recording.
- Runtime commands and GUI binary control.

Its supervisor loop updates epochs, transport, controls, and status every 5 ms. It does not consume captured frames for networking.

Important lifecycle detail: the capture ring is still created and fed by the callback even though no network consumer drains it. It can fill with stale audio and report capture overruns while the application is intentionally local. A persistent-engine design must either disable that transport tap when networking is inactive or continuously discard it without treating the condition as a fault.

### `listen`

`listen` is both a connection-setup role and a permanent timing role:

1. Creates and binds the UDP socket.
2. Uses a manual public endpoint, local fallback, or STUN discovery.
3. Creates the session id/key and invite URL.
4. Waits for an authenticated `Hello` from one endpoint.
5. Validates sample rate and packet frame size.
6. Locks to that endpoint and sends `HelloAck`.
7. Starts the audio device only after the peer is accepted.
8. Runs the shared two-peer packet engine with `listener_side=true`.

The `listener_side` flag gives the listener special shared-grid initialization and phase behavior. It is therefore more than a socket role.

### `connect`

`connect` performs the opposite bootstrap flow:

1. Parses the invite URL.
2. Binds an ephemeral UDP port.
3. Repeatedly sends `Hello` to the advertised endpoint.
4. Waits for an authenticated `HelloAck`.
5. Validates sample rate and packet frame size.
6. Starts the audio device only after the handshake.
7. Runs the same two-peer packet engine with `listener_side=false`.

After the handshake, its packet format and mixer controls are the same as `listen`, but timing behavior remains different because `listener_side` is fixed for the entire run.

### `mesh`

`mesh` has a different lifecycle and data engine:

1. Requires a session id, session key, bind endpoint, and explicit peer list.
2. Starts the audio device immediately, including with an empty peer list.
3. Does not perform the `Hello`/`HelloAck` stream-compatibility handshake.
4. Sends each encoded local audio packet to every configured endpoint.
5. Tracks sequence, RTT, jitter, and traffic per peer.
6. Mixes received audio in a shared map keyed by sender sample time.
7. Uses a fixed startup `grid_coordinator` flag for metronome timing.
8. Cannot update membership internally; the GUI restarts the engine when the peer list changes.

This mode already resembles the desired persistent local engine because it can run with zero peers. Its peer fan-out and per-peer statistics are useful, but its receive/timing implementation is not equivalent to the two-peer engine.

## Current Difference Matrix

| Concern | Local | Listen | Connect | Mesh |
| --- | --- | --- | --- | --- |
| Audio device starts | Immediately | After inbound handshake | After outbound handshake | Immediately |
| Audio UDP socket | None | Bound, optionally STUN-discovered | Ephemeral bind | Explicit bind |
| Remote peers | 0 | Exactly 1 | Exactly 1 | Static 0..N |
| Stream handshake | None | Accepts and locks one peer | Initiates to one peer | None |
| Sample/frame compatibility | Local only | Validated | Validated | Assumed from GUI/CLI |
| Packet loop | Local 5 ms supervisor | Mature two-peer loop | Same two-peer loop | Separate mesh loop |
| Network priority scope | N/A | Applied | Applied | Not applied by mesh loop |
| Sequence handling | N/A | Reorder window | Reorder window | Per-peer tracker; no equivalent reorder release |
| Jitter handling | N/A | Ordered jitter queue | Ordered jitter queue | Shared mix deadline only |
| Missing audio frames | N/A | Inserts/counts per remote stream | Same | Aggregate sample-time gap fill |
| Adaptive cushion | N/A | Implemented | Implemented | Not implemented |
| Drift measurement/correction | N/A | Implemented for one stream | Implemented for one stream | Playback ratio held at 1.0 |
| Playback maximum/drop logic | Local playback only | Implemented, with a ring ownership issue | Same | Not equivalently enforced |
| Leader-audio injection | Local leader enabled initially | Implemented | Implemented | Missing from mesh send path |
| Shared-grid authority | Local command | Fixed listener role initializes | Follows/compensates relative to listener | Fixed `grid_coordinator` process |
| Listener compensation | Local has no remote reference | Implemented asymmetrically | Implemented asymmetrically | Not implemented equivalently |
| Transport broadcast | None unless local action | One peer | One peer | Broadcast to all static peers |
| Peer departure | N/A | `Bye` ends session | `Bye` ends session | `Bye` is ignored; GUI membership restart |
| Stats | Local/audio | Detailed two-peer | Detailed two-peer | Reduced aggregate plus per-peer subset |

## Why Two-Person Should Use the Same Full-Mesh Path

For two people, a full mesh contains exactly two directed audio paths:

```text
peer A -> peer B
peer B -> peer A
```

That is the same audio topology as the former `listen/connect` session. A general peer table adds negligible inherent cost when it contains one remote peer. Encoding can still happen once per local packet, followed by one send.

Using the same path provides real maintenance benefits:

- Every normal session exercises peer-list, handshake, per-peer stats, timing, and mix code.
- Three/four-peer support no longer depends on a less-tested alternate loop.
- Fixes to authentication, buffering, drift, and packet scheduling apply to all session sizes.
- Adding or removing a peer does not change engine mode.
- Test results scale from one remote peer to several without changing implementations.
- The GUI no longer needs a “Mesh mode” checkbox.
- The network state can naturally contain zero peers while the local engine remains active.

The audio topology can be symmetric even though session creation and joining remain different user actions.

## Roles That Must Be Separated

The current implementation uses “listener,” “leader,” “host,” and “grid coordinator” in overlapping ways. The unified model should keep four responsibilities distinct.

### 1. Bootstrap/session coordinator

The peer that starts the jam initially:

- Creates the session id/key.
- Publishes the invite endpoint.
- Authenticates joining GUI/control connections.
- Distributes the immutable session contract and current membership.
- Orders and redistributes accepted shared revisions when simultaneous requests
  need a deterministic sequence.

This is a control-plane responsibility. It does not make that peer special in UDP audio mixing.

It also does not grant host-only musical or Track-view permissions. Every
authenticated peer is an equal collaborator and may originate valid grid,
transport, song, arrangement, crop, move, playback, recording, and additive
asset actions while that peer's local synchronization control is enabled. The
coordinator validates, orders, and distributes those actions; it does not decide
which collaborator is allowed to edit.

The first implementation can keep the current creator GUI as coordinator. Coordinator election or serverless membership recovery is unnecessary for v1.

### 2. Audio peer

Every admitted peer:

- Sends one local mono stream directly to every other admitted peer.
- Maintains independent receive, timing, drift, and stats state for each remote peer.
- Is identified by a stable session peer id, not only by its current IP/port.
- Can change endpoint or reconnect without changing musical authority accidentally.

All audio peers are equal.

### 3. Grid authority

The current grid authority owns one metronome epoch/revision:

- Running/stopped state.
- Mode.
- BPM and pattern.
- Future epoch frame on the authority's clock.
- Authority peer id and monotonic grid revision.

The authority should be selected by a user action, not inferred permanently from who created or joined the session.

For a lightweight first implementation, any peer's GUI sends “start grid” to the bootstrap coordinator. The coordinator assigns the next grid revision and broadcasts the selected initiating peer as authority. This permits any peer to start the metronome while giving simultaneous requests a deterministic order.

### 4. Arrangement/transport authority

Song arrangement, prepared-track state, quantized restart, and recording commands are separate from audio and grid authority.

`Arrangement authority` names the source of the accepted ordered revision; it
does not mean that the creator owns the arrangement or has exclusive edit
permission. Any authenticated synchronized peer may propose an arrangement
change. The coordinator validates it, resolves simultaneous requests, assigns a
globally unambiguous revision/event id, and redistributes the accepted result.
Local revision counters from different peers are not comparable by themselves.

A completed recording or imported WAV is an additive contribution. The
recording/importing peer offers a unique contribution id plus its intended
bank/lane and content hash; the coordinator requests and validates the asset,
fills the target only when it is still empty, otherwise appends a lane, and then
publishes the accepted arrangement revision. This preserves simultaneous takes
without allowing competing snapshots to overwrite one another. Local arming and
file writing still require local consent.

Separating these roles avoids replacing one fixed `listener_side` flag with a different fixed “host is everything” flag.

## Proposed Persistent Engine Lifecycle

### Application startup

The application constructs one `Engine` containing:

- Audio-device ownership.
- Local `FrameClock` driven by device callbacks.
- Metronome and transport state.
- Prepared tracks and recording.
- Mixer controls and statistics.
- An inactive `NetworkSession` attachment point.

The object can exist before a device is selected, but the authoritative sample-frame clock must not pretend to run from a GUI timer. It becomes active when the audio device starts.

If user preferences identify a valid device, local audio can start automatically. Otherwise the engine remains `WaitingForDevice` until the user selects one.

### Local operation

With no network session:

- Local monitor, metronome, track playback, and recording work normally.
- No audio UDP socket is required.
- The network capture tap is disabled or discarded at a safe boundary.
- Network loss/jitter counters remain inactive rather than reporting local-mode capture overruns.

“Local” becomes the base state, not an exclusive mode.

### Start Jam

Starting a jam attaches a `NetworkSession`:

1. Validate the current audio sample rate against the proposed session contract.
2. Bind one UDP socket.
3. Perform STUN discovery or use the explicit public endpoint.
4. Create session id/key and stable local peer id.
5. Start the authenticated TCP control coordinator.
6. Publish an invite.
7. Enter `NetworkActive` with zero remote peers.
8. Add `PeerStream` objects as peers are admitted and UDP handshakes complete.

The audio device, local tracks, and callback do not restart.

### Join Jam

Joining attaches to an existing session:

1. Parse the invite and authenticate to the bootstrap coordinator.
2. Receive the immutable wire contract before enabling UDP audio.
3. Compare it to the active local engine.
4. If the sample rate is incompatible, request an explicit audio-engine restart without restarting the application; do not silently resample the device or guess.
5. Bind the local UDP socket and discover/advertise endpoint candidates.
6. Receive current membership.
7. Perform authenticated symmetric UDP handshakes with every peer.
8. Create one `PeerStream` for each successful path.
9. Join the current grid at the next safe bar boundary if it is running.

After admission, the original creator/joiner distinction disappears from the audio engine.

### Leave or disconnect

Leaving the session should:

- Stop new network capture packets.
- Send bounded `Bye` notifications when possible.
- Remove all peer streams.
- Drain or explicitly discard residual remote playback.
- Close control and UDP sockets.
- Keep the local audio device, local metronome/track state, and application alive.
- Return to local operation without child-process replacement.

An individual peer departure removes only that peer. It must not restart the device or other peer streams.

## Proposed Network Data Path

```text
Audio callback
    -> timestamped local capture blocks
    -> NetworkSession packetizer
    -> encode once
    -> send same packet to every active peer

For each remote peer:
    UDP receive
    -> authenticate and identify PeerStream
    -> per-peer sequence/reorder window
    -> per-peer jitter/deadline queue
    -> per-peer remote-to-local clock mapper
    -> per-peer drift correction/resampler
    -> local-timeline audio block

Mix scheduler
    -> gather due block or explicit silence from each active peer
    -> wide accumulator
    -> one saturation step
    -> mixed playback ring
    -> audio callback output
```

### Reuse the mature path per peer

Each `PeerStream` should absorb the mature two-person behavior:

- Wrap-safe sequence tracking.
- Reorder recovery window.
- Jitter capacity and release deadlines.
- Missing, late, duplicate, and dropped frame accounting.
- RTT and jitter observations.
- Remote sample-time slope and drift estimate.
- Explicit resampler ratio and limits.
- Per-peer playout target and occupancy.

The current mesh `SequenceTracker` and peer counters can contribute to this component, but the mature normal-mode release and drift logic should be the behavioral baseline.

### Per-peer drift correction is mandatory for real mesh

Every audio interface has its own clock. With one remote peer, Jam2 can use one playback resampler ratio. With three remote peers, there are three remote clock domains relative to the local device.

Applying one ratio after mixing cannot correct them independently. A faster peer would accumulate while a slower peer starves, even if the aggregate mix depth looked acceptable.

Each `PeerStream` therefore needs its own drift estimator and resampler before mixing. Stats should expose per-peer:

- Raw and smoothed drift ppm.
- Applied ratio.
- Ratio clamp/deadband activity.
- Queue depth and target.
- Underrun/late/drop counts.

Aggregate stats can summarize minima, maxima, and totals, but must not replace per-peer measurements.

### Do not mix peers by raw sample number

Current mesh mixing assumes packets from different peers with the same `sample_time` belong in the same mix slot. That assumption is unsafe:

- Each engine starts its transmit sample counter independently.
- Peers can start at different wall times.
- Peers can reconnect or restart.
- Their device clocks drift independently.

Raw sender sample times are meaningful only within one peer stream. Each stream must map its sender timeline onto the receiver's local frame timeline before blocks can be mixed.

### Timestamp local capture explicitly

Today network `sample_time` starts at zero when a network loop begins, independently of the audio callback's `engine_frame_counter`. Attaching networking to an already-running local engine makes that split more visible.

Use either:

- Capture blocks tagged with their first local engine frame, or
- A stream epoch anchored atomically to a known callback frame, followed by strictly sequential capture accounting.

On network activation, discard stale local capture data and begin at a documented frame boundary. Do not send audio accumulated while networking was disabled.

## Timing and Metronome Model

### Time domains that must be explicit

The current code mixes several clocks:

- Local audio callback engine frames.
- Local outgoing packet sample time.
- Each remote peer's packet sample time.
- Monotonic microseconds used for packet scheduling and RTT.
- Metronome epoch frames.
- Prepared-track musical frames and render offsets.

The unified design should name conversions rather than treating similarly typed `uint64_t` values as interchangeable:

```text
LocalEngineFrame
RemoteStreamFrame(peer)
MonotonicTimeUs
GridPosition
GridRevision
```

No remote sample-frame value should be compared directly with a local engine frame without a `RemoteClockMapper` for that peer.

### Current two-peer asymmetry

The current shared-grid algorithm gives `listener_side` special behavior:

- The listener waits for RTT data and creates the initial future epoch.
- The connector aligns to the listener's bar.
- Shared-grid phase observations and compensation are applied asymmetrically.
- Listener-compensated mode assumes a distinguished remote reference.

This works as a two-person host/follower scheme but does not match “whoever starts becomes authority.”

### Current mesh asymmetry

Mesh replaces `listener_side` with `grid_coordinator`:

- The GUI sets it on the peer that created the session.
- Only that engine periodically sends metronome UDP state.
- Other engines wait for coordinator state and align to it.
- The role is fixed at process launch.

This is still permanent host authority, and it does not implement all shared-grid phase correction or listener compensation from the two-peer path.

### Proposed grid state

A grid state should contain at least:

```text
grid revision
authority peer id
running/stopped
metronome mode
pattern and BPM
authority epoch stream frame
authority packet frame used for projection
```

The authority chooses a future epoch after usable RTT observations exist. The lead should remain explicit and measurable, for example the current style of:

```text
max(500 ms, maximum relevant RTT + 200 ms)
```

Every receiving peer projects that authority epoch onto its local clock using its RTT/clock mapper and starts on a local future frame. Periodic authority state permits phase observation and bounded correction.

The bootstrap coordinator serializes authority changes, but the selected grid authority sends authoritative UDP grid timing directly to all peers.

### Shared-grid mode

In shared-grid mode:

- The peer that starts the grid becomes authority for that grid revision.
- Every peer renders its own local click from its mapped epoch.
- Only the authority originates authoritative grid state.
- Followers report their mapped offset and phase error.
- Correction remains bounded by explicit maximum, smoothing, deadband, and slew settings.
- A later pattern/BPM change creates a new future epoch/revision rather than mutating an ambiguous running timeline.

A peer joining an already running grid should receive the current revision and align at the next bar with adequate lead, not reset existing peers.

### Leader-audio mode

The requested semantics are a good fit for explicit grid authority:

- The peer whose GUI starts the metronome becomes `authority_peer_id`.
- That engine renders the click locally for its own output.
- That engine alone mixes the click into its outgoing mono network packet.
- The encoded packet is fanned out unchanged to every remote peer.
- Followers suppress local click rendering and hear the leader's click through the leader's remote stream.
- The bootstrap/session creator does not inject or relay the click unless it is also the selected authority.

The current two-peer path already contains the basic local-click/injected-click mechanism. The mesh send path currently omits click injection and must gain it before it can replace the normal path.

Authority changes need a new grid revision. If the leader disconnects, the safest v1 behavior is:

- Mark the leader-audio grid as `authority_missing`.
- Stop authoritative click audio rather than silently choosing another peer.
- Let a user explicitly start/take over, producing a new future epoch.

This keeps behavior measurable and avoids hidden leader election.

Send gain currently applies before injected leader click, so the click is not reduced by the leader's instrument send gain. That behavior should be either preserved intentionally or exposed as a separate technical control. At a follower, muting or reducing the leader peer will also affect the audible leader click; that consequence should be visible in per-peer mix controls.

### Listener-compensated mode

This mode is inherently follower-relative and should be redefined in terms of grid authority rather than listen/connect role:

- The grid authority renders the reference normally.
- Every non-authority peer compensates its local click relative to the mapped playback position of the authority's stream.
- Other remote peer streams are not used as the reference.
- If the authority stream is missing or stale, compensation becomes inactive and exposes a stale-reference counter/state.

The user-facing name can remain `listener-compensated` for compatibility, but internally the role is “non-authority follower.”

### Concurrent start requests

The GUI normally disables Start once it knows a grid is running, but requests can race across peers.

A small, deterministic solution is:

1. Any GUI sends a start request identifying its peer id.
2. The bootstrap coordinator orders requests and assigns the next grid revision.
3. The accepted requester becomes authority.
4. All peers receive the same assignment.
5. Stale revisions are ignored.

This is control-plane serialization, not an audio relay or permanent musical leader.

## Transport and Prepared Tracks

Current transport packets contain a revision and target sender frame. In mesh, revisions are tracked separately per source peer, but simultaneous local revisions from different peers do not define one session-wide order.

The unified design should include:

- Source peer id.
- Globally unambiguous event id, such as coordinator-assigned session revision or `(peer id, local counter)`.
- Associated grid revision.
- Target musical position.
- Authority/source frame used for projection.
- Explicit action.

Quantized actions should be expressed primarily against the shared grid, then mapped onto each local engine frame.

The current receive path accepts both `TrackRestart` and `RecordStart` but uses the remote action mainly to seek/play the prepared source; recording itself remains local. The refactor should document that split explicitly:

- Prepared-track restart may be shared.
- Arming and writing an input take remain local.
- A shared record countdown does not imply that another peer can write or control a local file without explicit local consent.

The session creator may remain the initial arrangement revision distributor
while grid authority remains dynamic, but every synchronized peer has equal
permission to originate arrangement actions.

## Session Configuration

The current GUI sends many “leader settings” before launching a joiner's engine. Some are true wire/session requirements; others should remain local choices.

### Immutable session contract

Peers must agree on values that affect wire interpretation or shared time:

- Protocol version.
- Network audio format.
- Sample rate.
- Network frames per packet.
- Session id/key and authenticated peer identity.
- Any future channel/stream format identifier.

Reject mismatches clearly during every peer handshake.

### Session admission policy

- The creator may set an optional peer limit when starting the jam.
- If omitted, authenticated membership has no application-wide count limit.
- The shared session controller rejects otherwise valid joins after the
  configured limit is reached.
- Pending unauthenticated connections, deadlines, and failed-key work are local
  safety limits and do not define the jam's authenticated peer capacity.

### Musical session state

These belong to versioned grid/arrangement state rather than process startup:

- Metronome mode and running state.
- BPM and pattern.
- Grid authority/revision/epoch.
- Shared transport and arrangement revisions.

### Local tuning

These can differ per peer and should not be silently dictated by the session creator:

- Audio device and channel selection.
- Device callback buffer size, subject to hardware.
- Capture/playback ring capacity.
- Local jitter/adaptive targets.
- Drift smoothing/deadband/correction limits.
- Socket buffer sizes.
- OS priority request.
- Local monitor, send, remote, metronome, and track levels.
- Stats and logging destinations.

The common frame size/sample rate must match, but local tolerance and device settings should remain explicit local controls. This is more consistent with equal UDP peers and heterogeneous hardware.

## Endpoint Discovery and NAT

The existing two-peer flow has a useful property: the connector initiates UDP to the listener, so the listener learns and locks the observed source endpoint. Current GUI mesh instead depends heavily on advertised manual endpoints and TCP peer-address substitution.

An always-mesh engine must preserve the reliable parts of the two-peer handshake:

- Each peer has a stable peer id plus one or more endpoint candidates.
- STUN remains endpoint discovery only, never an audio path.
- Peers perform authenticated UDP probing/handshake in both directions.
- The observed source endpoint can be promoted when it is valid.
- Membership becomes active only after the direct UDP path and stream contract are confirmed.
- No TURN or relay fallback is introduced.

For two peers, the joiner should still initiate to the invite endpoint. For additional peers, the coordinator distributes candidates and all pairs probe directly.

This does not eliminate NAT limits. CGNAT, symmetric NAT, or restrictive firewalls can still prevent one or more full-mesh edges. Report per-edge state explicitly:

```text
candidate known
probing
authenticated
stream compatible
active
timed out/rejected
```

A peer should not be reported simply as “connected” if only its TCP control link works and one or more required UDP mesh edges are missing.

## GUI Flow After Consolidation

The GUI no longer needs an engine-mode selector or experimental mesh checkbox.

### Base UI state

- Local engine/device status.
- Start Jam.
- Join Jam.
- Local metronome, track, recording, and mixer controls remain available.

### Start Jam

- Opens network/session settings.
- Creates the coordinator and invite.
- Leaves the local engine running.
- Shows zero or more peer-edge states and raw measurements.
- An optional creator-selected peer limit remains an explicit session setting.
  There is no application-wide authenticated-peer cap; pending and failed
  authentication work is bounded independently.
- Membership and diagnostic collections may grow during non-real-time peer
  admission/removal, while every `PeerStream`, packet queue, and mix timeslot
  remains independently bounded. Fixed hot-path storage must not create a
  hidden compile-time peer-count ceiling.

### Join Jam

- Accepts an invite.
- Shows session contract compatibility before joining.
- Restarts only the audio engine if a required sample-rate change is approved.
- Attaches the network session and displays each direct peer edge.

### Leave Jam

- Detaches networking.
- Returns immediately to local operation.
- Does not close the application or replace the engine.

### Authority display

The GUI should display raw, explicit roles:

- Session coordinator peer.
- Current grid authority peer and revision.
- Leader-audio source peer when applicable.
- Arrangement authority.
- Per-peer UDP edge and timing state.

Avoid a generic “host” label when the relevant role is more specific.

## One-Binary CLI Shape

The single public executable can expose the same engine model:

```text
jam2                         launch GUI
jam2 local [options]         headless/local engine, network disabled
jam2 network create [options]
jam2 network join <jam2-url> [options]
```

The exact command names can be finalized during implementation, but the internal distinction should be:

- `local`: network attachment absent.
- `network create`: local engine plus the permanent TCP membership/settings coordinator; TCP and UDP bind the same local numeric port.
- `network join`: authenticate to the creator URL, receive the immutable contract and current membership, then attach the same direct UDP session.

The completed migration retired the temporary mode adapters:

- `listen`, `connect`, `mesh`, and public static membership commands are no longer exposed.
- Deterministic stress uses create/join plus a debug-only inherited candidate-override seam for per-edge UDP proxies.
- Startup JSON should report lifecycle/session state rather than different audio-engine implementations.
- Existing Python harnesses migrated after parity testing rather than being removed.

## Benefits

### Simpler lifecycle

- The audio device is not stopped and reopened merely to start or leave a jam.
- Local tracks and metronome can survive network transitions.
- Peer membership changes do not restart the engine.
- Device, recording, and transport ownership stay in one RAII object graph.
- One stats stream describes local and network states.

### Better test coverage

- The common two-person case exercises the same per-peer implementation as mesh.
- Drift, jitter, authentication, and timing fixes are tested at every peer count.
- Headless multi-peer stress uses the public create/join bootstrap without a separate runtime engine.
- State-transition tests can cover local -> network -> local without process replacement.

### Better scaling discipline

- CPU, bitrate, packet rate, storage, drift, and timing remain visible per peer.
- Peer add/remove becomes a bounded state change.
- One packet encoder fans out to all peers.
- There is one place to implement allocation-free UDP improvements.

### Clearer product behavior

- Users choose Start Jam or Join Jam, not listen/connect/mesh engine internals.
- Two people remain the simplest workflow.
- Three/four-person sessions extend the same direct model.
- Musical leader can change by action without changing network topology.

## Costs and Risks

### Replacing the proven two-peer path

The largest extraction risk was accidentally discarding behavior from the former
`run_audio_packet_exchange` path while replacing the smaller mesh loop. The
retained parity and fault scenarios cover that regression boundary.

Mitigation: derive `PeerStream` from the mature two-peer behavior and use
one-peer parity evidence when investigating multi-peer behavior.

### Per-peer resampling complexity

Correctly mixing multiple independent device clocks is materially more complex than the current mesh implementation. Each peer requires bounded queueing, clock mapping, and resampling before mix.

This complexity is necessary for a robust full mesh; hiding it behind one aggregate playback ratio is not correct.

### Always-running local audio versus session contract

An already-running local device may not match a joined session's sample rate. Some buffers can change without device restart, but sample rate usually cannot.

Recommended policy:

- Starting a session uses the current active sample rate.
- Joining compares the contract first.
- A mismatch requires an explicit audio-engine restart or rejection.
- The application and project remain open during that restart.

### Capture attachment and stale audio

Networking cannot begin by draining a ring filled during local operation. Capture activation needs an explicit boundary and timestamp anchor.

### Authority and conflict handling

Allowing any peer to start the metronome requires ordered authority changes. A coordinator-serialized grid revision is small and deterministic, but it means loss of the control coordinator limits new membership and authority changes until reconnected.

Jam2 does not elect a replacement bootstrap coordinator. The creator emits a
small authenticated TCP heartbeat every 30 seconds. Five consecutive missed
check-ins form an approximately 150-second grace period in which the existing
reconnect path may recover and established direct UDP audio may continue. If the
same coordinator does not reauthenticate and resume heartbeats within that
period, GUI joiners stop the network session and return to Local; headless
`network join` commands terminate with an explicit coordinator-timeout result.
An authenticated creator-only End Jam message causes that transition
immediately. Lower-level UDP-only debug/engine cases do not participate in this
control-plane lifecycle. The 30-second/five-miss production default remains
native-owned; deterministic debug lifecycle scenarios may explicitly shorten
the interval without creating a Python-owned runtime default.

### NAT behavior

Making the full-mesh engine universal does not make every pair reachable. Additional peers introduce direct edges that the two-person join flow never needed. Two-person behavior must retain join-initiated probing, and per-edge failures must be visible.

### Control-plane fan-out

The current control plane is a star: every joiner connects to the creator GUI, and the server can broadcast, but messages received from a joiner are not generically rebroadcast to other joiners.

If any peer can become grid authority, the coordinator must explicitly validate and rebroadcast authority/settings changes, or the authoritative engine UDP message must carry the complete state. Relying on the current joiner-to-host message handling will not keep three/four GUIs consistent.

### Compatibility and benchmark migration

The final public interface does not retain compatibility aliases. Stress and
benchmark tools use `network create`, `network join`, and the explicit debug
surface over the universal mesh engine. Historical results remain useful for
comparison, but they are not completion gates.

## Shared Application Ownership Context

One non-UI session controller is shared by the GUI, ordinary headless commands,
and debug automation. `Engine` owns audio, callback time, local media, and
technical state. `NetworkSession` owns the UDP socket, peer streams, packet
scheduling, and mixing. The shared controller owns TCP bootstrap,
authentication, membership, the immutable session contract, and authority
coordination. UI code displays state and submits typed requests; it does not own
or duplicate session policy.

## Useful Validation Scenarios

### Lifecycle

- App/device start into local operation.
- Local metronome/track/recording before networking.
- Local -> create session with zero peers.
- Local -> join session.
- One peer joins and leaves while audio continues.
- Third/fourth peers join and leave without restarting existing streams.
- Leave session -> local with no stale remote audio.
- Rejoin a different session without restarting the application.
- Device/sample-rate restart on explicit join mismatch.

### One-peer parity

- Clean audio for every tuning profile.
- Packet loss, reorder, duplicate, late, jitter, and burst cases.
- Drift in both directions.
- Playback maximum and adaptive cushion behavior.
- Callback and receive-loop gaps.
- Recording stems and prepared tracks.
- Startup and shutdown behavior.

### Multi-peer clocking and mix

- Peers with independently simulated positive and negative drift.
- One weak/jittery peer while other peer streams remain stable.
- Missing peer block produces silence only for that peer.
- Late-after-release and excessive-future packet rejection.
- Deterministic saturation independent of arrival order.
- Per-peer mute/gain interactions and clipping stats.

### Metronome authority

- Session creator starts each mode.
- First joiner starts each mode.
- Third/fourth peer starts each mode.
- Concurrent start requests resolve to one revision/authority.
- BPM/pattern change creates a clean future revision.
- New peer joins a running grid at a bar boundary.
- Shared-grid phase correction remains bounded.
- Leader-audio click exists in exactly one outgoing stream.
- Leader-audio authority disconnects and reports missing authority.
- The ordered surviving peer creates a fresh epoch automatically; this is a
  grid-authority transition, never bootstrap-coordinator takeover.
- Listener compensation uses only the authority stream and reports stale reference.

### Transport

- Quantized restart initiated by different peers.
- Duplicate/stale/source-conflicting events.
- Grid revision changes around a scheduled action.
- Shared prepared-track restart versus local-only recording ownership.

### Connectivity

- LAN numeric endpoints.
- STUN-discovered two-person session.
- Joiner behind normal NAT initiating to creator.
- Three/four peers with all direct edges.
- One missing UDP edge while TCP control remains connected.
- Endpoint update and authenticated reprobe.
- No relay/TURN path.

### Unified interfaces

- `network create` and `network join` through the universal mesh engine.
- Existing stress, validation, and two-host benchmark workflows through the
  unified/debug surface.
- Typed debug/test startup and session events, opt-in automation manifests, and
  CSV consumers; ordinary GUI and direct CLI use do not require manifest
  output.

## Raw Measurements to Preserve

The unified mode should expose:

- Local engine state and device state.
- Network session state.
- Stable local peer id and peer count.
- Per-peer endpoint/path state.
- Per-peer sent/received packets, bytes, and bitrate.
- Per-peer loss, duplicate, reorder, late, replay, and capacity drops.
- Per-peer RTT and jitter.
- Per-peer reorder/jitter depth and high-water mark.
- Per-peer raw/smoothed drift ppm and resampler ratio.
- Per-peer missing frames and underruns.
- Mix-slot occupancy, missing contributors, late-after-release, and clipping.
- Grid authority id, revision, mapped epoch, phase error, and correction offset.
- Leader-audio source id or authority-missing state.
- Transport source/event/grid revisions.
- Callback gaps, xruns, ring depth, overruns, and underruns.
- Network-loop scheduling gaps and catch-up sends.

Aggregate data should supplement, not hide, per-peer behavior.

## Decisions and Recommended Defaults

The following defaults best match the current product constraints:

- Use one persistent local engine plus an optional `NetworkSession`.
- Use the same full-mesh audio path for one or many remote peers.
- Keep Start Jam and Join Jam as bootstrap workflows.
- Keep the session creator as the initial control/membership coordinator only.
- Treat all authenticated synchronized peers as equal musical and Track-view
  editors; coordinator ownership grants ordering/distribution responsibility,
  not edit authority.
- Let the initiator of a metronome start become grid authority for that revision.
- In leader-audio, only that authority injects and locally renders the authoritative click.
- Recover a departed running-grid authority through the existing ordered fresh
  survivor epoch; this is a musical transition, not bootstrap coordinator
  takeover.
- Keep arrangement revision distribution separate while allowing every
  synchronized peer to originate arrangement changes.
- Use authenticated TCP heartbeat expiry or an explicit creator End Jam to
  return joiners to Local; do not elect or transfer the bootstrap coordinator.
- Keep wire-required settings session-wide and tuning/mix/device settings local.
- Preserve join-initiated UDP probing and add symmetric per-edge handshakes.
- Require per-peer drift correction before claiming mesh parity.
- Generalize the mature two-peer engine; do not promote the present mesh loop unchanged.
- Expose only `network create` and `network join` for public network startup;
  no legacy mode aliases are retained after migration.

## Final Assessment

Consolidating the modes is worthwhile and should be part of the larger single-application refactor. The clean conceptual model is not really two exclusive modes; it is:

```text
Local Engine: always present when a device is active
Network Session: optionally attached, containing 0..N remote peers
```

The old `listen` and `connect` command names do not survive; their distinct
creator/joiner bootstrap responsibilities become `network create` and `network
join`. Mesh is the ordinary network topology rather than a user-selected
experimental engine. A two-person jam then becomes the smallest full mesh
naturally.

The major engineering work is not fan-out. It is preserving the mature two-peer buffering and timing behavior per remote clock, mapping every stream onto the local engine timeline, and replacing fixed listener/coordinator assumptions with explicit versioned musical authority. If those parts are handled first, the result should be simpler to operate and maintain while being technically stronger for both two-person and small-group use.
