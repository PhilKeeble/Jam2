#pragma once

#include "metronome.hpp"
#include "playback_tempo.hpp"

#include <cstdint>
#include <optional>

namespace jam2 {

enum class TransportBoundary { Bar, Beat };

struct QuantizedTransportSchedule {
    std::uint64_t countdown_start_raw_frame = 0;
    std::uint64_t target_raw_frame = 0;
    std::uint64_t target_musical_frame = 0;
};

struct RecoveredTransportSchedule {
    QuantizedTransportSchedule schedule;
    std::uint64_t source_frame = 0;
};

std::optional<RecoveredTransportSchedule> recover_transport_position(
    std::uint64_t now, std::int64_t renderOffset,
    std::uint64_t originalTarget, std::uint64_t countInFrames,
    std::uint64_t minimumLeadFrames) noexcept;

// Keep the original shared beat as the playback origin; late receivers seek
// forward instead of moving that origin to a later beat.
std::optional<RecoveredTransportSchedule> recover_received_restart(
    double sampleRate, metronome::PatternSnapshot pattern,
    std::uint64_t now, std::int64_t renderOffset, bool epochValid,
    std::uint64_t epoch, std::uint64_t estimatedOriginalTarget,
    std::uint64_t countInFrames, std::uint64_t minimumLeadFrames,
    PlaybackTempo tempo = {}, double epochSourcePosition = -1.0) noexcept;

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
    int countInBars,
    PlaybackTempo tempo = {},
    double epochSourcePosition = -1.0) noexcept;

// Aligns a receiver-clock estimate of a peer's already quantized target to
// the nearest local representation of the same shared musical bar. The
// countdown length remains exact; a result is rejected if the full countdown
// can no longer begin in the future.
std::optional<QuantizedTransportSchedule> align_received_transport_to_grid(
    double sampleRate,
    metronome::PatternSnapshot pattern,
    std::uint64_t rawCurrentFrame,
    std::int64_t renderOffsetFrames,
    bool epochValid,
    std::uint64_t epochMusicalFrame,
    std::uint64_t estimatedTargetRawFrame,
    std::uint64_t countInFrames,
    PlaybackTempo tempo = {},
    double epochSourcePosition = -1.0,
    TransportBoundary boundaryKind = TransportBoundary::Bar) noexcept;

} // namespace jam2
