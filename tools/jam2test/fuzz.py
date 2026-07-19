from __future__ import annotations

import hashlib
import hmac
import io
import json
import random
import struct
import subprocess
import time
import wave
from pathlib import Path
from typing import Any

from .artifacts import InvocationArtifacts, normalized_path_id
from .manifest import InvocationManifest, sha256
from .native import NativeCapabilities
from .udp_protocol import PacketHeader, PacketType, encode_packet


MAX_ITERATIONS = 10_000
MAX_INPUT_BYTES = 1024 * 1024
MAX_CAPTURE_BYTES = 64 * 1024
MAX_RETAINED_FAILURES = 16
MAX_MINIMIZE_ATTEMPTS = 16


def _control_frame(message: dict[str, Any], sequence: int = 7) -> bytes:
    payload = json.dumps(message, separators=(",", ":"), sort_keys=True).encode("utf-8")
    body = bytearray(bytes((2, 1, 0, 0)) + sequence.to_bytes(8, "big") + bytes(16) + payload)
    body[12:28] = hmac.new(b"k" * 32, body, hashlib.sha256).digest()[:16]
    return len(body).to_bytes(4, "big") + body


def _control_seeds() -> list[tuple[str, bytes]]:
    first = _control_frame({"type": "beat.set", "lane": "chord", "section": 0,
                            "beat": 0, "text": "Cmaj7"})
    second = _control_frame({"type": "beat.set", "lane": "chord", "section": 0,
                             "beat": 1, "text": "kick"}, 8)
    return [("valid-json", first), ("valid-coalesced", first + second),
            ("fragmented-prefix", first[:3])]


def _udp_seeds(audio_format: str) -> list[tuple[str, bytes]]:
    key = bytes(range(16))
    session = 0x0102030405060708
    audio_width = 2 if audio_format == "pcm16" else 3
    payloads = {
        PacketType.HELLO: bytes(8), PacketType.HELLO_ACK: bytes(8),
        PacketType.AUDIO: bytes((index * 17) & 0xFF for index in range(64 * audio_width)),
        PacketType.PING: b"", PacketType.PONG: b"",
        PacketType.METRONOME_STATE: bytes(56), PacketType.BYE: b"",
        PacketType.TRANSPORT_STATE: bytes(20),
    }
    return [
        (f"valid-type-{packet_type}", encode_packet(
            PacketHeader(packet_type=packet_type, session_id=session,
                         sequence=100 + packet_type, timing_value=1000 + packet_type),
            payload, key))
        for packet_type, payload in payloads.items()
    ]


def _asset_seeds() -> list[tuple[str, bytes]]:
    data = b"RIFF" + bytes(range(64))
    frame = bytes.fromhex("ab" * 32) + struct.pack(">IQI", 0, 0, len(data)) + data
    return [("valid-chunk", frame), ("truncated-prefix", frame[:47]),
            ("declared-length-mismatch", frame[:44] + struct.pack(">I", len(data) + 1) + data)]


def _wav_seeds() -> list[tuple[str, bytes]]:
    output = io.BytesIO()
    with wave.open(output, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(48_000)
        handle.writeframes(b"\x00\x00\x00\x20\x00\xe0" * 32)
    valid = output.getvalue()
    return [("valid-pcm16", valid), ("truncated-header", valid[:20]),
            ("trailing-byte", valid + b"\x00")]


def seed_corpus(target: str) -> list[tuple[str, bytes]]:
    if target == "control": return _control_seeds()
    if target == "udp-pcm16": return _udp_seeds("pcm16")
    if target == "udp-pcm24": return _udp_seeds("pcm24")
    if target == "asset": return _asset_seeds()
    if target == "wav": return _wav_seeds()
    raise ValueError(f"unknown fuzz target: {target}")


def _mutate(source: bytes, rng: random.Random, maximum: int) -> bytes:
    data = bytearray(source)
    operation = rng.randrange(8)
    if operation == 0 and data:
        del data[rng.randrange(len(data)):]
    elif operation == 1 and data:
        index = rng.randrange(len(data)); data[index] ^= 1 << rng.randrange(8)
    elif operation == 2 and len(data) < maximum:
        index = rng.randrange(len(data) + 1)
        data[index:index] = rng.randbytes(min(maximum - len(data), rng.randint(1, 32)))
    elif operation == 3 and data:
        start = rng.randrange(len(data)); count = rng.randint(1, min(64, len(data) - start))
        data[start:start + count] = rng.randbytes(count)
    elif operation == 4 and len(data) >= 4:
        data[:4] = rng.choice((b"\0\0\0\0", b"\xff\xff\xff\xff", struct.pack(">I", maximum)))
    elif operation == 5:
        data = bytearray(rng.randbytes(rng.randint(0, min(maximum, 4096))))
    elif operation == 6 and data:
        data.extend(data[:min(len(data), maximum - len(data))])
    elif data:
        data[rng.randrange(len(data))] = rng.choice((0, 0x7F, 0x80, 0xFF))
    return bytes(data[:maximum])


def _signature(result: dict[str, Any]) -> str:
    if result["timed_out"]: return "hang:timeout"
    if result["return_code"] != 0:
        detail = (result["stderr"] or result["stdout"]).splitlines()[:1]
        return f"exit:{result['return_code']}:" + (detail[0][:160] if detail else "no-output")
    if result.get("output_overflow"): return "resource:output-overflow"
    if not isinstance(result.get("native"), dict) or result["native"].get("event") != "fuzz_result":
        return "invariant:invalid-native-result"
    return ""


def _run_one(jam2: Path, target: str, input_path: Path, timeout_s: float) -> dict[str, Any]:
    started = time.monotonic()
    try:
        process = subprocess.run(
            [str(jam2), "debug", "fuzz", target, str(input_path)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout_s, check=False,
        )
        stdout = process.stdout[:MAX_CAPTURE_BYTES].decode("utf-8", "replace")
        stderr = process.stderr[:MAX_CAPTURE_BYTES].decode("utf-8", "replace")
        overflow = len(process.stdout) > MAX_CAPTURE_BYTES or len(process.stderr) > MAX_CAPTURE_BYTES
        native = None
        if process.returncode == 0 and not overflow:
            try: native = json.loads(stdout)
            except json.JSONDecodeError: pass
        return {"return_code": process.returncode, "timed_out": False,
                "duration_ms": round((time.monotonic() - started) * 1000, 3),
                "stdout": stdout, "stderr": stderr, "output_overflow": overflow,
                "native": native}
    except subprocess.TimeoutExpired as error:
        return {"return_code": -1, "timed_out": True,
                "duration_ms": round((time.monotonic() - started) * 1000, 3),
                "stdout": (error.stdout or b"")[:MAX_CAPTURE_BYTES].decode("utf-8", "replace"),
                "stderr": (error.stderr or b"")[:MAX_CAPTURE_BYTES].decode("utf-8", "replace"),
                "output_overflow": False, "native": None}


def _minimize(jam2: Path, target: str, original: bytes, signature: str,
              scratch: Path, timeout_s: float) -> tuple[bytes, int]:
    candidate = original
    attempts = 0
    while len(candidate) > 1 and attempts < MAX_MINIMIZE_ATTEMPTS:
        trial = candidate[:max(1, len(candidate) // 2)]
        scratch.write_bytes(trial)
        attempts += 1
        if _signature(_run_one(jam2, target, scratch, timeout_s)) == signature:
            candidate = trial
        else:
            break
    return candidate, attempts


def run(args: Any, repo: Path, invocation: InvocationArtifacts,
        manifest: InvocationManifest) -> int:
    started = time.monotonic()
    scratch = invocation.root / "current-input.bin"
    try:
        if not 1 <= args.iterations <= MAX_ITERATIONS:
            raise ValueError(f"--iterations must be from 1 through {MAX_ITERATIONS}")
        if not 0.05 <= args.input_timeout_s <= 60:
            raise ValueError("--input-timeout-s must be from 0.05 through 60")
        if not 1 <= args.total_timeout_s <= 86_400:
            raise ValueError("--total-timeout-s must be from 1 through 86400")
        if not 1 <= args.max_input_bytes <= MAX_INPUT_BYTES:
            raise ValueError(f"--max-input-bytes must be from 1 through {MAX_INPUT_BYTES}")
        capabilities = NativeCapabilities(args.jam2)
        declared = set(capabilities.description.get("fuzz_targets", []))
        selected = {
            "all": ["control", "udp-pcm16", "udp-pcm24", "asset", "wav"],
            "control": ["control"], "udp": ["udp-pcm16", "udp-pcm24"],
            "asset": ["asset"], "wav": ["wav"],
        }[args.selection]
        if not set(selected) <= declared:
            raise RuntimeError("native debug description does not declare every selected fuzz target")

        failures = []
        target_results = []
        total_inputs = total_accepts = total_rejects = 0
        for target_index, target in enumerate(selected):
            derived_seed = (int(args.seed) + 0x9E3779B97F4A7C15 * (target_index + 1)) & ((1 << 64) - 1)
            rng = random.Random(derived_seed)
            seeds = seed_corpus(target)
            cases = []
            for index in range(args.iterations):
                if time.monotonic() - started > args.total_timeout_s:
                    raise TimeoutError("fuzz invocation exceeded its total time bound")
                seed_name, seed_data = seeds[index % len(seeds)]
                data = seed_data if index < len(seeds) else _mutate(seed_data, rng, args.max_input_bytes)
                data = data[:args.max_input_bytes]
                scratch.write_bytes(data)
                result = _run_one(args.jam2, target, scratch, args.input_timeout_s)
                signature = _signature(result)
                digest = hashlib.sha256(data).hexdigest()
                native_class = (result.get("native") or {}).get("classification", "")
                total_inputs += 1
                total_accepts += native_class == "accepted"
                total_rejects += native_class == "rejected"
                case = {"index": index, "derived_seed": derived_seed,
                        "corpus": seed_name, "input_bytes": len(data),
                        "input_sha256": digest, "duration_ms": result["duration_ms"],
                        "classification": signature or native_class}
                cases.append(case)
                if signature and len(failures) < MAX_RETAINED_FAILURES:
                    failure_id = normalized_path_id(f"{target}-{index}-{digest[:12]}")
                    failure_root = invocation.root / failure_id
                    failure_root.mkdir(parents=True)
                    original_path = failure_root / "input.bin"; original_path.write_bytes(data)
                    minimized, attempts = _minimize(
                        args.jam2, target, data, signature, scratch, args.input_timeout_s)
                    minimized_path = failure_root / "minimized.bin"; minimized_path.write_bytes(minimized)
                    (failure_root / "result.json").write_text(
                        json.dumps({"target": target, "signature": signature,
                                    "result": result, "minimization_attempts": attempts,
                                    "original_sha256": digest,
                                    "minimized_sha256": hashlib.sha256(minimized).hexdigest()},
                                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
                    failures.append({"target": target, "index": index,
                                     "signature": signature, "path": str(failure_root.relative_to(invocation.root))})
            target_results.append({"target": target, "derived_seed": derived_seed,
                                   "seed_corpus": [{"name": name, "bytes": len(data),
                                                    "sha256": hashlib.sha256(data).hexdigest()}
                                                   for name, data in seeds],
                                   "iterations": len(cases), "cases": cases})
            manifest.add_case({"id": target, "status": "failed" if any(
                item["target"] == target for item in failures) else "passed",
                "iterations": len(cases), "derived_seed": derived_seed})

        if scratch.exists(): scratch.unlink()
        results_path = invocation.root / "fuzz-results.json"
        results_path.write_text(json.dumps({
            "schema": "jam2-fuzz-results", "selection": args.selection,
            "master_seed": int(args.seed), "limits": {
                "iterations_per_target": args.iterations,
                "input_timeout_s": args.input_timeout_s,
                "total_timeout_s": args.total_timeout_s,
                "max_input_bytes": args.max_input_bytes,
                "max_native_input_bytes": capabilities.description.get("max_fuzz_input_bytes"),
                "max_captured_output_bytes": MAX_CAPTURE_BYTES,
                "max_retained_failures": MAX_RETAINED_FAILURES,
                "max_minimize_attempts": MAX_MINIMIZE_ATTEMPTS,
                "native_parser_allocation_bound": "input-sized with target-specific fixed caps",
            }, "native": {
                "executable": str(args.jam2.resolve()), "sha256": sha256(args.jam2.resolve()),
                "control_protocol_version": capabilities.description.get("control_protocol_version"),
                "udp_protocol_version": capabilities.description.get("udp_protocol_version"),
            }, "inputs": total_inputs, "accepted": total_accepts, "rejected": total_rejects,
            "failures": failures, "targets": target_results,
            "elapsed_s": round(time.monotonic() - started, 3),
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        passed = not failures
        manifest.finish("passed" if passed else "failed", 0 if passed else 1,
                        master_seed=int(args.seed), total_inputs=total_inputs,
                        accepted=total_accepts, rejected=total_rejects,
                        failures=len(failures), native_protocols={
                            "control": capabilities.description.get("control_protocol_version"),
                            "udp": capabilities.description.get("udp_protocol_version")})
        return 0 if passed else 1
    except Exception as error:
        if scratch.exists(): scratch.unlink()
        manifest.add_case({"id": "fuzz-infrastructure", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
