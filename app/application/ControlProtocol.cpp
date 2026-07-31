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

AuthenticatedJsonFrames encodeAuthenticatedJsonFrames(
    const QJsonObject& message,
    const QByteArray& directionKey,
    quint64 firstSequence)
{
    AuthenticatedJsonFrames result;
    const QByteArray raw = QJsonDocument(message).toJson(QJsonDocument::Compact);
    result.rawBytes = raw.size();
    if (raw.isEmpty() || raw.size() > kMaxLargeJsonBytes) {
        return result;
    }
    if (raw.size() <= kMaxJsonBytes) {
        const QByteArray frame = encodeAuthenticated(message, directionKey, firstSequence);
        if (!frame.isEmpty()) {
            result.frames.append(frame);
        }
        return result;
    }

    const QByteArray compressed = qCompress(raw, 6);
    result.compressedBytes = compressed.size();
    if (compressed.isEmpty() || compressed.size() > kMaxLargeJsonBytes ||
        kLargeJsonChunkHeaderBytes + kLargeJsonChunkBytes > kMaxBinaryBytes) {
        result.frames.clear();
        return result;
    }
    const QByteArray hash = QCryptographicHash::hash(compressed, QCryptographicHash::Sha256);
    const quint64 transferId = firstSequence;
    quint64 sequence = firstSequence;
    qsizetype offset = 0;
    while (offset < compressed.size()) {
        const qsizetype bytes = std::min(kLargeJsonChunkBytes, compressed.size() - offset);
        QByteArray chunk;
        chunk.reserve(kLargeJsonChunkHeaderBytes + bytes);
        chunk.append("J2J1", 4);
        const bool first = offset == 0;
        const bool last = offset + bytes == compressed.size();
        chunk.append(static_cast<char>((first ? 0x01 : 0x00) | (last ? 0x02 : 0x00)));
        chunk.append(QByteArray(3, '\0'));
        appendU64(chunk, transferId);
        appendU32(chunk, static_cast<quint32>(raw.size()));
        appendU32(chunk, static_cast<quint32>(compressed.size()));
        appendU32(chunk, static_cast<quint32>(offset));
        appendU32(chunk, static_cast<quint32>(bytes));
        chunk.append(hash);
        chunk.append(compressed.constData() + offset, bytes);
        const QByteArray frame = encodeAuthenticatedPayload(
            AuthenticatedPayloadType::LargeJsonChunk,
            chunk,
            directionKey,
            sequence++);
        if (frame.isEmpty()) {
            result.frames.clear();
            return result;
        }
        result.frames.append(frame);
        offset += bytes;
    }
    result.chunked = true;
    return result;
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
        (type != AuthenticatedPayloadType::Json &&
         type != AuthenticatedPayloadType::AssetChunk &&
         type != AuthenticatedPayloadType::LargeJsonChunk) ||
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

bool LargeJsonReceiver::accept(
    const QByteArray& chunk,
    QJsonObject& completed,
    bool& ready,
    QString& error)
{
    ready = false;
    completed = {};
    if (chunk.size() <= kLargeJsonChunkHeaderBytes ||
        chunk.size() > kLargeJsonChunkHeaderBytes + kLargeJsonChunkBytes ||
        chunk.left(4) != QByteArray("J2J1", 4)) {
        error = QStringLiteral("large JSON chunk header or size is invalid");
        return false;
    }
    const quint8 flags = static_cast<quint8>(chunk[4]);
    if ((flags & ~0x03U) != 0 || chunk[5] != 0 || chunk[6] != 0 || chunk[7] != 0) {
        error = QStringLiteral("large JSON chunk flags are invalid");
        return false;
    }
    const bool first = (flags & 0x01U) != 0;
    const bool last = (flags & 0x02U) != 0;
    const quint64 transferId = readU64(chunk, 8);
    const quint32 rawBytes = readU32(chunk, 16);
    const quint32 compressedBytes = readU32(chunk, 20);
    const quint32 offset = readU32(chunk, 24);
    const quint32 dataBytes = readU32(chunk, 28);
    const QByteArray hash = chunk.mid(32, 32);
    if (transferId == 0 || rawBytes == 0 || rawBytes > kMaxLargeJsonBytes ||
        compressedBytes == 0 || compressedBytes > kMaxLargeJsonBytes ||
        dataBytes == 0 || dataBytes > kLargeJsonChunkBytes ||
        chunk.size() != kLargeJsonChunkHeaderBytes + static_cast<qsizetype>(dataBytes) ||
        offset > compressedBytes || dataBytes > compressedBytes - offset) {
        error = QStringLiteral("large JSON chunk bounds are invalid");
        return false;
    }
    if (first) {
        if (active_ || offset != 0) {
            error = QStringLiteral("large JSON transfer overlaps an active transfer");
            return false;
        }
        transferId_ = transferId;
        rawBytes_ = rawBytes;
        compressedBytes_ = compressedBytes;
        sha256_ = hash;
        compressed_.clear();
        compressed_.reserve(static_cast<qsizetype>(compressedBytes));
        active_ = true;
    }
    if (!active_ || transferId != transferId_ || rawBytes != rawBytes_ ||
        compressedBytes != compressedBytes_ || hash != sha256_ ||
        offset != static_cast<quint32>(compressed_.size())) {
        error = QStringLiteral("large JSON chunk identity or order is invalid");
        return false;
    }
    compressed_.append(chunk.constData() + kLargeJsonChunkHeaderBytes, dataBytes);
    if (!last) {
        if (compressed_.size() >= compressedBytes_) {
            error = QStringLiteral("large JSON transfer ended without its final marker");
            return false;
        }
        return true;
    }
    if (compressed_.size() != compressedBytes_ ||
        QCryptographicHash::hash(compressed_, QCryptographicHash::Sha256) != sha256_) {
        error = QStringLiteral("large JSON transfer size or digest is invalid");
        reset();
        return false;
    }
    // qCompress prefixes the uncompressed byte count. Validate it before
    // qUncompress so an authenticated-but-malformed frame cannot request an
    // allocation beyond the independent 4 MiB transfer bound.
    if (compressed_.size() < 4 || readU32(compressed_, 0) != rawBytes_) {
        error = QStringLiteral("large JSON compression header is invalid");
        reset();
        return false;
    }
    const QByteArray raw = qUncompress(compressed_);
    if (raw.size() != rawBytes_) {
        error = QStringLiteral("large JSON decompressed size is invalid");
        reset();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (!document.isObject()) {
        error = QStringLiteral("invalid reconstructed JSON: ") + parseError.errorString();
        reset();
        return false;
    }
    completed = document.object();
    ready = true;
    reset();
    return true;
}

void LargeJsonReceiver::reset() noexcept
{
    compressed_.clear();
    sha256_.clear();
    transferId_ = 0;
    rawBytes_ = 0;
    compressedBytes_ = 0;
    active_ = false;
}

} // namespace jam2::control_protocol
