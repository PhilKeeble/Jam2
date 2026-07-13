#!/usr/bin/env python3

import heapq
import random
import selectors
import socket
import time
from dataclasses import dataclass, field


@dataclass(frozen=True)
class DirectionImpairment:
    loss_percent: float = 0.0
    duplicate_percent: float = 0.0
    corrupt_percent: float = 0.0
    jitter_ms: float = 0.0
    fixed_delay_ms: float = 0.0
    reorder_percent: float = 0.0
    burst_pause_ms: float = 0.0
    burst_every_ms: float = 0.0
    preserve_order: bool = True


@dataclass(frozen=True)
class ProxyImpairment:
    client_to_server: DirectionImpairment = field(default_factory=DirectionImpairment)
    server_to_client: DirectionImpairment = field(default_factory=DirectionImpairment)

    @staticmethod
    def both(direction):
        return ProxyImpairment(client_to_server=direction, server_to_client=direction)


class UdpStressProxy:
    def __init__(
            self,
            server_endpoint,
            bind_host="127.0.0.1",
            bind_port=0,
            impairment=None,
            seed=1,
            packet_observer=None,
            packet_transformer=None,
            max_pending_packets=65536):
        self.server_endpoint = server_endpoint
        self.impairment = impairment or ProxyImpairment()
        self.random = random.Random(seed)
        self.packet_observer = packet_observer
        self.packet_transformer = packet_transformer
        self.max_pending_packets = max(1, int(max_pending_packets))
        self.selector = selectors.DefaultSelector()
        self.client_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client_sock.bind((bind_host, bind_port))
        self.server_sock.bind((bind_host, 0))
        self.client_sock.setblocking(False)
        self.server_sock.setblocking(False)
        self.selector.register(self.client_sock, selectors.EVENT_READ, "client")
        self.selector.register(self.server_sock, selectors.EVENT_READ, "server")
        self.client_endpoint = None
        self.pending = []
        self.next_order = 0
        now = time.monotonic()
        self.next_client_to_server_burst = self._first_burst_at(now, self.impairment.client_to_server)
        self.next_server_to_client_burst = self._first_burst_at(now, self.impairment.server_to_client)
        self.client_to_server_blackout_until = 0.0
        self.server_to_client_blackout_until = 0.0
        self.client_to_server_release_at = now
        self.server_to_client_release_at = now
        self.stats = {
            "client_to_server_packets": 0,
            "server_to_client_packets": 0,
            "client_to_server_dropped": 0,
            "server_to_client_dropped": 0,
            "client_to_server_capacity_dropped": 0,
            "server_to_client_capacity_dropped": 0,
            "client_to_server_duplicated": 0,
            "server_to_client_duplicated": 0,
            "client_to_server_corrupted": 0,
            "server_to_client_corrupted": 0,
            "client_to_server_delayed": 0,
            "server_to_client_delayed": 0,
            "client_to_server_reordered": 0,
            "server_to_client_reordered": 0,
            "client_to_server_blackout_events": 0,
            "server_to_client_blackout_events": 0,
            "client_to_server_recv_errors": 0,
            "server_to_client_recv_errors": 0,
            "client_to_server_send_errors": 0,
            "server_to_client_send_errors": 0,
            "client_to_server_injected": 0,
            "server_to_client_injected": 0,
            "packet_observer_errors": 0,
            "packet_transform_errors": 0,
            "pending_packets_high_water": 0,
            "pending_packet_limit": self.max_pending_packets,
            "client_endpoint": "",
            "server_endpoint": f"{server_endpoint[0]}:{server_endpoint[1]}",
        }

    @property
    def public_endpoint(self):
        host, port = self.client_sock.getsockname()
        return host, port

    def close(self):
        try:
            self.selector.close()
        finally:
            self.client_sock.close()
            self.server_sock.close()

    def inject_client_to_server(self, data):
        """Send a crafted datagram from the proxy address observed by the server."""
        try:
            self.server_sock.sendto(bytes(data), self.server_endpoint)
            self.stats["client_to_server_injected"] += 1
            return True
        except OSError:
            self.stats["client_to_server_send_errors"] += 1
            return False

    def inject_server_to_client(self, data):
        """Send a crafted datagram from the proxy address observed by the client."""
        if self.client_endpoint is None:
            return False
        try:
            self.client_sock.sendto(bytes(data), self.client_endpoint)
            self.stats["server_to_client_injected"] += 1
            return True
        except OSError:
            self.stats["server_to_client_send_errors"] += 1
            return False

    def run_until(self, stop_event):
        try:
            while not stop_event.is_set():
                self._send_due()
                timeout = self._select_timeout()
                for key, _ in self.selector.select(timeout):
                    if key.data == "client":
                        self._recv_client()
                    else:
                        self._recv_server()
        finally:
            self._flush_pending()

    def _recv_client(self):
        while True:
            try:
                data, addr = self.client_sock.recvfrom(65535)
            except BlockingIOError:
                return
            except ConnectionResetError:
                self.stats["client_to_server_recv_errors"] += 1
                return
            except OSError:
                self.stats["client_to_server_recv_errors"] += 1
                return
            self.client_endpoint = addr
            self.stats["client_endpoint"] = f"{addr[0]}:{addr[1]}"
            self._schedule(
                data,
                self.server_sock,
                self.server_endpoint,
                self.impairment.client_to_server,
                "client_to_server",
            )

    def _recv_server(self):
        while True:
            try:
                data, _ = self.server_sock.recvfrom(65535)
            except BlockingIOError:
                return
            except ConnectionResetError:
                self.stats["server_to_client_recv_errors"] += 1
                return
            except OSError:
                self.stats["server_to_client_recv_errors"] += 1
                return
            if self.client_endpoint is None:
                self.stats["server_to_client_dropped"] += 1
                continue
            self._schedule(
                data,
                self.client_sock,
                self.client_endpoint,
                self.impairment.server_to_client,
                "server_to_client",
            )

    def _schedule(self, data, out_sock, destination, impairment, prefix):
        self.stats[f"{prefix}_packets"] += 1
        if self.packet_observer is not None:
            try:
                self.packet_observer(prefix, bytes(data))
            except Exception:
                self.stats["packet_observer_errors"] += 1
        if self.packet_transformer is not None:
            try:
                data = self.packet_transformer(prefix, bytes(data))
            except Exception:
                self.stats["packet_transform_errors"] += 1
            if data is None:
                self.stats[f"{prefix}_dropped"] += 1
                return
        data = bytes(data)
        if impairment.loss_percent > 0.0 and self.random.random() < impairment.loss_percent / 100.0:
            self.stats[f"{prefix}_dropped"] += 1
            return

        if data and impairment.corrupt_percent > 0.0 and self.random.random() < impairment.corrupt_percent / 100.0:
            corrupted = bytearray(data)
            byte_index = self.random.randrange(len(corrupted))
            corrupted[byte_index] ^= 1 << self.random.randrange(8)
            data = bytes(corrupted)
            self.stats[f"{prefix}_corrupted"] += 1

        if not self._requires_timed_queue(impairment):
            if not self._send_immediate(out_sock, destination, data, prefix):
                return
            if impairment.duplicate_percent > 0.0 and self.random.random() < impairment.duplicate_percent / 100.0:
                if self._send_immediate(out_sock, destination, data, prefix):
                    self.stats[f"{prefix}_duplicated"] += 1
            return

        now = time.monotonic()
        delay_s = max(0.0, impairment.fixed_delay_ms / 1000.0)
        if impairment.jitter_ms > 0.0:
            delay_s += self.random.uniform(0.0, impairment.jitter_ms) / 1000.0
        delay_s += self._burst_delay(now, impairment, prefix)

        if impairment.reorder_percent > 0.0 and self.random.random() < impairment.reorder_percent / 100.0:
            delay_s += max(0.002, impairment.jitter_ms / 1000.0)
            self.stats[f"{prefix}_reordered"] += 1

        release_at = now + delay_s
        if impairment.preserve_order and impairment.reorder_percent <= 0.0:
            attr = f"{prefix}_release_at"
            release_at = max(release_at, getattr(self, attr, now) + 0.000001)
            setattr(self, attr, release_at)

        if delay_s > 0.0:
            self.stats[f"{prefix}_delayed"] += 1
        if not self._queue_packet(release_at, prefix, out_sock, destination, data):
            return
        if impairment.duplicate_percent > 0.0 and self.random.random() < impairment.duplicate_percent / 100.0:
            duplicate_at = release_at + 0.000001
            if self._queue_packet(duplicate_at, prefix, out_sock, destination, data):
                self.stats[f"{prefix}_duplicated"] += 1

    @staticmethod
    def _requires_timed_queue(impairment):
        return (
            impairment.fixed_delay_ms > 0.0
            or impairment.jitter_ms > 0.0
            or impairment.reorder_percent > 0.0
            or (impairment.burst_pause_ms > 0.0 and impairment.burst_every_ms > 0.0)
        )

    def _send_immediate(self, out_sock, destination, data, prefix):
        try:
            out_sock.sendto(data, destination)
            return True
        except OSError:
            self.stats[f"{prefix}_send_errors"] += 1
            return False

    def _queue_packet(self, release_at, prefix, out_sock, destination, data):
        if len(self.pending) >= self.max_pending_packets:
            self.stats[f"{prefix}_capacity_dropped"] += 1
            return False
        self.next_order += 1
        heapq.heappush(self.pending, (release_at, self.next_order, prefix, out_sock, destination, data))
        if len(self.pending) > self.stats["pending_packets_high_water"]:
            self.stats["pending_packets_high_water"] = len(self.pending)
        return True

    def _send_due(self):
        now = time.monotonic()
        while self.pending and self.pending[0][0] <= now:
            _, _, prefix, out_sock, destination, data = heapq.heappop(self.pending)
            try:
                out_sock.sendto(data, destination)
            except OSError:
                self.stats[f"{prefix}_send_errors"] += 1

    def _flush_pending(self):
        while self.pending:
            _, _, prefix, out_sock, destination, data = heapq.heappop(self.pending)
            try:
                out_sock.sendto(data, destination)
            except OSError:
                self.stats[f"{prefix}_send_errors"] += 1

    def _select_timeout(self):
        if not self.pending:
            return 0.05
        return max(0.0, min(0.05, self.pending[0][0] - time.monotonic()))

    def _first_burst_at(self, now, impairment):
        if impairment.burst_pause_ms <= 0.0 or impairment.burst_every_ms <= 0.0:
            return None
        return now + impairment.burst_every_ms / 1000.0

    def _burst_delay(self, now, impairment, prefix):
        if impairment.burst_pause_ms <= 0.0 or impairment.burst_every_ms <= 0.0:
            return 0.0
        attr = "next_client_to_server_burst" if prefix == "client_to_server" else "next_server_to_client_burst"
        blackout_attr = f"{prefix}_blackout_until"
        next_burst = getattr(self, attr)
        if next_burst is None:
            return 0.0
        pause_s = impairment.burst_pause_ms / 1000.0
        every_s = impairment.burst_every_ms / 1000.0
        blackout_until = getattr(self, blackout_attr)
        if now < blackout_until:
            return blackout_until - now
        if now < next_burst:
            return 0.0
        blackout_until = next_burst + pause_s
        setattr(self, blackout_attr, blackout_until)
        self.stats[f"{prefix}_blackout_events"] += 1
        while next_burst <= now:
            next_burst += every_s
        setattr(self, attr, next_burst)
        return max(0.0, blackout_until - now)
