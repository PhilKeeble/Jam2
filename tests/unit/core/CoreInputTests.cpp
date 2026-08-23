#include "common.hpp"
#include "input_source.hpp"
// Native unit coverage is owned by the repository-level test tree.
#include "audio_device_processing.hpp"
#include "engine.hpp"
#include "midi.hpp"
#include "peer_mixer.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <deque>
#include <iostream>
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
    engine.start(config);

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

void test_exactly_four_peer_multi_wake_catchup_returns_to_live_latency()
{
    LiveTailMixerSink sink;
    jam2::PeerMixerConfig config;
    config.sample_rate = 48000;
    config.frames_per_block = 64;
    config.deadline_frames = 64;
    config.output_max_frames = 1536;
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
    expect(sink.depthFrames() <= 128,
        "catch-up completion also returns the device-facing ring to the live tail");
    expect(totalPeerDepth + sink.depthFrames() <= peerIds.size() * 128U + 128U,
        "exactly-four-peer recovery bounds the complete receiver path instead of retaining burst latency");
    expect(peakCatchupIngestDepth >
            peerIds.size() * 128U + 128U,
        "the multi-wake fixture contains measurable stale latency before final recovery");
    expect(mixer.stats().output_drop_requested_frames > 0,
        "final recovery reports the stale device-facing frames it discarded");
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

void test_peer_mixer_release_does_not_replace_drained_audio_with_silence()
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

    // Release is reserved for recovery cushion that was explicitly raised by
    // a real device underrun. A normal packet burst at the minimum target must
    // not engage fast playback.
    sink.underruns += 64;
    peer->pushFrames(block);
    mixer.advance(1000);
    expect(mixer.stats().adaptive_target_frames == 320,
        "an underrun establishes recovery cushion before bounded release");

    const std::array<std::int32_t, 2048> burst{};
    peer->pushFrames(burst);
    mixer.advance(2000);
    expect(sink.resamplerRatio > 1.0049,
        "excess recovery cushion activates the bounded release ratio");

    const std::uint64_t paddingBeforeRelease =
        mixer.stats().adaptive_padding_frames;
    sink.consume(sink.depth - 512);
    mixer.advance(3000);
    expect(mixer.stats().adaptive_padding_frames == paddingBeforeRelease,
        "active depth release does not replace deliberately drained frames with silence");

    peer->pushFrames(block);
    mixer.advance(4000);
    expect(mixer.stats().adaptive_padding_frames == paddingBeforeRelease,
        "a newly received real block is not followed by synthetic cushion silence");
    expect(sink.resamplerRatio == 1.0,
        "release returns to unity once real playback depth reaches its stop band");

    sink.consume(sink.depth + 64);
    mixer.advance(5000);
    expect(mixer.stats().adaptive_target_frames == 384 &&
            mixer.stats().adaptive_raise_events == 2,
        "an actual device underrun still raises the adaptive target");
    expect(mixer.stats().adaptive_padding_frames > paddingBeforeRelease,
        "an actual device underrun still permits bounded recovery padding");
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

    std::array<std::int32_t, 4096> burst{};
    std::fill(burst.begin(), burst.end(), 100000000);
    std::uint64_t nowUs = 1000000;
    peer->pushFrames(burst);
    mixer.advance(nowUs);
    const std::uint64_t startupPadding = mixer.stats().adaptive_padding_frames;
    bool targetStayedAtUnity =
        sink.control.playback_ratio_ppm.load(std::memory_order_relaxed) == 1000000;
    for (std::uint64_t cycle = 0; cycle < 32; ++cycle) {
        if (cycle > 0) {
            peer->pushFrames(burst);
            mixer.advance(nowUs);
        }
        for (int callback = 0; callback < 128; ++callback) {
            sink.callback();
            nowUs += 667;
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

} // namespace

int main()
{
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
    test_exactly_four_peer_multi_wake_catchup_returns_to_live_latency();
    test_peer_mixer_batches_wrapped_queue_operations();
    test_peer_mixer_adapts_to_device_ring_underrun();
    test_peer_mixer_release_does_not_replace_drained_audio_with_silence();
    test_peer_mixer_periodic_batches_do_not_accelerate_into_underruns();
    if (failures == 0) std::cout << "Jam2 core input tests passed\n";
    return failures == 0 ? 0 : 1;
}
