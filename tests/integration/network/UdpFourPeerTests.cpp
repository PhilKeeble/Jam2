#include "UdpMultiplexTransport.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "ControlProtocol.hpp"
#include "protocol.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QJsonDocument>
#include <map>
#include <QThread>
#include <QUdpSocket>
#include <QUuid>
#include <QTimer>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace jam2::application;
namespace {
void require(bool okay, const char* detail) { if (!okay) throw std::runtime_error(detail); }
template<class Predicate> void until(Predicate predicate, const char* detail, int timeout = 12000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1);
    }
    require(predicate(), detail);
}
QString nonce() { return QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex()); }
QString peer(int i) { return QString(32, QChar('1' + i)); }
quint64 id(int i) { return *jam2::control_protocol::peerIdFromToken(peer(i)); }
// A UDP endpoint translator used only by native tests. Every production
// candidate still registers with the actual Go server from its own socket.
// The translator can block a whole candidate tuple, as an admission failure,
// without substituting a second implementation of the handshake.
class CandidateNetwork {
    struct Route {
        QUdpSocket socket;
        quint16 local = 0;
        QString peer, request;
        int attempt = 0;
    };
    QUdpSocket ingress_;
    quint16 server_;
    std::map<quint16, std::unique_ptr<Route>> routes_;
    quint64 directCount_ = 0;
    std::set<QString> firstMembership;
public:
    bool blockFirst = true, blockMesh = false, blockLeave = false;
    quint64 leavesDropped = 0;
    quint64 dropped = 0, duplicated = 0;
    explicit CandidateNetwork(quint16 server) : server_(server) {
        require(ingress_.bind(QHostAddress::LocalHost, 0), "bind simulated NAT ingress");
    }
    quint16 port() const { return ingress_.localPort(); }
    void pump() {
        while (ingress_.hasPendingDatagrams()) {
            QHostAddress address; quint16 local = 0;
            QByteArray packet(static_cast<int>(ingress_.pendingDatagramSize()), '\0');
            ingress_.readDatagram(packet.data(), packet.size(), &address, &local);
            auto& route = routes_[local];
            if (!route) {
                route = std::make_unique<Route>(); route->local = local;
                require(route->socket.bind(QHostAddress::LocalHost, 0), "bind simulated NAT mapping");
            }
            const auto object = QJsonDocument::fromJson(packet).object();
            if (blockLeave && object.value("type") == "jam_leave") { ++leavesDropped; continue; }
            const auto kind = object.value("type").toString();
            if (blockFirst && (kind == "jam_admit" || kind == "jam_remove" || kind == "jam_reject") &&
                firstMembership.insert(kind + object.value("to").toString()).second) { ++dropped; continue; }
            if (object.value("type") == "jam_candidate") {
                route->peer = object.value("peer").toString();
                route->request = object.value("nonce").toString();
                route->attempt = object.value("attempt").toInt();
            }
            route->socket.writeDatagram(packet, QHostAddress::LocalHost, server_);
        }
        for (auto& [local, route] : routes_) {
            while (route->socket.hasPendingDatagrams()) {
                QHostAddress source; quint16 from = 0;
                QByteArray packet(static_cast<int>(route->socket.pendingDatagramSize()), '\0');
                route->socket.readDatagram(packet.data(), packet.size(), &source, &from);
                if (from == server_) {
                    const auto message = QJsonDocument::fromJson(packet).object();
                    const auto kind = message.value("type").toString();
                    if (blockFirst && kind.endsWith("_ack") &&
                        firstMembership.insert(kind + message.value("peer").toString()).second) { ++dropped; continue; }
                    ingress_.writeDatagram(packet, QHostAddress::LocalHost, local); continue;
                }
                const auto found = routes_.find(from);
                if (found == routes_.end()) continue;
                auto& sender = found->second;
                if ((blockFirst && (route->attempt == 1 || sender->attempt == 1)) ||
                    (blockMesh && route->peer == peer(1) && sender->peer == peer(2)) ||
                    (blockMesh && route->peer == peer(2) && sender->peer == peer(1))) {
                    ++dropped; continue;
                }
                ++directCount_;
                if (packet.startsWith("J2MX") && directCount_ % 19 == 0) { ++dropped; continue; }
                sender->socket.writeDatagram(packet, QHostAddress::LocalHost, local);
                if (packet.startsWith("J2MX") && directCount_ % 23 == 0) {
                    sender->socket.writeDatagram(packet, QHostAddress::LocalHost, local); ++duplicated;
                }
            }
        }
    }
};

struct Server {
    QProcess process;
    quint16 port;
    Server(const QString& executable) {
        QUdpSocket reserve; require(reserve.bind(QHostAddress::LocalHost, 0), "reserve server port");
        port = reserve.localPort(); reserve.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, {"--jam2-server", "--listen", QStringLiteral("127.0.0.1:%1").arg(port)});
        require(process.waitForStarted(5000), "start real Go server");
    }
    ~Server() { process.kill(); process.waitForFinished(5000); }
};
int reusableInvitationRestart(const QString& executable) {
    Server server(executable);
    CandidateNetwork network(server.port);
    network.blockFirst = false; network.blockLeave = true;
    QTimer pump; QObject::connect(&pump, &QTimer::timeout, [&] { network.pump(); }); pump.start(1);
    for (int iteration = 0; iteration < 2; ++iteration) {
        std::array<std::unique_ptr<UdpMultiplexTransport>, 2> peers;
        for (auto& value : peers) value = std::make_unique<UdpMultiplexTransport>();
        peers[0]->onJoinRequest = [&](const QString& token) { peers[0]->introduce(peer(0), token, nonce()); };
        for (int index = 0; index < 2; ++index) {
            UdpMultiplexTransport::Config config;
            config.creator = index == 0; config.localToken = peer(index);
            config.sessionHex = "0123456789abcdef"; config.keyHex = "0123456789abcdef0123456789abcdef";
            config.server = {"127.0.0.1", network.port()}; config.bindHost = "127.0.0.1";
            QString error; require(peers[index]->start(config, error), "start reusable invitation peer");
            if (index == 0) {
                until([&] { return peers[0]->snapshot().registered; }, "same invitation registered before old lease expiry");
                require(peers[0]->snapshot().links.empty(), "restart restored an old physical link");
            }
        }
        until([&] { return peers[0]->peerEndpoint(id(1)) && peers[1]->peerEndpoint(id(0)); },
            "same session/key created fresh native connection");
        require(peers[0]->snapshot().links.front().attempt == 1, "restart reused failed attempt state");
        peers[1]->stop(); peers[0]->stop();
        network.pump();
    }
    require(network.leavesDropped >= 1, "restart fixture did not retain old server record");
    std::cout << "Reusable invitation native restart passed with old directory lease retained\n";
    return 0;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc >= 2, "native rendezvous test requires the Go server executable");
        if (argc >= 3 && QString::fromLocal8Bit(argv[2]) == "restart")
            return reusableInvitationRestart(QString::fromLocal8Bit(argv[1]));
        const bool impaired = argc >= 3;
        const bool partial = argc >= 3 && QString::fromLocal8Bit(argv[2]) == "partial";
        Server server(QString::fromLocal8Bit(argv[1]));
        CandidateNetwork network(server.port);
        network.blockMesh = partial;
        QTimer pump;
        QObject::connect(&pump, &QTimer::timeout, &app, [&] { network.pump(); });
        if (impaired) pump.start(1);
        const QString session = "0123456789abcdef", key = "0123456789abcdef0123456789abcdef";
        std::array<std::unique_ptr<UdpMultiplexTransport>, 4> peers;
        std::array<std::unique_ptr<ControlClient>, 4> control, asset;
        ControlServer coordinator;
        require(coordinator.listenViaTransport(session, key), "configure actual control protocol");
        std::set<QString> authenticated;
        std::array<bool, 4> started{};
        int warnings = 0;
        QByteArray receivedAsset;
        coordinator.onAuthenticated = [&](const QString& token, const QJsonObject&) {
            if (!authenticated.insert(token).second) return;
            const int newcomer = token[0].unicode() - '1';
            for (int index = 1; index < 4; ++index) {
                if (index == newcomer || !authenticated.contains(peer(index))) continue;
                const auto request = nonce();
                peers[index]->prepareIntroduction(request);
                peers[newcomer]->prepareIntroduction(request);
                peers[0]->introduce(peer(index), token, request);
            }
            peers[0]->admitPeer(token);
        };
        coordinator.onAssetBinaryMessage = [&](const QString&, const QByteArray& value) { receivedAsset += value; };
        for (int index = 0; index < 4; ++index) {
            peers[index] = std::make_unique<UdpMultiplexTransport>();
            control[index] = std::make_unique<ControlClient>();
            asset[index] = std::make_unique<ControlClient>();
            peers[index]->onSignalerStatus = [index](const QString& value) {
                std::cout << index << ": " << value.toStdString() << std::endl;
            };
            peers[index]->onPairFailure = [&](const QString&, const QString&, const QString& detail) {
                ++warnings; std::cerr << detail.toStdString() << std::endl;
            };
            peers[index]->onLinkReady = [&, index](const QString& token) {
                if (index != 0 && token == peer(0)) peers[index]->openStream(token, UdpMuxChannel::Control);
            };
            peers[index]->onStreamReady = [&, index](const QString& token, UdpMuxChannel channel,
                    std::shared_ptr<ControlConnection> connection, bool incoming) {
                if (index == 0 && incoming) { coordinator.acceptTransport(connection); return; }
                require(token == peer(0) && !incoming, "logical stream only on creator links");
                const auto observed = peers[index]->observedEndpoint(token);
                require(observed.has_value(), "Go server returned own public candidate");
                auto& client = channel == UdpMuxChannel::Asset ? asset[index] : control[index];
                client->connectViaTransport(connection, session, key, peer(index),
                    QString::fromStdString(jam2::endpoint_to_string(*observed)),
                    channel == UdpMuxChannel::Asset ? ControlClient::Channel::Asset : ControlClient::Channel::Control);
            };
        }
        peers[0]->onJoinRequest = [&](const QString& token) { peers[0]->introduce(peer(0), token, nonce()); };
        auto start = [&](int index) {
            UdpMultiplexTransport::Config config;
            config.creator = index == 0; config.localToken = peer(index);
            config.sessionHex = session; config.keyHex = key;
            config.server = {"127.0.0.1", impaired ? network.port() : server.port}; config.bindHost = "127.0.0.1";
            config.maximumPeers = 3;
            QString error;
            require(peers[index]->start(config, error), error.toUtf8().constData()); started[index] = true;
        };
        start(0);
        until([&] { return peers[0]->snapshot().registered; }, "creator registration");
        start(1);
        until([&] { return control[1]->isConnected(); }, "first authenticated UDP control link");
        const auto original = peers[0]->snapshot().links.front().local.port;
        auto audio = [&] {
            for (int from = 0; from < 4; ++from) {
                if (!started[from]) continue;
                for (int to = 0; to < 4; ++to) {
                    if (to == from || !started[to] || !peers[from]->peerEndpoint(id(to))) continue;
                    std::array<std::uint8_t, 80> packet{};
                    std::copy_n("JAM2", 4, packet.begin()); packet[4] = static_cast<std::uint8_t>(from);
                    packet[5] = static_cast<std::uint8_t>(to);
                    require(peers[from]->sendToPeer(id(to), {}, packet).outcome == jam2::UdpSendOutcome::Sent,
                        "send raw audio on existing selected socket");
                }
            }
            for (int to = 0; to < 4; ++to) {
                if (!started[to]) continue;
                std::array<std::uint8_t, 1200> data{};
                while (auto received = peers[to]->receiveFor(data, 0, nullptr)) {
                    require(received->size == 80 && data[5] == to, "audio payload changed or routed to wrong peer");
                }
            }
        };
        QElapsedTimer ticker; ticker.start();
        start(2);
        until([&] { audio(); return control[2]->isConnected() && (partial || peers[1]->peerEndpoint(id(2)).has_value()); },
            "third peer joined full mesh while first edge streams");
        peers[1]->openStream(peer(0), UdpMuxChannel::Asset);
        until([&] { audio(); return asset[1]->isConnected(); }, "asset stream authentication");
        QByteArray content(64000, '\0');
        for (int i = 0; i < content.size(); ++i) content[i] = static_cast<char>((i * 37) % 251);
        require(asset[1]->sendBinary(content), "queue actual authenticated binary asset");
        start(3);
        until([&] {
            audio();
            if (authenticated.size() != 3 || receivedAsset != content) return false;
            for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j)
                if (partial && i == 1 && j == 2) continue;
                else if (!peers[i]->peerEndpoint(id(j)) || !peers[j]->peerEndpoint(id(i))) return false;
            return peers[0]->snapshot().admissionPaused && peers[0]->snapshot().pauseConfirmed;
        }, "exactly four peers, UDP mesh, concurrent asset, capacity pause", partial ? 45000 : 20000);
        if (partial) until([&] { return warnings == 3; }, "one failure notification at both endpoints and creator");
        require(warnings == (partial ? 3 : 0), "unexpected connection failure count");
        if (impaired) {
            require(network.dropped > 0, "candidate tuple impairment was not exercised");
            for (int i = 0; i < 4; ++i) for (const auto& link : peers[i]->snapshot().links) {
                require(link.attempt >= 2, "blocked first candidate was reused or falsely connected");
                if (partial && ((i == 1 && link.peer == peer(2)) || (i == 2 && link.peer == peer(1))))
                    require(link.attempt == 10 && link.phase == UdpMultiplexTransport::LinkPhase::Failed,
                        "partial edge did not exhaust exactly ten fresh pairs");
            }
        }
        require(peers[0]->snapshot().links.front().local.port == original, "new joins rotated established audio socket");
        const auto before = peers[0]->snapshot().signalPacketsSent;
        ticker.restart();
        until([&] { audio(); return ticker.elapsed() > 500; }, "audio while server paused");
        require(peers[0]->snapshot().signalPacketsSent == before, "full jam kept sending to rendezvous");
        asset[1]->close();
        until([&] { return !asset[1]->isConnected(); }, "asset close");
        require(control[1]->isConnected() && peers[1]->peerEndpoint(id(0)), "asset close tore down physical/control link");
        peers[0]->removePeer(peer(3));
        until([&] { audio(); return peers[0]->snapshot().registered && !peers[0]->snapshot().admissionPaused; },
            "vacancy resumed registration");
        require(peers[0]->snapshot().links.front().local.port == original, "capacity resume changed existing socket");
        server.process.kill(); server.process.waitForFinished(5000);
        audio();
        require(control[1]->isConnected(), "server outage closed established control");
        for (auto& item : peers) item->stop();
        std::cout << "Four-peer native UDP transport integration passed" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << std::endl; return 1;
    }
}
