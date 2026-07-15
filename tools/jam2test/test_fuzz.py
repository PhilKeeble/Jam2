import unittest

from jam2test.fuzz import _mutate, seed_corpus
from jam2test.udp_protocol import PacketHeader, PacketType, encode_packet, verify_packet
import random


class FuzzGeneratorTests(unittest.TestCase):
    def test_every_target_has_bounded_reproducible_seed_corpus(self):
        for target in ("control", "udp-pcm16", "udp-pcm24", "asset", "wav"):
            corpus = seed_corpus(target)
            self.assertGreaterEqual(len(corpus), 3)
            self.assertTrue(all(name and 0 < len(data) <= 1024 * 1024 for name, data in corpus))

    def test_mutation_is_reproducible_and_bounded(self):
        source = b"0123456789" * 20
        left = _mutate(source, random.Random(99), 128)
        right = _mutate(source, random.Random(99), 128)
        self.assertEqual(left, right)
        self.assertLessEqual(len(left), 128)

    def test_udp_v2_golden_pcm_vectors(self):
        header = PacketHeader(packet_type=PacketType.AUDIO,
                              session_id=0x0102030405060708,
                              sequence=0x0A0B0C0D,
                              timing_value=0x1112131415161718)
        vectors = (
            (bytes.fromhex("0080ffff00000100ff7f"),
             "4a414d3202030a0008070605040302010d0c0b0a181716151413121145739dfac7e8d2bb0080ffff00000100ff7f"),
            (bytes.fromhex("00008000ffff00000000010000ff7f"),
             "4a414d3202030f0008070605040302010d0c0b0a1817161514131211f8f96efd3b229db200008000ffff00000000010000ff7f"),
        )
        for payload, expected in vectors:
            packet = encode_packet(header, payload, bytes(range(16)))
            self.assertEqual(packet.hex(), expected)
            self.assertEqual(verify_packet(packet, bytes(range(16)), header.session_id),
                             PacketHeader(**{**header.__dict__, "payload_length": len(payload),
                                            "auth_tag": verify_packet(packet, bytes(range(16))).auth_tag}))


if __name__ == "__main__":
    unittest.main()
