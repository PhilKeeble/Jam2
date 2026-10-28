#pragma once

#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "RuntimeContracts.hpp"
#include "UdpMultiplexTransport.hpp"

#include <QJsonObject>
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>
#include <set>
#include <utility>
#include <string>
#include <vector>
#include "PeerName.hpp"

class ApplicationRuntime;

// Non-UI owner of authenticated UDP multiplex bootstrap, membership, admission, and
// authority state. GUI and headless network commands bind presentation and
// engine lifecycle to this same controller.
class SharedSessionController final : public QObject {
public:
    static constexpr int kDefaultHeartbeatIntervalMs = 30000;
    static constexpr int kDefaultHeartbeatMissLimit = 5;

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
        jam2::application::PeerName name;
    };

    struct SessionContract {
        int protocolVersion = jam2::protocol::kProtocolVersion;
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
        quint64 localPeerId = 0;
        quint64 coordinatorPeerId = 0;
        QString editorAuthorityToken;
        QString arrangementAuthorityToken;
        quint64 membershipRevision = 0;
        quint64 contractRevision = 0;
        quint64 editorRevision = 0;
        quint64 arrangementRevision = 0;
        int sessionPeerLimit = 0;
        int reconnectAttempts = 0;
        int reconnectAttemptLimit = 0;
        int reconnectIntervalMs = 0;
        int heartbeatIntervalMs = 0;
        int heartbeatMissLimit = 0;
        int heartbeatMissed = 0;
        qint64 lastHeartbeatAgeMs = -1;
        quint64 heartbeatsSent = 0;
        quint64 heartbeatsReceived = 0;
        quint64 heartbeatAcksReceived = 0;
        quint64 validationRejections = 0;
        quint64 authorizationRejections = 0;
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
        int heartbeatIntervalMs = kDefaultHeartbeatIntervalMs;
        int heartbeatMissLimit = kDefaultHeartbeatMissLimit;
        jam2::ConnectionStrategy strategy = jam2::ConnectionStrategy::Stun;
        jam2::Endpoint rendezvousServer{"jam2gether.com", 40404};
        QString name;
        QString bindHost = QStringLiteral("0.0.0.0");
        std::optional<jam2::Endpoint> publicEndpoint;
        jam2::Endpoint stunServer{"stun.l.google.com",19302};
        int stunTimeoutMs = 1000, stunRetries = 3;
        jam2::application::UdpSocketBuffers socketBuffers;
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
        int heartbeatIntervalMs = kDefaultHeartbeatIntervalMs;
        int heartbeatMissLimit = kDefaultHeartbeatMissLimit;
        jam2::ConnectionStrategy strategy = jam2::ConnectionStrategy::Stun;
        QString name;
        QString bindHost = QStringLiteral("0.0.0.0");
        quint16 localPort = 0;
        jam2::application::UdpSocketBuffers socketBuffers;
    };

    explicit SharedSessionController(QObject* parent = nullptr);

    void bindRuntime(
        ApplicationRuntime& runtime,
        std::function<Jam2RuntimeOptions(const Snapshot&)> networkOptionsFactory);
    bool startLocal(const Jam2RuntimeOptions& options);
    bool startCreator(const CreatorConfig& config);
    bool startJoiner(const JoinerConfig& config);
    bool endSession(const QString& detail = QStringLiteral("The jam creator ended the session"));
    void close();
    void refresh();
    bool reconnectControlStream();
    void setReconnectEnabled(bool enabled);
    // Select the measured Server receive experiment before starting a jam.
    bool configureServerReceiveModel(jam2::application::UdpMultiplexTransport::ReceiveModel model);

    void updateLocalEndpoint(const QString& endpoint);
    void updatePeerEdgeState(
        quint64 peerId,
        EdgeState state,
        const QString& proofState = QString(),
        const QString& streamState = QString());

    bool send(const QJsonObject& message);
    bool sendTo(const QString& token, const QJsonObject& message, bool closeAfterWrite = false);
    bool sendBinaryTo(const QString& token, const QByteArray& payload);
    bool canQueueTo(const QString& token, qint64 additionalBytes) const;
    bool sendAssetTo(const QString& token, const QJsonObject& message);
    bool sendAssetBinaryTo(const QString& token, const QByteArray& payload);
    bool canQueueAssetTo(const QString& token, qint64 additionalBytes) const;
    void setAssetChannelRequired(bool required);
    bool assetChannelRequired() const noexcept { return assetChannelRequired_; }
    bool assetChannelConnected() const noexcept { return assetClient_.isConnected(); }
    bool hasPeer() const;
    bool isConnected() const;
    bool isServer() const noexcept { return role_ == Role::Creator; }
    QString errorString() const;
    Snapshot snapshot() const;
    QString peerName(const QString& token) const;
    QString peerName(quint64 id) const;
    jam2::application::UdpMultiplexTransport::Snapshot multiplexSnapshot() const;
    QJsonObject multiplexDiagnostics() const;
    std::function<void(const QString&)> onConnectionWarning;
    std::function<void(const QString&)> onSignalerStatus;
    ControlServer::Stats serverStats() const { return server_.stats(); }
    ControlClient::Stats clientStats() const { return client_.stats(); }
    ControlClient::Stats assetClientStats() const { return assetClient_.stats(); }

    std::function<void(const jam2::control_protocol::TransportEvent&, bool)> onTransportEvent;
    std::function<void(const Snapshot&)> onSnapshot;
    std::function<void(const QString&, const QJsonObject&)> onMessage;
    std::function<void(const QString&, const QByteArray&)> onBinaryMessage;
    std::function<void(const QString&, const QJsonObject&)> onAssetMessage;
    std::function<void(const QString&, const QByteArray&)> onAssetBinaryMessage;
    std::function<void(const QString&)> onAssetDisconnected;
    std::function<void(const QString&, const ControlServer::AuthenticatedPeer&)> onPeerAuthenticated;
    std::function<void(const QString&)> onPeerDisconnected;
    std::function<void(const SessionContract&)> onContract;

private:
    jam2::application::UdpMultiplexTransport::ReceiveModel serverReceiveModel_ = jam2::application::UdpMultiplexTransport::ReceiveModel::Runtime;
    struct Peer {
        jam2::application::PeerName name;
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
        QString editorAuthorityToken;
        QString arrangementAuthorityToken;
        quint64 editorRevision = 0;
        quint64 arrangementRevision = 0;
        QMap<int, QJsonObject> pages;
    };

    bool startMultiplex(bool creator);
    void recoverMultiplexPeer(const QString& peer);
    void introducePair(const QString& a, const QString& b);
    void introduceMeshPeer(const QString& token);
    void connectionWarning(const QString& a, const QString& b, const QString& detail, bool broadcast);
    void handleServerEvent(const jam2::control_protocol::TransportEvent& event);
    void handleClientEvent(const jam2::control_protocol::TransportEvent& event);
    void handleAssetClientEvent(const jam2::control_protocol::TransportEvent& event);
    void interruptAssetJoiner();
    void handleAuthenticatedPeer(const QString& token, const ControlServer::AuthenticatedPeer& message);
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
    void connectAssetJoiner();
    void sendHeartbeat(const QString& targetToken = {});
    void checkHeartbeatDeadline();
    void expireCoordinatorHeartbeat();
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
    struct PeerNameBinding { quint64 peerId; jam2::application::PeerName name; };
    QMap<QString, PeerNameBinding> knownPeerNames_;
    MembershipAssembly membershipAssembly_;
    QString coordinatorToken_;
    QString editorAuthorityToken_;
    QString arrangementAuthorityToken_;
    quint64 membershipRevision_ = 0;
    quint64 contractRevision_ = 0;
    quint64 editorRevision_ = 0;
    quint64 arrangementRevision_ = 0;
    bool contractReady_ = false;
    SessionContract contract_;
    bool reconnectEnabled_ = false;
    bool everAuthenticated_ = false;
    bool closing_ = false;
    int reconnectAttempts_ = 0;
    int reconnectAttemptLimit_ = 15;
    jam2::control_protocol::TransportFailure failure_ =
        jam2::control_protocol::TransportFailure::None;
    QString failureDetail_;
    bool failureRetryable_ = false;
    QJsonObject editorState_;
    QJsonObject arrangementState_;
    QMap<QString, quint64> controlRecoveryDeadlines_;
    int heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
    int heartbeatMissLimit_ = kDefaultHeartbeatMissLimit;
    int heartbeatSequence_ = 0;
    quint64 heartbeatsSent_ = 0;
    quint64 heartbeatsReceived_ = 0;
    quint64 heartbeatAcksReceived_ = 0;
    quint64 validationRejections_ = 0;
    quint64 authorizationRejections_ = 0;
    QElapsedTimer lastHeartbeat_;
    ApplicationRuntime* runtime_ = nullptr;
    std::function<Jam2RuntimeOptions(const Snapshot&)> networkOptionsFactory_;
    std::vector<Jam2RuntimePeer> runtimePeers_;
    bool runtimeAttachmentEnabled_ = false;
    bool runtimeAttachedForSession_ = false;
    bool reconcilingRuntime_ = false;
    QTimer reconnectTimer_;
    QTimer assetReconnectTimer_;
    QTimer assetIdleCloseTimer_;
    QTimer heartbeatTimer_;
    QTimer multiplexDiagnosticsTimer_;
    QTimer heartbeatDeadlineTimer_;
    ControlServer server_;
    ControlClient client_;
    ControlClient assetClient_;
    bool assetChannelRequired_ = false;
    std::shared_ptr<jam2::application::UdpMultiplexTransport> multiplex_;
    QSet<QString> introductionWarnings_;
    std::set<std::pair<QString, QString>> recoveringUdpPairs_;
};
