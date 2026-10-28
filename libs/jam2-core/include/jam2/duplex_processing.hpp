#pragma once
#include "audio_device_processing.hpp"

namespace jam2::audio::device_processing {

// Stream-owned, single audio-thread state. All storage is prepared before start.
struct DuplexProcessingState {
    long buffer_size = 0;
    InputChannels input_channels = InputChannels::Mono;
    MonoRingBuffer* capture = nullptr;
    MonoRingBuffer* pitch = nullptr;
    MonoRingBuffer* playback = nullptr;
    StreamControl* control = nullptr;
    OutputRecorder* recorder = nullptr;
    TrackTakeRecorder* track_take_recorder = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<const std::int32_t*> input_source_pointers;
    std::vector<double> input_peak_scratch;
    SmoothedMonoDownmix input_downmix;
    std::vector<std::int32_t> playback_scratch;
    std::vector<std::int32_t> playback_resampler_scratch;
    std::vector<std::int32_t> recorder_my_input_scratch;
    std::vector<std::int32_t> recorder_their_input_scratch;
    std::vector<std::int32_t> recorder_inputs_mix_scratch;
    std::vector<std::int32_t> recorder_metronome_scratch;
    std::vector<std::int32_t> recorder_prepared_scratch;
    std::size_t playback_prefill_frames = 0;
    double sample_rate = 48000.0;
    std::uint64_t test_input_sample_counter = 0;
    std::uint64_t engine_frame_counter = 0;
    std::uint64_t metronome_beat_index = 0;
    PlaybackResamplerState playback_resampler;
    std::atomic<long> callbacks{0};
    std::atomic<bool> playback_prefilled{false};
    CallbackIntervalState callback_intervals;
    std::uint64_t callback_generation = 0;
    MetronomeWaveBank metronome_wave_bank;
};

void prepare_duplex_processing(DuplexProcessingState& state, std::size_t maximumFrames,
    std::size_t inputCount, double sampleRate);
void process_duplex_block(DuplexProcessingState& state,
    std::span<const std::int32_t* const> inputs, std::size_t frames,
    bool renderOutput, DeviceClockSample clock, bool measureCallback = true) noexcept;

} // namespace jam2::audio::device_processing
