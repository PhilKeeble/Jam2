#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QUdpSocket>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>
#include <future>
#include <thread>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>

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
    std::set<quint64> suppressedStreams;
    QString suppressedPeer;
    int suppressCount = 0;
    QString blockedA, blockedB;
    std::set<quint16> blockedPairPorts;
    std::set<QString> firstMembership;
public:
    bool blockFirst = true, blockMesh = false, blockLeave = false, invalidLan = false;
    quint64 leavesDropped = 0;
    quint64 dropped = 0, duplicated = 0;
    explicit CandidateNetwork(quint16 server) : server_(server) {
        require(ingress_.bind(QHostAddress::LocalHost, 0), "bind simulated NAT ingress");
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
    int suppressedCount() const {
        int result = 0;
        QMetaObject::invokeMethod(context_, [&] { result = network_->suppressedCount(); }, Qt::BlockingQueuedConnection);
        return result;
    }
};
