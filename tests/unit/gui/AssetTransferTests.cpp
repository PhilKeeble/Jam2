#include "AssetChunkProtocol.hpp"
#include "AssetTransferService.hpp"
#include "ContentLimits.hpp"
#include "LooperAssetFiles.hpp"
#include "TrackAssetOwnership.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QtEndian>

#include <deque>
#include <functional>
#include <iostream>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const char* name)
{
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

template <typename T>
void appendLittleEndian(QByteArray& bytes, T value)
{
    const T encoded = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char*>(&encoded),
        static_cast<qsizetype>(sizeof(encoded)));
}

QByteArray pcm16Wav(
    quint32 frames,
    qint16 amplitude,
    quint32 sampleRate = 48000)
{
    const quint32 dataBytes = frames * 2;
    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendLittleEndian<quint32>(bytes, 36 + dataBytes);
    bytes.append("WAVEfmt ", 8);
    appendLittleEndian<quint32>(bytes, 16);
    appendLittleEndian<quint16>(bytes, 1);
    appendLittleEndian<quint16>(bytes, 1);
    appendLittleEndian<quint32>(bytes, sampleRate);
    appendLittleEndian<quint32>(bytes, sampleRate * 2);
    appendLittleEndian<quint16>(bytes, 2);
    appendLittleEndian<quint16>(bytes, 16);
    bytes.append("data", 4);
    appendLittleEndian<quint32>(bytes, dataBytes);
    for (quint32 frame = 0; frame < frames; ++frame) {
        appendLittleEndian<qint16>(bytes, frame % 2 == 0 ? amplitude : -amplitude);
    }
    return bytes;
}

QString sha256(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

class DeferredContext final : public QObject, public AssetTransferContext {
public:
    struct Task {
        std::function<void()> work;
        std::function<void()> complete;
        std::function<void(const QString&)> failed;
    };

    explicit DeferredContext(QString folder)
        : folder_(std::move(folder))
    {
    }

    QObject* dispatchContext() noexcept override { return this; }
    int sessionSampleRate() const noexcept override { return 48000; }
    QString assetPathForSend(const QString& hash) const override
    {
        return outgoingPaths.value(hash);
    }
    QString incomingAssetPath(const QString& hash) const override
    {
        return QDir(folder_).absoluteFilePath(hash + QStringLiteral(".wav"));
    }
    bool incomingAssetExpected(
        const QString& hash,
        const QString& sourcePeerToken) const override
    {
        return active && hash == expectedHash && sourcePeerToken == expectedSource;
    }
    void abandonIncomingAsset(const QString& hash) override
    {
        ++abandoned;
        abandonedHashes.append(hash);
        active = false;
    }
    void acceptIncomingAsset(
        const QString& hash,
        const QString& path,
        qint64 sourceFrames) override
    {
        ++accepted;
        acceptedHash = hash;
        acceptedPath = path;
        acceptedFrames = sourceFrames;
        active = false;
    }
    void noteAssetProgress(const QString&, const QString&, bool) override
    {
        ++progressEvents;
    }
    void appendAssetLog(const QString& message) override { logs.append(message); }
    bool startAssetFileTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed) override
    {
        tasks.push_back({std::move(work), std::move(complete), std::move(failed)});
        return true;
    }
    bool canQueueAssetControl(const QString&, qint64) const override { return true; }
    bool sendAssetControl(const QString&, const QJsonObject& message) override
    {
        controls.append(message);
        return true;
    }
    bool sendAssetBinary(const QString&, const QByteArray& payload) override
    {
        binaries.append(payload);
        return true;
    }

    bool runNext()
    {
        if (tasks.empty()) return false;
        Task task = std::move(tasks.front());
        tasks.pop_front();
        task.work();
        task.complete();
        return true;
    }

    Task takeNext()
    {
        expect(!tasks.empty(), "a deferred worker task is available");
        if (tasks.empty()) return {};
        Task task = std::move(tasks.front());
        tasks.pop_front();
        return task;
    }

    void runAll()
    {
        int guard = 0;
        while (runNext() && ++guard < 100) {}
        expect(tasks.empty(), "deferred worker queue drains within its bound");
    }

    void expectAsset(QString hash, QString source = QStringLiteral("peer-a"))
    {
        expectedHash = std::move(hash);
        expectedSource = std::move(source);
        active = true;
    }

    QString folder_;
    QMap<QString, QString> outgoingPaths;
    std::deque<Task> tasks;
    QString expectedHash;
    QString expectedSource = QStringLiteral("peer-a");
    bool active = false;
    int abandoned = 0;
    int accepted = 0;
    int progressEvents = 0;
    QStringList abandonedHashes;
    QString acceptedHash;
    QString acceptedPath;
    qint64 acceptedFrames = 0;
    QStringList logs;
    QList<QJsonObject> controls;
    QList<QByteArray> binaries;
};

QJsonObject startMessage(const QString& hash, qsizetype bytes)
{
    return {
        {QStringLiteral("sha256"), hash},
        {QStringLiteral("file_bytes"), static_cast<qint64>(bytes)},
        {QStringLiteral("chunk_size"),
            jam2::application::limits::kMaximumAssetChunkBytes},
    };
}

QJsonObject doneMessage(const QString& hash, int chunks = 1)
{
    return {
        {QStringLiteral("sha256"), hash},
        {QStringLiteral("chunks"), chunks},
    };
}

QByteArray chunk(const QString& hash, const QByteArray& wav)
{
    return jam2::application::asset_chunk::encode({hash, 0, 0, wav});
}

QList<QByteArray> chunks(const QString& hash, const QByteArray& bytes)
{
    QList<QByteArray> result;
    const qsizetype chunkBytes = jam2::application::limits::kMaximumAssetChunkBytes;
    for (qsizetype offset = 0, index = 0; offset < bytes.size();
         offset += chunkBytes, ++index) {
        result.append(jam2::application::asset_chunk::encode({
            hash,
            static_cast<quint32>(index),
            static_cast<quint64>(offset),
            bytes.mid(offset, chunkBytes),
        }));
    }
    return result;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QStringList partialFiles(const DeferredContext& context, const QString& hash)
{
    return QDir(context.folder_).entryList(
        {hash + QStringLiteral(".wav.partial.*")}, QDir::Files);
}

void receiveComplete(
    AssetTransferService& transfer,
    DeferredContext& context,
    const QString& hash,
    const QByteArray& bytes,
    const QString& source = QStringLiteral("peer-a"))
{
    const QList<QByteArray> encoded = chunks(hash, bytes);
    transfer.receiveStart(startMessage(hash, bytes.size()), source);
    for (const QByteArray& payload : encoded) {
        transfer.receiveChunk(payload, source);
    }
    transfer.receiveDone(doneMessage(hash, encoded.size()), source);
    context.runAll();
}

void test_chunk_sequence_rejects_duplicate_and_wrong_source()
{
    const QByteArray wav = pcm16Wav(8, 1000);
    const QString hash = sha256(wav);
    jam2::application::asset_chunk::ReceiveSequence sequence;
    QString error;
    expect(sequence.begin(hash, QStringLiteral("peer-a"), wav.size(),
        jam2::application::limits::kMaximumAssetChunkBytes, error),
        "asset sequence starts with current fixed chunk size");
    const jam2::application::asset_chunk::Chunk first{hash, 0, 0, wav};
    expect(!sequence.accept(first, QStringLiteral("peer-b"), error),
        "asset sequence rejects the wrong source");
    error.clear();
    expect(sequence.accept(first, QStringLiteral("peer-a"), error),
        "asset sequence accepts its exact first chunk");
    error.clear();
    expect(!sequence.accept(first, QStringLiteral("peer-a"), error),
        "asset sequence rejects a duplicate chunk");
    error.clear();
    expect(sequence.finish(hash, QStringLiteral("peer-a"), 1, error),
        "asset sequence completion preserves exact identity and count");
}

void test_arrangement_validation_reuses_received_assets(const QString& folder)
{
    const QString hash(64, QLatin1Char('a'));
    const QString assetFolder = QDir(folder).absoluteFilePath(QStringLiteral("wavs"));
    const QString localPath = QDir(folder).absoluteFilePath(QStringLiteral("local.wav"));
    const QString received = jam2::gui::looper_asset_files::receivedPath(folder, hash);
    const QStringList candidates =
        jam2::gui::looper_asset_files::validationCandidates(
            folder, assetFolder, hash, localPath);
    expect(candidates == QStringList{
            QDir(assetFolder).absoluteFilePath(hash + QStringLiteral(".wav")),
            received,
            localPath},
        "arrangement validation includes the hash-addressed receive cache");
    expect(jam2::gui::looper_asset_files::validationCandidates(
               folder, assetFolder, hash, received).count(received) == 1,
        "arrangement validation de-duplicates a received local candidate");
}

void test_chunk_without_active_transfer_has_a_reason(const QString& folder)
{
    const QByteArray wav = pcm16Wav(8, 1000);
    DeferredContext context(folder);
    AssetTransferService transfer(context);
    transfer.receiveChunk(chunk(sha256(wav), wav), QStringLiteral("peer-a"));
    expect(context.logs.size() == 1 &&
            context.logs.constFirst().contains(
                QStringLiteral("no incoming transfer is active")) &&
            !context.logs.constFirst().endsWith(QStringLiteral(": ")) &&
            context.abandoned == 0,
        "unsolicited asset chunks are rejected with an actionable reason");
}

void test_cancelled_incoming_completion_stays_stale(const QString& folder)
{
    const QByteArray wav = pcm16Wav(16, 1200);
    const QString hash = sha256(wav);
    DeferredContext context(folder);
    context.expectAsset(hash);
    AssetTransferService transfer(context);
    transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(hash, wav), context.expectedSource);
    expect(context.tasks.size() == 1, "incoming chunk queues one bounded file task");
    transfer.cancel();
    context.runAll();
    expect(context.abandoned == 1 && context.accepted == 0,
        "cancelled incoming completion cannot accept an asset");
    expect(!QFileInfo::exists(context.incomingAssetPath(hash)),
        "cancelled incoming completion leaves no committed asset");
}

void test_superseded_incoming_completion_cannot_replace_current(const QString& folder)
{
    const QByteArray oldWav = pcm16Wav(24, 900);
    const QByteArray currentWav = pcm16Wav(24, 1600);
    const QString oldHash = sha256(oldWav);
    const QString currentHash = sha256(currentWav);
    DeferredContext context(folder);
    AssetTransferService transfer(context);

    context.expectAsset(oldHash);
    transfer.receiveStart(startMessage(oldHash, oldWav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(oldHash, oldWav), context.expectedSource);
    transfer.cancel();

    context.expectAsset(currentHash);
    transfer.receiveStart(startMessage(currentHash, currentWav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(currentHash, currentWav), context.expectedSource);
    transfer.receiveDone(doneMessage(currentHash), context.expectedSource);
    context.runAll();

    QFile accepted(context.acceptedPath);
    const bool acceptedBytes = accepted.open(QIODevice::ReadOnly) &&
        sha256(accepted.readAll()) == currentHash;
    expect(context.abandoned == 1 && context.accepted == 1,
        "superseding transfer abandons only the stale generation");
    expect(context.acceptedHash == currentHash && acceptedBytes && context.acceptedFrames == 24,
        "current generation alone commits its validated WAV");
    expect(!QFileInfo::exists(context.incomingAssetPath(oldHash)),
        "superseded generation leaves no old output file");
}

void test_cancelled_outgoing_validation_cannot_send(const QString& folder)
{
    const QByteArray wav = pcm16Wav(32, 1300);
    const QString hash = sha256(wav);
    const QString path = QDir(folder).absoluteFilePath(QStringLiteral("send.wav"));
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(wav) == wav.size(), "outgoing fixture is written");
    file.close();

    DeferredContext context(folder);
    context.outgoingPaths.insert(hash, path);
    AssetTransferService transfer(context);
    transfer.queueSend(hash, QStringLiteral("peer-b"));
    expect(context.tasks.size() == 1, "outgoing validation queues one bounded file task");
    transfer.cancel();
    context.runAll();
    expect(context.controls.isEmpty() && context.binaries.isEmpty(),
        "cancelled outgoing validation cannot emit stale network data");
}

void test_hash_scoped_outgoing_discard_preserves_other_work(const QString& folder)
{
    const QByteArray removedWav = pcm16Wav(34, 1350);
    const QByteArray retainedWav = pcm16Wav(36, 1450);
    const QString removedHash = sha256(removedWav);
    const QString retainedHash = sha256(retainedWav);
    const QString removedPath = QDir(folder).absoluteFilePath(
        QStringLiteral("discard-send-removed.wav"));
    const QString retainedPath = QDir(folder).absoluteFilePath(
        QStringLiteral("discard-send-retained.wav"));
    QFile removedFile(removedPath);
    QFile retainedFile(retainedPath);
    expect(removedFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            removedFile.write(removedWav) == removedWav.size() &&
            retainedFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            retainedFile.write(retainedWav) == retainedWav.size(),
        "hash-scoped outgoing discard fixtures are written");
    removedFile.close();
    retainedFile.close();

    DeferredContext context(folder);
    context.outgoingPaths.insert(removedHash, removedPath);
    context.outgoingPaths.insert(retainedHash, retainedPath);
    AssetTransferService transfer(context);
    transfer.queueSend(removedHash, QStringLiteral("peer-b"));
    transfer.queueSend(removedHash, QStringLiteral("peer-c"));
    transfer.queueSend(retainedHash, QStringLiteral("peer-d"));
    expect(context.tasks.size() == 1 &&
            transfer.automationSnapshot().value(
                QStringLiteral("outgoing_queue_count")).toInt() == 2,
        "outgoing discard fixture has one validation and two queued requests");

    transfer.discardOutgoingHash(removedHash);
    context.runAll();
    expect(context.controls.size() == 1 &&
            context.controls.constFirst().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.start") &&
            context.controls.constFirst().value(QStringLiteral("sha256")).toString() ==
                retainedHash,
        "hash-scoped discard suppresses stale sends and advances unrelated work");
    const QJsonObject state = transfer.automationSnapshot();
    expect(!state.value(QStringLiteral("outgoing_validation_pending")).toBool() &&
            state.value(QStringLiteral("outgoing_hash")).toString() == retainedHash &&
            state.value(QStringLiteral("outgoing_queue_count")).toInt() == 0,
        "hash-scoped discard leaves only the retained transfer active");
    transfer.cancel();
}

void test_silent_incoming_discard_invalidates_workers_without_retry(const QString& folder)
{
    const QByteArray wav = pcm16Wav(38, 1550);
    const QString hash = sha256(wav);
    DeferredContext context(folder);
    context.expectAsset(hash);
    AssetTransferService transfer(context);
    transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(hash, wav), context.expectedSource);
    expect(context.tasks.size() == 1,
        "silent incoming discard fixture queues one bounded write");

    transfer.discardIncoming();
    context.runAll();
    expect(context.abandoned == 0 && context.accepted == 0 && context.active,
        "silent incoming discard leaves retry ownership with its caller");
    expect(context.controls.isEmpty() &&
            !QFileInfo::exists(context.incomingAssetPath(hash)) &&
            partialFiles(context, hash).isEmpty(),
        "silent incoming discard emits no acknowledgement and removes staged bytes");
}

void test_hash_mismatch_is_rejected(const QString& folder)
{
    const QByteArray wav = pcm16Wav(12, 700);
    const QByteArray different = pcm16Wav(12, 1700);
    const QString declaredHash = sha256(different);
    DeferredContext context(folder);
    context.expectAsset(declaredHash);
    AssetTransferService transfer(context);
    transfer.receiveStart(startMessage(declaredHash, wav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(declaredHash, wav), context.expectedSource);
    transfer.receiveDone(doneMessage(declaredHash), context.expectedSource);
    context.runAll();
    expect(context.accepted == 0 && context.abandoned == 1,
        "hash-mismatched incoming WAV is abandoned");
    expect(!QFileInfo::exists(context.incomingAssetPath(declaredHash)),
        "hash-mismatched incoming WAV is never committed");
    expect(partialFiles(context, declaredHash).isEmpty(),
        "hash-mismatched incoming WAV leaves no partial artifact");
}

void test_unrelated_and_stale_frames_do_not_cancel_current_transfer(const QString& folder)
{
    const QByteArray currentWav = pcm16Wav(48, 2100);
    const QByteArray staleWav = pcm16Wav(48, 800);
    const QString currentHash = sha256(currentWav);
    const QString staleHash = sha256(staleWav);
    DeferredContext context(folder);
    context.expectAsset(currentHash);
    AssetTransferService transfer(context);

    transfer.receiveStart(startMessage(currentHash, currentWav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(currentHash, currentWav), QStringLiteral("peer-b"));
    transfer.receiveChunk(QByteArrayLiteral("not an asset frame"), QStringLiteral("peer-b"));
    transfer.receiveDone(doneMessage(currentHash), QStringLiteral("peer-b"));
    transfer.receiveChunk(chunk(staleHash, staleWav), context.expectedSource);
    transfer.receiveDone(doneMessage(staleHash), context.expectedSource);

    expect(context.abandoned == 0 && context.active,
        "wrong-source and stale-hash frames preserve the current expected transfer");
    transfer.receiveChunk(chunk(currentHash, currentWav), context.expectedSource);
    transfer.receiveDone(doneMessage(currentHash), context.expectedSource);
    context.runAll();
    expect(context.abandoned == 0 && context.accepted == 1,
        "current transfer completes after unrelated and stale frames");
    expect(readFile(context.incomingAssetPath(currentHash)) == currentWav,
        "preserved current transfer commits exact WAV bytes");
    expect(partialFiles(context, currentHash).isEmpty(),
        "preserved current transfer leaves no partial artifact");
}

void test_current_source_malformed_duplicate_and_reordered_frames_fail_closed(
    const QString& folder)
{
    {
        const QByteArray wav = pcm16Wav(52, 2200);
        const QString hash = sha256(wav);
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
        transfer.receiveChunk(QByteArrayLiteral("malformed"), context.expectedSource);
        context.runAll();
        expect(context.abandoned == 1 && context.accepted == 0,
            "malformed frame from the current source abandons its transfer");
        expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "malformed current-source frame exposes no WAV or partial file");
    }
    {
        const QByteArray wav = pcm16Wav(60, 2300);
        const QString hash = sha256(wav);
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
        const QByteArray first = chunk(hash, wav);
        transfer.receiveChunk(first, context.expectedSource);
        transfer.receiveChunk(first, context.expectedSource);
        context.runAll();
        expect(context.abandoned == 1 && context.accepted == 0,
            "duplicate current-source chunk fails its exact sequence");
        expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "duplicate chunk cleanup removes all staged bytes");
    }
    {
        const QByteArray wav = pcm16Wav(20000, 2400);
        const QString hash = sha256(wav);
        const QList<QByteArray> encoded = chunks(hash, wav);
        expect(encoded.size() == 2, "reordering fixture spans exactly two chunks");
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
        transfer.receiveChunk(encoded.at(1), context.expectedSource);
        context.runAll();
        expect(context.abandoned == 1 && context.accepted == 0,
            "reordered current-source chunk fails its exact sequence");
        expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "reordered chunk exposes no WAV or partial file");
    }
}

void test_truncated_and_invalid_wav_never_commit(const QString& folder)
{
    {
        const QByteArray wav = pcm16Wav(40000, 2500);
        const QString hash = sha256(wav);
        const QList<QByteArray> encoded = chunks(hash, wav);
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
        transfer.receiveChunk(encoded.constFirst(), context.expectedSource);
        transfer.receiveDone(doneMessage(hash, 1), context.expectedSource);
        context.runAll();
        expect(context.abandoned == 1 && context.accepted == 0,
            "truncated completion is abandoned before final validation");
        expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "truncated transfer leaves no committed or partial WAV");
    }
    {
        const QByteArray invalidWav(128, 'x');
        const QString hash = sha256(invalidWav);
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        receiveComplete(transfer, context, hash, invalidWav);
        expect(context.abandoned == 1 && context.accepted == 0,
            "hash-correct structurally invalid WAV is abandoned");
        expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "invalid WAV never becomes visible and its staging file is removed");
    }
}

void test_cancelled_failed_worker_removes_partial_file(const QString& folder)
{
    const QByteArray wav = pcm16Wav(72, 2600);
    const QString hash = sha256(wav);
    DeferredContext context(folder);
    context.expectAsset(hash);
    AssetTransferService transfer(context);
    transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(hash, wav), context.expectedSource);

    DeferredContext::Task task = context.takeNext();
    task.work();
    expect(!partialFiles(context, hash).isEmpty() &&
            !QFileInfo::exists(context.incomingAssetPath(hash)),
        "worker staging bytes remain private before final commit");
    transfer.cancel();
    task.failed(QStringLiteral("injected worker completion failure"));
    context.runAll();
    expect(context.abandoned == 1 && context.accepted == 0,
        "cancelled failed worker cannot accept an asset");
    expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
            partialFiles(context, hash).isEmpty(),
        "cancelled failed worker removes its stale partial file");
}

void test_existing_valid_destination_is_idempotent(const QString& folder)
{
    const QByteArray wav = pcm16Wav(80, 2700);
    const QString hash = sha256(wav);
    DeferredContext context(folder);
    const QString output = context.incomingAssetPath(hash);
    QFile existing(output);
    expect(existing.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            existing.write(wav) == wav.size(),
        "idempotent destination fixture is written");
    existing.close();
    context.expectAsset(hash);
    AssetTransferService transfer(context);
    receiveComplete(transfer, context, hash, wav);
    expect(context.accepted == 1 && context.abandoned == 0,
        "existing exact destination is accepted once");
    expect(readFile(output) == wav && context.acceptedFrames == 80,
        "idempotent receive preserves exact destination bytes and frame metadata");
    expect(partialFiles(context, hash).isEmpty(),
        "idempotent receive removes its redundant staging file");
}

void test_multichunk_receive_and_existing_corrupt_destination(const QString& folder)
{
    const QByteArray wav = pcm16Wav(20000, 2750);
    const QString hash = sha256(wav);
    const QList<QByteArray> encoded = chunks(hash, wav);
    expect(encoded.size() == 2, "valid receive fixture spans exactly two chunks");
    DeferredContext context(folder);
    const QString output = context.incomingAssetPath(hash);
    QFile corrupt(output);
    expect(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            corrupt.write("old corrupt bytes", 17) == 17,
        "corrupt destination fixture is written");
    corrupt.close();
    context.expectAsset(hash);
    AssetTransferService transfer(context);
    receiveComplete(transfer, context, hash, wav);
    expect(context.accepted == 1 && context.abandoned == 0,
        "valid multi-chunk WAV is accepted atomically");
    expect(context.controls.size() == 2 &&
            context.controls.at(0).value(QStringLiteral("chunks")).toInt() == 1 &&
            context.controls.at(1).value(QStringLiteral("chunks")).toInt() == 2,
        "valid multi-chunk receive acknowledges each durable chunk in order");
    expect(readFile(output) == wav && sha256(readFile(output)) == hash,
        "validated multi-chunk WAV atomically replaces a corrupt destination");
    expect(context.acceptedFrames == 20000 && partialFiles(context, hash).isEmpty(),
        "multi-chunk commit preserves frame metadata and removes staging bytes");
}

void test_sample_rate_mismatch_is_rejected_in_both_directions(const QString& folder)
{
    const QByteArray wav = pcm16Wav(96, 2850, 44100);
    const QString hash = sha256(wav);
    {
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        receiveComplete(transfer, context, hash, wav);
        expect(context.accepted == 0 && context.abandoned == 1,
            "incoming WAV outside the session sample-rate contract is abandoned");
        expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "incoming sample-rate mismatch exposes no destination or partial file");
    }
    {
        const QString path = QDir(folder).absoluteFilePath(QStringLiteral("send-rate-mismatch.wav"));
        QFile file(path);
        expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(wav) == wav.size(),
            "outgoing sample-rate mismatch fixture is written");
        file.close();
        DeferredContext context(folder);
        context.outgoingPaths.insert(hash, path);
        AssetTransferService transfer(context);
        transfer.queueSend(hash, QStringLiteral("peer-b"));
        context.runAll();
        expect(context.controls.isEmpty() && context.binaries.isEmpty(),
            "outgoing WAV outside the session sample-rate contract emits no transfer data");
    }
}

void test_disconnect_cancels_only_the_owning_peer(const QString& folder)
{
    const QByteArray wav = pcm16Wav(104, 2900);
    const QString hash = sha256(wav);
    DeferredContext context(folder);
    context.expectAsset(hash);
    AssetTransferService transfer(context);
    transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
    transfer.receiveChunk(chunk(hash, wav), context.expectedSource);
    transfer.peerDisconnected(QStringLiteral("peer-b"));
    expect(context.abandoned == 0 && context.active,
        "unrelated peer disconnect preserves the active incoming transfer");
    transfer.peerDisconnected(context.expectedSource);
    context.runAll();
    expect(context.abandoned == 1 && context.accepted == 0,
        "owning peer disconnect abandons its active incoming transfer");
    expect(!QFileInfo::exists(context.incomingAssetPath(hash)) &&
            partialFiles(context, hash).isEmpty(),
        "disconnect cancellation settles its worker and removes staged bytes");
}

void test_outgoing_ack_ownership_and_request_deduplication(const QString& folder)
{
    const QByteArray wav = pcm16Wav(88, 2800);
    const QString hash = sha256(wav);
    const QString path = QDir(folder).absoluteFilePath(QStringLiteral("send-ack.wav"));
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(wav) == wav.size(),
        "outgoing acknowledgement fixture is written");
    file.close();

    DeferredContext context(folder);
    context.outgoingPaths.insert(hash, path);
    AssetTransferService transfer(context);
    transfer.queueSend(hash, QStringLiteral("peer-b"));
    transfer.queueSend(hash, QStringLiteral("peer-b"));
    expect(context.tasks.size() == 1,
        "duplicate outgoing request shares one validation task");
    context.runNext();
    expect(context.controls.size() == 1 &&
            context.controls.constFirst().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.start"),
        "validated outgoing request emits one transfer start");
    transfer.continueSend();
    context.runNext();
    expect(context.binaries.size() == 1,
        "outgoing transfer emits one exact binary chunk");
    jam2::application::asset_chunk::Chunk decoded;
    QString decodeError;
    expect(jam2::application::asset_chunk::decode(
               context.binaries.constFirst(), decoded, decodeError) &&
            decoded.sha256 == hash && decoded.index == 0 && decoded.offset == 0 &&
            decoded.data == wav,
        "outgoing binary chunk preserves identity, sequence, offset, and bytes");

    transfer.receiveAck({{QStringLiteral("sha256"), hash},
                            {QStringLiteral("chunks"), 1}},
        QStringLiteral("peer-c"));
    transfer.receiveAck({{QStringLiteral("sha256"), QString(64, QLatin1Char('0'))},
                            {QStringLiteral("chunks"), 1}},
        QStringLiteral("peer-b"));
    transfer.receiveAck({{QStringLiteral("sha256"), hash},
                            {QStringLiteral("chunks"), 2}},
        QStringLiteral("peer-b"));
    expect(context.controls.size() == 1,
        "wrong-source, wrong-hash, and future acknowledgements cannot complete a send");
    transfer.receiveAck({{QStringLiteral("sha256"), hash},
                            {QStringLiteral("chunks"), 1}},
        QStringLiteral("peer-b"));
    transfer.continueSend();
    expect(context.controls.size() == 2 &&
            context.controls.constLast().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.done"),
        "only the exact acknowledgement completes the outgoing transfer once");
    expect(context.binaries.size() == 1 && context.tasks.empty(),
        "deduplicated outgoing request emits no second transfer");
}

void test_active_outgoing_rerequest_restarts_from_zero(const QString& folder)
{
    const QByteArray wav = pcm16Wav(89, 2825);
    const QString hash = sha256(wav);
    const QString path = QDir(folder).absoluteFilePath(
        QStringLiteral("send-rerequest.wav"));
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(wav) == wav.size(),
        "outgoing re-request fixture is written");
    file.close();

    DeferredContext context(folder);
    context.outgoingPaths.insert(hash, path);
    AssetTransferService transfer(context);
    const QString peer = QStringLiteral("peer-b");
    transfer.queueSend(hash, peer);
    expect(context.runNext() && context.controls.size() == 1 &&
            context.controls.constFirst().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.start"),
        "initial outgoing request starts its stream");
    transfer.continueSend();
    expect(context.runNext() && context.binaries.size() == 1,
        "initial outgoing stream emits bytes before re-request");

    transfer.queueSend(hash, peer);
    expect(context.tasks.size() == 1 &&
            transfer.automationSnapshot().value(
                QStringLiteral("outgoing_validation_pending")).toBool(),
        "active same-peer re-request restarts validation from byte zero");
    expect(context.runNext() && context.controls.size() == 2 &&
            context.controls.constLast().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.start"),
        "active same-peer re-request emits a fresh transfer start");
    transfer.continueSend();
    expect(context.runNext() && context.binaries.size() == 2,
        "restarted outgoing stream emits its bytes from the beginning");
    jam2::application::asset_chunk::Chunk restarted;
    QString error;
    expect(jam2::application::asset_chunk::decode(
               context.binaries.constLast(), restarted, error) &&
            restarted.sha256 == hash && restarted.index == 0 &&
            restarted.offset == 0 && restarted.data == wav,
        "re-requested outgoing stream restarts with exact chunk zero");
    transfer.receiveAck({{QStringLiteral("sha256"), hash},
                            {QStringLiteral("chunks"), 1}},
        peer);
    transfer.continueSend();
    expect(context.controls.size() == 3 &&
            context.controls.constLast().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.done") &&
            transfer.automationSnapshot().value(
                QStringLiteral("outgoing_hash")).toString().isEmpty(),
        "re-requested outgoing stream completes normally");
}

void test_private_start_drop_is_one_shot_and_recoverable(const QString& folder)
{
    const QByteArray wav = pcm16Wav(92, 2875);
    const QString hash = sha256(wav);
    const QString path = QDir(folder).absoluteFilePath(QStringLiteral("drop-start.wav"));
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(wav) == wav.size(),
        "automation start-drop fixture is written");
    file.close();

    DeferredContext context(folder);
    context.outgoingPaths.insert(hash, path);
    AssetTransferService transfer(context);
    QString error;
    expect(transfer.armAutomationDropOutgoingStarts(1, error) && error.isEmpty(),
        "one private outgoing start drop is armed");
    expect(!transfer.armAutomationPause(
               QStringLiteral("outgoing-validation"), error),
        "an armed start drop excludes a simultaneous transfer pause");
    transfer.queueSend(hash, QStringLiteral("peer-b"));
    expect(context.runNext(), "start-drop fixture completes outgoing validation");
    QJsonObject snapshot = transfer.automationSnapshot();
    expect(context.controls.isEmpty() && context.binaries.isEmpty() &&
            !snapshot.value(QStringLiteral("drop_outgoing_start_armed")).toBool() &&
            snapshot.value(QStringLiteral("dropped_outgoing_starts")).toInteger() == 1 &&
            snapshot.value(QStringLiteral("last_dropped_outgoing_target")).toString() ==
                QStringLiteral("peer-b") &&
            snapshot.value(QStringLiteral("outgoing_hash")).toString().isEmpty(),
        "start drop suppresses exactly one frame and resets the outgoing lifecycle");

    error.clear();
    transfer.queueSend(hash, QStringLiteral("peer-b"));
    expect(context.runNext(), "post-drop retry completes outgoing validation");
    snapshot = transfer.automationSnapshot();
    expect(context.controls.size() == 1 &&
            context.controls.constFirst().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.start") &&
            snapshot.value(QStringLiteral("dropped_outgoing_starts")).toInteger() == 1,
        "the next request sends normally without rearming the one-shot fault");
    transfer.cancel();

    DeferredContext countedContext(folder);
    countedContext.outgoingPaths.insert(hash, path);
    AssetTransferService countedTransfer(countedContext);
    error.clear();
    expect(!countedTransfer.armAutomationDropOutgoingStarts(0, error) &&
            !error.isEmpty(),
        "a zero-count start drop is rejected");
    error.clear();
    expect(!countedTransfer.armAutomationDropOutgoingStarts(5, error) &&
            !error.isEmpty(),
        "an excessive start-drop count is rejected");
    error.clear();
    expect(countedTransfer.armAutomationDropOutgoingStarts(3, error) && error.isEmpty(),
        "three consecutive private outgoing start drops are armed");
    for (int drop = 1; drop <= 3; ++drop) {
        countedTransfer.queueSend(hash, QStringLiteral("peer-c"));
        expect(countedContext.runNext(),
            "counted start-drop fixture completes outgoing validation");
        snapshot = countedTransfer.automationSnapshot();
        expect(countedContext.controls.isEmpty() &&
                snapshot.value(QStringLiteral("dropped_outgoing_starts")).toInteger() == drop &&
                snapshot.value(QStringLiteral("last_dropped_outgoing_target")).toString() ==
                    QStringLiteral("peer-c") &&
                snapshot.value(QStringLiteral("drop_outgoing_starts_remaining")).toInt() ==
                    3 - drop,
            "counted start drop consumes exactly one fault per outgoing request");
    }
    countedTransfer.queueSend(hash, QStringLiteral("peer-c"));
    expect(countedContext.runNext(),
        "post-counted-drop request completes outgoing validation");
    expect(countedContext.controls.size() == 1 &&
            countedContext.controls.constFirst().value(QStringLiteral("type")).toString() ==
                QStringLiteral("looper.asset.start"),
        "the request after the bounded drop count sends normally");
    countedTransfer.cancel();
}

void test_track_batch_expiry_preserves_same_hash_ownership()
{
    using jam2::gui::track_asset_ownership::Claim;
    using jam2::gui::track_asset_ownership::planBatchExpiry;
    const QString hash(64, QLatin1Char('a'));
    const QString otherHash(64, QLatin1Char('b'));
    const QList<Claim> twoSources{
        {QStringLiteral("a-1"), QStringLiteral("peer-a"), QStringLiteral("batch-a"), hash},
        {QStringLiteral("b-1"), QStringLiteral("peer-b"), QStringLiteral("batch-b"), hash},
        {QStringLiteral("c-1"), QStringLiteral("peer-c"), QStringLiteral("batch-c"), otherHash},
    };
    const auto expireA = planBatchExpiry(
        twoSources,
        QStringLiteral("peer-a"),
        QStringLiteral("batch-a"),
        true,
        hash,
        QStringLiteral("peer-a"),
        {});
    expect(expireA.removedKeys == QStringList{QStringLiteral("a-1")} &&
            expireA.removedHashes == QSet<QString>{hash} &&
            expireA.remainingTrackSources.value(hash) == QStringLiteral("peer-b") &&
            expireA.stillExpectedHashes.contains(hash) &&
            expireA.activeSourceDetached,
        "expiring source A transfers same-hash ownership and detaches its active receive");

    const auto expireB = planBatchExpiry(
        twoSources,
        QStringLiteral("peer-b"),
        QStringLiteral("batch-b"),
        true,
        hash,
        QStringLiteral("peer-b"),
        {});
    expect(expireB.remainingTrackSources.value(hash) == QStringLiteral("peer-a") &&
            expireB.stillExpectedHashes.contains(hash) &&
            expireB.activeSourceDetached,
        "expiring source B preserves source A in the reverse order");

    QList<Claim> sameSource = twoSources;
    sameSource.prepend(Claim{QStringLiteral("a-2"), QStringLiteral("peer-a"),
        QStringLiteral("batch-a-2"), hash});
    const auto keepActiveA = planBatchExpiry(
        sameSource,
        QStringLiteral("peer-a"),
        QStringLiteral("batch-a"),
        true,
        hash,
        QStringLiteral("peer-a"),
        {});
    expect(!keepActiveA.activeSourceDetached &&
            keepActiveA.remainingTrackSources.value(hash) == QStringLiteral("peer-a"),
        "another same-source batch keeps the active same-hash receive intact");

    const auto arrangementOwnsHash = planBatchExpiry(
        QList<Claim>{twoSources.constFirst()},
        QStringLiteral("peer-a"),
        QStringLiteral("batch-a"),
        true,
        hash,
        QStringLiteral("peer-a"),
        QSet<QString>{hash});
    expect(arrangementOwnsHash.activeSourceDetached &&
            arrangementOwnsHash.remainingTrackSources.isEmpty() &&
            arrangementOwnsHash.stillExpectedHashes.contains(hash),
        "pending arrangement ownership preserves retry eligibility without a track source");

    const auto unrelated = planBatchExpiry(
        twoSources,
        QStringLiteral("peer-z"),
        QStringLiteral("batch-z"),
        true,
        hash,
        QStringLiteral("peer-a"),
        {});
    expect(!unrelated.found() && unrelated.removedHashes.isEmpty() &&
            unrelated.remainingTrackSources.isEmpty(),
        "unrelated batch expiry is a no-op");
}

void test_arrangement_supersession_preserves_the_active_wav()
{
    using jam2::gui::track_asset_ownership::Claim;
    using jam2::gui::track_asset_ownership::planPeerSupersession;
    const QString hash(64, QLatin1Char('b'));
    const QList<Claim> claims{
        {QStringLiteral("peer-a:batch-a:one"), QStringLiteral("peer-a"),
            QStringLiteral("batch-a"), hash, 2},
        {QStringLiteral("peer-a:batch-a:two"), QStringLiteral("peer-a"),
            QStringLiteral("batch-a"), QString(64, QLatin1Char('c')), 2},
        {QStringLiteral("peer-b:batch-b:one"), QStringLiteral("peer-b"),
            QStringLiteral("batch-b"), hash, 1},
    };
    const auto plan = planPeerSupersession(
        claims, QStringLiteral("peer-a"), true, hash, QStringLiteral("peer-a"),
        QSet<QString>{hash});
    expect(plan.removedKeys.size() == 2 && plan.removedHashes.size() == 2 &&
            plan.batchSizes.value(QStringLiteral("batch-a")) == 2 &&
            plan.preserveActiveTransfer,
        "newer arrangement supersedes batch metadata but preserves its active WAV receive");
    const auto unrelated = planPeerSupersession(
        claims, QStringLiteral("peer-b"), true, hash, QStringLiteral("peer-a"),
        QSet<QString>{hash});
    expect(unrelated.removedKeys.size() == 1 && !unrelated.preserveActiveTransfer,
        "supersession never preserves a transfer owned by another source");
    const auto removed = planPeerSupersession(
        claims, QStringLiteral("peer-a"), true, hash, QStringLiteral("peer-a"), {});
    expect(removed.found() && !removed.preserveActiveTransfer,
        "supersession cancels an active WAV omitted by the replacement arrangement");
}

void test_private_automation_gates_are_explicit_and_cancel_safe(const QString& folder)
{
    const QByteArray wav = pcm16Wav(112, 2950);
    const QString hash = sha256(wav);
    const QString path = QDir(folder).absoluteFilePath(QStringLiteral("pause-send.wav"));
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(wav) == wav.size(),
        "automation pause outgoing fixture is written");
    file.close();

    {
        DeferredContext context(folder);
        context.outgoingPaths.insert(hash, path);
        AssetTransferService transfer(context);
        QString error;
        expect(!transfer.armAutomationPause(QStringLiteral("invalid"), error) &&
                !error.isEmpty(),
            "unknown private pause point is rejected with an explanation");
        error.clear();
        expect(transfer.armAutomationPause(
                   QStringLiteral("outgoing-validation"), error),
            "outgoing-validation gate is armed");
        expect(!transfer.armAutomationPause(
                    QStringLiteral("incoming-chunk"), error),
            "a second private gate cannot overlap the armed gate");
        transfer.queueSend(hash, QStringLiteral("peer-b"));
        const QJsonObject paused = transfer.automationSnapshot();
        expect(paused.value(QStringLiteral("pause_active")).toString() ==
                    QStringLiteral("outgoing-validation") &&
                paused.value(QStringLiteral("outgoing_validation_pending")).toBool() &&
                context.tasks.empty() && context.controls.isEmpty(),
            "outgoing validation gate is observable before worker or wire activity");
        error.clear();
        expect(transfer.releaseAutomationPause(error) && context.tasks.size() == 1,
            "explicit release, not elapsed wall time, starts outgoing validation");
        transfer.cancel();
        context.runAll();
        const QJsonObject cancelled = transfer.automationSnapshot();
        expect(cancelled.value(QStringLiteral("pause_active")).toString().isEmpty() &&
                cancelled.value(QStringLiteral("pause_armed")).toString().isEmpty() &&
                !cancelled.value(QStringLiteral("outgoing_validation_pending")).toBool(),
            "cancellation clears an active outgoing-validation pause");
    }

    {
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        QString error;
        expect(transfer.armAutomationPause(
                   QStringLiteral("incoming-chunk"), error),
            "incoming-chunk gate is armed");
        transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
        transfer.receiveChunk(chunk(hash, wav), context.expectedSource);
        const QJsonObject paused = transfer.automationSnapshot();
        expect(paused.value(QStringLiteral("pause_active")).toString() ==
                    QStringLiteral("incoming-chunk") &&
                paused.value(QStringLiteral("incoming_queued_chunks")).toInt() == 1 &&
                context.tasks.empty(),
            "incoming chunk pause retains private bytes before the write worker");
        transfer.cancel();
        context.runAll();
        expect(context.abandoned == 1 && context.accepted == 0 &&
                !QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty(),
            "cancelling an incoming-chunk pause exposes no destination or partial file");
    }

    {
        DeferredContext context(folder);
        context.expectAsset(hash);
        AssetTransferService transfer(context);
        QString error;
        expect(transfer.armAutomationPause(
                   QStringLiteral("incoming-finalize"), error),
            "incoming-finalize gate is armed");
        transfer.receiveStart(startMessage(hash, wav.size()), context.expectedSource);
        transfer.receiveChunk(chunk(hash, wav), context.expectedSource);
        expect(context.runNext(), "incoming-finalize fixture writes its durable chunk");
        transfer.receiveDone(doneMessage(hash), context.expectedSource);
        const QJsonObject paused = transfer.automationSnapshot();
        expect(paused.value(QStringLiteral("pause_active")).toString() ==
                    QStringLiteral("incoming-finalize") &&
                paused.value(QStringLiteral("incoming_done_pending")).toBool() &&
                context.tasks.empty(),
            "incoming finalize pause is observable before atomic validation and commit");
        transfer.discardIncoming();
        context.runAll();
        const QJsonObject discarded = transfer.automationSnapshot();
        expect(context.abandoned == 0 && context.accepted == 0 &&
                !QFileInfo::exists(context.incomingAssetPath(hash)) &&
                partialFiles(context, hash).isEmpty() &&
                discarded.value(QStringLiteral("pause_active")).toString().isEmpty(),
            "discarding an incoming-finalize pause removes its staged WAV and gate");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir folder;
    expect(folder.isValid(), "asset test folder is available");
    if (folder.isValid()) {
        test_chunk_sequence_rejects_duplicate_and_wrong_source();
        test_arrangement_validation_reuses_received_assets(folder.path());
        test_chunk_without_active_transfer_has_a_reason(folder.path());
        test_cancelled_incoming_completion_stays_stale(folder.path());
        test_superseded_incoming_completion_cannot_replace_current(folder.path());
        test_cancelled_outgoing_validation_cannot_send(folder.path());
        test_hash_scoped_outgoing_discard_preserves_other_work(folder.path());
        test_silent_incoming_discard_invalidates_workers_without_retry(folder.path());
        test_hash_mismatch_is_rejected(folder.path());
        test_unrelated_and_stale_frames_do_not_cancel_current_transfer(folder.path());
        test_current_source_malformed_duplicate_and_reordered_frames_fail_closed(folder.path());
        test_truncated_and_invalid_wav_never_commit(folder.path());
        test_cancelled_failed_worker_removes_partial_file(folder.path());
        test_existing_valid_destination_is_idempotent(folder.path());
        test_multichunk_receive_and_existing_corrupt_destination(folder.path());
        test_sample_rate_mismatch_is_rejected_in_both_directions(folder.path());
        test_disconnect_cancels_only_the_owning_peer(folder.path());
        test_outgoing_ack_ownership_and_request_deduplication(folder.path());
        test_active_outgoing_rerequest_restarts_from_zero(folder.path());
        test_private_start_drop_is_one_shot_and_recoverable(folder.path());
        test_track_batch_expiry_preserves_same_hash_ownership();
        test_arrangement_supersession_preserves_the_active_wav();
        test_private_automation_gates_are_explicit_and_cancel_safe(folder.path());
    }
    if (failures == 0) std::cout << "Asset transfer checks passed\n";
    return failures == 0 ? 0 : 1;
}
