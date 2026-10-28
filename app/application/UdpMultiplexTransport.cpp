#include "UdpMultiplexTransport.hpp"
#include "ControlProtocol.hpp"
#include "protocol.hpp"
#include "stun.hpp"
#include "PeerBootstrap.hpp"
#include "PeerIntroductionCoordinator.hpp"

#include "RendezvousWire.hpp"
#include <QPointer>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace jam2::application {
namespace {
using Phase = UdpMultiplexTransport::LinkPhase;
namespace rv = rendezvous_wire;
using SignalKind = rv::Kind;
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
UdpMuxKind muxKind(std::span<const std::uint8_t> packet)
{ return static_cast<UdpMuxKind>(isCompactUdpMux(packet) ? packet[5]&15 : packet[5]); }
UdpMuxChannel muxChannel(std::span<const std::uint8_t> packet)
{ return static_cast<UdpMuxChannel>(isCompactUdpMux(packet) ? (packet[5]>>4)&3 : packet[6]); }
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
        bool hostLink = false, remoteReported = false;
        quint64 nextBootstrap = 0;
        quint64 deadline = 0, nextAction = 0;
        std::atomic<quint64> lastReceive{0};
        std::atomic<bool> stalledNotified{false}, mediaStarted{false}, readable{false};
        std::set<quint64> challenges;
        std::map<UdpMuxChannel, std::shared_ptr<UdpStreamConnection>> streams;
        std::map<quint16,std::weak_ptr<UdpStreamConnection>> compactStreams;
        quint32 nextCompactHandle=1;
        std::mutex intentMutex;
        std::array<std::uint8_t,28> lastIntent{};
        std::array<std::uint8_t,28> pendingIntentAck{};
        bool hasPendingIntentAck=false;
        bool intentUsed=false,allowIntentReceipts=true;
        bool intentSent=false,intentAcknowledged=false;
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
    struct ReadRoute {
        std::shared_ptr<Link> link;
        std::shared_ptr<jam2::UdpSocket> socket;
        jam2::ResolvedUdpEndpoint endpoint;
        quint64 peer = 0;
    };
    struct ReadRoutes {
        std::vector<ReadRoute> routes;
        std::vector<const jam2::UdpSocket*> sockets;
        std::vector<std::size_t> readerIndices;
    };
    struct IngressPacket {
        std::shared_ptr<Link> link;
        bool signal = false;
        bool bootstrap = false, proof = false;
        jam2::ResolvedUdpEndpoint source{};
        quint16 size = 0;
        quint64 queuedUs = 0;
        std::array<std::uint8_t, jam2::NetworkDatagramTransport::kReceiveBufferBytes> bytes;
    };
    struct IngressQueue {
        std::array<IngressPacket, kIngressPacketsPerQueue> packets;
        std::size_t read = 0, write = 0, count = 0;
    };
    struct WorkerScratch {
        std::vector<std::shared_ptr<jam2::UdpSocket>> keepAlive;
        std::vector<const jam2::UdpSocket*> sockets;
        std::vector<std::shared_ptr<Link>> readLinks, ordered;
        std::vector<const jam2::UdpSocket*> seenSockets;
        std::size_t capacity() const noexcept { return keepAlive.capacity()+sockets.capacity()+readLinks.capacity()+ordered.capacity()+seenSockets.capacity(); }
    } workerScratch;

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
    mutable std::mutex ingressMutex, receiverMutex;
    std::condition_variable receiverChanged;
    bool receiverRequested = false;
    std::atomic<bool> receiverOwned{false};
    // Apple libc++ requires the shared_ptr atomic free functions.
    std::shared_ptr<const ReadRoutes> readRoutes{std::make_shared<ReadRoutes>()};
    IngressQueue controlIngress, assetIngress;
    quint64 ingressDrops = 0, ingressHighWater = 0;
    std::atomic<quint64> nativeSent{0}, nativeReceived{0}, nativeDelivered{0}, bootstrapDiscarded{0};
    std::atomic<quint64> controlQueued{0}, assetQueued{0}, invalidClassified{0}, classificationYields{0};
    std::atomic<quint64> classificationSamples{0}, classificationSumNs{0}, classificationMaxNs{0};
    std::size_t receiveCursor = 0;
    quint64 classifications = 0;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::deque<Command> commands;
    std::map<std::pair<CommandKind, QString>, MembershipAction> membershipActions;
    std::map<QString, std::shared_ptr<Link>> links, retainedLinks;
    std::map<QString, PairRequest> requests;
    std::unique_ptr<PeerIntroductionCoordinator> directCoordinator;
    QString directJoinRequest;
    int directJoinAttempts=0;
    quint64 directAttemptDeadline=0;
    quint64 bootstrapFailureWindow = 0, bootstrapFailures = 0;
    std::optional<jam2::stun::BindingRequest> stunRequest;
    std::optional<jam2::ResolvedUdpEndpoint> stunEndpoint;
    quint64 stunDeadline=0;
    int stunAttempts=0;
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
    bool immediateSendWork = false;
    std::atomic<quint64> audioDrops{0};
    std::atomic<quint64> intentAcksSent{0},intentAcksReceived{0},intentRepeatsSuppressed{0},intentAckDeferrals{0},intentAckInvalid{0};

    Impl(UdpMultiplexTransport* parent, const Config& initial) : config(initial), owner(parent)
    {
        key = control_protocol::decodeHex(config.keyHex, 16);
        const QByteArray sid = control_protocol::decodeHex(config.sessionHex, 8);
        if (key.size() != 16 || sid.size() != 8 || !token(config.localToken) || peerId(config.localToken) == 0 ||
            config.maximumPeers < 0 || config.pendingAdmissionTimeoutMs < 1 || config.pendingAdmissionTimeoutMs > 60000 || config.controlBytesPerSecond < 1024 || (config.assetBytesPerSecond != 0 && config.assetBytesPerSecond < 1024) || config.linkRepairSilenceMs < 1000 || config.fastReorderUs > 2000000 ||
            (config.socketBuffers.sendBytes && *config.socketBuffers.sendBytes<=0) ||
            (config.socketBuffers.receiveBytes && *config.socketBuffers.receiveBytes<=0) ||
            config.stunTimeoutMs<1 || config.stunTimeoutMs>60000 || config.stunRetries<1 || config.stunRetries>100)
            throw std::runtime_error("Invalid UDP multiplex configuration");
        for (char byte : sid) session = (session << 8) | static_cast<unsigned char>(byte);
        std::copy(key.begin(), key.end(), nativeKey.begin());
        config.sessionHex = config.sessionHex.toLower(); config.keyHex = config.keyHex.toLower();
        config.localToken = config.localToken.toLower();
        signalSocket = std::make_shared<jam2::UdpSocket>();
        signalSocket->bind({config.bindHost.toStdString(),
            config.strategy==jam2::ConnectionStrategy::Jam2Server ? std::uint16_t{0} : config.bindPort});
        configureSocket(*signalSocket);
        server = jam2::resolve_udp_endpoint(config.strategy!=jam2::ConnectionStrategy::Jam2Server && config.creator
            ? jam2::Endpoint{"127.0.0.1",signalSocket->local_endpoint().port} : config.server);
        usedPorts.insert(signalSocket->local_endpoint().port);
        try { localHost = signalSocket->local_endpoint_for(
            config.strategy!=jam2::ConnectionStrategy::Jam2Server && config.creator
                ? jam2::resolve_udp_endpoint({"192.0.2.1",9}) : server).host; }
        catch (const std::exception& error) { log(QStringLiteral("LAN candidate unavailable: %1").arg(QString::fromUtf8(error.what()))); }
        joining = !config.creator;
        coordinator = config.creator ? config.localToken : QString{};
        if (config.strategy!=jam2::ConnectionStrategy::Jam2Server) {
            if (config.creator) directCoordinator=std::make_unique<PeerIntroductionCoordinator>(session,config.localToken);
            else directJoinRequest=QString::fromLatin1(control_protocol::randomNonce().toHex());
        }
        counters.usesRendezvous=config.strategy==jam2::ConnectionStrategy::Jam2Server;
        counters.bootstrapLocal=signalSocket->local_endpoint();
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
    void publishReadRoutes()
    {
        const auto previous = std::atomic_load_explicit(&readRoutes, std::memory_order_acquire);
        std::size_t count = 0; bool same = true;
        const auto inspect = [&](const auto& collection) {
            for (const auto& [peer, link] : collection) {
                (void)peer;
                if (link->phase != Phase::Active || !link->socket) continue;
                if (count >= previous->routes.size() || previous->routes[count].link != link || previous->routes[count].socket != link->socket) same = false;
                ++count;
            }
        };
        inspect(links); inspect(retainedLinks);
        if (same && count == previous->routes.size()) return;
        auto next = std::make_shared<ReadRoutes>(); next->routes.reserve(count); next->sockets.reserve(count);
        const auto append = [&](const auto& collection) {
            for (const auto& [peer, link] : collection) if (link->phase == Phase::Active && link->socket) {
                next->routes.push_back({link, link->socket, link->remote, peerId(peer)});
                if (std::find(next->sockets.begin(),next->sockets.end(),link->socket.get())==next->sockets.end()) {
                    next->readerIndices.push_back(next->routes.size()-1); next->sockets.push_back(link->socket.get());
                }
                link->readable.store(true, std::memory_order_release);
            }
        };
        append(links); append(retainedLinks);
        for (const auto& route : previous->routes)
            if (std::none_of(next->routes.begin(), next->routes.end(), [&](const auto& current) { return current.link == route.link; }))
                route.link->readable.store(false, std::memory_order_release);
        std::atomic_store_explicit(&readRoutes, std::shared_ptr<const ReadRoutes>(std::move(next)), std::memory_order_release);
        audioWake.signal(jam2::monotonic_us());
    }
    void updateReceiverOwnership()
    {
        const bool wanted = receiverRequested && config.receiveModel == ReceiveModel::Runtime;
        if (receiverOwned.load(std::memory_order_relaxed) == wanted) return;
        std::lock_guard reader(receiverMutex);
        receiverOwned.store(wanted, std::memory_order_release);
        if (wanted) ++counters.receiverHandovers; else ++counters.receiverDetachments;
        receiverChanged.notify_all();
    }
    void attachReceiver()
    {
        std::unique_lock lock(mutex);
        if (stopping || receiverRequested) throw std::runtime_error("UDP runtime receiver is stopped or already attached");
        receiverRequested = true;
        if (config.receiveModel == ReceiveModel::QueuedWorker) return;
        commandWake->notify();
        if (!receiverChanged.wait_for(lock, std::chrono::milliseconds(kReceiverHandoverTimeoutMs), [&] { return receiverOwned.load(std::memory_order_acquire) || stopping.load(); }) || stopping) {
            receiverRequested = false; commandWake->notify();
            throw std::runtime_error("UDP socket receive ownership handover failed");
        }
    }
    void detachReceiver() noexcept
    {
        std::lock_guard lock(mutex);
        receiverRequested = false; commandWake->notify();
    }
    bool rendezvous() const noexcept { return config.strategy==jam2::ConnectionStrategy::Jam2Server; }
    bool signalRuntimeOwned() const
    {
        if (!receiverOwned.load(std::memory_order_acquire)) return false;
        const auto routes=std::atomic_load_explicit(&readRoutes, std::memory_order_acquire);
        return std::any_of(routes->routes.begin(),routes->routes.end(),[&](const ReadRoute& route) { return route.socket==signalSocket; });
    }
    void activateLink(const std::shared_ptr<Link>& link,quint64 now)
    {
        if (link->phase==Phase::Active) return;
        link->phase=Phase::Active; link->lastReceive=now;
        if (config.creator && link->hostLink)
            link->deadline=now+config.pendingAdmissionTimeoutMs*1000;
        if (const auto held=retainedLinks.find(link->peer);held!=retainedLinks.end()) {
            for (const auto& [channel,connection] : held->second->streams) { (void)channel; connection->retire(); }
            retainedLinks.erase(held);
        }
        const auto peer=link->peer;
        post([peer](auto& self) { if (self.onLinkReady) self.onLinkReady(peer); });
    }
    void sendBootstrap(const std::shared_ptr<jam2::UdpSocket>& socket,const jam2::ResolvedUdpEndpoint& destination,
        PeerBootstrapMessage message)
    {
        message.session=session; message.sender=config.localToken;
        const auto packet=encodePeerBootstrap(message,key);
        if (!packet.isEmpty() && socket->send_to(destination,view(packet)).outcome==jam2::UdpSendOutcome::Sent) ++counters.signalPacketsSent;
    }
    PeerBootstrapMessage bootstrapMessage(PeerBootstrapKind kind,const std::shared_ptr<Link>& link) const
    {
        PeerBootstrapMessage message; message.kind=kind; message.request=link->request;
        message.introduction=link->introduction; message.attempt=static_cast<quint8>(link->attempt);
        return message;
    }
    void bootstrapFailure(const QString& detail)
    {
        joining=false;
        post([detail](auto& self) { if (self.onBootstrapFailure) self.onBootstrapFailure(detail); });
    }
    void restartDirectJoin(quint64 now,bool rotate)
    {
        if (config.creator) return;
        if (rotate) {
            if (const auto found=links.find(coordinator);found!=links.end() && found->second->phase==Phase::Active)
                retainedLinks[coordinator]=found->second;
            links.erase(coordinator);
            signalSocket=candidate();
            counters.bootstrapLocal=signalSocket->local_endpoint();
        }
        directJoinRequest=QString::fromLatin1(control_protocol::randomNonce().toHex());
        if (!directJoinAttempts) joinDeadline=now+45000000;
        ++directJoinAttempts;
        joining=directJoinAttempts<=10; nextJoin=0; directAttemptDeadline=now+3000000; ++counters.bootstrapAttempts;
        if (!joining) bootstrapFailure(QStringLiteral("Direct creator connection failed after ten fresh socket attempts"));
    }
    void receiveBootstrap(std::span<const std::uint8_t> packet,const jam2::ResolvedUdpEndpoint& source,quint64 now)
    {
        if (rendezvous()) return;
        if (!bootstrapFailureWindow || now-bootstrapFailureWindow>=10000000) { bootstrapFailureWindow=now; bootstrapFailures=0; }
        if (bootstrapFailures>=64) { ++counters.bootstrapRejected; return; }
        const auto decoded=decodePeerBootstrap(packet,session,key);
        if (!decoded) { ++bootstrapFailures; ++counters.bootstrapAuthenticationRejects; return; }
        ++counters.signalPacketsReceived;
        const auto& message=*decoded;
        if (config.creator) {
            if (!registered) return;
            if (message.sender==config.localToken || peerId(message.sender)==peerId(config.localToken)) { ++counters.bootstrapRejected; return; }
            if (message.kind==PeerBootstrapKind::Join) {
                const auto existing=links.find(message.sender);
                if (existing!=links.end() && existing->second->hostLink && existing->second->request==message.request && existing->second->remote==source) {
                    auto response=bootstrapMessage(existing->second->phase==Phase::Active ? PeerBootstrapKind::Accepted : PeerBootstrapKind::Welcome,existing->second);
                    response.observed=PeerBootstrapEndpoint::from(jam2::format_udp_endpoint(source));
                    sendBootstrap(signalSocket,source,std::move(response)); return;
                }
                const bool full=config.maximumPeers>0 && admitted.size()>=static_cast<std::size_t>(config.maximumPeers) && !admitted.contains(message.sender);
                const auto pending=std::count_if(links.begin(),links.end(),[&](const auto& entry) { return entry.second->hostLink && !admitted.contains(entry.first) && entry.second->phase!=Phase::Failed; });
                if (full || pending>=control_protocol::kMaxPendingPeers) {
                    PeerBootstrapMessage rejected; rejected.kind=PeerBootstrapKind::Rejected; rejected.request=message.request;
                    sendBootstrap(signalSocket,source,std::move(rejected)); ++counters.bootstrapRejected; return;
                }
                if (existing!=links.end() && existing->second->phase==Phase::Active) retainedLinks[message.sender]=existing->second;
                auto link=std::make_shared<Link>(); link->peer=message.sender; link->request=message.request;
                link->hostLink=true; link->socket=signalSocket; link->remote=source; link->attempt=1;
                link->introduction=jam2::random_u64(); link->phase=Phase::Probing; link->deadline=now+3000000;
                if (!counters.bootstrapPublic.host.empty()) link->observed=jam2::resolve_udp_endpoint(counters.bootstrapPublic);
                links[message.sender]=link; ++counters.bootstrapAttempts;
                auto response=bootstrapMessage(PeerBootstrapKind::Welcome,link); response.observed=PeerBootstrapEndpoint::from(jam2::format_udp_endpoint(source));
                sendBootstrap(signalSocket,source,std::move(response)); link->nextBootstrap=now+500000;
                return;
            }
            if (message.kind==PeerBootstrapKind::JoinReady) {
                const auto found=links.find(message.sender);
                if (found==links.end()) return;
                const auto link=found->second;
                if (!link->hostLink || link->request!=message.request || link->introduction!=message.introduction || link->remote!=source) return;
                link->remoteReported=true;
                if (link->phase==Phase::Active) sendBootstrap(signalSocket,source,bootstrapMessage(PeerBootstrapKind::Accepted,link));
                return;
            }
            if (directCoordinator) directCoordinator->receive(message,PeerBootstrapEndpoint::from(jam2::format_udp_endpoint(source)),now);
            return;
        }
        if (source!=server) return;
        if (message.kind==PeerBootstrapKind::Rejected && message.request==directJoinRequest) {
            bootstrapFailure(QStringLiteral("Creator rejected UDP admission (session limit or pending authentication cap)")); return;
        }
        if (message.kind==PeerBootstrapKind::Welcome && message.request==directJoinRequest) {
            coordinator=message.sender; joining=false;
            const auto found=links.find(coordinator);
            if (found!=links.end() && found->second->request==message.request && found->second->introduction==message.introduction) return;
            auto link=std::make_shared<Link>(); link->peer=coordinator; link->request=message.request;
            link->hostLink=true; link->socket=signalSocket; link->remote=source; link->attempt=1;
            link->introduction=message.introduction; link->observed=jam2::resolve_udp_endpoint(message.observed.endpoint());
            link->phase=Phase::Probing; link->deadline=now+3000000; links[coordinator]=link;
            return;
        }
        if (message.sender!=coordinator) return;
        if (message.kind==PeerBootstrapKind::Prepare) {
            auto found=links.find(message.partner);
            if (found!=links.end()) {
                const auto prior=found->second;
                if (prior->request==message.request && prior->attempt>=message.attempt) return;
                if (prior->request!=message.request && message.attempt!=1) return;
                if (prior->phase==Phase::Active) retainedLinks[message.partner]=prior;
                else for (const auto& [channel,connection] : prior->streams) { (void)channel; connection->retire(); }
            } else if (message.attempt!=1) return;
            auto link=std::make_shared<Link>(); link->peer=message.partner; link->request=message.request;
            link->attempt=message.attempt; link->introduction=message.introduction; link->socket=candidate();
            link->phase=Phase::Preparing; link->deadline=now+8000000; links[message.partner]=link;
            ++counters.bootstrapAttempts; return;
        }
        const auto found=std::find_if(links.begin(),links.end(),[&](const auto& entry) {
            const auto& link=entry.second; return link->request==message.request && link->introduction==message.introduction;
        });
        if (found==links.end()) return;
        const auto link=found->second;
        if (message.kind==PeerBootstrapKind::Accepted && link->hostLink) {
            if (link->gotHello && link->gotAck) activateLink(link,now);
        } else if (!link->hostLink && link->attempt==message.attempt) {
            if (message.kind==PeerBootstrapKind::Offer && (link->phase==Phase::Preparing || link->phase==Phase::Offered)) {
                link->remote=jam2::resolve_udp_endpoint(message.endpoint.endpoint());
                link->observed=jam2::resolve_udp_endpoint(message.observed.endpoint());
                link->route=message.lan ? QStringLiteral("lan") : QStringLiteral("public");
                link->phase=Phase::Offered; link->nextAction=0;
            } else if (message.kind==PeerBootstrapKind::Go && link->phase==Phase::Offered) {
                link->phase=Phase::Probing; link->nextAction=0; link->deadline=now+3000000;
            } else if (message.kind==PeerBootstrapKind::Complete && link->gotHello && link->gotAck) {
                activateLink(link,now);
                sendBootstrap(link->socket,server,bootstrapMessage(PeerBootstrapKind::CompleteAck,link));
            }
        }
    }
    bool queueIngress(const ReadRoute& route, std::span<const std::uint8_t> packet, bool signal,
        bool bootstrap=false,bool proof=false,const jam2::ResolvedUdpEndpoint& source={})
    {
        if (packet.size() > jam2::NetworkDatagramTransport::kReceiveBufferBytes) { ++invalidClassified; return false; }
        const bool asset = !signal && !bootstrap && !proof && packet.size() >= 8 && muxChannel(packet)==UdpMuxChannel::Asset &&
            (muxKind(packet)==UdpMuxKind::Data || muxKind(packet)==UdpMuxKind::DataAck);
        {
            std::lock_guard lock(ingressMutex);
            auto& queue = asset ? assetIngress : controlIngress;
            if (queue.count == queue.packets.size()) { ++ingressDrops; return false; }
            auto& slot = queue.packets[queue.write];
            slot.link = route.link; slot.signal = signal; slot.size = static_cast<quint16>(packet.size());
            slot.bootstrap=bootstrap; slot.proof=proof; slot.source=source;
            slot.queuedUs = jam2::monotonic_us(); std::copy(packet.begin(), packet.end(), slot.bytes.begin());
            queue.write = (queue.write + 1) % queue.packets.size(); ++queue.count;
            ingressHighWater = (std::max<quint64>)(ingressHighWater, controlIngress.count + assetIngress.count);
        }
        (asset ? assetQueued : controlQueued).fetch_add(1, std::memory_order_relaxed);
        commandWake->notify(); return true;
    }
    void releaseIngress(IngressQueue& queue)
    {
        std::shared_ptr<Link> retired;
        { std::lock_guard lock(ingressMutex);
            retired = std::move(queue.packets[queue.read].link);
            queue.read = (queue.read + 1) % queue.packets.size(); --queue.count;
        }
    }
    void processIngress()
    {
        for (auto* queue : {&controlIngress, &assetIngress}) for (std::size_t work = 0; work < 64; ++work) {
            IngressPacket* slot;
            { std::lock_guard lock(ingressMutex);
                if (!queue->count) break;
                slot = &queue->packets[queue->read];
            }
            // The sole consumer retains this slot until parsing completes.
            // Count still includes it, so the producer cannot overwrite it.
            struct Release { Impl& owner; IngressQueue& queue; ~Release() { owner.releaseIngress(queue); } } release{*this,*queue};
            const auto& packet = *slot;
            counters.ingressCopyBytesAvoided += packet.size;
            const auto now = jam2::monotonic_us(), delay = now >= packet.queuedUs ? now - packet.queuedUs : 0;
            ++counters.ingressWaitSamples; counters.ingressWaitSumUs += delay;
            counters.ingressWaitMaxUs = (std::max)(counters.ingressWaitMaxUs, delay);
            const auto contents = std::span(packet.bytes).first(packet.size);
            if (packet.bootstrap) { receiveBootstrap(contents,packet.source,now); continue; }
            if (packet.proof) {
                for (const auto& [peer,link] : links) {
                    (void)peer;
                    if (link->remote==packet.source && (link->phase==Phase::Probing || link->phase==Phase::Reporting))
                        directProof(link,contents,packet.source);
                }
                continue;
            }
            if (!packet.link || !packet.link->readable.load(std::memory_order_acquire) || packet.link->phase != Phase::Active) continue;
            if (packet.signal) receiveSignalBytes(bytes(contents), now);
            else muxReceive(packet.link, contents, now);
        }
    }
    bool ingressPending() const
    {
        std::lock_guard lock(ingressMutex); return controlIngress.count || assetIngress.count;
    }
    struct ClassificationTimer {
        Impl* owner;
        std::chrono::steady_clock::time_point began;
        explicit ClassificationTimer(Impl* value) : owner(value), began(value ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
        ~ClassificationTimer() {
            if (!owner) return;
            const auto ns = static_cast<quint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());
            owner->classificationSamples.fetch_add(1, std::memory_order_relaxed);
            owner->classificationSumNs.fetch_add(ns, std::memory_order_relaxed);
            auto highest = owner->classificationMaxNs.load(std::memory_order_relaxed);
            while (ns > highest && !owner->classificationMaxNs.compare_exchange_weak(highest, ns, std::memory_order_relaxed)) {}
        }
    };
    std::optional<jam2::UdpSocket::ReceivedDatagram> receiveRuntime(std::span<std::uint8_t> buffer, quint64 timeoutUs, jam2::RealtimeWakeSignal* captureWake)
    {
        if (buffer.size() < jam2::NetworkDatagramTransport::kReceiveBufferBytes)
            throw std::invalid_argument("UDP multiplex receiver requires a 1500-byte buffer");
        std::lock_guard reader(receiverMutex);
        if (stopping || !receiverOwned.load(std::memory_order_acquire)) return {};
        const auto routes = std::atomic_load_explicit(&readRoutes, std::memory_order_acquire);
        const auto deadline = jam2::monotonic_us() + (std::min)(timeoutUs, kRuntimeWaitMaximumUs);
        std::size_t work = 0; bool waited = false;
        for (;;) {
            bool receivedAny = false;
            const auto count = routes->readerIndices.size();
            for (std::size_t checked = 0; checked < count; ++checked) {
                const auto& initial = routes->routes[routes->readerIndices[receiveCursor++ % count]];
                const ReadRoute* selected=&initial;
                const auto& routeSocket=initial.socket;
                if (!initial.link->readable.load(std::memory_order_acquire)) continue;
                const auto packet = routeSocket->recv_from_for(buffer, 0);
                if (!packet) continue;
                receivedAny = true; ++work;
                ClassificationTimer timing((++classifications & (kClassificationTimingStride-1)) == 0 ? this : nullptr);
                const auto payload = buffer.first(packet->size);
                if (packet->endpoint!=initial.endpoint) {
                    const auto matched=std::find_if(routes->routes.begin(),routes->routes.end(),[&](const ReadRoute& candidate) {
                        return candidate.socket==routeSocket && candidate.endpoint==packet->endpoint && candidate.link->readable.load(std::memory_order_acquire);
                    });
                    if (matched!=routes->routes.end()) selected=&*matched;
                }
                const auto& route=*selected;
                if (!route.link->readable.load(std::memory_order_acquire)) ++invalidClassified;
                else if (!rendezvous() && isPeerBootstrap(payload)) {
                    if (payload.size()>=64 && payload.size()<=kPeerBootstrapMaximumBytes) queueIngress(route,payload,false,true,false,packet->endpoint);
                    else ++invalidClassified;
                }
                else if (rendezvous() && packet->endpoint == server) queueIngress(route, payload, true);
                else if (packet->endpoint == route.endpoint) {
                    route.link->lastReceive.store(jam2::monotonic_us(), std::memory_order_relaxed);
                    route.link->stalledNotified.store(false, std::memory_order_relaxed);
                    if (payload.size() >= 4 && std::equal(payload.begin(),payload.begin()+4,"JAM2")) {
                        if (payload.size() >= jam2::protocol::kHeaderSize && payload[5] == static_cast<std::uint8_t>(jam2::protocol::PacketType::Audio))
                            route.link->mediaStarted.store(true, std::memory_order_relaxed);
                        ++nativeReceived; ++nativeDelivered; return packet;
                    }
                    if (payload.size() >= 8 && std::equal(payload.begin(),payload.begin()+4,"J2MX")) queueIngress(route,payload,false);
                    else ++invalidClassified;
                } else if (!rendezvous() && config.creator && payload.size()>=jam2::protocol::kHeaderSize &&
                    std::equal(payload.begin(),payload.begin()+4,"JAM2")) queueIngress(route,payload,false,false,true,packet->endpoint);
                else ++invalidClassified;
                if (work >= kClassificationPacketsPerCall) { ++classificationYields; return {}; }
            }
            if (stopping) return {};
            // A zero wait budget still drains already available packets. The
            // packet budget bounds classification before other runtime work.
            if (receivedAny && work < kClassificationPacketsPerCall) continue;
            const auto now = jam2::monotonic_us();
            if (waited || now >= deadline) return {};
            waited = true;
            if (routes->sockets.empty()) audioWake.waitFor(deadline-now,captureWake);
            else jam2::UdpSocket::waitForDatagrams(routes->sockets,deadline-now,captureWake);
        }
    }
    void stop()
    {
        stopping = true; commandWake->notify(); audioWake.signal(jam2::monotonic_us());
        receiverChanged.notify_all();
        if (worker.joinable()) worker.join();
    }
    rv::Message fields(SignalKind kind, const std::shared_ptr<Link>& link = {})
    {
        rv::Message object; object.kind = kind; object.session = config.sessionHex;
        object.key = config.keyHex; object.peer = config.localToken;
        if (config.creator) object.directoryRevision = directoryRevision;
        if (kind == SignalKind::Create) object.nonce = registrationChallenge;
        if (link) {
            object.nonce = link->request; object.attempt = static_cast<quint8>(link->attempt);
            object.introduction = link->introduction;
            if (kind == SignalKind::Candidate && !localHost.empty())
                object.local = rv::Endpoint::from(QString::fromStdString(localHost), link->socket->local_endpoint().port);
        }
        return object;
    }
    void send(const std::shared_ptr<jam2::UdpSocket>& socket, const rv::Message& object)
    {
        const auto packet = rv::encode(object);
        if (packet.isEmpty()) { ++counters.sendErrors; return; }
        if (socket->send_to(server, view(packet)).outcome == jam2::UdpSendOutcome::Sent) ++counters.signalPacketsSent;
        else ++counters.sendErrors;
    }
    void receiveSignalBytes(const QByteArray& bytes, quint64 now)
    {
        const auto message = rv::decode(bytes);
        if (!message || rv::request(message->kind)) { ++counters.signalPacketsRejected; return; }
        receiveSignal(*message, now);
    }
    void configureSocket(jam2::UdpSocket& socket) const {
        if (config.socketBuffers.sendBytes) socket.set_send_buffer_size(*config.socketBuffers.sendBytes);
        if (config.socketBuffers.receiveBytes) socket.set_recv_buffer_size(*config.socketBuffers.receiveBytes);
    }
    std::shared_ptr<jam2::UdpSocket> candidate()
    {
        for (int index = 0; index < 100; ++index) {
            auto socket = std::make_shared<jam2::UdpSocket>();
            socket->bind({config.bindHost.toStdString(), 0});
            configureSocket(*socket);
            if (usedPorts.insert(socket->local_endpoint().port).second) return socket;
        }
        throw std::runtime_error("Could not allocate an unused UDP candidate port");
    }
    void ackPair(const rv::Message& message)
    {
        auto object = fields(SignalKind::PairAck);
        object.nonce = message.nonce; object.attempt = message.attempt; object.introduction = message.introduction;
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
            if (!rendezvous()) {
                if (command.kind==CommandKind::Recover) { if (command.a==coordinator) { directJoinAttempts=0; restartDirectJoin(now,true); } continue; }
                if (command.kind==CommandKind::PrepareRecovery || command.kind==CommandKind::RegisterMember) continue;
                if (command.kind==CommandKind::Introduce) {
                    if (directCoordinator) directCoordinator->introduce(command.a,command.b,command.request,now);
                    continue;
                }
                if (command.kind==CommandKind::Admit) {
                    if (config.creator) {
                        admitted.insert(command.a); pendingJoins.erase(command.a);
                        const auto found=links.find(command.a);
                        if (found!=links.end() && directCoordinator)
                            directCoordinator->admit(command.a,PeerBootstrapEndpoint::from(jam2::format_udp_endpoint(found->second->remote)));
                    }
                    continue;
                }
                if (command.kind==CommandKind::Reject) {
                    pendingJoins.erase(command.a);
                    const auto found=links.find(command.a);
                    if (found!=links.end() && !admitted.contains(command.a)) {
                        for (const auto& [channel,connection] : found->second->streams) { (void)channel; connection->retire(); }
                        links.erase(found);
                    }
                    continue;
                }
                if (command.kind==CommandKind::Remove && directCoordinator) directCoordinator->remove(command.a);
            }
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
                if (config.creator && rendezvous()) {
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
    void receiveSignal(const rv::Message& message, quint64 now)
    {
        if (message.to != config.localToken || message.session != config.sessionHex) return;
        ++counters.signalPacketsReceived;
        if (token(message.creator)) coordinator = message.creator;
        const auto kind = message.kind; const auto peer = message.peer, request = message.nonce;
        if (config.creator && (kind == SignalKind::AdmitAck || kind == SignalKind::RemoveAck || kind == SignalKind::RejectAck)) {
            const auto action = kind == SignalKind::AdmitAck ? CommandKind::Admit
                : kind == SignalKind::RemoveAck ? CommandKind::Remove : CommandKind::Reject;
            const auto pending = membershipActions.find({action, peer});
            if (pending != membershipActions.end() && pending->second.sequence == message.memberSequence)
                membershipActions.erase(pending);
            return;
        }
        if (kind == SignalKind::CreateChallenge && config.creator) {
            if (message.directoryRevision == directoryRevision && token(message.nonce)) {
                registrationChallenge = message.nonce; nextRegister = 0;
            }
            return;
        }
        if (kind == SignalKind::Created) {
            if (message.directoryRevision != directoryRevision) return;
            const bool first = !registered;
            if (!paused) { registered = true; nextRegister = now + 15000000; }
            if (first) post([](auto& self) { if (self.onRegistered) self.onRegistered(); });
            return;
        }
        if (kind == SignalKind::Paused) {
            if (message.directoryRevision == directoryRevision) pauseConfirmed = true;
            return;
        }
        if (kind == SignalKind::Wait) { registeringMember = false; return; }
        if (kind == SignalKind::JoinRequest && config.creator && token(peer)) {
            if (pendingJoins.contains(peer)) return;
            pendingJoins.insert(peer);
            post([peer](auto& self) { if (self.onJoinRequest) self.onJoinRequest(peer); }); return;
        }
        if (kind == SignalKind::Error) {
            log(rv::errorText(message.error));
            if (joining && now >= joinDeadline && !warningPosted) {
                warningPosted = true; joining = false; failure(config.localToken, coordinator, rv::errorText(message.error));
            }
            return;
        }
        if (kind == SignalKind::Connected || kind == SignalKind::Failed) {
            ackPair(message);
            if (!terminalRequests.insert(request).second) return;
            terminalOrder.push_back(request);
            if (terminalOrder.size() > 128) { terminalRequests.erase(terminalOrder.front()); terminalOrder.pop_front(); }
            requests.erase(request);
            if (const auto found = links.find(peer); found != links.end() && found->second->request == request) {
                const auto link = found->second;
                if (message.introduction != link->introduction ||
                    message.attempt != static_cast<quint64>(link->attempt) || link->phase == Phase::Active || link->phase == Phase::Failed) return;
                if (kind == SignalKind::Connected) {
                    if (!link->gotHello || !link->gotAck) return;
                    activateLink(link,now);
                    log(QStringLiteral("UDP link active: %1, attempt %2, local port %3, route=%4, remote=%5").arg(peer).arg(link->attempt).arg(link->socket->local_endpoint().port).arg(link->route, QString::fromStdString(jam2::endpoint_to_string(jam2::format_udp_endpoint(link->remote)))));
                    return;
                }
                link->phase = Phase::Failed; link->socket.reset();
                pendingJoins.erase(peer);
            } else if (kind == SignalKind::Connected) return;
            const auto detail = message.attempt >= 10
                ? QStringLiteral("Direct UDP connection failed after 10 socket pairs; existing links remain active.")
                : QStringLiteral("UDP introduction did not complete; existing links remain active.");
            failure(message.a, message.b, detail);
            return;
        }
        if (kind == SignalKind::Prepare) {
            if (!token(peer) || !token(request) || peer == config.localToken || terminalRequests.contains(request)) return;
            const auto attempt = message.attempt; const auto intro = message.introduction;
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
        if (link->request != request || link->introduction != message.introduction ||
            static_cast<quint64>(link->attempt) != message.attempt ||
            link->phase == Phase::Active || link->phase == Phase::Failed) return;
        if (message.observed.valid())
            link->observed = jam2::resolve_udp_endpoint({message.observed.host().toStdString(), message.observed.port});
        if (kind == SignalKind::Offer && (link->phase == Phase::Preparing || link->phase == Phase::Offered)) {
            if (message.endpoint.valid()) {
                link->remote = jam2::resolve_udp_endpoint({message.endpoint.host().toStdString(), message.endpoint.port}); link->phase = Phase::Offered; link->nextAction = 0;
                link->route = message.lan ? QStringLiteral("lan") : QStringLiteral("public");
            }
        } else if (kind == SignalKind::Go && link->phase == Phase::Offered) {
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
        ++nativeReceived;
        if (config.receiveModel == ReceiveModel::Runtime) { ++bootstrapDiscarded; return; }
        {
            std::lock_guard lock(audioMutex);
            if (audioCount == audioQueue.size()) { ++audioDrops; return; }
            auto& slot = audioQueue[audioWrite]; slot.endpoint = link->remote; slot.size = packet.size();
            std::copy(packet.begin(), packet.end(), slot.bytes.begin());
            audioWrite = (audioWrite + 1) % audioQueue.size(); ++audioCount;
        }
        if (packet.size() >= jam2::protocol::kHeaderSize && packet[5] == static_cast<std::uint8_t>(jam2::protocol::PacketType::Audio))
            link->mediaStarted = true;
        audioWake.signal(jam2::monotonic_us());
    }
    bool muxSend(const std::shared_ptr<Link>& link, const QByteArray& packet)
    {
        if (!link->socket || link->phase != Phase::Active || packet.size() < 8) return false;
        const auto packetView=view(packet);
        const bool assetChannel = muxChannel(packetView)==UdpMuxChannel::Asset;
        const bool asset = assetChannel && (muxKind(packetView)==UdpMuxKind::Data || muxKind(packetView)==UdpMuxKind::DataAck);
        auto& budget = asset ? assetBudget : controlBudget;
        const auto charge = static_cast<double>(packet.size() + 28);
        // In uncapped mode, asset ACKs must not consume the GUI control
        // budget or impose a second, hidden WAV throughput cap.
        const bool limited = !assetChannel || config.assetBytesPerSecond != 0;
        if (limited && budget < charge) { ++counters.budgetDeferrals; return false; }
        if (link->socket->send_to(link->remote, view(packet)).outcome != jam2::UdpSendOutcome::Sent) { ++counters.sendErrors; return false; }
        if (limited) budget -= charge;
        (asset ? counters.assetBytesSent : counters.controlBytesSent) += static_cast<quint64>(packet.size());
        return true;
    }
    jam2::UdpSendResult sendNative(const std::shared_ptr<Link>& link,const std::shared_ptr<jam2::UdpSocket>& socket,
        const jam2::ResolvedUdpEndpoint& endpoint,std::span<const std::uint8_t> packet) noexcept
    {
        if (config.acknowledgeNativeIntents && packet.size()==jam2::protocol::kHeaderSize+28 &&
            packet[5]==static_cast<std::uint8_t>(jam2::protocol::PacketType::TransportState)) {
            const auto payload=packet.subspan(jam2::protocol::kHeaderSize);
            std::lock_guard intent(link->intentMutex);
            link->intentUsed=true;
            if (!link->allowIntentReceipts) {
                const auto result=socket->send_to(endpoint,packet); if (result.outcome==jam2::UdpSendOutcome::Sent) ++nativeSent; return result;
            }
            const bool same=link->intentSent && std::equal(payload.begin(),payload.end(),link->lastIntent.begin());
            if (same && link->intentAcknowledged) { ++intentRepeatsSuppressed; return {}; }
            if (!same) { std::copy(payload.begin(),payload.end(),link->lastIntent.begin()); link->intentSent=true; link->intentAcknowledged=false; }
        }
        const auto result=socket->send_to(endpoint,packet);
        if (result.outcome==jam2::UdpSendOutcome::Sent) ++nativeSent;
        return result;
    }
    std::shared_ptr<UdpStreamConnection> stream(const std::shared_ptr<Link>& link, UdpMuxChannel channel,
                                               quint64 id, bool incoming,quint16 handle=0)
    {
        if (!incoming && config.compactFrames && link->nextCompactHandle<=UINT16_MAX) handle=static_cast<quint16>(link->nextCompactHandle++);
        if (const auto old=link->streams.find(channel);old!=link->streams.end()) link->compactStreams.erase(old->second->compactHandle());
        UdpStreamConnection::Options options; options.packUnsent=config.packUnsentFragments; options.fastRecovery=config.fastRecovery;
        options.fastReorderUs=config.fastReorderUs; options.combineAcks=config.combineAcks; options.compactFrames=config.compactFrames;
        auto connection = std::make_shared<UdpStreamConnection>(session, link->introduction, id, channel,
            key, QString::fromStdString(jam2::format_udp_endpoint(link->remote).host), incoming,
            options,handle);
        if (connection->compactHandle()) link->compactStreams[connection->compactHandle()]=connection;
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
    void muxReceive(const std::shared_ptr<Link>& link, std::span<const std::uint8_t> packet, quint64 now)
    {
        counters.muxCopyBytesAvoided += packet.size();
        if (isCompactUdpMux(packet)) {
            const auto found=link->compactStreams.find(compactUdpMuxHandle(packet));
            const auto connection=found==link->compactStreams.end() ? nullptr : found->second.lock();
            if (!connection || connection->terminal() || !connection->receiveDatagram(packet,now,[&](const QByteArray& outgoing) { return muxSend(link,outgoing); })) {
                ++counters.invalidMuxPackets; return;
            }
            const bool asset=muxChannel(packet)==UdpMuxChannel::Asset && (muxKind(packet)==UdpMuxKind::Data || muxKind(packet)==UdpMuxKind::DataAck);
            (asset ? counters.assetBytesReceived : counters.controlBytesReceived)+=packet.size();
            if (!config.creator && connection->isConnected()) notifyStream(link,connection,false);
            return;
        }
        const auto frame = decodeUdpMuxView(packet, session, key);
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
        if (frame->kind==UdpMuxKind::IntentAck) {
            counters.controlBytesReceived+=packet.size();
            std::lock_guard intent(link->intentMutex);
            if (frame->stream!=1 || !config.acknowledgeNativeIntents || !link->allowIntentReceipts || !link->intentSent || !std::equal(frame->payload.begin(),frame->payload.end(),link->lastIntent.begin())) ++intentAckInvalid;
            else { link->intentAcknowledged=true; ++intentAcksReceived; }
            return;
        }
        if (!(config.creator || link->peer == coordinator)) { ++counters.invalidMuxPackets; return; }
        counters.muxCopyBytesAvoided += frame->payload.size();
        const bool assetData = frame->channel == UdpMuxChannel::Asset && (frame->kind == UdpMuxKind::Data || frame->kind == UdpMuxKind::DataAck);
        (assetData ? counters.assetBytesReceived : counters.controlBytesReceived) += static_cast<quint64>(packet.size());
        const auto logicalChannel = frame->channel == UdpMuxChannel::IndependentControl ? UdpMuxChannel::Control : frame->channel;
        auto found = link->streams.find(logicalChannel);
        if (frame->kind == UdpMuxKind::Open && config.creator) {
            const auto handle=frame->payload.size()==2 ? qFromBigEndian<quint16>(frame->payload.data()) : 0;
            if (config.compactFrames && !handle) { ++counters.invalidMuxPackets; return; }
            if (handle) {
                const auto existing=link->compactStreams.find(handle);
                if (existing!=link->compactStreams.end()) if (const auto bound=existing->second.lock();bound && bound->streamId()!=frame->stream) {
                    ++counters.invalidMuxPackets; return;
                }
            }
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
                auto connection = stream(link, frame->channel, frame->stream, true,handle);
                notifyStream(link, connection, true);
                found = link->streams.find(frame->channel);
            }
        }
        if (found == link->streams.end() || found->second->streamId() != frame->stream) return;
        found->second->receiveView(*frame, now, [&](const QByteArray& outgoing) { return muxSend(link, outgoing); });
        if (!config.creator && found->second->isConnected()) notifyStream(link, found->second, false);
    }
    void readSockets(quint64 now, WorkerScratch& scratch, bool draining = false)
    {
        std::unique_lock reader(receiverMutex,std::try_to_lock);
        std::array<std::uint8_t,1500> buffer{};
        auto& seen=scratch.seenSockets; seen.clear();
        auto read=[&](const std::shared_ptr<jam2::UdpSocket>& socket) {
            if (!socket || std::find(seen.begin(),seen.end(),socket.get())!=seen.end()) return;
            seen.push_back(socket.get());
            const auto routes=std::atomic_load_explicit(&readRoutes, std::memory_order_acquire);
            const bool activeSocket=std::any_of(routes->routes.begin(),routes->routes.end(),[&](const ReadRoute& route) { return route.socket==socket; });
            if (activeSocket && ((!draining && receiverOwned.load(std::memory_order_acquire)) || !reader.owns_lock())) return;
            for (int count=0;count<32;++count) {
                const auto received=socket->recv_from_for(buffer,0);
                if (!received) break;
                const auto packet=std::span<const std::uint8_t>(buffer.data(),received->size);
                if (stunRequest && stunEndpoint && received->endpoint==*stunEndpoint) {
                    try {
                        counters.bootstrapPublic=jam2::stun::parse_binding_response(packet,stunRequest->transaction_id);
                        registered=true; stunRequest.reset(); stunEndpoint.reset();
                        post([](auto& self) { if (self.onRegistered) self.onRegistered(); });
                    } catch (const std::exception&) { ++counters.stunResponsesRejected; }
                    continue;
                }
                if (!rendezvous() && isPeerBootstrap(packet)) { receiveBootstrap(packet,received->endpoint,now); continue; }
                if (rendezvous() && received->endpoint==server) { receiveSignalBytes(bytes(packet),now); continue; }
                std::shared_ptr<Link> selected;
                for (const auto* collection : {&links,&retainedLinks}) {
                    for (const auto& [peer,link] : *collection) {
                        (void)peer;
                        if (link->socket==socket && link->remote==received->endpoint) { selected=link; break; }
                    }
                    if (selected) break;
                }
                if (!selected) continue;
                selected->lastReceive=now; selected->stalledNotified=false;
                if (selected->phase!=Phase::Active) directProof(selected,packet,received->endpoint);
                else if (packet.size()>=4 && std::equal(packet.begin(),packet.begin()+4,"J2MX")) muxReceive(selected,packet,now);
                else if (packet.size()>=4 && std::equal(packet.begin(),packet.begin()+4,"JAM2")) queueAudio(selected,packet);
            }
        };
        read(signalSocket);
        auto& current=scratch.readLinks; current.clear();
        for (const auto& [peer,link] : links) { (void)peer; current.push_back(link); }
        for (const auto& [peer,link] : retainedLinks) { (void)peer; current.push_back(link); }
        for (const auto& link : current) read(link->socket);
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
    void timers(quint64 now, WorkerScratch& scratch)
    {
        immediateSendWork = false;
        if (lastBudget) {
            const auto seconds = static_cast<double>(now - lastBudget) / 1000000.0;
            controlBudget = (std::min)(4096.0, controlBudget + seconds * config.controlBytesPerSecond);
            assetBudget = (std::min)(4096.0, assetBudget + seconds * config.assetBytesPerSecond);
        }
        lastBudget = now;
        if (stunRequest && now>=stunDeadline) {
            if (stunAttempts>=config.stunRetries) {
                stunRequest.reset(); stunEndpoint.reset();
                post([](auto& self) { if (self.onBootstrapFailure) self.onBootstrapFailure(QStringLiteral("STUN endpoint discovery timed out on the creator UDP socket")); });
            } else {
                const auto result=signalSocket->send_to(*stunEndpoint,stunRequest->bytes);
                if (result.outcome==jam2::UdpSendOutcome::Sent) ++counters.stunRequestsSent;
                else ++counters.sendErrors;
                ++stunAttempts; stunDeadline=now+static_cast<quint64>(config.stunTimeoutMs)*1000;
            }
        }
        if (rendezvous()) {
        if (config.creator) {
            const bool full = config.maximumPeers > 0 && admitted.size() >= static_cast<std::size_t>(config.maximumPeers);
            if (full && !pending() && !paused) {
                ++directoryRevision;
                send(signalSocket, fields(SignalKind::Pause)); paused = true; registered = false;
                pauseConfirmed = false; nextPause = now + 500000; pauseDeadline = now + 8000000;
                log(QStringLiteral("Jam full; rendezvous keepalives paused"));
            }
            if (paused && !pauseConfirmed && now >= nextPause && now < pauseDeadline) {
                send(signalSocket, fields(SignalKind::Pause)); nextPause = now + 500000;
            }
            if (!full && paused) { ++directoryRevision; paused = false; registered = false; nextRegister = 0; }
            if (!paused && now >= nextRegister) {
                auto object = fields(SignalKind::Create);
                object.maximumPeers = config.maximumPeers;
                object.memberCount = static_cast<int>(admitted.size());
                send(signalSocket, object); nextRegister = now + (registered ? 15000000 : 500000);
            }
        } else if (joining && now >= nextJoin) {
            if (now >= joinDeadline) {
                joining = false; failure(config.localToken, coordinator, QStringLiteral("Rendezvous server or creator did not accept the join"));
            } else { send(signalSocket, fields(recoveryJoin ? SignalKind::Recover : SignalKind::Join)); nextJoin = now + 500000; }
        }
        if (registeringMember && now >= memberDeadline) {
            registeringMember = false;
            log(QStringLiteral("Temporary mesh registration timed out; existing links remain active"));
        }
        if (registeringMember && now >= nextMember) {
            send(signalSocket, fields(SignalKind::Member)); nextMember = now + 500000;
        }
        for (auto it = membershipActions.begin(); it != membershipActions.end();) {
            if (now >= it->second.deadline) {
                log(QStringLiteral("Directory membership acknowledgement timed out; established links remain active"));
                it = membershipActions.erase(it); continue;
            }
            if (now >= it->second.nextSend) {
                const auto kind = it->first.first;
                auto object = fields(kind == CommandKind::Admit ? SignalKind::Admit : kind == CommandKind::Remove ? SignalKind::Remove : SignalKind::Reject);
                object.to = it->first.second;
                object.memberSequence = it->second.sequence;
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
                auto object = fields(SignalKind::Pair);
                object.a = request.a; object.b = request.b;
                object.nonce = request.nonce;
                send(signalSocket, object); request.nextSend = now + 500000;
            }
            ++it;
        }
        } else {
            if (!config.creator && joining && now>=nextJoin) {
                if (now>=joinDeadline) { bootstrapFailure(QStringLiteral("Direct creator admission timed out")); }
                else {
                    if (now>=directAttemptDeadline) restartDirectJoin(now,true);
                    if (joining) {
                        PeerBootstrapMessage join; join.kind=PeerBootstrapKind::Join; join.request=directJoinRequest;
                        sendBootstrap(signalSocket,server,std::move(join)); nextJoin=now+500000;
                    }
                }
            }
            if (directCoordinator) {
                for (auto& transmission : directCoordinator->poll(now))
                    sendBootstrap(signalSocket,jam2::resolve_udp_endpoint(transmission.to.endpoint()),std::move(transmission.message));
                for (const auto& failed : directCoordinator->takeFailures())
                    failure(failed.a,failed.b,QStringLiteral("Peer introduction failed after ten fresh socket pairs; healthy links remain active"));
                counters.introductionPending=directCoordinator->pending();
            }
        }
        // Failed unauthenticated attempts must not accumulate beyond the
        // bounded set currently proving/authenticating a creator connection.
        if (config.creator && !rendezvous()) std::erase_if(links,[&](const auto& entry) {
            const auto& link=entry.second;
            if (!link->hostLink || admitted.contains(entry.first)) return false;
            const bool expired=link->phase==Phase::Active && now>=link->deadline;
            if (expired) ++counters.pendingAdmissionTimeouts;
            if (link->phase!=Phase::Failed && !expired) return false;
            for (const auto& [channel,connection] : link->streams) { (void)channel; connection->retire(); }
            pendingJoins.erase(entry.first);
            return true;
        });
        auto& ordered = scratch.ordered; ordered.clear();
        for (const auto& [peer, link] : links) { (void)peer; ordered.push_back(link); }
        for (const auto& [peer, link] : retainedLinks) { (void)peer; ordered.push_back(link); }
        if (!ordered.empty()) cursor = (cursor + 1) % ordered.size();
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto link = ordered[(index + cursor) % ordered.size()];
            if (link->phase == Phase::Active) {
                if (config.acknowledgeNativeIntents) {
                    std::array<std::uint8_t,28> intent{}; bool pending=false;
                    { std::lock_guard lock(link->intentMutex); pending=link->hasPendingIntentAck; if (pending) intent=link->pendingIntentAck; }
                    if (pending) {
                        if (muxSend(link,encodeUdpMuxView({UdpMuxKind::IntentAck,UdpMuxChannel::Control,session,link->introduction,1,0,intent},key))) {
                            ++intentAcksSent;
                            std::lock_guard lock(link->intentMutex);
                            if (intent==link->pendingIntentAck) link->hasPendingIntentAck=false;
                        } else ++intentAckDeferrals;
                    }
                }
                // The audio runtime already reprobes the proven socket after
                // silence. Escalate a persistent outage to its coordinator.
                const auto lastReceived = link->lastReceive.load(std::memory_order_relaxed);
                if (link->mediaStarted && lastReceived && now >= lastReceived && now - lastReceived >= config.linkRepairSilenceMs * 1000 && !link->stalledNotified) {
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
                    const auto sent = connection->poll(now, 2, [&](const QByteArray& packet) { return muxSend(link, packet); });
                    // Aggregated ACKs must not become an artificial send clock.
                    // Continue existing credit in another bounded worker turn.
                    if (sent==2 && connection->hasSendCredit()) immediateSendWork = true;
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
            if (!rendezvous() && link->hostLink && config.creator && now>=link->nextBootstrap) {
                auto welcome=bootstrapMessage(PeerBootstrapKind::Welcome,link);
                welcome.observed=PeerBootstrapEndpoint::from(jam2::format_udp_endpoint(link->remote));
                sendBootstrap(signalSocket,link->remote,std::move(welcome)); link->nextBootstrap=now+500000;
            }
            if ((link->phase == Phase::Preparing || link->phase == Phase::Offered) && now >= link->deadline) {
                link->phase = Phase::Failed; link->socket.reset(); pendingJoins.erase(link->peer);
                failure(config.localToken, link->peer, QStringLiteral("UDP candidate registration or READY handoff timed out")); continue;
            }
            if (link->phase == Phase::Preparing && now >= link->nextAction) {
                if (rendezvous()) send(link->socket, fields(SignalKind::Candidate, link));
                else {
                    auto candidateMessage=bootstrapMessage(PeerBootstrapKind::Candidate,link);
                    candidateMessage.local=PeerBootstrapEndpoint::from({localHost,link->socket->local_endpoint().port});
                    sendBootstrap(link->socket,server,std::move(candidateMessage));
                }
                link->nextAction = now + 500000;
            } else if (link->phase == Phase::Offered && now >= link->nextAction) {
                if (rendezvous()) send(link->socket, fields(SignalKind::Ready, link));
                else sendBootstrap(link->socket,server,bootstrapMessage(PeerBootstrapKind::Ready,link));
                link->nextAction = now + 500000;
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
                if (!rendezvous()) {
                    if (link->hostLink) {
                        if (!link->report || now>=link->deadline) {
                            link->phase=Phase::Failed;
                            if (!config.creator) restartDirectJoin(now,true);
                            else failure(config.localToken,link->peer,QStringLiteral("Direct creator return-path proof timed out"));
                        } else if (config.creator && link->remoteReported) {
                            activateLink(link,now);
                            sendBootstrap(signalSocket,link->remote,bootstrapMessage(PeerBootstrapKind::Accepted,link));
                        } else if (!config.creator && now>=link->nextAction) {
                            sendBootstrap(link->socket,server,bootstrapMessage(PeerBootstrapKind::JoinReady,link));
                            link->nextAction=now+500000;
                        }
                    } else if (now>=link->deadline) {
                        link->phase=Phase::Failed;
                        failure(config.localToken,link->peer,QStringLiteral("Peer introduction completion timed out"));
                    } else if (now>=link->nextAction) {
                        auto outcome=bootstrapMessage(PeerBootstrapKind::Outcome,link); outcome.success=link->report;
                        sendBootstrap(link->socket,server,std::move(outcome)); link->nextAction=now+500000;
                    }
                    continue;
                }
                if (now >= link->deadline) {
                    link->phase = Phase::Failed; link->socket.reset(); pendingJoins.erase(link->peer);
                    failure(config.localToken, link->peer, QStringLiteral("UDP attempt result synchronization timed out"));
                } else if (now >= link->nextAction) {
                    auto object = fields(SignalKind::Outcome, link); object.success = link->report;
                    send(link->socket, object); link->nextAction = now + 500000;
                }
            }
        }
    }
    void run()
    {
        try {
            if (!rendezvous()) {
                if (config.creator) {
                    auto advertised=config.publicEndpoint.value_or(jam2::Endpoint{localHost,signalSocket->local_endpoint().port});
                    const bool discover=config.strategy==jam2::ConnectionStrategy::Stun && !config.publicEndpoint;
                    const auto discoveryEndpoint=discover ? std::optional{jam2::resolve_udp_endpoint(config.stunServer)} : std::nullopt;
                    {
                        std::lock_guard lock(mutex);
                        if (discover) {
                            stunEndpoint=discoveryEndpoint;
                            stunRequest=jam2::stun::make_binding_request();
                        } else { counters.bootstrapPublic=advertised; registered=true; }
                    }
                    if (registered) post([](auto& self) { if (self.onRegistered) self.onRegistered(); });
                } else { std::lock_guard lock(mutex); restartDirectJoin(jam2::monotonic_us(),false); }
            }
            quint64 previousCycle = 0;
            while (!stopping) {
                WorkerScratch fresh;
                auto& scratch = config.reuseWorkerBuffers ? workerScratch : fresh;
                const auto previousCapacity = scratch.capacity();
                auto& keepAlive = scratch.keepAlive; keepAlive.clear();
                auto& sockets = scratch.sockets; sockets.clear();
                quint64 wait = 10000;
                const auto lockStart = jam2::monotonic_us();
                {
                    std::lock_guard lock(mutex);
                    const auto now = jam2::monotonic_us();
                    counters.workerLockWaitMaxUs = qMax(counters.workerLockWaitMaxUs, now - lockStart);
                    if (previousCycle)
                        counters.workerLoopGapMaxUs = qMax(counters.workerLoopGapMaxUs, now - previousCycle);
                    previousCycle = now;
                    updateReceiverOwnership();
                    processCommands(now); processIngress(); readSockets(now,scratch); timers(now,scratch); publishReadRoutes();
                    if (ingressPending() || immediateSendWork) wait = 0;
                    if (immediateSendWork) ++counters.workerSendRepolls;
                    const bool signalOwned=signalRuntimeOwned();
                    const auto keep=[&](const std::shared_ptr<jam2::UdpSocket>& socket) {
                        if (socket && !(signalOwned && socket==signalSocket) &&
                            std::find(keepAlive.begin(),keepAlive.end(),socket)==keepAlive.end()) keepAlive.push_back(socket);
                    };
                    keep(signalSocket);
                    for (const auto& [peer, link] : links) { (void)peer; if (link->socket && !(receiverOwned.load() && link->phase == Phase::Active)) keep(link->socket); }
                    for (const auto& [peer, link] : retainedLinks) { (void)peer; if (link->socket && !(receiverOwned.load() && link->phase == Phase::Active)) keep(link->socket); }
                    for (const auto& socket : keepAlive) sockets.push_back(socket.get());
                    const auto serviceUs = jam2::monotonic_us() - now;
                    counters.workerServiceMaxUs = qMax(counters.workerServiceMaxUs, serviceUs);
                    counters.workerServiceSumUs += serviceUs; ++counters.workerTurns;
                    if (scratch.capacity() != previousCapacity) ++counters.workerWorkspaceGrowths;
                }
                jam2::UdpSocket::waitForAny(sockets, wait, &commandWake->signal);
            }
            // Stop wakes the receiver; wait for its final bounded socket wait
            // before the worker resumes reading active sockets for close/leave.
            { std::lock_guard reader(receiverMutex); }
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
                    processIngress(); readSockets(now,workerScratch,true);
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
            if (config.creator && rendezvous()) { ++directoryRevision; send(signalSocket, fields(SignalKind::Leave)); }
        } catch (const std::exception& exception) {
            const auto detail = QString::fromUtf8(exception.what());
            if (!rendezvous()) post([detail](auto& self) { if (self.onBootstrapFailure) self.onBootstrapFailure(detail); });
            else failure(config.localToken, coordinator, detail);
        }
        stopping = true; audioWake.signal(jam2::monotonic_us());
        receiverChanged.notify_all();
    }
};

UdpMultiplexTransport::UdpMultiplexTransport(QObject* parent) : QObject(parent) {}
UdpMultiplexTransport::~UdpMultiplexTransport() { stop(); }
bool UdpMultiplexTransport::start(const Config& config, QString& error)
{
    stop(); error.clear();
    try {
        impl_ = std::make_unique<Impl>(this, config);
        impl_->worker = std::thread([state = impl_.get()] { state->run(); });
        return true;
    } catch (const std::exception& exception) { impl_.reset(); error = QString::fromUtf8(exception.what()); return false; }
}
void UdpMultiplexTransport::stop() { if (impl_) impl_->stop(); }
std::optional<jam2::NetworkTransportInfo> UdpMultiplexTransport::transportInfo() const
{
    if (!impl_) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || !impl_->signalSocket) return std::nullopt;
    return jam2::NetworkTransportInfo{impl_->signalSocket->local_endpoint(),
        impl_->signalSocket->send_buffer_size(), impl_->signalSocket->recv_buffer_size()};
}

void UdpMultiplexTransport::receiverAttached()
{
    if (!impl_) throw std::runtime_error("UDP multiplex transport has not started");
    impl_->attachReceiver(); // Reject a duplicate owner before changing receipt state.
    { std::lock_guard lock(impl_->mutex);
        for (const auto* links : {&impl_->links,&impl_->retainedLinks}) for (const auto& [peer,link] : *links) {
            (void)peer; std::lock_guard intent(link->intentMutex);
            // Native event counters can restart with the runtime. Keep legacy
            // repeats on an already-used physical epoch until fresh link proof.
            if (link->intentUsed) link->allowIntentReceipts=false;
            link->intentAcknowledged=false; link->intentSent=false; link->hasPendingIntentAck=false;
        }
    }
}
void UdpMultiplexTransport::receiverDetached() noexcept { if (impl_) impl_->detachReceiver(); }
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
std::optional<jam2::Endpoint> UdpMultiplexTransport::bootstrapEndpoint() const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    if (!impl_->registered || impl_->counters.bootstrapPublic.port==0) return {};
    return impl_->counters.bootstrapPublic;
}
UdpMultiplexTransport::Snapshot UdpMultiplexTransport::snapshot() const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->counters;
    result.requestedBuffers=impl_->config.socketBuffers;
    result.pendingAdmissionTimeoutMs=impl_->config.pendingAdmissionTimeoutMs;
    std::set<const jam2::UdpSocket*> physical;
    if (impl_->signalSocket) {
        physical.insert(impl_->signalSocket.get());
        result.signalSendBufferBytes=impl_->signalSocket->send_buffer_size();
        result.signalReceiveBufferBytes=impl_->signalSocket->recv_buffer_size();
    }
    for (const auto* links : {&impl_->links,&impl_->retainedLinks})
        for (const auto& [peer,link] : *links) { (void)peer; if (link->socket) physical.insert(link->socket.get()); }
    result.physicalSocketCount=physical.size();
    result.reusesWorkerBuffers = impl_->config.reuseWorkerBuffers;
    result.compactFrames=impl_->config.compactFrames; result.combineAcks=impl_->config.combineAcks;
    result.fastRecovery=impl_->config.fastRecovery; result.acknowledgeNativeIntents=impl_->config.acknowledgeNativeIntents;
    result.fastReorderUs=impl_->config.fastReorderUs;
    result.running = !impl_->stopping; result.registered = impl_->registered; result.admissionPaused = impl_->paused; result.pauseConfirmed = impl_->pauseConfirmed;
    result.audioQueueDrops = impl_->audioDrops;
    result.audioPacketsSent = impl_->nativeSent.load(); result.audioPacketsReceived = impl_->nativeReceived.load();
    result.nativeIntentAcksSent=impl_->intentAcksSent; result.nativeIntentAcksReceived=impl_->intentAcksReceived;
    result.nativeIntentRepeatsSuppressed=impl_->intentRepeatsSuppressed; result.nativeIntentAckDeferrals=impl_->intentAckDeferrals;
    result.nativeIntentAckInvalid=impl_->intentAckInvalid;
    result.runtimeReceiveEnabled = impl_->config.receiveModel == ReceiveModel::Runtime;
    result.runtimeReceiverAttached = impl_->receiverOwned.load();
    result.runtimeNativeDelivered = impl_->nativeDelivered.load(); result.bootstrapNativeDiscarded = impl_->bootstrapDiscarded.load();
    result.runtimeControlQueued = impl_->controlQueued.load(); result.runtimeAssetQueued = impl_->assetQueued.load();
    result.runtimeInvalidPackets = impl_->invalidClassified.load(); result.runtimeBudgetYields = impl_->classificationYields.load();
    result.classificationSamples = impl_->classificationSamples.load(); result.classificationSumNs = impl_->classificationSumNs.load();
    result.classificationMaxNs = impl_->classificationMaxNs.load();
    { std::lock_guard ingress(impl_->ingressMutex);
        result.ingressQueueDrops = impl_->ingressDrops; result.ingressHighWater = impl_->ingressHighWater;
        result.controlIngressDepth = impl_->controlIngress.count; result.assetIngressDepth = impl_->assetIngress.count;
    }
    for (const auto& [peer, link] : impl_->links) {
        LinkSnapshot value; value.peer = peer; value.request = link->request; value.phase = link->phase;
        { std::lock_guard intent(link->intentMutex);
            value.nativeIntentSent=link->intentSent; value.nativeIntentAcknowledged=link->intentAcknowledged;
            value.nativeIntentReceiptsAllowed=link->allowIntentReceipts;
            if (!link->allowIntentReceipts) ++result.intentReceiptDisabledLinks;
        }
        value.route = link->route;
        value.attempt = link->attempt; value.helloSent = link->helloSent; value.introduction = link->introduction;
        if (link->socket) {
            value.local = link->socket->local_endpoint();
            value.sendBufferBytes=link->socket->send_buffer_size();
            value.receiveBufferBytes=link->socket->recv_buffer_size();
        }
        if (link->remote.port) value.remote = jam2::format_udp_endpoint(link->remote);
        if (link->observed.port) value.observed = jam2::format_udp_endpoint(link->observed);
        value.receivedHello = link->gotHello; value.receivedAck = link->gotAck;
        const auto lastReceived = link->lastReceive.load(); const auto now = jam2::monotonic_us();
        if (lastReceived && now >= lastReceived) value.lastReceiveAgeMs = (now - lastReceived) / 1000;
        if (const auto held = impl_->retainedLinks.find(peer); held != impl_->retainedLinks.end() && held->second->socket)
            value.retainedAudioLocal = held->second->socket->local_endpoint();
        for (const auto& [channel, connection] : link->streams) {
            (channel == UdpMuxChannel::Asset ? value.assetQueued : value.controlQueued) += static_cast<quint64>(connection->bytesToWrite());
            const auto streamStats = connection->stats();
            value.retransmissions += streamStats.retransmitted;
            value.inputDeferrals += connection->inputDeferrals();
            value.ackDatagramsSent += streamStats.ackDatagramsSent; value.acksCoalesced += streamStats.acksCoalesced;
            value.ackTransmitDeferrals += streamStats.ackTransmitDeferrals; value.invalidAckWindows += streamStats.invalidAckWindows;
            value.inOrderDirect += streamStats.in_order_direct; value.reorderInsertions += streamStats.reorder_insertions;
            value.tailBytesPacked += streamStats.tail_bytes_packed; value.fragmentsAvoided += streamStats.fragments_avoided;
            value.fastRetransmissions+=streamStats.fast_retransmitted; value.sackGapReports+=streamStats.sack_gap_reports;
            value.ackPairs+=streamStats.ackPairs; value.piggybackedAcks+=streamStats.piggybackedAcks;
            value.compactPackets+=streamStats.compactPackets; value.compactBytesSaved+=streamStats.compactBytesSaved;
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
    if (impl_->config.receiveModel == ReceiveModel::Runtime) {
        const auto routes = std::atomic_load_explicit(&impl_->readRoutes, std::memory_order_acquire);
        for (const auto& route : routes->routes) if (route.peer == id && route.link->readable.load(std::memory_order_acquire)) {
            return impl_->sendNative(route.link,route.socket,route.endpoint,packet);
        }
        return {jam2::UdpSendOutcome::WouldBlock, 0};
    }
    std::lock_guard lock(impl_->mutex);
    auto selected = impl_->links.end();
    for (auto it = impl_->links.begin(); it != impl_->links.end(); ++it)
        if (peerId(it->first) == id) { selected = it; break; }
    std::shared_ptr<Impl::Link> link;
    if (selected != impl_->links.end() && selected->second->phase == Phase::Active) link = selected->second;
    else for (const auto& [peer, held] : impl_->retainedLinks)
        if (peerId(peer) == id && held->phase == Phase::Active) { link = held; break; }
    if (link && link->socket) {
        return impl_->sendNative(link,link->socket,link->remote,packet);
    }
    return {jam2::UdpSendOutcome::WouldBlock, 0};
}
void UdpMultiplexTransport::transportIntentAccepted(std::uint64_t id,std::span<const std::uint8_t> payload) noexcept
{
    if (!impl_ || impl_->stopping || !impl_->config.acknowledgeNativeIntents || payload.size()!=28) return;
    const auto accepted=[&](const std::shared_ptr<Impl::Link>& link) {
        std::lock_guard intent(link->intentMutex); link->intentUsed=true;
        if (!link->allowIntentReceipts) return;
        std::copy(payload.begin(),payload.end(),link->pendingIntentAck.begin()); link->hasPendingIntentAck=true;
        impl_->commandWake->notify();
    };
    try {
        const auto routes=std::atomic_load_explicit(&impl_->readRoutes, std::memory_order_acquire);
        for (const auto& route : routes->routes) if (route.peer==id && route.link->readable.load(std::memory_order_acquire)) { accepted(route.link); return; }
        if (impl_->config.receiveModel==ReceiveModel::QueuedWorker) {
            std::lock_guard lock(impl_->mutex);
            for (const auto* links : {&impl_->links,&impl_->retainedLinks}) for (const auto& [peer,link] : *links)
                if (peerId(peer)==id && link->phase==Phase::Active) { accepted(link); return; }
        }
    } catch (...) { ++impl_->intentAckDeferrals; }
}
std::optional<jam2::UdpSocket::ReceivedDatagram> UdpMultiplexTransport::receiveFor(
    std::span<std::uint8_t> buffer, std::uint64_t timeoutUs, jam2::RealtimeWakeSignal* captureWake)
{
    if (!impl_) return {};
    if (impl_->config.receiveModel == ReceiveModel::Runtime) return impl_->receiveRuntime(buffer, timeoutUs, captureWake);
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
