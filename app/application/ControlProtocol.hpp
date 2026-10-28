#pragma once
#include "ControlWireVersion.hpp"

#include <QByteArray>
#include "PeerName.hpp"
#include "BinaryWire.hpp"
#include <variant>
#include <QString>

#include <optional>

namespace jam2::control_protocol {

constexpr qsizetype kMaximumHandshakeBodyBytes = 90;
constexpr qsizetype kMaximumRecordBodyBytes=64*1024+28;
constexpr int kControlProtocolVersion = jam2::application::kControlWireVersion;
constexpr qint64 kOutputHighWaterBytes = 5 * 1024 * 1024;
constexpr int kFramesPerTurn = 32;
constexpr int kMaxPendingPeers = 8;
constexpr int kAuthenticationFailureWindowMs = 10000;
constexpr int kMaxAuthenticationFailuresPerWindow = 64;

enum class TakeFrameResult {
    NeedMore,
    Ready,
    Invalid,
};

// Cold control-plane state is kept typed so reconnect and failure policy never
// depends on matching human-readable diagnostic text.
enum class TransportEventType {
    Listening,
    Connecting,
    Connected,
    ChallengeSent,
    ProofSent,
    Authenticated,
    Disconnected,
    AlreadyConnected,
    RefreshRequested,
    ReconnectScheduled,
    ReconnectAttempt,
    SessionEnded,
    Failure,
};

enum class TransportFailure {
    None,
    InvalidConfiguration,
    ConnectionRefused,
    HostNotFound,
    NetworkUnavailable,
    TransportError,
    PreAuthenticationDisconnect,
    AuthenticationRejected,
    AuthenticationTimeout,
    FrameTimeout,
    FrameRejected,
    AuthenticatedFrameRejected,
    OutputHighWater,
    WriteFailed,
    SessionPeerLimit,
    ContractRejected,
    MembershipRejected,
    ReconnectExhausted,
    CoordinatorTimeout,
    RuntimeStartFailed,
};

struct TransportEvent {
    TransportEventType type = TransportEventType::Connecting;
    TransportFailure failure = TransportFailure::None;
    QString detail;
    bool retryable = false;
    bool authenticated = false;
    bool assetChannel = false;
};

QByteArray randomNonce();
QString randomPeerToken();
QByteArray decodeHex(const QString& value, qsizetype expectedBytes);
QString encodeHex(const QByteArray& value);
std::optional<quint64> peerIdFromToken(const QString& token);
QByteArray makeTranscript(
    const QString& sessionHex,
    const QByteArray& serverNonce,
    const QByteArray& clientNonce,
    const QString& peerToken,
    const QString& udpEndpoint,
    const QString& channel,
    const QString& peerName);
QByteArray keyedValue(const QByteArray& masterKey, const QByteArray& domain, const QByteArray& transcript);
bool constantTimeEqual(const QByteArray& left, const QByteArray& right);

enum class HandshakeChannel : quint8 { Control = 1, Asset = 2 };
enum class HandshakeRejection : quint8 { InvalidFields = 1, InvalidProof, DuplicatePeer, ControlRequired, Capacity };
struct Challenge { QString session; QByteArray nonce; };
struct Proof {
    QByteArray nonce;
    QString peer;
    QByteArray proof;
    HandshakeChannel channel = HandshakeChannel::Control;
    jam2::application::binary_wire::Endpoint endpoint;
    jam2::application::PeerName name;
};
struct Accepted { QByteArray proof; };
struct Rejected { HandshakeRejection reason = HandshakeRejection::InvalidFields; };
using Handshake = std::variant<Challenge, Proof, Accepted, Rejected>;
QString rejectionText(HandshakeRejection reason);
QByteArray encodeHandshake(const Handshake& message);
TakeFrameResult takeFrame(QByteArray& buffer, QByteArray& body, QString& error,
    qsizetype maximumBodyBytes = kMaximumRecordBodyBytes);
bool decodeHandshake(const QByteArray& body, Handshake& message, QString& error);

} // namespace jam2::control_protocol
