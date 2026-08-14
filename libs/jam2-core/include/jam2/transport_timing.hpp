#pragma once

#include "metronome.hpp"

#include <cstdint>
#include <optional>

namespace jam2 {

struct QuantizedTransportSchedule {
    std::uint64_t countdown_start_raw_frame = 0;
    std::uint64_t target_raw_frame = 0;
    std::uint64_t target_musical_frame = 0;
};

std::uint64_t transport_musical_frame_from_raw(
    std::uint64_t rawFrame,
    std::int64_t renderOffsetFrames) noexcept;

std::uint64_t transport_raw_frame_from_musical(
    std::uint64_t musicalFrame,
    std::int64_t renderOffsetFrames) noexcept;

std::optional<QuantizedTransportSchedule> next_bar_transport_schedule(
    double sampleRate,
    metronome::PatternSnapshot pattern,
    std::uint64_t rawCurrentFrame,
    std::int64_t renderOffsetFrames,
    bool epochValid,
    std::uint64_t epochMusicalFrame,
    int countInBars) noexcept;

} // namespace jam2
