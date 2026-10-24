#include "UpdateDialog.hpp"
#include "ApplicationUpdateService.hpp"
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

UpdateDialog::UpdateDialog(jam2::update::ApplicationUpdateService& service,
    std::function<bool()> closeApplication, QWidget* parent)
    : QDialog(parent), service_(service), closeApplication_(std::move(closeApplication))
{
    setWindowTitle(QStringLiteral("Jam2 update"));
    setObjectName(QStringLiteral("Jam2UpdateDialog"));
    setModal(false);
    setProperty("jam2MaximumDialogHeight", 460);
    resize(570, 180);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14); layout->setSpacing(8);
    heading_ = new QLabel(this);
    auto font = heading_->font(); font.setBold(true); font.setPointSize(font.pointSize() + 2); heading_->setFont(font);
    message_ = new QLabel(this); message_->setObjectName(QStringLiteral("Jam2UpdateMessage"));
    explanation_ = new QLabel(this); explanation_->setProperty("muted", true);
    for (auto* label : {heading_, message_, explanation_}) {
        label->setWordWrap(true); label->setTextFormat(Qt::PlainText); layout->addWidget(label);
    }
    progress_ = new QProgressBar(this); progress_->setObjectName(QStringLiteral("Jam2UpdateProgress")); layout->addWidget(progress_);
    auto* actions = new QDialogButtonBox(this);
    const auto button = [this, actions](const QString& text, const QString& name, QDialogButtonBox::ButtonRole role) {
        auto* result = actions->addButton(text, role); result->setObjectName(name); result->setAutoDefault(false); return result;
    };
    download_ = button(QStringLiteral("Download update"), QStringLiteral("Jam2DownloadUpdate"), QDialogButtonBox::ActionRole);
    restart_ = button(QStringLiteral("Restart and install"), QStringLiteral("Jam2RestartUpdate"), QDialogButtonBox::ActionRole);
    later_ = button(QStringLiteral("Install on exit"), QStringLiteral("Jam2DeferUpdate"), QDialogButtonBox::ActionRole);
    cancel_ = button(QStringLiteral("Cancel update"), QStringLiteral("Jam2CancelUpdate"), QDialogButtonBox::ActionRole);
    retry_ = button(QStringLiteral("Check again"), QStringLiteral("Jam2RetryUpdate"), QDialogButtonBox::ActionRole);
    close_ = button(QStringLiteral("Close"), QStringLiteral("Jam2CloseUpdate"), QDialogButtonBox::RejectRole);
    layout->addWidget(actions);
    connect(download_, &QPushButton::clicked, this, [this] { service_.download(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { service_.cancel(); });
    connect(retry_, &QPushButton::clicked, this, [this] { service_.check(); });
    connect(later_, &QPushButton::clicked, this, [this] { service_.defer(); hide(); });
    connect(close_, &QPushButton::clicked, this, &QWidget::close);
    connect(restart_, &QPushButton::clicked, this, [this] {
        const QPointer<UpdateDialog> dialog(this);
        service_.requestRestart([dialog] {
            if (!dialog) return false;
            dialog->hide();
            const bool closed = dialog->closeApplication_();
            if (!closed && dialog) { dialog->show(); dialog->raise(); dialog->activateWindow(); }
            return closed;
        });
    });
    refresh();
}
void UpdateDialog::refresh()
{
    using jam2::update::State;
    const auto& status = service_.status();
    const auto state = status.state;
    heading_->setText(status.availableVersion.isEmpty()
        ? QStringLiteral("Jam2 updates") : QStringLiteral("Jam2 %1").arg(status.availableVersion));
    message_->setText(status.message);
    const bool busy = state == State::Preparing || state == State::Downloading || state == State::Unpacking;
    const bool ready = state == State::Ready;
    const bool restarting = state == State::Restarting;
    if (ready || restarting) {
        explanation_->setText(QStringLiteral(
            "Restart and install stops the jam, updates Jam2 and reopens it. "
            "Install on exit updates when you close Jam2, without reopening it. "
            "Cancelling a save prompt leaves the update ready for later."));
    } else if (busy) {
        explanation_->setText(QStringLiteral("Your jam can keep playing. Close this window to continue in the background, or cancel the update. Closing Jam2 during download cancels it."));
    } else if (state == State::Available) {
        explanation_->setText(QStringLiteral("Installed version: %1. Downloading will not close Jam2. You choose when to install after verification.").arg(status.installedVersion));
    } else explanation_->setText(QStringLiteral("Update checks and downloads run in the background. Settings, models and user content stay in your Jam2 folder."));
    progress_->setVisible(busy || ready || restarting || state == State::Checking || state == State::Cancelling);
    progress_->setRange(0, status.percent < 0 ? 0 : 100);
    if (status.percent >= 0) progress_->setValue(status.percent);
    download_->setVisible(state == State::Available);
    restart_->setVisible(ready);
    later_->setVisible(ready || restarting);
    later_->setEnabled(!restarting);
    cancel_->setVisible(busy || ready || state == State::Checking);
    cancel_->setText(ready ? QStringLiteral("Discard update") : state == State::Checking
        ? QStringLiteral("Cancel check") : QStringLiteral("Cancel update"));
    retry_->setVisible(state == State::Failed || state == State::Current || state == State::Cancelled || state == State::Idle);
    close_->setText(ready || busy ? QStringLiteral("Keep jamming") : QStringLiteral("Close"));
    // Fit the current text and visible actions instead of stretching labels
    // through a fixed-height window. Refresh the size used by the app's dialog policy too.
    layout()->invalidate();
    layout()->activate();
    const int fittedWidth = qMax(570, minimumSizeHint().width());
    const int fittedHeight = layout()->hasHeightForWidth()
        ? layout()->totalHeightForWidth(fittedWidth) : sizeHint().height();
    resize(fittedWidth, fittedHeight);
    setProperty("jam2PreparedDialogSize", size());
}
void UpdateDialog::closeEvent(QCloseEvent* event)
{
    if (service_.status().state == jam2::update::State::Ready) service_.defer();
    QDialog::closeEvent(event);
}
