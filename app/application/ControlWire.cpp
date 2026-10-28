#include "ControlWire.hpp"
#include "PeerName.hpp"
#include "ContentLimits.hpp"
#include "miniz_tinfl.h"

#include <QCborValue>
#include <QCborStreamReader>
#include <QCborMap>
#include <QCborArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QSet>
#include <QHash>
#include <QStringDecoder>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace jam2::application::control_wire {
namespace {
constexpr qsizetype kChunkBytes = 24 * 1024, kMaximumAssetBytes = 512 * 1024 * 1024;
constexpr qsizetype kPartBytes = 32 * 1024;
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
enum class Type { Bool, U8, U16, U32, I64, F64, Text, Hex16, Hash, TransferHash, Document, FrameText, Settings, Tempo, Hashes, Peers, Participants, Lane, Phase, Scope, Format, Profile, Ideas };
struct Field { const char* name; Type type; };
struct Schema { Operation operation; const char* name; std::vector<Field> fields; QSet<QString> allowed; };

// Positional fields, selected by a schema-sized u8/u16 presence bitmap. No field names or
// dynamic schema travel on the wire. Common authority fields are explicit slots.
const std::vector<Schema>& schemas()
{
    using enum Type;
    using enum Operation;
    static const std::vector<Schema> values = [] {
        std::vector<Schema> s{
            {Contract,"session.contract",{{"protocol_version",U8},{"audio_format",Format},{"profile",Profile},{"sample_rate",U32},{"frame_size",U16},{"coordinator_token",Hex16}}},
            {Membership,"session.membership",{{"page_index",U32},{"page_count",U32},{"coordinator_token",Hex16},{"editor_authority_token",Hex16},{"arrangement_authority_token",Hex16},{"editor_revision",I64},{"arrangement_revision",I64},{"peers",Peers}}},
            {Heartbeat,"session.heartbeat",{{"sequence",U32}}}, {HeartbeatReply,"session.heartbeat.ack",{{"sequence",U32}}},
            {Introduce,"session.udp.introduce",{{"peer",Hex16},{"nonce",Hex16}}}, {Repair,"session.udp.recover",{{"peer",Hex16}}},
            {PairFailure,"session.connection.failure",{{"a",Hex16},{"b",Hex16},{"detail",Text}}},
            {Endpoint,"session.endpoint.update",{{"udp_endpoint",Text}}}, {End,"session.end",{{"detail",Text}}}, {Error,"session.error",{{"message",Text}}},
            {SyncSet,"jam.sync.set",{}}, {SyncRequest,"jam.sync.request",{}},
            {MetronomeSet,"jam.metronome.state.set",{{"enabled",Bool}}}, {MetronomeRequest,"jam.metronome.state.request",{{"enabled",Bool}}},
            {ListeningRequest,"jam.listening.request",{}},
            {ListeningFailed,"jam.listening.failed",{}},
            {ListeningCancel,"jam.listening.cancel",{}},
            {ListeningState,"jam.listening.state",{}}, {ListeningReset,"jam.listening.reset",{}},
            {ListeningApplied,"jam.listening.applied",{{"actual_frame",FrameText}}},
            {Cell,"beat.set",{{"section",U8},{"lane",Lane},{"beat",U16},{"text",Text}}},
            {Hit,"beat.hit",{{"section",U8},{"beat",U16},{"lane",U8},{"text",Text}}},
            {BeatDivision,"beat.division",{{"section",U8},{"beat",U16},{"division",U8}}},
            {MusicDivision,"music.division",{{"section",U8},{"beat",U16},{"division",U8}}},
            {MusicStep,"music.step",{{"section",U8},{"beat",U16},{"step",U8},{"lane",Lane},{"text",Text}}},
            {Resize,"grid.resize",{{"section",U8},{"lane",Lane},{"beats",U16}}},
            {Song,"song.set",{{"song",Document},{"sync_scope",Scope},{"arrangement_revision",I64},{"base_arrangement_revision",I64},{"host_authoritative",Bool},{"track_playing",Bool}}},
            {Render,"practice.references.render",{{"request_id",Hex16},{"render_signature",Hash},{"render_chords",Bool},{"render_drums",Bool},{"render_melody",Bool},{"render_bass",Bool},{"render_support",Bool},{"chord_voicing",U32},{"drum_kit",U32}}},
            {BankRequest,"bank.request",{{"bank",U8},{"target_abs_beat",FrameText}}},
            {BankPrepare,"bank.prepare",{{"bank",U8},{"switch_id",Hex16},{"target_abs_beat",FrameText}}},
            {BankReady,"bank.ready",{{"bank",U8},{"switch_id",Hex16}}}, {BankCancel,"bank.cancel",{{"bank",U8},{"switch_id",Hex16}}},
            {BankSwitch,"bank.switch",{{"bank",U8},{"switch_id",Hex16},{"target_abs_beat",FrameText}}},
            {ShareRequest,"looper.track.share.request",{{"batch_id",Hex16}}},
            {ShareOffer,"looper.track.batch.offer",{{"batch_id",Hex16},{"tracks",Document}}},
            {ShareComplete,"looper.track.batch.complete",{{"batch_id",Hex16},{"tracks",U32},{"applied",Bool},{"reason",Text}}},
            {RecordingState,"looper.recording.state",{{"phase",Phase},{"state_revision",U32},{"bank",U8},{"lane_id",Text},{"lane_name",Text},{"count_in_remaining",U16},{"count_in_bars",U8},{"group_id",Hex16},{"source_peer_token",Hex16}}},
            {GroupStart,"looper.recording.group.start",{{"group_id",Hex16},{"participants",Participants},{"count_in_bars",U8},{"countdown_abs_beat",FrameText},{"target_abs_beat",FrameText}}},
            {GroupFinish,"looper.recording.group.finish",{{"group_id",Hex16}}},
            {GroupRecoverRequest,"looper.recording.group.recover.request",{{"group_id",Hex16}}},
            {GroupRecover,"looper.recording.group.recover",{{"group_id",Hex16}}},
            {ResyncRequest,"looper.recording.resync.request",{}},
            {ResyncState,"looper.recording.resync.state",{{"track_playing",Bool},{"active_bank",U8}}},
            {AssetRequest,"looper.asset.request",{{"hashes",Hashes},{"arrangement_revision",I64}}},
            {AssetStart,"looper.asset.start",{{"sha256",Hash},{"file_bytes",U32},{"chunk_size",U16}}},
            {AssetProgress,"looper.asset.ack",{{"sha256",TransferHash},{"chunks",U16}}},
            {AssetDone,"looper.asset.done",{{"sha256",TransferHash},{"chunks",U16}}},
            {AssetUnavailable,"looper.asset.unavailable",{{"sha256",Hash},{"reason",Text}}},
        };
        for (auto& schema : s) {
            if (schema.operation == SyncSet || schema.operation == SyncRequest) {
                schema.fields = {{"track_lanes",Bool},{"auto_share_wavs",Bool},{"global_playback",Bool},{"listening_tools",Bool},{"generated_ideas",Ideas},{"metronome_state",Bool},{"recordings",Bool}};
            }
            if (schema.operation >= ListeningRequest && schema.operation <= ListeningReset) {
                schema.fields = {{"settings",Settings},{"bank",U8},{"tempo",Tempo},{"target_frame",FrameText},{"epoch_frame",FrameText},{"epoch_position",F64},{"source_seconds",F64},{"playing",Bool},{"reason",Text},{"changed_groups",U8}};
            }
            schema.fields.push_back({"revision",I64});
            schema.fields.push_back({"authority_token",Hex16});
            schema.allowed.insert(QStringLiteral("type"));
            for (const auto& field : schema.fields) schema.allowed.insert(QLatin1String(field.name));
        }
        return s;
    }();
    return values;
}
const Schema* schemaFor(Operation operation)
{
    for (const auto& s : schemas()) if (s.operation == operation) return &s;
    return nullptr;
}
const Schema* schemaFor(const QString& name)
{
    for (const auto& s : schemas()) if (name == QLatin1String(s.name)) return &s;
    return nullptr;
}
bool scalarAssignment(Operation operation)
{
    return operation==Operation::Cell || operation==Operation::MusicStep ||
        operation==Operation::BeatDivision || operation==Operation::MusicDivision;
}

struct Writer {
    QByteArray bytes;
    template<class T> void number(T value) {
        const auto offset = bytes.size(); bytes.resize(offset + sizeof(T));
        qToBigEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
    }
    bool text(const QString& value) {
        const auto utf8 = value.toUtf8();
        if (utf8.size() > 16384) return false;
        number<quint16>(static_cast<quint16>(utf8.size())); bytes += utf8; return true;
    }
    void block(const QByteArray& value) { number<quint32>(static_cast<quint32>(value.size())); bytes += value; }
};
struct Reader {
    const QByteArray& bytes;
    qsizetype at = 0;
    bool okay = true;
    template<class T> T number() {
        if (!okay || at > bytes.size() - static_cast<qsizetype>(sizeof(T))) { okay = false; return {}; }
        const auto result = qFromBigEndian<T>(reinterpret_cast<const uchar*>(bytes.constData() + at));
        at += sizeof(T); return result;
    }
    QByteArray take(qsizetype count) {
        if (!okay || count < 0 || count > bytes.size() - at) { okay = false; return {}; }
        auto result = bytes.mid(at, count); at += count; return result;
    }
    QString text() {
        const auto count = number<quint16>();
        if (count > 16384) { okay = false; return {}; }
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString result = decoder.decode(take(count));
        if (decoder.hasError()) okay = false;
        return result;
    }
    QByteArray block() { const auto count = number<quint32>(); return take(count); }
    bool done() const { return okay && at == bytes.size(); }
};
std::span<const char* const> enumValues(Type type)
{
    static constexpr const char* lanes[]{"chord", "target", "beat", "lyric", "melody", "bass", "support"};
    static constexpr const char* phases[]{"idle", "armed", "ready", "waiting", "count-in", "recording", "complete", "finalizing"};
    static constexpr const char* scopes[]{"tracks", "idea.full", "idea.chords", "idea.beats"};
    static constexpr const char* formats[]{"pcm16-mono", "pcm24-mono"};
    static constexpr const char* profiles[]{"fast", "moderate", "safe"};
    static constexpr const char* ideas[]{"off", "chords", "beats", "full"};
    switch (type) {
    case Type::Lane: return lanes; case Type::Phase: return phases; case Type::Scope: return scopes;
    case Type::Format: return formats; case Type::Profile: return profiles; case Type::Ideas: return ideas;
    default: return {};
    }
}
// Versioned global property IDs. Unknown application-defined recipe properties
// retain a text escape; unknown numeric IDs fail instead of aliasing a field.
const auto& documentKeys()
{
    static constexpr auto keys = std::to_array<const char*>({
        "id", "name", "title", "looper", "banks", "lanes", "sections", "timing", "active_bank", "arrangement",
        "asset_hash", "asset_path", "source_frames", "sample_rate", "start_frame", "stop_frame", "loop_start_frame",
        "loop_end_frame", "loop_enabled", "gain_db", "muted", "solo", "origin_kind", "reference_kind",
        "reference_source_signature", "reference_bpm", "reference_stale", "grid_lock_enabled", "track_sync_enabled",
        "bpm", "beats_per_bar", "beat_unit", "tempo_pulse_units", "division", "play_mask_low", "play_mask_high",
        "accent_mask_low", "accent_mask_high", "inherits_bank_a", "enabled", "loop", "steps", "bank", "repeats",
        "label", "beats", "chords", "targets", "beat_notes", "lyrics", "beat_patterns", "musical_patterns", "drum_kit",
        "generated_kind", "generated_recipe", "state", "value", "velocity", "articulation", "voicing", "melody",
        "bass", "support", "recording_id", "bank_index", "bank_id", "lane_id", "sha256", "file_bytes", "recorded_bpm",
        "bars", "complexity", "style_id", "profile_id", "key", "mode", "meter_id", "harmonic_rhythm", "automation",
        "chord_voicing", "render_signature", "source_signature", "version", "format", "notes", "octave", "duration",
        "length", "probability", "swing", "gate", "accent", "instrument", "role", "phrase", "seed", "pattern"});
    return keys;
}
quint8 documentKeyId(const QString& key)
{
    static const auto ids = [] {
        QHash<QString, quint8> result; quint8 id = 1;
        for (const auto* name : documentKeys()) result.insert(QLatin1String(name), id++);
        return result;
    }();
    return ids.value(key);
}
bool writeDocumentKey(Writer& w, const QString& key, bool numeric)
{
    const auto id = numeric ? documentKeyId(key) : quint8{0};
    w.number<quint8>(id); return id || w.text(key);
}
QString readDocumentKey(Reader& r)
{
    const auto id = r.number<quint8>();
    if (!id) return r.text();
    if (id > documentKeys().size()) { r.okay = false; return {}; }
    return QLatin1String(documentKeys()[id - 1]);
}
bool snapshotFrameKey(const QString& key)
{
    return key == QLatin1String("source_frames") || key == QLatin1String("start_frame") || key == QLatin1String("stop_frame") ||
        key == QLatin1String("loop_start_frame") || key == QLatin1String("loop_end_frame");
}
QCborValue numericDocument(const QJsonValue& value)
{
    if (value.isObject()) {
        QCborMap map; const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            auto id = documentKeyId(it.key());
            auto content = numericDocument(it.value());
            if (snapshotFrameKey(it.key())) {
                bool okay = false; const auto frame = it.value().toString().toLongLong(&okay);
                if (it.value().isString() && okay && QString::number(frame) == it.value().toString()) content = QCborValue(frame);
                else id = 0; // Text-key escape preserves noncanonical/numeric application values.
            }
            map.insert(id ? QCborValue(static_cast<qint64>(id)) : QCborValue(it.key()), content);
        }
        return map;
    }
    if (value.isArray()) { QCborArray array; for (const auto& item : value.toArray()) array.append(numericDocument(item)); return array; }
    return QCborValue::fromJsonValue(value);
}
bool jsonDocument(const QCborValue& wire, QJsonValue& value)
{
    if (wire.isMap()) {
        QJsonObject object; const auto map = wire.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            QString key;
            if (it.key().isInteger()) {
                const auto id = it.key().toInteger();
                if (id <= 0 || id > static_cast<qint64>(documentKeys().size())) return false;
                key = QLatin1String(documentKeys()[static_cast<std::size_t>(id - 1)]);
            } else if (it.key().isString()) key = it.key().toString();
            else return false;
            QJsonValue item;
            if (object.contains(key)) return false;
            if (it.key().isInteger() && snapshotFrameKey(key) && it.value().isInteger()) item = QString::number(it.value().toInteger());
            else if (!jsonDocument(it.value(), item)) return false;
            object.insert(key, item);
        }
        value = object; return true;
    }
    if (wire.isArray()) {
        QJsonArray array; for (const auto& item : wire.toArray()) { QJsonValue decoded; if (!jsonDocument(item, decoded)) return false; array.append(decoded); }
        value = array; return true;
    }
    if (wire.isByteArray() || wire.isTag() || wire.isUndefined()) return false;
    value = wire.toJsonValue(); return !value.isUndefined();
}
bool fixedHex(const QString& text, int size, QByteArray& out)
{
    if (text.size() != size * 2) return false;
    for (const auto ch : text) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    out = QByteArray::fromHex(text.toLatin1()); return out.size() == size;
}
struct PeerHandles {
    QHash<QString,quint16> ids;
    QHash<QString,QString> names;
    QVector<QString> values;
    quint64 definitions=0,references=0;
};
bool peerField(const char* field)
{
    const QByteArrayView name(field);
    return name.endsWith("_token") || name=="peer" || name=="a" || name=="b";
}
bool writePeer(Writer& writer,const QString& text,PeerHandles& handles,bool compact,bool define=true)
{
    if (text.isEmpty()) { writer.number<quint8>(0); return true; }
    QByteArray identity; if (!fixedHex(text,16,identity)) return false;
    const auto canonical=QString::fromLatin1(identity.toHex());
    const auto found=handles.ids.constFind(canonical);
    if (compact && found!=handles.ids.cend()) {
        writer.number<quint8>(2); writer.number<quint16>(*found); ++handles.references; return true;
    }
    // An unsent scalar assignment may be superseded before reaching the wire.
    // It can refer to established mappings but must not introduce dependencies.
    writer.number<quint8>(define ? 1 : 3); writer.bytes+=identity;
    if (define && found==handles.ids.cend() && handles.values.size()<4096) {
        handles.values.append(canonical); handles.ids.insert(canonical,static_cast<quint16>(handles.values.size())); ++handles.definitions;
    }
    return true;
}
bool readPeer(Reader& reader,QString& text,PeerHandles& handles)
{
    const auto marker=reader.number<quint8>();
    if (marker==0) { text.clear(); return reader.okay; }
    if (marker==2) {
        const auto handle=reader.number<quint16>();
        if (!reader.okay || !handle || handle>handles.values.size()) return false;
        text=handles.values[handle-1]; return true;
    }
    if (marker!=1 && marker!=3) return false;
    const auto identity=reader.take(16); if (!reader.okay) return false;
    text=QString::fromLatin1(identity.toHex());
    if (marker==1 && !handles.ids.contains(text) && handles.values.size()<4096) {
        handles.values.append(text); handles.ids.insert(text,static_cast<quint16>(handles.values.size()));
    }
    return true;
}
bool encodeStructured(Type type, const QJsonValue& value, Writer& writer,PeerHandles& handles,bool compact)
{
    if (type == Type::Settings) {
        const auto object = value.toObject();
        if (!value.isObject() || object.size() != 7 || !object.value("focus_enabled").isBool() || !object.value("focus_preset").isString()) return false;
        const auto speed = object.value("speed_ppm").toInteger(-1), pitch = object.value("pitch_cents").toInteger(-99999);
        if (speed < 100000 || speed > 2000000 || pitch < -1200 || pitch > 1200) return false;
        writer.number<qint32>(static_cast<qint32>(speed)); writer.number<qint32>(static_cast<qint32>(pitch));
        const auto preset = object.value("focus_preset").toString();
        writer.number<quint8>((object.value("focus_enabled").toBool() ? 0x80 : 0) | (preset == QStringLiteral("custom") ? 1 : 0));
        if (preset != QStringLiteral("custom") && !writer.text(preset)) return false;
        for (const auto* name : {"focus_hz", "focus_gain_db", "focus_q"}) {
            const auto n = object.value(name);
            if (!n.isDouble() || !std::isfinite(n.toDouble())) return false;
            writer.number<quint64>(std::bit_cast<quint64>(n.toDouble()));
        }
        return true;
    }
    if (type == Type::Tempo) {
        const auto object = value.toObject(); bool okay = false;
        const auto rate = object.value("rate_ppm").toInteger(-1);
        const auto anchor = object.value("anchor_frame").toString().toULongLong(&okay);
        const auto position = object.value("anchor_position");
        if (!value.isObject() || object.size() != 3 || !okay || rate < 100000 || rate > 2000000 || !position.isDouble() || !std::isfinite(position.toDouble()) || position.toDouble() < 0) return false;
        writer.number<qint32>(static_cast<qint32>(rate)); writer.number<quint64>(anchor); writer.number<quint64>(std::bit_cast<quint64>(position.toDouble())); return true;
    }
    if (!value.isArray() || value.toArray().size() > 64) return false;
    const auto array = value.toArray(); writer.number<quint8>(static_cast<quint8>(array.size()));
    for (const auto& entry : array) {
        const auto text = type == Type::Peers ? entry.toObject().value("token").toString() : entry.toString();
        QByteArray bytes;
        if (!fixedHex(text, type == Type::Hashes ? 32 : 16, bytes)) return false;
        if (type==Type::Hashes) writer.bytes+=bytes;
        else if (!writePeer(writer,text,handles,compact)) return false;
        if (type == Type::Peers) {
            const auto object = entry.toObject();
            const auto id = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData()));
            const auto name = PeerName::fromWire(object.value("name").toString().toUtf8());
            if (object.size() != 4 || !name || object.value("peer_id").toString() != QString::number(id) ||
                !object.value("endpoint").isString() || !writer.text(object.value("endpoint").toString())) return false;
            const bool defineName = handles.names.value(text) != name->text();
            writer.number<quint8>(defineName ? static_cast<quint8>(name->utf8().size()) : 0);
            if (defineName) {
                writer.bytes += name->utf8();
                if (handles.ids.contains(text)) handles.names.insert(text,name->text());
            }
        }
    }
    return true;
}
bool decodeStructured(Type type, Reader& r, QJsonValue& value,PeerHandles& handles)
{
    if (type == Type::Settings) {
        const auto speed = r.number<qint32>(), pitch = r.number<qint32>(); const auto flags = r.number<quint8>();
        const bool enabled = flags & 0x80;
        const auto preset = (flags & 0x7f) == 1 ? QStringLiteral("custom") : r.text();
        if ((flags & 0x7f) > 1) r.okay = false;
        const auto hz = std::bit_cast<double>(r.number<quint64>()), gain = std::bit_cast<double>(r.number<quint64>()), q = std::bit_cast<double>(r.number<quint64>());
        if (!r.okay || speed < 100000 || speed > 2000000 || pitch < -1200 || pitch > 1200 || !std::isfinite(hz) || !std::isfinite(gain) || !std::isfinite(q)) return false;
        value = QJsonObject{{"speed_ppm", speed}, {"pitch_cents", pitch}, {"focus_enabled", bool(enabled)}, {"focus_preset", preset}, {"focus_hz", hz}, {"focus_gain_db", gain}, {"focus_q", q}}; return true;
    }
    if (type == Type::Tempo) {
        const auto rate = r.number<qint32>(); const auto anchor = r.number<quint64>(); const auto position = std::bit_cast<double>(r.number<quint64>());
        if (!r.okay || rate < 100000 || rate > 2000000 || !std::isfinite(position) || position < 0) return false;
        value = QJsonObject{{"rate_ppm", rate}, {"anchor_frame", QString::number(anchor)}, {"anchor_position", position}}; return true;
    }
    const auto count = r.number<quint8>(); if (!r.okay || count > 64) return false;
    QJsonArray array;
    for (quint16 i = 0; i < count; ++i) {
        QByteArray bytes; QString text;
        if (type==Type::Hashes) { bytes=r.take(32); text=QString::fromLatin1(bytes.toHex()); }
        else { if (!readPeer(r,text,handles) || text.isEmpty()) return false; bytes=QByteArray::fromHex(text.toLatin1()); }
        if (!r.okay) return false;
        if (type == Type::Peers) {
            const auto id = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData()));
            const auto endpoint = r.text();
            const auto length = r.number<quint8>();
            if (length > PeerName::kMaximumUtf8Bytes) return false;
            auto displayName=handles.names.value(text);
            if (length) {
                const auto name = PeerName::fromWire(r.take(length));
                if (!name) return false;
                displayName=name->text();
                if (handles.ids.contains(text)) handles.names.insert(text,displayName);
            }
            if (displayName.isEmpty()) return false;
            array.append(QJsonObject{{"token", text}, {"peer_id", QString::number(id)}, {"endpoint", endpoint}, {"name",displayName}});
        } else array.append(text);
    }
    value = array; return r.okay;
}

// Check nesting/item count before constructing any potentially nested Qt value.
bool cborBounded(QCborStreamReader& reader, int depth, qsizetype& items)
{
    if (depth > 32 || ++items > 1024 * 1024) return false;
    if (reader.isContainer()) {
        if (!reader.enterContainer()) return false;
        while (reader.hasNext()) if (!cborBounded(reader, depth + 1, items)) return false;
        return reader.leaveContainer();
    }
    if (reader.isByteArray() || reader.isString()) {
        if (reader.isByteArray()) {
            while (reader.readByteArray().status == QCborStreamReader::Ok) {}
        } else {
            while (reader.readString().status == QCborStreamReader::Ok) {}
        }
        return reader.lastError() == QCborError::NoError;
    }
    return reader.next();
}
bool readDocument(const QByteArray& bytes, QJsonValue& out)
{
    if (bytes.isEmpty() || bytes.size() > kMaximumDocumentBytes) return false;
    QCborStreamReader reader(bytes);
    qsizetype items = 0;
    if (!cborBounded(reader, 0, items) || reader.currentOffset() != bytes.size()) return false;
    QCborParserError error;
    const auto value = QCborValue::fromCbor(bytes, &error);
    if (error.error != QCborError::NoError) return false;
    return jsonDocument(value, out);
}
QByteArray documentBytes(const QJsonValue& value, bool numeric = true) { return (numeric ? numericDocument(value) : QCborValue::fromJsonValue(value)).toCbor(); }

// The owned manifest fields have a fixed schema; unknown extension fields keep
// the bounded document representation rather than being silently discarded.
enum class ManifestType { Uuid, Bank, Text, Hash, OptionalHash, U32, Frame, Bool, F64 };
struct ManifestField { const char* name; ManifestType type; };
constexpr std::array<ManifestField,16> manifestFields{{
    {"recording_id",ManifestType::Uuid},{"bank",ManifestType::Bank},{"target_lane_id",ManifestType::Text},
    {"sha256",ManifestType::Hash},{"name",ManifestType::Text},{"sample_rate",ManifestType::U32},
    {"source_frames",ManifestType::Frame},{"start_frame",ManifestType::Frame},{"stop_frame",ManifestType::Frame},
    {"loop_start_frame",ManifestType::Frame},{"loop_end_frame",ManifestType::Frame},{"loop_enabled",ManifestType::Bool},
    {"reference_kind",ManifestType::Text},{"reference_source_signature",ManifestType::OptionalHash},
    {"reference_bpm",ManifestType::F64},{"reference_stale",ManifestType::Bool}}};
QByteArray typedManifest(const QJsonValue& value)
{
    if (!value.isArray() || value.toArray().size() > limits::kMaximumLooperBankCount * limits::kMaximumLooperLanesPerBank) return {};
    Writer out; const auto entries=value.toArray(); out.number<quint16>(static_cast<quint16>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.isObject()) return {};
        const auto object=entry.toObject(); Writer fields; quint16 mask=0; quint8 flags=0;
        int found=0;
        for (std::size_t i=0;i<manifestFields.size();++i) {
            const auto& f=manifestFields[i]; const auto item=object.value(QLatin1String(f.name));
            if (item.isUndefined()) continue;
            ++found; mask|=quint16{1}<<i;
            switch (f.type) {
            case ManifestType::Uuid: {
                if (!item.isString()) return {};
                const auto uuid=QUuid::fromString(item.toString());
                if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces)!=item.toString()) return {};
                fields.bytes+=uuid.toRfc4122(); break;
            }
            case ManifestType::Hash: case ManifestType::OptionalHash: {
                if (!item.isString()) return {};
                const auto text=item.toString(); QByteArray hash;
                if (f.type==ManifestType::OptionalHash) fields.number<quint8>(!text.isEmpty());
                if (!text.isEmpty() || f.type==ManifestType::Hash) {
                    if (!fixedHex(text,32,hash) || QString::fromLatin1(hash.toHex())!=text) return {};
                    fields.bytes+=hash;
                }
                break;
            }
            case ManifestType::Text: if (!item.isString() || !fields.text(item.toString())) return {}; break;
            case ManifestType::Bank: case ManifestType::U32: {
                const auto n=item.toInteger(-1);
                if (!item.isDouble() || n<0 || item.toDouble()!=static_cast<double>(n) ||
                    (f.type==ManifestType::Bank ? n>=limits::kMaximumLooperBankCount : n>UINT32_MAX)) return {};
                if (f.type==ManifestType::Bank) fields.number<quint8>(static_cast<quint8>(n));
                else fields.number<quint32>(static_cast<quint32>(n)); break;
            }
            case ManifestType::Frame: {
                bool okay=false; const auto n=item.toString().toLongLong(&okay);
                if (!item.isString() || !okay || QString::number(n)!=item.toString() || n < -1) return {};
                fields.number<qint64>(n); break;
            }
            case ManifestType::Bool:
                if (!item.isBool()) return {};
                if (item.toBool()) flags|=i==11 ? 1 : 2;
                break;
            case ManifestType::F64:
                if (!item.isDouble() || !std::isfinite(item.toDouble())) return {};
                fields.number<quint64>(std::bit_cast<quint64>(item.toDouble())); break;
            }
        }
        if (found!=object.size()) return {};
        out.number<quint16>(mask); out.number<quint8>(flags); out.bytes+=fields.bytes;
        if (out.bytes.size()>kMaximumDocumentBytes) return {};
    }
    return out.bytes;
}
bool readManifest(const QByteArray& bytes,QJsonValue& value)
{
    Reader r{bytes}; const auto count=r.number<quint16>();
    if (!r.okay || count>limits::kMaximumLooperBankCount * limits::kMaximumLooperLanesPerBank) return false;
    QJsonArray entries;
    for (unsigned entry=0;entry<count;++entry) {
        const auto mask=r.number<quint16>(); const auto flags=r.number<quint8>(); QJsonObject object;
        if (flags>3 || ((flags&1) && !(mask&(1<<11))) || ((flags&2) && !(mask&(1<<15)))) return false;
        for (std::size_t i=0;i<manifestFields.size();++i) {
            if (!(mask&(quint16{1}<<i))) continue;
            const auto& f=manifestFields[i]; QJsonValue item;
            switch (f.type) {
            case ManifestType::Uuid: item=QUuid::fromRfc4122(r.take(16)).toString(QUuid::WithoutBraces); break;
            case ManifestType::Hash: item=QString::fromLatin1(r.take(32).toHex()); break;
            case ManifestType::OptionalHash: {
                const auto present=r.number<quint8>(); if (present>1) return false;
                item=present ? QString::fromLatin1(r.take(32).toHex()) : QString{}; break;
            }
            case ManifestType::Text: item=r.text(); break;
            case ManifestType::Bank: { const auto bank=r.number<quint8>(); if (bank>=limits::kMaximumLooperBankCount) return false; item=bank; break; }
            case ManifestType::U32: item=static_cast<qint64>(r.number<quint32>()); break;
            case ManifestType::Frame: { const auto n=r.number<qint64>(); if (n < -1) return false; item=QString::number(n); break; }
            case ManifestType::Bool: item=bool(flags & (i==11 ? 1 : 2)); break;
            case ManifestType::F64: { const auto n=std::bit_cast<double>(r.number<quint64>()); if (!std::isfinite(n)) return false; item=n; break; }
            }
            if (!r.okay) return false;
            object.insert(QLatin1String(f.name),item);
        }
        entries.append(object);
    }
    value=entries; return r.done();
}

// A typed rename within an already bound snapshot. Indices are safe only inside
// the checked base serial; this never resolves a stale proposal by array index.
bool applyRename(const QJsonValue& base, quint16 bankIndex, quint16 laneIndex, const QString& name, QJsonValue& out)
{
    auto song = base.toObject(); auto looper = song.value("looper").toObject();
    auto banks = looper.value("banks").toArray();
    if (bankIndex >= banks.size()) return false;
    auto bank = banks[bankIndex].toObject(); auto lanes = bank.value("lanes").toArray();
    if (laneIndex >= lanes.size() || !lanes[laneIndex].isObject()) return false;
    auto lane = lanes[laneIndex].toObject(); lane["name"] = name; lanes[laneIndex] = lane;
    bank["lanes"] = lanes; banks[bankIndex] = bank; looper["banks"] = banks; song["looper"] = looper; out = song;
    return true;
}
QByteArray typedRename(const QJsonValue& base, const QJsonValue& value)
{
    if (!base.isObject() || !value.isObject()) return {};
    const auto banks = value.toObject().value("looper").toObject().value("banks").toArray();
    const auto oldBanks = base.toObject().value("looper").toObject().value("banks").toArray();
    if (banks.size() != oldBanks.size() || banks.size() > UINT16_MAX) return {};
    for (qsizetype b = 0; b < banks.size(); ++b) {
        if (banks[b] == oldBanks[b]) continue;
        const auto lanes = banks[b].toObject().value("lanes").toArray();
        const auto oldLanes = oldBanks[b].toObject().value("lanes").toArray();
        if (lanes.size() != oldLanes.size() || lanes.size() > UINT16_MAX) return {};
        for (qsizetype l = 0; l < lanes.size(); ++l) {
            if (lanes[l] == oldLanes[l]) continue;
            const auto name = lanes[l].toObject().value("name"); QJsonValue reconstructed;
            if (!name.isString() || !applyRename(base, b, l, name.toString(), reconstructed) || reconstructed != value) return {};
            Writer w; w.number<quint8>(4); w.number<quint16>(b); w.number<quint16>(l);
            if (!w.text(name.toString())) return {};
            return w.bytes;
        }
        return {};
    }
    return {};
}

QJsonArray bankLanes(const QJsonValue& song,qsizetype bank)
{
    const auto banks=song.toObject().value("looper").toObject().value("banks").toArray();
    return bank>=0 && bank<banks.size() ? banks[bank].toObject().value("lanes").toArray() : QJsonArray{};
}
bool replaceLanes(const QJsonValue& base,quint16 bankIndex,const QJsonArray& lanes,QJsonValue& out)
{
    auto song=base.toObject(); auto looper=song.value("looper").toObject(); auto banks=looper.value("banks").toArray();
    if (bankIndex>=banks.size() || lanes.size()>limits::kMaximumLooperLanesPerBank) return false;
    auto bank=banks[bankIndex].toObject(); bank["lanes"]=lanes; banks[bankIndex]=bank;
    looper["banks"]=banks; song["looper"]=looper; out=song; return true;
}
constexpr std::array<const char*,4> regionFields{{"start_frame","stop_frame","loop_start_frame","loop_end_frame"}};
bool applyRegion(const QJsonValue& base,quint16 bank,quint16 laneIndex,
    const std::array<qint64,4>& frames,bool loop,QJsonValue& out)
{
    auto lanes=bankLanes(base,bank);
    if (laneIndex>=lanes.size() || !lanes[laneIndex].isObject()) return false;
    if (frames[0]<0 || frames[0]>limits::kMaximumLooperTimelineFrames ||
        (frames[1]!=-1 && (frames[1]<=frames[0] || frames[1]>limits::kMaximumLooperTimelineFrames))) return false;
    const bool unset=frames[2]==-1 && frames[3]==-1;
    const bool crop=frames[2]>=0 && frames[3]>frames[2] && frames[3]<=limits::kMaximumAssetFrames;
    auto lane=lanes[laneIndex].toObject();
    const auto source=lane.value("source_frames").toString().toLongLong();
    if ((!unset && !crop) || (crop && source>0 && frames[3]>source)) return false;
    for (std::size_t i=0;i<frames.size();++i) lane[QLatin1String(regionFields[i])]=QString::number(frames[i]);
    lane["loop_enabled"]=loop; lanes[laneIndex]=lane;
    return replaceLanes(base,bank,lanes,out);
}
QByteArray typedLaneEdit(const QJsonValue& base,const QJsonValue& value,bool region,bool structure)
{
    const auto banks=value.toObject().value("looper").toObject().value("banks").toArray();
    const auto oldBanks=base.toObject().value("looper").toObject().value("banks").toArray();
    if (!base.isObject() || !value.isObject() || banks.size()!=oldBanks.size() || banks.size()>limits::kMaximumLooperBankCount) return {};
    for (qsizetype b=0;b<banks.size();++b) {
        if (banks[b]==oldBanks[b]) continue;
        const auto lanes=bankLanes(value,b),old=bankLanes(base,b); Writer w; QJsonValue reconstructed;
        if (lanes.size()>limits::kMaximumLooperLanesPerBank || old.size()>limits::kMaximumLooperLanesPerBank) return {};
        if (structure && lanes.size()!=old.size()) {
            qsizetype prefix=0,suffix=0;
            while (prefix<std::min(lanes.size(),old.size()) && lanes[prefix]==old[prefix]) ++prefix;
            while (suffix<std::min(lanes.size(),old.size())-prefix && lanes[lanes.size()-suffix-1]==old[old.size()-suffix-1]) ++suffix;
            if (!replaceLanes(base,static_cast<quint16>(b),lanes,reconstructed) || reconstructed!=value) return {};
            QJsonArray inserted; for (auto i=prefix;i<lanes.size()-suffix;++i) inserted.append(lanes[i]);
            w.number<quint8>(6); w.number<quint16>(static_cast<quint16>(b)); w.number<quint16>(static_cast<quint16>(prefix));
            w.number<quint16>(static_cast<quint16>(old.size()-prefix-suffix)); w.block(documentBytes(inserted)); return w.bytes;
        }
        if (!region || lanes.size()!=old.size()) return {};
        for (qsizetype l=0;l<lanes.size();++l) {
            if (lanes[l]==old[l]) continue;
            const auto lane=lanes[l].toObject(); std::array<qint64,4> frames{};
            for (std::size_t i=0;i<frames.size();++i) {
                bool okay=false; const auto item=lane.value(QLatin1String(regionFields[i])); frames[i]=item.toString().toLongLong(&okay);
                if (!item.isString() || !okay || QString::number(frames[i])!=item.toString()) return {};
            }
            if (!lane.value("loop_enabled").isBool() || !applyRegion(base,b,l,frames,lane.value("loop_enabled").toBool(),reconstructed) || reconstructed!=value) return {};
            w.number<quint8>(5); w.number<quint16>(static_cast<quint16>(b)); w.number<quint16>(static_cast<quint16>(l));
            w.number<quint8>(lane.value("loop_enabled").toBool()); for (const auto n : frames) w.number<qint64>(n); return w.bytes;
        }
        return {};
    }
    return {};
}

// Explicit recursive patch tree: unchanged=0, replace=1, object edits=2,
// same-length array edits=3. Array insert/removal uses a replacement so indices
// cannot accidentally refer to a different object. A stream-local base serial
// guards every patch. This is a serialization delta, not authority reconciliation.
QByteArray delta(const QJsonValue& base, const QJsonValue& value, int depth = 0, bool numeric = true)
{
    Writer w;
    if (base == value) { w.number<quint8>(0); return w.bytes; }
    const auto replacement = [&] { Writer full; full.number<quint8>(1); full.block(documentBytes(value, numeric)); return full.bytes; };
    if (depth >= 24) return replacement();
    Writer changes;
    if (base.isObject() && value.isObject()) {
        const auto a = base.toObject(), b = value.toObject();
        QSet<QString> keys;
        for (auto it = a.begin(); it != a.end(); ++it) keys.insert(it.key());
        for (auto it = b.begin(); it != b.end(); ++it) keys.insert(it.key());
        auto ordered = keys.values(); std::sort(ordered.begin(), ordered.end());
        quint32 count = 0;
        for (const auto& key : ordered) {
            if (a.value(key) == b.value(key)) continue;
            if (!writeDocumentKey(changes, key, numeric)) return replacement();
            changes.number<quint8>(b.contains(key) ? 1 : 0);
            if (b.contains(key)) changes.block(delta(a.value(key), b.value(key), depth + 1, numeric));
            ++count;
        }
        Writer candidate; candidate.number<quint8>(2); candidate.number<quint32>(count); candidate.bytes += changes.bytes;
        if (count <= 2 && candidate.bytes.size() <= 256) return candidate.bytes;
        const auto full = replacement(); return candidate.bytes.size() < full.size() ? candidate.bytes : full;
    } else if (base.isArray() && value.isArray() && base.toArray().size() == value.toArray().size()) {
        const auto a = base.toArray(), b = value.toArray();
        quint32 count = 0;
        for (qsizetype i = 0; i < a.size(); ++i) {
            if (a[i] == b[i]) continue;
            changes.number<quint32>(static_cast<quint32>(i)); changes.block(delta(a[i], b[i], depth + 1, numeric)); ++count;
        }
        Writer candidate; candidate.number<quint8>(3); candidate.number<quint32>(count); candidate.bytes += changes.bytes;
        if (count <= 2 && candidate.bytes.size() <= 256) return candidate.bytes;
        const auto full = replacement(); return candidate.bytes.size() < full.size() ? candidate.bytes : full;
    }
    return replacement();
}
bool applyDelta(const QJsonValue& base, const QByteArray& bytes, QJsonValue& out, int depth = 0)
{
    if (depth > 24) return false;
    Reader r{bytes}; const auto kind = r.number<quint8>();
    if (kind == 0) { out = base; return r.done(); }
    if (kind == 1) { const auto content = r.block(); return r.done() && readDocument(content, out); }
    if (kind == 4) {
        const auto bank = r.number<quint16>(), lane = r.number<quint16>(); const auto name = r.text();
        return r.done() && applyRename(base, bank, lane, name, out);
    }
    if (kind == 5) {
        const auto bank=r.number<quint16>(),lane=r.number<quint16>(); const auto loop=r.number<quint8>();
        std::array<qint64,4> frames{}; for (auto& n : frames) n=r.number<qint64>();
        return r.done() && loop<=1 && applyRegion(base,bank,lane,frames,loop!=0,out);
    }
    if (kind == 6) {
        const auto bank=r.number<quint16>(),prefix=r.number<quint16>(),remove=r.number<quint16>();
        QJsonValue inserted; const auto content=r.block(); auto lanes=bankLanes(base,bank);
        if (!r.done() || !readDocument(content,inserted) || !inserted.isArray() || prefix>lanes.size() || remove>lanes.size()-prefix ||
            lanes.size()-remove+inserted.toArray().size()>limits::kMaximumLooperLanesPerBank) return false;
        for (unsigned i=0;i<remove;++i) lanes.removeAt(prefix);
        qsizetype at=prefix; for (const auto& item : inserted.toArray()) { if (!item.isObject()) return false; lanes.insert(at++,item); }
        return replaceLanes(base,bank,lanes,out);
    }
    const auto count = r.number<quint32>();
    if (!r.okay || count > 65536) return false;
    if (kind == 2 && base.isObject()) {
        auto object = base.toObject(); QSet<QString> seen;
        for (quint32 i = 0; i < count && r.okay; ++i) {
            const auto key = readDocumentKey(r); const auto present = r.number<quint8>();
            if (!r.okay || seen.contains(key) || present > 1) return false;
            seen.insert(key);
            if (!present) object.remove(key);
            else { QJsonValue value; const auto patch = r.block(); if (!applyDelta(object.value(key), patch, value, depth + 1)) return false; object.insert(key, value); }
        }
        out = object; return r.done();
    }
    if (kind == 3 && base.isArray()) {
        auto array = base.toArray(); quint32 previous = 0;
        for (quint32 i = 0; i < count && r.okay; ++i) {
            const auto index = r.number<quint32>(); const auto patch = r.block();
            if (index >= static_cast<quint32>(array.size()) || (i && index <= previous)) return false;
            QJsonValue value; if (!applyDelta(array[index], patch, value, depth + 1)) return false;
            array.replace(index, value); previous = index;
        }
        out = array; return r.done();
    }
    return false;
}
bool isUuidField(const char* field)
{
    return QByteArrayView(field).endsWith("_id");
}
bool requiresReceipt(Operation operation)
{
    using enum Operation;
    switch (operation) {
    case Heartbeat: case HeartbeatReply: case ListeningFailed: case ListeningApplied:
    case ListeningCancel: case BankReady: case BankCancel: case RecordingState:
    case AssetProgress: case AssetChunk: case Binary: case Receipt:
        return false;
    default: return true;
    }
}
bool independentOperation(Operation operation)
{
    // Replies reference explicit transaction IDs/revisions and are checked by
    // their existing owner. Commands and model/policy edits cannot overtake.
    using enum Operation;
    switch (operation) {
    case Heartbeat: case HeartbeatReply: case ListeningFailed:
    case ListeningApplied: case BankReady: case RecordingState: return true;
    default: return false;
    }
}
}

struct Codec::State {
    Options options;
    QJsonValue sentSong, receivedSong;
    quint64 sentSongSerial = 0, receivedSongSerial = 0;
    quint64 sentSongUpperBound = 0, receivedSongUpperBound = 0, checkpointBytes = 0;
    quint32 nextTransfer = 1;
    struct Transfer { quint32 handle = 0; QByteArray hash; quint64 bytes = 0; quint32 chunk = 0; };
    Transfer outgoing, incoming;
    Stats stats;
    PeerHandles sentPeers,receivedPeers;
};
Codec::Codec() : Codec(Options{}) {}
Codec::Codec(Options options) : state_(std::make_unique<State>()) { state_->options = options; }
Codec::~Codec() = default;
const Stats& Codec::stats() const noexcept { return state_->stats; }

bool Codec::encode(const QJsonObject& message, Encoded& out, QString& error, qsizetype maximumWireBytes)
{
    out = {};
    const auto* schema = schemaFor(message.value("type").toString());
    if (!schema) { error = QStringLiteral("unsupported Server control operation"); return false; }
    Writer fields; quint32 mask = 0, booleans = 0; int booleanCount = 0;
    QJsonValue nextSong(QJsonValue::Undefined); quint64 nextSongSerial = state_->sentSongSerial;
    quint64 nextSongUpperBound = state_->sentSongUpperBound, nextCheckpointBytes = state_->checkpointBytes;
    auto nextPeers=state_->sentPeers;
    quint8 nextPatchKind=0; bool nextManifest=false,nextManifestFallback=false;
    auto nextTransfer = state_->outgoing;
    for (std::size_t index = 0; index < schema->fields.size(); ++index) {
        const auto& field = schema->fields[index];
        const auto value = message.value(QLatin1String(field.name));
        if (value.isUndefined()) continue;
        mask |= quint32{1} << index;
        switch (field.type) {
        case Type::Bool:
            if (!value.isBool()) { error = QStringLiteral("Server boolean field is invalid"); return false; }
            if (value.toBool()) booleans |= quint32{1} << booleanCount;
            ++booleanCount; break;
        case Type::U8: case Type::U16: case Type::U32: {
            const auto n = value.toDouble(-1);
            const auto maximum = field.type == Type::U8 ? UINT8_MAX : field.type == Type::U16 ? UINT16_MAX : UINT32_MAX;
            if (!value.isDouble() || !std::isfinite(n) || n < 0 || n > maximum || std::floor(n) != n) { error = QStringLiteral("Server unsigned integer field is invalid"); return false; }
            if (field.type == Type::U8) fields.number<quint8>(static_cast<quint8>(n));
            else if (field.type == Type::U16) fields.number<quint16>(static_cast<quint16>(n));
            else fields.number<quint32>(static_cast<quint32>(n));
            break;
        }
        case Type::I64:
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble()) { error = QStringLiteral("Server integer field is invalid"); return false; }
            if (static_cast<double>(value.toInteger(INT64_MIN)) != value.toDouble()) { error = QStringLiteral("Server signed integer is outside i64"); return false; }
            fields.number<qint64>(value.toInteger()); break;
        case Type::F64:
            if (!value.isDouble() || !std::isfinite(value.toDouble())) { error = QStringLiteral("Server numeric field is invalid"); return false; }
            fields.number<quint64>(std::bit_cast<quint64>(value.toDouble())); break;
        case Type::Text:
            if (!value.isString() || !fields.text(value.toString())) { error = QStringLiteral("Server text field exceeds its UTF-8 bound"); return false; }
            break;
        case Type::Lane: case Type::Phase: case Type::Scope: case Type::Format: case Type::Profile: case Type::Ideas: {
            const auto values = enumValues(field.type); quint8 id = 1;
            if (!value.isString()) { error = QStringLiteral("Server enum is not text at the application boundary"); return false; }
            for (const auto* text : values) { if (value.toString() == QLatin1String(text)) break; ++id; }
            if (id > values.size()) { error = QStringLiteral("Server enum value is unknown"); return false; }
            fields.number<quint8>(id); break;
        }
        case Type::Hex16:
        case Type::Hash: {
            if (field.type==Type::Hex16 && peerField(field.name)) {
                if (!value.isString() || !writePeer(fields,value.toString(),nextPeers,state_->options.peerHandles,!scalarAssignment(schema->operation))) { error=QStringLiteral("Server peer identity is invalid"); return false; }
                break;
            }
            const auto text = value.toString(); const auto size = field.type == Type::Hash ? 32 : 16;
            QByteArray bytes;
            // Empty optional IDs/tokens retain their distinction from absence.
            if (!value.isString() || (field.type == Type::Hash && text.isEmpty())) { error = QStringLiteral("Server identity field is invalid"); return false; }
            if (field.type == Type::Hex16) fields.number<quint8>(text.isEmpty() ? 0 : 1);
            if (!text.isEmpty()) {
                auto hex = text;
                if (isUuidField(field.name)) {
                    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') { error = QStringLiteral("Server UUID syntax is invalid"); return false; }
                    hex.remove(QLatin1Char('-'));
                }
                if (!fixedHex(hex, size, bytes)) { error = QStringLiteral("Control binary identity is invalid"); return false; }
                fields.bytes += bytes;
            }
            break;
        }
        case Type::TransferHash: {
            const auto& transfer = schema->operation == Operation::AssetProgress ? state_->incoming : state_->outgoing;
            if (!transfer.handle || !value.isString() || value.toString() != QString::fromLatin1(transfer.hash.toHex())) { error = QStringLiteral("Server asset metadata has no matching transfer binding"); return false; }
            fields.number<quint32>(transfer.handle); break;
        }
        case Type::FrameText: {
            bool okay = false; const auto text = value.toString(); const auto n = text.toULongLong(&okay);
            if (!value.isString() || (!text.isEmpty() && (!okay || n > static_cast<quint64>(INT64_MAX)))) { error = QStringLiteral("Server frame is invalid"); return false; }
            fields.number<quint8>(!text.isEmpty()); if (!text.isEmpty()) fields.number<quint64>(n); break;
        }
        case Type::Document: {
            QByteArray bytes;
            if (schema->operation == Operation::Song && QByteArrayView(field.name) == "song") {
                if (!value.isObject() || state_->sentSongSerial == UINT64_MAX) { error = QStringLiteral("Server song identity exhausted or invalid"); return false; }
                Writer doc; nextSongSerial = state_->sentSongSerial + 1;
                doc.number<quint64>(state_->sentSongSerial); doc.number<quint64>(nextSongSerial);
                auto patch = state_->options.typedRenames ? typedRename(state_->sentSong, value) : QByteArray{};
                if (patch.isEmpty()) patch=typedLaneEdit(state_->sentSong,value,state_->options.typedRegions,state_->options.typedStructures);
                if (patch.isEmpty()) patch = delta(state_->sentSong, value, 0, state_->options.numericDocuments);
                if (!patch.isEmpty()) nextPatchKind=static_cast<quint8>(patch[0]);
                nextSongUpperBound = state_->sentSongUpperBound + static_cast<quint64>(patch.size());
                if (!patch.isEmpty() && patch[0] == 1) {
                    nextCheckpointBytes = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(patch.constData() + 1));
                    nextSongUpperBound = nextCheckpointBytes;
                }
                if (nextSongUpperBound > kMaximumDocumentBytes) nextSongUpperBound = documentBytes(value, state_->options.numericDocuments).size();
                if (nextSongUpperBound > kMaximumDocumentBytes) { error = QStringLiteral("Server expanded song exceeds 4 MiB"); return false; }
                doc.bytes += patch; bytes = doc.bytes; nextSong = value;
            } else bytes = documentBytes(value, state_->options.numericDocuments);
            if (bytes.size() > kMaximumDocumentBytes) { error = QStringLiteral("Server document exceeds 4 MiB"); return false; }
            QByteArray compressed;
            if (bytes.size() >= state_->options.compressionMinimumBytes) { compressed = qCompress(bytes, 1); ++state_->stats.compressionAttempts; }
            else ++state_->stats.compressionSkipped;
            bool pack = !compressed.isEmpty() && compressed.size() + 1 < bytes.size();
            QByteArray content=pack ? compressed : bytes;
            if (schema->operation==Operation::ShareOffer && QByteArrayView(field.name)=="tracks") {
                const auto typed=state_->options.typedManifests ? typedManifest(value) : QByteArray{};
                if (!typed.isEmpty()) {
                    QByteArray packed;
                    if (typed.size()>=state_->options.compressionMinimumBytes) { packed=qCompress(typed,1); ++state_->stats.compressionAttempts; }
                    else ++state_->stats.compressionSkipped;
                    const bool compress=!packed.isEmpty() && packed.size()+1<typed.size();
                    const auto candidate=compress ? packed : typed;
                    if (candidate.size()<content.size()) { content=candidate; pack=compress; nextManifest=true; }
                }
                nextManifestFallback=!nextManifest;
            }
            fields.number<quint8>((pack ? 1 : 0) | (nextManifest ? 2 : 0)); fields.block(content); break;
        }
        case Type::Settings: case Type::Tempo: case Type::Hashes: case Type::Peers: case Type::Participants:
            if (!encodeStructured(field.type, value, fields,nextPeers,state_->options.peerHandles)) { error = QStringLiteral("Server structured field violates its fixed schema"); return false; }
            break;
        }
    }
    for (auto it = message.begin(); it != message.end(); ++it) {
        if (!schema->allowed.contains(it.key())) { error = QStringLiteral("unsupported field for Control operation: ") + it.key(); return false; }
    }
    if (schema->operation == Operation::AssetStart) {
        if (state_->nextTransfer == 0 || message.value("chunk_size").toInt() != kChunkBytes || message.value("file_bytes").toInteger() < 44 || message.value("file_bytes").toInteger() > kMaximumAssetBytes) { error = QStringLiteral("Server asset start is invalid"); return false; }
        nextTransfer = {state_->nextTransfer, QByteArray::fromHex(message.value("sha256").toString().toLatin1()), static_cast<quint64>(message.value("file_bytes").toInteger()), static_cast<quint32>(kChunkBytes)};
        if (nextTransfer.hash.size() != 32) return false;
        fields.number<quint32>(nextTransfer.handle);
    }
    Writer payload;
    if (schema->fields.size() <= 8) payload.number<quint8>(static_cast<quint8>(mask));
    else payload.number<quint16>(static_cast<quint16>(mask));
    for (int offset = 0; offset < booleanCount; offset += 8) payload.number<quint8>(static_cast<quint8>(booleans >> offset));
    payload.bytes += fields.bytes;
    if (payload.bytes.size() > kMaximumDocumentBytes) { error = QStringLiteral("Control operation exceeds its bound"); return false; }
    out = {schema->operation, payload.bytes};
    const auto parts = std::max<qsizetype>(1, (out.payload.size() + kPartBytes - 1) / kPartBytes);
    const auto required = out.payload.size() + parts * (parts > 1 ? 40 : 32);
    if (required > maximumWireBytes) {
        out.capacityExceeded = true; error = QStringLiteral("Server application output queue is full"); return false;
    }
    if (!nextSong.isUndefined()) {
        if (state_->sentSongSerial == 0) ++state_->stats.checkpointsSent;
        else ++state_->stats.deltasSent;
        if (nextCheckpointBytes > static_cast<quint64>(payload.bytes.size())) state_->stats.savedSnapshotBytes += nextCheckpointBytes - payload.bytes.size();
        state_->sentSong = nextSong; state_->sentSongSerial = nextSongSerial;
        state_->sentSongUpperBound = nextSongUpperBound; state_->checkpointBytes = nextCheckpointBytes;
    }
    if (schema->operation == Operation::AssetStart) { state_->outgoing = nextTransfer; ++state_->nextTransfer; }
    if (schema->operation == Operation::AssetProgress || schema->operation == Operation::AssetDone) state_->stats.assetMetadataBytesSaved += 30;
    if (nextManifest) ++state_->stats.typedManifests;
    if (nextManifestFallback) ++state_->stats.manifestDocumentFallbacks;
    if (nextPatchKind==5) ++state_->stats.typedRegions;
    if (nextPatchKind==6) ++state_->stats.typedStructures;
    state_->sentPeers=std::move(nextPeers); state_->stats.peerDefinitions=state_->sentPeers.definitions; state_->stats.peerReferences=state_->sentPeers.references;
    return true;
}

bool Codec::encodeAsset(const QByteArray& chunk, Encoded& out, QString& error)
{
    const auto& t = state_->outgoing;
    if (chunk.size() <= 48 || chunk.size() > 48 + kChunkBytes || t.handle == 0 || chunk.first(32) != t.hash) { error = QStringLiteral("Typed WAV chunk has no matching transfer"); return false; }
    Reader r{chunk}; r.take(32); const auto index = r.number<quint32>(); const auto offset = r.number<quint64>(); const auto length = r.number<quint32>();
    if (!r.okay || index > kMaximumAssetBytes / kChunkBytes || offset != quint64{index} * t.chunk || offset >= t.bytes || length != static_cast<quint32>(std::min<quint64>(t.chunk, t.bytes - offset)) || chunk.size() != 48 + length) { error = QStringLiteral("Typed WAV chunk index, size or offset is invalid"); return false; }
    Writer w; w.number<quint32>(t.handle); w.number<quint16>(static_cast<quint16>(index)); w.bytes += chunk.sliced(48);
    out = {Operation::AssetChunk, w.bytes}; state_->stats.assetHeaderBytesSaved += 42;
    return true;
}

bool Codec::decode(Operation operation, const QByteArray& bytes, Decoded& out, QString& error)
{
    out = {};
    Reader r{bytes};
    if (operation == Operation::Binary) {
        if (bytes.isEmpty() || bytes.size() > 65536) { error = QStringLiteral("Control diagnostic binary size is invalid"); return false; }
        out.asset = bytes; return true;
    }
    if (operation == Operation::Receipt) {
        out.id = r.number<quint64>(); const auto stage = r.number<quint8>(); out.receipt = true;
        if (stage == static_cast<quint8>(ReceiptStage::Rejected)) {
            out.stage = ReceiptStage::Rejected; out.rejection = static_cast<Rejection>(r.number<quint8>()); out.reason = r.text();
            if (out.id && out.rejection >= Rejection::InvalidValue && out.rejection <= Rejection::Unauthorized && r.done()) return true;
        }
        if (out.id && stage == static_cast<quint8>(ReceiptStage::Dispatched) && r.done()) return true;
        error = QStringLiteral("Server dispatch receipt is invalid"); return false;
    }
    if (operation == Operation::AssetChunk) {
        const auto handle = r.number<quint32>(); const auto index = r.number<quint16>();
        const auto& t = state_->incoming; const auto offset = quint64{index} * t.chunk;
        const auto length = bytes.size() - 6;
        if (!r.okay || !t.handle || handle != t.handle || offset >= t.bytes || length != static_cast<qsizetype>(std::min<quint64>(t.chunk, t.bytes - offset))) { error = QStringLiteral("Typed WAV chunk uses an invalid transfer, index or size"); return false; }
        Writer legacy; legacy.bytes = t.hash; legacy.number<quint32>(index); legacy.number<quint64>(offset); legacy.number<quint32>(static_cast<quint32>(length)); legacy.bytes += bytes.sliced(6);
        out.asset = legacy.bytes; return true;
    }
    const auto* schema = schemaFor(operation);
    if (!schema || bytes.size() > kMaximumDocumentBytes) { error = QStringLiteral("Control operation or size is unsupported"); return false; }
    const quint32 mask = schema->fields.size() <= 8 ? r.number<quint8>() : r.number<quint16>();
    if (!r.okay || (mask >> schema->fields.size()) != 0) { error = QStringLiteral("Server presence mask is invalid"); return false; }
    int booleanCount = 0;
    for (std::size_t index = 0; index < schema->fields.size(); ++index)
        if ((mask & (quint32{1} << index)) && schema->fields[index].type == Type::Bool) ++booleanCount;
    quint32 booleans = 0;
    for (int offset = 0; offset < booleanCount; offset += 8) booleans |= quint32{r.number<quint8>()} << offset;
    if (booleanCount && (booleans >> booleanCount)) { error = QStringLiteral("Server boolean bitmap has reserved bits"); return false; }
    int booleanIndex = 0;
    QJsonObject message{{"type", QLatin1String(schema->name)}};
    QJsonValue nextSong(QJsonValue::Undefined); quint64 nextSerial = state_->receivedSongSerial;
    auto nextPeers=state_->receivedPeers;
    quint64 nextSongUpperBound = state_->receivedSongUpperBound;
    for (std::size_t index = 0; index < schema->fields.size() && r.okay; ++index) {
        if (!(mask & (quint32{1} << index))) continue;
        const auto& f = schema->fields[index]; QJsonValue value;
        switch (f.type) {
        case Type::Bool: value = bool(booleans & (quint32{1} << booleanIndex++)); break;
        case Type::U8: value = static_cast<qint64>(r.number<quint8>()); break;
        case Type::U16: value = static_cast<qint64>(r.number<quint16>()); break;
        case Type::U32: value = static_cast<qint64>(r.number<quint32>()); break;
        case Type::I64: value = r.number<qint64>(); break;
        case Type::F64: { const auto n = std::bit_cast<double>(r.number<quint64>()); if (!std::isfinite(n)) r.okay = false; value = n; break; }
        case Type::Text: value = r.text(); break;
        case Type::Lane: case Type::Phase: case Type::Scope: case Type::Format: case Type::Profile: case Type::Ideas: {
            const auto values = enumValues(f.type); const auto id = r.number<quint8>();
            if (!id || id > values.size()) r.okay = false;
            else value = QLatin1String(values[id - 1]);
            break;
        }
        case Type::Hex16:
        case Type::Hash: {
            if (f.type==Type::Hex16 && peerField(f.name)) {
                QString peer; if (!readPeer(r,peer,nextPeers)) r.okay=false; value=peer; break;
            }
            const auto present = f.type == Type::Hash ? quint8{1} : r.number<quint8>();
            if (present > 1) r.okay = false;
            const auto binary = present ? r.take(f.type == Type::Hash ? 32 : 16) : QByteArray{};
            value = binary.isEmpty() ? QString{} : isUuidField(f.name) ? QUuid::fromRfc4122(binary).toString(QUuid::WithoutBraces) : QString::fromLatin1(binary.toHex());
            break;
        }
        case Type::TransferHash: {
            const auto handle = r.number<quint32>();
            const auto& transfer = operation == Operation::AssetProgress ? state_->outgoing : state_->incoming;
            if (!transfer.handle || handle != transfer.handle) r.okay = false;
            value = QString::fromLatin1(transfer.hash.toHex()); break;
        }
        case Type::FrameText: {
            const auto present = r.number<quint8>();
            if (present > 1) r.okay = false;
            const auto n = present ? r.number<quint64>() : 0;
            if (n > static_cast<quint64>(INT64_MAX)) r.okay = false;
            value = present ? QString::number(n) : QString{}; break;
        }
        case Type::Document: {
            const auto packed = r.number<quint8>(); auto content = r.block();
            if (packed > 3 || ((packed&2) && (operation!=Operation::ShareOffer || QByteArrayView(f.name)!="tracks")) || content.size() > kMaximumDocumentBytes) { r.okay = false; break; }
            if (packed&1) {
                Reader size{content}; const auto rawBytes = size.number<quint32>();
                if (!size.okay || !rawBytes || rawBytes > kMaximumDocumentBytes) { r.okay = false; break; }
                // Qt's qUncompress may grow beyond the advertised length. Keep
                // decompression inside an exact, bounded output allocation.
                QByteArray expanded(static_cast<qsizetype>(rawBytes), Qt::Uninitialized);
                tinfl_decompressor inflater; tinfl_init(&inflater);
                size_t inputBytes = static_cast<size_t>(content.size() - 4), outputBytes = rawBytes;
                const auto status = tinfl_decompress(&inflater,
                    reinterpret_cast<const mz_uint8*>(content.constData() + 4), &inputBytes,
                    reinterpret_cast<mz_uint8*>(expanded.data()), reinterpret_cast<mz_uint8*>(expanded.data()), &outputBytes,
                    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                if (status != TINFL_STATUS_DONE || inputBytes != static_cast<size_t>(content.size() - 4) || outputBytes != rawBytes) { r.okay = false; break; }
                content = std::move(expanded);
            }
            if (operation == Operation::Song && QByteArrayView(f.name) == "song") {
                Reader doc{content}; const auto base = doc.number<quint64>(); nextSerial = doc.number<quint64>();
                if (!doc.okay || base != state_->receivedSongSerial || nextSerial != base + 1 || nextSerial == 0 || !applyDelta(state_->receivedSong, content.sliced(doc.at), value) || !value.isObject()) { r.okay = false; break; }
                const auto patch = content.sliced(doc.at);
                nextSongUpperBound = state_->receivedSongUpperBound + static_cast<quint64>(patch.size());
                if (!patch.isEmpty() && patch[0] == 1) nextSongUpperBound = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(patch.constData() + 1));
                if (nextSongUpperBound > kMaximumDocumentBytes) nextSongUpperBound = documentBytes(value).size();
                if (nextSongUpperBound > kMaximumDocumentBytes) { r.okay = false; break; }
                nextSong = value;
            } else if (packed&2) { if (!readManifest(content,value)) r.okay=false; }
            else if (!readDocument(content, value)) r.okay = false;
            break;
        }
        case Type::Settings: case Type::Tempo: case Type::Hashes: case Type::Peers: case Type::Participants:
            if (!decodeStructured(f.type, r, value,nextPeers)) r.okay = false;
            break;
        }
        message.insert(QLatin1String(f.name), value);
    }
    auto transfer = state_->incoming;
    if (operation == Operation::AssetStart) {
        const auto handle = r.number<quint32>();
        transfer = {handle, QByteArray::fromHex(message.value("sha256").toString().toLatin1()), static_cast<quint64>(message.value("file_bytes").toInteger()), static_cast<quint32>(message.value("chunk_size").toInt())};
        if (!handle || handle <= state_->incoming.handle || transfer.hash.size() != 32 || transfer.bytes < 44 || transfer.bytes > kMaximumAssetBytes || transfer.chunk != kChunkBytes) r.okay = false;
    }
    if (!r.done()) { error = QStringLiteral("Server typed payload is malformed, stale or truncated"); return false; }
    if (!nextSong.isUndefined()) { state_->receivedSong = nextSong; state_->receivedSongSerial = nextSerial; state_->receivedSongUpperBound = nextSongUpperBound; }
    if (operation == Operation::AssetStart) state_->incoming = transfer;
    state_->receivedPeers=std::move(nextPeers);
    out.message = message;
    return true;
}

QByteArray authenticate(Operation op, quint64 sequence, const QByteArray& payload, const QByteArray& key, quint8 flags)
{
    if (!sequence || key.size() != 32 || payload.size() > 65536 || flags > 1) return {};
    Writer w; w.number<quint32>(static_cast<quint32>(28 + payload.size()));
    w.number<quint16>(static_cast<quint16>(op)); w.number<quint8>(kVersion); w.number<quint8>(flags); w.number<quint64>(sequence);
    w.bytes += QByteArray(16, '\0'); w.bytes += payload;
    const auto tag = QMessageAuthenticationCode::hash(w.bytes, key, QCryptographicHash::Sha256).first(16);
    w.bytes.replace(16, 16, tag); return w.bytes;
}
bool verify(const QByteArray& body, quint64 sequence, const QByteArray& key, Operation& op, quint8& flags, QByteArray& payload, QString& error)
{
    if (body.size() < 28 || body.size() > 28 + 65536 || key.size() != 32) { error = QStringLiteral("Control record length/key is invalid"); return false; }
    Reader r{body}; op = static_cast<Operation>(r.number<quint16>()); const auto version = r.number<quint8>(); flags = r.number<quint8>();
    if (version != kVersion || flags > 1 || !sequence || r.number<quint64>() != sequence) { error = QStringLiteral("Control record version, flags or sequence is invalid"); return false; }
    const auto received = r.take(16); Writer complete; complete.number<quint32>(static_cast<quint32>(body.size())); complete.bytes += body;
    complete.bytes.replace(16, 16, QByteArray(16, '\0'));
    const auto expected = QMessageAuthenticationCode::hash(complete.bytes, key, QCryptographicHash::Sha256).first(16);
    unsigned difference = 0; for (int i = 0; i < 16; ++i) difference |= static_cast<unsigned char>(received[i]) ^ static_cast<unsigned char>(expected[i]);
    if (difference) { error = QStringLiteral("Control record authentication failed"); return false; }
    payload = body.sliced(28); return true;
}

Channel::Channel(QByteArray sendKey, QByteArray receiveKey, Write write, Capacity pending, QObject* parent)
    : Channel(std::move(sendKey), std::move(receiveKey), std::move(write), std::move(pending), Options{}, parent) {}
Channel::Channel(QByteArray sendKey, QByteArray receiveKey, Write write, Capacity pending, Options options, QObject* parent)
    : QObject(parent), sendKey_(std::move(sendKey)), receiveKey_(std::move(receiveKey)), write_(std::move(write)), pending_(std::move(pending))
{
    options_ = options;
    independentFrameTimer_.setSingleShot(true);
    QObject::connect(&independentFrameTimer_, &QTimer::timeout, this, [this] {
        failed_ = true;
        if (onFailure) onFailure(QStringLiteral("Independent control frame stalled for ten seconds"));
    });
    turnTimer_.setSingleShot(true);
    QObject::connect(&turnTimer_, &QTimer::timeout, this, [this] {
        drainIntents();
        if (pendingSuccess_) { const auto id = std::exchange(pendingSuccess_, 0); sendReceipt(id, {}); }
        flush();
    });
    receiptClock_.start();
    flushTimer_.setInterval(5);
    QObject::connect(&flushTimer_, &QTimer::timeout, this, [this] { flush(); });
    assemblyTimer_.setSingleShot(true);
    QObject::connect(&assemblyTimer_, &QTimer::timeout, this, [this] {
        failed_ = true; flushTimer_.stop();
        const auto failure = onFailure;
        if (failure) failure(QStringLiteral("Server document assembly made no progress for ten seconds"));
    });
}
bool Channel::canQueue(qint64 bytes) const noexcept
{
    const auto limit = independentOnly_ ? qsizetype{16 * 1024} : kMaximumQueuedBytes;
    return !failed_ && bytes >= 0 && bytes <= limit && stats_.queuedBytes + intentBytes_ <= static_cast<quint64>(limit - bytes);
}
bool Channel::send(const QJsonObject& message)
{
    if (closeAfterDrain_) return false;
    const auto* schema = schemaFor(message.value("type").toString());
    if (schema && independent_ && independentOperation(schema->operation)) return independent_->send(message);
    if (!canQueue(0) || (schema && requiresReceipt(schema->operation) && awaitingReceipt_.size() + intents_.size() >= 128)) { ++stats_.queueRejects; return false; }
    // Only complete scalar assignments may supersede an unsent assignment.
    // Transactions, relative actions, snapshots and policy changes are barriers.
    QString key;
    if (schema && scalarAssignment(schema->operation)) {
        key = message.value("type").toString() + '/' + QString::number(message.value("section").toInt()) + '/' +
            message.value("lane").toString() + '/' + QString::number(message.value("beat").toInt()) + '/' + QString::number(message.value("step").toInt());
    }
    if (key.isEmpty()) drainIntents();
    Codec::Encoded encoded; QString error;
    const auto limit = independentOnly_ ? qsizetype{4096} : kMaximumQueuedBytes;
    if (!codec_.encode(message, encoded, error, std::min(limit, (independentOnly_ ? 16 * 1024 : kMaximumQueuedBytes) - static_cast<qsizetype>(stats_.queuedBytes) - intentBytes_))) {
        if (encoded.capacityExceeded) ++stats_.queueRejects; else ++stats_.encodingRejects;
        stats_.lastRejection = error; return false;
    }
    if (key.isEmpty()) return enqueue(encoded);
    if (options_.coalesce) {
        for (auto it = intents_.begin(); it != intents_.end(); ++it) if (it->key == key) {
            intentBytes_ -= it->encoded.payload.size() + kRecordHeaderBytes;
            // Keep surviving assignments in receive/revision order. Replacing
            // in place could send revision3 before another cell's revision2.
            intents_.erase(it); ++stats_.coalescedUpdates; break;
        }
    }
    intentBytes_ += encoded.payload.size() + kRecordHeaderBytes;
    intents_.push_back({std::move(encoded), std::move(key), receiptClock_.nsecsElapsed()});
    stats_.queuedHighWaterBytes = std::max(stats_.queuedHighWaterBytes, stats_.queuedBytes + intentBytes_);
    turnTimer_.start(0); return true;
}
void Channel::drainIntents()
{
    while (!intents_.empty()) {
        auto intent = std::move(intents_.front()); intents_.pop_front();
        intentBytes_ -= intent.encoded.payload.size() + kRecordHeaderBytes;
        const auto delay = static_cast<quint64>(std::max<qint64>(0, receiptClock_.nsecsElapsed() - intent.queuedNs) / 1000);
        ++stats_.assignmentDelaySamples; stats_.assignmentDelaySumUs += delay;
        stats_.assignmentDelayMaxUs = std::max(stats_.assignmentDelayMaxUs, delay);
        enqueue(intent.encoded);
    }
}
bool Channel::sendAsset(const QByteArray& payload)
{
    drainIntents();
    if (closeAfterDrain_) return false;
    if (!canQueue(kChunkBytes + 128)) { ++stats_.queueRejects; return false; }
    Codec::Encoded encoded; QString error;
    if (!codec_.encodeAsset(payload, encoded, error)) { ++stats_.encodingRejects; stats_.lastRejection = error; return false; }
    return enqueue(encoded);
}
bool Channel::sendBinary(const QByteArray& payload)
{
    drainIntents();
    const auto required = payload.size() + 128;
    if (closeAfterDrain_ || payload.isEmpty() || payload.size() > 65536 ||
        !canQueue(required) || pending_() + static_cast<qint64>(stats_.queuedBytes) > kStreamQueueBytes - required) {
        ++stats_.queueRejects; return false;
    }
    return enqueue({Operation::Binary, payload});
}
bool Channel::enqueue(const Codec::Encoded& encoded)
{
    if (sendSequence_ >= UINT64_MAX - 256) { failed_ = true; if (onFailure) onFailure(QStringLiteral("Control wire counter exhausted")); return false; }
    // The first authenticated record sequence identifies this application
    // message. Transport datagram/fragment numbers remain entirely separate.
    const auto id = sendSequence_;
    if (requiresReceipt(encoded.operation)) awaitingReceipt_.push_back({id, receiptClock_.nsecsElapsed()});
    const bool fragmented = encoded.payload.size() > kPartBytes;
    const qsizetype count = std::max<qsizetype>(1, (encoded.payload.size() + kPartBytes - 1) / kPartBytes);
    for (qsizetype part = 0; part < count; ++part) {
        Writer w;
        if (fragmented) { w.number<quint32>(static_cast<quint32>(encoded.payload.size())); w.number<quint32>(static_cast<quint32>(part * kPartBytes)); }
        w.bytes += encoded.payload.mid(part * kPartBytes, kPartBytes);
        auto frame = authenticate(encoded.operation, sendSequence_++, w.bytes, sendKey_, fragmented ? 1 : 0);
        stats_.queuedBytes += frame.size(); output_.push_back(std::move(frame));
    }
    stats_.queuedHighWaterBytes = std::max(stats_.queuedHighWaterBytes, stats_.queuedBytes);
    stats_.pendingReceipts = awaitingReceipt_.size();
    // Batching has no timer delay for transactions or checkpoints. Scalar edit
    // bursts and successful receipts share one event-loop turn instead.
    if (options_.batch && scalarAssignment(encoded.operation)) turnTimer_.start(0);
    else flush();
    return true;
}
void Channel::flush()
{
    if (failed_) return;
    for (int turn = 0; turn < 16 && !output_.empty(); ++turn) {
        QByteArray bytes = output_.front(); std::size_t count = 1;
        // Fit small records in one existing 1024-byte reliable fragment.
        if (options_.batch) for (auto it = std::next(output_.begin()); it != output_.end() && bytes.size() + it->size() <= 1024; ++it) { bytes += *it; ++count; }
        if (pending_() > kStreamQueueBytes - bytes.size()) break;
        if (!write_(bytes)) break;
        stats_.wireBytesSent += bytes.size(); stats_.recordsSent += count; ++stats_.transportWrites;
        stats_.batchedRecords += count - 1;
        stats_.queuedBytes -= bytes.size();
        while (count--) output_.pop_front();
    }
    if (output_.empty()) {
        flushTimer_.stop();
        if (closeAfterDrain_) {
            if (independent_ && independent_->stats().queuedBytes) flushTimer_.start();
            else { auto close = std::move(closeAfterDrain_); failed_ = true; close(); }
        }
    } else if (!flushTimer_.isActive()) flushTimer_.start();
}
void Channel::closeAfterDrain(std::function<void()> close)
{
    drainIntents();
    if (pendingSuccess_) sendReceipt(std::exchange(pendingSuccess_, 0), {});
    closeAfterDrain_ = std::move(close); flush();
}
bool Channel::receive(const QByteArray& body, QString& error)
{
    if (failed_) { error = QStringLiteral("Server channel failed"); return false; }
    Operation operation{}; quint8 flags = 0; QByteArray payload;
    if (!verify(body, receiveSequence_, receiveKey_, operation, flags, payload, error)) return false;
    if (independentOnly_ && !independentOperation(operation) && operation != Operation::Receipt) {
        error = QStringLiteral("Snapshot-dependent operation on independent control ordering"); return false;
    }
    if (receiveSequence_ == UINT64_MAX) { error = QStringLiteral("Server receive counter exhausted"); return false; }
    const auto recordSequence = receiveSequence_++;
    ++stats_.recordsReceived;
    if (operation == Operation::Receipt) {
        Codec::Decoded decoded;
        if (flags || !codec_.decode(operation, payload, decoded, error) || decoded.id >= sendSequence_ || decoded.id < highestReceipt_) { error = QStringLiteral("Server receipt refers to unsent/stale operation"); return false; }
        highestReceipt_ = decoded.id; ++stats_.receiptsReceived; stats_.lastDispatched = highestReceipt_;
        if (decoded.stage == ReceiptStage::Rejected) { ++stats_.rejectionResultsReceived; stats_.lastRejection = decoded.reason; }
        const auto now = receiptClock_.nsecsElapsed();
        while (!awaitingReceipt_.empty() && awaitingReceipt_.front().id <= decoded.id) {
            const auto latency = static_cast<quint64>(std::max<qint64>(0, now - awaitingReceipt_.front().queuedNs) / 1000);
            ++stats_.resultLatencySamples; stats_.resultLatencySumUs += latency;
            stats_.resultLatencyMaxUs = std::max(stats_.resultLatencyMaxUs, latency);
            awaitingReceipt_.pop_front();
        }
        stats_.pendingReceipts = awaitingReceipt_.size(); return true;
    }
    Reader r{payload}; auto id = recordSequence;
    QByteArray completed;
    if (flags) {
        const auto total = r.number<quint32>(), offset = r.number<quint32>();
        const auto partIndex = offset / kPartBytes;
        if (offset % kPartBytes != 0 || static_cast<quint64>(partIndex) >= recordSequence) { error = QStringLiteral("Server fragment identity is invalid"); return false; }
        id = recordSequence - partIndex;
        const auto length = payload.size() - r.at;
        if (!r.okay || id <= highestMessage_ || !total || total > kMaximumDocumentBytes || length <= 0 || length > kPartBytes || offset > total || length > total - offset || (length < kPartBytes && offset + length != total) || offset != static_cast<quint32>(assembly_.size()) || (assemblyId_ && (assemblyId_ != id || assemblyOperation_ != operation || assemblyTotal_ != total))) { error = QStringLiteral("Server message reassembly is invalid"); return false; }
        if (!assemblyId_) { assemblyId_ = id; assemblyOperation_ = operation; assemblyTotal_ = total; }
        assembly_ += payload.sliced(r.at);
        if (assembly_.size() != total) { assemblyTimer_.start(10000); return true; }
        assemblyTimer_.stop();
        completed = std::move(assembly_); assembly_.clear(); assemblyId_ = 0;
    } else {
        if (assemblyId_) { error = QStringLiteral("Server message interrupted an incomplete document"); return false; }
        if (id <= highestMessage_) { error = QStringLiteral("Server message identity is out of order"); return false; }
        completed = payload.sliced(r.at);
    }
    Codec::Decoded decoded;
    if (!codec_.decode(operation, completed, decoded, error)) return false;
    highestMessage_ = id;
    const auto decision = !decoded.message.isEmpty() && validateMessage ? validateMessage(decoded.message) : DispatchDecision{};
    if (decision.rejection == Rejection::None) {
        if (!decoded.asset.isEmpty()) { if (onAsset) onAsset(decoded.asset); }
        else if (onMessage) onMessage(decoded.message);
    } else ++stats_.operationsRejected;
    if (!requiresReceipt(operation) && decision.rejection == Rejection::None) return true;
    // Cumulative dispatch confirmation. The existing stream still supplies loss
    // recovery. Domain commit/readiness/asset validation are separate messages.
    if (decision.rejection == Rejection::None && options_.cumulativeReceipts) {
        if (pendingSuccess_) ++stats_.cumulativeReceipts;
        pendingSuccess_ = id; turnTimer_.start(0); return true;
    }
    // A rejection explicitly reports every failed action. It cumulatively
    // confirms earlier successes too, so a later success cannot conceal it.
    pendingSuccess_ = 0;
    if (!sendReceipt(id, decision)) { error = QStringLiteral("Server receipt queue is full"); return false; }
    return true;
}
bool Channel::sendReceipt(quint64 id, const DispatchDecision& decision)
{
    Writer receipt; receipt.number<quint64>(id);
    receipt.number<quint8>(static_cast<quint8>(decision.rejection == Rejection::None ? ReceiptStage::Dispatched : ReceiptStage::Rejected));
    if (decision.rejection != Rejection::None) {
        receipt.number<quint8>(static_cast<quint8>(decision.rejection)); receipt.text(decision.reason.left(256));
    }
    if (sendSequence_ == UINT64_MAX) { failed_ = true; return false; }
    auto frame = authenticate(Operation::Receipt, sendSequence_, receipt.bytes, sendKey_);
    if (!canQueue(frame.size())) { failed_ = true; if (onFailure) onFailure(QStringLiteral("Server receipt queue is full")); return false; }
    ++sendSequence_;
    stats_.queuedBytes += frame.size(); output_.push_back(std::move(frame)); ++stats_.receiptsSent;
    flush(); return true;
}
Stats Channel::stats() const
{
    auto result = stats_; const auto& encoding = codec_.stats();
    result.queuedBytes += intentBytes_;
    result.checkpointsSent = encoding.checkpointsSent; result.deltasSent = encoding.deltasSent;
    result.savedSnapshotBytes = encoding.savedSnapshotBytes; result.assetHeaderBytesSaved = encoding.assetHeaderBytesSaved;
    result.assetMetadataBytesSaved = encoding.assetMetadataBytesSaved;
    result.typedManifests=encoding.typedManifests; result.manifestDocumentFallbacks=encoding.manifestDocumentFallbacks;
    result.typedRegions=encoding.typedRegions; result.typedStructures=encoding.typedStructures;
    result.peerDefinitions=encoding.peerDefinitions; result.peerReferences=encoding.peerReferences;
    result.compressionAttempts = encoding.compressionAttempts; result.compressionSkipped = encoding.compressionSkipped;
    if (independent_) {
        const auto extra = independent_->stats(); accumulate(result, extra);
        result.independentRecordsSent = extra.recordsSent; result.independentRecordsReceived = extra.recordsReceived;
        result.independentQueuedBytes = extra.queuedBytes;
    }
    return result;
}
void Channel::enableIndependent(Write write, Capacity pending)
{
    if (independent_) return;
    // Separate replay domains without changing the 32-byte header or tag scheme.
    const auto derive = [](const QByteArray& key) {
        return QMessageAuthenticationCode::hash(QByteArrayLiteral("jam2-independent-control-v")+QByteArray::number(kVersion), key, QCryptographicHash::Sha256);
    };
    independent_ = std::make_unique<Channel>(derive(sendKey_), derive(receiveKey_), std::move(write), std::move(pending), options_);
    independent_->independentOnly_ = true;
    independent_->onMessage = [this](const QJsonObject& message) { if (onMessage) onMessage(message); };
    independent_->validateMessage = [this](const QJsonObject& message) { return validateMessage ? validateMessage(message) : DispatchDecision{}; };
    independent_->onFailure = [this](const QString& error) { failed_ = true; if (onFailure) onFailure(error); };
}
bool Channel::receiveIndependent(const QByteArray& bytes, QString& error)
{
    if (!independent_ || bytes.size() + independentBuffer_.size() > 16 * 1024) {
        error = QStringLiteral("Independent control input exceeds its 16 KiB bound"); return false;
    }
    independentBuffer_ += bytes;
    if (!bytes.isEmpty()) independentFrameTimer_.start(10000);
    while (independentBuffer_.size() >= 4) {
        const auto size = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(independentBuffer_.constData()));
        if (size < 28 || size > 4096 - 4) { error = QStringLiteral("Independent control record length is invalid"); return false; }
        if (independentBuffer_.size() < size + 4) return true;
        const auto body = independentBuffer_.mid(4, size); independentBuffer_.remove(0, size + 4);
        if (!independent_->receive(body, error)) return false;
    }
    if (independentBuffer_.isEmpty()) independentFrameTimer_.stop();
    return true;
}
void accumulate(Stats& t, const Stats& v) noexcept
{
    t.recordsSent += v.recordsSent; t.recordsReceived += v.recordsReceived; t.wireBytesSent += v.wireBytesSent;
    t.receiptsSent += v.receiptsSent; t.receiptsReceived += v.receiptsReceived;
    t.lastDispatched = std::max(t.lastDispatched, v.lastDispatched);
    t.checkpointsSent += v.checkpointsSent; t.deltasSent += v.deltasSent; t.savedSnapshotBytes += v.savedSnapshotBytes;
    t.assetHeaderBytesSaved += v.assetHeaderBytesSaved; t.queueRejects += v.queueRejects;
    t.assetMetadataBytesSaved += v.assetMetadataBytesSaved;
    t.typedManifests+=v.typedManifests; t.manifestDocumentFallbacks+=v.manifestDocumentFallbacks;
    t.typedRegions+=v.typedRegions; t.typedStructures+=v.typedStructures;
    t.peerDefinitions+=v.peerDefinitions; t.peerReferences+=v.peerReferences;
    t.compressionAttempts += v.compressionAttempts; t.compressionSkipped += v.compressionSkipped;
    t.transportWrites += v.transportWrites; t.batchedRecords += v.batchedRecords;
    t.coalescedUpdates += v.coalescedUpdates; t.cumulativeReceipts += v.cumulativeReceipts;
    t.independentRecordsSent += v.independentRecordsSent; t.independentRecordsReceived += v.independentRecordsReceived;
    t.independentQueuedBytes += v.independentQueuedBytes;
    t.assignmentDelaySamples += v.assignmentDelaySamples; t.assignmentDelaySumUs += v.assignmentDelaySumUs;
    t.assignmentDelayMaxUs = std::max(t.assignmentDelayMaxUs, v.assignmentDelayMaxUs);
    t.pendingReceipts += v.pendingReceipts; t.queuedBytes += v.queuedBytes;
    t.queuedHighWaterBytes = std::max(t.queuedHighWaterBytes, v.queuedHighWaterBytes);
    t.encodingRejects += v.encodingRejects; t.unresolvedReceipts += v.unresolvedReceipts;
    t.operationsRejected += v.operationsRejected; t.rejectionResultsReceived += v.rejectionResultsReceived;
    t.resultLatencySamples += v.resultLatencySamples; t.resultLatencySumUs += v.resultLatencySumUs;
    t.resultLatencyMaxUs = std::max(t.resultLatencyMaxUs, v.resultLatencyMaxUs);
    if (!v.lastRejection.isEmpty()) t.lastRejection = v.lastRejection;
}
QJsonObject diagnostics(const Stats& s)
{
    return {{"version", kVersion}, {"records_sent", static_cast<qint64>(s.recordsSent)}, {"records_received", static_cast<qint64>(s.recordsReceived)},
        {"authenticated_bytes_sent", static_cast<qint64>(s.wireBytesSent)}, {"dispatch_receipts_sent", static_cast<qint64>(s.receiptsSent)},
        {"dispatch_receipts_received", static_cast<qint64>(s.receiptsReceived)}, {"last_dispatch_receipt", static_cast<qint64>(s.lastDispatched)},
        {"pending_dispatch_receipts", static_cast<qint64>(s.pendingReceipts)}, {"checkpoints_sent", static_cast<qint64>(s.checkpointsSent)},
        {"snapshot_deltas_sent", static_cast<qint64>(s.deltasSent)}, {"bytes_avoided_vs_last_checkpoint", static_cast<qint64>(s.savedSnapshotBytes)},
        {"wav_payload_header_bytes_saved", static_cast<qint64>(s.assetHeaderBytesSaved)}, {"wav_metadata_bytes_saved", static_cast<qint64>(s.assetMetadataBytesSaved)},
        {"typed_manifests",static_cast<qint64>(s.typedManifests)}, {"manifest_document_fallbacks",static_cast<qint64>(s.manifestDocumentFallbacks)},
        {"typed_region_patches",static_cast<qint64>(s.typedRegions)}, {"typed_lane_splices",static_cast<qint64>(s.typedStructures)},
        {"peer_handle_definitions",static_cast<qint64>(s.peerDefinitions)}, {"peer_handle_references",static_cast<qint64>(s.peerReferences)}, {"peer_handle_table_limit",4096},
        {"compression_attempts", static_cast<qint64>(s.compressionAttempts)}, {"compression_skipped_small", static_cast<qint64>(s.compressionSkipped)}, {"compression_minimum_bytes", 128},
        {"transport_writes", static_cast<qint64>(s.transportWrites)}, {"records_sharing_writes", static_cast<qint64>(s.batchedRecords)},
        {"unsent_updates_coalesced", static_cast<qint64>(s.coalescedUpdates)}, {"success_receipts_coalesced", static_cast<qint64>(s.cumulativeReceipts)},
        {"unsent_assignment_delay_samples", static_cast<qint64>(s.assignmentDelaySamples)}, {"unsent_assignment_delay_sum_us", static_cast<qint64>(s.assignmentDelaySumUs)}, {"unsent_assignment_delay_max_us", static_cast<qint64>(s.assignmentDelayMaxUs)},
        {"independent_records_sent", static_cast<qint64>(s.independentRecordsSent)}, {"independent_records_received", static_cast<qint64>(s.independentRecordsReceived)},
        {"independent_queued_bytes", static_cast<qint64>(s.independentQueuedBytes)}, {"independent_queue_limit_bytes", 16384}, {"independent_record_limit_bytes", 4096}, {"independent_packet_budget_per_poll", 2},
        {"queue_rejects", static_cast<qint64>(s.queueRejects)},
        {"queued_bytes", static_cast<qint64>(s.queuedBytes)}, {"queue_high_water_bytes", static_cast<qint64>(s.queuedHighWaterBytes)},
        {"queue_limit_bytes_per_channel", kMaximumQueuedBytes}, {"stream_queue_limit_bytes_per_channel", kStreamQueueBytes}, {"document_limit_bytes", kMaximumDocumentBytes},
        {"encoding_rejects", static_cast<qint64>(s.encodingRejects)}, {"last_encoding_rejection", s.lastRejection},
        {"unresolved_dispatch_receipts", static_cast<qint64>(s.unresolvedReceipts)},
        {"operations_rejected", static_cast<qint64>(s.operationsRejected)}, {"rejection_results_received", static_cast<qint64>(s.rejectionResultsReceived)},
        {"dispatch_result_latency_samples", static_cast<qint64>(s.resultLatencySamples)},
        {"dispatch_result_latency_sum_us", static_cast<qint64>(s.resultLatencySumUs)}, {"dispatch_result_latency_max_us", static_cast<qint64>(s.resultLatencyMaxUs)},
        {"flush_interval_ms", 5}, {"flush_records_per_turn", 16}, {"maximum_pending_dispatch_receipts_per_channel", 128}, {"assembly_idle_timeout_ms", 10000}};
}
} // namespace jam2::application::control_wire
