#include "AssetChunkProtocol.hpp"

#include "ContentLimits.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <limits>

namespace jam2::application::asset_chunk {
namespace {

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

} // namespace

QByteArray encode(const Chunk& chunk)
{
    const QByteArray hash = QByteArray::fromHex(chunk.sha256.toLatin1());
    if (hash.size() != 32 || chunk.sha256.size() != 64 || chunk.data.isEmpty() ||
        chunk.data.size() > limits::kMaximumAssetChunkBytes ||
        chunk.offset > static_cast<quint64>(limits::kMaximumAssetBytes) ||
        static_cast<quint64>(chunk.data.size()) >
            static_cast<quint64>(limits::kMaximumAssetBytes) - chunk.offset) {
        return {};
    }
    QByteArray payload;
    payload.reserve(kHeaderBytes + chunk.data.size());
    payload.append(hash);
    appendU32(payload, chunk.index);
    appendU64(payload, chunk.offset);
    appendU32(payload, static_cast<quint32>(chunk.data.size()));
    payload.append(chunk.data);
    return payload;
}

bool decode(const QByteArray& payload, Chunk& chunk, QString& error)
{
    if (payload.size() <= kHeaderBytes ||
        payload.size() > kHeaderBytes + limits::kMaximumAssetChunkBytes) {
        error = QStringLiteral("asset chunk frame size is outside its bound");
        return false;
    }
    const quint32 dataBytes = readU32(payload, 44);
    if (dataBytes == 0 || dataBytes > limits::kMaximumAssetChunkBytes ||
        payload.size() != kHeaderBytes + static_cast<qsizetype>(dataBytes)) {
        error = QStringLiteral("asset chunk declared length does not match its frame");
        return false;
    }
    const quint64 offset = readU64(payload, 36);
    if (offset > static_cast<quint64>(limits::kMaximumAssetBytes) ||
        static_cast<quint64>(dataBytes) >
            static_cast<quint64>(limits::kMaximumAssetBytes) - offset) {
        error = QStringLiteral("asset chunk offset exceeds the file bound");
        return false;
    }
    chunk.sha256 = QString::fromLatin1(payload.left(32).toHex());
    chunk.index = readU32(payload, 32);
    chunk.offset = offset;
    chunk.data = payload.mid(kHeaderBytes, dataBytes);
    return true;
}

bool ReceiveSequence::begin(
    const QString& sha256,
    const QString& sourceToken,
    qint64 fileBytes,
    int chunkBytes,
    QString& error)
{
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (active_ || !hashPattern.match(sha256).hasMatch() || fileBytes < 44 ||
        fileBytes > limits::kMaximumAssetBytes ||
        chunkBytes != limits::kMaximumAssetChunkBytes) {
        error = QStringLiteral("asset transfer start is invalid or overlaps an active transfer");
        return false;
    }
    sha256_ = sha256;
    sourceToken_ = sourceToken;
    fileBytes_ = fileBytes;
    acceptedBytes_ = 0;
    chunkBytes_ = chunkBytes;
    nextIndex_ = 0;
    active_ = true;
    return true;
}

bool ReceiveSequence::accept(
    const Chunk& chunk,
    const QString& sourceToken,
    QString& error)
{
    const qint64 remainingBytes = fileBytes_ - acceptedBytes_;
    const qint64 expectedBytes = std::min<qint64>(chunkBytes_, remainingBytes);
    if (!active_ || sourceToken != sourceToken_ || chunk.sha256 != sha256_ ||
        chunk.index != static_cast<quint32>(nextIndex_) ||
        chunk.offset != static_cast<quint64>(acceptedBytes_) || chunk.data.isEmpty() ||
        nextIndex_ >= limits::kMaximumAssetChunks ||
        chunk.data.size() != expectedBytes ||
        acceptedBytes_ > fileBytes_ - chunk.data.size()) {
        error = QStringLiteral("asset chunk source, identity, order, offset, or size is invalid");
        return false;
    }
    acceptedBytes_ += chunk.data.size();
    ++nextIndex_;
    return true;
}

bool ReceiveSequence::finish(
    const QString& sha256,
    const QString& sourceToken,
    int chunks,
    QString& error) const
{
    if (!active_ || sourceToken != sourceToken_ || sha256 != sha256_ ||
        chunks != nextIndex_ || chunks < 1 || chunks > limits::kMaximumAssetChunks ||
        acceptedBytes_ != fileBytes_) {
        error = QStringLiteral("asset completion source, identity, count, or byte total is invalid");
        return false;
    }
    return true;
}

void ReceiveSequence::reset() noexcept
{
    sha256_.clear();
    sourceToken_.clear();
    fileBytes_ = 0;
    acceptedBytes_ = 0;
    chunkBytes_ = 0;
    nextIndex_ = 0;
    active_ = false;
}

} // namespace jam2::application::asset_chunk
