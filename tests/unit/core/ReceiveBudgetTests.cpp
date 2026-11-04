#include "network_session.hpp"
#include "peer_mixer.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

void budgets()
{
    jam2::DatagramReceiveBudget work{3, 100};
    require(work.idle() && work.canRead(1000000), "waiting before the first packet must not consume service time");
    work.recordPacket(1000000);
    require(!work.idle() && work.canRead(1000099), "service budget must allow work below its elapsed limit");
    require(!work.canRead(1000100) && work.exhausted() && work.service_exhausted,
        "elapsed service limit must yield before another packet is read");
    jam2::DatagramReceiveBudget packets{2, 0};
    packets.recordPacket(0);
    require(packets.canRead(1000000000), "zero disables the elapsed limit for a controlled comparison");
    packets.recordPacket(1000000000);
    require(!packets.canRead(1000000001) && packets.exhausted() && !packets.service_exhausted,
        "packet exhaustion must be independent of the elapsed limit");
    jam2::DatagramReceiveBudget none{0, 0};
    require(!none.canRead(0) && !none.idle(), "an exhausted zero budget must neither read nor report an idle socket");
    jam2::DatagramReceiveBudget rewind{4, 10};
    rewind.recordPacket(100);
    require(rewind.canRead(99), "a clock rewind must not underflow the service budget");
}

struct Transport final : jam2::NetworkDatagramTransport {
    int reads = 0;
    bool available = true;
    jam2::UdpSendResult sendToPeer(std::uint64_t, const jam2::ResolvedUdpEndpoint&,
        std::span<const std::uint8_t>) noexcept override { return {}; }
    std::optional<jam2::UdpSocket::ReceivedDatagram> receiveFor(std::span<std::uint8_t> output,
        std::uint64_t, jam2::RealtimeWakeSignal*) override {
        ++reads;
        if (!available) return {};
        output[0] = 42;
        return jam2::UdpSocket::ReceivedDatagram{{}, 1};
    }
};
void sessionAccounting()
{
    auto transport = std::make_shared<Transport>();
    jam2::NetworkSessionContract contract;
    jam2::PeerStreamConfig config;
    std::array<jam2::NetworkPeerDescriptor, 1> peers{{{{2}, {}, jam2::PeerEndpointState::Active}}};
    jam2::NetworkSession session(std::static_pointer_cast<jam2::NetworkDatagramTransport>(transport),
        jam2::SessionInfo{}, contract, jam2::SessionBootstrapRole::Creator, {1}, peers, config, nullptr);
    jam2::DatagramReceiveBudget work{2, 0};
    for (int i = 0; i < 2; ++i) {
        const auto packet = session.receiveFor(0, nullptr, &work);
        require(packet && packet->bytes.size() == 1 && packet->bytes[0] == 42,
            "bounded receive must preserve native bytes and ownership");
    }
    require(!session.receiveFor(0, nullptr, &work) && transport->reads == 2 && work.exhausted(),
        "NetworkSession must not call its transport after shared budget exhaustion");
    jam2::DatagramReceiveBudget empty{2, 100};
    transport->available = false;
    require(!session.receiveFor(0, nullptr, &empty) && empty.idle() && empty.packets == 0,
        "empty receives must not consume packet budget");
    session.close();
    transport->available = true;
    require(!session.receiveFor(0, nullptr, &empty) && empty.packets == 0,
        "closed sessions must leave fresh receive work untouched");
}

struct Sink final : jam2::PeerStreamPlayback {
    std::array<std::int32_t, 2048> frames{};
    std::size_t depth = 0;
    std::uint64_t underruns = 0;
    std::size_t depthFrames() const noexcept override { return depth; }
    std::size_t callbackFrames() const noexcept override { return 32; }
    std::uint64_t underrunFrames() const noexcept override { return underruns; }
    std::size_t pushFrames(std::span<const std::int32_t> input) noexcept override {
        const auto count = (std::min)(input.size(), frames.size() - depth);
        std::copy_n(input.begin(), count, frames.begin() + depth); depth += count; return count;
    }
    void requestDropFrames(std::size_t) noexcept override {}
    void setResamplerRatio(double) noexcept override {}
};
std::uint64_t padding(bool reportExhaustion, std::uint64_t missing)
{
    Sink sink;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 64;
    config.output_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::int32_t, 64> pcm{}; pcm.fill(1000000);
    for (int i = 0; i < 3; ++i) {
        peers[i] = mixer.addPeer(i + 1, 1024);
        require(peers[i] && mixer.setPeerActive(i + 1, true), "four participants must own three active mixer sources");
        peers[i]->pushFrames(pcm);
    }
    mixer.advance(1000000);
    mixer.finishReceiveBatch(false);
    sink.depth = 0; sink.underruns += missing;
    const auto baseline = mixer.stats().adaptive_padding_frames;
    mixer.advance(1000100);
    // This is precisely the runtime's end-of-turn call after classification.
    mixer.finishReceiveBatch(reportExhaustion);
    const auto beforeAudio = mixer.stats().adaptive_padding_frames - baseline;
    for (auto* peer : peers) peer->pushFrames(pcm);
    mixer.advance(1000200);
    mixer.finishReceiveBatch(false);
    const auto extra = mixer.stats().adaptive_padding_frames - baseline;
    std::cout << "METRIC classification_padding exhaustion=" << reportExhaustion
        << " missing_frames=" << missing
        << " padding_before_pcm=" << beforeAudio << " total_padding_frames=" << extra
        << " output_frames=" << sink.depth << " target_frames=" << mixer.stats().adaptive_target_frames << '\n';
    require(mixer.stats().contributing_peers == 3, "classification yield must preserve all contributors");
    return extra;
}
}
int main()
{
    try {
        budgets(); sessionAccounting();
        for (std::uint64_t missing : {1ULL, 16ULL, 32ULL, 64ULL}) {
            const auto legacy = padding(false, missing), candidate = padding(true, missing);
            require(legacy > candidate && candidate == 0,
                "preserving receive exhaustion must let queued real PCM replace premature recovery silence");
        }
        std::cout << "receive work, service deadlines, cancellation and four-peer padding checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
