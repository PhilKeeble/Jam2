#include "UpdateUi.hpp"
#include "UpdateDialog.hpp"
#include <QApplication>
#include <QMessageBox>
#include <QWidget>

UpdateUi::UpdateUi(QWidget* window, QString root, QString installedVersion, std::function<bool()> closeWindow,
    jam2::update::ApplicationUpdateService* service)
    : QObject(nullptr), window_(window),
      ownedService_(service ? nullptr : std::make_unique<jam2::update::ApplicationUpdateService>(std::move(root), std::move(installedVersion))),
      service_(service ? service : ownedService_.get()),
      dialog_(std::make_unique<UpdateDialog>(*service_, std::move(closeWindow), window))
{
    service_->statusChanged = [this] { refresh(); };
    promptTimer_.setInterval(500);
    connect(&promptTimer_, &QTimer::timeout, this, [this] {
        using jam2::update::State;
        const auto state = service_->status().state;
        if (closing_ || (state != State::Available && state != State::Ready && state != State::Failed)) { promptTimer_.stop(); return; }
        if (!QApplication::activeModalWidget()) { promptTimer_.stop(); dialog_->show(); dialog_->raise(); }
    });
}
UpdateUi::~UpdateUi() { service_->statusChanged = {}; }
void UpdateUi::startAutomaticCheck()
{
    QTimer::singleShot(3000, this, [this] {
        if (!closing_ && service_->status().state == jam2::update::State::Idle) service_->check();
    });
}
void UpdateUi::showDialog()
{
    promptTimer_.stop();
    using jam2::update::State;
    const auto state = service_->status().state;
    if (state == State::Idle || state == State::Current || state == State::Cancelled) service_->check();
    dialog_->refresh(); dialog_->show(); dialog_->raise(); dialog_->activateWindow();
}
void UpdateUi::refresh()
{
    using jam2::update::State;
    const auto& status = service_->status();
    dialog_->refresh();
    QString text = QStringLiteral("Updates");
    if (status.state == State::Available) text = QStringLiteral("Update available");
    else if (status.state == State::Ready) text = QStringLiteral("Update ready");
    else if (status.state == State::Checking) text = QStringLiteral("Checking updates");
    else if (status.state == State::Preparing || status.state == State::Downloading || status.state == State::Unpacking)
        text = status.percent < 0 ? QStringLiteral("Updating...") : QStringLiteral("Update %1%").arg(status.percent);
    else if (status.state == State::Restarting) text = QStringLiteral("Restarting...");
    if (presentationChanged) presentationChanged(text, status.message);
    const bool preparationFailed = status.state == State::Failed &&
        (lastState_ == State::Preparing || lastState_ == State::Downloading || lastState_ == State::Unpacking);
    if (!closing_ && !dialog_->isVisible() && status.state != lastState_ &&
        (status.state == State::Available || status.state == State::Ready || preparationFailed)) promptTimer_.start();
    lastState_ = status.state;
}
void UpdateUi::windowClosing()
{
    if (closing_) return;
    closing_ = true; promptTimer_.stop(); dialog_->hide();
    const auto error = service_->windowClosing();
    if (!error.isEmpty()) QMessageBox::warning(window_, QStringLiteral("Could not start the update"),
        error + QStringLiteral("\n\nJam2 will close without applying the update. The downloaded package remains in update/ for retry."));
}
