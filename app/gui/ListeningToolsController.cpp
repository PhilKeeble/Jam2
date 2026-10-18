#include "ListeningToolsController.hpp"

#include <QFile>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace jam2::gui {
namespace {
bool integer(const QJsonValue& value, int low, int high) {
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number && number >= low && number <= high;
}
bool finite(const QJsonValue& value, double low, double high) {
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number) && number >= low && number <= high;
}
bool frame(const QJsonValue& value, std::uint64_t& result) {
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 20) return false;
    for (QChar ch : value.toString()) if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    bool ok = false;
    result = value.toString().toULongLong(&ok);
    return ok;
}
bool fields(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) if (!allowed.contains(it.key())) return false;
    return true;
}
QString type(const char* suffix) { return QStringLiteral("jam.listening.") + QString::fromLatin1(suffix); }
QJsonObject tempoJson(const jam2::PlaybackTempo& tempo) {
    return {{QStringLiteral("rate_ppm"), tempo.rate_ppm},
        {QStringLiteral("anchor_frame"), QString::number(tempo.anchor_frame)},
        {QStringLiteral("anchor_position"), tempo.anchor_position}};
}
bool parseTempo(const QJsonValue& value, jam2::PlaybackTempo& tempo) {
    const auto object = value.toObject();
    if (!value.isObject() || !fields(object, {QStringLiteral("rate_ppm"), QStringLiteral("anchor_frame"), QStringLiteral("anchor_position")}) ||
        !integer(object.value(QStringLiteral("rate_ppm")), 100000, 2000000) ||
        !frame(object.value(QStringLiteral("anchor_frame")), tempo.anchor_frame) ||
        !finite(object.value(QStringLiteral("anchor_position")), 0.0, 9007199254740991.0)) return false;
    tempo.rate_ppm = object.value(QStringLiteral("rate_ppm")).toInt();
    tempo.anchor_position = object.value(QStringLiteral("anchor_position")).toDouble();
    return true;
}
int nextRevision(int revision) { return revision == (std::numeric_limits<int>::max)() ? 1 : revision + 1; }
jam2::PlaybackTempo tempoAtTarget(const ListeningToolsController::Context& state, std::uint64_t target) {
    const auto raw = jam2::transport_raw_frame_from_musical(target, state.position.renderOffsetFrames);
    return state.engine.listening_commit_raw_frame > state.engine.engine_frame &&
        raw >= state.engine.listening_commit_raw_frame ? state.engine.pending_playback_tempo : state.position.tempo;
}
}

ListeningToolsSettings ListeningToolsSettings::fromModel(const SharedTrackModel& model) {
    return {qBound(100000, qRound(model.speed * 1000000.0), 2000000), model.pitchCents,
        model.focusEnabled, model.focusPreset, model.focusFrequencyHz, model.focusGainDb, model.focusQ};
}
void ListeningToolsSettings::applyTo(SharedTrackModel& model) const {
    model.speed = speedPpm / 1000000.0;
    model.pitchCents = pitchCents;
    model.focusEnabled = focusEnabled;
    model.focusPreset = focusPreset;
    model.focusFrequencyHz = focusHz;
    model.focusGainDb = focusGainDb;
    model.focusQ = focusQ;
}
QJsonObject listeningSettingsJson(const ListeningToolsSettings& settings) {
    return {{QStringLiteral("speed_ppm"), settings.speedPpm}, {QStringLiteral("pitch_cents"), settings.pitchCents},
        {QStringLiteral("focus_enabled"), settings.focusEnabled}, {QStringLiteral("focus_preset"), settings.focusPreset},
        {QStringLiteral("focus_hz"), settings.focusHz}, {QStringLiteral("focus_gain_db"), settings.focusGainDb},
        {QStringLiteral("focus_q"), settings.focusQ}};
}
bool parseListeningSettings(const QJsonObject& object, ListeningToolsSettings& settings, QString& error) {
    error.clear();
    if (!fields(object, {QStringLiteral("speed_ppm"), QStringLiteral("pitch_cents"), QStringLiteral("focus_enabled"),
            QStringLiteral("focus_preset"), QStringLiteral("focus_hz"), QStringLiteral("focus_gain_db"), QStringLiteral("focus_q")}) ||
        !integer(object.value(QStringLiteral("speed_ppm")), 100000, 2000000) ||
        !integer(object.value(QStringLiteral("pitch_cents")), -1200, 1200) ||
        !object.value(QStringLiteral("focus_enabled")).isBool() ||
        !object.value(QStringLiteral("focus_preset")).isString() || object.value(QStringLiteral("focus_preset")).toString().size() > 128 ||
        !finite(object.value(QStringLiteral("focus_hz")), 20.0, 20000.0) ||
        !finite(object.value(QStringLiteral("focus_gain_db")), -24.0, 24.0) ||
        !finite(object.value(QStringLiteral("focus_q")), 0.1, 20.0)) {
        error = QStringLiteral("listening-tool settings are invalid"); return false;
    }
    settings = {object.value(QStringLiteral("speed_ppm")).toInt(), object.value(QStringLiteral("pitch_cents")).toInt(),
        object.value(QStringLiteral("focus_enabled")).toBool(), object.value(QStringLiteral("focus_preset")).toString(),
        object.value(QStringLiteral("focus_hz")).toDouble(), object.value(QStringLiteral("focus_gain_db")).toDouble(),
        object.value(QStringLiteral("focus_q")).toDouble()};
    return true;
}
bool validateListeningMessage(const QJsonObject& message, QString& error) {
    error.clear();
    const QString messageType = message.value(QStringLiteral("type")).toString();
    QStringList allowed{QStringLiteral("type"), QStringLiteral("revision")};
    const bool requestMessage = messageType == type("request");
    const bool prepareMessage = messageType == type("prepare");
    const bool commitMessage = messageType == type("commit");
    const bool stateMessage = messageType == type("state");
    const bool resetMessage = messageType == type("reset");
    const bool settingsMessage = requestMessage || prepareMessage || stateMessage;
    const bool timingMessage = commitMessage || stateMessage || resetMessage;
    if (settingsMessage) allowed << QStringLiteral("settings");
    if (prepareMessage || commitMessage || stateMessage) allowed << QStringLiteral("bank");
    if (timingMessage) allowed << QStringLiteral("tempo") << QStringLiteral("target_frame") << QStringLiteral("epoch_frame") << QStringLiteral("epoch_position");
    if (commitMessage || stateMessage) allowed << QStringLiteral("source_seconds") << QStringLiteral("playing");
    if (messageType == type("cancel") || messageType == type("failed")) allowed << QStringLiteral("reason");
    if ((!requestMessage && !prepareMessage && !commitMessage && !stateMessage && !resetMessage &&
            messageType != type("ready") && messageType != type("cancel") && messageType != type("failed")) ||
        !fields(message, allowed) || (!requestMessage && !integer(message.value(QStringLiteral("revision")), 1, (std::numeric_limits<int>::max)())) ||
        (requestMessage && message.contains(QStringLiteral("revision")))) {
        error = QStringLiteral("listening message type, revision, or fields are invalid"); return false;
    }
    ListeningToolsSettings settings;
    if (settingsMessage && (!message.value(QStringLiteral("settings")).isObject() ||
            !parseListeningSettings(message.value(QStringLiteral("settings")).toObject(), settings, error))) return false;
    if (prepareMessage || commitMessage || stateMessage) {
        if (!integer(message.value(QStringLiteral("bank")), 0, 11)) { error = QStringLiteral("listening bank is invalid"); return false; }
    }
    if (timingMessage) {
        jam2::PlaybackTempo tempo;
        std::uint64_t target = 0;
        if (!parseTempo(message.value(QStringLiteral("tempo")), tempo) || !frame(message.value(QStringLiteral("target_frame")), target) ||
            (resetMessage && tempo.rate_ppm != 1000000)) { error = QStringLiteral("listening timing is invalid"); return false; }
    }
    if (timingMessage && (message.contains(QStringLiteral("epoch_frame")) || message.contains(QStringLiteral("epoch_position")))) {
        std::uint64_t epoch = 0;
        if (!frame(message.value(QStringLiteral("epoch_frame")), epoch) ||
            !finite(message.value(QStringLiteral("epoch_position")), 0.0, 9007199254740991.0)) {
            error = QStringLiteral("listening grid epoch is invalid"); return false;
        }
    }
    if ((commitMessage || stateMessage) && (!finite(message.value(QStringLiteral("source_seconds")), 0.0, 3600.0) ||
        !message.value(QStringLiteral("playing")).isBool())) { error = QStringLiteral("listening source position is invalid"); return false; }
    if (message.contains(QStringLiteral("reason")) && (!message.value(QStringLiteral("reason")).isString() ||
        message.value(QStringLiteral("reason")).toString().size() > 512)) { error = QStringLiteral("listening error is invalid"); return false; }
    return true;
}

ListeningToolsController::ListeningToolsController(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, [this] { request(); });
    poll_.setInterval(40);
    connect(&poll_, &QTimer::timeout, this, [this] { tick(); });
    poll_.start();
}
ListeningToolsController::~ListeningToolsController() {
    if (prepared_ && !prepared_->path.isEmpty()) QFile::remove(prepared_->path);
}
void ListeningToolsController::setCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }
ListeningToolsController::Context ListeningToolsController::context() const { return callbacks_.context ? callbacks_.context() : Context{}; }
bool ListeningToolsController::send(QJsonObject message, const QString& peer) { return callbacks_.send && callbacks_.send(message, peer); }
void ListeningToolsController::initialize(const ListeningToolsSettings& settings) {
    draft_ = applied_ = settings;
    revision_ = appliedRevision_ = 1;
}
void ListeningToolsController::edit(ListeningToolsSettings settings, bool commitNow) {
    const Context state = context();
    if (state.protectedRecording) {
        error_ = QStringLiteral("listening tools are unavailable while recording");
        if (callbacks_.rejected) callbacks_.rejected(applied_, error_);
        return;
    }
    draft_ = std::move(settings);
    if (callbacks_.selected) callbacks_.selected(draft_);
    if (commitNow) { debounce_.stop(); request(); } else debounce_.start();
}
void ListeningToolsController::flushEdits() {
    if (debounce_.isActive()) { debounce_.stop(); request(); }
}
void ListeningToolsController::request() {
    const Context state = context();
    if (!pending() && !queuedEdit_ && draft_ == applied_ &&
        (!state.sync || applied_.speedPpm == state.engine.playback_tempo.rate_ppm)) return;
    if (state.protectedRecording || state.transportChanging) { queuedEdit_ = true; return; }
    if (phase_ == Phase::Scheduled || pendingReset_) { queuedEdit_ = true; return; }
    if (state.sync && state.network && !state.creator) {
        if (!send({{QStringLiteral("type"), type("request")}, {QStringLiteral("settings"), listeningSettingsJson(draft_)}})) {
            error_ = QStringLiteral("listening request could not be sent");
            draft_ = applied_;
            if (callbacks_.rejected) callbacks_.rejected(applied_, error_);
        }
        return;
    }
    if (pending()) cancel({}, state.creator && shared_, false);
    begin(nextRevision(revision_), draft_, state.bank, state.sync && state.network && !state.peers.isEmpty());
}
void ListeningToolsController::begin(int revision, ListeningToolsSettings settings, int bank, bool shared) {
    const Context state = context();
    if (state.protectedRecording || state.transportChanging || bank != state.bank) { error_ = QStringLiteral("listening change is unavailable for the current transport"); return; }
    revision_ = revision;
    prepareGeneration_ = nextRevision(prepareGeneration_);
    bank_ = bank;
    preparing_ = std::move(settings);
    if (!queuedEdit_) draft_ = preparing_;
    if (callbacks_.selected) callbacks_.selected(draft_);
    shared_ = shared;
    coupled_ = state.sync;
    sourceSignature_ = state.sourceSignature;
    expected_ = shared && state.creator ? state.peers : QSet<QString>{};
    ready_.clear();
    prepared_.reset();
    error_.clear();
    preparationTime_.start();
    phase_ = Phase::Preparing;
    if (shared && state.creator && !send({{QStringLiteral("type"), type("prepare")},
            {QStringLiteral("revision"), revision_}, {QStringLiteral("bank"), bank_},
            {QStringLiteral("settings"), listeningSettingsJson(preparing_)}})) {
        cancel(QStringLiteral("listening preparation could not be published"), true); return;
    }
    if (!state.hasSources) { ready(); return; }
    if (!callbacks_.prepare || !callbacks_.prepare(preparing_, prepareGeneration_, bank_)) {
        cancel(QStringLiteral("listening preparation could not start"), shared && state.creator);
    }
}
void ListeningToolsController::prepared(int revision, PreparedMixResult result) {
    if (revision != prepareGeneration_ || phase_ != Phase::Preparing) { if (!result.path.isEmpty()) QFile::remove(result.path); return; }
    if (!result.error.isEmpty() || result.frames <= 0 || result.sampleRate <= 0 || result.path.isEmpty()) {
        if (!result.path.isEmpty()) QFile::remove(result.path);
        cancel(result.error.isEmpty() ? QStringLiteral("listening render produced no audio") : result.error, context().creator && shared_); return;
    }
    renderMs_ = result.renderMs;
    prepared_ = std::move(result);
    if (!context().engineRunning) { ready(); return; }
    stageCookie_ = (1ULL << 60) | ++stageSerial_;
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StageListeningTrack;
    command.cookie = stageCookie_;
    if (!jam2::engine_command_set_text(command, prepared_->path.toStdString())) {
        cancel(QStringLiteral("listening source path is too long"), context().creator && shared_); return;
    }
    phase_ = Phase::Loading;
    if (!callbacks_.submit || !callbacks_.submit(command)) cancel(QStringLiteral("listening stage command was rejected"), context().creator && shared_);
}
bool ListeningToolsController::engineEvent(const jam2::EngineEvent& event) {
    if (stageCookie_ == 0 || event.cookie != stageCookie_) return false;
    if (!event.ok || event.type == jam2::EngineEventType::CommandRejected) {
        // A rejected future command did not commit, even if its intended
        // boundary has elapsed by the time the GUI receives the event.
        if (phase_ == Phase::Scheduled) phase_ = Phase::Ready;
        cancel(QString::fromUtf8(jam2::engine_event_text(event).data(), static_cast<int>(jam2::engine_event_text(event).size())), context().creator && shared_);
    } else if (event.type == jam2::EngineEventType::ListeningTrackStaged && phase_ == Phase::Loading) ready();
    return true;
}
void ListeningToolsController::ready() {
    phase_ = Phase::Ready;
    const Context state = context();
    if (shared_ && !state.creator) {
        if (deferredState_) {
            const auto saved = *deferredState_;
            deferredState_.reset();
            if (!schedule(saved)) cancel(QStringLiteral("joined listening state could not be scheduled"), false);
        } else if (!send({{QStringLiteral("type"), type("ready")}, {QStringLiteral("revision"), revision_}})) {
            cancel(QStringLiteral("listening readiness could not be sent"), false);
        }
    } else maybeCommit();
}
void ListeningToolsController::maybeCommit() {
    if (phase_ != Phase::Ready || (!expected_.isEmpty() && ready_ != expected_)) return;
    const Context state = context();
    if (state.transportChanging || state.protectedRecording || (shared_ && !state.creator)) return;
    const int sampleRate = state.position.sampleRate > 0 ? state.position.sampleRate : 48000;
    const auto lead = static_cast<std::uint64_t>(sampleRate) * kCommitLeadMs / 1000ULL;
    std::uint64_t target = state.position.currentFrame + lead;
    const auto previousTempo = tempoAtTarget(state, target);
    if (state.position.engineAnchored && state.position.secondsPerBeat > 0.0) {
        const double source = previousTempo.position(target);
        const double beatFrames = state.position.secondsPerBeat * state.position.tempo.rate() * sampleRate;
        const double nextBeat = std::ceil(std::max(0.0, source - state.position.epochSourcePosition) / beatFrames);
        target = previousTempo.frame(state.position.epochSourcePosition + nextBeat * beatFrames);
    }
    const bool couple = state.sync;
    const jam2::PlaybackTempo tempo{couple ? preparing_.speedPpm : 1000000,
        target, previousTempo.position(target)};
    const double secondsAhead = target >= state.position.currentFrame ?
        static_cast<double>(target - state.position.currentFrame) / sampleRate : 0.0;
    double sourceSeconds = std::max(0.0, state.sourceSeconds + (state.playing ? secondsAhead * applied_.speedPpm / 1000000.0 : 0.0));
    if (prepared_ && prepared_->frames > 0) {
        const double sourceLength = prepared_->frames * preparing_.speedPpm / 1000000.0 / prepared_->sampleRate;
        const double start = std::max(0.0, state.loopStartSourceSeconds);
        const double end = state.loopEndSourceSeconds > start ? state.loopEndSourceSeconds : sourceLength;
        if (state.loopEnabled && end > start) {
            if (sourceSeconds >= end)
                sourceSeconds = start + std::fmod(sourceSeconds - start, end - start);
        } else sourceSeconds = std::min(sourceSeconds, sourceLength);
    }
    QJsonObject message{{QStringLiteral("type"), type("commit")}, {QStringLiteral("revision"), revision_},
        {QStringLiteral("bank"), bank_}, {QStringLiteral("tempo"), tempoJson(tempo)},
        {QStringLiteral("target_frame"), QString::number(target)},
        {QStringLiteral("epoch_frame"), QString::number(state.position.epochFrame)},
        {QStringLiteral("epoch_position"), state.position.epochSourcePosition}, {QStringLiteral("source_seconds"), sourceSeconds},
        {QStringLiteral("playing"), state.playing}};
    if (shared_ && !send(message)) { cancel(QStringLiteral("listening commit could not be published"), true); return; }
    if (!schedule(message)) cancel(QStringLiteral("listening commit could not be scheduled"), shared_);
}
bool ListeningToolsController::schedule(const QJsonObject& message) {
    const Context state = context();
    jam2::PlaybackTempo tempo;
    std::uint64_t target = 0;
    if (!parseTempo(message.value(QStringLiteral("tempo")), tempo) || !frame(message.value(QStringLiteral("target_frame")), target)) return false;
    const bool joining = message.value(QStringLiteral("type")).toString() == type("state");
    if (joining) target = state.position.currentFrame + static_cast<std::uint64_t>(std::max(1, state.position.sampleRate)) * kCommitLeadMs / 1000ULL;
    const std::uint64_t rawTarget = jam2::transport_raw_frame_from_musical(target, state.position.renderOffsetFrames);
    if (state.engineRunning && (rawTarget <= state.engine.engine_frame ||
        rawTarget - state.engine.engine_frame > static_cast<std::uint64_t>(std::max(1, state.position.sampleRate)) * 10ULL)) return false;
    if (stageCookie_ == 0) stageCookie_ = (1ULL << 60) | ++stageSerial_;
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::CommitListeningTools;
    command.cookie = stageCookie_;
    command.frame = rawTarget;
    command.playback_tempo = tempo;
    if (message.contains(QStringLiteral("epoch_position"))) {
        frame(message.value(QStringLiteral("epoch_frame")), command.grid_epoch_frame);
        command.grid_epoch_source_position = message.value(QStringLiteral("epoch_position")).toDouble();
    }
    command.update_playback_tempo = coupled_;
    if (prepared_) {
        command.signed_value = static_cast<std::int64_t>(stageCookie_);
        double sourceSeconds = message.value(QStringLiteral("source_seconds")).toDouble();
        if (joining && state.playing) {
            std::uint64_t sentFrame = 0;
            frame(message.value(QStringLiteral("target_frame")), sentFrame);
            if (target >= sentFrame) sourceSeconds += static_cast<double>(target - sentFrame) /
                std::max(1, state.position.sampleRate) * preparing_.speedPpm / 1000000.0;
        }
        const double outputScale = prepared_->sampleRate / (preparing_.speedPpm / 1000000.0);
        const double timelineFrames = state.loopEnabled && state.loopEndSourceSeconds > 0.0
            ? state.loopEndSourceSeconds * outputScale : static_cast<double>(prepared_->frames);
        command.frame_end = static_cast<std::uint64_t>(std::max(0.0, sourceSeconds * outputScale)) %
            static_cast<std::uint64_t>(std::max(1.0, timelineFrames));
        command.prepared_loop_enabled = state.loopEnabled;
        command.prepared_loop_start_frame = static_cast<std::uint64_t>(std::clamp(state.loopStartSourceSeconds * outputScale,
            0.0, std::max(0.0, timelineFrames - 1.0)));
        command.prepared_loop_end_frame = state.loopEndSourceSeconds > state.loopStartSourceSeconds ?
            static_cast<std::uint64_t>(std::clamp(state.loopEndSourceSeconds * outputScale,
                static_cast<double>(command.prepared_loop_start_frame + 1),
                static_cast<double>(prepared_->sampleRate) * 3600.0)) : static_cast<std::uint64_t>(prepared_->frames);
    }
    if (state.engineRunning && (!callbacks_.submit || !callbacks_.submit(command))) return false;
    committedTempo_ = tempo;
    commitRawFrame_ = state.engineRunning ? rawTarget : 0;
    phase_ = Phase::Scheduled;
    tick();
    return true;
}
void ListeningToolsController::cancel(const QString& error, bool broadcast, bool reportFailure) {
    const Context state = context();
    if (phase_ == Phase::Scheduled && (!state.engineRunning || state.engine.engine_frame >= commitRawFrame_)) {
        queuedEdit_ = false;
        tick();
        return;
    }
    if (broadcast && shared_) send({{QStringLiteral("type"), type("cancel")}, {QStringLiteral("revision"), revision_}, {QStringLiteral("reason"), error.left(512)}});
    else if (shared_ && !state.creator && !error.isEmpty() && reportFailure) send({{QStringLiteral("type"), type("failed")},
        {QStringLiteral("revision"), revision_}, {QStringLiteral("reason"), error.left(512)}});
    jam2::EngineCommand cancel;
    cancel.type = jam2::EngineCommandType::CancelListeningTools;
    cancel.cookie = stageCookie_;
    if (state.engineRunning && callbacks_.submit) callbacks_.submit(cancel);
    if (prepared_ && !prepared_->path.isEmpty()) QFile::remove(prepared_->path);
    prepared_.reset();
    deferredState_.reset();
    phase_ = Phase::Idle;
    commitRawFrame_ = 0;
    stageCookie_ = 0;
    error_ = error;
    if (!error.isEmpty()) draft_ = applied_;
    if (!error.isEmpty() && callbacks_.rejected) callbacks_.rejected(applied_, error);
    if (!error.isEmpty() && callbacks_.log) callbacks_.log(QStringLiteral("listening change cancelled: ") + error);
}
QJsonObject ListeningToolsController::resetMessage(const Context& state) const {
    const int sampleRate = std::max(1, state.position.sampleRate);
    const auto target = state.position.currentFrame + static_cast<std::uint64_t>(sampleRate) * kCommitLeadMs / 1000ULL;
    return {{QStringLiteral("type"), type("reset")}, {QStringLiteral("revision"), nextRevision(revision_)},
        {QStringLiteral("target_frame"), QString::number(target)},
        {QStringLiteral("epoch_frame"), QString::number(state.position.epochFrame)},
        {QStringLiteral("epoch_position"), state.position.epochSourcePosition},
        {QStringLiteral("tempo"), tempoJson({1000000, target, tempoAtTarget(state, target).position(target)})}};
}
void ListeningToolsController::applyReset(const QJsonObject& message) {
    if (pending()) cancel({}, false);
    revision_ = message.value(QStringLiteral("revision")).toInt();
    jam2::PlaybackTempo tempo;
    std::uint64_t target = 0;
    if (!parseTempo(message.value(QStringLiteral("tempo")), tempo) || !frame(message.value(QStringLiteral("target_frame")), target)) return;
    const Context state = context();
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::CommitListeningTools;
    command.playback_tempo = tempo;
    if (message.contains(QStringLiteral("epoch_position"))) {
        frame(message.value(QStringLiteral("epoch_frame")), command.grid_epoch_frame);
        command.grid_epoch_source_position = message.value(QStringLiteral("epoch_position")).toDouble();
    }
    command.frame = jam2::transport_raw_frame_from_musical(target, state.position.renderOffsetFrames);
    if (command.frame <= state.engine.engine_frame) command.frame = state.engine.engine_frame + 1;
    pendingReset_.reset();
    if (state.engineRunning && (!callbacks_.submit || !callbacks_.submit(command))) {
        pendingReset_ = command;
        error_ = QStringLiteral("metronome clock reset is waiting for the engine command queue");
        if (callbacks_.log) callbacks_.log(error_);
    }
    committedTempo_ = tempo;
}
void ListeningToolsController::policyChanged(bool previouslySynced) {
    const Context state = context();
    if (previouslySynced && !state.sync && (!state.network || state.creator)) {
        const auto message = resetMessage(state);
        if (state.network) send(message);
        applyReset(message);
    } else if (!previouslySynced && state.sync && (!state.network || state.creator)) request();
    else if (!state.sync && pending()) cancel({}, false);
}
void ListeningToolsController::receive(const QJsonObject& message, const QString& source) {
    QString error;
    if (!validateListeningMessage(message, error)) { error_ = error; return; }
    const Context state = context();
    const QString messageType = message.value(QStringLiteral("type")).toString();
    const int revision = message.value(QStringLiteral("revision")).toInt();
    if (messageType == type("reset")) {
        if (!state.creator && revision >= highestAuthoritativeRevision_) {
            highestAuthoritativeRevision_ = revision;
            applyReset(message);
        }
        return;
    }
    if (!state.sync || state.protectedRecording) return;
    if (messageType == type("request") && state.creator && state.peers.contains(source)) {
        parseListeningSettings(message.value(QStringLiteral("settings")).toObject(), draft_, error);
        request();
    } else if (messageType == type("prepare") && !state.creator && revision > appliedSharedRevision_ && revision >= highestAuthoritativeRevision_) {
        if (revision == revision_ && pending()) return;
        highestAuthoritativeRevision_ = revision;
        if (pending()) cancel({}, false);
        deferredState_.reset();
        if (!state.contentReady) { deferredState_ = message; return; }
        ListeningToolsSettings settings;
        parseListeningSettings(message.value(QStringLiteral("settings")).toObject(), settings, error);
        begin(revision, settings, message.value(QStringLiteral("bank")).toInt(), true);
    } else if (messageType == type("ready") && state.creator && pending() && revision == revision_ && expected_.contains(source)) {
        ready_.insert(source); maybeCommit();
    } else if (messageType == type("failed") && state.creator && pending() && revision == revision_ && expected_.contains(source)) {
        cancel(message.value(QStringLiteral("reason")).toString(QStringLiteral("a peer could not prepare listening audio")), true);
    } else if (messageType == type("cancel") && !state.creator &&
        ((revision == revision_ && pending()) ||
         (deferredState_ && deferredState_->value(QStringLiteral("revision")).toInt() == revision)))
        cancel(message.value(QStringLiteral("reason")).toString(), false, false);
    else if (messageType == type("commit") && !state.creator && revision == revision_ && phase_ == Phase::Ready) {
        if (!schedule(message)) cancel(QStringLiteral("shared listening commit arrived too late"), false);
    } else if (messageType == type("state") && !state.creator && revision >= appliedSharedRevision_ && revision >= highestAuthoritativeRevision_) {
        if (!state.engineRunning || !state.position.engineAnchored || state.transportChanging || !state.contentReady) { deferredState_ = message; return; }
        highestAuthoritativeRevision_ = revision;
        if (pending()) cancel({}, false);
        ListeningToolsSettings settings;
        parseListeningSettings(message.value(QStringLiteral("settings")).toObject(), settings, error);
        deferredState_ = message;
        begin(revision, settings, state.bank, true);
    }
}
void ListeningToolsController::sendCurrent(const QString& peer) {
    tick();
    const Context state = context();
    if (!state.creator) return;
    if (!state.sync) {
        // A unity-rate clock can still have an offset from earlier slowing.
        // Share its phase without publishing anybody's local listening tools.
        auto message = resetMessage(state);
        if (pendingReset_) message.insert(QStringLiteral("tempo"), tempoJson(committedTempo_));
        send(message, peer);
        return;
    }
    if (pending()) cancel(QStringLiteral("jam membership changed during listening preparation"), true);
    send({{QStringLiteral("type"), type("state")}, {QStringLiteral("revision"), appliedRevision_},
        {QStringLiteral("bank"), state.bank}, {QStringLiteral("settings"), listeningSettingsJson(applied_)},
        {QStringLiteral("tempo"), tempoJson(state.engine.playback_tempo)},
        {QStringLiteral("target_frame"), QString::number(state.position.currentFrame)},
        {QStringLiteral("epoch_frame"), QString::number(state.position.epochFrame)},
        {QStringLiteral("epoch_position"), state.position.epochSourcePosition},
        {QStringLiteral("source_seconds"), state.sourceSeconds}, {QStringLiteral("playing"), state.playing}}, peer);
}
void ListeningToolsController::tick() {
    if (!callbacks_.context) return;
    const Context state = context();
    if (pendingReset_ && state.engineRunning && callbacks_.submit) {
        pendingReset_->frame = std::max(pendingReset_->frame, state.engine.engine_frame + 1);
        if (callbacks_.submit(*pendingReset_)) {
            pendingReset_.reset();
            return; // Let the engine publish the reset before another edit.
        }
    }
    if (phase_ == Phase::Idle && deferredState_ && state.engineRunning && state.position.engineAnchored && state.contentReady && !state.protectedRecording && !state.transportChanging) {
        const auto message = *deferredState_;
        deferredState_.reset();
        receive(message, {});
        return;
    }
    if (phase_ == Phase::Scheduled && state.engineRunning && state.engine.engine_frame < commitRawFrame_ &&
        (state.bank != bank_ || state.protectedRecording || state.sourceSignature != sourceSignature_)) {
        cancel(QStringLiteral("track content or transport changed before listening commit"), shared_ && state.creator);
        return;
    }
    if (phase_ == Phase::Scheduled && (!state.engineRunning || state.engine.engine_frame >= commitRawFrame_)) {
        if (state.engineRunning && prepared_ && state.engine.listening_source_cookie != stageCookie_) {
            // The callback may not have published the source identity yet.
            // Never declare an audio change applied from its tempo alone.
            if (state.engine.engine_frame <= commitRawFrame_ + static_cast<std::uint64_t>(std::max(1, state.position.sampleRate)) * kAudioPublicationGraceMs / 1000) return;
            phase_ = Phase::Ready;
            cancel(QStringLiteral("listening audio swap was not applied by the engine"), shared_ && state.creator);
            return;
        }
        if (state.engineRunning && coupled_ && (state.engine.playback_tempo.rate_ppm != committedTempo_.rate_ppm ||
            state.engine.playback_tempo.anchor_frame != committedTempo_.anchor_frame ||
            state.engine.playback_tempo.anchor_position != committedTempo_.anchor_position)) {
            phase_ = Phase::Ready;
            cancel(QStringLiteral("listening change was not applied by the engine"), shared_ && state.creator);
            return;
        }
        applied_ = preparing_;
        appliedRevision_ = revision_;
        if (shared_) appliedSharedRevision_ = revision_;
        if (!queuedEdit_) draft_ = applied_;
        phase_ = Phase::Idle;
        if (callbacks_.applied) callbacks_.applied(applied_, prepared_);
        if (callbacks_.selected) callbacks_.selected(draft_);
        prepared_.reset();
        stageCookie_ = 0;
        if (callbacks_.log) callbacks_.log(QStringLiteral("listening revision=%1 speed_ppm=%2 musical_rate_ppm=%3 commit_raw_frame=%4 render_ms=%5")
            .arg(appliedRevision_).arg(applied_.speedPpm).arg(committedTempo_.rate_ppm).arg(commitRawFrame_).arg(renderMs_));
        if (queuedEdit_) { queuedEdit_ = false; request(); }
        return;
    }
    if (pending() && phase_ != Phase::Scheduled) {
        if (state.bank != bank_ || state.protectedRecording || state.sourceSignature != sourceSignature_) {
            cancel(QStringLiteral("track content or transport changed during preparation"), shared_ && state.creator);
        } else if (preparationTime_.elapsed() > kPreparationTimeoutMs) {
            cancel(QStringLiteral("listening preparation timed out"), shared_ && state.creator);
        } else if (shared_ && state.creator && state.peers != expected_) {
            cancel(QStringLiteral("jam membership changed during preparation"), true);
        }
    }
    if (phase_ == Phase::Ready) maybeCommit();
    if (phase_ == Phase::Idle && queuedEdit_ && !pendingReset_ && !state.transportChanging && !state.protectedRecording) {
        queuedEdit_ = false;
        request();
        return;
    }
    if (phase_ == Phase::Idle && !pendingReset_ && state.engineRunning && state.sync && (!state.network || state.creator) &&
        !state.protectedRecording && !state.transportChanging && !debounce_.isActive() &&
        applied_.speedPpm != state.engine.playback_tempo.rate_ppm) request();
}
void ListeningToolsController::reset() {
    debounce_.stop();
    if (pending()) cancel({}, false);
    queuedEdit_ = false;
    deferredState_.reset();
    pendingReset_.reset();
    initialize(ListeningToolsSettings{});
    appliedSharedRevision_ = 0;
    highestAuthoritativeRevision_ = 0;
    committedTempo_ = {};
}
QJsonObject ListeningToolsController::stats() const {
    const Context state = context();
    return {{QStringLiteral("sync"), state.sync}, {QStringLiteral("phase"), static_cast<int>(phase_)},
        {QStringLiteral("reset_pending"), pendingReset_.has_value()},
        {QStringLiteral("playback_ready"), playbackReady()},
        {QStringLiteral("audio_commit_cookie"), QString::number(state.engine.listening_source_cookie)},
        {QStringLiteral("audio_queue_depth"), static_cast<qint64>(state.engine.listening_source_queue_depth)},
        {QStringLiteral("audio_queue_capacity"), static_cast<qint64>(state.engine.listening_source_queue_capacity)},
        {QStringLiteral("audio_publication_grace_ms"), kAudioPublicationGraceMs},
        {QStringLiteral("requested_revision"), revision_}, {QStringLiteral("applied_revision"), appliedRevision_},
        {QStringLiteral("requested_speed_ppm"), draft_.speedPpm}, {QStringLiteral("applied_speed_ppm"), applied_.speedPpm},
        {QStringLiteral("effective_bpm"), state.engine.metronome_pattern.bpm * state.engine.playback_tempo.rate()},
        {QStringLiteral("musical_rate_ppm"), state.engine.playback_tempo.rate_ppm},
        {QStringLiteral("musical_position"), state.position.tempo.position(state.position.currentFrame)},
        {QStringLiteral("tempo_anchor_frame"), QString::number(state.engine.playback_tempo.anchor_frame)},
        {QStringLiteral("tempo_anchor_position"), state.engine.playback_tempo.anchor_position},
        {QStringLiteral("source_seconds"), state.sourceSeconds},
        {QStringLiteral("commit_raw_frame"), QString::number(commitRawFrame_)},
        {QStringLiteral("ready_peers"), ready_.size()}, {QStringLiteral("expected_peers"), expected_.size()},
        {QStringLiteral("render_ms"), renderMs_}, {QStringLiteral("debounce_ms"), kDebounceMs},
        {QStringLiteral("commit_lead_ms"), kCommitLeadMs}, {QStringLiteral("preparation_timeout_ms"), kPreparationTimeoutMs},
        {QStringLiteral("settings"), listeningSettingsJson(applied_)}, {QStringLiteral("error"), error_}};
}
} // namespace jam2::gui
