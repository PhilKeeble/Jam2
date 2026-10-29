#include "SessionNotifications.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
SharedSessionController::PeerSnapshot peer(const QString& token, const QString& name)
{
    SharedSessionController::PeerSnapshot result;
    result.token = token;
    const auto parsed = jam2::application::PeerName::fromInput(name);
    require(parsed.has_value(), "fixture requires a valid peer name");
    result.name = *parsed;
    return result;
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        QWidget owner;
        jam2::gui::SessionNotifications notifications;
        notifications.beginStartup();
        require(notifications.startupPending(), "starting session must allow startup errors");
        notifications.finishStartup();
        require(!notifications.startupPending(), "established session must not allow startup errors");
        SharedSessionController::Snapshot session;
        session.role = SharedSessionController::Role::Creator;
        session.localToken = "local";
        session.membershipReady = true;
        session.peers = {peer("local", "Local"), peer("a", "Alex"), peer("b", "Alex"), peer("c", "Mac")};
        notifications.observe(session, &owner, true);
        require(notifications.visibleDialogs().isEmpty(), "initial membership must not announce departures");
        session.membershipReady = false;
        session.peers.clear();
        notifications.observe(session, &owner, true);
        require(notifications.departureCount() == 0, "partial membership must not announce departures");
        session.membershipReady = true;
        session.peers = {peer("local", "Local"), peer("a", "Alex"), peer("b", "Alex")};
        notifications.observe(session, &owner, true);
        auto dialogs = notifications.visibleDialogs();
        require(dialogs.size() == 1 && notifications.lastDepartureName() == "Mac",
            "departure must use the previous authenticated display name");
        auto* dialog = dialogs.front();
        require(dialog->message() == "Mac disconnected" && !dialog->isModal() &&
            dialog->windowModality() == Qt::NonModal && QApplication::activeModalWidget() == nullptr,
            "real departure dialog must be named and nonmodal");
        require(dialog->findChild<QLabel*>()->textFormat() == Qt::PlainText,
            "peer names must be shown literally");
        QPointer<jam2::gui::PeerDisconnectedDialog> owned = dialog;
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(owned.isNull(), "real OK button must close and release the dialog");
        notifications.observe(session, &owner, true);
        require(notifications.departureCount() == 1 && notifications.visibleDialogs().isEmpty(),
            "repeated snapshots must not repeat notifications");
        session.peers.removeLast();
        notifications.observe(session, &owner, false);
        require(notifications.departureCount() == 2 && notifications.lastDepartureName() == "Alex",
            "duplicate display names must retain distinct membership identities");
        session.peers.push_back(peer("b", "Alex"));
        notifications.observe(session, &owner, true);
        session.peers.removeLast();
        notifications.observe(session, &owner, true);
        require(notifications.departureCount() == 3 && notifications.visibleDialogs().size() == 1,
            "a rejoined member may depart again");
        QPointer<jam2::gui::PeerDisconnectedDialog> remaining = notifications.visibleDialogs().front();
        notifications.reset();
        require(remaining.isNull() && notifications.departureCount() == 0 && !notifications.startupPending(),
            "local leave or cancellation must clean up presentation and ownership");
        session.role = SharedSessionController::Role::Joiner;
        notifications.observe(session, &owner, true);
        session.peers.clear(); session.role = SharedSessionController::Role::Inactive;
        notifications.observe(session, &owner, true);
        require(notifications.visibleDialogs().isEmpty(), "whole-session shutdown must not announce every member");
        std::cout << "session notification startup state, membership, quiet dialog and cleanup passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
