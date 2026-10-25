# UDP Multiplex feasibility plan

Status: implemented as an experimental feasibility build on both UDP-Multiplex branches. Windows Release validation completed on 2026-10-25; real-network testing remains. See [implementation and validation notes](UDP-Multiplex-Implementation.md).
Target Jam2 branch: `UDP-Multiplex`.
Companion server repository: `Jam2-Server`.
Planning date: 2026-10-25.

ALL WORK MUST ONLY BE DONE IN UDP-Multiplex BRANCH AND NOT TOUCH MAIN!

## Purpose and boundaries

Test whether one punched UDP socket per peer link can carry Jam2 audio and,
on creator links, reliable control messages and shared assets. Recover failed
introductions by rotating candidate sockets at both ends while preserving every
already working link.

This is an experiment in connection establishment and transport. Preserve the
audio processing, packet payloads, audio scheduling intent, jitter buffering,
mixing, resampling, clock/metronome behaviour and recording semantics. Changes to
network socket routing and packet dispatch must not become changes to audio DSP.
Do not claim that sharing a physical uplink can have zero congestion impact.

Keep the existing topology initially:

- Audio: direct full mesh between participants.
- GUI/session control: star, with the creator retaining its current authority.
- WAV/assets: existing creator-mediated distribution, carried on the same UDP
  sockets as audio/control rather than separate TCP connections.
- Creator-to-participant links carry all three logical traffic classes.
- Participant-to-participant links carry audio and the necessary connection and
  link-maintenance messages; they do not carry the GUI/control or asset star.

Keep STUN and Direct modes using their current transport and bootstrap behaviour.
The new multiplexed transport and server-assisted rotation apply to Jam2 Server
mode only. Do not silently migrate the other modes to the experimental transport.

## Connection dialog and invitation

Replace the current No STUN selection with a typed Connection strategy dropdown:

| Strategy | Strategy-specific fields | Behaviour |
| --- | --- | --- |
| Jam2 Server | No public-address/STUN fields | UDP rendezvous at jam2gether.com:40404 |
| STUN | Existing STUN server, timeout and retries | Existing STUN workflow and transports |
| Direct | Existing public-host input, labelled Your Public IPv4 Address | Existing explicit-public-endpoint workflow and transports |

Maximum peers remains available in every strategy. Preserve its current counting
semantics and the existing meaning of zero, rather than introducing an off-by-one
difference for the creator. Bind/port settings remain relevant to legacy modes;
Server mode must be able to choose fresh available local ports for candidate
sockets instead of forcing every peer link onto one fixed local port.

Hide the complete strategy-specific rows, including labels, when not applicable.
Apply the same typed settings consistently to the Start Jam dialog, saved
preferences, equivalent settings UI and the existing network create/join entry
points. Do not leave visible orphan controls or stale values overriding the
selected strategy. The default dropdown selection and exact placement of the
explicit new-session action remain UI details to settle during implementation.

A Server-mode invitation carries the jam2gether.com:40404 endpoint, the existing
session ID/key and an explicit strategy/protocol indication so a joiner does not
mistake the rendezvous server for the legacy creator TCP endpoint. Preserve
legacy invitation parsing. The exact extension/version is a protocol-design task.

### Session ID/key lifecycle: deliberately reusable invitations

Keep this as it is in the main branch, detailed below.
The creator retains the same session ID and key when leaving and restarting a jam
within the same running Jam2 application. The resulting Server-mode URL stays
the same, allowing existing participants to rejoin using their populated URL.

Generate new session credentials when the creator closes and reopens Jam2, or
explicitly chooses a new-session action. A joiner restarting its application
does not change the creator's credentials. Do not add disk persistence of the
creator's credentials unless separately requested.

Do not introduce an additional runtime/session identifier. Use the existing
session ID/key as the jam identity and the existing handshake challenges and
connection-attempt matching for individual introductions. Reusing an invitation
must not restore old membership, old connection state or delayed handshake
results. Preserve existing network authentication when changing its transport.

## Signaler lifecycle and admission capacity

The Go server is a directory and introduction coordinator, not an audio or asset
relay. Match participants to the intended session ID and key/proof instead of
pairing arbitrary waiting users. Keep live session records small and in RAM for
this experiment; database/storage choices are deferred.

Only the creator maintains persistent UDP registration:

1. Create/restart registers the session at jam2gether.com:40404.
2. While admission is open, the creator checks in every 15 seconds.
3. Joining peers contact the server temporarily for their introductions.
4. The creator tells existing peers over its established control links when to
   register candidate sockets for additional introductions.
5. Leaving the jam stops registration/checkins. Use existing peer-leave behaviour
   for the direct links.
6. After four missed 15-second checkins, expire the server record approximately
   60 seconds after its last valid creator checkin. Purge its session/key matching
   information and associated stale introduction state.
7. A restart may register the same session ID/key again, including before that
   old expiry. An old expiry or delayed cleanup must not delete a live registration.

The creator's signaling socket is separate from the per-link audio/multiplex
sockets. Rotating a candidate or closing one peer link must not stop the creator's
checkins or replace the other established sockets. Checkins must keep running
while introductions/retries are in progress.

### Stop server traffic while the jam is full

When the configured peer limit is reached, and pending introductions have
finished, the creator stops its persistent server checkins. Do not stop halfway
through the last admitted peer's remaining mesh introductions or retries.

A one-off admission-closed/unregister notification can retire the directory
entry promptly; the expiry remains the fallback if that message is lost. The
creator always enforces its peer limit locally, even if a stale directory entry
briefly remains visible. Never admit an extra peer solely because the server's
last view predates the limit being reached.

While full:

- Existing audio, control and asset links continue.
- The creator does not send periodic traffic merely to keep a directory record.
- The server may expire and forget the jam normally.
- An expired record cannot distinguish a full jam from an unavailable jam;
  do not promise an exact remote full/unavailable diagnosis without a record.

When someone leaves and capacity reopens, the creator re-registers the same
session ID/key and resumes 15-second checkins. This does not restart the jam,
rotate established sockets or change the invitation. An unlimited jam does not
pause registration on account of capacity. Serialize admission changes so
simultaneous joins cannot overfill the configured limit.

## Introduction and socket ownership

Every mesh link owns its selected local socket and remote endpoint. Connection
details belong to a peer pair; a participant does not have one universal audio
port that is substituted into every other participant's existing connection.

A link can use a dynamically selected local port. The server observes its public
UDP endpoint from packets sent by that exact candidate socket. Keep that socket
for subsequent direct traffic if it succeeds. Do not punch from one socket and
then move audio/control to a different fixed listening port.

For each new link:

1. Both candidates register with the server under their session and peer pair.
2. The server exchanges their observed endpoints.
3. A matching readiness barrier releases both peers for direct probing.
4. Each peer makes up to five native UDP HELLO/ACK connection checks.
5. If the pair fails, coordinate the failure and replace both candidate sockets
   with fresh, previously unused local ports.
6. Repeat for at most ten distinct socket pairs, including the initial pair.
7. Stop rotating as soon as mutual connectivity is proved; keep the successful
   pair for audio and the applicable multiplex channels.

Do not retry an unchanged failed socket pair. Preserve the peer identity and
jam identity through retries. Every attempt must reject stale endpoint offers,
readiness messages, HELLO challenges and acknowledgements from older attempts.
Keep missing-partner/server and handoff timeouts bounded; a missing handoff is
not the same observation as a completed handoff with no direct packets.

Five probes alone do not define how much time a slow path receives. The exact
HELLO interval and reply grace remain to be fixed and logged. A proposed starting
point is 250 ms between probes with a two-second reply grace after the last one,
giving roughly three seconds of direct-check time per failed socket pair.
This is a proposal, not an implemented or agreed timer setting.

The ten-pair limit is for the new Jam2 Server strategy. Do not silently replace
the Python sweep's existing three-attempt policy with this product policy.

### First peer B

A keeps its signaling registration while A/B prepare candidate sockets.
A/B complete their server introduction and direct checks. Their selected
socket pair then carries direct audio, reliable control and WAV/assets.
B does not need a permanent server keepalive after its introductions finish.

### Later peer C

1. C presents the invitation to the server, which notifies A.
2. A/C introduce and retry through the server while A/B remains active.
3. Once A/C works, A can admit C and exchange control over A/C.
4. A tells B and C to use the server temporarily for their B/C introduction.
5. B/C register their own candidate sockets; the server observes/exchanges their
   endpoints and coordinates retry handoffs.
6. Keep A/B and A/C sockets and their audio/control streams active while B/C rotates.
7. Repeat the per-link process for other existing participants as needed.

Use the server for B/C endpoint observation/introduction, rather than relying on
A behind NAT to receive unsolicited probes from their fresh sockets. A remains
the application coordinator. Do not give B the working A/C socket as though it
were already a proved B/C connection.

Multiple introductions need unambiguous peer-pair/attempt matching and bounded
pending work. They must not overwrite another introduction's endpoints or turn
a failure on one edge into a reset of every link in the jam.

## Partial meshes, failure messages and leaving

If a new link exhausts ten socket pairs, preserve every working link. In
particular, if A/C works but B/C fails, A/B and A/C keep running. C remains
connected to A; do not automatically eject C or roll back its working link.

Show a clear notification to A, B and C identifying the failed peer pair and
explaining that C could not complete its full-mesh join. Explain that existing
links remain active and participants can discuss the issue, then manually end
and restart using another strategy.

Suggested message intent:

> C could not complete the direct connection to B after 10 socket attempts.
> The current jam is still running, but B and C cannot exchange audio directly.
> Try restarting with STUN and forwarding the configured jam port (TCP and UDP 49000 by
> default), or use an appropriate Direct configuration.

Keep the cause wording accurate: ten failures do not by themselves prove a
specific NAT diagnosis. Because STUN/Direct retain their existing TCP control and
UDP audio transports, fallback guidance needs both TCP and UDP forwarding for
the configured creator port. STUN discovers an endpoint; it does not configure
a router. Do not instruct users to forward 49000 while actually binding the
legacy jam to a different configured port.

If A/C never establishes, deliver the join failure to C through its temporary
server exchange and let A notify existing participants. Deduplicate failure
notifications; do not present repeated popups for each delayed retry message.

A partial mesh must expose which links are active/failed and the existing
authority restrictions. Features requiring a complete mesh must not silently
claim full connectivity. Preserve normal peer-leave handling: remove the departing
peer's links, cancel its pending introductions/transfers, update membership and
resume creator server registration if its departure reopens capacity. Retain the
current behaviour when the creator leaves; host migration is not part of this plan.

## Reliable control and asset multiplex

Replacing TCP requires more than adding a packet-type byte. Retain the observable
application semantics of the control protocol and asset transfer system:

- Reliable delivery and appropriate ordering, duplicate suppression and bounded
  retransmission/acknowledgement handling.
- Existing creator authority, peer/session authentication and message validation.
- Explicit framing and separation of audio, connection management, control and
  bulk asset traffic on each shared physical socket.
- Separate logical progress for control and bulk transfers, so a missing WAV
  fragment does not block membership, leave, transport or other urgent commands.
- Fragmentation/reassembly of large control frames and assets into bounded
  datagrams, with size/count/time limits and transfer-integrity checks.
- Bounded queues, in-flight data, reassembly memory and work per network-loop turn.
- Backpressure and cancellation when peers leave or transfers fail.
- Visible queue, retransmit, RTT, throughput, drop and transfer-progress counters.

The current control protocol permits ordinary frames up to 64 KiB and large JSON
messages up to 4 MiB. Existing application chunks are not automatically safe UDP
datagram sizes. Define a transport fragment size that avoids relying on IP
fragmentation; preserve the audio payload format rather than enlarging/repacking
audio datagrams to carry unrelated control bytes.

Perform reliability, fragmentation, pacing and allocation outside real-time audio
callbacks. Keep audio independent of reliable-message acknowledgement waits.
Bound and pace control/bulk sends so retry or asset traffic cannot monopolize the
network loop, socket queue or creator's uplink. Preserve timely control progress
as well as audio progress.

Current branch experiment: the fixed asset byte-rate cap is disabled to measure
natural multiplex throughput. Reliable congestion control and bounded scheduling
remain active; logs record actual payload bytes/s and elapsed transfer time.
Revisit the aggregate budget below using the resulting real-network tests.

Apply a creator-wide outgoing asset budget across all recipients, not a separate
unrestricted full-rate budget per peer. Sending a WAV to six participants can
multiply creator uplink traffic even though every transfer shares its recipient's
audio socket. Scheduling policy, fragment sizes and rate limits must be measurable
and revisited using actual audio/control results, not assumed harmless.

No separate TCP asset connection is acceptable in Server mode. Do not require a
second successful hole punch for a second logical stream. No media/asset relay,
database or WebSocket migration is part of this experiment.

Reference: [UDP usage guidelines, including congestion and message sizing](https://www.rfc-editor.org/rfc/rfc8085.html).

## Preserve the independent network sweep

The Go implementation must retain the existing tokenless/keyless Python sweeps
for testing other networks. Keep this as an explicit test mode, isolated from
session-scoped product registrations; test peers must not match a Jam2 session
or consume its admission slots.

Preserve the current 39-trial sweep, three-attempt socket policies, timing
variations and A/B/A comparison unless a separate sweep change is requested.
The two latest completed sweeps produced 31/39 and 28/39 overall, respectively.
Both-rotating passed 6/6 in each, both-fixed passed 6/6 in each, and A/B/A
passed in both. These small samples support further experiments; they do not
establish universal connection success or identify the failure mechanism.

Build and test the Go server through Docker. Keep the Python sweep usable
without building/installing Jam2 and without requiring a product session/key.

## Implementation sequence and validation gates

Each stage needs the relevant native Release CTests; a successful compile alone
is insufficient. Keep production responsibilities in their owning components,
not in MainWindow simply because a GUI action reaches them.

1. Define typed connection strategy, invitation compatibility, reusable
   credentials, socket ownership and per-link introduction states.
2. Extend Go session-scoped signaling, admission/expiry and server-assisted
   per-pair retries while preserving explicit sweep mode. Docker-test it.
3. Add multiple link sockets to the native network session and route existing
   audio through the selected socket for each peer without DSP changes.
4. Implement and unit-test reliable control/bulk framing, bounded reassembly,
   sequencing, retransmission, cancellation and scheduling.
5. Integrate existing control/asset responsibilities with the Server-mode UDP
   transport; preserve existing application authority and legacy TCP paths.
6. Connect the strategy UI, equivalent headless flows, stable invitation
   lifecycle, explicit new-session action and failure/partial-mesh reporting.
7. Extend exactly-four-peer native integration tests and controlled impairment
   cases. Only then move to the user's real-network tests.

Implementation was explicitly authorized after review of this document. The work remains incomplete until the product flows and relevant validation below pass.

### Native CTest coverage

- Strategy-specific row visibility, preferences migration, invitation round trips,
  legacy mode compatibility and explicit new-session behaviour.
- Same invitation across creator leave/restart; different credentials after
  explicit renewal/application restart; no restoration of old peers or traffic.
- Session/key isolation, wrong/stale messages and tokenless sweep isolation.
- Five HELLO opportunities, fresh sockets on permitted retries, success on later
  attempts, exactly ten-pair exhaustion and bounded coordination failures.
- Loss/duplication/reordering of registration, introductions, READY releases,
  HELLO/ACK and retry commands; no false connectivity from delayed attempts.
- C/D joining while existing links stream; failed candidates never close or
  rebind active audio/control sockets.
- Partial mesh retained after B/C failure; one clear notification per failure;
  existing authority and leave/cancellation behaviour maintained.
- Reliable control under loss/reordering, bounded fragmentation, content integrity
  and separation of urgent control from blocked/retrying assets.
- Four-peer WAV sharing/fan-out under constrained bandwidth and loss while audio,
  metronome, transport and GUI commands continue; cancellation/interruption cases.
- Preserve existing relevant four-peer shared-content, session-command,
  metronome/epoch, performance/fake-audio and network-impairment workflows.
- Compare audio payload/PCM integrity and sequence behaviour with controlled fake
  audio. Measure packet timing, loss, jitter, queue depths and underruns; distinguish
  injected impairment from additional transport/scheduler problems.
- Keepalive every 15 seconds while admission is open; expiry after 60 seconds;
  leave stops traffic; full capacity pauses traffic; vacancy resumes it with the
  same URL and no audio reset. Cover simultaneous joins, pending introductions,
  delayed pause/resume messages and a server record expiring while full.
- Server outage/record expiry must not terminate already established peer streams.

### Local simulation and real-network validation

Local CTests can validate the real protocol/engine without reproducing carrier
NAT. Docker does not automatically reproduce a 4G NAT or its admission behaviour.
Use controlled UDP impairment/NAT-style proxies or isolated test networks to
exercise endpoint changes, blocked tuples, asymmetric admission, packet loss,
delay and reordered messages.

Include cases where a particular candidate tuple stays blocked despite repeated
probes, while a fresh pair is admissible. Include simultaneous/staggered joins,
multiple jams, full-to-open admission and delayed messages from a previous join.
Do not depend on local hairpin punching as proof of Internet behaviour.

Use deterministic fake audio and the actual native implementation, not a Python
reimplementation of product transport. Docker-based Go tests and the independent
Python sweep supplement native CTests; they do not replace them.

Build/test the optimized Release configuration through the repository's normal
commands and release/jam2.exe. Run the directly relevant CTests; run the full
suite only when explicitly requested. Real-network testing by the user remains
necessary across broadband, hotspot/4G and other available networks.

## Deferred investigations

### Distributing assets over the mesh

Keep creator-mediated control and asset authority in this first experiment.
Record and measure the creator bottleneck when one peer uploads a WAV and the
creator redistributes it to six or more participants.

A later experiment could use direct peer-to-peer asset delivery or peer-assisted
fan-out, with reliable logical channels on other mesh edges. This does not
automatically require decentralizing every control decision: retain a distinction
between authorizing a change and distributing its bytes.

Any broader change needs explicit rules for publisher/creator authority,
concurrent edits, ordering, duplicate/collision handling, content identifiers,
integrity, late joiners, cancellation and partial-mesh availability. Do not
silently introduce these topology/protocol changes in the initial transport work.

### Other measured questions

- Control/asset pacing, aggregate fan-out limits, fair progress and maximum useful
  fragment/in-flight sizes without extra audio interruption.
- HELLO interval/reply grace and whether ten socket pairs is an appropriate bound
  on different real networks.
- Whether known-good pairs remain usable after longer idle gaps, introductions,
  uplink changes or transient server outages.
- Admission-pause behaviour and join feedback when full jams have no live
  directory record.
- Creator-only signaling footprint, live-record expiry and eventual storage
  choices after reliable connection behaviour is demonstrated.
- Connection/transfer statistics that identify the failed stage without asserting
  an unproved NAT or firewall cause.
- Public-service exposure and hardening beyond existing authentication/basic
  bounds, after the controlled feasibility work.


### NOTES FROM PHIL - DO NOT TOUCH

look into the datagram spec for the control plane and how we can make it the most efficient messages possible
look into the start and mesh topology, i think the multiplex should allow this to be a true mesh only with the creator only being reliable for pulling people into the mesh, but needs careful arrangement and ordering around conflicting controls that might be coming in from multiple peers in a safe way, it also needs to handle wav transfers in a way that avoids congestion 
look into the cotnrol planes response handlers and how we are controlling acknowledgement and receipt

when joining a jam you need to choose a bind port / ip even when joining a jam2 server one

output from the jam2 server can stop spamming to make it clearer

