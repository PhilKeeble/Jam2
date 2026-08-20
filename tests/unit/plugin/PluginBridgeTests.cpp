// Native unit coverage is owned by the repository-level test tree.
#include "PluginAudioBridge.hpp"
#include "PluginHostService.hpp"
#include "PluginSharedMemory.hpp"
#include "common.hpp"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);

    {
        const std::string token =
            "nativeplugintransport" + std::to_string(jam2::monotonic_us());
        auto owner = jam2::pluginhost::PluginSharedMemory::create(token);
        auto peer = jam2::pluginhost::PluginSharedMemory::open(token);
        owner.get()->heartbeat.store(73U, std::memory_order_release);
        jam2::pluginhost::PluginSharedMemory moved;
        moved = std::move(peer);
        if (!owner || !moved || peer ||
            moved.get()->heartbeat.load(std::memory_order_acquire) != 73U) {
            std::cerr << "plugin shared-memory create/open did not share state\n";
            return 1;
        }
    }

    {
        jam2::pluginhost::SharedState shared;
        jam2::pluginhost::PluginAudioBridge bridge(
            shared, 16, jam2::audio::InputSourceKind::Audio);
        bridge.set_bypassed(true);
        if (!bridge.bypassed()) {
            std::cerr << "VST3 bypass state was not observable\n";
            return 1;
        }
        bridge.set_bypassed(false);
        shared.plugin_latency_frames.store(7, std::memory_order_relaxed);
        shared.worker_input_peak_ppm.store(321U, std::memory_order_relaxed);
        shared.midi_events_consumed.store(4U, std::memory_order_relaxed);
        const auto diagnostics = bridge.stats();
        if (bridge.latency_frames(16) != 39 ||
            diagnostics.worker_input_peak_ppm != 321U ||
            diagnostics.midi_events_consumed != 4U) {
            std::cerr << "VST3 transport omitted latency, input peak, or MIDI diagnostics\n";
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
            stats.wet_output_peak_ppm < 249999U ||
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
        bridge.set_muted(true);
        bridge.set_muted(false);
        bridge.request_midi_reset();
        (void)queue.push({jam2::monotonic_us(), 0, 0x92, 64, 100, 3});
        std::array<std::int32_t, 16> rendered{};
        jam2::audio::InputSourceRenderRequest request;
        request.frames = rendered.size();
        request.sample_rate = 48000.0;
        (void)bridge.render_mono(request, rendered);
        const auto& published = shared.transport_blocks[1];
        if (published.midi_count != 33 || published.midi_live_count != 1 ||
            published.midi[32].status != 0x92 ||
            published.midi[32].data1 != 64) {
            std::cerr << "MIDI reset reservation corrupted the following live event\n";
            return 1;
        }
    }

    {
        // The no-hardware baseline still owns the complete idle and validation
        // lifecycle. Loading a real plugin worker is reserved for the explicit
        // hardware/plugin profile.
        jam2::pluginhost::PluginHostService service;
        service.moveProcessToThread(QThread::currentThread());
        service.openEditor();
        service.closeEditor();
        service.requestRetire();
        if (service.bridge() != nullptr || service.healthy() || service.editorOpen() ||
            !service.statusText().isEmpty() || !service.errorText().isEmpty() ||
            jam2::pluginhost::PluginHostService::workerExecutablePath().isEmpty()) {
            std::cerr << "idle plugin-host service exposed invalid state\n";
            return 1;
        }
        bool rejected = false;
        try {
            service.start({}, {}, 48000.0, 0,
                jam2::audio::InputSourceKind::Audio, 0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        service.stop();
        if (!rejected) {
            std::cerr << "plugin-host service accepted an invalid block size\n";
            return 1;
        }
    }

    std::cout << "Jam2 plugin bridge tests passed\n";
    return 0;
}
