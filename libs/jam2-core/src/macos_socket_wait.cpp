#include "macos_socket_wait.hpp"
#include "udp_socket.hpp"
#include "udp_wait.hpp"
#include <mach/mach_time.h>
#include <sys/event.h>
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <system_error>
#include <utility>
#include <vector>

namespace jam2 {
namespace {
struct Descriptor {
    int value = -1;
    ~Descriptor() { if (value >= 0) close(value); }
    Descriptor() = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
[[noreturn]] void fail(const char* operation) { throw std::system_error(errno, std::generic_category(), operation); }
}
struct MacSocketWaiter::Impl {
    Descriptor queue;
    mach_timebase_info_data_t base{};
    std::vector<int> input, desired, registered, ready;
    std::vector<kevent64_s> changes, events;
    std::vector<pollfd> polls;

    Impl() {
        if (mach_timebase_info(&base) != KERN_SUCCESS || !base.numer || !base.denom)
            throw std::runtime_error("failed to read UDP wait timebase");
        input.reserve(8); desired.reserve(8); registered.reserve(8); ready.reserve(8);
        changes.reserve(9); events.resize(9); polls.reserve(8);
    }
    std::uint64_t microseconds(std::uint64_t ticks) const noexcept {
        return static_cast<std::uint64_t>(static_cast<long double>(ticks) * base.numer / base.denom / 1000);
    }
    void ensureQueue() {
        if (queue.value >= 0) return;
        queue.value = kqueue();
        if (queue.value < 0) fail("create UDP kqueue");
        if (fcntl(queue.value, F_SETFD, FD_CLOEXEC) < 0) fail("set UDP kqueue close-on-exec");
    }
    MacSocketWaitResult legacy(std::uint64_t timeout, LegacyUdpWaitStyle style) {
        MacSocketWaitResult result;
        if (style == LegacyUdpWaitStyle::Poll || std::any_of(desired.begin(), desired.end(),
                [](int fd) { return fd >= FD_SETSIZE; })) {
            polls.clear();
            for (int fd : desired) polls.push_back({fd, POLLIN, 0});
            const auto milliseconds = static_cast<int>((std::min<std::uint64_t>(timeout, 60000000) + 999) / 1000);
            const int count = poll(polls.data(), static_cast<nfds_t>(polls.size()), milliseconds);
            if (count < 0 && errno == EINTR) { result.interrupted = true; return result; }
            if (count < 0) fail("legacy UDP poll");
            for (const auto& entry : polls) {
                if (entry.revents & POLLNVAL) { errno = EBADF; fail("legacy UDP poll descriptor"); }
                if (entry.revents & (POLLIN | POLLHUP | POLLERR)) ready.push_back(entry.fd);
            }
        } else {
            fd_set reads; FD_ZERO(&reads); int maximum = -1;
            for (int fd : desired) { FD_SET(fd, &reads); maximum = std::max(maximum, fd); }
            timeval value{}; value.tv_sec = timeout / 1000000; value.tv_usec = timeout % 1000000;
            const int count = select(maximum + 1, &reads, nullptr, nullptr, &value);
            if (count < 0 && errno == EINTR) { result.interrupted = true; return result; }
            if (count < 0) fail("legacy UDP select");
            for (int fd : desired) if (FD_ISSET(fd, &reads)) ready.push_back(fd);
        }
        result.readiness = !ready.empty(); result.deadline = !result.readiness;
        return result;
    }
    void cancelTimer() {
        kevent64_s remove{}; EV_SET64(&remove, 1, EVFILT_TIMER, EV_DELETE, 0, 0, 0, 0, 0);
        if (kevent64(queue.value, &remove, 1, nullptr, 0, 0, nullptr) < 0 && errno != ENOENT)
            fail("cancel UDP deadline");
    }
    MacSocketWaitResult critical(std::uint64_t timeout, std::uint64_t start) {
        ensureQueue();
        for (int fd : registered) if (std::find(desired.begin(), desired.end(), fd) == desired.end()) {
            kevent64_s remove{}; EV_SET64(&remove, fd, EVFILT_READ, EV_DELETE, 0, 0, 0, 0, 0);
            if (kevent64(queue.value, &remove, 1, nullptr, 0, 0, nullptr) < 0 && errno != ENOENT && errno != EBADF)
                fail("remove UDP wait descriptor");
        }
        changes.clear();
        // EV_ADD refreshes every interest in the same syscall as arming the
        // deadline. A closed/reused numeric fd must register its new owner.
        for (int fd : desired) {
            kevent64_s add{}; EV_SET64(&add, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0, 0, 0);
            changes.push_back(add);
        }
        if (timeout) {
            const auto ticks = static_cast<std::uint64_t>((static_cast<long double>(timeout) * 1000 * base.denom + base.numer - 1) / base.numer);
            kevent64_s timer{};
            EV_SET64(&timer, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
                NOTE_CRITICAL | NOTE_LEEWAY | NOTE_MACHTIME | NOTE_ABSOLUTE, start + ticks, 0, 0, 0);
            changes.push_back(timer);
        }
        events.resize(desired.size() + 1);
        registered = desired;
        const int count = kevent64(queue.value, changes.data(), static_cast<int>(changes.size()),
            events.data(), static_cast<int>(events.size()), timeout ? 0 : KEVENT_FLAG_IMMEDIATE, nullptr);
        MacSocketWaitResult result;
        if (count < 0) {
            const int error = errno;
            if (timeout) cancelTimer();
            if (error == EINTR) { result.interrupted = true; return result; }
            errno = error; fail("wait for UDP readiness/deadline");
        }
        for (int i = 0; i < count; ++i) {
            const auto& event = events[static_cast<std::size_t>(i)];
            if (event.flags & EV_ERROR) {
                if (timeout) cancelTimer();
                errno = static_cast<int>(event.data); fail("register UDP wait event");
            }
            if (event.filter == EVFILT_TIMER) result.deadline = true;
            else if (event.filter == EVFILT_READ) ready.push_back(static_cast<int>(event.ident));
        }
        if (timeout && !result.deadline) cancelTimer();
        result.readiness = !ready.empty();
        if (!timeout && !result.readiness) result.deadline = true;
        return result;
    }
};
MacSocketWaiter::MacSocketWaiter() : impl_(std::make_unique<Impl>()) {}
MacSocketWaiter::~MacSocketWaiter() = default;
MacSocketWaitResult MacSocketWaiter::waitForRead(std::span<const int> descriptors,
    std::uint64_t timeout, LegacyUdpWaitStyle legacyStyle)
{
    timeout = std::min<std::uint64_t>(timeout, 24ULL * 60 * 60 * 1000000);
    const auto start = mach_absolute_time();
    auto* diagnostics = current_udp_wait_diagnostics();
    try {
        impl_->ready.clear(); impl_->desired.clear();
        if (legacyStyle != LegacyUdpWaitStyle::Select && legacyStyle != LegacyUdpWaitStyle::Poll)
            throw std::invalid_argument("invalid legacy UDP wait style");
        if (descriptors.size() >= INT_MAX) throw std::invalid_argument("too many UDP wait descriptors");
        for (int fd : descriptors) {
            if (fd < 0) throw std::invalid_argument("negative UDP wait descriptor");
            if (std::find(impl_->desired.begin(), impl_->desired.end(), fd) == impl_->desired.end())
                impl_->desired.push_back(fd);
        }
        impl_->ready.reserve(impl_->desired.size());
        const auto result = current_udp_wait_mode() == UdpWaitMode::Legacy
            ? impl_->legacy(timeout, legacyStyle) : impl_->critical(timeout, start);
        if (diagnostics) diagnostics->record(timeout, impl_->microseconds(mach_absolute_time() - start),
            result.readiness, result.deadline, result.interrupted, false);
        return result;
    } catch (...) {
        if (diagnostics) diagnostics->record(timeout, impl_->microseconds(mach_absolute_time() - start), false, false, false, true);
        throw;
    }
}
bool MacSocketWaiter::readable(int descriptor) const noexcept {
    return std::find(impl_->ready.begin(), impl_->ready.end(), descriptor) != impl_->ready.end();
}
MacSocketWaitResult MacSocketWaiter::waitUdp(std::span<const UdpSocket* const> sockets,
    std::uint64_t timeout, RealtimeWakeSignal* primary, RealtimeWakeSignal* secondary, LegacyUdpWaitStyle style)
{
    impl_->input.clear();
    for (const auto* socket : sockets) impl_->input.push_back(socket->handle_);
    if (primary) impl_->input.push_back(primary->read_handle_);
    if (secondary && secondary != primary) impl_->input.push_back(secondary->read_handle_);
    auto result = waitForRead(impl_->input, timeout, style);
    for (const auto* socket : sockets) result.datagram |= readable(socket->handle_);
    result.primary_wake = primary && readable(primary->read_handle_);
    result.secondary_wake = secondary && secondary != primary && readable(secondary->read_handle_);
    if (result.primary_wake) primary->consume();
    if (result.secondary_wake) secondary->consume();
    return result;
}
MacSocketWaiter& macos_thread_socket_waiter() {
    thread_local MacSocketWaiter waiter;
    return waiter;
}
} // namespace jam2
