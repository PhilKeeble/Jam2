#include "DebugActionValidation.hpp"

#include <QJsonValue>

#include <cmath>
#include <limits>
#include <set>

namespace {

constexpr qsizetype kMaxActionIdBytes = 128;
constexpr qsizetype kMaxActionPathBytes = 4096;
constexpr double kMaxExactJsonInteger = 9007199254740991.0;

bool boundedOptionalText(
    const QJsonObject& action,
    const QString& key,
    qsizetype maximumBytes,
    QString& error)
{
    if (!action.contains(key)) return true;
    const QJsonValue value = action.value(key);
    if (!value.isString() || value.toString().isEmpty() ||
        value.toString().toUtf8().size() > maximumBytes) {
        error = key + QStringLiteral(" must be a bounded non-empty string");
        return false;
    }
    return true;
}

bool boundedFrame(const QJsonObject& action, const QString& key, QString& error)
{
    if (!action.contains(key)) return true;
    const QJsonValue value = action.value(key);
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || !std::isfinite(number) || number < 0.0 ||
        number > kMaxExactJsonInteger || std::floor(number) != number) {
        error = key + QStringLiteral(" must be a non-negative exact JSON integer");
        return false;
    }
    return true;
}

bool boundedInteger(
    const QJsonObject& action,
    const QString& key,
    int minimum,
    int maximum,
    bool required,
    QString& error)
{
    if (!action.contains(key)) {
        if (required) error = key + QStringLiteral(" is required");
        return !required;
    }
    const QJsonValue value = action.value(key);
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number ||
        number < minimum || number > maximum) {
        error = key + QStringLiteral(" is outside its integer bounds");
        return false;
    }
    return true;
}

bool boundedGain(const QJsonObject& action, QString& error)
{
    const QJsonValue value = action.value(QStringLiteral("value"));
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) || number < 0.0 || number > 4.0) {
        error = QStringLiteral("value must be a finite gain from 0 through 4");
        return false;
    }
    return true;
}

} // namespace

bool jam2ValidateDebugAction(const QJsonObject& action, QString& error)
{
    error.clear();
    if (action.isEmpty()) {
        error = QStringLiteral("action must be an object");
        return false;
    }

    const QString type = action.value(QStringLiteral("type")).toString();
    static const std::set<QString> supportedTypes{
        QStringLiteral("metronome.enabled"), QStringLiteral("metronome.bpm"),
        QStringLiteral("metronome.mode"), QStringLiteral("metronome.level"),
        QStringLiteral("remote.level"), QStringLiteral("track.sync"),
        QStringLiteral("track.load"), QStringLiteral("track.play"),
        QStringLiteral("track.stop"), QStringLiteral("track.restart"),
        QStringLiteral("track.record-start"), QStringLiteral("recording.start"),
        QStringLiteral("recording.stop"), QStringLiteral("snapshot"),
        QStringLiteral("shutdown")};
    if (!supportedTypes.contains(type)) {
        error = QStringLiteral("unsupported action type");
        return false;
    }

    std::set<QString> allowed{
        QStringLiteral("id"), QStringLiteral("type"),
        QStringLiteral("after_event"), QStringLiteral("delay_frames"),
        QStringLiteral("apply_frame")};
    if (type == QStringLiteral("metronome.enabled") ||
        type == QStringLiteral("track.sync")) {
        allowed.insert(QStringLiteral("enabled"));
    } else if (type == QStringLiteral("metronome.bpm") ||
               type == QStringLiteral("metronome.level") ||
               type == QStringLiteral("remote.level")) {
        allowed.insert(QStringLiteral("value"));
    } else if (type == QStringLiteral("metronome.mode")) {
        allowed.insert(QStringLiteral("mode"));
    } else if (type == QStringLiteral("track.load") ||
               type == QStringLiteral("recording.start")) {
        allowed.insert(QStringLiteral("path"));
    } else if (type == QStringLiteral("track.restart") ||
               type == QStringLiteral("track.record-start")) {
        allowed.insert(QStringLiteral("count_in_bars"));
    }
    for (auto it = action.begin(); it != action.end(); ++it) {
        if (!allowed.contains(it.key())) {
            error = QStringLiteral("action contains a field not valid for its type: ") + it.key();
            return false;
        }
    }

    if (!boundedOptionalText(action, QStringLiteral("id"), kMaxActionIdBytes, error) ||
        !boundedFrame(action, QStringLiteral("apply_frame"), error) ||
        !boundedFrame(action, QStringLiteral("delay_frames"), error)) {
        return false;
    }
    if (action.contains(QStringLiteral("apply_frame")) &&
        action.contains(QStringLiteral("delay_frames"))) {
        error = QStringLiteral("action cannot contain both apply_frame and delay_frames");
        return false;
    }
    if (action.contains(QStringLiteral("after_event"))) {
        const QJsonValue value = action.value(QStringLiteral("after_event"));
        static const std::set<QString> events{
            QStringLiteral("controller_started"), QStringLiteral("network.waiting"),
            QStringLiteral("network.connected"), QStringLiteral("peer_snapshot"),
            QStringLiteral("command_applied")};
        if (!value.isString() || !events.contains(value.toString())) {
            error = QStringLiteral("after_event is not supported");
            return false;
        }
    }

    if (type == QStringLiteral("metronome.enabled") ||
        type == QStringLiteral("track.sync")) {
        if (!action.value(QStringLiteral("enabled")).isBool()) {
            error = QStringLiteral("enabled must be a boolean");
            return false;
        }
    } else if (type == QStringLiteral("metronome.bpm")) {
        return boundedInteger(action, QStringLiteral("value"), 1, 400, true, error);
    } else if (type == QStringLiteral("metronome.level") ||
               type == QStringLiteral("remote.level")) {
        return boundedGain(action, error);
    } else if (type == QStringLiteral("metronome.mode")) {
        const QJsonValue value = action.value(QStringLiteral("mode"));
        static const std::set<QString> modes{
            QStringLiteral("shared-grid"), QStringLiteral("leader-audio"),
            QStringLiteral("listener-compensated")};
        if (!value.isString() || !modes.contains(value.toString())) {
            error = QStringLiteral("mode is not supported");
            return false;
        }
    } else if (type == QStringLiteral("track.load") ||
               type == QStringLiteral("recording.start")) {
        if (!boundedOptionalText(action, QStringLiteral("path"), kMaxActionPathBytes, error) ||
            !action.contains(QStringLiteral("path"))) {
            if (error.isEmpty()) error = QStringLiteral("path is required");
            return false;
        }
    } else if ((type == QStringLiteral("track.restart") ||
                type == QStringLiteral("track.record-start")) &&
               !boundedInteger(action, QStringLiteral("count_in_bars"), 0, 8, false, error)) {
        return false;
    }
    return true;
}
