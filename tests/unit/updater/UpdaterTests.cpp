#include "UpdateDownload.hpp"
#include "UpdateLauncher.hpp"
#include "UpdatePackage.hpp"
#include "UpdaterPlatform.hpp"
#include "UpdatePreparation.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace jam2::update;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
QByteArray sha(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read test fixture"); return file.readAll();
}
void write(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
void fails(const std::function<void()>& action, const QString& text)
{
    try { action(); } catch (const std::exception& error) {
        if (!QString::fromUtf8(error.what()).contains(text, Qt::CaseInsensitive)) {
            std::cerr << error.what() << '\n'; throw std::runtime_error("unexpected failure diagnostic");
        }
        return;
    }
    throw std::runtime_error("expected operation to fail");
}
void waitFor(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    require(done(), "asynchronous updater operation timed out");
}

// Small real ZIPs containing deflated payloads, created entirely in process.
QByteArray zip(const QMap<QString, QByteArray>& files)
{
    QByteArray body, central;
    QDataStream output(&body, QIODevice::WriteOnly), directory(&central, QIODevice::WriteOnly);
    output.setByteOrder(QDataStream::LittleEndian); directory.setByteOrder(QDataStream::LittleEndian);
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto name = it.key().toUtf8(); const auto data = it.value();
        const auto zlib = qCompress(data, 6); const auto compressed = zlib.mid(6, zlib.size() - 10);
        const auto crc = static_cast<quint32>(mz_crc32(MZ_CRC32_INIT,
            reinterpret_cast<const unsigned char*>(data.constData()), static_cast<size_t>(data.size())));
        const auto offset = static_cast<quint32>(body.size());
        output << quint32(0x04034b50) << quint16(20) << quint16(0x800) << quint16(8) << quint16(0) << quint16(0)
            << crc << quint32(compressed.size()) << quint32(data.size()) << quint16(name.size()) << quint16(0);
        output.writeRawData(name.constData(), static_cast<int>(name.size()));
        output.writeRawData(compressed.constData(), static_cast<int>(compressed.size()));
        directory << quint32(0x02014b50) << quint16(0x0314) << quint16(20) << quint16(0x800) << quint16(8)
            << quint16(0) << quint16(0) << crc << quint32(compressed.size()) << quint32(data.size())
            << quint16(name.size()) << quint16(0) << quint16(0) << quint16(0) << quint16(0)
            << quint32(0100755u << 16) << offset;
        directory.writeRawData(name.constData(), static_cast<int>(name.size()));
    }
    const auto centralOffset = static_cast<quint32>(body.size());
    output.writeRawData(central.constData(), static_cast<int>(central.size()));
    output << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(files.size()) << quint16(files.size())
        << quint32(central.size()) << centralOffset << quint16(0);
    return body;
}

struct Fixture {
    QString root;
    QString version = QStringLiteral("0.1.2");
    QMap<QString, QByteArray> payload;
    QByteArray manifest;
    QByteArray archive;
    Metadata metadata;
    explicit Fixture(QString path) : root(std::move(path))
    {
        payload = {{executableRelativePath(), "new application"}, {helperRelativePath(), "new helper"},
            {QStringLiteral("Qt6Core.dll"), "new runtime"}, {QStringLiteral("version.txt"), version.toUtf8() + '\n'},
            {QStringLiteral("models.json"), "new model pin"}, {QStringLiteral("licenses/new.txt"), "new notice"}};
#ifdef Q_OS_MACOS
        payload.remove(QStringLiteral("Qt6Core.dll"));
#endif
        write(QDir(root).filePath(executableRelativePath()), "old application");
        write(QDir(root).filePath(helperRelativePath()), "old helper");
        write(QDir(root).filePath(QStringLiteral("Qt6Core.dll")), "old runtime");
        write(QDir(root).filePath(QStringLiteral("version.txt")), "0.1.1\n");
        write(QDir(root).filePath(QStringLiteral("models.json")), "old model pin");
        write(QDir(root).filePath(QStringLiteral("licenses/obsolete.txt")), "obsolete notice");
        for (const auto& folder : {QStringLiteral("settings"), QStringLiteral("tracks"), QStringLiteral("songs"),
            QStringLiteral("components/jamtaster/models"), QStringLiteral("temp/other-task")})
            write(QDir(root).filePath(folder + QStringLiteral("/preserve.txt")), "user content");
        QMap<QString, QByteArray> old{{executableRelativePath(), "old application"}, {helperRelativePath(), "old helper"},
            {QStringLiteral("version.txt"), "0.1.1\n"}, {QStringLiteral("models.json"), "old model pin"},
            {QStringLiteral("licenses/obsolete.txt"), "obsolete notice"}};
        write(QDir(root).filePath(QStringLiteral("package-manifest.json")), makeManifest(old, QStringLiteral("0.1.1")));
        rebuild();
    }
    QByteArray makeManifest(const QMap<QString, QByteArray>& files, const QString& value) const
    {
        QJsonArray entries;
        for (auto it = files.begin(); it != files.end(); ++it)
            entries.append(QJsonObject{{"path", it.key()}, {"bytes", it.value().size()}, {"sha256", QString::fromLatin1(sha(it.value()))}});
        QJsonArray architectures{QStringLiteral("x86_64")};
#ifdef Q_OS_MACOS
        architectures.append(QStringLiteral("arm64"));
#endif
        return QJsonDocument(QJsonObject{{"schema", 1}, {"version", value}, {"platform", platform()},
            {"architectures", architectures}, {"models_manifest", "models.json"}, {"files", entries}}).toJson();
    }
    void rebuild()
    {
        manifest = makeManifest(payload, version);
        auto entries = payload; entries.insert(QStringLiteral("package-manifest.json"), manifest);
        archive = zip(entries);
        metadata = {version, QUrl(QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/v%1/%2").arg(version, assetName())),
            archive.size(), sha(archive), sha(payload.value(QStringLiteral("models.json")))};
        write(QDir(root).filePath(QStringLiteral("update/package.zip")), archive);
    }
    QByteArray metadataJson() const
    {
        return QJsonDocument(QJsonObject{{"schema", 1}, {"version", version}, {"tag", QStringLiteral("v") + version},
            {"channel", "stable"}, {"asset", assetName()}, {"url", metadata.url.toString()},
            {"bytes", metadata.bytes}, {"sha256", QString::fromLatin1(metadata.sha256)},
            {"models_manifest_sha256", QString::fromLatin1(metadata.modelsManifestSha256)}}).toJson();
    }
    void preserve() const
    {
        for (const auto& folder : {QStringLiteral("settings"), QStringLiteral("tracks"), QStringLiteral("songs"),
            QStringLiteral("components/jamtaster/models"), QStringLiteral("temp/other-task")})
            require(read(QDir(root).filePath(folder + QStringLiteral("/preserve.txt"))) == "user content", "user content is preserved");
    }
};

class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request, QByteArray bytes, int http, int delay, QObject* parent)
        : QNetworkReply(parent), bytes_(std::move(bytes))
    {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, http);
        if (http != 200) setError(ContentNotFoundError, QStringLiteral("Asset not found"));
        open(QIODevice::ReadOnly);
        QTimer::singleShot(delay, this, [this] {
            if (isFinished()) return;
            available_ = bytes_.size(); emit readyRead();
            if (isFinished()) return;
            setFinished(true); emit finished();
        });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError, "Cancelled"); setFinished(true); emit finished(); } }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return available_ - offset_ + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* target, qint64 maximum) override
    {
        const auto count = std::min(maximum, available_ - offset_);
        if (count <= 0) return -1;
        std::memcpy(target, bytes_.constData() + offset_, static_cast<size_t>(count)); offset_ += count; return count;
    }
private:
    QByteArray bytes_; qint64 available_ = 0; qint64 offset_ = 0;
};
class Network final : public QNetworkAccessManager {
public:
    QByteArray metadata, archive; int http = 200; int delay = 1; QStringList requests;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requests << request.url().toString();
        return new Reply(request, request.url().path().endsWith(QStringLiteral(".json")) ? metadata : archive, http, delay, this);
    }
};

void testValidation(Fixture& fixture)
{
    require(parseMetadata(fixture.metadataJson(), fixture.version).sha256 == fixture.metadata.sha256, "valid exact-version metadata");
    fails([&] { parseMetadata(fixture.metadataJson(), QStringLiteral("0.1.3")); }, "does not match");
    require(!validVersion(QStringLiteral("01.1.2")) && !validVersion(QStringLiteral("1.2.65536")), "version validation");
    for (const auto& path : {executableRelativePath(), helperRelativePath(), QStringLiteral("version.txt"), QStringLiteral("models.json")}) {
        auto files = fixture.payload; files.remove(path);
        fails([&] { parseManifest(fixture.makeManifest(files, fixture.version)); }, path);
    }
    auto legacyFiles = fixture.payload; legacyFiles.remove(helperRelativePath());
    require(parseManifest(fixture.makeManifest(legacyFiles, fixture.version), false).version == fixture.version,
        "previous installation ownership can still be read without a helper");
    for (const auto& path : {QStringLiteral("../escape"), QStringLiteral("settings/preferences.ini"),
        QStringLiteral("components/jamtaster/models/user.bin"), QStringLiteral("C:/escape"), QStringLiteral("licenses/NUL.txt")}) {
        auto files = fixture.payload; files.insert(path, "unsafe");
        fails([&] { parseManifest(fixture.makeManifest(files, fixture.version)); }, "unsafe");
    }
    const std::atomic_bool cancelled{false};
    auto bad = fixture.metadata; bad.sha256 = QByteArray(64, '0');
    fails([&] { extractPackage(fixture.root, bad, cancelled); }, "SHA-256");
    auto entries = fixture.payload; entries.insert(QStringLiteral("package-manifest.json"), fixture.manifest);
    entries.insert(QStringLiteral("../escape"), "outside");
    const auto unsafe = zip(entries);
    write(QDir(fixture.root).filePath(QStringLiteral("update/package.zip")), unsafe);
    bad = fixture.metadata; bad.bytes = unsafe.size(); bad.sha256 = sha(unsafe);
    fails([&] { extractPackage(fixture.root, bad, cancelled); }, "Unsafe ZIP");
    require(!QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("escape"))) &&
        read(QDir(fixture.root).filePath(executableRelativePath())) == "old application", "unsafe ZIP cannot write outside staging or touch app");
    entries = fixture.payload;
    entries[executableRelativePath()] = "bad application";
    entries.insert(QStringLiteral("package-manifest.json"), fixture.manifest);
    const auto damaged = zip(entries);
    write(QDir(fixture.root).filePath(QStringLiteral("update/package.zip")), damaged);
    bad = fixture.metadata; bad.bytes = damaged.size(); bad.sha256 = sha(damaged);
    fails([&] { extractPackage(fixture.root, bad, cancelled); }, "SHA-256");
    require(read(QDir(fixture.root).filePath(executableRelativePath())) == "old application", "verified ZIP must still contain correctly hashed files");
    fixture.preserve();
}

void testInstall(Fixture& fixture)
{
    std::atomic_bool cancelled{false};
    auto manifest = extractPackage(fixture.root, fixture.metadata, cancelled);
    require(read(QDir(fixture.root).filePath(executableRelativePath())) == "old application", "extraction does not change installed app");
    cancelled.store(true);
    fails([&] { installPackage(fixture.root, manifest, cancelled); }, "cancelled");
    require(read(QDir(fixture.root).filePath(QStringLiteral("version.txt"))) == "0.1.1\n", "cancelled install preserves installed version");
    cancelled.store(false);
    const auto oldManifest = read(QDir(fixture.root).filePath(QStringLiteral("package-manifest.json")));
    auto badOld = QJsonDocument::fromJson(oldManifest).object();
    auto oldEntries = badOld.value(QStringLiteral("files")).toArray();
    oldEntries.append(QJsonObject{{"path", "settings/preferences.ini"}, {"bytes", 1}, {"sha256", QString(64, QLatin1Char('0'))}});
    badOld.insert(QStringLiteral("files"), oldEntries);
    write(QDir(fixture.root).filePath(QStringLiteral("package-manifest.json")), QJsonDocument(badOld).toJson());
    fails([&] { installPackage(fixture.root, manifest, cancelled); }, "unsafe");
    write(QDir(fixture.root).filePath(QStringLiteral("package-manifest.json")), oldManifest);
#ifdef Q_OS_WIN
    const auto lockedPath = QDir(fixture.root).filePath(QStringLiteral("Qt6Core.dll"));
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "lock installed runtime fixture");
    fails([&] { installPackage(fixture.root, manifest, cancelled); }, "Cannot replace");
    CloseHandle(handle);
    require(read(QDir(fixture.root).filePath(executableRelativePath())) == "old application", "locked destination fails before any replacement");
#endif
    const auto source = QDir(fixture.root).filePath(QStringLiteral("update/package/") + executableRelativePath());
    write(source, "tampered application");
    fails([&] { installPackage(fixture.root, manifest, cancelled); }, "verification");
    require(read(QDir(fixture.root).filePath(executableRelativePath())) == "old application", "changed staged files cannot install");
    write(source, fixture.payload.value(executableRelativePath()));
    installPackage(fixture.root, manifest, cancelled);
    for (auto it = fixture.payload.begin(); it != fixture.payload.end(); ++it)
        require(read(QDir(fixture.root).filePath(it.key())) == it.value(), "new package files installed");
    require(!QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("licenses/obsolete.txt"))), "obsolete package-owned file removed");
    require(read(QDir(fixture.root).filePath(QStringLiteral("package-manifest.json"))) == fixture.manifest, "new ownership manifest committed");
    fixture.preserve();
}

void testDownload(Fixture& fixture)
{
    Network network; network.metadata = fixture.metadataJson(); network.archive = fixture.archive;
    Download downloader(fixture.root, nullptr, &network);
    bool done = false; QString error; qint64 progressBytes = 0;
    downloader.progress = [&](qint64 bytes, qint64) { progressBytes = bytes; };
    downloader.finished = [&](const Metadata&, const QString& message) { done = true; error = message; };
    downloader.start(fixture.version);
    require(!done, "download starts asynchronously"); waitFor([&] { return done; });
    require(error.isEmpty() && network.requests.size() == 2 && progressBytes == fixture.archive.size() &&
        read(QDir(fixture.root).filePath(QStringLiteral("update/package.zip"))) == fixture.archive, "exact-tag metadata and ZIP streamed to update folder");
    done = false;
    auto approved = fixture.metadata; approved.sha256 = QByteArray(64, '0');
    const auto beforeChangedApproval = network.requests.size();
    downloader.start(fixture.version, approved); waitFor([&] { return done; });
    require(error.contains(QStringLiteral("changed since")) && network.requests.size() == beforeChangedApproval + 1,
        "release asset changed since user approval cannot start ZIP download");
    done = false; network.http = 404; downloader.start(fixture.version); waitFor([&] { return done; });
    require(error.contains(QStringLiteral("HTTP 404")), "HTTP failure is explicit");
    done = false; network.http = 200;
    auto invalid = QJsonDocument::fromJson(network.metadata).object(); invalid.insert(QStringLiteral("version"), "9.9.9");
    network.metadata = QJsonDocument(invalid).toJson();
    const auto requestsBefore = network.requests.size();
    downloader.start(fixture.version); waitFor([&] { return done; });
    require(error.contains(QStringLiteral("does not match")) && network.requests.size() == requestsBefore + 1, "mismatched release metadata never starts ZIP transfer");
    network.metadata = fixture.metadataJson();
    done = false; network.http = 200; network.archive.chop(5);
    downloader.start(fixture.version); waitFor([&] { return done; });
    require(error.contains(QStringLiteral("did not complete")) && !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("update/package.zip"))), "truncated ZIP is not committed");
    done = false; network.delay = 100; downloader.start(fixture.version); downloader.cancel(); waitFor([&] { return done; });
    require(error.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive), "download cancellation reported");
    fixture.preserve();
}

void stageRuntime(const QString& root, const QString& release)
{
    write(QDir(root).filePath(helperRelativePath()), read(QDir(release).filePath(helperRelativePath())));
#ifdef Q_OS_WIN
    for (const auto& info : QDir(release).entryInfoList({QStringLiteral("*.dll")}, QDir::Files))
        write(QDir(root).filePath(info.fileName()), read(info.absoluteFilePath()));
    write(QDir(root).filePath(QStringLiteral("tls/qschannelbackend.dll")), read(QDir(release).filePath(QStringLiteral("tls/qschannelbackend.dll"))));
#endif
}

void testHelper(Fixture& fixture, const QString& release)
{
    fixture.payload[helperRelativePath()] = read(QDir(release).filePath(helperRelativePath())); fixture.rebuild();
    const std::atomic_bool cancelled{false}; extractPackage(fixture.root, fixture.metadata, cancelled);
    stageRuntime(fixture.root, release);
    const QString helper = prepareHelper(fixture.root);
    require(QFileInfo(helper).isFile() && helper.startsWith(QDir(fixture.root).filePath(QStringLiteral("temp/updater/"))), "helper and runtime staged in local temp folder");
    QProcess process;
    process.start(helper, {QStringLiteral("--root"), fixture.root, QStringLiteral("--version"), fixture.version,
        QStringLiteral("--wait-pid"), QString::number(QCoreApplication::applicationPid()), QStringLiteral("--install-staged")});
    require(process.waitForFinished(15000) && process.exitCode() == 1, "helper rejects a PID belonging to a different executable");
    require(readResult(fixture.root)->error.contains(QStringLiteral("not Jam2")), "PID mismatch is saved clearly");
    process.start(helper, {QStringLiteral("--root"), fixture.root, QStringLiteral("--version"), fixture.version,
        QStringLiteral("--prepare-only"), QStringLiteral("--install-staged")});
    require(process.waitForFinished(15000) && process.exitCode() == 0, "real helper can verify a preparation pass without installation");
    require(read(QDir(fixture.root).filePath(executableRelativePath())) == "old application" &&
        QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("update/prepared.json"))) && !readResult(fixture.root),
        "prepared update leaves application unchanged and is not recorded as an installed update");
    writeResult(fixture.root, {false, false, fixture.version, QStringLiteral("Cleanup lock fixture")});
    QLockFile lock(QDir(fixture.root).filePath(QStringLiteral("temp/updater.lock")));
    require(lock.tryLock(), "hold active updater lock");
    require(!cleanupCompleted(fixture.root), "startup cleanup does not touch a running updater"); lock.unlock();
    process.start(helper, {QStringLiteral("--root"), fixture.root, QStringLiteral("--version"), fixture.version,
        QStringLiteral("--install-staged")});
    require(process.waitForFinished(15000), "staged helper completes");
    if (process.exitCode() != 0) std::cerr << process.readAllStandardError().constData() << '\n';
    require(process.exitCode() == 0 && readResult(fixture.root)->success, "actual helper executable installs successfully from its temporary Qt runtime");
    require(process.readAllStandardOutput().contains("\"tls_available\":true"), "temporary helper loads a working TLS backend without the installed Qt files");
    require(read(QDir(fixture.root).filePath(helperRelativePath())) == fixture.payload.value(helperRelativePath()), "installed updater replaced while temporary copy runs");
    require(cleanupCompleted(fixture.root) && !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("update"))) &&
        !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("temp/updater"))), "next-launch cleanup removes staging and helper copies");
    fixture.preserve();
}

void testDetachedLauncher(Fixture& fixture, const QString& release)
{
    fixture.payload[helperRelativePath()] = read(QDir(release).filePath(helperRelativePath()));
    fixture.rebuild();
    const std::atomic_bool cancelled{false};
    extractPackage(fixture.root, fixture.metadata, cancelled);
    stageRuntime(fixture.root, release);
    const auto pid = launch({fixture.root, fixture.version, 0, false, true});
    require(pid > 0, "real detached launcher starts helper");
    waitFor([&] { return readResult(fixture.root).has_value(); });
    require(readResult(fixture.root)->success, "detached launcher installs local package");
    ParentProcess helper(pid, QDir(fixture.root).filePath(QStringLiteral("temp/updater/jam2-updater.exe")));
    waitFor([&] { return helper.exited(); });
    require(read(QDir(fixture.root).filePath(QStringLiteral("temp/updater/updater.log"))).contains("\"tls_available\":true"), "detached updater log contains real TLS backend status");
    require(cleanupCompleted(fixture.root), "detached helper staging can be cleaned");
    fixture.preserve();
}

void testFailureCleanup(Fixture& fixture)
{
    write(QDir(fixture.root).filePath(QStringLiteral("temp/updater/partial-runtime")), "temporary runtime");
    writeResult(fixture.root, {false, false, fixture.version, QStringLiteral("Download failed")});
    require(!readResult(fixture.root)->reported, "new failure awaits user notification");
    require(cleanupCompleted(fixture.root) && readResult(fixture.root)->reported &&
        readResult(fixture.root)->error == QStringLiteral("Download failed") &&
        !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("temp/updater"))),
        "failure cleanup removes temporary runtime and retains diagnostic without repeating notification");
    fixture.preserve();
}

void testAsynchronousPreparation(Fixture& fixture, const QString& release)
{
    fixture.payload[helperRelativePath()] = read(QDir(release).filePath(helperRelativePath()));
    fixture.rebuild();
    const std::atomic_bool stopped{false};
    extractPackage(fixture.root, fixture.metadata, stopped);
    stageRuntime(fixture.root, release);
    Preparation preparation(fixture.root);
    bool done = false; bool cancelled = false; QString error;
    int heartbeat = 0; QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; }); timer.start(1);
    preparation.finished = [&](const QString& message, bool wasCancelled) { error = message; cancelled = wasCancelled; done = true; };
    preparation.start(fixture.metadata, fixture.metadataJson(), true);
    require(!done && preparation.active(), "preparation stages runtime asynchronously");
    waitFor([&] { return done; });
    if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
    require(error.isEmpty() && !cancelled && heartbeat > 0 && !preparation.active() &&
        read(QDir(fixture.root).filePath(executableRelativePath())) == "old application",
        "real preparation service returns ready while keeping installed application unchanged");
    done = false; preparation.cancel(); waitFor([&] { return done; });
    require(cancelled && !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("update"))) &&
        !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("temp/updater"))), "discarding real prepared update asynchronously removes staging and helper copies");
    fixture.preserve();
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 3, "expected build-local fixture workspace and public release root");
        const QString workspace = QString::fromLocal8Bit(argv[1]);
        require(QDir().mkpath(workspace), "create fixture workspace");
        QTemporaryDir temporary(QDir(workspace).filePath(QStringLiteral("updater-XXXXXX")));
        require(temporary.isValid(), "create isolated updater installation");
        Fixture validation(temporary.filePath(QStringLiteral("validation"))); testValidation(validation);
        Fixture download(temporary.filePath(QStringLiteral("download"))); testDownload(download);
        Fixture failed(temporary.filePath(QStringLiteral("failed"))); testFailureCleanup(failed);
#ifdef Q_OS_WIN
        Fixture installation(temporary.filePath(QStringLiteral("installation"))); testInstall(installation);
        Fixture helper(temporary.filePath(QStringLiteral("helper"))); testHelper(helper, QString::fromLocal8Bit(argv[2]));
        Fixture detached(temporary.filePath(QStringLiteral("detached"))); testDetachedLauncher(detached, QString::fromLocal8Bit(argv[2]));
        Fixture preparation(temporary.filePath(QStringLiteral("preparation"))); testAsynchronousPreparation(preparation, QString::fromLocal8Bit(argv[2]));
#else
        // The native signature check must reject an unsigned synthetic bundle.
        Fixture unsignedBundle(temporary.filePath(QStringLiteral("signature")));
        const std::atomic_bool cancelled{false};
        fails([&] { extractPackage(unsignedBundle.root, unsignedBundle.metadata, cancelled); }, "signature");
#endif
        ParentProcess current(QCoreApplication::applicationPid(), QCoreApplication::applicationFilePath());
        require(!current.exited(), "native waiter recognizes the original live process");
        std::cout << "Updater tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
