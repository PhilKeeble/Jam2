#pragma once

#include "RuntimeContracts.hpp"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// Non-UI owner for the persistent local Engine and optional UDP session
// worker. GUI and headless controllers provide typed configuration/actions;
// this class never constructs argv or invokes the CLI entry point.
class ApplicationRuntime final : public QObject {
public:
    explicit ApplicationRuntime(QObject* parent = nullptr);
    ~ApplicationRuntime() override;

    bool startLocal(const Jam2RuntimeOptions& options);
    bool startNetwork(const Jam2RuntimeOptions& options);
    void stopNetwork();
    void shutdown();

    bool isRunning() const noexcept;
    bool isNetworkRunning() const noexcept;
    bool submit(const jam2::EngineCommand& command) noexcept;
    bool updatePeers(const std::vector<Jam2RuntimePeer>& peers);
    void setTrackSyncEnabled(bool enabled) noexcept;

    jam2::EngineSnapshot engineSnapshot() const noexcept;
    jam2::EngineGuiPeakSnapshot consumeGuiPeaks() noexcept;
    std::optional<jam2::NetworkSessionSnapshot> networkSnapshot() const;
    std::uint64_t engineStarts() const noexcept { return engine_starts_; }
    std::uint64_t engineRestarts() const noexcept { return engine_restarts_; }
    std::uint64_t engineReuses() const noexcept { return engine_reuses_; }

    std::function<void(const jam2::EngineSnapshot&)> onEngineSnapshot;
    std::function<void(const jam2::EngineEvent&)> onEngineEvent;
    std::function<void(const jam2::NetworkSessionSnapshot&)> onNetworkSnapshot;
    std::function<void(const Jam2RuntimeStartup&)> onStartup;
    std::function<void(const QString&)> onLog;
    std::function<void(const QString&)> onError;
    std::function<void(int)> onNetworkFinished;

private:
    bool ensureEngine(const Jam2RuntimeOptions& options, bool leaderAudioLocalClick);
    void pollEngine();
    void publishNetworkSnapshot(jam2::NetworkSessionSnapshot snapshot);

    std::unique_ptr<jam2::Engine> engine_;
    Jam2RuntimeHost host_;
    std::thread network_worker_;
    std::atomic<bool> network_running_{false};
    std::atomic<bool> track_sync_enabled_{true};
    mutable std::mutex network_snapshot_mutex_;
    std::optional<jam2::NetworkSessionSnapshot> network_snapshot_;
    QTimer poll_timer_;
    std::uint64_t engine_starts_ = 0;
    std::uint64_t engine_restarts_ = 0;
    std::uint64_t engine_reuses_ = 0;
};
