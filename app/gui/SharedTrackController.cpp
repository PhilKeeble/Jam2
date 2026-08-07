#include "SharedTrackController.hpp"

SharedTrackModel& SharedTrackController::model()
{
    return model_;
}

const SharedTrackModel& SharedTrackController::model() const
{
    return model_;
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
        return QStringLiteral("Quarantined: sample-rate mismatch; unload or replace track");
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

QJsonObject SharedTrackController::processingMessage() const
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.processing")},
        {QStringLiteral("file_name"), model_.fileName},
        {QStringLiteral("file_path"), model_.filePath},
        {QStringLiteral("file_bytes"), model_.fileBytes},
        {QStringLiteral("sample_rate"), model_.sampleRate},
        {QStringLiteral("duration_ms"), model_.durationMs},
        {QStringLiteral("sha256"), model_.sha256},
        {QStringLiteral("loop_enabled"), model_.loopEnabled},
        {QStringLiteral("loop_start_seconds"), model_.loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), model_.loopEndSeconds},
    };
}

void SharedTrackController::applyProcessingMessage(const QJsonObject& message)
{
    model_.fileName = message.value(QStringLiteral("file_name")).toString(model_.fileName);
    model_.filePath = message.value(QStringLiteral("file_path")).toString(model_.filePath);
    model_.userProvidedSource = false;
    model_.fileBytes = static_cast<qint64>(message.value(QStringLiteral("file_bytes")).toDouble(model_.fileBytes));
    model_.sampleRate = message.value(QStringLiteral("sample_rate")).toInt(model_.sampleRate);
    model_.durationMs = message.value(QStringLiteral("duration_ms")).toInt(model_.durationMs);
    model_.sha256 = message.value(QStringLiteral("sha256")).toString(model_.sha256);
    model_.loopEnabled = message.value(QStringLiteral("loop_enabled")).toBool(model_.loopEnabled);
    model_.loopStartSeconds = message.value(QStringLiteral("loop_start_seconds")).toDouble(model_.loopStartSeconds);
    model_.loopEndSeconds = message.value(QStringLiteral("loop_end_seconds")).toDouble(model_.loopEndSeconds);
}
