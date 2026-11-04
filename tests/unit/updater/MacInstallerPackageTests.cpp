#include "MacInstallerPackage.hpp"
#include "UpdateLauncher.hpp"
#include "UpdatePreparation.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

using namespace jam2::update;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create test parent");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write test file");
}
template<class F> void fails(F&& action, const char* diagnostic)
{
    try { action(); } catch (const std::exception& error) {
        require(QString::fromUtf8(error.what()).contains(QLatin1String(diagnostic), Qt::CaseInsensitive), "failure reports the relevant cause");
        return;
    }
    throw std::runtime_error("expected installer operation to fail");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 3, "expected artifact root and public release root");
        QTemporaryDir temp(QDir(QString::fromLocal8Bit(argv[1])).filePath(QStringLiteral("mac installer-XXXXXX")));
        require(temp.isValid(), "create private installer fixture with spaces");
        const QString root = temp.path();
        const QByteArray payload("signed installer fixture");
        Metadata metadata{QStringLiteral("0.2.0"), QUrl(QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/v0.2.0/Jam2-macos.pkg")),
            payload.size(), QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex(), QByteArray(64, 'b')};
        const auto encoded = QJsonDocument(QJsonObject{{"schema", 1}, {"channel", "stable"}, {"tag", "v0.2.0"},
            {"asset", assetName()}, {"version", metadata.version}, {"url", metadata.url.toString()},
            {"bytes", metadata.bytes}, {"sha256", QString::fromLatin1(metadata.sha256)},
            {"models_manifest_sha256", QString::fromLatin1(metadata.modelsManifestSha256)}}).toJson();
        const QString package = QDir(root).filePath(downloadRelativePath());
        write(package, payload);
        write(package + QStringLiteral(".json"), encoded);
        write(QDir(root).filePath(QStringLiteral("settings/preferences.ini")), "user settings");
        QStringList calls;
        int signatureCode = 0, trustCode = 0, openCode = 0;
        MacToolRunner runner = [&](const QString& tool, const QStringList& arguments) -> MacToolResult {
            calls << tool;
            require(arguments.last() == package, "all system operations use the confined verified package");
            if (tool.endsWith(QStringLiteral("pkgutil"))) return {signatureCode, QStringLiteral("1. Developer ID Installer: Fixture (TEAM)")};
            if (tool.endsWith(QStringLiteral("spctl"))) {
                require(arguments.contains(QStringLiteral("install")), "Gatekeeper assesses an installer package");
                return {trustCode, QStringLiteral("assessment")};
            }
            require(tool == QStringLiteral("/usr/bin/open") && arguments.value(1) == QStringLiteral("/System/Library/CoreServices/Installer.app"),
                "handoff explicitly targets Apple's Installer");
            return {openCode, QStringLiteral("handoff")};
        };
        std::atomic_bool cancelled{false};
        prepareMacInstaller(root, metadata, cancelled, runner);
        require(calls.size() == 2 && !readResult(root), "verification prepares an installer without claiming it was installed");
        const auto approved = verifyPreparedMacInstaller(root, metadata.version, cancelled, runner);
        require(approved.sha256 == metadata.sha256, "prepared state remains bound to the approved package");
        openMacInstaller(root, metadata, cancelled, runner);
        require(calls.last() == QStringLiteral("/usr/bin/open"), "only a trusted package reaches the visible Installer");
        writeResult(root, {false, false, metadata.version, {}, true});
        require(readResult(root)->installerOpened && !confirmInstalledVersion(root, QStringLiteral("0.1.0"))->success &&
                cleanupCompleted(root, [] { return false; }) && QFileInfo::exists(package),
            "opening Installer is not installation success and does not delete its payload");
        signatureCode = 1;
        fails([&] { openMacInstaller(root, metadata, cancelled, runner); }, "signature");
        signatureCode = 0; trustCode = 1;
        fails([&] { openMacInstaller(root, metadata, cancelled, runner); }, "rejected");
        trustCode = 0; openCode = 1;
        fails([&] { openMacInstaller(root, metadata, cancelled, runner); }, "Could not open");
        openCode = 0;
        const auto count = calls.size();
        write(package, "tampered");
        fails([&] { openMacInstaller(root, metadata, cancelled, runner); }, "SHA-256");
        require(calls.size() == count, "tampering fails before invoking system tools");
        write(package, payload);
        cancelled.store(true);
        fails([&] { openMacInstaller(root, metadata, cancelled, runner); }, "cancelled");
        cancelled.store(false);
        fails([&] { verifyPreparedMacInstaller(root, QStringLiteral("0.3.0"), cancelled, runner); }, "does not match");
        // Exercise the real platform tool against an unsigned native package;
        // tests never invoke Installer or install into /Applications.
        const QString native = temp.filePath(QStringLiteral("unsigned.pkg"));
        QProcess process;
        process.start(QStringLiteral("/usr/bin/pkgbuild"), {QStringLiteral("--nopayload"),
            QStringLiteral("--identifier"), QStringLiteral("com.jam2.test.unsigned"), native});
        require(process.waitForFinished(30000) && process.exitCode() == 0, "build a real unsigned PKG fixture");
        require(QFile::remove(package) && QFile::copy(native, package), "stage unsigned native package");
        metadata.bytes = QFileInfo(package).size(); metadata.sha256 = sha256File(package);
        fails([&] { verifyMacInstaller(root, metadata, cancelled); }, "signature");
        auto nativeMetadata = QJsonDocument::fromJson(encoded).object();
        nativeMetadata.insert(QStringLiteral("bytes"), metadata.bytes);
        nativeMetadata.insert(QStringLiteral("sha256"), QString::fromLatin1(metadata.sha256));
        write(package + QStringLiteral(".json"), QJsonDocument(nativeMetadata).toJson());
        prepareMacInstaller(root, metadata, cancelled, runner);
        const QString installed = temp.filePath(QStringLiteral("read-only installation"));
        const QString sourceHelper = QDir(installed).filePath(helperRelativePath());
        require(QDir().mkpath(QFileInfo(sourceHelper).absolutePath()) &&
                QFile::copy(QDir(QString::fromLocal8Bit(argv[2])).filePath(helperRelativePath()), sourceHelper),
            "create read-only installed helper fixture from the actual public build");
        require(QFile::setPermissions(installed, QFile::ReadOwner | QFile::ExeOwner), "make installation root read-only");
        const QString stagedHelper = prepareHelper(root, installed);
        require(QFile::setPermissions(installed, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "restore fixture ownership for cleanup");
        process.start(stagedHelper, {QStringLiteral("--root"), root, QStringLiteral("--version"), metadata.version,
            QStringLiteral("--install-staged"), QStringLiteral("--prepare-only")});
        require(process.waitForFinished(30000) && process.exitCode() != 0 && readResult(root) &&
                !readResult(root)->success && readResult(root)->error.contains(QStringLiteral("signature")),
            "real temporary helper rejects an unsigned PKG without modifying the installed app");
        require(process.readAllStandardOutput().contains("\"tls_available\":true") && QFileInfo(sourceHelper).isFile(),
            "helper stages Qt/TLS in writable data while installed resources remain readable");
        require(!confirmInstalledVersion(root, metadata.version)->success,
            "a verification failure cannot become success just because version strings match");
        writeResult(root, {false, false, metadata.version, {}, true});
        require(confirmInstalledVersion(root, metadata.version)->success &&
                !cleanupCompleted(root, [] { return true; }) && QFileInfo::exists(package),
            "even a confirmed app launch retains the package while Installer remains open");
        require(cleanupCompleted(root, [] { return false; }) &&
                !QFileInfo::exists(package) && QFileInfo::exists(temp.filePath(QStringLiteral("settings/preferences.ini"))),
            "confirmed installed version cleans staging and preserves user data");
        const QString cancellationRoot = temp.filePath(QStringLiteral("cancellation"));
        const QString cancellationPackage = QDir(cancellationRoot).filePath(downloadRelativePath());
        write(cancellationPackage, payload);
        write(QDir(cancellationRoot).filePath(QStringLiteral("temp/updater/runtime")), "temporary runtime");
        for (const bool installerActive : {true, false}) {
            Preparation preparation(cancellationRoot, nullptr, {}, [installerActive] { return installerActive; });
            bool done = false; QString diagnostic;
            preparation.finished = [&](const QString& message, bool wasCancelled) {
                require(wasCancelled, "discard completes through the typed cancellation boundary"); diagnostic = message; done = true;
            };
            preparation.cancel();
            QElapsedTimer timer; timer.start();
            while (!done && timer.elapsed() < 10000) { QCoreApplication::processEvents(); QThread::msleep(1); }
            require(done && QFileInfo::exists(cancellationPackage) == installerActive &&
                    !QFileInfo::exists(QDir(cancellationRoot).filePath(QStringLiteral("temp/updater"))),
                "asynchronous discard keeps an active Installer's PKG while cleaning private helper files");
            require(diagnostic.contains(QStringLiteral("preserved")) == installerActive,
                "cancellation reports when Installer still owns its retained package");
        }
        std::cout << "Mac PKG verification and Installer handoff tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
