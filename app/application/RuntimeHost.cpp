#include "RuntimeContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

std::int64_t milliseconds_to_frames(double milliseconds, double sampleRate) noexcept
{
    return sampleRate > 0.0 && std::isfinite(sampleRate)
        ? static_cast<std::int64_t>(std::llround(milliseconds * sampleRate / 1000.0))
        : 0;
}

} // namespace

bool jam2_valid_metronome_compensation(
    const Jam2MetronomeCompensationSettings& settings) noexcept
{
    return std::isfinite(settings.maximum_ms) &&
        settings.maximum_ms >= 0.0 && settings.maximum_ms <= 1000.0 &&
        std::isfinite(settings.smoothing_ms) &&
        settings.smoothing_ms >= 0.0 && settings.smoothing_ms <= 10000.0 &&
        std::isfinite(settings.deadband_ms) &&
        settings.deadband_ms >= 0.0 && settings.deadband_ms <= 1000.0 &&
        std::isfinite(settings.slew_ms_per_second) &&
        settings.slew_ms_per_second >= 0.0 &&
        settings.slew_ms_per_second <= 10000.0;
}

std::int64_t jam2_next_metronome_compensation_offset(
    std::int64_t currentOffsetFrames,
    std::int64_t baseOffsetFrames,
    std::int64_t requestedTargetFrames,
    double elapsedMs,
    double sampleRate,
    const Jam2MetronomeCompensationSettings& settings) noexcept
{
    if (!jam2_valid_metronome_compensation(settings) ||
        !std::isfinite(elapsedMs) || elapsedMs <= 0.0 ||
        !std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return currentOffsetFrames;
    }
    const std::int64_t maximumFrames = std::abs(
        milliseconds_to_frames(settings.maximum_ms, sampleRate));
    const std::int64_t boundedTarget = baseOffsetFrames + std::clamp(
        requestedTargetFrames - baseOffsetFrames,
        -maximumFrames,
        maximumFrames);
    const std::int64_t remaining = boundedTarget - currentOffsetFrames;
    const std::int64_t deadbandFrames = std::abs(
        milliseconds_to_frames(settings.deadband_ms, sampleRate));
    if (std::abs(remaining) <= deadbandFrames) {
        return currentOffsetFrames;
    }
    const double alpha = settings.smoothing_ms > 0.0
        ? std::clamp(elapsedMs / settings.smoothing_ms, 0.0, 1.0)
        : 1.0;
    std::int64_t step = static_cast<std::int64_t>(
        std::llround(static_cast<double>(remaining) * alpha));
    const std::int64_t maximumStep = std::abs(milliseconds_to_frames(
        settings.slew_ms_per_second * elapsedMs / 1000.0,
        sampleRate));
    // A zero slew setting retains the established meaning of no slew limit.
    if (maximumStep > 0) {
        step = std::clamp(step, -maximumStep, maximumStep);
    }
    return currentOffsetFrames + step;
}

bool Jam2RuntimeHost::submitCommand(const jam2::EngineCommand& command) noexcept

{
    if (engine == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (commands_.size() >= kCommandCapacity) {
            return false;
        }
        commands_.push_back(command);
    }
    if (!engine->submit(command)) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (!commands_.empty()) {
            commands_.pop_back();
        }
        return false;
    }
    return true;
}

bool Jam2RuntimeHost::submitPeerUpdate(const std::vector<Jam2RuntimePeer>& peers)
{
    if (std::any_of(peers.begin(), peers.end(), [](const Jam2RuntimePeer& peer) {
            return peer.peer_id == 0 || peer.endpoint.host.empty() ||
                peer.endpoint.host.size() > 255 || peer.endpoint.port == 0;
        })) {
        return false;
    }
    std::lock_guard<std::mutex> lock(peer_mutex_);
    peer_update_ = peers;
    return true;
}

void Jam2RuntimeHost::submitPeerReprobe() noexcept
{
    peer_reprobe_requested_.store(true, std::memory_order_release);
}

bool Jam2RuntimeHost::submitPeerGain(std::uint64_t peer_id, int gain_ppm) noexcept
{
    if (peer_id == 0 || gain_ppm < 0 || gain_ppm > 4000000) {
        return false;
    }
    std::lock_guard<std::mutex> lock(peer_gain_mutex_);
    peer_gains_[peer_id] = gain_ppm;
    return true;
}

bool Jam2RuntimeHost::submitMetronomeCompensation(
    const Jam2MetronomeCompensationSettings& settings) noexcept
{
    if (!jam2_valid_metronome_compensation(settings)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(metronome_compensation_mutex_);
    metronome_compensation_ = settings;
    return true;
}

std::optional<std::vector<Jam2RuntimePeer>> Jam2RuntimeHost::takePeerUpdate()
{
    std::lock_guard<std::mutex> lock(peer_mutex_);
    auto result = std::move(peer_update_);
    peer_update_.reset();
    return result;
}

bool Jam2RuntimeHost::takePeerReprobe() noexcept
{
    return peer_reprobe_requested_.exchange(false, std::memory_order_acq_rel);
}

std::vector<Jam2PeerGainUpdate> Jam2RuntimeHost::takePeerGains()
{
    std::lock_guard<std::mutex> lock(peer_gain_mutex_);
    std::vector<Jam2PeerGainUpdate> result;
    result.reserve(peer_gains_.size());
    for (const auto& [peer_id, gain_ppm] : peer_gains_) {
        result.push_back({peer_id, gain_ppm});
    }
    peer_gains_.clear();
    return result;
}

std::optional<Jam2MetronomeCompensationSettings>
Jam2RuntimeHost::takeMetronomeCompensation()
{
    std::lock_guard<std::mutex> lock(metronome_compensation_mutex_);
    auto result = metronome_compensation_;
    metronome_compensation_.reset();
    return result;
}

std::optional<jam2::EngineCommand> Jam2RuntimeHost::takeCommand(
    std::uint64_t current_frame)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    const auto ready = std::find_if(
        commands_.begin(),
        commands_.end(),
        [current_frame](const jam2::EngineCommand& command) {
            return command.apply_frame == 0 || command.apply_frame <= current_frame;
        });
    if (ready == commands_.end()) {
        return std::nullopt;
    }
    jam2::EngineCommand result = *ready;
    commands_.erase(ready);
    return result;
}

std::uint64_t Jam2RuntimeHost::nextGridRequestId() noexcept
{
    std::uint64_t current = grid_request_id_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t next = current == (std::numeric_limits<std::uint64_t>::max)()
            ? 1ULL
            : current + 1ULL;
        if (grid_request_id_.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return next;
        }
    }
}

std::uint64_t Jam2RuntimeHost::nextTransportEventId() noexcept
{
    std::uint64_t current = transport_event_id_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t next = current == (std::numeric_limits<std::uint64_t>::max)()
            ? 1ULL
            : current + 1ULL;
        if (transport_event_id_.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return next;
        }
    }
}

void Jam2RuntimeHost::reset() noexcept
{
    stop_requested.store(false, std::memory_order_release);
    peer_reprobe_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_update_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        commands_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(peer_gain_mutex_);
        peer_gains_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(metronome_compensation_mutex_);
        metronome_compensation_.reset();
    }
}
