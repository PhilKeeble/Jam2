#!/usr/bin/env python3

import json
import hashlib
import os
import queue
import re
import socket
import socketserver
import tempfile
import threading
import time
import uuid


PROTOCOL_VERSION = 1
MAX_CONTROL_LINE_BYTES = 64 * 1024
MAX_UPLOAD_BYTES = 1024 * 1024 * 1024
TRANSFER_CHUNK_BYTES = 64 * 1024
MAX_CONTROL_MESSAGES = 256
MAX_CLIENT_INBOX = 128
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")


def now_ms():
    return int(time.time() * 1000)


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(TRANSFER_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


def new_suite_id():
    return uuid.uuid4().hex[:12]


def run_identity(payload):
    return {
        "suite_id": payload.get("suite_id", ""),
        "case_id": payload.get("case_id", ""),
        "run_index": int(payload.get("run_index", 0) or 0),
        "attempt_id": payload.get("attempt_id", ""),
    }


def same_run(a, b):
    return run_identity(a) == run_identity(b)


def control_endpoint_from_server_url(server_url, default_port=49000):
    from urllib.parse import urlparse

    parsed = urlparse(server_url)
    if parsed.hostname:
        host = parsed.hostname
    else:
        host = server_url.strip().rsplit(":", 1)[0]
    if not host:
        host = "127.0.0.1"
    return host, default_port


def parse_host_port(value):
    host, port_text = value.rsplit(":", 1)
    return host, int(port_text)


class JsonLinePeer:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.buffer = b""

    def send(self, message):
        data = json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(data) > MAX_CONTROL_LINE_BYTES:
            raise ValueError("benchmark control message exceeds its bound")
        with self.lock:
            self.sock.sendall(data)

    def send_file(self, message, path):
        data = json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(data) > MAX_CONTROL_LINE_BYTES:
            raise ValueError("benchmark control upload envelope exceeds its bound")
        with self.lock:
            self.sock.sendall(data)
            with open(path, "rb") as handle:
                for chunk in iter(lambda: handle.read(TRANSFER_CHUNK_BYTES), b""):
                    self.sock.sendall(chunk)

    def read(self):
        line = self._readline()
        if not line:
            return None
        return json.loads(line.decode("utf-8"))

    def read_to_file(self, size, path):
        remaining = size
        digest = hashlib.sha256()
        with open(path, "wb") as handle:
            if self.buffer:
                chunk = self.buffer[:remaining]
                self.buffer = self.buffer[remaining:]
                handle.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
            while remaining > 0:
                chunk = self.sock.recv(min(TRANSFER_CHUNK_BYTES, remaining))
                if not chunk:
                    raise OSError("socket closed during payload read")
                handle.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
        return digest.hexdigest()

    def _readline(self):
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                line = self.buffer[:newline].strip()
                self.buffer = self.buffer[newline + 1:]
                return line
            chunk = self.sock.recv(4096)
            if not chunk:
                if self.buffer:
                    line = self.buffer.strip()
                    self.buffer = b""
                    return line
                return None
            self.buffer += chunk
            if len(self.buffer) > MAX_CONTROL_LINE_BYTES:
                raise ValueError("benchmark control line exceeds its bound")

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class BenchmarkControlState:
    def __init__(self, suite_id, invocation_id, log, upload_callback=None, upload_temp_dir=None):
        self.suite_id = suite_id
        self.invocation_id = invocation_id
        self.log = log
        self.upload_callback = upload_callback
        self.upload_temp_dir = upload_temp_dir
        self.condition = threading.Condition()
        self.peer = None
        self.peer_id = ""
        self.connected_once = False
        self.disconnect_count = 0
        self.reconnect_count = 0
        self.active_case = None
        self.active_phase = "idle"
        self.messages = []
        self.message_drops = 0
        self.done_acked = False

    def attach_peer(self, peer, peer_id):
        with self.condition:
            old_peer = self.peer
            self.peer = peer
            self.peer_id = peer_id
            if self.connected_once:
                self.reconnect_count += 1
            self.connected_once = True
            self.condition.notify_all()
        if old_peer is not None and old_peer is not peer:
            old_peer.close()
        if self.active_case:
            self.send({
                "type": "case.offer",
                "suite_id": self.suite_id,
                "phase": self.active_phase,
                "case": self.active_case,
            })
        elif self.active_phase == "all_done":
            self.send({"type": "all_done", "suite_id": self.suite_id})

    def detach_peer(self, peer):
        with self.condition:
            if self.peer is not peer:
                return
            self.peer = None
            self.peer_id = ""
            self.disconnect_count += 1
            self.condition.notify_all()

    def send(self, message):
        with self.condition:
            peer = self.peer
        if peer is None:
            return False
        try:
            peer.send(message)
            return True
        except (OSError, ValueError) as error:
            self.log(f"[control] send failed: {error}")
            self.detach_peer(peer)
            peer.close()
            return False

    def publish_case(self, case_payload):
        with self.condition:
            self.active_case = dict(case_payload)
            self.active_phase = "offered"
            self.messages.clear()
            self.condition.notify_all()
        self.send({
            "type": "case.offer",
            "suite_id": self.suite_id,
            "phase": "offered",
            "case": self.active_case,
        })

    def clear_case(self):
        with self.condition:
            self.active_case = None
            self.active_phase = "idle"
            self.messages.clear()
            self.condition.notify_all()

    def mark_all_done(self):
        with self.condition:
            self.active_case = None
            self.active_phase = "all_done"
            self.condition.notify_all()
        self.send({"type": "all_done", "suite_id": self.suite_id})

    def handle_message(self, message):
        msg_type = message.get("type", "")
        with self.condition:
            if msg_type == "case.accept":
                self.active_phase = "accepted"
            elif msg_type == "case.started":
                self.active_phase = "client_started"
            elif msg_type == "case.finished":
                self.active_phase = "client_finished"
            elif msg_type == "case.uploaded":
                self.active_phase = "uploaded"
            elif msg_type == "done.ack":
                self.done_acked = True
            if len(self.messages) >= MAX_CONTROL_MESSAGES:
                self.messages.pop(0)
                self.message_drops += 1
            self.messages.append(dict(message))
            self.condition.notify_all()

    def wait_for(self, predicate, timeout_s=0.0):
        deadline = time.monotonic() + timeout_s if timeout_s and timeout_s > 0 else None
        with self.condition:
            while True:
                value = predicate()
                if value:
                    return value
                if deadline is not None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return None
                    self.condition.wait(min(remaining, 0.25))
                else:
                    self.condition.wait(0.25)

    def wait_for_peer(self, timeout_s=0.0):
        return self.wait_for(lambda: self.peer is not None, timeout_s=timeout_s)

    def has_message(self, msg_type, identity=None):
        with self.condition:
            for message in self.messages:
                if message.get("type") != msg_type:
                    continue
                if identity is None or same_run(message, identity):
                    return message
        return None

    def snapshot(self):
        with self.condition:
            return {
                "connected_once": self.connected_once,
                "peer_connected": self.peer is not None,
                "peer_id": self.peer_id,
                "disconnect_count": self.disconnect_count,
                "reconnect_count": self.reconnect_count,
                "message_drops": self.message_drops,
                "active_phase": self.active_phase,
            }


class BenchmarkControlHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(300.0)
        peer = JsonLinePeer(self.request)
        state = self.server.control_state
        peer_id = ""
        try:
            hello = peer.read()
            if (not isinstance(hello, dict) or hello.get("type") != "hello" or
                    hello.get("version") != PROTOCOL_VERSION):
                return
            peer_id = hello.get("client_id", "")
            if not isinstance(peer_id, str) or not SAFE_ID.fullmatch(peer_id):
                raise ValueError("benchmark agent identity is invalid")
            peer.send({
                "type": "hello.ok",
                "version": PROTOCOL_VERSION,
                "suite_id": state.suite_id,
                "invocation_id": state.invocation_id,
                "server_time_ms": now_ms(),
            })
            state.log(f"[control] client authenticated peer={peer_id or '-'}")
            state.attach_peer(peer, peer_id)
            while True:
                message = peer.read()
                if message is None:
                    return
                if not isinstance(message, dict):
                    raise ValueError("benchmark control message must be an object")
                if message.get("type") == "heartbeat":
                    peer.send({
                        "type": "heartbeat.ok",
                        "suite_id": state.suite_id,
                        "server_time_ms": now_ms(),
                    })
                    continue
                if message.get("type") == "artifact.upload":
                    size = int(message.get("size", 0) or 0)
                    if size <= 0 or size > MAX_UPLOAD_BYTES:
                        peer.send({
                            "type": "artifact.upload.error",
                            **run_identity(message),
                            "reason": "artifact upload size is outside the 1 GiB bound",
                        })
                        raise ValueError("artifact upload size is outside the 1 GiB bound")
                    try:
                        directory = str(state.upload_temp_dir) if state.upload_temp_dir else None
                        fd, upload_path = tempfile.mkstemp(prefix="jam2-upload-", suffix=".zip", dir=directory)
                        os.close(fd)
                        digest = peer.read_to_file(size, upload_path)
                        if digest != message.get("sha256"):
                            raise ValueError("artifact upload digest does not match its envelope")
                        if state.upload_callback is None:
                            raise ValueError("artifact upload callback is not configured")
                        state.upload_callback(message, upload_path)
                        state.handle_message({"type": "case.uploaded", **run_identity(message), "uploaded": True})
                        peer.send({
                            "type": "artifact.upload.ok",
                            **run_identity(message),
                            "size": size,
                        })
                    except Exception as error:
                        peer.send({
                            "type": "artifact.upload.error",
                            **run_identity(message),
                            "reason": str(error),
                        })
                    finally:
                        if 'upload_path' in locals():
                            try: os.unlink(upload_path)
                            except OSError: pass
                            del upload_path
                    continue
                state.handle_message(message)
        except (OSError, TypeError, ValueError, json.JSONDecodeError, UnicodeDecodeError) as error:
            state.log(f"[control] peer error: {error}")
        finally:
            state.detach_peer(peer)
            peer.close()
            if peer_id:
                state.log(f"[control] client disconnected peer={peer_id}")


class BenchmarkControlServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 8
    max_control_connections = 4

    def __init__(self, server_address, control_state):
        self.control_state = control_state
        self._connection_slots = threading.BoundedSemaphore(self.max_control_connections)
        super().__init__(server_address, BenchmarkControlHandler)

    def process_request(self, request, client_address):
        if not self._connection_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._connection_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._connection_slots.release()


def start_control_server(bind_control, suite_id, invocation_id, log, upload_callback=None, upload_temp_dir=None):
    host, port = parse_host_port(bind_control)
    state = BenchmarkControlState(suite_id, invocation_id, log, upload_callback=upload_callback, upload_temp_dir=upload_temp_dir)
    server = BenchmarkControlServer((host, port), state)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, state


class BenchmarkControlClient:
    def __init__(self, host, port, client_id, log, reconnect_delay_s=2.0):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.log = log
        self.reconnect_delay_s = reconnect_delay_s
        self.peer = None
        self.suite_id = ""
        self.invocation_id = ""
        self.inbox = queue.Queue(maxsize=MAX_CLIENT_INBOX)
        self.inbox_drops = 0
        self.reader_thread = None
        self.stop_event = threading.Event()
        self.connected_event = threading.Event()

    def start(self):
        self.stop_event.clear()
        self.reader_thread = threading.Thread(target=self._connect_loop, daemon=True)
        self.reader_thread.start()

    def close(self):
        self.stop_event.set()
        if self.peer is not None:
            self.peer.close()
        if self.reader_thread is not None:
            self.reader_thread.join(timeout=2.0)

    def send(self, message):
        peer = self.peer
        if peer is None:
            return False
        try:
            peer.send(message)
            return True
        except (OSError, ValueError) as error:
            self.log(f"[client] control send failed: {error}")
            peer.close()
            self.peer = None
            self.connected_event.clear()
            return False

    def _enqueue(self, message):
        try:
            self.inbox.put_nowait(message)
        except queue.Full:
            self.inbox_drops += 1
            self.log("[client] bounded control inbox dropped a message")

    def send_file(self, message, path):
        peer = self.peer
        if peer is None:
            return False
        try:
            peer.send_file(message, path)
            return True
        except (OSError, ValueError) as error:
            self.log(f"[client] control payload send failed: {error}")
            peer.close()
            self.peer = None
            self.connected_event.clear()
            return False

    def upload_artifact(self, identity, zip_path, timeout_s=0.0):
        size = zip_path.stat().st_size
        if size <= 0 or size > MAX_UPLOAD_BYTES:
            self.log("[agent] artifact archive is outside the 1 GiB upload bound")
            return False
        deadline = time.monotonic() + timeout_s if timeout_s and timeout_s > 0 else None
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                self.log("[client] artifact upload timed out waiting for TCP control reconnect")
                return False
            if not self.wait_connected(timeout_s=1.0):
                self.log("[client] waiting for TCP control reconnect before artifact upload")
                continue
            message = {
                "type": "artifact.upload",
                **run_identity(identity),
                "machine_id": identity.get("machine_id", ""),
                "size": size,
                "sha256": _sha256_file(zip_path),
                "client_time_ms": now_ms(),
            }
            if not self.send_file(message, zip_path):
                self.log("[client] artifact upload send failed; waiting for reconnect")
                continue
            result = self._wait_for_upload_ack(identity, deadline)
            if result is None:
                continue
            return result

    def _wait_for_upload_ack(self, identity, deadline):
        deferred = []
        while deadline is None or time.monotonic() < deadline:
            if not self.wait_connected(timeout_s=0.0):
                for message in deferred:
                    self._enqueue(message)
                self.log("[client] TCP control disconnected before upload ack; retrying after reconnect")
                return None
            wait_s = 1.0 if deadline is None else min(1.0, max(0.1, deadline - time.monotonic()))
            response = self.wait_message(timeout_s=wait_s)
            if response is None:
                continue
            response_type = response.get("type", "")
            if response_type not in ("artifact.upload.ok", "artifact.upload.error"):
                if len(deferred) < MAX_CLIENT_INBOX:
                    deferred.append(response)
                else:
                    self.inbox_drops += 1
                continue
            if same_run(response, identity):
                if response_type == "artifact.upload.ok":
                    for message in deferred:
                        self._enqueue(message)
                    return True
                self.log(f"[client] artifact upload rejected: {response.get('reason', 'unknown error')}")
                for message in deferred:
                    self._enqueue(message)
                return False
            if len(deferred) < MAX_CLIENT_INBOX:
                deferred.append(response)
            else:
                self.inbox_drops += 1
        for message in deferred:
            self._enqueue(message)
        self.log("[client] artifact upload timed out waiting for control ack")
        return False

    def wait_message(self, timeout_s=0.5):
        try:
            return self.inbox.get(timeout=timeout_s)
        except queue.Empty:
            return None

    def wait_connected(self, timeout_s=0.0):
        return self.connected_event.wait(timeout_s) if timeout_s and timeout_s > 0 else self.connected_event.is_set()

    def _connect_loop(self):
        while not self.stop_event.is_set():
            try:
                sock = socket.create_connection((self.host, self.port), timeout=5.0)
                sock.settimeout(None)
                peer = JsonLinePeer(sock)
                peer.send({
                    "type": "hello",
                    "version": PROTOCOL_VERSION,
                    "client_id": self.client_id,
                })
                response = peer.read()
                if not isinstance(response, dict) or response.get("type") != "hello.ok":
                    peer.close()
                    time.sleep(self.reconnect_delay_s)
                    continue
                self.peer = peer
                self.suite_id = response.get("suite_id", "")
                self.invocation_id = response.get("invocation_id", "")
                self.connected_event.set()
                self._enqueue(response)
                self.log(f"[client] TCP control connected suite={self.suite_id or '-'}")
                while not self.stop_event.is_set():
                    message = peer.read()
                    if message is None:
                        break
                    if not isinstance(message, dict):
                        raise ValueError("benchmark control message must be an object")
                    self._enqueue(message)
            except OSError as error:
                if not self.stop_event.is_set():
                    self.log(f"[client] waiting for TCP control: {error}")
            except (TypeError, ValueError, json.JSONDecodeError, UnicodeDecodeError) as error:
                if not self.stop_event.is_set():
                    self.log(f"[client] TCP control parse error: {error}")
            finally:
                if self.peer is not None:
                    self.peer.close()
                self.peer = None
                self.connected_event.clear()
            if not self.stop_event.is_set():
                self._enqueue({"type": "control.disconnected"})
                time.sleep(self.reconnect_delay_s)
