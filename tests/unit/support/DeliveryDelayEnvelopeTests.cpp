#include "DeliveryDelayEnvelope.hpp"
#include "UdpImpairmentProxy.hpp"
#include "SelectiveDeliveryProxy.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace jam2::test;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        DeliveryDelayEnvelope curve{{{0,0},{18000,0},{20000,400},{22000,650},{24000,0}},180000};
        require(curve.valid(), "Oct30 envelope is valid");
        require(curve.delayMs(10000)==0 && curve.delayMs(19000)==200 &&
            curve.delayMs(21000)==525 && curve.delayMs(23000)==325 &&
            curve.delayMs(24000)==0 && curve.delayMs(201000)==525,
            "baseline, delay growth, catch-up, recovery and three-minute recurrence");
        DeliveryDelayEnvelope gap{{{0,0},{18000,0},{19000,200},{19001,325},{20000,400},{22000,650},{24000,0}},180000};
        require(gap.valid() && gap.delayMs(19000)==200 && gap.delayMs(19000.5)==262.5 &&
            gap.delayMs(19001)==325 && gap.delayMs(199001)==325,
            "short delay step models the observed arrival gap and repeats");
        require(deliveryBatchDelayMs(1,0,4)==3 && deliveryBatchDelayMs(3,0,4)==1 &&
            deliveryBatchDelayMs(4,0,4)==4 && deliveryBatchDelayMs(0,7.5,4)==8 &&
            deliveryBatchDelayMs(7,2,0)==2,"bounded packet windows coalesce delivery and retain fixed delay");
        for (auto bad : {DeliveryDelayEnvelope{{{1,0}},0}, DeliveryDelayEnvelope{{{0,0},{0,1}},0},
                DeliveryDelayEnvelope{{{0,-1}},0}, DeliveryDelayEnvelope{{{0,1}},10},
                DeliveryDelayEnvelope{{{0,0}},std::numeric_limits<double>::infinity()}})
            require(!bad.valid(), "invalid timeline rejected");
        DirectionImpairment direction; direction.deliveryDelay = {{{0,20}},0};
        UdpImpairmentProxy proxy(QHostAddress::LocalHost, 9, direction, {}, 123, 2);
        QString error; require(proxy.start(error), "start bounded proxy");
        QByteArray delivered;
        for (char c : {'a','b','c'}) proxy.forwardDatagram(UdpProxyDirection::ClientToServer,
            QByteArray(1,c), [&](const QByteArray& b) { delivered+=b; return true; });
        require(delivered.isEmpty() && proxy.stats().clientToServer.capacityDropped==1 &&
            proxy.stats().pendingHighWater==2, "delay queues packets within the configured limit");
        QElapsedTimer elapsed; elapsed.start();
        while (delivered.size()<2 && elapsed.elapsed()<500) { proxy.pump(); QCoreApplication::processEvents(); }
        require(delivered=="ab" && proxy.stats().pendingPackets==0 &&
            proxy.stats().clientToServer.requestedDelayMaxUs==20000,
            "delayed datagrams retain order and expose requested timing");
        direction.deliveryDelay.points[0].delayMs=-1;
        UdpImpairmentProxy invalid(QHostAddress::LocalHost,9,direction,{},123);
        require(!invalid.start(error) && !error.isEmpty(), "proxy validates timeline before binding");
        const auto packet = [](jam2::protocol::PacketType type, int bytes) {
            jam2::protocol::Header header{}; header.type=type;header.session_id=1;
            std::vector<std::uint8_t> payload(bytes);
            const auto wire=jam2::protocol::encode_packet(header,payload,{},jam2::NetworkAudioFormat::Pcm24Mono);
            return QByteArray(reinterpret_cast<const char*>(wire.data()),qsizetype(wire.size()));
        };
        const auto audio=packet(jam2::protocol::PacketType::Audio,3);
        const auto beacon=packet(jam2::protocol::PacketType::MetronomeState,56);
        require(deliveryTraffic(audio)==DeliveryTraffic::Audio && deliveryTraffic(beacon)==DeliveryTraffic::Beacon &&
            deliveryTraffic(packet(jam2::protocol::PacketType::Ping,0))==DeliveryTraffic::RoundTrip &&
            deliveryTraffic(packet(jam2::protocol::PacketType::Pong,0))==DeliveryTraffic::RoundTrip &&
            deliveryTraffic(QByteArray("J2MX"))==DeliveryTraffic::Control &&
            deliveryTraffic(QByteArray("JAM2"))==DeliveryTraffic::Control,"classify production packet types without reading truncated headers");
        for (const auto* name : {"all","audio","beacons","ping-pong","non-audio","none"}) {
            const auto scope=deliveryDelayScope(name);
            require(scope && std::string_view(deliveryDelayScopeText(*scope))==name,"scope parses and round-trips");
        }
        require(!deliveryDelayScope("bogus") && !deliverySelected(DeliveryDelayScope::None,DeliveryTraffic::Audio) &&
            !deliverySelected(DeliveryDelayScope::NonAudio,DeliveryTraffic::Audio) &&
            deliverySelected(DeliveryDelayScope::NonAudio,DeliveryTraffic::Beacon),"invalid scopes and explicit selection boundaries");
        direction.deliveryDelay={{{0,20}},0};
        SelectiveDeliveryProxy isolated(9,{},direction,DeliveryDelayScope::Beacon,123,8);
        require(isolated.start(error),"start selective packet queues");
        QByteArray order;
        isolated.forward(UdpProxyDirection::ServerToClient,beacon,[&](const QByteArray&){order+='b';return true;});
        isolated.forward(UdpProxyDirection::ServerToClient,audio,[&](const QByteArray&){order+='a';return true;});
        require(order=="a" && isolated.forwardCounts().selected[std::size_t(DeliveryTraffic::Beacon)]==1 &&
            isolated.forwardCounts().baseline[std::size_t(DeliveryTraffic::Audio)]==1,
            "delaying a beacon must not create audio head-of-line blocking");
        elapsed.restart();
        while (order.size()<2 && elapsed.elapsed()<500) isolated.pump();
        require(order=="ab" && isolated.selectedStats().serverToClient.forwarded==1 &&
            isolated.baselineStats().serverToClient.forwarded==1,"both selective paths deliver and expose independent evidence");
        direction.deliveryDelay={};direction.deliveryBatchMs=20;
        UdpImpairmentProxy batched(QHostAddress::LocalHost,9,direction,{},123,8);
        require(batched.start(error),"start delivery-window proxy");order.clear();
        for (char c : {'a','b','c'}) batched.forwardDatagram(UdpProxyDirection::ClientToServer,QByteArray(1,c),
            [&](const QByteArray& b){order+=b;return true;});
        require(order.isEmpty(),"batch window holds real datagrams");elapsed.restart();
        while (order.size()<3 && elapsed.elapsed()<500) batched.pump();
        require(order=="abc" && batched.stats().clientToServer.forwarded==3 &&
            batched.stats().clientToServer.capacityDropped==0,"packet batches preserve order without synthetic loss");
        direction.deliveryBatchMs=1001;UdpImpairmentProxy invalidBatch(QHostAddress::LocalHost,9,direction,{},123);
        require(!invalidBatch.start(error),"excessive batch window rejected");
        std::cout << "delivery envelope timing, validation, queue bounds and ordered release passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
