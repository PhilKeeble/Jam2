#pragma once

#include "audio_device.hpp"

namespace jam2::audio {

// Channel numbers remain relative to each selected physical device in the UI.
// The aggregate stacks output first, then input, in both stream directions.
struct CoreAudioDevicePlan {
    DeviceInfo output;
    DeviceInfo input;
    bool has_input = false;
    bool aggregate = false;
    ChannelSelection device_channels;
    ChannelSelection stream_channels;
};

CoreAudioDevicePlan plan_coreaudio_devices(const CoreAudioConfig& config,
    const ChannelSelection& channels, std::span<const DeviceInfo> devices);

std::vector<std::uint32_t> coreaudio_stream_usage(std::span<const int> selected,
    std::span<const std::uint32_t> stream_channel_counts);

} // namespace jam2::audio
