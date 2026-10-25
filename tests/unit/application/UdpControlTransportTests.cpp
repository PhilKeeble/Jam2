#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "UdpStreamConnection.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>

#include <deque>
#include <iostream>
#include <memory>
#include <map>
#include <tuple>
#include <stdexcept>

using jam2::application::ControlConnection;

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
    bool connected = true;
    QPointer<QObject> context;
    DataHandler data;
    DisconnectHandler disconnected;
    std::deque<QByteArray> input;
    qint64 buffered = 0, highWater = 0;
    void drain()
    {
        if (!context || !data || !connected) return;
        const std::weak_ptr<Bytes> weak = shared_from_this();
        QMetaObject::invokeMethod(context, [weak] {
            const auto self = weak.lock();
            if (!self || !self->connected) return;
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

void authenticatedUdpStreamsUnderLoss()
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
    auto transmit = [&](bool toServer, const QByteArray& bytes) {
        const auto frame = decodeUdpMux(bytes, sessionId, key);
        require(frame.has_value(), "production UDP stream emitted invalid authentication/framing");
        const auto identity = std::make_tuple(toServer, static_cast<int>(frame->kind), frame->sequence);
        const int count = ++seen[identity];
        if (count == 1 && ((frame->kind == UdpMuxKind::Data && frame->sequence % 7 == 2) ||
                          (frame->kind == UdpMuxKind::Ack && frame->sequence % 5 == 0) ||
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
    server.acceptTransport(incoming);
    client.connectViaTransport(outgoing, session, keyText, token, QStringLiteral("127.0.0.1:52000"));
    wait([&] { return client.isConnected() && server.hasPeer(); });
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
        authenticatedControlAndAssets();
        authenticatedUdpStreamsUnderLoss();
        std::cout << "native control/asset transport boundary checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
