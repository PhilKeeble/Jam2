#include "ConcurrentLooperMerge.hpp"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>

#include <utility>

namespace {

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

QSet<QString> newAssetHashes(
    const QMap<QString, QJsonObject>& base,
    const QMap<QString, QJsonObject>& branch)
{
    QSet<QString> hashes;
    for (auto it = branch.cbegin(); it != branch.cend(); ++it) {
        if (base.contains(it.key())) continue;
        const QString hash = it.value().value(QStringLiteral("asset_hash"))
            .toString().toLower();
        if (!hash.isEmpty()) hashes.insert(hash);
    }
    return hashes;
}

} // namespace

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
        const QSet<QString> currentNewHashes = newAssetHashes(baseMap, currentMap);
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
                    const QString proposedHash = proposedMap.value(key)
                        .value(QStringLiteral("asset_hash")).toString().toLower();
                    // Two edits from the same base importing the same bytes are
                    // the same concurrent contribution. Keep the already-
                    // accepted lane. Do not collapse duplicates which existed
                    // in the base or were added by a later sequential edit.
                    if (!proposedHash.isEmpty() &&
                        currentNewHashes.contains(proposedHash)) {
                        continue;
                    }
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
