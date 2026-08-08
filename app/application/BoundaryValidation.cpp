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
#include "JamStorage.hpp"
#include "RecordingTiming.hpp"

#include "common.hpp"
#include "audio_device.hpp"
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
#include <thread>
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
    bool sendAssetControl(const QString& token, const QJsonObject& message) override
    {
        if (message.value(QStringLiteral("type")).toString() ==
            QStringLiteral("looper.asset.ack")) {
            ++acknowledgements;
            lastAcknowledgedChunks = message.value(QStringLiteral("chunks")).toInt(-1);
            lastAcknowledgementToken = token;
        }
        return true;
    }
    bool sendAssetBinary(const QString&, const QByteArray&) override { return true; }

    bool active = true;
    QString expectedHash;
    QString expectedSource = QStringLiteral("peer-a");
    int abandoned = 0;
    int accepted = 0;
    int acknowledgements = 0;
    int lastAcknowledgedChunks = -1;
    QString lastAcknowledgementToken;
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

QByteArray pcm16Wav(quint32 frames)
{
    const quint32 dataBytes = frames * 2;
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
    for (quint32 frame = 0; frame < frames; ++frame) {
        appendLittleEndian<qint16>(bytes, static_cast<qint16>(
            frame % 2 == 0 ? 1000 : -1000));
    }
    return bytes;
}

QByteArray minimalPcm16Wav()
{
    return pcm16Wav(2);
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
    record(QStringLiteral("metronome.transport-gates-audible-click-only"),
        !jam2::audio::metronome_output_allowed(true, false, true, false, false) &&
        jam2::audio::metronome_output_allowed(true, false, true, true, false) &&
        jam2::audio::metronome_output_allowed(true, false, true, false, true) &&
        jam2::audio::metronome_output_allowed(true, false, false, false, false) &&
        !jam2::audio::metronome_output_allowed(false, false, true, true, false) &&
        !jam2::audio::metronome_output_allowed(true, true, true, true, false));
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
        options.metronome_transport_gated = true;
        const jam2::EngineConfig configured =
            jam2_make_engine_config(options, true);
        jam2::EngineConfig changedLevel = configured;
        changedLevel.output_level_ppm = 500000;
        record(QStringLiteral("master-output.runtime-level-is-dynamic"),
            configured.output_level_ppm == 250000 &&
            !jam2_engine_restart_required(configured, changedLevel));
        record(QStringLiteral("metronome.transport-gate-applies-before-engine-start"),
            configured.metronome_transport_gated);
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
        const std::vector<std::int16_t> takeWithSilence{
            0, 100, 1000, 2000, 100, 0, 0};
        const std::vector<std::int16_t> trimmed =
            jam2::gui::trim_loopback_silence_pcm16(
                takeWithSilence, -40.0, 2, true, true);
        const std::vector<std::int16_t> shortTail{0, 1000, 0};
        const std::vector<std::int16_t> shortTailTrimmed =
            jam2::gui::trim_loopback_silence_pcm16(
                shortTail, -40.0, 2, true, true);
        record(QStringLiteral("loopback-recording.trim-independent-of-trigger"),
            trimmed == std::vector<std::int16_t>({1000, 2000}) &&
            shortTailTrimmed == std::vector<std::int16_t>({1000, 0}) &&
            jam2::gui::trim_loopback_silence_pcm16(
                takeWithSilence, -40.0, 2, false, false) == takeWithSilence);
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
            context.acceptedHash == context.expectedHash && acceptedFileReady &&
            context.acknowledgements == 1 && context.lastAcknowledgedChunks == 1 &&
            context.lastAcknowledgementToken == context.expectedSource);
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

    const QJsonObject largeControlMessage{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("payload"), QString(140 * 1024, QLatin1Char('x'))},
    };
    const AuthenticatedJsonFrames largeFrames = encodeAuthenticatedJsonFrames(
        largeControlMessage, directionKey, 20);
    LargeJsonReceiver largeReceiver;
    QJsonObject reconstructedLargeMessage;
    bool largeReady = false;
    bool largeValid = largeFrames.chunked && !largeFrames.frames.isEmpty();
    quint64 largeSequence = 20;
    error.clear();
    for (QByteArray frame : largeFrames.frames) {
        QByteArray largeBody;
        AuthenticatedPayload largePayload;
        bool ready = false;
        largeValid = largeValid &&
            takeFrame(frame, largeBody, error) == TakeFrameResult::Ready &&
            decodeAuthenticated(
                largeBody, directionKey, largeSequence++, largePayload, error) &&
            largePayload.type == AuthenticatedPayloadType::LargeJsonChunk &&
            largeReceiver.accept(
                largePayload.binary, reconstructedLargeMessage, ready, error);
        largeReady = largeReady || ready;
    }
    record(QStringLiteral("control.large-json-authenticated-compressed-atomic-reassembly"),
        largeValid && largeReady && reconstructedLargeMessage == largeControlMessage &&
            !largeReceiver.active(),
        error);
    record(QStringLiteral("control.reject-json-over-large-message-bound"),
        encodeAuthenticatedJsonFrames(
            QJsonObject{{QStringLiteral("value"),
                QString(kMaxLargeJsonBytes + 1, QLatin1Char('x'))}},
            directionKey,
            100).frames.isEmpty());

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
        {QStringLiteral("lane"), 9},
        {QStringLiteral("text"), QStringLiteral("x...")},
    };
    modelError.clear();
    const bool acceptedLastBeatLane =
        jam2::application::validateControlMessage(validBeatHit, modelError);
    validBeatHit[QStringLiteral("lane")] = 10;
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
        record(QStringLiteral("track.defaults-to-minus-ten-db"),
            defaults.trackGainDb == -10.0);
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
        {
            const QStringList everyMeter =
                jam2::practice::compatibleMeterIds(QString(), QString());
            bool constrainedGenerationValid = everyMeter.contains(QStringLiteral("4-4"));
            for (int styleIndex = 0;
                 styleIndex < ids.size() && constrainedGenerationValid;
                 ++styleIndex) {
                const QStringList available = jam2::practice::compatibleMeterIds(
                    ids.at(styleIndex), QString());
                constrainedGenerationValid = !available.isEmpty();
                if (!constrainedGenerationValid) break;
                jam2::practice::ChordIdeaRequest request;
                request.styleId = ids.at(styleIndex);
                request.meterId = available.front();
                request.bpm = 37;
                const auto idea = jam2::practice::generateCoupledPracticeIdeaForTest(
                    request, static_cast<std::uint32_t>(7000 + styleIndex));
                const auto* selectedProfile =
                    jam2::practice::findProfile(idea.recipe.profileId, true);
                constrainedGenerationValid = idea.recipe.isValid() &&
                    idea.recipe.styleId == request.styleId &&
                    idea.recipe.meterId == request.meterId &&
                    idea.recipe.bpm == request.bpm &&
                    selectedProfile != nullptr &&
                    selectedProfile->meterIds.contains(request.meterId);
            }
            for (int seed = 0; seed < 32 && constrainedGenerationValid; ++seed) {
                jam2::practice::ChordIdeaRequest request;
                request.meterId = QStringLiteral("4-4");
                const auto idea = jam2::practice::generateCoupledPracticeIdeaForTest(
                    request, static_cast<std::uint32_t>(7100 + seed));
                const auto* selectedProfile =
                    jam2::practice::findProfile(idea.recipe.profileId, true);
                constrainedGenerationValid =
                    idea.recipe.meterId == request.meterId &&
                    selectedProfile != nullptr &&
                    selectedProfile->meterIds.contains(request.meterId);
            }
            record(QStringLiteral("practice.meter-and-exact-tempo-constrain-random-generation"),
                constrainedGenerationValid);
        }
        {
            jam2::practice::ChordIdeaRequest request;
            request.profileId = QStringLiteral("pop_loop");
            request.meterId = QStringLiteral("4-4");
            request.bpm = 0;
            request.bars = 8;
            QSet<int> generatedTempos;
            bool styleTempoRangeValid = true;
            const auto* profile = jam2::practice::findProfile(
                request.profileId, true);
            for (int seed = 0; seed < 32; ++seed) {
                const auto idea =
                    jam2::practice::generateCoupledPracticeIdeaForTest(
                        request,
                        static_cast<std::uint32_t>(7350 + seed));
                generatedTempos.insert(idea.bpm);
                styleTempoRangeValid = styleTempoRangeValid && profile &&
                    idea.bpm >= profile->minimumBpm &&
                    idea.bpm <= profile->maximumBpm;
            }
            record(
                QStringLiteral(
                    "practice.unset-exact-tempo-varies-inside-style-range"),
                styleTempoRangeValid && generatedTempos.size() > 1);
        }
        {
            BeatGridModel model;
            model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
            model.setCell(0, QStringLiteral("target"), 0, QStringLiteral("E4"));
            model.setBeatHit(0, 0, 0, QStringLiteral("x..."));
            const QString originalPitched =
                jam2::practice::generatedChordFingerprint(model.section(0));

            jam2::practice::ChordIdeaRequest drums;
            drums.styleId = QStringLiteral("pop");
            drums.meterId = QStringLiteral("4-4");
            drums.bpm = 120;
            drums.bars = 8;
            drums.parts = jam2::practice::PracticeIdeaParts::DrumsOnly;
            drums.targetSectionIndex = 0;
            const auto drumsResult =
                jam2::practice::PracticeIdeaController::generateCoupled(
                    model, model, drums);
            const bool drumsPreservedPitched =
                jam2::practice::generatedChordFingerprint(model.section(0)) ==
                originalPitched;
            const auto* drumsOnlyKit =
                jam2::practice::researchDrumKitById(
                    model.section(0).generatedRecipe.drumPatchId);
            const bool drumsOnlyUsesMappedKit =
                drumsOnlyKit &&
                drumsOnlyKit->baseKitId == QStringLiteral("acoustic") &&
                model.section(0).drumKitId == QStringLiteral("acoustic");
            const QString generatedDrums =
                jam2::practice::generatedBeatFingerprint(model.section(0));

            jam2::practice::ChordIdeaRequest pitched = drums;
            pitched.parts = jam2::practice::PracticeIdeaParts::PitchedPartsOnly;
            const auto pitchedResult =
                jam2::practice::PracticeIdeaController::generateCoupled(
                    model, model, pitched);
            record(QStringLiteral("practice.partial-generation-preserves-unselected-parts"),
                drumsResult.has_value() && pitchedResult.has_value() &&
                drumsPreservedPitched &&
                drumsOnlyUsesMappedKit &&
                jam2::practice::generatedChordFingerprint(model.section(0)) !=
                    originalPitched &&
                jam2::practice::generatedBeatFingerprint(model.section(0)) ==
                    generatedDrums &&
                model.section(0).generatedKind == QStringLiteral("practice") &&
                model.section(0).beats == 32);
        }
        {
            const auto combinedIdeaSection = [](const jam2::practice::GeneratedPracticeIdea& idea) {
                SongSection section = idea.chordSection;
                section.beatNotes = idea.beatSection.beatNotes;
                section.beatPatterns = idea.beatSection.beatPatterns;
                return section;
            };
            const auto requestFor = [](const SongSection& source) {
                jam2::practice::ContinueIdeaRequest request;
                request.sourceSectionIndex = 0;
                request.targetSectionIndex = 1;
                request.bpm = source.generatedRecipe.isValid()
                    ? source.generatedRecipe.bpm : 112;
                request.meterId = source.generatedRecipe.isValid()
                    ? source.generatedRecipe.meterId : QStringLiteral("4-4");
                request.beatsPerBar = source.generatedRecipe.isValid()
                    ? source.generatedRecipe.meterNumerator : 4;
                request.beatUnit = source.generatedRecipe.isValid()
                    ? source.generatedRecipe.meterDenominator : 4;
                request.tempoPulseUnits = source.generatedRecipe.isValid()
                    ? source.generatedRecipe.tempoPulseUnits : 1;
                return request;
            };
            const auto modeIntervals = [](QString modeName) {
                modeName = modeName.toLower();
                if (modeName.contains(QStringLiteral("dorian")))
                    return QVector<int>{0,2,3,5,7,9,10};
                if (modeName.contains(QStringLiteral("mixolydian")))
                    return QVector<int>{0,2,4,5,7,9,10};
                if (modeName.contains(QStringLiteral("lydian")))
                    return QVector<int>{0,2,4,6,7,9,11};
                if (modeName.contains(QStringLiteral("phrygian")))
                    return QVector<int>{0,1,3,5,7,8,10};
                if (modeName.contains(QStringLiteral("minor")) ||
                    modeName.contains(QStringLiteral("aeolian")))
                    return QVector<int>{0,2,3,5,7,8,10};
                if (modeName.contains(QStringLiteral("major")) ||
                    modeName.contains(QStringLiteral("ionian")))
                    return QVector<int>{0,2,4,5,7,9,11};
                return QVector<int>{};
            };
            const auto harmonyFitsKey = [&modeIntervals](
                                            const SongSection& section,
                                            const QString& tonicName,
                                            const QString& modeName) {
                const auto tonic = jam2::practice::parseChord(tonicName);
                const QVector<int> allowed = modeIntervals(modeName);
                if (!tonic.valid || allowed.isEmpty()) return true;
                for (const QString& symbol : section.chords) {
                    if (symbol.trimmed().isEmpty() || symbol == QStringLiteral("-")) continue;
                    const auto chord = jam2::practice::parseChord(symbol);
                    if (!chord.valid || chord.rest) return false;
                    for (int interval : chord.intervals) {
                        const int relative = (chord.root + interval - tonic.root) % 12;
                        if (!allowed.contains((relative + 12) % 12)) return false;
                    }
                    if (chord.bass >= 0) {
                        const int relative = (chord.bass - tonic.root) % 12;
                        if (!allowed.contains((relative + 12) % 12)) return false;
                    }
                }
                return true;
            };
            bool generatedContinuationValid = true;
            double generatedChordTotal = 0.0;
            double generatedContrastTotal = 0.0;
            double generatedDrumTotal = 0.0;
            double generatedMelodyTotal = 0.0;
            double generatedContourTotal = 0.0;
            double generatedBassContourTotal = 0.0;
            double generatedMinimumChord = 1.0;
            double generatedMaximumChord = 0.0;
            QStringList generatedProfiles;
            QStringList generatedFailures;
            const QStringList generatedStyles = jam2::practice::styleIds();
            for (int index = 0; index < generatedStyles.size(); ++index) {
                jam2::practice::ChordIdeaRequest sourceRequest;
                sourceRequest.styleId = generatedStyles.at(index);
                const auto sourceIdea = jam2::practice::generateCoupledPracticeIdeaForTest(
                    sourceRequest, static_cast<std::uint32_t>(21000 + index));
                const SongSection source = combinedIdeaSection(sourceIdea);
                const QString sourceChord =
                    jam2::practice::generatedChordFingerprint(source);
                const QString sourceBeat =
                    jam2::practice::generatedBeatFingerprint(source);
                const auto continuationRequest = requestFor(source);
                const auto first = jam2::practice::generateContinuationPracticeIdeaForTest(
                    source, continuationRequest,
                    static_cast<std::uint32_t>(22000 + index));
                const auto second = jam2::practice::generateContinuationPracticeIdeaForTest(
                    source, continuationRequest,
                    static_cast<std::uint32_t>(22000 + index));
                const auto& analysis = first.analysis;
                const bool caseValid =
                    sourceChord == jam2::practice::generatedChordFingerprint(source) &&
                    sourceBeat == jam2::practice::generatedBeatFingerprint(source) &&
                    first.idea.recipe.isValid() &&
                    first.idea.recipe.bpm == sourceIdea.recipe.bpm &&
                    first.idea.recipe.meterId == sourceIdea.recipe.meterId &&
                    first.idea.recipe.mode == sourceIdea.recipe.mode &&
                    first.idea.recipe.profileId == sourceIdea.recipe.profileId &&
                    first.idea.recipe.formId == sourceIdea.recipe.formId &&
                    harmonyFitsKey(
                        first.idea.chordSection,
                        sourceIdea.recipe.tonic,
                        sourceIdea.recipe.mode) &&
                    first.idea.chordSection.beats == source.beats &&
                    first.idea.recipe.tonic == sourceIdea.recipe.tonic &&
                    first.idea.recipe.variationId == analysis.relationshipId &&
                    analysis.candidateCount == 32 &&
                    (sourceIdea.recipe.mode.contains(QStringLiteral("Blues")) ||
                        analysis.chordVocabularySimilarity <= 0.55 ||
                        analysis.chordQualityVocabularySimilarity <= 0.55) &&
                    (sourceIdea.recipe.mode.contains(QStringLiteral("Blues"))
                        ? analysis.chordOrderContrast > 0.0 &&
                            analysis.openingChordPositionSimilarity <= 0.75
                        : analysis.chordOrderContrast >= 0.50 &&
                            analysis.openingChordPositionSimilarity <= 0.25) &&
                    analysis.harmonicDensityRetention >= 0.65 &&
                    analysis.sourceChordEvents > 0 &&
                    analysis.continuationChordEvents > 0 &&
                    analysis.drumSimilarity <=
                        (sourceIdea.recipe.mode.contains(
                            QStringLiteral("Blues")) ? 0.78 : 0.72) &&
                    jam2::practice::generatedChordFingerprint(first.idea.chordSection) !=
                        sourceChord &&
                    jam2::practice::generatedChordFingerprint(first.idea.chordSection) ==
                        jam2::practice::generatedChordFingerprint(second.idea.chordSection) &&
                    jam2::practice::generatedBeatFingerprint(first.idea.beatSection) ==
                        jam2::practice::generatedBeatFingerprint(second.idea.beatSection);
                generatedContinuationValid = generatedContinuationValid && caseValid;
                if (!caseValid) {
                    generatedFailures << QStringLiteral(
                        "%1 valid=%2 bpm=%3/%4 meter=%5/%6 mode=%7/%8 beats=%9/%10 tonic=%11/%12 c=%13 o=%14 d=%15 m=%16 density=%17 (%18->%19)")
                        .arg(generatedStyles.at(index))
                        .arg(first.idea.recipe.isValid())
                        .arg(first.idea.recipe.bpm).arg(sourceIdea.recipe.bpm)
                        .arg(first.idea.recipe.meterId, sourceIdea.recipe.meterId)
                        .arg(first.idea.recipe.mode, sourceIdea.recipe.mode)
                        .arg(first.idea.chordSection.beats).arg(source.beats)
                        .arg(first.idea.recipe.tonic, sourceIdea.recipe.tonic)
                        .arg(qRound(analysis.chordVocabularySimilarity * 100.0))
                        .arg(qRound(analysis.chordOrderContrast * 100.0))
                        .arg(qRound(analysis.drumSimilarity * 100.0))
                        .arg(qRound(analysis.melodyRhythmSimilarity * 100.0))
                        .arg(qRound(analysis.harmonicDensityRetention * 100.0))
                        .arg(analysis.sourceChordEvents)
                        .arg(analysis.continuationChordEvents);
                }
                generatedChordTotal += analysis.chordVocabularySimilarity;
                generatedContrastTotal += analysis.chordOrderContrast;
                generatedDrumTotal += analysis.drumSimilarity;
                generatedMelodyTotal += analysis.melodyRhythmSimilarity;
                generatedContourTotal += analysis.melodyContourSimilarity;
                generatedBassContourTotal += analysis.bassContourSimilarity;
                generatedMinimumChord = qMin(
                    generatedMinimumChord, analysis.chordVocabularySimilarity);
                generatedMaximumChord = qMax(
                    generatedMaximumChord, analysis.chordVocabularySimilarity);
                generatedProfiles << first.idea.recipe.profileId;
            }
            const int generatedCount = qMax(1, generatedStyles.size());
            record(QStringLiteral("practice.continue-generated-a-corpus-preserves-identity-with-contrast"),
                generatedContinuationValid,
                QStringLiteral(
                    "%1 generated A→B rounds; mean chord=%2%, range=%3-%4%, order contrast=%5%, drums=%6%, melody rhythm=%7%, melody contour=%8%, bass contour=%9%; profiles=%10")
                    .arg(generatedStyles.size())
                    .arg(qRound(100.0 * generatedChordTotal / generatedCount))
                    .arg(qRound(100.0 * generatedMinimumChord))
                    .arg(qRound(100.0 * generatedMaximumChord))
                    .arg(qRound(100.0 * generatedContrastTotal / generatedCount))
                    .arg(qRound(100.0 * generatedDrumTotal / generatedCount))
                    .arg(qRound(100.0 * generatedMelodyTotal / generatedCount))
                    .arg(qRound(100.0 * generatedContourTotal / generatedCount))
                    .arg(qRound(100.0 * generatedBassContourTotal / generatedCount))
                    .arg(generatedProfiles.join(QLatin1Char(','))) +
                    (generatedFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            generatedFailures.join(QStringLiteral(" | ")))));

            struct ModeMatrixCase {
                QString id;
                QString name;
            };
            const QVector<ModeMatrixCase> continuationModes{
                {QStringLiteral("ionian"), QStringLiteral("Major")},
                {QStringLiteral("aeolian"), QStringLiteral("Natural Minor")},
                {QStringLiteral("dorian"), QStringLiteral("Dorian")},
                {QStringLiteral("mixolydian"), QStringLiteral("Mixolydian")},
                {QStringLiteral("lydian"), QStringLiteral("Lydian")},
                {QStringLiteral("phrygian"), QStringLiteral("Phrygian")},
            };
            bool keyModeMatrixValid = true;
            int keyModeMatrixRounds = 0;
            QStringList keyModeMatrixFailures;
            for (int key = 0; key < 12; ++key) {
                for (int modeIndex = 0;
                     modeIndex < continuationModes.size(); ++modeIndex) {
                    const ModeMatrixCase& mode = continuationModes.at(modeIndex);
                    jam2::practice::ChordIdeaRequest sourceRequest;
                    sourceRequest.key = key;
                    sourceRequest.styleId = QStringLiteral("modal-jam");
                    sourceRequest.profileId = QStringLiteral("modal_groove");
                    sourceRequest.modeId = mode.id;
                    sourceRequest.allowModeOverride = true;
                    sourceRequest.meterId = QStringLiteral("4-4");
                    sourceRequest.allowMeterOverride = true;
                    sourceRequest.bpm = 88 + ((key * 7 + modeIndex * 5) % 41);
                    sourceRequest.bars = 8;
                    const std::uint32_t sourceSeed = static_cast<std::uint32_t>(
                        24000 + key * 100 + modeIndex);
                    const auto sourceIdea =
                        jam2::practice::generateCoupledPracticeIdeaForTest(
                            sourceRequest, sourceSeed);
                    const SongSection source = combinedIdeaSection(sourceIdea);
                    const auto continuation =
                        jam2::practice::generateContinuationPracticeIdeaForTest(
                            source, requestFor(source), sourceSeed + 50U);
                    const auto sourceTonic =
                        jam2::practice::parseChord(sourceIdea.recipe.tonic);
                    const auto targetTonic =
                        jam2::practice::parseChord(continuation.idea.recipe.tonic);
                    const bool caseValid =
                        sourceIdea.recipe.isValid() &&
                        continuation.idea.recipe.isValid() &&
                        sourceTonic.valid && targetTonic.valid &&
                        sourceTonic.root == key &&
                        targetTonic.root == key &&
                        sourceIdea.recipe.mode == mode.name &&
                        continuation.idea.recipe.mode == mode.name &&
                        continuation.analysis.inferredMode == mode.name &&
                        continuation.idea.recipe.bpm == sourceIdea.recipe.bpm &&
                        continuation.idea.recipe.meterId == sourceIdea.recipe.meterId &&
                        continuation.idea.chordSection.beats == source.beats &&
                        harmonyFitsKey(
                            continuation.idea.chordSection,
                            sourceIdea.recipe.tonic,
                            sourceIdea.recipe.mode) &&
                        jam2::practice::generatedChordFingerprint(
                            continuation.idea.chordSection) !=
                            jam2::practice::generatedChordFingerprint(source) &&
                        continuation.analysis.chordOrderContrast > 0.0;
                    keyModeMatrixValid = keyModeMatrixValid && caseValid;
                    ++keyModeMatrixRounds;
                    if (!caseValid && keyModeMatrixFailures.size() < 12) {
                        keyModeMatrixFailures << QStringLiteral(
                            "%1/%2 src=%3-%4 dst=%5-%6 fit=%7 contrast=%8%")
                            .arg(key)
                            .arg(mode.id,
                                sourceIdea.recipe.tonic,
                                sourceIdea.recipe.mode,
                                continuation.idea.recipe.tonic,
                                continuation.idea.recipe.mode)
                            .arg(harmonyFitsKey(
                                continuation.idea.chordSection,
                                sourceIdea.recipe.tonic,
                                sourceIdea.recipe.mode))
                            .arg(qRound(
                                continuation.analysis.chordOrderContrast * 100.0));
                    }
                }
            }
            record(QStringLiteral(
                       "practice.continue-all-keys-and-diatonic-modes-remain-strict"),
                keyModeMatrixValid,
                QStringLiteral(
                    "%1 generated A→B rounds; 12 tonics × 6 modes; every target chord tone and slash bass checked against the source scale")
                    .arg(keyModeMatrixRounds) +
                    (keyModeMatrixFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            keyModeMatrixFailures.join(QStringLiteral(" | ")))));

            struct NativeCollectionCase {
                QString profileId;
                QString modeId;
                QString meterId;
                QString formId;
                int bars = 8;
                bool bluesHarmony = false;
            };
            const QVector<NativeCollectionCase> nativeCollections{
                {QStringLiteral("country_contemporary"),
                    QStringLiteral("major-pentatonic"),
                    QStringLiteral("4-4"), QString(), 8, false},
                {QStringLiteral("rock_riff_modal"),
                    QStringLiteral("minor-pentatonic"),
                    QStringLiteral("4-4"), QString(), 8, false},
                {QStringLiteral("blues_dominant"),
                    QStringLiteral("dominant-blues"),
                    QStringLiteral("4-4-shuffle"),
                    QStringLiteral("blues-12"), 12, true},
                {QStringLiteral("blues_minor"),
                    QStringLiteral("minor-blues"),
                    QStringLiteral("4-4"),
                    QStringLiteral("minor-blues-12"), 12, true},
            };
            const auto bluesHarmonyStaysFunctional = [](
                const SongSection& section, const QString& tonicName) {
                const auto tonic = jam2::practice::parseChord(tonicName);
                if (!tonic.valid) return false;
                const QSet<int> nativeRoots{0, 5, 7};
                for (const QString& symbol : section.chords) {
                    if (symbol.trimmed().isEmpty() || symbol == QStringLiteral("-")) {
                        continue;
                    }
                    const auto chord = jam2::practice::parseChord(symbol);
                    if (!chord.valid || chord.rest ||
                        !nativeRoots.contains(
                            (chord.root - tonic.root + 12) % 12)) {
                        return false;
                    }
                }
                return true;
            };
            bool nativeCollectionValid = true;
            int nativeCollectionRounds = 0;
            QSet<QString> nativeCollectionFingerprints;
            QStringList nativeCollectionFailures;
            for (int collectionIndex = 0;
                 collectionIndex < nativeCollections.size(); ++collectionIndex) {
                const NativeCollectionCase& item =
                    nativeCollections.at(collectionIndex);
                for (int key = 0; key < 12; ++key) {
                    jam2::practice::ChordIdeaRequest sourceRequest;
                    sourceRequest.key = key;
                    sourceRequest.profileId = item.profileId;
                    sourceRequest.modeId = item.modeId;
                    sourceRequest.allowModeOverride = true;
                    sourceRequest.meterId = item.meterId;
                    sourceRequest.allowMeterOverride = true;
                    sourceRequest.formId = item.formId;
                    sourceRequest.bars = item.bars;
                    sourceRequest.bpm = 82 +
                        (collectionIndex * 29 + key * 7) % 71;
                    sourceRequest.harmonicComplexity = 2;
                    sourceRequest.rhythmicComplexity = 3;
                    const std::uint32_t seed = static_cast<std::uint32_t>(
                        45000 + collectionIndex * 1000 + key * 10);
                    const auto sourceIdea =
                        jam2::practice::generateCoupledPracticeIdeaForTest(
                            sourceRequest, seed);
                    const SongSection source = combinedIdeaSection(sourceIdea);
                    const auto continuation =
                        jam2::practice::generateContinuationPracticeIdeaForTest(
                            source, requestFor(source), seed + 5U);
                    const bool keySafe = item.bluesHarmony
                        ? bluesHarmonyStaysFunctional(
                            continuation.idea.chordSection,
                            sourceIdea.recipe.tonic)
                        : harmonyFitsKey(
                            continuation.idea.chordSection,
                            sourceIdea.recipe.tonic,
                            sourceIdea.recipe.mode);
                    const bool caseValid =
                        sourceIdea.recipe.isValid() &&
                        continuation.idea.recipe.isValid() &&
                        continuation.idea.recipe.tonic ==
                            sourceIdea.recipe.tonic &&
                        continuation.idea.recipe.mode ==
                            sourceIdea.recipe.mode &&
                        continuation.idea.recipe.profileId ==
                            sourceIdea.recipe.profileId &&
                        continuation.idea.recipe.formId ==
                            sourceIdea.recipe.formId &&
                        continuation.idea.chordSection.beats == source.beats &&
                        continuation.analysis.inferredTonalConfidence == 1.0 &&
                        continuation.analysis.inferredProfileConfidence == 1.0 &&
                        continuation.analysis.harmonicDensityRetention >= 0.65 &&
                        continuation.analysis.chordOrderContrast > 0.0 &&
                        keySafe;
                    nativeCollectionValid = nativeCollectionValid && caseValid;
                    ++nativeCollectionRounds;
                    nativeCollectionFingerprints.insert(
                        jam2::practice::generatedChordFingerprint(
                            continuation.idea.chordSection));
                    if (!caseValid && nativeCollectionFailures.size() < 12) {
                        nativeCollectionFailures << QStringLiteral(
                            "%1/%2 key=%3 src=%4-%5/%6 dst=%7-%8/%9 form=%10/%11 density=%12% contrast=%13% safe=%14")
                            .arg(item.profileId, item.modeId)
                            .arg(key)
                            .arg(sourceIdea.recipe.tonic,
                                sourceIdea.recipe.mode,
                                sourceIdea.recipe.profileId,
                                continuation.idea.recipe.tonic,
                                continuation.idea.recipe.mode,
                                continuation.idea.recipe.profileId,
                                sourceIdea.recipe.formId,
                                continuation.idea.recipe.formId)
                            .arg(qRound(
                                continuation.analysis.harmonicDensityRetention *
                                100.0))
                            .arg(qRound(
                                continuation.analysis.chordOrderContrast * 100.0))
                            .arg(keySafe);
                    }
                }
            }
            record(QStringLiteral(
                       "practice.continue-generated-native-collections-preserve-key-form-and-density"),
                nativeCollectionValid && nativeCollectionRounds == 48 &&
                    nativeCollectionFingerprints.size() >= 40,
                QStringLiteral(
                    "%1 generated A-to-B rounds across major/minor pentatonic and dominant/minor Blues; %2 distinct B harmonies")
                    .arg(nativeCollectionRounds)
                    .arg(nativeCollectionFingerprints.size()) +
                    (nativeCollectionFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            nativeCollectionFailures.join(
                                QStringLiteral(" | ")))));

            bool mixolydianContrastValid = true;
            QSet<QString> mixolydianContrastFingerprints;
            QStringList mixolydianContrastFailures;
            constexpr int kMixolydianContrastRounds = 16;
            for (int iteration = 0;
                 iteration < kMixolydianContrastRounds; ++iteration) {
                jam2::practice::ChordIdeaRequest sourceRequest;
                sourceRequest.key = 10; // Bb
                sourceRequest.profileId = QStringLiteral("pop_loop");
                sourceRequest.modeId = QStringLiteral("mixolydian");
                sourceRequest.allowModeOverride = true;
                sourceRequest.meterId = QStringLiteral("4-4");
                sourceRequest.allowMeterOverride = true;
                sourceRequest.bars = 8;
                const auto sourceIdea =
                    jam2::practice::generateCoupledPracticeIdeaForTest(
                        sourceRequest,
                        static_cast<std::uint32_t>(47000 + iteration));
                SongSection source = combinedIdeaSection(sourceIdea);
                source.chords.fill(QString(), source.beats);
                const QStringList authoredRoute{
                    QStringLiteral("Bb"), QStringLiteral("Eb"),
                    QStringLiteral("Bb"), QStringLiteral("F")};
                for (int bar = 0; bar < source.beats / 4; ++bar) {
                    source.chords[bar * 4] = authoredRoute.at(
                        bar % authoredRoute.size());
                }
                const auto continuation =
                    jam2::practice::generateContinuationPracticeIdeaForTest(
                        source,
                        requestFor(source),
                        static_cast<std::uint32_t>(48000 + iteration));
                const auto& analysis = continuation.analysis;
                const bool caseValid =
                    continuation.idea.recipe.isValid() &&
                    continuation.idea.recipe.tonic == QStringLiteral("Bb") &&
                    continuation.idea.recipe.mode == QStringLiteral("Mixolydian") &&
                    continuation.idea.recipe.profileId ==
                        sourceIdea.recipe.profileId &&
                    continuation.idea.recipe.formId ==
                        sourceIdea.recipe.formId &&
                    continuation.idea.chordSection.beats == source.beats &&
                    harmonyFitsKey(
                        continuation.idea.chordSection,
                        QStringLiteral("Bb"),
                        QStringLiteral("Mixolydian")) &&
                    analysis.chordVocabularySimilarity <= 0.50 &&
                    analysis.chordOrderContrast >= 0.75 &&
                    analysis.openingChordPositionSimilarity <= 0.25 &&
                    analysis.drumSimilarity <= 0.72;
                mixolydianContrastValid =
                    mixolydianContrastValid && caseValid;
                mixolydianContrastFingerprints.insert(
                    jam2::practice::generatedChordFingerprint(
                        continuation.idea.chordSection));
                if (!caseValid && mixolydianContrastFailures.size() < 8) {
                    mixolydianContrastFailures << QStringLiteral(
                        "%1 chord=%2% order=%3% opening=%4% drums=%5% key=%6-%7")
                        .arg(iteration)
                        .arg(qRound(
                            analysis.chordVocabularySimilarity * 100.0))
                        .arg(qRound(analysis.chordOrderContrast * 100.0))
                        .arg(qRound(
                            analysis.openingChordPositionSimilarity * 100.0))
                        .arg(qRound(analysis.drumSimilarity * 100.0))
                        .arg(continuation.idea.recipe.tonic,
                            continuation.idea.recipe.mode);
                }
            }
            record(QStringLiteral(
                       "practice.continue-bb-mixolydian-establishes-new-section-identity"),
                mixolydianContrastValid &&
                    mixolydianContrastFingerprints.size() >= 12,
                QStringLiteral(
                    "%1 continuations of Bb-Eb-Bb-F; %2 distinct B harmonies; every B remains Bb Mixolydian with <=50% chord-root overlap, <=25% aligned opening chords, and <=72% drum overlap")
                    .arg(kMixolydianContrastRounds)
                    .arg(mixolydianContrastFingerprints.size()) +
                    (mixolydianContrastFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            mixolydianContrastFailures.join(
                                QStringLiteral(" | ")))));

            struct ContinuationProfileCase {
                QString styleId;
                QString profileId;
            };
            QVector<ContinuationProfileCase> continuationProfiles;
            for (const QString& styleId : jam2::practice::styleIds()) {
                for (const QString& profileId :
                     jam2::practice::profileIds(styleId)) {
                    continuationProfiles.push_back({styleId, profileId});
                }
            }
            continuationProfiles.push_back({
                QStringLiteral("metal-experimental"),
                QStringLiteral("metal_modern_progressive")});
            bool profileContinuationValid = true;
            double profileContourTotal = 0.0;
            double profileBassContourTotal = 0.0;
            double profileRhythmTotal = 0.0;
            double profileBoundaryTotal = 0.0;
            QSet<QString> profilePacingIds;
            QSet<QString> profileProgressionFamilies;
            QSet<QString> continuationRoleIds;
            QStringList profileFailures;
            for (int index = 0; index < continuationProfiles.size(); ++index) {
                const ContinuationProfileCase& item = continuationProfiles.at(index);
                jam2::practice::ChordIdeaRequest sourceRequest;
                sourceRequest.key = (index * 5) % 12;
                sourceRequest.styleId = item.styleId;
                sourceRequest.profileId = item.profileId;
                sourceRequest.modeId = index % 2 == 0
                    ? QStringLiteral("ionian") : QStringLiteral("aeolian");
                sourceRequest.allowModeOverride = true;
                sourceRequest.meterId = QStringLiteral("4-4");
                sourceRequest.allowMeterOverride = true;
                sourceRequest.bpm = 84 + (index * 7) % 72;
                sourceRequest.bars = 8;
                const auto sourceIdea =
                    jam2::practice::generateCoupledPracticeIdeaForTest(
                        sourceRequest,
                        static_cast<std::uint32_t>(31000 + index));
                const SongSection source = combinedIdeaSection(sourceIdea);
                const auto continuation =
                    jam2::practice::generateContinuationPracticeIdeaForTest(
                        source,
                        requestFor(source),
                        static_cast<std::uint32_t>(32000 + index));
                const bool caseValid =
                    sourceIdea.recipe.isValid() &&
                    continuation.idea.recipe.isValid() &&
                    sourceIdea.recipe.profileId == item.profileId &&
                    continuation.idea.recipe.profileId == item.profileId &&
                    continuation.idea.recipe.styleId == sourceIdea.recipe.styleId &&
                    continuation.idea.recipe.tonic == sourceIdea.recipe.tonic &&
                    continuation.idea.recipe.mode == sourceIdea.recipe.mode &&
                    continuation.idea.recipe.formId == sourceIdea.recipe.formId &&
                    continuation.idea.chordSection.beats == source.beats &&
                    continuation.idea.beatSection.beats == source.beats &&
                    continuation.idea.recipe.productionFamilyId ==
                        sourceIdea.recipe.productionFamilyId &&
                    continuation.idea.recipe.chordPatchId ==
                        sourceIdea.recipe.chordPatchId &&
                    continuation.idea.recipe.melodyPatchId ==
                        sourceIdea.recipe.melodyPatchId &&
                    continuation.idea.recipe.bassPatchId ==
                        sourceIdea.recipe.bassPatchId &&
                    continuation.idea.recipe.supportPatchId ==
                        sourceIdea.recipe.supportPatchId &&
                    continuation.idea.recipe.drumPatchId ==
                        sourceIdea.recipe.drumPatchId &&
                    continuation.idea.recipe.progressionId.startsWith(
                        QStringLiteral("continue-")) &&
                    !continuation.analysis.relationshipId.isEmpty() &&
                    !continuation.analysis.continuationRoleId.isEmpty() &&
                    !continuation.analysis.harmonicPacingId.isEmpty() &&
                    continuation.analysis.inferredProfileConfidence == 1.0 &&
                    continuation.analysis.inferredTonalConfidence == 1.0 &&
                    continuation.analysis.candidateCount == 32 &&
                    (continuation.analysis.chordVocabularySimilarity <= 0.55 ||
                        continuation.analysis.chordQualityVocabularySimilarity <= 0.55) &&
                    continuation.analysis.chordOrderContrast >= 0.50 &&
                    continuation.analysis.openingChordPositionSimilarity <= 0.25 &&
                    continuation.analysis.drumSimilarity <= 0.72 &&
                    continuation.analysis.melodyContourSimilarity >= 0.0 &&
                    continuation.analysis.melodyContourSimilarity <= 1.0 &&
                    continuation.analysis.bassContourSimilarity >= 0.0 &&
                    continuation.analysis.bassContourSimilarity <= 1.0 &&
                    continuation.analysis.boundaryVoiceLeading >= 0.0 &&
                    continuation.analysis.boundaryVoiceLeading <= 1.0 &&
                    continuation.analysis.harmonicDensityRetention >= 0.65 &&
                    continuation.analysis.sourceChordEvents > 0 &&
                    continuation.analysis.continuationChordEvents > 0 &&
                    continuation.idea.recipe.motifTransformations.join(
                        QLatin1Char(' ')).contains(
                            QStringLiteral("return boundary")) &&
                    harmonyFitsKey(
                        continuation.idea.chordSection,
                        sourceIdea.recipe.tonic,
                        sourceIdea.recipe.mode);
                profileContinuationValid = profileContinuationValid && caseValid;
                profileContourTotal +=
                    continuation.analysis.melodyContourSimilarity;
                profileBassContourTotal +=
                    continuation.analysis.bassContourSimilarity;
                profileBoundaryTotal +=
                    continuation.analysis.boundaryVoiceLeading;
                profileRhythmTotal +=
                    continuation.analysis.melodyRhythmSimilarity;
                profilePacingIds.insert(
                    continuation.analysis.harmonicPacingId);
                continuationRoleIds.insert(
                    continuation.analysis.continuationRoleId);
                const QString progressionId =
                    continuation.idea.recipe.progressionId;
                profileProgressionFamilies.insert(
                    progressionId.left(progressionId.lastIndexOf(QLatin1Char('-'))));
                if (!caseValid && profileFailures.size() < 12) {
                    profileFailures << QStringLiteral(
                        "%1 src=%2/%3 dst=%4/%5 progression=%6 pacing=%7 contour=%8% density=%9% (%10->%11) chord=%12% opening=%13% drums=%14%")
                        .arg(item.profileId,
                            sourceIdea.recipe.styleId,
                            sourceIdea.recipe.profileId,
                            continuation.idea.recipe.styleId,
                            continuation.idea.recipe.profileId,
                            continuation.idea.recipe.progressionId,
                            continuation.analysis.harmonicPacingId)
                        .arg(qRound(
                            continuation.analysis.melodyContourSimilarity * 100.0))
                        .arg(qRound(
                            continuation.analysis.harmonicDensityRetention * 100.0))
                        .arg(continuation.analysis.sourceChordEvents)
                        .arg(continuation.analysis.continuationChordEvents)
                        .arg(qRound(
                            continuation.analysis.chordVocabularySimilarity * 100.0))
                        .arg(qRound(
                            continuation.analysis.openingChordPositionSimilarity * 100.0))
                        .arg(qRound(
                            continuation.analysis.drumSimilarity * 100.0));
                }
            }
            const int profileContinuationCount =
                qMax(1, continuationProfiles.size());
            QStringList measuredRoleIds = continuationRoleIds.values();
            measuredRoleIds.sort();
            record(QStringLiteral(
                       "practice.continue-every-profile-uses-style-pacing-and-melodic-development"),
                profileContinuationValid &&
                    continuationProfiles.size() == 27 &&
                    profilePacingIds.size() >= 16 &&
                    profileProgressionFamilies.size() >= 16 &&
                    continuationRoleIds.size() == 5,
                QStringLiteral(
                    "%1 profiles; %2 pacing plans; %3 harmonic families; roles=%4; mean melody contour=%5%, bass contour=%6%, rhythm=%7%, boundary=%8%")
                    .arg(continuationProfiles.size())
                    .arg(profilePacingIds.size())
                    .arg(profileProgressionFamilies.size())
                    .arg(measuredRoleIds.join(QLatin1Char(',')))
                    .arg(qRound(100.0 * profileContourTotal /
                        profileContinuationCount))
                    .arg(qRound(100.0 * profileBassContourTotal /
                        profileContinuationCount))
                    .arg(qRound(100.0 * profileRhythmTotal /
                        profileContinuationCount))
                    .arg(qRound(100.0 * profileBoundaryTotal /
                        profileContinuationCount)) +
                    (profileFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            profileFailures.join(QStringLiteral(" | ")))));

            QStringList iteratedStyles = jam2::practice::styleIds();
            iteratedStyles << QStringLiteral("metal-experimental");
            const QStringList iterationModes{
                QStringLiteral("ionian"), QStringLiteral("aeolian"),
                QStringLiteral("dorian"), QStringLiteral("mixolydian"),
                QStringLiteral("lydian"), QStringLiteral("phrygian")};
            bool multiStyleIterationValid = true;
            QSet<QString> multiStyleFingerprints;
            QSet<QString> multiStyleProgressions;
            QSet<QString> multiStyleRoles;
            QStringList multiStyleFailures;
            int multiStyleRounds = 0;
            for (int styleIndex = 0;
                 styleIndex < iteratedStyles.size(); ++styleIndex) {
                for (int iteration = 0; iteration < 4; ++iteration) {
                    jam2::practice::ChordIdeaRequest sourceRequest;
                    sourceRequest.styleId = iteratedStyles.at(styleIndex);
                    if (sourceRequest.styleId == QStringLiteral("metal-experimental")) {
                        sourceRequest.profileId =
                            QStringLiteral("metal_modern_progressive");
                    }
                    sourceRequest.key = (styleIndex * 7 + iteration * 3) % 12;
                    sourceRequest.modeId = iterationModes.at(
                        (styleIndex + iteration) % iterationModes.size());
                    sourceRequest.allowModeOverride = true;
                    sourceRequest.meterId = QStringLiteral("4-4");
                    sourceRequest.allowMeterOverride = true;
                    sourceRequest.bpm = 76 +
                        (styleIndex * 13 + iteration * 17) % 105;
                    sourceRequest.bars = iteration % 2 == 0 ? 8 : 16;
                    const std::uint32_t seed = static_cast<std::uint32_t>(
                        40000 + styleIndex * 100 + iteration);
                    const auto sourceIdea =
                        jam2::practice::generateCoupledPracticeIdeaForTest(
                            sourceRequest, seed);
                    const SongSection source = combinedIdeaSection(sourceIdea);
                    const auto continuation =
                        jam2::practice::generateContinuationPracticeIdeaForTest(
                            source, requestFor(source), seed + 500U);
                    const bool caseValid =
                        sourceIdea.recipe.isValid() &&
                        continuation.idea.recipe.isValid() &&
                        continuation.idea.recipe.styleId ==
                            sourceIdea.recipe.styleId &&
                        continuation.idea.recipe.profileId ==
                            sourceIdea.recipe.profileId &&
                        continuation.idea.recipe.tonic == sourceIdea.recipe.tonic &&
                        continuation.idea.recipe.mode == sourceIdea.recipe.mode &&
                        continuation.idea.recipe.formId == sourceIdea.recipe.formId &&
                        continuation.idea.chordSection.beats == source.beats &&
                        continuation.idea.beatSection.beats == source.beats &&
                        continuation.idea.recipe.drumPatchId ==
                            sourceIdea.recipe.drumPatchId &&
                        !continuation.analysis.continuationRoleId.isEmpty() &&
                        continuation.analysis.inferredProfileConfidence == 1.0 &&
                        continuation.analysis.inferredTonalConfidence == 1.0 &&
                        continuation.analysis.harmonicDensityRetention >= 0.65 &&
                        (continuation.analysis.chordVocabularySimilarity <= 0.55 ||
                            continuation.analysis.chordQualityVocabularySimilarity <= 0.55) &&
                        continuation.analysis.chordOrderContrast >= 0.50 &&
                        continuation.analysis.openingChordPositionSimilarity <= 0.25 &&
                        continuation.analysis.drumSimilarity <= 0.72 &&
                        continuation.idea.recipe.motifTransformations.join(
                            QLatin1Char(' ')).contains(
                                QStringLiteral("return boundary")) &&
                        harmonyFitsKey(
                            continuation.idea.chordSection,
                            sourceIdea.recipe.tonic,
                            sourceIdea.recipe.mode);
                    multiStyleIterationValid =
                        multiStyleIterationValid && caseValid;
                    ++multiStyleRounds;
                    multiStyleFingerprints.insert(
                        jam2::practice::generatedChordFingerprint(
                            continuation.idea.chordSection));
                    multiStyleProgressions.insert(
                        continuation.idea.recipe.progressionId);
                    multiStyleRoles.insert(
                        continuation.analysis.continuationRoleId);
                    if (!caseValid && multiStyleFailures.size() < 12) {
                        multiStyleFailures << QStringLiteral(
                            "%1/%2 src=%3-%4/%5 dst=%6-%7/%8 role=%9")
                            .arg(sourceRequest.styleId)
                            .arg(iteration)
                            .arg(sourceIdea.recipe.tonic,
                                sourceIdea.recipe.mode,
                                sourceIdea.recipe.profileId,
                                continuation.idea.recipe.tonic,
                                continuation.idea.recipe.mode,
                                continuation.idea.recipe.profileId,
                                continuation.analysis.continuationRoleId);
                    }
                }
            }
            record(QStringLiteral(
                       "practice.continue-multiple-rounds-per-style-remain-source-specific"),
                multiStyleIterationValid &&
                    iteratedStyles.size() == 14 &&
                    multiStyleRounds == 56 &&
                    multiStyleFingerprints.size() >= 52 &&
                    multiStyleProgressions.size() >= 30 &&
                    multiStyleRoles.size() == 5,
                QStringLiteral(
                    "%1 rounds across %2 styles; %3 distinct B harmonies, %4 progression variants, %5 roles")
                    .arg(multiStyleRounds)
                    .arg(iteratedStyles.size())
                    .arg(multiStyleFingerprints.size())
                    .arg(multiStyleProgressions.size())
                    .arg(multiStyleRoles.size()) +
                    (multiStyleFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            multiStyleFailures.join(QStringLiteral(" | ")))));

            const auto manualSection = [](const QStringList& chords, int beatsPerBar) {
                SongSection section;
                section.label = QStringLiteral("A");
                section.name = QStringLiteral("Manual A");
                section.beats = beatsPerBar * 8;
                section.chords.fill(QString(), section.beats);
                section.targets.fill(QString(), section.beats);
                section.beatNotes.fill(QString(), section.beats);
                section.beatPatterns.resize(section.beats);
                section.musicalPatterns.resize(section.beats);
                const int chordSpan = qMax(1, section.beats / qMax(1, chords.size()));
                for (int index = 0; index < chords.size(); ++index) {
                    section.chords[qMin(section.beats - 1, index * chordSpan)] = chords.at(index);
                }
                for (int beat = 0; beat < section.beats; ++beat) {
                    BeatPattern& drums = section.beatPatterns[beat];
                    drums.division = 4;
                    drums.lanes.fill(QStringLiteral("...."), BeatGridModel::beatLaneNames().size());
                    if (beat % beatsPerBar == 0 || beat % beatsPerBar == 2) drums.lanes[0][0] = QLatin1Char('x');
                    if (beat % beatsPerBar == 1 || beat % beatsPerBar == 3) drums.lanes[1][0] = QLatin1Char('x');
                    drums.lanes[2] = QStringLiteral("x.x.");
                    MusicalBeatPattern& music = section.musicalPatterns[beat];
                    music.division = 2;
                    music.chords.fill(MusicalStep{}, 2);
                    music.melody.fill(MusicalStep{}, 2);
                    music.bass.fill(MusicalStep{}, 2);
                    music.support.fill(MusicalStep{}, 2);
                    if (beat % (beatsPerBar * 2) == 0) {
                        music.melody[0] = {MusicalStepState::Onset,
                            beat % (beatsPerBar * 4) == 0
                                ? QStringLiteral("E4") : QStringLiteral("G4"),
                            88, QString()};
                    }
                }
                return section;
            };
            struct ManualCase {
                QString name;
                QStringList chords;
                QString meterId;
                int beatsPerBar = 4;
                int bpm = 112;
            };
            const QVector<ManualCase> manualCases{
                {QStringLiteral("plain-diatonic"),
                    {QStringLiteral("C"), QStringLiteral("Am"), QStringLiteral("F"), QStringLiteral("G")},
                    QStringLiteral("4-4"), 4, 108},
                {QStringLiteral("modal-minor"),
                    {QStringLiteral("Dm"), QStringLiteral("C"), QStringLiteral("Bb"), QStringLiteral("C")},
                    QStringLiteral("4-4"), 4, 96},
                {QStringLiteral("modal-dorian-rnb"),
                    {QStringLiteral("Am7"), QStringLiteral("D7"), QStringLiteral("Am7"), QStringLiteral("D7")},
                    QStringLiteral("4-4"), 4, 98},
                {QStringLiteral("manual-power-riff"),
                    {QStringLiteral("D5"), QStringLiteral("F5"), QStringLiteral("C5"), QStringLiteral("D5")},
                    QStringLiteral("4-4"), 4, 132},
                {QStringLiteral("extended-nonswing"),
                    {QStringLiteral("Ebmaj9"), QStringLiteral("Cm9"), QStringLiteral("Abmaj7"), QStringLiteral("Bb13")},
                    QStringLiteral("4-4"), 4, 82},
            };
            bool manualContinuationValid = true;
            double manualChordTotal = 0.0;
            double manualContrastTotal = 0.0;
            double manualDrumTotal = 0.0;
            QStringList manualInferences;
            for (int index = 0; index < manualCases.size(); ++index) {
                const ManualCase& item = manualCases.at(index);
                const SongSection source = manualSection(item.chords, item.beatsPerBar);
                jam2::practice::ContinueIdeaRequest request;
                request.sourceSectionIndex = 0;
                request.targetSectionIndex = 1;
                request.bpm = item.bpm;
                request.meterId = item.meterId;
                request.beatsPerBar = item.beatsPerBar;
                const QString sourceChord = jam2::practice::generatedChordFingerprint(source);
                const QString sourceBeat = jam2::practice::generatedBeatFingerprint(source);
                const auto continuation = jam2::practice::generateContinuationPracticeIdeaForTest(
                    source, request, static_cast<std::uint32_t>(23000 + index));
                const auto& analysis = continuation.analysis;
                bool parseable = true;
                for (const QString& symbol : continuation.idea.chordSection.chords) {
                    if (!symbol.trimmed().isEmpty() && symbol != QStringLiteral("-")) {
                        parseable = parseable && jam2::practice::parseChord(symbol).valid;
                    }
                }
                manualContinuationValid = manualContinuationValid &&
                    sourceChord == jam2::practice::generatedChordFingerprint(source) &&
                    sourceBeat == jam2::practice::generatedBeatFingerprint(source) &&
                    continuation.idea.recipe.isValid() && parseable &&
                    continuation.idea.recipe.bpm == item.bpm &&
                    continuation.idea.recipe.meterId == item.meterId &&
                    continuation.idea.chordSection.beats == source.beats &&
                    harmonyFitsKey(
                        continuation.idea.chordSection,
                        analysis.inferredTonic,
                        analysis.inferredMode) &&
                    analysis.chordVocabularySimilarity <= 0.55 &&
                    analysis.chordOrderContrast >= 0.50 &&
                    analysis.openingChordPositionSimilarity <= 0.25 &&
                    analysis.drumSimilarity <= 0.72 &&
                    !analysis.inferredTonic.isEmpty() &&
                    !analysis.inferredMode.isEmpty() &&
                    !analysis.inferredProfileId.isEmpty();
                manualChordTotal += analysis.chordVocabularySimilarity;
                manualContrastTotal += analysis.chordOrderContrast;
                manualDrumTotal += analysis.drumSimilarity;
                manualInferences << QStringLiteral("%1:%2-%3/%4")
                    .arg(item.name, analysis.inferredTonic,
                        analysis.inferredMode, analysis.inferredProfileId);
            }
            const int manualCount = qMax(1, manualCases.size());
            record(QStringLiteral("practice.continue-manual-a-corpus-infers-and-develops-material"),
                manualContinuationValid,
                QStringLiteral(
                    "%1 manual A→B rounds; mean chord=%2%, order contrast=%3%, drums=%4%; inference=%5")
                    .arg(manualCases.size())
                    .arg(qRound(100.0 * manualChordTotal / manualCount))
                    .arg(qRound(100.0 * manualContrastTotal / manualCount))
                    .arg(qRound(100.0 * manualDrumTotal / manualCount))
                    .arg(manualInferences.join(QStringLiteral(", "))));

            struct AuthoredGrooveCase {
                QString name;
                QString expectedProfile;
                QStringList chords;
                int kind = 0;
            };
            const QVector<AuthoredGrooveCase> authoredGrooves{
                {QStringLiteral("straight-backbeat"), QStringLiteral("pop_loop"),
                    {QStringLiteral("C"), QStringLiteral("Am"), QStringLiteral("F"), QStringLiteral("G")}, 0},
                {QStringLiteral("four-on-floor"), QStringLiteral("electronic_house"),
                    {QStringLiteral("C"), QStringLiteral("Am"), QStringLiteral("F"), QStringLiteral("G")}, 1},
                {QStringLiteral("half-time-dense-hats"), QStringLiteral("hiphop_trap"),
                    {QStringLiteral("Dm"), QStringLiteral("C"), QStringLiteral("Bb"), QStringLiteral("C")}, 2},
            };
            bool authoredGroovesValid = true;
            QStringList authoredGrooveDetails;
            const auto bodyDrumGridDifferences = [](
                const SongSection& source,
                const SongSection& continuation,
                int beatsPerBar) {
                const int comparableBeats = qMax(
                    0, qMin(source.beats, continuation.beats) -
                        qMax(1, beatsPerBar));
                int differences = 0;
                for (int beat = 0; beat < comparableBeats; ++beat) {
                    const BeatPattern& left = source.beatPatterns.value(beat);
                    const BeatPattern& right =
                        continuation.beatPatterns.value(beat);
                    const int lanes = qMax(left.lanes.size(), right.lanes.size());
                    for (int lane = 0; lane < lanes; ++lane) {
                        const QString leftSteps = left.lanes.value(lane);
                        const QString rightSteps = right.lanes.value(lane);
                        const int steps = qMax(
                            leftSteps.size(), rightSteps.size());
                        for (int step = 0; step < steps; ++step) {
                            const QChar leftState = step < leftSteps.size()
                                ? leftSteps.at(step) : QLatin1Char('.');
                            const QChar rightState = step < rightSteps.size()
                                ? rightSteps.at(step) : QLatin1Char('.');
                            if (leftState != rightState) {
                                ++differences;
                            }
                        }
                    }
                }
                return differences;
            };
            for (int index = 0; index < authoredGrooves.size(); ++index) {
                const AuthoredGrooveCase& item = authoredGrooves.at(index);
                SongSection source = manualSection(item.chords, 4);
                for (int beat = 0; beat < source.beats; ++beat) {
                    BeatPattern& pattern = source.beatPatterns[beat];
                    pattern.division = 4;
                    pattern.lanes.fill(
                        QStringLiteral("...."), BeatGridModel::beatLaneNames().size());
                    const int beatInBar = beat % 4;
                    if (item.kind == 1) {
                        pattern.lanes[0] = QStringLiteral("x...");
                        if (beatInBar == 1 || beatInBar == 3)
                            pattern.lanes[1] = QStringLiteral("x...");
                        pattern.lanes[2] = QStringLiteral("x.x.");
                    } else if (item.kind == 2) {
                        if (beatInBar == 0 || beatInBar == 2)
                            pattern.lanes[0] = beatInBar == 0
                                ? QStringLiteral("x..x") : QStringLiteral("x.x.");
                        if (beatInBar == 2)
                            pattern.lanes[1] = QStringLiteral("x...");
                        pattern.lanes[2] = QStringLiteral("xxxx");
                    } else {
                        if (beatInBar == 0 || beatInBar == 2)
                            pattern.lanes[0] = QStringLiteral("x...");
                        if (beatInBar == 1 || beatInBar == 3)
                            pattern.lanes[1] = QStringLiteral("x...");
                        pattern.lanes[2] = QStringLiteral("x.x.");
                    }
                }
                jam2::practice::ContinueIdeaRequest request;
                request.sourceSectionIndex = 0;
                request.targetSectionIndex = 1;
                request.bpm = item.kind == 1 ? 124 : item.kind == 2 ? 142 : 108;
                request.meterId = QStringLiteral("4-4");
                request.beatsPerBar = 4;
                const auto continuation =
                    jam2::practice::generateContinuationPracticeIdeaForTest(
                        source, request, static_cast<std::uint32_t>(23500 + index));
                const auto& analysis = continuation.analysis;
                const int bodyDifferences = bodyDrumGridDifferences(
                    source, continuation.idea.beatSection, 4);
                const bool caseValid =
                    continuation.idea.recipe.isValid() &&
                    analysis.inferredProfileId == item.expectedProfile &&
                    analysis.drumSimilarity <= 0.72 &&
                    bodyDifferences >= 2 &&
                    analysis.chordOrderContrast > 0.0 &&
                    harmonyFitsKey(
                        continuation.idea.chordSection,
                        analysis.inferredTonic,
                        analysis.inferredMode);
                authoredGroovesValid = authoredGroovesValid && caseValid;
                authoredGrooveDetails << QStringLiteral(
                    "%1:%2 d=%3% c=%4% body-delta=%5")
                    .arg(item.name, analysis.inferredProfileId)
                    .arg(qRound(analysis.drumSimilarity * 100.0))
                    .arg(qRound(analysis.chordOrderContrast * 100.0))
                    .arg(bodyDifferences);
            }
            record(QStringLiteral(
                       "practice.continue-manual-drum-feel-keeps-pocket-with-variation"),
                authoredGroovesValid,
                authoredGrooveDetails.join(QStringLiteral(", ")));

            const QVector<QStringList> sparseHarmonySets{
                {QStringLiteral("C5")},
                {QStringLiteral("Cmaj7"), QStringLiteral("F#maj7")},
                {QStringLiteral("Em"), QStringLiteral("Bb"), QStringLiteral("C#m")},
                {QStringLiteral("Dsus2"), QStringLiteral("G5"), QStringLiteral("Bbmaj7")},
                {QStringLiteral("Abm7"), QStringLiteral("Db7")},
                {QStringLiteral("E5"), QStringLiteral("F5"), QStringLiteral("D5")},
                {QStringLiteral("Gmaj7"), QStringLiteral("Bbm7"), QStringLiteral("Dbmaj7"), QStringLiteral("E7")},
                {},
            };
            const QStringList sparseNotes{
                QStringLiteral("C4"), QStringLiteral("Eb4"), QStringLiteral("F#4"),
                QStringLiteral("A4"), QStringLiteral("D4"), QStringLiteral("G4"),
                QStringLiteral("Bb4"), QStringLiteral("E4"), QStringLiteral("C#5")};
            bool sparseManualValid = true;
            QSet<QString> sparseFingerprints;
            QSet<QString> sparseProfiles;
            QSet<QString> sparseRoles;
            double sparseTonalConfidenceTotal = 0.0;
            double sparseProfileConfidenceTotal = 0.0;
            double sparseMelodyContourTotal = 0.0;
            int transformedMotifCases = 0;
            QStringList sparseFailures;
            constexpr int kSparseManualRounds = 32;
            for (int index = 0; index < kSparseManualRounds; ++index) {
                SongSection source;
                source.label = QStringLiteral("A");
                source.name = QStringLiteral("Sparse Manual %1").arg(index + 1);
                const int bars = 4 + (index % 3) * 2;
                source.beats = bars * 4;
                source.chords.fill(QString(), source.beats);
                source.targets.fill(QString(), source.beats);
                source.beatNotes.fill(QString(), source.beats);
                source.beatPatterns.resize(source.beats);
                source.musicalPatterns.resize(source.beats);
                const QStringList& chords =
                    sparseHarmonySets.at(index % sparseHarmonySets.size());
                for (int chord = 0; chord < chords.size(); ++chord) {
                    const int beat = qMin(source.beats - 1,
                        chord * qMax(1, source.beats / chords.size()));
                    source.chords[beat] = chords.at(
                        (chord + index / sparseHarmonySets.size()) % chords.size());
                }
                int sourceMelodyOnsets = 0;
                for (int beat = 0; beat < source.beats; ++beat) {
                    BeatPattern& drums = source.beatPatterns[beat];
                    drums.division = 4;
                    drums.lanes.fill(
                        QStringLiteral("...."),
                        BeatGridModel::beatLaneNames().size());
                    const int beatInBar = beat % 4;
                    if ((beat + index) % (3 + index % 3) == 0)
                        drums.lanes[0][(index + beat) % 2 == 0 ? 0 : 2] = QLatin1Char('x');
                    if ((beatInBar == 1 || beatInBar == 3) && index % 4 != 0)
                        drums.lanes[1][0] = QLatin1Char('x');
                    if (index % 5 != 0 && beat % 2 == 0)
                        drums.lanes[2] = index % 2 == 0
                            ? QStringLiteral("x.x.") : QStringLiteral("x...");

                    MusicalBeatPattern& music = source.musicalPatterns[beat];
                    music.division = 4;
                    music.chords.fill(MusicalStep{}, 4);
                    music.melody.fill(MusicalStep{}, 4);
                    music.bass.fill(MusicalStep{}, 4);
                    music.support.fill(MusicalStep{}, 4);
                    const int melodyModulo = 3 + index % 5;
                    if ((beat * 3 + index) % melodyModulo == 0 &&
                        sourceMelodyOnsets < 2 + index % 6) {
                        const int step = (beat + index) % 3;
                        music.melody[step] = {
                            MusicalStepState::Onset,
                            sparseNotes.at((beat + index * 2) % sparseNotes.size()),
                            72 + (index * 5 + beat) % 30,
                            QStringLiteral("manual")};
                        ++sourceMelodyOnsets;
                    }
                    if ((beat + index) % (4 + index % 4) == 0) {
                        QString bassNote = sparseNotes.at(
                            (index + beat * 2) % sparseNotes.size());
                        if (!bassNote.isEmpty()) {
                            bassNote.chop(1);
                            bassNote += QLatin1Char('2');
                        }
                        music.bass[0] = {
                            MusicalStepState::Onset,
                            bassNote,
                            78, QStringLiteral("manual")};
                    }
                }
                const QString sourceChord =
                    jam2::practice::generatedChordFingerprint(source);
                const QString sourceBeat =
                    jam2::practice::generatedBeatFingerprint(source);
                jam2::practice::ContinueIdeaRequest request;
                request.sourceSectionIndex = 0;
                request.targetSectionIndex = 1;
                request.bpm = 72 + (index * 11) % 91;
                request.meterId = QStringLiteral("4-4");
                request.beatsPerBar = 4;
                const std::uint32_t continuationSeed =
                    static_cast<std::uint32_t>(36000 + index);
                const auto continuation =
                    jam2::practice::generateContinuationPracticeIdeaForTest(
                        source, request, continuationSeed);
                const auto repeated =
                    jam2::practice::generateContinuationPracticeIdeaForTest(
                        source, request, continuationSeed);
                const auto& analysis = continuation.analysis;
                const QString targetChord =
                    jam2::practice::generatedChordFingerprint(
                        continuation.idea.chordSection);
                const bool motifExpected = sourceMelodyOnsets >= 2;
                const bool motifTransformed =
                    continuation.idea.recipe.motifTransformations.join(
                        QLatin1Char(' ')).contains(
                            QStringLiteral("return boundary"));
                const bool caseValid =
                    sourceChord == jam2::practice::generatedChordFingerprint(source) &&
                    sourceBeat == jam2::practice::generatedBeatFingerprint(source) &&
                    continuation.idea.recipe.isValid() &&
                    continuation.idea.chordSection.beats == source.beats &&
                    continuation.idea.beatSection.beats == source.beats &&
                    targetChord != sourceChord &&
                    targetChord == jam2::practice::generatedChordFingerprint(
                        repeated.idea.chordSection) &&
                    jam2::practice::generatedBeatFingerprint(
                        continuation.idea.beatSection) ==
                        jam2::practice::generatedBeatFingerprint(
                            repeated.idea.beatSection) &&
                    !analysis.inferredTonic.isEmpty() &&
                    !analysis.inferredMode.isEmpty() &&
                    !analysis.inferredProfileId.isEmpty() &&
                    analysis.inferredTonalConfidence > 0.0 &&
                    analysis.inferredTonalConfidence < 1.0 &&
                    analysis.inferredProfileConfidence > 0.0 &&
                    analysis.inferredProfileConfidence < 1.0 &&
                    !analysis.alternativeProfileIds.isEmpty() &&
                    !analysis.continuationRoleId.isEmpty() &&
                    analysis.drumSimilarity <= 0.72 &&
                    (!motifExpected || motifTransformed) &&
                    harmonyFitsKey(
                        continuation.idea.chordSection,
                        analysis.inferredTonic,
                        analysis.inferredMode);
                sparseManualValid = sparseManualValid && caseValid;
                sparseFingerprints.insert(targetChord);
                sparseProfiles.insert(analysis.inferredProfileId);
                sparseRoles.insert(analysis.continuationRoleId);
                sparseTonalConfidenceTotal += analysis.inferredTonalConfidence;
                sparseProfileConfidenceTotal += analysis.inferredProfileConfidence;
                sparseMelodyContourTotal += analysis.melodyContourSimilarity;
                if (motifTransformed) ++transformedMotifCases;
                if (!caseValid && sparseFailures.size() < 12) {
                    sparseFailures << QStringLiteral(
                        "%1 chords=%2 melody=%3 key=%4-%5 tc=%6 pc=%7 profile=%8 role=%9 d=%10 motif=%11")
                        .arg(index)
                        .arg(chords.size())
                        .arg(sourceMelodyOnsets)
                        .arg(analysis.inferredTonic, analysis.inferredMode)
                        .arg(qRound(analysis.inferredTonalConfidence * 100.0))
                        .arg(qRound(analysis.inferredProfileConfidence * 100.0))
                        .arg(analysis.inferredProfileId,
                            analysis.continuationRoleId)
                        .arg(qRound(analysis.drumSimilarity * 100.0))
                        .arg(motifTransformed);
                }
            }
            record(QStringLiteral(
                       "practice.continue-random-sparse-chromatic-manual-corpus-responds-to-source"),
                sparseManualValid &&
                    sparseFingerprints.size() >= 28 &&
                    sparseProfiles.size() >= 4 &&
                    sparseRoles.size() >= 3 &&
                    transformedMotifCases >= 24,
                QStringLiteral(
                    "%1 rounds; %2 distinct B harmonies; profiles=%3 roles=%4 transformed motifs=%5; mean tonal confidence=%6%, profile confidence=%7%, melody contour=%8%")
                    .arg(kSparseManualRounds)
                    .arg(sparseFingerprints.size())
                    .arg(sparseProfiles.size())
                    .arg(sparseRoles.size())
                    .arg(transformedMotifCases)
                    .arg(qRound(100.0 * sparseTonalConfidenceTotal /
                        kSparseManualRounds))
                    .arg(qRound(100.0 * sparseProfileConfidenceTotal /
                        kSparseManualRounds))
                    .arg(qRound(100.0 * sparseMelodyContourTotal /
                        kSparseManualRounds)) +
                    (sparseFailures.isEmpty() ? QString() :
                        QStringLiteral("; failures=%1").arg(
                            sparseFailures.join(QStringLiteral(" | ")))));

            BeatGridModel controllerModel;
            const SongSection controllerSource = manualSection(
                {QStringLiteral("A"), QStringLiteral("D"), QStringLiteral("E"), QStringLiteral("A")}, 4);
            (void)controllerModel.replaceSection(0, controllerSource);
            const QString controllerSourceChord =
                jam2::practice::generatedChordFingerprint(controllerModel.section(0));
            jam2::practice::ContinueIdeaRequest controllerRequest;
            controllerRequest.sourceSectionIndex = 0;
            controllerRequest.targetSectionIndex = 1;
            controllerRequest.bpm = 118;
            controllerRequest.meterId = QStringLiteral("4-4");
            controllerRequest.beatsPerBar = 4;
            const auto controllerResult =
                jam2::practice::PracticeIdeaController::generateContinuation(
                    controllerModel, controllerRequest);
            record(QStringLiteral("practice.continue-controller-only-replaces-target-bank"),
                controllerResult.has_value() &&
                controllerSourceChord == jam2::practice::generatedChordFingerprint(
                    controllerModel.section(0)) &&
                controllerModel.section(1).generatedKind == QStringLiteral("practice") &&
                controllerModel.section(1).name == QStringLiteral("Continuation of Section A") &&
                controllerModel.section(1).generatedRecipe.isValid());
        }

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
            if (kind == QStringLiteral("country-extension"))
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
                            const int repeatedPitchLimit =
                                recipe.profileId ==
                                        QStringLiteral(
                                            "electronic_techno")
                                ? recipe.bars *
                                      recipe.beatsPerBar
                                : 4;
                            if ((!leapValid ||
                                 repeatedMelody >
                                     repeatedPitchLimit) &&
                                matrixDetail.isEmpty())
                                matrixDetail = QStringLiteral("melody motion style=%1 variation=%2 level=%3 leap=%4 repeats=%5")
                                    .arg(ids.at(style)).arg(variationCase).arg(complexity)
                                    .arg(std::abs(event.midi - previousMelody)).arg(repeatedMelody);
                            matrixValid = matrixValid &&
                                leapValid &&
                                repeatedMelody <=
                                    repeatedPitchLimit;
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
            const auto drumIdentity = [](const auto& events) {
                QSet<QString> result;
                for (const auto& event : events) {
                    result.insert(
                        QStringLiteral("%1:%2")
                            .arg(event.tick)
                            .arg(event.laneId));
                }
                return result;
            };
            const QSet<QString> simpleDrums = drumIdentity(
                simpleIdea.recipe.drumEvents);
            const QSet<QString> complexDrums = drumIdentity(
                complexIdea.recipe.drumEvents);
            int sharedDrums = 0;
            for (const QString& event : simpleDrums) {
                if (complexDrums.contains(event)) ++sharedDrums;
            }
            const int smallerPerformance = qMin(
                simpleDrums.size(), complexDrums.size());
            const bool boundedEnsembleResponse =
                simpleIdea.recipe.grooveId ==
                    complexIdea.recipe.grooveId &&
                simpleIdea.recipe.drumPatchId ==
                    complexIdea.recipe.drumPatchId &&
                sharedDrums * 100 >=
                    smallerPerformance * 88 &&
                std::abs(simpleHits - complexHits) <=
                    qMax(4, simpleHits / 8);
            densityValid = densityValid &&
                boundedEnsembleResponse;
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
        jam2::practice::ChordIdeaRequest reggaeFormRequest;
        reggaeFormRequest.styleId = QStringLiteral("reggae");
        reggaeFormRequest.profileId = QStringLiteral("reggae_roots");
        reggaeFormRequest.formId = QStringLiteral("reggae-16");
        reggaeFormRequest.harmonicComplexity = 1;
        reggaeFormRequest.rhythmicComplexity = 1;
        const auto reggaeFoundation =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                reggaeFormRequest, 3482698164U);
        reggaeFormRequest.harmonicComplexity = 4;
        reggaeFormRequest.rhythmicComplexity = 4;
        const auto reggaeDeveloped =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                reggaeFormRequest, 3482698164U);
        reggaeFormRequest.harmonicComplexity = 8;
        reggaeFormRequest.rhythmicComplexity = 8;
        const auto reggaeAdvanced =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                reggaeFormRequest, 3482698164U);
        const auto reggaeSupportRoles =
            [](const auto& generated) {
                QSet<QString> roles;
                for (const auto& event :
                     generated.recipe.supportingEvents) {
                    roles.insert(event.role);
                }
                return roles;
            };
        const auto reggaeHasPickup =
            [](const auto& generated) {
                return std::any_of(
                    generated.recipe.bassEvents.cbegin(),
                    generated.recipe.bassEvents.cend(),
                    [](const auto& event) {
                        return event.relationship.contains(
                            QStringLiteral(
                                "semitone pickup"));
                    });
            };
        const bool reggaeIdentityStable =
            reggaeFoundation.recipe.progressionId ==
                reggaeDeveloped.recipe.progressionId &&
            reggaeDeveloped.recipe.progressionId ==
                reggaeAdvanced.recipe.progressionId &&
            reggaeFoundation.recipe.grooveId ==
                reggaeDeveloped.recipe.grooveId &&
            reggaeDeveloped.recipe.grooveId ==
                reggaeAdvanced.recipe.grooveId &&
            reggaeFoundation.recipe.motifCell ==
                reggaeDeveloped.recipe.motifCell &&
            reggaeDeveloped.recipe.motifCell ==
                reggaeAdvanced.recipe.motifCell &&
            reggaeFoundation.recipe.bpm ==
                reggaeDeveloped.recipe.bpm &&
            reggaeDeveloped.recipe.bpm ==
                reggaeAdvanced.recipe.bpm;
        bool reggaeCompingVocabularyValid = true;
        int reggaeOffbeatAttacks = 0;
        bool reggaeDubUpperSilence = true;
        for (int beat = 0;
             beat <
                 reggaeAdvanced.chordSection
                     .musicalPatterns.size();
             ++beat) {
            const auto& pattern =
                reggaeAdvanced.chordSection
                    .musicalPatterns.at(beat);
            for (int step = 0;
                 step < pattern.chords.size();
                 ++step) {
                if (pattern.chords.at(step).state ==
                        MusicalStepState::Onset) {
                    const QString articulation =
                        pattern.chords.at(step).articulation;
                    reggaeCompingVocabularyValid =
                        reggaeCompingVocabularyValid &&
                        (articulation == QStringLiteral("short-offbeat") ||
                         articulation == QStringLiteral("open-sustain"));
                    if (step == pattern.division / 2) {
                        ++reggaeOffbeatAttacks;
                    }
                }
            }
            if (beat >= 32 && beat < 34) {
                reggaeDubUpperSilence =
                    reggaeDubUpperSilence &&
                    std::none_of(
                        pattern.chords.cbegin(),
                        pattern.chords.cend(),
                        [](const MusicalStep& step) {
                            return step.state ==
                                MusicalStepState::Onset;
                        }) &&
                    std::none_of(
                        pattern.melody.cbegin(),
                        pattern.melody.cend(),
                        [](const MusicalStep& step) {
                            return step.state ==
                                MusicalStepState::Onset;
                        });
            }
        }
        const int reggaeCycleTicks = 4 * 4 * 12;
        QSet<int> reggaeOpeningOnsets;
        QSet<int> reggaeReturnOnsets;
        for (const auto& event :
             reggaeAdvanced.recipe.melodyEvents) {
            if (event.tick < reggaeCycleTicks) {
                reggaeOpeningOnsets.insert(event.tick);
            } else if (event.tick >= 3 * reggaeCycleTicks) {
                reggaeReturnOnsets.insert(
                    event.tick - 3 * reggaeCycleTicks);
            }
        }
        const QSet<int> reggaeCommonOnsets =
            reggaeOpeningOnsets &
            reggaeReturnOnsets;
        const QSet<int> reggaeDifferentOnsets =
            (reggaeOpeningOnsets -
             reggaeReturnOnsets) |
            (reggaeReturnOnsets -
             reggaeOpeningOnsets);
        const bool reggaeCallRecall =
            !reggaeOpeningOnsets.isEmpty() &&
            reggaeCommonOnsets.size() >=
                qMax(
                    1,
                    reggaeOpeningOnsets.size() - 1) &&
            reggaeDifferentOnsets.size() <= 2;
        bool reggaeOneDropValid =
            reggaeAdvanced.recipe.grooveId !=
                QStringLiteral("reggae-one-drop");
        if (!reggaeOneDropValid) {
            bool beatOneKick = false;
            bool beatThreeKick = false;
            for (const auto& event :
                 reggaeAdvanced.recipe.drumEvents) {
                if (event.laneId !=
                        QStringLiteral("kick") ||
                    event.tick >= 48) {
                    continue;
                }
                beatOneKick =
                    beatOneKick || event.tick == 0;
                beatThreeKick =
                    beatThreeKick || event.tick == 24;
            }
            reggaeOneDropValid =
                !beatOneKick && beatThreeKick;
        }
        jam2::practice::ChordIdeaRequest steppersRequest =
            reggaeFormRequest;
        steppersRequest.formId =
            QStringLiteral("reggae-steppers-12");
        const auto steppersIdea =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                steppersRequest, 3482698164U);
        record(QStringLiteral(
            "practice.roots-reggae-preserves-riddim-interlock-and-develops-bass-response-dub-form"),
            reggaeIdentityStable &&
                reggaeFoundation.recipe.theoryDecisions
                    .isEmpty() &&
                reggaeDeveloped.recipe.theoryDecisions
                    .isEmpty() &&
                reggaeAdvanced.recipe.theoryDecisions
                    .isEmpty() &&
                reggaeFoundation.recipe.bassEvents.size() ==
                    32 &&
                reggaeDeveloped.recipe.bassEvents.size() ==
                    36 &&
                reggaeAdvanced.recipe.bassEvents.size() ==
                    36 &&
                !reggaeHasPickup(reggaeFoundation) &&
                reggaeHasPickup(reggaeDeveloped) &&
                reggaeHasPickup(reggaeAdvanced) &&
                reggaeSupportRoles(reggaeFoundation) ==
                    QSet<QString>{
                        QStringLiteral(
                            "support_comping")} &&
                reggaeSupportRoles(reggaeDeveloped)
                    .contains(
                        QStringLiteral(
                            "call_response")) &&
                reggaeSupportRoles(reggaeAdvanced)
                    .contains(
                        QStringLiteral(
                            "countermelody")) &&
                reggaeCompingVocabularyValid &&
                reggaeOffbeatAttacks > 0 &&
                reggaeDubUpperSilence &&
                reggaeCallRecall &&
                reggaeOneDropValid &&
                steppersIdea.recipe.grooveId ==
                    QStringLiteral(
                        "reggae-steppers"));
        jam2::practice::ChordIdeaRequest bossaRequest;
        bossaRequest.styleId =
            QStringLiteral("bossa-nova");
        bossaRequest.profileId =
            QStringLiteral("bossa_songbook");
        bossaRequest.formId =
            QStringLiteral("bossa-18");
        bossaRequest.harmonicComplexity = 1;
        bossaRequest.rhythmicComplexity = 1;
        const auto bossaFoundation =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bossaRequest, 350305450U);
        bossaRequest.harmonicComplexity = 4;
        bossaRequest.rhythmicComplexity = 4;
        const auto bossaDeveloped =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bossaRequest, 350305450U);
        bossaRequest.harmonicComplexity = 8;
        bossaRequest.rhythmicComplexity = 8;
        const auto bossaAdvanced =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                bossaRequest, 350305450U);
        const auto bossaSupportRoles =
            [](const auto& generated) {
                QSet<QString> roles;
                for (const auto& event :
                     generated.recipe.supportingEvents) {
                    roles.insert(event.role);
                }
                return roles;
            };
        const auto bossaHasApproach =
            [](const auto& generated) {
                return std::any_of(
                    generated.recipe.bassEvents.cbegin(),
                    generated.recipe.bassEvents.cend(),
                    [](const auto& event) {
                        return event.relationship.contains(
                            QStringLiteral(
                                "chromatic bass approach"));
                    });
            };
        const bool bossaIdentityStable =
            bossaFoundation.recipe.progressionId ==
                bossaDeveloped.recipe.progressionId &&
            bossaDeveloped.recipe.progressionId ==
                bossaAdvanced.recipe.progressionId &&
            bossaFoundation.recipe.grooveId ==
                bossaDeveloped.recipe.grooveId &&
            bossaDeveloped.recipe.grooveId ==
                bossaAdvanced.recipe.grooveId &&
            bossaFoundation.recipe.motifCell ==
                bossaDeveloped.recipe.motifCell &&
            bossaDeveloped.recipe.motifCell ==
                bossaAdvanced.recipe.motifCell &&
            bossaFoundation.recipe.bpm ==
                bossaDeveloped.recipe.bpm &&
            bossaDeveloped.recipe.bpm ==
                bossaAdvanced.recipe.bpm;
        const bool bossaFormValid =
            bossaAdvanced.recipe.formSections.size() == 4 &&
            bossaAdvanced.recipe.formSections.at(0).startBar == 1 &&
            bossaAdvanced.recipe.formSections.at(0).bars == 5 &&
            bossaAdvanced.recipe.formSections.at(1).startBar == 6 &&
            bossaAdvanced.recipe.formSections.at(1).bars == 5 &&
            bossaAdvanced.recipe.formSections.at(2).startBar == 11 &&
            bossaAdvanced.recipe.formSections.at(2).bars == 4 &&
            bossaAdvanced.recipe.formSections.at(3).startBar == 15 &&
            bossaAdvanced.recipe.formSections.at(3).bars == 4;
        const auto bossaBassValid =
            [](const auto& generated) {
                return generated.recipe.bassEvents.size() == 36 &&
                    std::count_if(
                        generated.recipe.bassEvents.cbegin(),
                        generated.recipe.bassEvents.cend(),
                        [](const auto& event) {
                            return event.tick % 24 == 18;
                        }) == 18;
            };
        bool bossaUpperCompingValid = true;
        for (const auto& pattern :
             bossaAdvanced.chordSection.musicalPatterns) {
            for (int step = 0;
                 step < pattern.chords.size();
                 ++step) {
                if (pattern.chords.at(step).state ==
                        MusicalStepState::Onset) {
                    const QString articulation =
                        pattern.chords.at(step).articulation;
                    bossaUpperCompingValid =
                        bossaUpperCompingValid &&
                        pattern.division == 4 &&
                        (articulation == QStringLiteral("soft-detached") ||
                         articulation == QStringLiteral("open-sustain"));
                }
            }
        }
        QSet<int> bossaOpeningOnsets;
        QSet<int> bossaAnswerOnsets;
        for (const auto& event :
             bossaAdvanced.recipe.melodyEvents) {
            if (event.tick < 48) {
                bossaOpeningOnsets.insert(event.tick);
            } else if (event.tick >= 120 &&
                       event.tick < 168) {
                bossaAnswerOnsets.insert(
                    event.tick - 120);
            }
        }
        const bool bossaCallRecall =
            bossaOpeningOnsets.size() <= 1 ||
            (bossaOpeningOnsets &
             bossaAnswerOnsets).size() * 2 >=
                bossaOpeningOnsets.size();
        record(QStringLiteral(
            "practice.bossa-preserves-binary-songbook-interlock-form-and-bounded-development"),
            bossaIdentityStable &&
                bossaFormValid &&
                bossaFoundation.recipe.theoryDecisions
                    .isEmpty() &&
                bossaDeveloped.recipe.theoryDecisions
                    .size() <= 1 &&
                bossaAdvanced.recipe.theoryDecisions
                    .size() <= 2 &&
                bossaBassValid(bossaFoundation) &&
                bossaBassValid(bossaDeveloped) &&
                bossaBassValid(bossaAdvanced) &&
                !bossaHasApproach(bossaFoundation) &&
                bossaHasApproach(bossaDeveloped) &&
                bossaHasApproach(bossaAdvanced) &&
                bossaFoundation.recipe.supportingEvents
                    .isEmpty() &&
                bossaSupportRoles(bossaDeveloped)
                    .contains(
                        QStringLiteral(
                            "support_comping")) &&
                bossaSupportRoles(bossaAdvanced)
                    .contains(
                        QStringLiteral(
                            "countermelody")) &&
                bossaUpperCompingValid &&
                bossaCallRecall &&
                bossaAdvanced.recipe.melodyEvents.size() <=
                    38);
        jam2::practice::ChordIdeaRequest metalRequest;
        metalRequest.styleId =
            QStringLiteral("metal-experimental");
        metalRequest.profileId =
            QStringLiteral(
                "metal_modern_progressive");
        metalRequest.formId =
            QStringLiteral("metal-contrast-18");
        metalRequest.harmonicComplexity = 1;
        metalRequest.rhythmicComplexity = 1;
        const auto metalFoundation =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                metalRequest, 3957204071U);
        metalRequest.harmonicComplexity = 4;
        metalRequest.rhythmicComplexity = 4;
        const auto metalDeveloped =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                metalRequest, 3957204071U);
        metalRequest.harmonicComplexity = 8;
        metalRequest.rhythmicComplexity = 8;
        const auto metalAdvanced =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                metalRequest, 3957204071U);
        const auto metalSupportRoles =
            [](const auto& generated) {
                QSet<QString> roles;
                for (const auto& event :
                     generated.recipe.supportingEvents) {
                    roles.insert(event.role);
                }
                return roles;
            };
        const bool metalIdentityStable =
            metalFoundation.recipe.progressionId ==
                metalDeveloped.recipe.progressionId &&
            metalDeveloped.recipe.progressionId ==
                metalAdvanced.recipe.progressionId &&
            metalFoundation.recipe.grooveId ==
                metalDeveloped.recipe.grooveId &&
            metalDeveloped.recipe.grooveId ==
                metalAdvanced.recipe.grooveId &&
            metalFoundation.recipe.motifCell ==
                metalDeveloped.recipe.motifCell &&
            metalDeveloped.recipe.motifCell ==
                metalAdvanced.recipe.motifCell &&
            metalFoundation.recipe.bpm ==
                metalDeveloped.recipe.bpm &&
            metalDeveloped.recipe.bpm ==
                metalAdvanced.recipe.bpm;
        const bool metalFormValid =
            metalAdvanced.recipe.formSections.size() == 3 &&
            metalAdvanced.recipe.formSections.at(0).startBar == 1 &&
            metalAdvanced.recipe.formSections.at(0).bars == 6 &&
            metalAdvanced.recipe.formSections.at(1).startBar == 7 &&
            metalAdvanced.recipe.formSections.at(1).bars == 8 &&
            metalAdvanced.recipe.formSections.at(2).startBar == 15 &&
            metalAdvanced.recipe.formSections.at(2).bars == 4;
        QSet<int> metalKickTicks;
        for (const auto& event :
             metalAdvanced.recipe.drumEvents) {
            if (event.laneId ==
                    QStringLiteral("kick")) {
                metalKickTicks.insert(event.tick);
            }
        }
        QSet<int> metalBassTicks;
        for (const auto& event :
             metalAdvanced.recipe.bassEvents) {
            metalBassTicks.insert(event.tick);
        }
        QSet<int> metalHeavyChordTicks;
        bool metalOpenState = false;
        int metalBassLinkedAttacks = 0;
        for (int beat = 0;
             beat <
                 metalAdvanced.chordSection
                     .musicalPatterns.size();
             ++beat) {
            const auto& pattern =
                metalAdvanced.chordSection
                    .musicalPatterns.at(beat);
            for (int step = 0;
                 step < pattern.chords.size();
                 ++step) {
                const MusicalStep& chord =
                    pattern.chords.at(step);
                if (chord.state !=
                    MusicalStepState::Onset) {
                    continue;
                }
                const int tick =
                    beat * 12 +
                    step * 12 /
                        qMax(1, pattern.division);
                if (metalBassTicks.contains(tick)) {
                    ++metalBassLinkedAttacks;
                }
                if (chord.articulation ==
                    QStringLiteral("open-sustain")) {
                    metalOpenState = true;
                } else if (
                    chord.articulation ==
                        QStringLiteral("palm-muted") ||
                    chord.articulation ==
                        QStringLiteral("open-accent") ||
                    chord.articulation ==
                        QStringLiteral("gated-choke")) {
                    metalHeavyChordTicks.insert(tick);
                }
            }
        }
        int metalLockedHeavyAttacks = 0;
        for (int tick : metalHeavyChordTicks) {
            if (metalKickTicks.contains(tick)) {
                ++metalLockedHeavyAttacks;
            }
        }
        const bool metalKickLock =
            !metalHeavyChordTicks.isEmpty() &&
            metalLockedHeavyAttacks > 0;
        const bool metalBassInterlock =
            metalBassLinkedAttacks > 0;
        const int metalFinalBarStart =
            17 * 9 * 12;
        bool metalFinalSubtraction = false;
        for (int tick : metalKickTicks) {
            if (tick >= metalFinalBarStart &&
                !metalHeavyChordTicks.contains(tick)) {
                metalFinalSubtraction = true;
                break;
            }
        }
        const auto metalMelodyDensity =
            [&metalAdvanced](
                int startBar, int bars) {
                const int start =
                    (startBar - 1) * 9 * 12;
                const int end =
                    start + bars * 9 * 12;
                return static_cast<double>(
                    std::count_if(
                        metalAdvanced.recipe
                            .melodyEvents.cbegin(),
                        metalAdvanced.recipe
                            .melodyEvents.cend(),
                        [start, end](
                            const auto& event) {
                            return event.tick >= start &&
                                event.tick < end;
                        })) /
                    qMax(1, bars);
            };
        const double metalHeavyLeadDensity =
            (metalMelodyDensity(1, 6) +
             metalMelodyDensity(15, 4)) /
            2.0;
        const double metalCleanLeadDensity =
            metalMelodyDensity(7, 8);
        record(QStringLiteral(
            "practice.modern-metal-preserves-riff-lock-heavy-clean-form-and-bounded-layers"),
            metalIdentityStable &&
                metalFormValid &&
                metalFoundation.recipe.theoryDecisions
                    .isEmpty() &&
                metalDeveloped.recipe.theoryDecisions
                    .isEmpty() &&
                metalAdvanced.recipe.theoryDecisions
                    .isEmpty() &&
                metalSupportRoles(metalFoundation) ==
                    QSet<QString>{
                        QStringLiteral("pad")} &&
                metalSupportRoles(metalDeveloped)
                    .contains(
                        QStringLiteral(
                            "hook_double")) &&
                metalSupportRoles(metalAdvanced)
                    .contains(
                        QStringLiteral(
                            "countermelody")) &&
                metalOpenState &&
                metalKickLock &&
                metalBassInterlock &&
                metalFinalSubtraction &&
                metalCleanLeadDensity >
                    metalHeavyLeadDensity);
        record(QStringLiteral("practice.v7-theory-decisions-are-bounded"), theoryBudgetsValid);
        record(QStringLiteral("practice.v7-complexity-is-an-unlocked-profile-palette"),
            cumulativePaletteValid && levelEightKinds.size() >= 4 &&
            levelEightKinds.contains(QStringLiteral("inversion")));
        record(QStringLiteral(
            "practice.v7-drum-core-is-stable-with-bounded-melodic-articulation"),
            densityValid);
        record(QStringLiteral("practice.v7-matched-complexity-keeps-seeded-tempo"),
            stableSeedTempoValid);
        record(QStringLiteral("practice.v7-matched-complexity-develops-seeded-motif"),
            stableSeedMotifValid);
        bool performanceEventsValid = true;
        bool researchedKitCatalogValid = true;
        bool researchedKitVoicesValid = true;
        bool twoBaseKitsValid = false;
        bool baseKitPieceDetailsValid = true;
        bool electronicSnareUsesDetailBanks = false;
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
        };
        QSet<QString> researchedKitIds;
        const QSet<QString> electronicProfiles{
            QStringLiteral("electronic_breakbeat"),
            QStringLiteral("electronic_house"),
            QStringLiteral("electronic_techno"),
            QStringLiteral("hiphop_boom_bap"),
            QStringLiteral("hiphop_trap"),
            QStringLiteral("jpop_anisong_rock"),
            QStringLiteral("jpop_idol_dance"),
            QStringLiteral("metal_modern_progressive"),
            QStringLiteral("rnb_contemporary_neosoul"),
            QStringLiteral("soul_classic_motown"),
        };
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
                kit->baseKitId == (electronicProfiles.contains(profile.id)
                    ? QStringLiteral("electronic")
                    : QStringLiteral("acoustic")) &&
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
                if (!piece) continue;
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
        const auto* acousticBase =
            jam2::practice::researchDrumKitForBase(
                QStringLiteral("acoustic"));
        const auto* electronicBase =
            jam2::practice::researchDrumKitForBase(
                QStringLiteral("electronic"));
        twoBaseKitsValid = acousticBase && electronicBase &&
            acousticBase->name == QStringLiteral("Acoustic") &&
            electronicBase->name == QStringLiteral("Electronic") &&
            acousticBase->pieces.size() == drumLaneIds.size() &&
            electronicBase->pieces.size() == drumLaneIds.size() &&
            jam2::practice::researchDrumPiece(
                *electronicBase, QStringLiteral("kick")) &&
            jam2::practice::researchDrumPiece(
                *electronicBase, QStringLiteral("kick"))
                ->intendedIdentity == QStringLiteral("Punchy electronic kick");
        const std::array<const jam2::practice::ResearchDrumKit*, 2> baseKits{
            acousticBase,
            electronicBase,
        };
        for (const auto* baseKit : baseKits) {
            baseKitPieceDetailsValid = baseKitPieceDetailsValid && baseKit;
            if (!baseKit) continue;
            for (const QString& laneId : drumLaneIds) {
                const auto* piece = jam2::practice::researchDrumPiece(
                    *baseKit, laneId);
                baseKitPieceDetailsValid =
                    baseKitPieceDetailsValid && piece &&
                    std::isfinite(piece ? piece->sourceLayerGain : 0.0f) &&
                    piece && piece->sourceLayerGain >= 0.0f &&
                    (piece->sourceLayerGain > 0.0001f ||
                     !piece->modalBands.isEmpty() ||
                     !piece->noiseBands.isEmpty());
                if (!piece) continue;
                for (const auto& band : piece->modalBands) {
                    baseKitPieceDetailsValid = baseKitPieceDetailsValid &&
                        std::isfinite(band.frequencyHz) &&
                        std::isfinite(band.detuneCents) &&
                        std::isfinite(band.level) &&
                        std::isfinite(band.decaySeconds) &&
                        band.frequencyHz > 0.0f &&
                        band.level >= 0.0f && band.decaySeconds > 0.0f;
                }
                for (const auto& band : piece->noiseBands) {
                    baseKitPieceDetailsValid = baseKitPieceDetailsValid &&
                        std::isfinite(band.frequencyHz) &&
                        std::isfinite(band.q) &&
                        std::isfinite(band.level) &&
                        std::isfinite(band.decaySeconds) &&
                        band.frequencyHz > 0.0f && band.q > 0.0f &&
                        band.level >= 0.0f && band.decaySeconds > 0.0f;
                }
                const auto rendered =
                    jam2::practice::renderResearchDrumVoices(
                        *baseKit,
                        {{
                            0,
                            laneId,
                            QStringLiteral("normal"),
                            96,
                            0,
                            0x7b4d1a23U,
                        }},
                        8192,
                        48000);
                double peak = 0.0;
                for (float sample : rendered.dry) {
                    baseKitPieceDetailsValid = baseKitPieceDetailsValid &&
                        std::isfinite(sample);
                    peak = qMax(peak, std::abs(sample));
                }
                baseKitPieceDetailsValid = baseKitPieceDetailsValid &&
                    peak > 0.000001 && peak < 2.0;
            }
        }
        if (electronicBase) {
            const auto* snare = jam2::practice::researchDrumPiece(
                *electronicBase, QStringLiteral("snare"));
            if (snare) {
                const auto full = jam2::practice::renderResearchDrumVoices(
                    *electronicBase,
                    {{0, QStringLiteral("snare"),
                      QStringLiteral("normal"), 96, 0, 0x421ec6d3U}},
                    8192,
                    48000);
                auto withoutDetail = *electronicBase;
                auto stripped = withoutDetail.pieces.find(
                    QStringLiteral("snare"));
                if (stripped != withoutDetail.pieces.end()) {
                    stripped->modalBands.clear();
                    stripped->noiseBands.clear();
                }
                const auto sourceOnly =
                    jam2::practice::renderResearchDrumVoices(
                        withoutDetail,
                        {{0, QStringLiteral("snare"),
                          QStringLiteral("normal"), 96, 0, 0x421ec6d3U}},
                        8192,
                        48000);
                double fullPeak = 0.0;
                double sourcePeak = 0.0;
                for (float sample : full.dry) {
                    fullPeak = qMax(fullPeak, std::abs(sample));
                }
                for (float sample : sourceOnly.dry) {
                    sourcePeak = qMax(sourcePeak, std::abs(sample));
                }
                electronicSnareUsesDetailBanks =
                    snare->sourceLayerGain <= 0.0001f &&
                    snare->modalBands.size() == 1 &&
                    snare->noiseBands.size() == 3 &&
                    fullPeak > 0.000001 && sourcePeak <= 0.000001;
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
                "practice.drum-catalog-has-only-acoustic-and-electronic-bases"),
            twoBaseKitsValid);
        record(
            QStringLiteral(
                "practice.base-drum-kits-render-all-pieces-with-full-detail"),
            baseKitPieceDetailsValid);
        record(
            QStringLiteral(
                "practice.electronic-snare-is-modal-noise-not-tonal-source"),
            electronicSnareUsesDetailBanks);
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
            loaded.section(0).drumKitId == idea.beatSection.drumKitId &&
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
                trapHarmonyRequest, 1872634584U);
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

        jam2::practice::ChordIdeaRequest countryWaltzRequest;
        countryWaltzRequest.styleId =
            QStringLiteral("country");
        countryWaltzRequest.profileId =
            QStringLiteral("country_honky_tonk");
        countryWaltzRequest.formId =
            QStringLiteral("country-waltz-24");
        countryWaltzRequest.harmonicComplexity = 1;
        countryWaltzRequest.rhythmicComplexity = 1;
        const auto countryWaltz =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    countryWaltzRequest,
                    2688135074U);
        bool waltzBassValid =
            countryWaltz.recipe.bassEvents.size() ==
            countryWaltz.recipe.bars * 2;
        for (const auto& event :
             countryWaltz.recipe.bassEvents) {
            const int beat =
                event.tick / 12 %
                countryWaltz.recipe.beatsPerBar;
            waltzBassValid =
                waltzBassValid &&
                (beat == 0 || beat == 2);
        }
        bool waltzFillsValid = true;
        for (const auto& phrase :
             countryWaltz.recipe.drumPhrases) {
            waltzFillsValid =
                waltzFillsValid &&
                (phrase.fillId.isEmpty() ||
                 phrase.fillId.startsWith(
                     QStringLiteral(
                         "country-waltz-")));
        }
        QHash<int, QSet<QString>>
            countryWaltzLanesByTick;
        for (const auto& event :
             countryWaltz.recipe.drumEvents) {
            if (!event.fill) {
                countryWaltzLanesByTick[
                    event.tick].insert(
                    event.laneId);
            }
        }
        bool waltzBackbeatValid = true;
        for (const QSet<QString>& lanes :
             countryWaltzLanesByTick) {
            waltzBackbeatValid =
                waltzBackbeatValid &&
                !(lanes.contains(
                      QStringLiteral("snare")) &&
                  lanes.contains(
                      QStringLiteral(
                          "cross_stick")));
        }
        QSet<int> waltzMelodyDegrees;
        for (const auto& event :
             countryWaltz.recipe.melodyEvents) {
            waltzMelodyDegrees.insert(
                ((event.midi -
                      countryWaltzRequest.key) %
                     12 +
                 12) %
                12);
        }
        record(QStringLiteral(
            "practice.country-waltz-routes-three-beat-bass-side-stick-and-fills"),
            countryWaltz.recipe.mode ==
                    QStringLiteral("Mixolydian") &&
                countryWaltz.recipe.grooveId ==
                    QStringLiteral(
                        "country-waltz") &&
                countryWaltz.recipe.beatsPerBar ==
                    3 &&
                waltzBassValid &&
                waltzFillsValid &&
                waltzBackbeatValid &&
                waltzMelodyDegrees.contains(10));

        jam2::practice::ChordIdeaRequest
            compoundCountryRequest;
        compoundCountryRequest.styleId =
            QStringLiteral("country");
        compoundCountryRequest.profileId =
            QStringLiteral(
                "country_contemporary");
        compoundCountryRequest.formId =
            QStringLiteral("country-pop-6-8");
        compoundCountryRequest.harmonicComplexity =
            1;
        compoundCountryRequest.rhythmicComplexity =
            1;
        const auto compoundCountry =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    compoundCountryRequest,
                    3194264253U);
        bool compoundCountryBassValid =
            compoundCountry.recipe.bassEvents.size() ==
            compoundCountry.recipe.bars * 2;
        for (const auto& event :
             compoundCountry.recipe.bassEvents) {
            const int beat =
                event.tick / 12 %
                compoundCountry.recipe.beatsPerBar;
            compoundCountryBassValid =
                compoundCountryBassValid &&
                (beat == 0 || beat == 3);
        }
        bool compoundCountryFillsValid = true;
        for (const auto& phrase :
             compoundCountry.recipe.drumPhrases) {
            compoundCountryFillsValid =
                compoundCountryFillsValid &&
                (phrase.fillId.isEmpty() ||
                 phrase.fillId.startsWith(
                     QStringLiteral(
                         "country-compound-")));
        }
        record(QStringLiteral(
            "practice.compound-country-routes-dotted-pulse-bass-groove-and-fills"),
            compoundCountry.recipe.grooveId ==
                    QStringLiteral(
                        "country-compound") &&
                compoundCountry.recipe.beatsPerBar ==
                    6 &&
                compoundCountry.recipe
                        .tempoPulseUnits ==
                    3 &&
                compoundCountryBassValid &&
                compoundCountryFillsValid);

        const auto countryTheoryValid =
            [](const jam2::practice::
                   GeneratedPracticeIdea& generated,
               int operationLimit,
               const QSet<QString>&
                   extensionSuffixes) {
                if (generated.recipe.theoryDecisions
                        .size() >
                    operationLimit) {
                    return false;
                }
                const QSet<QString> kinds{
                    QStringLiteral(
                        "country-extension"),
                    QStringLiteral("inversion"),
                    QStringLiteral(
                        "modal-interchange"),
                    QStringLiteral(
                        "passing-diminished"),
                    QStringLiteral(
                        "secondary-dominant"),
                };
                const auto& harmony =
                    generated.chordSection.chords;
                for (const auto& decision :
                     generated.recipe
                         .theoryDecisions) {
                    if (!kinds.contains(
                            decision.kind)) {
                        return false;
                    }
                    if (decision.beat < 0 ||
                        decision.beat >=
                            harmony.size() ||
                        harmony.at(decision.beat) !=
                            decision.afterChord) {
                        return false;
                    }
                    const auto parsed =
                        jam2::practice::parseChord(
                            harmony.at(
                                decision.beat));
                    if (!parsed.valid) return false;
                    if (decision.kind ==
                            QStringLiteral(
                                "country-extension") &&
                        !extensionSuffixes.contains(
                            parsed.suffix)) {
                        return false;
                    }
                    if (decision.kind !=
                        QStringLiteral("inversion")) {
                        continue;
                    }
                    if (parsed.bass < 0) {
                        return false;
                    }
                    int previousBeat =
                        decision.beat - 1;
                    while (previousBeat >= 0 &&
                           (harmony.at(previousBeat)
                                .isEmpty() ||
                            harmony.at(previousBeat) ==
                                QStringLiteral("-"))) {
                        --previousBeat;
                    }
                    int nextBeat =
                        decision.beat + 1;
                    while (nextBeat <
                               harmony.size() &&
                           (harmony.at(nextBeat)
                                .isEmpty() ||
                            harmony.at(nextBeat) ==
                                QStringLiteral("-"))) {
                        ++nextBeat;
                    }
                    if (previousBeat < 0) {
                        return false;
                    }
                    const auto before =
                        jam2::practice::parseChord(
                            decision.beforeChord);
                    const auto previous =
                        jam2::practice::parseChord(
                            harmony.at(
                                previousBeat));
                    const auto next =
                        nextBeat < harmony.size()
                        ? jam2::practice::parseChord(
                              harmony.at(nextBeat))
                        : jam2::practice::
                              ParsedChord{};
                    if (!before.valid ||
                        !previous.valid) {
                        return false;
                    }
                    const auto bass =
                        [](const auto& chord) {
                            return chord.bass >= 0
                                ? chord.bass
                                : chord.root;
                        };
                    const auto distance =
                        [](int left, int right) {
                            const int value =
                                std::abs(
                                    ((left - right) %
                                         12 +
                                     12) %
                                    12);
                            return qMin(
                                value, 12 - value);
                        };
                    int rootCost = distance(
                        bass(previous),
                        before.root);
                    int inversionCost = distance(
                        bass(previous),
                        parsed.bass);
                    if (next.valid && !next.rest) {
                        rootCost += distance(
                            before.root,
                            bass(next));
                        inversionCost += distance(
                            parsed.bass,
                            bass(next));
                    }
                    if (inversionCost >= rootCost) {
                        return false;
                    }
                }
                const auto& bassEvents =
                    generated.recipe.bassEvents;
                for (int index = 0;
                     index < bassEvents.size();
                     ++index) {
                    const auto& event =
                        bassEvents.at(index);
                    if (!event.relationship.contains(
                            QStringLiteral(
                                "phrase-edge walk"),
                            Qt::CaseInsensitive) &&
                        !event.relationship.contains(
                            QStringLiteral(
                                "restrained connecting "
                                "tone"),
                            Qt::CaseInsensitive)) {
                        continue;
                    }
                    if (index + 1 >=
                            bassEvents.size() ||
                        std::abs(
                            bassEvents.at(index + 1)
                                .midi -
                            event.midi) > 2) {
                        return false;
                    }
                }
                return true;
            };
        jam2::practice::ChordIdeaRequest
            advancedHonkyRequest =
                countryWaltzRequest;
        advancedHonkyRequest.formId =
            QStringLiteral("country-16");
        advancedHonkyRequest.harmonicComplexity =
            8;
        advancedHonkyRequest.rhythmicComplexity =
            8;
        const auto advancedHonky =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    advancedHonkyRequest,
                    3517937198U);
        jam2::practice::ChordIdeaRequest
            advancedCountryPopRequest =
                compoundCountryRequest;
        advancedCountryPopRequest.formId =
            QStringLiteral("country-pop-24");
        advancedCountryPopRequest
            .harmonicComplexity = 8;
        advancedCountryPopRequest
            .rhythmicComplexity = 8;
        const auto advancedCountryPop =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    advancedCountryPopRequest,
                    3818209117U);
        const bool advancedHonkyValid =
            countryTheoryValid(
                advancedHonky,
                3,
                QSet<QString>{
                    QStringLiteral("6"),
                    QStringLiteral("7")});
        const bool advancedCountryPopValid =
            countryTheoryValid(
                advancedCountryPop,
                4,
                QSet<QString>{
                    QStringLiteral("add9"),
                    QStringLiteral("m7"),
                    QStringLiteral("7")});
        record(QStringLiteral(
            "practice.country-complexity-uses-bounded-functional-operations-and-stepwise-bass"),
            advancedHonkyValid &&
                advancedCountryPopValid,
            QStringLiteral(
                "honky=%1/%2 country_pop=%3/%4")
                .arg(advancedHonkyValid)
                .arg(
                    advancedHonky.recipe
                        .theoryDecisions.size())
                .arg(advancedCountryPopValid)
                .arg(
                    advancedCountryPop.recipe
                        .theoryDecisions.size()));

        jam2::practice::ChordIdeaRequest
            compoundSoulRequest;
        compoundSoulRequest.styleId =
            QStringLiteral("rnb-soul");
        compoundSoulRequest.profileId =
            QStringLiteral(
                "soul_classic_motown");
        compoundSoulRequest.formId =
            QStringLiteral("soul-12-8");
        compoundSoulRequest.harmonicComplexity =
            1;
        compoundSoulRequest.rhythmicComplexity =
            1;
        const auto compoundSoul =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    compoundSoulRequest,
                    1692882116U);
        const bool compoundSoulBassValid =
            compoundSoul.recipe.bassEvents.size() ==
                compoundSoul.recipe.bars * 4 &&
            std::all_of(
                compoundSoul.recipe.bassEvents.cbegin(),
                compoundSoul.recipe.bassEvents.cend(),
                [](const auto& event) {
                    return event.tick % 12 == 0 &&
                        (event.tick / 12 % 12) % 3 ==
                            0;
                });
        const bool compoundSoulResponse =
            std::any_of(
                compoundSoul.recipe
                    .supportingEvents.cbegin(),
                compoundSoul.recipe
                    .supportingEvents.cend(),
                [](const auto& event) {
                    return event.role ==
                        QStringLiteral(
                            "call_response");
                });
        record(QStringLiteral(
            "practice.compound-soul-uses-dotted-pulse-bass-authored-groove-and-band-response"),
            compoundSoul.recipe.mode ==
                    QStringLiteral("Dorian") &&
                compoundSoul.recipe.grooveId ==
                    QStringLiteral("soul-12-8") &&
                compoundSoul.recipe
                        .tempoPulseUnits == 3 &&
                compoundSoulBassValid &&
                compoundSoulResponse);

        jam2::practice::ChordIdeaRequest
            neoSoulRequest;
        neoSoulRequest.styleId =
            QStringLiteral("rnb-soul");
        neoSoulRequest.profileId =
            QStringLiteral(
                "rnb_contemporary_neosoul");
        neoSoulRequest.formId =
            QStringLiteral("rnb-16");
        neoSoulRequest.harmonicComplexity = 1;
        neoSoulRequest.rhythmicComplexity = 1;
        const auto neoSoulFoundation =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    neoSoulRequest,
                    39451262U);
        neoSoulRequest.harmonicComplexity = 4;
        neoSoulRequest.rhythmicComplexity = 4;
        const auto neoSoulDeveloped =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    neoSoulRequest,
                    39451262U);
        neoSoulRequest.harmonicComplexity = 8;
        neoSoulRequest.rhythmicComplexity = 8;
        const auto neoSoulAdvanced =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    neoSoulRequest,
                    39451262U);
        const auto supportRoles =
            [](const auto& generated) {
                QSet<QString> result;
                for (const auto& event :
                     generated.recipe
                         .supportingEvents) {
                    result.insert(event.role);
                }
                return result;
            };
        const auto bassHasRelationship =
            [](const auto& generated,
               const QString& text) {
                return std::any_of(
                    generated.recipe
                        .bassEvents.cbegin(),
                    generated.recipe
                        .bassEvents.cend(),
                    [&text](const auto& event) {
                        return event.relationship
                            .contains(
                                text,
                                Qt::CaseInsensitive);
                    });
            };
        const bool neoSoulIdentityStable =
            neoSoulFoundation.recipe
                    .progressionId ==
                neoSoulDeveloped.recipe
                    .progressionId &&
            neoSoulDeveloped.recipe
                    .progressionId ==
                neoSoulAdvanced.recipe
                    .progressionId &&
            neoSoulFoundation.recipe.grooveId ==
                neoSoulDeveloped.recipe.grooveId &&
            neoSoulDeveloped.recipe.grooveId ==
                neoSoulAdvanced.recipe.grooveId &&
            neoSoulFoundation.recipe.motifCell ==
                neoSoulDeveloped.recipe.motifCell &&
            neoSoulDeveloped.recipe.motifCell ==
                neoSoulAdvanced.recipe.motifCell;
        const bool neoSoulComplexityValid =
            neoSoulFoundation.recipe
                    .theoryDecisions.isEmpty() &&
            neoSoulFoundation.recipe
                    .supportingEvents.isEmpty() &&
            neoSoulDeveloped.recipe
                    .theoryDecisions.size() == 1 &&
            neoSoulAdvanced.recipe
                    .theoryDecisions.size() <= 2 &&
            supportRoles(neoSoulDeveloped) ==
                QSet<QString>{
                    QStringLiteral("lead_harmony")} &&
            supportRoles(neoSoulAdvanced)
                .contains(
                    QStringLiteral("pad")) &&
            supportRoles(neoSoulAdvanced)
                .contains(
                    QStringLiteral(
                        "countermelody")) &&
            bassHasRelationship(
                neoSoulDeveloped,
                QStringLiteral(
                    "semitone pickup")) &&
            bassHasRelationship(
                neoSoulAdvanced,
                QStringLiteral(
                    "reverse-ninth"));
        record(QStringLiteral(
            "practice.neosoul-complexity-preserves-backbone-and-adds-voice-led-bass-and-support"),
            neoSoulIdentityStable &&
                neoSoulComplexityValid);

        const auto melodyCellAt =
            [](const jam2::practice::
                   GeneratedPracticeIdea& generated,
               int startTick,
               int cycleTicks) {
                QSet<int> result;
                for (const auto& event :
                     generated.recipe.melodyEvents) {
                    if (event.tick >= startTick &&
                        event.tick <
                            startTick + cycleTicks) {
                        result.insert(
                            event.tick - startTick);
                    }
                }
                return result;
            };

        jam2::practice::ChordIdeaRequest
            funkPocketRequest;
        funkPocketRequest.styleId =
            QStringLiteral("funk");
        funkPocketRequest.profileId =
            QStringLiteral("funk_static_pocket");
        funkPocketRequest.formId =
            QStringLiteral("funk-16");
        funkPocketRequest.harmonicComplexity = 1;
        funkPocketRequest.rhythmicComplexity = 1;
        const auto funkFoundation =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    funkPocketRequest,
                    2262587531U);
        funkPocketRequest.harmonicComplexity = 4;
        funkPocketRequest.rhythmicComplexity = 4;
        const auto funkDeveloped =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    funkPocketRequest,
                    2262587531U);
        funkPocketRequest.harmonicComplexity = 8;
        funkPocketRequest.rhythmicComplexity = 8;
        const auto funkAdvanced =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    funkPocketRequest,
                    2262587531U);
        const bool funkIdentityStable =
            funkFoundation.recipe.progressionId ==
                funkDeveloped.recipe.progressionId &&
            funkDeveloped.recipe.progressionId ==
                funkAdvanced.recipe.progressionId &&
            funkFoundation.recipe.grooveId ==
                funkDeveloped.recipe.grooveId &&
            funkDeveloped.recipe.grooveId ==
                funkAdvanced.recipe.grooveId &&
            funkFoundation.recipe.motifCell ==
                funkDeveloped.recipe.motifCell &&
            funkDeveloped.recipe.motifCell ==
                funkAdvanced.recipe.motifCell;
        const bool funkComplexityValid =
            funkFoundation.recipe
                    .theoryDecisions.isEmpty() &&
            funkDeveloped.recipe
                    .theoryDecisions.isEmpty() &&
            funkAdvanced.recipe
                    .theoryDecisions.isEmpty() &&
            funkFoundation.recipe
                    .supportingEvents.isEmpty() &&
            supportRoles(funkDeveloped) ==
                QSet<QString>{
                    QStringLiteral("horn_stab")} &&
            supportRoles(funkAdvanced)
                .contains(
                    QStringLiteral("horn_stab")) &&
            supportRoles(funkAdvanced)
                .contains(
                    QStringLiteral("call_response")) &&
            bassHasRelationship(
                funkDeveloped,
                QStringLiteral(
                    "chromatic pickup")) &&
            bassHasRelationship(
                funkAdvanced,
                QStringLiteral(
                    "chromatic pickup"));
        int funkReturnTick = -1;
        int funkBreakStartBar = -1;
        int funkBreakBars = 0;
        for (const auto& section :
             funkAdvanced.recipe.formSections) {
            if (section.label ==
                QStringLiteral("Return")) {
                funkReturnTick =
                    (section.startBar - 1) *
                    funkAdvanced.recipe
                        .beatsPerBar * 12;
            } else if (section.label ==
                       QStringLiteral("Break B")) {
                funkBreakStartBar =
                    section.startBar;
                funkBreakBars = section.bars;
            }
        }
        const int funkCycleTicks =
            2 * funkAdvanced.recipe.beatsPerBar *
            12;
        const QSet<int> funkOpeningCell =
            melodyCellAt(
                funkAdvanced, 0, funkCycleTicks);
        const QSet<int> funkReturnCell =
            melodyCellAt(
                funkAdvanced,
                funkReturnTick,
                funkCycleTicks);
        QSet<int> funkRecalledCell =
            funkOpeningCell;
        funkRecalledCell.intersect(funkReturnCell);
        const bool funkCellValid =
            funkReturnTick >= 0 &&
            !funkOpeningCell.isEmpty() &&
            funkRecalledCell.size() >=
                qMax(1, funkOpeningCell.size() - 1);
        QSet<int> funkBassTicks;
        for (const auto& event :
             funkAdvanced.recipe.bassEvents) {
            funkBassTicks.insert(event.tick);
        }
        bool funkBreakSubtractsBass = false;
        for (int bar = funkBreakStartBar;
             bar < funkBreakStartBar +
                 funkBreakBars;
             ++bar) {
            const int tick =
                (bar - 1) *
                funkAdvanced.recipe
                    .beatsPerBar * 12;
            funkBreakSubtractsBass =
                funkBreakSubtractsBass ||
                !funkBassTicks.contains(tick);
        }
        int funkKickOnes = 0;
        for (int bar = 1;
             bar <= funkAdvanced.recipe.bars;
             ++bar) {
            const int tick =
                (bar - 1) *
                funkAdvanced.recipe
                    .beatsPerBar * 12;
            funkKickOnes += std::any_of(
                funkAdvanced.recipe
                    .drumEvents.cbegin(),
                funkAdvanced.recipe
                    .drumEvents.cend(),
                [tick](const auto& event) {
                    return event.tick == tick &&
                        event.laneId ==
                            QStringLiteral("kick");
                });
        }
        const bool funkDrumOneValid =
            funkKickOnes * 4 >=
                funkAdvanced.recipe.bars * 3;
        record(QStringLiteral(
            "practice.funk-complexity-preserves-vamp-pocket-and-develops-interlock"),
            funkIdentityStable &&
                funkComplexityValid &&
                funkCellValid &&
                funkBreakSubtractsBass &&
                funkDrumOneValid);

        jam2::practice::ChordIdeaRequest
            boomBapRequest;
        boomBapRequest.styleId =
            QStringLiteral("hiphop-trap");
        boomBapRequest.profileId =
            QStringLiteral("hiphop_boom_bap");
        boomBapRequest.formId =
            QStringLiteral("boombap-16");
        boomBapRequest.harmonicComplexity = 1;
        boomBapRequest.rhythmicComplexity = 1;
        const auto boomBapFoundation =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    boomBapRequest,
                    3985821267U);
        boomBapRequest.harmonicComplexity = 4;
        boomBapRequest.rhythmicComplexity = 4;
        const auto boomBapDeveloped =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    boomBapRequest,
                    3985821267U);
        boomBapRequest.harmonicComplexity = 8;
        boomBapRequest.rhythmicComplexity = 8;
        const auto boomBapAdvanced =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    boomBapRequest,
                    3985821267U);
        const bool boomBapIdentityStable =
            boomBapFoundation.recipe
                    .progressionId ==
                boomBapDeveloped.recipe
                    .progressionId &&
            boomBapDeveloped.recipe
                    .progressionId ==
                boomBapAdvanced.recipe
                    .progressionId &&
            boomBapFoundation.recipe.grooveId ==
                boomBapDeveloped.recipe.grooveId &&
            boomBapDeveloped.recipe.grooveId ==
                boomBapAdvanced.recipe.grooveId &&
            boomBapFoundation.recipe.motifCell ==
                boomBapDeveloped.recipe.motifCell &&
            boomBapDeveloped.recipe.motifCell ==
                boomBapAdvanced.recipe.motifCell;
        const bool boomBapComplexityValid =
            boomBapFoundation.recipe
                    .theoryDecisions.isEmpty() &&
            boomBapDeveloped.recipe
                    .theoryDecisions.isEmpty() &&
            boomBapAdvanced.recipe
                    .theoryDecisions.isEmpty() &&
            boomBapFoundation.recipe
                    .supportingEvents.isEmpty() &&
            supportRoles(boomBapDeveloped) ==
                QSet<QString>{
                    QStringLiteral("riff")} &&
            supportRoles(boomBapAdvanced)
                .contains(
                    QStringLiteral("riff")) &&
            supportRoles(boomBapAdvanced)
                .contains(
                    QStringLiteral(
                        "hook_double")) &&
            bassHasRelationship(
                boomBapDeveloped,
                QStringLiteral(
                    "chromatic bass pickup"));
        int boomBapReturnTick = -1;
        for (const auto& form :
             boomBapAdvanced.recipe
                 .formSections) {
            if (form.role.contains(
                    QStringLiteral("return"),
                    Qt::CaseInsensitive)) {
                boomBapReturnTick =
                    (form.startBar - 1) *
                    boomBapAdvanced.recipe
                        .beatsPerBar * 12;
            }
        }
        const int boomBapCycleTicks =
            4 * boomBapAdvanced.recipe
                    .beatsPerBar * 12;
        const QSet<int> boomBapOpeningCell =
            melodyCellAt(
                boomBapAdvanced,
                0,
                boomBapCycleTicks);
        const QSet<int> boomBapReturnCell =
            melodyCellAt(
                boomBapAdvanced,
                boomBapReturnTick,
                boomBapCycleTicks);
        QSet<int> boomBapRecalledCell =
            boomBapOpeningCell;
        boomBapRecalledCell.intersect(
            boomBapReturnCell);
        const bool boomBapCellValid =
            boomBapReturnTick >= 0 &&
            !boomBapOpeningCell.isEmpty() &&
            boomBapRecalledCell.size() >=
                qMax(
                    1,
                    boomBapOpeningCell.size() - 1);
        QSet<int> boomBapBackbeatBars;
        const int boomBapBarTicks =
            boomBapAdvanced.recipe
                .beatsPerBar * 12;
        for (const auto& event :
             boomBapAdvanced.recipe.drumEvents) {
            const int within =
                event.tick % boomBapBarTicks;
            if (event.laneId ==
                    QStringLiteral("snare") &&
                (within == 12 ||
                 within == 36)) {
                boomBapBackbeatBars.insert(
                    event.tick /
                    boomBapBarTicks);
            }
        }
        record(QStringLiteral(
            "practice.boombap-complexity-preserves-original-beat-phrase-and-rap-space"),
            boomBapIdentityStable &&
                boomBapComplexityValid &&
                boomBapCellValid &&
                boomBapBackbeatBars.size() * 4 >=
                    boomBapAdvanced.recipe.bars *
                        3);

        jam2::practice::ChordIdeaRequest
            trapRequest;
        trapRequest.styleId =
            QStringLiteral("hiphop-trap");
        trapRequest.profileId =
            QStringLiteral("hiphop_trap");
        trapRequest.formId =
            QStringLiteral("trap-12");
        trapRequest.harmonicComplexity = 1;
        trapRequest.rhythmicComplexity = 1;
        const auto trapFoundation =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    trapRequest,
                    1855857455U);
        trapRequest.harmonicComplexity = 4;
        trapRequest.rhythmicComplexity = 4;
        const auto trapDeveloped =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    trapRequest,
                    1855857455U);
        trapRequest.harmonicComplexity = 8;
        trapRequest.rhythmicComplexity = 8;
        const auto trapAdvanced =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    trapRequest,
                    1855857455U);
        const bool trapIdentityStable =
            trapFoundation.recipe.progressionId ==
                trapDeveloped.recipe.progressionId &&
            trapDeveloped.recipe.progressionId ==
                trapAdvanced.recipe.progressionId &&
            trapFoundation.recipe.grooveId ==
                trapDeveloped.recipe.grooveId &&
            trapDeveloped.recipe.grooveId ==
                trapAdvanced.recipe.grooveId &&
            trapFoundation.recipe.motifCell ==
                trapDeveloped.recipe.motifCell &&
            trapDeveloped.recipe.motifCell ==
                trapAdvanced.recipe.motifCell;
        const bool trapBassValid =
            bassHasRelationship(
                trapDeveloped,
                QStringLiteral(
                    "semitone 808 approach")) &&
            std::any_of(
                trapDeveloped.recipe
                    .bassEvents.cbegin(),
                trapDeveloped.recipe
                    .bassEvents.cend(),
                [](const auto& event) {
                    return event.articulation ==
                        QStringLiteral("808-slide");
                }) &&
            std::any_of(
                trapFoundation.recipe
                    .bassEvents.cbegin(),
                trapFoundation.recipe
                    .bassEvents.cend(),
                [](const auto& event) {
                    return event.durationTicks >=
                        18;
                }) &&
            std::all_of(
                trapAdvanced.recipe
                    .bassEvents.cbegin(),
                trapAdvanced.recipe
                    .bassEvents.cend(),
                [](const auto& event) {
                    return event.midi >= 28 &&
                        event.midi <= 47;
                });
        const bool trapComplexityValid =
            trapFoundation.recipe
                    .theoryDecisions.isEmpty() &&
            trapDeveloped.recipe
                    .theoryDecisions.isEmpty() &&
            trapAdvanced.recipe
                    .theoryDecisions.isEmpty() &&
            trapFoundation.recipe
                    .supportingEvents.isEmpty() &&
            supportRoles(trapDeveloped) ==
                QSet<QString>{
                    QStringLiteral("riff")} &&
            supportRoles(trapAdvanced)
                .contains(
                    QStringLiteral("riff")) &&
            supportRoles(trapAdvanced)
                .contains(
                    QStringLiteral(
                        "hook_double"));
        int trapRollBeat = -1;
        int trapNegativeStartTick = -1;
        for (const auto& form :
             trapAdvanced.recipe.formSections) {
            if (form.label.contains(
                    QStringLiteral(
                        "Roll / Slide"))) {
                trapRollBeat =
                    (form.startBar +
                     form.bars - 1) *
                        trapAdvanced.recipe
                            .beatsPerBar -
                    1;
            }
            if (form.role.contains(
                    QStringLiteral(
                        "subtract hats"),
                    Qt::CaseInsensitive)) {
                trapNegativeStartTick =
                    (form.startBar - 1) *
                    trapAdvanced.recipe
                        .beatsPerBar * 12;
            }
        }
        int trapRollHits = 0;
        int trapHalfTimeBars = 0;
        bool trapNegativeHats = false;
        bool trapShiftedKick = false;
        const int trapBarTicks =
            trapAdvanced.recipe.beatsPerBar *
            12;
        QSet<int> trapHalfTimeBarSet;
        for (const auto& event :
             trapAdvanced.recipe.drumEvents) {
            if (event.laneId ==
                    QStringLiteral("closed_hat") &&
                event.tick >= trapRollBeat * 12 &&
                event.tick <
                    trapRollBeat * 12 + 12) {
                ++trapRollHits;
            }
            if (event.laneId ==
                    QStringLiteral("snare") &&
                event.tick % trapBarTicks == 24) {
                trapHalfTimeBarSet.insert(
                    event.tick / trapBarTicks);
            }
            trapNegativeHats =
                trapNegativeHats ||
                (trapNegativeStartTick >= 0 &&
                 (event.laneId ==
                      QStringLiteral(
                          "closed_hat") ||
                  event.laneId ==
                      QStringLiteral(
                          "open_hat")) &&
                 event.tick >=
                     trapNegativeStartTick &&
                 event.tick <
                     trapNegativeStartTick + 24);
            trapShiftedKick =
                trapShiftedKick ||
                (trapNegativeStartTick >= 0 &&
                 event.laneId ==
                     QStringLiteral("kick") &&
                 event.tick >
                     trapNegativeStartTick &&
                 event.tick <
                     trapNegativeStartTick + 12);
        }
        trapHalfTimeBars =
            trapHalfTimeBarSet.size();
        record(QStringLiteral(
            "practice.trap-complexity-preserves-half-time-phrase-and-develops-808-form"),
            trapIdentityStable &&
                trapBassValid &&
                trapComplexityValid &&
                trapRollBeat >= 0 &&
                trapRollHits >= 4 &&
                trapNegativeStartTick >= 0 &&
                !trapNegativeHats &&
                trapShiftedKick &&
                trapHalfTimeBars * 10 >=
                    trapAdvanced.recipe.bars *
                        7);

        jam2::practice::ChordIdeaRequest
            houseFoundationRequest;
        houseFoundationRequest.styleId =
            QStringLiteral("electronic");
        houseFoundationRequest.profileId =
            QStringLiteral("electronic_house");
        houseFoundationRequest.formId =
            QStringLiteral("house-16");
        houseFoundationRequest.harmonicComplexity = 1;
        houseFoundationRequest.rhythmicComplexity = 1;
        const auto houseFoundation =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    houseFoundationRequest,
                    1098693470U);
        const bool houseBassValid =
            houseFoundation.recipe.bassEvents.size() ==
                houseFoundation.recipe.bars * 2 &&
            std::all_of(
                houseFoundation.recipe.bassEvents.cbegin(),
                houseFoundation.recipe.bassEvents.cend(),
                [](const auto& event) {
                    return event.tick % 12 != 0;
                });
        const bool houseGridValid =
            std::all_of(
                houseFoundation.chordSection
                    .musicalPatterns.cbegin(),
                houseFoundation.chordSection
                    .musicalPatterns.cend(),
                [](const auto& pattern) {
                    return pattern.division == 4;
                });
        const QSet<int> houseOpeningCell =
            melodyCellAt(houseFoundation, 0, 96);
        const bool houseCellValid =
            !houseOpeningCell.isEmpty() &&
            houseOpeningCell ==
                melodyCellAt(
                    houseFoundation, 96, 96);
        jam2::practice::ChordIdeaRequest
            houseHoldoutRequest;
        houseHoldoutRequest.styleId =
            QStringLiteral("electronic");
        houseHoldoutRequest.profileId =
            QStringLiteral("electronic_house");
        houseHoldoutRequest.formId =
            QStringLiteral("house-32");
        houseHoldoutRequest.harmonicComplexity = 1;
        houseHoldoutRequest.rhythmicComplexity = 1;
        const auto houseHoldout =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    houseHoldoutRequest,
                    3832002506U);
        QVector<int> houseCycleOnsets(
            (houseHoldout.recipe.bars + 1) / 2,
            0);
        for (const auto& event :
             houseHoldout.recipe.melodyEvents) {
            const int cycle =
                event.tick /
                qMax(
                    1,
                    2 *
                        houseHoldout.recipe
                            .beatsPerBar *
                        12);
            if (cycle >= 0 &&
                cycle < houseCycleOnsets.size()) {
                ++houseCycleOnsets[cycle];
            }
        }
        const bool houseDensityValid =
            !houseHoldout.recipe.melodyEvents
                 .isEmpty() &&
            std::all_of(
                houseCycleOnsets.cbegin(),
                houseCycleOnsets.cend(),
                [](int onsets) {
                    return onsets <= 7;
                });

        jam2::practice::ChordIdeaRequest
            technoProcessRequest;
        technoProcessRequest.styleId =
            QStringLiteral("electronic");
        technoProcessRequest.profileId =
            QStringLiteral("electronic_techno");
        technoProcessRequest.formId =
            QStringLiteral("techno-odd-15");
        technoProcessRequest.harmonicComplexity = 4;
        technoProcessRequest.rhythmicComplexity = 4;
        const auto technoProcess =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    technoProcessRequest,
                    1136867690U);
        QSet<int> technoPitchClasses;
        for (const auto& event :
             technoProcess.recipe.melodyEvents) {
            technoPitchClasses.insert(
                ((event.midi -
                      technoProcessRequest.key) %
                     12 +
                 12) %
                12);
        }
        const bool technoBassValid =
            technoProcess.recipe.bassEvents.size() ==
                technoProcess.recipe.bars *
                    technoProcess.recipe.beatsPerBar &&
            std::all_of(
                technoProcess.recipe.bassEvents.cbegin(),
                technoProcess.recipe.bassEvents.cend(),
                [](const auto& event) {
                    return event.tick % 12 == 0;
                });
        const bool technoCellValid =
            technoPitchClasses.size() == 2 &&
            technoProcess.recipe.theoryDecisions
                .isEmpty() &&
            melodyCellAt(technoProcess, 0, 36) ==
                melodyCellAt(
                    technoProcess, 36, 36);

        jam2::practice::ChordIdeaRequest
            breakbeatProcessRequest;
        breakbeatProcessRequest.styleId =
            QStringLiteral("electronic");
        breakbeatProcessRequest.profileId =
            QStringLiteral(
                "electronic_breakbeat");
        breakbeatProcessRequest.formId =
            QStringLiteral("breakbeat-24");
        breakbeatProcessRequest.harmonicComplexity = 8;
        breakbeatProcessRequest.rhythmicComplexity = 8;
        const auto breakbeatProcess =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    breakbeatProcessRequest,
                    2517899596U);
        const bool breakbeatBassValid =
            breakbeatProcess.recipe.bassEvents.size() ==
                breakbeatProcess.recipe.bars * 4 &&
            std::any_of(
                breakbeatProcess.recipe.bassEvents.cbegin(),
                breakbeatProcess.recipe.bassEvents.cend(),
                [](const auto& event) {
                    return event.tick % 12 != 0;
                });
        bool breakbeatToolsValid = false;
        bool claimedPlaning = false;
        for (const auto& tool :
             breakbeatProcess.recipe.complexityTools) {
            if (tool.toolId ==
                    QStringLiteral("riff-mutation") &&
                tool.selected) {
                breakbeatToolsValid = true;
            }
            if (tool.toolId ==
                    QStringLiteral("planing") &&
                tool.selected) {
                claimedPlaning = true;
            }
        }
        record(QStringLiteral(
            "practice.electronic-profiles-preserve-distinct-machine-cells-bass-and-complexity"),
            houseFoundation.recipe.theoryDecisions
                    .isEmpty() &&
                houseBassValid &&
                houseGridValid &&
                houseCellValid &&
                houseDensityValid &&
                technoBassValid &&
                technoCellValid &&
                breakbeatBassValid &&
                breakbeatToolsValid &&
                !claimedPlaning);

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
            phrygianHarmony.recipe.supportingEvents.isEmpty();
        record(QStringLiteral(
            "practice.modal-atmospheric-retains-bass-pedal-without-static-drone"),
            phrygianPedalValid);

        jam2::practice::ChordIdeaRequest
            modalCharacteristicRequest;
        modalCharacteristicRequest.key = 2;
        modalCharacteristicRequest.styleId =
            QStringLiteral("modal-jam");
        modalCharacteristicRequest.profileId =
            QStringLiteral("modal_atmospheric");
        modalCharacteristicRequest.formId =
            QStringLiteral("modal-atmospheric-16");
        modalCharacteristicRequest.modeId =
            QStringLiteral("dorian");
        modalCharacteristicRequest.harmonicComplexity = 4;
        modalCharacteristicRequest.rhythmicComplexity = 4;
        const auto modalCharacteristic =
            jam2::practice::
                generateCoupledPracticeIdeaForTest(
                    modalCharacteristicRequest,
                    1850916699U);
        record(QStringLiteral(
            "practice.modal-atmospheric-melody-states-characteristic-degree"),
            std::any_of(
                modalCharacteristic.recipe
                    .melodyEvents.cbegin(),
                modalCharacteristic.recipe
                    .melodyEvents.cend(),
                [](const auto& event) {
                    // D Dorian's defining natural sixth is B.
                    return event.midi % 12 == 11;
                }));

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

        QJsonObject excessiveSections = BeatGridModel{}.toJson();
        QJsonArray fiveSections = excessiveSections.value(QStringLiteral("sections")).toArray();
        fiveSections.append(fiveSections.first());
        excessiveSections[QStringLiteral("sections")] = fiveSections;
        BeatGridModel rejectedFiveSections;
        record(QStringLiteral("practice.fixed-banks-reject-more-than-four-sections"),
            !rejectedFiveSections.loadJson(excessiveSections));

        LooperProject arrangementProject;
        const bool acceptedArrangement = arrangementProject.setArrangement(
            ArrangementDefinition{{ArrangementStep{0, 2}, ArrangementStep{1, 3}}, false});
        LooperProject arrangementRoundTrip;
        record(QStringLiteral("looper.arrangement-roundtrip-is-bounded"),
            acceptedArrangement && arrangementRoundTrip.loadJson(arrangementProject.toJson()) &&
            arrangementRoundTrip.arrangement().steps.size() == 2 &&
            arrangementRoundTrip.arrangement().steps.at(1).bankIndex == 1 &&
            arrangementRoundTrip.arrangement().steps.at(1).repeats == 3 &&
            !arrangementRoundTrip.arrangement().loop);

        LooperProject timingProject;
        const LooperBankTiming newProjectA = timingProject.resolvedTiming(0);
        const LooperBankTiming newProjectB = timingProject.resolvedTiming(1);
        LooperBankTiming bankA;
        bankA.bpm = 90;
        bankA.beatsPerBar = 12;
        bankA.beatUnit = 8;
        bankA.tempoPulseUnits = 3;
        bankA.division = 2;
        const bool bankASet = timingProject.setTiming(0, bankA);
        const LooperBankTiming inheritedB = timingProject.resolvedTiming(1);
        LooperBankTiming bankB = inheritedB;
        bankB.bpm = 124;
        bankB.beatsPerBar = 2;
        bankB.beatUnit = 4;
        bankB.tempoPulseUnits = 1;
        bankB.inheritsBankA = false;
        const bool bankBSet = timingProject.setTiming(1, bankB);
        bankA.bpm = 96;
        const bool bankAChanged = timingProject.setTiming(0, bankA);
        LooperProject timingRoundTrip;
        const bool timingLoaded = timingRoundTrip.loadJson(timingProject.toJson());
        record(QStringLiteral("looper.bank-timing-inherits-a-until-overridden"),
            newProjectA.beatsPerBar == 4 && newProjectA.beatUnit == 4 &&
            newProjectB.inheritsBankA && newProjectB.beatsPerBar == 4 &&
            bankASet && inheritedB.inheritsBankA && inheritedB.bpm == 90 &&
            inheritedB.beatsPerBar == 12 && inheritedB.tempoPulseUnits == 3 &&
            bankBSet && bankAChanged && timingProject.resolvedTiming(1).bpm == 124 &&
            timingLoaded && timingRoundTrip.resolvedTiming(0).bpm == 96 &&
            timingRoundTrip.resolvedTiming(1).bpm == 124 &&
            !timingRoundTrip.resolvedTiming(1).inheritsBankA &&
            jam2::gui::trackTimelineBarNumber(180, 12) == 16);

        QJsonObject invalidBeatLaneSchema = BeatGridModel{}.toJson();
        invalidBeatLaneSchema[QStringLiteral("beat_lane_schema")] = 4;
        BeatGridModel rejectedBeatLaneSchema;
        record(
            QStringLiteral(
                "practice.beat-lane-schema-rejects-non-current-version"),
            !rejectedBeatLaneSchema.loadJson(invalidBeatLaneSchema));

        QJsonObject missingBeatLaneSchema = BeatGridModel{}.toJson();
        missingBeatLaneSchema.remove(QStringLiteral("beat_lane_schema"));
        BeatGridModel rejectedMissingBeatLaneSchema;
        record(
            QStringLiteral("practice.beat-lane-schema-is-required"),
            !rejectedMissingBeatLaneSchema.loadJson(
                missingBeatLaneSchema));

        record(QStringLiteral("practice.beat-view-uses-rendered-ride-lane"),
            BeatGridModel::beatLaneSchemaVersion() == 3 &&
            BeatGridModel{}.toJson()
                .value(QStringLiteral("beat_lane_schema")).toInt() == 3 &&
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
            } &&
            BeatGridModel::beatVisualLaneNames() == QStringList{
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
        const unsigned reportedThreads = std::thread::hardware_concurrency();
        const int expectedRenderWorkers = static_cast<int>(
            std::min<unsigned>(5, reportedThreads > 0 ? reportedThreads : 1));
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
            renderedAgain.error.isEmpty() &&
            rendered.chords.sha256 == renderedAgain.chords.sha256 &&
            rendered.drums.sha256 == renderedAgain.drums.sha256 &&
            rendered.melody.sha256 == renderedAgain.melody.sha256 &&
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
            rendered.diagnostics.contains(
                QStringLiteral("render_workers=%1").arg(expectedRenderWorkers)) &&
            rendered.diagnostics.contains(
                QStringLiteral("reported_threads=%1").arg(reportedThreads)) &&
            rendered.diagnostics.contains(QStringLiteral("chord_ms=")) &&
            rendered.diagnostics.contains(QStringLiteral("drum_ms=")) &&
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
        settings.drumKit =
            jam2::practice::ReferenceDrumKit::Electronic;
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
            rendered.diagnostics.contains(
                QStringLiteral("drum_patch=base:electronic")) &&
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
        const bool neutralDefaults =
            model.section(0).name == QStringLiteral("Section A") &&
            model.section(1).name == QStringLiteral("Section B");
        model.renameSection(0, QStringLiteral("A"), QStringLiteral("Custom Verse"));
        model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
        const bool cleared = model.clearSection(0);
        record(QStringLiteral("practice.empty-and-cleared-banks-use-neutral-section-names"),
            neutralDefaults && cleared &&
            model.section(0).name == QStringLiteral("Section A") &&
            model.section(1).name == QStringLiteral("Section B"));
    }
    {
        BeatGridModel song;
        LooperProject project;
        const LooperBankTiming timing = project.resolvedTiming(0);
        const LooperBankTiming inherited = project.resolvedTiming(1);
        const SharedTrackModel track;
        record(QStringLiteral(
            "practice.new-project-is-blank-at-80-bpm-4-4-with-quarter-note-clicks"),
            song.hasOnlyPristineSection() &&
            project.banks().at(0).lanes.isEmpty() &&
            project.banks().at(1).lanes.isEmpty() &&
            track.acceptedBpm == 80.0 &&
            timing.bpm == 80 && timing.beatsPerBar == 4 &&
            timing.beatUnit == 4 && timing.tempoPulseUnits == 1 &&
            timing.division == 1 && timing.playMaskLow == 0x0fULL &&
            timing.playMaskHigh == 0 && timing.accentMaskLow == 0x01ULL &&
            timing.accentMaskHigh == 0 &&
            inherited.inheritsBankA && inherited.bpm == 80 &&
            inherited.beatsPerBar == 4 && inherited.beatUnit == 4 &&
            inherited.tempoPulseUnits == 1 && inherited.division == 1);
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
        const QDir tracks(appReleaseFolderPath(QStringLiteral("tracks")));
        const QDir songs(appReleaseFolderPath(QStringLiteral("songs")));
        const QStringList tracksBefore = tracks.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        const QStringList songsBefore = songs.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        const QString slug = JamStorage::portableSlug(
            QStringLiteral("  Purple   Orbit: Live  "));
        const QString randomName = JamStorage::randomDisplayName();
        record(QStringLiteral("jam-storage.portable-two-word-names-and-slugs"),
            slug == QStringLiteral("Purple_Orbit_Live") &&
            !slug.contains(QLatin1Char(' ')) &&
            randomName.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() == 2 &&
            !JamStorage::portableSlug(randomName).contains(QLatin1Char(' ')) &&
            tracks.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name) ==
                tracksBefore &&
            songs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name) ==
                songsBefore);
    }
    {
        LooperProject project;
        const bool emptyBankRejected =
            !PreparedMixRenderer::hasRenderableSources(project);
        LooperLane emptyLane;
        project.appendLane(0, emptyLane);
        const bool emptyLaneRejected =
            !PreparedMixRenderer::hasRenderableSources(project);
        project.banks()[0].lanes[0].assetPath = QStringLiteral("recorded/take.wav");
        const bool wavLaneAccepted =
            PreparedMixRenderer::hasRenderableSources(project);
        project.banks()[0].lanes[0].muted = true;
        const bool mutedLaneRejected =
            !PreparedMixRenderer::hasRenderableSources(project);
        record(QStringLiteral("prepared-mix.empty-bank-does-not-render"),
            emptyBankRejected && emptyLaneRejected && wavLaneAccepted &&
            mutedLaneRejected);
    }
    {
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/prepared-mix-short-bank-test-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString sourcePath = QDir(workspace).absoluteFilePath(
            QStringLiteral("long-lane.wav"));
        const QString outputPath = QDir(workspace).absoluteFilePath(
            QStringLiteral("cropped-mix.wav"));
        const bool folderReady = QDir().mkpath(workspace);
        QFile sourceFile(sourcePath);
        const QByteArray sourceWav = pcm16Wav(32);
        const bool sourceReady = folderReady &&
            sourceFile.open(QIODevice::WriteOnly) &&
            sourceFile.write(sourceWav) == sourceWav.size();
        sourceFile.close();

        LooperProject project;
        LooperLane lane;
        lane.name = QStringLiteral("Retained long lane");
        lane.assetPath = sourcePath;
        lane.stopFrame = 32;
        lane.sampleRateCompatible = true;
        const bool laneReady = project.appendLane(0, lane);
        const PreparedMixResult rendered = PreparedMixRenderer::render(
            project,
            workspace,
            48000,
            outputPath,
            SharedTrackModel{},
            0,
            8);
        const jam2::wav::InspectResult inspected =
            jam2::wav::inspect_pcm16_file(nativeFilePath(outputPath));
        record(QStringLiteral("prepared-mix.long-retained-lane-is-cropped-to-short-bank"),
            sourceReady && laneReady && rendered.error.isEmpty() &&
            rendered.frames == 8 && inspected && inspected.info.frames == 8);
        (void)QDir(workspace).removeRecursively();
    }
    {
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/prepared-mix-sequence-test-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString sourcePath = QDir(workspace).absoluteFilePath(
            QStringLiteral("bank-a.wav"));
        const QString outputPath = QDir(workspace).absoluteFilePath(
            QStringLiteral("arrangement.wav"));
        const bool folderReady = QDir().mkpath(workspace);
        QFile sourceFile(sourcePath);
        const QByteArray sourceWav = pcm16Wav(16);
        const bool sourceReady = folderReady &&
            sourceFile.open(QIODevice::WriteOnly) &&
            sourceFile.write(sourceWav) == sourceWav.size();
        sourceFile.close();

        LooperProject project;
        LooperLane lane;
        lane.name = QStringLiteral("Bank A audio");
        lane.assetPath = sourcePath;
        lane.stopFrame = 16;
        lane.sampleRateCompatible = true;
        const bool laneReady = project.appendLane(0, lane);
        const PreparedMixResult rendered = PreparedMixRenderer::renderSequence(
            project,
            workspace,
            48000,
            outputPath,
            SharedTrackModel{},
            {
                {0, 2, 4},
                {1, 1, 6},
            });
        const jam2::wav::InspectResult inspected =
            jam2::wav::inspect_pcm16_file(nativeFilePath(outputPath));
        QFile outputFile(outputPath);
        const bool outputReady = outputFile.open(QIODevice::ReadOnly);
        const QByteArray outputBytes = outputReady ? outputFile.readAll() : QByteArray{};
        const QByteArray bankAudio = outputBytes.mid(44, 16);
        const QByteArray emptyBankAudio = outputBytes.mid(60, 12);
        const bool bankHasAudio = std::any_of(
            bankAudio.cbegin(), bankAudio.cend(), [](char value) {
                return value != 0;
            });
        const bool emptyBankSilent = emptyBankAudio == QByteArray(12, '\0');
        record(QStringLiteral(
            "prepared-mix.sequence-repeats-banks-and-preserves-empty-section-duration"),
            sourceReady && laneReady && rendered.error.isEmpty() &&
            rendered.frames == 14 && inspected && inspected.info.frames == 14 &&
            bankAudio.size() == 16 && bankHasAudio && emptyBankSilent,
            QStringLiteral(
                "source=%1 lane=%2 error=%3 frames=%4 inspect=%5 inspect_frames=%6 bytes=%7 bank_bytes=%8 bank_audio=%9 empty_bytes=%10 empty_silent=%11")
                .arg(sourceReady).arg(laneReady).arg(rendered.error)
                .arg(rendered.frames).arg(static_cast<bool>(inspected))
                .arg(inspected ? inspected.info.frames : 0)
                .arg(outputBytes.size()).arg(bankAudio.size()).arg(bankHasAudio)
                .arg(emptyBankAudio.size()).arg(emptyBankSilent));
        outputFile.close();
        (void)QDir(workspace).removeRecursively();
    }
    {
        const QString workspace = QDir::current().absoluteFilePath(
            QStringLiteral("build/empty-transient-workspace-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString transientPath =
            QDir(workspace).absoluteFilePath(QStringLiteral("generated/discard.wav"));
        const bool folderReady = QDir().mkpath(QFileInfo(transientPath).absolutePath());
        QFile transientFile(transientPath);
        const bool fileReady = folderReady && transientFile.open(QIODevice::WriteOnly) &&
            transientFile.write("fixture", 7) == 7;
        transientFile.close();
        ProjectPersistenceCoordinator persistence;
        persistence.initializeWorkspace(workspace);
        persistence.registerTransientWav(transientPath);
        const bool discarded = persistence.discardTransientWav(transientPath);
        record(QStringLiteral("project.discard-prunes-empty-jam-workspace"),
            fileReady && discarded && !QFileInfo::exists(transientPath) &&
            !QFileInfo::exists(workspace));
        if (QFileInfo::exists(workspace)) {
            (void)QDir(workspace).removeRecursively();
        }
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
            QDir(root).absoluteFilePath(QStringLiteral("project/song.jamjar")),
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
        const int preserved = mergeSynchronizedLooperLanes(incoming, project);
        LooperProject merged;
        const bool mergedLoaded =
            merged.loadJson(incoming.value(QStringLiteral("looper")).toObject());
        const QJsonArray savedLanes = saved.value(QStringLiteral("banks")).toArray()
            .at(0).toObject().value(QStringLiteral("lanes")).toArray();
        const QJsonArray syncedLanes = synced.value(QStringLiteral("banks")).toArray()
            .at(0).toObject().value(QStringLiteral("lanes")).toArray();
        const QJsonObject syncedLane = syncedLanes.isEmpty()
            ? QJsonObject{} : syncedLanes.at(0).toObject();
        record(QStringLiteral("practice-reference.metadata-syncs-without-local-mix-state"),
            appended && roundTripped && savedLanes.size() == 1 &&
            savedLanes.at(0).toObject().value(QStringLiteral("local_only")).toBool() &&
            loaded.banks().at(0).lanes.size() == 1 &&
            loaded.banks().at(0).lanes.at(0).localOnly &&
            syncedLanes.size() == 1 && !syncedLane.contains(QStringLiteral("local_only")) &&
            !syncedLane.contains(QStringLiteral("gain_db")) &&
            !syncedLane.contains(QStringLiteral("muted")) &&
            !syncedLane.contains(QStringLiteral("solo")) &&
            preserved == 1 && mergedLoaded &&
            merged.banks().at(0).lanes.size() == 1);
    }
    {
        LooperProject local;
        LooperLane localLane;
        localLane.id = QStringLiteral("shared-lane-id");
        localLane.assetPath = QStringLiteral("C:/fixtures/local.wav");
        localLane.assetHash = QString(64, QLatin1Char('a'));
        localLane.name = QStringLiteral("Local work");
        localLane.sampleRate = 48000;
        localLane.gainDb = -12.0;
        localLane.muted = true;
        const bool localReady = local.appendLane(0, localLane);

        LooperProject remote;
        LooperLane remoteLane;
        remoteLane.id = localLane.id;
        remoteLane.assetPath = QStringLiteral("C:/fixtures/remote.wav");
        remoteLane.assetHash = QString(64, QLatin1Char('b'));
        remoteLane.name = QStringLiteral("Remote work");
        remoteLane.sampleRate = 48000;
        const bool remoteReady = remote.appendLane(0, remoteLane);
        QJsonObject incoming = BeatGridModel{}.toJson();
        incoming.insert(QStringLiteral("looper"), remote.toJson(true));
        const int preserved = mergeSynchronizedLooperLanes(incoming, local);
        LooperProject merged;
        const bool loaded = merged.loadJson(
            incoming.value(QStringLiteral("looper")).toObject());
        bool keptLocal = false;
        bool addedRemote = false;
        QString localId;
        QString remoteId;
        if (loaded) {
            for (const LooperLane& lane : merged.banks().at(0).lanes) {
                if (lane.assetHash == localLane.assetHash) {
                    keptLocal = lane.muted && std::abs(lane.gainDb + 12.0) < 0.001;
                    localId = lane.id;
                } else if (lane.assetHash == remoteLane.assetHash) {
                    addedRemote = true;
                    remoteId = lane.id;
                }
            }
        }
        record(QStringLiteral("track-sync.same-id-different-assets-merge-non-destructively"),
            localReady && remoteReady && preserved == 1 && loaded &&
            merged.banks().at(0).lanes.size() == 2 && keptLocal && addedRemote &&
            !localId.isEmpty() && !remoteId.isEmpty() && localId != remoteId);
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
    const QJsonObject validReferenceRender{
        {QStringLiteral("type"), QStringLiteral("practice.references.render")},
        {QStringLiteral("request_id"), QStringLiteral("abcdef01-1234-1234-1234-123456789abc")},
        {QStringLiteral("render_signature"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("render_chords"), true},
        {QStringLiteral("render_drums"), false},
        {QStringLiteral("render_melody"), false},
        {QStringLiteral("render_bass"), false},
        {QStringLiteral("render_support"), false},
        {QStringLiteral("chord_voicing"), 0},
        {QStringLiteral("drum_kit"), 0},
    };
    modelError.clear();
    record(QStringLiteral("reference-render.accept-complete-request"),
        jam2::application::validateControlMessage(validReferenceRender, modelError), modelError);
    QJsonObject invalidReferenceRender = validReferenceRender;
    invalidReferenceRender[QStringLiteral("render_chords")] = QStringLiteral("yes");
    modelError.clear();
    record(QStringLiteral("reference-render.reject-non-boolean-part"),
        !jam2::application::validateControlMessage(invalidReferenceRender, modelError), modelError);
    record(QStringLiteral("track-sync.classifies-all-control-payloads"),
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("song.set")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("practice.references.render")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.track.share.request")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.track.batch.offer")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.track.batch.complete")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.asset.request")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.asset.start")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("looper.asset.done")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("bank.request")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("bank.prepare")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("bank.ready")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("bank.cancel")) &&
        jam2::application::isTrackSyncControlMessageType(QStringLiteral("bank.switch")) &&
        !jam2::application::isTrackSyncControlMessageType(QStringLiteral("metronome.settings")));
    record(QStringLiteral("track-sync.manual-share-bypasses-automatic-sync-gate"),
        jam2::application::isManualTrackShareControlMessageType(
            QStringLiteral("looper.track.batch.offer")) &&
        jam2::application::isManualTrackShareControlMessageType(
            QStringLiteral("looper.track.batch.complete")) &&
        jam2::application::isManualTrackShareControlMessageType(
            QStringLiteral("looper.asset.request")) &&
        !jam2::application::isManualTrackShareControlMessageType(
            QStringLiteral("song.set")));
    {
        const QString switchId = QStringLiteral("abcdef01-1234-1234-1234-123456789abc");
        const QJsonObject prepare{
            {QStringLiteral("type"), QStringLiteral("bank.prepare")},
            {QStringLiteral("switch_id"), switchId},
            {QStringLiteral("bank"), 2},
        };
        const QJsonObject ready{
            {QStringLiteral("type"), QStringLiteral("bank.ready")},
            {QStringLiteral("switch_id"), switchId},
            {QStringLiteral("bank"), 2},
        };
        const QJsonObject launch{
            {QStringLiteral("type"), QStringLiteral("bank.switch")},
            {QStringLiteral("switch_id"), switchId},
            {QStringLiteral("bank"), 2},
            {QStringLiteral("target_abs_beat"), QStringLiteral("128")},
        };
        QJsonObject invalidLaunch = launch;
        invalidLaunch.remove(QStringLiteral("target_abs_beat"));
        modelError.clear();
        record(QStringLiteral("bank-switch.prepare-ready-and-exact-target-validate"),
            jam2::application::validateControlMessage(prepare, modelError) &&
            jam2::application::validateControlMessage(ready, modelError) &&
            jam2::application::validateControlMessage(launch, modelError) &&
            !jam2::application::validateControlMessage(invalidLaunch, modelError));
        const auto prepareFromCoordinator = jam2::application::evaluateControlMessage(
            prepare, jam2::application::ControlMessageSource::Coordinator);
        const auto prepareFromPeer = jam2::application::evaluateControlMessage(
            prepare, jam2::application::ControlMessageSource::AuthenticatedPeer);
        const auto readyFromPeer = jam2::application::evaluateControlMessage(
            ready, jam2::application::ControlMessageSource::AuthenticatedPeer);
        const auto readyFromCoordinator = jam2::application::evaluateControlMessage(
            ready, jam2::application::ControlMessageSource::Coordinator);
        record(QStringLiteral("bank-switch.authority-separates-prepare-and-ready"),
            prepareFromCoordinator.accepted && !prepareFromPeer.accepted &&
            readyFromPeer.accepted && !readyFromCoordinator.accepted);
    }
    const QJsonObject validTrack{
        {QStringLiteral("recording_id"), QStringLiteral("12345678-1234-1234-1234-123456789abc")},
        {QStringLiteral("bank"), 0},
        {QStringLiteral("target_lane_id"), QStringLiteral("peer-lane")},
        {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("name"), QStringLiteral("Peer WAV")},
        {QStringLiteral("sample_rate"), 48000},
    };
    const QJsonObject validTrackOffer{
        {QStringLiteral("type"), QStringLiteral("looper.track.batch.offer")},
        {QStringLiteral("batch_id"), QStringLiteral("abcdef01-1234-1234-1234-123456789abc")},
        {QStringLiteral("tracks"), QJsonArray{validTrack}},
    };
    modelError.clear();
    record(QStringLiteral("track-share.accepts-bounded-additive-offer"),
        jam2::application::validateControlMessage(validTrackOffer, modelError),
        modelError);
    record(QStringLiteral("track-share.offer-authorized-in-both-directions"),
        jam2::application::evaluateControlMessage(
            validTrackOffer,
            jam2::application::ControlMessageSource::LocalCreator).accepted &&
        jam2::application::evaluateControlMessage(
            validTrackOffer,
            jam2::application::ControlMessageSource::AuthenticatedPeer).accepted);
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
        grid.updateEngine(400, 400, 100, 0, 100, true);
        const PlaybackGrid::Position advanced = grid.position();
        record(QStringLiteral("transport-actions-do-not-replace-continuous-grid-epoch"),
            advanced.running && advanced.epochFrame == 100 &&
            advanced.absoluteBeat >= 3);
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
        record(QStringLiteral("record-start.is-track-sync-without-changing-grid-epoch"),
            jam2::is_track_sync_transport_action(
                jam2::EngineTransportAction::RecordStart) &&
            jam2::is_track_sync_transport_action(
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
    record(QStringLiteral("record-count-in.starts-at-safe-next-beat"),
        jam2::gui::recording_count_in_start_beat(3, 10000, 10100, 48000) == 5 &&
        jam2::gui::recording_count_in_start_beat(3, 10000, 20000, 48000) == 4);
    record(QStringLiteral("record-count-in-depends-on-grid-not-local-click"),
        jam2::gui::recording_grid_ready_for_count_in(true) &&
        !jam2::gui::recording_grid_ready_for_count_in(false));
    record(QStringLiteral("global-play.gui-transport-runs-without-wav"),
        jam2::gui::global_transport_elapsed_frames(
            true, true, 72000, 48000) == 24000 &&
        jam2::gui::global_transport_elapsed_frames(
            false, true, 72000, 48000) == 0 &&
        jam2::gui::global_transport_elapsed_frames(
            true, false, 72000, 48000) == 0 &&
        jam2::gui::global_transport_elapsed_frames(
            true, true, 47000, 48000) == 0);
    record(QStringLiteral("global-play.next-beat-keeps-session-epoch"),
        [] {
            PlaybackGrid::Position position;
            position.running = true;
            position.engineAnchored = true;
            position.rawCurrentFrame = 60000;
            position.epochFrame = 0;
            position.absoluteBeat = 2;
            position.secondsPerBeat = 0.5;
            position.sampleRate = 48000;
            return jam2::gui::next_safe_grid_beat_raw_frame(position) == 72000;
        }());
    record(QStringLiteral("prepared-track.attach-keeps-gui-on-song-clock-until-applied"),
        !jam2::gui::prepared_attach_has_applied(72000, 71999, 48000) &&
        !jam2::gui::prepared_attach_has_applied(72000, 72000, 48000) &&
        jam2::gui::prepared_attach_has_applied(72000, 72000, 72000));
    {
        jam2::audio::StreamControl control;
        control.metronome_pattern_scheduled_origin_raw_frame.store(48000);
        std::uint64_t position = 0;
        const bool beforeStart = jam2::audio::metronome_pattern_position(
            control, 47999, 47999, true, 0, position) && position == 47999;
        const bool atStart = jam2::audio::metronome_pattern_position(
            control, 48000, 48000, true, 0, position) && position == 0;
        const bool afterStart = jam2::audio::metronome_pattern_position(
            control, 72000, 72000, true, 0, position) && position == 24000;
        control.recording_count_in_start_frame.store(96000);
        control.recording_count_in_target_frame.store(192000);
        control.recording_count_in_active.store(true);
        const bool countInStartsAccented = jam2::audio::metronome_pattern_position(
            control, 96000, 96000, true, 0, position) && position == 0;
        record(QStringLiteral("global-play.metronome-accent-resets-without-epoch-change"),
            beforeStart && atStart && afterStart && countInStartsAccented &&
            control.metronome_pattern_origin_valid.load() &&
            control.metronome_pattern_origin_frame.load() == 48000 &&
            control.metronome_pattern_scheduled_origin_raw_frame.load() == 0);
    }
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
        workspace.publishStoppedTrackStateWhenApplied = true;
        workspace.pendingSongTrackRestart = true;
        workspace.trackController.requestPlayback(true, 12);

        workspace.cancelPendingTrackPlayback();

        record(QStringLiteral("practice.generate-cancels-track-playback-and-restarts"),
            !workspace.playPreparedMixWhenReady &&
            !workspace.publishStoppedTrackStateWhenApplied &&
            !workspace.pendingSongTrackRestart &&
            !workspace.trackController.playback().requestedPlaying);
    }
    {
        ApplicationRuntime runtime;
        TrackRecordingWorkflow workflow(runtime);
        PlaybackGrid::Position position;
        position.running = true;
        position.engineAnchored = true;
        position.rawCurrentFrame = 60000;
        position.epochFrame = 0;
        position.absoluteBeat = 2;
        position.secondsPerBeat = 0.5;
        position.sampleRate = 48000;
        // No running engine is required to exercise committed transport
        // snapshots, so use a remote-style commit below rather than submitting.
        const bool waitingBeforeCommit =
            !workflow.globalTransportRequestedPlaying() &&
            !workflow.globalTransportPlaying();
        jam2::EngineSnapshot snapshot;
        snapshot.transport_revision = 1;
        snapshot.transport_pending = true;
        snapshot.transport_action = jam2::EngineTransportAction::TrackRestart;
        snapshot.transport_target_frame = 72000;
        snapshot.engine_frame = 71000;
        snapshot.sample_rate = 48000.0;
        workflow.consumeSnapshot(
            snapshot, MetronomeTransportController::SnapshotUpdate{});
        const bool remoteStartArmedBeforeCommit =
            workflow.globalTransportRequestedPlaying() &&
            !workflow.globalTransportPlaying();
        snapshot.engine_frame = 72000;
        snapshot.transport_pending = false;
        snapshot.transport_commit_count = 1;
        workflow.consumeSnapshot(
            snapshot, MetronomeTransportController::SnapshotUpdate{});
        PlaybackGrid::Position advanced = position;
        advanced.rawCurrentFrame = 96000;
        const bool runningWithoutWav = workflow.globalTransportPlaying() &&
            workflow.globalTransportStartFrame() == 72000 &&
            workflow.currentTransportPositionMs(advanced, 0) == 500;
        snapshot.transport_revision = 2;
        snapshot.transport_pending = true;
        snapshot.transport_action = jam2::EngineTransportAction::TrackStop;
        snapshot.transport_target_frame = 120000;
        snapshot.engine_frame = 119000;
        workflow.consumeSnapshot(
            snapshot, MetronomeTransportController::SnapshotUpdate{});
        const bool remoteStopArmedBeforeCommit =
            !workflow.globalTransportRequestedPlaying() &&
            workflow.globalTransportPlaying();
        snapshot.engine_frame = 120000;
        snapshot.transport_pending = false;
        snapshot.transport_commit_count = 2;
        workflow.consumeSnapshot(
            snapshot, MetronomeTransportController::SnapshotUpdate{});
        record(QStringLiteral("global-play.transport-state-is-independent-of-wav"),
            waitingBeforeCommit && remoteStartArmedBeforeCommit &&
            runningWithoutWav && remoteStopArmedBeforeCommit &&
            !workflow.globalTransportRequestedPlaying() &&
            !workflow.globalTransportPlaying());
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
        std::array<std::int32_t, 4> stem{};
        const int peakPpm = queued
            ? source.mix(output.data(), output.size(), 0, stem)
            : 0;
        record(QStringLiteral("prepared-track.mix-reports-contribution-peak"),
            queued && peakPpm >= 999900 && peakPpm <= 1000000 &&
            output[0] > 0 && output[1] < 0 && output == stem &&
            static_cast<int>(jam2::audio::TrackTakeSource::CurrentJam) == 1 &&
            (jam2::audio::kTrackTakeIncludePrepared &
                jam2::audio::kTrackTakeIncludeMetronome) == 0);
    }
    {
        jam2::audio::PreparedTrackSource source(4);
        const int slot = source.claimLoadingSlot();
        std::int16_t* samples = source.loadingData(slot);
        if (samples != nullptr) {
            std::fill(samples, samples + 4, static_cast<std::int16_t>(1200));
        }
        const bool ready = samples != nullptr && source.publishReady(slot, 4, 48000);
        const std::array<jam2::audio::PreparedTrackSource::Command, 2> start{{
            {jam2::audio::PreparedTrackSource::CommandType::Swap,
             static_cast<std::uint32_t>(qMax(0, slot)), 100, 0, 0, 1000000},
            {jam2::audio::PreparedTrackSource::CommandType::Play,
             0, 100, 0, 0, 1000000},
        }};
        std::array<std::int32_t, 2> output{};
        const bool started = ready && source.enqueueBatch(start);
        if (started) {
            (void)source.mix(output.data(), output.size(), 100);
        }
        const bool cleared = source.enqueue({
            jam2::audio::PreparedTrackSource::CommandType::Clear,
            0, 102, 0, 0, 1000000});
        output.fill(0);
        if (cleared) {
            (void)source.mix(output.data(), output.size(), 102);
        }
        record(QStringLiteral("prepared-track.clear-unloads-stale-audio"),
            started && cleared && !source.playing() && source.sourceFrame() == 0 &&
            source.scheduledStartFrame() == 0 && source.actualStartFrame() == 0 &&
            output[0] == 0 && output[1] == 0);
    }
    {
        jam2::audio::PreparedTrackSource source(4);
        const int firstSlot = source.claimLoadingSlot();
        std::int16_t* first = source.loadingData(firstSlot);
        if (first != nullptr) {
            first[0] = 100;
            first[1] = 200;
            first[2] = 300;
            first[3] = 400;
        }
        const bool firstReady = first != nullptr &&
            source.publishReady(firstSlot, 4, 48000);
        const std::array<jam2::audio::PreparedTrackSource::Command, 2> start{{
            {jam2::audio::PreparedTrackSource::CommandType::Swap,
             static_cast<std::uint32_t>(qMax(0, firstSlot)), 100, 0, 0, 1000000},
            {jam2::audio::PreparedTrackSource::CommandType::Play,
             0, 100, 0, 0, 1000000},
        }};
        std::array<std::int32_t, 1> output{};
        const bool started = firstReady && source.enqueueBatch(start);
        if (started) {
            (void)source.mix(output.data(), output.size(), 100);
        }
        const int replacementSlot = source.claimLoadingSlot();
        std::int16_t* replacement = source.loadingData(replacementSlot);
        if (replacement != nullptr) {
            replacement[0] = 1000;
            replacement[1] = 2000;
            replacement[2] = 3000;
            replacement[3] = 4000;
        }
        const bool replacementReady = replacement != nullptr &&
            source.publishReady(replacementSlot, 4, 48000);
        const jam2::audio::PreparedTrackSource::Command attach{
            jam2::audio::PreparedTrackSource::CommandType::Swap,
            static_cast<std::uint32_t>(qMax(0, replacementSlot)),
            200,
            1,
            0,
            1000000,
        };
        const bool attached = replacementReady && source.enqueue(attach);
        output[0] = 0;
        if (attached) {
            (void)source.mix(output.data(), output.size(), 201);
        }
        record(QStringLiteral("prepared-track.attach-keeps-play-origin-and-catches-up"),
            started && attached && source.actualStartFrame() == 100 &&
            source.scheduledStartFrame() == 200 && source.sourceFrame() == 3 &&
            output[0] == (static_cast<std::int32_t>(3000) << 16));
    }
    {
        jam2::audio::PreparedTrackSource source(4);
        const int slot = source.claimLoadingSlot();
        std::int16_t* samples = source.loadingData(slot);
        if (samples != nullptr) {
            samples[0] = 1000;
            samples[1] = 2000;
            samples[2] = 3000;
            samples[3] = 4000;
        }
        const bool ready = samples != nullptr &&
            source.publishReady(slot, 4, 48000);
        const std::array<jam2::audio::PreparedTrackSource::Command, 3> attach{{
            {jam2::audio::PreparedTrackSource::CommandType::Swap,
             static_cast<std::uint32_t>(qMax(0, slot)), 72000, 2, 0, 1000000},
            {jam2::audio::PreparedTrackSource::CommandType::SetLoop,
             0, 72000, 0, 4, 1000000},
            {jam2::audio::PreparedTrackSource::CommandType::Play,
             0, 72000, 0, 0, 1000000},
        }};
        const bool queued = ready && source.enqueueBatch(attach);
        std::array<std::int32_t, 2> output{};
        if (queued) {
            (void)source.mix(output.data(), output.size(), 72000);
        }
        record(QStringLiteral("prepared-track.late-attach-starts-stopped-source"),
            queued && source.playing() && source.actualStartFrame() == 72000 &&
            output[0] == (static_cast<std::int32_t>(3000) << 16) &&
            output[1] == (static_cast<std::int32_t>(4000) << 16));
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
        jam2::GridProposal bootstrap;
        bootstrap.requester_peer_id = 1;
        bootstrap.request_id = 1;
        bootstrap.run_state = jam2::GridRunState::Running;
        const auto initial = authority.orderGridProposal(bootstrap);
        const bool activated = initial && authority.activateLocalGrid(1000, 1200);
        jam2::GridProposal ordinaryPeerEdit;
        ordinaryPeerEdit.requester_peer_id = 2;
        ordinaryPeerEdit.request_id = 1;
        ordinaryPeerEdit.run_state = jam2::GridRunState::Running;
        const auto ordinary = authority.orderGridProposal(ordinaryPeerEdit);
        jam2::GridProposal leaderStart = ordinaryPeerEdit;
        leaderStart.request_id = 2;
        leaderStart.mode = 1;
        leaderStart.claim_leader_audio_source = true;
        const auto leader = authority.orderGridProposal(leaderStart);
        jam2::GridProposal bpmReset = leaderStart;
        bpmReset.request_id = 3;
        bpmReset.reset_epoch = true;
        bpmReset.claim_leader_audio_source = false;
        const auto reset = authority.orderGridProposal(bpmReset);
        record(QStringLiteral("leader-audio-source-handoff-preserves-grid-epoch"),
            activated && ordinary && ordinary->authority_peer_id == 1 &&
            ordinary->authority_epoch_frame == 1000 &&
            leader && leader->authority_peer_id == 2 &&
            leader->authority_epoch_frame == 1000 &&
            reset && reset->authority_peer_id == 2 &&
            reset->authority_epoch_frame == 0);
    }

    {
        jam2::SessionAuthority authority(1, 1, 1);
        jam2::GridProposal bootstrap;
        bootstrap.requester_peer_id = 1;
        bootstrap.request_id = 1;
        bootstrap.run_state = jam2::GridRunState::Running;
        const auto initial = authority.orderGridProposal(bootstrap);
        jam2::GridProposal peerStart;
        peerStart.requester_peer_id = 2;
        peerStart.request_id = 1;
        peerStart.run_state = jam2::GridRunState::Running;
        peerStart.mode = 1;
        peerStart.claim_leader_audio_source = true;
        const auto leader = authority.orderGridProposal(peerStart);
        const bool missing = authority.markPeerInactive(2);
        jam2::GridProposal localRecovery;
        localRecovery.requester_peer_id = 1;
        localRecovery.request_id = 2;
        localRecovery.run_state = jam2::GridRunState::Running;
        const auto recovered = authority.orderGridProposal(localRecovery);
        record(QStringLiteral("grid-authority.departure-recovers-running-local-grid"),
            initial.has_value() && leader.has_value() &&
            leader->authority_peer_id == 2 && missing && recovered.has_value() &&
            recovered->authority_peer_id == 1 &&
            recovered->run_state == jam2::GridRunState::Running &&
            recovered->revision > leader->revision);
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
