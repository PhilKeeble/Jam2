#pragma once

#include <QByteArray>
#include <QString>

namespace jam2::application::asset_chunk {

inline constexpr qsizetype kHeaderBytes = 48;

struct Chunk {
    QString sha256;
    quint32 index = 0;
    quint64 offset = 0;
    QByteArray data;
};

QByteArray encode(const Chunk& chunk);
bool decode(const QByteArray& payload, Chunk& chunk, QString& error);

class ReceiveSequence {
public:
    bool begin(
        const QString& sha256,
        const QString& sourceToken,
        qint64 fileBytes,
        int chunkBytes,
        QString& error);
    bool accept(const Chunk& chunk, const QString& sourceToken, QString& error);
    bool finish(
        const QString& sha256,
        const QString& sourceToken,
        int chunks,
        QString& error) const;
    void reset() noexcept;

    bool active() const noexcept { return active_; }
    bool sourceMatches(const QString& sourceToken) const noexcept
    {
        return active_ && sourceToken_ == sourceToken;
    }
    qint64 acceptedBytes() const noexcept { return acceptedBytes_; }
    int nextIndex() const noexcept { return nextIndex_; }

private:
    QString sha256_;
    QString sourceToken_;
    qint64 fileBytes_ = 0;
    qint64 acceptedBytes_ = 0;
    int chunkBytes_ = 0;
    int nextIndex_ = 0;
    bool active_ = false;
};

} // namespace jam2::application::asset_chunk
