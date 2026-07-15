#include "AssetTransferService.hpp"

#include "ContentLimits.hpp"
#include "MainWindow.hpp"

#include "pcm16_wav.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace {

constexpr qint64 kMaxLooperAssetBytes = jam2::application::limits::kMaximumAssetBytes;
constexpr int kLooperAssetChunkBytes = jam2::application::limits::kMaximumAssetChunkBytes;
constexpr int kMaxLooperAssetRequests = jam2::application::limits::kMaximumAssetRequests;
constexpr qint64 kLooperAssetFrameQueueEstimate = 48 * 1024;
constexpr int kLooperAssetProgressDeadlineMs = 10000;
constexpr int kMaxIncomingAssetQueuedChunks = 8;

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

struct AssetTransferService::IncomingWorkerState {
    QString temporaryPath;
    QString outputPath;
    QString expectedHash;
    qint64 expectedBytes = 0;
    qint64 receivedBytes = 0;
    int chunkSize = 0;
    int expectedSampleRate = 0;
    std::unique_ptr<QCryptographicHash> hasher;
};

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
    if (!window_.looperProject_.trackSyncEnabled()) {
        return;
    }
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
    if (!window_.looperProject_.trackSyncEnabled()) {
        cancel();
        return;
    }
    if (outgoingLooperAssetHash_.isEmpty()) {
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
        const int expectedSampleRate = window_.sessionController_.snapshot().contract.sampleRate;
        outgoingLooperAssetValidationPending_ = true;
        outgoingLooperAssetPendingHash_ = hash;
        outgoingLooperAssetPendingTargetToken_ = targetPeerToken;
        const quint64 generation = outgoingLooperAssetGeneration_;
        const bool started = window_.startFileWorkerTask(
            [path, hash, expectedSampleRate, validation] {
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
                if (expectedSampleRate > 0 &&
                    inspected.info.sample_rate != static_cast<std::uint32_t>(expectedSampleRate)) {
                    validation->error = QStringLiteral(
                        "WAV sample rate does not match the active session contract");
                    return;
                }
                validation->bytes = workerInfo.size();
            },
            [this, path, hash, targetPeerToken, validation, generation] {
                if (generation != outgoingLooperAssetGeneration_) {
                    return;
                }
                outgoingLooperAssetValidationPending_ = false;
                outgoingLooperAssetPendingHash_.clear();
                outgoingLooperAssetPendingTargetToken_.clear();
                if (!validation->error.isEmpty()) {
                    window_.appendLog(QStringLiteral("looper asset validation failed hash=%1: %2")
                        .arg(hash, validation->error));
                    continueSend();
                    return;
                }
                outgoingLooperAssetPath_ = path;
                outgoingLooperAssetHash_ = hash;
                outgoingLooperAssetTargetToken_ = targetPeerToken;
                outgoingLooperAssetBytes_ = validation->bytes;
                outgoingLooperAssetOffset_ = 0;
                outgoingLooperAssetNextChunk_ = -1;
                outgoingLooperAssetProgress_.start();
                continueSend();
            },
            [this, request, generation](const QString&) {
                if (generation != outgoingLooperAssetGeneration_) {
                    return;
                }
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
            resetOutgoing();
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
            resetOutgoing();
            return;
        }
        outgoingLooperAssetNextChunk_ = 0;
        outgoingLooperAssetProgress_.restart();
        return;
    }

    if (!outgoingLooperAssetPreparedChunk_.isEmpty()) {
        const QJsonObject chunk{
            {QStringLiteral("type"), QStringLiteral("looper.asset.chunk")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("index"), outgoingLooperAssetNextChunk_},
            {QStringLiteral("data"), QString::fromLatin1(outgoingLooperAssetPreparedChunk_)},
        };
        if (!window_.sendControlTo(outgoingLooperAssetTargetToken_, chunk)) {
            resetOutgoing();
            return;
        }
        outgoingLooperAssetOffset_ += std::min<qint64>(
            kLooperAssetChunkBytes, outgoingLooperAssetBytes_ - outgoingLooperAssetOffset_);
        outgoingLooperAssetPreparedChunk_.clear();
        ++outgoingLooperAssetNextChunk_;
        outgoingLooperAssetProgress_.restart();
        return;
    }

    if (outgoingLooperAssetOffset_ < outgoingLooperAssetBytes_) {
        if (outgoingLooperAssetReadPending_) {
            return;
        }
        const QString path = outgoingLooperAssetPath_;
        const qint64 offset = outgoingLooperAssetOffset_;
        auto encoded = std::make_shared<QByteArray>();
        auto error = std::make_shared<QString>();
        outgoingLooperAssetReadPending_ = true;
        const quint64 generation = outgoingLooperAssetGeneration_;
        const bool started = window_.startFileWorkerTask(
            [path, offset, encoded, error] {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
                    *error = QStringLiteral("could not open or seek asset chunk");
                    return;
                }
                const QByteArray bytes = file.read(kLooperAssetChunkBytes);
                if (bytes.isEmpty()) {
                    *error = QStringLiteral("could not read asset chunk");
                    return;
                }
                *encoded = bytes.toBase64();
            },
            [this, encoded, error, generation] {
                if (generation != outgoingLooperAssetGeneration_) {
                    return;
                }
                outgoingLooperAssetReadPending_ = false;
                if (!error->isEmpty()) {
                    window_.appendLog(QStringLiteral("failed while reading looper asset for sync: ") +
                        *error);
                    resetOutgoing();
                    return;
                }
                outgoingLooperAssetPreparedChunk_ = *encoded;
                continueSend();
            },
            [this, generation](const QString& error) {
                if (generation != outgoingLooperAssetGeneration_) {
                    return;
                }
                outgoingLooperAssetReadPending_ = false;
                window_.appendLog(QStringLiteral("asset read worker failed: ") + error);
                resetOutgoing();
            });
        if (!started) {
            outgoingLooperAssetReadPending_ = false;
        }
        return;
    }

    if (!window_.sendControlTo(outgoingLooperAssetTargetToken_, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.asset.done")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("chunks"), outgoingLooperAssetNextChunk_},
        })) {
        resetOutgoing();
        return;
    }
    window_.appendLog(QStringLiteral("sent looper asset: %1 bytes hash=%2")
        .arg(outgoingLooperAssetBytes_)
        .arg(outgoingLooperAssetHash_));
    resetOutgoing();
}

void AssetTransferService::cancel()
{
    ++outgoingLooperAssetGeneration_;
    outgoingLooperAssetTimer_.stop();
    outgoingLooperAssetQueue_.clear();
    outgoingLooperAssetPendingHash_.clear();
    outgoingLooperAssetPendingTargetToken_.clear();
    outgoingLooperAssetValidationPending_ = false;
    resetOutgoing();
    resetIncoming();
}

void AssetTransferService::resetOutgoing()
{
    outgoingLooperAssetPath_.clear();
    outgoingLooperAssetHash_.clear();
    outgoingLooperAssetTargetToken_.clear();
    outgoingLooperAssetBytes_ = 0;
    outgoingLooperAssetOffset_ = 0;
    outgoingLooperAssetNextChunk_ = 0;
    outgoingLooperAssetReadPending_ = false;
    outgoingLooperAssetPreparedChunk_.clear();
    outgoingLooperAssetProgress_.invalidate();
}

void AssetTransferService::resetIncoming()
{
    const QString interruptedHash = incomingLooperAssetHash_;
    const auto interruptedState = incomingWorkerState_;
    const bool workerPending = incomingLooperAssetWritePending_;
    ++incomingLooperAssetGeneration_;
    incomingLooperAssetTimer_.stop();
    incomingWorkerState_.reset();
    incomingLooperAssetHash_.clear();
    incomingLooperAssetSourceToken_.clear();
    incomingLooperAssetBytesExpected_ = 0;
    incomingLooperAssetBytesReceived_ = 0;
    incomingLooperAssetChunkSize_ = 0;
    incomingLooperAssetNextChunk_ = 0;
    incomingLooperAssetQueue_.clear();
    incomingLooperAssetWritePending_ = false;
    incomingLooperAssetDonePending_ = false;
    incomingLooperAssetDoneChunks_ = 0;
    if (interruptedState && !workerPending && !interruptedState->temporaryPath.isEmpty()) {
        (void)window_.startFileWorkerTask(
            [interruptedState] { QFile::remove(interruptedState->temporaryPath); },
            [] {},
            [](const QString&) {});
    }
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
    if (incomingWorkerState_ || !isSha256Hex(hash) || !window_.pendingLooperAssetHashes_.contains(hash) ||
        (!expectedSource.isEmpty() && expectedSource != sourcePeerToken) ||
        !std::isfinite(declaredBytes) || std::floor(declaredBytes) != declaredBytes ||
        declaredBytes < 44.0 || declaredBytes > static_cast<double>(kMaxLooperAssetBytes) ||
        chunkSize <= 0 || chunkSize > kLooperAssetChunkBytes) {
        window_.appendLog(QStringLiteral("rejected unsolicited or invalid looper asset start"));
        return;
    }

    const QString output = window_.looperAssetPathForHash(hash);
    auto state = std::make_shared<IncomingWorkerState>();
    state->temporaryPath = output + QStringLiteral(".partial.") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    state->outputPath = output;
    state->expectedHash = hash;
    state->expectedBytes = static_cast<qint64>(declaredBytes);
    state->chunkSize = chunkSize;
    state->expectedSampleRate = window_.sessionController_.snapshot().contract.sampleRate;
    incomingWorkerState_ = std::move(state);
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
    if (incomingWorkerState_ && sourcePeerToken != incomingLooperAssetSourceToken_) {
        window_.appendLog(QStringLiteral("looper asset chunk rejected from unexpected peer"));
        return;
    }
    if (!incomingWorkerState_ || hash != incomingLooperAssetHash_ ||
        index != incomingLooperAssetNextChunk_ || encodedText.isEmpty() ||
        encoded.size() > maxEncodedBytes || encoded.size() % 4 != 0 ||
        !base64Expression.match(encodedText).hasMatch() ||
        incomingLooperAssetQueue_.size() >= kMaxIncomingAssetQueuedChunks) {
        window_.appendLog(QStringLiteral("looper asset chunk rejected"));
        resetIncoming();
        return;
    }
    incomingLooperAssetQueue_.append({index, encodedText});
    ++incomingLooperAssetNextChunk_;
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
    scheduleIncomingWrite();
}

void AssetTransferService::receiveDone(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString expectedHash = message.value(QStringLiteral("sha256")).toString().toLower();
    const int chunks = message.value(QStringLiteral("chunks")).toInt(-1);
    if (incomingWorkerState_ && sourcePeerToken != incomingLooperAssetSourceToken_) {
        window_.appendLog(QStringLiteral("looper asset completion rejected from unexpected peer"));
        return;
    }
    if (!incomingWorkerState_ || expectedHash != incomingLooperAssetHash_ ||
        chunks != incomingLooperAssetNextChunk_) {
        window_.appendLog(QStringLiteral("received looper asset failed sequence verification"));
        resetIncoming();
        return;
    }
    incomingLooperAssetDonePending_ = true;
    incomingLooperAssetDoneChunks_ = chunks;
    if (!incomingLooperAssetWritePending_ && incomingLooperAssetQueue_.isEmpty()) {
        finalizeIncoming();
    }
}

void AssetTransferService::scheduleIncomingWrite()
{
    if (incomingLooperAssetWritePending_ || incomingLooperAssetQueue_.isEmpty() ||
        !incomingWorkerState_) {
        return;
    }
    const QPair<int, QString> chunk = incomingLooperAssetQueue_.takeFirst();
    const auto state = incomingWorkerState_;
    const quint64 generation = incomingLooperAssetGeneration_;
    auto error = std::make_shared<QString>();
    incomingLooperAssetWritePending_ = true;
    const bool started = window_.startFileWorkerTask(
        [state, chunk, error] {
            const QByteArray encoded = chunk.second.toLatin1();
            const QByteArray decoded = QByteArray::fromBase64(encoded);
            if (decoded.isEmpty() || decoded.size() > state->chunkSize ||
                decoded.toBase64() != encoded ||
                state->receivedBytes > state->expectedBytes - decoded.size()) {
                *error = QStringLiteral("asset chunk decoding or size validation failed");
                return;
            }
            if (!QDir().mkpath(QFileInfo(state->temporaryPath).absolutePath())) {
                *error = QStringLiteral("could not create asset staging folder");
                return;
            }
            QFile file(state->temporaryPath);
            const QIODevice::OpenMode mode = state->receivedBytes == 0
                ? QIODevice::WriteOnly | QIODevice::Truncate
                : QIODevice::WriteOnly | QIODevice::Append;
            if (!file.open(mode) || file.write(decoded) != decoded.size() || !file.flush()) {
                *error = QStringLiteral("asset chunk write failed");
                return;
            }
            if (!state->hasher) {
                state->hasher = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
            }
            state->hasher->addData(decoded);
            state->receivedBytes += decoded.size();
        },
        [this, state, generation, error] {
            if (generation != incomingLooperAssetGeneration_ ||
                state != incomingWorkerState_) {
                (void)window_.startFileWorkerTask(
                    [state] { QFile::remove(state->temporaryPath); }, [] {}, [](const QString&) {});
                return;
            }
            incomingLooperAssetWritePending_ = false;
            if (!error->isEmpty()) {
                window_.appendLog(QStringLiteral("looper asset worker rejected chunk: ") + *error);
                resetIncoming();
                return;
            }
            incomingLooperAssetBytesReceived_ = state->receivedBytes;
            incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
            if (!incomingLooperAssetQueue_.isEmpty()) {
                scheduleIncomingWrite();
            } else if (incomingLooperAssetDonePending_) {
                finalizeIncoming();
            }
        },
        [this, state, generation](const QString& errorText) {
            if (generation != incomingLooperAssetGeneration_ ||
                state != incomingWorkerState_) {
                return;
            }
            incomingLooperAssetWritePending_ = false;
            window_.appendLog(QStringLiteral("looper asset write worker failed: ") + errorText);
            resetIncoming();
        });
    if (!started) {
        incomingLooperAssetWritePending_ = false;
        incomingLooperAssetQueue_.prepend(chunk);
        QTimer::singleShot(100, &window_, [this] { scheduleIncomingWrite(); });
    }
}

void AssetTransferService::finalizeIncoming()
{
    if (incomingLooperAssetWritePending_ || !incomingLooperAssetDonePending_ ||
        !incomingLooperAssetQueue_.isEmpty() || !incomingWorkerState_) {
        return;
    }
    const auto state = incomingWorkerState_;
    const quint64 generation = incomingLooperAssetGeneration_;
    const int doneChunks = incomingLooperAssetDoneChunks_;
    auto error = std::make_shared<QString>();
    incomingLooperAssetWritePending_ = true;
    incomingLooperAssetTimer_.stop();
    const bool started = window_.startFileWorkerTask(
        [state, doneChunks, error] {
            if (doneChunks < 1 || state->receivedBytes != state->expectedBytes || !state->hasher ||
                QString::fromLatin1(state->hasher->result().toHex()) != state->expectedHash) {
                *error = QStringLiteral("asset size, sequence, or incremental hash verification failed");
            } else {
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(state->temporaryPath),
                    static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                if (!inspected) {
                    *error = QStringLiteral("WAV validation failed: ") +
                        QString::fromStdString(inspected.error);
                } else if (state->expectedSampleRate > 0 &&
                           inspected.info.sample_rate !=
                               static_cast<std::uint32_t>(state->expectedSampleRate)) {
                    *error = QStringLiteral("WAV sample rate does not match the active session contract");
                }
            }
            if (error->isEmpty() &&
                !(QFileInfo::exists(state->outputPath) &&
                  sha256FileHex(state->outputPath) == state->expectedHash)) {
                QFile source(state->temporaryPath);
                QSaveFile destination(state->outputPath);
                if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
                    *error = QStringLiteral("could not open asset for atomic commit");
                } else {
                    constexpr qint64 copyBlockBytes = 1024 * 1024;
                    while (error->isEmpty() && !source.atEnd()) {
                        const QByteArray block = source.read(copyBlockBytes);
                        if ((block.isEmpty() && source.error() != QFileDevice::NoError) ||
                            destination.write(block) != block.size()) {
                            *error = QStringLiteral("failed while atomically copying validated asset");
                        }
                    }
                    if (error->isEmpty() && !destination.commit()) {
                        *error = QStringLiteral("failed to atomically commit validated asset");
                    }
                }
            }
            QFile::remove(state->temporaryPath);
        },
        [this, state, generation, error] {
            if (generation != incomingLooperAssetGeneration_ ||
                state != incomingWorkerState_) {
                return;
            }
            incomingLooperAssetWritePending_ = false;
            if (!error->isEmpty()) {
                window_.appendLog(QStringLiteral("received looper asset validation failed: ") + *error);
                resetIncoming();
                return;
            }
            const QString output = state->outputPath;
            const QString expectedHash = state->expectedHash;
            state->temporaryPath.clear();
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
        [this, state, generation](const QString& errorText) {
            if (generation != incomingLooperAssetGeneration_ ||
                state != incomingWorkerState_) {
                return;
            }
            incomingLooperAssetWritePending_ = false;
            window_.appendLog(QStringLiteral("looper asset finalize worker failed: ") + errorText);
            resetIncoming();
        });
    if (!started) {
        incomingLooperAssetWritePending_ = false;
        incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
        QTimer::singleShot(100, &window_, [this] { finalizeIncoming(); });
    }
}
