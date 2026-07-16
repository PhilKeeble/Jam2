#include "BoundaryValidation.hpp"

#include "AssetChunkProtocol.hpp"
#include "ControlMessageValidation.hpp"
#include "ControlProtocol.hpp"
#include "ContentLimits.hpp"
#include "RuntimeContracts.hpp"

#include "BeatGridModel.hpp"
#include "LooperProject.hpp"
#include "ProjectPersistenceCoordinator.hpp"
#include "SharedTrackController.hpp"
#include "TrackWidgets.hpp"
#include "TrackWorkspaceSupport.hpp"
#include "TrackRecordingWorkflow.hpp"
#include "PlaybackGrid.hpp"
#include "MetronomeTransportController.hpp"

#include "common.hpp"
#include "engine.hpp"
#include "metronome.hpp"
#include "pcm16_wav.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>

namespace {

std::filesystem::path nativeFilePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

struct LooperLaneLocation {
    int bank = -1;
    int lane = -1;
    bool valid() const noexcept { return bank >= 0 && lane >= 0; }
};

LooperLaneLocation findLooperLaneLocation(
    const LooperProject& project,
    const QString& bankId,
    const QString& laneId)
{
    const QVector<LooperBank>& banks = project.banks();
    for (int bankIndex = 0; bankIndex < banks.size(); ++bankIndex) {
        if (banks.at(bankIndex).id != bankId) continue;
        const QVector<LooperLane>& lanes = banks.at(bankIndex).lanes;
        for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
            if (lanes.at(laneIndex).id == laneId) return {bankIndex, laneIndex};
        }
        break;
    }
    return {};
}

std::uint64_t rawFrameFromMusicalFrame(
    std::uint64_t musicalFrame,
    std::int64_t renderOffsetFrames)
{
    if (renderOffsetFrames >= 0) {
        const std::uint64_t offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return musicalFrame > offset ? musicalFrame - offset : 0ULL;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(-(renderOffsetFrames + 1)) + 1ULL;
    return musicalFrame > (std::numeric_limits<std::uint64_t>::max)() - offset
        ? (std::numeric_limits<std::uint64_t>::max)()
        : musicalFrame + offset;
}

std::uint64_t musicalFrameFromRawFrame(
    std::uint64_t rawFrame,
    std::int64_t renderOffsetFrames)
{
    if (renderOffsetFrames >= 0) {
        const std::uint64_t offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return rawFrame > (std::numeric_limits<std::uint64_t>::max)() - offset
            ? (std::numeric_limits<std::uint64_t>::max)()
            : rawFrame + offset;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(-(renderOffsetFrames + 1)) + 1ULL;
    return rawFrame > offset ? rawFrame - offset : 0ULL;
}

int recordingCountInBeat(
    std::uint64_t currentFrame,
    std::uint64_t recordingStartFrame,
    std::uint64_t beatFrames)
{
    if (beatFrames == 0 || currentFrame >= recordingStartFrame) return 0;
    const std::uint64_t remaining = recordingStartFrame - currentFrame;
    const std::uint64_t beats = (remaining - 1ULL) / beatFrames + 1ULL;
    return static_cast<int>(std::min<std::uint64_t>(
        beats,
        static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
}

std::uint64_t nextGridBoundaryBeat(
    std::uint64_t absoluteBeat,
    int beatsPerBar,
    bool quantizeToBar)
{
    if (!quantizeToBar) {
        return absoluteBeat == (std::numeric_limits<std::uint64_t>::max)()
            ? absoluteBeat
            : absoluteBeat + 1ULL;
    }
    const std::uint64_t barBeats = static_cast<std::uint64_t>(qMax(1, beatsPerBar));
    const std::uint64_t bar = absoluteBeat / barBeats;
    if (bar >= (std::numeric_limits<std::uint64_t>::max)() / barBeats) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return (bar + 1ULL) * barBeats;
}

} // namespace

QJsonObject jam2RunBoundaryValidation(const QStringList& fixtureSpecs)
{
    QJsonArray cases;
    bool allOk = true;
    const auto record = [&](const QString& name, bool ok, const QString& detail = QString()) {
        QJsonObject result{
            {QStringLiteral("name"), name},
            {QStringLiteral("ok"), ok},
        };
        if (!detail.isEmpty()) {
            result.insert(QStringLiteral("detail"), detail);
        }
        cases.append(result);
        allOk = allOk && ok;
    };

    using namespace jam2::control_protocol;
    const QByteArray directionKey(32, 'k');
    const QJsonObject controlMessage{
        {QStringLiteral("type"), QStringLiteral("beat.set")},
        {QStringLiteral("lane"), QStringLiteral("chord")},
        {QStringLiteral("section"), 0},
        {QStringLiteral("beat"), 0},
        {QStringLiteral("text"), QStringLiteral("Cmaj7")},
    };
    QByteArray encoded = encodeAuthenticated(controlMessage, directionKey, 7);
    QByteArray body;
    QString error;
    const bool framed = takeFrame(encoded, body, error) == TakeFrameResult::Ready;
    AuthenticatedPayload decoded;
    record(QStringLiteral("control.valid-authenticated-frame"),
        framed && decodeAuthenticated(body, directionKey, 7, decoded, error) &&
            decoded.type == AuthenticatedPayloadType::Json && decoded.message == controlMessage,
        error);

    QByteArray tampered = body;
    if (tampered.size() > kAuthenticatedHeaderBytes) {
        tampered[kAuthenticatedHeaderBytes] = static_cast<char>(tampered[kAuthenticatedHeaderBytes] ^ 1);
    }
    error.clear();
    record(QStringLiteral("control.reject-tampered-tag"),
        !decodeAuthenticated(tampered, directionKey, 7, decoded, error), error);
    error.clear();
    record(QStringLiteral("control.reject-replayed-sequence"),
        !decodeAuthenticated(body, directionKey, 8, decoded, error), error);


    const quint32 excessiveLength = static_cast<quint32>(kMaxJsonBytes + kAuthenticatedHeaderBytes + 1);
    QByteArray excessiveFrame;

    excessiveFrame.append(static_cast<char>((excessiveLength >> 24) & 0xffU));
    excessiveFrame.append(static_cast<char>((excessiveLength >> 16) & 0xffU));
    excessiveFrame.append(static_cast<char>((excessiveLength >> 8) & 0xffU));
    excessiveFrame.append(static_cast<char>(excessiveLength & 0xffU));
    error.clear();
    record(QStringLiteral("control.reject-excessive-frame"),
        takeFrame(excessiveFrame, body, error) == TakeFrameResult::Invalid, error);
    record(QStringLiteral("control.reject-excessive-json"),
        encodeHandshake(QJsonObject{{QStringLiteral("value"), QString(70 * 1024, QLatin1Char('x'))}}).isEmpty());

    const jam2::application::asset_chunk::Chunk assetChunk{
        QString(64, QLatin1Char('a')), 3, 12, QByteArray("raw-audio", 9)};
    const QByteArray assetPayload = jam2::application::asset_chunk::encode(assetChunk);
    QByteArray binaryFrame = encodeAuthenticatedBinary(assetPayload, directionKey, 9);
    QByteArray binaryBody;
    AuthenticatedPayload decodedBinary;
    jam2::application::asset_chunk::Chunk decodedChunk;
    error.clear();
    const bool binaryFramed = takeFrame(binaryFrame, binaryBody, error) == TakeFrameResult::Ready;
    const bool binaryAuthenticated = binaryFramed &&
        decodeAuthenticated(binaryBody, directionKey, 9, decodedBinary, error);
    QString chunkError;
    const bool binaryDecoded = binaryAuthenticated &&
        decodedBinary.type == AuthenticatedPayloadType::AssetChunk &&
        jam2::application::asset_chunk::decode(decodedBinary.binary, decodedChunk, chunkError);
    record(QStringLiteral("control.valid-authenticated-binary-asset-chunk"),
        binaryDecoded && decodedChunk.sha256 == assetChunk.sha256 &&
            decodedChunk.index == assetChunk.index && decodedChunk.offset == assetChunk.offset &&
            decodedChunk.data == assetChunk.data,
        error.isEmpty() ? chunkError : error);

    const QJsonObject heartbeat{
        {QStringLiteral("type"), QStringLiteral("session.heartbeat")},
        {QStringLiteral("sample_rate"), 48000},
        {QStringLiteral("heartbeat_id"), 1},
    };
    const QJsonObject heartbeatAck{
        {QStringLiteral("type"), QStringLiteral("session.heartbeat.ack")},
        {QStringLiteral("sample_rate"), 48000},
        {QStringLiteral("heartbeat_id"), 1},
    };
    const QByteArray heartbeatFrame = encodeAuthenticated(heartbeat, directionKey, 10);
    const QByteArray interleavedBinaryFrame = encodeAuthenticatedBinary(assetPayload, directionKey, 11);
    const QByteArray heartbeatAckFrame = encodeAuthenticated(heartbeatAck, directionKey, 12);
    QByteArray fragmented;
    bool fragmentedNeedsMore = true;
    for (qsizetype index = 0; index + 1 < interleavedBinaryFrame.size(); ++index) {
        fragmented.append(interleavedBinaryFrame[index]);
        QByteArray prematureBody;
        QString prematureError;
        fragmentedNeedsMore = fragmentedNeedsMore &&
            takeFrame(fragmented, prematureBody, prematureError) == TakeFrameResult::NeedMore;
    }
    fragmented.append(interleavedBinaryFrame.back());
    QByteArray fragmentedBody;
    AuthenticatedPayload fragmentedPayload;
    QString interleavingError;
    const bool fragmentedReady =
        takeFrame(fragmented, fragmentedBody, interleavingError) == TakeFrameResult::Ready &&
        decodeAuthenticated(
            fragmentedBody, directionKey, 11, fragmentedPayload, interleavingError) &&
        fragmentedPayload.type == AuthenticatedPayloadType::AssetChunk && fragmented.isEmpty();

    QByteArray coalesced = heartbeatFrame + interleavedBinaryFrame + heartbeatAckFrame;
    bool interleaved = true;
    for (quint64 sequence = 10; sequence <= 12; ++sequence) {
        QByteArray nextBody;
        AuthenticatedPayload nextPayload;
        interleaved = interleaved &&
            takeFrame(coalesced, nextBody, interleavingError) == TakeFrameResult::Ready &&
            decodeAuthenticated(nextBody, directionKey, sequence, nextPayload, interleavingError);
        if (sequence == 11) {
            interleaved = interleaved &&
                nextPayload.type == AuthenticatedPayloadType::AssetChunk &&
                nextPayload.binary == assetPayload;
        } else {
            interleaved = interleaved && nextPayload.type == AuthenticatedPayloadType::Json;
        }
    }
    record(QStringLiteral("control.binary-fragmentation-coalescing-heartbeat-interleaving"),
        fragmentedNeedsMore && fragmentedReady && interleaved && coalesced.isEmpty(),
        interleavingError);
    record(QStringLiteral("control.reject-oversize-authenticated-binary"),
        encodeAuthenticatedBinary(
            QByteArray(kMaxBinaryBytes + 1, 'x'), directionKey, 13).isEmpty());

    QByteArray malformedAsset = assetPayload;
    malformedAsset[44] = 0x7f;
    jam2::application::asset_chunk::Chunk rejectedChunk;
    chunkError.clear();
    record(QStringLiteral("asset.reject-malformed-declared-length"),
        !jam2::application::asset_chunk::decode(malformedAsset, rejectedChunk, chunkError),
        chunkError);
    chunkError.clear();
    record(QStringLiteral("asset.reject-truncated-prefix"),
        !jam2::application::asset_chunk::decode(
            assetPayload.left(jam2::application::asset_chunk::kHeaderBytes - 1),
            rejectedChunk,
            chunkError),
        chunkError);

    jam2::application::asset_chunk::ReceiveSequence assetSequence;
    const QString assetHash(64, QLatin1Char('b'));
    const int sequenceChunkBytes = jam2::application::limits::kMaximumAssetChunkBytes;
    const qint64 sequenceFileBytes = static_cast<qint64>(sequenceChunkBytes) + 20;
    QString sequenceError;
    const bool sequenceStarted = assetSequence.begin(
        assetHash, QStringLiteral("peer-a"), sequenceFileBytes, sequenceChunkBytes, sequenceError);
    const jam2::application::asset_chunk::Chunk firstAsset{
        assetHash, 0, 0, QByteArray(sequenceChunkBytes, 'a')};
    const bool firstAccepted = assetSequence.accept(firstAsset, QStringLiteral("peer-a"), sequenceError);
    auto overlappingAsset = firstAsset;
    overlappingAsset.index = 1;
    const bool overlapRejected = !assetSequence.accept(
        overlappingAsset, QStringLiteral("peer-a"), sequenceError);
    const jam2::application::asset_chunk::Chunk finalAsset{
        assetHash, 1, static_cast<quint64>(sequenceChunkBytes), QByteArray(20, 'b')};
    const bool finalAccepted = assetSequence.accept(finalAsset, QStringLiteral("peer-a"), sequenceError);
    const bool completionAccepted = assetSequence.finish(
        assetHash, QStringLiteral("peer-a"), 2, sequenceError);
    record(QStringLiteral("asset.sequence-enforces-order-offset-and-completion"),
        sequenceStarted && firstAccepted && overlapRejected && finalAccepted && completionAccepted,
        sequenceError);
    assetSequence.reset();
    sequenceError.clear();
    record(QStringLiteral("asset.sequence-cancel-rejects-late-chunk"),
        !assetSequence.accept(finalAsset, QStringLiteral("peer-a"), sequenceError),
        sequenceError);

    jam2::application::asset_chunk::ReceiveSequence sourceBoundSequence;
    sequenceError.clear();
    const bool sourceBoundStarted = sourceBoundSequence.begin(
        assetHash, QStringLiteral("peer-a"), sequenceFileBytes, sequenceChunkBytes, sequenceError);
    const bool wrongSourceRejected = !sourceBoundSequence.accept(
        firstAsset, QStringLiteral("peer-b"), sequenceError);
    const bool correctSourceStillAccepted = sourceBoundSequence.accept(
        firstAsset, QStringLiteral("peer-a"), sequenceError);
    record(QStringLiteral("asset.sequence-source-mismatch-does-not-advance"),
        sourceBoundStarted && wrongSourceRejected && correctSourceStillAccepted,
        sequenceError);

    jam2::application::asset_chunk::ReceiveSequence orderSequence;
    sequenceError.clear();
    const bool orderStarted = orderSequence.begin(
        assetHash, QStringLiteral("peer-a"), sequenceFileBytes, sequenceChunkBytes, sequenceError);
    auto outOfOrderAsset = firstAsset;
    outOfOrderAsset.index = 1;
    outOfOrderAsset.offset = static_cast<quint64>(sequenceChunkBytes);
    const bool outOfOrderRejected = !orderSequence.accept(
        outOfOrderAsset, QStringLiteral("peer-a"), sequenceError);
    const bool earlyCompletionRejected = !orderSequence.finish(
        assetHash, QStringLiteral("peer-a"), 0, sequenceError);
    record(QStringLiteral("asset.sequence-rejects-out-of-order-and-early-completion"),
        orderStarted && outOfOrderRejected && earlyCompletionRejected,
        sequenceError);

    jam2::application::asset_chunk::ReceiveSequence sizeSequence;
    sequenceError.clear();
    const bool sizeStarted = sizeSequence.begin(
        assetHash, QStringLiteral("peer-a"), sequenceFileBytes, sequenceChunkBytes, sequenceError);
    auto oversizedAsset = firstAsset;
    oversizedAsset.data = QByteArray(sequenceChunkBytes + 1, 'x');
    const bool oversizedRejected = !sizeSequence.accept(
        oversizedAsset, QStringLiteral("peer-a"), sequenceError);
    auto undersizedNonFinalAsset = firstAsset;
    undersizedNonFinalAsset.data.chop(1);
    const bool undersizedNonFinalRejected = !sizeSequence.accept(
        undersizedNonFinalAsset, QStringLiteral("peer-a"), sequenceError);
    auto wrongHashAsset = firstAsset;
    wrongHashAsset.sha256 = QString(64, QLatin1Char('c'));
    const bool wrongHashRejected = !sizeSequence.accept(
        wrongHashAsset, QStringLiteral("peer-a"), sequenceError);
    record(QStringLiteral("asset.sequence-rejects-chunk-bound-and-hash-mismatch"),
        sizeStarted && oversizedRejected && undersizedNonFinalRejected && wrongHashRejected,
        sequenceError);

    jam2::application::asset_chunk::ReceiveSequence nonCanonicalChunkSequence;
    sequenceError.clear();
    record(QStringLiteral("asset.sequence-rejects-unbounded-chunk-count-declaration"),
        !nonCanonicalChunkSequence.begin(
            assetHash,
            QStringLiteral("peer-a"),
            sequenceFileBytes,
            sequenceChunkBytes - 1,
            sequenceError),
        sequenceError);

    const QByteArray maximumChunk = jam2::application::asset_chunk::encode({
        assetHash,
        0,
        0,
        QByteArray(jam2::application::limits::kMaximumAssetChunkBytes, 'm'),
    });
    const QByteArray excessiveChunk = jam2::application::asset_chunk::encode({
        assetHash,
        0,
        0,
        QByteArray(jam2::application::limits::kMaximumAssetChunkBytes + 1, 'x'),
    });
    record(QStringLiteral("asset.binary-codec-exact-chunk-bound"),
        maximumChunk.size() == jam2::application::asset_chunk::kHeaderBytes +
                jam2::application::limits::kMaximumAssetChunkBytes &&
            excessiveChunk.isEmpty());

    QString modelError;
    record(QStringLiteral("model.accept-valid-beat"),
        jam2::application::validateControlMessage(controlMessage, modelError), modelError);
    QJsonObject invalidBeat = controlMessage;
    invalidBeat.insert(QStringLiteral("beat"), 512);
    modelError.clear();
    record(QStringLiteral("model.reject-out-of-range-beat"),
        !jam2::application::validateControlMessage(invalidBeat, modelError), modelError);
    modelError.clear();
    record(QStringLiteral("authorization.reject-unknown-family"),
        !jam2::application::validateControlMessage(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("debug.set-remote-state")}},
            modelError),
        modelError);
    QJsonArray membershipEntries;
    for (int index = 0; index < 64; ++index) {
        const QString peerToken = QStringLiteral("%1%2")
            .arg(index + 1, 16, 16, QLatin1Char('0'))
            .arg(index + 1, 16, 16, QLatin1Char('0'));
        membershipEntries.append(QJsonObject{
            {QStringLiteral("token"), peerToken},
            {QStringLiteral("peer_id"), QString::number(index + 1)},
            {QStringLiteral("endpoint"), QStringLiteral("127.0.0.1:%1").arg(49000 + index)},
        });
    }
    const QJsonObject validMembershipPage{
        {QStringLiteral("type"), QStringLiteral("session.membership")},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("page_index"), 0},
        {QStringLiteral("page_count"), 2},
        {QStringLiteral("coordinator_token"), QString(32, QLatin1Char('1'))},
        {QStringLiteral("peers"), membershipEntries},
    };
    modelError.clear();
    record(QStringLiteral("membership.accept-bounded-page"),
        jam2::application::validateControlMessage(validMembershipPage, modelError), modelError);
    QJsonObject zeroIdentityMembership = validMembershipPage;
    QJsonArray zeroIdentityEntries = membershipEntries;
    QJsonObject zeroIdentityPeer = zeroIdentityEntries.first().toObject();
    zeroIdentityPeer[QStringLiteral("token")] = QString(16, QLatin1Char('0')) +
        QString(16, QLatin1Char('1'));
    zeroIdentityEntries[0] = zeroIdentityPeer;
    zeroIdentityMembership[QStringLiteral("peers")] = zeroIdentityEntries;
    modelError.clear();
    record(QStringLiteral("membership.reject-zero-derived-peer-id"),
        !jam2::application::validateControlMessage(zeroIdentityMembership, modelError), modelError);
    QJsonObject mismatchedIdentityMembership = validMembershipPage;
    QJsonArray mismatchedIdentityEntries = membershipEntries;
    QJsonObject mismatchedIdentityPeer = mismatchedIdentityEntries.first().toObject();
    mismatchedIdentityPeer[QStringLiteral("peer_id")] = QStringLiteral("99");
    mismatchedIdentityEntries[0] = mismatchedIdentityPeer;
    mismatchedIdentityMembership[QStringLiteral("peers")] = mismatchedIdentityEntries;
    modelError.clear();
    record(QStringLiteral("membership.reject-token-id-mismatch"),
        !jam2::application::validateControlMessage(mismatchedIdentityMembership, modelError), modelError);
    membershipEntries.append(QJsonObject{
        {QStringLiteral("token"), QString(32, QLatin1Char('f'))},
        {QStringLiteral("peer_id"), QStringLiteral("65")},
        {QStringLiteral("endpoint"), QStringLiteral("127.0.0.1:49065")},
    });

    QJsonObject excessiveMembershipPage = validMembershipPage;
    excessiveMembershipPage[QStringLiteral("peers")] = membershipEntries;
    modelError.clear();
    record(QStringLiteral("membership.reject-oversized-page"),
        !jam2::application::validateControlMessage(excessiveMembershipPage, modelError), modelError);
    QJsonArray invalidBanks;
    for (int index = 0; index < 5; ++index) {
        invalidBanks.append(QJsonObject{{QStringLiteral("lanes"), QJsonArray{}}});
    }
    QJsonObject invalidSongModel = BeatGridModel{}.toJson();
    invalidSongModel.insert(
        QStringLiteral("looper"),
        QJsonObject{{QStringLiteral("banks"), invalidBanks}});
    const QJsonObject invalidSong{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("arrangement_revision"), 1},
        {QStringLiteral("song"), invalidSongModel},
    };
    modelError.clear();
    record(QStringLiteral("asset-model.reject-excessive-banks"),
        !jam2::application::validateControlMessage(invalidSong, modelError), modelError);
    QJsonObject collaborativeSong = invalidSong;
    collaborativeSong[QStringLiteral("arrangement_revision")] = 0;
    collaborativeSong[QStringLiteral("host_authoritative")] = false;
    QJsonObject collaborativeSongModel = BeatGridModel{}.toJson();
    collaborativeSongModel.insert(QStringLiteral("looper"), LooperProject{}.toJson());
    collaborativeSong[QStringLiteral("song")] = collaborativeSongModel;
    modelError.clear();
    record(QStringLiteral("asset-model.accept-collaborative-proposal"),
        jam2::application::validateControlMessage(collaborativeSong, modelError),
        modelError);
    {
        LooperProject local;
        LooperLane incompatible;
        incompatible.id = QStringLiteral("local-wrong-rate");
        incompatible.assetPath = QStringLiteral("C:/fixtures/wrong-rate.wav");
        incompatible.assetHash = QString(64, QLatin1Char('c'));
        incompatible.name = QStringLiteral("Local 44.1 kHz lane");
        incompatible.sampleRate = 44100;
        incompatible.sampleRateCompatible = false;
        const bool appended = local.appendLane(0, std::move(incompatible));
        QJsonObject incoming = BeatGridModel{}.toJson();
        incoming.insert(QStringLiteral("looper"), LooperProject{}.toJson());
        const int preserved = mergeQuarantinedLocalLanes(incoming, local, 48000);
        LooperProject merged;
        const bool loaded = merged.loadJson(incoming.value(QStringLiteral("looper")).toObject());
        if (loaded && !merged.banks().at(0).lanes.isEmpty()) {
            merged.banks()[0].lanes[0].sampleRateCompatible =
                merged.banks().at(0).lanes.at(0).sampleRate == 48000;
        }
        const QJsonArray syncBanks = merged.toJson(true)
            .value(QStringLiteral("banks")).toArray();
        record(QStringLiteral("wav.quarantine-preserved-and-excluded-from-sync"),
            appended && preserved == 1 && loaded &&
            merged.banks().at(0).lanes.size() == 1 &&
            merged.banks().at(0).lanes.at(0).sampleRate == 44100 &&
            syncBanks.at(0).toObject().value(QStringLiteral("lanes")).toArray().isEmpty());
    }
    QJsonObject invalidNestedSong = collaborativeSong;
    QJsonObject invalidNestedModel = collaborativeSongModel;
    QJsonArray invalidNestedSections = invalidNestedModel.value(QStringLiteral("sections")).toArray();
    QJsonObject invalidNestedSection = invalidNestedSections.at(0).toObject();
    invalidNestedSection[QStringLiteral("beats")] = 513;
    invalidNestedSections[0] = invalidNestedSection;
    invalidNestedModel[QStringLiteral("sections")] = invalidNestedSections;
    invalidNestedSong[QStringLiteral("song")] = invalidNestedModel;
    modelError.clear();
    record(QStringLiteral("model.reject-invalid-nested-song-before-mutation"),
        !jam2::application::validateControlMessage(invalidNestedSong, modelError),
        modelError);
    {
        LooperProject initial;
        LooperLane first;
        first.id = QStringLiteral("first-lane");
        LooperLane target;
        target.id = QStringLiteral("armed-lane");
        const bool appended = initial.appendLane(0, std::move(first)) &&
            initial.appendLane(0, std::move(target));
        const QString bankId = initial.banks().at(0).id;
        const QString laneId = initial.banks().at(0).lanes.at(1).id;
        const bool moved = initial.moveLane(0, 1, 0);
        LooperProject replaced;
        const bool loaded = replaced.loadJson(initial.toJson());
        const LooperLaneLocation resolved = findLooperLaneLocation(
            replaced, bankId, laneId);
        record(QStringLiteral("track-arm.resolve-stable-lane-after-snapshot"),
            appended && moved && loaded && resolved.valid() &&
            resolved.bank == 0 && resolved.lane == 0 &&
            replaced.banks().at(resolved.bank).lanes.at(resolved.lane).id == laneId);
    }
    const QJsonObject validTrackReady{
        {QStringLiteral("type"), QStringLiteral("track.ready")},
        {QStringLiteral("arrangement_revision"), 1},
    };
    modelError.clear();
    record(QStringLiteral("track-ready.accept-positive-revision"),
        jam2::application::validateControlMessage(validTrackReady, modelError), modelError);
    QJsonObject invalidTrackReady = validTrackReady;
    invalidTrackReady[QStringLiteral("arrangement_revision")] = 0;
    modelError.clear();
    record(QStringLiteral("track-ready.reject-zero-revision"),
        !jam2::application::validateControlMessage(invalidTrackReady, modelError), modelError);
    record(QStringLiteral("track-sync.classifies-all-control-payloads"),
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("song.set")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("track.ready")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.track.share.request")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.recording.offer")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.asset.request")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.asset.start")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.asset.done")) &&
        !jam2::application::isTrackSyncControlMessageType(QStringLiteral("metronome.settings")));
    const QJsonObject validTrackOffer{
        {QStringLiteral("type"), QStringLiteral("looper.recording.offer")},
        {QStringLiteral("recording_id"), QStringLiteral("12345678-1234-1234-1234-123456789abc")},
        {QStringLiteral("bank"), 0},
        {QStringLiteral("target_lane_id"), QStringLiteral("peer-lane")},
        {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("name"), QStringLiteral("Peer WAV")},
        {QStringLiteral("sample_rate"), 48000},
    };
    modelError.clear();
    record(QStringLiteral("track-share.accepts-bounded-additive-offer"),
        jam2::application::validateControlMessage(validTrackOffer, modelError),
        modelError);
    {
        LooperProject additive;
        LooperLane creatorLane;
        creatorLane.id = QStringLiteral("creator-lane");
        creatorLane.assetHash = QString(64, QLatin1Char('a'));
        LooperLane peerLane;
        peerLane.id = QStringLiteral("peer-lane");
        peerLane.assetHash = QString(64, QLatin1Char('b'));
        const bool creatorAdded = additive.appendLane(0, std::move(creatorLane));
        const bool peerAdded = additive.appendLane(0, std::move(peerLane));
        record(QStringLiteral("track-share.additive-union-preserves-both-lanes"),
            creatorAdded && peerAdded && additive.banks().at(0).lanes.size() == 2 &&
            additive.banks().at(0).lanes.at(0).assetHash !=
                additive.banks().at(0).lanes.at(1).assetHash);
    }
    {
        LooperProject project;
        project.setTrackSyncEnabled(false);
        QJsonObject legacySnapshot = project.toJson();
        legacySnapshot.insert(QStringLiteral("track_sync"), true);
        record(QStringLiteral("track-sync.project-snapshot-cannot-enable-local-sync"),
            project.loadJson(legacySnapshot) &&
            !project.trackSyncEnabled() &&
            !project.toJson().contains(QStringLiteral("track_sync")));
    }
    record(QStringLiteral("track-timeline.playhead-independent-view-range"),
        jam2::gui::looperTimelineViewFrames(48000, 96000, -1, -1) == 384000 &&
        jam2::gui::looperTimelineViewFrames(48000, 480000, -1, -1) == 480000 &&
        jam2::gui::looperTimelineViewFrames(48000, 96000, -1, 12000) == 576000);

    {
        PlaybackGrid grid;
        grid.setPattern(60.0, 4, 1);
        grid.updateEngine(200, 200, 100, 0, 100, true);
        grid.scheduleEpoch(300, 300, 1);
        grid.updateEngine(250, 250, 100, 0, 100, true);
        const bool futureTransportEpochDeferred = grid.position().epochFrame == 100;
        grid.updateEngine(400, 400, 100, 0, 100, true);
        const bool transportEpochApplied = grid.position().epochFrame == 300;
        grid.updateEngine(500, 500, 100, 0, 100, false);
        grid.updateEngine(1100, 1100, 1000, 0, 100, true);
        const PlaybackGrid::Position restarted = grid.position();
        record(QStringLiteral("metronome.restart-clears-prior-transport-epoch"),
            futureTransportEpochDeferred && transportEpochApplied && restarted.running &&
            restarted.epochFrame == 1000 && restarted.absoluteBeat == 1);
        record(QStringLiteral("record-start.future-epoch-remains-deferred-until-target"),
            futureTransportEpochDeferred && transportEpochApplied);
    }
    {
        PlaybackGrid grid;
        grid.setPattern(60.0, 4, 1);
        grid.updateEngine(200, 200, 100, 0, 100, true);
        grid.scheduleEpoch(300, 300, 1);
        grid.updateEngine(400, 400, 300, 0, 100, true);
        grid.scheduleEpoch(300, 300, 1);
        grid.updateEngine(500, 500, 450, 0, 100, true);
        grid.scheduleEpoch(300, 300, 1);
        record(QStringLiteral("transport-clock.stale-event-cannot-override-fresh-epoch"),
            grid.position().epochFrame == 450);
    }
    {
        record(QStringLiteral("metronome.remote-settings-are-presentation-only"),
            MetronomeTransportController::allowsLocalGridMutation(false) &&
            !MetronomeTransportController::allowsLocalGridMutation(true));
        record(QStringLiteral("record-start.is-track-sync-and-resets-shared-grid"),
            jam2::is_track_sync_transport_action(
                jam2::EngineTransportAction::RecordStart) &&
            MetronomeTransportController::transportActionResetsGridEpoch(
                jam2::EngineTransportAction::RecordStart) &&
            MetronomeTransportController::transportActionResetsGridEpoch(
                jam2::EngineTransportAction::TrackRestart) &&
            !jam2::is_track_sync_transport_action(
                jam2::EngineTransportAction::RecordStop));
    }

    record(QStringLiteral("transport-clock.map-positive-render-offset"),
        rawFrameFromMusicalFrame(
            musicalFrameFromRawFrame(1000, 200),
            200) == 1000);
    record(QStringLiteral("transport-clock.map-negative-render-offset"),
        rawFrameFromMusicalFrame(
            musicalFrameFromRawFrame(1200, -200),
            -200) == 1200);
    {
        const auto establishedEngine = jam2::metronome::map_authority_clock(
            1000, 5000, 8000);
        const auto youngerEngine = jam2::metronome::map_authority_clock(
            1000, 5000, 2000);
        const auto futureStart = jam2::metronome::map_authority_clock(
            5000, 4000, 2000);
        record(QStringLiteral("transport-clock.late-join-preserves-absolute-position"),
            establishedEngine.valid && establishedEngine.epoch_sample_time == 4000 &&
            establishedEngine.render_offset_frames == 0 &&
            youngerEngine.valid && youngerEngine.epoch_sample_time == 0 &&
            youngerEngine.render_offset_frames == 2000 &&
            futureStart.valid && futureStart.epoch_sample_time == 3000 &&
            futureStart.render_offset_frames == 0);
    }
    record(QStringLiteral("record-count-in.wait-then-4-3-2-1"),
        recordingCountInBeat(1000, 5000, 1000) == 4 &&
        recordingCountInBeat(2000, 5000, 1000) == 3 &&
        recordingCountInBeat(3000, 5000, 1000) == 2 &&
        recordingCountInBeat(4000, 5000, 1000) == 1 &&
        recordingCountInBeat(5000, 5000, 1000) == 0);
    record(QStringLiteral("record-count-in.starts-at-safe-whole-bar"),
        jam2::gui::recording_count_in_bar_beat(3, 4, 10000, 10100, 48000) == 8 &&
        jam2::gui::recording_count_in_bar_beat(3, 4, 10000, 20000, 48000) == 4);
    record(QStringLiteral("record-count-in.waits-for-fresh-grid-after-start"),
        !jam2::gui::recording_grid_ready_for_count_in(true, true, 1000, true, 1000) &&
        jam2::gui::recording_grid_ready_for_count_in(true, true, 1200, true, 1000) &&
        jam2::gui::recording_grid_ready_for_count_in(true, true, 1000, false, 1000) &&
        !jam2::gui::recording_grid_ready_for_count_in(false, true, 1200, true, 1000));
    record(QStringLiteral("record-stop.quantizes-to-next-whole-bar"),
        nextGridBoundaryBeat(0, 4, true) == 4 &&
        nextGridBoundaryBeat(3, 4, true) == 4 &&
        nextGridBoundaryBeat(4, 4, true) == 8 &&
        nextGridBoundaryBeat(15, 4, true) == 16 &&
        nextGridBoundaryBeat(8, 3, true) == 9 &&
        nextGridBoundaryBeat(8, 3, false) == 9);
    {
        SharedTrackController playback;
        playback.waitForAssets(7, true);
        const bool waitingAssets =
            playback.playback().phase == SharedTrackController::PlaybackPhase::WaitingForAssets &&
            playback.playback().requestedPlaying;
        playback.prepareMix(7, true);
        playback.preparedForTransport(7);
        const bool waitingTransport = playback.playback().phase ==
            SharedTrackController::PlaybackPhase::WaitingForTransport;
        playback.observeEnginePlaying(true);
        const bool playing = playback.playback().phase ==
            SharedTrackController::PlaybackPhase::Playing;
        playback.requestPlayback(false, 8);
        playback.observeEnginePlaying(true);
        const bool stopPending = !playback.playback().requestedPlaying &&
            playback.playback().phase == SharedTrackController::PlaybackPhase::WaitingForTransport;
        playback.observeEnginePlaying(false);
        const bool stopped = playback.playback().phase ==
            SharedTrackController::PlaybackPhase::Stopped;
        record(QStringLiteral("track-playback.typed-state-through-asset-ready-transport"),
            waitingAssets && waitingTransport && playing && stopPending && stopped);
    }
    {
        Jam2RuntimeHost requestIds;
        const std::uint64_t gridBeforeReset = requestIds.nextGridRequestId();
        const std::uint64_t transportBeforeReset = requestIds.nextTransportEventId();
        requestIds.reset();
        const std::uint64_t gridAfterReset = requestIds.nextGridRequestId();
        const std::uint64_t transportAfterReset = requestIds.nextTransportEventId();
        record(QStringLiteral("grid-request.monotonic-across-network-reset"),
            gridBeforeReset != 0 && gridAfterReset > gridBeforeReset);
        record(QStringLiteral("transport-event.monotonic-across-network-reset"),
            transportBeforeReset != 0 && transportAfterReset > transportBeforeReset);
    }

    {
        jam2::SessionAuthority authority(1, 1, 1);
        jam2::GridProposal proposal;
        proposal.requester_peer_id = 1;
        proposal.request_id = 1;
        proposal.run_state = jam2::GridRunState::Running;
        const auto grid = authority.orderGridProposal(proposal);
        record(QStringLiteral("transport-authority.create-grid"), grid.has_value());
        const std::uint64_t gridRevision = grid ? grid->revision : 0;
        record(QStringLiteral("transport-authority.accept-track-from-peer"),
            authority.acceptTransportEvent(2, 1, gridRevision, false));
        record(QStringLiteral("transport-authority.reject-peer-replay"),
            !authority.acceptTransportEvent(2, 1, gridRevision, false));
        record(QStringLiteral("transport-authority.accept-independent-peer-counter"),
            authority.acceptTransportEvent(3, 1, gridRevision, false));
        record(QStringLiteral("transport-authority.reject-peer-record"),
            !authority.acceptTransportEvent(2, 2, gridRevision, true));
        record(QStringLiteral("transport-authority.accept-peer-track-sync-recordstart"),
            authority.acceptTransportEvent(2, 2, gridRevision, false));
        record(QStringLiteral("transport-authority.accept-authority-record"),
            authority.acceptTransportEvent(1, 1, gridRevision, true));
    }

    {
        jam2::SessionAuthority authority(1, 1, 1);
        jam2::GridProposal peerStart;
        peerStart.requester_peer_id = 2;
        peerStart.request_id = 1;
        peerStart.run_state = jam2::GridRunState::Running;
        const auto initial = authority.orderGridProposal(peerStart);
        const bool missing = authority.markPeerInactive(2);
        jam2::GridProposal localRecovery;
        localRecovery.requester_peer_id = 1;
        localRecovery.request_id = 1;
        localRecovery.run_state = jam2::GridRunState::Running;
        const auto recovered = authority.orderGridProposal(localRecovery);
        record(QStringLiteral("grid-authority.departure-recovers-running-local-grid"),
            initial.has_value() && missing && recovered.has_value() &&
            recovered->authority_peer_id == 1 &&
            recovered->run_state == jam2::GridRunState::Running &&
            recovered->revision > initial->revision);
    }

    {
        const std::array<std::int32_t, 5> samples{
            -8388608, -256, 0, 256, 8388352};
        const auto pcm16 = jam2::protocol::pack_pcm16(samples);
        const auto pcm24 = jam2::protocol::pack_pcm24(samples);
        record(QStringLiteral("protocol.pcm16-bounded-roundtrip"),
            pcm16.size() == samples.size() * 2 &&
            jam2::protocol::unpack_pcm16(pcm16) ==
                std::vector<std::int32_t>(samples.begin(), samples.end()));
        record(QStringLiteral("protocol.pcm24-bounded-roundtrip"),
            pcm24.size() == samples.size() * 3 &&
            jam2::protocol::unpack_pcm24(pcm24) ==
                std::vector<std::int32_t>(samples.begin(), samples.end()));

        std::array<std::uint8_t, 16> key{};
        for (std::size_t index = 0; index < key.size(); ++index) {
            key[index] = static_cast<std::uint8_t>(index);
        }
        jam2::protocol::Header header;
        header.type = jam2::protocol::PacketType::Audio;
        header.session_id = 0x0102030405060708ULL;
        header.sequence = 0x0a0b0c0dU;
        header.timing_value = 0x1112131415161718ULL;
        auto packet16 = jam2::protocol::encode_packet(
            header, pcm16, key, jam2::NetworkAudioFormat::Pcm16Mono);
        const auto packet24 = jam2::protocol::encode_packet(
            header, pcm24, key, jam2::NetworkAudioFormat::Pcm24Mono);
        const QByteArray expected16 = QByteArray::fromHex(
            "4a414d3202030a0008070605040302010d0c0b0a181716151413121145739dfac7e8d2bb0080ffff00000100ff7f");
        const QByteArray expected24 = QByteArray::fromHex(
            "4a414d3202030f0008070605040302010d0c0b0a1817161514131211f8f96efd3b229db200008000ffff00000000010000ff7f");
        record(QStringLiteral("protocol.pcm16-golden-wire-vector"),
            QByteArray(reinterpret_cast<const char*>(packet16.data()),
                       static_cast<qsizetype>(packet16.size())) == expected16);
        record(QStringLiteral("protocol.pcm24-golden-wire-vector"),
            QByteArray(reinterpret_cast<const char*>(packet24.data()),
                       static_cast<qsizetype>(packet24.size())) == expected24);
        record(QStringLiteral("protocol.parse-both-audio-formats"),
            jam2::protocol::parse_packet(
                packet16, key, header.session_id, jam2::NetworkAudioFormat::Pcm16Mono) &&
            jam2::protocol::parse_packet(
                packet24, key, header.session_id, jam2::NetworkAudioFormat::Pcm24Mono));
        packet16.pop_back();
        record(QStringLiteral("protocol.reject-exact-size-mismatch"),
            jam2::protocol::parse_packet(
                packet16, key, header.session_id, jam2::NetworkAudioFormat::Pcm16Mono).error ==
                jam2::protocol::ParseError::InvalidPayloadSize);
    }

    {
        jam2::PeerStreamConfig config;
        config.sample_rate = 44100;
        config.frames_per_packet = 64;
        config.stats_warmup_us = 0;
        std::array<std::int32_t, 64> samples{};
        const std::vector<std::uint8_t> payload = jam2::protocol::pack_pcm24(samples);
        jam2::protocol::Header header;
        header.type = jam2::protocol::PacketType::Audio;
        header.sequence = 1;
        header.timing_value = 60ULL * 44100ULL;
        header.payload_length = static_cast<std::uint16_t>(payload.size());
        jam2::PeerStream stream(config, 1, nullptr);
        record(QStringLiteral("peer-stream.accept-late-join-baseline"),
            stream.receiveAudio(header, payload, 2) == jam2::PeerAudioResult::Accepted);
        ++header.sequence;
        header.timing_value += samples.size();
        record(QStringLiteral("peer-stream.accept-contiguous-late-join-audio"),
            stream.receiveAudio(header, payload, 3) == jam2::PeerAudioResult::Accepted);
        ++header.sequence;
        header.timing_value += 10ULL * 44100ULL + samples.size() + 1ULL;
        record(QStringLiteral("peer-stream.reject-post-baseline-future-jump"),
            stream.receiveAudio(header, payload, 4) == jam2::PeerAudioResult::FutureSampleTime);
    }

    {
        bool initialReady = false;
        bool initialFullBlock = false;
        bool initialPartialBlock = false;
        bool reattachReady = false;
        bool reattachFullBlock = false;
        bool reattachPartialBlock = false;
        QString captureError;
        jam2::Engine engine;
        bool engineStarted = false;
        try {
            jam2::EngineConfig config;
            config.backend = jam2::EngineAudioBackend::Headless;
            config.sample_rate = 44100;

            config.audio_buffer_frames = 32;
            config.capture_ring_frames = 512;
            config.playback_ring_frames = 512;
            config.test_input = jam2::EngineTestInput::Tone440;
            engine.start(config);
            engineStarted = true;

            const auto exerciseAttachment = [&](jam2::NetworkCaptureAttachment attachment,
                                                bool& ready,
                                                bool& fullBlock,
                                                bool& partialBlock) {
                const std::uint64_t readyDeadline = jam2::monotonic_us() + 1000000ULL;
                while (!engine.networkCaptureReady(attachment) &&
                       jam2::monotonic_us() < readyDeadline) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                ready = engine.networkCaptureReady(attachment);
                if (!ready) {
                    return;
                }
                std::array<std::int32_t, 64> packet{};
                const std::uint64_t exerciseDeadline = jam2::monotonic_us() + 100000ULL;
                while (jam2::monotonic_us() < exerciseDeadline) {
                    const jam2::CapturedAudioBlock captured = engine.popNetworkCapture(attachment, packet);
                    if (captured.frames == packet.size()) {
                        fullBlock = true;
                    } else if (captured.frames != 0) {
                        partialBlock = true;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            };

            jam2::NetworkCaptureAttachment attachment = engine.attachNetworkCapture();
            exerciseAttachment(attachment, initialReady, initialFullBlock, initialPartialBlock);
            engine.detachNetworkCapture(attachment);
            const std::uint64_t detachDeadline = jam2::monotonic_us() + 1000000ULL;
            while (engine.snapshot().network_capture_enabled &&
                   jam2::monotonic_us() < detachDeadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            attachment = engine.attachNetworkCapture();
            exerciseAttachment(attachment, reattachReady, reattachFullBlock, reattachPartialBlock);
            engine.detachNetworkCapture(attachment);
        } catch (const std::exception& exception) {
            captureError = QString::fromUtf8(exception.what());
        }
        if (engineStarted) {
            engine.requestStop();
            engine.join();
        }
        record(QStringLiteral("engine-capture.exact-64-from-32-initial"),
            captureError.isEmpty() && initialReady && initialFullBlock && !initialPartialBlock,
            captureError);
        record(QStringLiteral("engine-capture.exact-64-from-32-reattach"),
            captureError.isEmpty() && reattachReady && reattachFullBlock && !reattachPartialBlock,
            captureError);
    }

    if (fixtureSpecs.size() > 32) {
        record(QStringLiteral("wav.fixture-count-bound"), false, QStringLiteral("too many fixture specs"));
    } else {
        for (const QString& spec : fixtureSpecs) {
            const qsizetype separator = spec.indexOf(QLatin1Char(':'));
            const QString expectation = separator > 0 ? spec.left(separator) : QString();
            const QString path = separator > 0 ? spec.mid(separator + 1) : QString();
            const bool importMatch = expectation == QStringLiteral("import-match-48000");
            const bool importMismatch = expectation == QStringLiteral("import-mismatch-48000");
            if ((expectation != QStringLiteral("valid") && expectation != QStringLiteral("invalid") &&
                 !importMatch && !importMismatch) ||
                path.isEmpty() || path.toUtf8().size() > 4096) {
                record(QStringLiteral("wav.fixture-spec-bound"), false, QStringLiteral("invalid fixture spec"));
                continue;
            }
            if (importMatch || importMismatch) {
                const StagedPcm16Asset staged = stagePcm16Asset(
                    path,
                    QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("staged")),
                    48000);
                const bool ok = importMatch
                    ? staged.error.isEmpty() && staged.metadata.sampleRate == 48000 &&
                        QFileInfo::exists(staged.stagedPath)
                    : !staged.error.isEmpty() && staged.stagedPath.isEmpty() &&
                        staged.error.contains(QStringLiteral("48000")) &&
                        staged.error.contains(QStringLiteral("44100"));
                record(
                    QStringLiteral("wav.%1.%2").arg(expectation, QFileInfo(path).fileName()),
                    ok,
                    staged.error);
            } else {
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(path), 8ULL * 1024ULL * 1024ULL);
                const bool expectedValid = expectation == QStringLiteral("valid");
                record(
                    QStringLiteral("wav.%1.%2").arg(expectation, QFileInfo(path).fileName()),
                    static_cast<bool>(inspected) == expectedValid,
                    QString::fromStdString(inspected.error));
            }
        }
    }

    return QJsonObject{
        {QStringLiteral("event"), QStringLiteral("debug_boundary_result")},
        {QStringLiteral("schema"), 1},
        {QStringLiteral("ok"), allOk},
        {QStringLiteral("cases"), cases},
    };
}
