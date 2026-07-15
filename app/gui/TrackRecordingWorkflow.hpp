#pragma once

#include "ApplicationRuntime.hpp"
#include "MetronomeTransportController.hpp"
#include "PlaybackGrid.hpp"

#include "engine.hpp"

#include <QString>

#include <cstdint>
#include <optional>

namespace jam2::gui {

std::uint64_t recording_count_in_bar_beat(
    std::uint64_t absoluteBeat,
    int beatsPerBar,
    std::uint64_t rawCurrentFrame,
    std::uint64_t nextBarRawFrame,
    int sampleRate) noexcept;

bool recording_grid_ready_for_count_in(
    bool metronomeEnabled,
    bool epochValid,
    std::uint64_t epochFrame,
    bool requireFreshEpoch,
    std::uint64_t priorEpochFrame) noexcept;

}

class TrackRecordingWorkflow {
public:
    enum class CaptureMode {
        Input,
        Loopback,
    };

    enum class CountdownPhase {
        Hidden,
        WaitingForBar,
        Counting,
        Recording,
    };

    struct CountdownPresentation {
        CountdownPhase phase = CountdownPhase::Hidden;
        int remainingBeats = 0;
        bool stopMetronome = false;
    };

    struct TrackTakeCompletion {
        bool handled = false;
        bool ok = false;
        QString takeId;
        QString wavPath;
        QString error;
        std::uint64_t frames = 0;
    };

    explicit TrackRecordingWorkflow(ApplicationRuntime& runtime) noexcept;

    bool seekPrepared(std::uint64_t sourceFrame, std::uint64_t targetFrame) noexcept;
    bool setPreparedLoop(
        bool enabled,
        std::uint64_t startFrame = 0,
        std::uint64_t endFrame = 0) noexcept;
    bool restartPrepared(
        const PlaybackGrid::Position& position,
        int beatsPerBar,
        bool publishTransport) noexcept;
    bool stopPrepared(
        std::uint64_t targetFrame,
        std::uint64_t musicalFrame,
        bool publishTransport) noexcept;
    void noteManualPreparedSeek(qint64 sourceFrame, qint64 engineFrame) noexcept;
    qint64 currentAudiblePositionMs(
        const PlaybackGrid::Position& enginePosition,
        qint64 durationMs) const noexcept;

    void consumeSnapshot(
        const jam2::EngineSnapshot& snapshot,
        const MetronomeTransportController::SnapshotUpdate& transportUpdate) noexcept;
    std::optional<int> takeReadyPendingCountIn(const jam2::EngineSnapshot& snapshot) noexcept;

    bool startInputTake(
        const QString& outputPath,
        bool transientOutput,
        std::uint64_t targetFrame,
        std::optional<int> countInBars,
        const PlaybackGrid::Position& position,
        int beatsPerBar,
        QString& error);
    bool stopInputTake(std::uint64_t targetFrame) noexcept;
    TrackTakeCompletion consumeTrackTakeEvent(const jam2::EngineEvent& event);

    void waitForCountIn(
        int bars,
        bool stopMetronomeAtStart,
        bool requireFreshEpoch = false,
        std::uint64_t priorEpochFrame = 0) noexcept;
    void clearRecordingSchedule() noexcept;
    CountdownPresentation countdown(const PlaybackGrid::Position& position) noexcept;

    void beginLoopbackCapture(const QString& outputPath, bool transientOutput);
    QString finishLoopbackCapture(const QString& outputPath);
    QString abandonPendingCapture();

    void armLane(int bankIndex, int laneIndex, CaptureMode mode) noexcept;
    void disarmLane() noexcept;
    bool laneArmed() const noexcept;
    bool laneArmedAt(int bankIndex, int laneIndex) const noexcept;
    int armedBank() const noexcept { return armed_bank_; }
    int armedLane() const noexcept { return armed_lane_; }
    CaptureMode captureMode() const noexcept { return capture_mode_; }

    bool startJamRecording(const QString& folder);
    bool stopJamRecording() noexcept;
    bool consumeJamRecordingEvent(const jam2::EngineEvent& event) noexcept;
    void clearJamRecordingState() noexcept;

    void clearProjectCapture() noexcept;
    void clearSessionSchedule() noexcept;

    const QString& lastCapturePath() const noexcept { return last_capture_path_; }
    void setLastCapturePath(const QString& path) { last_capture_path_ = path; }
    void clearLastCapturePath() { last_capture_path_.clear(); }
    const QString& pendingTransientCapturePath() const noexcept { return pending_transient_capture_path_; }
    bool inputTakeActive() const noexcept { return input_take_active_; }
    bool preparedPlaying() const noexcept { return prepared_playing_; }
    int preparedSampleRate() const noexcept { return prepared_sample_rate_; }
    std::uint64_t appliedLatencyFrames() const noexcept { return applied_latency_frames_; }
    std::uint32_t inputLatencyFrames() const noexcept { return input_latency_frames_; }
    std::uint32_t outputLatencyFrames() const noexcept { return output_latency_frames_; }
    int latencySampleRate() const noexcept { return latency_sample_rate_; }
    bool jamRecordingActive() const noexcept { return jam_recording_active_; }
    const QString& jamRecordingFolder() const noexcept { return jam_recording_folder_; }

private:
    bool submit(jam2::EngineCommand command) noexcept;
    bool armTrackTake(const QString& id, const QString& output) noexcept;
    bool startTrackTake(std::uint64_t targetFrame) noexcept;
    bool startTrackTakeQuantized(
        int countInBars,
        const PlaybackGrid::Position& position,
        int beatsPerBar,
        QString& error) noexcept;

    ApplicationRuntime& runtime_;
    std::uint64_t command_cookie_ = 0;

    QString last_capture_path_;
    QString pending_transient_capture_path_;
    QString active_take_id_;
    bool input_take_active_ = false;

    int armed_bank_ = -1;
    int armed_lane_ = -1;
    CaptureMode capture_mode_ = CaptureMode::Input;

    qint64 prepared_source_frame_ = 0;
    qint64 prepared_engine_frame_ = 0;
    bool prepared_playing_ = false;
    int prepared_sample_rate_ = 48000;

    int pending_count_in_bars_ = 0;
    bool pending_count_in_requires_fresh_epoch_ = false;
    std::uint64_t pending_count_in_prior_epoch_frame_ = 0;
    bool stop_metronome_at_recording_start_ = false;
    std::uint64_t recording_countdown_start_frame_ = 0;
    std::uint64_t recording_start_frame_ = 0;

    std::uint32_t input_latency_frames_ = 0;
    std::uint32_t output_latency_frames_ = 0;
    std::uint64_t applied_latency_frames_ = 0;
    int latency_sample_rate_ = 48000;

    QString jam_recording_folder_;
    bool jam_recording_active_ = false;
};
