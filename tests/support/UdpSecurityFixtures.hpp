#pragma once

#include "UdpImpairmentProxy.hpp"

#include "protocol.hpp"

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace jam2::test {

struct NamedUdpDatagram {
    QString name;
    QByteArray bytes;
};

std::vector<NamedUdpDatagram> malformedUdpVariants(const QByteArray& validPacket);

struct UdpSequenceTransformStats {
    std::array<std::uint64_t, 2> audioPackets{};
    std::array<bool, 2> wrapped{};
    std::array<bool, 2> forwardGapInjected{};
    std::array<bool, 2> extremeSampleTimeInjected{};
};

class UdpSequenceSecurityTransformer final {
public:
    UdpSequenceSecurityTransformer(
        std::array<std::uint8_t, 16> sessionKey,
        std::uint64_t sessionId,
        NetworkAudioFormat audioFormat);

    QByteArray transform(UdpProxyDirection direction, const QByteArray& packet);
    const UdpSequenceTransformStats& stats() const noexcept;

private:
    std::array<std::uint8_t, 16> sessionKey_{};
    std::uint64_t sessionId_ = 0;
    NetworkAudioFormat audioFormat_ = NetworkAudioFormat::Pcm16Mono;
    UdpSequenceTransformStats stats_;
};

} // namespace jam2::test
