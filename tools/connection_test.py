#!/usr/bin/env python3

import argparse
import base64
import errno
import json
import os
import socket
import struct
import time
from pathlib import Path


MAGIC_COOKIE = 0x2112A442
STUN_BINDING_REQUEST = 0x0001
STUN_BINDING_RESPONSE = 0x0101
ATTR_MAPPED_ADDRESS = 0x0001
ATTR_XOR_MAPPED_ADDRESS = 0x0020
PROBE_PREFIX = b"J2CONN1 "


def parse_endpoint(value):
    host, port = value.rsplit(":", 1)
    return host, int(port)


def resolve_ipv4(endpoint):
    host, port = parse_endpoint(endpoint)
    results = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)
    if not results:
        raise RuntimeError(f"could not resolve IPv4 endpoint: {endpoint}")
    return results[0][4]


def encode_token(payload):
    data = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def decode_token(token):
    padded = token.strip() + "=" * (-len(token.strip()) % 4)
    return json.loads(base64.urlsafe_b64decode(padded.encode("ascii")).decode("utf-8"))


def stun_request(sock, server, timeout_s):
    transaction_id = os.urandom(12)
    request = struct.pack("!HHI12s", STUN_BINDING_REQUEST, 0, MAGIC_COOKIE, transaction_id)
    address = resolve_ipv4(server)
    deadline = time.monotonic() + timeout_s
    sock.sendto(request, address)
    while time.monotonic() < deadline:
        sock.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            data, source = sock.recvfrom(2048)
        except socket.timeout:
            break
        if source != address:
            continue
        mapped = parse_stun_response(data, transaction_id)
        if mapped:
            return mapped
    raise TimeoutError(f"no STUN response from {server}")


def parse_stun_response(data, transaction_id):
    if len(data) < 20:
        return None
    msg_type, msg_len, cookie, response_id = struct.unpack("!HHI12s", data[:20])
    if msg_type != STUN_BINDING_RESPONSE or cookie != MAGIC_COOKIE or response_id != transaction_id:
        return None
    offset = 20
    end = min(len(data), 20 + msg_len)
    while offset + 4 <= end:
        attr_type, attr_len = struct.unpack("!HH", data[offset:offset + 4])
        value_start = offset + 4
        value_end = value_start + attr_len
        value = data[value_start:value_end]
        if attr_type == ATTR_XOR_MAPPED_ADDRESS:
            mapped = parse_xor_mapped_address(value)
            if mapped:
                return mapped
        if attr_type == ATTR_MAPPED_ADDRESS:
            mapped = parse_mapped_address(value)
            if mapped:
                return mapped
        offset = value_end + ((4 - (attr_len % 4)) % 4)
    return None


def parse_mapped_address(value):
    if len(value) < 8:
        return None
    family = value[1]
    if family != 0x01:
        return None
    port = struct.unpack("!H", value[2:4])[0]
    host = socket.inet_ntoa(value[4:8])
    return host, port


def parse_xor_mapped_address(value):
    if len(value) < 8:
        return None
    family = value[1]
    if family != 0x01:
        return None
    xport = struct.unpack("!H", value[2:4])[0]
    port = xport ^ (MAGIC_COOKIE >> 16)
    cookie_bytes = struct.pack("!I", MAGIC_COOKIE)
    host_bytes = bytes(value[4 + index] ^ cookie_bytes[index] for index in range(4))
    host = socket.inet_ntoa(host_bytes)
    return host, port


def discover_mapping(sock, stun_servers, timeout_s):
    results = []
    for server in stun_servers:
        try:
            mapped = stun_request(sock, server, timeout_s)
            print(f"[test] STUN {server} mapped endpoint: {mapped[0]}:{mapped[1]}", flush=True)
            results.append({"server": server, "endpoint": [mapped[0], mapped[1]], "ok": True})
        except Exception as error:
            print(f"[test] STUN {server} failed: {error}", flush=True)
            results.append({"server": server, "endpoint": None, "ok": False, "error": str(error)})
    mapped_endpoints = [tuple(item["endpoint"]) for item in results if item.get("endpoint")]
    stable = len(set(mapped_endpoints)) == 1 if mapped_endpoints else False
    return results, stable


def make_probe_message(local_id, peer_id, nonce, seen_peer_nonce):
    payload = {
        "type": "hello",
        "from": local_id,
        "to": peer_id,
        "nonce": nonce,
        "seen_peer_nonce": seen_peer_nonce,
        "time": time.time(),
    }
    return PROBE_PREFIX + json.dumps(payload, separators=(",", ":")).encode("utf-8")


def parse_probe_message(data):
    if not data.startswith(PROBE_PREFIX):
        return None
    try:
        return json.loads(data[len(PROBE_PREFIX):].decode("utf-8"))
    except json.JSONDecodeError:
        return None


def token_endpoint(token, mode):
    if mode == "direct":
        if not token.get("direct_endpoint"):
            raise ValueError("peer token does not contain a direct endpoint")
        return tuple(token["direct_endpoint"])
    if not token.get("public_endpoint"):
        raise ValueError("peer token does not contain a STUN public endpoint")
    return tuple(token["public_endpoint"])


def run_peer_probe(sock, local_token, peer_token, mode, duration_s, interval_s):
    peer_endpoint = token_endpoint(peer_token, mode)
    peer_id = peer_token["id"]
    local_id = local_token["id"]
    nonce = base64.urlsafe_b64encode(os.urandom(9)).decode("ascii").rstrip("=")
    seen_peer_nonce = ""
    received_from_peer = False
    peer_confirmed_local = False
    received_packets = 0
    ignored_packets = 0
    reset_events = 0
    last_send = 0.0
    logged_receive = False
    logged_confirm = False
    confirm_linger_until = None
    deadline = time.monotonic() + duration_s
    sock.setblocking(False)
    print(f"[test] sending UDP probes to peer {mode} endpoint {peer_endpoint[0]}:{peer_endpoint[1]}", flush=True)
    while time.monotonic() < deadline:
        now = time.monotonic()
        if confirm_linger_until is not None and now >= confirm_linger_until:
            break
        if now - last_send >= interval_s:
            message = make_probe_message(local_id, peer_id, nonce, seen_peer_nonce)
            sock.sendto(message, peer_endpoint)
            last_send = now
        try:
            data, source = sock.recvfrom(2048)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        except ConnectionResetError:
            reset_events += 1
            time.sleep(0.01)
            continue
        except OSError as error:
            if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK) or getattr(error, "winerror", None) == 10035:
                time.sleep(0.01)
                continue
            if getattr(error, "winerror", None) == 10054:
                reset_events += 1
                time.sleep(0.01)
                continue
            raise
        payload = parse_probe_message(data)
        if not payload:
            ignored_packets += 1
            continue
        if payload.get("from") != peer_id or payload.get("to") != local_id:
            ignored_packets += 1
            continue
        received_packets += 1
        received_from_peer = True
        seen_peer_nonce = payload.get("nonce", "")
        if payload.get("seen_peer_nonce") == nonce:
            peer_confirmed_local = True
            if not logged_confirm:
                print("[test] peer confirmed receiving this side", flush=True)
                logged_confirm = True
            if confirm_linger_until is None:
                confirm_linger_until = time.monotonic() + 2.0
            if time.monotonic() >= confirm_linger_until:
                break
        if not logged_receive:
            print(f"[test] received peer UDP packet from {source[0]}:{source[1]}", flush=True)
            logged_receive = True
    return {
        "received_from_peer": received_from_peer,
        "peer_confirmed_local": peer_confirmed_local,
        "received_packets": received_packets,
        "ignored_packets": ignored_packets,
        "reset_events": reset_events,
    }


def bind_socket(bind):
    host, port = parse_endpoint(bind)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    return sock


def main():
    parser = argparse.ArgumentParser(description="Diagnose direct UDP and STUN hole-punch connectivity between two peers.")
    parser.add_argument("--mode", choices=("stun", "direct"), default="stun", help="stun tests public STUN-discovered endpoints; direct tests a manually reachable endpoint")
    parser.add_argument("--bind", default="0.0.0.0:49001", help="local UDP bind endpoint; use 0.0.0.0:0 to test an ephemeral port")
    parser.add_argument("--direct-host", default="", help="host/IP this peer should advertise for direct mode; defaults to local socket address")
    parser.add_argument("--name", default=socket.gethostname(), help="label included in the peer token")
    parser.add_argument("--stun-server", action="append", default=[], help="STUN server host:port; repeat to test mapping stability")
    parser.add_argument("--stun-timeout-s", type=float, default=3.0)
    parser.add_argument("--peer-token", default="", help="token printed by the other peer; omit to paste interactively")
    parser.add_argument("--probe-seconds", type=float, default=120.0)
    parser.add_argument("--probe-interval-s", type=float, default=0.1)
    parser.add_argument("--token-file", default="", help="optional path to write this peer's token")
    args = parser.parse_args()

    sock = bind_socket(args.bind)
    local_endpoint = sock.getsockname()
    print(f"[test] mode: {args.mode}", flush=True)
    print(f"[test] local UDP bind: {local_endpoint[0]}:{local_endpoint[1]}", flush=True)
    stun_results = []
    stable_mapping = False
    mapped = []
    public_endpoint = None
    if args.mode == "stun":
        stun_servers = args.stun_server or [
            "stun.l.google.com:19302",
            "stun1.l.google.com:19302",
        ]
        stun_results, stable_mapping = discover_mapping(sock, stun_servers, args.stun_timeout_s)
        mapped = [item["endpoint"] for item in stun_results if item.get("endpoint")]
        if not mapped:
            print("[test] verdict: FAIL - no STUN mapping discovered", flush=True)
            return 2
        public_endpoint = mapped[0]
    direct_host = args.direct_host or local_endpoint[0]
    if direct_host == "0.0.0.0":
        direct_host = "127.0.0.1"
    local_token = {
        "version": 1,
        "tool": Path(__file__).name,
        "id": base64.urlsafe_b64encode(os.urandom(9)).decode("ascii").rstrip("="),
        "name": args.name,
        "mode": args.mode,
        "local_endpoint": [local_endpoint[0], local_endpoint[1]],
        "direct_endpoint": [direct_host, local_endpoint[1]],
        "public_endpoint": public_endpoint,
        "mapped_endpoints": mapped,
        "mapping_stable": stable_mapping,
    }
    token = encode_token(local_token)
    print("", flush=True)
    print("[test] share this token with the other peer:", flush=True)
    print(token, flush=True)
    print("", flush=True)
    if args.token_file:
        Path(args.token_file).write_text(token + "\n", encoding="utf-8")
        print(f"[test] wrote token to {args.token_file}", flush=True)
    peer_token_text = args.peer_token.strip()
    if not peer_token_text:
        peer_token_text = input("[test] paste peer token, then press Enter: ").strip()
    peer_token = decode_token(peer_token_text)
    try:
        peer_endpoint = token_endpoint(peer_token, args.mode)
    except ValueError as error:
        print(f"[test] verdict: FAIL - {error}", flush=True)
        return 2
    print(f"[test] peer name: {peer_token.get('name', '')}", flush=True)
    print(f"[test] peer {args.mode} endpoint: {peer_endpoint[0]}:{peer_endpoint[1]}", flush=True)
    if args.mode == "stun":
        print(f"[test] local NAT mapping stable across STUN servers: {'yes' if stable_mapping else 'no'}", flush=True)
    result = run_peer_probe(sock, local_token, peer_token, args.mode, args.probe_seconds, args.probe_interval_s)
    if result["reset_events"]:
        print(f"[test] UDP reset events observed: {result['reset_events']}", flush=True)
    if result["peer_confirmed_local"]:
        print("[test] verdict: PASS - bidirectional UDP probe succeeded", flush=True)
        return 0
    if result["received_from_peer"]:
        print("[test] verdict: PARTIAL - received peer UDP, but peer did not confirm receiving this side", flush=True)
        return 1
    print(f"[test] verdict: FAIL - timed out after {args.probe_seconds:g}s with no peer UDP packets received", flush=True)
    if args.mode == "stun" and not stable_mapping:
        print("[test] likely cause: destination-dependent/symmetric NAT mapping", flush=True)
    elif args.mode == "stun":
        print("[test] likely cause: UDP filtering, firewall, CGNAT, stale token, same-host public-IP hairpin failure, or only one side probing", flush=True)
    else:
        print("[test] likely cause: wrong direct endpoint, local firewall, stale token, or only one side probing", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
