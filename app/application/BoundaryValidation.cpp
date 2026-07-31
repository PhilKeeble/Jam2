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
#include "StyleProfileCatalog.hpp"
#include "PracticeIdeaController.hpp"
#include "PracticeReferenceRenderer.hpp"
#include "PreparedMixRenderer.hpp"
#include "ResearchDrumKit.hpp"
#include "SharedTrackModel.hpp"
#include "GuiLoopbackRecorder.hpp"
#include "GuiPresentation.hpp"
#include "RecordingTiming.hpp"

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
#include <QSet>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QUuid>
#include <QtEndian>
#include <QtGlobal>

#include <algorithm>
#include <array>
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
        record(QStringLiteral("recording.bar-duration-is-frame-exact"),
            jam2::gui::recording_frames_for_bars(8, 4, 120.0, 48000) == 768000 &&
            jam2::gui::recording_frames_for_bars(3, 3, 90.0, 44100) == 264600 &&
            jam2::gui::recording_frames_for_bars(2, 6, 120.0, 48000, 3) == 96000 &&
            jam2::gui::recording_frames_for_bars(0, 4, 120.0, 48000) == 0);
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
        {QStringLiteral("lane"), 11},
        {QStringLiteral("text"), QStringLiteral("x...")},
    };
    modelError.clear();
    const bool acceptedLastBeatLane =
        jam2::application::validateControlMessage(validBeatHit, modelError);
    validBeatHit[QStringLiteral("lane")] = 12;
    const bool rejectedOutOfRangeBeatLane =
        !jam2::application::validateControlMessage(validBeatHit, modelError);
    record(QStringLiteral("model.beat-hit-uses-current-percussion-lane-bound"),
        acceptedLastBeatLane && rejectedOutOfRangeBeatLane, modelError);
    QJsonObject musicalDivision{
        {QStringLiteral("type"), QStringLiteral("music.division")},
        {QStringLiteral("section"), 0},
        {QStringLiteral("beat"), 0},
        {QStringLiteral("division"), 3},
    };
    QJsonObject musicalStep{
        {QStringLiteral("type"), QStringLiteral("music.step")},
        {QStringLiteral("section"), 0},
        {QStringLiteral("beat"), 0},
        {QStringLiteral("step"), 2},
        {QStringLiteral("lane"), QStringLiteral("melody")},
        {QStringLiteral("text"), QStringLiteral("F#4")},
    };
    modelError.clear();
    const bool acceptedMusicalControls =
        jam2::application::validateControlMessage(musicalDivision, modelError) &&
        jam2::application::validateControlMessage(musicalStep, modelError);
    musicalDivision[QStringLiteral("division")] = 8;
    musicalStep[QStringLiteral("step")] = 4;
    const bool rejectedMusicalControls =
        !jam2::application::validateControlMessage(musicalDivision, modelError) &&
        !jam2::application::validateControlMessage(musicalStep, modelError);
    record(QStringLiteral("model.shared-musical-grid-controls-are-bounded"),
        acceptedMusicalControls && rejectedMusicalControls, modelError);
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
        jam2::practice::ChordIdeaRequest request;
        request.styleId = QStringLiteral("pop");
        request.bars = 8;
        const auto generated = jam2::practice::generateCoupledPracticeIdeaForTest(request, 123);
        BeatGridModel generatedModel;
        (void)generatedModel.replaceGeneratedSection(QStringLiteral("chord"), generated.chordSection);
        QJsonObject complexityMessage = collaborativeSong;
        QJsonObject song = generatedModel.toJson();
        song.insert(QStringLiteral("looper"), LooperProject{}.toJson());
        complexityMessage[QStringLiteral("song")] = song;
        modelError.clear();
        const bool acceptedComplexity =
            jam2::application::validateControlMessage(complexityMessage, modelError);
        QJsonArray sections = song.value(QStringLiteral("sections")).toArray();
        QJsonObject section = sections.first().toObject();
        QJsonObject recipe = section.value(QStringLiteral("generated_recipe")).toObject();
        recipe[QStringLiteral("complexity")] = 9;
        section[QStringLiteral("generated_recipe")] = recipe;
        sections[0] = section;
        song[QStringLiteral("sections")] = sections;
        complexityMessage[QStringLiteral("song")] = song;
        modelError.clear();
        const bool rejectedHighComplexity =
            !jam2::application::validateControlMessage(complexityMessage, modelError);
        recipe[QStringLiteral("complexity")] = 4.5;
        section[QStringLiteral("generated_recipe")] = recipe;
        sections[0] = section;
        song[QStringLiteral("sections")] = sections;
        complexityMessage[QStringLiteral("song")] = song;
        modelError.clear();
        const bool rejectedFractionalComplexity =
            !jam2::application::validateControlMessage(complexityMessage, modelError);
        record(QStringLiteral("model.generated-complexity-is-bounded"),
            acceptedComplexity && rejectedHighComplexity && rejectedFractionalComplexity, modelError);
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
        record(QStringLiteral("practice.chord-tones-use-diatonic-letter-spelling"),
            jam2::practice::chordToneNames(QStringLiteral("F7")) ==
                QStringLiteral("F A C Eb") &&
            jam2::practice::chordToneNames(QStringLiteral("Cm")) ==
                QStringLiteral("C Eb G") &&
            jam2::practice::chordToneNames(QStringLiteral("F#maj7")) ==
                QStringLiteral("F# A# C# E#") &&
            jam2::practice::chordToneNames(QStringLiteral("Gbmaj7")) ==
                QStringLiteral("Gb Bb Db F"));
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
        const QStringList ids = jam2::practice::styleIds();
        const QStringList names = jam2::practice::chordStyleNames();
        constexpr int variationCases = 8;
        QSet<QString> grooveCatalogIds;
        bool grooveCatalogValid = true;
        QSet<QString> profileCatalogIds;
        for (const QString& styleId : ids) {
            const QStringList familyIds = jam2::practice::grooveFamilyIds(styleId);
            const QStringList familyNames = jam2::practice::grooveFamilyNames(styleId);
            const QStringList profileIds = jam2::practice::profileIds(styleId);
            const QStringList profileNames = jam2::practice::profileNames(styleId);
            grooveCatalogValid = grooveCatalogValid && !familyIds.isEmpty() &&
                familyIds.size() == familyNames.size() &&
                QSet<QString>(familyIds.cbegin(), familyIds.cend()).size() == familyIds.size() &&
                profileIds.size() == profileNames.size() && !profileIds.isEmpty();
            for (const QString& familyId : familyIds) grooveCatalogIds.insert(familyId);
            for (const QString& profileId : profileIds) profileCatalogIds.insert(profileId);
        }
        record(QStringLiteral("practice.v7-catalog-has-research-approved-profiles"),
            ids.size() == 13 && names.size() == 13 && QSet<QString>(ids.cbegin(), ids.cend()).size() == 13 &&
            profileCatalogIds.size() == 26 &&
            !names.contains(QStringLiteral("Modern Metal")) &&
            names.contains(QStringLiteral("J-Pop / Anisong")) &&
            names.contains(QStringLiteral("Hip-Hop / Trap")) &&
            names.contains(QStringLiteral("Reggae")) &&
            names.contains(QStringLiteral("Bossa Nova")));
        record(QStringLiteral("practice.v7-catalog-grooves-are-profile-routed"),
            grooveCatalogValid && grooveCatalogIds.size() >= 30);

        bool matrixValid = true;
        bool theoryBudgetsValid = true;
        bool cumulativePaletteValid = true;
        bool densityValid = true;
        bool stableClickValid = true;
        bool stableSeedTempoValid = true;
        bool stableSeedMotifValid = true;
        QString matrixDetail;
        QSet<QString> levelEightKinds;
        const auto theoryTier = [](const QString& kind) {
            if (kind == QStringLiteral("inversion")) return 2;
            if (kind == QStringLiteral("modal-interchange")) return 3;
            if (kind == QStringLiteral("diatonic-extension"))
                return 3;
            if (kind == QStringLiteral("secondary-dominant")) return 5;
            if (kind == QStringLiteral("passing-diminished")) return 5;
            if (kind == QStringLiteral("backdoor-dominant")) return 6;
            if (kind == QStringLiteral("tritone-substitution")) return 7;
            if (kind == QStringLiteral("temporary-modulation")) return 7;
            return 99;
        };
        static constexpr std::array<int, 8> perEight{0, 1, 1, 1, 2, 2, 3, 4};
        for (int style = 0; style < ids.size(); ++style) {
            for (int variationCase = 0;
                 variationCase < variationCases;
                 ++variationCase) {
                for (int complexity = 1; complexity <= 8; ++complexity) {
                    jam2::practice::ChordIdeaRequest request;
                    request.key = (style + variationCase) % 12;
                    request.styleId = ids.at(style);
                    request.bars = 8;
                    request.beatsPerBar = 4;
                    request.harmonicComplexity = complexity;
                    request.rhythmicComplexity = complexity;
                    const auto idea = jam2::practice::generateCoupledPracticeIdeaForTest(
                        request, static_cast<std::uint32_t>(
                            1000 + style * 100 + variationCase * 10 + complexity));
                    const auto& recipe = idea.recipe;
                    const int expectedClickPulses = recipe.tempoPulseUnits == 3
                        ? recipe.beatGrouping.size() : recipe.meterNumerator;
                    stableClickValid = stableClickValid && idea.clickDivision == recipe.clickDivision &&
                        idea.clickEnabled.size() == recipe.meterNumerator * recipe.clickDivision &&
                        idea.clickAccents.size() == recipe.meterNumerator * recipe.clickDivision &&
                        std::count(idea.clickEnabled.cbegin(), idea.clickEnabled.cend(), true) ==
                            expectedClickPulses &&
                        !idea.clickAccents.isEmpty() && idea.clickAccents.front() &&
                        std::count(idea.clickAccents.cbegin(), idea.clickAccents.cend(), true) ==
                            1;
                    const auto* profileDefinition =
                        jam2::practice::findProfile(recipe.profileId, true);
                    const bool headerValid = recipe.isValid() && recipe.generatorVersion == 7 &&
                        recipe.styleId == request.styleId &&
                        !recipe.variationId.isEmpty() &&
                        !recipe.variationSummary.isEmpty() &&
                        profileDefinition != nullptr &&
                        recipe.bpm >= profileDefinition->minimumBpm &&
                        recipe.bpm <= profileDefinition->maximumBpm &&
                        (recipe.tempoPulseUnits == 1 ||
                         recipe.tempoPulseUnits == 3) &&
                        jam2::practice::profileIds(request.styleId).contains(recipe.profileId) &&
                        recipe.complexity == complexity &&
                        idea.chordSection.beats == recipe.bars * recipe.meterNumerator &&
                        idea.beatSection.beats == idea.chordSection.beats &&
                        idea.chordSection.generatedRecipe.isValid() &&
                        jam2::practice::grooveFamilyIds(request.styleId).contains(recipe.grooveId) &&
                        !recipe.formId.isEmpty() && !recipe.meterId.isEmpty() &&
                        !recipe.bassPatchId.isEmpty() && !recipe.supportPatchId.isEmpty() &&
                        !recipe.teachingSummary.isEmpty() && !recipe.jamGuidance.isEmpty() &&
                        !recipe.complexityTools.isEmpty() &&
                        recipe.swingPercent >= 50 && recipe.swingPercent <= 67 &&
                        recipe.snareOffsetMs >= -20 && recipe.snareOffsetMs <= 25 &&
                        recipe.timingVariationMs >= 0 && recipe.timingVariationMs <= 5 &&
                        recipe.velocityVariationPercent >= 0 && recipe.velocityVariationPercent <= 12 &&
                        !recipe.drumEvents.isEmpty() &&
                        recipe.drumPatchRevision >= 1 &&
                        recipe.drumMixGainDb == 3.0 &&
                        !idea.chordSection.name.startsWith(QStringLiteral("Key -")) &&
                        idea.chordSection.name.startsWith(recipe.tonic + QLatin1Char(' ') + recipe.mode);
                    if (!headerValid && matrixDetail.isEmpty()) {
                        QFile diagnostic(
                            QDir::current().absoluteFilePath(
                                QStringLiteral(
                                    "build/invalid-generation-recipe.json")));
                        if (diagnostic.open(
                                QIODevice::WriteOnly |
                                QIODevice::Truncate)) {
                            diagnostic.write(
                                QJsonDocument(
                                    jam2::practice::
                                        generationRecipeToJson(
                                            recipe))
                                    .toJson(
                                        QJsonDocument::Indented));
                        }
                        QStringList failed;
                        if (!recipe.isValid()) failed << QStringLiteral("recipe");
                        if (recipe.styleId != request.styleId) failed << QStringLiteral("style");
                        if (profileDefinition == nullptr) failed << QStringLiteral("profile");
                        if (profileDefinition != nullptr &&
                            (recipe.bpm < profileDefinition->minimumBpm ||
                             recipe.bpm > profileDefinition->maximumBpm))
                            failed << QStringLiteral("bpm");
                        if (!idea.chordSection.generatedRecipe.isValid())
                            failed << QStringLiteral("embedded-recipe");
                        if (idea.chordSection.beats !=
                                recipe.bars * recipe.meterNumerator ||
                            idea.beatSection.beats != idea.chordSection.beats)
                            failed << QStringLiteral("section-length");
                        if (!jam2::practice::grooveFamilyIds(
                                 request.styleId).contains(recipe.grooveId))
                            failed << QStringLiteral("groove");
                        if (recipe.drumMixGainDb != 3.0)
                            failed << QStringLiteral("drum-gain");
                        if (!idea.chordSection.name.startsWith(
                                recipe.tonic + QLatin1Char(' ') +
                                recipe.mode))
                            failed << QStringLiteral("title");
                        matrixDetail = QStringLiteral(
                            "header style=%1 variation=%2 level=%3 "
                            "failed=%4 profile=%5 form=%6 bars=%7 "
                            "melody=%8 phrases=%9 bass=%10 support=%11 "
                            "drums=%12 drum_phrases=%13 theory=%14 "
                            "variation_notes=%15 motif_notes=%16")
                            .arg(ids.at(style))
                            .arg(variationCase)
                            .arg(complexity)
                            .arg(failed.join(QLatin1Char(',')))
                            .arg(recipe.profileId)
                            .arg(recipe.formId)
                            .arg(recipe.bars)
                            .arg(recipe.melodyEvents.size())
                            .arg(recipe.melodyPhrases.size())
                            .arg(recipe.bassEvents.size())
                            .arg(recipe.supportingEvents.size())
                            .arg(recipe.drumEvents.size())
                            .arg(recipe.drumPhrases.size())
                            .arg(recipe.theoryDecisions.size())
                            .arg(recipe.variationDecisions.size())
                            .arg(recipe.motifTransformations.size());
                        if (!recipe.supportingEvents.isEmpty()) {
                            const auto& event =
                                recipe.supportingEvents.back();
                            matrixDetail += QStringLiteral(
                                " last_support=%1/%2/%3/%4/%5/%6/%7")
                                .arg(event.tick)
                                .arg(event.durationTicks)
                                .arg(event.midi)
                                .arg(event.velocity)
                                .arg(event.note.size())
                                .arg(event.role.size())
                                .arg(event.relationship.size());
                        }
                        matrixDetail += QStringLiteral(
                            " phrase_bars=%1 sections=%2 automation=%3 "
                            "tools=%4 voices=%5 groove_notes=%6 "
                            "patch_notes=%7 timing=%8")
                            .arg(recipe.phraseBars)
                            .arg(recipe.formSections.size())
                            .arg(recipe.automationEvents.size())
                            .arg(recipe.complexityTools.size())
                            .arg(recipe.synthVoices.size())
                            .arg(recipe.grooveDecisions.size())
                            .arg(recipe.patchModifiers.size())
                            .arg(recipe.laneTiming.size());
                        const auto maximumStringLength =
                            [](const QStringList& values) {
                                int maximum = 0;
                                for (const QString& value : values)
                                    maximum = qMax(
                                        maximum, value.size());
                                return maximum;
                            };
                        matrixDetail += QStringLiteral(
                            " string_max=%1/%2/%3/%4/%5/%6")
                            .arg(maximumStringLength(
                                recipe.variationDecisions))
                            .arg(maximumStringLength(
                                recipe.motifTransformations))
                            .arg(maximumStringLength(
                                recipe.grooveDecisions))
                            .arg(maximumStringLength(
                                recipe.patchModifiers))
                            .arg(maximumStringLength(
                                recipe.continuationStrategies))
                            .arg(maximumStringLength(
                                recipe.variationAxes));
                        for (const auto& event :
                             recipe.melodyEvents) {
                            if (event.tick < 0 ||
                                event.tick > 8192 ||
                                event.durationTicks < 1 ||
                                event.durationTicks > 8192 ||
                                event.midi < 0 ||
                                event.midi > 127 ||
                                event.velocity < 1 ||
                                event.velocity > 127 ||
                                event.note.size() > 96 ||
                                event.chord.size() > 96 ||
                                event.chordRole.size() > 96 ||
                                event.melodicRole.size() > 96) {
                                matrixDetail += QStringLiteral(
                                    " bad_melody=%1/%2/%3/%4/%5/%6")
                                    .arg(event.tick)
                                    .arg(event.durationTicks)
                                    .arg(event.midi)
                                    .arg(event.velocity)
                                    .arg(event.chordRole.size())
                                    .arg(event.melodicRole.size());
                                break;
                            }
                        }
                        for (const auto& phrase :
                             recipe.melodyPhrases) {
                            if (phrase.startBar < 1 ||
                                phrase.endBar <
                                    phrase.startBar ||
                                phrase.endBar > 64 ||
                                phrase.label.size() > 96 ||
                                phrase.summary.size() > 256) {
                                matrixDetail += QStringLiteral(
                                    " bad_melody_phrase=%1-%2/%3/%4")
                                    .arg(phrase.startBar)
                                    .arg(phrase.endBar)
                                    .arg(phrase.label.size())
                                    .arg(phrase.summary.size());
                                break;
                            }
                        }
                        const auto invalidRoleEvent =
                            [](const auto& event) {
                                return event.tick < 0 ||
                                    event.tick > 32768 ||
                                    event.durationTicks < 1 ||
                                    event.durationTicks > 32768 ||
                                    event.midi < 0 ||
                                    event.midi > 127 ||
                                    event.velocity < 1 ||
                                    event.velocity > 127 ||
                                    event.note.size() > 96 ||
                                    event.role.size() > 64 ||
                                    event.relationship.size() > 256 ||
                                    event.articulation.size() > 96;
                            };
                        for (const auto& event :
                             recipe.bassEvents) {
                            if (invalidRoleEvent(event)) {
                                matrixDetail += QStringLiteral(
                                    " bad_bass=%1/%2/%3/%4/%5")
                                    .arg(event.tick)
                                    .arg(event.durationTicks)
                                    .arg(event.midi)
                                    .arg(event.relationship.size())
                                    .arg(event.articulation.size());
                                break;
                            }
                        }
                        for (const auto& event :
                             recipe.supportingEvents) {
                            if (invalidRoleEvent(event)) {
                                matrixDetail += QStringLiteral(
                                    " bad_support=%1/%2/%3/%4/%5")
                                    .arg(event.tick)
                                    .arg(event.durationTicks)
                                    .arg(event.midi)
                                    .arg(event.relationship.size())
                                    .arg(event.articulation.size());
                                break;
                            }
                        }
                        for (const auto& phrase :
                             recipe.drumPhrases) {
                            const bool noFill =
                                phrase.fillStartBeat == -1 &&
                                phrase.fillBeatCount == 0;
                            const bool fillValid =
                                phrase.fillStartBeat >= 0 &&
                                phrase.fillBeatCount >= 1 &&
                                phrase.fillStartBeat +
                                        phrase.fillBeatCount <=
                                    recipe.bars *
                                        recipe.beatsPerBar;
                            if (phrase.startBar < 1 ||
                                phrase.endBar <
                                    phrase.startBar ||
                                phrase.endBar > recipe.bars ||
                                (!noFill && !fillValid)) {
                                matrixDetail += QStringLiteral(
                                    " bad_drum_phrase=%1-%2/%3+%4")
                                    .arg(phrase.startBar)
                                    .arg(phrase.endBar)
                                    .arg(phrase.fillStartBeat)
                                    .arg(phrase.fillBeatCount);
                                break;
                            }
                        }
                        int previousDrumTick = -1;
                        for (const auto& event :
                             recipe.drumEvents) {
                            if (event.tick < 0 ||
                                event.tick >= recipe.bars *
                                    recipe.beatsPerBar * 12 ||
                                event.tick < previousDrumTick ||
                                event.velocity < 1 ||
                                event.velocity > 127 ||
                                event.offsetMs < -40 ||
                                event.offsetMs > 40 ||
                                event.repeatGroup < 0 ||
                                event.repeatGroup > 4096) {
                                matrixDetail += QStringLiteral(
                                    " bad_drum=%1/%2/%3/%4")
                                    .arg(event.tick)
                                    .arg(previousDrumTick)
                                    .arg(event.velocity)
                                    .arg(event.repeatGroup);
                                break;
                            }
                            previousDrumTick = event.tick;
                        }
                    }
                    matrixValid = matrixValid && headerValid;
                    theoryBudgetsValid = theoryBudgetsValid &&
                        recipe.theoryDecisions.size() <=
                            perEight.at(complexity - 1) * ((recipe.bars + 7) / 8) &&
                        (complexity != 1 || recipe.theoryDecisions.isEmpty());
                    for (const auto& decision : recipe.theoryDecisions) {
                        cumulativePaletteValid = cumulativePaletteValid && theoryTier(decision.kind) <= complexity;
                        if (complexity == 8) levelEightKinds.insert(decision.kind);
                    }
                    for (const QString& chord : idea.chordSection.chords) {
                        const bool chordValid = chord.isEmpty() || jam2::practice::parseChord(chord).valid;
                        if (!chordValid && matrixDetail.isEmpty()) matrixDetail = QStringLiteral("chord '%1' style=%2 variation=%3 level=%4").arg(chord, ids.at(style)).arg(variationCase).arg(complexity);
                        matrixValid = matrixValid && chordValid;
                    }
                    for (const QString& note : idea.chordSection.targets)
                        matrixValid = matrixValid && (note.isEmpty() || note == QStringLiteral("-") ||
                            jam2::practice::parseMidiNote(note).has_value());
                    const bool timedPatternValid =
                        idea.chordSection.musicalPatterns.size() == idea.chordSection.beats &&
                        !recipe.melodyEvents.isEmpty() && !recipe.melodyPhrases.isEmpty();
                    if (!timedPatternValid && matrixDetail.isEmpty())
                        matrixDetail = QStringLiteral("timed pattern missing style=%1 variation=%2 level=%3")
                            .arg(ids.at(style)).arg(variationCase).arg(complexity);
                    matrixValid = matrixValid && timedPatternValid;
                    int previousMelody = -1;
                    int repeatedMelody = 0;
                    for (int beatIndex = 0; beatIndex < idea.chordSection.musicalPatterns.size(); ++beatIndex) {
                        const MusicalBeatPattern& musical = idea.chordSection.musicalPatterns[beatIndex];
                        matrixValid = matrixValid &&
                            BeatGridModel::musicalDivisionValues().contains(musical.division) &&
                            musical.chords.size() == musical.division &&
                            musical.melody.size() == musical.division &&
                            musical.bass.size() == musical.division &&
                            musical.support.size() == musical.division;
                    }
                    for (const auto& event : recipe.melodyEvents) {
                        const bool eventValid = event.midi >= 52 && event.midi <= 81 &&
                            event.durationTicks >= 1 && event.velocity >= 1 && event.velocity <= 127 &&
                            !event.chordRole.isEmpty() && !event.melodicRole.isEmpty();
                        if (!eventValid && matrixDetail.isEmpty())
                            matrixDetail = QStringLiteral("invalid melody event style=%1 variation=%2 level=%3 note=%4")
                                .arg(ids.at(style)).arg(variationCase).arg(complexity).arg(event.note);
                        matrixValid = matrixValid && eventValid;
                        if (previousMelody >= 0) {
                            const bool leapValid = std::abs(event.midi - previousMelody) <= 7;
                            repeatedMelody = event.midi == previousMelody ? repeatedMelody + 1 : 1;
                            if ((!leapValid || repeatedMelody > 4) && matrixDetail.isEmpty())
                                matrixDetail = QStringLiteral("melody motion style=%1 variation=%2 level=%3 leap=%4 repeats=%5")
                                    .arg(ids.at(style)).arg(variationCase).arg(complexity)
                                    .arg(std::abs(event.midi - previousMelody)).arg(repeatedMelody);
                            matrixValid = matrixValid && leapValid && repeatedMelody <= 4;
                        } else {
                            repeatedMelody = 1;
                        }
                        previousMelody = event.midi;
                    }
                    for (int drumBeat = 0; drumBeat < idea.beatSection.beatPatterns.size(); ++drumBeat) {
                        const BeatPattern& pattern = idea.beatSection.beatPatterns[drumBeat];
                        matrixValid = matrixValid && BeatGridModel::beatDivisionValues().contains(pattern.division) &&
                            pattern.lanes.size() == BeatGridModel::beatLaneNames().size();
                        for (const QString& laneText : pattern.lanes) {
                            matrixValid = matrixValid && laneText.size() == pattern.division &&
                                std::all_of(laneText.cbegin(), laneText.cend(), [](QChar state) {
                                    return state == QLatin1Char('.') || state == QLatin1Char('x') ||
                                        state == QLatin1Char('a') || state == QLatin1Char('g');
                                });
                        }
                        const QStringList drumLanes = BeatGridModel::beatLaneNames();
                        const std::array<int, 4> cymbalLanes{
                            static_cast<int>(drumLanes.indexOf(QStringLiteral("Closed HH"))),
                            static_cast<int>(drumLanes.indexOf(QStringLiteral("Open HH"))),
                            static_cast<int>(drumLanes.indexOf(QStringLiteral("Ride"))),
                            static_cast<int>(drumLanes.indexOf(QStringLiteral("Crash"))),
                        };
                        for (int step = 0; step < pattern.division; ++step) {
                            int activeVoices = 0;
                            int activeCymbals = 0;
                            for (int laneIndex = 0; laneIndex < pattern.lanes.size(); ++laneIndex) {
                                if (pattern.lanes[laneIndex].at(step) == QLatin1Char('.')) continue;
                                ++activeVoices;
                                if (std::find(cymbalLanes.cbegin(), cymbalLanes.cend(), laneIndex) != cymbalLanes.cend())
                                    ++activeCymbals;
                            }
                            if ((activeVoices > 3 || activeCymbals > 1) && matrixDetail.isEmpty()) {
                                matrixDetail = QStringLiteral(
                                    "drum limbs style=%1 variation=%2 level=%3 family=%4 beat=%5 step=%6 voices=%7 cymbals=%8")
                                    .arg(ids.at(style)).arg(variationCase).arg(complexity)
                                    .arg(recipe.grooveId).arg(drumBeat).arg(step)
                                    .arg(activeVoices).arg(activeCymbals);
                            }
                            matrixValid = matrixValid && activeVoices <= 3 && activeCymbals <= 1;
                        }
                    }
                }
            }
            QSet<QString> families;
            QSet<QString> grooveFamilies;
            QSet<QString> beatFingerprints;
            for (std::uint32_t seed = 0; seed < 160; ++seed) {
                jam2::practice::ChordIdeaRequest request;
                request.styleId = ids.at(style);
                request.bars = 8;
                const auto generated = jam2::practice::generateCoupledPracticeIdeaForTest(request, seed);
                families.insert(generated.recipe.progressionId);
                grooveFamilies.insert(generated.recipe.grooveId);
                beatFingerprints.insert(generated.recipe.beatFingerprint);
            }
            if (families.isEmpty() && matrixDetail.isEmpty()) matrixDetail = QStringLiteral("family coverage style=%1 count=%2").arg(ids.at(style)).arg(families.size());
            if ((grooveFamilies.isEmpty() || beatFingerprints.size() < 80) && matrixDetail.isEmpty())
                matrixDetail = QStringLiteral("groove coverage style=%1 families=%2 fingerprints=%3")
                    .arg(ids.at(style)).arg(grooveFamilies.size()).arg(beatFingerprints.size());
            matrixValid = matrixValid && !families.isEmpty() && !grooveFamilies.isEmpty() &&
                beatFingerprints.size() >= 80;

            jam2::practice::ChordIdeaRequest simple;
            simple.styleId = ids.at(style);
            simple.bars = 16;
            simple.harmonicComplexity = 1;
            simple.rhythmicComplexity = 1;
            jam2::practice::ChordIdeaRequest complex = simple;
            complex.harmonicComplexity = 8;
            complex.rhythmicComplexity = 8;
            const auto simpleIdea = jam2::practice::generateCoupledPracticeIdeaForTest(simple, 77);
            const auto complexIdea = jam2::practice::generateCoupledPracticeIdeaForTest(complex, 77);
            stableSeedTempoValid =
                stableSeedTempoValid &&
                simpleIdea.recipe.bpm == complexIdea.recipe.bpm;
            if (simpleIdea.recipe.progressionId ==
                complexIdea.recipe.progressionId) {
                stableSeedMotifValid =
                    stableSeedMotifValid &&
                    simpleIdea.recipe.motifCell ==
                        complexIdea.recipe.motifCell;
            }
            const auto hits = [](const SongSection& section) {
                int count = 0;
                for (const BeatPattern& pattern : section.beatPatterns)
                    for (const QString& lane : pattern.lanes)
                        for (QChar state : lane) if (state == QLatin1Char('x') || state == QLatin1Char('a') || state == QLatin1Char('g')) ++count;
                return count;
            };
            const int simpleHits = hits(simpleIdea.beatSection);
            const int complexHits = hits(complexIdea.beatSection);
            bool performanceSame =
                simpleIdea.recipe.drumEvents.size() ==
                complexIdea.recipe.drumEvents.size();
            for (int eventIndex = 0;
                 performanceSame &&
                 eventIndex < simpleIdea.recipe.drumEvents.size();
                 ++eventIndex) {
                const auto& simpleEvent =
                    simpleIdea.recipe.drumEvents.at(eventIndex);
                const auto& complexEvent =
                    complexIdea.recipe.drumEvents.at(eventIndex);
                performanceSame =
                    simpleEvent.tick == complexEvent.tick &&
                    simpleEvent.laneId == complexEvent.laneId &&
                    simpleEvent.velocity == complexEvent.velocity &&
                    simpleEvent.offsetMs == complexEvent.offsetMs &&
                    simpleEvent.articulation ==
                        complexEvent.articulation &&
                    simpleEvent.role == complexEvent.role &&
                    simpleEvent.repeatGroup ==
                        complexEvent.repeatGroup &&
                    simpleEvent.fill == complexEvent.fill;
            }
            densityValid = densityValid &&
                simpleHits == complexHits &&
                simpleIdea.recipe.beatFingerprint ==
                    complexIdea.recipe.beatFingerprint &&
                performanceSame;
        }
        bool compoundBluesValid = true;
        QString compoundBluesDetail;
        for (std::uint32_t seed = 0; seed < 256; ++seed) {
            jam2::practice::ChordIdeaRequest request;
            request.styleId = QStringLiteral("rock");
            request.profileId = QStringLiteral("rock_shuffle_blues");
            request.formId = QStringLiteral("rock-blues-12-8");
            request.meterId = QStringLiteral("12-8");
            request.harmonicComplexity = 2;
            request.rhythmicComplexity = 2;
            const auto generated =
                jam2::practice::generateCoupledPracticeIdeaForTest(request, seed);
            const auto& recipe = generated.recipe;
            const QString expectedName = QStringLiteral("%1 %2 - %3 - 12/8")
                .arg(recipe.tonic, recipe.mode, recipe.profileName);
            const bool clickValid =
                generated.clickDivision == 1 &&
                generated.clickEnabled.size() == 12 &&
                generated.clickEnabled.value(0) &&
                generated.clickEnabled.value(3) &&
                generated.clickEnabled.value(6) &&
                generated.clickEnabled.value(9) &&
                std::count(generated.clickEnabled.cbegin(),
                    generated.clickEnabled.cend(), true) == 4 &&
                generated.clickAccents.value(0) &&
                std::count(generated.clickAccents.cbegin(),
                    generated.clickAccents.cend(), true) == 1;
            const bool seedValid =
                recipe.generatorVersion == 7 &&
                recipe.profileId == QStringLiteral("rock_shuffle_blues") &&
                recipe.formId == QStringLiteral("rock-blues-12-8") &&
                recipe.meterNumerator == 12 &&
                recipe.meterDenominator == 8 &&
                recipe.beatUnit == 8 &&
                recipe.tempoPulseUnits == 3 &&
                recipe.tempoPulseName == QStringLiteral("dotted quarter note") &&
                recipe.bpm >= 60 && recipe.bpm <= 180 &&
                generated.bpm == recipe.bpm &&
                generated.chordSection.name == expectedName &&
                generated.beatSection.name == expectedName &&
                !generated.chordSection.name.contains(QStringLiteral("Intense"),
                    Qt::CaseInsensitive) &&
                clickValid &&
                jam2::metronome::step_interval_samples(
                    48000.0, recipe.bpm, 1, recipe.tempoPulseUnits) ==
                    static_cast<std::uint64_t>(std::llround(
                        60.0 * 48000.0 /
                        static_cast<double>(recipe.bpm * 3)));
            if (!seedValid && compoundBluesDetail.isEmpty()) {
                compoundBluesDetail = QStringLiteral(
                    "seed=%1 bpm=%2 pulse_units=%3 title=%4 clicks=%5")
                    .arg(seed)
                    .arg(recipe.bpm)
                    .arg(recipe.tempoPulseUnits)
                    .arg(generated.chordSection.name)
                    .arg(std::count(generated.clickEnabled.cbegin(),
                        generated.clickEnabled.cend(), true));
            }
            compoundBluesValid = compoundBluesValid && seedValid;
        }
        record(QStringLiteral("practice.v7-style-profile-complexity-matrix"), matrixValid, matrixDetail);
        record(QStringLiteral("practice.v7-shuffle-blues-12-8-keeps-researched-tempo-and-grouped-pulse"),
            compoundBluesValid, compoundBluesDetail);
        jam2::practice::ChordIdeaRequest reggaeRequest;
        reggaeRequest.styleId = QStringLiteral("reggae");
        reggaeRequest.profileId = QStringLiteral("reggae_roots");
        reggaeRequest.bars = 8;
        const auto reggaeIdea =
            jam2::practice::generateCoupledPracticeIdeaForTest(reggaeRequest, 0x52454747U);
        BeatGridModel reggaeModel;
        const int reggaeSection =
            reggaeModel.replaceGeneratedSection(QStringLiteral("chord"), reggaeIdea.chordSection);
        bool reggaeHasOffbeatChords = false;
        for (const MusicalBeatPattern& pattern : reggaeIdea.chordSection.musicalPatterns) {
            if (pattern.division > 1 &&
                pattern.chords.front().state == MusicalStepState::Rest &&
                std::any_of(
                    pattern.chords.cbegin() + 1,
                    pattern.chords.cend(),
                    [](const MusicalStep& step) {
                        return step.state == MusicalStepState::Onset &&
                            !step.value.trimmed().isEmpty();
                    })) {
                reggaeHasOffbeatChords = true;
                break;
            }
        }
        const bool reggaeHasVisibleHarmony = reggaeSection >= 0 &&
            std::any_of(
                reggaeModel.section(reggaeSection).chords.cbegin(),
                reggaeModel.section(reggaeSection).chords.cend(),
                [](const QString& chord) {
                    const QString value = chord.trimmed();
                    return !value.isEmpty() && value != QStringLiteral("-");
                });
        record(QStringLiteral("practice.reggae-offbeat-comping-preserves-chord-display"),
            reggaeSection >= 0 &&
            reggaeHasOffbeatChords &&
            reggaeHasVisibleHarmony &&
            reggaeModel.section(reggaeSection).chords ==
                reggaeIdea.chordSection.chords);
        record(QStringLiteral("practice.v7-theory-decisions-are-bounded"), theoryBudgetsValid);
        record(QStringLiteral("practice.v7-complexity-is-an-unlocked-profile-palette"),
            cumulativePaletteValid && levelEightKinds.size() >= 4 &&
            levelEightKinds.contains(QStringLiteral("inversion")));
        record(QStringLiteral("practice.v7-drums-are-independent-of-global-complexity"), densityValid);
        record(QStringLiteral("practice.v7-matched-complexity-keeps-seeded-tempo"),
            stableSeedTempoValid);
        record(QStringLiteral("practice.v7-matched-complexity-develops-seeded-motif"),
            stableSeedMotifValid);
        bool performanceEventsValid = true;
        bool researchedKitCatalogValid = true;
        bool researchedKitVoicesValid = true;
        bool trapSubLayerValid = false;
        bool fullFormFlowValid = true;
        bool acousticThreeTomFill = false;
        bool drumPhrasePlansValid = true;
        QString fullFormFlowDetail;
        QString drumPhrasePlanDetail;
        const QStringList drumLaneIds{
            QStringLiteral("kick"),
            QStringLiteral("snare"),
            QStringLiteral("closed_hat"),
            QStringLiteral("open_hat"),
            QStringLiteral("ride"),
            QStringLiteral("crash"),
            QStringLiteral("high_tom"),
            QStringLiteral("mid_tom"),
            QStringLiteral("floor_tom"),
            QStringLiteral("cross_stick"),
            QStringLiteral("shaker"),
            QStringLiteral("hand_percussion"),
        };
        QSet<QString> researchedKitIds;
        for (const auto& profile :
             jam2::practice::profileCatalog()) {
            const auto* kit =
                jam2::practice::researchDrumKitForProfile(
                    profile.id);
            researchedKitCatalogValid =
                researchedKitCatalogValid &&
                kit &&
                !kit->id.isEmpty() &&
                kit->id.startsWith(profile.id + QLatin1Char(':')) &&
                kit->revision > 0 &&
                kit->pieces.size() ==
                    drumLaneIds.size();
            if (!kit) continue;
            researchedKitCatalogValid =
                researchedKitCatalogValid &&
                !researchedKitIds.contains(kit->id);
            researchedKitIds.insert(kit->id);
            for (const QString& laneId : drumLaneIds) {
                const auto* piece =
                    jam2::practice::researchDrumPiece(
                        *kit,
                        laneId);
                researchedKitCatalogValid =
                    researchedKitCatalogValid &&
                    piece &&
                    !piece->intendedIdentity.isEmpty() &&
                    jam2::practice::researchDrumSourceSupportsLane(
                        laneId,
                        piece->source) &&
                    (piece->secondSource ==
                         QStringLiteral("off") ||
                     jam2::practice::researchDrumSourceSupportsLane(
                         laneId,
                         piece->secondSource)) &&
                    piece->ghost.minimum >= 1 &&
                    piece->ghost.maximum >=
                        piece->ghost.minimum &&
                    piece->normal.minimum >
                        piece->ghost.minimum &&
                    piece->normal.maximum >=
                        piece->normal.minimum &&
                    piece->accent.minimum >
                        piece->normal.minimum &&
                    piece->accent.maximum >=
                        piece->accent.minimum &&
                    piece->accent.maximum <= 127;
                if (!piece ||
                    piece->source ==
                        QStringLiteral("jam2-native")) {
                    continue;
                }
                const std::array<int, 3> testVelocities{
                    (piece->ghost.minimum +
                     piece->ghost.maximum) / 2,
                    (piece->normal.minimum +
                     piece->normal.maximum) / 2,
                    (piece->accent.minimum +
                     piece->accent.maximum) / 2,
                };
                for (int velocityIndex = 0;
                     velocityIndex <
                         static_cast<int>(testVelocities.size());
                     ++velocityIndex) {
                    const int testVelocity =
                        testVelocities[velocityIndex];
                    const QString articulation =
                        velocityIndex == 0
                        ? QStringLiteral("ghost")
                        : velocityIndex == 2
                            ? QStringLiteral("accent")
                            : QStringLiteral("normal");
                    const auto rendered =
                        jam2::practice::renderResearchDrumVoices(
                            *kit,
                            {{
                                0,
                                laneId,
                                articulation,
                                testVelocity,
                                velocityIndex,
                                0x4a326b1dU,
                            }},
                            4096,
                            48000);
                    double peak = 0.0;
                    for (float sample : rendered.dry) {
                        researchedKitVoicesValid =
                            researchedKitVoicesValid &&
                            std::isfinite(sample);
                        peak = qMax(peak, std::abs(sample));
                    }
                    researchedKitVoicesValid =
                        researchedKitVoicesValid &&
                        peak > 0.000001 &&
                        peak < 2.0;
                }
            }
        }
        if (const auto* trapKit =
                jam2::practice::researchDrumKitForProfile(
                    QStringLiteral("hiphop_trap"))) {
            if (const auto* kick =
                    jam2::practice::researchDrumPiece(
                        *trapKit,
                        QStringLiteral("kick"))) {
                jam2::practice::ResearchDrumKit withoutSub =
                    *trapKit;
                withoutSub.pieces[QStringLiteral("kick")]
                    .synthSource = QStringLiteral("off");
                const QVector<
                    jam2::practice::ResearchDrumRenderEvent>
                    event{{
                        0,
                        QStringLiteral("kick"),
                        QStringLiteral("normal"),
                        (kick->normal.minimum +
                         kick->normal.maximum) / 2,
                        0,
                        0x7a9d31e5U,
                    }};
                const auto withSub =
                    jam2::practice::renderResearchDrumVoices(
                        *trapKit,
                        event,
                        48000,
                        48000);
                const auto without =
                    jam2::practice::renderResearchDrumVoices(
                        withoutSub,
                        event,
                        48000,
                        48000);
                double differenceEnergy = 0.0;
                for (int frame = 0;
                     frame < withSub.dry.size() &&
                     frame < without.dry.size();
                     ++frame) {
                    const double difference =
                        withSub.dry.at(frame) -
                        without.dry.at(frame);
                    differenceEnergy +=
                        difference * difference;
                }
                const double differenceRms = std::sqrt(
                    differenceEnergy /
                    qMax(1, withSub.dry.size()));
                trapSubLayerValid =
                    kick->synthSource ==
                        QStringLiteral("sine-fundamental") &&
                    kick->synthMidiNote == 30 &&
                    kick->synthLevel > 0.1f &&
                    differenceRms > 0.0001;
            }
        }
        researchedKitCatalogValid =
            researchedKitCatalogValid &&
            researchedKitIds.size() ==
                jam2::practice::profileCatalog().size();
        int phraseProfileIndex = 0;
        for (const auto& profile :
             jam2::practice::profileCatalog(true)) {
            int phraseFormIndex = 0;
            for (const auto& form : profile.forms) {
                jam2::practice::ChordIdeaRequest request;
                request.styleId = profile.styleId;
                request.profileId = profile.id;
                request.formId = form.id;
                request.meterId = form.meterId;
                request.bars = form.bars;
                request.harmonicComplexity = 4;
                request.rhythmicComplexity = 4;
                const auto generated =
                    jam2::practice::generateCoupledPracticeIdeaForTest(
                        request,
                        static_cast<std::uint32_t>(
                            61000 +
                            phraseProfileIndex * 100 +
                            phraseFormIndex));
                const auto& generatedRecipe =
                    generated.recipe;
                int expectedStartBar = 1;
                int plannedFillCount = 0;
                QSet<int> plannedFillBeats;
                bool planValid =
                    !generatedRecipe.drumPhrases.isEmpty();
                for (const auto& phrase :
                     generatedRecipe.drumPhrases) {
                    planValid =
                        planValid &&
                        phrase.startBar == expectedStartBar &&
                        phrase.endBar >= phrase.startBar &&
                        !phrase.development.isEmpty() &&
                        !phrase.transition.isEmpty();
                    expectedStartBar = phrase.endBar + 1;
                    if (phrase.fillStartBeat < 0) continue;
                    ++plannedFillCount;
                    const int phraseFirstBeat =
                        (phrase.startBar - 1) *
                        generatedRecipe.beatsPerBar;
                    const int phraseEndBeat =
                        phrase.endBar *
                        generatedRecipe.beatsPerBar;
                    planValid =
                        planValid &&
                        phrase.fillBeatCount > 0 &&
                        phrase.fillStartBeat >=
                            phraseFirstBeat &&
                        phrase.fillStartBeat +
                                phrase.fillBeatCount ==
                            phraseEndBeat &&
                        phrase.fillBeatCount <=
                            generatedRecipe.beatsPerBar &&
                        !phrase.fillId.isEmpty();
                    for (int beat = phrase.fillStartBeat;
                         beat < phraseEndBeat;
                         ++beat) {
                        plannedFillBeats.insert(beat);
                    }
                }
                QHash<int, int> voicesAtTick;
                bool hasRealizedFill = false;
                for (const auto& event :
                     generatedRecipe.drumEvents) {
                    ++voicesAtTick[event.tick];
                    const bool planned =
                        plannedFillBeats.contains(
                            event.tick / 12);
                    planValid =
                        planValid &&
                        event.fill == planned;
                    hasRealizedFill =
                        hasRealizedFill || event.fill;
                }
                for (int voices : voicesAtTick) {
                    planValid =
                        planValid && voices <= 3;
                }
                planValid =
                    planValid &&
                    expectedStartBar ==
                        generatedRecipe.bars + 1 &&
                    plannedFillCount ==
                        generatedRecipe.fillCount &&
                    hasRealizedFill;
                if (!planValid &&
                    drumPhrasePlanDetail.isEmpty()) {
                    drumPhrasePlanDetail =
                        QStringLiteral(
                            "profile=%1 form=%2 bars=%3 "
                            "phrases=%4 fills=%5 expected_start=%6")
                            .arg(profile.id, form.id)
                            .arg(generatedRecipe.bars)
                            .arg(
                                generatedRecipe
                                    .drumPhrases.size())
                            .arg(
                                generatedRecipe.fillCount)
                            .arg(expectedStartBar);
                }
                drumPhrasePlansValid =
                    drumPhrasePlansValid &&
                    planValid;
                ++phraseFormIndex;
            }
            ++phraseProfileIndex;
        }
        for (const QString& styleId : ids) {
            jam2::practice::ChordIdeaRequest request;
            request.styleId = styleId;
            request.bars = 16;
            request.harmonicComplexity = 4;
            request.rhythmicComplexity = 4;
            const auto generated =
                jam2::practice::generateCoupledPracticeIdeaForTest(
                    request,
                    static_cast<std::uint32_t>(
                        50000 + ids.indexOf(styleId)));
            const auto& events = generated.recipe.drumEvents;
            const auto* generatedKit =
                jam2::practice::researchDrumKitForProfile(
                    generated.recipe.profileId);
            int previousTick = -1;
            bool hasFill = false;
            for (const auto& event : events) {
                const int laneIndex =
                    drumLaneIds.indexOf(event.laneId);
                QChar gridState;
                if (laneIndex >= 0) {
                    const int beatIndex = event.tick / 12;
                    const int tickInBeat = event.tick % 12;
                    const BeatPattern& pattern =
                        generated.beatSection.beatPatterns.at(
                            beatIndex);
                    const int scaledStep =
                        tickInBeat * pattern.division;
                    if (scaledStep % 12 == 0) {
                        const int step = scaledStep / 12;
                        if (laneIndex < pattern.lanes.size() &&
                            step <
                                pattern.lanes.at(
                                    laneIndex).size()) {
                            gridState =
                                pattern.lanes.at(
                                    laneIndex).at(step);
                        }
                    }
                }
                const auto* piece = generatedKit
                    ? jam2::practice::researchDrumPiece(
                          *generatedKit,
                          event.laneId)
                    : nullptr;
                const auto* band =
                    !piece
                    ? nullptr
                    : gridState == QLatin1Char('g')
                        ? &piece->ghost
                        : gridState == QLatin1Char('a')
                            ? &piece->accent
                            : gridState == QLatin1Char('x')
                                ? &piece->normal
                                : nullptr;
                performanceEventsValid =
                    performanceEventsValid &&
                    event.tick >= previousTick &&
                    event.tick <
                        generated.recipe.bars *
                        generated.recipe.beatsPerBar * 12 &&
                    event.velocity >= 1 && event.velocity <= 127 &&
                    event.offsetMs >= -40 && event.offsetMs <= 40 &&
                    !event.laneId.isEmpty() &&
                    !event.articulation.isEmpty() &&
                    !event.role.isEmpty() &&
                    band &&
                    event.velocity >= band->minimum &&
                    event.velocity <= band->maximum;
                previousTick = event.tick;
                hasFill = hasFill || event.fill;
            }
            QSet<QString> barFingerprints;
            for (int bar = 0; bar < generated.recipe.bars; ++bar) {
                QString fingerprint;
                const int firstBeat =
                    bar * generated.recipe.beatsPerBar;
                for (int beat = 0;
                     beat < generated.recipe.beatsPerBar;
                     ++beat) {
                    const BeatPattern& pattern =
                        generated.beatSection.beatPatterns.at(
                            firstBeat + beat);
                    fingerprint += QString::number(pattern.division);
                    fingerprint += QLatin1Char(':');
                    for (const QString& laneText : pattern.lanes) {
                        fingerprint += laneText;
                        fingerprint += QLatin1Char('/');
                    }
                    fingerprint += QLatin1Char('|');
                }
                barFingerprints.insert(fingerprint);
            }
            const bool styleFlowValid =
                generated.recipe.fillCount > 0 &&
                hasFill &&
                barFingerprints.size() >= 3;
            if (!styleFlowValid && fullFormFlowDetail.isEmpty()) {
                fullFormFlowDetail = QStringLiteral(
                    "style=%1 profile=%2 family=%3 fills=%4 "
                    "event_fill=%5 unique_bars=%6")
                    .arg(styleId)
                    .arg(generated.recipe.profileId)
                    .arg(generated.recipe.grooveId)
                    .arg(generated.recipe.fillCount)
                    .arg(hasFill)
                    .arg(barFingerprints.size());
            }
            fullFormFlowValid =
                fullFormFlowValid && styleFlowValid;
            if (styleId == QStringLiteral("rock")) {
                bool high = false;
                bool mid = false;
                bool floor = false;
                for (const auto& event : events) {
                    if (!event.fill) continue;
                    high = high ||
                        event.laneId ==
                            QStringLiteral("high_tom");
                    mid = mid ||
                        event.laneId ==
                            QStringLiteral("mid_tom");
                    floor = floor ||
                        event.laneId ==
                            QStringLiteral("floor_tom");
                }
                acousticThreeTomFill = high && mid && floor;
            }
        }
        record(
            QStringLiteral(
                "practice.v7-performance-events-are-bounded-and-articulated"),
            performanceEventsValid);
        record(
            QStringLiteral(
                "practice.researched-drum-catalog-covers-production-profiles"),
            researchedKitCatalogValid);
        record(
            QStringLiteral(
                "practice.researched-drum-voices-are-finite-and-audible"),
            researchedKitVoicesValid);
        record(
            QStringLiteral(
                "practice.trap-kick-retains-audible-sine-sub-layer"),
            trapSubLayerValid);
        record(
            QStringLiteral(
                "practice.v7-every-full-form-groove-develops-and-fills"),
            fullFormFlowValid,
            fullFormFlowDetail);
        record(
            QStringLiteral(
                "practice.v7-drum-phrase-plans-cover-all-profile-forms"),
            drumPhrasePlansValid,
            drumPhrasePlanDetail);
        record(
            QStringLiteral(
                "practice.v7-acoustic-fills-use-high-mid-and-floor-toms"),
            acousticThreeTomFill);
        record(QStringLiteral("practice.generation-uses-meter-grouped-click"),
            stableClickValid);
        record(QStringLiteral("metronome.pattern-labels-show-beat-and-subdivision"),
            metronomeStepLabel(0, 1) == QStringLiteral("1.1") &&
            metronomeStepLabel(3, 1) == QStringLiteral("4.1") &&
            metronomeStepLabel(0, 2) == QStringLiteral("1.1") &&
            metronomeStepLabel(1, 2) == QStringLiteral("1.3") &&
            metronomeStepLabel(2, 2) == QStringLiteral("2.1") &&
            metronomeStepLabel(3, 4) == QStringLiteral("1.4") &&
            metronomeStepLabel(4, 4) == QStringLiteral("2.1") &&
            metronomeStepLabel(2, 3) == QStringLiteral("1.3") &&
            metronomeStepLabel(3, 3) == QStringLiteral("2.1"));

        QSet<QString> motifForms;
        QSet<QString> motifCells;
        bool motifFollowsNativeForm = true;
        for (std::uint32_t seed = 0; seed < 48; ++seed) {
            jam2::practice::ChordIdeaRequest request;
            request.styleId = QStringLiteral("pop");
            request.bars = 16;
            const auto generated =
                jam2::practice::generateCoupledPracticeIdeaForTest(request, seed);
            motifForms.insert(generated.recipe.motifForm);
            motifCells.insert(generated.recipe.motifCell);
            QStringList expectedLabels;
            for (const auto& section : generated.recipe.formSections) {
                expectedLabels << section.label;
            }
            motifFollowsNativeForm = motifFollowsNativeForm &&
                generated.recipe.motifForm == expectedLabels.join(QLatin1Char('-'));
        }
        record(QStringLiteral("practice.v7-motif-follows-native-form-with-seeded-contour"),
            motifFollowsNativeForm && !motifForms.isEmpty() && motifCells.size() >= 3);

        jam2::practice::ChordIdeaRequest request;
        request.key = 5;
        request.styleId = QStringLiteral("modal-jam");
        request.profileId = QStringLiteral("modal_atmospheric");
        request.bars = 16;
        request.harmonicComplexity = 6;
        request.rhythmicComplexity = 6;
        const auto idea = jam2::practice::generateCoupledPracticeIdeaForTest(request, 0xffffffffU);
        BeatGridModel model;
        const int sectionIndex = model.replaceGeneratedSection(QStringLiteral("chord"), idea.chordSection);
        BeatGridModel loaded;
        const QJsonObject serializedModel = model.toJson();
        const QByteArray serializedRecipeText =
            QJsonDocument(serializedModel).toJson(QJsonDocument::Compact);
        const bool loadedRecipe = loaded.loadJson(serializedModel);
        record(QStringLiteral("practice.v7-recipe-roundtrip-and-role-layers"),
            sectionIndex == 0 && loadedRecipe && loaded.section(0).generatedRecipe.seed == 0xffffffffU &&
            loaded.section(0).generatedRecipe.generatorVersion == 7 &&
            loaded.section(0).generatedRecipe.profileId == idea.recipe.profileId &&
            loaded.section(0).generatedRecipe.variationId == idea.recipe.variationId &&
            loaded.section(0).generatedRecipe.progressionId == idea.recipe.progressionId &&
            loaded.section(0).generatedRecipe.grooveId == idea.recipe.grooveId &&
            loaded.section(0).generatedRecipe.beatUnit == idea.recipe.beatUnit &&
            loaded.section(0).generatedRecipe.tempoPulseUnits == idea.recipe.tempoPulseUnits &&
            loaded.section(0).generatedRecipe.swingPercent == idea.recipe.swingPercent &&
            loaded.section(0).generatedRecipe.kickVariationCount == idea.recipe.kickVariationCount &&
            loaded.section(0).generatedRecipe.drumEvents.size() ==
                idea.recipe.drumEvents.size() &&
            loaded.section(0).generatedRecipe.drumPatchRevision ==
                idea.recipe.drumPatchRevision &&
            loaded.section(0).generatedRecipe.drumMixGainDb ==
                idea.recipe.drumMixGainDb &&
            loaded.section(0).generatedRecipe.bassEvents.size() == idea.recipe.bassEvents.size() &&
            loaded.section(0).generatedRecipe.synthVoices.size() == idea.recipe.synthVoices.size() &&
            loaded.section(0).generatedRecipe.formSections.size() == idea.recipe.formSections.size() &&
            loaded.section(0).generatedRecipe.automationEvents.size() == idea.recipe.automationEvents.size() &&
            !loaded.section(0).generatedRecipe.formSections.isEmpty() &&
            !loaded.section(0).generatedRecipe.automationEvents.isEmpty() &&
            loaded.section(0).name.startsWith(QStringLiteral("F ")) &&
            !loaded.section(0).name.contains(QStringLiteral("Key -")));
        record(QStringLiteral("practice.v7-recipe-has-no-mood-or-character-records"),
            !serializedRecipeText.contains("\"mood\"") &&
            !serializedRecipeText.contains("\"character\""));

        jam2::practice::ChordIdeaRequest modeRequest;
        modeRequest.styleId = QStringLiteral("modal-jam");
        modeRequest.profileId = QStringLiteral("modal_atmospheric");
        modeRequest.modeId = QStringLiteral("lydian");
        const auto requestedMode =
            jam2::practice::generateCoupledPracticeIdeaForTest(modeRequest, 91);
        record(QStringLiteral("practice.v7-manual-mode-is-profile-filtered-and-applied"),
            jam2::practice::modeIds(modeRequest.profileId).contains(modeRequest.modeId) &&
            requestedMode.recipe.mode == QStringLiteral("Lydian"));

        record(QStringLiteral("practice.key-picker-uses-twelve-canonical-pitch-classes"),
            jam2::practice::keyNames() ==
                QStringList{
                    QStringLiteral("C"), QStringLiteral("C#"),
                    QStringLiteral("D"), QStringLiteral("D#"),
                    QStringLiteral("E"), QStringLiteral("F"),
                    QStringLiteral("F#"), QStringLiteral("G"),
                    QStringLiteral("G#"), QStringLiteral("A"),
                    QStringLiteral("A#"), QStringLiteral("B")});
        const auto roleNotesAvoid = [](
                                       const jam2::practice::GenerationRecipe& recipe,
                                       QLatin1Char forbidden) {
            const auto avoids = [forbidden](const QString& note) {
                return note.size() < 2 || note.at(1) != forbidden;
            };
            return std::all_of(
                       recipe.melodyEvents.cbegin(),
                       recipe.melodyEvents.cend(),
                       [&avoids](const auto& event) {
                           return avoids(event.note);
                       }) &&
                std::all_of(
                       recipe.bassEvents.cbegin(),
                       recipe.bassEvents.cend(),
                       [&avoids](const auto& event) {
                           return avoids(event.note);
                       }) &&
                std::all_of(
                       recipe.supportingEvents.cbegin(),
                       recipe.supportingEvents.cend(),
                       [&avoids](const auto& event) {
                           return avoids(event.note);
                       });
        };
        jam2::practice::ChordIdeaRequest gMinorSpellingRequest;
        gMinorSpellingRequest.key = 7;
        gMinorSpellingRequest.styleId = QStringLiteral("pop");
        gMinorSpellingRequest.profileId = QStringLiteral("pop_loop");
        gMinorSpellingRequest.modeId = QStringLiteral("aeolian");
        gMinorSpellingRequest.harmonicComplexity = 1;
        gMinorSpellingRequest.rhythmicComplexity = 1;
        const auto gMinorSpelling =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                gMinorSpellingRequest, 318491U);
        jam2::practice::ChordIdeaRequest cSharpMinorSpellingRequest =
            gMinorSpellingRequest;
        cSharpMinorSpellingRequest.key = 1;
        const auto cSharpMinorSpelling =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                cSharpMinorSpellingRequest, 318491U);
        record(QStringLiteral("practice.generated-note-spelling-follows-key-and-mode"),
            gMinorSpelling.recipe.tonic == QStringLiteral("G") &&
            gMinorSpelling.recipe.mode == QStringLiteral("Natural Minor") &&
            roleNotesAvoid(gMinorSpelling.recipe, QLatin1Char('#')) &&
            cSharpMinorSpelling.recipe.tonic == QStringLiteral("C#") &&
            cSharpMinorSpelling.recipe.mode == QStringLiteral("Natural Minor") &&
            roleNotesAvoid(cSharpMinorSpelling.recipe, QLatin1Char('b')));

        jam2::practice::ChordIdeaRequest swingTwoFeelRequest;
        swingTwoFeelRequest.styleId = QStringLiteral("jazz");
        swingTwoFeelRequest.profileId =
            QStringLiteral("jazz_swing_standards");
        swingTwoFeelRequest.formId =
            QStringLiteral("jazz-aaba-32");
        swingTwoFeelRequest.harmonicComplexity = 1;
        swingTwoFeelRequest.rhythmicComplexity = 1;
        const auto swingTwoFeel =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                swingTwoFeelRequest, 1473189102U);
        record(QStringLiteral("practice.jazz-two-feel-uses-sustained-half-note-bass"),
            swingTwoFeel.recipe.grooveId ==
                QStringLiteral("jazz-two-feel") &&
            swingTwoFeel.recipe.bassEvents.size() ==
                swingTwoFeel.recipe.bars * 2 &&
            std::all_of(
                swingTwoFeel.recipe.bassEvents.cbegin(),
                swingTwoFeel.recipe.bassEvents.cend(),
                [](const auto& event) {
                    return event.durationTicks == 24;
                }));

        jam2::practice::ChordIdeaRequest bebopRequest;
        bebopRequest.styleId = QStringLiteral("jazz");
        bebopRequest.profileId = QStringLiteral("jazz_bebop");
        bebopRequest.formId = QStringLiteral("bebop-32");
        bebopRequest.harmonicComplexity = 1;
        bebopRequest.rhythmicComplexity = 1;
        const auto bebop =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bebopRequest, 3888487240U);
        jam2::practice::ChordIdeaRequest bebopTwentyRequest =
            bebopRequest;
        bebopTwentyRequest.formId =
            QStringLiteral("bebop-20");
        const auto bebopTwenty =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bebopTwentyRequest, 3888487240U);
        record(QStringLiteral("practice.bebop-uses-fast-functional-motion-and-native-form"),
            bebop.recipe.baseHarmony.size() ==
                bebop.recipe.bars * 2 &&
            std::all_of(
                bebop.recipe.baseHarmony.cbegin(),
                bebop.recipe.baseHarmony.cend(),
                [](const auto& event) {
                    return event.durationBeats == 2;
                }) &&
            bebopTwenty.recipe.formSections.size() == 4 &&
            bebopTwenty.recipe.formSections.at(0).bars == 4 &&
            bebopTwenty.recipe.formSections.at(1).bars == 5 &&
            bebopTwenty.recipe.formSections.at(2).bars == 4 &&
            bebopTwenty.recipe.formSections.at(3).bars == 7);

        jam2::practice::ChordIdeaRequest fusionRequest;
        fusionRequest.styleId = QStringLiteral("jazz");
        fusionRequest.profileId = QStringLiteral("jazz_fusion");
        fusionRequest.formId = QStringLiteral("fusion-14");
        fusionRequest.harmonicComplexity = 1;
        fusionRequest.rhythmicComplexity = 1;
        const auto fusion =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                fusionRequest, 2242922740U);
        QSet<int> fusionVampRoots;
        QSet<int> fusionChangesRoots;
        QSet<int> fusionReturnRoots;
        for (const auto& event : fusion.recipe.baseHarmony) {
            const auto parsed =
                jam2::practice::parseChord(event.chord);
            if (!parsed.valid) continue;
            if (event.beat < 4 * 7)
                fusionVampRoots.insert(parsed.root);
            else if (event.beat < 10 * 7)
                fusionChangesRoots.insert(parsed.root);
            else
                fusionReturnRoots.insert(parsed.root);
        }
        record(QStringLiteral("practice.fusion-separates-straight-vamp-changes-and-return"),
            fusion.recipe.formSections.size() == 3 &&
            fusion.recipe.formSections.at(0).bars == 4 &&
            fusion.recipe.formSections.at(1).bars == 6 &&
            fusion.recipe.formSections.at(2).bars == 4 &&
            std::all_of(
                fusion.chordSection.musicalPatterns.cbegin(),
                fusion.chordSection.musicalPatterns.cend(),
                [](const MusicalBeatPattern& pattern) {
                    return pattern.division == 4;
                }) &&
            fusionVampRoots.size() == 2 &&
            fusionChangesRoots.size() >= 3 &&
            fusionReturnRoots == fusionVampRoots);

        const auto baseChordList = [](const jam2::practice::GeneratedPracticeIdea& generated) {
            QStringList result;
            for (const auto& event : generated.recipe.baseHarmony) {
                result << event.chord;
            }
            return result;
        };
        jam2::practice::ChordIdeaRequest popHarmonyRequest;
        popHarmonyRequest.key = 8;
        popHarmonyRequest.styleId = QStringLiteral("pop");
        popHarmonyRequest.profileId = QStringLiteral("pop_loop");
        popHarmonyRequest.formId = QStringLiteral("pop-loop-8");
        popHarmonyRequest.harmonicComplexity = 1;
        popHarmonyRequest.rhythmicComplexity = 1;
        const auto popHarmony =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                popHarmonyRequest, 1528705262U);
        const QStringList popBase = baseChordList(popHarmony);

        jam2::practice::ChordIdeaRequest trapHarmonyRequest;
        trapHarmonyRequest.key = 2;
        trapHarmonyRequest.styleId = QStringLiteral("hiphop-trap");
        trapHarmonyRequest.profileId = QStringLiteral("hiphop_trap");
        trapHarmonyRequest.formId = QStringLiteral("trap-16");
        trapHarmonyRequest.harmonicComplexity = 1;
        trapHarmonyRequest.rhythmicComplexity = 1;
        const auto trapHarmony =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                trapHarmonyRequest, 1204219440U);
        const QStringList trapBase = baseChordList(trapHarmony);

        jam2::practice::ChordIdeaRequest bluesHarmonyRequest;
        bluesHarmonyRequest.key = 5;
        bluesHarmonyRequest.styleId = QStringLiteral("blues");
        bluesHarmonyRequest.profileId = QStringLiteral("blues_dominant");
        bluesHarmonyRequest.formId = QStringLiteral("blues-12");
        bluesHarmonyRequest.harmonicComplexity = 1;
        bluesHarmonyRequest.rhythmicComplexity = 1;
        const auto bluesHarmony =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bluesHarmonyRequest, 2223768394U);
        const QStringList bluesBase = baseChordList(bluesHarmony);
        const auto bluesIv = std::find_if(
            bluesHarmony.recipe.baseHarmony.cbegin(),
            bluesHarmony.recipe.baseHarmony.cend(),
            [](const auto& event) {
                return event.roman == QStringLiteral("IV7");
            });
        const bool bluesUsesFlatIvSpelling =
            bluesIv !=
                bluesHarmony.recipe.baseHarmony.cend() &&
            bluesIv->chord == QStringLiteral("Bb7");
        QSet<int> dominantBluesMelodyDegrees;
        for (const auto& event :
             bluesHarmony.recipe.melodyEvents) {
            dominantBluesMelodyDegrees.insert(
                ((event.midi - bluesHarmonyRequest.key) %
                     12 +
                 12) %
                12);
        }
        record(QStringLiteral(
            "practice.dominant-blues-melody-retains-major-minor-third-friction"),
            bluesHarmony.recipe.mode ==
                    QStringLiteral("Dominant Blues") &&
                dominantBluesMelodyDegrees.contains(3) &&
                dominantBluesMelodyDegrees.contains(4));

        bool foundOpenBluesEnding = false;
        bool foundClosedBluesEnding = false;
        for (std::uint32_t endingSeed = 1;
             endingSeed <= 24;
             ++endingSeed) {
            const auto ending =
                jam2::practice::
                    generateCoupledPracticeIdeaForTest(
                        bluesHarmonyRequest,
                        endingSeed);
            if (ending.recipe.baseHarmony.isEmpty())
                continue;
            const auto parsed =
                jam2::practice::parseChord(
                    ending.recipe.baseHarmony.back()
                        .chord);
            if (!parsed.valid) continue;
            const int relative =
                ((parsed.root -
                      bluesHarmonyRequest.key) %
                     12 +
                 12) %
                12;
            foundClosedBluesEnding |= relative == 0;
            foundOpenBluesEnding |= relative == 7;
        }
        record(QStringLiteral(
            "practice.blues-seeds-expose-both-tonic-close-and-open-dominant-endings"),
            foundClosedBluesEnding &&
                foundOpenBluesEnding);

        jam2::practice::ChordIdeaRequest compoundMinorRequest;
        compoundMinorRequest.key = 0;
        compoundMinorRequest.styleId =
            QStringLiteral("blues");
        compoundMinorRequest.profileId =
            QStringLiteral("blues_minor");
        compoundMinorRequest.formId =
            QStringLiteral("minor-blues-12-8");
        compoundMinorRequest.harmonicComplexity = 1;
        compoundMinorRequest.rhythmicComplexity = 1;
        const auto compoundMinor =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    compoundMinorRequest,
                    334821771U);
        const bool compoundBassOnPulse =
            compoundMinor.recipe.bassEvents.size() ==
                12 * 4 &&
            std::all_of(
                compoundMinor.recipe.bassEvents.cbegin(),
                compoundMinor.recipe.bassEvents.cend(),
                [](const auto& event) {
                    const int eighth =
                        event.tick / 12;
                    return eighth % 12 % 3 == 0;
                });
        const bool foundationalMinorHarmony =
            std::none_of(
                compoundMinor.recipe.baseHarmony.cbegin(),
                compoundMinor.recipe.baseHarmony.cend(),
                [](const auto& event) {
                    return event.roman.startsWith(
                               QStringLiteral("iiø")) ||
                        event.roman.startsWith(
                            QStringLiteral("V7"));
                });
        record(QStringLiteral(
            "practice.compound-minor-blues-bass-follows-dotted-quarter-pulse"),
            compoundMinor.recipe.beatsPerBar == 12 &&
                compoundMinor.recipe.tempoPulseUnits == 3 &&
                compoundBassOnPulse &&
                foundationalMinorHarmony);

        jam2::practice::ChordIdeaRequest phrygianHarmonyRequest;
        phrygianHarmonyRequest.key = 2;
        phrygianHarmonyRequest.styleId = QStringLiteral("modal-jam");
        phrygianHarmonyRequest.profileId = QStringLiteral("modal_atmospheric");
        phrygianHarmonyRequest.formId = QStringLiteral("modal-atmospheric-12");
        phrygianHarmonyRequest.modeId = QStringLiteral("phrygian");
        phrygianHarmonyRequest.harmonicComplexity = 1;
        phrygianHarmonyRequest.rhythmicComplexity = 1;
        const auto phrygianHarmony =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                phrygianHarmonyRequest, 1527951245U);
        const QStringList phrygianBase =
            baseChordList(phrygianHarmony);
        const bool phrygianPedalValid =
            phrygianHarmony.recipe.baseHarmony.size() == 3 &&
            std::all_of(
                phrygianHarmony.recipe.baseHarmony.cbegin(),
                phrygianHarmony.recipe.baseHarmony.cend(),
                [](const auto& event) {
                    const auto chord =
                        jam2::practice::parseChord(
                            event.chord);
                    return chord.valid &&
                        (chord.bass >= 0
                             ? chord.bass
                             : chord.root) == 2;
                }) &&
            phrygianHarmony.recipe.bassEvents.size() == 3 &&
            std::all_of(
                phrygianHarmony.recipe.bassEvents.cbegin(),
                phrygianHarmony.recipe.bassEvents.cend(),
                [](const auto& event) {
                    return event.midi % 12 == 2 &&
                        event.durationTicks == 144;
                }) &&
            phrygianHarmony.recipe.supportingEvents.size() ==
                1 &&
            phrygianHarmony.recipe.supportingEvents.front()
                    .midi %
                    12 ==
                2 &&
            phrygianHarmony.recipe.supportingEvents.front()
                    .durationTicks ==
                432;
        record(QStringLiteral(
            "practice.modal-atmospheric-retains-one-tonic-pedal-under-flat-degree-colours"),
            phrygianPedalValid);

        jam2::practice::ChordIdeaRequest lydianExtensionRequest;
        lydianExtensionRequest.key = 2;
        lydianExtensionRequest.styleId =
            QStringLiteral("modal-jam");
        lydianExtensionRequest.profileId =
            QStringLiteral("modal_atmospheric");
        lydianExtensionRequest.formId =
            QStringLiteral("modal-atmospheric-12");
        lydianExtensionRequest.modeId =
            QStringLiteral("lydian");
        lydianExtensionRequest.harmonicComplexity = 8;
        lydianExtensionRequest.rhythmicComplexity = 8;
        const auto lydianExtension =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                lydianExtensionRequest, 2389050187U);
        record(QStringLiteral(
            "practice.mode-derived-extensions-stack-sevenths-and-ninths-inside-active-mode"),
            lydianExtension.recipe.mode ==
                QStringLiteral("Lydian") &&
            lydianExtension.recipe.baseHarmony.size() == 3 &&
            lydianExtension.recipe.baseHarmony.at(1).chord ==
                QStringLiteral("E/D") &&
            lydianExtension.recipe.finalChordPlan.contains(
                QStringLiteral("5:1 E9/D")) &&
            lydianExtension.recipe.finalChordPlan.contains(
                QStringLiteral("9:1 Dmaj9")) &&
            lydianExtension.recipe.theoryDecisions.size() ==
                2 &&
            std::all_of(
                lydianExtension.recipe.theoryDecisions.cbegin(),
                lydianExtension.recipe.theoryDecisions.cend(),
                [](const auto& decision) {
                    return decision.kind ==
                        QStringLiteral(
                            "diatonic-extension");
                }));

        jam2::practice::ChordIdeaRequest bossaHarmonyRequest;
        bossaHarmonyRequest.key = 2;
        bossaHarmonyRequest.styleId = QStringLiteral("bossa-nova");
        bossaHarmonyRequest.profileId = QStringLiteral("bossa_songbook");
        bossaHarmonyRequest.formId = QStringLiteral("bossa-aaba-32");
        bossaHarmonyRequest.harmonicComplexity = 1;
        bossaHarmonyRequest.rhythmicComplexity = 1;
        const auto bossaHarmony =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bossaHarmonyRequest, 1754303273U);
        const QStringList bossaBase = baseChordList(bossaHarmony);

        const bool romanReferenceValid =
            popHarmony.recipe.mode == QStringLiteral("Major") &&
            popBase.mid(0, 4) ==
                QStringList{
                    QStringLiteral("Ab"),
                    QStringLiteral("Eb"),
                    QStringLiteral("Fm"),
                    QStringLiteral("Db")} &&
            trapBase.mid(0, 4) ==
                QStringList{
                    QStringLiteral("Dm"),
                    QStringLiteral("Bb"),
                    QStringLiteral("C"),
                    QStringLiteral("Dm")} &&
            bluesUsesFlatIvSpelling &&
            phrygianHarmony.recipe.progressionId ==
                QStringLiteral("modal-phrygian") &&
            phrygianBase.size() >= 2 &&
            phrygianBase.at(1) == QStringLiteral("Eb/D") &&
            bossaHarmony.recipe.mode == QStringLiteral("Major") &&
            bossaBase.mid(0, 4) ==
                QStringList{
                    QStringLiteral("Em7"),
                    QStringLiteral("A7"),
                    QStringLiteral("Dmaj7"),
                    QStringLiteral("Bm7")};
        record(QStringLiteral(
            "practice.v7-roman-degrees-use-one-major-scale-reference"),
            romanReferenceValid,
            romanReferenceValid ? QString() : QStringLiteral(
                "pop=%1 trap=%2 blues=%3 phrygian=%4/%5 bossa=%6")
                .arg(popBase.mid(0, 4).join(QLatin1Char(',')))
                .arg(trapBase.mid(0, 4).join(QLatin1Char(',')))
                .arg(bluesBase.mid(0, 4).join(QLatin1Char(',')))
                .arg(phrygianHarmony.recipe.progressionId)
                .arg(phrygianBase.mid(0, 4).join(QLatin1Char(',')))
                .arg(bossaBase.mid(0, 4).join(QLatin1Char(','))));

        bool earlierRecipesRejected = true;
        for (int version = 1; version <= 6; ++version) {
            QJsonObject earlier = model.toJson();
            QJsonArray sections = earlier.value(QStringLiteral("sections")).toArray();
            QJsonObject section = sections.first().toObject();
            QJsonObject recipe =
                section.value(QStringLiteral("generated_recipe")).toObject();
            recipe[QStringLiteral("generator_version")] = version;
            section[QStringLiteral("generated_recipe")] = recipe;
            sections[0] = section;
            earlier[QStringLiteral("sections")] = sections;
            BeatGridModel rejected;
            earlierRecipesRejected =
                earlierRecipesRejected && !rejected.loadJson(earlier);
        }
        record(QStringLiteral("practice.v7-rejects-all-earlier-generated-recipes"),
            earlierRecipesRejected);

        QJsonObject invalidFeel = model.toJson();
        QJsonArray invalidFeelSections = invalidFeel.value(QStringLiteral("sections")).toArray();
        QJsonObject invalidFeelSection = invalidFeelSections.first().toObject();
        QJsonObject invalidFeelRecipe = invalidFeelSection.value(QStringLiteral("generated_recipe")).toObject();
        QJsonObject invalidGroove = invalidFeelRecipe.value(QStringLiteral("groove")).toObject();
        invalidGroove[QStringLiteral("swing_percent")] = 68;
        invalidFeelRecipe[QStringLiteral("groove")] = invalidGroove;
        invalidFeelSection[QStringLiteral("generated_recipe")] = invalidFeelRecipe;
        invalidFeelSections[0] = invalidFeelSection;
        invalidFeel[QStringLiteral("sections")] = invalidFeelSections;
        BeatGridModel rejectedFeel;
        record(QStringLiteral("practice.v7-out-of-range-groove-feel-is-rejected"),
            !rejectedFeel.loadJson(invalidFeel));

        QJsonObject oldGenerated = BeatGridModel{}.toJson();
        QJsonArray oldSections = oldGenerated.value(QStringLiteral("sections")).toArray();
        QJsonObject oldSection = oldSections.first().toObject();
        oldSection[QStringLiteral("generated_kind")] = QStringLiteral("chord");
        oldSection[QStringLiteral("generated_style")] = QStringLiteral("Modern Metal");
        oldSections[0] = oldSection;
        oldGenerated[QStringLiteral("sections")] = oldSections;
        BeatGridModel rejectedV1;
        record(QStringLiteral("practice.v1-generated-sections-are-rejected"), !rejectedV1.loadJson(oldGenerated));

        QJsonObject legacyBeatLanes = BeatGridModel{}.toJson();
        legacyBeatLanes.remove(QStringLiteral("beat_lane_schema"));
        QJsonArray legacySections =
            legacyBeatLanes.value(QStringLiteral("sections")).toArray();
        QJsonObject legacySection = legacySections.first().toObject();
        QJsonArray legacyPatterns =
            legacySection.value(QStringLiteral("beat_patterns")).toArray();
        for (int patternIndex = 0; patternIndex < legacyPatterns.size();
             ++patternIndex) {
            QJsonObject legacyPattern =
                legacyPatterns.at(patternIndex).toObject();
            legacyPattern[QStringLiteral("lanes")] =
                patternIndex == 0
                ? QJsonArray{
                      QStringLiteral("k"),
                      QStringLiteral("s"),
                      QStringLiteral("c"),
                      QStringLiteral("o"),
                      QStringLiteral("r"),
                      QStringLiteral("z"),
                      QStringLiteral("t"),
                      QStringLiteral("x"),
                      QStringLiteral("h"),
                      QStringLiteral("p"),
                  }
                : QJsonArray{
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                      QString(),
                  };
            legacyPatterns[patternIndex] = legacyPattern;
        }
        legacySection[QStringLiteral("beat_patterns")] = legacyPatterns;
        legacySections[0] = legacySection;
        legacyBeatLanes[QStringLiteral("sections")] = legacySections;
        BeatGridModel migratedBeatLanes;
        const bool migratedLegacyBeatLanes =
            migratedBeatLanes.loadJson(legacyBeatLanes);
        const QVector<QString> migrated =
            migratedLegacyBeatLanes
                ? migratedBeatLanes.section(0).beatPatterns.at(0).lanes
                : QVector<QString>{};
        record(
            QStringLiteral(
                "practice.beat-lane-v1-migrates-generic-tom-and-percussion-by-identity"),
            migrated.size() == 12 &&
                migrated.value(0) == QStringLiteral("k") &&
                migrated.value(5) == QStringLiteral("z") &&
                migrated.value(6).isEmpty() &&
                migrated.value(7) == QStringLiteral("t") &&
                migrated.value(8).isEmpty() &&
                migrated.value(9) == QStringLiteral("x") &&
                migrated.value(10) == QStringLiteral("h") &&
                migrated.value(11) == QStringLiteral("p"));

        QJsonObject invalidBeatLaneSchema = BeatGridModel{}.toJson();
        invalidBeatLaneSchema[QStringLiteral("beat_lane_schema")] = 3;
        BeatGridModel rejectedBeatLaneSchema;
        record(
            QStringLiteral(
                "practice.beat-lane-schema-rejects-unknown-future-version"),
            !rejectedBeatLaneSchema.loadJson(invalidBeatLaneSchema));

        record(QStringLiteral("practice.beat-view-uses-rendered-ride-lane"),
            BeatGridModel::beatLaneSchemaVersion() == 2 &&
            BeatGridModel{}.toJson()
                .value(QStringLiteral("beat_lane_schema")).toInt() == 2 &&
            BeatGridModel::beatLaneNames() == QStringList{
                QStringLiteral("Kick"),
                QStringLiteral("Snare"),
                QStringLiteral("Closed HH"),
                QStringLiteral("Open HH"),
                QStringLiteral("Ride"),
                QStringLiteral("Crash"),
                QStringLiteral("High Tom"),
                QStringLiteral("Mid Tom"),
                QStringLiteral("Floor Tom"),
                QStringLiteral("Cross-stick / Rim"),
                QStringLiteral("Shaker"),
                QStringLiteral("Hand Percussion"),
            } &&
            BeatGridModel::beatVisualLaneNames() == QStringList{
                QStringLiteral("Hand Percussion"),
                QStringLiteral("Shaker"),
                QStringLiteral("Cross-stick / Rim"),
                QStringLiteral("Floor Tom"),
                QStringLiteral("Mid Tom"),
                QStringLiteral("High Tom"),
                QStringLiteral("Crash"),
                QStringLiteral("Ride"),
                QStringLiteral("Open HH"),
                QStringLiteral("Closed HH"),
                QStringLiteral("Snare"),
                QStringLiteral("Kick"),
            });
        record(QStringLiteral("practice.shared-musical-division-uses-drum-names"),
            BeatGridModel::musicalDivisionValues() == QList<int>{1, 2, 4, 3, 6} &&
            BeatGridModel::musicalDivisionLabel(1) == QStringLiteral("Quarter") &&
            BeatGridModel::musicalDivisionLabel(2) == QStringLiteral("Eighth") &&
            BeatGridModel::musicalDivisionLabel(4) == QStringLiteral("16th") &&
            BeatGridModel::musicalDivisionLabel(3) == QStringLiteral("Triplet") &&
            BeatGridModel::musicalDivisionLabel(6) == QStringLiteral("6th"));

        jam2::practice::ReferenceRenderSettings settings;
        settings.sampleRate = 8000;
        settings.bpm = idea.bpm;
        settings.renderMelody = true;
        settings.renderBass = true;
        settings.renderSupport = true;
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/practice-v5-reference-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto rendered = jam2::practice::renderPracticeReferences(
            &idea.chordSection, &idea.beatSection, settings, workspace);
        const auto renderedAgain = jam2::practice::renderPracticeReferences(
            &idea.chordSection, &idea.beatSection, settings, workspace);
        const QString renderDetail = QStringLiteral(
            "error=%1 chord_peak=%2 drum_peak=%3 melody_peak=%4 "
            "bass_peak=%5 support_peak=%6 deterministic_drums=%7")
            .arg(rendered.error)
            .arg(rendered.chords.peak)
            .arg(rendered.drums.peak)
            .arg(rendered.melody.peak)
            .arg(rendered.bass.peak)
            .arg(rendered.support.peak)
            .arg(rendered.drums.sha256 ==
                renderedAgain.drums.sha256);
        record(QStringLiteral("practice.v7-role-separated-procedural-patches-render"),
            rendered.error.isEmpty() && rendered.chords.peak > 0.001f && rendered.drums.peak > 0.001f &&
            rendered.melody.peak > 0.001f && rendered.bass.peak > 0.001f &&
            rendered.support.peak > 0.001f && rendered.chords.peak < 4.0f &&
            renderedAgain.error.isEmpty() && rendered.drums.sha256 == renderedAgain.drums.sha256 &&
            rendered.bass.sha256 == renderedAgain.bass.sha256 &&
            rendered.support.sha256 == renderedAgain.support.sha256 &&
            rendered.bass.frames == rendered.chords.frames &&
            rendered.support.frames == rendered.chords.frames &&
            rendered.drums.preMakeupPeak > 0.001f &&
            rendered.drums.makeupGainDb ==
                jam2::practice::kGeneratedDrumStemMakeupDb &&
            rendered.drums.peak <=
                jam2::practice::kGeneratedDrumSoftLimitCeiling +
                    0.0001 &&
            rendered.diagnostics.contains(idea.recipe.chordPatchId) &&
            rendered.diagnostics.contains(idea.recipe.bassPatchId) &&
            rendered.diagnostics.contains(idea.recipe.supportPatchId) &&
            rendered.diagnostics.contains(QStringLiteral("groove=") + idea.recipe.grooveId) &&
            rendered.diagnostics.contains(QStringLiteral("swing_percent=")) &&
            rendered.diagnostics.contains(QStringLiteral("elapsed_ms=")),
            renderDetail);
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
        BeatGridModel model;
        model.resizeSection(0, 4);
        model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
        model.setCell(0, QStringLiteral("target"), 0, QStringLiteral("E4"));
        model.setBeatHit(0, 0, 0, QStringLiteral("x..."));
        const auto layers =
            jam2::practice::PracticeIdeaController::referenceLayers(model.section(0));
        jam2::practice::ReferenceRenderSettings settings;
        settings.sampleRate = 8000;
        settings.bpm = 240.0;
        settings.renderChords = true;
        settings.renderDrums = true;
        settings.renderMelody = true;
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/manual-reference-test-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const bool workspaceReady = QDir().mkpath(workspace);
        const jam2::practice::ReferenceRenderResult rendered =
            jam2::practice::renderPracticeReferences(
                &model.section(0), &model.section(0), settings, workspace);
        const jam2::wav::InspectResult chordWav = rendered.chords.path.isEmpty()
            ? jam2::wav::InspectResult{} : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.chords.path), 1024ULL * 1024ULL);
        const jam2::wav::InspectResult drumWav = rendered.drums.path.isEmpty()
            ? jam2::wav::InspectResult{} : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.drums.path), 1024ULL * 1024ULL);
        const jam2::wav::InspectResult melodyWav = rendered.melody.path.isEmpty()
            ? jam2::wav::InspectResult{} : jam2::wav::inspect_pcm16_file(
                nativeFilePath(rendered.melody.path), 1024ULL * 1024ULL);
        record(QStringLiteral("practice.manual-section-renders-all-entered-layers"),
            workspaceReady && layers.chords && layers.drums && layers.melody &&
            rendered.error.isEmpty() &&
            rendered.chords.eventCount > 0 &&
            rendered.drums.eventCount > 0 &&
            rendered.melody.eventCount > 0 &&
            chordWav && drumWav && melodyWav &&
            pcm16WavHasSignal(rendered.chords.path, chordWav.info) &&
            pcm16WavHasSignal(rendered.drums.path, drumWav.info) &&
            pcm16WavHasSignal(rendered.melody.path, melodyWav.info),
            rendered.error);
        (void)QDir(workspace).removeRecursively();
    }
    {
        LooperProject project;
        LooperLane custom;
        custom.id = QStringLiteral("custom-take");
        custom.name = QStringLiteral("Custom recorded take");
        const bool customApplied = project.appendLane(0, custom);
        LooperLane oldPeerReference;
        oldPeerReference.id = QStringLiteral("old-peer-reference");
        oldPeerReference.name = QStringLiteral("Peer Practice Chords");
        oldPeerReference.referenceKind = QStringLiteral("chord");
        oldPeerReference.localOnly = false;
        const bool peerApplied = project.appendLane(3, oldPeerReference);

        jam2::practice::PracticeIdeaController::clearReferences(project);
        jam2::practice::ReferenceRenderResult rendered;
        rendered.chords = {
            QStringLiteral("test-fixtures/section-a-chords.wav"),
            QString(64, QLatin1Char('a')),
            48000};
        rendered.drums = {
            QStringLiteral("test-fixtures/section-b-drums.wav"),
            QString(64, QLatin1Char('b')),
            48000};
        rendered.sourceSignature = QString(64, QLatin1Char('c'));
        jam2::practice::ReferenceRenderSettings chordSettings;
        chordSettings.renderChords = true;
        chordSettings.renderDrums = false;
        chordSettings.renderMelody = false;
        jam2::practice::ReferenceRenderSettings drumSettings = chordSettings;
        drumSettings.renderChords = false;
        drumSettings.renderDrums = true;
        QString chordError;
        QString drumError;
        const bool chordApplied =
            jam2::practice::PracticeIdeaController::applyReferences(
                project, 0, chordSettings, rendered, chordError, QStringLiteral("Section A"));
        const bool drumApplied =
            jam2::practice::PracticeIdeaController::applyReferences(
                project, 1, drumSettings, rendered, drumError, QStringLiteral("Section B"));
        record(QStringLiteral("practice.sections-map-to-banks-and-preserve-custom-tracks"),
            customApplied && peerApplied && chordApplied && drumApplied &&
            chordError.isEmpty() && drumError.isEmpty() &&
            project.banks().at(0).lanes.size() == 2 &&
            project.banks().at(0).lanes.at(0).id == QStringLiteral("custom-take") &&
            project.banks().at(0).lanes.at(1).name == QStringLiteral("Section A Chords") &&
            project.banks().at(1).lanes.size() == 1 &&
            project.banks().at(1).lanes.front().name == QStringLiteral("Section B Drums") &&
            project.banks().at(1).lanes.front().gainDb ==
                jam2::practice::kGeneratedDrumLaneGainDb &&
            project.banks().at(2).lanes.isEmpty() &&
            project.banks().at(3).lanes.isEmpty());
    }
    {
        LooperProject project;
        LooperLane existingDrums;
        existingDrums.id = QStringLiteral("existing-generated-drums");
        existingDrums.name = QStringLiteral("Practice Drums");
        existingDrums.referenceKind = QStringLiteral("drum");
        existingDrums.gainDb = 10.0;
        const bool appended = project.appendLane(0, existingDrums);
        jam2::practice::ReferenceRenderResult rendered;
        rendered.drums = {
            QStringLiteral("test-fixtures/replaced-drums.wav"),
            QString(64, QLatin1Char('d')),
            48000};
        rendered.sourceSignature = QString(64, QLatin1Char('e'));
        jam2::practice::ReferenceRenderSettings settings;
        settings.renderChords = false;
        settings.renderDrums = true;
        settings.renderMelody = false;
        settings.renderBass = false;
        settings.renderSupport = false;
        QString error;
        const bool applied =
            jam2::practice::PracticeIdeaController::applyReferences(
                project,
                0,
                settings,
                rendered,
                error);
        record(
            QStringLiteral(
                "practice.generated-drum-default-gain-does-not-overwrite-user-lane-gain"),
            appended && applied && error.isEmpty() &&
                project.banks().at(0).lanes.size() == 1 &&
                project.banks().at(0).lanes.front().gainDb == 10.0);
    }
    {
        LooperProject project;
        LooperLane generatedLane;
        generatedLane.name = QStringLiteral("Practice Drums");
        LooperLane manualLane;
        manualLane.name = QStringLiteral("Manual take");
        LooperLane peerGeneratedLane;
        peerGeneratedLane.name = QStringLiteral("Peer Practice Chords");
        peerGeneratedLane.referenceKind = QStringLiteral("chord");
        peerGeneratedLane.localOnly = false;
        const bool generatedApplied = project.appendLane(0, generatedLane);
        const bool manualApplied = project.appendLane(0, manualLane);
        const bool peerGeneratedApplied = project.appendLane(1, peerGeneratedLane);

        jam2::practice::PracticeIdeaController::clearReferences(project);

        record(QStringLiteral("practice.clear-idea-removes-local-and-peer-generated-references-only"),
            generatedApplied && manualApplied && peerGeneratedApplied &&
            project.banks().at(0).lanes.size() == 1 &&
            project.banks().at(0).lanes.front().referenceKind.isEmpty() &&
            project.banks().at(0).lanes.front().name == QStringLiteral("Manual take") &&
            project.banks().at(1).lanes.isEmpty());
    }
    {
        BeatGridModel model;
        model.setTitle(QStringLiteral("Keep This Title"));
        model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
        model.setCell(0, QStringLiteral("target"), 0, QStringLiteral("E4"));
        model.setCell(0, QStringLiteral("lyric"), 0, QStringLiteral("hello"));
        model.setBeatHit(0, 0, 0, QStringLiteral("x..."));
        const int previousRevision = model.revision();

        model.clearContent();

        record(QStringLiteral("practice.clear-idea-empties-all-views-and-keeps-title"),
            model.title() == QStringLiteral("Keep This Title") &&
            model.revision() == previousRevision + 1 &&
            model.hasOnlyPristineSection());
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
    record(QStringLiteral("track-timeline.labels-bars-only"),
        jam2::gui::trackTimelineBarNumber(0, 4) == 1 &&
        jam2::gui::trackTimelineBarNumber(1, 4) == 0 &&
        jam2::gui::trackTimelineBarNumber(3, 4) == 0 &&
        jam2::gui::trackTimelineBarNumber(4, 4) == 2 &&
        jam2::gui::trackTimelineBarNumber(8, 4) == 3);

    {
        PlaybackGrid grid;
        grid.setPattern(60.0, 4, 1, 1);
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
        grid.setPattern(60.0, 4, 1, 1);
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
        PlaybackGrid grid;
        grid.setPattern(60.0, 6, 1, 3);
        grid.updateEngine(100, 100, 0, 0, 100, true);
        const PlaybackGrid::Position position = grid.position();
        record(QStringLiteral("playback-grid.tempo-pulse-controls-written-unit-duration"),
            position.absoluteBeat == 3 &&
            std::abs(position.secondsPerBeat - (1.0 / 3.0)) < 0.000001);
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
            jam2::metronome::ClickVoice::Normal,
            jam2::metronome::ClickSound::Classic);
        const double countInEarly = jam2::metronome::render_sample(
            pattern, 24, 48000.0, 0.5,
            jam2::metronome::ClickVoice::CountIn,
            jam2::metronome::ClickSound::Classic);
        const double normalTail = jam2::metronome::render_sample(
            pattern, 480, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal,
            jam2::metronome::ClickSound::Classic);
        const double countInTail = jam2::metronome::render_sample(
            pattern, 480, 48000.0, 0.5,
            jam2::metronome::ClickVoice::CountIn,
            jam2::metronome::ClickSound::Classic);
        record(QStringLiteral("record-count-in.uses-distinct-lower-longer-click"),
            std::abs(normalEarly - countInEarly) > 0.01 &&
            normalTail == 0.0 &&
            std::abs(countInTail) > 0.01);

        const double classic = jam2::metronome::render_sample(
            pattern, 37, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal,
            jam2::metronome::ClickSound::Classic);
        const double woodblock = jam2::metronome::render_sample(
            pattern, 37, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal,
            jam2::metronome::ClickSound::Woodblock);
        const double rimClick = jam2::metronome::render_sample(
            pattern, 37, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal,
            jam2::metronome::ClickSound::RimClick);
        const double digitalTick = jam2::metronome::render_sample(
            pattern, 37, 48000.0, 0.5,
            jam2::metronome::ClickVoice::Normal,
            jam2::metronome::ClickSound::DigitalTick);
        record(QStringLiteral("metronome.click-sounds-have-distinct-waveforms"),
            std::abs(classic - woodblock) > 0.01 &&
            std::abs(woodblock - rimClick) > 0.01 &&
            std::abs(rimClick - digitalTick) > 0.01);
        record(QStringLiteral("metronome.click-sound-id-is-bounded"),
            jam2::metronome::sanitize_click_sound(-1) ==
                jam2::metronome::ClickSound::Classic &&
            jam2::metronome::sanitize_click_sound(99) ==
                jam2::metronome::ClickSound::DigitalTick);
        record(QStringLiteral("metronome.tempo-pulse-is-independent-of-written-beat-unit"),
            jam2::metronome::step_interval_samples(48000.0, 120, 1, 1) == 24000 &&
            jam2::metronome::step_interval_samples(48000.0, 120, 1, 3) == 8000 &&
            jam2::metronome::step_interval_samples(48000.0, 120, 2, 1) == 12000 &&
            jam2::metronome::step_interval_samples(48000.0, 90, 1, 3) == 10667);
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
        int mutedPeakPpm = -1;
        int audibleLevelPpm = -1;
        int audiblePeakPpm = -1;
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
            mutedPeakPpm = engine.snapshot().output_peak_ppm;
            muted = mutedPeakPpm == 0;
            jam2::EngineCommand level;
            level.type = jam2::EngineCommandType::SetOutputLevel;
            level.value = 1000000;
            if (!engine.submit(level)) {
                throw std::runtime_error("master output command queue unavailable");
            }
            const std::uint64_t deadline = jam2::monotonic_us() + 1000000ULL;
            while (jam2::monotonic_us() < deadline) {
                const jam2::EngineSnapshot snapshot = engine.snapshot();
                audibleLevelPpm = snapshot.output_level_ppm;
                audiblePeakPpm = snapshot.output_peak_ppm;
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
            outputError.isEmpty()
                ? QStringLiteral("muted_peak_ppm=%1 audible_level_ppm=%2 audible_peak_ppm=%3")
                    .arg(mutedPeakPpm).arg(audibleLevelPpm).arg(audiblePeakPpm)
                : outputError);
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
