#include "SharedTrackController.hpp"

SharedTrackModel& SharedTrackController::model()
{
    return model_;
}

const SharedTrackModel& SharedTrackController::model() const
{
    return model_;
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
        {QStringLiteral("speed"), model_.speed},
        {QStringLiteral("pitch_cents"), model_.pitchCents},
        {QStringLiteral("loop_enabled"), model_.loopEnabled},
        {QStringLiteral("loop_start_seconds"), model_.loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), model_.loopEndSeconds},
        {QStringLiteral("focus_frequency_hz"), model_.focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), model_.focusGainDb},
        {QStringLiteral("focus_q"), model_.focusQ},
        {QStringLiteral("highpass_hz"), model_.highpassHz},
        {QStringLiteral("lowpass_hz"), model_.lowpassHz},
    };
}

void SharedTrackController::applyProcessingMessage(const QJsonObject& message)
{
    model_.speed = message.value(QStringLiteral("speed")).toDouble(model_.speed);
    model_.fileName = message.value(QStringLiteral("file_name")).toString(model_.fileName);
    model_.filePath = message.value(QStringLiteral("file_path")).toString(model_.filePath);
    model_.fileBytes = static_cast<qint64>(message.value(QStringLiteral("file_bytes")).toDouble(model_.fileBytes));
    model_.sampleRate = message.value(QStringLiteral("sample_rate")).toInt(model_.sampleRate);
    model_.durationMs = message.value(QStringLiteral("duration_ms")).toInt(model_.durationMs);
    model_.sha256 = message.value(QStringLiteral("sha256")).toString(model_.sha256);
    model_.pitchCents = message.value(QStringLiteral("pitch_cents")).toInt(model_.pitchCents);
    model_.loopEnabled = message.value(QStringLiteral("loop_enabled")).toBool(model_.loopEnabled);
    model_.loopStartSeconds = message.value(QStringLiteral("loop_start_seconds")).toDouble(model_.loopStartSeconds);
    model_.loopEndSeconds = message.value(QStringLiteral("loop_end_seconds")).toDouble(model_.loopEndSeconds);
    model_.focusFrequencyHz = message.value(QStringLiteral("focus_frequency_hz")).toDouble(model_.focusFrequencyHz);
    model_.focusGainDb = message.value(QStringLiteral("focus_gain_db")).toDouble(model_.focusGainDb);
    model_.focusQ = message.value(QStringLiteral("focus_q")).toDouble(model_.focusQ);
    model_.highpassHz = message.value(QStringLiteral("highpass_hz")).toDouble(model_.highpassHz);
    model_.lowpassHz = message.value(QStringLiteral("lowpass_hz")).toDouble(model_.lowpassHz);
}
