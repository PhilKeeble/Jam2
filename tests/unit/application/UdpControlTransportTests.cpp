#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "UdpStreamConnection.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <QtEndian>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>

#include <deque>
#include <iostream>
#include <memory>
#include <map>
#include <tuple>
#include <stdexcept>

using jam2::application::ControlConnection;

void udpEfficiencyFixtures();
namespace {
void require(bool okay, const char* detail) { if (!okay) throw std::runtime_error(detail); }
template<class Condition> void until(Condition condition)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(condition(), "timed out waiting for native control protocol");
}

// A byte-only fixture: all authentication, framing, sequencing, JSON and binary
// handling under test are the actual ControlClient/ControlServer implementations.
class Bytes final : public ControlConnection, public std::enable_shared_from_this<Bytes> {
public:
    std::weak_ptr<Bytes> remote;
    bool connected = true, paused = false;
    jam2::application::ControlTransport kind = jam2::application::ControlTransport::Tcp;
    jam2::application::ControlTransport transport() const override { return kind; }
    QPointer<QObject> context;
    DataHandler data;
    DisconnectHandler disconnected;
    std::deque<QByteArray> input;
    qint64 buffered = 0, highWater = 0;
    void drain()
    {
        if (!context || !data || !connected || paused) return;
        const std::weak_ptr<Bytes> weak = shared_from_this();
        QMetaObject::invokeMethod(context, [weak] {
            const auto self = weak.lock();
            if (!self || !self->connected || self->paused) return;
            while (!self->input.empty() && self->data) {
                const QByteArray bytes = std::move(self->input.front());
                self->input.pop_front(); self->buffered -= bytes.size();
                self->data(bytes);
            }
        }, Qt::QueuedConnection);
    }
    void start(QObject* owner, DataHandler onData, DisconnectHandler onDisconnected) override
    {
        context = owner; data = std::move(onData); disconnected = std::move(onDisconnected); drain();
    }
    bool write(const QByteArray& bytes, bool closeAfterWrite = false) override
    {
        const auto other = remote.lock();
        if (!connected || !other || !other->connected ||
            other->buffered + bytes.size() > 256 * 1024) return false;
        // Split framing boundaries without interpreting any protocol fields.
        for (qsizetype offset = 0; offset < bytes.size(); offset += 503) {
            other->input.push_back(bytes.mid(offset, 503));
        }
        other->buffered += bytes.size();
        other->highWater = std::max(other->highWater, other->buffered);
        other->drain();
        if (closeAfterWrite) {
            const std::weak_ptr<Bytes> weak = shared_from_this();
            QMetaObject::invokeMethod(context, [weak] { if (const auto self = weak.lock()) self->close(); },
                Qt::QueuedConnection);
        }
        return true;
    }
    void close() override
    {
        if (!connected) return;
        connected = false;
        if (const auto other = remote.lock(); other && other->connected) {
            other->connected = false;
            const auto callback = other->disconnected;
            if (other->context && callback) {
                QMetaObject::invokeMethod(other->context, [callback] { callback(QStringLiteral("byte link closed")); },
                    Qt::QueuedConnection);
            }
        }
        data = {}; disconnected = {}; input.clear(); buffered = 0;
    }
    bool isConnected() const override { return connected; }
    qint64 bytesToWrite() const override { return 0; }
    qint64 maxPendingReadBytes() const override { return highWater; }
    QString peerHost() const override { return QStringLiteral("127.0.0.1"); }
};

auto endpoints()
{
    auto first = std::make_shared<Bytes>(), second = std::make_shared<Bytes>();
    first->remote = second; second->remote = first;
    return std::make_pair(first, second);
}


void framingProgressDeadline()
{
    using namespace jam2::application;
    ControlFrameDeadline deadline;
    deadline.received(0);
    require(deadline.remaining(ControlTransport::ReliableUdp, 9999) == 1, "UDP idle bound is not ten seconds");
    deadline.received(9000);
    require(deadline.remaining(ControlTransport::ReliableUdp, 10000) == 9000, "new bytes did not extend UDP idle deadline");
    require(deadline.remaining(ControlTransport::Tcp, 5000) == 0, "legacy TCP absolute bound changed");
    deadline.received(200000);
    require(deadline.remaining(ControlTransport::ReliableUdp, 200001) == 9999, "progressing message acquired an absolute UDP timeout");
    require(deadline.remaining(ControlTransport::ReliableUdp, 210000) == 0, "stalled message has no idle deadline");

    const QString session = "0123456789abcdef", key = "0123456789abcdef0123456789abcdef";
    ControlServer server;
    ControlClient client;
    auto [toServer, toClient] = endpoints();
    toServer->kind = toClient->kind = ControlTransport::ReliableUdp;
    require(server.listenViaTransport(session, key), "configure progress deadline test");
    server.acceptTransport(toServer);
    client.connectViaTransport(toClient, session, key, QString(32, '2'), "127.0.0.1:50000");
    until([&] { return client.isConnected() && server.hasPeer(); });
    toServer->paused = toClient->paused = true;
    QJsonObject fromClient, fromServer;
    server.onMessage = [&](const QString&, const QJsonObject& message) { fromClient = message; };
    client.onMessage = [&](const QJsonObject& message) { fromServer = message; };
    const QJsonObject message{{"type", "session.error"}, {"message", QString(4000, 'x')}};
    require(client.send(message) && server.sendTo(QString(32, '2'), message), "queue delayed frames");
    auto release = [](const std::shared_ptr<Bytes>& bytes, bool all) {
        do {
            if (bytes->input.empty()) break;
            auto part = bytes->input.front(); bytes->input.pop_front();
            bytes->buffered -= part.size(); bytes->data(part);
        } while (all);
    };
    auto wait = [](int ms) {
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(2);
        }
    };
    release(toServer, false); release(toClient, false);
    wait(4100);
    release(toServer, false); release(toClient, false);
    wait(4100);
    require(client.isConnected() && server.hasPeer() &&
        server.stats().frameTimeouts == 0 && client.stats().frameTimeouts == 0,
        "progressing frames were disconnected after five seconds");
    release(toServer, true); release(toClient, true);
    until([&] { return fromClient == message && fromServer == message; });
    // Drain the new application dispatch receipts before deliberately releasing
    // only the prefix of the next frame. A complete receipt is not a stalled
    // application record and must not start its incomplete-frame deadline.
    release(toServer, true); release(toClient, true);
    require(client.send(message), "queue intentionally stalled frame");
    release(toServer, false);
    wait(9300);
    require(server.hasPeer(), "UDP frame expired before ten seconds without progress");
    wait(900);
    require(server.stats().frameTimeouts == 1, "ten-second idle frame was not expired");
    client.close(); server.close();
}

class OrderedOnly final : public ControlConnection {
public:
    explicit OrderedOnly(std::shared_ptr<jam2::application::UdpStreamConnection> connection) : connection_(std::move(connection)) {}
    void start(QObject* owner, DataHandler data, DisconnectHandler closed) override { connection_->start(owner,std::move(data),std::move(closed)); }
    bool write(const QByteArray& bytes, bool closeAfter = false) override { return connection_->write(bytes,closeAfter); }
    void close() override { connection_->close(); }
    bool isConnected() const override { return connection_->isConnected(); }
    qint64 bytesToWrite() const override { return connection_->bytesToWrite(); }
    qint64 maxPendingReadBytes() const override { return connection_->maxPendingReadBytes(); }
    QString peerHost() const override { return connection_->peerHost(); }
    jam2::application::ControlTransport transport() const override { return jam2::application::ControlTransport::ReliableUdp; }
private:
    std::shared_ptr<jam2::application::UdpStreamConnection> connection_;
};
void authenticatedUdpStreamsUnderLoss(bool independent)
{
    using namespace jam2::application;
    const QString session = QStringLiteral("0123456789abcdef");
    const QString keyText = QStringLiteral("0123456789abcdef0123456789abcdef");
    const QByteArray key = QByteArray::fromHex(keyText.toLatin1());
    const quint64 sessionId = 0x0123456789abcdefULL;
    const quint64 introduction = 912345;
    const QString token = QStringLiteral("22222222222222222222222222222222");
    auto incoming = std::make_shared<UdpStreamConnection>(sessionId, introduction, 123,
        UdpMuxChannel::Control, key, QStringLiteral("127.0.0.1"), true);
    auto outgoing = std::make_shared<UdpStreamConnection>(sessionId, introduction, 123,
        UdpMuxChannel::Control, key, QStringLiteral("127.0.0.1"), false);
    struct Datagram { bool toServer; QByteArray bytes; quint64 due; };
    std::deque<Datagram> network;
    std::map<std::tuple<bool, int, quint64>, int> seen;
    quint64 now = 1;
    quint64 blockPrimaryUntil = 0;
    quint64 primaryDrops = 0;
    auto transmit = [&](bool toServer, const QByteArray& bytes) {
        const auto frame = decodeUdpMux(bytes, sessionId, key);
        require(frame.has_value(), "production UDP stream emitted invalid authentication/framing");
        if (now < blockPrimaryUntil && toServer && (frame->kind == UdpMuxKind::Data || frame->kind==UdpMuxKind::DataAck) && frame->channel == UdpMuxChannel::Control) {
            ++primaryDrops; return true;
        }
        const auto identity = std::make_tuple(toServer, static_cast<int>(frame->kind), frame->sequence);
        const int count = ++seen[identity];
        if (count == 1 && (((frame->kind == UdpMuxKind::Data || frame->kind==UdpMuxKind::DataAck) && frame->sequence % 7 == 2) ||
                          ((frame->kind == UdpMuxKind::Ack || frame->kind == UdpMuxKind::AckWindow || frame->kind==UdpMuxKind::AckPair) && frame->sequence % 5 == 0) ||
                          frame->kind == UdpMuxKind::OpenAck)) return true;
        network.push_back({toServer, bytes, now + (frame->sequence % 3 == 0 ? 1000 : 11000)});
        if (count == 1 && frame->kind == UdpMuxKind::Data && frame->sequence % 11 == 0)
            network.push_back(network.back());
        return true;
    };
    auto step = [&] {
        incoming->poll(now, 4, [&](const QByteArray& bytes) { return transmit(false, bytes); });
        outgoing->poll(now, 4, [&](const QByteArray& bytes) { return transmit(true, bytes); });
        std::deque<Datagram> ready, waiting;
        while (!network.empty()) {
            auto datagram = std::move(network.front()); network.pop_front();
            (datagram.due <= now ? ready : waiting).push_back(std::move(datagram));
        }
        network.swap(waiting);
        for (const auto& datagram : ready) {
            const auto frame = decodeUdpMux(datagram.bytes, sessionId, key);
            require(frame.has_value(), "serialized UDP datagram could not be parsed");
            (datagram.toServer ? incoming : outgoing)->receive(*frame, now,
                [&](const QByteArray& bytes) { return transmit(!datagram.toServer, bytes); });
        }
        now += 10000;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    };
    auto wait = [&](auto condition) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 3000) step();
        if (!condition()) std::cerr << "UDP fixture now=" << now << " queued=" << outgoing->bytesToWrite()
            << " received=" << incoming->stats().received << " retransmissions=" << outgoing->stats().retransmitted
            << " timeouts=" << outgoing->stats().timeouts << " input-deferrals=" << incoming->inputDeferrals()
            << " failure=" << outgoing->failure().toStdString() << std::endl;
        require(condition(), "native UDP control stream did not recover under packet impairment");
    };
    wait([&] { return outgoing->isConnected(); });
    ControlServer server;
    ControlClient client;
    require(server.listenViaTransport(session, keyText), "UDP control fixture did not configure");
    const ControlConnection::Pointer incomingControl = independent ? ControlConnection::Pointer(incoming) : std::make_shared<OrderedOnly>(incoming);
    const ControlConnection::Pointer outgoingControl = independent ? ControlConnection::Pointer(outgoing) : std::make_shared<OrderedOnly>(outgoing);
    server.acceptTransport(incomingControl);
    client.connectViaTransport(outgoingControl, session, keyText, token, QStringLiteral("127.0.0.1:52000"));
    wait([&] { return client.isConnected() && server.hasPeer(); });
    require(server.serverWireDiagnostics().size() == 1, "Server wire diagnostics did not expose its authenticated peer channel");
    // Hold the main ordering for 250 ms of deterministic network time. An
    // independent readiness reply must bypass it; an edit must remain behind it.
    bool ready = false, edited = false;
    server.onMessage = [&](const QString&, const QJsonObject& m) {
        if (m.value("type") == "bank.ready") ready = true;
        if (m.value("type") == "beat.set") edited = true;
    };
    const auto began = now; blockPrimaryUntil = now + 250000;
    require(client.send({{"type","song.set"},{"song",QJsonObject{{"title",QString(8000,'x')}}}}) &&
        client.send({{"type","beat.set"},{"text","after-checkpoint"}}) &&
        client.send({{"type","bank.ready"},{"bank",0},{"switch_id","12345678-1234-1234-1234-123456789abc"}}), "queue independent ordering experiment");
    wait([&] { return ready; });
    const auto replyUs = now - began;
    require(primaryDrops > 0, "checkpoint loss was not exercised");
    if (independent) require(now < blockPrimaryUntil && !edited, "independent reply waited for lost checkpoint, or dependent edit overtook it");
    else require(now >= blockPrimaryUntil && edited, "ordered baseline unexpectedly bypassed lost checkpoint");
    wait([&] { return edited; });
    std::cout << "ordering=" << (independent ? "independent" : "single") << " blocked_primary_us=250000 reply_us=" << replyUs << " dependent_edit_us=" << now - began << " dropped_primary_datagrams=" << primaryDrops << '\n';
    QByteArray content(64000, 'x'), received;
    for (qsizetype i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i % 251);
    server.onBinaryMessage = [&](const QString&, const QByteArray& bytes) { received += bytes; };
    require(client.sendBinary(content), "native authenticated binary write over UDP failed");
    wait([&] { return received.size() == content.size(); });
    require(received == content, "UDP loss/reordering changed or duplicated native binary content");
    wait([&] { return outgoing->bytesToWrite() == 0; });
    received.clear();
    for (int index = 0; index < 4; ++index)
        require(client.sendBinary(content), "UDP adapter failed before the native control high-water mark");
    require(!client.sendBinary(content) && client.isConnected(),
        "native control queue saturation did not apply temporary backpressure");
    wait([&] { return received.size() == content.size() * 4; });
    require(received == content.repeated(4), "bounded native frames changed or reordered under backpressure");
    require(outgoing->stats().retransmitted + incoming->stats().retransmitted > 0,
        "UDP stream recovery was not exercised");
    QByteArray damaged = encodeUdpMux({UdpMuxKind::Data, UdpMuxChannel::Control, sessionId,
        introduction, 123, 99, QByteArrayLiteral("corrupt")}, key);
    damaged[damaged.size() - 1] ^= 1;
    require(!decodeUdpMux(damaged, sessionId, key) &&
        !decodeUdpMux(damaged.first(20), sessionId, key), "invalid/truncated multiplex packet was accepted");
    require(!decodeUdpMux(encodeUdpMux({UdpMuxKind::Ack, UdpMuxChannel::Control, sessionId,
        introduction, 123, 1, {}}, key), sessionId + 1, key),
        "different jam session accepted a multiplex acknowledgement");
    client.close(); server.close();
}

void aggregatedAcknowledgementsAndBorrowedPayload()
{
    using namespace jam2::application;
    const QByteArray key(16,'k');
    auto receiver=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Control,key,"local",true);
    QObject owner; QByteArray delivered;
    receiver->start(&owner,[&](const QByteArray& bytes) { delivered+=bytes; },{});
    QByteArray bytes(1024,'p');
    std::vector<QByteArray> acks;
    const auto emitPacket=[&](const QByteArray& packet) { acks.push_back(packet); return true; };
    for (quint64 sequence=1;sequence<=32;++sequence) {
        auto wire=encodeUdpMux({UdpMuxKind::Data,UdpMuxChannel::Control,1,2,3,sequence,bytes},key);
        const auto frame=decodeUdpMuxView({reinterpret_cast<const std::uint8_t*>(wire.constData()),static_cast<std::size_t>(wire.size())},1,key);
        require(frame && frame->payload.data()==reinterpret_cast<const std::uint8_t*>(wire.constData()+42),"decoder did not borrow authenticated payload");
        receiver->receiveView(*frame,sequence,emitPacket);
        wire.fill('x'); // Asynchronous application delivery must own its bytes.
    }
    require(acks.empty(),"fragment ACKs were emitted before batch flush");
    receiver->poll(33,2,[](const QByteArray&) { return false; });
    require(receiver->stats().ackTransmitDeferrals==1,"would-block lost pending ACK state");
    receiver->poll(34,2,emitPacket);
    require(acks.size()==1,"contiguous batch emitted more than one ACK");
    const auto ack=decodeUdpMux(acks.front(),1,key);
    require(ack && ack->kind==UdpMuxKind::AckWindow && ack->sequence==32 && ack->payload.isEmpty(),"contiguous ACK wire representation changed");
    require(receiver->stats().acksCoalesced==31 && receiver->stats().in_order_direct==32,"ACK/fast-path counters missing");
    until([&] { return delivered.size()==32*1024; });
    require(delivered==bytes.repeated(32),"borrowed packet storage escaped into asynchronous delivery");
    auto gaps=std::make_shared<UdpStreamConnection>(1,2,4,UdpMuxChannel::Control,key,"local",true);
    acks.clear();
    gaps->receive({UdpMuxKind::Data,UdpMuxChannel::Control,1,2,4,2,bytes},40,emitPacket);
    gaps->receive({UdpMuxKind::Data,UdpMuxChannel::Control,1,2,4,4,bytes},41,emitPacket);
    gaps->receive({UdpMuxKind::Data,UdpMuxChannel::IndependentControl,1,2,4,1,bytes},42,emitPacket);
    gaps->poll(43,2,emitPacket);
    require(acks.size()==1,"ready ordering-domain ACKs were not combined");
    const auto primary=decodeUdpMux(acks[0],1,key);
    require(primary && primary->kind==UdpMuxKind::AckPair && primary->sequence==0 && primary->payload.size()==24 &&
        qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(primary->payload.constData()))==10,
        "selective ACK acknowledged a missing fragment or lost gaps");
    require(qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(primary->payload.constData()+8))==1,
        "independent ACK lost its ordering domain");
    auto corrupt=acks[0]; corrupt[corrupt.size()-1]^=1;
    require(!decodeUdpMux(corrupt,1,key),"corrupt aggregate ACK authenticated");
    auto old=acks[0]; old[4]=1;
    old.chop(16); old+=QMessageAuthenticationCode::hash(old,key,QCryptographicHash::Sha256).first(16);
    require(!decodeUdpMux(old,1,key),"old multiplex version was silently accepted");
    require(!decodeUdpMux(encodeUdpMux({UdpMuxKind::AckWindow,UdpMuxChannel::Control,1,2,4,0,{}},key),1,key) &&
        !decodeUdpMux(encodeUdpMux({UdpMuxKind::AckWindow,UdpMuxChannel::Control,1,2,4,1,QByteArray(2,'x')},key),1,key),
        "empty or malformed aggregate ACK was accepted");
    receiver->close(); gaps->retire();
}

void authenticatedControlAndAssets()
{
    const QString session = QStringLiteral("0123456789abcdef");
    const QString key = QStringLiteral("0123456789abcdef0123456789abcdef");
    const QString token = QStringLiteral("22222222222222222222222222222222");
    ControlServer server;
    ControlClient client, assets;
    require(server.listenViaTransport(session, key), "external transport could not configure control server");
    auto [serverBytes, clientBytes] = endpoints();
    bool authenticated = false;
    server.onAuthenticated = [&](const QString& peer, const QJsonObject&) { authenticated = peer == token; };
    server.acceptTransport(serverBytes);
    client.connectViaTransport(clientBytes, session, key, token, QStringLiteral("127.0.0.1:50000"));
    until([&] { return authenticated && client.isConnected(); });
    require(server.serverWireDiagnostics().isEmpty(), "legacy TCP transport unexpectedly enabled the Server codec");

    QJsonObject received;
    server.onMessage = [&](const QString&, const QJsonObject& value) { received = value; };
    const QJsonObject change{{QStringLiteral("type"), QStringLiteral("test.control")},
                            {QStringLiteral("value"), 42}};
    require(client.send(change), "authenticated control send failed");
    until([&] { return received == change; });

    auto [serverAssetBytes, clientAssetBytes] = endpoints();
    server.acceptTransport(serverAssetBytes);
    assets.connectViaTransport(clientAssetBytes, session, key, token, QString{}, ControlClient::Channel::Asset);
    until([&] { return assets.isConnected(); });
    QByteArray binary(64000, 'w'), delivered;
    for (qsizetype i = 0; i < binary.size(); ++i) binary[i] = static_cast<char>(i % 251);
    server.onAssetBinaryMessage = [&](const QString& peer, const QByteArray& bytes) {
        require(peer == token, "asset changed authenticated peer identity"); delivered += bytes;
    };
    require(assets.sendBinary(binary), "asset frame was rejected by shared transport boundary");
    until([&] { return delivered.size() == binary.size(); });
    require(delivered == binary, "asset bytes changed across stream framing boundaries");
    assets.close();
    require(client.isConnected(), "closing asset stream also closed control");

    received = {};
    require(client.send(change), "control stopped after asset close");
    until([&] { return received == change; });
    client.close(); server.close();

    // Reuse session/key and peer token on a fresh transport; handshake state must reset.
    require(server.listenViaTransport(session, key), "same session/key could not restart");
    auto [restartedServer, restartedClient] = endpoints();
    authenticated = false;
    server.acceptTransport(restartedServer);
    client.connectViaTransport(restartedClient, session, key, token, QStringLiteral("127.0.0.1:50001"));
    until([&] { return authenticated && client.isConnected(); });
    client.close(); server.close();

    require(server.listenViaTransport(session, key), "wrong-key fixture could not configure");
    auto [wrongServer, wrongClient] = endpoints();
    server.acceptTransport(wrongServer);
    client.connectViaTransport(wrongClient, session, QString(32, QLatin1Char('f')), token, QStringLiteral("127.0.0.1:50002"));
    until([&] { return server.stats().authenticationRejects > 0; });
    require(!client.isConnected() && !server.hasPeer(), "wrong key authenticated on alternate transport");
    client.close(); server.close();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        framingProgressDeadline();
        aggregatedAcknowledgementsAndBorrowedPayload();
        udpEfficiencyFixtures();
        authenticatedControlAndAssets();
        authenticatedUdpStreamsUnderLoss(false);
        authenticatedUdpStreamsUnderLoss(true);
        std::cout << "native control/asset transport boundary checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
