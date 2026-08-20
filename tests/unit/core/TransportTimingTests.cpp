#include "transport_timing.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

jam2::metronome::PatternSnapshot normalPattern()
{
    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = 120;
    pattern.beats_per_bar = 4;
    pattern.division = 1;
    pattern.tempo_pulse_units = 1;
    return pattern;
}

void testFrameConversions()
{
    using limits = std::numeric_limits<std::uint64_t>;
    expect(jam2::transport_musical_frame_from_raw(123, 0) == 123 &&
            jam2::transport_raw_frame_from_musical(123, 0) == 123,
        "zero render offset preserves raw and musical frames");
    expect(jam2::transport_musical_frame_from_raw(200, 100) == 300 &&
            jam2::transport_raw_frame_from_musical(300, 100) == 200,
        "positive render offset round-trips an in-range frame");
    expect(jam2::transport_musical_frame_from_raw(200, -100) == 100 &&
            jam2::transport_raw_frame_from_musical(100, -100) == 200,
        "negative render offset round-trips an in-range frame");
    expect(jam2::transport_musical_frame_from_raw(50, -100) == 0 &&
            jam2::transport_raw_frame_from_musical(50, 100) == 0,
        "frame conversion floors an offset before the representable origin");
    expect(jam2::transport_musical_frame_from_raw(limits::max(), 1) ==
                limits::max() &&
            jam2::transport_raw_frame_from_musical(limits::max(), -1) ==
                limits::max(),
        "frame conversion saturates positive overflow");

    const auto minimumOffset = (std::numeric_limits<std::int64_t>::min)();
    expect(jam2::transport_musical_frame_from_raw(limits::max(), minimumOffset) ==
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)()) &&
            jam2::transport_raw_frame_from_musical(0, minimumOffset) ==
                (1ULL << 63U),
        "minimum signed render offset has a defined unsigned magnitude");
}

void testOrdinarySchedules()
{
    const auto oneBar = jam2::next_bar_transport_schedule(
        48000.0, normalPattern(), 1000, 0, true, 0, 1);
    expect(oneBar.has_value() &&
            oneBar->countdown_start_raw_frame == 96000 &&
            oneBar->target_raw_frame == 192000 &&
            oneBar->target_musical_frame == 192000,
        "one-bar count-in owns distinct countdown and target boundaries");

    const auto leadSkip = jam2::next_bar_transport_schedule(
        48000.0, normalPattern(), 90000, 0, true, 0, 0);
    expect(leadSkip.has_value() &&
            leadSkip->countdown_start_raw_frame == 192000 &&
            leadSkip->target_raw_frame == 192000,
        "a bar inside the 200 ms lead window advances once more");

    const auto positiveOffset = jam2::next_bar_transport_schedule(
        48000.0, normalPattern(), 1000, 1000, true, 0, 1);
    expect(positiveOffset.has_value() &&
            positiveOffset->countdown_start_raw_frame == 95000 &&
            positiveOffset->target_raw_frame == 191000 &&
            positiveOffset->target_musical_frame == 192000,
        "positive render offset preserves musical bar ownership");

    const auto negativeOffset = jam2::next_bar_transport_schedule(
        48000.0, normalPattern(), 1000, -1000, true, 0, 1);
    expect(negativeOffset.has_value() &&
            negativeOffset->countdown_start_raw_frame == 97000 &&
            negativeOffset->target_raw_frame == 193000 &&
            negativeOffset->target_musical_frame == 192000,
        "negative render offset preserves musical bar ownership");

    const auto anchoredEpoch = jam2::next_bar_transport_schedule(
        48000.0, normalPattern(), 60000, 0, true, 50000, 1);
    expect(anchoredEpoch.has_value() &&
            anchoredEpoch->countdown_start_raw_frame == 146000 &&
            anchoredEpoch->target_raw_frame == 242000,
        "nonzero epoch anchors the next bar and count-in target");

    auto subdivided = normalPattern();
    subdivided.beats_per_bar = 3;
    subdivided.division = 4;
    const auto compound = jam2::next_bar_transport_schedule(
        48000.0, subdivided, 1000, 0, true, 0, 8);
    expect(compound.has_value() &&
            compound->countdown_start_raw_frame == 72000 &&
            compound->target_raw_frame == 648000,
        "division, meter, and eight count-in bars retain exact frame math");
}

void testReceivedGridAlignment()
{
    const auto corrected = jam2::align_received_transport_to_grid(
        48000.0,
        normalPattern(),
        1000,
        1000,
        true,
        0,
        191304,
        96000);
    expect(corrected.has_value() &&
            corrected->countdown_start_raw_frame == 95000 &&
            corrected->target_raw_frame == 191000 &&
            corrected->target_musical_frame == 192000,
        "received quantized transport snaps arrival/RTT error to the shared bar");

    const auto anchored = jam2::align_received_transport_to_grid(
        48000.0,
        normalPattern(),
        1000,
        -500,
        true,
        50000,
        242300,
        96000);
    expect(anchored.has_value() &&
            anchored->countdown_start_raw_frame == 146500 &&
            anchored->target_raw_frame == 242500 &&
            anchored->target_musical_frame == 242000,
        "received grid alignment respects a mapped epoch and negative offset");

    expect(!jam2::align_received_transport_to_grid(
                48000.0,
                normalPattern(),
                95000,
                1000,
                true,
                0,
                191304,
                96000) &&
            !jam2::align_received_transport_to_grid(
                48000.0,
                normalPattern(),
                1000,
                1000,
                false,
                0,
                191304,
                96000),
        "received grid alignment rejects a missed countdown and an invalid epoch");
}

void testInvalidAndExhaustedSchedules()
{
    const auto pattern = normalPattern();
    expect(!jam2::next_bar_transport_schedule(
                (std::numeric_limits<double>::quiet_NaN)(),
                pattern, 0, 0, true, 0, 0) &&
            !jam2::next_bar_transport_schedule(
                (std::numeric_limits<double>::infinity)(),
                pattern, 0, 0, true, 0, 0) &&
            !jam2::next_bar_transport_schedule(
                7999.0, pattern, 0, 0, true, 0, 0) &&
            !jam2::next_bar_transport_schedule(
                384001.0, pattern, 0, 0, true, 0, 0),
        "invalid and unsupported sample rates fail closed");
    expect(!jam2::next_bar_transport_schedule(
                48000.0, pattern, 0, 0, true, 0, -1),
        "negative count-in bars fail closed");

    const std::uint64_t maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    expect(!jam2::next_bar_transport_schedule(
                48000.0, pattern, maximum, 0, true, 0, 0) &&
            !jam2::next_bar_transport_schedule(
                48000.0, pattern, 0, 0, true, maximum, 0) &&
            !jam2::next_bar_transport_schedule(
                48000.0, pattern, maximum, 1, true, 0, 0),
        "exhausted current, epoch, and offset ranges fail closed");
}

} // namespace

int main()
{
    testFrameConversions();
    testOrdinarySchedules();
    testReceivedGridAlignment();
    testInvalidAndExhaustedSchedules();
    if (failures != 0) {
        std::cerr << failures << " transport timing checks failed\n";
        return 1;
    }
    std::cout << "transport timing checks passed\n";
    return 0;
}
