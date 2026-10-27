#include "../../support/UdpCandidateNetwork.hpp"
#include "UdpMultiplexTransport.hpp"
#include "AssetChunkProtocol.hpp"
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
struct Server {
    QProcess process;
    quint16 port;
    Server(const QString& executable, bool allowLocal = false) {
        QUdpSocket reserve; require(reserve.bind(QHostAddress::LocalHost, 0), "reserve server port");
        port = reserve.localPort(); reserve.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        QStringList arguments{"--jam2-server", "--listen", QStringLiteral("127.0.0.1:%1").arg(port)};
        if (allowLocal) arguments << "--allow-local";
        process.start(executable, arguments);
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
        const bool pressure = argc >= 3 && QString::fromLocal8Bit(argv[2]) == "receive-pressure";
        const bool impaired = argc >= 3 && !pressure;
        const bool partial = argc >= 3 && QString::fromLocal8Bit(argv[2]) == "partial";
        const bool local = argc >= 3 && QString::fromLocal8Bit(argv[2]) == "local";
        const bool localFallback = argc >= 3 && QString::fromLocal8Bit(argv[2]) == "local-fallback";
        Server server(QString::fromLocal8Bit(argv[1]), local || localFallback);
        CandidateNetwork network(server.port);
        network.blockMesh = partial; network.invalidLan = localFallback;
        QTimer pump;
        QObject::connect(&pump, &QTimer::timeout, &app, [&] { network.pump(); });
        if (impaired) pump.start(1);
        const QString session = "0123456789abcdef", key = "0123456789abcdef0123456789abcdef";
        std::array<std::unique_ptr<UdpMultiplexTransport>, 4> peers;
        std::array<std::unique_ptr<ControlClient>, 4> control, asset;
        std::array<ControlConnection::Pointer, 4> controlStreams;
        std::array<int, 4> controlOpens{}, assetOpens{};
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
        coordinator.onAssetBinaryMessage = [&](const QString&, const QByteArray& value) {
            jam2::application::asset_chunk::Chunk chunk; QString error;
            require(jam2::application::asset_chunk::decode(value, chunk, error), "compact WAV did not reconstruct a valid application chunk");
            receivedAsset += chunk.data;
        };
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
                if (channel == UdpMuxChannel::Control) { controlStreams[index] = connection; ++controlOpens[index]; }
                else ++assetOpens[index];
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
                std::array<std::uint8_t, jam2::NetworkDatagramTransport::kReceiveBufferBytes> data{};
                while (auto received = peers[to]->receiveFor(data, 0, nullptr)) {
                    require(received->size == 80 && data[5] == to, "audio payload changed or routed to wrong peer");
                }
            }
        };
        QElapsedTimer ticker; ticker.start();
        peers[0]->receiverAttached(); peers[1]->receiverAttached();
        start(2);
        peers[2]->receiverAttached();
        until([&] { audio(); return control[2]->isConnected() && (partial || peers[1]->peerEndpoint(id(2)).has_value()); },
            "third peer joined full mesh while first edge streams");
        for (int repeat = 0; repeat < 8; ++repeat) peers[1]->openStream(peer(0), UdpMuxChannel::Asset);
        until([&] { audio(); peers[1]->openStream(peer(0), UdpMuxChannel::Asset);
            return asset[1]->isConnected(); }, "asset stream authentication despite repeated open requests");
        require(assetOpens[1] == 1, "repeated ensure-open requests replaced an opening stream");
        QByteArray content(64000, '\0');
        for (int i = 0; i < content.size(); ++i) content[i] = static_cast<char>((i * 37) % 251);
        const auto sendContent = [&] {
            const QString hash(64, QLatin1Char('a'));
            require(asset[1]->send({{"type", "looper.asset.start"}, {"sha256", hash},
                {"file_bytes", content.size()}, {"chunk_size", 24576}}), "queue typed WAV transfer start");
            for (qsizetype offset = 0; offset < content.size(); offset += 24576) {
                const auto payload = jam2::application::asset_chunk::encode({hash,
                    static_cast<quint32>(offset / 24576), static_cast<quint64>(offset), content.mid(offset, 24576)});
                require(!payload.isEmpty() && asset[1]->sendBinary(payload), "queue actual authenticated WAV chunk");
            }
        };
        sendContent();
        start(3);
        peers[3]->receiverAttached();
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
                if (local) {
                    require(link.attempt == 1 && link.route == "lan", "LAN media did not bypass the blocked public mapping");
                    const int remote = link.peer[0].unicode() - '1';
                    for (const auto& reverse : peers[remote]->snapshot().links) if (reverse.peer == peer(i))
                        require(link.remote.port == reverse.local.port, "LAN offer used a port other than the real media socket");
                } else {
                    require(link.attempt >= 2, "blocked first candidate was reused or falsely connected");
                    require(link.route == "public", "failed LAN path did not fall back to public candidates");
                }
                if (partial && ((i == 1 && link.peer == peer(2)) || (i == 2 && link.peer == peer(1))))
                    require(link.attempt == 10 && link.phase == UdpMultiplexTransport::LinkPhase::Failed,
                        "partial edge did not exhaust exactly ten fresh pairs");
            }
        }
        require(peers[0]->snapshot().links.front().local.port == original, "new joins rotated established audio socket");
        for (int index=0; index<4; ++index) {
            require(peers[index]->snapshot().runtimeReceiverAttached && peers[index]->snapshot().receiverHandovers == 1,
                "four-peer runtime socket-reader ownership did not transfer");
        }
        bool duplicateAttachRejected = false;
        try { peers[0]->receiverAttached(); } catch (const std::exception&) { duplicateAttachRejected = true; }
        require(duplicateAttachRejected, "a second socket reader was allowed to attach");
        bool smallBufferRejected = false;
        try { std::array<std::uint8_t,804> buffer{}; (void)peers[0]->receiveFor(buffer,0,nullptr); }
        catch (const std::invalid_argument&) { smallBufferRejected = true; }
        require(smallBufferRejected,"audio-sized buffer was allowed to silently discard full multiplex fragments");
        // Exercise the new control paths through all three authenticated star
        // edges while the same four-peer mesh carries the existing fake audio.
        std::array<int, 4> edits{}, readiness{};
        coordinator.onMessage = [&](const QString& token, const QJsonObject& message) {
            const auto index = token[0].unicode() - '1';
            if (message.value("type") == "beat.set") {
                ++edits[index]; require(message.value("text") == "31", "unsent scalar assignment did not converge to latest");
                coordinator.send(message);
            }
            if (message.value("type") == "bank.ready") ++readiness[index];
        };
        for (int index = 1; index < 4; ++index) {
            for (int change = 0; change < 32; ++change)
                require(control[index]->send({{"type","beat.set"},{"section",0},{"lane","chord"},{"beat",index},{"text",QString::number(change)}}), "four-peer scalar burst");
            require(control[index]->send({{"type","bank.ready"},{"bank",0},{"switch_id","12345678-1234-1234-1234-123456789abc"}}), "four-peer independent readiness");
        }
        until([&] { audio(); return edits[1] == 1 && edits[2] == 1 && edits[3] == 1 &&
            readiness[1] == 1 && readiness[2] == 1 && readiness[3] == 1; }, "four-peer coalesced edits and independent replies");
        require(coordinator.stats().serverWire.independentRecordsReceived >= 3, "independent ordering was not used on four-peer star");
        if (pressure) {
            const auto beforeNoise = peers[0]->snapshot();
            std::array<std::uint8_t,80> invalid{};
            for (int packet=0; packet<128; ++packet)
                require(peers[1]->sendToPeer(id(0),{},invalid).outcome==jam2::UdpSendOutcome::Sent,"send bounded classification flood");
            network.pump();
            until([&] { std::array<std::uint8_t,jam2::NetworkDatagramTransport::kReceiveBufferBytes> buffer{}; (void)peers[0]->receiveFor(buffer,0,nullptr);
                return peers[0]->snapshot().runtimeInvalidPackets>=beforeNoise.runtimeInvalidPackets+128; },"invalid prefix flood was not categorized");
            require(peers[0]->snapshot().runtimeBudgetYields>beforeNoise.runtimeBudgetYields,"classification flood bypassed per-call work limit");
            std::array<int,4> heartbeats{};
            coordinator.onMessage = [&](const QString& token, const QJsonObject& message) {
                if (message.value("type") == "session.heartbeat") ++heartbeats[token[0].unicode()-'1'];
            };
            for (int round=0; round<12; ++round) {
                for (int index=1; index<4; ++index) for (int update=0; update<32; ++update)
                    require(control[index]->send({{"type","session.heartbeat"},{"sequence",round*32+update}}), "control pressure lost bounded enqueue");
                sendContent();
                const auto expected = content.repeated(round+2);
                until([&] { audio(); return receivedAsset == expected && heartbeats[1] == (round+1)*32 &&
                    heartbeats[2] == (round+1)*32 && heartbeats[3] == (round+1)*32; }, "native audio/control/WAV categorization under pressure");
            }
            const auto stats = peers[0]->snapshot();
            require(stats.runtimeNativeDelivered > 0 && stats.runtimeControlQueued > 0 && stats.runtimeAssetQueued > 0 &&
                stats.ingressHighWater <= 2*UdpMultiplexTransport::kIngressPacketsPerQueue && stats.classificationSamples > 0,
                "pressure test did not exercise bounded native/control/WAV processing");
            std::cout << "receive_model=runtime native_delivered=" << stats.runtimeNativeDelivered << " control_queued=" << stats.runtimeControlQueued
                << " asset_queued=" << stats.runtimeAssetQueued << " ingress_drops=" << stats.ingressQueueDrops
                << " ingress_high_water=" << stats.ingressHighWater << " ingress_wait_max_us=" << stats.ingressWaitMaxUs
                << " classification_mean_ns=" << stats.classificationSumNs/stats.classificationSamples << " classification_max_ns=" << stats.classificationMaxNs << '\n';
            receivedAsset = content; // The remaining reuse checks start at one completed file.
            coordinator.onMessage = {};
        }
        const auto before = peers[0]->snapshot().signalPacketsSent;
        ticker.restart();
        until([&] { audio(); return ticker.elapsed() > 500; }, "audio while server paused");
        require(peers[0]->snapshot().signalPacketsSent == before, "full jam kept sending to rendezvous");
        // Repeated sharing must reuse a live stream, then reopen a deliberately
        // closed logical stream without rotating the proven media socket.
        for (int round = 0; round < 3; ++round) {
            for (int repeat = 0; repeat < 8; ++repeat) peers[1]->openStream(peer(0), UdpMuxChannel::Asset);
            const auto expected = content.repeated(round + 2);
            sendContent();
            until([&] { audio(); return receivedAsset == expected; }, "repeated asset byte integrity");
            require(assetOpens[1] == 1 && control[1]->isConnected(), "repeat sharing closed logical control or asset stream");
        }
        asset[1]->close();
        until([&] { audio(); return coordinator.stats().assetActiveConnections == 0; }, "remote asset close");
        peers[1]->openStream(peer(0), UdpMuxChannel::Asset);
        peers[1]->openStream(peer(0), UdpMuxChannel::Asset);
        until([&] { audio(); return asset[1]->isConnected(); }, "asset reopen on same socket");
        require(assetOpens[1] == 2, "closed asset stream was not reopened once");
        require(control[1]->isConnected() && peers[1]->peerEndpoint(id(0)), "asset close tore down physical/control link");
        for (auto& transport : peers) transport->receiverDetached();
        until([&] { return std::all_of(peers.begin(),peers.end(),[](const auto& transport) { return !transport->snapshot().runtimeReceiverAttached; }); },
            "socket ownership returned to bootstrap worker");
        int workerOnly = 0;
        coordinator.onMessage = [&](const QString&, const QJsonObject& message) { if (message.value("type") == "session.heartbeat") ++workerOnly; };
        for (int index=1; index<4; ++index) require(control[index]->send({{"type","session.heartbeat"},{"sequence",12345}}),"worker-only heartbeat enqueue");
        until([&] { return workerOnly == 3; },"control stalled after runtime detached");
        std::array<quint64,4> deliveredBefore{};
        for (int index=0; index<4; ++index) { deliveredBefore[index]=peers[index]->snapshot().runtimeNativeDelivered; peers[index]->receiverAttached(); }
        until([&] { audio(); for (int index=0; index<4; ++index) if (peers[index]->snapshot().runtimeNativeDelivered<=deliveredBefore[index]) return false; return true; },
            "reattached readers did not resume native media delivery on all four peers");
        // Simulate loss of the old logical stream without its CLOSE reaching
        // the creator. A fresh logical stream must authenticate on the exact
        // same established physical pair, including when the old owner lives.
        const auto stable = peers[1]->snapshot().links.front().local.port;
        std::dynamic_pointer_cast<UdpStreamConnection>(controlStreams[1])->retire();
        until([&] { return !control[1]->isConnected(); }, "old logical stream is closing");
        peers[1]->openStream(peer(0), UdpMuxChannel::Control);
        until([&] { audio(); return controlOpens[1] == 2 && control[1]->isConnected(); },
            "control recovery authenticated over retained physical socket");
        require(peers[1]->snapshot().links.front().local.port == stable &&
            peers[0]->snapshot().links.front().local.port == original,
            "logical recovery rotated a proven media socket");
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
