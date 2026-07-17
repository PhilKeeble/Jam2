#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

class QThreadPool;

class ProjectPersistenceCoordinator {
public:
    void initializeWorkspace(const QString& workspaceFolder);

    const QString& projectFilePath() const noexcept;
    const QString& projectFolder() const noexcept;
    const QString& workspaceFolder() const noexcept;
    QString workingProjectFolder() const;
    void useWorkspaceAsProjectFolderIfUnset();
    void setProjectFolder(const QString& folder);
    void setProjectLocation(const QString& path);

    void acceptNewProject(const QByteArray& snapshot);
    void acceptOpenedProject(const QString& path, const QByteArray& snapshot);
    void acceptSavedProject(
        const QString& path,
        const QByteArray& snapshot,
        const QSet<QString>& persistentAssetPaths);
    bool hasUnsavedChanges(const QByteArray& currentSnapshot) const noexcept;

    void registerTransientWav(const QString& path);
    bool ownsTransientWav(const QString& path) const;
    bool discardTransientWav(const QString& path);
    bool hasExistingTransientWavs() const;
    void scheduleTransientCleanup(QThreadPool& workerPool);

    static bool readSongJson(const QString& path, QJsonObject& root, QString& error);
    static bool writeSongJson(const QString& path, const QByteArray& json, QString& error);

private:
    static QString canonicalFilePath(const QString& path);

    QString projectFolder_;
    QString projectFilePath_;
    QString workspaceFolder_;
    QByteArray savedSnapshot_;
    QSet<QString> transientWavs_;
    QSet<QString> deferredCleanupWavs_;
};
