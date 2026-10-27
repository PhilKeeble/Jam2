#include "LoopbackPortReservations.hpp"

#include <QHostAddress>
#include <QRandomGenerator>
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
            // Keep UDP candidates outside the ephemeral allocator range.
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
            reservations_.push_back({std::move(udp),port});
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
    }
    reservations_.clear();
}
