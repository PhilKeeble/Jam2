#!/usr/bin/env python3
"""Tiny LAN upload server for collecting Jam2 logs from another device."""

from __future__ import annotations

import argparse
import html
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote


FORM = b"""<!doctype html>
<html>
<head><meta charset="utf-8"><title>Jam2 Upload</title></head>
<body>
<h1>Jam2 Upload</h1>
<form method="post" enctype="multipart/form-data">
<p><input type="file" name="file"></p>
<p><button type="submit">Upload</button></p>
</form>
</body>
</html>
"""


def safe_name(raw: str) -> str:
    name = Path(unquote(raw)).name.strip()
    keep = []
    for ch in name:
        keep.append(ch if ch.isalnum() or ch in "._- " else "_")
    cleaned = "".join(keep).strip(" .")
    return cleaned or "upload.bin"


class UploadHandler(BaseHTTPRequestHandler):
    upload_dir: Path

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.client_address[0]} - {fmt % args}", flush=True)

    def do_GET(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(FORM)))
        self.end_headers()
        self.wfile.write(FORM)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            self.send_error(400, "empty upload")
            return

        content_type = self.headers.get("Content-Type", "")
        if "multipart/form-data" in content_type:
            body = self.rfile.read(length)
            saved = self.save_multipart(body, content_type)
            if saved is None:
                self.send_error(400, "could not find uploaded file")
                return
            response = f"saved {html.escape(saved.name)} ({saved.stat().st_size} bytes)\n".encode("utf-8")
            self.send_response(201)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
            return

        filename = self.headers.get("X-Filename")
        if not filename:
            filename = self.path.rsplit("/", 1)[-1]
        filename = safe_name(filename)

        target = self.upload_dir / filename
        stem = target.stem
        suffix = target.suffix
        index = 1
        while target.exists():
            target = self.upload_dir / f"{stem}-{index}{suffix}"
            index += 1

        remaining = length
        with target.open("wb") as out:
            while remaining > 0:
                chunk = self.rfile.read(min(1024 * 1024, remaining))
                if not chunk:
                    break
                out.write(chunk)
                remaining -= len(chunk)

        if remaining:
            self.send_error(400, "upload ended early")
            return

        body = f"saved {html.escape(target.name)} ({length} bytes)\n".encode("utf-8")
        self.send_response(201)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def save_multipart(self, body: bytes, content_type: str) -> Path | None:
        marker = "boundary="
        boundary_index = content_type.find(marker)
        if boundary_index < 0:
            return None
        boundary = content_type[boundary_index + len(marker):].strip().strip('"')
        if not boundary:
            return None
        delimiter = b"--" + boundary.encode("utf-8")
        for part in body.split(delimiter):
            part = part.strip()
            if not part or part == b"--":
                continue
            header_end = part.find(b"\r\n\r\n")
            separator_len = 4
            if header_end < 0:
                header_end = part.find(b"\n\n")
                separator_len = 2
            if header_end < 0:
                continue
            header_bytes = part[:header_end]
            data = part[header_end + separator_len:]
            if data.endswith(b"\r\n"):
                data = data[:-2]
            elif data.endswith(b"\n"):
                data = data[:-1]
            headers = header_bytes.decode("utf-8", errors="replace").replace("\r", "")
            disposition = ""
            for line in headers.split("\n"):
                if line.lower().startswith("content-disposition:"):
                    disposition = line
                    break
            if "filename=" not in disposition:
                continue
            filename = "upload.bin"
            for section in disposition.split(";"):
                section = section.strip()
                if section.startswith("filename="):
                    filename = section[len("filename="):].strip().strip('"')
                    break
            target = self.unique_target(safe_name(filename))
            target.write_bytes(data)
            return target
        return None

    def unique_target(self, filename: str) -> Path:
        target = self.upload_dir / filename
        stem = target.stem
        suffix = target.suffix
        index = 1
        while target.exists():
            target = self.upload_dir / f"{stem}-{index}{suffix}"
            index += 1
        return target


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect Jam2 log uploads over the LAN.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--dir", default="incoming_uploads")
    args = parser.parse_args()

    upload_dir = Path(args.dir).resolve()
    upload_dir.mkdir(parents=True, exist_ok=True)
    UploadHandler.upload_dir = upload_dir

    server = ThreadingHTTPServer((args.host, args.port), UploadHandler)
    print(f"Upload directory: {upload_dir}", flush=True)
    print(f"Listening on http://{args.host}:{args.port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
