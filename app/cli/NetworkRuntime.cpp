#include <exception>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <netdb.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "audio_device.hpp"
#include "common.hpp"
#include "engine.hpp"
#include "metronome.hpp"
#include "network_session.hpp"
#include "pcm16_wav.hpp"
#include "peer_stream.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include "stun.hpp"
#include "tuning_profile.hpp"
#include "udp_socket.hpp"
#include "CliRuntime.hpp"
#include "CliOptions.hpp"
#include "CliRecordingSupervisor.hpp"
#include "CliStats.hpp"
#include "RuntimeContracts.hpp"

namespace {

using MetronomeMode = Jam2MetronomeMode;
using TestInputMode = Jam2TestInputMode;
using OsPriorityMode = Jam2OsPriorityMode;
using Options = Jam2RuntimeOptions;
using namespace jam2::cli;
using namespace jam2::cli::stats;



void apply_socket_options(jam2::UdpSocket& socket, const Options& options)
{
    if (options.socket_send_buffer) {
        socket.set_send_buffer_size(*options.socket_send_buffer);
    }
    if (options.socket_recv_buffer) {
        socket.set_recv_buffer_size(*options.socket_recv_buffer);
    }
}

void print_socket_options(const jam2::UdpSocket& socket)
{
    std::cout << "UDP send buffer bytes: " << socket.send_buffer_size() << "\n";
    std::cout << "UDP receive buffer bytes: " << socket.recv_buffer_size() << "\n";
}














struct OutstandingPing {
    std::uint32_t sequence = 0;
    std::uint64_t send_time_us = 0;
    bool active = false;
};

struct RuntimeState {
    mutable std::mutex track_take_mutex;
    mutable std::mutex transport_mutex;
    std::atomic<bool> quit{false};
    std::atomic<bool> stats_enabled{false};
    std::atomic<bool> print_stats{false};
    std::atomic<bool> print_status{false};
    std::atomic<bool> metronome{false};
    std::atomic<int> bpm{120};
    std::atomic<int> metronome_beats_per_bar{4};
    std::atomic<int> metronome_division{1};
    std::atomic<int> metronome_step_count{4};
    std::atomic<std::uint64_t> metronome_play_mask_low{0x0fULL};
    std::atomic<std::uint64_t> metronome_play_mask_high{0};
    std::atomic<std::uint64_t> metronome_accent_mask_low{0x01ULL};
    std::atomic<std::uint64_t> metronome_accent_mask_high{0};
    std::atomic<int> metronome_level_ppm{1000000};
    std::atomic<int> remote_level_ppm{1000000};
    std::atomic<int> send_level_ppm{1000000};
    std::atomic<bool> local_monitor{false};
    std::atomic<int> local_monitor_level_ppm{250000};
    std::atomic<int> metronome_mode{0};
    std::atomic<bool> leader_audio_local_click{false};
    std::atomic<bool> metronome_local_authority{false};
    std::atomic<std::uint64_t> metronome_epoch_sample_time{0};
    std::atomic<bool> metronome_epoch_valid{false};
    std::atomic<std::int64_t> metronome_render_offset_frames{0};
    std::atomic<std::uint64_t> metronome_revision{0};
    std::atomic<std::uint64_t> metronome_epoch_revision{0};
    // Changed only by local controls. Network-applied grid state deliberately
    // does not touch this counter, preventing authority update feedback loops.
    std::atomic<std::uint64_t> grid_request_sequence{0};
    std::atomic<std::uint64_t> transport_revision{0};
    std::atomic<std::uint64_t> transport_network_revision{0};
    std::atomic<std::uint64_t> transport_network_target_raw_frame{0};
    std::atomic<int> transport_network_action{0};
    std::atomic<std::uint64_t> transport_target_raw_frame{0};
    std::atomic<std::uint64_t> transport_target_musical_frame{0};
    std::atomic<std::uint64_t> transport_countdown_start_frame{0};
    std::atomic<int> transport_action{0};
    std::atomic<bool> transport_pending{false};
};

void request_grid_revision(RuntimeState& state) noexcept
{
    state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
    state.grid_request_sequence.fetch_add(1, std::memory_order_release);
}

int ppm_from_gain(double value)
{
    return static_cast<int>(std::clamp(value, 0.0, 4.0) * 1000000.0);
}

int ratio_to_ppm(double ratio)
{
    return static_cast<int>(std::clamp(ratio, 0.5, 2.0) * 1000000.0);
}



double gain_from_ppm(int value)
{
    return static_cast<double>(std::clamp(value, 0, 4000000)) / 1000000.0;
}

int pcm24_peak_ppm(std::span<const std::int32_t> samples)
{
    std::int32_t peak = 0;
    for (std::int32_t sample : samples) {
        peak = std::max<std::int32_t>(peak, std::abs(sample));
    }
    const double normalized = static_cast<double>(peak) / 8388607.0;
    return static_cast<int>(std::clamp(normalized, 0.0, 1.0) * 1000000.0);
}

void update_peak(std::atomic<int>& peak, int candidate)
{
    int current = peak.load(std::memory_order_relaxed);
    while (candidate > current &&
           !peak.compare_exchange_weak(current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void apply_send_level(std::span<std::int32_t> frames, int level_ppm)
{
    if (level_ppm == 1000000) {
        return;
    }
    const double level = gain_from_ppm(level_ppm);
    for (std::int32_t& sample : frames) {
        const double scaled = static_cast<double>(sample) * level;
        sample = static_cast<std::int32_t>(std::clamp(scaled, -8388608.0, 8388607.0));
    }
}

jam2::metronome::PatternSnapshot metronome_pattern_from_runtime(const RuntimeState& runtime)
{
    return jam2::metronome::sanitize({
        runtime.bpm.load(std::memory_order_relaxed),
        runtime.metronome_beats_per_bar.load(std::memory_order_relaxed),
        runtime.metronome_division.load(std::memory_order_relaxed),
        runtime.metronome_step_count.load(std::memory_order_relaxed),
        runtime.metronome_play_mask_low.load(std::memory_order_relaxed),
        runtime.metronome_play_mask_high.load(std::memory_order_relaxed),
        runtime.metronome_accent_mask_low.load(std::memory_order_relaxed),
        runtime.metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
}

bool same_metronome_pattern(
    const jam2::metronome::PatternSnapshot& left,
    const jam2::metronome::PatternSnapshot& right) noexcept
{
    return left.bpm == right.bpm &&
        left.beats_per_bar == right.beats_per_bar &&
        left.division == right.division &&
        left.step_count == right.step_count &&
        left.play_mask_low == right.play_mask_low &&
        left.play_mask_high == right.play_mask_high &&
        left.accent_mask_low == right.accent_mask_low &&
        left.accent_mask_high == right.accent_mask_high;
}

void store_metronome_pattern(RuntimeState& runtime, const jam2::metronome::PatternSnapshot& pattern)
{
    const jam2::metronome::PatternSnapshot sanitized = jam2::metronome::sanitize(pattern);
    runtime.bpm.store(sanitized.bpm, std::memory_order_relaxed);
    runtime.metronome_beats_per_bar.store(sanitized.beats_per_bar, std::memory_order_relaxed);
    runtime.metronome_division.store(sanitized.division, std::memory_order_relaxed);
    runtime.metronome_step_count.store(sanitized.step_count, std::memory_order_relaxed);
    runtime.metronome_play_mask_low.store(sanitized.play_mask_low, std::memory_order_relaxed);
    runtime.metronome_play_mask_high.store(sanitized.play_mask_high, std::memory_order_relaxed);
    runtime.metronome_accent_mask_low.store(sanitized.accent_mask_low, std::memory_order_relaxed);
    runtime.metronome_accent_mask_high.store(sanitized.accent_mask_high, std::memory_order_relaxed);
}

bool parse_u64(std::string_view token, std::uint64_t& value)
{
    if (token.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    try {
        value = std::stoull(std::string(token), &consumed, 0);
    } catch (const std::exception&) {
        return false;
    }
    return consumed == token.size();
}

struct EngineControlMirror {
    bool initialized = false;
    jam2::EngineSnapshot snapshot;
};

void sync_engine_control(
    const RuntimeState& runtime,
    jam2::Engine* engine,
    EngineControlMirror& mirror)
{
    if (engine == nullptr) {
        return;
    }
    if (!mirror.initialized) {
        mirror.snapshot = engine->snapshot();
        mirror.initialized = true;
    }
    auto submit = [&](jam2::EngineCommand command) {
        return engine->submit(command);
    };
    const bool enabled = runtime.metronome.load(std::memory_order_relaxed);
    if (mirror.snapshot.metronome_enabled != enabled) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeEnabled; command.enabled = enabled;
        if (submit(command)) mirror.snapshot.metronome_enabled = enabled;
    }
    const auto pattern = metronome_pattern_from_runtime(runtime);
    const auto& previous = mirror.snapshot.metronome_pattern;
    if (pattern.bpm != previous.bpm || pattern.beats_per_bar != previous.beats_per_bar ||
        pattern.division != previous.division || pattern.play_mask_low != previous.play_mask_low ||
        pattern.play_mask_high != previous.play_mask_high || pattern.accent_mask_low != previous.accent_mask_low ||
        pattern.accent_mask_high != previous.accent_mask_high) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomePattern; command.pattern = pattern;
        if (submit(command)) mirror.snapshot.metronome_pattern = pattern;
    }
    const auto sync_value = [&](jam2::EngineCommandType type, int value, int& cached) {
        if (value == cached) return;
        jam2::EngineCommand command; command.type = type; command.value = value;
        if (submit(command)) cached = value;
    };
    sync_value(jam2::EngineCommandType::SetMetronomeLevel, runtime.metronome_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.metronome_level_ppm);
    sync_value(jam2::EngineCommandType::SetRemoteLevel, runtime.remote_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.remote_level_ppm);
    sync_value(jam2::EngineCommandType::SetSendLevel, runtime.send_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.send_level_ppm);

    sync_value(jam2::EngineCommandType::SetLocalMonitorLevel, runtime.local_monitor_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.local_monitor_level_ppm);
    const bool monitor = runtime.local_monitor.load(std::memory_order_relaxed);
    if (mirror.snapshot.local_monitor_enabled != monitor) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetLocalMonitorEnabled; command.enabled = monitor;
        if (submit(command)) mirror.snapshot.local_monitor_enabled = monitor;
    }
    const int mode = runtime.metronome_mode.load(std::memory_order_relaxed);
    if (static_cast<int>(mirror.snapshot.metronome_mode) != mode) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeMode; command.value = mode;
        if (submit(command)) mirror.snapshot.metronome_mode = static_cast<jam2::EngineMetronomeMode>(mode);
    }
    const bool leader_audio_local_click =
        runtime.leader_audio_local_click.load(std::memory_order_relaxed);
    if (mirror.snapshot.leader_audio_local_click != leader_audio_local_click) {
        jam2::EngineCommand command;
        command.type = jam2::EngineCommandType::SetLeaderAudioLocalClick;
        command.enabled = leader_audio_local_click;
        if (submit(command)) {
            mirror.snapshot.leader_audio_local_click = leader_audio_local_click;
        }
    }
    const std::uint64_t epoch = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    const bool epoch_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
    if (mirror.snapshot.metronome_epoch_frame != epoch || mirror.snapshot.metronome_epoch_valid != epoch_valid) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeEpoch; command.frame = epoch; command.enabled = epoch_valid;
        if (submit(command)) { mirror.snapshot.metronome_epoch_frame = epoch; mirror.snapshot.metronome_epoch_valid = epoch_valid; }
    }
    const std::int64_t offset = runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
    if (mirror.snapshot.metronome_render_offset_frames != offset) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeRenderOffset; command.signed_value = offset;
        if (submit(command)) mirror.snapshot.metronome_render_offset_frames = offset;
    }
}



std::uint64_t current_engine_frame(const jam2::Engine* engine)
{
    return engine != nullptr ? engine->snapshot().engine_frame : 0ULL;
}

void begin_metronome_epoch(
    RuntimeState& state,
    const jam2::Engine* engine,
    int sample_rate)
{
    const std::uint64_t lead_frames = static_cast<std::uint64_t>(std::max(1, sample_rate)) / 5ULL;
    state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    state.metronome_epoch_sample_time.store(
        current_engine_frame(engine) + lead_frames,
        std::memory_order_relaxed);
    state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
}

void hold_shared_grid_at_start(
    RuntimeState& state,
    jam2::Engine* engine)
{
    if (state.metronome_mode.load(std::memory_order_relaxed) !=
        metronome_mode_id(MetronomeMode::SharedGrid)) {
        return;
    }
    state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
    state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    if (engine != nullptr) {
        jam2::EngineCommand epoch; epoch.type = jam2::EngineCommandType::SetMetronomeEpoch; epoch.frame = 0; epoch.enabled = false;
        jam2::EngineCommand offset; offset.type = jam2::EngineCommandType::SetMetronomeRenderOffset; offset.signed_value = 0;
        (void)engine->submit(epoch);
        (void)engine->submit(offset);
    }
}

struct QuantizedSchedule {
    std::uint64_t countdown_start_raw_frame = 0;
    std::uint64_t target_raw_frame = 0;
    std::uint64_t target_musical_frame = 0;
};

std::uint64_t musical_frame_from_raw(std::uint64_t raw_frame, std::int64_t offset)
{
    if (offset < 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(-offset);
        return raw_frame > magnitude ? raw_frame - magnitude : 0ULL;
    }
    return raw_frame + static_cast<std::uint64_t>(offset);
}

std::uint64_t raw_frame_from_musical(std::uint64_t musical_frame, std::int64_t offset)
{
    if (offset >= 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(offset);
        return musical_frame > magnitude ? musical_frame - magnitude : 0ULL;

    }
    return musical_frame + static_cast<std::uint64_t>(-offset);

}

QuantizedSchedule next_bar_schedule(
    const RuntimeState& state,
    const jam2::Engine* engine,
    int sample_rate,
    int count_in_bars)
{
    const auto pattern = metronome_pattern_from_runtime(state);
    const std::uint64_t step_frames = jam2::metronome::step_interval_samples(

        static_cast<double>(std::max(1, sample_rate)), pattern.bpm, pattern.division);
    const std::uint64_t bar_frames = std::max<std::uint64_t>(
        1,
        step_frames * static_cast<std::uint64_t>(pattern.division) *
            static_cast<std::uint64_t>(pattern.beats_per_bar));
    const std::uint64_t raw_now = current_engine_frame(engine);
    const std::int64_t offset = state.metronome_render_offset_frames.load(std::memory_order_relaxed);
    const std::uint64_t musical_now = musical_frame_from_raw(raw_now, offset);
    const std::uint64_t epoch = state.metronome_epoch_valid.load(std::memory_order_relaxed)
        ? state.metronome_epoch_sample_time.load(std::memory_order_relaxed)
        : 0ULL;
    const std::uint64_t elapsed = musical_now >= epoch ? musical_now - epoch : 0ULL;
    std::uint64_t next_bar_musical = epoch + (elapsed / bar_frames + 1ULL) * bar_frames;
    const std::uint64_t minimum_lead_frames = static_cast<std::uint64_t>(std::max(1, sample_rate)) / 5ULL;
    if (raw_frame_from_musical(next_bar_musical, offset) <= raw_now + minimum_lead_frames) {
        next_bar_musical += bar_frames;
    }
    const std::uint64_t target_musical = next_bar_musical +
        static_cast<std::uint64_t>(std::max(0, count_in_bars)) * bar_frames;
    return {
        raw_frame_from_musical(next_bar_musical, offset),
        raw_frame_from_musical(target_musical, offset),
        target_musical,
    };
}

std::uint64_t publish_transport_schedule(
    RuntimeState& state,
    jam2::EngineTransportAction action,
    const QuantizedSchedule& schedule,
    bool share_with_peer)
{
    std::lock_guard<std::mutex> lock(state.transport_mutex);
    const std::uint64_t revision = state.transport_revision.load(std::memory_order_relaxed) + 1ULL;
    state.transport_target_raw_frame.store(schedule.target_raw_frame, std::memory_order_relaxed);
    state.transport_target_musical_frame.store(schedule.target_musical_frame, std::memory_order_relaxed);
    state.transport_countdown_start_frame.store(schedule.countdown_start_raw_frame, std::memory_order_relaxed);
    state.transport_action.store(static_cast<int>(action), std::memory_order_relaxed);
    state.transport_pending.store(true, std::memory_order_release);
    if (share_with_peer) {
        state.transport_network_target_raw_frame.store(schedule.target_raw_frame, std::memory_order_relaxed);
        state.transport_network_action.store(static_cast<int>(action), std::memory_order_relaxed);
        state.transport_network_revision.store(revision, std::memory_order_release);
    }
    state.transport_revision.store(revision, std::memory_order_release);
    return revision;
}

void commit_due_transport(RuntimeState& state, const jam2::Engine* engine)
{
    std::lock_guard<std::mutex> lock(state.transport_mutex);
    if (!state.transport_pending.load(std::memory_order_acquire) ||
        current_engine_frame(engine) < state.transport_target_raw_frame.load(std::memory_order_relaxed)) {
        return;
    }
    state.transport_pending.store(false, std::memory_order_release);
    const int action = state.transport_action.load(std::memory_order_relaxed);
    if (action == static_cast<int>(jam2::EngineTransportAction::TrackRestart) ||
        action == static_cast<int>(jam2::EngineTransportAction::RecordStart)) {
        state.metronome_epoch_sample_time.store(
            state.transport_target_musical_frame.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
    }
}

bool parse_frame_or_now(std::string_view text, const jam2::Engine* engine, std::uint64_t& out)
{
    if (text == "now" || text.empty()) {
        out = current_engine_frame(engine);
        return true;
    }
    return parse_u64(text, out);
}

void write_u64_le(std::span<std::uint8_t> payload, std::size_t offset, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        payload[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

std::uint64_t read_u64_le(std::span<const std::uint8_t> payload, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | payload[offset + static_cast<std::size_t>(i)];
    }
    return value;
}

enum class GridMessageKind : std::uint8_t {
    LegacyState = 0,
    Proposal = 1,
    Assignment = 2,
    AuthorityState = 3,
};

constexpr std::uint8_t kGridMessageMarker = 0x80;

std::array<std::uint8_t, 56> encode_metronome_payload(
    int bpm,
    std::uint64_t revision_or_request,
    std::uint64_t epoch_sample_time,
    jam2::metronome::PatternSnapshot pattern,
    GridMessageKind kind,
    std::uint8_t mode,
    jam2::GridRunState run_state)
{
    pattern = jam2::metronome::sanitize(pattern);
    std::array<std::uint8_t, 56> payload{};
    payload[0] = static_cast<std::uint8_t>(bpm & 0xff);
    payload[1] = static_cast<std::uint8_t>((bpm >> 8) & 0xff);
    payload[2] = static_cast<std::uint8_t>((bpm >> 16) & 0xff);
    payload[3] = static_cast<std::uint8_t>((bpm >> 24) & 0xff);
    write_u64_le(std::span<std::uint8_t>(payload), 4, revision_or_request);
    write_u64_le(std::span<std::uint8_t>(payload), 12, epoch_sample_time);
    payload[20] = static_cast<std::uint8_t>(
        kGridMessageMarker |
        (static_cast<std::uint8_t>(kind) & 0x03U) |
        ((mode & 0x03U) << 2U) |
        ((static_cast<std::uint8_t>(run_state) & 0x03U) << 4U));
    payload[21] = static_cast<std::uint8_t>(pattern.beats_per_bar);
    payload[22] = static_cast<std::uint8_t>(pattern.division);
    payload[23] = static_cast<std::uint8_t>(pattern.step_count);
    write_u64_le(std::span<std::uint8_t>(payload), 24, pattern.play_mask_low);
    write_u64_le(std::span<std::uint8_t>(payload), 32, pattern.play_mask_high);
    write_u64_le(std::span<std::uint8_t>(payload), 40, pattern.accent_mask_low);
    write_u64_le(std::span<std::uint8_t>(payload), 48, pattern.accent_mask_high);
    return payload;
}

struct MetronomePayload {
    int bpm = 120;
    std::uint64_t revision_or_request = 0;
    std::uint64_t epoch_sample_time = 0;
    jam2::metronome::PatternSnapshot pattern;
    bool has_pattern = false;
    GridMessageKind kind = GridMessageKind::LegacyState;
    std::uint8_t mode = 0;
    jam2::GridRunState run_state = jam2::GridRunState::Stopped;
};

MetronomePayload decode_metronome_payload(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 12 && payload.size() != 20 && payload.size() != 56) {
        throw std::runtime_error("metronome payload size mismatch");
    }
    const int bpm = static_cast<int>(payload[0]) |
        (static_cast<int>(payload[1]) << 8) |
        (static_cast<int>(payload[2]) << 16) |
        (static_cast<int>(payload[3]) << 24);
    const std::uint64_t revision_or_request = read_u64_le(payload, 4);
    std::uint64_t epoch = 0;
    if (payload.size() == 20) {
        epoch = read_u64_le(payload, 12);
    }
    MetronomePayload decoded;
    decoded.bpm = bpm;
    decoded.revision_or_request = revision_or_request;
    decoded.epoch_sample_time = epoch;
    if (payload.size() == 56) {
        decoded.epoch_sample_time = read_u64_le(payload, 12);
        decoded.pattern = jam2::metronome::sanitize({
            std::abs(bpm),
            static_cast<int>(payload[21]),
            static_cast<int>(payload[22]),
            static_cast<int>(payload[23]),
            read_u64_le(payload, 24),
            read_u64_le(payload, 32),
            read_u64_le(payload, 40),
            read_u64_le(payload, 48),
        });
        const std::uint8_t control = payload[20];
        decoded.has_pattern = control == 1 || (control & kGridMessageMarker) != 0;
        if ((control & kGridMessageMarker) != 0) {
            decoded.kind = static_cast<GridMessageKind>(control & 0x03U);
            decoded.mode = static_cast<std::uint8_t>((control >> 2U) & 0x03U);
            decoded.run_state = static_cast<jam2::GridRunState>((control >> 4U) & 0x03U);
            if (decoded.kind == GridMessageKind::LegacyState || decoded.mode > 2 ||
                static_cast<std::uint8_t>(decoded.run_state) >
                    static_cast<std::uint8_t>(jam2::GridRunState::AuthorityMissing)) {
                throw std::runtime_error("invalid grid authority message");
            }
        } else {
            decoded.run_state = bpm > 0

                ? jam2::GridRunState::Running
                : jam2::GridRunState::Stopped;
        }
    }
    return decoded;
}

struct TransportPayload {
    jam2::EngineTransportAction action = jam2::EngineTransportAction::None;
    std::uint32_t event_counter = 0;
    std::uint32_t grid_revision = 0;
    std::uint64_t target_sender_frame = 0;
};

std::array<std::uint8_t, 20> encode_transport_payload(const TransportPayload& value)
{
    std::array<std::uint8_t, 20> payload{};
    payload[0] = 1;
    payload[1] = static_cast<std::uint8_t>(value.action);
    const std::uint64_t identity =
        (static_cast<std::uint64_t>(value.grid_revision) << 32U) |
        static_cast<std::uint64_t>(value.event_counter);
    write_u64_le(std::span<std::uint8_t>(payload), 4, identity);
    write_u64_le(std::span<std::uint8_t>(payload), 12, value.target_sender_frame);
    return payload;
}

TransportPayload decode_transport_payload(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 20 || payload[0] != 1) {
        throw std::runtime_error("transport payload size or version mismatch");
    }
    const std::uint64_t identity = read_u64_le(payload, 4);
    const auto action = static_cast<jam2::EngineTransportAction>(payload[1]);
    if (action != jam2::EngineTransportAction::TrackRestart &&
        action != jam2::EngineTransportAction::TrackStop &&
        action != jam2::EngineTransportAction::TrackPlay &&
        action != jam2::EngineTransportAction::RecordStart &&
        action != jam2::EngineTransportAction::RecordStop) {
        throw std::runtime_error("transport action is invalid");
    }
    return {
        action,
        static_cast<std::uint32_t>(identity & 0xffffffffULL),
        static_cast<std::uint32_t>(identity >> 32U),
        read_u64_le(payload, 12),
    };
}

void observe_timing(
    std::uint64_t value,
    std::uint64_t& min_value,
    std::uint64_t& sum_value,
    std::uint64_t& max_value)
{
    if (min_value == 0 || value < min_value) {
        min_value = value;
    }
    if (value > max_value) {
        max_value = value;
    }
    sum_value += value;
}



std::int64_t ms_to_signed_frames(double ms, double sample_rate)
{
    return sample_rate > 0.0 ? static_cast<std::int64_t>(std::llround(ms * sample_rate / 1000.0)) : 0;
}



std::string os_error_text(unsigned long code)
{
    if (code == 0) {
        return {};
    }
    return "error " + std::to_string(code);
}

#if defined(_WIN32)
std::string win_priority_class_text(DWORD value)
{

    switch (value) {
    case IDLE_PRIORITY_CLASS: return "idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below-normal";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above-normal";
    case HIGH_PRIORITY_CLASS: return "high";
    case REALTIME_PRIORITY_CLASS: return "realtime";
    default: return "unknown-" + std::to_string(value);
    }
}

std::string win_thread_priority_text(int value)
{
    switch (value) {
    case THREAD_PRIORITY_IDLE: return "idle";
    case THREAD_PRIORITY_LOWEST: return "lowest";
    case THREAD_PRIORITY_BELOW_NORMAL: return "below-normal";
    case THREAD_PRIORITY_NORMAL: return "normal";
    case THREAD_PRIORITY_ABOVE_NORMAL: return "above-normal";
    case THREAD_PRIORITY_HIGHEST: return "highest";
    case THREAD_PRIORITY_TIME_CRITICAL: return "time-critical";
    case THREAD_PRIORITY_ERROR_RETURN: return "error";
    default: return std::to_string(value);
    }
}
#endif

#if defined(__APPLE__)
std::string mac_qos_text(qos_class_t value)
{
    switch (value) {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    case QOS_CLASS_UNSPECIFIED: return "unspecified";
    default: return std::to_string(static_cast<int>(value));
    }
}

std::uint64_t ns_to_mach_absolute(std::uint64_t ns)
{
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) != KERN_SUCCESS || info.numer == 0) {
        return ns;
    }
    return (ns * static_cast<std::uint64_t>(info.denom)) / static_cast<std::uint64_t>(info.numer);
}
#endif

class OsPriorityScope {
public:
    explicit OsPriorityScope(const Options& options)
    {
        status_.requested = options.os_priority;
        status_.platform = platform_name();
        status_.cpu_count = std::thread::hardware_concurrency();
        apply(options);
    }

    ~OsPriorityScope()
    {
#if defined(_WIN32)
        if (mmcss_handle_ != nullptr) {
            (void)AvRevertMmThreadCharacteristics(mmcss_handle_);
        }
        if (original_thread_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
            (void)SetThreadPriority(GetCurrentThread(), original_thread_priority_);
        }
        if (timer_resolution_active_) {
            (void)timeEndPeriod(1);
        }
#endif
    }

    OsPriorityScope(const OsPriorityScope&) = delete;
    OsPriorityScope& operator=(const OsPriorityScope&) = delete;

    const OsSchedulingStatus& status() const { return status_; }

private:
    void apply(const Options& options)
    {
#if defined(_WIN32)
        apply_windows(options);
#elif defined(__APPLE__)
        apply_macos(options);
#else
        (void)options;
        status_.process_priority = "unsupported";
        status_.thread_priority = "unsupported";
#endif
    }

#if defined(_WIN32)
    void apply_windows(const Options& options)
    {
        status_.process_priority = win_priority_class_text(GetPriorityClass(GetCurrentProcess()));
        original_thread_priority_ = GetThreadPriority(GetCurrentThread());
        status_.thread_priority = win_thread_priority_text(original_thread_priority_);
        status_.mmcss_requested = "off";
        status_.mmcss_active = "off";
        status_.timer_resolution_requested = "off";
        status_.timer_resolution_active = "off";
        if (options.os_priority == OsPriorityMode::Off) {
            return;
        }

        // Keep the whole application at high priority. Realtime is restricted
        // to this packet worker (plus MMCSS) so Qt/file/control workers cannot
        // starve the OS if a GUI task misbehaves.
        const DWORD priority_class = HIGH_PRIORITY_CLASS;
        if (!SetPriorityClass(GetCurrentProcess(), priority_class)) {
            status_.process_priority = "request-failed:" + os_error_text(GetLastError());
        } else {
            status_.process_priority = win_priority_class_text(GetPriorityClass(GetCurrentProcess()));
        }

        const int thread_priority = options.os_priority == OsPriorityMode::Realtime ?
            THREAD_PRIORITY_TIME_CRITICAL :
            THREAD_PRIORITY_HIGHEST;
        if (!SetThreadPriority(GetCurrentThread(), thread_priority)) {
            status_.thread_priority = "request-failed:" + os_error_text(GetLastError());
        } else {
            status_.thread_priority = win_thread_priority_text(GetThreadPriority(GetCurrentThread()));
        }

        status_.timer_resolution_requested = "1ms";
        const MMRESULT timer_result = timeBeginPeriod(1);
        if (timer_result == TIMERR_NOERROR) {
            timer_resolution_active_ = true;
            status_.timer_resolution_active = "1ms";
        } else {
            status_.timer_resolution_active = "off";
            status_.timer_resolution_error = os_error_text(timer_result);
        }

        status_.mmcss_requested = "Pro Audio";
        DWORD task_index = 0;
        mmcss_handle_ = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
        if (mmcss_handle_ == nullptr) {
            const DWORD pro_audio_error = GetLastError();
            task_index = 0;
            mmcss_handle_ = AvSetMmThreadCharacteristicsA("Audio", &task_index);
            if (mmcss_handle_ != nullptr) {
                status_.mmcss_active = "on";
                status_.mmcss_profile = "Audio";
                status_.mmcss_error = "Pro Audio failed: " + os_error_text(pro_audio_error);
            } else {
                status_.mmcss_active = "off";
                status_.mmcss_error = "Pro Audio failed: " + os_error_text(pro_audio_error) +
                    "; Audio failed: " + os_error_text(GetLastError());
            }
        } else {
            status_.mmcss_active = "on";
            status_.mmcss_profile = "Pro Audio";
        }
    }
#endif

#if defined(__APPLE__)
    void apply_macos(const Options& options)
    {
        status_.process_priority = "unchanged";
        status_.qos_requested = "off";
        status_.qos_active = "off";
        status_.realtime_requested = "off";
        status_.realtime_active = "off";
        qos_class_t active_qos = QOS_CLASS_UNSPECIFIED;
        int relative_priority = 0;
        if (pthread_get_qos_class_np(pthread_self(), &active_qos, &relative_priority) != 0) {
            active_qos = QOS_CLASS_UNSPECIFIED;
        }
        status_.thread_priority = mac_qos_text(active_qos);
        if (options.os_priority == OsPriorityMode::Off) {
            return;
        }
        if (options.os_priority == OsPriorityMode::High) {
            status_.qos_requested = "user-interactive";
            const int result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            if (result != 0) {
                status_.qos_error = "error " + std::to_string(result);
            }
            if (pthread_get_qos_class_np(pthread_self(), &active_qos, &relative_priority) != 0) {
                active_qos = QOS_CLASS_UNSPECIFIED;
            }
            status_.qos_active = mac_qos_text(active_qos);
            status_.thread_priority = status_.qos_active;
            return;
        }

        status_.realtime_requested = "thread-time-constraint";
        const std::uint64_t packet_period_ns =
            options.sample_rate > 0 && options.frame_size > 0 ?
                (static_cast<std::uint64_t>(options.frame_size) * 1000000000ULL) /
                    static_cast<std::uint64_t>(options.sample_rate) :
                1000000ULL;
        const std::uint64_t period = ns_to_mach_absolute(packet_period_ns);
        thread_time_constraint_policy_data_t policy{};
        policy.period = static_cast<std::uint32_t>(std::max<std::uint64_t>(period, 1));
        policy.computation = static_cast<std::uint32_t>(std::max<std::uint64_t>(period / 4, 1));
        policy.constraint = static_cast<std::uint32_t>(std::max<std::uint64_t>(period / 2, policy.computation + 1));
        policy.preemptible = TRUE;
        const kern_return_t result = thread_policy_set(
            pthread_mach_thread_np(pthread_self()),
            THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),
            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (result == KERN_SUCCESS) {
            status_.realtime_active = "on";
            status_.thread_priority = "thread-time-constraint";
        } else {
            status_.realtime_active = "off";
            status_.realtime_error = mach_error_string(result);

        }
    }
#endif

    OsSchedulingStatus status_;
#if defined(_WIN32)
    HANDLE mmcss_handle_ = nullptr;
    bool timer_resolution_active_ = false;
    int original_thread_priority_ = THREAD_PRIORITY_ERROR_RETURN;
#endif
};





void mix_leader_click_into_packet(
    std::span<std::int32_t> samples,
    std::uint64_t packet_sample_time,
    int sample_rate,
    double level,
    std::uint64_t epoch_sample_time,
    jam2::metronome::PatternSnapshot pattern)
{
    if (sample_rate <= 0 || samples.empty()) return;
    pattern = jam2::metronome::sanitize(pattern);
    const std::uint64_t step_interval = jam2::metronome::step_interval_samples(
        static_cast<double>(sample_rate), pattern.bpm, pattern.division);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint64_t absolute_sample = packet_sample_time + static_cast<std::uint64_t>(i);
        if (absolute_sample < epoch_sample_time) continue;
        const std::uint64_t grid_sample = absolute_sample - epoch_sample_time;
        samples[i] = jam2::metronome::mix_pcm24(
            samples[i],
            jam2::metronome::render_sample(
                pattern, grid_sample, step_interval, static_cast<double>(sample_rate), level));
    }
}

class CliPeerStreamPlayback final : public jam2::PeerStreamPlayback {
public:
    explicit CliPeerStreamPlayback(jam2::Engine* engine) noexcept : engine_(engine) {}
    bool acceptsFrames() const noexcept override { return engine_ != nullptr; }
    std::size_t depthFrames() const noexcept override
    {
        return engine_ != nullptr
            ? engine_->networkPlaybackDepth()
            : (std::numeric_limits<std::size_t>::max)() / 2U;
    }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        return engine_ != nullptr ? engine_->pushNetworkPlayback(frames) : 0;
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        if (engine_ != nullptr) engine_->requestNetworkPlaybackDrop(frames);
    }
    void setResamplerRatio(double ratio) noexcept override
    {
        if (engine_ != nullptr) engine_->setNetworkPlaybackRatio(ratio);
    }
    void detach() noexcept { engine_ = nullptr; }

private:
    jam2::Engine* engine_ = nullptr;
};

jam2::PeerStreamConfig make_peer_stream_config(const Options& options, bool collect_diagnostics)
{
    jam2::PeerStreamConfig config;
    config.sample_rate = options.sample_rate;
    config.frames_per_packet = options.frame_size;
    config.audio_format = options.network_audio_format;
    config.sample_time_playout = options.sample_time_playout;
    config.playout_delay_frames = options.playout_delay_frames;
    config.playback_max_frames = options.playback_max_frames;
    config.playback_queue_capacity_frames = options.playback_ring_frames;
    config.jitter_buffer_frames = options.jitter_buffer_frames;
    config.jitter_buffer_max_frames = options.jitter_buffer_max_frames;
    config.adaptive_playback_cushion = options.adaptive_playback_cushion;
    config.adaptive_playback_target_frames = options.adaptive_playback_target_frames;
    config.adaptive_playback_min_frames = options.adaptive_playback_min_frames;
    config.adaptive_playback_max_frames = options.adaptive_playback_max_frames;
    config.adaptive_playback_release_ppm = options.adaptive_playback_release_ppm;
    config.drift_correction = options.drift_correction;
    config.drift_smoothing = options.drift_smoothing;
    config.drift_deadband_ppm = options.drift_deadband_ppm;
    config.drift_max_correction_ppm = options.drift_max_correction_ppm;
    config.stats_warmup_us = static_cast<std::uint64_t>(options.stats_warmup_ms) * 1000ULL;
    config.collect_diagnostics = collect_diagnostics;
    return config;
}

jam2::NetworkSessionContract make_network_session_contract(const Options& options)
{
    return {
        jam2::protocol::kProtocolVersion,
        options.network_audio_format,
        options.sample_rate,
        options.frame_size,
    };
}

jam2::PeerId compatibility_peer_id(const jam2::Endpoint& endpoint) noexcept
{
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t value = offset_basis;
    for (const unsigned char byte : endpoint.host) {
        value = (value ^ static_cast<std::uint64_t>(byte)) * prime;
    }
    value = (value ^ 0xffULL) * prime;
    value = (value ^ static_cast<std::uint64_t>(endpoint.port & 0xffU)) * prime;
    value = (value ^ static_cast<std::uint64_t>((endpoint.port >> 8U) & 0xffU)) * prime;
    return jam2::PeerId{value == 0 ? 1ULL : value};
}



template <typename T>
struct EngineObserver {
    T* value = nullptr;

    T* get() const noexcept { return value; }
    T* operator->() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
    bool operator==(std::nullptr_t) const noexcept { return value == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return value != nullptr; }
};

struct OptionalAudioStream {
    std::unique_ptr<jam2::Engine> owned_engine;
    EngineObserver<jam2::Engine> engine;
    jam2::NetworkCaptureAttachment network_capture;
    bool persistent_engine = false;
};

bool engine_restart_required(
    const jam2::EngineConfig& active,
    const jam2::EngineConfig& requested) noexcept
{
    return active.backend != requested.backend ||
        active.audio_device_id != requested.audio_device_id ||
        active.sample_rate != requested.sample_rate ||
        active.audio_buffer_frames != requested.audio_buffer_frames ||
        active.headless_clock_drift_ppm != requested.headless_clock_drift_ppm ||
        active.input_channels != requested.input_channels ||
        active.channels.input != requested.channels.input ||
        active.channels.output != requested.channels.output ||
        active.capture_ring_frames != requested.capture_ring_frames ||
        active.playback_ring_frames != requested.playback_ring_frames ||
        active.playback_prefill_frames != requested.playback_prefill_frames ||
        active.diagnostics_enabled != requested.diagnostics_enabled ||
        active.test_input != requested.test_input ||
        active.prepared_track_max_frames != requested.prepared_track_max_frames;
}

jam2::EngineConfig make_engine_config_impl(const Options& options, bool leader_audio_local_click)
{
    const bool diagnostics_enabled =
        options.stats_enabled && (options.log_stats_dir.has_value() || options.stats_interval_ms > 0);
    jam2::EngineConfig config;
    config.backend = options.headless_audio
        ? jam2::EngineAudioBackend::Headless
        : jam2::EngineAudioBackend::Device;
    config.audio_device_id = options.audio_device_id.value_or(-1);
    config.sample_rate = options.sample_rate;
    config.audio_buffer_frames = options.headless_audio && options.audio_buffer_size <= 0
        ? static_cast<long>(options.frame_size)
        : options.audio_buffer_size;
    config.headless_clock_drift_ppm = options.headless_clock_drift_ppm;
    config.input_channels = options.input_channels;
    config.channels = options.channel_selection;
    config.capture_ring_frames = options.capture_ring_frames;
    config.playback_ring_frames = options.playback_ring_frames;
    config.playback_prefill_frames = options.playback_prefill_frames;
    config.diagnostics_enabled = diagnostics_enabled;
    config.metronome_enabled = options.metronome;
    config.metronome_pattern = jam2::metronome::sanitize({options.bpm, 4, 1, 4, 0x0fULL, 0, 0x01ULL, 0});
    config.metronome_level_ppm = ppm_from_gain(options.metronome_level);
    config.remote_level_ppm = ppm_from_gain(options.remote_level);
    config.send_level_ppm = ppm_from_gain(options.send_level);
    config.local_monitor_enabled = options.local_monitor;
    config.local_monitor_level_ppm = ppm_from_gain(options.local_monitor_level);
    config.metronome_mode = static_cast<jam2::EngineMetronomeMode>(metronome_mode_id(options.metronome_mode));
    config.leader_audio_local_click = leader_audio_local_click;
    config.test_input = static_cast<jam2::EngineTestInput>(test_input_mode_id(options.test_input));
    config.test_input_level_ppm = 125000;
    config.prepared_track_max_frames =
        static_cast<std::size_t>(std::max(1, options.sample_rate)) * 60U * 5U;
    return config;
}

OptionalAudioStream start_optional_audio(
    const Options& options,
    bool leader_audio_local_click,
    Jam2RuntimeHost* runtime_host = nullptr)
{
    OptionalAudioStream audio;
    if (!options.audio_device_id && !options.headless_audio) {
        return audio;
    }
    const jam2::EngineConfig config = make_engine_config_impl(options, leader_audio_local_click);

    if (runtime_host != nullptr && runtime_host->engine != nullptr) {
        const jam2::EngineConfig* active = runtime_host->engine->config();
        if (active == nullptr || engine_restart_required(*active, config)) {
            throw std::runtime_error("native runtime engine configuration does not match the active engine");
        }
        audio.engine.value = runtime_host->engine;
        audio.persistent_engine = true;
    } else {
        audio.owned_engine = std::make_unique<jam2::Engine>();
        audio.owned_engine->start(config);
        audio.engine.value = audio.owned_engine.get();
    }
    return audio;

}

void attach_network_capture(OptionalAudioStream& audio)
{
    if (!audio.engine) {
        return;
    }
    audio.network_capture = audio.engine->attachNetworkCapture();

    if (audio.network_capture.generation == 0) {
        throw std::runtime_error("failed to attach the local engine capture tap");

    }
    const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;
    while (!audio.engine->networkCaptureReady(audio.network_capture)) {
        if (jam2::monotonic_us() >= deadline) {
            audio.engine->detachNetworkCapture(audio.network_capture);
            audio.network_capture = {};
            throw std::runtime_error("audio callback did not acknowledge the network capture epoch within 5 seconds");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void detach_network_capture(OptionalAudioStream& audio) noexcept
{
    if (audio.engine && audio.network_capture.generation != 0) {
        audio.engine->detachNetworkCapture(audio.network_capture);
        audio.network_capture = {};
        const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;

        while (audio.engine->snapshot().network_capture_enabled) {
            if (jam2::monotonic_us() >= deadline) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        audio.engine->requestNetworkPlaybackDrop(audio.engine->networkPlaybackDepth());
    }
}

int drain_pending_udp(jam2::UdpSocket& socket)
{
    int drained = 0;
    for (;;) {
        const auto received = socket.recv_from(0);
        if (!received) {
            break;
        }
        ++drained;
    }
    return drained;
}

struct CommandThread {
    RuntimeState state;
    jam2::cli::CliRecordingSupervisor recording;

    CommandThread(
        const Options& options,
        bool leader_audio_local_click)
    {
        state.metronome.store(options.metronome, std::memory_order_relaxed);
        state.bpm.store(options.bpm, std::memory_order_relaxed);
        state.metronome_beats_per_bar.store(4, std::memory_order_relaxed);
        state.metronome_division.store(1, std::memory_order_relaxed);
        state.metronome_step_count.store(4, std::memory_order_relaxed);
        state.metronome_play_mask_low.store(0x0fULL, std::memory_order_relaxed);
        state.metronome_play_mask_high.store(0, std::memory_order_relaxed);
        state.metronome_accent_mask_low.store(0x01ULL, std::memory_order_relaxed);
        state.metronome_accent_mask_high.store(0, std::memory_order_relaxed);
        state.metronome_level_ppm.store(ppm_from_gain(options.metronome_level), std::memory_order_relaxed);
        state.remote_level_ppm.store(ppm_from_gain(options.remote_level), std::memory_order_relaxed);
        state.send_level_ppm.store(ppm_from_gain(options.send_level), std::memory_order_relaxed);
        state.local_monitor.store(options.local_monitor, std::memory_order_relaxed);
        state.local_monitor_level_ppm.store(ppm_from_gain(options.local_monitor_level), std::memory_order_relaxed);
        state.metronome_mode.store(metronome_mode_id(options.metronome_mode), std::memory_order_relaxed);
        state.leader_audio_local_click.store(leader_audio_local_click, std::memory_order_relaxed);
        state.metronome_local_authority.store(leader_audio_local_click, std::memory_order_relaxed);
        state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        state.stats_enabled.store(options.stats_enabled, std::memory_order_relaxed);
    }
};

jam2::cli::RecordingSidecarContext make_recording_sidecar_context(
    const Options& options,
    const RuntimeState& state)
{
    jam2::cli::RecordingSidecarContext context;
    context.metronome_enabled = state.metronome.load(std::memory_order_relaxed);
    context.bpm = state.bpm.load(std::memory_order_relaxed);
    context.metronome_level = gain_from_ppm(state.metronome_level_ppm.load(std::memory_order_relaxed));
    context.remote_level = gain_from_ppm(state.remote_level_ppm.load(std::memory_order_relaxed));
    context.send_level = gain_from_ppm(state.send_level_ppm.load(std::memory_order_relaxed));
    context.local_monitor_enabled = state.local_monitor.load(std::memory_order_relaxed);
    context.local_monitor_level = gain_from_ppm(state.local_monitor_level_ppm.load(std::memory_order_relaxed));
    context.metronome_mode = std::string(
        metronome_mode_text(state.metronome_mode.load(std::memory_order_relaxed)));
    context.test_input = std::string(test_input_mode_text(options.test_input));
    context.metronome_epoch_sample_time =
        state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    context.metronome_epoch_valid =
        state.metronome_epoch_valid.load(std::memory_order_relaxed);
    context.pattern = metronome_pattern_from_runtime(state);
    context.sample_time_playout = options.sample_time_playout;
    context.playout_delay_frames = options.playout_delay_frames;
    return context;
}






void print_optional_audio_stats(const OptionalAudioStream& audio, const Options& options)
{
    if (!audio.engine) {
        return;
    }
    const auto engine_snapshot = audio.engine->snapshot();
    const auto cold_snapshot = audio.engine->coldSnapshot();
    const auto capture_stats = engine_snapshot.capture_ring;
    const auto playback_stats = engine_snapshot.playback_ring;
    const auto callback_stats = engine_snapshot.callback_timing;
    const std::size_t capture_readable = engine_snapshot.capture_ring_depth_frames;
    const std::size_t playback_readable = engine_snapshot.playback_ring_depth_frames;
    const auto& stream_info = cold_snapshot.stream;
    std::cout << "Audio callbacks: " << engine_snapshot.callbacks << "\n";
    if (callback_stats.interval_samples > 0) {
        std::cout << "Audio callback interval ms min: "
                  << static_cast<double>(callback_stats.interval_min_us) / 1000.0 << "\n";
        std::cout << "Audio callback interval ms avg: "
                  << avg_us_to_ms(callback_stats.interval_sum_us, callback_stats.interval_samples) << "\n";
        std::cout << "Audio callback interval ms max: "
                  << static_cast<double>(callback_stats.interval_max_us) / 1000.0 << "\n";
    }
    std::cout << "Audio callback gaps over 1.1x: " << callback_stats.gap_over_1_1x_count << "\n";
    std::cout << "Audio callback gaps over 1.5x: " << callback_stats.gap_over_1_5x_count << "\n";
    std::cout << "Audio callback gaps over 2x: " << callback_stats.gap_over_2x_count << "\n";
    if (options.audio_device_id) {
        std::cout << "Audio device id: " << *options.audio_device_id << "\n";
    }
    std::cout << "Requested sample rate: " << options.sample_rate << "\n";
    std::cout << "Active sample rate: " << stream_info.sample_rate << "\n";
    std::cout << "Requested audio buffer size frames: " << options.audio_buffer_size << "\n";
    std::cout << "Requested audio buffer size ms: "
              << frames_to_ms(static_cast<std::size_t>(options.audio_buffer_size > 0 ? options.audio_buffer_size : 0), options.sample_rate)
              << "\n";
    std::cout << "Active audio buffer size frames: " << stream_info.buffer_size << "\n";
    std::cout << "Active audio buffer size ms: "
              << frames_to_ms(static_cast<std::size_t>(stream_info.buffer_size > 0 ? stream_info.buffer_size : 0), stream_info.sample_rate)
              << "\n";
    std::cout << "Requested input mix: " << mono_mix_mode_text(options.channel_selection.input.size()) << "\n";
    std::cout << "Active input mix: " << mono_mix_mode_text(stream_info.channels.input.size()) << "\n";
    std::cout << "Requested audio channels: " << channel_selection_text(options.channel_selection) << "\n";
    std::cout << "Active audio channels: " << channel_selection_text(stream_info.channels) << "\n";
    std::cout << "Backend sample format: " << stream_info.sample_format << "\n";
    std::cout << "Output channels: duplicated mono to selected output channels\n";
    std::cout << "Capture ring capacity frames: " << engine_snapshot.capture_ring_capacity_frames << "\n";
    std::cout << "Playback ring capacity frames: " << engine_snapshot.playback_ring_capacity_frames << "\n";
    const auto& recording = engine_snapshot.jam_recording;
    std::cout << "Recording active: " << (recording.active ? "yes" : "no") << "\n";
    std::cout << "Recording folder: " << cold_snapshot.recording_folder << "\n";
    std::cout << "Recording sample rate: " << cold_snapshot.recording_sample_rate << "\n";
    std::cout << "Recording frames written: " << recording.frames_written << "\n";
    std::cout << "Recording dropped frames: " << recording.dropped_frames << "\n";
    std::cout << "Recording drop events: " << recording.drop_events << "\n";
    std::cout << "Recording queue depth frames: " << recording.queue_depth_frames << "\n";
    std::cout << "Recording queue capacity frames: " << recording.queue_capacity_frames << "\n";
    std::cout << "Recording writer errors: " << recording.writer_errors << "\n";
    std::cout << "Playback prefilled: " << (engine_snapshot.playback_prefilled ? "yes" : "no") << "\n";
    std::cout << "Playback prefill frames: " << options.playback_prefill_frames << "\n";
    std::cout << "Playback prefill ms: " << frames_to_ms(options.playback_prefill_frames, options.sample_rate) << "\n";
    std::cout << "Capture ring overruns frames: " << capture_stats.overruns << "\n";
    std::cout << "Capture ring underruns frames: " << capture_stats.underruns << "\n";
    std::cout << "Capture ring underrun events: " << capture_stats.underrun_events << "\n";
    std::cout << "Capture ring readable frames: " << capture_readable << "\n";
    std::cout << "Capture ring readable ms: " << frames_to_ms(capture_readable, options.sample_rate) << "\n";
    std::cout << "Network capture enabled: " << (engine_snapshot.network_capture_enabled ? "yes" : "no") << "\n";
    std::cout << "Network capture ready: " << (engine_snapshot.network_capture_ready ? "yes" : "no") << "\n";
    std::cout << "Network capture generation: " << engine_snapshot.network_capture_generation << "\n";
    std::cout << "Network capture epoch frame: " << engine_snapshot.network_capture_epoch_frame << "\n";
    std::cout << "Network capture stale frames discarded: "
              << engine_snapshot.network_capture_stale_frames_discarded << "\n";
    std::cout << "Network capture attach count: " << engine_snapshot.network_capture_attach_count << "\n";
    std::cout << "Network capture detach count: " << engine_snapshot.network_capture_detach_count << "\n";
    std::cout << "Network playback enabled: " << (engine_snapshot.network_playback_enabled ? "yes" : "no") << "\n";
    std::cout << "Engine command queue depth/capacity/high-water/rejections: "
              << engine_snapshot.command_queue_depth << "/"
              << engine_snapshot.command_queue_capacity << "/"
              << engine_snapshot.command_queue_high_water << "/"
              << engine_snapshot.command_queue_rejections << "\n";
    std::cout << "Engine scheduled command depth/capacity/high-water/rejections: "
              << engine_snapshot.scheduled_command_depth << "/"
              << engine_snapshot.scheduled_command_capacity << "/"
              << engine_snapshot.scheduled_command_high_water << "/"
              << engine_snapshot.scheduled_command_rejections << "\n";
    std::cout << "Engine event queue depth/capacity/high-water/drops: "
              << engine_snapshot.event_queue_depth << "/"
              << engine_snapshot.event_queue_capacity << "/"
              << engine_snapshot.event_queue_high_water << "/"
              << engine_snapshot.event_queue_drops << "\n";
    std::cout << "Playback ring overruns frames: " << playback_stats.overruns << "\n";
    std::cout << "Playback ring underruns frames: " << playback_stats.underruns << "\n";
    std::cout << "Playback ring underrun events: " << playback_stats.underrun_events << "\n";
    std::cout << "Playback ring underrun event max frames: " << playback_stats.underrun_event_max_frames << "\n";
    std::cout << "Playback ring underrun event max ms: "
              << frames_to_ms(static_cast<std::size_t>(playback_stats.underrun_event_max_frames), stream_info.sample_rate) << "\n";
    std::cout << "Playback ring underrun burst events: " << playback_stats.underrun_burst_events << "\n";
    std::cout << "Playback ring underrun burst max frames: " << playback_stats.underrun_burst_max_frames << "\n";
    std::cout << "Playback ring underrun burst max ms: "
              << frames_to_ms(static_cast<std::size_t>(playback_stats.underrun_burst_max_frames), stream_info.sample_rate) << "\n";
    std::cout << "Playback ring underrun time ms: "
              << frames_to_ms(static_cast<std::size_t>(playback_stats.underruns), stream_info.sample_rate) << "\n";
    std::cout << "Playback depth observed frames: " << playback_stats.depth_observed_frames << "\n";
    std::cout << "Playback depth under 2ms frames: " << playback_stats.depth_under_2ms_frames << "\n";
    std::cout << "Playback depth under 2ms percent: "
              << frames_percent(playback_stats.depth_under_2ms_frames, playback_stats.depth_observed_frames) << "\n";
    std::cout << "Playback depth under 5ms frames: " << playback_stats.depth_under_5ms_frames << "\n";
    std::cout << "Playback depth under 5ms percent: "
              << frames_percent(playback_stats.depth_under_5ms_frames, playback_stats.depth_observed_frames) << "\n";
    std::cout << "Playback depth under 10ms frames: " << playback_stats.depth_under_10ms_frames << "\n";
    std::cout << "Playback depth under 10ms percent: "
              << frames_percent(playback_stats.depth_under_10ms_frames, playback_stats.depth_observed_frames) << "\n";
    std::cout << "Playback ring readable frames: " << playback_readable << "\n";
    std::cout << "Playback ring readable ms: " << frames_to_ms(playback_readable, options.sample_rate) << "\n";
    std::cout << "Audio control metronome: " << (engine_snapshot.metronome_enabled ? "on" : "off") << "\n";
    std::cout << "Audio control BPM: " << engine_snapshot.metronome_pattern.bpm << "\n";
    std::cout << "Audio control metronome level: " << gain_from_ppm(engine_snapshot.metronome_level_ppm) << "\n";
    std::cout << "Audio control remote playback level: " << gain_from_ppm(engine_snapshot.remote_level_ppm) << "\n";
    std::cout << "Audio control metronome mode: " << metronome_mode_text(static_cast<int>(engine_snapshot.metronome_mode)) << "\n";
    std::cout << "Audio control metronome epoch sample time: " << engine_snapshot.metronome_epoch_frame << "\n";
    std::cout << "Audio control metronome epoch valid: " << (engine_snapshot.metronome_epoch_valid ? "yes" : "no") << "\n";
    std::cout << "Audio control resampler ratio: " << (static_cast<double>(engine_snapshot.playback_ratio_ppm) / 1000000.0) << "\n";
}

int run_test_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("test-device requires a device id");
    }
    const int id = std::stoi(argv[2]);
    double sample_rate = 48000.0;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--sample-rate") {
            sample_rate = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (sample_rate <= 0.0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }

    const auto probe = jam2::audio::probe_device(id, sample_rate);
    std::cout << "Device: [" << probe.device.id << "] " << probe.device.backend << " " << probe.device.name << "\n";
    if (!probe.driver_name.empty()) {
        std::cout << "Driver name: " << probe.driver_name << "\n";
    }
    std::cout << "Driver version: " << probe.driver_version << "\n";
    std::cout << "Channels: input=" << probe.input_channels << " output=" << probe.output_channels << "\n";
    std::cout << "Latencies samples: input=" << probe.input_latency_samples
              << " output=" << probe.output_latency_samples << "\n";
    std::cout << "Buffer sizes samples: min=" << probe.min_buffer_size
              << " max=" << probe.max_buffer_size
              << " preferred=" << probe.preferred_buffer_size
              << " granularity=" << probe.buffer_granularity << "\n";
    std::cout << "Current sample rate: " << probe.current_sample_rate << "\n";
    std::cout << "Requested sample rate " << sample_rate << ": "
              << (probe.requested_sample_rate_supported ? "supported" : "not supported") << "\n";
    std::cout << "Input channel ids: ";
    if (probe.input_channels <= 0) {
        std::cout << "none";
    } else {
        for (long channel = 1; channel <= probe.input_channels; ++channel) {
            std::cout << (channel == 1 ? "" : ",") << channel;
        }
    }

    std::cout << "\n";
    std::cout << "Output channel ids: ";
    if (probe.output_channels <= 0) {
        std::cout << "none";
    } else {
        for (long channel = 1; channel <= probe.output_channels; ++channel) {
            std::cout << (channel == 1 ? "" : ",") << channel;

        }
    }
    std::cout << "\n";
    if (probe.input_channels > 0) {

        std::cout << "Example one-channel input mixed to mono stream: --input-channels 1\n";
    }
    if (probe.input_channels > 1) {
        std::cout << "Example two-channel input mixed to mono stream: --input-channels 1,2\n";
    }
    if (probe.input_channels > 2) {
        std::cout << "Example multi-input mix to mono stream: --input-channels 1,2,3,4\n";
    }
    if (probe.output_channels > 0) {
        std::cout << "Example one-channel output: --output-channels 1\n";
    }
    if (probe.output_channels > 1) {
        std::cout << "Example duplicated two-output mono: --output-channels 1,2\n";
    }
    if (probe.output_channels > 2) {
        std::cout << "Example duplicated multi-output: --output-channels 1,2,3,4\n";
    }
    return 0;
}


int run_meter_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("meter-device requires a device id");
    }
    const int id = std::stoi(argv[2]);
    double sample_rate = 48000.0;
    long buffer_size = 0;
    int duration_ms = 3000;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--sample-rate") {
            sample_rate = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (sample_rate <= 0.0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else if (arg == "--buffer-size") {
            buffer_size = std::stol(std::string(require_value(argc, argv, i, arg)));
            if (buffer_size <= 0) {
                throw std::runtime_error("--buffer-size must be positive");
            }
        } else if (arg == "--duration-ms") {
            duration_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (duration_ms <= 0) {
                throw std::runtime_error("--duration-ms must be positive");
            }
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }

    const auto result = jam2::audio::meter_device(id, sample_rate, buffer_size, duration_ms);
    std::cout << "Device: [" << result.device.id << "] " << result.device.backend << " " << result.device.name << "\n";
    std::cout << "Sample rate: " << result.sample_rate << "\n";
    std::cout << "Buffer size samples: " << result.buffer_size << "\n";
    std::cout << "Callbacks: " << result.callbacks << "\n";
    std::cout << "Input sample type: " << result.input_sample_type << "\n";
    std::cout << "Output sample type: " << result.output_sample_type << "\n";
    std::cout << "Input peak: " << result.input_peak << "\n";
    return 0;
}

int run_ring_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("ring-device requires a device id");
    }
    const int id = std::stoi(argv[2]);
    double sample_rate = 48000.0;
    long buffer_size = 0;
    int duration_ms = 3000;
    std::size_t ring_frames = 4096;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--sample-rate") {
            sample_rate = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (sample_rate <= 0.0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else if (arg == "--buffer-size") {
            buffer_size = std::stol(std::string(require_value(argc, argv, i, arg)));
            if (buffer_size <= 0) {
                throw std::runtime_error("--buffer-size must be positive");
            }
        } else if (arg == "--duration-ms") {
            duration_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (duration_ms <= 0) {
                throw std::runtime_error("--duration-ms must be positive");
            }
        } else if (arg == "--ring-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            if (parsed == 0) {
                throw std::runtime_error("--ring-frames must be positive");
            }
            ring_frames = static_cast<std::size_t>(parsed);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }

    const auto result = jam2::audio::ring_device(id, sample_rate, buffer_size, duration_ms, ring_frames);
    std::cout << "Device: [" << result.device.id << "] " << result.device.backend << " " << result.device.name << "\n";
    std::cout << "Sample rate: " << result.sample_rate << "\n";
    std::cout << "Buffer size samples: " << result.buffer_size << "\n";
    std::cout << "Callbacks: " << result.callbacks << "\n";
    std::cout << "Ring overruns frames: " << result.ring_overruns << "\n";
    std::cout << "Ring underruns frames: " << result.ring_underruns << "\n";
    std::cout << "Ring underrun events: " << result.ring_underrun_events << "\n";
    std::cout << "Ring readable frames: " << result.ring_readable << "\n";
    return 0;
}

int run_local(int argc, char** argv)
{
    const Options options = parse_options(argc, argv, 2);
    if (!options.audio_device_id && !options.headless_audio) {
        throw std::runtime_error("local requires --audio-device; use list-devices to inspect available low-latency devices");
    }

    // Local is the finite or signal-terminated audio engine without network
    // bootstrap, socket creation, or an interactive command channel.
    auto audio = start_optional_audio(options, true);
    if (!audio.engine || !audio.engine->snapshot().frame_clock_active) {
        throw std::runtime_error("local failed to start an audio stream");
    }

    CommandThread commands(options, true);
    commands.recording.start(audio.engine.get(), options.record_jam_folder);

    std::cout << "{\"event\":\"startup\",\"mode\":\"local\",\"stage\":\"running\"}\n";
    std::cout.flush();
    const std::uint64_t started_us = jam2::monotonic_us();
    while (!commands.state.quit.load(std::memory_order_relaxed) &&
           (options.stream_ms <= 0 ||
            jam2::monotonic_us() - started_us < static_cast<std::uint64_t>(options.stream_ms) * 1000ULL)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    commands.recording.finalize(
        audio.engine.get(),
        make_recording_sidecar_context(options, commands.state));
    return 0;
}

int run_network_session(Options options, Jam2RuntimeHost& runtime_host)
{
    // This function is the universal direct-mesh UDP packet worker. Keep the
    // process High but elevate only this thread to Time Critical/MMCSS when
    // realtime was requested, leaving Qt and file workers out of realtime.
    OsPriorityScope os_priority_scope(options);
    if (!options.session_id || !options.session_key) {
        throw std::runtime_error("network session requires --session-id and --session-key");
    }
    if (!options.mesh_peers_configured) {
        throw std::runtime_error("network session requires typed coordinator membership state");
    }
    for (auto& peer : options.mesh_peers) {
        peer = jam2::resolve_udp_endpoint(peer);
    }

    jam2::NetworkRuntime network;
    jam2::UdpSocket socket;
    apply_socket_options(socket, options);
    socket.bind(options.bind);
    const jam2::Endpoint local = socket.local_endpoint();
    const jam2::SessionInfo session{local, *options.session_id, *options.session_key};

    std::optional<jam2::Endpoint> public_candidate;
    if (options.bootstrap_role == jam2::SessionBootstrapRole::Creator) {
        if (options.public_endpoint) {
            public_candidate = options.public_endpoint;
        } else if (!options.no_stun) {
            public_candidate = jam2::stun::discover_public_endpoint(
                socket,
                options.stun_server,
                options.stun_timeout_ms,
                options.stun_retries);
        }
    }

    std::cout << "Mode: network session\n";
    std::cout << "Local UDP bind: " << jam2::endpoint_to_string(local) << "\n";
    if (public_candidate) {
        std::cout << "Public UDP candidate: " << jam2::endpoint_to_string(*public_candidate) << "\n";
    }
    std::cout << "Mesh peers: " << options.mesh_peers.size() << "\n";
    for (std::size_t i = 0; i < options.mesh_peers.size(); ++i) {
        std::cout << "  peer" << (i + 1) << ": " << jam2::endpoint_to_string(options.mesh_peers[i]);
        if (i < options.mesh_peer_ids.size() && options.mesh_peer_ids[i] != 0) {
            std::cout << " id=" << options.mesh_peer_ids[i];
        }
        std::cout << "\n";
    }
    print_socket_options(socket);
    auto audio = start_optional_audio(options, false, &runtime_host);
    attach_network_capture(audio);
    const int drained_startup_packets = drain_pending_udp(socket);
    if (drained_startup_packets > 0) {
        std::cout << "Drained startup UDP packets: " << drained_startup_packets << "\n";
    }
    std::optional<CsvStatsLog> csv_log;
    if (options.log_stats_dir) {
        const jam2::Endpoint peer_context{
            options.mesh_peers.empty() ? std::string("mesh:none") : std::string("mesh:") + std::to_string(options.mesh_peers.size()),
            0};
        csv_log.emplace(
            *options.log_stats_dir,
            make_csv_context(
                options.bootstrap_role == jam2::SessionBootstrapRole::Creator
                    ? "jam2 network create (typed runtime)"
                    : "jam2 network join (typed runtime)",
                "mesh", options, socket, local, peer_context, "coordinator-membership"));
        std::cout << "Stats CSV: " << csv_log->path().string() << "\n";
    }
    if (runtime_host.startup) {
        runtime_host.startup(Jam2RuntimeStartup{
            local,
            public_candidate,
            csv_log ? std::optional<std::filesystem::path>(csv_log->path()) : std::nullopt,
        });
    }
    const int recording_sample_rate = audio.engine
        ? static_cast<int>(std::lround(audio.engine->snapshot().sample_rate))
        : options.sample_rate;
    CommandThread commands(options, false);
    EngineControlMirror engine_control_mirror;
    hold_shared_grid_at_start(commands.state, audio.engine.get());
    auto apply_runtime_host_commands = [&] {
        std::size_t processed = 0;
        while (processed < Jam2RuntimeHost::kCommandCapacity) {
            const auto next = runtime_host.takeCommand(
                current_engine_frame(audio.engine.get()));
            if (!next) {
                break;
            }
            const jam2::EngineCommand& command = *next;
            switch (command.type) {
            case jam2::EngineCommandType::SetMetronomeEnabled: {
                const bool previous = commands.state.metronome.exchange(command.enabled, std::memory_order_relaxed);
                if (previous != command.enabled) {
                    if (command.enabled) {
                        begin_metronome_epoch(commands.state, audio.engine.get(), recording_sample_rate);
                    }
                    request_grid_revision(commands.state);
                }
                break;
            }
            case jam2::EngineCommandType::SetMetronomePattern: {
                const auto previous = metronome_pattern_from_runtime(commands.state);
                const auto next = jam2::metronome::sanitize(command.pattern);
                if (!same_metronome_pattern(previous, next)) {
                    store_metronome_pattern(commands.state, next);
                    request_grid_revision(commands.state);
                    if (previous.bpm != next.bpm ||
                        previous.beats_per_bar != next.beats_per_bar ||
                        previous.division != next.division) {
                        commands.state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                break;
            }
            case jam2::EngineCommandType::SetMetronomeLevel:
                commands.state.metronome_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetRemoteLevel:
                commands.state.remote_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetSendLevel:
                commands.state.send_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetLocalMonitorEnabled:
                commands.state.local_monitor.store(command.enabled, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetLocalMonitorLevel:
                commands.state.local_monitor_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetMetronomeMode: {
                const int next = std::clamp(command.value, 0, 2);
                const int previous = commands.state.metronome_mode.exchange(
                    next, std::memory_order_relaxed);
                if (previous != next) {
                    commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
                    commands.state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                    request_grid_revision(commands.state);
                }
                break;
            }
            case jam2::EngineCommandType::SetLeaderAudioLocalClick:
                commands.state.leader_audio_local_click.store(command.enabled, std::memory_order_relaxed);
                commands.state.metronome_local_authority.store(command.enabled, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetMetronomeEpoch:
                commands.state.metronome_epoch_sample_time.store(command.frame, std::memory_order_relaxed);
                commands.state.metronome_epoch_valid.store(command.enabled, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetMetronomeRenderOffset:
                commands.state.metronome_render_offset_frames.store(command.signed_value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::ScheduleTransport: {
                std::lock_guard<std::mutex> lock(commands.state.transport_mutex);
                commands.state.transport_revision.fetch_add(1, std::memory_order_relaxed);

                commands.state.transport_target_raw_frame.store(command.transport_target_frame, std::memory_order_relaxed);
                commands.state.transport_target_musical_frame.store(command.transport_musical_frame, std::memory_order_relaxed);
                commands.state.transport_countdown_start_frame.store(command.transport_countdown_start_frame, std::memory_order_relaxed);
                commands.state.transport_action.store(
                    static_cast<int>(command.transport_action),
                    std::memory_order_relaxed);

                commands.state.transport_pending.store(true, std::memory_order_relaxed);
                commands.state.transport_network_revision.store(
                    runtime_host.nextTransportEventId(),
                    std::memory_order_relaxed);
                commands.state.transport_network_target_raw_frame.store(command.transport_target_frame, std::memory_order_relaxed);
                commands.state.transport_network_action.store(

                    commands.state.transport_action.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                break;
            }
            default:
                break;
            }
            ++processed;
        }
    };
    std::cout << "Embedded network controls ready\n";
    commands.recording.start(audio.engine.get(), options.record_jam_folder);

    enum class EndpointProofState {
        Candidate,
        Probing,
        Active,
        Failed,
    };


    struct ProbeChallenge {
        std::uint32_t sequence = 0;
        std::uint64_t send_time_us = 0;
        bool used = false;
    };

    struct MeshPeerState {
        jam2::Endpoint endpoint;
        jam2::PeerId peer_id;
        EndpointProofState endpoint_proof = EndpointProofState::Candidate;
        std::array<ProbeChallenge, 8> probe_challenges{};
        std::uint32_t proof_attempts = 0;
        std::uint64_t proof_deadline_us = 0;
        std::uint64_t next_probe_us = 0;
        std::uint64_t proof_successes = 0;
        std::uint64_t proof_failures = 0;
        std::uint64_t proof_unverified_drops = 0;
        std::uint64_t proof_unmatched_pongs = 0;
        std::uint64_t proof_challenge_overwrites = 0;
        std::uint64_t sent_packets = 0;
        std::uint64_t sent_bytes = 0;
        std::uint64_t recv_packets = 0;
        std::uint64_t recv_bytes = 0;
        std::uint64_t ignored_packets = 0;
        UdpParseStats udp_parse;
        std::uint64_t sent_pings = 0;
        std::uint64_t sent_pongs = 0;
        std::uint64_t recv_pongs = 0;
        std::uint64_t last_transport_revision = 0;
    };

    auto endpoint_key = [](const jam2::Endpoint& endpoint) {
        return endpoint.host + ":" + std::to_string(endpoint.port);
    };

    auto endpoint_proof_name = [](EndpointProofState state) -> const char* {
        switch (state) {
        case EndpointProofState::Candidate:
            return "candidate";
        case EndpointProofState::Probing:
            return "probing";
        case EndpointProofState::Active:
            return "active";
        case EndpointProofState::Failed:
            return "failed";
        }
        return "unknown";
    };

    std::map<std::string, MeshPeerState> peers;
    for (std::size_t peer_index = 0; peer_index < options.mesh_peers.size(); ++peer_index) {
        const auto& peer = options.mesh_peers[peer_index];
        const std::uint64_t configured_id = peer_index < options.mesh_peer_ids.size()
            ? options.mesh_peer_ids[peer_index]
            : 0ULL;
        const auto inserted = peers.emplace(
            endpoint_key(peer),
            MeshPeerState{peer, configured_id != 0 ? jam2::PeerId{configured_id} : compatibility_peer_id(peer)});
        if (!inserted.second) {
            throw std::runtime_error("mesh peer list contains a duplicate endpoint");
        }
    }

    const jam2::PeerId local_peer_id = options.local_peer_id
        ? jam2::PeerId{*options.local_peer_id}
        : compatibility_peer_id(local);
    std::vector<jam2::NetworkPeerDescriptor> peer_descriptors;
    peer_descriptors.reserve(peers.size());
    for (const auto& entry : peers) {
        const auto& peer = entry.second;
        if (peer.peer_id == local_peer_id) {
            throw std::runtime_error("mesh peer identity collides with the local endpoint");
        }
        const bool duplicate_id = std::any_of(
            peer_descriptors.begin(),
            peer_descriptors.end(),
            [&](const auto& descriptor) { return descriptor.peer_id == peer.peer_id; });
        if (duplicate_id) {
            throw std::runtime_error("mesh endpoint hash collision; use a different local port");
        }
        peer_descriptors.push_back({
            peer.peer_id,
            peer.endpoint,
            jam2::PeerEndpointState::Candidate,
        });
    }
    std::uint64_t last_single_remote_peer_id = peers.size() == 1
        ? peers.begin()->second.peer_id.value
        : 0ULL;

    const jam2::PeerStreamConfig peer_stream_config = make_peer_stream_config(
        options,
        options.stats_enabled && (csv_log.has_value() || options.stats_interval_ms > 0));
    CliPeerStreamPlayback mesh_playback(audio.engine.get());
    jam2::NetworkSession network_session(
        std::move(socket),
        session,
        make_network_session_contract(options),
        options.bootstrap_role,
        local_peer_id,
        peer_descriptors,
        peer_stream_config,
        audio.engine ? &mesh_playback : nullptr,
        options.headless_clock_drift_ppm);
    auto& packet_schedule = network_session.schedule();
    std::uint64_t bootstrap_coordinator_peer_id = options.bootstrap_coordinator_peer_id.value_or(0);
    if (bootstrap_coordinator_peer_id == 0) {
        bootstrap_coordinator_peer_id = local_peer_id.value;
        for (const auto& descriptor : peer_descriptors) {
            bootstrap_coordinator_peer_id = std::min(
                bootstrap_coordinator_peer_id,
                descriptor.peer_id.value);
        }
    } else if (bootstrap_coordinator_peer_id != local_peer_id.value &&
               std::none_of(peer_descriptors.begin(), peer_descriptors.end(), [&](const auto& descriptor) {
                   return descriptor.peer_id.value == bootstrap_coordinator_peer_id;
               })) {
        throw std::runtime_error("bootstrap coordinator is not the local peer or a configured remote peer");
    }
    jam2::SessionAuthority authority(
        local_peer_id.value,
        bootstrap_coordinator_peer_id,
        bootstrap_coordinator_peer_id);

    std::vector<std::int32_t> asio_frames(static_cast<std::size_t>(options.frame_size), 0);
    std::vector<std::int32_t> network_frames(static_cast<std::size_t>(options.frame_size), 0);
    const std::uint16_t audio_payload_size = static_cast<std::uint16_t>(
        jam2::protocol::audio_payload_size(options.network_audio_format, network_frames.size()));
    const std::vector<std::uint8_t> silence_payload(audio_payload_size, 0);
    std::vector<std::uint8_t> packed_audio_payload(audio_payload_size);
    std::uint64_t mesh_work_budget_yields = 0;
    std::uint64_t mesh_receive_batch_max = 0;
    std::uint64_t last_local_grid_request_sequence =
        commands.state.grid_request_sequence.load(std::memory_order_acquire);
    std::optional<jam2::GridProposal> pending_local_grid_proposal;
    std::uint64_t next_grid_proposal_send_us = 0;
    std::uint64_t next_grid_assignment_send_us = 0;
    std::uint64_t sending_transport_revision = 0;
    std::uint64_t next_transport_send = 0;
    bool sent_current_transport = false;
    const std::uint64_t start_time = packet_schedule.startTimeUs();
    std::uint64_t next_stats = options.stats_enabled && options.stats_interval_ms > 0
        ? start_time + static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL
        : 0;
    // Public create/join validation measures established UDP audio, not
    // TCP/audio bootstrap time. Standalone internal lifecycle runs retain a
    // finite local deadline even when their intentional peer list is empty.
    const bool wait_for_first_peer = options.stream_ms > 0 && options.arm_stream_on_first_peer;
    std::uint64_t send_deadline = options.stream_ms > 0 && !wait_for_first_peer
        ? start_time + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL
        : UINT64_MAX;
    std::uint64_t receive_deadline = options.stream_ms > 0 && !wait_for_first_peer
        ? send_deadline + static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL
        : UINT64_MAX;
    bool timed_stream_clock_armed = options.stream_ms > 0 && !wait_for_first_peer;
    const std::uint64_t playout_delay_frames = options.jitter_buffer_frames > 0
        ? static_cast<std::uint64_t>(options.jitter_buffer_frames)
        : static_cast<std::uint64_t>(options.playout_delay_frames);

    std::uint64_t mesh_grid_proposals_sent = 0;
    std::uint64_t mesh_grid_assignments_sent = 0;
    std::uint64_t mesh_grid_authority_states_sent = 0;
    std::uint64_t mesh_transport_source_peer_id = 0;
    std::uint64_t mesh_transport_event_counter = 0;
    std::uint64_t mesh_transport_grid_revision = 0;
    jam2::EngineTransportAction mesh_transport_action = jam2::EngineTransportAction::None;
    std::uint64_t mesh_leader_audio_source_peer_id = 0;
    std::uint64_t mesh_leader_audio_injected_packets = 0;
    std::uint64_t mesh_transport_source_frame = 0;
    std::uint64_t mesh_transport_requested_target_frame = 0;
    std::uint64_t mesh_transport_applied_target_frame = 0;
    std::uint64_t mesh_compensation_stale_events = 0;
    bool mesh_compensation_was_stale = false;
    bool timed_stream_audio_detached = false;
    std::uint64_t last_authority_state_received_us = 0;
    std::uint64_t remote_authority_epoch_frame = 0;
    std::int64_t mesh_grid_base_offset_frames = 0;
    std::int64_t mesh_grid_target_offset_frames = 0;
    bool mesh_grid_target_valid = false;
    std::uint64_t mesh_grid_last_update_us = 0;

    auto grid_run_state_from_runtime = [&]() {
        return commands.state.metronome.load(std::memory_order_relaxed)
            ? jam2::GridRunState::Running
            : jam2::GridRunState::Stopped;
    };
    auto choose_safe_local_epoch = [&]() {
        std::uint64_t max_rtt_us = 0;
        for (const auto& entry : peers) {
            max_rtt_us = std::max(
                max_rtt_us,
                network_session.peerStream(entry.second.peer_id).stats().rtt_min_us);
        }
        const std::uint64_t rtt_frames = max_rtt_us *
            static_cast<std::uint64_t>(options.sample_rate) / 1000000ULL;
        const std::uint64_t lead_frames = std::clamp(
            rtt_frames + static_cast<std::uint64_t>(options.sample_rate) / 5ULL,
            static_cast<std::uint64_t>(options.sample_rate) / 10ULL,
            static_cast<std::uint64_t>(options.sample_rate) / 2ULL);
        // Every explicit local start/revision gets a fresh bounded epoch. Do
        // not quantize from an earlier membership's epoch: a past epoch can
        // otherwise defer the requested start by almost a full later bar.
        return current_engine_frame(audio.engine.get()) + lead_frames;
    };
    auto apply_authority_role = [&]() {
        const auto& grid = authority.grid();
        const bool local_authority = authority.localIsGridAuthority();
        commands.state.metronome_local_authority.store(local_authority, std::memory_order_relaxed);
        commands.state.leader_audio_local_click.store(
            local_authority && grid.run_state == jam2::GridRunState::Running &&
                grid.mode == metronome_mode_id(MetronomeMode::LeaderAudio),
            std::memory_order_relaxed);
    };
    auto activate_local_grid = [&]() {
        const std::uint64_t packet_frame = current_engine_frame(audio.engine.get());
        const std::uint64_t epoch = choose_safe_local_epoch();
        if (!authority.activateLocalGrid(epoch, packet_frame)) {
            return false;
        }
        const auto& grid = authority.grid();
        commands.state.metronome.store(
            grid.run_state == jam2::GridRunState::Running,
            std::memory_order_relaxed);
        commands.state.metronome_mode.store(grid.mode, std::memory_order_relaxed);
        commands.state.metronome_epoch_sample_time.store(epoch, std::memory_order_relaxed);
        commands.state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        mesh_grid_base_offset_frames = 0;
        commands.state.metronome_revision.store(grid.revision, std::memory_order_relaxed);
        mesh_grid_target_valid = false;
        apply_authority_role();
        return true;
    };
    auto clear_departed_authority_state = [&]() {
        commands.state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
        commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
        commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
        pending_local_grid_proposal.reset();
        last_authority_state_received_us = 0;
        remote_authority_epoch_frame = 0;
        mesh_grid_base_offset_frames = 0;
        mesh_grid_target_offset_frames = 0;
        mesh_grid_target_valid = false;
        mesh_grid_last_update_us = 0;
        {
            std::lock_guard<std::mutex> lock(commands.state.transport_mutex);
            commands.state.transport_pending.store(false, std::memory_order_release);
            commands.state.transport_network_revision.store(0, std::memory_order_relaxed);
            commands.state.transport_network_target_raw_frame.store(0, std::memory_order_relaxed);
            commands.state.transport_network_action.store(0, std::memory_order_relaxed);
        }
        mesh_transport_source_peer_id = 0;
        mesh_transport_event_counter = 0;
        mesh_transport_grid_revision = 0;
        mesh_transport_action = jam2::EngineTransportAction::None;
        mesh_transport_source_frame = 0;
        mesh_transport_requested_target_frame = 0;
        mesh_transport_applied_target_frame = 0;
        if (audio.engine != nullptr) {
            jam2::EngineCommand cancel;
            cancel.type = jam2::EngineCommandType::CancelTransport;
            (void)audio.engine->submit(cancel);
        }
    };
    auto restore_running_grid_after_departure = [&] (
        bool was_running,
        std::uint64_t departed_peer_id) {
        if (!was_running) {
            return;
        }
        const jam2::GridProposal proposal{
            local_peer_id.value,
            runtime_host.nextGridRequestId(),
            jam2::GridRunState::Running,
            static_cast<std::uint8_t>(
                commands.state.metronome_mode.load(std::memory_order_relaxed)),
            0,
        };
        if (authority.localIsBootstrapCoordinator()) {
            if (authority.orderGridProposal(proposal)) {
                pending_local_grid_proposal.reset();
                (void)activate_local_grid();
                next_grid_assignment_send_us = 0;
                return;
            }
        } else if (authority.bootstrapCoordinatorPeerId() != departed_peer_id) {
            pending_local_grid_proposal = proposal;
            next_grid_proposal_send_us = 0;
            return;
        }
        commands.state.metronome.store(false, std::memory_order_relaxed);
    };
    auto align_to_authority_clock = [&](const MetronomePayload& metronome,
                                        std::uint64_t authority_packet_frame,
                                        const jam2::PeerStream& stream) {
        if (stream.stats().rtt_min_us == 0 || audio.engine == nullptr) {
            return false;

        }
        const std::uint64_t one_way_frames = stream.stats().rtt_min_us *
            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
        if (authority_packet_frame >
            (std::numeric_limits<std::uint64_t>::max)() - one_way_frames) {
            return false;
        }
        const std::uint64_t projected = authority_packet_frame + one_way_frames;
        const auto mapping = jam2::metronome::map_authority_clock(
            metronome.epoch_sample_time,
            projected,
            current_engine_frame(audio.engine.get()));
        if (!mapping.valid) {
            return false;
        }
        commands.state.metronome_epoch_sample_time.store(
            mapping.epoch_sample_time,
            std::memory_order_relaxed);
        commands.state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        commands.state.metronome_render_offset_frames.store(
            mapping.render_offset_frames,
            std::memory_order_relaxed);
        mesh_grid_base_offset_frames = mapping.render_offset_frames;
        mesh_grid_target_offset_frames = mapping.render_offset_frames;
        mesh_grid_target_valid = true;
        return true;
    };

    commands.state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);

    commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
    commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
    if (authority.localIsBootstrapCoordinator()) {
        if (authority.orderGridProposal({
                local_peer_id.value,
                runtime_host.nextGridRequestId(),
                grid_run_state_from_runtime(),
                static_cast<std::uint8_t>(
                    commands.state.metronome_mode.load(std::memory_order_relaxed)),
                0,
            })) {
            (void)activate_local_grid();
        }
    }

    AudioPacketStats retired_peer_stats;
    auto aggregate_stats = [&]() {
        AudioPacketStats stats = retired_peer_stats;
        stats.os_scheduling = os_priority_scope.status();
        stats.local_peer_id = network_session.localPeerId().value;
        switch (network_session.bootstrapRole()) {
        case jam2::SessionBootstrapRole::Creator: stats.bootstrap_role = "creator"; break;
        case jam2::SessionBootstrapRole::Joiner: stats.bootstrap_role = "joiner"; break;
        }
        stats.session_protocol_version = network_session.contract().protocol_version;
        stats.session_audio_format = jam2::protocol::audio_format_text(options.network_audio_format);
        stats.session_sample_rate = network_session.contract().sample_rate;
        stats.session_frames_per_packet = network_session.contract().frames_per_packet;
        stats.network_peer_count = network_session.peerCount();
        if (peers.size() == 1) {
            stats.remote_peer_id = peers.begin()->second.peer_id.value;
        } else if (peers.empty()) {
            stats.remote_peer_id = last_single_remote_peer_id;
        }
        const auto& grid = authority.grid();
        const auto& authority_stats = authority.stats();
        stats.bootstrap_coordinator_peer_id = authority.bootstrapCoordinatorPeerId();
        stats.arrangement_authority_peer_id = authority.arrangementAuthorityPeerId();
        stats.grid_authority_peer_id = grid.authority_peer_id;
        stats.grid_revision = grid.revision;
        stats.grid_run_state = static_cast<std::uint64_t>(grid.run_state);
        stats.grid_mode = grid.mode;
        stats.grid_authority_epoch_frame = grid.authority_epoch_frame;
        stats.grid_mapped_epoch_frame =
            commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        stats.grid_authority_packet_frame = grid.authority_packet_frame;
        stats.grid_proposals_accepted = authority_stats.grid_proposals_accepted;
        stats.grid_proposals_rejected = authority_stats.grid_proposals_rejected;
        stats.grid_assignments_accepted = authority_stats.grid_assignments_accepted;
        stats.grid_assignments_rejected = authority_stats.grid_assignments_rejected;
        stats.grid_authority_states_accepted = authority_stats.grid_authority_states_accepted;
        stats.grid_authority_states_rejected = authority_stats.grid_authority_states_rejected;
        stats.grid_authority_missing_events = authority_stats.grid_authority_missing_events;
        stats.transport_events_accepted = authority_stats.transport_events_accepted;
        stats.transport_events_rejected = authority_stats.transport_events_rejected;
        stats.grid_proposals_sent = mesh_grid_proposals_sent;
        stats.grid_assignments_sent = mesh_grid_assignments_sent;
        stats.grid_authority_states_sent = mesh_grid_authority_states_sent;
        stats.metronome_sent = mesh_grid_authority_states_sent;
        stats.metronome_received = authority_stats.grid_authority_states_accepted;
        stats.grid_mapping_error_frames = mesh_grid_target_valid
            ? mesh_grid_target_offset_frames -
                commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed)
            : 0;
        stats.transport_source_peer_id = mesh_transport_source_peer_id;
        stats.transport_event_counter = mesh_transport_event_counter;
        stats.transport_grid_revision = mesh_transport_grid_revision;
        stats.transport_action = static_cast<std::uint64_t>(mesh_transport_action);
        stats.leader_audio_source_peer_id = mesh_leader_audio_source_peer_id;
        stats.leader_audio_injected_packets = mesh_leader_audio_injected_packets;
        stats.transport_source_frame = mesh_transport_source_frame;
        stats.transport_requested_target_frame = mesh_transport_requested_target_frame;
        stats.transport_applied_target_frame = mesh_transport_applied_target_frame;
        stats.metronome_compensation_stale_events = mesh_compensation_stale_events;
        stats.final_metronome_enabled = commands.state.metronome.load(std::memory_order_relaxed);
        stats.final_bpm = commands.state.bpm.load(std::memory_order_relaxed);
        stats.final_metronome_level = gain_from_ppm(
            commands.state.metronome_level_ppm.load(std::memory_order_relaxed));
        stats.final_remote_level = gain_from_ppm(
            commands.state.remote_level_ppm.load(std::memory_order_relaxed));
        stats.final_send_level = gain_from_ppm(
            commands.state.send_level_ppm.load(std::memory_order_relaxed));
        stats.final_local_monitor_enabled =
            commands.state.local_monitor.load(std::memory_order_relaxed);
        stats.final_local_monitor_level = gain_from_ppm(
            commands.state.local_monitor_level_ppm.load(std::memory_order_relaxed));
        stats.startup_drained_packets = static_cast<std::uint64_t>(drained_startup_packets);
        stats.sample_time_playout_enabled = true;
        stats.playout_delay_frames = playout_delay_frames;
        stats.jitter_buffer_enabled = playout_delay_frames > 0;
        stats.jitter_buffer_target_frames = playout_delay_frames;
        stats.jitter_buffer_max_frames = static_cast<std::uint64_t>(options.jitter_buffer_max_frames);
        stats.udp_work_budget_yields = mesh_work_budget_yields;
        stats.udp_receive_batch_max = mesh_receive_batch_max;
        for (const auto& entry : peers) {
            const auto& peer = entry.second;
            stats.sent_packets += peer.sent_packets;
            stats.sent_bytes += peer.sent_bytes;
            stats.recv_packets += peer.recv_packets;
            stats.recv_bytes += peer.recv_bytes;
            stats.ignored_packets += peer.ignored_packets;
            stats.udp_parse.add(peer.udp_parse);
            stats.sent_pings += peer.sent_pings;
            stats.sent_pongs += peer.sent_pongs;
            stats.recv_pongs += peer.recv_pongs;
            add_peer_stream_stats(stats, network_session.peerStream(peer.peer_id).stats());
        }
        const auto pattern = metronome_pattern_from_runtime(commands.state);
        const std::uint64_t beat_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate), pattern.bpm, pattern.division) *
            static_cast<std::uint64_t>(std::max(1, pattern.division));
        const bool epoch_valid = commands.state.metronome_epoch_valid.load(std::memory_order_relaxed);
        const std::uint64_t mapped_epoch =
            commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        stats.metronome_epoch_sample_time = mapped_epoch;
        if (epoch_valid && beat_frames > 0 && grid.run_state == jam2::GridRunState::Running) {
            const std::uint64_t local_frame = musical_frame_from_raw(
                current_engine_frame(audio.engine.get()),
                commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed));
            stats.local_metronome_beat = local_frame >= mapped_epoch
                ? (local_frame - mapped_epoch) / beat_frames
                : 0ULL;
            if (authority.localIsGridAuthority()) {
                stats.remote_metronome_beat = stats.local_metronome_beat;
                stats.metronome_alignment_valid = true;
            } else {
                for (const auto& entry : peers) {
                    const auto& peer = entry.second;
                    if (peer.peer_id.value != grid.authority_peer_id) {
                        continue;
                    }
                    const auto& peer_stats = network_session.peerStream(peer.peer_id).stats();
                    if (peer_stats.last_received_sample_time >= grid.authority_epoch_frame) {
                        stats.remote_metronome_beat =
                            (peer_stats.last_received_sample_time - grid.authority_epoch_frame) / beat_frames;
                        const std::uint64_t difference = stats.local_metronome_beat > stats.remote_metronome_beat
                            ? stats.local_metronome_beat - stats.remote_metronome_beat
                            : stats.remote_metronome_beat - stats.local_metronome_beat;
                        stats.metronome_alignment_valid = difference <= 1ULL;
                    }
                    break;
                }
            }
        }
        stats.metronome_compensation_offset_frames =
            commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
        stats.metronome_compensation_target_frames = mesh_grid_target_valid
            ? mesh_grid_target_offset_frames
            : stats.metronome_compensation_offset_frames;
        stats.metronome_compensation_active =
            commands.state.metronome_mode.load(std::memory_order_relaxed) ==
                metronome_mode_id(MetronomeMode::ListenerCompensated) &&
            !authority.localIsGridAuthority() && epoch_valid && mesh_grid_target_valid;
        copy_peer_mixer_stats(stats, network_session.mixStats());
        stats.udp_work_budget_yields += network_session.mixStats().work_budget_yields;
        return stats;
    };

    auto print_mesh_stats = [&](std::uint64_t now_us, const CsvStatsLog::AudioSnapshot* provided_audio_snapshot = nullptr) {
        const std::uint64_t elapsed_ms = (now_us - start_time) / 1000ULL;
        const AudioPacketStats stats = aggregate_stats();
        CsvStatsLog::AudioSnapshot current_audio_snapshot;
        if (provided_audio_snapshot == nullptr) {
            current_audio_snapshot = make_audio_snapshot(audio.engine.get());
            provided_audio_snapshot = &current_audio_snapshot;
        }
        const auto& audio_snapshot = *provided_audio_snapshot;
        const auto engine_snapshot = audio.engine ? audio.engine->snapshot() : jam2::EngineSnapshot{};
        {
            std::cout << "mesh_stats elapsed_ms=" << elapsed_ms
                      << " peer_count=" << peers.size()
                      << " metronome=" << (commands.state.metronome.load(std::memory_order_relaxed) ? "on" : "off")
                      << " bpm=" << commands.state.bpm.load(std::memory_order_relaxed)
                      << " metronome_beats_per_bar=" << commands.state.metronome_beats_per_bar.load(std::memory_order_relaxed)
                      << " metronome_division=" << commands.state.metronome_division.load(std::memory_order_relaxed)
                      << " metronome_epoch_sample_frame=" << commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed)
                      << " engine_frame=" << engine_snapshot.engine_frame
                      << " metronome_render_offset_frames=" << commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed)
                      << " audio_sample_rate=" << options.sample_rate
                      << " sent_packets=" << stats.sent_packets
                      << " recv_packets=" << stats.recv_packets
                      << " sequence_lost=" << stats.sequence.lost
                      << " sequence_loss_percent=" << sequence_loss_percent(stats)
                      << " jitter_avg_ms=" << avg_us_to_ms(stats.jitter_sum_us, stats.jitter_samples)
                      << " rtt_avg_ms=" << rtt_avg_ms(stats)
                      << " active_peers=" << stats.network_active_peer_count
                      << " mix_released_slots=" << stats.mix_released_slots
                      << " mix_deadline_slots=" << stats.mix_deadline_slots
                      << " mix_missing_peer_frames=" << stats.mix_missing_peer_frames
                      << " mix_capacity_drops=" << stats.mix_capacity_drops
                      << " playback_ring_readable_ms=" << frames_to_ms(audio_snapshot.playback_ring_readable, options.sample_rate)
                      << " remote_peak=" << audio_snapshot.remote_peak
                      << " output_peak=" << audio_snapshot.output_peak << "\n";
            for (const auto& entry : peers) {
                const auto& peer = entry.second;
                const auto& peer_stats = network_session.peerStream(peer.peer_id).stats();
                const auto* mix_stats = network_session.peerMixStats(peer.peer_id);
                std::cout << "mesh_peer endpoint=" << jam2::endpoint_to_string(peer.endpoint)
                          << " peer_id=" << peer.peer_id.value
                          << " endpoint_proof_state=" << endpoint_proof_name(peer.endpoint_proof)
                          << " endpoint_proof_attempts=" << peer.proof_attempts
                          << " endpoint_proof_successes=" << peer.proof_successes
                          << " endpoint_proof_failures=" << peer.proof_failures
                          << " endpoint_unverified_drops=" << peer.proof_unverified_drops
                          << " endpoint_unmatched_pongs=" << peer.proof_unmatched_pongs
                          << " endpoint_challenge_overwrites=" << peer.proof_challenge_overwrites
                          << " sent_packets=" << peer.sent_packets
                          << " recv_packets=" << peer.recv_packets
                          << " ignored_packets=" << peer.ignored_packets
                          << " sequence_lost=" << peer_stats.sequence.lost
                          << " sequence_duplicate=" << peer_stats.sequence.duplicate
                          << " sequence_out_of_order=" << peer_stats.sequence.out_of_order
                          << " sequence_late=" << peer_stats.sequence.late
                          << " drift_ppm=" << peer_stats.drift_ppm
                          << " resampler_ratio=" << peer_stats.resampler_ratio
                          << " mix_queue_depth_frames=" << (mix_stats != nullptr ? mix_stats->queue_depth_frames : 0ULL)
                          << " mix_queue_high_water_frames=" << (mix_stats != nullptr ? mix_stats->queue_high_water_frames : 0ULL)
                          << " mix_late_after_release_frames=" << (mix_stats != nullptr ? mix_stats->late_after_release_frames : 0ULL)
                          << "\n";
            }
        }
        if (runtime_host.network_snapshot) {
            runtime_host.network_snapshot(network_session.snapshot());
        }
    };

    auto apply_membership_update = [&]() {
        auto update = runtime_host.takePeerUpdate();
        if (!update) {
            return;
        }
        try {
        std::map<std::uint64_t, jam2::Endpoint> desired;
        for (const Jam2RuntimePeer& peer : *update) {
            const jam2::Endpoint endpoint = jam2::resolve_udp_endpoint(peer.endpoint);
            const std::uint64_t peer_id = peer.peer_id;
            if (peer_id == local_peer_id.value || !desired.emplace(peer_id, endpoint).second) {
                throw std::runtime_error("embedded peer update contains a duplicate/local peer identity");
            }
        }

        std::size_t removed = 0;
        std::size_t endpoint_updates = 0;
        for (auto it = peers.begin(); it != peers.end();) {
            const std::uint64_t peer_id = it->second.peer_id.value;
            const auto wanted = desired.find(peer_id);
            if (wanted == desired.end()) {
                const auto& retired = it->second;
                retired_peer_stats.sent_packets += retired.sent_packets;
                retired_peer_stats.sent_bytes += retired.sent_bytes;
                retired_peer_stats.recv_packets += retired.recv_packets;
                retired_peer_stats.recv_bytes += retired.recv_bytes;
                retired_peer_stats.ignored_packets += retired.ignored_packets;
                retired_peer_stats.udp_parse.add(retired.udp_parse);

                retired_peer_stats.sent_pings += retired.sent_pings;
                retired_peer_stats.sent_pongs += retired.sent_pongs;
                retired_peer_stats.recv_pongs += retired.recv_pongs;
                add_peer_stream_stats(

                    retired_peer_stats,
                    network_session.peerStream(retired.peer_id).stats());
                const bool departed_grid_authority =
                    peer_id == authority.grid().authority_peer_id;
                const bool grid_was_running = departed_grid_authority &&
                    authority.grid().run_state == jam2::GridRunState::Running;
                (void)authority.markPeerInactive(peer_id);
                if (departed_grid_authority) {
                    clear_departed_authority_state();
                    restore_running_grid_after_departure(grid_was_running, peer_id);
                }
                (void)network_session.removePeer(it->second.peer_id);

                it = peers.erase(it);
                ++removed;
                continue;
            }
            if (it->second.endpoint.host != wanted->second.host ||
                it->second.endpoint.port != wanted->second.port) {
                if (!network_session.updatePeerEndpoint(
                        it->second.peer_id,
                        wanted->second,
                        jam2::PeerEndpointState::Candidate)) {
                    throw std::runtime_error("embedded peer endpoint update was rejected");
                }
                auto node = peers.extract(it++);
                node.key() = endpoint_key(wanted->second);
                node.mapped().endpoint = wanted->second;
                node.mapped().endpoint_proof = EndpointProofState::Candidate;
                node.mapped().probe_challenges = {};
                node.mapped().proof_attempts = 0;
                node.mapped().proof_deadline_us = 0;
                node.mapped().next_probe_us = 0;
                if (!peers.insert(std::move(node)).inserted) {
                    throw std::runtime_error("embedded peer endpoint update collided with another endpoint");

                }
                ++endpoint_updates;
                continue;
            }
            ++it;
        }

        std::size_t added = 0;
        for (const auto& [peer_id, endpoint] : desired) {
            if (network_session.peer(jam2::PeerId{peer_id}) != nullptr) {
                continue;
            }
            if (!network_session.addPeer(
                    {jam2::PeerId{peer_id}, endpoint, jam2::PeerEndpointState::Candidate},
                    peer_stream_config)) {
                throw std::runtime_error("embedded peer add was rejected");
            }
            if (!peers.emplace(
                    endpoint_key(endpoint),
                    MeshPeerState{endpoint, jam2::PeerId{peer_id}}).second) {
                (void)network_session.removePeer(jam2::PeerId{peer_id});
                throw std::runtime_error("embedded peer add collided with another endpoint");
            }
            ++added;
        }
        if (peers.size() == 1) {
            last_single_remote_peer_id = peers.begin()->second.peer_id.value;
        }
        std::cout << "Embedded mesh peer update: added=" << added
                  << " removed=" << removed
                  << " endpoint_updates=" << endpoint_updates
                  << " peers=" << peers.size() << "\n";
        } catch (const std::exception& error) {
            // This runs outside the real-time callback and is intentionally
            // retained: a rejected coordinator membership update must leave a
            // useful cause in CLI/stress logs instead of only exit code 1.
            std::cout << "Embedded mesh peer update failed: " << error.what() << "\n";
            throw;
        }
    };

    while (jam2::monotonic_us() < receive_deadline &&
           !commands.state.quit.load(std::memory_order_relaxed) &&
           !runtime_host.stop_requested.load(std::memory_order_acquire)) {
        apply_runtime_host_commands();
        apply_membership_update();
        const std::uint64_t now = jam2::monotonic_us();
        if (!timed_stream_clock_armed && options.stream_ms > 0 && network_session.activePeerCount() > 0) {
            send_deadline = now + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL;
            receive_deadline = send_deadline +
                static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL;
            timed_stream_clock_armed = true;
        }
        const std::uint64_t local_grid_request_sequence =
            commands.state.grid_request_sequence.load(std::memory_order_acquire);
        if (local_grid_request_sequence != last_local_grid_request_sequence) {
            last_local_grid_request_sequence = local_grid_request_sequence;
            jam2::GridProposal proposal{
                local_peer_id.value,
                runtime_host.nextGridRequestId(),
                grid_run_state_from_runtime(),
                static_cast<std::uint8_t>(
                    commands.state.metronome_mode.load(std::memory_order_relaxed)),
                commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
            };
            if (authority.localIsBootstrapCoordinator()) {
                if (authority.orderGridProposal(proposal)) {
                    pending_local_grid_proposal.reset();
                    (void)activate_local_grid();
                    next_grid_assignment_send_us = 0;
                }
            } else {
                pending_local_grid_proposal = proposal;
                next_grid_proposal_send_us = 0;
                commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
                commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
            }
        }
        const int grid_mode = commands.state.metronome_mode.load(std::memory_order_relaxed);
        const bool listener_compensated =
            grid_mode == metronome_mode_id(MetronomeMode::ListenerCompensated);
        if (listener_compensated && !authority.localIsGridAuthority()) {
            const jam2::PeerId authority_peer{authority.grid().authority_peer_id};
            const jam2::PeerStream* authority_stream = nullptr;
            for (const auto& entry : peers) {
                if (entry.second.peer_id == authority_peer) {
                    authority_stream = &network_session.peerStream(authority_peer);
                    break;
                }
            }
            const auto* authority_mix = network_session.peerMixStats(authority_peer);
            const bool fresh = authority_stream != nullptr && authority_mix != nullptr &&
                authority.grid().run_state != jam2::GridRunState::AuthorityMissing &&
                last_authority_state_received_us != 0 &&
                now - last_authority_state_received_us <= 500000ULL &&
                authority_stream->playoutSampleTimeInitialized() &&
                commands.state.metronome_epoch_valid.load(std::memory_order_relaxed);
            if (fresh) {
                const std::uint64_t remote_head =
                    authority_stream->nextPlayoutRemoteSampleTime() > authority_mix->queue_depth_frames
                    ? authority_stream->nextPlayoutRemoteSampleTime() - authority_mix->queue_depth_frames
                    : 0ULL;
                const std::uint64_t local_frame = musical_frame_from_raw(
                    current_engine_frame(audio.engine.get()),
                    commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed));
                const std::uint64_t local_epoch =
                    commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
                const auto pattern = metronome_pattern_from_runtime(commands.state);
                const std::int64_t interval = static_cast<std::int64_t>(
                    jam2::metronome::step_interval_samples(
                        static_cast<double>(options.sample_rate), pattern.bpm, pattern.division));
                if (interval > 0) {
                    auto phase = [&](std::uint64_t frame, std::uint64_t epoch) {
                        const std::int64_t position = frame >= epoch
                            ? static_cast<std::int64_t>(frame - epoch)
                            : -static_cast<std::int64_t>(epoch - frame);
                        const std::int64_t value = position % interval;
                        return value < 0 ? value + interval : value;
                    };
                    std::int64_t error = phase(remote_head, remote_authority_epoch_frame) -
                        phase(local_frame, local_epoch);
                    if (error > interval / 2) error -= interval;
                    if (error < -interval / 2) error += interval;
                    mesh_grid_target_offset_frames =
                        commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed) + error;
                    mesh_grid_target_valid = true;
                }
                mesh_compensation_was_stale = false;
            } else if (!mesh_compensation_was_stale) {
                ++mesh_compensation_stale_events;
                mesh_compensation_was_stale = true;
            }
        }
        const bool correction_enabled = !authority.localIsGridAuthority() &&
            (grid_mode == metronome_mode_id(MetronomeMode::SharedGrid) || listener_compensated) &&
            mesh_grid_target_valid;
        if (correction_enabled &&
            (mesh_grid_last_update_us == 0 || now - mesh_grid_last_update_us >= 10000ULL)) {
            const std::int64_t current =
                commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
            const std::int64_t max_frames = std::abs(ms_to_signed_frames(
                options.metronome_compensation_max_ms, options.sample_rate));
            const std::int64_t target = mesh_grid_base_offset_frames + std::clamp(
                mesh_grid_target_offset_frames - mesh_grid_base_offset_frames,
                -max_frames,
                max_frames);
            const double elapsed_ms = mesh_grid_last_update_us == 0
                ? 10.0
                : static_cast<double>(now - mesh_grid_last_update_us) / 1000.0;
            const double alpha = options.metronome_compensation_smoothing_ms > 0.0
                ? std::clamp(elapsed_ms / options.metronome_compensation_smoothing_ms, 0.0, 1.0)
                : 1.0;
            std::int64_t step = static_cast<std::int64_t>(
                std::llround(static_cast<double>(target - current) * alpha));
            const std::int64_t max_step = std::abs(ms_to_signed_frames(
                options.metronome_compensation_slew_ms_per_sec * elapsed_ms / 1000.0,
                options.sample_rate));
            if (max_step > 0) {
                step = std::clamp(step, -max_step, max_step);
            }
            commands.state.metronome_render_offset_frames.store(
                current + step,
                std::memory_order_relaxed);
            mesh_grid_last_update_us = now;
        }
        commit_due_transport(commands.state, audio.engine.get());
        sync_engine_control(commands.state, audio.engine.get(), engine_control_mirror);
        if (!timed_stream_audio_detached && now >= send_deadline) {
            detach_network_capture(audio);
            mesh_playback.detach();
            timed_stream_audio_detached = true;
        }
        network_session.advance(now);

        int sends_this_loop = 0;
        while (now >= packet_schedule.nextAudioSendUs() &&
               packet_schedule.nextAudioSendUs() < receive_deadline &&
               sends_this_loop < 8) {
            std::span<const std::uint8_t> payload = silence_payload;
            if (audio.engine != nullptr && !timed_stream_audio_detached) {
                const auto captured = audio.engine->popNetworkCapture(audio.network_capture, asio_frames);
                if (captured.frames < static_cast<std::size_t>(options.frame_size)) {
                    break;
                }
                for (std::size_t i = 0; i < asio_frames.size(); ++i) {
                    network_frames[i] = asio_frames[i] / 256;
                }
                apply_send_level(network_frames, commands.state.send_level_ppm.load(std::memory_order_relaxed));
                if (commands.state.metronome_mode.load(std::memory_order_relaxed) ==
                        metronome_mode_id(MetronomeMode::LeaderAudio) &&
                    commands.state.leader_audio_local_click.load(std::memory_order_relaxed) &&
                    commands.state.metronome.load(std::memory_order_relaxed)) {
                    mix_leader_click_into_packet(
                        network_frames,
                        packet_schedule.sampleTime(),
                        options.sample_rate,
                        gain_from_ppm(commands.state.metronome_level_ppm.load(std::memory_order_relaxed)),
                        commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                        metronome_pattern_from_runtime(commands.state));
                    mesh_leader_audio_source_peer_id = local_peer_id.value;
                    ++mesh_leader_audio_injected_packets;
                }
                (void)jam2::protocol::pack_audio_into(
                    options.network_audio_format,
                    network_frames,
                    packed_audio_payload);
                payload = packed_audio_payload;
            } else if (commands.state.metronome_mode.load(std::memory_order_relaxed) ==
                           metronome_mode_id(MetronomeMode::LeaderAudio) &&
                       commands.state.leader_audio_local_click.load(std::memory_order_relaxed) &&
                       commands.state.metronome.load(std::memory_order_relaxed)) {
                std::fill(network_frames.begin(), network_frames.end(), 0);
                mix_leader_click_into_packet(
                    network_frames,
                    packet_schedule.sampleTime(),
                    options.sample_rate,
                    gain_from_ppm(commands.state.metronome_level_ppm.load(std::memory_order_relaxed)),
                    commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                    metronome_pattern_from_runtime(commands.state));
                mesh_leader_audio_source_peer_id = local_peer_id.value;
                ++mesh_leader_audio_injected_packets;
                (void)jam2::protocol::pack_audio_into(
                    options.network_audio_format,
                    network_frames,
                    packed_audio_payload);
                payload = packed_audio_payload;
            }
            const auto send_result = network_session.sendToActive(
                jam2::protocol::PacketType::Audio,
                packet_schedule.audioSequence(),
                packet_schedule.sampleTime(),
                payload);
            for (auto& entry : peers) {
                auto& peer = entry.second;
                if (peer.endpoint_proof != EndpointProofState::Active) {
                    continue;
                }
                ++peer.sent_packets;
                peer.sent_bytes += send_result.packet_size;
            }
            packet_schedule.commitAudioPacket();
            ++sends_this_loop;
        }

        if (now >= packet_schedule.nextPingUs() && now < send_deadline) {
            for (auto& entry : peers) {
                auto& peer = entry.second;
                if (peer.endpoint_proof == EndpointProofState::Probing &&
                    peer.proof_attempts >= 8 &&
                    now >= peer.proof_deadline_us) {
                    peer.endpoint_proof = EndpointProofState::Failed;
                    network_session.setPeerEndpointState(peer.peer_id, jam2::PeerEndpointState::Failed);
                    ++peer.proof_failures;
                    peer.next_probe_us = now + 5000000ULL;
                }
                if (peer.endpoint_proof == EndpointProofState::Failed) {
                    if (now < peer.next_probe_us) {
                        continue;
                    }
                    peer.endpoint_proof = EndpointProofState::Candidate;
                    network_session.setPeerEndpointState(peer.peer_id, jam2::PeerEndpointState::Candidate);
                    peer.proof_attempts = 0;
                    peer.proof_deadline_us = 0;
                    for (ProbeChallenge& challenge : peer.probe_challenges) {
                        challenge.used = false;
                    }
                }
                if (peer.endpoint_proof != EndpointProofState::Active) {
                    if (peer.proof_attempts >= 8 || now < peer.next_probe_us) {
                        continue;

                    }
                    peer.endpoint_proof = EndpointProofState::Probing;
                    network_session.setPeerEndpointState(peer.peer_id, jam2::PeerEndpointState::Probing);

                    peer.next_probe_us = now + 250000ULL;
                }
                const std::uint32_t ping_sequence = packet_schedule.takeControlSequence();
                ProbeChallenge& challenge =
                    peer.probe_challenges[ping_sequence % peer.probe_challenges.size()];
                if (challenge.used) {
                    ++peer.proof_challenge_overwrites;
                }
                challenge = ProbeChallenge{ping_sequence, now, true};
                if (peer.endpoint_proof == EndpointProofState::Probing) {
                    ++peer.proof_attempts;
                    if (peer.proof_attempts == 8) {

                        peer.proof_deadline_us = now + 1000000ULL;
                    }
                }
                network_session.sendToPeer(
                    peer.peer_id,
                    jam2::protocol::PacketType::Ping,
                    ping_sequence,
                    now,
                    {},
                    true);
                ++peer.sent_pings;
            }
            packet_schedule.commitPing();
        }

        if (now >= packet_schedule.nextGridStateUs() && now < send_deadline) {
            const int bpm = commands.state.bpm.load(std::memory_order_relaxed);
            const auto pattern = metronome_pattern_from_runtime(commands.state);
            auto make_grid_payload = [&](GridMessageKind kind,
                                         std::uint64_t revision_or_request,
                                         std::uint64_t epoch,
                                         std::uint8_t mode,

                                         jam2::GridRunState run_state) {
                return encode_metronome_payload(
                    run_state == jam2::GridRunState::Running ? bpm : -bpm,
                    revision_or_request,
                    epoch,
                    pattern,
                    kind,
                    mode,
                    run_state);
            };
            if (pending_local_grid_proposal && now >= next_grid_proposal_send_us) {
                const auto& proposal = *pending_local_grid_proposal;
                const auto payload = make_grid_payload(
                    GridMessageKind::Proposal,
                    proposal.request_id,
                    proposal.proposed_epoch_frame,
                    proposal.mode,
                    proposal.run_state);
                network_session.sendToPeer(
                    jam2::PeerId{authority.bootstrapCoordinatorPeerId()},
                    jam2::protocol::PacketType::MetronomeState,
                    packet_schedule.takeControlSequence(),
                    current_engine_frame(audio.engine.get()),
                    payload);
                ++mesh_grid_proposals_sent;
                next_grid_proposal_send_us = now + 20000ULL;
            }
            if (authority.localIsBootstrapCoordinator() && authority.grid().revision != 0 &&
                now >= next_grid_assignment_send_us) {
                const auto& grid = authority.grid();
                const auto payload = make_grid_payload(
                    GridMessageKind::Assignment,
                    grid.revision,
                    grid.authority_epoch_frame,
                    grid.mode,
                    grid.run_state);
                network_session.sendToActive(
                    jam2::protocol::PacketType::MetronomeState,
                    packet_schedule.takeControlSequence(),
                    grid.authority_peer_id,
                    payload);
                ++mesh_grid_assignments_sent;
                next_grid_assignment_send_us = now + 100000ULL;
            }
            if (authority.localIsGridAuthority() &&
                commands.state.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                const auto& grid = authority.grid();
                const auto payload = make_grid_payload(
                    GridMessageKind::AuthorityState,
                    grid.revision,
                    commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                    grid.mode,
                    grid.run_state);
                network_session.sendToActive(
                    jam2::protocol::PacketType::MetronomeState,
                    packet_schedule.takeControlSequence(),
                    current_engine_frame(audio.engine.get()),
                    payload);
                ++mesh_grid_authority_states_sent;
            }
            packet_schedule.scheduleNextGridState(now, 20000ULL);
        }

        std::uint64_t network_transport_revision = 0;
        std::uint64_t transport_target = 0;
        int transport_action = 0;
        {
            std::lock_guard<std::mutex> lock(commands.state.transport_mutex);
            network_transport_revision = commands.state.transport_network_revision.load(std::memory_order_relaxed);
            transport_target = commands.state.transport_network_target_raw_frame.load(std::memory_order_relaxed);
            transport_action = commands.state.transport_network_action.load(std::memory_order_relaxed);
        }
        if (network_transport_revision != sending_transport_revision) {
            sending_transport_revision = network_transport_revision;
            next_transport_send = 0;
            sent_current_transport = false;
        }
        const bool sending_track_transport = jam2::is_track_sync_transport_action(
            static_cast<jam2::EngineTransportAction>(transport_action));
        const bool may_send_transport = sending_track_transport
            ? runtime_host.track_sync_enabled.load(std::memory_order_acquire)
            : authority.localIsArrangementAuthority();
        if (may_send_transport &&
            authority.grid().revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            sending_transport_revision != 0 &&
            sending_transport_revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            now >= next_transport_send &&
            (!sent_current_transport || current_engine_frame(audio.engine.get()) <= transport_target) &&
            now < send_deadline) {
            const std::uint64_t engine_now = current_engine_frame(audio.engine.get());
            const auto payload = encode_transport_payload({
                static_cast<jam2::EngineTransportAction>(
                    transport_action),
                static_cast<std::uint32_t>(sending_transport_revision),
                static_cast<std::uint32_t>(authority.grid().revision),
                transport_target,
            });
            network_session.sendToActive(
                jam2::protocol::PacketType::TransportState,
                packet_schedule.takeControlSequence(),
                engine_now,
                payload);
            mesh_transport_source_peer_id = local_peer_id.value;
            mesh_transport_event_counter = sending_transport_revision;
            mesh_transport_grid_revision = authority.grid().revision;
            mesh_transport_action = static_cast<jam2::EngineTransportAction>(transport_action);
            mesh_transport_source_frame = engine_now;
            mesh_transport_requested_target_frame = transport_target;
            mesh_transport_applied_target_frame = transport_target;
            sent_current_transport = true;
            next_transport_send = now + 20000ULL;
        }

        if (commands.state.print_stats.exchange(false, std::memory_order_relaxed) ||
            commands.state.print_status.exchange(false, std::memory_order_relaxed)) {
            print_mesh_stats(now);
        }
        if (next_stats != 0 && now >= next_stats) {
            if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
                print_mesh_stats(now);
            }
            if (csv_log) {
                csv_log->write_periodic(
                    (now - start_time) / 1000ULL,
                    aggregate_stats(),
                    options,
                    make_audio_snapshot(audio.engine.get()));
            }
            next_stats += static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL;
        }

        bool received_any = false;
        std::size_t mesh_datagrams_this_wake = 0;
        while (mesh_datagrams_this_wake < 64) {
            const auto received = network_session.receiveFor(received_any ? 0ULL : 1000ULL);
            if (!received) {
                break;
            }
            ++mesh_datagrams_this_wake;
            received_any = true;
            const auto& from = received->endpoint;
            const std::span<const std::uint8_t> bytes = received->bytes;
            auto peer_it = peers.find(endpoint_key(from));
            if (peer_it == peers.end()) {
                continue;
            }
            auto& peer = peer_it->second;
            const auto parsed = network_session.parse(bytes);
            if (!parsed) {
                peer.udp_parse.observe(parsed.error);
                ++peer.ignored_packets;
                continue;
            }
            try {
                const auto& header = parsed.header;
                if (header.type != jam2::protocol::PacketType::Ping &&
                    header.type != jam2::protocol::PacketType::Pong &&
                    (peer.endpoint_proof != EndpointProofState::Active ||
                     !network_session.acceptsEndpoint(from))) {
                    ++peer.proof_unverified_drops;
                    ++peer.ignored_packets;
                    continue;
                }
                if (header.type == jam2::protocol::PacketType::Audio) {
                    if (header.payload_length != audio_payload_size) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    const auto received_payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const auto receive_result = network_session.peerStream(peer.peer_id).receiveAudio(
                        header,
                        received_payload,
                        receive_time);
                    if (receive_result != jam2::PeerAudioResult::Accepted) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    ++peer.recv_packets;
                    peer.recv_bytes += bytes.size();
                } else if (header.type == jam2::protocol::PacketType::Ping) {
                    auto& peer_stream = network_session.peerStream(peer.peer_id);
                    if (!peer_stream.acceptReplay(jam2::PeerReplayChannel::Ping, header.sequence)) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    network_session.sendToPeer(
                        peer.peer_id,
                        jam2::protocol::PacketType::Pong,
                        header.sequence,
                        header.timing_value,
                        {},
                        true);
                    ++peer.sent_pongs;
                } else if (header.type == jam2::protocol::PacketType::Pong) {
                    ProbeChallenge& challenge =
                        peer.probe_challenges[header.sequence % peer.probe_challenges.size()];
                    if (!challenge.used ||
                        challenge.sequence != header.sequence ||
                        challenge.send_time_us != header.timing_value) {
                        ++peer.proof_unmatched_pongs;
                        ++peer.ignored_packets;
                        continue;
                    }
                    challenge.used = false;
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (receive_time >= challenge.send_time_us) {
                        network_session.peerStream(peer.peer_id).observeRtt(
                            receive_time - challenge.send_time_us);
                    }
                    if (peer.endpoint_proof != EndpointProofState::Active) {
                        peer.endpoint_proof = EndpointProofState::Active;
                        network_session.setPeerEndpointState(
                            peer.peer_id,
                            jam2::PeerEndpointState::Active);
                        ++peer.proof_successes;
                    }
                    ++peer.recv_pongs;
                } else if (header.type == jam2::protocol::PacketType::MetronomeState) {
                    auto& peer_stream = network_session.peerStream(peer.peer_id);
                    if (!peer_stream.acceptReplay(
                            jam2::PeerReplayChannel::Metronome,
                            header.sequence)) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const MetronomePayload metronome = decode_metronome_payload(payload);
                    const int remote_abs_bpm = std::abs(metronome.bpm);
                    if (remote_abs_bpm <= 0 || remote_abs_bpm > 400 ||
                        metronome.kind == GridMessageKind::LegacyState) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    auto store_grid_settings = [&] {
                        if (commands.state.grid_request_sequence.load(std::memory_order_acquire) !=
                            last_local_grid_request_sequence) {
                            return;
                        }
                        if (metronome.has_pattern) {
                            store_metronome_pattern(commands.state, metronome.pattern);
                        }
                        commands.state.bpm.store(remote_abs_bpm, std::memory_order_relaxed);
                        commands.state.metronome.store(
                            metronome.run_state == jam2::GridRunState::Running,
                            std::memory_order_relaxed);
                        commands.state.metronome_mode.store(metronome.mode, std::memory_order_relaxed);
                    };
                    if (metronome.kind == GridMessageKind::Proposal) {

                        const auto ordered = authority.orderGridProposal({
                            peer.peer_id.value,

                            metronome.revision_or_request,
                            metronome.run_state,
                            metronome.mode,
                            metronome.epoch_sample_time,
                        });
                        if (ordered) {
                            store_grid_settings();
                            commands.state.metronome_revision.store(
                                ordered->revision,
                                std::memory_order_relaxed);
                            commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                            mesh_grid_target_valid = false;
                            apply_authority_role();
                            next_grid_assignment_send_us = 0;

                        }
                    } else if (metronome.kind == GridMessageKind::Assignment) {
                        const jam2::GridAuthorityState assignment{
                            metronome.revision_or_request,
                            header.timing_value,
                            metronome.run_state,
                            metronome.mode,
                            metronome.epoch_sample_time,
                            0,
                        };
                        const auto result = authority.acceptGridAssignment(
                            peer.peer_id.value,
                            assignment);
                        if (result == jam2::AuthorityUpdateResult::Accepted) {
                            store_grid_settings();
                            commands.state.metronome_revision.store(
                                assignment.revision,
                                std::memory_order_relaxed);
                            pending_local_grid_proposal.reset();
                            mesh_grid_target_valid = false;
                            if (authority.localIsGridAuthority()) {
                                (void)activate_local_grid();
                            } else {
                                commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);

                                apply_authority_role();
                            }
                        }
                    } else if (metronome.kind == GridMessageKind::AuthorityState) {
                        const jam2::GridAuthorityState remote_state{
                            metronome.revision_or_request,
                            peer.peer_id.value,
                            metronome.run_state,
                            metronome.mode,
                            metronome.epoch_sample_time,
                            header.timing_value,
                        };
                        const auto result = authority.acceptGridAuthorityState(
                            peer.peer_id.value,
                            remote_state);
                        if (result == jam2::AuthorityUpdateResult::Accepted) {
                            store_grid_settings();
                            commands.state.metronome_revision.store(
                                remote_state.revision,
                                std::memory_order_relaxed);
                            remote_authority_epoch_frame = metronome.epoch_sample_time;
                            last_authority_state_received_us = jam2::monotonic_us();
                            if (!commands.state.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                                (void)align_to_authority_clock(
                                    metronome,
                                    header.timing_value,
                                    peer_stream);
                            }
                            if (metronome.mode == metronome_mode_id(MetronomeMode::SharedGrid) &&
                                commands.state.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                                const auto pattern = metronome_pattern_from_runtime(commands.state);
                                const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
                                    static_cast<double>(options.sample_rate), pattern.bpm, pattern.division);
                                const std::uint64_t bar_frames = step_frames *
                                    static_cast<std::uint64_t>(pattern.step_count);
                                if (bar_frames > 0) {
                                    const std::uint64_t projected = header.timing_value +
                                        peer_stream.stats().rtt_min_us *
                                            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
                                    const std::uint64_t local_frame = musical_frame_from_raw(
                                        current_engine_frame(audio.engine.get()),
                                        commands.state.metronome_render_offset_frames.load(
                                            std::memory_order_relaxed));
                                    const std::uint64_t local_epoch =
                                        commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
                                    auto phase = [&](std::uint64_t frame, std::uint64_t epoch) {
                                        return frame >= epoch
                                            ? static_cast<std::int64_t>((frame - epoch) % bar_frames)
                                            : -static_cast<std::int64_t>((epoch - frame) % bar_frames);
                                    };
                                    std::int64_t error = phase(projected, metronome.epoch_sample_time) -
                                        phase(local_frame, local_epoch);
                                    const std::int64_t interval = static_cast<std::int64_t>(bar_frames);
                                    if (error > interval / 2) error -= interval;
                                    if (error < -interval / 2) error += interval;
                                    mesh_grid_target_offset_frames =
                                        commands.state.metronome_render_offset_frames.load(
                                            std::memory_order_relaxed) + error;
                                    mesh_grid_target_valid = true;
                                }
                            }
                            apply_authority_role();
                        }
                    }
                } else if (header.type == jam2::protocol::PacketType::TransportState) {
                    auto& peer_stream = network_session.peerStream(peer.peer_id);
                    if (!peer_stream.acceptReplay(
                            jam2::PeerReplayChannel::Transport,
                            header.sequence)) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const TransportPayload transport = decode_transport_payload(payload);
                    const bool track_transport =
                        jam2::is_track_sync_transport_action(transport.action);
                    const bool accept_track_transport =
                        runtime_host.track_sync_enabled.load(std::memory_order_acquire);
                    const bool track_transport_ready =
                        !track_transport || !accept_track_transport ||
                        (peer.recv_pongs > 0 &&
                         peer_stream.stats().rtt_min_us > 0 &&
                         audio.engine != nullptr);
                    if (!track_transport_ready) {
                        // Transport packets are repeated until their target.
                        // Do not consume this source event counter before the
                        // rejoined edge has the clock mapping needed to apply
                        // it; a later repeat can then schedule the same action.
                        ++peer.ignored_packets;
                        continue;
                    }
                    const bool accepted_transport = authority.acceptTransportEvent(
                        peer.peer_id.value,
                        transport.event_counter,
                        transport.grid_revision,
                        !track_transport);
                    if (accepted_transport) {
                        mesh_transport_source_peer_id = peer.peer_id.value;
                        mesh_transport_event_counter = transport.event_counter;
                        mesh_transport_grid_revision = transport.grid_revision;
                        mesh_transport_action = transport.action;
                        mesh_transport_source_frame = header.timing_value;
                        mesh_transport_requested_target_frame = transport.target_sender_frame;
                    }
                    if (track_transport && accepted_transport && !accept_track_transport) {
                        ++peer.ignored_packets;
                    }
                    if (track_transport && accept_track_transport &&
                        accepted_transport &&
                        peer.recv_pongs > 0 &&
                        peer_stream.stats().rtt_min_us > 0 &&
                        audio.engine != nullptr) {
                        const std::uint64_t sender_lead_frames =
                            transport.target_sender_frame > header.timing_value
                            ? transport.target_sender_frame - header.timing_value
                            : 0ULL;
                        const std::uint64_t one_way_frames = peer_stream.stats().rtt_min_us *
                            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
                        const std::uint64_t frames_until_target = sender_lead_frames > one_way_frames
                            ? sender_lead_frames - one_way_frames
                            : 0ULL;
                        const std::uint64_t target_raw_frame =
                            current_engine_frame(audio.engine.get()) + frames_until_target;
                        mesh_transport_applied_target_frame = target_raw_frame;
                        const std::int64_t offset =
                            commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
                        const QuantizedSchedule schedule{
                            target_raw_frame,
                            target_raw_frame,
                            musical_frame_from_raw(target_raw_frame, offset),
                        };
                        bool scheduled = false;
                        if (transport.action == jam2::EngineTransportAction::TrackRestart ||
                            transport.action == jam2::EngineTransportAction::RecordStart) {
                            scheduled = jam2::cli::restart_prepared_source(audio.engine.get(), schedule.target_raw_frame);
                        } else {
                            jam2::EngineCommand command;
                            command.type = transport.action == jam2::EngineTransportAction::TrackPlay
                                ? jam2::EngineCommandType::PreparedPlay
                                : jam2::EngineCommandType::PreparedStop;
                            command.frame = schedule.target_raw_frame;
                            scheduled = audio.engine->submit(command);
                        }
                        if (scheduled) {
                            publish_transport_schedule(
                                commands.state,
                                transport.action,
                                schedule,
                                false);
                        }
                    }
                } else if (header.type == jam2::protocol::PacketType::Bye) {
                    (void)network_session.peerStream(peer.peer_id).acceptReplay(
                        jam2::PeerReplayChannel::Bye,
                        header.sequence);
                    const bool departed_grid_authority =
                        peer.peer_id.value == authority.grid().authority_peer_id;
                    const bool grid_was_running = departed_grid_authority &&
                        authority.grid().run_state == jam2::GridRunState::Running;
                    (void)authority.markPeerInactive(peer.peer_id.value);
                    if (departed_grid_authority && !timed_stream_audio_detached) {
                        clear_departed_authority_state();
                        restore_running_grid_after_departure(
                            grid_was_running,
                            peer.peer_id.value);
                    }
                    ++peer.ignored_packets;
                } else {
                    ++peer.ignored_packets;
                }
            } catch (const std::exception&) {
                ++peer.ignored_packets;
            }
        }
        mesh_receive_batch_max = std::max<std::uint64_t>(
            mesh_receive_batch_max,
            static_cast<std::uint64_t>(mesh_datagrams_this_wake));
        if (mesh_datagrams_this_wake == 64) {
            ++mesh_work_budget_yields;
        }
    }

    const std::uint64_t now = jam2::monotonic_us();
    // Stop callback consumption before flushing final reorder/mix state. A
    // trailing partial mix block is diagnostic state, not live playback, and
    // must not manufacture a shutdown-only playback underrun.
    detach_network_capture(audio);
    network_session.finish(now);
    network_session.sendToActive(
        jam2::protocol::PacketType::Bye,
        packet_schedule.takeControlSequence(),
        now,
        {});
    const auto final_audio_snapshot = make_audio_snapshot(audio.engine.get());
    print_mesh_stats(now, &final_audio_snapshot);
    auto final_stats = aggregate_stats();
    final_stats.elapsed_ms = (now - start_time) / 1000ULL;
    if (csv_log) {
        csv_log->write(
            "final",
            final_stats.elapsed_ms,
            final_stats,
            options,
            final_audio_snapshot);
    }
    if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
        print_audio_packet_stats(final_stats, options);
        print_optional_audio_stats(audio, options);
    }
    commands.recording.finalize(
        audio.engine.get(),
        make_recording_sidecar_context(options, commands.state));
    std::cout.flush();
    return 0;
}



} // namespace

int jam2::cli::runTestDevice(int argc, char** argv)
{
    return run_test_device(argc, argv);
}

int jam2::cli::runMeterDevice(int argc, char** argv)
{
    return run_meter_device(argc, argv);
}

int jam2::cli::runRingDevice(int argc, char** argv)
{
    return run_ring_device(argc, argv);
}

int jam2::cli::runLocal(int argc, char** argv)
{
    return run_local(argc, argv);
}


Jam2RuntimeOptions jam2_parse_runtime_options(int argc, char** argv, int start_index)
{
    return parse_options(argc, argv, start_index);
}

jam2::EngineConfig jam2_make_engine_config(
    const Jam2RuntimeOptions& options,
    bool leader_audio_local_click)
{
    return make_engine_config_impl(options, leader_audio_local_click);
}

bool jam2_engine_restart_required(
    const jam2::EngineConfig& active,
    const jam2::EngineConfig& requested) noexcept
{
    return engine_restart_required(active, requested);
}

int jam2_run_network_runtime(Jam2RuntimeOptions options, Jam2RuntimeHost& host)
{
    try {
        return run_network_session(std::move(options), host);
    } catch (const std::exception& exception) {
        std::cerr << "Network runtime failed: " << exception.what() << "\n";
        if (host.error) {
            host.error(exception.what());
        }
        return 1;
    } catch (...) {
        std::cerr << "Network runtime failed: unknown native network runtime failure\n";
        if (host.error) {
            host.error("unknown native network runtime failure");
        }
        return 1;
    }
}
