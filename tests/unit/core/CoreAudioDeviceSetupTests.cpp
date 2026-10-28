#include "coreaudio_device_setup.hpp"

#include <iostream>
#include <stdexcept>

using namespace jam2::audio;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "invalid CoreAudio device/channel selection must fail");
}
}
int main() {
    try {
        const std::vector<std::uint32_t> streams{2, 2, 1};
        require(coreaudio_stream_usage(std::vector<int>{3, 4}, streams) == std::vector<std::uint32_t>{0, 1, 1},
            "aggregate stream usage must enable only the selected physical device streams");
        require(coreaudio_stream_usage({}, streams) == std::vector<std::uint32_t>{0, 0, 0},
            "playback only must disable every input stream");
        rejects([&] { coreaudio_stream_usage(std::vector<int>{5}, streams); });
        rejects([&] { coreaudio_stream_usage(std::vector<int>{-1}, streams); });
        DeviceInfo output; output.backend = "CoreAudio"; output.clsid = "speakers";
        output.input_channel_count = 4; output.output_channel_count = 6;
        DeviceInfo input; input.backend = "CoreAudio"; input.clsid = "microphone";
        input.input_channel_count = 2; input.output_channel_count = 2;
        const std::vector<DeviceInfo> devices{output, input};
        CoreAudioConfig config; config.output_device_uid = "speakers";
        ChannelSelection channels{{1}, {0, 1}};
        auto plan = plan_coreaudio_devices(config, channels, devices);
        require(!plan.aggregate && plan.has_input && plan.stream_channels.input == channels.input,
            "single device must use its native layout without creating an aggregate");
        config.setup = AudioDeviceSetup::SeparateInputOutput; config.input_device_uid = "microphone";
        plan = plan_coreaudio_devices(config, channels, devices);
        require(plan.aggregate && plan.stream_channels.input == std::vector<int>{5} &&
            plan.device_channels.input == std::vector<int>{1} && plan.stream_channels.output == channels.output,
            "split device must offset capture past output-device inputs while preserving physical channel labels");
        config.input_device_uid = "speakers";
        plan = plan_coreaudio_devices(config, channels, devices);
        require(!plan.aggregate && plan.stream_channels.input == channels.input,
            "same input/output device must bypass aggregation even in separate mode");
        config.input_device_uid.clear();
        plan = plan_coreaudio_devices(config, channels, devices);
        require(!plan.aggregate && !plan.has_input && plan.stream_channels.input.empty(),
            "optional input must produce playback only without an aggregate");
        config.input_device_uid = "missing"; rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        config.input_device_uid = "microphone";
        channels.input = {2}; rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        channels.input = {0, 0}; rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        channels.input.clear(); rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        channels.input = {0}; channels.output = {6}; rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        channels.output = {-1}; rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        channels.output = {0}; config.output_device_uid = "missing";
        rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        config.output_device_uid = "speakers";
        auto noOutput = devices; noOutput[0].output_channel_count = 0;
        rejects([&] { plan_coreaudio_devices(config, channels, noOutput); });
        auto noInput = devices; noInput[1].input_channel_count = 0;
        rejects([&] { plan_coreaudio_devices(config, channels, noInput); });
        config.setup = static_cast<AudioDeviceSetup>(99);
        rejects([&] { plan_coreaudio_devices(config, channels, devices); });
        std::cout << "CoreAudio single/split planning, channel mapping and validation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
