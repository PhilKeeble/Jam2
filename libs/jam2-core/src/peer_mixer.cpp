#include "peer_mixer.hpp"

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
        std::size_t read_index = 0;
        std::size_t write_index = 0;
        std::size_t queued = 0;
        bool has_previous_sample = false;
        std::int32_t previous_sample = 0;
        double next_output_phase = 0.0;

        PeerSlot(Impl* mixer, std::uint64_t id, std::size_t capacity)
            : owner(mixer),
              queue(capacity, 0)
        {
            stats.peer_id = id;
            stats.queue_capacity_frames = capacity;
        }

        void resetTimeline() noexcept
        {
            read_index = 0;
            write_index = 0;
            queued = 0;
            has_previous_sample = false;
            previous_sample = 0;
            next_output_phase = 0.0;
            stats.contributing = false;
            stats.queue_depth_frames = 0;
        }

        bool enqueue(std::int32_t sample) noexcept
        {
            if (queued == queue.size()) {
                ++stats.queue_capacity_drops;
                ++stats.queue_capacity_dropped_frames;
                ++owner->stats.capacity_drops;
                ++owner->stats.capacity_dropped_frames;
                return false;
            }
            queue[write_index] = sample;
            write_index = (write_index + 1U) % queue.size();
            ++queued;
            stats.contributing = true;
            stats.queue_depth_frames = queued;
            stats.queue_high_water_frames = std::max<std::uint64_t>(
                stats.queue_high_water_frames,
                queued);
            return true;
        }

        std::int32_t pop() noexcept
        {
            const std::int32_t sample = queue[read_index];
            read_index = (read_index + 1U) % queue.size();
            --queued;
            stats.queue_depth_frames = queued;
            return sample;
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
            std::size_t pushed = 0;
            const double ratio = std::clamp(stats.resampler_ratio, kMinimumRatio, kMaximumRatio);
            for (const std::int32_t sample : frames) {
                if (!has_previous_sample) {
                    has_previous_sample = true;
                    previous_sample = sample;
                    next_output_phase = ratio;
                    if (enqueue(sample)) {
                        ++pushed;
                    }
                    ++stats.resampled_output_frames;
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
                    if (enqueue(output_sample)) {
                        ++pushed;
                    }
                    ++stats.resampled_output_frames;
                    next_output_phase += ratio;
                }
                next_output_phase -= 1.0;
                previous_sample = sample;
            }
            return pushed;
        }

        void requestDropFrames(std::size_t frames) noexcept override
        {
            const std::size_t dropped = std::min(frames, queued);
            read_index = (read_index + dropped) % queue.size();
            queued -= dropped;
            stats.requested_drop_frames += dropped;
            stats.queue_depth_frames = queued;
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
    std::uint64_t consecutive_deadline_slots = 0;

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
        if (config.sample_rate <= 0 || config.frames_per_block <= 0 ||
            config.max_blocks_per_advance == 0) {
            throw std::runtime_error("invalid PeerMixer configuration");
        }
        if (config.adaptive_playback_cushion &&
            (config.adaptive_min_frames > config.adaptive_target_frames ||
             config.adaptive_target_frames > config.adaptive_max_frames)) {
            throw std::runtime_error("PeerMixer adaptive playback bounds are inconsistent");
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

    void updateOccupancy() noexcept
    {
        std::uint64_t active = 0;
        std::uint64_t contributing = 0;
        std::uint64_t active_slots = 0;
        std::uint64_t max_slots = 0;
        for (auto& peer : peers) {
            if (peer->stats.active) {
                ++active;
            }
            if (peer->stats.active && peer->stats.contributing) {
                ++contributing;
                active_slots = std::max<std::uint64_t>(
                    active_slots,
                    (peer->queued + static_cast<std::size_t>(config.frames_per_block) - 1U) /
                        static_cast<std::size_t>(config.frames_per_block));
            }
            max_slots += (peer->queue.size() + static_cast<std::size_t>(config.frames_per_block) - 1U) /
                static_cast<std::size_t>(config.frames_per_block);
            peer->stats.queue_depth_frames = peer->queued;
        }
        stats.active_peers = active;
        stats.contributing_peers = contributing;
        stats.active_slots = active_slots;
        stats.max_slots = max_slots;
        stats.active_slots_high_water = std::max(stats.active_slots_high_water, active_slots);
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

    void updateAdaptiveTarget(std::uint64_t now_us, bool missing) noexcept
    {
        if (!config.adaptive_playback_cushion) {
            return;
        }
        if (missing && adaptive_target_frames < config.adaptive_max_frames) {
            adaptive_target_frames = std::min<std::uint64_t>(
                config.adaptive_max_frames,
                std::max<std::uint64_t>(
                    config.adaptive_min_frames,
                    adaptive_target_frames + static_cast<std::uint64_t>(config.frames_per_block)));
            ++stats.adaptive_raise_events;
        } else if (!missing && adaptive_target_frames > config.adaptive_min_frames &&
                   config.adaptive_release_ppm > 0 && adaptive_last_update_us != 0 &&
                   now_us > adaptive_last_update_us) {
            const double release_frames =
                static_cast<double>(adaptive_target_frames) *
                static_cast<double>(config.adaptive_release_ppm) *
                (static_cast<double>(now_us - adaptive_last_update_us) / 1000000.0) / 1000000.0;
            const std::uint64_t release = static_cast<std::uint64_t>(release_frames);
            if (release > 0) {
                adaptive_target_frames = adaptive_target_frames > release
                    ? std::max<std::uint64_t>(config.adaptive_min_frames, adaptive_target_frames - release)
                    : config.adaptive_min_frames;
                ++stats.adaptive_release_events;
            }
        }
        adaptive_last_update_us = now_us;
        stats.adaptive_target_frames = adaptive_target_frames;
    }

    bool allContributorsReady() const noexcept
    {
        bool any = false;
        for (const auto& peer : peers) {
            if (!peer->stats.active || !peer->stats.contributing) {
                continue;
            }
            any = true;
            if (peer->queued < static_cast<std::size_t>(config.frames_per_block)) {
                return false;
            }
        }
        return any;
    }

    bool outputAtLimit() const noexcept
    {
        return output != nullptr && config.output_max_frames > 0 &&
            output->depthFrames() + mixed_output.size() > config.output_max_frames;
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
            }
            const int gain = peer->stats.muted ? 0 : peer->stats.gain_ppm;
            for (std::size_t frame = 0; frame < available; ++frame) {
                const std::int64_t sample = peer->pop();
                accumulator[frame] += sample * static_cast<std::int64_t>(gain) / 1000000LL;
            }
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
        updateOccupancy();
        if (stats.contributing_peers == 0) {
            return;
        }
        if (!started) {
            started = true;
            next_deadline_us = now_us + deadlineDelay();
            adaptive_last_update_us = now_us;
        }
        ensureAdaptiveCushion();
        std::size_t work = 0;
        while (work < config.max_blocks_per_advance) {
            if (outputAtLimit()) {
                break;
            }
            const bool complete = allContributorsReady();
            if (!complete && now_us < next_deadline_us) {
                break;
            }
            releaseSlot(complete);
            if (complete) {
                consecutive_deadline_slots = 0;
            } else {
                ++consecutive_deadline_slots;
            }
            updateAdaptiveTarget(now_us, consecutive_deadline_slots >= 3);
            ++work;
            if (complete) {
                if (!allContributorsReady()) {
                    next_deadline_us = now_us + deadlineDelay();
                }
            } else {
                next_deadline_us += deadlineStep();
            }
            updateOccupancy();
        }
        if (work == config.max_blocks_per_advance &&
            (allContributorsReady() || now_us >= next_deadline_us)) {
            ++stats.work_budget_yields;
        }
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
