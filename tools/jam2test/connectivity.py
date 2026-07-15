from __future__ import annotations

import base64
import hashlib
import json
import socket
from pathlib import Path
from typing import Any

from .artifacts import InvocationArtifacts
from .connectivity_native import (
    bind_socket, decode_token, discover_mapping, encode_token, run_peer_probe,
    token_endpoint,
)
from .manifest import InvocationManifest


MAX_TOKEN_TEXT_BYTES = 64 * 1024
MAX_STUN_SERVERS = 8


def _token(sock: socket.socket, mode: str, public_endpoint: tuple[str, int] | None,
           mapped: list[tuple[str, int]], stable: bool,
           direct_host: str = "", name: str = "") -> dict[str, Any]:
    local = sock.getsockname()
    advertised_host = direct_host or ("127.0.0.1" if local[0] == "0.0.0.0" else local[0])
    machine_name = name or socket.gethostname()
    identity_source = f"{machine_name}|{advertised_host}|{local[1]}".encode("utf-8")
    stable_id = base64.urlsafe_b64encode(hashlib.sha256(identity_source).digest()[:9]).decode("ascii").rstrip("=")
    return {
        "version": 1, "tool": "jam2_test.py connectivity",
        "id": stable_id,
        "name": machine_name, "mode": mode,
        "local_endpoint": [local[0], local[1]],
        "direct_endpoint": [advertised_host, local[1]],
        "public_endpoint": list(public_endpoint) if public_endpoint else None,
        "mapped_endpoints": [list(value) for value in mapped],
        "mapping_stable": stable,
    }


def _validate_peer_token(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or value.get("version") != 1:
        raise ValueError("peer token has an unsupported shape or version")
    peer_id = value.get("id")
    endpoint = value.get("direct_endpoint")
    if not isinstance(peer_id, str) or not (1 <= len(peer_id.encode("utf-8")) <= 64):
        raise ValueError("peer token identity is invalid")
    if (not isinstance(endpoint, list) or len(endpoint) != 2 or
            not isinstance(endpoint[0], str) or not (1 <= len(endpoint[0]) <= 255) or
            not isinstance(endpoint[1], int) or isinstance(endpoint[1], bool) or
            not (1 <= endpoint[1] <= 65535)):
        raise ValueError("peer token direct endpoint is invalid")
    name = value.get("name", "")
    if not isinstance(name, str) or len(name.encode("utf-8")) > 256:
        raise ValueError("peer token machine label is invalid")
    return value


def run(args: Any, invocation: InvocationArtifacts,
        manifest: InvocationManifest) -> int:
    sock: socket.socket | None = None
    try:
        if args.connectivity_mode == "stun":
            if not 0.05 <= args.timeout_s <= 60.0:
                raise ValueError("--timeout-s must be from 0.05 through 60 seconds")
            if not 1 <= len(args.server) <= MAX_STUN_SERVERS:
                raise ValueError(f"one through {MAX_STUN_SERVERS} STUN servers are supported")
            if any(not isinstance(value, str) or not 1 <= len(value) <= 256 for value in args.server):
                raise ValueError("STUN server endpoints must be bounded non-empty strings")
        else:
            if not 0.1 <= args.duration_s <= 3600.0:
                raise ValueError("--duration-s must be from 0.1 through 3600 seconds")
            if not 0.01 <= args.interval_s <= args.duration_s:
                raise ValueError("--interval-s must be from 0.01 seconds through the probe duration")
            if len(args.name.encode("utf-8")) > 256:
                raise ValueError("--name exceeds its 256-byte bound")
            if args.direct_host and len(args.direct_host) > 255:
                raise ValueError("--direct-host exceeds its 255-character bound")
            if args.peer_token and len(args.peer_token.encode("utf-8")) > MAX_TOKEN_TEXT_BYTES:
                raise ValueError("--peer-token exceeds its 64 KiB bound")
        sock = bind_socket(args.bind)
        if args.connectivity_mode == "stun":
            results, stable = discover_mapping(sock, args.server, args.timeout_s)
            mapped = [tuple(item["endpoint"]) for item in results if item.get("endpoint")]
            result = {
                "mode": "stun", "bind": list(sock.getsockname()),
                "servers": results, "mapping_stable": stable,
                "raw_measurement": True,
            }
            status = "passed" if mapped else "failed"
        else:
            local = _token(sock, "direct", None, [], True, args.direct_host, args.name)
            token_text = encode_token(local)
            result = {"mode": "direct", "bind": list(sock.getsockname()), "local_token": token_text}
            token_path = invocation.root / "token.txt"
            token_path.write_text(token_text + "\n", encoding="utf-8")
            result["token_file"] = "token.txt"
            print("[connectivity] share token:", flush=True)
            print(token_text, flush=True)
            if args.peer_token:
                peer = _validate_peer_token(decode_token(args.peer_token))
                endpoint = token_endpoint(peer, "direct")
                result["peer_endpoint"] = list(endpoint)
                result["probe"] = run_peer_probe(sock, local, peer, "direct", args.duration_s, args.interval_s)
                status = "passed" if result["probe"]["peer_confirmed_local"] else "failed"
            else:
                status = "awaiting-peer"
                result["instruction"] = "run connectivity direct on the other machine with --peer-token, then repeat here with its token"
        path = invocation.root / "result.json"
        path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        manifest.add_case({"id": args.connectivity_mode, "status": status, "result": "result.json"})
        code = 0 if status in ("passed", "awaiting-peer") else 1
        manifest.finish(status, code)
        return code
    except Exception as error:
        manifest.add_case({"id": args.connectivity_mode, "status": "infrastructure-error", "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
    finally:
        if sock is not None:
            sock.close()
