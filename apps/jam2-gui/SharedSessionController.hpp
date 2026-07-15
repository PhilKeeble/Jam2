#pragma once

#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "runtime_entry.hpp"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>
#include <string>
#include <vector>

class ApplicationRuntime;

// Non-UI owner of authenticated TCP bootstrap, membership, admission, and
// authority state. GUI and headless network commands bind presentation and
// engine lifecycle to this same controller.
class SharedSessionController final : public QObject {
public:
    enum class Role {
        Inactive,
        Local,
        Creator,
        Joiner,
    };

    enum class Lifecycle {
        Inactive,
        Listening,
        Connecting,
        Authenticated,
        Active,
        Reconnecting,
        Failed,
    };

    enum class EdgeState {
        Candidate,
        Authenticated,
        Probing,
        Active,
        Failed,
    };

    struct PeerSnapshot {
        QString token;
        quint64 peerId = 0;
        QString endpoint;
        EdgeState edgeState = EdgeState::Candidate;
        QString proofState;
        QString streamState;
        bool sameMachine = false;
    };

    struct SessionContract {
        int protocolVersion = 1;
        QString audioFormat = QStringLiteral("pcm24-mono");
        QString profile;
        int sampleRate = 0;
        int frameSize = 0;
    };

    struct Snapshot {
        Role role = Role::Inactive;
        Lifecycle lifecycle = Lifecycle::Inactive;
        QString localToken;
        QString coordinatorToken;
        QString gridAuthorityToken;
        QString arrangementAuthorityToken;
        quint64 membershipRevision = 0;
        quint64 contractRevision = 0;
        quint64 gridRevision = 0;
        quint64 arrangementRevision = 0;
        int sessionPeerLimit = 0;
        int reconnectAttempts = 0;
        int reconnectAttemptLimit = 0;
        int reconnectIntervalMs = 0;
        int totalPeerCount = 0;
        int remotePeerCount = 0;
        bool contractReady = false;
        bool membershipReady = false;
        bool networkAttachmentReady = false;
        jam2::control_protocol::TransportFailure failure =
            jam2::control_protocol::TransportFailure::None;
        QString failureDetail;
        bool failureRetryable = false;
        SessionContract contract;
        QVector<PeerSnapshot> peers;
    };

    struct CreatorConfig {
        quint16 port = 0;
        QString sessionHex;
        QString keyHex;
        QString localToken;
        QString localEndpoint;
        int sessionPeerLimit = 0; // Remote peers; zero means unlimited.
        SessionContract contract;
    };

    struct JoinerConfig {
        QString host;
        quint16 port = 0;
        QString sessionHex;
        QString keyHex;
        QString localToken;
        QString localEndpoint;
        SessionContract expectedContract;
        bool enforceExpectedContract = false;
        int reconnectIntervalMs = 2000;
        int reconnectAttemptLimit = 15;
    };

    explicit SharedSessionController(QObject* parent = nullptr);

    void bindRuntime(
        ApplicationRuntime& runtime,
        std::function<Jam2RuntimeOptions(const Snapshot&)> networkOptionsFactory);
    bool startLocal(const Jam2RuntimeOptions& options);
    bool startCreator(const CreatorConfig& config);
    bool startJoiner(const JoinerConfig& config);
    void close();
    void refresh();
    void setReconnectEnabled(bool enabled);

    void updateLocalEndpoint(const QString& endpoint);
    void updatePeerEdgeState(
        quint64 peerId,
        EdgeState state,
        const QString& proofState = QString(),
        const QString& streamState = QString());

    bool send(const QJsonObject& message);
    bool sendTo(const QString& token, const QJsonObject& message, bool closeAfterWrite = false);
    bool canQueueTo(const QString& token, qint64 additionalBytes) const;
    bool hasPeer() const;
    bool isConnected() const;
    bool isServer() const noexcept { return role_ == Role::Creator; }
    QString errorString() const;
    Snapshot snapshot() const;
    ControlServer::Stats serverStats() const { return server_.stats(); }
    ControlClient::Stats clientStats() const { return client_.stats(); }

    std::function<void(const jam2::control_protocol::TransportEvent&, bool)> onTransportEvent;
    std::function<void(const Snapshot&)> onSnapshot;
    std::function<void(const QString&, const QJsonObject&)> onMessage;
    std::function<void(const QString&, const QJsonObject&)> onPeerAuthenticated;
    std::function<void(const QString&)> onPeerDisconnected;
    std::function<void(const SessionContract&)> onContract;
    std::function<QString(const QString&, const QString&)> endpointOverride;

private:
    struct Peer {
        QString endpoint;
        quint64 peerId = 0;
        EdgeState edgeState = EdgeState::Candidate;
        QString proofState;
        QString streamState;
        bool sameMachine = false;
    };

    struct MembershipAssembly {
        quint64 revision = 0;
        int pageCount = 0;
        QString coordinatorToken;
        QString gridAuthorityToken;
        QString arrangementAuthorityToken;
        quint64 gridRevision = 0;
        quint64 arrangementRevision = 0;
        QMap<int, QJsonObject> pages;
    };

    static quint64 peerIdForToken(const QString& token);
    void handleServerEvent(const jam2::control_protocol::TransportEvent& event);
    void handleClientEvent(const jam2::control_protocol::TransportEvent& event);
    void handleAuthenticatedPeer(const QString& token, const QJsonObject& message);
    void handleDisconnectedPeer(const QString& token);
    void handleClientMessage(const QJsonObject& message);
    bool acceptContract(const QJsonObject& message);
    bool acceptAuthorityUpdate(const QJsonObject& message);
    QJsonObject contractMessage() const;
    bool acceptMembershipPage(const QJsonObject& message);
    QJsonObject membershipPageFor(
        const QString& observerToken,
        int pageIndex,
        int pageCount,
        const QList<QString>& tokens) const;
    void broadcastMembership();
    void scheduleReconnect();
    void connectJoiner();
    void publishSnapshot();
    void setLifecycle(Lifecycle lifecycle);
    void reconcileRuntime(const Snapshot& snapshot);
    void reset(bool stopRuntime);
    void fail(
        jam2::control_protocol::TransportFailure failure,
        const QString& detail,
        bool retryable);
    void publishTransportEvent(
        jam2::control_protocol::TransportEvent event,
        bool serverSide);

    Role role_ = Role::Inactive;
    Lifecycle lifecycle_ = Lifecycle::Inactive;
    CreatorConfig creator_;
    JoinerConfig joiner_;
    QMap<QString, Peer> peers_;
    MembershipAssembly membershipAssembly_;
    QString coordinatorToken_;
    QString gridAuthorityToken_;
    QString arrangementAuthorityToken_;
    quint64 membershipRevision_ = 0;
    quint64 contractRevision_ = 0;
    quint64 gridRevision_ = 0;
    quint64 arrangementRevision_ = 0;
    bool contractReady_ = false;
    SessionContract contract_;
    bool reconnectEnabled_ = false;
    bool closing_ = false;
    int reconnectAttempts_ = 0;
    int reconnectAttemptLimit_ = 15;
    jam2::control_protocol::TransportFailure failure_ =
        jam2::control_protocol::TransportFailure::None;
    QString failureDetail_;
    bool failureRetryable_ = false;
    QJsonObject gridState_;
    QJsonObject arrangementState_;
    ApplicationRuntime* runtime_ = nullptr;
    std::function<Jam2RuntimeOptions(const Snapshot&)> networkOptionsFactory_;
    std::vector<std::string> runtimePeers_;
    bool runtimeAttachmentEnabled_ = false;
    bool reconcilingRuntime_ = false;
    QTimer reconnectTimer_;
    ControlServer server_;
    ControlClient client_;
};
