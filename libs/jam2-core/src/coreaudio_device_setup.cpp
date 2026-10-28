#include "coreaudio_device_setup.hpp"

#include <algorithm>
#include <stdexcept>

namespace jam2::audio {
std::vector<std::uint32_t> coreaudio_stream_usage(std::span<const int> selected,
    std::span<const std::uint32_t> stream_channel_counts)
{
    std::vector<std::uint32_t> usage(stream_channel_counts.size(), 0);
    for (const int channel : selected) {
        if (channel < 0) throw std::runtime_error("invalid CoreAudio stream channel");
        std::size_t offset = 0;
        bool found = false;
        for (std::size_t i = 0; i < stream_channel_counts.size(); ++i) {
            if (static_cast<std::size_t>(channel) >= offset && static_cast<std::size_t>(channel) - offset < stream_channel_counts[i]) {
                usage[i] = 1; found = true; break;
            }
            offset += stream_channel_counts[i];
        }
        if (!found) throw std::runtime_error("selected CoreAudio channel has no stream");
    }
    return usage;
}

CoreAudioDevicePlan plan_coreaudio_devices(const CoreAudioConfig& config,
    const ChannelSelection& channels, std::span<const DeviceInfo> devices)
{
    if (config.setup != AudioDeviceSetup::SingleDevice && config.setup != AudioDeviceSetup::SeparateInputOutput)
        throw std::runtime_error("invalid CoreAudio device setup");
    const auto find = [&](const std::string& uid) -> DeviceInfo {
        for (const auto& device : devices)
            if (!uid.empty() && device.backend == "CoreAudio" && stable_device_id(device) == uid) return device;
        throw std::runtime_error("selected CoreAudio device is unavailable: " + uid);
    };
    CoreAudioDevicePlan plan;
    plan.output = find(config.output_device_uid);
    plan.has_input = config.setup == AudioDeviceSetup::SingleDevice || !config.input_device_uid.empty();
    if (plan.has_input) plan.input = config.setup == AudioDeviceSetup::SingleDevice ? plan.output : find(config.input_device_uid);
    plan.aggregate = plan.has_input && stable_device_id(plan.input) != stable_device_id(plan.output);
    plan.device_channels = channels;
    if (!plan.has_input) plan.device_channels.input.clear();
    const auto validate = [](const std::vector<int>& selected, std::size_t count, const char* direction) {
        if (selected.empty() || count == 0 || count > 1024) throw std::runtime_error(std::string("select valid CoreAudio ") + direction + " channels");
        for (std::size_t i = 0; i < selected.size(); ++i)
            if (selected[i] < 0 || static_cast<std::size_t>(selected[i]) >= count ||
                std::find(selected.begin(), selected.begin() + i, selected[i]) != selected.begin() + i)
                throw std::runtime_error(std::string("invalid CoreAudio ") + direction + " channel selection");
    };
    validate(plan.device_channels.output, plan.output.output_channel_count, "output");
    if (plan.has_input) validate(plan.device_channels.input, plan.input.input_channel_count, "input");
    plan.stream_channels = plan.device_channels;
    if (plan.aggregate) {
        if (plan.output.input_channel_count > 1024) throw std::runtime_error("invalid CoreAudio aggregate channel layout");
        for (auto& channel : plan.stream_channels.input) channel += static_cast<int>(plan.output.input_channel_count);
    }
    return plan;
}
} // namespace jam2::audio
