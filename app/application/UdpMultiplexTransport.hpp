#pragma once
#include "UdpStreamConnection.hpp"
#include "udp_socket.hpp"
#include <QObject>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace jam2::application {
class UdpMultiplexTransport final : public QObject, public jam2::NetworkDatagramTransport {
public:
    enum class LinkPhase { Preparing, Offered, Probing, Reporting, Active, Failed };
    struct Config {
        bool creator = false;
        QString sessionHex, keyHex, localToken;
        jam2::Endpoint server{"jam2gether.com", 40404};
        QString bindHost = QStringLiteral("0.0.0.0");
        int maximumPeers = 0;
        quint64 controlBytesPerSecond = 65536, assetBytesPerSecond = 131072;
        quint64 linkRepairSilenceMs = 10000;
    };
    struct LinkSnapshot {
        QString peer, request;
        QString route = QStringLiteral("public");
        LinkPhase phase = LinkPhase::Preparing;
        int attempt = 0, helloSent = 0;
        quint64 introduction = 0;
        jam2::Endpoint local, remote, observed;
        bool receivedHello = false, receivedAck = false;
        quint64 assetQueued = 0, controlQueued = 0, retransmissions = 0;
        quint64 controlStream = 0, assetStream = 0, controlRttUs = 0, assetRttUs = 0;
        quint64 inputDeferrals = 0, lastReceiveAgeMs = 0;
        double controlWindow = 0, assetWindow = 0;
        jam2::Endpoint retainedAudioLocal;
    };
    struct Snapshot {
        bool running = false, registered = false, admissionPaused = false, pauseConfirmed = false;
        quint64 signalPacketsSent = 0, signalPacketsReceived = 0;
        quint64 audioPacketsSent = 0, audioPacketsReceived = 0, audioQueueDrops = 0;
        quint64 assetBytesSent = 0, controlBytesSent = 0, invalidMuxPackets = 0;
        quint64 assetBytesReceived = 0, controlBytesReceived = 0, budgetDeferrals = 0, sendErrors = 0;
        quint64 linkRepairSilenceMs = 0;
        quint64 controlBytesPerSecond = 0, assetBytesPerSecond = 0;
        std::vector<LinkSnapshot> links;
    };
    explicit UdpMultiplexTransport(QObject* parent = nullptr);
    ~UdpMultiplexTransport() override;
    bool start(const Config& config, QString& error);
    void stop();
    bool introduce(const QString& a, const QString& b, const QString& request);
    bool prepareIntroduction(const QString& request);
    bool admitPeer(const QString& peer);
    bool rejectPeer(const QString& peer);
    bool removePeer(const QString& peer);
    bool openStream(const QString& peer, UdpMuxChannel channel);
    bool recoverPeer(const QString& peer);
    bool prepareRecovery(const QString& peer);
    std::optional<jam2::Endpoint> peerEndpoint(quint64 peerId) const;
    std::optional<jam2::Endpoint> observedEndpoint(const QString& peer) const;
    QString coordinatorToken() const;
    Snapshot snapshot() const;
    jam2::UdpSendResult sendToPeer(std::uint64_t id, const jam2::ResolvedUdpEndpoint& endpoint,
        std::span<const std::uint8_t> packet) noexcept override;
    std::optional<jam2::UdpSocket::ReceivedDatagram> receiveFor(std::span<std::uint8_t> buffer,
        std::uint64_t timeoutUs, jam2::RealtimeWakeSignal* captureWake) override;
    std::function<void()> onRegistered;
    std::function<void(const QString&)> onJoinRequest;
    std::function<void(const QString&)> onLinkReady;
    std::function<void(const QString&)> onPeerLeft;
    std::function<void(const QString&)> onLinkStalled;
    std::function<void(const QString&, UdpMuxChannel, const QString&)> onStreamFailed;
    std::function<void(const QString&, UdpMuxChannel, std::shared_ptr<ControlConnection>, bool)> onStreamReady;
    std::function<void(const QString&, const QString&, const QString&)> onPairFailure;
    std::function<void(const QString&)> onSignalerStatus;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace jam2::application
