#pragma once
#include "UdpStreamConnection.hpp"
#include "udp_socket.hpp"
#include <QObject>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace jam2::application {
struct UdpSocketBuffers { std::optional<int> sendBytes, receiveBytes; };
class UdpMultiplexTransport final : public QObject, public jam2::NetworkDatagramTransport {
public:
    enum class LinkPhase { Preparing, Offered, Probing, Reporting, Active, Failed };
    enum class ReceiveModel { Runtime, QueuedWorker };
    static constexpr std::size_t kIngressPacketsPerQueue = 128;
    static constexpr std::size_t kClassificationPacketsPerCall = 64;
    static constexpr quint64 kRuntimeWaitMaximumUs = 1000;
    static constexpr quint64 kClassificationTimingStride = 64;
    static constexpr quint64 kReceiverHandoverTimeoutMs = 2000;
    struct Config {
        bool creator = false;
        jam2::ConnectionStrategy strategy = jam2::ConnectionStrategy::Jam2Server;
        quint16 bindPort = 0;
        UdpSocketBuffers socketBuffers;
        std::optional<jam2::Endpoint> publicEndpoint;
        jam2::Endpoint stunServer{"stun.l.google.com",19302};
        int stunTimeoutMs = 1000, stunRetries = 3;
        QString sessionHex, keyHex, localToken;
        jam2::Endpoint server{"jam2gether.com", 40404};
        QString bindHost = QStringLiteral("0.0.0.0");
        int maximumPeers = 0;
        // Zero removes the fixed asset rate cap; reliable-stream congestion control still applies.
        quint64 controlBytesPerSecond = 65536, assetBytesPerSecond = 0;
        quint64 linkRepairSilenceMs = 10000;
        quint64 pendingAdmissionTimeoutMs = 30000;
        ReceiveModel receiveModel = ReceiveModel::Runtime;
        // Explicit diagnostic ablation; normal sessions reuse workspace.
        bool reuseWorkerBuffers = true;
        bool packUnsentFragments = true;
        bool compactFrames = true, combineAcks = true, fastRecovery = true, acknowledgeNativeIntents = true;
        quint64 fastReorderUs = 10000;
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
        quint64 ackDatagramsSent = 0, acksCoalesced = 0, ackTransmitDeferrals = 0, invalidAckWindows = 0;
        quint64 inOrderDirect = 0, reorderInsertions = 0;
        quint64 tailBytesPacked = 0, fragmentsAvoided = 0;
        quint64 fastRetransmissions = 0, sackGapReports = 0, ackPairs = 0, piggybackedAcks = 0;
        quint64 compactPackets = 0, compactBytesSaved = 0;
        bool nativeIntentSent = false, nativeIntentAcknowledged = false, nativeIntentReceiptsAllowed = true;
        double controlWindow = 0, assetWindow = 0;
        jam2::Endpoint retainedAudioLocal;
        int sendBufferBytes=0, receiveBufferBytes=0;
    };
    struct Snapshot {
        bool running = false, registered = false, admissionPaused = false, pauseConfirmed = false;
        bool usesRendezvous = false;
        jam2::Endpoint bootstrapLocal, bootstrapPublic;
        UdpSocketBuffers requestedBuffers;
        int signalSendBufferBytes=0, signalReceiveBufferBytes=0;
        quint64 physicalSocketCount=0;
        quint64 bootstrapRejected = 0, bootstrapAuthenticationRejects = 0, bootstrapAttempts = 0;
        quint64 pendingAdmissionTimeouts = 0, pendingAdmissionTimeoutMs = 0;
        quint64 stunRequestsSent = 0, stunResponsesRejected = 0;
        quint64 introductionPending = 0;
        quint64 signalPacketsSent = 0, signalPacketsReceived = 0, signalPacketsRejected = 0;
        quint64 audioPacketsSent = 0, audioPacketsReceived = 0, audioQueueDrops = 0;
        quint64 assetBytesSent = 0, controlBytesSent = 0, invalidMuxPackets = 0;
        quint64 assetBytesReceived = 0, controlBytesReceived = 0, budgetDeferrals = 0, sendErrors = 0;
        quint64 workerLoopGapMaxUs = 0, workerServiceMaxUs = 0, workerLockWaitMaxUs = 0;
        quint64 linkRepairSilenceMs = 0;
        quint64 controlBytesPerSecond = 0, assetBytesPerSecond = 0;
        bool runtimeReceiveEnabled = false, runtimeReceiverAttached = false;
        quint64 receiverHandovers = 0, receiverDetachments = 0;
        quint64 runtimeNativeDelivered = 0, bootstrapNativeDiscarded = 0, runtimeInvalidPackets = 0;
        quint64 runtimeControlQueued = 0, runtimeAssetQueued = 0, runtimeBudgetYields = 0;
        quint64 ingressQueueDrops = 0, controlIngressDepth = 0, assetIngressDepth = 0, ingressHighWater = 0;
        quint64 ingressWaitSamples = 0, ingressWaitSumUs = 0, ingressWaitMaxUs = 0;
        quint64 classificationSamples = 0, classificationSumNs = 0, classificationMaxNs = 0;
        quint64 workerTurns = 0, workerWorkspaceGrowths = 0, workerServiceSumUs = 0;
        quint64 ingressCopyBytesAvoided = 0, muxCopyBytesAvoided = 0;
        quint64 workerSendRepolls = 0;
        bool reusesWorkerBuffers = false;
        bool compactFrames = false, combineAcks = false, fastRecovery = false, acknowledgeNativeIntents = false;
        quint64 fastReorderUs = 0, intentReceiptDisabledLinks = 0;
        quint64 nativeIntentAcksSent = 0, nativeIntentAcksReceived = 0, nativeIntentRepeatsSuppressed = 0;
        quint64 nativeIntentAckDeferrals = 0, nativeIntentAckInvalid = 0;
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
    std::optional<jam2::Endpoint> bootstrapEndpoint() const;
    Snapshot snapshot() const;
    std::optional<jam2::NetworkTransportInfo> transportInfo() const override;
    void receiverAttached() override;
    void receiverDetached() noexcept override;
    jam2::UdpSendResult sendToPeer(std::uint64_t id, const jam2::ResolvedUdpEndpoint& endpoint,
        std::span<const std::uint8_t> packet) noexcept override;
    void transportIntentAccepted(std::uint64_t id,std::span<const std::uint8_t> payload) noexcept override;
    std::optional<jam2::UdpSocket::ReceivedDatagram> receiveFor(std::span<std::uint8_t> buffer,
        std::uint64_t timeoutUs, jam2::RealtimeWakeSignal* captureWake) override;
    std::function<void()> onRegistered;
    std::function<void(const QString&)> onBootstrapFailure;
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
