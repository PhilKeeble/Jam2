#include "LoopbackPortReservations.hpp"

#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QUdpSocket>

#include <stdexcept>
#include <utility>

LoopbackPortReservations::~LoopbackPortReservations() = default;

bool LoopbackPortReservations::reserve(std::size_t count, QString& error)
{
    // Keep test listeners below the OS ephemeral allocation range. Reserving a
    // dynamic/private port proves that it is free at selection time, but an
    // unrelated outbound connection can reclaim it during the deliberate
    // parent-to-child handoff window on Windows and macOS.
    constexpr std::uint32_t kFirstTestPort = 20000;
    constexpr std::uint32_t kTestPortCount = 28000;
    constexpr std::uint32_t kCandidateStride = 251;

    release();
    reservations_.reserve(count);
    const std::uint32_t candidateStart =
        QRandomGenerator::system()->bounded(kTestPortCount);
    std::uint32_t candidateIndex = 0;
    for (std::size_t index = 0; index < count; ++index) {
        QString lastError;
        bool reserved = false;
        for (int attempt = 0; attempt < 256 && !reserved; ++attempt) {
            // TCP and UDP can have different platform exclusion tables. Do not
            // let either protocol's ephemeral allocator choose for the other;
            // spread explicit candidates across a broad non-ephemeral range and
            // retain only a number that both protocols can own.
            const auto port = static_cast<std::uint16_t>(
                kFirstTestPort +
                ((candidateStart + candidateIndex * kCandidateStride) %
                    kTestPortCount));
            ++candidateIndex;
            auto udp = std::make_unique<QUdpSocket>();
            if (!udp->bind(
                    QHostAddress::LocalHost,
                    port,
                    QUdpSocket::DontShareAddress)) {
                lastError = QStringLiteral("UDP port %1: %2")
                    .arg(port).arg(udp->errorString());
                continue;
            }
            auto tcp = std::make_unique<QTcpServer>();
            if (!tcp->listen(QHostAddress::LocalHost, port)) {
                lastError = QStringLiteral("TCP port %1: %2")
                    .arg(port).arg(tcp->errorString());
                continue;
            }
            reservations_.push_back({std::move(tcp), std::move(udp), port});
            reserved = true;
        }
        if (!reserved) {
            error = QStringLiteral("loopback reservation %1 failed after 256 attempts: %2")
                .arg(index + 1).arg(lastError);
            release();
            return false;
        }
    }
    error.clear();
    return true;
}

std::uint16_t LoopbackPortReservations::port(std::size_t index) const
{
    if (index >= reservations_.size()) {
        throw std::out_of_range("loopback port reservation index is out of range");
    }
    return reservations_[index].port;
}

void LoopbackPortReservations::release() noexcept
{
    for (auto& reservation : reservations_) {
        if (reservation.udp) reservation.udp->close();
        if (reservation.tcp) reservation.tcp->close();
    }
    reservations_.clear();
}
