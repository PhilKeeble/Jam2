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

enum class TakeFrameResult {
    NeedMore,
    Ready,
    Invalid,
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
