# Direct Full-Mesh Network

Every Jam2 network session uses one bootstrap model and one direct UDP full-mesh audio engine. Two players are the primary and best-tested workflow. Three or four players use the same `NetworkSession` and per-peer `PeerStream` implementation; larger groups are experimental and limited by each machine and network rather than an application-wide peer cap. A jam creator may optionally configure a limit for that session.

## Create And Join Model

One participant creates the jam and remains its TCP coordinator. Other participants join the `jam2://...` URL published by that creator.

The TCP coordinator only handles session setup and control:

- authenticating the session id and key;
- checking the immutable protocol, sample-rate, and frame-size contract;
- assigning stable peer identities;
- distributing the current peer membership and each peer's UDP candidate;
- carrying revisioned grid/arrangement authority state and authenticated routed
  control messages.

The creator binds TCP and UDP to the same local numeric port. A joiner first authenticates to that TCP port, receives the contract and membership, then starts its UDP session. TCP never carries or relays audio.

Every proven UDP edge is direct peer to peer. Each engine sends its local mono PCM packet to every other active peer, receives one independent stream from each peer, and mixes those streams locally. For `N` peers, each peer sends to `N - 1` peers and the session has `N * (N - 1)` directed UDP paths.

There is no public static topology command, separate two-person transport, central audio mixer, relay, or TURN fallback.

## CLI Usage

Creator:

```powershell
.\release\jam2.exe network create --bind 0.0.0.0:49000 --public-endpoint 203.0.113.10:49000 --profile fast --audio-device 5 --sample-rate 44100
```

The creator prints a `jam2://...` URL. A joiner uses that URL:

```powershell
.\release\jam2.exe network join "jam2://..." --bind 0.0.0.0:49001 --profile fast --audio-device 16 --sample-rate 44100
```

Use `network create -h` and `network join -h` for the full option list. Device ids and channel selections are local. The coordinator rejects incompatible protocol, sample-rate, or frame-size contracts before UDP starts.

## GUI Flow

The GUI exposes the same create/join model as **Start Jam** and **Join Jam**:

1. The creator selects the device, channels, profile, bind port, and reachable public endpoint, then starts the jam.
2. Jam2 copies or displays the generated invite URL.
3. Each joiner pastes that URL, selects local device and channel settings, and joins.
4. The creator authenticates the joiner and distributes updated membership.
5. The existing audio engine adds or updates only the affected peer stream; unchanged audio devices and peer streams keep running.

The creator may optionally impose a session peer limit through the GUI or
`network create --max-peers`. `0` means no user-selected cap.

## Address Discovery And Reachability

STUN is used only to discover a candidate public UDP endpoint. It is never in the audio path and does not make an unreachable TCP or UDP port reachable by itself.

On a LAN, publish a stable LAN address. Across the internet, every participant needs a public or forwarded UDP endpoint reachable by every other participant. The creator also needs its TCP coordinator port reachable. Because the creator uses the same numeric TCP and UDP port, forward both protocols for that port when manual forwarding is required. Other peers need their selected UDP ports forwarded when NAT does not establish a usable direct mapping.

A peer behind CGNAT or restrictive/symmetric NAT can fail to form one or more direct edges. Jam2 deliberately provides no relay audio fallback. Use [Connection Test](ConnectionTest.md) when routes are uncertain, and never publish `0.0.0.0` as a peer candidate.

## Coordinator Loss

If the creator's TCP coordinator disappears, already proven UDP audio edges continue. Membership and shared coordinator state freeze because nobody else is promoted to coordinator. Joiners attempt to reconnect to the original creator; new participants cannot join and membership cannot change until it returns.

This keeps the failure behavior simple and avoids adding leader election or a room-service protocol.

## Measurements

Periodic stats include aggregate `mesh_stats` and one `mesh_peer` record per remote peer. Watch:

- explicit total/remote peer counts and active edges for membership and proven
  edge state;
- sent/received packets, loss, duplicate, reorder, late, and replay counters;
- per-peer RTT, jitter, playout depth, drift ppm, and resampler ratio;
- mixer released/deadline slots, missing frames, and capacity drops;
- playback ring depth, underruns, and overruns;
- revisioned transport source, action, requested target, and applied target;
- input, send, remote, and output peak levels;
- outbound and inbound bitrate as peer count grows.

CSV logs keep the same raw fields for regression comparisons. Increase peer count gradually and compare the per-peer routes: one weak route should be diagnosable without hiding it behind a subjective score.

Track transport replay counters are source-specific and remain monotonic across local network-worker reattachment. A repeated authenticated transport event is not accepted until the re-established direct UDP edge has enough clock mapping to schedule it; this preserves replay rejection without dropping the first Play, Stop, or Restart after rejoin.

## Practical Limits

- Two peers remain the main low-latency use case.
- Three and four peers are expected small-group experiments.
- Larger direct meshes have no artificial cap, but upload bandwidth, CPU, and route stability become practical limits quickly.
- Membership is coordinated over creator TCP; audio remains direct UDP.
- There are no rooms, accounts, relays, moderation services, or server-side audio mixing.
