#include "reliable_udp_stream.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jam2 {

bool ReliableUdpStream::enqueue(std::span<const std::uint8_t> bytes)
{
    if (failed_ || bytes.size() > kQueueBytes - queued_bytes_) {
        ++stats_.queue_rejected;
        return false;
    }
    for (std::size_t offset = 0; offset < bytes.size(); offset += kFragmentBytes) {
        const auto part = bytes.subspan(offset, (std::min)(kFragmentBytes, bytes.size() - offset));
        queue_.emplace_back(part.begin(), part.end());
    }
    queued_bytes_ += bytes.size();
    stats_.queue_high_water_bytes = (std::max)(stats_.queue_high_water_bytes, queued_bytes_);
    return true;
}

std::size_t ReliableUdpStream::poll(
    std::uint64_t now_us, std::size_t packet_budget, const Transmit& transmit)
{
    if (failed_) return 0;
    std::size_t sent = 0;
    for (auto& [sequence, fragment] : pending_) {
        if (now_us >= fragment.first_sent_us &&
            now_us - fragment.first_sent_us >= kMaximumUnacknowledgedUs) {
            failed_ = true;
            ++stats_.timeouts;
            return sent;
        }
        if (sent >= packet_budget || now_us < fragment.last_sent_us ||
            now_us - fragment.last_sent_us < fragment.retry_us) continue;
        if (!transmit(sequence, fragment.bytes, true)) return sent;
        ++sent;
        ++stats_.retransmitted;
        ++stats_.sent;
        ++fragment.transmissions;
        fragment.last_sent_us = now_us;
        fragment.retry_us = (std::min)(fragment.retry_us * 2, std::uint64_t{2000000});
        stats_.congestion_window = (std::max)(2.0, stats_.congestion_window / 2.0);
    }
    const std::size_t window = static_cast<std::size_t>(stats_.congestion_window);
    while (sent < packet_budget && !queue_.empty() && pending_.size() < window &&
           (pending_.empty() || next_send_ - pending_.begin()->first < kReceiveWindow)) {
        if (next_send_ == (std::numeric_limits<std::uint64_t>::max)()) {
            failed_ = true;
            return sent;
        }
        if (!transmit(next_send_, queue_.front(), false)) break;
        Fragment fragment;
        fragment.bytes = std::move(queue_.front());
        queue_.pop_front();
        fragment.first_sent_us = fragment.last_sent_us = now_us;
        fragment.retry_us = stats_.retransmit_timeout_us;
        fragment.transmissions = 1;
        pending_.emplace(next_send_++, std::move(fragment));
        ++sent;
        ++stats_.sent;
    }
    return sent;
}

void ReliableUdpStream::acknowledge(std::uint64_t sequence, std::uint64_t now_us)
{
    const auto found = pending_.find(sequence);
    if (found == pending_.end()) return;
    const Fragment& fragment = found->second;
    // Karn's rule: a retransmitted fragment cannot yield an unambiguous RTT.
    if (fragment.transmissions == 1 && now_us >= fragment.first_sent_us) {
        const std::uint64_t sample = (std::max)(std::uint64_t{1}, now_us - fragment.first_sent_us);
        if (stats_.smoothed_rtt_us == 0) {
            stats_.smoothed_rtt_us = sample;
            rtt_variation_us_ = sample / 2;
        } else {
            const auto difference = stats_.smoothed_rtt_us > sample
                ? stats_.smoothed_rtt_us - sample : sample - stats_.smoothed_rtt_us;
            rtt_variation_us_ = (3 * rtt_variation_us_ + difference) / 4;
            stats_.smoothed_rtt_us = (7 * stats_.smoothed_rtt_us + sample) / 8;
        }
        stats_.retransmit_timeout_us = std::clamp(
            stats_.smoothed_rtt_us + 4 * rtt_variation_us_,
            std::uint64_t{100000}, std::uint64_t{2000000});
    }
    queued_bytes_ -= fragment.bytes.size();
    pending_.erase(found);
    ++stats_.acknowledged;
    stats_.congestion_window = (std::min)(
        32.0, stats_.congestion_window + 1.0 / stats_.congestion_window);
}

bool ReliableUdpStream::receive(
    std::uint64_t sequence, std::span<const std::uint8_t> bytes,
    const Deliver& deliver, const Acknowledge& acknowledge)
{
    if (failed_ || sequence == 0 || bytes.empty() || bytes.size() > kFragmentBytes) {
        ++stats_.rejected;
        return false;
    }
    if (sequence < next_receive_) {
        ++stats_.duplicates;
        acknowledge(sequence); // A lost ACK must not force duplicate application delivery.
        return true;
    }
    if (sequence - next_receive_ >= kReceiveWindow) {
        ++stats_.rejected;
        return false;
    }
    auto [found, inserted] = reordered_.try_emplace(sequence, bytes.begin(), bytes.end());
    if (!inserted) {
        // Conflicting authenticated bytes at the same sequence are a protocol error.
        if (found->second.size() != bytes.size() ||
            !std::equal(bytes.begin(), bytes.end(), found->second.begin())) {
            ++stats_.rejected;
            return false;
        }
        ++stats_.duplicates;
    } else {
        reordered_bytes_ += bytes.size();
        ++stats_.received;
        if (sequence != next_receive_) ++stats_.reordered;
        stats_.reorder_high_water_bytes = (std::max)(stats_.reorder_high_water_bytes, reordered_bytes_);
    }
    acknowledge(sequence);
    while ((found = reordered_.find(next_receive_)) != reordered_.end()) {
        deliver(found->second);
        reordered_bytes_ -= found->second.size();
        reordered_.erase(found);
        ++next_receive_;
    }
    return true;
}

} // namespace jam2
