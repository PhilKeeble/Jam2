#include "AssetTransferService.hpp"

#include "AssetChunkProtocol.hpp"
#include "ContentLimits.hpp"
#include "ControlProtocol.hpp"

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
constexpr qint64 kLooperAssetFrameQueueEstimate = 32 * 1024;
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

AssetTransferService::AssetTransferService(AssetTransferContext& context)
    : context_(context)
{
    outgoingLooperAssetTimer_.setInterval(5);
    QObject::connect(&outgoingLooperAssetTimer_, &QTimer::timeout, [&] { continueSend(); });
    incomingLooperAssetTimer_.setSingleShot(true);
    QObject::connect(&incomingLooperAssetTimer_, &QTimer::timeout, [&] {
        context_.appendAssetLog(QStringLiteral("looper asset receive progress timeout"));
        resetIncoming();
    });
}

AssetTransferService::~AssetTransferService() = default;

void AssetTransferService::handleRequest(const QJsonObject& message, const QString& sourcePeerToken)
{
    if (!context_.trackSyncEnabled()) {
        return;
    }
    const QJsonArray hashes = message.value(QStringLiteral("hashes")).toArray();
    if (hashes.isEmpty() || hashes.size() > kMaxLooperAssetRequests) {
        context_.appendAssetLog(QStringLiteral("rejected looper asset request with invalid hash count"));
        return;
    }
    for (const QJsonValue& value : hashes) {
        const QString hash = value.toString().toLower();
        if (!isSha256Hex(hash)) {
            context_.appendAssetLog(QStringLiteral("rejected looper asset request with invalid hash"));
            return;
        }
        queueSend(hash, sourcePeerToken);
    }
}

void AssetTransferService::queueSend(const QString& hash, const QString& targetPeerToken)
{
    if (!context_.trackSyncEnabled()) {
        return;
    }
    const QPair<QString, QString> request{hash, targetPeerToken};
    if ((outgoingLooperAssetHash_ == hash && outgoingLooperAssetTargetToken_ == targetPeerToken) ||
        (outgoingLooperAssetPendingHash_ == hash && outgoingLooperAssetPendingTargetToken_ == targetPeerToken) ||
        outgoingLooperAssetQueue_.contains(request)) {
        return;
    }
    if (outgoingLooperAssetQueue_.size() >= kMaxLooperAssetRequests) {
        context_.appendAssetLog(QStringLiteral("looper asset send queue is full"));
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
    if (!context_.trackSyncEnabled()) {
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

        const QString path = context_.assetPathForSend(hash);
        struct ValidationResult {
            qint64 bytes = 0;
            QString error;
        };
        auto validation = std::make_shared<ValidationResult>();
        const int expectedSampleRate = context_.sessionSampleRate();
        outgoingLooperAssetValidationPending_ = true;
        outgoingLooperAssetPendingHash_ = hash;
        outgoingLooperAssetPendingTargetToken_ = targetPeerToken;
        const quint64 generation = outgoingLooperAssetGeneration_;
        const bool started = context_.startAssetFileTask(
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
                    context_.appendAssetLog(QStringLiteral("looper asset validation failed hash=%1: %2")
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

    if (!context_.canQueueAssetControl(outgoingLooperAssetTargetToken_, kLooperAssetFrameQueueEstimate)) {
        if (outgoingLooperAssetProgress_.isValid() &&
            outgoingLooperAssetProgress_.elapsed() > kLooperAssetProgressDeadlineMs) {
            context_.appendAssetLog(QStringLiteral("looper asset send progress timeout: ") + outgoingLooperAssetHash_);
            resetOutgoing();
        }
        return;
    }
    if (outgoingLooperAssetNextChunk_ < 0) {
        if (!context_.sendAssetControl(outgoingLooperAssetTargetToken_, QJsonObject{
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
        const qint64 preparedBytes = outgoingLooperAssetPreparedChunk_.size();
        const QByteArray chunk = jam2::application::asset_chunk::encode({
            outgoingLooperAssetHash_,
            static_cast<quint32>(outgoingLooperAssetNextChunk_),
            static_cast<quint64>(outgoingLooperAssetOffset_),
            outgoingLooperAssetPreparedChunk_,
        });
        if (chunk.isEmpty() ||
            !context_.sendAssetBinary(outgoingLooperAssetTargetToken_, chunk)) {
            resetOutgoing();
            return;
        }
        outgoingLooperAssetOffset_ += preparedBytes;
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
        auto bytes = std::make_shared<QByteArray>();
        auto error = std::make_shared<QString>();
        outgoingLooperAssetReadPending_ = true;
        const quint64 generation = outgoingLooperAssetGeneration_;
        const bool started = context_.startAssetFileTask(
            [path, offset, bytes, error] {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
                    *error = QStringLiteral("could not open or seek asset chunk");
                    return;
                }
                *bytes = file.read(kLooperAssetChunkBytes);
                if (bytes->isEmpty()) {
                    *error = QStringLiteral("could not read asset chunk");
                }
            },
            [this, bytes, error, generation] {
                if (generation != outgoingLooperAssetGeneration_) {
                    return;
                }
                outgoingLooperAssetReadPending_ = false;
                if (!error->isEmpty()) {
                    context_.appendAssetLog(QStringLiteral("failed while reading looper asset for sync: ") +
                        *error);
                    resetOutgoing();
                    return;
                }
                outgoingLooperAssetPreparedChunk_ = *bytes;
                continueSend();
            },
            [this, generation](const QString& error) {
                if (generation != outgoingLooperAssetGeneration_) {
                    return;
                }
                outgoingLooperAssetReadPending_ = false;
                context_.appendAssetLog(QStringLiteral("asset read worker failed: ") + error);
                resetOutgoing();
            });
        if (!started) {
            outgoingLooperAssetReadPending_ = false;
        }
        return;
    }

    if (!context_.sendAssetControl(outgoingLooperAssetTargetToken_, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.asset.done")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("chunks"), outgoingLooperAssetNextChunk_},
        })) {
        resetOutgoing();
        return;
    }
    const qint64 fullChunks = outgoingLooperAssetBytes_ / kLooperAssetChunkBytes;
    const qint64 finalChunkBytes = outgoingLooperAssetBytes_ % kLooperAssetChunkBytes;
    const qint64 replacedEncodedDataCharacters =
        fullChunks * (4 * ((kLooperAssetChunkBytes + 2) / 3)) +
        (finalChunkBytes > 0 ? 4 * ((finalChunkBytes + 2) / 3) : 0);
    const qint64 binaryBodyBytes = outgoingLooperAssetBytes_ +
        static_cast<qint64>(outgoingLooperAssetNextChunk_) *
            jam2::application::asset_chunk::kHeaderBytes;
    const qint64 authenticatedWireBytes = binaryBodyBytes +
        static_cast<qint64>(outgoingLooperAssetNextChunk_) *
            (jam2::control_protocol::kAuthenticatedHeaderBytes + 4);
    context_.appendAssetLog(QStringLiteral(
        "sent looper asset: raw_bytes=%1 chunks=%2 binary_body_bytes=%3 "
        "authenticated_chunk_wire_bytes=%4 replaced_text_data_characters=%5 hash=%6")
        .arg(outgoingLooperAssetBytes_)
        .arg(outgoingLooperAssetNextChunk_)
        .arg(binaryBodyBytes)
        .arg(authenticatedWireBytes)
        .arg(replacedEncodedDataCharacters)
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

void AssetTransferService::peerDisconnected(const QString& peerToken)
{
    if (peerToken.isEmpty()) {
        return;
    }
    for (qsizetype index = outgoingLooperAssetQueue_.size(); index-- > 0;) {
        if (outgoingLooperAssetQueue_[index].second == peerToken) {
            outgoingLooperAssetQueue_.removeAt(index);
        }
    }
    const bool outgoingInterrupted =
        outgoingLooperAssetTargetToken_ == peerToken ||
        outgoingLooperAssetPendingTargetToken_ == peerToken;
    if (outgoingInterrupted) {
        ++outgoingLooperAssetGeneration_;
        outgoingLooperAssetPendingHash_.clear();
        outgoingLooperAssetPendingTargetToken_.clear();
        outgoingLooperAssetValidationPending_ = false;
        resetOutgoing();
        if (outgoingLooperAssetQueue_.isEmpty()) {
            outgoingLooperAssetTimer_.stop();
        } else {
            outgoingLooperAssetTimer_.start();
            continueSend();
        }
    }
    if (incomingSequence_.sourceMatches(peerToken)) {
        resetIncoming();
    }
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
    clearIncoming(true);
}

void AssetTransferService::clearIncoming(bool abandonExpected)
{
    const QString interruptedHash = incomingLooperAssetHash_;
    const auto interruptedState = incomingWorkerState_;
    const bool workerPending = incomingLooperAssetWritePending_;
    ++incomingLooperAssetGeneration_;
    incomingLooperAssetTimer_.stop();
    incomingSequence_.reset();
    incomingWorkerState_.reset();
    incomingLooperAssetHash_.clear();
    incomingLooperAssetBytesExpected_ = 0;
    incomingLooperAssetQueue_.clear();
    incomingLooperAssetWritePending_ = false;
    incomingLooperAssetDonePending_ = false;
    incomingLooperAssetDoneChunks_ = 0;
    if (interruptedState && !workerPending && !interruptedState->temporaryPath.isEmpty()) {
        (void)context_.startAssetFileTask(
            [interruptedState] { QFile::remove(interruptedState->temporaryPath); },
            [] {},
            [](const QString&) {});
    }
    if (abandonExpected && !interruptedHash.isEmpty()) {
        context_.abandonIncomingAsset(interruptedHash);
    }
}

void AssetTransferService::receiveStart(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    const double declaredBytes = message.value(QStringLiteral("file_bytes")).toDouble(-1.0);
    const int chunkSize = message.value(QStringLiteral("chunk_size")).toInt(-1);
    QString sequenceError;
    if (incomingWorkerState_ || !isSha256Hex(hash) ||
        !context_.incomingAssetExpected(hash, sourcePeerToken) ||
        !std::isfinite(declaredBytes) || std::floor(declaredBytes) != declaredBytes ||
        declaredBytes < 44.0 || declaredBytes > static_cast<double>(kMaxLooperAssetBytes) ||
        chunkSize <= 0 || chunkSize > kLooperAssetChunkBytes) {
        context_.appendAssetLog(QStringLiteral("rejected unsolicited or invalid looper asset start"));
        return;
    }
    if (!incomingSequence_.begin(
            hash,
            sourcePeerToken,
            static_cast<qint64>(declaredBytes),
            chunkSize,
            sequenceError)) {
        context_.appendAssetLog(QStringLiteral("rejected looper asset start: ") + sequenceError);
        return;
    }

    const QString output = context_.incomingAssetPath(hash);
    auto state = std::make_shared<IncomingWorkerState>();
    state->temporaryPath = output + QStringLiteral(".partial.") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    state->outputPath = output;
    state->expectedHash = hash;
    state->expectedBytes = static_cast<qint64>(declaredBytes);
    state->chunkSize = chunkSize;
    state->expectedSampleRate = context_.sessionSampleRate();
    incomingWorkerState_ = std::move(state);
    incomingLooperAssetHash_ = hash;
    incomingLooperAssetBytesExpected_ = static_cast<qint64>(declaredBytes);
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
    context_.appendAssetLog(QStringLiteral("receiving looper asset: %1 bytes hash=%2")
        .arg(incomingLooperAssetBytesExpected_)
        .arg(incomingLooperAssetHash_));
}

void AssetTransferService::receiveChunk(const QByteArray& payload, const QString& sourcePeerToken)
{
    jam2::application::asset_chunk::Chunk chunk;
    QString error;
    if (!jam2::application::asset_chunk::decode(payload, chunk, error)) {
        context_.appendAssetLog(QStringLiteral("looper asset chunk rejected: ") + error);
        resetIncoming();
        return;
    }
    if (!incomingWorkerState_ || incomingLooperAssetQueue_.size() >= kMaxIncomingAssetQueuedChunks ||
        !incomingSequence_.accept(chunk, sourcePeerToken, error)) {
        context_.appendAssetLog(QStringLiteral("looper asset chunk rejected: ") + error);
        resetIncoming();
        return;
    }
    incomingLooperAssetQueue_.append({static_cast<int>(chunk.index), chunk.data});
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
    scheduleIncomingWrite();
}

void AssetTransferService::receiveDone(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString expectedHash = message.value(QStringLiteral("sha256")).toString().toLower();
    const int chunks = message.value(QStringLiteral("chunks")).toInt(-1);
    QString error;
    if (!incomingWorkerState_ ||
        !incomingSequence_.finish(expectedHash, sourcePeerToken, chunks, error)) {
        context_.appendAssetLog(QStringLiteral("received looper asset failed sequence verification: ") + error);
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
    const QPair<int, QByteArray> chunk = incomingLooperAssetQueue_.takeFirst();
    const auto state = incomingWorkerState_;
    const quint64 generation = incomingLooperAssetGeneration_;
    auto error = std::make_shared<QString>();
    incomingLooperAssetWritePending_ = true;
    const bool started = context_.startAssetFileTask(
        [state, chunk, error] {
            const QByteArray& decoded = chunk.second;
            if (decoded.isEmpty() || decoded.size() > state->chunkSize ||
                state->receivedBytes > state->expectedBytes - decoded.size()) {
                *error = QStringLiteral("asset chunk size validation failed");
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
                (void)context_.startAssetFileTask(
                    [state] { QFile::remove(state->temporaryPath); }, [] {}, [](const QString&) {});
                return;
            }
            incomingLooperAssetWritePending_ = false;
            if (!error->isEmpty()) {
                context_.appendAssetLog(QStringLiteral("looper asset worker rejected chunk: ") + *error);
                resetIncoming();
                return;
            }
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
            context_.appendAssetLog(QStringLiteral("looper asset write worker failed: ") + errorText);
            resetIncoming();
        });
    if (!started) {
        incomingLooperAssetWritePending_ = false;
        incomingLooperAssetQueue_.prepend(chunk);
        QTimer::singleShot(100, context_.dispatchContext(), [this] { scheduleIncomingWrite(); });
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
    const bool started = context_.startAssetFileTask(
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
                context_.appendAssetLog(QStringLiteral("received looper asset validation failed: ") + *error);
                resetIncoming();
                return;
            }
            const QString output = state->outputPath;
            const QString expectedHash = state->expectedHash;
            state->temporaryPath.clear();
            clearIncoming(false);
            context_.acceptIncomingAsset(expectedHash, output);
        },
        [this, state, generation](const QString& errorText) {
            if (generation != incomingLooperAssetGeneration_ ||
                state != incomingWorkerState_) {
                return;
            }
            incomingLooperAssetWritePending_ = false;
            context_.appendAssetLog(QStringLiteral("looper asset finalize worker failed: ") + errorText);
            resetIncoming();
        });
    if (!started) {
        incomingLooperAssetWritePending_ = false;
        incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
        QTimer::singleShot(100, context_.dispatchContext(), [this] { finalizeIncoming(); });
    }
}
