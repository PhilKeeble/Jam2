#include "ControlMessageValidation.hpp"
#include "ContentLimits.hpp"
#include "ControlProtocol.hpp"
#include "protocol.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <limits>

namespace {

namespace limits = jam2::application::limits;

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

bool isOptionalBoundedString(const QJsonObject& object, const QString& key, qsizetype maximum)
{
    const QJsonValue value = object.value(key);
    return value.isUndefined() || isBoundedString(value, maximum);
}

bool isOptionalBoolean(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    return value.isUndefined() || value.isBool();
}

bool validateTextArray(
    const QJsonObject& object,
    const QString& key,
    int maximumCount,
    QString& reason)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        return true;
    }
    if (!value.isArray() || value.toArray().size() > maximumCount) {
        reason = QStringLiteral("song grid text array is invalid");
        return false;
    }
    for (const QJsonValue& cell : value.toArray()) {
        if (!isBoundedString(cell, limits::kMaximumCellCharacters)) {
            reason = QStringLiteral("song grid cell text is invalid");
            return false;
        }
    }
    return true;
}

bool validateBeatGrid(const QJsonObject& grid, QString& reason)
{
    if (grid.value(QStringLiteral("format")).toString() != QStringLiteral("jam2.song.v1") ||
        !isOptionalBoundedString(grid, QStringLiteral("title"), limits::kMaximumTitleCharacters) ||
        !isOptionalBoundedString(grid, QStringLiteral("lyrics_text"), limits::kMaximumLyricsCharacters) ||
        !grid.value(QStringLiteral("sections")).isArray()) {
        reason = QStringLiteral("song grid header is invalid");
        return false;
    }

    const QJsonArray sections = grid.value(QStringLiteral("sections")).toArray();
    if (sections.isEmpty() || sections.size() > limits::kMaximumSongSections) {
        reason = QStringLiteral("song grid section count is invalid");
        return false;
    }
    static const QList<int> allowedDivisions{1, 2, 3, 4, 6, 8};
    for (const QJsonValue& sectionValue : sections) {
        if (!sectionValue.isObject()) {
            reason = QStringLiteral("song grid section is not an object");
            return false;
        }
        const QJsonObject section = sectionValue.toObject();
        const QJsonValue beatsValue = section.value(QStringLiteral("beats"));
        const int beats = beatsValue.isUndefined() ? 8 : beatsValue.toInt(-1);
        if (!isOptionalBoundedString(section, QStringLiteral("label"), limits::kMaximumCellCharacters) ||
            !isOptionalBoundedString(section, QStringLiteral("name"), limits::kMaximumCellCharacters) ||
            (!beatsValue.isUndefined() && !isBoundedInteger(
                beatsValue, limits::kMinimumBeatsPerSection, limits::kMaximumBeatsPerSection)) ||
            beats < limits::kMinimumBeatsPerSection ||
            beats > limits::kMaximumBeatsPerSection ||
            !validateTextArray(section, QStringLiteral("chords"), beats, reason) ||
            !validateTextArray(section, QStringLiteral("beat_notes"), beats, reason) ||
            !validateTextArray(section, QStringLiteral("lyrics"), beats, reason)) {
            if (reason.isEmpty()) {
                reason = QStringLiteral("song grid section fields are invalid");
            }
            return false;
        }

        const QJsonValue patternsValue = section.value(QStringLiteral("beat_patterns"));
        if (patternsValue.isUndefined()) {
            continue;
        }
        if (!patternsValue.isArray() || patternsValue.toArray().size() > beats) {
            reason = QStringLiteral("song beat pattern count is invalid");
            return false;
        }
        for (const QJsonValue& patternValue : patternsValue.toArray()) {
            if (!patternValue.isObject()) {
                reason = QStringLiteral("song beat pattern is not an object");
                return false;
            }
            const QJsonObject pattern = patternValue.toObject();
            const QJsonValue divisionValue = pattern.value(QStringLiteral("division"));
            const int division = divisionValue.isUndefined() ? 4 : divisionValue.toInt(-1);
            if ((!divisionValue.isUndefined() && !isBoundedInteger(divisionValue, 1, 8)) ||
                !allowedDivisions.contains(division)) {
                reason = QStringLiteral("song beat division is invalid");
                return false;
            }
            if (!validateTextArray(
                    pattern, QStringLiteral("lanes"), limits::kMaximumBeatLanes, reason)) {
                return false;
            }
        }
    }
    return true;
}

bool readJsonFrame(const QJsonValue& value, qint64 fallback, qint64& result)
{
    if (value.isUndefined()) {
        result = fallback;
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        result = value.toString().toLongLong(&ok);
        return ok;
    }
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    constexpr double maximumExactJsonInteger = 9007199254740991.0;
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < -maximumExactJsonInteger || number > maximumExactJsonInteger) {
        return false;
    }
    result = static_cast<qint64>(number);
    return true;
}

bool validateRemoteLooper(const QJsonObject& looper, QString& reason)
{
    const QJsonValue format = looper.value(QStringLiteral("format"));
    const QJsonValue activeBank = looper.value(QStringLiteral("active_bank"));
    if ((!format.isUndefined() && format.toString() != QStringLiteral("jam2.looper.v1")) ||
        (!activeBank.isUndefined() &&
         !isBoundedInteger(activeBank, 0, limits::kLooperBankCount - 1)) ||
        !isOptionalBoolean(looper, QStringLiteral("grid_lock")) ||
        !looper.value(QStringLiteral("banks")).isArray()) {
        reason = QStringLiteral("song looper header is invalid");
        return false;
    }
    const QJsonArray banks = looper.value(QStringLiteral("banks")).toArray();
    if (banks.size() != limits::kLooperBankCount) {
        reason = QStringLiteral("song looper bank count is invalid");
        return false;
    }
    for (int bankIndex = 0; bankIndex < banks.size(); ++bankIndex) {
        const QJsonValue& bankValue = banks[bankIndex];
        if (!bankValue.isObject()) {
            reason = QStringLiteral("song looper bank is not an object");
            return false;
        }
        const QJsonObject bank = bankValue.toObject();
        const QJsonValue bankId = bank.value(QStringLiteral("id"));
        const QJsonValue lanesValue = bank.value(QStringLiteral("lanes"));
        if ((!bankId.isUndefined() &&
             bankId.toString() != QString(QChar(static_cast<ushort>('A' + bankIndex)))) ||
            !lanesValue.isArray() ||
            lanesValue.toArray().size() > limits::kMaximumLooperLanesPerBank) {
            reason = QStringLiteral("song looper bank fields are invalid");
            return false;
        }
        for (const QJsonValue& laneValue : lanesValue.toArray()) {
            if (!laneValue.isObject()) {
                reason = QStringLiteral("song looper lane is not an object");
                return false;
            }
            const QJsonObject lane = laneValue.toObject();
            if (!isOptionalBoundedString(
                    lane, QStringLiteral("id"), limits::kMaximumLooperIdCharacters) ||
                !isOptionalBoundedString(
                    lane, QStringLiteral("asset_path"), limits::kMaximumLooperPathCharacters) ||
                !isOptionalBoundedString(lane, QStringLiteral("asset_hash"), 64) ||
                !isOptionalBoundedString(
                    lane, QStringLiteral("name"), limits::kMaximumLooperNameCharacters) ||
                !isOptionalBoolean(lane, QStringLiteral("muted")) ||
                !isOptionalBoolean(lane, QStringLiteral("solo")) ||
                !isOptionalBoolean(lane, QStringLiteral("loop_enabled"))) {
                reason = QStringLiteral("song looper lane fields are invalid");
                return false;
            }
            const QString hash = lane.value(QStringLiteral("asset_hash")).toString();
            const QJsonValue sampleRate = lane.value(QStringLiteral("sample_rate"));
            const QJsonValue gain = lane.value(QStringLiteral("gain_db"));
            qint64 startFrame = 0;
            qint64 stopFrame = -1;
            qint64 loopStartFrame = -1;
            qint64 loopEndFrame = -1;
            if ((!hash.isEmpty() && !isSha256Hex(hash)) ||
                (!sampleRate.isUndefined() &&
                 !isBoundedInteger(sampleRate, 0, limits::kMaximumSampleRate)) ||
                (!hash.isEmpty() && !isBoundedInteger(
                    sampleRate, limits::kMinimumSampleRate, limits::kMaximumSampleRate)) ||
                (!gain.isUndefined() &&
                 (!gain.isDouble() || !std::isfinite(gain.toDouble()) ||
                  gain.toDouble() < -120.0 || gain.toDouble() > 24.0)) ||
                !readJsonFrame(lane.value(QStringLiteral("start_frame")), 0, startFrame) ||
                !readJsonFrame(lane.value(QStringLiteral("stop_frame")), -1, stopFrame) ||
                !readJsonFrame(lane.value(QStringLiteral("loop_start_frame")), -1, loopStartFrame) ||
                !readJsonFrame(lane.value(QStringLiteral("loop_end_frame")), -1, loopEndFrame) ||
                startFrame < 0 || (stopFrame >= 0 && stopFrame < startFrame) ||
                (loopStartFrame >= 0 && loopEndFrame >= 0 && loopEndFrame <= loopStartFrame)) {
                reason = QStringLiteral("song looper lane values are invalid");
                return false;
            }
        }
    }
    return true;
}

bool validateRemoteSong(const QJsonObject& song, QString& reason)
{
    if (!validateBeatGrid(song, reason)) {
        return false;
    }
    const QJsonValue independentViews = song.value(QStringLiteral("independent_views"));
    if (!independentViews.isUndefined() && !independentViews.isBool()) {
        reason = QStringLiteral("song independent-view flag is invalid");
        return false;
    }
    for (const QString& key : {QStringLiteral("beat_view"), QStringLiteral("lyric_view")}) {
        const QJsonValue viewValue = song.value(key);
        if (viewValue.isUndefined()) {
            continue;
        }
        if (!viewValue.isObject()) {
            reason = QStringLiteral("song independent grid is not an object");
            return false;
        }
        if (!viewValue.toObject().isEmpty() && !validateBeatGrid(viewValue.toObject(), reason)) {
            return false;
        }
    }
    const QJsonValue looperValue = song.value(QStringLiteral("looper"));
    if (looperValue.isUndefined()) {
        return true;
    }
    if (!looperValue.isObject()) {
        reason = QStringLiteral("song looper field is not an object");
        return false;
    }
    return looperValue.toObject().isEmpty() || validateRemoteLooper(looperValue.toObject(), reason);
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
        type == QStringLiteral("looper.asset.done");
}

bool jam2::application::isGridControlMessageType(const QString& type) noexcept
{
    return type == QStringLiteral("metronome.settings") ||
        type == QStringLiteral("beat.set") ||
        type == QStringLiteral("beat.hit") ||
        type == QStringLiteral("beat.division") ||
        type == QStringLiteral("grid.resize") ||
        type == QStringLiteral("lyrics.set");
}

bool jam2::application::isArrangementControlMessageType(const QString& type) noexcept
{
    return type == QStringLiteral("song.set");
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
        if (!jam2::control_protocol::peerIdFromToken(
                message.value(QStringLiteral("coordinator_token")).toString()).has_value()) {
            reason = QStringLiteral("mesh coordinator token is invalid");
            return false;
        }
        for (const QJsonValue& peerValue : peersValue.toArray()) {
            const QJsonObject peer = peerValue.toObject();
            const auto peerId = jam2::control_protocol::peerIdFromToken(
                peer.value(QStringLiteral("token")).toString());
            bool encodedIdOk = false;
            const quint64 encodedId = peer.value(QStringLiteral("peer_id"))
                .toString().toULongLong(&encodedIdOk);
            if (!peerValue.isObject() ||
                !peerId || !encodedIdOk || encodedId != *peerId ||
                !isBoundedString(peer.value(QStringLiteral("endpoint")), 255)) {
                reason = QStringLiteral("mesh peer entry is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("session.contract")) {
        return isBoundedInteger(
                   message.value(QStringLiteral("protocol_version")),
                   jam2::protocol::kProtocolVersion,
                   jam2::protocol::kProtocolVersion) &&
            jam2::protocol::parse_audio_format(
                message.value(QStringLiteral("audio_format")).toString().toStdString()).has_value() &&
            isBoundedString(message.value(QStringLiteral("profile")), 128) &&
            isBoundedInteger(message.value(QStringLiteral("sample_rate")),
                limits::kMinimumSampleRate, limits::kMaximumSampleRate) &&
            isBoundedInteger(message.value(QStringLiteral("frame_size")), 1, 8192) &&
            jam2::control_protocol::peerIdFromToken(
                message.value(QStringLiteral("coordinator_token")).toString()).has_value()
            ? true : (reason = QStringLiteral("session contract is invalid"), false);
    }
    if (type == QStringLiteral("session.heartbeat") ||
        type == QStringLiteral("session.heartbeat.ack")) {
        return isBoundedInteger(message.value(QStringLiteral("sequence")), 1,
            (std::numeric_limits<int>::max)())
            ? true : (reason = QStringLiteral("heartbeat sequence is invalid"), false);
    }
    if (type == QStringLiteral("session.endpoint.update")) {
        const QString endpoint = message.value(QStringLiteral("udp_endpoint")).toString();
        return !endpoint.isEmpty() && endpoint.size() <= 255 &&
            endpoint.lastIndexOf(QLatin1Char(':')) > 0
            ? true : (reason = QStringLiteral("UDP endpoint update is invalid"), false);
    }
    if (type == QStringLiteral("session.end")) {
        const QJsonValue detail = message.value(QStringLiteral("detail"));
        return detail.isUndefined() || isBoundedString(detail, 512)
            ? true : (reason = QStringLiteral("session end detail is invalid"), false);
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
        return validateRemoteSong(songValue.toObject(), reason);
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
            isSha256Hex(hash) && isBoundedString(message.value(QStringLiteral("name")), 512) &&
            isBoundedInteger(message.value(QStringLiteral("sample_rate")),
                limits::kMinimumSampleRate, limits::kMaximumSampleRate)
            ? true : (reason = QStringLiteral("recording offer is invalid"), false);
    }
    if (type == QStringLiteral("looper.asset.request")) {
        const QJsonValue arrangementRevision =
            message.value(QStringLiteral("arrangement_revision"));
        const QJsonArray hashes = message.value(QStringLiteral("hashes")).toArray();
        if (!message.value(QStringLiteral("hashes")).isArray() ||
            hashes.isEmpty() || hashes.size() > limits::kMaximumAssetRequests ||
            (arrangementRevision.isUndefined() ||
             isBoundedInteger(
                 arrangementRevision, 0, (std::numeric_limits<int>::max)())) == false) {
            reason = QStringLiteral("asset request is invalid");
            return false;
        }
        for (const QJsonValue& hash : hashes) {
            if (!hash.isString() || !isSha256Hex(hash.toString().toLower())) {
                reason = QStringLiteral("asset request hash is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("looper.asset.start")) {
        return isSha256Hex(message.value(QStringLiteral("sha256")).toString().toLower()) &&
            isBoundedInteger(message.value(QStringLiteral("file_bytes")), 44,
                limits::kMaximumAssetBytes) &&
            isBoundedInteger(message.value(QStringLiteral("chunk_size")),
                limits::kMaximumAssetChunkBytes,
                limits::kMaximumAssetChunkBytes)
            ? true : (reason = QStringLiteral("asset start is invalid"), false);
    }
    if (type == QStringLiteral("looper.asset.done")) {
        return isSha256Hex(message.value(QStringLiteral("sha256")).toString().toLower()) &&
            isBoundedInteger(message.value(QStringLiteral("chunks")), 1,
                limits::kMaximumAssetChunks)
            ? true : (reason = QStringLiteral("asset completion is invalid"), false);
    }
    if (type == QStringLiteral("debug.lifecycle.disconnect")) {
        return true;
    }
    reason = QStringLiteral("unknown message type");
    return false;
}

jam2::application::ControlMessageDecision jam2::application::evaluateControlMessage(
    const QJsonObject& message,
    ControlMessageSource source)
{
    ControlMessageDecision decision;
    if (!validateControlMessage(message, decision.reason)) {
        return decision;
    }

    const QString type = message.value(QStringLiteral("type")).toString();
    const bool coordinatorOnly = type == QStringLiteral("session.contract") ||
        type == QStringLiteral("session.membership") ||
        type == QStringLiteral("session.heartbeat") ||
        type == QStringLiteral("session.end") ||
        type == QStringLiteral("looper.track.share.request");
    const bool peerOnly = type == QStringLiteral("session.heartbeat.ack") ||
        type == QStringLiteral("session.endpoint.update");
    const bool internalOnly = type == QStringLiteral("debug.lifecycle.disconnect");

    const bool authorized = internalOnly
        ? source == ControlMessageSource::Internal
        : coordinatorOnly
            ? source == ControlMessageSource::Coordinator ||
                source == ControlMessageSource::LocalCreator
            : peerOnly
                ? source == ControlMessageSource::AuthenticatedPeer ||
                    source == ControlMessageSource::LocalJoiner
                : source != ControlMessageSource::Internal;
    if (!authorized) {
        decision.rejection = ControlMessageRejection::Authorization;
        decision.reason = QStringLiteral("message family is not authorized for its authenticated source");
        return decision;
    }
    decision.accepted = true;
    decision.rejection = ControlMessageRejection::None;
    decision.reason.clear();
    return decision;
}
