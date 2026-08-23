#pragma once

#include "audio_device.hpp"
#include "metronome.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace jam2::audio::device_processing {

// Platform-neutral, allocation-free state used by the ASIO and CoreAudio
// callbacks. It is owned by the stream and touched only by its audio callback.
struct PlaybackResamplerState {
    std::int32_t current = 0;
    std::int32_t next = 0;
    bool hasCurrent = false;
    bool hasNext = false;
    double phase = 0.0;
    std::int32_t underrunConcealmentOrigin = 0;
    std::uint32_t underrunConcealmentFrames = 0;
    PlaybackRatioSmoother ratioSmoother;

    void reset() noexcept;
};

// One audio callback is the only writer. Atomics permit non-real-time readers
// without locks; the writer uses ordinary relaxed stores rather than locked
// read-modify-write instructions.
struct CallbackIntervalState {
    std::uint64_t lastCallbackUs = 0;
    std::atomic<std::uint64_t> minimumUs{0};
    std::atomic<std::uint64_t> sumUs{0};
    std::atomic<std::uint64_t> maximumUs{0};
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> gapsOver1_1x{0};
    std::atomic<std::uint64_t> gapsOver1_5x{0};
    std::atomic<std::uint64_t> gapsOver2x{0};
    std::atomic<std::uint64_t> workMinimumUs{0};
    std::atomic<std::uint64_t> workSumUs{0};
    std::atomic<std::uint64_t> workMaximumUs{0};
    std::atomic<std::uint64_t> workSamples{0};
};

enum class DriverOutputReadyObservation {
    Accepted,
    Unsupported,
    Error,
};

// Probed once before stream start. Supported drivers are then notified only
// after each output block is fully written; any later failure disables the
// optional callback without affecting the audio stream.
struct DriverOutputReadyState {
    DriverOutputReadyStatus status = DriverOutputReadyStatus::NotApplicable;
    long error = 0;

    bool shouldNotify() const noexcept;
    void observe(
        DriverOutputReadyObservation observation,
        long errorCode = 0) noexcept;
};

long driver_output_ready_latency_reduction(
    const DriverOutputReadyState& state,
    long beforeFrames,
    long afterFrames) noexcept;

// Prepared before the device starts. The callback only performs bounded
// indexed reads; click synthesis never allocates or evaluates trig/exp there.
class MetronomeWaveBank {
public:
    MetronomeWaveBank() = default;
    explicit MetronomeWaveBank(double sampleRate);

    void prepare(double sampleRate);
    bool preparedFor(double sampleRate) const noexcept;
    double render(
        const metronome::PatternSnapshot& pattern,
        int patternStep,
        std::uint64_t stepOffset,
        double level,
        metronome::ClickVoice voice,
        metronome::ClickSound sound) const noexcept;

private:
    double sampleRate_ = 0.0;
    std::array<std::vector<double>, 16> waves_;
};

std::uint64_t callback_now_us() noexcept;
void observe_callback_interval(
    CallbackIntervalState& state,
    std::uint64_t nowUs,
    std::size_t bufferFrames,
    double sampleRate) noexcept;
void observe_callback_work(
    CallbackIntervalState& state,
    std::uint64_t startUs,
    std::uint64_t endUs) noexcept;
void publish_callback_begin(
    StreamControl* control,
    std::uint64_t& writerGeneration) noexcept;
void publish_callback_end(
    StreamControl* control,
    std::uint64_t& writerGeneration) noexcept;

void update_peak(std::atomic<int>& peak, int candidate) noexcept;
int i32_peak_ppm(std::span<const std::int32_t> samples) noexcept;
std::int32_t scale_i32_sample(std::int32_t sample, double level) noexcept;
void observe_peak(
    std::atomic<int>& peak,
    std::span<const std::int32_t> samples) noexcept;
void observe_shared_peak(
    std::atomic<int>& currentPeak,
    std::atomic<int>& intervalPeak,
    std::span<const std::int32_t> samples) noexcept;
void observe_input_peaks(
    StreamControl* control,
    std::span<const std::int32_t> samples) noexcept;
void observe_input_peak_value(StreamControl* control, int inputPeak) noexcept;

bool read_network_playback_timeline(
    const StreamControl& control,
    const MonoRingBuffer& playback,
    std::uint64_t& engineFrame,
    std::size_t& queuedFrames) noexcept;

std::int32_t mix_i32_samples(std::int32_t a, std::int32_t b) noexcept;
void apply_remote_level(
    StreamControl* control,
    std::span<std::int32_t> output) noexcept;
void apply_output_level(
    StreamControl* control,
    std::span<std::int32_t> output) noexcept;
void mix_local_monitor(
    StreamControl* control,
    std::span<std::int32_t> output,
    std::span<const std::int32_t> input) noexcept;
void mix_prepared_source(
    StreamControl* control,
    std::span<std::int32_t> output,
    std::uint64_t frame,
    std::span<std::int32_t> stem) noexcept;
void observe_output_peak(
    StreamControl* control,
    std::span<const std::int32_t> output) noexcept;

void pop_resampled_playback(
    MonoRingBuffer* playback,
    StreamControl* control,
    PlaybackResamplerState& state,
    std::span<std::int32_t> sourceScratch,
    std::span<std::int32_t> output) noexcept;

std::int32_t render_test_input_sample(
    int mode,
    std::uint64_t sampleTime,
    double sampleRate,
    double level) noexcept;
std::int32_t render_metronome_test_input_sample(
    const StreamControl& control,
    std::uint64_t sampleTime,
    double sampleRate,
    double level) noexcept;
void fill_test_input(
    StreamControl* control,
    double sampleRate,
    std::uint64_t& sampleCounter,
    std::span<std::int32_t> output) noexcept;
void mix_metronome_click(
    StreamControl* control,
    double sampleRate,
    std::uint64_t engineFrame,
    std::uint64_t& beatIndex,
    std::span<std::int32_t> output,
    std::span<std::int32_t> metronomeStem,
    const MetronomeWaveBank* waveBank = nullptr) noexcept;

} // namespace jam2::audio::device_processing
