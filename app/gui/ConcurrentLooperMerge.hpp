#pragma once

#include <QJsonObject>

enum class LooperConflictPolicy { KeepCurrent, LatestReceived };

// Legacy rebasing permits intervening track-only edits. Server generated-Idea
// operations replace their declared scope atomically in later receive order.
bool canRebaseIdeaOverTrackEdits(const QJsonObject& baseSong, const QJsonObject& currentSong,
    LooperConflictPolicy policy = LooperConflictPolicy::KeepCurrent);

// Three-way reconciliation for concurrent Track-lane metadata proposals. The
// base is the authoritative model both edits started from, current is the
// already-accepted branch, and proposed is the later branch being rebased.
QJsonObject mergeConcurrentLooperMetadata(
    const QJsonObject& baseSong,
    const QJsonObject& currentSong,
    const QJsonObject& proposedSong,
    int* mergedChanges = nullptr,
    int* conflicts = nullptr,
    LooperConflictPolicy policy = LooperConflictPolicy::KeepCurrent);
