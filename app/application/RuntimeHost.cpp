#include "RuntimeContracts.hpp"

#include <algorithm>
#include <limits>
#include <utility>

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

bool Jam2RuntimeHost::submitPeerGain(std::uint64_t peer_id, int gain_ppm) noexcept
{
    if (peer_id == 0 || gain_ppm < 0 || gain_ppm > 4000000) {
        return false;
    }
    std::lock_guard<std::mutex> lock(peer_gain_mutex_);
    peer_gains_[peer_id] = gain_ppm;
    return true;
}

std::optional<std::vector<Jam2RuntimePeer>> Jam2RuntimeHost::takePeerUpdate()
{
    std::lock_guard<std::mutex> lock(peer_mutex_);
    auto result = std::move(peer_update_);
    peer_update_.reset();
    return result;
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
}
