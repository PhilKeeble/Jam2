#include "ProjectPersistenceCoordinator.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRunnable>
#include <QSaveFile>
#include <QThreadPool>

#include <utility>

namespace {

constexpr qint64 kMaxSongFileBytes = 4LL * 1024LL * 1024LL;

}

void ProjectPersistenceCoordinator::initializeWorkspace(const QString& workspaceFolder)
{
    workspaceFolder_ = QDir(workspaceFolder).absolutePath();
    (void)QDir().mkpath(QDir(workspaceFolder_).absoluteFilePath(QStringLiteral("wavs")));
}

const QString& ProjectPersistenceCoordinator::projectFilePath() const noexcept
{
    return projectFilePath_;
}

const QString& ProjectPersistenceCoordinator::projectFolder() const noexcept
{
    return projectFolder_;
}

const QString& ProjectPersistenceCoordinator::workspaceFolder() const noexcept
{
    return workspaceFolder_;
}

QString ProjectPersistenceCoordinator::workingProjectFolder() const
{
    return projectFolder_.isEmpty() ? workspaceFolder_ : projectFolder_;
}

void ProjectPersistenceCoordinator::useWorkspaceAsProjectFolderIfUnset()
{
    if (projectFolder_.isEmpty()) {
        projectFolder_ = workspaceFolder_;
    }
}

void ProjectPersistenceCoordinator::setProjectFolder(const QString& folder)
{
    projectFolder_ = folder.trimmed().isEmpty() ? QString{} : QDir(folder).absolutePath();
}

void ProjectPersistenceCoordinator::setProjectLocation(const QString& path)
{
    const QFileInfo info(path);
    projectFilePath_ = info.absoluteFilePath();
    projectFolder_ = info.absolutePath();
}

void ProjectPersistenceCoordinator::acceptNewProject(const QByteArray& snapshot)
{
    projectFilePath_.clear();
    projectFolder_.clear();
    savedSnapshot_ = snapshot;
}

void ProjectPersistenceCoordinator::acceptOpenedProject(
    const QString& path,
    const QByteArray& snapshot)
{
    setProjectLocation(path);
    savedSnapshot_ = snapshot;
}

void ProjectPersistenceCoordinator::acceptSavedProject(
    const QString& path,
    const QByteArray& snapshot,
    const QSet<QString>& persistentAssetPaths)
{
    const QFileInfo info(path);
    projectFilePath_ = info.absoluteFilePath();
    projectFolder_ = info.absolutePath();
    for (const QString& assetPath : persistentAssetPaths) {
        transientWavs_.remove(canonicalFilePath(assetPath));
    }
    savedSnapshot_ = snapshot;
    deferredCleanupWavs_.unite(transientWavs_);
    transientWavs_.clear();
}

bool ProjectPersistenceCoordinator::hasUnsavedChanges(
    const QByteArray& currentSnapshot) const noexcept
{
    return currentSnapshot != savedSnapshot_;
}

void ProjectPersistenceCoordinator::registerTransientWav(const QString& path)
{
    if (!path.trimmed().isEmpty()) {
        transientWavs_.insert(canonicalFilePath(path));
    }
}

bool ProjectPersistenceCoordinator::hasExistingTransientWavs() const
{
    for (const QString& path : transientWavs_) {
        if (QFileInfo::exists(path)) {
            return true;
        }
    }
    return false;
}

void ProjectPersistenceCoordinator::scheduleTransientCleanup(QThreadPool& workerPool)
{
    QSet<QString> paths = std::move(transientWavs_);
    transientWavs_.clear();
    paths.unite(deferredCleanupWavs_);
    deferredCleanupWavs_.clear();
    const QString workspacePath = workspaceFolder_;
    workerPool.start(QRunnable::create([paths = std::move(paths), workspacePath] {
        for (const QString& path : paths) {
            const QFileInfo info(path);
            if (info.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0 &&
                info.exists()) {
                (void)QFile::remove(info.absoluteFilePath());
            }
        }
        QDir workspace(workspacePath);
        workspace.rmdir(QStringLiteral("wavs"));
        QDir parent = workspace;
        if (parent.cdUp()) {
            parent.rmdir(workspace.dirName());
        }
    }));
}

bool ProjectPersistenceCoordinator::readSongJson(
    const QString& path,
    QJsonObject& root,
    QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > kMaxSongFileBytes) {
        error = QStringLiteral("Could not open song file within the 4 MiB limit.");
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        error = QStringLiteral("Invalid Jam2 song JSON.");
        return false;
    }
    root = document.object();
    return true;
}

bool ProjectPersistenceCoordinator::writeSongJson(
    const QString& path,
    const QByteArray& json,
    QString& error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
        error = QStringLiteral("Could not atomically write the song file.");
        return false;
    }
    return true;
}

QString ProjectPersistenceCoordinator::canonicalFilePath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
