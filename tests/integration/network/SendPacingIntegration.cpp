#include "ArtifactReader.hpp"
#include "audio_device.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QThread>
#include <QUdpSocket>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void require(bool okay, const QString& reason)
{ if (!okay) throw std::runtime_error(reason.toStdString()); }

void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush(),
        QStringLiteral("write pacing artifact: ") + path);
}

struct Device {
    QString label;
    int id = -1;
    int callback = 32;
    QString backend;
    QString input;
    int captureTargetMs = 20;
    bool physicalCapture = false;
};

struct Child {
    QProcess process;
    QString folder;
    ~Child() {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(5000); }
        }
    }
};

QJsonObject evidence(const Child& child, const Device& device, int count, int packet)
{
    QDirIterator paths(child.folder, {"jam2_stats*.csv"}, QDir::Files, QDirIterator::Subdirectories);
    require(paths.hasNext(), "pacing run did not emit CSV");
    const auto path = paths.next();
    jam2::test::CsvTable csv;
    QString error;
    require(csv.read(path, error), error);
    const auto final = csv.rowsOfType("final");
    require(final.size() == 1, "pacing child must finish with one final row");
    const auto last = final.front();
    require(csv.integer(last, "active_sample_rate") == 48000 &&
        csv.integer(last, "session_frames_per_packet") == packet &&
        csv.integer(last, "audio_device_error") == 0, "pacing device/session failed or changed contract");
    if (device.id >= 0) require(csv.value(last, "device_backend") == device.backend &&
        csv.value(last, "device_name") == device.label, "pacing test opened a different hardware endpoint");
    else require(csv.integer(last, "active_audio_buffer_frames") == device.callback,
        "pacing fake callback differs from requested publication size");
    QVector<qsizetype> active;
    for (auto row : csv.rowsOfType("periodic"))
        if (csv.integer(row, "network_active_peer_count") == count - 1 &&
            csv.number(row, "elapsed_ms") >= (device.physicalCapture ? 10000 : 4000)) active.push_back(row);
    require(active.size() >= 6, "insufficient fully connected pacing observations");
    const auto stop = csv.number(active.back(), "elapsed_ms") - 2000;
    while (!active.isEmpty() && csv.number(active.back(), "elapsed_ms") > stop) active.removeLast();
    require(active.size() >= 3, "insufficient steady pacing observations before teardown");
    const auto first = active.front(), end = active.back();
    QJsonObject result{{"device", device.label}, {"csv", path},
        {"window_begin_ms", csv.number(first, "elapsed_ms")},
        {"window_end_ms", csv.number(end, "elapsed_ms")}};
    for (const auto* key : {"send_interval_samples", "send_catchup_events", "sent_packets", "recv_packets",
            "audio_packet_gap_over_2x_count", "audio_packet_gap_over_4x_count", "sequence_lost",
            "playback_ring_underruns", "mix_live_tail_trimmed_frames", "adaptive_playback_padding_frames",
            "mix_receive_recovery_events", "wasapi_capture_underrun_frames", "wasapi_capture_overrun_frames",
            "wasapi_capture_discontinuities", "capture_ring_overrun_events"})
        result[key] = csv.number(end, key) - csv.number(first, key);
    require(result["send_interval_samples"].toDouble() > 1000 &&
        result["sent_packets"].toDouble() > 1000, "live sender timing must be populated through the public runtime");
    require(result["wasapi_capture_overrun_frames"].toDouble() == 0 &&
        result["capture_ring_overrun_events"].toDouble() == 0, "pacing run overflowed capture");
    // These are cumulative high waters/means at the end of the observation,
    // not reconstructed per-window extremes or per-packet distributions.
    for (const auto* key : {"send_interval_min_ms", "send_interval_avg_ms", "send_interval_max_ms",
            "send_catchup_max_packets", "jitter_avg_ms", "audio_packet_gap_max_ms", "receive_loop_gap_max_ms",
            "audio_callback_interval_avg_ms", "audio_callback_interval_max_ms", "audio_callback_work_max_us",
            "active_audio_buffer_frames", "maximum_audio_callback_frames", "wasapi_capture_resets"})
        result[key] = csv.number(end, key);
    result["capture_clock_packet_pacing_active"] = csv.yes(end, "capture_clock_packet_pacing_active");
    result["physical_capture"] = device.physicalCapture;
    result["capture_target_ms"] = device.captureTargetMs;
    if (device.physicalCapture) {
        // Snapshot distributions, not a per-sample acoustic latency measurement.
        for (const auto* key : {"driver_input_latency_frames", "wasapi_capture_depth_frames",
                "wasapi_capture_correction_ppm"}) {
            require(csv.hasColumn(key), QStringLiteral("missing capture measurement: ") + key);
            std::vector<double> values;
            for (const auto row : active) values.push_back(csv.number(row, key));
            std::sort(values.begin(), values.end());
            result[QString::fromLatin1(key) + "_min"] = values.front();
            result[QString::fromLatin1(key) + "_median"] = values[values.size() / 2];
            result[QString::fromLatin1(key) + "_p95"] = values[(values.size() - 1) * 95 / 100];
            result[QString::fromLatin1(key) + "_max"] = values.back();
        }
        result["capture_snapshot_count"] = static_cast<int>(active.size());
    }
    // Device configuration is emitted only on the final CSV row.
    result["active_audio_buffer_frames"] = csv.number(last, "active_audio_buffer_frames");
    return result;
}

QJsonObject run(const QString& executable, const QString& root, const QString& label,
    const std::vector<Device>& devices, int packet, int duration)
{
    const auto folder = QDir(root).filePath(label);
    require(QDir().mkpath(folder), "create pacing case folder");
    QUdpSocket reserve;
    require(reserve.bind(QHostAddress::LocalHost, 0), "reserve pacing UDP port");
    const auto port = reserve.localPort(); reserve.close();
    const QString session = "13579bdf02468ace", key = "0123456789abcdef0123456789abcdef";
    const auto invite = QStringLiteral("jam2://v1?endpoint=127.0.0.1:%1&session=%2&key=%3").arg(port).arg(session, key);
    QJsonArray commands;
    std::vector<std::unique_ptr<Child>> children;
    const char* names[] = {"Sender", "PeerB", "PeerC", "PeerD"};
    require(devices.size() >= 2 && devices.size() <= 4, "bounded pacing participant count");
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        auto child = std::make_unique<Child>();
        child->folder = QDir(folder).filePath(QStringLiteral("peer-%1").arg(i));
        require(QDir().mkpath(child->folder), "create pacing peer folder");
        QStringList args{"network", i == 0 ? "create" : "join"};
        if (i == 0) args << "--connection-strategy" << "direct" << "--no-stun" << "--bind" <<
            QStringLiteral("127.0.0.1:%1").arg(port) << "--public-endpoint" << QStringLiteral("127.0.0.1:%1").arg(port) <<
            "--session-id" << session << "--session-key" << key << "--max-peers" << QString::number(devices.size());
        else args << invite << "--bind" << "127.0.0.1:0";
        args << "--name" << names[i] << "--profile" << "fast" << "--sample-rate" << "48000" <<
            "--frame-size" << QString::number(packet) << "--network-audio-format" << "pcm16" <<
            "--audio-buffer-size" << QString::number(device.callback) << "--test-input" <<
            (device.physicalCapture ? "off" : "tone-440") <<
            "--metronome" << "off" << "--local-monitor" << "off" << "--remote-level" << "0" <<
            "--stream-ms" << QString::number(duration + (i == 0 ? 2000 : 0)) << "--stream-linger-ms" << "0" <<
            "--wait-ms" << "15000" << "--stats" << "enabled" << "--stats-warmup-ms" << "3000" <<
            "--stats-interval-ms" << "500" << "--log-stats" << child->folder;
        if (device.id < 0) args << "--headless-audio" << "on";
        else args << "--audio-device" << QString::number(device.id);
        if (device.backend == "WASAPI") args << "--wasapi-input" << (device.input.isEmpty() ? "none" : device.input) <<
            "--wasapi-capture-target-ms" << QString::number(device.captureTargetMs) <<
            "--wasapi-capture-capacity-ms" << QString::number(std::max(200, 2 * device.captureTargetMs));
        commands.append(QJsonArray::fromStringList(args));
        child->process.setStandardOutputFile(QDir(child->folder).filePath("stdout.log"));
        child->process.setStandardErrorFile(QDir(child->folder).filePath("stderr.log"));
        child->process.start(executable, args);
        require(child->process.waitForStarted(5000), "start public pacing executable");
        children.push_back(std::move(child));
        if (i == 0) QThread::msleep(750);
    }
    write(QDir(folder).filePath("commands.json"), QJsonDocument(commands).toJson());
    QElapsedTimer timer; timer.start();
    for (;;) {
        bool running = false;
        for (const auto& child : children) {
            child->process.waitForFinished(20);
            running |= child->process.state() != QProcess::NotRunning;
        }
        if (!running) break;
        require(timer.elapsed() < duration + 20000, "bounded pacing session did not stop");
    }
    QJsonArray peers;
    for (std::size_t i = 0; i < children.size(); ++i) {
        require(children[i]->process.exitStatus() == QProcess::NormalExit && children[i]->process.exitCode() == 0,
            "pacing child failed; inspect " + children[i]->folder);
        peers.append(evidence(*children[i], devices[i], static_cast<int>(devices.size()), packet));
    }
    QJsonObject result{{"case", label}, {"packet_frames", packet}, {"participants", static_cast<int>(devices.size())},
        {"peers", peers}, {"scope", "Application send dispatch before fan-out and receiver delivery; unshaped localhost; no NIC/WAN measurement"}};
    write(QDir(folder).filePath("summary.json"), QJsonDocument(result).toJson());
    std::cout << "SEND_PACING " << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
    return result;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool captureBuffer = argc > 2 && QString::fromLocal8Bit(argv[2]) == "--capture-buffer-hardware";
    const bool hardware = captureBuffer || (argc > 2 && QString::fromLocal8Bit(argv[2]) == "--hardware");
    const auto profilePath = qEnvironmentVariable("JAM2_SEND_PACING_HARDWARE_PROFILE");
    if (hardware && profilePath.isEmpty()) { std::cout << "Set JAM2_SEND_PACING_HARDWARE_PROFILE.\n"; return 77; }
    QString root;
    try {
        require(argc >= 2, "pacing test requires the public Release executable");
        root = QDir(qEnvironmentVariable("JAM2_SEND_PACING_ARTIFACT_ROOT",
            qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT", QDir::tempPath()))).filePath(
            "send-pacing-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss-zzz"));
        require(QDir().mkpath(root), "create retained pacing artifacts");
        const auto executable = QString::fromLocal8Bit(argv[1]);
        QFile binary(executable); require(binary.open(QIODevice::ReadOnly), "read pacing public executable identity");
        const auto sha = QCryptographicHash::hash(binary.readAll(), QCryptographicHash::Sha256).toHex();
        QJsonArray cases;
        if (!hardware) {
            for (const int packet : {64, 128}) for (const int callback : {32, 480}) {
                std::vector<Device> devices(4, Device{"synthetic-32", -1, 32, {}, {}});
                devices[0] = {QStringLiteral("synthetic-%1").arg(callback), -1, callback, {}, {}};
                // Windows can deliver the synthetic 32-frame clock slower than
                // its nominal period. Allow enough real observations without
                // treating that backend as a hardware clock-quality control.
                cases.append(run(executable, root, QStringLiteral("four-packet%1-callback%2").arg(packet).arg(callback), devices, packet, 20000));
            }
        } else {
            QJsonObject profile; QString error;
            require(jam2::test::readJsonObject(profilePath, profile, error), error);
            require(profile["schema"].toString() == "jam2-send-pacing-hardware-v1", "invalid explicit pacing hardware profile");
            std::vector<int> targets{20, 10, 5, 1};
            if (captureBuffer && profile.contains("capture_targets_ms")) {
                require(profile["capture_targets_ms"].isArray(), "capture targets must be an array");
                targets.clear();
                for (const auto& value : profile["capture_targets_ms"].toArray()) {
                    const int target = value.toInt(-1);
                    require(value.isDouble() && value.toDouble() == target && target >= 1 && target <= 500,
                        "capture target must be an integer from 1 to 500 ms");
                    targets.push_back(target);
                }
                require(!targets.empty() && targets.size() <= 4, "capture sweep requires one to four targets");
            }
            const auto available = jam2::audio::list_devices();
            for (const auto& item : profile["devices"].toArray()) {
                const auto value = item.toObject();
                Device device; device.backend = value["backend"].toString();
                const auto identity = value["id"].toString();
                for (const auto& entry : available) if (QString::fromStdString(jam2::audio::stable_device_id(entry)) == identity &&
                        QString::fromStdString(entry.backend) == device.backend) {
                    device.id = entry.id; device.label = QString::fromStdString(entry.name); break;
                }
                require(device.id >= 0, "profiled pacing endpoint unavailable: " + identity);
                device.callback = device.backend == "ASIO" ? 32 : 0;
                device.input = value["input_endpoint_id"].toString();
                if (captureBuffer) {
                    require(device.backend == "WASAPI" && !device.input.isEmpty(),
                        "capture-buffer profile requires WASAPI output and physical input");
                    bool inputFound = false;
                    for (const auto& entry : available) if (entry.endpoint_id == device.input.toStdString() &&
                            entry.direction == jam2::audio::EndpointDirection::Input && entry.backend == "WASAPI")
                        inputFound = true;
                    require(inputFound, "profiled physical capture endpoint unavailable");
                    device.physicalCapture = true;
                    for (const int target : targets) {
                        device.captureTargetMs = target;
                        cases.append(run(executable, root,
                            value["label"].toString() + QStringLiteral("-target%1ms").arg(target),
                            {device, Device{"synthetic-480", -1, 480, {}, {}}}, 128, 45000));
                    }
                } else for (const int packet : {64, 128}) cases.append(run(executable, root,
                    value["label"].toString() + QStringLiteral("-packet%1").arg(packet),
                    {device, Device{"synthetic-480", -1, 480, {}, {}}}, packet, 30000));
            }
            require(!cases.isEmpty(), "pacing profile must select hardware");
        }
        write(QDir(root).filePath("summary.json"), QJsonDocument(QJsonObject{{"sha256", QString::fromLatin1(sha)}, {"cases", cases}}).toJson());
        std::cout << "Artifacts: " << root.toStdString() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nArtifacts: " << root.toStdString() << '\n'; return 1;
    }
}
