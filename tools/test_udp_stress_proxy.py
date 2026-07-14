#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))

from udp_stress_proxy import DirectionImpairment, UdpStressProxy


class FakeSocket:
    def __init__(self):
        self.sent = []

    def sendto(self, data, destination):
        self.sent.append((bytes(data), destination))


class ProxySchedulingTests(unittest.TestCase):
    def make_proxy(self):
        return UdpStressProxy(("127.0.0.1", 9), seed=1)

    def test_clean_packet_is_forwarded_without_timed_heap(self):
        proxy = self.make_proxy()
        output = FakeSocket()
        try:
            proxy._schedule(
                b"packet",
                output,
                ("127.0.0.1", 10),
                DirectionImpairment(),
                "client_to_server")
            self.assertEqual(output.sent, [(b"packet", ("127.0.0.1", 10))])
            self.assertEqual(proxy.pending, [])
            self.assertEqual(proxy.stats["pending_packets_high_water"], 0)
        finally:
            proxy.close()

    def test_duplicate_without_delay_is_forwarded_adjacent_to_original(self):
        proxy = self.make_proxy()
        output = FakeSocket()
        try:
            proxy._schedule(
                b"packet",
                output,
                ("127.0.0.1", 10),
                DirectionImpairment(duplicate_percent=100.0),
                "client_to_server")
            self.assertEqual(len(output.sent), 2)
            self.assertEqual(output.sent[0], output.sent[1])
            self.assertEqual(proxy.pending, [])
            self.assertEqual(proxy.stats["client_to_server_duplicated"], 1)
        finally:
            proxy.close()

    def test_actual_delay_still_uses_timed_heap(self):
        proxy = self.make_proxy()
        output = FakeSocket()
        try:
            proxy._schedule(
                b"packet",
                output,
                ("127.0.0.1", 10),
                DirectionImpairment(fixed_delay_ms=5.0),
                "client_to_server")
            self.assertEqual(output.sent, [])
            self.assertEqual(len(proxy.pending), 1)
            self.assertEqual(proxy.stats["pending_packets_high_water"], 1)
        finally:
            proxy.close()

    def test_one_shot_burst_disarms_after_first_blackout(self):
        proxy = self.make_proxy()
        impairment = DirectionImpairment(
            burst_pause_ms=120.0,
            burst_every_ms=8000.0,
            burst_limit=1)
        try:
            proxy.next_client_to_server_burst = 10.0
            first_delay = proxy._burst_delay(10.0, impairment, "client_to_server")
            second_delay = proxy._burst_delay(20.0, impairment, "client_to_server")

            self.assertAlmostEqual(first_delay, 0.12)
            self.assertEqual(second_delay, 0.0)
            self.assertIsNone(proxy.next_client_to_server_burst)
            self.assertEqual(proxy.stats["client_to_server_blackout_events"], 1)
        finally:
            proxy.close()


if __name__ == "__main__":
    unittest.main()
