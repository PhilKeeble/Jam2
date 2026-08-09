#include "ProjectPersistenceCoordinator.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
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
    QDir received(QDir(workspaceFolder_).absoluteFilePath(QStringLiteral("received")));
    static const QRegularExpression partialName(QStringLiteral(
        "^[0-9a-f]{64}\\.wav\\.partial\\.[0-9a-f]{8}-[0-9a-f]{4}-"
        "[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    for (const QFileInfo& file : received.entryInfoList(
             QDir::Files | QDir::NoDotAndDotDot)) {
        if (partialName.match(file.fileName()).hasMatch()) {
            (void)QFile::remove(file.absoluteFilePath());
        }
    }
}

void ProjectPersistenceCoordinator::relocateWorkspace(const QString& workspaceFolder)
{
    const QString oldRoot = QDir(workspaceFolder_).absolutePath();
    const QString newRoot = QDir(workspaceFolder).absolutePath();
    const auto relocated = [&oldRoot, &newRoot](const QString& path) {
        const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const QString prefix = oldRoot + QLatin1Char('/');
        return absolute.startsWith(prefix, Qt::CaseInsensitive)
            ? QDir(newRoot).absoluteFilePath(absolute.mid(prefix.size()))
            : absolute;
    };
    QSet<QString> nextTransient;
    for (const QString& path : std::as_const(transientWavs_)) {
        nextTransient.insert(relocated(path));
    }
    QSet<QString> nextDeferred;
    for (const QString& path : std::as_const(deferredCleanupWavs_)) {
        nextDeferred.insert(relocated(path));
    }
    transientWavs_ = std::move(nextTransient);
    deferredCleanupWavs_ = std::move(nextDeferred);
    workspaceFolder_ = newRoot;
    if (projectFolder_.isEmpty() || QDir(projectFolder_).absolutePath() == oldRoot) {
        projectFolder_ = newRoot;
    }
}

void ProjectPersistenceCoordinator::clearTransientTracking() noexcept
{
    transientWavs_.clear();
    deferredCleanupWavs_.clear();
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

bool ProjectPersistenceCoordinator::ownsTransientWav(const QString& path) const
{
    const QString canonical = canonicalFilePath(path);
    return transientWavs_.contains(canonical) ||
        deferredCleanupWavs_.contains(canonical);
}

bool ProjectPersistenceCoordinator::discardTransientWav(const QString& path)
{
    const QString canonical = canonicalFilePath(path);
    if (!transientWavs_.contains(canonical) &&
        !deferredCleanupWavs_.contains(canonical)) {
        return false;
    }
    const QFileInfo info(canonical);
    if (info.exists() &&
        (!info.isFile() ||
         info.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0 ||
         !QFile::remove(canonical))) {
        return false;
    }
    transientWavs_.remove(canonical);
    deferredCleanupWavs_.remove(canonical);
    pruneEmptyWorkspace(workspaceFolder_);
    return true;
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

void ProjectPersistenceCoordinator::pruneEmptyWorkspaceDirectories() const
{
    pruneEmptyWorkspace(workspaceFolder_);
}

void ProjectPersistenceCoordinator::pruneEmptyWorkspace(const QString& workspacePath)
{
    if (workspacePath.trimmed().isEmpty()) return;
    QDir workspace(QDir(workspacePath).absolutePath());
    for (const QString& folder : {
             QStringLiteral("generated"),
             QStringLiteral("received"),
             QStringLiteral("imported"),
             QStringLiteral("recorded"),
             QStringLiteral("prepared")}) {
        (void)workspace.rmdir(folder);
    }
    QDir parent = workspace;
    if (parent.cdUp()) {
        (void)parent.rmdir(workspace.dirName());
    }
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
        ProjectPersistenceCoordinator::pruneEmptyWorkspace(workspacePath);
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
        error = QStringLiteral("Invalid JamJar JSON.");
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
