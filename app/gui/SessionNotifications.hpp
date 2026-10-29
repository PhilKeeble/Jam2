#pragma once

#include "SharedSessionController.hpp"

#include <QDialog>
#include <QPointer>
#include <QVector>

namespace jam2::gui {

class PeerDisconnectedDialog final : public QDialog {
public:
    explicit PeerDisconnectedDialog(const QString& peerName, QWidget* parent);
    QString message() const;
};

// Session presentation state stays on the GUI thread, outside the audio path.
class SessionNotifications final {
public:
    ~SessionNotifications();
    void beginStartup();
    void finishStartup() { starting_ = false; }
    bool startupPending() const { return starting_; }
    void reset();
    void observe(const SharedSessionController::Snapshot& session, QWidget* parent, bool showDialogs);
    quint64 departureCount() const { return departureCount_; }
    const QString& lastDepartureName() const { return lastDepartureName_; }
    QVector<PeerDisconnectedDialog*> visibleDialogs() const;

private:
    struct Member { QString token; QString name; };
    bool starting_ = false;
    QVector<Member> members_;
    QVector<QPointer<PeerDisconnectedDialog>> dialogs_;
    quint64 departureCount_ = 0;
    QString lastDepartureName_;
};

} // namespace jam2::gui
