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
uses adaptive retransmission timing and a congestion window. Default aggregate
sender budgets are 64 KiB/s for control and 128 KiB/s for asset data, with a
4096-byte burst allowance. These budgets do not reserve capacity on the physical
uplink; congestion effects still require measurement on real networks.

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

## Remaining experiments

Keep the creator-mediated asset star for now. Measure fan-out congestion and
audio timing under constrained upload bandwidth before considering peer-assisted
asset distribution. Investigate authority, ordering, collisions, content identity
and partial-mesh availability before changing that topology. Tune retry grace,
budgets and queue limits using raw network/audio measurements.
