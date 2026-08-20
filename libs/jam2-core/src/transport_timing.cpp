#include "transport_timing.hpp"

#include "runtime_limits.hpp"

#include <cmath>
#include <limits>

namespace jam2 {
namespace {

constexpr std::uint64_t kMaximumFrame =
    (std::numeric_limits<std::uint64_t>::max)();

std::uint64_t signed_magnitude(std::int64_t value) noexcept
{
    return value >= 0
        ? static_cast<std::uint64_t>(value)
        : static_cast<std::uint64_t>(-(value + 1)) + 1ULL;
}

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (left > kMaximumFrame - right) {
        return false;
    }
    result = left + right;
    return true;
}

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (left != 0 && right > kMaximumFrame / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool checked_musical_from_raw(
    std::uint64_t rawFrame,
    std::int64_t renderOffsetFrames,
    std::uint64_t& result) noexcept
{
    const std::uint64_t magnitude = signed_magnitude(renderOffsetFrames);
    if (renderOffsetFrames < 0) {
        result = rawFrame > magnitude ? rawFrame - magnitude : 0ULL;
        return true;
    }
    return checked_add(rawFrame, magnitude, result);
}

bool checked_raw_from_musical(
    std::uint64_t musicalFrame,
    std::int64_t renderOffsetFrames,
    std::uint64_t& result) noexcept
{
    const std::uint64_t magnitude = signed_magnitude(renderOffsetFrames);
    if (renderOffsetFrames >= 0) {
        result = musicalFrame > magnitude ? musicalFrame - magnitude : 0ULL;
        return true;
    }
    return checked_add(musicalFrame, magnitude, result);
}

} // namespace

std::uint64_t transport_musical_frame_from_raw(
    std::uint64_t rawFrame,
    std::int64_t renderOffsetFrames) noexcept
{
    std::uint64_t result = 0;
    return checked_musical_from_raw(rawFrame, renderOffsetFrames, result)
        ? result
        : kMaximumFrame;
}

std::uint64_t transport_raw_frame_from_musical(
    std::uint64_t musicalFrame,
    std::int64_t renderOffsetFrames) noexcept
{
    std::uint64_t result = 0;
    return checked_raw_from_musical(musicalFrame, renderOffsetFrames, result)
        ? result
        : kMaximumFrame;
}

std::optional<QuantizedTransportSchedule> next_bar_transport_schedule(
    double sampleRate,
    metronome::PatternSnapshot pattern,
    std::uint64_t rawCurrentFrame,
    std::int64_t renderOffsetFrames,
    bool epochValid,
    std::uint64_t epochMusicalFrame,
    int countInBars) noexcept
{
    if (!std::isfinite(sampleRate) ||
        sampleRate < static_cast<double>(limits::kMinimumSampleRate) ||
        sampleRate > static_cast<double>(limits::kMaximumSampleRate) ||
        countInBars < 0) {
        return std::nullopt;
    }
    const int integerSampleRate = static_cast<int>(std::llround(sampleRate));
    if (!limits::valid_sample_rate(integerSampleRate)) {
        return std::nullopt;
    }

    pattern = metronome::sanitize(pattern);
    const std::uint64_t stepFrames = metronome::step_interval_samples(
        static_cast<double>(integerSampleRate),
        pattern.bpm,
        pattern.division,
        pattern.tempo_pulse_units);
    std::uint64_t stepsPerBar = 0;
    std::uint64_t barFrames = 0;
    if (stepFrames == 0 ||
        !checked_multiply(
            static_cast<std::uint64_t>(pattern.division),
            static_cast<std::uint64_t>(pattern.beats_per_bar),
            stepsPerBar) ||
        !checked_multiply(stepFrames, stepsPerBar, barFrames) ||
        barFrames == 0) {
        return std::nullopt;
    }

    std::uint64_t musicalNow = 0;
    if (!checked_musical_from_raw(
            rawCurrentFrame, renderOffsetFrames, musicalNow)) {
        return std::nullopt;
    }
    const std::uint64_t epoch = epochValid ? epochMusicalFrame : 0ULL;
    const std::uint64_t elapsed = musicalNow >= epoch
        ? musicalNow - epoch
        : 0ULL;
    const std::uint64_t completedBars = elapsed / barFrames;
    if (completedBars == kMaximumFrame) {
        return std::nullopt;
    }
    std::uint64_t nextBarOffset = 0;
    std::uint64_t nextBarMusical = 0;
    if (!checked_multiply(completedBars + 1ULL, barFrames, nextBarOffset) ||
        !checked_add(epoch, nextBarOffset, nextBarMusical)) {
        return std::nullopt;
    }

    const std::uint64_t minimumLeadFrames =
        static_cast<std::uint64_t>(integerSampleRate) / 5ULL;
    std::uint64_t minimumRawFrame = 0;
    std::uint64_t countdownRawFrame = 0;
    if (!checked_add(rawCurrentFrame, minimumLeadFrames, minimumRawFrame) ||
        !checked_raw_from_musical(
            nextBarMusical, renderOffsetFrames, countdownRawFrame)) {
        return std::nullopt;
    }
    if (countdownRawFrame <= minimumRawFrame) {
        if (!checked_add(nextBarMusical, barFrames, nextBarMusical) ||
            !checked_raw_from_musical(
                nextBarMusical, renderOffsetFrames, countdownRawFrame)) {
            return std::nullopt;
        }
    }

    std::uint64_t countInFrames = 0;
    std::uint64_t targetMusicalFrame = 0;
    std::uint64_t targetRawFrame = 0;
    if (!checked_multiply(
            static_cast<std::uint64_t>(countInBars),
            barFrames,
            countInFrames) ||
        !checked_add(nextBarMusical, countInFrames, targetMusicalFrame) ||
        !checked_raw_from_musical(
            targetMusicalFrame, renderOffsetFrames, targetRawFrame) ||
        countdownRawFrame <= rawCurrentFrame ||
        targetRawFrame < countdownRawFrame) {
        return std::nullopt;
    }

    return QuantizedTransportSchedule{
        countdownRawFrame,
        targetRawFrame,
        targetMusicalFrame,
    };
}

std::optional<QuantizedTransportSchedule> align_received_transport_to_grid(
    double sampleRate,
    metronome::PatternSnapshot pattern,
    std::uint64_t rawCurrentFrame,
    std::int64_t renderOffsetFrames,
    bool epochValid,
    std::uint64_t epochMusicalFrame,
    std::uint64_t estimatedTargetRawFrame,
    std::uint64_t countInFrames) noexcept
{
    if (!epochValid || !std::isfinite(sampleRate) ||
        sampleRate < static_cast<double>(limits::kMinimumSampleRate) ||
        sampleRate > static_cast<double>(limits::kMaximumSampleRate)) {
        return std::nullopt;
    }
    const int integerSampleRate = static_cast<int>(std::llround(sampleRate));
    if (!limits::valid_sample_rate(integerSampleRate)) {
        return std::nullopt;
    }

    pattern = metronome::sanitize(pattern);
    const std::uint64_t stepFrames = metronome::step_interval_samples(
        static_cast<double>(integerSampleRate),
        pattern.bpm,
        pattern.division,
        pattern.tempo_pulse_units);
    std::uint64_t stepsPerBar = 0;
    std::uint64_t barFrames = 0;
    if (stepFrames == 0 ||
        !checked_multiply(
            static_cast<std::uint64_t>(pattern.division),
            static_cast<std::uint64_t>(pattern.beats_per_bar),
            stepsPerBar) ||
        !checked_multiply(stepFrames, stepsPerBar, barFrames) ||
        barFrames == 0) {
        return std::nullopt;
    }

    std::uint64_t estimatedMusical = 0;
    if (!checked_musical_from_raw(
            estimatedTargetRawFrame,
            renderOffsetFrames,
            estimatedMusical) ||
        estimatedMusical < epochMusicalFrame) {
        return std::nullopt;
    }
    const std::uint64_t elapsed = estimatedMusical - epochMusicalFrame;
    const std::uint64_t completedBars = elapsed / barFrames;
    std::uint64_t lowerOffset = 0;
    std::uint64_t lowerBoundary = 0;
    if (!checked_multiply(completedBars, barFrames, lowerOffset) ||
        !checked_add(epochMusicalFrame, lowerOffset, lowerBoundary)) {
        return std::nullopt;
    }
    std::uint64_t targetMusicalFrame = lowerBoundary;
    const std::uint64_t distanceBelow = estimatedMusical - lowerBoundary;
    if (distanceBelow > barFrames / 2ULL) {
        if (!checked_add(lowerBoundary, barFrames, targetMusicalFrame)) {
            return std::nullopt;
        }
    }

    std::uint64_t targetRawFrame = 0;
    if (!checked_raw_from_musical(
            targetMusicalFrame,
            renderOffsetFrames,
            targetRawFrame) ||
        targetRawFrame < countInFrames) {
        return std::nullopt;
    }
    const std::uint64_t countdownRawFrame = targetRawFrame - countInFrames;
    if (countdownRawFrame <= rawCurrentFrame ||
        targetRawFrame < countdownRawFrame) {
        return std::nullopt;
    }
    return QuantizedTransportSchedule{
        countdownRawFrame,
        targetRawFrame,
        targetMusicalFrame,
    };
}

} // namespace jam2
