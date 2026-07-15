#include "ApplicationRuntime.hpp"

#include <QMetaObject>

#include <exception>
#include <utility>

ApplicationRuntime::ApplicationRuntime(QObject* parent)
    : QObject(parent)
{
    poll_timer_.setInterval(20);
    QObject::connect(&poll_timer_, &QTimer::timeout, this, [this] { pollEngine(); });
    poll_timer_.start();

    host_.startup = [this](const Jam2RuntimeStartup& startup) {
        QMetaObject::invokeMethod(this, [this, startup] {
            if (onStartup) onStartup(startup);
        }, Qt::QueuedConnection);
    };
    host_.network_snapshot = [this](const jam2::NetworkSessionSnapshot& snapshot) {
        publishNetworkSnapshot(snapshot);
    };
    host_.log = [this](std::string_view message) {
        const QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        QMetaObject::invokeMethod(this, [this, text] {
            if (onLog) onLog(text);
        }, Qt::QueuedConnection);
    };
    host_.error = [this](std::string_view message) {
        const QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        QMetaObject::invokeMethod(this, [this, text] {
            if (onError) onError(text);
        }, Qt::QueuedConnection);
    };
}

ApplicationRuntime::~ApplicationRuntime()
{
    shutdown();
}

bool ApplicationRuntime::ensureEngine(
    const Jam2RuntimeOptions& options,
    bool leaderAudioLocalClick)
{
    if (!options.audio_device_id && !options.headless_audio) {
        host_.engine = nullptr;
        return true;
    }
    const jam2::EngineConfig requested = jam2_make_engine_config(options, leaderAudioLocalClick);
    if (engine_ != nullptr) {
        const jam2::EngineConfig* active = engine_->config();
        if (active != nullptr && !jam2_engine_restart_required(*active, requested)) {
            ++engine_reuses_;
            host_.engine = engine_.get();
            jam2::EngineCommand command;
            command.type = jam2::EngineCommandType::SetLeaderAudioLocalClick;
            command.enabled = leaderAudioLocalClick;
            (void)engine_->submit(command);
            return true;
        }
        stopNetwork();
        engine_->requestStop();
        engine_->join();
        engine_.reset();
        ++engine_restarts_;
    }
    try {
        engine_ = std::make_unique<jam2::Engine>();
        engine_->start(requested);
        host_.engine = engine_.get();
        ++engine_starts_;
        return true;
    } catch (const std::exception& exception) {
        host_.engine = nullptr;
        engine_.reset();
        if (onError) onError(QString::fromUtf8(exception.what()));
        return false;
    }
}

bool ApplicationRuntime::startLocal(const Jam2RuntimeOptions& options)
{
    stopNetwork();
    return ensureEngine(options, true);
}

bool ApplicationRuntime::startNetwork(const Jam2RuntimeOptions& options)
{
    stopNetwork();
    if (!ensureEngine(options, false)) {
        return false;
    }
    host_.reset();
    host_.engine = engine_.get();
    host_.track_sync_enabled.store(
        track_sync_enabled_.load(std::memory_order_acquire),
        std::memory_order_release);
    network_running_.store(true, std::memory_order_release);
    network_worker_ = std::thread([this, options] {
        const int result = jam2_run_network_runtime(options, host_);
        network_running_.store(false, std::memory_order_release);
        QMetaObject::invokeMethod(this, [this, result] {
            if (onNetworkFinished) onNetworkFinished(result);
        }, Qt::QueuedConnection);
    });
    return true;
}

void ApplicationRuntime::stopNetwork()
{
    host_.stop_requested.store(true, std::memory_order_release);
    if (network_worker_.joinable()) {
        network_worker_.join();
    }
    network_running_.store(false, std::memory_order_release);
    host_.reset();
    std::lock_guard<std::mutex> lock(network_snapshot_mutex_);
    network_snapshot_.reset();
}

void ApplicationRuntime::shutdown()
{
    poll_timer_.stop();
    stopNetwork();
    if (engine_ != nullptr) {
        engine_->requestStop();
        engine_->join();
        engine_.reset();
    }
    host_.engine = nullptr;
}

bool ApplicationRuntime::isRunning() const noexcept
{
    return engine_ != nullptr && engine_->snapshot().lifecycle == jam2::EngineLifecycle::Local;
}

bool ApplicationRuntime::isNetworkRunning() const noexcept
{
    return network_running_.load(std::memory_order_acquire);
}

bool ApplicationRuntime::submit(const jam2::EngineCommand& command) noexcept
{
    if (engine_ == nullptr) {
        return false;
    }
    return isNetworkRunning() ? host_.submitCommand(command) : engine_->submit(command);
}

bool ApplicationRuntime::updatePeers(const std::vector<std::string>& peers)
{
    return isNetworkRunning() && host_.submitPeerUpdate(peers);
}

void ApplicationRuntime::setTrackSyncEnabled(bool enabled) noexcept
{
    track_sync_enabled_.store(enabled, std::memory_order_release);
    host_.track_sync_enabled.store(enabled, std::memory_order_release);
}

jam2::EngineSnapshot ApplicationRuntime::engineSnapshot() const noexcept
{
    return engine_ != nullptr ? engine_->snapshot() : jam2::EngineSnapshot{};
}

std::optional<jam2::NetworkSessionSnapshot> ApplicationRuntime::networkSnapshot() const
{
    std::lock_guard<std::mutex> lock(network_snapshot_mutex_);
    return network_snapshot_;
}

void ApplicationRuntime::pollEngine()
{
    if (engine_ == nullptr) {
        return;
    }
    const jam2::EngineSnapshot snapshot = engine_->snapshot();
    if (onEngineSnapshot) onEngineSnapshot(snapshot);
    jam2::EngineEvent event;
    std::size_t count = 0;
    while (count < 32 && engine_->pollEvent(event)) {
        if (onEngineEvent) onEngineEvent(event);
        ++count;
    }
}

void ApplicationRuntime::publishNetworkSnapshot(jam2::NetworkSessionSnapshot snapshot)
{
    {
        std::lock_guard<std::mutex> lock(network_snapshot_mutex_);
        network_snapshot_ = snapshot;
    }
    QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)] {
        if (onNetworkSnapshot) onNetworkSnapshot(snapshot);
    }, Qt::QueuedConnection);
}
