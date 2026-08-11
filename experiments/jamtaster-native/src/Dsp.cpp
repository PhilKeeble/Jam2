#include "Dsp.hpp"

#include "fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace jamtaster::native {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

double hzToSlaneyMel(double hz)
{
    constexpr double linearSpacing = 200.0 / 3.0;
    constexpr double minLogHz = 1000.0;
    constexpr double minLogMel = minLogHz / linearSpacing;
    constexpr double logStep = 0.06875177742094912; // log(6.4) / 27
    if (hz < minLogHz) return hz / linearSpacing;
    return minLogMel + std::log(hz / minLogHz) / logStep;
}

double slaneyMelToHz(double mel)
{
    constexpr double linearSpacing = 200.0 / 3.0;
    constexpr double minLogHz = 1000.0;
    constexpr double minLogMel = minLogHz / linearSpacing;
    constexpr double logStep = 0.06875177742094912;
    if (mel < minLogMel) return mel * linearSpacing;
    return minLogHz * std::exp(logStep * (mel - minLogMel));
}

std::size_t reflectedIndex(std::int64_t index, std::size_t size)
{
    if (size < 2) return 0;
    const auto limit = static_cast<std::int64_t>(size);
    while (index < 0 || index >= limit) {
        index = index < 0 ? -index : (2 * limit - 2 - index);
    }
    return static_cast<std::size_t>(index);
}

} // namespace

float& Matrix::at(std::size_t row, std::size_t column)
{
    return values.at(row * columns + column);
}

float Matrix::at(std::size_t row, std::size_t column) const
{
    return values.at(row * columns + column);
}

std::vector<float> resampleSinc(
    const std::vector<float>& input,
    int sourceSampleRate,
    int targetSampleRate)
{
    if (sourceSampleRate <= 0 || targetSampleRate <= 0) {
        throw std::invalid_argument("resampling rates must be positive");
    }
    if (input.empty() || sourceSampleRate == targetSampleRate) return input;
    const long double exactFrames = static_cast<long double>(input.size()) *
        targetSampleRate / sourceSampleRate;
    if (!std::isfinite(exactFrames) || exactFrames >
        static_cast<long double>((std::numeric_limits<std::size_t>::max)())) {
        throw std::length_error("resampled audio would be too large");
    }
    const auto outputFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(exactFrames)));
    std::vector<float> output(outputFrames);
    constexpr int halfTaps = 24;
    const double sourcePerTarget = static_cast<double>(sourceSampleRate) / targetSampleRate;
    const double cutoff = 0.96 * std::min(1.0,
        static_cast<double>(targetSampleRate) / sourceSampleRate);
    for (std::size_t out = 0; out < outputFrames; ++out) {
        const double position = static_cast<double>(out) * sourcePerTarget;
        const auto centre = static_cast<std::int64_t>(std::floor(position));
        const double fraction = position - static_cast<double>(centre);
        double sum = 0.0;
        double weightSum = 0.0;
        for (int tap = -halfTaps + 1; tap <= halfTaps; ++tap) {
            const auto source = centre + tap;
            if (source < 0 || source >= static_cast<std::int64_t>(input.size())) continue;
            const double distance = fraction - tap;
            const double normalized = std::abs(distance) / halfTaps;
            if (normalized >= 1.0) continue;
            const double argument = cutoff * distance;
            const double sinc = std::abs(argument) < 1.0e-12
                ? 1.0 : std::sin(kPi * argument) / (kPi * argument);
            const double window = 0.5 * (1.0 + std::cos(kPi * normalized));
            const double weight = cutoff * sinc * window;
            sum += input[static_cast<std::size_t>(source)] * weight;
            weightSum += weight;
        }
        output[out] = static_cast<float>(std::abs(weightSum) > 1.0e-12 ? sum / weightSum : 0.0);
    }
    return output;
}

AudioBuffer resampleSinc(const AudioBuffer& input, int targetSampleRate)
{
    if (input.channels <= 0 || input.samples.size() % static_cast<std::size_t>(input.channels) != 0) {
        throw std::invalid_argument("invalid interleaved audio passed to resampler");
    }
    if (input.sampleRate == targetSampleRate) return input;
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(input.channels));
    for (auto& channel : channels) channel.resize(input.frames());
    for (std::size_t frame = 0; frame < input.frames(); ++frame) {
        for (int channel = 0; channel < input.channels; ++channel) {
            channels[static_cast<std::size_t>(channel)][frame] =
                input.samples[frame * static_cast<std::size_t>(input.channels) +
                    static_cast<std::size_t>(channel)];
        }
    }
    for (auto& channel : channels) {
        channel = resampleSinc(channel, input.sampleRate, targetSampleRate);
    }
    AudioBuffer result;
    result.sampleRate = targetSampleRate;
    result.channels = input.channels;
    result.samples.resize(channels.front().size() * static_cast<std::size_t>(result.channels));
    for (std::size_t frame = 0; frame < channels.front().size(); ++frame) {
        for (int channel = 0; channel < result.channels; ++channel) {
            result.samples[frame * static_cast<std::size_t>(result.channels) +
                static_cast<std::size_t>(channel)] = channels[static_cast<std::size_t>(channel)][frame];
        }
    }
    return result;
}

Matrix beatThisLogMel(const std::vector<float>& audio)
{
    constexpr int sampleRate = 22050;
    constexpr std::size_t fftSize = 1024;
    constexpr std::size_t hop = 441;
    constexpr std::size_t melBands = 128;
    constexpr std::size_t bins = fftSize / 2 + 1;
    constexpr std::size_t pad = fftSize / 2;
    if (audio.size() < 2) throw std::runtime_error("audio is too short for Beat This preprocessing");

    const std::size_t paddedSize = audio.size() + 2 * pad;
    const std::size_t frames = 1 + (paddedSize - fftSize) / hop;
    Matrix result{frames, melBands, std::vector<float>(frames * melBands, 0.0F)};

    std::vector<float> window(fftSize);
    for (std::size_t index = 0; index < fftSize; ++index) {
        window[index] = static_cast<float>(0.5 *
            (1.0 - std::cos(2.0 * kPi * index / fftSize)));
    }

    const double melMin = hzToSlaneyMel(30.0);
    const double melMax = hzToSlaneyMel(11000.0);
    std::vector<double> edges(melBands + 2);
    for (std::size_t index = 0; index < edges.size(); ++index) {
        edges[index] = slaneyMelToHz(melMin +
            (melMax - melMin) * index / (edges.size() - 1));
    }
    std::vector<std::vector<std::pair<std::size_t, float>>> filters(melBands);
    for (std::size_t mel = 0; mel < melBands; ++mel) {
        for (std::size_t bin = 0; bin < bins; ++bin) {
            const double hz = static_cast<double>(bin) * sampleRate / fftSize;
            const double left = (hz - edges[mel]) / (edges[mel + 1] - edges[mel]);
            const double right = (edges[mel + 2] - hz) / (edges[mel + 2] - edges[mel + 1]);
            const float weight = static_cast<float>(std::max(0.0, std::min(left, right)));
            if (weight > 0.0F) filters[mel].emplace_back(bin, weight);
        }
    }

    signalsmith::linear::SimpleFFT<float> fft(fftSize);
    std::vector<std::complex<float>> time(fftSize);
    std::vector<std::complex<float>> frequency(fftSize);
    std::vector<float> magnitudes(bins);
    const float normalization = std::sqrt(static_cast<float>(fftSize));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int64_t start = static_cast<std::int64_t>(frame * hop) -
            static_cast<std::int64_t>(pad);
        for (std::size_t index = 0; index < fftSize; ++index) {
            const auto source = reflectedIndex(start + static_cast<std::int64_t>(index), audio.size());
            time[index] = {audio[source] * window[index], 0.0F};
        }
        fft.fft(time.data(), frequency.data());
        for (std::size_t bin = 0; bin < bins; ++bin) {
            magnitudes[bin] = std::abs(frequency[bin]) / normalization;
        }
        for (std::size_t mel = 0; mel < melBands; ++mel) {
            double energy = 0.0;
            for (const auto& [bin, weight] : filters[mel]) {
                energy += magnitudes[bin] * weight;
            }
            result.at(frame, mel) = static_cast<float>(std::log1p(1000.0 * energy));
        }
    }
    return result;
}

Matrix adtofLogFilterbank(const std::vector<float>& audio)
{
    constexpr int sampleRate = 44100;
    constexpr std::size_t fftSize = 2048;
    constexpr std::size_t hop = 441;
    constexpr std::size_t fftBins = fftSize / 2; // ADTOF deliberately omits Nyquist.
    if (audio.empty()) throw std::runtime_error("audio is empty");

    std::vector<int> frequencyBins;
    const double step = std::pow(2.0, 1.0 / 12.0);
    for (double frequency = 20.0; frequency <= 20000.0 * (1.0 + 1.0e-12);
         frequency *= step) {
        const auto nearest = static_cast<int>(std::llround(frequency * fftSize / sampleRate));
        const int clamped = std::clamp(nearest, 0, static_cast<int>(fftBins) - 1);
        if (frequencyBins.empty() || clamped > frequencyBins.back()) {
            frequencyBins.push_back(clamped);
        }
    }
    if (frequencyBins.size() < 3) throw std::runtime_error("ADTOF filterbank is empty");
    const std::size_t filters = frequencyBins.size() - 2;
    if (filters != 84) throw std::runtime_error("ADTOF filterbank contract changed");

    std::vector<std::vector<std::pair<std::size_t, float>>> filterbank(filters);
    for (std::size_t filter = 0; filter < filters; ++filter) {
        const int left = frequencyBins[filter];
        const int centre = frequencyBins[filter + 1];
        const int right = frequencyBins[filter + 2];
        auto& weights = filterbank[filter];
        if (right - left < 2) {
            weights.emplace_back(static_cast<std::size_t>(left), 1.0F);
        } else {
            for (int bin = left; bin < centre; ++bin) {
                weights.emplace_back(static_cast<std::size_t>(bin),
                    static_cast<float>(bin - left) / static_cast<float>(centre - left));
            }
            weights.emplace_back(static_cast<std::size_t>(centre), 1.0F);
            for (int bin = centre + 1; bin < right && bin < static_cast<int>(fftBins); ++bin) {
                weights.emplace_back(static_cast<std::size_t>(bin),
                    static_cast<float>(right - bin) / static_cast<float>(right - centre));
            }
        }
        double sum = 0.0;
        for (const auto& entry : weights) sum += entry.second;
        if (sum > 0.0) {
            for (auto& entry : weights) entry.second = static_cast<float>(entry.second / sum);
        }
    }

    // librosa.stft(center=True, pad_mode="constant") yields one frame at time zero
    // plus one frame for every complete hop in the original signal.
    const std::size_t frames = 1 + audio.size() / hop;
    Matrix result{frames, filters, std::vector<float>(frames * filters, 0.0F)};
    std::vector<float> window(fftSize);
    for (std::size_t index = 0; index < fftSize; ++index) {
        // np.hanning is the symmetric Hann window, unlike librosa's default periodic one.
        window[index] = static_cast<float>(0.5 *
            (1.0 - std::cos(2.0 * kPi * index / (fftSize - 1))));
    }
    signalsmith::linear::SimpleFFT<float> fft(fftSize);
    std::vector<std::complex<float>> time(fftSize);
    std::vector<std::complex<float>> frequency(fftSize);
    std::vector<float> magnitudes(fftBins);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int64_t start = static_cast<std::int64_t>(frame * hop) -
            static_cast<std::int64_t>(fftSize / 2);
        for (std::size_t index = 0; index < fftSize; ++index) {
            const auto source = start + static_cast<std::int64_t>(index);
            const float sample = source >= 0 && source < static_cast<std::int64_t>(audio.size())
                ? audio[static_cast<std::size_t>(source)] : 0.0F;
            time[index] = {sample * window[index], 0.0F};
        }
        fft.fft(time.data(), frequency.data());
        for (std::size_t bin = 0; bin < fftBins; ++bin) {
            magnitudes[bin] = std::abs(frequency[bin]);
        }
        for (std::size_t filter = 0; filter < filters; ++filter) {
            double value = 0.0;
            for (const auto& [bin, weight] : filterbank[filter]) {
                value += magnitudes[bin] * weight;
            }
            result.at(frame, filter) = static_cast<float>(std::log10(1.0 + value));
        }
    }
    return result;
}

Matrix chordMiniLogCqt(const std::vector<float>& audio)
{
    constexpr int sampleRate = 22050;
    constexpr std::size_t hop = 2048;
    constexpr std::size_t fftSize = 32768;
    constexpr std::size_t bins = 144;
    constexpr double fmin = 32.70319566257483; // C1
    const double relativeBandwidth =
        (std::pow(2.0, 2.0 / 24.0) - 1.0) /
        (std::pow(2.0, 2.0 / 24.0) + 1.0);
    const double quality = 1.0 / relativeBandwidth;
    if (audio.empty()) throw std::runtime_error("audio is empty");

    // ChordMini was trained on librosa CQT magnitudes. A fixed, sufficiently
    // long spectral window preserves the same 24-bin-per-octave frequency grid
    // without carrying librosa's recursive Python implementation into JamTaster.
    const std::size_t frames = 1 + audio.size() / hop;
    Matrix result{frames, bins, std::vector<float>(frames * bins, 0.0F)};
    std::vector<float> window(fftSize);
    double windowSum = 0.0;
    for (std::size_t index = 0; index < fftSize; ++index) {
        window[index] = static_cast<float>(0.5 *
            (1.0 - std::cos(2.0 * kPi * index / fftSize)));
        windowSum += window[index];
    }
    signalsmith::linear::SimpleFFT<float> fft(fftSize);
    std::vector<std::complex<float>> time(fftSize);
    std::vector<std::complex<float>> frequency(fftSize);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int64_t start = static_cast<std::int64_t>(frame * hop) -
            static_cast<std::int64_t>(fftSize / 2);
        for (std::size_t index = 0; index < fftSize; ++index) {
            const auto source = start + static_cast<std::int64_t>(index);
            const float sample = source >= 0 && source < static_cast<std::int64_t>(audio.size())
                ? audio[static_cast<std::size_t>(source)] : 0.0F;
            time[index] = {sample * window[index], 0.0F};
        }
        fft.fft(time.data(), frequency.data());
        for (std::size_t bin = 0; bin < bins; ++bin) {
            const double target = fmin * std::pow(2.0, static_cast<double>(bin) / 24.0);
            const double position = target * fftSize / sampleRate;
            const auto lower = static_cast<std::size_t>(std::floor(position));
            const auto upper = std::min(lower + 1, fftSize / 2);
            const float fraction = static_cast<float>(position - lower);
            // librosa's L1-normalized wavelets are multiplied by their length
            // at the FFT projection boundary and scale=True subsequently
            // divides by sqrt(length). The net magnitude scale is sqrt(length).
            const double waveletLength = quality * sampleRate / target;
            const float magnitude = std::lerp(
                std::abs(frequency[lower]), std::abs(frequency[upper]), fraction) *
                static_cast<float>(2.0 * std::sqrt(waveletLength) / windowSum);
            result.at(frame, bin) = std::log(magnitude + 1.0e-6F);
        }
    }
    return result;
}

} // namespace jamtaster::native
