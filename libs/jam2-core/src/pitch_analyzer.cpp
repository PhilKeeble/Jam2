#include "pitch_analyzer.hpp"

#include "common.hpp"

extern "C" {
#include "types.h"
#include "fvec.h"
#include "pitch/pitchyinfft.h"
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jam2 {

namespace {

constexpr std::size_t kWindowFrames = 8192;
constexpr std::size_t kHopFrames = 1024;
constexpr std::size_t kWarmupFrames = 4096;
constexpr double kMinimumDb = -55.0;
constexpr double kMinimumConfidence = 0.60;
constexpr double kMinimumFrequencyHz = 27.5;
constexpr double kMaximumFrequencyHz = 4186.01;
constexpr std::uint64_t kStaleAfterUs = 300000;
constexpr double kI32Scale = 1.0 / 2147483648.0;

struct PitchDeleter {
    void operator()(aubio_pitchyinfft_t* value) const noexcept
    {
        if (value != nullptr) {
            del_aubio_pitchyinfft(value);
        }
    }
};

double medianFrequency(std::array<double, 3> values)
{
    std::sort(values.begin(), values.end());
    return values[1];
}

} // namespace

class PitchAnalyzer::Detector final {
public:
    explicit Detector(int sampleRate)
        : sampleRate_(sampleRate)
        , pitch_(new_aubio_pitchyinfft(
              static_cast<uint_t>(sampleRate),
              static_cast<uint_t>(kWindowFrames)))
        , window_(kWindowFrames, 0.0F)
    {
        if (pitch_ == nullptr) {
            throw std::runtime_error("aubio YINFFT initialization failed");
        }
        if (aubio_pitchyinfft_set_tolerance(pitch_.get(), 0.85F) != 0) {
            throw std::runtime_error("aubio YINFFT tolerance setup failed");
        }
    }

    struct Result {
        bool ready = false;
        bool valid = false;
        double frequencyHz = 0.0;
        double confidence = 0.0;
    };

    Result push(std::span<const std::int32_t> input)
    {
        if (input.size() != kHopFrames) {
            return {};
        }
        if (filled_ < kWindowFrames) {
            const std::size_t writable = std::min(input.size(), kWindowFrames - filled_);
            convert(input.first(writable), std::span<smpl_t>(window_).subspan(filled_, writable));
            filled_ += writable;
            if (filled_ < kWarmupFrames) {
                return {};
            }
        } else {
            std::move(window_.begin() + static_cast<std::ptrdiff_t>(kHopFrames),
                window_.end(), window_.begin());
            convert(input, std::span<smpl_t>(window_).last(kHopFrames));
        }

        double squareSum = 0.0;
        for (smpl_t sample : window_) {
            const double value = static_cast<double>(sample);
            squareSum += value * value;
        }
        const double rms = std::sqrt(squareSum / static_cast<double>(window_.size()));
        const double levelDb = 20.0 * std::log10(std::max(rms, 1.0e-12));

        smpl_t period = 0.0F;
        fvec_t inputVector{
            static_cast<uint_t>(window_.size()),
            window_.data(),
        };
        fvec_t outputVector{1U, &period};
        aubio_pitchyinfft_do(pitch_.get(), &inputVector, &outputVector);
        const double confidence =
            static_cast<double>(aubio_pitchyinfft_get_confidence(pitch_.get()));
        double refinedPeriod = static_cast<double>(period);
        if (filled_ == kWindowFrames &&
            refinedPeriod > static_cast<double>(sampleRate_) / 100.0) {
            refinedPeriod = refineLowPeriod(refinedPeriod);
        }
        const double frequency =
            refinedPeriod > std::numeric_limits<smpl_t>::epsilon()
            ? static_cast<double>(sampleRate_) / refinedPeriod
            : 0.0;
        return {
            true,
            levelDb >= kMinimumDb &&
                confidence >= kMinimumConfidence &&
                frequency >= kMinimumFrequencyHz &&
                frequency <= kMaximumFrequencyHz,
            frequency,
            confidence,
        };
    }

private:
    double differenceAt(std::size_t lag) const noexcept
    {
        double sum = 0.0;
        const std::size_t compared = window_.size() - lag;
        for (std::size_t index = 0; index < compared; ++index) {
            const double delta =
                static_cast<double>(window_[index]) -
                static_cast<double>(window_[index + lag]);
            sum += delta * delta;
        }
        return sum / static_cast<double>(compared);
    }

    double refineLowPeriod(double coarsePeriod) const noexcept
    {
        const std::size_t centre = static_cast<std::size_t>(
            std::clamp(std::lround(coarsePeriod), 2L,
                static_cast<long>(window_.size() / 2U - 2U)));
        const std::size_t radius = std::max<std::size_t>(
            8U, static_cast<std::size_t>(std::ceil(coarsePeriod * 0.05)));
        const std::size_t first = centre > radius ? centre - radius : 2U;
        const std::size_t last = std::min(
            centre + radius, window_.size() / 2U - 2U);

        std::size_t best = first;
        double bestDifference = differenceAt(first);
        for (std::size_t lag = first + 1U; lag <= last; ++lag) {
            const double difference = differenceAt(lag);
            if (difference < bestDifference) {
                best = lag;
                bestDifference = difference;
            }
        }
        if (best <= first || best >= last) {
            return static_cast<double>(best);
        }

        const double left = differenceAt(best - 1U);
        const double right = differenceAt(best + 1U);
        const double denominator = left - 2.0 * bestDifference + right;
        if (std::abs(denominator) <= std::numeric_limits<double>::epsilon()) {
            return static_cast<double>(best);
        }
        const double offset = std::clamp(
            0.5 * (left - right) / denominator, -0.5, 0.5);
        return static_cast<double>(best) + offset;
    }

    static void convert(
        std::span<const std::int32_t> input,
        std::span<smpl_t> output) noexcept
    {
        for (std::size_t index = 0; index < input.size(); ++index) {
            output[index] = static_cast<smpl_t>(
                static_cast<double>(input[index]) * kI32Scale);
        }
    }

    int sampleRate_ = 48000;
    std::unique_ptr<aubio_pitchyinfft_t, PitchDeleter> pitch_;
    std::vector<smpl_t> window_;
    std::size_t filled_ = 0;
};

PitchAnalyzer::PitchAnalyzer(
    audio::MonoRingBuffer& input,
    int sampleRate,
    const std::atomic<std::uint64_t>& engineFrame)
    : input_(input)
    , sampleRate_(sampleRate)
    , engineFrame_(engineFrame)
{
}

PitchAnalyzer::~PitchAnalyzer()
{
    stop();
}

bool PitchAnalyzer::start(std::string& error) noexcept
{
    if (worker_.joinable()) {
        return true;
    }
    try {
        (void)input_.discard_all();
        detector_ = std::make_unique<Detector>(sampleRate_);
        stopRequested_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            snapshot_.enabled = true;
            snapshot_.valid = false;
            snapshot_.frequency_hz = 0.0;
            snapshot_.cents = 0.0;
            snapshot_.confidence = 0.0;
            snapshot_.midi_note = -1;
            snapshot_.last_valid_engine_frame = 0;
        }
        worker_ = std::thread([this] { run(); });
        return true;
    } catch (const std::exception& exception) {
        detector_.reset();
        error = exception.what();
    } catch (...) {
        detector_.reset();
        error = "unknown pitch analyzer initialization failure";
    }
    return false;
}

void PitchAnalyzer::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    detector_.reset();
    (void)input_.discard_all();
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshot_.enabled = false;
    snapshot_.valid = false;
    snapshot_.frequency_hz = 0.0;
    snapshot_.cents = 0.0;
    snapshot_.confidence = 0.0;
    snapshot_.midi_note = -1;
    snapshot_.last_valid_engine_frame = 0;
}

EnginePitchSnapshot PitchAnalyzer::snapshot() const noexcept
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

void PitchAnalyzer::publishInvalidIfStale(std::uint64_t nowUs) noexcept
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    if (snapshot_.valid &&
        snapshot_.last_valid_monotonic_us > 0 &&
        nowUs - snapshot_.last_valid_monotonic_us >= kStaleAfterUs) {
        snapshot_.valid = false;
        snapshot_.frequency_hz = 0.0;
        snapshot_.cents = 0.0;
        snapshot_.confidence = 0.0;
        snapshot_.midi_note = -1;
        snapshot_.last_valid_engine_frame = 0;
    }
}

void PitchAnalyzer::run() noexcept
{
    std::array<std::int32_t, kHopFrames> hop{};
    std::array<double, 3> validFrequencies{};
    std::size_t validFrequencyCount = 0;
    std::size_t validFrequencyWrite = 0;
    try {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (input_.available_read() < hop.size()) {
                publishInvalidIfStale(monotonic_us());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            (void)input_.pop(hop);
            {
                std::lock_guard<std::mutex> lock(snapshotMutex_);
                ++snapshot_.input_hops;
            }
            const std::uint64_t started = monotonic_us();
            const Detector::Result detected = detector_->push(hop);
            if (!detected.ready) {
                continue;
            }
            const std::uint64_t elapsed = monotonic_us() - started;
            const std::uint64_t now = monotonic_us();
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            ++snapshot_.analyzed_windows;
            snapshot_.processing_time_sum_us += elapsed;
            snapshot_.processing_time_max_us =
                std::max(snapshot_.processing_time_max_us, elapsed);
            if (!detected.valid) {
                ++snapshot_.rejected_windows;
                if (snapshot_.valid &&
                    snapshot_.last_valid_monotonic_us > 0 &&
                    now - snapshot_.last_valid_monotonic_us >= kStaleAfterUs) {
                    snapshot_.valid = false;
                    snapshot_.frequency_hz = 0.0;
                    snapshot_.cents = 0.0;
                    snapshot_.confidence = 0.0;
                    snapshot_.midi_note = -1;
                    snapshot_.last_valid_engine_frame = 0;
                }
                continue;
            }

            validFrequencies[validFrequencyWrite] = detected.frequencyHz;
            validFrequencyWrite = (validFrequencyWrite + 1U) % validFrequencies.size();
            validFrequencyCount = std::min(validFrequencyCount + 1U, validFrequencies.size());
            double frequency = detected.frequencyHz;
            if (validFrequencyCount == validFrequencies.size()) {
                frequency = medianFrequency(validFrequencies);
            }
            const double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
            const int nearestMidi = static_cast<int>(std::lround(midi));
            snapshot_.enabled = true;
            snapshot_.valid = true;
            snapshot_.frequency_hz = frequency;
            snapshot_.cents = 100.0 * (midi - static_cast<double>(nearestMidi));
            snapshot_.confidence = detected.confidence;
            snapshot_.midi_note = nearestMidi;
            snapshot_.last_valid_engine_frame =
                engineFrame_.load(std::memory_order_acquire);
            snapshot_.last_valid_monotonic_us = now;
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_.enabled = false;
        snapshot_.valid = false;
        snapshot_.frequency_hz = 0.0;
        snapshot_.cents = 0.0;
        snapshot_.confidence = 0.0;
        snapshot_.midi_note = -1;
    }
}

} // namespace jam2
