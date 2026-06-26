#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio_ring.hpp"

namespace jam2::audio {

struct DeviceInfo {
    int id = 0;
    std::string backend;
    std::string name;
    std::string clsid;
    std::string driver_path;
};

struct DeviceProbe {
    DeviceInfo device;
    std::string driver_name;
    long driver_version = 0;
    long input_channels = 0;
    long output_channels = 0;
    long input_latency_samples = 0;
    long output_latency_samples = 0;
    long min_buffer_size = 0;
    long max_buffer_size = 0;
    long preferred_buffer_size = 0;
    long buffer_granularity = 0;
    double current_sample_rate = 0.0;
    bool requested_sample_rate_supported = false;
};

struct DeviceMeterResult {
    DeviceInfo device;
    double sample_rate = 0.0;
    long buffer_size = 0;
    long callbacks = 0;
    long input_sample_type = 0;
    long output_sample_type = 0;
    double input_peak = 0.0;
};

struct DeviceRingResult {
    DeviceInfo device;
    double sample_rate = 0.0;
    long buffer_size = 0;
    long callbacks = 0;
    std::uint64_t ring_overruns = 0;
    std::uint64_t ring_underruns = 0;
    std::size_t ring_readable = 0;
};

class DeviceStream {
public:
    virtual ~DeviceStream() = default;
    DeviceStream(const DeviceStream&) = delete;
    DeviceStream& operator=(const DeviceStream&) = delete;

    virtual long callbacks() const = 0;
    virtual bool playback_prefilled() const = 0;

protected:
    DeviceStream() = default;
};

std::vector<DeviceInfo> list_devices();
DeviceProbe probe_device(int id, double requested_sample_rate);
DeviceMeterResult meter_device(int id, double requested_sample_rate, long buffer_size, int duration_ms);
DeviceRingResult ring_device(
    int id,
    double requested_sample_rate,
    long buffer_size,
    int duration_ms,
    std::size_t ring_capacity_frames);
std::unique_ptr<DeviceStream> start_duplex_stream(
    int id,
    double requested_sample_rate,
    long buffer_size,
    MonoRingBuffer& capture_ring,
    MonoRingBuffer& playback_ring,
    std::size_t playback_prefill_frames);

} // namespace jam2::audio
