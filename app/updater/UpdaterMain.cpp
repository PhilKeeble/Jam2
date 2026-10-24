#include "UpdateDownload.hpp"
#include "UpdateLauncher.hpp"
#include "UpdatePackage.hpp"
#include "UpdaterPlatform.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSslSocket>
#include <QTextStream>
#include <QTimer>

#include <atomic>
#include <stdexcept>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Jam2 updater"));
#ifdef Q_OS_MACOS
    QCoreApplication::setLibraryPaths({QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../PlugIns"))});
#else
    QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath()});
#endif
    using namespace jam2::update;
    QString root;
    QString version;
    std::atomic_bool cancelled{false};
    try {
        bool staged = false;
        bool restart = false;
        bool prepareOnly = false;
        qint64 waitPid = 0;
        const auto arguments = app.arguments();
        for (qsizetype i = 1; i < arguments.size(); ++i) {
            const auto& argument = arguments[i];
            if (argument == QStringLiteral("--root") || argument == QStringLiteral("--version") || argument == QStringLiteral("--wait-pid")) {
                if (i + 1 >= arguments.size()) throw std::runtime_error("Missing updater option value.");
                const auto value = arguments[++i];
                if (argument == QStringLiteral("--root")) root = QDir::cleanPath(value);
                else if (argument == QStringLiteral("--version")) version = value;
                else { bool valid = false; waitPid = value.toLongLong(&valid); if (!valid || waitPid < 0) throw std::runtime_error("Invalid process ID."); }
            } else if (argument == QStringLiteral("--restart")) restart = true;
            else if (argument == QStringLiteral("--install-staged")) staged = true;
            else if (argument == QStringLiteral("--prepare-only")) prepareOnly = true;
            else throw std::runtime_error("Unknown updater option.");
        }
        if (!validVersion(version)) throw std::runtime_error("Specify a valid updater version.");
        const auto runner = confinedPath(root, QStringLiteral("temp/updater"));
        if (!QCoreApplication::applicationFilePath().startsWith(runner + QLatin1Char('/'), Qt::CaseInsensitive))
            throw std::runtime_error("Launch the updater through Jam2 so it runs from temp/updater/.");
        QLockFile lock(confinedPath(root, QStringLiteral("temp/updater.lock"), true));
        if (!lock.tryLock(5000)) throw std::runtime_error("Another updater is already running for this Jam2 folder.");
        const bool tlsAvailable = QSslSocket::supportsSsl();
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"state", "starting"},
            {"tls_available", tlsAvailable}, {"tls_backend", QSslSocket::activeBackend()}})
            .toJson(QJsonDocument::Compact) << Qt::endl;
        if (!staged && !tlsAvailable)
            throw std::runtime_error("HTTPS is unavailable in the temporary updater. Its Qt TLS backend is missing or could not load; reinstall this Jam2 build.");
        ParentProcess parent(waitPid, QDir(root).filePath(executableRelativePath()));
        QElapsedTimer statusTimer;
        statusTimer.start();
        const auto status = [&](const QString& state, const QString& message, int percent, bool force = false) {
            if (!force && statusTimer.elapsed() < 200) return;
            statusTimer.restart();
            const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"state", state},
                {"message", message}, {"percent", percent}, {"version", version}}).toJson(QJsonDocument::Compact);
            QSaveFile file(confinedPath(root, QStringLiteral("update/status.json"), true));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                throw std::runtime_error("Could not write update/status.json.");
            QTextStream(stdout) << bytes << Qt::endl;
        };
        const auto prepared = [&](const Manifest& manifest) {
            const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"version", version},
                {"manifest_sha256", QString::fromLatin1(QCryptographicHash::hash(manifest.encoded, QCryptographicHash::Sha256).toHex())}}).toJson();
            QSaveFile file(confinedPath(root, QStringLiteral("update/prepared.json"), true));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                throw std::runtime_error("Could not save the prepared update marker.");
            QFile::remove(confinedPath(root, QStringLiteral("update/result.json")));
            status(QStringLiteral("ready"), QStringLiteral("Download verified. The update is ready to install."), 100, true);
        };
        auto install = [&](const Manifest& manifest) {
            status(QStringLiteral("waiting"), QStringLiteral("Update verified. Waiting for Jam2 to close."), 100, true);
            QElapsedTimer timeout;
            timeout.start();
            QTimer timer;
            timer.setInterval(100);
            QString waitError;
            QObject::connect(&timer, &QTimer::timeout, &app, [&] {
                try {
                    if (parent.exited()) { timer.stop(); app.quit(); }
                    else if (timeout.elapsed() > 120000) { waitError = QStringLiteral("Jam2 did not close within two minutes. Close it and retry the update."); timer.stop(); app.quit(); }
                } catch (const std::exception& exception) { waitError = QString::fromUtf8(exception.what()); timer.stop(); app.quit(); }
            });
            if (!parent.exited()) { timer.start(); app.exec(); }
            if (!waitError.isEmpty()) throw std::runtime_error(waitError.toStdString());
            status(QStringLiteral("installing"), QStringLiteral("Installing Jam2..."), 0, true);
            installPackage(root, manifest, cancelled, [&](const QString& message, int percent) {
                status(QStringLiteral("installing"), message, percent);
            });
            status(QStringLiteral("complete"), QStringLiteral("Jam2 update installed."), 100, true);
            writeResult(root, {true, false, version, {}});
            if (!QDir(QDir(root).filePath(QStringLiteral("update/package"))).removeRecursively())
                QTextStream(stderr) << "Staged package cleanup will be retried on the next launch.\n";
            QFile::remove(QDir(root).filePath(QStringLiteral("update/package.zip")));
            if (restart) {
                restartApplication(root);
            }
        };
        if (staged) {
            const auto manifest = verifyPackage(root, version, cancelled);
            if (prepareOnly) prepared(manifest); else install(manifest);
        } else {
            Download download(root);
            QString downloadError;
            Metadata metadata;
            download.progress = [&](qint64 current, qint64 total) {
                try {
                    status(QStringLiteral("downloading"), QStringLiteral("Downloading Jam2: %1 / %2 MB")
                        .arg(static_cast<double>(current) / 1'000'000.0, 0, 'f', 1)
                        .arg(static_cast<double>(total) / 1'000'000.0, 0, 'f', 1), static_cast<int>(current * 100 / total));
                } catch (const std::exception& exception) { downloadError = QString::fromUtf8(exception.what()); download.cancel(); }
            };
            bool completed = false;
            download.finished = [&](const Metadata& result, const QString& error) {
                metadata = result;
                if (downloadError.isEmpty()) downloadError = error;
                completed = true;
                app.quit();
            };
            std::optional<Metadata> approved;
            const auto approvedPath = confinedPath(root, QStringLiteral("temp/updater/approved.json"));
            if (prepareOnly && QFileInfo::exists(approvedPath)) {
                QFile file(approvedPath);
                if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
                    throw std::runtime_error("Could not read the approved release metadata.");
                approved = parseMetadata(file.readAll(), version);
            }
            download.start(version, approved);
            if (!completed) app.exec();
            if (!downloadError.isEmpty()) throw std::runtime_error(downloadError.toStdString());
            status(QStringLiteral("unpacking"), QStringLiteral("Verifying the downloaded update..."), -1, true);
            const auto manifest = extractPackage(root, metadata, cancelled, [&](const QString& message, int percent) {
                status(QStringLiteral("unpacking"), message, percent);
            });
            if (prepareOnly) prepared(manifest); else install(manifest);
        }
        return 0;
    } catch (const std::exception& exception) {
        const QString message = QString::fromUtf8(exception.what());
        QTextStream(stderr) << message << Qt::endl;
        try { if (!root.isEmpty()) writeResult(root, {false, false, version, message}); }
        catch (...) { QTextStream(stderr) << "Could not save the updater error.\n"; }
        return 1;
    } catch (...) {
        QTextStream(stderr) << "Unexpected updater failure.\n";
        return 1;
    }
}
