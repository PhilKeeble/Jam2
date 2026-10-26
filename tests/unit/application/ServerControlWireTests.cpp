#include "ServerControlWire.hpp"
#include "ControlProtocol.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCborValue>
#include <QtEndian>

#include <deque>
#include <iostream>
#include <stdexcept>

namespace wire = jam2::application::server_wire;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
QJsonObject song(int lanes = 12)
{
    QJsonArray entries;
    for (int i = 0; i < lanes; ++i) entries.append(QJsonObject{{"id", QStringLiteral("lane-%1").arg(i)},
        {"name", QStringLiteral("Recording %1").arg(i)}, {"asset_hash", QString(64, 'a')},
        {"source_frames", QStringLiteral("1500000")}, {"start_frame", QStringLiteral("0")},
        {"stop_frame", QStringLiteral("-1")}, {"loop_start_frame", QStringLiteral("0")},
        {"loop_end_frame", QStringLiteral("1000000")}, {"loop_enabled", true}, {"gain_db", -3.0}});
    return {{"type", "song.set"}, {"song", QJsonObject{{"title", QString::fromUtf8("Étude 🎸")},
        {"looper", QJsonObject{{"banks", QJsonArray{QJsonObject{{"id", "A"}, {"lanes", entries}}}}}}}},
        {"sync_scope", "tracks"}, {"arrangement_revision", 1}, {"host_authoritative", true}, {"track_playing", false}};
}
QJsonObject rename(QJsonObject message, const QString& name)
{
    auto document = message.value("song").toObject(); auto looper = document.value("looper").toObject();
    auto banks = looper.value("banks").toArray(); auto bank = banks[0].toObject(); auto lanes = bank.value("lanes").toArray();
    auto lane = lanes[0].toObject(); lane.insert("name", name); lanes[0] = lane; bank["lanes"] = lanes;
    banks[0] = bank; looper["banks"] = banks; document["looper"] = looper; message["song"] = document; return message;
}
void recordsAndDeltas()
{
    wire::Codec sender, receiver;
    wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
    require(sender.encode({{"type", "session.heartbeat"}, {"sequence", 0x01020304}}, encoded, error) &&
        encoded.payload.toHex() == "0000000101020304", "golden positional heartbeat bytes changed");
    const auto golden = wire::authenticate(encoded.operation, 0x0102030405060708ULL, encoded.payload, QByteArray(32, '\0'));
    require(golden.toHex() == "00000024000301000102030405060708388ccb3044aabb9d5ff46a9f427d24bf0000000101020304",
        "golden big-endian/HMAC record changed");
    auto message = song();
    require(sender.encode(message, encoded, error), "encode checkpoint");
    const auto firstSize = encoded.payload.size();
    require(receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == message, "checkpoint did not round trip");
    message = rename(message, QString::fromUtf8("Renamed 🎵")); message["arrangement_revision"] = 2;
    require(sender.encode(message, encoded, error), "encode delta");
    require(encoded.payload.size() < firstSize, "rename sent another full checkpoint");
    wire::Codec fresh;
    require(!fresh.decode(encoded.operation, encoded.payload, decoded, error), "unbound delta accepted");
    require(receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == message, "delta did not converge");
    auto invalid = message; invalid["unrecognized"] = 3;
    require(!sender.encode(invalid, encoded, error), "unknown field accepted");
    require(!sender.encode(message, encoded, error, 1) && encoded.capacityExceeded,
        "output capacity must be checked before advancing the snapshot binding");
    message = rename(message, "after-rejection");
    require(sender.encode(message, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == message, "rejected encode advanced snapshot binding");

    const QByteArray key(32, 'k');
    const auto frame = wire::authenticate(encoded.operation, 9, encoded.payload, key);
    require(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(frame.constData())) == frame.size() - 4, "big-endian frame length");
    wire::Operation operation{}; quint8 flags = 0; QByteArray payload;
    require(wire::verify(frame.sliced(4), 9, key, operation, flags, payload, error) && payload == encoded.payload, "authenticated record round trip");
    require(!wire::verify(frame.sliced(4), 8, key, operation, flags, payload, error), "sequence replay accepted");
    auto damaged = frame; damaged[damaged.size() - 1] ^= 1;
    require(!wire::verify(damaged.sliced(4), 9, key, operation, flags, payload, error), "tampered payload accepted");
    require(!wire::verify(frame.sliced(4), 9, QByteArray(32, 'x'), operation, flags, payload, error), "wrong direction key accepted");
    auto extra = encoded.payload + 'x';
    require(!fresh.decode(wire::Operation::Song, extra, decoded, error), "trailing garbage accepted");
    require(!fresh.decode(static_cast<wire::Operation>(65535), {}, decoded, error), "unknown mandatory operation accepted");
    const QJsonObject badBool{{"type", "jam.metronome.state.set"}, {"enabled", true}};
    const QJsonObject narrow{{"type", "beat.hit"}, {"section", 255}, {"beat", 65535}, {"lane", 255}, {"text", "x"}};
    require(sender.encode(narrow, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == narrow,
        "u8/u16 wire fields did not preserve their boundary values");
    auto wide = narrow; wide["section"] = 256;
    require(!sender.encode(wide, encoded, error), "u8 overflow was truncated");
    wide = narrow; wide["beat"] = 65536;
    require(!sender.encode(wide, encoded, error), "u16 overflow was truncated");
    wire::Codec booleanSender, booleanReceiver;
    require(booleanSender.encode(badBool, encoded, error), "encode boolean"); encoded.payload[4] = 2;
    require(!booleanReceiver.decode(encoded.operation, encoded.payload, decoded, error), "invalid boolean accepted");
    const QJsonObject listening{{"type", "jam.listening.request"}, {"changed_groups", 1},
        {"settings", QJsonObject{{"speed_ppm", 300000}, {"pitch_cents", -100}, {"focus_enabled", true},
            {"focus_preset", "custom"}, {"focus_hz", 120.0}, {"focus_gain_db", 12.0}, {"focus_q", 6.0}}}};
    require(booleanSender.encode(listening, encoded, error) && encoded.payload.size() < 64 &&
        booleanReceiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == listening,
        "listening settings did not use/round-trip the compact fixed numeric layout");
    const QJsonObject batchResult{{"type", "looper.track.batch.complete"}, {"batch_id", "12345678-1234-1234-1234-123456789abc"},
        {"tracks", 2}, {"applied", false}, {"reason", "superseded-by-arrangement"}};
    require(booleanSender.encode(batchResult, encoded, error) && booleanReceiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == batchResult,
        "application batch success/rejection acknowledgement fields were lost");
    auto badIdentity = batchResult; badIdentity["batch_id"] = QString(36, 'x');
    require(!booleanSender.encode(badIdentity, encoded, error), "malformed UUID was normalized into a valid identity");
    require(!booleanSender.encode({{"type", "looper.asset.unavailable"}, {"sha256", QString(63, 'a') + 'g'}, {"reason", "missing"}}, encoded, error),
        "invalid hexadecimal hash was silently changed while encoding");
    auto compressed = qCompress(QCborValue::fromJsonValue(QJsonArray{QString(10000, 'x')}).toCbor(), 1);
    qToBigEndian(quint32{1}, reinterpret_cast<uchar*>(compressed.data()));
    QByteArray bomb(9, '\0');
    qToBigEndian(quint32{2}, reinterpret_cast<uchar*>(bomb.data()));
    bomb[4] = 1;
    qToBigEndian(static_cast<quint32>(compressed.size()), reinterpret_cast<uchar*>(bomb.data() + 5));
    bomb += compressed;
    require(!booleanReceiver.decode(wire::Operation::ShareOffer, bomb, decoded, error),
        "compressed payload expanded beyond its declared output bound");
}
QByteArray legacyChunk(const QByteArray& hash, quint32 index, const QByteArray& data)
{
    QByteArray bytes = hash; bytes.resize(48);
    qToBigEndian(index, reinterpret_cast<uchar*>(bytes.data() + 32));
    qToBigEndian(quint64{index} * 24576, reinterpret_cast<uchar*>(bytes.data() + 36));
    qToBigEndian(static_cast<quint32>(data.size()), reinterpret_cast<uchar*>(bytes.data() + 44));
    bytes += data; return bytes;
}
void assets()
{
    wire::Codec sender, receiver; wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
    const QByteArray hash(32, 'a');
    const auto chunk = legacyChunk(hash, 0, QByteArray(24576, 'w'));
    require(!sender.encodeAsset(chunk, encoded, error), "chunk without start accepted");
    const QJsonObject start{{"type", "looper.asset.start"}, {"sha256", QString::fromLatin1(hash.toHex())}, {"file_bytes", 24590}, {"chunk_size", 24576}};
    require(sender.encode(start, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == start, "asset start round trip");
    require(sender.encodeAsset(chunk, encoded, error) && encoded.payload.size() == chunk.size() - 42, "asset header did not save forty-two bytes");
    const auto oldChunk = encoded;
    require(receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.asset == chunk, "compact chunk reconstruction differs");
    const QJsonObject progress{{"type", "looper.asset.ack"}, {"sha256", QString::fromLatin1(hash.toHex())}, {"chunks", 1}};
    require(receiver.encode(progress, encoded, error) && encoded.payload.size() == 10 &&
        sender.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == progress,
        "WAV progress repeated the full hash or lost its transfer binding");
    const auto final = legacyChunk(hash, 1, QByteArray(14, 'z'));
    require(sender.encodeAsset(final, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.asset == final, "final short chunk round trip");
    require(!sender.encodeAsset(legacyChunk(hash, 1, QByteArray(15, 'z')), encoded, error), "oversized final chunk accepted");
    const QJsonObject done{{"type", "looper.asset.done"}, {"sha256", QString::fromLatin1(hash.toHex())}, {"chunks", 2}};
    require(sender.encode(done, encoded, error) && encoded.payload.size() == 10 &&
        receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == done,
        "WAV completion metadata did not use its transfer handle");
    require(sender.encode(start, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error), "replacement transfer start");
    require(!receiver.decode(oldChunk.operation, oldChunk.payload, decoded, error), "retired transfer chunk accepted");
}
void channels()
{
    std::deque<QByteArray> toA, toB; qint64 stalled = 0;
    const QByteArray keyAB(32, 'a'), keyBA(32, 'b');
    wire::Channel a(keyAB, keyBA, [&](const QByteArray& f) { toB.push_back(f); return true; }, [&] { return stalled; });
    wire::Channel b(keyBA, keyAB, [&](const QByteArray& f) { toA.push_back(f); return true; }, [] { return qint64{0}; });
    QJsonObject received; int deliveries = 0;
    b.onMessage = [&](const QJsonObject& m) { received = m; ++deliveries; };
    QString error;
    const auto message = song(128);
    require(a.send(message), "channel send");
    require(a.stats().pendingReceipts == 1 && deliveries == 0, "queueing pretended to apply/acknowledge");
    while (!toB.empty()) { const auto f = toB.front(); toB.pop_front(); require(b.receive(f.sliced(4), error), "channel receive"); }
    require(received == message && deliveries == 1 && a.stats().pendingReceipts == 1, "message delivery/result distinction");
    require(!toA.empty(), "dispatch receipt absent");
    while (!toA.empty()) { const auto f = toA.front(); toA.pop_front(); require(a.receive(f.sliced(4), error), "receipt receive"); }
    require(a.stats().pendingReceipts == 0 && a.stats().receiptsReceived == 1 && toB.empty(), "receipt loop or missing receipt");
    b.validateMessage = [](const QJsonObject&) { return wire::DispatchDecision{wire::Rejection::Unauthorized, QStringLiteral("creator authority required")}; };
    require(a.send({{"type", "jam.metronome.state.set"}, {"enabled", true}}), "queue rejected mutation");
    while (!toB.empty()) { const auto f = toB.front(); toB.pop_front(); require(b.receive(f.sliced(4), error), "receive rejected mutation"); }
    while (!toA.empty()) { const auto f = toA.front(); toA.pop_front(); require(a.receive(f.sliced(4), error), "receive typed rejection result"); }
    require(deliveries == 1 && b.stats().operationsRejected == 1 && a.stats().rejectionResultsReceived == 1 &&
        a.stats().pendingReceipts == 0 && a.stats().resultLatencySamples == 2,
        "rejected mutation reached application, lost its result, or pretended to commit");
    stalled = wire::kStreamQueueBytes;
    require(a.send({{"type", "session.heartbeat"}, {"sequence", 1}}), "enqueue while stream full");
    bool closed = false; a.closeAfterDrain([&] { closed = true; });
    require(!closed && a.stats().queuedBytes > 0, "drain closed before output accepted");
    stalled = 0;
    QElapsedTimer wait; wait.start();
    while (!closed && wait.elapsed() < 1000) QCoreApplication::processEvents();
    require(closed, "queued output did not resume after backpressure");
}
void performance()
{
    constexpr int count = 1000;
    const QByteArray key(32, 'k');
    for (const auto& mode : {"direct", "stun", "server"}) {
        wire::Codec encoder, decoder;
        quint64 bytes = 0; QElapsedTimer timer; timer.start();
        auto message = song(64); QString error;
        for (int i = 0; i < count; ++i) {
            message = rename(message, QStringLiteral("Take-%1").arg(i)); message["arrangement_revision"] = i + 1;
            if (QByteArrayView(mode) == "server") {
                wire::Codec::Encoded encoded; wire::Codec::Decoded decoded;
                require(encoder.encode(message, encoded, error), "benchmark binary encode");
                const auto frame = wire::authenticate(encoded.operation, i + 1, encoded.payload, key);
                wire::Operation operation{}; quint8 flags = 0; QByteArray body;
                require(wire::verify(frame.sliced(4), i + 1, key, operation, flags, body, error) && decoder.decode(operation, body, decoded, error) && decoded.message == message, "benchmark binary convergence");
                bytes += frame.size();
            } else {
                const auto frames = jam2::control_protocol::encodeAuthenticatedJsonFrames(message, key, 1);
                require(!frames.frames.isEmpty(), "benchmark legacy encode");
                jam2::control_protocol::LargeJsonReceiver large;
                QJsonObject received;
                quint64 sequence = 1;
                for (const auto& f : frames.frames) {
                    jam2::control_protocol::AuthenticatedPayload payload;
                    require(jam2::control_protocol::decodeAuthenticated(f.sliced(4), key, sequence++, payload, error), "benchmark legacy decode");
                    if (payload.type == jam2::control_protocol::AuthenticatedPayloadType::Json) received = payload.message;
                    else { bool ready = false; require(large.accept(payload.binary, received, ready, error), "benchmark legacy chunks"); }
                    bytes += f.size();
                }
                require(received == message, "benchmark legacy convergence");
            }
        }
        std::cout << "mode=" << mode << " workload=64-lane-rename operations=" << count
            << " authenticated_bytes=" << bytes << " encode_decode_ns=" << timer.nsecsElapsed() << '\n';
        if (QByteArrayView(mode) == "server") require(bytes < 200000, "ordinary renames retained full snapshot wire cost");
    }
    quint64 legacyWavBytes = 0;
    for (const auto& mode : {"direct", "stun", "server"}) {
        const bool binary = QByteArrayView(mode) == "server";
        wire::Codec sender, receiver; wire::Codec::Encoded encoded; wire::Codec::Decoded decoded;
        QString error; quint64 bytes = 0, sequence = 1;
        const QByteArray file(1024 * 1024, 'w'), hash(32, 'a');
        const QString hashText = QString::fromLatin1(hash.toHex());
        const auto metadata = [&](const QJsonObject& message, bool reverse) {
            if (binary) {
                auto& encoder = reverse ? receiver : sender;
                auto& decoder = reverse ? sender : receiver;
                require(encoder.encode(message, encoded, error), "WAV metadata benchmark encode");
                const auto frame = wire::authenticate(encoded.operation, sequence++, encoded.payload, key);
                wire::Operation operation{}; quint8 flags = 0; QByteArray payload;
                require(wire::verify(frame.sliced(4), sequence - 1, key, operation, flags, payload, error) &&
                    decoder.decode(operation, payload, decoded, error) && decoded.message == message, "WAV metadata benchmark round trip");
                bytes += frame.size();
            } else {
                const auto frame = jam2::control_protocol::encodeAuthenticated(message, key, sequence++);
                jam2::control_protocol::AuthenticatedPayload payload;
                require(jam2::control_protocol::decodeAuthenticated(frame.sliced(4), key, sequence - 1, payload, error) && payload.message == message,
                    "WAV legacy metadata benchmark round trip");
                bytes += frame.size();
            }
        };
        QElapsedTimer timer; timer.start();
        metadata({{"type", "looper.asset.start"}, {"sha256", hashText}, {"file_bytes", file.size()}, {"chunk_size", 24576}}, false);
        quint32 chunks = 0;
        for (qsizetype offset = 0; offset < file.size(); offset += 24576) {
            const auto chunk = legacyChunk(hash, chunks, file.mid(offset, 24576));
            if (binary) {
                require(sender.encodeAsset(chunk, encoded, error), "WAV benchmark encode chunk");
                const auto frame = wire::authenticate(encoded.operation, sequence++, encoded.payload, key);
                wire::Operation operation{}; quint8 flags = 0; QByteArray payload;
                require(wire::verify(frame.sliced(4), sequence - 1, key, operation, flags, payload, error) &&
                    receiver.decode(operation, payload, decoded, error) && decoded.asset == chunk, "WAV benchmark binary bytes changed");
                bytes += frame.size();
            } else {
                const auto frame = jam2::control_protocol::encodeAuthenticatedBinary(chunk, key, sequence++);
                jam2::control_protocol::AuthenticatedPayload payload;
                require(jam2::control_protocol::decodeAuthenticated(frame.sliced(4), key, sequence - 1, payload, error) && payload.binary == chunk,
                    "WAV benchmark legacy bytes changed");
                bytes += frame.size();
            }
            metadata({{"type", "looper.asset.ack"}, {"sha256", hashText}, {"chunks", static_cast<qint64>(++chunks)}}, true);
        }
        metadata({{"type", "looper.asset.done"}, {"sha256", hashText}, {"chunks", static_cast<qint64>(chunks)}}, false);
        std::cout << "mode=" << mode << " workload=wav-transfer-codec raw_bytes=" << file.size() << " chunks=" << chunks
            << " authenticated_bytes_both_directions=" << bytes << " overhead_bytes=" << bytes - file.size()
            << " encode_decode_ns=" << timer.nsecsElapsed() << '\n';
        if (!binary) legacyWavBytes = bytes;
        else require(bytes < legacyWavBytes, "typed WAV framing failed to reduce data/metadata overhead");
    }
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        if (argc > 1 && QByteArrayView(argv[1]) == "--performance") performance();
        else { recordsAndDeltas(); assets(); channels(); }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
