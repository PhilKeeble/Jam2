#include "ControlMessageValidation.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <limits>

namespace {

bool isSha256Hex(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

bool isBoundedInteger(const QJsonValue& value, qint64 minimum, qint64 maximum)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number &&
        number >= static_cast<double>(minimum) && number <= static_cast<double>(maximum);
}

bool isBoundedString(const QJsonValue& value, qsizetype maximum)
{
    return value.isString() && value.toString().size() <= maximum;
}

bool validateRemoteSongAssets(const QJsonObject& song, QString& reason)
{
    const QJsonValue looperValue = song.value(QStringLiteral("looper"));
    if (looperValue.isUndefined()) {
        return true;
    }
    if (!looperValue.isObject()) {
        reason = QStringLiteral("song looper field is not an object");
        return false;
    }
    const QJsonArray banks = looperValue.toObject().value(QStringLiteral("banks")).toArray();
    if (banks.size() > 4) {
        reason = QStringLiteral("song contains too many looper banks");
        return false;
    }
    for (const QJsonValue& bankValue : banks) {
        if (!bankValue.isObject()) {
            reason = QStringLiteral("song looper bank is not an object");
            return false;
        }
        const QJsonArray lanes = bankValue.toObject().value(QStringLiteral("lanes")).toArray();
        if (lanes.size() > 128) {
            reason = QStringLiteral("song looper bank contains too many lanes");
            return false;
        }
        for (const QJsonValue& laneValue : lanes) {
            if (!laneValue.isObject()) {
                reason = QStringLiteral("song looper lane is not an object");
                return false;
            }
            const QString hash = laneValue.toObject().value(QStringLiteral("asset_hash")).toString();
            if (!hash.isEmpty() && !isSha256Hex(hash)) {
                reason = QStringLiteral("song contains an invalid asset hash");
                return false;
            }
        }
    }
    return true;
}

}

bool jam2::application::isTrackSyncControlMessageType(const QString& type) noexcept
{
    return type == QStringLiteral("song.set") ||
        type == QStringLiteral("track.ready") ||
        type == QStringLiteral("looper.track.share.request") ||
        type == QStringLiteral("looper.recording.offer") ||
        type == QStringLiteral("looper.asset.request") ||
        type == QStringLiteral("looper.asset.start") ||
        type == QStringLiteral("looper.asset.chunk") ||
        type == QStringLiteral("looper.asset.done");
}

bool jam2::application::validateControlMessage(
    const QJsonObject& message,
    QString& reason)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type.isEmpty() || type.size() > 64) {
        reason = QStringLiteral("message type is missing or too long");
        return false;
    }
    const QJsonValue revision = message.value(QStringLiteral("revision"));
    if (!revision.isUndefined() &&
        !isBoundedInteger(revision, 0, (std::numeric_limits<int>::max)())) {
        reason = QStringLiteral("message revision is invalid");
        return false;
    }
    if (type == QStringLiteral("session.membership")) {
        const QJsonValue peersValue = message.value(QStringLiteral("peers"));
        const int pageIndex = message.value(QStringLiteral("page_index")).toInt(-1);
        const int pageCount = message.value(QStringLiteral("page_count")).toInt(-1);
        if (!peersValue.isArray() || peersValue.toArray().size() > 64 ||
            pageIndex < 0 || pageCount <= 0 || pageIndex >= pageCount) {
            reason = QStringLiteral("network membership page is invalid");
            return false;
        }
        static const QRegularExpression tokenExpression(QStringLiteral("^[0-9a-f]{32}$"));
        if (!tokenExpression.match(
                message.value(QStringLiteral("coordinator_token")).toString()).hasMatch()) {
            reason = QStringLiteral("mesh coordinator token is invalid");
            return false;
        }
        for (const QJsonValue& peerValue : peersValue.toArray()) {
            const QJsonObject peer = peerValue.toObject();
            if (!peerValue.isObject() ||
                !tokenExpression.match(peer.value(QStringLiteral("token")).toString()).hasMatch() ||
                !isBoundedString(peer.value(QStringLiteral("endpoint")), 255)) {
                reason = QStringLiteral("mesh peer entry is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("session.error")) {
        return isBoundedString(message.value(QStringLiteral("message")), 4096)
            ? true : (reason = QStringLiteral("session error text is invalid"), false);
    }
    if (type == QStringLiteral("metronome.settings")) {
        static const QStringList maskKeys{
            QStringLiteral("play_mask_low"), QStringLiteral("play_mask_high"),
            QStringLiteral("accent_mask_low"), QStringLiteral("accent_mask_high")};
        if (!message.value(QStringLiteral("running")).isBool() ||
            !message.value(QStringLiteral("leader")).isBool() ||
            !isBoundedString(message.value(QStringLiteral("mode")), 32) ||
            !isBoundedInteger(message.value(QStringLiteral("bpm")), 1, 400) ||
            !isBoundedInteger(message.value(QStringLiteral("beats")), 1, 16) ||
            !isBoundedInteger(message.value(QStringLiteral("division")), 1, 16)) {
            reason = QStringLiteral("metronome settings are invalid");
            return false;
        }
        static const QRegularExpression maskExpression(QStringLiteral("^[0-9a-fA-F]{1,16}$"));
        for (const QString& key : maskKeys) {
            if (!maskExpression.match(message.value(key).toString()).hasMatch()) {
                reason = QStringLiteral("metronome mask is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("beat.set")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        return isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beat")), 0, 511) &&
            (lane == QStringLiteral("chord") || lane == QStringLiteral("beat") ||
             lane == QStringLiteral("lyric")) &&
            isBoundedString(message.value(QStringLiteral("text")), 4096)
            ? true : (reason = QStringLiteral("beat cell update is invalid"), false);
    }
    if (type == QStringLiteral("grid.resize")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        return (lane == QStringLiteral("chord") || lane == QStringLiteral("beat")) &&
            isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beats")), 4, 512)
            ? true : (reason = QStringLiteral("grid resize is invalid"), false);
    }
    if (type == QStringLiteral("beat.hit")) {
        return isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beat")), 0, 511) &&
            isBoundedInteger(message.value(QStringLiteral("lane")), 0, 10) &&
            isBoundedString(message.value(QStringLiteral("text")), 4096)
            ? true : (reason = QStringLiteral("beat hit is invalid"), false);
    }
    if (type == QStringLiteral("beat.division")) {
        const int division = message.value(QStringLiteral("division")).toInt(-1);
        return isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beat")), 0, 511) &&
            QList<int>{1, 2, 3, 4, 6, 8}.contains(division)
            ? true : (reason = QStringLiteral("beat division is invalid"), false);
    }
    if (type == QStringLiteral("lyrics.set")) {
        return isBoundedString(message.value(QStringLiteral("text")), 64 * 1024)
            ? true : (reason = QStringLiteral("lyrics update is invalid"), false);
    }
    if (type == QStringLiteral("song.set")) {
        const QJsonValue songValue = message.value(QStringLiteral("song"));
        if (!songValue.isObject() ||
            !isBoundedInteger(
                message.value(QStringLiteral("arrangement_revision")),
                0,
                (std::numeric_limits<int>::max)()) ||
            (!message.value(QStringLiteral("host_authoritative")).isUndefined() &&
             !message.value(QStringLiteral("host_authoritative")).isBool()) ||
            (!message.value(QStringLiteral("track_playing")).isUndefined() &&
             !message.value(QStringLiteral("track_playing")).isBool())) {
            reason = QStringLiteral("song snapshot or revision is invalid");
            return false;
        }
        return validateRemoteSongAssets(songValue.toObject(), reason);
    }
    if (type == QStringLiteral("track.ready")) {
        return isBoundedInteger(
            message.value(QStringLiteral("arrangement_revision")),
            1,
            (std::numeric_limits<int>::max)())
            ? true : (reason = QStringLiteral("track readiness revision is invalid"), false);
    }
    if (type == QStringLiteral("looper.track.share.request")) {
        return true;
    }
    if (type == QStringLiteral("looper.recording.offer")) {
        static const QRegularExpression recordingIdExpression(QStringLiteral(
            "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
        const QString recordingId = message.value(QStringLiteral("recording_id")).toString();
        const QString targetLaneId = message.value(QStringLiteral("target_lane_id")).toString();
        const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
        return recordingIdExpression.match(recordingId).hasMatch() &&
            isBoundedInteger(message.value(QStringLiteral("bank")), 0, 3) &&
            !targetLaneId.isEmpty() && targetLaneId.size() <= 80 &&
            isSha256Hex(hash) && isBoundedString(message.value(QStringLiteral("name")), 512)
            ? true : (reason = QStringLiteral("recording offer is invalid"), false);
    }
    if (type == QStringLiteral("looper.asset.request")) {
        const QJsonValue arrangementRevision =
            message.value(QStringLiteral("arrangement_revision"));
        return message.value(QStringLiteral("hashes")).isArray() &&
            (arrangementRevision.isUndefined() ||
             isBoundedInteger(
                 arrangementRevision, 0, (std::numeric_limits<int>::max)()));
    }
    if (type == QStringLiteral("looper.asset.start") ||
        type == QStringLiteral("looper.asset.chunk") ||
        type == QStringLiteral("looper.asset.done")) {
        return true; // The asset service applies transfer-specific strict checks before file mutation.
    }
    reason = QStringLiteral("unknown message type");
    return false;
}
