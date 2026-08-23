#include "udp_socket.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace jam2 {
namespace {

std::string socket_error_text()
{
#if defined(_WIN32)
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

sockaddr_in to_sockaddr(const Endpoint& endpoint)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    if (endpoint.host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
        return addr;
    }
    if (inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) == 1) {
        return addr;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(endpoint.host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        throw std::runtime_error("failed to resolve endpoint host: " + endpoint.host);
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return addr;
}

Endpoint from_sockaddr(const sockaddr_in& addr)
{
    std::array<char, 64> host{};
    if (inet_ntop(AF_INET, &addr.sin_addr, host.data(), static_cast<socklen_t>(host.size())) == nullptr) {
        throw std::runtime_error("failed to format IPv4 address");
    }
    return Endpoint{host.data(), ntohs(addr.sin_port)};
}

ResolvedUdpEndpoint resolved_from_sockaddr(const sockaddr_in& addr) noexcept
{
    return ResolvedUdpEndpoint{addr.sin_addr.s_addr, ntohs(addr.sin_port)};
}

sockaddr_in to_sockaddr(const ResolvedUdpEndpoint& endpoint) noexcept
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    addr.sin_addr.s_addr = endpoint.address;
    return addr;
}

int last_socket_error() noexcept
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

UdpSendOutcome classify_send_error(int error) noexcept
{
#if defined(_WIN32)
    switch (error) {
    case WSAEWOULDBLOCK: return UdpSendOutcome::WouldBlock;
    case WSAENOBUFS: return UdpSendOutcome::NoBufferSpace;
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH: return UdpSendOutcome::Unreachable;
    case WSAECONNREFUSED: return UdpSendOutcome::Refused;
    default: return UdpSendOutcome::Fatal;
    }
#else
    if (error == EAGAIN || error == EWOULDBLOCK) {
        return UdpSendOutcome::WouldBlock;
    }
    switch (error) {
    case ENOBUFS: return UdpSendOutcome::NoBufferSpace;
    case EHOSTUNREACH:
    case ENETUNREACH: return UdpSendOutcome::Unreachable;
    case ECONNREFUSED: return UdpSendOutcome::Refused;
    default: return UdpSendOutcome::Fatal;
    }
#endif
}

} // namespace

ResolvedUdpEndpoint resolve_udp_endpoint(const Endpoint& endpoint)
{
    return resolved_from_sockaddr(to_sockaddr(endpoint));
}

Endpoint format_udp_endpoint(const ResolvedUdpEndpoint& endpoint)
{
    return from_sockaddr(to_sockaddr(endpoint));
}

NetworkRuntime::NetworkRuntime()
{
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
}

NetworkRuntime::~NetworkRuntime()
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

UdpSocket::UdpSocket()
{
    handle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (handle_ == INVALID_SOCKET) {
#else
    if (handle_ < 0) {
#endif
        throw std::runtime_error("failed to create UDP socket: " + socket_error_text());
    }
#if defined(_WIN32)
    u_long enabled = 1;
    if (ioctlsocket(handle_, FIONBIO, &enabled) != 0) {
        const std::string error = socket_error_text();
        closesocket(handle_);
        handle_ = INVALID_SOCKET;
        throw std::runtime_error("failed to make UDP socket nonblocking: " + error);
    }
    receive_event_ = WSACreateEvent();
    if (receive_event_ == WSA_INVALID_EVENT ||
        WSAEventSelect(handle_, receive_event_, FD_READ | FD_CLOSE) != 0) {
        const std::string error = socket_error_text();
        if (receive_event_ != WSA_INVALID_EVENT) WSACloseEvent(receive_event_);
        closesocket(handle_);
        receive_event_ = WSA_INVALID_EVENT;
        handle_ = INVALID_SOCKET;
        throw std::runtime_error("failed to create UDP receive event: " + error);
    }
#else
    const int flags = fcntl(handle_, F_GETFL, 0);
    if (flags < 0 || fcntl(handle_, F_SETFL, flags | O_NONBLOCK) != 0) {
        const std::string error = socket_error_text();
        close(handle_);
        handle_ = -1;
        throw std::runtime_error("failed to make UDP socket nonblocking: " + error);
    }
#endif
}

UdpSocket::~UdpSocket()
{
#if defined(_WIN32)
    if (receive_event_ != WSA_INVALID_EVENT) {
        WSACloseEvent(receive_event_);
    }
    if (handle_ != INVALID_SOCKET) {
        closesocket(handle_);
    }
#else
    if (handle_ >= 0) {
        close(handle_);
    }
#endif
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_)
#if defined(_WIN32)
    , receive_event_(other.receive_event_)
#endif
{
#if defined(_WIN32)
    other.handle_ = INVALID_SOCKET;
    other.receive_event_ = WSA_INVALID_EVENT;
#else
    other.handle_ = -1;
#endif
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
#if defined(_WIN32)
        if (receive_event_ != WSA_INVALID_EVENT) {
            WSACloseEvent(receive_event_);
        }
        if (handle_ != INVALID_SOCKET) {
            closesocket(handle_);
        }
#else
        if (handle_ >= 0) {
            close(handle_);
        }
#endif
        handle_ = other.handle_;
#if defined(_WIN32)
        receive_event_ = other.receive_event_;
        other.handle_ = INVALID_SOCKET;
        other.receive_event_ = WSA_INVALID_EVENT;
#else
        other.handle_ = -1;
#endif
    }
    return *this;
}

void UdpSocket::bind(const Endpoint& endpoint)
{
    const sockaddr_in addr = to_sockaddr(endpoint);
    if (::bind(handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("failed to bind UDP socket: " + socket_error_text());
    }
}

Endpoint UdpSocket::local_endpoint() const
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        throw std::runtime_error("failed to read local UDP endpoint: " + socket_error_text());
    }
    return from_sockaddr(addr);
}

void UdpSocket::set_send_buffer_size(int bytes)
{
    if (bytes <= 0) {
        throw std::runtime_error("UDP send buffer size must be positive");
    }
    if (setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) {
        throw std::runtime_error("failed to set UDP send buffer size: " + socket_error_text());
    }
}

void UdpSocket::set_recv_buffer_size(int bytes)
{
    if (bytes <= 0) {
        throw std::runtime_error("UDP receive buffer size must be positive");
    }
    if (setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) {
        throw std::runtime_error("failed to set UDP receive buffer size: " + socket_error_text());
    }
}

int UdpSocket::send_buffer_size() const
{
    int bytes = 0;
    socklen_t len = sizeof(bytes);
    if (getsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&bytes), &len) != 0) {
        throw std::runtime_error("failed to read UDP send buffer size: " + socket_error_text());
    }
    return bytes;
}

int UdpSocket::recv_buffer_size() const
{
    int bytes = 0;
    socklen_t len = sizeof(bytes);
    if (getsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&bytes), &len) != 0) {
        throw std::runtime_error("failed to read UDP receive buffer size: " + socket_error_text());
    }
    return bytes;
}

UdpSendResult UdpSocket::send_to(
    const ResolvedUdpEndpoint& endpoint,
    std::span<const std::uint8_t> bytes) const noexcept
{
    const sockaddr_in addr = to_sockaddr(endpoint);
    const int sent = ::sendto(
        handle_,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&addr),
        sizeof(addr));
    if (sent < 0) {
        const int error = last_socket_error();
        return UdpSendResult{classify_send_error(error), error};
    }
    if (static_cast<std::size_t>(sent) != bytes.size()) {
        return UdpSendResult{UdpSendOutcome::Fatal, 0};
    }
    return {};
}

RealtimeWakeSignal::RealtimeWakeSignal()
{
#if defined(_WIN32)
    event_ = WSACreateEvent();
    if (event_ == WSA_INVALID_EVENT) {
        throw std::runtime_error("failed to create real-time wake event: " + socket_error_text());
    }
#else
    int handles[2]{-1, -1};
    if (pipe(handles) != 0) {
        throw std::runtime_error("failed to create real-time wake pipe: " + socket_error_text());
    }
    read_handle_ = handles[0];
    write_handle_ = handles[1];
    const int read_flags = fcntl(read_handle_, F_GETFL, 0);
    const int write_flags = fcntl(write_handle_, F_GETFL, 0);
    const int read_descriptor_flags = fcntl(read_handle_, F_GETFD, 0);
    const int write_descriptor_flags = fcntl(write_handle_, F_GETFD, 0);
    if (read_flags < 0 || write_flags < 0 ||
        read_descriptor_flags < 0 || write_descriptor_flags < 0 ||
        fcntl(read_handle_, F_SETFL, read_flags | O_NONBLOCK) != 0 ||
        fcntl(write_handle_, F_SETFL, write_flags | O_NONBLOCK) != 0 ||
        fcntl(read_handle_, F_SETFD, read_descriptor_flags | FD_CLOEXEC) != 0 ||
        fcntl(write_handle_, F_SETFD, write_descriptor_flags | FD_CLOEXEC) != 0) {
        const std::string error = socket_error_text();
        close(read_handle_);
        close(write_handle_);
        read_handle_ = -1;
        write_handle_ = -1;
        throw std::runtime_error("failed to configure real-time wake pipe: " + error);
    }
#endif
}

RealtimeWakeSignal::~RealtimeWakeSignal()
{
#if defined(_WIN32)
    if (event_ != WSA_INVALID_EVENT) {
        WSACloseEvent(event_);
    }
#else
    if (read_handle_ >= 0) close(read_handle_);
    if (write_handle_ >= 0) close(write_handle_);
#endif
}

void RealtimeWakeSignal::signalNative() noexcept
{
#if defined(_WIN32)
    (void)WSASetEvent(event_);
#else
    constexpr std::uint8_t value = 1;
    (void)write(write_handle_, &value, sizeof(value));
#endif
}

void RealtimeWakeSignal::requestWake() noexcept
{
    wake_requested_.store(true, std::memory_order_release);
}

void RealtimeWakeSignal::cancelWakeRequest() noexcept
{
    wake_requested_.store(false, std::memory_order_release);
    if (pending_.load(std::memory_order_acquire)) {
        consume();
    }
}

void RealtimeWakeSignal::signal(std::uint64_t ready_time_us) noexcept
{
    if (!wake_requested_.load(std::memory_order_acquire)) {
        return;
    }
    request_generation_.fetch_add(1, std::memory_order_release);
    if (pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    last_signal_time_us_.store(ready_time_us, std::memory_order_relaxed);
    signal_count_.fetch_add(1, std::memory_order_release);
    signalNative();
}

void RealtimeWakeSignal::consume() noexcept
{
    const std::uint64_t generation_before =
        request_generation_.load(std::memory_order_acquire);
#if defined(_WIN32)
    (void)WSAResetEvent(event_);
#else
    std::array<std::uint8_t, 64> drain{};
    while (read(read_handle_, drain.data(), drain.size()) > 0) {
    }
#endif
    pending_.store(false, std::memory_order_release);
    consumption_count_.fetch_add(1, std::memory_order_relaxed);

    // If the callback tried to signal while the old notification was being
    // consumed, re-arm it here. A callback racing after pending_ is cleared
    // performs the normal transition itself.
    if (request_generation_.load(std::memory_order_acquire) != generation_before &&
        wake_requested_.load(std::memory_order_acquire) &&
        !pending_.exchange(true, std::memory_order_acq_rel)) {
        signalNative();
    }
}

std::uint64_t RealtimeWakeSignal::signalCount() const noexcept
{
    return signal_count_.load(std::memory_order_acquire);
}

std::uint64_t RealtimeWakeSignal::consumptionCount() const noexcept
{
    return consumption_count_.load(std::memory_order_relaxed);
}

std::uint64_t RealtimeWakeSignal::lastSignalTimeUs() const noexcept
{
    return last_signal_time_us_.load(std::memory_order_relaxed);
}

std::optional<UdpSocket::ReceivedDatagram> UdpSocket::recv_from(
    std::span<std::uint8_t> buffer,
    int timeout_ms) const
{
    const std::uint64_t timeout_us = timeout_ms > 0
        ? static_cast<std::uint64_t>(timeout_ms) * 1000ULL
        : 0ULL;
    return recv_from_for(buffer, timeout_us);
}

std::optional<UdpSocket::ReceivedDatagram> UdpSocket::recv_from_for(
    std::span<std::uint8_t> buffer,
    std::uint64_t timeout_us,
    RealtimeWakeSignal* wake_signal) const
{
    if (timeout_us > 0) {
        constexpr std::uint64_t kMaxSelectTimeoutUs =
            24ULL * 60ULL * 60ULL * 1000000ULL;
        timeout_us = (std::min)(timeout_us, kMaxSelectTimeoutUs);
        int ready = 0;
        bool socket_ready = false;
#if defined(_WIN32)
        if (wake_signal != nullptr) {
            fd_set immediate_read_set;
            FD_ZERO(&immediate_read_set);
            FD_SET(handle_, &immediate_read_set);
            timeval immediate_timeout{};
            ready = select(
                static_cast<int>(handle_ + 1),
                &immediate_read_set,
                nullptr,
                nullptr,
                &immediate_timeout);
            socket_ready = ready > 0 && FD_ISSET(handle_, &immediate_read_set);
            if (!socket_ready && ready >= 0) {
                WSANETWORKEVENTS pending_events{};
                if (WSAEnumNetworkEvents(handle_, receive_event_, &pending_events) != 0) {
                    throw std::runtime_error("UDP event query failed: " + socket_error_text());
                }
                socket_ready = (pending_events.lNetworkEvents & FD_READ) != 0;
            }
            if (!socket_ready && ready >= 0) {
                const DWORD timeout_ms = static_cast<DWORD>(
                    (timeout_us + 999ULL) / 1000ULL);
                const WSAEVENT events[2]{
                    receive_event_,
                    wake_signal->event_,
                };
                const DWORD wait_result = WSAWaitForMultipleEvents(
                    2, events, FALSE, timeout_ms, FALSE);
                if (wait_result == WSA_WAIT_FAILED) {
                    throw std::runtime_error(
                        "UDP/wake wait failed: " + socket_error_text());
                }
                if (wait_result == WSA_WAIT_TIMEOUT) return std::nullopt;
                socket_ready = wait_result == WSA_WAIT_EVENT_0;
                if (socket_ready) {
                    WSANETWORKEVENTS received_events{};
                    if (WSAEnumNetworkEvents(
                            handle_, receive_event_, &received_events) != 0) {
                        throw std::runtime_error(
                            "UDP event query failed: " + socket_error_text());
                    }
                    socket_ready = (received_events.lNetworkEvents & FD_READ) != 0;
                }
            }
            if (WSAWaitForMultipleEvents(
                    1, &wake_signal->event_, FALSE, 0, FALSE) ==
                WSA_WAIT_EVENT_0) {
                wake_signal->consume();
            }
            if (!socket_ready) return std::nullopt;
        } else {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(handle_, &read_set);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(timeout_us / 1000000ULL);
            timeout.tv_usec = static_cast<long>(timeout_us % 1000000ULL);
            ready = select(
                static_cast<int>(handle_ + 1), &read_set, nullptr, nullptr, &timeout);
            socket_ready = ready > 0 && FD_ISSET(handle_, &read_set);
        }
#else
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(handle_, &read_set);
        int maximum_handle = handle_;
        if (wake_signal != nullptr) {
            FD_SET(wake_signal->read_handle_, &read_set);
            maximum_handle = (std::max)(maximum_handle, wake_signal->read_handle_);
        }
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(timeout_us / 1000000ULL);
        timeout.tv_usec = static_cast<long>(timeout_us % 1000000ULL);
        ready = select(maximum_handle + 1, &read_set, nullptr, nullptr, &timeout);
        socket_ready = ready > 0 && FD_ISSET(handle_, &read_set);
        if (ready > 0 && wake_signal != nullptr &&
            FD_ISSET(wake_signal->read_handle_, &read_set)) {
            wake_signal->consume();
        }
#endif
        if (ready < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEINTR) return std::nullopt;
#else
            if (errno == EINTR) return std::nullopt;
#endif
            throw std::runtime_error("UDP select failed: " + socket_error_text());
        }
        if (ready == 0) return std::nullopt;
        if (!socket_ready) return std::nullopt;
    }

    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
#if defined(_WIN32)
    const int received = ::recvfrom(
        handle_,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&from),
        &from_len);
#else
    iovec vector{};
    vector.iov_base = buffer.data();
    vector.iov_len = buffer.size();
    msghdr message{};
    message.msg_name = &from;
    message.msg_namelen = from_len;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    const ssize_t received = ::recvmsg(handle_, &message, 0);
    from_len = message.msg_namelen;
#endif
    if (received < 0) {
#if defined(_WIN32)
        const int error = WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNREFUSED ||
            error == WSAEHOSTUNREACH || error == WSAENETUNREACH ||
            error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAEMSGSIZE) {
            return std::nullopt;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
            errno == EMSGSIZE || errno == ECONNREFUSED ||
            errno == EHOSTUNREACH || errno == ENETUNREACH) {
            return std::nullopt;
        }
#endif
        throw std::runtime_error("UDP receive failed: " + socket_error_text());
    }
#if !defined(_WIN32)
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        return std::nullopt;
    }
#endif
    return ReceivedDatagram{resolved_from_sockaddr(from), static_cast<std::size_t>(received)};
}

} // namespace jam2
