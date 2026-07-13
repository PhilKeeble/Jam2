#!/usr/bin/env python3

"""Reusable real-process UDP v1 adversarial fixtures."""

import struct
import threading
import time

from jam2_udp_protocol import MAGIC, VERSION, PacketType, parse_header, resign_packet


class PacketCapture:
    def __init__(self):
        self._lock = threading.Lock()
        self._packets = {}

    def observe(self, direction, packet):
        try:
            header = parse_header(packet)
        except ValueError:
            return
        if header.magic != MAGIC or header.version != VERSION:
            return
        with self._lock:
            self._packets.setdefault((direction, header.packet_type), bytes(packet))

    def packet(self, direction, packet_type):
        with self._lock:
            return self._packets.get((direction, packet_type))

    def wait_for(self, direction, packet_type, timeout_s=3.0):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            packet = self.packet(direction, packet_type)
            if packet is not None:
                return packet
            time.sleep(0.01)
        return None


class NearWrapSequenceTransformer:
    def __init__(self, key, start=0xFFFFFFF0):
        self.key = bytes(key)
        self.start = int(start) & 0xFFFFFFFF
        self.next_by_direction = {}
        self.transformed_by_direction = {}

    def __call__(self, direction, packet):
        try:
            header = parse_header(packet)
        except ValueError:
            return packet
        if header.magic != MAGIC or header.version != VERSION or header.packet_type != PacketType.AUDIO:
            return packet
        sequence = self.next_by_direction.setdefault(direction, self.start)
        self.next_by_direction[direction] = (sequence + 1) & 0xFFFFFFFF
        self.transformed_by_direction[direction] = self.transformed_by_direction.get(direction, 0) + 1
        return resign_packet(packet, self.key, sequence=sequence)

    def stats(self):
        return {
            "start_sequence": self.start,
            "transformed_by_direction": dict(self.transformed_by_direction),
            "wrapped_directions": sorted(
                direction
                for direction, count in self.transformed_by_direction.items()
                if count > ((1 << 32) - self.start)),
        }


class OneShotAudioHeaderTransformer:
    def __init__(self, key, mode, after_audio_packets=512):
        if mode not in ("forward-sequence-gap", "extreme-sample-time"):
            raise ValueError("unsupported one-shot UDP transform")
        self.key = bytes(key)
        self.mode = mode
        self.after_audio_packets = max(1, int(after_audio_packets))
        self.seen_by_direction = {}
        self.transformed_by_direction = {}

    def __call__(self, direction, packet):
        try:
            header = parse_header(packet)
        except ValueError:
            return packet
        if header.magic != MAGIC or header.version != VERSION or header.packet_type != PacketType.AUDIO:
            return packet
        seen = self.seen_by_direction.get(direction, 0) + 1
        self.seen_by_direction[direction] = seen
        if seen != self.after_audio_packets or self.transformed_by_direction.get(direction, 0) != 0:
            return packet
        self.transformed_by_direction[direction] = 1
        if self.mode == "forward-sequence-gap":
            return resign_packet(packet, self.key, sequence=(header.sequence + 100000) & 0xFFFFFFFF)
        return resign_packet(packet, self.key, sample_time=(1 << 64) - 128)

    def stats(self):
        return {
            "mode": self.mode,
            "after_audio_packets": self.after_audio_packets,
            "transformed_by_direction": dict(self.transformed_by_direction),
        }


def malformed_variants(packet):
    """Create inputs that exercise each cheap fixed-header rejection branch."""
    packet = bytes(packet)
    if len(packet) < 48:
        raise ValueError("a complete UDP v1 packet is required")

    variants = [("short", packet[:12])]

    def changed(name, offset, value, fmt="B"):
        data = bytearray(packet)
        struct.pack_into("<" + fmt, data, offset, value)
        variants.append((name, bytes(data)))

    changed("magic", 0, 0, "I")
    changed("version", 4, (packet[4] + 1) & 0xFF)
    changed("type", 5, 0xFF)
    changed("flags", 6, 1, "H")
    changed("reserved", 46, 1, "H")
    changed("session", 8, 0, "Q")
    changed("payload_size", 36, 0xFFFF, "H")
    auth = bytearray(packet)
    auth[38] ^= 0x01
    variants.append(("authentication", bytes(auth)))
    return variants


def inject_malformed_corpus(proxy, capture, timeout_s=3.0):
    outcomes = []
    directions = (
        ("client_to_server", proxy.inject_client_to_server),
        ("server_to_client", proxy.inject_server_to_client),
    )
    for direction, inject in directions:
        packet = capture.wait_for(direction, PacketType.AUDIO, timeout_s)
        if packet is None:
            outcomes.append({"direction": direction, "error": "audio_capture_timeout"})
            continue
        for name, variant in malformed_variants(packet):
            outcomes.append({
                "direction": direction,
                "case": name,
                "bytes": len(variant),
                "injected": bool(inject(variant)),
            })
    return outcomes


def inject_delayed_replay(proxy, capture, timeout_s=3.0, delay_s=0.25):
    outcomes = []
    for direction, inject in (
            ("client_to_server", proxy.inject_client_to_server),
            ("server_to_client", proxy.inject_server_to_client)):
        packet = capture.wait_for(direction, PacketType.AUDIO, timeout_s)
        if packet is None:
            outcomes.append({"direction": direction, "error": "audio_capture_timeout"})
            continue
        time.sleep(delay_s)
        outcomes.append({"direction": direction, "injected": bool(inject(packet)), "bytes": len(packet)})
    return outcomes


def inject_short_packet_flood(proxy, capture, packets_per_direction=2048, timeout_s=3.0):
    outcomes = []
    for direction, inject in (
            ("client_to_server", proxy.inject_client_to_server),
            ("server_to_client", proxy.inject_server_to_client)):
        if capture.wait_for(direction, PacketType.AUDIO, timeout_s) is None:
            outcomes.append({"direction": direction, "error": "audio_capture_timeout"})
            continue
        sent = 0
        for index in range(int(packets_per_direction)):
            sent += 1 if inject(struct.pack("<Q", index)) else 0
        outcomes.append({
            "direction": direction,
            "requested": int(packets_per_direction),
            "injected": sent,
            "datagram_bytes": 8,
        })
    return outcomes
