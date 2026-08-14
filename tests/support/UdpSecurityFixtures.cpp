#include "UdpSecurityFixtures.hpp"

#include <limits>
#include <span>
#include <utility>

namespace jam2::test {
namespace {

constexpr std::uint32_t kNearWrapStart = 0xfffffff0U;
constexpr std::uint64_t kForwardGapPacket = 512;
constexpr std::uint64_t kExtremeSampleTimePacket = 768;

std::size_t directionIndex(UdpProxyDirection direction) noexcept
{
    return direction == UdpProxyDirection::ClientToServer ? 0U : 1U;
}

template <typename Integer>
void writeLittleEndian(QByteArray& bytes, qsizetype offset, Integer value)
{
    for (qsizetype index = 0; index < static_cast<qsizetype>(sizeof(Integer)); ++index) {
        bytes[offset + index] = static_cast<char>(
            (value >> (index * 8)) & static_cast<Integer>(0xff));
    }
}

} // namespace

std::vector<NamedUdpDatagram> malformedUdpVariants(const QByteArray& validPacket)
{
    if (validPacket.size() < static_cast<qsizetype>(protocol::kHeaderSize)) return {};

    std::vector<NamedUdpDatagram> variants;
    variants.reserve(7);
    variants.push_back({QStringLiteral("short"), validPacket.first(12)});

    QByteArray wrongMagic = validPacket;
    writeLittleEndian<std::uint32_t>(wrongMagic, 0, 0);
    variants.push_back({QStringLiteral("magic"), std::move(wrongMagic)});

    QByteArray wrongVersion = validPacket;
    wrongVersion[4] = static_cast<char>(
        static_cast<unsigned char>(wrongVersion[4]) + 1U);
    variants.push_back({QStringLiteral("version"), std::move(wrongVersion)});

    QByteArray unknownType = validPacket;
    unknownType[5] = static_cast<char>(0xff);
    variants.push_back({QStringLiteral("type"), std::move(unknownType)});

    QByteArray wrongSession = validPacket;
    writeLittleEndian<std::uint64_t>(wrongSession, 8, 0);
    variants.push_back({QStringLiteral("session"), std::move(wrongSession)});

    QByteArray invalidPayload = validPacket;
    writeLittleEndian<std::uint16_t>(invalidPayload, 6, 0xffffU);
    variants.push_back({QStringLiteral("payload-size"), std::move(invalidPayload)});

    QByteArray invalidAuthentication = validPacket;
    invalidAuthentication[28] = static_cast<char>(
        static_cast<unsigned char>(invalidAuthentication[28]) ^ 0x01U);
    variants.push_back({QStringLiteral("authentication"), std::move(invalidAuthentication)});
    return variants;
}

UdpSequenceSecurityTransformer::UdpSequenceSecurityTransformer(
    std::array<std::uint8_t, 16> sessionKey,
    std::uint64_t sessionId,
    NetworkAudioFormat audioFormat)
    : sessionKey_(sessionKey)
    , sessionId_(sessionId)
    , audioFormat_(audioFormat)
{
}

QByteArray UdpSequenceSecurityTransformer::transform(
    UdpProxyDirection direction,
    const QByteArray& packet)
{
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(packet.constData()),
        static_cast<std::size_t>(packet.size()));
    const protocol::ParseResult parsed = protocol::parse_packet(
        bytes, sessionKey_, sessionId_, audioFormat_);
    if (!parsed || parsed.header.type != protocol::PacketType::Audio) return packet;

    const std::size_t index = directionIndex(direction);
    const std::uint64_t packetNumber = ++stats_.audioPackets[index];
    protocol::Header header = parsed.header;
    // The two deliberately rejected packets must not manufacture holes in the
    // otherwise contiguous near-wrap stream. Reuse each rejected packet's
    // baseline sequence on the next valid audio packet.
    const std::uint64_t rejectedBefore =
        (packetNumber > kForwardGapPacket ? 1ULL : 0ULL) +
        (packetNumber > kExtremeSampleTimePacket ? 1ULL : 0ULL);
    header.sequence = kNearWrapStart + static_cast<std::uint32_t>(
        packetNumber - 1 - rejectedBefore);
    if (packetNumber > (std::numeric_limits<std::uint32_t>::max)() - kNearWrapStart + 1ULL) {
        stats_.wrapped[index] = true;
    }
    if (packetNumber == kForwardGapPacket) {
        header.sequence += 100000U;
        stats_.forwardGapInjected[index] = true;
    }
    if (packetNumber == kExtremeSampleTimePacket) {
        header.timing_value = (std::numeric_limits<std::uint64_t>::max)() - 128ULL;
        stats_.extremeSampleTimeInjected[index] = true;
    }

    const std::span<const std::uint8_t> payload = bytes.subspan(protocol::kHeaderSize);
    const std::vector<std::uint8_t> encoded = protocol::encode_packet(
        header, payload, sessionKey_, audioFormat_);
    return QByteArray(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<qsizetype>(encoded.size()));
}

const UdpSequenceTransformStats& UdpSequenceSecurityTransformer::stats() const noexcept
{
    return stats_;
}

} // namespace jam2::test
