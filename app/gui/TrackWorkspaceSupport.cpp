#include "TrackWorkspaceSupport.hpp"

#include "GuiLoopbackRecorder.hpp"

#include "pcm16_wav.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

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
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
    return QString::fromLatin1(hash.result().toHex());
}

void appendLe16(QByteArray& bytes, std::uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendLe32(QByteArray& bytes, std::uint32_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
    bytes.append(static_cast<char>((value >> 16U) & 0xffU));
    bytes.append(static_cast<char>((value >> 24U) & 0xffU));
}

QByteArray pcm16WavHeader(
    std::uint16_t channels,
    std::uint32_t sampleRate,
    std::uint32_t dataBytes)
{
    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    appendLe32(header, 36U + dataBytes);
    header.append("WAVEfmt ", 8);
    appendLe32(header, 16U);
    appendLe16(header, 1U);
    appendLe16(header, channels);
    appendLe32(header, sampleRate);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2U);
    appendLe32(header, sampleRate * static_cast<std::uint32_t>(blockAlign));
    appendLe16(header, blockAlign);
    appendLe16(header, 16U);
    header.append("data", 4);
    appendLe32(header, dataBytes);
    return header;
}

std::vector<std::int16_t> readPcm16Samples(
    const QString& path,
    const jam2::wav::Pcm16Info& info)
{
    if (info.data_bytes > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::length_error("WAV audio data is too large to import");
    }
    std::vector<std::int16_t> samples(
        static_cast<std::size_t>(info.data_bytes / sizeof(std::int16_t)));
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) ||
        !source.seek(static_cast<qint64>(info.data_offset))) {
        throw std::runtime_error("could not open WAV audio data for resampling");
    }
    char* destination = reinterpret_cast<char*>(samples.data());
    qint64 remaining = static_cast<qint64>(info.data_bytes);
    while (remaining > 0) {
        const qint64 read = source.read(destination, remaining);
        if (read <= 0) {
            throw std::runtime_error("failed while reading WAV audio data for resampling");
        }
        destination += read;
        remaining -= read;
    }
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
    for (std::int16_t& sample : samples) {
        sample = qFromLittleEndian(sample);
    }
#endif
    return samples;
}

QByteArray pcm16Bytes(const std::vector<std::int16_t>& samples)
{
    const auto byteCount = static_cast<qsizetype>(
        samples.size() * sizeof(std::int16_t));
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    return QByteArray::fromRawData(
        reinterpret_cast<const char*>(samples.data()), byteCount);
#else
    QByteArray bytes(byteCount, Qt::Uninitialized);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        qToLittleEndian(samples[index], bytes.data() + index * sizeof(std::int16_t));
    }
    return bytes;
#endif
}

bool isManagedPracticeReference(const LooperLane& lane)
{
    return !lane.referenceKind.isEmpty() ||
        lane.name == QStringLiteral("Practice Chords") ||
        lane.name == QStringLiteral("Practice Drums") ||
        lane.name == QStringLiteral("Practice Melody") ||
        lane.name == QStringLiteral("Practice Bass") ||
        lane.name == QStringLiteral("Practice Support");
}

QString laneMergeKey(const QJsonObject& lane, int index)
{
    const QString id = lane.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) return QStringLiteral("id:") + id;
    const QString hash = lane.value(QStringLiteral("asset_hash")).toString();
    if (!hash.isEmpty()) return QStringLiteral("hash:") + hash;
    return QStringLiteral("index:%1").arg(index);
}

QMap<QString, QJsonObject> lanesByKey(const QJsonArray& lanes)
{
    QMap<QString, QJsonObject> mapped;
    for (int index = 0; index < lanes.size(); ++index) {
        mapped.insert(laneMergeKey(lanes.at(index).toObject(), index),
            lanes.at(index).toObject());
    }
    return mapped;
}

QStringList laneOrder(const QJsonArray& lanes)
{
    QStringList order;
    for (int index = 0; index < lanes.size(); ++index) {
        order.append(laneMergeKey(lanes.at(index).toObject(), index));
    }
    return order;
}

QJsonObject mergeLaneObject(
    const QJsonObject& base,
    const QJsonObject& current,
    const QJsonObject& proposed,
    int& mergedChanges,
    int& conflicts)
{
    QJsonObject merged = current;
    QSet<QString> keys;
    for (auto it = base.begin(); it != base.end(); ++it) keys.insert(it.key());
    for (auto it = current.begin(); it != current.end(); ++it) keys.insert(it.key());
    for (auto it = proposed.begin(); it != proposed.end(); ++it) keys.insert(it.key());
    for (const QString& key : std::as_const(keys)) {
        const QJsonValue baseValue = base.value(key);
        const QJsonValue currentValue = current.value(key);
        const QJsonValue proposedValue = proposed.value(key);
        if (proposedValue == baseValue || proposedValue == currentValue) continue;
        if (currentValue == baseValue) {
            if (proposedValue.isUndefined()) merged.remove(key);
            else merged.insert(key, proposedValue);
            ++mergedChanges;
        } else {
            ++conflicts;
        }
    }
    return merged;
}

} // namespace

WavMetadata readWavMetadata(const QString& path)
{
    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(nativeFilePath(path));
    if (!inspected) throw std::runtime_error(inspected.error);
    WavMetadata meta;
    meta.audioFormat = 1;
    meta.sampleRate = static_cast<int>(inspected.info.sample_rate);
    meta.channels = static_cast<int>(inspected.info.channels);
    meta.bitsPerSample = 16;
    meta.dataBytes = static_cast<qint64>(inspected.info.data_bytes);
    meta.frames = static_cast<qint64>(inspected.info.frames);
    const std::uint64_t durationMs = inspected.info.frames * 1000ULL / inspected.info.sample_rate;
    meta.durationMs = static_cast<int>(std::min<std::uint64_t>(
        durationMs,
        std::numeric_limits<int>::max()));
    meta.sha256 = sha256FileHex(path);
    if (meta.sha256.isEmpty()) throw std::runtime_error("failed to hash WAV");
    return meta;
}

StagedPcm16Asset stagePcm16Asset(
    const QString& sourcePath,
    const QString& stagingFolder,
    int expectedSampleRate,
    const QString& assetFolder)
{
    StagedPcm16Asset result;
    const QFileInfo sourceInfo(sourcePath);
    result.sourcePath = sourceInfo.absoluteFilePath();
    result.displayName = sourceInfo.completeBaseName();
    try {
        result.metadata = readWavMetadata(result.sourcePath);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        return result;
    }
    if (result.metadata.dataBytes <= 0) {
        result.error = QStringLiteral("WAV contains no audio frames");
        return result;
    }
    result.sourceSampleRate = result.metadata.sampleRate;
    result.sourceFrames = result.metadata.frames;
    if (expectedSampleRate > 0 && result.metadata.sampleRate != expectedSampleRate) {
        try {
            const jam2::wav::InspectResult inspected =
                jam2::wav::inspect_pcm16_file(nativeFilePath(result.sourcePath));
            if (!inspected) throw std::runtime_error(inspected.error);
            const std::uint64_t bytesPerFrame =
                static_cast<std::uint64_t>(inspected.info.channels) * sizeof(std::int16_t);
            const std::uint64_t maximumDataBytes = std::min<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max() - 36ULL,
                jam2::wav::kDefaultMaxFileBytes - 44ULL);
            const std::uint64_t maximumOutputFrames = maximumDataBytes / bytesPerFrame;
            const long double exactOutputFrames =
                static_cast<long double>(inspected.info.frames) *
                static_cast<long double>(expectedSampleRate) /
                static_cast<long double>(inspected.info.sample_rate);
            if (!std::isfinite(exactOutputFrames) ||
                exactOutputFrames > static_cast<long double>(maximumOutputFrames) + 0.499L) {
                throw std::length_error("resampled WAV exceeds the managed file-size limit");
            }
            const std::uint64_t expectedOutputFrames = std::max<std::uint64_t>(
                1ULL,
                static_cast<std::uint64_t>(std::llround(exactOutputFrames)));
            std::vector<std::int16_t> sourceSamples =
                readPcm16Samples(result.sourcePath, inspected.info);
            std::vector<std::int16_t> converted =
                jam2::gui::resample_pcm16_interleaved(
                    sourceSamples,
                    static_cast<int>(inspected.info.channels),
                    static_cast<int>(inspected.info.sample_rate),
                    expectedSampleRate);
            if (converted.size() != expectedOutputFrames * inspected.info.channels) {
                throw std::runtime_error("resampler produced an unexpected frame count");
            }

            const std::uint64_t dataBytes64 =
                static_cast<std::uint64_t>(converted.size()) * sizeof(std::int16_t);
            if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() - 36ULL ||
                dataBytes64 + 44ULL > jam2::wav::kDefaultMaxFileBytes) {
                throw std::length_error("resampled WAV exceeds the managed file-size limit");
            }
            const std::uint32_t dataBytes = static_cast<std::uint32_t>(dataBytes64);
            const QByteArray header = pcm16WavHeader(
                inspected.info.channels,
                static_cast<std::uint32_t>(expectedSampleRate),
                dataBytes);
            const QByteArray audioBytes = pcm16Bytes(converted);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(header);
            hash.addData(audioBytes);
            result.sha256 = QString::fromLatin1(hash.result().toHex());
            result.stagedPath = QDir(stagingFolder).absoluteFilePath(
                assetFolder + QLatin1Char('/') + result.sha256 + QStringLiteral(".wav"));
            if (!QDir().mkpath(QFileInfo(result.stagedPath).absolutePath())) {
                throw std::runtime_error("could not create the WAV staging folder");
            }
            if (!QFileInfo::exists(result.stagedPath) ||
                sha256FileHex(result.stagedPath) != result.sha256) {
                QSaveFile destination(result.stagedPath);
                if (!destination.open(QIODevice::WriteOnly) ||
                    destination.write(header) != header.size() ||
                    destination.write(audioBytes) != audioBytes.size() ||
                    !destination.commit()) {
                    throw std::runtime_error("could not atomically write the resampled WAV");
                }
            }

            result.metadata.audioFormat = 1;
            result.metadata.sampleRate = expectedSampleRate;
            result.metadata.channels = static_cast<int>(inspected.info.channels);
            result.metadata.bitsPerSample = 16;
            result.metadata.dataBytes = static_cast<qint64>(dataBytes);
            result.metadata.frames = static_cast<qint64>(
                converted.size() / inspected.info.channels);
            result.metadata.durationMs = static_cast<int>(
                std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(result.metadata.frames) * 1000ULL /
                        static_cast<std::uint64_t>(expectedSampleRate),
                    std::numeric_limits<int>::max()));
            result.metadata.sha256 = result.sha256;
            result.resampled = true;
            return result;
        } catch (const std::exception& error) {
            result.stagedPath.clear();
            result.sha256.clear();
            result.error = QStringLiteral("could not resample WAV from %1 Hz to %2 Hz: %3")
                .arg(result.sourceSampleRate)
                .arg(expectedSampleRate)
                .arg(QString::fromUtf8(error.what()));
            return result;
        }
    }
    result.sha256 = result.metadata.sha256;
    const QString managedFolder = QDir(stagingFolder).absoluteFilePath(assetFolder);
    if (assetFolder == QStringLiteral("recorded") &&
        QDir::cleanPath(sourceInfo.absolutePath()) == QDir::cleanPath(managedFolder)) {
        result.stagedPath = result.sourcePath;
        return result;
    }
    result.stagedPath = QDir(stagingFolder).absoluteFilePath(
        assetFolder + QLatin1Char('/') + result.sha256 + QStringLiteral(".wav"));
    if (!QDir().mkpath(QFileInfo(result.stagedPath).absolutePath())) {
        result.error = QStringLiteral("could not create the WAV staging folder");
        return result;
    }
    if (QFileInfo::exists(result.stagedPath) &&
        sha256FileHex(result.stagedPath) == result.sha256) {
        return result;
    }
    QFile source(result.sourcePath);
    QSaveFile destination(result.stagedPath);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("could not open the WAV for atomic staging");
        return result;
    }
    constexpr qint64 kCopyBlockBytes = 1024 * 1024;
    while (!source.atEnd()) {
        const QByteArray block = source.read(kCopyBlockBytes);
        if (block.isEmpty() && source.error() != QFileDevice::NoError) {
            result.error = QStringLiteral("failed while reading the WAV for staging");
            return result;
        }
        if (destination.write(block) != block.size()) {
            result.error = QStringLiteral("failed while writing the staged WAV");
            return result;
        }
    }
    if (!destination.commit()) {
        result.error = QStringLiteral("could not atomically commit the staged WAV");
    }
    return result;
}

int mergeSynchronizedLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject)
{
    LooperProject received;
    const QJsonObject receivedLooper = song.value(QStringLiteral("looper")).toObject();
    if (receivedLooper.isEmpty() || !received.loadJson(receivedLooper)) return 0;

    int preserved = 0;
    for (int bankIndex = 0;
         bankIndex < localProject.banks().size() && bankIndex < received.banks().size();
         ++bankIndex) {
        auto& remoteLanes = received.banks()[bankIndex].lanes;
        const QVector<LooperLane>& localLanes = localProject.banks().at(bankIndex).lanes;
        for (LooperLane& remote : remoteLanes) {
            auto match = std::find_if(
                localLanes.cbegin(), localLanes.cend(),
                [&remote](const LooperLane& local) {
                    if (!remote.assetHash.isEmpty() &&
                        local.assetHash == remote.assetHash) {
                        return true;
                    }
                    if (remote.id.isEmpty() || local.id != remote.id) return false;
                    return local.assetHash == remote.assetHash ||
                        (local.assetHash.isEmpty() && local.assetPath.trimmed().isEmpty());
                });
            if (match != localLanes.cend()) {
                const double gainDb = match->gainDb;
                const bool muted = match->muted;
                const bool solo = match->solo;
                const bool localOnly = match->localOnly;
                const QString localPath = match->assetPath;
                remote.gainDb = gainDb;
                remote.muted = muted;
                remote.solo = solo;
                remote.localOnly = localOnly;
                if (!localPath.trimmed().isEmpty() && !remote.assetHash.isEmpty()) {
                    remote.assetPath = localPath;
                }
            }
        }

        for (const LooperLane& local : localLanes) {
            const bool alreadyPresent = std::any_of(
                remoteLanes.cbegin(), remoteLanes.cend(),
                [&local](const LooperLane& remote) {
                    if (!local.assetHash.isEmpty() &&
                        remote.assetHash == local.assetHash) {
                        return true;
                    }
                    if (local.id.isEmpty() || remote.id != local.id) return false;
                    return remote.assetHash == local.assetHash ||
                        (local.assetHash.isEmpty() && local.assetPath.trimmed().isEmpty());
                });
            if (alreadyPresent) continue;

            const bool conflictingSameLaneId = std::any_of(
                remoteLanes.cbegin(), remoteLanes.cend(),
                [&local](const LooperLane& remote) {
                    return !local.id.isEmpty() && remote.id == local.id;
                });
            if (!isManagedPracticeReference(local) && !conflictingSameLaneId) {
                continue;
            }

            LooperLane preservedLane = local;
            if (conflictingSameLaneId) preservedLane.id.clear();
            if (received.appendLane(bankIndex, std::move(preservedLane))) ++preserved;
        }
    }
    song.insert(QStringLiteral("looper"), received.toJson());
    return preserved;
}

int mergeQuarantinedLocalLanes(
    QJsonObject& song,
    const LooperProject& localProject,
    int expectedSampleRate)
{
    LooperProject received;
    const QJsonObject receivedLooper = song.value(QStringLiteral("looper")).toObject();
    if (receivedLooper.isEmpty() || !received.loadJson(receivedLooper)) return 0;
    int preserved = 0;
    for (int bankIndex = 0;
         bankIndex < localProject.banks().size() && bankIndex < received.banks().size();
        ++bankIndex) {
        for (const LooperLane& local : localProject.banks().at(bankIndex).lanes) {
            const bool incompatible =
                expectedSampleRate > 0 && local.sampleRate != expectedSampleRate;
            if (local.assetPath.trimmed().isEmpty() ||
                isManagedPracticeReference(local) ||
                (!local.localOnly && !incompatible)) {
                continue;
            }
            const bool alreadyPresent = std::any_of(
                received.banks().at(bankIndex).lanes.cbegin(),
                received.banks().at(bankIndex).lanes.cend(),
                [&local](const LooperLane& candidate) {
                    return !local.assetHash.isEmpty() && candidate.assetHash == local.assetHash;
                });
            if (alreadyPresent) continue;
            LooperLane quarantined = local;
            const bool idCollision = std::any_of(
                received.banks().at(bankIndex).lanes.cbegin(),
                received.banks().at(bankIndex).lanes.cend(),
                [&quarantined](const LooperLane& candidate) {
                    return !quarantined.id.isEmpty() && candidate.id == quarantined.id;
                });
            if (idCollision) quarantined.id.clear();
            if (incompatible) quarantined.sampleRateCompatible = false;
            if (received.appendLane(bankIndex, std::move(quarantined))) ++preserved;
        }
    }
    if (preserved > 0) song.insert(QStringLiteral("looper"), received.toJson());
    return preserved;
}

QJsonObject mergeConcurrentLooperMetadata(
    const QJsonObject& baseSong,
    const QJsonObject& currentSong,
    const QJsonObject& proposedSong,
    int* mergedChanges,
    int* conflicts)
{
    int changes = 0;
    int collisions = 0;
    QJsonObject result = currentSong;
    const QJsonObject baseLooper = baseSong.value(QStringLiteral("looper")).toObject();
    QJsonObject currentLooper = currentSong.value(QStringLiteral("looper")).toObject();
    const QJsonObject proposedLooper = proposedSong.value(QStringLiteral("looper")).toObject();
    QJsonArray baseBanks = baseLooper.value(QStringLiteral("banks")).toArray();
    QJsonArray currentBanks = currentLooper.value(QStringLiteral("banks")).toArray();
    const QJsonArray proposedBanks = proposedLooper.value(QStringLiteral("banks")).toArray();
    const int bankCount = qMin(currentBanks.size(), proposedBanks.size());
    for (int bankIndex = 0; bankIndex < bankCount; ++bankIndex) {
        const QJsonObject baseBank = bankIndex < baseBanks.size()
            ? baseBanks.at(bankIndex).toObject() : QJsonObject{};
        QJsonObject currentBank = currentBanks.at(bankIndex).toObject();
        const QJsonObject proposedBank = proposedBanks.at(bankIndex).toObject();
        const QJsonArray baseLanes = baseBank.value(QStringLiteral("lanes")).toArray();
        const QJsonArray currentLanes = currentBank.value(QStringLiteral("lanes")).toArray();
        const QJsonArray proposedLanes = proposedBank.value(QStringLiteral("lanes")).toArray();
        const auto baseMap = lanesByKey(baseLanes);
        const auto currentMap = lanesByKey(currentLanes);
        const auto proposedMap = lanesByKey(proposedLanes);
        QMap<QString, QJsonObject> mergedMap;
        QSet<QString> laneKeys;
        for (auto it = baseMap.cbegin(); it != baseMap.cend(); ++it) laneKeys.insert(it.key());
        for (auto it = currentMap.cbegin(); it != currentMap.cend(); ++it) laneKeys.insert(it.key());
        for (auto it = proposedMap.cbegin(); it != proposedMap.cend(); ++it) laneKeys.insert(it.key());
        for (const QString& key : std::as_const(laneKeys)) {
            const bool inBase = baseMap.contains(key);
            const bool inCurrent = currentMap.contains(key);
            const bool inProposed = proposedMap.contains(key);
            if (!inBase) {
                if (inCurrent) mergedMap.insert(key, currentMap.value(key));
                if (inProposed && !inCurrent) {
                    mergedMap.insert(key, proposedMap.value(key));
                    ++changes;
                } else if (inProposed && inCurrent &&
                           proposedMap.value(key) != currentMap.value(key)) {
                    ++collisions;
                }
                continue;
            }
            if (!inCurrent && !inProposed) continue;
            if (!inCurrent) {
                if (proposedMap.value(key) != baseMap.value(key)) {
                    mergedMap.insert(key, proposedMap.value(key));
                    ++changes;
                    ++collisions;
                } else {
                    ++changes;
                }
                continue;
            }
            if (!inProposed) {
                if (currentMap.value(key) != baseMap.value(key)) {
                    mergedMap.insert(key, currentMap.value(key));
                    ++collisions;
                } else {
                    ++changes;
                }
                continue;
            }
            mergedMap.insert(key, mergeLaneObject(
                baseMap.value(key), currentMap.value(key), proposedMap.value(key),
                changes, collisions));
        }
        QStringList order = laneOrder(currentLanes) == laneOrder(baseLanes)
            ? laneOrder(proposedLanes) : laneOrder(currentLanes);
        for (auto it = mergedMap.cbegin(); it != mergedMap.cend(); ++it) {
            if (!order.contains(it.key())) order.append(it.key());
        }
        QJsonArray mergedLanes;
        for (const QString& key : std::as_const(order)) {
            if (mergedMap.contains(key)) mergedLanes.append(mergedMap.value(key));
        }
        currentBank.insert(QStringLiteral("lanes"), mergedLanes);
        for (const QString& key : {QStringLiteral("id"), QStringLiteral("timing")}) {
            const QJsonValue baseValue = baseBank.value(key);
            const QJsonValue currentValue = currentBank.value(key);
            const QJsonValue proposedValue = proposedBank.value(key);
            if (proposedValue != baseValue && proposedValue != currentValue) {
                if (currentValue == baseValue) {
                    currentBank.insert(key, proposedValue);
                    ++changes;
                } else {
                    ++collisions;
                }
            }
        }
        currentBanks.replace(bankIndex, currentBank);
    }
    currentLooper.insert(QStringLiteral("banks"), currentBanks);
    for (const QString& key : {QStringLiteral("arrangement"), QStringLiteral("active_bank")}) {
        const QJsonValue baseValue = baseLooper.value(key);
        const QJsonValue currentValue = currentLooper.value(key);
        const QJsonValue proposedValue = proposedLooper.value(key);
        if (proposedValue != baseValue && proposedValue != currentValue) {
            if (currentValue == baseValue) {
                currentLooper.insert(key, proposedValue);
                ++changes;
            } else {
                ++collisions;
            }
        }
    }
    result.insert(QStringLiteral("looper"), currentLooper);
    if (mergedChanges) *mergedChanges = changes;
    if (conflicts) *conflicts = collisions;
    return result;
}
