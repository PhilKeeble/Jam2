#include "audio_device.hpp"
#include "audio_ring.hpp"
#include "common.hpp"
#include "midi.hpp"
#include "network_session.hpp"
#include "peer_mixer.hpp"
#include "peer_stream.hpp"
#include "prepared_track_source.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include "stun.hpp"
#include "track_take_recorder.hpp"
#include "tuning_profile.hpp"
#include "udp_socket.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <typename Callable>
void expectThrows(Callable&& callable, const char* message)
{
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

class BufferSink final : public jam2::PeerStreamPlayback {
public:
    std::size_t depthFrames() const noexcept override { return depth; }

    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        depth += frames.size();
        pushed += frames.size();
        return frames.size();
    }

    void requestDropFrames(std::size_t frames) noexcept override
    {
        const std::size_t dropped = (std::min)(depth, frames);
        depth -= dropped;
        requestedDrops += dropped;
    }

    void setResamplerRatio(double value) noexcept override { ratio = value; }

    std::size_t depth = 0;
    std::size_t pushed = 0;
    std::size_t requestedDrops = 0;
    double ratio = 1.0;
};

void putU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void putU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::vector<std::uint8_t> stunResponse(
    std::span<const std::uint8_t, 12> transaction,
    std::uint32_t address,
    std::uint16_t port)
{
    constexpr std::uint32_t magicCookie = 0x2112a442U;
    std::vector<std::uint8_t> response(32, 0);
    putU16(response, 0, 0x0101U);
    putU16(response, 2, 12U);
    putU32(response, 4, magicCookie);
    std::copy(transaction.begin(), transaction.end(), response.begin() + 8);
    putU16(response, 20, 0x0020U);
    putU16(response, 22, 8U);
    response[25] = 0x01U;
    putU16(response, 26, static_cast<std::uint16_t>(port ^ (magicCookie >> 16U)));
    putU32(response, 28, address ^ magicCookie);
    return response;
}

jam2::PeerStreamConfig peerConfig()
{
    jam2::PeerStreamConfig config;
    config.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    config.sample_rate = 48000;
    config.frames_per_packet = 64;
    config.playback_queue_capacity_frames = 512;
    config.playback_max_frames = 1024;
    config.collect_diagnostics = true;
    return config;
}

void testRingMidiAndDownmixBoundaries()
{
    jam2::audio::MonoRingBuffer ring(4);
    expect(ring.available_write() == 4, "empty ring reports its writable capacity");
    const std::array<std::int32_t, 2> frames{10, 20};
    expect(ring.push(frames) == frames.size() && ring.available_write() == 2,
        "ring writable capacity follows pushes");
    ring.set_diagnostics_enabled(true);
    std::array<std::int32_t, 3> output{};
    expect(ring.pop(output) == 2 && output[2] == 0,
        "ring zero-fills a bounded underrun");
    ring.reset();
    const auto resetStats = ring.stats();
    expect(ring.available_read() == 0 && ring.available_write() == 4 &&
            resetStats.underruns == 0 && resetStats.overruns == 0,
        "ring reset restores capacity and clears diagnostics");

    jam2::midi::EventQueue queue;
    expect(queue.push({1, 0, 0x90, 60, 100, 3}), "MIDI clear fixture enqueues");
    queue.clear();
    jam2::midi::Event event;
    expect(queue.depth() == 0 && !queue.pop(event),
        "MIDI clear atomically discards pending events");

    jam2::audio::SmoothedMonoDownmix downmix;
    downmix.configure(2, 48000.0, 64);
    const std::array<double, 2> peaks{0.5, 0.25};
    for (int block = 0; block < 8; ++block) downmix.beginBlock(peaks);
    expect(downmix.channelCount() == 2 && downmix.effectiveWeight() > 0.0 &&
            downmix.maxGainChangePerBlock() >= 0.0,
        "smoothed downmix publishes configured channel diagnostics");
    expect(jam2::audio::block_downmix_channel_active(0.5, 0.5) &&
            jam2::audio::block_downmix_active_channels(peaks) == 2,
        "offline downmix identifies channels relative to the loudest input");
    expect(jam2::audio::downmix_unit_to_ppm(-1.0) == 0 &&
            jam2::audio::downmix_unit_to_ppm(5.0) == 4000000,
        "downmix diagnostic conversion clamps its public range");
    jam2::audio::StreamControl control;
    jam2::audio::publish_downmix_diagnostics(control, downmix);
    expect(control.input_downmix_selected_channels.load() == 2 &&
            control.input_downmix_transition_count.load() >= 2,
        "downmix diagnostics publish to the callback control surface");
}

void testPreparedAndRecorderBoundaries()
{
    jam2::audio::PreparedTrackSource source(32);
    const int loading = source.claimLoadingSlot();
    expect(loading >= 0 && source.loadingData(loading) != nullptr,
        "prepared source exposes a claimed loading slot");
    source.abandonLoadingSlot(loading);
    const int ready = source.claimLoadingSlot();
    expect(ready == loading && source.publishReady(ready, 16, 48000),
        "abandoned loading slot is immediately reusable");
    source.abandonReadySlot(ready);
    expect(source.claimLoadingSlot() == ready,
        "abandoned ready slot is immediately reusable");
    source.abandonLoadingSlot(-1);
    source.abandonReadySlot(99);

    jam2::audio::TrackTakeRecorder recorder(128);
    const auto stats = recorder.stats();
    expect(!stats.armed && !stats.recording && !stats.finalized &&
            stats.queue_capacity_frames == 4096,
        "track-take recorder stats expose a clean state and minimum queue bound");
}

void testProtocolPeerAndMixerBoundaries()
{
    for (int value = static_cast<int>(jam2::protocol::ParseError::None);
         value <= static_cast<int>(jam2::protocol::ParseError::AuthenticationFailed);
         ++value) {
        expect(jam2::protocol::parse_error_text(
                   static_cast<jam2::protocol::ParseError>(value)) != nullptr,
            "every packet parse error has diagnostic text");
    }
    jam2::protocol::ReplayWindow replay;
    expect(replay.observe(10) == jam2::protocol::ReplayResult::New,
        "replay window accepts its first sequence");
    replay.reset();
    expect(replay.observe(10) == jam2::protocol::ReplayResult::New,
        "replay reset forgets the previous window");

    BufferSink firstSink;
    BufferSink secondSink;
    const jam2::PeerStreamConfig streamConfig = peerConfig();
    expect(firstSink.acceptsFrames(),
        "default peer playback sink accepts frames");
    jam2::PeerStream first(streamConfig, jam2::monotonic_us(), &firstSink);
    jam2::PeerStream second(streamConfig, jam2::monotonic_us(), &secondSink);
    first = std::move(second);
    expect(first.config().frames_per_packet == 64 &&
            first.effectivePlayoutDelayFrames() == 0,
        "peer-stream move assignment retains immutable tuning");

    jam2::PeerMixerConfig mixerConfig;
    mixerConfig.sample_rate = 48000;
    mixerConfig.frames_per_block = 64;
    mixerConfig.output_max_frames = 1024;
    jam2::PeerMixer firstMixer(mixerConfig, &firstSink);
    jam2::PeerMixer secondMixer(mixerConfig, &secondSink);
    jam2::PeerStreamPlayback* slot = firstMixer.addPeer(44, 256);
    expect(slot != nullptr, "peer mixer creates a bounded peer slot");
    expect(firstMixer.setPeerActive(44, true),
        "peer mixer explicitly activates a new slot before contribution");
    const std::array<std::int32_t, 4> samples{1, 2, 3, 4};
    expect(slot->pushFrames(samples) > 0,
        "peer mixer slot accepts bounded source frames");
    slot->requestDropFrames(2);
    expect(firstMixer.setPeerGain(44, 750000) && firstMixer.setPeerMuted(44, true),
        "peer mixer applies per-peer gain and mute controls");
    const auto* peerStats = firstMixer.peerStats(44);
    expect(peerStats != nullptr && peerStats->gain_ppm == 750000 && peerStats->muted,
        "peer mixer diagnostics reflect gain and mute controls");
    secondMixer = std::move(firstMixer);
    expect(secondMixer.peerStats(44) != nullptr,
        "peer-mixer move assignment preserves owned slots");
}

void testUdpStunAndSessionBoundaries()
{
    jam2::NetworkRuntime networkRuntime;

    jam2::UdpSocket movedFrom;
    jam2::UdpSocket movedTo;
    movedTo = std::move(movedFrom);
    movedTo.bind({"127.0.0.1", 0});
    movedTo.set_send_buffer_size(65536);
    movedTo.set_recv_buffer_size(65536);
    expect(movedTo.send_buffer_size() > 0 && movedTo.recv_buffer_size() > 0,
        "UDP socket applies and reports positive buffer sizes");
    expectThrows([&] { movedTo.set_send_buffer_size(0); },
        "UDP socket rejects a zero send buffer");
    expectThrows([&] { movedTo.set_recv_buffer_size(-1); },
        "UDP socket rejects a negative receive buffer");
    const std::array<std::uint8_t, 1> invalidPacket{0};
    const auto invalidSend = movedFrom.send_to({}, invalidPacket);
    expect(invalidSend.outcome == jam2::UdpSendOutcome::Fatal,
        "moved-from UDP send reports a classified fatal socket error");

    jam2::UdpSocket duplicateBind;
    expectThrows([&] { duplicateBind.bind(movedTo.local_endpoint()); },
        "UDP bind collision reports its platform socket error");

    const auto request = jam2::stun::make_binding_request();
    expect(request.bytes.size() == 20,
        "STUN binding request uses the fixed RFC header shape");
    const auto directResponse = stunResponse(
        request.transaction_id, 0xcb007109U, 54321);
    const auto directEndpoint = jam2::stun::parse_binding_response(
        directResponse, request.transaction_id);
    expect(directEndpoint.host == "203.0.113.9" && directEndpoint.port == 54321,
        "STUN parser decodes XOR-mapped IPv4 and port");
    expectThrows([&] {
        std::array<std::uint8_t, 4> shortPacket{};
        (void)jam2::stun::parse_binding_response(shortPacket, request.transaction_id);
    }, "STUN parser rejects a short response");

    jam2::UdpSocket stunServer;
    stunServer.bind({"127.0.0.1", 0});
    std::exception_ptr serverFailure;
    std::thread serverThread([&] {
        try {
            std::array<std::uint8_t, 128> buffer{};
            const auto received = stunServer.recv_from(buffer, 1000);
            if (!received || received->size != 20) {
                throw std::runtime_error("fake STUN server did not receive a fixed request");
            }
            std::array<std::uint8_t, 12> transaction{};
            std::copy_n(buffer.begin() + 8, transaction.size(), transaction.begin());
            const auto response = stunResponse(transaction, 0xc6336407U, 45678);
            const auto sent = stunServer.send_to(received->endpoint, response);
            if (sent.outcome != jam2::UdpSendOutcome::Sent) {
                throw std::runtime_error("fake STUN server could not send its response");
            }
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });
    jam2::UdpSocket stunClient;
    stunClient.bind({"127.0.0.1", 0});
    jam2::Endpoint discovered;
    try {
        discovered = jam2::stun::discover_public_endpoint(
            stunClient, stunServer.local_endpoint(), 500, 1);
    } catch (...) {
        serverThread.join();
        throw;
    }
    serverThread.join();
    if (serverFailure) std::rethrow_exception(serverFailure);
    expect(discovered.host == "198.51.100.7" && discovered.port == 45678,
        "STUN discovery accepts a matching local server response");
    expectThrows([&] {
        (void)jam2::stun::discover_public_endpoint(
            stunClient, stunServer.local_endpoint(), 0, 1);
    }, "STUN discovery rejects an invalid timeout");

    BufferSink outputSink;
    jam2::UdpSocket receiverOne;
    receiverOne.bind({"127.0.0.1", 0});
    jam2::UdpSocket sender;
    sender.bind({"127.0.0.1", 0});
    jam2::SessionInfo sessionInfo;
    sessionInfo.session_id = 0x1122334455667788ULL;
    sessionInfo.key.fill(0x5aU);
    jam2::NetworkSessionContract contract;
    contract.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    contract.frames_per_packet = 64;
    const jam2::NetworkPeerDescriptor remote{
        jam2::PeerId{2},
        jam2::resolve_udp_endpoint(receiverOne.local_endpoint()),
        jam2::PeerEndpointState::Active,
    };
    jam2::NetworkSession networkSession(
        std::move(sender), sessionInfo, contract,
        jam2::SessionBootstrapRole::Creator, jam2::PeerId{1}, remote,
        peerConfig(), &outputSink);
    expect(networkSession.sessionId() == sessionInfo.session_id &&
            networkSession.activePeerCount() == 1 &&
            networkSession.remotePeer().peer_id == jam2::PeerId{2} &&
            networkSession.recognizesEndpoint(remote.endpoint),
        "network session exposes its fixed identity and active endpoint");
    expect(networkSession.setPeerGain({2}, 700000) &&
            networkSession.setPeerMuted({2}, true),
        "network session applies peer gain and mute through its mixer");
    auto snapshot = networkSession.snapshot();
    expect(snapshot.peers.size() == 1 && snapshot.peers[0].has_mix_stats &&
            snapshot.peers[0].mix.gain_ppm == 700000 &&
            snapshot.peers[0].mix.muted,
        "network session snapshot publishes exact peer mixer state");

    const std::size_t packetSize = networkSession.send(
        jam2::protocol::PacketType::Ping, 1, 99, {});
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> packet{};
    const auto receivedOne = receiverOne.recv_from(packet, 500);
    expect(packetSize == jam2::protocol::kHeaderSize && receivedOne &&
            receivedOne->size == packetSize &&
            networkSession.parse(std::span<const std::uint8_t>(
                packet.data(), receivedOne ? receivedOne->size : 0)),
        "network session sends and authenticates a fixed-shape ping");

    jam2::UdpSocket receiverTwo;
    receiverTwo.bind({"127.0.0.1", 0});
    const auto replacementEndpoint = jam2::resolve_udp_endpoint(
        receiverTwo.local_endpoint());
    expect(networkSession.updatePeerEndpoint(
               {2}, replacementEndpoint, jam2::PeerEndpointState::Probing) &&
            networkSession.activePeerCount() == 0 &&
            networkSession.recognizesEndpoint(replacementEndpoint) &&
            !networkSession.acceptsEndpoint(replacementEndpoint),
        "network endpoint replacement resets the stream in a non-active state");
    expect(networkSession.setPeerEndpointState({2}, jam2::PeerEndpointState::Active) &&
            networkSession.activePeerCount() == 1,
        "network endpoint promotion restores active fan-out");
    const std::size_t replacementPacketSize = networkSession.send(
        jam2::protocol::PacketType::Ping, 2, 100, {});
    const auto receivedTwo = receiverTwo.recv_from(packet, 500);
    expect(replacementPacketSize == jam2::protocol::kHeaderSize && receivedTwo &&
            receivedTwo->size == replacementPacketSize &&
            !receiverOne.recv_from(packet, 0),
        "network endpoint replacement sends only to the promoted endpoint");

    jam2::UdpSocket placeholderSocket;
    placeholderSocket.bind({"127.0.0.1", 0});
    const jam2::NetworkPeerDescriptor placeholderPeer{
        jam2::PeerId{10}, remote.endpoint, jam2::PeerEndpointState::Active};
    jam2::NetworkSession owner(
        std::move(placeholderSocket), sessionInfo, contract,
        jam2::SessionBootstrapRole::Joiner, jam2::PeerId{9}, placeholderPeer,
        peerConfig(), &outputSink);
    owner = std::move(networkSession);
    expect(owner.sessionId() == sessionInfo.session_id && owner.peerCount() == 1,
        "network-session move assignment preserves the owned session");
    owner.close();
    expect(owner.bootstrapState() == jam2::SessionBootstrapState::Closed &&
            !owner.recognizesEndpoint(replacementEndpoint),
        "network session close rejects every former endpoint");
    expectThrows([&] {
        (void)owner.send(jam2::protocol::PacketType::Ping, 2, 100, {});
    }, "closed network session rejects sends");
}

void testSmallDiagnosticBoundaries()
{
    jam2::SessionAuthority authority(7, 7, 9);
    expect(authority.localPeerId() == 7,
        "session authority exposes its local peer identity");
    expect(!jam2::tuning_profile_names().empty(),
        "tuning profile diagnostics enumerate maintained profiles");
}

} // namespace

int main()
{
    try {
        testRingMidiAndDownmixBoundaries();
        testPreparedAndRecorderBoundaries();
        testProtocolPeerAndMixerBoundaries();
        testUdpStunAndSessionBoundaries();
        testSmallDiagnosticBoundaries();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: unexpected core-boundary exception: "
                  << exception.what() << '\n';
    }

    if (failures != 0) {
        std::cerr << failures << " core-boundary checks failed\n";
        return 1;
    }
    std::cout << "core boundary checks passed\n";
    return 0;
}
