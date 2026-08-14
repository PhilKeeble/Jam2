#include "MetronomeTransportController.hpp"
#include "PlaybackGrid.hpp"

#include <QCoreApplication>
#include <QThread>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void testTapTempo()
{
    TapTempoTracker tracker;
    require(!tracker.tap(1000).has_value() && tracker.tap(1500) == 120,
        "two ordinary 500-ms taps must produce 120 BPM");
    require(tracker.tap(2020) == 118 && tracker.tap(2500) == 120 &&
            tracker.tap(3000) == 120 && tracker.tap(3500) == 120,
        "tap tempo must use the bounded rolling median of recent intervals");

    require(!tracker.tap(3600).has_value() && tracker.tap(4100) == 120,
        "a too-fast tap must reset prior intervals while retaining the new anchor");
    require(!tracker.tap(7001).has_value() && tracker.tap(7501) == 120,
        "a too-slow tap must begin a fresh sequence");
    require(!tracker.tap(7400).has_value() && tracker.tap(7900) == 120,
        "a backward timestamp must reset without signed subtraction");

    tracker.reset();
    require(!tracker.tap((std::numeric_limits<std::int64_t>::min)()).has_value() &&
            !tracker.tap((std::numeric_limits<std::int64_t>::max)()).has_value(),
        "signed timestamp extremes must remain defined and reset the sequence");
    tracker.reset();
    require(!tracker.tap((std::numeric_limits<std::int64_t>::max)() - 150).has_value() &&
            tracker.tap((std::numeric_limits<std::int64_t>::max)()) == 400,
        "valid tapping at the top of the signed timestamp domain must stay bounded");
    tracker.reset();
    require(!tracker.tap(0).has_value() && tracker.tap(150) == 400 &&
            tracker.tap(2150) == 56,
        "tap tempo must include the exact fastest and sequence-reset boundaries");
}

void testPlaybackGrid()
{
    PlaybackGrid grid;
    require(!grid.position().engineAnchored && grid.bpm() == 120.0,
        "grid must begin unanchored at its explicit default tempo");

    grid.setPattern(
        std::numeric_limits<double>::quiet_NaN(), 0,
        (std::numeric_limits<int>::max)(), 99);
    grid.updateEngine(100, 200, 50, -10, 7999, true);
    require(!grid.position().engineAnchored && grid.bpm() == 120.0,
        "nonfinite pattern and unsupported sample rate must fail to bounded defaults");

    grid.setPattern(120.0, 4, 4, 1);
    grid.updateEngine(48000, 48000, 0, 0, 48000, false);
    const PlaybackGrid::Position stopped = grid.position();
    require(stopped.engineAnchored && !stopped.running &&
            stopped.rawCurrentFrame == 48000 && stopped.currentFrame == 48000 &&
            stopped.sampleRate == 48000 && stopped.secondsPerBeat == 0.5 &&
            stopped.secondsPerStep == 0.125,
        "stopped grid must retain exact engine anchors and sanitized pattern timing");

    const std::uint64_t maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    grid.updateEngine(maximum, maximum, maximum, 0, 48000, true);
    QThread::msleep(2);
    const PlaybackGrid::Position saturated = grid.position();
    require(saturated.rawCurrentFrame == maximum &&
            saturated.currentFrame == maximum && saturated.absoluteStep == 0,
        "running interpolation at the final frame must saturate without wrapping");

    grid.clearEngine();
    require(!grid.position().engineAnchored,
        "clearing the engine must remove all public grid anchoring");
}

void testTransportController()
{
    std::vector<jam2::EngineCommand> submitted;
    bool accept = true;
    bool throwOnSubmit = false;
    MetronomeTransportController controller(
        [&](const jam2::EngineCommand& command) {
            if (throwOnSubmit) throw std::runtime_error("injected submit failure");
            submitted.push_back(command);
            return accept;
        });

    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::SetMetronomeEnabled;
    command.enabled = true;
    require(controller.submit(command) && submitted.size() == 1,
        "transport controller must submit through its narrow runtime boundary");
    accept = false;
    require(!controller.submit(command) && submitted.size() == 2,
        "runtime rejection must remain observable");
    throwOnSubmit = true;
    require(!controller.submit(command),
        "submit boundary must contain unexpected exceptions without terminating");
    MetronomeTransportController empty(
        MetronomeTransportController::CommandSubmitter{});
    require(!empty.submit(command), "missing submit target must fail closed");

    require(controller.localRunning() && controller.localGridMutationAllowed(),
        "controller must begin locally running and mutable");
    controller.setLocalState(false);
    controller.setApplyingRemoteSettings(true);
    require(!controller.localRunning() && controller.applyingRemoteSettings() &&
            !controller.localGridMutationAllowed() &&
            !MetronomeTransportController::allowsLocalGridMutation(true) &&
            MetronomeTransportController::allowsLocalGridMutation(false),
        "remote setting application must gate only local grid mutations");
    controller.setApplyingRemoteSettings(false);

    jam2::EngineSnapshot snapshot;
    snapshot.sample_rate = 48000.0;
    snapshot.engine_frame = 1000;
    snapshot.metronome_render_offset_frames = 100;
    snapshot.metronome_epoch_frame = 500;
    snapshot.metronome_epoch_valid = true;
    snapshot.metronome_pattern.bpm = 120;
    snapshot.metronome_pattern.beats_per_bar = 4;
    snapshot.metronome_pattern.division = 1;
    snapshot.metronome_pattern.tempo_pulse_units = 1;
    snapshot.transport_revision = 4;
    snapshot.transport_action = jam2::EngineTransportAction::RecordStart;
    snapshot.transport_countdown_start_frame = 2000;
    snapshot.transport_target_frame = 98000;
    const auto first = controller.consume(snapshot);
    const PlaybackGrid::Position ordinary = controller.grid().position();
    require(first.recordingScheduleAdvanced &&
            first.recordingCountdownStartFrame == 2000 &&
            first.recordingStartFrame == 98000 && ordinary.engineAnchored &&
            ordinary.rawCurrentFrame >= 1000 && ordinary.currentFrame >= 1100 &&
            ordinary.renderOffsetFrames == 100 && ordinary.epochFrame == 500,
        "snapshot consumption must publish its clock mapping and new recording schedule");
    require(!controller.consume(snapshot).recordingScheduleAdvanced,
        "the same recording schedule revision must not advance twice");
    snapshot.transport_revision = 5;
    snapshot.transport_action = jam2::EngineTransportAction::TrackRestart;
    require(!controller.consume(snapshot).recordingScheduleAdvanced,
        "non-recording transport revisions must not masquerade as recording schedules");

    snapshot.transport_action = jam2::EngineTransportAction::None;
    snapshot.metronome_epoch_valid = false;
    snapshot.engine_frame =
        (std::numeric_limits<std::uint64_t>::max)();
    snapshot.metronome_render_offset_frames = 1;
    controller.consume(snapshot);
    require(controller.grid().position().currentFrame ==
                (std::numeric_limits<std::uint64_t>::max)(),
        "positive render offset at the final raw frame must saturate");
    snapshot.metronome_render_offset_frames =
        (std::numeric_limits<std::int64_t>::min)();
    controller.consume(snapshot);
    require(controller.grid().position().currentFrame ==
                ((std::uint64_t{1} << 63U) - 1ULL),
        "minimum signed render offset must convert without undefined negation");

    snapshot.engine_frame = 0;
    snapshot.metronome_render_offset_frames = 0;
    snapshot.sample_rate = 7999.6;
    controller.consume(snapshot);
    require(controller.grid().position().sampleRate == 8000,
        "near-integer engine rates must retain round-then-validate behavior");
    snapshot.sample_rate = std::numeric_limits<double>::quiet_NaN();
    controller.consume(snapshot);
    require(!controller.grid().position().engineAnchored,
        "nonfinite engine sample rate must invalidate the UI grid without numeric conversion");
    snapshot.sample_rate = std::numeric_limits<double>::infinity();
    controller.consume(snapshot);
    require(!controller.grid().position().engineAnchored,
        "infinite engine sample rate must also fail closed");

    controller.clearEngine();
    snapshot.sample_rate = 48000.0;
    snapshot.engine_frame = 0;
    snapshot.metronome_render_offset_frames = 0;
    snapshot.transport_action = jam2::EngineTransportAction::RecordStart;
    snapshot.transport_revision = 4;
    require(controller.consume(snapshot).recordingScheduleAdvanced,
        "engine clear must reset recording-revision ownership for a restarted engine");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        testTapTempo();
        testPlaybackGrid();
        testTransportController();
        std::cout << "Metronome transport controller tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Metronome transport controller test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
