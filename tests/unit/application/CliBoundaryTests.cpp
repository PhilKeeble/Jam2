#include "CliEntrypoint.hpp"
#include "CliOptions.hpp"
#include "CliRuntimeSupport.hpp"
#include "CliStats.hpp"
#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <chrono>
#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memory>
#endif

namespace {

int failures = 0;
int testDeviceDispatches = 0;
int localDispatches = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <typename Callable>
void expectThrows(Callable&& callable, const char* message)
{
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

class Arguments {
public:
    Arguments(std::initializer_list<std::string_view> values)
    {
        storage_.reserve(values.size());
        for (const std::string_view value : values) storage_.emplace_back(value);
        pointers_.reserve(storage_.size());
        for (std::string& value : storage_) pointers_.push_back(value.data());
    }

    int count() const noexcept { return static_cast<int>(pointers_.size()); }
    char** data() noexcept { return pointers_.data(); }

private:
    std::vector<std::string> storage_;
    std::vector<char*> pointers_;
};

struct CapturedStreams {
    CapturedStreams()
        : previousOut(std::cout.rdbuf(output.rdbuf())),
          previousError(std::cerr.rdbuf(error.rdbuf()))
    {
    }

    ~CapturedStreams()
    {
        std::cout.rdbuf(previousOut);
        std::cerr.rdbuf(previousError);
    }

    std::ostringstream output;
    std::ostringstream error;
    std::streambuf* previousOut;
    std::streambuf* previousError;
};

std::pair<int, std::string> runFrontend(
    std::initializer_list<std::string_view> values)
{
    Arguments arguments(values);
    CapturedStreams capture;
    const int result = jam2::cli::runFrontend(arguments.count(), arguments.data());
    return {result, capture.output.str() + capture.error.str()};
}

void testFrontendAndHelpContracts()
{
    expect(jam2::cli::is_help_argument("-h") &&
            jam2::cli::is_help_argument("--help") &&
            jam2::cli::is_help_argument("help") &&
            !jam2::cli::is_help_argument("--helpful"),
        "CLI help tokens are exact");

    Arguments helpScan{"jam2", "local", "--stream-ms", "1", "--help"};
    expect(jam2::cli::has_help_argument(helpScan.count(), helpScan.data(), 2) &&
            !jam2::cli::has_help_argument(helpScan.count(), helpScan.data(), 5),
        "CLI help scanning honors its start boundary");

    const auto [bareCode, bareText] = runFrontend({"jam2"});
    expect(bareCode == 0 && bareText.find("direct low-latency") != std::string::npos,
        "direct frontend exposes top-level usage for an empty CLI invocation");
    for (const std::string_view token : {"-h", "--help", "help"}) {
        const auto [code, text] = runFrontend({"jam2", token});
        expect(code == 0 && text.find("Commands:") != std::string::npos,
            "every top-level help token prints usage");
    }

    const auto [listHelpCode, listHelp] =
        runFrontend({"jam2", "list-devices", "--help"});
    const auto [deviceHelpCode, deviceHelp] =
        runFrontend({"jam2", "test-device", "-h"});
    const auto [localHelpCode, localHelp] =
        runFrontend({"jam2", "local", "help"});
    const auto [networkHelpCode, networkHelp] = runFrontend({"jam2", "network"});
    const auto [createHelpCode, createHelp] =
        runFrontend({"jam2", "network", "create", "--help"});
    const auto [joinHelpCode, joinHelp] =
        runFrontend({"jam2", "network", "join", "-h"});
    expect(listHelpCode == 0 && listHelp.find("jam2 list-devices") != std::string::npos,
        "list-devices help is complete");
    expect(deviceHelpCode == 0 && deviceHelp.find("jam2 test-device <id>") != std::string::npos,
        "test-device help is complete");
    expect(localHelpCode == 0 && localHelp.find("headless-audio") != std::string::npos,
        "local help includes audio options");
    expect(networkHelpCode == 0 && networkHelp.find("Subcommands:") != std::string::npos,
        "network help lists its public subcommands");
    expect(createHelpCode == 0 && createHelp.find("max-peers") != std::string::npos,
        "network-create help includes bootstrap options");
    expect(joinHelpCode == 0 && joinHelp.find("jam2-url") != std::string::npos,
        "network-join help includes its invitation contract");

    const auto [deviceDispatchCode, ignoredDeviceText] =
        runFrontend({"jam2", "test-device", "7"});
    const auto [localDispatchCode, ignoredLocalText] =
        runFrontend({"jam2", "local", "--headless-audio", "on"});
    (void)ignoredDeviceText;
    (void)ignoredLocalText;
    expect(deviceDispatchCode == 17 && localDispatchCode == 18 &&
            testDeviceDispatches == 1 && localDispatches == 1,
        "frontend dispatches device and local commands through their owned runtimes");

    const auto [ownedCreateCode, ownedCreateText] =
        runFrontend({"jam2", "network", "create"});
    const auto [ownedJoinCode, ownedJoinText] =
        runFrontend({"jam2", "network", "join", "jam2://fixture"});
    const auto [unknownNetworkCode, unknownNetworkText] =
        runFrontend({"jam2", "network", "relay"});
    const auto [unknownCode, unknownText] = runFrontend({"jam2", "unknown"});
    expect(ownedCreateCode == 1 && ownedCreateText.find("unified Jam2 application") != std::string::npos &&
            ownedJoinCode == 1 && ownedJoinText.find("unified Jam2 application") != std::string::npos,
        "legacy frontend refuses to own public network bootstrap");
    expect(unknownNetworkCode == 2 && unknownNetworkText.find("Unknown network subcommand") != std::string::npos &&
            unknownCode == 2 && unknownText.find("Unknown command") != std::string::npos,
        "unknown CLI commands retain the argument-error result");

    expectThrows([] { jam2::cli::print_device_help("unknown"); },
        "device help rejects an unowned command");
}

void testOptionAndStatsContracts()
{
    Arguments valid{
        "jam2", "local", "--headless-audio", "on",
        "--input-channels", "1,3", "--output-channels", "2",
        "--stream-ms", "1"};
    const Jam2RuntimeOptions options =
        jam2::cli::parse_options(valid.count(), valid.data(), 2);
    expect(options.headless_audio && options.stream_ms == 1 &&
            options.channel_selection.input == std::vector<int>({0, 2}) &&
            options.channel_selection.output == std::vector<int>({1}) &&
            options.os_priority == Jam2OsPriorityMode::High,
        "channel-list parsing preserves unique one-based CLI selections as zero-based indices");

    for (const std::string_view profile : {"fast", "moderate", "safe"}) {
        Arguments preset{"jam2", "network", "create", "--profile", profile};
        const auto create = jam2::cli::parse_options(preset.count(), preset.data(), 3);
        const auto join = jam2::cli::parse_options(
            preset.count(), preset.data(), 3, Jam2ProfileApplication::Join);
        expect(create.drift_deadband_ppm == 0 && join.drift_deadband_ppm == 0,
            "CLI create and join apply zero drift deadband for every maintained profile");
        if (profile == "fast") {
            expect(create.frame_size == 64 && create.audio_buffer_size == 32 &&
                    create.playback_prefill_frames == 64 && join.playback_prefill_frames == 64,
                "CLI Fast defaults to 64-frame packets, 32-frame callbacks and 64-frame prefill");
        }
    }
    Arguments numericOverride{"jam2", "network", "create", "--profile", "fast",
        "--frame-size", "128", "--playback-prefill-frames", "192", "--drift-deadband-ppm", "7"};
    const auto overridden = jam2::cli::parse_options(numericOverride.count(), numericOverride.data(), 3);
    expect(overridden.frame_size == 128 && overridden.playback_prefill_frames == 192 &&
            overridden.drift_deadband_ppm == 7,
        "explicit packet, prefill and deadband overrides remain available for controlled comparisons");

    const auto offScheduling =
        jam2::cli::windows_scheduling_request(Jam2OsPriorityMode::Off);
    const auto highScheduling =
        jam2::cli::windows_scheduling_request(Jam2OsPriorityMode::High);
    expect(offScheduling.process == jam2::cli::WindowsProcessPriorityRequest::Unchanged &&
            offScheduling.thread == jam2::cli::WindowsThreadPriorityRequest::Unchanged &&
            offScheduling.mmcss == jam2::cli::WindowsMmcssPriorityRequest::Off &&
            highScheduling.process == jam2::cli::WindowsProcessPriorityRequest::High &&
            highScheduling.thread == jam2::cli::WindowsThreadPriorityRequest::Highest &&
            highScheduling.mmcss == jam2::cli::WindowsMmcssPriorityRequest::High &&
            jam2::cli::windows_mmcss_priority_text(highScheduling.mmcss) == "high",
        "Windows scheduling modes map to explicit process, worker, and MMCSS priorities");

    Arguments removedPriority{
        "jam2", "local", "--headless-audio", "on", "--os-priority", "realtime"};
    expectThrows([&] {
        (void)jam2::cli::parse_options(
            removedPriority.count(), removedPriority.data(), 2);
    }, "removed realtime scheduling mode is rejected");

    for (const std::string_view invalid : {"1,", "1,1", "0", "one"}) {
        Arguments arguments{"jam2", "local", "--input-channels", invalid};
        expectThrows([&] {
            (void)jam2::cli::parse_options(arguments.count(), arguments.data(), 2);
        }, "invalid channel lists are rejected");
    }

    Arguments separateKey{
        "jam2", "network", "join", "--session-key", "secret-value"};
    const std::string separateText = jam2::cli::stats::command_line_text(
        separateKey.count(), separateKey.data());
    Arguments embedded{
        "jam2", "network", "join",
        "jam2://v1?host=127.0.0.1&key=invite-secret&peer=2",
        "--session-key=inline-secret"};
    const std::string embeddedText = jam2::cli::stats::command_line_text(
        embedded.count(), embedded.data());
    expect(separateText.find("secret-value") == std::string::npos &&
            embeddedText.find("invite-secret") == std::string::npos &&
            embeddedText.find("inline-secret") == std::string::npos &&
            separateText.find("<redacted>") != std::string::npos &&
            embeddedText.find("<redacted>") != std::string::npos,
        "diagnostic command lines redact every supported session-key shape");

    jam2::PeerStreamStats source;
    source.replay_rejects = 3;
    source.sequence.lost = 4;
    source.playback_depth_samples = 5;
    source.jitter_buffer_forced_releases = 6;
    source.adaptive_playback_burst_events = 7;
    source.jitter_buffer_target_releases = 8;
    source.jitter_buffer_timeout_releases = 9;
    source.jitter_buffer_rebases = 10;
    jam2::cli::stats::AudioPacketStats copied;
    jam2::cli::stats::copy_peer_stream_stats(copied, source);
    expect(copied.udp_replay_rejects == 3 && copied.sequence.lost == 4 &&
            copied.playback_depth_samples == 5 &&
            copied.jitter_buffer_forced_releases == 6 &&
            copied.adaptive_playback_burst_events == 7 &&
            copied.jitter_buffer_target_releases == 8 &&
            copied.jitter_buffer_timeout_releases == 9 &&
            copied.jitter_buffer_rebases == 10,
        "CLI diagnostics copy the complete peer-stream snapshot categories");

    jam2::PeerMixerStats mixerSource;
    mixerSource.receive_recovery_active = true;
    mixerSource.receive_recovery_events = 11;
    mixerSource.receive_recovery_completions = 12;
    mixerSource.receive_recovery_debt_frames = 13;
    mixerSource.receive_recovery_debt_max_frames = 14;
    mixerSource.receive_recovery_duration_us = 15;
    mixerSource.receive_recovery_duration_max_us = 17;
    mixerSource.receive_recovery_trigger_frames = 256;
    mixerSource.adaptive_delivery_gap_hold_active = true;
    mixerSource.adaptive_delivery_gap_raise_events = 18;
    mixerSource.adaptive_delivery_gap_hold_events = 19;
    mixerSource.adaptive_delivery_gap_hold_remaining_us = 20000;
    mixerSource.adaptive_delivery_gap_required_frames = 21;
    mixerSource.adaptive_delivery_gap_required_max_frames = 22;
    mixerSource.live_tail_batch_tolerance_frames = 128;
    mixerSource.normal_output_limit_frames = 256;
    mixerSource.normal_path_limit_frames = 320;
    mixerSource.live_tail_rounding_tolerance_frames = 64;
    mixerSource.adaptive_release_alignment_margin_frames = 64;
    mixerSource.partial_release_events = 23;
    mixerSource.partial_release_frames = 63;
    mixerSource.lookahead_hold_events = 31;
    mixerSource.lookahead_hold_frames = 32;
    mixerSource.lookahead_hold_max_fraction_ppm = 33;
    mixerSource.isolated_recovery_events = 34;
    mixerSource.worker_service_gap_us = 35;
    mixerSource.worker_runway_frames = 36;
    mixerSource.playback_stopping_frames = 37;
    mixerSource.release_worker_guard_active = true;
    jam2::cli::stats::copy_peer_mixer_stats(copied, mixerSource);
    expect(copied.mix_receive_recovery_active &&
            copied.mix_receive_recovery_events == 11 &&
            copied.mix_receive_recovery_completions == 12 &&
            copied.mix_receive_recovery_debt_frames == 13 &&
            copied.mix_receive_recovery_debt_max_frames == 14 &&
            copied.mix_receive_recovery_duration_us == 15 &&
            copied.mix_receive_recovery_duration_max_us == 17 &&
            copied.mix_receive_recovery_trigger_frames == 256 &&
            copied.adaptive_delivery_gap_hold_active &&
            copied.adaptive_delivery_gap_raise_events == 18 &&
            copied.adaptive_delivery_gap_hold_events == 19 &&
            copied.adaptive_delivery_gap_hold_remaining_us == 20000 &&
            copied.adaptive_delivery_gap_required_frames == 21 &&
            copied.adaptive_delivery_gap_required_max_frames == 22 &&
            copied.mix_live_tail_batch_tolerance_frames == 128 &&
            copied.mix_normal_output_limit_frames == 256 && copied.mix_normal_path_limit_frames == 320 &&
            copied.mix_live_tail_rounding_tolerance_frames == 64 &&
            copied.mix_adaptive_release_alignment_margin_frames == 64 &&
            copied.mix_partial_release_events == 23 && copied.mix_partial_release_frames == 63 &&
            copied.mix_lookahead_hold_events == 31 &&
            copied.mix_lookahead_hold_frames == 32 &&
            copied.mix_lookahead_hold_max_fraction_ppm == 33 &&
            copied.mix_isolated_recovery_events == 34 &&
            copied.mix_worker_service_gap_us == 35 &&
            copied.mix_worker_runway_frames == 36 &&
            copied.mix_playback_stopping_frames == 37 &&
            copied.mix_release_worker_guard_active,
        "CLI diagnostics copy debt-driven recovery and delivery-gap learning state");

    jam2::PeerStreamStats firstGap;
    firstGap.audio_packet_gap_min_us = 1200;
    firstGap.audio_packet_gap_sum_us = 4000;
    firstGap.audio_packet_gap_max_us = 2800;
    firstGap.audio_packet_gap_samples = 2;
    firstGap.audio_packet_gap_over_2x_count = 1;
    jam2::PeerStreamStats secondGap;
    secondGap.audio_packet_gap_min_us = 900;
    secondGap.audio_packet_gap_sum_us = 5900;
    secondGap.audio_packet_gap_max_us = 5000;
    secondGap.audio_packet_gap_samples = 2;
    secondGap.audio_packet_gap_over_2x_count = 1;
    secondGap.audio_packet_gap_over_4x_count = 1;
    jam2::cli::stats::AudioPacketStats aggregated;
    jam2::cli::stats::add_peer_stream_stats(aggregated, firstGap);
    jam2::cli::stats::add_peer_stream_stats(aggregated, secondGap);
    expect(aggregated.audio_packet_gap_min_us == 900 &&
            aggregated.audio_packet_gap_sum_us == 9900 &&
            aggregated.audio_packet_gap_max_us == 5000 &&
            aggregated.audio_packet_gap_samples == 4 &&
            aggregated.audio_packet_gap_over_2x_count == 2 &&
            aggregated.audio_packet_gap_over_4x_count == 1,
        "mesh diagnostics aggregate exact packet-gap timing across peers");

    jam2::cli::stats::ReceiveLoopDiagnostics receiveLoop;
    receiveLoop.beginWake(1000);
    receiveLoop.finishWake(0, 0, 0);
    receiveLoop.beginWake(2200);
    receiveLoop.finishWake(7, 2300, 2600);
    receiveLoop.beginWake(7200);
    receiveLoop.finishWake(64, 7300, 8100);
    jam2::cli::stats::AudioPacketStats receiveLoopStats;
    receiveLoop.applyTo(receiveLoopStats);
    expect(receiveLoopStats.receive_loop_gap_min_us == 1200 &&
            receiveLoopStats.receive_loop_gap_sum_us == 6200 &&
            receiveLoopStats.receive_loop_gap_max_us == 5000 &&
            receiveLoopStats.receive_loop_gap_samples == 2 &&
            receiveLoopStats.recv_loop_iterations == 3 &&
            receiveLoopStats.recv_loop_idle_count == 1 &&
            receiveLoopStats.recv_loop_batch_sum == 71 &&
            receiveLoopStats.recv_loop_batch_max == 64 &&
            receiveLoopStats.receive_processing_min_us == 300 &&
            receiveLoopStats.receive_processing_sum_us == 1100 &&
            receiveLoopStats.receive_processing_max_us == 800 &&
            receiveLoopStats.receive_processing_samples == 2,
        "receive-loop diagnostics expose scheduling gaps and bounded drain batches");

    receiveLoopStats.pre_receive_work_samples = 4;
    receiveLoopStats.pre_receive_advance_sum_us = 20;
    receiveLoopStats.pre_receive_maintenance_sum_us = 40;
    receiveLoopStats.pre_receive_send_sum_us = 60;
    receiveLoopStats.pre_receive_peak_advance_us = 7;
    receiveLoopStats.pre_receive_peak_maintenance_us = 11;
    receiveLoopStats.pre_receive_peak_send_us = 13;
    expect(receiveLoopStats.pre_receive_advance_sum_us /
                receiveLoopStats.pre_receive_work_samples == 5 &&
            receiveLoopStats.pre_receive_maintenance_sum_us /
                receiveLoopStats.pre_receive_work_samples == 10 &&
            receiveLoopStats.pre_receive_send_sum_us /
                receiveLoopStats.pre_receive_work_samples == 15 &&
            receiveLoopStats.pre_receive_peak_advance_us +
                receiveLoopStats.pre_receive_peak_maintenance_us +
                receiveLoopStats.pre_receive_peak_send_us == 31,
        "pre-receive diagnostics retain stage averages and the exact peak decomposition");

    Jam2RuntimeOptions statsOptions;
    statsOptions.sample_rate = 48000;
    statsOptions.frame_size = 64;
    copied.os_scheduling.process_priority_error = "priority-request-failed";
    copied.os_scheduling.mmcss_priority_active = "high";
    copied.capture_ready_wake_signals = 9;
    copied.capture_ready_wake_consumptions = 8;
    copied.capture_ready_dispatch_sum_us = 60;
    copied.capture_ready_dispatch_samples = 3;
    copied.capture_clock_packet_pacing_active = true;
    jam2::cli::stats::CsvStatsLog::AudioSnapshot audio;
    audio.stream.input_latency_frames = 32;
    audio.stream.output_latency_frames = 64;
    audio.driver_output_ready_status =
        jam2::audio::DriverOutputReadyStatus::Active;
    audio.driver_output_ready_latency_reduction_frames = 32;
    audio.stream.maximum_callback_frames = 64;
    audio.stream.variable_callback_frames = true;
    audio.stream.input_device_latency_frames = 8;
    audio.stream.input_safety_offset_frames = 24;
    audio.stream.input_stream_latency_frames = 4;
    audio.callback_timing.frame_min = 32;
    audio.callback_timing.frame_max = 64;
    audio.callback_timing.frame_samples = 10;
    audio.callback_timing.processor_overloads = 2;
    audio.callback_timing.cycle_jitter_max_ns = 125000;
    audio.callback_timing.cycle_jitter_samples = 9;
    QTemporaryDir statsRoot;
    expect(statsRoot.isValid(), "CLI stats test creates a temporary artifact root");
    std::filesystem::path statsCsvPath;
    if (statsRoot.isValid()) {
        {
            jam2::cli::stats::CsvStatsLog log(
                std::filesystem::path(statsRoot.path().toStdString()), {});
            statsCsvPath = log.path();
            log.write_periodic(24, copied, statsOptions, audio);
            log.write("final", 25, copied, statsOptions, audio);
        }
        QFile statsCsv(QString::fromStdString(statsCsvPath.string()));
        const bool opened = statsCsv.open(QIODevice::ReadOnly);
        const QList<QByteArray> lines = opened
            ? statsCsv.readAll().trimmed().split('\n')
            : QList<QByteArray>{};
        const QByteArray header = lines.size() == 3 ? lines[0].trimmed() : QByteArray{};
        const QByteArray periodicRow = lines.size() == 3 ? lines[1].trimmed() : QByteArray{};
        const QByteArray finalRow = lines.size() == 3 ? lines[2].trimmed() : QByteArray{};
        const auto csvFieldCount = [](const QByteArray& row) {
            int fields = 1;
            bool quoted = false;
            for (qsizetype index = 0; index < row.size(); ++index) {
                if (row[index] == '"') {
                    if (quoted && index + 1 < row.size() && row[index + 1] == '"') {
                        ++index;
                    } else {
                        quoted = !quoted;
                    }
                } else if (row[index] == ',' && !quoted) {
                    ++fields;
                }
            }
            return fields;
        };
        const bool schemaMatches = opened && lines.size() == 3 &&
                csvFieldCount(header) == csvFieldCount(finalRow) &&
                csvFieldCount(header) == csvFieldCount(periodicRow) &&
                header.endsWith("mix_live_tail_rounding_tolerance_frames,mix_adaptive_release_alignment_margin_frames,mix_partial_release_events,mix_partial_release_frames,mix_lookahead_hold_events,mix_lookahead_hold_frames,mix_lookahead_hold_max_fraction_ppm,mix_isolated_recovery_events,mix_worker_service_gap_us,mix_worker_runway_frames,mix_playback_stopping_frames,mix_release_worker_guard_active") &&
                periodicRow.endsWith(",17,256,yes,18,19,20,21,22,128,256,320,64,64,23,63,31,32,33,34,35,36,37,yes") &&
                finalRow.endsWith(",17,256,yes,18,19,20,21,22,128,256,320,64,64,23,63,31,32,33,34,35,36,37,yes");
        expect(schemaMatches,
            "final CSV preserves the capture-clock pacing field and schema width");
    }
    CapturedStreams capture;
    jam2::cli::stats::print_periodic_stream_stats(copied, statsOptions, audio, 25);
    expect(capture.output.str().find("stats elapsed_ms=25") != std::string::npos &&
            capture.output.str().find("sequence_lost=4") != std::string::npos &&
            capture.output.str().find(
                "os_process_priority_error=priority-request-failed") !=
                std::string::npos &&
            capture.output.str().find("os_mmcss_priority_active=high") !=
                std::string::npos &&
            capture.output.str().find("capture_ready_wake_signals=9") !=
                std::string::npos &&
            capture.output.str().find("capture_ready_dispatch_avg_us=20") !=
                std::string::npos &&
            capture.output.str().find(
                "capture_clock_packet_pacing_active=yes") !=
                std::string::npos &&
            capture.output.str().find("driver_output_ready=active") !=
                std::string::npos &&
            capture.output.str().find("driver_input_latency_frames=32") !=
                std::string::npos &&
            capture.output.str().find("driver_output_latency_frames=64") !=
                std::string::npos &&
            capture.output.str().find(
                "driver_output_ready_latency_reduction_frames=32") !=
                std::string::npos &&
            capture.output.str().find("audio_callback_frames_max=64") !=
                std::string::npos &&
            capture.output.str().find("coreaudio_processor_overloads=2") !=
                std::string::npos &&
            capture.output.str().find("coreaudio_cycle_jitter_max_us=125") !=
                std::string::npos,
        "periodic CLI diagnostics emit exact raw stream counters");
    statsOptions.adaptive_playback_ratio_ramp_ms = 250;
    jam2::cli::stats::print_audio_packet_stats(copied, statsOptions);
    expect(capture.output.str().find("Adaptive playback ratio ramp ms: 250") != std::string::npos &&
            capture.output.str().find("Adaptive playback release return ramp ms: 50") != std::string::npos,
        "CLI diagnostics distinguish configured acceleration from bounded release cancellation");
    statsOptions.adaptive_playback_ratio_ramp_ms = 10;
    jam2::cli::stats::print_audio_packet_stats(copied, statsOptions);
    expect(capture.output.str().find("Adaptive playback release return ramp ms: 10") != std::string::npos,
        "CLI cancellation diagnostics preserve faster custom ramps");

    expect(jam2::cli::os_error_text(0).empty() &&
            jam2::cli::os_error_text(5) == "error 5",
        "CLI platform diagnostics preserve empty-success and numeric-error text");

    jam2::Engine engine;
    jam2::cli::CliPeerStreamPlayback playback(&engine);
    const std::array<std::int32_t, 2> frames{1, 2};
    expect(playback.acceptsFrames() && playback.depthFrames() == 0 && playback.callbackFrames() == 0 &&
            playback.playbackRecoveryState().unity && playback.playbackRecoveryState().stopping_frames == 0 &&
            playback.pushFrames(frames) == 0,
        "CLI playback adapter delegates safely to an unstarted engine");
    playback.requestDropFrames(1);
    playback.setResamplerRatio(1.0001);
    playback.detach();
    playback.requestDropFrames(1);
    playback.setResamplerRatio(1.0);
    expect(!playback.acceptsFrames() && playback.pushFrames(frames) == 0 && playback.callbackFrames() == 0 &&
            playback.depthFrames() ==
                (std::numeric_limits<std::size_t>::max)() / 2U,
        "detached CLI playback adapter is a bounded non-accepting sink");
}

struct ProcessResult {
    bool started = false;
    bool finished = false;
    QProcess::ExitStatus status = QProcess::CrashExit;
    int code = -1;
    QByteArray output;
};

ProcessResult runProcess(const QString& executable, const QStringList& arguments)
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    const int timeout = static_cast<int>(
        jam2::test::deadmanTimeout(std::chrono::seconds(30)).count());
    ProcessResult result;
    result.started = process.waitForStarted(timeout);
    if (result.started) result.finished = process.waitForFinished(timeout);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.status = process.exitStatus();
    result.code = process.exitCode();
    result.output = process.readAll();
    return result;
}

#if defined(_WIN32)
struct CloseTestHandle {
    void operator()(void* handle) const noexcept { (void)CloseHandle(handle); }
};
using TestHandle = std::unique_ptr<void, CloseTestHandle>;

int runConsoleChild(const QString& executable, const QString& arguments)
{
    std::wstring command = (QStringLiteral("\"") + QDir::toNativeSeparators(executable) +
        QStringLiteral("\" ") + arguments).toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // Deliberately supply no inherited streams: Jam2 must attach to this
    // parent's console and initialize its own CRT streams.
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &startup, &process);
    expect(started != FALSE, "terminal starts staged GUI-subsystem executable");
    if (!started) return -1;
    TestHandle processHandle(process.hProcess);
    TestHandle threadHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(processHandle.get(), 15000);
    expect(waited == WAIT_OBJECT_0, "attached CLI command exits within its deadline");
    if (waited != WAIT_OBJECT_0) {
        (void)TerminateProcess(processHandle.get(), 1);
        (void)WaitForSingleObject(processHandle.get(), 5000);
        return -1;
    }
    DWORD code = 0;
    expect(GetExitCodeProcess(processHandle.get(), &code) != FALSE,
        "attached CLI exit code is available to its caller");
    return static_cast<int>(code);
}

int testParentConsole(const QString& executable)
{
    TestHandle console(CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (console.get() == INVALID_HANDLE_VALUE) {
        (void)console.release();
        expect(false, "terminal fixture has its own console");
        return 1;
    }
    expect(runConsoleChild(executable, QStringLiteral("--help")) == 0,
        "CLI help succeeds when launched directly from a terminal");
    expect(runConsoleChild(executable, QStringLiteral("unknown-command")) == 2,
        "CLI invalid command preserves its nonzero exit code");
    expect(runConsoleChild(executable,
        QStringLiteral("local --headless-audio on --stream-ms 150 --test-input tone-440")) == 0,
        "bounded headless audio succeeds from a terminal");
    CONSOLE_SCREEN_BUFFER_INFO info{};
    expect(GetConsoleScreenBufferInfo(console.get(), &info) != FALSE,
        "terminal fixture can inspect actual console output");
    const int length = info.dwSize.X * (info.dwCursorPosition.Y + 1);
    QByteArray screen(length, '\0');
    DWORD read = 0;
    expect(length > 0 && ReadConsoleOutputCharacterA(console.get(), screen.data(),
        static_cast<DWORD>(screen.size()), COORD{0, 0}, &read) != FALSE,
        "terminal fixture reads attached CLI output");
    screen.resize(read);
    expect(screen.contains("Usage:") && screen.contains("Unknown command: unknown-command") &&
            screen.contains("\"mode\":\"local\""),
        "help, stderr and headless startup reach the launching terminal");

    // Redirection must also survive attachment to a real parent console.
    const ProcessResult redirected = runProcess(executable, {QStringLiteral("--help")});
    expect(redirected.finished && redirected.code == 0 && redirected.output.contains("Usage:"),
        "inherited output pipe survives attaching to a parent console");

    QProcess gui;
    gui.setProgram(executable);
    gui.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
    });
    gui.start();
    const bool guiStarted = gui.waitForStarted(10000);
    expect(guiStarted, "no-argument launch starts the actual GUI");
    if (guiStarted) {
        const DWORD guiId = static_cast<DWORD>(gui.processId());
        TestHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, guiId));
        expect(process && WaitForInputIdle(process.get(), 15000) == 0,
            "no-argument launch reaches the GUI event loop");
        console.reset();
        expect(FreeConsole() != FALSE, "terminal fixture detaches before probing GUI console state");
        const BOOL attached = AttachConsole(guiId);
        const DWORD attachError = attached ? ERROR_SUCCESS : GetLastError();
        expect(!attached && attachError == ERROR_INVALID_HANDLE,
            "actual no-argument GUI has neither a new console nor the parent terminal");
        if (attached) (void)FreeConsole();

        struct WindowSearch { DWORD processId; HWND window = nullptr; } search{guiId};
        EnumWindows([](HWND window, LPARAM context) -> BOOL {
            auto& found = *reinterpret_cast<WindowSearch*>(context);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            wchar_t title[128]{};
            GetWindowTextW(window, title, 128);
            if (owner == found.processId && std::wstring_view(title) == L"Jam2gether" &&
                GetWindow(window, GW_OWNER) == nullptr) {
                found.window = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        expect(search.window != nullptr, "no-argument GUI creates its main window");
        // This fixture tests startup only. Avoid the separate project-save
        // close workflow and ensure the hidden GUI cannot outlive the fixture.
        gui.kill();
        expect(gui.waitForFinished(5000), "startup fixture cleans up its GUI process");
    }
    return failures == 0 ? 0 : 1;
}

void testWindowsStartup(const QString& executable)
{
    QFile binary(executable);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 headers{};
    const bool opened = binary.open(QIODevice::ReadOnly);
    const bool validHeaders = opened &&
        binary.read(reinterpret_cast<char*>(&dos), sizeof(dos)) == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= sizeof(dos) &&
        binary.seek(dos.e_lfanew) &&
        binary.read(reinterpret_cast<char*>(&headers), sizeof(headers)) == sizeof(headers) &&
        headers.Signature == IMAGE_NT_SIGNATURE;
    expect(validHeaders && headers.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI,
        "public executable starts as a Windows GUI without creating a console");

    QProcess terminal;
    terminal.setProgram(QCoreApplication::applicationFilePath());
    terminal.setArguments({QStringLiteral("--console-parent"), executable});
    terminal.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags = (args->flags & ~CREATE_NO_WINDOW) | CREATE_NEW_CONSOLE;
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
    });
    terminal.start();
    const bool started = terminal.waitForStarted(10000);
    const bool finished = started && terminal.waitForFinished(30000);
    expect(finished && terminal.exitStatus() == QProcess::NormalExit && terminal.exitCode() == 0,
        "native terminal attachment and headless regression checks pass");
    if (!finished) {
        terminal.kill();
        terminal.waitForFinished(5000);
    }
    if (terminal.exitCode() != 0) std::cerr << terminal.readAllStandardError().toStdString();

    QTemporaryDir root;
    expect(root.isValid(), "redirected CLI fixture creates a temporary directory");
    if (!root.isValid()) return;
    const QString outputPath = QDir(root.path()).filePath(QStringLiteral("stdout.txt"));
    const QString errorPath = QDir(root.path()).filePath(QStringLiteral("stderr.txt"));
    QProcess redirected;
    redirected.setProgram(executable);
    redirected.setArguments({QStringLiteral("unknown-command")});
    redirected.setStandardOutputFile(outputPath);
    redirected.setStandardErrorFile(errorPath);
    redirected.start();
    const bool redirectedStarted = redirected.waitForStarted(10000);
    const bool redirectedFinished = redirectedStarted && redirected.waitForFinished(10000);
    if (!redirectedFinished) {
        redirected.kill();
        redirected.waitForFinished(5000);
    }
    QFile output(outputPath);
    QFile error(errorPath);
    expect(redirectedFinished && redirected.exitStatus() == QProcess::NormalExit &&
            redirected.exitCode() == 2 && output.open(QIODevice::ReadOnly) &&
            error.open(QIODevice::ReadOnly) && output.readAll().isEmpty() &&
            error.readAll().contains("Unknown command: unknown-command"),
        "file redirection preserves separate stdout/stderr and nonzero exit codes");
}
#endif

void testStagedLocalRuntime(const QString& executable)
{
    const ProcessResult createHelp = runProcess(executable, {
        QStringLiteral("network"), QStringLiteral("create"),
        QStringLiteral("--help"),
    });
    const ProcessResult joinHelp = runProcess(executable, {
        QStringLiteral("network"), QStringLiteral("join"),
        QStringLiteral("jam2://fixture"), QStringLiteral("help"),
    });
    expect(createHelp.started && createHelp.finished &&
            createHelp.status == QProcess::NormalExit && createHelp.code == 0 &&
            createHelp.output.contains("jam2 network create") &&
            joinHelp.started && joinHelp.finished &&
            joinHelp.status == QProcess::NormalExit && joinHelp.code == 0 &&
            joinHelp.output.contains("jam2 network join"),
        "staged application routes network help tokens without starting a session");

    const ProcessResult missingDevice =
        runProcess(executable, {QStringLiteral("test-device")});
    expect(missingDevice.started && missingDevice.finished &&
            missingDevice.status == QProcess::NormalExit &&
            missingDevice.code == 1 &&
            missingDevice.output.contains("requires a device id"),
        "staged device runtime reports its missing-id boundary without hardware access");

    QTemporaryDir root;
    expect(root.isValid(), "CLI fixture creates a build-local temporary root");
    if (!root.isValid()) return;
    const QString recording = QDir(root.path()).absoluteFilePath(
        QStringLiteral("recording-backslash-contract"));
    const ProcessResult local = runProcess(executable, {
        QStringLiteral("local"),
        QStringLiteral("--headless-audio"), QStringLiteral("on"),
        QStringLiteral("--stream-ms"), QStringLiteral("150"),
        QStringLiteral("--test-input"), QStringLiteral("tone-440"),
        QStringLiteral("--record-jam-folder"), recording,
    });
    const QString sidecarPath = QDir(recording).absoluteFilePath(
        QStringLiteral("recording.json"));
    QFile sidecar(sidecarPath);
    const bool opened = sidecar.open(QIODevice::ReadOnly);
    QJsonParseError parseError;
    const QJsonDocument document = opened
        ? QJsonDocument::fromJson(sidecar.readAll(), &parseError)
        : QJsonDocument{};
    const QJsonObject object = document.object();
    expect(local.started && local.finished &&
            local.status == QProcess::NormalExit && local.code == 0 &&
            local.output.contains("\"mode\":\"local\"") &&
            opened && parseError.error == QJsonParseError::NoError &&
            object.value(QStringLiteral("format")).toString() ==
                QStringLiteral("pcm16_mono_wav") &&
            object.value(QStringLiteral("test_input")).toString() ==
                QStringLiteral("tone-440") &&
            object.value(QStringLiteral("frames_written")).toInteger() > 0,
        "staged headless local runtime records audio and writes a valid escaped sidecar");
    if (failures != 0) {
        root.setAutoRemove(false);
        std::cerr << "CLI artifacts retained at "
                  << root.path().toStdString() << '\n'
                  << local.output.toStdString();
    }
}

} // namespace

namespace jam2::cli {

int runTestDevice(int, char**)
{
    ++testDeviceDispatches;
    return 17;
}

int runLocal(int, char**)
{
    ++localDispatches;
    return 18;
}

} // namespace jam2::cli

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
#if defined(_WIN32)
    if (argc == 3 && std::string_view(argv[1]) == "--console-parent") {
        return testParentConsole(QString::fromLocal8Bit(argv[2]));
    }
#endif
    if (argc != 2) {
        std::cerr << "usage: jam2_cli_boundary_tests <release-jam2>\n";
        return 2;
    }

    try {
        testFrontendAndHelpContracts();
        testOptionAndStatsContracts();
        testStagedLocalRuntime(QString::fromLocal8Bit(argv[1]));
#if defined(_WIN32)
        testWindowsStartup(QString::fromLocal8Bit(argv[1]));
#endif
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: unexpected CLI-boundary exception: "
                  << exception.what() << '\n';
    }

    if (failures != 0) {
        std::cerr << failures << " CLI boundary checks failed\n";
        return 1;
    }
    std::cout << "CLI boundary checks passed\n";
    return 0;
}
