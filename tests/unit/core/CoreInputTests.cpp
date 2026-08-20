#include "common.hpp"
#include "input_source.hpp"
// Native unit coverage is owned by the repository-level test tree.
#include "engine.hpp"
#include "midi.hpp"
#include "peer_mixer.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
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

    std::size_t depth = 0;
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
    test_playback_count_in_commits_without_control_toggle();
    test_headless_send_peak_follows_gain();
    test_headless_injected_audio_uses_input_router();
    test_current_transport_packet_contract();
    test_peer_mixer_recovery_rebases_every_source();
    if (failures == 0) std::cout << "Jam2 core input tests passed\n";
    return failures == 0 ? 0 : 1;
}
