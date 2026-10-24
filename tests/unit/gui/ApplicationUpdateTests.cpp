#include "ApplicationUpdateService.hpp"
#include "UpdateDialog.hpp"
#include "UpdateUi.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace jam2::update;
namespace {
void require(bool valid, const char* error) { if (!valid) throw std::runtime_error(error); }
void waitFor(const std::function<bool()>& done)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!done() && elapsed.elapsed() < 10000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    require(done(), "asynchronous application update timed out");
}
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
    qint64 readData(char* target, qint64 limit) override
    {
        const auto bytes = std::min(limit, available_ - offset_);
        if (bytes <= 0) return -1;
        std::memcpy(target, bytes_.constData() + offset_, static_cast<size_t>(bytes)); offset_ += bytes; return bytes;
    }
private:
    QByteArray bytes_; qint64 offset_ = 0; qint64 available_ = 0;
};
class Network final : public QNetworkAccessManager {
public:
    QByteArray version = "0.10.0\n";
    QByteArray metadata;
    int versionHttp = 200;
    int metadataHttp = 200;
    int delay = 1;
    QStringList requests;
    Network()
    {
        metadata = QJsonDocument(QJsonObject{{"schema", 1}, {"version", "0.10.0"}, {"tag", "v0.10.0"},
            {"channel", "stable"}, {"asset", assetName()},
            {"url", QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/v0.10.0/%1").arg(assetName())},
            {"bytes", 125000000}, {"sha256", QString(64, 'a')}, {"models_manifest_sha256", QString(64, 'b')}}).toJson();
    }
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requests << request.url().toString();
        const bool first = request.url().path() == QStringLiteral("/version.txt");
        return new Reply(request, first ? version : metadata, first ? versionHttp : metadataHttp, delay, this);
    }
};
class FakePreparation final : public Preparation {
public:
    FakePreparation() : Preparation({}) {}
    bool running = false;
    int starts = 0;
    int cancellations = 0;
    Metadata approved;
    QByteArray encoded;
    void start(const Metadata& metadata, const QByteArray& json, bool = false) override
    {
        ++starts; approved = metadata; encoded = json; running = true;
        if (statusChanged) statusChanged({PreparationPhase::Staging, QStringLiteral("Preparing update..."), -1});
    }
    bool active() const override { return running; }
    void cancel() override
    {
        ++cancellations; running = false;
        QTimer::singleShot(0, this, [this] { if (finished) finished({}, true); });
    }
    void progress(PreparationPhase phase, int percent)
    {
        if (statusChanged) statusChanged({phase, QStringLiteral("Working in background..."), percent});
    }
    void complete(const QString& error = {}) { running = false; if (finished) finished(error, false); }
};
struct Fixture {
    Network network;
    FakePreparation preparation;
    QVector<LaunchOptions> installs;
    ApplicationUpdateService service;
    Fixture() : service(QDir::currentPath(), QStringLiteral("0.9.10"), nullptr, &network, &preparation,
        [this](const LaunchOptions& options) -> qint64 { installs.push_back(options); return 123; }) {}
    void available()
    {
        service.check(); waitFor([&] { return service.status().state != State::Checking; });
        require(service.status().state == State::Available, "newer version and metadata produce approval state");
    }
    void ready() { available(); service.download(); preparation.complete(); require(service.status().state == State::Ready, "download produces ready state"); }
};
QPushButton* button(UpdateDialog& dialog, const char* name)
{
    auto* result = dialog.findChild<QPushButton*>(QString::fromLatin1(name));
    require(result != nullptr, "update dialog has expected action"); return result;
}

void testVersionCheck()
{
    require(newerVersion(QStringLiteral("0.10.0"), QStringLiteral("0.9.10")) &&
        !newerVersion(QStringLiteral("0.9.99"), QStringLiteral("0.10.0")) &&
        !newerVersion(QStringLiteral("0.10.0"), QStringLiteral("0.10.0")), "semantic version comparison is numeric");
    Fixture fixture;
    fixture.network.delay = 10;
    int heartbeat = 0; QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; }); timer.start(1);
    fixture.available();
    require(heartbeat > 0 && fixture.network.requests.size() == 2 && fixture.preparation.starts == 0,
        "version/metadata checks are asynchronous and never download before approval");
    require(fixture.network.requests[1].contains(QStringLiteral("/v0.10.0/")) &&
        fixture.network.requests[1].endsWith(QStringLiteral(".zip.json")), "metadata GET pins exact advertised version");
    fixture.network.version = "0.9.10\n";
    const auto before = fixture.network.requests.size();
    fixture.service.check(); waitFor([&] { return fixture.service.status().state != State::Checking; });
    require(fixture.service.status().state == State::Current && fixture.network.requests.size() == before + 1,
        "unchanged version fetches no GitHub metadata");
    fixture.network.version = "0.9.9";
    fixture.service.check(); waitFor([&] { return fixture.service.status().state != State::Checking; });
    require(fixture.service.status().state == State::Current, "older advertised version cannot downgrade");
    fixture.network.version = "not-a-version";
    fixture.service.check(); waitFor([&] { return fixture.service.status().state != State::Checking; });
    require(fixture.service.status().state == State::Failed && fixture.preparation.starts == 0,
        "invalid version fails without fetching archive");
    fixture.network.version = "0.10.0"; fixture.network.metadataHttp = 404;
    fixture.service.check(); waitFor([&] { return fixture.service.status().state != State::Checking; });
    require(fixture.service.status().message.contains(QStringLiteral("HTTP 404")), "metadata failure is recoverable and explicit");
    fixture.network.metadataHttp = 200;
    fixture.available(); fixture.service.download();
    require(fixture.preparation.approved.version == QStringLiteral("0.10.0") &&
        fixture.preparation.encoded == fixture.network.metadata, "user approval pins metadata passed into helper");
}

void testDialogAndCancellation()
{
    Fixture fixture;
    int closeRequests = 0;
    UpdateDialog dialog(fixture.service, [&] { ++closeRequests; return true; });
    fixture.service.statusChanged = [&] { dialog.refresh(); };
    dialog.show(); fixture.available();
    require(button(dialog, "Jam2DownloadUpdate")->isVisible() && !button(dialog, "Jam2RestartUpdate")->isVisible(),
        "available popup offers download before any restart");
    require(dialog.findChild<QLabel*>(QStringLiteral("Jam2UpdateMessage"))->text().contains(QStringLiteral("125.0 MB")),
        "popup shows exact download size before consent");
    QApplication::processEvents();
    require(dialog.height() < 260 && dialog.height() <= dialog.layout()->totalHeightForWidth(dialog.width()) + 2,
        "available popup fits its visible content without stretched vertical gaps");
    button(dialog, "Jam2DownloadUpdate")->click();
    require(fixture.preparation.starts == 1 && fixture.service.status().state == State::Preparing, "actual download button starts helper asynchronously");
    fixture.preparation.progress(PreparationPhase::Downloading, 42);
    auto* bar = dialog.findChild<QProgressBar*>(QStringLiteral("Jam2UpdateProgress"));
    require(bar && bar->isVisible() && bar->maximum() == 100 && bar->value() == 42,
        "real progress bar presents download percent");
    dialog.close();
    require(fixture.preparation.running && fixture.preparation.cancellations == 0, "closing progress popup leaves jam and background download running");
    dialog.show(); button(dialog, "Jam2CancelUpdate")->click();
    waitFor([&] { return fixture.service.status().state == State::Cancelled; });
    require(fixture.preparation.cancellations == 1 && closeRequests == 0 && fixture.installs.isEmpty(),
        "cancel button cancels preparation without closing or installing");
    fixture.ready();
    require(button(dialog, "Jam2RestartUpdate")->isVisible() && button(dialog, "Jam2DeferUpdate")->isVisible() && bar->value() == 100,
        "ready popup presents explicit restart and install-on-exit choices");
    button(dialog, "Jam2DeferUpdate")->click();
    require(!dialog.isVisible() && fixture.service.status().installOnExit && fixture.installs.isEmpty(),
        "install-on-exit defers without quitting");
    fixture.service.windowClosing();
    require(fixture.installs.size() == 1 && !fixture.installs.front().restart && fixture.installs.front().staged,
        "normal exit applies downloaded update without reopening");
    fixture.service.windowClosing();
    require(fixture.installs.size() == 1, "duplicate closing notifications cannot launch twice");
}

void testRestartAndSaveCancellation()
{
    Fixture fixture;
    bool savePromptSeen = false;
    UpdateDialog dialog(fixture.service, [&] {
        const auto result = QMessageBox::question(nullptr, QStringLiteral("Save project"), QStringLiteral("Save before closing?"),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        savePromptSeen = true;
        if (result == QMessageBox::Cancel) return false;
        fixture.service.windowClosing(); return true;
    });
    fixture.service.statusChanged = [&] { dialog.refresh(); };
    dialog.show(); fixture.ready();
    QTimer answer; answer.setInterval(1);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message->button(QMessageBox::Cancel)->click(); answer.stop();
        }
    }); answer.start();
    button(dialog, "Jam2RestartUpdate")->click();
    waitFor([&] { return savePromptSeen && fixture.service.status().state == State::Ready; });
    require(dialog.isVisible() && fixture.installs.isEmpty() && fixture.service.status().installOnExit &&
        fixture.service.status().message.contains(QStringLiteral("Restart cancelled")),
        "cancelling modal save/close keeps app open and verified package queued");
    fixture.service.windowClosing();
    require(fixture.installs.size() == 1 && !fixture.installs.front().restart, "later normal exit after cancelled restart does not reopen");

    Fixture restart;
    UpdateDialog restartDialog(restart.service, [&] { restart.service.windowClosing(); return true; });
    restart.service.statusChanged = [&] { restartDialog.refresh(); };
    restartDialog.show(); restart.ready();
    button(restartDialog, "Jam2RestartUpdate")->click();
    waitFor([&] { return !restart.installs.isEmpty(); });
    require(restart.installs.front().restart && !restartDialog.isVisible(), "restart choice launches installation with reopening enabled");
}

void testReadyCloseDiscardAndFailure()
{
    Fixture fixture;
    UpdateDialog dialog(fixture.service, [] { return false; });
    fixture.service.statusChanged = [&] { dialog.refresh(); };
    dialog.show(); fixture.ready(); dialog.close();
    require(fixture.service.status().installOnExit && fixture.preparation.cancellations == 0, "closing ready popup queues install without reopening");
    dialog.show(); button(dialog, "Jam2CancelUpdate")->click();
    waitFor([&] { return fixture.service.status().state == State::Cancelled; });
    fixture.service.windowClosing(); require(fixture.installs.isEmpty(), "discarded update cannot install on exit");
    fixture.available(); fixture.service.download(); fixture.preparation.complete(QStringLiteral("ZIP failed SHA-256 verification"));
    require(fixture.service.status().state == State::Failed && button(dialog, "Jam2RetryUpdate")->isVisible() &&
        fixture.service.status().message.contains(QStringLiteral("SHA-256")), "corrupt download reports error and retry without quitting");
    QApplication::processEvents();
    require(dialog.height() <= dialog.layout()->totalHeightForWidth(dialog.width()) + 2,
        "error popup shrinks to fit after the progress bar and actions are hidden");
    fixture.available(); fixture.service.download(); fixture.service.windowClosing();
    waitFor([&] { return fixture.service.status().state == State::Cancelled; });
    require(fixture.installs.isEmpty(), "closing app during incomplete preparation cancels instead of installing");
}

void testAutomaticPresenter()
{
    Fixture fixture;
    fixture.network.versionHttp = 404;
    QWidget window; window.show();
    UpdateUi ui(&window, {}, {}, [] { return false; }, &fixture.service);
    QString presentation;
    ui.presentationChanged = [&](const QString& text, const QString&) { presentation = text; };
    ui.startAutomaticCheck();
    waitFor([&] { return fixture.service.status().state == State::Failed; });
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("Jam2UpdateDialog"));
    require(dialog && !dialog->isVisible() && presentation == QStringLiteral("Updates"),
        "automatic startup failure stays silent and does not block the jam");
    const auto requests = fixture.network.requests.size();
    ui.showDialog();
    require(dialog->isVisible() && fixture.service.status().state == State::Failed && fixture.network.requests.size() == requests,
        "opening failed-update dialog preserves the actual error until explicit retry");
    fixture.network.versionHttp = 200;
    dialog->findChild<QPushButton*>(QStringLiteral("Jam2RetryUpdate"))->click();
    waitFor([&] { return fixture.service.status().state == State::Available; });
    fixture.service.download(); fixture.preparation.complete();
    require(presentation == QStringLiteral("Update ready"), "header reflects queued update");
    dialog->findChild<QPushButton*>(QStringLiteral("Jam2RestartUpdate"))->click();
    waitFor([&] { return fixture.service.status().state == State::Ready; });
    dialog->close();
    QElapsedTimer elapsed; elapsed.start();
    waitFor([&] { return elapsed.elapsed() > 650; });
    require(!dialog->isVisible() && fixture.service.status().installOnExit, "dismissing ready popup after cancelled restart does not reopen it");
    fixture.service.cancel(); waitFor([&] { return fixture.service.status().state == State::Cancelled; });
    fixture.available(); fixture.service.download();
    fixture.preparation.complete(QStringLiteral("Expected download error"));
    waitFor([&] { return dialog->isVisible(); });
    require(fixture.service.status().state == State::Failed &&
        dialog->findChild<QLabel*>(QStringLiteral("Jam2UpdateMessage"))->text().contains(QStringLiteral("Expected download error")),
        "preparation failure is shown when background progress was dismissed");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        testVersionCheck(); testDialogAndCancellation(); testRestartAndSaveCancellation(); testReadyCloseDiscardAndFailure();
        testAutomaticPresenter();
        std::cout << "Application update GUI/service tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
