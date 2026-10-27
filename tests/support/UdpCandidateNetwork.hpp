#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QUdpSocket>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>
#include <future>
#include <chrono>
#include <thread>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>
#include <vector>

class CandidateNetwork {
    static void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
    static QString peer(int i) { return QString(32, QChar('1' + i)); }
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
    const bool traceMux_ = qEnvironmentVariableIsSet("JAM2_TRACE_CANDIDATE_MUX");
    std::vector<QJsonObject> muxTrace_;
    void traceMux(const Route& from, const Route& to, const QByteArray& packet, bool wasDropped) {
        if (!traceMux_ || muxTrace_.size() >= 1024 || packet.size()<32 || !packet.startsWith("J2MX")) return;
        const bool compact=static_cast<quint8>(packet[5])&0x80;
        if (!compact && packet.size()<58) return;
        const auto u64 = [&](int offset) { return QString::number(qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(packet.constData()+offset))); };
        muxTrace_.push_back({{"from",from.peer},{"to",to.peer},{"local",to.local},{"from_mapping",from.socket.localPort()},
            {"kind",compact ? static_cast<quint8>(packet[5])&15 : static_cast<quint8>(packet[5])},
            {"channel",compact ? (static_cast<quint8>(packet[5])>>4)&3 : static_cast<quint8>(packet[6])},
            {"stream",compact ? QString::number(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(packet.constData()+6))) : u64(24)},
            {"compact",compact},{"sequence",u64(compact ? 8 : 32)},{"bytes",packet.size()},{"dropped",wasDropped},{"packet_index",static_cast<qint64>(directCount_)}});
    }
    int mediaProofDelayMs_ = 0;
    quint64 delayedMediaProofs_ = 0;
    bool authorityBeaconsEnabled_ = true;
    quint64 authorityBeaconIntervalMs_ = 0;
    std::map<std::pair<QString,QString>,std::chrono::steady_clock::time_point> authorityBeaconTimes_;
    quint64 authorityBeaconsForwarded_ = 0, authorityBeaconsDropped_ = 0;
    bool assignmentsEnabled_ = true;
    quint64 assignmentIntervalMs_ = 0, assignmentsForwarded_ = 0, assignmentsDropped_ = 0;
    struct AssignmentStamp { std::chrono::steady_clock::time_point time; quint64 revision=0; };
    std::map<std::pair<QString,QString>,AssignmentStamp> assignmentTimes_;
    bool assignmentAcksEnabled_ = true;
    quint64 assignmentAcksForwarded_ = 0, assignmentAcksDropped_ = 0;
    quint64 firstAssignmentsToDrop_ = 0;
    QString blockedAssignmentPeer_;
    quint64 recoveryRequestsToDrop_ = 0;
    std::chrono::steady_clock::time_point mediaProofUntil_;
    std::set<quint64> suppressedStreams;
    QString suppressedPeer;
    int suppressCount = 0;
    QString blockedA, blockedB;
    std::set<quint16> blockedPairPorts;
    std::set<QString> firstMembership;
public:
    struct AuthorityBeaconStats { quint64 forwarded=0,dropped=0; };
    void setAuthorityBeaconsEnabled(bool enabled) { authorityBeaconsEnabled_=enabled; }
    void setAuthorityBeaconInterval(quint64 milliseconds) { authorityBeaconIntervalMs_=milliseconds; authorityBeaconTimes_.clear(); }
    AuthorityBeaconStats authorityBeaconStats() const { return {authorityBeaconsForwarded_,authorityBeaconsDropped_}; }
    void setGridAssignmentsEnabled(bool enabled) { assignmentsEnabled_=enabled; }
    void setGridAssignmentInterval(quint64 milliseconds) { assignmentIntervalMs_=milliseconds; assignmentTimes_.clear(); }
    AuthorityBeaconStats gridAssignmentStats() const { return {assignmentsForwarded_,assignmentsDropped_}; }
    void setGridAssignmentAcksEnabled(bool enabled) { assignmentAcksEnabled_=enabled; }
    AuthorityBeaconStats gridAssignmentAckStats() const { return {assignmentAcksForwarded_,assignmentAcksDropped_}; }
    void dropGridAssignments(quint64 count) { firstAssignmentsToDrop_=count; }
    void blockGridAssignmentsTo(const QString& peer) { blockedAssignmentPeer_=peer; }
    void dropGridRecoveryRequests(quint64 count) { recoveryRequestsToDrop_=count; }
    void delayMediaProof(int milliseconds) { mediaProofDelayMs_ = milliseconds; }
    quint64 delayedMediaProofs() const { return delayedMediaProofs_; }
    bool blockFirst = true, blockMesh = false, blockLeave = false, invalidLan = false;
    bool impairMultiplex = true;
    quint64 leavesDropped = 0;
    quint64 dropped = 0, duplicated = 0;
    explicit CandidateNetwork(quint16 server) : server_(server) {
        require(ingress_.bind(QHostAddress::LocalHost, 0), "bind simulated NAT ingress");
    }
    ~CandidateNetwork() {
        if (!traceMux_) return;
        QFile trace(QDir(qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT",QDir::tempPath())).filePath("udp-candidate-mux-trace.jsonl"));
        if (trace.open(QIODevice::WriteOnly)) for (const auto& row : muxTrace_) trace.write(QJsonDocument(row).toJson(QJsonDocument::Compact)+'\n');
    }
    quint16 port() const { return ingress_.localPort(); }
    void suppressControlOpens(const QString& peer, int count) {
        suppressedPeer = peer; suppressCount = count; suppressedStreams.clear();
    }
    int suppressedCount() const { return static_cast<int>(suppressedStreams.size()); }
    void blockCurrentPair(const QString& a, const QString& b) {
        blockedA = a; blockedB = b; blockedPairPorts.clear();
        for (const auto& [local, route] : routes_)
            if (route->peer == a || route->peer == b) blockedPairPorts.insert(local);
    }
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
            auto object = QJsonDocument::fromJson(packet).object();
            if (invalidLan && object.value("type") == "jam_candidate" && object.value("attempt").toInt() == 1) {
                auto hint = object.value("local_endpoint").toObject(); hint["host"] = "127.0.0.2";
                object["local_endpoint"] = hint; packet = QJsonDocument(object).toJson(QJsonDocument::Compact);
            }
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
                if (packet.size() >= 42 && packet.startsWith("J2MX") &&
                    static_cast<quint8>(packet[5]) == 1 && static_cast<quint8>(packet[6]) == 1 &&
                    sender->peer == suppressedPeer) {
                    const auto stream = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(packet.constData() + 24));
                    if (suppressedStreams.contains(stream) || static_cast<int>(suppressedStreams.size()) < suppressCount) {
                        suppressedStreams.insert(stream); ++dropped; continue;
                    }
                }
                if (((route->peer == blockedA && sender->peer == blockedB) ||
                     (route->peer == blockedB && sender->peer == blockedA)) &&
                    blockedPairPorts.contains(route->local) && blockedPairPorts.contains(sender->local)) {
                    ++dropped; continue;
                }
                // Delay only raw audio-runtime HELLO/ACK proof, not the
                // multiplex handoff/authentication. Model slow audio attachment.
                if (mediaProofDelayMs_ > 0 && packet.size() >= 6 && packet.startsWith("JAM2") &&
                    (packet[5] == 1 || packet[5] == 2)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (mediaProofUntil_ == std::chrono::steady_clock::time_point{})
                        mediaProofUntil_ = now + std::chrono::milliseconds(mediaProofDelayMs_);
                    if (now < mediaProofUntil_) { ++delayedMediaProofs_; continue; }
                }
                if ((packet.size()==100 && packet.startsWith("JAM2") && packet[5]==9) ||
                    (packet.size()==92 && packet.startsWith("JAM2") && packet[5]==6 &&
                     (static_cast<quint8>(packet[56])&0x83)==0x82)) {
                    if (!assignmentsEnabled_ || route->peer==blockedAssignmentPeer_ || firstAssignmentsToDrop_) {
                        if (firstAssignmentsToDrop_) --firstAssignmentsToDrop_;
                        ++assignmentsDropped_; ++dropped; continue;
                    }
                    if (assignmentIntervalMs_) {
                        const auto now=std::chrono::steady_clock::now();
                        const auto revision=qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(packet.constData()+40));
                        auto& last=assignmentTimes_[{sender->peer,route->peer}];
                        // Preserve the producer's immediate publication of changed revisions.
                        if (last.time!=std::chrono::steady_clock::time_point{} && revision==last.revision &&
                            now-last.time<std::chrono::milliseconds(assignmentIntervalMs_)) { ++assignmentsDropped_; ++dropped; continue; }
                        last={now,revision};
                    }
                    ++assignmentsForwarded_;
                }
                if (packet.size()==44 && packet.startsWith("JAM2") && packet[5]==10) {
                    if (!assignmentAcksEnabled_) { ++assignmentAcksDropped_; ++dropped; continue; }
                    ++assignmentAcksForwarded_;
                }
                if (packet.size()==36 && packet.startsWith("JAM2") && packet[5]==11 && recoveryRequestsToDrop_) {
                    --recoveryRequestsToDrop_; ++dropped; continue;
                }
                // Test-only loss injection for native authority beacons. Keep
                // assignments/proposals, transport intents and audio intact.
                if (packet.size()==92 && packet.startsWith("JAM2") && packet[5]==6 &&
                    (static_cast<quint8>(packet[56])&0x83)==0x83) {
                    if (!authorityBeaconsEnabled_) { ++authorityBeaconsDropped_; ++dropped; continue; }
                    if (authorityBeaconIntervalMs_) {
                        const auto now=std::chrono::steady_clock::now();
                        auto& last=authorityBeaconTimes_[{sender->peer,route->peer}];
                        if (last!=std::chrono::steady_clock::time_point{} && now-last<std::chrono::milliseconds(authorityBeaconIntervalMs_)) {
                            ++authorityBeaconsDropped_; ++dropped; continue;
                        }
                        last=now;
                    }
                    ++authorityBeaconsForwarded_;
                }
                ++directCount_;
                if (impairMultiplex && packet.startsWith("J2MX") && directCount_ % 19 == 0) { traceMux(*sender,*route,packet,true); ++dropped; continue; }
                traceMux(*sender,*route,packet,false);
                sender->socket.writeDatagram(packet, QHostAddress::LocalHost, local);
                if (impairMultiplex && packet.startsWith("J2MX") && directCount_ % 23 == 0) {
                    sender->socket.writeDatagram(packet, QHostAddress::LocalHost, local); ++duplicated;
                }
            }
        }
    }
};

// Runs the actual UDP translator while the four-process GUI harness waits on
// native automation pipes. All socket access remains on its owning thread.
class ThreadedCandidateNetwork {
    std::thread worker_;
    QObject* context_ = nullptr;
    CandidateNetwork* network_ = nullptr;
    QEventLoop* loop_ = nullptr;
    quint16 port_ = 0;
public:
    explicit ThreadedCandidateNetwork(quint16 server) {
        std::promise<quint16> ready;
        auto future = ready.get_future();
        worker_ = std::thread([this, server, ready = std::move(ready)]() mutable {
            try {
                CandidateNetwork network(server); network.blockFirst = false;
                QObject context; QEventLoop loop; QTimer timer;
                context_ = &context; network_ = &network; loop_ = &loop;
                QObject::connect(&timer, &QTimer::timeout, &context, [&] { network.pump(); });
                timer.start(1); ready.set_value(network.port()); loop.exec();
            } catch (...) { ready.set_exception(std::current_exception()); }
        });
        try { port_ = future.get(); }
        catch (...) { worker_.join(); throw; }
    }
    ~ThreadedCandidateNetwork() {
        if (worker_.joinable()) {
            QMetaObject::invokeMethod(context_, [this] { loop_->quit(); }, Qt::QueuedConnection);
            worker_.join();
        }
    }
    quint16 port() const { return port_; }
    void suppressControlOpens(const QString& peer, int count) {
        QMetaObject::invokeMethod(context_, [this, peer, count] { network_->suppressControlOpens(peer, count); },
            Qt::BlockingQueuedConnection);
    }
    void blockCurrentPair(const QString& a, const QString& b) {
        QMetaObject::invokeMethod(context_, [this, a, b] { network_->blockCurrentPair(a, b); },
            Qt::BlockingQueuedConnection);
    }
    void setMultiplexImpairment(bool enabled) {
        QMetaObject::invokeMethod(context_, [this, enabled] { network_->impairMultiplex = enabled; },
            Qt::BlockingQueuedConnection);
    }
    void setAuthorityBeaconsEnabled(bool enabled) {
        QMetaObject::invokeMethod(context_, [this,enabled] { network_->setAuthorityBeaconsEnabled(enabled); }, Qt::BlockingQueuedConnection);
    }
    void setAuthorityBeaconInterval(quint64 milliseconds) {
        QMetaObject::invokeMethod(context_, [this,milliseconds] { network_->setAuthorityBeaconInterval(milliseconds); }, Qt::BlockingQueuedConnection);
    }
    CandidateNetwork::AuthorityBeaconStats authorityBeaconStats() const {
        CandidateNetwork::AuthorityBeaconStats result;
        QMetaObject::invokeMethod(context_, [&] { result=network_->authorityBeaconStats(); }, Qt::BlockingQueuedConnection);
        return result;
    }
    void setGridAssignmentsEnabled(bool enabled) {
        QMetaObject::invokeMethod(context_, [this,enabled] { network_->setGridAssignmentsEnabled(enabled); }, Qt::BlockingQueuedConnection);
    }
    void setGridAssignmentInterval(quint64 milliseconds) {
        QMetaObject::invokeMethod(context_, [this,milliseconds] { network_->setGridAssignmentInterval(milliseconds); }, Qt::BlockingQueuedConnection);
    }
    CandidateNetwork::AuthorityBeaconStats gridAssignmentStats() const {
        CandidateNetwork::AuthorityBeaconStats result;
        QMetaObject::invokeMethod(context_, [&] { result=network_->gridAssignmentStats(); }, Qt::BlockingQueuedConnection);
        return result;
    }
    void setGridAssignmentAcksEnabled(bool enabled) {
        QMetaObject::invokeMethod(context_, [this,enabled] { network_->setGridAssignmentAcksEnabled(enabled); }, Qt::BlockingQueuedConnection);
    }
    CandidateNetwork::AuthorityBeaconStats gridAssignmentAckStats() const {
        CandidateNetwork::AuthorityBeaconStats result;
        QMetaObject::invokeMethod(context_, [&] { result=network_->gridAssignmentAckStats(); }, Qt::BlockingQueuedConnection);
        return result;
    }
    void dropGridAssignments(quint64 count) {
        QMetaObject::invokeMethod(context_, [this,count] { network_->dropGridAssignments(count); }, Qt::BlockingQueuedConnection);
    }
    void blockGridAssignmentsTo(const QString& peer) {
        QMetaObject::invokeMethod(context_, [this,peer] { network_->blockGridAssignmentsTo(peer); }, Qt::BlockingQueuedConnection);
    }
    void dropGridRecoveryRequests(quint64 count) {
        QMetaObject::invokeMethod(context_, [this,count] { network_->dropGridRecoveryRequests(count); }, Qt::BlockingQueuedConnection);
    }
    void delayMediaProof(int milliseconds) {
        QMetaObject::invokeMethod(context_, [this, milliseconds] { network_->delayMediaProof(milliseconds); },
            Qt::BlockingQueuedConnection);
    }
    quint64 delayedMediaProofs() const {
        quint64 result = 0;
        QMetaObject::invokeMethod(context_, [&] { result = network_->delayedMediaProofs(); }, Qt::BlockingQueuedConnection);
        return result;
    }
    int suppressedCount() const {
        int result = 0;
        QMetaObject::invokeMethod(context_, [&] { result = network_->suppressedCount(); }, Qt::BlockingQueuedConnection);
        return result;
    }
};
