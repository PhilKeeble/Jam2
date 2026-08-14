#pragma once

#include <QJsonObject>

// Three-way reconciliation for concurrent Track-lane metadata proposals. The
// base is the authoritative model both edits started from, current is the
// already-accepted branch, and proposed is the later branch being rebased.
QJsonObject mergeConcurrentLooperMetadata(
    const QJsonObject& baseSong,
    const QJsonObject& currentSong,
    const QJsonObject& proposedSong,
    int* mergedChanges = nullptr,
    int* conflicts = nullptr);
