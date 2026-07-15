#include "MetronomeTransportController.hpp"

#include <cmath>

MetronomeTransportController::MetronomeTransportController(
    ApplicationRuntime& runtime) noexcept
    : runtime_(runtime)
{
}

MetronomeTransportController::SnapshotUpdate MetronomeTransportController::consume(
    const jam2::EngineSnapshot& snapshot)
{
    const auto pattern = snapshot.metronome_pattern;
    const std::int64_t offset = snapshot.metronome_render_offset_frames;
    const std::uint64_t musical = offset >= 0
        ? snapshot.engine_frame + static_cast<std::uint64_t>(offset)
        : snapshot.engine_frame > static_cast<std::uint64_t>(-offset)
            ? snapshot.engine_frame - static_cast<std::uint64_t>(-offset)
            : 0ULL;
    grid_.setPattern(pattern.bpm, pattern.beats_per_bar, pattern.division);
    grid_.updateEngine(
        snapshot.engine_frame,
        musical,
        snapshot.metronome_epoch_frame,
        offset,
        static_cast<int>(std::lround(snapshot.sample_rate)),
        snapshot.metronome_enabled && snapshot.metronome_epoch_valid);

    SnapshotUpdate update;
    const std::uint64_t revision = snapshot.transport_revision;
    if (transportActionResetsGridEpoch(snapshot.transport_action) && revision > 0) {
        grid_.scheduleEpoch(snapshot.transport_target_frame, snapshot.transport_musical_frame, revision);
    }
    if (snapshot.transport_action == jam2::EngineTransportAction::RecordStart &&
        revision > recording_schedule_revision_) {
        recording_schedule_revision_ = revision;
        update.recordingScheduleAdvanced = true;
        update.recordingCountdownStartFrame = snapshot.transport_countdown_start_frame;
        update.recordingStartFrame = snapshot.transport_target_frame;
    }
    return update;
}

bool MetronomeTransportController::submit(const jam2::EngineCommand& command) noexcept
{
    return runtime_.submit(command);
}

void MetronomeTransportController::clearEngine() noexcept
{
    grid_.clearEngine();
    recording_schedule_revision_ = 0;
}

void MetronomeTransportController::setLocalState(bool running, bool leader) noexcept
{
    local_running_ = running;
    local_leader_ = running && leader;
}

void MetronomeTransportController::setApplyingRemoteSettings(bool applying) noexcept
{
    applying_remote_settings_ = applying;
}

bool MetronomeTransportController::allowsLocalGridMutation(
    bool applyingRemoteSettings) noexcept
{
    return !applyingRemoteSettings;
}

bool MetronomeTransportController::transportActionResetsGridEpoch(
    jam2::EngineTransportAction action) noexcept
{
    return action == jam2::EngineTransportAction::TrackRestart ||
        action == jam2::EngineTransportAction::RecordStart;
}
