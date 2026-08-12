#include "Vst3Host.hpp"
#include "PluginSharedMemory.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <ole2.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

std::string argument(int index, int argc, char** argv)
{
    return index < argc ? std::string(argv[index]) : std::string{};
}

int probe(const std::string& path)
{
    for (const auto& plugin : jam2::pluginhost::scan_vst3(path)) {
        std::cout << plugin.class_id << '\t' << plugin.name << '\t' << plugin.vendor
                  << '\t' << plugin.version << '\t' << plugin.subcategories << '\n';
    }
    return 0;
}

int probe_all(const std::string& path)
{
    for (const auto& plugin : jam2::pluginhost::scan_vst3_factory_classes(path)) {
        std::cout << plugin.category << " [bytes=" << plugin.category.size() << "]\t"
                  << plugin.class_id << '\t' << plugin.name
                  << '\t' << plugin.vendor << '\t' << plugin.version
                  << '\t' << plugin.subcategories << '\n';
    }
    return 0;
}

int probe_file(const std::string& path, const std::string& output_path)
{
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not create plugin probe result");
    for (const auto& plugin : jam2::pluginhost::scan_vst3(path)) {
        output << plugin.class_id << '\t' << plugin.name << '\t' << plugin.vendor
               << '\t' << plugin.version << '\t' << plugin.subcategories << '\n';
    }
    if (!output) throw std::runtime_error("Could not write plugin probe result");
    return 0;
}

int self_test(const std::string& path)
{
    const auto plugins = jam2::pluginhost::scan_vst3(path);
    if (plugins.empty()) throw std::runtime_error("No audio-effect classes found");
    for (const auto& plugin : plugins) {
        jam2::pluginhost::Vst3Instance instance;
        instance.load(path, plugin.class_id);
        instance.configure(48000.0, 256, instance.input_channels() == 0 ? 0U : 1U);
        std::vector<float> left(256, 0.0f);
        std::vector<float> right(256, 0.0f);
        std::vector<float> out_left(256, 0.0f);
        std::vector<float> out_right(256, 0.0f);
        if (!instance.process(left, right, {}, out_left, out_right))
            throw std::runtime_error("VST3 process call failed for " + plugin.name);
        instance.reset();
        std::cout << "OK\t" << plugin.name << '\n';
    }
    return 0;
}

void set_status(jam2::pluginhost::SharedState& state, const std::string& text)
{
    const auto length = (std::min)(text.size(), state.status_text.size() - 1U);
    std::copy_n(text.data(), length, state.status_text.data());
    state.status_text[length] = '\0';
}

int run_worker(int argc, char** argv)
{
    if (argc < 8) throw std::invalid_argument("Incomplete plugin runtime arguments");
    const std::string token = argument(2, argc, argv);
    const std::string path = argument(3, argc, argv);
    const std::string class_id = argument(4, argc, argv);
    const double sample_rate = std::stod(argument(5, argc, argv));
    const auto maximum_frames = static_cast<std::size_t>(std::stoul(argument(6, argc, argv)));
    const auto source_input_channels = static_cast<std::size_t>(std::stoul(argument(7, argc, argv)));
    if (maximum_frames == 0 || maximum_frames > jam2::pluginhost::kMaximumFrames)
        throw std::invalid_argument("Invalid plugin runtime block size");

    auto memory = jam2::pluginhost::PluginSharedMemory::open(token);
    auto& state = *memory.get();
    try {
#ifdef _WIN32
        (void)SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
        jam2::pluginhost::Vst3Instance plugin;
        plugin.load(path, class_id);
        plugin.configure(sample_rate, maximum_frames, source_input_channels);
        if (plugin.latency_samples() > jam2::pluginhost::kMaximumPluginLatencyFrames)
            throw std::runtime_error("Plugin latency exceeds Jam2's two-second safety limit");
        std::array<std::array<float, jam2::pluginhost::kMaximumFrames>, 2> input{};
        std::array<std::array<float, jam2::pluginhost::kMaximumFrames>, 2> output{};
        std::array<jam2::pluginhost::MidiMessage, jam2::pluginhost::kMaximumMidiEvents> midi{};
        const auto warm_left = source_input_channels > 0
            ? std::span<const float>(input[0].data(), maximum_frames)
            : std::span<const float>{};
        const auto warm_right = source_input_channels > 1
            ? std::span<const float>(input[1].data(), maximum_frames)
            : std::span<const float>{};
        for (unsigned block = 0; block < 4; ++block) {
            if (!plugin.process(warm_left, warm_right, {},
                    std::span<float>(output[0].data(), maximum_frames),
                    std::span<float>(output[1].data(), maximum_frames))) {
                throw std::runtime_error("VST3 warm-up process call failed");
            }
        }
        state.plugin_latency_frames.store(plugin.latency_samples(), std::memory_order_release);
        state.negotiated_input_channels.store(
            static_cast<std::uint32_t>(plugin.input_channels()), std::memory_order_release);
        state.negotiated_output_channels.store(
            static_cast<std::uint32_t>(plugin.output_channels()), std::memory_order_release);
        set_status(state, plugin.description().name);
        state.worker_state.store(jam2::pluginhost::WorkerState::Ready, std::memory_order_release);
        std::thread audio_thread([&] {
#ifdef _WIN32
            (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__APPLE__)
            (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            std::uint64_t last_sequence = 0;
            while (!state.shutdown.load(std::memory_order_acquire)) {
                jam2::pluginhost::TransportSlot* selected = nullptr;
                std::uint64_t sequence = 0;
                for (auto& slot : state.transport_blocks) {
                    const auto generation = slot.request_generation.load(std::memory_order_acquire);
                    const std::uint64_t candidate = generation / 2U;
                    // Drain pending requests in timeline order. Choosing the
                    // newest request here discarded a still-viable older block
                    // whenever Windows briefly delayed this process, turning a
                    // recoverable scheduling wobble into an audible fallback.
                    if ((generation & 1U) == 0U && candidate > last_sequence &&
                        (selected == nullptr || candidate < sequence)) {
                        selected = &slot;
                        sequence = candidate;
                    }
                }
                if (!selected) {
                    // A Windows sleep can exceed an entire 32-frame callback.
                    // This is a dedicated high-priority process, so yield-spin
                    // while active and let the OS schedule the ASIO callback.
                    std::this_thread::yield();
                    continue;
                }
                const auto before = selected->request_generation.load(std::memory_order_acquire);
                const std::size_t frames = selected->frames;
                const std::size_t input_channels = selected->input_channels;
                const std::size_t midi_count = selected->midi_count;
                if ((before & 1U) != 0U || frames == 0 || frames > maximum_frames ||
                    input_channels > 2 || midi_count > midi.size()) continue;
                for (std::size_t channel = 0; channel < 2; ++channel)
                    std::copy_n(selected->input[channel].begin(), frames, input[channel].begin());
                for (std::size_t index = 0; index < midi_count; ++index) {
                    const auto& source = selected->midi[index];
                    midi[index] = {source.sample_offset, source.status, source.data1, source.data2};
                }
                if (selected->request_generation.load(std::memory_order_acquire) != before) continue;

                const auto left = input_channels > 0
                    ? std::span<const float>(input[0].data(), frames) : std::span<const float>{};
                const auto right = input_channels > 1
                    ? std::span<const float>(input[1].data(), frames) : std::span<const float>{};
                const auto process_started = std::chrono::steady_clock::now();
                const bool ok = plugin.process(left, right,
                    std::span<const jam2::pluginhost::MidiMessage>(midi.data(), midi_count),
                    std::span<float>(output[0].data(), frames),
                    std::span<float>(output[1].data(), frames));
                const auto process_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - process_started).count());
                state.process_time_last_us.store(process_us, std::memory_order_relaxed);
                state.process_time_sum_us.fetch_add(process_us, std::memory_order_relaxed);
                std::uint64_t maximum = state.process_time_max_us.load(std::memory_order_relaxed);
                while (process_us > maximum && !state.process_time_max_us.compare_exchange_weak(
                    maximum, process_us, std::memory_order_relaxed)) {}

                selected->response_generation.store(sequence * 2U - 1U, std::memory_order_release);
                selected->response_frames = static_cast<std::uint32_t>(frames);
                selected->output_channels = static_cast<std::uint32_t>(plugin.output_channels());
                selected->process_ok = ok ? 1U : 0U;
                for (std::size_t channel = 0; channel < 2; ++channel)
                    std::copy_n(output[channel].begin(), frames, selected->output[channel].begin());
                selected->response_generation.store(sequence * 2U, std::memory_order_release);
                if (ok) state.processed_blocks.fetch_add(1, std::memory_order_relaxed);
                else state.failed_blocks.fetch_add(1, std::memory_order_relaxed);
                state.plugin_latency_frames.store(plugin.latency_samples(), std::memory_order_relaxed);
                state.heartbeat.fetch_add(1, std::memory_order_relaxed);
                last_sequence = sequence;
            }
        });

        // Native editor work stays on the worker's main thread. The high-
        // priority audio thread above never pumps windows, blocks on GUI work,
        // or waits for the editor, matching the separation used by VST hosts.
        while (!state.shutdown.load(std::memory_order_acquire)) {
            const std::uint32_t editor_command =
                state.editor_command.exchange(0U, std::memory_order_acq_rel);
            if (editor_command == 1U) {
                const bool opened = plugin.open_editor();
                if (!opened) set_status(state, plugin.editor_error());
                state.editor_state.store(opened ? 1U : 2U, std::memory_order_release);
            } else if (editor_command == 2U) {
                plugin.close_editor();
                state.editor_state.store(0U, std::memory_order_release);
            }
            plugin.pump_editor();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (audio_thread.joinable()) audio_thread.join();
        plugin.close_editor();
        plugin.reset();
        state.worker_state.store(jam2::pluginhost::WorkerState::Stopped, std::memory_order_release);
        return 0;
    } catch (const std::exception& error) {
        set_status(state, error.what());
        state.worker_state.store(jam2::pluginhost::WorkerState::Failed, std::memory_order_release);
        throw;
    }
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    struct ScopedOle final {
        ScopedOle() : initialized(SUCCEEDED(OleInitialize(nullptr))) {}
        ~ScopedOle() { if (initialized) OleUninitialize(); }
        bool initialized = false;
    } ole;
#endif
    try {
        const std::string command = argument(1, argc, argv);
        const std::string path = argument(2, argc, argv);
        if (command == "--run") return run_worker(argc, argv);
        if (command == "--probe-file" && argc >= 4)
            return probe_file(path, argument(3, argc, argv));
        if ((command != "--probe" && command != "--probe-all" &&
             command != "--self-test") || path.empty()) {
            std::cerr << "usage: jam2-plugin-worker --probe|--probe-all|--self-test <plugin.vst3>\n"
                         "       jam2-plugin-worker --run <token> <plugin.vst3> <class> <rate> <frames> <source-channels>\n";
            return 2;
        }
        if (command == "--probe") return probe(path);
        if (command == "--probe-all") return probe_all(path);
        return self_test(path);
    } catch (const std::exception& error) {
        std::cerr << "plugin-worker error: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "plugin-worker error: unknown exception\n";
        return 1;
    }
}
