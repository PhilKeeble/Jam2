#include "SessionController.hpp"

#include "ControlClient.hpp"
#include "ControlProtocol.hpp"
#include "ControlServer.hpp"
#include "Jam2Process.hpp"

#include "common.hpp"
#include "tuning_profile.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTimer>
#include <QUdpSocket>

#include <cstdio>
#include <optional>
#include <stdexcept>

namespace {

QString siblingToolPath(const char* binary)
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sibling = appDir.absoluteFilePath(QString::fromLatin1(binary));
    if (QFileInfo::exists(sibling)) {
        return sibling;
    }

    QDir releaseDir(appDir);
    if (releaseDir.dirName() == QStringLiteral("MacOS") &&
        releaseDir.cdUp() &&
        releaseDir.dirName() == QStringLiteral("Contents") &&
        releaseDir.cdUp() &&
        releaseDir.dirName().endsWith(QStringLiteral(".app")) &&
        releaseDir.cdUp()) {
        const QString besideBundle = releaseDir.absoluteFilePath(QString::fromLatin1(binary));
        if (QFileInfo::exists(besideBundle)) {
            return besideBundle;
        }
    }

    QDir root(appDir);
    if (root.dirName() == QStringLiteral("release")) {
        root.cdUp();
    }
    return root.absoluteFilePath(QStringLiteral("release/%1").arg(QString::fromLatin1(binary)));
}

bool isHelpArgument(const QString& value)
{
    return value == QStringLiteral("-h") || value == QStringLiteral("--help") ||
        value == QStringLiteral("help");
}

QString sessionHex(std::uint64_t value)
{
    return QString::number(value, 16).rightJustified(16, QLatin1Char('0'));
}

QString keyHex(const std::array<std::uint8_t, 16>& key)
{
    return QString::fromStdString(jam2::hex_encode(key.data(), key.size()));
}

quint64 peerIdForToken(const QString& token)
{
    bool ok = false;
    const quint64 value = token.left(16).toULongLong(&ok, 16);
    return ok && value != 0 ? value : 1ULL;
}

QString normalizedPeerHost(QString host)
{
    if (host.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
        host = host.mid(7);
    }
    return host;
}

void writeConsoleLine(const QString& line, bool error = false)
{
    const QByteArray bytes = line.toUtf8();
    FILE* stream = error ? stderr : stdout;
    (void)std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stream);
    (void)std::fwrite("\n", 1, 1, stream);
    (void)std::fflush(stream);
}

class NetworkCommandController final : public QObject {
public:
    NetworkCommandController(int argc, char* argv[])
        : runner_(this), server_(this), client_(this)
    {
        if (argc < 3 || QString::fromLocal8Bit(argv[1]) != QStringLiteral("network")) {
            throw std::runtime_error("invalid network bootstrap command");
        }
        const QString operation = QString::fromLocal8Bit(argv[2]);
        creator_ = operation == QStringLiteral("create");
        if (!creator_ && operation != QStringLiteral("join")) {
            throw std::runtime_error("network bootstrap supports only create or join");
        }

        int optionStart = 3;
        if (!creator_) {
            if (argc < 4) {
                throw std::runtime_error("network join requires a jam2 URL");
            }
            const QByteArray url = QByteArray(argv[3]);
            session_ = jam2::parse_jam_url(std::string_view(url.constData(), static_cast<std::size_t>(url.size())));
            optionStart = 4;
        }
        for (int i = optionStart; i < argc; ++i) {
            originalOptions_.push_back(QString::fromLocal8Bit(argv[i]));
        }
        parseBootstrapOptions();
        determineRequestedContract();

        if (creator_) {
            if (!explicitSessionId_.isEmpty()) {
                session_.session_id = jam2::parse_hex_u64(explicitSessionId_.toStdString());
                const auto decoded = jam2::hex_decode(explicitSessionKey_.toStdString());
                if (decoded.size() != session_.key.size()) {
                    throw std::runtime_error("--session-key must be 16 bytes encoded as 32 hex characters");
                }
                std::copy(decoded.begin(), decoded.end(), session_.key.begin());
            } else {
                session_.session_id = jam2::random_u64();
                session_.key = jam2::random_key();
            }
        }
        sessionHex_ = sessionHex(session_.session_id);
        keyHex_ = keyHex(session_.key);

        const QByteArray inheritedToken = qgetenv("JAM2_DEBUG_PEER_TOKEN");
        const bool debugScenario = qgetenv("JAM2_DEBUG_SCENARIO") == QByteArrayLiteral("1");
        if (debugScenario && jam2::control_protocol::decodeHex(QString::fromLatin1(inheritedToken), 16).size() == 16) {
            localToken_ = QString::fromLatin1(inheritedToken).toLower();
        } else {
            localToken_ = jam2::control_protocol::encodeHex(jam2::control_protocol::randomNonce());
        }

        runner_.onOutputLine = [this](const QString& line) { handleRunnerLine(line, false); };
        runner_.onErrorLine = [this](const QString& line) { handleRunnerLine(line, true); };
        runner_.onFinished = [this](int code) { handleRunnerFinished(code); };
        server_.onAuthenticated = [this](const QString& token, const QJsonObject& message) {
            handleAuthenticatedPeer(token, message);
        };
        server_.onDisconnected = [this](const QString& token) {
            if (peers_.remove(token) > 0) {
                updateRunnerPeers();
                broadcastMembership();
            }
        };
        server_.onMessage = [this](const QString& token, const QJsonObject& message) {
            if (message.value(QStringLiteral("type")).toString() == QStringLiteral("session.error")) {
                writeConsoleLine(QStringLiteral("peer %1: %2")
                    .arg(token.left(8), message.value(QStringLiteral("message")).toString()), true);
            }
        };
        server_.onState = [](const QString& state) { writeConsoleLine(QStringLiteral("TCP control: ") + state); };
        client_.onMessage = [this](const QJsonObject& message) { handleCoordinatorMessage(message); };
        client_.onState = [this](const QString& state) { handleClientState(state); };
    }

    void start()
    {
        if (waitMs_ > 0) {
            QTimer::singleShot(waitMs_, this, [this] {
                if ((creator_ && !hadAuthenticatedPeer_) || (!creator_ && !networkStarted_)) {
                    emitStartup(QStringLiteral("error"), QStringLiteral("TCP bootstrap timed out"));
                    writeConsoleLine(QStringLiteral("TCP bootstrap timed out"), true);
                    finish(3);
                }
            });
        }
        if (creator_) {
            writeConsoleLine(QStringLiteral("Mode: create"));
            coordinatorToken_ = localToken_;
            startRunner(bindText_.isEmpty() ? QStringLiteral("0.0.0.0:49000") : bindText_);
        } else {
            writeConsoleLine(QStringLiteral("Mode: join"));
            reserveJoinEndpoint();
            emitStartup(QStringLiteral("connecting"));
            connectCoordinator();
        }
    }

private:
    struct Contract {
        QString profile;
        int sampleRate = 48000;
        int frameSize = 128;
    };

    QString takeOptionValue(int& index, const QString& name)
    {
        if (index + 1 >= originalOptions_.size()) {
            throw std::runtime_error((QStringLiteral("missing value for ") + name).toStdString());
        }
        return originalOptions_[++index];
    }

    void parseBootstrapOptions()
    {
        bool haveSessionId = false;
        bool haveSessionKey = false;
        for (int i = 0; i < originalOptions_.size(); ++i) {
            const QString arg = originalOptions_[i];
            if (arg == QStringLiteral("--bind")) {
                bindText_ = takeOptionValue(i, arg);
            } else if (arg == QStringLiteral("--wait-ms")) {
                bool ok = false;
                waitMs_ = takeOptionValue(i, arg).toInt(&ok);
                if (!ok || waitMs_ < 0) {
                    throw std::runtime_error("--wait-ms must be non-negative");
                }
            } else if (arg == QStringLiteral("--machine-readable-startup")) {
                const QString value = takeOptionValue(i, arg);
                machineStartup_ = value == QStringLiteral("on") || value == QStringLiteral("true") || value == QStringLiteral("1");
            } else if (arg == QStringLiteral("--session-id")) {
                explicitSessionId_ = takeOptionValue(i, arg);
                haveSessionId = true;
            } else if (arg == QStringLiteral("--session-key")) {
                explicitSessionKey_ = takeOptionValue(i, arg);
                haveSessionKey = true;
            } else if (arg == QStringLiteral("--public-endpoint")) {
                publicEndpointText_ = takeOptionValue(i, arg);
                bootstrapRunnerOptions_ << arg << publicEndpointText_;
            } else if (arg == QStringLiteral("--stun") ||
                       arg == QStringLiteral("--stun-timeout-ms") ||
                       arg == QStringLiteral("--stun-retries")) {
                bootstrapRunnerOptions_ << arg << takeOptionValue(i, arg);
            } else if (arg == QStringLiteral("--no-stun")) {
                noStun_ = true;
                bootstrapRunnerOptions_ << arg;
            } else if (arg == QStringLiteral("--peers") ||
                       arg == QStringLiteral("--local-peer-id") ||
                       arg == QStringLiteral("--bootstrap-coordinator-peer-id") ||
                       arg == QStringLiteral("--bootstrap-role")) {
                (void)takeOptionValue(i, arg);
            } else {
                runnerOptions_ << arg;
            }
        }
        if (creator_ && haveSessionId != haveSessionKey) {
            throw std::runtime_error("--session-id and --session-key must be provided together");
        }
        if (!creator_ && (haveSessionId || haveSessionKey || !publicEndpointText_.isEmpty())) {
            throw std::runtime_error("join session identity and coordinator endpoint come from the jam2 URL");
        }
    }

    void determineRequestedContract()
    {
        QString profileName = QStringLiteral("fast");
        for (int i = 0; i + 1 < originalOptions_.size(); ++i) {
            if (originalOptions_[i] == QStringLiteral("--profile")) {
                profileName = originalOptions_[i + 1];
            }
        }
        const QByteArray profileBytes = profileName.toUtf8();
        const jam2::TuningProfile* profile = jam2::find_tuning_profile(
            std::string_view(profileBytes.constData(), static_cast<std::size_t>(profileBytes.size())));
        if (profile == nullptr) {
            throw std::runtime_error("--profile must be fast, moderate, or safe");
        }
        requestedContract_.profile = profileName;
        requestedContract_.sampleRate = profile->sample_rate;
        requestedContract_.frameSize = profile->frame_size;
        for (int i = 0; i + 1 < originalOptions_.size(); ++i) {
            if (originalOptions_[i] == QStringLiteral("--sample-rate")) {
                requestedContract_.sampleRate = originalOptions_[i + 1].toInt();
            } else if (originalOptions_[i] == QStringLiteral("--frame-size")) {
                requestedContract_.frameSize = originalOptions_[i + 1].toInt();
            }
        }
    }

    void reserveJoinEndpoint()
    {
        const QString requested = bindText_.isEmpty() ? QStringLiteral("0.0.0.0:0") : bindText_;
        const jam2::Endpoint endpoint = jam2::parse_bind_endpoint(requested.toStdString());
        QHostAddress address(QString::fromStdString(endpoint.host));
        if (address.isNull() && endpoint.host == "0.0.0.0") {
            address = QHostAddress::AnyIPv4;
        }
        if (address.isNull()) {
            throw std::runtime_error("join --bind host must be a numeric local address");
        }
        QUdpSocket reservation;
        if (!reservation.bind(address, endpoint.port)) {
            throw std::runtime_error((QStringLiteral("could not reserve join UDP endpoint: ") + reservation.errorString()).toStdString());
        }
        localEndpointText_ = QStringLiteral("%1:%2")
            .arg(QString::fromStdString(endpoint.host))
            .arg(reservation.localPort());
        reservation.close();
    }

    QStringList peerSpecsExcludingSelf() const
    {
        QStringList result;
        const QJsonObject observer = debugTopology().value(localToken_).toObject();
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
            if (it.key() != localToken_ && !it.value().isEmpty()) {
                const QString endpoint = observer.value(it.key()).toString(it.value());
                result << QStringLiteral("%1@%2").arg(peerIdForToken(it.key())).arg(endpoint);
            }
        }
        return result;
    }

    void startRunner(const QString& bind)
    {
        if (runner_.isRunning() || networkStarted_) {
            return;
        }
        const quint64 coordinatorId = peerIdForToken(coordinatorToken_.isEmpty() ? localToken_ : coordinatorToken_);
        const QStringList initialPeerSpecs = peerSpecsExcludingSelf();
        desiredRunnerPeerSpecs_ = initialPeerSpecs;
        runnerAppliedPeerSpecs_ = initialPeerSpecs;
        QStringList args{
            QStringLiteral("network"), QStringLiteral("_run"),
            QStringLiteral("--bind"), bind,
            QStringLiteral("--session-id"), sessionHex_,
            QStringLiteral("--session-key"), keyHex_,
            QStringLiteral("--bootstrap-role"), creator_ ? QStringLiteral("creator") : QStringLiteral("joiner"),
            QStringLiteral("--local-peer-id"), QString::number(peerIdForToken(localToken_)),
            QStringLiteral("--bootstrap-coordinator-peer-id"), QString::number(coordinatorId),
            QStringLiteral("--peers"), initialPeerSpecs.join(QLatin1Char(',')),
            QStringLiteral("--arm-stream-on-first-peer"),
        };
        args << bootstrapRunnerOptions_ << runnerOptions_
             << QStringLiteral("--machine-readable-startup") << QStringLiteral("on");
        networkStarted_ = true;
        runner_.start(QCoreApplication::applicationFilePath(), args, false);
    }

    void handleRunnerLine(const QString& line, bool error)
    {
        if (error) {
            writeConsoleLine(line, true);
            return;
        }
        if (line.startsWith(QStringLiteral("Embedded mesh peer update:"))) {
            runnerPeerUpdateInFlight_ = false;
            runnerAppliedPeerSpecs_ = runnerSubmittedPeerSpecs_;
            dispatchRunnerPeerUpdate();
        }
        if (line.startsWith(QLatin1Char('{'))) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line.toUtf8(), &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QJsonObject object = document.object();
                if (object.value(QStringLiteral("event")).toString() == QStringLiteral("startup") &&
                    object.value(QStringLiteral("mode")).toString() == QStringLiteral("mesh")) {
                    runnerStartup_ = object;
                    const QString endpoint = object.value(QStringLiteral("local_endpoint")).toString();
                    if (!endpoint.isEmpty()) {
                        localEndpointText_ = endpoint;
                    }
                    if (creator_) {
                        completeCreatorBootstrap();
                    } else if (contractReady_ && membershipReady_) {
                        emitJoinConnected();
                    }
                    dispatchRunnerPeerUpdate();
                    return;
                }
            }
        }
        if (line.startsWith(QStringLiteral("Local UDP bind: "))) {
            localEndpointText_ = line.mid(QStringLiteral("Local UDP bind: ").size()).trimmed();
        } else if (line.startsWith(QStringLiteral("Public UDP candidate: "))) {
            publicCandidateText_ = line.mid(QStringLiteral("Public UDP candidate: ").size()).trimmed();
        }
        if (line == QStringLiteral("Mode: network session") || line.startsWith(QStringLiteral("Mesh peers: ")) ||
            line.startsWith(QStringLiteral("  peer"))) {
            return;
        }
        writeConsoleLine(line);
    }

    void completeCreatorBootstrap()
    {
        if (controlStarted_ || localEndpointText_.isEmpty() || runnerStartup_.isEmpty()) {
            return;
        }
        const jam2::Endpoint local = jam2::parse_endpoint(localEndpointText_.toStdString());
        if (!server_.listen(local.port, sessionHex_, keyHex_)) {
            writeConsoleLine(QStringLiteral("TCP coordinator listen failed: ") + server_.errorString(), true);
            finish(2);
            return;
        }
        controlStarted_ = true;
        QString advertisedText = !publicCandidateText_.isEmpty() ? publicCandidateText_ : publicEndpointText_;
        if (advertisedText.isEmpty()) {
            QString host = QString::fromStdString(local.host);
            if (host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::")) {
                host = QStringLiteral("127.0.0.1");
            }
            advertisedText = QStringLiteral("%1:%2").arg(host).arg(local.port);
        }
        const jam2::Endpoint advertised = jam2::parse_endpoint(advertisedText.toStdString());
        session_.endpoint = advertised;
        connectionUrl_ = QString::fromStdString(jam2::make_jam_url(session_));
        peers_[localToken_] = advertisedText;
        writeConsoleLine(QStringLiteral("TCP coordinator: 0.0.0.0:%1").arg(local.port));
        writeConsoleLine(QStringLiteral("Connection string:"));
        writeConsoleLine(connectionUrl_);
        writeConsoleLine(QStringLiteral("Waiting for peers..."));
        emitStartup(QStringLiteral("waiting"));
    }

    QJsonObject contractMessage() const
    {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.contract")},
            {QStringLiteral("protocol_version"), 1},
            {QStringLiteral("audio_format"), QStringLiteral("pcm24-mono")},
            {QStringLiteral("profile"), runnerStartup_.value(QStringLiteral("profile")).toString(requestedContract_.profile)},
            {QStringLiteral("sample_rate"), runnerStartup_.value(QStringLiteral("sample_rate")).toInt(requestedContract_.sampleRate)},
            {QStringLiteral("frame_size"), runnerStartup_.value(QStringLiteral("frame_size")).toInt(requestedContract_.frameSize)},
            {QStringLiteral("coordinator_token"), localToken_},
        };
    }

    void handleAuthenticatedPeer(const QString& token, const QJsonObject& message)
    {
        QString endpoint = message.value(QStringLiteral("udp_endpoint")).toString();
        const int separator = endpoint.lastIndexOf(QLatin1Char(':'));
        const QString host = separator > 0 ? endpoint.left(separator) : QString{};
        if (host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::") || host == QStringLiteral("[::]")) {
            const QString tcpHost = normalizedPeerHost(message.value(QStringLiteral("tcp_peer_host")).toString());
            if (tcpHost.isEmpty()) {
                server_.sendTo(token, QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("session.error")},
                    {QStringLiteral("message"), QStringLiteral("wildcard UDP candidate could not be resolved")},
                }, true);
                return;
            }
            endpoint = tcpHost + endpoint.mid(separator);
        }
        peers_[token] = endpoint;
        hadAuthenticatedPeer_ = true;
        writeConsoleLine(QStringLiteral("Coordinator membership accepted peer %1; remote peers=%2")
            .arg(QString::number(peerIdForToken(token)))
            .arg(peers_.size() - 1));
        server_.sendTo(token, contractMessage());
        updateRunnerPeers();
        broadcastMembership();
        emitStartup(QStringLiteral("connected"));
    }

    QJsonObject membershipFor(const QString& observerToken) const
    {
        QJsonArray peerArray;
        const QJsonObject topology = debugTopology();
        const QJsonObject observer = topology.value(observerToken).toObject();
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
            QString endpoint = it.value();
            const QString override = observer.value(it.key()).toString();
            if (!override.isEmpty()) {
                endpoint = override;
            }
            peerArray.append(QJsonObject{
                {QStringLiteral("token"), it.key()},
                {QStringLiteral("peer_id"), QString::number(peerIdForToken(it.key()))},
                {QStringLiteral("endpoint"), endpoint},
            });
        }
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.membership")},
            {QStringLiteral("revision"), static_cast<qint64>(membershipRevision_)},
            {QStringLiteral("coordinator_token"), localToken_},
            {QStringLiteral("peers"), peerArray},
        };
    }

    QJsonObject debugTopology() const
    {
        if (qgetenv("JAM2_DEBUG_SCENARIO") != QByteArrayLiteral("1")) {
            return {};
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(qgetenv("JAM2_DEBUG_TOPOLOGY"), &error);
        return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
    }

    void broadcastMembership()
    {
        ++membershipRevision_;
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
            if (it.key() != localToken_) {
                server_.sendTo(it.key(), membershipFor(it.key()));
            }
        }
    }

    void updateRunnerPeers()
    {
        desiredRunnerPeerSpecs_ = peerSpecsExcludingSelf();
        dispatchRunnerPeerUpdate();
    }

    void dispatchRunnerPeerUpdate()
    {
        if (!runner_.isRunning() || runnerPeerUpdateInFlight_ ||
            desiredRunnerPeerSpecs_ == runnerAppliedPeerSpecs_) {
            return;
        }
        if (runner_.updatePeers(desiredRunnerPeerSpecs_)) {
            writeConsoleLine(QStringLiteral("Embedded membership handoff queued; remote peers=%1")
                .arg(desiredRunnerPeerSpecs_.size()));
            runnerSubmittedPeerSpecs_ = desiredRunnerPeerSpecs_;
            runnerPeerUpdateInFlight_ = true;
            return;
        }
        if (!runnerPeerRetryScheduled_) {
            runnerPeerRetryScheduled_ = true;
            QTimer::singleShot(10, this, [this] {
                runnerPeerRetryScheduled_ = false;
                dispatchRunnerPeerUpdate();
            });
        }
    }

    void handleCoordinatorMessage(const QJsonObject& message)
    {
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("session.contract")) {
            if (message.value(QStringLiteral("protocol_version")).toInt() != 1 ||
                message.value(QStringLiteral("audio_format")).toString() != QStringLiteral("pcm24-mono") ||
                message.value(QStringLiteral("sample_rate")).toInt() != requestedContract_.sampleRate ||
                message.value(QStringLiteral("frame_size")).toInt() != requestedContract_.frameSize) {
                const QString reason = QStringLiteral(
                    "session contract mismatch (local %1 Hz/%2 frames, coordinator %3 Hz/%4 frames)")
                    .arg(requestedContract_.sampleRate)
                    .arg(requestedContract_.frameSize)
                    .arg(message.value(QStringLiteral("sample_rate")).toInt())
                    .arg(message.value(QStringLiteral("frame_size")).toInt());
                (void)client_.send(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("session.error")},
                    {QStringLiteral("message"), reason},
                });
                writeConsoleLine(reason, true);
                emitStartup(QStringLiteral("error"), reason);
                finish(4);
                return;
            }
            coordinatorToken_ = message.value(QStringLiteral("coordinator_token")).toString();
            contractReady_ = !coordinatorToken_.isEmpty();
        } else if (type == QStringLiteral("session.membership")) {
            const QString coordinator = message.value(QStringLiteral("coordinator_token")).toString();
            const QJsonArray values = message.value(QStringLiteral("peers")).toArray();
            if (coordinator.isEmpty() || values.size() > 4096) {
                writeConsoleLine(QStringLiteral("coordinator sent invalid membership"), true);
                finish(4);
                return;
            }
            QMap<QString, QString> next;
            for (const QJsonValue& value : values) {
                const QJsonObject peer = value.toObject();
                const QString token = peer.value(QStringLiteral("token")).toString();
                const QString endpoint = peer.value(QStringLiteral("endpoint")).toString();
                if (jam2::control_protocol::decodeHex(token, 16).size() != 16 || endpoint.isEmpty()) {
                    writeConsoleLine(QStringLiteral("coordinator sent invalid membership entry"), true);
                    finish(4);
                    return;
                }
                next[token] = endpoint;
            }
            if (!next.contains(localToken_) || !next.contains(coordinator)) {
                writeConsoleLine(QStringLiteral("coordinator membership omitted required identities"), true);
                finish(4);
                return;
            }
            coordinatorToken_ = coordinator;
            peers_ = std::move(next);
            membershipReady_ = true;
            updateRunnerPeers();
        } else if (type == QStringLiteral("session.error")) {
            writeConsoleLine(message.value(QStringLiteral("message")).toString(QStringLiteral("session rejected")), true);
            finish(4);
            return;
        }
        if (contractReady_ && membershipReady_ && !networkStarted_) {
            startRunner(localEndpointText_);
        }
    }

    void handleClientState(const QString& state)
    {
        writeConsoleLine(QStringLiteral("TCP control: ") + state);
        if (state.startsWith(QStringLiteral("TCP auth failed:")) ||
            state.contains(QStringLiteral("rejected"), Qt::CaseInsensitive) ||
            state.contains(QStringLiteral("invalid"), Qt::CaseInsensitive)) {
            finish(4);
        } else if (state == QStringLiteral("TCP control authenticated")) {
            reconnectScheduled_ = false;
        } else if (state == QStringLiteral("TCP control disconnected") && !finishing_) {
            if (networkStarted_) {
                writeConsoleLine(QStringLiteral(
                    "Coordinator TCP is unavailable; existing proven UDP peers continue and membership is frozen."));
            }
            scheduleCoordinatorReconnect();
        }
    }

    void connectCoordinator()
    {
        client_.connectToHost(
            QString::fromStdString(session_.endpoint.host),
            session_.endpoint.port,
            sessionHex_,
            keyHex_,
            localToken_,
            localEndpointText_);
    }

    void scheduleCoordinatorReconnect()
    {
        if (creator_ || finishing_ || reconnectScheduled_) {
            return;
        }
        reconnectScheduled_ = true;
        QTimer::singleShot(1000, this, [this] {
            reconnectScheduled_ = false;
            if (!finishing_ && !client_.isConnected()) {
                connectCoordinator();
            }
        });
    }

    void emitJoinConnected()
    {
        if (joinedStartupEmitted_ || runnerStartup_.isEmpty()) {
            return;
        }
        joinedStartupEmitted_ = true;
        writeConsoleLine(QStringLiteral("TCP bootstrap complete; direct UDP mesh active."));
        emitStartup(QStringLiteral("connected"));
    }

    void emitStartup(const QString& stage, const QString& error = QString())
    {
        if (!machineStartup_) {
            return;
        }
        QJsonObject object = runnerStartup_;
        object[QStringLiteral("event")] = QStringLiteral("startup");
        object[QStringLiteral("mode")] = creator_ ? QStringLiteral("create") : QStringLiteral("join");
        object[QStringLiteral("stage")] = stage;
        if (!localEndpointText_.isEmpty()) {
            object[QStringLiteral("local_endpoint")] = localEndpointText_;
        }
        object[QStringLiteral("endpoint_mode")] = creator_
            ? (publicCandidateText_.isEmpty() ? QStringLiteral("local/manual") : QStringLiteral("STUN/manual public candidate"))
            : QStringLiteral("jam2-url TCP bootstrap");
        if (creator_ && !connectionUrl_.isEmpty()) {
            object[QStringLiteral("connection_url")] = connectionUrl_;
        } else if (!creator_) {
            object[QStringLiteral("peer_endpoint")] = QString::fromStdString(jam2::endpoint_to_string(session_.endpoint));
        }
        if (!error.isEmpty()) {
            object[QStringLiteral("error")] = error;
        }
        writeConsoleLine(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    }

    void finish(int code)
    {
        if (finishing_) {
            return;
        }
        finishing_ = true;
        exitOverride_ = code;
        server_.close();
        client_.close();
        if (runner_.isRunning()) {
            runner_.stop();
        } else {
            QCoreApplication::exit(code);
        }
    }

    void handleRunnerFinished(int code)
    {
        server_.close();
        client_.close();
        QCoreApplication::exit(exitOverride_.value_or(code));
    }

    bool creator_ = false;
    bool noStun_ = false;
    bool machineStartup_ = false;
    bool networkStarted_ = false;
    bool controlStarted_ = false;
    bool contractReady_ = false;
    bool membershipReady_ = false;
    bool joinedStartupEmitted_ = false;
    bool hadAuthenticatedPeer_ = false;
    bool reconnectScheduled_ = false;
    bool runnerPeerUpdateInFlight_ = false;
    bool runnerPeerRetryScheduled_ = false;
    bool finishing_ = false;
    int waitMs_ = 0;
    quint64 membershipRevision_ = 0;
    QStringList originalOptions_;
    QStringList runnerOptions_;
    QStringList bootstrapRunnerOptions_;
    QString bindText_;
    QString publicEndpointText_;
    QString publicCandidateText_;
    QString explicitSessionId_;
    QString explicitSessionKey_;
    QString sessionHex_;
    QString keyHex_;
    QString localToken_;
    QString coordinatorToken_;
    QString localEndpointText_;
    QString connectionUrl_;
    Contract requestedContract_;
    jam2::SessionInfo session_;
    QJsonObject runnerStartup_;
    QMap<QString, QString> peers_;
    QStringList desiredRunnerPeerSpecs_;
    QStringList runnerSubmittedPeerSpecs_;
    QStringList runnerAppliedPeerSpecs_;
    std::optional<int> exitOverride_;
    Jam2Process runner_;
    ControlServer server_;
    ControlClient client_;
};

}

QString SessionController::defaultJam2Path()
{
#if defined(Q_OS_WIN)
    constexpr const char* binary = "jam2.exe";
#else
    constexpr const char* binary = "jam2";
#endif
    return siblingToolPath(binary);
}

QString SessionController::defaultBindHost()
{
    return QStringLiteral("0.0.0.0");
}

QString SessionController::defaultPublicHost()
{
    return QStringLiteral("127.0.0.1");
}

bool SessionController::handlesNetworkCommand(int argc, char* argv[])
{
    if (argc < 3 || QString::fromLocal8Bit(argv[1]) != QStringLiteral("network")) {
        return false;
    }
    const QString operation = QString::fromLocal8Bit(argv[2]);
    if (operation != QStringLiteral("create") && operation != QStringLiteral("join")) {
        return false;
    }
    for (int i = 3; i < argc; ++i) {
        if (isHelpArgument(QString::fromLocal8Bit(argv[i]))) {
            return false;
        }
    }
    return true;
}

int SessionController::runNetworkCommand(int argc, char* argv[])
{
    try {
        NetworkCommandController controller(argc, argv);
        QTimer::singleShot(0, &controller, [&controller] { controller.start(); });
        return QCoreApplication::exec();
    } catch (const std::exception& error) {
        writeConsoleLine(QString::fromUtf8(error.what()), true);
        return 2;
    }
}
