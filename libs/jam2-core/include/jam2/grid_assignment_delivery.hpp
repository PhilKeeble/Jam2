#pragma once

#include <cstdint>

namespace jam2 {

// Control-thread delivery of the latest grid snapshot to one authenticated peer.
// Receipt is emitted by the receiver only after application, never by the socket.
class GridAssignmentDelivery {
public:
    static constexpr std::uint64_t kInitialRetryUs = 100000;
    static constexpr std::uint64_t kMaximumRetryUs = 1000000;

    void reset() noexcept { *this = {}; }
    void begin(std::uint64_t revision, std::uint64_t token) noexcept;
    bool due(std::uint64_t now_us) const noexcept;
    void attempted(std::uint64_t now_us) noexcept;
    bool acknowledge(std::uint64_t revision, std::uint64_t token) noexcept;
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t token() const noexcept { return token_; }
    std::uint64_t attempts() const noexcept { return attempts_; }
    bool applied() const noexcept { return applied_; }
    bool pending() const noexcept { return revision_ != 0 && !applied_; }

private:
    std::uint64_t revision_ = 0, token_ = 0, attempts_ = 0, next_retry_us_ = 0;
    std::uint64_t retry_us_ = kInitialRetryUs;
    bool applied_ = false;
};

} // namespace jam2
