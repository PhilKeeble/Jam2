#include "audio_ring.hpp"
#include "common.hpp"
#include "protocol.hpp"
#include "stun.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const char* message)
{
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void test_url_roundtrip()
{
    jam2::SessionInfo info;
    info.endpoint = {"127.0.0.1", 49000};
    info.session_id = 0x1122334455667788ULL;
    for (std::size_t i = 0; i < info.key.size(); ++i) {
        info.key[i] = static_cast<std::uint8_t>(i + 1);
    }
    const std::string url = jam2::make_jam_url(info);
    const auto parsed = jam2::parse_jam_url(url);
    require(parsed.endpoint.host == info.endpoint.host, "URL endpoint host mismatch");
    require(parsed.endpoint.port == info.endpoint.port, "URL endpoint port mismatch");
    require(parsed.session_id == info.session_id, "URL session mismatch");
    require(parsed.key == info.key, "URL key mismatch");
    require(jam2::parse_bind_endpoint("127.0.0.1:0").port == 0, "bind endpoint should allow port 0");
    require_throws([] { (void)jam2::parse_jam_url("http://example.invalid"); }, "invalid URL should fail");
    require_throws([] { (void)jam2::parse_endpoint("127.0.0.1:0"); }, "public endpoint should reject port 0");
}

void test_protocol_auth()
{
    jam2::SessionInfo info;
    info.session_id = 7;
    info.key.fill(0x42);
    const jam2::protocol::Header header{
        jam2::protocol::PacketType::Hello,
        0,
        info.session_id,
        10,
        0,
        1234,
        0,
        0,
    };
    const auto packet = jam2::protocol::encode_packet(header, {}, info.key);
    const auto decoded = jam2::protocol::decode_packet(packet, info.key, info.session_id);
    require(decoded.type == jam2::protocol::PacketType::Hello, "packet type mismatch");
    require(decoded.sequence == 10, "packet sequence mismatch");

    auto wrong_key = info.key;
    wrong_key[0] ^= 0xff;
    require_throws([&] { (void)jam2::protocol::decode_packet(packet, wrong_key, info.session_id); }, "wrong key should fail");
    require_throws([&] { (void)jam2::protocol::decode_packet(packet, info.key, 8); }, "wrong session should fail");

    auto tampered = packet;
    tampered.back() ^= 0x7f;
    require_throws([&] { (void)jam2::protocol::decode_packet(tampered, info.key, info.session_id); }, "tampered payload should fail auth");

    auto bad_length = packet;
    bad_length.pop_back();
    require_throws([&] { (void)jam2::protocol::decode_packet(bad_length, info.key, info.session_id); }, "bad packet length should fail");
}

void test_protocol_packet_types_and_payloads()
{
    std::array<std::uint8_t, 16> key{};
    key.fill(0x24);
    const std::vector<jam2::protocol::PacketType> types{
        jam2::protocol::PacketType::Hello,
        jam2::protocol::PacketType::HelloAck,
        jam2::protocol::PacketType::Audio,
        jam2::protocol::PacketType::Ping,
        jam2::protocol::PacketType::Pong,
        jam2::protocol::PacketType::MetronomeState,
        jam2::protocol::PacketType::Bye,
    };
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
    std::uint32_t sequence = 1;
    for (const auto type : types) {
        const jam2::protocol::Header header{
            type,
            0,
            0xabcdefULL,
            sequence++,
            44,
            55,
            0,
            0,
        };
        const auto packet = jam2::protocol::encode_packet(header, payload, key);
        require(packet.size() == jam2::protocol::kHeaderSize + payload.size(), "encoded packet size mismatch");
        const auto decoded = jam2::protocol::decode_packet(packet, key, 0xabcdefULL);
        require(decoded.type == type, "packet type roundtrip mismatch");
        require(decoded.payload_length == payload.size(), "payload length roundtrip mismatch");
        require(decoded.sample_time == 44, "sample timestamp roundtrip mismatch");
        require(decoded.send_time_us == 55, "send timestamp roundtrip mismatch");
    }
}

void test_peer_lock_rule_shape()
{
    const jam2::Endpoint first{"127.0.0.1", 50000};
    const jam2::Endpoint second{"127.0.0.1", 50001};
    std::optional<jam2::Endpoint> locked;
    auto accepts = [&](const jam2::Endpoint& from) {
        if (!locked) {
            locked = from;
            return true;
        }
        return locked->host == from.host && locked->port == from.port;
    };
    require(accepts(first), "first peer should lock");
    require(accepts(first), "locked peer should continue");
    require(!accepts(second), "second peer should be rejected after lock");
}

void test_pcm24()
{
    const std::vector<std::int32_t> samples{0, 1, -1, 8388607, -8388608, 9000000, -9000000};
    const auto packed = jam2::protocol::pack_pcm24(samples);
    require(packed.size() == samples.size() * 3, "PCM24 packed size mismatch");
    const auto unpacked = jam2::protocol::unpack_pcm24(packed);
    const std::vector<std::int32_t> expected{0, 1, -1, 8388607, -8388608, 8388607, -8388608};
    require(unpacked == expected, "PCM24 roundtrip/clamp mismatch");
    require_throws([] {
        const std::array<std::uint8_t, 1> bad{0};
        (void)jam2::protocol::unpack_pcm24(bad);
    }, "invalid PCM24 byte count should fail");
}

void put_be16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void put_be32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{
    out[offset] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[offset + 3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::vector<std::uint8_t> make_stun_response(
    const std::array<std::uint8_t, 12>& transaction_id,
    std::uint8_t family)
{
    constexpr std::uint32_t cookie = 0x2112a442U;
    std::vector<std::uint8_t> response(28);
    put_be16(response, 0, 0x0101);
    put_be16(response, 2, 8);
    put_be32(response, 4, cookie);
    for (std::size_t i = 0; i < transaction_id.size(); ++i) {
        response[8 + i] = transaction_id[i];
    }
    put_be16(response, 20, 0x0020);
    put_be16(response, 22, 8);
    response[24] = 0;
    response[25] = family;
    put_be16(response, 26, static_cast<std::uint16_t>(49000U ^ (cookie >> 16)));
    response.resize(32);
    const std::uint32_t ip = (203U << 24) | (0U << 16) | (113U << 8) | 10U;
    put_be32(response, 28, ip ^ cookie);
    put_be16(response, 2, 12);
    put_be16(response, 22, 8);
    return response;
}

void test_stun_parse()
{
    std::array<std::uint8_t, 12> tx{};
    for (std::size_t i = 0; i < tx.size(); ++i) {
        tx[i] = static_cast<std::uint8_t>(i + 10);
    }
    const auto response = make_stun_response(tx, 0x01);
    const auto endpoint = jam2::stun::parse_binding_response(response, tx);
    require(endpoint.host == "203.0.113.10", "STUN mapped address mismatch");
    require(endpoint.port == 49000, "STUN mapped port mismatch");

    auto wrong_tx = tx;
    wrong_tx[0] ^= 0xff;
    require_throws([&] { (void)jam2::stun::parse_binding_response(response, wrong_tx); }, "STUN transaction mismatch should fail");
    require_throws([&] {
        const std::array<std::uint8_t, 3> short_packet{0, 1, 2};
        (void)jam2::stun::parse_binding_response(short_packet, tx);
    }, "short STUN packet should fail");
    const auto ipv6_response = make_stun_response(tx, 0x02);
    require_throws([&] { (void)jam2::stun::parse_binding_response(ipv6_response, tx); }, "unsupported STUN address family should fail");
}

void test_sequence_tracker()
{
    jam2::protocol::SequenceTracker tracker;
    require(tracker.observe(10) == jam2::protocol::SequenceResult::InOrder, "first sequence should be in order");
    require(tracker.observe(11) == jam2::protocol::SequenceResult::InOrder, "next sequence should be in order");
    require(tracker.observe(11) == jam2::protocol::SequenceResult::Duplicate, "duplicate sequence not detected");
    require(tracker.observe(14) == jam2::protocol::SequenceResult::InOrder, "gap sequence should advance");
    require(tracker.observe(13) == jam2::protocol::SequenceResult::OutOfOrder, "out-of-order sequence not detected");
    require(tracker.observe(100) == jam2::protocol::SequenceResult::InOrder, "large jump should advance");
    require(tracker.observe(20) == jam2::protocol::SequenceResult::Late, "late sequence outside window not detected");
    const auto& stats = tracker.stats();
    require(stats.duplicate == 1, "duplicate count mismatch");
    require(stats.out_of_order == 1, "out-of-order count mismatch");
    require(stats.late == 1, "late count mismatch");
    require(stats.lost == 87, "lost count mismatch");
}

void test_audio_ring()
{
    jam2::audio::MonoRingBuffer ring(4);
    const std::array<std::int32_t, 3> first{1, 2, 3};
    require(ring.push(first) == 3, "ring push count mismatch");
    require(ring.available_read() == 3, "ring readable mismatch after push");

    std::array<std::int32_t, 2> out{};
    require(ring.pop(out) == 2, "ring pop count mismatch");
    require(out[0] == 1 && out[1] == 2, "ring pop content mismatch");

    const std::array<std::int32_t, 4> second{4, 5, 6, 7};
    require(ring.push(second) == 3, "ring overrun push count mismatch");
    require(ring.stats().overruns == 1, "ring overrun count mismatch");

    std::array<std::int32_t, 5> wrapped{};
    require(ring.pop(wrapped) == 4, "ring wrapped pop count mismatch");
    const std::array<std::int32_t, 5> expected{3, 4, 5, 6, 0};
    require(wrapped == expected, "ring wrapped content or underrun fill mismatch");
    require(ring.stats().underruns == 1, "ring underrun count mismatch");
    require(ring.stats().underrun_events == 1, "ring underrun event count mismatch");

    ring.reset();
    require(ring.available_read() == 0, "ring reset readable mismatch");
    require(
        ring.stats().overruns == 0 &&
        ring.stats().underruns == 0 &&
        ring.stats().underrun_events == 0,
        "ring reset stats mismatch");

    const std::array<std::int32_t, 4> drop_source{10, 11, 12, 13};
    require(ring.push(drop_source) == 4, "ring drop setup push mismatch");
    require(ring.drop_oldest(2) == 2, "ring drop count mismatch");
    std::array<std::int32_t, 2> remaining{};
    require(ring.pop(remaining) == 2, "ring drop remaining pop mismatch");
    require(remaining[0] == 12 && remaining[1] == 13, "ring drop remaining content mismatch");
}

} // namespace

int main()
{
    try {
        test_url_roundtrip();
        test_protocol_auth();
        test_protocol_packet_types_and_payloads();
        test_peer_lock_rule_shape();
        test_pcm24();
        test_stun_parse();
        test_sequence_tracker();
        test_audio_ring();
        std::cout << "jam2 core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
