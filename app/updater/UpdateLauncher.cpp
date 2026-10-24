#include "UpdateLauncher.hpp"
#include "UpdatePackage.hpp"
#include "UpdaterPlatform.hpp"
#include "../application/ApplicationPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>

#include <filesystem>
#include <stdexcept>

namespace jam2::update {
namespace {
void require(bool condition, const QString& error)
{
    if (!condition) throw std::runtime_error(error.toStdString());
}
void copyFile(const QString& source, const QString& destination)
{
    require(QFileInfo(source).isFile() && QDir().mkpath(QFileInfo(destination).absolutePath()) &&
        QFile::copy(source, destination), QStringLiteral("Could not stage updater runtime file: %1").arg(source));
}
#ifdef Q_OS_MACOS
void copyTree(const QString& source, const QString& destination)
{
    require(QFileInfo(source).isDir() && QDir().mkpath(destination), QStringLiteral("Missing updater runtime folder: %1").arg(source));
    for (const auto& info : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const QString target = QDir(destination).filePath(info.fileName());
        if (info.isSymLink()) {
            const auto link = std::filesystem::read_symlink(std::filesystem::path(info.absoluteFilePath().toUtf8().constData()));
            require(link.is_relative(), QStringLiteral("An updater runtime link is absolute: %1").arg(info.filePath()));
            std::filesystem::create_symlink(link, std::filesystem::path(target.toUtf8().constData()));
        } else if (info.isDir()) copyTree(info.absoluteFilePath(), target);
        else copyFile(info.absoluteFilePath(), target);
    }
}
#endif
}

QString prepareHelper(const QString& root)
{
    const auto source = confinedPath(root, helperRelativePath());
    require(QFileInfo(source).isFile(), QStringLiteral("This Jam2 build has no updater helper. Install an updater-enabled build first."));
    clearDirectory(root, QStringLiteral("temp/updater"));
    const QString runner = QDir(root).filePath(QStringLiteral("temp/updater"));
#ifdef Q_OS_WIN
    const QString helper = QDir(runner).filePath(QStringLiteral("jam2-updater.exe"));
    copyFile(source, helper);
    for (const auto& name : {QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Network.dll")})
        copyFile(confinedPath(root, name), QDir(runner).filePath(name));
    copyFile(confinedPath(root, QStringLiteral("tls/qschannelbackend.dll")),
        QDir(runner).filePath(QStringLiteral("tls/qschannelbackend.dll")));
    for (const auto& file : QDir(root).entryInfoList(
            {QStringLiteral("msvcp*.dll"), QStringLiteral("vcruntime*.dll"), QStringLiteral("concrt*.dll")}, QDir::Files))
        copyFile(confinedPath(root, file.fileName()), QDir(runner).filePath(file.fileName()));
    QSaveFile config(QDir(runner).filePath(QStringLiteral("qt.conf")));
    const QByteArray configBytes("[Paths]\nPlugins=.\n");
#else
    const QString contents = QDir(runner).filePath(QStringLiteral("Contents"));
    const QString helper = QDir(contents).filePath(QStringLiteral("Helpers/jam2-updater"));
    copyFile(source, helper);
    const QString bundleContents = QDir(root).filePath(QStringLiteral("Jam2.app/Contents"));
    for (const auto& name : {QStringLiteral("QtCore.framework"), QStringLiteral("QtNetwork.framework")}) {
        QString from = QDir(bundleContents).filePath(QStringLiteral("Frameworks/") + name);
        // Native development builds can use the already-installed Qt SDK.
        if (!QFileInfo(from).isDir()) from = QDir(QLibraryInfo::path(QLibraryInfo::LibrariesPath)).filePath(name);
        copyTree(from, QDir(contents).filePath(QStringLiteral("Frameworks/") + name));
    }
    QString tls = QDir(bundleContents).filePath(QStringLiteral("PlugIns/tls"));
    if (!QFileInfo(tls).isDir()) tls = QDir(QLibraryInfo::path(QLibraryInfo::PluginsPath)).filePath(QStringLiteral("tls"));
    copyTree(tls, QDir(contents).filePath(QStringLiteral("PlugIns/tls")));
    QSaveFile config(QDir(contents).filePath(QStringLiteral("Helpers/qt.conf")));
    const QByteArray configBytes("[Paths]\nPrefix=..\nPlugins=PlugIns\n");
#endif
    require(config.open(QIODevice::WriteOnly) && config.write(configBytes) == configBytes.size() && config.commit(),
        QStringLiteral("Could not write the temporary updater's Qt configuration."));
    return helper;
}

QString temporaryHelperPath(const QString& root)
{
#ifdef Q_OS_WIN
    return confinedPath(root, QStringLiteral("temp/updater/jam2-updater.exe"));
#else
    return confinedPath(root, QStringLiteral("temp/updater/Contents/Helpers/jam2-updater"));
#endif
}

QStringList helperArguments(const LaunchOptions& options)
{
    QStringList arguments{QStringLiteral("--root"), QDir(options.root).absolutePath(),
        QStringLiteral("--version"), options.version, QStringLiteral("--wait-pid"), QString::number(options.waitPid)};
    if (options.restart) arguments << QStringLiteral("--restart");
    if (options.staged) arguments << QStringLiteral("--install-staged");
    if (options.prepareOnly) arguments << QStringLiteral("--prepare-only");
    return arguments;
}

namespace {
qint64 startHelper(const LaunchOptions& options, const QString& helper)
{
    const QString update = confinedPath(options.root, QStringLiteral("update"), true);
    require(QDir().mkpath(update), QStringLiteral("Could not create update/."));
    QProcess process;
    configureHelperProcess(process);
    process.setProgram(helper);
    process.setArguments(helperArguments(options));
    process.setWorkingDirectory(options.root);
    const auto log = QDir(options.root).filePath(QStringLiteral("temp/updater/updater.log"));
    process.setStandardOutputFile(log, QIODevice::Append);
    process.setStandardErrorFile(log, QIODevice::Append);
    qint64 pid = 0;
    // The child retries the same lock briefly while this launch completes.
    require(process.startDetached(&pid), QStringLiteral("Could not launch the temporary updater: %1").arg(process.errorString()));
    return pid;
}
}

qint64 launch(const LaunchOptions& options)
{
    require(validVersion(options.version) && options.waitPid >= 0, QStringLiteral("Invalid update version or process ID."));
    QLockFile lock(confinedPath(options.root, QStringLiteral("temp/updater.lock"), true));
    require(lock.tryLock(), QStringLiteral("An updater is already running for this Jam2 folder."));
    return startHelper(options, prepareHelper(options.root));
}

qint64 launchPrepared(const LaunchOptions& options)
{
    require(validVersion(options.version) && options.staged && !options.prepareOnly,
        QStringLiteral("Invalid prepared update launch."));
    QLockFile lock(confinedPath(options.root, QStringLiteral("temp/updater.lock"), true));
    require(lock.tryLock(), QStringLiteral("An updater is still preparing this update. Wait for it to finish."));
    const auto helper = temporaryHelperPath(options.root);
    require(QFileInfo(helper).isFile(), QStringLiteral("The temporary updater is missing. Download the update again."));
    return startHelper(options, helper);
}

void writeResult(const QString& root, const Result& result)
{
    const auto path = confinedPath(root, QStringLiteral("update/result.json"), true);
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1},
        {QStringLiteral("success"), result.success}, {QStringLiteral("reported"), result.reported},
        {QStringLiteral("version"), result.version}, {QStringLiteral("error"), result.error}}).toJson();
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
        QStringLiteral("Could not save the updater result."));
}

std::optional<Result> readResult(const QString& root)
{
    const auto path = confinedPath(root, QStringLiteral("update/result.json"));
    if (!QFileInfo(path).exists()) return std::nullopt;
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 65536, QStringLiteral("Could not read the updater result."));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto object = document.object();
    require(error.error == QJsonParseError::NoError && object.value(QStringLiteral("schema")).toInt() == 1 &&
        object.value(QStringLiteral("success")).isBool(), QStringLiteral("The updater result is invalid."));
    return Result{object.value(QStringLiteral("success")).toBool(), object.value(QStringLiteral("reported")).toBool(),
        object.value(QStringLiteral("version")).toString(), object.value(QStringLiteral("error")).toString()};
}

bool cleanupCompleted(const QString& root)
{
    auto result = readResult(root);
    if (!result) return true;
    QLockFile lock(confinedPath(root, QStringLiteral("temp/updater.lock"), true));
    if (!lock.tryLock()) return false;
    const QString runner = confinedPath(root, QStringLiteral("temp/updater"));
    if (QFileInfo(runner).exists() && !QDir(runner).removeRecursively()) return false;
    if (result->success) {
        const QString stage = confinedPath(root, QStringLiteral("update"));
        return !QFileInfo(stage).exists() || QDir(stage).removeRecursively();
    }
    result->reported = true;
    writeResult(root, *result);
    return true;
}

int runUpdateCommand(const QStringList& arguments)
{
    QTextStream out(stdout);
    QTextStream error(stderr);
    try {
        LaunchOptions options;
        options.root = appReleaseDir().absolutePath();
        options.waitPid = QCoreApplication::applicationPid();
        bool check = false;
        for (qsizetype i = 0; i < arguments.size(); ++i) {
            const auto& argument = arguments[i];
            if (argument == QStringLiteral("--version") || argument == QStringLiteral("--wait-pid")) {
                require(i + 1 < arguments.size(), QStringLiteral("Missing update option value."));
                const auto value = arguments[++i];
                if (argument == QStringLiteral("--version")) options.version = value;
                else { bool valid = false; options.waitPid = value.toLongLong(&valid); require(valid && options.waitPid >= 0, QStringLiteral("Invalid process ID.")); }
            } else if (argument == QStringLiteral("--restart")) options.restart = true;
            else if (argument == QStringLiteral("--install-staged")) options.staged = true;
            else if (argument == QStringLiteral("--check-staged")) check = true;
            else if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
                out << "jam2 debug update --version X.Y.Z [--wait-pid PID] [--restart] [--install-staged | --check-staged]\n";
                out << "Uses this installation's update/ and temp/updater/ folders. No network request is made for staged modes.\n";
                return 0;
            } else throw std::runtime_error("Unknown update option: " + argument.toStdString());
        }
        require(validVersion(options.version), QStringLiteral("Specify --version major.minor.patch."));
        if (check) {
            const std::atomic_bool cancelled{false};
            const auto manifest = verifyPackage(options.root, options.version, cancelled);
            out << "Staged update verified: " << manifest.version << " (" << manifest.files.size() << " files)\n";
            return 0;
        }
        const auto pid = launch(options);
        out << "Updater started (PID " << pid << "). Progress: update/status.json; details: temp/updater/updater.log.\n";
        out << "Installation waits for the specified Jam2 process to exit.\n";
        return 0;
    } catch (const std::exception& exception) {
        error << "Could not start update: " << exception.what() << '\n';
        return 1;
    }
}
} // namespace jam2::update
