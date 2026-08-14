#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QUdpSocket>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <vector>

namespace jam2::test {

struct DirectionImpairment {
    double lossPercent = 0.0;
    double duplicatePercent = 0.0;
    double corruptPercent = 0.0;
    double reorderPercent = 0.0;
    double fixedDelayMs = 0.0;
    double jitterMs = 0.0;
    double burstLossAfterMs = 0.0;
    double burstLossDurationMs = 0.0;
    bool preserveOrder = true;
};

struct DirectionProxyStats {
    std::uint64_t packets = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t corrupted = 0;
    std::uint64_t transformed = 0;
    std::uint64_t injected = 0;
    std::uint64_t injectionErrors = 0;
    std::uint64_t delayed = 0;
    std::uint64_t reordered = 0;
    std::uint64_t burstLossEvents = 0;
    std::uint64_t burstDropped = 0;
    std::uint64_t capacityDropped = 0;
    std::uint64_t receiveErrors = 0;
    std::uint64_t sendErrors = 0;
    std::uint64_t destinationUnreachable = 0;
    std::uint64_t unroutable = 0;
};

enum class UdpProxyDirection {
    ClientToServer,
    ServerToClient,
};

struct UdpProxyStats {
    DirectionProxyStats clientToServer;
    DirectionProxyStats serverToClient;
    std::size_t pendingPackets = 0;
    std::size_t pendingHighWater = 0;
    std::size_t pendingLimit = 0;
};

class UdpImpairmentProxy final {
public:
    using DatagramTransformer =
        std::function<QByteArray(UdpProxyDirection, const QByteArray&)>;

    UdpImpairmentProxy(
        QHostAddress serverAddress,
        quint16 serverPort,
        DirectionImpairment clientToServer,
        DirectionImpairment serverToClient,
        std::uint64_t seed,
        std::size_t maximumPendingPackets = 65536);

    bool start(QString& error);
    void pump();
    void setDatagramTransformer(DatagramTransformer transformer);

    std::optional<QByteArray> capturedAudioClientToServer() const;
    std::optional<QByteArray> capturedAudioServerToClient() const;
    bool injectClientToServer(const QByteArray& bytes);
    bool injectServerToClient(const QByteArray& bytes);

    QString publicEndpoint() const;
    QString serverPublicEndpoint() const;
    UdpProxyStats stats() const noexcept;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct DirectionState {
        std::optional<TimePoint> firstPacketAt;
        std::optional<QByteArray> capturedAudio;
        TimePoint lastReleaseAt{};
        bool burstRecorded = false;
    };

    struct PendingDatagram {
        TimePoint releaseAt;
        std::uint64_t order = 0;
        UdpProxyDirection direction = UdpProxyDirection::ClientToServer;
        QHostAddress destinationAddress;
        quint16 destinationPort = 0;
        QByteArray bytes;
    };

    struct PendingLater {
        bool operator()(const PendingDatagram& left, const PendingDatagram& right) const noexcept
        {
            if (left.releaseAt != right.releaseAt) return left.releaseAt > right.releaseAt;
            return left.order > right.order;
        }
    };

    void drainClientSocket();
    void drainServerSocket();
    void schedule(
        UdpProxyDirection direction,
        QByteArray bytes,
        const QHostAddress& destinationAddress,
        quint16 destinationPort);
    bool queue(
        UdpProxyDirection direction,
        TimePoint releaseAt,
        const QHostAddress& destinationAddress,
        quint16 destinationPort,
        const QByteArray& bytes);
    bool send(
        UdpProxyDirection direction,
        const QHostAddress& destinationAddress,
        quint16 destinationPort,
        const QByteArray& bytes);
    bool inject(
        UdpProxyDirection direction,
        const QHostAddress& destinationAddress,
        quint16 destinationPort,
        const QByteArray& bytes);
    void releaseDue();
    bool chance(double percent);
    bool inBurstLoss(
        const DirectionImpairment& impairment,
        DirectionState& state,
        DirectionProxyStats& stats,
        TimePoint now);
    const DirectionImpairment& impairment(UdpProxyDirection direction) const noexcept;
    DirectionState& state(UdpProxyDirection direction) noexcept;
    DirectionProxyStats& mutableStats(UdpProxyDirection direction) noexcept;

    QHostAddress serverAddress_;
    quint16 serverPort_ = 0;
    DirectionImpairment clientToServerImpairment_;
    DirectionImpairment serverToClientImpairment_;
    std::mt19937_64 random_;
    std::size_t maximumPendingPackets_ = 0;
    QUdpSocket clientSocket_;
    QUdpSocket serverSocket_;
    QHostAddress clientAddress_;
    quint16 clientPort_ = 0;
    DirectionState clientToServerState_;
    DirectionState serverToClientState_;
    DirectionProxyStats clientToServerStats_;
    DirectionProxyStats serverToClientStats_;
    DatagramTransformer transformer_;
    std::priority_queue<PendingDatagram, std::vector<PendingDatagram>, PendingLater> pending_;
    std::uint64_t nextOrder_ = 0;
    std::size_t pendingHighWater_ = 0;
};

} // namespace jam2::test
