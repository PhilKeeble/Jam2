#include "UdpImpairmentProxy.hpp"

#include <QAbstractSocket>
#include <QNetworkDatagram>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace jam2::test {
namespace {

constexpr qsizetype kMaximumDatagramBytes = 65535;
constexpr int kMaximumDatagramsPerPumpAndSocket = 4096;
constexpr qsizetype kUdpHeaderBytes = 36;
constexpr int kPacketTypeOffset = 5;
constexpr unsigned char kAudioPacketType = 3;

bool validPercent(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

bool validMilliseconds(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

QString endpointText(const QHostAddress& address, quint16 port)
{
    return address.toString() + QLatin1Char(':') + QString::number(port);
}

} // namespace

UdpImpairmentProxy::UdpImpairmentProxy(
    QHostAddress serverAddress,
    quint16 serverPort,
    DirectionImpairment clientToServer,
    DirectionImpairment serverToClient,
    std::uint64_t seed,
    std::size_t maximumPendingPackets)
    : serverAddress_(std::move(serverAddress))
    , serverPort_(serverPort)
    , clientToServerImpairment_(clientToServer)
    , serverToClientImpairment_(serverToClient)
    , random_(seed)
    , maximumPendingPackets_(std::max<std::size_t>(1, maximumPendingPackets))
{
}

bool UdpImpairmentProxy::start(QString& error)
{
    error.clear();
    const auto valid = [](const DirectionImpairment& value) {
        return validPercent(value.lossPercent) &&
            validPercent(value.duplicatePercent) &&
            validPercent(value.corruptPercent) &&
            validPercent(value.reorderPercent) &&
            validMilliseconds(value.fixedDelayMs) &&
            validMilliseconds(value.jitterMs) &&
            validMilliseconds(value.burstLossAfterMs) &&
            validMilliseconds(value.burstLossDurationMs);
    };
    if (serverAddress_.isNull() || serverPort_ == 0 ||
        !valid(clientToServerImpairment_) || !valid(serverToClientImpairment_)) {
        error = QStringLiteral("invalid UDP impairment proxy configuration");
        return false;
    }
    constexpr auto bindMode = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    if (!clientSocket_.bind(QHostAddress::LocalHost, 0, bindMode)) {
        error = QStringLiteral("binding client-facing UDP proxy socket failed: ") +
            clientSocket_.errorString();
        return false;
    }
    if (!serverSocket_.bind(QHostAddress::LocalHost, 0, bindMode)) {
        error = QStringLiteral("binding server-facing UDP proxy socket failed: ") +
            serverSocket_.errorString();
        clientSocket_.close();
        return false;
    }
    return true;
}

void UdpImpairmentProxy::pump()
{
    releaseDue();
    drainClientSocket();
    drainServerSocket();
    releaseDue();
}

void UdpImpairmentProxy::setDatagramTransformer(DatagramTransformer transformer)
{
    transformer_ = std::move(transformer);
}

std::optional<QByteArray> UdpImpairmentProxy::capturedAudioClientToServer() const
{
    return clientToServerState_.capturedAudio;
}

std::optional<QByteArray> UdpImpairmentProxy::capturedAudioServerToClient() const
{
    return serverToClientState_.capturedAudio;
}

bool UdpImpairmentProxy::injectClientToServer(const QByteArray& bytes)
{
    return inject(
        UdpProxyDirection::ClientToServer,
        serverAddress_,
        serverPort_,
        bytes);
}

bool UdpImpairmentProxy::injectServerToClient(const QByteArray& bytes)
{
    if (clientAddress_.isNull() || clientPort_ == 0) {
        ++serverToClientStats_.injectionErrors;
        return false;
    }
    return inject(
        UdpProxyDirection::ServerToClient,
        clientAddress_,
        clientPort_,
        bytes);
}

QString UdpImpairmentProxy::publicEndpoint() const
{
    return endpointText(clientSocket_.localAddress(), clientSocket_.localPort());
}

QString UdpImpairmentProxy::serverPublicEndpoint() const
{
    return endpointText(serverSocket_.localAddress(), serverSocket_.localPort());
}

UdpProxyStats UdpImpairmentProxy::stats() const noexcept
{
    return {
        clientToServerStats_,
        serverToClientStats_,
        pending_.size(),
        pendingHighWater_,
        maximumPendingPackets_,
    };
}

void UdpImpairmentProxy::drainClientSocket()
{
    for (int count = 0;
         count < kMaximumDatagramsPerPumpAndSocket && clientSocket_.hasPendingDatagrams();
         ++count) {
        QNetworkDatagram datagram = clientSocket_.receiveDatagram(kMaximumDatagramBytes);
        if (!datagram.isValid()) {
            // On Windows an earlier unconnected UDP send to a process that has
            // not bound yet is surfaced asynchronously on the next receive as
            // ConnectionRefusedError (WSAECONNRESET). It is a destination
            // lifecycle event in the opposite proxy direction, not a corrupt
            // receive from the current sender.
            if (clientSocket_.error() == QAbstractSocket::ConnectionRefusedError) {
                ++serverToClientStats_.destinationUnreachable;
                continue;
            }
            ++clientToServerStats_.receiveErrors;
            break;
        }
        clientAddress_ = datagram.senderAddress();
        clientPort_ = datagram.senderPort();
        schedule(UdpProxyDirection::ClientToServer, datagram.data(), serverAddress_, serverPort_);
    }
}

void UdpImpairmentProxy::drainServerSocket()
{
    for (int count = 0;
         count < kMaximumDatagramsPerPumpAndSocket && serverSocket_.hasPendingDatagrams();
         ++count) {
        QNetworkDatagram datagram = serverSocket_.receiveDatagram(kMaximumDatagramBytes);
        if (!datagram.isValid()) {
            if (serverSocket_.error() == QAbstractSocket::ConnectionRefusedError) {
                ++clientToServerStats_.destinationUnreachable;
                continue;
            }
            ++serverToClientStats_.receiveErrors;
            break;
        }
        if (clientPort_ == 0 || clientAddress_.isNull()) {
            ++serverToClientStats_.unroutable;
            continue;
        }
        schedule(UdpProxyDirection::ServerToClient, datagram.data(), clientAddress_, clientPort_);
    }
}

void UdpImpairmentProxy::schedule(
    UdpProxyDirection direction,
    QByteArray bytes,
    const QHostAddress& destinationAddress,
    quint16 destinationPort)
{
    auto& proxyStats = mutableStats(direction);
    auto& directionState = state(direction);
    const auto& settings = impairment(direction);
    ++proxyStats.packets;

    if (transformer_) {
        QByteArray transformed = transformer_(direction, bytes);
        if (transformed != bytes) ++proxyStats.transformed;
        bytes = std::move(transformed);
    }
    if (!directionState.capturedAudio && bytes.size() >= kUdpHeaderBytes &&
        static_cast<unsigned char>(bytes[kPacketTypeOffset]) == kAudioPacketType) {
        directionState.capturedAudio = bytes;
    }

    const TimePoint now = Clock::now();
    if (!directionState.firstPacketAt) directionState.firstPacketAt = now;
    if (inBurstLoss(settings, directionState, proxyStats, now)) {
        ++proxyStats.dropped;
        ++proxyStats.burstDropped;
        return;
    }
    if (chance(settings.lossPercent)) {
        ++proxyStats.dropped;
        return;
    }
    if (!bytes.isEmpty() && chance(settings.corruptPercent)) {
        std::uniform_int_distribution<qsizetype> byteIndex(0, bytes.size() - 1);
        std::uniform_int_distribution<int> bitIndex(0, 7);
        const qsizetype index = byteIndex(random_);
        bytes[index] = static_cast<char>(
            static_cast<unsigned char>(bytes[index]) ^ (1U << bitIndex(random_)));
        ++proxyStats.corrupted;
    }

    double delayMs = settings.fixedDelayMs;
    if (settings.jitterMs > 0.0) {
        std::uniform_real_distribution<double> jitter(0.0, settings.jitterMs);
        delayMs += jitter(random_);
    }
    const bool reordered = chance(settings.reorderPercent);
    if (reordered) {
        delayMs += std::max(2.0, settings.jitterMs);
        ++proxyStats.reordered;
    }
    const bool duplicate = chance(settings.duplicatePercent);
    const bool timed = delayMs > 0.0 || reordered;
    if (!timed) {
        if (!send(direction, destinationAddress, destinationPort, bytes)) return;
        if (duplicate && send(direction, destinationAddress, destinationPort, bytes)) {
            ++proxyStats.duplicated;
        }
        return;
    }

    auto delay = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double, std::milli>(delayMs));
    TimePoint releaseAt = now + delay;
    if (settings.preserveOrder && settings.reorderPercent <= 0.0) {
        releaseAt = std::max(
            releaseAt,
            directionState.lastReleaseAt + std::chrono::nanoseconds(1));
        directionState.lastReleaseAt = releaseAt;
    }
    ++proxyStats.delayed;
    if (!queue(direction, releaseAt, destinationAddress, destinationPort, bytes)) return;
    if (duplicate && queue(
            direction,
            releaseAt + std::chrono::nanoseconds(1),
            destinationAddress,
            destinationPort,
            bytes)) {
        ++proxyStats.duplicated;
    }
}

bool UdpImpairmentProxy::queue(
    UdpProxyDirection direction,
    TimePoint releaseAt,
    const QHostAddress& destinationAddress,
    quint16 destinationPort,
    const QByteArray& bytes)
{
    auto& proxyStats = mutableStats(direction);
    if (pending_.size() >= maximumPendingPackets_) {
        ++proxyStats.capacityDropped;
        return false;
    }
    pending_.push(PendingDatagram{
        releaseAt,
        ++nextOrder_,
        direction,
        destinationAddress,
        destinationPort,
        bytes,
    });
    pendingHighWater_ = std::max(pendingHighWater_, pending_.size());
    return true;
}

bool UdpImpairmentProxy::send(
    UdpProxyDirection direction,
    const QHostAddress& destinationAddress,
    quint16 destinationPort,
    const QByteArray& bytes)
{
    QUdpSocket& socket = direction == UdpProxyDirection::ClientToServer
        ? serverSocket_ : clientSocket_;
    auto& proxyStats = mutableStats(direction);
    const qint64 written = socket.writeDatagram(bytes, destinationAddress, destinationPort);
    if (written != bytes.size()) {
        ++proxyStats.sendErrors;
        return false;
    }
    ++proxyStats.forwarded;
    return true;
}

bool UdpImpairmentProxy::inject(
    UdpProxyDirection direction,
    const QHostAddress& destinationAddress,
    quint16 destinationPort,
    const QByteArray& bytes)
{
    QUdpSocket& socket = direction == UdpProxyDirection::ClientToServer
        ? serverSocket_ : clientSocket_;
    auto& proxyStats = mutableStats(direction);
    const qint64 written = socket.writeDatagram(bytes, destinationAddress, destinationPort);
    if (written != bytes.size()) {
        ++proxyStats.injectionErrors;
        return false;
    }
    ++proxyStats.injected;
    return true;
}

void UdpImpairmentProxy::releaseDue()
{
    const TimePoint now = Clock::now();
    while (!pending_.empty() && pending_.top().releaseAt <= now) {
        PendingDatagram datagram = pending_.top();
        pending_.pop();
        (void)send(
            datagram.direction,
            datagram.destinationAddress,
            datagram.destinationPort,
            datagram.bytes);
    }
}

bool UdpImpairmentProxy::chance(double percent)
{
    if (percent <= 0.0) return false;
    if (percent >= 100.0) return true;
    std::uniform_real_distribution<double> sample(0.0, 100.0);
    return sample(random_) < percent;
}

bool UdpImpairmentProxy::inBurstLoss(
    const DirectionImpairment& settings,
    DirectionState& directionState,
    DirectionProxyStats& proxyStats,
    TimePoint now)
{
    if (!directionState.firstPacketAt || settings.burstLossDurationMs <= 0.0) return false;
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        now - *directionState.firstPacketAt).count();
    const double stopMs = settings.burstLossAfterMs + settings.burstLossDurationMs;
    if (elapsedMs < settings.burstLossAfterMs || elapsedMs >= stopMs) return false;
    if (!directionState.burstRecorded) {
        directionState.burstRecorded = true;
        ++proxyStats.burstLossEvents;
    }
    return true;
}

const DirectionImpairment& UdpImpairmentProxy::impairment(
    UdpProxyDirection direction) const noexcept
{
    return direction == UdpProxyDirection::ClientToServer
        ? clientToServerImpairment_ : serverToClientImpairment_;
}

UdpImpairmentProxy::DirectionState& UdpImpairmentProxy::state(
    UdpProxyDirection direction) noexcept
{
    return direction == UdpProxyDirection::ClientToServer
        ? clientToServerState_ : serverToClientState_;
}

DirectionProxyStats& UdpImpairmentProxy::mutableStats(UdpProxyDirection direction) noexcept
{
    return direction == UdpProxyDirection::ClientToServer
        ? clientToServerStats_ : serverToClientStats_;
}

} // namespace jam2::test
