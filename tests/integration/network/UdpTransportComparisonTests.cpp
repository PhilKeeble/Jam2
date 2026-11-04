#include "UdpMultiplexTransport.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "ControlProtocol.hpp"
#include "ProxyPollWait.hpp"
#include "network_session.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <mmsystem.h>
#endif

// This diagnostic isolates the actual socket/transport boundary. It uses the
// production handshakes, send fan-out, parsing and receive waits, but deliberately
// has no device or mixer. Timing is observational; conformance is asserted.
namespace {
using namespace jam2::application;
constexpr int count = 4, rate = 48000, durationUs = 1500000;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct TimerPeriod {
#ifdef _WIN32
    TimerPeriod() { require(timeBeginPeriod(1) == TIMERR_NOERROR, "comparison 1 ms timer request failed"); }
    ~TimerPeriod() { (void)timeEndPeriod(1); }
#endif
};
QString token(int i) { return QString(32, QChar('1' + i)); }
jam2::PeerId id(int i) { return {*jam2::control_protocol::peerIdFromToken(token(i))}; }
QString nonce() { return QString::fromLatin1(jam2::control_protocol::randomNonce().toHex()); }
template<class Predicate> void until(Predicate predicate, const char* reason) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1); QThread::msleep(1);
    }
    require(predicate(), reason);
}
jam2::SessionInfo sessionInfo() {
    jam2::SessionInfo result; result.session_id = 0x0123456789abcdefULL;
    const auto key = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
    std::copy(key.begin(), key.end(), result.key.begin()); return result;
}
struct MultiplexMesh {
    std::array<std::shared_ptr<UdpMultiplexTransport>, count> transports;
    std::array<std::unique_ptr<ControlClient>, count> clients;
    ControlServer host;
    std::set<int> authenticated;
    explicit MultiplexMesh(int creator) {
        const QString session = "0123456789abcdef", key = "0123456789abcdef0123456789abcdef";
        require(host.listenViaTransport(session, key), "configure real control authentication");
        host.onEvent = [](const jam2::control_protocol::TransportEvent& event) {
            if (event.failure != jam2::control_protocol::TransportFailure::None)
                std::cerr << "comparison host: " << event.detail.toStdString() << '\n';
        };
        for (int i = 0; i < count; ++i) {
            transports[i] = std::make_shared<UdpMultiplexTransport>();
            clients[i] = std::make_unique<ControlClient>();
            const auto name = PeerName::fromInput("Test");
            require(bool(name), "invalid comparison fixture name");
            clients[i]->setPeerName(*name);
            clients[i]->onEvent = [i](const jam2::control_protocol::TransportEvent& event) {
                if (event.failure != jam2::control_protocol::TransportFailure::None)
                    std::cerr << "comparison client " << i << ": " << event.detail.toStdString() << '\n';
            };
            transports[i]->onSignalerStatus = [i](const QString& detail) {
                std::cerr << "comparison transport " << i << ": " << detail.toStdString() << '\n';
            };
            transports[i]->onLinkReady = [&, i, creator](const QString& peer) {
                if (i != creator && peer == token(creator)) transports[i]->openStream(peer, UdpMuxChannel::Control);
            };
            transports[i]->onStreamReady = [&, i, creator, session, key](const QString& peer,
                    UdpMuxChannel channel, ControlConnection::Pointer connection, bool incoming) {
                require(channel == UdpMuxChannel::Control, "unexpected comparison stream");
                if (i == creator && incoming) { host.acceptTransport(connection); return; }
                require(i != creator && !incoming && peer == token(creator), "unexpected comparison stream owner");
                const auto observed = transports[i]->observedEndpoint(peer);
                require(bool(observed), "comparison observed endpoint missing");
                clients[i]->connectViaTransport(connection, session, key, token(i),
                    QString::fromStdString(jam2::endpoint_to_string(*observed)));
            };
        }
        host.onAuthenticated = [&, creator](const QString& peer, const ControlServer::AuthenticatedPeer&) {
            const int newcomer = peer[0].unicode() - '1';
            if (!authenticated.insert(newcomer).second) return;
            transports[creator]->admitPeer(peer);
            for (int other : authenticated) if (other != newcomer)
                transports[creator]->introduce(peer, token(other), nonce());
        };
        UdpMultiplexTransport::Config config;
        config.creator = true; config.strategy = jam2::ConnectionStrategy::Direct;
        config.bindHost = "127.0.0.1"; config.localToken = token(creator);
        config.sessionHex = session; config.keyHex = key; config.socketBuffers = {65536, 65536};
        QString error;
        require(transports[creator]->start(config, error), error.toUtf8().constData());
        until([&] { return transports[creator]->bootstrapEndpoint().has_value(); }, "creator endpoint unavailable");
        config.server = *transports[creator]->bootstrapEndpoint();
        config.creator = false;
        for (int i = 0; i < count; ++i) if (i != creator) {
            config.localToken = token(i);
            require(transports[i]->start(config, error), error.toUtf8().constData());
            until([&] { return authenticated.contains(i); }, "comparison peer authentication failed");
        }
        until([&] {
            for (int i = 0; i < count; ++i) for (int j = 0; j < count; ++j)
                if (i != j && !transports[i]->peerEndpoint(id(j).value)) return false;
            return true;
        }, "comparison four-peer mesh did not converge");
    }
    ~MultiplexMesh() { for (auto& transport : transports) if (transport) transport->stop(); }
};
struct Distribution {
    std::vector<quint64> values;
    void add(quint64 value) { values.push_back(value); }
    QJsonObject json() {
        std::sort(values.begin(), values.end());
        const auto at = [&](double fraction) -> qint64 {
            return values.empty() ? 0 : qint64(values[std::size_t((values.size() - 1) * fraction)]);
        };
        return {{"samples", qint64(values.size())}, {"p50", at(.5)}, {"p99", at(.99)}, {"max", at(1)}};
    }
};
struct Observation {
    Distribution delivery, sendLate, gaps, waits, loopGaps;
    quint64 received = 0, invalid = 0, duplicates = 0, gapsOver20ms = 0;
    std::exception_ptr error;
};

// Queue known, authenticated control packets ahead of real PCM audio. No WAV
// transfer or artificial control parser is involved in these boundary probes.
void ingressBoundary(bool mixed) {
    MultiplexMesh mesh(0);
    std::array<std::unique_ptr<jam2::NetworkSession>, count> sessions;
    jam2::NetworkSessionContract contract;
    contract.frames_per_packet = 64;
    contract.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    jam2::PeerStreamConfig config;
    config.frames_per_packet = contract.frames_per_packet;
    config.audio_format = contract.audio_format;
    for (int local = 0; local < count; ++local) {
        std::vector<jam2::NetworkPeerDescriptor> peers;
        for (int remote = 0; remote < count; ++remote) if (local != remote)
            peers.push_back({id(remote), jam2::resolve_udp_endpoint(*mesh.transports[local]->peerEndpoint(id(remote).value)),
                jam2::PeerEndpointState::Active});
        sessions[local] = std::make_unique<jam2::NetworkSession>(
            std::static_pointer_cast<jam2::NetworkDatagramTransport>(mesh.transports[local]), sessionInfo(), contract,
            local == 0 ? jam2::SessionBootstrapRole::Creator : jam2::SessionBootstrapRole::Joiner,
            id(local), peers, config, nullptr);
    }
    // Finish the real four-peer control handshake before isolating the probe.
    QElapsedTimer quiet; quiet.start();
    while (quiet.elapsed() < 30) {
        for (auto& session : sessions) while (session->receiveFor(0)) {}
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QThread::msleep(1);
    }
    const auto links = mesh.transports[1]->snapshot().links;
    const auto link = std::find_if(links.begin(), links.end(), [](const auto& item) { return item.peer == token(0); });
    require(link != links.end() && link->controlStream, "probe requires its real authenticated control stream");
    const auto key = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
    const auto ack = encodeUdpMuxView({UdpMuxKind::Ack, UdpMuxChannel::Control,
        sessionInfo().session_id, link->introduction, link->controlStream, 1, {}}, key);
    require(!ack.isEmpty(), "encode duplicate authenticated ACK");
    const auto before = mesh.transports[0]->snapshot();
    std::array<std::uint8_t, 128> pcm{};
    const int groups = mixed ? 8 : 1, controls = mixed ? 8 : 64;
    for (int group = 0; group < groups; ++group) {
        for (int packet = 0; packet < controls; ++packet)
            require(mesh.transports[1]->sendToPeer(id(0).value, {},
                {reinterpret_cast<const std::uint8_t*>(ack.constData()), std::size_t(ack.size())}).outcome == jam2::UdpSendOutcome::Sent,
                "enqueue authenticated control prefix");
        require(sessions[1]->sendToPeer(id(0), jam2::protocol::PacketType::Audio,
            group + 1, std::uint64_t(group) * 64, pcm) != 0, "enqueue real PCM packet");
    }
    std::size_t native = 0;
    jam2::DatagramReceiveBudget budget{64, 0}; // Isolate packet counting from host scheduling.
    while (native < 64) {
        const auto packet = sessions[0]->receiveFor(0, nullptr, &budget);
        if (!packet) break;
        const auto parsed = sessions[0]->parse(packet->bytes);
        require(bool(parsed) && parsed.header.type == jam2::protocol::PacketType::Audio,
            "control prefix must not escape as native audio");
        require(sessions[0]->peerStream(id(1)).receiveAudio(parsed.header,
            packet->bytes.subspan(jam2::protocol::kHeaderSize), jam2::monotonic_us()) == jam2::PeerAudioResult::Accepted,
            "probe PCM must pass the production peer stream");
        ++native;
    }
    const auto after = mesh.transports[0]->snapshot();
    const auto raw = after.runtimeControlQueued - before.runtimeControlQueued +
        after.runtimeNativeDelivered - before.runtimeNativeDelivered;
    const bool exhausted = budget.exhausted();
    std::cout << "METRIC ingress_probe mixed=" << mixed << " native=" << native << " socket_packets=" << raw
        << " classification_yields=" << after.runtimeBudgetYields - before.runtimeBudgetYields
        << " runtime_reports_exhausted=" << exhausted << '\n';
    // These assertions deliberately fail on the original runtime contract.
    // The retained baseline log records the failure before trying a candidate.
    require(mixed ? raw <= 64 : exhausted,
        mixed ? "one receive turn exceeded its total socket-work budget" :
            "classification yield was reported as a drained receive batch while PCM remained queued");
    require(!budget.idle() && raw == budget.packets, "control-only work must be counted and cannot enable idle telemetry");
    // Yielding must retain the remaining PCM, peer identity, order and replay state.
    const auto expected = std::size_t(groups);
    for (int turn = 0; native < expected && turn < 4; ++turn) {
        jam2::DatagramReceiveBudget next{64, 0};
        while (const auto packet = sessions[0]->receiveFor(0, nullptr, &next)) {
            const auto parsed = sessions[0]->parse(packet->bytes);
            require(bool(parsed) && parsed.header.sequence == native + 1,
                "yield lost or reordered queued PCM");
            ++native;
        }
    }
    require(native == expected, "all queued PCM must survive bounded receive turns");
    if (!mixed) for (bool prequeued : {false, true}) for (bool bounded : {false, true}) {
        std::vector<quint64> service;
        for (int trial = 0; trial < 32; ++trial) {
            require(mesh.transports[1]->sendToPeer(id(0).value, {},
                {reinterpret_cast<const std::uint8_t*>(ack.constData()), std::size_t(ack.size())}).outcome == jam2::UdpSendOutcome::Sent,
                "enqueue control-only service probe");
            // Contrast normal asynchronous arrival with a packet which has
            // already waited in the local receive queue before the call.
            if (prequeued) QThread::msleep(2);
            jam2::DatagramReceiveBudget controlOnly{64, 0};
            const auto began = jam2::monotonic_us();
            require(!sessions[0]->receiveFor(1000, nullptr, bounded ? &controlOnly : nullptr),
                "control-only probe must not manufacture native packets");
            service.push_back(jam2::monotonic_us() - began);
            if (bounded) require(controlOnly.packets == 1 && !controlOnly.idle() && !controlOnly.exhausted(),
                "a genuinely drained control-only turn is busy but not budget-exhausted");
        }
        std::sort(service.begin(), service.end());
        std::cout << "METRIC control_only_wait prequeued=" << prequeued << " bounded=" << bounded << " p50_us=" << service[16]
            << " p99_us=" << service[31] << '\n';
    }
}
void runCase(MultiplexMesh* mux, int creator, int pass, int frames, jam2::NetworkAudioFormat format,
        quint64 batchUs, bool captureWake, int controlPrefix = 0, bool boundedReceive = false, int serviceUs = 1000) {
    const auto sequences = std::uint32_t(durationUs * std::uint64_t(rate) / (frames * 1000000ULL));
    std::array<jam2::UdpSocket, count> raw;
    std::array<jam2::ResolvedUdpEndpoint, count> endpoints;
    if (!mux) for (int i = 0; i < count; ++i) {
        raw[i].bind({"127.0.0.1", 0}); raw[i].set_send_buffer_size(65536); raw[i].set_recv_buffer_size(65536);
        endpoints[i] = jam2::resolve_udp_endpoint(raw[i].local_endpoint());
    }
    std::array<std::unique_ptr<jam2::NetworkSession>, count> sessions;
    for (int i = 0; i < count; ++i) {
        std::vector<jam2::NetworkPeerDescriptor> peers;
        for (int j = 0; j < count; ++j) if (j != i) peers.push_back({id(j),
            mux ? jam2::resolve_udp_endpoint(*mux->transports[i]->peerEndpoint(id(j).value)) : endpoints[j],
            jam2::PeerEndpointState::Active});
        jam2::NetworkSessionContract contract; contract.sample_rate = rate;
        contract.frames_per_packet = frames; contract.audio_format = format;
        jam2::PeerStreamConfig config; config.sample_rate = rate;
        config.frames_per_packet = frames; config.audio_format = format;
        jam2::NetworkSessionTransport source = mux
            ? jam2::NetworkSessionTransport{std::static_pointer_cast<jam2::NetworkDatagramTransport>(mux->transports[i])}
            : jam2::NetworkSessionTransport{std::move(raw[i])};
        sessions[i] = std::make_unique<jam2::NetworkSession>(std::move(source), sessionInfo(), contract,
            i == creator ? jam2::SessionBootstrapRole::Creator : jam2::SessionBootstrapRole::Joiner,
            id(i), peers, config, nullptr);
    }
    std::array<std::vector<std::atomic<quint64>>, count> sentTimes;
    std::array<std::vector<std::pair<quint64, QByteArray>>, count> controlPackets;
    if (mux && controlPrefix) for (int local = 0; local < count; ++local)
        for (const auto& link : mux->transports[local]->snapshot().links) if (link.controlStream) {
            const auto ack = encodeUdpMuxView({UdpMuxKind::Ack, UdpMuxChannel::Control,
                sessionInfo().session_id, link.introduction, link.controlStream, 1, {}},
                QByteArray::fromHex("0123456789abcdef0123456789abcdef"));
            controlPackets[local].emplace_back(*jam2::control_protocol::peerIdFromToken(link.peer), ack);
        }
    for (auto& times : sentTimes) times = std::vector<std::atomic<quint64>>(sequences + 1);
    std::array<jam2::RealtimeWakeSignal, count> wake;
    std::array<Observation, count> observations;
    std::atomic<quint64> epoch{0};
    std::array<std::jthread, count> workers;
    for (int local = 0; local < count; ++local) workers[local] = std::jthread([&, local] {
        try {
            auto& session = *sessions[local]; auto& out = observations[local];
            out.delivery.values.reserve(sequences * (count - 1));
            out.gaps.values.reserve(sequences * (count - 1));
            out.sendLate.values.reserve(sequences);
            out.waits.values.reserve(20000); out.loopGaps.values.reserve(20000);
            std::vector<std::int32_t> samples(frames, 0x12000000);
            std::vector<std::uint8_t> payload(frames * (format == jam2::NetworkAudioFormat::Pcm16Mono ? 2 : 3));
            require(jam2::protocol::pack_audio_q31_into(format, samples, payload), "comparison payload packing");
            std::array<std::vector<bool>, count> seen;
            for (auto& v : seen) v.resize(sequences + 1);
            while (!epoch.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto start = epoch.load();
            std::uint32_t next = 1; quint64 lastTurn = 0;
            std::array<quint64, count> lastReceived{};
            const auto due = [&](std::uint32_t seq) {
                const quint64 elapsed = std::uint64_t(seq - 1) * frames * 1000000ULL / rate;
                return start + (batchUs ? (elapsed + batchUs - 1) / batchUs * batchUs : elapsed);
            };
            while (jam2::monotonic_us() < start + durationUs + 250000) {
                const auto now = jam2::monotonic_us();
                if (lastTurn) out.loopGaps.add(now - lastTurn); lastTurn = now;
                while (next <= sequences && now >= due(next)) {
                    for (const auto& [peer, packet] : controlPackets[local]) for (int repeat = 0; repeat < controlPrefix; ++repeat)
                        require(mux->transports[local]->sendToPeer(peer, {},
                            {reinterpret_cast<const std::uint8_t*>(packet.constData()), std::size_t(packet.size())}).outcome == jam2::UdpSendOutcome::Sent,
                            "comparison control prefix send failed");
                    const auto sending = jam2::monotonic_us();
                    sentTimes[local][next].store(sending, std::memory_order_release);
                    const auto result = session.sendToActive(jam2::protocol::PacketType::Audio,
                        next, std::uint64_t(next - 1) * frames, payload);
                    require(result.peer_count == count - 1, "comparison send failed");
                    out.sendLate.add(sending - due(next)); ++next;
                }
                const auto beforeWait = jam2::monotonic_us();
                const quint64 waitUs = next <= sequences ? std::min<quint64>(1000,
                    due(next) > beforeWait ? due(next) - beforeWait : 0) : 1000;
                if (captureWake) wake[local].requestWake();
                jam2::DatagramReceiveBudget work{64, std::uint64_t(serviceUs)};
                auto datagram = session.receiveFor(waitUs, captureWake ? &wake[local] : nullptr, boundedReceive ? &work : nullptr);
                if (captureWake) wake[local].cancelWakeRequest();
                out.waits.add(jam2::monotonic_us() - beforeWait);
                for (int budget = 0; datagram && budget < 64; ++budget) {
                    const auto receivedAt = jam2::monotonic_us();
                    const auto parsed = session.parse(datagram->bytes);
                    const auto peer = session.peerIdForEndpoint(datagram->endpoint);
                    int sender = 0; while (sender < count && id(sender) != peer) ++sender;
                    if (!parsed || sender == count) { ++out.invalid; }
                    else if (parsed.header.type == jam2::protocol::PacketType::Audio) {
                        const auto seq = parsed.header.sequence;
                        if (!seq || seq > sequences || parsed.header.timing_value != std::uint64_t(seq - 1) * frames ||
                            parsed.header.payload_length != payload.size() ||
                            !std::equal(payload.begin(), payload.end(), datagram->bytes.begin() + jam2::protocol::kHeaderSize)) ++out.invalid;
                        else if (seen[sender][seq]) ++out.duplicates;
                        else {
                            seen[sender][seq] = true; ++out.received;
                            const auto sentAt = sentTimes[sender][seq].load(std::memory_order_acquire);
                            if (!sentAt || receivedAt < sentAt) ++out.invalid;
                            else out.delivery.add(receivedAt - sentAt);
                            if (lastReceived[sender]) {
                                const auto gap = receivedAt - lastReceived[sender]; out.gaps.add(gap);
                                out.gapsOver20ms += gap > 20000;
                            }
                            lastReceived[sender] = receivedAt;
                        }
                    }
                    datagram = budget + 1 < 64 ? session.receiveFor(0, nullptr, boundedReceive ? &work : nullptr) : std::nullopt;
                }
                if (out.received == std::uint64_t(sequences) * (count - 1) && next > sequences) break;
            }
        } catch (...) { observations[local].error = std::current_exception(); }
    });
    const auto start = jam2::monotonic_us() + 20000; epoch.store(start, std::memory_order_release);
    jam2::test::ProxyPollWait poll;
    quint64 nextWake = start;
    while (jam2::monotonic_us() < start + durationUs + 270000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        const auto now = jam2::monotonic_us();
        if (captureWake && now >= nextWake) {
            for (auto& signal : wake) signal.signal(now);
            nextWake = now + 32 * 1000000ULL / rate;
        }
        require(poll.wait(), "comparison high-resolution wait failed");
    }
    for (auto& worker : workers) worker.join();
    for (int i = 0; i < count; ++i) {
        auto& out = observations[i];
        if (out.error) std::rethrow_exception(out.error);
        QJsonObject row{{"backend", mux ? "multiplex" : "native"}, {"pass", pass}, {"peer", i},
            {"creator", i == creator}, {"sample_rate", rate}, {"packet_frames", frames},
            {"windows_timer_period_ms", 1},
            {"format", format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24"},
            {"batch_us", qint64(batchUs)}, {"capture_notifications", captureWake},
            {"control_prefix", controlPrefix}, {"bounded_receive", boundedReceive}, {"receive_service_us", serviceUs},
            {"capture_signals", qint64(wake[i].signalCount())},
            {"capture_consumptions", qint64(wake[i].consumptionCount())},
            {"received", qint64(out.received)}, {"expected", qint64(sequences) * (count - 1)},
            {"invalid", qint64(out.invalid)}, {"duplicates", qint64(out.duplicates)},
            {"gaps_over_20ms", qint64(out.gapsOver20ms)},
            {"send_to_receive_us", out.delivery.json()}, {"send_lateness_us", out.sendLate.json()},
            {"receive_gap_us", out.gaps.json()}, {"wait_us", out.waits.json()}, {"loop_gap_us", out.loopGaps.json()}};
        if (mux) {
            const auto s = mux->transports[i]->snapshot();
            row["mux_queue_drops"] = qint64(s.audioQueueDrops + s.ingressQueueDrops);
            row["mux_send_errors"] = qint64(s.sendErrors);
            row["mux_budget_yields"] = qint64(s.runtimeBudgetYields);
            row["physical_sockets"] = qint64(s.physicalSocketCount);
            row["mux_worker_lock_max_us"] = qint64(s.workerLockWaitMaxUs);
        }
        std::cout << QJsonDocument(row).toJson(QJsonDocument::Compact).constData() << std::endl;
        require(out.received == std::uint64_t(sequences) * (count - 1), "comparison lost loopback audio");
        require(!out.invalid && !out.duplicates, "comparison corrupted or duplicated loopback audio");
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        jam2::NetworkRuntime runtime;
        TimerPeriod timer;
        if (argc == 2 && (QByteArrayView(argv[1]) == "--ingress-boundary" || QByteArrayView(argv[1]) == "--ingress-mixed")) {
            ingressBoundary(QByteArrayView(argv[1]) == "--ingress-mixed");
            return 0;
        }
        if (argc == 2 && QByteArrayView(argv[1]) == "--ingress-performance") {
            for (int pass = 0; pass < 4; ++pass) {
                MultiplexMesh mesh(pass >= 2 ? 1 : 0);
                const bool bounded = pass == 1 || pass == 2;
                for (quint64 batch : {0ULL, 8000ULL}) for (int controls : {0, 4})
                    runCase(&mesh, pass >= 2 ? 1 : 0, pass, 64,
                        jam2::NetworkAudioFormat::Pcm16Mono, batch, true, controls, bounded);
            }
            return 0;
        }
        // ABBA ordering limits ordering bias; repeat with a different creator.
        for (int pass = 0; pass < 4; ++pass) {
            const bool multiplex = pass == 1 || pass == 2;
            const int creator = pass >= 2 ? 1 : 0;
            std::unique_ptr<MultiplexMesh> mesh;
            if (multiplex) mesh = std::make_unique<MultiplexMesh>(creator);
            for (const auto format : {jam2::NetworkAudioFormat::Pcm16Mono, jam2::NetworkAudioFormat::Pcm24Mono})
                for (int frames : {64, 128}) for (quint64 batch : {0ULL, 8000ULL}) for (bool capture : {false, true})
                    runCase(mesh.get(), creator, pass, frames, format, batch, capture);
        }
        std::cout << "four-peer native/multiplex transport conformance passed; timing is observational\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
