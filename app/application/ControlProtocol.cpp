#include "ControlProtocol.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

#include <algorithm>

namespace jam2::control_protocol {
namespace {

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data);

void appendU32(QByteArray& out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xffU));
    out.append(static_cast<char>((value >> 16) & 0xffU));
    out.append(static_cast<char>((value >> 8) & 0xffU));
    out.append(static_cast<char>(value & 0xffU));
}

void appendU64(QByteArray& out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

quint32 readU32(const QByteArray& data, qsizetype offset)
{
    return (static_cast<quint32>(static_cast<unsigned char>(data[offset])) << 24) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 8) |
        static_cast<quint32>(static_cast<unsigned char>(data[offset + 3]));
}

quint64 readU64(const QByteArray& data, qsizetype offset)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[offset + i]);
    }
    return value;
}

void appendField(QByteArray& out, const QByteArray& value)
{
    appendU32(out, static_cast<quint32>(value.size()));
    out.append(value);
}

QByteArray frameBody(const QByteArray& body)
{
    QByteArray frame;
    frame.reserve(4 + body.size());
    appendU32(frame, static_cast<quint32>(body.size()));
    frame.append(body);
    return frame;
}

QByteArray encodeAuthenticatedPayload(
    AuthenticatedPayloadType type,
    const QByteArray& payload,
    const QByteArray& directionKey,
    quint64 sequence)
{
    const qsizetype limit = type == AuthenticatedPayloadType::Json
        ? kMaxJsonBytes
        : kMaxBinaryBytes;
    if (payload.isEmpty() || payload.size() > limit) {
        return {};
    }
    QByteArray body;
    body.reserve(kAuthenticatedHeaderBytes + payload.size());
    body.append(static_cast<char>(kControlProtocolVersion));
    body.append(static_cast<char>(type));
    body.append('\0');
    body.append('\0');
    appendU64(body, sequence);
    body.append(QByteArray(16, '\0'));
    body.append(payload);
    const QByteArray tag = hmacSha256(directionKey, body).left(16);
    body.replace(12, 16, tag);
    return frameBody(body);
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

} // namespace

QByteArray randomNonce()
{
    QByteArray nonce;
    nonce.reserve(16);
    appendU64(nonce, QRandomGenerator::system()->generate64());
    appendU64(nonce, QRandomGenerator::system()->generate64());
    return nonce;
}

QString randomPeerToken()
{
    for (;;) {
        const QString token = encodeHex(randomNonce());
        if (peerIdFromToken(token).has_value()) {
            return token;
        }
    }
}

QByteArray decodeHex(const QString& value, qsizetype expectedBytes)
{
    const QByteArray encoded = value.toLatin1();
    if (encoded.size() != expectedBytes * 2) {
        return {};
    }
    for (const char valueChar : encoded) {
        const bool hex = (valueChar >= '0' && valueChar <= '9') ||
            (valueChar >= 'a' && valueChar <= 'f') ||
            (valueChar >= 'A' && valueChar <= 'F');
        if (!hex) {
            return {};
        }
    }
    const QByteArray decoded = QByteArray::fromHex(encoded);
    return decoded.size() == expectedBytes ? decoded : QByteArray{};
}

QString encodeHex(const QByteArray& value)
{
    return QString::fromLatin1(value.toHex());
}

std::optional<quint64> peerIdFromToken(const QString& token)
{
    const QByteArray decoded = decodeHex(token, 16);
    if (decoded.size() != 16) {
        return std::nullopt;
    }

    quint64 peerId = 0;
    for (qsizetype index = 0; index < 8; ++index) {
        peerId = (peerId << 8U) |
            static_cast<quint64>(static_cast<unsigned char>(decoded.at(index)));
    }
    return peerId == 0 ? std::nullopt : std::optional<quint64>(peerId);
}

QByteArray makeTranscript(
    const QString& sessionHex,
    const QByteArray& serverNonce,
    const QByteArray& clientNonce,
    const QString& peerToken,
    const QString& udpEndpoint)
{
    QByteArray transcript("jam2-control-v2", 15);
    appendField(transcript, sessionHex.toLower().toLatin1());
    appendField(transcript, serverNonce);
    appendField(transcript, clientNonce);
    appendField(transcript, peerToken.toUtf8());
    appendField(transcript, udpEndpoint.toUtf8());
    return transcript;
}

QByteArray keyedValue(const QByteArray& masterKey, const QByteArray& domain, const QByteArray& transcript)
{
    QByteArray input;
    input.reserve(domain.size() + transcript.size() + 4);
    appendField(input, domain);
    input.append(transcript);
    return hmacSha256(masterKey, input);
}

bool constantTimeEqual(const QByteArray& left, const QByteArray& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    }
    return difference == 0;
}

QByteArray encodeHandshake(const QJsonObject& message)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > kMaxJsonBytes) {
        return {};
    }
    return frameBody(payload);
}

QByteArray encodeAuthenticated(
    const QJsonObject& message,
    const QByteArray& directionKey,
    quint64 sequence)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > kMaxJsonBytes) {
        return {};
    }
    return encodeAuthenticatedPayload(
        AuthenticatedPayloadType::Json,
        payload,
        directionKey,
        sequence);
}

QByteArray encodeAuthenticatedBinary(
    const QByteArray& payload,
    const QByteArray& directionKey,
    quint64 sequence)
{
    return encodeAuthenticatedPayload(
        AuthenticatedPayloadType::AssetChunk,
        payload,
        directionKey,
        sequence);
}

TakeFrameResult takeFrame(QByteArray& buffer, QByteArray& body, QString& error)
{
    if (buffer.size() < 4) {
        return TakeFrameResult::NeedMore;
    }
    const quint32 size = readU32(buffer, 0);
    const qsizetype maximumPayload = std::max(kMaxJsonBytes, kMaxBinaryBytes);
    if (size == 0 || size > static_cast<quint32>(maximumPayload + kAuthenticatedHeaderBytes)) {
        error = QStringLiteral("control frame length is outside the v2 bound");
        return TakeFrameResult::Invalid;
    }
    if (buffer.size() < 4 + static_cast<qsizetype>(size)) {
        return TakeFrameResult::NeedMore;
    }
    body = buffer.mid(4, size);
    buffer.remove(0, 4 + size);
    return TakeFrameResult::Ready;
}

bool decodeHandshake(const QByteArray& body, QJsonObject& message, QString& error)
{
    if (body.size() > kMaxJsonBytes) {
        error = QStringLiteral("handshake JSON exceeds the v2 bound");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (!document.isObject()) {
        error = QStringLiteral("invalid handshake JSON: ") + parseError.errorString();
        return false;
    }
    message = document.object();
    return true;
}

bool decodeAuthenticated(
    const QByteArray& body,
    const QByteArray& directionKey,
    quint64 expectedSequence,
    AuthenticatedPayload& payload,
    QString& error)
{
    if (body.size() < kAuthenticatedHeaderBytes ||
        body.size() > std::max(kMaxJsonBytes, kMaxBinaryBytes) + kAuthenticatedHeaderBytes) {
        error = QStringLiteral("authenticated control frame size is invalid");
        return false;
    }
    const auto type = static_cast<AuthenticatedPayloadType>(static_cast<unsigned char>(body[1]));
    if (static_cast<unsigned char>(body[0]) != kControlProtocolVersion ||
        (type != AuthenticatedPayloadType::Json && type != AuthenticatedPayloadType::AssetChunk) ||
        body[2] != 0 || body[3] != 0) {
        error = QStringLiteral("authenticated control header is invalid");
        return false;
    }
    if (readU64(body, 4) != expectedSequence) {
        error = QStringLiteral("authenticated control sequence is invalid");
        return false;
    }
    const QByteArray receivedTag = body.mid(12, 16);
    QByteArray authenticated = body;
    authenticated.replace(12, 16, QByteArray(16, '\0'));
    const QByteArray expectedTag = hmacSha256(directionKey, authenticated).left(16);
    if (!constantTimeEqual(receivedTag, expectedTag)) {
        error = QStringLiteral("authenticated control tag is invalid");
        return false;
    }
    const QByteArray decoded = body.mid(kAuthenticatedHeaderBytes);
    payload = {};
    payload.type = type;
    if (type == AuthenticatedPayloadType::Json) {
        if (decoded.size() > kMaxJsonBytes) {
            error = QStringLiteral("authenticated JSON exceeds its bound");
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(decoded, &parseError);
        if (!document.isObject()) {
            error = QStringLiteral("invalid authenticated JSON: ") + parseError.errorString();
            return false;
        }
        payload.message = document.object();
    } else {
        if (decoded.size() > kMaxBinaryBytes) {
            error = QStringLiteral("authenticated binary payload exceeds its bound");
            return false;
        }
        payload.binary = decoded;
    }
    return true;
}

} // namespace jam2::control_protocol
