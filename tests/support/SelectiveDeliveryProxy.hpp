#pragma once
#include "UdpImpairmentProxy.hpp"
#include "protocol.hpp"
#include <array>
#include <optional>
#include <string_view>

namespace jam2::test {
enum class DeliveryTraffic : std::size_t { Audio, Beacon, RoundTrip, Control, Count };
enum class DeliveryDelayScope { All, Audio, Beacon, RoundTrip, NonAudio, None };

inline DeliveryTraffic deliveryTraffic(const QByteArray& packet) noexcept {
    // Observation only: product authentication/parsing still runs at the receiver.
    if (packet.size() >= qsizetype(protocol::kHeaderSize) && packet.startsWith("JAM2") &&
        static_cast<unsigned char>(packet[4]) == protocol::kProtocolVersion) {
        switch (static_cast<protocol::PacketType>(static_cast<unsigned char>(packet[5]))) {
        case protocol::PacketType::Audio: return DeliveryTraffic::Audio;
        case protocol::PacketType::MetronomeState: return DeliveryTraffic::Beacon;
        case protocol::PacketType::Ping:
        case protocol::PacketType::Pong: return DeliveryTraffic::RoundTrip;
        default: break;
        }
    }
    return DeliveryTraffic::Control;
}
inline std::optional<DeliveryDelayScope> deliveryDelayScope(std::string_view name) noexcept {
    if (name == "all") return DeliveryDelayScope::All;
    if (name == "audio") return DeliveryDelayScope::Audio;
    if (name == "beacons") return DeliveryDelayScope::Beacon;
    if (name == "ping-pong") return DeliveryDelayScope::RoundTrip;
    if (name == "non-audio") return DeliveryDelayScope::NonAudio;
    if (name == "none") return DeliveryDelayScope::None;
    return {};
}
inline const char* deliveryDelayScopeText(DeliveryDelayScope scope) noexcept {
    switch (scope) {
    case DeliveryDelayScope::All: return "all";
    case DeliveryDelayScope::Audio: return "audio";
    case DeliveryDelayScope::Beacon: return "beacons";
    case DeliveryDelayScope::RoundTrip: return "ping-pong";
    case DeliveryDelayScope::NonAudio: return "non-audio";
    case DeliveryDelayScope::None: return "none";
    }
    return "invalid";
}
inline bool deliverySelected(DeliveryDelayScope scope, DeliveryTraffic traffic) noexcept {
    switch (scope) {
    case DeliveryDelayScope::All: return true;
    case DeliveryDelayScope::Audio: return traffic == DeliveryTraffic::Audio;
    case DeliveryDelayScope::Beacon: return traffic == DeliveryTraffic::Beacon;
    case DeliveryDelayScope::RoundTrip: return traffic == DeliveryTraffic::RoundTrip;
    case DeliveryDelayScope::NonAudio: return traffic != DeliveryTraffic::Audio;
    case DeliveryDelayScope::None: return false;
    }
    return false;
}
struct DeliveryTrafficCounts {
    std::array<std::uint64_t, std::size_t(DeliveryTraffic::Count)> selected{}, baseline{};
};
class SelectiveDeliveryProxy final {
    static DirectionImpairment baseline(DirectionImpairment value) {
        value.deliveryDelay = {};
        value.deliveryBatchMs = 0;
        return value;
    }
    DeliveryDelayScope scope_;
    UdpImpairmentProxy selected_, baseline_;
    DeliveryTrafficCounts forward_, reverse_;
public:
    SelectiveDeliveryProxy(quint16 port, DirectionImpairment reverse, DirectionImpairment forward,
        DeliveryDelayScope scope, std::uint64_t seed, std::size_t capacity)
        : scope_(scope), selected_(QHostAddress::LocalHost, port, reverse, forward, seed, capacity),
          baseline_(QHostAddress::LocalHost, port, baseline(reverse), baseline(forward), seed, capacity) {}
    bool start(QString& error) { return selected_.start(error) && baseline_.start(error); }
    void forward(UdpProxyDirection direction, const QByteArray& packet, UdpImpairmentProxy::DatagramDelivery deliver) {
        const auto kind = deliveryTraffic(packet);
        const bool selected = deliverySelected(scope_, kind);
        auto& counts = direction == UdpProxyDirection::ServerToClient ? forward_ : reverse_;
        ++(selected ? counts.selected : counts.baseline)[std::size_t(kind)];
        // Separate queues prevent a delayed beacon from holding up bypass audio.
        (selected ? selected_ : baseline_).forwardDatagram(direction, packet, std::move(deliver));
    }
    void pump() { selected_.pump(); baseline_.pump(); }
    UdpProxyStats selectedStats() const noexcept { return selected_.stats(); }
    UdpProxyStats baselineStats() const noexcept { return baseline_.stats(); }
    const DeliveryTrafficCounts& forwardCounts() const noexcept { return forward_; }
    const DeliveryTrafficCounts& reverseCounts() const noexcept { return reverse_; }
};
}
