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

void reusableInvitations()
{
    jam2::SessionInfo info{{"jam2gether.com", 40440}, 0x123456789abcdef0ULL, {}};
    info.strategy = jam2::ConnectionStrategy::Jam2Server;
    const auto url = jam2::make_jam_url(info);
    const auto parsed = jam2::parse_jam_url(url);
    require(parsed.strategy == jam2::ConnectionStrategy::Jam2Server &&
        parsed.session_id == info.session_id && parsed.key == info.key &&
        parsed.endpoint.host == info.endpoint.host && parsed.endpoint.port == 40440,
        "server invitation did not retain strategy and credentials");
    require(jam2::make_jam_url(parsed) == url, "recreating a jam changed its reusable invitation");
    info.strategy = jam2::ConnectionStrategy::Direct;
    const auto direct = jam2::make_jam_url(info);
    require(direct.find("strategy=direct") != std::string::npos &&
        jam2::parse_jam_url(direct).strategy == jam2::ConnectionStrategy::Direct,
        "direct invitation lost its discovery strategy");
    info.strategy=jam2::ConnectionStrategy::Stun;
    require(jam2::parse_jam_url(jam2::make_jam_url(info)).strategy==jam2::ConnectionStrategy::Stun,"STUN invitation lost its discovery strategy");
    bool rejected = false;
    try { (void)jam2::parse_jam_url(jam2::make_jam_url(info) + "&strategy=unknown"); }
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
    jam2::SessionInfo info{{"127.0.0.1", 0}, 99, {}};
    const std::array<jam2::NetworkPeerDescriptor, 2> peers{{
        {{2}, jam2::resolve_udp_endpoint({"127.0.0.1", 53000}), jam2::PeerEndpointState::Active},
        {{3}, jam2::resolve_udp_endpoint({"127.0.0.1", 54000}), jam2::PeerEndpointState::Active}}};
    jam2::NetworkSessionContract contract;
    jam2::PeerStreamConfig config;
    config.sample_rate = contract.sample_rate;
    config.frames_per_packet = contract.frames_per_packet;
    const auto transport = std::make_shared<Routed>();
    jam2::NetworkSession session(std::shared_ptr<jam2::NetworkDatagramTransport>(transport), info, contract,
        jam2::SessionBootstrapRole::Creator, {1}, peers, config, nullptr);
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

void inOrderAndSelectiveAcknowledgements()
{
    jam2::ReliableUdpStream sender, receiver;
    const std::vector<std::uint8_t> bytes(3*jam2::ReliableUdpStream::kFragmentBytes,42);
    require(sender.enqueue(bytes),"selective ACK enqueue");
    require(sender.hasSendCredit(),"newly queued stream has no send credit");
    sender.poll(1,4,[](auto,auto,bool) { return true; });
    std::vector<std::uint8_t> result;
    auto deliver=[&](auto part) { result.insert(result.end(),part.begin(),part.end()); };
    int acks=0; auto ack=[&](auto) { ++acks; };
    const auto fragment=std::span(bytes).first(jam2::ReliableUdpStream::kFragmentBytes);
    require(receiver.receive(2,fragment,deliver,ack) && receiver.receive(3,fragment,deliver,ack),"out-of-order receive");
    const auto gap=receiver.acknowledgementWindow();
    require(gap.contiguous==0 && gap.selective==6 && result.empty(),"selective ACK crossed a missing first fragment");
    require(!sender.acknowledgeWindow({1,std::uint64_t{1}<<63},20) && sender.inFlightFragments()==3,
        "invalid ACK window partially freed valid fragments");
    require(sender.acknowledgeWindow(gap,20) && sender.inFlightFragments()==1,"selective ACK did not preserve gap");
    require(receiver.receive(1,fragment,deliver,ack) && result==bytes,"fast receive did not drain reordered fragments in order");
    require(receiver.stats().in_order_direct==1 && receiver.stats().reorder_insertions==2,
        "in-order fragment used owned reorder storage");
    const auto complete=receiver.acknowledgementWindow();
    require(complete.contiguous==3 && complete.selective==0 && sender.acknowledgeWindow(complete,30) && sender.empty(),
        "contiguous ACK did not complete sender");
    require(receiver.receive(1,fragment,deliver,ack) && result==bytes && acks==4,"duplicate was redelivered or not acknowledged");
    require(!sender.hasSendCredit(),"empty stream requested immediate worker turns");
    require(!sender.acknowledgeWindow({4,0},40),"ACK accepted an unsent contiguous sequence");
    jam2::ReliableUdpStream conflicting;
    require(conflicting.receive(2,fragment,[](auto) {},[](auto) {}),"conflict fixture failed");
    auto changed=std::vector<std::uint8_t>(fragment.begin(),fragment.end()); changed[0]^=1;
    require(!conflicting.receive(2,changed,[](auto) {},[](auto) {}) && conflicting.acknowledgementWindow().selective==2,
        "conflicting out-of-order retransmission replaced authenticated pending bytes");
    jam2::ReliableUdpStream paced;
    require(paced.enqueue(std::vector<std::uint8_t>(8*jam2::ReliableUdpStream::kFragmentBytes,1)),"send-credit fixture enqueue");
    require(paced.poll(1,2,[](auto,auto,bool) { return true; })==2 && paced.hasSendCredit(),"bounded poll hid remaining congestion credit");
    require(paced.poll(2,2,[](auto,auto,bool) { return true; })==2 && !paced.hasSendCredit(),"full congestion window requested immediate sends");
    paced.acknowledge(1,3);
    require(paced.hasSendCredit() && paced.poll(4,2,[](auto,auto,bool) { return false; })==0,
        "would-block consumed newly available send credit");
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
    {
        auto direct = std::shared_ptr<jam2::NetworkDatagramTransport>(first);
        jam2::NetworkSession session(direct, jam2::SessionInfo{{"127.0.0.1",0},99,{}}, {},
            jam2::SessionBootstrapRole::Creator, {1}, std::span<const jam2::NetworkPeerDescriptor>{}, {}, nullptr);
        require(first->attachments==4,"construction did not attach the routed receiver");
        bool threw=false;
        try { session.setDatagramTransport({}); } catch (const std::invalid_argument&) { threw=true; }
        require(threw && first->detachments==3,"routed session silently fell back to a missing native socket");
    }
    require(first->detachments==4,"routed construction did not release receiver ownership");
    for (auto transport : {std::shared_ptr<Transport>{}, rejected}) {
        bool threw=false;
        try {
            jam2::NetworkSession session(std::shared_ptr<jam2::NetworkDatagramTransport>(transport),
                jam2::SessionInfo{{"127.0.0.1",0},99,{}}, {}, jam2::SessionBootstrapRole::Creator,
                {1}, std::span<const jam2::NetworkPeerDescriptor>{}, {}, nullptr);
        } catch (const std::exception&) { threw=true; }
        require(threw,"invalid routed construction succeeded");
    }
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

void selectiveFastRecovery()
{
    using Stream=jam2::ReliableUdpStream;
    const std::vector<std::uint8_t> data(4096,42);
    const auto transmit=[](auto,auto,bool) { return true; };
    Stream fast;
    require(fast.enqueue(data) && fast.poll(1,4,transmit)==4,"fast recovery initial window");
    require(fast.acknowledgeWindow({0,14},1000) && fast.inFlightFragments()==1,"selective evidence lost unacknowledged gap");
    require(fast.poll(10999,4,transmit)==0,"ordinary reorder grace was bypassed");
    require(fast.poll(11000,4,[](auto,auto,bool) { return false; })==0 && fast.stats().fast_retransmitted==0,"would-block consumed early retry");
    require(fast.poll(11001,4,[](auto sequence,auto bytes,bool retry) { return sequence==1 && retry && bytes.size()==1024; })==1 &&
        fast.stats().fast_retransmitted==1,"selective gap waited for bootstrap RTO");
    require(fast.acknowledgeWindow({0,14},12000) && fast.poll(22000,4,transmit)==0,"duplicate ACK repeated early repair");
    require(fast.poll(111001,4,transmit)==1 && fast.stats().fast_retransmitted==1,"ordinary retry timer did not recover lost early repair");
    Stream reordered;
    require(reordered.enqueue(data) && reordered.poll(1,4,transmit)==4 && reordered.acknowledgeWindow({0,14},1000) &&
        reordered.acknowledgeWindow({4,0},10000) && reordered.poll(11000,4,transmit)==0 && reordered.empty() && reordered.stats().fast_retransmitted==0,
        "reordered fragment arriving in grace caused a spurious resend");
    Stream duplicate;
    require(duplicate.enqueue(data) && duplicate.poll(1,4,transmit)==4,"duplicate evidence setup");
    for (int i=0;i<10;++i) require(duplicate.acknowledgeWindow({0,2},1000+i),"duplicate ACK validity");
    require(duplicate.poll(50000,4,transmit)==0,"duplicate ACKs counted as distinct later fragments");
    Stream sparse; require(sparse.enqueue(std::span(data).first(1)) && sparse.poll(1,1,transmit)==1 &&
        sparse.poll(1000000,1,transmit)==0 && sparse.poll(1000001,1,transmit)==1,"uniform recovery lost the no-evidence timer");
    Stream reference(Stream::Options{true,true,false});
    require(reference.enqueue(data) && reference.poll(1,4,transmit)==4 && reference.acknowledgeWindow({0,14},1000) &&
        reference.poll(11000,4,transmit)==0,"disabled fast recovery still repaired early");
    Stream longGrace(Stream::Options{true,true,true,2000000});
    require(longGrace.enqueue(data) && longGrace.poll(1,4,transmit)==4 && longGrace.acknowledgeWindow({0,14},1000) &&
        longGrace.poll(1000001,4,transmit)==1 && longGrace.poll(2001000,4,transmit)==0 && longGrace.stats().fast_retransmitted==0,
        "late gap grace bypassed ordinary retry backoff");
    std::cout << "selective_recovery distinct_later=3 reorder_grace_us=10000 early_retry_us=11001 bootstrap_retry_us=1000001\n";
}
void unsentPackingPreservesOwnership()
{
    using Stream=jam2::ReliableUdpStream;
    Stream packed,reference(Stream::Options{true,false});
    const std::vector<std::uint8_t> bytes(73,42);
    for (int i=0;i<8;++i) require(packed.enqueue(bytes) && reference.enqueue(bytes),"queue scalar packing fixture");
    std::vector<std::uint8_t> delivered;
    require(packed.poll(1,32,[&](auto,auto part,bool) { delivered.insert(delivered.end(),part.begin(),part.end()); return true; })==1 &&
        reference.poll(1,32,[](auto,auto,bool) { return true; })==4,"already queued scalars were not packed into one fragment");
    require(delivered==std::vector<std::uint8_t>(584,42) && packed.stats().fragments_avoided==7 && packed.stats().tail_bytes_packed==511,
        "packing counters or concatenated bytes differ");
    for (std::uint64_t sequence=1;sequence<=4;++sequence) reference.acknowledge(sequence,2);
    require(reference.poll(3,32,[](auto,auto part,bool) { return part.size()==73; })==4,"unpacked reference fragment count changed");
    const auto original=delivered;
    require(packed.enqueue(bytes),"enqueue after transmit");
    std::vector<std::uint8_t> retry;
    packed.poll(1000001,1,[&](auto sequence,auto part,bool repeated) {
        require(sequence==1 && repeated,"first packet was not the timed-out fragment"); retry.assign(part.begin(),part.end()); return true;
    });
    require(retry==original,"later write mutated a sequenced retry fragment");
    packed.acknowledge(1,1000002);
    require(packed.poll(1000003,1,[&](auto sequence,auto part,bool repeated) {
        require(sequence==2 && !repeated && part.size()==73,"pending packet was extended or sequence skipped"); return true;
    })==1,"sparse write waited for another write");
    Stream blocked;
    require(blocked.enqueue(bytes) && blocked.poll(1,1,[](auto,auto,bool) { return false; })==0 && blocked.enqueue(bytes),"would-block packing setup");
    require(blocked.poll(2,1,[](auto seq,auto part,bool retrying) { return seq==1 && part.size()==146 && !retrying; })==1,
        "would-block allocated a sequence or prevented unsequenced tail packing");
    Stream boundary;
    require(boundary.enqueue(std::vector<std::uint8_t>(1000,1)) && boundary.enqueue(std::vector<std::uint8_t>(1000,2)),"cross-fragment packing setup");
    std::vector<std::vector<std::uint8_t>> packets;
    boundary.poll(1,2,[&](auto,auto part,bool) { packets.emplace_back(part.begin(),part.end()); return true; });
    require(packets.size()==2 && packets[0].size()==1024 && packets[1].size()==976 && packets[0][999]==1 && packets[0][1000]==2,
        "record boundary packing lost bytes");
    Stream remote; std::vector<std::uint8_t> received;
    auto accept=[&](auto part) { received.insert(received.end(),part.begin(),part.end()); };
    require(remote.receive(2,packets[1],accept,[](auto) {}) && received.empty() &&
        remote.receive(1,packets[0],accept,[](auto) {}) && received.size()==2000 &&
        received[999]==1 && received[1000]==2,"loss/reordering across packed record boundaries changed byte order");
    std::cout << "packing_fixture reference_fragments=8 packed_fragments=1 sparse_poll=immediate retry_bytes=unchanged\n";
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
        reusableInvitations();
        reliableDeliveryWithLossDuplicationAndReordering();
        boundsBackpressureAndIndependentChannels();
        unsentPackingPreservesOwnership();
        selectiveFastRecovery();
        audioRoutingRetainsNativePacketBytes();
        boundedTransportWake();
        inOrderAndSelectiveAcknowledgements();
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
