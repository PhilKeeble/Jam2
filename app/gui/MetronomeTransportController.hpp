#pragma once

#include "ApplicationRuntime.hpp"
#include "PlaybackGrid.hpp"

#include "engine.hpp"

#include <cstdint>

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
