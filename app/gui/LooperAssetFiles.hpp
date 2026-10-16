#pragma once

#include <QString>
#include <QStringList>

namespace jam2::gui::looper_asset_files {

QString receivedPath(const QString& workspaceFolder, const QString& hash);

// Resolve published and live lane locations using the project root, never the
// process working directory. Missing published files may fall back to a lane.
QString sendPath(
    const QString& projectFolder,
    const QString& publishedPath,
    const QStringList& lanePaths);

QStringList validationCandidates(
    const QString& workspaceFolder,
    const QString& assetFolder,
    const QString& hash,
    const QString& localPath = {});

} // namespace jam2::gui::looper_asset_files
