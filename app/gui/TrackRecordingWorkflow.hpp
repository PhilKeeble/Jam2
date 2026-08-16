#pragma once

#include "ApplicationRuntime.hpp"
#include "MetronomeTransportController.hpp"
#include "PlaybackGrid.hpp"

#include "engine.hpp"

#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace jam2::gui {

std::uint64_t recording_count_in_start_beat(
    std::uint64_t absoluteBeat,
    std::uint64_t rawCurrentFrame,
    std::uint64_t nextBeatRawFrame,
    int sampleRate) noexcept;

bool recording_grid_ready_for_count_in(bool epochValid) noexcept;

std::uint64_t global_transport_elapsed_frames(
    bool playing,
    bool engineAnchored,
    std::uint64_t rawCurrentFrame,
    std::uint64_t actualStartFrame) noexcept;

std::uint64_t next_safe_grid_beat_raw_frame(
    const PlaybackGrid::Position& position) noexcept;

std::uint64_t synced_recording_countdown_beat(
    const PlaybackGrid::Position& position,
    int beatsPerBar) noexcept;

bool prepared_attach_has_applied(
    std::uint64_t pendingTargetFrame,
    std::uint64_t engineFrame,
    std::uint64_t preparedScheduledStartFrame) noexcept;

int resolve_active_sample_rate(
    int sessionSampleRate,
    double engineSampleRate,
    int configuredSampleRate) noexcept;

bool sample_rate_matches_engine(
    int expectedSampleRate,
    double engineSampleRate) noexcept;

}

class TrackRecordingWorkflow {
public:
    enum class CaptureMode {
        Input,
        CurrentJam,
        Loopback,
    };

    enum class CountdownPhase {
        Hidden,
        WaitingForBeat,
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
        int sampleRate = 0;
    };

    struct PreparedAttachPlan {
        bool alignToTransport = false;
        std::uint64_t targetFrame = 0;
        std::uint64_t sourceFrame = 0;
    };

    using CommandSubmitter = std::function<bool(const jam2::EngineCommand&)>;
    using SnapshotProvider = std::function<jam2::EngineSnapshot()>;

    explicit TrackRecordingWorkflow(ApplicationRuntime& runtime)
        : TrackRecordingWorkflow(
            [&runtime](const jam2::EngineCommand& command) {
                return runtime.submit(command);
            },
            [&runtime] { return runtime.engineSnapshot(); })
    {}
    TrackRecordingWorkflow(
        CommandSubmitter submitter,
        SnapshotProvider snapshotProvider);

    bool seekPrepared(std::uint64_t sourceFrame, std::uint64_t targetFrame) noexcept;
    bool setPreparedLoop(
        bool enabled,
        std::uint64_t startFrame = 0,
        std::uint64_t endFrame = 0) noexcept;
    bool restartPrepared(
        const PlaybackGrid::Position& position,
        bool localOnly = false,
        int countInBars = 0,
        int beatsPerBar = 4) noexcept;
    bool restartGlobalTransport(
        const PlaybackGrid::Position& position,
        bool localOnly = false,
        int countInBars = 0,
        int beatsPerBar = 4) noexcept;
    bool scheduleBankRestart(
        std::uint64_t targetFrame,
        std::uint64_t musicalFrame,
        bool preparedAvailable) noexcept;
    bool stopPrepared(
        std::uint64_t targetFrame,
        std::uint64_t musicalFrame,
        bool localOnly = false) noexcept;
    void noteManualPreparedSeek(qint64 sourceFrame, qint64 engineFrame) noexcept;
    void notePreparedAttachScheduled(std::uint64_t targetFrame) noexcept;
    void cancelPreparedAttach() noexcept;
    qint64 currentAudiblePositionMs(
        const PlaybackGrid::Position& enginePosition,
        qint64 durationMs) const noexcept;
    qint64 currentTransportPositionMs(
        const PlaybackGrid::Position& enginePosition,
        qint64 durationMs) const noexcept;
    PreparedAttachPlan preparedAttachPlan(
        const PlaybackGrid::Position& enginePosition,
        qint64 preparedFrames) const noexcept;

    void consumeSnapshot(
        const jam2::EngineSnapshot& snapshot,
        const MetronomeTransportController::SnapshotUpdate& transportUpdate) noexcept;
    std::optional<int> takeReadyPendingCountIn(const jam2::EngineSnapshot& snapshot) noexcept;

    bool startInputTake(
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
        QString& error);
    bool startInputTakeAtSchedule(
        const QString& outputPath,
        bool transientOutput,
        int expectedSampleRate,
        std::uint64_t countdownStartFrame,
        std::uint64_t targetFrame,
        std::uint64_t targetMusicalFrame,
        std::uint64_t durationFrames,
        bool includePrepared,
        bool includeMetronome,
        QString& error);
    bool scheduleRecordingTransport(
        std::uint64_t countdownStartFrame,
        std::uint64_t targetFrame,
        std::uint64_t targetMusicalFrame,
        bool localOnly = false) noexcept;
    bool stopInputTake(std::uint64_t targetFrame) noexcept;
    TrackTakeCompletion consumeTrackTakeEvent(const jam2::EngineEvent& event);

    void waitForCountIn(
        int bars,
        bool stopMetronomeAtStart) noexcept;
    void clearRecordingSchedule() noexcept;
    CountdownPresentation countdown(const PlaybackGrid::Position& position) noexcept;

    void beginLoopbackCapture(
        const QString& outputPath,
        bool transientOutput,
        int sampleRate);
    QString finishLoopbackCapture(const QString& outputPath);
    QString failLoopbackCapture();
    QString abandonPendingCapture();

    void armLane(
        int bankIndex,
        int laneIndex,
        CaptureMode mode,
        bool includePrepared = false,
        bool includeMetronome = false) noexcept;
    void disarmLane() noexcept;
    bool laneArmed() const noexcept;
    bool laneArmedAt(int bankIndex, int laneIndex) const noexcept;
    int armedBank() const noexcept { return armed_bank_; }
    int armedLane() const noexcept { return armed_lane_; }
    CaptureMode captureMode() const noexcept { return capture_mode_; }
    bool includePreparedInTake() const noexcept { return include_prepared_in_take_; }
    bool includeMetronomeInTake() const noexcept { return include_metronome_in_take_; }

    bool startJamRecording(const QString& folder);
    bool stopJamRecording() noexcept;
    bool consumeJamRecordingEvent(const jam2::EngineEvent& event) noexcept;
    void clearJamRecordingState() noexcept;

    void clearProjectCapture() noexcept;
    void clearSessionSchedule() noexcept;

    const QString& lastCapturePath() const noexcept { return last_capture_path_; }
    void setLastCapturePath(const QString& path) { last_capture_path_ = path; }
    void clearLastCapturePath() { last_capture_path_.clear(); }
    int lastCaptureSampleRate() const noexcept { return last_capture_sample_rate_; }
    const QString& pendingTransientCapturePath() const noexcept { return pending_transient_capture_path_; }
    bool inputTakeActive() const noexcept { return input_take_active_; }
    bool preparedPlaying() const noexcept { return prepared_playing_; }
    bool globalTransportRequestedPlaying() const noexcept {
        return global_transport_requested_playing_;
    }
    bool globalTransportPlaying() const noexcept {
        return global_transport_playing_;
    }
    std::uint64_t globalTransportStartFrame() const noexcept {
        return global_transport_start_frame_;
    }
    std::uint64_t globalTransportTimelineStartFrame() const noexcept {
        return pending_global_transport_start_frame_ > 0
            ? pending_global_transport_start_frame_
            : global_transport_start_frame_;
    }
    std::uint64_t globalTransportCountdownStartFrame() const noexcept {
        return pending_global_transport_countdown_start_frame_;
    }
    std::uint64_t preparedActualStartFrame() const noexcept {
        return prepared_actual_start_frame_;
    }
    bool preparedAttachPending() const noexcept {
        return pending_prepared_attach_target_frame_ != 0;
    }
    std::uint64_t recordingStartFrame() const noexcept { return recording_start_frame_; }
    int preparedSampleRate() const noexcept { return prepared_sample_rate_; }
    std::uint64_t appliedLatencyFrames() const noexcept { return applied_latency_frames_; }
    std::uint32_t inputLatencyFrames() const noexcept { return input_latency_frames_; }
    std::uint32_t outputLatencyFrames() const noexcept { return output_latency_frames_; }
    std::uint64_t sourceLatencyFrames() const noexcept { return source_latency_frames_; }
    int latencySampleRate() const noexcept { return latency_sample_rate_; }
    bool jamRecordingActive() const noexcept { return jam_recording_active_; }
    const QString& jamRecordingFolder() const noexcept { return jam_recording_folder_; }

private:
    bool scheduleGlobalTransportStart(
        std::uint64_t countdownStartFrame,
        std::uint64_t targetFrame,
        std::uint64_t musicalFrame,
        bool localOnly = false) noexcept;
    void clearGlobalTransport() noexcept;
    bool submit(
        jam2::EngineCommand command,
        std::uint64_t* submittedCookie = nullptr) noexcept;
    bool submitTakeCommand(jam2::EngineCommand command) noexcept;
    bool ownsTakeCommandCookie(std::uint64_t cookie) const noexcept;
    void clearTakeCommandCookies() noexcept;
    void clearActiveTakeState() noexcept;
    bool armTrackTake(
        const QString& id,
        const QString& output,
        bool includePrepared,
        bool includeMetronome) noexcept;
    bool startTrackTake(
        std::uint64_t targetFrame,
        std::uint64_t durationFrames) noexcept;
    void cancelTrackTake() noexcept;
    bool startTrackTakeAtSchedule(
        std::uint64_t countdownStartFrame,
        std::uint64_t targetFrame,
        std::uint64_t targetMusicalFrame,
        std::uint64_t durationFrames,
        bool transportLocalOnly,
        QString& error) noexcept;

    CommandSubmitter command_submitter_;
    SnapshotProvider snapshot_provider_;
    std::uint64_t command_cookie_ = 0;

    QString last_capture_path_;
    QString pending_transient_capture_path_;
    QString active_take_id_;
    int active_take_sample_rate_ = 0;
    int last_capture_sample_rate_ = 0;
    bool input_take_active_ = false;
    std::array<std::uint64_t, 16> active_take_command_cookies_{};
    std::size_t active_take_command_cookie_count_ = 0;

    int armed_bank_ = -1;
    int armed_lane_ = -1;
    CaptureMode capture_mode_ = CaptureMode::Input;
    bool include_prepared_in_take_ = false;
    bool include_metronome_in_take_ = false;

    qint64 prepared_source_frame_ = 0;
    qint64 prepared_engine_frame_ = 0;
    std::uint64_t prepared_actual_start_frame_ = 0;
    std::uint64_t pending_prepared_attach_target_frame_ = 0;
    bool prepared_playing_ = false;
    int prepared_sample_rate_ = 48000;
    std::uint64_t observed_transport_revision_ = 0;
    std::uint64_t observed_transport_commit_count_ = 0;
    std::uint64_t global_transport_start_frame_ = 0;
    std::uint64_t pending_global_transport_countdown_start_frame_ = 0;
    std::uint64_t pending_global_transport_start_frame_ = 0;
    std::uint64_t pending_global_transport_stop_frame_ = 0;
    bool global_transport_requested_playing_ = false;
    bool global_transport_playing_ = false;

    int pending_count_in_bars_ = 0;
    bool stop_metronome_at_recording_start_ = false;
    std::uint64_t recording_countdown_start_frame_ = 0;
    std::uint64_t recording_start_frame_ = 0;

    std::uint32_t input_latency_frames_ = 0;
    std::uint32_t output_latency_frames_ = 0;
    std::uint64_t source_latency_frames_ = 0;
    std::uint64_t applied_latency_frames_ = 0;
    int latency_sample_rate_ = 48000;

    QString jam_recording_folder_;
    bool jam_recording_active_ = false;
    bool jam_recording_confirmed_active_ = false;
    std::uint64_t jam_recording_start_cookie_ = 0;
    std::uint64_t jam_recording_stop_cookie_ = 0;
};
