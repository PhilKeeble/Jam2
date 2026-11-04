#include "ApplicationRuntime.hpp"
#include "SharedSessionController.hpp"
#include "tuning_profile.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using Controller = SharedSessionController;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class Predicate> void until(Predicate ready, const char* reason, int timeout = 12000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1); QThread::msleep(1);
    }
    require(ready(), reason);
}
QString token(int index) { return QString(32, QChar('1' + index)); }
struct Server {
    QProcess process;
    QTemporaryDir logs;
    quint16 port;
    explicit Server(const QString& executable) {
        QUdpSocket reserve;
        require(reserve.bind(QHostAddress::LocalHost, 0), "reserve investigation signaller port");
        port = reserve.localPort(); reserve.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, {"--jam2-server", "--listen", QStringLiteral("127.0.0.1:%1").arg(port),
            "--allow-local", "--log-dir", logs.path()});
        require(process.waitForStarted(5000), "start real investigation signaller");
    }
    ~Server() { process.kill(); process.waitForFinished(5000); }
};
QList<QByteArray> fields(const QByteArray& line) {
    QList<QByteArray> result; QByteArray value; bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') { value += '"'; ++i; }
            else quoted = !quoted;
        } else if (ch == ',' && !quoted) { result.push_back(value); value.clear(); }
        else value += ch;
    }
    result.push_back(value); return result;
}
void report(const QString& label, int local, const QString& path, bool captureWake) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "open real runtime CSV");
    const auto lines = file.readAll().trimmed().split('\n');
    require(lines.size() >= 2, "runtime CSV must contain its final row");
    const auto header = fields(lines.front()), last = fields(lines.back());
    require(header.size() == last.size() && last.front() == "final", "runtime final CSV schema mismatch");
    QJsonObject metric{{"case", label}, {"peer", local}, {"csv", path}};
    for (const auto* name : {"recv_packets", "sent_packets", "jitter_avg_ms", "audio_packet_gap_max_ms",
            "receive_loop_gap_max_ms", "receive_processing_avg_us", "receive_processing_max_us",
            "capture_ready_dispatch_avg_us", "capture_ready_dispatch_max_us", "mix_worker_service_gap_us",
            "playback_ring_underruns", "adaptive_target_frames", "mix_adaptive_target_frames",
            "receive_socket_packets", "receive_socket_packets_max", "receive_packet_budget_yields",
            "receive_service_budget_yields", "receive_work_packet_limit", "receive_service_limit_us", "telemetry_service_samples", "telemetry_service_sum_us", "telemetry_service_max_us"}) {
        const auto index = header.indexOf(name);
        if (index >= 0) metric[name] = QString::fromUtf8(last[index]).toDouble();
    }
    require(header.indexOf("receive_socket_packets") >= 0 &&
        last[header.indexOf("receive_socket_packets")].toULongLong() > 1000,
        "runtime investigation must exercise sustained packet reception");
    require((last[header.indexOf("capture_ready_wake_signals")].toULongLong() != 0) == captureWake,
        "capture notification must follow its explicit configuration");
    require(last[header.indexOf("sent_packets")].toULongLong() > 1000,
        "capture polling must continue publishing audio packets");
    require(last[header.indexOf("capture_ring_overruns")].toULongLong() == 0,
        "bounded polling must continue servicing capture without overflowing its ring");
    std::cout << "RUNTIME_INGRESS " << QJsonDocument(metric).toJson(QJsonDocument::Compact).toStdString() << '\n';
}
void run(const QDir& root, bool serverMode, quint16 serverPort, int telemetry, int serviceUs, int repetition, Jam2CaptureWakeMode captureWake = Jam2CaptureWakeMode::Native, bool receiveTimestamps = false, int capturePollUs = 1000,
    std::optional<jam2::UdpBackend> backend=jam2::UdpBackend::Bsd, jam2::UdpServiceClass service=jam2::UdpServiceClass::BestEffort, bool queued=false,
    jam2::UdpWaitMode waitMode=jam2::UdpWaitMode::PlatformDefault) {
#if defined(__APPLE__)
    const auto expectedBackend=backend.value_or(serverMode ? jam2::UdpBackend::ConnectedBsd : jam2::UdpBackend::Bsd);
#else
    const auto expectedBackend=backend.value_or(jam2::UdpBackend::Bsd);
#endif
    const auto label = QStringLiteral("%1-diag%2-service%3-repeat%4-wake%5-poll%6")
        .arg(serverMode ? "server" : "direct").arg(telemetry).arg(serviceUs).arg(repetition).arg(static_cast<int>(captureWake)).arg(capturePollUs);
    // Engines outlive controllers so all network workers detach before their
    // callback-facing rings are destroyed, including assertion/error cleanup.
    std::array<ApplicationRuntime, 4> runtimes;
    std::array<Controller, 4> peers;
    std::array<QString, 4> csv;
    QString failure;
    for (int i = 0; i < 4; ++i) {
        require(peers[i].configureUdpDiagnostics(backend,service,waitMode),"configure typed UDP experiment before startup");
        require(peers[i].configureServerReceiveModel(queued ? jam2::application::UdpMultiplexTransport::ReceiveModel::QueuedWorker
            : jam2::application::UdpMultiplexTransport::ReceiveModel::Runtime),"configure receive owner before startup");
        peers[i].onTransportEvent = [&](const auto& event, bool) {
            if (event.failure != jam2::control_protocol::TransportFailure::None) failure = event.detail;
        };
        runtimes[i].onError = [&](const QString& error) { failure = error; };
        runtimes[i].onStartup = [&, i](const Jam2RuntimeStartup& startup) {
            if (startup.stats_csv) csv[i] = QString::fromStdString(startup.stats_csv->string());
        };
        peers[i].bindRuntime(runtimes[i], [&, i](const Controller::Snapshot& snapshot) {
            Jam2RuntimeOptions options;
            options.headless_audio = true; options.audio_buffer_size = 32;
            options.sample_rate = 48000; options.frame_size = 64;
            options.network_audio_format = jam2::NetworkAudioFormat::Pcm24Mono;
            options.test_input = Jam2TestInputMode::Tone440;
            options.stats_enabled = telemetry != 0;
            options.stats_interval_ms = telemetry == 2 ? 10 : telemetry == 1 ? 2000 : 0;
            options.stats_warmup_ms = 1000;
            options.receive_service_us = serviceUs;
            options.capture_wake = captureWake;
            options.capture_poll_us = capturePollUs;
            options.receive_timestamps = receiveTimestamps;
            const auto profile = *jam2::find_join_profile("fast");
            options.playback_ring_frames = profile.playback_ring_frames;
            options.playback_prefill_frames = profile.playback_prefill_frames;
            options.playback_max_frames = profile.playback_max_frames;
            options.playout_delay_frames = profile.playout_delay_frames;
            options.jitter_buffer_frames = profile.jitter_buffer_frames;
            options.jitter_buffer_max_frames = profile.jitter_buffer_max_frames;
            options.adaptive_playback_cushion = profile.adaptive_playback_cushion;
            options.adaptive_playback_target_frames = profile.adaptive_playback_target_frames;
            options.adaptive_playback_min_frames = profile.adaptive_playback_min_frames;
            options.adaptive_playback_max_frames = profile.adaptive_playback_max_frames;
            const auto folder = root.filePath(label + QStringLiteral("/peer-%1").arg(i));
            require(QDir().mkpath(folder), "create runtime CSV artifact folder");
            options.log_stats_dir = folder.toStdString();
            options.session_id = 0x0123456789abcdefULL;
            std::array<std::uint8_t, 16> key{};
            const auto keyBytes = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
            std::copy(keyBytes.begin(), keyBytes.end(), key.begin()); options.session_key = key;
            options.local_peer_id = snapshot.localPeerId;
            options.bootstrap_coordinator_peer_id = snapshot.coordinatorPeerId;
            options.bootstrap_role = snapshot.role == Controller::Role::Creator
                ? jam2::SessionBootstrapRole::Creator : jam2::SessionBootstrapRole::Joiner;
            options.mesh_peers_configured = true;
            for (const auto& peer : snapshot.peers) if (peer.token != snapshot.localToken && !peer.endpoint.isEmpty()) {
                options.mesh_peer_ids.push_back(peer.peerId);
                options.mesh_peers.push_back(jam2::parse_endpoint(peer.endpoint.toStdString()));
            }
            return options;
        });
    }
    Controller::CreatorConfig creator;
    creator.sessionHex = "0123456789abcdef"; creator.keyHex = "0123456789abcdef0123456789abcdef";
    creator.localToken = token(0); creator.name = "Alice";
    creator.bindHost = "127.0.0.1"; creator.localEndpoint = "127.0.0.1:1";
    creator.strategy = serverMode ? jam2::ConnectionStrategy::Jam2Server : jam2::ConnectionStrategy::Direct;
    creator.rendezvousServer = {"127.0.0.1", serverPort};
    creator.contract = {2, "pcm24-mono", "fast", 48000, 64};
    const bool created = peers[0].startCreator(creator);
    require(created, failure.isEmpty() ? "start investigation creator" : failure.toUtf8().constData());
    until([&] { return peers[0].multiplexSnapshot().registered; }, "creator registration");
    const auto port = serverMode ? serverPort : peers[0].multiplexSnapshot().bootstrapPublic.port;
    for (int i = 1; i < 4; ++i) {
        Controller::JoinerConfig join;
        join.host = "127.0.0.1"; join.port = port; join.bindHost = "127.0.0.1";
        join.localEndpoint = "127.0.0.1:1"; join.sessionHex = creator.sessionHex;
        join.keyHex = creator.keyHex; join.localToken = token(i);
        join.name = i == 1 ? "Bob" : i == 2 ? "Carol" : "Dave";
        join.strategy = creator.strategy;
        require(peers[i].startJoiner(join), "start investigation joiner");
        until([&] { return peers[i].snapshot().lifecycle == Controller::Lifecycle::Active; }, "joiner membership");
    }
    until([&] {
        for (int i = 0; i < 4; ++i) if (peers[i].snapshot().totalPeerCount != 4 || !runtimes[i].isNetworkRunning() || csv[i].isEmpty()) return false;
        return true;
    }, "exactly four runtime workers must stream through the real controller/multiplexer");
    QElapsedTimer duration; duration.start();
    until([&] { return duration.elapsed() >= 6000 || !failure.isEmpty(); }, "streaming observation", 8000);
    require(failure.isEmpty(), failure.toUtf8().constData());
    for (int i = 0; i < 4; ++i) {
        require(runtimes[i].engineSnapshot().audio_buffer_frames == 32, "investigation callback must remain 32 frames");
        const auto timing = peers[i].multiplexSnapshot();
        require(timing.waitMode==waitMode,"four-peer runtime must retain the selected wait comparison");
#if defined(__APPLE__)
        require(timing.runtimeWait.calls>0 && timing.runtimeWait.errors==0,
            "four-peer runtime must export completed readiness/deadline waits without platform errors");
#endif
        require(timing.backend==expectedBackend && timing.service==service && timing.runtimeReceiveEnabled==!queued,
            "four-peer production sockets must resolve automatic defaults and preserve explicit backend, service and reader choices");
        if (receiveTimestamps) {
            require(peers[i].multiplexDiagnostics().value("receive_trace").toObject().value("worst_phase_spans").isArray(),
                "production diagnostics must export bounded service spans even when no delayed packet exists");
            require(timing.receiveTimestamps && (expectedBackend==jam2::UdpBackend::NetworkFramework || (timing.kernelAudioGapSamples > 1000 && timing.socketResidenceSamples > 1000)),
                "four-peer production transport must export real kernel gap and residence measurements");
            require(timing.receiveTrace.invalid_samples == 0,
                "four-peer receive traces must keep kernel, read and worker clocks in one valid domain");
            require(timing.audioQueueDrops==0,"receive worker must not lose packets in its application queue");
            if (queued && backend==jam2::UdpBackend::Bsd) require(timing.audioQueueWaitSamples>1000,
                "queued reader must export its actual additional handoff delay");
        }
        runtimes[i].stopNetwork();
    }
    for (auto& peer : peers) peer.close();
    for (int i = 0; i < 4; ++i) { runtimes[i].shutdown(); report(label, i, csv[i], captureWake != Jam2CaptureWakeMode::Off); }
}
}
int runNetworkIngressRuntimeInvestigation(const QString& signaller, const QString& artifactRoot)
{
    try {
        const QDir root(QDir(artifactRoot).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")));
        require(QDir().mkpath(root.path()), "create runtime experiment artifacts");
        Server server(signaller);
        // Reverse all conditions on the second pass to expose ordering bias.
        for (int repetition = 0; repetition < 2; ++repetition) for (int item = 0; item < 12; ++item) {
            const int condition = repetition ? 11 - item : item;
            run(root, condition >= 6, server.port, (condition % 6) / 2,
                condition % 2 ? 1000 : 0, repetition);
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

int runCaptureWakeRuntimeInvestigation(const QString& signaller, const QString& artifactRoot)
{
    try {
        const QDir root(QDir(artifactRoot).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")));
        require(QDir().mkpath(root.path()), "create capture-wake experiment artifacts");
        Server server(signaller);
        for (const bool serverMode : {false, true}) for (const auto wake : {Jam2CaptureWakeMode::Native, Jam2CaptureWakeMode::PollOnly, Jam2CaptureWakeMode::Off, Jam2CaptureWakeMode::Automatic}) {
            run(root, serverMode, server.port, 1, 1000, 0, wake,
#if defined(__APPLE__)
                true
#else
                false
#endif
            );
            if (wake == Jam2CaptureWakeMode::PollOnly) run(root, serverMode, server.port, 1, 1000, 1, wake,
#if defined(__APPLE__)
                true,
#else
                false,
#endif
                250);
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
int runUdpPathsRuntimeInvestigation(const QString& signaller, const QString& artifactRoot)
{
    try {
        const QDir root(QDir(artifactRoot).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")));
        require(QDir().mkpath(root.path()),"create UDP path comparison artifacts");
        Server server(signaller);
        // The connected diagnostic uses the existing BSD bootstrap. Each
        // proven Server peer then retains its own local port in a Network flow.
        for (bool serverMode : {false,true}) for (int condition=0;condition<7;++condition) {
            if ((condition==3 || condition==4) && !serverMode) continue;
#if !defined(__APPLE__)
            if (condition==1 || condition==3 || condition==4 || condition==6) continue;
#endif
            const std::optional<jam2::UdpBackend> backend=condition>=5 ? std::nullopt : std::optional{
                condition==3 ? jam2::UdpBackend::NetworkFramework : condition==4 ? jam2::UdpBackend::ConnectedBsd : jam2::UdpBackend::Bsd};
            run(root,serverMode,server.port,1,1000,condition,Jam2CaptureWakeMode::PollOnly,
#if defined(__APPLE__)
                true,
#else
                false,
#endif
                250, backend,
                condition==1 ? jam2::UdpServiceClass::InteractiveVoice : jam2::UdpServiceClass::BestEffort,condition==2,
                condition==6 ? jam2::UdpWaitMode::Legacy : jam2::UdpWaitMode::PlatformDefault);
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
