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
    int countInBars,
    PlaybackTempo tempo,
    double epochSourcePosition) noexcept
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
    const double sourceEpoch = epochValid && epochSourcePosition >= 0.0 ? epochSourcePosition : static_cast<double>(epoch);
    if (tempo.rate_ppm != 1000000 || tempo.anchor_frame != 0 || tempo.anchor_position != 0.0) {
        const double elapsedSource = std::max(0.0, tempo.position(musicalNow) - sourceEpoch);
        double next = sourceEpoch + (std::floor(elapsedSource / barFrames) + 1.0) * barFrames;
        std::uint64_t minimum = 0;
        if (!checked_add(rawCurrentFrame, static_cast<std::uint64_t>(integerSampleRate) / 5ULL, minimum)) return std::nullopt;
        auto countdown = transport_raw_frame_from_musical(tempo.frame(next), renderOffsetFrames);
        if (countdown <= minimum) {
            const auto minimumMusical = transport_musical_frame_from_raw(minimum, renderOffsetFrames);
            next = sourceEpoch + (std::floor(std::max(0.0,
                tempo.position(minimumMusical) - sourceEpoch) / barFrames) + 1.0) * barFrames;
            countdown = transport_raw_frame_from_musical(tempo.frame(next), renderOffsetFrames);
        }
        const auto targetMusical = tempo.frame(next + static_cast<double>(countInBars) * barFrames);
        const auto target = transport_raw_frame_from_musical(targetMusical, renderOffsetFrames);
        if (targetMusical == kMaximumFrame || target <= rawCurrentFrame || target < countdown) return std::nullopt;
        return QuantizedTransportSchedule{countdown, target, targetMusical};
    }
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
    std::uint64_t countInFrames,
    PlaybackTempo tempo,
    double epochSourcePosition,
    TransportBoundary boundaryKind) noexcept
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

    if (boundaryKind == TransportBoundary::Beat) {
        const auto beatFrames = stepFrames * static_cast<std::uint64_t>(pattern.division);
        const double sourceEpoch = epochSourcePosition >= 0.0 ? epochSourcePosition : static_cast<double>(epochMusicalFrame);
        const auto estimate = transport_musical_frame_from_raw(estimatedTargetRawFrame, renderOffsetFrames);
        const double elapsed = tempo.position(estimate) - sourceEpoch;
        if (elapsed < -static_cast<double>(beatFrames) / 2.0) return std::nullopt;
        const double beat = std::floor(std::max(0.0, elapsed) / beatFrames + 0.5);
        const auto musical = tempo.frame(sourceEpoch + beat * beatFrames);
        const auto raw = transport_raw_frame_from_musical(musical, renderOffsetFrames);
        if (musical == kMaximumFrame || raw < countInFrames || raw - countInFrames <= rawCurrentFrame)
            return std::nullopt;
        return QuantizedTransportSchedule{raw - countInFrames, raw, musical};
    }
    const double sourceEpoch = epochSourcePosition >= 0.0 ? epochSourcePosition : static_cast<double>(epochMusicalFrame);
    std::uint64_t estimatedMusical = 0;
    if (!checked_musical_from_raw(
            estimatedTargetRawFrame,
            renderOffsetFrames,
            estimatedMusical) ||
        estimatedMusical < epochMusicalFrame) {
        return std::nullopt;
    }
    if (tempo.rate_ppm != 1000000 || tempo.anchor_frame != 0 || tempo.anchor_position != 0.0) {
        const double estimatedSource = std::max(0.0, tempo.position(estimatedMusical) - sourceEpoch);
        double boundary = sourceEpoch + std::floor(estimatedSource / barFrames + 0.5) * barFrames;
        auto musical = tempo.frame(boundary);
        auto raw = transport_raw_frame_from_musical(musical, renderOffsetFrames);
        if (raw < countInFrames || raw - countInFrames <= rawCurrentFrame) {
            if (rawCurrentFrame > kMaximumFrame - countInFrames) return std::nullopt;
            const auto safeMusical = transport_musical_frame_from_raw(rawCurrentFrame + countInFrames, renderOffsetFrames);
            boundary = sourceEpoch + (std::floor(std::max(0.0,
                tempo.position(safeMusical) - sourceEpoch) / barFrames) + 1.0) * barFrames;
            musical = tempo.frame(boundary);
            raw = transport_raw_frame_from_musical(musical, renderOffsetFrames);
        }
        if (musical == kMaximumFrame || raw < countInFrames || raw - countInFrames <= rawCurrentFrame) return std::nullopt;
        return QuantizedTransportSchedule{raw - countInFrames, raw, musical};
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
    std::uint64_t countdownRawFrame = targetRawFrame - countInFrames;
    if (countdownRawFrame <= rawCurrentFrame) {
        const std::uint64_t missedBy = rawCurrentFrame - countdownRawFrame;
        const std::uint64_t barsToAdvance = missedBy / barFrames + 1ULL;
        std::uint64_t advanceFrames = 0;
        if (!checked_multiply(barsToAdvance, barFrames, advanceFrames) ||
            !checked_add(targetMusicalFrame, advanceFrames, targetMusicalFrame) ||
            !checked_add(targetRawFrame, advanceFrames, targetRawFrame) ||
            !checked_add(countdownRawFrame, advanceFrames, countdownRawFrame)) {
            return std::nullopt;
        }
    }
    if (targetRawFrame < countdownRawFrame) {
        return std::nullopt;
    }
    return QuantizedTransportSchedule{
        countdownRawFrame,
        targetRawFrame,
        targetMusicalFrame,
    };
}

std::optional<RecoveredTransportSchedule> recover_transport_position(
    std::uint64_t now, std::int64_t renderOffset,
    std::uint64_t originalTarget, std::uint64_t countInFrames,
    std::uint64_t minimumLeadFrames) noexcept
{
    if (now > kMaximumFrame - minimumLeadFrames || originalTarget == kMaximumFrame) return std::nullopt;
    const auto target = std::max(originalTarget, now + minimumLeadFrames);
    const auto countdown = originalTarget >= countInFrames && originalTarget - countInFrames > now
        ? originalTarget - countInFrames : target;
    return RecoveredTransportSchedule{
        {countdown, target, transport_musical_frame_from_raw(target, renderOffset)}, target - originalTarget};
}

std::optional<RecoveredTransportSchedule> recover_received_restart(
    double sampleRate, metronome::PatternSnapshot pattern,
    std::uint64_t now, std::int64_t renderOffset, bool epochValid,
    std::uint64_t epoch, std::uint64_t estimatedOriginalTarget,
    std::uint64_t countInFrames, std::uint64_t minimumLeadFrames,
    PlaybackTempo tempo, double epochSourcePosition) noexcept
{
    if (!epochValid || !std::isfinite(sampleRate) ||
        sampleRate < limits::kMinimumSampleRate || sampleRate > limits::kMaximumSampleRate ||
        now > kMaximumFrame - minimumLeadFrames) return std::nullopt;
    pattern = metronome::sanitize(pattern);
    const auto beatFrames = metronome::step_interval_samples(sampleRate, pattern.bpm,
        pattern.division, pattern.tempo_pulse_units) * static_cast<std::uint64_t>(pattern.division);
    if (beatFrames == 0) return std::nullopt;
    const double sourceEpoch = epochSourcePosition >= 0.0 ? epochSourcePosition : static_cast<double>(epoch);
    const auto estimatedMusical = transport_musical_frame_from_raw(estimatedOriginalTarget, renderOffset);
    const double beats = (tempo.position(estimatedMusical) - sourceEpoch) / beatFrames;
    if (!std::isfinite(beats) || beats < -0.5) return std::nullopt;
    const auto originalMusical = tempo.frame(sourceEpoch + std::floor(std::max(0.0, beats) + 0.5) * beatFrames);
    if (originalMusical == kMaximumFrame) return std::nullopt;
    const auto originalRaw = transport_raw_frame_from_musical(originalMusical, renderOffset);
    return recover_transport_position(now, renderOffset, originalRaw, countInFrames, minimumLeadFrames);
}

} // namespace jam2
