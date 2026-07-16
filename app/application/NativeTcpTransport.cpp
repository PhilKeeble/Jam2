#include "NativeTcpTransport.hpp"

#include <QMetaObject>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jam2::application {
namespace {

using NativeHandle = std::uintptr_t;
constexpr NativeHandle kInvalidHandle = std::numeric_limits<NativeHandle>::max();
constexpr int kIoTimeoutMs = 250;
constexpr int kConnectTimeoutMs = 10000;

#if defined(_WIN32)
using OsSocket = SOCKET;
constexpr OsSocket kInvalidSocket = INVALID_SOCKET;
#else
using OsSocket = int;
constexpr OsSocket kInvalidSocket = -1;
#endif

class SocketRuntime {
public:
    SocketRuntime()
    {
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#else
        // A reset control peer is an ordinary recoverable network event. The
        // default POSIX SIGPIPE action would terminate the entire GUI before
        // the writer can report EPIPE, including from any future socket path
        // that forgets to opt out per descriptor.
        static std::once_flag signalOnce;
        std::call_once(signalOnce, [] { (void)std::signal(SIGPIPE, SIG_IGN); });
#endif
    }

    ~SocketRuntime()
    {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

NativeHandle toNative(OsSocket socket) noexcept
{
    return static_cast<NativeHandle>(socket);
}

OsSocket fromNative(NativeHandle handle) noexcept
{
    return static_cast<OsSocket>(handle);
}

int lastSocketError() noexcept
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

QString socketErrorText(int error)
{
#if defined(_WIN32)
    return QStringLiteral("socket error %1").arg(error);
#else
    return QString::fromLocal8Bit(std::strerror(error));
#endif
}

void closeSocket(OsSocket socket) noexcept
{
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    (void)shutdown(socket, SD_BOTH);
    (void)closesocket(socket);
#else
    (void)shutdown(socket, SHUT_RDWR);
    (void)::close(socket);
#endif
}

void closeAtomicSocket(std::atomic<NativeHandle>& handle) noexcept
{
    const NativeHandle prior = handle.exchange(kInvalidHandle);
    if (prior != kInvalidHandle) {
        closeSocket(fromNative(prior));
    }
}

bool isRetryableIoError(int error) noexcept
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT || error == WSAEINTR;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

bool isConnectInProgress(int error) noexcept
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
#else
    return error == EINPROGRESS || error == EALREADY || error == EINTR;
#endif
}

NativeTcpError::Code classifyConnectError(int error) noexcept
{
#if defined(_WIN32)
    if (error == WSAECONNREFUSED) {
        return NativeTcpError::Code::ConnectionRefused;
    }
    if (error == WSAENETUNREACH || error == WSAEHOSTUNREACH || error == WSAENETDOWN) {
        return NativeTcpError::Code::NetworkUnavailable;
    }
    if (error == WSAETIMEDOUT) {
        return NativeTcpError::Code::Timeout;
    }
#else
    if (error == ECONNREFUSED) {
        return NativeTcpError::Code::ConnectionRefused;
    }
    if (error == ENETUNREACH || error == EHOSTUNREACH || error == ENETDOWN) {
        return NativeTcpError::Code::NetworkUnavailable;
    }
    if (error == ETIMEDOUT) {
        return NativeTcpError::Code::Timeout;
    }
#endif
    return NativeTcpError::Code::Transport;
}

bool setBlocking(OsSocket socket, bool blocking)
{
#if defined(_WIN32)
    u_long mode = blocking ? 0UL : 1UL;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL,
        blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

void configureConnectedSocket(OsSocket socket)
{
    const int enabled = 1;
    (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#if defined(__APPLE__)
    // A peer reset is a normal control-plane failure. Never let the default
    // SIGPIPE action terminate the GUI while the writer reports that failure.
    (void)setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#if defined(_WIN32)
    const DWORD timeout = kIoTimeoutMs;
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{kIoTimeoutMs / 1000, (kIoTimeoutMs % 1000) * 1000};
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

QString numericPeerHost(const sockaddr_in& address)
{
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address.sin_addr, text.data(),
            static_cast<socklen_t>(text.size())) == nullptr) {
        return {};
    }
    return QString::fromLatin1(text.data());
}

template <typename Function>
bool postTo(QObject* context, Function&& function)
{
    if (!context) {
        return false;
    }
    return QMetaObject::invokeMethod(
        context,
        std::forward<Function>(function),
        Qt::QueuedConnection);
}

struct PendingWrite {
    QByteArray bytes;
    bool closeAfterWrite = false;
    bool opensReadGateAfterWrite = false;
};

void finishThread(std::thread& thread)
{
    if (!thread.joinable()) {
        return;
    }
    if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
    } else {
        thread.join();
    }
}

} // namespace

class NativeTcpConnection::Impl {
public:
    Impl(
        std::shared_ptr<SocketRuntime> ownedRuntime,
        OsSocket ownedSocket,
        QString remoteHost)
        : runtime(std::move(ownedRuntime)),
          handle(toNative(ownedSocket)),
          host(std::move(remoteHost))
    {
        configureConnectedSocket(ownedSocket);
    }

    ~Impl()
    {
        stop();
        join();
    }

    void start(QObject* target, DataHandler dataHandler, DisconnectHandler disconnectHandler)
    {
        bool expected = false;
        if (!started.compare_exchange_strong(expected, true)) {
            return;
        }
        context = target;
        onData = std::move(dataHandler);
        onDisconnected = std::move(disconnectHandler);
        {
            std::lock_guard lock(queueMutex);
            if (writes.empty()) {
                readGateOpen = true;
            } else {
                // The control protocol is server-speaks-first. Writes queued
                // before start() (currently the authentication challenge)
                // must reach the peer before this connection can enter recv().
                // Mark the end of that fixed bootstrap prefix so later writes
                // do not delay the receive side.
                writes.back().opensReadGateAfterWrite = true;
            }
        }
        try {
            writer = std::thread([this] {
                try {
                    writeLoop();
                } catch (const std::exception& error) {
                    transportEnded(QStringLiteral("TCP writer failed: ") +
                        QString::fromUtf8(error.what()));
                } catch (...) {
                    transportEnded(QStringLiteral("TCP writer failed with an unknown exception"));
                }
            });
            reader = std::thread([this] {
                try {
                    readLoop();
                } catch (const std::exception& error) {
                    transportEnded(QStringLiteral("TCP reader failed: ") +
                        QString::fromUtf8(error.what()));
                } catch (...) {
                    transportEnded(QStringLiteral("TCP reader failed with an unknown exception"));
                }
            });
        } catch (const std::exception& error) {
            transportEnded(QStringLiteral("TCP worker creation failed: ") +
                QString::fromUtf8(error.what()));
            join();
        } catch (...) {
            transportEnded(QStringLiteral("TCP worker creation failed with an unknown exception"));
            join();
        }
    }

    bool queueWrite(const QByteArray& bytes, bool closeAfterWrite)
    {
        if (bytes.isEmpty() || !connected.load()) {
            return false;
        }
        std::lock_guard lock(queueMutex);
        if (!connected.load() || stopping.load()) {
            return false;
        }
        queuedBytes.fetch_add(bytes.size());
        writes.push_back(PendingWrite{bytes, closeAfterWrite, false});
        queueReady.notify_one();
        return true;
    }

    void stop()
    {
        stopping.store(true);
        connected.store(false);
        closeAtomicSocket(handle);
        {
            std::lock_guard lock(queueMutex);
            writes.clear();
            queuedBytes.store(0);
        }
        queueReady.notify_all();
    }

    void join()
    {
        finishThread(reader);
        finishThread(writer);
    }

    void transportEnded(const QString& detail)
    {
        if (disconnectPosted.exchange(true)) {
            return;
        }
        stopping.store(true);
        connected.store(false);
        closeAtomicSocket(handle);
        queueReady.notify_all();
        const DisconnectHandler handler = onDisconnected;
        QObject* target = context;
        (void)postTo(target, [handler, detail] {
            if (handler) {
                handler(detail);
            }
        });
    }

    void readLoop()
    {
        {
            std::unique_lock lock(queueMutex);
            queueReady.wait(lock, [this] {
                return stopping.load() || readGateOpen;
            });
            if (stopping.load()) {
                return;
            }
        }
        std::array<char, 64 * 1024> buffer{};
        while (!stopping.load()) {
            const NativeHandle current = handle.load();
            if (current == kInvalidHandle) {
                break;
            }
            const int received = static_cast<int>(::recv(
                fromNative(current), buffer.data(), static_cast<int>(buffer.size()), 0));
            if (received > 0) {
                const QByteArray bytes(buffer.data(), received);
                const DataHandler handler = onData;
                QObject* target = context;
                if (!postTo(target, [handler, bytes] {
                        if (handler) {
                            handler(bytes);
                        }
                    })) {
                    break;
                }
                continue;
            }
            if (received == 0) {
                transportEnded({});
                return;
            }
            const int error = lastSocketError();
            if (isRetryableIoError(error)) {
                continue;
            }
            if (!stopping.load()) {
                transportEnded(QStringLiteral("TCP receive failed: ") + socketErrorText(error));
            }
            return;
        }
    }

    void writeLoop()
    {
        while (!stopping.load()) {
            PendingWrite pending;
            {
                std::unique_lock lock(queueMutex);
                queueReady.wait(lock, [this] {
                    return stopping.load() || !writes.empty();
                });
                if (stopping.load()) {
                    return;
                }
                pending = std::move(writes.front());
                writes.pop_front();
            }

            qsizetype offset = 0;
            while (offset < pending.bytes.size() && !stopping.load()) {
                const NativeHandle current = handle.load();
                if (current == kInvalidHandle) {
                    return;
                }
                const int sent = static_cast<int>(::send(
                    fromNative(current),
                    pending.bytes.constData() + offset,
                    static_cast<int>(pending.bytes.size() - offset),
                    0));
                if (sent > 0) {
                    offset += sent;
                    queuedBytes.fetch_sub(sent);
                    continue;
                }
                const int error = lastSocketError();
                if (isRetryableIoError(error)) {
                    continue;
                }
                if (!stopping.load()) {
                    transportEnded(QStringLiteral("TCP send failed: ") + socketErrorText(error));
                }
                return;
            }
            if (pending.closeAfterWrite && offset == pending.bytes.size()) {
                transportEnded({});
                return;
            }
            if (pending.opensReadGateAfterWrite && offset == pending.bytes.size()) {
                {
                    std::lock_guard lock(queueMutex);
                    readGateOpen = true;
                }
                queueReady.notify_all();
            }
        }
    }

    std::shared_ptr<SocketRuntime> runtime;
    std::atomic<NativeHandle> handle{kInvalidHandle};
    QString host;
    QObject* context = nullptr;
    DataHandler onData;
    DisconnectHandler onDisconnected;
    std::atomic<bool> started{false};
    std::atomic<bool> connected{true};
    std::atomic<bool> stopping{false};
    std::atomic<bool> disconnectPosted{false};
    std::atomic<qint64> queuedBytes{0};
    std::mutex queueMutex;
    std::condition_variable_any queueReady;
    std::deque<PendingWrite> writes;
    bool readGateOpen = false;
    std::thread reader;
    std::thread writer;
};

NativeTcpConnection::NativeTcpConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

NativeTcpConnection::~NativeTcpConnection() = default;

void NativeTcpConnection::start(
    QObject* context,
    DataHandler onData,
    DisconnectHandler onDisconnected)
{
    impl_->start(context, std::move(onData), std::move(onDisconnected));
}

bool NativeTcpConnection::write(const QByteArray& bytes, bool closeAfterWrite)
{
    return impl_->queueWrite(bytes, closeAfterWrite);
}

void NativeTcpConnection::close()
{
    impl_->stop();
    impl_->join();
}

bool NativeTcpConnection::isConnected() const
{
    return impl_->connected.load();
}

qint64 NativeTcpConnection::bytesToWrite() const
{
    return impl_->queuedBytes.load();
}

QString NativeTcpConnection::peerHost() const
{
    return impl_->host;
}

class NativeTcpListener::Impl {
public:
    Impl() : runtime(std::make_shared<SocketRuntime>()) {}

    ~Impl()
    {
        close();
    }

    bool listen(
        quint16 requestedPort,
        QObject* target,
        AcceptHandler acceptedHandler,
        ErrorHandler errorHandler,
        int maximumPendingDeliveries)
    {
        close();
        context = target;
        onAccepted = std::move(acceptedHandler);
        onError = std::move(errorHandler);
        maxPending = std::max(1, maximumPendingDeliveries);
        pendingDeliveries = std::make_shared<std::atomic<int>>(0);

        const OsSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == kInvalidSocket) {
            setError(QStringLiteral("failed to create TCP listener: ") +
                socketErrorText(lastSocketError()));
            return false;
        }
#if !defined(_WIN32)
        const int reuse = 1;
        (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(requestedPort);
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(socket, maxPending) != 0) {
            const int error = lastSocketError();
            closeSocket(socket);
            setError(QStringLiteral("TCP control listen failed: ") + socketErrorText(error));
            return false;
        }

        sockaddr_in local{};
#if defined(_WIN32)
        int localSize = sizeof(local);
#else
        socklen_t localSize = sizeof(local);
#endif
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize) != 0) {
            const int error = lastSocketError();
            closeSocket(socket);
            setError(QStringLiteral("failed to inspect TCP listener: ") + socketErrorText(error));
            return false;
        }
        port.store(ntohs(local.sin_port));
        handle.store(toNative(socket));
        listening.store(true);
        try {
            thread = std::thread([this] {
                try {
                    acceptLoop();
                } catch (const std::exception& error) {
                    listening.store(false);
                    postError(QStringLiteral("TCP accept worker failed: ") +
                        QString::fromUtf8(error.what()));
                } catch (...) {
                    listening.store(false);
                    postError(QStringLiteral("TCP accept worker failed with an unknown exception"));
                }
            });
        } catch (const std::exception& error) {
            listening.store(false);
            closeAtomicSocket(handle);
            setError(QStringLiteral("TCP accept worker creation failed: ") +
                QString::fromUtf8(error.what()));
            return false;
        } catch (...) {
            listening.store(false);
            closeAtomicSocket(handle);
            setError(QStringLiteral("TCP accept worker creation failed with an unknown exception"));
            return false;
        }
        return true;
    }

    void close()
    {
        listening.store(false);
        closeAtomicSocket(handle);
        finishThread(thread);
        port.store(0);
    }

    void acceptLoop()
    {
        while (listening.load()) {
            const NativeHandle current = handle.load();
            if (current == kInvalidHandle) {
                return;
            }
            fd_set readSet;
            FD_ZERO(&readSet);
            const OsSocket listener = fromNative(current);
            FD_SET(listener, &readSet);
            timeval timeout{0, 100000};
#if defined(_WIN32)
            const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int selected = select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (selected == 0) {
                continue;
            }
            if (selected < 0) {
                const int error = lastSocketError();
                if (!listening.load() || isRetryableIoError(error)) {
                    continue;
                }
                postError(QStringLiteral("TCP accept wait failed: ") + socketErrorText(error));
                return;
            }

            sockaddr_in peer{};
#if defined(_WIN32)
            int peerSize = sizeof(peer);
#else
            socklen_t peerSize = sizeof(peer);
#endif
            const OsSocket accepted = ::accept(
                listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
            if (accepted == kInvalidSocket) {
                const int error = lastSocketError();
                if (!listening.load() || isRetryableIoError(error)) {
                    continue;
                }
                postError(QStringLiteral("TCP accept failed: ") + socketErrorText(error));
                continue;
            }

            const int priorPending = pendingDeliveries->fetch_add(1);
            if (priorPending >= maxPending) {
                pendingDeliveries->fetch_sub(1);
                closeSocket(accepted);
                continue;
            }
            auto connection = NativeTcpConnection::Pointer(new NativeTcpConnection(
                std::make_unique<NativeTcpConnection::Impl>(
                    runtime, accepted, numericPeerHost(peer))));
            const AcceptHandler handler = onAccepted;
            const auto pending = pendingDeliveries;
            if (!postTo(context, [handler, connection, pending] {
                    pending->fetch_sub(1);
                    if (handler) {
                        handler(connection);
                    }
                })) {
                pending->fetch_sub(1);
                connection->close();
            }
        }
    }

    void postError(const QString& detail)
    {
        setError(detail);
        const ErrorHandler handler = onError;
        (void)postTo(context, [handler, detail] {
            if (handler) {
                handler(detail);
            }
        });
    }

    void setError(const QString& detail)
    {
        std::lock_guard lock(errorMutex);
        lastError = detail;
    }

    QString error() const
    {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    std::shared_ptr<SocketRuntime> runtime;
    std::atomic<NativeHandle> handle{kInvalidHandle};
    std::atomic<bool> listening{false};
    std::atomic<quint16> port{0};
    QObject* context = nullptr;
    AcceptHandler onAccepted;
    ErrorHandler onError;
    int maxPending = 1;
    std::shared_ptr<std::atomic<int>> pendingDeliveries;
    mutable std::mutex errorMutex;
    QString lastError;
    std::thread thread;
};

NativeTcpListener::NativeTcpListener() : impl_(std::make_unique<Impl>()) {}
NativeTcpListener::~NativeTcpListener() = default;

bool NativeTcpListener::listen(
    quint16 port,
    QObject* context,
    AcceptHandler onAccepted,
    ErrorHandler onError,
    int maxPendingDeliveries)
{
    return impl_->listen(
        port, context, std::move(onAccepted), std::move(onError), maxPendingDeliveries);
}

void NativeTcpListener::close() { impl_->close(); }
bool NativeTcpListener::isListening() const { return impl_->listening.load(); }
quint16 NativeTcpListener::localPort() const { return impl_->port.load(); }
QString NativeTcpListener::errorString() const { return impl_->error(); }

class NativeTcpConnector::Impl {
public:
    Impl() : runtime(std::make_shared<SocketRuntime>()) {}
    ~Impl() { cancel(); }

    void connectToHost(
        const QString& requestedHost,
        quint16 requestedPort,
        QObject* target,
        ConnectedHandler connectedHandler,
        ErrorHandler errorHandler)
    {
        cancel();
        context = target;
        onConnected = std::move(connectedHandler);
        onError = std::move(errorHandler);
        cancelled.store(false);
        try {
            thread = std::thread([this, requestedHost, requestedPort] {
                try {
                    connectLoop(requestedHost, requestedPort);
                } catch (const std::exception& error) {
                    if (!cancelled.load()) {
                        postFailure(NativeTcpError{
                            NativeTcpError::Code::Transport,
                            QStringLiteral("TCP connect worker failed: ") +
                                QString::fromUtf8(error.what())});
                    }
                } catch (...) {
                    if (!cancelled.load()) {
                        postFailure(NativeTcpError{
                            NativeTcpError::Code::Transport,
                            QStringLiteral("TCP connect worker failed with an unknown exception")});
                    }
                }
            });
        } catch (const std::exception& error) {
            postFailure(NativeTcpError{
                NativeTcpError::Code::Transport,
                QStringLiteral("TCP connect worker creation failed: ") +
                    QString::fromUtf8(error.what())});
        } catch (...) {
            postFailure(NativeTcpError{
                NativeTcpError::Code::Transport,
                QStringLiteral("TCP connect worker creation failed with an unknown exception")});
        }
    }

    void cancel()
    {
        cancelled.store(true);
        closeAtomicSocket(connectingHandle);
        finishThread(thread);
    }

    void connectLoop(const QString& host, quint16 port)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const QByteArray hostBytes = host.toUtf8();
        const QByteArray portBytes = QByteArray::number(port);
        const int resolveResult = getaddrinfo(
            hostBytes.constData(), portBytes.constData(), &hints, &addresses);
        if (resolveResult != 0 || !addresses) {
            postFailure(NativeTcpError{
                NativeTcpError::Code::HostNotFound,
                QStringLiteral("TCP host resolution failed for %1").arg(host)});
            return;
        }

        NativeTcpError lastError{
            NativeTcpError::Code::Transport,
            QStringLiteral("TCP connection failed")};
        for (addrinfo* address = addresses; address && !cancelled.load();
             address = address->ai_next) {
            const OsSocket socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket == kInvalidSocket) {
                const int error = lastSocketError();
                lastError = {classifyConnectError(error), socketErrorText(error)};
                continue;
            }
            connectingHandle.store(toNative(socket));
            if (!setBlocking(socket, false)) {
                const int error = lastSocketError();
                closeAtomicSocket(connectingHandle);
                lastError = {classifyConnectError(error), socketErrorText(error)};
                continue;
            }

            int connectError = 0;
            if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) != 0) {
                connectError = lastSocketError();
                if (!isConnectInProgress(connectError)) {
                    closeAtomicSocket(connectingHandle);
                    lastError = {classifyConnectError(connectError), socketErrorText(connectError)};
                    continue;
                }

                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kConnectTimeoutMs);
                bool completed = false;
                while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
                    fd_set writeSet;
                    fd_set errorSet;
                    FD_ZERO(&writeSet);
                    FD_ZERO(&errorSet);
                    FD_SET(socket, &writeSet);
                    FD_SET(socket, &errorSet);
                    timeval timeout{0, 50000};
#if defined(_WIN32)
                    const int selected = select(0, nullptr, &writeSet, &errorSet, &timeout);
#else
                    const int selected = select(socket + 1, nullptr, &writeSet, &errorSet, &timeout);
#endif
                    if (selected < 0) {
                        connectError = lastSocketError();
                        break;
                    }
                    if (selected == 0) {
                        continue;
                    }
                    int pendingError = 0;
#if defined(_WIN32)
                    int pendingErrorSize = sizeof(pendingError);
#else
                    socklen_t pendingErrorSize = sizeof(pendingError);
#endif
                    if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                            reinterpret_cast<char*>(&pendingError), &pendingErrorSize) == 0 &&
                        pendingError == 0) {
                        completed = true;
                    } else {
                        connectError = pendingError != 0 ? pendingError : lastSocketError();
                    }
                    break;
                }
                if (!completed) {
                    if (!cancelled.load() && connectError == 0) {
#if defined(_WIN32)
                        connectError = WSAETIMEDOUT;
#else
                        connectError = ETIMEDOUT;
#endif
                    }
                    closeAtomicSocket(connectingHandle);
                    lastError = {classifyConnectError(connectError), socketErrorText(connectError)};
                    continue;
                }
            }

            if (cancelled.load()) {
                closeAtomicSocket(connectingHandle);
                break;
            }
            (void)setBlocking(socket, true);
            const NativeHandle adopted = connectingHandle.exchange(kInvalidHandle);
            auto connection = NativeTcpConnection::Pointer(new NativeTcpConnection(
                std::make_unique<NativeTcpConnection::Impl>(
                    runtime, fromNative(adopted), host)));
            freeaddrinfo(addresses);
            const ConnectedHandler handler = onConnected;
            if (!postTo(context, [handler, connection] {
                    if (handler) {
                        handler(connection);
                    }
                })) {
                connection->close();
            }
            return;
        }
        freeaddrinfo(addresses);
        if (!cancelled.load()) {
            postFailure(NativeTcpError{
                lastError.code,
                QStringLiteral("TCP control transport error: ") + lastError.message});
        }
    }

    void postFailure(const NativeTcpError& error)
    {
        const ErrorHandler handler = onError;
        (void)postTo(context, [handler, error] {
            if (handler) {
                handler(error);
            }
        });
    }

    std::shared_ptr<SocketRuntime> runtime;
    std::atomic<NativeHandle> connectingHandle{kInvalidHandle};
    std::atomic<bool> cancelled{true};
    QObject* context = nullptr;
    ConnectedHandler onConnected;
    ErrorHandler onError;
    std::thread thread;
};

NativeTcpConnector::NativeTcpConnector() : impl_(std::make_unique<Impl>()) {}
NativeTcpConnector::~NativeTcpConnector() = default;

void NativeTcpConnector::connectToHost(
    const QString& host,
    quint16 port,
    QObject* context,
    ConnectedHandler onConnected,
    ErrorHandler onError)
{
    impl_->connectToHost(
        host, port, context, std::move(onConnected), std::move(onError));
}

void NativeTcpConnector::cancel() { impl_->cancel(); }

class NativeTcpPortReservation::Impl {
public:
    Impl() : runtime(std::make_shared<SocketRuntime>()) {}
    ~Impl() { close(); }

    bool bind(const QString& host, quint16 requestedPort)
    {
        close();
        const OsSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == kInvalidSocket) {
            lastError = QStringLiteral("failed to create TCP reservation: ") +
                socketErrorText(lastSocketError());
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(requestedPort);
        const QByteArray hostBytes = host.toLatin1();
        if (host == QStringLiteral("0.0.0.0")) {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, hostBytes.constData(), &address.sin_addr) != 1) {
            closeSocket(socket);
            lastError = QStringLiteral("TCP reservation host must be numeric IPv4");
            return false;
        }
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const int error = lastSocketError();
            closeSocket(socket);
            lastError = QStringLiteral("TCP reservation bind failed: ") + socketErrorText(error);
            return false;
        }
        sockaddr_in local{};
#if defined(_WIN32)
        int localSize = sizeof(local);
#else
        socklen_t localSize = sizeof(local);
#endif
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize) != 0) {
            const int error = lastSocketError();
            closeSocket(socket);
            lastError = QStringLiteral("TCP reservation inspection failed: ") + socketErrorText(error);
            return false;
        }
        handle = socket;
        port = ntohs(local.sin_port);
        lastError.clear();
        return true;
    }

    void close()
    {
        closeSocket(handle);
        handle = kInvalidSocket;
        port = 0;
    }

    std::shared_ptr<SocketRuntime> runtime;
    OsSocket handle = kInvalidSocket;
    quint16 port = 0;
    QString lastError;
};

NativeTcpPortReservation::NativeTcpPortReservation() : impl_(std::make_unique<Impl>()) {}
NativeTcpPortReservation::~NativeTcpPortReservation() = default;
bool NativeTcpPortReservation::bind(const QString& host, quint16 port) { return impl_->bind(host, port); }
void NativeTcpPortReservation::close() { impl_->close(); }
quint16 NativeTcpPortReservation::localPort() const { return impl_->port; }
QString NativeTcpPortReservation::errorString() const { return impl_->lastError; }

} // namespace jam2::application
