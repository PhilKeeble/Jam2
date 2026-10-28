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

void testRestartRecovery()
{
    const auto unquantized = jam2::recover_transport_position(144000, 300, 24000, 0, 256);
    expect(unquantized && unquantized->source_frame == 120256 &&
        unquantized->schedule.target_musical_frame == 144556,
        "leader-audio and listener-compensation restarts retain elapsed time without beat quantization");
    const auto onTime = jam2::recover_received_restart(48000, normalPattern(), 1000, 0, true, 0, 24020, 0, 256);
    expect(onTime && onTime->schedule.target_raw_frame == 24000 && onTime->source_frame == 0,
        "an on-time restart retains the original beat and starts at source zero");
    const auto late = jam2::recover_received_restart(48000, normalPattern(), 144000, 0, true, 0, 24020, 0, 256);
    expect(late && late->schedule.target_raw_frame == 144256 && late->source_frame == 120256,
        "a receiver delayed over two seconds seeks to the current shared position");
    const auto countIn = jam2::recover_received_restart(48000, normalPattern(), 1000, 0, true, 0, 96000, 96000, 256);
    expect(countIn && countIn->schedule.target_raw_frame == 96000 && countIn->source_frame == 0,
        "missing the countdown does not postpone a future playback origin");
    const jam2::PlaybackTempo half{500000, 0, 0};
    const auto slowed = jam2::recover_received_restart(48000, normalPattern(), 144000, 0, true, 0, 48020, 0, 256, half);
    expect(slowed && slowed->source_frame == 96256,
        "recovery seeks elapsed rendered audio frames at a slowed shared tempo");
    expect(!jam2::recover_received_restart(48000, normalPattern(), 1000, 0, false, 0, 24000, 0, 256),
        "restart recovery requires a valid epoch");
    expect(!jam2::recover_received_restart(48000, normalPattern(), UINT64_MAX, 0, true, 0, 24000, 0, 256),
        "restart recovery rejects frame overflow");
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

    const auto late = jam2::align_received_transport_to_grid(
        48000.0,
        normalPattern(),
        95000,
        1000,
        true,
        0,
        191304,
        96000);
    expect(late.has_value() &&
            late->countdown_start_raw_frame == 191000 &&
            late->target_raw_frame == 287000 &&
            late->target_musical_frame == 288000,
        "a late received transport intent advances to the next exact shared bar");

    expect(!jam2::align_received_transport_to_grid(
                48000.0,
                normalPattern(),
                1000,
                1000,
                false,
                0,
                191304,
                96000),
        "received grid alignment rejects an invalid epoch");
}

void testCanonicalClockAndResetAfterSlowing()
{
    for (const std::uint64_t local : {2000ULL, 8000ULL, 900000ULL}) {
        const auto mapped = jam2::metronome::map_authority_clock(1000, 5000, local);
        expect(mapped.valid && mapped.epoch_sample_time == 1000 &&
            jam2::transport_musical_frame_from_raw(local, mapped.render_offset_frames) == 5000,
            "peers with different engine ages share the publisher's numeric clock and epoch");
    }
    expect(!jam2::metronome::map_authority_clock(1000, (std::numeric_limits<std::uint64_t>::max)(), 0).valid &&
        !jam2::metronome::map_authority_clock(1000, 0, (std::numeric_limits<std::uint64_t>::max)()).valid,
        "unrepresentable positive and negative shared-clock offsets are rejected");
    expect(jam2::metronome::raw_epoch_phase(1000, 50000, 24000) == 23000 &&
        jam2::metronome::raw_epoch_phase(1000, -12000, 24000) == 13000 &&
        jam2::metronome::raw_epoch_phase(1000, 1, 0) == 0,
        "listener phase reports translate nominal grid time into the peer's raw clock without losing negative epochs");
    const jam2::PlaybackTempo restored{1000000, 14020140, 12652167.5};
    constexpr std::uint64_t epoch = 18000000;
    const double sourceEpoch = restored.position(epoch);
    for (const auto& tempo : {restored, jam2::PlaybackTempo{500000, epoch, sourceEpoch}}) {
        for (const auto offset : {-12345LL, 0LL, 12345LL}) {
            const auto now = jam2::transport_raw_frame_from_musical(epoch + 1000, offset);
            const auto schedule = jam2::next_bar_transport_schedule(48000, normalPattern(), now,
                offset, true, epoch, 0, tempo, sourceEpoch);
            expect(schedule && schedule->target_raw_frame > now &&
                schedule->target_raw_frame - now <= static_cast<std::uint64_t>(96000 / tempo.rate() + 9600),
                "reset after accumulated half-speed time schedules the next bar, not 28 seconds ahead");
        }
    }
    for (const int rate : {500000, 730000, 1000000}) {
        const jam2::PlaybackTempo tempo{rate, epoch, sourceEpoch};
        const auto beat = static_cast<std::uint64_t>(std::llround(24000 / tempo.rate()));
        for (const std::uint64_t index : {1ULL, 2ULL, 3ULL, 4ULL}) {
            const auto target = epoch + index * beat;
            const auto received = jam2::align_received_transport_to_grid(48000, normalPattern(), epoch,
                0, true, epoch, target + 8, 0, tempo, sourceEpoch, jam2::TransportBoundary::Beat);
            expect(received && std::abs(static_cast<double>(received->target_raw_frame) - target) <= 2,
                "receiver preserves every sender-selected beat instead of moving to a bar");
        }
        expect(!jam2::align_received_transport_to_grid(48000, normalPattern(), epoch + beat + 100,
            0, true, epoch, epoch + beat, 0, tempo, sourceEpoch, jam2::TransportBoundary::Beat),
            "late beat restart is rejected instead of independently moving one peer forward");
    }
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
    testRestartRecovery();
    const jam2::PlaybackTempo half{500000, 48000, 48000.0};
    expect(half.position(48000) == 48000.0 && half.position(96000) == 72000.0 && half.frame(72000.0) == 96000,
        "playback tempo preserves its anchor and maps source frames in both directions");
    jam2::metronome::PatternSnapshot slowedPattern;
    const auto slowed = jam2::next_bar_transport_schedule(48000, slowedPattern, 60000, 0, true, 0, 1, half);
    expect(slowed && slowed->countdown_start_raw_frame == 144000 && slowed->target_raw_frame == 336000,
        "slowed playback keeps bar phase and doubles the physical count-in duration");
    const auto aligned = jam2::align_received_transport_to_grid(48000, slowedPattern, 60000, 0, true, 0, 343000, 192000, half);
    expect(aligned && aligned->target_raw_frame == 336000 && aligned->countdown_start_raw_frame == 144000,
        "receiver alignment uses the same slowed musical mapping");
    testCanonicalClockAndResetAfterSlowing();
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
