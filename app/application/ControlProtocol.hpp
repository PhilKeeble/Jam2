#pragma once
#include "ControlWireVersion.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace jam2::control_protocol {

constexpr qsizetype kMaxJsonBytes = 64 * 1024;
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

QByteArray encodeHandshake(const QJsonObject& message);
TakeFrameResult takeFrame(QByteArray& buffer, QByteArray& body, QString& error);
bool decodeHandshake(const QByteArray& body, QJsonObject& message, QString& error);

} // namespace jam2::control_protocol
