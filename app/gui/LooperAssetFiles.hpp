#pragma once

#include <QString>
#include <QStringList>

namespace jam2::gui::looper_asset_files {

QString receivedPath(const QString& workspaceFolder, const QString& hash);

QStringList validationCandidates(
    const QString& workspaceFolder,
    const QString& assetFolder,
    const QString& hash,
    const QString& localPath = {});

} // namespace jam2::gui::looper_asset_files
