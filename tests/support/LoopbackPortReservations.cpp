#include "LoopbackPortReservations.hpp"

#include <QHostAddress>
#include <QTcpServer>
#include <QUdpSocket>

#include <stdexcept>
#include <utility>

LoopbackPortReservations::~LoopbackPortReservations() = default;

bool LoopbackPortReservations::reserve(std::size_t count, QString& error)
{
    release();
    reservations_.reserve(count);
    std::vector<std::unique_ptr<QTcpServer>> rejectedTcpPorts;
    for (std::size_t index = 0; index < count; ++index) {
        QString lastError;
        bool reserved = false;
        for (int attempt = 0; attempt < 256 && !reserved; ++attempt) {
            auto tcp = std::make_unique<QTcpServer>();
            if (!tcp->listen(QHostAddress::LocalHost, 0)) {
                lastError = tcp->errorString();
                continue;
            }
            const std::uint16_t port = tcp->serverPort();
            auto udp = std::make_unique<QUdpSocket>();
            if (!udp->bind(
                    QHostAddress::LocalHost,
                    port,
                    QUdpSocket::DontShareAddress)) {
                lastError = QStringLiteral("port %1: %2").arg(port).arg(udp->errorString());
                // Keep the rejected TCP choice bound until selection finishes,
                // otherwise Windows can return the same unusable ephemeral
                // TCP port on every retry while UDP still owns that number.
                rejectedTcpPorts.push_back(std::move(tcp));
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
