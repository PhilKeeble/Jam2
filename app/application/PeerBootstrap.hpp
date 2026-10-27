#pragma once

#include "common.hpp"
#include <QByteArray>
#include <QString>
#include <optional>
#include <span>

namespace jam2::application {

enum class PeerBootstrapKind : quint8 {
    Join = 1, Welcome, JoinReady, Accepted, Prepare, Candidate,
    Offer, Ready, Go, Outcome, Complete, CompleteAck, Rejected,
};

struct PeerBootstrapEndpoint {
    quint32 address = 0;
    quint16 port = 0;
    bool valid() const noexcept { return address != 0 && port != 0; }
    static PeerBootstrapEndpoint from(const jam2::Endpoint& endpoint);
    jam2::Endpoint endpoint() const;
    friend bool operator==(const PeerBootstrapEndpoint&, const PeerBootstrapEndpoint&) = default;
};

// Bounded, authenticated direct-session bootstrap. The session key is never
// transmitted. Established control and media use the existing multiplex.
struct PeerBootstrapMessage {
    PeerBootstrapKind kind = PeerBootstrapKind::Join;
    quint64 session = 0, introduction = 0;
    QString sender, request, partner;
    quint8 attempt = 0;
    PeerBootstrapEndpoint endpoint, observed, local;
    bool success = false, lan = false;
};

constexpr qsizetype kPeerBootstrapMaximumBytes = 128;
bool isPeerBootstrap(std::span<const std::uint8_t> packet) noexcept;
QByteArray encodePeerBootstrap(const PeerBootstrapMessage& message, const QByteArray& key);
std::optional<PeerBootstrapMessage> decodePeerBootstrap(
    std::span<const std::uint8_t> packet, quint64 session, const QByteArray& key);

} // namespace jam2::application
