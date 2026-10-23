#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

// Shared by GUI storage, preferences and private workers' staging paths.
inline QString& appReleaseRootOverride()
{
    static QString path;
    return path;
}

inline QDir appReleaseDir()
{
    if (!appReleaseRootOverride().isEmpty()) {
        return QDir(appReleaseRootOverride());
    }
    const QDir executable(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    QDir bundle(executable);
    if (bundle.dirName() == QStringLiteral("MacOS") && bundle.cdUp() &&
        bundle.dirName() == QStringLiteral("Contents") && bundle.cdUp() &&
        bundle.dirName().endsWith(QStringLiteral(".app")) && bundle.cdUp()) {
        return bundle;
    }
#endif
    return executable;
}

inline bool setAppReleaseRootForTesting(const QString& path, QString& error)
{
    error.clear();
    const QFileInfo info(path);
    if (!info.isAbsolute()) {
        error = QStringLiteral("test storage root must be absolute");
        return false;
    }
    const QString clean = QDir::cleanPath(info.absoluteFilePath());
    if (clean.isEmpty() || !QDir().mkpath(clean)) {
        error = QStringLiteral("test storage root could not be created");
        return false;
    }
    appReleaseRootOverride() = clean;
    return true;
}

inline QString appReleaseFilePath(const QString& folder, const QString& fileName)
{
    QDir directory = appReleaseDir();
    directory.mkpath(folder);
    return directory.absoluteFilePath(folder + QLatin1Char('/') + fileName);
}

inline QString appReleaseFolderPath(const QString& folder)
{
    QDir directory = appReleaseDir();
    directory.mkpath(folder);
    return directory.absoluteFilePath(folder);
}
