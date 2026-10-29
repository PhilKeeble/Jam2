#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTemporaryDir>
#include <QUdpSocket>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr int sampleRate = 48000, packetFrames = 64, callbackFrames = 128;
constexpr int samplesPerRun = 8, warmupMs = 3000;
constexpr double medianBudgetMs = 1000.0 * packetFrames / sampleRate;
constexpr double tailBudgetMs = 1000.0 * callbackFrames / sampleRate;
// The GUI's RTT contribution and total are rendered to 0.1 ms.
constexpr double displayResolutionMs = 0.1;
void require(bool condition, const QString& reason)
{
    if (!condition) throw std::runtime_error(reason.toStdString());
}
QJsonObject command(AutomationProcess& peer, const QJsonObject& request, const QString& expected)
{
    QString error; QJsonObject event;
    require(peer.send(request, error) && peer.readEvent(event, 15s, error), error);
    require(event.value("event").toString() == expected, QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
    return event;
}
QJsonObject snapshot(AutomationProcess& peer)
{
    return command(peer, {{"type", "snapshot"}, {"id", "latency"}, {"control", "looper.listening-tools"}}, "snapshot");
}
struct Signaller {
    QTemporaryDir logDirectory;
    QProcess process;
    QString endpoint;
    explicit Signaller(const QString& executable)
    {
        QUdpSocket reserve;
        require(reserve.bind(QHostAddress::LocalHost, 0), "reserve local signaller port");
        endpoint = QStringLiteral("127.0.0.1:%1").arg(reserve.localPort()); reserve.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, {"--jam2-server", "--log-dir", logDirectory.path(), "--debug", "--listen", endpoint, "--allow-local"});
        require(process.waitForStarted(5000), "start real signaller: " + process.errorString());
    }
    ~Signaller()
    {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); }
    }
};
struct Measurement {
    double total = 0, network = 0, jitter = 0, mixer = 0, playback = 0, output = 0;
};
struct RunResult { std::vector<Measurement> values; std::uint64_t cpuNs = 0; double wallSeconds = 0; bool hasCpu = false; };
std::optional<std::uint64_t> processCpu(const FourPeerCoordinator& peers)
{
    std::uint64_t total = 0;
    for (std::size_t i=0; i<4; ++i) { const auto value = peers.peer(i).cpuTimeNs(); if (!value) return {}; total += *value; }
    return total;
}
std::vector<Measurement> incoming(const QString& html)
{
    // Read the exact production GUI breakdown, retaining frame counts rather
    // than comparing its rounded whole-millisecond latency pill.
    static const QRegularExpression pattern(QStringLiteral(
        "<b>[^<]+ incoming: ~([0-9.]+) ms</b><br>"
        "Network RTT/2 ([0-9.]+) ms \\(RTT [0-9.]+ ms\\)<br>"
        "Jitter buffer [0-9.]+ ms \\(([0-9]+) fr\\) \\| Mixer queue [0-9.]+ ms \\(([0-9]+) fr\\)<br>"
        "Playback ring [0-9.]+ ms \\(([0-9]+) fr\\) \\| Output [0-9.]+ ms \\(([0-9]+) fr, [^)]+\\)"));
    std::vector<Measurement> result;
    auto matches = pattern.globalMatch(html);
    while (matches.hasNext()) {
        const auto match = matches.next();
        Measurement value;
        value.total = match.captured(1).toDouble(); value.network = match.captured(2).toDouble();
        const auto milliseconds = [&](int field) { return match.captured(field).toDouble() * 1000.0 / sampleRate; };
        value.jitter = milliseconds(3); value.mixer = milliseconds(4);
        value.playback = milliseconds(5); value.output = milliseconds(6);
        const double reconstructed = value.network + value.jitter + value.mixer + value.playback + value.output;
        require(std::isfinite(value.total) && value.total >= 0 && std::abs(value.total - reconstructed) <= displayResolutionMs + 0.001,
            "GUI incoming latency does not match its measured component breakdown");
        require(match.captured(6).toInt() == callbackFrames, "headless output callback differs between modes");
        result.push_back(value);
    }
    return result;
}
void verifyContract(const QString& root)
{
    const QDir logs(QDir(root).filePath("logs"));
    const auto names = logs.entryList({"jam2_gui*.log"}, QDir::Files);
    require(!names.isEmpty(), "mode latency peer log is missing");
    QFile file(logs.filePath(names.back()));
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto text = QString::fromUtf8(file.readAll());
    require(text.contains("session contract protocol=2 format=pcm24-mono profile=fast sample_rate=48000 frame_size=64"),
        "mode comparison requires identical fast/48-kHz/64-frame/PCM24 session contracts");
}
RunResult run(FourPeerCoordinator& peers, const QString& executable,
    const Signaller& signaller, bool server, bool queued, int repetition, QFile& raw, QFile& snapshots, const QDir& detailsRoot)
{
    const char* modeName = !server ? "direct" : queued ? "server-queued" : "server";
    LoopbackPortReservations ports; QString error;
    require(ports.reserve(4, error), error);
    std::array<quint16, 4> local;
    std::array<QStringList, 4> arguments;
    for (int i = 0; i < 4; ++i) {
        local[i] = ports.port(i);
        arguments[i] = {"debug", "gui-agent", "--instance-id", QStringLiteral("mode-latency-%1-%2").arg(repetition).arg(i)};
    }
    require(peers.launch(executable, arguments, error), error);
    for (int i = 0; i < 4; ++i) {
        QJsonObject hello;
        require(peers.peer(i).readEvent(hello, 30s, error) && hello.value("event") == "hello", error);
    }
    ports.release();
    if (repetition == 0) command(peers.peer(0), {{"type", "jam.create"}, {QStringLiteral("name"), QStringLiteral("Alex")}, {"id", "invalid-diagnostics"},
        {"port", local[0]}, {"diagnostics", "invalid"}}, "command_rejected");
    if (repetition == 0) command(peers.peer(0), {{"type", "jam.create"}, {QStringLiteral("name"), QStringLiteral("Alex")}, {"id", "invalid-receive-model"},
        {"port", local[0]}, {"queued_server_receive", "invalid"}}, "command_rejected");
    QJsonObject create{{"type", "jam.create"}, {QStringLiteral("name"), QStringLiteral("Alex")}, {"id", "create"}, {"port", local[0]},
        {"test_input", "tone-440"}, {"strategy", server ? "server" : "direct"}, {"diagnostics", true}};
    if (server) create.insert("rendezvous", signaller.endpoint);
    if (queued) create.insert("queued_server_receive",true);
    const auto created = command(peers.peer(0), create, "command_applied");
    require(created.value("invite_url").toString().startsWith("jam2://") &&
        created.value("invite_url").toString().contains("strategy=server") == server,
        "invitation does not select the mode under comparison");
    for (int i = 1; i < 4; ++i) command(peers.peer(i), {{"type", "jam.join"}, {QStringLiteral("name"), QStringLiteral("Alex")}, {"id", "join"},
        {"port", local[i]}, {"invite_url", created.value("invite_url")}, {"test_input", "tone-440"}, {"diagnostics", true},
        {"queued_server_receive",queued}}, "command_applied");
    QElapsedTimer ready; ready.start();
    for (;;) {
        bool all = true;
        for (int i = 0; i < 4; ++i) {
            const auto state = snapshot(peers.peer(i));
            const auto jam = state.value("jam").toObject(); const auto performance = state.value("performance").toObject();
            all = all && jam.value("active_remote_peer_count").toInt() == 3 &&
                performance.value("network_playback_enabled").toBool() && performance.value("remote_peak_ppm").toInt() > 0 &&
                incoming(jam.value("incoming_audio_breakdown").toString()).size() == 3;
        }
        if (all) break;
        require(ready.elapsed() < 30000, "all twelve directed audio paths must become measurable");
        QThread::msleep(50);
    }
    // Joiners log the creator-owned contract they received; the creator does
    // not log receipt of its own contract. Check every receiving participant.
    for (int i = 1; i < 4; ++i) verifyContract(peers.storageRoot(i));
    QThread::msleep(warmupMs);
    RunResult result; auto& measurements = result.values;
    const auto cpuBefore = processCpu(peers); QElapsedTimer cpuWall; cpuWall.start();
    for (int sample = 0; sample < samplesPerRun; ++sample) {
        for (int i = 0; i < 4; ++i) {
            const auto state = snapshot(peers.peer(i)); const auto jam = state.value("jam").toObject();
            const auto performance = state.value("performance").toObject();
            const QJsonObject detail{{"mode", modeName}, {"run", repetition}, {"sample", sample}, {"receiver", i},
                {"performance", performance}, {"jam", jam}};
            const auto detailBytes = QJsonDocument(detail).toJson(QJsonDocument::Compact) + '\n';
            require(snapshots.write(detailBytes) == detailBytes.size(), "write raw engine/transport snapshots: " + snapshots.errorString());
            require(jam.value("active_remote_peer_count").toInt() == 3 && performance.value("headless_audio").toBool() &&
                performance.value("sample_rate").toInt() == sampleRate && performance.value("audio_buffer_frames").toInt() == callbackFrames,
                "audio profile, callback or four-peer topology changed during measurement");
            require(jam.value("udp_multiplex").toObject().value("enabled").toBool(),
                "latency measurement used the wrong transport mode");
            require(jam.value("grid_authority_beacon_interval_us").toInteger()==1000000,
                "authority beacon cadence leaked across connection modes");
            require(jam.value("grid_authority_phase_freshness_us").toInteger()==2500000,
                "authority phase validity leaked across connection modes");
            {
                const auto transport = jam.value("udp_multiplex").toObject();
                require(transport.value("runtime_receive_enabled").toBool() == !queued && transport.value("runtime_receive_owner").toBool() == !queued,
                    "latency measurement used the wrong socket-reader model");
                if (!queued) require(transport.value("runtime_native_delivered").toInteger() > 0,"runtime reader did not deliver native audio directly");
            }
            const auto values = incoming(jam.value("incoming_audio_breakdown").toString());
            require(values.size() == 3, "steady measurement lost a directed audio path");
            for (const auto& value : values) {
                measurements.push_back(value);
                const QJsonObject row{{"mode", modeName}, {"run", repetition}, {"sample", sample}, {"receiver", i},
                    {"total_ms", value.total}, {"network_ms", value.network}, {"jitter_ms", value.jitter}, {"mixer_ms", value.mixer},
                    {"playback_ms", value.playback}, {"output_ms", value.output}};
                const auto bytes = QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
                require(raw.write(bytes) == bytes.size(), "write raw latency measurements: " + raw.errorString());
                std::cout << "latency_sample " << bytes.constData();
            }
        }
        QThread::msleep(1000);
    }
    result.wallSeconds = cpuWall.nsecsElapsed()/1000000000.0;
    const auto cpuAfter = processCpu(peers);
    if (cpuBefore && cpuAfter) { result.hasCpu = true; result.cpuNs = *cpuAfter-*cpuBefore; }
#if defined(_WIN32)
    require(result.hasCpu,"cannot query native process CPU time");
#endif
    std::cout << "mode=" << modeName << " run=" << repetition << " process_cpu_ns=" << result.cpuNs << " wall_seconds=" << result.wallSeconds
        << " cpu_one_core_percent=" << (result.hasCpu ? result.cpuNs/result.wallSeconds/10000000.0 : -1) << '\n';
    for (int i = 0; i < 4; ++i) {
        QString shutdownError; int code = -1;
        command(peers.peer(i), {{"type", "shutdown"}, {"id", "shutdown"}}, "command_applied");
        QJsonObject stopped;
        require(peers.peer(i).readEvent(stopped, 30s, shutdownError) && stopped.value("event") == "shutdown", shutdownError);
        require(peers.peer(i).waitForExit(30s, code, shutdownError) && code == 0, shutdownError);
    }
    // Retain existing diagnostics before successful process roots are removed.
    // This adds observation only; audio settings and the mode budgets stay fixed.
    for (int i=0; i<4; ++i) {
        const QDir source(peers.storageRoot(i));
        const QString destination = detailsRoot.filePath(QStringLiteral("run-%1-%2/peer-%3")
            .arg(repetition).arg(modeName).arg(i));
        QDirIterator files(source.absolutePath(), {"*.csv","*.log","*.ini","*.json"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            const QString relative = source.relativeFilePath(path);
            require(!relative.startsWith(".."), "diagnostic artifact escaped peer storage root");
            const QString target = QDir(destination).filePath(relative);
            require(QDir().mkpath(QFileInfo(target).absolutePath()) && QFile::copy(path,target), "copy diagnostic artifact: " + relative);
        }
    }
    peers.markSuccessful();
    return result;
}
double quantile(std::vector<double> values, double q)
{
    require(!values.empty(), "latency sample set is empty");
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(std::ceil(q * static_cast<double>(values.size() - 1)))];
}
double summary(const std::vector<Measurement>& samples, double Measurement::*field, double q)
{
    std::vector<double> values; for (const auto& sample : samples) values.push_back(sample.*field);
    return quantile(std::move(values), q);
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 3 || (argc == 4 && QByteArrayView(argv[3]) == "--receive-models"), "requires release/jam2 and real Go signaller");
        Signaller signaller(QString::fromLocal8Bit(argv[2]));
        const QDir artifact(qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT", QDir::tempPath()));
        QFile raw(artifact.filePath("mode-audio-latency.jsonl"));
        require(raw.open(QIODevice::WriteOnly), raw.errorString());
        QFile snapshots(artifact.filePath("mode-audio-snapshots.jsonl"));
        require(snapshots.open(QIODevice::WriteOnly), snapshots.errorString());
        QTemporaryDir details(artifact.filePath("mode-audio-details-XXXXXX"));
        require(details.isValid(), "create retained mode diagnostic artifact directory");
        details.setAutoRemove(false);
        const QDir detailsRoot(details.path());
        std::cout << "diagnostic_artifacts=" << details.path().toStdString() << '\n';
        std::array<std::vector<Measurement>, 3> modes;
        std::array<std::uint64_t,3> cpuNs{}; std::array<double,3> wallSeconds{};
        // ABBA avoids always charging startup/thermal/scheduler drift to one mode.
        const std::vector<int> order = argc==4 ? std::vector<int>{0,2,1,1,2,0} : std::vector<int>{0,1,1,0};
        for (int repetition = 0; repetition < static_cast<int>(order.size()); ++repetition) {
            FourPeerCoordinator peers;
            const auto mode = order[repetition];
            auto values = run(peers, QString::fromLocal8Bit(argv[1]), signaller, mode!=0, mode==2, repetition, raw, snapshots, detailsRoot);
            auto& target = modes[mode]; target.insert(target.end(), values.values.begin(), values.values.end());
            cpuNs[mode] += values.cpuNs; wallSeconds[mode] += values.wallSeconds;
        }
        require(raw.flush(), "flush raw mode-latency measurements");
        require(snapshots.flush(), "flush raw engine/transport snapshots");
        for (int mode = 0; mode < (argc==4 ? 3 : 2); ++mode) {
            std::cout << "mode=" << (mode==0 ? "direct" : mode==1 ? "server" : "server-queued") << " paths=12 samples=" << modes[mode].size()
                << " median_total_ms=" << summary(modes[mode], &Measurement::total, 0.5)
                << " p95_total_ms=" << summary(modes[mode], &Measurement::total, 0.95)
                << " median_network_ms=" << summary(modes[mode], &Measurement::network, 0.5)
                << " median_jitter_ms=" << summary(modes[mode], &Measurement::jitter, 0.5)
                << " median_mixer_ms=" << summary(modes[mode], &Measurement::mixer, 0.5)
                << " median_playback_ms=" << summary(modes[mode], &Measurement::playback, 0.5)
                << " median_output_ms=" << summary(modes[mode], &Measurement::output, 0.5)
                << " cpu_one_core_percent=" << cpuNs[mode]/wallSeconds[mode]/10000000.0 << '\n';
        }
        const double medianDelta = summary(modes[1], &Measurement::total, 0.5) - summary(modes[0], &Measurement::total, 0.5);
        const double tailDelta = summary(modes[1], &Measurement::total, 0.95) - summary(modes[0], &Measurement::total, 0.95);
        std::cout << "server_minus_direct_median_ms=" << medianDelta << " median_budget_ms=" << medianBudgetMs
            << " server_minus_direct_p95_ms=" << tailDelta << " p95_budget_ms=" << tailBudgetMs << '\n';
        require(std::abs(medianDelta) <= medianBudgetMs + displayResolutionMs && std::abs(tailDelta) <= tailBudgetMs + displayResolutionMs,
            "mode latency differs by more than one packet at median or one callback at p95; inspect raw components");
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
