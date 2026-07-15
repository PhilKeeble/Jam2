#include "AutomationChannel.hpp"

#include <QJsonDocument>
#include <QProcessEnvironment>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {

constexpr auto kCommandHandleVariable = "JAM2_AUTOMATION_COMMAND_HANDLE";
constexpr auto kEventHandleVariable = "JAM2_AUTOMATION_EVENT_HANDLE";

bool parseHandle(const QString& text, std::uintptr_t& result)
{
    bool ok = false;
    const qulonglong parsed = text.toULongLong(&ok, 10);
    if (!ok || parsed == 0 || parsed > std::numeric_limits<std::uintptr_t>::max()) {
        return false;
    }
    result = static_cast<std::uintptr_t>(parsed);
    return true;
}

std::array<std::uint8_t, 4> framePrefix(std::size_t size)
{
    return {
        static_cast<std::uint8_t>(size & 0xffU),
        static_cast<std::uint8_t>((size >> 8U) & 0xffU),
        static_cast<std::uint8_t>((size >> 16U) & 0xffU),
        static_cast<std::uint8_t>((size >> 24U) & 0xffU),
    };
}

std::uint32_t decodeFrameSize(const std::array<std::uint8_t, 4>& prefix)
{
    return static_cast<std::uint32_t>(prefix[0]) |
        (static_cast<std::uint32_t>(prefix[1]) << 8U) |
        (static_cast<std::uint32_t>(prefix[2]) << 16U) |
        (static_cast<std::uint32_t>(prefix[3]) << 24U);
}

} // namespace

std::unique_ptr<AutomationChannel> AutomationChannel::fromInheritedEnvironment(
    bool required,
    std::string& error)
{
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString commandText = environment.value(QString::fromLatin1(kCommandHandleVariable));
    const QString eventText = environment.value(QString::fromLatin1(kEventHandleVariable));
    if (commandText.isEmpty() && eventText.isEmpty()) {
        if (required) {
            error = "reactive debug run requires both inherited automation handles";
        }
        return nullptr;
    }
    if (!required) {
        error = "automation handles are accepted only by a reactive debug run";
        return nullptr;
    }
    std::uintptr_t commandHandle = 0;
    std::uintptr_t eventHandle = 0;
    if (!parseHandle(commandText, commandHandle) || !parseHandle(eventText, eventHandle)) {
        error = "automation handle pair is missing or invalid";
        return nullptr;
    }
    return std::unique_ptr<AutomationChannel>(
        new AutomationChannel(commandHandle, eventHandle));
}

AutomationChannel::AutomationChannel(
    std::uintptr_t commandHandle,
    std::uintptr_t eventHandle)
    : command_handle_(commandHandle), event_handle_(eventHandle)
{
#if !defined(_WIN32)
    const int descriptor = static_cast<int>(event_handle_);
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

AutomationChannel::~AutomationChannel()
{
    stop();
}

void AutomationChannel::start(
    std::function<void(QJsonObject)> command,
    std::function<void(std::string)> disconnected)
{
    command_callback_ = std::move(command);
    disconnected_callback_ = std::move(disconnected);
    reader_ = std::thread([this] { readLoop(); });
    writer_ = std::thread([this] { writeLoop(); });
}

bool AutomationChannel::send(QJsonObject event)
{
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    event.insert(QStringLiteral("format"), QStringLiteral("jam2-automation"));
    const QByteArray payload = QJsonDocument(event).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || static_cast<std::size_t>(payload.size()) > kMaxFrameBytes) {
        rejected_events_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (event_queue_.size() >= kQueueCapacity) {
            rejected_events_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        event_queue_.push_back(payload);
        event_queue_high_water_ = std::max(event_queue_high_water_, event_queue_.size());
    }
    queue_ready_.notify_one();
    return true;
}

void AutomationChannel::stop(bool drainEvents) noexcept
{
    if (cleanup_started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    drain_events_on_stop_.store(drainEvents, std::memory_order_release);
    stopping_.store(true, std::memory_order_release);
    if (!drainEvents) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        rejected_events_.fetch_add(event_queue_.size(), std::memory_order_relaxed);
        event_queue_.clear();
    }
    queue_ready_.notify_all();
#if defined(_WIN32)
    if (reader_.joinable()) {
        (void)CancelSynchronousIo(static_cast<HANDLE>(reader_.native_handle()));
    }
    if (!drainEvents && writer_.joinable()) {
        (void)CancelSynchronousIo(static_cast<HANDLE>(writer_.native_handle()));
    }
#endif
    closeCommandHandle();
    if (reader_.joinable()) {
        reader_.join();
    }
    if (writer_.joinable()) {
        writer_.join();
    }
    closeEventHandle();
}

std::size_t AutomationChannel::queuedEvents() const noexcept
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return event_queue_.size();
}

std::size_t AutomationChannel::eventQueueHighWater() const noexcept
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return event_queue_high_water_;
}

std::uint64_t AutomationChannel::rejectedFrames() const noexcept
{
    return rejected_frames_.load(std::memory_order_relaxed);
}

std::uint64_t AutomationChannel::rejectedEvents() const noexcept
{
    return rejected_events_.load(std::memory_order_relaxed);
}

bool AutomationChannel::readExact(
    void* destination,
    std::size_t bytes,
    std::chrono::steady_clock::time_point& frameDeadline,
    std::string& error)
{
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < bytes && !stopping_.load(std::memory_order_acquire)) {
        if (frameDeadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= frameDeadline) {
            error = "automation frame timed out before completion";
            return false;
        }
#if defined(_WIN32)
        DWORD available = 0;
        const HANDLE handle = reinterpret_cast<HANDLE>(command_handle_);
        if (handle == nullptr ||
            !PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            error = "automation command pipe closed";
            return false;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        DWORD read = 0;
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes - offset, available));
        if (!ReadFile(handle, output + offset, requested, &read, nullptr) || read == 0) {
            error = "automation command pipe read failed";
            return false;
        }
        if (frameDeadline == std::chrono::steady_clock::time_point::max()) {
            frameDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kIncompleteFrameTimeoutMs);
        }
        offset += read;
#else
        pollfd descriptor{static_cast<int>(command_handle_), POLLIN, 0};
        const int polled = ::poll(&descriptor, 1, 25);
        if (polled < 0 && errno != EINTR) {
            error = "automation command pipe poll failed";
            return false;
        }
        if (polled <= 0) {
            continue;
        }
        const ssize_t read = ::read(
            static_cast<int>(command_handle_), output + offset, bytes - offset);
        if (read <= 0) {
            error = "automation command pipe closed";
            return false;
        }
        if (frameDeadline == std::chrono::steady_clock::time_point::max()) {
            frameDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kIncompleteFrameTimeoutMs);
        }
        offset += static_cast<std::size_t>(read);
#endif
    }
    return offset == bytes;
}

bool AutomationChannel::writeExact(const void* source, std::size_t bytes, std::string& error)
{
    const auto* input = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kIncompleteFrameTimeoutMs);
    while (offset < bytes) {
        if ((stopping_.load(std::memory_order_acquire) &&
             !drain_events_on_stop_.load(std::memory_order_acquire)) ||
            std::chrono::steady_clock::now() >= deadline) {
            error = "automation event frame write timed out or was cancelled";
            return false;
        }
#if defined(_WIN32)
        DWORD written = 0;
        const HANDLE handle = reinterpret_cast<HANDLE>(event_handle_);
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        if (handle == nullptr ||
            !WriteFile(handle, input + offset, requested, &written, nullptr) || written == 0) {
            error = "automation event pipe write failed";
            return false;
        }
        offset += written;
#else
        const ssize_t written = ::write(
            static_cast<int>(event_handle_), input + offset, bytes - offset);
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            pollfd descriptor{static_cast<int>(event_handle_), POLLOUT, 0};
            (void)::poll(&descriptor, 1, 25);
            continue;
        }
        if (written <= 0) {
            error = "automation event pipe write failed";
            return false;
        }
        offset += static_cast<std::size_t>(written);
#endif
    }
    return offset == bytes;
}

void AutomationChannel::readLoop()
{
    std::string error;
    while (!stopping_.load(std::memory_order_acquire)) {
        std::array<std::uint8_t, 4> prefix{};
        auto frameDeadline = std::chrono::steady_clock::time_point::max();
        if (!readExact(prefix.data(), prefix.size(), frameDeadline, error)) {
            break;
        }
        const std::uint32_t size = decodeFrameSize(prefix);
        if (size == 0 || size > kMaxFrameBytes) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            error = "automation command frame size is outside its bound";
            break;
        }
        QByteArray payload(static_cast<qsizetype>(size), Qt::Uninitialized);
        if (!readExact(payload.data(), size, frameDeadline, error)) {
            break;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        if (!document.isObject() || parseError.error != QJsonParseError::NoError ||
            document.object().value(QStringLiteral("format")).toString() !=
                QStringLiteral("jam2-automation")) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!stopping_.load(std::memory_order_acquire) && command_callback_) {
            command_callback_(document.object());
        }
    }
    if (!stopping_.load(std::memory_order_acquire)) {
        reportDisconnected(error);
    }
}

void AutomationChannel::writeLoop()
{
    for (;;) {
        QByteArray payload;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_ready_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) || !event_queue_.empty();
            });
            if (event_queue_.empty()) {
                return;
            }
            payload = std::move(event_queue_.front());
            event_queue_.pop_front();
        }
        const auto prefix = framePrefix(static_cast<std::size_t>(payload.size()));
        QByteArray frame;
        frame.reserve(static_cast<qsizetype>(prefix.size()) + payload.size());
        frame.append(reinterpret_cast<const char*>(prefix.data()),
                     static_cast<qsizetype>(prefix.size()));
        frame.append(payload);
        std::string error;
        if (!writeExact(frame.constData(), static_cast<std::size_t>(frame.size()), error)) {
            stopping_.store(true, std::memory_order_release);
            queue_ready_.notify_all();
            reportDisconnected(error);
            return;
        }
    }
}

void AutomationChannel::reportDisconnected(std::string error)
{
    if (disconnect_reported_.exchange(true, std::memory_order_acq_rel) ||
        !disconnected_callback_) {
        return;
    }
    disconnected_callback_(
        error.empty() ? "automation controller disconnected" : std::move(error));
}

void AutomationChannel::closeCommandHandle() noexcept
{
    if (command_handle_ == 0) {
        return;
    }
#if defined(_WIN32)
    (void)CloseHandle(reinterpret_cast<HANDLE>(command_handle_));
#else
    (void)::close(static_cast<int>(command_handle_));
#endif
    command_handle_ = 0;
}

void AutomationChannel::closeEventHandle() noexcept
{
    if (event_handle_ == 0) {
        return;
    }
#if defined(_WIN32)
    (void)CloseHandle(reinterpret_cast<HANDLE>(event_handle_));
#else
    (void)::close(static_cast<int>(event_handle_));
#endif
    event_handle_ = 0;
}
