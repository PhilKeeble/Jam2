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
    std::size_t offset = 0, added = 0;
    // Only queue_ owns unsequenced bytes. Pending/retry fragments are immutable.
    if (options_.pack_unsent && !queue_.empty() && queue_.back().size() < kFragmentBytes) {
        auto& tail = queue_.back();
        offset = (std::min)(bytes.size(), kFragmentBytes - tail.size());
        tail.insert(tail.end(), bytes.begin(), bytes.begin() + offset);
        stats_.tail_bytes_packed += offset;
    }
    for (; offset < bytes.size(); offset += kFragmentBytes) {
        const auto part = bytes.subspan(offset, (std::min)(kFragmentBytes, bytes.size() - offset));
        std::vector<std::uint8_t> fragment;
        fragment.reserve(options_.pack_unsent ? kFragmentBytes : part.size());
        fragment.insert(fragment.end(), part.begin(), part.end());
        queue_.push_back(std::move(fragment)); ++added;
    }
    stats_.fragments_avoided += (bytes.size() + kFragmentBytes - 1) / kFragmentBytes - added;
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
        const bool fast=options_.fast_recovery && fragment.transmissions==1 && !fragment.fast_retried && fragment.fast_due_us && now_us>=fragment.fast_due_us;
        if (sent >= packet_budget || now_us < fragment.last_sent_us ||
            (!fast && now_us - fragment.last_sent_us < fragment.retry_us)) continue;
        if (!transmit(sequence, fragment.bytes, true)) return sent;
        ++sent;
        ++stats_.retransmitted;
        ++stats_.sent;
        ++fragment.transmissions;
        fragment.last_sent_us = now_us;
        if (fast) { fragment.fast_retried=true; ++stats_.fast_retransmitted; fragment.retry_us=stats_.retransmit_timeout_us; }
        else fragment.retry_us = (std::min)(fragment.retry_us * 2, std::uint64_t{2000000});
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
        // Keep a scheduler margin even when measured RTT variation reaches
        // zero; an ACK due on the polling tick must not imply packet loss.
        stats_.retransmit_timeout_us = std::clamp(
            stats_.smoothed_rtt_us + (std::max)(std::uint64_t{10000}, 4 * rtt_variation_us_),
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
    if (options_.direct_in_order && sequence == next_receive_) {
        ++stats_.received;
        ++stats_.in_order_direct;
        // Delivery borrows the caller's bytes synchronously. Only gaps need
        // owned reorder storage; downstream asynchronous readers still copy.
        deliver(bytes);
        ++next_receive_;
        for (auto found = reordered_.find(next_receive_); found != reordered_.end(); found = reordered_.find(next_receive_)) {
            deliver(found->second);
            reordered_bytes_ -= found->second.size();
            reordered_.erase(found);
            ++next_receive_;
        }
        acknowledge(sequence);
        return true;
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
        ++stats_.reorder_insertions;
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

ReliableUdpStream::AckWindow ReliableUdpStream::acknowledgementWindow() const noexcept
{
    AckWindow window{next_receive_ - 1, 0};
    for (const auto& [sequence, bytes] : reordered_) {
        (void)bytes;
        const auto offset = sequence - next_receive_;
        if (offset < kReceiveWindow) window.selective |= std::uint64_t{1} << offset;
    }
    return window;
}

bool ReliableUdpStream::validAckWindow(AckWindow window) const noexcept
{
    if (window.contiguous >= next_send_) return false;
    for (unsigned bit = 0; bit < kReceiveWindow; ++bit) {
        if (!(window.selective & (std::uint64_t{1} << bit))) continue;
        if (window.contiguous > (std::numeric_limits<std::uint64_t>::max)() - bit - 1 ||
            window.contiguous + bit + 1 >= next_send_) return false;
    }
    return true;
}
bool ReliableUdpStream::acknowledgeWindow(AckWindow window, std::uint64_t now_us)
{
    if (!validAckWindow(window)) return false;
    const auto contains=[&](std::uint64_t sequence) {
        return sequence<=window.contiguous || (sequence-window.contiguous-1<kReceiveWindow &&
            (window.selective & (std::uint64_t{1}<<(sequence-window.contiguous-1))));
    };
    unsigned later=0;
    if (options_.fast_recovery) for (auto it=pending_.rbegin();it!=pending_.rend();++it) {
        const auto sequence=it->first; auto& fragment=it->second;
        if (contains(sequence)) { ++later; continue; }
        if (fragment.fast_retried || fragment.transmissions!=1) continue;
        // Only newly acknowledged pending fragments count. Duplicate ACKs do
        // not create loss evidence, and ordinary reordering gets a grace period.
        if (later) { ++stats_.sack_gap_reports; fragment.later_acked+=later; }
        if (fragment.later_acked>=3 && !fragment.fast_due_us && now_us<=(std::numeric_limits<std::uint64_t>::max)()-options_.fast_reorder_us)
            fragment.fast_due_us=now_us+options_.fast_reorder_us;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
        const auto sequence = (it++)->first;
        if (contains(sequence))
            acknowledge(sequence, now_us);
    }
    return true;
}

} // namespace jam2
