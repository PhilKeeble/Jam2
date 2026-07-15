#pragma once

#include <QByteArray>
#include <QJsonObject>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class AutomationChannel final {
public:
    static constexpr std::size_t kMaxFrameBytes = 64 * 1024;
    static constexpr std::size_t kQueueCapacity = 128;
    static constexpr std::size_t kCommandsPerTurn = 8;
    static constexpr int kIncompleteFrameTimeoutMs = 5000;

    static std::unique_ptr<AutomationChannel> fromInheritedEnvironment(
        bool required,
        std::string& error);

    ~AutomationChannel();

    AutomationChannel(const AutomationChannel&) = delete;
    AutomationChannel& operator=(const AutomationChannel&) = delete;

    void start(
        std::function<void(QJsonObject)> command,
        std::function<void(std::string)> disconnected);
    bool send(QJsonObject event);
    void stop(bool drainEvents = false) noexcept;

    std::size_t queuedEvents() const noexcept;
    std::size_t eventQueueHighWater() const noexcept;
    std::uint64_t rejectedFrames() const noexcept;
    std::uint64_t rejectedEvents() const noexcept;

private:
    AutomationChannel(std::uintptr_t commandHandle, std::uintptr_t eventHandle);

    bool readExact(
        void* destination,
        std::size_t bytes,
        std::chrono::steady_clock::time_point& frameDeadline,
        std::string& error);
    bool writeExact(const void* source, std::size_t bytes, std::string& error);
    void readLoop();
    void writeLoop();
    void reportDisconnected(std::string error);
    void closeCommandHandle() noexcept;
    void closeEventHandle() noexcept;

    std::uintptr_t command_handle_ = 0;
    std::uintptr_t event_handle_ = 0;
    std::function<void(QJsonObject)> command_callback_;
    std::function<void(std::string)> disconnected_callback_;
    std::thread reader_;
    std::thread writer_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> cleanup_started_{false};
    std::atomic<bool> drain_events_on_stop_{false};
    std::atomic<bool> disconnect_reported_{false};
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::deque<QByteArray> event_queue_;
    std::size_t event_queue_high_water_ = 0;
    std::atomic<std::uint64_t> rejected_frames_{0};
    std::atomic<std::uint64_t> rejected_events_{0};
};
