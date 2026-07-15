#include "CliRecordingSupervisor.hpp"

#include "common.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace jam2::cli {
namespace {

std::string json_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

void CliRecordingSupervisor::start(
    jam2::Engine* engine,
    const std::optional<std::filesystem::path>& folder)
{
    if (engine == nullptr || !folder) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StartJamRecording;
    if (!jam2::engine_command_set_text(command, folder->string()) ||
        !engine->submit(command)) {
        std::cerr << "record jam start command was rejected\n";
    }
}

void CliRecordingSupervisor::finalize(
    jam2::Engine* engine,
    const RecordingSidecarContext& context)
{
    if (engine == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = engine->snapshot().jam_recording;
    const auto cold_before = engine->coldSnapshot();
    if (!before.active) {
        return;
    }

    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StopJamRecording;
    const bool accepted = engine->submit(command);
    const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;
    auto after = before;
    while (accepted && jam2::monotonic_us() < deadline) {
        after = engine->snapshot().jam_recording;
        if (!after.active) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!cold_before.recording_folder.empty()) {
        write_sidecar(
            std::filesystem::path(cold_before.recording_folder),
            after,
            cold_before.recording_sample_rate,
            context);
    }
    if (!accepted) {
        std::cerr << "record jam finalization command was rejected\n";
    }
}

void CliRecordingSupervisor::write_sidecar(
    const std::filesystem::path& folder,
    const jam2::EngineRecordingSnapshot& stats,
    int sample_rate,
    const RecordingSidecarContext& context)
{
    if (folder.empty()) {
        return;
    }
    std::filesystem::create_directories(folder);
    std::ofstream out(folder / "recording.json", std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "record jam sidecar failed: "
                  << (folder / "recording.json").string() << "\n";
        return;
    }
    const auto& pattern = context.pattern;
    out << "{\n"
        << "  \"format\": \"pcm16_mono_wav\",\n"
        << "  \"sample_rate\": " << sample_rate << ",\n"
        << "  \"stems\": [\"mix.wav\", \"my-input.wav\", \"their-input.wav\", \"inputs-mix.wav\", \"metronome.wav\"],\n"
        << "  \"recording_folder\": \"" << json_escape(folder.string()) << "\",\n"
        << "  \"start_audio_frame\": " << stats.start_frame << ",\n"
        << "  \"stop_audio_frame\": " << stats.stop_frame << ",\n"
        << "  \"frames_queued\": " << stats.frames_queued << ",\n"
        << "  \"frames_written\": " << stats.frames_written << ",\n"
        << "  \"dropped_frames\": " << stats.dropped_frames << ",\n"
        << "  \"drop_events\": " << stats.drop_events << ",\n"
        << "  \"writer_errors\": " << stats.writer_errors << ",\n"
        << "  \"queue_capacity_frames\": " << stats.queue_capacity_frames << ",\n"
        << "  \"metronome\": \"" << (context.metronome_enabled ? "on" : "off") << "\",\n"
        << "  \"bpm\": " << context.bpm << ",\n"
        << "  \"metronome_level\": " << context.metronome_level << ",\n"
        << "  \"remote_level\": " << context.remote_level << ",\n"
        << "  \"send_level\": " << context.send_level << ",\n"
        << "  \"local_monitor\": \"" << (context.local_monitor_enabled ? "on" : "off") << "\",\n"
        << "  \"local_monitor_level\": " << context.local_monitor_level << ",\n"
        << "  \"metronome_mode\": \"" << json_escape(context.metronome_mode) << "\",\n"
        << "  \"test_input\": \"" << json_escape(context.test_input) << "\",\n"
        << "  \"metronome_epoch_sample_time\": " << context.metronome_epoch_sample_time << ",\n"
        << "  \"metronome_epoch_valid\": " << (context.metronome_epoch_valid ? "true" : "false") << ",\n"
        << "  \"metronome_beats_per_bar\": " << pattern.beats_per_bar << ",\n"
        << "  \"metronome_division\": " << pattern.division << ",\n"
        << "  \"metronome_step_count\": " << pattern.step_count << ",\n"
        << "  \"metronome_play_mask_low\": " << pattern.play_mask_low << ",\n"
        << "  \"metronome_play_mask_high\": " << pattern.play_mask_high << ",\n"
        << "  \"metronome_accent_mask_low\": " << pattern.accent_mask_low << ",\n"
        << "  \"metronome_accent_mask_high\": " << pattern.accent_mask_high << ",\n"
        << "  \"sample_time_playout\": " << (context.sample_time_playout ? "true" : "false") << ",\n"
        << "  \"playout_delay_frames\": " << context.playout_delay_frames << "\n"
        << "}\n";
}

bool restart_prepared_source(
    jam2::Engine* engine,
    std::uint64_t target_frame)
{
    if (engine == nullptr) {
        return false;
    }
    jam2::EngineCommand seek;
    seek.type = jam2::EngineCommandType::PreparedSeek;
    seek.frame = target_frame;
    jam2::EngineCommand play;
    play.type = jam2::EngineCommandType::PreparedPlay;
    play.frame = target_frame;
    return engine->submit(seek) && engine->submit(play);
}

} // namespace jam2::cli

