#include "peer_mixer.hpp"
#include "runtime_limits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jam2 {
namespace {

constexpr double kMinimumRatio = 0.995;
constexpr double kMaximumRatio = 1.005;
constexpr std::uint64_t kMaximumBoundaryLookaheadFrames = 2;

std::int32_t saturate_i32(std::int64_t value) noexcept
{
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        value,
        (std::numeric_limits<std::int32_t>::min)(),
        (std::numeric_limits<std::int32_t>::max)()));
}

} // namespace

struct PeerMixer::Impl {
    struct PeerSlot final : PeerStreamPlayback {
        Impl* owner = nullptr;
        PeerMixerPeerStats stats;
        std::vector<std::int32_t> queue;
        std::vector<std::int32_t> resample_scratch;
        std::size_t read_index = 0;
        std::size_t write_index = 0;
        std::size_t queued = 0;
        std::size_t late_frames_to_discard = 0;
        bool timeline_recovery_pending = false;
        bool has_previous_sample = false;
        std::int32_t previous_sample = 0;
        double next_output_phase = 0.0;
        PeerDeliveryGap delivery_gap;
        // Bounded recent gap history. Keep original timestamps; smaller later
        // gaps cannot renew the age of a larger observation.
        std::array<PeerDeliveryGap, 8> pending_delivery_gaps{};
        std::uint64_t observed_delivery_gap_us = 0;

        PeerSlot(Impl* mixer, std::uint64_t id, std::size_t capacity)
            : owner(mixer),
              queue(capacity, 0),
              resample_scratch(std::max<std::size_t>(
                  capacity,
                  static_cast<std::size_t>(mixer->config.frames_per_block) * 2U + 2U), 0)
        {
            stats.peer_id = id;
            stats.queue_capacity_frames = capacity;
        }

        void resetTimeline() noexcept
        {
            read_index = 0;
            write_index = 0;
            queued = 0;
            late_frames_to_discard = 0;
            timeline_recovery_pending = false;
            has_previous_sample = false;
            previous_sample = 0;
            next_output_phase = 0.0;
            delivery_gap = {};
            pending_delivery_gaps = {};
            observed_delivery_gap_us = 0;
            owner->surplus_stable_since_us = 0;
            stats.contributing = false;
            stats.queue_depth_frames = 0;
            stats.recent_peak_ppm = 0;
            owner->occupancy_dirty = true;
        }

        void resetSourceTimeline() noexcept override { resetTimeline(); }

        std::size_t enqueueFrames(std::span<const std::int32_t> frames) noexcept
        {
            if (frames.empty() ||
                (owner->output != nullptr && !owner->output->acceptsFrames())) {
                return 0;
            }

            const std::size_t late = std::min(late_frames_to_discard, frames.size());
            late_frames_to_discard -= late;
            stats.late_after_release_frames += late;
            owner->stats.late_after_release_frames += late;
            frames = frames.subspan(late);
            if (frames.empty()) {
                return 0;
            }

            const std::size_t accepted = frames.size();
            const std::size_t capacity = queue.size();
            const std::size_t dropped = queued + frames.size() > capacity
                ? queued + frames.size() - capacity
                : 0;
            if (dropped > 0) {
                // Keep the stream current after a burst. Retaining a full queue
                // of stale audio while rejecting every new sample can leave the
                // peer permanently behind real time.
                stats.queue_capacity_drops += dropped;
                stats.queue_capacity_dropped_frames += dropped;
                owner->stats.capacity_drops += dropped;
                owner->stats.capacity_dropped_frames += dropped;
            }

            if (frames.size() >= capacity) {
                const std::size_t final_write = (write_index + frames.size()) % capacity;
                frames = frames.last(capacity);
                read_index = final_write;
                write_index = final_write;
                queued = capacity;
            } else {
                const std::size_t queued_to_drop = std::min(dropped, queued);
                read_index = (read_index + queued_to_drop) % capacity;
                queued -= queued_to_drop;
            }

            const std::size_t first = std::min(frames.size(), capacity - write_index);
            std::copy_n(frames.data(), first, queue.data() + write_index);
            const std::size_t second = frames.size() - first;
            if (second > 0) {
                std::copy_n(frames.data() + first, second, queue.data());
            }
            write_index = (write_index + frames.size()) % capacity;
            if (frames.size() < capacity) {
                queued += frames.size();
            } else {
                read_index = write_index;
            }
            stats.contributing = true;
            stats.queue_depth_frames = queued;
            stats.queue_high_water_frames = std::max<std::uint64_t>(
                stats.queue_high_water_frames,
                queued);
            owner->occupancy_dirty = true;
            return accepted;
        }

        void noteMissingFrames(std::size_t frames) noexcept
        {
            const std::size_t available = (std::numeric_limits<std::size_t>::max)() -
                late_frames_to_discard;
            late_frames_to_discard += std::min(frames, available);
            if (owner->receiveRecoveryDebtFrames() <
                owner->config.receive_recovery_trigger_frames) {
                return;
            }
            timeline_recovery_pending = true;
            owner->armReceiveRecovery();
        }

        std::size_t retainLiveTail(std::size_t retain_frames) noexcept
        {
            const std::size_t dropped = queued > retain_frames ? queued - retain_frames : 0;
            read_index = (read_index + dropped) % queue.size();
            queued -= dropped;
            stats.late_after_release_frames += dropped;
            owner->stats.late_after_release_frames += dropped;
            if (dropped > 0) {
                ++stats.live_tail_trim_events;
                stats.live_tail_trimmed_frames += dropped;
                stats.live_tail_trim_max_frames = std::max<std::uint64_t>(
                    stats.live_tail_trim_max_frames,
                    dropped);
                ++owner->stats.live_tail_trim_events;
                owner->stats.live_tail_trimmed_frames += dropped;
                owner->stats.live_tail_trim_max_frames = std::max<std::uint64_t>(
                    owner->stats.live_tail_trim_max_frames,
                    dropped);
            }
            stats.queue_depth_frames = queued;
            if (dropped > 0) {
                owner->occupancy_dirty = true;
            }
            return dropped;
        }

        void completeTimelineRecovery() noexcept
        {
            timeline_recovery_pending = false;
        }

        std::span<const std::int32_t> firstReadableSpan(std::size_t frames) const noexcept
        {
            const std::size_t count = std::min({frames, queued, queue.size() - read_index});
            return std::span<const std::int32_t>(queue.data() + read_index, count);
        }

        std::span<const std::int32_t> secondReadableSpan(std::size_t frames) const noexcept
        {
            const std::size_t first = firstReadableSpan(frames).size();
            const std::size_t count = std::min(frames, queued) - first;
            return std::span<const std::int32_t>(queue.data(), count);
        }

        void consumeFrames(std::size_t frames) noexcept
        {
            const std::size_t consumed = std::min(frames, queued);
            read_index = (read_index + consumed) % queue.size();
            queued -= consumed;
            stats.queue_depth_frames = queued;
            if (consumed > 0) {
                owner->occupancy_dirty = true;
            }
        }

        std::size_t depthFrames() const noexcept override
        {
            return queued;
        }

        std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
        {
            if (!stats.active || frames.empty()) {
                return 0;
            }
            const double ratio = std::clamp(stats.resampler_ratio, kMinimumRatio, kMaximumRatio);
            const bool unity_aligned = ratio == 1.0 &&
                (!has_previous_sample || next_output_phase == 1.0);
            if (unity_aligned) {
                const std::size_t pushed = enqueueFrames(frames);
                has_previous_sample = true;
                previous_sample = frames.back();
                next_output_phase = 1.0;
                stats.resampled_output_frames += frames.size();
                stats.unity_resampler_fast_frames += frames.size();
                return pushed;
            }
            std::size_t pushed = 0;
            std::size_t rendered = 0;
            auto flushRendered = [&]() noexcept {
                pushed += enqueueFrames(std::span<const std::int32_t>(
                    resample_scratch.data(), rendered));
                rendered = 0;
            };
            auto appendRendered = [&](std::int32_t sample) noexcept {
                resample_scratch[rendered++] = sample;
                ++stats.resampled_output_frames;
                if (rendered == resample_scratch.size()) {
                    flushRendered();
                }
            };
            for (const std::int32_t sample : frames) {
                if (!has_previous_sample) {
                    has_previous_sample = true;
                    previous_sample = sample;
                    next_output_phase = ratio;
                    appendRendered(sample);
                    continue;
                }
                while (next_output_phase <= 1.0) {
                    const double mixed = static_cast<double>(previous_sample) +
                        (static_cast<double>(sample) - static_cast<double>(previous_sample)) *
                            next_output_phase;
                    const auto output_sample = static_cast<std::int32_t>(std::clamp(
                        mixed,
                        static_cast<double>((std::numeric_limits<std::int32_t>::min)()),
                        static_cast<double>((std::numeric_limits<std::int32_t>::max)())));
                    appendRendered(output_sample);
                    next_output_phase += ratio;
                }
                next_output_phase -= 1.0;
                previous_sample = sample;
            }
            flushRendered();
            return pushed;
        }

        void releaseBoundaryLookahead() noexcept
        {
            if (!has_previous_sample || queued >= owner->remainingSlotFrames() ||
                next_output_phase <= 0.0 || next_output_phase >= 1.0) return;
            const double ratio = std::clamp(stats.resampler_ratio, kMinimumRatio, kMaximumRatio);
            std::uint64_t emitted = 0;
            // Approximate only the missing neighbour, never an unavailable
            // source interval. Advancing phase prevents replay next packet.
            while (next_output_phase < 1.0 && emitted < kMaximumBoundaryLookaheadFrames) {
                if (enqueueFrames(std::span<const std::int32_t>(&previous_sample, 1)) != 1) break;
                owner->stats.lookahead_hold_max_fraction_ppm = std::max(
                    owner->stats.lookahead_hold_max_fraction_ppm,
                    static_cast<std::uint64_t>(std::ceil(next_output_phase * 1000000.0)));
                ++stats.resampled_output_frames;
                next_output_phase += ratio;
                ++emitted;
            }
            owner->stats.lookahead_hold_frames += emitted;
            owner->stats.lookahead_hold_events += emitted != 0;
        }

        void requestDropFrames(std::size_t frames) noexcept override
        {
            const std::size_t dropped = std::min(frames, queued);
            read_index = (read_index + dropped) % queue.size();
            queued -= dropped;
            stats.requested_drop_frames += dropped;
            stats.queue_depth_frames = queued;
            if (dropped > 0) {
                owner->occupancy_dirty = true;
            }
        }

        void setResamplerRatio(double ratio) noexcept override
        {
            stats.resampler_ratio = std::isfinite(ratio)
                ? std::clamp(ratio, kMinimumRatio, kMaximumRatio)
                : 1.0;
        }

        void observeDeliveryGap(const PeerDeliveryGap& gap) noexcept override
        {
            if (gap.receive_time_us >= delivery_gap.receive_time_us) {
                delivery_gap = gap;
                auto* destination = &pending_delivery_gaps.front();
                for (auto& pending : pending_delivery_gaps) {
                    if (pending.receive_time_us < destination->receive_time_us) destination = &pending;
                }
                // With a custom, shorter observation threshold, discard the
                // oldest entry on saturation rather than refreshing its age.
                *destination = gap;
                if (owner->config.adaptive_playback_cushion) {
                    // Capture the peak before a later observation in this
                    // receive batch changes the latest-gap snapshot.
                    owner->stats.adaptive_delivery_gap_required_max_frames = std::max<std::uint64_t>(
                        owner->stats.adaptive_delivery_gap_required_max_frames, gap.required_playback_frames);
                }
            }
        }
    };

    const PeerMixerConfig config;
    PeerStreamPlayback* output = nullptr;
    PeerMixerStats stats;
    std::vector<std::unique_ptr<PeerSlot>> peers;
    std::vector<std::int64_t> accumulator;
    std::vector<std::int32_t> mixed_output;
    std::vector<std::int32_t> silence_output;
    bool started = false;
    std::size_t pending_recovery_padding_goal = 0;
    std::size_t slot_frames_released = 0;
    bool slot_had_missing = false;
    std::uint64_t next_deadline_us = 0;
    std::uint64_t interval_us = 0;
    std::uint64_t interval_remainder_us = 0;
    std::uint64_t interval_denominator = 1;
    std::uint64_t interval_remainder_accumulator = 0;
    std::size_t deadline_blocks = 2;
    std::uint64_t adaptive_target_frames = 0;
    std::uint64_t adaptive_last_update_us = 0;
    double adaptive_release_accumulator_frames = 0.0;
    bool adaptive_release_active = false;
    std::uint64_t surplus_stable_since_us = 0;
    std::uint64_t adaptive_gap_hold_until_us = 0;
    std::uint64_t adaptive_gap_target_frames = 0;
    std::uint64_t adaptive_last_output_underrun_us = 0;
    std::uint64_t consecutive_deadline_slots = 0;
    std::uint64_t observed_output_underrun_frames = 0;
    bool cached_all_ready = false;
    bool cached_any_ready = false;
    bool occupancy_dirty = true;
    bool receive_catchup_active = false;
    bool receive_rebase_started = false;
    std::uint64_t current_advance_us = 0;
    std::uint64_t receive_recovery_started_us = 0;
    std::uint64_t receive_recovery_stable_until_us = 0;
    std::uint64_t receive_recovery_window_us = 0;
    std::size_t receive_recovery_peak_debt_frames = 0;
    std::uint64_t last_small_recovery_observation_us = 0;
    std::uint64_t last_service_us = 0;
    std::uint64_t service_bucket = 0;
    std::uint64_t service_peak_us = 0;
    std::uint64_t service_previous_peak_us = 0;
    std::uint64_t path_bucket = 0, path_peak_frames = 0, path_previous_peak_frames = 0;
    std::uint64_t delivery_gap_evidence_until_us = 0;
    std::uint64_t observed_extra_consumed_frames = 0;
    std::uint64_t recovery_clean_since_us = 0;
    std::uint64_t recovery_depth_bucket = 0;
    std::uint64_t recovery_depth_minimum = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t recovery_previous_minimum = (std::numeric_limits<std::uint64_t>::max)();
    bool deliveryGapGuardActive() const noexcept
    { return current_advance_us<adaptive_gap_hold_until_us || current_advance_us<delivery_gap_evidence_until_us; }

    void observePathHighWater(std::uint64_t now_us) noexcept
    {
        if (!config.adaptive_playback_cushion || output==nullptr) return;
        const auto window=config.adaptive_surplus_stable_us ? config.adaptive_surplus_stable_us : 1000000ULL;
        const auto bucket=now_us/window;
        if (bucket!=path_bucket) {
            path_previous_peak_frames=bucket==path_bucket+1 ? path_peak_frames : 0;
            path_peak_frames=0;path_bucket=bucket;
        }
        std::uint64_t source=0;
        for (const auto& peer:peers) if (peer->stats.active && peer->stats.contributing)
            source=std::max<std::uint64_t>(source,peer->queued);
        path_peak_frames=std::max<std::uint64_t>(path_peak_frames,source+output->depthFrames());
        stats.adaptive_path_high_water_frames=std::max(path_peak_frames,path_previous_peak_frames);
    }

    void observeService(std::uint64_t now_us) noexcept
    {
        if (last_service_us != 0 && now_us > last_service_us) {
            const auto window = std::max<std::uint64_t>(
                1, std::max(config.adaptive_surplus_stable_us, interval_us));
            const auto bucket = now_us / window;
            if (bucket != service_bucket) {
                service_previous_peak_us = bucket == service_bucket + 1 ? service_peak_us : 0;
                service_peak_us = 0;
                service_bucket = bucket;
            }
            service_peak_us = std::max(service_peak_us, now_us - last_service_us);
        }
        if (now_us > last_service_us) last_service_us = now_us;
        stats.worker_service_gap_us = std::max(service_peak_us, service_previous_peak_us);
        const auto rate = static_cast<std::uint64_t>(config.sample_rate);
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        const auto gap = stats.worker_service_gap_us;
        const auto frames = gap > (maximum - 999999) / rate ? maximum
            : (gap * rate + 999999) / 1000000;
        const auto callback = output != nullptr ? output->callbackFrames() : 0;
        stats.worker_runway_frames = callback == 0 || frames > maximum - callback
            ? frames : (frames + callback - 1) / callback * callback;
    }

    Impl(const PeerMixerConfig& requested, PeerStreamPlayback* sink)
        : config(requested),
          output(sink),
          accumulator(static_cast<std::size_t>(requested.frames_per_block), 0),
          mixed_output(static_cast<std::size_t>(requested.frames_per_block), 0),
          silence_output(static_cast<std::size_t>(requested.frames_per_block), 0),
          interval_denominator(static_cast<std::uint64_t>(requested.sample_rate)),
          deadline_blocks(std::max<std::size_t>(
              2U,
              (requested.deadline_frames + static_cast<std::size_t>(requested.frames_per_block) - 1U) /
                  static_cast<std::size_t>(requested.frames_per_block))),
          adaptive_target_frames(requested.adaptive_target_frames)
    {
        if (!limits::valid_sample_rate(config.sample_rate) || config.frames_per_block <= 0 ||
            config.max_blocks_per_advance == 0) {
            throw std::runtime_error("invalid PeerMixer configuration");
        }
        if (config.adaptive_playback_cushion &&
            (config.adaptive_min_frames > config.adaptive_target_frames ||
             config.adaptive_target_frames > config.adaptive_max_frames)) {
            throw std::runtime_error("PeerMixer adaptive playback bounds are inconsistent");
        }
        if (config.adaptive_release_ppm < 0 || config.adaptive_release_ppm > 1000000) {
            throw std::runtime_error("PeerMixer adaptive playback release is outside 0..1000000 ppm");
        }
        if (config.receive_recovery_trigger_frames == 0) {
            throw std::runtime_error("PeerMixer receive recovery trigger must be nonzero");
        }
        if (config.live_tail_rounding_tolerance_us > 10000) {
            throw std::runtime_error("PeerMixer source rounding tolerance exceeds 10 ms");
        }
        const std::uint64_t numerator =
            static_cast<std::uint64_t>(config.frames_per_block) * 1000000ULL;
        interval_us = numerator / interval_denominator;
        interval_remainder_us = numerator % interval_denominator;
        if (output != nullptr) {
            output->setResamplerRatio(1.0);
            observed_extra_consumed_frames = output->playbackRecoveryState().extra_consumed_frames;
        }
        stats.adaptive_playback_cushion_enabled = config.adaptive_playback_cushion;
        stats.adaptive_target_frames = adaptive_target_frames;
        stats.receive_recovery_trigger_frames = config.receive_recovery_trigger_frames;
        // Cover ordinary packet dispatch or two device callback quanta,
        // whichever is larger. A 256-frame device callback is not four
        // independently scheduled 64-frame callbacks. This permits real
        // queued audio only, never proactive silence, and output_max still wins.
        const std::size_t batch_frames = std::max(
            static_cast<std::size_t>(config.frames_per_block) * 4U,
            output != nullptr ? output->callbackFrames() * 2U : 0U);
        stats.live_tail_batch_tolerance_frames = config.adaptive_playback_cushion &&
            config.adaptive_min_frames < batch_frames
            ? batch_frames - config.adaptive_min_frames : 0;
        stats.live_tail_rounding_tolerance_frames = roundingToleranceFrames();
        updateLimitStats();
    }

    PeerSlot* find(std::uint64_t peer_id) noexcept
    {
        for (auto& peer : peers) {
            if (peer->stats.peer_id == peer_id) {
                return peer.get();
            }
        }
        return nullptr;
    }

    const PeerSlot* find(std::uint64_t peer_id) const noexcept
    {
        for (const auto& peer : peers) {
            if (peer->stats.peer_id == peer_id) {
                return peer.get();
            }
        }
        return nullptr;
    }

    std::uint64_t deadlineStep() noexcept
    {
        std::uint64_t step = interval_us == 0 ? 1 : interval_us;
        interval_remainder_accumulator += interval_remainder_us;
        if (interval_remainder_accumulator >= interval_denominator) {
            ++step;
            interval_remainder_accumulator -= interval_denominator;
        }
        return step;
    }

    std::uint64_t deadlineDelay() noexcept
    {
        std::uint64_t delay = 0;
        for (std::size_t block = 0; block < deadline_blocks; ++block) {
            delay += deadlineStep();
        }
        return std::max<std::uint64_t>(1, delay);
    }

    std::size_t receiveRecoveryDebtFrames() const noexcept
    {
        std::size_t debt = 0;
        for (const auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing) {
                debt = std::max(debt, peer->late_frames_to_discard);
            }
        }
        return debt;
    }

    std::size_t liveTargetFrames() const noexcept
    {
        const std::size_t requested = config.adaptive_playback_cushion
            ? static_cast<std::size_t>(adaptive_target_frames)
            : config.fixed_target_frames;
        if (requested == 0) {
            return 0;
        }
        const std::size_t target = std::max<std::size_t>(
            static_cast<std::size_t>(config.frames_per_block),
            requested);
        return config.output_max_frames > 0
            ? std::min(config.output_max_frames, target)
            : target;
    }

    std::size_t receiveRecoveryTailFrames() const noexcept
    {
        const std::size_t minimum_tail =
            static_cast<std::size_t>(config.frames_per_block) * 2U;
        if (config.adaptive_playback_cushion || output == nullptr) {
            return minimum_tail;
        }
        const std::size_t fixed_target = liveTargetFrames();
        const std::size_t output_depth = output->depthFrames();
        const std::size_t target_fill = output_depth < fixed_target
            ? fixed_target - output_depth
            : 0;
        return std::max(minimum_tail, target_fill);
    }

    std::size_t receiveRecoveryOutputTailFrames() const noexcept
    {
        const std::size_t fixed_target = !config.adaptive_playback_cushion
            ? liveTargetFrames()
            : 0;
        return std::max<std::size_t>(
            static_cast<std::size_t>(config.frames_per_block) * 2U,
            std::max(config.adaptive_min_frames, fixed_target));
    }

    static constexpr std::uint64_t kAdaptiveGapAssociationUs = 250000ULL;
    static constexpr std::uint64_t kAdaptiveGapHoldUs = 30000000ULL;

    void extendAdaptiveGapHold(std::uint64_t receive_time_us) noexcept
    {
        const std::uint64_t requested = receive_time_us >
                (std::numeric_limits<std::uint64_t>::max)() - kAdaptiveGapHoldUs
            ? (std::numeric_limits<std::uint64_t>::max)()
            : receive_time_us + kAdaptiveGapHoldUs;
        if (requested > adaptive_gap_hold_until_us) {
            adaptive_gap_hold_until_us = requested;
            ++stats.adaptive_delivery_gap_hold_events;
        }
        stats.adaptive_delivery_gap_hold_active = true;
        stats.adaptive_delivery_gap_hold_remaining_us =
            adaptive_gap_hold_until_us > current_advance_us
            ? adaptive_gap_hold_until_us - current_advance_us
            : 0;
    }

    void raiseAdaptiveTargetForGap(
        std::size_t required_frames,
        std::uint64_t receive_time_us) noexcept
    {
        if (!config.adaptive_playback_cushion) {
            return;
        }
        const std::uint64_t requested = std::min<std::uint64_t>(
            config.adaptive_max_frames,
            std::max<std::uint64_t>(config.adaptive_min_frames, required_frames));
        adaptive_gap_target_frames = current_advance_us < adaptive_gap_hold_until_us
            ? std::max(adaptive_gap_target_frames, requested) : requested;
        if (requested > adaptive_target_frames) {
            adaptive_target_frames = requested;
            ++stats.adaptive_raise_events;
            ++stats.adaptive_delivery_gap_raise_events;
            adaptive_release_accumulator_frames = 0.0;
            adaptive_release_active = false;
            stats.adaptive_target_frames = adaptive_target_frames;
        }
        extendAdaptiveGapHold(receive_time_us);
        updateLimitStats();
    }

    void observeAdaptiveDeliveryGaps(std::uint64_t now_us) noexcept
    {
        if (!config.adaptive_playback_cushion) {
            return;
        }
        // Keep evidence owned by its source: a newer small gap on another
        // peer must neither replace nor refresh an older larger requirement.
        for (const auto& peer : peers) {
            const auto& gap = peer->delivery_gap;
            if (!peer->stats.active || gap.receive_time_us == 0 ||
                gap.receive_time_us <= peer->observed_delivery_gap_us) {
                continue;
            }
            peer->observed_delivery_gap_us = gap.receive_time_us;
            if (now_us>=gap.receive_time_us && now_us-gap.receive_time_us<=kAdaptiveGapAssociationUs)
                delivery_gap_evidence_until_us=gap.receive_time_us>(std::numeric_limits<std::uint64_t>::max)()-kAdaptiveGapAssociationUs
                    ? (std::numeric_limits<std::uint64_t>::max)() : gap.receive_time_us+kAdaptiveGapAssociationUs;
            stats.adaptive_delivery_gap_required_frames = gap.required_playback_frames;
            stats.adaptive_delivery_gap_required_max_frames = std::max<std::uint64_t>(
                stats.adaptive_delivery_gap_required_max_frames, gap.required_playback_frames);
            const std::uint64_t target_with_tolerance =
                gap.required_playback_frames >
                        (std::numeric_limits<std::uint64_t>::max)() -
                            static_cast<std::uint64_t>(config.frames_per_block)
                    ? (std::numeric_limits<std::uint64_t>::max)()
                    : static_cast<std::uint64_t>(gap.required_playback_frames) +
                        static_cast<std::uint64_t>(config.frames_per_block);
            if (now_us >= gap.receive_time_us &&
                now_us - gap.receive_time_us <= kAdaptiveGapAssociationUs &&
                adaptive_target_frames > config.adaptive_min_frames &&
                target_with_tolerance >= adaptive_target_frames) {
                extendAdaptiveGapHold(gap.receive_time_us);
            }
        }
        if (now_us >= adaptive_last_output_underrun_us &&
            now_us - adaptive_last_output_underrun_us > kAdaptiveGapAssociationUs) {
            adaptive_last_output_underrun_us = 0;
        }
        if (adaptive_last_output_underrun_us != 0) {
            applyAssociatedAdaptiveGaps(now_us, adaptive_last_output_underrun_us);
        }
    }

    void applyAssociatedAdaptiveGaps(std::uint64_t now_us, std::uint64_t not_before_us) noexcept
    {
        PeerDeliveryGap largest;
        for (const auto& peer : peers) {
            for (auto& gap : peer->pending_delivery_gaps) {
                if (!peer->stats.active || (now_us >= gap.receive_time_us &&
                        now_us - gap.receive_time_us > kAdaptiveGapAssociationUs)) {
                    gap = {};
                }
                if (gap.receive_time_us == 0 || gap.receive_time_us > now_us ||
                    gap.receive_time_us < not_before_us ||
                    (not_before_us != 0 && gap.receive_time_us - not_before_us > kAdaptiveGapAssociationUs)) {
                    continue;
                }
                if (gap.required_playback_frames > largest.required_playback_frames ||
                    (gap.required_playback_frames == largest.required_playback_frames &&
                        gap.receive_time_us > largest.receive_time_us)) {
                    largest = gap;
                }
                gap = {}; // each source observation is associated only once
            }
        }
        if (largest.receive_time_us != 0) {
            raiseAdaptiveTargetForGap(largest.required_playback_frames, largest.receive_time_us);
            // Another peer may finish the same disruption on a later worker
            // wake. Keep the underrun's original association window; consumed
            // per-peer evidence cannot retrigger or extend it.
        }
    }

    void applyPendingAdaptiveGapAfterUnderrun(std::uint64_t now_us) noexcept
    {
        adaptive_last_output_underrun_us = now_us;
        applyAssociatedAdaptiveGaps(now_us, 0);
    }

    std::uint64_t receiveRecoveryWindowUs() const noexcept
    {
        const std::uint64_t debt_us = static_cast<std::uint64_t>(
            (static_cast<long double>(receive_recovery_peak_debt_frames) * 1000000.0L) /
            static_cast<long double>(config.sample_rate));
        const std::uint64_t minimum_us = static_cast<std::uint64_t>(
            (static_cast<long double>(config.frames_per_block) * 4.0L * 1000000.0L) /
            static_cast<long double>(config.sample_rate));
        return std::max<std::uint64_t>(1, std::max(debt_us, minimum_us));
    }

    void armReceiveRecovery() noexcept
    {
        if (!receive_catchup_active) {
            receive_catchup_active = true;
            receive_recovery_started_us = current_advance_us;
            receive_recovery_peak_debt_frames = 0;
            receive_rebase_started = false;
            receive_recovery_stable_until_us = 0;
            ++stats.receive_recovery_events;
        }
        // Additional real debt keeps this recovery active until it is repaid,
        // but it does not erase an already-established stability window. In
        // particular, scheduler-batched packets must not turn one recovery
        // event into an indefinitely moving deadline.
        receive_recovery_peak_debt_frames = std::max(
            receive_recovery_peak_debt_frames,
            receiveRecoveryDebtFrames());
        stats.receive_recovery_active = true;
        stats.receive_recovery_debt_frames = receiveRecoveryDebtFrames();
        stats.receive_recovery_debt_max_frames = std::max<std::uint64_t>(
            stats.receive_recovery_debt_max_frames,
            receive_recovery_peak_debt_frames);
    }

    std::size_t trimReceiveRecoverySources() noexcept
    {
        std::size_t dropped = 0;
        const std::size_t recovery_tail = receiveRecoveryTailFrames();
        for (auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing) {
                dropped += peer->retainLiveTail(recovery_tail);
                peer->completeTimelineRecovery();
            }
        }
        return dropped;
    }

    void processReceiveRecovery(std::uint64_t now_us) noexcept
    {
        if (!receive_catchup_active) {
            return;
        }
        const std::size_t debt = receiveRecoveryDebtFrames();
        stats.receive_recovery_active = true;
        stats.receive_recovery_debt_frames = debt;
        stats.receive_recovery_duration_us = now_us >= receive_recovery_started_us
            ? now_us - receive_recovery_started_us
            : 0;
        if (debt > 0) {
            receive_recovery_peak_debt_frames = std::max(
                receive_recovery_peak_debt_frames,
                debt);
            stats.receive_recovery_debt_max_frames = std::max<std::uint64_t>(
                stats.receive_recovery_debt_max_frames,
                receive_recovery_peak_debt_frames);
            return;
        }

        (void)trimReceiveRecoverySources();
        if (!receive_rebase_started) {
            receive_recovery_window_us = receiveRecoveryWindowUs();
            receive_recovery_stable_until_us = now_us + receive_recovery_window_us;
            if (output != nullptr) {
                const std::size_t output_tail = receiveRecoveryOutputTailFrames();
                const std::size_t output_depth = output->depthFrames();
                if (output_depth > output_tail) {
                    const std::size_t requested = output_depth - output_tail;
                    output->requestDropFrames(requested);
                    stats.output_drop_requested_frames += requested;
                    ++stats.output_drop_request_events;
                }
                output->setResamplerRatio(1.0);
            }
            // Discard obsolete audio, not still-valid evidence about delivery
            // gaps. Recovery's small source/output tails remain unchanged.
            const bool learned_hold_active = now_us < adaptive_gap_hold_until_us;
            adaptive_target_frames = learned_hold_active
                ? std::max<std::uint64_t>(config.adaptive_min_frames, adaptive_gap_target_frames)
                : config.adaptive_min_frames;
            adaptive_release_accumulator_frames = 0.0;
            adaptive_release_active = false;
            if (!learned_hold_active) {
                adaptive_gap_hold_until_us = 0;
                adaptive_gap_target_frames = 0;
            }
            adaptive_last_output_underrun_us = 0;
            surplus_stable_since_us = 0;
            for (const auto& peer : peers) peer->pending_delivery_gaps = {};
            stats.adaptive_delivery_gap_hold_active = learned_hold_active;
            stats.adaptive_delivery_gap_hold_remaining_us = learned_hold_active
                ? adaptive_gap_hold_until_us - now_us : 0;
            stats.adaptive_target_frames = adaptive_target_frames;
            receive_rebase_started = true;
            updateLimitStats();
            occupancy_dirty = true;
        }
    }

    bool receiveRecoveryPathBounded() const noexcept
    {
        const std::size_t peer_tail = receiveRecoveryTailFrames();
        for (const auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing && peer->queued > peer_tail) {
                return false;
            }
        }
        if (output == nullptr || config.output_max_frames == 0) {
            return true;
        }
        return output->depthFrames() <= receiveRecoveryOutputTailFrames() +
            static_cast<std::size_t>(config.frames_per_block);
    }

    void completeReceiveRecovery(std::uint64_t now_us) noexcept
    {
        if (output != nullptr && config.output_max_frames > 0) {
            const std::size_t output_tail = receiveRecoveryOutputTailFrames();
            const std::size_t output_depth = output->depthFrames();
            if (output_depth > output_tail) {
                const std::size_t requested = output_depth - output_tail;
                output->requestDropFrames(requested);
                stats.output_drop_requested_frames += requested;
                ++stats.output_drop_request_events;
            }
        }
        const std::uint64_t duration = now_us >= receive_recovery_started_us
            ? now_us - receive_recovery_started_us
            : 0;
        stats.receive_recovery_duration_us = duration;
        stats.receive_recovery_duration_max_us = std::max(
            stats.receive_recovery_duration_max_us,
            duration);
        ++stats.receive_recovery_completions;
        stats.receive_recovery_active = false;
        stats.receive_recovery_debt_frames = 0;
        receive_catchup_active = false;
        receive_rebase_started = false;
        pending_recovery_padding_goal = 0;
        slot_frames_released = 0;
        slot_had_missing = false;
        receive_recovery_stable_until_us = 0;
        receive_recovery_window_us = 0;
        receive_recovery_peak_debt_frames = 0;
        occupancy_dirty = true;
    }

    struct ContributorReadiness {
        bool all_ready = false;
        bool any_ready = false;
    };

    ContributorReadiness updateOccupancy() noexcept
    {
        std::uint64_t active = 0;
        std::uint64_t contributing = 0;
        std::uint64_t active_slots = 0;
        std::uint64_t max_slots = 0;
        bool all_ready = true;
        bool any_ready = false;
        const std::size_t frames_per_block = static_cast<std::size_t>(config.frames_per_block);
        const std::size_t required_frames = remainingSlotFrames();
        for (auto& peer : peers) {
            if (peer->stats.active) {
                ++active;
            }
            if (peer->stats.active && peer->stats.contributing) {
                ++contributing;
                const bool ready = peer->queued >= required_frames;
                all_ready = all_ready && ready;
                any_ready = any_ready || ready;
                active_slots = std::max<std::uint64_t>(
                    active_slots,
                    (peer->queued + frames_per_block - 1U) / frames_per_block);
            }
            max_slots += (peer->queue.size() + frames_per_block - 1U) / frames_per_block;
            peer->stats.queue_depth_frames = peer->queued;
        }
        if (contributing == 0) {
            slot_frames_released = 0;
            slot_had_missing = false;
            pending_recovery_padding_goal = 0;
        }
        stats.active_peers = active;
        stats.contributing_peers = contributing;
        stats.active_slots = active_slots;
        stats.max_slots = max_slots;
        stats.active_slots_high_water = std::max(stats.active_slots_high_water, active_slots);
        const ContributorReadiness readiness{
            contributing != 0 && all_ready,
            any_ready};
        cached_all_ready = readiness.all_ready;
        cached_any_ready = readiness.any_ready;
        occupancy_dirty = false;
        return readiness;
    }

    std::size_t pushOutput(std::span<const std::int32_t> frames) noexcept
    {
        if (output == nullptr) {
            stats.output_frames += frames.size();
            return frames.size();
        }
        if (config.output_max_frames > 0) {
            const std::size_t depth = output->depthFrames();
            if (depth + frames.size() > config.output_max_frames) {
                const std::size_t requested = depth + frames.size() - config.output_max_frames;
                output->requestDropFrames(requested);
                stats.output_drop_requested_frames += requested;
                ++stats.output_drop_request_events;
            }
        }
        const std::size_t pushed = output->pushFrames(frames);
        stats.output_frames += pushed;
        stats.output_dropped_frames += frames.size() - pushed;
        return pushed;
    }

    void cancelRecoveryBudget() noexcept
    {
        stats.recovery_cancelled_frames += stats.recovery_pending_frames;
        stats.recovery_pending_frames = 0;
        stats.recovery_release_active = false;
        recovery_clean_since_us = 0;
        recovery_depth_minimum = recovery_previous_minimum = (std::numeric_limits<std::uint64_t>::max)();
    }

    void creditRecoveryFrames(std::uint64_t frames) noexcept
    {
        const auto room = config.adaptive_max_frames > stats.recovery_pending_frames
            ? config.adaptive_max_frames - stats.recovery_pending_frames : 0;
        stats.recovery_pending_frames += std::min<std::uint64_t>(frames, room);
    }

    enum class CushionPurpose { InitialPrefill, UnderrunRecovery };

    void padOutputTo(std::uint64_t cushion_goal, CushionPurpose purpose) noexcept
    {
        if (!config.adaptive_playback_cushion || output == nullptr) return;
        std::size_t work = 0;
        while (output->depthFrames() < cushion_goal &&
               work < config.max_blocks_per_advance) {
            const std::size_t missing = static_cast<std::size_t>(
                cushion_goal - output->depthFrames());
            const std::size_t count = std::min(missing, silence_output.size());
            const std::size_t pushed = pushOutput(
                std::span<const std::int32_t>(silence_output.data(), count));
            stats.adaptive_padding_frames += pushed;
            if (purpose == CushionPurpose::UnderrunRecovery) creditRecoveryFrames(pushed);
            if (pushed < count) {
                break;
            }
            ++work;
        }
        if (work == config.max_blocks_per_advance && output->depthFrames() < cushion_goal) {
            ++stats.work_budget_yields;
        }
    }

    void ensureAdaptiveCushion() noexcept
    {
        padOutputTo(adaptive_target_frames > mixed_output.size()
            ? adaptive_target_frames - mixed_output.size() : 0, CushionPurpose::InitialPrefill);
    }

    void updateAdaptiveReleaseState(
        bool missing,
        bool allow_start) noexcept
    {
        if (!config.adaptive_playback_cushion || output == nullptr) {
            if (missing) {
                adaptive_release_active = false;
            }
            return;
        }
        const int effective_release_ppm = config.adaptive_release_ppm;
        const std::uint64_t output_depth = output->depthFrames();
        const auto playback_state = output->playbackRecoveryState();
        if (playback_state.extra_consumed_frames < observed_extra_consumed_frames) cancelRecoveryBudget();
        const auto consumed = playback_state.extra_consumed_frames >= observed_extra_consumed_frames
            ? playback_state.extra_consumed_frames - observed_extra_consumed_frames : 0;
        observed_extra_consumed_frames = playback_state.extra_consumed_frames;
        const auto retired = std::min(consumed, stats.recovery_pending_frames);
        stats.recovery_pending_frames -= retired;
        stats.recovery_retired_frames += retired;
        std::uint64_t shortest_source = (std::numeric_limits<std::uint64_t>::max)();
        bool independently_resampled = false;
        for (const auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing) {
                shortest_source = std::min<std::uint64_t>(shortest_source, peer->queued);
                independently_resampled |= peer->stats.resampler_ratio != 1.0;
            }
        }
        const auto block = static_cast<std::uint64_t>(config.frames_per_block);
        const auto callback_frames = std::max<std::uint64_t>(block, output->callbackFrames());
        const auto base_runway = std::max<std::uint64_t>(2 * callback_frames, config.adaptive_min_frames);
        const bool frame_release_safe = stats.worker_runway_frames < base_runway;
        stats.release_worker_guard_active = !frame_release_safe;
        const std::uint64_t mixable_source = shortest_source == (std::numeric_limits<std::uint64_t>::max)()
            ? 0 : (frame_release_safe && config.adaptive_min_frames <= block &&
                output->callbackFrames() != 0 && !receive_catchup_active && receiveRecoveryDebtFrames() == 0
                    ? shortest_source : shortest_source / block * block);
        const std::uint64_t actual_depth = output_depth + mixable_source;
        const std::uint64_t minimum_output = std::max<std::uint64_t>(2 * callback_frames,
            std::max<std::uint64_t>(
            config.adaptive_min_frames, config.frames_per_block) +
            stats.live_tail_batch_tolerance_frames);
        // Worker stalls retain the original independent-source block margin.
        // Clean service uses the actual stopping cost, without treating cached
        // interpolation history as additional queued latency.
        stats.playback_stopping_frames = playback_state.stopping_frames;
        const auto alignment_margin = frame_release_safe ? stats.playback_stopping_frames
            : (stats.contributing_peers > 1 && independently_resampled ? block : 0);
        stats.adaptive_release_alignment_margin_frames = alignment_margin;
        const bool recovery_cushion_active =
            adaptive_target_frames > config.adaptive_min_frames;
        // At minimum target, scheduling headroom is a ceiling, not a latency
        // floor. Preserve the original margin for small packets; large packets
        // need not retain two whole packet blocks. Worker runway plus a callback
        // remains protected throughout a complete stable low-water interval.
        // If callback rounding would require more than the original margin,
        // retain the original controller rather than adding a latency floor.
        const bool callback_release_safe = frame_release_safe && output->callbackFrames() != 0 &&
            stats.worker_runway_frames <= 2 * callback_frames - output->callbackFrames();
        const auto release_quantum = callback_release_safe
            ? std::min<std::uint64_t>(callback_frames, 2 * output->callbackFrames()) : callback_frames;
        const auto service_floor = callback_release_safe
            ? stats.worker_runway_frames + output->callbackFrames() : 2 * release_quantum;
        const auto surplus_floor = std::max<std::uint64_t>(
            std::max<std::uint64_t>(2 * release_quantum, service_floor), config.adaptive_min_frames) + alignment_margin;
        const bool surplus_safe = !missing && !receive_catchup_active && actual_depth > surplus_floor;
        if (!surplus_safe) surplus_stable_since_us = 0;
        else if (surplus_stable_since_us == 0) surplus_stable_since_us = current_advance_us;
        const bool stable_surplus = config.adaptive_surplus_stable_us != 0 && surplus_stable_since_us != 0 &&
            current_advance_us >= surplus_stable_since_us &&
            current_advance_us - surplus_stable_since_us >= config.adaptive_surplus_stable_us;
        const bool gap_guard=deliveryGapGuardActive();
        stats.delivery_gap_guard_active=gap_guard;
        if (missing || gap_guard || receive_catchup_active || receiveRecoveryDebtFrames() != 0)
            recovery_clean_since_us = current_advance_us;
        const bool clean_recovery = recovery_clean_since_us != 0 && config.adaptive_surplus_stable_us != 0 &&
            current_advance_us >= recovery_clean_since_us &&
            current_advance_us - recovery_clean_since_us >= config.adaptive_surplus_stable_us;
        // Recovery credit survives temporary valleys. Measure low water across
        // complete recent callback/packet cycles instead of discarding credit
        // at an instantaneous valley just before another shortage. Keep
        // one callback beyond measured worker runway, two interpolation frames,
        // and the actual return-ramp stopping cost in the output ring itself.
        stats.recovery_safe_depth_frames = std::max<std::uint64_t>(config.adaptive_min_frames,
            std::max<std::uint64_t>(block, stats.worker_runway_frames + output->callbackFrames()))
            + 2 + playback_state.stopping_frames;
        const auto depth_window = std::max<std::uint64_t>(1, config.adaptive_surplus_stable_us);
        const auto depth_bucket = current_advance_us / depth_window;
        if (depth_bucket != recovery_depth_bucket) {
            recovery_previous_minimum = depth_bucket == recovery_depth_bucket + 1
                ? recovery_depth_minimum : (std::numeric_limits<std::uint64_t>::max)();
            recovery_depth_minimum = (std::numeric_limits<std::uint64_t>::max)();
            recovery_depth_bucket = depth_bucket;
        }
        recovery_depth_minimum = std::min(recovery_depth_minimum, output_depth);
        stats.recovery_low_water_frames = std::min(recovery_depth_minimum, recovery_previous_minimum);
        const bool recovery_release = !recovery_cushion_active && playback_state.consumption_accounting &&
            clean_recovery && !gap_guard && !missing && !receive_catchup_active && receiveRecoveryDebtFrames() == 0 &&
            frame_release_safe && output->callbackFrames() != 0 && effective_release_ppm > 0 &&
            stats.recovery_pending_frames > playback_state.stopping_frames + 2 &&
            stats.recovery_low_water_frames > stats.recovery_safe_depth_frames &&
            output_depth > stats.recovery_safe_depth_frames;
        if (recovery_release) {
            stats.recovery_release_events += !stats.recovery_release_active;
            stats.recovery_release_active = true;
            adaptive_release_active = false;
            output->setResamplerRatio(1.0 + static_cast<double>(effective_release_ppm) / 1000000.0);
            return;
        }
        stats.recovery_release_active = false;
        const bool release_ready = stable_surplus || (recovery_cushion_active &&
            (!gap_guard || config.adaptive_surplus_stable_us == 0));
        const std::uint64_t release_stop_depth = recovery_cushion_active ? (minimum_output >
                static_cast<std::uint64_t>(config.frames_per_block)
            ? minimum_output - static_cast<std::uint64_t>(config.frames_per_block) : 0) +
            alignment_margin : surplus_floor;
        const std::uint64_t release_start_depth = recovery_cushion_active
            ? minimum_output + alignment_margin : surplus_floor;
        const bool actual_depth_above_start =
            actual_depth > release_start_depth;
        const bool actual_depth_at_stop =
            actual_depth <= release_stop_depth;

        const bool protected_runway = callback_release_safe ? (mixable_source >= block || output_depth > surplus_floor) :
            (mixable_source >= block || output_depth > block * 2);
        const bool callback_worker_safe = !frame_release_safe ||
            (output_depth >= stats.worker_runway_frames &&
                output_depth - stats.worker_runway_frames >= alignment_margin);
        // A held target protects arrival runway while allowing genuine surplus
        // from clock-estimation error to retire after a safe low-water interval.
        if (missing || !protected_runway || !callback_worker_safe || !release_ready || receive_catchup_active) {
            adaptive_release_active = false;
        } else {
            // Callback consumption changes real surplus independently of
            // packet arrival. A low-water observation cancels minimum-target
            // release immediately and requires a fresh stable interval.
            if (adaptive_release_active && actual_depth_at_stop) {
                adaptive_release_active = false;
            }
            if (!adaptive_release_active && allow_start &&
                effective_release_ppm > 0 && actual_depth_above_start) {
                adaptive_release_active = true;
            }
        }
        const bool releasing = !missing && protected_runway &&
            effective_release_ppm > 0 &&
            adaptive_release_active;
        output->setResamplerRatio(
            releasing ? 1.0 + static_cast<double>(effective_release_ppm) / 1000000.0 : 1.0);
    }

    void updateAdaptiveTarget(
        std::uint64_t now_us,
        bool missing,
        std::uint64_t recovery_step_frames = 0) noexcept
    {
        if (!config.adaptive_playback_cushion) {
            return;
        }
        if (missing) {
            adaptive_release_accumulator_frames = 0.0;
            adaptive_release_active = false;
            if (adaptive_target_frames < config.adaptive_max_frames) {
                adaptive_target_frames = std::min<std::uint64_t>(
                    config.adaptive_max_frames,
                    std::max<std::uint64_t>(
                        config.adaptive_min_frames,
                        adaptive_target_frames + (recovery_step_frames != 0 ? recovery_step_frames
                            : static_cast<std::uint64_t>(config.frames_per_block))));
                ++stats.adaptive_raise_events;
            }
        } else if (!missing && now_us >= adaptive_gap_hold_until_us &&
                   adaptive_target_frames > config.adaptive_min_frames &&
                   config.adaptive_release_ppm > 0 && adaptive_last_update_us != 0 &&
                   now_us > adaptive_last_update_us) {
            // Release ppm describes a temporary playback-rate offset. Accumulate
            // fractions across the frequent network-thread updates instead of
            // truncating every sub-frame update to zero.
            const int effective_release_ppm = config.adaptive_release_ppm;
            adaptive_release_accumulator_frames +=
                static_cast<double>(config.sample_rate) *
                static_cast<double>(effective_release_ppm) *
                static_cast<double>(now_us - adaptive_last_update_us) /
                1000000000000.0;
            const std::uint64_t available_release =
                static_cast<std::uint64_t>(adaptive_release_accumulator_frames);
            // A target is permission to retain audio, not proof that audio was
            // retired. Never lower it through the real buffered path: smooth
            // playback release must first create room under the smaller bound.
            std::uint64_t largest_source = 0;
            for (const auto& peer : peers) {
                if (peer->stats.active && peer->stats.contributing) {
                    largest_source = std::max<std::uint64_t>(largest_source, peer->queued);
                }
            }
            // An instantaneous valley between arriving batches is not spare
            // capacity. Retire unused target allowance against the complete
            // recent path peak; extra audio is still retired by safe release.
            const std::uint64_t actual_path=largest_source+(output != nullptr ? output->depthFrames() : 0);
            const std::uint64_t path = deliveryGapGuardActive()
                ? std::max<std::uint64_t>(stats.adaptive_path_high_water_frames,actual_path) : actual_path;
            const std::uint64_t allowance = stats.live_tail_batch_tolerance_frames;
            const std::uint64_t minimum_target = std::max<std::uint64_t>(
                config.adaptive_min_frames, path > allowance ? path - allowance : 0);
            const std::uint64_t room = adaptive_target_frames > minimum_target
                ? adaptive_target_frames - minimum_target : 0;
            const std::uint64_t release = std::min(available_release, room);
            if (release > 0) {
                adaptive_target_frames -= release;
                adaptive_release_accumulator_frames -= static_cast<double>(release);
                ++stats.adaptive_release_events;
            }
            // Do not bank a blocked release and apply a large target jump on
            // the next callback. Keep only the sub-frame fractional remainder.
            adaptive_release_accumulator_frames -= std::floor(adaptive_release_accumulator_frames);
        }
        updateAdaptiveReleaseState(missing, true);
        adaptive_last_update_us = now_us;
        stats.adaptive_target_frames = adaptive_target_frames;
        updateLimitStats();
    }

    void updateLimitStats() noexcept
    {
        stats.normal_output_limit_frames = normalOutputLimitFrames();
        stats.normal_path_limit_frames = stats.normal_output_limit_frames > 0
            ? stats.normal_output_limit_frames + sourceTailQuantumFrames() + roundingToleranceFrames() : 0;
    }

    std::size_t sourceTailQuantumFrames() const noexcept
    {
        const auto block = static_cast<std::size_t>(config.frames_per_block);
        if (!config.adaptive_playback_cushion) return block;
        const auto callback = output != nullptr ? output->callbackFrames() : 0;
        // One released block plus the remaining callback batch share this
        // path; do not count a complete callback again on both sides.
        return std::max(block, callback > block ? callback - block : 0);
    }

    std::size_t roundingToleranceFrames() const noexcept
    {
        return config.adaptive_playback_cushion
            ? std::min<std::size_t>(config.adaptive_min_frames,
                (static_cast<std::uint64_t>(config.sample_rate) * config.live_tail_rounding_tolerance_us + 999999) / 1000000) : 0;
    }

    std::size_t normalOutputLimitFrames() const noexcept
    {
        const std::size_t target = liveTargetFrames();
        if (target == 0) return config.output_max_frames;
        // Scheduling headroom is one shared budget, not a second queue.
        // Let already-received audio reach the callback-facing ring; retaining
        // it only upstream cannot protect a callback while this worker sleeps.
        const std::size_t requested = target +
            static_cast<std::size_t>(stats.live_tail_batch_tolerance_frames);
        return config.output_max_frames > 0
            ? std::min(config.output_max_frames, requested) : requested;
    }

    void trimLivePathToTarget() noexcept
    {
        if (output == nullptr || receive_catchup_active) {
            return;
        }
        const std::size_t output_target = liveTargetFrames();
        if (output_target == 0) {
            return;
        }
        const std::size_t output_depth = output->depthFrames();
        const std::size_t path_target = normalOutputLimitFrames();
        const std::size_t path_limit = path_target + sourceTailQuantumFrames();
        const std::size_t fill_frames = output_depth < path_limit
            ? path_limit - output_depth
            : 0;
        // Source and output share one budget, including the remainder of a
        // callback-sized burst and a fixed 2ms rounding edge. Do not add
        // the scheduling allowance a second time or turn the edge into padding.
        const std::size_t retain_frames = std::max<std::size_t>(
            static_cast<std::size_t>(config.frames_per_block),
            fill_frames);
        for (auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing) {
                // Do not slice ordinary packet batches as the adaptive target
                // decays. A one-block overshoot retires only the excess above
                // the scheduling tail. Larger bursts shed their obsolete
                // prefix down to the original live target.
                if (peer->queued > retain_frames + roundingToleranceFrames()) {
                    const bool large_burst = peer->queued - retain_frames - roundingToleranceFrames() >
                        static_cast<std::size_t>(config.frames_per_block);
                    const std::size_t original_fill = output_depth < output_target
                        ? output_target - output_depth : 0;
                    (void)peer->retainLiveTail(large_burst
                        ? std::max<std::size_t>(config.frames_per_block, original_fill)
                        : retain_frames + roundingToleranceFrames());
                }
            }
        }
    }

    bool outputAtLimit(std::size_t frames = 0) const noexcept
    {
        if (output == nullptr) {
            return false;
        }
        const std::size_t limit = receive_catchup_active && receive_rebase_started &&
            config.output_max_frames > 0
            ? receiveRecoveryOutputTailFrames()
            : normalOutputLimitFrames();
        return limit > 0 && output->depthFrames() + (frames != 0 ? frames : remainingSlotFrames()) > limit;
    }

    std::size_t remainingSlotFrames() const noexcept
    {
        return mixed_output.size() - slot_frames_released;
    }

    // Move only a common, real source prefix. A fractional resampler tail is
    // usable audio, not a missing packet; padding here would create discard
    // debt every time the clocks crossed a packet boundary.
    std::size_t partialReleaseFrames() const noexcept
    {
        // A raised recovery target must not withhold usable audio while the
        // receive worker sleeps. Keep the same output cap and catch-up/debt
        // protections; moving real frames does not add recovery padding.
        if (!config.adaptive_playback_cushion || output == nullptr ||
            output->callbackFrames() == 0 || receive_catchup_active ||
            receiveRecoveryDebtFrames() != 0) return 0;
        const std::size_t output_depth = output->depthFrames();
        const std::size_t limit = normalOutputLimitFrames();
        if (limit == 0 || output_depth >= limit) return 0;
        std::size_t common = remainingSlotFrames();
        bool contributing = false;
        for (const auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing) {
                contributing = true;
                common = std::min(common, peer->queued);
            }
        }
        common = std::min(common, limit - output_depth);
        // Once recovery has already run out of callback runway, retain its
        // full-callback restart policy. Above that low water, release real
        // prefixes proactively so the next worker hold cannot strand them.
        const std::size_t needed = output_depth < output->callbackFrames()
            ? output->callbackFrames() - output_depth : 0;
        if (adaptive_target_frames > config.adaptive_min_frames && common < needed) return 0;
        return contributing && common < remainingSlotFrames() ? common : 0;
    }

    void releaseSlot(bool complete, std::size_t frames) noexcept
    {
        std::fill(accumulator.begin(), accumulator.end(), 0);
        for (auto& peer : peers) {
            if (!peer->stats.active || !peer->stats.contributing) {
                continue;
            }
            const std::size_t available = std::min<std::size_t>(
                peer->queued,
                frames);
            const std::size_t missing = frames - available;
            if (missing > 0) {
                ++stats.missing_peer_contributions;
                stats.missing_peer_frames += missing;
                peer->noteMissingFrames(missing);
            }
            const int gain = peer->stats.muted ? 0 : peer->stats.gain_ppm;
            std::uint64_t block_peak = 0;
            if (gain == 0) {
                peer->consumeFrames(available);
            } else {
                std::size_t frame_offset = 0;
                auto mixSpan = [&](std::span<const std::int32_t> source) noexcept {
                    for (std::size_t index = 0; index < source.size(); ++index) {
                        const std::int64_t sample = source[index];
                        const std::int64_t contribution = gain == 1000000
                            ? sample
                            : sample * static_cast<std::int64_t>(gain) / 1000000LL;
                        accumulator[frame_offset + index] += contribution;
                        const std::uint64_t magnitude = contribution < 0
                            ? static_cast<std::uint64_t>(-contribution)
                            : static_cast<std::uint64_t>(contribution);
                        block_peak = std::max(block_peak, magnitude);
                    }
                    frame_offset += source.size();
                };
                mixSpan(peer->firstReadableSpan(available));
                mixSpan(peer->secondReadableSpan(available));
                peer->consumeFrames(available);
            }
            const int block_peak_ppm = static_cast<int>(std::min<std::uint64_t>(
                1000000ULL,
                block_peak * 1000000ULL /
                    static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())));
            peer->stats.recent_peak_ppm = std::max(
                block_peak_ppm,
                peer->stats.recent_peak_ppm * 127 / 128);
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (accumulator[frame] < (std::numeric_limits<std::int32_t>::min)() ||
                accumulator[frame] > (std::numeric_limits<std::int32_t>::max)()) {
                ++stats.clipped_samples;
            }
            mixed_output[frame] = saturate_i32(accumulator[frame]);
        }
        (void)pushOutput(std::span<const std::int32_t>(mixed_output).first(frames));
        slot_frames_released += frames;
        slot_had_missing |= !complete;
        if (frames < mixed_output.size()) {
            ++stats.partial_release_events;
            stats.partial_release_frames += frames;
        }
        if (slot_frames_released < mixed_output.size()) return;
        slot_frames_released = 0;
        ++stats.released_slots;
        if (!slot_had_missing) {
            ++stats.complete_slots;
        } else {
            ++stats.deadline_slots;
        }
        slot_had_missing = false;
    }

    void advance(std::uint64_t now_us) noexcept
    {
        observeService(now_us);
        current_advance_us = now_us;
        observePathHighWater(now_us);
        stats.adaptive_delivery_gap_hold_active =
            config.adaptive_playback_cushion && now_us < adaptive_gap_hold_until_us;
        stats.adaptive_delivery_gap_hold_remaining_us =
            stats.adaptive_delivery_gap_hold_active
            ? adaptive_gap_hold_until_us - now_us
            : 0;
        if (output != nullptr && !output->acceptsFrames()) {
            return;
        }
        // Cover a held interpolation neighbour at the measured worker boundary.
        // Predicting one sample cannot cover a whole missing callback, and is
        // unnecessary when real queued audio already covers the next turn.
        const auto boundary_depth = output != nullptr ? output->depthFrames() : 0;
        const auto boundary_callback = output != nullptr ? output->callbackFrames() : 0;
        const bool callback_boundary = boundary_callback != 0 && boundary_depth < boundary_callback;
        const auto worker_runway = stats.worker_runway_frames;
        const bool short_worker_boundary = worker_runway > boundary_callback && boundary_depth < worker_runway &&
            worker_runway - boundary_depth <= kMaximumBoundaryLookaheadFrames;
        if (output != nullptr && boundary_callback != 0 &&
            (callback_boundary || short_worker_boundary) && !receive_catchup_active &&
            receiveRecoveryDebtFrames() == 0 && worker_runway < std::max<std::uint64_t>(
                2 * std::max<std::uint64_t>(mixed_output.size(), boundary_callback), config.adaptive_min_frames)) {
            const auto needed = worker_runway > boundary_depth ? worker_runway - boundary_depth : 1;
            for (auto& peer : peers) {
                if (peer->stats.active && peer->stats.contributing && (callback_boundary || peer->queued < needed))
                    peer->releaseBoundaryLookahead();
            }
        }
        if (!occupancy_dirty && stats.contributing_peers == 0 &&
            !adaptive_release_active && !receive_catchup_active) {
            return;
        }
        const std::uint64_t output_underrun_frames = output != nullptr
            ? output->underrunFrames()
            : observed_output_underrun_frames;
        const bool output_underrun_changed =
            output_underrun_frames > observed_output_underrun_frames;
        if (!occupancy_dirty && !adaptive_release_active && !receive_catchup_active &&
            !output_underrun_changed) {
            if ((!cached_any_ready ||
                    (!cached_all_ready && now_us < next_deadline_us) ||
                    (cached_all_ready && outputAtLimit())) &&
                partialReleaseFrames() == 0) {
                // No mixing work does not mean no callback consumption. Keep
                // the surplus guard aware of low water before the next batch.
                updateAdaptiveReleaseState(false, false);
                return;
            }
        }
        const bool recovery_rebase_was_started = receive_rebase_started;
        processReceiveRecovery(now_us);
        if (!recovery_rebase_was_started && receive_rebase_started) {
            // Recovery can leave one source with a partial block. Reusing an
            // already-due deadline would immediately release that fragment,
            // recreate the same missing-frame debt, and keep the source in a
            // permanent discard/release loop. Give the aligned live tails one
            // normal bounded prefill window before another incomplete release.
            next_deadline_us = now_us + deadlineDelay();
            consecutive_deadline_slots = 0;
        }
        ContributorReadiness readiness = occupancy_dirty
            ? updateOccupancy()
            : ContributorReadiness{cached_all_ready, cached_any_ready};
        if (stats.contributing_peers == 0) {
            cancelRecoveryBudget();
            pending_recovery_padding_goal = 0;
            slot_frames_released = 0;
            slot_had_missing = false;
            stats.release_worker_guard_active = false;
            stats.playback_stopping_frames = 0;
            stats.adaptive_release_alignment_margin_frames = 0;
            if (output != nullptr) {
                output->setResamplerRatio(1.0);
            }
            adaptive_release_active = false;
            surplus_stable_since_us = 0;
            return;
        }
        if (!started) {
            started = true;
            next_deadline_us = now_us + deadlineDelay();
            adaptive_last_update_us = now_us;
            if (output != nullptr) {
                observed_output_underrun_frames = output->underrunFrames();
            }
            // Initial prefill establishes the configured latency before the
            // first real block. Later padding must never outrank queued audio.
            ensureAdaptiveCushion();
        }
        observeAdaptiveDeliveryGaps(now_us);
        bool output_underrun_observed =
            output_underrun_frames > observed_output_underrun_frames;
        const auto missing_frames = output_underrun_observed
            ? output_underrun_frames - observed_output_underrun_frames : 0;
        observed_output_underrun_frames = output_underrun_frames;
        const bool actual_output_underrun = output_underrun_observed;
        if (actual_output_underrun) {
            // An empty output ring does not prove old delay has disappeared:
            // real audio can still be queued upstream or held in flight. Keep
            // bounded outstanding credit; only measured extra consumption and
            // observed current surplus retire it.
            stats.recovery_release_active = false;
            creditRecoveryFrames(missing_frames);
            recovery_clean_since_us = now_us;
            // Raise the target before applying its output gate. Otherwise a
            // full old target can prevent the queued recovery block from being
            // released, hiding the underrun that should expand the cushion.
            const auto quiet_window = std::max(config.adaptive_surplus_stable_us, interval_us);
            const bool isolated = last_small_recovery_observation_us == 0 ||
                (now_us >= last_small_recovery_observation_us &&
                    now_us - last_small_recovery_observation_us >= quiet_window);
            const bool tiny_recovery = config.adaptive_playback_cushion && missing_frames == 1 && isolated &&
                config.adaptive_min_frames <= mixed_output.size() &&
                adaptive_target_frames == config.adaptive_min_frames && !receive_catchup_active &&
                receiveRecoveryDebtFrames() == 0 && now_us >= adaptive_gap_hold_until_us &&
                output->playbackRecoveryState().unity;
            stats.isolated_recovery_events += tiny_recovery;
            last_small_recovery_observation_us = now_us;
            updateAdaptiveTarget(now_us, true, tiny_recovery ? 1 : 0);
            applyPendingAdaptiveGapAfterUnderrun(now_us);
        } else {
            updateAdaptiveReleaseState(false, false);
        }
        // Resolve newly observed starvation before discarding queued audio:
        // the old target may be precisely the limit that caused the underrun.
        trimLivePathToTarget();
        if (occupancy_dirty) {
            readiness = updateOccupancy();
        }
        std::size_t work = 0;
        while (work < config.max_blocks_per_advance) {
            const bool complete = readiness.all_ready;
            const std::size_t partial = complete && !outputAtLimit() ? 0 : partialReleaseFrames();
            if (outputAtLimit(partial != 0 ? partial : remainingSlotFrames())) break;
            if (!complete && partial == 0) {
                // A packet-batched source can briefly be empty while the
                // device still has real audio runway. Preserve that future
                // audio. Once the device actually underruns, however, the
                // shared timeline must advance so delayed catch-up audio is
                // classified as stale instead of becoming permanent latency.
                if (!readiness.any_ready && !output_underrun_observed) {
                    break;
                }
                if (now_us < next_deadline_us) {
                    break;
                }
            }
            releaseSlot(complete || partial != 0,
                partial != 0 ? partial : remainingSlotFrames());
            if (complete || partial != 0) {
                consecutive_deadline_slots = 0;
            } else {
                ++consecutive_deadline_slots;
            }
            readiness = updateOccupancy();
            updateAdaptiveTarget(
                now_us,
                consecutive_deadline_slots >= 3);
            output_underrun_observed = false;
            ++work;
            if (slot_frames_released == 0) {
                if (complete || partial != 0) {
                    if (!readiness.all_ready) next_deadline_us = now_us + deadlineDelay();
                } else {
                    next_deadline_us += deadlineStep();
                }
            }
        }
        // Fill the measured runway goal after the batch; real audio
        // arriving in this batch counts toward that goal.
        if (!readiness.any_ready && work == 0 && actual_output_underrun && output != nullptr) {
            const std::size_t goal = adaptive_target_frames > mixed_output.size()
                ? static_cast<std::size_t>(adaptive_target_frames - mixed_output.size()) : 0;
            pending_recovery_padding_goal = std::max(pending_recovery_padding_goal, goal);
        }
        trimLivePathToTarget();
        if (work == config.max_blocks_per_advance &&
            (readiness.all_ready || now_us >= next_deadline_us)) {
            ++stats.work_budget_yields;
        }
    }

    void finishReceiveBatch(bool receive_budget_exhausted) noexcept
    {
        if (stats.contributing_peers == 0 || output == nullptr || !output->acceptsFrames()) {
            pending_recovery_padding_goal = 0;
        }
        if (pending_recovery_padding_goal && !receive_budget_exhausted &&
            !receive_catchup_active) {
            padOutputTo(std::min<std::uint64_t>(normalOutputLimitFrames(),
                pending_recovery_padding_goal), CushionPurpose::UnderrunRecovery);
            pending_recovery_padding_goal = 0;
        }
        if (!receive_catchup_active) {
            return;
        }
        processReceiveRecovery(current_advance_us);
        if (receive_budget_exhausted || !receive_rebase_started ||
            current_advance_us < receive_recovery_stable_until_us ||
            !receiveRecoveryPathBounded()) {
            return;
        }
        completeReceiveRecovery(current_advance_us);
    }
};

PeerMixer::PeerMixer(const PeerMixerConfig& config, PeerStreamPlayback* output)
    : impl_(std::make_unique<Impl>(config, output))
{
}

PeerMixer::~PeerMixer() = default;
PeerMixer::PeerMixer(PeerMixer&&) noexcept = default;
PeerMixer& PeerMixer::operator=(PeerMixer&&) noexcept = default;

PeerStreamPlayback* PeerMixer::addPeer(std::uint64_t peer_id, std::size_t queue_capacity_frames)
{
    if (peer_id == 0 || impl_->find(peer_id) != nullptr) {
        throw std::runtime_error("PeerMixer requires a unique nonzero peer ID");
    }
    const std::size_t minimum = static_cast<std::size_t>(impl_->config.frames_per_block) * 8U;
    const std::size_t capacity = std::max(queue_capacity_frames, minimum);
    auto peer = std::make_unique<Impl::PeerSlot>(impl_.get(), peer_id, capacity);
    PeerStreamPlayback* playback = peer.get();
    impl_->peers.push_back(std::move(peer));
    impl_->cancelRecoveryBudget();
    impl_->surplus_stable_since_us = 0;
    impl_->updateOccupancy();
    return playback;
}

bool PeerMixer::removePeer(std::uint64_t peer_id) noexcept
{
    const auto found = std::find_if(
        impl_->peers.begin(),
        impl_->peers.end(),
        [peer_id](const auto& peer) { return peer->stats.peer_id == peer_id; });
    if (found == impl_->peers.end()) {
        return false;
    }
    impl_->peers.erase(found);
    impl_->cancelRecoveryBudget();
    impl_->surplus_stable_since_us = 0;
    impl_->updateOccupancy();
    return true;
}

bool PeerMixer::setPeerActive(std::uint64_t peer_id, bool active) noexcept
{
    auto* peer = impl_->find(peer_id);
    if (peer == nullptr) {
        return false;
    }
    if (peer->stats.active != active) {
        impl_->cancelRecoveryBudget();
        peer->resetTimeline();
        peer->stats.active = active;
    }
    impl_->updateOccupancy();
    return true;
}

bool PeerMixer::setPeerGain(std::uint64_t peer_id, int gain_ppm) noexcept
{
    auto* peer = impl_->find(peer_id);
    if (peer == nullptr) {
        return false;
    }
    peer->stats.gain_ppm = std::clamp(gain_ppm, 0, 4000000);
    return true;
}

bool PeerMixer::setPeerMuted(std::uint64_t peer_id, bool muted) noexcept
{
    auto* peer = impl_->find(peer_id);
    if (peer == nullptr) {
        return false;
    }
    peer->stats.muted = muted;
    return true;
}

void PeerMixer::advance(std::uint64_t now_us) noexcept
{
    impl_->advance(now_us);
}

void PeerMixer::finishReceiveBatch(bool receive_budget_exhausted) noexcept
{
    impl_->finishReceiveBatch(receive_budget_exhausted);
}

void PeerMixer::finish(std::uint64_t now_us) noexcept
{
    impl_->advance(now_us);
}

const PeerMixerStats& PeerMixer::stats() const noexcept
{
    return impl_->stats;
}

const PeerMixerPeerStats* PeerMixer::peerStats(std::uint64_t peer_id) const noexcept
{
    const auto* peer = impl_->find(peer_id);
    return peer != nullptr ? &peer->stats : nullptr;
}

} // namespace jam2
