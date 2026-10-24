#include "ApplicationUpdateService.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <stdexcept>

namespace jam2::update {
ApplicationUpdateService::ApplicationUpdateService(QString root, QString installedVersion, QObject* parent,
    QNetworkAccessManager* network, Preparation* preparation, Installer installer)
    : QObject(parent), root_(std::move(root)), checker_(nullptr, network),
      ownedPreparation_(preparation ? nullptr : std::make_unique<Preparation>(root_)),
      preparation_(preparation ? preparation : ownedPreparation_.get()),
      installer_(installer ? std::move(installer) : Installer(launchPrepared))
{
    status_.installedVersion = std::move(installedVersion);
    checker_.finished = [this](const CheckResult& result, const QString& error) {
        if (!error.isEmpty()) { set(State::Failed, error + QStringLiteral(" You can keep jamming and retry later.")); return; }
        available_ = result;
        if (!result.available) { set(State::Current, QStringLiteral("Jam2 is up to date.")); return; }
        status_.availableVersion = result.available->version;
        status_.bytes = result.available->bytes;
        set(State::Available, QStringLiteral("Jam2 %1 is available. Download %2 MB? Your jam can keep playing.")
            .arg(status_.availableVersion).arg(static_cast<double>(status_.bytes) / 1'000'000.0, 0, 'f', 1));
    };
    preparation_->statusChanged = [this](const PreparationStatus& progress) {
        if (status_.state == State::Cancelling) return;
        State state = State::Preparing;
        if (progress.phase == PreparationPhase::Downloading) state = State::Downloading;
        else if (progress.phase == PreparationPhase::Unpacking) state = State::Unpacking;
        set(state, progress.message, progress.percent);
    };
    preparation_->finished = [this](const QString& error, bool cancelled) {
        if (cancelled) { status_.installOnExit = false; set(State::Cancelled, QStringLiteral("Update cancelled. Your jam can keep playing.")); return; }
        if (!error.isEmpty()) {
            status_.installOnExit = false;
            try { if (auto result = readResult(root_)) { result->reported = true; writeResult(root_, *result); } } catch (...) {}
            set(State::Failed, error + QStringLiteral(" Your current Jam2 installation is unchanged. You can keep jamming and retry later.")); return;
        }
        restart_ = false;
        status_.installOnExit = true;
        set(State::Ready, QStringLiteral("The update is downloaded and verified. Restart now, or keep jamming and install when Jam2 closes."), 100);
    };
}
ApplicationUpdateService::~ApplicationUpdateService()
{
    statusChanged = {}; checker_.finished = {}; checker_.cancel();
    preparation_->statusChanged = {}; preparation_->finished = {};
}
void ApplicationUpdateService::set(State state, QString message, int percent)
{
    status_.state = state; status_.message = std::move(message); status_.percent = percent;
    if (statusChanged) statusChanged();
}
void ApplicationUpdateService::check()
{
    if (preparation_->active() || status_.installOnExit || status_.state == State::Restarting || status_.state == State::Checking) return;
    available_ = {}; status_.availableVersion.clear(); status_.bytes = 0;
    set(State::Checking, QStringLiteral("Checking for Jam2 updates..."));
    checker_.start(status_.installedVersion);
}
void ApplicationUpdateService::download()
{
    if (status_.state != State::Available || !available_.available) return;
    set(State::Preparing, QStringLiteral("Preparing the update download..."));
    preparation_->start(*available_.available, available_.encodedMetadata);
}
void ApplicationUpdateService::cancel()
{
    if (status_.state == State::Checking) {
        checker_.cancel(); set(State::Cancelled, QStringLiteral("Update check cancelled.")); return;
    }
    if (status_.state == State::Available) { set(State::Cancelled, QStringLiteral("No update was downloaded.")); return; }
    if (!preparation_->active() && !status_.installOnExit) return;
    status_.installOnExit = false; restart_ = false;
    set(State::Cancelling, QStringLiteral("Cancelling the update and removing temporary files..."));
    preparation_->cancel();
}
void ApplicationUpdateService::defer()
{
    if (status_.state != State::Ready && status_.state != State::Restarting) return;
    restart_ = false; status_.installOnExit = true;
    set(State::Ready, QStringLiteral("Keep jamming. This update will install when you close Jam2 normally; Jam2 will not reopen."), 100);
}
void ApplicationUpdateService::requestRestart(std::function<bool()> closeWindow)
{
    if (status_.state != State::Ready) return;
    restart_ = true;
    set(State::Restarting, QStringLiteral("Closing Jam2 to install the update, then reopening it. Save your project if prompted."), 100);
    QTimer::singleShot(0, this, [this, closeWindow = std::move(closeWindow)] {
        if (status_.state != State::Restarting) return;
        if (!closeWindow()) {
            restart_ = false;
            set(State::Ready, QStringLiteral("Restart cancelled. The update stays ready and will install on your next normal exit, without reopening Jam2."), 100);
        }
    });
}
QString ApplicationUpdateService::windowClosing()
{
    checker_.cancel();
    if (launched_) return {};
    if (status_.installOnExit && (status_.state == State::Ready || status_.state == State::Restarting)) {
        try {
            installer_({root_, status_.availableVersion, QCoreApplication::applicationPid(), restart_, true});
            launched_ = true; status_.installOnExit = false;
        } catch (const std::exception& error) {
            status_.installOnExit = false;
            const auto message = QString::fromUtf8(error.what());
            set(State::Failed, message); return message;
        }
    } else if (preparation_->active()) cancel();
    return {};
}
}
