#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "common.hpp"
#include "pcm16_wav.hpp"

namespace jam2 {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

template <typename T, std::size_t Capacity>
class FixedQueue {
public:
    bool push(const T& value) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == Capacity) {
            ++drops_;
            return false;
        }
        values_[write_] = value;
        write_ = (write_ + 1U) % Capacity;
        ++count_;
        high_water_ = std::max(high_water_, count_);
        return true;
    }

    bool pop(T& value) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) {
            return false;
        }
        value = values_[read_];
        read_ = (read_ + 1U) % Capacity;
        --count_;
        return true;
    }

    std::size_t depth() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    std::size_t highWater() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_;
    }

    std::uint64_t drops() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return drops_;
    }

private:
    mutable std::mutex mutex_;
    std::array<T, Capacity> values_{};
    std::size_t read_ = 0;
    std::size_t write_ = 0;
    std::size_t count_ = 0;
    std::size_t high_water_ = 0;
    std::uint64_t drops_ = 0;
};

template <std::size_t Size>
bool set_bounded_text(std::array<char, Size>& output, std::string_view text) noexcept
{
    if (text.size() >= output.size()) {
        return false;
    }
    output.fill('\0');
    if (!text.empty()) {
        std::memcpy(output.data(), text.data(), text.size());
    }
    return true;
}

template <std::size_t Size>
std::string_view bounded_text(const std::array<char, Size>& text) noexcept
{
    const auto end = std::find(text.begin(), text.end(), '\0');
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

int clamp_gain(int value) noexcept
{
    return std::clamp(value, 0, 4000000);
}

int clamp_ratio(int value) noexcept
{
    return std::clamp(value, 500000, 2000000);
}

std::int32_t clamp_i32(double sample) noexcept
{
    return static_cast<std::int32_t>(std::clamp(
        sample,
        static_cast<double>((std::numeric_limits<std::int32_t>::min)()),
        static_cast<double>((std::numeric_limits<std::int32_t>::max)())));
}

std::int32_t mix_i32(std::int32_t left, std::int32_t right) noexcept
{
    const std::int64_t mixed = static_cast<std::int64_t>(left) + static_cast<std::int64_t>(right);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        mixed,
        (std::numeric_limits<std::int32_t>::min)(),
        (std::numeric_limits<std::int32_t>::max)()));
}

int peak_ppm(std::span<const std::int32_t> samples) noexcept
{
    std::int64_t peak = 0;
    for (const std::int32_t sample : samples) {
        const std::int64_t magnitude = sample < 0
            ? -static_cast<std::int64_t>(sample)
            : static_cast<std::int64_t>(sample);
        peak = std::max(peak, magnitude);
    }
    const double normalized = static_cast<double>(peak) /
        static_cast<double>((std::numeric_limits<std::int32_t>::max)());
    return static_cast<int>(std::clamp(normalized, 0.0, 1.0) * 1000000.0);
}

void update_peak(std::atomic<int>& target, int value) noexcept
{
    int current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

std::int32_t render_test_input(
    int mode,
    std::uint64_t frame,
    double sample_rate,
    double level) noexcept
{
    if (mode == static_cast<int>(EngineTestInput::Silence) || sample_rate <= 0.0) {
        return 0;
    }
    if (mode == static_cast<int>(EngineTestInput::Tone440)) {
        const double phase = std::fmod(static_cast<double>(frame) * 440.0 / sample_rate, 1.0);
        return clamp_i32(std::sin(phase * 2.0 * kPi) * level * 2147483647.0);
    }
    if (mode == static_cast<int>(EngineTestInput::Pulse1s)) {
        const std::uint64_t period = static_cast<std::uint64_t>(std::max(1.0, sample_rate));
        const std::uint64_t width = std::max<std::uint64_t>(1, period / 100U);
        return frame % period < width ? clamp_i32(level * 2147483647.0) : 0;
    }
    return 0;
}

std::int32_t render_metronome_test_input(
    const audio::StreamControl& control,
    std::uint64_t frame,
    double sample_rate,
    double level) noexcept
{
    if (sample_rate <= 0.0 ||
        !control.metronome_enabled.load(std::memory_order_relaxed) ||
        !control.metronome_epoch_valid.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::uint64_t epoch = control.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    if (frame < epoch) {
        return 0;
    }
    const auto pattern = metronome::sanitize({
        control.metronome_bpm.load(std::memory_order_relaxed),
        control.metronome_beats_per_bar.load(std::memory_order_relaxed),
        control.metronome_division.load(std::memory_order_relaxed),
        control.metronome_step_count.load(std::memory_order_relaxed),
        control.metronome_play_mask_low.load(std::memory_order_relaxed),
        control.metronome_play_mask_high.load(std::memory_order_relaxed),
        control.metronome_accent_mask_low.load(std::memory_order_relaxed),
        control.metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
    const std::uint64_t interval = metronome::step_interval_samples(
        sample_rate,
        pattern.bpm,
        pattern.division);
    return metronome::mix_i32(
        0,
        metronome::render_sample(pattern, frame - epoch, interval, sample_rate, level));
}

void mix_metronome_output(
    audio::StreamControl& control,
    std::uint64_t callback_frame,
    double sample_rate,
    std::span<std::int32_t> output,
    std::span<std::int32_t> stem) noexcept
{
    if (stem.size() == output.size()) {
        std::fill(stem.begin(), stem.end(), 0);
    }
    const bool local_click_suppressed =
        control.metronome_mode.load(std::memory_order_relaxed) == 1 &&
        !control.leader_audio_local_click.load(std::memory_order_relaxed);
    if (sample_rate <= 0.0 ||
        !control.metronome_enabled.load(std::memory_order_relaxed) ||
        local_click_suppressed) {
        return;
    }

    const double level = static_cast<double>(
        std::clamp(control.metronome_level_ppm.load(std::memory_order_relaxed), 0, 4000000)) /
        1000000.0;
    const bool epoch_valid = control.metronome_epoch_valid.load(std::memory_order_relaxed);
    const std::uint64_t epoch = control.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    const std::int64_t render_offset =
        control.metronome_render_offset_frames.load(std::memory_order_relaxed);
    const auto pattern = metronome::sanitize({
        control.metronome_bpm.load(std::memory_order_relaxed),
        control.metronome_beats_per_bar.load(std::memory_order_relaxed),
        control.metronome_division.load(std::memory_order_relaxed),
        control.metronome_step_count.load(std::memory_order_relaxed),
        control.metronome_play_mask_low.load(std::memory_order_relaxed),
        control.metronome_play_mask_high.load(std::memory_order_relaxed),
        control.metronome_accent_mask_low.load(std::memory_order_relaxed),
        control.metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
    const std::uint64_t interval = metronome::step_interval_samples(
        sample_rate,
        pattern.bpm,
        pattern.division);

    for (std::size_t index = 0; index < output.size(); ++index) {
        std::uint64_t frame = callback_frame + static_cast<std::uint64_t>(index);
        if (render_offset < 0) {
            const std::uint64_t magnitude = static_cast<std::uint64_t>(-(render_offset + 1)) + 1ULL;
            frame = frame > magnitude ? frame - magnitude : 0ULL;
        } else {
            frame += static_cast<std::uint64_t>(render_offset);
        }
        if (epoch_valid && frame < epoch) {
            continue;
        }
        const std::uint64_t position = epoch_valid ? frame - epoch : frame;
        const double rendered = metronome::render_sample(
            pattern,
            position,
            interval,
            sample_rate,
            level);
        if (stem.size() == output.size()) {
            stem[index] = metronome::mix_i32(0, rendered);
        }
        output[index] = metronome::mix_i32(output[index], rendered);
    }
}

class HeadlessDeviceStream final : public audio::DeviceStream {
public:
    HeadlessDeviceStream(
        double sample_rate,
        long buffer_size,
        int clock_drift_ppm,
        audio::InputChannels input_channels,
        audio::ChannelSelection channels,
        audio::MonoRingBuffer& capture_ring,
        audio::MonoRingBuffer& playback_ring,
        std::size_t playback_prefill_frames,
        audio::StreamControl& control,
        audio::OutputRecorder* recorder,
        audio::TrackTakeRecorder* track_take_recorder)
        : sample_rate_(sample_rate)
        , buffer_size_(buffer_size > 0 ? buffer_size : 128)
        , clock_rate_(sample_rate * (1.0 + static_cast<double>(clock_drift_ppm) / 1000000.0))
        , capture_ring_(capture_ring)
        , playback_ring_(playback_ring)
        , playback_prefill_frames_(playback_prefill_frames)
        , control_(control)
        , recorder_(recorder)
        , track_take_recorder_(track_take_recorder)
    {
        info_.device.id = -1;
        info_.device.backend = "headless";
        info_.device.name = "Headless synthetic audio";
        info_.sample_rate = sample_rate_;
        info_.buffer_size = buffer_size_;
        info_.input_channels = input_channels;
        info_.channels = std::move(channels);
        info_.sample_format = "s32";
        const std::size_t frames = static_cast<std::size_t>(buffer_size_);
        capture_scratch_.resize(frames, 0);
        playback_scratch_.resize(frames, 0);
        output_scratch_.resize(frames, 0);
        inputs_mix_scratch_.resize(frames, 0);
        metronome_scratch_.resize(frames, 0);
        thread_ = std::thread([this] { run(); });
    }

    ~HeadlessDeviceStream() override
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    long callbacks() const override { return callbacks_.load(std::memory_order_relaxed); }
    bool playback_prefilled() const override { return playback_prefilled_.load(std::memory_order_relaxed); }
    audio::StreamInfo info() const override { return info_; }

    audio::CallbackTimingStats callback_timing_stats() const override
    {
        return {
            interval_min_us_.load(std::memory_order_relaxed),
            interval_sum_us_.load(std::memory_order_relaxed),
            interval_max_us_.load(std::memory_order_relaxed),
            interval_samples_.load(std::memory_order_relaxed),
            gap_over_1_1x_count_.load(std::memory_order_relaxed),
            gap_over_1_5x_count_.load(std::memory_order_relaxed),
            gap_over_2x_count_.load(std::memory_order_relaxed),
        };
    }

private:
    void observe_callback_interval(std::uint64_t now_us) noexcept
    {
        if (last_callback_us_ == 0) {
            last_callback_us_ = now_us;
            return;
        }
        const std::uint64_t interval = now_us >= last_callback_us_ ? now_us - last_callback_us_ : 0;
        last_callback_us_ = now_us;
        const std::uint64_t expected = static_cast<std::uint64_t>(
            static_cast<double>(buffer_size_) * 1000000.0 / clock_rate_);
        std::uint64_t minimum = interval_min_us_.load(std::memory_order_relaxed);
        while ((minimum == 0 || interval < minimum) &&
               !interval_min_us_.compare_exchange_weak(minimum, interval, std::memory_order_relaxed)) {
        }
        interval_sum_us_.fetch_add(interval, std::memory_order_relaxed);
        std::uint64_t maximum = interval_max_us_.load(std::memory_order_relaxed);
        while (interval > maximum &&
               !interval_max_us_.compare_exchange_weak(maximum, interval, std::memory_order_relaxed)) {
        }
        interval_samples_.fetch_add(1, std::memory_order_relaxed);
        if (expected > 0) {
            if (interval > expected * 11U / 10U) {
                gap_over_1_1x_count_.fetch_add(1, std::memory_order_relaxed);
            }
            if (interval > expected * 3U / 2U) {
                gap_over_1_5x_count_.fetch_add(1, std::memory_order_relaxed);
            }
            if (interval > expected * 2U) {
                gap_over_2x_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void fill_capture(std::uint64_t callback_frame) noexcept
    {
        const int mode = control_.test_input_mode.load(std::memory_order_relaxed);
        const double level = static_cast<double>(
            std::clamp(control_.test_input_level_ppm.load(std::memory_order_relaxed), 0, 1000000)) /
            1000000.0;
        for (std::int32_t& sample : capture_scratch_) {
            sample = mode == static_cast<int>(EngineTestInput::MetronomePulse)
                ? render_metronome_test_input(control_, test_input_frame_, sample_rate_, level)
                : render_test_input(mode, test_input_frame_, sample_rate_, level);
            ++test_input_frame_;
        }
        if (audio::prepare_network_capture_callback(control_, capture_ring_, callback_frame)) {
            capture_ring_.push(capture_scratch_);
        }
        const int peak = peak_ppm(capture_scratch_);
        update_peak(control_.input_peak_ppm, peak);
        update_peak(control_.gui_input_peak_ppm, peak);
    }

    void render_output(std::uint64_t callback_frame) noexcept
    {
        const bool network_playback = control_.network_playback_enabled.load(std::memory_order_acquire);
        if (!network_playback) {
            playback_prefilled_.store(false, std::memory_order_relaxed);
            playback_ring_.pop(std::span<std::int32_t>{}, false);
            std::fill(playback_scratch_.begin(), playback_scratch_.end(), 0);
        } else if (!playback_prefilled_.load(std::memory_order_relaxed)) {
            if (playback_ring_.available_read() >= playback_prefill_frames_) {
                playback_prefilled_.store(true, std::memory_order_relaxed);
            } else {
                std::fill(playback_scratch_.begin(), playback_scratch_.end(), 0);
            }
        }
        if (network_playback && playback_prefilled_.load(std::memory_order_relaxed)) {
            playback_ring_.pop(playback_scratch_);
        }
        control_.network_playback_enabled_applied.store(network_playback, std::memory_order_release);
        const int remote_peak = peak_ppm(playback_scratch_);
        update_peak(control_.remote_peak_ppm, remote_peak);
        update_peak(control_.gui_remote_peak_ppm, remote_peak);

        const double remote_level = static_cast<double>(
            clamp_gain(control_.remote_level_ppm.load(std::memory_order_relaxed))) / 1000000.0;
        const bool monitor = control_.local_monitor_enabled.load(std::memory_order_relaxed);
        const double monitor_level = static_cast<double>(
            clamp_gain(control_.local_monitor_level_ppm.load(std::memory_order_relaxed))) / 1000000.0;
        for (std::size_t index = 0; index < output_scratch_.size(); ++index) {
            std::int32_t sample = clamp_i32(static_cast<double>(playback_scratch_[index]) * remote_level);
            if (monitor) {
                sample = mix_i32(
                    sample,
                    clamp_i32(static_cast<double>(capture_scratch_[index]) * monitor_level));
            }
            output_scratch_[index] = sample;
            if (sample == (std::numeric_limits<std::int32_t>::min)() ||
                sample == (std::numeric_limits<std::int32_t>::max)()) {
                control_.output_clipped_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (control_.prepared_source != nullptr && !output_scratch_.empty()) {
            control_.prepared_source->mix(output_scratch_.data(), output_scratch_.size(), callback_frame);
            control_.prepared_source_frame.store(control_.prepared_source->sourceFrame(), std::memory_order_relaxed);
            control_.prepared_source_scheduled_start_frame.store(
                control_.prepared_source->scheduledStartFrame(),
                std::memory_order_relaxed);
            control_.prepared_source_actual_start_frame.store(
                control_.prepared_source->actualStartFrame(),
                std::memory_order_relaxed);
            control_.prepared_source_underruns.store(control_.prepared_source->underruns(), std::memory_order_relaxed);
        }
        mix_metronome_output(
            control_,
            callback_frame,
            sample_rate_,
            output_scratch_,
            metronome_scratch_);
        const int metronome_peak = peak_ppm(metronome_scratch_);
        update_peak(control_.metronome_peak_ppm, metronome_peak);
        update_peak(control_.gui_metronome_peak_ppm, metronome_peak);
        if (track_take_recorder_ != nullptr) {
            track_take_recorder_->record(callback_frame, capture_scratch_);
        }
        const int output_peak = peak_ppm(output_scratch_);
        update_peak(control_.output_peak_ppm, output_peak);
        update_peak(control_.gui_output_peak_ppm, output_peak);
        if (recorder_ != nullptr) {
            for (std::size_t index = 0; index < inputs_mix_scratch_.size(); ++index) {
                inputs_mix_scratch_[index] = mix_i32(capture_scratch_[index], playback_scratch_[index]);
            }
            recorder_->record({
                callback_frame,
                output_scratch_,
                capture_scratch_,
                playback_scratch_,
                inputs_mix_scratch_,
                metronome_scratch_,
            });
        }
    }

    void run() noexcept
    {
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        const auto period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(static_cast<double>(buffer_size_) / clock_rate_));
        while (!stop_.load(std::memory_order_acquire)) {
            const std::uint64_t callback_frame = engine_frame_;
            observe_callback_interval(monotonic_us());
            fill_capture(callback_frame);
            render_output(callback_frame);
            engine_frame_ += static_cast<std::uint64_t>(buffer_size_);
            control_.engine_frame_counter.store(engine_frame_, std::memory_order_release);
            callbacks_.fetch_add(1, std::memory_order_relaxed);
            next += period;
            std::this_thread::sleep_until(next);
            if (next + period < clock::now()) {
                next = clock::now();
            }
        }
    }

    double sample_rate_ = 48000.0;
    long buffer_size_ = 128;
    double clock_rate_ = 48000.0;
    audio::MonoRingBuffer& capture_ring_;
    audio::MonoRingBuffer& playback_ring_;
    std::size_t playback_prefill_frames_ = 0;
    audio::StreamControl& control_;
    audio::OutputRecorder* recorder_ = nullptr;
    audio::TrackTakeRecorder* track_take_recorder_ = nullptr;
    audio::StreamInfo info_;
    std::vector<std::int32_t> capture_scratch_;
    std::vector<std::int32_t> playback_scratch_;
    std::vector<std::int32_t> output_scratch_;
    std::vector<std::int32_t> inputs_mix_scratch_;
    std::vector<std::int32_t> metronome_scratch_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<long> callbacks_{0};
    std::atomic<bool> playback_prefilled_{false};
    std::atomic<std::uint64_t> interval_min_us_{0};
    std::atomic<std::uint64_t> interval_sum_us_{0};
    std::atomic<std::uint64_t> interval_max_us_{0};
    std::atomic<std::uint64_t> interval_samples_{0};
    std::atomic<std::uint64_t> gap_over_1_1x_count_{0};
    std::atomic<std::uint64_t> gap_over_1_5x_count_{0};
    std::atomic<std::uint64_t> gap_over_2x_count_{0};
    std::uint64_t last_callback_us_ = 0;
    std::uint64_t test_input_frame_ = 0;
    std::uint64_t engine_frame_ = 0;
};

struct PreparedLoadResult {
    bool ok = false;
    std::uint64_t frames = 0;
    std::string error;
};

PreparedLoadResult load_prepared_track(
    audio::PreparedTrackSource& source,
    const std::filesystem::path& path,
    int sample_rate,
    std::size_t maximum_frames,
    std::uint64_t target_frame)
{
    const wav::InspectResult inspected = wav::inspect_pcm16_file(path);
    if (!inspected) {
        return {false, 0, "prepared track load failed: " + inspected.error};
    }
    if (inspected.info.channels != 1U) {
        return {false, 0, "prepared track load failed: WAV must be mono PCM16"};
    }
    if (inspected.info.sample_rate != static_cast<std::uint32_t>(sample_rate)) {
        std::ostringstream message;
        message << "prepared track load failed: WAV sample rate " << inspected.info.sample_rate
                << " does not match active engine rate " << sample_rate;
        return {false, 0, message.str()};
    }
    if (inspected.info.frames > static_cast<std::uint64_t>(maximum_frames)) {
        return {false, 0, "prepared track load failed: WAV exceeds configured source capacity"};
    }
    const int slot = source.claimLoadingSlot();
    if (slot < 0) {
        return {false, 0, "prepared track load failed: no free source slot"};
    }
    std::int16_t* destination = source.loadingData(slot);
    if (destination == nullptr) {
        source.abandonLoadingSlot(slot);
        return {false, 0, "prepared track load failed: source slot unavailable"};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file || inspected.info.data_offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
        source.abandonLoadingSlot(slot);
        return {false, 0, "prepared track load failed: cannot reopen WAV data"};
    }
    file.seekg(static_cast<std::streamoff>(inspected.info.data_offset), std::ios::beg);
    std::array<unsigned char, 8192> input{};
    std::uint64_t frame = 0;
    while (frame < inspected.info.frames) {
        const std::uint64_t count = std::min<std::uint64_t>(
            inspected.info.frames - frame,
            input.size() / 2U);
        const std::streamsize bytes = static_cast<std::streamsize>(count * 2U);
        file.read(reinterpret_cast<char*>(input.data()), bytes);
        if (file.gcount() != bytes) {
            source.abandonLoadingSlot(slot);
            return {false, 0, "prepared track load failed: truncated WAV data"};
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::size_t byte = static_cast<std::size_t>(index * 2U);
            const std::uint16_t sample = static_cast<std::uint16_t>(input[byte]) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[byte + 1U]) << 8U);
            destination[frame + index] = static_cast<std::int16_t>(sample);
        }
        frame += count;
    }
    if (!source.publishReady(slot, inspected.info.frames, sample_rate)) {
        source.abandonLoadingSlot(slot);
        return {false, 0, "prepared track load failed: source slot publish failed"};
    }
    const std::uint64_t replacement = source.playing()
        ? std::min(source.sourceFrame(), inspected.info.frames)
        : 0ULL;
    const std::array<audio::PreparedTrackSource::Command, 2> commands{{
        {
            audio::PreparedTrackSource::CommandType::Swap,
            static_cast<std::uint32_t>(slot),
            0,
            replacement,
            0,
            1000000,
        },
        {
            audio::PreparedTrackSource::CommandType::SetLoop,
            0,
            target_frame,
            0,
            inspected.info.frames,
            1000000,
        },
    }};
    if (!source.enqueueBatch(commands)) {
        source.abandonReadySlot(slot);
        return {false, 0, "prepared track load failed: command queue full"};
    }
    return {true, inspected.info.frames, {}};
}

} // namespace

struct Engine::Impl {
    struct ScheduledCommand {
        EngineCommand command;
        std::uint64_t order = 0;
    };

    mutable std::mutex lifecycle_mutex;
    mutable std::mutex scheduled_mutex;
    EngineConfig config;
    bool has_config = false;
    std::atomic<EngineLifecycle> lifecycle{EngineLifecycle::Stopped};
    std::atomic<bool> stop_requested{false};
    std::thread supervisor;
    std::unique_ptr<audio::StreamControl> control;
    std::unique_ptr<audio::OutputRecorder> recorder;
    std::unique_ptr<audio::TrackTakeRecorder> track_take_recorder;
    std::unique_ptr<audio::PreparedTrackSource> prepared_source;
    std::unique_ptr<audio::MonoRingBuffer> capture_ring;
    std::unique_ptr<audio::MonoRingBuffer> playback_ring;
    std::unique_ptr<audio::DeviceStream> stream;
    audio::StreamInfo stream_info;
    FixedQueue<EngineCommand, Engine::kCommandCapacity> commands;
    FixedQueue<EngineEvent, Engine::kEventCapacity> events;
    std::array<ScheduledCommand, Engine::kScheduledCommandCapacity> scheduled{};
    std::size_t scheduled_count = 0;
    std::size_t scheduled_high_water = 0;
    std::uint64_t scheduled_rejections = 0;
    std::uint64_t schedule_order = 0;
    std::atomic<bool> attachment_active{false};
    std::atomic<std::uint64_t> active_attachment_generation{0};
    std::atomic<std::uint64_t> capture_frames_popped{0};
    std::atomic<std::uint64_t> capture_attach_count{0};
    std::atomic<std::uint64_t> capture_detach_count{0};
    std::atomic<std::uint64_t> transport_revision{0};
    std::atomic<EngineTransportAction> transport_action{EngineTransportAction::None};
    std::atomic<std::uint64_t> transport_target_frame{0};
    std::atomic<std::uint64_t> transport_musical_frame{0};
    std::atomic<std::uint64_t> transport_countdown_start_frame{0};
    std::atomic<std::uint64_t> transport_cookie{0};
    std::atomic<bool> transport_pending{false};
    std::atomic<std::uint64_t> transport_commit_count{0};

    void push_event(
        EngineEventType type,
        bool ok,
        std::uint64_t cookie,
        std::uint64_t requested_frame,
        std::uint64_t applied_frame,
        std::uint64_t value,
        std::string_view message = {}) noexcept
    {
        EngineEvent event;
        event.type = type;
        event.lifecycle = lifecycle.load(std::memory_order_relaxed);
        event.cookie = cookie;
        event.requested_frame = requested_frame;
        event.applied_frame = applied_frame;
        event.value = value;
        event.ok = ok;
        if (!set_bounded_text(event.text, message)) {
            (void)set_bounded_text(event.text, "engine event text exceeded fixed capacity");
        }
        (void)events.push(event);
    }

    void set_lifecycle(EngineLifecycle next, std::string_view message = {}) noexcept
    {
        lifecycle.store(next, std::memory_order_release);
        push_event(EngineEventType::Lifecycle, next != EngineLifecycle::Failed, 0, 0, 0, 0, message);
    }

    bool enqueue_prepared(const audio::PreparedTrackSource::Command& command) noexcept
    {
        if (prepared_source == nullptr || !prepared_source->enqueue(command)) {
            if (control != nullptr) {
                control->prepared_source_busy_events.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        return true;
    }

    bool apply_command(const EngineCommand& command, std::uint64_t applied_frame, std::string& error)
    {
        if (control == nullptr) {
            error = "local audio engine is not running";
            return false;
        }
        const auto queue_prepared = [&](const audio::PreparedTrackSource::Command& prepared_command) {
            if (!enqueue_prepared(prepared_command)) {
                error = "prepared track command queue is full";
                return false;
            }
            return true;
        };
        switch (command.type) {
        case EngineCommandType::Stop:
            stop_requested.store(true, std::memory_order_release);
            return true;
        case EngineCommandType::SetMetronomeEnabled:
            control->metronome_enabled.store(command.enabled, std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetMetronomePattern: {
            const auto pattern = metronome::sanitize(command.pattern);
            control->metronome_bpm.store(pattern.bpm, std::memory_order_relaxed);
            control->metronome_beats_per_bar.store(pattern.beats_per_bar, std::memory_order_relaxed);
            control->metronome_division.store(pattern.division, std::memory_order_relaxed);
            control->metronome_step_count.store(pattern.step_count, std::memory_order_relaxed);
            control->metronome_play_mask_low.store(pattern.play_mask_low, std::memory_order_relaxed);
            control->metronome_play_mask_high.store(pattern.play_mask_high, std::memory_order_relaxed);
            control->metronome_accent_mask_low.store(pattern.accent_mask_low, std::memory_order_relaxed);
            control->metronome_accent_mask_high.store(pattern.accent_mask_high, std::memory_order_relaxed);
            return true;
        }
        case EngineCommandType::SetMetronomeLevel:
            control->metronome_level_ppm.store(clamp_gain(command.value), std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetRemoteLevel:
            control->remote_level_ppm.store(clamp_gain(command.value), std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetSendLevel:
            control->send_level_ppm.store(clamp_gain(command.value), std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetLocalMonitorEnabled:
            control->local_monitor_enabled.store(command.enabled, std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetLocalMonitorLevel:
            control->local_monitor_level_ppm.store(clamp_gain(command.value), std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetPlaybackRatio:
            control->playback_ratio_ppm.store(clamp_ratio(command.value), std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetMetronomeMode:
            control->metronome_mode.store(std::clamp(command.value, 0, 2), std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetLeaderAudioLocalClick:
            control->leader_audio_local_click.store(command.enabled, std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetMetronomeEpoch:
            control->metronome_epoch_sample_time.store(command.frame, std::memory_order_relaxed);
            control->metronome_epoch_valid.store(command.enabled, std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetMetronomeRenderOffset:
            control->metronome_render_offset_frames.store(command.signed_value, std::memory_order_relaxed);
            return true;
        case EngineCommandType::SetRecordingLatencyAdjustment: {
            const std::int64_t adjustment = std::clamp<std::int64_t>(command.signed_value, -1000000, 1000000);
            const std::int64_t reported =
                static_cast<std::int64_t>(control->input_latency_frames.load(std::memory_order_relaxed)) +
                static_cast<std::int64_t>(control->output_latency_frames.load(std::memory_order_relaxed));
            control->recording_latency_adjustment_frames.store(adjustment, std::memory_order_relaxed);
            control->recording_latency_compensation_frames.store(
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, reported + adjustment)),
                std::memory_order_relaxed);
            return true;
        }
        case EngineCommandType::ScheduleTransport: {
            if (command.transport_action != EngineTransportAction::TrackRestart &&
                command.transport_action != EngineTransportAction::TrackStop &&
                command.transport_action != EngineTransportAction::TrackPlay &&
                command.transport_action != EngineTransportAction::RecordStart &&
                command.transport_action != EngineTransportAction::RecordStop) {
                error = "transport action is invalid";
                return false;
            }
            if (command.transport_countdown_start_frame > command.transport_target_frame) {
                error = "transport countdown begins after its target frame";
                return false;
            }
            transport_target_frame.store(command.transport_target_frame, std::memory_order_relaxed);
            transport_musical_frame.store(command.transport_musical_frame, std::memory_order_relaxed);
            transport_countdown_start_frame.store(
                command.transport_countdown_start_frame,
                std::memory_order_relaxed);
            transport_action.store(command.transport_action, std::memory_order_relaxed);
            transport_cookie.store(command.cookie, std::memory_order_relaxed);
            transport_revision.fetch_add(1, std::memory_order_relaxed);
            transport_pending.store(true, std::memory_order_release);
            return true;
        }
        case EngineCommandType::CancelTransport:
            prepared_source->cancelScheduled();
            transport_pending.store(false, std::memory_order_release);
            transport_action.store(EngineTransportAction::None, std::memory_order_relaxed);
            transport_cookie.store(0, std::memory_order_relaxed);
            transport_revision.fetch_add(1, std::memory_order_relaxed);
            return true;
        case EngineCommandType::LoadPreparedTrack: {
            const auto result = load_prepared_track(
                *prepared_source,
                std::filesystem::path(std::string(bounded_text(command.text))),
                config.sample_rate,
                config.prepared_track_max_frames,
                applied_frame);
            if (!result.ok) {
                error = result.error;
                return false;
            }
            push_event(
                EngineEventType::PreparedTrackLoaded,
                true,
                command.cookie,
                command.apply_frame,
                applied_frame,
                result.frames);
            return true;
        }
        case EngineCommandType::PreparedPlay:
            return queue_prepared({audio::PreparedTrackSource::CommandType::Play, 0, command.frame, 0, 0, 1000000});
        case EngineCommandType::PreparedStop:
            return queue_prepared({audio::PreparedTrackSource::CommandType::Stop, 0, command.frame, 0, 0, 1000000});
        case EngineCommandType::PreparedSeek:
            return queue_prepared({audio::PreparedTrackSource::CommandType::Seek, 0, command.frame, command.frame_end, 0, 1000000});
        case EngineCommandType::PreparedSetLoop: {
            if (command.signed_value < 0 ||
                (command.signed_value != 0 &&
                 static_cast<std::uint64_t>(command.signed_value) < command.frame_end)) {
                error = "prepared track loop bounds are invalid";
                return false;
            }
            return queue_prepared({
                audio::PreparedTrackSource::CommandType::SetLoop,
                0,
                command.frame,
                command.frame_end,
                static_cast<std::uint64_t>(command.signed_value),
                1000000});
        }
        case EngineCommandType::PreparedSetLevel:
            return queue_prepared({audio::PreparedTrackSource::CommandType::SetLevel, 0, command.frame, 0, 0, clamp_gain(command.value)});
        case EngineCommandType::StartJamRecording: {
            std::string recorder_error;
            if (!recorder->start(
                    std::filesystem::path(std::string(bounded_text(command.text))),
                    config.sample_rate,
                    recorder_error)) {
                error = recorder_error;
                return false;
            }
            push_event(EngineEventType::JamRecordingStarted, true, command.cookie, command.apply_frame, applied_frame, 0);
            return true;
        }
        case EngineCommandType::StopJamRecording: {
            std::string recorder_error;
            if (!recorder->stop(recorder_error)) {
                error = recorder_error;
                return false;
            }
            push_event(EngineEventType::JamRecordingStopped, true, command.cookie, command.apply_frame, applied_frame, 0);
            return true;
        }
        case EngineCommandType::ArmTrackTake: {
            std::string recorder_error;
            if (!track_take_recorder->arm(
                    std::string(bounded_text(command.id)),
                    std::filesystem::path(std::string(bounded_text(command.text))),
                    config.sample_rate,
                    recorder_error)) {
                error = recorder_error;
                return false;
            }
            return true;
        }
        case EngineCommandType::StartTrackTake: {
            std::string recorder_error;
            if (!track_take_recorder->start_at(command.frame, recorder_error)) {
                error = recorder_error;
                return false;
            }
            return true;
        }
        case EngineCommandType::StopTrackTake: {
            std::string recorder_error;
            if (!track_take_recorder->stop_at(command.frame, recorder_error)) {
                error = recorder_error;
                return false;
            }
            return true;
        }
        case EngineCommandType::CancelTrackTake:
            track_take_recorder->cancel();
            return true;
        }
        error = "unknown engine command";
        return false;
    }

    bool schedule(const EngineCommand& command) noexcept
    {
        std::lock_guard<std::mutex> lock(scheduled_mutex);
        if (scheduled_count == scheduled.size()) {
            ++scheduled_rejections;
            return false;
        }
        scheduled[scheduled_count++] = {command, schedule_order++};
        scheduled_high_water = std::max(scheduled_high_water, scheduled_count);
        return true;
    }

    bool pop_due(std::uint64_t engine_frame, EngineCommand& command) noexcept
    {
        std::lock_guard<std::mutex> lock(scheduled_mutex);
        std::size_t selected = scheduled_count;
        for (std::size_t index = 0; index < scheduled_count; ++index) {
            const auto& candidate = scheduled[index];
            if (candidate.command.apply_frame > engine_frame) {
                continue;
            }
            if (selected == scheduled_count ||
                candidate.command.apply_frame < scheduled[selected].command.apply_frame ||
                (candidate.command.apply_frame == scheduled[selected].command.apply_frame &&
                 candidate.order < scheduled[selected].order)) {
                selected = index;
            }
        }
        if (selected == scheduled_count) {
            return false;
        }
        command = scheduled[selected].command;
        for (std::size_t index = selected + 1; index < scheduled_count; ++index) {
            scheduled[index - 1] = scheduled[index];
        }
        --scheduled_count;
        return true;
    }

    void process_command(const EngineCommand& command, std::uint64_t engine_frame)
    {
        std::string error;
        const bool ok = apply_command(command, engine_frame, error);
        push_event(
            ok ? EngineEventType::CommandApplied : EngineEventType::CommandRejected,
            ok,
            command.cookie,
            command.apply_frame,
            engine_frame,
            0,
            error);
    }

    void commit_due_transport(std::uint64_t engine_frame) noexcept
    {
        if (!transport_pending.load(std::memory_order_acquire) ||
            engine_frame < transport_target_frame.load(std::memory_order_relaxed)) {
            return;
        }
        transport_pending.store(false, std::memory_order_release);
        const auto action = transport_action.load(std::memory_order_relaxed);
        if (action == EngineTransportAction::TrackRestart || action == EngineTransportAction::RecordStart) {
            control->metronome_epoch_sample_time.store(
                transport_musical_frame.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            control->metronome_epoch_valid.store(true, std::memory_order_relaxed);
        }
        const std::uint64_t revision = transport_revision.load(std::memory_order_relaxed);
        transport_commit_count.fetch_add(1, std::memory_order_relaxed);
        push_event(
            EngineEventType::TransportCommitted,
            true,
            transport_cookie.load(std::memory_order_relaxed),
            transport_target_frame.load(std::memory_order_relaxed),
            engine_frame,
            revision);
    }

    void supervise() noexcept
    {
        try {
            while (!stop_requested.load(std::memory_order_acquire)) {
                EngineCommand incoming;
                std::size_t processed = 0;
                while (processed < 32 && commands.pop(incoming)) {
                    if (incoming.type == EngineCommandType::Stop || incoming.apply_frame == 0) {
                        process_command(
                            incoming,
                            control->engine_frame_counter.load(std::memory_order_acquire));
                    } else if (!schedule(incoming)) {
                        push_event(
                            EngineEventType::CommandRejected,
                            false,
                            incoming.cookie,
                            incoming.apply_frame,
                            control->engine_frame_counter.load(std::memory_order_relaxed),
                            0,
                            "scheduled command capacity exhausted");
                    }
                    ++processed;
                }
                const std::uint64_t frame = control->engine_frame_counter.load(std::memory_order_acquire);
                EngineCommand due;
                processed = 0;
                while (processed < 32 && pop_due(frame, due)) {
                    process_command(due, frame);
                    ++processed;
                }
                commit_due_transport(frame);
                if (track_take_recorder != nullptr) {
                    const auto completion = track_take_recorder->consume_completion();
                    if (completion.available) {
                        push_event(
                            EngineEventType::TrackTakeCompleted,
                            completion.ok,
                            0,
                            completion.stop_frame,
                            frame,
                            completion.frames_written,
                            completion.ok ? std::string_view{} : std::string_view(completion.error));
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            set_lifecycle(EngineLifecycle::Stopping);
        } catch (const std::exception& error) {
            push_event(EngineEventType::Error, false, 0, 0, 0, 0, error.what());
            lifecycle.store(EngineLifecycle::Failed, std::memory_order_release);
            stop_requested.store(true, std::memory_order_release);
        } catch (...) {
            push_event(EngineEventType::Error, false, 0, 0, 0, 0, "unknown engine supervisor failure");
            lifecycle.store(EngineLifecycle::Failed, std::memory_order_release);
            stop_requested.store(true, std::memory_order_release);
        }
    }
};

Engine::Engine()
    : impl_(std::make_unique<Impl>())
{
}

Engine::~Engine()
{
    requestStop();
    join();
}

void Engine::start(const EngineConfig& requested)
{
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    if (impl_->has_config ||
        impl_->lifecycle.load(std::memory_order_acquire) != EngineLifecycle::Stopped ||
        impl_->stream != nullptr || impl_->supervisor.joinable()) {
        throw std::runtime_error("engine instances are single-start and this instance has already run");
    }
    if (requested.sample_rate <= 0 || requested.sample_rate > 384000) {
        throw std::runtime_error("engine sample rate must be 1..384000 Hz");
    }
    if (requested.backend != EngineAudioBackend::Device && requested.backend != EngineAudioBackend::Headless) {
        throw std::runtime_error("engine audio backend is invalid");
    }
    if (requested.metronome_mode > EngineMetronomeMode::ListenerCompensated) {
        throw std::runtime_error("engine metronome mode is invalid");
    }
    if (requested.test_input > EngineTestInput::MetronomePulse) {
        throw std::runtime_error("engine test-input mode is invalid");
    }
    if (requested.audio_buffer_frames < 0) {
        throw std::runtime_error("engine audio buffer size cannot be negative");
    }
    if (requested.headless_clock_drift_ppm < -5000 || requested.headless_clock_drift_ppm > 5000) {
        throw std::runtime_error("engine headless clock drift must be -5000..5000 ppm");
    }
    if (requested.backend != EngineAudioBackend::Headless && requested.headless_clock_drift_ppm != 0) {
        throw std::runtime_error("engine headless clock drift requires the headless backend");
    }
    if (requested.backend == EngineAudioBackend::Device && requested.audio_device_id < 0) {
        throw std::runtime_error("device engine requires a non-negative audio device id");
    }
    if (requested.capture_ring_frames == 0 || requested.playback_ring_frames == 0) {
        throw std::runtime_error("engine ring capacities must be positive");
    }
    if (requested.playback_prefill_frames > requested.playback_ring_frames) {
        throw std::runtime_error("engine playback prefill must fit the playback ring");
    }
    if (requested.backend == EngineAudioBackend::Device && requested.channels.input.empty()) {
        throw std::runtime_error("device engine requires at least one input channel");
    }
    const std::size_t maximum_prepared_track_frames =
        static_cast<std::size_t>(requested.sample_rate) * 60U * 5U;
    if (requested.prepared_track_max_frames > maximum_prepared_track_frames) {
        throw std::runtime_error("engine prepared-track capacity exceeds the five-minute limit");
    }

    impl_->lifecycle.store(EngineLifecycle::Starting, std::memory_order_release);
    impl_->config = requested;
    impl_->config.metronome_pattern = metronome::sanitize(requested.metronome_pattern);
    if (impl_->config.prepared_track_max_frames == 0) {
        impl_->config.prepared_track_max_frames = maximum_prepared_track_frames;
    }
    impl_->has_config = true;
    impl_->stop_requested.store(false, std::memory_order_relaxed);

    try {
        impl_->control = std::make_unique<audio::StreamControl>();
        impl_->recorder = std::make_unique<audio::OutputRecorder>();
        impl_->track_take_recorder = std::make_unique<audio::TrackTakeRecorder>();
        impl_->prepared_source = std::make_unique<audio::PreparedTrackSource>(
            impl_->config.prepared_track_max_frames);
        impl_->capture_ring = std::make_unique<audio::MonoRingBuffer>(impl_->config.capture_ring_frames);
        impl_->playback_ring = std::make_unique<audio::MonoRingBuffer>(impl_->config.playback_ring_frames);
        impl_->capture_ring->set_diagnostics_enabled(impl_->config.diagnostics_enabled);
        impl_->playback_ring->set_diagnostics_enabled(impl_->config.diagnostics_enabled);
        impl_->playback_ring->set_depth_bucket_thresholds(static_cast<double>(impl_->config.sample_rate));

        auto& control = *impl_->control;
        const auto pattern = impl_->config.metronome_pattern;
        control.prepared_source = impl_->prepared_source.get();
        control.metronome_enabled.store(impl_->config.metronome_enabled, std::memory_order_relaxed);
        control.metronome_bpm.store(pattern.bpm, std::memory_order_relaxed);
        control.metronome_beats_per_bar.store(pattern.beats_per_bar, std::memory_order_relaxed);
        control.metronome_division.store(pattern.division, std::memory_order_relaxed);
        control.metronome_step_count.store(pattern.step_count, std::memory_order_relaxed);
        control.metronome_play_mask_low.store(pattern.play_mask_low, std::memory_order_relaxed);
        control.metronome_play_mask_high.store(pattern.play_mask_high, std::memory_order_relaxed);
        control.metronome_accent_mask_low.store(pattern.accent_mask_low, std::memory_order_relaxed);
        control.metronome_accent_mask_high.store(pattern.accent_mask_high, std::memory_order_relaxed);
        control.metronome_level_ppm.store(clamp_gain(impl_->config.metronome_level_ppm), std::memory_order_relaxed);
        control.remote_level_ppm.store(clamp_gain(impl_->config.remote_level_ppm), std::memory_order_relaxed);
        control.send_level_ppm.store(clamp_gain(impl_->config.send_level_ppm), std::memory_order_relaxed);
        control.local_monitor_enabled.store(impl_->config.local_monitor_enabled, std::memory_order_relaxed);
        control.local_monitor_level_ppm.store(clamp_gain(impl_->config.local_monitor_level_ppm), std::memory_order_relaxed);
        control.playback_ratio_ppm.store(1000000, std::memory_order_relaxed);
        control.metronome_mode.store(static_cast<int>(impl_->config.metronome_mode), std::memory_order_relaxed);
        control.leader_audio_local_click.store(impl_->config.leader_audio_local_click, std::memory_order_relaxed);
        control.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        control.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        control.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        control.engine_frame_counter.store(0, std::memory_order_relaxed);
        control.test_input_mode.store(static_cast<int>(impl_->config.test_input), std::memory_order_relaxed);
        control.test_input_level_ppm.store(
            std::clamp(impl_->config.test_input_level_ppm, 0, 1000000),
            std::memory_order_relaxed);
        control.network_capture_requested_enabled.store(false, std::memory_order_relaxed);
        control.network_capture_enabled.store(false, std::memory_order_relaxed);
        control.network_capture_generation_requested.store(0, std::memory_order_relaxed);
        control.network_capture_generation_applied.store(0, std::memory_order_relaxed);
        control.network_capture_epoch_frame.store(0, std::memory_order_relaxed);
        control.network_playback_enabled.store(false, std::memory_order_relaxed);
        control.network_playback_enabled_applied.store(false, std::memory_order_relaxed);

        const long buffer_frames = impl_->config.audio_buffer_frames > 0
            ? impl_->config.audio_buffer_frames
            : 128;
        if (impl_->config.backend == EngineAudioBackend::Headless) {
            impl_->stream = std::make_unique<HeadlessDeviceStream>(
                static_cast<double>(impl_->config.sample_rate),
                buffer_frames,
                impl_->config.headless_clock_drift_ppm,
                impl_->config.input_channels,
                impl_->config.channels,
                *impl_->capture_ring,
                *impl_->playback_ring,
                impl_->config.playback_prefill_frames,
                control,
                impl_->recorder.get(),
                impl_->track_take_recorder.get());
        } else {
            impl_->stream = audio::start_duplex_stream(
                impl_->config.audio_device_id,
                static_cast<double>(impl_->config.sample_rate),
                impl_->config.audio_buffer_frames,
                impl_->config.input_channels,
                impl_->config.channels,
                *impl_->capture_ring,
                *impl_->playback_ring,
                impl_->config.playback_prefill_frames,
                control,
                impl_->recorder.get(),
                impl_->track_take_recorder.get());
        }
        impl_->stream_info = impl_->stream != nullptr ? impl_->stream->info() : audio::StreamInfo{};
        const double active_rate = impl_->stream_info.sample_rate;
        if (active_rate > 0.0 &&
            std::abs(active_rate - static_cast<double>(impl_->config.sample_rate)) > 1.0) {
            std::ostringstream message;
            message << "active audio sample rate mismatch: requested " << impl_->config.sample_rate
                    << " Hz but device started at " << active_rate << " Hz";
            throw std::runtime_error(message.str());
        }
        impl_->supervisor = std::thread([implementation = impl_.get()] { implementation->supervise(); });
        impl_->set_lifecycle(EngineLifecycle::Local);
    } catch (...) {
        impl_->stream.reset();
        impl_->playback_ring.reset();
        impl_->capture_ring.reset();
        impl_->prepared_source.reset();
        impl_->track_take_recorder.reset();
        impl_->recorder.reset();
        impl_->control.reset();
        impl_->lifecycle.store(EngineLifecycle::Failed, std::memory_order_release);
        throw;
    }
}

void Engine::requestStop() noexcept
{
    if (impl_ == nullptr) {
        return;
    }
    impl_->stop_requested.store(true, std::memory_order_release);
}

void Engine::join() noexcept
{
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->supervisor.joinable()) {
        impl_->supervisor.join();
    }
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    impl_->attachment_active.store(false, std::memory_order_relaxed);
    if (impl_->control != nullptr) {
        impl_->control->network_capture_requested_enabled.store(false, std::memory_order_release);
        impl_->control->network_playback_enabled.store(false, std::memory_order_release);
    }
    impl_->stream.reset();
    if (impl_->recorder != nullptr && impl_->recorder->stats().active) {
        std::string ignored;
        (void)impl_->recorder->stop(ignored);
    }
    if (impl_->track_take_recorder != nullptr) {
        impl_->track_take_recorder->cancel();
    }
    if (impl_->lifecycle.load(std::memory_order_relaxed) != EngineLifecycle::Failed) {
        impl_->set_lifecycle(EngineLifecycle::Stopped);
    }
}

bool Engine::submit(const EngineCommand& command) noexcept
{
    if (impl_ == nullptr || impl_->lifecycle.load(std::memory_order_acquire) != EngineLifecycle::Local) {
        return false;
    }
    return impl_->commands.push(command);
}

EngineSnapshot Engine::snapshot() const noexcept
{
    EngineSnapshot result;
    if (impl_ == nullptr) {
        return result;
    }
    result.lifecycle = impl_->lifecycle.load(std::memory_order_acquire);
    result.backend = impl_->config.backend;
    result.command_queue_capacity = kCommandCapacity;
    result.command_queue_depth = impl_->commands.depth();
    result.command_queue_high_water = impl_->commands.highWater();
    result.command_queue_rejections = impl_->commands.drops();
    {
        std::lock_guard<std::mutex> lock(impl_->scheduled_mutex);
        result.scheduled_command_capacity = kScheduledCommandCapacity;
        result.scheduled_command_depth = impl_->scheduled_count;
        result.scheduled_command_high_water = impl_->scheduled_high_water;
        result.scheduled_command_rejections = impl_->scheduled_rejections;
    }
    result.event_queue_capacity = kEventCapacity;
    result.event_queue_depth = impl_->events.depth();
    result.event_queue_high_water = impl_->events.highWater();
    result.event_queue_drops = impl_->events.drops();
    if (impl_->control == nullptr) {
        return result;
    }
    const auto& control = *impl_->control;
    result.engine_frame = control.engine_frame_counter.load(std::memory_order_acquire);
    result.frame_clock_active = impl_->stream != nullptr;
    result.network_capture_enabled = control.network_capture_enabled.load(std::memory_order_relaxed);
    result.network_capture_generation = control.network_capture_generation_requested.load(std::memory_order_relaxed);
    result.network_capture_ready = result.network_capture_enabled &&
        result.network_capture_generation == control.network_capture_generation_applied.load(std::memory_order_acquire);
    result.network_capture_epoch_frame = control.network_capture_epoch_frame.load(std::memory_order_relaxed);
    result.network_capture_stale_frames_discarded =
        control.network_capture_stale_frames_discarded.load(std::memory_order_relaxed);
    result.network_capture_attach_count = impl_->capture_attach_count.load(std::memory_order_relaxed);
    result.network_capture_detach_count = impl_->capture_detach_count.load(std::memory_order_relaxed);
    result.network_playback_enabled = control.network_playback_enabled.load(std::memory_order_relaxed);
    result.metronome_enabled = control.metronome_enabled.load(std::memory_order_relaxed);
    result.metronome_mode = static_cast<EngineMetronomeMode>(
        std::clamp(control.metronome_mode.load(std::memory_order_relaxed), 0, 2));
    result.metronome_pattern = metronome::sanitize({
        control.metronome_bpm.load(std::memory_order_relaxed),
        control.metronome_beats_per_bar.load(std::memory_order_relaxed),
        control.metronome_division.load(std::memory_order_relaxed),
        control.metronome_step_count.load(std::memory_order_relaxed),
        control.metronome_play_mask_low.load(std::memory_order_relaxed),
        control.metronome_play_mask_high.load(std::memory_order_relaxed),
        control.metronome_accent_mask_low.load(std::memory_order_relaxed),
        control.metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
    result.metronome_level_ppm = control.metronome_level_ppm.load(std::memory_order_relaxed);
    result.remote_level_ppm = control.remote_level_ppm.load(std::memory_order_relaxed);
    result.send_level_ppm = control.send_level_ppm.load(std::memory_order_relaxed);
    result.local_monitor_enabled = control.local_monitor_enabled.load(std::memory_order_relaxed);
    result.local_monitor_level_ppm = control.local_monitor_level_ppm.load(std::memory_order_relaxed);
    result.playback_ratio_ppm = control.playback_ratio_ppm.load(std::memory_order_relaxed);
    result.metronome_epoch_frame = control.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    result.metronome_epoch_valid = control.metronome_epoch_valid.load(std::memory_order_relaxed);
    result.metronome_render_offset_frames = control.metronome_render_offset_frames.load(std::memory_order_relaxed);
    result.transport_pending = impl_->transport_pending.load(std::memory_order_acquire);
    result.transport_revision = impl_->transport_revision.load(std::memory_order_relaxed);
    result.transport_action = impl_->transport_action.load(std::memory_order_relaxed);
    result.transport_target_frame = impl_->transport_target_frame.load(std::memory_order_relaxed);
    result.transport_musical_frame = impl_->transport_musical_frame.load(std::memory_order_relaxed);
    result.transport_countdown_start_frame =
        impl_->transport_countdown_start_frame.load(std::memory_order_relaxed);
    result.transport_commit_count = impl_->transport_commit_count.load(std::memory_order_relaxed);
    result.input_peak_ppm = control.input_peak_ppm.load(std::memory_order_relaxed);
    result.send_peak_ppm = control.send_peak_ppm.load(std::memory_order_relaxed);
    result.monitor_peak_ppm = control.monitor_peak_ppm.load(std::memory_order_relaxed);
    result.remote_peak_ppm = control.remote_peak_ppm.load(std::memory_order_relaxed);
    result.metronome_peak_ppm = control.metronome_peak_ppm.load(std::memory_order_relaxed);
    result.output_peak_ppm = control.output_peak_ppm.load(std::memory_order_relaxed);
    result.output_clipped_samples = control.output_clipped_samples.load(std::memory_order_relaxed);
    result.prepared_source_frame = control.prepared_source_frame.load(std::memory_order_relaxed);
    result.prepared_source_scheduled_start_frame =
        control.prepared_source_scheduled_start_frame.load(std::memory_order_relaxed);
    result.prepared_source_actual_start_frame =
        control.prepared_source_actual_start_frame.load(std::memory_order_relaxed);
    result.prepared_source_underruns = control.prepared_source_underruns.load(std::memory_order_relaxed);
    result.prepared_source_busy_events = control.prepared_source_busy_events.load(std::memory_order_relaxed);
    result.prepared_source_playing = impl_->prepared_source != nullptr && impl_->prepared_source->playing();
    if (impl_->capture_ring != nullptr) {
        result.capture_ring_capacity_frames = impl_->capture_ring->capacity();
        result.capture_ring_depth_frames = impl_->capture_ring->available_read();
        result.capture_ring = impl_->capture_ring->stats();
    }
    if (impl_->playback_ring != nullptr) {
        result.playback_ring_capacity_frames = impl_->playback_ring->capacity();
        result.playback_ring_depth_frames = impl_->playback_ring->available_read();
        result.playback_ring = impl_->playback_ring->stats();
    }
    if (impl_->stream != nullptr) {
        result.sample_rate = impl_->stream_info.sample_rate;
        result.audio_buffer_frames = impl_->stream_info.buffer_size;
        result.input_latency_frames = impl_->stream_info.input_latency_frames;
        result.output_latency_frames = impl_->stream_info.output_latency_frames;
        result.recording_latency_adjustment_frames =
            control.recording_latency_adjustment_frames.load(std::memory_order_relaxed);
        result.recording_latency_compensation_frames =
            control.recording_latency_compensation_frames.load(std::memory_order_relaxed);
        result.callbacks = impl_->stream->callbacks();
        result.playback_prefilled = impl_->stream->playback_prefilled();
        result.callback_timing = impl_->stream->callback_timing_stats();
    }
    if (impl_->recorder != nullptr) {
        const auto recording = impl_->recorder->snapshot();
        result.jam_recording.active = recording.active;
        result.jam_recording.start_frame = recording.start_audio_frame;
        result.jam_recording.stop_frame = recording.stop_audio_frame;
        result.jam_recording.frames_queued = recording.frames_queued;
        result.jam_recording.frames_written = recording.frames_written;
        result.jam_recording.dropped_frames = recording.dropped_frames;
        result.jam_recording.drop_events = recording.drop_events;
        result.jam_recording.writer_errors = recording.writer_errors;
        result.jam_recording.queue_depth_frames = recording.queue_depth_frames;
        result.jam_recording.queue_capacity_frames = recording.queue_capacity_frames;
    }
    if (impl_->track_take_recorder != nullptr) {
        const auto take = impl_->track_take_recorder->snapshot();
        result.track_take = {
            take.armed,
            take.recording,
            take.finalized,
            take.start_frame,
            take.stop_frame,
            take.frames_queued,
            take.frames_written,
            take.dropped_frames,
            take.drop_events,
            take.writer_errors,
            take.queue_depth_frames,
            take.queue_capacity_frames,
        };
    }
    return result;
}

EngineGuiPeakSnapshot Engine::consumeGuiPeaks() noexcept
{
    EngineGuiPeakSnapshot result;
    if (impl_ == nullptr || impl_->control == nullptr) {
        return result;
    }
    auto& control = *impl_->control;
    result.input_peak_ppm = control.gui_input_peak_ppm.exchange(0, std::memory_order_acq_rel);
    result.monitor_peak_ppm = control.gui_monitor_peak_ppm.exchange(0, std::memory_order_acq_rel);
    result.remote_peak_ppm = control.gui_remote_peak_ppm.exchange(0, std::memory_order_acq_rel);
    result.metronome_peak_ppm = control.gui_metronome_peak_ppm.exchange(0, std::memory_order_acq_rel);
    result.output_peak_ppm = control.gui_output_peak_ppm.exchange(0, std::memory_order_acq_rel);
    return result;
}

EngineColdSnapshot Engine::coldSnapshot() const
{
    EngineColdSnapshot result;
    if (impl_ == nullptr) {
        return result;
    }
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    result.stream = impl_->stream_info;
    if (impl_->recorder != nullptr) {
        const auto recording = impl_->recorder->stats();
        result.recording_folder = recording.folder;
        result.recording_sample_rate = recording.sample_rate;
    }
    return result;
}

bool Engine::pollEvent(EngineEvent& event) noexcept
{
    return impl_ != nullptr && impl_->events.pop(event);
}

NetworkCaptureAttachment Engine::attachNetworkCapture() noexcept
{
    if (impl_ == nullptr ||
        impl_->lifecycle.load(std::memory_order_acquire) != EngineLifecycle::Local ||
        impl_->control == nullptr ||
        impl_->capture_ring == nullptr ||
        impl_->stream == nullptr) {
        return {};
    }
    bool expected = false;
    if (!impl_->attachment_active.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return {impl_->active_attachment_generation.load(std::memory_order_acquire)};
    }
    impl_->control->network_capture_requested_enabled.store(true, std::memory_order_release);
    const std::uint64_t generation =
        impl_->control->network_capture_generation_requested.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    impl_->active_attachment_generation.store(generation, std::memory_order_release);
    impl_->capture_frames_popped.store(0, std::memory_order_relaxed);
    impl_->capture_attach_count.fetch_add(1, std::memory_order_relaxed);
    impl_->control->network_playback_enabled.store(true, std::memory_order_release);
    return {generation};
}

void Engine::detachNetworkCapture(NetworkCaptureAttachment attachment) noexcept
{
    if (impl_ == nullptr || impl_->control == nullptr || attachment.generation == 0 ||
        attachment.generation != impl_->active_attachment_generation.load(std::memory_order_acquire)) {
        return;
    }
    impl_->control->network_capture_requested_enabled.store(false, std::memory_order_release);
    impl_->control->network_playback_enabled.store(false, std::memory_order_release);
    (void)impl_->control->network_capture_generation_requested.fetch_add(1, std::memory_order_acq_rel);
    impl_->attachment_active.store(false, std::memory_order_release);
    impl_->capture_detach_count.fetch_add(1, std::memory_order_relaxed);
}

bool Engine::networkCaptureReady(NetworkCaptureAttachment attachment) const noexcept
{
    if (impl_ == nullptr || impl_->control == nullptr || attachment.generation == 0) {
        return false;
    }
    return impl_->attachment_active.load(std::memory_order_acquire) &&
        impl_->active_attachment_generation.load(std::memory_order_acquire) == attachment.generation &&
        impl_->control->network_capture_enabled.load(std::memory_order_acquire) &&
        impl_->control->network_capture_generation_applied.load(std::memory_order_acquire) == attachment.generation;
}

CapturedAudioBlock Engine::popNetworkCapture(
    NetworkCaptureAttachment attachment,
    std::span<std::int32_t> output) noexcept
{
    if (!networkCaptureReady(attachment) || impl_->capture_ring == nullptr || output.empty()) {
        return {};
    }
    // Network packets are fixed-size blocks.  Consuming a partial device
    // callback here loses those frames permanently when the device buffer is
    // smaller than the packet size (for example 32-frame ASIO callbacks and
    // 64-frame packets).  There is one capture consumer and the producer can
    // only increase the readable depth, so this availability check is stable
    // until the following pop.
    if (impl_->capture_ring->available_read() < output.size()) {
        return {};
    }
    const std::size_t frames = impl_->capture_ring->pop(output);
    if (frames != output.size()) {
        return {};
    }
    const std::uint64_t offset = impl_->capture_frames_popped.fetch_add(frames, std::memory_order_relaxed);
    const std::uint64_t first =
        impl_->control->network_capture_epoch_frame.load(std::memory_order_acquire) + offset;
    return {first, frames};
}

std::size_t Engine::networkPlaybackDepth() const noexcept
{
    return impl_ != nullptr && impl_->playback_ring != nullptr
        ? impl_->playback_ring->available_read()
        : 0;
}

std::size_t Engine::pushNetworkPlayback(std::span<const std::int32_t> input) noexcept
{
    return impl_ != nullptr &&
        impl_->playback_ring != nullptr &&
        impl_->control != nullptr &&
        impl_->control->network_playback_enabled.load(std::memory_order_acquire)
        ? impl_->playback_ring->push(input)
        : 0;
}

void Engine::requestNetworkPlaybackDrop(std::size_t frames) noexcept
{
    if (impl_ != nullptr && impl_->playback_ring != nullptr) {
        impl_->playback_ring->request_drop_oldest(frames);
    }
}

void Engine::setNetworkPlaybackRatio(double ratio) noexcept
{
    if (impl_ == nullptr || impl_->control == nullptr || !std::isfinite(ratio)) {
        return;
    }
    const int ratio_ppm = clamp_ratio(static_cast<int>(std::llround(ratio * 1000000.0)));
    impl_->control->playback_ratio_ppm.store(ratio_ppm, std::memory_order_relaxed);
}

const EngineConfig* Engine::config() const noexcept
{
    return impl_ != nullptr && impl_->has_config ? &impl_->config : nullptr;
}

bool engine_command_set_text(EngineCommand& command, std::string_view text) noexcept
{
    return set_bounded_text(command.text, text);
}

bool engine_command_set_id(EngineCommand& command, std::string_view id) noexcept
{
    return set_bounded_text(command.id, id);
}

std::string_view engine_event_text(const EngineEvent& event) noexcept
{
    return bounded_text(event.text);
}

} // namespace jam2
