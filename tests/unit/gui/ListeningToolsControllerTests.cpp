#include "ListeningToolsController.hpp"
#include "JamSyncDialog.hpp"
#include "ControlMessageValidation.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QTemporaryDir>
#include <QFile>
#include <QEventLoop>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <array>

namespace {
using namespace jam2::gui;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Harness {
    ListeningToolsController controller;
    ListeningToolsController::Context state;
    std::vector<QJsonObject> sent;
    std::vector<jam2::EngineCommand> commands;
    std::vector<int> renders;
    int applications = 0;
    bool failSubmit = false;
    Harness() {
        state.engineRunning = true;
        state.position.engineAnchored = true;
        state.position.running = true;
        state.position.sampleRate = 48000;
        state.position.currentFrame = state.position.rawCurrentFrame = state.engine.engine_frame = 96000;
        state.position.secondsPerBeat = 0.5;
        state.position.absoluteBeat = 4;
        state.engine.sample_rate = 48000;
        state.engine.metronome_pattern.bpm = 120;
        state.sourceSignature = QStringLiteral("unchanged");
        ListeningToolsController::Callbacks callbacks;
        callbacks.context = [this] { return state; };
        callbacks.send = [this](const QJsonObject& object, const QString&) { sent.push_back(object); return true; };
        callbacks.prepare = [this](const ListeningToolsSettings&, int generation, int) { renders.push_back(generation); return true; };
        callbacks.submit = [this](const jam2::EngineCommand& command) { commands.push_back(command); return !failSubmit; };
        callbacks.applied = [this](const ListeningToolsSettings&, const std::optional<PreparedMixResult>&) { ++applications; };
        controller.setCallbacks(std::move(callbacks));
        controller.initialize({});
    }
    void finish() {
        require(!commands.empty() && commands.back().type == jam2::EngineCommandType::CommitListeningTools, "change must commit through the engine");
        state.engine.engine_frame = commands.back().frame;
        if (commands.back().update_playback_tempo) state.engine.playback_tempo = commands.back().playback_tempo;
        state.position.tempo = state.engine.playback_tempo;
        state.position.currentFrame = state.engine.engine_frame;
        controller.tick();
    }
};
QJsonObject readiness(int revision) { return {{QStringLiteral("type"), QStringLiteral("jam.listening.ready")}, {QStringLiteral("revision"), revision}}; }
void testStableNumericEdit() {
    Harness h;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    h.controller.edit(settings);
    settings.speedPpm = 730000;
    h.controller.edit(settings);
    require(h.commands.empty(), "numeric edits do not submit while settling");
    QEventLoop settle;
    QTimer::singleShot(ListeningToolsController::kDebounceMs + 25, &settle, &QEventLoop::quit);
    settle.exec();
    require(h.commands.size() == 1 && h.commands.back().playback_tempo.rate_ppm == 730000,
        "the numeric debounce prepares only the latest stable settings");
    h.finish();
}
void testReadinessAndContinuity() {
    Harness h;
    h.state.network = true;
    h.state.peers = {QStringLiteral("p2"), QStringLiteral("p3"), QStringLiteral("p4")};
    ListeningToolsSettings settings;
    settings.speedPpm = 730000;
    settings.pitchCents = 300;
    h.controller.edit(settings, true);
    require(h.controller.pending() && h.commands.empty(), "creator waits for all three peers before a musical change");
    const int revision = h.sent.front().value(QStringLiteral("revision")).toInt();
    h.controller.receive(readiness(revision), QStringLiteral("outsider"));
    h.controller.receive(readiness(revision - 1), QStringLiteral("p2"));
    require(h.commands.empty(), "unknown peers and stale readiness cannot commit");
    h.controller.receive(readiness(revision), QStringLiteral("p2"));
    h.controller.receive(readiness(revision), QStringLiteral("p2"));
    h.controller.receive(readiness(revision), QStringLiteral("p3"));
    require(h.commands.empty(), "duplicate readiness does not replace another peer");
    h.controller.receive(readiness(revision), QStringLiteral("p4"));
    const auto command = h.commands.back();
    require(command.playback_tempo.rate_ppm == 730000 && command.frame > 96000 && h.applications == 0,
        "fractional tempo is scheduled and does not apply early");
    require(std::abs(command.playback_tempo.position(command.playback_tempo.anchor_frame) -
        static_cast<double>(command.playback_tempo.anchor_frame)) < 0.0001, "rate change preserves source time at its boundary");
    h.finish();
    require(h.applications == 1 && !h.controller.pending() && h.controller.appliedSettings() == settings,
        "completed boundary applies exact speed and independent pitch once");
    h.controller.receive(readiness(revision), QStringLiteral("p4"));
    require(h.applications == 1, "late duplicate readiness cannot reapply committed state");
}
void testLocalOnlyAndDisable() {
    Harness h;
    h.state.sync = false;
    h.state.network = true;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    settings.focusEnabled = true;
    h.controller.edit(settings, true);
    require(h.sent.empty() && !h.commands.back().update_playback_tempo && h.commands.back().playback_tempo.rate_ppm == 1000000,
        "local listening edits do not publish or change global metronome speed");
    h.finish();
    require(h.controller.appliedSettings().speedPpm == 500000, "local audio settings still apply");
    h.state.sync = true;
    h.controller.policyChanged(false);
    h.finish();
    require(h.state.engine.playback_tempo.rate_ppm == 500000, "re-enabling sync couples musical tempo to listening speed");
    h.state.sync = false;
    h.controller.policyChanged(true);
    require(h.commands.back().playback_tempo.rate_ppm == 1000000 && h.controller.appliedSettings().speedPpm == 500000,
        "disabling sync restores the shared clock while retaining local listening speed");
}
void testResetRetryJoinAndPendingClock() {
    Harness creator;
    creator.state.network = true;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    creator.controller.edit(settings, true); creator.finish();
    creator.state.sync = false;
    creator.failSubmit = true;
    creator.controller.policyChanged(true);
    require(creator.controller.stats().value(QStringLiteral("reset_pending")).toBool(),
        "a rejected clock reset remains pending instead of silently losing unity tempo");
    creator.failSubmit = false;
    creator.controller.tick();
    require(!creator.controller.stats().value(QStringLiteral("reset_pending")).toBool(),
        "clock reset retries successfully when the engine queue has room");
    const auto reset = creator.commands.back();
    creator.state.engine.listening_commit_raw_frame = reset.frame;
    creator.state.engine.pending_playback_tempo = reset.playback_tempo;
    creator.controller.sendCurrent(QStringLiteral("new-peer"));
    require(creator.sent.back().value(QStringLiteral("type")) == QStringLiteral("jam.listening.reset"),
        "a sync-off late join receives unity clock phase without shared listening settings");
    Harness joining;
    joining.state.sync = false; joining.state.creator = false; joining.state.network = true;
    joining.controller.receive(creator.sent.back(), QStringLiteral("creator"));
    require(joining.commands.back().playback_tempo.rate_ppm == 1000000 &&
        std::abs(joining.commands.back().playback_tempo.position(reset.playback_tempo.anchor_frame) -
            reset.playback_tempo.anchor_position) < 0.001 && joining.controller.appliedSettings().speedPpm == 1000000,
        "joining with sync disabled retains local settings and matches the reset clock phase");
    creator.state.network = false;
    creator.state.sync = true;
    settings.speedPpm = 730000;
    creator.controller.edit(settings, true);
    const auto commit = creator.commands.back();
    require(std::abs(commit.playback_tempo.anchor_position -
        reset.playback_tempo.position(commit.playback_tempo.anchor_frame)) < 0.001,
        "a new speed edit anchors to the pending unity reset rather than the old slowed clock");
}
void testPreparationErrorsAndStaleResults() {
    Harness h;
    h.state.hasSources = true;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    h.controller.edit(settings, true);
    require(h.renders.size() == 1 && h.commands.empty(), "backing audio is prepared without replacing the old mix");
    const int oldGeneration = h.renders.front();
    settings.speedPpm = 800000;
    h.controller.edit(settings, true);
    require(h.renders.size() == 2 && h.renders.back() != oldGeneration, "new edits use unique render ownership");
    QTemporaryDir folder;
    require(folder.isValid(), "temporary render directory must exist");
    const QString stalePath = folder.filePath(QStringLiteral("stale.wav"));
    QFile stale(stalePath);
    require(stale.open(QIODevice::WriteOnly), "stale render fixture must open");
    stale.write("temporary"); stale.close();
    PreparedMixResult staleResult;
    staleResult.path = stalePath;
    h.controller.prepared(oldGeneration, staleResult);
    require(!QFile::exists(stalePath) && h.controller.pending(), "superseded renders are cleaned without disturbing the current request");
    PreparedMixResult failed;
    failed.error = QStringLiteral("test render failure");
    h.controller.prepared(h.renders.back(), failed);
    require(!h.controller.pending() && h.applications == 0 && h.controller.appliedSettings().speedPpm == 1000000,
        "failed preparation retains previous settings and playback");
    h.controller.edit(settings, true);
    h.state.sourceSignature = QStringLiteral("changed");
    h.controller.tick();
    require(!h.controller.pending() && h.applications == 0, "a source edit cancels preparation instead of publishing obsolete audio");
}
void testStagedLoadAndRollback() {
    Harness h;
    h.state.hasSources = true;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    h.controller.edit(settings, true);
    QTemporaryDir folder;
    PreparedMixResult result;
    result.path = folder.filePath(QStringLiteral("prepared.wav"));
    QFile file(result.path); require(file.open(QIODevice::WriteOnly), "prepared fixture must open"); file.write("fixture"); file.close();
    result.frames = 48000; result.sampleRate = 48000;
    h.controller.prepared(h.renders.back(), result);
    require(h.commands.back().type == jam2::EngineCommandType::StageListeningTrack && h.applications == 0,
        "prepared audio is loaded into a staging slot before readiness");
    jam2::EngineEvent staged;
    staged.type = jam2::EngineEventType::ListeningTrackStaged;
    staged.cookie = h.commands.back().cookie;
    require(h.controller.engineEvent(staged), "staging event must correlate to its owned load");
    require(h.commands.back().type == jam2::EngineCommandType::CommitListeningTools &&
        h.commands.back().signed_value == static_cast<std::int64_t>(staged.cookie), "commit owns the staged slot");
    h.state.sourceSignature = QStringLiteral("changed before commit");
    h.controller.tick();
    require(!h.controller.pending() && !QFile::exists(result.path) &&
        h.commands.back().type == jam2::EngineCommandType::CancelListeningTools && h.applications == 0,
        "cancelled scheduled changes discard the staged file and retain the old playback state");
}
void testLoopPassageAcrossSpeedChange() {
    Harness h;
    h.state.hasSources = h.state.playing = true;
    h.state.sourceSeconds = 1.8;
    h.state.loopStartSourceSeconds = 1.0;
    h.state.loopEndSourceSeconds = 2.0;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    h.controller.edit(settings, true);
    QTemporaryDir folder;
    PreparedMixResult result;
    result.path = folder.filePath(QStringLiteral("loop.wav"));
    QFile file(result.path); require(file.open(QIODevice::WriteOnly), "loop fixture must open"); file.write("fixture"); file.close();
    result.frames = 480000; result.sampleRate = 48000;
    h.controller.prepared(h.renders.back(), result);
    jam2::EngineEvent event;
    event.type = jam2::EngineEventType::ListeningTrackStaged;
    event.cookie = h.commands.back().cookie;
    h.controller.engineEvent(event);
    const auto& commit = h.commands.back();
    require(commit.prepared_loop_enabled && commit.prepared_loop_start_frame == 96000 &&
        commit.prepared_loop_end_frame == 192000 && std::abs(static_cast<double>(commit.frame_end) - 172800.0) <= 1.0,
        "speed transition keeps the audible passage while rescaling custom loop bounds");
}
void testValidationAndDialog() {
    QString error;
    ListeningToolsSettings settings;
    auto object = listeningSettingsJson(settings);
    require(parseListeningSettings(object, settings, error), "settings round-trip through a bounded typed parser");
    object.insert(QStringLiteral("speed_ppm"), 99999);
    require(!parseListeningSettings(object, settings, error), "out-of-range listening speed is rejected");
    object = listeningSettingsJson(settings); object.insert(QStringLiteral("extra"), true);
    require(!parseListeningSettings(object, settings, error), "unknown settings fields are rejected");
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("jam.listening.request")}, {QStringLiteral("settings"), listeningSettingsJson({})}};
    require(validateListeningMessage(message, error), "peer proposal needs only listening settings");
    message.insert(QStringLiteral("revision"), 1);
    require(!validateListeningMessage(message, error), "peers cannot assign the authoritative revision");
    using jam2::application::ControlMessageSource;
    using jam2::application::evaluateControlMessage;
    message.remove(QStringLiteral("revision"));
    require(evaluateControlMessage(message, ControlMessageSource::AuthenticatedPeer).accepted &&
        !evaluateControlMessage(message, ControlMessageSource::Coordinator).accepted,
        "listening proposals are peer commands, not authoritative commits");
    message = {{QStringLiteral("type"), QStringLiteral("jam.listening.prepare")},
        {QStringLiteral("revision"), 2}, {QStringLiteral("bank"), 0},
        {QStringLiteral("settings"), listeningSettingsJson({})}};
    require(evaluateControlMessage(message, ControlMessageSource::Coordinator).accepted &&
        !evaluateControlMessage(message, ControlMessageSource::AuthenticatedPeer).accepted &&
        !evaluateControlMessage(message, ControlMessageSource::LocalJoiner).accepted,
        "only the coordinator can assign shared listening state");
    JamSyncDialog dialog({}, false, false);
    QCheckBox* listening = nullptr;
    QCheckBox* playback = nullptr;
    for (QCheckBox* check : dialog.findChildren<QCheckBox*>()) {
        if (check->text() == QStringLiteral("Listening Tools / Metronome Sync")) listening = check;
        if (check->text() == QStringLiteral("Sync Global Playback")) playback = check;
    }
    require(listening && playback && listening->isChecked() && listening->isEnabled() &&
        listening->toolTip().contains(QStringLiteral("global playhead")), "real Jam Sync control defaults on and explains local deviation");
    playback->click();
    require(!listening->isEnabled() && dialog.policy().listeningTools && !dialog.policy().globalPlayback,
        "Global Playback dependency disables the control while preserving its preference");
    playback->click(); listening->click();
    require(!dialog.policy().listeningTools, "real checkbox updates the typed Jam Sync policy");
}
void testFourPeersWithDelayedDuplicateMessages() {
    std::array<Harness, 4> peers;
    for (std::size_t i = 0; i < peers.size(); ++i) {
        peers[i].state.network = true;
        peers[i].state.creator = i == 0;
        peers[i].state.localToken = QStringLiteral("p%1").arg(i);
    }
    peers[0].state.peers = {QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3")};
    ListeningToolsSettings settings;
    settings.speedPpm = 730000;
    settings.focusEnabled = true;
    peers[0].controller.edit(settings, true);
    const auto prepare = peers[0].sent.back();
    for (std::size_t i = 1; i < peers.size(); ++i) peers[i].controller.receive(prepare, QStringLiteral("p0"));
    // Deliver out of order, duplicate one response, and withhold the last
    // peer's response. No peer can commit while that peer remains unready.
    peers[0].controller.receive(peers[2].sent.back(), QStringLiteral("p2"));
    peers[0].controller.receive(peers[1].sent.back(), QStringLiteral("p1"));
    peers[0].controller.receive(peers[1].sent.back(), QStringLiteral("p1"));
    for (auto& peer : peers) require(peer.commands.empty(), "four-peer commit waits through delayed/duplicate readiness");
    peers[0].controller.receive(peers[3].sent.back(), QStringLiteral("p3"));
    const auto commit = peers[0].sent.back();
    for (std::size_t i = 1; i < peers.size(); ++i) {
        peers[i].controller.receive(commit, QStringLiteral("p0"));
        peers[i].controller.receive(commit, QStringLiteral("p0"));
    }
    for (auto& peer : peers) {
        require(peer.commands.size() == 1 && peer.commands.back().playback_tempo.rate_ppm == 730000,
            "duplicate commits schedule one exact fractional tempo on every peer");
        peer.finish();
        require(peer.controller.appliedSettings() == settings, "four peers converge after impaired message ordering");
    }
    settings.speedPpm = 500000;
    peers[0].controller.edit(settings, true);
    const auto newer = peers[0].sent.back();
    peers[1].controller.receive(newer, QStringLiteral("p0"));
    const auto count = peers[1].commands.size();
    peers[1].controller.receive(commit, QStringLiteral("p0"));
    peers[1].controller.receive(prepare, QStringLiteral("p0"));
    require(peers[1].commands.size() == count && peers[1].controller.appliedSettings().speedPpm == 730000,
        "old prepare and commit cannot overwrite a newer four-peer revision");
}
void testQueueFailureAndPassedBoundary() {
    Harness failed;
    failed.failSubmit = true;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    failed.controller.edit(settings, true);
    require(!failed.controller.pending() && failed.applications == 0 &&
        failed.controller.appliedSettings().speedPpm == 1000000,
        "rejected engine commands leave previous listening settings applied");
    Harness rejected;
    rejected.controller.edit(settings, true);
    jam2::EngineEvent event;
    event.type = jam2::EngineEventType::CommandRejected;
    event.ok = false;
    event.cookie = rejected.commands.back().cookie;
    rejected.state.engine.engine_frame = rejected.commands.back().frame;
    require(rejected.controller.engineEvent(event) && rejected.applications == 0 && !rejected.controller.pending(),
        "clock-only commit rejection remains correlated even after its intended boundary");
    Harness committed;
    committed.state.network = true;
    committed.controller.edit(settings, true);
    committed.state.engine.engine_frame = committed.commands.back().frame;
    committed.state.engine.playback_tempo = committed.commands.back().playback_tempo;
    committed.state.position.tempo = committed.state.engine.playback_tempo;
    committed.controller.sendCurrent(QStringLiteral("new-peer"));
    require(committed.applications == 1 && !committed.controller.pending() &&
        committed.sent.back().value(QStringLiteral("settings")).toObject().value(QStringLiteral("speed_ppm")).toInt() == 500000,
        "joining after the boundary publishes committed state instead of cancelling active audio");
}
void testStoppedEngineAndDeferredJoin() {
    Harness stopped;
    stopped.state.engineRunning = false;
    stopped.state.hasSources = true;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    stopped.controller.edit(settings, true);
    require(stopped.renders.size() == 1 && stopped.applications == 0,
        "listening tools still prepare the waveform while audio is stopped");
    QTemporaryDir folder;
    PreparedMixResult result;
    result.path = folder.filePath(QStringLiteral("stopped.wav"));
    result.sampleRate = 48000; result.frames = 96000;
    stopped.controller.prepared(stopped.renders.back(), result);
    require(stopped.applications == 1 && stopped.commands.empty() && stopped.controller.appliedSettings() == settings,
        "stopped audio adopts the rendered settings without engine commands");
    Harness creator;
    creator.state.network = true;
    creator.controller.edit(settings, true); creator.finish();
    creator.controller.sendCurrent(QStringLiteral("p3"));
    Harness joining;
    joining.state.network = true; joining.state.creator = false; joining.state.contentReady = false;
    joining.controller.receive(creator.sent.back(), QStringLiteral("p0"));
    require(joining.commands.empty() && joining.applications == 0, "late join waits for the song and WAV content");
    joining.state.contentReady = true;
    joining.controller.tick(); joining.finish();
    require(joining.controller.appliedSettings() == settings,
        "late join applies authoritative listening tools after content is ready");
}
}
int main(int argc, char** argv) {
    try {
        QApplication application(argc, argv);
        testStableNumericEdit();
        testReadinessAndContinuity(); testLocalOnlyAndDisable(); testPreparationErrorsAndStaleResults();
        testResetRetryJoinAndPendingClock();
        testStagedLoadAndRollback(); testValidationAndDialog();
        testLoopPassageAcrossSpeedChange();
        testFourPeersWithDelayedDuplicateMessages(); testQueueFailureAndPassedBoundary();
        testStoppedEngineAndDeferredJoin();
        std::cout << "listening-tool controller tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
