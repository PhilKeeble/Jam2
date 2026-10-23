#include "ModelDownloadService.hpp"
#include "JamTasterDialog.hpp"
#include "JamTasterService.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLockFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}
QByteArray sha(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
void waitFor(const std::function<bool()>& done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 15000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(done(), "asynchronous model operation timed out");
}

class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request, QByteArray body, int http, int delay, QObject* parent)
        : QNetworkReply(parent), body_(std::move(body))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, http);
        if (http != 200) setError(ContentNotFoundError, QStringLiteral("Release asset not found"));
        open(QIODevice::ReadOnly);
        QTimer::singleShot(delay, this, [this] {
            if (isFinished()) return;
            available_ = body_.size();
            emit readyRead();
            if (isFinished()) return;
            setFinished(true);
            emit finished();
        });
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return available_ - position_ + QNetworkReply::bytesAvailable(); }
    void abort() override
    {
        if (isFinished()) return;
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
        setFinished(true);
        emit finished();
    }
protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        const auto size = std::min(maximum, available_ - position_);
        if (size <= 0) return -1;
        std::memcpy(data, body_.constData() + position_, static_cast<size_t>(size));
        position_ += size;
        return size;
    }
private:
    QByteArray body_;
    qint64 available_ = 0;
    qint64 position_ = 0;
};

class Network final : public QNetworkAccessManager {
public:
    QByteArray metadata;
    QByteArray archive;
    int metadataHttp = 200;
    int archiveHttp = 200;
    int delay = 1;
    QStringList requests;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requests.push_back(request.url().toString());
        const bool json = request.url().path().endsWith(QStringLiteral(".json"));
        return new Reply(request, json ? metadata : archive,
            json ? metadataHttp : archiveHttp, delay, this);
    }
};

struct Fixture {
    QString root;
    QString models;
    QString payload;
    QStringList names;
    QByteArray manifest;
    QByteArray archive;
    QString tag = QStringLiteral("models-0123456789abcdef");
    QString url = QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/models-0123456789abcdef/Jam2-models.zip");
    Fixture(const QString& path, const QString& cmake) : root(path),
        models(QDir(path).filePath(QStringLiteral("components/jamtaster/models"))),
        payload(QDir(path).filePath(QStringLiteral("payload")))
    {
        names = {QStringLiteral("adtof.onnx"), QStringLiteral("basic_pitch.onnx"),
            QStringLiteral("beat_this.onnx"), QStringLiteral("chordmini_btc.onnx"),
            QStringLiteral("htdemucs_ft_0.onnx"), QStringLiteral("htdemucs_ft_1.onnx"),
            QStringLiteral("htdemucs_ft_2.onnx"), QStringLiteral("htdemucs_ft_3.onnx")};
        QJsonArray files;
        qint64 total = 0;
        for (const auto& name : names) {
            const auto data = QByteArray("native model fixture ") + name.toUtf8();
            const auto hash = sha(data);
            const QJsonObject sidecar{{"model", name}, {"bytes", data.size()}, {"sha256", QString::fromLatin1(hash)}};
            write(QDir(payload).filePath(QStringLiteral("models/") + name), data);
            write(QDir(payload).filePath(QStringLiteral("models/") + name + QStringLiteral(".json")),
                QJsonDocument(sidecar).toJson());
            files.append(QJsonObject{{"path", name}, {"bytes", data.size()}, {"sha256", QString::fromLatin1(hash)}});
            total += data.size();
        }
        write(QDir(payload).filePath(QStringLiteral("models/MODEL_NOTICES.md")), "Model fixture notices");
        manifest = QJsonDocument(QJsonObject{{"schema", 1}, {"tag", tag}, {"asset", "Jam2-models.zip"},
            {"archive_root", "models/"}, {"url", url}, {"unpacked_bytes", total}, {"files", files}}).toJson();
        write(QDir(root).filePath(QStringLiteral("models.json")), manifest);
        write(QDir(root).filePath(QStringLiteral("settings/preferences.ini")), "preserve user settings");
        QProcess process;
        process.setWorkingDirectory(payload);
        process.start(cmake, {QStringLiteral("-E"), QStringLiteral("tar"), QStringLiteral("cf"),
            QDir(root).filePath(QStringLiteral("fixture.zip")), QStringLiteral("--format=zip"), QStringLiteral("models")});
        require(process.waitForFinished(10000) && process.exitCode() == 0, "create real model fixture ZIP");
        archive = read(QDir(root).filePath(QStringLiteral("fixture.zip")));
    }
    QByteArray metadata(const QByteArray& bytes) const
    {
        return QJsonDocument(QJsonObject{{"schema", 1}, {"channel", "models"}, {"tag", tag},
            {"models_tag", tag}, {"asset", "Jam2-models.zip"}, {"url", url},
            {"bytes", bytes.size()}, {"sha256", QString::fromLatin1(sha(bytes))},
            {"models_manifest_sha256", QString::fromLatin1(sha(manifest))}}).toJson();
    }
    void configure(Network& network) const { network.archive = archive; network.metadata = metadata(archive); }
    void installFiles() const
    {
        for (const auto& name : names)
            write(QDir(models).filePath(name), read(QDir(payload).filePath(QStringLiteral("models/") + name)));
    }
    void missing(ModelDownloadService& service) const
    {
        service.check();
        waitFor([&] { return !service.busy(); });
        if (service.status().state != ModelDownloadService::State::AwaitingApproval)
            std::cerr << service.status().message.toStdString() << '\n';
        require(service.status().state == ModelDownloadService::State::AwaitingApproval, "missing models await explicit approval");
    }
    void checkCleanup() const
    {
        require(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("temp/jamtaster-models"))), "staging is removed");
        require(read(QDir(root).filePath(QStringLiteral("settings/preferences.ini"))) == "preserve user settings", "user settings survive");
    }
};

void testInstalled(Fixture& fixture)
{
    fixture.installFiles();
    Network network;
    ModelDownloadService service(fixture.root, fixture.models, nullptr, &network);
    bool mainThread = true;
    service.statusChanged = [&] { mainThread &= QThread::currentThread() == QApplication::instance()->thread(); };
    service.check();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Ready && network.requests.isEmpty() && mainThread,
        "verified installed models are ready without network and callbacks stay on GUI thread");
    write(QDir(fixture.models).filePath(fixture.names.front()), "damaged");
    fixture.configure(network);
    fixture.missing(service);
    require(network.requests.size() == 1 && network.requests.front().endsWith(QStringLiteral("Jam2-models.zip.json")),
        "damaged model checks only pinned metadata before approval");
}

void testDownload(Fixture& fixture)
{
    Network network;
    fixture.configure(network);
    ModelDownloadService service(fixture.root, fixture.models, nullptr, &network);
    fixture.missing(service);
    require(network.requests.size() == 1 && service.downloadBytes() == fixture.archive.size(), "only metadata before approval with exact compressed size");
    bool installing = false;
    int heartbeat = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; });
    timer.start(1);
    service.statusChanged = [&] { installing |= service.status().state == ModelDownloadService::State::Installing; };
    service.download();
    waitFor([&] { return !service.busy(); });
    if (service.status().state != ModelDownloadService::State::Ready) std::cerr << service.status().message.toStdString() << '\n';
    require(service.status().state == ModelDownloadService::State::Ready && installing && heartbeat > 0,
        "approved ZIP installs asynchronously while event loop remains responsive");
    for (const auto& name : fixture.names) {
        require(read(QDir(fixture.models).filePath(name)) == read(QDir(fixture.payload).filePath(QStringLiteral("models/") + name)), "installed model content");
        require(QFileInfo::exists(QDir(fixture.models).filePath(name + QStringLiteral(".json"))), "sidecar installed");
    }
    require(QFileInfo::exists(QDir(fixture.models).filePath(QStringLiteral("MODEL_NOTICES.md"))), "notice stays beside models");
    fixture.checkCleanup();
    service.check();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Ready && network.requests.size() == 2, "reopening verified models needs no further download");
}

void testErrors(Fixture& fixture)
{
    Network network;
    fixture.configure(network);
    ModelDownloadService service(fixture.root, fixture.models, nullptr, &network);
    require(QFile::remove(QDir(fixture.root).filePath(QStringLiteral("models.json"))), "remove missing-manifest fixture");
    service.check();
    require(service.status().state == ModelDownloadService::State::Failed && network.requests.isEmpty(), "missing manifest fails clearly without guessing a model release");
    write(QDir(fixture.root).filePath(QStringLiteral("models.json")), fixture.manifest);
    network.metadataHttp = 404;
    service.check();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Failed && service.status().message.contains(QStringLiteral("HTTP 404")) &&
        service.status().message.contains(QStringLiteral("continue the jam")), "missing release is a clear recoverable error");
    network.metadataHttp = 200;
    auto invalidMetadata = QJsonDocument::fromJson(network.metadata).object();
    invalidMetadata.insert(QStringLiteral("models_manifest_sha256"), QString(64, QLatin1Char('0')));
    network.metadata = QJsonDocument(invalidMetadata).toJson();
    service.check();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Failed &&
        service.status().message.contains(QStringLiteral("does not match")), "mismatched model release pin rejected before approval");
    fixture.configure(network);
    fixture.missing(service);
    auto json = QJsonDocument::fromJson(network.metadata).object();
    json.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
    network.metadata = QJsonDocument(json).toJson();
    fixture.missing(service);
    service.download();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Failed && service.status().message.contains(QStringLiteral("SHA-256")) &&
        !QFileInfo::exists(QDir(fixture.models).filePath(fixture.names.front())), "corrupt ZIP never installs models");
    fixture.checkCleanup();
    fixture.configure(network);
    fixture.missing(service);
    network.archive.chop(5);
    service.download();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Failed && service.status().message.contains(QStringLiteral("did not complete")), "truncated transfer rejected");
    fixture.checkCleanup();
    network.archive = fixture.archive;
    network.archive.replace("models/", "../bad/");
    network.metadata = fixture.metadata(network.archive);
    fixture.missing(service);
    service.download();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Failed && service.status().message.contains(QStringLiteral("ZIP")), "path traversal rejected before extraction");
    require(!QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("temp/bad"))), "archive cannot write outside staging");
    fixture.checkCleanup();
    // Even a ZIP whose own digest is correct must contain the pinned weights.
    auto manifestObject = QJsonDocument::fromJson(fixture.manifest).object();
    auto modelEntries = manifestObject.value(QStringLiteral("files")).toArray();
    auto firstModel = modelEntries[0].toObject();
    firstModel.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
    modelEntries[0] = firstModel;
    manifestObject.insert(QStringLiteral("files"), modelEntries);
    fixture.manifest = QJsonDocument(manifestObject).toJson();
    write(QDir(fixture.root).filePath(QStringLiteral("models.json")), fixture.manifest);
    fixture.configure(network);
    fixture.missing(service);
    service.download();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Failed &&
        service.status().message.contains(QStringLiteral("SHA-256")) &&
        !QFileInfo::exists(QDir(fixture.models).filePath(fixture.names.front())), "all unpacked weights must verify before any file is installed");
    fixture.checkCleanup();
    ModelDownloadService escaping(fixture.root, QDir(fixture.root).filePath(QStringLiteral("../escaped")), nullptr, &network);
    escaping.check();
    require(escaping.status().state == ModelDownloadService::State::Failed &&
        !QFileInfo::exists(QDir(fixture.root).filePath(QStringLiteral("../escaped"))), "model destination cannot escape installation root");
}

void testCancellationAndLock(Fixture& fixture)
{
    Network network;
    fixture.configure(network);
    network.delay = 100;
    ModelDownloadService service(fixture.root, fixture.models, nullptr, &network);
    service.check();
    waitFor([&] { return service.status().state == ModelDownloadService::State::FetchingMetadata; });
    service.cancel();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Cancelled && network.requests.size() == 1, "metadata cancellation cannot fetch weights");
    fixture.missing(service);
    service.download();
    service.cancel();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Cancelled, "download cancellation completes");
    fixture.checkCleanup();
    network.delay = 1;
    fixture.missing(service);
    service.statusChanged = [&] {
        if (service.status().state == ModelDownloadService::State::Installing) service.cancel();
    };
    service.download();
    waitFor([&] { return !service.busy(); });
    require(service.status().state == ModelDownloadService::State::Cancelled, "verification cancellation prevents install");
    fixture.checkCleanup();
    service.statusChanged = {};
    fixture.missing(service);
    const QString stage = QDir(fixture.root).filePath(QStringLiteral("temp/jamtaster-models"));
    write(QDir(stage).filePath(QStringLiteral("other-instance.txt")), "preserve other download");
    QLockFile lock(QDir(fixture.root).filePath(QStringLiteral("temp/jamtaster-models.lock")));
    require(lock.tryLock(), "hold other instance lock");
    service.download();
    require(service.status().state == ModelDownloadService::State::Failed &&
        read(QDir(stage).filePath(QStringLiteral("other-instance.txt"))) == "preserve other download", "lock contention never deletes another instance's staging");
}

void testDialog(Fixture& fixture)
{
    Network network;
    fixture.configure(network);
    const QString worker = QDir(fixture.root).filePath(QStringLiteral("worker-placeholder"));
    write(worker, "worker");
    // Models remain missing, and the manifest is read from the injected root.
    JamTasterService service(fixture.root, worker);
    const QString dialogModels = QDir(fixture.root).filePath(QStringLiteral("models"));
    JamTasterDialog dialog(service, fixture.root, {}, {}, {}, {}, nullptr, &network);
    dialog.show();
    auto* download = dialog.findChild<QPushButton*>(QStringLiteral("JamTasterDownloadModels"));
    auto* retry = dialog.findChild<QPushButton*>(QStringLiteral("JamTasterRetryModels"));
    auto* label = dialog.findChild<QLabel*>(QStringLiteral("JamTasterModelsStatus"));
    require(download && retry && label, "dialog owns visible model controls");
    waitFor([&] { return download->isVisible(); });
    require(label->text().contains(QStringLiteral("MB")) && network.requests.size() == 1,
        "dialog shows size and waits for download approval");
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Analyse Everything"))
            require(!button->isEnabled(), "analysis disabled while models missing");
    }
    QTimer answer;
    answer.setInterval(1);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            require(message->text().contains(QStringLiteral("MB")), "approval popup describes size");
            message->done(QMessageBox::Cancel);
            answer.stop();
        }
    });
    answer.start();
    download->click();
    require(network.requests.size() == 1, "declining popup starts no archive request");
    network.metadataHttp = 404;
    dialog.reject();
    dialog.show();
    waitFor([&] { return retry->isVisible() && retry->isEnabled() && label->text().contains(QStringLiteral("HTTP 404")); });
    require(label->text().contains(QStringLiteral("HTTP 404")), "dialog shows release failure inline");
    network.metadataHttp = 200;
    retry->click();
    waitFor([&] { return download->isVisible(); });
    network.delay = 100;
    QObject::disconnect(&answer, nullptr, nullptr, nullptr);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message->done(QMessageBox::Yes);
            answer.stop();
        }
    });
    answer.start();
    download->click();
    const auto bars = dialog.findChildren<QProgressBar*>();
    require(std::any_of(bars.begin(), bars.end(), [](auto* bar) { return bar->isVisible(); }), "download has a visible progress bar");
    dialog.reject();
    QApplication::processEvents();
    require(!dialog.isVisible() && !service.taskActive() && !QFileInfo::exists(dialogModels + QStringLiteral("/adtof.onnx")), "close cancels download without starting analysis");
    fixture.checkCleanup();
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        require(argc == 3, "expected CMake executable and build-local workspace");
        const QString workspace = QString::fromLocal8Bit(argv[2]);
        require(QDir().mkpath(workspace), "create build-local workspace");
        QTemporaryDir temporary(QDir(workspace).filePath(QStringLiteral("model-download-XXXXXX")));
        require(temporary.isValid(), "create model fixture workspace");
        const QString cmake = QString::fromLocal8Bit(argv[1]);
        Fixture installed(temporary.filePath(QStringLiteral("installed")), cmake);
        testInstalled(installed);
        Fixture download(temporary.filePath(QStringLiteral("download")), cmake);
        testDownload(download);
        Fixture errors(temporary.filePath(QStringLiteral("errors")), cmake);
        testErrors(errors);
        Fixture cancellation(temporary.filePath(QStringLiteral("cancellation")), cmake);
        testCancellationAndLock(cancellation);
        Fixture dialog(temporary.filePath(QStringLiteral("dialog")), cmake);
        testDialog(dialog);
        std::cout << "Model download and dialog tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
