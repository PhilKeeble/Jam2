#include "recording_calibration.hpp"
#include "audio_device.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace jam2::audio {
void set_recording_calibration(StreamControl& c, std::int64_t adjustment) noexcept
{
    c.device_recording_calibration_frames.store(
        std::clamp<std::int64_t>(adjustment, -1000000, 1000000), std::memory_order_release);
}
std::int64_t applied_recording_calibration_frames(const StreamControl& c) noexcept
{
    const auto* router = c.input_source_router;
    if (!router) return 0;
    const auto kind = router->recording_input_kind();
    if (kind != RecordingInputKind::PhysicalAudio && kind != RecordingInputKind::Mixed) return 0;
    return c.device_recording_calibration_frames.load(std::memory_order_acquire);
}
std::uint64_t input_take_compensation_frames(const StreamControl& c) noexcept
{
    const auto* router = c.input_source_router;
    const auto kind = router ? router->recording_input_kind() : RecordingInputKind::PhysicalAudio;
    const std::int64_t input = kind == RecordingInputKind::MidiInstrument ? 0 :
        c.input_latency_frames.load(std::memory_order_relaxed);
    const auto base = std::max<std::int64_t>(0, input +
        c.output_latency_frames.load(std::memory_order_relaxed) +
        c.recording_latency_adjustment_frames.load(std::memory_order_relaxed) +
        applied_recording_calibration_frames(c));
    return static_cast<std::uint64_t>(base) + (router ? router->recording_latency_frames() : 0);
}

RecordingCalibrationCapture::RecordingCalibrationCapture(int rate) : rate_(rate)
{
    if (rate < 8000 || rate > 384000) throw std::invalid_argument("Invalid calibration sample rate");
    input_.resize(static_cast<std::size_t>(rate) * 5);
    reference_.resize(input_.size());
    constexpr double pi = 3.14159265358979323846;
    for (const double seconds : {.6, 1.27, 1.95, 2.71, 3.48, 4.21}) {
        const auto start = static_cast<std::size_t>(std::llround(seconds * rate));
        clicks_.push_back(start);
        const auto length = static_cast<std::size_t>(rate * .012);
        for (std::size_t i = 0; i < length; ++i) {
            const double t = static_cast<double>(i) / rate;
            const double window = std::sin(pi * i / length);
            reference_[start + i] = static_cast<float>(.12 * window * window *
                std::sin(2 * pi * (500 * t + 5500 * t * t / (.024))));
        }
    }
}
bool RecordingCalibrationCapture::start(std::size_t channel) noexcept
{
    const auto s = state();
    if (s != State::Idle && s != State::Finished && s != State::Cancelled) return false;
    channel_.store(channel, std::memory_order_relaxed);
    cancelled_.store(false, std::memory_order_relaxed);
    state_.store(State::Requested, std::memory_order_release);
    return true;
}
void RecordingCalibrationCapture::cancel() noexcept
{
    cancelled_.store(true, std::memory_order_release);
    // An analysis job retains exclusive ownership of the buffers until it exits.
    auto current = state();
    while (current != State::Analysing && current != State::Cancelled &&
        !state_.compare_exchange_weak(current, State::Cancelled, std::memory_order_acq_rel)) {}
}
void RecordingCalibrationCapture::capture_input(std::span<const std::int32_t> input) noexcept
{
    if (state() == State::Requested) {
        cursor_ = captured_frames_ = 0; reported_sum_ = reported_count_ = reported_max_ = 0;
        reported_min_ = (std::numeric_limits<std::uint64_t>::max)();
        auto expected = State::Requested;
        if (!state_.compare_exchange_strong(expected, State::Capturing, std::memory_order_acq_rel)) return;
    }
    if (state() != State::Capturing) return;
    const auto count = std::min(input.size(), input_.size() - cursor_);
    for (std::size_t i = 0; i < count; ++i) input_[cursor_ + i] = input[i] / 2147483648.f;
    captured_frames_ = count;
}
void RecordingCalibrationCapture::render(std::span<std::int32_t> output, std::uint64_t reported) noexcept
{
    if (state() == State::Requested) { std::fill(output.begin(), output.end(), 0); return; }
    if (state() != State::Capturing) return;
    const auto count = std::min(output.size(), reference_.size() - cursor_);
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = i < count ? static_cast<std::int32_t>(reference_[cursor_ + i] * 2147483647.f) : 0;
    if (cursor_ >= static_cast<std::size_t>(rate_ / 2)) {
        reported_sum_ += reported; ++reported_count_;
        reported_min_ = std::min(reported_min_, reported); reported_max_ = std::max(reported_max_, reported);
    }
    for (auto i = std::min(captured_frames_, count); i < count; ++i) input_[cursor_ + i] = 0;
    captured_frames_ = 0;
    cursor_ += count;
    if (cursor_ == reference_.size()) {
        auto expected = State::Capturing;
        state_.compare_exchange_strong(expected, State::Complete, std::memory_order_release);
    }
}
RecordingCalibrationCapture::Result RecordingCalibrationCapture::analyse()
{
    Result r;
    auto expected = State::Complete;
    if (!state_.compare_exchange_strong(expected, State::Analysing, std::memory_order_acq_rel)) {
        r.error = "Calibration capture is not complete."; return r;
    }
    struct FinishAnalysis {
        std::atomic<State>& state;
        ~FinishAnalysis() { state.store(State::Finished, std::memory_order_release); }
    } finishAnalysis{state_};
    const auto finish = [](Result value) {
        return value;
    };
    std::size_t clipped = 0;
    for (float sample : input_) { r.peak = std::max(r.peak, std::abs(static_cast<double>(sample))); if (std::abs(sample) >= .999) ++clipped; }
    if (clipped) { r.error = "The input clipped. Reduce the input gain and retry."; return finish(r); }
    if (!reported_count_ || reported_max_ - reported_min_ > 2) {
        r.error = "Device timing changed during the test. Retry with the same audio settings."; return finish(r);
    }
    std::vector<double> lags;
    const auto length = static_cast<std::size_t>(rate_ * .012);
    const auto maximum = static_cast<std::size_t>(rate_ / 4);
    for (auto click : clicks_) {
        if (cancelled_.load(std::memory_order_acquire)) { r.error = "Calibration cancelled."; return finish(r); }
        double best = 0; std::size_t bestLag = 0;
        const auto score = [&](std::size_t lag, std::size_t stride) {
            double dot = 0, a = 0, b = 0;
            for (std::size_t i = 0; i < length; i += stride) {
                const double x = reference_[click + i], y = input_[click + lag + i];
                dot += x * y; a += x * x; b += y * y;
            }
            return b > 1e-8 ? std::abs(dot) / std::sqrt(a * b) : 0.;
        };
        for (std::size_t lag = 0; lag <= maximum; lag += 4) {
            const double s = score(lag, 4); if (s > best) { best = s; bestLag = lag; }
        }
        const auto first = bestLag > 8 ? bestLag - 8 : 0;
        const auto last = std::min(maximum, bestLag + 8);
        best = 0;
        for (auto lag = first; lag <= last; ++lag) {
            const double s = score(lag, 1); if (s > best) { best = s; bestLag = lag; }
        }
        if (best >= .35 && bestLag > 0 && bestLag < maximum) lags.push_back(static_cast<double>(bestLag));
    }
    r.detected_clicks = static_cast<int>(lags.size());
    if (lags.size() < 5) { r.error = "Too few clear clicks returned. Check the cable or microphone setup and retry."; return finish(r); }
    std::sort(lags.begin(), lags.end()); r.measured_frames = lags[lags.size() / 2];
    r.spread_frames = lags.back() - lags.front();
    if (r.spread_frames > rate_ * .001) { r.error = "The click delays varied by more than 1 ms. Check the setup and retry."; return finish(r); }
    r.reported_frames = static_cast<double>(reported_sum_) / reported_count_;
    r.adjustment_frames = static_cast<std::int64_t>(std::llround(r.measured_frames - r.reported_frames));
    r.ok = !cancelled_.load(std::memory_order_acquire);
    if (!r.ok) r.error = "Calibration cancelled.";
    return finish(r);
}
} // namespace jam2::audio
