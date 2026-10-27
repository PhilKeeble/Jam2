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
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void localRouteDiscovery()
{
    jam2::NetworkRuntime runtime;
    jam2::UdpSocket wildcard, explicitLocal;
    wildcard.bind({"0.0.0.0", 0});
    explicitLocal.bind({"127.0.0.1", 0});
    const auto remote = jam2::resolve_udp_endpoint(explicitLocal.local_endpoint());
    const auto local = wildcard.local_endpoint_for(remote);
    require(local.host == "127.0.0.1" && local.port == wildcard.local_endpoint().port,
        "LAN candidate did not preserve the exact bound media port");
    const auto fixed = explicitLocal.local_endpoint_for(remote);
    require(fixed.host == "127.0.0.1" && fixed.port == explicitLocal.local_endpoint().port,
        "explicit socket bind changed during route discovery");
    std::array<std::uint8_t, 8> bytes{};
    require(!explicitLocal.recv_from_for(bytes, 0), "local route discovery sent an unsolicited packet");
}

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


void initialRtoLearnsHotspotLatencyWithoutInventingLoss()
{
    jam2::ReliableUdpStream sender, receiver;
    std::vector<std::uint8_t> input(64 * 1024, 42), output;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> acks;
    require(sender.enqueue(input), "enqueue hotspot latency fixture");
    for (std::uint64_t now = 1; now < 10000000 && !sender.empty(); now += 1000) {
        sender.poll(now, 2, [&](auto sequence, auto bytes, bool) {
            receiver.receive(sequence, bytes,
                [&](auto part) { output.insert(output.end(), part.begin(), part.end()); },
                [&](auto seq) { acks.emplace_back(seq, now + 420000); });
            return true;
        });
        while (!acks.empty() && acks.front().second <= now) {
            sender.acknowledge(acks.front().first, now); acks.pop_front();
        }
    }
    require(sender.empty() && output == input, "lossless hotspot latency prevented reliable delivery");
    require(sender.stats().retransmitted == 0, "lossless 420ms RTT triggered premature retransmission and congestion collapse");
    require(sender.stats().smoothed_rtt_us >= 420000 && sender.stats().congestion_window > 4,
        "retransmission ambiguity prevented RTT learning on a hotspot");
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

void receiverOwnershipLifecycle()
{
    class Transport final : public jam2::NetworkDatagramTransport {
    public:
        int attachments = 0, detachments = 0;
        bool reject = false;
        void receiverAttached() override { ++attachments; if (reject) throw std::runtime_error("attachment rejected"); }
        void receiverDetached() noexcept override { ++detachments; }
        jam2::UdpSendResult sendToPeer(std::uint64_t, const jam2::ResolvedUdpEndpoint&,
            std::span<const std::uint8_t>) noexcept override { return {}; }
        std::optional<jam2::UdpSocket::ReceivedDatagram> receiveFor(
            std::span<std::uint8_t>, std::uint64_t, jam2::RealtimeWakeSignal*) override { return {}; }
    };
    jam2::NetworkRuntime native;
    auto makeSession = [] {
        jam2::UdpSocket socket; socket.bind({"127.0.0.1",0});
        return jam2::NetworkSession(std::move(socket), jam2::SessionInfo{{"127.0.0.1",0},99,{}}, {},
            jam2::SessionBootstrapRole::Creator, {1}, std::span<const jam2::NetworkPeerDescriptor>{}, {}, nullptr);
    };
    auto first = std::make_shared<Transport>(), second = std::make_shared<Transport>();
    auto rejected = std::make_shared<Transport>(); rejected->reject = true;
    {
        auto session = makeSession();
        session.setDatagramTransport(first); session.setDatagramTransport(first);
        require(first->attachments==1 && first->detachments==0,"same receiver attachment was duplicated");
        bool threw = false;
        try { session.setDatagramTransport(rejected); } catch (const std::exception&) { threw = true; }
        require(threw && rejected->attachments==1 && rejected->detachments==0 && first->detachments==0,
            "failed receiver attachment detached the current receiver");
        session.setDatagramTransport(second);
        require(first->detachments==1 && second->attachments==1,"receiver replacement did not transfer ownership");
        auto moved = std::move(session);
        auto destination = makeSession(); destination.setDatagramTransport(first);
        destination = std::move(moved);
        require(first->detachments==2 && second->detachments==0,"move assignment lost receiver cleanup");
        destination.close(); destination.close();
        require(second->detachments==1,"close did not detach receiver exactly once");
    }
    require(first->detachments==2 && second->detachments==1,"destruction detached an already closed receiver");
    { auto session = makeSession(); session.setDatagramTransport(first); }
    require(first->detachments==3,"session destruction retained receiver ownership");
}

void routedReceivePreservesMultiplexSizedDatagrams()
{
    class SocketRoute final : public jam2::NetworkDatagramTransport {
    public:
        jam2::UdpSocket socket;
        SocketRoute() { socket.bind({"127.0.0.1",0}); }
        jam2::UdpSendResult sendToPeer(std::uint64_t, const jam2::ResolvedUdpEndpoint&,
            std::span<const std::uint8_t>) noexcept override { return {}; }
        std::optional<jam2::UdpSocket::ReceivedDatagram> receiveFor(
            std::span<std::uint8_t> buffer, std::uint64_t wait, jam2::RealtimeWakeSignal* wake) override {
            return socket.recv_from_for(buffer,wait,wake);
        }
    };
    jam2::NetworkRuntime native;
    jam2::UdpSocket placeholder, sender; placeholder.bind({"127.0.0.1",0}); sender.bind({"127.0.0.1",0});
    jam2::NetworkSession session(std::move(placeholder), jam2::SessionInfo{{"127.0.0.1",0},99,{}}, {},
        jam2::SessionBootstrapRole::Creator, {1}, std::span<const jam2::NetworkPeerDescriptor>{}, {}, nullptr);
    auto route = std::make_shared<SocketRoute>(); session.setDatagramTransport(route);
    for (const std::size_t size : {804,805,1082,1500}) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t index=0; index<size; ++index) bytes[index]=static_cast<std::uint8_t>(index%251);
        require(sender.send_to(jam2::resolve_udp_endpoint(route->socket.local_endpoint()),bytes).outcome==jam2::UdpSendOutcome::Sent,
            "cannot send multiplex-sized receive fixture");
        const auto packet = session.receiveFor(1000000);
        require(packet && packet->bytes.size()==bytes.size() && std::equal(bytes.begin(),bytes.end(),packet->bytes.begin()),
            "routed receive truncated a datagram larger than the native audio maximum");
    }
}

void datagramWaitPreservesPacketsAndCaptureWake()
{
    jam2::NetworkRuntime native;
    jam2::UdpSocket first, second, sender;
    first.bind({"127.0.0.1",0}); second.bind({"127.0.0.1",0}); sender.bind({"127.0.0.1",0});
    const std::array<const jam2::UdpSocket*,2> sockets{&first,&second};
    const std::array<std::uint8_t,4> bytes{9,2,7,4};
    require(sender.send_to(jam2::resolve_udp_endpoint(second.local_endpoint()),bytes).outcome==jam2::UdpSendOutcome::Sent,
        "cannot send multi-socket wait fixture");
    jam2::UdpSocket::waitForDatagrams(sockets,1000000);
    std::array<std::uint8_t,64> buffer{};
    require(!first.recv_from_for(buffer,0),"multi-socket wait changed the destination");
    const auto packet = second.recv_from_for(buffer,0);
    require(packet && packet->size==bytes.size() && std::equal(bytes.begin(),bytes.end(),buffer.begin()),
        "multi-socket wait consumed or altered a native datagram");
    jam2::RealtimeWakeSignal capture; capture.requestWake(); capture.signal(123);
    jam2::UdpSocket::waitForDatagrams(sockets,1000000,&capture);
    require(capture.consumptionCount()==1,"multi-socket wait missed the capture wake");
    jam2::UdpSocket::waitForDatagrams(sockets,1,&capture);
    require(capture.consumptionCount()==1,"empty multi-socket wait consumed an unsignalled wake");
}
void receiveWaitPerformance(bool requestOneMsTimer)
{
#if defined(_WIN32)
    struct TimerPeriod {
        bool active;
        explicit TimerPeriod(bool requested) : active(requested) {
            if (active && timeBeginPeriod(1)!=TIMERR_NOERROR) throw std::runtime_error("failed to request diagnostic 1ms timer period");
        }
        ~TimerPeriod() { if (active) timeEndPeriod(1); }
    } timer(requestOneMsTimer);
#endif
    jam2::NetworkRuntime native;
    jam2::UdpSocket socket; socket.bind({"127.0.0.1",0});
    jam2::RealtimeWakeSignal queued, capture; queued.requestWake(); capture.requestWake();
    std::array<std::uint8_t,64> buffer{};
    constexpr std::array<const char*,4> names{"socket-select","mux-event","socket-capture-event","mux-capture-event"};
    for (const std::uint64_t requested : {1,100,250,500,750,1000,1333}) {
        std::array<std::vector<std::uint64_t>,4> elapsed;
        for (int iteration=0; iteration<48; ++iteration) for (int turn=0; turn<4; ++turn) {
            const auto method=(iteration+turn)%4;
            const auto began=jam2::monotonic_us();
            if (method==0 || method==2) require(!socket.recv_from_for(buffer,requested,method==2 ? &capture : nullptr),"empty wait benchmark received unsolicited data");
            else queued.waitFor(requested,method==3 ? &capture : nullptr);
            elapsed[method].push_back(jam2::monotonic_us()-began);
        }
        for (int method=0; method<4; ++method) {
            auto& values=elapsed[method]; std::sort(values.begin(),values.end());
            std::uint64_t sum=0; for (const auto value:values) sum+=value;
            std::cout << "workload=empty-receive-wait requested_timer_ms=" << (requestOneMsTimer ? 1 : 0)
                << " method=" << names[method] << " requested_us=" << requested
                << " samples=" << values.size() << " min_us=" << values.front() << " median_us=" << values[values.size()/2]
                << " p95_us=" << values[((values.size()-1)*95+99)/100] << " max_us=" << values.back()
                << " mean_us=" << static_cast<double>(sum)/values.size() << '\n';
        }
    }
    require(queued.consumptionCount()==0 && capture.consumptionCount()==0,"timeout benchmark consumed an unsignalled event");
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

int main(int argc, char** argv)
{
    try {
        if (argc==2 && std::string_view(argv[1])=="--wait-performance") {
            receiveWaitPerformance(false);
#if defined(_WIN32)
            receiveWaitPerformance(true);
#endif
            return 0;
        }
        initialRtoLearnsHotspotLatencyWithoutInventingLoss();
        localRouteDiscovery();
        invitationCompatibility();
        reliableDeliveryWithLossDuplicationAndReordering();
        boundsBackpressureAndIndependentChannels();
        audioRoutingRetainsNativePacketBytes();
        boundedTransportWake();
        receiverOwnershipLifecycle();
        routedReceivePreservesMultiplexSizedDatagrams();
        datagramWaitPreservesPacketsAndCaptureWake();
        std::cout << "UDP multiplex strategy and reliable byte-stream checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
