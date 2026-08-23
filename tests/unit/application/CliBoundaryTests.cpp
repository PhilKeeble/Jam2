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
            log.write("final", 25, copied, statsOptions, audio);
        }
        QFile statsCsv(QString::fromStdString(statsCsvPath.string()));
        const bool opened = statsCsv.open(QIODevice::ReadOnly);
        const QList<QByteArray> lines = opened
            ? statsCsv.readAll().trimmed().split('\n')
            : QList<QByteArray>{};
        const QByteArray header = lines.size() == 2 ? lines[0].trimmed() : QByteArray{};
        const QByteArray finalRow = lines.size() == 2 ? lines[1].trimmed() : QByteArray{};
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
        const bool schemaMatches = opened && lines.size() == 2 &&
                csvFieldCount(header) == csvFieldCount(finalRow) &&
                header.endsWith("coreaudio_cycle_jitter_samples") &&
                finalRow.endsWith(",9");
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

    expect(jam2::cli::os_error_text(0).empty() &&
            jam2::cli::os_error_text(5) == "error 5",
        "CLI platform diagnostics preserve empty-success and numeric-error text");

    jam2::Engine engine;
    jam2::cli::CliPeerStreamPlayback playback(&engine);
    const std::array<std::int32_t, 2> frames{1, 2};
    expect(playback.acceptsFrames() && playback.depthFrames() == 0 &&
            playback.pushFrames(frames) == 0,
        "CLI playback adapter delegates safely to an unstarted engine");
    playback.requestDropFrames(1);
    playback.setResamplerRatio(1.0001);
    playback.detach();
    playback.requestDropFrames(1);
    playback.setResamplerRatio(1.0);
    expect(!playback.acceptsFrames() && playback.pushFrames(frames) == 0 &&
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
    if (argc != 2) {
        std::cerr << "usage: jam2_cli_boundary_tests <release-jam2>\n";
        return 2;
    }

    try {
        testFrontendAndHelpContracts();
        testOptionAndStatsContracts();
        testStagedLocalRuntime(QString::fromLocal8Bit(argv[1]));
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
