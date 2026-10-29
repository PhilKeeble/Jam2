#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
void require(bool ok, const QString& reason) { if (!ok) throw std::runtime_error(reason.toStdString()); }
QJsonObject command(AutomationProcess& peer, const QJsonObject& request, const QString& expected)
{
    QString error; QJsonObject result;
    require(peer.send(request, error) && peer.readEvent(result, 15s, error), error);
    require(result.value("event") == expected, QString::fromUtf8(QJsonDocument(result).toJson()));
    return result;
}
QJsonObject snapshot(AutomationProcess& peer)
{
    return command(peer, {{"type", "snapshot"}, {"id", "departure"}, {"control", "looper.listening-tools"}}, "snapshot");
}
template<class Predicate> void until(Predicate predicate, const char* reason)
{
    QElapsedTimer timer; timer.start();
    do {
        if (predicate()) return;
        QThread::msleep(30);
    } while (timer.elapsed() < 30000);
    require(false, reason);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2, "requires release/jam2.exe");
        FourPeerCoordinator peers;
        LoopbackPortReservations ports; QString error;
        require(ports.reserve(4, error), error);
        std::array<quint16, 4> local;
        std::array<QStringList, 4> arguments;
        for (int i = 0; i < 4; ++i) {
            local[i] = ports.port(i);
            arguments[i] = {"debug", "gui-agent", "--instance-id", QStringLiteral("departure-%1").arg(i)};
        }
        require(peers.launch(QString::fromLocal8Bit(argv[1]), arguments, error), error);
        for (int i = 0; i < 4; ++i) {
            QJsonObject hello;
            require(peers.peer(i).readEvent(hello, 30s, error) && hello.value("event") == "hello", error);
            until([&] {
                return snapshot(peers.peer(i)).value("window").toObject()
                    .value("active_modal_title") == "Start Local Engine";
            }, "initial local engine setup dialog did not open");
            command(peers.peer(i), {{"type", "invoke"}, {"id", "cancel-local"},
                {"control", "application.local-engine.cancel"}, {"operation", "click"}}, "command_applied");
        }
        ports.release();
        const auto created = command(peers.peer(0), {{"type", "jam.create"}, {"id", "create"},
            {"port", local[0]}, {"name", "Alice"}, {"strategy", "direct"}, {"test_input", "tone-440"}}, "command_applied");
        const std::array<QString, 4> names{"Alice", "Alex", "Alex", QString::fromUtf8("\xc3\x89lodie")};
        for (int i = 1; i < 4; ++i) command(peers.peer(i), {{"type", "jam.join"}, {"id", "join"},
            {"port", local[i]}, {"name", names[i]}, {"invite_url", created.value("invite_url")},
            {"test_input", "tone-440"}}, "command_applied");
        until([&] {
            for (int i = 0; i < 4; ++i) {
                const auto jam = snapshot(peers.peer(i)).value("jam").toObject();
                if (jam.value("active_remote_peer_count").toInt() != 3 || jam.value("startup_pending").toBool()) return false;
                require(jam.value("last_startup_failure").toString().isEmpty(), "clean startup reported a failure");
            }
            return true;
        }, "four-peer mesh did not establish");
        command(peers.peer(3), {{"type", "jam.leave"}, {"id", "leave"}}, "command_applied");
        until([&] {
            for (int i = 0; i < 3; ++i) {
                const auto state = snapshot(peers.peer(i)); const auto jam = state.value("jam").toObject();
                if (jam.value("active_remote_peer_count").toInt() != 2 || jam.value("peer_departure_count").toInt() != 1) return false;
                require(jam.value("last_departed_peer_name") == names[3], "departure did not preserve the authenticated display name");
                require(jam.value("last_startup_failure").toString().isEmpty() && jam.value("network_running").toBool(),
                    "one peer leaving failed or stopped the remaining jam");
            }
            return true;
        }, "remaining peers did not observe one named departure");
        // Exercise the existing asynchronous failure presentation boundary after
        // establishment; it must no longer produce a startup-error popup.
        command(peers.peer(0), {{"type", "application.boundary"}, {"id", "late-error"},
            {"action", "failure-presentation"}}, "command_applied");
        for (int pass = 0; pass < 3; ++pass) {
            for (int i = 0; i < 3; ++i) {
                const auto state = snapshot(peers.peer(i)); const auto jam = state.value("jam").toObject();
                require(jam.value("peer_departure_count").toInt() == 1 && jam.value("last_startup_failure").toString().isEmpty(),
                    "repeated snapshots or late errors produced another notification/startup failure");
                require(!state.value("window").toObject().value("active_modal").toBool(),
                    QStringLiteral("peer %1 has active modal: %2").arg(i).arg(
                        state.value("window").toObject().value("active_modal_title").toString()));
            }
        }
        require(snapshot(peers.peer(3)).value("jam").toObject().value("peer_departure_count").toInt() == 0,
            "local leave announced remote departures");
        peers.markSuccessful();
        std::cout << "four-peer GUI: named departure, startup-only failures and remaining mesh passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
