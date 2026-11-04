#include "ApplicationPaths.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString installation = appReleaseDir().absolutePath();
#ifdef Q_OS_MACOS
        require(appDataDir().absolutePath() == QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                .filePath(QStringLiteral("Jam2")) && appDataDir().absolutePath() != installation,
            "unmarked macOS applications use stable Application Support, independent of the installation");
#else
        require(appDataDir().absolutePath() == installation, "Windows retains portable storage");
#endif
        QTemporaryDir fixture;
        require(fixture.isValid(), "create private data root");
#ifdef Q_OS_MACOS
        const QDir sourceBuild(fixture.filePath(QStringLiteral("release")));
        const QString support = fixture.filePath(QStringLiteral("Library/Application Support"));
        const QString productionData = QDir(support).filePath(QStringLiteral("Jam2"));
        require(QDir().mkpath(sourceBuild.path()), "create local source-build output");
        require(macAppDataDir(sourceBuild, support).absolutePath() == productionData,
            "a folder named release alone never opts a distribution app into local writes");
        const QString markerPath = sourceBuild.filePath(QStringLiteral(".jam2-development-data"));
        QFile marker(markerPath);
        require(marker.open(QIODevice::WriteOnly), "create the source-build marker beside the app"); marker.close();
        require(macAppDataDir(sourceBuild, support).absolutePath() == sourceBuild.absolutePath() &&
                macAppDataDir(sourceBuild, {}).absolutePath() == sourceBuild.absolutePath(),
            "explicit source builds use local data without requiring Application Support");
        require(marker.remove(), "remove local source-build marker");
        require(macAppDataDir(sourceBuild, support).absolutePath() == productionData,
            "the same executable without its external marker uses production data");
        require(QDir().mkdir(markerPath), "create invalid marker directory");
        require(macAppDataDir(sourceBuild, support).absolutePath() == productionData,
            "a directory cannot opt into development storage");
        require(QDir().rmdir(markerPath), "remove invalid marker directory");
        QFile target(fixture.filePath(QStringLiteral("marker-target")));
        require(target.open(QIODevice::WriteOnly), "create symlink target"); target.close();
        require(QFile::link(target.fileName(), markerPath) &&
                macAppDataDir(sourceBuild, support).absolutePath() == productionData,
            "a symlink cannot opt into development storage");
        bool supportFailed = false;
        try { macAppDataDir(sourceBuild, {}); }
        catch (const std::exception& failure) { supportFailed = QString::fromUtf8(failure.what()).contains(QStringLiteral("Application Support")); }
        require(supportFailed, "missing production user storage is reported explicitly");
#endif
        QString error;
        require(!setAppDataRootForTesting(QStringLiteral("relative"), error), "reject a relative data root");
        require(setAppDataRootForTesting(fixture.path(), error), "select isolated writable data root");
        require(appReleaseDir().absolutePath() == installation, "data isolation never changes helper/resource discovery");
        for (const QString& folder : {QStringLiteral("settings"), QStringLiteral("logs"), QStringLiteral("tracks"),
                QStringLiteral("songs"), QStringLiteral("captures"), QStringLiteral("cache"), QStringLiteral("temp"),
                QStringLiteral("update"), QStringLiteral("components/jamtaster/models")}) {
            const auto path = appDataFilePath(folder, QStringLiteral("write-check"));
            QFile file(path);
            require(path.startsWith(fixture.path() + QLatin1Char('/')) && file.open(QIODevice::WriteOnly) && file.write("ok") == 2,
                "every managed runtime folder is writable beneath the data root");
        }
        QFile blocked(fixture.filePath(QStringLiteral("blocked")));
        require(blocked.open(QIODevice::WriteOnly), "create invalid directory fixture"); blocked.close();
        bool failed = false;
        try { appDataFolderPath(QStringLiteral("blocked/child")); }
        catch (const std::exception& failure) { failed = QString::fromUtf8(failure.what()).contains(QStringLiteral("blocked/child")); }
        require(failed, "directory creation errors report the intended data path");
        std::cout << "Application installation and writable-data paths passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
