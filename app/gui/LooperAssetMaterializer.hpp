#pragma once

#include "LooperProject.hpp"

#include <QString>
#include <QStringList>

namespace jam2::gui {

struct LooperAssetMaterializationResult {
    LooperProject project;
    QStringList createdPaths;
    QString error;

    bool succeeded() const noexcept { return error.isEmpty(); }
};

// Copies externally referenced lane WAVs into a JamJar folder without
// overwriting unrelated content. The returned project contains only paths
// relative to targetProjectFolder. A failed transaction rolls back everything
// it can and leaves any cleanup failures in createdPaths; a caller that cannot
// commit a successful candidate must pass createdPaths to the rollback helper.
LooperAssetMaterializationResult materializeLooperAssets(
    const LooperProject& sourceProject,
    const QString& sourceProjectFolder,
    const QString& targetProjectFolder);

QStringList rollbackLooperAssetMaterialization(
    const QStringList& createdPaths) noexcept;

} // namespace jam2::gui
