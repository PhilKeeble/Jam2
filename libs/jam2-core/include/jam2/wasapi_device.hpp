#pragma once
#include "audio_device.hpp"

namespace jam2::audio {
void append_wasapi_devices(std::vector<DeviceInfo>& devices);
DeviceTestResult test_wasapi_device(const DeviceInfo& device);
std::unique_ptr<DeviceStream> start_wasapi_stream(
    int id, double rate, long period, const WasapiConfig& config,
    MonoRingBuffer& capture, MonoRingBuffer& pitch, MonoRingBuffer& playback,
    std::size_t prefill, StreamControl& control, OutputRecorder* recorder,
    TrackTakeRecorder* takeRecorder);
} // namespace jam2::audio
