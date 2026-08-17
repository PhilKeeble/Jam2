#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>

namespace {

using namespace std::chrono_literals;

constexpr qint64 kJamRecordingSignalProofFrames =
    ((48000LL * 60LL + 173LL - 1LL) / 173LL) + 1024LL;

int failures = 0;

void fail(const QString& message)
{
    std::cerr << "FAIL: " << message.toStdString() << '\n';
    ++failures;
}

bool send(AutomationProcess& process, QJsonObject command)
{
    QString error;
    if (process.send(std::move(command), error)) return true;
    fail(QStringLiteral("sending automation command: ") + error);
    return false;
}

std::optional<QJsonObject> receive(
    AutomationProcess& process,
    const QString& expected,
    std::chrono::milliseconds timeout = 20s)
{
    QJsonObject event;
    QString error;
    if (!process.readEvent(event, timeout, error)) {
        fail(QStringLiteral("reading %1: %2").arg(expected, error));
        return std::nullopt;
    }
    if (event.value(QStringLiteral("event")).toString() != expected) {
        fail(QStringLiteral("expected %1, received %2 (%3)").arg(
            expected,
            event.value(QStringLiteral("event")).toString(),
            event.value(QStringLiteral("reason")).toString()));
        return std::nullopt;
    }
    return event;
}

bool invoke(
    AutomationProcess& process,
    const QString& id,
    const QString& control,
    const QString& operation,
    QJsonValue value = {})
{
    QJsonObject command{
        {QStringLiteral("type"), QStringLiteral("invoke")},
        {QStringLiteral("id"), id},
        {QStringLiteral("control"), control},
        {QStringLiteral("operation"), operation},
    };
    if (!value.isUndefined()) command.insert(QStringLiteral("value"), std::move(value));
    if (!send(process, std::move(command))) return false;
    const auto applied = receive(process, QStringLiteral("command_applied"));
    if (!applied || applied->value(QStringLiteral("id")).toString() != id) {
        fail(QStringLiteral("invoke %1 did not return its command id").arg(id));
        return false;
    }
    return true;
}

std::optional<QJsonObject> controlState(
    AutomationProcess& process,
    const QString& controlId,
    const QString& requestPrefix)
{
    int cursor = 0;
    for (;;) {
        const QString requestId = requestPrefix + QLatin1Char('-') + QString::number(cursor);
        if (!send(process, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), requestId},
                {QStringLiteral("cursor"), cursor},
            })) return std::nullopt;
        const auto event = receive(process, QStringLiteral("snapshot"));
        if (!event || event->value(QStringLiteral("id")).toString() != requestId) {
            fail(QStringLiteral("control snapshot response id mismatch"));
            return std::nullopt;
        }
        for (const QJsonValue& value : event->value(QStringLiteral("controls")).toArray()) {
            const QJsonObject control = value.toObject();
            if (control.value(QStringLiteral("test_id")).toString() == controlId) {
                return control.value(QStringLiteral("state")).toObject();
            }
        }
        cursor = event->value(QStringLiteral("next_cursor")).toInt(-1);
        if (cursor < 0) break;
    }
    fail(QStringLiteral("GUI view omitted control ") + controlId);
    return std::nullopt;
}

std::optional<QJsonObject> snapshot(
    AutomationProcess& process,
    int peer,
    int sequence)
{
    if (!send(process, {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), QStringLiteral("performance-%1-%2")
                .arg(peer).arg(sequence)},
            {QStringLiteral("cursor"), 0},
        })) return std::nullopt;
    const auto event = receive(process, QStringLiteral("snapshot"));
    if (!event || !event->value(QStringLiteral("jam")).isObject() ||
        !event->value(QStringLiteral("performance")).isObject()) {
        fail(QStringLiteral("peer %1 snapshot omitted jam or performance state").arg(peer));
        return std::nullopt;
    }
    return event;
}

void printState(std::size_t peer, const QJsonObject& state)
{
    const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
    std::cerr << "  peer-" << peer + 1
              << " remotes=" << jam.value(QStringLiteral("remote_peer_count")).toInt()
              << " active=" << jam.value(QStringLiteral("active_remote_peer_count")).toInt()
              << " frame=" << performance.value(QStringLiteral("engine_frame")).toInteger()
              << " callbacks=" << performance.value(QStringLiteral("callbacks")).toInteger()
              << " input_peak=" << performance.value(QStringLiteral("input_peak_ppm")).toInt()
              << " send_peak=" << performance.value(QStringLiteral("send_peak_ppm")).toInt()
              << " remote_peak=" << performance.value(QStringLiteral("remote_peak_ppm")).toInt()
              << " metro=" << performance.value(QStringLiteral("metronome_enabled")).toBool()
              << " bpm=" << performance.value(QStringLiteral("metronome_bpm")).toInt()
              << " action=" << performance.value(QStringLiteral("transport_action"))
                    .toString().toStdString()
              << " revision=" << performance.value(QStringLiteral("transport_revision")).toInteger()
              << " commits=" << performance.value(QStringLiteral("transport_commit_count")).toInteger()
              << " requested=" << performance.value(
                    QStringLiteral("global_transport_requested_playing")).toBool()
              << " playing=" << performance.value(
                    QStringLiteral("global_transport_playing")).toBool()
              << " recording_latency=" << performance.value(
                    QStringLiteral("recording_latency_adjustment_frames")).toInteger()
              << " lanes=" << content.value(QStringLiteral("lane_count")).toInt(-1)
              << " recording_phase=" << content.value(
                    QStringLiteral("lane_recording_local_phase")).toString().toStdString()
              << " recording_group=" << content.value(
                    QStringLiteral("lane_recording_active_group_id")).toString()
                    .left(8).toStdString()
              << " recording_protected=" << content.value(
                    QStringLiteral("lane_recording_protected")).toBool()
              << " recording_import_retries=" << content.value(
                    QStringLiteral("lane_recording_import_busy_retries")).toInteger()
              << " recording_import_failures=" << content.value(
                    QStringLiteral("lane_recording_import_failures")).toInteger()
              << " recording_import_status=" << content.value(
                    QStringLiteral("lane_recording_import_status")).toString().toStdString()
              << " recording_import_target=" << content.value(
                    QStringLiteral("lane_recording_import_target_id")).toString().toStdString()
              << " recording_import_hash=" << content.value(
                    QStringLiteral("lane_recording_import_last_hash")).toString().toStdString()
              << " track_ms=" << content.value(
                    QStringLiteral("track_duration_ms")).toInt(-1)
              << " loop=" << content.value(
                    QStringLiteral("track_loop_enabled")).toBool()
              << ':' << content.value(
                    QStringLiteral("track_loop_start_seconds")).toDouble(-2.0)
              << '-' << content.value(
                    QStringLiteral("track_loop_end_seconds")).toDouble(-2.0)
              << " prepared_frames=" << content.value(
                    QStringLiteral("prepared_mix_frames")).toInteger(-1)
              << " prepared_available=" << content.value(
                    QStringLiteral("track_file_available")).toBool()
              << " prepared_worker=" << content.value(
                    QStringLiteral("prepared_mix_worker_running")).toBool()
              << " prepared_failures=" << content.value(
                    QStringLiteral("prepared_mix_failures")).toInteger(-1)
              << " prepared_playing=" << performance.value(
                    QStringLiteral("prepared_source_playing")).toBool()
              << " prepared_source_frame=" << performance.value(
                    QStringLiteral("prepared_source_frame")).toInteger(-1)
              << " prepared_scheduled_frame=" << performance.value(
                    QStringLiteral("prepared_source_scheduled_start_frame")).toInteger(-1)
              << " prepared_actual_frame=" << performance.value(
                    QStringLiteral("prepared_source_actual_start_frame")).toInteger(-1)
              << " file_tasks=" << content.value(
                    QStringLiteral("file_tasks_active")).toInt(-1)
              << " lane_ids=" << QJsonDocument(content.value(
                    QStringLiteral("lane_ids")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " asset_hashes=" << QJsonDocument(content.value(
                    QStringLiteral("asset_hashes")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " asset_available=" << QJsonDocument(content.value(
                    QStringLiteral("asset_available")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " asset_bytes=" << QJsonDocument(content.value(
                    QStringLiteral("asset_bytes")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " failure=" << jam.value(QStringLiteral("failure")).toString().toStdString()
              << '\n';
}

template <typename Predicate>
bool waitForAll(
    FourPeerCoordinator& coordinator,
    const QString& description,
    Predicate predicate,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    std::chrono::milliseconds timeout = 40s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(timeout);
    int sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (std::size_t peer = 0; peer < states.size(); ++peer) {
            const auto current = snapshot(
                coordinator.peer(peer), static_cast<int>(peer + 1), sequence);
            if (!current) return false;
            states[peer] = *current;
            ready = predicate(peer, *current) && ready;
        }
        if (ready) return true;
        ++sequence;
        QThread::msleep(75);
    }
    fail(QStringLiteral("timed out waiting for ") + description);
    for (std::size_t peer = 0; peer < states.size(); ++peer) printState(peer, states[peer]);
    return false;
}

bool fullMeshWithAudio(std::size_t, const QJsonObject& state)
{
    const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
    const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
    return jam.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
        jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
        jam.value(QStringLiteral("network_attachment_ready")).toBool() &&
        jam.value(QStringLiteral("network_running")).toBool() &&
        jam.value(QStringLiteral("failure")).toString().isEmpty() &&
        performance.value(QStringLiteral("engine_running")).toBool() &&
        performance.value(QStringLiteral("network_running")).toBool() &&
        performance.value(QStringLiteral("headless_audio")).toBool() &&
        performance.value(QStringLiteral("test_input")).toString() ==
            QStringLiteral("tone-440") &&
        performance.value(QStringLiteral("engine_frame")).toInteger() > 0 &&
        performance.value(QStringLiteral("callbacks")).toInteger() > 0 &&
        performance.value(QStringLiteral("network_capture_enabled")).toBool() &&
        performance.value(QStringLiteral("network_capture_ready")).toBool() &&
        performance.value(QStringLiteral("network_playback_enabled")).toBool() &&
        performance.value(QStringLiteral("input_peak_ppm")).toInt() > 0 &&
        performance.value(QStringLiteral("send_peak_ppm")).toInt() > 0 &&
        performance.value(QStringLiteral("remote_peak_ppm")).toInt() > 0;
}

int jsonStringIndex(const QJsonArray& values, const QString& target)
{
    for (qsizetype index = 0; index < values.size(); ++index) {
        if (values.at(index).toString() == target) return static_cast<int>(index);
    }
    return -1;
}

quint16 readU16(const QByteArray& bytes, qsizetype offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset))) |
        static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset + 1))) << 8U;
}

quint32 readU32(const QByteArray& bytes, qsizetype offset)
{
    quint32 value = 0;
    for (int byte = 0; byte < 4; ++byte) {
        value |= static_cast<quint32>(
            static_cast<unsigned char>(bytes.at(offset + byte))) << (byte * 8U);
    }
    return value;
}

bool verifyRecordedWav(
    const QString& path,
    qint64 expectedFrames,
    QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("could not open ") + path;
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 44 || bytes.sliced(0, 4) != QByteArrayLiteral("RIFF") ||
        bytes.sliced(8, 4) != QByteArrayLiteral("WAVE") ||
        bytes.sliced(12, 4) != QByteArrayLiteral("fmt ") ||
        bytes.sliced(36, 4) != QByteArrayLiteral("data") ||
        readU16(bytes, 20) != 1 || readU16(bytes, 22) != 1 ||
        readU32(bytes, 24) != 48000 || readU16(bytes, 34) != 16) {
        error = QStringLiteral("invalid mono PCM16/48k header in ") + path;
        return false;
    }
    const quint32 dataBytes = readU32(bytes, 40);
    if ((dataBytes % 2U) != 0U || dataBytes == 0U ||
        static_cast<quint64>(dataBytes) + 44ULL != static_cast<quint64>(bytes.size()) ||
        static_cast<qint64>(dataBytes / 2U) != expectedFrames) {
        error = QStringLiteral("WAV length does not match recorder frames in ") + path;
        return false;
    }
    bool signal = false;
    for (qsizetype offset = 44; offset + 1 < bytes.size(); offset += 2) {
        if (readU16(bytes, offset) != 0U) {
            signal = true;
            break;
        }
    }
    if (!signal) {
        error = QStringLiteral("recorded stem contains only silence: ") + path;
        return false;
    }
    return true;
}

bool shutdown(FourPeerCoordinator& coordinator)
{
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        if (!send(coordinator.peer(peer), {
                {QStringLiteral("type"), QStringLiteral("shutdown")},
                {QStringLiteral("id"), QStringLiteral("shutdown-%1").arg(peer + 1)},
            })) return false;
    }
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        (void)receive(coordinator.peer(peer), QStringLiteral("command_applied"));
        (void)receive(coordinator.peer(peer), QStringLiteral("shutdown"));
        int exitCode = -1;
        QString error;
        if (!coordinator.peer(peer).waitForExit(20s, exitCode, error) || exitCode != 0) {
            fail(QStringLiteral("peer %1 did not exit cleanly: %2 code=%3")
                .arg(peer + 1).arg(error).arg(exitCode));
        }
    }
    return failures == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: jam2_four_performance_integration <release-jam2>\n";
        return 2;
    }

    LoopbackPortReservations portReservations;
    QString portError;
    if (!portReservations.reserve(FourPeerCoordinator::kPeerCount, portError)) {
        fail(QStringLiteral("could not reserve four distinct local TCP/UDP ports: ") +
            portError);
        return 1;
    }
    std::array<quint16, FourPeerCoordinator::kPeerCount> ports{};
    for (std::size_t peer = 0; peer < ports.size(); ++peer) {
        ports[peer] = portReservations.port(peer);
    }

    std::array<QStringList, FourPeerCoordinator::kPeerCount> arguments;
    const bool show = qEnvironmentVariableIntValue("JAM2_TEST_SHOW_GUI") == 1;
    for (std::size_t peer = 0; peer < arguments.size(); ++peer) {
        arguments[peer] << QStringLiteral("debug") << QStringLiteral("gui-agent")
            << QStringLiteral("--instance-id")
            << QStringLiteral("performance-peer-%1").arg(peer + 1);
        if (show) arguments[peer] << QStringLiteral("--show-gui");
    }
    FourPeerCoordinator coordinator;
    QString error;
    if (!coordinator.launch(QString::fromLocal8Bit(argv[1]), arguments, error)) {
        fail(QStringLiteral("launching four performance peers: ") + error);
        return 1;
    }
    for (std::size_t peer = 0; peer < arguments.size(); ++peer) {
        (void)receive(coordinator.peer(peer), QStringLiteral("hello"));
    }
    portReservations.release();

    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("jam.create")},
            {QStringLiteral("id"), QStringLiteral("create")},
            {QStringLiteral("port"), ports[0]},
            {QStringLiteral("test_input"), QStringLiteral("tone-440")},
        })) return 1;
    const auto created = receive(coordinator.peer(0), QStringLiteral("command_applied"));
    if (!created) return 1;
    const QString invite = created->value(QStringLiteral("invite_url")).toString();
    if (invite.isEmpty()) {
        fail(QStringLiteral("creator did not return its invite URL"));
        return 1;
    }
    for (std::size_t peer = 1; peer < arguments.size(); ++peer) {
        if (!send(coordinator.peer(peer), {
                {QStringLiteral("type"), QStringLiteral("jam.join")},
                {QStringLiteral("id"), QStringLiteral("join-%1").arg(peer + 1)},
                {QStringLiteral("port"), ports[peer]},
                {QStringLiteral("invite_url"), invite},
                {QStringLiteral("test_input"), QStringLiteral("tone-440")},
            }) || !receive(coordinator.peer(peer), QStringLiteral("command_applied"))) {
            return 1;
        }
    }

    std::array<QJsonObject, FourPeerCoordinator::kPeerCount> states;
    if (!waitForAll(coordinator, QStringLiteral("four-peer injected audio flow"),
            fullMeshWithAudio, states)) return 1;
    QString convergedLooperHash;
    if (!waitForAll(coordinator, QStringLiteral("four-peer initial lane convergence"),
        [&convergedLooperHash](std::size_t peer, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            const QString hash = content.value(QStringLiteral("looper_sha256")).toString();
            if (peer == 0) convergedLooperHash = hash;
            return !hash.isEmpty() && hash == convergedLooperHash;
        }, states)) return 1;
    const QJsonObject baselineContent = states[0]
        .value(QStringLiteral("content")).toObject();
    const int baselineLaneCount = baselineContent
        .value(QStringLiteral("lane_count")).toInt(-1);
    const int baselineActiveBankLaneCount = baselineContent
        .value(QStringLiteral("active_bank_lane_count")).toInt(-1);
    const QJsonArray baselineActiveBankLaneIds = baselineContent
        .value(QStringLiteral("active_bank_lane_ids")).toArray();
    if (baselineLaneCount < 0 || baselineActiveBankLaneCount < 0 ||
        baselineActiveBankLaneIds.size() != baselineActiveBankLaneCount) {
        fail(QStringLiteral("four-peer baseline omitted active-bank lane identity"));
        return 1;
    }

    std::array<double, FourPeerCoordinator::kPeerCount> originalMaximum{};
    std::array<double, FourPeerCoordinator::kPeerCount> originalSmoothing{};
    std::array<double, FourPeerCoordinator::kPeerCount> originalDeadband{};
    std::array<double, FourPeerCoordinator::kPeerCount> originalSlew{};
    std::array<qint64, FourPeerCoordinator::kPeerCount> originalRecordingLatency{};
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject performance = states[peer]
            .value(QStringLiteral("performance")).toObject();
        if (!performance.value(
                QStringLiteral("metronome_compensation_runtime_available")).toBool()) {
            fail(QStringLiteral("peer %1 did not expose effective compensation tuning")
                .arg(peer + 1));
        }
        originalMaximum[peer] = performance.value(
            QStringLiteral("metronome_compensation_max_ms")).toDouble();
        originalSmoothing[peer] = performance.value(
            QStringLiteral("metronome_compensation_smoothing_ms")).toDouble();
        originalDeadband[peer] = performance.value(
            QStringLiteral("metronome_compensation_deadband_ms")).toDouble();
        originalSlew[peer] = performance.value(
            QStringLiteral("metronome_compensation_slew_ms_per_sec")).toDouble();
        originalRecordingLatency[peer] = performance.value(
            QStringLiteral("recording_latency_adjustment_frames")).toInteger();
    }
    if (failures != 0) return 1;

    constexpr std::size_t recordingDialogPeer = 1;
    const qint64 changedRecordingLatency =
        originalRecordingLatency[recordingDialogPeer] == 731 ? 732 : 731;
    auto& recordingDialogProcess = coordinator.peer(recordingDialogPeer);
    const QString recordingDialogLane = QStringLiteral("looper.lane.%1")
        .arg(baselineActiveBankLaneCount);
    if (!invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-add-lane"),
            QStringLiteral("looper.lane.add-empty"),
            QStringLiteral("click")) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-open"),
            recordingDialogLane + QStringLiteral(".arm"),
            QStringLiteral("click-async")) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-latency"),
            QStringLiteral("looper.recording.latency-adjustment"),
            QStringLiteral("set-value"),
            changedRecordingLatency) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-cancel"),
            QStringLiteral("looper.recording-dialog.cancel"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("cancelled recording latency isolation"),
        [&originalRecordingLatency](std::size_t peer, const QJsonObject& state) {
            return state.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("recording_latency_adjustment_frames"))
                .toInteger() == originalRecordingLatency[peer];
        }, states, 5s)) return 1;
    if (!invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-apply-open"),
            recordingDialogLane + QStringLiteral(".arm"),
            QStringLiteral("click-async")) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-apply-latency"),
            QStringLiteral("looper.recording.latency-adjustment"),
            QStringLiteral("set-value"),
            changedRecordingLatency) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-arm"),
            QStringLiteral("looper.recording-dialog.arm"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("accepted recording latency locality"),
        [&originalRecordingLatency, changedRecordingLatency, recordingDialogPeer](
            std::size_t peer, const QJsonObject& state) {
            const qint64 expected = peer == recordingDialogPeer
                ? changedRecordingLatency : originalRecordingLatency[peer];
            return state.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("recording_latency_adjustment_frames"))
                .toInteger() == expected;
        }, states, 5s)) return 1;
    if (!invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-disarm"),
            recordingDialogLane + QStringLiteral(".arm"),
            QStringLiteral("click")) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-restore-open"),
            recordingDialogLane + QStringLiteral(".arm"),
            QStringLiteral("click-async")) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-restore-latency"),
            QStringLiteral("looper.recording.latency-adjustment"),
            QStringLiteral("set-value"),
            originalRecordingLatency[recordingDialogPeer]) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-restore-arm"),
            QStringLiteral("looper.recording-dialog.arm"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("restored recording latency"),
        [&originalRecordingLatency](std::size_t peer, const QJsonObject& state) {
            return state.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("recording_latency_adjustment_frames"))
                .toInteger() == originalRecordingLatency[peer];
        }, states, 5s)) return 1;
    if (!invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-restored-disarm"),
            recordingDialogLane + QStringLiteral(".arm"),
            QStringLiteral("click"))) return 1;
    if (!invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-remove-lane-open"),
            recordingDialogLane + QStringLiteral(".remove"),
            QStringLiteral("click-async")) ||
        !invoke(recordingDialogProcess,
            QStringLiteral("recording-dialog-remove-lane"),
            QStringLiteral("looper.lane-remove-dialog.remove-only"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("recording dialog lane removal convergence"),
        [baselineLaneCount, baselineActiveBankLaneCount, baselineActiveBankLaneIds](
            std::size_t, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            return content.value(QStringLiteral("lane_count")).toInt(-1) ==
                    baselineLaneCount &&
                content.value(QStringLiteral("active_bank_lane_count")).toInt(-1) ==
                    baselineActiveBankLaneCount &&
                content.value(QStringLiteral("active_bank_lane_ids")).toArray() ==
                    baselineActiveBankLaneIds;
        }, states, 8s)) return 1;

    if (!invoke(
            coordinator.peer(0),
            QStringLiteral("listener-mode"),
            QStringLiteral("metronome.mode"),
            QStringLiteral("set-index"),
            2)) return 1;
    if (!waitForAll(coordinator, QStringLiteral("listener-compensated mode convergence"),
        [](std::size_t, const QJsonObject& state) {
            return state.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("metronome_mode")).toString() ==
                    QStringLiteral("listener-compensated");
        }, states)) return 1;

    constexpr std::size_t tunedPeer = 1;
    const double changedMaximum = originalMaximum[tunedPeer] == 333.3 ? 334.3 : 333.3;
    const double changedSmoothing = originalSmoothing[tunedPeer] == 1444.4 ? 1445.4 : 1444.4;
    const double changedDeadband = originalDeadband[tunedPeer] == 11.1 ? 12.1 : 11.1;
    const double changedSlew = originalSlew[tunedPeer] == 88.8 ? 89.8 : 88.8;
    auto& tuned = coordinator.peer(tunedPeer);
    if (!invoke(tuned, QStringLiteral("open-live-compensation"),
            QStringLiteral("metronome.compensation"), QStringLiteral("click-async")) ||
        !invoke(tuned, QStringLiteral("live-maximum"),
            QStringLiteral("metronome.compensation-dialog.maximum"),
            QStringLiteral("set-value"), changedMaximum) ||
        !invoke(tuned, QStringLiteral("live-smoothing"),
            QStringLiteral("metronome.compensation-dialog.smoothing"),
            QStringLiteral("set-value"), changedSmoothing) ||
        !invoke(tuned, QStringLiteral("live-deadband"),
            QStringLiteral("metronome.compensation-dialog.deadband"),
            QStringLiteral("set-value"), changedDeadband) ||
        !invoke(tuned, QStringLiteral("live-slew"),
            QStringLiteral("metronome.compensation-dialog.slew"),
            QStringLiteral("set-value"), changedSlew) ||
        !invoke(tuned, QStringLiteral("apply-live-compensation"),
            QStringLiteral("metronome.compensation-dialog.apply"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("local live compensation update"),
        [&](std::size_t peer, const QJsonObject& state) {
            const QJsonObject performance = state
                .value(QStringLiteral("performance")).toObject();
            const bool tunedValues =
                performance.value(QStringLiteral("metronome_compensation_max_ms"))
                    .toDouble() == changedMaximum &&
                performance.value(QStringLiteral("metronome_compensation_smoothing_ms"))
                    .toDouble() == changedSmoothing &&
                performance.value(QStringLiteral("metronome_compensation_deadband_ms"))
                    .toDouble() == changedDeadband &&
                performance.value(QStringLiteral("metronome_compensation_slew_ms_per_sec"))
                    .toDouble() == changedSlew;
            const bool originalValues =
                performance.value(QStringLiteral("metronome_compensation_max_ms"))
                    .toDouble() == originalMaximum[peer] &&
                performance.value(QStringLiteral("metronome_compensation_smoothing_ms"))
                    .toDouble() == originalSmoothing[peer] &&
                performance.value(QStringLiteral("metronome_compensation_deadband_ms"))
                    .toDouble() == originalDeadband[peer] &&
                performance.value(QStringLiteral("metronome_compensation_slew_ms_per_sec"))
                    .toDouble() == originalSlew[peer];
            return peer == tunedPeer ? tunedValues : originalValues;
        }, states)) return 1;

    if (!invoke(tuned, QStringLiteral("reopen-live-compensation"),
            QStringLiteral("metronome.compensation"), QStringLiteral("click-async")) ||
        !invoke(tuned, QStringLiteral("restore-live-maximum"),
            QStringLiteral("metronome.compensation-dialog.maximum"),
            QStringLiteral("set-value"), originalMaximum[tunedPeer]) ||
        !invoke(tuned, QStringLiteral("restore-live-smoothing"),
            QStringLiteral("metronome.compensation-dialog.smoothing"),
            QStringLiteral("set-value"), originalSmoothing[tunedPeer]) ||
        !invoke(tuned, QStringLiteral("restore-live-deadband"),
            QStringLiteral("metronome.compensation-dialog.deadband"),
            QStringLiteral("set-value"), originalDeadband[tunedPeer]) ||
        !invoke(tuned, QStringLiteral("restore-live-slew"),
            QStringLiteral("metronome.compensation-dialog.slew"),
            QStringLiteral("set-value"), originalSlew[tunedPeer]) ||
        !invoke(tuned, QStringLiteral("apply-restored-compensation"),
            QStringLiteral("metronome.compensation-dialog.apply"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("restored compensation tuning"),
        [&](std::size_t peer, const QJsonObject& state) {
            const QJsonObject performance = state
                .value(QStringLiteral("performance")).toObject();
            return performance.value(QStringLiteral("metronome_compensation_max_ms"))
                    .toDouble() == originalMaximum[peer] &&
                performance.value(QStringLiteral("metronome_compensation_smoothing_ms"))
                    .toDouble() == originalSmoothing[peer] &&
                performance.value(QStringLiteral("metronome_compensation_deadband_ms"))
                    .toDouble() == originalDeadband[peer] &&
                performance.value(QStringLiteral("metronome_compensation_slew_ms_per_sec"))
                    .toDouble() == originalSlew[peer];
        }, states)) return 1;
    if (!invoke(
            coordinator.peer(0),
            QStringLiteral("restore-shared-grid-mode"),
            QStringLiteral("metronome.mode"),
            QStringLiteral("set-index"),
            0)) return 1;
    if (!waitForAll(coordinator, QStringLiteral("shared-grid mode restoration"),
        [](std::size_t, const QJsonObject& state) {
            return state.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("metronome_mode")).toString() ==
                    QStringLiteral("shared-grid");
        }, states)) return 1;

    const int initialPolicyRevision = states[0].value(QStringLiteral("jam")).toObject()
        .value(QStringLiteral("policy")).toObject().value(QStringLiteral("revision")).toInt();
    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("jam.policy")},
            {QStringLiteral("id"), QStringLiteral("performance-policy")},
            {QStringLiteral("track_lanes"), true},
            {QStringLiteral("auto_share_wavs"), true},
            {QStringLiteral("global_playback"), true},
            {QStringLiteral("generated_ideas"), QStringLiteral("full")},
            {QStringLiteral("metronome_state"), true},
            {QStringLiteral("recordings"), true},
        }) || !receive(coordinator.peer(0), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("performance policy convergence"),
        [initialPolicyRevision](std::size_t, const QJsonObject& state) {
            const QJsonObject policy = state.value(QStringLiteral("jam")).toObject()
                .value(QStringLiteral("policy")).toObject();
            return policy.value(QStringLiteral("revision")).toInt() ==
                    initialPolicyRevision + 1 &&
                policy.value(QStringLiteral("global_playback")).toBool() &&
                policy.value(QStringLiteral("metronome_state")).toBool();
        }, states)) return 1;

    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("performance.metronome")},
            {QStringLiteral("id"), QStringLiteral("metronome-baseline-off")},
            {QStringLiteral("enabled"), false},
            {QStringLiteral("bpm"), 173},
        }) || !receive(coordinator.peer(0), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("known shared metronome off baseline"),
        [](std::size_t, const QJsonObject& state) {
            const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
            return !performance.value(QStringLiteral("metronome_enabled")).toBool() &&
                performance.value(QStringLiteral("metronome_bpm")).toInt() == 173 &&
                performance.value(QStringLiteral("tempo_view_bpm")).toInt() == 173;
        }, states)) return 1;

    if (!send(coordinator.peer(1), {
            {QStringLiteral("type"), QStringLiteral("performance.metronome")},
            {QStringLiteral("id"), QStringLiteral("metronome-on")},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("bpm"), 173},
        }) || !receive(coordinator.peer(1), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("shared metronome enable and tempo"),
        [](std::size_t, const QJsonObject& state) {
            const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
            return performance.value(QStringLiteral("metronome_enabled")).toBool() &&
                performance.value(QStringLiteral("metronome_bpm")).toInt() == 173 &&
                performance.value(QStringLiteral("tempo_view_bpm")).toInt() == 173 &&
                performance.value(QStringLiteral("metronome_transport_gated")).toBool();
        }, states)) return 1;

    std::array<qint64, FourPeerCoordinator::kPeerCount> playRevisions{};
    std::array<qint64, FourPeerCoordinator::kPeerCount> playCommits{};
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject performance = states[peer]
            .value(QStringLiteral("performance")).toObject();
        playRevisions[peer] = performance.value(
            QStringLiteral("transport_revision")).toInteger();
        playCommits[peer] = performance.value(
            QStringLiteral("transport_commit_count")).toInteger();
    }
    if (!send(coordinator.peer(3), {
            {QStringLiteral("type"), QStringLiteral("performance.transport")},
            {QStringLiteral("id"), QStringLiteral("transport-play")},
            {QStringLiteral("action"), QStringLiteral("play")},
        }) || !receive(coordinator.peer(3), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("four-peer global playback start"),
        [&playRevisions, &playCommits](std::size_t peer, const QJsonObject& state) {
            const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
            return performance.value(QStringLiteral("transport_revision")).toInteger() >
                    playRevisions[peer] &&
                performance.value(QStringLiteral("transport_commit_count")).toInteger() >
                    playCommits[peer] &&
                performance.value(QStringLiteral("transport_action")).toString() ==
                    QStringLiteral("restart") &&
                performance.value(QStringLiteral("global_transport_requested_playing")).toBool() &&
                performance.value(QStringLiteral("global_transport_playing")).toBool() &&
                performance.value(QStringLiteral("transport_playback_active")).toBool() &&
                performance.value(QStringLiteral("metronome_peak_ppm")).toInt() > 0;
        }, states)) return 1;

    std::array<qint64, FourPeerCoordinator::kPeerCount> bankLaunchEpochs{};
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject performance = states[peer]
            .value(QStringLiteral("performance")).toObject();
        bankLaunchEpochs[peer] = performance.value(
            QStringLiteral("metronome_epoch_frame")).toInteger();
        if (!performance.value(QStringLiteral("metronome_epoch_valid")).toBool()) {
            fail(QStringLiteral("peer %1 lacked an epoch before shared Section launch")
                .arg(peer + 1));
        }
    }
    if (failures != 0 || !invoke(
            coordinator.peer(2),
            QStringLiteral("joiner-queue-section-b"),
            QStringLiteral("performance.home.section.queue.1"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("joiner-requested four-peer Section B launch"),
        [&bankLaunchEpochs](std::size_t peer, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            const QJsonObject performance = state.value(
                QStringLiteral("performance")).toObject();
            return content.value(QStringLiteral("active_bank")).toInt(-1) == 1 &&
                content.value(QStringLiteral("pending_bank")).toInt(-2) == -1 &&
                content.value(QStringLiteral("pending_bank_absolute_beat")).toString() ==
                    QStringLiteral("0") &&
                !content.value(QStringLiteral("shared_bank_launch_active")).toBool() &&
                content.value(QStringLiteral("shared_bank_switch_id")).toString().isEmpty() &&
                content.value(QStringLiteral("shared_bank_expected_peers")).toArray().isEmpty() &&
                content.value(QStringLiteral("shared_bank_ready_peers")).toArray().isEmpty() &&
                performance.value(QStringLiteral("global_transport_requested_playing")).toBool() &&
                performance.value(QStringLiteral("global_transport_playing")).toBool() &&
                performance.value(QStringLiteral("metronome_epoch_valid")).toBool() &&
                performance.value(QStringLiteral("metronome_epoch_frame")).toInteger() ==
                    bankLaunchEpochs[peer];
        }, states)) return 1;

    if (!invoke(
            coordinator.peer(0),
            QStringLiteral("creator-queue-section-a"),
            QStringLiteral("performance.home.section.queue.0"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("creator-requested four-peer Section A restoration"),
        [&bankLaunchEpochs, baselineActiveBankLaneCount, baselineActiveBankLaneIds](
            std::size_t peer, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            const QJsonObject performance = state.value(
                QStringLiteral("performance")).toObject();
            return content.value(QStringLiteral("active_bank")).toInt(-1) == 0 &&
                content.value(QStringLiteral("pending_bank")).toInt(-2) == -1 &&
                !content.value(QStringLiteral("shared_bank_launch_active")).toBool() &&
                content.value(QStringLiteral("active_bank_lane_count")).toInt(-1) ==
                    baselineActiveBankLaneCount &&
                content.value(QStringLiteral("active_bank_lane_ids")).toArray() ==
                    baselineActiveBankLaneIds &&
                performance.value(QStringLiteral("global_transport_playing")).toBool() &&
                performance.value(QStringLiteral("metronome_epoch_frame")).toInteger() ==
                    bankLaunchEpochs[peer];
        }, states)) return 1;

    constexpr std::size_t sharedRecordingPeer = 1;
    constexpr std::size_t recordingLockObserver = 2;
    const int initialLaneCount = states[0].value(QStringLiteral("content"))
        .toObject().value(QStringLiteral("lane_count")).toInt(-1);
    if (initialLaneCount != baselineLaneCount) {
        fail(QStringLiteral("shared recording fixture did not preserve its baseline lanes"));
        return 1;
    }
    auto& sharedRecorder = coordinator.peer(sharedRecordingPeer);
    const QString sharedRecordingLane = QStringLiteral("looper.lane.%1")
        .arg(baselineActiveBankLaneCount);
    if (!invoke(sharedRecorder,
            QStringLiteral("shared-recording-add-lane"),
            QStringLiteral("looper.lane.add-empty"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("shared recording lane convergence"),
        [baselineLaneCount, baselineActiveBankLaneCount](
            std::size_t, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            return content.value(QStringLiteral("lane_count")).toInt() ==
                    baselineLaneCount + 1 &&
                content.value(QStringLiteral("active_bank_lane_count")).toInt() ==
                    baselineActiveBankLaneCount + 1;
        }, states)) return 1;
    const QJsonArray recordingLaneIds = states[sharedRecordingPeer]
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("active_bank_lane_ids")).toArray();
    if (recordingLaneIds.size() <= baselineActiveBankLaneCount) {
        fail(QStringLiteral("shared recording lane convergence omitted the appended lane identity"));
        return 1;
    }
    const QString sharedRecordingLaneId = recordingLaneIds
        .at(baselineActiveBankLaneCount).toString();
    if (sharedRecordingLaneId.isEmpty()) {
        fail(QStringLiteral("shared recording lane identity was empty"));
        return 1;
    }

    if (!invoke(sharedRecorder,
            QStringLiteral("shared-recording-arm-open"),
            sharedRecordingLane + QStringLiteral(".arm"),
            QStringLiteral("click-async")) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-input-mode"),
            QStringLiteral("looper.recording-dialog.mode"),
            QStringLiteral("set-index"), 0) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-arm"),
            QStringLiteral("looper.recording-dialog.arm"),
            QStringLiteral("click")) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-manual-stop"),
            QStringLiteral("looper.recording.manual-stop"),
            QStringLiteral("set-checked"), true) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-no-count-in"),
            QStringLiteral("looper.recording.count-in"),
            QStringLiteral("set-checked"), false) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-ready"),
            QStringLiteral("looper.recording.start-stop"),
            QStringLiteral("click"))) return 1;

    if (!waitForAll(coordinator, QStringLiteral("genuine shared lane recording group"),
        [sharedRecordingPeer](std::size_t peer, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            const QString groupId = content.value(
                QStringLiteral("lane_recording_active_group_id")).toString();
            if (groupId.isEmpty() ||
                !content.value(QStringLiteral("lane_recording_protected")).toBool() ||
                !content.value(QStringLiteral("lane_recording_isolation_active")).toBool() ||
                content.value(QStringLiteral("lane_recording_group_participants"))
                    .toArray().size() != 1) {
                return false;
            }
            if (peer == sharedRecordingPeer) {
                return content.value(QStringLiteral("lane_recording_local_phase"))
                    .toString() == QStringLiteral("recording");
            }
            const QJsonArray remote = content.value(
                QStringLiteral("lane_recording_peer_states")).toArray();
            return remote.size() == 1 && remote.first().toObject()
                .value(QStringLiteral("phase")).toString() == QStringLiteral("recording");
        }, states)) return 1;
    const QString sharedRecordingGroup = states[0]
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("lane_recording_active_group_id")).toString();
    for (std::size_t peer = 1; peer < states.size(); ++peer) {
        if (states[peer].value(QStringLiteral("content")).toObject()
                .value(QStringLiteral("lane_recording_active_group_id")).toString() !=
            sharedRecordingGroup) {
            fail(QStringLiteral("four peers did not expose one shared recording group"));
        }
    }
    if (failures != 0) return 1;

    auto& lockObserver = coordinator.peer(recordingLockObserver);
    if (!invoke(lockObserver,
            QStringLiteral("recording-lock-jam-sync-open"),
            QStringLiteral("session.jam-sync"),
            QStringLiteral("click-async"))) return 1;
    const std::array<QString, 7> lockedPolicyControls{
        QStringLiteral("session.jam-sync-dialog.track-lanes"),
        QStringLiteral("session.jam-sync-dialog.automatic-wavs"),
        QStringLiteral("session.jam-sync-dialog.generated-ideas"),
        QStringLiteral("session.jam-sync-dialog.global-playback"),
        QStringLiteral("session.jam-sync-dialog.metronome-state"),
        QStringLiteral("session.jam-sync-dialog.recordings"),
        QStringLiteral("session.jam-sync-dialog.apply"),
    };
    for (std::size_t control = 0; control < lockedPolicyControls.size(); ++control) {
        const auto state = controlState(
            lockObserver,
            lockedPolicyControls[control],
            QStringLiteral("recording-lock-control-%1").arg(control));
        if (!state || state->value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral("shared recording did not lock Jam Sync control ") +
                lockedPolicyControls[control]);
        }
    }
    const auto cancelState = controlState(
        lockObserver,
        QStringLiteral("session.jam-sync-dialog.cancel"),
        QStringLiteral("recording-lock-cancel-state"));
    if (!cancelState || !cancelState->value(QStringLiteral("enabled")).toBool()) {
        fail(QStringLiteral("shared recording lock also disabled Jam Sync Cancel"));
    }
    if (failures != 0 || !invoke(lockObserver,
            QStringLiteral("recording-lock-jam-sync-cancel"),
            QStringLiteral("session.jam-sync-dialog.cancel"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("recording import worker baseline"),
        [sharedRecordingPeer](std::size_t peer, const QJsonObject& state) {
            return peer != sharedRecordingPeer || state.value(
                QStringLiteral("content")).toObject()
                    .value(QStringLiteral("file_tasks_active")).toInt(-1) == 0;
        }, states)) return 1;
    if (!send(sharedRecorder, {
            {QStringLiteral("type"), QStringLiteral("looper.file-workers.hold")},
            {QStringLiteral("id"), QStringLiteral("recording-import-worker-hold")},
            {QStringLiteral("milliseconds"), 2000},
        }) || !receive(sharedRecorder, QStringLiteral("command_applied")) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-stop"),
            QStringLiteral("looper.recording.start-stop"),
            QStringLiteral("click"))) return 1;

    if (!waitForAll(coordinator, QStringLiteral("shared lane WAV finalization and convergence"),
        [baselineLaneCount, sharedRecordingLaneId, sharedRecordingPeer](
            std::size_t peer, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            const QJsonArray laneIds = content.value(QStringLiteral("lane_ids")).toArray();
            const QJsonArray hashes = content.value(QStringLiteral("asset_hashes")).toArray();
            const QJsonArray available = content.value(
                QStringLiteral("asset_available")).toArray();
            const QJsonArray bytes = content.value(QStringLiteral("asset_bytes")).toArray();
            const QJsonObject performance = state.value(
                QStringLiteral("performance")).toObject();
            const int assetIndex = jsonStringIndex(laneIds, sharedRecordingLaneId);
            return !content.value(QStringLiteral("lane_recording_protected")).toBool() &&
                !content.value(QStringLiteral("lane_recording_isolation_active")).toBool() &&
                content.value(QStringLiteral("lane_recording_active_group_id"))
                    .toString().isEmpty() &&
                content.value(QStringLiteral("lane_recording_local_phase")).toString() ==
                    QStringLiteral("idle") &&
                content.value(QStringLiteral("lane_count")).toInt() ==
                    baselineLaneCount + 1 &&
                assetIndex >= 0 && assetIndex < hashes.size() &&
                assetIndex < available.size() && assetIndex < bytes.size() &&
                hashes.at(assetIndex).toString().size() == 64 &&
                available.at(assetIndex).toBool() &&
                bytes.at(assetIndex).toInteger() > 44 &&
                content.value(QStringLiteral("lane_recording_import_failures"))
                    .toInteger() == 0 &&
                (peer != sharedRecordingPeer || content.value(
                    QStringLiteral("lane_recording_import_busy_retries")).toInteger() > 0) &&
                performance.value(QStringLiteral("global_transport_playing")).toBool();
        }, states, 40s)) return 1;
    const QJsonObject sharedTake = states[0]
        .value(QStringLiteral("content")).toObject();
    const int sharedTakeIndex = jsonStringIndex(
        sharedTake.value(QStringLiteral("lane_ids")).toArray(), sharedRecordingLaneId);
    if (sharedTakeIndex < 0) {
        fail(QStringLiteral("finalized shared take omitted its lane identity"));
        return 1;
    }
    const QString sharedTakeHash = sharedTake.value(QStringLiteral("asset_hashes"))
        .toArray().at(sharedTakeIndex).toString();
    const qint64 sharedTakeSize = sharedTake.value(QStringLiteral("asset_bytes"))
        .toArray().at(sharedTakeIndex).toInteger();
    for (std::size_t peer = 1; peer < states.size(); ++peer) {
        const QJsonObject content = states[peer]
            .value(QStringLiteral("content")).toObject();
        const int assetIndex = jsonStringIndex(
            content.value(QStringLiteral("lane_ids")).toArray(), sharedRecordingLaneId);
        if (assetIndex < 0 || content.value(QStringLiteral("asset_hashes"))
                .toArray().at(assetIndex).toString() != sharedTakeHash ||
            content.value(QStringLiteral("asset_bytes"))
                .toArray().at(assetIndex).toInteger() != sharedTakeSize) {
            fail(QStringLiteral("shared lane take did not converge to exact WAV content"));
        }
    }
    if (failures != 0 || !waitForAll(
            coordinator,
            QStringLiteral("four prepared tracks before loop controls"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                return content.value(QStringLiteral("track_duration_ms")).toInt() > 1 &&
                    content.value(QStringLiteral("track_file_available")).toBool() &&
                    content.value(QStringLiteral("prepared_mix_frames")).toInteger() > 1 &&
                    content.value(QStringLiteral("prepared_mix_sample_rate")).toInt() > 0 &&
                    !content.value(QStringLiteral("prepared_mix_worker_running")).toBool() &&
                    content.value(QStringLiteral("prepared_mix_failures")).toInteger() == 0;
            },
            states)) return 1;
    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("performance.transport")},
            {QStringLiteral("id"), QStringLiteral("loop-fixture-stop")},
            {QStringLiteral("action"), QStringLiteral("stop")},
        }) || !receive(coordinator.peer(0), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("four stopped prepared loop fixtures"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject performance = state.value(
                    QStringLiteral("performance")).toObject();
                return !performance.value(QStringLiteral(
                        "global_transport_requested_playing")).toBool() &&
                    !performance.value(QStringLiteral(
                        "global_transport_playing")).toBool() &&
                    !performance.value(QStringLiteral("prepared_source_playing")).toBool();
            }, states)) return 1;
    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("performance.transport")},
            {QStringLiteral("id"), QStringLiteral("loop-fixture-play")},
            {QStringLiteral("action"), QStringLiteral("play")},
        }) || !receive(coordinator.peer(0), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("four playing prepared loop fixtures"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                const QJsonObject performance = state.value(
                    QStringLiteral("performance")).toObject();
                return performance.value(QStringLiteral(
                        "global_transport_requested_playing")).toBool() &&
                    performance.value(QStringLiteral(
                        "global_transport_playing")).toBool() &&
                    performance.value(QStringLiteral("prepared_source_playing")).toBool() &&
                    performance.value(QStringLiteral("prepared_source_busy_events"))
                        .toInteger() == 0 &&
                    content.value(QStringLiteral("prepared_mix_failures")).toInteger() == 0;
            }, states)) return 1;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!invoke(
                coordinator.peer(peer),
                QStringLiteral("loop-clear-%1").arg(peer + 1),
                QStringLiteral("looper.loop.clear"),
                QStringLiteral("click"))) return 1;
    }
    if (!waitForAll(coordinator, QStringLiteral("four cleared local track loops"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                return !content.value(QStringLiteral("track_loop_enabled")).toBool() &&
                    content.value(QStringLiteral("track_loop_start_seconds"))
                        .toDouble(-2.0) == -1.0 &&
                    content.value(QStringLiteral("track_loop_end_seconds"))
                        .toDouble(-2.0) == -1.0 &&
                    !content.value(QStringLiteral("track_loop_effective_enabled"))
                        .toBool();
            }, states)) return 1;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!invoke(
                coordinator.peer(peer),
                QStringLiteral("loop-start-%1").arg(peer + 1),
                QStringLiteral("looper.loop.start"),
                QStringLiteral("click"))) return 1;
    }
    if (!waitForAll(coordinator, QStringLiteral("four bounded local loop starts"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                const double start = content.value(
                    QStringLiteral("track_loop_start_seconds")).toDouble(-2.0);
                return content.value(QStringLiteral("track_loop_enabled")).toBool() &&
                    start >= 0.0 &&
                    start * 1000.0 < content.value(
                        QStringLiteral("track_duration_ms")).toInt() &&
                    content.value(QStringLiteral("track_loop_end_seconds"))
                        .toDouble(-2.0) == -1.0 &&
                    content.value(QStringLiteral("track_loop_effective_enabled"))
                        .toBool() &&
                    content.value(QStringLiteral("track_loop_effective_end_frame"))
                        .toInteger() > content.value(
                            QStringLiteral("track_loop_effective_start_frame"))
                            .toInteger();
            }, states)) return 1;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!invoke(
                coordinator.peer(peer),
                QStringLiteral("loop-end-%1").arg(peer + 1),
                QStringLiteral("looper.loop.end"),
                QStringLiteral("click"))) return 1;
    }
    if (!waitForAll(coordinator, QStringLiteral("four canonical local loop ends"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                const double start = content.value(
                    QStringLiteral("track_loop_start_seconds")).toDouble(-2.0);
                const double end = content.value(
                    QStringLiteral("track_loop_end_seconds")).toDouble(-2.0);
                const qint64 effectiveStart = content.value(
                    QStringLiteral("track_loop_effective_start_frame")).toInteger(-1);
                const qint64 effectiveEnd = content.value(
                    QStringLiteral("track_loop_effective_end_frame")).toInteger(-1);
                return content.value(QStringLiteral("track_loop_enabled")).toBool() &&
                    end > 0.0 && (start < 0.0 || end > start) &&
                    content.value(QStringLiteral("track_loop_effective_enabled"))
                        .toBool() && effectiveStart >= 0 && effectiveEnd > effectiveStart &&
                    effectiveEnd <= content.value(
                        QStringLiteral("prepared_mix_frames")).toInteger();
            }, states)) return 1;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!invoke(
                coordinator.peer(peer),
                QStringLiteral("loop-disable-%1").arg(peer + 1),
                QStringLiteral("looper.loop.enabled"),
                QStringLiteral("set-checked"), false)) return 1;
    }
    if (!waitForAll(coordinator, QStringLiteral("four disabled local loop regions"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                return !content.value(QStringLiteral("track_loop_enabled")).toBool() &&
                    !content.value(QStringLiteral("track_loop_effective_enabled")).toBool();
            }, states)) return 1;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!invoke(
                coordinator.peer(peer),
                QStringLiteral("loop-enable-%1").arg(peer + 1),
                QStringLiteral("looper.loop.enabled"),
                QStringLiteral("set-checked"), true) ||
            !invoke(
                coordinator.peer(peer),
                QStringLiteral("loop-final-clear-%1").arg(peer + 1),
                QStringLiteral("looper.loop.clear"),
                QStringLiteral("click"))) return 1;
    }
    if (!waitForAll(coordinator, QStringLiteral("four cleared final local loop regions"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(
                    QStringLiteral("content")).toObject();
                return !content.value(QStringLiteral("track_loop_enabled")).toBool() &&
                    content.value(QStringLiteral("track_loop_start_seconds"))
                        .toDouble(-2.0) == -1.0 &&
                    content.value(QStringLiteral("track_loop_end_seconds"))
                        .toDouble(-2.0) == -1.0;
            }, states)) return 1;
    if (failures != 0 || !invoke(sharedRecorder,
            QStringLiteral("shared-recording-remove-open"),
            sharedRecordingLane + QStringLiteral(".remove"),
            QStringLiteral("click-async")) ||
        !invoke(sharedRecorder,
            QStringLiteral("shared-recording-remove"),
            QStringLiteral("looper.lane-remove-dialog.remove-only"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("shared recording fixture restoration"),
        [baselineLaneCount, baselineActiveBankLaneCount, baselineActiveBankLaneIds](
            std::size_t, const QJsonObject& state) {
            const QJsonObject content = state.value(QStringLiteral("content")).toObject();
            return content.value(QStringLiteral("lane_count")).toInt() ==
                    baselineLaneCount &&
                content.value(QStringLiteral("active_bank_lane_count")).toInt() ==
                    baselineActiveBankLaneCount &&
                content.value(QStringLiteral("active_bank_lane_ids")).toArray() ==
                    baselineActiveBankLaneIds;
        }, states)) return 1;

    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!send(coordinator.peer(peer), {
                {QStringLiteral("type"), QStringLiteral("performance.recording")},
                {QStringLiteral("id"), QStringLiteral("recording-start-%1").arg(peer + 1)},
                {QStringLiteral("action"), QStringLiteral("start")},
            }) || !receive(coordinator.peer(peer), QStringLiteral("command_applied"))) {
            return 1;
        }
    }
    if (!waitForAll(coordinator, QStringLiteral("four active jam recordings"),
        [](std::size_t, const QJsonObject& state) {
            const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
            const QString folder = performance.value(
                QStringLiteral("jam_recording_folder")).toString();
            return performance.value(QStringLiteral("jam_recording_active")).toBool() &&
                performance.value(QStringLiteral("jam_recording_workflow_active")).toBool() &&
                performance.value(QStringLiteral("jam_recording_view_enabled")).toBool() &&
                performance.value(QStringLiteral("jam_recording_view_active")).toBool() &&
                !folder.isEmpty() &&
                performance.value(QStringLiteral("jam_recording_view_take")).toString() ==
                    QFileInfo(folder).fileName() &&
                performance.value(QStringLiteral("jam_recording_frames_written")).toInteger() >=
                    kJamRecordingSignalProofFrames &&
                performance.value(QStringLiteral("jam_recording_dropped_frames")).toInteger() == 0 &&
                performance.value(QStringLiteral("jam_recording_writer_errors")).toInteger() == 0;
        }, states)) return 1;

    std::array<QString, FourPeerCoordinator::kPeerCount> recordingFolders;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        recordingFolders[peer] = states[peer].value(QStringLiteral("performance")).toObject()
            .value(QStringLiteral("jam_recording_folder")).toString();
        const QString root = QDir::cleanPath(coordinator.storageRoot(peer));
        const QString folder = QDir::cleanPath(recordingFolders[peer]);
        if (!folder.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) {
            fail(QStringLiteral("peer %1 recording escaped its isolated storage root: %2")
                .arg(peer + 1).arg(folder));
            return 1;
        }
    }
    for (std::size_t left = 0; left < recordingFolders.size(); ++left) {
        for (std::size_t right = left + 1; right < recordingFolders.size(); ++right) {
            if (QDir::cleanPath(recordingFolders[left]).compare(
                    QDir::cleanPath(recordingFolders[right]), Qt::CaseInsensitive) == 0) {
                fail(QStringLiteral("two peers wrote to the same recording folder"));
                return 1;
            }
        }
    }

    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (!send(coordinator.peer(peer), {
                {QStringLiteral("type"), QStringLiteral("performance.recording")},
                {QStringLiteral("id"), QStringLiteral("recording-stop-%1").arg(peer + 1)},
                {QStringLiteral("action"), QStringLiteral("stop")},
            }) || !receive(coordinator.peer(peer), QStringLiteral("command_applied"))) {
            return 1;
        }
    }
    if (!waitForAll(coordinator, QStringLiteral("four finalized jam recordings"),
        [](std::size_t, const QJsonObject& state) {
            const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
            const qint64 queued = performance.value(
                QStringLiteral("jam_recording_frames_queued")).toInteger();
            const qint64 written = performance.value(
                QStringLiteral("jam_recording_frames_written")).toInteger();
            return !performance.value(QStringLiteral("jam_recording_active")).toBool() &&
                !performance.value(QStringLiteral("jam_recording_workflow_active")).toBool() &&
                performance.value(QStringLiteral("jam_recording_view_enabled")).toBool() &&
                !performance.value(QStringLiteral("jam_recording_view_active")).toBool() &&
                queued > 4800 && written == queued &&
                performance.value(QStringLiteral("jam_recording_dropped_frames")).toInteger() == 0 &&
                performance.value(QStringLiteral("jam_recording_drop_events")).toInteger() == 0 &&
                performance.value(QStringLiteral("jam_recording_writer_errors")).toInteger() == 0;
        }, states)) return 1;

    const std::array<QString, 5> stemNames{
        QStringLiteral("mix.wav"),
        QStringLiteral("my-input.wav"),
        QStringLiteral("their-input.wav"),
        QStringLiteral("inputs-mix.wav"),
        QStringLiteral("metronome.wav"),
    };
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const qint64 frames = states[peer].value(QStringLiteral("performance")).toObject()
            .value(QStringLiteral("jam_recording_frames_written")).toInteger();
        for (const QString& stem : stemNames) {
            QString wavError;
            if (!verifyRecordedWav(
                    QDir(recordingFolders[peer]).absoluteFilePath(stem), frames, wavError)) {
                fail(QStringLiteral("peer %1: %2").arg(peer + 1).arg(wavError));
                return 1;
            }
        }
    }

    std::array<qint64, FourPeerCoordinator::kPeerCount> stopRevisions{};
    std::array<qint64, FourPeerCoordinator::kPeerCount> stopCommits{};
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject performance = states[peer]
            .value(QStringLiteral("performance")).toObject();
        stopRevisions[peer] = performance.value(
            QStringLiteral("transport_revision")).toInteger();
        stopCommits[peer] = performance.value(
            QStringLiteral("transport_commit_count")).toInteger();
    }
    if (!send(coordinator.peer(2), {
            {QStringLiteral("type"), QStringLiteral("performance.transport")},
            {QStringLiteral("id"), QStringLiteral("transport-stop")},
            {QStringLiteral("action"), QStringLiteral("stop")},
        }) || !receive(coordinator.peer(2), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("four-peer global playback stop"),
        [&stopRevisions, &stopCommits](std::size_t peer, const QJsonObject& state) {
            const QJsonObject performance = state.value(QStringLiteral("performance")).toObject();
            return performance.value(QStringLiteral("transport_revision")).toInteger() >
                    stopRevisions[peer] &&
                performance.value(QStringLiteral("transport_commit_count")).toInteger() >
                    stopCommits[peer] &&
                performance.value(QStringLiteral("transport_action")).toString() ==
                    QStringLiteral("stop") &&
                !performance.value(QStringLiteral("global_transport_requested_playing")).toBool() &&
                !performance.value(QStringLiteral("global_transport_playing")).toBool() &&
                !performance.value(QStringLiteral("transport_playback_active")).toBool();
        }, states)) return 1;

    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("performance.metronome")},
            {QStringLiteral("id"), QStringLiteral("metronome-off")},
            {QStringLiteral("enabled"), false},
            {QStringLiteral("bpm"), 173},
        }) || !receive(coordinator.peer(0), QStringLiteral("command_applied"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("shared metronome disable"),
        [](std::size_t, const QJsonObject& state) {
            return !state.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("metronome_enabled")).toBool();
        }, states)) return 1;

    (void)shutdown(coordinator);
    if (failures != 0) {
        std::cerr << failures << " four-peer performance checks failed\n";
        return 1;
    }
    coordinator.markSuccessful();
    std::cout << "four-peer fake-audio, metronome, and transport checks passed\n";
    return 0;
}
