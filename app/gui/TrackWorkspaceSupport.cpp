#include "TrackWorkspaceSupport.hpp"

#include "pcm16_wav.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

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
    if (expectedSampleRate > 0 && result.metadata.sampleRate != expectedSampleRate) {
        result.error = QStringLiteral(
            "Sample-rate mismatch: this jam uses %1 Hz but the WAV is %2 Hz. "
            "The WAV was not loaded; convert it or use a %1 Hz source.")
            .arg(expectedSampleRate)
            .arg(result.metadata.sampleRate);
        return result;
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
