#include "UdpMultiplexTransport.hpp"
#include "ControlProtocol.hpp"
#include "protocol.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace jam2::application {
namespace {
using Phase = UdpMultiplexTransport::LinkPhase;
quint64 number(const QJsonObject& object, const char* field)
{
    const auto value = object.value(QLatin1String(field)).toInteger(-1);
    return value < 0 ? 0 : static_cast<quint64>(value);
}
QString text(const QJsonObject& object, const char* field) { return object.value(QLatin1String(field)).toString(); }
bool token(const QString& value) { return control_protocol::decodeHex(value, 16).size() == 16; }
quint64 peerId(const QString& value) { return control_protocol::peerIdFromToken(value).value_or(0); }
QByteArray bytes(std::span<const std::uint8_t> value)
{
    return QByteArray(reinterpret_cast<const char*>(value.data()), static_cast<qsizetype>(value.size()));
}
std::span<const std::uint8_t> view(const QByteArray& value)
{
    return {reinterpret_cast<const std::uint8_t*>(value.constData()), static_cast<std::size_t>(value.size())};
}
}

struct UdpMultiplexTransport::Impl {
    struct Wake {
        jam2::RealtimeWakeSignal signal;
        Wake() { signal.requestWake(); }
        void notify() { signal.signal(jam2::monotonic_us()); }
    };
    struct Link {
        QString peer, request;
        QString route = QStringLiteral("public");
        Phase phase = Phase::Preparing;
        std::shared_ptr<jam2::UdpSocket> socket;
        jam2::ResolvedUdpEndpoint remote{}, observed{};
        quint64 introduction = 0;
        int attempt = 0, helloSent = 0;
        bool gotHello = false, gotAck = false, report = false;
        quint64 deadline = 0, nextAction = 0, lastReceive = 0;
        bool stalledNotified = false, mediaStarted = false;
        std::set<quint64> challenges;
        std::map<UdpMuxChannel, std::shared_ptr<UdpStreamConnection>> streams;
        std::set<quint64> retiredStreams, notifiedStreams;
        std::set<UdpMuxChannel> wantedStreams;
    };
    struct PairRequest { QString a, b, nonce; quint64 nextSend = 0, deadline = 0; };
    enum class CommandKind { Introduce, RegisterMember, Admit, Reject, Remove, Open, Recover, PrepareRecovery };
    struct Command { CommandKind kind; QString a, b, request; UdpMuxChannel channel = UdpMuxChannel::Control; };
    struct MembershipAction { quint64 nextSend = 0, deadline = 0, sequence = 0; };
    struct Audio {
        jam2::ResolvedUdpEndpoint endpoint;
        std::size_t size = 0;
        std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> bytes{};
    };

    jam2::NetworkRuntime network;
    Config config;
    UdpMultiplexTransport* owner;
    std::shared_ptr<int> callbackLifetime = std::make_shared<int>(0);
    QByteArray key;
    std::array<std::uint8_t, 16> nativeKey{};
    quint64 session = 0;
    jam2::ResolvedUdpEndpoint server{};
    std::shared_ptr<jam2::UdpSocket> signalSocket;
    std::shared_ptr<Wake> commandWake = std::make_shared<Wake>();
    jam2::RealtimeWakeSignal audioWake;
    mutable std::mutex mutex, audioMutex;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::deque<Command> commands;
    std::map<std::pair<CommandKind, QString>, MembershipAction> membershipActions;
    std::map<QString, std::shared_ptr<Link>> links, retainedLinks;
    std::map<QString, PairRequest> requests;
    std::set<QString> admitted, pendingJoins, terminalRequests;
    std::deque<QString> terminalOrder;
    std::set<std::uint16_t> usedPorts;
    std::array<Audio, 512> audioQueue{};
    std::size_t audioRead = 0, audioWrite = 0, audioCount = 0, cursor = 0;
    QString coordinator, registrationChallenge;
    std::string localHost;
    bool registered = false, paused = false, joining = false, registeringMember = false, warningPosted = false;
    bool recoveryJoin = false;
    std::set<QString> repairing;
    quint64 nextMembershipSequence = 0;
    quint64 directoryRevision = 1, nextPause = 0, pauseDeadline = 0;
    bool pauseConfirmed = false;
    quint64 nextRegister = 0, nextJoin = 0, nextMember = 0, memberDeadline = 0, joinDeadline = 0, lastBudget = 0;
    double controlBudget = 4096, assetBudget = 4096;
    Snapshot counters;
    std::atomic<quint64> audioDrops{0};

    Impl(UdpMultiplexTransport* parent, const Config& initial) : config(initial), owner(parent)
    {
        key = control_protocol::decodeHex(config.keyHex, 16);
        const QByteArray sid = control_protocol::decodeHex(config.sessionHex, 8);
        if (key.size() != 16 || sid.size() != 8 || !token(config.localToken) || peerId(config.localToken) == 0 ||
            config.maximumPeers < 0 || config.controlBytesPerSecond < 1024 || config.assetBytesPerSecond < 1024 || config.linkRepairSilenceMs < 1000)
            throw std::runtime_error("Invalid UDP multiplex configuration");
        for (char byte : sid) session = (session << 8) | static_cast<unsigned char>(byte);
        std::copy(key.begin(), key.end(), nativeKey.begin());
        config.sessionHex = config.sessionHex.toLower(); config.keyHex = config.keyHex.toLower();
        config.localToken = config.localToken.toLower();
        server = jam2::resolve_udp_endpoint(config.server);
        signalSocket = std::make_shared<jam2::UdpSocket>();
        signalSocket->bind({config.bindHost.toStdString(), 0});
        usedPorts.insert(signalSocket->local_endpoint().port);
        try { localHost = signalSocket->local_endpoint_for(server).host; }
        catch (const std::exception& error) { log(QStringLiteral("LAN candidate unavailable: %1").arg(QString::fromUtf8(error.what()))); }
        joining = !config.creator;
        coordinator = config.creator ? config.localToken : QString{};
        audioWake.requestWake();
        joinDeadline = jam2::monotonic_us() + 30000000;
        counters.controlBytesPerSecond = config.controlBytesPerSecond;
        counters.assetBytesPerSecond = config.assetBytesPerSecond;
        counters.linkRepairSilenceMs = config.linkRepairSilenceMs;
    }
    ~Impl() { stop(); }
    void post(std::function<void(UdpMultiplexTransport&)> action)
    {
        const QPointer<UdpMultiplexTransport> guard(owner);
        const std::weak_ptr<int> lifetime = callbackLifetime;
        QMetaObject::invokeMethod(owner, [guard, lifetime, action = std::move(action)] { if (guard && !lifetime.expired()) action(*guard); },
            Qt::QueuedConnection);
    }
    void log(const QString& detail) { post([detail](auto& self) { if (self.onSignalerStatus) self.onSignalerStatus(detail); }); }
    bool enqueue(Command command)
    {
        std::lock_guard lock(mutex);
        if (stopping || commands.size() >= 128) return false;
        commands.push_back(std::move(command)); commandWake->notify(); return true;
    }
    void stop()
    {
        stopping = true; commandWake->notify(); audioWake.signal(jam2::monotonic_us());
        if (worker.joinable()) worker.join();
    }
    QJsonObject fields(const char* kind, const std::shared_ptr<Link>& link = {})
    {
        QJsonObject object{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QLatin1String(kind)},
            {QStringLiteral("session"), config.sessionHex}, {QStringLiteral("key"), config.keyHex},
            {QStringLiteral("peer"), config.localToken}};
        if (config.creator) object[QStringLiteral("directory_revision")] = static_cast<qint64>(directoryRevision);
        if (QString::fromLatin1(kind) == QLatin1String("jam_create") && !registrationChallenge.isEmpty())
            object[QStringLiteral("nonce")] = registrationChallenge;
        if (link) {
            object[QStringLiteral("nonce")] = link->request;
            object[QStringLiteral("attempt")] = link->attempt;
            object[QStringLiteral("introduction")] = static_cast<qint64>(link->introduction);
            if (QString::fromLatin1(kind) == QLatin1String("jam_candidate") && !localHost.empty()) {
                object[QStringLiteral("local_endpoint")] = QJsonObject{
                    {QStringLiteral("host"), QString::fromStdString(localHost)},
                    {QStringLiteral("port"), link->socket->local_endpoint().port}};
            }
        }
        return object;
    }
    void send(const std::shared_ptr<jam2::UdpSocket>& socket, const QJsonObject& object)
    {
        const auto packet = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (socket->send_to(server, view(packet)).outcome == jam2::UdpSendOutcome::Sent) ++counters.signalPacketsSent;
    }
    std::shared_ptr<jam2::UdpSocket> candidate()
    {
        for (int index = 0; index < 100; ++index) {
            auto socket = std::make_shared<jam2::UdpSocket>();
            socket->bind({config.bindHost.toStdString(), 0});
            if (usedPorts.insert(socket->local_endpoint().port).second) return socket;
        }
        throw std::runtime_error("Could not allocate an unused UDP candidate port");
    }
    void ackPair(const QJsonObject& message)
    {
        auto object = fields("jam_pair_ack");
        for (const char* name : {"nonce", "attempt", "introduction"}) object[QLatin1String(name)] = message.value(QLatin1String(name));
        send(signalSocket, object);
    }
    void failure(const QString& a, const QString& b, const QString& detail)
    {
        post([a,b,detail](auto& self) { if (self.onPairFailure) self.onPairFailure(a,b,detail); });
    }
    void queueMembership(CommandKind kind, const QString& peer, quint64 now)
    {
        for (const auto pending : {CommandKind::Admit, CommandKind::Remove, CommandKind::Reject})
            membershipActions.erase({pending, peer});
        membershipActions[{kind, peer}] = {0, now + 8000000, ++nextMembershipSequence};
    }
    void processCommands(quint64 now)
    {
        while (!commands.empty()) {
            const auto command = std::move(commands.front()); commands.pop_front();
            switch (command.kind) {
            case CommandKind::Recover:
                if (!config.creator && command.a == coordinator) {
                    recoveryJoin = true; joining = true; warningPosted = false;
                    nextJoin = 0; joinDeadline = now + 45000000;
                }
                break;
            case CommandKind::PrepareRecovery:
                if (!config.creator) break;
                repairing.insert(command.a);
                if (paused) { ++directoryRevision; paused = false; }
                registered = false; nextRegister = 0;
                break;
            case CommandKind::Introduce:
                if (!config.creator || !token(command.a) || !token(command.b) || !token(command.request) ||
                    command.a == command.b || requests.contains(command.request)) break;
                if (paused) { ++directoryRevision; paused = false; registered = false; nextRegister = 0; }
                if (std::any_of(requests.begin(), requests.end(), [&](const auto& entry) {
                    const auto& pending = entry.second;
                    return (pending.a == command.a && pending.b == command.b) ||
                        (pending.a == command.b && pending.b == command.a);
                })) break;
                requests[command.request] = {command.a, command.b, command.request, 0, now + 45000000};
                break;
            case CommandKind::RegisterMember:
                registeringMember = true; nextMember = 0; memberDeadline = now + 8000000;
                break;
            case CommandKind::Admit:
                if (!config.creator) break;
                pendingJoins.erase(command.a); admitted.insert(command.a); repairing.erase(command.a);
                queueMembership(command.kind, command.a, now);
                registered = false; nextRegister = 0;
                break;
            case CommandKind::Reject:
                if (!config.creator) break;
                pendingJoins.erase(command.a);
                queueMembership(command.kind, command.a, now);
                break;
            case CommandKind::Remove:
                admitted.erase(command.a); pendingJoins.erase(command.a); repairing.erase(command.a);
                if (const auto held = retainedLinks.find(command.a); held != retainedLinks.end()) {
                    for (const auto& [channel, connection] : held->second->streams) { (void)channel; connection->retire(); }
                    retainedLinks.erase(held);
                }
                if (const auto found = links.find(command.a); found != links.end()) {
                    for (const auto& [channel, connection] : found->second->streams) { (void)channel; connection->close(); }
                    links.erase(found);
                }
                if (config.creator) {
                    membershipActions.erase({CommandKind::Admit, command.a});
                    queueMembership(command.kind, command.a, now);
                }
                if (paused) { ++directoryRevision; paused = false; registered = false; }
                nextRegister = 0;
                break;
            case CommandKind::Open:
                if (const auto found = links.find(command.a); found != links.end() &&
                    found->second->phase == Phase::Active && !config.creator && command.a == coordinator)
                    found->second->wantedStreams.insert(command.channel);
                break;
            }
        }
    }
    void receiveSignal(const QJsonObject& message, quint64 now)
    {
        if (number(message, "v") != 1 || text(message,"to") != config.localToken ||
            text(message,"session") != config.sessionHex) return;
        ++counters.signalPacketsReceived;
        if (token(text(message,"creator"))) coordinator = text(message,"creator");
        const auto kind = text(message,"type"), peer = text(message,"peer"), request = text(message,"nonce");
        if (config.creator && (kind == QLatin1String("jam_admit_ack") || kind == QLatin1String("jam_remove_ack") || kind == QLatin1String("jam_reject_ack"))) {
            const auto action = kind == QLatin1String("jam_admit_ack") ? CommandKind::Admit
                : kind == QLatin1String("jam_remove_ack") ? CommandKind::Remove : CommandKind::Reject;
            const auto pending = membershipActions.find({action, peer});
            if (pending != membershipActions.end() && pending->second.sequence == number(message, "member_sequence"))
                membershipActions.erase(pending);
            return;
        }
        if (kind == QLatin1String("jam_create_challenge") && config.creator) {
            if (number(message, "directory_revision") == directoryRevision && token(text(message, "nonce"))) {
                registrationChallenge = text(message, "nonce"); nextRegister = 0;
            }
            return;
        }
        if (kind == QLatin1String("jam_created")) {
            if (number(message, "directory_revision") != directoryRevision) return;
            const bool first = !registered;
            if (!paused) { registered = true; nextRegister = now + 15000000; }
            if (first) post([](auto& self) { if (self.onRegistered) self.onRegistered(); });
            return;
        }
        if (kind == QLatin1String("jam_paused")) {
            if (number(message, "directory_revision") == directoryRevision) pauseConfirmed = true;
            return;
        }
        if (kind == QLatin1String("jam_wait")) { registeringMember = false; return; }
        if (kind == QLatin1String("jam_join_request") && config.creator && token(peer)) {
            if (pendingJoins.contains(peer)) return;
            pendingJoins.insert(peer);
            post([peer](auto& self) { if (self.onJoinRequest) self.onJoinRequest(peer); }); return;
        }
        if (kind == QLatin1String("jam_error")) {
            log(text(message,"detail"));
            if (joining && now >= joinDeadline && !warningPosted) {
                warningPosted = true; joining = false; failure(config.localToken, coordinator, text(message,"detail"));
            }
            return;
        }
        if (kind == QLatin1String("jam_connected") || kind == QLatin1String("jam_failed")) {
            ackPair(message);
            if (!terminalRequests.insert(request).second) return;
            terminalOrder.push_back(request);
            if (terminalOrder.size() > 128) { terminalRequests.erase(terminalOrder.front()); terminalOrder.pop_front(); }
            requests.erase(request);
            if (const auto found = links.find(peer); found != links.end() && found->second->request == request) {
                const auto link = found->second;
                if (number(message,"introduction") != link->introduction ||
                    number(message,"attempt") != static_cast<quint64>(link->attempt) || link->phase == Phase::Active || link->phase == Phase::Failed) return;
                if (kind == QLatin1String("jam_connected")) {
                    if (!link->gotHello || !link->gotAck) return;
                    link->phase = Phase::Active; link->lastReceive = now;
                    if (const auto held = retainedLinks.find(peer); held != retainedLinks.end()) {
                        for (const auto& [channel, connection] : held->second->streams) { (void)channel; connection->retire(); }
                        retainedLinks.erase(held);
                    }
                    log(QStringLiteral("UDP link active: %1, attempt %2, local port %3, route=%4, remote=%5").arg(peer).arg(link->attempt).arg(link->socket->local_endpoint().port).arg(link->route, QString::fromStdString(jam2::endpoint_to_string(jam2::format_udp_endpoint(link->remote)))));
                    post([peer](auto& self) { if (self.onLinkReady) self.onLinkReady(peer); });
                    return;
                }
                link->phase = Phase::Failed; link->socket.reset();
                pendingJoins.erase(peer);
            } else if (kind == QLatin1String("jam_connected")) return;
            const auto detail = number(message,"attempt") >= 10
                ? QStringLiteral("Direct UDP connection failed after 10 socket pairs; existing links remain active.")
                : QStringLiteral("UDP introduction did not complete; existing links remain active.");
            failure(text(message,"a"), text(message,"b"), detail);
            return;
        }
        if (kind == QLatin1String("jam_prepare")) {
            if (!token(peer) || !token(request) || peer == config.localToken || terminalRequests.contains(request)) return;
            const auto attempt = number(message,"attempt"), intro = number(message,"introduction");
            if (attempt < 1 || attempt > 10 || intro == 0) return;
            auto found = links.find(peer);
            if (found != links.end()) {
                const auto prior = found->second;
                if (prior->request != request && attempt == 1) {
                    if (prior->phase == Phase::Active) retainedLinks[peer] = prior;
                    else for (const auto& [channel, connection] : prior->streams) { (void)channel; connection->retire(); }
                    links.erase(found); found = links.end();
                } else {
                    if (prior->phase == Phase::Active || prior->phase == Phase::Failed) return;
                    if (prior->request != request || attempt < static_cast<quint64>(prior->attempt) ||
                        (attempt == static_cast<quint64>(prior->attempt) && intro != prior->introduction)) return;
                    if (attempt == static_cast<quint64>(prior->attempt)) return;
                }
            } else if (attempt != 1) return;
            auto link = std::make_shared<Link>();
            link->peer = peer; link->request = request; link->attempt = static_cast<int>(attempt);
            link->introduction = intro; link->socket = candidate(); link->deadline = now + 8000000;
            links[peer] = link; joining = false;
            log(QStringLiteral("UDP candidate: %1, attempt %2, local port %3").arg(peer).arg(attempt).arg(link->socket->local_endpoint().port));
            return;
        }
        const auto found = links.find(peer);
        if (found == links.end()) return;
        const auto link = found->second;
        if (link->request != request || link->introduction != number(message,"introduction") ||
            static_cast<quint64>(link->attempt) != number(message,"attempt") ||
            link->phase == Phase::Active || link->phase == Phase::Failed) return;
        const auto endpointObject = message.value(QStringLiteral("endpoint")).toObject();
        const auto endpoint = [&]() -> std::optional<jam2::ResolvedUdpEndpoint> {
            const auto host = text(endpointObject,"host");
            const auto port = number(endpointObject,"port");
            if (host.isEmpty() || port == 0 || port > 65535) return {};
            try { return jam2::resolve_udp_endpoint({host.toStdString(), static_cast<std::uint16_t>(port)}); }
            catch (...) { return {}; }
        };
        if (kind == QLatin1String("jam_registered") || kind == QLatin1String("jam_offer") || kind == QLatin1String("jam_go")) {
            // Endpoint is the partner on generic server messages; own observed
            // endpoint is provided explicitly in jam_registered by the server.
            const auto observed = message.value(QStringLiteral("observed")).toObject();
            const auto host = text(observed,"host"); const auto port = number(observed,"port");
            if (!host.isEmpty() && port > 0 && port <= 65535)
                link->observed = jam2::resolve_udp_endpoint({host.toStdString(), static_cast<std::uint16_t>(port)});
        }
        if (kind == QLatin1String("jam_offer") && (link->phase == Phase::Preparing || link->phase == Phase::Offered)) {
            if (const auto remote = endpoint()) {
                link->remote = *remote; link->phase = Phase::Offered; link->nextAction = 0;
                link->route = text(message, "route") == QLatin1String("lan") ? QStringLiteral("lan") : QStringLiteral("public");
            }
        } else if (kind == QLatin1String("jam_go") && link->phase == Phase::Offered) {
            link->phase = Phase::Probing; link->deadline = now + 3000000; link->nextAction = 0;
        }
    }
    void hello(const std::shared_ptr<Link>& link, quint64 challenge, bool acknowledgement)
    {
        const auto payload = jam2::protocol::encode_peer_identity(peerId(config.localToken));
        const auto packet = jam2::protocol::encode_packet(
            {acknowledgement ? jam2::protocol::PacketType::HelloAck : jam2::protocol::PacketType::Hello,
             session, static_cast<std::uint32_t>(link->helloSent), challenge, 0, 0},
            payload, nativeKey, jam2::NetworkAudioFormat::Pcm24Mono);
        (void)link->socket->send_to(link->remote, packet);
    }
    void directProof(const std::shared_ptr<Link>& link, std::span<const std::uint8_t> packet,
                     const jam2::ResolvedUdpEndpoint& source)
    {
        if (source != link->remote || (link->phase != Phase::Probing && link->phase != Phase::Reporting)) return;
        const auto parsed = jam2::protocol::parse_packet(packet, nativeKey, session, jam2::NetworkAudioFormat::Pcm24Mono);
        if (!parsed || packet.size() < jam2::protocol::kHeaderSize) return;
        const auto sender = jam2::protocol::decode_peer_identity(packet.subspan(jam2::protocol::kHeaderSize));
        if (!sender || *sender != peerId(link->peer)) return;
        if (parsed.header.type == jam2::protocol::PacketType::Hello) {
            link->gotHello = true; hello(link, parsed.header.timing_value, true);
        } else if (parsed.header.type == jam2::protocol::PacketType::HelloAck &&
                   link->challenges.contains(parsed.header.timing_value)) link->gotAck = true;
    }
    void queueAudio(const std::shared_ptr<Link>& link, std::span<const std::uint8_t> packet)
    {
        if (packet.size() > jam2::protocol::kMaxDatagramSize) return;
        {
            std::lock_guard lock(audioMutex);
            if (audioCount == audioQueue.size()) { ++audioDrops; return; }
            auto& slot = audioQueue[audioWrite]; slot.endpoint = link->remote; slot.size = packet.size();
            std::copy(packet.begin(), packet.end(), slot.bytes.begin());
            audioWrite = (audioWrite + 1) % audioQueue.size(); ++audioCount;
        }
        if (packet.size() >= jam2::protocol::kHeaderSize && packet[5] == static_cast<std::uint8_t>(jam2::protocol::PacketType::Audio))
            link->mediaStarted = true;
        ++counters.audioPacketsReceived; audioWake.signal(jam2::monotonic_us());
    }
    bool muxSend(const std::shared_ptr<Link>& link, const QByteArray& packet)
    {
        if (!link->socket || link->phase != Phase::Active || packet.size() < 8) return false;
        const bool asset = packet[6] == static_cast<char>(UdpMuxChannel::Asset) &&
            packet[5] == static_cast<char>(UdpMuxKind::Data);
        auto& budget = asset ? assetBudget : controlBudget;
        const auto charge = static_cast<double>(packet.size() + 28);
        if (budget < charge) { ++counters.budgetDeferrals; return false; }
        if (link->socket->send_to(link->remote, view(packet)).outcome != jam2::UdpSendOutcome::Sent) { ++counters.sendErrors; return false; }
        budget -= charge;
        (asset ? counters.assetBytesSent : counters.controlBytesSent) += static_cast<quint64>(packet.size());
        return true;
    }
    std::shared_ptr<UdpStreamConnection> stream(const std::shared_ptr<Link>& link, UdpMuxChannel channel,
                                               quint64 id, bool incoming)
    {
        auto connection = std::make_shared<UdpStreamConnection>(session, link->introduction, id, channel,
            key, QString::fromStdString(jam2::format_udp_endpoint(link->remote).host), incoming);
        const std::weak_ptr<Wake> weak = commandWake;
        connection->setWakeHandler([weak] { if (const auto wake = weak.lock()) wake->notify(); });
        link->streams[channel] = connection;
        return connection;
    }
    void notifyStream(const std::shared_ptr<Link>& link, const std::shared_ptr<UdpStreamConnection>& connection, bool incoming)
    {
        if (!link->notifiedStreams.insert(connection->streamId()).second) return;
        const auto peer = link->peer; const auto channel = connection->channel();
        post([peer,channel,connection,incoming](auto& self) {
            if (self.onStreamReady) self.onStreamReady(peer, channel, connection, incoming);
        });
    }
    void muxReceive(const std::shared_ptr<Link>& link, const QByteArray& packet, quint64 now)
    {
        const auto frame = decodeUdpMux(packet, session, key);
        if (!frame || frame->introduction != link->introduction ||
            (frame->kind != UdpMuxKind::PeerLeave && link->retiredStreams.contains(frame->stream))) {
            ++counters.invalidMuxPackets; return;
        }
        if (frame->kind == UdpMuxKind::PeerLeave) {
            link->phase = Phase::Failed;
            for (const auto& [channel, connection] : link->streams) { (void)channel; connection->retire(UdpStreamRetirement::PeerLeft); }
            const auto peer = link->peer;
            post([peer](auto& self) { if (self.onPeerLeft) self.onPeerLeft(peer); });
            return;
        }
        if (!(config.creator || link->peer == coordinator)) { ++counters.invalidMuxPackets; return; }
        const bool assetData = frame->channel == UdpMuxChannel::Asset && frame->kind == UdpMuxKind::Data;
        (assetData ? counters.assetBytesReceived : counters.controlBytesReceived) += static_cast<quint64>(packet.size());
        auto found = link->streams.find(frame->channel);
        if (frame->kind == UdpMuxKind::Open && config.creator) {
            if (found != link->streams.end() && found->second->streamId() != frame->stream) {
                // A new authenticated stream ID from the same punched peer
                // also recovers when the old stream's close was lost.
                found->second->retire();
                link->retiredStreams.insert(found->second->streamId());
                link->notifiedStreams.erase(found->second->streamId());
                if (link->retiredStreams.size() > 64) link->retiredStreams.erase(link->retiredStreams.begin());
                found = link->streams.end();
            }
            if (found == link->streams.end()) {
                auto connection = stream(link, frame->channel, frame->stream, true);
                notifyStream(link, connection, true);
                found = link->streams.find(frame->channel);
            }
        }
        if (found == link->streams.end() || found->second->streamId() != frame->stream) return;
        found->second->receive(*frame, now, [&](const QByteArray& outgoing) { return muxSend(link, outgoing); });
        if (!config.creator && found->second->isConnected()) notifyStream(link, found->second, false);
    }
    void readSockets(quint64 now)
    {
        std::array<std::uint8_t, 1500> buffer{};
        auto read = [&](const std::shared_ptr<jam2::UdpSocket>& socket, const std::shared_ptr<Link>& link) {
            for (int count = 0; count < 32; ++count) {
                const auto received = socket->recv_from_for(buffer, 0);
                if (!received) break;
                const auto packet = std::span<const std::uint8_t>(buffer.data(), received->size);
                if (received->endpoint == server) {
                    const auto object = QJsonDocument::fromJson(bytes(packet)).object();
                    receiveSignal(object, now);
                } else if (link && received->endpoint == link->remote) {
                    link->lastReceive = now; link->stalledNotified = false;
                    if (link->phase != Phase::Active) directProof(link, packet, received->endpoint);
                    else if (packet.size() >= 4 && std::equal(packet.begin(), packet.begin()+4, "J2MX"))
                        muxReceive(link, bytes(packet), now);
                    else if (packet.size() >= 4 && std::equal(packet.begin(), packet.begin()+4, "JAM2"))
                        queueAudio(link, packet);
                }
                if (link && link->socket != socket) break;
            }
        };
        read(signalSocket, {});
        // receiveSignal can replace a candidate in links: retain the old shared
        // socket through this receive batch and never iterate an invalidated map.
        std::vector<std::shared_ptr<Link>> current;
        for (const auto& [peer, link] : links) { (void)peer; current.push_back(link); }
        for (const auto& [peer, link] : retainedLinks) { (void)peer; current.push_back(link); }
        for (const auto& link : current) if (link->socket) { const auto socket = link->socket; read(socket, link); }
    }
    bool pending() const
    {
        if (!requests.empty() || !pendingJoins.empty() || !membershipActions.empty() || !repairing.empty()) return true;
        for (const auto& [peer, link] : links) {
            (void)peer;
            if (link->phase != Phase::Active && link->phase != Phase::Failed) return true;
        }
        return false;
    }
    void timers(quint64 now)
    {
        if (lastBudget) {
            const auto seconds = static_cast<double>(now - lastBudget) / 1000000.0;
            controlBudget = (std::min)(4096.0, controlBudget + seconds * config.controlBytesPerSecond);
            assetBudget = (std::min)(4096.0, assetBudget + seconds * config.assetBytesPerSecond);
        }
        lastBudget = now;
        if (config.creator) {
            const bool full = config.maximumPeers > 0 && admitted.size() >= static_cast<std::size_t>(config.maximumPeers);
            if (full && !pending() && !paused) {
                ++directoryRevision;
                send(signalSocket, fields("jam_pause")); paused = true; registered = false;
                pauseConfirmed = false; nextPause = now + 500000; pauseDeadline = now + 8000000;
                log(QStringLiteral("Jam full; rendezvous keepalives paused"));
            }
            if (paused && !pauseConfirmed && now >= nextPause && now < pauseDeadline) {
                send(signalSocket, fields("jam_pause")); nextPause = now + 500000;
            }
            if (!full && paused) { ++directoryRevision; paused = false; registered = false; nextRegister = 0; }
            if (!paused && now >= nextRegister) {
                auto object = fields("jam_create");
                object[QStringLiteral("max_peers")] = config.maximumPeers;
                object[QStringLiteral("member_count")] = static_cast<int>(admitted.size());
                send(signalSocket, object); nextRegister = now + (registered ? 15000000 : 500000);
            }
        } else if (joining && now >= nextJoin) {
            if (now >= joinDeadline) {
                joining = false; failure(config.localToken, coordinator, QStringLiteral("Rendezvous server or creator did not accept the join"));
            } else { send(signalSocket, fields(recoveryJoin ? "jam_recover" : "jam_join")); nextJoin = now + 500000; }
        }
        if (registeringMember && now >= memberDeadline) {
            registeringMember = false;
            log(QStringLiteral("Temporary mesh registration timed out; existing links remain active"));
        }
        if (registeringMember && now >= nextMember) {
            send(signalSocket, fields("jam_member")); nextMember = now + 500000;
        }
        for (auto it = membershipActions.begin(); it != membershipActions.end();) {
            if (now >= it->second.deadline) {
                log(QStringLiteral("Directory membership acknowledgement timed out; established links remain active"));
                it = membershipActions.erase(it); continue;
            }
            if (now >= it->second.nextSend) {
                const auto kind = it->first.first;
                auto object = fields(kind == CommandKind::Admit ? "jam_admit" : kind == CommandKind::Remove ? "jam_remove" : "jam_reject");
                object[QStringLiteral("to")] = it->first.second;
                object[QStringLiteral("member_sequence")] = static_cast<qint64>(it->second.sequence);
                send(signalSocket, object); it->second.nextSend = now + 500000;
            }
            ++it;
        }
        for (auto it = requests.begin(); it != requests.end();) {
            auto& request = it->second;
            if (now >= request.deadline) {
                failure(request.a, request.b, QStringLiteral("Coordinated UDP introduction timed out"));
                it = requests.erase(it); continue;
            }
            if (now >= request.nextSend) {
                auto object = fields("jam_pair");
                object[QStringLiteral("a")] = request.a; object[QStringLiteral("b")] = request.b;
                object[QStringLiteral("nonce")] = request.nonce;
                send(signalSocket, object); request.nextSend = now + 500000;
            }
            ++it;
        }
        std::vector<std::shared_ptr<Link>> ordered;
        for (const auto& [peer, link] : links) { (void)peer; ordered.push_back(link); }
        for (const auto& [peer, link] : retainedLinks) { (void)peer; ordered.push_back(link); }
        if (!ordered.empty()) cursor = (cursor + 1) % ordered.size();
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto link = ordered[(index + cursor) % ordered.size()];
            if (link->phase == Phase::Active) {
                // The audio runtime already reprobes the proven socket after
                // silence. Escalate a persistent outage to its coordinator.
                if (link->mediaStarted && link->lastReceive && now - link->lastReceive >= config.linkRepairSilenceMs * 1000 && !link->stalledNotified) {
                    link->stalledNotified = true;
                    const auto peer = link->peer;
                    post([peer](auto& self) { if (self.onLinkStalled) self.onLinkStalled(peer); });
                }
                for (auto it = link->wantedStreams.begin(); it != link->wantedStreams.end();) {
                    const auto old = link->streams.find(*it);
                    if (old != link->streams.end() && old->second->reusable()) {
                        it = link->wantedStreams.erase(it); continue;
                    }
                    if (old != link->streams.end() && !old->second->terminal()) {
                        ++it; continue;
                    }
                    if (old != link->streams.end()) {
                        link->retiredStreams.insert(old->second->streamId());
                        link->notifiedStreams.erase(old->second->streamId());
                        if (link->retiredStreams.size() > 64) link->retiredStreams.erase(link->retiredStreams.begin());
                    }
                    quint64 id = jam2::random_u64(); if (id == 0) id = 1;
                    stream(link, *it, id, false); it = link->wantedStreams.erase(it);
                }
                for (const auto& [channel, connection] : link->streams) {
                    (void)channel;
                    connection->poll(now, 2, [&](const QByteArray& packet) { return muxSend(link, packet); });
                    if (!config.creator && connection->isConnected()) notifyStream(link, connection, false);
                    else if (!config.creator && connection->terminal() &&
                        !connection->failure().isEmpty() && link->notifiedStreams.insert(connection->streamId()).second) {
                        const auto peer = link->peer; const auto detail = connection->failure();
                        post([peer, channel, detail](auto& self) {
                            if (self.onStreamFailed) self.onStreamFailed(peer, channel, detail);
                        });
                    }
                }
                continue;
            }
            if (link->phase == Phase::Failed || !link->socket) continue;
            if ((link->phase == Phase::Preparing || link->phase == Phase::Offered) && now >= link->deadline) {
                link->phase = Phase::Failed; link->socket.reset(); pendingJoins.erase(link->peer);
                failure(config.localToken, link->peer, QStringLiteral("UDP candidate registration or READY handoff timed out")); continue;
            }
            if (link->phase == Phase::Preparing && now >= link->nextAction) {
                send(link->socket, fields("jam_candidate", link)); link->nextAction = now + 500000;
            } else if (link->phase == Phase::Offered && now >= link->nextAction) {
                send(link->socket, fields("jam_ready", link)); link->nextAction = now + 500000;
            } else if (link->phase == Phase::Probing) {
                if (link->gotHello && link->gotAck) {
                    link->phase = Phase::Reporting; link->report = true; link->nextAction = 0; link->deadline = now + 8000000;
                } else if (now >= link->deadline) {
                    link->phase = Phase::Reporting; link->report = false; link->nextAction = 0; link->deadline = now + 8000000;
                } else if (link->helloSent < 5 && now >= link->nextAction) {
                    quint64 challenge = jam2::random_u64(); if (challenge == 0) challenge = 1;
                    link->challenges.insert(challenge); ++link->helloSent;
                    hello(link, challenge, false); link->nextAction = now + 250000;
                }
            }
            if (link->phase == Phase::Reporting) {
                if (now >= link->deadline) {
                    link->phase = Phase::Failed; link->socket.reset(); pendingJoins.erase(link->peer);
                    failure(config.localToken, link->peer, QStringLiteral("UDP attempt result synchronization timed out"));
                } else if (now >= link->nextAction) {
                    auto object = fields("jam_outcome", link); object[QStringLiteral("success")] = link->report;
                    send(link->socket, object); link->nextAction = now + 500000;
                }
            }
        }
    }
    void run()
    {
        try {
            while (!stopping) {
                std::vector<std::shared_ptr<jam2::UdpSocket>> keepAlive;
                std::vector<const jam2::UdpSocket*> sockets;
                quint64 wait = 10000;
                {
                    std::lock_guard lock(mutex);
                    const auto now = jam2::monotonic_us();
                    processCommands(now); readSockets(now); timers(now);
                    keepAlive.push_back(signalSocket);
                    for (const auto& [peer, link] : links) { (void)peer; if (link->socket) keepAlive.push_back(link->socket); }
                    for (const auto& [peer, link] : retainedLinks) { (void)peer; if (link->socket) keepAlive.push_back(link->socket); }
                    for (const auto& socket : keepAlive) sockets.push_back(socket.get());
                }
                jam2::UdpSocket::waitForAny(sockets, wait, &commandWake->signal);
            }
            // Closing a logical stream must reach the remote owner, as TCP FIN
            // did previously. Give the reliable close exchange a bounded drain
            // before destroying physical sockets; the audio runtime has stopped.
            const auto closeDeadline = jam2::monotonic_us() + 400000;
            std::vector<std::shared_ptr<Link>> closingLinks;
            {
                std::lock_guard lock(mutex);
                for (const auto& [peer, link] : links) { (void)peer; closingLinks.push_back(link); }
                for (const auto& [peer, link] : retainedLinks) { (void)peer; closingLinks.push_back(link); }
                for (const auto& link : closingLinks) for (const auto& [channel, connection] : link->streams) {
                    (void)channel; connection->close();
                }
            }
            quint64 nextLeave = 0;
            int leavesSent = 0;
            while (jam2::monotonic_us() < closeDeadline) {
                bool complete = true;
                std::vector<std::shared_ptr<jam2::UdpSocket>> held;
                std::vector<const jam2::UdpSocket*> sockets;
                {
                    std::lock_guard lock(mutex);
                    const auto now = jam2::monotonic_us();
                    readSockets(now);
                    controlBudget = 4096;
                    if (now >= nextLeave && leavesSent < 3) {
                        for (const auto& link : closingLinks) {
                            if (link->phase == Phase::Active) muxSend(link,
                                encodeUdpMux({UdpMuxKind::PeerLeave, UdpMuxChannel::Control,
                                    session, link->introduction, 1, 0, {}}, key));
                        }
                        ++leavesSent; nextLeave = now + 100000;
                    }
                    for (const auto& link : closingLinks) {
                        if (link->socket) { held.push_back(link->socket); sockets.push_back(link->socket.get()); }
                        for (const auto& [channel, connection] : link->streams) {
                            (void)channel;
                            connection->poll(now, 1, [&](const QByteArray& packet) { return muxSend(link, packet); });
                            complete = complete && connection->terminal();
                        }
                    }
                }
                if (complete && leavesSent == 3) break;
                jam2::UdpSocket::waitForAny(sockets, 10000);
            }
            if (config.creator) { ++directoryRevision; send(signalSocket, fields("jam_leave")); }
        } catch (const std::exception& exception) {
            const auto detail = QString::fromUtf8(exception.what());
            failure(config.localToken, coordinator, detail);
        }
        stopping = true; audioWake.signal(jam2::monotonic_us());
    }
};

UdpMultiplexTransport::UdpMultiplexTransport(QObject* parent) : QObject(parent) {}
UdpMultiplexTransport::~UdpMultiplexTransport() { stop(); }
bool UdpMultiplexTransport::start(const Config& config, QString& error)
{
    stop();
    try {
        impl_ = std::make_unique<Impl>(this, config);
        impl_->worker = std::thread([state = impl_.get()] { state->run(); });
        return true;
    } catch (const std::exception& exception) { impl_.reset(); error = QString::fromUtf8(exception.what()); return false; }
}
void UdpMultiplexTransport::stop() { if (impl_) impl_->stop(); }
bool UdpMultiplexTransport::introduce(const QString& a, const QString& b, const QString& request)
{
    return impl_ && impl_->enqueue({Impl::CommandKind::Introduce, a, b, request});
}
bool UdpMultiplexTransport::prepareIntroduction(const QString& request)
{
    return impl_ && impl_->enqueue({Impl::CommandKind::RegisterMember, {}, {}, request});
}
bool UdpMultiplexTransport::admitPeer(const QString& peer) { return impl_ && impl_->enqueue({Impl::CommandKind::Admit, peer, {}, {}}); }
bool UdpMultiplexTransport::rejectPeer(const QString& peer) { return impl_ && impl_->enqueue({Impl::CommandKind::Reject, peer, {}, {}}); }
bool UdpMultiplexTransport::removePeer(const QString& peer) { return impl_ && impl_->enqueue({Impl::CommandKind::Remove, peer, {}, {}}); }
bool UdpMultiplexTransport::recoverPeer(const QString& peer)
{ return impl_ && impl_->enqueue({Impl::CommandKind::Recover, peer, {}, {}}); }
bool UdpMultiplexTransport::prepareRecovery(const QString& peer)
{ return impl_ && impl_->enqueue({Impl::CommandKind::PrepareRecovery, peer, {}, {}}); }
bool UdpMultiplexTransport::openStream(const QString& peer, UdpMuxChannel channel)
{
    return impl_ && impl_->enqueue({Impl::CommandKind::Open, peer, {}, {}, channel});
}
std::optional<jam2::Endpoint> UdpMultiplexTransport::peerEndpoint(quint64 id) const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    for (const auto& [peer, link] : impl_->links)
        if (peerId(peer) == id && link->phase == Phase::Active) return jam2::format_udp_endpoint(link->remote);
    for (const auto& [peer, link] : impl_->retainedLinks)
        if (peerId(peer) == id && link->phase == Phase::Active) return jam2::format_udp_endpoint(link->remote);
    return {};
}
std::optional<jam2::Endpoint> UdpMultiplexTransport::observedEndpoint(const QString& peer) const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->links.find(peer);
    if (found == impl_->links.end() || found->second->observed.port == 0) return {};
    return jam2::format_udp_endpoint(found->second->observed);
}
QString UdpMultiplexTransport::coordinatorToken() const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex); return impl_->coordinator;
}
UdpMultiplexTransport::Snapshot UdpMultiplexTransport::snapshot() const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->counters;
    result.running = !impl_->stopping; result.registered = impl_->registered; result.admissionPaused = impl_->paused; result.pauseConfirmed = impl_->pauseConfirmed;
    result.audioQueueDrops = impl_->audioDrops;
    for (const auto& [peer, link] : impl_->links) {
        LinkSnapshot value; value.peer = peer; value.request = link->request; value.phase = link->phase;
        value.route = link->route;
        value.attempt = link->attempt; value.helloSent = link->helloSent; value.introduction = link->introduction;
        if (link->socket) value.local = link->socket->local_endpoint();
        if (link->remote.port) value.remote = jam2::format_udp_endpoint(link->remote);
        if (link->observed.port) value.observed = jam2::format_udp_endpoint(link->observed);
        value.receivedHello = link->gotHello; value.receivedAck = link->gotAck;
        if (link->lastReceive) value.lastReceiveAgeMs = (jam2::monotonic_us() - link->lastReceive) / 1000;
        if (const auto held = impl_->retainedLinks.find(peer); held != impl_->retainedLinks.end() && held->second->socket)
            value.retainedAudioLocal = held->second->socket->local_endpoint();
        for (const auto& [channel, connection] : link->streams) {
            (channel == UdpMuxChannel::Asset ? value.assetQueued : value.controlQueued) += static_cast<quint64>(connection->bytesToWrite());
            const auto streamStats = connection->stats();
            value.retransmissions += streamStats.retransmitted;
            value.inputDeferrals += connection->inputDeferrals();
            if (channel == UdpMuxChannel::Control) {
                value.controlStream = connection->streamId(); value.controlRttUs = streamStats.smoothed_rtt_us;
                value.controlWindow = streamStats.congestion_window;
            } else {
                value.assetStream = connection->streamId(); value.assetRttUs = streamStats.smoothed_rtt_us;
                value.assetWindow = streamStats.congestion_window;
            }
        }
        result.links.push_back(value);
    }
    return result;
}
jam2::UdpSendResult UdpMultiplexTransport::sendToPeer(
    std::uint64_t id, const jam2::ResolvedUdpEndpoint&, std::span<const std::uint8_t> packet) noexcept
{
    if (!impl_ || impl_->stopping) return {jam2::UdpSendOutcome::WouldBlock, 0};
    std::lock_guard lock(impl_->mutex);
    auto selected = impl_->links.end();
    for (auto it = impl_->links.begin(); it != impl_->links.end(); ++it)
        if (peerId(it->first) == id) { selected = it; break; }
    std::shared_ptr<Impl::Link> link;
    if (selected != impl_->links.end() && selected->second->phase == Phase::Active) link = selected->second;
    else for (const auto& [peer, held] : impl_->retainedLinks)
        if (peerId(peer) == id && held->phase == Phase::Active) { link = held; break; }
    if (link && link->socket) {
        const auto result = link->socket->send_to(link->remote, packet);
        if (result.outcome == jam2::UdpSendOutcome::Sent) ++impl_->counters.audioPacketsSent;
        return result;
    }
    return {jam2::UdpSendOutcome::WouldBlock, 0};
}
std::optional<jam2::UdpSocket::ReceivedDatagram> UdpMultiplexTransport::receiveFor(
    std::span<std::uint8_t> buffer, std::uint64_t timeoutUs, jam2::RealtimeWakeSignal* captureWake)
{
    if (!impl_) return {};
    auto take = [&]() -> std::optional<jam2::UdpSocket::ReceivedDatagram> {
        std::lock_guard lock(impl_->audioMutex);
        if (impl_->audioCount == 0) return {};
        const auto& slot = impl_->audioQueue[impl_->audioRead];
        const bool fits = slot.size <= buffer.size();
        if (fits) std::copy_n(slot.bytes.begin(), slot.size, buffer.begin());
        const auto result = jam2::UdpSocket::ReceivedDatagram{slot.endpoint, slot.size};
        impl_->audioRead = (impl_->audioRead + 1) % impl_->audioQueue.size(); --impl_->audioCount;
        return fits ? std::optional(result) : std::nullopt;
    };
    if (auto packet = take()) return packet;
    if (timeoutUs && !impl_->stopping) impl_->audioWake.waitFor(timeoutUs, captureWake);
    return take();
}

} // namespace jam2::application
