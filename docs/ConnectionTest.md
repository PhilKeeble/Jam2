# Connectivity Tests

`tools\jam2_test.py connectivity` provides independent STUN mapping and direct
UDP reachability diagnostics. It does not launch Jam2, require an audio device,
or use the benchmark coordinator.

Run commands from the repository root.

## STUN Mapping

STUN reports the raw mapped endpoints observed through one local UDP socket and
whether repeated results are stable. It does not perform the peer-to-peer probe
itself.

```powershell
python tools\jam2_test.py connectivity stun --bind 0.0.0.0:0 --timeout-s 3
```

The default server is `stun.l.google.com:19302`. Repeat `--server HOST:PORT` to
query additional servers. One through eight server entries are accepted.

Inspect `result.json` for each server's endpoint/error and
`mapping_stable`. A failed mapping means no selected server returned a usable
endpoint; it does not diagnose the exact firewall/NAT cause or make a
playability recommendation.

## Direct UDP Probe

Direct mode is deliberately non-interactive. It uses two short invocations per
peer: first generate/share tokens, then run both probes with the other peer's
token.

### 1. Generate Tokens

Peer A (`192.168.1.10`):

```powershell
python tools\jam2_test.py connectivity direct --bind 0.0.0.0:49001 --direct-host 192.168.1.10 --name peer-a --duration-s 30
```

Peer B (`192.168.1.20`):

```powershell
python tools\jam2_test.py connectivity direct --bind 0.0.0.0:49001 --direct-host 192.168.1.20 --name peer-b --duration-s 30
```

Each command prints a share token and saves it as `token.txt` under that
invocation. With no `--peer-token`, the manifest state is `awaiting-peer` and
the command exits successfully.

### 2. Probe Simultaneously

Run both commands within the selected duration. Peer A receives Peer B's token:

```powershell
python tools\jam2_test.py connectivity direct --bind 0.0.0.0:49001 --direct-host 192.168.1.10 --name peer-a --peer-token "<TOKEN_FROM_PEER_B>" --duration-s 30 --interval-s 0.5
```

Peer B receives Peer A's token:

```powershell
python tools\jam2_test.py connectivity direct --bind 0.0.0.0:49001 --direct-host 192.168.1.20 --name peer-b --peer-token "<TOKEN_FROM_PEER_A>" --duration-s 30 --interval-s 0.5
```

A passing result requires `probe.peer_confirmed_local: true`: the peer received
this side's nonce and returned that confirmation. `result.json` also retains
`received_from_peer`, packet counts, ignored packets, reset events, and the peer
endpoint.

Do not use `--clean` between token generation and the probe if you want to keep
both invocations. Consecutive runs already receive unique invocation folders.

## Same-Machine Sanity Check

Use distinct local ports. This checks only the tool and loopback sockets.

Terminal 1:

```powershell
python tools\jam2_test.py connectivity direct --bind 127.0.0.1:49001 --direct-host 127.0.0.1 --name one --duration-s 20
```

Terminal 2:

```powershell
python tools\jam2_test.py connectivity direct --bind 127.0.0.1:49002 --direct-host 127.0.0.1 --name two --duration-s 20
```

Exchange the tokens, then rerun both with the other's `--peer-token` and the
same bind ports.

## Public Port Forwarding

Use direct mode with the local forwarded bind port and advertise the public IP
the other peer can reach. Both routers/firewalls must permit the chosen UDP
path. A same-network test through public addresses can fail when the router does
not support hairpin/loopback; use LAN addresses to distinguish that case.

## Artifacts And Interpretation

Defaults are isolated below:

```text
tools/connectivity_logs/<invocation-id>/
```

An explicit `--output PATH` creates `PATH/<invocation-id>` directly. The
invocation ID is a UTC minute timestamp, with a numeric suffix only for a
same-minute collision. Each run contains
`invocation-manifest.json` and `result.json`; direct runs also contain
`token.txt`. Without a custom output, `--clean` clears only
`tools/connectivity_logs`; with one, it clears the exact custom root.

Exit code `0` means STUN mapping succeeded, a direct probe passed, or a token
was generated and is awaiting its peer. Exit code `1` means the requested
mapping/probe completed without success. Exit code `2` means invalid input or
an infrastructure failure prevented a trustworthy test.

A direct pass proves that this simple bidirectional UDP route worked during the
probe. It does not measure latency, jitter, audio quality, or whether every
future NAT mapping will remain unchanged.
