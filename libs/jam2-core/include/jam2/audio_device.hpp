#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio_ring.hpp"
#include "metronome.hpp"
#include "output_recorder.hpp"

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
    std::uint64_t ring_underrun_events = 0;
    std::size_t ring_readable = 0;
};

struct MetronomeConfig {
    bool enabled = false;
    int bpm = 120;
    double level = 0.20;
};

struct StreamControl {
    std::atomic<bool> metronome_enabled{false};
    std::atomic<int> metronome_bpm{120};
    std::atomic<int> metronome_beats_per_bar{4};
    std::atomic<int> metronome_division{1};
    std::atomic<int> metronome_step_count{4};
    std::atomic<std::uint64_t> metronome_play_mask_low{0x0fULL};
    std::atomic<std::uint64_t> metronome_play_mask_high{0};
    std::atomic<std::uint64_t> metronome_accent_mask_low{0x01ULL};
    std::atomic<std::uint64_t> metronome_accent_mask_high{0};
    std::atomic<int> metronome_level_ppm{200000};
    std::atomic<int> remote_level_ppm{1000000};
    std::atomic<bool> local_monitor_enabled{false};
    std::atomic<int> local_monitor_level_ppm{250000};
    std::atomic<int> send_level_ppm{1000000};
    std::atomic<int> playback_ratio_ppm{1000000};
    std::atomic<int> metronome_mode{0};
    std::atomic<bool> leader_audio_local_click{false};
    std::atomic<std::uint64_t> metronome_epoch_sample_time{0};
    std::atomic<bool> metronome_epoch_valid{false};
    std::atomic<int> test_input_mode{0};
    std::atomic<int> test_input_level_ppm{125000};
    std::atomic<int> input_peak_ppm{0};
    std::atomic<int> send_peak_ppm{0};
    std::atomic<int> monitor_peak_ppm{0};
    std::atomic<int> remote_peak_ppm{0};
    std::atomic<int> metronome_peak_ppm{0};
    std::atomic<int> output_peak_ppm{0};
    std::atomic<int> gui_input_peak_ppm{0};
    std::atomic<int> gui_send_peak_ppm{0};
    std::atomic<int> gui_monitor_peak_ppm{0};
    std::atomic<int> gui_remote_peak_ppm{0};
    std::atomic<int> gui_metronome_peak_ppm{0};
    std::atomic<int> gui_output_peak_ppm{0};
    std::atomic<std::uint64_t> output_clipped_samples{0};
};

enum class InputChannels {
    Mono = 1,
    Stereo = 2,
};

struct ChannelSelection {
    std::vector<int> input{0};
    std::vector<int> output{0, 1};
};

struct StreamInfo {
    DeviceInfo device;
    double sample_rate = 0.0;
    long buffer_size = 0;
    InputChannels input_channels = InputChannels::Mono;
    ChannelSelection channels;
    std::string sample_format;
};

struct CallbackTimingStats {
    std::uint64_t interval_min_us = 0;
    std::uint64_t interval_sum_us = 0;
    std::uint64_t interval_max_us = 0;
    std::uint64_t interval_samples = 0;
    std::uint64_t gap_over_1_1x_count = 0;
    std::uint64_t gap_over_1_5x_count = 0;
    std::uint64_t gap_over_2x_count = 0;
};

class DeviceStream {
public:
    virtual ~DeviceStream() = default;
    DeviceStream(const DeviceStream&) = delete;
    DeviceStream& operator=(const DeviceStream&) = delete;

    virtual long callbacks() const = 0;
    virtual bool playback_prefilled() const = 0;
    virtual StreamInfo info() const = 0;
    virtual CallbackTimingStats callback_timing_stats() const = 0;

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
    InputChannels input_channels,
    ChannelSelection channels,
    MonoRingBuffer& capture_ring,
    MonoRingBuffer& playback_ring,
    std::size_t playback_prefill_frames,
    StreamControl& control,
    OutputRecorder* recorder);

} // namespace jam2::audio
