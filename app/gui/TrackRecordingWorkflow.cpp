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

std::uint64_t jam2::gui::recording_count_in_start_beat(
    std::uint64_t absoluteBeat,
    std::uint64_t rawCurrentFrame,
    std::uint64_t nextBeatRawFrame,
    int sampleRate) noexcept
{
    if (absoluteBeat == (std::numeric_limits<std::uint64_t>::max)()) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    const std::uint64_t nextBeat = absoluteBeat + 1ULL;
    const std::uint64_t minimumLeadFrames =
        static_cast<std::uint64_t>(qMax(1, sampleRate) / 5);
    const std::uint64_t minimumCountdownRawFrame =
        rawCurrentFrame > (std::numeric_limits<std::uint64_t>::max)() - minimumLeadFrames
        ? (std::numeric_limits<std::uint64_t>::max)()
        : rawCurrentFrame + minimumLeadFrames;
    if (nextBeatRawFrame < minimumCountdownRawFrame) {
        return nextBeat == (std::numeric_limits<std::uint64_t>::max)()
            ? (std::numeric_limits<std::uint64_t>::max)()
            : nextBeat + 1ULL;
    }
    return nextBeat;
}

bool jam2::gui::recording_grid_ready_for_count_in(bool epochValid) noexcept
{
    return epochValid;
}

std::uint64_t jam2::gui::global_transport_elapsed_frames(
    bool playing,
    bool engineAnchored,
    std::uint64_t rawCurrentFrame,
    std::uint64_t actualStartFrame) noexcept
{
    if (!playing || !engineAnchored || rawCurrentFrame < actualStartFrame) {
        return 0;
    }
    return rawCurrentFrame - actualStartFrame;
}

std::uint64_t jam2::gui::next_safe_grid_beat_raw_frame(
    const PlaybackGrid::Position& position) noexcept
{
    if (!position.engineAnchored || position.sampleRate <= 0) {
        return 0;
    }
    const std::uint64_t leadFrames =
        static_cast<std::uint64_t>(position.sampleRate / 5);
    const std::uint64_t minimumLead = position.rawCurrentFrame >
        (std::numeric_limits<std::uint64_t>::max)() - leadFrames
        ? (std::numeric_limits<std::uint64_t>::max)()
        : position.rawCurrentFrame + leadFrames;
    if (!position.running || position.secondsPerBeat <= 0.0 ||
        position.absoluteBeat == (std::numeric_limits<std::uint64_t>::max)()) {
        return minimumLead;
    }
    const auto targetForBeat = [&position](std::uint64_t beat) {
        const long double offset = static_cast<long double>(beat) *
            static_cast<long double>(position.secondsPerBeat) *
            static_cast<long double>(position.sampleRate);
        const long double maximumOffset = std::min(
            static_cast<long double>(
                (std::numeric_limits<std::uint64_t>::max)() - position.epochFrame),
            static_cast<long double>((std::numeric_limits<long long>::max)()));
        if (!std::isfinite(offset) || offset < 0.0L || offset > maximumOffset) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return rawFrameFromMusicalFrame(
            position.epochFrame + static_cast<std::uint64_t>(std::llround(offset)),
            position.renderOffsetFrames);
    };
    std::uint64_t beat = position.absoluteBeat + 1ULL;
    std::uint64_t target = targetForBeat(beat);
    if (target < minimumLead &&
        beat != (std::numeric_limits<std::uint64_t>::max)()) {
        target = targetForBeat(beat + 1ULL);
    }
    return target;
}

bool jam2::gui::prepared_attach_has_applied(
    std::uint64_t pendingTargetFrame,
    std::uint64_t engineFrame,
    std::uint64_t preparedScheduledStartFrame) noexcept
{
    return pendingTargetFrame > 0 && engineFrame >= pendingTargetFrame &&
        preparedScheduledStartFrame == pendingTargetFrame;
}

int jam2::gui::resolve_active_sample_rate(
    int sessionSampleRate,
    double engineSampleRate,
    int configuredSampleRate) noexcept
{
    if (sessionSampleRate > 0) {
        return sessionSampleRate;
    }
    if (std::isfinite(engineSampleRate) && engineSampleRate > 0.0 &&
        engineSampleRate <= static_cast<double>((std::numeric_limits<int>::max)())) {
        return static_cast<int>(std::lround(engineSampleRate));
    }
    return configuredSampleRate > 0 ? configuredSampleRate : 48000;
}

bool jam2::gui::sample_rate_matches_engine(
    int expectedSampleRate,
    double engineSampleRate) noexcept
{
    return expectedSampleRate > 0 && std::isfinite(engineSampleRate) &&
        engineSampleRate > 0.0 &&
        std::abs(engineSampleRate - static_cast<double>(expectedSampleRate)) <= 1.0;
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
    bool localOnly) noexcept
{
    const std::uint64_t target = jam2::gui::next_safe_grid_beat_raw_frame(position);
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
    cancelPreparedAttach();
    return scheduleGlobalTransportStart(
        target,
        musicalFrameFromRawFrame(target, position.renderOffsetFrames),
        localOnly);
}

bool TrackRecordingWorkflow::restartGlobalTransport(
    const PlaybackGrid::Position& position,
    bool localOnly) noexcept
{
    const std::uint64_t target = jam2::gui::next_safe_grid_beat_raw_frame(position);
    if (target == 0) {
        return false;
    }
    return scheduleGlobalTransportStart(
        target,
        musicalFrameFromRawFrame(target, position.renderOffsetFrames),
        localOnly);
}

bool TrackRecordingWorkflow::scheduleBankRestart(
    std::uint64_t targetFrame,
    std::uint64_t musicalFrame,
    bool preparedAvailable) noexcept
{
    if (targetFrame == 0) return false;
    jam2::EngineCommand source;
    source.type = preparedAvailable
        ? jam2::EngineCommandType::PreparedSeek
        : jam2::EngineCommandType::PreparedStop;
    source.frame = targetFrame;
    source.frame_end = 0;
    if (!submit(source)) return false;
    if (preparedAvailable) {
        jam2::EngineCommand play;
        play.type = jam2::EngineCommandType::PreparedPlay;
        play.frame = targetFrame;
        if (!submit(play)) return false;
    }
    cancelPreparedAttach();
    return scheduleGlobalTransportStart(targetFrame, musicalFrame);
}

bool TrackRecordingWorkflow::scheduleGlobalTransportStart(
    std::uint64_t targetFrame,
    std::uint64_t musicalFrame,
    bool localOnly) noexcept
{
    jam2::EngineCommand transport;
    transport.type = jam2::EngineCommandType::ScheduleTransport;
    transport.transport_local_only = localOnly;
    transport.transport_action = jam2::EngineTransportAction::TrackRestart;
    transport.transport_target_frame = targetFrame;
    transport.transport_musical_frame = musicalFrame;
    transport.transport_countdown_start_frame = targetFrame;
    if (!submit(transport)) {
        return false;
    }
    global_transport_requested_playing_ = true;
    pending_global_transport_start_frame_ = targetFrame;
    pending_global_transport_stop_frame_ = 0;
    return true;
}

bool TrackRecordingWorkflow::stopPrepared(
    std::uint64_t targetFrame,
    std::uint64_t musicalFrame,
    bool localOnly) noexcept
{
    // A bank change may already have queued a later seek/play. Invalidate that
    // generation first so a user Stop cannot be followed by the stale bank
    // restart after the stop boundary.
    jam2::EngineCommand cancel;
    cancel.type = jam2::EngineCommandType::CancelTransport;
    if (!submit(cancel)) {
        return false;
    }
    jam2::EngineCommand stop;
    stop.type = jam2::EngineCommandType::PreparedStop;
    stop.frame = targetFrame;
    if (!submit(stop)) {
        return false;
    }
    cancelPreparedAttach();
    jam2::EngineCommand transport;
    transport.type = jam2::EngineCommandType::ScheduleTransport;
    transport.transport_local_only = localOnly;
    transport.transport_action = jam2::EngineTransportAction::TrackStop;
    transport.transport_target_frame = targetFrame;
    transport.transport_musical_frame = musicalFrame;
    transport.transport_countdown_start_frame = targetFrame;
    if (!submit(transport)) {
        return false;
    }
    global_transport_requested_playing_ = false;
    pending_global_transport_start_frame_ = 0;
    if (targetFrame == 0) {
        clearGlobalTransport();
    } else {
        pending_global_transport_stop_frame_ = targetFrame;
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

void TrackRecordingWorkflow::notePreparedAttachScheduled(
    std::uint64_t targetFrame) noexcept
{
    pending_prepared_attach_target_frame_ = targetFrame;
}

void TrackRecordingWorkflow::cancelPreparedAttach() noexcept
{
    pending_prepared_attach_target_frame_ = 0;
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

qint64 TrackRecordingWorkflow::currentTransportPositionMs(
    const PlaybackGrid::Position& enginePosition,
    qint64 durationMs) const noexcept
{
    (void)durationMs;
    if (enginePosition.sampleRate <= 0) {
        return 0;
    }
    const std::uint64_t elapsedFrames = jam2::gui::global_transport_elapsed_frames(
        global_transport_playing_,
        enginePosition.engineAnchored,
        enginePosition.rawCurrentFrame,
        global_transport_start_frame_);
    const std::uint64_t sampleRate = static_cast<std::uint64_t>(enginePosition.sampleRate);
    const std::uint64_t wholeSeconds = elapsedFrames / sampleRate;
    const std::uint64_t remainingFrames = elapsedFrames % sampleRate;
    const std::uint64_t maximumMs = static_cast<std::uint64_t>(
        (std::numeric_limits<qint64>::max)());
    if (wholeSeconds > maximumMs / 1000ULL) {
        return (std::numeric_limits<qint64>::max)();
    }
    const std::uint64_t elapsedMs = wholeSeconds * 1000ULL +
        remainingFrames * 1000ULL / sampleRate;
    return static_cast<qint64>(elapsedMs);
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
    prepared_actual_start_frame_ = snapshot.prepared_source_actual_start_frame;
    prepared_playing_ = snapshot.prepared_source_playing;
    if (snapshot.transport_revision > observed_transport_revision_) {
        observed_transport_revision_ = snapshot.transport_revision;
        if (snapshot.transport_pending &&
            (snapshot.transport_action == jam2::EngineTransportAction::TrackRestart ||
             snapshot.transport_action == jam2::EngineTransportAction::TrackPlay)) {
            global_transport_requested_playing_ = true;
            pending_global_transport_start_frame_ = snapshot.transport_target_frame;
            pending_global_transport_stop_frame_ = 0;
        } else if (snapshot.transport_pending &&
                   snapshot.transport_action ==
                       jam2::EngineTransportAction::TrackStop) {
            global_transport_requested_playing_ = false;
            pending_global_transport_start_frame_ = 0;
            pending_global_transport_stop_frame_ = snapshot.transport_target_frame;
        }
    }
    if (snapshot.transport_commit_count > observed_transport_commit_count_) {
        observed_transport_commit_count_ = snapshot.transport_commit_count;
        if (snapshot.transport_action == jam2::EngineTransportAction::TrackRestart ||
            snapshot.transport_action == jam2::EngineTransportAction::TrackPlay ||
            snapshot.transport_action == jam2::EngineTransportAction::RecordStart) {
            global_transport_requested_playing_ = true;
            global_transport_playing_ = true;
            global_transport_start_frame_ = snapshot.transport_target_frame;
            pending_global_transport_start_frame_ = 0;
            pending_global_transport_stop_frame_ = 0;
        } else if (snapshot.transport_action ==
                   jam2::EngineTransportAction::TrackStop) {
            clearGlobalTransport();
        }
    }
    if (pending_global_transport_start_frame_ > 0 &&
        snapshot.engine_frame >= pending_global_transport_start_frame_) {
        global_transport_playing_ = true;
        global_transport_start_frame_ = pending_global_transport_start_frame_;
        pending_global_transport_start_frame_ = 0;
    }
    if (pending_global_transport_stop_frame_ > 0 &&
        snapshot.engine_frame >= pending_global_transport_stop_frame_) {
        clearGlobalTransport();
    }
    if (jam2::gui::prepared_attach_has_applied(
            pending_prepared_attach_target_frame_,
            snapshot.engine_frame,
            snapshot.prepared_source_scheduled_start_frame)) {
        pending_prepared_attach_target_frame_ = 0;
    }
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
            snapshot.metronome_epoch_valid)) {
        const int bars = pending_count_in_bars_;
        pending_count_in_bars_ = 0;
        return bars;
    }
    return std::nullopt;
}

bool TrackRecordingWorkflow::armTrackTake(
    const QString& id,
    const QString& output,
    bool includePrepared,
    bool includeMetronome) noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::ArmTrackTake;
    if (!jam2::engine_command_set_id(command, id.toStdString()) ||
        !jam2::engine_command_set_text(command, output.toStdString())) {
        return false;
    }
    command.value = capture_mode_ == CaptureMode::CurrentJam
        ? static_cast<std::int32_t>(jam2::audio::TrackTakeSource::CurrentJam)
        : static_cast<std::int32_t>(jam2::audio::TrackTakeSource::Input);
    if (includePrepared) command.value |= jam2::audio::kTrackTakeIncludePrepared;
    if (includeMetronome) command.value |= jam2::audio::kTrackTakeIncludeMetronome;
    return submit(command);
}

bool TrackRecordingWorkflow::startTrackTake(
    std::uint64_t targetFrame,
    std::uint64_t durationFrames) noexcept
{
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StartTrackTake;
    command.frame = targetFrame;
    if (!submit(command)) {
        return false;
    }
    if (durationFrames == 0) {
        return true;
    }
    if (targetFrame > (std::numeric_limits<std::uint64_t>::max)() - durationFrames) {
        return false;
    }
    command.type = jam2::EngineCommandType::StopTrackTake;
    command.frame = targetFrame + durationFrames;
    return submit(command);
}

bool TrackRecordingWorkflow::startTrackTakeQuantized(
    int countInBars,
    std::uint64_t durationFrames,
    const PlaybackGrid::Position& position,
    int beatsPerBar,
    bool transportLocalOnly,
    QString& error) noexcept
{
    if (!position.engineAnchored || !position.running || position.sampleRate <= 0) {
        error = QStringLiteral("recording count-in is waiting for a running engine grid");
        return false;
    }
    const auto beats = static_cast<std::uint64_t>(qMax(1, beatsPerBar));
    const std::uint64_t currentBar = position.absoluteBeat / beats;
    if (currentBar == (std::numeric_limits<std::uint64_t>::max)() ||
        currentBar + 1ULL > (std::numeric_limits<std::uint64_t>::max)() / beats) {
        error = QStringLiteral("recording count-in target exceeds the engine clock range");
        return false;
    }
    std::uint64_t nextBeat = (currentBar + 1ULL) * beats;
    const std::uint64_t beatFrames = static_cast<std::uint64_t>(std::llround(
        position.secondsPerBeat * static_cast<double>(position.sampleRate)));
    if (beatFrames == 0 || nextBeat >
        ((std::numeric_limits<std::uint64_t>::max)() - position.epochFrame) / beatFrames) {
        error = QStringLiteral("recording count-in target exceeds the engine clock range");
        return false;
    }
    std::uint64_t countdownMusicalFrame = position.epochFrame + nextBeat * beatFrames;
    const std::uint64_t minimumLeadFrames = static_cast<std::uint64_t>(
        qMax(1, position.sampleRate) / 5);
    const std::uint64_t minimumCountdownRawFrame =
        position.rawCurrentFrame >
            (std::numeric_limits<std::uint64_t>::max)() - minimumLeadFrames
        ? (std::numeric_limits<std::uint64_t>::max)()
        : position.rawCurrentFrame + minimumLeadFrames;
    const std::uint64_t countdownRawFrame = rawFrameFromMusicalFrame(
        countdownMusicalFrame, position.renderOffsetFrames);
    if (countdownRawFrame < minimumCountdownRawFrame) {
        if (nextBeat > (std::numeric_limits<std::uint64_t>::max)() - beats ||
            nextBeat + beats >
                ((std::numeric_limits<std::uint64_t>::max)() - position.epochFrame) / beatFrames) {
            error = QStringLiteral("recording count-in target exceeds the engine clock range");
            return false;
        }
        nextBeat += beats;
        countdownMusicalFrame = position.epochFrame + nextBeat * beatFrames;
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
    if (!startTrackTake(target, durationFrames)) {
        error = QStringLiteral("engine command queue unavailable while scheduling the recording take or its bar limit");
        return false;
    }
    jam2::EngineCommand transport;
    transport.type = jam2::EngineCommandType::ScheduleTransport;
    transport.transport_local_only = transportLocalOnly;
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
    int expectedSampleRate,
    std::uint64_t targetFrame,
    std::uint64_t durationFrames,
    std::optional<int> countInBars,
    const PlaybackGrid::Position& position,
    int beatsPerBar,
    bool includePrepared,
    bool includeMetronome,
    bool transportLocalOnly,
    QString& error)
{
    if (input_take_active_) {
        error = QStringLiteral("an input take is already active");
        return false;
    }
    const jam2::EngineSnapshot engine = runtime_.engineSnapshot();
    if (!jam2::gui::sample_rate_matches_engine(
            expectedSampleRate, engine.sample_rate)) {
        error = QStringLiteral(
            "recording target is %1 Hz but the active engine is %2 Hz")
            .arg(expectedSampleRate)
            .arg(engine.sample_rate, 0, 'f', 0);
        return false;
    }
    const QString takeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!armTrackTake(
            takeId,
            QDir::toNativeSeparators(outputPath),
            includePrepared,
            includeMetronome)) {
        error = QStringLiteral("track take id/output is too long or the engine command queue is unavailable");
        return false;
    }
    const bool started = countInBars
        ? startTrackTakeQuantized(
            *countInBars,
            durationFrames,
            position,
            beatsPerBar,
            transportLocalOnly,
            error)
        : startTrackTake(targetFrame, durationFrames);
    if (!started) {
        if (error.isEmpty()) {
            error = QStringLiteral("engine command queue unavailable while starting the recording take");
        }
        return false;
    }
    active_take_id_ = takeId;
    active_take_sample_rate_ = expectedSampleRate;
    last_capture_sample_rate_ = expectedSampleRate;
    input_take_active_ = true;
    last_capture_path_ = outputPath;
    pending_transient_capture_path_ = transientOutput ? outputPath : QString{};
    if (!countInBars) {
        recording_start_frame_ = targetFrame;
    }
    return true;
}

bool TrackRecordingWorkflow::startInputTakeAtSchedule(
    const QString& outputPath,
    bool transientOutput,
    int expectedSampleRate,
    std::uint64_t countdownStartFrame,
    std::uint64_t targetFrame,
    std::uint64_t targetMusicalFrame,
    std::uint64_t durationFrames,
    bool includePrepared,
    bool includeMetronome,
    QString& error)
{
    if (input_take_active_) {
        error = QStringLiteral("an input take is already active");
        return false;
    }
    const jam2::EngineSnapshot engine = runtime_.engineSnapshot();
    if (!jam2::gui::sample_rate_matches_engine(
            expectedSampleRate, engine.sample_rate)) {
        error = QStringLiteral(
            "recording target is %1 Hz but the active engine is %2 Hz")
            .arg(expectedSampleRate)
            .arg(engine.sample_rate, 0, 'f', 0);
        return false;
    }
    if (targetFrame == 0 || countdownStartFrame > targetFrame) {
        error = QStringLiteral("shared recording schedule is invalid");
        return false;
    }
    const QString takeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!armTrackTake(
            takeId,
            QDir::toNativeSeparators(outputPath),
            includePrepared,
            includeMetronome)) {
        error = QStringLiteral(
            "track take id/output is too long or the engine command queue is unavailable");
        return false;
    }
    jam2::EngineCommand seek;
    seek.type = jam2::EngineCommandType::PreparedSeek;
    seek.frame = targetFrame;
    if (!submit(seek)) {
        error = QStringLiteral(
            "engine command queue unavailable while scheduling shared track reset");
        return false;
    }
    jam2::EngineCommand play;
    play.type = jam2::EngineCommandType::PreparedPlay;
    play.frame = targetFrame;
    if (!submit(play)) {
        error = QStringLiteral(
            "engine command queue unavailable while scheduling shared track playback");
        return false;
    }
    if (!startTrackTake(targetFrame, durationFrames)) {
        error = QStringLiteral(
            "engine command queue unavailable while scheduling the shared recording take");
        return false;
    }
    jam2::EngineCommand localTransport;
    localTransport.type = jam2::EngineCommandType::ScheduleTransport;
    localTransport.transport_local_only = true;
    localTransport.transport_action = jam2::EngineTransportAction::RecordStart;
    localTransport.transport_target_frame = targetFrame;
    localTransport.transport_musical_frame = targetMusicalFrame;
    localTransport.transport_countdown_start_frame = countdownStartFrame;
    if (!submit(localTransport)) {
        error = QStringLiteral(
            "engine command queue unavailable while adopting the shared recording schedule");
        return false;
    }
    active_take_id_ = takeId;
    active_take_sample_rate_ = expectedSampleRate;
    last_capture_sample_rate_ = expectedSampleRate;
    input_take_active_ = true;
    last_capture_path_ = outputPath;
    pending_transient_capture_path_ = transientOutput ? outputPath : QString{};
    recording_countdown_start_frame_ = countdownStartFrame;
    recording_start_frame_ = targetFrame;
    return true;
}

bool TrackRecordingWorkflow::scheduleRecordingTransport(
    std::uint64_t countdownStartFrame,
    std::uint64_t targetFrame,
    std::uint64_t targetMusicalFrame,
    bool localOnly) noexcept
{
    if (targetFrame == 0 || countdownStartFrame > targetFrame) return false;
    jam2::EngineCommand seek;
    seek.type = jam2::EngineCommandType::PreparedSeek;
    seek.frame = targetFrame;
    if (!submit(seek)) return false;
    jam2::EngineCommand play;
    play.type = jam2::EngineCommandType::PreparedPlay;
    play.frame = targetFrame;
    if (!submit(play)) return false;
    jam2::EngineCommand transport;
    transport.type = jam2::EngineCommandType::ScheduleTransport;
    transport.transport_local_only = localOnly;
    transport.transport_action = jam2::EngineTransportAction::RecordStart;
    transport.transport_target_frame = targetFrame;
    transport.transport_musical_frame = targetMusicalFrame;
    transport.transport_countdown_start_frame = countdownStartFrame;
    return submit(transport);
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
    completion.sampleRate = event.sample_rate > 0
        ? event.sample_rate
        : active_take_sample_rate_;
    if (completion.sampleRate > 0) {
        last_capture_sample_rate_ = completion.sampleRate;
    }
    const std::string_view text = jam2::engine_event_text(event);
    completion.error = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    input_take_active_ = false;
    active_take_id_.clear();
    active_take_sample_rate_ = 0;
    clearRecordingSchedule();
    if (event.ok && !completion.wavPath.isEmpty()) {
        last_capture_path_ = QDir::fromNativeSeparators(completion.wavPath);
        pending_transient_capture_path_.clear();
    }
    return completion;
}

void TrackRecordingWorkflow::waitForCountIn(
    int bars,
    bool stopMetronomeAtStart) noexcept
{
    pending_count_in_bars_ = qMax(0, bars);
    stop_metronome_at_recording_start_ = stopMetronomeAtStart;
}

void TrackRecordingWorkflow::clearRecordingSchedule() noexcept
{
    recording_countdown_start_frame_ = 0;
    recording_start_frame_ = 0;
    pending_count_in_bars_ = 0;
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
        presentation.phase = CountdownPhase::WaitingForBeat;
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
    bool transientOutput,
    int sampleRate)
{
    last_capture_path_ = outputPath;
    last_capture_sample_rate_ = sampleRate;
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
    CaptureMode mode,
    bool includePrepared,
    bool includeMetronome) noexcept
{
    armed_bank_ = bankIndex;
    armed_lane_ = laneIndex;
    capture_mode_ = mode;
    include_prepared_in_take_ = includePrepared;
    include_metronome_in_take_ = includeMetronome;
}

void TrackRecordingWorkflow::disarmLane() noexcept
{
    armed_bank_ = -1;
    armed_lane_ = -1;
    capture_mode_ = CaptureMode::Input;
    include_prepared_in_take_ = false;
    include_metronome_in_take_ = false;
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
    last_capture_sample_rate_ = 0;
    pending_transient_capture_path_.clear();
    cancelPreparedAttach();
    disarmLane();
}

void TrackRecordingWorkflow::clearSessionSchedule() noexcept
{
    clearRecordingSchedule();
    clearJamRecordingState();
    cancelPreparedAttach();
    clearGlobalTransport();
    observed_transport_revision_ = 0;
    observed_transport_commit_count_ = 0;
}

void TrackRecordingWorkflow::clearGlobalTransport() noexcept
{
    global_transport_requested_playing_ = false;
    global_transport_playing_ = false;
    global_transport_start_frame_ = 0;
    pending_global_transport_start_frame_ = 0;
    pending_global_transport_stop_frame_ = 0;
}
