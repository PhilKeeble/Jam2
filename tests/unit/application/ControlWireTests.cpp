#include "ControlWire.hpp"
#include "ControlProtocol.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCborValue>
#include <QMessageAuthenticationCode>
#include <QtEndian>

#include <deque>
#include <iostream>
#include <stdexcept>

namespace wire = jam2::application::control_wire;
void controlWireCoverageFixtures();
void controlWireRefinementFixtures();
void controlWireRefinementPerformance();
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
        encoded.payload.toHex() == "0101020304", "golden positional heartbeat bytes changed");
    const auto golden = wire::authenticate(encoded.operation, 0x0102030405060708ULL, encoded.payload, QByteArray(32, '\0'));
    require(golden.toHex() == "000000210003050001020304050607084bdf4eb1c21190ca0ed923d07091bd970101020304",
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
    require(booleanSender.encode(badBool, encoded, error), "encode boolean"); encoded.payload[1] = 2;
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
    QByteArray bomb(6, '\0');
    bomb[0] = 2;
    bomb[1] = 1;
    qToBigEndian(static_cast<quint32>(compressed.size()), reinterpret_cast<uchar*>(bomb.data() + 2));
    bomb += compressed;
    require(!booleanReceiver.decode(wire::Operation::ShareOffer, bomb, decoded, error),
        "compressed payload expanded beyond its declared output bound");
    auto structured = song();
    structured["song"] = QJsonObject{{"source_frames", QStringLiteral("9223372036854775807")},
        {"start_frame", QStringLiteral("-1")}, {"stop_frame", 12}, {"loop_start_frame", QStringLiteral("0001")},
        {"custom_recipe_field", QJsonArray{1, "kept", QJsonObject{{"arbitrary", true}}}}};
    wire::Codec framesEncoder, framesDecoder;
    require(framesEncoder.encode(structured, encoded, error) && framesDecoder.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == structured,
        "numeric snapshot fields changed application types, i64 precision or unknown recipe keys");
    require(!framesEncoder.encode({{"type","looper.recording.state"},{"phase","unknown-phase"}}, encoded, error), "unknown enum accepted");
    // Rename optimization must fall back when deletion/replacement or any other
    // field changes alongside a name, and remain bound to its exact base.
    auto multi = rename(message, "renamed-after-replace"); auto content = multi.value("song").toObject();
    content["title"] = "other simultaneous edit"; multi["song"] = content;
    require(sender.encode(multi, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == multi, "typed rename swallowed another edit");
    auto removed = multi; content.remove("looper"); removed["song"] = content;
    require(sender.encode(removed, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == removed, "deletion did not supersede rename baseline");
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
    require(receiver.encode(progress, encoded, error) && encoded.payload.size() == 7 &&
        sender.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == progress,
        "WAV progress repeated the full hash or lost its transfer binding");
    const auto final = legacyChunk(hash, 1, QByteArray(14, 'z'));
    require(sender.encodeAsset(final, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.asset == final, "final short chunk round trip");
    require(!sender.encodeAsset(legacyChunk(hash, 1, QByteArray(15, 'z')), encoded, error), "oversized final chunk accepted");
    const QJsonObject done{{"type", "looper.asset.done"}, {"sha256", QString::fromLatin1(hash.toHex())}, {"chunks", 2}};
    require(sender.encode(done, encoded, error) && encoded.payload.size() == 7 &&
        receiver.decode(encoded.operation, encoded.payload, decoded, error) && decoded.message == done,
        "WAV completion metadata did not use its transfer handle");
    require(sender.encode(start, encoded, error) && receiver.decode(encoded.operation, encoded.payload, decoded, error), "replacement transfer start");
    require(!receiver.decode(oldChunk.operation, oldChunk.payload, decoded, error), "retired transfer chunk accepted");
}
void channels()
{
    std::deque<QByteArray> toA, toB; qint64 stalled = 0;
    const QByteArray keyAB(32, 'a'), keyBA(32, 'b');
    const wire::Channel::Options immediate{false, false, false};
    wire::Channel a(keyAB, keyBA, [&](const QByteArray& f) { toB.push_back(f); return true; }, [&] { return stalled; }, immediate);
    wire::Channel b(keyBA, keyAB, [&](const QByteArray& f) { toA.push_back(f); return true; }, [] { return qint64{0}; }, immediate);
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
void deliver(std::deque<QByteArray>& queue, wire::Channel& receiver)
{
    QString error;
    while (!queue.empty()) {
        auto bytes = std::move(queue.front()); queue.pop_front();
        while (!bytes.isEmpty()) {
            require(bytes.size() >= 4, "batch split length");
            const auto size = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData()));
            require(bytes.size() >= size + 4 && receiver.receive(bytes.mid(4, size), error), "batched channel receive");
            bytes.remove(0, size + 4);
        }
    }
}
void editBursts(bool benchmark)
{
    for (int mode = 0; mode < 4; ++mode) {
        wire::Channel::Options options{mode != 0, mode >= 3, mode >= 2};
        std::deque<QByteArray> toA, toB;
        wire::Channel a(QByteArray(32, 'a'), QByteArray(32, 'b'), [&](const QByteArray& f) { toB.push_back(f); return true; }, [] { return qint64{0}; }, options);
        wire::Channel b(QByteArray(32, 'b'), QByteArray(32, 'a'), [&](const QByteArray& f) { toA.push_back(f); return true; }, [] { return qint64{0}; }, options);
        int deliveries = 0; QString latest;
        b.onMessage = [&](const QJsonObject& m) { ++deliveries; latest = m.value("text").toString(); };
        QElapsedTimer timer; timer.start();
        constexpr int turns = 32, burst = 32;
        for (int turn = 0; turn < turns; ++turn) {
            for (int i = 0; i < burst; ++i) require(a.send({{"type", "beat.set"}, {"section", 0}, {"lane", "chord"}, {"beat", 1}, {"text", QString::number(i)}}), "queue edit burst");
            QCoreApplication::processEvents(); deliver(toB, b);
            QCoreApplication::processEvents(); deliver(toA, a);
            require(latest == "31" && a.stats().pendingReceipts == 0, "burst final state/result did not converge");
        }
        const auto elapsed = timer.nsecsElapsed(); const auto sa = a.stats(), sb = b.stats();
        require(deliveries == turns * (mode >= 3 ? 1 : burst), "coalescing lost a non-superseded assignment");
        const auto writes = sa.transportWrites + sb.transportWrites;
        // Each small write is exactly one reliable data datagram. Include the
        // unchanged 58-byte mux header/tag + IPv4/UDP28, and one 86-byte ACK.
        if (benchmark) std::cout << "mode=burst-" << mode << " operations=" << turns * burst << " dispatched=" << deliveries
            << " authenticated_bytes=" << sa.wireBytesSent + sb.wireBytesSent << " data_and_ack_datagrams=" << writes * 2
            << " ipv4_bytes_with_acks=" << sa.wireBytesSent + sb.wireBytesSent + writes * 172 << " cpu_ns=" << elapsed
            << " assignment_delay_mean_us=" << sa.assignmentDelaySumUs / std::max<quint64>(1,sa.assignmentDelaySamples)
            << " assignment_delay_max_us=" << sa.assignmentDelayMaxUs << '\n';
        if (mode == 3) require(sa.coalescedUpdates == turns * (burst - 1) && sb.receiptsSent == turns, "coalesced update metrics incorrect");
    }
    // A transaction is a barrier: the old assignment must dispatch before it.
    std::deque<QByteArray> toA, toB;
    wire::Channel a(QByteArray(32,'a'), QByteArray(32,'b'), [&](const auto& f) { toB.push_back(f); return true; }, [] { return qint64{0}; });
    wire::Channel b(QByteArray(32,'b'), QByteArray(32,'a'), [&](const auto& f) { toA.push_back(f); return true; }, [] { return qint64{0}; });
    QStringList seen;
    b.onMessage = [&](const QJsonObject& m) { seen.push_back(m.value("type").toString() + m.value("text").toString()); };
    require(a.send({{"type","beat.set"},{"text","old"}}) && a.send({{"type","bank.request"},{"bank",0}}) &&
        a.send({{"type","beat.set"},{"text","new"}}), "barrier queue");
    QCoreApplication::processEvents(); deliver(toB,b); QCoreApplication::processEvents(); deliver(toA,a);
    require(seen == QStringList{"beat.setold","bank.request","beat.setnew"}, "coalescing crossed a transaction barrier");
    seen.clear();
    b.onMessage = [&](const QJsonObject& m) { seen.push_back(QString::number(m.value("revision").toInt())); };
    require(a.send({{"type","beat.set"},{"beat",1},{"text","first"},{"revision",1}}) &&
        a.send({{"type","beat.set"},{"beat",2},{"text","other cell"},{"revision",2}}) &&
        a.send({{"type","beat.set"},{"beat",1},{"text","latest"},{"revision",3}}), "interleaved revision burst");
    QCoreApplication::processEvents(); deliver(toB,b); QCoreApplication::processEvents(); deliver(toA,a);
    require(seen == QStringList{"2","3"}, "coalescing reordered surviving authority revisions");
    // A sparse assignment is scheduled once; it never waits for a second edit.
    seen.clear();
    require(a.send({{"type","beat.set"},{"revision",4}}) && toB.empty(), "sparse assignment did not use event-loop scheduling");
    QCoreApplication::processEvents(); deliver(toB,b);
    require(seen == QStringList{"4"} && a.stats().assignmentDelaySamples > 0, "sparse assignment waited for another edit or lost delay diagnostics");
    seen.clear();
    // The first record for an identity can be superseded, including by an
    // assignment for another cell. Definitions must survive in wire order.
    require(a.send({{"type","beat.set"},{"beat",1},{"revision",5},{"authority_token",QString(32,'a')}}) &&
        a.send({{"type","beat.set"},{"beat",2},{"revision",6},{"authority_token",QString(32,'b')}}) &&
        a.send({{"type","beat.set"},{"beat",1},{"revision",7},{"authority_token",QString(32,'a')}}),"coalesced identity queue");
    QCoreApplication::processEvents(); deliver(toB,b); QCoreApplication::processEvents(); deliver(toA,a);
    require(seen==QStringList{"6","7"} && a.stats().peerDefinitions==0,"superseded scalar introduced a peer dictionary dependency");
    require(a.send({{"type","session.connection.failure"},{"a",QString(32,'b')},{"b",QString(32,'a')}}),"peer definition after coalescing");
    deliver(toB,b); QCoreApplication::processEvents(); deliver(toA,a); seen.clear();
    require(a.send({{"type","beat.set"},{"revision",8},{"authority_token",QString(32,'a')}}),"established peer scalar reference");
    QCoreApplication::processEvents(); deliver(toB,b); QCoreApplication::processEvents(); deliver(toA,a);
    require(seen==QStringList{"8"} && a.stats().peerDefinitions==2 && a.stats().peerReferences>0,"literal scalar polluted later peer handle numbering");
    QCoreApplication::processEvents(); deliver(toA,a);
    // A rejected action between successes must keep its explicit result even
    // when both neighboring successes would otherwise share one cumulative ACK.
    b.onMessage = {};
    b.validateMessage = [](const QJsonObject& m) { return m.value("enabled").toBool() ? wire::DispatchDecision{} :
        wire::DispatchDecision{wire::Rejection::Unauthorized,QStringLiteral("rejected middle action")}; };
    const auto receipts = b.stats().receiptsSent, results = a.stats().rejectionResultsReceived;
    for (const bool value : {true,false,true}) require(a.send({{"type","jam.metronome.state.set"},{"enabled",value}}), "queue mixed dispatch outcomes");
    deliver(toB,b); QCoreApplication::processEvents(); deliver(toA,a);
    require(b.stats().receiptsSent == receipts + 2 && a.stats().rejectionResultsReceived == results + 1 && a.stats().pendingReceipts == 0,
        "cumulative success concealed a rejection or lost dispatch confirmation");
}
void independentSafety()
{
    const QByteArray ab(32,'a'), ba(32,'b');
    std::deque<QByteArray> primary, independent;
    wire::Channel a(ab,ba,[&](const auto& bytes) { primary.push_back(bytes); return true; }, [] { return qint64{0}; });
    wire::Channel b(ba,ab,[](const auto&) { return true; }, [] { return qint64{0}; });
    a.enableIndependent([&](const auto& bytes) { independent.push_back(bytes); return true; }, [] { return qint64{0}; });
    b.enableIndependent([](const auto&) { return true; }, [] { return qint64{0}; });
    require(a.send(song()) && a.send({{"type","session.heartbeat"},{"sequence",17}}), "independent send");
    require(primary.size() == 1 && independent.size() == 1, "ordering selected wrong wire");
    QString error; int delivered = 0;
    b.onMessage = [&](const auto& message) { require(message.value("sequence") == 17, "unexpected independent operation"); ++delivered; };
    require(!b.receive(independent.front().sliced(4), error), "independent record replayed on primary ordering");
    auto tampered = independent.front(); tampered.back() ^= 1;
    require(!b.receiveIndependent(tampered, error), "independent HMAC tampering accepted");
    require(b.receiveIndependent(independent.front().first(3),error) && b.receiveIndependent(independent.front().sliced(3),error) && delivered == 1,
        "independent framing splits failed");
    require(!b.receiveIndependent(independent.front(),error), "independent sequence replay accepted");
    wire::Channel restricted(ba,ab,[](const auto&) { return true; }, [] { return qint64{0}; });
    restricted.enableIndependent([](const auto&) { return true; }, [] { return qint64{0}; });
    wire::Codec codec; wire::Codec::Encoded encoded;
    require(codec.encode(song(),encoded,error), "prepare forbidden independent operation");
    const auto key = QMessageAuthenticationCode::hash(QByteArrayLiteral("jam2-independent-control-v")+QByteArray::number(wire::kVersion), ab, QCryptographicHash::Sha256);
    require(!restricted.receiveIndependent(wire::authenticate(encoded.operation,1,encoded.payload,key),error), "dependent checkpoint accepted on independent ordering");
    wire::Channel stalled(ab,ba,[](const auto&) { return true; }, [] { return qint64{0}; });
    stalled.enableIndependent([](const auto&) { return false; }, [] { return qint64{16*1024}; });
    bool rejected = false;
    for (int i=0; i<1000; ++i) if (!stalled.send({{"type","session.heartbeat"},{"sequence",i}})) { rejected = true; break; }
    require(rejected && stalled.stats().independentQueuedBytes <= 16*1024 && stalled.stats().queueRejects > 0,
        "independent application queue has no backpressure bound");
}
void performance()
{
    constexpr int count = 1000;
    const QByteArray key(32, 'k');
    for (const bool numeric : {false,true}) for (const bool compress : {false,true}) {
        quint64 bytes = 0; QElapsedTimer timer; timer.start();
        for (int checkpoint = 0; checkpoint < 100; ++checkpoint) {
            wire::Codec::Options options; options.numericDocuments = numeric;
            if (!compress) options.compressionMinimumBytes = wire::kMaximumDocumentBytes;
            wire::Codec encoder(options), decoder; wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
            const auto message = rename(song(64), QString::number(checkpoint));
            require(encoder.encode(message,encoded,error) && decoder.decode(encoded.operation,encoded.payload,decoded,error) && decoded.message == message,
                "checkpoint numeric/text ablation convergence");
            bytes += encoded.payload.size() + wire::kRecordHeaderBytes;
        }
        std::cout << "workload=100-fresh-checkpoints numeric=" << numeric << " compressed=" << compress
            << " authenticated_bytes=" << bytes << " encode_decode_ns=" << timer.nsecsElapsed() << '\n';
    }
    for (const auto& mode : {"typed", "typed-text-snapshots", "typed-compress-small", "typed-generic-edits"}) {
        wire::Codec::Options options;
        if (QByteArrayView(mode) == "typed-text-snapshots") options.numericDocuments = false;
        if (QByteArrayView(mode) == "typed-compress-small") options.compressionMinimumBytes = 0;
        if (QByteArrayView(mode) == "typed-generic-edits") options.typedRenames = false;
        wire::Codec encoder(options), decoder;
        quint64 bytes = 0; QElapsedTimer timer; timer.start();
        auto message = song(64); QString error;
        for (int i = 0; i < count; ++i) {
            message = rename(message, QStringLiteral("Take-%1").arg(i)); message["arrangement_revision"] = i + 1;
                wire::Codec::Encoded encoded; wire::Codec::Decoded decoded;
                require(encoder.encode(message, encoded, error), "benchmark binary encode");
                const auto frame = wire::authenticate(encoded.operation, i + 1, encoded.payload, key);
                wire::Operation operation{}; quint8 flags = 0; QByteArray body;
                require(wire::verify(frame.sliced(4), i + 1, key, operation, flags, body, error) && decoder.decode(operation, body, decoded, error) && decoded.message == message, "benchmark binary convergence");
                bytes += frame.size();

        }
        std::cout << "mode=" << mode << " workload=64-lane-rename operations=" << count
            << " authenticated_bytes=" << bytes << " encode_decode_ns=" << timer.nsecsElapsed()
            << " compression_attempts=" << encoder.stats().compressionAttempts << " compression_skipped=" << encoder.stats().compressionSkipped << '\n';
        if (QByteArrayView(mode) == "typed") require(bytes < 200000, "ordinary renames retained full snapshot wire cost");
    }
    for (const auto& mode : {"typed"}) {
        wire::Codec sender, receiver; wire::Codec::Encoded encoded; wire::Codec::Decoded decoded;
        QString error; quint64 bytes = 0, sequence = 1;
        const QByteArray file(1024 * 1024, 'w'), hash(32, 'a');
        const QString hashText = QString::fromLatin1(hash.toHex());
        const auto metadata = [&](const QJsonObject& message, bool reverse) {
                auto& encoder = reverse ? receiver : sender;
                auto& decoder = reverse ? sender : receiver;
                require(encoder.encode(message, encoded, error), "WAV metadata benchmark encode");
                const auto frame = wire::authenticate(encoded.operation, sequence++, encoded.payload, key);
                wire::Operation operation{}; quint8 flags = 0; QByteArray payload;
                require(wire::verify(frame.sliced(4), sequence - 1, key, operation, flags, payload, error) &&
                    decoder.decode(operation, payload, decoded, error) && decoded.message == message, "WAV metadata benchmark round trip");
                bytes += frame.size();

        };
        QElapsedTimer timer; timer.start();
        metadata({{"type", "looper.asset.start"}, {"sha256", hashText}, {"file_bytes", file.size()}, {"chunk_size", 24576}}, false);
        quint32 chunks = 0;
        for (qsizetype offset = 0; offset < file.size(); offset += 24576) {
            const auto chunk = legacyChunk(hash, chunks, file.mid(offset, 24576));
                require(sender.encodeAsset(chunk, encoded, error), "WAV benchmark encode chunk");
                const auto frame = wire::authenticate(encoded.operation, sequence++, encoded.payload, key);
                wire::Operation operation{}; quint8 flags = 0; QByteArray payload;
                require(wire::verify(frame.sliced(4), sequence - 1, key, operation, flags, payload, error) &&
                    receiver.decode(operation, payload, decoded, error) && decoded.asset == chunk, "WAV benchmark binary bytes changed");
                bytes += frame.size();

            metadata({{"type", "looper.asset.ack"}, {"sha256", hashText}, {"chunks", static_cast<qint64>(++chunks)}}, true);
        }
        metadata({{"type", "looper.asset.done"}, {"sha256", hashText}, {"chunks", static_cast<qint64>(chunks)}}, false);
        std::cout << "mode=" << mode << " workload=wav-transfer-codec raw_bytes=" << file.size() << " chunks=" << chunks
            << " authenticated_bytes_both_directions=" << bytes << " overhead_bytes=" << bytes - file.size()
            << " encode_decode_ns=" << timer.nsecsElapsed() << '\n';
        require(bytes-file.size()<8192,"typed WAV overhead exceeded its bounded transfer budget");
    }
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        if (argc > 1 && QByteArrayView(argv[1]) == "--performance") { performance(); editBursts(true); controlWireRefinementPerformance(); }
        else { controlWireCoverageFixtures(); controlWireRefinementFixtures(); recordsAndDeltas(); assets(); channels(); editBursts(false); independentSafety(); }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
