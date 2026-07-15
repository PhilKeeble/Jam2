#include "AssetTransferService.hpp"

#include "MainWindow.hpp"

#include "pcm16_wav.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace {

constexpr qint64 kMaxLooperAssetBytes = 512LL * 1024LL * 1024LL;
constexpr int kLooperAssetChunkBytes = 24 * 1024;
constexpr int kMaxLooperAssetRequests = 64;
constexpr qint64 kLooperAssetFrameQueueEstimate = 48 * 1024;
constexpr int kLooperAssetProgressDeadlineMs = 10000;

bool isSha256Hex(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

std::filesystem::path nativeFilePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

QString sha256FileHex(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kBlockBytes = 1024 * 1024;
    while (!file.atEnd()) {
        const QByteArray block = file.read(kBlockBytes);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            return {};
        }
        hash.addData(block);
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace

AssetTransferService::AssetTransferService(MainWindow& window)
    : window_(window)
{
    outgoingLooperAssetTimer_.setInterval(5);
    QObject::connect(&outgoingLooperAssetTimer_, &QTimer::timeout, [&] { continueSend(); });
    incomingLooperAssetTimer_.setSingleShot(true);
    QObject::connect(&incomingLooperAssetTimer_, &QTimer::timeout, [&] {
        window_.appendLog(QStringLiteral("looper asset receive progress timeout"));
        resetIncoming();
    });
}

AssetTransferService::~AssetTransferService() = default;

void AssetTransferService::handleRequest(const QJsonObject& message, const QString& sourcePeerToken)
{
    if (!window_.looperProject_.trackSyncEnabled()) {
        return;
    }
    const QJsonArray hashes = message.value(QStringLiteral("hashes")).toArray();
    if (hashes.isEmpty() || hashes.size() > kMaxLooperAssetRequests) {
        window_.appendLog(QStringLiteral("rejected looper asset request with invalid hash count"));
        return;
    }
    for (const QJsonValue& value : hashes) {
        const QString hash = value.toString().toLower();
        if (!isSha256Hex(hash)) {
            window_.appendLog(QStringLiteral("rejected looper asset request with invalid hash"));
            return;
        }
        queueSend(hash, sourcePeerToken);
    }
}

void AssetTransferService::queueSend(const QString& hash, const QString& targetPeerToken)
{
    const QPair<QString, QString> request{hash, targetPeerToken};
    if ((outgoingLooperAssetHash_ == hash && outgoingLooperAssetTargetToken_ == targetPeerToken) ||
        (outgoingLooperAssetPendingHash_ == hash && outgoingLooperAssetPendingTargetToken_ == targetPeerToken) ||
        outgoingLooperAssetQueue_.contains(request)) {
        return;
    }
    if (outgoingLooperAssetQueue_.size() >= kMaxLooperAssetRequests) {
        window_.appendLog(QStringLiteral("looper asset send queue is full"));
        return;
    }
    outgoingLooperAssetQueue_.append(request);
    if (!outgoingLooperAssetTimer_.isActive()) {
        outgoingLooperAssetTimer_.start();
    }
    continueSend();
}

void AssetTransferService::continueSend()


{
    if (!outgoingLooperAssetFile_) {
        if (outgoingLooperAssetValidationPending_) {
            return;
        }
        if (outgoingLooperAssetQueue_.isEmpty()) {
            outgoingLooperAssetTimer_.stop();
            return;
        }
        const QPair<QString, QString> request = outgoingLooperAssetQueue_.takeFirst();
        const QString hash = request.first;
        const QString targetPeerToken = request.second;
        if (!isSha256Hex(hash)) {
            return;
        }

        QString path = window_.trackOfferAssetPaths_.value(hash);
        for (const LooperBank& bank : window_.looperProject_.banks()) {
            if (!path.isEmpty()) {
                break;
            }
            for (const LooperLane& lane : bank.lanes) {
                if (lane.assetHash == hash) {
                    path = window_.looperAssetAbsolutePath(lane);
                    break;
                }
            }
            if (!path.isEmpty()) {
                break;
            }
        }
        struct ValidationResult {
            qint64 bytes = 0;
            QString error;
        };
        auto validation = std::make_shared<ValidationResult>();
        outgoingLooperAssetValidationPending_ = true;
        outgoingLooperAssetPendingHash_ = hash;
        outgoingLooperAssetPendingTargetToken_ = targetPeerToken;
        const bool started = window_.startFileWorkerTask(
            [path, hash, validation] {
                const QFileInfo workerInfo(path);
                if (path.isEmpty() || !workerInfo.isFile() || workerInfo.size() <= 0 ||
                    workerInfo.size() > kMaxLooperAssetBytes || sha256FileHex(path) != hash) {
                    validation->error = QStringLiteral("asset unavailable, hash-mismatched, or outside transfer bounds");
                    return;
                }
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                if (!inspected) {
                    validation->error = QStringLiteral("WAV validation failed: ") + QString::fromStdString(inspected.error);
                    return;
                }
                validation->bytes = workerInfo.size();
            },
            [this, path, hash, targetPeerToken, validation] {
                outgoingLooperAssetValidationPending_ = false;
                outgoingLooperAssetPendingHash_.clear();
                outgoingLooperAssetPendingTargetToken_.clear();
                if (!validation->error.isEmpty()) {
                    window_.appendLog(QStringLiteral("looper asset validation failed hash=%1: %2")
                        .arg(hash, validation->error));
                    continueSend();
                    return;
                }
                auto file = std::make_unique<QFile>(path);
                if (!file->open(QIODevice::ReadOnly)) {
                    window_.appendLog(QStringLiteral("could not open looper asset for sync: ") + path);
                    continueSend();
                    return;
                }
                outgoingLooperAssetFile_ = std::move(file);
                outgoingLooperAssetHash_ = hash;
                outgoingLooperAssetTargetToken_ = targetPeerToken;
                outgoingLooperAssetBytes_ = validation->bytes;
                outgoingLooperAssetNextChunk_ = -1;
                outgoingLooperAssetProgress_.start();
                continueSend();
            },
            [this, request](const QString&) {
                outgoingLooperAssetValidationPending_ = false;
                outgoingLooperAssetPendingHash_.clear();
                outgoingLooperAssetPendingTargetToken_.clear();
                outgoingLooperAssetQueue_.prepend(request);
            });
        if (!started) {
            outgoingLooperAssetValidationPending_ = false;
            outgoingLooperAssetPendingHash_.clear();
            outgoingLooperAssetPendingTargetToken_.clear();
            outgoingLooperAssetQueue_.prepend(request);
        }
        return;
    }

    if (!window_.canQueueControlTo(outgoingLooperAssetTargetToken_, kLooperAssetFrameQueueEstimate)) {
        if (outgoingLooperAssetProgress_.isValid() &&
            outgoingLooperAssetProgress_.elapsed() > kLooperAssetProgressDeadlineMs) {
            window_.appendLog(QStringLiteral("looper asset send progress timeout: ") + outgoingLooperAssetHash_);
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            outgoingLooperAssetTargetToken_.clear();
        }
        return;
    }
    if (outgoingLooperAssetNextChunk_ < 0) {
        if (!window_.sendControlTo(outgoingLooperAssetTargetToken_, QJsonObject{
                {QStringLiteral("type"), QStringLiteral("looper.asset.start")},
                {QStringLiteral("sha256"), outgoingLooperAssetHash_},
                {QStringLiteral("file_bytes"), static_cast<double>(outgoingLooperAssetBytes_)},
                {QStringLiteral("chunk_size"), kLooperAssetChunkBytes},
            })) {
            outgoingLooperAssetFile_.reset();

            outgoingLooperAssetHash_.clear();
            return;
        }
        outgoingLooperAssetNextChunk_ = 0;
        outgoingLooperAssetProgress_.restart();
        return;
    }

    if (!outgoingLooperAssetFile_->atEnd()) {
        const QByteArray bytes = outgoingLooperAssetFile_->read(kLooperAssetChunkBytes);
        if (bytes.isEmpty()) {
            window_.appendLog(QStringLiteral("failed while reading looper asset for sync: ") + outgoingLooperAssetHash_);
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            return;
        }
        const QJsonObject chunk{
            {QStringLiteral("type"), QStringLiteral("looper.asset.chunk")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("index"), outgoingLooperAssetNextChunk_},
            {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
        };
        if (!window_.sendControlTo(outgoingLooperAssetTargetToken_, chunk)) {
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            return;
        }
        ++outgoingLooperAssetNextChunk_;
        outgoingLooperAssetProgress_.restart();
        return;
    }

    if (!window_.sendControlTo(outgoingLooperAssetTargetToken_, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.asset.done")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("chunks"), outgoingLooperAssetNextChunk_},
        })) {
        outgoingLooperAssetFile_.reset();
        outgoingLooperAssetHash_.clear();
        return;
    }
    window_.appendLog(QStringLiteral("sent looper asset: %1 bytes hash=%2")
        .arg(outgoingLooperAssetBytes_)
        .arg(outgoingLooperAssetHash_));
    outgoingLooperAssetFile_.reset();
    outgoingLooperAssetHash_.clear();
    outgoingLooperAssetTargetToken_.clear();
    outgoingLooperAssetBytes_ = 0;
    outgoingLooperAssetNextChunk_ = 0;
    outgoingLooperAssetProgress_.invalidate();
}

void AssetTransferService::resetIncoming()
{
    const QString interruptedHash = incomingLooperAssetHash_;
    incomingLooperAssetTimer_.stop();
    incomingLooperAssetFile_.reset();
    incomingLooperAssetHasher_.reset();
    incomingLooperAssetHash_.clear();
    incomingLooperAssetSourceToken_.clear();
    incomingLooperAssetBytesExpected_ = 0;
    incomingLooperAssetBytesReceived_ = 0;
    incomingLooperAssetChunkSize_ = 0;
    incomingLooperAssetNextChunk_ = 0;
    if (!interruptedHash.isEmpty() &&
        window_.pendingTrackAssetSources_.remove(interruptedHash) > 0) {
        window_.pendingLooperAssetHashes_.removeAll(interruptedHash);
    }
}

void AssetTransferService::receiveStart(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    const QString expectedSource = window_.pendingTrackAssetSources_.value(hash);
    const double declaredBytes = message.value(QStringLiteral("file_bytes")).toDouble(-1.0);
    const int chunkSize = message.value(QStringLiteral("chunk_size")).toInt(-1);
    if (incomingLooperAssetFile_ || !isSha256Hex(hash) || !window_.pendingLooperAssetHashes_.contains(hash) ||
        (!expectedSource.isEmpty() && expectedSource != sourcePeerToken) ||
        !std::isfinite(declaredBytes) || std::floor(declaredBytes) != declaredBytes ||
        declaredBytes < 44.0 || declaredBytes > static_cast<double>(kMaxLooperAssetBytes) ||
        chunkSize <= 0 || chunkSize > kLooperAssetChunkBytes) {
        window_.appendLog(QStringLiteral("rejected unsolicited or invalid looper asset start"));
        return;
    }

    const QString output = window_.looperAssetPathForHash(hash);
    QFileInfo outputInfo(output);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        window_.appendLog(QStringLiteral("failed to create looper asset cache folder"));
        return;
    }
    auto file = std::make_unique<QTemporaryFile>(output + QStringLiteral(".partial.XXXXXX"));
    file->setAutoRemove(true);
    if (!file->open()) {
        window_.appendLog(QStringLiteral("failed to create temporary looper asset"));
        return;
    }
    incomingLooperAssetFile_ = std::move(file);
    incomingLooperAssetHasher_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    incomingLooperAssetHash_ = hash;
    incomingLooperAssetSourceToken_ = sourcePeerToken;
    incomingLooperAssetBytesExpected_ = static_cast<qint64>(declaredBytes);
    incomingLooperAssetBytesReceived_ = 0;
    incomingLooperAssetChunkSize_ = chunkSize;
    incomingLooperAssetNextChunk_ = 0;
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
    window_.appendLog(QStringLiteral("receiving looper asset: %1 bytes hash=%2")
        .arg(incomingLooperAssetBytesExpected_)
        .arg(incomingLooperAssetHash_));
}

void AssetTransferService::receiveChunk(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    const int index = message.value(QStringLiteral("index")).toInt(-1);
    const QString encodedText = message.value(QStringLiteral("data")).toString();
    const QByteArray encoded = encodedText.toLatin1();
    static const QRegularExpression base64Expression(QStringLiteral("^[A-Za-z0-9+/]*={0,2}$"));
    const int maxEncodedBytes = ((incomingLooperAssetChunkSize_ + 2) / 3) * 4;
    if (incomingLooperAssetFile_ && sourcePeerToken != incomingLooperAssetSourceToken_) {
        window_.appendLog(QStringLiteral("looper asset chunk rejected from unexpected peer"));
        return;
    }
    if (!incomingLooperAssetFile_ || !incomingLooperAssetHasher_ || hash != incomingLooperAssetHash_ ||
        index != incomingLooperAssetNextChunk_ || encodedText.isEmpty() ||
        encoded.size() > maxEncodedBytes || encoded.size() % 4 != 0 ||
        !base64Expression.match(encodedText).hasMatch()) {
        window_.appendLog(QStringLiteral("looper asset chunk rejected"));
        resetIncoming();
        return;
    }
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    if (decoded.isEmpty() || decoded.size() > incomingLooperAssetChunkSize_ ||
        decoded.toBase64() != encoded ||
        incomingLooperAssetBytesReceived_ > incomingLooperAssetBytesExpected_ - decoded.size() ||
        incomingLooperAssetFile_->write(decoded) != decoded.size()) {
        window_.appendLog(QStringLiteral("looper asset chunk decoding or write failed"));
        resetIncoming();
        return;
    }
    incomingLooperAssetHasher_->addData(decoded);
    incomingLooperAssetBytesReceived_ += decoded.size();
    ++incomingLooperAssetNextChunk_;
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
}

void AssetTransferService::receiveDone(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString expectedHash = message.value(QStringLiteral("sha256")).toString().toLower();
    const int chunks = message.value(QStringLiteral("chunks")).toInt(-1);
    if (incomingLooperAssetFile_ && sourcePeerToken != incomingLooperAssetSourceToken_) {
        window_.appendLog(QStringLiteral("looper asset completion rejected from unexpected peer"));
        return;
    }
    if (!incomingLooperAssetFile_ || !incomingLooperAssetHasher_ || expectedHash != incomingLooperAssetHash_ ||
        chunks != incomingLooperAssetNextChunk_ || incomingLooperAssetBytesExpected_ != incomingLooperAssetBytesReceived_ ||
        QString::fromLatin1(incomingLooperAssetHasher_->result().toHex()) != expectedHash) {
        window_.appendLog(QStringLiteral("received looper asset failed size, sequence, or hash verification"));
        resetIncoming();
        return;
    }
    incomingLooperAssetFile_->flush();

    incomingLooperAssetFile_->close();
    incomingLooperAssetTimer_.stop();
    const QString temporaryPath = incomingLooperAssetFile_->fileName();
    const QString output = window_.looperAssetPathForHash(expectedHash);
    auto validationError = std::make_shared<QString>();
    const bool started = window_.startFileWorkerTask(
        [temporaryPath, output, expectedHash, validationError] {
            const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                nativeFilePath(temporaryPath), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
            if (!inspected) {
                *validationError = QStringLiteral("WAV validation failed: ") +
                    QString::fromStdString(inspected.error);
                return;
            }
            if (QFileInfo::exists(output) && sha256FileHex(output) == expectedHash) {
                return;
            }
            QFile source(temporaryPath);
            QSaveFile destination(output);
            if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
                *validationError = QStringLiteral("could not open asset for atomic commit");
                return;
            }
            constexpr qint64 copyBlockBytes = 1024 * 1024;
            while (!source.atEnd()) {
                const QByteArray block = source.read(copyBlockBytes);
                if (block.isEmpty() && source.error() != QFileDevice::NoError) {
                    *validationError = QStringLiteral("failed while reading validated asset");

                    return;

                }
                if (destination.write(block) != block.size()) {
                    *validationError = QStringLiteral("failed while writing validated asset");
                    return;
                }
            }
            if (!destination.commit()) {
                *validationError = QStringLiteral("failed to atomically commit validated asset");
            }
        },
        [this, output, expectedHash, validationError] {
            if (!validationError->isEmpty()) {
                window_.appendLog(QStringLiteral("received looper asset validation failed: ") + *validationError);
                resetIncoming();
                return;
            }
            window_.registerTransientTrackWav(output);
            window_.looperWaveformCache_.remove(output);
            window_.appendLog(QStringLiteral("received looper asset: ") + output);
            window_.pendingLooperAssetHashes_.removeAll(expectedHash);
            if (window_.pendingTrackAssetSources_.remove(expectedHash) > 0) {
                window_.validatedTrackAssetHashes_.insert(expectedHash);
            }
            resetIncoming();
            window_.applyPendingTrackContributions();
            window_.requestNextPendingTrackAsset();
            window_.applyPendingSongIfAssetsReady();
        },
        [this](const QString&) { resetIncoming(); });
    if (!started) {
        window_.appendLog(QStringLiteral("received looper asset validation rejected by saturated file worker"));
        resetIncoming();
    }
}
