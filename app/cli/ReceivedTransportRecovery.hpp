#pragma once

#include "engine.hpp"
#include <cstdint>
#include <optional>

namespace jam2::cli {
struct TransportIntent {
    EngineTransportAction action = EngineTransportAction::None;
    std::uint32_t event_counter = 0, grid_revision = 0;
    std::uint64_t target_sender_frame = 0, countdown_start_sender_frame = 0;
};

// One owned intent per receiver, independent of the sender's retry lifetime.
// The authenticated authority gate must accept an event before retaining it.
class ReceivedTransportRecovery {
public:
    struct Pending {
        TransportIntent transport;
        std::uint64_t peer_id = 0, sender_frame = 0, receiver_frame = 0;
    };
    static constexpr std::uint64_t retry_us = 20000;
    void retain(Pending value) noexcept { pending_ = value; next_retry_us_ = 0; }
    void clear() noexcept { pending_.reset(); next_retry_us_ = 0; }
    const Pending* pending() const noexcept { return pending_ ? &*pending_ : nullptr; }
    const Pending* takeDue(std::uint64_t now) noexcept {
        if (!pending_ || now < next_retry_us_) return nullptr;
        next_retry_us_ = now + retry_us;
        return &*pending_;
    }
private:
    std::optional<Pending> pending_;
    std::uint64_t next_retry_us_ = 0;
};
} // namespace jam2::cli
