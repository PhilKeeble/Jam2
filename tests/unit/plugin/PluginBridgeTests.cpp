// Native unit coverage is owned by the repository-level test tree.
#include "PluginAudioBridge.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>

int main()
{
    {
        jam2::pluginhost::SharedState shared;
        jam2::pluginhost::PluginAudioBridge bridge(
            shared, 16, jam2::audio::InputSourceKind::Audio);
        shared.plugin_latency_frames.store(7, std::memory_order_relaxed);
        if (bridge.latency_frames(16) != 39) {
            std::cerr << "VST3 recording latency omitted isolation or worker latency\n";
            return 1;
        }
        std::array<std::int32_t, 16> silence{};
        std::array<std::int32_t, 16> rendered{};
        jam2::audio::InputSourceRenderRequest request;
        request.inputs[0] = silence.data();
        request.input_channels = 1;
        request.frames = silence.size();
        request.sample_rate = 48000.0;
        (void)bridge.render_mono(request, rendered);
        request.engine_frame += request.frames;
        (void)bridge.render_mono(request, rendered);
        request.engine_frame += request.frames;
        (void)bridge.render_mono(request, rendered);
        auto& late = shared.transport_blocks[1];
        late.response_generation.store(1U, std::memory_order_release);
        late.response_frames = static_cast<std::uint32_t>(rendered.size());
        late.output_channels = 1;
        late.process_ok = 1;
        std::fill_n(late.output[0].begin(), rendered.size(), 1.0f);
        late.response_generation.store(2U, std::memory_order_release);
        request.engine_frame += request.frames;
        (void)bridge.render_mono(request, rendered);
        if (std::any_of(rendered.begin(), rendered.end(),
                [](std::int32_t sample) { return sample != 0; })) {
            std::cerr << "late VST3 response leaked into a newer audio block\n";
            return 1;
        }
    }

    {
        jam2::pluginhost::SharedState shared;
        jam2::pluginhost::PluginAudioBridge bridge(
            shared, 16, jam2::audio::InputSourceKind::Audio);
        std::array<std::int32_t, 16> silence{};
        std::array<std::int32_t, 16> rendered{};
        jam2::audio::InputSourceRenderRequest request;
        request.inputs[0] = silence.data();
        request.input_channels = 1;
        request.frames = silence.size();
        request.sample_rate = 48000.0;
        (void)bridge.render_mono(request, rendered);
        request.engine_frame += request.frames;
        (void)bridge.render_mono(request, rendered);

        auto& ready = shared.transport_blocks[1];
        ready.response_generation.store(1U, std::memory_order_release);
        ready.response_frames = static_cast<std::uint32_t>(rendered.size());
        ready.output_channels = 1;
        ready.process_ok = 1;
        std::fill_n(ready.output[0].begin(), rendered.size(), 0.25f);
        ready.response_generation.store(2U, std::memory_order_release);
        request.engine_frame += request.frames;
        (void)bridge.render_mono(request, rendered);
        const std::int32_t last_wet_sample = rendered.back();

        request.engine_frame += request.frames;
        (void)bridge.render_mono(request, rendered);
        const auto stats = bridge.stats();
        if (stats.deadline_misses != 1 || stats.deadline_concealments != 1 ||
            !std::all_of(rendered.begin(), rendered.end(),
                [last_wet_sample](std::int32_t sample) {
                    return sample == last_wet_sample;
                })) {
            std::cerr << "single VST3 deadline miss was not continuity-concealed\n";
            return 1;
        }
    }

    {
        jam2::pluginhost::SharedState shared;
        jam2::pluginhost::PluginAudioBridge bridge(
            shared, 16, jam2::audio::InputSourceKind::MidiInstrument);
        jam2::midi::EventQueue queue;
        bridge.set_midi_queue(&queue);
        bridge.request_midi_reset();
        (void)queue.push({jam2::monotonic_us(), 0, 0x92, 64, 100, 3});
        std::array<std::int32_t, 16> rendered{};
        jam2::audio::InputSourceRenderRequest request;
        request.frames = rendered.size();
        request.sample_rate = 48000.0;
        (void)bridge.render_mono(request, rendered);
        const auto& published = shared.transport_blocks[1];
        if (published.midi_count != 33 || published.midi[32].status != 0x92 ||
            published.midi[32].data1 != 64) {
            std::cerr << "MIDI reset reservation corrupted the following live event\n";
            return 1;
        }
    }

    std::cout << "Jam2 plugin bridge tests passed\n";
    return 0;
}
