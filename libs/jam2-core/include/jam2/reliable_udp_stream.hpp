#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace jam2 {

// Bounded, ordered byte delivery for a single logical control/asset channel.
// Authentication, link/stream identity and UDP I/O belong to the outer transport.
// Audio never enters this class and never waits for these acknowledgements.
class ReliableUdpStream {
public:
    static constexpr std::size_t kFragmentBytes = 1024;
    static constexpr std::size_t kQueueBytes = 256 * 1024;
    static constexpr std::size_t kReceiveWindow = 64;
    static constexpr std::uint64_t kMaximumUnacknowledgedUs = 15000000;
    // Bootstrap above ordinary mobile RTT so unambiguous first ACKs can
    // establish an RTT estimate instead of every fragment being retransmitted.
    struct Stats {
        std::uint64_t sent = 0, retransmitted = 0, acknowledged = 0;
        std::uint64_t received = 0, duplicates = 0, reordered = 0, rejected = 0;
        std::uint64_t queue_rejected = 0, timeouts = 0;
        std::size_t queue_high_water_bytes = 0, reorder_high_water_bytes = 0;
        std::uint64_t smoothed_rtt_us = 0, retransmit_timeout_us = 1000000;
        double congestion_window = 4.0;
        std::uint64_t in_order_direct = 0, reorder_insertions = 0;
        std::uint64_t tail_bytes_packed = 0, fragments_avoided = 0;
        std::uint64_t fast_retransmitted = 0, sack_gap_reports = 0;
        std::uint64_t fast_reorder_us = 10000;
    };
    struct Options {
        bool direct_in_order = true, pack_unsent = true, fast_recovery = true;
        std::uint64_t fast_reorder_us = 10000;
    };
    struct AckWindow { std::uint64_t contiguous = 0, selective = 0; };
    ReliableUdpStream() = default;
    explicit ReliableUdpStream(Options options) : options_(options) { stats_.fast_reorder_us=options.fast_reorder_us; }
    using Transmit = std::function<bool(std::uint64_t, std::span<const std::uint8_t>, bool)>;
    using Deliver = std::function<void(std::span<const std::uint8_t>)>;
    using Acknowledge = std::function<void(std::uint64_t)>;

    bool enqueue(std::span<const std::uint8_t> bytes);
    // The caller applies a shared byte/packet budget across channels and peers.
    std::size_t poll(std::uint64_t now_us, std::size_t packet_budget, const Transmit& transmit);
    void acknowledge(std::uint64_t sequence, std::uint64_t now_us);
    // Reject a whole invalid window before acknowledging any fragment.
    bool acknowledgeWindow(AckWindow window, std::uint64_t now_us);
    bool validAckWindow(AckWindow window) const noexcept;
    AckWindow acknowledgementWindow() const noexcept;
    bool receive(std::uint64_t sequence, std::span<const std::uint8_t> bytes,
                 const Deliver& deliver, const Acknowledge& acknowledge);
    bool failed() const noexcept { return failed_; }
    bool hasSendCredit() const noexcept {
        return !failed_ && !queue_.empty() && pending_.size()<static_cast<std::size_t>(stats_.congestion_window) &&
            (pending_.empty() || next_send_-pending_.begin()->first<kReceiveWindow);
    }
    bool empty() const noexcept { return queued_bytes_ == 0; }
    std::size_t queuedBytes() const noexcept { return queued_bytes_; }
    std::size_t inFlightFragments() const noexcept { return pending_.size(); }
    std::size_t reorderedBytes() const noexcept { return reordered_bytes_; }
    const Stats& stats() const noexcept { return stats_; }

private:
    struct Fragment {
        std::vector<std::uint8_t> bytes;
        std::uint64_t first_sent_us = 0, last_sent_us = 0, retry_us = 1000000;
        unsigned transmissions = 0;
        unsigned later_acked = 0;
        std::uint64_t fast_due_us = 0;
        bool fast_retried = false;
    };
    std::deque<std::vector<std::uint8_t>> queue_;
    std::map<std::uint64_t, Fragment> pending_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> reordered_;
    std::uint64_t next_send_ = 1, next_receive_ = 1;
    std::size_t queued_bytes_ = 0, reordered_bytes_ = 0;
    std::uint64_t rtt_variation_us_ = 0;
    bool failed_ = false;
    Stats stats_;
    Options options_;
};

} // namespace jam2
