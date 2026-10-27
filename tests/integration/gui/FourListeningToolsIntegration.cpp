#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"
#include "UdpCandidateNetwork.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QProcess>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
void require(bool ok, const QString& error) { if (!ok) throw std::runtime_error(error.toStdString()); }
struct ServerFixture {
    QProcess process;
    std::unique_ptr<ThreadedCandidateNetwork> network;
    QString endpoint;
    explicit ServerFixture(const QString& executable) {
        QUdpSocket reserve; require(reserve.bind(QHostAddress::LocalHost, 0), "reserve signaller port");
        const auto port = reserve.localPort(); reserve.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, {"--jam2-server", "--listen", QStringLiteral("127.0.0.1:%1").arg(port)});
        require(process.waitForStarted(5000), "start real Go signaller: " + process.errorString());
        network = std::make_unique<ThreadedCandidateNetwork>(port);
        endpoint = QStringLiteral("127.0.0.1:%1").arg(network->port());
    }
    ~ServerFixture() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); } }
};
QJsonObject command(AutomationProcess& peer, QJsonObject message, const char* expected = "command_applied") {
    QString error;
    require(peer.send(std::move(message), error), error);
    QJsonObject event;
    require(peer.readEvent(event, 60s, error), error);
    require(event.value(QStringLiteral("event")).toString() == QString::fromLatin1(expected),
        QStringLiteral("unexpected response: ") + QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
    return event;
}
void invoke(AutomationProcess& peer, const QString& control, const QString& operation, QJsonValue value = {}) {
    command(peer, {{QStringLiteral("type"), QStringLiteral("invoke")}, {QStringLiteral("id"), control},
        {QStringLiteral("control"), control}, {QStringLiteral("operation"), operation}, {QStringLiteral("value"), value}});
}
QJsonObject snapshot(AutomationProcess& peer) {
    return command(peer, {{QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("id"), QStringLiteral("state")}, {QStringLiteral("cursor"), 0}}, "snapshot");
}
QJsonObject markerSnapshot(AutomationProcess& peer) {
    return command(peer, {{QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("id"), QStringLiteral("markers")},
        {QStringLiteral("control"), QStringLiteral("looper.lane.0.select")}}, "snapshot");
}
QJsonObject listening(const QJsonObject& state) {
    return state.value(QStringLiteral("content")).toObject().value(QStringLiteral("listening_tools")).toObject();
}
void requireSummary(AutomationProcess& peer, const QString& speed, int pitch, int focusHz, bool expanded) {
    const auto state = command(peer, {{QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("id"), QStringLiteral("listening-summary")},
        {QStringLiteral("control"), QStringLiteral("looper.listening-tools")}}, "snapshot");
    const auto control = state.value(QStringLiteral("control")).toObject().value(QStringLiteral("state")).toObject();
    const QString text = control.value(QStringLiteral("text")).toString();
    require(state.value(QStringLiteral("control_found")).toBool() &&
        control.value(QStringLiteral("checkable")).toBool() && control.value(QStringLiteral("checked")).toBool() == expanded &&
        text.contains(QStringLiteral("Speed %1\u00d7").arg(speed)) &&
        text.contains(QStringLiteral("Pitch %1 st").arg(pitch)) &&
        text.endsWith(focusHz > 0 ? QStringLiteral("Focus %1 Hz").arg(focusHz) : QStringLiteral("Focus off")),
        QStringLiteral("real listening-tools summary disagrees with current settings: ") + text);
}
template<class Predicate>
std::array<QJsonObject, 4> waitAll(FourPeerCoordinator& peers, Predicate predicate, const QString& description) {
    std::array<QJsonObject, 4> states;
    const auto deadline = std::chrono::steady_clock::now() + jam2::test::deadmanTimeout(60s);
    do {
        bool ready = true;
        for (std::size_t i = 0; i < states.size(); ++i) {
            states[i] = snapshot(peers.peer(i));
            ready = predicate(i, states[i]) && ready;
        }
        if (ready) return states;
        QThread::msleep(50);
    } while (std::chrono::steady_clock::now() < deadline);
    for (const auto& state : states)
        std::cerr << QJsonDocument(state.value(QStringLiteral("content")).toObject()).toJson(QJsonDocument::Compact).constData() << '\n';
    throw std::runtime_error((QStringLiteral("timeout: ") + description).toStdString());
}
bool applied(const QJsonObject& state, int speed, int rate, int pitch, int hz, bool focus) {
    const auto tools = listening(state);
    const auto settings = tools.value(QStringLiteral("settings")).toObject();
    return tools.value(QStringLiteral("phase")).toInt(-1) == 0 &&
        tools.value(QStringLiteral("applied_speed_ppm")).toInt() == speed &&
        tools.value(QStringLiteral("musical_rate_ppm")).toInt() == rate &&
        settings.value(QStringLiteral("pitch_cents")).toInt() == pitch &&
        settings.value(QStringLiteral("focus_hz")).toInt() == hz &&
        settings.value(QStringLiteral("focus_enabled")).toBool() == focus;
}
void syncPolicy(AutomationProcess& peer, bool enabled) {
    invoke(peer, QStringLiteral("session.jam-sync"), QStringLiteral("click-async"));
    invoke(peer, QStringLiteral("session.jam-sync-dialog.listening-tools"), QStringLiteral("set-checked"), enabled);
    invoke(peer, QStringLiteral("session.jam-sync-dialog.apply"), QStringLiteral("click"));
}
QString fixture(const QString& directory) {
    // One second of deterministic mono PCM; the real renderer expands it to
    // the Section length before applying speed, pitch and focus processing.
    QByteArray bytes;
    const auto u16 = [&bytes](quint16 value) { bytes.append(char(value)); bytes.append(char(value >> 8)); };
    const auto u32 = [&bytes](quint32 value) { for (int shift = 0; shift < 32; shift += 8) bytes.append(char(value >> shift)); };
    bytes.append("RIFF", 4); u32(36 + 96000); bytes.append("WAVEfmt ", 8); u32(16);
    u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16); bytes.append("data", 4); u32(96000);
    for (int frame = 0; frame < 48000; ++frame) u16(quint16(qint16(9000 * std::sin(frame * 440.0 * 6.283185307179586 / 48000.0))));
    const QString path = directory + QStringLiteral("/listening.wav");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), QStringLiteral("fixture write failed"));
    return path;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2 || argc == 3, QStringLiteral("expected release/jam2 executable and optional Go signaller"));
        const bool useRendezvous = argc == 3;
        const auto server = useRendezvous ? std::make_unique<ServerFixture>(QString::fromLocal8Bit(argv[2])) : nullptr;
        QTemporaryDir directory;
        require(directory.isValid(), QStringLiteral("fixture directory unavailable"));
        const QString wav = fixture(directory.path());
        LoopbackPortReservations reservations;
        QString error;
        require(reservations.reserve(4, error), error);
        std::array<quint16, 4> ports;
        std::array<QStringList, 4> arguments;
        for (std::size_t i = 0; i < 4; ++i) {
            ports[i] = reservations.port(i);
            arguments[i] << QStringLiteral("debug") << QStringLiteral("gui-agent")
                << QStringLiteral("--instance-id") << QStringLiteral("listening-peer-%1").arg(i + 1);
        }
        FourPeerCoordinator peers;
        require(peers.launch(QString::fromLocal8Bit(argv[1]), arguments, error), error);
        for (std::size_t i = 0; i < 4; ++i) {
            QJsonObject hello;
            require(peers.peer(i).readEvent(hello, 60s, error) && hello.value(QStringLiteral("event")) == QStringLiteral("hello"), error);
        }
        reservations.release();
        QJsonObject create{{QStringLiteral("type"), QStringLiteral("jam.create")}, {QStringLiteral("name"), QStringLiteral("Alex")},
            {QStringLiteral("id"), QStringLiteral("create")}, {QStringLiteral("port"), ports[0]},
            {QStringLiteral("test_input"), QStringLiteral("tone-440")}};
        if (useRendezvous) { create.insert("strategy", "server"); create.insert("rendezvous", server->endpoint); }
        const auto created = command(peers.peer(0), create);
        if (useRendezvous) require(created.value("invite_url").toString().contains("strategy=server"), "creator did not select Server discovery");
        for (std::size_t i = 1; i < 4; ++i)
            command(peers.peer(i), {{QStringLiteral("type"), QStringLiteral("jam.join")}, {QStringLiteral("name"), QStringLiteral("Alex")},
                {QStringLiteral("id"), QStringLiteral("join")}, {QStringLiteral("port"), ports[i]},
                {QStringLiteral("invite_url"), created.value(QStringLiteral("invite_url"))},
                {QStringLiteral("test_input"), QStringLiteral("tone-440")}});
        waitAll(peers, [](std::size_t, const QJsonObject& state) {
            const auto jam = state.value(QStringLiteral("jam")).toObject();
            return jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
                jam.value(QStringLiteral("transport_grid_ready")).toBool() &&
                jam.value(QStringLiteral("policy")).toObject().value(QStringLiteral("listening_tools")).toBool();
        }, QStringLiteral("four-peer grid and default listening sync"));
        command(peers.peer(0), {{QStringLiteral("type"), QStringLiteral("looper.import")},
            {QStringLiteral("id"), QStringLiteral("import")}, {QStringLiteral("lane"), -1}, {QStringLiteral("source_path"), wav}});
        const auto original = waitAll(peers, [](std::size_t, const QJsonObject& state) {
            const auto content = state.value(QStringLiteral("content")).toObject();
            return content.value(QStringLiteral("pending_asset_count")).toInt() == 0 &&
                content.value(QStringLiteral("track_duration_ms")).toInt() > 0 &&
                content.value(QStringLiteral("prepared_mix_frames")).toInteger() > 0 &&
                !content.value(QStringLiteral("prepared_mix_worker_running")).toBool();
        }, QStringLiteral("real prepared WAV on four peers"));
        waitAll(peers, [](std::size_t index, const QJsonObject& state) {
            if (index == 0) return true;
            const auto tools = listening(state);
            const int authoritative = tools.value(QStringLiteral("shared_authoritative_revision")).toInt();
            return authoritative > 0 && tools.value(QStringLiteral("phase")).toInt(-1) == 0 &&
                tools.value(QStringLiteral("shared_applied_revision")).toInt() == authoritative &&
                !tools.value(QStringLiteral("shared_deferred_state")).toBool();
        }, QStringLiteral("initial Shared listening state survives startup song/WAV changes without another edit"));
        for (std::size_t i = 0; i < 4; ++i) requireSummary(peers.peer(i), QStringLiteral("1.00"), 0, 0, false);
        invoke(peers.peer(2), QStringLiteral("looper.listening-tools"), QStringLiteral("click"));
        requireSummary(peers.peer(2), QStringLiteral("1.00"), 0, 0, true);
        invoke(peers.peer(0), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        waitAll(peers, [](std::size_t, const QJsonObject& state) {
            return state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
        }, QStringLiteral("shared playback started"));
        {
            const auto receipts=waitAll(peers, [](std::size_t index,const QJsonObject& state) {
                const auto transport=state.value("jam").toObject().value("udp_multiplex").toObject();
                if (index!=0) return transport.value("native_intent_acks_sent").toInteger()>0;
                const auto links=transport.value("links").toArray();
                return links.size()==3 && std::all_of(links.begin(),links.end(),[](const QJsonValue& link) { return link.toObject().value("native_intent_acknowledged").toBool(); }) &&
                    transport.value("native_intent_repeats_suppressed").toInteger()>0;
            },QStringLiteral("runtime accepted and receipted shared playback on all four peers"));
            const auto transport=receipts[0].value("jam").toObject().value("udp_multiplex").toObject();
            std::cout << "runtime_transport_receipts=" << transport.value("native_intent_acks_received").toInteger()
                << " suppressed_repeats=" << transport.value("native_intent_repeats_suppressed").toInteger() << '\n';
        }
        // Several numeric edits settle together; only their stable final state
        // is prepared and committed through the real control/engine boundary.
        invoke(peers.peer(2), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 0.5);
        requireSummary(peers.peer(2), QStringLiteral("0.50"), 0, 0, true);
        invoke(peers.peer(2), QStringLiteral("looper.pitch.value"), QStringLiteral("set-value"), 3);
        invoke(peers.peer(2), QStringLiteral("looper.focus.value"), QStringLiteral("set-value"), 640);
        invoke(peers.peer(2), QStringLiteral("looper.focus.enabled"), QStringLiteral("set-checked"), true);
        auto slow = waitAll(peers, [](std::size_t, const QJsonObject& state) { return applied(state, 500000, 500000, 300, 640, true); },
            QStringLiteral("peer-originated speed, pitch and focus commit"));
        for (std::size_t i = 0; i < 4; ++i) requireSummary(peers.peer(i), QStringLiteral("0.50"), 3, 640, i == 2);
        waitAll(peers, [](std::size_t index, const QJsonObject& state) {
            if (index != 0) return true;
            const auto tools = listening(state);
            const auto reports = tools.value(QStringLiteral("shared_peer_applications")).toArray();
            if (reports.size() != 3) return false;
            for (const auto& report : reports)
                if (report.toObject().value(QStringLiteral("revision")).toInt() != tools.value(QStringLiteral("shared_authoritative_revision")).toInt()) return false;
            return true;
        }, QStringLiteral("all three receivers acknowledge actual application of the Shared listening state"));
        invoke(peers.peer(2), QStringLiteral("looper.listening-tools"), QStringLiteral("click"));
        requireSummary(peers.peer(2), QStringLiteral("0.50"), 3, 640, false);
        invoke(peers.peer(2), QStringLiteral("looper.listening-tools"), QStringLiteral("click"));
        requireSummary(peers.peer(2), QStringLiteral("0.50"), 3, 640, true);
        const QString anchor = listening(slow[0]).value(QStringLiteral("tempo_anchor_frame")).toString();
        for (std::size_t i = 0; i < 4; ++i) {
            require(listening(slow[i]).value(QStringLiteral("tempo_anchor_frame")).toString() == anchor,
                QStringLiteral("four peers must share the exact tempo boundary"));
            const auto duration = slow[i].value(QStringLiteral("content")).toObject().value(QStringLiteral("track_duration_ms")).toInt();
            const auto previous = original[i].value(QStringLiteral("content")).toObject().value(QStringLiteral("track_duration_ms")).toInt();
            require(std::abs(duration - previous * 2) <= 3, QStringLiteral("half-speed prepared WAV duration is incorrect"));
        }
        for (std::size_t i = 0; i < 4; ++i) {
            const auto state = markerSnapshot(peers.peer(i));
            const auto marker = state.value(QStringLiteral("control")).toObject().value(QStringLiteral("state")).toObject();
            const double expected = state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("prepared_source_frame")).toDouble() / 96.0;
            require(marker.contains(QStringLiteral("playhead_ms")) &&
                std::abs(marker.value(QStringLiteral("playhead_ms")).toDouble() - expected) < 100.0,
                QStringLiteral("audible lane marker must follow original source time on all four peers"));
        }
        const auto first = markerSnapshot(peers.peer(0));
        QThread::msleep(300);
        const auto second = markerSnapshot(peers.peer(0));
        const double frames = second.value(QStringLiteral("performance")).toObject().value(QStringLiteral("engine_frame")).toDouble() -
            first.value(QStringLiteral("performance")).toObject().value(QStringLiteral("engine_frame")).toDouble();
        const double musical = listening(second).value(QStringLiteral("musical_position")).toDouble() - listening(first).value(QStringLiteral("musical_position")).toDouble();
        require(frames > 0 && std::abs(musical / frames - 0.5) < 0.04, QStringLiteral("musical playhead must advance at half the raw audio clock rate"));
        const double markerDelta = second.value(QStringLiteral("control")).toObject().value(QStringLiteral("state")).toObject().value(QStringLiteral("grid_position_ms")).toDouble() -
            first.value(QStringLiteral("control")).toObject().value(QStringLiteral("state")).toObject().value(QStringLiteral("grid_position_ms")).toDouble();
        require(std::abs(markerDelta - frames / 96.0) < 80.0,
            QStringLiteral("real Track view global position marker must slow with the shared click"));
        syncPolicy(peers.peer(1), false);
        waitAll(peers, [](std::size_t, const QJsonObject& state) { return applied(state, 500000, 1000000, 300, 640, true); },
            QStringLiteral("disabled sync restores metronome rate and retains listening tools"));
        invoke(peers.peer(3), QStringLiteral("looper.speed.slider"), QStringLiteral("set-value"), 73);
        waitAll(peers, [](std::size_t i, const QJsonObject& state) { return applied(state, i == 3 ? 730000 : 500000, 1000000, 300, 640, true); },
            QStringLiteral("local-only speed isolation"));
        for (std::size_t i = 0; i < 4; ++i) requireSummary(peers.peer(i), i == 3 ? QStringLiteral("0.73") : QStringLiteral("0.50"), 3, 640, i == 2);
        invoke(peers.peer(3), QStringLiteral("looper.focus.enabled"), QStringLiteral("set-checked"), false);
        waitAll(peers, [](std::size_t i, const QJsonObject& state) {
            return applied(state, i == 3 ? 730000 : 500000, 1000000, 300, 640, i != 3);
        }, QStringLiteral("local focus toggle remains isolated"));
        requireSummary(peers.peer(3), QStringLiteral("0.73"), 3, 0, false);
        syncPolicy(peers.peer(0), true);
        waitAll(peers, [](std::size_t, const QJsonObject& state) { return applied(state, 500000, 500000, 300, 640, true); },
            QStringLiteral("reenable reconciles local edits"));
        invoke(peers.peer(1), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 0.73);
        waitAll(peers, [](std::size_t, const QJsonObject& state) { return applied(state, 730000, 730000, 300, 640, true); },
            QStringLiteral("fractional shared rate"));
        // Reproduce the real GUI race: settle another speed edit while Stop
        // cancels the ordinary transport, keeping all four engines running.
        invoke(peers.peer(1), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 0.61);
        invoke(peers.peer(0), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        invoke(peers.peer(1), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 0.67);
        waitAll(peers, [](std::size_t, const QJsonObject& state) {
            return applied(state, 670000, 670000, 300, 640, true) &&
                !state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
        }, QStringLiteral("speed selection applies across the mesh despite Stop"));

        // Play immediately after stopped edits. Playback must wait for the
        // shared prepared audio, rather than restart the old 0.67x mix.
        invoke(peers.peer(2), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 0.82);
        invoke(peers.peer(2), QStringLiteral("looper.pitch.value"), QStringLiteral("set-value"), 5);
        invoke(peers.peer(2), QStringLiteral("looper.focus.value"), QStringLiteral("set-value"), 900);
        invoke(peers.peer(2), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        const auto resumed = waitAll(peers, [](std::size_t, const QJsonObject& state) {
            const bool playing = state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
            if (playing) require(applied(state, 820000, 820000, 500, 900, true),
                QStringLiteral("Play started before the latest listening settings applied"));
            return playing && applied(state, 820000, 820000, 500, 900, true);
        }, QStringLiteral("stopped speed/pitch/focus edits persist into the next shared Play"));
        for (std::size_t i = 0; i < 4; ++i) requireSummary(peers.peer(i), QStringLiteral("0.82"), 5, 900, i == 2);
        for (const auto& state : resumed) {
            const auto jam=state.value(QStringLiteral("jam")).toObject();
            const auto performance=state.value(QStringLiteral("performance")).toObject();
            std::cout << "listening_resume grid_ready=" << jam.value("transport_grid_ready").toBool()
                << " mapping_error_frames=" << jam.value("grid_mapping_error_frames").toInteger()
                << " epoch_valid=" << performance.value("metronome_epoch_valid").toBool()
                << " engine_frame=" << performance.value("engine_frame").toInteger() << '\n';
            if (!jam.value("transport_grid_ready").toBool() || !performance.value("metronome_epoch_valid").toBool()) {
                QJsonArray before,after;
                for (const auto& previous : resumed) before.append(previous);
                for (std::size_t peer=0;peer<4;++peer) after.append(snapshot(peers.peer(peer)));
                const auto diagnostic=QJsonDocument(QJsonObject{{"at_resume",before},{"at_assertion",after}}).toJson(QJsonDocument::Compact);
                QFile file(qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT",QDir::tempPath())+"/listening-grid-failure.json");
                require(file.open(QIODevice::WriteOnly) && file.write(diagnostic)==diagnostic.size(),"write listening grid failure evidence");
            }
            require(state.value(QStringLiteral("jam")).toObject().value(QStringLiteral("transport_grid_ready")).toBool() &&
                state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("metronome_epoch_valid")).toBool(),
                QStringLiteral("listening edits and restart must retain a valid synchronized epoch/grid"));
        }
        for (const int bpm : {80, 60, 120, 80}) {
            const auto before = waitAll(peers, [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
            }, QStringLiteral("playback before live BPM edit"));
            command(peers.peer(0), {{QStringLiteral("type"), QStringLiteral("performance.metronome")},
                {QStringLiteral("id"), QStringLiteral("live-bpm")}, {QStringLiteral("enabled"), true}, {QStringLiteral("bpm"), bpm}});
            const auto after = waitAll(peers, [bpm](std::size_t, const QJsonObject& state) {
                const auto performance = state.value(QStringLiteral("performance")).toObject();
                require(performance.value(QStringLiteral("global_transport_playing")).toBool(), QStringLiteral("BPM edit stopped global playback"));
                return performance.value(QStringLiteral("metronome_bpm")).toInt() == bpm &&
                    state.value(QStringLiteral("jam")).toObject().value(QStringLiteral("transport_grid_ready")).toBool();
            }, QStringLiteral("live BPM converges on four peers"));
            for (std::size_t i = 0; i < 4; ++i) {
                const auto oldContent = before[i].value(QStringLiteral("content")).toObject();
                const auto newContent = after[i].value(QStringLiteral("content")).toObject();
                require(oldContent.value(QStringLiteral("prepared_mix_requests")) == newContent.value(QStringLiteral("prepared_mix_requests")) &&
                    oldContent.value(QStringLiteral("prepared_mix_frames")) == newContent.value(QStringLiteral("prepared_mix_frames")),
                    QStringLiteral("BPM edit unnecessarily rebuilt the WAV"));
                require(before[i].value(QStringLiteral("performance")).toObject().value(QStringLiteral("prepared_source_actual_start_frame")) ==
                    after[i].value(QStringLiteral("performance")).toObject().value(QStringLiteral("prepared_source_actual_start_frame")),
                    QStringLiteral("BPM edit restarted audio instead of retaining source position"));
            }
        }
        invoke(peers.peer(0), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        waitAll(peers, [](std::size_t, const QJsonObject& state) {
            return !state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
        }, QStringLiteral("all four peers stopped before pending-Play cancellation"));
        invoke(peers.peer(3), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 0.93);
        invoke(peers.peer(3), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        invoke(peers.peer(3), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        waitAll(peers, [](std::size_t, const QJsonObject& state) {
            return applied(state, 930000, 930000, 500, 900, true) &&
                !state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
        }, QStringLiteral("Stop cancels waiting Play while retaining the newest shared speed"));
        invoke(peers.peer(0), QStringLiteral("looper.speed.value"), QStringLiteral("set-value"), 1.0);
        waitAll(peers, [](std::size_t, const QJsonObject& state) { return applied(state, 1000000, 1000000, 500, 900, true); },
            QStringLiteral("restored normal speed retains accumulated musical phase"));
        for (const int bpm : {85, 90, 80}) {
            command(peers.peer(0), {{QStringLiteral("type"), QStringLiteral("performance.metronome")},
                {QStringLiteral("id"), QStringLiteral("stopped-bpm")}, {QStringLiteral("enabled"), true}, {QStringLiteral("bpm"), bpm}});
            waitAll(peers, [bpm](std::size_t, const QJsonObject& state) {
                const auto performance = state.value(QStringLiteral("performance")).toObject();
                require(!performance.value(QStringLiteral("global_transport_playing")).toBool(), QStringLiteral("stopped BPM edit started playback"));
                return performance.value(QStringLiteral("metronome_bpm")).toInt() == bpm &&
                    state.value(QStringLiteral("jam")).toObject().value(QStringLiteral("transport_grid_ready")).toBool();
            }, QStringLiteral("stopped BPM edit converges before Play"));
        }
        const auto stoppedBeforePlay = waitAll(peers, [](std::size_t, const QJsonObject&) { return true; }, QStringLiteral("pre-Play clocks"));
        invoke(peers.peer(0), QStringLiteral("performance.track-toggle"), QStringLiteral("click"));
        const auto finalPlayback = waitAll(peers, [](std::size_t, const QJsonObject& state) {
            return state.value(QStringLiteral("performance")).toObject().value(QStringLiteral("global_transport_playing")).toBool();
        }, QStringLiteral("Play after normal-speed restoration and repeated BPM resets"));
        for (std::size_t i = 0; i < 4; ++i) {
            const auto previous = stoppedBeforePlay[i].value(QStringLiteral("performance")).toObject();
            const auto performance = finalPlayback[i].value(QStringLiteral("performance")).toObject();
            const auto lead = performance.value(QStringLiteral("prepared_source_actual_start_frame")).toInteger() -
                previous.value(QStringLiteral("engine_frame")).toInteger();
            require(lead > 0 && lead < 2 * 48000,
                QStringLiteral("Play after BPM reset retained stale tempo delay: %1 frames").arg(lead));
        }
        for (std::size_t i = 0; i < 4; ++i) {
            command(peers.peer(i), {{QStringLiteral("type"), QStringLiteral("shutdown")}, {QStringLiteral("id"), QStringLiteral("shutdown")}});
            QJsonObject shutdown;
            require(peers.peer(i).readEvent(shutdown, 60s, error), error);
            int code = -1;
            require(peers.peer(i).waitForExit(60s, code, error) && code == 0, error);
        }
        peers.markSuccessful();
        std::cout << "four-peer listening tools integration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
