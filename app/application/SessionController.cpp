#include "SessionController.hpp"

#include "ApplicationRuntime.hpp"
#include "AutomationChannel.hpp"
#include "ControlProtocol.hpp"
#include "DebugActionValidation.hpp"
#include "NativeTcpTransport.hpp"
#include "SharedSessionController.hpp"

#include "common.hpp"
#include "tuning_profile.hpp"
#include "udp_socket.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

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

quint64 requirePeerId(const QString& token)
{
    const auto peerId = jam2::control_protocol::peerIdFromToken(token);
    if (!peerId) {
        throw std::runtime_error("peer token must encode a stable non-zero peer id");
    }
    return *peerId;
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
        : runtime_(this), sessionController_(this)
    {
        if (QCoreApplication::instance() != nullptr) {
            debugScenario_ = QCoreApplication::instance()
                ->property("jam2.debug.scenario").toJsonObject();
            debugNetwork_ = debugScenario_.value(QStringLiteral("network")).toObject();
        }
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
        runtimeOptions_ = parseRuntimeOptions(runnerOptions_ + bootstrapRunnerOptions_);
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

        const QString configuredToken = debugNetwork_.value(QStringLiteral("peer_token")).toString();
        if (!debugScenario_.isEmpty() && !configuredToken.isEmpty()) {
            if (!jam2::control_protocol::peerIdFromToken(configuredToken).has_value()) {
                throw std::runtime_error(
                    "debug scenario peer_token must encode a stable non-zero peer id");
            }
            localToken_ = configuredToken.toLower();
        } else {
            localToken_ = jam2::control_protocol::randomPeerToken();
        }

        runtime_.onStartup = [this](const Jam2RuntimeStartup& startup) {
            handleRuntimeStartup(startup);
        };
        runtime_.onError = [this](const QString& line) {
            writeConsoleLine(line, true);
            emitAutomation(QStringLiteral("error"), {{QStringLiteral("message"), line}});
        };
        runtime_.onEngineEvent = [this](const jam2::EngineEvent& event) {
            if (event.type == jam2::EngineEventType::JamRecordingStopped) {
                writeRecordingSidecar();
            }
            handleAutomationEngineEvent(event);
        };
        runtime_.onNetworkSnapshot = [this](const jam2::NetworkSessionSnapshot& snapshot) {
            for (const jam2::NetworkPeerSnapshot& peer : snapshot.peers) {
                const SharedSessionController::EdgeState edge =
                    peer.descriptor.endpoint_state == jam2::PeerEndpointState::Active
                        ? SharedSessionController::EdgeState::Active
                        : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Probing
                            ? SharedSessionController::EdgeState::Probing
                            : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Failed
                                ? SharedSessionController::EdgeState::Failed
                                : SharedSessionController::EdgeState::Candidate;
                const QString proof =
                    peer.descriptor.endpoint_state == jam2::PeerEndpointState::Active
                        ? QStringLiteral("active")
                        : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Probing
                            ? QStringLiteral("probing")
                            : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Failed
                                ? QStringLiteral("failed") : QStringLiteral("candidate");
                sessionController_.updatePeerEdgeState(
                    peer.descriptor.peer_id.value,
                    edge,
                    proof,
                    peer.stream.expected_remote_sample_time > 0
                        ? QStringLiteral("receiving") : QStringLiteral("waiting"));
            }
        };
        runtime_.onNetworkFinished = [this](int code) { handleRuntimeFinished(code); };
        sessionController_.onPeerAuthenticated = [this](const QString& token, const QJsonObject& message) {
            handleAuthenticatedPeer(token, message);
        };
        sessionController_.onSnapshot = [this](const SharedSessionController::Snapshot& snapshot) {
            if (snapshot.role == SharedSessionController::Role::Inactive) {
                return;
            }
            QMap<QString, QString> next;
            for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
                next[peer.token] = peer.endpoint;
            }
            peers_ = std::move(next);
            maxRemotePeerCount_ = std::max(maxRemotePeerCount_, snapshot.remotePeerCount);
            int activeRemotePeers = 0;
            for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
                if (peer.token != snapshot.localToken &&
                    peer.edgeState == SharedSessionController::EdgeState::Active) {
                    ++activeRemotePeers;
                }
            }
            maxActiveRemotePeerCount_ = std::max(maxActiveRemotePeerCount_, activeRemotePeers);
            coordinatorToken_ = snapshot.coordinatorToken;
            emitAutomation(QStringLiteral("peer_snapshot"), {
                {QStringLiteral("remote_peer_count"), snapshot.remotePeerCount},
                {QStringLiteral("coordinator_token"), snapshot.coordinatorToken},
                {QStringLiteral("network_attachment_ready"), snapshot.networkAttachmentReady},
            });
            if (!creator_ && snapshot.networkAttachmentReady && runtimeStarted_) {
                emitJoinConnected();
            }
        };
        sessionController_.onMessage = [this](const QString& token, const QJsonObject& message) {
            if (creator_) {
                if (message.value(QStringLiteral("type")).toString() == QStringLiteral("session.error")) {
                    writeConsoleLine(QStringLiteral("peer %1: %2")
                        .arg(token.left(8), message.value(QStringLiteral("message")).toString()), true);
                }
                return;
            }
            handleCoordinatorMessage(message);
        };
        sessionController_.onTransportEvent = [this](
            const jam2::control_protocol::TransportEvent& event,
            bool serverSide) {
            if (serverSide) {
                writeConsoleLine(QStringLiteral("TCP control: ") + event.detail);
            } else {
                handleClientEvent(event);
            }
        };
        sessionController_.endpointOverride = [this](const QString& observer, const QString& peer) {
            return debugTopology().value(observer).toObject().value(peer).toString();
        };
        sessionController_.bindRuntime(
            runtime_,
            [this](const SharedSessionController::Snapshot& snapshot) {
                networkStarted_ = true;
                return runtimeOptionsForSnapshot(snapshot);
            });
    }

    void start()
    {
        startAutomation();
        emitAutomation(QStringLiteral("controller_started"));
        if (waitMs_ > 0) {
            QTimer::singleShot(waitMs_, this, [this] {
                if ((creator_ && !hadAuthenticatedPeer_) ||
                    (!creator_ && !runtime_.isNetworkRunning())) {
                    emitStartup(QStringLiteral("error"), QStringLiteral("TCP bootstrap timed out"));
                    writeConsoleLine(QStringLiteral("TCP bootstrap timed out"), true);
                    finish(3);
                }
            });
        }
        if (creator_) {
            writeConsoleLine(QStringLiteral("Mode: create"));
            coordinatorToken_ = localToken_;
            startRunner(reserveCreatorEndpoint());
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

    struct PendingControllerAction {
        std::uint64_t target = 0;
        bool shutdown = false;
        bool snapshot = false;
        bool enabled = false;
        QString id;
    };

    static QString metronomeModeText(jam2::EngineMetronomeMode mode)
    {
        switch (mode) {
        case jam2::EngineMetronomeMode::LeaderAudio:
            return QStringLiteral("leader-audio");
        case jam2::EngineMetronomeMode::ListenerCompensated:
            return QStringLiteral("listener-compensated");
        case jam2::EngineMetronomeMode::SharedGrid:
        default:
            return QStringLiteral("shared-grid");
        }
    }

    static QString testInputText(Jam2TestInputMode mode)
    {
        switch (mode) {
        case Jam2TestInputMode::Silence: return QStringLiteral("silence");
        case Jam2TestInputMode::Tone440: return QStringLiteral("tone-440");
        case Jam2TestInputMode::Pulse1s: return QStringLiteral("pulse-1s");
        case Jam2TestInputMode::MetroPulse: return QStringLiteral("metro-pulse");
        case Jam2TestInputMode::Off:
        default:
            return QStringLiteral("off");
        }
    }

    void startAutomation()
    {
        if (debugScenario_.isEmpty()) {
            return;
        }
        pendingActions_ = debugScenario_.value(QStringLiteral("actions")).toArray();
        const QJsonObject automation = debugScenario_.value(QStringLiteral("automation")).toObject();
        controllerLossStops_ = automation
            .value(QStringLiteral("controller_loss")).toString(QStringLiteral("stop")) ==
            QStringLiteral("stop");
        const bool reactive = automation.value(QStringLiteral("reactive")).toBool(false);
        std::string channelError;
        automationChannel_ = AutomationChannel::fromInheritedEnvironment(reactive, channelError);
        if (!channelError.empty()) {
            throw std::runtime_error(channelError);
        }
        if (!automationChannel_) {
            return;
        }
        QPointer<NetworkCommandController> self(this);
        automationChannel_->start(
            [self](QJsonObject command) {
                if (!self) return;
                self->enqueueAutomationCommand(std::move(command));
            },
            [self](std::string error) {
                if (!self) return;
                QMetaObject::invokeMethod(self, [self, error = std::move(error)] {
                    if (!self) return;
                    self->automationDisconnects_++;
                    writeConsoleLine(QString::fromStdString(error), true);
                    if (self->controllerLossStops_) self->finish(5);
                }, Qt::QueuedConnection);
            });
        emitAutomation(QStringLiteral("hello"), {
            {QStringLiteral("run_id"), debugScenario_.value(QStringLiteral("run_id"))},
            {QStringLiteral("queue_capacity"), static_cast<qint64>(AutomationChannel::kQueueCapacity)},
            {QStringLiteral("max_frame_bytes"), static_cast<qint64>(AutomationChannel::kMaxFrameBytes)},
            {QStringLiteral("commands_per_turn"), static_cast<qint64>(AutomationChannel::kCommandsPerTurn)},
        });
    }

    void enqueueAutomationCommand(QJsonObject command)
    {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(automationCommandMutex_);
            if (automationCommandQueue_.size() >= AutomationChannel::kQueueCapacity) {
                automationCommandQueueDrops_++;
                return;
            }
            automationCommandQueue_.push_back(std::move(command));
            automationCommandQueueHighWater_ = std::max(
                automationCommandQueueHighWater_, automationCommandQueue_.size());
            if (!automationCommandDrainScheduled_) {
                automationCommandDrainScheduled_ = true;
                schedule = true;
            }
        }
        if (!schedule) return;
        QPointer<NetworkCommandController> self(this);
        QMetaObject::invokeMethod(this, [self] {
            if (self) self->drainAutomationCommands();
        }, Qt::QueuedConnection);
    }

    void drainAutomationCommands()
    {
        std::vector<QJsonObject> commands;
        commands.reserve(AutomationChannel::kCommandsPerTurn);
        bool reschedule = false;
        {
            std::lock_guard<std::mutex> lock(automationCommandMutex_);
            while (!automationCommandQueue_.empty() &&
                   commands.size() < AutomationChannel::kCommandsPerTurn) {
                commands.push_back(std::move(automationCommandQueue_.front()));
                automationCommandQueue_.pop_front();
            }
            reschedule = !automationCommandQueue_.empty();
            automationCommandDrainScheduled_ = reschedule;
        }
        for (const QJsonObject& command : commands) {
            handleAutomationCommand(command);
            if (finishing_) return;
        }
        if (reschedule) {
            QPointer<NetworkCommandController> self(this);
            QMetaObject::invokeMethod(this, [self] {
                if (self) self->drainAutomationCommands();
            }, Qt::QueuedConnection);
        }
    }

    void emitAutomation(QString event, QJsonObject fields = {})
    {
        fields.insert(QStringLiteral("event"), event);
        fields.insert(QStringLiteral("engine_frame"),
            static_cast<qint64>(runtime_.engineSnapshot().engine_frame));
        if (automationTrace_.size() < static_cast<qsizetype>(AutomationChannel::kQueueCapacity)) {
            automationTrace_.push_back(fields);
        } else {
            automationTraceDrops_++;
        }
        if (automationChannel_ && !automationChannel_->send(fields)) {
            automationEventRejects_++;
        }
        activateStaticActions(event);
    }

    void activateStaticActions(const QString& event)
    {
        if (pendingActions_.isEmpty()) return;
        QJsonArray remaining;
        QJsonArray activated;
        for (const QJsonValue& value : pendingActions_) {
            const QJsonObject action = value.toObject();
            const QString after = action.value(QStringLiteral("after_event"))
                .toString(QStringLiteral("controller_started"));
            if (after == event) {
                activated.push_back(action);
            } else {
                remaining.push_back(action);
            }
        }
        pendingActions_ = remaining;
        for (const QJsonValue& value : activated) {
            executeAutomationAction(value.toObject());
        }
    }

    void handleAutomationCommand(const QJsonObject& frame)
    {
        const QString type = frame.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("action")) {
            const QJsonObject action = frame.value(QStringLiteral("action")).toObject();
            static const std::set<QString> frameFields{
                QStringLiteral("format"), QStringLiteral("type"), QStringLiteral("action")};
            QString validationError;
            bool valid = jam2ValidateDebugAction(action, validationError) &&
                !action.contains(QStringLiteral("after_event"));
            for (auto it = frame.begin(); valid && it != frame.end(); ++it) {
                valid = frameFields.contains(it.key());
            }
            if (valid) {
                executeAutomationAction(action);
                return;
            }
        }
        if (type == QStringLiteral("snapshot")) {
            static const std::set<QString> fields{
                QStringLiteral("format"), QStringLiteral("type"), QStringLiteral("id")};
            QJsonObject action = frame;
            action.remove(QStringLiteral("format"));
            QString validationError;
            bool valid = jam2ValidateDebugAction(action, validationError);
            for (auto it = frame.begin(); it != frame.end(); ++it) valid &= fields.contains(it.key());
            if (valid) {
                emitAutomationSnapshot(frame.value(QStringLiteral("id")).toString());
                return;
            }
        }
        if (type == QStringLiteral("shutdown")) {
            static const std::set<QString> fields{
                QStringLiteral("format"), QStringLiteral("type"), QStringLiteral("id"),
                QStringLiteral("delay_frames"), QStringLiteral("apply_frame")};
            QJsonObject action = frame;
            action.remove(QStringLiteral("format"));
            QString validationError;
            bool valid = jam2ValidateDebugAction(action, validationError);
            for (auto it = frame.begin(); it != frame.end(); ++it) valid &= fields.contains(it.key());
            if (valid) {
                scheduleControllerAction(frame, true);
                return;
            }
        }
        automationCommandRejects_++;
        emitAutomation(QStringLiteral("command_rejected"), {
            {QStringLiteral("id"), frame.value(QStringLiteral("id"))},
            {QStringLiteral("reason"), QStringLiteral("unsupported automation command type")},
        });
    }

    std::uint64_t automationTargetFrame(const QJsonObject& action) const
    {
        if (action.contains(QStringLiteral("apply_frame"))) {
            return static_cast<std::uint64_t>(action.value(QStringLiteral("apply_frame")).toDouble());
        }
        if (action.contains(QStringLiteral("delay_frames"))) {
            return runtime_.engineSnapshot().engine_frame +
                static_cast<std::uint64_t>(action.value(QStringLiteral("delay_frames")).toDouble());
        }
        return 0;
    }

    QString actionIdentity(const QJsonObject& action)
    {
        const QString provided = action.value(QStringLiteral("id")).toString();
        return provided.isEmpty()
            ? QStringLiteral("action-%1").arg(++anonymousActionId_)
            : provided;
    }

    bool submitAutomationCommand(
        jam2::EngineCommand command,
        const QString& id,
        std::uint64_t applyFrame)
    {
        command.cookie = ++commandCookie_;
        command.apply_frame = applyFrame;
        commandActions_.insert(command.cookie, id);
        if (runtime_.submit(command)) {
            automationCommandsSubmitted_++;
            return true;
        }
        commandActions_.remove(command.cookie);
        automationCommandRejects_++;
        emitAutomation(QStringLiteral("command_rejected"), {
            {QStringLiteral("id"), id},
            {QStringLiteral("reason"), QStringLiteral("native engine command queue is full or unavailable")},
            {QStringLiteral("requested_frame"), static_cast<qint64>(applyFrame)},
        });
        return false;
    }

    static bool actionGain(const QJsonObject& action, int& result)
    {
        if (!action.value(QStringLiteral("value")).isDouble()) return false;
        const double value = action.value(QStringLiteral("value")).toDouble();
        if (!std::isfinite(value) || value < 0.0 || value > 4.0) return false;
        result = static_cast<int>(std::llround(value * 1000000.0));
        return true;
    }

    void executeAutomationAction(const QJsonObject& action)
    {
        const QString id = actionIdentity(action);
        const QString type = action.value(QStringLiteral("type")).toString();
        const std::uint64_t target = automationTargetFrame(action);
        const jam2::EngineSnapshot snapshot = runtime_.engineSnapshot();
        jam2::EngineCommand command;
        bool valid = true;

        if (type == QStringLiteral("metronome.enabled") &&
            action.value(QStringLiteral("enabled")).isBool()) {
            command.type = jam2::EngineCommandType::SetMetronomeEnabled;
            command.enabled = action.value(QStringLiteral("enabled")).toBool();
        } else if (type == QStringLiteral("metronome.bpm") &&
                   action.value(QStringLiteral("value")).isDouble()) {
            const int bpm = action.value(QStringLiteral("value")).toInt();
            valid = bpm >= 1 && bpm <= 400;
            command.type = jam2::EngineCommandType::SetMetronomePattern;
            command.pattern = snapshot.metronome_pattern;
            command.pattern.bpm = bpm;
        } else if (type == QStringLiteral("metronome.mode")) {
            const QString mode = action.value(QStringLiteral("mode")).toString();
            valid = mode == QStringLiteral("shared-grid") ||
                mode == QStringLiteral("leader-audio") ||
                mode == QStringLiteral("listener-compensated");
            command.type = jam2::EngineCommandType::SetMetronomeMode;
            command.value = mode == QStringLiteral("leader-audio") ? 1 :
                (mode == QStringLiteral("listener-compensated") ? 2 : 0);
        } else if (type == QStringLiteral("metronome.level")) {
            command.type = jam2::EngineCommandType::SetMetronomeLevel;
            valid = actionGain(action, command.value);
        } else if (type == QStringLiteral("remote.level")) {
            command.type = jam2::EngineCommandType::SetRemoteLevel;
            valid = actionGain(action, command.value);
        } else if (type == QStringLiteral("track.sync") &&
                   action.value(QStringLiteral("enabled")).isBool()) {
            scheduleControllerAction(action, false);
            return;
        } else if (type == QStringLiteral("track.load")) {
            command.type = jam2::EngineCommandType::LoadPreparedTrack;
            const QString path = action.value(QStringLiteral("path")).toString();
            const QString canonical = QFileInfo(path).canonicalFilePath();
            const QJsonArray fixtures = debugScenario_.value(QStringLiteral("fixtures")).toArray();
            valid = !canonical.isEmpty() && fixtures.contains(canonical) &&
                jam2::engine_command_set_text(command, path.toStdString());
        } else if (type == QStringLiteral("track.play") ||
                   type == QStringLiteral("track.stop")) {
            command.type = type == QStringLiteral("track.play")
                ? jam2::EngineCommandType::PreparedPlay
                : jam2::EngineCommandType::PreparedStop;
            command.frame = target == 0 ? snapshot.engine_frame : target;
            if (!submitAutomationCommand(command, id + QStringLiteral("/local"), target)) return;
            if (!trackSyncEnabled_) return;
            jam2::EngineCommand transport;
            transport.type = jam2::EngineCommandType::ScheduleTransport;
            transport.transport_action = type == QStringLiteral("track.play")
                ? jam2::EngineTransportAction::TrackPlay
                : jam2::EngineTransportAction::TrackStop;
            transport.transport_target_frame = command.frame;
            transport.transport_musical_frame = command.frame;
            transport.transport_countdown_start_frame = command.frame;
            (void)submitAutomationCommand(transport, id, target);
            return;
        } else if (type == QStringLiteral("track.restart") ||
                   type == QStringLiteral("track.record-start")) {
            const int countIn = action.value(QStringLiteral("count_in_bars"))
                .toInt(type == QStringLiteral("track.record-start") ? 1 : 0);
            valid = countIn >= 0 && countIn <= 8;
            const std::uint64_t transportFrame = target == 0
                ? nextBarTarget(snapshot, countIn)
                : target;
            jam2::EngineCommand seek;
            seek.type = jam2::EngineCommandType::PreparedSeek;
            seek.frame = transportFrame;
            jam2::EngineCommand play;
            play.type = jam2::EngineCommandType::PreparedPlay;
            play.frame = transportFrame;
            if (valid && !submitAutomationCommand(seek, id + QStringLiteral("/seek"), transportFrame)) return;
            if (valid && !submitAutomationCommand(play, id + QStringLiteral("/play"), transportFrame)) return;
            command.type = jam2::EngineCommandType::ScheduleTransport;
            command.transport_action = type == QStringLiteral("track.record-start")
                ? jam2::EngineTransportAction::RecordStart
                : jam2::EngineTransportAction::TrackRestart;
            command.transport_target_frame = transportFrame;
            command.transport_musical_frame = transportFrame;
            command.transport_countdown_start_frame = transportFrame;
            if (valid && trackSyncEnabled_) {
                (void)submitAutomationCommand(command, id, transportFrame);
            }
            return;
        } else if (type == QStringLiteral("recording.start")) {
            command.type = jam2::EngineCommandType::StartJamRecording;
            const QString path = action.value(QStringLiteral("path")).toString();
            const QString root = debugScenario_.value(QStringLiteral("artifacts"))
                .toObject().value(QStringLiteral("root")).toString();
            const QString absolute = QFileInfo(path).absoluteFilePath();
            const QString relative = QDir(root).relativeFilePath(absolute);
            valid = !root.isEmpty() && relative != QStringLiteral("..") &&
                !relative.startsWith(QStringLiteral("../")) && !QFileInfo(relative).isAbsolute() &&
                jam2::engine_command_set_text(command, path.toStdString());
            if (valid) activeRecordingFolder_ = path;
        } else if (type == QStringLiteral("recording.stop")) {
            command.type = jam2::EngineCommandType::StopJamRecording;
        } else if (type == QStringLiteral("snapshot")) {
            if (target == 0) emitAutomationSnapshot(id);
            else scheduleControllerAction(action, false, true);
            return;
        } else if (type == QStringLiteral("shutdown")) {
            scheduleControllerAction(action, true);
            return;
        } else {
            valid = false;
        }

        if (!valid) {
            automationCommandRejects_++;
            emitAutomation(QStringLiteral("command_rejected"), {
                {QStringLiteral("id"), id},
                {QStringLiteral("reason"), QStringLiteral("invalid action fields")},
            });
            return;
        }
        (void)submitAutomationCommand(command, id, target);
    }

    void scheduleControllerAction(
        const QJsonObject& action,
        bool shutdown,
        bool snapshot = false)
    {
        const QString id = actionIdentity(action);
        const std::uint64_t target = automationTargetFrame(action);
        if (target == 0 || runtime_.engineSnapshot().engine_frame >= target) {
            if (shutdown) {
                emitAutomation(QStringLiteral("command_applied"), {{QStringLiteral("id"), id}});
                finish(0);
            } else if (snapshot) {
                emitAutomationSnapshot(id);
            } else {
                trackSyncEnabled_ = action.value(QStringLiteral("enabled")).toBool();
                runtime_.setTrackSyncEnabled(trackSyncEnabled_);
                emitAutomation(QStringLiteral("command_applied"), {{QStringLiteral("id"), id}});
            }
            return;
        }
        if (pendingControllerActions_.size() >= AutomationChannel::kQueueCapacity) {
            automationCommandRejects_++;
            emitAutomation(QStringLiteral("command_rejected"), {
                {QStringLiteral("id"), id},
                {QStringLiteral("reason"), QStringLiteral("native controller action queue is full")},
            });
            return;
        }
        pendingControllerActions_.push_back({target, shutdown, snapshot,
            action.value(QStringLiteral("enabled")).toBool(), id});
        if (!controllerActionTimer_.isActive()) {
            controllerActionTimer_.setInterval(5);
            connect(&controllerActionTimer_, &QTimer::timeout, this, [this] {
                pollControllerActions();
            });
            controllerActionTimer_.start();
        }
    }

    void pollControllerActions()
    {
        const std::uint64_t now = runtime_.engineSnapshot().engine_frame;
        auto it = pendingControllerActions_.begin();
        while (it != pendingControllerActions_.end()) {
            if (it->target > now) { ++it; continue; }
            const auto action = *it;
            it = pendingControllerActions_.erase(it);
            if (action.shutdown) {
                emitAutomation(QStringLiteral("command_applied"), {
                    {QStringLiteral("id"), action.id},
                    {QStringLiteral("requested_frame"), static_cast<qint64>(action.target)},
                    {QStringLiteral("applied_frame"), static_cast<qint64>(now)},
                });
                finish(0);
                return;
            }
            if (action.snapshot) {
                emitAutomationSnapshot(action.id);
                continue;
            }
            trackSyncEnabled_ = action.enabled;
            runtime_.setTrackSyncEnabled(trackSyncEnabled_);
            emitAutomation(QStringLiteral("command_applied"), {
                {QStringLiteral("id"), action.id},
                {QStringLiteral("requested_frame"), static_cast<qint64>(action.target)},
                {QStringLiteral("applied_frame"), static_cast<qint64>(now)},
            });
        }
        if (pendingControllerActions_.empty()) controllerActionTimer_.stop();
    }

    void emitAutomationSnapshot(const QString& id)
    {
        const auto snapshot = runtime_.engineSnapshot();
        const auto network = sessionController_.snapshot();
        emitAutomation(QStringLiteral("snapshot"), {
            {QStringLiteral("id"), id},
            {QStringLiteral("metronome_enabled"), snapshot.metronome_enabled},
            {QStringLiteral("bpm"), snapshot.metronome_pattern.bpm},
            {QStringLiteral("remote_level_ppm"), snapshot.remote_level_ppm},
            {QStringLiteral("track_sync"), trackSyncEnabled_},
            {QStringLiteral("network_running"), runtime_.isNetworkRunning()},
            {QStringLiteral("remote_peer_count"), network.remotePeerCount},
        });
    }

    void handleAutomationEngineEvent(const jam2::EngineEvent& event)
    {
        if (event.type != jam2::EngineEventType::CommandApplied &&
            event.type != jam2::EngineEventType::CommandRejected) return;
        const QString id = commandActions_.take(event.cookie);
        if (id.isEmpty()) return;
        const qint64 difference = event.applied_frame >= event.requested_frame
            ? static_cast<qint64>(event.applied_frame - event.requested_frame)
            : -static_cast<qint64>(event.requested_frame - event.applied_frame);
        emitAutomation(event.type == jam2::EngineEventType::CommandApplied
                ? QStringLiteral("command_applied")
                : QStringLiteral("command_rejected"), {
            {QStringLiteral("id"), id},
            {QStringLiteral("cookie"), static_cast<qint64>(event.cookie)},
            {QStringLiteral("requested_frame"), static_cast<qint64>(event.requested_frame)},
            {QStringLiteral("applied_frame"), static_cast<qint64>(event.applied_frame)},
            {QStringLiteral("difference_frames"), difference},
        });
    }

    std::uint64_t nextBarTarget(
        const jam2::EngineSnapshot& snapshot,
        int extraBars = 0) const
    {
        const auto pattern = jam2::metronome::sanitize(snapshot.metronome_pattern);
        const int sampleRate = std::max(1, static_cast<int>(std::llround(snapshot.sample_rate)));
        const std::uint64_t stepFrames = jam2::metronome::step_interval_samples(
            static_cast<double>(sampleRate), pattern.bpm, pattern.division);
        const std::uint64_t barFrames = std::max<std::uint64_t>(
            1,
            stepFrames * static_cast<std::uint64_t>(pattern.division) *
                static_cast<std::uint64_t>(pattern.beats_per_bar));
        const auto musicalFromRaw = [](std::uint64_t raw, std::int64_t offset) {
            if (offset < 0) {
                const auto magnitude = static_cast<std::uint64_t>(-offset);
                return raw > magnitude ? raw - magnitude : 0ULL;
            }
            return raw + static_cast<std::uint64_t>(offset);
        };
        const auto rawFromMusical = [](std::uint64_t musical, std::int64_t offset) {
            if (offset >= 0) {
                const auto magnitude = static_cast<std::uint64_t>(offset);
                return musical > magnitude ? musical - magnitude : 0ULL;
            }
            return musical + static_cast<std::uint64_t>(-offset);
        };
        const std::uint64_t musicalNow = musicalFromRaw(
            snapshot.engine_frame, snapshot.metronome_render_offset_frames);
        const std::uint64_t epoch = snapshot.metronome_epoch_valid
            ? snapshot.metronome_epoch_frame
            : 0ULL;
        const std::uint64_t elapsed = musicalNow >= epoch ? musicalNow - epoch : 0ULL;
        std::uint64_t nextMusical = epoch + (elapsed / barFrames + 1ULL) * barFrames;
        const std::uint64_t minimumLead = static_cast<std::uint64_t>(sampleRate) / 5ULL;
        if (rawFromMusical(nextMusical, snapshot.metronome_render_offset_frames) <=
            snapshot.engine_frame + minimumLead) {
            nextMusical += barFrames;
        }
        nextMusical += static_cast<std::uint64_t>(std::max(0, extraBars)) * barFrames;
        return rawFromMusical(nextMusical, snapshot.metronome_render_offset_frames);
    }

    void writeRecordingSidecar()
    {
        if (activeRecordingFolder_.isEmpty()) {
            return;
        }
        const jam2::EngineSnapshot snapshot = runtime_.engineSnapshot();
        const auto& recording = snapshot.jam_recording;
        const auto pattern = jam2::metronome::sanitize(snapshot.metronome_pattern);
        QJsonObject object{
            {QStringLiteral("format"), QStringLiteral("pcm16_mono_wav")},
            {QStringLiteral("sample_rate"), static_cast<int>(std::llround(snapshot.sample_rate))},
            {QStringLiteral("stems"), QJsonArray{
                QStringLiteral("mix.wav"), QStringLiteral("my-input.wav"),
                QStringLiteral("their-input.wav"), QStringLiteral("inputs-mix.wav"),
                QStringLiteral("metronome.wav")}},
            {QStringLiteral("recording_folder"), activeRecordingFolder_},
            {QStringLiteral("start_audio_frame"), static_cast<qint64>(recording.start_frame)},
            {QStringLiteral("stop_audio_frame"), static_cast<qint64>(recording.stop_frame)},
            {QStringLiteral("frames_queued"), static_cast<qint64>(recording.frames_queued)},
            {QStringLiteral("frames_written"), static_cast<qint64>(recording.frames_written)},
            {QStringLiteral("dropped_frames"), static_cast<qint64>(recording.dropped_frames)},
            {QStringLiteral("drop_events"), static_cast<qint64>(recording.drop_events)},
            {QStringLiteral("writer_errors"), static_cast<qint64>(recording.writer_errors)},
            {QStringLiteral("queue_capacity_frames"), static_cast<qint64>(recording.queue_capacity_frames)},
            {QStringLiteral("metronome"), snapshot.metronome_enabled ? QStringLiteral("on") : QStringLiteral("off")},
            {QStringLiteral("bpm"), pattern.bpm},
            {QStringLiteral("metronome_level"), static_cast<double>(snapshot.metronome_level_ppm) / 1000000.0},
            {QStringLiteral("remote_level"), static_cast<double>(snapshot.remote_level_ppm) / 1000000.0},
            {QStringLiteral("send_level"), static_cast<double>(snapshot.send_level_ppm) / 1000000.0},
            {QStringLiteral("local_monitor"), snapshot.local_monitor_enabled ? QStringLiteral("on") : QStringLiteral("off")},
            {QStringLiteral("local_monitor_level"), static_cast<double>(snapshot.local_monitor_level_ppm) / 1000000.0},
            {QStringLiteral("metronome_mode"), metronomeModeText(snapshot.metronome_mode)},
            {QStringLiteral("test_input"), testInputText(runtimeOptions_.test_input)},
            {QStringLiteral("metronome_epoch_sample_time"), static_cast<qint64>(snapshot.metronome_epoch_frame)},
            {QStringLiteral("metronome_epoch_valid"), snapshot.metronome_epoch_valid},
            {QStringLiteral("metronome_beats_per_bar"), pattern.beats_per_bar},
            {QStringLiteral("metronome_division"), pattern.division},
            {QStringLiteral("metronome_step_count"), pattern.step_count},
            {QStringLiteral("metronome_play_mask_low"), static_cast<qint64>(pattern.play_mask_low)},
            {QStringLiteral("metronome_play_mask_high"), static_cast<qint64>(pattern.play_mask_high)},
            {QStringLiteral("metronome_accent_mask_low"), static_cast<qint64>(pattern.accent_mask_low)},
            {QStringLiteral("metronome_accent_mask_high"), static_cast<qint64>(pattern.accent_mask_high)},
            {QStringLiteral("sample_time_playout"), runtimeOptions_.sample_time_playout},
            {QStringLiteral("playout_delay_frames"), static_cast<qint64>(runtimeOptions_.playout_delay_frames)},
        };
        QDir().mkpath(activeRecordingFolder_);
        QFile file(QDir(activeRecordingFolder_).filePath(QStringLiteral("recording.json")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) {
            writeConsoleLine(QStringLiteral("record jam sidecar failed: ") + file.fileName(), true);
        }
        activeRecordingFolder_.clear();
    }

    QString takeOptionValue(int& index, const QString& name)
    {
        if (index + 1 >= originalOptions_.size()) {
            throw std::runtime_error((QStringLiteral("missing value for ") + name).toStdString());
        }
        return originalOptions_[++index];
    }

    static Jam2RuntimeOptions parseRuntimeOptions(const QStringList& options)
    {
        std::vector<QByteArray> storage;
        storage.reserve(static_cast<std::size_t>(options.size() + 1));
        storage.emplace_back("jam2");
        for (const QString& option : options) {
            storage.push_back(option.toUtf8());
        }
        std::vector<char*> argv;
        argv.reserve(storage.size());
        for (QByteArray& value : storage) {
            argv.push_back(value.data());
        }
        return jam2_parse_runtime_options(static_cast<int>(argv.size()), argv.data(), 1);
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
            } else if (arg == QStringLiteral("--max-peers")) {
                bool ok = false;
                sessionPeerLimit_ = takeOptionValue(i, arg).toInt(&ok);
                if (!ok || sessionPeerLimit_ < 0 || !creator_) {
                    throw std::runtime_error("--max-peers is a non-negative network create option");
                }
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
        jam2::NetworkRuntime networkRuntime;
        jam2::UdpSocket reservation;
        reservation.bind(endpoint);
        const jam2::Endpoint local = reservation.local_endpoint();
        localEndpointText_ = QStringLiteral("%1:%2")
            .arg(QString::fromStdString(endpoint.host))
            .arg(local.port);
    }

    QString reserveCreatorEndpoint() const
    {
        const QString requested = bindText_.isEmpty()
            ? QStringLiteral("0.0.0.0:49000")
            : bindText_;
        const jam2::Endpoint endpoint = jam2::parse_bind_endpoint(requested.toStdString());
        if (endpoint.port != 0) {
            return requested;
        }

        jam2::NetworkRuntime networkRuntime;
        for (int attempt = 0; attempt < 32; ++attempt) {
            jam2::application::NativeTcpPortReservation tcpReservation;
            if (!tcpReservation.bind(QString::fromStdString(endpoint.host), 0)) {
                continue;
            }
            const quint16 port = tcpReservation.localPort();
            try {
                jam2::UdpSocket udpReservation;
                udpReservation.bind(jam2::Endpoint{endpoint.host, port});
            } catch (const std::exception&) {
                continue;
            }
            return QStringLiteral("%1:%2")
                .arg(QString::fromStdString(endpoint.host))
                .arg(port);
        }
        throw std::runtime_error("could not reserve a creator port usable by both TCP and UDP");
    }

    QStringList peerSpecsExcludingSelf() const
    {
        QStringList result;
        const QJsonObject observer = debugTopology().value(localToken_).toObject();
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
            if (it.key() != localToken_ && !it.value().isEmpty()) {
                const QString endpoint = observer.value(it.key()).toString(it.value());
                result << QStringLiteral("%1@%2").arg(requirePeerId(it.key())).arg(endpoint);
            }
        }
        return result;
    }

    void applyPeerSpecs(const QStringList& specs)
    {
        runtimeOptions_.mesh_peers.clear();
        runtimeOptions_.mesh_peer_ids.clear();
        runtimeOptions_.mesh_peers.reserve(static_cast<std::size_t>(specs.size()));
        runtimeOptions_.mesh_peer_ids.reserve(static_cast<std::size_t>(specs.size()));
        for (const QString& spec : specs) {
            const qsizetype separator = spec.indexOf(QLatin1Char('@'));
            if (separator <= 0 || separator + 1 >= spec.size()) {
                throw std::runtime_error("typed membership entry must be peer-id@endpoint");
            }
            bool ok = false;
            const quint64 peerId = spec.left(separator).toULongLong(&ok);
            if (!ok || peerId == 0) {
                throw std::runtime_error("typed membership peer id must be non-zero");
            }
            runtimeOptions_.mesh_peer_ids.push_back(peerId);
            runtimeOptions_.mesh_peers.push_back(
                jam2::parse_endpoint(spec.mid(separator + 1).toStdString()));
        }
        runtimeOptions_.mesh_peers_configured = true;
    }

    void startRunner(const QString& bind)
    {
        if (runtime_.isNetworkRunning() || networkStarted_) {
            return;
        }
        const quint64 coordinatorId = requirePeerId(
            coordinatorToken_.isEmpty() ? localToken_ : coordinatorToken_);
        const QStringList initialPeerSpecs = peerSpecsExcludingSelf();
        runtimeOptions_.bind = jam2::parse_bind_endpoint(bind.toStdString());
        runtimeOptions_.session_id = session_.session_id;
        runtimeOptions_.session_key = session_.key;
        runtimeOptions_.bootstrap_role = creator_
            ? jam2::SessionBootstrapRole::Creator
            : jam2::SessionBootstrapRole::Joiner;
        runtimeOptions_.local_peer_id = requirePeerId(localToken_);
        runtimeOptions_.bootstrap_coordinator_peer_id = coordinatorId;
        runtimeOptions_.arm_stream_on_first_peer = true;
        applyPeerSpecs(initialPeerSpecs);
        networkStarted_ = true;
        if (!runtime_.startNetwork(runtimeOptions_)) {
            networkStarted_ = false;
            finish(2);
        }
    }

    Jam2RuntimeOptions runtimeOptionsForSnapshot(
        const SharedSessionController::Snapshot& snapshot) const
    {
        Jam2RuntimeOptions options = runtimeOptions_;
        options.bind = jam2::parse_bind_endpoint(localEndpointText_.toStdString());
        options.session_id = session_.session_id;
        options.session_key = session_.key;
        options.bootstrap_role = snapshot.role == SharedSessionController::Role::Creator
            ? jam2::SessionBootstrapRole::Creator
            : jam2::SessionBootstrapRole::Joiner;
        if (snapshot.localPeerId == 0 || snapshot.coordinatorPeerId == 0) {
            throw std::runtime_error("session membership is missing stable peer identities");
        }
        options.local_peer_id = snapshot.localPeerId;
        options.bootstrap_coordinator_peer_id = snapshot.coordinatorPeerId;
        options.sample_rate = snapshot.contract.sampleRate;
        options.frame_size = snapshot.contract.frameSize;
        const auto audioFormat = jam2::protocol::parse_audio_format(
            snapshot.contract.audioFormat.toStdString());
        if (!audioFormat) {
            throw std::runtime_error("session contract has an unsupported network audio format");
        }
        options.network_audio_format = *audioFormat;
        options.arm_stream_on_first_peer = true;
        options.mesh_peers.clear();
        options.mesh_peer_ids.clear();
        options.mesh_peers_configured = true;
        for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
            if (peer.token == snapshot.localToken || peer.endpoint.isEmpty()) {
                continue;
            }
            options.mesh_peer_ids.push_back(peer.peerId);
            options.mesh_peers.push_back(jam2::parse_endpoint(peer.endpoint.toStdString()));
        }
        return options;
    }

    void handleRuntimeStartup(const Jam2RuntimeStartup& startup)
    {
        localEndpointText_ = QString::fromStdString(jam2::endpoint_to_string(startup.local_endpoint));
        publicCandidateText_.clear();
        if (startup.public_candidate) {
            publicCandidateText_ = QString::fromStdString(
                jam2::endpoint_to_string(*startup.public_candidate));
        }
        runtimeStarted_ = true;
        if (creator_) {
            completeCreatorBootstrap();
        } else if (sessionController_.snapshot().networkAttachmentReady) {
            emitJoinConnected();
        }
    }

    void completeCreatorBootstrap()
    {
        if (controlStarted_ || localEndpointText_.isEmpty() || !runtimeStarted_) {
            return;
        }
        const jam2::Endpoint local = jam2::parse_endpoint(localEndpointText_.toStdString());
        QString advertisedText = !publicCandidateText_.isEmpty() ? publicCandidateText_ : publicEndpointText_;
        if (advertisedText.isEmpty()) {
            QString host = QString::fromStdString(local.host);
            if (host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::")) {
                host = QStringLiteral("127.0.0.1");
            }
            advertisedText = QStringLiteral("%1:%2").arg(host).arg(local.port);
        }
        SharedSessionController::CreatorConfig controlConfig{
            local.port,
            sessionHex_,
            keyHex_,
            localToken_,
            advertisedText,
            sessionPeerLimit_,
            SharedSessionController::SessionContract{
                jam2::protocol::kProtocolVersion,
                QString::fromLatin1(jam2::protocol::audio_format_text(runtimeOptions_.network_audio_format)),
                requestedContract_.profile,
                requestedContract_.sampleRate,
                requestedContract_.frameSize,
            }};
        controlConfig.heartbeatIntervalMs = debugNetwork_
            .value(QStringLiteral("heartbeat_interval_ms"))
            .toInt(SharedSessionController::kDefaultHeartbeatIntervalMs);
        controlConfig.heartbeatMissLimit = debugNetwork_
            .value(QStringLiteral("heartbeat_miss_limit"))
            .toInt(SharedSessionController::kDefaultHeartbeatMissLimit);
        if (!sessionController_.startCreator(controlConfig)) {
            writeConsoleLine(QStringLiteral("TCP coordinator listen failed: ") + sessionController_.errorString(), true);
            finish(2);
            return;
        }
        controlStarted_ = true;
        const jam2::Endpoint advertised = jam2::parse_endpoint(advertisedText.toStdString());
        session_.endpoint = advertised;
        connectionUrl_ = QString::fromStdString(jam2::make_jam_url(session_));
        writeConsoleLine(QStringLiteral("TCP coordinator: 0.0.0.0:%1").arg(local.port));
        writeConsoleLine(QStringLiteral("Connection string:"));
        writeConsoleLine(connectionUrl_);
        writeConsoleLine(QStringLiteral("Waiting for peers..."));
        emitStartup(QStringLiteral("waiting"));
    }

    void handleAuthenticatedPeer(const QString& token, const QJsonObject& message)
    {
        (void)message;
        hadAuthenticatedPeer_ = true;
        writeConsoleLine(QStringLiteral("Coordinator membership accepted peer %1; remote peers=%2")
            .arg(QString::number(requirePeerId(token)))
            .arg(std::max(0, static_cast<int>(peers_.size()) - 1)));
        emitStartup(QStringLiteral("connected"));
    }

    QJsonObject debugTopology() const
    {
        return debugNetwork_.value(QStringLiteral("topology")).toObject();
    }

    void handleCoordinatorMessage(const QJsonObject& message)
    {
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("session.error")) {
            writeConsoleLine(message.value(QStringLiteral("message")).toString(QStringLiteral("session rejected")), true);
            finish(4);
            return;
        }
    }

    void handleClientEvent(const jam2::control_protocol::TransportEvent& event)
    {
        using jam2::control_protocol::TransportEventType;
        writeConsoleLine(QStringLiteral("TCP control: ") + event.detail);
        if (event.type == TransportEventType::SessionEnded) {
            controlEndReason_ = event.detail;
            controlEndFailure_ = event.failure;
            writeConsoleLine(QStringLiteral("Jam ended by creator; network join is stopping."));
            finish(0);
        } else if (event.type == TransportEventType::Failure && !event.retryable) {
            controlEndReason_ = event.detail;
            controlEndFailure_ = event.failure;
            finish(4);
        } else if (event.type == TransportEventType::Disconnected && !finishing_) {
            if (runtime_.isNetworkRunning()) {
                writeConsoleLine(QStringLiteral(
                    "Coordinator TCP is unavailable; existing proven UDP peers continue and membership is frozen."));
            }
        }
    }

    void connectCoordinator()
    {
        SharedSessionController::JoinerConfig controlConfig{
            QString::fromStdString(session_.endpoint.host),
            session_.endpoint.port,
            sessionHex_,
            keyHex_,
            localToken_,
            localEndpointText_,
            SharedSessionController::SessionContract{
                jam2::protocol::kProtocolVersion,
                QString(),
                requestedContract_.profile,
                requestedContract_.sampleRate,
                requestedContract_.frameSize,
            },
            false,
        };
        controlConfig.heartbeatIntervalMs = debugNetwork_
            .value(QStringLiteral("heartbeat_interval_ms"))
            .toInt(SharedSessionController::kDefaultHeartbeatIntervalMs);
        controlConfig.heartbeatMissLimit = debugNetwork_
            .value(QStringLiteral("heartbeat_miss_limit"))
            .toInt(SharedSessionController::kDefaultHeartbeatMissLimit);
        (void)sessionController_.startJoiner(controlConfig);
    }

    void emitJoinConnected()
    {
        if (joinedStartupEmitted_ || !runtimeStarted_) {
            return;
        }
        joinedStartupEmitted_ = true;
        writeConsoleLine(QStringLiteral("TCP bootstrap complete; direct UDP mesh active."));
        emitStartup(QStringLiteral("connected"));
    }

    void emitStartup(const QString& stage, const QString& error = QString())
    {
        QJsonObject object;
        object[QStringLiteral("event")] = QStringLiteral("startup");
        object[QStringLiteral("mode")] = creator_ ? QStringLiteral("create") : QStringLiteral("join");
        object[QStringLiteral("stage")] = stage;
        object[QStringLiteral("profile")] = requestedContract_.profile;
        object[QStringLiteral("sample_rate")] = requestedContract_.sampleRate;
        object[QStringLiteral("frame_size")] = requestedContract_.frameSize;
        if (!localEndpointText_.isEmpty()) {
            object[QStringLiteral("local_endpoint")] = localEndpointText_;
        }
        object[QStringLiteral("endpoint_mode")] = creator_
            ? (publicCandidateText_.isEmpty() ? QStringLiteral("local/manual") : QStringLiteral("STUN/manual public candidate"))
            : QStringLiteral("jam2-url TCP bootstrap");
        if (creator_ && !connectionUrl_.isEmpty()) {
            object[QStringLiteral("connection_url")] = connectionUrl_;
            object[QStringLiteral("session_peer_limit")] = sessionPeerLimit_;
        } else if (!creator_) {
            object[QStringLiteral("peer_endpoint")] = QString::fromStdString(jam2::endpoint_to_string(session_.endpoint));
        }
        if (!error.isEmpty()) {
            object[QStringLiteral("error")] = error;
        }
        writeConsoleLine(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
        emitAutomation(QStringLiteral("network.") + stage, object);
    }

    void finish(int code)
    {
        if (finishing_) {
            return;
        }
        finishing_ = true;
        exitOverride_ = code;
        finalControlSnapshot_ = sessionController_.snapshot();
        hasFinalControlSnapshot_ = true;
        sessionController_.close();
        if (runtime_.isNetworkRunning()) {
            runtime_.stopNetwork();
        } else {
            publishAutomationResult(code);
            QCoreApplication::exit(code);
        }
    }

    void handleRuntimeFinished(int code)
    {
        if (!hasFinalControlSnapshot_) {
            finalControlSnapshot_ = sessionController_.snapshot();
            hasFinalControlSnapshot_ = true;
        }
        sessionController_.close();
        const int finalCode = exitOverride_.value_or(code);
        publishAutomationResult(finalCode);
        QCoreApplication::exit(finalCode);
    }

    void publishAutomationResult(int code)
    {
        if (automationResultPublished_ || debugScenario_.isEmpty()) return;
        automationResultPublished_ = true;
        const auto snapshot = runtime_.engineSnapshot();
        std::size_t commandQueuePending = 0;
        std::size_t commandQueueHighWater = 0;
        std::uint64_t commandQueueDrops = 0;
        {
            std::lock_guard<std::mutex> lock(automationCommandMutex_);
            commandQueuePending = automationCommandQueue_.size();
            commandQueueHighWater = automationCommandQueueHighWater_;
            commandQueueDrops = automationCommandQueueDrops_;
        }
        const SharedSessionController::Snapshot controlSnapshot = hasFinalControlSnapshot_
            ? finalControlSnapshot_ : sessionController_.snapshot();
        const ControlServer::Stats controlServerStats = sessionController_.serverStats();
        const ControlClient::Stats controlClientStats = sessionController_.clientStats();
        int activeControlPeers = 0;
        for (const SharedSessionController::PeerSnapshot& peer : controlSnapshot.peers) {
            if (peer.token != controlSnapshot.localToken &&
                peer.edgeState == SharedSessionController::EdgeState::Active) {
                ++activeControlPeers;
            }
        }
        QJsonObject result{
            {QStringLiteral("ok"), code == 0},
            {QStringLiteral("return_code"), code},
            {QStringLiteral("final_engine_frame"), static_cast<qint64>(snapshot.engine_frame)},
            {QStringLiteral("commands_submitted"), static_cast<qint64>(automationCommandsSubmitted_)},
            {QStringLiteral("commands_rejected"), static_cast<qint64>(automationCommandRejects_)},
            {QStringLiteral("event_rejects"), static_cast<qint64>(automationEventRejects_)},
            {QStringLiteral("controller_disconnects"), static_cast<qint64>(automationDisconnects_)},
            {QStringLiteral("pending_static_actions"), pendingActions_.size()},
            {QStringLiteral("pending_controller_actions"), static_cast<qint64>(pendingControllerActions_.size())},
            {QStringLiteral("automation_command_queue_pending"), static_cast<qint64>(commandQueuePending)},
            {QStringLiteral("automation_command_queue_high_water"), static_cast<qint64>(commandQueueHighWater)},
            {QStringLiteral("automation_command_queue_drops"), static_cast<qint64>(commandQueueDrops)},
            {QStringLiteral("remote_peer_count"), maxRemotePeerCount_},
            {QStringLiteral("local_peer_id"), controlSnapshot.localPeerId == 0
                ? QJsonValue(QJsonValue::Null)
                : QJsonValue(QString::number(controlSnapshot.localPeerId))},
            {QStringLiteral("events"), automationTrace_},
            {QStringLiteral("event_trace_drops"), static_cast<qint64>(automationTraceDrops_)},
            {QStringLiteral("heartbeat_interval_ms"), controlSnapshot.heartbeatIntervalMs},
            {QStringLiteral("heartbeat_miss_limit"), controlSnapshot.heartbeatMissLimit},
            {QStringLiteral("heartbeat_missed"), controlSnapshot.heartbeatMissed},
            {QStringLiteral("last_heartbeat_age_ms"), controlSnapshot.lastHeartbeatAgeMs},
            {QStringLiteral("heartbeats_sent"), static_cast<qint64>(controlSnapshot.heartbeatsSent)},
            {QStringLiteral("heartbeats_received"), static_cast<qint64>(controlSnapshot.heartbeatsReceived)},
            {QStringLiteral("heartbeat_acks_received"), static_cast<qint64>(controlSnapshot.heartbeatAcksReceived)},
            {QStringLiteral("control_validation_rejections"), static_cast<qint64>(controlSnapshot.validationRejections)},
            {QStringLiteral("control_authorization_rejections"), static_cast<qint64>(controlSnapshot.authorizationRejections)},
            {QStringLiteral("control_active_remote_peers"), activeControlPeers},
            {QStringLiteral("control_max_active_remote_peers"), maxActiveRemotePeerCount_},
            {QStringLiteral("control_failure"), static_cast<int>(controlEndFailure_)},
            {QStringLiteral("control_end_reason"), controlEndReason_.isEmpty()
                ? controlSnapshot.failureDetail : controlEndReason_},
            {QStringLiteral("control_server"), QJsonObject{
                {QStringLiteral("accepted_connections"), static_cast<qint64>(controlServerStats.acceptedConnections)},
                {QStringLiteral("pending_cap_rejects"), static_cast<qint64>(controlServerStats.pendingCapRejects)},
                {QStringLiteral("authentication_rate_limit_rejects"), static_cast<qint64>(controlServerStats.authenticationRateLimitRejects)},
                {QStringLiteral("authentication_rejects"), static_cast<qint64>(controlServerStats.authenticationRejects)},
                {QStringLiteral("frame_rejects"), static_cast<qint64>(controlServerStats.frameRejects)},
                {QStringLiteral("tag_or_sequence_rejects"), static_cast<qint64>(controlServerStats.sequenceOrTagRejects)},
                {QStringLiteral("input_high_water_bytes"), static_cast<qint64>(controlServerStats.maxBufferedInputBytes)},
                {QStringLiteral("output_high_water_bytes"), static_cast<qint64>(controlServerStats.maxQueuedOutputBytes)},
                {QStringLiteral("active_connections"), static_cast<qint64>(controlServerStats.activeConnections)},
                {QStringLiteral("active_connection_high_water"), static_cast<qint64>(controlServerStats.activeConnectionHighWater)},
                {QStringLiteral("disconnected_connections"), static_cast<qint64>(controlServerStats.disconnectedConnections)},
            }},
            {QStringLiteral("control_client"), QJsonObject{
                {QStringLiteral("authentication_rejects"), static_cast<qint64>(controlClientStats.authenticationRejects)},
                {QStringLiteral("frame_rejects"), static_cast<qint64>(controlClientStats.frameRejects)},
                {QStringLiteral("tag_or_sequence_rejects"), static_cast<qint64>(controlClientStats.sequenceOrTagRejects)},
                {QStringLiteral("input_high_water_bytes"), static_cast<qint64>(controlClientStats.maxBufferedInputBytes)},
                {QStringLiteral("output_high_water_bytes"), static_cast<qint64>(controlClientStats.maxQueuedOutputBytes)},
                {QStringLiteral("connection_attempts"), static_cast<qint64>(controlClientStats.connectionAttempts)},
                {QStringLiteral("completed_connections"), static_cast<qint64>(controlClientStats.completedConnections)},
                {QStringLiteral("disconnected_connections"), static_cast<qint64>(controlClientStats.disconnectedConnections)},
            }},
        };
        if (automationChannel_) {
            result.insert(QStringLiteral("channel_queue_high_water"),
                static_cast<qint64>(automationChannel_->eventQueueHighWater()));
            result.insert(QStringLiteral("channel_rejected_frames"),
                static_cast<qint64>(automationChannel_->rejectedFrames()));
            result.insert(QStringLiteral("channel_rejected_events"),
                static_cast<qint64>(automationChannel_->rejectedEvents()));
            (void)automationChannel_->send(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("shutdown")},
                {QStringLiteral("return_code"), code},
                {QStringLiteral("engine_frame"), static_cast<qint64>(snapshot.engine_frame)},
            });
            automationChannel_->stop(true);
        }
        QCoreApplication::instance()->setProperty("jam2.debug.result", result);
    }

    bool creator_ = false;
    bool noStun_ = false;
    bool networkStarted_ = false;
    bool runtimeStarted_ = false;
    bool controlStarted_ = false;
    bool joinedStartupEmitted_ = false;
    bool hadAuthenticatedPeer_ = false;
    bool finishing_ = false;
    int maxActiveRemotePeerCount_ = 0;
    QString controlEndReason_;
    jam2::control_protocol::TransportFailure controlEndFailure_ =
        jam2::control_protocol::TransportFailure::None;
    SharedSessionController::Snapshot finalControlSnapshot_;
    bool hasFinalControlSnapshot_ = false;
    int waitMs_ = 0;
    int sessionPeerLimit_ = 0;
    int maxRemotePeerCount_ = 0;
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
    QString activeRecordingFolder_;
    Contract requestedContract_;
    Jam2RuntimeOptions runtimeOptions_;
    jam2::SessionInfo session_;
    QMap<QString, QString> peers_;
    std::optional<int> exitOverride_;
    std::uint64_t commandCookie_ = 0;
    std::uint64_t anonymousActionId_ = 0;
    std::uint64_t automationCommandsSubmitted_ = 0;
    std::uint64_t automationCommandRejects_ = 0;
    std::uint64_t automationEventRejects_ = 0;
    std::uint64_t automationDisconnects_ = 0;
    std::uint64_t automationTraceDrops_ = 0;
    mutable std::mutex automationCommandMutex_;
    std::deque<QJsonObject> automationCommandQueue_;
    std::size_t automationCommandQueueHighWater_ = 0;
    std::uint64_t automationCommandQueueDrops_ = 0;
    bool automationCommandDrainScheduled_ = false;
    bool trackSyncEnabled_ = true;
    bool controllerLossStops_ = true;
    bool automationResultPublished_ = false;
    QJsonObject debugScenario_;
    QJsonObject debugNetwork_;
    QJsonArray pendingActions_;
    QJsonArray automationTrace_;
    QMap<std::uint64_t, QString> commandActions_;
    std::vector<PendingControllerAction> pendingControllerActions_;
    QTimer controllerActionTimer_;
    std::unique_ptr<AutomationChannel> automationChannel_;
    ApplicationRuntime runtime_;
    SharedSessionController sessionController_;
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
        QTimer::singleShot(0, &controller, [&controller] {
            try {
                controller.start();
            } catch (const std::exception& error) {
                writeConsoleLine(QString::fromUtf8(error.what()), true);
                QCoreApplication::exit(2);
            }
        });
        return QCoreApplication::exec();
    } catch (const std::exception& error) {
        writeConsoleLine(QString::fromUtf8(error.what()), true);
        return 2;
    }
}
