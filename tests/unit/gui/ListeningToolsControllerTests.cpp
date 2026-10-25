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
    ListeningToolsSettings visible;
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
        callbacks.selected = [this](const ListeningToolsSettings& settings) { visible = settings; };
        callbacks.rejected = [this](const ListeningToolsSettings& settings, const QString&) { visible = settings; };
        controller.setCallbacks(std::move(callbacks));
        controller.initialize({});
    }
    void finish() {
        require(!commands.empty() && commands.back().type == jam2::EngineCommandType::CommitListeningTools, "change must commit through the engine");
        state.engine.engine_frame = commands.back().frame;
        if (commands.back().update_playback_tempo) state.engine.playback_tempo = commands.back().playback_tempo;
        if (commands.back().signed_value != 0) state.engine.listening_source_cookie = commands.back().cookie;
        if (commands.back().grid_epoch_source_position >= 0.0) {
            state.engine.metronome_epoch_source_position = commands.back().grid_epoch_source_position;
            state.position.epochSourcePosition = commands.back().grid_epoch_source_position;
        }
        state.position.tempo = state.engine.playback_tempo;
        state.position.rawCurrentFrame = state.engine.engine_frame;
        state.position.currentFrame = jam2::transport_musical_frame_from_raw(state.engine.engine_frame, state.position.renderOffsetFrames);
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
    QJsonObject introduction{{QStringLiteral("type"), QStringLiteral("session.udp.introduce")},
        {QStringLiteral("nonce"), QString(32, QLatin1Char('a'))},
        {QStringLiteral("peer"), QString(32, QLatin1Char('b'))}};
    require(evaluateControlMessage(introduction, ControlMessageSource::Coordinator).accepted &&
        !evaluateControlMessage(introduction, ControlMessageSource::AuthenticatedPeer).accepted,
        "mesh introductions retain creator-only authority");
    introduction[QStringLiteral("nonce")] = QStringLiteral("invalid");
    require(!evaluateControlMessage(introduction, ControlMessageSource::Coordinator).accepted,
        "malformed mesh introduction rejected");
    const QJsonObject repair{{"type", "session.udp.recover"}, {"peer", QString(32, 'b')}};
    require(evaluateControlMessage(repair, ControlMessageSource::AuthenticatedPeer).accepted &&
        !evaluateControlMessage(repair, ControlMessageSource::Coordinator).accepted,
        "repair requests retain peer-to-creator authority");
    const QJsonObject unavailable{{"type", "looper.asset.unavailable"},
        {"sha256", QString(64, 'a')}, {"reason", "asset file is unavailable"}};
    require(evaluateControlMessage(unavailable, ControlMessageSource::AuthenticatedPeer).accepted &&
        evaluateControlMessage(unavailable, ControlMessageSource::Coordinator).accepted,
        "both asset providers can report an unavailable hash");
    auto invalid = unavailable; invalid["sha256"] = "invalid";
    require(!evaluateControlMessage(invalid, ControlMessageSource::AuthenticatedPeer).accepted,
        "malformed asset failure was accepted");
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
        const std::array<std::int64_t, 4> offsets{0, 48000, -96000, 12000};
        peers[i].state.position.renderOffsetFrames = offsets[i];
        peers[i].state.engine.engine_frame = peers[i].state.position.rawCurrentFrame =
            jam2::transport_raw_frame_from_musical(96000, offsets[i]);
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
    const auto sharedBoundary = peers[0].commands.back().playback_tempo.anchor_frame;
    for (auto& peer : peers) {
        require(jam2::transport_musical_frame_from_raw(peer.commands.back().frame,
            peer.state.position.renderOffsetFrames) == sharedBoundary,
            "different local engine ages translate the same shared listening boundary exactly");
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
void testPlaybackChangesRetainListeningSelection() {
    ListeningToolsSettings settings;
    settings.speedPpm = 730000; settings.pitchCents = 200; settings.focusHz = 900;
    Harness transitioning;
    transitioning.state.transportChanging = true;
    transitioning.controller.edit(settings);
    transitioning.controller.flushEdits();
    require(transitioning.commands.empty() && !transitioning.controller.playbackReady() && transitioning.visible == settings,
        "a settled edit during a section transition is retained and makes Play wait");
    transitioning.state.transportChanging = false;
    transitioning.controller.tick(); transitioning.finish();
    require(transitioning.controller.playbackReady() && transitioning.controller.appliedSettings() == settings,
        "the retained edit is submitted when the section transition settles");

    Harness h;
    h.state.hasSources = h.state.playing = true;
    h.controller.edit(settings, true);
    h.state.playing = false;
    h.controller.tick();
    require(h.controller.pending() && h.visible == settings && h.renders.size() == 1,
        "Stop during rendering retains the selection and its existing render");
    settings.speedPpm = 610000;
    h.controller.edit(settings, true);
    require(h.visible == settings && h.renders.size() == 2,
        "superseding an unfinished edit never flashes the old applied settings into the controls");
    QTemporaryDir folder;
    PreparedMixResult result;
    result.path = folder.filePath(QStringLiteral("paused.wav"));
    QFile file(result.path); require(file.open(QIODevice::WriteOnly), "paused render fixture opens"); file.write("fixture"); file.close();
    result.frames = 96000; result.sampleRate = 48000;
    h.controller.prepared(h.renders.back(), result);
    jam2::EngineEvent staged;
    staged.type = jam2::EngineEventType::ListeningTrackStaged;
    staged.cookie = h.commands.back().cookie;
    h.controller.engineEvent(staged);
    h.state.transportChanging = true;
    h.controller.tick();
    require(h.controller.pending() && h.visible == settings && h.applications == 0,
        "Stop after scheduling does not cancel the pending audio/tempo commit");
    h.finish();
    require(h.applications == 1 && h.controller.appliedSettings() == settings && h.visible == settings &&
            h.controller.playbackReady() && !h.state.playing,
        "settings apply with the engine running and playback stopped, ready for the next Play");

    Harness missingAudio;
    missingAudio.state.hasSources = true;
    missingAudio.controller.edit(settings, true);
    result.path = folder.filePath(QStringLiteral("missing-swap.wav"));
    missingAudio.controller.prepared(missingAudio.renders.back(), result);
    staged.cookie = missingAudio.commands.back().cookie;
    missingAudio.controller.engineEvent(staged);
    const auto commit = missingAudio.commands.back();
    missingAudio.state.engine.playback_tempo = commit.playback_tempo;
    missingAudio.state.engine.engine_frame = commit.frame + 48000;
    missingAudio.controller.tick();
    require(missingAudio.applications == 0 && !missingAudio.controller.pending(),
        "tempo alone cannot report success when the corresponding audio swap did not apply");
}

void testListeningChangeDuringSilentSectionTail() {
    Harness h;
    h.state.hasSources = h.state.playing = true;
    h.state.sourceSeconds = 5.0;
    h.state.loopEndSourceSeconds = 12.0;
    ListeningToolsSettings settings;
    settings.speedPpm = 500000;
    h.controller.edit(settings, true);
    PreparedMixResult result;
    QTemporaryDir folder;
    result.path = folder.filePath(QStringLiteral("short-audio.wav"));
    result.sampleRate = 48000;
    result.frames = 96000; // One second of source audio rendered at half speed.
    h.controller.prepared(h.renders.back(), result);
    jam2::EngineEvent staged;
    staged.type = jam2::EngineEventType::ListeningTrackStaged;
    staged.cookie = h.commands.back().cookie;
    h.controller.engineEvent(staged);
    const auto commit = h.commands.back();
    require(commit.frame_end > static_cast<std::uint64_t>(result.frames) &&
        commit.prepared_loop_end_frame == 24 * 48000,
        "speed change in the silent tail preserves section position and musical loop length");
    h.finish();
}

void testJoinAfterTempoAndGridReset() {
    Harness creator;
    creator.state.network = true;
    creator.state.engine.playback_tempo = creator.state.position.tempo = {1000000, 14020140, 12652167.5};
    creator.state.position.currentFrame = creator.state.position.rawCurrentFrame = creator.state.engine.engine_frame = 19000000;
    creator.state.position.epochFrame = creator.state.engine.metronome_epoch_frame = 18000000;
    creator.state.position.epochSourcePosition = creator.state.engine.metronome_epoch_source_position = 16632027.5;
    creator.controller.sendCurrent(QStringLiteral("joining"));
    Harness joining;
    joining.state.network = true;
    joining.state.creator = false;
    joining.state.position.currentFrame = joining.state.position.rawCurrentFrame = joining.state.engine.engine_frame = 19000000;
    joining.state.position.epochFrame = joining.state.engine.metronome_epoch_frame = 18000000;
    joining.state.position.epochSourcePosition = 18000000; // Fresh peer has not adopted the slowed musical history.
    joining.controller.receive(creator.sent.back(), QStringLiteral("creator"));
    require(joining.commands.back().grid_epoch_frame == 18000000 &&
        joining.commands.back().grid_epoch_source_position == 16632027.5,
        "late join carries the actual source anchor rather than reconstructing old musical history");
    joining.finish();
    require(joining.controller.playbackReady() && joining.state.position.epochSourcePosition == 16632027.5,
        "joined musical clock and grid source anchor publish together");
    auto invalid = creator.sent.back();
    invalid.insert(QStringLiteral("epoch_position"), -5.0);
    QString error;
    require(!validateListeningMessage(invalid, error), "invalid joined grid anchor is rejected");
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
        testListeningChangeDuringSilentSectionTail();
        testJoinAfterTempoAndGridReset();
        testPlaybackChangesRetainListeningSelection();
        std::cout << "listening-tool controller tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
