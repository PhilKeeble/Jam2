#include "JamSyncPolicy.hpp"

#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool onlyFields(const QJsonObject& message, const QSet<QString>& allowed)
{
    for (auto it = message.begin(); it != message.end(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

bool exactRevision(const QJsonValue& value, int& revision)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < 1.0 ||
        number > static_cast<double>((std::numeric_limits<int>::max)())) return false;
    revision = static_cast<int>(number);
    return true;
}

bool parseGeneratedIdeas(const QJsonValue& value, GeneratedIdeaSyncMode& mode)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text == QStringLiteral("full")) mode = GeneratedIdeaSyncMode::Full;
    else if (text == QStringLiteral("chords")) mode = GeneratedIdeaSyncMode::Chords;
    else if (text == QStringLiteral("beats")) mode = GeneratedIdeaSyncMode::Beats;
    else if (text == QStringLiteral("off")) mode = GeneratedIdeaSyncMode::Off;
    else return false;
    return true;
}

} // namespace

JamSyncPolicy jam2NormalizeJamSyncPolicy(JamSyncPolicy policy) noexcept
{
    policy.recordings = policy.recordings && policy.trackLanes && policy.globalPlayback;
    return policy;
}

bool jam2JamSyncAllows(const JamSyncPolicy& source, JamSyncRoute route) noexcept
{
    const JamSyncPolicy policy = jam2NormalizeJamSyncPolicy(source);
    switch (route) {
    case JamSyncRoute::TrackLanes:
        return policy.trackLanes;
    case JamSyncRoute::AutomaticWav:
        return policy.trackLanes && policy.autoShareWavs;
    case JamSyncRoute::ManualWav:
        return true;
    case JamSyncRoute::GlobalPlayback:
        return policy.globalPlayback;
    case JamSyncRoute::Recording:
        return policy.recordings;
    case JamSyncRoute::MetronomeState:
        return policy.metronomeState;
    case JamSyncRoute::IdeaFull:
        return policy.generatedIdeas == GeneratedIdeaSyncMode::Full;
    case JamSyncRoute::IdeaChords:
        return policy.generatedIdeas == GeneratedIdeaSyncMode::Full ||
            policy.generatedIdeas == GeneratedIdeaSyncMode::Chords;
    case JamSyncRoute::IdeaBeats:
        return policy.generatedIdeas == GeneratedIdeaSyncMode::Full ||
            policy.generatedIdeas == GeneratedIdeaSyncMode::Beats;
    }
    return false;
}

QString jam2GeneratedIdeaSyncModeText(GeneratedIdeaSyncMode mode)
{
    switch (mode) {
    case GeneratedIdeaSyncMode::Full: return QStringLiteral("full");
    case GeneratedIdeaSyncMode::Chords: return QStringLiteral("chords");
    case GeneratedIdeaSyncMode::Beats: return QStringLiteral("beats");
    case GeneratedIdeaSyncMode::Off: return QStringLiteral("off");
    }
    return {};
}

QJsonObject jam2JamSyncPolicyMessage(const QString& type, const JamSyncPolicy& source)
{
    const JamSyncPolicy policy = jam2NormalizeJamSyncPolicy(source);
    QJsonObject message{
        {QStringLiteral("type"), type},
        {QStringLiteral("track_lanes"), policy.trackLanes},
        {QStringLiteral("auto_share_wavs"), policy.autoShareWavs},
        {QStringLiteral("global_playback"), policy.globalPlayback},
        {QStringLiteral("generated_ideas"), jam2GeneratedIdeaSyncModeText(policy.generatedIdeas)},
        {QStringLiteral("metronome_state"), policy.metronomeState},
        {QStringLiteral("recordings"), policy.recordings},
    };
    if (type == QStringLiteral("jam.sync.set")) {
        message.insert(QStringLiteral("revision"), policy.revision);
    }
    return message;
}

bool jam2ParseJamSyncPolicyMessage(
    const QJsonObject& message,
    JamSyncPolicy& policy,
    QString& error)
{
    error.clear();
    const QString type = message.value(QStringLiteral("type")).toString();
    const bool authoritative = type == QStringLiteral("jam.sync.set");
    if (!authoritative && type != QStringLiteral("jam.sync.request")) {
        error = QStringLiteral("message is not a Jam Sync policy message");
        return false;
    }
    QSet<QString> allowed{
        QStringLiteral("type"), QStringLiteral("track_lanes"),
        QStringLiteral("auto_share_wavs"), QStringLiteral("global_playback"),
        QStringLiteral("generated_ideas"), QStringLiteral("metronome_state"),
        QStringLiteral("recordings")};
    if (authoritative) allowed.insert(QStringLiteral("revision"));
    if (!onlyFields(message, allowed)) {
        error = QStringLiteral("Jam Sync policy contains a field outside the current format");
        return false;
    }
    if (!message.value(QStringLiteral("track_lanes")).isBool() ||
        !message.value(QStringLiteral("auto_share_wavs")).isBool() ||
        !message.value(QStringLiteral("global_playback")).isBool() ||
        !message.value(QStringLiteral("metronome_state")).isBool() ||
        !message.value(QStringLiteral("recordings")).isBool()) {
        error = QStringLiteral("Jam Sync policy boolean field is invalid");
        return false;
    }
    JamSyncPolicy parsed;
    parsed.trackLanes = message.value(QStringLiteral("track_lanes")).toBool();
    parsed.autoShareWavs = message.value(QStringLiteral("auto_share_wavs")).toBool();
    parsed.globalPlayback = message.value(QStringLiteral("global_playback")).toBool();
    parsed.metronomeState = message.value(QStringLiteral("metronome_state")).toBool();
    parsed.recordings = message.value(QStringLiteral("recordings")).toBool();
    if (!parseGeneratedIdeas(message.value(QStringLiteral("generated_ideas")), parsed.generatedIdeas)) {
        error = QStringLiteral("Jam Sync generated idea mode is invalid");
        return false;
    }
    if (authoritative) {
        if (!exactRevision(message.value(QStringLiteral("revision")), parsed.revision)) {
            error = QStringLiteral("authoritative Jam Sync revision is missing or invalid");
            return false;
        }
    } else if (message.contains(QStringLiteral("revision"))) {
        error = QStringLiteral("Jam Sync requests must not assign a revision");
        return false;
    }
    policy = jam2NormalizeJamSyncPolicy(parsed);
    return true;
}

bool jam2JamSyncAllowsControlMessage(
    const JamSyncPolicy& policy,
    const QJsonObject& message) noexcept
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("song.set")) {
        const QString scope = message.value(QStringLiteral("sync_scope"))
            .toString(QStringLiteral("tracks"));
        if (scope == QStringLiteral("tracks")) {
            return jam2JamSyncAllows(policy, JamSyncRoute::TrackLanes);
        }
        if (scope == QStringLiteral("idea.full")) {
            return jam2JamSyncAllows(policy, JamSyncRoute::IdeaFull);
        }
        if (scope == QStringLiteral("idea.chords")) {
            return jam2JamSyncAllows(policy, JamSyncRoute::IdeaChords);
        }
        if (scope == QStringLiteral("idea.beats")) {
            return jam2JamSyncAllows(policy, JamSyncRoute::IdeaBeats);
        }
        return false;
    }
    if (type == QStringLiteral("practice.references.render")) {
        return jam2JamSyncAllows(policy, JamSyncRoute::AutomaticWav);
    }
    if (type == QStringLiteral("looper.recording.state") ||
        type == QStringLiteral("looper.recording.group.start") ||
        type == QStringLiteral("looper.recording.group.finish") ||
        type == QStringLiteral("looper.recording.group.recover.request") ||
        type == QStringLiteral("looper.recording.group.recover")) {
        return jam2JamSyncAllows(policy, JamSyncRoute::Recording);
    }
    if (type == QStringLiteral("looper.recording.resync.request") ||
        type == QStringLiteral("looper.recording.resync.state") ||
        type == QStringLiteral("bank.request") ||
        type == QStringLiteral("bank.prepare") ||
        type == QStringLiteral("bank.ready") ||
        type == QStringLiteral("bank.cancel") ||
        type == QStringLiteral("bank.switch")) {
        return jam2JamSyncAllows(policy, JamSyncRoute::GlobalPlayback);
    }
    if (type == QStringLiteral("jam.metronome.state.request") ||
        type == QStringLiteral("jam.metronome.state.set")) {
        return jam2JamSyncAllows(policy, JamSyncRoute::MetronomeState);
    }
    return true;
}

int jam2NextJamSyncRevision(int current) noexcept
{
    return current >= (std::numeric_limits<int>::max)() ? 1 : std::max(1, current + 1);
}

JamSyncPolicyState::JamSyncPolicyState(JamSyncPolicy policy)
    : policy_(jam2NormalizeJamSyncPolicy(policy))
{
}

const JamSyncPolicy& JamSyncPolicyState::policy() const noexcept
{
    return policy_;
}

void JamSyncPolicyState::prepareCreatorSession() noexcept
{
    policy_.revision = std::max(1, policy_.revision);
}

void JamSyncPolicyState::prepareJoinerSession() noexcept
{
    policy_.revision = -1;
}

JamSyncPolicy JamSyncPolicyState::order(JamSyncPolicy proposal) noexcept
{
    proposal = jam2NormalizeJamSyncPolicy(proposal);
    proposal.revision = jam2NextJamSyncRevision(policy_.revision);
    policy_ = proposal;
    return policy_;
}

bool JamSyncPolicyState::adopt(JamSyncPolicy authoritative) noexcept
{
    if (authoritative.revision < 1 || authoritative.revision <= policy_.revision) return false;
    policy_ = jam2NormalizeJamSyncPolicy(authoritative);
    return true;
}
