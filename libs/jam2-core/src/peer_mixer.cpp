#include "peer_mixer.hpp"
#include "runtime_limits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jam2 {
namespace {

constexpr double kMinimumRatio = 0.995;
constexpr double kMaximumRatio = 1.005;

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
            stats.contributing = false;
            stats.queue_depth_frames = 0;
            stats.recent_peak_ppm = 0;
            owner->occupancy_dirty = true;
        }

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
    };

    const PeerMixerConfig config;
    PeerStreamPlayback* output = nullptr;
    PeerMixerStats stats;
    std::vector<std::unique_ptr<PeerSlot>> peers;
    std::vector<std::int64_t> accumulator;
    std::vector<std::int32_t> mixed_output;
    std::vector<std::int32_t> silence_output;
    bool started = false;
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
        const std::uint64_t numerator =
            static_cast<std::uint64_t>(config.frames_per_block) * 1000000ULL;
        interval_us = numerator / interval_denominator;
        interval_remainder_us = numerator % interval_denominator;
        if (output != nullptr) {
            output->setResamplerRatio(1.0);
        }
        stats.adaptive_playback_cushion_enabled = config.adaptive_playback_cushion;
        stats.adaptive_target_frames = adaptive_target_frames;
        stats.receive_recovery_trigger_frames = config.receive_recovery_trigger_frames;
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
            adaptive_target_frames = config.adaptive_min_frames;
            adaptive_release_accumulator_frames = 0.0;
            adaptive_release_active = false;
            stats.adaptive_target_frames = adaptive_target_frames;
            receive_rebase_started = true;
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
        const std::size_t frames_per_block =
            static_cast<std::size_t>(config.frames_per_block);
        for (auto& peer : peers) {
            if (peer->stats.active) {
                ++active;
            }
            if (peer->stats.active && peer->stats.contributing) {
                ++contributing;
                const bool ready = peer->queued >= frames_per_block;
                all_ready = all_ready && ready;
                any_ready = any_ready || ready;
                active_slots = std::max<std::uint64_t>(
                    active_slots,
                    (peer->queued + frames_per_block - 1U) / frames_per_block);
            }
            max_slots += (peer->queue.size() + frames_per_block - 1U) / frames_per_block;
            peer->stats.queue_depth_frames = peer->queued;
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

    void ensureAdaptiveCushion() noexcept
    {
        if (!config.adaptive_playback_cushion || output == nullptr) {
            return;
        }
        const std::uint64_t cushion_goal = adaptive_target_frames > mixed_output.size()
            ? adaptive_target_frames - mixed_output.size()
            : 0;
        std::size_t work = 0;
        while (output->depthFrames() < cushion_goal &&
               work < config.max_blocks_per_advance) {
            const std::size_t missing = static_cast<std::size_t>(
                cushion_goal - output->depthFrames());
            const std::size_t count = std::min(missing, silence_output.size());
            const std::size_t pushed = pushOutput(
                std::span<const std::int32_t>(silence_output.data(), count));
            stats.adaptive_padding_frames += pushed;
            if (pushed < count) {
                break;
            }
            ++work;
        }
        if (work == config.max_blocks_per_advance && output->depthFrames() < cushion_goal) {
            ++stats.work_budget_yields;
        }
    }

    void updateAdaptiveReleaseState(
        bool missing,
        bool allow_start,
        bool source_ready) noexcept
    {
        if (!config.adaptive_playback_cushion || output == nullptr) {
            if (missing) {
                adaptive_release_active = false;
            }
            return;
        }
        const int effective_release_ppm = config.adaptive_release_ppm;
        const std::uint64_t actual_depth = output->depthFrames();
        const std::uint64_t release_stop_tolerance =
            static_cast<std::uint64_t>(config.frames_per_block) * 4ULL;
        const std::uint64_t release_start_tolerance =
            static_cast<std::uint64_t>(config.frames_per_block) * 7ULL;
        const bool actual_depth_above_start =
            actual_depth > adaptive_target_frames &&
            actual_depth - adaptive_target_frames > release_start_tolerance;
        const bool actual_depth_at_stop =
            actual_depth <= adaptive_target_frames ||
            actual_depth - adaptive_target_frames <= release_stop_tolerance;

        const bool recovery_cushion_active =
            adaptive_target_frames > config.adaptive_min_frames;
        if (missing || !source_ready || !recovery_cushion_active) {
            adaptive_release_active = false;
        } else {
            // Output depth changes in the device callback even when no peer
            // block is ready. Stop release as soon as the peer runway is gone,
            // and never use ordinary packet batching at the minimum target as
            // a reason to accelerate playback. Only cushion explicitly raised
            // after an underrun may be retired at a non-unity ratio.
            if (adaptive_release_active && actual_depth_at_stop) {
                adaptive_release_active = false;
            }
            if (!adaptive_release_active && allow_start &&
                effective_release_ppm > 0 && actual_depth_above_start) {
                adaptive_release_active = true;
            }
        }
        const bool releasing = !missing && source_ready &&
            recovery_cushion_active && effective_release_ppm > 0 &&
            adaptive_release_active;
        output->setResamplerRatio(
            releasing ? 1.0 + static_cast<double>(effective_release_ppm) / 1000000.0 : 1.0);
    }

    void updateAdaptiveTarget(
        std::uint64_t now_us,
        bool missing,
        bool source_ready) noexcept
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
                        adaptive_target_frames + static_cast<std::uint64_t>(config.frames_per_block)));
                ++stats.adaptive_raise_events;
            }
        } else if (!missing && adaptive_target_frames > config.adaptive_min_frames &&
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
            const std::uint64_t release = std::min<std::uint64_t>(
                available_release,
                adaptive_target_frames - config.adaptive_min_frames);
            if (release > 0) {
                adaptive_target_frames -= release;
                adaptive_release_accumulator_frames -= static_cast<double>(release);
                ++stats.adaptive_release_events;
            }
        }
        updateAdaptiveReleaseState(missing, true, source_ready);
        adaptive_last_update_us = now_us;
        stats.adaptive_target_frames = adaptive_target_frames;
    }

    std::size_t normalOutputLimitFrames() const noexcept
    {
        const std::size_t target = liveTargetFrames();
        return target > 0 ? target : config.output_max_frames;
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
        const std::size_t fill_frames = output_depth < output_target
            ? output_target - output_depth
            : 0;
        // Keep enough source audio to fill the moving output target, plus at
        // least one complete callback when the output is already at target.
        // The latter is the bounded scheduling allowance for a packet that
        // arrives just before the device consumes its next block.
        const std::size_t retain_frames = std::max<std::size_t>(
            static_cast<std::size_t>(config.frames_per_block),
            fill_frames);
        for (auto& peer : peers) {
            if (peer->stats.active && peer->stats.contributing) {
                (void)peer->retainLiveTail(retain_frames);
            }
        }
    }

    bool outputAtLimit() const noexcept
    {
        if (output == nullptr) {
            return false;
        }
        const std::size_t limit = receive_catchup_active && receive_rebase_started &&
            config.output_max_frames > 0
            ? receiveRecoveryOutputTailFrames()
            : normalOutputLimitFrames();
        return limit > 0 && output->depthFrames() + mixed_output.size() > limit;
    }

    void releaseSlot(bool complete) noexcept
    {
        std::fill(accumulator.begin(), accumulator.end(), 0);
        for (auto& peer : peers) {
            if (!peer->stats.active || !peer->stats.contributing) {
                continue;
            }
            const std::size_t available = std::min<std::size_t>(
                peer->queued,
                static_cast<std::size_t>(config.frames_per_block));
            const std::size_t missing = static_cast<std::size_t>(config.frames_per_block) - available;
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
        for (std::size_t frame = 0; frame < mixed_output.size(); ++frame) {
            if (accumulator[frame] < (std::numeric_limits<std::int32_t>::min)() ||
                accumulator[frame] > (std::numeric_limits<std::int32_t>::max)()) {
                ++stats.clipped_samples;
            }
            mixed_output[frame] = saturate_i32(accumulator[frame]);
        }
        (void)pushOutput(mixed_output);
        ++stats.released_slots;
        if (complete) {
            ++stats.complete_slots;
        } else {
            ++stats.deadline_slots;
        }
    }

    void advance(std::uint64_t now_us) noexcept
    {
        current_advance_us = now_us;
        if (output != nullptr && !output->acceptsFrames()) {
            return;
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
            if (!cached_any_ready ||
                (!cached_all_ready && now_us < next_deadline_us) ||
                (cached_all_ready && outputAtLimit())) {
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
            if (output != nullptr) {
                output->setResamplerRatio(1.0);
            }
            adaptive_release_active = false;
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
        trimLivePathToTarget();
        if (occupancy_dirty) {
            readiness = updateOccupancy();
        }
        bool output_underrun_observed =
            output_underrun_frames > observed_output_underrun_frames;
        observed_output_underrun_frames = output_underrun_frames;
        const bool actual_output_underrun = output_underrun_observed;
        if (actual_output_underrun) {
            // Raise the target before applying its output gate. Otherwise a
            // full old target can prevent the queued recovery block from being
            // released, hiding the underrun that should expand the cushion.
            updateAdaptiveTarget(now_us, true, readiness.any_ready);
        } else {
            updateAdaptiveReleaseState(false, false, readiness.any_ready);
        }
        std::size_t work = 0;
        while (work < config.max_blocks_per_advance) {
            if (outputAtLimit()) {
                break;
            }
            const bool complete = readiness.all_ready;
            if (!complete) {
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
            releaseSlot(complete);
            if (complete) {
                consecutive_deadline_slots = 0;
            } else {
                ++consecutive_deadline_slots;
            }
            readiness = updateOccupancy();
            updateAdaptiveTarget(
                now_us,
                consecutive_deadline_slots >= 3,
                readiness.any_ready);
            output_underrun_observed = false;
            ++work;
            if (complete) {
                if (!readiness.all_ready) {
                    next_deadline_us = now_us + deadlineDelay();
                }
            } else {
                next_deadline_us += deadlineStep();
            }
        }
        // Initial prefill owns proactive cushion silence. After playback has
        // started, an empty packet queue is only a transient scheduling state;
        // padding it recreates the exact speed-up/silence feedback loop that
        // release is meant to remove. Recovery padding is therefore permitted
        // only after the device reports a real underrun and no real block was
        // available during this advance.
        if (!readiness.any_ready && work == 0 && actual_output_underrun) {
            ensureAdaptiveCushion();
        }
        trimLivePathToTarget();
        if (work == config.max_blocks_per_advance &&
            (readiness.all_ready || now_us >= next_deadline_us)) {
            ++stats.work_budget_yields;
        }
    }

    void finishReceiveBatch(bool receive_budget_exhausted) noexcept
    {
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
