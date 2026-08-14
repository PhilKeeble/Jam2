#include "MetronomeTransportController.hpp"

#include "runtime_limits.hpp"
#include "transport_timing.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

std::optional<int> TapTempoTracker::tap(std::int64_t elapsedMs) noexcept
{
    if (!has_last_tap_) {
        last_tap_ms_ = elapsedMs;
        has_last_tap_ = true;
        return std::nullopt;
    }

    const std::int64_t previousTapMs = last_tap_ms_;
    last_tap_ms_ = elapsedMs;
    constexpr std::int64_t kFastestIntervalMs = 150;
    constexpr std::int64_t kSequenceResetIntervalMs = 2000;
    if (elapsedMs <= previousTapMs) {
        interval_count_ = 0;
        return std::nullopt;
    }
    const std::uint64_t unsignedInterval =
        static_cast<std::uint64_t>(elapsedMs) -
        static_cast<std::uint64_t>(previousTapMs);
    if (unsignedInterval < static_cast<std::uint64_t>(kFastestIntervalMs) ||
        unsignedInterval > static_cast<std::uint64_t>(kSequenceResetIntervalMs)) {
        interval_count_ = 0;
        return std::nullopt;
    }
    const std::int64_t interval = static_cast<std::int64_t>(unsignedInterval);

    if (interval_count_ < kIntervalCapacity) {
        intervals_ms_[interval_count_++] = interval;
    } else {
        for (std::size_t index = 1; index < kIntervalCapacity; ++index) {
            intervals_ms_[index - 1] = intervals_ms_[index];
        }
        intervals_ms_.back() = interval;
    }

    std::array<std::int64_t, kIntervalCapacity> sorted = intervals_ms_;
    std::sort(sorted.begin(), sorted.begin() + interval_count_);
    const std::int64_t median = interval_count_ % 2 == 0
        ? (sorted[interval_count_ / 2 - 1] + sorted[interval_count_ / 2]) / 2
        : sorted[interval_count_ / 2];
    return std::clamp(
        static_cast<int>(std::lround(60000.0 / static_cast<double>(median))),
        1,
        400);
}

void TapTempoTracker::reset() noexcept
{
    last_tap_ms_ = 0;
    interval_count_ = 0;
    has_last_tap_ = false;
}

MetronomeTransportController::MetronomeTransportController(
    CommandSubmitter submitter)
    : submitter_(std::move(submitter))
{
}

MetronomeTransportController::SnapshotUpdate MetronomeTransportController::consume(
    const jam2::EngineSnapshot& snapshot)
{
    const auto pattern = snapshot.metronome_pattern;
    const std::int64_t offset = snapshot.metronome_render_offset_frames;
    const std::uint64_t musical = jam2::transport_musical_frame_from_raw(
        snapshot.engine_frame, offset);
    int sampleRate = 0;
    if (std::isfinite(snapshot.sample_rate) &&
        snapshot.sample_rate >= jam2::limits::kMinimumSampleRate - 0.5 &&
        snapshot.sample_rate < jam2::limits::kMaximumSampleRate + 0.5) {
        const int rounded = static_cast<int>(std::lround(snapshot.sample_rate));
        if (jam2::limits::valid_sample_rate(rounded)) {
            sampleRate = rounded;
        }
    }
    grid_.setPattern(
        pattern.bpm,
        pattern.beats_per_bar,
        pattern.division,
        pattern.tempo_pulse_units);
    grid_.updateEngine(
        snapshot.engine_frame,
        musical,
        snapshot.metronome_epoch_frame,
        offset,
        sampleRate,
        snapshot.metronome_epoch_valid && sampleRate > 0);

    SnapshotUpdate update;
    const std::uint64_t revision = snapshot.transport_revision;
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
    if (!submitter_) {
        return false;
    }
    try {
        return submitter_(command);
    } catch (...) {
        return false;
    }
}

void MetronomeTransportController::clearEngine() noexcept
{
    grid_.clearEngine();
    recording_schedule_revision_ = 0;
}

void MetronomeTransportController::setLocalState(bool running) noexcept
{
    local_running_ = running;
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
