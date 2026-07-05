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
    def __init__(self, server_endpoint, bind_host="127.0.0.1", bind_port=0, impairment=None, seed=1):
        self.server_endpoint = server_endpoint
        self.impairment = impairment or ProxyImpairment()
        self.random = random.Random(seed)
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
        if impairment.loss_percent > 0.0 and self.random.random() < impairment.loss_percent / 100.0:
            self.stats[f"{prefix}_dropped"] += 1
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
        self.next_order += 1
        heapq.heappush(self.pending, (release_at, self.next_order, prefix, out_sock, destination, data))

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
