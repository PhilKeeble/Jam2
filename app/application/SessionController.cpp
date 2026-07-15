#include "SessionController.hpp"

#include "ApplicationRuntime.hpp"
#include "ControlProtocol.hpp"
#include "SharedSessionController.hpp"

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
#include <QMetaObject>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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

        const QByteArray inheritedToken = qgetenv("JAM2_DEBUG_PEER_TOKEN");
        const bool debugScenario = qgetenv("JAM2_DEBUG_SCENARIO") == QByteArrayLiteral("1");
        if (debugScenario && jam2::control_protocol::decodeHex(QString::fromLatin1(inheritedToken), 16).size() == 16) {
            localToken_ = QString::fromLatin1(inheritedToken).toLower();
        } else {
            localToken_ = jam2::control_protocol::encodeHex(jam2::control_protocol::randomNonce());
        }

        runtime_.onStartup = [this](const Jam2RuntimeStartup& startup) {
            handleRuntimeStartup(startup);
        };
        runtime_.onError = [](const QString& line) { writeConsoleLine(line, true); };
        runtime_.onEngineEvent = [this](const jam2::EngineEvent& event) {
            if (event.type == jam2::EngineEventType::JamRecordingStopped) {
                writeRecordingSidecar();
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
            coordinatorToken_ = snapshot.coordinatorToken;
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
        startConsoleInput();
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

    void startConsoleInput()
    {
        // Console input is a headless adapter only. It translates legacy
        // diagnostic lines into typed commands on the Qt owner thread; the
        // engine and UDP workers never parse or block on stdin.
        std::thread([this] {
            std::string line;
            while (std::getline(std::cin, line)) {
                const QString command = QString::fromUtf8(line).trimmed();
                QMetaObject::invokeMethod(this, [this, command] {
                    handleConsoleCommand(command);
                }, Qt::QueuedConnection);
            }
        }).detach();
    }

    bool submitConsoleCommand(jam2::EngineCommand command, const QString& context)
    {
        command.cookie = ++commandCookie_;
        if (runtime_.submit(command)) {
            return true;
        }
        writeConsoleLine(QStringLiteral("headless command rejected: ") + context, true);
        return false;
    }

    static int gainPpm(const QString& token, int current, bool& ok)
    {
        bool parsed = false;
        const double value = token.toDouble(&parsed);
        if (!parsed) {
            ok = false;
            return current;
        }
        const double gain = (token.startsWith(QLatin1Char('+')) || token.startsWith(QLatin1Char('-')))
            ? static_cast<double>(current) / 1000000.0 + value
            : value;
        if (gain < 0.0 || gain > 4.0) {
            ok = false;
            return current;
        }
        ok = true;
        return static_cast<int>(std::llround(gain * 1000000.0));
    }

    std::uint64_t parseFrameOrNow(const QString& text, bool& ok) const
    {
        if (text.isEmpty() || text == QStringLiteral("now")) {
            ok = true;
            return runtime_.engineSnapshot().engine_frame;
        }
        return text.toULongLong(&ok);
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

    void handleConsoleCommand(const QString& line)
    {
        if (line.isEmpty()) {
            return;
        }
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        const QString command = parts.value(0);
        const jam2::EngineSnapshot snapshot = runtime_.engineSnapshot();
        jam2::EngineCommand engineCommand;
        QString context = line;

        if (command == QStringLiteral("quit") || command == QStringLiteral("exit")) {
            finish(0);
            return;
        }
        if (command == QStringLiteral("bpm") && parts.size() == 2) {
            bool ok = false;
            const int bpm = parts[1].toInt(&ok);
            if (ok && bpm >= 1 && bpm <= 400) {
                engineCommand.type = jam2::EngineCommandType::SetMetronomePattern;
                engineCommand.pattern = snapshot.metronome_pattern;
                engineCommand.pattern.bpm = bpm;
                (void)submitConsoleCommand(engineCommand, context);
                return;
            }
        } else if (command == QStringLiteral("metro") && parts.size() >= 2) {
            const QString action = parts[1];
            if (action == QStringLiteral("on") || action == QStringLiteral("off")) {
                engineCommand.type = jam2::EngineCommandType::SetMetronomeEnabled;
                engineCommand.enabled = action == QStringLiteral("on");
                (void)submitConsoleCommand(engineCommand, context);
                return;
            }
            if (action == QStringLiteral("mode") && parts.size() == 3) {
                const QString mode = parts[2];
                if (mode == QStringLiteral("shared-grid") || mode == QStringLiteral("leader-audio") ||
                    mode == QStringLiteral("listener-compensated")) {
                    engineCommand.type = jam2::EngineCommandType::SetMetronomeMode;
                    engineCommand.value = mode == QStringLiteral("leader-audio") ? 1 :
                        (mode == QStringLiteral("listener-compensated") ? 2 : 0);
                    (void)submitConsoleCommand(engineCommand, context);
                    return;
                }
            }
            if (action == QStringLiteral("level") && parts.size() == 3) {
                bool ok = false;
                engineCommand.value = gainPpm(parts[2], snapshot.metronome_level_ppm, ok);
                if (ok) {
                    engineCommand.type = jam2::EngineCommandType::SetMetronomeLevel;
                    (void)submitConsoleCommand(engineCommand, context);
                    return;
                }
            }
        } else if (command == QStringLiteral("remote") && parts.size() >= 2) {
            engineCommand.type = jam2::EngineCommandType::SetRemoteLevel;
            if (parts[1] == QStringLiteral("mute")) {
                engineCommand.value = 0;
            } else if (parts[1] == QStringLiteral("unmute")) {
                engineCommand.value = 1000000;
            } else if (parts[1] == QStringLiteral("level") && parts.size() == 3) {
                bool ok = false;
                engineCommand.value = gainPpm(parts[2], snapshot.remote_level_ppm, ok);
                if (!ok) {
                    writeConsoleLine(QStringLiteral("invalid headless command: ") + line, true);
                    return;
                }
            } else {
                writeConsoleLine(QStringLiteral("invalid headless command: ") + line, true);
                return;
            }
            (void)submitConsoleCommand(engineCommand, context);
            return;
        } else if (command == QStringLiteral("track") && parts.size() >= 2) {
            const QString action = parts[1];
            if (action == QStringLiteral("sync") && parts.size() == 3 &&
                (parts[2] == QStringLiteral("on") || parts[2] == QStringLiteral("off"))) {
                trackSyncEnabled_ = parts[2] == QStringLiteral("on");
                runtime_.setTrackSyncEnabled(trackSyncEnabled_);
                return;
            } else if (action == QStringLiteral("load")) {
                const QString path = line.mid(line.indexOf(action) + action.size()).trimmed();
                engineCommand.type = jam2::EngineCommandType::LoadPreparedTrack;
                if (jam2::engine_command_set_text(engineCommand, path.toStdString())) {
                    (void)submitConsoleCommand(engineCommand, context);
                    return;
                }
            } else if (action == QStringLiteral("restart") ||
                       action == QStringLiteral("record-start")) {
                bool countInOk = true;
                const int countInBars = action == QStringLiteral("record-start")
                    ? (parts.size() == 3 ? parts[2].toInt(&countInOk) : 1)
                    : 0;
                if (!countInOk || countInBars < 0 || countInBars > 8 ||
                    (action == QStringLiteral("restart") && parts.size() != 2) ||
                    parts.size() > 3) {
                    writeConsoleLine(QStringLiteral("invalid headless command: ") + line, true);
                    return;
                }
                const std::uint64_t target = nextBarTarget(snapshot, countInBars);
                jam2::EngineCommand seek;
                seek.type = jam2::EngineCommandType::PreparedSeek;
                seek.frame = target;
                if (!submitConsoleCommand(seek, context + QStringLiteral(" seek"))) return;
                jam2::EngineCommand play;
                play.type = jam2::EngineCommandType::PreparedPlay;
                play.frame = target;
                if (!submitConsoleCommand(play, context + QStringLiteral(" play"))) return;
                if (!trackSyncEnabled_) return;
                engineCommand.type = jam2::EngineCommandType::ScheduleTransport;
                engineCommand.transport_action = action == QStringLiteral("record-start")
                    ? jam2::EngineTransportAction::RecordStart
                    : jam2::EngineTransportAction::TrackRestart;
                engineCommand.transport_target_frame = target;
                engineCommand.transport_musical_frame = snapshot.metronome_render_offset_frames >= 0
                    ? target + static_cast<std::uint64_t>(snapshot.metronome_render_offset_frames)
                    : target > static_cast<std::uint64_t>(-snapshot.metronome_render_offset_frames)
                        ? target - static_cast<std::uint64_t>(-snapshot.metronome_render_offset_frames)
                        : 0ULL;
                engineCommand.transport_countdown_start_frame = target;
                (void)submitConsoleCommand(engineCommand, context);
                return;
            } else if ((action == QStringLiteral("play") || action == QStringLiteral("stop")) && parts.size() <= 3) {
                bool ok = false;
                engineCommand.frame = parseFrameOrNow(parts.value(2), ok);
                if (ok) {
                    engineCommand.type = action == QStringLiteral("play")
                        ? jam2::EngineCommandType::PreparedPlay
                        : jam2::EngineCommandType::PreparedStop;
                    if (!submitConsoleCommand(engineCommand, context)) return;
                    if (!trackSyncEnabled_) return;
                    jam2::EngineCommand transport;
                    transport.type = jam2::EngineCommandType::ScheduleTransport;
                    transport.transport_action = action == QStringLiteral("play")
                        ? jam2::EngineTransportAction::TrackPlay
                        : jam2::EngineTransportAction::TrackStop;
                    transport.transport_target_frame = engineCommand.frame;
                    transport.transport_musical_frame = engineCommand.frame;
                    transport.transport_countdown_start_frame = engineCommand.frame;
                    (void)submitConsoleCommand(transport, context + QStringLiteral(" transport"));
                    return;
                }
            }
        } else if (command == QStringLiteral("record") && parts.size() >= 3 &&
                   parts[1] == QStringLiteral("jam")) {
            if (parts[2] == QStringLiteral("start")) {
                const QString path = line.mid(line.indexOf(QStringLiteral("start")) + 5).trimmed();
                engineCommand.type = jam2::EngineCommandType::StartJamRecording;
                if (!path.isEmpty() && jam2::engine_command_set_text(engineCommand, path.toStdString()) &&
                    submitConsoleCommand(engineCommand, context)) {
                    activeRecordingFolder_ = path;
                    return;
                }
            } else if (parts[2] == QStringLiteral("stop")) {
                engineCommand.type = jam2::EngineCommandType::StopJamRecording;
                (void)submitConsoleCommand(engineCommand, context);
                return;
            }
        } else if (command == QStringLiteral("stats") || command == QStringLiteral("status")) {
            writeConsoleLine(QStringLiteral("Headless runtime: frame=%1 network=%2 peers=%3")
                .arg(snapshot.engine_frame)
                .arg(runtime_.isNetworkRunning() ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(sessionController_.snapshot().remotePeerCount));
            return;
        }
        writeConsoleLine(QStringLiteral("invalid headless command: ") + line, true);
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

    QString reserveCreatorEndpoint() const
    {
        const QString requested = bindText_.isEmpty()
            ? QStringLiteral("0.0.0.0:49000")
            : bindText_;
        const jam2::Endpoint endpoint = jam2::parse_bind_endpoint(requested.toStdString());
        if (endpoint.port != 0) {
            return requested;
        }

        QHostAddress address(QString::fromStdString(endpoint.host));
        if (address.isNull() && endpoint.host == "0.0.0.0") {
            address = QHostAddress::AnyIPv4;
        }
        if (address.isNull()) {
            throw std::runtime_error("create --bind host must be a numeric local address");
        }
        for (int attempt = 0; attempt < 32; ++attempt) {
            QTcpServer tcpReservation;
            if (!tcpReservation.listen(address, 0)) {
                continue;
            }
            const quint16 port = tcpReservation.serverPort();
            QUdpSocket udpReservation;
            if (!udpReservation.bind(address, port)) {
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
                result << QStringLiteral("%1@%2").arg(peerIdForToken(it.key())).arg(endpoint);
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
        const quint64 coordinatorId = peerIdForToken(coordinatorToken_.isEmpty() ? localToken_ : coordinatorToken_);
        const QStringList initialPeerSpecs = peerSpecsExcludingSelf();
        runtimeOptions_.bind = jam2::parse_bind_endpoint(bind.toStdString());
        runtimeOptions_.session_id = session_.session_id;
        runtimeOptions_.session_key = session_.key;
        runtimeOptions_.bootstrap_role = creator_
            ? jam2::SessionBootstrapRole::Creator
            : jam2::SessionBootstrapRole::Joiner;
        runtimeOptions_.local_peer_id = peerIdForToken(localToken_);
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
        options.local_peer_id = peerIdForToken(snapshot.localToken);
        options.bootstrap_coordinator_peer_id = peerIdForToken(snapshot.coordinatorToken);
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
        if (!sessionController_.startCreator(SharedSessionController::CreatorConfig{
                local.port,
                sessionHex_,
                keyHex_,
                localToken_,
                advertisedText,
                sessionPeerLimit_,
                SharedSessionController::SessionContract{
                    1,
                    QStringLiteral("pcm24-mono"),
                    requestedContract_.profile,
                    requestedContract_.sampleRate,
                    requestedContract_.frameSize,
                }})) {
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
            .arg(QString::number(peerIdForToken(token)))
            .arg(std::max(0, static_cast<int>(peers_.size()) - 1)));
        emitStartup(QStringLiteral("connected"));
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
        if (event.type == TransportEventType::Failure && !event.retryable) {
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
        sessionController_.startJoiner(SharedSessionController::JoinerConfig{
            QString::fromStdString(session_.endpoint.host),
            session_.endpoint.port,
            sessionHex_,
            keyHex_,
            localToken_,
            localEndpointText_,
            SharedSessionController::SessionContract{
                1,
                QStringLiteral("pcm24-mono"),
                requestedContract_.profile,
                requestedContract_.sampleRate,
                requestedContract_.frameSize,
            },
            true,
        });
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
    }

    void finish(int code)
    {
        if (finishing_) {
            return;
        }
        finishing_ = true;
        exitOverride_ = code;
        sessionController_.close();
        if (runtime_.isNetworkRunning()) {
            runtime_.stopNetwork();
        } else {
            QCoreApplication::exit(code);
        }
    }

    void handleRuntimeFinished(int code)
    {
        sessionController_.close();
        QCoreApplication::exit(exitOverride_.value_or(code));
    }

    bool creator_ = false;
    bool noStun_ = false;
    bool networkStarted_ = false;
    bool runtimeStarted_ = false;
    bool controlStarted_ = false;
    bool joinedStartupEmitted_ = false;
    bool hadAuthenticatedPeer_ = false;
    bool finishing_ = false;
    int waitMs_ = 0;
    int sessionPeerLimit_ = 0;
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
    bool trackSyncEnabled_ = true;
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
        QTimer::singleShot(0, &controller, [&controller] { controller.start(); });
        return QCoreApplication::exec();
    } catch (const std::exception& error) {
        writeConsoleLine(QString::fromUtf8(error.what()), true);
        return 2;
    }
}
