#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStandardPaths>

#include <stdexcept>

// Shared by GUI storage, preferences and private workers' staging paths.
inline QString& appDataRootOverride()
{
    static QString path;
    return path;
}

inline QDir appReleaseDir()
{
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

#ifdef Q_OS_MACOS
inline QDir macAppDataDir(const QDir& installation, const QString& applicationSupport)
{
    const QFileInfo developmentMarker(installation.filePath(QStringLiteral(".jam2-development-data")));
    if (developmentMarker.isFile() && !developmentMarker.isSymLink()) return installation;
    if (applicationSupport.isEmpty()) throw std::runtime_error("macOS Application Support is unavailable.");
    return QDir(QDir(applicationSupport).filePath(QStringLiteral("Jam2")));
}
#endif

// Source builds explicitly opt into local data using a marker beside the app.
// Distributed apps never require their bundle or its parent to be writable.
inline QDir appDataDir()
{
    if (!appDataRootOverride().isEmpty()) return QDir(appDataRootOverride());
#ifdef Q_OS_MACOS
    return macAppDataDir(appReleaseDir(), QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
#else
    return appReleaseDir();
#endif
}

inline QString appBundlePath()
{
    QDir directory(QCoreApplication::applicationDirPath());
    if (directory.dirName() == QStringLiteral("MacOS") && directory.cdUp() &&
        directory.dirName() == QStringLiteral("Contents") && directory.cdUp() &&
        directory.dirName().endsWith(QStringLiteral(".app"))) return directory.absolutePath();
    return {};
}

inline bool setAppDataRootForTesting(const QString& path, QString& error)
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
    appDataRootOverride() = clean;
    return true;
}

inline QString appDataFolderPath(const QString& folder)
{
    QDir directory = appDataDir();
    const QString path = directory.absoluteFilePath(folder);
    if (!QDir().mkpath(path))
        throw std::runtime_error(QStringLiteral("Could not create Jam2 data folder: %1").arg(path).toStdString());
    return path;
}

inline QString appDataFilePath(const QString& folder, const QString& fileName)
{
    return QDir(appDataFolderPath(folder)).absoluteFilePath(fileName);
}
