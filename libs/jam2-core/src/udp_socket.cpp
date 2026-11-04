#include "udp_socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include "network_framework_udp.hpp"
#include "macos_socket_wait.hpp"
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

UdpSocket::UdpSocket(UdpBackend backend, UdpServiceClass service) : requested_backend_(backend)
{
    if ((backend!=UdpBackend::Bsd && backend!=UdpBackend::NetworkFramework && backend!=UdpBackend::ConnectedBsd) ||
        (service!=UdpServiceClass::BestEffort && service!=UdpServiceClass::InteractiveVoice))
        throw std::invalid_argument("invalid UDP backend or service class");
#if !defined(__APPLE__)
    if (backend != UdpBackend::Bsd || service != UdpServiceClass::BestEffort)
        throw std::runtime_error("experimental UDP backend/service options require macOS");
#endif
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
#if defined(__APPLE__)
    const int service_type = service == UdpServiceClass::InteractiveVoice ? NET_SERVICE_TYPE_VO : NET_SERVICE_TYPE_BE;
    if (setsockopt(handle_, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service_type, sizeof(service_type)) != 0) {
        const std::string error = socket_error_text(); close(handle_); handle_ = -1;
        throw std::runtime_error("failed to configure UDP service type: " + error);
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
    : receive_timestamps_(other.receive_timestamps_), requested_backend_(other.requested_backend_),
      connected_peer_(other.connected_peer_), connected_endpoint_(other.connected_endpoint_)
#if defined(__APPLE__)
    , network_framework_(std::move(other.network_framework_))
#endif
    , handle_(other.handle_)
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
        receive_timestamps_ = other.receive_timestamps_;
        requested_backend_ = other.requested_backend_;
        connected_peer_ = other.connected_peer_; connected_endpoint_ = other.connected_endpoint_;
#if defined(__APPLE__)
        network_framework_ = std::move(other.network_framework_);
#endif
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

void UdpSocket::set_receive_timestamps(bool enabled)
{
#if defined(__APPLE__)
    if (enabled) (void)receive_diagnostic_time_us(); // Initialise timebase before packet service.
    if (network_framework_) { network_framework_->set_receive_timestamps(enabled); receive_timestamps_=enabled; return; }
    const int value = enabled ? 1 : 0;
    if (setsockopt(handle_, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &value, sizeof(value)) != 0)
        throw std::runtime_error("failed to configure UDP receive timestamps: " + socket_error_text());
    receive_timestamps_ = enabled;
#else
    if (enabled) throw std::runtime_error("kernel UDP receive timestamps are supported only on macOS");
#endif
}

void UdpSocket::activate_peer(const ResolvedUdpEndpoint& remote)
{
    if (requested_backend_ == UdpBackend::Bsd) return;
#if defined(__APPLE__)
    if (requested_backend_ == UdpBackend::ConnectedBsd) {
        if (connected_peer_) return;
        const auto address=to_sockaddr(remote);
        if (::connect(handle_,reinterpret_cast<const sockaddr*>(&address),sizeof(address)))
            throw std::runtime_error("failed to connect proven UDP peer: "+socket_error_text());
        connected_endpoint_=remote; connected_peer_=true; return;
    }
    if (network_framework_) return;
    const auto local=local_endpoint(); const auto service=service_class(); const auto timing=receive_timestamps_;
    auto next=std::make_unique<NetworkFrameworkUdp>(service);
    close(handle_); handle_=-1;
    try {
        next->bind(local,remote); next->set_receive_timestamps(timing);
        handle_=next->duplicate_wait_handle(); network_framework_=std::move(next);
    } catch (...) {
        next.reset();
        UdpSocket rollback(UdpBackend::Bsd,service); rollback.bind(local); rollback.set_receive_timestamps(timing);
        *this=std::move(rollback);
        throw;
    }
#else
    (void)remote;
    throw std::runtime_error("Network.framework UDP requires macOS");
#endif
}

UdpServiceClass UdpSocket::service_class() const
{
#if defined(__APPLE__)
    if (network_framework_) return network_framework_->service_class();
    int value=0; socklen_t size=sizeof(value);
    if (getsockopt(handle_,SOL_SOCKET,SO_NET_SERVICE_TYPE,&value,&size))
        throw std::runtime_error("failed to read UDP service class: "+socket_error_text());
    if (value!=NET_SERVICE_TYPE_BE && value!=NET_SERVICE_TYPE_VO) throw std::runtime_error("unexpected UDP service class");
    return value==NET_SERVICE_TYPE_VO ? UdpServiceClass::InteractiveVoice : UdpServiceClass::BestEffort;
#else
    return UdpServiceClass::BestEffort;
#endif
}

Endpoint UdpSocket::local_endpoint_for(const ResolvedUdpEndpoint& destination) const
{
    Endpoint local = local_endpoint();
    if (local.host != "0.0.0.0") return local;
    // UDP connect selects a local route without emitting a datagram. Use an
    // independent RAII probe so the real socket remains connectionless and can
    // receive both server notifications and direct peer traffic.
    UdpSocket route;
    const auto address = to_sockaddr(destination);
    if (::connect(route.handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("failed to discover local UDP route: " + socket_error_text());
    local.host = route.local_endpoint().host;
    return local;
}

void UdpSocket::waitForAny(std::span<const UdpSocket* const> sockets,
                           std::uint64_t timeout_us, RealtimeWakeSignal* command_wake)
{
#if defined(__APPLE__)
    macos_thread_socket_waiter().waitUdp(sockets, (std::min<std::uint64_t>)(timeout_us, 60000000ULL),
        command_wake, nullptr, LegacyUdpWaitStyle::Poll);
    return;
#else
    const int timeout_ms = static_cast<int>((std::min<std::uint64_t>)(timeout_us, 60000000ULL) + 999) / 1000;
#if defined(_WIN32)
    if (sockets.size() + (command_wake ? 1U : 0U) <= WSA_MAXIMUM_WAIT_EVENTS) {
        std::array<WSAEVENT, WSA_MAXIMUM_WAIT_EVENTS> events{};
        std::size_t count = 0;
        for (const auto* socket : sockets) events[count++] = socket->receive_event_;
        if (command_wake) events[count++] = command_wake->event_;
        if (!count) return;
        const DWORD result = WSAWaitForMultipleEvents(static_cast<DWORD>(count),
            events.data(), FALSE, static_cast<DWORD>(timeout_ms), FALSE);
        if (result == WSA_WAIT_FAILED) throw std::runtime_error("UDP link wait failed: " + socket_error_text());
        if (result == WSA_WAIT_TIMEOUT) return;
        for (std::size_t index = 0; index < sockets.size(); ++index) {
            if (WSAWaitForMultipleEvents(1, &events[index], FALSE, 0, FALSE) == WSA_WAIT_EVENT_0) {
                WSANETWORKEVENTS received{};
                if (WSAEnumNetworkEvents(sockets[index]->handle_, events[index], &received) != 0)
                    throw std::runtime_error("UDP link event failed: " + socket_error_text());
            }
        }
        if (command_wake && WSAWaitForMultipleEvents(1, &command_wake->event_, FALSE, 0, FALSE) == WSA_WAIT_EVENT_0)
            command_wake->consume();
        return;
    }
    // WSAPoll avoids imposing Windows' event-count limit on experimental meshes.
    std::vector<WSAPOLLFD> reads;
    reads.reserve(sockets.size());
    for (const auto* socket : sockets) reads.push_back({socket->handle_, POLLRDNORM, 0});
    if (WSAPoll(reads.data(), static_cast<ULONG>(reads.size()), (std::min)(timeout_ms, 2)) == SOCKET_ERROR)
        throw std::runtime_error("UDP link poll failed: " + socket_error_text());
    if (command_wake) command_wake->waitFor(0);
#else
    std::vector<pollfd> reads;
    reads.reserve(sockets.size() + 1);
    for (const auto* socket : sockets) reads.push_back({socket->handle_, POLLIN, 0});
    if (command_wake) reads.push_back({command_wake->read_handle_, POLLIN, 0});
    const int result = poll(reads.data(), reads.size(), timeout_ms);
    if (result < 0 && errno != EINTR) throw std::runtime_error("UDP link poll failed: " + socket_error_text());
    if (result > 0 && command_wake && (reads.back().revents & POLLIN) != 0) command_wake->consume();
#endif
#endif
}

void UdpSocket::waitForDatagrams(std::span<const UdpSocket* const> sockets,
                                std::uint64_t timeout_us, RealtimeWakeSignal* capture_wake)
{
    if (sockets.empty() || timeout_us == 0) return;
#if defined(__APPLE__)
    macos_thread_socket_waiter().waitUdp(sockets, (std::min<std::uint64_t>)(timeout_us, 60000000ULL), capture_wake);
    return;
#else
#if defined(_WIN32)
    if (capture_wake || sockets.size() > FD_SETSIZE) {
        waitForAny(sockets, timeout_us, capture_wake); return;
    }
#else
    if (std::any_of(sockets.begin(), sockets.end(), [](const auto* socket) { return socket->handle_ >= FD_SETSIZE; }) ||
        (capture_wake && capture_wake->read_handle_ >= FD_SETSIZE)) {
        waitForAny(sockets, timeout_us, capture_wake); return;
    }
#endif
    fd_set reads; FD_ZERO(&reads);
    int maximum = 0;
    for (const auto* socket : sockets) { FD_SET(socket->handle_, &reads); maximum = (std::max)(maximum, static_cast<int>(socket->handle_)); }
#if !defined(_WIN32)
    if (capture_wake) { FD_SET(capture_wake->read_handle_, &reads); maximum = (std::max)(maximum, capture_wake->read_handle_); }
#endif
    const auto bounded = (std::min<std::uint64_t>)(timeout_us, 60000000ULL);
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(bounded / 1000000ULL);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(bounded % 1000000ULL);
    const auto result = select(maximum + 1, &reads, nullptr, nullptr, &timeout);
    if (result < 0) {
#if defined(_WIN32)
        if (WSAGetLastError() == WSAEINTR) return;
#else
        if (errno == EINTR) return;
#endif
        throw std::runtime_error("UDP runtime receive wait failed: " + socket_error_text());
    }
#if !defined(_WIN32)
    if (result > 0 && capture_wake && FD_ISSET(capture_wake->read_handle_, &reads)) capture_wake->consume();
#endif
#endif
}

Endpoint UdpSocket::local_endpoint() const
{
#if defined(__APPLE__)
    if (network_framework_) return network_framework_->local_endpoint();
#endif
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        throw std::runtime_error("failed to read local UDP endpoint: " + socket_error_text());
    }
    return from_sockaddr(addr);
}

void UdpSocket::set_send_buffer_size(int bytes)
{
#if defined(__APPLE__)
    if (network_framework_) throw std::runtime_error("Network.framework does not expose socket buffer sizing");
#endif
    if (bytes <= 0) {
        throw std::runtime_error("UDP send buffer size must be positive");
    }
    if (setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) {
        throw std::runtime_error("failed to set UDP send buffer size: " + socket_error_text());
    }
}

void UdpSocket::set_recv_buffer_size(int bytes)
{
#if defined(__APPLE__)
    if (network_framework_) throw std::runtime_error("Network.framework does not expose socket buffer sizing");
#endif
    if (bytes <= 0) {
        throw std::runtime_error("UDP receive buffer size must be positive");
    }
    if (setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) {
        throw std::runtime_error("failed to set UDP receive buffer size: " + socket_error_text());
    }
}

int UdpSocket::send_buffer_size() const
{
#if defined(__APPLE__)
    if (network_framework_) return 0; // Unavailable, not a claimed OS buffer size.
#endif
    int bytes = 0;
    socklen_t len = sizeof(bytes);
    if (getsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&bytes), &len) != 0) {
        throw std::runtime_error("failed to read UDP send buffer size: " + socket_error_text());
    }
    return bytes;
}

int UdpSocket::recv_buffer_size() const
{
#if defined(__APPLE__)
    if (network_framework_) return 0;
#endif
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
#if defined(__APPLE__)
    if (network_framework_) return network_framework_->send_to(endpoint,bytes);
#endif
    const sockaddr_in addr = to_sockaddr(endpoint);
    if (connected_peer_ && endpoint!=connected_endpoint_) return {UdpSendOutcome::Fatal,EINVAL};
    const int sent = connected_peer_ ? ::send(handle_,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0) : ::sendto(
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

RealtimeWakeSignal::RealtimeWakeSignal(NotificationMode mode) : notification_mode_(mode)
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
    if (notification_mode_ == NotificationMode::PollOnly) return;
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

void RealtimeWakeSignal::waitFor(std::uint64_t timeout_us, RealtimeWakeSignal* additional)
{
    timeout_us = (std::min<std::uint64_t>)(timeout_us, 24ULL * 60ULL * 60ULL * 1000000ULL);
    if (additional == this) additional = nullptr;
#if defined(_WIN32)
    const WSAEVENT events[2]{event_, additional ? additional->event_ : event_};
    const DWORD count = additional ? 2U : 1U;
    const DWORD result = WSAWaitForMultipleEvents(
        count, events, FALSE, static_cast<DWORD>((timeout_us + 999) / 1000), FALSE);
    if (result == WSA_WAIT_FAILED) throw std::runtime_error("transport wake wait failed: " + socket_error_text());
    if (result == WSA_WAIT_TIMEOUT) return;
    for (DWORD i = 0; i < count; ++i) {
        if (WSAWaitForMultipleEvents(1, &events[i], FALSE, 0, FALSE) == WSA_WAIT_EVENT_0) {
            (i == 0 ? this : additional)->consume();
        }
    }
#elif defined(__APPLE__)
    macos_thread_socket_waiter().waitUdp({}, timeout_us, this, additional);
#else
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(read_handle_, &reads);
    int maximum = read_handle_;
    if (additional) { FD_SET(additional->read_handle_, &reads); maximum = (std::max)(maximum, additional->read_handle_); }
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_us / 1000000ULL);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(timeout_us % 1000000ULL);
    const int result = select(maximum + 1, &reads, nullptr, nullptr, &timeout);
    if (result < 0 && errno != EINTR) throw std::runtime_error("transport wake wait failed: " + socket_error_text());
    if (result > 0) {
        if (FD_ISSET(read_handle_, &reads)) consume();
        if (additional && FD_ISSET(additional->read_handle_, &reads)) additional->consume();
    }
#endif
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
    RealtimeWakeSignal* wake_signal,
    ReceiveAttemptTiming* attempt) const
{
    if (attempt) *attempt = {};
    if (timeout_us > 0) {
        constexpr std::uint64_t kMaxSelectTimeoutUs =
            24ULL * 60ULL * 60ULL * 1000000ULL;
        timeout_us = (std::min)(timeout_us, kMaxSelectTimeoutUs);
#if defined(__APPLE__)
        const std::array<const UdpSocket*, 1> sockets{this};
        if (!macos_thread_socket_waiter().waitUdp(sockets, timeout_us, wake_signal).datagram)
            return std::nullopt;
#else
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
#endif
    }

#if defined(__APPLE__)
    if (network_framework_) return network_framework_->take(buffer);
#endif
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
#if defined(__APPLE__)
    alignas(cmsghdr) std::array<std::uint8_t, CMSG_SPACE(sizeof(std::uint64_t))> ancillary;
    if (receive_timestamps_) {
        message.msg_control = ancillary.data();
        message.msg_controllen = static_cast<socklen_t>(ancillary.size());
    }
#endif
#if defined(__APPLE__)
    const auto read_begin = receive_timestamps_ ? receive_clock_sample() : ReceiveClockSample{};
    const auto read_begin_us = read_begin.wall_us;
#endif
    const ssize_t received = ::recvmsg(handle_, &message, 0);
    const int read_error = received < 0 ? errno : 0;
#if defined(__APPLE__)
    const auto read_end = receive_timestamps_ ? receive_clock_sample() : ReceiveClockSample{};
    const auto read_end_us = read_end.wall_us;
    if (attempt) *attempt = {read_begin_us, read_end_us,
        received < 0 && (read_error == EAGAIN || read_error == EWOULDBLOCK)};
#endif
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
        if (read_error == EAGAIN || read_error == EWOULDBLOCK || read_error == EINTR ||
            read_error == EMSGSIZE || read_error == ECONNREFUSED ||
            read_error == EHOSTUNREACH || read_error == ENETUNREACH) {
            return std::nullopt;
        }
        errno = read_error;
#endif
        throw std::runtime_error("UDP receive failed: " + socket_error_text());
    }
#if !defined(_WIN32)
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        return std::nullopt;
    }
#endif
    ReceivedDatagram result{resolved_from_sockaddr(from), static_cast<std::size_t>(received)};
#if defined(__APPLE__)
    if (receive_timestamps_ && (message.msg_flags & MSG_CTRUNC) == 0) {
        for (const cmsghdr* control = CMSG_FIRSTHDR(&message); control;
             control = CMSG_NXTHDR(&message, control)) {
            if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_TIMESTAMP_MONOTONIC ||
                control->cmsg_len < CMSG_LEN(sizeof(std::uint64_t))) continue;
            std::uint64_t arrival = 0;
            std::memcpy(&arrival, CMSG_DATA(control), sizeof(arrival));
            const auto observed_us = receive_diagnostic_time_us();
            const auto arrival_us = receive_diagnostic_mach_us(arrival);
            if (arrival && observed_us >= arrival_us) {
                result.kernel_arrival_us = arrival_us;
                result.socket_residence_us = observed_us - arrival_us;
                result.read_begin_us = read_begin_us; result.read_end_us = read_end_us;
                result.observed_us = observed_us;
                result.read_cpu.valid = read_begin.cpu_valid && read_end.cpu_valid && read_end.cpu_us >= read_begin.cpu_us;
                if (result.read_cpu.valid) result.read_cpu.us = read_end.cpu_us - read_begin.cpu_us;
            }
            break;
        }
    }
#endif
    return result;
}

} // namespace jam2
