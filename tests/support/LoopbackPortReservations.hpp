#pragma once

#include <QString>
#include <QTcpServer>
#include <QUdpSocket>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class LoopbackPortReservations final {
public:
    ~LoopbackPortReservations();

    bool reserve(std::size_t count, QString& error);
    std::uint16_t port(std::size_t index) const;
    void release() noexcept;

private:
    struct Reservation {
        std::unique_ptr<QTcpServer> tcp;
        std::unique_ptr<QUdpSocket> udp;
        std::uint16_t port = 0;
    };

    std::vector<Reservation> reservations_;
};
