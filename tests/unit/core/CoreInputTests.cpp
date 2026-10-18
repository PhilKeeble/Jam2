#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "common.hpp"
#include "input_source.hpp"
// Native unit coverage is owned by the repository-level test tree.
#include "audio_device_processing.hpp"
#include "engine.hpp"
#include "midi.hpp"
#include "peer_mixer.hpp"
#include "network_session.hpp"
#include "protocol.hpp"
#include "tuning_profile.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* name)
{
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

class StereoRenderer final : public jam2::audio::InputSourceRenderer {
public:
    bool render_mono(
        const jam2::audio::InputSourceRenderRequest& request,
        std::span<std::int32_t> output) noexcept override
    {
        if (request.input_channels != 2 || request.frames != output.size()) return false;
        for (std::size_t frame = 0; frame < output.size(); ++frame) {
            output[frame] = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(request.inputs[0][frame]) +
                 static_cast<std::int64_t>(request.inputs[1][frame])) / 2);
        }
        return true;
    }

    std::size_t latency_frames(std::size_t) const noexcept override
    {
        return 23;
    }
};

class FailedInstrument final : public jam2::audio::InputSourceRenderer {
public:
    bool render_mono(
        const jam2::audio::InputSourceRenderRequest&,
        std::span<std::int32_t>) noexcept override
    {
        return false;
    }
};

class CountingInstrument final : public jam2::audio::InputSourceRenderer {
public:
    bool render_mono(const jam2::audio::InputSourceRenderRequest&,
        std::span<std::int32_t> output) noexcept override
    {
        ++blocks;
        std::fill(output.begin(), output.end(), 123);
        return true;
    }
    int blocks = 0;
};

class MixerSink final : public jam2::PeerStreamPlayback {
public:
    bool acceptsFrames() const noexcept override { return true; }
    std::size_t depthFrames() const noexcept override { return depth; }
    std::uint64_t underrunFrames() const noexcept override { return underruns; }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        depth += frames.size();
        return frames.size();
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        depth -= std::min(depth, frames);
    }
    void setResamplerRatio(double ratio) noexcept override
    {
        resamplerRatio = ratio;
    }

    void consume(std::size_t frames) noexcept
    {
        if (frames > depth) {
            underruns += frames - depth;
            depth = 0;
            return;
        }
        depth -= frames;
    }

    std::size_t depth = 0;
    std::uint64_t underruns = 0;
    double resamplerRatio = 1.0;
};

class MarkerMixerSink final : public jam2::PeerStreamPlayback {
public:
    std::size_t callbackFrames() const noexcept override { return callbackQuantum; }
    std::size_t depthFrames() const noexcept override { return depth; }
    std::uint64_t underrunFrames() const noexcept override { return underruns; }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        samples.insert(samples.end(), frames.begin(), frames.end());
        depth += frames.size();
        return frames.size();
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        depth -= std::min(depth, frames);
    }
    void setResamplerRatio(double) noexcept override {}
    void consume(std::size_t frames) noexcept
    {
        if (frames > depth) {
            underruns += frames - depth;
            depth = 0;
            return;
        }
        depth -= frames;
    }

    std::size_t callbackQuantum = 0;
    std::size_t depth = 0;
    std::uint64_t underruns = 0;
    std::vector<std::int32_t> samples;
};

class LiveTailMixerSink final : public jam2::PeerStreamPlayback {
public:
    std::size_t depthFrames() const noexcept override { return samples.size(); }
    std::uint64_t underrunFrames() const noexcept override { return underruns; }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        samples.insert(samples.end(), frames.begin(), frames.end());
        return frames.size();
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        const std::size_t dropped = std::min(frames, samples.size());
        for (std::size_t index = 0; index < dropped; ++index) {
            samples.pop_front();
        }
    }
    void setResamplerRatio(double) noexcept override {}
    void consume(std::size_t frames) noexcept
    {
        const std::size_t consumed = std::min(frames, samples.size());
        for (std::size_t index = 0; index < consumed; ++index) {
            samples.pop_front();
        }
        underruns += frames - consumed;
    }

    std::deque<std::int32_t> samples;
    std::uint64_t underruns = 0;
};

class ResampledMixerSink final : public jam2::PeerStreamPlayback {
public:
    jam2::audio::PlaybackRecoveryState playbackRecoveryState() const noexcept override
    { return jam2::audio::device_processing::read_playback_recovery_state(control); }
    ResampledMixerSink()
        : ring(4096)
    {
        ring.set_diagnostics_enabled(true);
        control.playback_ratio_ramp_frames.store(12000, std::memory_order_relaxed);
    }

    bool acceptsFrames() const noexcept override { return true; }
    std::size_t depthFrames() const noexcept override { return ring.available_read(); }
    std::uint64_t underrunFrames() const noexcept override
    {
        return ring.underrun_frames();
    }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        return ring.push(frames);
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        ring.request_drop_oldest(frames);
    }
    void setResamplerRatio(double ratio) noexcept override
    {
        control.playback_ratio_ppm.store(
            static_cast<int>(ratio * 1000000.0 + 0.5),
            std::memory_order_relaxed);
    }

    void callback() noexcept
    {
        jam2::audio::device_processing::pop_resampled_playback(
            &ring, &control, resampler, resamplerSource, callbackOutput);
    }

    jam2::audio::MonoRingBuffer ring;
    jam2::audio::StreamControl control;
    jam2::audio::device_processing::PlaybackResamplerState resampler;
    std::array<std::int32_t, 66> resamplerSource{};
    std::array<std::int32_t, 32> callbackOutput{};
};

class ProfileMixerSink final : public jam2::PeerStreamPlayback {
public:
    jam2::audio::PlaybackRecoveryState playbackRecoveryState() const noexcept override
    { return jam2::audio::device_processing::read_playback_recovery_state(control); }
    ProfileMixerSink(std::size_t capacityFrames, std::size_t prefillFrames, std::uint64_t sampleRate = 48000)
        : ring(capacityFrames), prefillFrames(prefillFrames)
    {
        ring.set_diagnostics_enabled(true);
        control.playback_ratio_ramp_frames.store(sampleRate / 4, std::memory_order_relaxed);
        control.playback_release_return_ramp_frames.store(
            sampleRate * jam2::audio::kPlaybackReleaseReturnRampMs / 1000, std::memory_order_relaxed);
    }

    bool acceptsFrames() const noexcept override { return true; }
    std::size_t callbackFrames() const noexcept override { return callbackQuantum; }
    std::size_t depthFrames() const noexcept override { return ring.available_read(); }
    std::uint64_t underrunFrames() const noexcept override
    {
        return ring.underrun_frames();
    }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        return ring.push(frames);
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        ring.request_drop_oldest(frames);
    }
    void setResamplerRatio(double ratio) noexcept override
    {
        // Test-only counterfactual: veto acceleration near callback-facing low
        // water. It does not change the mixer, target decay or queue behavior.
        if (releaseGuardFrames != 0 && ring.available_read() <= releaseGuardFrames) ratio = 1.0;
        control.playback_ratio_ppm.store(
            static_cast<int>(ratio * 1000000.0 + 0.5),
            std::memory_order_relaxed);
    }

    void callback(std::size_t frames = 64) noexcept
    {
        if (!started) {
            if (ring.available_read() < prefillFrames) {
                return;
            }
            started = true;
        }
        jam2::audio::device_processing::pop_resampled_playback(
            &ring, &control, resampler, resamplerSource,
            std::span<std::int32_t>(callbackOutput).first(frames));
    }

    jam2::audio::MonoRingBuffer ring;
    jam2::audio::StreamControl control;
    jam2::audio::device_processing::PlaybackResamplerState resampler;
    std::array<std::int32_t, 520> resamplerSource{};
    std::array<std::int32_t, 256> callbackOutput{};
    std::size_t prefillFrames = 0;
    std::size_t callbackQuantum = 64;
    std::size_t releaseGuardFrames = 0;
    bool started = false;
};

class DeliveryGapSink final : public jam2::PeerStreamPlayback {
public:
    std::size_t depthFrames() const noexcept override { return depth; }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        depth += frames.size();
        return frames.size();
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        depth -= std::min(depth, frames);
    }
    void setResamplerRatio(double) noexcept override {}
    void observeDeliveryGap(const jam2::PeerDeliveryGap& gap) noexcept override
    {
        ++gapEvents;
        lastGap = gap;
    }

    std::size_t depth = 0;
    std::uint64_t gapEvents = 0;
    jam2::PeerDeliveryGap lastGap;
};

struct WifiBurstResult {
    bool packetsAccepted = true;
    std::uint64_t sequenceLoss = 0;
    std::uint64_t missingAudioFrames = 0;
    std::uint64_t underrunBursts = 0;
    std::uint64_t underrunFrames = 0;
    std::uint64_t startupPaddingFrames = 0;
    std::uint64_t paddingFrames = 0;
    std::uint64_t trimmedFrames = 0;
    std::uint64_t adaptiveTargetFrames = 0;
    std::uint64_t adaptiveMaximumFrames = 0;
    std::uint64_t deliveryGapRaiseEvents = 0;
    bool deliveryGapHoldActive = false;
    std::size_t maximumBufferedPathFrames = 0;
    double averageBufferedPathFrames = 0.0;
};

WifiBurstResult run_profile_wifi_burst(
    std::string_view profileName,
    bool impaired,
    bool includeExtremeGap,
    std::size_t peerCount = 1,
    std::uint64_t routineGapUs = 100000,
    bool deliveryGapLearning = true)
{
    const auto* createProfile = jam2::find_create_profile(profileName);
    if (createProfile == nullptr || createProfile->local == nullptr) {
        return {.packetsAccepted = false};
    }
    const auto& profile = *createProfile->local;
    const std::size_t packetFrames = static_cast<std::size_t>(createProfile->frame_size);
    ProfileMixerSink sink(profile.playback_ring_frames, profile.playback_prefill_frames);

    jam2::PeerMixerConfig mixerConfig;
    mixerConfig.sample_rate = createProfile->sample_rate;
    mixerConfig.frames_per_block = createProfile->frame_size;
    mixerConfig.deadline_frames = std::max<std::size_t>({
        packetFrames * 2U,
        profile.playout_delay_frames,
        profile.jitter_buffer_frames,
        profile.jitter_buffer_max_frames,
    });
    mixerConfig.output_max_frames = profile.playback_max_frames;
    mixerConfig.max_blocks_per_advance = 64;
    mixerConfig.fixed_target_frames = profile.playout_delay_frames;
    mixerConfig.adaptive_playback_cushion = profile.adaptive_playback_cushion;
    mixerConfig.adaptive_target_frames = profile.adaptive_playback_target_frames;
    mixerConfig.adaptive_min_frames = profile.adaptive_playback_min_frames;
    mixerConfig.adaptive_max_frames = profile.adaptive_playback_max_frames;
    mixerConfig.adaptive_release_ppm = profile.adaptive_playback_release_ppm;
    jam2::PeerMixer mixer(mixerConfig, &sink);

    const std::size_t baseCapacity = std::max(
        profile.playback_ring_frames,
        profile.adaptive_playback_max_frames);
    const std::size_t timingHeadroom = std::max(
        profile.adaptive_playback_max_frames,
        profile.jitter_buffer_max_frames);
    std::vector<jam2::PeerStreamPlayback*> peerPlaybacks;
    peerPlaybacks.reserve(peerCount);
    for (std::size_t peer = 0; peer < peerCount; ++peer) {
        const std::uint64_t peerId = 7001U + peer;
        auto* peerPlayback = mixer.addPeer(peerId, baseCapacity + timingHeadroom);
        if (peerPlayback == nullptr || !mixer.setPeerActive(peerId, true)) {
            return {.packetsAccepted = false};
        }
        peerPlaybacks.push_back(peerPlayback);
    }

    jam2::PeerStreamConfig streamConfig;
    streamConfig.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    streamConfig.sample_rate = createProfile->sample_rate;
    streamConfig.frames_per_packet = createProfile->frame_size;
    streamConfig.sample_time_playout = profile.sample_time_playout;
    streamConfig.playout_delay_frames = profile.playout_delay_frames;
    streamConfig.playback_max_frames = 0;
    streamConfig.playback_queue_capacity_frames = profile.playback_ring_frames;
    streamConfig.jitter_buffer_frames = profile.jitter_buffer_frames;
    streamConfig.jitter_buffer_max_frames = profile.jitter_buffer_max_frames;
    streamConfig.adaptive_playback_cushion = false;
    streamConfig.drift_correction = profile.drift_correction;
    streamConfig.drift_smoothing = profile.drift_smoothing;
    streamConfig.drift_deadband_ppm = profile.drift_deadband_ppm;
    streamConfig.drift_max_correction_ppm = profile.drift_max_correction_ppm;
    if (!deliveryGapLearning) {
        streamConfig.adaptive_delivery_gap_minimum_us =
            (std::numeric_limits<std::uint64_t>::max)();
    }
    // Delivery-gap learning is playback control, not telemetry. Keep the
    // fixture inside stats warm-up with diagnostics disabled so this behavior
    // cannot accidentally depend on CSV collection being enabled.
    streamConfig.stats_warmup_us = 60000000;
    streamConfig.collect_diagnostics = false;
    std::vector<std::unique_ptr<jam2::PeerStream>> streams;
    streams.reserve(peerCount);
    for (auto* peerPlayback : peerPlaybacks) {
        streams.push_back(std::make_unique<jam2::PeerStream>(
            streamConfig,
            0,
            peerPlayback));
    }

    struct PendingPacket {
        std::uint64_t deliveryFrame = 0;
        std::uint32_t sequence = 0;
        std::uint64_t sampleTime = 0;
    };
    std::deque<PendingPacket> pending;
    const std::vector<std::uint8_t> payload(packetFrames * 2U, 0);
    constexpr std::uint64_t sampleRate = 48000;
    constexpr std::uint64_t callbackFrames = 64;
    constexpr std::uint64_t testSeconds = 40;
    constexpr std::uint64_t impairmentStartSecond = 4;
    const std::uint64_t routineGapFrames = sampleRate * routineGapUs / 1000000U;
    constexpr std::uint64_t extremeGapFrames = sampleRate * 17U / 100U;
    constexpr std::uint64_t extremeGapSecond = 25;
    std::uint64_t timelineFrame = 0;
    std::uint64_t nextPacketFrame = packetFrames;
    std::uint32_t nextSequence = 0;
    std::size_t maximumBufferedPath = 0;
    long double bufferedPathSum = 0.0L;
    std::uint64_t bufferedPathSamples = 0;
    std::uint64_t startupPadding = 0;
    bool capturedStartupPadding = false;
    bool accepted = true;

    while (timelineFrame < sampleRate * testSeconds) {
        timelineFrame += callbackFrames;
        const std::uint64_t nowUs = timelineFrame * 1000000ULL / sampleRate;
        while (nextPacketFrame <= timelineFrame) {
            std::uint64_t deliveryFrame = nextPacketFrame;
            if (impaired && nextPacketFrame >= sampleRate * impairmentStartSecond) {
                const std::uint64_t second = nextPacketFrame / sampleRate;
                const std::uint64_t phase = nextPacketFrame % sampleRate;
                const std::uint64_t heldFrames = includeExtremeGap && second == extremeGapSecond
                    ? extremeGapFrames
                    : routineGapFrames;
                if (phase >= sampleRate - heldFrames) {
                    deliveryFrame = (second + 1U) * sampleRate;
                }
            }
            pending.push_back({
                deliveryFrame,
                nextSequence,
                static_cast<std::uint64_t>(nextSequence) * packetFrames,
            });
            ++nextSequence;
            nextPacketFrame += packetFrames;
        }

        sink.callback();
        // The runtime expires deadlines before draining a catch-up batch.
        for (auto& stream : streams) stream->advance(nowUs);
        mixer.advance(nowUs);
        while (!pending.empty() && pending.front().deliveryFrame <= timelineFrame) {
            const PendingPacket packet = pending.front();
            pending.pop_front();
            const jam2::protocol::Header header{
                jam2::protocol::PacketType::Audio,
                1,
                packet.sequence,
                packet.sampleTime,
                static_cast<std::uint16_t>(payload.size()),
                0,
            };
            for (auto& stream : streams) {
                accepted = accepted && stream->receiveAudio(header, payload, nowUs) ==
                    jam2::PeerAudioResult::Accepted;
            }
        }
        for (auto& stream : streams) {
            stream->advance(nowUs);
        }
        mixer.advance(nowUs);
        mixer.finishReceiveBatch(false);
        if (sink.started && !capturedStartupPadding) {
            startupPadding = mixer.stats().adaptive_padding_frames;
            capturedStartupPadding = true;
        }
        std::size_t currentMaximumBufferedPath = 0;
        for (std::size_t peer = 0; peer < streams.size(); ++peer) {
            const auto* peerStats = mixer.peerStats(7001U + peer);
            const std::size_t peerDepth = peerStats != nullptr
                ? static_cast<std::size_t>(peerStats->queue_depth_frames)
                : 0;
            currentMaximumBufferedPath = std::max(
                currentMaximumBufferedPath,
                sink.depthFrames() + peerDepth + static_cast<std::size_t>(
                    streams[peer]->stats().jitter_buffer_depth_frames));
        }
        maximumBufferedPath = std::max(maximumBufferedPath, currentMaximumBufferedPath);
        if (sink.started) {
            bufferedPathSum += currentMaximumBufferedPath;
            ++bufferedPathSamples;
        }
    }

    const auto ringStats = sink.ring.stats();
    std::uint64_t lostPackets = 0;
    std::uint64_t missingFrames = 0;
    for (const auto& stream : streams) {
        lostPackets += stream->stats().sequence.lost;
        missingFrames += stream->stats().missing_audio_frames_inserted;
    }
    return {
        accepted,
        lostPackets,
        missingFrames,
        ringStats.underrun_burst_events,
        ringStats.underruns,
        startupPadding,
        mixer.stats().adaptive_padding_frames,
        mixer.stats().live_tail_trimmed_frames,
        mixer.stats().adaptive_target_frames,
        profile.adaptive_playback_max_frames,
        mixer.stats().adaptive_delivery_gap_raise_events,
        mixer.stats().adaptive_delivery_gap_hold_active,
        maximumBufferedPath,
        bufferedPathSamples > 0
            ? static_cast<double>(bufferedPathSum /
                static_cast<long double>(bufferedPathSamples))
            : 0.0,
    };
}

void test_delivery_gap_learning_evidence_is_narrow()
{
    jam2::PeerStreamConfig config;
    config.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    config.sample_rate = 48000;
    config.frames_per_packet = 64;
    config.sample_time_playout = true;
    config.playback_queue_capacity_frames = 4096;
    config.jitter_buffer_frames = 0;
    config.jitter_buffer_max_frames = 0;
    config.drift_correction = false;
    config.stats_warmup_us = 60000000;
    config.collect_diagnostics = false;
    const std::vector<std::uint8_t> payload(128, 0);
    const auto receive = [&](jam2::PeerStream& stream,
                             std::uint32_t sequence,
                             std::uint64_t sampleTime,
                             std::uint64_t receiveTimeUs) {
        const jam2::protocol::Header header{
            jam2::protocol::PacketType::Audio,
            1,
            sequence,
            sampleTime,
            static_cast<std::uint16_t>(payload.size()),
            0,
        };
        return stream.receiveAudio(header, payload, receiveTimeUs) ==
            jam2::PeerAudioResult::Accepted;
    };

    DeliveryGapSink contiguousSink;
    jam2::PeerStream contiguous(config, 0, &contiguousSink);
    expect(receive(contiguous, 0, 0, 1000) &&
            receive(contiguous, 1, 64, 101000),
        "contiguous delivery-gap fixture accepts both packets");
    expect(contiguousSink.gapEvents == 1 &&
            contiguousSink.lastGap.required_playback_frames >= 4800,
        "a contiguous 100 ms delivery pause supplies measured adaptive evidence");

    DeliveryGapSink schedulerSink;
    jam2::PeerStream scheduler(config, 0, &schedulerSink);
    expect(receive(scheduler, 0, 0, 1000) &&
            receive(scheduler, 1, 64, 41000) &&
            schedulerSink.gapEvents == 0,
        "a 40 ms receive-thread batch is below the delivery-learning threshold");

    DeliveryGapSink missingSink;
    jam2::PeerStream missing(config, 0, &missingSink);
    expect(receive(missing, 0, 0, 1000) &&
            receive(missing, 1, 128, 101000),
        "missing-audio delivery-gap fixture accepts both packets");
    expect(missing.stats().missing_audio_frames_inserted == 64 &&
            missingSink.gapEvents == 0,
        "true missing audio is not misclassified as latency-correctable delivery jitter");
}

void test_midi_queue()
{
    jam2::midi::EventQueue queue;
    jam2::midi::Event source{1000, 0, 0x91, 60, 100, 3};
    expect(queue.push(source), "midi queue accepts event");
    jam2::midi::Event received;
    expect(queue.pop(received), "midi queue returns event");
    expect(received.status == 0x91 && received.data1 == 60 && received.data2 == 100,
        "midi queue preserves message and channel");
    expect(queue.depth() == 0, "midi queue depth returns to zero");
}

void test_midi_block_mapping()
{
    jam2::midi::EventQueue queue;
    queue.push({900, 0, 0x90, 60, 1, 3});
    queue.push({1500, 0, 0x90, 61, 2, 3});
    queue.push({2100, 0, 0x80, 60, 0, 3});
    jam2::midi::BlockBuilder builder;
    std::array<jam2::midi::Event, 8> events{};
    const auto first = builder.build(queue, 1000, 2000, 100, events);
    expect(first.count == 2, "midi block accepts late and in-range events");
    expect(first.late == 1 && events[0].sample_offset == 0,
        "late MIDI event is delivered at block start");
    expect(events[1].sample_offset == 50, "MIDI timestamp maps to sample offset");
    const auto second = builder.build(queue, 2000, 3000, 100, events);
    expect(second.count == 1 && events[0].sample_offset == 10,
        "deferred MIDI event reaches following block");
}

void test_mpe_validation()
{
    expect(jam2::midi::valid_mpe_zone({0, 1, 15, 48}), "lower MPE zone is valid");
    expect(jam2::midi::valid_mpe_zone({15, 0, 14, 48}), "upper MPE zone is valid");
    expect(!jam2::midi::valid_mpe_zone({0, 0, 15, 48}),
        "MPE master cannot be a member channel");
}

void test_source_router()
{
    std::array<std::int32_t, 4> left{100, 200, 300, 400};
    std::array<std::int32_t, 4> right{300, 400, 500, 600};
    std::array<const std::int32_t*, 2> inputs{left.data(), right.data()};
    std::array<std::int32_t, 4> output{};
    StereoRenderer renderer;
    jam2::audio::InputSourceRouter router(4, 2);
    expect(router.configure(0, {
        jam2::audio::InputSourceKind::Audio, 0, 1, 1000000, true, &renderer}),
        "stereo source configuration is accepted");
    expect(router.process(inputs, 4, 0, 48000.0, output),
        "source router produces canonical mono");
    expect(output == std::array<std::int32_t, 4>{200, 300, 400, 500},
        "stereo input is deterministically downmixed");
    expect(router.last_peak_ppm() == router.stats().peak_ppm,
        "router publishes its already-computed callback peak for meter reuse");
    expect(router.recording_latency_frames() == 23,
        "combined recording exposes active renderer latency");
    expect(router.configure(0, {
        jam2::audio::InputSourceKind::Audio, 0, 1, 1000000, true, nullptr}),
        "dry source replaces renderer");
    expect(router.process(inputs, 4, 4, 48000.0, output),
        "dry replacement continues routing");
    expect(router.recording_latency_frames() == 0,
        "removing renderer clears recording processing latency dynamically");
    expect(router.set_enabled(0, false) && router.set_level(0, 730000),
        "source routing controls update a configured slot");
    const jam2::audio::InputSourceSlotSnapshot routed = router.slot_snapshot(0);
    expect(routed.configured && !routed.enabled &&
            routed.kind == jam2::audio::InputSourceKind::Audio &&
            routed.first_channel == 0 && routed.second_channel == 1 &&
            routed.level_ppm == 730000 && !routed.renderer_attached,
        "source slot diagnostics expose exact live routing state");
    router.clear(0);
    const jam2::audio::InputSourceSlotSnapshot cleared = router.slot_snapshot(0);
    expect(!cleared.configured && !cleared.enabled &&
            cleared.first_channel == jam2::audio::kNoInputChannel &&
            cleared.second_channel == jam2::audio::kNoInputChannel,
        "source slot diagnostics expose cleared topology");
}

void test_instrument_failure_is_silence()
{
    FailedInstrument renderer;
    jam2::audio::InputSourceRouter router(4, 0);
    expect(router.configure(0, {
        jam2::audio::InputSourceKind::MidiInstrument,
        jam2::audio::kNoInputChannel,
        jam2::audio::kNoInputChannel,
        1000000,
        true,
        &renderer}), "MIDI instrument source configuration is accepted");
    std::array<std::int32_t, 4> output{1, 1, 1, 1};
    expect(router.process({}, 4, 0, 48000.0, output),
        "failed instrument remains a configured source");
    expect(output == std::array<std::int32_t, 4>{0, 0, 0, 0},
        "failed instrument renders silence");
    expect(router.stats().renderer_failures == 1,
        "renderer failure is exposed in stats");
}

void test_selected_recording_source_is_independent_of_send_mix()
{
    std::array<std::int32_t, 4> first{100, 200, 300, 400};
    std::array<std::int32_t, 4> second{900, 800, 700, 600};
    std::array<const std::int32_t*, 2> inputs{first.data(), second.data()};
    std::array<std::int32_t, 4> send{};
    std::array<std::int32_t, 4> recording{};
    jam2::audio::InputSourceRouter router(4, 2);
    expect(router.configure(0, {jam2::audio::InputSourceKind::Audio, 0,
        jam2::audio::kNoInputChannel, 1000000, true, nullptr}),
        "first recording source configures");
    expect(router.configure(1, {jam2::audio::InputSourceKind::Audio, 1,
        jam2::audio::kNoInputChannel, 1000000, false, nullptr}),
        "excluded recording source configures");
    router.set_recording_source(1);
    expect(router.process(inputs, 4, 0, 48000.0, send),
        "included source still produces My Send");
    expect(send == first, "excluded selected source does not enter My Send");
    expect(router.copy_recording_source(4, recording),
        "selected source is available to recorder");
    expect(recording == second,
        "recording can capture a source excluded from My Send");
}

void test_excluded_midi_source_keeps_draining()
{
    CountingInstrument renderer;
    jam2::audio::InputSourceRouter router(4, 0);
    expect(router.configure(0, {
        jam2::audio::InputSourceKind::MidiInstrument,
        jam2::audio::kNoInputChannel,
        jam2::audio::kNoInputChannel,
        1000000,
        false,
        &renderer}), "excluded MIDI instrument configures");
    std::array<std::int32_t, 4> output{1, 1, 1, 1};
    expect(!router.process({}, 4, 0, 48000.0, output),
        "excluded MIDI instrument stays outside My Send");
    expect(renderer.blocks == 1,
        "excluded MIDI instrument still drains its real-time event path");
    expect(output == std::array<std::int32_t, 4>{0, 0, 0, 0},
        "excluded MIDI instrument does not leak audio");
}

void measure_single_source_router_cost()
{
    constexpr std::size_t kFrames = 128;
    constexpr std::size_t kWarmupBlocks = 2000;
    constexpr std::size_t kMeasuredBlocks = 200000;
    std::array<std::int32_t, kFrames> input{};
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
        input[frame] = static_cast<std::int32_t>(frame * 100003U);
    }
    const std::array<const std::int32_t*, 1> inputs{input.data()};
    std::array<std::int32_t, kFrames> output{};
    jam2::audio::InputSourceRouter router(kFrames, inputs.size());
    expect(router.configure(0, {
        jam2::audio::InputSourceKind::Audio,
        0,
        jam2::audio::kNoInputChannel,
        1000000,
        true,
        nullptr}), "single-source router benchmark configures production path");

    std::uint64_t engineFrame = 0;
    for (std::size_t block = 0; block < kWarmupBlocks; ++block) {
        (void)router.process(inputs, kFrames, engineFrame, 48000.0, output);
        engineFrame += kFrames;
    }
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        (void)router.process(inputs, kFrames, engineFrame, 48000.0, output);
        engineFrame += kFrames;
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - started).count();
    expect(output == input,
        "single-source router benchmark preserves unity mono samples");
    std::cout << "METRIC input_router_single_mono_us_per_128="
              << elapsed / static_cast<double>(kMeasuredBlocks) << '\n';

    jam2::audio::StreamControl control;
    const auto callbackStarted = std::chrono::steady_clock::now();
    for (std::size_t block = 0; block < kMeasuredBlocks; ++block) {
        (void)router.process(inputs, kFrames, engineFrame, 48000.0, output);
        jam2::audio::device_processing::observe_input_peak_value(
            &control, router.last_peak_ppm());
        engineFrame += kFrames;
    }
    const auto callbackElapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - callbackStarted).count();
    std::cout << "METRIC input_router_with_peak_us_per_128="
              << callbackElapsed / static_cast<double>(kMeasuredBlocks) << '\n';
}

void test_playback_count_in_commits_without_control_toggle()
{
    jam2::Engine engine;
    jam2::EngineConfig config;
    config.backend = jam2::EngineAudioBackend::Headless;
    config.sample_rate = 48000;
    config.audio_buffer_frames = 64;
    config.metronome_enabled = false;
    config.metronome_pattern.bpm = 400;
    engine.start(config);

    const std::uint64_t readyDeadline = jam2::monotonic_us() + 30000000ULL;
    while (engine.snapshot().engine_frame < 1024 &&
           jam2::monotonic_us() < readyDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t now = engine.snapshot().engine_frame;
    jam2::EngineCommand transport;
    transport.type = jam2::EngineCommandType::ScheduleTransport;
    transport.transport_action = jam2::EngineTransportAction::TrackRestart;
    transport.transport_countdown_start_frame = now + 128ULL;
    transport.transport_target_frame = transport.transport_countdown_start_frame + 512ULL;
    transport.transport_musical_frame = transport.transport_target_frame;
    const bool submitted = engine.submit(transport);

    bool countInAudible = false;
    bool committed = false;
    jam2::EngineSnapshot finalSnapshot;
    const std::uint64_t commitDeadline = jam2::monotonic_us() + 30000000ULL;
    while (jam2::monotonic_us() < commitDeadline) {
        const jam2::EngineSnapshot snapshot = engine.snapshot();
        finalSnapshot = snapshot;
        countInAudible = countInAudible || snapshot.metronome_peak_ppm > 0;
        if (snapshot.transport_commit_count > 0 &&
            snapshot.transport_playback_active &&
            !snapshot.transport_pending) {
            committed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.requestStop();
    engine.join();
    if (!committed) {
        std::cerr << "count-in transport diagnostic: frame=" << finalSnapshot.engine_frame
                  << " target=" << finalSnapshot.transport_target_frame
                  << " pending=" << finalSnapshot.transport_pending
                  << " commits=" << finalSnapshot.transport_commit_count
                  << " active=" << finalSnapshot.transport_playback_active
                  << " action=" << static_cast<int>(finalSnapshot.transport_action)
                  << '\n';
    }
    expect(submitted, "playback count-in schedule is accepted");
    expect(countInAudible,
        "playback count-in is audible while the normal metronome is off");
    expect(committed,
        "playback count-in commits without another control command");
}

jam2::EngineSnapshot measure_headless_send_peak(int sendLevelPpm)
{
    jam2::Engine engine;
    jam2::EngineConfig config;
    config.backend = jam2::EngineAudioBackend::Headless;
    config.sample_rate = 48000;
    config.audio_buffer_frames = 64;
    config.test_input = jam2::EngineTestInput::Tone440;
    config.test_input_level_ppm = 200000;
    config.send_level_ppm = sendLevelPpm;
    expect(engine.networkPlaybackCallbackFrames() == 0, "unstarted engine exposes no callback quantum");
    engine.start(config);
    expect(engine.networkPlaybackCallbackFrames() == 64, "mixer sees the active headless device callback quantum");

    jam2::EngineSnapshot snapshot;
    const std::uint64_t deadline = jam2::monotonic_us() + 30000000ULL;
    while (jam2::monotonic_us() < deadline) {
        snapshot = engine.snapshot();
        if (snapshot.engine_frame >= 4096 && snapshot.input_peak_ppm > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.requestStop();
    engine.join();
    return snapshot;
}

void test_headless_send_peak_follows_gain()
{
    const jam2::EngineSnapshot half = measure_headless_send_peak(500000);
    expect(half.input_peak_ppm > 150000,
        "headless tone publishes its input diagnostic peak");
    expect(half.send_peak_ppm > 0 &&
        std::abs(half.send_peak_ppm * 2 - half.input_peak_ppm) <= 2,
        "send diagnostic peak follows the configured half gain");

    const jam2::EngineSnapshot muted = measure_headless_send_peak(0);
    expect(muted.input_peak_ppm > 150000,
        "muted send retains the local input diagnostic peak");
    expect(muted.send_peak_ppm == 0,
        "muted send publishes a zero send diagnostic peak");
}

void test_headless_injected_audio_uses_input_router()
{
    jam2::audio::InputSourceRouter router(8192, 2);
    expect(router.configure(0, {
        jam2::audio::InputSourceKind::Audio, 0,
        jam2::audio::kNoInputChannel, 1000000, true, nullptr}) &&
        router.configure(1, {
            jam2::audio::InputSourceKind::Audio, 1,
            jam2::audio::kNoInputChannel, 1000000, true, nullptr}),
        "headless router accepts two physical input sources");

    jam2::Engine engine;
    jam2::EngineConfig config;
    config.backend = jam2::EngineAudioBackend::Headless;
    config.sample_rate = 48000;
    config.audio_buffer_frames = 64;
    config.input_channels = jam2::audio::InputChannels::Stereo;
    config.channels.input = {0, 1};
    config.test_input = jam2::EngineTestInput::Tone440;
    config.test_input_level_ppm = 200000;
    config.input_source_router = &router;
    expect(engine.currentFrame() == 0,
        "lightweight engine frame query is zero before startup");
    engine.start(config);

    const auto waitForRouter = [&router](
        std::uint64_t minimumBlocks,
        bool expectSignal) {
        const std::uint64_t deadline = jam2::monotonic_us() + 30000000ULL;
        jam2::audio::InputSourceRouterStats stats;
        while (jam2::monotonic_us() < deadline) {
            stats = router.stats();
            if (stats.rendered_blocks >= minimumBlocks &&
                (expectSignal ? stats.peak_ppm > 0 : stats.peak_ppm == 0)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return stats;
    };

    const jam2::audio::InputSourceRouterStats active = waitForRouter(4, true);
    expect(active.rendered_blocks >= 4 && active.peak_ppm > 100000,
        "headless injected tone traverses the two-source router");
    expect(router.set_enabled(0, false) && router.set_enabled(1, false),
        "headless router sources can be excluded live");
    const jam2::audio::InputSourceRouterStats muted = waitForRouter(
        active.rendered_blocks + 4, false);
    expect(muted.rendered_blocks >= active.rendered_blocks + 4 &&
            muted.peak_ppm == 0,
        "excluding every source mutes headless injected audio through the router");
    expect(router.set_level(1, 500000) && router.set_enabled(1, true),
        "one headless source can resume at half level");
    const jam2::audio::InputSourceRouterStats resumed = waitForRouter(
        muted.rendered_blocks + 4, true);
    engine.requestStop();
    engine.join();
    expect(engine.currentFrame() > 0 &&
            engine.currentFrame() == engine.snapshot().engine_frame,
        "lightweight engine frame query matches the stopped diagnostic snapshot");
    expect(resumed.rendered_blocks >= muted.rendered_blocks + 4 &&
            resumed.peak_ppm > 40000 && resumed.peak_ppm <= 100000,
        "resumed headless source applies its exact router gain");
    expect(resumed.invalid_configurations == 0 &&
            resumed.renderer_failures == 0,
        "headless injected routing remains allocation-free and error-free");
}

void test_current_transport_packet_contract()
{
    constexpr std::uint64_t sessionId = 0x123456789abcdef0ULL;
    const std::array<std::uint8_t, 16> key{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::array<std::uint8_t, jam2::protocol::kTransportStatePayloadSize> payload{};
    payload[0] = 2;
    payload[1] = static_cast<std::uint8_t>(jam2::EngineTransportAction::TrackStop);
    const jam2::protocol::Header header{
        jam2::protocol::PacketType::TransportState,
        sessionId,
        7,
        48000,
        0,
        0,
    };
    const std::vector<std::uint8_t> packet = jam2::protocol::encode_packet(
        header, payload, key, jam2::NetworkAudioFormat::Pcm24Mono);
    const jam2::protocol::ParseResult parsed = jam2::protocol::parse_packet(
        packet, key, sessionId, jam2::NetworkAudioFormat::Pcm24Mono);
    expect(parsed.error == jam2::protocol::ParseError::None,
        "current 28-byte transport packet encodes and parses");
    expect(parsed.header.payload_length == payload.size(),
        "transport packet preserves the fixed payload size");
    expect(parsed.header.auth_tag == 0xaf2b7d1a445b9350ULL,
        "transport authentication retains the independent fixed SipHash vector");

    const std::array<std::uint8_t, 20> obsoletePayload{};
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> output{};
    expect(jam2::protocol::encode_packet_into(
        header,
        obsoletePayload,
        key,
        jam2::NetworkAudioFormat::Pcm24Mono,
        output) == 0,
        "obsolete 20-byte transport packet is rejected");
}

void test_peer_mixer_recovery_rebases_every_source()
{
    MixerSink sink;
    expect(sink.jam2::PeerStreamPlayback::underrunFrames() == 0,
        "playback sinks without device underrun telemetry report zero");
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.max_blocks_per_advance = 64;
    // This fixture isolates the hard-recovery rebase mechanics themselves.
    // Threshold policy is covered separately at the production 256 frames.
    config.receive_recovery_trigger_frames = 64;
    jam2::PeerMixer mixer(config, &sink);
    auto* first = mixer.addPeer(1, 512);
    auto* second = mixer.addPeer(2, 512);
    auto* recovering = mixer.addPeer(3, 512);
    expect(first != nullptr && second != nullptr && recovering != nullptr,
        "peer mixer creates three independent source queues");
    expect(mixer.setPeerActive(1, true) && mixer.setPeerActive(2, true) &&
        mixer.setPeerActive(3, true),
        "peer mixer activates all recovery sources");

    const std::array<std::int32_t, 64> block{};
    first->pushFrames(block);
    second->pushFrames(block);
    recovering->pushFrames(block);
    mixer.advance(0);

    const std::array<std::int32_t, 512> backlog{};
    first->pushFrames(backlog);
    second->pushFrames(backlog);
    mixer.advance(3000);
    const auto* firstBefore = mixer.peerStats(1);
    expect(firstBefore != nullptr && firstBefore->queue_depth_frames > 128,
        "healthy source has backlog while one peer misses a deadline");

    const std::array<std::int32_t, 192> catchup{};
    recovering->pushFrames(catchup);
    mixer.advance(3100);
    const auto* firstAfter = mixer.peerStats(1);
    const auto* secondAfter = mixer.peerStats(2);
    const auto* recoveredAfter = mixer.peerStats(3);
    expect(firstAfter != nullptr && secondAfter != nullptr && recoveredAfter != nullptr &&
        firstAfter->queue_depth_frames <= 128 &&
        secondAfter->queue_depth_frames <= 128 &&
        recoveredAfter->queue_depth_frames <= 128,
        "one peer recovery rebases every source to the bounded live tail");
    expect(mixer.stats().late_after_release_frames >= 640,
        "peer mixer exposes all recovery alignment drops in diagnostics");

    MixerSink treadmillSink;
    jam2::PeerMixer treadmill(config, &treadmillSink);
    auto* treadmillFirst = treadmill.addPeer(11, 512);
    auto* treadmillSecond = treadmill.addPeer(12, 512);
    auto* treadmillRecovering = treadmill.addPeer(13, 512);
    expect(treadmill.setPeerActive(11, true) && treadmill.setPeerActive(12, true) &&
        treadmill.setPeerActive(13, true),
        "peer mixer activates all partial-block recovery sources");
    treadmillFirst->pushFrames(block);
    treadmillSecond->pushFrames(block);
    treadmillRecovering->pushFrames(block);
    treadmill.advance(0);

    treadmillFirst->pushFrames(block);
    treadmillSecond->pushFrames(block);
    treadmill.advance(3000);
    std::array<std::int32_t, 65> partialRecovery{};
    treadmillFirst->pushFrames(block);
    treadmillSecond->pushFrames(block);
    treadmillRecovering->pushFrames(partialRecovery);
    treadmill.advance(4000);
    for (std::uint64_t cycle = 0; cycle < 20; ++cycle) {
        treadmillFirst->pushFrames(block);
        treadmillSecond->pushFrames(block);
        treadmillRecovering->pushFrames(block);
        treadmill.advance(5400 + cycle * 1400);
    }
    expect(treadmill.stats().deadline_slots == 1,
        "partial-block recovery does not re-enter a perpetual deadline-release loop");
    expect(treadmill.stats().complete_slots >= 20,
        "all sources resume complete mixed blocks after partial-block recovery");
}

void test_peer_mixer_global_gap_discards_obsolete_timeline()
{
    MarkerMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 1024;
    config.output_max_frames = 1536;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 256;
    config.adaptive_min_frames = 256;
    config.adaptive_max_frames = 1536;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(81, 16384);
    expect(peer != nullptr && mixer.setPeerActive(81, true),
        "live-gap mixer creates its active source");

    std::array<std::int32_t, 64> block{};
    block.fill(7);
    peer->pushFrames(block);
    mixer.advance(0);

    // Reproduce the 225 ms receive-thread stalls seen in the jam. The device
    // continues consuming while the peer source is empty, so the shared live
    // timeline must advance instead of preserving delayed audio for replay.
    for (std::uint64_t now = 1000; now <= 225000; now += 1000) {
        sink.consume(48);
        mixer.advance(now);
    }
    expect(mixer.stats().deadline_slots > 50 &&
            mixer.stats().missing_peer_frames > 3200,
        "global source gap advances the bounded live deadline");

    std::vector<std::int32_t> catchup(170U * 64U, 1111);
    std::fill(catchup.end() - 128, catchup.end(), 2222);
    peer->pushFrames(catchup);
    sink.consume(sink.depth);
    sink.samples.clear();
    mixer.advance(226000);

    const auto* recovered = mixer.peerStats(81);
    expect(mixer.stats().late_after_release_frames > 9000 &&
            mixer.stats().live_tail_trimmed_frames > 0,
        "global source recovery exposes discarded obsolete audio");
    expect(recovered != nullptr && recovered->queue_depth_frames <= 128,
        "global source recovery retains at most two current packet blocks");
    expect(std::find(sink.samples.begin(), sink.samples.end(), 1111) == sink.samples.end() &&
            std::find(sink.samples.begin(), sink.samples.end(), 2222) != sink.samples.end(),
        "global source recovery emits fresh markers without replaying stale markers");
}

void test_peer_mixer_recovery_waits_for_256_frame_debt()
{
    LiveTailMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 44100;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1024;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 64;
    config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    config.receive_recovery_trigger_frames = 256;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(91, 4096);
    expect(peer != nullptr && mixer.setPeerActive(91, true),
        "provisional recovery creates its active source");

    const std::array<std::int32_t, 64> packet{};
    peer->pushFrames(packet);
    mixer.advance(0);

    std::uint64_t now = 0;
    const auto missOnePacket = [&] {
        sink.consume(sink.depthFrames() + 64U);
        now += 3000;
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    };
    for (std::size_t packetIndex = 0; packetIndex < 3; ++packetIndex) {
        missOnePacket();
    }
    expect(mixer.stats().missing_peer_frames == 192 &&
            mixer.stats().receive_recovery_trigger_frames == 256,
        "three missing packets remain below the visible 256-frame hard-recovery trigger");

    std::vector<std::int32_t> delayedPrefix(192, 1111);
    peer->pushFrames(delayedPrefix);
    expect(mixer.stats().late_after_release_frames == 192 &&
            mixer.stats().receive_recovery_events == 0 &&
            mixer.stats().receive_recovery_completions == 0 &&
            !mixer.stats().receive_recovery_active &&
            mixer.stats().live_tail_trim_events == 0,
        "sub-threshold debt discards only its exact stale prefix without hard recovery or trimming");

    peer->pushFrames(packet);
    mixer.advance(now + 1);
    now += 1;
    for (std::size_t packetIndex = 0; packetIndex < 4; ++packetIndex) {
        missOnePacket();
    }
    expect(mixer.stats().receive_recovery_events == 1 &&
            mixer.stats().receive_recovery_active &&
            mixer.stats().receive_recovery_debt_frames >= 256,
        "four missing packets arm one hard recovery at the configured threshold");

    bool rejectedZeroTrigger = false;
    try {
        jam2::PeerMixerConfig invalid = config;
        invalid.receive_recovery_trigger_frames = 0;
        jam2::PeerMixer invalidMixer(invalid, &sink);
    } catch (const std::runtime_error&) {
        rejectedZeroTrigger = true;
    }
    expect(rejectedZeroTrigger,
        "receive recovery rejects a zero-frame trigger instead of silently restoring eager recovery");
}

void test_exactly_four_peer_multi_wake_catchup_returns_to_live_latency()
{
    LiveTailMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1024;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 64;
    config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);

    // One local listener plus these three remote contributors is the normal
    // exactly-four-peer full-mesh ownership seen by each participant.
    constexpr std::array<std::uint64_t, 3> peerIds{201, 202, 203};
    std::array<jam2::PeerStreamPlayback*, peerIds.size()> peers{};
    const std::array<std::int32_t, 64> initial{};
    for (std::size_t index = 0; index < peerIds.size(); ++index) {
        peers[index] = mixer.addPeer(peerIds[index], 16384);
        expect(peers[index] != nullptr && mixer.setPeerActive(peerIds[index], true),
            "multi-wake recovery activates every peer");
        peers[index]->pushFrames(initial);
    }
    mixer.advance(0);

    // Reproduce a 225 ms receive stall while the device clock continues. The
    // subsequent 230-packet catch-up is larger than three 64-datagram receive
    // budgets, just like the saturated wakes observed in the real jam.
    for (std::uint64_t now = 1000; now <= 225000; now += 1000) {
        sink.consume(48);
        mixer.advance(now);
    }
    expect(mixer.stats().deadline_slots > 100,
        "multi-wake recovery first advances the missing live timeline");

    constexpr std::array<std::size_t, 4> packetsPerWake{64, 64, 64, 38};
    std::vector<std::int32_t> batch;
    std::uint64_t now = 226000;
    std::size_t beforeFinalPeerDepth = 0;
    std::size_t beforeFinalOutputDepth = 0;
    std::size_t peakCatchupIngestDepth = 0;
    for (std::size_t wake = 0; wake < packetsPerWake.size(); ++wake) {
        batch.assign(packetsPerWake[wake] * 64U,
            static_cast<std::int32_t>(wake + 1));
        for (auto* peer : peers) {
            peer->pushFrames(batch);
        }
        std::size_t ingestDepth = sink.depthFrames();
        for (const std::uint64_t peerId : peerIds) {
            const auto* stats = mixer.peerStats(peerId);
            if (stats != nullptr) {
                ingestDepth += static_cast<std::size_t>(stats->queue_depth_frames);
            }
        }
        peakCatchupIngestDepth = std::max(peakCatchupIngestDepth, ingestDepth);
        mixer.advance(now++);
        if (wake + 1U == packetsPerWake.size()) {
            beforeFinalOutputDepth = sink.depthFrames();
            for (const std::uint64_t peerId : peerIds) {
                const auto* stats = mixer.peerStats(peerId);
                if (stats != nullptr) {
                    beforeFinalPeerDepth +=
                        static_cast<std::size_t>(stats->queue_depth_frames);
                }
            }
        }
        mixer.finishReceiveBatch(wake + 1U < packetsPerWake.size());
    }

    const std::array<std::int32_t, 64> stablePacket = [] {
        std::array<std::int32_t, 64> result{};
        result.fill(4);
        return result;
    }();
    for (std::size_t callback = 0; callback < 200; ++callback) {
        sink.consume(64);
        now += 1333;
        for (auto* peer : peers) {
            peer->pushFrames(stablePacket);
        }
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }

    std::size_t totalPeerDepth = 0;
    bool everyPeerAtLiveTail = true;
    for (const std::uint64_t peerId : peerIds) {
        const auto* stats = mixer.peerStats(peerId);
        everyPeerAtLiveTail = everyPeerAtLiveTail && stats != nullptr &&
            stats->queue_depth_frames <= 128;
        if (stats != nullptr) {
            totalPeerDepth += static_cast<std::size_t>(stats->queue_depth_frames);
        }
    }
    expect(everyPeerAtLiveTail,
        "every peer returns to the bounded two-packet live tail after a split catch-up burst");
    expect(sink.depthFrames() <= 192,
        "catch-up completion returns the device-facing ring within one callback of the live tail");
    expect(totalPeerDepth + sink.depthFrames() <= peerIds.size() * 128U + 128U,
        "exactly-four-peer recovery bounds the complete receiver path instead of retaining burst latency");
    expect(peakCatchupIngestDepth >
            peerIds.size() * 128U + 128U,
        "the multi-wake fixture contains measurable stale latency before final recovery");
    expect(mixer.stats().receive_recovery_events > 0 &&
            mixer.stats().receive_recovery_completions > 0 &&
            !mixer.stats().receive_recovery_active &&
            mixer.stats().receive_recovery_debt_frames == 0,
        "large-burst recovery reports a completed debt-driven recovery cycle");
    expect(!sink.samples.empty() && std::all_of(
            sink.samples.begin(), sink.samples.end(),
            [](std::int32_t sample) { return sample == 12; }),
        "exactly-four-peer recovery retains the newest mixed audio rather than stale catch-up samples");
    std::cout << "METRIC exactly_four_peer_catchup_peak_ingest_frames="
              << peakCatchupIngestDepth << '\n'
              << "METRIC exactly_four_peer_catchup_before_finalize_frames="
              << beforeFinalPeerDepth + beforeFinalOutputDepth << '\n'
              << "METRIC exactly_four_peer_catchup_after_finalize_frames="
              << totalPeerDepth + sink.depthFrames() << '\n';
}

void test_exactly_four_peer_trickled_catchup_stays_at_live_latency()
{
    LiveTailMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 44100;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1024;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 64;
    config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);

    constexpr std::array<std::uint64_t, 3> peerIds{301, 302, 303};
    std::array<jam2::PeerStreamPlayback*, peerIds.size()> peers{};
    const std::array<std::int32_t, 64> initial{};
    for (std::size_t index = 0; index < peerIds.size(); ++index) {
        peers[index] = mixer.addPeer(peerIds[index], 16384);
        expect(peers[index] != nullptr && mixer.setPeerActive(peerIds[index], true),
            "trickled recovery activates every remote contributor");
        peers[index]->pushFrames(initial);
    }
    mixer.advance(0);

    // Match the Wi-Fi jam's approximately 94 ms interruption while the local
    // device continues consuming one 64-frame callback every 1.451 ms.
    std::uint64_t now = 0;
    for (std::size_t callback = 0; callback < 65; ++callback) {
        sink.consume(64);
        now += 1451;
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }
    expect(mixer.stats().deadline_slots > 0,
        "trickled recovery first advances the missing 94 ms timeline");

    // Repay the missing timeline and deliver the trailing catch-up through
    // small, non-saturated wakes. The old 64-datagram completion rule treated
    // the first such wake after debt repayment as the end of recovery.
    constexpr std::array<std::size_t, 18> packetsPerWake{
        5, 7, 9, 6, 8, 5, 9, 7, 6, 8, 5, 9, 7, 6, 8, 5, 9, 7};
    std::vector<std::int32_t> batch;
    for (const std::size_t packets : packetsPerWake) {
        batch.assign(packets * 64U, 1111);
        for (auto* peer : peers) {
            peer->pushFrames(batch);
        }
        now += 250;
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }

    // Once packet pacing is normal again, recovery must remain at the live
    // edge and must not replay the preceding trickled catch-up prefix.
    const std::array<std::int32_t, 64> currentPacket = [] {
        std::array<std::int32_t, 64> result{};
        result.fill(2222);
        return result;
    }();
    std::size_t maximumStablePath = 0;
    for (std::size_t callback = 0; callback < 100; ++callback) {
        sink.consume(64);
        now += 1451;
        for (auto* peer : peers) {
            peer->pushFrames(currentPacket);
        }
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
        std::size_t currentPath = sink.depthFrames();
        for (const std::uint64_t peerId : peerIds) {
            const auto* stats = mixer.peerStats(peerId);
            if (stats != nullptr) {
                currentPath += static_cast<std::size_t>(stats->queue_depth_frames);
            }
        }
        maximumStablePath = std::max(maximumStablePath, currentPath);
    }

    std::size_t finalPeerDepth = 0;
    bool everyPeerAtLiveTail = true;
    for (const std::uint64_t peerId : peerIds) {
        const auto* stats = mixer.peerStats(peerId);
        if (stats != nullptr) {
            finalPeerDepth += static_cast<std::size_t>(stats->queue_depth_frames);
        }
        everyPeerAtLiveTail = everyPeerAtLiveTail && stats != nullptr &&
            stats->queue_depth_frames <= 128;
    }
    const std::size_t finalPath = finalPeerDepth + sink.depthFrames();
    const std::size_t livePathBound = peerIds.size() * 128U + 128U;
    expect(maximumStablePath <= livePathBound && finalPath <= livePathBound,
        "small-wake recovery reaches and retains the configured live-path bound");
    expect(everyPeerAtLiveTail && sink.depthFrames() <= 192,
        "small-wake recovery bounds every peer and the callback-quantized output tail");
    expect(!sink.samples.empty() && std::all_of(
            sink.samples.begin(), sink.samples.end(),
            [](std::int32_t sample) { return sample == 6666; }),
        "small-wake recovery emits only current mixed audio after pacing normalizes");
    expect(mixer.stats().receive_recovery_events > 0 &&
            mixer.stats().receive_recovery_completions > 0 &&
            !mixer.stats().receive_recovery_active &&
            mixer.stats().receive_recovery_debt_frames == 0,
        "small-wake recovery reports a completed debt-driven recovery cycle");
    std::cout << "METRIC exactly_four_peer_trickle_max_stable_frames="
              << maximumStablePath << '\n'
              << "METRIC exactly_four_peer_trickle_final_frames="
              << finalPath << '\n';
}

void test_exactly_four_peer_receive_burst_obeys_adaptive_path_target()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1024;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 128;
    config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);

    constexpr std::array<std::uint64_t, 3> peerIds{401, 402, 403};
    std::array<jam2::PeerStreamPlayback*, peerIds.size()> peers{};
    const std::array<std::int32_t, 64> initial{};
    for (std::size_t index = 0; index < peerIds.size(); ++index) {
        peers[index] = mixer.addPeer(peerIds[index], 16384);
        expect(peers[index] != nullptr && mixer.setPeerActive(peerIds[index], true),
            "burst-path fixture activates every remote contributor");
        peers[index]->pushFrames(initial);
    }
    mixer.advance(0);

    // The jam logs contained receive wakes of up to 38 packets (50.7 ms of
    // audio). This is delayed captured audio, not legitimate future audio, so
    // the adaptive target must bound the complete per-peer path rather than
    // allowing the device ring to grow toward its 1536-frame safety capacity.
    sink.consume(64);
    std::vector<std::int32_t> observedBurst(38U * 64U, 1234);
    for (auto* peer : peers) {
        peer->pushFrames(observedBurst);
    }
    std::uint64_t now = 1333;
    mixer.advance(now);
    mixer.finishReceiveBatch(false);

    auto currentPerPeerPath = [&]() {
        std::size_t maximumPeerQueue = 0;
        for (const std::uint64_t peerId : peerIds) {
            const auto* stats = mixer.peerStats(peerId);
            if (stats != nullptr) {
                maximumPeerQueue = std::max(
                    maximumPeerQueue,
                    static_cast<std::size_t>(stats->queue_depth_frames));
            }
        }
        return sink.depthFrames() + maximumPeerQueue;
    };

    std::size_t maximumStablePath = currentPerPeerPath();
    const std::array<std::int32_t, 64> pacedPacket{};
    for (std::size_t callback = 0; callback < 100; ++callback) {
        sink.consume(64);
        now += 1333;
        for (auto* peer : peers) {
            peer->pushFrames(pacedPacket);
        }
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
        maximumStablePath = std::max(maximumStablePath, currentPerPeerPath());
    }

    const std::size_t maximumLivePathLimit =
        config.adaptive_target_frames +
        static_cast<std::size_t>(config.frames_per_block);
    const std::size_t finalLivePathLimit =
        static_cast<std::size_t>(mixer.stats().adaptive_target_frames) +
        static_cast<std::size_t>(config.frames_per_block);
    expect(maximumStablePath <= maximumLivePathLimit &&
            currentPerPeerPath() <= finalLivePathLimit,
        "a 38-packet wake remains within the adaptive target plus one callback instead of filling the safety maximum");
    expect(mixer.stats().live_tail_trim_events > 0,
        "ordinary burst bounding reports the obsolete prefix through existing live-tail diagnostics");
    std::cout << "METRIC exactly_four_peer_burst_path_peak_frames="
              << maximumStablePath << '\n'
              << "METRIC exactly_four_peer_burst_path_final_frames="
              << currentPerPeerPath() << '\n'
              << "METRIC exactly_four_peer_burst_path_limit_frames="
              << maximumLivePathLimit << '\n'
              << "METRIC exactly_four_peer_burst_path_final_limit_frames="
              << finalLivePathLimit << '\n';
}

void test_exactly_four_peer_adaptive_off_uses_fixed_playout_target()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1024;
    config.max_blocks_per_advance = 64;
    config.fixed_target_frames = 128;
    config.adaptive_playback_cushion = false;
    config.adaptive_target_frames = 0;
    config.adaptive_min_frames = 0;
    config.adaptive_max_frames = 0;
    jam2::PeerMixer mixer(config, &sink);

    constexpr std::array<std::uint64_t, 3> peerIds{421, 422, 423};
    std::array<jam2::PeerStreamPlayback*, peerIds.size()> peers{};
    const std::array<std::int32_t, 64> packet{};
    for (std::size_t index = 0; index < peerIds.size(); ++index) {
        peers[index] = mixer.addPeer(peerIds[index], 16384);
        expect(peers[index] != nullptr && mixer.setPeerActive(peerIds[index], true),
            "fixed-target fixture activates every remote contributor");
        peers[index]->pushFrames(packet);
    }
    mixer.advance(0);
    for (auto* peer : peers) {
        peer->pushFrames(packet);
    }
    mixer.advance(1);
    sink.consume(64);

    std::vector<std::int32_t> observedBurst(38U * 64U, 4444);
    for (auto* peer : peers) {
        peer->pushFrames(observedBurst);
    }
    std::uint64_t now = 1333;
    mixer.advance(now);
    mixer.finishReceiveBatch(false);

    auto currentPerPeerPath = [&]() {
        std::size_t maximumPeerQueue = 0;
        for (const std::uint64_t peerId : peerIds) {
            const auto* stats = mixer.peerStats(peerId);
            if (stats != nullptr) {
                maximumPeerQueue = std::max(
                    maximumPeerQueue,
                    static_cast<std::size_t>(stats->queue_depth_frames));
            }
        }
        return sink.depthFrames() + maximumPeerQueue;
    };

    std::size_t maximumPath = currentPerPeerPath();
    for (std::size_t callback = 0; callback < 100; ++callback) {
        sink.consume(64);
        now += 1333;
        for (auto* peer : peers) {
            peer->pushFrames(packet);
        }
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
        maximumPath = std::max(maximumPath, currentPerPeerPath());
    }

    const std::size_t fixedPathLimit =
        config.fixed_target_frames +
        static_cast<std::size_t>(config.frames_per_block);
    expect(!mixer.stats().adaptive_playback_cushion_enabled &&
            mixer.stats().adaptive_target_frames == 0,
        "fixed target does not silently enable or invent an adaptive target");
    expect(maximumPath <= fixedPathLimit &&
            currentPerPeerPath() <= fixedPathLimit,
        "adaptive-off receive bursts remain near the explicit fixed playout floor instead of the safety maximum");

    // Hard recovery remains available in fixed mode. The same scheduler-shaped
    // three-packet batches must complete recovery without changing the fixed
    // target into a hidden adaptive value.
    for (std::size_t callback = 0; callback < 48; ++callback) {
        sink.consume(64);
        now += 1333;
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }
    expect(mixer.stats().receive_recovery_active,
        "fixed-target mode still enters hard recovery after a sustained outage");
    const std::size_t debtPackets = static_cast<std::size_t>(
        (mixer.stats().receive_recovery_debt_frames + 63U) / 64U);
    std::vector<std::int32_t> repayment((debtPackets + 3U) * 64U, 5555);
    for (auto* peer : peers) {
        peer->pushFrames(repayment);
    }
    now += 250;
    mixer.advance(now);
    mixer.finishReceiveBatch(false);

    std::vector<std::int32_t> scheduledBatch(3U * 64U, 6666);
    for (std::size_t callback = 0; callback < 200; ++callback) {
        sink.consume(64);
        now += 1333;
        if (callback % 3U == 0) {
            for (auto* peer : peers) {
                peer->pushFrames(scheduledBatch);
            }
        }
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }
    expect(!mixer.stats().receive_recovery_active &&
            mixer.stats().receive_recovery_events ==
                mixer.stats().receive_recovery_completions,
        "fixed-target hard recovery completes under average-rate receive batching");
    expect(currentPerPeerPath() <= fixedPathLimit,
        "fixed-target recovery returns the complete inbound path to the profile floor");
    std::cout << "METRIC exactly_four_peer_fixed_burst_path_peak_frames="
              << maximumPath << '\n'
              << "METRIC exactly_four_peer_fixed_burst_path_limit_frames="
              << fixedPathLimit << '\n'
              << "METRIC exactly_four_peer_fixed_recovery_events="
              << mixer.stats().receive_recovery_events << '\n'
              << "METRIC exactly_four_peer_fixed_recovery_completions="
              << mixer.stats().receive_recovery_completions << '\n';
}

void test_exactly_four_peer_recovery_completes_during_continuous_batching()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1024;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 64;
    config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    config.receive_recovery_trigger_frames = 256;
    jam2::PeerMixer mixer(config, &sink);

    constexpr std::array<std::uint64_t, 3> peerIds{411, 412, 413};
    std::array<jam2::PeerStreamPlayback*, peerIds.size()> peers{};
    const std::array<std::int32_t, 64> packet{};
    for (std::size_t index = 0; index < peerIds.size(); ++index) {
        peers[index] = mixer.addPeer(peerIds[index], 16384);
        expect(peers[index] != nullptr && mixer.setPeerActive(peerIds[index], true),
            "batched-recovery fixture activates every remote contributor");
        peers[index]->pushFrames(packet);
    }
    mixer.advance(0);

    std::uint64_t now = 0;
    std::uint64_t recoveryArmedAt = 0;
    // A 48-callback outage is 64 ms at this block size: long enough to model
    // the earlier high-debt recovery periods rather than only a short hiccup.
    for (std::size_t callback = 0; callback < 48; ++callback) {
        sink.consume(64);
        now += 1333;
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
        if (recoveryArmedAt == 0 && mixer.stats().receive_recovery_active) {
            recoveryArmedAt = now;
        }
    }
    const std::uint64_t recoveryPeakDebt =
        mixer.stats().receive_recovery_debt_max_frames;
    expect(recoveryArmedAt != 0 && mixer.stats().receive_recovery_active &&
            mixer.stats().receive_recovery_debt_frames >= 256,
        "batched-recovery fixture enters debt-driven recovery before catch-up arrives");

    const std::size_t debtPackets = static_cast<std::size_t>(
        (mixer.stats().receive_recovery_debt_frames + 63U) / 64U);
    std::vector<std::int32_t> repayment((debtPackets + 3U) * 64U, 2222);
    for (auto* peer : peers) {
        peer->pushFrames(repayment);
    }
    now += 250;
    mixer.advance(now);
    mixer.finishReceiveBatch(false);
    const std::uint64_t debtRepaidAt = now;

    // Three packets every three callbacks preserves the source's average
    // audio rate while reproducing the multi-packet receive batches in the
    // jam. A trimmed delayed prefix must not restart the same recovery's
    // stability deadline forever.
    std::vector<std::int32_t> threePacketBatch(3U * 64U, 3333);
    for (std::size_t callback = 0; callback < 200; ++callback) {
        sink.consume(64);
        now += 1333;
        if (callback % 3U == 0) {
            for (auto* peer : peers) {
                peer->pushFrames(threePacketBatch);
            }
        }
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }

    expect(!mixer.stats().receive_recovery_active &&
            mixer.stats().receive_recovery_events ==
                mixer.stats().receive_recovery_completions,
        "average-rate receive batching cannot keep one recovery active indefinitely");
    const std::uint64_t debtWindowUs =
        (recoveryPeakDebt * 1000000ULL +
            static_cast<std::uint64_t>(config.sample_rate) - 1ULL) /
        static_cast<std::uint64_t>(config.sample_rate);
    const std::uint64_t minimumWindowUs =
        (static_cast<std::uint64_t>(config.frames_per_block) * 4ULL * 1000000ULL +
            static_cast<std::uint64_t>(config.sample_rate) - 1ULL) /
        static_cast<std::uint64_t>(config.sample_rate);
    const std::uint64_t recoveryWindowUs =
        std::max(debtWindowUs, minimumWindowUs);
    const std::uint64_t schedulingAllowanceUs = 3ULL * 1333ULL;
    const std::uint64_t expectedMaximumDurationUs =
        debtRepaidAt - recoveryArmedAt + recoveryWindowUs + schedulingAllowanceUs;
    expect(mixer.stats().receive_recovery_duration_max_us <=
            expectedMaximumDurationUs,
        "long batched recovery is bounded by debt repayment, its measured recovery window, and scheduling granularity");
    std::cout << "METRIC exactly_four_peer_batched_recovery_events="
              << mixer.stats().receive_recovery_events << '\n'
              << "METRIC exactly_four_peer_batched_recovery_completions="
              << mixer.stats().receive_recovery_completions << '\n'
              << "METRIC exactly_four_peer_batched_recovery_active="
              << (mixer.stats().receive_recovery_active ? 1 : 0) << '\n'
              << "METRIC exactly_four_peer_batched_recovery_max_us="
              << mixer.stats().receive_recovery_duration_max_us << '\n'
              << "METRIC exactly_four_peer_batched_recovery_bound_us="
              << expectedMaximumDurationUs << '\n';
}

void test_exactly_four_peer_partial_blocks_preserve_audio_and_bounds()
{
    MarkerMixerSink sink;
    sink.callbackQuantum = 32;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 64;
    config.deadline_frames = 128;
    config.output_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 64;
    config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::array<std::int32_t, 64>, 3> audio{};
    for (std::size_t p = 0; p < peers.size(); ++p) {
        peers[p] = mixer.addPeer(p + 1, 1024);
        expect(peers[p] != nullptr && mixer.setPeerActive(p + 1, true),
            "partial-block fixture activates exactly three remote peers");
        if (!peers[p]) return;
        for (std::size_t f = 0; f < 64; ++f) audio[p][f] = static_cast<std::int32_t>(p * 100 + f + 1);
        peers[p]->pushFrames(std::span(audio[p]).first(63));
    }
    mixer.advance(0);
    expect(sink.samples.size() == 63 && sink.samples.front() == 303 && sink.samples.back() == 489 &&
            mixer.stats().partial_release_frames == 63 && mixer.stats().missing_peer_frames == 0 &&
            mixer.stats().adaptive_padding_frames == 0 && mixer.stats().receive_recovery_debt_frames == 0,
        "63 real common frames reach playback without a padded frame or timeline debt");
    sink.consume(32);
    for (std::size_t p = 0; p < peers.size(); ++p) peers[p]->pushFrames(std::span(audio[p]).last(1));
    mixer.advance(1);
    expect(sink.samples.size() == 64 && sink.samples.back() == 492 &&
            mixer.stats().partial_release_frames == 64 && mixer.stats().late_after_release_frames == 0,
        "the later 64th sample follows in order instead of being discarded as late");
    sink.depth = mixer.stats().normal_output_limit_frames;
    for (auto* peer : peers) peer->pushFrames(std::span(audio[0]).first(63));
    mixer.advance(2);
    expect(sink.depth == mixer.stats().normal_output_limit_frames && peers[0]->depthFrames() == 63,
        "partial tails stay upstream when the existing callback-ring limit is full");
    sink.consume(64);
    mixer.advance(3);
    for (std::uint64_t cycle = 0; cycle < 10000; ++cycle) {
        sink.consume(sink.depth);
        for (auto* peer : peers) peer->pushFrames(std::span(audio[0]).first(63));
        mixer.advance(4 + cycle * 1000);
        expect(sink.depth <= 127 && peers[0]->depthFrames() < 64,
            "repeated fractional blocks cannot grow the callback ring or source queues");
    }
    expect(mixer.stats().missing_peer_frames == 0 && mixer.stats().adaptive_padding_frames == 0 &&
            mixer.stats().late_after_release_frames == 0 && mixer.stats().live_tail_trimmed_frames == 0,
        "partial delivery preserves all real samples without accumulating silence, cuts or discard debt");
    sink.consume(sink.depth);
    mixer.advance(10000004);
    sink.consume(sink.depth);
    peers[0]->pushFrames(std::span(audio[0]).first(63));
    peers[1]->pushFrames(std::span(audio[1]).first(63));
    mixer.advance(10000005);
    expect(sink.depth == 0 && peers[0]->depthFrames() == 63,
        "one empty peer cannot cause early partial delivery or synthetic padding for other peers");
    for (std::size_t p = 0; p < peers.size(); ++p) mixer.setPeerActive(p + 1, false);
    const auto partialEvents = mixer.stats().partial_release_events;
    auto* replacement = mixer.addPeer(99, 1024);
    expect(replacement != nullptr && mixer.setPeerActive(99, true),
        "a fresh source can replace every previous contributor");
    if (!replacement) return;
    replacement->pushFrames(audio[0]);
    mixer.advance(10000006);
    expect(sink.depth == 64 && mixer.stats().partial_release_events == partialEvents &&
            mixer.stats().active_slots == 0 && mixer.stats().max_slots == 4 * 16,
        "a new source starts a fresh logical block and slot metrics retain full-block units");
}

void test_exactly_four_peer_partial_blocks_reach_real_callbacks()
{
    {
        ProfileMixerSink sink(512, 0);
        sink.callbackQuantum = 64;
        jam2::PeerMixerConfig config;
        config.frames_per_block = 64;
        config.deadline_frames = 128;
        config.output_max_frames = config.adaptive_max_frames = 512;
        config.adaptive_playback_cushion = true;
        config.adaptive_target_frames = config.adaptive_min_frames = 64;
        jam2::PeerMixer mixer(config, &sink);
        std::array<std::int32_t, 63> prefix{};
        for (std::uint64_t p = 1; p <= 3; ++p) {
            auto* peer = mixer.addPeer(p, 512);
            expect(peer != nullptr && mixer.setPeerActive(p, true),
                "one-frame-short callback fixture activates three remote sources");
            if (!peer) return;
            prefix.fill(static_cast<std::int32_t>(p));
            peer->pushFrames(prefix);
        }
        mixer.advance(0);
        sink.callback(64);
        expect(std::all_of(sink.callbackOutput.begin(), sink.callbackOutput.begin() + 63,
                   [](auto sample) { return sample == 6; }) && sink.callbackOutput[63] == 0 &&
                sink.underrunFrames() == 1 && mixer.stats().partial_release_frames == 63 &&
                mixer.stats().missing_peer_frames == 0 && mixer.stats().adaptive_padding_frames == 0,
            "a 64-frame callback renders all 63 real samples and pads only its one missing frame");
    }
    for (const std::size_t callback : {32U, 64U}) {
        ProfileMixerSink sink(512, 0);
        sink.callbackQuantum = callback;
        jam2::PeerMixerConfig config;
        config.frames_per_block = 64;
        config.deadline_frames = 128;
        config.output_max_frames = config.adaptive_max_frames = 512;
        config.adaptive_playback_cushion = true;
        config.adaptive_target_frames = config.adaptive_min_frames = 64;
        jam2::PeerMixer mixer(config, &sink);
        std::array<jam2::PeerStreamPlayback*, 3> peers{};
        for (std::size_t p = 0; p < peers.size(); ++p) {
            peers[p] = mixer.addPeer(p + 1, 512);
            expect(peers[p] != nullptr && mixer.setPeerActive(p + 1, true),
                "callback-boundary fixture activates exactly three remote sources");
            if (!peers[p]) return;
        }
        const std::array<std::int32_t, 1> preceding{999};
        sink.pushFrames(preceding);
        std::vector<std::int32_t> rendered;
        const auto renderReady = [&] {
            while (sink.depthFrames() >= callback) {
                sink.callback(callback);
                rendered.insert(rendered.end(), sink.callbackOutput.begin(),
                    sink.callbackOutput.begin() + callback);
            }
        };
        std::array<std::int32_t, 63> prefix{};
        for (std::size_t p = 0; p < peers.size(); ++p) {
            prefix.fill(static_cast<std::int32_t>(p + 1));
            peers[p]->pushFrames(prefix);
        }
        mixer.advance(0);
        renderReady();
        for (std::size_t p = 0; p < peers.size(); ++p) {
            const std::array<std::int32_t, 1> last{static_cast<std::int32_t>(p + 11)};
            peers[p]->pushFrames(last);
        }
        mixer.advance(1);
        for (std::size_t p = 0; p < peers.size(); ++p) {
            prefix.fill(static_cast<std::int32_t>(p + 21));
            peers[p]->pushFrames(prefix);
        }
        mixer.advance(2);
        renderReady();
        expect(rendered.size() == 128 && rendered[0] == 999 && rendered[63] == 6 &&
                rendered[64] == 36 && rendered[127] == 66 &&
                std::all_of(rendered.begin() + 1, rendered.begin() + 64,
                    [](auto sample) { return sample == 6; }) &&
                std::all_of(rendered.begin() + 65, rendered.end(),
                    [](auto sample) { return sample == 66; }),
            "32/64-frame production callbacks render both 63-frame prefixes and their later sample in order");
        expect(sink.underrunFrames() == 0 && mixer.stats().missing_peer_frames == 0 &&
                mixer.stats().adaptive_padding_frames == 0 && mixer.stats().late_after_release_frames == 0,
            "a one-frame real reserve and 63-frame prefix cover callbacks without silence or discard debt");
    }
}

void test_exactly_four_peer_worker_hold_uses_real_partial_runway()
{
    ProfileMixerSink sink(512, 0);
    sink.callbackQuantum = 32;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 64;
    config.deadline_frames = 128;
    config.output_max_frames = config.adaptive_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 64;
    jam2::PeerMixer mixer(config, &sink);
    std::array<std::int32_t, 160> preceding{};
    preceding.fill(9);
    sink.pushFrames(preceding);
    std::array<std::int32_t, 40> tail{};
    for (std::uint64_t p = 1; p <= 3; ++p) {
        auto* peer = mixer.addPeer(p, 512);
        expect(peer != nullptr && mixer.setPeerActive(p, true), "worker-hold fixture activates three remote peers");
        if (!peer) return;
        tail.fill(static_cast<std::int32_t>(p));
        peer->pushFrames(tail);
    }
    mixer.advance(0);
    expect(sink.depthFrames() == 200 && mixer.stats().partial_release_frames == 40 &&
            sink.depthFrames() <= mixer.stats().normal_output_limit_frames,
        "160 callback-facing frames and 40 common upstream frames share the existing output budget");
    // Four ms at 48 kHz consumes 192 frames without any network-worker wake.
    for (int callback = 0; callback < 6; ++callback) {
        sink.callback(32);
        expect(std::all_of(sink.callbackOutput.begin(), sink.callbackOutput.begin() + 32,
                [callback](auto sample) { return sample == (callback < 5 ? 9 : 6); }),
            "the scheduling hold renders only the existing real audio in order");
    }
    expect(sink.underrunFrames() == 0 && sink.depthFrames() == 8 &&
            mixer.stats().adaptive_padding_frames == 0 && mixer.stats().missing_peer_frames == 0,
        "a four-ms worker hold consumes the real partial tail without padding, underruns or buffer growth");
}

void test_exactly_four_peer_recovery_releases_real_partial_runway()
{
    // Recovery already has callback runway, but a worker hold needs the
    // remaining common prefix too. Keep recovery's low-water restart policy.
    constexpr std::size_t precedingFrames = 163;
    ProfileMixerSink sink(512, 0);
    sink.callbackQuantum = 32;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 64;
    config.deadline_frames = 512;
    config.output_max_frames = config.adaptive_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 64;
    jam2::PeerMixer mixer(config, &sink);
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::int32_t, 163> audio{};
    for (std::size_t p = 0; p < peers.size(); ++p) {
        peers[p] = mixer.addPeer(p + 1, 512);
        expect(peers[p] != nullptr && mixer.setPeerActive(p + 1, true),
            "recovery-prefix fixture activates exactly three remote peers");
        if (!peers[p]) return;
        peers[p]->pushFrames(std::span(audio).first(64));
    }
    mixer.advance(0);
    sink.callback(32);
    sink.callback(32);
    sink.callback(1);
    mixer.advance(1000);
    expect(mixer.stats().adaptive_target_frames == 65 && sink.underrunFrames() == 1,
        "a real callback shortage raises the production recovery target");
    audio.fill(9);
    sink.pushFrames(std::span(audio).first(precedingFrames));
    constexpr std::size_t common = 29;
    const std::array<std::size_t, 3> lengths{120, common, 101};
    for (std::size_t p = 0; p < peers.size(); ++p) {
        audio.fill(static_cast<std::int32_t>(p + 1));
        peers[p]->pushFrames(std::span(audio).first(lengths[p]));
    }
    mixer.advance(1001);
    expect(sink.depthFrames() == precedingFrames + common &&
            mixer.stats().partial_release_frames == common &&
            sink.depthFrames() <= mixer.stats().normal_output_limit_frames &&
            peers[1]->depthFrames() == 0,
        "raised recovery transfers every common real frame within the existing output cap");
    for (int callback = 0; callback < 6; ++callback) {
        sink.callback(32);
        const auto preceding = callback < 5 ? 32 : 3;
        expect(std::all_of(sink.callbackOutput.begin(), sink.callbackOutput.begin() + preceding,
                   [](auto sample) { return sample == 9; }) &&
                std::all_of(sink.callbackOutput.begin() + preceding, sink.callbackOutput.begin() + 32,
                   [](auto sample) { return sample == 6; }),
            "recovery renders 163 preceding frames followed by the 29-frame mixed prefix in order");
    }
    expect(sink.underrunFrames() == 1 && sink.depthFrames() == 0,
        "recovery's 29 real frames prevent another underrun across the four-ms worker hold");
    expect(mixer.stats().adaptive_padding_frames == 0 && mixer.stats().missing_peer_frames == 0 &&
            mixer.stats().late_after_release_frames == 0 && mixer.stats().receive_recovery_debt_frames == 0,
        "partial recovery adds no padding, missing contributions, discarded samples or timeline debt");
}

void test_four_peer_boundary_hold_preserves_phase_and_bounds()
{
    constexpr std::size_t inputFrames = 64 * 4096;
    for (const double ppm : {-500., -100., -25., -10., -1., -0.027, 0., 0.027, 1., 10., 25., 100., 500.}) {
        MarkerMixerSink sink;
        sink.callbackQuantum = 64;
        jam2::PeerMixerConfig config;
        config.sample_rate = 48000; config.frames_per_block = 64; config.deadline_frames = 128;
        config.output_max_frames = 1024; config.adaptive_playback_cushion = true;
        config.adaptive_target_frames = config.adaptive_min_frames = 64;
        config.adaptive_max_frames = 512;
        jam2::PeerMixer mixer(config, &sink);
        std::array<jam2::PeerStreamPlayback*, 3> sources{};
        const double ratio = 1.0 + ppm / 1000000.0;
        for (std::size_t p = 0; p < sources.size(); ++p) {
            sources[p] = mixer.addPeer(p + 1, 4096);
            expect(sources[p] && mixer.setPeerActive(p + 1, true), "boundary hold activates exactly four participants");
            if (!sources[p]) return;
            sources[p]->setResamplerRatio(ratio);
        }
        std::array<std::int32_t, 64> packet{};
        std::uint64_t peak = 0;
        for (std::size_t begin = 0; begin < inputFrames; begin += 64) {
            for (std::size_t f = 0; f < 64; ++f) packet[f] = static_cast<std::int32_t>((begin + f) * 16);
            for (auto* source : sources) source->pushFrames(packet);
            mixer.advance(1 + begin * 1000000ULL / 48000);
            peak = std::max<std::uint64_t>(peak, sink.depth + sources[0]->depthFrames());
            sink.consume(sink.depth);
        }
        mixer.advance(1 + inputFrames * 1000000ULL / 48000);
        const auto rendered = sink.samples.size();
        const auto held = mixer.stats().lookahead_hold_frames;
        for (std::uint64_t wake = 1; wake <= 100; ++wake) {
            sink.consume(sink.depth);
            mixer.advance(1 + inputFrames * 1000000ULL / 48000 + wake * 1000);
        }
        expect(sink.samples.size() == rendered && mixer.stats().lookahead_hold_frames == held,
            "a missing packet cannot generate unlimited held audio or additional padding");
        bool continuous = true;
        for (std::size_t i = 0; i < rendered; ++i)
            continuous &= std::abs(static_cast<double>(sink.samples[i]) - i * ratio * 48.0) <= 52.0;
        const auto exact = static_cast<std::size_t>(std::floor((inputFrames - 1) / ratio)) + 1;
        expect(continuous && rendered >= exact && rendered <= exact + 2,
            "held output follows the uninterrupted fractional timeline and genuine clock correction");
        expect(peak <= 128 && mixer.stats().adaptive_padding_frames == 0 &&
                mixer.stats().live_tail_trimmed_frames == 0 && mixer.stats().capacity_dropped_frames == 0 &&
                held <= 2 * mixer.stats().lookahead_hold_events &&
                mixer.stats().lookahead_hold_max_fraction_ppm <= 1000000,
            "boundary holds stay within one source interval and the existing queue bounds");
        for (std::size_t p = 0; p < sources.size(); ++p) {
            expect(mixer.peerStats(p + 1)->resampled_output_frames == rendered + sources[p]->depthFrames(),
                "every real or approximated source output remains explicitly accounted");
            mixer.setPeerActive(p + 1, false);
        }
        mixer.advance(7000000);
        expect(mixer.stats().lookahead_hold_frames == held && mixer.stats().contributing_peers == 0,
            "deactivation clears interpolation state without reviving held audio");
    }
}

void test_isolated_recovery_escalates_repeated_shortages()
{
    for (const std::size_t missing : {1U, 32U}) {
        MarkerMixerSink sink;
        sink.callbackQuantum = 32;
        jam2::PeerMixerConfig config;
        config.frames_per_block = 64; config.deadline_frames = 1024;
        config.output_max_frames = config.adaptive_max_frames = 512;
        config.adaptive_playback_cushion = true;
        config.adaptive_target_frames = config.adaptive_min_frames = 64;
        jam2::PeerMixer mixer(config, &sink);
        auto* peer = mixer.addPeer(1, 1024);
        expect(peer && mixer.setPeerActive(1, true), "isolated recovery source activates");
        if (!peer) return;
        std::array<std::int32_t, 64> audio{};
        audio.fill(42);
        peer->pushFrames(audio); mixer.advance(1);
        sink.consume(sink.depth + missing); mixer.advance(1000);
        peer->pushFrames(audio); mixer.advance(1000); mixer.finishReceiveBatch(false);
        const auto increase = missing == 1 ? 1U : 64U;
        expect(mixer.stats().adaptive_target_frames == 64 + increase &&
                mixer.stats().adaptive_padding_frames == increase &&
                mixer.stats().isolated_recovery_events == (missing == 1 ? 1U : 0U),
            "only an isolated one-frame shortage receives one-frame recovery");
        const auto padding = mixer.stats().adaptive_padding_frames;
        for (int i = 0; i < 100; ++i) mixer.finishReceiveBatch(false);
        expect(mixer.stats().adaptive_padding_frames == padding, "idle batches cannot grow isolated recovery buffers");
        sink.consume(sink.depth + 1); mixer.advance(1500);
        expect(mixer.stats().adaptive_target_frames == 128 + increase,
            "a repeated shortage retains full packet-sized escalation");
        mixer.setPeerActive(1, false); mixer.finishReceiveBatch(false);
        expect(mixer.stats().adaptive_padding_frames == padding, "deactivation cancels pending recovery silence");
    }
    MarkerMixerSink fixed_sink;
    fixed_sink.callbackQuantum = 32;
    jam2::PeerMixerConfig fixed;
    fixed.frames_per_block = 64; fixed.deadline_frames = 128; fixed.output_max_frames = 512;
    jam2::PeerMixer fixed_mixer(fixed, &fixed_sink);
    auto* fixed_peer = fixed_mixer.addPeer(1, 1024);
    expect(fixed_peer && fixed_mixer.setPeerActive(1, true), "fixed playback source activates");
    if (!fixed_peer) return;
    const std::array<std::int32_t, 64> audio{};
    fixed_peer->pushFrames(audio); fixed_mixer.advance(1);
    fixed_sink.consume(fixed_sink.depth + 1); fixed_mixer.advance(1000); fixed_mixer.finishReceiveBatch(false);
    expect(fixed_mixer.stats().isolated_recovery_events == 0 && fixed_mixer.stats().adaptive_padding_frames == 0,
        "disabled adaptive recovery cannot add runway or report a recovery it did not perform");
}

void test_four_peer_worker_margin_tracks_service_gaps()
{
    MarkerMixerSink sink;
    sink.callbackQuantum = 64;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000; config.frames_per_block = 64; config.deadline_frames = 128;
    config.output_max_frames = 1024; config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 64; config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);
    std::array<std::int32_t, 64> audio{};
    std::array<jam2::PeerStreamPlayback*, 3> sources{};
    for (std::size_t p = 1; p <= 3; ++p) {
        auto* source = mixer.addPeer(p, 1024);
        expect(source && mixer.setPeerActive(p, true), "worker-margin fixture activates exactly four participants");
        if (!source) return;
        sources[p - 1] = source;
        source->setResamplerRatio(1.000001);
        source->pushFrames(audio);
    }
    mixer.advance(1); mixer.advance(1001);
    sink.consume(sink.depth);
    for (auto* source : sources) source->pushFrames(audio);
    const auto holds_before_stall = mixer.stats().lookahead_hold_frames;
    mixer.advance(5001);
    expect(mixer.stats().worker_service_gap_us == 4000 && mixer.stats().worker_runway_frames == 192 &&
            mixer.stats().release_worker_guard_active && mixer.stats().adaptive_release_alignment_margin_frames == 64,
        "four-ms worker stalls retain the original independent-source alignment protection");
    expect(mixer.stats().lookahead_hold_frames == holds_before_stall,
        "worker stalls preserve exact interpolation instead of approximating queued source boundaries");
    for (std::uint64_t now = 6001; now <= 2100001; now += 1000) mixer.advance(now);
    expect(mixer.stats().worker_runway_frames == 64 && !mixer.stats().release_worker_guard_active &&
            mixer.stats().adaptive_release_alignment_margin_frames == 0,
        "clean worker service expires the stall allowance without permanently retaining a packet");
}

void test_recovery_padding_waits_for_receive_batch()
{
    MarkerMixerSink sink;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 64;
    config.deadline_frames = 1024;
    config.output_max_frames = config.adaptive_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 64;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(1, 1024);
    expect(peer != nullptr && mixer.setPeerActive(1, true), "batch-padding fixture activates source");
    if (!peer) return;
    std::array<std::int32_t, 64> audio{};
    audio.fill(42);
    peer->pushFrames(audio);
    mixer.advance(0);
    sink.consume(96);
    mixer.advance(1000);
    expect(sink.depth == 0 && mixer.stats().adaptive_padding_frames == 0,
        "pre-receive maintenance records starvation without inserting silence ahead of ready packets");
    peer->pushFrames(audio);
    mixer.advance(1000);
    mixer.finishReceiveBatch(false);
    expect(sink.depth == 128 && mixer.stats().adaptive_padding_frames == 64,
        "real audio arriving in the same wake precedes the preserved bounded recovery runway");
    expect(sink.samples.size() == 192 && sink.samples[64] == 42 &&
            sink.samples[127] == 42 && sink.samples[128] == 0 && sink.samples.back() == 0,
        "the real received block is rendered before the deferred recovery silence");
    sink.consume(129);
    mixer.advance(2000);
    mixer.finishReceiveBatch(true);
    expect(sink.depth == 0 && mixer.stats().adaptive_padding_frames == 64,
        "a saturated receive batch defers padding until queued datagrams are drained");
    mixer.advance(2001);
    mixer.finishReceiveBatch(false);
    const auto padded = mixer.stats().adaptive_padding_frames;
    expect(padded > 0 && sink.depth <= config.adaptive_max_frames,
        "genuine starvation still permits bounded recovery silence after an empty receive batch");
    mixer.finishReceiveBatch(false);
    expect(mixer.stats().adaptive_padding_frames == padded,
        "finishing an idle batch repeatedly cannot grow the recovery buffer");
    sink.consume(sink.depth + 1);
    mixer.advance(3000);
    expect(mixer.setPeerActive(1, false), "padding fixture removes its final contributing source");
    mixer.finishReceiveBatch(false);
    expect(mixer.stats().adaptive_padding_frames == padded,
        "deactivation cancels pending padding instead of reviving a stopped output");
}

void test_peer_mixer_batches_wrapped_queue_operations()
{
    MarkerMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.max_blocks_per_advance = 16;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(82, 512);
    expect(peer != nullptr && mixer.setPeerActive(82, true),
        "batched mixer creates its bounded source queue");

    std::vector<std::int32_t> burst(640);
    for (std::size_t frame = 0; frame < burst.size(); ++frame) {
        burst[frame] = static_cast<std::int32_t>(frame);
    }
    expect(peer->pushFrames(burst) == burst.size(),
        "batched enqueue accepts every current frame in an oversized burst");
    const auto* queued = mixer.peerStats(82);
    expect(queued != nullptr && queued->queue_capacity_drops == 128 &&
            queued->queue_capacity_dropped_frames == 128 &&
            queued->queue_depth_frames == 512,
        "batched enqueue reports the exact obsolete prefix discarded at capacity");

    mixer.advance(0);
    expect(sink.samples.size() == 512 && sink.samples.front() == 128 &&
            sink.samples.back() == 639,
        "batched wrapped dequeue preserves the newest source frames in order");
    expect(mixer.stats().capacity_drops == 128 &&
            mixer.stats().capacity_dropped_frames == 128,
        "batched capacity accounting remains visible at mixer ownership");
}

void test_peer_mixer_adapts_to_device_ring_underrun()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.max_blocks_per_advance = 8;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 256;
    config.adaptive_min_frames = 256;
    config.adaptive_max_frames = 512;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(91, 512);
    expect(peer != nullptr && mixer.setPeerActive(91, true),
        "adaptive mixer creates its active source");
    const std::array<std::int32_t, 64> block{};
    peer->pushFrames(block);
    mixer.advance(0);

    sink.underruns += 64;
    peer->pushFrames(block);
    mixer.advance(1400);
    expect(mixer.stats().adaptive_target_frames == 320 &&
            mixer.stats().adaptive_raise_events == 1,
        "device-facing ring underrun raises the bounded adaptive cushion");
}

void test_peer_mixer_burst_bound_preserves_adaptive_recovery()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 1024;
    config.output_max_frames = 1536;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 256;
    config.adaptive_min_frames = 256;
    config.adaptive_max_frames = 1536;
    config.adaptive_release_ppm = 5000;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(101, 4096);
    expect(peer != nullptr && mixer.setPeerActive(101, true),
        "adaptive-release mixer creates its active source");

    const std::array<std::int32_t, 64> block{};
    peer->pushFrames(block);
    mixer.advance(0);

    // A real underrun may raise the live target. A following delayed burst
    // must fill that target, not the much larger safety capacity and then rely
    // on several seconds of accelerated playback to remove the mistake.
    sink.underruns += 64;
    peer->pushFrames(block);
    mixer.advance(1000);
    expect(mixer.stats().adaptive_target_frames == 320,
        "an underrun establishes recovery cushion before bounded release");

    const std::array<std::int32_t, 2048> burst{};
    peer->pushFrames(burst);
    mixer.advance(2000);
    expect(sink.depth <= mixer.stats().adaptive_target_frames &&
            sink.resamplerRatio >= 1.0 && sink.resamplerRatio <= 1.005 &&
            mixer.stats().live_tail_trim_events > 0,
        "a delayed burst stays bounded while the raised cushion may retire smoothly");

    const std::uint64_t paddingBeforeRelease =
        mixer.stats().adaptive_padding_frames;
    sink.consume(64);
    peer->pushFrames(block);
    mixer.advance(3000);
    expect(mixer.stats().adaptive_padding_frames == paddingBeforeRelease,
        "a normally paced real block is not followed by synthetic cushion silence");
    expect(sink.resamplerRatio >= 1.0 && sink.resamplerRatio <= 1.005,
        "raised cushion release stays inside the configured rate limit");

    sink.consume(sink.depth + 64);
    mixer.advance(5000);
    expect(mixer.stats().adaptive_target_frames == 384 &&
            mixer.stats().adaptive_raise_events == 2,
        "an actual device underrun still raises the adaptive target");
    expect(mixer.stats().adaptive_padding_frames == paddingBeforeRelease,
        "an actual device underrun uses the retained real source before adding silence");
    sink.consume(sink.depth + 64);
    mixer.advance(6000);
    mixer.finishReceiveBatch(false);
    expect(mixer.stats().adaptive_padding_frames > paddingBeforeRelease,
        "an actual device underrun with no remaining source permits bounded recovery padding");
}

void test_peer_mixer_periodic_batches_do_not_accelerate_into_underruns()
{
    ResampledMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 1024;
    config.output_max_frames = 1536;
    config.max_blocks_per_advance = 64;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = 256;
    config.adaptive_min_frames = 256;
    config.adaptive_max_frames = 1536;
    config.adaptive_release_ppm = 5000;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(102, 8192);
    expect(peer != nullptr && mixer.setPeerActive(102, true),
        "burst-cycle mixer creates its active source");

    std::array<std::int32_t, 38U * 64U> catchupBurst{};
    std::fill(catchupBurst.begin(), catchupBurst.end(), 100000000);
    std::uint64_t nowUs = 1000000;
    peer->pushFrames(catchupBurst);
    mixer.advance(nowUs);
    const std::uint64_t startupPadding = mixer.stats().adaptive_padding_frames;
    bool targetStayedAtUnity =
        sink.control.playback_ratio_ppm.load(std::memory_order_relaxed) == 1000000;
    const std::array<std::int32_t, 3U * 64U> scheduledBatch{};
    for (std::uint64_t cycle = 0; cycle < 32; ++cycle) {
        // Three packets every six 32-frame device callbacks is the same
        // average source rate with realistic receive-thread scheduling
        // batching. It must use the target as runway without rebuilding the
        // discarded catch-up latency.
        for (int callback = 0; callback < 6; ++callback) {
            sink.callback();
            nowUs += 667;
            if (callback == 5) {
                peer->pushFrames(scheduledBatch);
            }
            mixer.advance(nowUs);
            targetStayedAtUnity = targetStayedAtUnity &&
                sink.control.playback_ratio_ppm.load(std::memory_order_relaxed) == 1000000;
        }
    }
    expect(targetStayedAtUnity,
        "minimum adaptive target treats periodic packet batching as playback runway");
    expect(sink.ring.stats().underrun_burst_events == 0,
        "periodic packet batches do not accelerate the real playback resampler into underruns");
    expect(mixer.stats().adaptive_padding_frames == startupPadding,
        "periodic packet batches do not trigger post-startup silence padding");
}

void test_adaptive_release_retires_real_surplus_without_trimming()
{
    for (const std::size_t peerCount : {1U, 3U}) {
    ProfileMixerSink sink(4096, 256);
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 1024;
    config.output_max_frames = 1536;
    config.adaptive_playback_cushion = true;
    config.adaptive_min_frames = 256;
    config.adaptive_target_frames = 1024;
    config.adaptive_max_frames = 1536;
    jam2::PeerMixer mixer(config, &sink);
    auto* peer = mixer.addPeer(1, 4096);
    expect(peer != nullptr && mixer.setPeerActive(1, true), "surplus release activates source");
    if (!peer) return;
    std::vector<jam2::PeerStreamPlayback*> additional;
    for (std::size_t id = 2; id <= peerCount; ++id) {
        additional.push_back(mixer.addPeer(id, 4096));
        expect(additional.back() != nullptr && mixer.setPeerActive(id, true), "surplus fixture activates all four participants");
        if (!additional.back()) return;
    }
    std::array<std::int32_t, 64> block;
    block.fill(100000000);
    peer->pushFrames(block);
    for (auto* other : additional) other->pushFrames(block);
    mixer.advance(1);
    const auto padding = mixer.stats().adaptive_padding_frames;
    bool accelerated = false;
    for (std::uint64_t callback = 1; callback <= 9000; ++callback) {
        const auto now = 1 + callback * 64000000ULL / 48000;
        sink.callback();
        mixer.advance(now);
        peer->pushFrames(block);
        for (auto* other : additional) other->pushFrames(block);
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
        accelerated |= sink.control.playback_ratio_ppm.load(std::memory_order_relaxed) > 1000000;
    }
    std::cout << "METRIC surplus_release target=" << mixer.stats().adaptive_target_frames
              << " trimmed=" << mixer.stats().live_tail_trimmed_frames
              << " underruns=" << sink.underrunFrames()
              << " path=" << sink.depthFrames() + peer->depthFrames()
              << " accelerated=" << accelerated << '\n';
    expect(accelerated && mixer.stats().adaptive_target_frames == config.adaptive_min_frames,
        "raised real cushion retires smoothly to its minimum within a bounded time");
    expect(mixer.stats().live_tail_trimmed_frames == 0 && sink.underrunFrames() == 0 &&
            mixer.stats().adaptive_padding_frames == padding,
        "target release does not turn real audio into trims, underruns or new padding");
    expect(sink.depthFrames() + peer->depthFrames() <= config.adaptive_min_frames + 64,
        "smooth release leaves no unbounded latency tail");
    }
}

void test_source_rounding_edge_is_fixed_and_bounded()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 256;
    config.sample_rate = 48000;
    config.output_max_frames = 2048;
    config.adaptive_playback_cushion = true;
    config.adaptive_min_frames = config.adaptive_target_frames = 1024;
    config.adaptive_max_frames = 2048;
    jam2::PeerMixer mixer(config, &sink);
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::int32_t, 4096> audio{};
    for (std::size_t i = 0; i < peers.size(); ++i) {
        peers[i] = mixer.addPeer(i + 1, 8192);
        expect(peers[i] && mixer.setPeerActive(i + 1, true), "edge fixture activates four participants");
        if (!peers[i]) return;
        peers[i]->pushFrames(std::span(audio).first(256));
    }
    mixer.advance(1);
    expect(sink.depth == 1024 && mixer.stats().live_tail_rounding_tolerance_frames == 96,
        "source edge is the explicit fixed 2 ms allowance, not another output prefill");
    peers[0]->pushFrames(std::span(audio).first(352));
    mixer.advance(2);
    expect(mixer.stats().live_tail_trimmed_frames == 0 && peers[0]->depthFrames() == 352,
        "one block plus the complete small edge is retained without a sample cut");
    for (std::uint64_t step = 3; step < 103; ++step) {
        peers[0]->pushFrames(std::span(audio).first(1));
        mixer.advance(step);
        expect(peers[0]->depthFrames() == 352 && mixer.stats().adaptive_target_frames == 1024,
            "repeated excess cannot ratchet the allowance or target upward");
    }
    expect(mixer.stats().live_tail_trimmed_frames == 100 && mixer.stats().normal_path_limit_frames == 1376,
        "only frames beyond the fixed edge are cut and the published path cap includes it");
    peers[0]->pushFrames(audio);
    mixer.advance(103);
    expect(peers[0]->depthFrames() == 256 && sink.depth == 1024,
        "a real backlog still sheds its obsolete prefix to the original live tail");
    config.live_tail_rounding_tolerance_us = 10001;
    bool rejected = false;
    try { jam2::PeerMixer invalid(config, &sink); } catch (const std::exception&) { rejected = true; }
    expect(rejected, "oversized source rounding allowance is rejected");
    config.live_tail_rounding_tolerance_us = 0;
    jam2::PeerMixer strict(config, &sink);
    expect(strict.stats().live_tail_rounding_tolerance_frames == 0 && strict.stats().normal_path_limit_frames == 1280,
        "explicit zero disables the source edge without changing the output target");
}

void test_four_peer_rebase_preserves_unexpired_delivery_learning()
{
    MixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 128;
    config.output_max_frames = 2048;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 256;
    config.adaptive_max_frames = 2048;
    jam2::PeerMixer mixer(config, &sink);
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::int32_t, 64> block{};
    for (std::size_t i = 0; i < peers.size(); ++i) {
        peers[i] = mixer.addPeer(i + 1, 4096);
        expect(peers[i] && mixer.setPeerActive(i + 1, true), "learning/rebase activates exactly four peers");
        if (!peers[i]) return;
        peers[i]->pushFrames(block);
    }
    mixer.advance(1000);
    sink.consume(sink.depth + 64);
    peers[0]->observeDeliveryGap({2000, 1024});
    for (auto* peer : peers) peer->pushFrames(block);
    mixer.advance(2000);
    expect(mixer.stats().adaptive_delivery_gap_hold_active && mixer.stats().adaptive_target_frames == 1024,
        "a real underrun associates with measured gap evidence before the later interruption");
    for (std::uint64_t now = 20000; now <= 36000; now += 2000) {
        sink.consume(sink.depth + 64);
        mixer.advance(now);
    }
    expect(mixer.stats().receive_recovery_active && mixer.stats().receive_recovery_debt_frames >= 256,
        "a separate interruption arms debt recovery");
    const std::array<std::int32_t, 2048> repayment{};
    for (auto* peer : peers) peer->pushFrames(repayment);
    mixer.advance(40000);
    expect(mixer.stats().adaptive_delivery_gap_hold_active && mixer.stats().adaptive_target_frames == 1024,
        "rebasing stale audio preserves the still-valid learned target and hold");
    expect(sink.depth <= 256 && std::all_of(peers.begin(), peers.end(),
            [](auto* peer) { return peer->depthFrames() <= 128; }),
        "retaining learning does not retain stale queues or enlarge the hard recovery tail");
    for (std::uint64_t now = 42000; now < 100000; now += 2000) {
        sink.consume(64);
        for (auto* peer : peers) peer->pushFrames(block);
        mixer.advance(now);
        mixer.finishReceiveBatch(false);
    }
    expect(!mixer.stats().receive_recovery_active && mixer.stats().receive_recovery_completions == 1,
        "valid learning does not prevent bounded debt recovery completion");
    mixer.advance(30002001);
    expect(!mixer.stats().adaptive_delivery_gap_hold_active,
        "the original learned hold still expires rather than being restarted by rebasing");
}

void test_four_peer_heterogeneous_gap_evidence()
{
    for (const bool largerFirst : {false, true}) {
        for (const bool splitAdvance : {false, true}) {
            for (const bool underrunFirst : {false, true}) {
                MixerSink sink;
                jam2::PeerMixerConfig config;
                config.sample_rate = 48000;
                config.frames_per_block = 64;
                config.output_max_frames = 8192;
                config.adaptive_playback_cushion = true;
                config.adaptive_target_frames = config.adaptive_min_frames = 256;
                config.adaptive_max_frames = 6144;
                jam2::PeerMixer mixer(config, &sink);
                std::array<jam2::PeerStreamPlayback*, 3> peers{};
                std::array<std::int32_t, 64> block{};
                for (std::size_t i = 0; i < peers.size(); ++i) {
                    peers[i] = mixer.addPeer(i + 1, 8192);
                    expect(peers[i] && mixer.setPeerActive(i + 1, true), "mixed gap fixture has exactly four participants");
                    if (!peers[i]) return;
                    peers[i]->pushFrames(block);
                }
                mixer.advance(1000);
                if (underrunFirst) {
                    sink.consume(sink.depth + 64);
                    mixer.advance(2000); // same pre-receive ordering as the app
                }
                peers[0]->observeDeliveryGap({3000, largerFirst ? 4800U : 3000U});
                peers[0]->pushFrames(block);
                if (splitAdvance) mixer.advance(3000);
                peers[1]->observeDeliveryGap({4000, largerFirst ? 3000U : 4800U});
                peers[1]->pushFrames(block);
                mixer.advance(4000);
                if (!underrunFirst) {
                    sink.consume(sink.depth + 64);
                    mixer.advance(5000);
                }
                std::cout << "METRIC mixed_gap split=" << splitAdvance << " underrun_first=" << underrunFirst
                          << " larger_first=" << largerFirst << " target=" << mixer.stats().adaptive_target_frames << '\n';
                expect(mixer.stats().adaptive_target_frames == 4800,
                    "smaller newer peer gap cannot replace the larger associated delivery requirement");
                expect(mixer.stats().adaptive_target_frames <= config.adaptive_max_frames,
                    "mixed peer learning respects the existing profile ceiling");
            }
        }
    }
}

void test_four_peer_gap_expiry_and_departure()
{
    for (const int mode : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        MixerSink sink;
        jam2::PeerMixerConfig config;
        config.sample_rate = 48000;
        config.frames_per_block = 64;
        config.output_max_frames = 8192;
        config.adaptive_playback_cushion = true;
        config.adaptive_target_frames = config.adaptive_min_frames = 256;
        config.adaptive_max_frames = 6144;
        jam2::PeerMixer mixer(config, &sink);
        std::array<jam2::PeerStreamPlayback*, 3> peers{};
        std::array<std::int32_t, 64> block{};
        for (std::size_t i = 0; i < peers.size(); ++i) {
            peers[i] = mixer.addPeer(i + 1, 8192);
            expect(peers[i] && mixer.setPeerActive(i + 1, true), "gap lifecycle starts with four participants");
            if (!peers[i]) return;
            peers[i]->pushFrames(block);
        }
        mixer.advance(1000);
        peers[0]->observeDeliveryGap({2000, mode == 4 ? 12000U : 4800U});
        peers[0]->pushFrames(block);
        mixer.advance(2000);
        expect(mixer.stats().adaptive_target_frames == 256 && !mixer.stats().adaptive_delivery_gap_hold_active,
            "a delivery gap alone does not buy extra latency without a real output underrun");
        if (mode == 1) {
            mixer.setPeerActive(1, false);
            mixer.setPeerActive(1, true);
            peers[0]->pushFrames(block);
        } else if (mode == 2) {
            expect(mixer.removePeer(1), "departing source is removed with its unconsumed evidence");
        }
        const auto newerTime = mode == 0 || mode == 6 ? 253000ULL : mode == 7 ? 252000ULL : 3000ULL;
        if (mode >= 6) {
            // Preserve newer smaller same-source evidence when the original
            // peak expires; exact 250 ms boundary still includes the peak.
            peers[0]->observeDeliveryGap({102000, 3000});
            if (mode == 8) {
                // Short custom observation thresholds may saturate the fixed
                // history. Retention is bounded and never renews old ages.
                for (std::uint64_t i = 0; i < 16; ++i) peers[0]->observeDeliveryGap({103000 + i, 3000});
            }
        } else peers[1]->observeDeliveryGap({newerTime, 3000});
        peers[1]->pushFrames(block);
        // Consume before advance so actual callback starvation, not injected
        // counters or a source-only shortage, triggers the association.
        if (mode != 5) sink.consume(sink.depth + 64);
        const auto observationTime = mode == 8 ? 104000ULL : newerTime;
        mixer.advance(observationTime);
        const auto expected = mode == 5 ? 256U : mode == 4 ? 6144U : mode == 3 || mode == 7 ? 4800U : 3000U;
        expect(mixer.stats().adaptive_target_frames == expected,
            "expired/departed evidence cannot inflate learning; live evidence remains profile capped");
        const auto holds = mixer.stats().adaptive_delivery_gap_hold_events;
        for (std::uint64_t step = 1; step < 5; ++step) {
            peers[1]->pushFrames(block);
            mixer.advance(observationTime + step);
        }
        expect(mixer.stats().adaptive_delivery_gap_hold_events == holds,
            "revisiting unchanged source evidence cannot renew the learned hold");
        if (mode == 7) {
            mixer.advance(30002000);
            expect(!mixer.stats().adaptive_delivery_gap_hold_active,
                "a smaller same-peer observation cannot refresh the original peak hold timestamp");
        }
        mixer.advance(observationTime + 30000001ULL);
        expect(!mixer.stats().adaptive_delivery_gap_hold_active,
            "mixed-source learning retains the existing finite hold lifetime");
    }
}

void test_four_peer_packet_gap_association()
{
    // Production packet acceptance -> delivery evidence -> mixer -> real ring
    // and callback resampler. Per-datagram timestamps differ even within one
    // app receive batch; do not quantize them to a single mixer timestamp.
    for (const bool samePeerSecondGap : {false, true}) {
        for (const bool splitBatch : {false, true}) {
            ProfileMixerSink sink(8192, 64);
            jam2::PeerMixerConfig config;
            config.sample_rate = 48000;
            config.frames_per_block = 64;
            config.deadline_frames = 8192;
            config.output_max_frames = 8192;
            config.adaptive_playback_cushion = true;
            config.adaptive_min_frames = config.adaptive_target_frames = 256;
            config.adaptive_max_frames = 6144;
            jam2::PeerMixer mixer(config, &sink);
            jam2::PeerStreamConfig streamConfig;
            streamConfig.frames_per_packet = 64;
            streamConfig.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
            streamConfig.playback_queue_capacity_frames = 8192;
            streamConfig.drift_correction = false;
            std::array<std::unique_ptr<jam2::PeerStream>, 3> streams;
            for (std::size_t i = 0; i < streams.size(); ++i) {
                auto* peer = mixer.addPeer(i + 1, 8192);
                expect(peer && mixer.setPeerActive(i + 1, true), "packet gap test activates four participants");
                if (!peer) return;
                streams[i] = std::make_unique<jam2::PeerStream>(streamConfig, 0, peer);
            }
            std::array<std::uint8_t, 128> payload{};
            payload.fill(1); // audible nonzero PCM, so padded silence cannot pass
            auto receive = [&](std::size_t peer, std::uint32_t sequence, std::uint64_t time) {
                const jam2::protocol::Header header{jam2::protocol::PacketType::Audio, 1,
                    sequence, sequence * 64ULL, 128, 0};
                expect(streams[peer]->receiveAudio(header, payload, time) == jam2::PeerAudioResult::Accepted,
                    "heterogeneous gap packets remain contiguous and accepted");
            };
            receive(0, 0, 1000);
            receive(1, 0, 40000);
            receive(2, 0, 100000);
            mixer.advance(100000);
            receive(0, 1, 101000); // 100 ms -> 4864 frames including packet reserve
            if (splitBatch) { mixer.advance(101000); mixer.finishReceiveBatch(false); }
            receive(1, 1, 102000); // 62 ms -> 3040, must not replace the 100 ms gap
            receive(2, 1, 102001); // wired-like peer supplies no long-gap evidence
            if (samePeerSecondGap) {
                // A second 61 ms interval on the same source arrives while its
                // earlier 100 ms evidence is still inside the association window.
                receive(0, 2, 162000);
            }
            const std::uint64_t episodeOffset = samePeerSecondGap ? 60000 : 0;
            mixer.advance(102001 + episodeOffset);
            mixer.finishReceiveBatch(false);
            expect(mixer.stats().adaptive_delivery_gap_raise_events == 0,
                "real packet evidence alone does not raise latency");
            expect(mixer.stats().adaptive_delivery_gap_required_max_frames == 4864,
                "gap peak telemetry preserves the same-batch evidence used by recovery");
            for (int callback = 0; callback < 8; ++callback) sink.callback();
            mixer.advance(113000 + episodeOffset);
            std::cout << "METRIC packet_mixed_gap split=" << splitBatch
                      << " same_peer=" << samePeerSecondGap
                      << " target=" << mixer.stats().adaptive_target_frames
                      << " missing=" << sink.underrunFrames() << '\n';
            expect(sink.underrunFrames() > 0 && mixer.stats().adaptive_target_frames == 4864,
                "real callback underrun learns the largest still-relevant accepted packet gap");
            // Refill with real contiguous catch-up packets, using the app's bounded
            // 64-datagram drain and pre/post advance. Then revisit a pause shorter
            // than the measured 100 ms event: the learned cushion must actually
            // bridge it, not merely publish a larger target statistic.
            // Render away any explicit recovery padding from the first underrun
            // before evaluating the next episode. Do not mistake it for real audio.
            while (sink.depthFrames() != 0) sink.callback(std::min<std::size_t>(64, sink.depthFrames()));
            const auto recoveryPadding = mixer.stats().adaptive_padding_frames;
            std::uint64_t now = 114000 + episodeOffset;
            std::size_t drained = 0;
            mixer.advance(now);
            for (std::uint32_t sequence = 2; sequence < 82; ++sequence) {
                for (std::size_t peer = 0; peer < streams.size(); ++peer) {
                    receive(peer, sequence + (samePeerSecondGap && peer == 0 ? 1U : 0U), now++);
                    if (++drained == 64) {
                        for (auto& stream : streams) stream->advance(now);
                        mixer.advance(now);
                        mixer.finishReceiveBatch(true);
                        drained = 0;
                        mixer.advance(now);
                    }
                }
            }
            for (auto& stream : streams) stream->advance(now);
            mixer.advance(now);
            mixer.finishReceiveBatch(false);
            const auto missingBefore = sink.underrunFrames();
            const auto paddingBefore = mixer.stats().adaptive_padding_frames;
            const auto outputBefore = sink.depthFrames();
            bool realAudio = true;
            for (int callback = 0; callback < 70; ++callback) {
                now += 1333;
                sink.callback();
                realAudio &= std::all_of(sink.callbackOutput.begin(), sink.callbackOutput.begin() + 64,
                    [](std::int32_t sample) { return sample != 0; });
                for (auto& stream : streams) stream->advance(now);
                mixer.advance(now);
                mixer.finishReceiveBatch(false);
            }
            std::cout << "METRIC packet_mixed_repeat split=" << splitBatch << " same_peer=" << samePeerSecondGap
                      << " output_before=" << outputBefore
                      << " missing=" << sink.underrunFrames() - missingBefore
                      << " padding=" << mixer.stats().adaptive_padding_frames - paddingBefore << '\n';
            expect(realAudio && paddingBefore == recoveryPadding && sink.underrunFrames() == missingBefore &&
                    mixer.stats().adaptive_padding_frames == paddingBefore,
                "learned real audio bridges the repeated pause without underruns or replacement silence");
            for (const auto& stream : streams) {
                expect(stream->stats().sequence.lost == 0 && stream->stats().missing_audio_frames_inserted == 0,
                    "delivery adaptation is not masking packet loss or inserted source silence");
            }
        }
    }
}

void test_profiled_wifi_return_to_clean()
{
    for (const auto profileName : {"fast", "moderate", "safe"}) {
        for (const int variant : {0, 1, 2, 3}) {
            if (variant != 0 && std::string_view(profileName) != "fast") continue;
            for (const std::size_t remoteCount : {1U, 3U}) {
                if (variant == 3 && remoteCount != 1) continue;
                for (const bool depart : {false, true}) {
                    if (depart && remoteCount == 1) continue;
                    const auto& create = *jam2::find_create_profile(profileName);
                    const auto& profile = *create.local;
                    const int rate = variant == 1 ? 44100 : 48000;
                    const std::size_t callbackFrames = variant == 1 ? 32 : 64;
                    constexpr std::uint64_t endUs = 100000000;
                    constexpr std::uint64_t cleanUs = 15000000;
                    const auto packetFrames = static_cast<std::size_t>(create.frame_size);
                    ProfileMixerSink sink(profile.playback_ring_frames, profile.playback_prefill_frames, rate);
                    sink.callbackQuantum = callbackFrames;
                    jam2::PeerMixerConfig config;
                    config.sample_rate = rate;
                    config.frames_per_block = create.frame_size;
                    config.deadline_frames = std::max({packetFrames * 2, profile.playout_delay_frames,
                        profile.jitter_buffer_frames, profile.jitter_buffer_max_frames});
                    config.output_max_frames = profile.playback_max_frames;
                    config.fixed_target_frames = profile.playout_delay_frames;
                    config.adaptive_playback_cushion = profile.adaptive_playback_cushion;
                    config.adaptive_target_frames = profile.adaptive_playback_target_frames;
                    config.adaptive_min_frames = profile.adaptive_playback_min_frames;
                    config.adaptive_max_frames = profile.adaptive_playback_max_frames;
                    config.adaptive_release_ppm = profile.adaptive_playback_release_ppm;
                    if (variant == 3) config.adaptive_surplus_stable_us = 0;
                    jam2::PeerMixer mixer(config, &sink);
                    jam2::PeerStreamConfig streamConfig;
                    streamConfig.sample_rate = rate;
                    streamConfig.frames_per_packet = create.frame_size;
                    streamConfig.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
                    streamConfig.sample_time_playout = profile.sample_time_playout;
                    streamConfig.playout_delay_frames = profile.playout_delay_frames;
                    streamConfig.playback_queue_capacity_frames = profile.playback_ring_frames;
                    streamConfig.jitter_buffer_frames = profile.jitter_buffer_frames;
                    streamConfig.jitter_buffer_max_frames = profile.jitter_buffer_max_frames;
                    streamConfig.drift_correction = profile.drift_correction;
                    streamConfig.drift_smoothing = profile.drift_smoothing;
                    streamConfig.drift_deadband_ppm = profile.drift_deadband_ppm;
                    streamConfig.drift_max_correction_ppm = profile.drift_max_correction_ppm;
                    std::vector<std::unique_ptr<jam2::PeerStream>> streams;
                    std::vector<std::uint64_t> indices(remoteCount, 1);
                    std::vector<bool> active(remoteCount, true);
                    for (std::size_t peer = 0; peer < remoteCount; ++peer) {
                        auto* playback = mixer.addPeer(peer + 1,
                            profile.playback_ring_frames + profile.jitter_buffer_max_frames);
                        expect(playback && mixer.setPeerActive(peer + 1, true), "return-to-clean fixture activates its real sources");
                        if (!playback) return;
                        streams.push_back(std::make_unique<jam2::PeerStream>(streamConfig, 0, playback));
                    }
                    std::vector<std::uint8_t> payload(packetFrames * 2);
                    auto arrival = [&](std::size_t peer) {
                        if (!active[peer]) return (std::numeric_limits<std::uint64_t>::max)();
                        auto time = indices[peer] * packetFrames * 1000000ULL / rate + 300 + peer * 250;
                        // Only peer zero is on Wi-Fi. The receiver worker remains
                        // live, serving wired contributors and expiring deadlines.
                        if (peer == 0 && time >= 4000000 && time < cleanUs && time % 1000000 >= 900000) {
                            time = (time / 1000000 + 1) * 1000000;
                        }
                        return static_cast<std::uint64_t>(time);
                    };
                    auto nextPeer = [&]() {
                        std::size_t result = remoteCount;
                        for (std::size_t peer = 0; peer < remoteCount; ++peer) {
                            if (active[peer] && (result == remoteCount || arrival(peer) < arrival(result))) result = peer;
                        }
                        return result;
                    };
                    auto path = [&]() {
                        std::uint64_t maximum = 0;
                        for (std::size_t peer = 0; peer < remoteCount; ++peer) {
                            if (!active[peer]) continue;
                            maximum = std::max(maximum, sink.depthFrames() + mixer.peerStats(peer + 1)->queue_depth_frames +
                                streams[peer]->stats().jitter_buffer_depth_frames);
                        }
                        return maximum;
                    };
                    auto dispatchTime = [&](std::uint64_t time) {
                        if (variant == 2) {
                            time = (time + 999) / 1000 * 1000;
                            if (time % 70000 >= 67000) time = (time / 70000 + 1) * 70000;
                        }
                        return time;
                    };
                    std::uint64_t now = 0, worker = 1000, callbackIndex = 1, currentPath = 0;
                    std::uint64_t baseUs = 0, finalUs = 0, peakPath = 0;
                    long double baseIntegral = 0, finalIntegral = 0;
                    std::uint64_t cleanUnderruns = 0, cleanPadding = 0, cleanTrims = 0;
                    std::uint64_t largestSampleStep = 0;
                    std::int32_t previousSample = 0;
                    bool havePreviousSample = false;
                    bool departed = false, cleanSnapshot = false, accepted = true;
                    while (now < endUs) {
                        const auto callback = callbackIndex * callbackFrames * 1000000ULL / rate + 137;
                        const auto next = std::min({callback, worker, endUs});
                        if (now >= 2000000 && now < 3500000) { baseIntegral += currentPath * (next - now); baseUs += next - now; }
                        if (now >= 90000000) { finalIntegral += currentPath * (next - now); finalUs += next - now; }
                        now = next;
                        if (!cleanSnapshot && now >= 17000000) {
                            cleanSnapshot = true;
                            cleanUnderruns = sink.underrunFrames();
                            cleanPadding = mixer.stats().adaptive_padding_frames;
                            cleanTrims = mixer.stats().live_tail_trimmed_frames;
                        }
                        if (now == callback) {
                            sink.callback(callbackFrames);
                            ++callbackIndex;
                            if (now >= 17000000) {
                                for (const auto sample : std::span(sink.callbackOutput).first(callbackFrames)) {
                                    if (havePreviousSample) largestSampleStep = std::max<std::uint64_t>(largestSampleStep,
                                        static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(sample) - previousSample)));
                                    previousSample = sample;
                                    havePreviousSample = true;
                                }
                            }
                        }
                        if (now == worker) {
                            if (depart && !departed && now >= cleanUs) {
                                // Destroy the stream before its owned mixer sink.
                                streams[0].reset();
                                expect(mixer.removePeer(1), "the impaired peer leaves without removing wired contributors");
                                active[0] = false;
                                departed = true;
                            }
                            for (std::size_t peer = 0; peer < remoteCount; ++peer) if (active[peer]) streams[peer]->advance(now);
                            mixer.advance(now);
                            std::size_t drained = 0;
                            auto peer = nextPeer();
                            while (peer < remoteCount && arrival(peer) <= now && drained < 64) {
                                const auto sequence = indices[peer] - 1;
                                for (std::size_t frame = 0; frame < packetFrames; ++frame) {
                                    const auto sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(
                                        std::sin((sequence * packetFrames + frame) * 2.0 * 3.14159265358979323846 * 220.0 / rate) * 4096.0 / remoteCount));
                                    payload[2 * frame] = static_cast<std::uint8_t>(sample);
                                    payload[2 * frame + 1] = static_cast<std::uint8_t>(sample >> 8);
                                }
                                const jam2::protocol::Header header{jam2::protocol::PacketType::Audio, 1,
                                    static_cast<std::uint32_t>(sequence), sequence * packetFrames, static_cast<std::uint16_t>(payload.size()), 0};
                                accepted &= streams[peer]->receiveAudio(header, payload, now) == jam2::PeerAudioResult::Accepted;
                                ++indices[peer]; ++drained;
                                peer = nextPeer();
                            }
                            for (std::size_t p = 0; p < remoteCount; ++p) if (active[p]) streams[p]->advance(now);
                            mixer.advance(now);
                            mixer.finishReceiveBatch(drained == 64);
                            worker = peer < remoteCount ? std::max(now + 1, std::min(now + 1000, arrival(peer))) : now + 1000;
                            worker = dispatchTime(worker);
                        }
                        currentPath = path();
                        peakPath = std::max(peakPath, currentPath);
                    }
                    const auto stats = mixer.stats();
                    const auto baseMs = static_cast<double>(baseIntegral / baseUs / rate * 1000);
                    const auto finalMs = static_cast<double>(finalIntegral / finalUs / rate * 1000);
                    std::cout << "METRIC return_clean profile=" << profileName << " remotes=" << remoteCount << " departed=" << depart
                              << " variant=" << variant << " rate=" << rate << " callback=" << callbackFrames
                              << " base_ms=" << baseMs << " final_ms=" << finalMs << " target=" << stats.adaptive_target_frames
                              << " holds=" << stats.adaptive_delivery_gap_hold_events << " hold_active=" << stats.adaptive_delivery_gap_hold_active
                              << " missing_after_17s=" << sink.underrunFrames() - cleanUnderruns
                              << " padding_after_17s=" << stats.adaptive_padding_frames - cleanPadding
                              << " cuts_after_17s=" << stats.live_tail_trimmed_frames - cleanTrims << " peak_frames=" << peakPath
                              << " final_path_frames=" << currentPath << " final_ring_frames=" << sink.depthFrames()
                              << " applied_ratio=" << sink.control.playback_ratio_applied_ppm.load()
                              << " sample_step=" << largestSampleStep << '\n';
                    expect(accepted && !stats.adaptive_delivery_gap_hold_active && !stats.receive_recovery_active,
                        "clean delivery or peer departure eventually clears recovery and the learned hold");
                    // The no-minimum-release control can retain exactly two
                    // packets. Allow one frame of interval/phase rounding at
                    // that boundary rather than requiring more than two.
                    expect(stats.adaptive_target_frames <= profile.adaptive_playback_min_frames + packetFrames &&
                            (variant == 3 ? finalMs >= baseMs + (2.0 * packetFrames - 1) * 1000.0 / rate :
                                finalMs <= baseMs + 2.0 * packetFrames * 1000.0 / rate),
                        variant == 3 ? "disabling minimum-target release reproduces retained real buffering" :
                            "actual buffered latency returns near the clean baseline, not merely the target statistic");
                    expect(sink.underrunFrames() == cleanUnderruns && stats.adaptive_padding_frames == cleanPadding &&
                            stats.live_tail_trimmed_frames == cleanTrims && largestSampleStep < 33554432,
                        "retiring learned Wi-Fi protection does not create fresh underruns, silence or sample cuts");
                }
            }
        }
    }
}

// Independent packet, callback and network-maintenance clocks reproduce the
// short local scheduling batches missing from the callback-aligned Wi-Fi test.
// This is a bounded synthetic analogue of the September 11 logs, not a replay:
// their two-second CSV snapshots do not contain individual packet timestamps.
void test_profiled_short_scheduler_batches(bool lowSkew = false, bool releaseOnly = false,
    std::string_view timingProfile = {}, std::size_t timingPacketFrames = 0,
    std::size_t timingPrefillFrames = 0)
{
    struct Scenario {
        std::string_view profileName;
        bool batched;
        std::size_t callbackFrames;
        std::uint64_t callbackPhaseUs = 137;
        std::size_t remotePeers = 1;
        std::uint64_t rate = 44100;
        int senderPpm = 0;
        std::uint64_t callbackJitterUs = 0;
        std::size_t packetFrames = 0;
        std::uint64_t durationUs = 60000000;
        std::size_t senderCallbackFrames = 0;
        bool startupClockChange = false;
        std::uint64_t startupClockEndUs = 4000000;
        bool disableMinimumRelease = false;
        bool disableDriftDeadband = false;
        bool disableAllRelease = false;
        std::size_t outputReleaseGuardFrames = 0;
        bool instantaneousOutputRatio = false;
        std::uint64_t releaseStopRampMs = 0;
    };
    std::vector<Scenario> scenarios;
    if (!timingProfile.empty()) {
        const auto* profile = jam2::find_create_profile(timingProfile);
        expect(profile != nullptr, "profile timing fixture uses a maintained profile");
        if (profile == nullptr) return;
        for (const std::size_t peers : {1U, 3U}) {
            for (const std::uint64_t rate : {44100U, 48000U}) {
                for (const bool batched : {false, true}) {
                    for (const int ppm : {-10, 10}) {
                        scenarios.push_back({timingProfile, batched,
                            static_cast<std::size_t>(profile->local->audio_buffer_size),
                            137, peers, rate, ppm, 0, timingPacketFrames, 600000000, 0, true});
                    }
                }
            }
        }
    } else if (lowSkew) {
        // Three minutes crosses a complete 64-frame phase slip at 10 ppm.
        // Use the actual estimator/deadband, not a forced playback ratio.
        for (const auto profile : {"fast", "safe"}) {
            for (const std::size_t peers : {1U, 3U}) {
                for (const std::size_t callback : {32U, 64U}) {
                    for (const bool batched : {false, true}) {
                        for (const int ppm : {-10, 0, 10}) {
                            for (const bool startup : {false, true}) {
                                if (std::string_view(profile) == "safe" && (ppm == 0 || !startup)) continue;
                                scenarios.push_back({profile, batched, callback, 137, peers,
                                    44100, ppm, 0, 0, 180000000, 0, startup});
                            }
                        }
                    }
                }
            }
        }
        for (const int ppm : {-26, -24, 24, 26}) {
            for (const std::size_t peers : {1U, 3U}) {
                for (const std::size_t callback : {32U, 64U}) {
                    scenarios.push_back({"fast", false, callback, 137, peers,
                        44100, ppm, 0, 64, 180000000});
                }
            }
        }
        for (const std::uint64_t phase : {0U, 637U, 1137U}) {
            for (const std::size_t callback : {32U, 64U}) {
                for (const bool batched : {false, true}) {
                    scenarios.push_back({"fast", batched, callback, phase, 1,
                        44100, 0, 0, 64, 180000000, 0, true});
                }
            }
        }
        // Narrow counterfactual controls use existing configuration seams.
        // They do not change the production defaults or recommend a setting.
        for (const std::size_t callback : {32U, 64U}) {
            scenarios.push_back({"fast", true, callback, 137, 1,
                44100, 0, 0, 64, 180000000, 0, true, 4000000, true});
            scenarios.push_back({"fast", true, callback, 137, 1,
                44100, 0, 0, 64, 180000000, 0, true, 4000000, false, false, true});
            for (const std::size_t peers : {1U, 3U}) {
                scenarios.push_back({"fast", false, callback, 137, peers,
                    44100, -10, 0, 64, 180000000, 0, false, 4000000, false, true});
            }
            // Very brief early excursions exercise near-complete remainders
            // without bypassing the real drift calibration and slew limit.
            for (const std::uint64_t end : {3350000U, 3500000U}) {
                for (const bool batched : {false, true}) {
                    scenarios.push_back({"fast", batched, callback, 137, 1,
                        44100, 0, 0, 64, 180000000, 0, true, end});
                }
            }
        }
        // Isolate output stopping runway from source alignment and the
        // smoother's remaining consumption after a stop request. These are
        // diagnostic vetoes at the playback seam, NOT proposed app defaults.
        for (const std::size_t callback : {32U, 64U}) {
            for (const std::size_t peers : {1U, 3U}) {
                for (const bool batched : {false, true}) {
                    for (const bool startup : {false, true}) {
                        for (const std::size_t guard : {224U, 256U}) {
                            Scenario probe{"fast", batched, callback, 137, peers,
                                44100, 0, 0, 64, 180000000, 0, startup};
                            probe.outputReleaseGuardFrames = guard;
                            scenarios.push_back(probe);
                        }
                    }
                }
            }
            Scenario instant{"fast", true, callback, 137, 1,
                44100, 0, 0, 64, 180000000, 0, true};
            instant.instantaneousOutputRatio = true;
            scenarios.push_back(instant);
        }
        // Preserve the production 250 ms acceleration ramp, varying only the
        // return to unity. This separates stopping lag from abrupt startup.
        for (const std::uint64_t stopMs : {1U, 5U, 10U, 25U, 50U}) {
            for (const std::size_t callback : {32U, 64U}) {
                for (const std::size_t peers : {1U, 3U}) {
                    for (const bool batched : {false, true}) {
                        Scenario probe{"fast", batched, callback, 137, peers,
                            44100, 0, 0, 64, 180000000, 0, true};
                        probe.releaseStopRampMs = stopMs;
                        scenarios.push_back(probe);
                    }
                }
            }
        }
        for (const std::uint64_t phase : {0U, 637U, 1137U}) {
            for (const std::uint64_t stopMs : {10U, 50U}) {
                for (const std::size_t callback : {32U, 64U}) {
                    Scenario probe{"fast", true, callback, phase, 1,
                        44100, 0, 0, 64, 180000000, 0, true};
                    probe.releaseStopRampMs = stopMs;
                    scenarios.push_back(probe);
                }
            }
        }
        for (const std::uint64_t stopMs : {75U, 100U, 150U, 200U}) {
            Scenario probe{"fast", true, 32, 137, 1,
                44100, 0, 0, 64, 180000000, 0, true};
            probe.releaseStopRampMs = stopMs;
            scenarios.push_back(probe);
        }
    } else {
        for (const auto profileName : {"fast", "moderate", "safe"}) {
            for (const bool batched : {false, true}) {
                for (const std::size_t callbackFrames : {32U, 64U}) {
                    scenarios.push_back({profileName, batched, callbackFrames});
                }
            }
        }
        for (const std::uint64_t phase : {0U, 637U, 1137U}) {
            for (const std::size_t callbackFrames : {32U, 64U}) {
                scenarios.push_back({"fast", true, callbackFrames, phase});
                // One local receiver plus three remote contributors is exactly
                // four peers. Include the usual wired 48 kHz sample rate too.
                scenarios.push_back({"fast", true, callbackFrames, phase, 3, 48000});
            }
        }
        for (const auto profile : {"fast", "moderate", "safe"}) {
            for (const int ppm : {-100, 100}) {
                scenarios.push_back({profile, true, 64, 137, 3, 48000, ppm, 400});
            }
        }
        for (const auto profile : {"moderate", "safe"}) {
            for (const std::size_t callback : {128U, 256U}) {
                scenarios.push_back({profile, true, callback, 137, 3, 44100, 0, 400, 64});
            }
        }
        scenarios.push_back({"fast", false, 64, 137, 1, 48000, 100, 0, 0, 180000000});
        // Separate the initial clock-phase transition from recurring release
        // starvation, and isolate the worker hold from the source clocks.
        scenarios.push_back({"fast", true, 64, 137, 3, 48000, 100, 400, 0, 180000000});
        scenarios.push_back({"fast", false, 64, 137, 3, 48000, 100, 400});
        // Separate callback jitter, independent source clocks and packet granularity;
        // the two-peer variants protect the primary wired workflow.
        for (const int ppm : {-100, 100}) {
            scenarios.push_back({"fast", true, 64, 137, 3, 48000, ppm, 0});
            scenarios.push_back({"fast", true, 64, 137, 1, 48000, ppm, 400});
            scenarios.push_back({"safe", true, 64, 137, 3, 48000, ppm, 400, 64});
        }
        for (const std::size_t peers : {1U, 3U}) {
            for (const std::size_t callback : {128U, 256U}) {
                scenarios.push_back({"fast", false, callback, 137, peers, 48000, 100, 400, 64, 60000000, callback});
            }
        }
    }
    for (const auto& scenario : scenarios) {
        if (releaseOnly && (scenario.senderPpm != 0 || scenario.disableMinimumRelease ||
                scenario.disableDriftDeadband || scenario.disableAllRelease ||
                scenario.outputReleaseGuardFrames != 0 || scenario.instantaneousOutputRatio ||
                scenario.releaseStopRampMs != 0)) continue;
        const auto profileName = scenario.profileName;
        const auto callbackFrames = scenario.callbackFrames;
        const auto batched = scenario.batched;
        const auto& create = *jam2::find_create_profile(profileName);
        auto profile = *create.local;
        // Diagnostic override only: leave the maintained product profile intact.
        if (timingPrefillFrames != 0) profile.playback_prefill_frames = timingPrefillFrames;
        const auto rate = scenario.rate;
        const auto durationUs = scenario.durationUs;
        const auto packetFrames = scenario.packetFrames != 0
            ? scenario.packetFrames : static_cast<std::size_t>(create.frame_size);
        ProfileMixerSink sink(profile.playback_ring_frames, profile.playback_prefill_frames, rate);
        sink.callbackQuantum = callbackFrames;
        sink.releaseGuardFrames = scenario.outputReleaseGuardFrames;
        if (scenario.releaseStopRampMs != 0) sink.control.playback_release_return_ramp_frames.store(
            scenario.releaseStopRampMs * rate / 1000, std::memory_order_relaxed);
        if (scenario.instantaneousOutputRatio) sink.control.playback_ratio_ramp_frames.store(1, std::memory_order_relaxed);
        jam2::PeerMixerConfig config;
        config.sample_rate = static_cast<int>(rate);
        config.frames_per_block = static_cast<int>(packetFrames);
        config.deadline_frames = std::max({packetFrames * 2,
            profile.playout_delay_frames, profile.jitter_buffer_frames,
            profile.jitter_buffer_max_frames});
        config.output_max_frames = profile.playback_max_frames;
        config.fixed_target_frames = profile.playout_delay_frames;
        config.adaptive_playback_cushion = profile.adaptive_playback_cushion;
        config.adaptive_target_frames = profile.adaptive_playback_target_frames;
        config.adaptive_min_frames = profile.adaptive_playback_min_frames;
        config.adaptive_max_frames = profile.adaptive_playback_max_frames;
        config.adaptive_release_ppm = profile.adaptive_playback_release_ppm;
        if (scenario.disableMinimumRelease) config.adaptive_surplus_stable_us = 0;
        if (scenario.disableAllRelease) config.adaptive_release_ppm = 0;
        jam2::PeerMixer mixer(config, &sink);
        std::vector<jam2::PeerStreamPlayback*> playbacks;
        for (std::size_t peer = 0; peer < scenario.remotePeers; ++peer) {
            auto* playback = mixer.addPeer(peer + 1, profile.playback_ring_frames + profile.jitter_buffer_max_frames);
            expect(playback != nullptr && mixer.setPeerActive(peer + 1, true), "scheduler fixture activates source");
            if (playback == nullptr) return;
            playbacks.push_back(playback);
        }
        jam2::PeerStreamConfig streamConfig;
        streamConfig.sample_rate = static_cast<int>(rate);
        streamConfig.frames_per_packet = static_cast<int>(packetFrames);
        streamConfig.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
        streamConfig.sample_time_playout = profile.sample_time_playout;
        streamConfig.playout_delay_frames = profile.playout_delay_frames;
        streamConfig.playback_queue_capacity_frames = profile.playback_ring_frames;
        streamConfig.jitter_buffer_frames = profile.jitter_buffer_frames;
        streamConfig.jitter_buffer_max_frames = profile.jitter_buffer_max_frames;
        streamConfig.adaptive_playback_cushion = false;
        streamConfig.drift_correction = profile.drift_correction;
        streamConfig.drift_smoothing = profile.drift_smoothing;
        streamConfig.drift_deadband_ppm = profile.drift_deadband_ppm;
        if (scenario.disableDriftDeadband) streamConfig.drift_deadband_ppm = 0;
        streamConfig.drift_max_correction_ppm = profile.drift_max_correction_ppm;
        std::vector<std::unique_ptr<jam2::PeerStream>> streams;
        for (auto* playback : playbacks) {
            streams.push_back(std::make_unique<jam2::PeerStream>(streamConfig, 0, playback));
        }
        std::vector<std::uint8_t> payload(packetFrames * 2);
        std::vector<std::vector<std::uint64_t>> captureSendTimes(streams.size());
        if (scenario.senderCallbackFrames != 0) {
            for (std::size_t peer = 0; peer < streams.size(); ++peer) {
                const int ppm = peer % 2 == 0 ? scenario.senderPpm : -scenario.senderPpm;
                const auto pacing = jam2::capture_audio_pacing(static_cast<long>(scenario.senderCallbackFrames), static_cast<int>(packetFrames));
                jam2::NetworkCapturePacketPacer pacer(pacing, packetFrames);
                jam2::NetworkPacketSchedule schedule(static_cast<int>(rate), static_cast<int>(packetFrames), 0);
                auto callbackTime = [&](std::uint64_t index) {
                    return static_cast<std::uint64_t>(static_cast<long double>(index) * scenario.senderCallbackFrames * 1000000000000.0L /
                        rate / (1000000 + ppm)) + peer * 250;
                };
                std::uint64_t callbackIndex = 1;
                std::uint64_t senderNow = callbackTime(callbackIndex);
                std::size_t captured = 0;
                bool captureOverride = true;
                while (senderNow <= durationUs + 1000000) {
                    if (senderNow == callbackTime(callbackIndex)) {
                        captured += scenario.senderCallbackFrames;
                        ++callbackIndex;
                    }
                    captureOverride |= pacer.captureReady(captured);
                    std::size_t sent = 0;
                    while (captured >= packetFrames && schedule.audioSendReady(senderNow, pacing, captureOverride) && sent < 8) {
                        captureSendTimes[peer].push_back(senderNow);
                        captured -= packetFrames;
                        schedule.commitAudioPacket();
                        pacer.observePacketSent(captured);
                        captureOverride = pacer.catchupReady(captured);
                        ++sent;
                    }
                    if (captured < packetFrames) captureOverride = true;
                    senderNow = captured >= packetFrames
                        ? std::min(callbackTime(callbackIndex), std::max(senderNow + 1, schedule.nextAudioSendUs()))
                        : callbackTime(callbackIndex);
                }
            }
        }
        auto receiveTime = [&](std::uint64_t index, std::size_t peer) {
            const int ppm = peer % 2 == 0 ? scenario.senderPpm : -scenario.senderPpm;
            std::uint64_t time = static_cast<std::uint64_t>(
                static_cast<long double>(index) * packetFrames * 1000000000000.0L /
                rate / (1000000 + ppm)) + 300;
            if (scenario.startupClockChange) {
                // Continuous sender sample clock: +/-100 ppm for four nominal
                // seconds (or the specified shorter excursion), then low skew.
                // No sample-time jump,
                // loss, ratio override or estimator warmup shortcut.
                const auto nominalUs = static_cast<long double>(index) * packetFrames * 1000000.0L / rate;
                const int startupPpm = peer % 2 == 0 ? 100 : -100;
                const auto end = static_cast<long double>(scenario.startupClockEndUs);
                time = static_cast<std::uint64_t>(
                    std::min(nominalUs, end) * 1000000.0L / (1000000 + startupPpm) +
                    std::max(nominalUs - end, 0.0L) * 1000000.0L / (1000000 + ppm)) + 300;
            }
            if (scenario.senderPpm != 0) time += peer * 250;
            if (scenario.senderCallbackFrames != 0) time = captureSendTimes[peer].at(index - 1) + 300;
            if (batched) {
                // Normal 1 ms dispatch quantization, plus a 3 ms hold
                // every 70 ms: ordered, lossless short delivery gaps.
                time = (time + 999) / 1000 * 1000;
                if (time % 70000 >= 67000) time = (time / 70000 + 1) * 70000;
            }
            return time;
        };
        std::vector<std::uint64_t> packetIndices(streams.size(), 1);
        auto nextPeer = [&]() {
            std::size_t result = 0;
            for (std::size_t i = 1; i < streams.size(); ++i) {
                if (receiveTime(packetIndices[i], i) < receiveTime(packetIndices[result], result)) result = i;
            }
            return result;
        };
        auto nextReceiveTime = [&]() {
            const auto peer = nextPeer();
            return receiveTime(packetIndices[peer], peer);
        };
        std::uint64_t callbackIndex = 1;
        // The runtime has one receive/mixer worker, not an independent 250 us
        // mixer clock. A scheduling hold stops both socket draining and mixing.
        auto dispatchTime = [&](std::uint64_t time) {
            if (batched) {
                time = (time + 999) / 1000 * 1000;
                if (time % 70000 >= 67000) time = (time / 70000 + 1) * 70000;
            }
            return time;
        };
        std::uint64_t workerUs = dispatchTime(std::min<std::uint64_t>(1000, nextReceiveTime()));
        std::uint64_t now = 0, measuredUs = 0, steadyUnderruns = 0;
        std::uint64_t steadyPadding = 0, steadyTrimmed = 0;
        std::uint64_t lastUnderrunUs = 0;
        long double pathIntegral = 0;
        std::size_t path = 0, maximumPath = 0;
        bool accepted = true;
        std::uint64_t mixedRealFrames = 0;
        auto advanceMixer = [&](std::uint64_t time) {
            std::uint64_t contributors = 0;
            for (std::size_t i = 0; i < playbacks.size(); ++i) {
                contributors += mixer.peerStats(i + 1)->contributing ? 1 : 0;
            }
            const auto before = mixer.stats();
            mixer.advance(time);
            mixedRealFrames += (mixer.stats().output_frames - before.output_frames -
                (mixer.stats().adaptive_padding_frames - before.adaptive_padding_frames)) * contributors -
                (mixer.stats().missing_peer_frames - before.missing_peer_frames);
        };
        bool havePreviousSample = false;
        bool previousCallbackHadExpectedGap = false;
        std::int32_t previousSample = 0;
        std::uint64_t maximumSampleStep = 0;
        std::uint64_t unexplainedSampleStep = 0;
        std::uint64_t unexpectedUnderruns = 0, unexpectedPadding = 0;
        std::uint64_t startupStarvationFrames = 0;
        std::uint64_t recoveryPaddingAllowance = 0;
        struct SilenceRange { std::uint64_t begin, end; };
        std::deque<SilenceRange> recoverySilence;
        struct Window {
            std::uint64_t begin, end, measuredUs = 0;
            long double pathIntegral = 0;
            std::uint64_t underruns = 0, padding = 0, trims = 0, missing = 0, drops = 0;
            std::uint64_t starvation = 0, unexpectedPadding = 0, unexplainedStep = 0;
            std::uint64_t outputCorrectionUs = 0, sourceCorrectionUs = 0, maximumStep = 0;
        };
        std::array<Window, 3> windows{{{5000000, 30000000}, {30000000, 90000000}, {90000000, durationUs}}};
        std::vector<long double> remainderIntegral(streams.size(), 0);
        std::vector<std::size_t> minimumRemainder(streams.size(), packetFrames), maximumRemainder(streams.size(), 0);
        std::vector<bool> corrected(streams.size(), false);
        std::size_t traceEvents = 0;
        while (now < durationUs) {
            const auto callbackUs = callbackIndex * callbackFrames * 1000000ULL / rate + scenario.callbackPhaseUs +
                (callbackIndex % 31 == 0 ? scenario.callbackJitterUs : 0);
            const auto next = std::min({callbackUs, workerUs, durationUs});
            if (lowSkew) {
                bool sourceCorrection = false;
                for (const auto& stream : streams) sourceCorrection |= stream->stats().resampler_ratio != 1.0;
                for (auto& window : windows) {
                    const auto begin = std::max(now, window.begin), end = std::min(next, window.end);
                    if (end <= begin) continue;
                    const auto elapsed = end - begin;
                    window.measuredUs += elapsed;
                    window.pathIntegral += static_cast<long double>(path) * elapsed;
                    window.outputCorrectionUs += sink.control.playback_ratio_ppm.load(std::memory_order_relaxed) != 1000000 ? elapsed : 0;
                    window.sourceCorrectionUs += sourceCorrection ? elapsed : 0;
                    if (window.begin == 90000000) {
                        for (std::size_t peer = 0; peer < streams.size(); ++peer) {
                            const auto remainder = playbacks[peer]->depthFrames() % packetFrames;
                            remainderIntegral[peer] += static_cast<long double>(remainder) * elapsed;
                            minimumRemainder[peer] = std::min(minimumRemainder[peer], remainder);
                            maximumRemainder[peer] = std::max(maximumRemainder[peer], remainder);
                        }
                    }
                }
            }
            const auto beforeUnderruns = sink.underrunFrames();
            const auto beforeMixer = mixer.stats();
            const auto beforeDrops = lowSkew ? sink.ring.stats().drop_applied_frames : 0;
            const auto beforeOutputRatio = sink.control.playback_ratio_ppm.load(std::memory_order_relaxed);
            const auto beforeOutputDepth = sink.depthFrames();
            if (now >= 5000000) {
                pathIntegral += static_cast<long double>(path) * (next - now);
                measuredUs += next - now;
            } else {
                steadyUnderruns = sink.underrunFrames();
                steadyPadding = mixer.stats().adaptive_padding_frames;
                steadyTrimmed = mixer.stats().live_tail_trimmed_frames;
            }
            now = next;
            std::uint64_t starvationFrames = 0, unexpectedPaddingFrames = 0;
            if (now == callbackUs) {
                const auto before = sink.underrunFrames();
                const auto beforeRing = sink.ring.stats();
                std::size_t commonSource = (std::numeric_limits<std::size_t>::max)();
                bool emptySourceAndJitter = false;
                for (std::size_t peer = 0; peer < playbacks.size(); ++peer) {
                    if (mixer.peerStats(peer + 1)->contributing) {
                        commonSource = std::min(commonSource, playbacks[peer]->depthFrames());
                        emptySourceAndJitter |= playbacks[peer]->depthFrames() == 0 &&
                            streams[peer]->stats().jitter_buffer_depth_frames == 0;
                    }
                }
                const bool unity = beforeOutputRatio == 1000000 &&
                    sink.control.playback_ratio_applied_ppm.load(std::memory_order_relaxed) == 1000000;
                const auto readBegin = beforeRing.depth_observed_frames - beforeRing.underruns +
                    beforeRing.drop_applied_frames;
                sink.callback(callbackFrames);
                const auto missing = sink.underrunFrames() - before;
                if (timingPrefillFrames != 0 && missing != 0) {
                    std::cout << "PREFILL_UNDERRUN time_us=" << now << " prefill_frames=" << profile.playback_prefill_frames
                        << " remote_peers=" << scenario.remotePeers << " rate=" << rate
                        << " sender_ppm=" << scenario.senderPpm << " batched=" << batched
                        << " missing_frames=" << missing << " output_before=" << beforeOutputDepth
                        << " common_source=" << commonSource << " empty_source_and_jitter=" << emptySourceAndJitter
                        << " requested_ratio_ppm=" << beforeOutputRatio
                        << " applied_ratio_ppm=" << sink.control.playback_ratio_applied_ppm.load(std::memory_order_relaxed)
                        << " target_frames=" << mixer.stats().adaptive_target_frames << '\n';
                }
                // Fast explicitly trades scheduling tolerance for latency. A
                // delayed worker cannot render a source that has no audio.
                // Queued common audio or still-accelerated output is a bug,
                // rather than an acceptable no-data shortage.
                if (profileName == "fast" && commonSource == 0 && emptySourceAndJitter && unity)
                    starvationFrames = missing;
                if (now < 5000000) startupStarvationFrames += starvationFrames;
                recoveryPaddingAllowance += starvationFrames != 0 ? packetFrames : 0;
                if (now >= 5000000) unexpectedUnderruns += missing - starvationFrames;
                const auto afterRing = sink.ring.stats();
                const auto readEnd = afterRing.depth_observed_frames - afterRing.underruns +
                    afterRing.drop_applied_frames;
                // Padding ranges are observed from the production mixer's
                // writes, not inferred from the tone's sample values. Include
                // its two interpolation-history samples at a read boundary.
                while (!recoverySilence.empty() && recoverySilence.front().end + 2 <= readBegin)
                    recoverySilence.pop_front();
                bool expectedGap = starvationFrames != 0;
                for (const auto& range : recoverySilence)
                    expectedGap |= range.begin < readEnd && range.end + 2 > readBegin;
                if (now >= 5000000) {
                    bool first = true;
                    for (const auto sample : std::span(sink.callbackOutput).first(callbackFrames)) {
                        if (havePreviousSample) {
                            const auto step = static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(sample) - previousSample));
                            maximumSampleStep = std::max(maximumSampleStep, step);
                            if (!expectedGap && !(first && previousCallbackHadExpectedGap)) {
                                unexplainedSampleStep = std::max(unexplainedSampleStep, step);
                                if (lowSkew) for (auto& window : windows) {
                                    if (now >= window.begin && now < window.end)
                                        window.unexplainedStep = std::max(window.unexplainedStep, step);
                                }
                            }
                            if (lowSkew) for (auto& window : windows) {
                                if (now >= window.begin && now < window.end) window.maximumStep = std::max(window.maximumStep, step);
                            }
                        }
                        previousSample = sample;
                        havePreviousSample = true;
                        first = false;
                    }
                }
                previousCallbackHadExpectedGap = expectedGap;
                if (sink.underrunFrames() != before) lastUnderrunUs = now;
                ++callbackIndex;
            }
            if (now == workerUs) {
                // Match NetworkRuntime: expire deadlines before receiving,
                // drain a bounded batch, then advance and finish the batch.
                for (auto& stream : streams) stream->advance(now);
                advanceMixer(now);
                std::size_t received = 0;
                while (nextReceiveTime() <= now && received < 64) {
                    const auto peer = nextPeer();
                    auto& packetIndex = packetIndices[peer];
                    for (std::size_t frame = 0; frame < packetFrames; ++frame) {
                        const double phase = static_cast<double>((packetIndex - 1) * packetFrames + frame) *
                            (2.0 * 3.14159265358979323846 * 220.0 / static_cast<double>(rate));
                        const auto sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(
                            std::sin(phase) * 4096.0 / static_cast<double>(streams.size())));
                        payload[frame * 2] = static_cast<std::uint8_t>(sample);
                        payload[frame * 2 + 1] = static_cast<std::uint8_t>(sample >> 8);
                    }
                    const jam2::protocol::Header header{jam2::protocol::PacketType::Audio,
                        1, static_cast<std::uint32_t>(packetIndex - 1),
                        (packetIndex - 1) * packetFrames,
                        static_cast<std::uint16_t>(payload.size()), 0};
                    accepted &= streams[peer]->receiveAudio(header, payload, now) == jam2::PeerAudioResult::Accepted;
                    ++packetIndex;
                    ++received;
                }
                for (auto& stream : streams) stream->advance(now);
                advanceMixer(now);
                mixer.finishReceiveBatch(received >= 64);
                const auto padding = mixer.stats().adaptive_padding_frames - beforeMixer.adaptive_padding_frames;
                if (padding != 0 && now >= 5000000) {
                    const auto allowed = std::min(padding, recoveryPaddingAllowance);
                    recoveryPaddingAllowance -= allowed;
                    unexpectedPaddingFrames = padding - allowed;
                    unexpectedPadding += unexpectedPaddingFrames;
                    if (allowed != 0) recoverySilence.push_back({
                        mixer.stats().output_frames - padding, mixer.stats().output_frames});
                }
                // A completed receive batch ends this recovery opportunity;
                // unused allowance cannot excuse silence added much later.
                if (received < 64) recoveryPaddingAllowance = 0;
                workerUs = dispatchTime(std::max(now + 1,
                    std::min(now + 1000, nextReceiveTime())));
            }
            path = 0;
            for (std::size_t peer = 0; peer < streams.size(); ++peer) {
                path = std::max(path, sink.depthFrames() + playbacks[peer]->depthFrames() +
                    static_cast<std::size_t>(streams[peer]->stats().jitter_buffer_depth_frames));
            }
            maximumPath = std::max(maximumPath, path);
            if (lowSkew) {
                for (std::size_t peer = 0; peer < streams.size(); ++peer) corrected[peer] = corrected[peer] || streams[peer]->stats().resampler_ratio != 1.0;
                for (auto& window : windows) {
                    if (now < window.begin || now >= window.end) continue;
                    window.underruns += sink.underrunFrames() - beforeUnderruns;
                    window.starvation += starvationFrames;
                    window.unexpectedPadding += unexpectedPaddingFrames;
                    window.padding += mixer.stats().adaptive_padding_frames - beforeMixer.adaptive_padding_frames;
                    window.trims += mixer.stats().live_tail_trimmed_frames - beforeMixer.live_tail_trimmed_frames;
                    window.missing += mixer.stats().missing_peer_frames - beforeMixer.missing_peer_frames;
                    window.drops += sink.ring.stats().drop_applied_frames - beforeDrops;
                }
                // Bounded event trace for the two-peer phase/release A/B only.
                // These are test-thread observations, never callback logging.
                const auto outputRatio = sink.control.playback_ratio_ppm.load(std::memory_order_relaxed);
                if (profileName == "fast" && callbackFrames == 32 && scenario.remotePeers == 1 &&
                        scenario.senderPpm == 0 && scenario.startupClockChange &&
                        scenario.startupClockEndUs == 4000000 && scenario.callbackPhaseUs == 137 &&
                        batched && now >= 30000000 && traceEvents < 24 &&
                        (outputRatio != beforeOutputRatio || sink.underrunFrames() != beforeUnderruns ||
                         mixer.stats().adaptive_padding_frames != beforeMixer.adaptive_padding_frames)) {
                    ++traceEvents;
                    std::cout << "TRACE low_skew_release time_us=" << now
                        << " minimum_release_disabled=" << scenario.disableMinimumRelease
                        << " all_release_disabled=" << scenario.disableAllRelease
                        << " output_before=" << beforeOutputDepth << " output_after=" << sink.depthFrames()
                        << " source=" << playbacks.front()->depthFrames()
                        << " ratio_before=" << beforeOutputRatio << " ratio_after=" << outputRatio
                        << " applied_ratio_ppm=" << sink.resampler.ratioSmoother.appliedPpm()
                        << " target=" << mixer.stats().adaptive_target_frames
                        << " underruns=" << sink.underrunFrames() - beforeUnderruns
                        << " padding=" << mixer.stats().adaptive_padding_frames - beforeMixer.adaptive_padding_frames << '\n';
                }
            }
        }
        const auto stats = mixer.stats();
        const auto ringStats = sink.ring.stats();
        const auto averageMs = static_cast<double>(pathIntegral / measuredUs / rate * 1000);
        if (!lowSkew && profileName == "fast" && scenario.remotePeers == 1 && callbackFrames <= 64) {
            // The original budget used 64-frame prefill. Account explicitly
            // for the agreed 128-frame prefill, keeping the same other limits.
            const auto extraPrefillMs = static_cast<double>(profile.playback_prefill_frames > 64
                ? profile.playback_prefill_frames - 64 : 0) * 1000 / rate;
            expect(averageMs <= (batched ? 6.0 + 1000.0 / rate : 2.0) + extraPrefillMs,
                "primary two-peer Fast keeps its lean measured latency budget");
        }
        if (profileName == "fast" && scenario.remotePeers == 3 && scenario.senderPpm != 0) {
            expect(averageMs <= (callbackFrames > 128 ? 12.0 : 8.0),
                "independent-clock protection remains within a small bounded latency budget");
        }
        std::cout << "METRIC " << (lowSkew ? "low_skew_" : "scheduler_") << profileName << '_' << callbackFrames
                  << (batched ? "_batched" : "_clean")
                  << " phase_us=" << scenario.callbackPhaseUs
                  << " remote_peers=" << scenario.remotePeers << " rate=" << rate
                  << " sender_ppm=" << scenario.senderPpm << " callback_jitter_us=" << scenario.callbackJitterUs
                  << " packet_frames=" << packetFrames << " duration_us=" << durationUs
                  << " prefill_frames=" << profile.playback_prefill_frames
                  << " sender_callback=" << scenario.senderCallbackFrames
                  << " bursts=" << ringStats.underrun_burst_events
                  << " underrun_frames=" << ringStats.underruns
                  << " steady_underrun_frames=" << sink.underrunFrames() - steadyUnderruns
                  << " unexpected_underrun_frames=" << unexpectedUnderruns
                  << " unexpected_padding_frames=" << unexpectedPadding
                  << " steady_padding=" << stats.adaptive_padding_frames - steadyPadding
                  << " steady_trimmed=" << stats.live_tail_trimmed_frames - steadyTrimmed
                  << " padding=" << stats.adaptive_padding_frames
                  << " trimmed=" << stats.live_tail_trimmed_frames
                  << " target=" << stats.adaptive_target_frames
                  << " tolerance_frames=" << stats.live_tail_batch_tolerance_frames
                  << " last_underrun_ms=" << static_cast<double>(lastUnderrunUs) / 1000.0
                  << " average_ms=" << static_cast<double>(pathIntegral / measuredUs / rate * 1000)
                  << " max_sample_step=" << maximumSampleStep
                  << " max_frames=" << maximumPath << '\n';
        if (lowSkew) {
            std::cout << "METRIC low_skew_setup startup_clock_change=" << scenario.startupClockChange
                << " startup_clock_end_us=" << scenario.startupClockEndUs
                << " minimum_release_disabled=" << scenario.disableMinimumRelease
                << " drift_deadband_disabled=" << scenario.disableDriftDeadband
                << " all_release_disabled=" << scenario.disableAllRelease
                << " output_release_guard=" << scenario.outputReleaseGuardFrames
                << " instantaneous_output_ratio=" << scenario.instantaneousOutputRatio
                << " release_stop_ramp_ms=" << scenario.releaseStopRampMs << '\n';
            for (const auto& window : windows) {
                std::cout << "METRIC low_skew_window from_s=" << window.begin / 1000000
                    << " to_s=" << window.end / 1000000
                    << " average_ms=" << static_cast<double>(window.pathIntegral / window.measuredUs / rate * 1000)
                    << " underruns=" << window.underruns << " padding=" << window.padding
                    << " starvation_frames=" << window.starvation
                    << " unexpected_padding=" << window.unexpectedPadding
                    << " trims=" << window.trims << " missing=" << window.missing << " drops=" << window.drops
                    << " source_correction_ms=" << window.sourceCorrectionUs / 1000.0
                    << " output_correction_ms=" << window.outputCorrectionUs / 1000.0
                    << " max_sample_step=" << window.maximumStep
                    << " unexplained_sample_step=" << window.unexplainedStep << '\n';
            }
            for (std::size_t peer = 0; peer < streams.size(); ++peer) {
                std::cout << "METRIC low_skew_source peer=" << peer << " corrected=" << corrected[peer]
                    << " final_drift_ppm=" << streams[peer]->stats().drift_ppm
                    << " final_ratio_ppm=" << (streams[peer]->stats().resampler_ratio - 1.0) * 1000000.0
                    << " late_remainder_min=" << minimumRemainder[peer] << " late_remainder_max=" << maximumRemainder[peer]
                    << " late_remainder_mean=" << static_cast<double>(remainderIntegral[peer] / windows.back().measuredUs) << '\n';
            }
        }
        bool lossless = true;
        for (const auto& stream : streams) lossless &= stream->stats().sequence.lost == 0;
        std::uint64_t resampledFrames = 0, retainedFrames = 0, requestedDrops = 0;
        for (std::size_t i = 0; i < playbacks.size(); ++i) {
            const auto& peer = *mixer.peerStats(i + 1);
            resampledFrames += peer.resampled_output_frames;
            retainedFrames += peer.queue_depth_frames;
            requestedDrops += peer.requested_drop_frames;
        }
        expect(resampledFrames == mixedRealFrames + retainedFrames + requestedDrops +
                stats.late_after_release_frames + stats.capacity_dropped_frames,
            "every resampled source frame is mixed, queued or explicitly accounted as discarded");
        if (scenario.senderPpm != 0) {
            for (std::size_t i = 0; i < streams.size(); ++i) {
                std::cout << "METRIC scheduler_clock peer=" << i << " drift=" << streams[i]->stats().drift_ppm
                    << " ratio=" << streams[i]->stats().resampler_ratio
                    << " queue=" << playbacks[i]->depthFrames()
                    << " trim=" << mixer.peerStats(i + 1)->live_tail_trimmed_frames << '\n';
            }
        }
        expect(accepted && lossless &&
                stats.adaptive_delivery_gap_raise_events == 0,
            "short scheduler batches preserve packets without long-gap learning");
        const bool independentClockStress = scenario.senderPpm != 0 && batched;
        if (lowSkew) {
            // Preserve the strict checks for avoidable withholding, release
            // starvation, cuts, unexplained padding and discontinuities. Only
            // observed Fast no-data shortages have the latency exception.
            for (const auto& window : windows) {
                expect(window.measuredUs == window.end - window.begin,
                    "low-skew measurements cover each full declared interval");
                if (window.begin < 30000000) continue;
                expect(window.underruns == window.starvation && window.unexpectedPadding == 0 &&
                        window.trims == 0 && window.missing == 0 && window.drops == 0 &&
                        window.unexplainedStep < 33554432,
                    "settled playback has no avoidable starvation, unexplained padding, cuts or discontinuities");
            }
            for (std::size_t peer = 0; peer < streams.size(); ++peer) {
                expect(streams[peer]->stats().drift_valid,
                    "low-skew fixture reaches the production drift estimator");
                if (scenario.startupClockChange) expect(corrected[peer],
                    "startup clock excursion genuinely exercises source resampling");
                if (!scenario.disableDriftDeadband && profile.drift_deadband_ppm > 0 &&
                        std::abs(scenario.senderPpm) < profile.drift_deadband_ppm) {
                    expect(streams[peer]->stats().resampler_ratio == 1.0,
                        "inside-deadband source correction returns to unity");
                }
            }
        } else if (batched && profileName == "fast") {
            // No-data scheduling shortages are the agreed latency tradeoff.
            // They must not hide release starvation, cuts or extra padding.
            expect(unexpectedUnderruns == 0 && unexpectedPadding == 0 &&
                    stats.live_tail_trimmed_frames == steadyTrimmed &&
                    unexplainedSampleStep < 33554432,
                "Fast accepts observed no-data shortages but rejects withholding, release starvation and unexplained artifacts");
            expect(stats.adaptive_target_frames <= profile.adaptive_playback_min_frames + packetFrames * 2,
                "four-peer Fast stress cannot retain a large adaptive cushion");
        } else {
            expect(sink.underrunFrames() == steadyUnderruns &&
                    stats.adaptive_padding_frames == steadyPadding,
                "short scheduler batches settle without recurring output underruns or padding");
            if (independentClockStress && profileName == "safe") {
                expect(stats.live_tail_trimmed_frames == steadyTrimmed && maximumSampleStep < 33554432,
                    "Safe clock convergence preserves the rendered tone without source cuts");
            } else {
                expect(stats.live_tail_trimmed_frames == steadyTrimmed && maximumSampleStep < 33554432,
                    "steady rendered tone has no discarded samples or abrupt discontinuities");
            }
            expect(stats.adaptive_target_frames == profile.adaptive_playback_min_frames,
                "short scheduling batches let the adaptive target return to its profile minimum");
        }
        const auto batchFrames = std::max(4 * packetFrames, 2 * callbackFrames);
        expect(stats.live_tail_batch_tolerance_frames == (batchFrames > profile.adaptive_playback_min_frames
                ? batchFrames - profile.adaptive_playback_min_frames : 0),
            "raw stats expose the lean-profile scheduling allowance and zero for larger profile floors");
        expect(maximumPath <= profile.adaptive_playback_max_frames + packetFrames +
                stats.live_tail_batch_tolerance_frames + profile.jitter_buffer_max_frames,
            "scheduling tolerance remains inside an explicit bounded receiver path");
        if (!lowSkew && !batched && scenario.senderCallbackFrames == 0) {
            const bool singleStartupShortage = profileName == "fast" &&
                ringStats.underruns == 1 && ringStats.underrun_burst_events == 1 &&
                startupStarvationFrames == 1;
            if (singleStartupShortage) {
                std::cout << "WARNING clean Fast playback had one missing source frame during startup"
                          << " rate=" << rate << " callback_frames=" << callbackFrames
                          << " underrun_time_ms=" << static_cast<double>(lastUnderrunUs) / 1000.0 << '\n';
            }
            expect((ringStats.underruns == 0 || singleStartupShortage) && stats.live_tail_trimmed_frames == 0,
                "clean profile pacing permits only one verified Fast startup shortage and no trimming");
        }
    }
}

void test_profiled_recurring_wifi_bursts_match_observed_session()
{
    const WifiBurstResult fastClean = run_profile_wifi_burst("fast", false, false);
    const WifiBurstResult moderateClean = run_profile_wifi_burst("moderate", false, false);
    const WifiBurstResult safeClean = run_profile_wifi_burst("safe", false, false);
    const WifiBurstResult fastWifi = run_profile_wifi_burst("fast", true, true);
    const WifiBurstResult moderateRoutineWifi = run_profile_wifi_burst("moderate", true, false);
    const WifiBurstResult safeRoutineWifi = run_profile_wifi_burst("safe", true, false);
    const WifiBurstResult moderateWifi = run_profile_wifi_burst("moderate", true, true);
    const WifiBurstResult safeWifi = run_profile_wifi_burst("safe", true, true);
    const WifiBurstResult moderateWifiWithoutGapLearning =
        run_profile_wifi_burst("moderate", true, true, 1, 100000, false);
    const WifiBurstResult safeWifiWithoutGapLearning =
        run_profile_wifi_burst("safe", true, true, 1, 100000, false);
    const WifiBurstResult fourPeerModerateRoutineWifi =
        run_profile_wifi_burst("moderate", true, false, 3);
    const WifiBurstResult fourPeerSafeWifi =
        run_profile_wifi_burst("safe", true, true, 3);
    const WifiBurstResult moderateSchedulerBatch =
        run_profile_wifi_burst("moderate", true, false, 1, 40000);

    const auto cleanHealthy = [](const WifiBurstResult& result) {
        return result.packetsAccepted && result.sequenceLoss == 0 &&
            result.missingAudioFrames == 0 && result.underrunBursts == 0 &&
            result.paddingFrames == result.startupPaddingFrames &&
            result.trimmedFrames == 0;
    };
    expect(cleanHealthy(fastClean) && cleanHealthy(moderateClean) &&
            cleanHealthy(safeClean) && fastClean.adaptiveTargetFrames == 64 &&
            moderateClean.adaptiveTargetFrames == 512 &&
            safeClean.adaptiveTargetFrames == 1024,
        "clean Fast, Moderate, and Safe profiles add no post-startup adaptive latency or correction");
    expect(fastWifi.packetsAccepted && moderateRoutineWifi.packetsAccepted &&
            safeRoutineWifi.packetsAccepted && moderateWifi.packetsAccepted &&
            safeWifi.packetsAccepted && fastWifi.sequenceLoss == 0 &&
            moderateRoutineWifi.sequenceLoss == 0 && safeRoutineWifi.sequenceLoss == 0 &&
            moderateWifi.sequenceLoss == 0 && safeWifi.sequenceLoss == 0 &&
            fastWifi.missingAudioFrames == 0 && moderateRoutineWifi.missingAudioFrames == 0 &&
            safeRoutineWifi.missingAudioFrames == 0 && moderateWifi.missingAudioFrames == 0 &&
            safeWifi.missingAudioFrames == 0,
        "profiled Wi-Fi delivery pauses retain every ordered audio packet");
    expect(fastWifi.underrunBursts >= moderateWifi.underrunBursts &&
            fastWifi.adaptiveTargetFrames <= fastWifi.adaptiveMaximumFrames,
        "Fast remains bounded and unsuitable for the observed Wi-Fi blackouts");
    expect(moderateRoutineWifi.underrunBursts <= 4 &&
            safeRoutineWifi.underrunBursts <= 4 &&
            moderateRoutineWifi.paddingFrames > moderateRoutineWifi.startupPaddingFrames &&
            safeRoutineWifi.paddingFrames > safeRoutineWifi.startupPaddingFrames &&
            moderateRoutineWifi.deliveryGapRaiseEvents > 0 &&
            safeRoutineWifi.deliveryGapRaiseEvents > 0 &&
            moderateRoutineWifi.deliveryGapHoldActive &&
            safeRoutineWifi.deliveryGapHoldActive,
        "Moderate and Safe learn enough runway after a bounded number of routine Wi-Fi artifacts");
    expect(moderateWifi.underrunBursts > moderateRoutineWifi.underrunBursts &&
            safeWifi.underrunBursts <= safeRoutineWifi.underrunBursts + 4,
        "the extreme Wi-Fi pause remains an occasional Moderate artifact while Safe absorbs it with bounded learning");
    expect(moderateWifi.maximumBufferedPathFrames <= 4096 + 1536 + 128 &&
            safeWifi.maximumBufferedPathFrames <= 7168 + 2048 + 256,
        "Wi-Fi learning stays within each profile's explicit adaptive and jitter bounds");
    expect(moderateWifi.averageBufferedPathFrames >
                moderateWifiWithoutGapLearning.averageBufferedPathFrames &&
            safeWifi.averageBufferedPathFrames >
                safeWifiWithoutGapLearning.averageBufferedPathFrames,
        "delivery-gap learning spends measurable profile-bounded latency to remove underruns");
    expect(fourPeerModerateRoutineWifi.packetsAccepted &&
            fourPeerSafeWifi.packetsAccepted &&
            fourPeerModerateRoutineWifi.sequenceLoss == 0 &&
            fourPeerSafeWifi.sequenceLoss == 0 &&
            fourPeerModerateRoutineWifi.missingAudioFrames == 0 &&
            fourPeerSafeWifi.missingAudioFrames == 0 &&
            fourPeerModerateRoutineWifi.underrunBursts <= 4 &&
            fourPeerSafeWifi.underrunBursts <= 6 &&
            fourPeerModerateRoutineWifi.deliveryGapRaiseEvents > 0 &&
            fourPeerSafeWifi.deliveryGapRaiseEvents > 0,
        "exactly four impaired peer streams retain packets and learn bounded profile runway");
    expect(moderateSchedulerBatch.deliveryGapRaiseEvents == 0,
        "sub-50 ms receive-thread batching is not treated as unstable-network learning evidence");

    auto printResult = [](std::string_view name, const WifiBurstResult& result) {
        std::cout << "METRIC wifi_" << name << "_underrun_bursts="
                  << result.underrunBursts << '\n'
                  << "METRIC wifi_" << name << "_underrun_frames="
                  << result.underrunFrames << '\n'
                  << "METRIC wifi_" << name << "_padding_frames="
                  << result.paddingFrames - result.startupPaddingFrames << '\n'
                  << "METRIC wifi_" << name << "_trimmed_frames="
                  << result.trimmedFrames << '\n'
                  << "METRIC wifi_" << name << "_adaptive_target_frames="
                  << result.adaptiveTargetFrames << '\n'
                  << "METRIC wifi_" << name << "_delivery_gap_raise_events="
                  << result.deliveryGapRaiseEvents << '\n'
                  << "METRIC wifi_" << name << "_maximum_path_frames="
                  << result.maximumBufferedPathFrames << '\n'
                  << "METRIC wifi_" << name << "_average_path_frames="
                  << result.averageBufferedPathFrames << '\n';
    };
    printResult("moderate_without_gap_learning", moderateWifiWithoutGapLearning);
    printResult("safe_without_gap_learning", safeWifiWithoutGapLearning);
    printResult("fast", fastWifi);
    printResult("moderate_routine", moderateRoutineWifi);
    printResult("safe_routine", safeRoutineWifi);
    printResult("moderate", moderateWifi);
    printResult("safe", safeWifi);
    printResult("four_peer_moderate_routine", fourPeerModerateRoutineWifi);
    printResult("four_peer_safe", fourPeerSafeWifi);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--playback-recovery") {
        test_four_peer_boundary_hold_preserves_phase_and_bounds();
        test_isolated_recovery_escalates_repeated_shortages();
        test_four_peer_worker_margin_tracks_service_gaps();
        test_profiled_wifi_return_to_clean();
        if (failures == 0) std::cout << "Jam2 playback recovery tests passed\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]).starts_with("--profile-")) {
        const auto mode = std::string_view(argv[1]).substr(10);
        if (mode == "fast64" || mode == "fast128") {
            test_profiled_short_scheduler_batches(true, false, "fast", mode == "fast64" ? 64 : 128);
        } else if (mode == "fast64-prefill64") {
            test_profiled_short_scheduler_batches(true, false, "fast", 64, 64);
        } else if (mode == "moderate" || mode == "safe") {
            test_profiled_short_scheduler_batches(true, false, mode);
        } else {
            return 2;
        }
        if (failures == 0) std::cout << "Jam2 profile timing tests passed\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--callback-runway") {
        test_exactly_four_peer_partial_blocks_preserve_audio_and_bounds();
        test_recovery_padding_waits_for_receive_batch();
        test_exactly_four_peer_partial_blocks_reach_real_callbacks();
        test_exactly_four_peer_worker_hold_uses_real_partial_runway();
        test_exactly_four_peer_recovery_releases_real_partial_runway();
        if (failures == 0) std::cout << "Jam2 callback runway tests passed\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--release-return") {
        test_profiled_short_scheduler_batches(true, true);
        if (failures == 0) std::cout << "Jam2 release-return tests passed\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--low-skew") {
        test_profiled_short_scheduler_batches(true);
        if (failures == 0) std::cout << "Jam2 low-skew playback tests passed\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc != 1) return 2;
    test_midi_queue();
    test_midi_block_mapping();
    test_mpe_validation();
    test_source_router();
    test_instrument_failure_is_silence();
    test_selected_recording_source_is_independent_of_send_mix();
    test_excluded_midi_source_keeps_draining();
    measure_single_source_router_cost();
    test_playback_count_in_commits_without_control_toggle();
    test_headless_send_peak_follows_gain();
    test_headless_injected_audio_uses_input_router();
    test_current_transport_packet_contract();
    test_peer_mixer_recovery_rebases_every_source();
    test_peer_mixer_global_gap_discards_obsolete_timeline();
    test_peer_mixer_recovery_waits_for_256_frame_debt();
    test_exactly_four_peer_multi_wake_catchup_returns_to_live_latency();
    test_exactly_four_peer_trickled_catchup_stays_at_live_latency();
    test_exactly_four_peer_receive_burst_obeys_adaptive_path_target();
    test_exactly_four_peer_adaptive_off_uses_fixed_playout_target();
    test_exactly_four_peer_recovery_completes_during_continuous_batching();
    test_exactly_four_peer_partial_blocks_preserve_audio_and_bounds();
    test_recovery_padding_waits_for_receive_batch();
    test_exactly_four_peer_partial_blocks_reach_real_callbacks();
    test_exactly_four_peer_worker_hold_uses_real_partial_runway();
    test_exactly_four_peer_recovery_releases_real_partial_runway();
    test_peer_mixer_batches_wrapped_queue_operations();
    test_peer_mixer_adapts_to_device_ring_underrun();
    test_peer_mixer_burst_bound_preserves_adaptive_recovery();
    test_peer_mixer_periodic_batches_do_not_accelerate_into_underruns();
    test_four_peer_boundary_hold_preserves_phase_and_bounds();
    test_isolated_recovery_escalates_repeated_shortages();
    test_four_peer_worker_margin_tracks_service_gaps();
    test_delivery_gap_learning_evidence_is_narrow();
    test_profiled_short_scheduler_batches();
    test_adaptive_release_retires_real_surplus_without_trimming();
    test_source_rounding_edge_is_fixed_and_bounded();
    test_four_peer_rebase_preserves_unexpired_delivery_learning();
    test_four_peer_heterogeneous_gap_evidence();
    test_four_peer_gap_expiry_and_departure();
    test_four_peer_packet_gap_association();
    test_profiled_wifi_return_to_clean();
    test_profiled_recurring_wifi_bursts_match_observed_session();
    if (failures == 0) std::cout << "Jam2 core input tests passed\n";
    return failures == 0 ? 0 : 1;
}
