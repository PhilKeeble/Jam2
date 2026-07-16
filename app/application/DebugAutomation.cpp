#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DebugAutomation.hpp"
#include "BoundaryValidation.hpp"
#include "runtime_limits.hpp"

#include "ApplicationRuntime.hpp"
#include "AutomationChannel.hpp"
#include "AssetChunkProtocol.hpp"
#include "ControlProtocol.hpp"
#include "ControlMessageValidation.hpp"
#include "ControllerLifecycleValidation.hpp"
#include "DebugActionValidation.hpp"
#include "SessionController.hpp"
#include "CliEntrypoint.hpp"
#include "CliOptions.hpp"
#include "tuning_profile.hpp"
#include "pcm16_wav.hpp"
#include "protocol.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace {

constexpr qsizetype kMaxScenarioBytes = 256 * 1024;
constexpr qsizetype kMaxStringBytes = 4096;
constexpr qsizetype kMaxRunIdBytes = 128;
constexpr qsizetype kMaxActions = 128;
constexpr qsizetype kMaxFixtures = 128;
constexpr qsizetype kMaxArtifactFiles = 256;
constexpr qint64 kMaxArtifactBytes = 1024LL * 1024LL * 1024LL;
constexpr qint64 kMaxFuzzInputBytes = 1024LL * 1024LL;
constexpr auto kScenarioFormat = "jam2-debug-scenario";
constexpr auto kDescriptionFormat = "jam2-debug-description";
constexpr auto kAutomationFormat = "jam2-automation";
constexpr auto kScenarioProperty = "jam2.debug.scenario";

std::optional<std::uint64_t> processCpuTimeNanoseconds() noexcept
{
#if defined(_WIN32)
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return std::nullopt;
    }
    ULARGE_INTEGER kernelTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTicks{};
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return (kernelTicks.QuadPart + userTicks.QuadPart) * 100ULL;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::nullopt;
    }
    const auto timevalNanoseconds = [](const timeval& value) {
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(value.tv_usec) * 1000ULL;
    };
    return timevalNanoseconds(usage.ru_utime) + timevalNanoseconds(usage.ru_stime);
#endif
}

constexpr std::string_view kDebugUsage = R"(Usage:
  jam2 debug describe --json
  jam2 debug run <scenario.json>
  jam2 debug fuzz <control|udp-pcm16|udp-pcm24|asset|wav> <input-file>

Subcommands:
  describe    Emit the supported unversioned automation formats, operations, profiles, fields, and bounds
  run         Validate and execute a bounded declarative scenario
  fuzz        Execute one bounded local native parser input (test-only)

Run `jam2 debug <subcommand> -h` for details.
)";

enum class FieldKind {
    Boolean,
    Integer,
    Number,
    String,
};

struct RuntimeField {
    const char* key;
    const char* option;
    FieldKind kind;
    double minimum;
    double maximum;
    const char* choices;
};

constexpr std::array kRuntimeFields{
    RuntimeField{"audio_device", "--audio-device", FieldKind::Integer, 0, 65535, ""},
    RuntimeField{"headless_audio", "--headless-audio", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"headless_clock_drift_ppm", "--headless-clock-drift-ppm", FieldKind::Integer, -5000, 5000, ""},
    RuntimeField{
        "sample_rate",
        "--sample-rate",
        FieldKind::Integer,
        jam2::limits::kMinimumSampleRate,
        jam2::limits::kMaximumSampleRate,
        ""},
    RuntimeField{"audio_buffer_size", "--audio-buffer-size", FieldKind::Integer, 1, 1048576, ""},
    RuntimeField{"frame_size", "--frame-size", FieldKind::Integer, 32, 256, "32|64|128|256"},
    RuntimeField{"network_audio_format", "--network-audio-format", FieldKind::String, 0, 0, "pcm16|pcm24"},
    RuntimeField{"capture_ring_frames", "--capture-ring-frames", FieldKind::Integer, 1, 1073741824, ""},
    RuntimeField{"playback_ring_frames", "--playback-ring-frames", FieldKind::Integer, 1, 1073741824, ""},
    RuntimeField{"playback_prefill_frames", "--playback-prefill-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"playback_max_frames", "--playback-max-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"drift_correction", "--drift-correction", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"drift_smoothing", "--drift-smoothing", FieldKind::Number, 0, 1, ""},
    RuntimeField{"drift_deadband_ppm", "--drift-deadband-ppm", FieldKind::Integer, 0, 50000, ""},
    RuntimeField{"drift_max_correction_ppm", "--drift-max-correction-ppm", FieldKind::Integer, 0, 50000, ""},
    RuntimeField{"sample_time_playout", "--sample-time-playout", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"playout_delay_frames", "--playout-delay-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"jitter_buffer_frames", "--jitter-buffer-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"jitter_buffer_max_frames", "--jitter-buffer-max-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"adaptive_playback_cushion", "--adaptive-playback-cushion", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"adaptive_playback_target_frames", "--adaptive-playback-target-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"adaptive_playback_min_frames", "--adaptive-playback-min-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"adaptive_playback_max_frames", "--adaptive-playback-max-frames", FieldKind::Integer, 0, 1073741824, ""},
    RuntimeField{"adaptive_playback_release_ppm", "--adaptive-playback-release-ppm", FieldKind::Integer, 0, 1000000, ""},
    RuntimeField{"adaptive_playback_ratio_ramp_ms", "--adaptive-playback-ratio-ramp-ms", FieldKind::Integer, 0, 60000, ""},
    RuntimeField{"metronome", "--metronome", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"bpm", "--bpm", FieldKind::Integer, 1, 400, ""},
    RuntimeField{"metronome_level", "--metronome-level", FieldKind::Number, 0, 4, ""},
    RuntimeField{"metronome_mode", "--metronome-mode", FieldKind::String, 0, 0, "shared-grid|leader-audio|listener-compensated"},
    RuntimeField{"metronome_compensation_max_ms", "--metronome-compensation-max-ms", FieldKind::Number, 0, 1000, ""},
    RuntimeField{"metronome_compensation_smoothing_ms", "--metronome-compensation-smoothing-ms", FieldKind::Number, 0, 10000, ""},
    RuntimeField{"metronome_compensation_deadband_ms", "--metronome-compensation-deadband-ms", FieldKind::Number, 0, 1000, ""},
    RuntimeField{"metronome_compensation_slew_ms_per_sec", "--metronome-compensation-slew-ms-per-sec", FieldKind::Number, 0, 10000, ""},
    RuntimeField{"remote_level", "--remote-level", FieldKind::Number, 0, 4, ""},
    RuntimeField{"send_level", "--send-level", FieldKind::Number, 0, 4, ""},
    RuntimeField{"local_monitor", "--local-monitor", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"local_monitor_level", "--local-monitor-level", FieldKind::Number, 0, 4, ""},
    RuntimeField{"test_input", "--test-input", FieldKind::String, 0, 0, "off|silence|tone-440|pulse-1s|metro-pulse"},
    RuntimeField{"os_priority", "--os-priority", FieldKind::String, 0, 0, "off|high|realtime"},
    RuntimeField{"input_channels", "--input-channels", FieldKind::String, 0, 0, "channel-list"},
    RuntimeField{"output_channels", "--output-channels", FieldKind::String, 0, 0, "channel-list"},
    RuntimeField{"socket_send_buffer", "--socket-send-buffer", FieldKind::Integer, 1, 1073741824, ""},
    RuntimeField{"socket_recv_buffer", "--socket-recv-buffer", FieldKind::Integer, 1, 1073741824, ""},
    RuntimeField{"stats", "--stats", FieldKind::Boolean, 0, 1, ""},
    RuntimeField{"stats_interval_ms", "--stats-interval-ms", FieldKind::Integer, 0, 2147483647, ""},
    RuntimeField{"stats_warmup_ms", "--stats-warmup-ms", FieldKind::Integer, 0, 2147483647, ""},
    RuntimeField{"stream_ms", "--stream-ms", FieldKind::Integer, 0, 2147483647, ""},
    RuntimeField{"stream_linger_ms", "--stream-linger-ms", FieldKind::Integer, 0, 2147483647, ""},
};

struct ParsedScenario {
    QJsonObject source;
    QString path;
    QString runId;
    QString operation;
    QString artifactRoot;
    QString manifestPath;
    QStringList optionArguments;
    Jam2RuntimeOptions effectiveOptions;
    QStringList fixtures;
    bool reactive = false;
};

bool isHelpArgument(std::string_view argument) noexcept
{
    return argument == "-h" || argument == "--help" || argument == "help";
}

bool hasHelpArgument(int argc, char* argv[], int start) noexcept
{
    for (int index = start; index < argc; ++index) {
        if (isHelpArgument(argv[index])) {
            return true;
        }
    }
    return false;
}

void printDescribeHelp()
{
    std::cout << R"(Usage:
  jam2 debug describe --json

Writes one JSON object describing the unversioned local automation formats,
supported operations/actions, native profiles and effective defaults, numeric
limits, capacity limits, and inherited reactive-handle variable names.
)";
}

void printRunHelp()
{
    std::cout << R"(Usage:
  jam2 debug run <scenario.json>

The file must use schema `jam2-debug-scenario`. The temporary
`jam2-debug-scenario-v1` argv-wrapper format is not accepted. Run
`jam2 debug describe --json` for the bounded fields, actions, and limits.
)";
}

void printFuzzHelp()
{
    std::cout << R"(Usage:
  jam2 debug fuzz <control|udp-pcm16|udp-pcm24|asset|wav> <input-file>

Runs one bounded local parser input and emits a JSON accepted/rejected result.
This opt-in operation opens no listener and is intended for jam2_test.py fuzz.
)";
}

int runFuzzInput(const QString& target, const QString& inputPath)
{
    QFile file(inputPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > kMaxFuzzInputBytes) {
        throw std::runtime_error("fuzz input is unreadable or exceeds 1048576 bytes");
    }
    const QByteArray input = file.readAll();
    bool accepted = false;
    QString detail;

    if (target == QStringLiteral("control")) {
        QByteArray buffer = input;
        quint64 sequence = 7;
        accepted = !buffer.isEmpty();
        while (accepted && !buffer.isEmpty()) {
            QByteArray body;
            const auto framed = jam2::control_protocol::takeFrame(buffer, body, detail);
            if (framed != jam2::control_protocol::TakeFrameResult::Ready) {
                if (framed == jam2::control_protocol::TakeFrameResult::NeedMore) {
                    detail = QStringLiteral("incomplete control frame");
                }
                accepted = false;
                break;
            }
            jam2::control_protocol::AuthenticatedPayload payload;
            accepted = jam2::control_protocol::decodeAuthenticated(
                body, QByteArray(32, 'k'), sequence++, payload, detail);
            if (accepted && payload.type == jam2::control_protocol::AuthenticatedPayloadType::Json) {
                QString validation;
                accepted = jam2::application::validateControlMessage(payload.message, validation);
                if (!accepted) detail = validation;
            } else if (accepted) {
                jam2::application::asset_chunk::Chunk chunk;
                accepted = jam2::application::asset_chunk::decode(payload.binary, chunk, detail);
            }
        }
    } else if (target == QStringLiteral("udp-pcm16") ||
               target == QStringLiteral("udp-pcm24")) {
        std::array<std::uint8_t, 16> key{};
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
        const auto format = target == QStringLiteral("udp-pcm16")
            ? jam2::NetworkAudioFormat::Pcm16Mono
            : jam2::NetworkAudioFormat::Pcm24Mono;
        const auto bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(input.constData()),
            static_cast<std::size_t>(input.size()));
        const auto parsed = jam2::protocol::parse_packet(
            bytes, key, 0x0102030405060708ULL, format);
        accepted = static_cast<bool>(parsed);
        if (accepted) {
            jam2::protocol::ReplayWindow replay;
            if (replay.observe(parsed.header.sequence) != jam2::protocol::ReplayResult::New ||
                replay.observe(parsed.header.sequence) != jam2::protocol::ReplayResult::Duplicate) {
                throw std::runtime_error("UDP replay invariant failed");
            }
            if (parsed.header.type == jam2::protocol::PacketType::Audio) {
                const std::size_t frames = parsed.header.payload_length /
                    jam2::protocol::audio_bytes_per_sample(format);
                std::vector<std::int32_t> samples(frames);
                if (!jam2::protocol::unpack_audio_into(
                        format, bytes.subspan(jam2::protocol::kHeaderSize), samples)) {
                    throw std::runtime_error("accepted UDP audio failed codec invariant");
                }
            }
        } else {
            detail = QString::fromLatin1(jam2::protocol::parse_error_text(parsed.error));
        }
    } else if (target == QStringLiteral("asset")) {
        jam2::application::asset_chunk::Chunk chunk;
        accepted = jam2::application::asset_chunk::decode(input, chunk, detail);
    } else if (target == QStringLiteral("wav")) {
#if defined(_WIN32)
        const std::filesystem::path nativePath(inputPath.toStdWString());
#else
        const std::filesystem::path nativePath(inputPath.toUtf8().constData());
#endif
        const auto inspected = jam2::wav::inspect_pcm16_file(
            nativePath,
            static_cast<std::uint64_t>(kMaxFuzzInputBytes));
        accepted = static_cast<bool>(inspected);
        if (!accepted) detail = QString::fromStdString(inspected.error);
    } else {
        throw std::runtime_error("unknown native fuzz target");
    }

    std::cout << QJsonDocument(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("fuzz_result")},
        {QStringLiteral("target"), target},
        {QStringLiteral("accepted"), accepted},
        {QStringLiteral("classification"), accepted ? QStringLiteral("accepted") : QStringLiteral("rejected")},
        {QStringLiteral("detail"), detail.left(512)},
        {QStringLiteral("input_bytes"), input.size()},
        {QStringLiteral("control_protocol_version"), jam2::control_protocol::kControlProtocolVersion},
        {QStringLiteral("udp_protocol_version"), jam2::protocol::kProtocolVersion},
    }).toJson(QJsonDocument::Compact).constData() << '\n';
    return 0;
}

QJsonObject profileJson(const jam2::CreateProfile& profile)
{
    const jam2::JoinProfile& local = *profile.local;
    return {
        {QStringLiteral("name"), QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size()))},
        {QStringLiteral("label"), QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size()))},
        {QStringLiteral("sample_rate"), profile.sample_rate},
        {QStringLiteral("audio_buffer_size"), static_cast<qint64>(local.audio_buffer_size)},
        {QStringLiteral("frame_size"), profile.frame_size},
        {QStringLiteral("playback_prefill_frames"), static_cast<qint64>(local.playback_prefill_frames)},
        {QStringLiteral("playback_ring_frames"), static_cast<qint64>(local.playback_ring_frames)},
        {QStringLiteral("playback_max_frames"), static_cast<qint64>(local.playback_max_frames)},
        {QStringLiteral("capture_ring_frames"), static_cast<qint64>(local.capture_ring_frames)},
        {QStringLiteral("drift_correction"), local.drift_correction},
        {QStringLiteral("drift_smoothing"), local.drift_smoothing},
        {QStringLiteral("drift_deadband_ppm"), local.drift_deadband_ppm},
        {QStringLiteral("drift_max_correction_ppm"), local.drift_max_correction_ppm},
        {QStringLiteral("sample_time_playout"), local.sample_time_playout},
        {QStringLiteral("playout_delay_frames"), static_cast<qint64>(local.playout_delay_frames)},
        {QStringLiteral("jitter_buffer_frames"), static_cast<qint64>(local.jitter_buffer_frames)},
        {QStringLiteral("jitter_buffer_max_frames"), static_cast<qint64>(local.jitter_buffer_max_frames)},
        {QStringLiteral("adaptive_playback_cushion"), local.adaptive_playback_cushion},
        {QStringLiteral("adaptive_playback_target_frames"), static_cast<qint64>(local.adaptive_playback_target_frames)},
        {QStringLiteral("adaptive_playback_min_frames"), static_cast<qint64>(local.adaptive_playback_min_frames)},
        {QStringLiteral("adaptive_playback_max_frames"), static_cast<qint64>(local.adaptive_playback_max_frames)},
        {QStringLiteral("adaptive_playback_release_ppm"), local.adaptive_playback_release_ppm},
        {QStringLiteral("adaptive_playback_ratio_ramp_ms"), local.adaptive_playback_ratio_ramp_ms},
    };
}

QString fieldKindText(FieldKind kind)
{
    switch (kind) {
    case FieldKind::Boolean: return QStringLiteral("boolean");
    case FieldKind::Integer: return QStringLiteral("integer");
    case FieldKind::Number: return QStringLiteral("number");
    case FieldKind::String: return QStringLiteral("string");
    }
    return QStringLiteral("unknown");
}

QJsonObject descriptionJson()
{
    QJsonArray profiles;
    for (const auto& profile : jam2::create_profiles()) {
        profiles.push_back(profileJson(profile));
    }
    QJsonArray fields;
    for (const auto& field : kRuntimeFields) {
        QJsonObject item{
            {QStringLiteral("name"), QString::fromLatin1(field.key)},
            {QStringLiteral("type"), fieldKindText(field.kind)},
        };
        if (field.kind == FieldKind::Integer || field.kind == FieldKind::Number) {
            item.insert(QStringLiteral("minimum"), field.minimum);
            item.insert(QStringLiteral("maximum"), field.maximum);
        }
        if (field.choices[0] != '\0') {
            QJsonArray choices;
            for (const QString& choice : QString::fromLatin1(field.choices).split(QLatin1Char('|'))) {
                choices.push_back(choice);
            }
            item.insert(QStringLiteral("choices"), choices);
        }
        fields.push_back(item);
    }
    return {
        {QStringLiteral("schema"), QString::fromLatin1(kDescriptionFormat)},
        {QStringLiteral("scenario_schema"), QString::fromLatin1(kScenarioFormat)},
        {QStringLiteral("automation_protocol"), QString::fromLatin1(kAutomationFormat)},
        {QStringLiteral("control_protocol_version"), jam2::control_protocol::kControlProtocolVersion},
        {QStringLiteral("udp_protocol_version"), jam2::protocol::kProtocolVersion},
        {QStringLiteral("max_scenario_bytes"), kMaxScenarioBytes},
        {QStringLiteral("max_string_bytes"), kMaxStringBytes},
        {QStringLiteral("max_actions"), kMaxActions},
        {QStringLiteral("max_automation_frame_bytes"), static_cast<qint64>(AutomationChannel::kMaxFrameBytes)},
        {QStringLiteral("automation_queue_capacity"), static_cast<qint64>(AutomationChannel::kQueueCapacity)},
        {QStringLiteral("automation_commands_per_turn"), static_cast<qint64>(AutomationChannel::kCommandsPerTurn)},
        {QStringLiteral("automation_incomplete_frame_timeout_ms"), AutomationChannel::kIncompleteFrameTimeoutMs},
        {QStringLiteral("max_pending_unauthenticated_peers"), jam2::control_protocol::kMaxPendingPeers},
        {QStringLiteral("authentication_failure_window_ms"), jam2::control_protocol::kAuthenticationFailureWindowMs},
        {QStringLiteral("max_authentication_failures_per_window"), jam2::control_protocol::kMaxAuthenticationFailuresPerWindow},
        {QStringLiteral("fuzz_targets"), QJsonArray{
            QStringLiteral("control"), QStringLiteral("udp-pcm16"),
            QStringLiteral("udp-pcm24"), QStringLiteral("asset"), QStringLiteral("wav")}},
        {QStringLiteral("max_fuzz_input_bytes"), kMaxFuzzInputBytes},
        {QStringLiteral("operations"), QJsonArray{
            QStringLiteral("local"),
            QStringLiteral("lifecycle.local-network-local"),
            QStringLiteral("validate.boundaries"),
            QStringLiteral("validate.controller-lifecycle"),
            QStringLiteral("network.create"),
            QStringLiteral("network.join")}},
        {QStringLiteral("actions"), QJsonArray{
            QStringLiteral("metronome.enabled"), QStringLiteral("metronome.bpm"),
            QStringLiteral("metronome.mode"), QStringLiteral("metronome.level"),
            QStringLiteral("remote.level"), QStringLiteral("track.sync"),
            QStringLiteral("track.load"), QStringLiteral("track.play"),
            QStringLiteral("track.stop"), QStringLiteral("track.restart"),
            QStringLiteral("track.record-start"), QStringLiteral("recording.start"),
            QStringLiteral("recording.stop"), QStringLiteral("snapshot"),
            QStringLiteral("shutdown")}},
        {QStringLiteral("runtime_fields"), fields},
        {QStringLiteral("profiles"), profiles},
        {QStringLiteral("default_profile"), QString::fromUtf8(jam2::default_create_profile().name.data(), static_cast<qsizetype>(jam2::default_create_profile().name.size()))},
        {QStringLiteral("test_inputs"), QJsonArray{
            QStringLiteral("off"), QStringLiteral("silence"), QStringLiteral("tone-440"),
            QStringLiteral("pulse-1s"), QStringLiteral("metro-pulse")}},
        {QStringLiteral("reactive_handle_environment"), QJsonArray{
            QStringLiteral("JAM2_AUTOMATION_COMMAND_HANDLE"),
            QStringLiteral("JAM2_AUTOMATION_EVENT_HANDLE")}},
    };
}

const RuntimeField* findRuntimeField(const QString& key)
{
    for (const auto& field : kRuntimeFields) {
        if (key == QString::fromLatin1(field.key)) {
            return &field;
        }
    }
    return nullptr;
}

QString jsonScalarText(const RuntimeField& field, const QJsonValue& value)
{
    if (field.kind == FieldKind::Boolean) {
        if (!value.isBool()) {
            throw std::runtime_error((std::string(field.key) + " must be a boolean").c_str());
        }
        return value.toBool() ? QStringLiteral("on") : QStringLiteral("off");
    }
    if (field.kind == FieldKind::Integer) {
        if (!value.isDouble()) {
            throw std::runtime_error((std::string(field.key) + " must be an integer").c_str());
        }
        const double number = value.toDouble();
        const qint64 integer = static_cast<qint64>(number);
        if (number != static_cast<double>(integer) || number < field.minimum || number > field.maximum) {
            throw std::runtime_error((std::string(field.key) + " is outside its integer bounds").c_str());
        }
        return QString::number(integer);
    }
    if (field.kind == FieldKind::Number) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
            value.toDouble() < field.minimum || value.toDouble() > field.maximum) {
            throw std::runtime_error((std::string(field.key) + " is outside its numeric bounds").c_str());
        }
        return QString::number(value.toDouble(), 'g', 17);
    }
    if (!value.isString() || value.toString().isEmpty() ||
        value.toString().toUtf8().size() > kMaxStringBytes) {
        throw std::runtime_error((std::string(field.key) + " must be a bounded non-empty string").c_str());
    }
    if (field.choices[0] != '\0' && std::string_view(field.choices) != "channel-list" &&
        !QString::fromLatin1(field.choices).split(QLatin1Char('|')).contains(value.toString())) {
        throw std::runtime_error((std::string(field.key) + " is not a supported choice").c_str());
    }
    return value.toString();
}

void appendRuntimeOptions(const QJsonObject& runtime, QStringList& arguments)
{
    for (auto it = runtime.begin(); it != runtime.end(); ++it) {
        const RuntimeField* field = findRuntimeField(it.key());
        if (field == nullptr) {
            throw std::runtime_error((QStringLiteral("unknown debug runtime field: ") + it.key()).toStdString());
        }
        arguments.push_back(QString::fromLatin1(field->option));
        arguments.push_back(jsonScalarText(*field, it.value()));
    }
}

void appendBoundedStringOption(
    const QJsonObject& object,
    const QString& key,
    const QString& option,
    QStringList& arguments)
{
    if (!object.contains(key)) {
        return;
    }
    const QString value = object.value(key).toString();
    if (value.isEmpty() || value.toUtf8().size() > kMaxStringBytes) {
        throw std::runtime_error((key + QStringLiteral(" must be a bounded non-empty string")).toStdString());
    }
    arguments << option << value;
}

std::vector<std::string> toStorage(const QStringList& arguments, const char* executable)
{
    std::vector<std::string> storage;
    storage.reserve(static_cast<std::size_t>(arguments.size() + 1));
    storage.emplace_back(executable);
    for (const QString& value : arguments) {
        storage.push_back(value.toUtf8().toStdString());
    }
    return storage;
}

Jam2RuntimeOptions parseRuntimeArguments(
    const QStringList& arguments,
    const char* executable,
    Jam2ProfileApplication profileApplication)
{
    auto storage = toStorage(arguments, executable);
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& value : storage) {
        argv.push_back(value.data());
    }
    return jam2_parse_runtime_options(
        static_cast<int>(argv.size()), argv.data(), 1, profileApplication);
}

QString canonicalArtifactRoot(const QString& scenarioPath, const QJsonObject& artifacts)
{
    const QString rootText = artifacts.value(QStringLiteral("root")).toString();
    if (rootText.isEmpty() || rootText.toUtf8().size() > kMaxStringBytes) {
        throw std::runtime_error("debug scenario artifacts.root must be a bounded non-empty path");
    }
    QFileInfo info(rootText);
    const QString absolute = info.isAbsolute()
        ? info.absoluteFilePath()
        : QFileInfo(QFileInfo(scenarioPath).absoluteDir(), rootText).absoluteFilePath();
    if (!QDir().mkpath(absolute)) {
        throw std::runtime_error("debug scenario artifact root could not be created");
    }
    const QString canonical = QFileInfo(absolute).canonicalFilePath();
    if (canonical.isEmpty() || canonical.toUtf8().size() > kMaxStringBytes) {
        throw std::runtime_error("debug scenario artifact root could not be resolved");
    }
    return canonical;
}

void validateActions(const QJsonArray& actions)
{
    if (actions.size() > kMaxActions) {
        throw std::runtime_error("debug scenario has too many actions");
    }
    for (const QJsonValue& value : actions) {
        if (!value.isObject()) {
            throw std::runtime_error("debug scenario actions must contain objects");
        }
        const QJsonObject action = value.toObject();
        QString error;
        if (!jam2ValidateDebugAction(action, error)) {
            throw std::runtime_error(
                (QStringLiteral("invalid debug scenario action: ") + error).toStdString());
        }
    }
}

ParsedScenario parseScenario(const QString& path, const QJsonObject& source, const char* executable)
{
    const std::set<QString> allowed{
        QStringLiteral("schema"), QStringLiteral("run_id"), QStringLiteral("operation"),
        QStringLiteral("profile"), QStringLiteral("runtime"), QStringLiteral("network"),
        QStringLiteral("artifacts"), QStringLiteral("actions"),
        QStringLiteral("automation"), QStringLiteral("fixtures")};
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (!allowed.contains(it.key())) {
            throw std::runtime_error("debug scenario contains an unknown top-level field");
        }
    }
    for (const auto& field : std::array{
             std::pair{QStringLiteral("runtime"), QJsonValue::Object},
             std::pair{QStringLiteral("network"), QJsonValue::Object},
             std::pair{QStringLiteral("artifacts"), QJsonValue::Object},
             std::pair{QStringLiteral("automation"), QJsonValue::Object},
             std::pair{QStringLiteral("actions"), QJsonValue::Array},
             std::pair{QStringLiteral("fixtures"), QJsonValue::Array}}) {
        if (source.contains(field.first) && source.value(field.first).type() != field.second) {
            throw std::runtime_error(
                (QStringLiteral("debug scenario field has the wrong container type: ") +
                 field.first).toStdString());
        }
    }
    if (source.value(QStringLiteral("schema")).toString() != QString::fromLatin1(kScenarioFormat)) {
        throw std::runtime_error("unsupported debug scenario schema");
    }
    ParsedScenario result;
    result.source = source;
    result.path = path;
    result.runId = source.value(QStringLiteral("run_id")).toString();
    result.operation = source.value(QStringLiteral("operation")).toString();
    if (result.runId.isEmpty() || result.runId.toUtf8().size() > kMaxRunIdBytes) {
        throw std::runtime_error("debug scenario run_id must be 1..128 UTF-8 bytes");
    }
    const QJsonArray operations = descriptionJson().value(QStringLiteral("operations")).toArray();
    if (!operations.contains(result.operation)) {
        throw std::runtime_error("unsupported debug scenario operation");
    }
    const QJsonObject artifacts = source.value(QStringLiteral("artifacts")).toObject();
    if (artifacts.isEmpty()) {
        throw std::runtime_error("debug scenario requires an artifacts object");
    }
    for (auto it = artifacts.begin(); it != artifacts.end(); ++it) {
        if (it.key() != QStringLiteral("root")) {
            throw std::runtime_error("debug scenario artifacts contains an unknown field");
        }
    }
    result.artifactRoot = canonicalArtifactRoot(path, artifacts);
    result.manifestPath = QDir(result.artifactRoot).filePath(QStringLiteral("native-manifest.json"));

    const QString profile = source.value(QStringLiteral("profile")).toString(QStringLiteral("fast"));
    const QByteArray profileBytes = profile.toUtf8();
    if (jam2::find_join_profile(std::string_view(
            profileBytes.constData(), static_cast<std::size_t>(profileBytes.size()))) == nullptr) {
        throw std::runtime_error("debug scenario profile is not supported");
    }
    result.optionArguments << QStringLiteral("--profile") << profile;
    const QJsonValue runtimeValue = source.value(QStringLiteral("runtime"));
    if (!runtimeValue.isUndefined() && !runtimeValue.isObject()) {
        throw std::runtime_error("debug scenario runtime must be an object");
    }
    appendRuntimeOptions(runtimeValue.toObject(), result.optionArguments);

    const bool runtimeHasStats = runtimeValue.toObject().contains(QStringLiteral("stats"));
    const bool runtimeHasStatsPath = runtimeValue.toObject().contains(QStringLiteral("log_stats"));
    (void)runtimeHasStatsPath;
    if (!runtimeHasStats) {
        result.optionArguments << QStringLiteral("--stats") << QStringLiteral("enabled");
    }
    result.optionArguments << QStringLiteral("--log-stats")
                           << QDir(result.artifactRoot).filePath(QStringLiteral("csv"));
    // Validate the native runtime/profile layer before adding structured TCP
    // bootstrap fields. SessionController owns --wait-ms, --max-peers and
    // explicit session material; they are intentionally not CLI runtime
    // options and must not be fed through the audio option parser.
    result.effectiveOptions = parseRuntimeArguments(
        result.optionArguments,
        executable,
        result.operation == QStringLiteral("network.join")
            ? Jam2ProfileApplication::Join
            : Jam2ProfileApplication::Create);

    const QJsonObject network = source.value(QStringLiteral("network")).toObject();
    static const std::set<QString> networkFields{
        QStringLiteral("bind"), QStringLiteral("join_url"), QStringLiteral("no_stun"),
        QStringLiteral("stun"), QStringLiteral("stun_timeout_ms"),
        QStringLiteral("stun_retries"), QStringLiteral("wait_ms"),
        QStringLiteral("public_endpoint"), QStringLiteral("session_id"),
        QStringLiteral("session_key"), QStringLiteral("max_peers"),
        QStringLiteral("peer_token"), QStringLiteral("topology"),
        QStringLiteral("heartbeat_interval_ms"), QStringLiteral("heartbeat_miss_limit")};
    for (auto it = network.begin(); it != network.end(); ++it) {
        if (!networkFields.contains(it.key())) {
            throw std::runtime_error("debug scenario network contains an unknown field");
        }
    }
    if (network.contains(QStringLiteral("topology")) &&
        !network.value(QStringLiteral("topology")).isObject()) {
        throw std::runtime_error("debug topology must be an object");
    }
    const QJsonObject topology = network.value(QStringLiteral("topology")).toObject();
    if (topology.size() > 32) {
        throw std::runtime_error("debug topology contains too many observers");
    }
    for (auto observer = topology.begin(); observer != topology.end(); ++observer) {
        if (!observer.value().isObject() || observer.key().toUtf8().size() > kMaxRunIdBytes ||
            observer.value().toObject().size() > 32) {
            throw std::runtime_error("debug topology observer map is invalid");
        }
        const QJsonObject peers = observer.value().toObject();
        for (auto peer = peers.begin(); peer != peers.end(); ++peer) {
            if (!peer.value().isString() || peer.value().toString().isEmpty() ||
                peer.value().toString().toUtf8().size() > kMaxStringBytes) {
                throw std::runtime_error("debug topology endpoint override is invalid");
            }
        }
    }
    appendBoundedStringOption(network, QStringLiteral("bind"), QStringLiteral("--bind"), result.optionArguments);
    appendBoundedStringOption(network, QStringLiteral("stun"), QStringLiteral("--stun"), result.optionArguments);
    appendBoundedStringOption(network, QStringLiteral("public_endpoint"), QStringLiteral("--public-endpoint"), result.optionArguments);
    appendBoundedStringOption(network, QStringLiteral("session_id"), QStringLiteral("--session-id"), result.optionArguments);
    appendBoundedStringOption(network, QStringLiteral("session_key"), QStringLiteral("--session-key"), result.optionArguments);
    for (const QString& field : {QStringLiteral("join_url"), QStringLiteral("peer_token")}) {
        if (network.contains(field)) {
            const QJsonValue value = network.value(field);
            if (!value.isString() || value.toString().isEmpty() ||
                value.toString().toUtf8().size() > kMaxStringBytes) {
                throw std::runtime_error(
                    (QStringLiteral("debug scenario network string is invalid: ") + field).toStdString());
            }
        }
    }
    for (const auto& item : std::array{
             std::pair{QStringLiteral("stun_timeout_ms"), QStringLiteral("--stun-timeout-ms")},
             std::pair{QStringLiteral("stun_retries"), QStringLiteral("--stun-retries")},
             std::pair{QStringLiteral("wait_ms"), QStringLiteral("--wait-ms")},
             std::pair{QStringLiteral("max_peers"), QStringLiteral("--max-peers")}}) {
        if (!network.contains(item.first)) continue;
        if (!network.value(item.first).isDouble()) {
            throw std::runtime_error("debug scenario network integer has the wrong type");
        }
        const double number = network.value(item.first).toDouble();
        if (number < 0 || number > std::numeric_limits<int>::max() ||
            number != static_cast<double>(static_cast<int>(number))) {
            throw std::runtime_error("debug scenario network integer is invalid");
        }
        result.optionArguments << item.second << QString::number(static_cast<int>(number));
    }
    for (const auto& item : std::array{
             std::pair{QStringLiteral("heartbeat_interval_ms"), std::pair{10, 60000}},
             std::pair{QStringLiteral("heartbeat_miss_limit"), std::pair{1, 20}}}) {
        if (!network.contains(item.first)) continue;
        const QJsonValue value = network.value(item.first);
        const double number = value.toDouble(-1.0);
        if (!value.isDouble() || std::floor(number) != number ||
            number < item.second.first || number > item.second.second) {
            throw std::runtime_error(
                (QStringLiteral("debug scenario heartbeat field is invalid: ") +
                 item.first).toStdString());
        }
    }
    if (network.contains(QStringLiteral("no_stun"))) {
        if (!network.value(QStringLiteral("no_stun")).isBool()) {
            throw std::runtime_error("debug scenario network.no_stun must be boolean");
        }
        if (network.value(QStringLiteral("no_stun")).toBool()) {
            result.optionArguments << QStringLiteral("--no-stun");
        }
    }

    const QJsonArray actions = source.value(QStringLiteral("actions")).toArray();
    if (result.operation == QStringLiteral("local") && !actions.isEmpty()) {
        throw std::runtime_error("retained local debug scenarios do not require runtime actions");
    }
    validateActions(actions);

    const QJsonObject automation = source.value(QStringLiteral("automation")).toObject();
    for (auto it = automation.begin(); it != automation.end(); ++it) {
        if (it.key() != QStringLiteral("reactive") &&
            it.key() != QStringLiteral("controller_loss")) {
            throw std::runtime_error("debug scenario automation contains an unknown field");
        }
    }
    result.reactive = automation.value(QStringLiteral("reactive")).toBool(false);
    if (result.reactive && !result.operation.startsWith(QStringLiteral("network."))) {
        throw std::runtime_error("reactive automation is retained only for network debug runs");
    }
    const QString controllerLoss = automation.value(QStringLiteral("controller_loss")).toString(QStringLiteral("stop"));
    if (controllerLoss != QStringLiteral("stop") && controllerLoss != QStringLiteral("continue")) {
        throw std::runtime_error("automation.controller_loss must be stop or continue");
    }

    const QJsonArray fixtures = source.value(QStringLiteral("fixtures")).toArray();
    if (fixtures.size() > kMaxFixtures) {
        throw std::runtime_error("debug scenario has too many fixtures");
    }
    const bool runtimeOperation = result.operation == QStringLiteral("local") ||
        result.operation.startsWith(QStringLiteral("network."));
    for (const QJsonValue& fixture : fixtures) {
        const QString text = fixture.toString();
        if (text.isEmpty() || text.toUtf8().size() > kMaxStringBytes) {
            throw std::runtime_error("debug scenario fixture path is invalid");
        }
        if (runtimeOperation) {
            const QFileInfo source(text);
            const QString absolute = source.isAbsolute() ? source.absoluteFilePath()
                : QFileInfo(QFileInfo(path).absoluteDir(), text).absoluteFilePath();
            const QString canonical = QFileInfo(absolute).canonicalFilePath();
            if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
                throw std::runtime_error("debug runtime fixture is not a readable file");
            }
            result.fixtures.push_back(canonical);
        } else {
            result.fixtures.push_back(text);
        }
    }
    if (runtimeOperation) {
        const QJsonArray retainedActions = source.value(QStringLiteral("actions")).toArray();
        for (const QJsonValue& value : retainedActions) {
            const QJsonObject action = value.toObject();
            const QString type = action.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("track.load")) {
                const QString canonical = QFileInfo(action.value(QStringLiteral("path")).toString())
                    .canonicalFilePath();
                if (canonical.isEmpty() || !result.fixtures.contains(canonical)) {
                    throw std::runtime_error("track.load path must name a declared local fixture");
                }
            } else if (type == QStringLiteral("recording.start")) {
                const QString output = QFileInfo(action.value(QStringLiteral("path")).toString())
                    .absoluteFilePath();
                const QString relative = QDir(result.artifactRoot).relativeFilePath(output);
                if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
                    QFileInfo(relative).isAbsolute()) {
                    throw std::runtime_error("recording.start path must remain beneath artifacts.root");
                }
            }
        }
    }
    if (network.contains(QStringLiteral("bind"))) {
        result.effectiveOptions.bind = jam2::parse_bind_endpoint(
            network.value(QStringLiteral("bind")).toString().toStdString());
    }
    if (network.contains(QStringLiteral("stun"))) {
        result.effectiveOptions.stun_server = jam2::parse_endpoint(
            network.value(QStringLiteral("stun")).toString().toStdString());
        result.effectiveOptions.no_stun = false;
    }
    if (network.contains(QStringLiteral("public_endpoint"))) {
        result.effectiveOptions.public_endpoint = jam2::parse_endpoint(
            network.value(QStringLiteral("public_endpoint")).toString().toStdString());
    }
    if (network.contains(QStringLiteral("no_stun"))) {
        result.effectiveOptions.no_stun = network.value(QStringLiteral("no_stun")).toBool();
    }
    if (network.contains(QStringLiteral("stun_timeout_ms"))) {
        result.effectiveOptions.stun_timeout_ms = network.value(QStringLiteral("stun_timeout_ms")).toInt();
        if (result.effectiveOptions.stun_timeout_ms <= 0) {
            throw std::runtime_error("debug scenario network.stun_timeout_ms must be positive");
        }
    }
    if (network.contains(QStringLiteral("stun_retries"))) {
        result.effectiveOptions.stun_retries = network.value(QStringLiteral("stun_retries")).toInt();
        if (result.effectiveOptions.stun_retries <= 0) {
            throw std::runtime_error("debug scenario network.stun_retries must be positive");
        }
    }
    if (network.contains(QStringLiteral("wait_ms"))) {
        result.effectiveOptions.wait_ms = network.value(QStringLiteral("wait_ms")).toInt();
    }
    QJsonObject normalizedArtifacts = result.source.value(QStringLiteral("artifacts")).toObject();
    normalizedArtifacts.insert(QStringLiteral("root"), result.artifactRoot);
    result.source.insert(QStringLiteral("artifacts"), normalizedArtifacts);
    QJsonArray normalizedFixtures;
    for (const QString& fixture : result.fixtures) normalizedFixtures.push_back(fixture);
    result.source.insert(QStringLiteral("fixtures"), normalizedFixtures);
    return result;
}

QJsonObject optionsJson(const Jam2RuntimeOptions& options)
{
    const auto endpoint = [](const jam2::Endpoint& value) {
        return QString::fromStdString(jam2::endpoint_to_string(value));
    };
    const auto optionalEndpoint = [&endpoint](const std::optional<jam2::Endpoint>& value) {
        return value ? QJsonValue(endpoint(*value)) : QJsonValue(QJsonValue::Null);
    };
    const auto optionalInteger = [](const auto& value) {
        return value ? QJsonValue(static_cast<qint64>(*value)) : QJsonValue(QJsonValue::Null);
    };
    return {
        {QStringLiteral("profile"), QString::fromStdString(options.profile_name)},
        {QStringLiteral("local_profile"), QString::fromStdString(options.profile_name)},
        {QStringLiteral("session_profile"), QString::fromStdString(options.session_profile_name)},
        {QStringLiteral("sample_rate"), options.sample_rate},
        {QStringLiteral("audio_buffer_size"), static_cast<qint64>(options.audio_buffer_size)},
        {QStringLiteral("frame_size"), options.frame_size},
        {QStringLiteral("network_audio_format"), QString::fromLatin1(
            jam2::protocol::audio_format_text(options.network_audio_format))},
        {QStringLiteral("capture_ring_frames"), static_cast<qint64>(options.capture_ring_frames)},
        {QStringLiteral("playback_ring_frames"), static_cast<qint64>(options.playback_ring_frames)},
        {QStringLiteral("playback_prefill_frames"), static_cast<qint64>(options.playback_prefill_frames)},
        {QStringLiteral("playback_max_frames"), static_cast<qint64>(options.playback_max_frames)},
        {QStringLiteral("drift_correction"), options.drift_correction},
        {QStringLiteral("drift_smoothing"), options.drift_smoothing},
        {QStringLiteral("drift_deadband_ppm"), options.drift_deadband_ppm},
        {QStringLiteral("drift_max_correction_ppm"), options.drift_max_correction_ppm},
        {QStringLiteral("sample_time_playout"), options.sample_time_playout},
        {QStringLiteral("playout_delay_frames"), static_cast<qint64>(options.playout_delay_frames)},
        {QStringLiteral("jitter_buffer_frames"), static_cast<qint64>(options.jitter_buffer_frames)},
        {QStringLiteral("jitter_buffer_max_frames"), static_cast<qint64>(options.jitter_buffer_max_frames)},
        {QStringLiteral("adaptive_playback_cushion"), options.adaptive_playback_cushion},
        {QStringLiteral("adaptive_playback_target_frames"), static_cast<qint64>(options.adaptive_playback_target_frames)},
        {QStringLiteral("adaptive_playback_min_frames"), static_cast<qint64>(options.adaptive_playback_min_frames)},
        {QStringLiteral("adaptive_playback_max_frames"), static_cast<qint64>(options.adaptive_playback_max_frames)},
        {QStringLiteral("adaptive_playback_release_ppm"), options.adaptive_playback_release_ppm},
        {QStringLiteral("adaptive_playback_ratio_ramp_ms"), options.adaptive_playback_ratio_ramp_ms},
        {QStringLiteral("headless_audio"), options.headless_audio},
        {QStringLiteral("headless_clock_drift_ppm"), options.headless_clock_drift_ppm},
        {QStringLiteral("audio_device"), options.audio_device_id ? QJsonValue(*options.audio_device_id) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("bind"), endpoint(options.bind)},
        {QStringLiteral("stun"), endpoint(options.stun_server)},
        {QStringLiteral("public_endpoint"), optionalEndpoint(options.public_endpoint)},
        {QStringLiteral("no_stun"), options.no_stun},
        {QStringLiteral("stun_timeout_ms"), options.stun_timeout_ms},
        {QStringLiteral("stun_retries"), options.stun_retries},
        {QStringLiteral("wait_ms"), options.wait_ms},
        {QStringLiteral("stream_ms"), options.stream_ms},
        {QStringLiteral("stream_linger_ms"), options.stream_linger_ms},
        {QStringLiteral("stats"), options.stats_enabled},
        {QStringLiteral("stats_interval_ms"), options.stats_interval_ms},
        {QStringLiteral("stats_warmup_ms"), options.stats_warmup_ms},
        {QStringLiteral("log_stats_root"), options.log_stats_dir
            ? QJsonValue(QString::fromStdString(options.log_stats_dir->string()))
            : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("socket_send_buffer"), optionalInteger(options.socket_send_buffer)},
        {QStringLiteral("socket_recv_buffer"), optionalInteger(options.socket_recv_buffer)},
        {QStringLiteral("metronome"), options.metronome},
        {QStringLiteral("bpm"), options.bpm},
        {QStringLiteral("metronome_level"), options.metronome_level},
        {QStringLiteral("metronome_mode"), QString::fromLatin1(
            jam2::cli::metronome_mode_text(options.metronome_mode).data(),
            static_cast<qsizetype>(jam2::cli::metronome_mode_text(options.metronome_mode).size()))},
        {QStringLiteral("metronome_compensation_max_ms"), options.metronome_compensation_max_ms},
        {QStringLiteral("metronome_compensation_smoothing_ms"), options.metronome_compensation_smoothing_ms},
        {QStringLiteral("metronome_compensation_deadband_ms"), options.metronome_compensation_deadband_ms},
        {QStringLiteral("metronome_compensation_slew_ms_per_sec"), options.metronome_compensation_slew_ms_per_sec},
        {QStringLiteral("remote_level"), options.remote_level},
        {QStringLiteral("send_level"), options.send_level},
        {QStringLiteral("local_monitor"), options.local_monitor},
        {QStringLiteral("local_monitor_level"), options.local_monitor_level},
        {QStringLiteral("test_input"), QString::fromLatin1(
            jam2::cli::test_input_mode_text(options.test_input).data(),
            static_cast<qsizetype>(jam2::cli::test_input_mode_text(options.test_input).size()))},
        {QStringLiteral("os_priority"), QString::fromLatin1(
            jam2::cli::os_priority_text(options.os_priority).data(),
            static_cast<qsizetype>(jam2::cli::os_priority_text(options.os_priority).size()))},
        {QStringLiteral("input_mix"), QString::fromStdString(
            jam2::cli::mono_mix_mode_text(options.channel_selection.input.size()))},
        {QStringLiteral("channel_selection"), QString::fromStdString(
            jam2::cli::channel_selection_text(options.channel_selection))},
    };
}

QByteArray hashFile(const QString& path, qint64 maximumBytes, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maximumBytes) {
        error = QStringLiteral("artifact is unreadable or exceeds the retained-byte bound");
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 64 * 1024> buffer{};
    while (!file.atEnd()) {
        const qint64 read = file.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0) {
            error = QStringLiteral("artifact read failed while hashing");
            return {};
        }
        hash.addData(QByteArrayView(buffer.data(), read));
    }
    return hash.result().toHex();
}

QJsonArray artifactInventory(
    const QString& root,
    const QString& manifestPath,
    bool& truncated)
{
    truncated = false;
    QStringList files;
    QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (QFileInfo(path).absoluteFilePath() == QFileInfo(manifestPath).absoluteFilePath()) {
            continue;
        }
        if (files.size() >= kMaxArtifactFiles) {
            truncated = true;
            break;
        }
        files.push_back(path);
    }
    files.sort();
    QJsonArray result;
    qint64 remaining = kMaxArtifactBytes;
    const QDir rootDir(root);
    for (const QString& path : files) {
        const QFileInfo info(path);
        QString error;
        const QByteArray hash = hashFile(path, remaining, error);
        QJsonObject artifact{
            {QStringLiteral("path"), rootDir.relativeFilePath(path)},
            {QStringLiteral("bytes"), info.size()},
        };
        if (error.isEmpty()) {
            artifact.insert(QStringLiteral("sha256"), QString::fromLatin1(hash));
            remaining -= info.size();
        } else {
            artifact.insert(QStringLiteral("error"), error);
        }
        result.push_back(artifact);
    }
    return result;
}

void writeManifest(
    const ParsedScenario& scenario,
    int returnCode,
    const QString& startedUtc,
    const QJsonObject& result)
{
    const QString executable = QCoreApplication::applicationFilePath();
    const QFileInfo executableInfo(executable);
    QString executableHashError;
    const QByteArray executableHash = hashFile(executable, kMaxArtifactBytes, executableHashError);
    QString scenarioHashError;
    const QByteArray scenarioHash = hashFile(scenario.path, kMaxScenarioBytes, scenarioHashError);
    QJsonObject build{
        {QStringLiteral("executable"), executable},
        {QStringLiteral("bytes"), executableInfo.size()},
        {QStringLiteral("modified_utc"), executableInfo.lastModified().toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("compiled_date"), QStringLiteral(__DATE__)},
        {QStringLiteral("compiled_time"), QStringLiteral(__TIME__)},
        {QStringLiteral("qt_runtime_version"), QString::fromLatin1(qVersion())},
    };
    if (executableHashError.isEmpty()) {
        build.insert(QStringLiteral("sha256"), QString::fromLatin1(executableHash));
    }
    QJsonObject effectiveConfiguration = optionsJson(scenario.effectiveOptions);
    const QJsonObject network = scenario.source.value(QStringLiteral("network")).toObject();
    effectiveConfiguration.insert(
        QStringLiteral("max_peers"),
        network.value(QStringLiteral("max_peers")).toInt(0));
    bool artifactInventoryTruncated = false;
    const QJsonArray artifacts = artifactInventory(
        scenario.artifactRoot, scenario.manifestPath, artifactInventoryTruncated);
    QJsonObject manifest{
        {QStringLiteral("schema"), QStringLiteral("jam2-debug-manifest")},
        {QStringLiteral("scenario_format"), QString::fromLatin1(kScenarioFormat)},
        {QStringLiteral("description_format"), QString::fromLatin1(kDescriptionFormat)},
        {QStringLiteral("automation_format"), QString::fromLatin1(kAutomationFormat)},
        {QStringLiteral("run_id"), scenario.runId},
        {QStringLiteral("operation"), scenario.operation},
        {QStringLiteral("started_utc"), startedUtc},
        {QStringLiteral("finished_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("return_code"), returnCode},
        {QStringLiteral("ok"), returnCode == 0},
        {QStringLiteral("reactive"), scenario.reactive},
        {QStringLiteral("scenario_path"), QFileInfo(scenario.path).absoluteFilePath()},
        {QStringLiteral("scenario_sha256"), QString::fromLatin1(scenarioHash)},
        {QStringLiteral("build"), build},
        {QStringLiteral("platform"), QJsonObject{
            {QStringLiteral("product_type"), QSysInfo::productType()},
            {QStringLiteral("product_version"), QSysInfo::productVersion()},
            {QStringLiteral("pretty_product_name"), QSysInfo::prettyProductName()},
            {QStringLiteral("kernel_type"), QSysInfo::kernelType()},
            {QStringLiteral("kernel_version"), QSysInfo::kernelVersion()},
            {QStringLiteral("current_cpu_architecture"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("build_cpu_architecture"), QSysInfo::buildCpuArchitecture()}}},
        {QStringLiteral("local_peer_id"), result.contains(QStringLiteral("local_peer_id"))
            ? result.value(QStringLiteral("local_peer_id"))
            : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("protocols"), QJsonObject{
            {QStringLiteral("scenario"), QString::fromLatin1(kScenarioFormat)},
            {QStringLiteral("description"), QString::fromLatin1(kDescriptionFormat)},
            {QStringLiteral("automation"), QString::fromLatin1(kAutomationFormat)},
            {QStringLiteral("control_version"), jam2::control_protocol::kControlProtocolVersion},
            {QStringLiteral("udp_version"), jam2::protocol::kProtocolVersion}}},
        {QStringLiteral("automation_limits"), QJsonObject{
            {QStringLiteral("max_scenario_bytes"), kMaxScenarioBytes},
            {QStringLiteral("max_string_bytes"), kMaxStringBytes},
            {QStringLiteral("max_actions"), kMaxActions},
            {QStringLiteral("max_frame_bytes"), static_cast<qint64>(AutomationChannel::kMaxFrameBytes)},
            {QStringLiteral("queue_capacity"), static_cast<qint64>(AutomationChannel::kQueueCapacity)},
            {QStringLiteral("commands_per_turn"), static_cast<qint64>(AutomationChannel::kCommandsPerTurn)},
            {QStringLiteral("incomplete_frame_timeout_ms"), AutomationChannel::kIncompleteFrameTimeoutMs}}},
        {QStringLiteral("effective_configuration"), effectiveConfiguration},
        {QStringLiteral("lifecycle"), QJsonObject{
            {QStringLiteral("state"), QStringLiteral("finished")},
            {QStringLiteral("end_reason"), result.contains(QStringLiteral("error"))
                ? result.value(QStringLiteral("error"))
                : QJsonValue(returnCode == 0
                    ? QStringLiteral("completed")
                    : QStringLiteral("operation-failed"))},
            {QStringLiteral("return_code"), returnCode}}},
        {QStringLiteral("result"), result},
        {QStringLiteral("artifacts"), artifacts},
        {QStringLiteral("artifact_inventory_truncated"), artifactInventoryTruncated},
        {QStringLiteral("artifact_limits"), QJsonObject{
            {QStringLiteral("max_files"), kMaxArtifactFiles},
            {QStringLiteral("max_total_bytes"), kMaxArtifactBytes}}},
    };
    QSaveFile file(scenario.manifestPath);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        throw std::runtime_error("failed to publish native debug manifest");
    }
}

int runFocusedOperation(const ParsedScenario& scenario, QJsonObject& result)
{
    if (scenario.operation == QStringLiteral("validate.boundaries")) {
        result = jam2RunBoundaryValidation(scenario.fixtures);
        return result.value(QStringLiteral("ok")).toBool(false) ? 0 : 3;
    }
    if (scenario.operation == QStringLiteral("validate.controller-lifecycle")) {
        const QJsonObject network = scenario.source.value(QStringLiteral("network")).toObject();
        result = jam2RunControllerLifecycleValidation(
            network.value(QStringLiteral("heartbeat_interval_ms")).toInt(20),
            network.value(QStringLiteral("heartbeat_miss_limit")).toInt(3));
        return result.value(QStringLiteral("ok")).toBool(false) ? 0 : 3;
    }
    return -1;
}

int runLifecycleSmoke(const ParsedScenario& scenario, QJsonObject& result)
{
    ApplicationRuntime runtime;
    const bool localBefore = runtime.startLocal(scenario.effectiveOptions);
    const auto pumpFor = [](int milliseconds) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    pumpFor(750);
    const std::uint64_t frameBefore = runtime.engineSnapshot().engine_frame;
    Jam2RuntimeOptions networkOptions = scenario.effectiveOptions;
    networkOptions.bind = jam2::parse_bind_endpoint("127.0.0.1:0");
    networkOptions.session_id = 1;
    networkOptions.session_key = std::array<std::uint8_t, 16>{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    networkOptions.no_stun = true;
    networkOptions.bootstrap_role = jam2::SessionBootstrapRole::Creator;
    networkOptions.local_peer_id = 1;
    networkOptions.bootstrap_coordinator_peer_id = 1;
    networkOptions.mesh_peers_configured = true;
    networkOptions.stream_ms = 750;
    networkOptions.arm_stream_on_first_peer = false;
    const bool networkStarted = runtime.startNetwork(networkOptions);
    while (runtime.isNetworkRunning()) pumpFor(10);
    const std::uint64_t frameNetwork = runtime.engineSnapshot().engine_frame;
    const bool localAfter = runtime.startLocal(scenario.effectiveOptions);
    pumpFor(750);
    const std::uint64_t finalFrame = runtime.engineSnapshot().engine_frame;
    const std::uint64_t starts = runtime.engineStarts();
    const std::uint64_t restarts = runtime.engineRestarts();
    const std::uint64_t reuses = runtime.engineReuses();
    runtime.shutdown();
    const bool ok = localBefore && networkStarted && localAfter && starts == 1 &&
        restarts == 0 && reuses == 2 && frameBefore > 0 &&
        frameNetwork > frameBefore && finalFrame > frameNetwork;
    result = {
        {QStringLiteral("event"), QStringLiteral("debug_lifecycle_result")},
        {QStringLiteral("ok"), ok},
        {QStringLiteral("engine_starts"), static_cast<qint64>(starts)},
        {QStringLiteral("engine_restarts"), static_cast<qint64>(restarts)},
        {QStringLiteral("engine_reuses"), static_cast<qint64>(reuses)},
        {QStringLiteral("frame_before_network"), static_cast<qint64>(frameBefore)},
        {QStringLiteral("frame_after_network"), static_cast<qint64>(frameNetwork)},
        {QStringLiteral("frame_after_return_local"), static_cast<qint64>(finalFrame)},
    };
    return ok ? 0 : 3;
}

int runScenario(const ParsedScenario& scenario, int argc, char* argv[], QJsonObject& result)
{
    int focused = runFocusedOperation(scenario, result);
    if (focused >= 0) {
        return focused;
    }
    if (scenario.operation == QStringLiteral("lifecycle.local-network-local")) {
        return runLifecycleSmoke(scenario, result);
    }

    QStringList command;
    if (scenario.operation == QStringLiteral("local")) {
        command << QStringLiteral("local");
    } else if (scenario.operation == QStringLiteral("network.create")) {
        command << QStringLiteral("network") << QStringLiteral("create");
    } else if (scenario.operation == QStringLiteral("network.join")) {
        const QString joinUrl = scenario.source.value(QStringLiteral("network"))
            .toObject().value(QStringLiteral("join_url")).toString();
        if (joinUrl.isEmpty() || joinUrl.toUtf8().size() > kMaxStringBytes ||
            !joinUrl.startsWith(QStringLiteral("jam2://"))) {
            throw std::runtime_error("network.join requires a bounded network.join_url");
        }
        command << QStringLiteral("network") << QStringLiteral("join") << joinUrl;
    }
    command.append(scenario.optionArguments);
    auto storage = toStorage(command, argv[0]);
    std::vector<char*> forwarded;
    forwarded.reserve(storage.size());
    for (std::string& value : storage) {
        forwarded.push_back(value.data());
    }
    QCoreApplication::instance()->setProperty(kScenarioProperty, scenario.source);
    int code = 0;
    if (scenario.operation.startsWith(QStringLiteral("network."))) {
        code = SessionController::runNetworkCommand(
            static_cast<int>(forwarded.size()), forwarded.data());
    } else {
        code = jam2::cli::runFrontend(static_cast<int>(forwarded.size()), forwarded.data());
    }
    result = QCoreApplication::instance()->property("jam2.debug.result").toJsonObject();
    return code;
}

} // namespace

int jam2RunDebugCommand(int argc, char* argv[])
{
    if (argc < 3 || isHelpArgument(argv[2])) {
        std::cout << kDebugUsage;
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "describe" && hasHelpArgument(argc, argv, 3)) {
        printDescribeHelp();
        return 0;
    }
    if (subcommand == "run" && hasHelpArgument(argc, argv, 3)) {
        printRunHelp();
        return 0;
    }
    if (subcommand == "fuzz" && hasHelpArgument(argc, argv, 3)) {
        printFuzzHelp();
        return 0;
    }
    if (argc == 4 && subcommand == "describe" && std::string_view(argv[3]) == "--json") {
        std::cout << QJsonDocument(descriptionJson()).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    }
    if (argc == 5 && subcommand == "fuzz") {
        try {
            return runFuzzInput(
                QString::fromLocal8Bit(argv[3]),
                QFileInfo(QString::fromLocal8Bit(argv[4])).absoluteFilePath());
        } catch (const std::exception& error) {
            std::cerr << "debug fuzz failed: " << error.what() << '\n';
            return 2;
        }
    }
    if (argc != 4 || subcommand != "run") {
        std::cerr << kDebugUsage;
        return 2;
    }

    const QString scenarioPath = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
    QFile file(scenarioPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 2 || file.size() > kMaxScenarioBytes) {
        std::cerr << "debug scenario must be a readable file of 2..262144 bytes\n";
        return 2;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject() || parseError.error != QJsonParseError::NoError) {
        std::cerr << "debug scenario is not valid JSON object data\n";
        return 2;
    }

    std::optional<ParsedScenario> scenario;
    const QString startedUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    try {
        scenario = parseScenario(scenarioPath, document.object(), argv[0]);
        const bool hasCommandHandle = qEnvironmentVariableIsSet("JAM2_AUTOMATION_COMMAND_HANDLE");
        const bool hasEventHandle = qEnvironmentVariableIsSet("JAM2_AUTOMATION_EVENT_HANDLE");
        if ((!scenario->reactive && (hasCommandHandle || hasEventHandle)) ||
            (scenario->reactive && (!hasCommandHandle || !hasEventHandle))) {
            throw std::runtime_error("automation handles do not match the scenario reactive contract");
        }
        QJsonObject result;
        const auto wallStarted = std::chrono::steady_clock::now();
        const auto cpuStarted = processCpuTimeNanoseconds();
        const int code = runScenario(*scenario, argc, argv, result);
        const auto wallFinished = std::chrono::steady_clock::now();
        const auto cpuFinished = processCpuTimeNanoseconds();
        const double wallElapsedMs = std::chrono::duration<double, std::milli>(
            wallFinished - wallStarted).count();
        result.insert(QStringLiteral("wall_elapsed_ms"), wallElapsedMs);
        if (cpuStarted && cpuFinished && *cpuFinished >= *cpuStarted) {
            const double cpuElapsedMs = static_cast<double>(*cpuFinished - *cpuStarted) / 1000000.0;
            result.insert(QStringLiteral("process_cpu_time_ms"), cpuElapsedMs);
            result.insert(
                QStringLiteral("process_cpu_percent_one_core"),
                wallElapsedMs > 0.0 ? cpuElapsedMs * 100.0 / wallElapsedMs : 0.0);
        }
        writeManifest(*scenario, code, startedUtc, result);
        std::cout << QJsonDocument(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("debug_result")},
            {QStringLiteral("format"), QString::fromLatin1(kAutomationFormat)},
            {QStringLiteral("run_id"), scenario->runId},
            {QStringLiteral("ok"), code == 0},
            {QStringLiteral("return_code"), code},
            {QStringLiteral("manifest"), scenario->manifestPath},
        }).toJson(QJsonDocument::Compact).constData() << '\n';
        return code;
    } catch (const std::exception& error) {
        std::cerr << "debug run failed: " << error.what() << '\n';
        if (scenario) {
            try {
                writeManifest(*scenario, 2, startedUtc, QJsonObject{
                    {QStringLiteral("error"), QString::fromUtf8(error.what())}});
            } catch (...) {
            }
        }
        return 2;
    }
}
