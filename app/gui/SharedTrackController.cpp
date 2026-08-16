#include "SharedTrackController.hpp"

#include "../application/ContentLimits.hpp"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr int kMaximumTrackTextCharacters = 512;
constexpr int kMaximumTrackPathCharacters =
    jam2::application::limits::kMaximumLooperPathCharacters;

bool readString(
    const QJsonObject& object,
    QStringView key,
    QString& value,
    int maximumCharacters,
    QString& error)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined()) return true;
    if (!field.isString() || field.toString().size() > maximumCharacters) {
        error = QStringLiteral("invalid track field: %1").arg(key);
        return false;
    }
    value = field.toString();
    return true;
}

bool readInteger(
    const QJsonObject& object,
    QStringView key,
    qint64& value,
    qint64 minimum,
    qint64 maximum,
    QString& error)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined()) return true;
    if (!field.isDouble()) {
        error = QStringLiteral("invalid track field: %1").arg(key);
        return false;
    }
    const qint64 parsed = field.toInteger((std::numeric_limits<qint64>::min)());
    if (parsed < minimum || parsed > maximum) {
        error = QStringLiteral("track field out of range: %1").arg(key);
        return false;
    }
    value = parsed;
    return true;
}

bool readFinite(
    const QJsonObject& object,
    QStringView key,
    double& value,
    double minimum,
    double maximum,
    QString& error)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined()) return true;
    if (!field.isDouble()) {
        error = QStringLiteral("invalid track field: %1").arg(key);
        return false;
    }
    const double parsed = field.toDouble();
    if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        error = QStringLiteral("track field out of range: %1").arg(key);
        return false;
    }
    value = parsed;
    return true;
}

bool readBoolean(
    const QJsonObject& object,
    QStringView key,
    bool& value,
    QString& error)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined()) return true;
    if (!field.isBool()) {
        error = QStringLiteral("invalid track field: %1").arg(key);
        return false;
    }
    value = field.toBool();
    return true;
}

bool validSha256(const QString& value)
{
    if (value.isEmpty()) return true;
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return (code >= '0' && code <= '9') ||
            (code >= 'a' && code <= 'f') ||
            (code >= 'A' && code <= 'F');
    });
}

bool isUnsetLoopPoint(double value) noexcept
{
    return value == -1.0;
}

} // namespace

SharedTrackModel& SharedTrackController::model()
{
    return model_;
}

const SharedTrackModel& SharedTrackController::model() const
{
    return model_;
}

void SharedTrackController::replaceModel(SharedTrackModel model) noexcept
{
    model_ = std::move(model);
}

void SharedTrackController::setLoopEnabled(bool enabled) noexcept
{
    model_.loopEnabled = enabled;
}

bool SharedTrackController::setLoopStartAtMilliseconds(qint64 positionMs) noexcept
{
    if (model_.durationMs <= 0) return false;
    const qint64 bounded = qBound<qint64>(
        0,
        positionMs,
        qMax<qint64>(0, static_cast<qint64>(model_.durationMs) - 1));
    model_.loopStartSeconds = static_cast<double>(bounded) / 1000.0;
    if (model_.loopEndSeconds >= 0.0 &&
        model_.loopEndSeconds <= model_.loopStartSeconds) {
        model_.loopEndSeconds = -1.0;
    }
    model_.loopEnabled = true;
    return true;
}

bool SharedTrackController::setLoopEndAtMilliseconds(qint64 positionMs) noexcept
{
    if (model_.durationMs <= 0) return false;
    const qint64 bounded = qBound<qint64>(
        1,
        positionMs,
        static_cast<qint64>(model_.durationMs));
    model_.loopEndSeconds = static_cast<double>(bounded) / 1000.0;
    if (model_.loopStartSeconds >= model_.loopEndSeconds) {
        model_.loopStartSeconds = -1.0;
    }
    model_.loopEnabled = true;
    return true;
}

void SharedTrackController::setWholeTrackLoop() noexcept
{
    model_.loopEnabled = true;
    model_.loopStartSeconds = -1.0;
    model_.loopEndSeconds = -1.0;
}

void SharedTrackController::clearLoop() noexcept
{
    model_.loopEnabled = false;
    model_.loopStartSeconds = -1.0;
    model_.loopEndSeconds = -1.0;
}

SharedTrackController::EffectiveLoop SharedTrackController::effectiveLoop(
    int sampleRate,
    qint64 frames) const noexcept
{
    EffectiveLoop result;
    if (!model_.loopEnabled || sampleRate <= 0 || frames <= 0) return result;

    const auto frameAt = [sampleRate, frames](double seconds, qint64 fallback) {
        if (!std::isfinite(seconds) || seconds < 0.0) return fallback;
        const long double scaled = static_cast<long double>(seconds) *
            static_cast<long double>(sampleRate);
        if (scaled >= static_cast<long double>(frames)) return frames;
        return qMax<qint64>(0, static_cast<qint64>(std::llround(scaled)));
    };
    result.enabled = true;
    result.startFrame = qBound<qint64>(
        0,
        frameAt(model_.loopStartSeconds, 0),
        frames - 1);
    const qint64 requestedEnd = model_.loopEndSeconds > model_.loopStartSeconds
        ? frameAt(model_.loopEndSeconds, frames)
        : frames;
    result.endFrame = qBound<qint64>(
        result.startFrame + 1,
        requestedEnd,
        frames);
    return result;
}

QJsonObject SharedTrackController::projectJson() const
{
    return {
        {QStringLiteral("file_name"), model_.fileName},
        {QStringLiteral("file_path"), model_.filePath},
        {QStringLiteral("file_bytes"), model_.fileBytes},
        {QStringLiteral("sample_rate"), model_.sampleRate},
        {QStringLiteral("duration_ms"), model_.durationMs},
        {QStringLiteral("sha256"), model_.sha256},
        {QStringLiteral("guessed_bpm"), model_.guessedBpm},
        {QStringLiteral("accepted_bpm"), model_.acceptedBpm},
        {QStringLiteral("key"), model_.key},
        {QStringLiteral("speed"), model_.speed},
        {QStringLiteral("pitch_cents"), model_.pitchCents},
        {QStringLiteral("track_gain_db"), model_.trackGainDb},
        {QStringLiteral("loop_enabled"), model_.loopEnabled},
        {QStringLiteral("loop_start_seconds"), model_.loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), model_.loopEndSeconds},
        {QStringLiteral("focus_enabled"), model_.focusEnabled},
        {QStringLiteral("focus_preset"), model_.focusPreset},
        {QStringLiteral("focus_frequency_hz"), model_.focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), model_.focusGainDb},
        {QStringLiteral("focus_q"), model_.focusQ},
        {QStringLiteral("highpass_hz"), model_.highpassHz},
        {QStringLiteral("lowpass_hz"), model_.lowpassHz},
    };
}

SharedTrackController::ProjectDecodeResult SharedTrackController::decodeProjectJson(
    const QJsonValue& value,
    const QString& projectFolder,
    bool syncControls)
{
    ProjectDecodeResult result;
    result.model.syncControls = syncControls;
    if (value.isUndefined()) {
        result.valid = true;
        return result;
    }
    if (!value.isObject()) {
        result.error = QStringLiteral("track must be an object");
        return result;
    }
    const QJsonObject object = value.toObject();
    SharedTrackModel candidate;
    QString& error = result.error;
    qint64 fileBytes = candidate.fileBytes;
    qint64 sampleRate = candidate.sampleRate;
    qint64 durationMs = candidate.durationMs;
    qint64 pitchCents = candidate.pitchCents;
    if (!readString(object, u"file_name", candidate.fileName,
            kMaximumTrackTextCharacters, error) ||
        !readString(object, u"file_path", candidate.filePath,
            kMaximumTrackPathCharacters, error) ||
        !readInteger(object, u"file_bytes", fileBytes, 0,
            jam2::application::limits::kMaximumAssetBytes, error) ||
        !readInteger(object, u"sample_rate", sampleRate, 0,
            jam2::application::limits::kMaximumSampleRate, error) ||
        (sampleRate > 0 && sampleRate <
            jam2::application::limits::kMinimumSampleRate) ||
        !readInteger(object, u"duration_ms", durationMs, 0,
            (std::numeric_limits<int>::max)(), error) ||
        !readString(object, u"sha256", candidate.sha256, 64, error) ||
        !readFinite(object, u"guessed_bpm", candidate.guessedBpm, 0.0, 400.0, error) ||
        !readFinite(object, u"accepted_bpm", candidate.acceptedBpm, 1.0, 400.0, error) ||
        !readString(object, u"key", candidate.key, 128, error) ||
        !readFinite(object, u"speed", candidate.speed, 0.10, 2.0, error) ||
        !readInteger(object, u"pitch_cents", pitchCents, -1200, 1200, error) ||
        !readFinite(object, u"track_gain_db", candidate.trackGainDb, -60.0, 12.0, error) ||
        !readBoolean(object, u"loop_enabled", candidate.loopEnabled, error) ||
        !readFinite(object, u"loop_start_seconds", candidate.loopStartSeconds,
            -1.0, static_cast<double>((std::numeric_limits<int>::max)()) / 1000.0,
            error) ||
        !readFinite(object, u"loop_end_seconds", candidate.loopEndSeconds,
            -1.0, static_cast<double>((std::numeric_limits<int>::max)()) / 1000.0,
            error) ||
        !readBoolean(object, u"focus_enabled", candidate.focusEnabled, error) ||
        !readString(object, u"focus_preset", candidate.focusPreset, 128, error) ||
        !readFinite(object, u"focus_frequency_hz", candidate.focusFrequencyHz,
            20.0, 8000.0, error) ||
        !readFinite(object, u"focus_gain_db", candidate.focusGainDb,
            -24.0, 24.0, error) ||
        !readFinite(object, u"focus_q", candidate.focusQ, 0.1, 20.0, error) ||
        !readFinite(object, u"highpass_hz", candidate.highpassHz,
            0.0, 20000.0, error) ||
        !readFinite(object, u"lowpass_hz", candidate.lowpassHz,
            0.0, 20000.0, error)) {
        if (error.isEmpty()) error = QStringLiteral("track sample rate is out of range");
        return result;
    }
    if (!validSha256(candidate.sha256)) {
        result.error = QStringLiteral("track sha256 is invalid");
        return result;
    }
    if ((!isUnsetLoopPoint(candidate.loopStartSeconds) &&
         candidate.loopStartSeconds < 0.0) ||
        (!isUnsetLoopPoint(candidate.loopEndSeconds) &&
         candidate.loopEndSeconds < 0.0)) {
        result.error = QStringLiteral("track loop points must be -1 or non-negative");
        return result;
    }

    candidate.fileBytes = fileBytes;
    candidate.sampleRate = static_cast<int>(sampleRate);
    candidate.durationMs = static_cast<int>(durationMs);
    candidate.pitchCents = static_cast<int>(pitchCents);
    candidate.sha256 = candidate.sha256.toLower();
    if (!candidate.filePath.isEmpty() && !QFileInfo(candidate.filePath).isAbsolute() &&
        !projectFolder.isEmpty()) {
        candidate.filePath = QDir(projectFolder).absoluteFilePath(candidate.filePath);
    }
    candidate.sampleRateCompatible = true;
    candidate.userProvidedSource = !candidate.filePath.isEmpty();
    candidate.syncControls = syncControls;

    const double durationSeconds = static_cast<double>(candidate.durationMs) / 1000.0;
    if (candidate.durationMs <= 0) {
        result.normalized = candidate.loopStartSeconds != -1.0 ||
            candidate.loopEndSeconds != -1.0;
        candidate.loopStartSeconds = -1.0;
        candidate.loopEndSeconds = -1.0;
    } else {
        if (candidate.loopStartSeconds >= 0.0) {
            const double maximumStart = qMax(
                0.0, static_cast<double>(candidate.durationMs - 1) / 1000.0);
            if (candidate.loopStartSeconds > maximumStart) {
                candidate.loopStartSeconds = maximumStart;
                result.normalized = true;
            }
        }
        if (candidate.loopEndSeconds >= 0.0 &&
            candidate.loopEndSeconds > durationSeconds) {
            candidate.loopEndSeconds = durationSeconds;
            result.normalized = true;
        }
        if (candidate.loopStartSeconds >= 0.0 &&
            candidate.loopEndSeconds >= 0.0 &&
            candidate.loopEndSeconds <= candidate.loopStartSeconds) {
            candidate.loopStartSeconds = -1.0;
            candidate.loopEndSeconds = -1.0;
            result.normalized = true;
        }
    }
    result.model = std::move(candidate);
    result.valid = true;
    return result;
}

void SharedTrackController::requestPlayback(
    bool playing,
    quint64 arrangementRevision) noexcept
{
    playback_.requestedPlaying = playing;
    if (arrangementRevision > 0) {
        playback_.arrangementRevision = arrangementRevision;
    }
    playback_.phase = playing ? PlaybackPhase::WaitingForTransport :
        (playback_.actualPlaying ? PlaybackPhase::WaitingForTransport : PlaybackPhase::Stopped);
}

void SharedTrackController::waitForAssets(
    quint64 arrangementRevision,
    bool playing) noexcept
{
    playback_.requestedPlaying = playing;
    playback_.arrangementRevision = arrangementRevision;
    playback_.phase = PlaybackPhase::WaitingForAssets;
}

void SharedTrackController::prepareMix(
    quint64 arrangementRevision,
    bool playing) noexcept
{
    playback_.requestedPlaying = playing;
    playback_.arrangementRevision = arrangementRevision;
    playback_.phase = PlaybackPhase::PreparingMix;
}

void SharedTrackController::preparedForTransport(quint64 arrangementRevision) noexcept
{
    if (arrangementRevision != 0 && arrangementRevision != playback_.arrangementRevision) {
        return;
    }
    playback_.phase = playback_.requestedPlaying
        ? PlaybackPhase::WaitingForTransport
        : (playback_.actualPlaying ? PlaybackPhase::WaitingForTransport : PlaybackPhase::Stopped);
}

bool SharedTrackController::observeEnginePlaying(bool playing) noexcept
{
    const PlaybackState before = playback_;
    playback_.actualPlaying = playing;
    if (playing) {
        if (playback_.phase != PlaybackPhase::WaitingForTransport ||
            playback_.requestedPlaying) {
            playback_.requestedPlaying = true;
            playback_.phase = PlaybackPhase::Playing;
        }
    } else if (!playback_.requestedPlaying) {
        playback_.phase = PlaybackPhase::Stopped;
    } else if (playback_.phase == PlaybackPhase::Playing) {
        playback_.requestedPlaying = false;
        playback_.phase = PlaybackPhase::Stopped;
    }
    return playback_ != before;
}

QString SharedTrackController::playbackStatusText(bool syncEnabled) const
{
    if (!model_.sampleRateCompatible) {
        return QStringLiteral("Unavailable: WAV conversion failed; unload or replace track");
    }
    const QString scope = syncEnabled ? QStringLiteral("Shared") : QStringLiteral("Independent");
    switch (playback_.phase) {
    case PlaybackPhase::WaitingForAssets:
        return scope + QStringLiteral(": waiting for assets");
    case PlaybackPhase::PreparingMix:
        return scope + QStringLiteral(": preparing");
    case PlaybackPhase::WaitingForTransport:
        return playback_.requestedPlaying
            ? scope + QStringLiteral(": waiting to play")
            : scope + QStringLiteral(": waiting to stop");
    case PlaybackPhase::Playing:
        return scope + QStringLiteral(": playing");
    case PlaybackPhase::Stopped:
        return scope + QStringLiteral(": stopped");
    }
    return scope;
}
