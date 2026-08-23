#include "LooperAssetFiles.hpp"

#include <QDir>

namespace jam2::gui::looper_asset_files {

QString receivedPath(const QString& workspaceFolder, const QString& hash)
{
    return QDir(workspaceFolder).absoluteFilePath(
        QStringLiteral("received/") + hash + QStringLiteral(".wav"));
}

QStringList validationCandidates(
    const QString& workspaceFolder,
    const QString& assetFolder,
    const QString& hash,
    const QString& localPath)
{
    QStringList candidates;
    const auto appendUnique = [&candidates](const QString& path) {
        if (!path.isEmpty() && !candidates.contains(path)) candidates.append(path);
    };
    appendUnique(QDir(assetFolder).absoluteFilePath(hash + QStringLiteral(".wav")));
    appendUnique(receivedPath(workspaceFolder, hash));
    appendUnique(localPath);
    return candidates;
}

} // namespace jam2::gui::looper_asset_files
