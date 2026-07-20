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
#include "TrackWorkspaceController.hpp"
#include "TrackWorkspaceSupport.hpp"
#include "TrackRecordingWorkflow.hpp"
#include "PlaybackGrid.hpp"
#include "MetronomeTransportController.hpp"
#include "MusicTheory.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "PracticeIdeaController.hpp"
#include "PracticeReferenceRenderer.hpp"
#include "PreparedMixRenderer.hpp"
#include "SharedTrackModel.hpp"
#include "GuiLoopbackRecorder.hpp"

#include "common.hpp"
#include "engine.hpp"
#include "metronome.hpp"
#include "pcm16_wav.hpp"
#include "prepared_track_source.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include "tuning_profile.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QUuid>
#include <QtEndian>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace {

class AssetTransferValidationContext final : public QObject, public AssetTransferContext {
public:
    explicit AssetTransferValidationContext(QString folder)
        : folder_(std::move(folder))
    {
    }

    QObject* dispatchContext() noexcept override { return this; }
    bool trackSyncEnabled() const noexcept override { return true; }
    int sessionSampleRate() const noexcept override { return 48000; }
    QString assetPathForSend(const QString&) const override { return {}; }
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
    void abandonIncomingAsset(const QString&) override
    {
        ++abandoned;
        active = false;
    }
    void acceptIncomingAsset(const QString& hash, const QString& path) override
    {
        ++accepted;
        acceptedHash = hash;
        acceptedPath = path;
        active = false;
    }
    void appendAssetLog(const QString&) override {}
    bool startAssetFileTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)>) override
    {
        work();
        complete();
        return true;
    }
    bool canQueueAssetControl(const QString&, qint64) const override { return true; }
    bool sendAssetControl(const QString&, const QJsonObject&) override { return true; }
    bool sendAssetBinary(const QString&, const QByteArray&) override { return true; }

    bool active = true;
    QString expectedHash;
    QString expectedSource = QStringLiteral("peer-a");
    int abandoned = 0;
    int accepted = 0;
    QString acceptedHash;
    QString acceptedPath;

private:
    QString folder_;
};

template <typename T>
void appendLittleEndian(QByteArray& bytes, T value)
{
    const T encoded = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char*>(&encoded),
        static_cast<qsizetype>(sizeof(encoded)));
}

QByteArray minimalPcm16Wav()
{
    constexpr quint32 dataBytes = 4;
    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendLittleEndian<quint32>(bytes, 36 + dataBytes);
    bytes.append("WAVEfmt ", 8);
    appendLittleEndian<quint32>(bytes, 16);
    appendLittleEndian<quint16>(bytes, 1);
    appendLittleEndian<quint16>(bytes, 1);
    appendLittleEndian<quint32>(bytes, 48000);
    appendLittleEndian<quint32>(bytes, 96000);
    appendLittleEndian<quint16>(bytes, 2);
    appendLittleEndian<quint16>(bytes, 16);
    bytes.append("data", 4);
    appendLittleEndian<quint32>(bytes, dataBytes);
    appendLittleEndian<qint16>(bytes, 0);
    appendLittleEndian<qint16>(bytes, 1);
    return bytes;
}

std::filesystem::path nativeFilePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

bool pcm16WavHasSignal(const QString& path, const jam2::wav::Pcm16Info& info)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) ||
        !file.seek(static_cast<qint64>(info.data_offset))) {
        return false;
    }
    const QByteArray pcm = file.read(static_cast<qint64>(
        qMin<std::uint64_t>(info.data_bytes, 1024ULL * 1024ULL)));
    for (qsizetype offset = 0; offset + 1 < pcm.size(); offset += 2) {
        const qint16 sample = qFromLittleEndian<qint16>(
            reinterpret_cast<const uchar*>(pcm.constData() + offset));
        if (std::abs(static_cast<int>(sample)) > 32) return true;
    }
    return false;
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
    {
        jam2::audio::SmoothedMonoDownmix downmix;
        downmix.configure(2, 48000.0, 64);
        const auto render = [&downmix](
                                const std::array<double, 2>& peaks,
                                const std::array<double, 2>& samples) {
            downmix.beginBlock(peaks);
            std::array<double, 64> output{};
            for (std::size_t frame = 0; frame < output.size(); ++frame) {
                double sum = 0.0;
                for (std::size_t channel = 0; channel < samples.size(); ++channel) {
                    sum += samples[channel] * downmix.weightAt(channel, frame);
                }
                output[frame] = sum * downmix.normalizationAt(frame);
            }
            return output;
        };
        std::array<double, 64> stable{};
        for (int block = 0; block < 200; ++block) {
            stable = render({0.5, 0.0}, {0.5, 0.0});
        }
        const double beforeJoin = stable.back();
        const std::array<double, 64> joining = render({0.5, 0.02}, {0.5, 0.02});
        double largestStep = std::abs(joining.front() - beforeJoin);
        for (std::size_t frame = 1; frame < joining.size(); ++frame) {
            largestStep = std::max(
                largestStep, std::abs(joining[frame] - joining[frame - 1U]));
        }
        for (int block = 0; block < 800; ++block) {
            stable = render({0.5, 0.0}, {0.5, 0.0});
        }
        record(QStringLiteral("audio.smoothed-downmix-no-binary-channel-step"),
            largestStep < 0.005 &&
            std::abs(stable.back() - 0.5) < 0.001 &&
            downmix.channelWeight(0) > 0.999 &&
            downmix.channelWeight(1) < 0.001 &&
            downmix.transitionCount() >= 3);
    }
    {
        jam2::audio::SmoothedMonoDownmix downmix;
        downmix.configure(2, 48000.0, 64);
        for (int block = 0; block < 1500; ++block) {
            downmix.beginBlock({std::array<double, 2>{0.35, 0.0005}});
        }
        const bool fastAttackAndLearnedFloor =
            downmix.channelWeight(0) > 0.99 &&
            downmix.channelWeight(1) < 0.001 &&
            downmix.channelNoiseFloor(1) > 0.00045;
        for (int block = 0; block < 700; ++block) {
            const double decayingNote = 0.35 * std::exp(-static_cast<double>(block) / 18.0);
            downmix.beginBlock({std::array<double, 2>{decayingNote, 0.0005}});
        }
        record(QStringLiteral("audio.downmix-per-channel-noise-floor-does-not-return-on-note-decay"),
            fastAttackAndLearnedFloor &&
            downmix.channelWeight(0) < 0.001 &&
            downmix.channelWeight(1) < 0.001 &&
            downmix.channelNoiseFloor(1) > 0.00045);
    }
    {
        jam2::audio::SmoothedMonoDownmix downmix;
        downmix.configure(4, 48000.0, 64);
        const auto render = [&downmix](
                                const std::array<double, 4>& peaks,
                                const std::array<double, 4>& samples) {
            downmix.beginBlock(peaks);
            double maximum = 0.0;
            double last = 0.0;
            for (std::size_t frame = 0; frame < 64; ++frame) {
                double sum = 0.0;
                for (std::size_t channel = 0; channel < samples.size(); ++channel) {
                    sum += samples[channel] * downmix.weightAt(channel, frame);
                }
                last = sum * downmix.normalizationAt(frame);
                maximum = std::max(maximum, std::abs(last));
            }
            return std::pair<double, double>{last, maximum};
        };
        std::pair<double, double> allActive{};
        for (int block = 0; block < 300; ++block) {
            allActive = render(
                {0.8, 0.8, 0.8, 0.8},
                {0.8, 0.8, 0.8, 0.8});
        }
        std::pair<double, double> oneActive{};
        for (int block = 0; block < 1000; ++block) {
            oneActive = render(
                {0.8, 0.0, 0.0, 0.0},
                {0.8, 0.0, 0.0, 0.0});
        }
        record(QStringLiteral("audio.smoothed-downmix-four-channel-level-and-headroom"),
            std::abs(allActive.first - 0.8) < 0.001 &&
            allActive.second <= 0.800001 &&
            std::abs(oneActive.first - 0.8) < 0.001 &&
            oneActive.second <= 0.800001 &&
            std::abs(downmix.effectiveWeight() - 1.0) < 0.001 &&
            std::abs(downmix.normalizationGain() - 1.0) < 0.001);
    }
    record(QStringLiteral("recording-rate.authority-precedence"),
        jam2::gui::resolve_active_sample_rate(44100, 48000.0, 48000) == 44100 &&
        jam2::gui::resolve_active_sample_rate(0, 44100.0, 48000) == 44100 &&
        jam2::gui::resolve_active_sample_rate(0, 0.0, 44100) == 44100 &&
        jam2::gui::resolve_active_sample_rate(0, 0.0, 0) == 48000);
    record(QStringLiteral("recording-rate.engine-contract-guard"),
        jam2::gui::sample_rate_matches_engine(44100, 44100.0) &&
        jam2::gui::sample_rate_matches_engine(44100, 44100.75) &&
        !jam2::gui::sample_rate_matches_engine(44100, 48000.0) &&
        !jam2::gui::sample_rate_matches_engine(44100, 0.0));
    {
        jam2::EngineConfig active;
        active.sample_rate = 48000;
        jam2::EngineConfig requested = active;
        requested.sample_rate = 44100;
        record(QStringLiteral("recording-rate.engine-restarts-for-contract"),
            jam2_engine_restart_required(active, requested));
    }
    {
        Jam2RuntimeOptions options;
        options.output_level = 0.25;
        const jam2::EngineConfig configured =
            jam2_make_engine_config(options, true);
        jam2::EngineConfig changedLevel = configured;
        changedLevel.output_level_ppm = 500000;
        record(QStringLiteral("master-output.runtime-level-is-dynamic"),
            configured.output_level_ppm == 250000 &&
            !jam2_engine_restart_required(configured, changedLevel));
    }
    {
        constexpr int sourceRate = 48000;
        constexpr int targetRate = 44100;
        constexpr double pi = 3.1415926535897932384626433832795;
        std::vector<std::int16_t> tone(sourceRate);
        for (std::size_t frame = 0; frame < tone.size(); ++frame) {
            tone[frame] = static_cast<std::int16_t>(std::lrint(
                12000.0 * std::sin(
                    2.0 * pi * 1000.0 *
                    static_cast<double>(frame) /
                    static_cast<double>(sourceRate))));
        }
        const std::vector<std::int16_t> converted =
            jam2::gui::resample_pcm16_mono(tone, sourceRate, targetRate);
        const std::vector<std::int16_t> identity =
            jam2::gui::resample_pcm16_mono(tone, sourceRate, sourceRate);
        double dot = 0.0;
        double outputEnergy = 0.0;
        double referenceEnergy = 0.0;
        for (std::size_t frame = 64;
             frame + 64 < converted.size();
             ++frame) {
            const double reference = std::sin(
                2.0 * pi * 1000.0 *
                static_cast<double>(frame) /
                static_cast<double>(targetRate));
            const double output = static_cast<double>(converted[frame]);
            dot += output * reference;
            outputEnergy += output * output;
            referenceEnergy += reference * reference;
        }
        const double correlation =
            dot / std::sqrt(outputEnergy * referenceEnergy);
        record(QStringLiteral("loopback-resample.duration-pitch-and-identity"),
            converted.size() == static_cast<std::size_t>(targetRate) &&
            identity == tone &&
            correlation > 0.999);

        std::vector<std::int16_t> highTone(sourceRate);
        for (std::size_t frame = 0; frame < highTone.size(); ++frame) {
            highTone[frame] = static_cast<std::int16_t>(std::lrint(
                16000.0 * std::sin(
                    2.0 * pi * 23000.0 *
                    static_cast<double>(frame) /
                    static_cast<double>(sourceRate))));
        }
        const std::vector<std::int16_t> filtered =
            jam2::gui::resample_pcm16_mono(
                highTone, sourceRate, targetRate);
        double filteredEnergy = 0.0;
        std::size_t filteredFrames = 0;
        for (std::size_t frame = 64;
             frame + 64 < filtered.size();
             ++frame) {
            const double sample = static_cast<double>(filtered[frame]);
            filteredEnergy += sample * sample;
            ++filteredFrames;
        }
        const double filteredRms = filteredFrames > 0
            ? std::sqrt(filteredEnergy / static_cast<double>(filteredFrames))
            : (std::numeric_limits<double>::infinity)();
        record(QStringLiteral("loopback-resample.downsample-anti-alias"),
            filteredRms < 1200.0);
    }
    {
        const QString folder = QDir(QDir::tempPath()).absoluteFilePath(
            QStringLiteral("jam2-asset-validation-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const bool folderReady = QDir().mkpath(folder);
        AssetTransferValidationContext context(folder);
        const QByteArray wav = minimalPcm16Wav();
        context.expectedHash = QString::fromLatin1(
            QCryptographicHash::hash(wav, QCryptographicHash::Sha256).toHex());
        {
            AssetTransferService transfer(context);
            transfer.receiveStart(QJsonObject{
                {QStringLiteral("sha256"), context.expectedHash},
                {QStringLiteral("file_bytes"), wav.size()},
                {QStringLiteral("chunk_size"),
                    jam2::application::limits::kMaximumAssetChunkBytes},
            }, context.expectedSource);
            transfer.receiveChunk(jam2::application::asset_chunk::encode({
                context.expectedHash, 0, 0, wav,
            }), context.expectedSource);
            transfer.receiveDone(QJsonObject{
                {QStringLiteral("sha256"), context.expectedHash},
                {QStringLiteral("chunks"), 1},
            }, context.expectedSource);
            transfer.resetIncoming();
        }
        QFile acceptedFile(context.acceptedPath);
        const bool acceptedFileReady =
            acceptedFile.open(QIODevice::ReadOnly) &&
            QCryptographicHash::hash(
                acceptedFile.readAll(), QCryptographicHash::Sha256).toHex() ==
                context.expectedHash.toLatin1();
        record(QStringLiteral("asset-transfer.success-clears-without-abandon"),
            folderReady && context.accepted == 1 && context.abandoned == 0 &&
            context.acceptedHash == context.expectedHash && acceptedFileReady);
        (void)QDir(folder).removeRecursively();
    }

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
    QJsonObject validBeatHit{
        {QStringLiteral("type"), QStringLiteral("beat.hit")},
        {QStringLiteral("section"), 0},
        {QStringLiteral("beat"), 0},
        {QStringLiteral("lane"), 6},
        {QStringLiteral("text"), QStringLiteral("x...")},
    };
    modelError.clear();
    const bool acceptedLastBeatLane =
        jam2::application::validateControlMessage(validBeatHit, modelError);
    validBeatHit[QStringLiteral("lane")] = 7;
    const bool rejectedRemovedBeatLane =
        !jam2::application::validateControlMessage(validBeatHit, modelError);
    record(QStringLiteral("model.beat-hit-uses-seven-lane-bound"),
        acceptedLastBeatLane && rejectedRemovedBeatLane, modelError);
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
        QJsonObject complexityMessage = collaborativeSong;
        QJsonObject song = complexityMessage.value(QStringLiteral("song")).toObject();
        QJsonArray sections = song.value(QStringLiteral("sections")).toArray();
        QJsonObject section = sections.first().toObject();
        section[QStringLiteral("generated_harmonic_complexity")] = 8;
        section[QStringLiteral("generated_rhythmic_complexity")] = 1;
        sections[0] = section;
        song[QStringLiteral("sections")] = sections;
        complexityMessage[QStringLiteral("song")] = song;
        modelError.clear();
        const bool acceptedComplexity =
            jam2::application::validateControlMessage(complexityMessage, modelError);
        section[QStringLiteral("generated_harmonic_complexity")] = 9;
        sections[0] = section;
        song[QStringLiteral("sections")] = sections;
        complexityMessage[QStringLiteral("song")] = song;
        modelError.clear();
        const bool rejectedHighComplexity =
            !jam2::application::validateControlMessage(complexityMessage, modelError);
        section[QStringLiteral("generated_harmonic_complexity")] = 4.5;
        sections[0] = section;
        song[QStringLiteral("sections")] = sections;
        complexityMessage[QStringLiteral("song")] = song;
        modelError.clear();
        const bool rejectedFractionalComplexity =
            !jam2::application::validateControlMessage(complexityMessage, modelError);
        record(QStringLiteral("model.generated-complexity-is-bounded"),
            acceptedComplexity && rejectedHighComplexity &&
            rejectedFractionalComplexity, modelError);
    }
    {
        const SharedTrackModel defaults;
        record(QStringLiteral("track.defaults-to-minus-ten-db-and-metronome-sync"),
            defaults.trackGainDb == -10.0 && defaults.syncMetronome);
    }
    {
        const QStringList advancedSymbols{
            QStringLiteral("C9"),
            QStringLiteral("D13"),
            QStringLiteral("E7b9"),
            QStringLiteral("F7#9"),
            QStringLiteral("Gmaj9"),
            QStringLiteral("Am9"),
            QStringLiteral("Balt"),
            QStringLiteral("C#11"),
            QStringLiteral("Dmaj7#11"),
            QStringLiteral("Emaj9#11"),
            QStringLiteral("Fmaj9/A"),
            QStringLiteral("G7b9/Db"),
        };
        bool advancedSymbolsValid = true;
        for (const QString& symbol : advancedSymbols) {
            advancedSymbolsValid =
                advancedSymbolsValid && jam2::practice::parseChord(symbol).valid;
        }
        const jam2::practice::ParsedChord slash =
            jam2::practice::parseChord(QStringLiteral("Cmaj9/G"));
        record(QStringLiteral("practice.advanced-and-slash-chords-parse"),
            advancedSymbolsValid && slash.valid && slash.root == 0 && slash.bass == 7 &&
            slash.suffix == QStringLiteral("maj9") && slash.intervals.size() == 5 &&
            jam2::practice::chordToneNames(QStringLiteral("Cmaj9/G")).startsWith(
                QStringLiteral("G ")) &&
            !jam2::practice::parseChord(QStringLiteral("Cmaj9/")).valid &&
            !jam2::practice::parseChord(QStringLiteral("C/G/B")).valid &&
            !jam2::practice::parseChord(QStringLiteral("Cmaj9/H")).valid);
    }
    {
        SongSection section;
        section.beats = 4;
        section.chords = {
            QStringLiteral("Cmaj9/G"),
            QStringLiteral("D13/F#"),
            QStringLiteral("F7#9/Ab"),
            QStringLiteral("Galt/Db"),
        };
        section.targets.resize(section.beats);
        jam2::practice::ReferenceRenderSettings settings;
        settings.renderChords = true;
        settings.renderDrums = false;
        settings.renderMelody = false;
        settings.sampleRate = 8000;
        settings.bpm = 240.0;
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/advanced-chord-reference-test-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const jam2::practice::ReferenceRenderResult rendered =
            jam2::practice::renderPracticeReferences(
                &section, nullptr, settings, workspace);
        const jam2::wav::InspectResult wav = rendered.chords.path.isEmpty()
            ? jam2::wav::InspectResult{}
            : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.chords.path), 1024ULL * 1024ULL);
        record(QStringLiteral("practice.advanced-slash-chords-render"),
            rendered.error.isEmpty() && wav &&
            rendered.chords.frames == 8000 &&
            pcm16WavHasSignal(rendered.chords.path, wav.info),
            rendered.error);
        (void)QDir(workspace).removeRecursively();
    }
    {
        const int styleCount = jam2::practice::chordStyleNames().size();
        QVector<int> styleBpm(styleCount, -1);
        QVector<int> simpleBeatHits(styleCount, 0);
        QVector<int> expertBeatHits(styleCount, 0);
        bool matrixValid = true;
        bool groundedValid = true;
        bool expertFeaturesPresent = true;
        bool allLevelsResolveHome = true;
        bool advancedResolutionPresent = true;
        bool advancedDensityBounded = true;
        bool dominantMelodyUsesGuideTones = true;
        for (int style = 0; style < styleCount; ++style) {
            for (int level = 1; level <= 8; ++level) {
                jam2::practice::ChordIdeaRequest request;
                request.key = 0;
                request.style = style;
                request.character = jam2::practice::chordCharacters(style).constFirst();
                request.bars = 8;
                request.beatsPerBar = 4;
                request.harmonicComplexity = level;
                request.rhythmicComplexity = level;
                const jam2::practice::GeneratedPracticeIdea idea =
                    jam2::practice::generateCoupledPracticeIdeaForTest(
                        request, static_cast<std::uint32_t>(700 + style));
                matrixValid = matrixValid &&
                    idea.chordSection.generatedStyle ==
                        jam2::practice::chordStyleNames().at(style) &&
                    idea.chordSection.generatedCharacter == request.character &&
                    idea.chordSection.generatedHarmonicComplexity == level &&
                    idea.chordSection.generatedRhythmicComplexity == level &&
                    idea.beatSection.generatedHarmonicComplexity == level &&
                    idea.beatSection.generatedRhythmicComplexity == level &&
                    idea.chordSection.beats == idea.beatSection.beats &&
                    idea.chordSection.beats == request.bars * request.beatsPerBar;
                if (styleBpm[style] < 0) styleBpm[style] = idea.bpm;
                matrixValid = matrixValid && styleBpm[style] == idea.bpm;

                int firstRoot = -1;
                int lastRoot = -1;
                bool hasExpertChord = false;
                QVector<QPair<int, jam2::practice::ParsedChord>> harmonicEvents;
                for (int beat = 0; beat < idea.chordSection.beats; ++beat) {
                    const QString symbol = idea.chordSection.chords.value(beat).trimmed();
                    if (symbol.isEmpty()) continue;
                    const jam2::practice::ParsedChord chord =
                        jam2::practice::parseChord(symbol);
                    matrixValid = matrixValid && chord.valid;
                    if (!chord.valid) continue;
                    if (firstRoot < 0) firstRoot = chord.root;
                    lastRoot = chord.root;
                    harmonicEvents.push_back({beat, chord});
                    hasExpertChord = hasExpertChord || chord.intervals.size() >= 4;
                    if (level == 1) {
                        groundedValid = groundedValid &&
                            beat % request.beatsPerBar == 0 &&
                            chord.intervals.size() <= 3;
                    }
                }
                if (level == 1) {
                    groundedValid = groundedValid &&
                        firstRoot >= 0 && firstRoot == lastRoot;
                }
                allLevelsResolveHome = allLevelsResolveHome &&
                    firstRoot >= 0 && lastRoot == request.key;
                if (level == 8) {
                    expertFeaturesPresent = expertFeaturesPresent && hasExpertChord;
                }
                if (style != static_cast<int>(jam2::practice::ChordStyle::ModernMetal)) {
                    advancedDensityBounded = advancedDensityBounded &&
                        harmonicEvents.size() <= request.bars * 2 + 2;
                }
                if (level >= 5 &&
                    (style == static_cast<int>(jam2::practice::ChordStyle::FunctionalPop) ||
                     style == static_cast<int>(jam2::practice::ChordStyle::JazzTurnaround))) {
                    bool foundResolvedDominant = false;
                    for (int event = 0; event + 1 < harmonicEvents.size(); ++event) {
                        const jam2::practice::ParsedChord& chord =
                            harmonicEvents.at(event).second;
                        const QString suffix = chord.suffix.toLower();
                        const bool dominantQuality =
                            suffix == QStringLiteral("7") ||
                            suffix == QStringLiteral("9") ||
                            suffix == QStringLiteral("13") ||
                            suffix == QStringLiteral("7b9") ||
                            suffix == QStringLiteral("7#9") ||
                            suffix == QStringLiteral("alt");
                        if (!dominantQuality) continue;
                        const int movement =
                            (harmonicEvents.at(event + 1).second.root - chord.root + 12) % 12;
                        if (movement == 5 || movement == 11 || movement == 2) {
                            foundResolvedDominant = true;
                        }
                        const QString melody =
                            idea.chordSection.targets.value(harmonicEvents.at(event).first);
                        const std::optional<int> midi =
                            jam2::practice::parseMidiNote(melody);
                        if (midi) {
                            const int relative = ((*midi % 12) - chord.root + 12) % 12;
                            dominantMelodyUsesGuideTones =
                                dominantMelodyUsesGuideTones &&
                                (relative == 4 || relative == 10);
                        } else {
                            dominantMelodyUsesGuideTones = false;
                        }
                    }
                    advancedResolutionPresent =
                        advancedResolutionPresent && foundResolvedDominant;
                }
                for (const QString& target : idea.chordSection.targets) {
                    matrixValid = matrixValid &&
                        (target.trimmed().isEmpty() ||
                         target == QStringLiteral("-") ||
                         jam2::practice::parseMidiNote(target).has_value());
                }

                int hitCount = 0;
                for (const BeatPattern& pattern : idea.beatSection.beatPatterns) {
                    matrixValid = matrixValid &&
                        BeatGridModel::beatDivisionValues().contains(pattern.division);
                    for (const QString& lane : pattern.lanes) {
                        if (lane.isEmpty()) continue;
                        matrixValid = matrixValid && lane.size() == pattern.division;
                        for (const QChar state : lane) {
                            matrixValid = matrixValid &&
                                (state == QLatin1Char('.') || state == QLatin1Char('x') ||
                                 state == QLatin1Char('a') || state == QLatin1Char('g'));
                            if (state != QLatin1Char('.')) ++hitCount;
                        }
                    }
                }
                if (level == 1) simpleBeatHits[style] = hitCount;
                if (level == 8) expertBeatHits[style] = hitCount;
            }
        }
        bool rhythmScalesIndependently = true;
        for (int style = 0; style < styleCount; ++style) {
            rhythmScalesIndependently = rhythmScalesIndependently &&
                expertBeatHits.at(style) > simpleBeatHits.at(style);
        }
        record(QStringLiteral("practice.complexity-matrix-preserves-style-and-tempo"),
            matrixValid);
        record(QStringLiteral("practice.level-one-is-grounded-and-resolved"),
            groundedValid);
        record(QStringLiteral("practice.level-eight-is-style-native-and-advanced"),
            expertFeaturesPresent && rhythmScalesIndependently);
        record(QStringLiteral("practice.all-complexities-return-home-with-bounded-colour"),
            allLevelsResolveHome && advancedDensityBounded);
        record(QStringLiteral("practice.advanced-dominants-resolve-with-melodic-guide-tones"),
            advancedResolutionPresent && dominantMelodyUsesGuideTones);
    }
    {
        BeatGridModel model;
        jam2::practice::ChordIdeaRequest request;
        request.key = 4;
        request.style = static_cast<int>(jam2::practice::ChordStyle::ModernMetal);
        request.character = QStringLiteral("Chugging");
        request.bars = 4;
        request.beatsPerBar = 4;
        const SongSection first = jam2::practice::generateChordIdeaForTest(request, 17);
        const int firstIndex = model.replaceGeneratedSection(QStringLiteral("chord"), first);
        const QString firstId = model.section(firstIndex).id;
        const SongSection second = jam2::practice::generateChordIdeaForTest(request, 23);
        const int secondIndex = model.replaceGeneratedSection(QStringLiteral("chord"), second);
        record(QStringLiteral("practice.generation-replaces-pristine-eight-bar-placeholder"),
            firstIndex == 0 && firstIndex == secondIndex &&
            model.sections().size() == 1 &&
            model.section(secondIndex).id == firstId &&
            model.section(secondIndex).generatedKind == QStringLiteral("chord"));
        BeatGridModel manualModel;
        manualModel.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("C"));
        const int appendedIndex = manualModel.replaceGeneratedSection(QStringLiteral("chord"), first);
        record(QStringLiteral("practice.generation-preserves-manual-section"),
            appendedIndex == 1 && manualModel.sections().size() == 2 &&
            manualModel.section(0).generatedKind.isEmpty() &&
            manualModel.section(0).chords.value(0) == QStringLiteral("C"));
        BeatGridModel complexityRoundTrip;
        const bool complexityLoaded = complexityRoundTrip.loadJson(model.toJson());
        record(QStringLiteral("practice.complexity-metadata-roundtrip"),
            complexityLoaded &&
            complexityRoundTrip.section(secondIndex).generatedHarmonicComplexity == 4 &&
            complexityRoundTrip.section(secondIndex).generatedRhythmicComplexity == 4);
        bool melodyValid = true;
        bool chordChangesUseChordTones = true;
        int previousMelody = -1;
        int maximumMelodyLeap = 0;
        for (int beatIndex = 0; beatIndex < model.section(secondIndex).beats; ++beatIndex) {
            const QString melody = model.section(secondIndex).targets.value(beatIndex).trimmed();
            if (melody.isEmpty() || melody == QStringLiteral("-")) continue;
            const std::optional<int> midi = jam2::practice::parseMidiNote(melody);
            melodyValid = melodyValid && midi.has_value();
            if (!midi) continue;
            if (previousMelody >= 0) {
                maximumMelodyLeap = qMax(maximumMelodyLeap, std::abs(*midi - previousMelody));
            }
            previousMelody = *midi;
            const QString chordSymbol = model.section(secondIndex).chords.value(beatIndex).trimmed();
            if (!chordSymbol.isEmpty() && chordSymbol != QStringLiteral("-")) {
                const jam2::practice::ParsedChord parsed = jam2::practice::parseChord(chordSymbol);
                const int relative = ((*midi % 12) - parsed.root + 12) % 12;
                chordChangesUseChordTones = chordChangesUseChordTones &&
                    parsed.intervals.contains(relative);
            }
        }
        record(QStringLiteral("practice.melody-and-chord-symbols-are-coherent"),
            model.section(secondIndex).targets.size() == model.section(secondIndex).beats &&
            std::any_of(model.section(secondIndex).targets.cbegin(), model.section(secondIndex).targets.cend(),
                [](const QString& value) {
                    return jam2::practice::parseMidiNote(value).has_value();
                }) &&
            std::all_of(model.section(secondIndex).targets.cbegin(), model.section(secondIndex).targets.cend(),
                [](const QString& value) {
                    return value.trimmed().isEmpty() ||
                        value == QStringLiteral("-") ||
                        jam2::practice::parseMidiNote(value).has_value();
                }) &&
            melodyValid && chordChangesUseChordTones && maximumMelodyLeap <= 6 &&
            std::all_of(model.section(secondIndex).chords.cbegin(), model.section(secondIndex).chords.cend(),
                [](const QString& chord) {
                    return chord.isEmpty() || jam2::practice::parseChord(chord).valid;
                }));
        jam2::practice::ChordIdeaRequest constrainedRandom;
        constrainedRandom.character = QStringLiteral("Phrygian");
        constrainedRandom.bars = 1;
        constrainedRandom.beatsPerBar = 4;
        const SongSection modalMetal =
            jam2::practice::generateChordIdeaForTest(constrainedRandom, 41);
        record(QStringLiteral("practice.random-style-honors-selected-character"),
            modalMetal.generatedStyle == QStringLiteral("Modern Metal") &&
            modalMetal.generatedCharacter == QStringLiteral("Phrygian") &&
            modalMetal.generatedBars == 4);
        const jam2::practice::GeneratedPracticeIdea coupled =
            jam2::practice::generateCoupledPracticeIdeaForTest(request, 29);
        record(QStringLiteral("practice.coupled-generation-matches-style-length-and-click"),
            coupled.chordSection.beats == coupled.beatSection.beats &&
            coupled.beatSection.generatedStyle == QStringLiteral("Modern Metal") &&
            coupled.bpm >= 110 && coupled.bpm <= 180 &&
            coupled.clickDivision == 4 &&
            coupled.clickEnabled.size() == request.beatsPerBar * coupled.clickDivision &&
            coupled.clickAccents.size() == coupled.clickEnabled.size());

        const QJsonObject currentSong = model.toJson();
        BeatGridModel roundTrip;
        record(QStringLiteral("practice.song-current-shape-needs-no-version-field"),
            !currentSong.contains(QStringLiteral("format")) &&
            !currentSong.contains(QStringLiteral("lyrics_text")) &&
            roundTrip.loadJson(currentSong) &&
            !roundTrip.toJson().contains(QStringLiteral("format")));
    }
    {
        jam2::practice::BeatIdeaRequest request;
        request.style = static_cast<int>(jam2::practice::BeatStyle::ModernMetal);
        request.character = QStringLiteral("Chugging");
        request.bars = 32;
        request.beatsPerBar = 4;
        const SongSection beat = jam2::practice::generateBeatIdeaForTest(request, 5);
        const int guitar = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Guitar"));
        record(QStringLiteral("practice.metal-beat-includes-guitar-visual-lane"),
            beat.beatPatterns.size() == beat.beats && guitar >= 0 &&
            std::any_of(beat.beatPatterns.cbegin(), beat.beatPatterns.cend(),
                [guitar](const BeatPattern& pattern) {
                    return pattern.lanes.value(guitar).contains(QLatin1Char('x'));
                }));
        record(QStringLiteral("practice.beat-view-exposes-only-useful-lanes"),
            BeatGridModel::beatLaneNames() == QStringList{
                QStringLiteral("Kick"),
                QStringLiteral("Snare"),
                QStringLiteral("Closed HH"),
                QStringLiteral("Open HH"),
                QStringLiteral("Crash"),
                QStringLiteral("Tom"),
                QStringLiteral("Guitar"),
            });
        QJsonObject invalidSong = BeatGridModel{}.toJson();
        QJsonArray invalidSections = invalidSong.value(QStringLiteral("sections")).toArray();
        QJsonObject invalidSection = invalidSections.first().toObject();
        QJsonArray invalidPatterns = invalidSection.value(QStringLiteral("beat_patterns")).toArray();
        QJsonObject invalidPattern = invalidPatterns.first().toObject();
        invalidPattern[QStringLiteral("lanes")] = QJsonArray{
            QStringLiteral("kick"), QStringLiteral("snare"), QStringLiteral("closed"),
            QStringLiteral("open"), QStringLiteral("crash"), QStringLiteral("splash"),
            QStringLiteral("cymbal"), QStringLiteral("tom"), QStringLiteral("special"),
            QStringLiteral("guitar"), QStringLiteral("bass"),
        };
        invalidPatterns[0] = invalidPattern;
        invalidSection[QStringLiteral("beat_patterns")] = invalidPatterns;
        invalidSections[0] = invalidSection;
        invalidSong[QStringLiteral("sections")] = invalidSections;
        BeatGridModel rejectedOldShape;
        record(QStringLiteral("practice.song-rejects-non-current-lane-shape"),
            !rejectedOldShape.loadJson(invalidSong));
        const int tom = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Tom"));
        const int fillBeats = static_cast<int>(std::count_if(
            beat.beatPatterns.cbegin(), beat.beatPatterns.cend(),
            [tom](const BeatPattern& pattern) {
                return tom >= 0 && !pattern.lanes.value(tom).trimmed().isEmpty();
            }));
        record(QStringLiteral("practice.long-beat-fills-are-phrase-aware-and-sparse"),
            beat.generatedBars == 16 &&
            fillBeats > 0 && fillBeats <= beat.generatedBars / 2 &&
            !beat.beatPatterns.constLast().lanes.value(tom).trimmed().isEmpty());
        record(QStringLiteral("track.timeline-labels-every-beat"),
            jam2::gui::trackTimelineBeatNumber(0) == 1 &&
            jam2::gui::trackTimelineBeatNumber(15) == 16);
    }
    {
        jam2::practice::ChordIdeaRequest request;
        request.key = 0;
        request.style = static_cast<int>(jam2::practice::ChordStyle::FunctionalPop);
        request.character = QStringLiteral("Bright");
        request.bars = 32;
        request.beatsPerBar = 4;
        BeatGridModel chordModel;
        BeatGridModel beatModel;
        const std::optional<jam2::practice::GeneratedPracticeIdea> idea =
            jam2::practice::PracticeIdeaController::generateCoupled(
                chordModel, beatModel, request);
        BeatGridModel serializedChordModel;
        BeatGridModel serializedBeatModel;
        const bool modelRoundTrip = idea &&
            serializedChordModel.loadJson(chordModel.toJson()) &&
            serializedBeatModel.loadJson(beatModel.toJson());
        const std::optional<SongSection> chord =
            jam2::practice::PracticeIdeaController::generatedSection(
                serializedChordModel, QStringLiteral("chord"));
        const std::optional<SongSection> beat =
            jam2::practice::PracticeIdeaController::generatedSection(
                serializedBeatModel, QStringLiteral("beat"));
        jam2::practice::ReferenceRenderSettings settings;
        settings.sampleRate = 8000;
        settings.bpm = idea ? idea->bpm : 120.0;
        settings.renderMelody = true;
        const qint64 expectedFrames = idea ? static_cast<qint64>(std::ceil(
            static_cast<double>(idea->chordSection.beats) * 60.0 /
            static_cast<double>(idea->bpm) * settings.sampleRate)) : 0;
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/practice-reference-test-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const bool workspaceReady = QDir().mkpath(workspace);
        const jam2::practice::ReferenceRenderResult rendered =
            jam2::practice::renderPracticeReferences(
                chord ? &*chord : nullptr, beat ? &*beat : nullptr, settings, workspace);
        const jam2::wav::InspectResult chordWav = rendered.chords.path.isEmpty()
            ? jam2::wav::InspectResult{} : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.chords.path), 4ULL * 1024ULL * 1024ULL);
        const jam2::wav::InspectResult drumWav = rendered.drums.path.isEmpty()
            ? jam2::wav::InspectResult{} : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.drums.path), 4ULL * 1024ULL * 1024ULL);
        const jam2::wav::InspectResult melodyWav = rendered.melody.path.isEmpty()
            ? jam2::wav::InspectResult{} : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.melody.path), 4ULL * 1024ULL * 1024ULL);
        LooperProject preparedProject;
        QString applyError;
        const bool referencesApplied =
            jam2::practice::PracticeIdeaController::applyReferences(
                preparedProject, 0, settings, rendered, applyError);
        const QString preparedPath = PreparedMixRenderer::outputPath(workspace, 0, 1);
        const QString nextPreparedPath = PreparedMixRenderer::outputPath(workspace, 0, 2);
        const PreparedMixResult prepared = referencesApplied
            ? PreparedMixRenderer::render(
                preparedProject, workspace, settings.sampleRate, preparedPath, SharedTrackModel{})
            : PreparedMixResult{};
        record(QStringLiteral("practice.coupled-generation-clamps-to-16-bar-maximum"),
            modelRoundTrip && chord && beat && workspaceReady && rendered.error.isEmpty() &&
            chord->generatedBars == 16 && beat->generatedBars == 16 &&
            chord->beats == 64 && beat->beats == 64 &&
            rendered.chords.frames == expectedFrames &&
            rendered.chords.frames == rendered.drums.frames &&
            rendered.chords.frames == rendered.melody.frames &&
            chordWav && drumWav && melodyWav &&
            chordWav.info.sample_rate == 8000 && drumWav.info.sample_rate == 8000 &&
            melodyWav.info.sample_rate == 8000 &&
            chordWav.info.channels == 1 && drumWav.info.channels == 1 &&
            melodyWav.info.channels == 1 &&
            pcm16WavHasSignal(rendered.chords.path, chordWav.info) &&
            pcm16WavHasSignal(rendered.drums.path, drumWav.info) &&
            pcm16WavHasSignal(rendered.melody.path, melodyWav.info),
            rendered.error);
        record(QStringLiteral("practice.replacement-prepared-mix-has-new-path-and-full-duration"),
            referencesApplied && applyError.isEmpty() &&
            prepared.error.isEmpty() &&
            prepared.path == preparedPath &&
            preparedPath != nextPreparedPath &&
            QDir::cleanPath(preparedPath).startsWith(QDir::cleanPath(workspace)) &&
            prepared.frames == expectedFrames,
            prepared.error.isEmpty() ? applyError : prepared.error);
        (void)QDir(workspace).removeRecursively();
    }
    {
        LooperProject project;
        LooperLane lane;
        lane.name = QStringLiteral("Practice Chords");
        lane.referenceKind = QStringLiteral("chord");
        lane.referenceSourceSignature = QString(64, QLatin1Char('a'));
        lane.referenceBpm = 137.0;
        lane.referenceStale = true;
        const bool appended = project.appendLane(0, lane);
        LooperProject loaded;
        const bool loadedOk = loaded.loadJson(project.toJson());
        record(QStringLiteral("practice.reference-metadata-roundtrip"),
            appended && loadedOk && loaded.banks().at(0).lanes.at(0).referenceKind == QStringLiteral("chord") &&
            loaded.banks().at(0).lanes.at(0).referenceBpm == 137.0 &&
            loaded.banks().at(0).lanes.at(0).referenceStale);
    }
    {
        LooperProject project;
        for (int index = 0; index < 2; ++index) {
            LooperLane lane;
            lane.name = QStringLiteral("Old Practice Chords");
            lane.referenceKind = QStringLiteral("chord");
            lane.referenceSourceSignature = QString(64, QLatin1Char('b'));
            (void)project.appendLane(0, lane);
        }
        jam2::practice::ReferenceRenderSettings settings;
        settings.renderChords = true;
        settings.renderDrums = false;
        settings.renderMelody = true;
        jam2::practice::ReferenceRenderResult result;
        // Metadata-only fixture paths: applyReferences does not access files.
        result.chords = {
            QStringLiteral("test-fixtures/reference.wav"), QString(64, QLatin1Char('c')), 48000};
        result.melody = {
            QStringLiteral("test-fixtures/melody.wav"), QString(64, QLatin1Char('e')), 48000};
        result.sourceSignature = QString(64, QLatin1Char('d'));
        QString applyError;
        const bool applied = jam2::practice::PracticeIdeaController::applyReferences(
            project, 0, settings, result, applyError);
        const int managedCount = static_cast<int>(std::count_if(
            project.banks().at(0).lanes.cbegin(), project.banks().at(0).lanes.cend(),
            [](const LooperLane& lane) { return lane.referenceKind == QStringLiteral("chord"); }));
        const int melodyCount = static_cast<int>(std::count_if(
            project.banks().at(0).lanes.cbegin(), project.banks().at(0).lanes.cend(),
            [](const LooperLane& lane) { return lane.referenceKind == QStringLiteral("melody"); }));
        record(QStringLiteral("practice.reference-apply-manages-chord-and-melody-lanes"),
            applied && applyError.isEmpty() && managedCount == 1 &&
            melodyCount == 1 &&
            std::any_of(project.banks().at(0).lanes.cbegin(), project.banks().at(0).lanes.cend(),
                [&result](const LooperLane& lane) {
                    return lane.referenceKind == QStringLiteral("melody") &&
                        lane.assetHash == result.melody.sha256;
                }),
            applyError);
    }
    {
        LooperProject project;
        LooperLane generatedLane;
        generatedLane.name = QStringLiteral("Practice Drums");
        LooperLane manualLane;
        manualLane.name = QStringLiteral("Manual take");
        const bool generatedApplied = project.appendLane(0, generatedLane);
        const bool manualApplied = project.appendLane(0, manualLane);

        jam2::practice::PracticeIdeaController::clearReferences(project);

        record(QStringLiteral("practice.new-idea-clears-only-generated-reference-lanes"),
            generatedApplied && manualApplied &&
            project.banks().at(0).lanes.size() == 1 &&
            project.banks().at(0).lanes.front().referenceKind.isEmpty() &&
            project.banks().at(0).lanes.front().name == QStringLiteral("Manual take"));
    }
    {
        const QString root = QDir::current().absoluteFilePath(
            QStringLiteral("build/project-persistence-test-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString workspace = QDir(root).absoluteFilePath(QStringLiteral("staging"));
        const QString transientPath =
            QDir(workspace).absoluteFilePath(QStringLiteral("wavs/generated.wav"));
        const QString obsoletePath =
            QDir(workspace).absoluteFilePath(QStringLiteral("wavs/obsolete.wav"));
        const QString savedPath =
            QDir(root).absoluteFilePath(QStringLiteral("project/wavs/generated.wav"));
        const bool foldersReady =
            QDir().mkpath(QFileInfo(transientPath).absolutePath()) &&
            QDir().mkpath(QFileInfo(savedPath).absolutePath());
        auto writeFixture = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) &&
                file.write("fixture", 7) == 7;
        };
        const bool filesReady = foldersReady &&
            writeFixture(transientPath) && writeFixture(obsoletePath) &&
            writeFixture(savedPath);
        ProjectPersistenceCoordinator persistence;
        persistence.initializeWorkspace(workspace);
        persistence.registerTransientWav(transientPath);
        persistence.registerTransientWav(obsoletePath);
        const bool obsoleteDiscarded = persistence.discardTransientWav(obsoletePath);
        persistence.acceptSavedProject(
            QDir(root).absoluteFilePath(QStringLiteral("project/song.jam2song")),
            QByteArrayLiteral("snapshot"),
            QSet<QString>{savedPath});
        QThreadPool cleanupPool;
        cleanupPool.setMaxThreadCount(1);
        persistence.scheduleTransientCleanup(cleanupPool);
        cleanupPool.waitForDone();
        record(QStringLiteral("project.temporary-wavs-cleaned-and-saved-copy-retained"),
            filesReady && obsoleteDiscarded &&
            !QFileInfo::exists(obsoletePath) &&
            !QFileInfo::exists(transientPath) && QFileInfo::exists(savedPath));
        (void)QDir(root).removeRecursively();
    }
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
    {
        LooperProject project;
        LooperLane reference;
        reference.id = QStringLiteral("local-reference");
        reference.assetPath = QStringLiteral("C:/fixtures/practice-chords.wav");
        reference.assetHash = QString(64, QLatin1Char('d'));
        reference.name = QStringLiteral("Practice Chords");
        reference.sampleRate = 48000;
        reference.referenceKind = QStringLiteral("chords");
        reference.localOnly = true;
        const bool appended = project.appendLane(0, std::move(reference));
        const QJsonObject saved = project.toJson();
        const QJsonObject synced = project.toJson(true);
        LooperProject loaded;
        const bool roundTripped = loaded.loadJson(saved);
        QJsonObject incoming = BeatGridModel{}.toJson();
        incoming.insert(QStringLiteral("looper"), LooperProject{}.toJson());
        const int preserved = mergeQuarantinedLocalLanes(incoming, project, 48000);
        LooperProject merged;
        const bool mergedLoaded =
            merged.loadJson(incoming.value(QStringLiteral("looper")).toObject());
        const QJsonArray savedLanes = saved.value(QStringLiteral("banks")).toArray()
            .at(0).toObject().value(QStringLiteral("lanes")).toArray();
        const QJsonArray syncedLanes = synced.value(QStringLiteral("banks")).toArray()
            .at(0).toObject().value(QStringLiteral("lanes")).toArray();
        record(QStringLiteral("practice-reference.local-only-roundtrip-and-sync-exclusion"),
            appended && roundTripped && savedLanes.size() == 1 &&
            savedLanes.at(0).toObject().value(QStringLiteral("local_only")).toBool() &&
            loaded.banks().at(0).lanes.size() == 1 &&
            loaded.banks().at(0).lanes.at(0).localOnly &&
            syncedLanes.isEmpty() && preserved == 1 && mergedLoaded &&
            merged.banks().at(0).lanes.size() == 1 &&
            merged.banks().at(0).lanes.at(0).localOnly);
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
        jam2::gui::looperTimelineViewFrames(48000, 384000, 96000, -1, -1) == 384000 &&
        jam2::gui::looperTimelineViewFrames(48000, 384000, 480000, -1, -1) == 480000 &&
        jam2::gui::looperTimelineViewFrames(48000, 384000, 96000, -1, 12000) == 576000);

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
    {
        TapTempoTracker tap;
        const std::optional<int> first = tap.tap(0);
        const std::optional<int> second = tap.tap(500);
        const std::optional<int> settled = tap.tap(1010);
        const std::optional<int> reset = tap.tap(3500);
        const std::optional<int> afterReset = tap.tap(4000);
        record(QStringLiteral("metronome.tap-tempo-median-and-pause-reset"),
            !first && second == 120 && settled == 119 &&
            !reset && afterReset == 120);
    }
    {
        const jam2::metronome::PatternSnapshot pattern =
            jam2::metronome::sanitize({});
        const double normalEarly = jam2::metronome::render_sample(
            pattern, 24, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal);
        const double countInEarly = jam2::metronome::render_sample(
            pattern, 24, 48000.0, 0.5,
            jam2::metronome::ClickVoice::CountIn);
        const double normalTail = jam2::metronome::render_sample(
            pattern, 480, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal);
        const double countInTail = jam2::metronome::render_sample(
            pattern, 480, 48000.0, 0.5,
            jam2::metronome::ClickVoice::CountIn);
        record(QStringLiteral("record-count-in.uses-distinct-lower-longer-click"),
            std::abs(normalEarly - countInEarly) > 0.01 &&
            normalTail == 0.0 &&
            std::abs(countInTail) > 0.01);
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
    record(QStringLiteral("record-count-in.starts-when-metronome-becomes-ready"),
        jam2::gui::recording_grid_ready_for_count_in(true, true) &&
        !jam2::gui::recording_grid_ready_for_count_in(false, true) &&
        !jam2::gui::recording_grid_ready_for_count_in(true, false));
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
        ApplicationRuntime runtime;
        TrackWorkspaceController workspace(runtime);
        workspace.playPreparedMixWhenReady = true;
        workspace.pendingPreparedTrackReadyRevision = 12;
        workspace.pendingSharedTrackRevision = 12;
        workspace.pendingSharedTrackHostReady = false;
        workspace.pendingSharedTrackReadyTokens.insert(QStringLiteral("peer"));
        workspace.publishStoppedTrackStateWhenApplied = true;
        workspace.pendingSongTrackRestart = true;
        workspace.trackController.requestPlayback(true, 12);

        workspace.cancelPendingTrackPlayback();

        record(QStringLiteral("practice.generate-cancels-track-playback-and-restarts"),
            !workspace.playPreparedMixWhenReady &&
            workspace.pendingPreparedTrackReadyRevision == 0 &&
            workspace.pendingSharedTrackRevision == 0 &&
            workspace.pendingSharedTrackHostReady &&
            workspace.pendingSharedTrackReadyTokens.isEmpty() &&
            !workspace.publishStoppedTrackStateWhenApplied &&
            !workspace.pendingSongTrackRestart &&
            !workspace.trackController.playback().requestedPlaying);
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
        Jam2RuntimeHost peerGains;
        const bool rejectsInvalid =
            !peerGains.submitPeerGain(0, 1000000) &&
            !peerGains.submitPeerGain(1, -1) &&
            !peerGains.submitPeerGain(1, 4000001);
        const bool accepted =
            peerGains.submitPeerGain(11, 500000) &&
            peerGains.submitPeerGain(11, 750000) &&
            peerGains.submitPeerGain(22, 1250000);
        const std::vector<Jam2PeerGainUpdate> updates = peerGains.takePeerGains();
        const auto peer11 = std::find_if(
            updates.cbegin(), updates.cend(),
            [](const Jam2PeerGainUpdate& update) { return update.peer_id == 11; });
        const auto peer22 = std::find_if(
            updates.cbegin(), updates.cend(),
            [](const Jam2PeerGainUpdate& update) { return update.peer_id == 22; });
        record(QStringLiteral("peer-gain.latest-value-coalesces-per-peer"),
            rejectsInvalid && accepted && updates.size() == 2 &&
            peer11 != updates.cend() && peer11->gain_ppm == 750000 &&
            peer22 != updates.cend() && peer22->gain_ppm == 1250000 &&
            peerGains.takePeerGains().empty());
    }
    {
        jam2::audio::PreparedTrackSource source(4);
        const int slot = source.claimLoadingSlot();
        std::int16_t* samples = source.loadingData(slot);
        if (samples != nullptr) {
            samples[0] = 32767;
            samples[1] = -16384;
            samples[2] = 8192;
            samples[3] = 0;
        }
        const bool published = samples != nullptr && source.publishReady(slot, 4, 48000);
        const std::array<jam2::audio::PreparedTrackSource::Command, 2> commands{{
            {jam2::audio::PreparedTrackSource::CommandType::Swap,
             static_cast<std::uint32_t>(qMax(0, slot)), 0, 0, 0, 1000000},
            {jam2::audio::PreparedTrackSource::CommandType::Play, 0, 0, 0, 0, 1000000},
        }};
        const bool queued = published && source.enqueueBatch(commands);
        std::array<std::int32_t, 4> output{};
        const int peakPpm = queued ? source.mix(output.data(), output.size(), 0) : 0;
        record(QStringLiteral("prepared-track.mix-reports-contribution-peak"),
            queued && peakPpm >= 999900 && peakPpm <= 1000000 &&
            output[0] > 0 && output[1] < 0);
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
        bool pitchDetected = false;
        bool pitchDisabled = false;
        bool pitchStoppedProcessing = false;
        jam2::EnginePitchSnapshot lastPitch;
        long lastPitchCallbacks = 0;
        std::uint64_t lastPitchEngineFrame = 0;
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
            jam2::EngineCommand enablePitch;
            enablePitch.type = jam2::EngineCommandType::SetPitchAnalysisEnabled;
            enablePitch.enabled = true;
            if (!engine.submit(enablePitch)) {
                throw std::runtime_error("pitch-analysis enable command queue unavailable");
            }

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
                const std::uint64_t exerciseDeadline = jam2::monotonic_us() + 250000ULL;
                while (jam2::monotonic_us() < exerciseDeadline) {
                    const jam2::CapturedAudioBlock captured = engine.popNetworkCapture(attachment, packet);
                    if (captured.frames == packet.size()) {
                        fullBlock = true;
                    } else if (captured.frames != 0) {
                        partialBlock = true;
                    }
                    const jam2::EnginePitchSnapshot pitch = engine.snapshot().pitch;
                    if (pitch.enabled &&
                        pitch.valid &&
                        pitch.midi_note == 69 &&
                        std::abs(pitch.cents) <= 5.0) {
                        pitchDetected = true;
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
            const std::uint64_t pitchDeadline = jam2::monotonic_us() + 2000000ULL;
            while (jam2::monotonic_us() < pitchDeadline) {
                const jam2::EngineSnapshot snapshot = engine.snapshot();
                lastPitch = snapshot.pitch;
                lastPitchCallbacks = snapshot.callbacks;
                lastPitchEngineFrame = snapshot.engine_frame;
                if (snapshot.pitch.enabled &&
                    snapshot.pitch.valid &&
                    snapshot.pitch.midi_note == 69 &&
                    std::abs(snapshot.pitch.cents) <= 5.0) {
                    pitchDetected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            jam2::EngineCommand disablePitch;
            disablePitch.type = jam2::EngineCommandType::SetPitchAnalysisEnabled;
            disablePitch.enabled = false;
            if (!engine.submit(disablePitch)) {
                throw std::runtime_error("pitch-analysis disable command queue unavailable");
            }
            const std::uint64_t disableDeadline = jam2::monotonic_us() + 1000000ULL;
            std::uint64_t stoppedWindows = 0;
            while (jam2::monotonic_us() < disableDeadline) {
                const jam2::EngineSnapshot snapshot = engine.snapshot();
                if (!snapshot.pitch.enabled) {
                    pitchDisabled = true;
                    stoppedWindows = snapshot.pitch.analyzed_windows;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            pitchStoppedProcessing = pitchDisabled &&
                engine.snapshot().pitch.analyzed_windows == stoppedWindows;
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
        record(QStringLiteral("engine-pitch.detects-local-tone-440-with-network-capture"),
            captureError.isEmpty() && pitchDetected,
            captureError.isEmpty() && !pitchDetected
                ? QStringLiteral(
                    "enabled=%1 tap=%2 valid=%3 hz=%4 cents=%5 confidence=%6 hops=%7 analyzed=%8 rejected=%9 depth=%10 overruns=%11 callbacks=%12 engine_frame=%13")
                    .arg(lastPitch.enabled)
                    .arg(lastPitch.callback_tap_enabled)
                    .arg(lastPitch.valid)
                    .arg(lastPitch.frequency_hz, 0, 'f', 3)
                    .arg(lastPitch.cents, 0, 'f', 3)
                    .arg(lastPitch.confidence, 0, 'f', 3)
                    .arg(static_cast<qulonglong>(lastPitch.input_hops))
                    .arg(static_cast<qulonglong>(lastPitch.analyzed_windows))
                    .arg(static_cast<qulonglong>(lastPitch.rejected_windows))
                    .arg(lastPitch.ring_depth_frames)
                    .arg(static_cast<qulonglong>(lastPitch.ring.overruns))
                    .arg(lastPitchCallbacks)
                    .arg(static_cast<qulonglong>(lastPitchEngineFrame))
                : captureError);
        record(QStringLiteral("engine-pitch.stops-processing-when-disabled"),
            captureError.isEmpty() && pitchDisabled && pitchStoppedProcessing,
            captureError);
    }
    {
        bool bassPitchDetected = false;
        jam2::EnginePitchSnapshot lastPitch;
        QString pitchError;
        jam2::Engine engine;
        bool engineStarted = false;
        try {
            jam2::EngineConfig config;
            config.backend = jam2::EngineAudioBackend::Headless;
            config.sample_rate = 44100;
            config.audio_buffer_frames = 256;
            config.test_input = jam2::EngineTestInput::ToneBassB0;
            engine.start(config);
            engineStarted = true;

            jam2::EngineCommand enablePitch;
            enablePitch.type = jam2::EngineCommandType::SetPitchAnalysisEnabled;
            enablePitch.enabled = true;
            if (!engine.submit(enablePitch)) {
                throw std::runtime_error("bass pitch-analysis enable command queue unavailable");
            }

            const std::uint64_t deadline = jam2::monotonic_us() + 3000000ULL;
            while (jam2::monotonic_us() < deadline) {
                lastPitch = engine.snapshot().pitch;
                if (lastPitch.enabled &&
                    lastPitch.valid &&
                    lastPitch.midi_note == 23 &&
                    std::abs(lastPitch.cents) <= 5.0) {
                    bassPitchDetected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } catch (const std::exception& exception) {
            pitchError = QString::fromUtf8(exception.what());
        }
        if (engineStarted) {
            engine.requestStop();
            engine.join();
        }
        record(QStringLiteral("engine-pitch.detects-five-string-bass-low-b"),
            pitchError.isEmpty() && bassPitchDetected,
            pitchError.isEmpty() && !bassPitchDetected
                ? QStringLiteral(
                    "enabled=%1 tap=%2 valid=%3 midi=%4 hz=%5 cents=%6 confidence=%7 hops=%8 analyzed=%9 rejected=%10 depth=%11 overruns=%12")
                    .arg(lastPitch.enabled)
                    .arg(lastPitch.callback_tap_enabled)
                    .arg(lastPitch.valid)
                    .arg(lastPitch.midi_note)
                    .arg(lastPitch.frequency_hz, 0, 'f', 3)
                    .arg(lastPitch.cents, 0, 'f', 3)
                    .arg(lastPitch.confidence, 0, 'f', 3)
                    .arg(static_cast<qulonglong>(lastPitch.input_hops))
                    .arg(static_cast<qulonglong>(lastPitch.analyzed_windows))
                    .arg(static_cast<qulonglong>(lastPitch.rejected_windows))
                    .arg(lastPitch.ring_depth_frames)
                    .arg(static_cast<qulonglong>(lastPitch.ring.overruns))
                : pitchError);
    }
    {
        bool muted = false;
        bool audible = false;
        QString outputError;
        jam2::Engine engine;
        bool engineStarted = false;
        try {
            jam2::EngineConfig config;
            config.backend = jam2::EngineAudioBackend::Headless;
            config.sample_rate = 48000;
            config.audio_buffer_frames = 64;
            config.metronome_enabled = true;
            config.metronome_level_ppm = 1000000;
            config.output_level_ppm = 0;
            engine.start(config);
            engineStarted = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            muted = engine.snapshot().output_peak_ppm == 0;
            jam2::EngineCommand level;
            level.type = jam2::EngineCommandType::SetOutputLevel;
            level.value = 1000000;
            if (!engine.submit(level)) {
                throw std::runtime_error("master output command queue unavailable");
            }
            const std::uint64_t deadline = jam2::monotonic_us() + 1000000ULL;
            while (jam2::monotonic_us() < deadline) {
                const jam2::EngineSnapshot snapshot = engine.snapshot();
                if (snapshot.output_level_ppm == 1000000 &&
                    snapshot.output_peak_ppm > 0) {
                    audible = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } catch (const std::exception& exception) {
            outputError = QString::fromUtf8(exception.what());
        }
        if (engineStarted) {
            engine.requestStop();
            engine.join();
        }
        record(QStringLiteral("master-output.scales-complete-headless-mix"),
            outputError.isEmpty() && muted && audible,
            outputError);
    }

    jam2::audio::PlaybackRatioSmoother ratioSmoother;
    ratioSmoother.setTargetPpm(1005000, 4);
    const double rampFirst = ratioSmoother.nextRatio();
    (void)ratioSmoother.nextRatio();
    (void)ratioSmoother.nextRatio();
    const double rampLast = ratioSmoother.nextRatio();
    record(QStringLiteral("engine-playback-ratio.ramps-to-target"),
        std::abs(rampFirst - 1.00125) < 0.0000001 &&
            std::abs(rampLast - 1.005) < 0.0000001 &&
            ratioSmoother.appliedPpm() == 1005000 && !ratioSmoother.ramping());
    ratioSmoother.setTargetPpm(1000000, 4);
    (void)ratioSmoother.nextRatio();
    (void)ratioSmoother.nextRatio();
    (void)ratioSmoother.nextRatio();
    const double unityRampLast = ratioSmoother.nextRatio();
    record(QStringLiteral("engine-playback-ratio.ramps-back-to-unity"),
        std::abs(unityRampLast - 1.0) < 0.0000001 && ratioSmoother.steadyUnity());
    const bool profileRatioPolicy = std::all_of(
        jam2::join_profiles().begin(), jam2::join_profiles().end(),
        [](const jam2::JoinProfile& profile) {
            return profile.adaptive_playback_release_ppm == 5000 &&
                profile.adaptive_playback_ratio_ramp_ms == 250;
        });
    record(QStringLiteral("profiles.adaptive-release-is-audibility-bounded"), profileRatioPolicy);

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
