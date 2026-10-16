#include "LooperAssetFiles.hpp"

#include <QDir>
#include <QFileInfo>

namespace jam2::gui::looper_asset_files {

QString receivedPath(const QString& workspaceFolder, const QString& hash)
{
    return QDir(workspaceFolder).absoluteFilePath(
        QStringLiteral("received/") + hash + QStringLiteral(".wav"));
}

QString sendPath(
    const QString& projectFolder,
    const QString& publishedPath,
    const QStringList& lanePaths)
{
    const auto existingPath = [&projectFolder](const QString& path) {
        if (path.trimmed().isEmpty()) return QString{};
        QString resolved = path;
        if (!QDir::isAbsolutePath(path)) {
            if (projectFolder.isEmpty() || !QDir::isAbsolutePath(projectFolder)) return QString{};
            resolved = QDir(projectFolder).absoluteFilePath(path);
        }
        resolved = QDir::cleanPath(resolved);
        return QFileInfo(resolved).isFile() ? resolved : QString{};
    };
    const QString published = existingPath(publishedPath);
    if (!published.isEmpty()) return published;
    for (const QString& path : lanePaths) {
        const QString resolved = existingPath(path);
        if (!resolved.isEmpty()) return resolved;
    }
    return {};
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
