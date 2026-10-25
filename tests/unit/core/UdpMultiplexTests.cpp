#include "common.hpp"
#include "reliable_udp_stream.hpp"
#include "network_session.hpp"
#include <array>
#include <thread>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void invitationCompatibility()
{
    jam2::SessionInfo info{{"jam2gether.com", 40404}, 0x123456789abcdef0ULL, {}};
    info.strategy = jam2::ConnectionStrategy::Jam2Server;
    const auto url = jam2::make_jam_url(info);
    const auto parsed = jam2::parse_jam_url(url);
    require(parsed.strategy == jam2::ConnectionStrategy::Jam2Server &&
        parsed.session_id == info.session_id && parsed.key == info.key &&
        parsed.endpoint.host == info.endpoint.host && parsed.endpoint.port == 40404,
        "server invitation did not retain strategy and credentials");
    require(jam2::make_jam_url(parsed) == url, "recreating a jam changed its reusable invitation");
    info.strategy = jam2::ConnectionStrategy::Direct;
    const auto legacy = jam2::make_jam_url(info);
    require(legacy.find("strategy=") == std::string::npos &&
        jam2::parse_jam_url(legacy).strategy == jam2::ConnectionStrategy::Stun,
        "legacy invitation encoding/bootstrap changed");
    bool rejected = false;
    try { (void)jam2::parse_jam_url(legacy + "&strategy=unknown"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "unknown strategy was accepted");
}

void reliableDeliveryWithLossDuplicationAndReordering()
{
    jam2::ReliableUdpStream sender, receiver;
    std::vector<std::uint8_t> input(180000), output;
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>((i * 47 + i / 1024) % 251);
    require(sender.enqueue(input), "bounded stream refused an allowed message");
    struct Packet { std::uint64_t sequence, due; std::vector<std::uint8_t> bytes; };
    std::deque<Packet> packets;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> acknowledgements;
    std::map<std::uint64_t, unsigned> transmissions, ackCount;
    for (std::uint64_t now = 1; now < 10000000 && !sender.empty(); now += 1000) {
        sender.poll(now, 3, [&](std::uint64_t sequence, std::span<const std::uint8_t> bytes, bool) {
            const unsigned count = ++transmissions[sequence];
            if ((sequence == 2 || sequence == 7 || sequence % 29 == 0) && count == 1) return true;
            Packet packet{sequence, now + (sequence % 3 == 0 ? 1000 : 11000), {bytes.begin(), bytes.end()}};
            packets.push_back(packet);
            if (sequence % 11 == 0 && count == 1) packets.push_back(packet);
            return true;
        });
        for (auto it = packets.begin(); it != packets.end();) {
            if (it->due > now) { ++it; continue; }
            const Packet packet = std::move(*it);
            it = packets.erase(it);
            require(receiver.receive(packet.sequence, packet.bytes,
                [&](std::span<const std::uint8_t> bytes) { output.insert(output.end(), bytes.begin(), bytes.end()); },
                [&](std::uint64_t sequence) {
                    if (++ackCount[sequence] == 1 && sequence % 13 == 0) return;
                    acknowledgements.emplace_back(sequence, now + 10000);
                }), "valid reordered fragment rejected");
        }
        for (auto it = acknowledgements.begin(); it != acknowledgements.end();) {
            if (it->second > now) { ++it; continue; }
            sender.acknowledge(it->first, now);
            it = acknowledgements.erase(it);
        }
        require(receiver.reorderedBytes() <= 64 * jam2::ReliableUdpStream::kFragmentBytes,
            "reorder storage grew without a bound");
    }
    require(sender.empty() && output == input, "loss/reordering changed or duplicated delivered bytes");
    require(sender.stats().retransmitted > 0 && receiver.stats().reordered > 0 &&
        receiver.stats().duplicates > 0, "fault injection did not exercise recovery");
    require(sender.stats().smoothed_rtt_us > 0 && !sender.failed(), "RTT/reliability state failed");
}

void audioRoutingRetainsNativePacketBytes()
{
    class Routed final : public jam2::NetworkDatagramTransport {
    public:
        struct Sent { std::uint64_t peer; jam2::ResolvedUdpEndpoint endpoint; std::vector<std::uint8_t> bytes; };
        std::vector<Sent> sent;
        jam2::UdpSendResult sendToPeer(std::uint64_t peer, const jam2::ResolvedUdpEndpoint& endpoint,
            std::span<const std::uint8_t> bytes) noexcept override {
            sent.push_back({peer, endpoint, {bytes.begin(), bytes.end()}});
            return {};
        }
        std::optional<jam2::UdpSocket::ReceivedDatagram> receiveFor(
            std::span<std::uint8_t> buffer, std::uint64_t, jam2::RealtimeWakeSignal*) override {
            if (sent.empty()) return std::nullopt;
            std::copy(sent.front().bytes.begin(), sent.front().bytes.end(), buffer.begin());
            return jam2::UdpSocket::ReceivedDatagram{sent.front().endpoint, sent.front().bytes.size()};
        }
    };
    jam2::NetworkRuntime native;
    jam2::UdpSocket placeholder; placeholder.bind({"127.0.0.1", 0});
    jam2::SessionInfo info{{"127.0.0.1", 0}, 99, {}};
    const std::array<jam2::NetworkPeerDescriptor, 2> peers{{
        {{2}, jam2::resolve_udp_endpoint({"127.0.0.1", 53000}), jam2::PeerEndpointState::Active},
        {{3}, jam2::resolve_udp_endpoint({"127.0.0.1", 54000}), jam2::PeerEndpointState::Active}}};
    jam2::NetworkSessionContract contract;
    jam2::PeerStreamConfig config;
    config.sample_rate = contract.sample_rate;
    config.frames_per_packet = contract.frames_per_packet;
    jam2::NetworkSession session(std::move(placeholder), info, contract,
        jam2::SessionBootstrapRole::Creator, {1}, peers, config, nullptr);
    const auto transport = std::make_shared<Routed>();
    session.setDatagramTransport(transport);
    std::vector<std::uint8_t> pcm(128 * 3);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::uint8_t>(i % 251);
    const auto result = session.sendToActive(jam2::protocol::PacketType::Audio, 7, 896, pcm);
    const auto expected = jam2::protocol::encode_packet(
        {jam2::protocol::PacketType::Audio, 99, 7, 896, 0, 0}, pcm, info.key, contract.audio_format);
    require(result.peer_count == 2 && transport->sent.size() == 2 &&
        transport->sent[0].peer == 2 && transport->sent[1].peer == 3 &&
        transport->sent[0].bytes == expected && transport->sent[1].bytes == expected,
        "per-peer transport routing changed native audio bytes or fan-out");
    const auto incoming = session.receiveFor(0);
    require(incoming && session.parse(incoming->bytes) &&
        incoming->endpoint == peers[0].endpoint && incoming->bytes.size() == expected.size(),
        "routed incoming audio changed native packet parsing");
}

void boundedTransportWake()
{
    jam2::NetworkRuntime native;
    jam2::RealtimeWakeSignal queued, capture;
    queued.requestWake(); capture.requestWake();
    queued.waitFor(1);
    queued.waitFor(1, &capture);
    require(queued.consumptionCount() == 0 && capture.consumptionCount() == 0,
        "microsecond wait timeout consumed an unsignalled wake");
    capture.signal(123);
    queued.waitFor(1000000, &capture);
    require(capture.consumptionCount() == 1 && capture.signalCount() == 1,
        "existing audio-capture wake was not consumed through transport wait");
    queued.signal(124);
    queued.waitFor(1000000, &capture);
    require(queued.consumptionCount() == 1, "queued UDP input did not wake the network receiver");
}

void boundsBackpressureAndIndependentChannels()
{
    jam2::ReliableUdpStream bulk, control, remote;
    std::vector<std::uint8_t> full(jam2::ReliableUdpStream::kQueueBytes, 42);
    require(bulk.enqueue(full) && !bulk.enqueue(std::span(full).first(1)),
        "queued byte limit was not enforced atomically");
    require(bulk.poll(1, 1, [](auto, auto, bool) { return false; }) == 0 &&
        bulk.inFlightFragments() == 0, "would-block consumed unsent data");
    std::vector<std::uint8_t> tiny{1, 2, 3};
    require(control.enqueue(tiny), "independent control channel blocked behind bulk");
    std::vector<std::uint8_t> got;
    for (std::uint64_t now = 1; now < 20000000 && !bulk.failed(); now += 10000) {
        bulk.poll(now, 2, [](auto, auto, bool) { return true; }); // Black-hole every bulk ACK.
        std::vector<std::uint64_t> acks;
        control.poll(now, 1, [&](auto sequence, auto bytes, bool) {
            remote.receive(sequence, bytes,
                [&](auto part) { got.insert(got.end(), part.begin(), part.end()); },
                [&](auto seq) { acks.push_back(seq); });
            return true;
        });
        for (auto seq : acks) control.acknowledge(seq, now + 1000);
    }
    require(got == tiny && control.empty() && !control.failed(),
        "bulk loss blocked or duplicated unrelated control");
    require(bulk.failed() && bulk.stats().timeouts == 1, "black-holed reliable traffic did not time out");
    require(!remote.receive(0, tiny, [](auto) {}, [](auto) {}) &&
        !remote.receive(1000000, tiny, [](auto) {}, [](auto) {}), "invalid receive sequence accepted");
}
}

int main()
{
    try {
        invitationCompatibility();
        reliableDeliveryWithLossDuplicationAndReordering();
        boundsBackpressureAndIndependentChannels();
        audioRoutingRetainsNativePacketBytes();
        boundedTransportWake();
        std::cout << "UDP multiplex strategy and reliable byte-stream checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
