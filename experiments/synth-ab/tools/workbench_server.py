#!/usr/bin/env python3
"""Local-only static server and bounded native renderer for the sound lab."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


MAX_REQUEST_BYTES = 256 * 1024
MAX_RENDER_FILES = 96
RENDER_TIMEOUT_SECONDS = 25
STYLE_ROLES = frozenset({"chords", "melody", "bass", "support", "drums"})
DRUM_PIECES = frozenset(
    {
        "kick",
        "snare",
        "closed-hat",
        "open-hat",
        "high-tom",
        "mid-tom",
        "floor-tom",
        "crash",
        "ride",
        "cross-stick",
    }
)


def valid_style_mix_role(item):
    if (
        not isinstance(item, dict)
        or item.get("role") not in STYLE_ROLES
        or item.get("source") not in {"designed", "jam2-native"}
        or not isinstance(item.get("enabled"), bool)
        or not isinstance(item.get("selection"), dict)
    ):
        return False
    if item["source"] == "jam2-native":
        return item.get("parameterType") == "jam2-native" and item.get("parameters") is None
    parameters = item.get("parameters")
    if not isinstance(parameters, dict) or not isinstance(item.get("enabledLayers"), dict):
        return False
    if item["role"] != "drums":
        return item.get("parameterType") == "instrument-patch"
    selections = item["selection"].get("pieces")
    return (
        item.get("parameterType") == "drum-kit"
        and isinstance(parameters.get("bus"), dict)
        and isinstance(parameters.get("pieces"), dict)
        and set(parameters["pieces"]) == DRUM_PIECES
        and isinstance(selections, dict)
        and set(selections) == DRUM_PIECES
        and all(
            isinstance(selection, dict)
            and isinstance(selection.get("enabled"), bool)
            and selection.get("source")
            in {"selected-kit", "factory-piece", "saved-base-drum"}
            for selection in selections.values()
        )
        and set(item["enabledLayers"]) == DRUM_PIECES
    )


class WorkbenchServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, handler, site: Path, renderer: Path, qt_bin: Path):
        super().__init__(address, handler)
        self.site = site
        self.renderer = renderer
        self.qt_bin = qt_bin
        self.render_lock = threading.Lock()
        self.preset_lock = threading.Lock()
        self.preset_file = renderer.parent.parent / "presets" / "workbench-presets.json"
        self.style_mix_file = renderer.parent.parent / "presets" / "style-mixes.json"


class WorkbenchHandler(SimpleHTTPRequestHandler):
    server: WorkbenchServer

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(kwargs.pop("directory")), **kwargs)

    def do_GET(self):
        if urlparse(self.path).path == "/api/status":
            self.send_json(
                HTTPStatus.OK,
                {
                    "ok": self.server.renderer.is_file(),
                    "schema": "jam2-instrument-service-v1",
                    "serverVersion": 2,
                    "capabilities": ["render", "preset-save", "style-mix-save"],
                    "renderer": self.server.renderer.name,
                },
            )
            return
        super().do_GET()

    def do_POST(self):
        endpoint = urlparse(self.path).path
        if endpoint not in {"/api/render", "/api/preset", "/api/style-mix"}:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "Unknown endpoint."})
            return
        expected_origin = f"http://127.0.0.1:{self.server.server_port}"
        if self.headers.get("Origin") != expected_origin:
            self.send_json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "Origin is not the local workbench."})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            size = 0
        if size <= 0 or size > MAX_REQUEST_BYTES:
            self.send_json(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                {
                    "ok": False,
                    "error": f"Workbench request must be 1..{MAX_REQUEST_BYTES} bytes.",
                },
            )
            return
        raw = self.rfile.read(size)
        try:
            request = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Request is not valid JSON."})
            return
        if endpoint == "/api/preset":
            self.save_preset(request)
            return
        if endpoint == "/api/style-mix":
            self.save_style_mix(request)
            return
        if not isinstance(request, dict) or request.get("schema") != "jam2-instrument-patch-v1":
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Unsupported render schema."})
            return

        renderer_stat = self.server.renderer.stat()
        canonical = (
            json.dumps(request, sort_keys=True, separators=(",", ":")).encode("utf-8")
            + f"|renderer:{renderer_stat.st_size}:{renderer_stat.st_mtime_ns}".encode("ascii")
        )
        render_id = hashlib.sha256(canonical).hexdigest()[:24]
        output_folder = (self.server.site / "lab-renders").resolve()
        if output_folder.parent != self.server.site:
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": "Invalid render folder."})
            return
        output_folder.mkdir(parents=True, exist_ok=True)
        output_path = output_folder / f"{render_id}.wav"

        try:
            with self.server.render_lock:
                if not output_path.is_file():
                    with tempfile.NamedTemporaryFile(
                        mode="w",
                        encoding="utf-8",
                        suffix=".json",
                        prefix="jam2-instrument-",
                        delete=False,
                    ) as request_file:
                        json.dump(request, request_file, separators=(",", ":"))
                        request_path = Path(request_file.name)
                    try:
                        completed = subprocess.run(
                            [
                                str(self.server.renderer),
                                "--render-instrument",
                                str(request_path),
                                str(output_path),
                            ],
                            cwd=str(self.server.renderer.parent),
                            capture_output=True,
                            text=True,
                            timeout=RENDER_TIMEOUT_SECONDS,
                            check=False,
                            env={
                                **os.environ,
                                "PATH": str(self.server.qt_bin)
                                + os.pathsep
                                + os.environ.get("PATH", ""),
                            },
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                        )
                    finally:
                        request_path.unlink(missing_ok=True)
                    if completed.returncode != 0 or not output_path.is_file():
                        output_path.unlink(missing_ok=True)
                        detail = (completed.stderr or completed.stdout or "Native renderer failed.").strip()
                        self.send_json(
                            HTTPStatus.UNPROCESSABLE_ENTITY,
                            {"ok": False, "error": detail[:1000]},
                        )
                        return
                    native = json.loads(completed.stdout.strip().splitlines()[-1])
                else:
                    native = {"ok": True, "cached": True}
                self.trim_cache(output_folder)
        except subprocess.TimeoutExpired:
            output_path.unlink(missing_ok=True)
            self.send_json(HTTPStatus.GATEWAY_TIMEOUT, {"ok": False, "error": "Native render timed out."})
            return
        except (OSError, json.JSONDecodeError) as error:
            output_path.unlink(missing_ok=True)
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(error)[:1000]})
            return

        self.send_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "schema": "jam2-instrument-service-result-v1",
                "renderId": render_id,
                "url": f"/lab-renders/{output_path.name}",
                "native": native,
            },
        )

    def save_preset(self, request):
        if (
            not isinstance(request, dict)
            or request.get("schema") != "jam2-sound-design-preset-v1"
            or not isinstance(request.get("profileId"), str)
            or not isinstance(request.get("role"), str)
            or request.get("role") not in {"chords", "melody", "bass", "support", "drums"}
        ):
            self.send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "Unsupported sound-design preset."},
            )
            return
        key = f"{request['profileId']}/{request['role']}"
        try:
            with self.server.preset_lock:
                target = self.server.preset_file.resolve()
                experiment = self.server.renderer.parent.parent.resolve()
                if experiment not in target.parents:
                    raise OSError("Preset path escaped the experiment folder.")
                target.parent.mkdir(parents=True, exist_ok=True)
                document = {
                    "schema": "jam2-sound-design-preset-collection-v1",
                    "presets": {},
                }
                if target.is_file():
                    loaded = json.loads(target.read_text(encoding="utf-8"))
                    if (
                        isinstance(loaded, dict)
                        and loaded.get("schema")
                        == "jam2-sound-design-preset-collection-v1"
                        and isinstance(loaded.get("presets"), dict)
                    ):
                        document = loaded
                document["presets"][key] = request
                with tempfile.NamedTemporaryFile(
                    mode="w",
                    encoding="utf-8",
                    prefix="workbench-presets-",
                    suffix=".json",
                    dir=target.parent,
                    delete=False,
                ) as temporary:
                    json.dump(document, temporary, indent=2, sort_keys=True)
                    temporary.write("\n")
                    temporary_path = Path(temporary.name)
                os.replace(temporary_path, target)
        except (OSError, json.JSONDecodeError) as error:
            self.send_json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                {"ok": False, "error": str(error)[:1000]},
            )
            return
        self.send_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "schema": "jam2-sound-design-preset-save-result-v1",
                "key": key,
                "path": str(self.server.preset_file.relative_to(experiment)),
            },
        )

    def save_style_mix(self, request):
        roles = request.get("roles") if isinstance(request, dict) else None
        if (
            not isinstance(request, dict)
            or request.get("schema") != "jam2-style-mix-handoff-v1"
            or not isinstance(request.get("profileId"), str)
            or not request["profileId"]
            or not isinstance(request.get("styleId"), str)
            or not isinstance(roles, list)
            or not 1 <= len(roles) <= len(STYLE_ROLES)
            or len({item.get("role") for item in roles if isinstance(item, dict)})
            != len(roles)
            or any(not valid_style_mix_role(item) for item in roles)
            or not isinstance(request.get("mix"), dict)
            or not isinstance(request["mix"].get("roles"), dict)
            or set(request["mix"]["roles"]) != {item["role"] for item in roles}
            or any(
                not isinstance(mix_role, dict)
                or not isinstance(mix_role.get("enabled"), bool)
                or not isinstance(mix_role.get("gain"), (int, float))
                or not math.isfinite(mix_role["gain"])
                or not 0 <= mix_role["gain"] <= 8
                or (
                    "baseGain" in mix_role
                    and (
                        not isinstance(mix_role["baseGain"], (int, float))
                        or not math.isfinite(mix_role["baseGain"])
                        or not 0 <= mix_role["baseGain"] <= 8
                    )
                )
                or (
                    "trimDb" in mix_role
                    and (
                        not isinstance(mix_role["trimDb"], (int, float))
                        or not math.isfinite(mix_role["trimDb"])
                        or not -36 <= mix_role["trimDb"] <= 12
                    )
                )
                for mix_role in request["mix"]["roles"].values()
            )
        ):
            self.send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "Unsupported complete style mix."},
            )
            return
        key = request["profileId"]
        try:
            with self.server.preset_lock:
                target = self.server.style_mix_file.resolve()
                experiment = self.server.renderer.parent.parent.resolve()
                if experiment not in target.parents:
                    raise OSError("Style mix path escaped the experiment folder.")
                target.parent.mkdir(parents=True, exist_ok=True)
                document = {
                    "schema": "jam2-style-mix-handoff-collection-v1",
                    "styleMixes": {},
                }
                if target.is_file():
                    loaded = json.loads(target.read_text(encoding="utf-8"))
                    if (
                        isinstance(loaded, dict)
                        and loaded.get("schema")
                        == "jam2-style-mix-handoff-collection-v1"
                        and isinstance(loaded.get("styleMixes"), dict)
                    ):
                        document = loaded
                document["styleMixes"][key] = request
                with tempfile.NamedTemporaryFile(
                    mode="w",
                    encoding="utf-8",
                    prefix="style-mixes-",
                    suffix=".json",
                    dir=target.parent,
                    delete=False,
                ) as temporary:
                    json.dump(document, temporary, indent=2, sort_keys=True)
                    temporary.write("\n")
                    temporary_path = Path(temporary.name)
                os.replace(temporary_path, target)
        except (OSError, json.JSONDecodeError) as error:
            self.send_json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                {"ok": False, "error": str(error)[:1000]},
            )
            return
        self.send_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "schema": "jam2-style-mix-save-result-v1",
                "key": key,
                "path": str(self.server.style_mix_file.relative_to(experiment)),
                "absolutePath": str(self.server.style_mix_file),
            },
        )

    def end_headers(self):
        path = urlparse(self.path).path
        if (
            path == "/"
            or path.startswith("/lab-renders/")
            or path.endswith((".html", ".js", ".css", ".json"))
        ):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_json(self, status: HTTPStatus, value: dict):
        body = json.dumps(value, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    @staticmethod
    def trim_cache(folder: Path):
        files = sorted(
            folder.glob("*.wav"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        for old in files[MAX_RENDER_FILES:]:
            old.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--site", required=True)
    parser.add_argument("--renderer", required=True)
    parser.add_argument("--qt-bin", required=True)
    parser.add_argument("--port", type=int, default=48727)
    args = parser.parse_args()

    site = Path(args.site).resolve()
    renderer = Path(args.renderer).resolve()
    qt_bin = Path(args.qt_bin).resolve()
    if not site.is_dir():
        parser.error(f"site folder does not exist: {site}")
    if not renderer.is_file():
        parser.error(f"native renderer does not exist: {renderer}")
    if not (qt_bin / "Qt6Core.dll").is_file():
        parser.error(f"Qt runtime folder is invalid: {qt_bin}")
    if not 1024 <= args.port <= 65535:
        parser.error("port must be between 1024 and 65535")

    handler = lambda *handler_args, **handler_kwargs: WorkbenchHandler(
        *handler_args,
        directory=site,
        **handler_kwargs,
    )
    server = WorkbenchServer(
        ("127.0.0.1", args.port),
        handler,
        site,
        renderer,
        qt_bin,
    )
    print(f"Jam2 Sound Workbench: http://127.0.0.1:{args.port}/index.html", flush=True)
    print("Close this window to stop the local renderer.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
