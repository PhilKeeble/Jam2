# UDP multiplex implementation notes

The experimental implementation is on `UDP-Multiplex` in both repositories.
The reviewed design remains in [UDP-Muliplex.md](UDP-Muliplex.md).

## Running it

Use the normal Release build and `release/jam2.exe`. In Start Jam, choose
**Jam2 Server**. Its invitation points to `jam2gether.com:40404` and includes
`strategy=server`. Existing STUN/Direct invites and transports retain their
previous behavior. Existing saved preferences migrate to STUN or Direct;
new preferences default to Jam2 Server.

Run the companion Go binary with `--jam2-server --listen 0.0.0.0:40404`.
The Python sweep remains a separate explicit server mode. One process cannot
serve product and tokenless protocols simultaneously; use separate ports/processes
if both are needed.

For a controlled local run:

```powershell
.\release\jam2.exe network create --connection-strategy server --rendezvous 127.0.0.1:40404 --headless-audio on --test-input tone-440
.\release\jam2.exe network join 'THE_PRINTED_INVITATION' --headless-audio on --test-input tone-440
```

## Transport behavior

Each mesh edge owns one selected physical UDP socket. Five native HELLO
opportunities are spaced 250 ms apart, with a two-second reply grace after
the fifth, approximately three seconds total. Both directions must prove
HELLO/ACK connectivity. A failed pair rotates both sockets, up to ten distinct
pairs. Registration/READY/result coordination has separate bounded timeouts.
An established edge is retained when another edge joins or fails.

Existing native audio datagrams travel unchanged through the selected socket.
Audio bypasses reliable byte-stream queues. No DSP, mixer, resampler, jitter
buffer or audio-payload codec changes are part of this implementation.

Creator edges carry reliable ordered control and asset streams alongside audio.
Other edges carry the existing audio/link protocol. Control/asset framing and
authentication still use the native ControlClient/ControlServer, preserving the
creator's authority and current WAV distribution rules. Assets do not open a
TCP connection in Server mode.

The outer reliable datagram format uses `J2MX`, a fixed 42-byte header,
up to 1024 bytes of content and a 16-byte HMAC tag. Channel and stream identifiers
keep control and assets independent. Each logical queue is bounded at 256 KiB;
out-of-order receive storage is bounded by a 64-fragment window. Loss recovery
uses adaptive retransmission timing and a congestion window. The control sender
budget is 64 KiB/s with a 4096-byte burst allowance. The current experimental
default has no fixed asset rate cap (`asset_bytes_per_second=0`), including asset
ACKs; they do not consume the GUI control budget. A nonzero asset configuration
still enables the aggregate token bucket. Congestion windows, bounded queues,
chunk acknowledgements and bounded per-stream scheduling remain active.

The creator checks in every 15 seconds while admission is open. At capacity,
it finishes introductions and their acknowledgements before pausing. A small
closed lease record orders delayed directory commands and expires after 60
seconds without checkins. Vacancy resumes registration with the same invitation.
Admit/remove/reject messages have bounded acknowledgement/retry handling and
per-command ordering. A restarted creator signaling endpoint must answer a
fresh return-path challenge before replacing an old directory registration.
There is no additional runtime/session identity in invitations or media.

Leaving drains logical close notifications for at most 400 ms before destroying
the sockets. Session credentials retain their existing process-lifetime behavior;
the existing New Session button generates new credentials.

## Diagnostics and failure behavior

GUI automation snapshots and headless debug results include `udp_multiplex`:
signaling/audio counters, queue drops, byte budgets, per-edge selected/observed
ports, attempt, HELLO/ACK proof, pending bytes and retransmissions. Candidate and
active-link transitions are also logged.

A failed edge produces a nonfatal notification identifying the endpoints and
suggesting a manual strategy change and configured TCP/UDP port forwarding
(49000 by default). Existing edges keep running. Ten failed pairs establish a
failed connection attempt; they do not prove a particular NAT implementation
or explain its filtering behavior.

## Validation

Build with the repository's normal Release workflow. For the Go-dependent
native tests, build the real companion server and configure
`JAM2_RENDEZVOUS_TEST_SERVER`, or use the automatic Windows sibling-repository
artifact described in Jam2-Server/README.md.

Relevant native CTests:

- `jam2_udp_multiplex_units`: invitation compatibility, reliable fragments,
  packet identity and native audio-route/wake boundaries.
- `jam2_udp_control_transport_units`: native authentication, ordered control,
  binary contents, loss/reordering/duplication, bounded backpressure and
  independent logical stream lifecycle.
- `jam2_udp_four_peer_integration`: all six physical links, concurrent audio
  and asset traffic, existing-port preservation, capacity pause/resume and outage.
- `jam2_udp_four_peer_rotation_integration`: blocked first tuples plus lost
  directory requests/ACKs and multiplex packet loss/duplication.
- `jam2_udp_four_peer_partial_integration`: ten blocked B/C pairs, retained
  other links and endpoint/creator failure notifications.
- `jam2_udp_reused_invitation_integration`: same credentials restart while
  the old server record is retained.
- `jam2_udp_four_application_integration`: four public Release application
  processes, actual synthetic PCM, periodic CSV proof of three active remote
  audio peers, nonzero decoded/mixed audio.
- `jam2_udp_four_shared_content_integration`: the existing four-GUI-peer
  WAV/authority/conflict/reopen/recording workflow over UDP, with 96 KiB WAV
  fixtures and content integrity checks.

All eight UDP-multiplex CTests passed in the final scoped run. The existing
four-peer shared-content, WAV interruption, performance and session-command
workflows also passed, as did shared-grid metronome checks under clean,
loss and reordering conditions. Strategy GUI, preference migration, authority,
core/application boundaries and asset lifecycle checks passed in targeted runs. A separate
`jam2_four_session_dialog_integration` run reported six unclassified hidden
updater-dialog buttons. Updater code was unchanged; artifacts were retained in
the companion repository's `logs/udp-regression-failure` directory. This
independent inventory failure is not counted as a passing check.

Windows Release builds were validated. The portable socket/wake and reliable
stream sources also passed a POSIX compiler syntax check in Docker; this is not
a complete macOS application build. A native macOS build remains to be run.

Local UDP endpoint translators deliberately block selected tuples and impair
packets; they do not reproduce a carrier NAT. Broadband/hotspot testing remains
necessary. No public deployment or Internet connectivity result is implied by
the local checks.


## LAN testing option

Start the companion server with:

```sh
./rendezvous --jam2-server --allow-local --listen 0.0.0.0:40404
```

Use updated UDP-Multiplex builds on both peers, place them on the same LAN,
and choose Jam2 Server normally. No client flag or invitation format changes
are needed. Each media candidate includes the actual outbound interface address
and its exact bound socket port. Route discovery does not send an extra probe
or connect the real media socket.

When enabled, the server offers local endpoints on attempt one only when both
peers share the same observed public IPv4 and have valid local candidates.
A failed local attempt rotates both sockets and falls back to public endpoints.
The original ten-pair total bound remains. Without the flag, or when one peer
has no local hint, existing public rendezvous behavior remains.

Inspect link logs for `route=lan, remote=192.168?:PORT` (or a local address
appropriate to the test) and `udp_multiplex.links[].route` in diagnostics.
Sharing a public IP is a heuristic; HELLO/ACK proof decides whether the local
path actually works. The signaling socket continues its existing server
checkins, while the selected media socket carries audio/control/WAV directly.

Additional native coverage:
- `jam2_udp_four_peer_local_integration`: four native peers, all six direct LAN
  edges, control and binary assets, while simulated public mappings are blocked.
- `jam2_udp_four_peer_local_fallback_integration`: intentionally unusable LAN
  hints, fresh socket rotation and successful public endpoint fallback.
- `jam2_udp_four_application_local_integration`: four public Release Jam2
  processes, three logged LAN links each and measured decoded PCM.

## Failure recovery and diagnostics (25 October 2026)

The supplied Windows and Mac logs show control frame timeouts followed by
peer removal, rather than a process crash. A requested WAV failed local
validation on the Mac before transfer began; the old combined error does not
identify which validation condition failed.

Multiplexed framing now uses a **10-second idle deadline**, refreshed by new
ordered message bytes. A message can take longer overall while progressing.
TCP retains its existing framing policy. Reliable fragments are acknowledged
and retransmitted on the same UDP socket. Initial retransmission timing is one
second so ordinary mobile RTT can be measured without immediately classifying
every initial packet as lost; adaptive timing retains a scheduling margin.

A control-stream failure retains peer membership and the proven physical audio
socket. Recovery first opens/authenticates a fresh logical control stream on the
same socket. After two failed attempts, the creator coordinates a fresh server
introduction for that pair. Usable old audio sockets remain selected until a
replacement pair proves connectivity. Other peer pairs retain their sockets.
Existing-peer repair can wake the directory while admission is paused/full.
A persistent silent media edge can request creator-coordinated repair, including
B-C mesh links. Explicit leave still removes a peer; a transient logical control
fault is not treated as leaving. Exhausted ongoing repair rounds can retry.

Ensure-open requests are idempotent while a logical stream is opening/live.
Fresh authenticated stream identities can retire stranded old logical streams,
and outgoing open failures notify the controller instead of silently stalling.

An unavailable or changed source WAV now produces a specific validation reason
and authenticated `looper.asset.unavailable` control response. The receiver can
release/retry its request promptly rather than waiting silently for 60 seconds.
This does not reconstruct the original missing/mismatched Mac file.

The GUI log records structured `UDP multiplex stats` every two seconds:
current queues, stream identities, RTT/window sizes, retransmissions, input
deferrals, sent/received multiplexed bytes, pacing deferrals, send errors and
retained audio sockets. Audio CSV byte counters exclude multiplexed traffic;
raw arrangement JSON size and cumulative high-water marks alone cannot prove
a current flood or uplink saturation.

Native coverage includes slow progressing frames, ten-second idle expiry,
lossless 420 ms RTT without premature retransmissions, repeated asset opens,
exactly-four-peer WAV sharing under loss/duplication, same-socket recovery,
server fallback after two blocked opens, and automatic repair of a failed
non-creator mesh tuple. These controlled checks do not replace real macOS and
NAT testing. No DSP, audio codec, resampler, jitter-buffer or mixer changes
are part of these repairs.

## WAV-sharing synchronization repairs (25 October, later logs)

The later Windows `185149` / macOS `185309` logs show a model synchronization
failure rather than a jam-ending transport timeout. The creator imported a
2,375,302-byte WAV at 19:00:00.162 UTC. The Mac started receiving it at
19:00:01.441, submitted a track edit against received revision 11 at
19:00:12.010 while that revision was still awaiting the WAV, and cancelled the
transfer at 19:00:13.982 after the creator accepted the edit. Its displayed
arrangement was older than the revision it claimed as its edit base. The
creator consequently treated the absent imported lane as a deliberate removal.

Track proposals now identify their last **applied** authoritative revision, so
the existing three-way metadata merge can preserve a newer creator import.
Rebasing runs within the ordered control-message dispatch: deferring it to the
event loop allowed its following WAV offer to overtake it, then falsely
superseded that offer and released the sender's unpublished WAV too early.
Incoming scoped song updates also compose over the latest staged authoritative
model before asynchronous file validation. Idea and track updates received
during a download no longer repeatedly project over the older displayed model
and lose each other's changes. Outgoing initial WAV offers hold installation
of the authoritative snapshot until the offers have been reconciled, including
when publication starts during asynchronous validation.

Automatic initial sharing waits for authenticated membership, the creator's
sharing policy, and completion of the WAV compatibility audit. It is retriggered
when those prerequisites become ready. A read-only compatibility audit no
longer initiates a two-way share of every peer's displayed tracks: that feedback
loop could restore references that another peer had just removed. Actual local
resampling can still publish its converted assets. Explicitly opening a local
saved project also schedules automatic publication after its metadata audit,
including relative-path WAVs during an existing jam.

Reference identity, generation signature, tempo and stale state survive generic
Track Sync offers, with the same validation bounds as song metadata. Clearing
or replacing generated ideas publishes the resulting reference-lane removals
through the track scope as well as the idea scope. Custom WAV lanes remain.

Adopting a creator's jam name installs the validated incoming lanes before
relocating workspace paths. Installing them afterward restored paths into the
directory that had just been moved, leaving visible lanes with unreadable WAVs.

The receive indicator now reports accepted WAV bytes against the declared
file size. A served peer's normal on-demand asset-stream closure is not a
global retry error; the transient retry indicator clears when sharing is idle.
At the time of those logs, the asset budget was 131,072 bytes/s including
multiplex overhead: the observed 1,616,888-byte reference took about 13 seconds
and the imported 2,375,302-byte file required roughly 19 seconds. The subsequent
uncapped-transfer experiment below changes that budget. No DSP, audio codec,
resampler, jitter-buffer or mixer behavior is changed by these repairs.

The native `jam2_udp_four_sharing_races_integration` workflow uses exactly four
real app processes with fake audio and the real Go signaller behind injected
UDP network behavior. It checks a WAV retained across leaving a local jam and
joining another creator, hashes the actual files on every peer, holds a WAV at
the incoming-write boundary while a real lane rename and a scoped idea update
occur, verifies convergence without further user actions, and clears shared
references through the real confirmation dialog while retaining a custom WAV.
Failure artifacts retain per-peer model and presentation state. Unit coverage
checks accepted-byte progress, reference metadata round trips and invalid
reference fields. Real macOS/NAT validation remains a separate manual check.

Validated in the optimized Windows Release build (`release/jam2.exe`):

- `jam2_track_workspace_support_units` ??? passed.
- `jam2_asset_transfer_units` ??? passed.
- `jam2_listening_tools_units` ??? passed.
- `jam2_four_shared_content_integration` ??? passed (46.77 s).
- `jam2_udp_four_shared_content_integration` ??? passed (172.87 s).
- `jam2_four_wav_interruption_integration` ??? passed (37.23 s).
- `jam2_udp_four_recovery_integration` ??? passed (108.20 s).
- `jam2_udp_four_sharing_races_integration` ??? passed (38.91 s).

The interruption workflow's offer gate now recognizes a pending arrangement
asset as well as a pending contribution: a newer authoritative snapshot may
legitimately supersede the offer without releasing the pre-byte pause. Both
paths still exercise the real leave/rejoin and WAV ownership recovery.

## Uncapped WAV and slow audio startup experiment

The brief failed edge at 18:53:38.398 UTC was the audio runtime's proof deadline,
not a failed multiplex handoff. The Mac received its contract at 18:53:35.478
but reported its audio network runtime ready at 18:53:38.725. The creator's
eight probes plus response grace expired before that startup completed; its
edge became active about 200 ms after the failed status.

Server-mode media proof now continues on the same socket for up to 40 HELLOs,
spaced at least 250 ms apart, with the existing one-second final response grace. There
is no minimum wait: a matching ACK activates it immediately. Direct/STUN keep
their previous eight-attempt policy. The mesh diagnostic includes
`endpoint_proof_attempt_limit`. This changes network startup proof, not audio
processing.

For this branch's throughput experiment, asset data and asset ACKs bypass the
fixed byte-rate buckets by default. GUI control pacing, reliable congestion
control, bounded queues, the four-chunk application window and the transport's
per-stream scheduling quantum remain. Sent/received WAV log records include
`elapsed_ms` and `payload_bytes_per_second`; receive timing includes validation
and atomic commit, while send timing runs from validated source readiness
through queueing DONE.

The exactly-four-peer `jam2_udp_four_unthrottled_wav_integration` uses real app
processes and the real Go signaller. Only its test translator blocks media
HELLO/ACK packets for four seconds, simulating delayed audio attachment while
the physical handoff and control connection proceed. It requires no failed
proof state, then measures a 4,194,348-byte WAV reaching all three receiving
peers, checks file hashes, continued audio packet reception and zero multiplex
audio queue drops. The benchmark disables artificial loss/duplication; the
separate reliability and recovery tests retain those faults.

Measured local result: 10,898 ms end-to-end for the three recipients, or
1,154,619 aggregate payload bytes/s (about 1.10 MiB/s). This is a local
application/transport measurement, not an Internet throughput prediction.

All ten targeted native Release checks passed for this experiment: the new
unthrottled WAV test, asset-transfer units, UDP multiplex units, UDP control
transport units, four-peer transport, impaired rotation, four-peer public-mode
application, four-peer LAN application, WAV-sharing races and four-peer recovery.
The tested Windows executable is `release/jam2.exe`; the four-second delay is
exclusive to the test translator and is not a production startup delay.

## Remaining experiments

Keep the creator-mediated asset star for now. Measure fan-out congestion and
audio timing under constrained upload bandwidth before considering peer-assisted
asset distribution. Investigate authority, ordering, collisions, content identity
and partial-mesh availability before changing that topology. Tune retry grace,
budgets and queue limits using raw network/audio measurements.
