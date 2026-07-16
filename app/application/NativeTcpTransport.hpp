#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace jam2::application {

struct NativeTcpError {
    enum class Code {
        None,
        HostNotFound,
        ConnectionRefused,
        NetworkUnavailable,
        Timeout,
        Transport,
    };

    Code code = Code::None;
    QString message;
};

class NativeTcpConnection final {
public:
    using Pointer = std::shared_ptr<NativeTcpConnection>;
    using DataHandler = std::function<void(const QByteArray&)>;
    using DisconnectHandler = std::function<void(const QString&)>;

    ~NativeTcpConnection();
    NativeTcpConnection(const NativeTcpConnection&) = delete;
    NativeTcpConnection& operator=(const NativeTcpConnection&) = delete;

    void start(QObject* context, DataHandler onData, DisconnectHandler onDisconnected);
    bool write(const QByteArray& bytes, bool closeAfterWrite = false);
    void close();
    bool isConnected() const;
    qint64 bytesToWrite() const;
    QString peerHost() const;

private:
    class Impl;
    explicit NativeTcpConnection(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class NativeTcpListener;
    friend class NativeTcpConnector;
};

class NativeTcpListener final {
public:
    using AcceptHandler = std::function<void(const NativeTcpConnection::Pointer&)>;
    using ErrorHandler = std::function<void(const QString&)>;

    NativeTcpListener();
    ~NativeTcpListener();
    NativeTcpListener(const NativeTcpListener&) = delete;
    NativeTcpListener& operator=(const NativeTcpListener&) = delete;

    bool listen(
        quint16 port,
        QObject* context,
        AcceptHandler onAccepted,
        ErrorHandler onError,
        int maxPendingDeliveries);
    void close();
    bool isListening() const;
    quint16 localPort() const;
    QString errorString() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class NativeTcpConnector final {
public:
    using ConnectedHandler = std::function<void(const NativeTcpConnection::Pointer&)>;
    using ErrorHandler = std::function<void(const NativeTcpError&)>;

    NativeTcpConnector();
    ~NativeTcpConnector();
    NativeTcpConnector(const NativeTcpConnector&) = delete;
    NativeTcpConnector& operator=(const NativeTcpConnector&) = delete;

    void connectToHost(
        const QString& host,
        quint16 port,
        QObject* context,
        ConnectedHandler onConnected,
        ErrorHandler onError);
    void cancel();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class NativeTcpPortReservation final {
public:
    NativeTcpPortReservation();
    ~NativeTcpPortReservation();
    NativeTcpPortReservation(const NativeTcpPortReservation&) = delete;
    NativeTcpPortReservation& operator=(const NativeTcpPortReservation&) = delete;

    bool bind(const QString& numericHost, quint16 port);
    void close();
    quint16 localPort() const;
    QString errorString() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2::application
