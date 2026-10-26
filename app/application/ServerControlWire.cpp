#include "ServerControlWire.hpp"
#include "miniz_tinfl.h"

#include <QCborValue>
#include <QCborStreamReader>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QSet>
#include <QStringDecoder>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace jam2::application::server_wire {
namespace {
constexpr qsizetype kChunkBytes = 24 * 1024, kMaximumAssetBytes = 512 * 1024 * 1024;
constexpr qsizetype kPartBytes = 32 * 1024;
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
enum class Type { Bool, U8, U16, U32, I64, F64, Text, Hex16, Hash, TransferHash, Document, FrameText, Settings, Tempo, Hashes, Peers, Participants };
struct Field { const char* name; Type type; };
struct Schema { Operation operation; const char* name; std::vector<Field> fields; };

// Positional fields, selected by a fixed u32 presence bitmap. No field names or
// dynamic schema travel on the wire. Common authority fields are explicit slots.
const std::vector<Schema>& schemas()
{
    using enum Type;
    using enum Operation;
    static const std::vector<Schema> values = [] {
        std::vector<Schema> s{
            {Contract,"session.contract",{{"protocol_version",U8},{"audio_format",Text},{"profile",Text},{"sample_rate",U32},{"frame_size",U16},{"coordinator_token",Hex16}}},
            {Membership,"session.membership",{{"page_index",U32},{"page_count",U32},{"coordinator_token",Hex16},{"editor_authority_token",Hex16},{"arrangement_authority_token",Hex16},{"editor_revision",I64},{"arrangement_revision",I64},{"peers",Peers}}},
            {Heartbeat,"session.heartbeat",{{"sequence",U32}}}, {HeartbeatReply,"session.heartbeat.ack",{{"sequence",U32}}},
            {Introduce,"session.udp.introduce",{{"peer",Hex16},{"nonce",Hex16}}}, {Repair,"session.udp.recover",{{"peer",Hex16}}},
            {PairFailure,"session.connection.failure",{{"a",Hex16},{"b",Hex16},{"detail",Text}}},
            {Endpoint,"session.endpoint.update",{{"udp_endpoint",Text}}}, {End,"session.end",{{"detail",Text}}}, {Error,"session.error",{{"message",Text}}},
            {SyncSet,"jam.sync.set",{}}, {SyncRequest,"jam.sync.request",{}},
            {MetronomeSet,"jam.metronome.state.set",{{"enabled",Bool}}}, {MetronomeRequest,"jam.metronome.state.request",{{"enabled",Bool}}},
            {ListeningRequest,"jam.listening.request",{}}, {ListeningPrepare,"jam.listening.prepare",{}},
            {ListeningReady,"jam.listening.ready",{}}, {ListeningFailed,"jam.listening.failed",{}},
            {ListeningCommit,"jam.listening.commit",{}}, {ListeningCancel,"jam.listening.cancel",{}},
            {ListeningState,"jam.listening.state",{}}, {ListeningReset,"jam.listening.reset",{}},
            {ListeningApplied,"jam.listening.applied",{{"actual_frame",FrameText}}},
            {Cell,"beat.set",{{"section",U8},{"lane",Text},{"beat",U16},{"text",Text}}},
            {Hit,"beat.hit",{{"section",U8},{"beat",U16},{"lane",U8},{"text",Text}}},
            {BeatDivision,"beat.division",{{"section",U8},{"beat",U16},{"division",U8}}},
            {MusicDivision,"music.division",{{"section",U8},{"beat",U16},{"division",U8}}},
            {MusicStep,"music.step",{{"section",U8},{"beat",U16},{"step",U8},{"lane",Text},{"text",Text}}},
            {Resize,"grid.resize",{{"section",U8},{"lane",Text},{"beats",U16}}},
            {Song,"song.set",{{"song",Document},{"sync_scope",Text},{"arrangement_revision",I64},{"base_arrangement_revision",I64},{"host_authoritative",Bool},{"track_playing",Bool}}},
            {Render,"practice.references.render",{{"request_id",Hex16},{"render_signature",Hash},{"render_chords",Bool},{"render_drums",Bool},{"render_melody",Bool},{"render_bass",Bool},{"render_support",Bool},{"chord_voicing",U32},{"drum_kit",U32}}},
            {BankRequest,"bank.request",{{"bank",U8},{"target_abs_beat",FrameText}}},
            {BankPrepare,"bank.prepare",{{"bank",U8},{"switch_id",Hex16},{"target_abs_beat",FrameText}}},
            {BankReady,"bank.ready",{{"bank",U8},{"switch_id",Hex16}}}, {BankCancel,"bank.cancel",{{"bank",U8},{"switch_id",Hex16}}},
            {BankSwitch,"bank.switch",{{"bank",U8},{"switch_id",Hex16},{"target_abs_beat",FrameText}}},
            {ShareRequest,"looper.track.share.request",{{"batch_id",Hex16}}},
            {ShareOffer,"looper.track.batch.offer",{{"batch_id",Hex16},{"tracks",Document}}},
            {ShareComplete,"looper.track.batch.complete",{{"batch_id",Hex16},{"tracks",U32},{"applied",Bool},{"reason",Text}}},
            {RecordingState,"looper.recording.state",{{"phase",Text},{"state_revision",U32},{"bank",U8},{"lane_id",Text},{"lane_name",Text},{"count_in_remaining",U16},{"count_in_bars",U8},{"group_id",Hex16},{"source_peer_token",Hex16}}},
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
                schema.fields = {{"track_lanes",Bool},{"auto_share_wavs",Bool},{"global_playback",Bool},{"listening_tools",Bool},{"generated_ideas",Text},{"metronome_state",Bool},{"recordings",Bool}};
            }
            if (schema.operation >= ListeningRequest && schema.operation <= ListeningReset) {
                schema.fields = {{"settings",Settings},{"bank",U8},{"tempo",Tempo},{"target_frame",FrameText},{"epoch_frame",FrameText},{"epoch_position",F64},{"source_seconds",F64},{"playing",Bool},{"reason",Text},{"changed_groups",U8}};
            }
            schema.fields.push_back({"revision",I64});
            schema.fields.push_back({"authority_token",Hex16});
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
bool fixedHex(const QString& text, int size, QByteArray& out)
{
    if (text.size() != size * 2) return false;
    for (const auto ch : text) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    out = QByteArray::fromHex(text.toLatin1()); return out.size() == size;
}
bool encodeStructured(Type type, const QJsonValue& value, Writer& writer)
{
    if (type == Type::Settings) {
        const auto object = value.toObject();
        if (!value.isObject() || object.size() != 7 || !object.value("focus_enabled").isBool() || !object.value("focus_preset").isString()) return false;
        const auto speed = object.value("speed_ppm").toInteger(-1), pitch = object.value("pitch_cents").toInteger(-99999);
        if (speed < 100000 || speed > 2000000 || pitch < -1200 || pitch > 1200) return false;
        writer.number<qint32>(static_cast<qint32>(speed)); writer.number<qint32>(static_cast<qint32>(pitch));
        writer.number<quint8>(object.value("focus_enabled").toBool());
        if (!writer.text(object.value("focus_preset").toString())) return false;
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
        writer.bytes += bytes;
        if (type == Type::Peers) {
            const auto object = entry.toObject();
            const auto id = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData()));
            if (object.size() != 3 || object.value("peer_id").toString() != QString::number(id) || !object.value("endpoint").isString() || !writer.text(object.value("endpoint").toString())) return false;
        }
    }
    return true;
}
bool decodeStructured(Type type, Reader& r, QJsonValue& value)
{
    if (type == Type::Settings) {
        const auto speed = r.number<qint32>(), pitch = r.number<qint32>(); const auto enabled = r.number<quint8>(); const auto preset = r.text();
        const auto hz = std::bit_cast<double>(r.number<quint64>()), gain = std::bit_cast<double>(r.number<quint64>()), q = std::bit_cast<double>(r.number<quint64>());
        if (!r.okay || speed < 100000 || speed > 2000000 || pitch < -1200 || pitch > 1200 || enabled > 1 || !std::isfinite(hz) || !std::isfinite(gain) || !std::isfinite(q)) return false;
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
        const auto bytes = r.take(type == Type::Hashes ? 32 : 16); if (!r.okay) return false;
        const auto text = QString::fromLatin1(bytes.toHex());
        if (type == Type::Peers) {
            const auto id = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData()));
            array.append(QJsonObject{{"token", text}, {"peer_id", QString::number(id)}, {"endpoint", r.text()}});
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
    out = value.toJsonValue();
    return !out.isUndefined();
}
QByteArray documentBytes(const QJsonValue& value) { return QCborValue::fromJsonValue(value).toCbor(); }

// Explicit recursive patch tree: unchanged=0, replace=1, object edits=2,
// same-length array edits=3. Array insert/removal uses a replacement so indices
// cannot accidentally refer to a different object. A stream-local base serial
// guards every patch. This is a serialization delta, not authority reconciliation.
QByteArray delta(const QJsonValue& base, const QJsonValue& value, int depth = 0)
{
    Writer w;
    if (base == value) { w.number<quint8>(0); return w.bytes; }
    w.number<quint8>(1); w.block(documentBytes(value));
    if (depth >= 24) return w.bytes;
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
            if (!changes.text(key)) return w.bytes;
            changes.number<quint8>(b.contains(key) ? 1 : 0);
            if (b.contains(key)) changes.block(delta(a.value(key), b.value(key), depth + 1));
            ++count;
        }
        Writer candidate; candidate.number<quint8>(2); candidate.number<quint32>(count); candidate.bytes += changes.bytes;
        if (candidate.bytes.size() < w.bytes.size()) return candidate.bytes;
    } else if (base.isArray() && value.isArray() && base.toArray().size() == value.toArray().size()) {
        const auto a = base.toArray(), b = value.toArray();
        quint32 count = 0;
        for (qsizetype i = 0; i < a.size(); ++i) {
            if (a[i] == b[i]) continue;
            changes.number<quint32>(static_cast<quint32>(i)); changes.block(delta(a[i], b[i], depth + 1)); ++count;
        }
        Writer candidate; candidate.number<quint8>(3); candidate.number<quint32>(count); candidate.bytes += changes.bytes;
        if (candidate.bytes.size() < w.bytes.size()) return candidate.bytes;
    }
    return w.bytes;
}
bool applyDelta(const QJsonValue& base, const QByteArray& bytes, QJsonValue& out, int depth = 0)
{
    if (depth > 24) return false;
    Reader r{bytes}; const auto kind = r.number<quint8>();
    if (kind == 0) { out = base; return r.done(); }
    if (kind == 1) { const auto content = r.block(); return r.done() && readDocument(content, out); }
    const auto count = r.number<quint32>();
    if (!r.okay || count > 65536) return false;
    if (kind == 2 && base.isObject()) {
        auto object = base.toObject(); QSet<QString> seen;
        for (quint32 i = 0; i < count && r.okay; ++i) {
            const auto key = r.text(); const auto present = r.number<quint8>();
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
    case Heartbeat: case HeartbeatReply: case ListeningReady: case ListeningFailed: case ListeningApplied:
    case ListeningCancel: case BankReady: case BankCancel: case RecordingState:
    case AssetProgress: case AssetChunk: case Binary: case Receipt:
        return false;
    default: return true;
    }
}
}

struct Codec::State {
    QJsonValue sentSong, receivedSong;
    quint64 sentSongSerial = 0, receivedSongSerial = 0;
    quint32 nextTransfer = 1;
    struct Transfer { quint32 handle = 0; QByteArray hash; quint64 bytes = 0; quint32 chunk = 0; };
    Transfer outgoing, incoming;
    Stats stats;
};
Codec::Codec() : state_(std::make_unique<State>()) {}
Codec::~Codec() = default;
const Stats& Codec::stats() const noexcept { return state_->stats; }

bool Codec::encode(const QJsonObject& message, Encoded& out, QString& error, qsizetype maximumWireBytes)
{
    out = {};
    const auto* schema = schemaFor(message.value("type").toString());
    if (!schema) { error = QStringLiteral("unsupported Server control operation"); return false; }
    QSet<QString> allowed{QStringLiteral("type")};
    Writer fields; quint32 mask = 0;
    QJsonValue nextSong(QJsonValue::Undefined); quint64 nextSongSerial = state_->sentSongSerial;
    auto nextTransfer = state_->outgoing;
    for (std::size_t index = 0; index < schema->fields.size(); ++index) {
        const auto& field = schema->fields[index]; allowed.insert(QLatin1String(field.name));
        const auto value = message.value(QLatin1String(field.name));
        if (value.isUndefined()) continue;
        mask |= quint32{1} << index;
        switch (field.type) {
        case Type::Bool:
            if (!value.isBool()) { error = QStringLiteral("Server boolean field is invalid"); return false; }
            fields.number<quint8>(value.toBool()); break;
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
        case Type::Hex16:
        case Type::Hash: {
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
                if (!fixedHex(hex, size, bytes)) { error = QStringLiteral("Server binary identity is invalid"); return false; }
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
                const auto patch = delta(state_->sentSong, value);
                doc.bytes += patch; bytes = doc.bytes; nextSong = value;
            } else bytes = documentBytes(value);
            if (bytes.size() > kMaximumDocumentBytes) { error = QStringLiteral("Server document exceeds 4 MiB"); return false; }
            const auto compressed = qCompress(bytes, 1);
            const bool pack = compressed.size() + 1 < bytes.size();
            fields.number<quint8>(pack ? 1 : 0); fields.block(pack ? compressed : bytes); break;
        }
        case Type::Settings: case Type::Tempo: case Type::Hashes: case Type::Peers: case Type::Participants:
            if (!encodeStructured(field.type, value, fields)) { error = QStringLiteral("Server structured field violates its fixed schema"); return false; }
            break;
        }
    }
    for (auto it = message.begin(); it != message.end(); ++it) {
        if (!allowed.contains(it.key())) { error = QStringLiteral("unsupported field for Server operation: ") + it.key(); return false; }
    }
    if (schema->operation == Operation::AssetStart) {
        if (state_->nextTransfer == 0 || message.value("chunk_size").toInt() != kChunkBytes || message.value("file_bytes").toInteger() < 44 || message.value("file_bytes").toInteger() > kMaximumAssetBytes) { error = QStringLiteral("Server asset start is invalid"); return false; }
        nextTransfer = {state_->nextTransfer, QByteArray::fromHex(message.value("sha256").toString().toLatin1()), static_cast<quint64>(message.value("file_bytes").toInteger()), static_cast<quint32>(kChunkBytes)};
        if (nextTransfer.hash.size() != 32) return false;
        fields.number<quint32>(nextTransfer.handle);
    }
    Writer payload; payload.number<quint32>(mask); payload.bytes += fields.bytes;
    if (payload.bytes.size() > kMaximumDocumentBytes) { error = QStringLiteral("Server operation exceeds its bound"); return false; }
    out = {schema->operation, payload.bytes};
    const auto parts = std::max<qsizetype>(1, (out.payload.size() + kPartBytes - 1) / kPartBytes);
    const auto required = out.payload.size() + parts * (parts > 1 ? 40 : 32);
    if (required > maximumWireBytes) {
        out.capacityExceeded = true; error = QStringLiteral("Server application output queue is full"); return false;
    }
    if (!nextSong.isUndefined()) {
        if (state_->sentSongSerial == 0) ++state_->stats.checkpointsSent;
        else ++state_->stats.deltasSent;
        const auto fullBytes = documentBytes(nextSong).size();
        if (fullBytes > payload.bytes.size()) state_->stats.savedSnapshotBytes += fullBytes - payload.bytes.size();
        state_->sentSong = nextSong; state_->sentSongSerial = nextSongSerial;
    }
    if (schema->operation == Operation::AssetStart) { state_->outgoing = nextTransfer; ++state_->nextTransfer; }
    if (schema->operation == Operation::AssetProgress || schema->operation == Operation::AssetDone) state_->stats.assetMetadataBytesSaved += 30;
    return true;
}

bool Codec::encodeAsset(const QByteArray& chunk, Encoded& out, QString& error)
{
    const auto& t = state_->outgoing;
    if (chunk.size() <= 48 || chunk.size() > 48 + kChunkBytes || t.handle == 0 || chunk.first(32) != t.hash) { error = QStringLiteral("Server WAV chunk has no matching transfer"); return false; }
    Reader r{chunk}; r.take(32); const auto index = r.number<quint32>(); const auto offset = r.number<quint64>(); const auto length = r.number<quint32>();
    if (!r.okay || index > kMaximumAssetBytes / kChunkBytes || offset != quint64{index} * t.chunk || offset >= t.bytes || length != static_cast<quint32>(std::min<quint64>(t.chunk, t.bytes - offset)) || chunk.size() != 48 + length) { error = QStringLiteral("Server WAV chunk index, size or offset is invalid"); return false; }
    Writer w; w.number<quint32>(t.handle); w.number<quint16>(static_cast<quint16>(index)); w.bytes += chunk.sliced(48);
    out = {Operation::AssetChunk, w.bytes}; state_->stats.assetHeaderBytesSaved += 42;
    return true;
}

bool Codec::decode(Operation operation, const QByteArray& bytes, Decoded& out, QString& error)
{
    out = {};
    Reader r{bytes};
    if (operation == Operation::Binary) {
        if (bytes.isEmpty() || bytes.size() > 65536) { error = QStringLiteral("Server diagnostic binary size is invalid"); return false; }
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
        if (!r.okay || !t.handle || handle != t.handle || offset >= t.bytes || length != static_cast<qsizetype>(std::min<quint64>(t.chunk, t.bytes - offset))) { error = QStringLiteral("Server WAV chunk uses an invalid transfer, index or size"); return false; }
        Writer legacy; legacy.bytes = t.hash; legacy.number<quint32>(index); legacy.number<quint64>(offset); legacy.number<quint32>(static_cast<quint32>(length)); legacy.bytes += bytes.sliced(6);
        out.asset = legacy.bytes; return true;
    }
    const auto* schema = schemaFor(operation);
    if (!schema || bytes.size() > kMaximumDocumentBytes) { error = QStringLiteral("Server operation or size is unsupported"); return false; }
    const auto mask = r.number<quint32>();
    if (!r.okay || (mask >> schema->fields.size()) != 0) { error = QStringLiteral("Server presence mask is invalid"); return false; }
    QJsonObject message{{"type", QLatin1String(schema->name)}};
    QJsonValue nextSong(QJsonValue::Undefined); quint64 nextSerial = state_->receivedSongSerial;
    for (std::size_t index = 0; index < schema->fields.size() && r.okay; ++index) {
        if (!(mask & (quint32{1} << index))) continue;
        const auto& f = schema->fields[index]; QJsonValue value;
        switch (f.type) {
        case Type::Bool: { const auto b = r.number<quint8>(); if (b > 1) r.okay = false; value = bool(b); break; }
        case Type::U8: value = static_cast<qint64>(r.number<quint8>()); break;
        case Type::U16: value = static_cast<qint64>(r.number<quint16>()); break;
        case Type::U32: value = static_cast<qint64>(r.number<quint32>()); break;
        case Type::I64: value = r.number<qint64>(); break;
        case Type::F64: { const auto n = std::bit_cast<double>(r.number<quint64>()); if (!std::isfinite(n)) r.okay = false; value = n; break; }
        case Type::Text: value = r.text(); break;
        case Type::Hex16:
        case Type::Hash: {
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
            if (packed > 1 || content.size() > kMaximumDocumentBytes) { r.okay = false; break; }
            if (packed) {
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
                if (documentBytes(value).size() > kMaximumDocumentBytes) { r.okay = false; break; }
                nextSong = value;
            } else if (!readDocument(content, value)) r.okay = false;
            break;
        }
        case Type::Settings: case Type::Tempo: case Type::Hashes: case Type::Peers: case Type::Participants:
            if (!decodeStructured(f.type, r, value)) r.okay = false;
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
    if (!nextSong.isUndefined()) { state_->receivedSong = nextSong; state_->receivedSongSerial = nextSerial; }
    if (operation == Operation::AssetStart) state_->incoming = transfer;
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
    if (body.size() < 28 || body.size() > 28 + 65536 || key.size() != 32) { error = QStringLiteral("Server record length/key is invalid"); return false; }
    Reader r{body}; op = static_cast<Operation>(r.number<quint16>()); const auto version = r.number<quint8>(); flags = r.number<quint8>();
    if (version != kVersion || flags > 1 || !sequence || r.number<quint64>() != sequence) { error = QStringLiteral("Server record version, flags or sequence is invalid"); return false; }
    const auto received = r.take(16); Writer complete; complete.number<quint32>(static_cast<quint32>(body.size())); complete.bytes += body;
    complete.bytes.replace(16, 16, QByteArray(16, '\0'));
    const auto expected = QMessageAuthenticationCode::hash(complete.bytes, key, QCryptographicHash::Sha256).first(16);
    unsigned difference = 0; for (int i = 0; i < 16; ++i) difference |= static_cast<unsigned char>(received[i]) ^ static_cast<unsigned char>(expected[i]);
    if (difference) { error = QStringLiteral("Server record authentication failed"); return false; }
    payload = body.sliced(28); return true;
}

Channel::Channel(QByteArray sendKey, QByteArray receiveKey, Write write, Capacity pending, QObject* parent)
    : QObject(parent), sendKey_(std::move(sendKey)), receiveKey_(std::move(receiveKey)), write_(std::move(write)), pending_(std::move(pending))
{
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
    return !failed_ && bytes >= 0 && bytes <= kMaximumQueuedBytes && stats_.queuedBytes <= static_cast<quint64>(kMaximumQueuedBytes - bytes);
}
bool Channel::send(const QJsonObject& message)
{
    if (closeAfterDrain_) return false;
    const auto* schema = schemaFor(message.value("type").toString());
    if (!canQueue(0) || (schema && requiresReceipt(schema->operation) && awaitingReceipt_.size() >= 128)) { ++stats_.queueRejects; return false; }
    Codec::Encoded encoded; QString error;
    if (!codec_.encode(message, encoded, error, kMaximumQueuedBytes - static_cast<qsizetype>(stats_.queuedBytes))) {
        if (encoded.capacityExceeded) ++stats_.queueRejects; else ++stats_.encodingRejects;
        stats_.lastRejection = error; return false;
    }
    return enqueue(encoded);
}
bool Channel::sendAsset(const QByteArray& payload)
{
    if (closeAfterDrain_) return false;
    if (!canQueue(kChunkBytes + 128)) { ++stats_.queueRejects; return false; }
    Codec::Encoded encoded; QString error;
    if (!codec_.encodeAsset(payload, encoded, error)) { ++stats_.encodingRejects; stats_.lastRejection = error; return false; }
    return enqueue(encoded);
}
bool Channel::sendBinary(const QByteArray& payload)
{
    const auto required = payload.size() + 128;
    if (closeAfterDrain_ || payload.isEmpty() || payload.size() > 65536 ||
        !canQueue(required) || pending_() + static_cast<qint64>(stats_.queuedBytes) > kStreamQueueBytes - required) {
        ++stats_.queueRejects; return false;
    }
    return enqueue({Operation::Binary, payload});
}
bool Channel::enqueue(const Codec::Encoded& encoded)
{
    if (sendSequence_ >= UINT64_MAX - 256) { failed_ = true; if (onFailure) onFailure(QStringLiteral("Server wire counter exhausted")); return false; }
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
    flush(); return true;
}
void Channel::flush()
{
    if (failed_) return;
    for (int turn = 0; turn < 16 && !output_.empty(); ++turn) {
        if (pending_() > kStreamQueueBytes - output_.front().size()) break;
        if (!write_(output_.front())) break;
        stats_.wireBytesSent += output_.front().size(); ++stats_.recordsSent;
        stats_.queuedBytes -= output_.front().size(); output_.pop_front();
    }
    if (output_.empty()) {
        flushTimer_.stop();
        if (closeAfterDrain_) { auto close = std::move(closeAfterDrain_); failed_ = true; close(); }
    } else if (!flushTimer_.isActive()) flushTimer_.start();
}
void Channel::closeAfterDrain(std::function<void()> close)
{
    closeAfterDrain_ = std::move(close); flush();
}
bool Channel::receive(const QByteArray& body, QString& error)
{
    if (failed_) { error = QStringLiteral("Server channel failed"); return false; }
    Operation operation{}; quint8 flags = 0; QByteArray payload;
    if (!verify(body, receiveSequence_, receiveKey_, operation, flags, payload, error)) return false;
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
    Writer receipt; receipt.number<quint64>(id);
    receipt.number<quint8>(static_cast<quint8>(decision.rejection == Rejection::None ? ReceiptStage::Dispatched : ReceiptStage::Rejected));
    if (decision.rejection != Rejection::None) {
        receipt.number<quint8>(static_cast<quint8>(decision.rejection)); receipt.text(decision.reason.left(256));
    }
    if (sendSequence_ == UINT64_MAX) { error = QStringLiteral("Server receipt counter exhausted"); failed_ = true; return false; }
    auto frame = authenticate(Operation::Receipt, sendSequence_, receipt.bytes, sendKey_);
    if (!canQueue(frame.size())) { error = QStringLiteral("Server receipt queue is full"); failed_ = true; return false; }
    ++sendSequence_;
    stats_.queuedBytes += frame.size(); output_.push_back(std::move(frame)); ++stats_.receiptsSent;
    flush(); return true;
}
Stats Channel::stats() const
{
    auto result = stats_; const auto& encoding = codec_.stats();
    result.checkpointsSent = encoding.checkpointsSent; result.deltasSent = encoding.deltasSent;
    result.savedSnapshotBytes = encoding.savedSnapshotBytes; result.assetHeaderBytesSaved = encoding.assetHeaderBytesSaved;
    result.assetMetadataBytesSaved = encoding.assetMetadataBytesSaved;
    return result;
}
void accumulate(Stats& t, const Stats& v) noexcept
{
    t.recordsSent += v.recordsSent; t.recordsReceived += v.recordsReceived; t.wireBytesSent += v.wireBytesSent;
    t.receiptsSent += v.receiptsSent; t.receiptsReceived += v.receiptsReceived;
    t.lastDispatched = std::max(t.lastDispatched, v.lastDispatched);
    t.checkpointsSent += v.checkpointsSent; t.deltasSent += v.deltasSent; t.savedSnapshotBytes += v.savedSnapshotBytes;
    t.assetHeaderBytesSaved += v.assetHeaderBytesSaved; t.queueRejects += v.queueRejects;
    t.assetMetadataBytesSaved += v.assetMetadataBytesSaved;
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
        {"snapshot_deltas_sent", static_cast<qint64>(s.deltasSent)}, {"snapshot_bytes_saved", static_cast<qint64>(s.savedSnapshotBytes)},
        {"wav_payload_header_bytes_saved", static_cast<qint64>(s.assetHeaderBytesSaved)}, {"wav_metadata_bytes_saved", static_cast<qint64>(s.assetMetadataBytesSaved)},
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
} // namespace jam2::application::server_wire
