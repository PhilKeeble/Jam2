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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace jam2 {
namespace {

constexpr std::size_t kJam2HeaderBytes = 48;
constexpr std::size_t kMaxAudioFramesPerPacket = 256;
constexpr std::size_t kPcm24BytesPerFrame = 3;
constexpr std::size_t kMaxExpectedDatagramBytes = kJam2HeaderBytes + (kMaxAudioFramesPerPacket * kPcm24BytesPerFrame);

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

} // namespace

Endpoint resolve_udp_endpoint(const Endpoint& endpoint)
{
    sockaddr_in address = to_sockaddr(endpoint);
    address.sin_port = htons(endpoint.port);
    return from_sockaddr(address);
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
}

UdpSocket::~UdpSocket()
{
#if defined(_WIN32)
    if (handle_ != INVALID_SOCKET) {
        closesocket(handle_);
    }
#else
    if (handle_ >= 0) {
        close(handle_);
    }
#endif
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_)
{
#if defined(_WIN32)
    other.handle_ = INVALID_SOCKET;
#else
    other.handle_ = -1;
#endif
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
#if defined(_WIN32)
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
        other.handle_ = INVALID_SOCKET;
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

void UdpSocket::send_to(const Endpoint& endpoint, std::span<const std::uint8_t> bytes) const
{
    const sockaddr_in addr = to_sockaddr(endpoint);
    const int sent = ::sendto(
        handle_,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&addr),
        sizeof(addr));
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        throw std::runtime_error("failed to send UDP packet: " + socket_error_text());
    }
}

std::optional<std::pair<Endpoint, std::vector<std::uint8_t>>> UdpSocket::recv_from(int timeout_ms) const
{
    std::vector<std::uint8_t> buffer(kMaxExpectedDatagramBytes);
    const auto received = recv_from(buffer, timeout_ms);
    if (!received) {
        return std::nullopt;
    }
    buffer.resize(received->size);
    return std::make_pair(received->endpoint, std::move(buffer));
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
    std::uint64_t timeout_us) const
{
    constexpr std::uint64_t kMaxSelectTimeoutUs = 24ULL * 60ULL * 60ULL * 1000000ULL;
    timeout_us = (std::min)(timeout_us, kMaxSelectTimeoutUs);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(handle_, &read_set);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_us / 1000000ULL);
    timeout.tv_usec = static_cast<long>(timeout_us % 1000000ULL);
    const int ready = select(static_cast<int>(handle_ + 1), &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
        throw std::runtime_error("UDP select failed: " + socket_error_text());
    }
    if (ready == 0) {
        return std::nullopt;
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
        if (WSAGetLastError() == WSAECONNRESET) {
            return std::nullopt;
        }
        if (WSAGetLastError() == WSAEMSGSIZE) {
            return std::nullopt;
        }
#else
        if (errno == EMSGSIZE) {
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
    return ReceivedDatagram{from_sockaddr(from), static_cast<std::size_t>(received)};
}

} // namespace jam2
