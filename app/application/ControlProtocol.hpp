#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace jam2::control_protocol {

constexpr qsizetype kMaxJsonBytes = 64 * 1024;
constexpr qsizetype kMaxBinaryBytes = 64 * 1024;
constexpr qsizetype kMaxLargeJsonBytes = 4 * 1024 * 1024;
constexpr qsizetype kLargeJsonChunkBytes = 32 * 1024;
constexpr qsizetype kLargeJsonChunkHeaderBytes = 64;
constexpr qsizetype kAuthenticatedHeaderBytes = 28;
constexpr int kControlProtocolVersion = 3;
constexpr qint64 kOutputHighWaterBytes = 5 * 1024 * 1024;
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

enum class AuthenticatedPayloadType : quint8 {
    Json = 1,
    AssetChunk = 2,
    LargeJsonChunk = 3,
};

struct AuthenticatedPayload {
    AuthenticatedPayloadType type = AuthenticatedPayloadType::Json;
    QJsonObject message;
    QByteArray binary;
};

struct AuthenticatedJsonFrames {
    QList<QByteArray> frames;
    qsizetype rawBytes = 0;
    qsizetype compressedBytes = 0;
    bool chunked = false;
};

class LargeJsonReceiver {
public:
    bool accept(
        const QByteArray& chunk,
        QJsonObject& completed,
        bool& ready,
        QString& error);
    void reset() noexcept;
    bool active() const noexcept { return active_; }

private:
    QByteArray compressed_;
    QByteArray sha256_;
    quint64 transferId_ = 0;
    quint32 rawBytes_ = 0;
    quint32 compressedBytes_ = 0;
    bool active_ = false;
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
    const QString& channel = QStringLiteral("control"));
QByteArray keyedValue(const QByteArray& masterKey, const QByteArray& domain, const QByteArray& transcript);
bool constantTimeEqual(const QByteArray& left, const QByteArray& right);

QByteArray encodeHandshake(const QJsonObject& message);
QByteArray encodeAuthenticated(
    const QJsonObject& message,
    const QByteArray& directionKey,
    quint64 sequence);
AuthenticatedJsonFrames encodeAuthenticatedJsonFrames(
    const QJsonObject& message,
    const QByteArray& directionKey,
    quint64 firstSequence);
QByteArray encodeAuthenticatedBinary(
    const QByteArray& payload,
    const QByteArray& directionKey,
    quint64 sequence);
TakeFrameResult takeFrame(QByteArray& buffer, QByteArray& body, QString& error);
bool decodeHandshake(const QByteArray& body, QJsonObject& message, QString& error);
bool decodeAuthenticated(
    const QByteArray& body,
    const QByteArray& directionKey,
    quint64 expectedSequence,
    AuthenticatedPayload& payload,
    QString& error);

} // namespace jam2::control_protocol
