#include "PluginHostService.hpp"

#include "engine.hpp"
#include "input_source.hpp"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int integer_argument(char** argv, int index, int fallback)
{
    return argv[index] != nullptr ? std::stoi(argv[index]) : fallback;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: jam2_plugin_device_tests <plugin.vst3> "
                     "[device-id=5] [input-channel=2] [frames=32]\n";
        return 2;
    }

    try {
        const int deviceId = argc >= 3 ? integer_argument(argv, 2, 5) : 5;
        const int userInputChannel = argc >= 4 ? integer_argument(argv, 3, 2) : 2;
        const long blockFrames = argc >= 5 ? integer_argument(argv, 4, 32) : 32;
        if (userInputChannel < 1 || blockFrames < 1) {
            throw std::invalid_argument("Input channels are one-based and frames must be positive");
        }

        std::cerr << "phase: start isolated plugin worker\n";
        jam2::pluginhost::PluginHostService plugin;
        plugin.start(argv[1], {}, 48000.0, static_cast<std::size_t>(blockFrames),
            jam2::audio::InputSourceKind::Audio, 1U);
        if (!plugin.healthy() || plugin.bridge() == nullptr) {
            throw std::runtime_error("Plugin worker did not become healthy");
        }

        std::cerr << "phase: configure input router\n";
        jam2::audio::InputSourceRouter router(
            static_cast<std::size_t>(blockFrames), 1U);
        jam2::audio::InputSourceConfiguration source;
        source.kind = jam2::audio::InputSourceKind::Audio;
        source.first_channel = 0;
        source.enabled = true;
        source.renderer = plugin.bridge();
        if (!router.configure(0, source)) {
            throw std::runtime_error("Could not configure the plugin input source");
        }

        std::cerr << "phase: start audio device\n";
        jam2::Engine engine;
        jam2::EngineConfig config;
        config.audio_device_id = deviceId;
        config.sample_rate = 48000;
        config.audio_buffer_frames = blockFrames;
        config.input_channels = jam2::audio::InputChannels::Mono;
        config.channels.input = {userInputChannel - 1};
        config.channels.output = {0, 1};
        config.diagnostics_enabled = true;
        config.local_monitor_enabled = false;
        config.input_source_router = &router;
        engine.start(config);

        std::cerr << "phase: run real callbacks for five seconds\n";
        plugin.openEditor();
        int maximumInputPeak = 0;
        int maximumSendPeak = 0;
        int maximumRouterPeak = 0;
        std::uint64_t missesAtHalf = 0;
        bool sampledHalf = false;
        const auto started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            QCoreApplication::processEvents();
            const auto engineStats = engine.snapshot();
            const auto routerStats = router.stats();
            maximumInputPeak = std::max(maximumInputPeak, engineStats.input_peak_ppm);
            maximumSendPeak = std::max(maximumSendPeak, engineStats.send_peak_ppm);
            maximumRouterPeak = std::max(maximumRouterPeak, routerStats.peak_ppm);
            if (!sampledHalf &&
                std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(2500)) {
                missesAtHalf = plugin.bridge()->stats().deadline_misses;
                sampledHalf = true;
            }
            QThread::msleep(10);
        }

        std::cerr << "phase: collect and stop\n";
        const auto engineStats = engine.snapshot();
        const auto routerStats = router.stats();
        const auto pluginStats = plugin.bridge()->stats();
        plugin.closeEditor();
        engine.requestStop();
        engine.join();
        plugin.stop();

        if (engineStats.callbacks == 0 || routerStats.rendered_blocks == 0 ||
            pluginStats.completed_blocks == 0) {
            throw std::runtime_error("The real device/plugin callback path did not run");
        }

        std::cout << "Jam2 real-device plugin test passed; device=" << deviceId
                  << " input=" << userInputChannel
                  << " frames=" << engineStats.audio_buffer_frames
                  << " callbacks=" << engineStats.callbacks
                  << " input_peak_ppm=" << maximumInputPeak
                  << " send_peak_ppm=" << maximumSendPeak
                  << " plugin_peak_ppm=" << maximumRouterPeak
                  << " plugin_completed=" << pluginStats.completed_blocks
                  << " plugin_misses=" << pluginStats.deadline_misses
                  << " steady_half_misses=" <<
                         (pluginStats.deadline_misses - missesAtHalf)
                  << " callback_gaps(1.1x/1.5x/2x)="
                  << engineStats.callback_timing.gap_over_1_1x_count << '/'
                  << engineStats.callback_timing.gap_over_1_5x_count << '/'
                  << engineStats.callback_timing.gap_over_2x_count
                  << " process_us(last/avg/max)=" << pluginStats.worker_process_last_us << '/'
                  << pluginStats.worker_process_average_us << '/'
                  << pluginStats.worker_process_max_us << '\n';
        return pluginStats.deadline_misses == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
