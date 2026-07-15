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

std::optional<std::vector<Jam2RuntimePeer>> Jam2RuntimeHost::takePeerUpdate()
{
    std::lock_guard<std::mutex> lock(peer_mutex_);
    auto result = std::move(peer_update_);
    peer_update_.reset();
    return result;
}

std::optional<jam2::EngineCommand> Jam2RuntimeHost::takeCommand()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (commands_.empty()) {
        return std::nullopt;
    }
    jam2::EngineCommand result = commands_.front();
    commands_.pop_front();
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
}
