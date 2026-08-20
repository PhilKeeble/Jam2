#include "ApplicationRuntime.hpp"
#include "AutomationChannel.hpp"
#include "ControlProtocol.hpp"
#include "NativeTcpTransport.hpp"

#include <QCoreApplication>
#include <QJsonObject>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct EnvironmentRestore {
    EnvironmentRestore()
        : hadCommand(qEnvironmentVariableIsSet("JAM2_AUTOMATION_COMMAND_HANDLE")),
          hadEvent(qEnvironmentVariableIsSet("JAM2_AUTOMATION_EVENT_HANDLE")),
          command(qgetenv("JAM2_AUTOMATION_COMMAND_HANDLE")),
          event(qgetenv("JAM2_AUTOMATION_EVENT_HANDLE"))
    {
    }

    ~EnvironmentRestore()
    {
        if (hadCommand) qputenv("JAM2_AUTOMATION_COMMAND_HANDLE", command);
        else qunsetenv("JAM2_AUTOMATION_COMMAND_HANDLE");
        if (hadEvent) qputenv("JAM2_AUTOMATION_EVENT_HANDLE", event);
        else qunsetenv("JAM2_AUTOMATION_EVENT_HANDLE");
    }

    bool hadCommand;
    bool hadEvent;
    QByteArray command;
    QByteArray event;
};

class PipeFixture {
public:
    PipeFixture()
    {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        valid_ = CreatePipe(&commandRead_, &commandWrite_, &attributes, 0) &&
            CreatePipe(&eventRead_, &eventWrite_, &attributes, 0);
#else
        int command[2]{-1, -1};
        int event[2]{-1, -1};
        valid_ = ::pipe(command) == 0 && ::pipe(event) == 0;
        if (valid_) {
            commandRead_ = command[0];
            commandWrite_ = command[1];
            eventRead_ = event[0];
            eventWrite_ = event[1];
        } else {
            if (command[0] >= 0) ::close(command[0]);
            if (command[1] >= 0) ::close(command[1]);
            if (event[0] >= 0) ::close(event[0]);
            if (event[1] >= 0) ::close(event[1]);
        }
#endif
    }

    ~PipeFixture()
    {
        closeCommandRead();
        closeCommandWrite();
        closeEventRead();
        closeEventWrite();
    }

    bool valid() const noexcept { return valid_; }

    std::unique_ptr<AutomationChannel> makeChannel(std::string& error)
    {
        qputenv("JAM2_AUTOMATION_COMMAND_HANDLE",
            QByteArray::number(static_cast<qulonglong>(commandReadValue())));
        qputenv("JAM2_AUTOMATION_EVENT_HANDLE",
            QByteArray::number(static_cast<qulonglong>(eventWriteValue())));
        auto channel = AutomationChannel::fromInheritedEnvironment(true, error);
        if (channel) {
#if defined(_WIN32)
            commandRead_ = nullptr;
            eventWrite_ = nullptr;
#else
            commandRead_ = -1;
            eventWrite_ = -1;
#endif
        }
        return channel;
    }

    bool writeMalformedJson()
    {
        const std::array<std::uint8_t, 5> frame{1, 0, 0, 0, '{'};
#if defined(_WIN32)
        DWORD written = 0;
        return WriteFile(commandWrite_, frame.data(),
                   static_cast<DWORD>(frame.size()), &written, nullptr) &&
            written == frame.size();
#else
        return ::write(commandWrite_, frame.data(), frame.size()) ==
            static_cast<ssize_t>(frame.size());
#endif
    }

    void closeCommandWrite() noexcept
    {
#if defined(_WIN32)
        if (commandWrite_ != nullptr) CloseHandle(commandWrite_);
        commandWrite_ = nullptr;
#else
        if (commandWrite_ >= 0) ::close(commandWrite_);
        commandWrite_ = -1;
#endif
    }

private:
    std::uintptr_t commandReadValue() const noexcept
    {
#if defined(_WIN32)
        return reinterpret_cast<std::uintptr_t>(commandRead_);
#else
        return static_cast<std::uintptr_t>(commandRead_);
#endif
    }

    std::uintptr_t eventWriteValue() const noexcept
    {
#if defined(_WIN32)
        return reinterpret_cast<std::uintptr_t>(eventWrite_);
#else
        return static_cast<std::uintptr_t>(eventWrite_);
#endif
    }

    void closeCommandRead() noexcept
    {
#if defined(_WIN32)
        if (commandRead_ != nullptr) CloseHandle(commandRead_);
        commandRead_ = nullptr;
#else
        if (commandRead_ >= 0) ::close(commandRead_);
        commandRead_ = -1;
#endif
    }

    void closeEventRead() noexcept
    {
#if defined(_WIN32)
        if (eventRead_ != nullptr) CloseHandle(eventRead_);
        eventRead_ = nullptr;
#else
        if (eventRead_ >= 0) ::close(eventRead_);
        eventRead_ = -1;
#endif
    }

    void closeEventWrite() noexcept
    {
#if defined(_WIN32)
        if (eventWrite_ != nullptr) CloseHandle(eventWrite_);
        eventWrite_ = nullptr;
#else
        if (eventWrite_ >= 0) ::close(eventWrite_);
        eventWrite_ = -1;
#endif
    }

    bool valid_ = false;
#if defined(_WIN32)
    HANDLE commandRead_ = nullptr;
    HANDLE commandWrite_ = nullptr;
    HANDLE eventRead_ = nullptr;
    HANDLE eventWrite_ = nullptr;
#else
    int commandRead_ = -1;
    int commandWrite_ = -1;
    int eventRead_ = -1;
    int eventWrite_ = -1;
#endif
};

void testAutomationChannelStateAndDisconnect()
{
    EnvironmentRestore restore;
    qunsetenv("JAM2_AUTOMATION_COMMAND_HANDLE");
    qunsetenv("JAM2_AUTOMATION_EVENT_HANDLE");
    std::string error;
    expect(!AutomationChannel::fromInheritedEnvironment(false, error) && error.empty(),
        "optional automation channel accepts an absent handle pair");
    expect(!AutomationChannel::fromInheritedEnvironment(true, error) && !error.empty(),
        "required automation channel rejects an absent handle pair");

    PipeFixture queuePipes;
    expect(queuePipes.valid(), "automation queue fixture creates native pipes");
    if (!queuePipes.valid()) return;
    error.clear();
    auto queued = queuePipes.makeChannel(error);
    expect(queued != nullptr && error.empty(),
        "automation channel accepts a valid native handle pair");
    if (!queued) return;
    for (std::size_t index = 0; index < AutomationChannel::kQueueCapacity; ++index) {
        expect(queued->send(QJsonObject{{QStringLiteral("index"),
                          static_cast<qint64>(index)}}),
            "automation event queue accepts each bounded slot");
    }
    expect(!queued->send(QJsonObject{{QStringLiteral("overflow"), true}}) &&
            queued->queuedEvents() == AutomationChannel::kQueueCapacity &&
            queued->eventQueueHighWater() == AutomationChannel::kQueueCapacity &&
            queued->rejectedEvents() == 1 && queued->rejectedFrames() == 0,
        "automation channel exposes exact bounded queue and rejection diagnostics");
    queued->stop(false);
    expect(queued->queuedEvents() == 0 &&
            queued->rejectedEvents() == AutomationChannel::kQueueCapacity + 1,
        "non-draining automation stop accounts for every discarded event");

    PipeFixture disconnectPipes;
    expect(disconnectPipes.valid(), "automation disconnect fixture creates native pipes");
    if (!disconnectPipes.valid()) return;
    error.clear();
    auto disconnected = disconnectPipes.makeChannel(error);
    expect(disconnected != nullptr && error.empty(),
        "automation disconnect fixture creates its channel");
    if (!disconnected) return;
    std::mutex mutex;
    std::condition_variable ready;
    std::string disconnectError;
    disconnected->start(
        [](QJsonObject) {},
        [&](std::string detail) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                disconnectError = std::move(detail);
            }
            ready.notify_one();
        });
    expect(disconnectPipes.writeMalformedJson(),
        "automation fixture writes a bounded malformed JSON frame");
    disconnectPipes.closeCommandWrite();
    {
        std::unique_lock<std::mutex> lock(mutex);
        (void)ready.wait_for(lock, std::chrono::seconds(30), [&] {
            return !disconnectError.empty();
        });
    }
    expect(disconnected->rejectedFrames() == 1 && !disconnectError.empty(),
        "automation reader counts malformed frames and reports one disconnect");
    disconnected->stop();
}

void testTcpReservationAndListener()
{
    using namespace jam2::application;
    NativeTcpPortReservation reservation;
    expect(reservation.localPort() == 0 && reservation.errorString().isEmpty(),
        "new TCP reservation has no port or error");
    expect(reservation.bind(QStringLiteral("127.0.0.1"), 0) &&
            reservation.localPort() != 0,
        "TCP reservation binds an ephemeral numeric loopback port");
    const quint16 occupied = reservation.localPort();
    NativeTcpPortReservation collision;
    expect(!collision.bind(QStringLiteral("127.0.0.1"), occupied) &&
            !collision.errorString().isEmpty(),
        "TCP reservation reports a deterministic loopback bind collision");
    collision.close();
    expect(collision.localPort() == 0,
        "closing a failed TCP reservation retains no port");
    reservation.close();
    expect(reservation.localPort() == 0,
        "closing a live TCP reservation releases its port");
    expect(!reservation.bind(QStringLiteral("not-a-numeric-host"), 0) &&
            !reservation.errorString().isEmpty(),
        "TCP reservation rejects a nonnumeric bind host");

    QObject context;
    NativeTcpListener listener;
    QString callbackError;
    expect(listener.listen(0, &context,
               [](const NativeTcpConnection::Pointer&) {},
               [&](const QString& detail) { callbackError = detail; }, 1) &&
            listener.isListening() && listener.localPort() != 0,
        "native TCP listener publishes its actual ephemeral port");
    listener.close();
    expect(!listener.isListening() && listener.localPort() == 0 &&
            callbackError.isEmpty(),
        "native TCP listener closes without manufacturing an error callback");
}

void testRuntimeCountersAndPeerGain()
{
    ApplicationRuntime runtime;
    Jam2RuntimeOptions options;
    options.headless_audio = true;
    options.sample_rate = 48000;
    options.frame_size = 64;
    options.audio_buffer_size = 64;
    options.channel_selection.input = {0};
    options.channel_selection.output = {0};
    options.capture_ring_frames = 512;
    options.playback_ring_frames = 512;

    expect(runtime.engineStarts() == 0 && runtime.engineRestarts() == 0 &&
            runtime.engineReuses() == 0,
        "application runtime counters begin at zero");
    expect(runtime.startLocal(options),
        "application runtime starts a synthetic local engine");
    expect(runtime.startLocal(options) && runtime.engineStarts() == 1 &&
            runtime.engineRestarts() == 0 && runtime.engineReuses() == 1,
        "identical local configuration reuses the persistent engine");
    options.sample_rate = 44100;
    expect(runtime.startLocal(options) && runtime.engineStarts() == 2 &&
            runtime.engineRestarts() == 1 && runtime.engineReuses() == 1,
        "changed cold configuration restarts and recounts the engine");

    expect(runtime.startNetwork(options) && runtime.isNetworkRunning(),
        "application runtime starts its bounded fake network worker");
    expect(runtime.engineReuses() == 2 &&
            runtime.reprobePeers() &&
            runtime.setPeerGainDb(2, 0.0) &&
            runtime.setPeerGainDb(2, -60.0) &&
            !runtime.setPeerGainDb(0, 0.0) &&
            !runtime.setPeerGainDb(2, -60.1) &&
            !runtime.setPeerGainDb(2, 12.1) &&
            !runtime.setPeerGainDb(2, std::numeric_limits<double>::quiet_NaN()),
        "network peer gain accepts finite bounds and rejects invalid identity/range");
    runtime.stopNetwork();
    expect(!runtime.isNetworkRunning() && !runtime.setPeerGainDb(2, 0.0) &&
            !runtime.reprobePeers(),
        "peer gain and proof reset reject a stopped network worker");
    runtime.shutdown();

    Jam2RuntimeHost host;
    expect(!host.takePeerReprobe(),
        "runtime host starts without a pending UDP proof reset");
    host.submitPeerReprobe();
    expect(host.takePeerReprobe() && !host.takePeerReprobe(),
        "runtime host consumes each UDP proof reset exactly once");
    host.submitPeerReprobe();
    host.reset();
    expect(!host.takePeerReprobe(),
        "runtime host reset clears stale UDP proof requests");
}

void testControlToken()
{
    const QString first = jam2::control_protocol::randomPeerToken();
    const QString second = jam2::control_protocol::randomPeerToken();
    expect(first.size() == 32 && second.size() == 32 && first != second &&
            jam2::control_protocol::peerIdFromToken(first).has_value() &&
            jam2::control_protocol::peerIdFromToken(second).has_value(),
        "random control peer tokens are fixed-shape, distinct, and carry usable peer IDs");
}

} // namespace

jam2::EngineConfig jam2_make_engine_config(
    const Jam2RuntimeOptions& options,
    bool leaderAudioLocalClick)
{
    jam2::EngineConfig config;
    config.backend = options.headless_audio
        ? jam2::EngineAudioBackend::Headless
        : jam2::EngineAudioBackend::Device;
    config.audio_device_id = options.audio_device_id.value_or(-1);
    config.sample_rate = options.sample_rate;
    config.audio_buffer_frames = options.audio_buffer_size;
    config.channels = options.channel_selection;
    config.capture_ring_frames = options.capture_ring_frames;
    config.playback_ring_frames = options.playback_ring_frames;
    config.leader_audio_local_click = leaderAudioLocalClick;
    config.prepared_track_max_frames = 4096;
    return config;
}

bool jam2_engine_restart_required(
    const jam2::EngineConfig& active,
    const jam2::EngineConfig& requested) noexcept
{
    return active.backend != requested.backend ||
        active.audio_device_id != requested.audio_device_id ||
        active.sample_rate != requested.sample_rate ||
        active.audio_buffer_frames != requested.audio_buffer_frames ||
        active.channels.input != requested.channels.input ||
        active.channels.output != requested.channels.output ||
        active.capture_ring_frames != requested.capture_ring_frames ||
        active.playback_ring_frames != requested.playback_ring_frames;
}

int jam2_run_network_runtime(Jam2RuntimeOptions, Jam2RuntimeHost& host)
{
    while (!host.stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        testAutomationChannelStateAndDisconnect();
        testTcpReservationAndListener();
        testRuntimeCountersAndPeerGain();
        testControlToken();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: unexpected application-boundary exception: "
                  << exception.what() << '\n';
    }

    if (failures != 0) {
        std::cerr << failures << " application boundary checks failed\n";
        return 1;
    }
    std::cout << "application boundary checks passed\n";
    return 0;
}
