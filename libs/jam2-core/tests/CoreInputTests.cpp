#include "input_source.hpp"
#include "midi.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>

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
    if (failures == 0) std::cout << "Jam2 core input tests passed\n";
    return failures == 0 ? 0 : 1;
}
