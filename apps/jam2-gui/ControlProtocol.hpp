#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace jam2::control_protocol {

constexpr qsizetype kMaxJsonBytes = 64 * 1024;
constexpr qsizetype kAuthenticatedHeaderBytes = 28;
constexpr qint64 kOutputHighWaterBytes = 256 * 1024;
constexpr int kAuthenticationDeadlineMs = 5000;
constexpr int kIncompleteFrameDeadlineMs = 5000;
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
    Failure,
};

enum class TransportFailure {
    None,
    InvalidConfiguration,
    ConnectionRefused,
    HostNotFound,
    NetworkUnavailable,
    TransportError,
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
    RuntimeStartFailed,
};

struct TransportEvent {
    TransportEventType type = TransportEventType::Connecting;
    TransportFailure failure = TransportFailure::None;
    QString detail;
    bool retryable = false;
    bool authenticated = false;
};

QByteArray randomNonce();
QByteArray decodeHex(const QString& value, qsizetype expectedBytes);
QString encodeHex(const QByteArray& value);
QByteArray makeTranscript(
    const QString& sessionHex,
    const QByteArray& serverNonce,
    const QByteArray& clientNonce,
    const QString& peerToken,
    const QString& udpEndpoint);
QByteArray keyedValue(const QByteArray& masterKey, const QByteArray& domain, const QByteArray& transcript);
bool constantTimeEqual(const QByteArray& left, const QByteArray& right);

QByteArray encodeHandshake(const QJsonObject& message);
QByteArray encodeAuthenticated(
    const QJsonObject& message,
    const QByteArray& directionKey,
    quint64 sequence);
TakeFrameResult takeFrame(QByteArray& buffer, QByteArray& body, QString& error);
bool decodeHandshake(const QByteArray& body, QJsonObject& message, QString& error);
bool decodeAuthenticated(
    const QByteArray& body,
    const QByteArray& directionKey,
    quint64 expectedSequence,
    QJsonObject& message,
    QString& error);

} // namespace jam2::control_protocol
