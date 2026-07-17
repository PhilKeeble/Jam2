#pragma once

#include "ApplicationRuntime.hpp"
#include "PlaybackGrid.hpp"

#include "engine.hpp"

#include <array>
#include <cstdint>
#include <optional>

class TapTempoTracker {
public:
    std::optional<int> tap(std::int64_t elapsedMs) noexcept;
    void reset() noexcept;

private:
    static constexpr std::size_t kIntervalCapacity = 4;
    std::array<std::int64_t, kIntervalCapacity> intervals_ms_{};
    std::int64_t last_tap_ms_ = 0;
    std::size_t interval_count_ = 0;
    bool has_last_tap_ = false;
};

class MetronomeTransportController {
public:
    struct SnapshotUpdate {
        bool recordingScheduleAdvanced = false;
        std::uint64_t recordingCountdownStartFrame = 0;
        std::uint64_t recordingStartFrame = 0;
    };

    explicit MetronomeTransportController(ApplicationRuntime& runtime) noexcept;

    PlaybackGrid& grid() noexcept { return grid_; }
    const PlaybackGrid& grid() const noexcept { return grid_; }

    SnapshotUpdate consume(const jam2::EngineSnapshot& snapshot);
    bool submit(const jam2::EngineCommand& command) noexcept;
    void clearEngine() noexcept;

    bool localRunning() const noexcept { return local_running_; }
    bool localLeader() const noexcept { return local_leader_; }
    void setLocalState(bool running, bool leader) noexcept;

    bool applyingRemoteSettings() const noexcept { return applying_remote_settings_; }
    void setApplyingRemoteSettings(bool applying) noexcept;
    static bool allowsLocalGridMutation(bool applyingRemoteSettings) noexcept;
    static bool transportActionResetsGridEpoch(
        jam2::EngineTransportAction action) noexcept;
    bool localGridMutationAllowed() const noexcept {
        return allowsLocalGridMutation(applying_remote_settings_);
    }

private:
    ApplicationRuntime& runtime_;
    PlaybackGrid grid_;
    std::uint64_t recording_schedule_revision_ = 0;
    bool local_running_ = false;
    bool local_leader_ = false;
    bool applying_remote_settings_ = false;
};
