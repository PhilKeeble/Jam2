# Mesh Network

Jam2 mesh mode lets more than two known peers exchange live audio in a direct UDP full mesh. It is intended for controlled technical testing with a small number of players, not for rooms, relays, accounts, or hosted audio.

The normal two-person mode is still the lowest-risk path for playing. Use mesh mode when you need every participant to hear every other participant directly and you can manage the extra endpoint, bandwidth, CPU, and tuning cost.

## When To Use It

Use mesh mode for:

- Testing how Jam2 behaves with more than one remote audio source.
- LAN or port-forwarded sessions where each participant has a reachable UDP endpoint.
- Comparing packet loss, jitter, RTT, underruns, overruns, and bitrate as peer count increases.
- Small experiments where a direct peer list is acceptable.

Do not use mesh mode when you need automatic room discovery, relay traversal, TURN fallback, moderation, account identity, or a server-mixed session. Jam2 does not add any relay audio path in mesh mode. Each peer still sends audio directly to the other peers.

## Network Model

Mesh mode uses a fixed-shape full mesh:

- Each peer runs one local `jam2 mesh` engine.
- Each engine has one UDP bind endpoint.
- Each engine receives a list of all other peer UDP endpoints.
- Each engine sends its local mono PCM audio packet to every listed peer.
- Received peer packets are mixed locally for playback.

For `N` peers, each peer sends to `N - 1` peers. Across the whole session, the live UDP audio paths grow as `N * (N - 1)`. There is no central audio mixer and no relay. This keeps the audio path direct, but it means scaling cost is paid by every participant and by every participant's network.

STUN is not part of the mesh audio path. The GUI mesh flow uses the GUI TCP control connection to exchange endpoint data and distribute the peer list. The UDP audio path remains direct between engines.

## GUI Launch Flow

The GUI is the normal way to start mesh mode.

### Host

1. Open `jam2-gui`.
2. Open **Start Jam**.
3. Set **Bind** and **Port** for the local UDP socket.
4. Set **Public endpoint host** to the host address other peers should use. On a LAN this is usually the host's LAN IP. Across the internet this must be a reachable public or forwarded address.
5. Enable **Mesh mode**.
6. Optionally set **Max mesh peers**. `0` means no GUI-enforced cap.
7. Choose the local audio device, channels, profile, sample rate, frame size, and other numeric tuning options.
8. Start the jam.
9. Send the generated `jam2://...` URL to each joining peer.

When mesh mode is enabled for **Start Jam**, the GUI launches:

```text
jam2 mesh --bind <host:port> --session-id <hex> --session-key <hex32> --peers ""
```

The empty peer list is valid for the initial host. The GUI then starts its TCP control server on the same session id and key. As peers authenticate, the GUI records their advertised UDP endpoints, broadcasts the full peer list, and restarts the local engine with the current `--peers` list when needed.

### Joiner

1. Open `jam2-gui`.
2. Open **Join Jam**.
3. Paste the host's `jam2://...` URL.
4. Set **Local UDP bind host** and **Local UDP bind port** for this peer.
5. Enable **Mesh mode**.
6. Choose the local audio device and channels.
7. Join.

The joiner connects to the host GUI over the authenticated TCP control connection, advertises its UDP endpoint, receives the current mesh peer list, then launches:

```text
jam2 mesh --bind <local-host:port> --session-id <hex> --session-key <hex32> --peers <comma-separated-peer-endpoints>
```

If a peer list changes, the GUI restarts the mesh engine with the updated list. This is expected behavior in the current implementation.

## CLI Usage

The CLI form is useful for headless tests and repeatable local stress runs:

```powershell
.\release\jam2.exe mesh --bind 0.0.0.0:49000 --session-id <hex> --session-key <hex32> --peers 192.168.1.20:49000,192.168.1.30:49000
```

Every peer must use the same session id, session key, sample rate, frame size, and compatible tuning values. Each peer's `--peers` value should include every other peer and must not include its own bind endpoint.

Use an explicitly empty list only when intentionally starting with no known peers:

```powershell
.\release\jam2.exe mesh --bind 0.0.0.0:49000 --session-id <hex> --session-key <hex32> --peers ""
```

## What To Watch While It Scales

Mesh mode exposes aggregate and per-peer stats. With periodic stats enabled, the engine prints `mesh_stats` plus one `mesh_peer` line per remote peer in text mode, or a JSON `mesh_stats` object with a `peers` array in JSONL mode.

Watch these values first:

- `peer_count`: confirms the local engine's current peer list size.
- `sent_packets` and `recv_packets`: should increase for each active peer.
- `sequence_lost`, `sequence_duplicate`, `sequence_out_of_order`, and `sequence_late`: show packet health.
- `rtt_avg_ms` and `jitter_avg_ms`: show network timing pressure.
- `playout_delay_frames` and jitter-buffer fields: show configured receive delay.
- `playback_ring_readable_ms`, underruns, and underrun events: show whether playback is starving.
- `input_peak`, `send_peak`, `remote_peak`, `output_peak`, and `output_clipped_samples`: show audio level and clipping behavior.
- CSV logs from **Log stats folder**: use these for comparing peer counts and profiles across runs.

As peer count grows, expect:

- Higher outbound bitrate on every peer because each local audio packet is copied to every remote peer.
- Higher inbound bitrate because every remote peer sends a separate stream.
- More local mixing work before playback.
- More sensitivity to one weak peer's route, because that peer still needs a direct path to every other peer.
- More engine restarts when people join while the GUI distributes updated peer lists.

Keep tests small and measurable. Increase peer count gradually, compare CSV logs, and tune with explicit numeric controls such as frame size, buffer sizes, playout delay, jitter buffer frames, and socket buffer sizes.

## Connection Considerations

Every peer needs a UDP endpoint that every other peer can reach. On a LAN, this usually means stable LAN IPs and local firewall rules allowing the chosen UDP ports. Across the internet, this usually means manual port forwarding or a known reachable public endpoint for each peer.

Because mesh mode does not add relay audio, a single peer behind CGNAT or restrictive NAT can fail to receive or send direct UDP with one or more participants. Use [Connection Test](ConnectionTest.md) before the session when routes are uncertain.

Avoid using `0.0.0.0` as the public endpoint host. It is valid as a local bind address, but other peers need a real address they can route to. In the GUI host flow, set **Public endpoint host** to the address peers should dial.

## Current Limits

- Mesh mode is experimental.
- Peer membership is distributed by the GUI control connection and engine restarts, not by a dynamic in-engine membership protocol.
- The host GUI is the control-plane coordinator for the peer list.
- There is no relay, TURN fallback, or server-side audio mixer.
- There is no subjective playability score. Use the raw stats and CSV logs.
- Large peer counts can exceed practical upload bandwidth, CPU, or route stability quickly.
