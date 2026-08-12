#include "PluginHostService.hpp"
#include "common.hpp"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: jam2_plugin_worker_tests <plugin.vst3> [audio|midi] "
                     "[frames] [class-id]\n";
        return 2;
    }
    try {
        jam2::pluginhost::PluginHostService service;
        const bool instrument = argc >= 3 && std::string(argv[2]) == "midi";
        const std::size_t blockFrames = argc >= 4
            ? static_cast<std::size_t>(std::stoul(argv[3])) : 256U;
        const std::string classId = argc == 5 ? argv[4] : std::string{};
        service.start(argv[1], classId, 48000.0, blockFrames,
            instrument ? jam2::audio::InputSourceKind::MidiInstrument
                       : jam2::audio::InputSourceKind::Audio,
            instrument ? 0U : 1U);
        if (!service.healthy() || !service.bridge()) {
            std::cerr << "worker did not become healthy\n";
            return 1;
        }
        std::vector<std::int32_t> input(blockFrames, 0);
        std::vector<std::int32_t> output(blockFrames, 0);
        if (!instrument) {
            for (std::size_t frame = 0; frame < input.size(); ++frame) {
                const double phase = 2.0 * 3.14159265358979323846 * 220.0 *
                    static_cast<double>(frame) / 48000.0;
                input[frame] = static_cast<std::int32_t>(
                    std::sin(phase) * 0.05 * 2147483647.0);
            }
        }
        jam2::midi::EventQueue midi;
        if (instrument) {
            service.bridge()->set_midi_queue(&midi);
            jam2::midi::Event note;
            note.monotonic_us = jam2::monotonic_us();
            note.status = 0x90;
            note.data1 = 60;
            note.data2 = 96;
            note.size = 3;
            (void)midi.push(note);
        }
        const std::int32_t* input_pointer = input.data();
        jam2::audio::InputSourceRenderRequest request;
        request.inputs[0] = input_pointer;
        request.input_channels = instrument ? 0U : 1U;
        request.frames = input.size();
        request.sample_rate = 48000.0;
        service.openEditor();
        for (int attempt = 0; attempt < 100 && !service.editorOpen(); ++attempt)
            QThread::msleep(10);
        if (!service.editorOpen()) {
            std::cerr << "plugin editor could not be opened in the worker: "
                      << service.statusText().toStdString() << '\n';
            return 1;
        }
        std::uint64_t missesAtHalf = 0;
        const std::uint64_t processedBlocks = (48000ULL * 5ULL) / blockFrames;
        const auto callbackPeriod = std::chrono::duration<double>(
            static_cast<double>(blockFrames) / 48000.0);
        auto nextCallback = std::chrono::steady_clock::now();
        for (std::uint64_t block = 0; block < processedBlocks; ++block) {
            while (std::chrono::steady_clock::now() < nextCallback)
                std::this_thread::yield();
            nextCallback += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                callbackPeriod);
            request.engine_frame = block * input.size();
            if (!service.bridge()->render_mono(request, output)) {
                std::cerr << "bridge rejected a valid block\n";
                return 1;
            }
            if (block + 1U == processedBlocks / 2U) {
                missesAtHalf = service.bridge()->stats().deadline_misses;
            }
        }
        const auto stats = service.bridge()->stats();
        if (stats.submitted_blocks != processedBlocks || stats.completed_blocks == 0) {
            std::cerr << "worker did not complete transported blocks: submitted="
                      << stats.submitted_blocks << " completed=" << stats.completed_blocks << '\n';
            return 1;
        }
        service.bridge()->set_bypassed(true);
        if (!service.bridge()->render_mono(request, output)) {
            std::cerr << "bypass failed\n";
            return 1;
        }
        if (!service.editorOpen()) {
            std::cerr << "plugin editor could not be opened in the worker: "
                      << service.statusText().toStdString() << '\n';
            return 1;
        }
        service.closeEditor();
        QThread::msleep(50);
        service.stop();
        const std::uint64_t steadyMisses = stats.deadline_misses - missesAtHalf;
        std::cout << "Jam2 isolated plugin worker test completed; completed="
                  << stats.completed_blocks << " deadline_misses=" << stats.deadline_misses
                  << " steady_half_misses=" << steadyMisses
                  << " frames=" << blockFrames
                  << " process_us(last/avg/max)=" << stats.worker_process_last_us << '/'
                  << stats.worker_process_average_us << '/' << stats.worker_process_max_us << '\n';
        return steadyMisses == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
