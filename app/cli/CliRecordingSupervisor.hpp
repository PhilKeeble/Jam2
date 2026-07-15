#pragma once

#include "engine.hpp"
#include "metronome.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace jam2::cli {

struct RecordingSidecarContext {
    bool metronome_enabled = false;
    int bpm = 120;
    double metronome_level = 1.0;
    double remote_level = 1.0;
    double send_level = 1.0;
    bool local_monitor_enabled = false;
    double local_monitor_level = 0.0;
    std::string metronome_mode;
    std::string test_input;
    std::uint64_t metronome_epoch_sample_time = 0;
    bool metronome_epoch_valid = false;
    jam2::metronome::PatternSnapshot pattern;
    bool sample_time_playout = false;
    std::size_t playout_delay_frames = 0;
};

class CliRecordingSupervisor {
public:
    void start(
        jam2::Engine* engine,
        const std::optional<std::filesystem::path>& folder);
    void finalize(
        jam2::Engine* engine,
        const RecordingSidecarContext& context);

private:
    static void write_sidecar(
        const std::filesystem::path& folder,
        const jam2::EngineRecordingSnapshot& stats,
        int sample_rate,
        const RecordingSidecarContext& context);

    std::mutex mutex_;
};

bool restart_prepared_source(
    jam2::Engine* engine,
    std::uint64_t target_frame);

} // namespace jam2::cli

