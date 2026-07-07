# Connection Test Tool

`tools/connection_test.py` checks whether two peers can exchange UDP packets before running Jam2. It is useful for diagnosing firewall rules, port forwarding, public IP routing, LAN routing, and whether STUN hole punching is likely to work.

The tool does not run Jam2.

## Modes

- `stun`: uses Google STUN servers to discover each peer's public UDP endpoint, then tries a bidirectional UDP probe between the two public endpoints.
- `direct`: skips STUN and tests a manually reachable endpoint, such as `127.0.0.1`, a LAN IP, or a known public IP with port forwarding.

Both peers must run the tool at the same time. Each side prints a token. Paste the other side's token when prompted.

There is no timeout while waiting at the paste prompt. After a peer token is pasted, the UDP probe runs for 120 seconds by default. Override this with `--probe-seconds`.

## STUN Public Route

Use this to test whether two peers on different networks can connect without manual port forwarding.

Peer A:

```bash
python tools/connection_test.py --mode stun --name peer-a
```

Peer B:

```bash
python tools/connection_test.py --mode stun --name peer-b
```

Expected good result:

```text
[test] verdict: PASS - bidirectional UDP probe succeeded
```

Useful failure clues:

- `no STUN mapping discovered`: outbound UDP to the STUN server failed.
- `destination-dependent/symmetric NAT mapping`: STUN saw unstable mapped ports; STUN-only direct connection may not work reliably.
- `UDP filtering, firewall, CGNAT, stale token, same-host public-IP hairpin failure, or only one side probing`: STUN mapping was stable, but peer UDP did not arrive.

## Direct LAN Route

Use this to check whether two machines on the same LAN can exchange UDP packets directly.

Peer A:

```bash
python tools/connection_test.py --mode direct --bind 0.0.0.0:49001 --direct-host 192.168.1.10 --name peer-a
```

Peer B:

```bash
python tools/connection_test.py --mode direct --bind 0.0.0.0:49002 --direct-host 192.168.1.20 --name peer-b
```

Replace `192.168.1.10` and `192.168.1.20` with each machine's LAN IP.

If this fails, check the OS firewall, wrong LAN IP, Wi-Fi client isolation, or whether both sides were running at the same time.

## Same-Machine Sanity Check

Use this only to verify that the tool and local UDP sockets work. It does not test your router.

Terminal 1:

```bash
python tools/connection_test.py --mode direct --bind 0.0.0.0:49001 --direct-host 127.0.0.1 --name one
```

Terminal 2:

```bash
python tools/connection_test.py --mode direct --bind 0.0.0.0:49002 --direct-host 127.0.0.1 --name two
```

Exchange the printed tokens between terminals.

## Public IP With Port Forwarding

Use `direct` mode to test a known forwarded UDP port.

Peer behind the forwarded router:

```bash
python tools/connection_test.py --mode direct --bind 0.0.0.0:49001 --direct-host 81.86.171.138 --name forwarded-peer
```

Remote peer:

```bash
python tools/connection_test.py --mode direct --bind 0.0.0.0:49001 --direct-host REMOTE_PUBLIC_IP --name remote-peer
```

For this route, the advertised `--direct-host` should be the address the other peer can actually reach.

## Notes

- The default bind is `0.0.0.0:49001` so it does not conflict with Jam2's usual `49000` port.
- Use `--bind 0.0.0.0:0` to test an ephemeral UDP port.
- Use `--probe-seconds 180` or similar if token exchange will be slow.
- Same-machine STUN tests through your public IP may fail because many routers do not support UDP hairpin/loopback. Use `direct` mode with `127.0.0.1` for same-machine testing.
- On Windows, UDP reset events can appear if the other side exits first. The tool reports them but does not treat them as a crash.
- A `PASS` result means this simple UDP route works. It does not measure audio quality or latency.
