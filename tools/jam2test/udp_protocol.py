"""Fixed-shape Jam2 UDP helpers for retained validation injections."""

"""Independent Jam2 UDP v2 fixtures for black-box validation tools.

This module intentionally does not share code with the C++ codec.  It is used
only by Python validators that observe or inject packets through real Jam2 UDP
sockets.  Keeping the implementation small and literal makes it useful for
checking the fixed wire layout while the production codec is refactored.
"""

from dataclasses import dataclass, replace
import struct
from urllib.parse import parse_qs, urlparse


MAGIC = 0x324D414A
VERSION = 2
HEADER_SIZE = 36
AUTH_TAG_OFFSET = 28
AUTH_TAG_SIZE = 8
_HEADER = struct.Struct("<IBBHQIQQ")
_MASK64 = (1 << 64) - 1


class PacketType:
    HELLO = 1
    HELLO_ACK = 2
    AUDIO = 3
    PING = 4
    PONG = 5
    METRONOME_STATE = 6
    BYE = 7
    TRANSPORT_STATE = 8


@dataclass(frozen=True)
class PacketHeader:
    packet_type: int
    session_id: int = 0
    sequence: int = 0
    timing_value: int = 0
    payload_length: int = 0
    auth_tag: int = 0
    magic: int = MAGIC
    version: int = VERSION


@dataclass(frozen=True)
class JamUrl:
    host: str
    port: int
    session_id: int
    key: bytes


def _rotl64(value, bits):
    return ((value << bits) | (value >> (64 - bits))) & _MASK64


def siphash24(data, key):
    """Return the SipHash-2-4 value used by the production UDP v2 codec."""
    data = bytes(data)
    key = bytes(key)
    if len(key) != 16:
        raise ValueError("SipHash key must be exactly 16 bytes")

    k0, k1 = struct.unpack("<QQ", key)
    v0 = 0x736F6D6570736575 ^ k0
    v1 = 0x646F72616E646F6D ^ k1
    v2 = 0x6C7967656E657261 ^ k0
    v3 = 0x7465646279746573 ^ k1

    def sip_round():
        nonlocal v0, v1, v2, v3
        v0 = (v0 + v1) & _MASK64
        v1 = _rotl64(v1, 13) ^ v0
        v0 = _rotl64(v0, 32)
        v2 = (v2 + v3) & _MASK64
        v3 = _rotl64(v3, 16) ^ v2
        v0 = (v0 + v3) & _MASK64
        v3 = _rotl64(v3, 21) ^ v0
        v2 = (v2 + v1) & _MASK64
        v1 = _rotl64(v1, 17) ^ v2
        v2 = _rotl64(v2, 32)

    whole_bytes = len(data) - (len(data) % 8)
    for offset in range(0, whole_bytes, 8):
        word = struct.unpack_from("<Q", data, offset)[0]
        v3 ^= word
        sip_round()
        sip_round()
        v0 ^= word

    final = (len(data) & 0xFF) << 56
    for index, value in enumerate(data[whole_bytes:]):
        final |= value << (index * 8)
    v3 ^= final
    sip_round()
    sip_round()
    v0 ^= final
    v2 ^= 0xFF
    sip_round()
    sip_round()
    sip_round()
    sip_round()
    return (v0 ^ v1 ^ v2 ^ v3) & _MASK64


def parse_header(packet):
    packet = bytes(packet)
    if len(packet) < HEADER_SIZE:
        raise ValueError("packet is shorter than the fixed UDP v2 header")
    fields = _HEADER.unpack_from(packet)
    return PacketHeader(
        magic=fields[0],
        version=fields[1],
        packet_type=fields[2],
        payload_length=fields[3],
        session_id=fields[4],
        sequence=fields[5],
        timing_value=fields[6],
        auth_tag=fields[7],
    )


def encode_packet(header, payload, key):
    payload = bytes(payload)
    key = bytes(key)
    if len(payload) > 0xFFFF:
        raise ValueError("payload is too large for UDP v2")
    unsigned_header = replace(header, payload_length=len(payload), auth_tag=0)
    packet = bytearray(_HEADER.pack(
        unsigned_header.magic,
        unsigned_header.version,
        unsigned_header.packet_type,
        unsigned_header.payload_length,
        unsigned_header.session_id,
        unsigned_header.sequence & 0xFFFFFFFF,
        unsigned_header.timing_value,
        0,
    ))
    packet.extend(payload)
    struct.pack_into("<Q", packet, AUTH_TAG_OFFSET, siphash24(packet, key))
    return bytes(packet)


def verify_packet(packet, key, expected_session_id=None):
    """Validate fixed framing and authentication, returning the parsed header."""
    packet = bytes(packet)
    header = parse_header(packet)
    if header.magic != MAGIC:
        raise ValueError("wrong packet magic")
    if header.version != VERSION:
        raise ValueError("wrong packet version")
    if expected_session_id is not None and header.session_id != expected_session_id:
        raise ValueError("wrong session id")
    if len(packet) != HEADER_SIZE + header.payload_length:
        raise ValueError("packet payload length mismatch")
    auth_bytes = bytearray(packet)
    auth_bytes[AUTH_TAG_OFFSET:AUTH_TAG_OFFSET + AUTH_TAG_SIZE] = b"\0" * AUTH_TAG_SIZE
    if siphash24(auth_bytes, key) != header.auth_tag:
        raise ValueError("packet authentication failed")
    return header


def resign_packet(packet, key, **changes):
    """Apply header changes and recompute the UDP v2 tag without changing payload."""
    packet = bytes(packet)
    header = parse_header(packet)
    payload = packet[HEADER_SIZE:]
    return encode_packet(replace(header, **changes), payload, key)


def parse_jam_url(url):
    parsed = urlparse(url)
    if parsed.scheme != "jam2" or parsed.netloc != "v1":
        raise ValueError("unsupported Jam2 URL")
    values = parse_qs(parsed.query, strict_parsing=True)
    endpoint = values.get("endpoint", [""])[0]
    session = values.get("session", [""])[0]
    key_hex = values.get("key", [""])[0]
    host, separator, port_text = endpoint.rpartition(":")
    if not separator or not host:
        raise ValueError("Jam2 URL endpoint is invalid")
    key = bytes.fromhex(key_hex)
    if len(key) != 16:
        raise ValueError("Jam2 URL key must be exactly 16 bytes")
    return JamUrl(host, int(port_text), int(session, 16), key)
