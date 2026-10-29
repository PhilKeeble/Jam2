#include "SessionNotifications.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace jam2::gui {

PeerDisconnectedDialog::PeerDisconnectedDialog(const QString& peerName, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("peer-disconnected-dialog"));
    setWindowTitle(QStringLiteral("Peer disconnected"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    auto* layout = new QVBoxLayout(this);
    auto* label = new QLabel(peerName + QStringLiteral(" disconnected"), this);
    label->setObjectName(QStringLiteral("peer-disconnected-message"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
    // A plain modeless QDialog does not invoke QMessageBox's alert sound.
}

QString PeerDisconnectedDialog::message() const
{
    return findChild<QLabel*>(QStringLiteral("peer-disconnected-message"))->text();
}

SessionNotifications::~SessionNotifications() { reset(); }

void SessionNotifications::beginStartup()
{
    reset();
    starting_ = true;
}

void SessionNotifications::reset()
{
    starting_ = false;
    members_.clear();
    for (const auto& dialog : dialogs_) if (dialog) delete dialog.data();
    dialogs_.clear();
    departureCount_ = 0;
    lastDepartureName_.clear();
}

void SessionNotifications::observe(
    const SharedSessionController::Snapshot& session, QWidget* parent, bool showDialogs)
{
    if (session.role == SharedSessionController::Role::Inactive ||
        session.role == SharedSessionController::Role::Local) {
        reset();
        return;
    }
    if (!session.membershipReady) return;
    QVector<Member> next;
    for (const auto& peer : session.peers) {
        if (peer.token != session.localToken) next.push_back({peer.token, peer.name.text()});
    }
    // Commit the new membership before opening any windows. Repeated snapshots,
    // including logical control recovery, cannot repeat a departure notification.
    const auto previous = std::exchange(members_, std::move(next));
    dialogs_.removeIf([](const auto& dialog) { return dialog.isNull(); });
    for (const auto& peer : previous) {
        if (std::any_of(members_.cbegin(), members_.cend(), [&](const auto& member) {
                return member.token == peer.token;
            })) continue;
        ++departureCount_;
        lastDepartureName_ = peer.name;
        if (showDialogs) {
            auto* dialog = new PeerDisconnectedDialog(peer.name, parent);
            dialogs_.push_back(dialog);
            dialog->show();
        }
    }
}

QVector<PeerDisconnectedDialog*> SessionNotifications::visibleDialogs() const
{
    QVector<PeerDisconnectedDialog*> result;
    for (const auto& dialog : dialogs_) if (dialog && dialog->isVisible()) result.push_back(dialog.data());
    return result;
}

} // namespace jam2::gui
