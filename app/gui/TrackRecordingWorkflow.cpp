#include "TrackRecordingWorkflow.hpp"

#include <QDir>
#include <QUuid>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

std::uint64_t rawFrameFromMusicalFrame(
    std::uint64_t musicalFrame,
    std::int64_t renderOffsetFrames) noexcept
{
    if (renderOffsetFrames >= 0) {
        const auto offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return musicalFrame > offset ? musicalFrame - offset : 0ULL;
    }
    const auto offset = static_cast<std::uint64_t>(-renderOffsetFrames);
    return musicalFrame > (std::numeric_limits<std::uint64_t>::max)() - offset
        ? (std::numeric_limits<std::uint64_t>::max)()
        : musicalFrame + offset;
}

std::uint64_t musicalFrameFromRawFrame(
    std::uint64_t rawFrame,
    std::int64_t renderOffsetFrames) noexcept
{
    if (renderOffsetFrames >= 0) {
        const auto offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return rawFrame > (std::numeric_limits<std::uint64_t>::max)() - offset
            ? (std::numeric_limits<std::uint64_t>::max)()
            : rawFrame + offset;
    }
    const auto offset = static_cast<std::uint64_t>(-renderOffsetFrames);
    return rawFrame > offset ? rawFrame - offset : 0ULL;
}

int remainingCountInBeats(
    std::uint64_t currentFrame,
    std::uint64_t startFrame,
    std::uint64_t beatFrames) noexcept
{
    if (currentFrame >= startFrame || beatFrames == 0) {
        return 0;
    }
    const std::uint64_t remaining = startFrame - currentFrame;
    return static_cast<int>(std::clamp<std::uint64_t>(
        (remaining + beatFrames - 1ULL) / beatFrames,
        1ULL,
        static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
}

}

std::uint64_t jam2::gui::recording_count_in_bar_beat(
    std::uint64_t absoluteBeat,
    int beatsPerBar,
    std::uint64_t rawCurrentFrame,
    std::uint64_t nextBarRawFrame,
    int sampleRate) noexcept
{
    const std::uint64_t beats = static_cast<std::uint64_t>(qMax(1, beatsPerBar));
    if (absoluteBeat > (std::numeric_limits<std::uint64_t>::max)() - beats) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    std::uint64_t nextBarBeat = (absoluteBeat / beats + 1ULL) * beats;
    const std::uint64_t minimumLeadFrames =
        static_cast<std::uint64_t>(qMax(1, sampleRate) / 5);
    const std::uint64_t minimumCountdownRawFrame =
        rawCurrentFrame > (std::numeric_limits<std::uint64_t>::max)() - minimumLeadFrames
        ? (std::numeric_limits<std::uint64_t>::max)()
        : rawCurrentFrame + minimumLeadFrames;
    if (nextBarRawFrame < minimumCountdownRawFrame) {
        return nextBarBeat > (std::numeric_limits<std::uint64_t>::max)() - beats
            ? (std::numeric_limits<std::uint64_t>::max)()
            : nextBarBeat + beats;
    }
    return nextBarBeat;
}

bool jam2::gui::recording_grid_ready_for_count_in(
    bool metronomeEnabled,
    bool epochValid,
    std::uint64_t epochFrame,
    bool requireFreshEpoch,
    std::uint64_t priorEpochFrame) noexcept
{
    return metronomeEnabled && epochValid &&
        (!requireFreshEpoch || epochFrame != priorEpochFrame);
}

TrackRecordingWorkflow::TrackRecordingWorkflow(ApplicationRuntime& runtime) noexcept
    : runtime_(runtime)
{
}

bool TrackRecordingWorkflow::submit(jam2::EngineCommand command) noexcept
{
    command.cookie = ++command_cookie_;
    return runtime_.submit(command);
}

bool TrackRecordingWorkflow::seekPrepared(
    std::uint64_t sourceFrame,
    std::uint64_t targetFrame) noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::PreparedSeek;
    command.frame = targetFrame;
    command.frame_end = sourceFrame;
    return submit(command);
}

bool TrackRecordingWorkflow::setPreparedLoop(
    bool enabled,
    std::uint64_t startFrame,
    std::uint64_t endFrame) noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::PreparedSetLoop;
    command.frame_end = enabled ? startFrame : 0ULL;
    command.signed_value = enabled && endFrame > startFrame
        ? static_cast<std::int64_t>(endFrame)
        : (enabled ? (std::numeric_limits<std::int64_t>::max)() : 0);
    return submit(command);
}

bool TrackRecordingWorkflow::restartPrepared(
    const PlaybackGrid::Position& position,
    int beatsPerBar,
    bool publishTransport) noexcept
{
    std::uint64_t target = 0;
    const auto quantizedTarget = [&position, beatsPerBar](int extraBars) {
        if (!position.engineAnchored || position.sampleRate <= 0) {
            return 0ULL;
        }
        const auto beats = static_cast<std::uint64_t>(qMax(1, beatsPerBar));
        const std::uint64_t nextBarBeat =
            ((position.absoluteBeat / beats) + 1ULL + static_cast<std::uint64_t>(qMax(0, extraBars))) * beats;
        const auto musical = position.epochFrame + static_cast<std::uint64_t>(std::llround(
            static_cast<double>(nextBarBeat) * position.secondsPerBeat * position.sampleRate));
        return rawFrameFromMusicalFrame(musical, position.renderOffsetFrames);
    };
    if (position.running) {
        target = quantizedTarget(0);
    }
    const std::uint64_t minimumLead = position.engineAnchored && position.sampleRate > 0
        ? position.rawCurrentFrame + static_cast<std::uint64_t>(position.sampleRate / 5)
        : 0;
    if (position.running && target > 0 && target < minimumLead) {
        target = quantizedTarget(1);
    }
    if (target == 0 && position.engineAnchored && position.sampleRate > 0) {
        target = minimumLead;
    }
    if (target == 0) {
        return false;
    }
    jam2::EngineCommand seek;
    seek.type = jam2::EngineCommandType::PreparedSeek;
    seek.frame = target;
    if (!submit(seek)) {
        return false;
    }
    jam2::EngineCommand play;
    play.type = jam2::EngineCommandType::PreparedPlay;
    play.frame = target;
    if (!submit(play)) {
        return false;
    }
    if (publishTransport) {
        jam2::EngineCommand transport;
        transport.type = jam2::EngineCommandType::ScheduleTransport;
        transport.transport_action = jam2::EngineTransportAction::TrackRestart;
        transport.transport_target_frame = target;
        transport.transport_musical_frame = musicalFrameFromRawFrame(
            target, position.renderOffsetFrames);
        transport.transport_countdown_start_frame = target;
        return submit(transport);
    }
    return true;
}

bool TrackRecordingWorkflow::stopPrepared(
    std::uint64_t targetFrame,
    std::uint64_t musicalFrame,
    bool publishTransport) noexcept
{
    jam2::EngineCommand stop;
    stop.type = jam2::EngineCommandType::PreparedStop;
    stop.frame = targetFrame;
    if (!submit(stop)) {
        return false;
    }
    if (publishTransport) {
        jam2::EngineCommand transport;
        transport.type = jam2::EngineCommandType::ScheduleTransport;
        transport.transport_action = jam2::EngineTransportAction::TrackStop;
        transport.transport_target_frame = targetFrame;
        transport.transport_musical_frame = musicalFrame;
        transport.transport_countdown_start_frame = targetFrame;
        return submit(transport);
    }
    return true;
}

void TrackRecordingWorkflow::noteManualPreparedSeek(
    qint64 sourceFrame,
    qint64 engineFrame) noexcept
{
    prepared_source_frame_ = sourceFrame;
    prepared_engine_frame_ = engineFrame;
}

qint64 TrackRecordingWorkflow::currentAudiblePositionMs(
    const PlaybackGrid::Position& enginePosition,
    qint64 durationMs) const noexcept
{
    if (prepared_sample_rate_ <= 0) {
        return 0;
    }
    qint64 sourceFrame = prepared_source_frame_;
    if (prepared_playing_ && enginePosition.engineAnchored &&
        static_cast<qint64>(enginePosition.rawCurrentFrame) >= prepared_engine_frame_) {
        sourceFrame += static_cast<qint64>(enginePosition.rawCurrentFrame) - prepared_engine_frame_;
    }
    return qBound<qint64>(0, sourceFrame * 1000 / prepared_sample_rate_, durationMs);
}

void TrackRecordingWorkflow::consumeSnapshot(
    const jam2::EngineSnapshot& snapshot,
    const MetronomeTransportController::SnapshotUpdate& transportUpdate) noexcept
{
    input_latency_frames_ = static_cast<std::uint32_t>(std::max<long>(0, snapshot.input_latency_frames));
    output_latency_frames_ = static_cast<std::uint32_t>(std::max<long>(0, snapshot.output_latency_frames));
    applied_latency_frames_ = snapshot.recording_latency_compensation_frames;
    latency_sample_rate_ = qMax(1, static_cast<int>(std::lround(snapshot.sample_rate)));
    prepared_engine_frame_ = static_cast<qint64>(snapshot.engine_frame);
    prepared_source_frame_ = static_cast<qint64>(snapshot.prepared_source_frame);
    prepared_playing_ = snapshot.prepared_source_playing;
    if (snapshot.sample_rate > 0.0) {
        prepared_sample_rate_ = static_cast<int>(std::lround(snapshot.sample_rate));
    }
    if (transportUpdate.recordingScheduleAdvanced) {
        recording_countdown_start_frame_ = transportUpdate.recordingCountdownStartFrame;
        recording_start_frame_ = transportUpdate.recordingStartFrame;
    }
}

std::optional<int> TrackRecordingWorkflow::takeReadyPendingCountIn(
    const jam2::EngineSnapshot& snapshot) noexcept
{
    if (pending_count_in_bars_ > 0 && jam2::gui::recording_grid_ready_for_count_in(
            snapshot.metronome_enabled,
            snapshot.metronome_epoch_valid,
            snapshot.metronome_epoch_frame,
            pending_count_in_requires_fresh_epoch_,
            pending_count_in_prior_epoch_frame_)) {
        const int bars = pending_count_in_bars_;
        pending_count_in_bars_ = 0;
        pending_count_in_requires_fresh_epoch_ = false;
        pending_count_in_prior_epoch_frame_ = 0;
        return bars;
    }
    return std::nullopt;
}

bool TrackRecordingWorkflow::armTrackTake(const QString& id, const QString& output) noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::ArmTrackTake;
    if (!jam2::engine_command_set_id(command, id.toStdString()) ||
        !jam2::engine_command_set_text(command, output.toStdString())) {
        return false;
    }
    return submit(command);
}

bool TrackRecordingWorkflow::startTrackTake(std::uint64_t targetFrame) noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StartTrackTake;
    command.frame = targetFrame;
    return submit(command);
}

bool TrackRecordingWorkflow::startTrackTakeQuantized(
    int countInBars,
    const PlaybackGrid::Position& position,
    int beatsPerBar,
    QString& error) noexcept
{
    if (!position.engineAnchored || !position.running || position.sampleRate <= 0) {
        error = QStringLiteral("recording count-in is waiting for a running engine grid");
        return false;
    }
    const auto beats = static_cast<std::uint64_t>(qMax(1, beatsPerBar));
    std::uint64_t nextBarBeat = (position.absoluteBeat / beats + 1ULL) * beats;
    const std::uint64_t beatFrames = static_cast<std::uint64_t>(std::llround(
        position.secondsPerBeat * static_cast<double>(position.sampleRate)));
    if (beatFrames == 0 || nextBarBeat >
        ((std::numeric_limits<std::uint64_t>::max)() - position.epochFrame) / beatFrames) {
        error = QStringLiteral("recording count-in target exceeds the engine clock range");
        return false;
    }
    std::uint64_t countdownMusicalFrame = position.epochFrame + nextBarBeat * beatFrames;
    const std::uint64_t safeNextBarBeat = jam2::gui::recording_count_in_bar_beat(
        position.absoluteBeat,
        beatsPerBar,
        position.rawCurrentFrame,
        rawFrameFromMusicalFrame(countdownMusicalFrame, position.renderOffsetFrames),
        position.sampleRate);
    if (safeNextBarBeat != nextBarBeat) {
        if (safeNextBarBeat == (std::numeric_limits<std::uint64_t>::max)() ||
            safeNextBarBeat >
                ((std::numeric_limits<std::uint64_t>::max)() - position.epochFrame) / beatFrames) {
            error = QStringLiteral("recording count-in target exceeds the engine clock range");
            return false;
        }
        nextBarBeat = safeNextBarBeat;
        countdownMusicalFrame = position.epochFrame + nextBarBeat * beatFrames;
    }
    const std::uint64_t countInBeats = static_cast<std::uint64_t>(qMax(0, countInBars)) * beats;
    if (countInBeats >
        ((std::numeric_limits<std::uint64_t>::max)() - countdownMusicalFrame) / beatFrames) {
        error = QStringLiteral("recording start target exceeds the engine clock range");
        return false;
    }
    const std::uint64_t targetMusicalFrame = countdownMusicalFrame + countInBeats * beatFrames;
    const std::uint64_t countdownStart = rawFrameFromMusicalFrame(
        countdownMusicalFrame, position.renderOffsetFrames);
    const std::uint64_t target = rawFrameFromMusicalFrame(
        targetMusicalFrame, position.renderOffsetFrames);
    jam2::EngineCommand seek;
    seek.type = jam2::EngineCommandType::PreparedSeek;
    seek.frame = target;
    if (!submit(seek)) {
        error = QStringLiteral("engine command queue unavailable while scheduling track reset for recording");
        return false;
    }
    jam2::EngineCommand play;
    play.type = jam2::EngineCommandType::PreparedPlay;
    play.frame = target;
    if (!submit(play)) {
        error = QStringLiteral("engine command queue unavailable while scheduling track playback for recording");
        return false;
    }
    jam2::EngineCommand start;
    start.type = jam2::EngineCommandType::StartTrackTake;
    start.frame = target;
    if (!submit(start)) {
        error = QStringLiteral("engine command queue unavailable while scheduling the recording take");
        return false;
    }
    jam2::EngineCommand transport;
    transport.type = jam2::EngineCommandType::ScheduleTransport;
    transport.transport_action = jam2::EngineTransportAction::RecordStart;
    transport.transport_target_frame = target;
    transport.transport_musical_frame = targetMusicalFrame;
    transport.transport_countdown_start_frame = countdownStart;
    if (!submit(transport)) {
        error = QStringLiteral("engine command queue unavailable while publishing the recording schedule");
        return false;
    }
    recording_countdown_start_frame_ = countdownStart;
    recording_start_frame_ = target;
    return true;
}

bool TrackRecordingWorkflow::startInputTake(
    const QString& outputPath,
    bool transientOutput,
    std::uint64_t targetFrame,
    std::optional<int> countInBars,
    const PlaybackGrid::Position& position,
    int beatsPerBar,
    QString& error)
{
    if (input_take_active_) {
        error = QStringLiteral("an input take is already active");
        return false;
    }
    const QString takeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!armTrackTake(takeId, QDir::toNativeSeparators(outputPath))) {
        error = QStringLiteral("track take id/output is too long or the engine command queue is unavailable");
        return false;
    }
    const bool started = countInBars
        ? startTrackTakeQuantized(*countInBars, position, beatsPerBar, error)
        : startTrackTake(targetFrame);
    if (!started) {
        if (error.isEmpty()) {
            error = QStringLiteral("engine command queue unavailable while starting the recording take");
        }
        return false;
    }
    active_take_id_ = takeId;
    input_take_active_ = true;
    last_capture_path_ = outputPath;
    pending_transient_capture_path_ = transientOutput ? outputPath : QString{};
    return true;
}

bool TrackRecordingWorkflow::stopInputTake(std::uint64_t targetFrame) noexcept
{
    if (!input_take_active_ || active_take_id_.isEmpty()) {
        return false;
    }
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StopTrackTake;
    command.frame = targetFrame;
    return submit(command);
}

TrackRecordingWorkflow::TrackTakeCompletion TrackRecordingWorkflow::consumeTrackTakeEvent(
    const jam2::EngineEvent& event)
{
    TrackTakeCompletion completion;
    if (event.type != jam2::EngineEventType::TrackTakeCompleted) {
        return completion;
    }
    completion.handled = true;
    completion.ok = event.ok;
    completion.takeId = active_take_id_;
    completion.wavPath = pending_transient_capture_path_;
    completion.frames = event.value;
    const std::string_view text = jam2::engine_event_text(event);
    completion.error = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    input_take_active_ = false;
    active_take_id_.clear();
    clearRecordingSchedule();
    if (event.ok && !completion.wavPath.isEmpty()) {
        last_capture_path_ = QDir::fromNativeSeparators(completion.wavPath);
        pending_transient_capture_path_.clear();
    }
    return completion;
}

void TrackRecordingWorkflow::waitForCountIn(
    int bars,
    bool stopMetronomeAtStart,
    bool requireFreshEpoch,
    std::uint64_t priorEpochFrame) noexcept
{
    pending_count_in_bars_ = qMax(0, bars);
    pending_count_in_requires_fresh_epoch_ =
        pending_count_in_bars_ > 0 && requireFreshEpoch;
    pending_count_in_prior_epoch_frame_ = pending_count_in_requires_fresh_epoch_
        ? priorEpochFrame
        : 0;
    stop_metronome_at_recording_start_ = stopMetronomeAtStart;
}

void TrackRecordingWorkflow::clearRecordingSchedule() noexcept
{
    recording_countdown_start_frame_ = 0;
    recording_start_frame_ = 0;
    pending_count_in_bars_ = 0;
    pending_count_in_requires_fresh_epoch_ = false;
    pending_count_in_prior_epoch_frame_ = 0;
    stop_metronome_at_recording_start_ = false;
}

TrackRecordingWorkflow::CountdownPresentation TrackRecordingWorkflow::countdown(
    const PlaybackGrid::Position& position) noexcept
{
    CountdownPresentation presentation;
    if (recording_start_frame_ == 0 || !input_take_active_) {
        return presentation;
    }
    if (position.rawCurrentFrame < recording_countdown_start_frame_) {
        presentation.phase = CountdownPhase::WaitingForBar;
        return presentation;
    }
    if (position.rawCurrentFrame < recording_start_frame_) {
        const std::uint64_t beatFrames = position.secondsPerBeat > 0.0 && position.sampleRate > 0
            ? static_cast<std::uint64_t>(std::llround(position.secondsPerBeat * position.sampleRate))
            : static_cast<std::uint64_t>(qMax(1, position.sampleRate)) / 2ULL;
        presentation.phase = CountdownPhase::Counting;
        presentation.remainingBeats = remainingCountInBeats(
            position.rawCurrentFrame, recording_start_frame_, std::max<std::uint64_t>(1ULL, beatFrames));
        return presentation;
    }
    presentation.phase = CountdownPhase::Recording;
    presentation.stopMetronome = stop_metronome_at_recording_start_;
    stop_metronome_at_recording_start_ = false;
    return presentation;
}

void TrackRecordingWorkflow::beginLoopbackCapture(
    const QString& outputPath,
    bool transientOutput)
{
    last_capture_path_ = outputPath;
    pending_transient_capture_path_ = transientOutput ? outputPath : QString{};
}

QString TrackRecordingWorkflow::finishLoopbackCapture(const QString& outputPath)
{
    last_capture_path_ = outputPath;
    return abandonPendingCapture();
}

QString TrackRecordingWorkflow::abandonPendingCapture()
{
    const QString path = pending_transient_capture_path_;
    pending_transient_capture_path_.clear();
    return path;
}

void TrackRecordingWorkflow::armLane(
    int bankIndex,
    int laneIndex,
    CaptureMode mode) noexcept
{
    armed_bank_ = bankIndex;
    armed_lane_ = laneIndex;
    capture_mode_ = mode;
}

void TrackRecordingWorkflow::disarmLane() noexcept
{
    armed_bank_ = -1;
    armed_lane_ = -1;
    capture_mode_ = CaptureMode::Input;
}

bool TrackRecordingWorkflow::laneArmed() const noexcept
{
    return armed_bank_ >= 0 && armed_lane_ >= 0;
}

bool TrackRecordingWorkflow::laneArmedAt(int bankIndex, int laneIndex) const noexcept
{
    return armed_bank_ == bankIndex && armed_lane_ == laneIndex;
}

bool TrackRecordingWorkflow::startJamRecording(const QString& folder)
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StartJamRecording;
    if (!jam2::engine_command_set_text(command, folder.toStdString()) || !submit(command)) {
        return false;
    }
    jam_recording_folder_ = folder;
    jam_recording_active_ = true;
    return true;
}

bool TrackRecordingWorkflow::stopJamRecording() noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StopJamRecording;
    if (!submit(command)) {
        return false;
    }
    jam_recording_active_ = false;
    return true;
}

bool TrackRecordingWorkflow::consumeJamRecordingEvent(const jam2::EngineEvent& event) noexcept
{
    if (event.type == jam2::EngineEventType::JamRecordingStarted) {
        jam_recording_active_ = true;
        return true;
    }
    if (event.type == jam2::EngineEventType::JamRecordingStopped) {
        jam_recording_active_ = false;
        return true;
    }
    return false;
}

void TrackRecordingWorkflow::clearJamRecordingState() noexcept
{
    jam_recording_active_ = false;
}

void TrackRecordingWorkflow::clearProjectCapture() noexcept
{
    last_capture_path_.clear();
    pending_transient_capture_path_.clear();
    disarmLane();
}

void TrackRecordingWorkflow::clearSessionSchedule() noexcept
{
    clearRecordingSchedule();
    clearJamRecordingState();
}
