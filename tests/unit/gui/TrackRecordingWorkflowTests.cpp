#include "TrackRecordingWorkflow.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct FakeRuntime {
    jam2::EngineSnapshot snapshot;
    std::vector<jam2::EngineCommand> commands;
    int rejectAt = -1;
    bool throwOnSubmit = false;

    bool submit(const jam2::EngineCommand& command)
    {
        if (throwOnSubmit) throw std::runtime_error("injected submit exception");
        const int index = static_cast<int>(commands.size());
        commands.push_back(command);
        return index != rejectAt;
    }

    TrackRecordingWorkflow workflow()
    {
        return TrackRecordingWorkflow(
            [this](const jam2::EngineCommand& command) { return submit(command); },
            [this] { return snapshot; });
    }

    void clearCommands()
    {
        commands.clear();
        rejectAt = -1;
        throwOnSubmit = false;
    }
};

PlaybackGrid::Position runningPosition()
{
    PlaybackGrid::Position position;
    position.running = true;
    position.engineAnchored = true;
    position.absoluteBeat = 10;
    position.secondsPerBeat = 0.5;
    position.epochFrame = 0;
    position.rawCurrentFrame = 241000;
    position.currentFrame = 241000;
    position.renderOffsetFrames = 0;
    position.sampleRate = 48000;
    return position;
}

void setEventText(jam2::EngineEvent& event, const std::string& text)
{
    require(text.size() < event.text.size(), "event fixture text is too long");
    std::copy(text.cbegin(), text.cend(), event.text.begin());
    event.text[text.size()] = '\0';
}

void setEventId(jam2::EngineEvent& event, const std::string& id)
{
    require(id.size() < event.id.size(), "event fixture id is too long");
    event.id.fill('\0');
    std::copy(id.cbegin(), id.cend(), event.id.begin());
}

void requireTypes(
    const std::vector<jam2::EngineCommand>& commands,
    std::initializer_list<jam2::EngineCommandType> expected,
    const std::string& context)
{
    require(commands.size() == expected.size(), context + " command count");
    std::size_t index = 0;
    std::uint64_t previousCookie = 0;
    for (const jam2::EngineCommandType type : expected) {
        require(commands[index].type == type &&
                commands[index].cookie > previousCookie,
            context + " command type/cookie order");
        previousCookie = commands[index].cookie;
        ++index;
    }
}

void testPureTimingBoundaries()
{
    using Limits = std::numeric_limits<std::uint64_t>;
    require(jam2::gui::recording_count_in_start_beat(10, 1000, 12000, 48000) == 11 &&
            jam2::gui::recording_count_in_start_beat(10, 1000, 2000, 48000) == 12 &&
            jam2::gui::recording_count_in_start_beat(
                Limits::max(), 0, 0, 48000) == Limits::max(),
        "count-in beat must enforce lead time and saturate absolute-beat overflow");
    require(jam2::gui::recording_grid_ready_for_count_in(true) &&
            !jam2::gui::recording_grid_ready_for_count_in(false),
        "count-in readiness must exactly mirror epoch validity");
    require(jam2::gui::global_transport_elapsed_frames(true, true, 150, 100) == 50 &&
            jam2::gui::global_transport_elapsed_frames(false, true, 150, 100) == 0 &&
            jam2::gui::global_transport_elapsed_frames(true, false, 150, 100) == 0 &&
            jam2::gui::global_transport_elapsed_frames(true, true, 99, 100) == 0,
        "transport elapsed frames must require playing anchored forward time");

    PlaybackGrid::Position position = runningPosition();
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 264000 &&
            jam2::gui::synced_recording_countdown_beat(position, 4) == 11,
        "running grid must choose the next beat beyond the 200-ms safety lead");
    position.rawCurrentFrame = 260000;
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 288000 &&
            jam2::gui::synced_recording_countdown_beat(position, 4) == 12,
        "a too-near next beat must advance exactly one more beat");
    position.renderOffsetFrames = 1000;
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 287000,
        "safe grid frame must translate musical time through render offset");
    position = runningPosition();
    position.renderOffsetFrames = (std::numeric_limits<std::int64_t>::min)();
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) ==
            (std::uint64_t{1} << 63U) + 264000ULL,
        "minimum signed render offset must translate without signed overflow");
    position = runningPosition();
    position.running = false;
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 250600 &&
            jam2::gui::synced_recording_countdown_beat(position, 4) == 0,
        "stopped anchored grid must expose only a raw safety lead, not a synced beat");
    position.engineAnchored = false;
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 0,
        "unanchored grid must not invent a target");
    position.engineAnchored = true;
    position.running = true;
    position.secondsPerBeat = std::numeric_limits<double>::quiet_NaN();
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 0 &&
            jam2::gui::synced_recording_countdown_beat(position, 4) == 0,
        "nonfinite running-grid intervals must fail closed without numeric-domain errors");
    position = runningPosition();
    position.absoluteBeat = (std::numeric_limits<std::uint64_t>::max)();
    require(jam2::gui::next_safe_grid_beat_raw_frame(position) == 0 &&
            jam2::gui::synced_recording_countdown_beat(position, 4) == 0,
        "exhausted absolute-beat range must not invent another transport target");

    require(jam2::gui::prepared_attach_has_applied(100, 100, 100) &&
            !jam2::gui::prepared_attach_has_applied(0, 100, 100) &&
            !jam2::gui::prepared_attach_has_applied(100, 99, 100) &&
            !jam2::gui::prepared_attach_has_applied(100, 100, 101),
        "prepared attachment must require exact scheduled ownership after target");
    require(jam2::gui::resolve_active_sample_rate(44100, 48000.0, 96000) == 44100 &&
            jam2::gui::resolve_active_sample_rate(0, 47999.6, 96000) == 48000 &&
            jam2::gui::resolve_active_sample_rate(
                0, std::numeric_limits<double>::quiet_NaN(), 96000) == 96000 &&
            jam2::gui::resolve_active_sample_rate(0, 0.0, 0) == 48000,
        "sample-rate resolution must prefer session, finite engine, configured, then default");
    require(jam2::gui::sample_rate_matches_engine(48000, 48001.0) &&
            !jam2::gui::sample_rate_matches_engine(48000, 48001.1) &&
            !jam2::gui::sample_rate_matches_engine(
                48000, std::numeric_limits<double>::infinity()) &&
            !jam2::gui::sample_rate_matches_engine(0, 48000.0),
        "engine sample-rate matching must enforce finite positive one-Hz tolerance");
}

void testPreparedAndTransportCommands()
{
    FakeRuntime fake;
    fake.snapshot.sample_rate = 48000.0;
    TrackRecordingWorkflow workflow = fake.workflow();

    require(workflow.seekPrepared(321, 654),
        "prepared seek command must submit");
    requireTypes(fake.commands, {jam2::EngineCommandType::PreparedSeek}, "prepared seek");
    require(fake.commands[0].frame == 654 && fake.commands[0].frame_end == 321,
        "prepared seek must preserve engine target and source frame");
    fake.clearCommands();

    require(workflow.setPreparedLoop(true, 10, 20) &&
            workflow.setPreparedLoop(true, 30, 20) &&
            workflow.setPreparedLoop(false),
        "prepared loop enable/infinite/disable commands must submit");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::PreparedSetLoop,
        jam2::EngineCommandType::PreparedSetLoop,
        jam2::EngineCommandType::PreparedSetLoop}, "prepared loop");
    require(fake.commands[0].frame_end == 10 && fake.commands[0].signed_value == 20 &&
            fake.commands[1].frame_end == 30 &&
            fake.commands[1].signed_value ==
                (std::numeric_limits<std::int64_t>::max)() &&
            fake.commands[2].frame_end == 0 && fake.commands[2].signed_value == 0,
        "prepared loop commands must encode finite, open-ended, and disabled bounds");
    fake.clearCommands();

    PlaybackGrid::Position position = runningPosition();
    workflow.notePreparedAttachScheduled(777);
    require(workflow.preparedAttachPending() &&
            workflow.restartPrepared(position, true, 0, 4),
        "prepared restart must accept a safe anchored grid");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::PreparedSeek,
        jam2::EngineCommandType::PreparedPlay,
        jam2::EngineCommandType::ScheduleTransport}, "prepared restart");
    require(fake.commands[0].frame == 264000 && fake.commands[1].frame == 264000 &&
            fake.commands[2].transport_local_only &&
            fake.commands[2].transport_action ==
                jam2::EngineTransportAction::TrackRestart &&
            fake.commands[2].transport_target_frame == 264000 &&
            fake.commands[2].transport_musical_frame == 264000 &&
            fake.commands[2].transport_countdown_start_frame == 264000 &&
            workflow.globalTransportRequestedPlaying() &&
            workflow.globalTransportTimelineStartFrame() == 264000 &&
            !workflow.preparedAttachPending(),
        "prepared restart must align source and global transport to one target");
    fake.clearCommands();

    require(workflow.restartGlobalTransport(position, false, 1, 4),
        "counted global restart must submit");
    requireTypes(fake.commands, {jam2::EngineCommandType::ScheduleTransport},
        "counted global restart");
    require(fake.commands[0].transport_countdown_start_frame == 264000 &&
            fake.commands[0].transport_target_frame == 360000 &&
            fake.commands[0].transport_musical_frame == 360000,
        "one-bar restart must schedule four beats after countdown start");
    fake.clearCommands();

    PlaybackGrid::Position invalid;
    require(!workflow.restartPrepared(invalid) &&
            !workflow.restartGlobalTransport(invalid, false, 1, 4) &&
            fake.commands.empty(),
        "invalid grid must reject prepared/global restart without commands");

    require(!workflow.scheduleBankRestart(0, 10, true),
        "bank restart must reject a zero target");
    require(workflow.scheduleBankRestart(500, 600, false),
        "bank restart without prepared audio must submit stop and transport");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::PreparedStop,
        jam2::EngineCommandType::ScheduleTransport}, "empty-bank restart");
    require(fake.commands[1].transport_target_frame == 500 &&
            fake.commands[1].transport_musical_frame == 600,
        "empty-bank restart must retain distinct raw/musical targets");
    fake.clearCommands();

    require(workflow.scheduleBankRestart(700, 800, true),
        "prepared bank restart must submit seek/play/transport");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::PreparedSeek,
        jam2::EngineCommandType::PreparedPlay,
        jam2::EngineCommandType::ScheduleTransport}, "prepared bank restart");
    fake.clearCommands();

    require(workflow.stopPrepared(900, 1000, true),
        "prepared stop must cancel stale schedule then stop and publish transport");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::CancelTransport,
        jam2::EngineCommandType::PreparedStop,
        jam2::EngineCommandType::ScheduleTransport}, "prepared stop");
    require(fake.commands[2].transport_local_only &&
            fake.commands[2].transport_action == jam2::EngineTransportAction::TrackStop &&
            !workflow.globalTransportRequestedPlaying(),
        "prepared stop must publish exact local stop state");
    fake.clearCommands();

    fake.throwOnSubmit = true;
    require(!workflow.seekPrepared(1, 2) && fake.commands.empty(),
        "submitter exceptions must fail closed at the workflow boundary");
    TrackRecordingWorkflow unavailable({}, {});
    require(!unavailable.seekPrepared(1, 2),
        "missing command dependency must fail closed");
}

void testSnapshotsAndCountdown()
{
    FakeRuntime fake;
    TrackRecordingWorkflow workflow = fake.workflow();
    workflow.noteManualPreparedSeek(50, 100);
    workflow.notePreparedAttachScheduled(120);

    jam2::EngineSnapshot snapshot;
    snapshot.sample_rate = 1000.0;
    snapshot.engine_frame = 120;
    snapshot.prepared_source_frame = 50;
    snapshot.prepared_source_scheduled_start_frame = 120;
    snapshot.prepared_source_actual_start_frame = 110;
    snapshot.prepared_source_playing = true;
    snapshot.input_latency_frames = -5;
    snapshot.output_latency_frames = 7;
    snapshot.recording_source_latency_frames = 8;
    snapshot.recording_latency_compensation_frames = 9;
    snapshot.transport_revision = 1;
    snapshot.transport_pending = true;
    snapshot.transport_action = jam2::EngineTransportAction::TrackRestart;
    snapshot.transport_countdown_start_frame = 130;
    snapshot.transport_target_frame = 200;
    MetronomeTransportController::SnapshotUpdate update;
    update.recordingScheduleAdvanced = true;
    update.recordingCountdownStartFrame = 300;
    update.recordingStartFrame = 400;
    workflow.consumeSnapshot(snapshot, update);
    require(workflow.inputLatencyFrames() == 0 && workflow.outputLatencyFrames() == 7 &&
            workflow.sourceLatencyFrames() == 8 && workflow.appliedLatencyFrames() == 9 &&
            workflow.latencySampleRate() == 1000 &&
            workflow.preparedSampleRate() == 1000 &&
            workflow.preparedPlaying() && workflow.preparedActualStartFrame() == 110 &&
            !workflow.preparedAttachPending() &&
            workflow.globalTransportRequestedPlaying() &&
            workflow.globalTransportCountdownStartFrame() == 130 &&
            workflow.globalTransportTimelineStartFrame() == 200 &&
            workflow.recordingStartFrame() == 400,
        "snapshot must atomically update latency, prepared, transport, attach, and recording state");

    PlaybackGrid::Position pendingPosition = runningPosition();
    pendingPosition.rawCurrentFrame = 150;
    const TrackRecordingWorkflow::PreparedAttachPlan pendingAttach =
        workflow.preparedAttachPlan(pendingPosition, 75);
    require(pendingAttach.alignToTransport &&
            pendingAttach.targetFrame == 200 &&
            pendingAttach.sourceFrame == 0,
        "a late prepared mix must attach to a pending global play even without a lifecycle play-when-ready flag");
    require(!workflow.preparedAttachPlan(pendingPosition, 0).alignToTransport,
        "a prepared attach plan must reject an empty render");
    PlaybackGrid::Position position;
    position.engineAnchored = true;
    position.rawCurrentFrame = 170;
    position.sampleRate = 1000;
    require(workflow.currentAudiblePositionMs(position, 1000) == 100,
        "audible position must advance from the latest prepared source/engine anchor");

    snapshot.transport_pending = false;
    snapshot.transport_commit_count = 1;
    snapshot.transport_action = jam2::EngineTransportAction::TrackRestart;
    snapshot.transport_target_frame = 200;
    snapshot.engine_frame = 200;
    workflow.consumeSnapshot(snapshot, {});
    position.rawCurrentFrame = 750;
    require(workflow.globalTransportPlaying() &&
            workflow.globalTransportStartFrame() == 200 &&
            workflow.currentTransportPositionMs(position, 1) == 550,
        "committed transport must expose unclamped elapsed timeline milliseconds");
    PlaybackGrid::Position playingPosition = runningPosition();
    playingPosition.rawCurrentFrame = 241000;
    const TrackRecordingWorkflow::PreparedAttachPlan runningAttach =
        workflow.preparedAttachPlan(playingPosition, 100000);
    require(runningAttach.alignToTransport &&
            runningAttach.targetFrame == 264000 &&
            runningAttach.sourceFrame == 63800,
        "a replacement prepared mix must join running transport on a safe beat at the shared timeline offset");

    snapshot.transport_revision = 2;
    snapshot.transport_pending = true;
    snapshot.transport_action = jam2::EngineTransportAction::TrackStop;
    snapshot.transport_target_frame = 800;
    snapshot.engine_frame = 700;
    workflow.consumeSnapshot(snapshot, {});
    require(!workflow.globalTransportRequestedPlaying() &&
            workflow.globalTransportPlaying(),
        "pending stop must retain current playback while clearing requested play");
    snapshot.transport_pending = false;
    snapshot.engine_frame = 800;
    workflow.consumeSnapshot(snapshot, {});
    require(!workflow.globalTransportPlaying() && workflow.globalTransportStartFrame() == 0,
        "passed pending-stop target must clear global transport state");

    workflow.waitForCountIn(2, true);
    snapshot.metronome_epoch_valid = false;
    require(!workflow.takeReadyPendingCountIn(snapshot).has_value(),
        "pending count-in must wait for a valid metronome epoch");
    snapshot.metronome_epoch_valid = true;
    require(workflow.takeReadyPendingCountIn(snapshot) == std::optional<int>(2) &&
            !workflow.takeReadyPendingCountIn(snapshot).has_value(),
        "ready count-in must be consumed exactly once");

    workflow.clearSessionSchedule();
    require(!workflow.globalTransportPlaying() &&
            !workflow.globalTransportRequestedPlaying() &&
            workflow.recordingStartFrame() == 0 && !workflow.jamRecordingActive(),
        "session clear must reset recording, jam, attach, revision, and transport state");
}

void testInputTakeLifecycle()
{
    FakeRuntime fake;
    fake.snapshot.sample_rate = 48000.0;
    TrackRecordingWorkflow workflow = fake.workflow();
    PlaybackGrid::Position position = runningPosition();
    QString error;

    require(!workflow.startInputTake({}, true, 48000, 100, 50, std::nullopt,
                position, 4, false, false, false, error) &&
            error.contains(QStringLiteral("output WAV")) && fake.commands.empty(),
        "blank take output must fail before queuing an arm command");
    require(!workflow.startInputTake(QStringLiteral("take.wav"), true, 44100, 100,
                50, std::nullopt, position, 4, false, false, false, error) &&
            error.contains(QStringLiteral("active engine")) && fake.commands.empty(),
        "sample-rate mismatch must fail before queuing an arm command");
    require(!workflow.startInputTake(QStringLiteral("take.wav"), true, 48000,
                (std::numeric_limits<std::uint64_t>::max)() - 4, 8,
                std::nullopt, position, 4, false, false, false, error) &&
            error.contains(QStringLiteral("clock range")) && fake.commands.empty(),
        "overflowing nonquantized duration must fail before arming or starting");
    PlaybackGrid::Position invalidBeat = position;
    invalidBeat.secondsPerBeat = std::numeric_limits<double>::infinity();
    require(!workflow.startInputTake(QStringLiteral("take.wav"), true, 48000,
                100, 50, 1, invalidBeat, 4, false, false, false, error) &&
            error.contains(QStringLiteral("beat interval")) && fake.commands.empty(),
        "invalid quantized beat interval must fail before arming a recorder");

    workflow.armLane(2, 3, TrackRecordingWorkflow::CaptureMode::CurrentJam, true, true);
    require(workflow.laneArmed() && workflow.laneArmedAt(2, 3) &&
            workflow.captureMode() == TrackRecordingWorkflow::CaptureMode::CurrentJam &&
            workflow.includePreparedInTake() && workflow.includeMetronomeInTake(),
        "lane arm must retain exact bank/lane/capture options");
    const QString output = QStringLiteral("recorded/take.wav");
    require(workflow.startInputTake(output, true, 48000, 100, 50, std::nullopt,
                position, 4, true, true, false, error),
        "nonquantized current-jam take must submit");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::ArmTrackTake,
        jam2::EngineCommandType::StartTrackTake,
        jam2::EngineCommandType::StopTrackTake}, "nonquantized take");
    const std::int32_t expectedOptions =
        static_cast<std::int32_t>(jam2::audio::TrackTakeSource::CurrentJam) |
        jam2::audio::kTrackTakeIncludePrepared |
        jam2::audio::kTrackTakeIncludeMetronome;
    require(!std::string(fake.commands[0].id.data()).empty() &&
            std::string(fake.commands[0].text.data()) ==
                QDir::toNativeSeparators(output).toStdString() &&
            fake.commands[0].value == expectedOptions &&
            fake.commands[1].frame == 100 && fake.commands[2].frame == 150 &&
            workflow.inputTakeActive() && workflow.lastCapturePath() == output &&
            workflow.pendingTransientCapturePath() == output &&
            workflow.lastCaptureSampleRate() == 48000 &&
            workflow.recordingStartFrame() == 100,
        "take arm/start/stop must preserve id, path, source flags, frames, and owned state");
    const std::string firstTakeId(fake.commands[0].id.data());
    require(!workflow.startInputTake(output, true, 48000, 200, 0, std::nullopt,
                position, 4, false, false, false, error) &&
            error == QStringLiteral("an input take is already active"),
        "second active input take must be rejected");
    fake.clearCommands();
    require(workflow.stopInputTake(140),
        "active input take must accept an explicit early stop");
    requireTypes(fake.commands, {jam2::EngineCommandType::StopTrackTake},
        "explicit take stop");
    require(fake.commands[0].frame == 140, "explicit take stop must preserve target frame");

    jam2::EngineEvent unrelated;
    unrelated.type = jam2::EngineEventType::CommandApplied;
    require(!workflow.consumeTrackTakeEvent(unrelated).handled,
        "noncompletion engine event must not consume the active take");
    jam2::EngineEvent completed;
    completed.type = jam2::EngineEventType::TrackTakeCompleted;
    completed.ok = true;
    completed.value = 40;
    completed.sample_rate = 0;
    setEventId(completed, "stale-take-id");
    require(!workflow.consumeTrackTakeEvent(completed).handled &&
            workflow.inputTakeActive(),
        "completion from a stale take must not clear the active take");
    setEventId(completed, firstTakeId);
    const TrackRecordingWorkflow::TrackTakeCompletion result =
        workflow.consumeTrackTakeEvent(completed);
    require(result.handled && result.ok && !result.takeId.isEmpty() &&
            result.wavPath == output && result.frames == 40 &&
            result.sampleRate == 48000 && !workflow.inputTakeActive() &&
            workflow.pendingTransientCapturePath().isEmpty() &&
            workflow.lastCapturePath() == output &&
            workflow.recordingStartFrame() == 0 && !workflow.stopInputTake(200),
        "successful completion must finalize exact take ownership and clear schedule");

    fake.clearCommands();
    workflow.waitForCountIn(1, true);
    fake.snapshot.metronome_epoch_valid = true;
    const std::optional<int> bars = workflow.takeReadyPendingCountIn(fake.snapshot);
    require(bars == std::optional<int>(1), "quantized fixture count-in must become ready");
    require(workflow.startInputTake(QStringLiteral("quantized.wav"), false, 48000,
                0, 48000, bars, position, 4, false, false, true, error),
        "quantized input take must submit against the anchored grid");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::ArmTrackTake,
        jam2::EngineCommandType::PreparedSeek,
        jam2::EngineCommandType::PreparedPlay,
        jam2::EngineCommandType::StartTrackTake,
        jam2::EngineCommandType::StopTrackTake,
        jam2::EngineCommandType::ScheduleTransport}, "quantized take");
    require(fake.commands[3].frame == 360000 && fake.commands[4].frame == 408000 &&
            fake.commands[5].transport_local_only &&
            fake.commands[5].transport_action == jam2::EngineTransportAction::RecordStart &&
            fake.commands[5].transport_countdown_start_frame == 264000 &&
            fake.commands[5].transport_target_frame == 360000 &&
            workflow.recordingStartFrame() == 360000,
        "quantized take must align countdown, source reset, duration, and transport");
    const std::string quantizedTakeId(fake.commands[0].id.data());
    PlaybackGrid::Position countdownPosition = position;
    countdownPosition.rawCurrentFrame = 263999;
    require(workflow.countdown(countdownPosition).phase ==
            TrackRecordingWorkflow::CountdownPhase::WaitingForBeat,
        "countdown must wait before the shared countdown boundary");
    countdownPosition.rawCurrentFrame = 300000;
    const auto counting = workflow.countdown(countdownPosition);
    require(counting.phase == TrackRecordingWorkflow::CountdownPhase::Counting &&
            counting.remainingBeats == 3,
        "countdown must report ceiling beats until the recording target");
    countdownPosition.rawCurrentFrame = 360000;
    const auto recording = workflow.countdown(countdownPosition);
    const auto recordingAgain = workflow.countdown(countdownPosition);
    require(recording.phase == TrackRecordingWorkflow::CountdownPhase::Recording &&
            recording.stopMetronome && !recordingAgain.stopMetronome,
        "recording boundary must consume stop-metronome intent exactly once");
    completed.ok = false;
    completed.sample_rate = 44100;
    setEventId(completed, quantizedTakeId);
    setEventText(completed, "injected writer failure");
    const auto failed = workflow.consumeTrackTakeEvent(completed);
    require(failed.handled && !failed.ok && failed.sampleRate == 44100 &&
            failed.error == QStringLiteral("injected writer failure") &&
            workflow.pendingTransientCapturePath().isEmpty(),
        "failed nontransient completion must expose engine error and clear active state");

    fake.clearCommands();
    require(!workflow.startInputTakeAtSchedule({}, true, 48000, 10, 20, 20, 5,
                false, false, error) && fake.commands.empty(),
        "shared scheduled take must reject blank output before arming");
    require(!workflow.startInputTakeAtSchedule(QStringLiteral("shared.wav"), true,
                48000, 30, 20, 20, 5, false, false, error) && fake.commands.empty(),
        "shared scheduled take must reject inverted countdown/target before arming");
    require(workflow.startInputTakeAtSchedule(QStringLiteral("shared.wav"), true,
                48000, 10, 20, 25, 5, true, false, error),
        "valid shared scheduled take must submit exact local adoption");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::ArmTrackTake,
        jam2::EngineCommandType::PreparedSeek,
        jam2::EngineCommandType::PreparedPlay,
        jam2::EngineCommandType::StartTrackTake,
        jam2::EngineCommandType::StopTrackTake,
        jam2::EngineCommandType::ScheduleTransport}, "shared scheduled take");
    require(fake.commands.back().transport_local_only &&
            fake.commands.back().transport_countdown_start_frame == 10 &&
            fake.commands.back().transport_target_frame == 20 &&
            fake.commands.back().transport_musical_frame == 25,
        "shared take must adopt transport locally without republishing mesh intent");
    setEventId(completed, std::string(fake.commands[0].id.data()));
    (void)workflow.consumeTrackTakeEvent(completed);

    fake.clearCommands();
    require(workflow.startInputTake(QStringLiteral("rejected.wav"), true, 48000,
                100, 50, std::nullopt, position, 4, false, false, false, error),
        "rejection fixture take must submit");
    const std::uint64_t rejectedStartCookie = fake.commands[1].cookie;
    jam2::EngineEvent rejected;
    rejected.type = jam2::EngineEventType::CommandRejected;
    rejected.ok = false;
    rejected.cookie = rejectedStartCookie + 1000;
    setEventText(rejected, "unrelated rejection");
    require(!workflow.consumeTrackTakeEvent(rejected).handled &&
            workflow.inputTakeActive(),
        "unrelated command rejection must not cancel an active take");
    rejected.cookie = rejectedStartCookie;
    setEventText(rejected, "injected start rejection");
    const auto rejectedCompletion = workflow.consumeTrackTakeEvent(rejected);
    require(rejectedCompletion.handled && !rejectedCompletion.ok &&
            rejectedCompletion.error == QStringLiteral("injected start rejection") &&
            !workflow.inputTakeActive() &&
            workflow.pendingTransientCapturePath().isEmpty() &&
            fake.commands.back().type == jam2::EngineCommandType::CancelTrackTake,
        "owned command rejection must fail the take, release GUI state, and cancel the recorder");

    fake.clearCommands();
    require(!workflow.scheduleRecordingTransport(30, 20, 25) && fake.commands.empty(),
        "recording transport must reject inverted schedule");
    require(workflow.scheduleRecordingTransport(10, 20, 25, true),
        "recording transport must accept valid local schedule");
    requireTypes(fake.commands, {
        jam2::EngineCommandType::PreparedSeek,
        jam2::EngineCommandType::PreparedPlay,
        jam2::EngineCommandType::ScheduleTransport}, "recording transport");
}

void testInputTakeSubmissionFailures()
{
    const PlaybackGrid::Position position = runningPosition();
    for (int rejectedIndex = 0; rejectedIndex < 6; ++rejectedIndex) {
        FakeRuntime fake;
        fake.snapshot.sample_rate = 48000.0;
        fake.rejectAt = rejectedIndex;
        TrackRecordingWorkflow workflow = fake.workflow();
        QString error;
        require(!workflow.startInputTake(
                    QStringLiteral("submission-failure.wav"),
                    true,
                    48000,
                    0,
                    48000,
                    1,
                    position,
                    4,
                    true,
                    true,
                    false,
                    error) &&
                !workflow.inputTakeActive() && !error.isEmpty(),
            "every quantized command submission failure must fail closed");
        require(fake.commands.size() == static_cast<std::size_t>(
                    rejectedIndex == 0 ? 1 : rejectedIndex + 2),
            "failed take must submit only the attempted prefix and optional cancel");
        if (rejectedIndex > 0) {
            require(fake.commands.back().type ==
                    jam2::EngineCommandType::CancelTrackTake &&
                    workflow.abandonPendingCapture() ==
                        QStringLiteral("submission-failure.wav"),
                "partial take submission must end with best-effort cancellation");
        } else {
            require(workflow.abandonPendingCapture().isEmpty(),
                "rejected arm must not claim transient output ownership");
        }
    }

    FakeRuntime fake;
    fake.snapshot.sample_rate = 48000.0;
    TrackRecordingWorkflow workflow = fake.workflow();
    QString error;
    const QString oversizedPath(
        static_cast<qsizetype>(jam2::kEngineCommandTextBytes), QLatin1Char('x'));
    require(!workflow.startInputTake(
                oversizedPath,
                true,
                48000,
                100,
                50,
                std::nullopt,
                position,
                4,
                false,
                false,
                false,
                error) && fake.commands.empty() && !workflow.inputTakeActive(),
        "oversized output path must fail before a command enters the engine queue");
}

void testCaptureLaneAndJamState()
{
    FakeRuntime fake;
    TrackRecordingWorkflow workflow = fake.workflow();
    workflow.beginLoopbackCapture(QStringLiteral("loop.wav"), true, 44100);
    require(workflow.lastCapturePath() == QStringLiteral("loop.wav") &&
            workflow.pendingTransientCapturePath() == QStringLiteral("loop.wav") &&
            workflow.lastCaptureSampleRate() == 44100,
        "loopback begin must own path, transient status, and sample rate");
    require(workflow.finishLoopbackCapture(QStringLiteral("finished.wav")) ==
                QStringLiteral("loop.wav") &&
            workflow.lastCapturePath() == QStringLiteral("finished.wav") &&
            workflow.pendingTransientCapturePath().isEmpty() &&
            workflow.abandonPendingCapture().isEmpty(),
        "loopback finish must return and consume the original transient path once");
    workflow.beginLoopbackCapture(QStringLiteral("failed.wav"), true, 48000);
    require(workflow.failLoopbackCapture() == QStringLiteral("failed.wav") &&
            workflow.lastCapturePath().isEmpty() &&
            workflow.lastCaptureSampleRate() == 0 &&
            workflow.pendingTransientCapturePath().isEmpty(),
        "loopback failure must return transient cleanup ownership and expose no completed capture");
    workflow.beginLoopbackCapture(QStringLiteral("persistent.wav"), false, 48000);
    require(workflow.abandonPendingCapture().isEmpty(),
        "nontransient loopback capture must have no cleanup ownership");

    workflow.armLane(1, 4, TrackRecordingWorkflow::CaptureMode::Loopback, true, false);
    require(workflow.laneArmedAt(1, 4) && workflow.laneArmed() &&
            workflow.armedBank() == 1 && workflow.armedLane() == 4 &&
            workflow.captureMode() == TrackRecordingWorkflow::CaptureMode::Loopback,
        "lane state must expose exact armed location and mode");
    workflow.disarmLane();
    require(!workflow.laneArmed() && !workflow.laneArmedAt(1, 4) &&
            workflow.captureMode() == TrackRecordingWorkflow::CaptureMode::Input &&
            !workflow.includePreparedInTake() && !workflow.includeMetronomeInTake(),
        "lane disarm must restore complete input defaults");

    require(!workflow.startJamRecording(QStringLiteral("   ")) && fake.commands.empty() &&
            !workflow.stopJamRecording(),
        "jam recording must reject blank start and inactive stop without commands");
    require(workflow.startJamRecording(QStringLiteral("jam-capture")) &&
            workflow.jamRecordingActive() &&
            workflow.jamRecordingFolder() == QStringLiteral("jam-capture") &&
            !workflow.startJamRecording(QStringLiteral("second")) &&
            !workflow.stopJamRecording(),
        "jam recording start must own one folder and serialize confirmation");
    requireTypes(fake.commands, {jam2::EngineCommandType::StartJamRecording},
        "jam recording start");
    const std::uint64_t rejectedStartCookie = fake.commands[0].cookie;
    jam2::EngineEvent event;
    event.type = jam2::EngineEventType::CommandRejected;
    event.ok = false;
    event.cookie = rejectedStartCookie + 1000;
    require(!workflow.consumeJamRecordingEvent(event) &&
            workflow.jamRecordingActive(),
        "unowned command rejection must not alter jam recording state");
    event.cookie = rejectedStartCookie;
    require(workflow.consumeJamRecordingEvent(event) &&
            !workflow.jamRecordingActive() && workflow.jamRecordingFolder().isEmpty(),
        "rejected jam recording start must roll requested state back to inactive");

    fake.clearCommands();
    require(workflow.startJamRecording(QStringLiteral("jam-capture")),
        "jam recording must be retryable after asynchronous start rejection");
    const std::uint64_t confirmedStartCookie = fake.commands[0].cookie;
    event.type = jam2::EngineEventType::JamRecordingStarted;
    event.ok = true;
    event.cookie = confirmedStartCookie;
    require(workflow.consumeJamRecordingEvent(event) && workflow.jamRecordingActive(),
        "started event must confirm active jam recording");
    fake.clearCommands();
    require(workflow.stopJamRecording() && !workflow.jamRecordingActive(),
        "active jam recording stop must submit and clear requested state");
    require(!workflow.startJamRecording(QStringLiteral("too-early")),
        "jam recording restart must wait for stop confirmation");
    requireTypes(fake.commands, {jam2::EngineCommandType::StopJamRecording},
        "jam recording stop");
    const std::uint64_t rejectedStopCookie = fake.commands[0].cookie;
    event.type = jam2::EngineEventType::CommandRejected;
    event.ok = false;
    event.cookie = rejectedStopCookie;
    require(workflow.consumeJamRecordingEvent(event) && workflow.jamRecordingActive(),
        "rejected jam recording stop must restore confirmed active state");
    fake.clearCommands();
    require(workflow.stopJamRecording(),
        "jam recording stop must be retryable after asynchronous rejection");
    const std::uint64_t confirmedStopCookie = fake.commands[0].cookie;
    event.type = jam2::EngineEventType::JamRecordingStopped;
    event.ok = true;
    event.cookie = confirmedStopCookie;
    require(workflow.consumeJamRecordingEvent(event) && !workflow.jamRecordingActive(),
        "stopped event must confirm inactive jam recording");
    event.type = jam2::EngineEventType::CommandApplied;
    require(!workflow.consumeJamRecordingEvent(event),
        "unrelated event must not mutate jam recording state");

    workflow.setLastCapturePath(QStringLiteral("manual.wav"));
    workflow.notePreparedAttachScheduled(123);
    workflow.armLane(0, 0, TrackRecordingWorkflow::CaptureMode::Input);
    workflow.clearProjectCapture();
    require(workflow.lastCapturePath().isEmpty() && workflow.lastCaptureSampleRate() == 0 &&
            workflow.pendingTransientCapturePath().isEmpty() &&
            !workflow.preparedAttachPending() && !workflow.laneArmed(),
        "project capture clear must release path, rate, transient, attach, and lane state");
    workflow.setLastCapturePath(QStringLiteral("temporary.wav"));
    workflow.clearLastCapturePath();
    require(workflow.lastCapturePath().isEmpty(),
        "explicit last-capture clear must release only its path");
}

void testEngineTakeCompletionCorrelation()
{
    const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    require(!artifactRoot.isEmpty(),
        "CTest omitted the build-local artifact root");
    QTemporaryDir root(QDir(artifactRoot).absoluteFilePath(
        QStringLiteral("track-workflow-engine-XXXXXX")));
    require(root.isValid(), "engine take fixture directory must be created");
    const QString outputPath = root.filePath(QStringLiteral("correlated-take.wav"));
    constexpr std::string_view takeId = "engine-correlation-take";

    jam2::Engine engine;
    jam2::EngineConfig config;
    config.backend = jam2::EngineAudioBackend::Headless;
    config.sample_rate = 48000;
    config.audio_buffer_frames = 64;
    config.test_input = jam2::EngineTestInput::Tone440;
    engine.start(config);

    const std::uint64_t readyDeadline = jam2::monotonic_us() + 30000000ULL;
    while (engine.snapshot().engine_frame < 512 &&
           jam2::monotonic_us() < readyDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t target = engine.snapshot().engine_frame + 512ULL;
    jam2::EngineCommand arm;
    arm.type = jam2::EngineCommandType::ArmTrackTake;
    arm.cookie = 101;
    require(jam2::engine_command_set_id(arm, takeId) &&
            jam2::engine_command_set_text(arm, outputPath.toStdString()) &&
            engine.submit(arm),
        "headless engine must accept correlated take arm");
    jam2::EngineCommand start;
    start.type = jam2::EngineCommandType::StartTrackTake;
    start.cookie = 102;
    start.frame = target;
    require(engine.submit(start), "headless engine must accept take start");
    jam2::EngineCommand stop;
    stop.type = jam2::EngineCommandType::StopTrackTake;
    stop.cookie = 103;
    stop.frame = target + 1024ULL;
    require(engine.submit(stop), "headless engine must accept take stop");

    jam2::EngineEvent completion;
    bool found = false;
    const std::uint64_t completionDeadline = jam2::monotonic_us() + 30000000ULL;
    while (!found && jam2::monotonic_us() < completionDeadline) {
        jam2::EngineEvent event;
        while (engine.pollEvent(event)) {
            if (event.type == jam2::EngineEventType::TrackTakeCompleted) {
                completion = event;
                found = true;
                break;
            }
        }
        if (!found) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    engine.requestStop();
    engine.join();
    require(found && completion.ok &&
            jam2::engine_event_id(completion) == takeId &&
            completion.sample_rate == 48000 && completion.value > 0 &&
            QFileInfo::exists(outputPath),
        "engine completion must preserve take id, rate, frames, and build-local WAV");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        testPureTimingBoundaries();
        testPreparedAndTransportCommands();
        testSnapshotsAndCountdown();
        testInputTakeLifecycle();
        testInputTakeSubmissionFailures();
        testCaptureLaneAndJamState();
        testEngineTakeCompletionCorrelation();
        std::cout << "Track recording workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Track recording workflow test failed: " << error.what() << '\n';
        return 1;
    }
}
