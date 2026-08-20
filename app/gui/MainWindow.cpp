#include "MainWindow.hpp"
#include "MainWindowPages.hpp"
#include "ArrangementEditorDialog.hpp"
#include "AudioDeviceUiSupport.hpp"
#include "TrackWidgets.hpp"
#include "TrackAssetOwnership.hpp"
#include "TrackWorkspaceSupport.hpp"
#include "JamTasterProjectSupport.hpp"
#include "GuiPresentation.hpp"
#include "GuiControlContract.hpp"
#include "GuiInteractionPolicy.hpp"
#include "InputSourceDialogs.hpp"
#include "JamSyncDialog.hpp"
#include "JamTasterDialog.hpp"
#include "LaneRecordingDialog.hpp"
#include "SessionStartupDialogs.hpp"
#include "SettingsDialog.hpp"
#include "ListenerCompensationDialog.hpp"
#include "LooperAssetMaterializer.hpp"
#include "MidiInputBackend.hpp"
#include "InputPluginBackend.hpp"
#include "../jamtaster/JamTasterService.hpp"
#include "GuiControlMessageRouter.hpp"
#include "CuratedIdeaCatalog.hpp"
#include "CuratedIdeaDialog.hpp"
#include "ConnectionGuidance.hpp"
#include "MusicTheory.hpp"
#include "PracticeIdeaDialogs.hpp"
#include "RecordingTiming.hpp"
#include "SectionTimeline.hpp"
#include "PracticeIdeaController.hpp"
#include "PracticeReferenceRenderer.hpp"
#include "StyleProfileCatalog.hpp"

#include "Jam2MacPermissions.hpp"
#include "SessionController.hpp"
#include "ControlProtocol.hpp"
#include "ControlMessageValidation.hpp"
#include "ContentLimits.hpp"
#include "AssetChunkProtocol.hpp"

#include "common.hpp"
#include "audio_device.hpp"
#include "engine.hpp"
#include "peer_stream.hpp"
#include "pcm16_wav.hpp"
#include "session_authority.hpp"
#include "transport_timing.hpp"
#include "tuning_profile.hpp"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAbstractScrollArea>
#include <QAbstractButton>
#include <QApplication>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QKeyEvent>
#include <QList>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QIODevice>
#include <QProgressDialog>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QRunnable>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QScreen>
#include <QSet>
#include <QUrl>
#include <QSlider>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabBar>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QUuid>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr qint64 kMaxLooperAssetBytes = jam2::application::limits::kMaximumAssetBytes;
constexpr int kMaxLooperAssetRequests = jam2::application::limits::kMaximumAssetRequests;
constexpr int kMaxLooperTrackContributions = 512;
constexpr qint64 kTrackBatchIdleTimeoutMs = 30000;
// This is a hang detector, not a transfer-speed requirement. A sender serves
// its bounded asset queue serially, so a valid request can wait behind other
// peers without indicating a failed product operation.
constexpr int kLooperAssetRequestStartHangTimeoutMs = 60000;
constexpr int kFirewallGuidanceDisconnectThreshold = 3;
constexpr int kFirewallGuidanceWindowMs = 10000;

QString automationCompletionGateText(
    jam2::application::AutomationCompletionGateState state)
{
    using State = jam2::application::AutomationCompletionGateState;
    switch (state) {
    case State::Armed: return QStringLiteral("armed");
    case State::Active: return QStringLiteral("active");
    case State::Idle: return QStringLiteral("idle");
    case State::Unsupported:
    default: return QStringLiteral("unsupported");
    }
}

QString promptJamTasterSourceDispositionDialog(QWidget* parent)
{
    QMessageBox box(QMessageBox::Question,
                    QStringLiteral("Original source WAV"),
                    QStringLiteral(
                        "This action creates a new section arrangement. What should Jam2 "
                        "do with the original source WAV?"),
                    QMessageBox::NoButton,
                    parent);
    QPushButton* keep = box.addButton(
        QStringLiteral("Keep in final muted section"), QMessageBox::AcceptRole);
    QPushButton* remove = box.addButton(
        QStringLiteral("Move source file to Recycle Bin"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(keep);
    box.exec();
    if (box.clickedButton() == keep) return QStringLiteral("keep");
    if (box.clickedButton() == remove) return QStringLiteral("delete");
    return {};
}

QString practiceMeterIdForPattern(const jam2::metronome::PatternSnapshot& pattern)
{
    const auto matchesWrittenMeter = [&pattern](
        const jam2::practice::MeterDefinition& meter) {
        return meter.numerator == pattern.beats_per_bar &&
            meter.denominator == pattern.beat_unit &&
            meter.tempoPulseUnits == pattern.tempo_pulse_units;
    };
    for (const jam2::practice::MeterDefinition& meter :
         jam2::practice::meterCatalog()) {
        if (matchesWrittenMeter(meter)) return meter.id;
    }
    return {};
}

bool isManagedPracticeReference(const LooperLane& lane)
{
    return !lane.referenceKind.isEmpty() ||
        lane.name == QStringLiteral("Practice Chords") ||
        lane.name == QStringLiteral("Practice Drums") ||
        lane.name == QStringLiteral("Practice Melody") ||
        lane.name == QStringLiteral("Practice Bass") ||
        lane.name == QStringLiteral("Practice Support");
}

QString normalizedNetworkHost(QString host)
{
    if (host.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
        host = host.mid(7);
    }
    return host;
}

bool isLocalMachineAddress(const QString& host)
{
    const QHostAddress address(normalizedNetworkHost(host));
    if (address.isNull()) {
        return false;
    }
    if (address.isLoopback()) {
        return true;
    }
    const QList<QHostAddress> localAddresses = QNetworkInterface::allAddresses();
    return std::any_of(localAddresses.cbegin(), localAddresses.cend(), [&address](const QHostAddress& local) {
        return local == address;
    });
}

std::filesystem::path nativeFilePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

bool readPcm16WaveformPeaks(
    const QString& path,
    int peakCount,
    std::vector<float>& peaks,
    qint64* sourceFrames = nullptr)
{
    if (peakCount <= 0) {
        return false;
    }
    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
        nativeFilePath(path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
    if (!inspected || inspected.info.frames > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) ||
        inspected.info.data_offset > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
        !file.seek(static_cast<qint64>(inspected.info.data_offset))) {
        return false;
    }

    const qint64 frames = static_cast<qint64>(inspected.info.frames);
    const qint64 blockAlign = inspected.info.block_align;
    std::vector<int> peakValues(static_cast<std::size_t>(peakCount), 0);
    QByteArray buffer(64 * 1024, '\0');
    qint64 frameIndex = 0;
    qint64 bytesRemaining = static_cast<qint64>(inspected.info.data_bytes);
    while (bytesRemaining > 0) {
        qint64 bytesToRead = std::min<qint64>(bytesRemaining, buffer.size());
        bytesToRead -= bytesToRead % blockAlign;
        if (bytesToRead <= 0 || file.read(buffer.data(), bytesToRead) != bytesToRead) {
            return false;
        }
        for (qint64 offset = 0; offset < bytesToRead; offset += blockAlign, ++frameIndex) {
            int mixed = 0;
            for (std::uint16_t channel = 0; channel < inspected.info.channels; ++channel) {
                const qint64 sampleOffset = offset + static_cast<qint64>(channel) * 2;
                const std::uint16_t raw = static_cast<std::uint16_t>(
                    static_cast<unsigned char>(buffer[sampleOffset])) |
                    static_cast<std::uint16_t>(
                        static_cast<std::uint16_t>(static_cast<unsigned char>(buffer[sampleOffset + 1])) << 8U);
                mixed += std::abs(static_cast<int>(static_cast<std::int16_t>(raw)));
            }
            const int bucket = static_cast<int>(std::min<qint64>(
                peakCount - 1,
                frameIndex * peakCount / std::max<qint64>(1, frames)));
            peakValues[bucket] = std::max(
                peakValues[bucket],
                mixed / static_cast<int>(inspected.info.channels));
        }
        bytesRemaining -= bytesToRead;
    }

    peaks.resize(peakCount);
    for (int index = 0; index < peakCount; ++index) {
        peaks[index] = static_cast<float>(peakValues[index]) / 32768.0f;
    }
    const auto maxPeak = std::max_element(peaks.begin(), peaks.end());
    if (maxPeak != peaks.end() && *maxPeak > 0.0f && *maxPeak < 0.85f) {
        const float scale = 0.85f / *maxPeak;
        for (float& peak : peaks) {
            peak = std::min(1.0f, peak * scale);
        }
    }
    if (sourceFrames) {
        *sourceFrames = frames;
    }
    return true;
}

bool isSha256Hex(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}








QString timestampedCapturePath(const QString& prefix, const QString& folder = {})
{
    const QString fileName = QStringLiteral("%1-%2-%3.wav")
        .arg(
            prefix,
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")),
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    return folder.trimmed().isEmpty()
        ? appReleaseFilePath(QStringLiteral("captures"), fileName)
        : QDir(folder).absoluteFilePath(fileName);
}

QString safeFileName(QString name)
{
    name = name.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('-')) || name.startsWith(QLatin1Char('.'))) {
        name.remove(0, 1);
    }
    while (name.endsWith(QLatin1Char('-')) || name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
    }
    return name;
}

bool isDefaultEmptyTrackName(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed == QStringLiteral("Empty Track") || trimmed == QStringLiteral("Empty Lane")) {
        return true;
    }
    const QString prefix = QStringLiteral("Empty Track ");
    if (!trimmed.startsWith(prefix)) {
        return false;
    }
    bool ok = false;
    const int number = trimmed.mid(prefix.size()).toInt(&ok);
    return ok && number > 0;
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
        if (banks.at(bankIndex).id != bankId) {
            continue;
        }
        const QVector<LooperLane>& lanes = banks.at(bankIndex).lanes;
        for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
            if (lanes.at(laneIndex).id == laneId) {
                return {bankIndex, laneIndex};
            }
        }
        break;
    }
    return {};
}

bool isAutoCapturePath(const QString& path)
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    return path.isEmpty() ||
        name == QStringLiteral("capture.wav") ||
        name == QStringLiteral("take.wav") ||
        name.startsWith(QStringLiteral("capture-")) ||
        name.startsWith(QStringLiteral("take-")) ||
        name.startsWith(QStringLiteral("loopback-")) ||
        info.absolutePath() == QStringLiteral("/captures");
}

QString resolvedManagedCaptureFolder(
    const QString& configuredFolder,
    const QString& managedFolder)
{
    const QString configured = configuredFolder.trimmed();
    if (configured.isEmpty() ||
        QDir(configured).absolutePath() ==
            QDir(appReleaseFolderPath(QStringLiteral("captures"))).absolutePath()) {
        return managedFolder;
    }
    return configured;
}

QString sha256FileHex(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString frameText(qint64 frame)
{
    return frame >= 0 ? QString::number(frame) : QStringLiteral("-");
}

std::uint64_t rawFrameFromMusicalFrame(std::uint64_t musicalFrame, std::int64_t renderOffsetFrames)
{
    return jam2::transport_raw_frame_from_musical(
        musicalFrame, renderOffsetFrames);
}

std::uint64_t musicalFrameFromRawFrame(std::uint64_t rawFrame, std::int64_t renderOffsetFrames)
{
    return jam2::transport_musical_frame_from_raw(
        rawFrame, renderOffsetFrames);
}

int recordingCountInBeat(
    std::uint64_t currentFrame,
    std::uint64_t recordingStartFrame,
    std::uint64_t beatFrames)
{
    if (beatFrames == 0 || currentFrame >= recordingStartFrame) {
        return 0;
    }
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
    return jam2::gui::nextBankBoundaryBeat(
        absoluteBeat, beatsPerBar, quantizeToBar);
}

}

namespace {

QString keyToHex(const std::array<std::uint8_t, 16>& key)
{
    return QString::fromStdString(jam2::hex_encode(key.data(), key.size()));
}

QString sessionToHex(std::uint64_t session)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[bytes.size() - 1 - i] = static_cast<std::uint8_t>((session >> (i * 8)) & 0xffU);
    }
    return QString::fromStdString(jam2::hex_encode(bytes.data(), bytes.size()));
}














QString deviceId(const QString& text)
{
    const QRegularExpression re(QStringLiteral("^\\s*\\[?(\\d+)\\]?"));
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1) : text.trimmed();
}






QString onOff(bool value)
{
    return value ? QStringLiteral("on") : QStringLiteral("off");
}





std::vector<int> parseUiChannels(const QString& text, const char* label)
{
    std::vector<int> result;
    const QStringList parts = text.split(QLatin1Char(','), Qt::KeepEmptyParts);
    if (parts.isEmpty()) {
        throw std::runtime_error(std::string(label) + " requires at least one channel");
    }
    result.reserve(static_cast<std::size_t>(parts.size()));
    for (const QString& part : parts) {
        bool ok = false;
        const int channel = part.trimmed().toInt(&ok);
        if (!ok || channel <= 0) {
            throw std::runtime_error(std::string(label) + " channels must be positive 1-based numbers");
        }
        if (std::find(result.begin(), result.end(), channel - 1) != result.end()) {
            throw std::runtime_error(std::string(label) + " contains a duplicate channel");
        }
        result.push_back(channel - 1);
    }
    return result;
}



} // namespace


MainWindow::MainWindow(QWidget* parent)
    : MainWindow(
          parent,
          jam2::application::makeSystemMidiInputBackend(),
          jam2::application::makeSystemInputPluginBackend())
{
}

MainWindow::MainWindow(
    QWidget* parent,
    std::unique_ptr<jam2::application::MidiInputBackend> midiInputBackend)
    : MainWindow(
          parent,
          std::move(midiInputBackend),
          jam2::application::makeSystemInputPluginBackend())
{
}

MainWindow::MainWindow(
    QWidget* parent,
    std::unique_ptr<jam2::application::MidiInputBackend> midiInputBackend,
    std::unique_ptr<jam2::application::InputPluginBackend> inputPluginBackend)
    : QWidget(parent)
    , midiInputBackend_(midiInputBackend
          ? std::shared_ptr<jam2::application::MidiInputBackend>(
                std::move(midiInputBackend))
          : std::shared_ptr<jam2::application::MidiInputBackend>(
                jam2::application::makeSystemMidiInputBackend()))
    , inputPluginBackend_(inputPluginBackend
          ? std::shared_ptr<jam2::application::InputPluginBackend>(
                std::move(inputPluginBackend))
          : std::shared_ptr<jam2::application::InputPluginBackend>(
                jam2::application::makeSystemInputPluginBackend()))
    , preferences_(UserPreferencesStore::load())
    , trackWorkspace_(jam2_, this)
    , trackController_(trackWorkspace_.trackController)
    , looperProject_(trackWorkspace_.looperProject)
    , assetTransfer_(trackWorkspace_.assetTransfer)
    , metronomeTransport_(jam2_)
    , trackRecordingWorkflow_(trackWorkspace_.recordingWorkflow)
    , chordModel_(trackWorkspace_.songModel)
    , beatModel_(trackWorkspace_.songModel)
    , lyricModel_(trackWorkspace_.songModel)
    , projectPersistence_(trackWorkspace_.persistence)
    , preparedMixLifecycle_(trackWorkspace_.preparedMixLifecycle)
    , fileWorkerPool_(trackWorkspace_.fileWorkers)
    , publishStoppedTrackStateWhenApplied_(trackWorkspace_.publishStoppedTrackStateWhenApplied)
    , fileWorkerTasksActive_(trackWorkspace_.fileWorkerTasksActive)
    , fileWorkerTasksHighWater_(trackWorkspace_.fileWorkerTasksHighWater)
    , fileWorkerTasksCompleted_(trackWorkspace_.fileWorkerTasksCompleted)
    , fileWorkerTasksRejected_(trackWorkspace_.fileWorkerTasksRejected)
    , trackWaveformWorkerRunning_(trackWorkspace_.trackWaveformWorkerRunning)
    , trackWaveformRevision_(trackWorkspace_.trackWaveformRevision)
    , looperWaveformCache_(trackWorkspace_.looperWaveformCache)
    , looperWaveformWorkerRunning_(trackWorkspace_.looperWaveformWorkerRunning)
    , wavCompatibilityAuditRunning_(trackWorkspace_.wavCompatibilityAuditRunning)
    , pendingWavCompatibilityAuditRate_(trackWorkspace_.pendingWavCompatibilityAuditRate)
    , reportedIncompatibleWavs_(trackWorkspace_.reportedIncompatibleWavs)
    , pendingTrackContributions_(trackWorkspace_.pendingTrackContributions)
    , appliedTrackContributionIds_(trackWorkspace_.appliedTrackContributionIds)
    , localTrackOffers_(trackWorkspace_.localTrackOffers)
    , trackOfferAssetPaths_(trackWorkspace_.trackOfferAssetPaths)
    , pendingTrackAssetSources_(trackWorkspace_.pendingTrackAssetSources)
    , validatedTrackAssetHashes_(trackWorkspace_.validatedTrackAssetHashes)
    , incomingAssetWorkflow_(trackWorkspace_.incomingAssetWorkflow)
    , incomingAssetHash_(trackWorkspace_.incomingAssetHash)
    , incomingAssetSourcePeerToken_(trackWorkspace_.incomingAssetSourcePeerToken)
    , pendingSongSet_(trackWorkspace_.pendingSongSet)
    , pendingLooperAssetHashes_(trackWorkspace_.pendingLooperAssetHashes)
    , pendingSongRevision_(trackWorkspace_.pendingSongRevision)
    , pendingSongTrackRestart_(trackWorkspace_.pendingSongTrackRestart)
    , pendingSongSourcePeerToken_(trackWorkspace_.pendingSongSourcePeerToken)
    , pendingSongNeedsAuthoritativePublish_(trackWorkspace_.pendingSongNeedsAuthoritativePublish)
    , songAssetCheckRevision_(trackWorkspace_.songAssetCheckRevision)
    , deferredSongSetMessage_(trackWorkspace_.deferredSongSetMessage)
    , deferredSongSetSourcePeerToken_(trackWorkspace_.deferredSongSetSourcePeerToken)
    , looperArrangementRevision_(trackWorkspace_.looperArrangementRevision)
    , lastAppliedHostArrangementRevision_(trackWorkspace_.lastAppliedHostArrangementRevision)
{
    trackWorkspace_.setCallbacks({
        [this] { return sessionController_.snapshot().contract.sampleRate; },
        [this](const QString& message) {
            appendLog(message);
            const QString lower = message.toLower();
            if (performanceHome_ &&
                (lower.contains(QStringLiteral("failed")) ||
                 lower.contains(QStringLiteral("timeout")) ||
                 lower.contains(QStringLiteral("could not")) ||
                 lower.contains(QStringLiteral("cancelled")))) {
                performanceHome_->setTrackTransferStatus(
                    QStringLiteral("TRACK SHARE ERROR — see Data / Logs"));
            }
        },
        [this](const QString& token, qint64 bytes) { return canQueueAssetTo(token, bytes); },
        [this](const QString& token, const QJsonObject& message) {
            return sendAssetControlTo(token, message);
        },
        [this](const QString& token, const QByteArray& payload) {
            return sendAssetBinaryTo(token, payload);
        },
        [this] {
            applyPendingTrackContributions();
            applyPendingSongIfAssetsReady();
            requestNextPendingAsset();
        },
        [this](const QString& hash, const QString& source) {
            retryOrFailIncomingAsset(hash, source);
        },
        [this](const QString& hash, const QString& peerToken, bool receiving) {
            noteTrackAssetProgress(hash, peerToken, receiving);
        },
    });
    jamTaster_ = std::make_unique<JamTasterService>(this);
    JamTasterService::Observer jamTasterObserver;
    jamTasterObserver.log = [this](const QString& message) {
        appendLog(QStringLiteral("JamTaster worker: ") + message);
    };
    jamTasterObserver.jobProgress = [this](const QJsonObject& event) {
        const QString message = event.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) appendLog(QStringLiteral("JamTaster: ") + message);
    };
    jamTasterObserver.jobStarted = [this](const QString& action) {
        appendLog(QStringLiteral("JamTaster job started in isolated process: ") + action);
    };
    const auto showJamTasterOutcome = [this](
        const QString& pill,
        const QString& status,
        const QString& state,
        bool issue) {
        ++jamTasterStatusRevision_;
        setSessionHeaderStatus(
            pill,
            status,
            {QStringLiteral("Click to reopen JamTaster.")},
            issue,
            true);
        connectionLabel_->setProperty("jamtaster", state);
        connectionLabel_->style()->unpolish(connectionLabel_);
        connectionLabel_->style()->polish(connectionLabel_);
        const quint64 revision = jamTasterStatusRevision_;
        QTimer::singleShot(8000, this, [this, revision] {
            if (revision == jamTasterStatusRevision_ &&
                !jamTaster_->taskActive()) {
                restoreSessionHeaderStatus();
            }
        });
    };
    jamTasterObserver.jobFinished = [showJamTasterOutcome](const QJsonObject&) {
        showJamTasterOutcome(
            QStringLiteral("JAMTASTER DONE"),
            QStringLiteral("JamTaster analysis complete"),
            QStringLiteral("complete"),
            false);
    };
    jamTasterObserver.jobFailed = [this, showJamTasterOutcome](const QString& error) {
        appendLog(QStringLiteral("JamTaster worker failed: ") + error);
        showJamTasterOutcome(
            QStringLiteral("JAMTASTER ERROR"), error,
            QStringLiteral("error"), true);
    };
    jamTasterObserver.taskCancelled = [showJamTasterOutcome] {
        showJamTasterOutcome(
            QStringLiteral("JAMTASTER STOPPED"),
            QStringLiteral("JamTaster analysis cancelled"),
            QStringLiteral("cancelled"),
            false);
    };
    jamTasterObserver.taskStatusChanged = [this](const QString&, int percent, bool active) {
                ++jamTasterStatusRevision_;
                if (performanceHome_) {
                    performanceHome_->setJamTasterTaskStatus(active, percent);
                }
                if (active) {
                    showJamTasterSessionHeaderStatus();
                }
            };
    (void)jamTaster_->addObserver(std::move(jamTasterObserver));
    installJam2Style();
    generateSession();
    (void)JamStorage::pruneEmptyUnsavedWorkspaces();
    const QString initialJamName = JamStorage::randomDisplayName();
    chordModel_.setTitle(initialJamName);
    looperProject_.ensureInitialEmptyLanes();
    jamStorage_.startNew(initialJamName);
    projectPersistence_.initializeWorkspace(jamStorage_.rootFolder());
    MainWindowPages::build(*this);
    applyPreferencesToControls();
    applyNewJamDefaults();
    projectPersistence_.acceptNewProject(currentProjectSnapshot());
    QApplication::instance()->installEventFilter(this);

    jam2_.onLog = [this](const QString& line) { appendLog(line); };
    jam2_.onError = [this](const QString& line) {
        appendLog(QStringLiteral("runtime error: ") + line);
        pendingJamRuntimeError_ = line.trimmed();
        const SharedSessionController::Role role = sessionController_.snapshot().role;
        if (!shuttingDown_ && (jamStartupPending_ ||
            role == SharedSessionController::Role::Creator ||
            role == SharedSessionController::Role::Joiner)) {
            // Runtime startup can fail synchronously inside the controller. Let
            // that stack unwind and publish its typed failure before opening a
            // modal dialog; otherwise the nested event loop can expose both the
            // low-level and controller errors as separate popups.
            QTimer::singleShot(0, this, [this] {
                const SharedSessionController::Role currentRole =
                    sessionController_.snapshot().role;
                if (!shuttingDown_ && (jamStartupPending_ ||
                    currentRole == SharedSessionController::Role::Creator ||
                    currentRole == SharedSessionController::Role::Joiner)) {
                    showJamFailure(pendingJamRuntimeError_);
                }
            });
        }
    };
    jam2_.onEngineSnapshot = [this](const jam2::EngineSnapshot& snapshot) {
        handleEngineSnapshot(snapshot);
    };
    jam2_.onEngineEvent = [this](const jam2::EngineEvent& event) {
        handleEngineEvent(event);
    };
    jam2_.onNetworkSnapshot = [this](const Jam2NetworkOperationalSnapshot& snapshot) {
        handleNetworkSnapshot(snapshot);
    };
    jam2_.onConnectionDiagnostics = [this](const ConnectionDiagnosticsSnapshot& snapshot) {
        handleConnectionDiagnostics(snapshot);
    };
    jam2_.onStartup = [this](const Jam2RuntimeStartup& startup) {
        jamStartupPending_ = false;
        const QString local = QString::fromStdString(jam2::endpoint_to_string(startup.local_endpoint));
        appendLog(QStringLiteral("native UDP session ready: ") + local);
        if (startup.public_candidate) {
            activePublicEndpoint_ = QString::fromStdString(jam2::endpoint_to_string(*startup.public_candidate));
            sessionRuntimeDraft_.configuration.publicHost =
                QString::fromStdString(startup.public_candidate->host);
        }
        if (sessionController_.isServer()) {
            const QString advertised = activePublicEndpoint_.isEmpty() ? local : activePublicEndpoint_;

            meshPeerEndpoints_[meshPeerToken()] = advertised;
            sessionController_.updateLocalEndpoint(advertised);
            showPendingMeshInviteUrl();
        }
        if (startup.stats_csv) {
            appendLog(QStringLiteral("Stats CSV: ") + QString::fromStdString(startup.stats_csv->string()));
        }
        updateRuntimeControls();
        if (tunerCommandCookie_ == 0 &&
            jam2_.engineSnapshot().pitch.enabled != tunerRequestedEnabled_) {
            setTunerEnabled(tunerRequestedEnabled_);
        }
        const SharedSessionController::Snapshot session = sessionController_.snapshot();
        if ((session.role == SharedSessionController::Role::Creator ||
             session.role == SharedSessionController::Role::Joiner) &&
            session.contract.sampleRate > 0) {
            auditWavCompatibilityForSession(session.contract.sampleRate, true);
        }
        if (sessionController_.isServer()) {
            // A joiner must adopt the existing ordered grid before it may
            // originate edits. Publishing its retained local/default settings
            // here can replace a running session with a stopped revision during
            // rejoin. The creator seeds a new session; later user changes remain
            // collaborative from either peer.
            sendMetronomeModeToJam();
            sendMetronomePatternToJam();
            sendJamSyncPolicy();
            if (jamSyncPolicy_.trackLanes) {
                sendSongSnapshot();
            }
        }
    };
    jam2_.onNetworkFinished = [this](int code) {
        appendLog(QStringLiteral("network runtime finished rc=%1").arg(code));
        const SharedSessionController::Role role = sessionController_.snapshot().role;
        if (code != 0 && !shuttingDown_ &&
            (role == SharedSessionController::Role::Creator ||
             role == SharedSessionController::Role::Joiner)) {
            showJamFailure(pendingJamRuntimeError_.isEmpty()
                ? QStringLiteral("The in-process network runtime stopped during startup (error code %1).")
                    .arg(code)
                : pendingJamRuntimeError_);
            QTimer::singleShot(0, this, [this] { stopJam(true); });
        }
    };
    sessionController_.onTransportEvent = [this](
        const jam2::control_protocol::TransportEvent& event,
        bool serverSide) {
        handleControlEvent(event, serverSide);
    };
    sessionController_.onMessage = [this](const QString& sourcePeerToken, const QJsonObject& message) {
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("looper.recording.group.start")) {
            if (!syncedRecordingsEnabled()) return;
            handleRecordingGroupStart(message);
            return;
        }
        if (type == QStringLiteral("looper.recording.group.finish")) {
            if (!syncedRecordingsEnabled()) return;
            handleRecordingGroupFinish(message);
            return;
        }
        if (type == QStringLiteral("looper.recording.group.recover.request")) {
            if (!sessionController_.isServer() || !syncedRecordingsEnabled()) return;
            const QString groupId = message.value(QStringLiteral("group_id")).toString();
            if (groupId != activeRecordingGroupId_) return;
            const QJsonObject recovery{
                {QStringLiteral("type"),
                    QStringLiteral("looper.recording.group.recover")},
                {QStringLiteral("group_id"), groupId},
            };
            sendControl(recovery);
            handleRecordingGroupRecovery(recovery);
            return;
        }
        if (type == QStringLiteral("looper.recording.group.recover")) {
            if (!syncedRecordingsEnabled()) return;
            handleRecordingGroupRecovery(message);
            return;
        }
        if (type == QStringLiteral("looper.recording.resync.request")) {
            if (!jamSyncPolicy_.globalPlayback) return;
            if (sessionController_.isServer()) {
                sendJamSyncPolicy(sourcePeerToken);
                sendSongSnapshot();
                (void)sendControlTo(sourcePeerToken, QJsonObject{
                    {QStringLiteral("type"),
                        QStringLiteral("looper.recording.resync.state")},
                    {QStringLiteral("track_playing"),
                        trackRecordingWorkflow_.globalTransportRequestedPlaying()},
                    {QStringLiteral("active_bank"),
                        looperProject_.activeBankIndex()},
                });
            }
            return;
        }
        if (type == QStringLiteral("looper.recording.resync.state")) {
            if (!jamSyncPolicy_.globalPlayback) return;
            pendingRecordingResyncPlaying_ = message.value(
                QStringLiteral("track_playing")).toBool();
            applyPendingRecordingTransportResync();
            return;
        }
        if (deferIncomingControlForLaneRecording(message, sourcePeerToken)) {
            return;
        }
        if (type == QStringLiteral("jam.sync.set") ||
            type == QStringLiteral("jam.sync.request")) {
            handleJamSyncMessage(message, sourcePeerToken);
            return;
        }
        if (type == QStringLiteral("jam.metronome.state.set") ||
            type == QStringLiteral("jam.metronome.state.request")) {
            const bool enabled = message.value(QStringLiteral("enabled")).toBool();
            if (type == QStringLiteral("jam.metronome.state.request")) {
                if (sessionController_.isServer() && jamSyncPolicy_.metronomeState) {
                    setMetronomeEnabled(enabled, false);
                    sendControl(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("jam.metronome.state.set")},
                        {QStringLiteral("enabled"), enabled},
                    });
                }
            } else if (!sessionController_.isServer() && jamSyncPolicy_.metronomeState) {
                setMetronomeEnabled(enabled, false);
            }
            return;
        }
        if (type == QStringLiteral("bank.request")) {
            if (sessionController_.isServer() && jamSyncPolicy_.globalPlayback) {
                std::optional<quint64> targetBeat;
                bool targetOk = false;
                const quint64 requestedTarget = message.value(
                    QStringLiteral("target_abs_beat")).toString().toULongLong(&targetOk);
                if (targetOk && requestedTarget > 0) targetBeat = requestedTarget;
                beginSharedBankLaunch(
                    message.value(QStringLiteral("bank")).toInt(), targetBeat);
            }
            return;
        }
        if (type == QStringLiteral("bank.prepare")) {
            if (!sessionController_.isServer()) {
                const int bank = message.value(QStringLiteral("bank")).toInt();
                const QString switchId = message.value(QStringLiteral("switch_id")).toString();
                if (jamSyncPolicy_.globalPlayback) {
                    prepareSharedBankLaunch(bank, switchId);
                } else {
                    // An opted-out peer is not part of shared playback, but it
                    // must not hold the other peers behind the bounded barrier.
                    sendControl(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("bank.ready")},
                        {QStringLiteral("switch_id"), switchId},
                        {QStringLiteral("bank"), bank},
                    });
                }
            }
            return;
        }
        if (type == QStringLiteral("bank.ready")) {
            if (sessionController_.isServer() && jamSyncPolicy_.globalPlayback) {
                handleSharedBankReady(
                    message.value(QStringLiteral("bank")).toInt(),
                    message.value(QStringLiteral("switch_id")).toString(),
                    sourcePeerToken);
            }
            return;
        }
        if (type == QStringLiteral("bank.cancel")) {
            const QString switchId = message.value(QStringLiteral("switch_id")).toString();
            if (!sessionController_.isServer() &&
                switchId == sharedBankLaunch_.snapshot().switchId) {
                cancelSharedBankLaunch(false, QStringLiteral("cancelled by the jam creator"));
            }
            return;
        }
        if (type == QStringLiteral("bank.switch")) {
            if (!sessionController_.isServer() && jamSyncPolicy_.globalPlayback) {
                bool targetOk = false;
                const quint64 targetBeat = message.value(QStringLiteral("target_abs_beat"))
                    .toString().toULongLong(&targetOk);
                const QString switchId = message.value(QStringLiteral("switch_id")).toString();
                const int bankIndex = message.value(QStringLiteral("bank")).toInt();
                if (targetOk && sharedBankLaunch_.matches(switchId, bankIndex)) {
                    sharedBankLaunch_.clear();
                    schedulePreparedBankLaunch(bankIndex, targetBeat);
                }
            }
            return;
        }
        if (type == QStringLiteral("practice.references.render")) {
            handlePracticeReferenceRenderRequest(message, sourcePeerToken);
            return;
        }
        GuiControlMessageRouter::dispatch({
            [] { return true; },
            [this](const QString& text) { appendLog(text); },
            [this](const QString& text) {
                QMessageBox::warning(this, QStringLiteral("Jam2"), text);
            },
            &chordModel_,
            &beatModel_,
            &lyricModel_,
            [this](const QString& lane) { refreshSongView(lane); },
            [this](const QJsonObject& value, const QString& source) {
                handleSongSet(value, source);
            },
            [this](const QJsonObject& request) {
                publishLocalTrackBatch(
                    request.value(QStringLiteral("batch_id")).toString());
            },
            [this](const QJsonObject& value, const QString& source) {
                handleTrackBatchOffer(value, source);
            },
            [this](const QJsonObject& value, const QString& source) {
                handleTrackBatchComplete(value, source);
            },
            [this](const QJsonObject& value, const QString& source) {
                handleTrackRecordingState(value, source);
            },
            &assetTransfer_,
        }, message, sourcePeerToken);
    };
    sessionController_.onAssetMessage = [this](
        const QString& sourcePeerToken,
        const QJsonObject& message) {
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("looper.asset.start")) {
            assetTransfer_.receiveStart(message, sourcePeerToken);
        } else if (type == QStringLiteral("looper.asset.ack")) {
            assetTransfer_.receiveAck(message, sourcePeerToken);
        } else if (type == QStringLiteral("looper.asset.done")) {
            assetTransfer_.receiveDone(message, sourcePeerToken);
        } else {
            appendLog(QStringLiteral("rejected unexpected TCP asset control message"));
        }
    };
    sessionController_.onAssetBinaryMessage = [this](
        const QString& sourcePeerToken,
        const QByteArray& payload) {
        assetTransfer_.receiveChunk(payload, sourcePeerToken);
    };
    sessionController_.onAssetDisconnected = [this](const QString& sourcePeerToken) {
        assetTransfer_.peerDisconnected(sourcePeerToken);
    };
    sessionController_.onPeerAuthenticated = [this](const QString& token, const QJsonObject& message) {
        handleMeshPeerAuthenticated(token, message);
    };
    sessionController_.onPeerDisconnected = [this](const QString& token) {
        assetTransfer_.peerDisconnected(token);
        if (pendingSongSourcePeerToken_ == token) {
            ++songAssetCheckRevision_;
            pendingSongSet_ = {};
            pendingSongRevision_ = 0;
            trackWorkspace_.pendingSongBaseRevision = 0;
            pendingSongTrackRestart_ = false;
            pendingSongSourcePeerToken_.clear();
            pendingSongNeedsAuthoritativePublish_ = false;
            pendingLooperAssetHashes_.clear();
        }
        if (deferredSongSetSourcePeerToken_ == token) {
            ++songAssetCheckRevision_;
            deferredSongSetMessage_ = {};
            deferredSongSetSourcePeerToken_.clear();
        }
        if (trackWorkspace_.heldTrackShareSongSourcePeerToken == token) {
            trackWorkspace_.heldTrackShareSongSet = {};
            trackWorkspace_.heldTrackShareSongSourcePeerToken.clear();
        }
        for (auto it = pendingTrackAssetSources_.begin();
             it != pendingTrackAssetSources_.end();) {
            if (it.value() == token) {
                trackWorkspace_.incomingAssetRetryAttempts.remove(it.key());
                trackWorkspace_.incomingAssetRetrySources.remove(it.key());
                it = pendingTrackAssetSources_.erase(it);
            } else {
                ++it;
            }
        }
        QSet<QString> disconnectedBatches;
        for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
            if (contribution.sourcePeerToken == token) {
                disconnectedBatches.insert(contribution.batchId);
            }
        }
        for (const QString& batchId : std::as_const(disconnectedBatches)) {
            expirePendingTrackBatch(token, batchId);
        }
        for (auto it = trackWorkspace_.outgoingTrackSharePendingPeers.begin();
             it != trackWorkspace_.outgoingTrackSharePendingPeers.end();) {
            it->remove(token);
            if (it->isEmpty()) {
                const QString batchId = it.key();
                it = trackWorkspace_.outgoingTrackSharePendingPeers.erase(it);
                trackWorkspace_.outgoingTrackShareBatchHashes.remove(batchId);
                trackWorkspace_.outgoingTrackShareLastProgressMs.remove(batchId);
            } else {
                ++it;
            }
        }
        releaseHeldTrackSnapshotIfReady();
        localMeshPeerTokens_.remove(token);
        updateMixRemotePeers();
        if (sessionController_.isServer() && sharedBankLaunch_.active()) {
            (void)sharedBankLaunch_.removeExpectedPeer(token);
            maybeCommitSharedBankLaunch();
        }
    };
    sessionController_.onContract = [this](const SharedSessionController::SessionContract& contract) {
        appendLog(QStringLiteral("session contract protocol=%1 format=%2 profile=%3 sample_rate=%4 frame_size=%5")
            .arg(contract.protocolVersion)
            .arg(contract.audioFormat, contract.profile)
            .arg(contract.sampleRate)
            .arg(contract.frameSize));
        if (sessionController_.snapshot().role == SharedSessionController::Role::Joiner) {
            sessionRuntimeDraft_.configuration.sampleRate = contract.sampleRate;
            sessionRuntimeDraft_.configuration.tuning.frameSize = contract.frameSize;
            sessionRuntimeDraft_.configuration.audioFormat = contract.audioFormat;
        }
        auditWavCompatibilityForSession(contract.sampleRate, true);
    };
    sessionController_.onSnapshot = [this](const SharedSessionController::Snapshot& snapshot) {
        applySessionSnapshot(snapshot);
    };
    sessionController_.bindRuntime(
        jam2_,
        [this](const SharedSessionController::Snapshot& snapshot) {
            Jam2RuntimeOptions options = networkRuntimeOptions(snapshot);
            if (snapshot.role == SharedSessionController::Role::Joiner &&
                !options.headless_audio &&
                !selectedDeviceSupportsSampleRate(snapshot.contract.sampleRate)) {
                throw std::runtime_error(
                    QStringLiteral("Selected audio device '%1' does not support session sample rate %2 Hz")
                        .arg(selectedDeviceDescription())
                        .arg(snapshot.contract.sampleRate).toStdString());
            }
            return options;
        });
    songAssetCheckRetryTimer_.setSingleShot(true);
    QObject::connect(&songAssetCheckRetryTimer_, &QTimer::timeout, this, [this] {
        const QJsonObject message = deferredSongSetMessage_;
        const QString sourcePeerToken = deferredSongSetSourcePeerToken_;
        deferredSongSetMessage_ = QJsonObject{};
        deferredSongSetSourcePeerToken_.clear();
        if (!message.isEmpty()) {
            handleSongSet(message, sourcePeerToken);
        }
    });
    playbackGridTimer_.setInterval(16);
    playbackGridTimer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&playbackGridTimer_, &QTimer::timeout, this, [this] {
        refreshInputSourceRouting();
        updatePlaybackGrid();
    });
    playbackGridTimer_.start();
    trackTimelineTimer_.setInterval(16);
    trackTimelineTimer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&trackTimelineTimer_, &QTimer::timeout, this, [this] { updateTrackTimeline(); });
    trackTimelineTimer_.start();
    QTimer::singleShot(0, this, [this] { initializeStartupWorkflow(); });
}

MainWindow::~MainWindow()
{
    shuttingDown_ = true;
    QString ignored;
    (void)automationReleaseFileWorkers(ignored);
    std::string midiIgnored;
    (void)midiInputBackend_->releaseAutomationCompletionGate(midiIgnored);
    fileWorkerPool_.waitForDone();
    QApplication::instance()->removeEventFilter(this);
    stopJam(false);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    bool discardUnsavedWorkspace = false;
    const bool hasTransientWavs = projectPersistence_.hasExistingTransientWavs() ||
        !trackRecordingWorkflow_.pendingTransientCapturePath().isEmpty();
    if (hasUnsavedProjectChanges() || hasTransientWavs) {
        QMessageBox dialog(
            QMessageBox::Question,
            QStringLiteral("Close Jam2"),
            QStringLiteral("Save this project before closing?"),
            QMessageBox::NoButton,
            this);
        QPushButton* save = dialog.addButton(QStringLiteral("Save Project"), QMessageBox::AcceptRole);
        QPushButton* discard = dialog.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        QPushButton* cancel = dialog.addButton(QMessageBox::Cancel);
        jam2::gui::registerGuiControl(
            *save, QStringLiteral("application.close-dialog.save"),
            QStringLiteral("application.window-close"),
            jam2::gui::GuiControlAvailability::Modal);
        jam2::gui::registerGuiControl(
            *discard, QStringLiteral("application.close-dialog.discard"),
            QStringLiteral("application.window-close"),
            jam2::gui::GuiControlAvailability::Modal);
        jam2::gui::registerGuiControl(
            *cancel, QStringLiteral("application.close-dialog.cancel"),
            QStringLiteral("application.window-close"),
            jam2::gui::GuiControlAvailability::Modal);
        dialog.setInformativeText(QStringLiteral(
            "Discarding removes temporary Track-view WAVs created or staged in this session."));
        dialog.exec();
        if (dialog.clickedButton() == cancel || dialog.clickedButton() == nullptr) {
            event->ignore();
            return;
        }
        if (dialog.clickedButton() == save && !saveSong()) {
            event->ignore();
            return;
        }
        discardUnsavedWorkspace = dialog.clickedButton() == discard;
    }
    shuttingDown_ = true;
    stopJam(false);
    if (loopbackRecorder_.isRunning()) {
        loopbackRecorder_.stop();
    }
    if (!trackRecordingWorkflow_.pendingTransientCapturePath().isEmpty()) {
        registerTransientTrackWav(trackRecordingWorkflow_.abandonPendingCapture());
    }
    if (discardUnsavedWorkspace && !jamStorage_.isSaved()) {
        fileWorkerPool_.waitForDone();
        QString discardError;
        if (!jamStorage_.discardUnsaved(discardError)) {
            QMessageBox::warning(this, QStringLiteral("Discard Jam"), discardError);
            event->ignore();
            return;
        }
        projectPersistence_.clearTransientTracking();
    } else {
        cleanupTransientTrackWavs();
    }
    event->accept();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* target = qobject_cast<QWidget*>(watched);
        const bool belongsToMainWindow = target &&
            (target == this || isAncestorOf(target));
        QWidget* focus = QApplication::focusWidget();
        const bool modalOpen = QApplication::activeModalWidget() != nullptr;
        const bool dialogTarget = target &&
            qobject_cast<QDialog*>(target->window()) != nullptr;
        const bool valueEditorFocused =
            jam2::gui::explicitValueEditorHasFocus(focus);
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers eventModifiers =
            keyEvent->modifiers() & ~Qt::KeypadModifier;

        if (belongsToMainWindow && !dialogTarget &&
            keyEvent->key() == Qt::Key_Tab &&
            eventModifiers == Qt::NoModifier) {
            return true;
        }

        if (belongsToMainWindow && !valueEditorFocused) {
            const int key = keyEvent->key();
            const bool navigationKey = key == Qt::Key_Left || key == Qt::Key_Right ||
                key == Qt::Key_Up || key == Qt::Key_Down ||
                key == Qt::Key_PageUp || key == Qt::Key_PageDown ||
                key == Qt::Key_End;
            if (navigationKey &&
                jam2::gui::blocksIncidentalNavigationKey(focus)) {
                return true;
            }
        }

        if (belongsToMainWindow && !modalOpen && !dialogTarget &&
            !valueEditorFocused) {
            const Qt::KeyboardModifiers modifiers = eventModifiers;
            const bool noModifiers = modifiers == Qt::NoModifier;
            const bool shiftOnly = modifiers == Qt::ShiftModifier;
            const bool controlOnly = modifiers == Qt::ControlModifier;
            const bool repeated = keyEvent->isAutoRepeat();
            const int key = keyEvent->key();

            const auto currentView = [this]() -> QString {
                if (!performanceStageStack_ || performanceStageStack_->currentIndex() == 0) {
                    return QStringLiteral("performance");
                }
                if (!workspaceStack_) return QStringLiteral("performance");
                for (auto it = workspacePages_.cbegin(); it != workspacePages_.cend(); ++it) {
                    if (it.value() == workspaceStack_->currentIndex()) return it.key();
                }
                return QStringLiteral("performance");
            };
            const auto triggerView = [this, repeated](const QString& view) {
                if (!repeated) openWorkspace(view);
            };

            if (noModifiers && key >= Qt::Key_1 && key <= Qt::Key_6) {
                static const QStringList views{
                    QStringLiteral("performance"),
                    QStringLiteral("chords"),
                    QStringLiteral("beats"),
                    QStringLiteral("lyrics"),
                    QStringLiteral("metronome"),
                    QStringLiteral("looper"),
                };
                triggerView(views.at(key - Qt::Key_1));
                return true;
            }
            if (noModifiers && key == Qt::Key_Home) {
                triggerView(QStringLiteral("performance"));
                return true;
            }
            if ((key == Qt::Key_Backtab && (shiftOnly || noModifiers)) ||
                (key == Qt::Key_Tab && shiftOnly)) {
                static const QStringList views{
                    QStringLiteral("performance"),
                    QStringLiteral("chords"),
                    QStringLiteral("beats"),
                    QStringLiteral("lyrics"),
                    QStringLiteral("metronome"),
                    QStringLiteral("looper"),
                };
                const int current = qMax(0, views.indexOf(currentView()));
                triggerView(views.at((current + 1) % views.size()));
                return true;
            }
            if (noModifiers && key == Qt::Key_Space) {
                if (!repeated && performanceTrackToggle_ &&
                    performanceTrackToggle_->isEnabled()) {
                    performanceTrackToggle_->click();
                }
                return true;
            }
            if (shiftOnly && key == Qt::Key_Space) {
                if (!repeated && trackRecordingWorkflow_.globalTransportPlaying()) {
                    const int bankCount = qMax(1, looperProject_.banks().size());
                    const int next =
                        (qBound(0, looperProject_.activeBankIndex(), bankCount - 1) + 1) % bankCount;
                    requestBankLaunch(next);
                }
                return true;
            }
            if (noModifiers && key == Qt::Key_M) {
                if (!repeated && performanceMetronomeToggle_ &&
                    performanceMetronomeToggle_->isEnabled()) {
                    performanceMetronomeToggle_->click();
                }
                return true;
            }
            if (noModifiers && key == Qt::Key_F) {
                if (!repeated) {
                    const QString view = currentView();
                    if (view == QStringLiteral("chords") && chordGrid_) {
                        chordGrid_->toggleFocusCurrentBar();
                    } else if (view == QStringLiteral("beats") && beatGrid_) {
                        beatGrid_->toggleFocusCurrentBar();
                    }
                }
                return true;
            }
            if (controlOnly && key == Qt::Key_A) {
                if (!repeated && currentView() == QStringLiteral("looper") &&
                    selectedLooperLane_ >= 0 && !sharedRecordingProtected() &&
                    !trackRecordingWorkflow_.laneArmedAt(
                        viewedBankIndex_, selectedLooperLane_)) {
                    (void)armSelectedLooperLaneRecording();
                }
                return true;
            }
            if (controlOnly && key == Qt::Key_R) {
                if (!repeated && currentView() == QStringLiteral("looper") &&
                    selectedLooperLane_ >= 0 && startArmedLaneRecordingButton_ &&
                    startArmedLaneRecordingButton_->isEnabled()) {
                    startArmedLaneRecordingButton_->click();
                }
                return true;
            }

            if ((key == Qt::Key_Return || key == Qt::Key_Enter ||
                 key == Qt::Key_Space) && qobject_cast<QAbstractButton*>(focus)) {
                return true;
            }
        }
    }
    if (watched == connectionLabel_ &&
        event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton &&
            connectionLabel_->text().startsWith(QStringLiteral("JAMTASTER"))) {
            showJamTasterDialog();
            return true;
        }
        if (mouse->button() == Qt::LeftButton && controlRefreshAvailable_) {
            refreshControlConnection();
            return true;
        }
        const SharedSessionController::Role role = sessionController_.snapshot().role;
        const bool audioAction = connectionLabel_->text() == QStringLiteral("AUDIO OFF") ||
            connectionLabel_->text() == QStringLiteral("AUDIO ISSUE");
        if (mouse->button() == Qt::LeftButton &&
            (audioAction || !jam2_.isRunning()) &&
            !jamStartupPending_ &&
            role != SharedSessionController::Role::Creator &&
            role != SharedSessionController::Role::Joiner) {
            showSettingsDialog();
            return true;
        }
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (!jam2::gui::isComboBoxPopupObject(watched)) {
            const bool directHorizontal =
                wheel->pixelDelta().x() != 0 || wheel->angleDelta().x() != 0;
            const bool shiftedHorizontal =
                wheel->modifiers().testFlag(Qt::ShiftModifier) &&
                (wheel->pixelDelta().y() != 0 || wheel->angleDelta().y() != 0);
            if (directHorizontal || shiftedHorizontal) {
                if (auto* scrollArea = jam2::gui::parentScrollArea(
                        watched, Qt::Horizontal)) {
                    if (jam2::gui::scrollAreaByWheel(
                            *scrollArea,
                            *wheel,
                            Qt::Horizontal,
                            shiftedHorizontal && !directHorizontal)) {
                        return true;
                    }
                }
            }
        }
        if (jam2::gui::isWheelValueEditor(watched)) {
            if (jam2::gui::isComboBoxPopupObject(watched)) return false;
            if (auto* scrollArea = jam2::gui::parentScrollArea(
                    watched, Qt::Vertical)) {
                if (jam2::gui::scrollAreaByWheel(
                        *scrollArea, *wheel, Qt::Vertical)) return true;
            }
            return false;
        }
    }
    return QWidget::eventFilter(watched, event);
}













void MainWindow::updateMixControls()

{
    const bool jamActive = jam2_.isRunning();
    const bool networkActive = jam2_.isNetworkRunning();
    auto setModeEnabled = [](QWidget* widget, bool enabled) {
        if (widget != nullptr) {
            widget->setEnabled(enabled);
        }
    };

    setModeEnabled(mixLocalInputSection_, true);
    setModeEnabled(mixInputMeterRow_, jamActive);
    setModeEnabled(mixSendRow_, true);
    setModeEnabled(mixSendMeterRow_, jamActive);
    setModeEnabled(mixMonitorEnableRow_, true);
    setModeEnabled(mixMonitorRow_, true);
    setModeEnabled(mixMonitorMeterRow_, jamActive);
    setModeEnabled(mixTrackSection_, true);
    setModeEnabled(mixTrackRow_, true);
    setModeEnabled(mixTrackMeterRow_, true);
    setModeEnabled(mixMetronomeSection_, true);
    setModeEnabled(mixMetronomeRow_, true);
    setModeEnabled(mixMetronomeMeterRow_, true);
    setModeEnabled(mixOutputSection_, jamActive);
    setModeEnabled(mixOutputMeterRow_, jamActive);
    setModeEnabled(mixRemotePeersSection_, true);
    setModeEnabled(mixRemotePeerRow_, true);
    const bool peerSelected = networkActive && selectedPeerId_ != 0;
    if (performanceLocalControls_) {
        performanceLocalControls_->setVisible(!peerSelected);
    }
    if (performancePeerControls_) {
        performancePeerControls_->setVisible(peerSelected);
    }
    for (QPushButton* button : {performanceAudioInputsButton_,
             performanceMidiInputsButton_, performancePluginsButton_,
             performancePluginBypassButton_}) {
        if (button) button->setVisible(!peerSelected);
    }
    if (performanceMasterOutputControls_) {
        performanceMasterOutputControls_->setVisible(true);
    }
    if (performanceLeftTitle_) {
        performanceLeftTitle_->setText(peerSelected
            ? QStringLiteral("PEER %1 · RECEIVE VOLUME")
                .arg(peerOrdinals_.value(selectedPeerId_, 0))
            : QStringLiteral("YOU / LOCAL INPUT"));
    }
    if (performanceRightTitle_) {
        performanceRightTitle_->setText(QStringLiteral("MASTER OUTPUT"));
    }
    if (!jamActive) {
        mixerStatsViewModel_.reset();
        if (mixInputMeter_) {
            mixInputMeter_->setLevel(0.0);
        }
        if (mixSendMeter_) {
            mixSendMeter_->setLevel(0.0);
        }
        if (mixMonitorMeter_) {
            mixMonitorMeter_->setLevel(0.0);
        }
        if (mixRemotePeerMeter_) {
            mixRemotePeerMeter_->setLevel(0.0);
        }
        if (mixOutputMeter_) {
            mixOutputMeter_->setLevel(0.0);
        }
        if (mixOutputClipLabel_) {
            mixOutputClipLabel_->setText(QStringLiteral("clip 0"));
        }
    }

    if (trackLevelSlider_ && trackLevelDbLabel_) {
        const QSignalBlocker blocker(trackLevelSlider_);
        trackLevelSlider_->setValue(qBound(-60, qRound(trackController_.model().trackGainDb), 12));
        trackLevelDbLabel_->setText(dbText(trackController_.model().trackGainDb));
    }
    if (metronomeLevelSlider_ && mixMetronomeLevelLabel_) {
        mixMetronomeLevelLabel_->setText(dbText(static_cast<double>(metronomeLevelSlider_->value())));
    }
    if (remoteLevelSlider_ && mixRemotePeerLevelLabel_) {
        mixRemotePeerLevelLabel_->setText(dbText(static_cast<double>(remoteLevelSlider_->value())));
    }
    if (mixSendLevelSlider_ && mixSendLevelLabel_) {
        mixSendLevelLabel_->setText(dbText(static_cast<double>(mixSendLevelSlider_->value())));
    }
    if (mixMonitorLevelSlider_ && mixMonitorLevelLabel_) {
        mixMonitorLevelLabel_->setText(dbText(static_cast<double>(mixMonitorLevelSlider_->value())));
    }
    if (masterOutputLevelSlider_ && masterOutputLevelLabel_) {
        masterOutputLevelLabel_->setText(
            dbText(static_cast<double>(masterOutputLevelSlider_->value())));
    }
}

void MainWindow::setMixRemotePeerVisible(bool visible)
{
    (void)visible;
    updateMixRemotePeers();
    updateTrackControls();
}

void MainWindow::updateMixRemotePeers()
{
    updatePerformancePeers();
    const bool visible = !operationalPeers_.isEmpty();
    if (mixRemotePeerRow_) {
        mixRemotePeerRow_->setVisible(visible);
        mixRemotePeerRow_->setEnabled(true);
    }
    if (mixRemotePeersSection_) {
        mixRemotePeersSection_->setVisible(visible);
        mixRemotePeersSection_->setEnabled(true);
    }
}

void MainWindow::startJamRecording()
{
    if (!jam2_.isRunning()) {
        return;
    }
    const QString defaultTake = jamStorage_.nextTakeName();
    QString requested = defaultTake;
    if (!automationSuppressDialogs_ && preferences_.recording.jam.promptForName) {
        bool accepted = false;
        requested = QInputDialog::getText(
            this,
            QStringLiteral("Record Jam"),
            QStringLiteral("Recording name"),
            QLineEdit::Normal,
            defaultTake,
            &accepted).trimmed();
        if (!accepted) return;
    }
    const QString folder = jamStorage_.uniqueTakeFolder(
        requested.isEmpty() ? defaultTake : requested);
    if (!QDir().mkpath(folder)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Record Jam"),
            QStringLiteral("Could not create the recording folder: %1").arg(folder));
        return;
    }
    if (!trackRecordingWorkflow_.startJamRecording(folder)) {
        appendLog(QStringLiteral("engine command queue unavailable: start jam recording"));
        return;
    }
    jamStorage_.markArtifactCreated();
    appendLog(QStringLiteral("record jam start: ") + folder);
    updateJamRecordingControls();
}

void MainWindow::stopJamRecording()
{
    if (!jam2_.isRunning()) {
        return;
    }
    if (!trackRecordingWorkflow_.stopJamRecording()) {
        appendLog(QStringLiteral("engine command queue unavailable: stop jam recording"));
        return;
    }
    appendLog(QStringLiteral("record jam stop"));
    updateJamRecordingControls();
}

void MainWindow::updateJamRecordingControls()
{
    if (performanceHome_) {
        performanceHome_->setJamRecordingState(
            jam2_.isRunning(),
            trackRecordingWorkflow_.jamRecordingActive(),
            QFileInfo(trackRecordingWorkflow_.jamRecordingFolder()).fileName());
    }
}

void MainWindow::showJamRecordingFinished(const QString& folder)
{
    const QString absoluteFolder = QDir(folder).absolutePath();
    if (automationSuppressDialogs_) {
        appendLog(QStringLiteral("recording completion dialog suppressed for automation: ") +
            absoluteFolder);
        return;
    }
    if (preferences_.recording.jam.completionAction == QStringLiteral("import")) {
        showJamRecordingImportDialog(absoluteFolder);
        return;
    }
    QMessageBox message(this);
    message.setIcon(QMessageBox::Information);
    message.setWindowTitle(QStringLiteral("Jam Recording Saved"));
    message.setText(QStringLiteral("The jam recording has finished."));
    message.setInformativeText(QStringLiteral("Files were saved here:\n%1")
        .arg(QDir::toNativeSeparators(absoluteFolder)));
    message.setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QPushButton* importButton = nullptr;
    if (preferences_.recording.jam.completionAction != QStringLiteral("notify")) {
        importButton = message.addButton(
            QStringLiteral("Import to Section"), QMessageBox::ActionRole);
    }
    QPushButton* closeButton = message.addButton(QMessageBox::Close);
    message.setDefaultButton(closeButton);
    message.exec();
    if (importButton && message.clickedButton() == importButton) {
        showJamRecordingImportDialog(absoluteFolder);
    }
}

void MainWindow::showJamRecordingImportDialog(const QString& folder)
{
    struct StemChoice {
        QString fileName;
        QString label;
        QString laneSuffix;
        bool selectedByDefault = false;
        QCheckBox* check = nullptr;
    };

    QVector<StemChoice> choices{
        {QStringLiteral("mix.wav"),
            QStringLiteral("Mix (what you heard)"),
            QStringLiteral("Mix"), preferences_.recording.jam.importMix},
        {QStringLiteral("my-input.wav"),
            QStringLiteral("My Input"),
            QStringLiteral("My Input"), preferences_.recording.jam.importMyInput},
        {QStringLiteral("their-input.wav"),
            QStringLiteral("Their Input"),
            QStringLiteral("Their Input"), preferences_.recording.jam.importTheirInput},
        {QStringLiteral("inputs-mix.wav"),
            QStringLiteral("Inputs Mix"),
            QStringLiteral("Inputs Mix"), preferences_.recording.jam.importInputsMix},
        {QStringLiteral("metronome.wav"),
            QStringLiteral("Metronome"),
            QStringLiteral("Metronome"), preferences_.recording.jam.importMetronome},
    };
    for (auto it = choices.begin(); it != choices.end();) {
        if (!QFileInfo::exists(QDir(folder).absoluteFilePath(it->fileName))) {
            it = choices.erase(it);
        } else {
            ++it;
        }
    }
    if (choices.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Import Jam Recording"),
            QStringLiteral("No completed recording WAVs were found in:\n%1")
                .arg(QDir::toNativeSeparators(folder)));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Import Jam Recording"));
    dialog.setMinimumWidth(520);
    auto* form = new QFormLayout(&dialog);

    auto* path = new QLineEdit(QDir::toNativeSeparators(folder), &dialog);
    path->setReadOnly(true);
    path->setCursorPosition(0);
    form->addRow(QStringLiteral("Recording"), path);

    auto* bankBox = new QComboBox(&dialog);
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        QString sectionName = bank < chordModel_.sections().size()
            ? chordModel_.section(bank).name.trimmed() : QString{};
        if (sectionName.isEmpty()) {
            sectionName = QStringLiteral("Section %1")
                .arg(QChar(QLatin1Char('A').unicode() + bank));
        }
        bankBox->addItem(
            QStringLiteral("Section %1 — %2")
                .arg(QChar(QLatin1Char('A').unicode() + bank), sectionName),
            bank);
    }
    bankBox->setCurrentIndex(qBound(
        0, looperProject_.activeBankIndex(), bankBox->count() - 1));
    form->addRow(QStringLiteral("Destination"), bankBox);

    auto* trackContainer = new QWidget(&dialog);
    auto* trackLayout = new QVBoxLayout(trackContainer);
    trackLayout->setContentsMargins(0, 0, 0, 0);
    trackLayout->setSpacing(4);
    for (StemChoice& choice : choices) {
        choice.check = new QCheckBox(
            QStringLiteral("%1  (%2)").arg(choice.label, choice.fileName),
            trackContainer);
        choice.check->setChecked(choice.selectedByDefault);
        trackLayout->addWidget(choice.check);
    }
    form->addRow(QStringLiteral("Tracks"), trackContainer);

    auto* availability = new QLabel(&dialog);
    availability->setWordWrap(true);
    form->addRow(QString(), availability);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton* importButton = buttons->addButton(
        QStringLiteral("Import to Section"), QDialogButtonBox::AcceptRole);
    form->addRow(buttons);

    const auto updateAvailability = [this, bankBox, availability, importButton, &choices] {
        int selected = 0;
        for (const StemChoice& choice : choices) {
            if (choice.check && choice.check->isChecked()) ++selected;
        }
        const int bank = bankBox->currentData().toInt();
        const int used = bank >= 0 && bank < looperProject_.banks().size()
            ? looperProject_.banks().at(bank).lanes.size() : 0;
        const int remaining = qMax(
            0, jam2::application::limits::kMaximumLooperLanesPerBank - used);
        importButton->setEnabled(selected > 0 && selected <= remaining);
        availability->setText(selected > remaining
            ? QStringLiteral("This section has space for %1 more track(s); select fewer recording tracks.")
                .arg(remaining)
            : QStringLiteral("Each selected WAV will be added as a separate track. %1 slot(s) available.")
                .arg(remaining));
        availability->setStyleSheet(selected > remaining
            ? QStringLiteral("color:#d46b6b;") : QString{});
    };
    for (const StemChoice& choice : std::as_const(choices)) {
        QObject::connect(
            choice.check, &QCheckBox::toggled, &dialog,
            [updateAvailability](bool) { updateAvailability(); });
    }
    QObject::connect(
        bankBox, &QComboBox::currentIndexChanged, &dialog,
        [updateAvailability](int) { updateAvailability(); });
    QObject::connect(importButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateAvailability();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    struct SelectedStem {
        QString sourcePath;
        QString laneName;
    };
    const QString takeName = QFileInfo(folder).fileName();
    QVector<SelectedStem> selected;
    for (const StemChoice& choice : std::as_const(choices)) {
        if (choice.check && choice.check->isChecked()) {
            selected.push_back({
                QDir(folder).absoluteFilePath(choice.fileName),
                QStringLiteral("%1 — %2").arg(takeName, choice.laneSuffix),
            });
        }
    }
    if (selected.isEmpty()) {
        return;
    }

    struct ImportResult {
        QString laneName;
        StagedPcm16Asset asset;
    };
    const int bankIndex = bankBox->currentData().toInt();
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = activeTrackSampleRate();
    auto results = std::make_shared<QVector<ImportResult>>();
    const bool started = startFileWorkerTask(
        [selected, stagingFolder, expectedSampleRate, results] {
            results->reserve(selected.size());
            for (const SelectedStem& stem : selected) {
                results->push_back({
                    stem.laneName,
                    stagePcm16Asset(
                        stem.sourcePath, stagingFolder, expectedSampleRate),
                });
            }
        },
        [this, bankIndex, results] {
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
                return;
            }
            int imported = 0;
            QStringList failures;
            for (const ImportResult& result : std::as_const(*results)) {
                if (!result.asset.error.isEmpty()) {
                    failures.append(QStringLiteral("%1: %2")
                        .arg(QFileInfo(result.asset.sourcePath).fileName(), result.asset.error));
                    continue;
                }
                registerTransientTrackWav(result.asset.stagedPath);
                looperWaveformCache_.remove(result.asset.stagedPath);
                LooperLane lane;
                lane.assetPath = result.asset.stagedPath;
                lane.assetHash = result.asset.sha256;
                lane.name = result.laneName;
                lane.sampleRate = result.asset.metadata.sampleRate;
                lane.sampleRateCompatible = true;
                lane.sourceFrames = result.asset.metadata.frames;
                lane.originKind = QStringLiteral("recorded");
                if (!looperProject_.appendLane(bankIndex, std::move(lane))) {
            failures.append(QStringLiteral("%1: no track slot was available")
                        .arg(result.laneName));
                    continue;
                }
                ++imported;
            }
            appendLog(QStringLiteral(
                "jam recording import completed: bank=%1 imported=%2 requested=%3")
                .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
                .arg(imported)
                .arg(results->size()));
            refreshLooperLanes();
            if (imported > 0) {
                regeneratePreparedMix(bankIndex);
                syncLooperArrangement();
                if (automaticWavSharingEnabled() && jam2_.isNetworkRunning()) {
                    shareLocalTracks();
                }
            }
            if (!failures.isEmpty()) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Import Jam Recording"),
                    QStringLiteral("Some recording tracks were not imported:\n\n") +
                        failures.join(QStringLiteral("\n")));
            }
        });
    if (!started) {
        QMessageBox::warning(
            this,
            QStringLiteral("Import Jam Recording"),
            QStringLiteral("The recording import could not be queued."));
    }
}

QString MainWindow::meshInviteUrl() const
{
    if (!activePublicEndpoint_.isEmpty()) {
        jam2::SessionInfo info;
        info.endpoint = jam2::parse_endpoint(activePublicEndpoint_.toStdString());
        info.session_id = sessionId_;
        info.key = sessionKey_;
        return QString::fromStdString(jam2::make_jam_url(info));
    }
    const CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    const QString host = !configuration.publicHost.trimmed().isEmpty()
        ? configuration.publicHost.trimmed()
        : (!configuration.bindHost.trimmed().isEmpty()
            ? configuration.bindHost.trimmed()
            : QStringLiteral("127.0.0.1"));
    jam2::SessionInfo info;
    info.endpoint = {
        host.toStdString(), static_cast<std::uint16_t>(configuration.port)};
    info.session_id = sessionId_;
    info.key = sessionKey_;
    return QString::fromStdString(jam2::make_jam_url(info));
}

void MainWindow::showPendingMeshInviteUrl()
{
    if (!pendingMeshInvitePopup_ || !sessionController_.isServer()) {
        return;
    }
    pendingMeshInvitePopup_ = false;
    const QString inviteUrl = meshInviteUrl();
    appendLog(QStringLiteral("jam ready; invite URL copied to clipboard"));
    if (automationSuppressDialogs_) {
        return;
    }
    showJamReadyInviteDialog(this, inviteUrl);
}

void MainWindow::setSessionHeaderStatus(
    const QString& text,
    const QString& title,
    const QStringList& lines,
    bool issue,
    bool actionable)
{
    if (!connectionLabel_) return;
    connectionLabel_->setText(text);
    connectionLabel_->setProperty("issue", issue);
    connectionLabel_->setProperty("jamtaster", QString());
    connectionLabel_->setCursor(
        issue || actionable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QString tooltip = QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());
    for (const QString& line : lines) {
        if (!line.trimmed().isEmpty()) {
            tooltip += QStringLiteral("<br>%1").arg(line.toHtmlEscaped());
        }
    }
    connectionLabel_->setToolTip(tooltip);
    connectionLabel_->style()->unpolish(connectionLabel_);
    connectionLabel_->style()->polish(connectionLabel_);
}

void MainWindow::showLocalSessionHeaderStatus()
{
    if (jamTaster_ && jamTaster_->taskActive()) {
        showJamTasterSessionHeaderStatus();
        return;
    }
    if (leaveJamButton_) leaveJamButton_->setEnabled(false);
    QString deviceName = preferences_.localAudio.name.trimmed();
    bool deviceOk = false;
    const int selectedId = selectedDeviceId().toInt(&deviceOk);
    if (deviceOk) {
        const auto device = std::find_if(
            availableDevices_.cbegin(), availableDevices_.cend(),
            [selectedId](const auto& item) { return item.id == selectedId; });
        if (device != availableDevices_.cend()) {
            deviceName = QString::fromStdString(device->name);
        }
    }
    if (deviceName.size() > 56) deviceName = deviceName.left(53) + QStringLiteral("...");
    const int sampleRate = jam2_.engineSnapshot().sample_rate > 0.0
        ? qRound(jam2_.engineSnapshot().sample_rate)
        : sessionRuntimeDraft_.configuration.sampleRate;
    const int buffer = sessionRuntimeDraft_.configuration.tuning.bufferSize;
    QStringList lines;
    if (!deviceName.isEmpty()) lines << deviceName;
    lines << QStringLiteral("%1 Hz  ·  %2-frame buffer").arg(sampleRate).arg(buffer);
    setSessionHeaderStatus(
        QStringLiteral("LOCAL"), QStringLiteral("Local performance"), lines);
}

void MainWindow::showAudioOffSessionHeaderStatus()
{
    if (jamTaster_ && jamTaster_->taskActive()) {
        showJamTasterSessionHeaderStatus();
        return;
    }
    if (leaveJamButton_) leaveJamButton_->setEnabled(false);
    setSessionHeaderStatus(
        QStringLiteral("AUDIO OFF"),
        QStringLiteral("Audio engine is off"),
        {QStringLiteral("Click to open Local Audio settings.")},
        false,
        true);
}

void MainWindow::showJamTasterSessionHeaderStatus()
{
    if (!jamTaster_ || !jamTaster_->taskActive()) return;
    const int percent = jamTaster_->taskProgress();
    const QString pill = percent > 0
        ? QStringLiteral("JAMTASTER %1%").arg(percent)
        : QStringLiteral("JAMTASTER");
    setSessionHeaderStatus(
        pill,
        QStringLiteral("JamTaster analysis"),
        {jamTaster_->taskStatusText(), QStringLiteral("Click to view or cancel the task.")},
        false,
        true);
    connectionLabel_->setProperty("jamtaster", QStringLiteral("running"));
    connectionLabel_->style()->unpolish(connectionLabel_);
    connectionLabel_->style()->polish(connectionLabel_);
}

void MainWindow::restoreSessionHeaderStatus()
{
    updateJamSessionHeaderStatus(sessionController_.snapshot());
}

void MainWindow::updateJamSessionHeaderStatus(
    const SharedSessionController::Snapshot& snapshot)
{
    if (jamTaster_ && jamTaster_->taskActive()) {
        showJamTasterSessionHeaderStatus();
        return;
    }
    const bool activeJam = snapshot.role == SharedSessionController::Role::Creator ||
        snapshot.role == SharedSessionController::Role::Joiner;
    if (leaveJamButton_) leaveJamButton_->setEnabled(activeJam);
    if (!activeJam) {
        if (jam2_.isRunning()) showLocalSessionHeaderStatus();
        else showAudioOffSessionHeaderStatus();
        return;
    }
    int activeRemotePeers = 0;
    for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
        if (peer.token != snapshot.localToken &&
            peer.edgeState == SharedSessionController::EdgeState::Active) {
            ++activeRemotePeers;
        }
    }
    const bool creator = snapshot.role == SharedSessionController::Role::Creator;
    const QString role = creator
        ? QStringLiteral("Jam creator") : QStringLiteral("Jam participant");
    if (!creator && activeRemotePeers == 0) {
        setSessionHeaderStatus(
            QStringLiteral("JOINING"), role,
            {QStringLiteral("Waiting for an active peer connection.")});
        return;
    }
    const QString quality = snapshot.contract.audioFormat == QStringLiteral("pcm16-mono")
        ? QStringLiteral("16-bit PCM")
        : snapshot.contract.audioFormat == QStringLiteral("pcm24-mono")
            ? QStringLiteral("24-bit PCM") : QStringLiteral("PCM audio");
    QStringList lines{
        QStringLiteral("%1 people  ·  %2 active remote")
            .arg(activeRemotePeers + 1)
            .arg(activeRemotePeers),
    };
    if (!sessionHeaderRtt_.isEmpty()) {
        lines << QStringLiteral("Peer RTT %1").arg(sessionHeaderRtt_);
    }
    if (snapshot.contract.sampleRate > 0) {
        lines << QStringLiteral("%1  ·  %2 Hz")
            .arg(quality)
            .arg(snapshot.contract.sampleRate);
    }
    QString pill = QStringLiteral("JAM · WAITING");
    if (!creator || activeRemotePeers > 0) {
        const int totalPeers = activeRemotePeers + 1;
        pill = QStringLiteral("JAM · %1 %2")
            .arg(totalPeers)
            .arg(totalPeers == 1 ? QStringLiteral("PEER") : QStringLiteral("PEERS"));
        if (!sessionHeaderRtt_.isEmpty()) {
            pill += QStringLiteral(" · %1").arg(sessionHeaderRtt_);
        }
    }
    setSessionHeaderStatus(pill, role, lines);
}

void MainWindow::showLocalPerformSetup()
{
    if (jam2_.isRunning()) {
        return;
    }
    refreshDevices();
    QString preferredDeviceId;
    for (const auto& info : availableDevices_) {
        const QString stable = QString::fromStdString(info.clsid.empty() ? info.name : info.clsid);
        if (QString::fromStdString(info.backend) == preferences_.localAudio.backend &&
            stable == preferences_.localAudio.stableId) {
            preferredDeviceId = QString::number(info.id);
            break;
        }
    }
    if (preferredDeviceId.isEmpty()) {
        preferredDeviceId = sessionRuntimeDraft_.selectedDeviceId;
    }

    const jam2::gui::SessionAudioDeviceList devices = sessionDialogDevices(
        AudioDevicePreference{}, preferredDeviceId);
    jam2::gui::LocalEngineDialogState initial{
        devices.selectedId,
        preferences_.localAudio.sampleRate,
        preferences_.localAudio.bufferSize,
        preferences_.localAudio.inputChannels,
        preferences_.localAudio.outputChannels,
        false,
    };
    jam2::gui::LocalEngineDialogCallbacks callbacks;
    callbacks.testDevice = [this](
        QComboBox* device, QPushButton* button, QWidget* parent) {
        testDeviceSelection(device, button, parent);
    };
    jam2::gui::LocalEngineDialog dialog(
        initial, devices, std::move(callbacks), this);
    const auto centerDialog = [this, &dialog] {
        const QPoint center = frameGeometry().center();
        const QSize dialogFrame = dialog.frameGeometry().size();
        dialog.move(
            center.x() - dialogFrame.width() / 2,
            center.y() - dialogFrame.height() / 2);
    };
    dialog.show();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    centerDialog();
    QTimer::singleShot(50, &dialog, centerDialog);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const jam2::gui::LocalEngineDialogState state = dialog.state();
    if (state.selectedDeviceId.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Perform"),
            QStringLiteral("Select a low-latency audio device first."));
        return;
    }
    sessionRuntimeDraft_.selectedDeviceId = state.selectedDeviceId;
    sessionRuntimeDraft_.configuration.sampleRate = state.sampleRate;
    sessionRuntimeDraft_.configuration.tuning.bufferSize = state.bufferSize;
    sessionRuntimeDraft_.audio.inputChannels = state.inputChannels;
    sessionRuntimeDraft_.audio.outputChannels = state.outputChannels;
    if (state.saveDefaults) {
        preferences_.localAudio = sessionRuntimeDraft_.audio;
        preferences_.localAudio.sampleRate = state.sampleRate;
        preferences_.localAudio.bufferSize = state.bufferSize;
        jam2::gui::storeSelectedDevicePreference(
            preferences_.localAudio,
            sessionRuntimeDraft_.selectedDeviceId,
            availableDevices_);
        UserPreferencesStore::save(preferences_);
    }
    startLocalPerform();
}

void MainWindow::startLocalPerform()
{
    if (jam2_.isRunning()) {
        return;
    }
    QString permissionError;
    if (!jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Microphone Access"), permissionError);
        return;
    }
    setSessionHeaderStatus(
        QStringLiteral("STARTING"),
        QStringLiteral("Starting local audio"),
        {QStringLiteral("Preparing the selected device and channels.")});
    pendingJamRuntimeError_.clear();
    try {
        launchLocalRuntime(runtimeOptions());
    } catch (const std::exception& error) {
        const QString detail = pendingJamRuntimeError_.isEmpty()
            ? QString::fromUtf8(error.what())
            : pendingJamRuntimeError_;
        pendingJamRuntimeError_.clear();
        QMessageBox::warning(this, QStringLiteral("Local engine failed"), detail);
        showAudioOffSessionHeaderStatus();
        QTimer::singleShot(0, this, [this] {
            if (!shuttingDown_ && !jam2_.isRunning()) {
                showLocalPerformSetup();
            }
        });
    }
}

void MainWindow::resetTrackSyncSessionState()
{
    ++songAssetCheckRevision_;
    songAssetCheckRetryTimer_.stop();
    pendingTrackContributions_.clear();
    appliedTrackContributionIds_.clear();
    localTrackOffers_.clear();
    trackOfferAssetPaths_.clear();
    trackWorkspace_.outgoingTrackSharePendingPeers.clear();
    trackWorkspace_.outgoingTrackShareBatchHashes.clear();
    trackWorkspace_.outgoingTrackShareLastProgressMs.clear();
    trackWorkspace_.incomingTrackShareLastProgressMs.clear();
    trackWorkspace_.authoritativeTrackHistory.clear();
    trackWorkspace_.heldTrackShareSongSet = {};
    trackWorkspace_.heldTrackShareSongSourcePeerToken.clear();
    pendingTrackAssetSources_.clear();
    validatedTrackAssetHashes_.clear();
    trackWorkspace_.incomingAssetRetryAttempts.clear();
    trackWorkspace_.incomingAssetRetrySources.clear();
    trackWorkspace_.assetRequestStartTimeouts = 0;
    ++trackWorkspace_.incomingAssetRequestGeneration;
    incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
    incomingAssetHash_.clear();
    incomingAssetSourcePeerToken_.clear();
    pendingSongSet_ = {};
    pendingSongRevision_ = 0;
    trackWorkspace_.pendingSongBaseRevision = 0;
    pendingSongTrackRestart_ = false;
    pendingSongSourcePeerToken_.clear();
    pendingSongNeedsAuthoritativePublish_ = false;
    pendingLooperAssetHashes_.clear();
    deferredSongSetMessage_ = {};
    deferredSongSetSourcePeerToken_.clear();
    looperArrangementRevision_ = 0;
    lastAppliedHostArrangementRevision_ = 0;
}

void MainWindow::startJam(bool createSession)
{
    const SharedSessionController::Role activeRole = sessionController_.snapshot().role;
    if (activeRole == SharedSessionController::Role::Creator ||
        activeRole == SharedSessionController::Role::Joiner) {
        return;
    }
    QString permissionError;
    if (!jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Microphone Access"), permissionError);
        appendLog(permissionError);
        return;
    }
    setSessionHeaderStatus(
        createSession ? QStringLiteral("STARTING JAM") : QStringLiteral("JOINING"),
        createSession ? QStringLiteral("Starting a jam") : QStringLiteral("Joining a jam"),
        {QStringLiteral("Preparing authenticated control and direct audio connections.")});
    jamStartupPending_ = true;
    jamStartupCreating_ = createSession;
    if (createSession) {
        JamSyncPolicyState state(jamSyncPolicy_);
        state.prepareCreatorSession();
        jamSyncPolicy_ = state.policy();
    } else {
        // The creator's first authenticated policy snapshot must supersede any
        // local choices made before joining this jam.
        JamSyncPolicyState state(jamSyncPolicy_);
        state.prepareJoinerSession();
        jamSyncPolicy_ = state.policy();
    }
    pendingJamRuntimeError_.clear();
    lastJamFailureDialog_.clear();
    queuedJamFailureDetail_.clear();
    jamFailureDialogQueued_ = false;
    preAuthenticationDisconnectWindow_.invalidate();
    preAuthenticationDisconnectCount_ = 0;
    firewallGuidanceShown_ = false;
    cancelSharedBankLaunch(false, QStringLiteral("starting a new network session"));
    jam2_.setTrackSyncEnabled(jamSyncPolicy_.globalPlayback);
    jam2_.setRecordingSyncEnabled(syncedRecordingsEnabled());
    activePublicEndpoint_.clear();
    lastLoggedSessionSummary_.clear();
    localMeshPeerTokens_.clear();

    resetTrackSyncSessionState();
    activeRecordingGroupId_.clear();
    activeRecordingGroupParticipants_.clear();
    recoveredRecordingGroupIds_.clear();
    activeRecordingGroupStartMessage_ = {};
    lastRecordingGroupFinishMessage_ = {};
    deferredRecordingControls_.clear();
    deferredRecordingControlsOverflowed_ = false;
    prepareNetworkRuntimePresentation(createSession);
    try {
        const CreatePreference& configuration =
            sessionRuntimeDraft_.configuration;
        if (createSession) {
            meshPeerEndpoints_.clear();
            meshPeerEndpoints_[meshPeerToken()] = localMeshEndpoint(true);
            if (!sessionController_.startCreator(SharedSessionController::CreatorConfig{
                    static_cast<quint16>(configuration.port),
                    sessionHex(),
                    keyHex(),
                    meshPeerToken(),
                    localMeshEndpoint(true),
                    configuration.maxPeers,
                    SharedSessionController::SessionContract{
                        jam2::protocol::kProtocolVersion,
                        configuration.audioFormat,
                        configuration.tuning.profile,
                        configuration.sampleRate,
                        configuration.tuning.frameSize,
                    }})) {
                const QString error = QStringLiteral("control server failed: ") + sessionController_.errorString();
                appendLog(error);
                showJamFailure(error);
                stopJam(true);
                return;
            }
            pendingMeshInvitePopup_ = true;
        } else {
            const std::string url =
                sessionRuntimeDraft_.inviteUrl.toStdString();
            const jam2::SessionInfo info = jam2::parse_jam_url(url);
            sessionId_ = info.session_id;
            sessionKey_ = info.key;
            meshPeerEndpoints_.clear();
            meshPeerEndpoints_[meshPeerToken()] = localMeshEndpoint(false);
            if (!sessionController_.startJoiner(SharedSessionController::JoinerConfig{
                QString::fromStdString(info.endpoint.host),
                info.endpoint.port,
                sessionHex(),
                keyHex(),
                meshPeerToken(),
                localMeshEndpoint(false),
                {},
                false,
            })) {
                showJamFailure(sessionController_.snapshot().failureDetail.isEmpty()
                    ? QStringLiteral("The join connection could not be started.")
                    : sessionController_.snapshot().failureDetail);
                stopJam(true);
                return;
            }
        }
    } catch (const std::exception& error) {
        showJamFailure(QString::fromUtf8(error.what()));
        stopJam(true);
        return;
    }

    sessionController_.setReconnectEnabled(true);
    if (!createSession) {
        appendLog(QStringLiteral("waiting for the session contract and membership before network attachment"));
        return;
    }
}

bool MainWindow::startAutomationJam(
    bool createSession,
    int localPort,
    const QString& inviteUrl,
    Jam2TestInputMode testInput,
    QString& error)
{
    error.clear();
    if (localPort < 1024 || localPort > 65535) {
        error = QStringLiteral("automation jam port is outside its bound");
        return false;
    }
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner) {
        error = QStringLiteral("automation jam is already active");
        return false;
    }
    if (!createSession && inviteUrl.trimmed().isEmpty()) {
        error = QStringLiteral("automation join requires an invite URL");
        return false;
    }

    automationHeadlessAudio_ = true;
    automationTestInput_ = testInput;
    automationSuppressDialogs_ = true;
    CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    configuration.bindHost = QStringLiteral("127.0.0.1");
    configuration.publicHost = QStringLiteral("127.0.0.1");
    configuration.port = localPort;
    configuration.noStun = true;
    configuration.maxPeers = 4;
    configuration.runtime.diagnostics = false;
    configuration.runtime.waitMs = 15000;
    configuration.runtime.streamMs = 0;
    if (!createSession) sessionRuntimeDraft_.inviteUrl = inviteUrl.trimmed();

    startJam(createSession);
    pendingMeshInvitePopup_ = false;
    const SharedSessionController::Role startedRole = sessionController_.snapshot().role;
    if (startedRole != (createSession
            ? SharedSessionController::Role::Creator
            : SharedSessionController::Role::Joiner)) {
        error = sessionController_.snapshot().failureDetail;
        if (error.isEmpty()) error = QStringLiteral("automation jam did not enter its requested role");
        return false;
    }
    return true;
}

bool MainWindow::prepareAutomationDialogJam(
    Jam2TestInputMode testInput,
    QString& error)
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner) {
        error = QStringLiteral("automation jam is already active");
        return false;
    }
    automationHeadlessAudio_ = true;
    automationTestInput_ = testInput;
    automationSuppressDialogs_ = true;
    QString loopbackError;
    if (!loopbackRecorder_.setCaptureBackendForTesting(
            [](const GuiLoopbackOptions& options,
               const std::atomic<bool>& stopRequested) {
                if (stopRequested.load(std::memory_order_acquire)) {
                    return GuiLoopbackCaptureResult{
                        false,
                        QStringLiteral("automation loopback capture stopped"),
                        QStringLiteral("fake loopback stop requested before capture")};
                }
                std::array<std::int16_t, 64> samples{};
                for (std::size_t index = 0; index < samples.size(); ++index) {
                    samples[index] = (index % 16U) < 8U ? 4096 : -4096;
                }
                jam2::gui::write_loopback_wav_pcm16(
                    options.outputPath, options.targetSampleRate, samples);
                return GuiLoopbackCaptureResult{
                    true, {}, QStringLiteral("fake loopback frames=64")};
            },
            &loopbackError)) {
        error = loopbackError;
        return false;
    }
    refreshDevices();
    error.clear();
    return true;
}

QJsonObject MainWindow::automationJamSnapshot() const
{
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    const std::optional<Jam2NetworkOperationalSnapshot> network = jam2_.networkSnapshot();
    QString role = QStringLiteral("inactive");
    if (session.role == SharedSessionController::Role::Creator) role = QStringLiteral("creator");
    else if (session.role == SharedSessionController::Role::Joiner) role = QStringLiteral("joiner");
    int activeRemotePeers = 0;
    for (const SharedSessionController::PeerSnapshot& peer : session.peers) {
        if (peer.token != session.localToken &&
            peer.edgeState == SharedSessionController::EdgeState::Active) {
            ++activeRemotePeers;
        }
    }
    QJsonObject policy = jam2JamSyncPolicyMessage(
        QStringLiteral("jam.sync.set"), jamSyncPolicy_);
    policy.remove(QStringLiteral("type"));
    QJsonObject snapshot{
        {QStringLiteral("role"), role},
        {QStringLiteral("remote_peer_count"), session.remotePeerCount},
        {QStringLiteral("active_remote_peer_count"), activeRemotePeers},
        {QStringLiteral("network_attachment_ready"), session.networkAttachmentReady},
        {QStringLiteral("network_running"), jam2_.isNetworkRunning()},
        {QStringLiteral("transport_grid_ready"),
            network.has_value() && network->transport_grid_ready},
        {QStringLiteral("grid_mapping_error_frames"), network.has_value()
            ? static_cast<qint64>(network->grid_mapping_error_frames)
            : 0LL},
        {QStringLiteral("local_token"), session.localToken},
        {QStringLiteral("coordinator_token"), session.coordinatorToken},
        {QStringLiteral("failure"), session.failureDetail},
        {QStringLiteral("last_startup_failure"), lastJamFailureDialog_},
        {QStringLiteral("policy"), policy},
    };
    if (role == QStringLiteral("creator")) {
        snapshot.insert(QStringLiteral("invite_url"), meshInviteUrl());
    }
    return snapshot;
}

QJsonObject MainWindow::automationContentSnapshot() const
{
    const QJsonObject model = chordModel_.toJson();
    const QJsonObject looper = looperProject_.toJson(true);
    QJsonObject song = model;
    song.insert(QStringLiteral("looper"), looper);
    const QByteArray encoded = QJsonDocument(song).toJson(QJsonDocument::Compact);
    const QByteArray digest = QCryptographicHash::hash(
        encoded, QCryptographicHash::Sha256).toHex();
    const QByteArray modelDigest = QCryptographicHash::hash(
        QJsonDocument(model).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    const QByteArray looperDigest = QCryptographicHash::hash(
        QJsonDocument(looper).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    QJsonObject firstSection;
    if (!chordModel_.sections().isEmpty()) {
        const SongSection& section = chordModel_.sections().first();
        const auto cell = [](const QVector<QString>& values) {
            return values.isEmpty() ? QString{} : values.first();
        };
        firstSection = {
            {QStringLiteral("id"), section.id},
            {QStringLiteral("label"), section.label},
            {QStringLiteral("name"), section.name},
            {QStringLiteral("beats"), section.beats},
            {QStringLiteral("chord_0"), cell(section.chords)},
            {QStringLiteral("target_0"), cell(section.targets)},
            {QStringLiteral("beat_0"), cell(section.beatNotes)},
            {QStringLiteral("lyric_0"), cell(section.lyrics)},
            {QStringLiteral("generated_kind"), section.generatedKind},
            {QStringLiteral("chord_fingerprint"),
                jam2::practice::generatedChordFingerprint(section)},
            {QStringLiteral("beat_fingerprint"),
                jam2::practice::generatedBeatFingerprint(section)},
        };
    }
    int laneCount = 0;
    QJsonArray assetHashes;
    QJsonArray assetAvailable;
    QJsonArray assetBytes;
    QJsonArray laneIds;
    QJsonArray lanePaths;
    QJsonArray laneNames;
    QJsonArray laneBanks;
    for (const LooperBank& bank : looperProject_.banks()) {
        laneCount += bank.lanes.size();
        for (const LooperLane& lane : bank.lanes) {
            assetHashes.append(lane.assetHash.toLower());
            const QFileInfo assetFile(looperAssetAbsolutePath(lane));
            assetAvailable.append(!lane.assetHash.isEmpty() && assetFile.isFile());
            assetBytes.append(assetFile.isFile() ? assetFile.size() : 0);
            laneIds.append(lane.id);
            lanePaths.append(lane.assetPath);
            laneNames.append(lane.name);
            laneBanks.append(bank.id);
        }
    }
    QJsonArray sectionIds;
    for (const SongSection& section : chordModel_.sections()) sectionIds.append(section.id);
    const int activeBankIndex = looperProject_.activeBankIndex();
    QJsonArray activeBankLaneIds;
    if (activeBankIndex >= 0 && activeBankIndex < looperProject_.banks().size()) {
        for (const LooperLane& lane : looperProject_.banks().at(activeBankIndex).lanes) {
            activeBankLaneIds.append(lane.id);
        }
    }
    QJsonArray arrangementSteps;
    for (const ArrangementStep& step : looperProject_.arrangement().steps) {
        arrangementSteps.append(QJsonObject{
            {QStringLiteral("bank"), step.bankIndex},
            {QStringLiteral("repeats"), step.repeats},
        });
    }
    const QJsonObject arrangementState{
        {QStringLiteral("steps"), arrangementSteps},
        {QStringLiteral("loop"), looperProject_.arrangement().loop},
        {QStringLiteral("enabled"), looperProject_.arrangement().enabled},
        {QStringLiteral("running"), arrangementRunning_},
        {QStringLiteral("armed"), arrangementArmed_},
        {QStringLiteral("step_index"), arrangementStepIndex_},
        {QStringLiteral("step_repeat"), arrangementStepRepeat_},
    };
    const auto& sharedBankLaunch = sharedBankLaunch_.snapshot();
    QJsonArray sharedBankExpectedPeers;
    for (const QString& token : sharedBankLaunch.expectedPeerTokens) {
        sharedBankExpectedPeers.append(token);
    }
    QJsonArray sharedBankReadyPeers;
    for (const QString& token : sharedBankLaunch.readyPeerTokens) {
        sharedBankReadyPeers.append(token);
    }
    QJsonArray pendingTrackContributions;
    for (auto it = pendingTrackContributions_.cbegin();
         it != pendingTrackContributions_.cend(); ++it) {
        pendingTrackContributions.append(QJsonObject{
            {QStringLiteral("key"), it.key()},
            {QStringLiteral("source"), it->sourcePeerToken},
            {QStringLiteral("batch"), it->batchId},
            {QStringLiteral("batch_size"), it->batchSize},
            {QStringLiteral("contribution"), it->contributionId},
            {QStringLiteral("bank"), it->bankIndex},
            {QStringLiteral("target_lane"), it->targetLaneId},
            {QStringLiteral("hash"), it->assetHash},
            {QStringLiteral("validated"),
                validatedTrackAssetHashes_.contains(it->assetHash)},
        });
    }
    QJsonArray outgoingTrackBatches;
    const qint64 outgoingTrackBatchSnapshotMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = trackWorkspace_.outgoingTrackSharePendingPeers.cbegin();
         it != trackWorkspace_.outgoingTrackSharePendingPeers.cend(); ++it) {
        const qint64 lastProgressMs =
            trackWorkspace_.outgoingTrackShareLastProgressMs.value(
                it.key(), outgoingTrackBatchSnapshotMs);
        outgoingTrackBatches.append(QJsonObject{
            {QStringLiteral("batch"), it.key()},
            {QStringLiteral("pending_peers"), it.value().size()},
            {QStringLiteral("hashes"),
                static_cast<int>(trackWorkspace_.outgoingTrackShareBatchHashes
                    .value(it.key()).size())},
            {QStringLiteral("idle_ms"), qMax<qint64>(
                0, outgoingTrackBatchSnapshotMs - lastProgressMs)},
        });
    }
    QJsonObject pendingTrackAssetSources;
    for (auto it = pendingTrackAssetSources_.cbegin();
         it != pendingTrackAssetSources_.cend(); ++it) {
        pendingTrackAssetSources.insert(it.key(), it.value());
    }
    QJsonObject incomingAssetRetrySources;
    for (auto it = trackWorkspace_.incomingAssetRetrySources.cbegin();
         it != trackWorkspace_.incomingAssetRetrySources.cend(); ++it) {
        incomingAssetRetrySources.insert(it.key(), it.value());
    }
    QJsonArray validatedTrackAssetHashes;
    for (const QString& hash : validatedTrackAssetHashes_) {
        validatedTrackAssetHashes.append(hash);
    }
    QJsonArray recordingPeerStates;
    for (auto it = peerTrackRecordingStates_.cbegin();
         it != peerTrackRecordingStates_.cend(); ++it) {
        recordingPeerStates.append(QJsonObject{
            {QStringLiteral("peer_token"), it.key()},
            {QStringLiteral("phase"), it->phase},
            {QStringLiteral("group_id"), it->groupId},
            {QStringLiteral("bank"), it->bank},
            {QStringLiteral("lane_id"), it->laneId},
            {QStringLiteral("count_in_remaining"), it->countInRemaining},
        });
    }
    const SharedTrackModel& sharedTrack = trackController_.model();
    const PreparedMixResult& activePreparedMix = preparedMixLifecycle_.active();
    const SharedTrackController::EffectiveLoop effectiveTrackLoop =
        trackController_.effectiveLoop(
            activePreparedMix.sampleRate,
            activePreparedMix.frames);
    return {
        {QStringLiteral("arrangement_revision"), static_cast<double>(
            sessionController_.snapshot().arrangementRevision)},
        {QStringLiteral("track_sync_local_arrangement_revision"),
            looperArrangementRevision_},
        {QStringLiteral("track_sync_last_applied_host_arrangement_revision"),
            lastAppliedHostArrangementRevision_},
        {QStringLiteral("model_revision"), chordModel_.revision()},
        {QStringLiteral("model_sha256"), QString::fromLatin1(digest)},
        {QStringLiteral("song_model_sha256"), QString::fromLatin1(modelDigest)},
        {QStringLiteral("looper_sha256"), QString::fromLatin1(looperDigest)},
        {QStringLiteral("encoded_bytes"), encoded.size()},
        {QStringLiteral("title"), chordModel_.title()},
        {QStringLiteral("title_view"), songTitleEdit_ ? songTitleEdit_->text() : QString{}},
        {QStringLiteral("storage_saved"), jamStorage_.isSaved()},
        {QStringLiteral("storage_has_artifacts"), jamStorage_.hasArtifacts()},
        {QStringLiteral("storage_root"), jamStorage_.rootFolder()},
        {QStringLiteral("project_path"), jamStorage_.projectFilePath()},
        {QStringLiteral("project_folder"), projectPersistence_.projectFolder()},
        {QStringLiteral("unsaved_changes"), hasUnsavedProjectChanges()},
        {QStringLiteral("section_count"), chordModel_.sections().size()},
        {QStringLiteral("section_ids"), sectionIds},
        {QStringLiteral("arrangement"), arrangementState},
        {QStringLiteral("first_section"), firstSection},
        {QStringLiteral("active_bank"), activeBankIndex},
        {QStringLiteral("pending_bank"), pendingBankIndex_},
        {QStringLiteral("pending_bank_absolute_beat"),
            QString::number(pendingBankAbsoluteBeat_)},
        {QStringLiteral("shared_bank_launch_active"), sharedBankLaunch.active()},
        {QStringLiteral("shared_bank_switch_id"), sharedBankLaunch.switchId},
        {QStringLiteral("shared_bank_index"), sharedBankLaunch.bankIndex},
        {QStringLiteral("shared_bank_requested_target_beat"),
            QString::number(sharedBankLaunch.requestedTargetBeat)},
        {QStringLiteral("shared_bank_host_ready"), sharedBankLaunch.hostReady},
        {QStringLiteral("shared_bank_expected_peers"), sharedBankExpectedPeers},
        {QStringLiteral("shared_bank_ready_peers"), sharedBankReadyPeers},
        {QStringLiteral("active_bank_lane_count"), activeBankLaneIds.size()},
        {QStringLiteral("active_bank_lane_ids"), activeBankLaneIds},
        {QStringLiteral("bank_count"), looperProject_.banks().size()},
        {QStringLiteral("lane_count"), laneCount},
        {QStringLiteral("asset_hashes"), assetHashes},
        {QStringLiteral("asset_available"), assetAvailable},
        {QStringLiteral("asset_bytes"), assetBytes},
        {QStringLiteral("lane_ids"), laneIds},
        {QStringLiteral("lane_paths"), lanePaths},
        {QStringLiteral("lane_names"), laneNames},
        {QStringLiteral("lane_banks"), laneBanks},
        {QStringLiteral("track_file_name"), sharedTrack.fileName},
        {QStringLiteral("track_file_available"),
            !sharedTrack.filePath.isEmpty() && QFileInfo::exists(sharedTrack.filePath)},
        {QStringLiteral("track_file_bytes"), sharedTrack.fileBytes},
        {QStringLiteral("track_sample_rate"), sharedTrack.sampleRate},
        {QStringLiteral("track_duration_ms"), sharedTrack.durationMs},
        {QStringLiteral("track_loop_enabled"), sharedTrack.loopEnabled},
        {QStringLiteral("track_loop_start_seconds"), sharedTrack.loopStartSeconds},
        {QStringLiteral("track_loop_end_seconds"), sharedTrack.loopEndSeconds},
        {QStringLiteral("track_loop_effective_enabled"), effectiveTrackLoop.enabled},
        {QStringLiteral("track_loop_effective_start_frame"),
            effectiveTrackLoop.startFrame},
        {QStringLiteral("track_loop_effective_end_frame"), effectiveTrackLoop.endFrame},
        {QStringLiteral("prepared_mix_frames"), activePreparedMix.frames},
        {QStringLiteral("prepared_mix_sample_rate"), activePreparedMix.sampleRate},
        {QStringLiteral("prepared_mix_worker_running"),
            preparedMixLifecycle_.workerRunning()},
        {QStringLiteral("prepared_mix_worker_bank"),
            preparedMixLifecycle_.workerBankIndex()},
        {QStringLiteral("prepared_mix_revision"), static_cast<qint64>(
            preparedMixLifecycle_.revision())},
        {QStringLiteral("prepared_mix_requests"), static_cast<qint64>(
            preparedMixLifecycle_.requests())},
        {QStringLiteral("prepared_mix_coalesced"), static_cast<qint64>(
            preparedMixLifecycle_.coalesced())},
        {QStringLiteral("prepared_mix_failures"), static_cast<qint64>(
            preparedMixLifecycle_.failures())},
        {QStringLiteral("chord_view_section"), chordGrid_
            ? chordGrid_->selectedSectionIndex() : -1},
        {QStringLiteral("beat_view_section"), beatGrid_
            ? beatGrid_->selectedSectionIndex() : -1},
        {QStringLiteral("lyric_view_section"), lyricGrid_
            ? lyricGrid_->selectedSectionIndex() : -1},
        {QStringLiteral("pending_asset_count"), pendingLooperAssetHashes_.size()},
        {QStringLiteral("pending_track_contribution_count"),
            pendingTrackContributions_.size()},
        {QStringLiteral("pending_track_contributions"), pendingTrackContributions},
        {QStringLiteral("outgoing_track_batch_count"),
            trackWorkspace_.outgoingTrackSharePendingPeers.size()},
        {QStringLiteral("outgoing_track_batches"), outgoingTrackBatches},
        {QStringLiteral("incoming_asset_active"),
            incomingAssetWorkflow_ != IncomingAssetWorkflow::None},
        {QStringLiteral("incoming_asset_hash"), incomingAssetHash_},
        {QStringLiteral("incoming_asset_source"), incomingAssetSourcePeerToken_},
        {QStringLiteral("incoming_asset_retry_count"),
             trackWorkspace_.incomingAssetRetryAttempts.size()},
        {QStringLiteral("incoming_asset_retry_source_count"),
             trackWorkspace_.incomingAssetRetrySources.size()},
        {QStringLiteral("incoming_asset_retry_sources"), incomingAssetRetrySources},
        {QStringLiteral("asset_request_start_timeouts"), static_cast<qint64>(
             trackWorkspace_.assetRequestStartTimeouts)},
        {QStringLiteral("incoming_asset_request_generation"), static_cast<qint64>(
             trackWorkspace_.incomingAssetRequestGeneration)},
        {QStringLiteral("pending_track_asset_sources"), pendingTrackAssetSources},
        {QStringLiteral("validated_track_asset_hashes"), validatedTrackAssetHashes},
        {QStringLiteral("lane_recording_isolation_active"),
            laneRecordingIsolationActive()},
        {QStringLiteral("lane_recording_protected"), sharedRecordingProtected()},
        {QStringLiteral("lane_recording_local_phase"), localTrackRecordingPhase_},
        {QStringLiteral("lane_recording_active_group_id"), activeRecordingGroupId_},
        {QStringLiteral("lane_recording_group_participants"),
            QJsonArray::fromStringList(activeRecordingGroupParticipants_)},
        {QStringLiteral("lane_recording_peer_states"), recordingPeerStates},
        {QStringLiteral("lane_recording_import_retry_attempts"),
            recordedLaneImportRetryAttempts_},
        {QStringLiteral("lane_recording_import_busy_retries"), static_cast<qint64>(
            recordedLaneImportBusyRetries_)},
        {QStringLiteral("lane_recording_import_failures"), static_cast<qint64>(
            recordedLaneImportFailures_)},
        {QStringLiteral("lane_recording_import_status"), recordedLaneImportStatus_},
        {QStringLiteral("lane_recording_import_target_id"),
            recordedLaneImportTargetId_},
        {QStringLiteral("lane_recording_import_last_hash"),
            recordedLaneImportLastHash_},
        {QStringLiteral("loopback_recording_running"),
            loopbackRecorder_.isRunning()},
        {QStringLiteral("loopback_capture_path"),
            laneRecordingState_.outputPath},
        {QStringLiteral("loopback_capture_available"),
            !laneRecordingState_.outputPath.isEmpty() &&
                QFileInfo::exists(laneRecordingState_.outputPath)},
        {QStringLiteral("file_tasks_active"), fileWorkerTasksActive_},
        {QStringLiteral("transfer"), automationTransferSnapshot()},
    };
}

QJsonObject MainWindow::automationPerformanceSnapshot() const
{
    const jam2::EngineSnapshot engine = jam2_.engineSnapshot();
    const auto network = jam2_.networkSnapshot();
    const Jam2MetronomeCompensationSettings runtimeCompensation = network
        ? network->metronome_compensation
        : metronomeCompensationSettings();
    const auto testInputText = [this] {
        switch (automationTestInput_) {
        case Jam2TestInputMode::Tone440: return QStringLiteral("tone-440");
        case Jam2TestInputMode::Pulse1s: return QStringLiteral("pulse-1s");
        case Jam2TestInputMode::MetroPulse: return QStringLiteral("metro-pulse");
        case Jam2TestInputMode::Off: return QStringLiteral("off");
        case Jam2TestInputMode::Silence:
        default: return QStringLiteral("silence");
        }
    };
    const auto transportActionText = [&engine] {
        switch (engine.transport_action) {
        case jam2::EngineTransportAction::TrackPlay: return QStringLiteral("play");
        case jam2::EngineTransportAction::TrackStop: return QStringLiteral("stop");
        case jam2::EngineTransportAction::TrackRestart: return QStringLiteral("restart");
        case jam2::EngineTransportAction::RecordStart: return QStringLiteral("record-start");
        case jam2::EngineTransportAction::RecordStop: return QStringLiteral("record-stop");
        case jam2::EngineTransportAction::None:
        default: return QStringLiteral("none");
        }
    };
    const auto metronomeModeText = [&engine] {
        switch (engine.metronome_mode) {
        case jam2::EngineMetronomeMode::LeaderAudio:
            return QStringLiteral("leader-audio");
        case jam2::EngineMetronomeMode::ListenerCompensated:
            return QStringLiteral("listener-compensated");
        case jam2::EngineMetronomeMode::SharedGrid:
        default:
            return QStringLiteral("shared-grid");
        }
    };
    const jam2::metronome::PatternSnapshot pattern =
        jam2::metronome::sanitize(engine.metronome_pattern);
    QJsonObject inputSourceRouter;
    if (const auto* router = jam2_.inputSourceRouter()) {
        const jam2::audio::InputSourceRouterStats stats = router->stats();
        QJsonArray sourceSlots;
        for (std::size_t slot = 0;
             slot < jam2::audio::kMaximumInputSources; ++slot) {
            const jam2::audio::InputSourceSlotSnapshot source =
                router->slot_snapshot(slot);
            sourceSlots.append(QJsonObject{
                {QStringLiteral("slot"), static_cast<qint64>(slot)},
                {QStringLiteral("kind"),
                    source.kind == jam2::audio::InputSourceKind::MidiInstrument
                        ? QStringLiteral("midi-instrument")
                        : QStringLiteral("audio")},
                {QStringLiteral("first_channel"),
                    source.first_channel == jam2::audio::kNoInputChannel
                        ? static_cast<qint64>(-1)
                        : static_cast<qint64>(source.first_channel)},
                {QStringLiteral("second_channel"),
                    source.second_channel == jam2::audio::kNoInputChannel
                        ? static_cast<qint64>(-1)
                        : static_cast<qint64>(source.second_channel)},
                {QStringLiteral("level_ppm"), source.level_ppm},
                {QStringLiteral("configured"), source.configured},
                {QStringLiteral("enabled"), source.enabled},
                {QStringLiteral("renderer_attached"), source.renderer_attached},
            });
        }
        inputSourceRouter = {
            {QStringLiteral("available"), true},
            {QStringLiteral("physical_channels"), static_cast<qint64>(
                router->physical_channels())},
            {QStringLiteral("configured_sources"), static_cast<qint64>(
                stats.configured_sources)},
            {QStringLiteral("rendered_blocks"), static_cast<qint64>(
                stats.rendered_blocks)},
            {QStringLiteral("renderer_failures"), static_cast<qint64>(
                stats.renderer_failures)},
            {QStringLiteral("invalid_configurations"), static_cast<qint64>(
                stats.invalid_configurations)},
            {QStringLiteral("peak_ppm"), stats.peak_ppm},
            {QStringLiteral("slots"), sourceSlots},
        };
    } else {
        inputSourceRouter = {{QStringLiteral("available"), false}};
    }
    QJsonArray midiInputSources;
    for (const auto& source : midiPluginSources_) {
        if (!source) continue;
        midiInputSources.push_back(QJsonObject{
            {QStringLiteral("device_id"), QString::fromStdString(source->deviceInfo.id)},
            {QStringLiteral("device_name"), QString::fromStdString(source->deviceInfo.name)},
            {QStringLiteral("mode"), source->mode == jam2::midi::InputMode::Mpe
                ? QStringLiteral("mpe") : QStringLiteral("standard")},
            {QStringLiteral("router_slot"), static_cast<qint64>(source->routerSlot)},
            {QStringLiteral("included"), source->included},
            {QStringLiteral("level_ppm"), source->levelPpm},
            {QStringLiteral("device_open"), source->device != nullptr},
            {QStringLiteral("plugin_loaded"), source->host != nullptr},
            {QStringLiteral("muted"), source->muted},
            {QStringLiteral("short_messages"), static_cast<qint64>(
                source->device ? source->device->short_messages() : 0)},
            {QStringLiteral("unsupported_messages"), static_cast<qint64>(
                source->device ? source->device->unsupported_messages() : 0)},
            {QStringLiteral("queue_depth"), static_cast<qint64>(source->events.depth())},
            {QStringLiteral("queue_high_water"), static_cast<qint64>(
                source->events.high_water())},
            {QStringLiteral("queue_dropped"), static_cast<qint64>(
                source->events.dropped())},
        });
    }
    const auto pluginStatsJson = [](const auto& host) {
        const jam2::application::InputPluginStats stats = host
            ? host->stats() : jam2::application::InputPluginStats{};
        return QJsonObject{
            {QStringLiteral("submitted_blocks"), static_cast<qint64>(stats.submittedBlocks)},
            {QStringLiteral("completed_blocks"), static_cast<qint64>(stats.completedBlocks)},
            {QStringLiteral("deadline_misses"), static_cast<qint64>(stats.deadlineMisses)},
            {QStringLiteral("deadline_concealments"), static_cast<qint64>(
                stats.deadlineConcealments)},
            {QStringLiteral("failed_blocks"), static_cast<qint64>(stats.failedBlocks)},
            {QStringLiteral("stale_responses"), static_cast<qint64>(stats.staleResponses)},
            {QStringLiteral("midi_dropped"), static_cast<qint64>(stats.midiDropped)},
            {QStringLiteral("midi_events_consumed"), static_cast<qint64>(
                stats.midiEventsConsumed)},
            {QStringLiteral("worker_latency_frames"), static_cast<qint64>(
                stats.workerLatencyFrames)},
            {QStringLiteral("input_channels"), static_cast<qint64>(
                stats.negotiatedInputChannels)},
            {QStringLiteral("output_channels"), static_cast<qint64>(
                stats.negotiatedOutputChannels)},
            {QStringLiteral("isolation_latency_frames"), static_cast<qint64>(
                stats.isolationLatencyFrames)},
            {QStringLiteral("midi_queue_depth"), static_cast<qint64>(stats.midiQueueDepth)},
            {QStringLiteral("midi_queue_high_water"), static_cast<qint64>(
                stats.midiQueueHighWater)},
        };
    };
    QJsonArray inputPlugins;
    for (std::size_t slot = 0; slot < audioPluginSources_.size(); ++slot) {
        const auto& source = audioPluginSources_[slot];
        if (source.firstChannel == jam2::audio::kNoInputChannel ||
            source.consumedByStereoGroup) {
            continue;
        }
        inputPlugins.push_back(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("audio")},
            {QStringLiteral("slot"), static_cast<qint64>(slot)},
            {QStringLiteral("loaded"), source.host != nullptr},
            {QStringLiteral("name"), source.name},
            {QStringLiteral("healthy"), source.host && source.host->healthy()},
            {QStringLiteral("editor_open"), source.host && source.host->editorOpen()},
            {QStringLiteral("bypassed"), source.bypassed},
            {QStringLiteral("stats"), pluginStatsJson(source.host)},
        });
    }
    for (const auto& source : midiPluginSources_) {
        if (!source) continue;
        inputPlugins.push_back(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("midi-instrument")},
            {QStringLiteral("slot"), static_cast<qint64>(source->routerSlot)},
            {QStringLiteral("loaded"), source->host != nullptr},
            {QStringLiteral("name"), source->pluginName},
            {QStringLiteral("healthy"), source->host && source->host->healthy()},
            {QStringLiteral("editor_open"), source->host && source->host->editorOpen()},
            {QStringLiteral("bypassed"), source->muted},
            {QStringLiteral("device_open"), source->device != nullptr},
            {QStringLiteral("stats"), pluginStatsJson(source->host)},
        });
    }
    QJsonArray jamRecordingStemSignalFrames;
    for (const std::uint64_t frames : engine.jam_recording.stem_signal_frames) {
        jamRecordingStemSignalFrames.append(static_cast<qint64>(frames));
    }
    return {
        {QStringLiteral("engine_running"), jam2_.isRunning()},
        {QStringLiteral("network_running"), jam2_.isNetworkRunning()},
        {QStringLiteral("headless_audio"),
            engine.backend == jam2::EngineAudioBackend::Headless},
        {QStringLiteral("test_input"), testInputText()},
        {QStringLiteral("engine_frame"), static_cast<qint64>(engine.engine_frame)},
        {QStringLiteral("callbacks"), static_cast<qint64>(engine.callbacks)},
        {QStringLiteral("sample_rate"), engine.sample_rate},
        {QStringLiteral("audio_buffer_frames"), static_cast<qint64>(engine.audio_buffer_frames)},
        {QStringLiteral("network_capture_enabled"), engine.network_capture_enabled},
        {QStringLiteral("network_capture_ready"), engine.network_capture_ready},
        {QStringLiteral("network_playback_enabled"), engine.network_playback_enabled},
        {QStringLiteral("capture_ring_depth_frames"), static_cast<qint64>(
            engine.capture_ring_depth_frames)},
        {QStringLiteral("playback_ring_depth_frames"), static_cast<qint64>(
            engine.playback_ring_depth_frames)},
        {QStringLiteral("input_peak_ppm"), engine.input_peak_ppm},
        {QStringLiteral("send_peak_ppm"), engine.send_peak_ppm},
        {QStringLiteral("remote_peak_ppm"), engine.remote_peak_ppm},
        {QStringLiteral("output_peak_ppm"), engine.output_peak_ppm},
        {QStringLiteral("metronome_peak_ppm"), engine.metronome_peak_ppm},
        {QStringLiteral("metronome_enabled"), engine.metronome_enabled},
        {QStringLiteral("metronome_transport_gated"), engine.metronome_transport_gated},
        {QStringLiteral("metronome_bpm"), pattern.bpm},
        {QStringLiteral("metronome_beats_per_bar"), pattern.beats_per_bar},
        {QStringLiteral("metronome_division"), pattern.division},
        {QStringLiteral("metronome_mode"), metronomeModeText()},
        {QStringLiteral("metronome_epoch_frame"), static_cast<qint64>(
            engine.metronome_epoch_frame)},
        {QStringLiteral("metronome_epoch_valid"), engine.metronome_epoch_valid},
        {QStringLiteral("metronome_compensation_runtime_available"),
            network.has_value()},
        {QStringLiteral("metronome_compensation_max_ms"),
            runtimeCompensation.maximum_ms},
        {QStringLiteral("metronome_compensation_smoothing_ms"),
            runtimeCompensation.smoothing_ms},
        {QStringLiteral("metronome_compensation_deadband_ms"),
            runtimeCompensation.deadband_ms},
        {QStringLiteral("metronome_compensation_slew_ms_per_sec"),
            runtimeCompensation.slew_ms_per_second},
        {QStringLiteral("transport_action"), transportActionText()},
        {QStringLiteral("transport_revision"), static_cast<qint64>(
            engine.transport_revision)},
        {QStringLiteral("transport_target_frame"), static_cast<qint64>(
            engine.transport_target_frame)},
        {QStringLiteral("transport_pending"), engine.transport_pending},
        {QStringLiteral("transport_commit_count"), static_cast<qint64>(
            engine.transport_commit_count)},
        {QStringLiteral("transport_playback_active"), engine.transport_playback_active},
        {QStringLiteral("global_transport_requested_playing"),
            trackRecordingWorkflow_.globalTransportRequestedPlaying()},
        {QStringLiteral("global_transport_playing"),
            trackRecordingWorkflow_.globalTransportPlaying()},
        {QStringLiteral("prepared_source_playing"), engine.prepared_source_playing},
        {QStringLiteral("prepared_source_frame"), static_cast<qint64>(
            engine.prepared_source_frame)},
        {QStringLiteral("prepared_source_scheduled_start_frame"),
            static_cast<qint64>(engine.prepared_source_scheduled_start_frame)},
        {QStringLiteral("prepared_source_actual_start_frame"),
            static_cast<qint64>(engine.prepared_source_actual_start_frame)},
        {QStringLiteral("prepared_source_underruns"), static_cast<qint64>(
            engine.prepared_source_underruns)},
        {QStringLiteral("prepared_source_busy_events"), static_cast<qint64>(
            engine.prepared_source_busy_events)},
        {QStringLiteral("metronome_button_text"), performanceMetronomeToggle_
            ? performanceMetronomeToggle_->text() : QString{}},
        {QStringLiteral("transport_button_text"), performanceTrackToggle_
            ? performanceTrackToggle_->text() : QString{}},
        {QStringLiteral("tempo_view_bpm"), metronomeBpmSpin_
            ? metronomeBpmSpin_->value() : 0},
        {QStringLiteral("jam_recording_active"), engine.jam_recording.active},
        {QStringLiteral("jam_recording_workflow_active"),
            trackRecordingWorkflow_.jamRecordingActive()},
        {QStringLiteral("jam_recording_folder"),
            trackRecordingWorkflow_.jamRecordingFolder()},
        {QStringLiteral("jam_recording_view_enabled"), performanceHome_
            ? performanceHome_->jamRecordingEnabled() : false},
        {QStringLiteral("jam_recording_view_active"), performanceHome_
            ? performanceHome_->jamRecordingActive() : false},
        {QStringLiteral("jam_recording_view_take"), performanceHome_
            ? performanceHome_->jamRecordingTake() : QString{}},
        {QStringLiteral("jam_recording_frames_written"), static_cast<qint64>(
            engine.jam_recording.frames_written)},
        {QStringLiteral("jam_recording_frames_queued"), static_cast<qint64>(
            engine.jam_recording.frames_queued)},
        {QStringLiteral("jam_recording_dropped_frames"), static_cast<qint64>(
            engine.jam_recording.dropped_frames)},
        {QStringLiteral("jam_recording_drop_events"), static_cast<qint64>(
            engine.jam_recording.drop_events)},
        {QStringLiteral("jam_recording_writer_errors"), static_cast<qint64>(
            engine.jam_recording.writer_errors)},
        {QStringLiteral("jam_recording_stem_signal_frames"),
            jamRecordingStemSignalFrames},
        {QStringLiteral("track_take_armed"), engine.track_take.armed},
        {QStringLiteral("track_take_recording"), engine.track_take.recording},
        {QStringLiteral("track_take_finalized"), engine.track_take.finalized},
        {QStringLiteral("recording_latency_adjustment_frames"),
            static_cast<qint64>(engine.recording_latency_adjustment_frames)},
        {QStringLiteral("input_source_router"), inputSourceRouter},
        {QStringLiteral("midi_enumeration_gate"), automationCompletionGateText(
            midiInputBackend_->automationCompletionGateState())},
        {QStringLiteral("input_plugin_load_gate"), automationCompletionGateText(
            inputPluginBackend_->automationCompletionGateState())},
        {QStringLiteral("midi_discovery_completions"), static_cast<qint64>(
            automationMidiDiscoveryCompletions_)},
        {QStringLiteral("input_plugin_load_completions"), static_cast<qint64>(
            automationInputPluginLoadCompletions_)},
        {QStringLiteral("midi_input_sources"), midiInputSources},
        {QStringLiteral("input_plugins"), inputPlugins},
    };
}

bool MainWindow::automationSetMetronome(bool enabled, int bpm, QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("metronome automation requires a running jam");
        return false;
    }
    if (bpm < 1 || bpm > 400 || !metronomeBpmSpin_ || !performanceMetronomeToggle_) {
        error = QStringLiteral("metronome automation controls are unavailable");
        return false;
    }
    metronomeBpmSpin_->setValue(bpm);
    if (metronomeTransport_.localRunning() != enabled) {
        performanceMetronomeToggle_->click();
    }
    return true;
}

bool MainWindow::automationSetGlobalPlayback(bool playing, QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("transport automation requires a running jam");
        return false;
    }
    if (!performanceTrackToggle_) {
        error = QStringLiteral("global transport control is unavailable");
        return false;
    }
    if (trackRecordingWorkflow_.globalTransportRequestedPlaying() != playing) {
        performanceTrackToggle_->click();
    }
    return true;
}

bool MainWindow::automationSetJamRecording(bool recording, QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("recording automation requires a running jam");
        return false;
    }
    if (!performanceHome_ || !performanceHome_->onJamRecordingToggle) {
        error = QStringLiteral("jam recording control is unavailable");
        return false;
    }
    if (trackRecordingWorkflow_.jamRecordingActive() != recording) {
        performanceHome_->onJamRecordingToggle();
    }
    if (trackRecordingWorkflow_.jamRecordingActive() != recording) {
        error = recording
            ? QStringLiteral("the real jam recording workflow did not start")
            : QStringLiteral("the real jam recording workflow did not stop");
        return false;
    }
    return true;
}

bool MainWindow::automationRenameSong(const QString& title, QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("song rename requires a running automation jam");
        return false;
    }
    if (!renameCurrentJam(title)) {
        error = QStringLiteral("the real song rename workflow rejected the title");
        return false;
    }
    return true;
}

bool MainWindow::automationEditSongCell(
    int section,
    const QString& lane,
    int beat,
    const QString& value,
    QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("song cell editing requires a running automation jam");
        return false;
    }
    if (section < 0 || section >= chordModel_.sections().size() ||
        beat < 0 || beat >= chordModel_.section(section).beats ||
        (lane != QStringLiteral("chord") && lane != QStringLiteral("target") &&
         lane != QStringLiteral("beat") && lane != QStringLiteral("lyric"))) {
        error = QStringLiteral("song cell target is outside the current model");
        return false;
    }
    const int before = chordModel_.revision();
    chordModel_.setCell(section, lane, beat, value);
    if (chordModel_.revision() == before) {
        error = QStringLiteral("song cell edit did not mutate the model");
        return false;
    }
    refreshSongView(lane);
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("beat.set")},
        {QStringLiteral("revision"), chordModel_.revision()},
        {QStringLiteral("section"), section},
        {QStringLiteral("lane"), lane},
        {QStringLiteral("beat"), beat},
        {QStringLiteral("text"), value},
    });
    return true;
}

bool MainWindow::automationResizeSongSection(int section, int beats, QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("song resizing requires a running automation jam");
        return false;
    }
    if (section < 0 || section >= chordModel_.sections().size() ||
        beats < jam2::application::limits::kMinimumBeatsPerSection ||
        beats > jam2::application::limits::kMaximumBeatsPerSection) {
        error = QStringLiteral("song resize target is outside the current model");
        return false;
    }
    chordModel_.resizeSection(section, beats);
    refreshSongViews();
    refreshLooperLanes();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("grid.resize")},
        {QStringLiteral("revision"), chordModel_.revision()},
        {QStringLiteral("section"), section},
        {QStringLiteral("lane"), QStringLiteral("chord")},
        {QStringLiteral("beats"), beats},
    });
    syncLooperArrangement();
    return true;
}

bool MainWindow::automationGenerateIdea(
    jam2::practice::PracticeIdeaParts parts,
    std::uint32_t seed,
    QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("idea generation requires a running automation jam");
        return false;
    }
    jam2::practice::ChordIdeaRequest request;
    request.key = 0;
    request.styleId = QStringLiteral("pop");
    request.parts = parts;
    request.targetSectionIndex = 0;
    request.bpm = 120;
    request.bars = 4;
    request.harmonicComplexity = 2;
    request.rhythmicComplexity = 2;
    jam2::practice::GeneratedPracticeIdea idea =
        jam2::practice::generateCoupledPracticeIdeaSeeded(request, seed);
    if (!applyPracticeIdea(std::move(idea), parts, 0, true, true)) {
        error = QStringLiteral("the real practice-idea workflow rejected generated content");
        return false;
    }
    return true;
}

bool MainWindow::automationImportWav(
    int laneIndex,
    const QString& sourcePath,
    QString& error)
{
    error.clear();
    if (!jam2_.isNetworkRunning()) {
        error = QStringLiteral("WAV import requires a running automation jam");
        return false;
    }
    if (!QFileInfo(sourcePath).isAbsolute()) {
        error = QStringLiteral("WAV import requires an absolute source path");
        return false;
    }
    if (!importWavIntoLooperLane(laneIndex, sourcePath)) {
        error = QStringLiteral("the real WAV import workflow rejected the request");
        return false;
    }
    return true;
}

bool MainWindow::automationShareTracks(QString& error)
{
    error.clear();
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (!jam2_.isNetworkRunning() || session.remotePeerCount <= 0) {
        error = QStringLiteral("Share Tracks requires an active automation jam with peers");
        return false;
    }
    shareLocalTracks(true);
    return true;
}

bool MainWindow::automationArmTransferPause(
    const QString& point,
    QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("transfer pauses are available only to the private GUI agent");
        return false;
    }
    if (point != QStringLiteral("offer")) {
        if (automationOfferPauseArmed_ || automationOfferPauseActive_) {
            error = QStringLiteral("a transfer pause is already armed or active");
            return false;
        }
        return assetTransfer_.armAutomationPause(point, error);
    }
    const QJsonObject transfer = assetTransfer_.automationSnapshot();
    if (automationOfferPauseArmed_ || automationOfferPauseActive_ ||
        !transfer.value(QStringLiteral("pause_armed")).toString().isEmpty() ||
        !transfer.value(QStringLiteral("pause_active")).toString().isEmpty()) {
        error = QStringLiteral("a transfer pause is already armed or active");
        return false;
    }
    automationOfferPauseArmed_ = true;
    ++automationOfferPauseGeneration_;
    error.clear();
    return true;
}

bool MainWindow::automationReleaseTransferPause(QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("transfer gates are available only to the private GUI agent");
        return false;
    }
    if (automationOfferPauseActive_) {
        automationOfferPauseActive_ = false;
        ++automationOfferPauseGeneration_;
        error.clear();
        applyPendingTrackContributions();
        requestNextPendingAsset();
        return true;
    }
    return assetTransfer_.releaseAutomationPause(error);
}

bool MainWindow::automationDropOutgoingAssetStarts(int count, QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("transfer faults are available only to the private GUI agent");
        return false;
    }
    if (automationOfferPauseArmed_ || automationOfferPauseActive_) {
        error = QStringLiteral("a transfer fault or pause is already armed or active");
        return false;
    }
    return assetTransfer_.armAutomationDropOutgoingStarts(count, error);
}

bool MainWindow::automationExpireAssetRequestStart(QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("transfer expiry is available only to the private GUI agent");
        return false;
    }
    if (incomingAssetWorkflow_ == IncomingAssetWorkflow::None ||
        incomingAssetHash_.isEmpty() || assetTransfer_.incomingTransferActive()) {
        error = QStringLiteral("no unstarted incoming asset request is active");
        return false;
    }
    const std::uint64_t generation = trackWorkspace_.incomingAssetRequestGeneration;
    const std::uint64_t timeoutsBefore = trackWorkspace_.assetRequestStartTimeouts;
    handleAssetRequestStartTimeout(
        incomingAssetWorkflow_,
        incomingAssetHash_,
        incomingAssetSourcePeerToken_,
        generation,
        0);
    if (trackWorkspace_.assetRequestStartTimeouts != timeoutsBefore + 1) {
        error = QStringLiteral("incoming asset request changed before explicit expiry");
        return false;
    }
    error.clear();
    return true;
}

bool MainWindow::automationHoldFileWorkers(QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("file-worker holds are available only to the private GUI agent");
        return false;
    }
    if (fileWorkerTasksActive_ != 0 || automationFileWorkerGate_) {
        error = QStringLiteral("file workers must be idle before arming a deterministic hold");
        return false;
    }
    auto gate = std::make_shared<AutomationFileWorkerGate>();
    automationFileWorkerGate_ = gate;
    for (int worker = 0; worker < 2; ++worker) {
        if (!startFileWorkerTask(
                [gate] {
                    std::unique_lock<std::mutex> lock(gate->mutex);
                    gate->releasedCondition.wait(lock, [&gate] { return gate->released; });
                },
                [] {})) {
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->released = true;
            }
            gate->releasedCondition.notify_all();
            automationFileWorkerGate_.reset();
            error = QStringLiteral("could not occupy both bounded file workers");
            return false;
        }
    }
    error.clear();
    return true;
}

bool MainWindow::automationReleaseFileWorkers(QString& error)
{
    const auto gate = automationFileWorkerGate_;
    if (!gate) {
        error = QStringLiteral("no file-worker gate is active");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->releasedCondition.notify_all();
    automationFileWorkerGate_.reset();
    error.clear();
    return true;
}

bool MainWindow::automationArmCompletionGate(
    const QString& target, QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("completion gates are available only to the private GUI agent");
        return false;
    }
    if (target == QStringLiteral("midi-enumeration")) {
        std::string nativeError;
        const bool armed = midiInputBackend_->armAutomationCompletionGate(nativeError);
        error = QString::fromStdString(nativeError);
        return armed;
    }
    if (target == QStringLiteral("input-plugin-load")) {
        return inputPluginBackend_->armAutomationCompletionGate(error);
    }
    error = QStringLiteral("unknown completion gate target");
    return false;
}

bool MainWindow::automationReleaseCompletionGate(
    const QString& target, QString& error)
{
    if (!automationSuppressDialogs_) {
        error = QStringLiteral("completion gates are available only to the private GUI agent");
        return false;
    }
    if (target == QStringLiteral("midi-enumeration")) {
        std::string nativeError;
        const bool released = midiInputBackend_->releaseAutomationCompletionGate(nativeError);
        error = QString::fromStdString(nativeError);
        return released;
    }
    if (target == QStringLiteral("input-plugin-load")) {
        return inputPluginBackend_->releaseAutomationCompletionGate(error);
    }
    error = QStringLiteral("unknown completion gate target");
    return false;
}

QJsonObject MainWindow::automationTransferSnapshot() const
{
    QJsonObject snapshot = assetTransfer_.automationSnapshot();
    if (automationOfferPauseArmed_) {
        snapshot.insert(QStringLiteral("pause_armed"), QStringLiteral("offer"));
    }
    if (automationOfferPauseActive_) {
        snapshot.insert(QStringLiteral("pause_active"), QStringLiteral("offer"));
    }
    return snapshot;
}

void MainWindow::clearAutomationTransferPause()
{
    ++automationOfferPauseGeneration_;
    automationOfferPauseArmed_ = false;
    automationOfferPauseActive_ = false;
    assetTransfer_.clearAutomationPause();
}

void MainWindow::showJamFailure(const QString& detail)
{
    const QString normalized = detail.trimmed().isEmpty()
        ? QStringLiteral("An unknown in-process network error occurred.")
        : detail.trimmed();
    if (!lastJamFailureDialog_.isEmpty()) {
        return;
    }
    if (queuedJamFailureDetail_.isEmpty() ||
        queuedJamFailureDetail_.contains(QStringLiteral("unknown"), Qt::CaseInsensitive)) {
        queuedJamFailureDetail_ = normalized;
    }
    if (jamFailureDialogQueued_) {
        return;
    }
    jamFailureDialogQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        jamFailureDialogQueued_ = false;
        if (!lastJamFailureDialog_.isEmpty() || shuttingDown_) {
            return;
        }
        lastJamFailureDialog_ = queuedJamFailureDetail_.isEmpty()
            ? QStringLiteral("An unknown in-process network error occurred.")
            : queuedJamFailureDetail_;
        pendingJamRuntimeError_.clear();
        jamStartupPending_ = false;
        const QString action = jamStartupCreating_
            ? QStringLiteral("Start Jam failed")
            : QStringLiteral("Join Jam failed");
        setSessionHeaderStatus(
            QStringLiteral("CONNECTION ISSUE"),
            action,
            {lastJamFailureDialog_.left(160)},
            true,
            true);
        if (!automationSuppressDialogs_) {
            QMessageBox::critical(this, action, lastJamFailureDialog_);
        }
    });
}

void MainWindow::launchLocalRuntime(Jam2RuntimeOptions options)
{
    controlRefreshAvailable_ = false;
    if (!sessionController_.startLocal(options)) {
        throw std::runtime_error("local engine failed to start");
    }
    appendLog(QStringLiteral("local engine active through typed runtime"));
    startButton_->setEnabled(true);
    joinButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(false);
    }
    // The transport is built while no engine is running and is therefore
    // disabled. Refresh it immediately after startup; otherwise an unrelated
    // control refresh (such as toggling the metronome) is required before the
    // first Play click can be delivered.
    updateTrackPlaybackPresentation();
    showLocalSessionHeaderStatus();
    QTimer::singleShot(250, this, [this] {
        updateRuntimeControls();
        sendMetronomeModeToJam();
        sendMetronomePatternToJam();
    });
}

void MainWindow::prepareNetworkRuntimePresentation(bool createSession)
{
    controlRefreshAvailable_ = false;
    metronomeTransport_.clearEngine();
    if (createSession && startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }
    if (createSession && stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
    appendLog(QStringLiteral("starting typed session lifecycle"));
    startButton_->setEnabled(false);
    joinButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(true);
    }
    trackRecordingWorkflow_.clearJamRecordingState();
    updateJamRecordingControls();
    updateMixControls();
    setSessionHeaderStatus(
        createSession ? QStringLiteral("STARTING JAM") : QStringLiteral("JOINING"),
        createSession ? QStringLiteral("Starting a jam") : QStringLiteral("Joining a jam"),
        {QStringLiteral("Preparing authenticated control and direct audio connections.")});
}


bool MainWindow::submitEngineCommand(jam2::EngineCommand command, const QString& context)
{
    command.cookie = ++engineCommandCookie_;
    if (jam2_.submit(command)) {
        return true;

    }
    appendLog(QStringLiteral("engine command queue unavailable: ") + context);
    return false;
}

void MainWindow::setTunerEnabled(bool enabled)
{
    tunerRequestedEnabled_ = enabled;
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::SetPitchAnalysisEnabled;
    command.enabled = enabled;
    command.cookie = ++engineCommandCookie_;
    if (jam2_.submit(command)) {
        tunerCommandCookie_ = command.cookie;
        return;
    }
    tunerRequestedEnabled_ = false;
    tunerCommandCookie_ = 0;
    appendLog(QStringLiteral("engine command queue unavailable: tuner"));
}

void MainWindow::submitEngineGain(
    jam2::EngineCommandType type,
    double gain,
    const QString& context)
{
    jam2::EngineCommand command;
    command.type = type;
    command.value = static_cast<int>(std::clamp(std::llround(gain * 1000000.0), 0LL, 4000000LL));
    (void)submitEngineCommand(command, context);
}

void MainWindow::submitEngineToggle(
    jam2::EngineCommandType type,
    bool enabled,
    const QString& context)
{
    jam2::EngineCommand command;
    command.type = type;

    command.enabled = enabled;
    (void)submitEngineCommand(command, context);
}

void MainWindow::setPreparedTrackLoop(bool enabled, std::uint64_t startFrame, std::uint64_t endFrame)
{
    if (!trackRecordingWorkflow_.setPreparedLoop(enabled, startFrame, endFrame)) {
        appendLog(QStringLiteral("engine command queue unavailable: prepared track loop"));
    }
}

void MainWindow::restartPreparedTrackQuantized()
{
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    const int countInBars = performanceCountInCheck_ &&
        performanceCountInCheck_->isChecked() ? 1 : 0;
    const int activeBank = looperProject_.activeBankIndex();
    const int beatsPerBar = bankMetronomePattern(activeBank).beats_per_bar;
    if (!trackRecordingWorkflow_.restartPrepared(
            position, false, countInBars, beatsPerBar)) {
        appendLog(QStringLiteral("engine command queue unavailable: prepared track restart"));
    }
}

void MainWindow::handleEngineSnapshot(const jam2::EngineSnapshot& snapshot)
{
    const MetronomeTransportController::SnapshotUpdate transportUpdate =
        metronomeTransport_.consume(snapshot);
    updateMetronomePresentationFromEngine(snapshot);
    const bool transportWasRequested =
        trackRecordingWorkflow_.globalTransportRequestedPlaying();
    const bool transportWasPlaying =
        trackRecordingWorkflow_.globalTransportPlaying();
    trackRecordingWorkflow_.consumeSnapshot(snapshot, transportUpdate);
    const bool transportRequestChanged = transportWasRequested !=
        trackRecordingWorkflow_.globalTransportRequestedPlaying();
    if (transportRequestChanged) {
        // A peer-originated UDP transport schedule is applied directly by the
        // local engine. Adopt its requested state for the local controller and
        // button without originating another network transport event.
        trackController_.requestPlayback(
            trackRecordingWorkflow_.globalTransportRequestedPlaying());
    }
    if (transportRequestChanged || transportWasPlaying !=
            trackRecordingWorkflow_.globalTransportPlaying()) {
        appendLog(QStringLiteral(
            "global transport state: requested=%1 playing=%2 pending=%3 action=%4 "
            "countdown_frame=%5 target_frame=%6 engine_frame=%7 prepared_playing=%8 "
            "prepared_start_frame=%9")
            .arg(trackRecordingWorkflow_.globalTransportRequestedPlaying())
            .arg(trackRecordingWorkflow_.globalTransportPlaying())
            .arg(snapshot.transport_pending)
            .arg(static_cast<int>(snapshot.transport_action))
            .arg(snapshot.transport_countdown_start_frame)
            .arg(snapshot.transport_target_frame)
            .arg(snapshot.engine_frame)
            .arg(snapshot.prepared_source_playing)
            .arg(snapshot.prepared_source_actual_start_frame));
    }
    if (trackController_.observeEnginePlaying(
            trackRecordingWorkflow_.globalTransportPlaying()) ||
        transportRequestChanged) {
        updateTrackPlaybackPresentation();
    }
    laneRecordingState_.latencyAdjustmentFrames = static_cast<int>(qBound<qint64>(
        static_cast<qint64>(-8192),
        static_cast<qint64>(snapshot.recording_latency_adjustment_frames),
        static_cast<qint64>(8192)));
    if (const std::optional<int> bars = trackRecordingWorkflow_.takeReadyPendingCountIn(snapshot)) {
        startInputCapture(0, *bars);
    }
    if (publishStoppedTrackStateWhenApplied_ &&
        !trackRecordingWorkflow_.globalTransportPlaying() &&
        sessionController_.isServer()) {
        publishStoppedTrackStateWhenApplied_ = false;
        sendSongSnapshot(false);
    }

    const jam2::EngineGuiPeakSnapshot peaks = jam2_.consumeGuiPeaks();
    if (performanceHome_) {
        performanceHome_->setAudioPeaks(peaks);
        performanceHome_->setTunerSnapshot(snapshot.pitch);
    }
    if (snapshot.lifecycle != jam2::EngineLifecycle::Local) {
        tunerCommandCookie_ = 0;
    } else if (tunerRequestedEnabled_ != snapshot.pitch.enabled &&
               tunerCommandCookie_ == 0) {
        setTunerEnabled(tunerRequestedEnabled_);
    }
    updateMixMeters(mixerStatsViewModel_.consume(
        peaks,
        static_cast<double>(snapshot.send_level_ppm) / 1000000.0,
        snapshot.output_clipped_samples));
    if (diagnosticSampleRateValue_) {
        diagnosticSampleRateValue_->setText(
            snapshot.sample_rate > 0.0
                ? QStringLiteral("%1 Hz").arg(snapshot.sample_rate, 0, 'f', 0)
                : QStringLiteral("-"));
    }
    updatePlaybackGrid();
}

void MainWindow::handleEngineEvent(const jam2::EngineEvent& event)
{
    if (tunerCommandCookie_ != 0 && event.cookie == tunerCommandCookie_) {
        tunerCommandCookie_ = 0;
        if (!event.ok) {
            tunerRequestedEnabled_ = jam2_.engineSnapshot().pitch.enabled;
        }
    }
    const QString text = QString::fromUtf8(
        jam2::engine_event_text(event).data(),
        static_cast<qsizetype>(jam2::engine_event_text(event).size()));
    if (!event.ok || event.type == jam2::EngineEventType::Error ||
        event.type == jam2::EngineEventType::CommandRejected) {
        appendLog(QStringLiteral("engine event type=%1 ok=%2%3")
            .arg(static_cast<int>(event.type))
            .arg(event.ok ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(text.isEmpty() ? QString{} : QStringLiteral(" message=") + text));
    }
    const bool jamRecordingFinished =
        event.ok && event.type == jam2::EngineEventType::JamRecordingStopped;
    const QString completedJamRecordingFolder = jamRecordingFinished
        ? trackRecordingWorkflow_.jamRecordingFolder() : QString{};
    if (trackRecordingWorkflow_.consumeJamRecordingEvent(event)) {
        updateJamRecordingControls();
    }
    if (jamRecordingFinished && !shuttingDown_ &&
        !completedJamRecordingFolder.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, [this, completedJamRecordingFolder] {
            if (!shuttingDown_) {
                showJamRecordingFinished(completedJamRecordingFolder);
            }
        });
    }
    const TrackRecordingWorkflow::TrackTakeCompletion completion =
        trackRecordingWorkflow_.consumeTrackTakeEvent(event);
    if (completion.handled) {
        if (!completion.ok && !completion.wavPath.isEmpty()) {
            registerTransientTrackWav(completion.wavPath);
        }
        if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
        if (!activeRecordingGroupId_.isEmpty()) {
            recordedLaneImportStatus_ = completion.ok
                ? QStringLiteral("take-complete") : QStringLiteral("take-error");
            pendingGroupTakeCompletion_ = completion;
            publishLocalTrackRecordingState(QStringLiteral("complete"));
        } else if (completion.ok && !completion.wavPath.isEmpty()) {
            publishLocalTrackRecordingState(QStringLiteral("finalizing"));
            registerTransientTrackWav(completion.wavPath);
            importLastCaptureToArmedLane();
        } else {
            appendLog(QStringLiteral("track take could not be finalised: %1")
                .arg(completion.error));
            finishLaneTakeFinalization();
        }
        if (loadWavButton_) loadWavButton_->setEnabled(true);
        appendLog(completion.ok
            ? QStringLiteral("track take stopped: take_id=%1 wav=%2 frames=%3 sample_rate=%4")
                .arg(completion.takeId, completion.wavPath)
                .arg(completion.frames)
                .arg(completion.sampleRate)
            : QStringLiteral("track take error: take_id=%1 reason=%2")
                .arg(completion.takeId, completion.error));
    }
}


void MainWindow::handleNetworkSnapshot(const Jam2NetworkOperationalSnapshot& snapshot)
{
    operationalPeers_ = QVector<Jam2OperationalPeer>(
        snapshot.peers.cbegin(), snapshot.peers.cend());
    for (const Jam2OperationalPeer& peer : snapshot.peers) {
        if (!peerOrdinals_.contains(peer.peer_id)) {
            peerOrdinals_.insert(peer.peer_id, nextPeerOrdinal_++);
        }
        if (!desiredPeerGainDb_.contains(peer.peer_id)) {
            desiredPeerGainDb_.insert(peer.peer_id, preferences_.levels.remotePeerDb);
            if (std::abs(peer.gain_db - preferences_.levels.remotePeerDb) > 0.05) {
                (void)jam2_.setPeerGainDb(peer.peer_id, preferences_.levels.remotePeerDb);
            }
        } else if (std::abs(desiredPeerGainDb_.value(peer.peer_id) - peer.gain_db) > 0.05) {
            (void)jam2_.setPeerGainDb(peer.peer_id, desiredPeerGainDb_.value(peer.peer_id));
        }
        const SharedSessionController::EdgeState edge =
            peer.endpoint_state == jam2::PeerEndpointState::Active
                ? SharedSessionController::EdgeState::Active
                : peer.endpoint_state == jam2::PeerEndpointState::Probing
                    ? SharedSessionController::EdgeState::Probing
                    : peer.endpoint_state == jam2::PeerEndpointState::Failed
                        ? SharedSessionController::EdgeState::Failed
                        : SharedSessionController::EdgeState::Candidate;
        const QString proof = peer.endpoint_state == jam2::PeerEndpointState::Active
            ? QStringLiteral("active") : peer.endpoint_state == jam2::PeerEndpointState::Probing
                ? QStringLiteral("probing") : peer.endpoint_state == jam2::PeerEndpointState::Failed
                    ? QStringLiteral("failed") : QStringLiteral("candidate");
        sessionController_.updatePeerEdgeState(
            peer.peer_id,
            edge,
            proof,
            peer.receiving_audio ? QStringLiteral("receiving") : QStringLiteral("waiting"));
    }
    updatePerformancePeers();
    updateJamSessionHeaderStatus(sessionController_.snapshot());
}

void MainWindow::handleConnectionDiagnostics(const ConnectionDiagnosticsSnapshot& snapshot)
{
    lastDiagnostics_ = snapshot;
    updateStatsDisplay(&snapshot);
}

jam2::gui::SessionAudioDeviceList MainWindow::sessionDialogDevices(
    const AudioDevicePreference& preference,
    const QString& requestedDeviceId) const
{
    jam2::gui::SessionAudioDeviceList result;
    result.devices.reserve(static_cast<qsizetype>(availableDevices_.size()));
    for (const jam2::audio::DeviceInfo& device : availableDevices_) {
        result.devices.push_back({
            QStringLiteral("[%1] %2 %3")
                .arg(device.id)
                .arg(QString::fromStdString(device.backend),
                     QString::fromStdString(device.name)),
            QString::number(device.id),
        });
    }

    if (!requestedDeviceId.isEmpty()) {
        const auto requested = std::find_if(
            result.devices.cbegin(), result.devices.cend(),
            [&requestedDeviceId](const auto& item) {
                return item.id == requestedDeviceId;
            });
        if (requested != result.devices.cend()) {
            result.selectedId = requestedDeviceId;
            return result;
        }
    }
    for (const jam2::audio::DeviceInfo& device : availableDevices_) {
        const QString stable = QString::fromStdString(
            device.clsid.empty() ? device.name : device.clsid);
        if (QString::fromStdString(device.backend) == preference.backend &&
            stable == preference.stableId) {
            result.selectedId = QString::number(device.id);
            return result;
        }
    }
    if (!result.devices.isEmpty()) result.selectedId = result.devices.front().id;
    return result;
}

void MainWindow::applyStartJamDialogState(
    const jam2::gui::StartJamDialogState& state)
{
    sessionRuntimeDraft_.configuration = state.create;
    sessionRuntimeDraft_.audio = state.audio;
    sessionRuntimeDraft_.selectedDeviceId = state.selectedDeviceId;
}

void MainWindow::applyJoinJamDialogState(
    const jam2::gui::JoinJamDialogState& state)
{
    CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    configuration.bindHost = state.join.bindHost;
    configuration.port = state.join.port;
    configuration.tuning = state.join.tuning;
    configuration.runtime = state.join.runtime;
    sessionRuntimeDraft_.audio = state.audio;
    sessionRuntimeDraft_.selectedDeviceId = state.selectedDeviceId;
    sessionRuntimeDraft_.inviteUrl = state.inviteUrl;
    sessionRuntimeDraft_.joinProfileName = state.join.tuning.profile;
}

void MainWindow::showStartJamDialog()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner) {
        return;
    }

    applyCreateDefaultsToControls();
    refreshDevices();
    const jam2::gui::SessionAudioDeviceList devices = sessionDialogDevices(
        preferences_.createAudio(), sessionRuntimeDraft_.selectedDeviceId);
    jam2::gui::StartJamDialogState initial{
        preferences_.create,
        preferences_.createAudio(),
        devices.selectedId,
    };
    jam2::gui::StartJamDialogCallbacks callbacks;
    callbacks.refreshDevices = [this](const QString& requestedDeviceId) {
        refreshDevices();
        return sessionDialogDevices(
            preferences_.createAudio(), requestedDeviceId);
    };
    callbacks.testDevice = [this](
        QComboBox* device, QPushButton* button, QWidget* parent) {
        testDeviceSelection(device, button, parent);
    };
    callbacks.saveDefaults = [this](
        const jam2::gui::StartJamDialogState& state) {
        applyStartJamDialogState(state);
        saveCreateDefaults();
    };
    callbacks.generateSession = [this] { generateSession(); };

    activePublicEndpoint_.clear();
    jam2::gui::StartJamDialog dialog(
        initial, devices, std::move(callbacks), this);
    if (dialog.exec() == QDialog::Accepted) {
        applyStartJamDialogState(dialog.state());
        startJam(true);
    }
}

void MainWindow::showJoinJamDialog()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner) {
        return;
    }

    applyJoinDefaultsToControls();
    refreshDevices();
    const jam2::gui::SessionAudioDeviceList devices = sessionDialogDevices(
        preferences_.joinAudio(), sessionRuntimeDraft_.selectedDeviceId);
    jam2::gui::JoinJamDialogState initial{
        sessionRuntimeDraft_.inviteUrl,
        preferences_.join,
        preferences_.joinAudio(),
        devices.selectedId,
    };
    jam2::gui::JoinJamDialogCallbacks callbacks;
    callbacks.refreshDevices = [this](const QString& requestedDeviceId) {
        refreshDevices();
        return sessionDialogDevices(
            preferences_.joinAudio(), requestedDeviceId);
    };
    callbacks.testDevice = [this](
        QComboBox* device, QPushButton* button, QWidget* parent) {
        testDeviceSelection(device, button, parent);
    };
    callbacks.saveDefaults = [this](
        const jam2::gui::JoinJamDialogState& state) {
        applyJoinJamDialogState(state);
        saveJoinDefaults();
    };

    jam2::gui::JoinJamDialog dialog(
        initial, devices, std::move(callbacks), this);
    const int result = dialog.exec();
    const jam2::gui::JoinJamDialogState draft = dialog.state();
    if (result == QDialog::Accepted) {
        applyJoinJamDialogState(draft);
        startJam(false);
    } else {
        // Keep the invite draft so a copied or hand-edited URL is not lost when
        // the user briefly dismisses the dialog. All other edits remain local.
        sessionRuntimeDraft_.inviteUrl = draft.inviteUrl;
    }
}

bool MainWindow::applyLocalAudioSettings(
    const AudioDevicePreference& desired,
    const QString& selectedDeviceId,
    QWidget* parent)
{
    const bool wasLocalActive = jam2_.isRunning() && !jam2_.isNetworkRunning();
    QString permissionError;
    if (!wasLocalActive && !jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(
            parent, QStringLiteral("Jam2 Microphone Access"), permissionError);
        return false;
    }

    const jam2::gui::SessionRuntimeDraft previousDraft = sessionRuntimeDraft_;
    std::optional<Jam2RuntimeOptions> previousOptions;
    try {
        if (wasLocalActive) previousOptions = runtimeOptions();
        bool deviceIdOk = false;
        const int selectedId = selectedDeviceId.toInt(&deviceIdOk);
        if (!deviceIdOk || std::none_of(
                availableDevices_.cbegin(), availableDevices_.cend(),
                [selectedId](const auto& item) {
                    return item.id == selectedId;
                })) {
            throw std::runtime_error("the selected local audio device is unavailable");
        }
        sessionRuntimeDraft_.selectedDeviceId = selectedDeviceId;
        sessionRuntimeDraft_.audio = desired;
        sessionRuntimeDraft_.configuration.sampleRate = desired.sampleRate;
        sessionRuntimeDraft_.configuration.tuning.bufferSize = desired.bufferSize;
        if (wasLocalActive) {
            if (!sessionController_.startLocal(runtimeOptions())) {
                throw std::runtime_error(
                    "the new local audio configuration did not start");
            }
            showLocalSessionHeaderStatus();
        } else {
            launchLocalRuntime(runtimeOptions());
        }
    } catch (const std::exception& error) {
        sessionRuntimeDraft_ = previousDraft;
        const bool restored = !wasLocalActive ||
            (previousOptions && sessionController_.startLocal(*previousOptions));
        if (restored && wasLocalActive) showLocalSessionHeaderStatus();
        else if (!wasLocalActive) showAudioOffSessionHeaderStatus();
        QMessageBox::warning(
            parent,
            QStringLiteral("Local Audio Not Applied"),
            QStringLiteral("%1\n\nPrevious local audio settings %2.")
                .arg(QString::fromUtf8(error.what()),
                     restored ? QStringLiteral("were restored")
                              : QStringLiteral("could not be restored")));
        return false;
    }

    preferences_.localAudio = desired;
    UserPreferencesStore::save(preferences_);
    appendLog(QStringLiteral("local audio settings applied"));
    return true;
}

void MainWindow::showSettingsDialog()
{
    refreshDevices();
    refreshLoopbackSources();
    const bool networkActive = jam2_.isNetworkRunning() ||
        sessionController_.snapshot().role == SharedSessionController::Role::Creator ||
        sessionController_.snapshot().role == SharedSessionController::Role::Joiner;
    const bool localActive = jam2_.isRunning() && !networkActive;

    AudioDevicePreference localInitial = preferences_.localAudio;
    if (localActive) {
        localInitial = sessionRuntimeDraft_.audio;
        localInitial.sampleRate = sessionRuntimeDraft_.configuration.sampleRate;
        localInitial.bufferSize =
            sessionRuntimeDraft_.configuration.tuning.bufferSize;
        jam2::gui::storeSelectedDevicePreference(
            localInitial,
            sessionRuntimeDraft_.selectedDeviceId,
            availableDevices_);
    }

    const auto loopbackChoices = [this] {
        QVector<jam2::gui::SettingsLoopbackSourceChoice> choices;
        choices.reserve(laneRecordingState_.loopbackSources.size());
        for (const LoopbackSourceChoice& source :
             laneRecordingState_.loopbackSources) {
            choices.push_back({source.label, source.id});
        }
        return choices;
    };

    jam2::gui::SettingsDialogCallbacks callbacks;
    callbacks.testDevice =
        [this](QComboBox* device, QPushButton* button, QWidget* parent) {
            testDeviceSelection(device, button, parent);
        };
    callbacks.applyLocalAudio =
        [this, &localInitial](
            const AudioDevicePreference& desired,
            const QString& selectedDeviceId,
            QWidget* parent) {
            const bool applied =
                applyLocalAudioSettings(desired, selectedDeviceId, parent);
            if (applied) localInitial = desired;
            return applied;
        };
    callbacks.refreshLoopbackSources = [this, loopbackChoices] {
        refreshLoopbackSources();
        return loopbackChoices();
    };

    auto result = jam2::gui::SettingsDialog::run(
        {
            preferences_,
            localInitial,
            availableDevices_,
            loopbackChoices(),
            networkActive,
        },
        std::move(callbacks),
        this);
    if (!result) return;

    UserPreferences updated = std::move(result->preferences);
    const bool localChanged = !networkActive && (
        localInitial.backend != updated.localAudio.backend ||
        localInitial.stableId != updated.localAudio.stableId ||
        localInitial.sampleRate != updated.localAudio.sampleRate ||
        localInitial.bufferSize != updated.localAudio.bufferSize ||
        localInitial.inputChannels != updated.localAudio.inputChannels ||
        localInitial.outputChannels != updated.localAudio.outputChannels);
    if (jam2_.isRunning() && !jam2_.isNetworkRunning() && localChanged) {
        std::optional<Jam2RuntimeOptions> previousOptions;
        const jam2::gui::SessionRuntimeDraft previousDraft =
            sessionRuntimeDraft_;
        bool restartAttempted = false;
        try {
            previousOptions = runtimeOptions();
            sessionRuntimeDraft_.selectedDeviceId =
                result->selectedLocalDeviceId;
            sessionRuntimeDraft_.audio = updated.localAudio;
            sessionRuntimeDraft_.configuration.sampleRate =
                updated.localAudio.sampleRate;
            sessionRuntimeDraft_.configuration.tuning.bufferSize =
                updated.localAudio.bufferSize;
            restartAttempted = true;
            if (!sessionController_.startLocal(runtimeOptions())) {
                throw std::runtime_error(
                    "the new local audio configuration did not start");
            }
            showLocalSessionHeaderStatus();
        } catch (const std::exception& error) {
            sessionRuntimeDraft_ = previousDraft;
            const bool restored = !restartAttempted ||
                (previousOptions && sessionController_.startLocal(*previousOptions));
            QMessageBox::warning(
                this,
                QStringLiteral("Settings not applied"),
                QStringLiteral("%1\n\nPrevious local audio settings %2.")
                    .arg(QString::fromUtf8(error.what()),
                         restored ? QStringLiteral("were restored")
                                  : QStringLiteral("could not be restored")));
            return;
        }
    }

    preferences_ = std::move(updated);
    sessionRuntimeDraft_.joinProfileName = preferences_.join.tuning.profile;
    UserPreferencesStore::save(preferences_);
    applyPreferencesToControls();
    appendLog(QStringLiteral("preferences saved"));
}

void MainWindow::stopJam(bool returnToLocal)
{
    const bool shouldReturnToLocal = returnToLocal && !shuttingDown_;
    jamStartupPending_ = false;
    controlRefreshAvailable_ = false;
    lastDiagnostics_.reset();
    updateStatsDisplay(nullptr);
    clearAutomationTransferPause();
    assetTransfer_.cancel();
    cancelSharedBankLaunch(true, QStringLiteral("leaving the jam"));
    sessionController_.close();
    // The Engine intentionally survives Leave so Perform can resume without a
    // device restart. Stop the old session click after detaching the network;
    // otherwise the audio callback keeps rendering it while the local UI has
    // already changed back to the stopped state.
    if (jam2_.isRunning()) {
        submitEngineToggle(
            jam2::EngineCommandType::SetMetronomeEnabled,
            false,
            QStringLiteral("leave metronome enabled"));
    }
    localMeshPeerTokens_.clear();
    operationalPeers_.clear();
    peerTrackRecordingStates_.clear();
    peerTrackRecordingRevisions_.clear();
    peerOrdinals_.clear();
    desiredPeerGainDb_.clear();
    selectedPeerId_ = 0;
    nextPeerOrdinal_ = 1;
    peerMembershipSignature_.clear();
    updatePerformancePeers();
    updateSharedRecordingPresentation();

    meshPeerEndpoints_.clear();
    lastLoggedSessionSummary_.clear();
    pendingMeshInvitePopup_ = false;
    activePublicEndpoint_.clear();
    resetTrackSyncSessionState();
    handledReferenceRenderRequests_.clear();
    deferredReferenceRenderRequests_.clear();
    publishStoppedTrackStateWhenApplied_ = false;
    trackRecordingWorkflow_.clearSessionSchedule();
    if (recordingCountdownLabel_) recordingCountdownLabel_->hide();
    metronomeTransport_.setLocalState(false);
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }

    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
    startButton_->setEnabled(true);
    joinButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(false);
    }
    updateJamRecordingControls();
    updateMixControls();
    setMixRemotePeerVisible(false);
    if (shouldReturnToLocal && jam2_.isRunning()) {
        try {
            if (!sessionController_.startLocal(runtimeOptions())) {
                throw std::runtime_error("local runtime failed to resume");
            }
        } catch (const std::exception& error) {
            appendLog(QStringLiteral("local resume failed: ") + QString::fromUtf8(error.what()));
            setSessionHeaderStatus(
                QStringLiteral("AUDIO ISSUE"),
                QStringLiteral("Local audio could not resume"),
                {QString::fromUtf8(error.what()).left(160),
                 QStringLiteral("Click to open Local Audio settings.")},
                true,
                true);
            return;
        }
        showLocalSessionHeaderStatus();
        updateRuntimeControls();
        sendMetronomeModeToJam();
        sendMetronomePatternToJam();
    } else if (!shuttingDown_) {
        showAudioOffSessionHeaderStatus();
    }
}

void MainWindow::refreshDevices()
{
    const QString previous = sessionRuntimeDraft_.selectedDeviceId;
    availableDevices_.clear();
    if (automationHeadlessAudio_) {
        jam2::audio::DeviceInfo device{
            0,
            "automation",
            "Headless fake audio device",
            "jam2-automation-headless",
            {},
        };
        availableDevices_.push_back(device);
        jam2::audio::DeviceTestResult capabilities;
        capabilities.device = std::move(device);
        capabilities.current_sample_rate = 48000.0;
        capabilities.sample_rate_supported.fill(true);
        capabilities.buffer_size_supported.fill(true);
        deviceCapabilitiesCache_.insert(
            jam2::gui::audioDevicePreferenceKey(capabilities.device), capabilities);
    } else {
        try {
            availableDevices_ = jam2::audio::list_devices();
        } catch (const std::exception& error) {
            appendLog(QStringLiteral("device refresh failed: ") +
                QString::fromUtf8(error.what()));
        }
    }
    if (availableDevices_.empty()) {
        sessionRuntimeDraft_.selectedDeviceId.clear();
        appendLog(QStringLiteral("no audio devices returned by the local engine"));
        return;
    }

    const auto hasId = [this](const QString& value) {
        bool ok = false;
        const int id = value.toInt(&ok);
        return ok && std::any_of(
            availableDevices_.cbegin(), availableDevices_.cend(),
            [id](const auto& device) { return device.id == id; });
    };
    if (hasId(previous)) {
        sessionRuntimeDraft_.selectedDeviceId = previous;
    } else {
        sessionRuntimeDraft_.selectedDeviceId.clear();
        for (const auto& device : availableDevices_) {
            const QString stable = QString::fromStdString(
                device.clsid.empty() ? device.name : device.clsid);
            if (QString::fromStdString(device.backend) ==
                    preferences_.networkAudio.backend &&
                stable == preferences_.networkAudio.stableId) {
                sessionRuntimeDraft_.selectedDeviceId =
                    QString::number(device.id);
                break;
            }
        }
        if (sessionRuntimeDraft_.selectedDeviceId.isEmpty()) {
            sessionRuntimeDraft_.selectedDeviceId =
                QString::number(availableDevices_.front().id);
        }
    }
    appendLog(QStringLiteral("loaded %1 audio devices")
        .arg(availableDevices_.size()));
}

void MainWindow::appendLog(const QString& line)
{
    if (logEdit_) {
        logEdit_->appendPlainText(line);
    }
    if (!guiLogFile_) {
        QString root = preferences_.logging.folder.trimmed();
        if (root.isEmpty()) {
            root = appReleaseFolderPath(QStringLiteral("logs"));
        }
        QDir directory(root);
        if (directory.mkpath(QStringLiteral("."))) {
            guiLogPath_ = directory.filePath(QStringLiteral("jam2_gui_%1_pid%2.log")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")))
                .arg(QCoreApplication::applicationPid()));
            auto file = std::make_unique<QFile>(guiLogPath_);
            if (file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                guiLogFile_ = std::move(file);
            }
        }
    }
    if (guiLogFile_ && guiLogFile_->isOpen()) {
        const QByteArray record = QStringLiteral("%1 %2\n")
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), line)
            .toUtf8();
        (void)guiLogFile_->write(record);
        (void)guiLogFile_->flush();
    }
}

void MainWindow::updateStatsDisplay(const ConnectionDiagnosticsSnapshot* stats)
{
    const MixerStatsLabels labels = mixerStatsViewModel_.present(stats);
    if (latencyLabel_) {
        latencyLabel_->setText(labels.latency);
        latencyLabel_->setToolTip(labels.latencyTooltip);
    }
    if (jitterLabel_) jitterLabel_->setText(labels.jitter);
    if (lossLabel_) lossLabel_->setText(labels.loss);
    if (underrunLabel_) underrunLabel_->setText(labels.underrun);
    diagnosisLabel_->setText(labels.diagnosis);
    QString measuredRtt;
    if (stats != nullptr) {
        const auto measured = std::find_if(
            stats->peers.cbegin(),
            stats->peers.cend(),
            [](const Jam2PeerDiagnostics& peer) { return peer.has_rtt; });
        if (measured != stats->peers.cend()) {
            measuredRtt = QStringLiteral("%1 ms").arg(measured->rtt_ms, 0, 'f', 1);
        }
    }
    if (sessionHeaderRtt_ != measuredRtt) {
        sessionHeaderRtt_ = measuredRtt;
        updateJamSessionHeaderStatus(sessionController_.snapshot());
    }
    if (diagnosticPacketsValue_) {
        diagnosticPacketsValue_->setText(
            stats != nullptr
                ? QString::number(static_cast<qulonglong>(stats->received_packets))
                : QStringLiteral("0"));
    }
    if (diagnosticLateValue_) {
        diagnosticLateValue_->setText(
            stats != nullptr
                ? QString::number(
                    static_cast<qulonglong>(stats->reordered_or_late_packets))
                : QStringLiteral("0"));
    }
    if (diagnosticLossEventsValue_) {
        diagnosticLossEventsValue_->setText(
            stats != nullptr
                ? QString::number(static_cast<qulonglong>(stats->loss_events))
                : QStringLiteral("0"));
    }
    if (diagnosticBurstGapsValue_) {
        diagnosticBurstGapsValue_->setText(
            stats != nullptr
                ? QString::number(static_cast<qulonglong>(stats->packet_gap_over_4x))
                : QStringLiteral("0"));
    }
    if (diagnosticDriftValue_) {
        const Jam2PeerDiagnostics* strongest = nullptr;
        if (stats != nullptr) {
            for (const Jam2PeerDiagnostics& peer : stats->peers) {
                if (peer.drift_valid &&
                    (strongest == nullptr ||
                     std::abs(peer.drift_ppm) > std::abs(strongest->drift_ppm))) {
                    strongest = &peer;
                }
            }
        }
        diagnosticDriftValue_->setText(strongest != nullptr
            ? QStringLiteral("%1%2 ppm")
                .arg(strongest->drift_ppm >= 0.0 ? QStringLiteral("+") : QString{})
                .arg(strongest->drift_ppm, 0, 'f', 1)
            : stats != nullptr && !stats->peers.empty()
                ? QStringLiteral("MEASURING")
                : QStringLiteral("-"));
    }
    if (diagnosticMissingAudioValue_) {
        diagnosticMissingAudioValue_->setText(stats != nullptr
            ? QStringLiteral("%1 fr").arg(
                static_cast<qulonglong>(stats->missing_audio_frames))
            : QStringLiteral("0 fr"));
    }
    if (diagnosticOutputUnderrunsValue_) {
        diagnosticOutputUnderrunsValue_->setText(stats != nullptr
            ? QStringLiteral("%1 · %2 ms")
                .arg(static_cast<qulonglong>(stats->output_underrun_events))
                .arg(stats->output_underrun_ms, 0, 'f', 1)
            : QStringLiteral("0"));
    }
    if (diagnosticPeerTable_) {
        diagnosticPeerTable_->setRowCount(
            stats != nullptr ? static_cast<int>(stats->peers.size()) : 0);
        if (stats != nullptr) {
            for (int row = 0; row < static_cast<int>(stats->peers.size()); ++row) {
                const Jam2PeerDiagnostics& peer = stats->peers.at(
                    static_cast<std::size_t>(row));
                const int ordinal = peerOrdinals_.value(peer.peer_id, row + 1);
                const QStringList values{
                    QStringLiteral("Peer %1").arg(ordinal),
                    peer.has_rtt
                        ? QStringLiteral("%1 ms").arg(peer.rtt_ms, 0, 'f', 1)
                        : QStringLiteral("-"),
                    QStringLiteral("%1 ms").arg(peer.jitter_average_ms, 0, 'f', 1),
                    QStringLiteral("%1%").arg(peer.packet_loss_percent, 0, 'f', 2),
                    QString::number(
                        static_cast<qulonglong>(peer.reordered_or_late_packets)),
                    peer.drift_valid
                        ? QStringLiteral("%1%2")
                            .arg(peer.drift_ppm >= 0.0 ? QStringLiteral("+") : QString{})
                            .arg(peer.drift_ppm, 0, 'f', 1)
                        : QStringLiteral("-")};
                for (int column = 0; column < values.size(); ++column) {
                    diagnosticPeerTable_->setItem(
                        row,
                        column,
                        new QTableWidgetItem(values.at(column)));
                }
            }
        }
    }
    if (diagnosisEvidenceLabel_) {
        if (stats == nullptr) {
            diagnosisEvidenceLabel_->setText(
                QStringLiteral("Waiting for live connection measurements."));
        } else {
            QStringList findings;
            if (stats->output_underrun_events > 0) {
                findings << QStringLiteral(
                    "<b>Output underruns:</b> %1 events / %2 ms. "
                    "Increase local prefill or buffer.")
                    .arg(stats->output_underrun_events)
                    .arg(stats->output_underrun_ms, 0, 'f', 1);
            }
            if (stats->packet_loss_percent >= 0.5) {
                findings << QStringLiteral(
                    "<b>Packet loss:</b> %1%. Try wired networking or a larger frame size.")
                    .arg(stats->packet_loss_percent, 0, 'f', 2);
            }
            const double burstPercent = stats->packet_gap_samples > 0
                ? static_cast<double>(stats->packet_gap_over_4x) * 100.0 /
                    static_cast<double>(stats->packet_gap_samples)
                : 0.0;
            const double latePercent = stats->received_packets > 0
                ? static_cast<double>(stats->reordered_or_late_packets) * 100.0 /
                    static_cast<double>(stats->received_packets)
                : 0.0;
            if (burstPercent >= 0.5 || latePercent >= 0.25) {
                findings << QStringLiteral(
                    "<b>Jitter/reordering pressure:</b> burst %1%, late %2%. "
                    "Increase local playout or prefill.")
                    .arg(burstPercent, 0, 'f', 2)
                    .arg(latePercent, 0, 'f', 2);
            }
            if (stats->callback_gap_over_2x > 0) {
                findings << QStringLiteral(
                    "<b>Audio callback gaps:</b> %1. Try a larger device buffer.")
                    .arg(stats->callback_gap_over_2x);
            }
            if (stats->drift_clamped_samples > 0 || stats->drift_abs_ppm_max >= 200.0) {
                findings << QStringLiteral(
                    "<b>Clock drift pressure:</b> peak %1 ppm, clamped samples %2. "
                    "Increase local buffering.")
                    .arg(stats->drift_abs_ppm_max, 0, 'f', 1)
                    .arg(stats->drift_clamped_samples);
            }
            for (const Jam2PeerDiagnostics& peer : stats->peers) {
                if (peer.has_rtt && peer.rtt_ms >= 100.0) {
                    findings << QStringLiteral(
                        "<b>High peer RTT:</b> %1 ms. Physical distance is limiting latency.")
                        .arg(peer.rtt_ms, 0, 'f', 1);
                    break;
                }
            }
            diagnosisEvidenceLabel_->setText(
                findings.isEmpty()
                    ? QStringLiteral(
                        "<b>No active issue detected.</b> Live loss, jitter, callback and "
                        "drift thresholds are currently below the diagnosis rules.")
                    : findings.join(QStringLiteral("<br><br>")));
        }
    }
}

void MainWindow::openWorkspace(const QString& page)
{
    if (!workspaceStack_ || !performanceStageStack_) {
        return;
    }
    if (page == QStringLiteral("performance")) {
        if (detailIdentityPanel_) {
            detailIdentityPanel_->setVisible(false);
        }
        performanceStageStack_->setCurrentIndex(0);
        return;
    }
    const auto found = workspacePages_.constFind(page);
    if (found != workspacePages_.cend()) {
        const bool looperOpen = page == QStringLiteral("looper");
        const bool sectionEditorOpen = page == QStringLiteral("chords") ||
            page == QStringLiteral("beats") || page == QStringLiteral("lyrics") ||
            looperOpen;
        if (detailIdentityPanel_) {
            detailIdentityPanel_->setVisible(looperOpen || sectionEditorOpen);
        }
        if (detailPositionLabel_) {
            detailPositionLabel_->setReadOnly(true);
            detailPositionLabel_->setFocusPolicy(Qt::NoFocus);
            detailPositionLabel_->deselect();
            detailPositionLabel_->setProperty("editing", false);
            detailPositionLabel_->setProperty("sectionEditable", sectionEditorOpen);
            detailPositionLabel_->setToolTip(sectionEditorOpen
                ? QStringLiteral("Double-click to rename this section") : QString());
            detailPositionLabel_->style()->unpolish(detailPositionLabel_);
            detailPositionLabel_->style()->polish(detailPositionLabel_);
            if (sectionEditorOpen && viewedBankIndex_ >= 0 &&
                viewedBankIndex_ < chordModel_.sections().size()) {
                const QString name = chordModel_.section(viewedBankIndex_).name.trimmed();
                detailPositionLabel_->setText(name.isEmpty()
                    ? QStringLiteral("Section %1").arg(
                        QChar(QLatin1Char('A').unicode() + viewedBankIndex_))
                    : name);
                detailPositionLabel_->setCursorPosition(0);
                detailPositionLabel_->deselect();
            }
        }
        if (page == QStringLiteral("chords") && chordGrid_) {
            chordGrid_->refresh();
        } else if (page == QStringLiteral("beats") && beatGrid_) {
            beatGrid_->refresh();
        } else if (page == QStringLiteral("lyrics") && lyricGrid_) {
            lyricGrid_->refresh();
        }
        workspaceStack_->setCurrentIndex(found.value());
        performanceStageStack_->setCurrentIndex(1);
        const QList<QPushButton*> buttons =
            findChildren<QPushButton*>(QStringLiteral("DetailTab"));
        for (QPushButton* button : buttons) {
            const bool active = button->property("workspaceKey").toString() == page;
            if (button->property("active").toBool() != active) {
                button->setProperty("active", active);
                button->style()->unpolish(button);
                button->style()->polish(button);
            }
        }
    }
}

void MainWindow::toggleDataDrawer()
{
    if (dataOverlay_) {
        dataOverlay_->setVisible(!dataOverlay_->isVisible());
        if (dataOverlay_->isVisible()) {
            dataOverlay_->raise();
            updateStatsDisplay(lastDiagnostics_ ? &*lastDiagnostics_ : nullptr);
        }
    }
}

void MainWindow::selectPerformancePeer(std::uint64_t peerId)
{
    const auto found = std::find_if(
        operationalPeers_.cbegin(),
        operationalPeers_.cend(),
        [peerId](const Jam2OperationalPeer& peer) { return peer.peer_id == peerId; });
    if (found == operationalPeers_.cend()) {
        selectedPeerId_ = 0;
        if (performanceHome_) performanceHome_->setSelectedPeer(0);
        if (selectedPeerNameLabel_) selectedPeerNameLabel_->setText(QStringLiteral("Volume"));
        if (selectedPeerGainSlider_) selectedPeerGainSlider_->setEnabled(false);
        if (selectedPeerGainLabel_) selectedPeerGainLabel_->setText(QStringLiteral("- dB"));
        updateMixControls();
        return;
    }
    selectedPeerId_ = peerId;
    if (performanceHome_) {
        performanceHome_->setSelectedPeer(peerId);
    }
    const int ordinal = peerOrdinals_.value(peerId, 0);
    const double gainDb = desiredPeerGainDb_.value(peerId, found->gain_db);
    if (selectedPeerNameLabel_) {
        selectedPeerNameLabel_->setText(QStringLiteral("Volume"));
    }
    if (selectedPeerGainSlider_) {
        const QSignalBlocker blocker(selectedPeerGainSlider_);
        selectedPeerGainSlider_->setValue(qBound(-60, qRound(gainDb), 12));
        selectedPeerGainSlider_->setEnabled(true);
    }
    if (selectedPeerGainLabel_) {
        selectedPeerGainLabel_->setText(dbText(gainDb));
    }
    updateMixControls();
}

void MainWindow::applySelectedPeerGain(int db)
{
    if (selectedPeerId_ == 0) {
        return;
    }
    const double gainDb = static_cast<double>(qBound(-60, db, 12));
    desiredPeerGainDb_[selectedPeerId_] = gainDb;
    if (selectedPeerGainLabel_) {
        selectedPeerGainLabel_->setText(dbText(gainDb));
    }
    if (QLabel* value = peerGainValueLabels_.value(selectedPeerId_, nullptr)) {
        value->setText(dbText(gainDb));
    }
    if (QSlider* slider = peerGainSliders_.value(selectedPeerId_, nullptr);
        slider != nullptr && slider->value() != db) {
        const QSignalBlocker blocker(slider);
        slider->setValue(db);
    }
    if (!jam2_.setPeerGainDb(selectedPeerId_, gainDb)) {
        appendLog(QStringLiteral("peer gain update unavailable for Peer %1")
            .arg(peerOrdinals_.value(selectedPeerId_, 0)));
    }
    updatePerformancePeers();
}

void MainWindow::updatePerformancePeers()
{
    std::sort(
        operationalPeers_.begin(),
        operationalPeers_.end(),
        [this](const Jam2OperationalPeer& left, const Jam2OperationalPeer& right) {
            return peerOrdinals_.value(left.peer_id) < peerOrdinals_.value(right.peer_id);
        });

    QVector<PerformancePeerPresentation> presentation;
    QStringList membership;
    presentation.reserve(operationalPeers_.size());
    membership.reserve(operationalPeers_.size());
    for (const Jam2OperationalPeer& peer : std::as_const(operationalPeers_)) {
        const int ordinal = peerOrdinals_.value(peer.peer_id);
        const double gainDb = desiredPeerGainDb_.value(peer.peer_id, peer.gain_db);
        presentation.push_back({
            peer.peer_id,
            QStringLiteral("Peer %1").arg(ordinal),
            peer.receiving_audio,
            gainDb,
            peer.peer_id == selectedPeerId_});
        membership.push_back(QString::number(peer.peer_id));
    }
    if (performanceHome_) {
        performanceHome_->setPeers(presentation);
    }

    const QString signature = membership.join(QLatin1Char(','));
    if (signature != peerMembershipSignature_ && peerGainListLayout_) {
        peerMembershipSignature_ = signature;
        peerGainSliders_.clear();
        peerGainValueLabels_.clear();
        while (QLayoutItem* item = peerGainListLayout_->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
        for (const PerformancePeerPresentation& peer : std::as_const(presentation)) {
            auto* row = new QWidget(peerGainListContent_);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 3, 0, 3);
            auto* name = new QPushButton(peer.label, row);
            name->setFlat(true);
            name->setMinimumWidth(92);
            name->setToolTip(peer.receiving
                ? QStringLiteral("Receiving audio")
                : QStringLiteral("Connected; waiting for audio"));
            QObject::connect(name, &QPushButton::clicked, this, [this, id = peer.peerId] {
                selectPerformancePeer(id);
            });
            auto* slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(-60, 12);
            slider->setValue(qRound(peer.gainDb));
            applyJamSliderStyle(slider);
            auto* value = new QLabel(dbText(peer.gainDb), row);
            value->setMinimumWidth(68);
            QObject::connect(slider, &QSlider::valueChanged, this,
                [this, id = peer.peerId, value](int db) {
                    desiredPeerGainDb_[id] = static_cast<double>(db);
                    value->setText(dbText(static_cast<double>(db)));
                    (void)jam2_.setPeerGainDb(id, static_cast<double>(db));
                    if (selectedPeerId_ == id) {
                        selectPerformancePeer(id);
                    }
                    updatePerformancePeers();
                });
            layout->addWidget(name);
            layout->addWidget(slider, 1);
            layout->addWidget(value);
            peerGainSliders_.insert(peer.peerId, slider);
            peerGainValueLabels_.insert(peer.peerId, value);
            peerGainListLayout_->addWidget(row);
        }
        peerGainListLayout_->addStretch(1);
    }

    if (peerGainScroll_) {
        const int shownRows = qMin(10, operationalPeers_.size());
        peerGainScroll_->setVerticalScrollBarPolicy(
            operationalPeers_.size() > 10 ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        peerGainScroll_->setFixedHeight(qMax(52, shownRows * 48 + 8));
    }

    const bool selectedStillPresent = std::any_of(
        operationalPeers_.cbegin(),
        operationalPeers_.cend(),
        [this](const Jam2OperationalPeer& peer) { return peer.peer_id == selectedPeerId_; });
    if (selectedPeerId_ != 0 && !selectedStillPresent) {
        selectedPeerId_ = 0;
    }
    selectPerformancePeer(selectedPeerId_);
}

void MainWindow::updateMixMeters(const MixerMeterLevels& levels)
{
    if (mixInputMeter_) mixInputMeter_->setLevel(levels.input);
    if (mixSendMeter_) mixSendMeter_->setLevel(levels.send);
    if (mixMonitorMeter_) mixMonitorMeter_->setLevel(levels.monitor);
    if (mixRemotePeerMeter_) {
        const auto selected = std::find_if(
            operationalPeers_.cbegin(),
            operationalPeers_.cend(),
            [this](const Jam2OperationalPeer& peer) {
                return peer.peer_id == selectedPeerId_;
            });
        mixRemotePeerMeter_->setLevel(
            selected != operationalPeers_.cend()
                ? static_cast<double>(selected->peak_ppm) / 1000000.0
                : levels.remote);
    }
    if (mixTrackMeter_) mixTrackMeter_->setLevel(levels.track);
    if (mixMetronomeMeter_) mixMetronomeMeter_->setLevel(levels.metronome);
    if (mixOutputMeter_) mixOutputMeter_->setLevel(levels.output);
    if (mixOutputClipLabel_) {
        mixOutputClipLabel_->setText(
            QStringLiteral("clip %1").arg(static_cast<qulonglong>(levels.outputClippedSamples)));
    }
}

void MainWindow::applySessionSnapshot(const SharedSessionController::Snapshot& snapshot)
{
    if (snapshot.role != SharedSessionController::Role::Creator &&
        snapshot.role != SharedSessionController::Role::Joiner) {
        return;
    }
    QMap<QString, QString> endpoints;
    QSet<QString> sameMachineTokens;
    for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
        endpoints[peer.token] = peer.endpoint;
        if (peer.sameMachine) {
            sameMachineTokens.insert(peer.token);
        }
    }
    meshPeerEndpoints_ = std::move(endpoints);
    localMeshPeerTokens_ = std::move(sameMachineTokens);
    if (stopButton_) {
        stopButton_->setText(snapshot.role == SharedSessionController::Role::Creator
            ? QStringLiteral("End Jam") : QStringLiteral("Leave Jam"));
    }

    if (snapshot.membershipRevision > 0) {
        const QString summary = QStringLiteral(
            "session snapshot membership_revision=%1 total_peers=%2 remote_peers=%3 "
            "coordinator=%4 editor_authority=%5 editor_revision=%6 "
            "arrangement_authority=%7 arrangement_revision=%8")
            .arg(snapshot.membershipRevision)
            .arg(snapshot.totalPeerCount)
            .arg(snapshot.remotePeerCount)
            .arg(snapshot.coordinatorToken.left(8), snapshot.editorAuthorityToken.left(8))
            .arg(snapshot.editorRevision)
            .arg(snapshot.arrangementAuthorityToken.left(8))
            .arg(snapshot.arrangementRevision);
        QString diagnosticKey = summary;
        for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
            if (peer.token != snapshot.localToken) {
                diagnosticKey += QStringLiteral("|%1|%2|%3|%4")
                    .arg(peer.token, peer.endpoint)
                    .arg(static_cast<int>(peer.edgeState))
                    .arg(peer.proofState);
            }
        }
        if (diagnosticKey != lastLoggedSessionSummary_) {
            lastLoggedSessionSummary_ = diagnosticKey;
            appendLog(summary);
            for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
                if (peer.token != snapshot.localToken) {
                    appendLog(QStringLiteral(
                        "session peer token=%1 endpoint=%2 edge=%3 proof=%4")
                        .arg(peer.token.left(8), peer.endpoint)
                        .arg(static_cast<int>(peer.edgeState))
                        .arg(peer.proofState));
                }
            }
        }
    }
    updateJamSessionHeaderStatus(snapshot);
    QSet<QString> memberTokens;
    for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
        memberTokens.insert(peer.token);
    }
    for (auto it = peerTrackRecordingStates_.begin();
         it != peerTrackRecordingStates_.end();) {
        if (!memberTokens.contains(it.key())) {
            it = peerTrackRecordingStates_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = peerTrackRecordingRevisions_.begin();
         it != peerTrackRecordingRevisions_.end();) {
        if (!memberTokens.contains(it.key())) {
            it = peerTrackRecordingRevisions_.erase(it);
        } else {
            ++it;
        }
    }
    updateMixRemotePeers();
    updateTrackControls();
    updateSharedRecordingPresentation();
    maybeFinishRecordingGroup();
    if (incomingAssetWorkflow_ == IncomingAssetWorkflow::None &&
        (!pendingTrackContributions_.isEmpty() || !pendingLooperAssetHashes_.isEmpty())) {
        QTimer::singleShot(0, this, [this] { requestNextPendingAsset(); });
    }
}

void MainWindow::handleControlEvent(
    const jam2::control_protocol::TransportEvent& event,
    bool serverSide)
{

    using jam2::control_protocol::TransportEventType;
    if (event.assetChannel) {
        appendLog(QStringLiteral("asset_tcp: ") + event.detail);
        if (event.type == TransportEventType::Disconnected ||
            event.type == TransportEventType::Failure) {
            if (serverSide) {
                const ControlServer::Stats stats = sessionController_.serverStats();
                appendLog(QStringLiteral(
                    "asset_tcp_stats side=server accepted=%1 active=%2 active_high_water=%3 disconnected=%4")
                    .arg(stats.assetAcceptedConnections)
                    .arg(stats.assetActiveConnections)
                    .arg(stats.assetConnectionHighWater)
                    .arg(stats.assetDisconnectedConnections));
            } else {
                const ControlClient::Stats stats = sessionController_.assetClientStats();
                appendLog(QStringLiteral(
                    "asset_tcp_stats side=client attempts=%1 completed=%2 disconnected=%3 "
                    "input_high_water=%4 output_high_water=%5 output_rejects=%6")
                    .arg(stats.connectionAttempts)
                    .arg(stats.completedConnections)
                    .arg(stats.disconnectedConnections)
                    .arg(stats.maxBufferedInputBytes)
                    .arg(stats.maxQueuedOutputBytes)
                    .arg(stats.outputHighWaterRejects));
            }
            if (performanceHome_) {
                performanceHome_->setTrackTransferStatus(
                    QStringLiteral("TRACK SHARE ERROR — asset connection retrying"));
            }
        }
        return;
    }
    if (serverSide && !event.authenticated &&
        (event.type == TransportEventType::ChallengeSent ||
         event.type == TransportEventType::Failure ||
         event.type == TransportEventType::Disconnected)) {
        appendLog(QStringLiteral("pending_tcp: ") + event.detail);
        if (event.type == TransportEventType::Failure &&
            event.failure == jam2::control_protocol::TransportFailure::PreAuthenticationDisconnect) {
            notePreAuthenticationDisconnect();
        }
        // Pending authentication traffic has not joined the session and must
        // not change the established control-session presentation.
        return;
    }
    QString displayState = event.detail;
    switch (event.type) {
    case TransportEventType::Listening:
        displayState = QStringLiteral("Waiting for peers");
        controlRefreshAvailable_ = false;
        break;
    case TransportEventType::Connecting:
    case TransportEventType::Connected:
    case TransportEventType::ChallengeSent:
    case TransportEventType::ProofSent:
        displayState = QStringLiteral("Connecting");
        break;
    case TransportEventType::Authenticated:
        displayState = QStringLiteral("TCP peer authenticated");
        controlRefreshAvailable_ = false;
        break;
    case TransportEventType::Disconnected:
        displayState = serverSide ? QStringLiteral("Waiting for peers") : QStringLiteral("Reconnecting");
        controlRefreshAvailable_ = !serverSide;
        break;
    case TransportEventType::AlreadyConnected:
        displayState = QStringLiteral("TCP peer authenticated");
        controlRefreshAvailable_ = false;
        break;
    case TransportEventType::RefreshRequested:
        displayState = QStringLiteral("Refreshing");
        controlRefreshAvailable_ = false;
        break;
    case TransportEventType::ReconnectScheduled:
    case TransportEventType::ReconnectAttempt:
        displayState = QStringLiteral("TCP issue");
        controlRefreshAvailable_ = true;
        break;
    case TransportEventType::SessionEnded:
        displayState = QStringLiteral("Local");
        controlRefreshAvailable_ = false;
        break;
    case TransportEventType::Failure:
        controlRefreshAvailable_ = true;
        if (event.retryable) {
            displayState = QStringLiteral("TCP issue");
        }
        break;
    }
    if (event.type == TransportEventType::Listening ||
        event.type == TransportEventType::Authenticated ||
        event.type == TransportEventType::AlreadyConnected) {
        updateJamSessionHeaderStatus(sessionController_.snapshot());
    } else if (event.type == TransportEventType::SessionEnded) {
        if (jam2_.isRunning()) showLocalSessionHeaderStatus();
        else showAudioOffSessionHeaderStatus();
    } else {
        const SharedSessionController::Role role = sessionController_.snapshot().role;
        const bool joining = role == SharedSessionController::Role::Joiner;
        QString pill = displayState.toUpper();
        if (event.type == TransportEventType::Connecting ||
            event.type == TransportEventType::Connected ||
            event.type == TransportEventType::ChallengeSent ||
            event.type == TransportEventType::ProofSent) {
            pill = joining ? QStringLiteral("JOINING") : QStringLiteral("STARTING JAM");
        } else if (event.type == TransportEventType::Disconnected ||
                   event.type == TransportEventType::ReconnectScheduled ||
                   event.type == TransportEventType::ReconnectAttempt) {
            pill = joining ? QStringLiteral("RECONNECTING") : QStringLiteral("JAM · WAITING");
        } else if (event.type == TransportEventType::Failure) {
            pill = QStringLiteral("CONNECTION ISSUE");
        }
        QStringList detailLines;
        if (!event.detail.trimmed().isEmpty() && event.detail != displayState) {
            detailLines << event.detail.left(160);
        }
        if (controlRefreshAvailable_) {
            detailLines << QStringLiteral("Click to retry the control connection.");
        }
        setSessionHeaderStatus(
            pill,
            displayState,
            detailLines,
            controlRefreshAvailable_,
            controlRefreshAvailable_);
    }
    appendLog(QStringLiteral("control: ") + event.detail);
    if (event.type == TransportEventType::Disconnected ||
        event.type == TransportEventType::Failure) {
        const SharedSessionController::Snapshot policy = sessionController_.snapshot();
        appendLog(QStringLiteral(
            "control_policy_stats validation_rejects=%1 authorization_rejects=%2 "
            "heartbeat_interval_ms=%3 heartbeat_miss_limit=%4 heartbeat_missed=%5 "
            "last_heartbeat_age_ms=%6 heartbeats_sent=%7 heartbeats_received=%8 heartbeat_acks=%9")
            .arg(policy.validationRejections)
            .arg(policy.authorizationRejections)
            .arg(policy.heartbeatIntervalMs)
            .arg(policy.heartbeatMissLimit)
            .arg(policy.heartbeatMissed)
            .arg(policy.lastHeartbeatAgeMs)
            .arg(policy.heartbeatsSent)
            .arg(policy.heartbeatsReceived)
            .arg(policy.heartbeatAcksReceived));
        if (serverSide) {
            const ControlServer::Stats stats = sessionController_.serverStats();
            appendLog(QStringLiteral(
                "control_stats side=server accepted=%1 pending_cap_rejects=%2 auth_rate_limit_rejects=%3 "
                "session_cap_rejects=%4 auth_rejects=%5 auth_timeouts=%6 frame_rejects=%7 "
                "frame_timeouts=%8 tag_or_sequence_rejects=%9 output_rejects=%10 input_high_water=%11 output_high_water=%12 "
                "active_connections=%13 active_connection_high_water=%14 disconnected_connections=%15 "
                "pre_authentication_disconnects=%16 large_json_sent=%17 large_json_received=%18 "
                "large_json_raw_bytes_sent=%19 large_json_compressed_bytes_sent=%20")
                .arg(stats.acceptedConnections)
                .arg(stats.pendingCapRejects)
                .arg(stats.authenticationRateLimitRejects)
                .arg(stats.authenticatedCapRejects)
                .arg(stats.authenticationRejects)
                .arg(stats.authenticationTimeouts)
                .arg(stats.frameRejects)
                .arg(stats.frameTimeouts)
                .arg(stats.sequenceOrTagRejects)
                .arg(stats.outputHighWaterRejects)

                .arg(stats.maxBufferedInputBytes)
                .arg(stats.maxQueuedOutputBytes)
                .arg(stats.activeConnections)
                .arg(stats.activeConnectionHighWater)
                .arg(stats.disconnectedConnections)
                .arg(stats.preAuthenticationDisconnects)
                .arg(stats.largeJsonMessagesSent)
                .arg(stats.largeJsonMessagesReceived)
                .arg(stats.largeJsonRawBytesSent)
                .arg(stats.largeJsonCompressedBytesSent));
        } else {
            const ControlClient::Stats stats = sessionController_.clientStats();
            appendLog(QStringLiteral(
                "control_stats side=client auth_rejects=%1 auth_timeouts=%2 frame_rejects=%3 frame_timeouts=%4 "
                "tag_or_sequence_rejects=%5 output_rejects=%6 input_high_water=%7 output_high_water=%8 "
                "connection_attempts=%9 completed_connections=%10 disconnected_connections=%11 "
                "large_json_sent=%12 large_json_received=%13 large_json_raw_bytes_sent=%14 "
                "large_json_compressed_bytes_sent=%15")
                .arg(stats.authenticationRejects)
                .arg(stats.authenticationTimeouts)
                .arg(stats.frameRejects)

                .arg(stats.frameTimeouts)
                .arg(stats.sequenceOrTagRejects)
                .arg(stats.outputHighWaterRejects)
                .arg(stats.maxBufferedInputBytes)
                .arg(stats.maxQueuedOutputBytes)
                .arg(stats.connectionAttempts)
                .arg(stats.completedConnections)
                .arg(stats.disconnectedConnections)
                .arg(stats.largeJsonMessagesSent)
                .arg(stats.largeJsonMessagesReceived)
                .arg(stats.largeJsonRawBytesSent)
                .arg(stats.largeJsonCompressedBytesSent));
        }
    }
    if (serverSide && event.type == TransportEventType::Failure &&
        event.failure == jam2::control_protocol::TransportFailure::PreAuthenticationDisconnect) {
        notePreAuthenticationDisconnect();
    }
    if (event.type == TransportEventType::SessionEnded) {
        appendLog(QStringLiteral("jam ended by creator; returning to local mode"));
        QTimer::singleShot(0, this, [this] { stopJam(true); });
    } else if (event.type == TransportEventType::Authenticated) {
        setMixRemotePeerVisible(true);
        if (automaticWavSharingEnabled()) {
            if (serverSide) {
                QTimer::singleShot(0, this, [this] { shareLocalTracks(); });
            } else {
                shareLocalTracks();
            }
        }
    } else if (event.type == TransportEventType::Failure && !serverSide &&
               !event.retryable &&
               sessionController_.snapshot().lifecycle ==
                   SharedSessionController::Lifecycle::Failed) {
        if (startButton_) {
            startButton_->setEnabled(true);
        }
        if (joinButton_) {
            joinButton_->setEnabled(true);
        }
        if (stopButton_) {
            stopButton_->setEnabled(false);
        }
        setMixRemotePeerVisible(false);
        QString detail = pendingJamRuntimeError_.isEmpty()
            ? event.detail : pendingJamRuntimeError_;
        if (event.failure == jam2::control_protocol::TransportFailure::ReconnectExhausted &&
            !detail.contains(QStringLiteral("firewall"), Qt::CaseInsensitive)) {
            detail += jam2::gui::joinerFirewallGuidance();
        }
        showJamFailure(detail);
        QTimer::singleShot(0, this, [this] { stopJam(true); });
    } else if (event.type == TransportEventType::Disconnected) {
        if (serverSide) {
            setMixRemotePeerVisible(sessionController_.hasPeer());
        } else {
            setMixRemotePeerVisible(false);
        }
    }
}

void MainWindow::notePreAuthenticationDisconnect()
{
    if (firewallGuidanceShown_ || shuttingDown_ || !sessionController_.isServer()) {
        return;
    }
    if (!preAuthenticationDisconnectWindow_.isValid() ||
        preAuthenticationDisconnectWindow_.elapsed() > kFirewallGuidanceWindowMs) {
        preAuthenticationDisconnectWindow_.restart();
        preAuthenticationDisconnectCount_ = 0;
    }
    ++preAuthenticationDisconnectCount_;
    if (preAuthenticationDisconnectCount_ < kFirewallGuidanceDisconnectThreshold) {
        return;
    }
    firewallGuidanceShown_ = true;
    QTimer::singleShot(0, this, [this] {
        if (!shuttingDown_ && sessionController_.isServer()) {
            QMessageBox::warning(
                this,
                QStringLiteral("Incoming connection may be blocked"),
                jam2::gui::creatorFirewallGuidance());
        }
    });
}

void MainWindow::refreshControlConnection()
{
    if (sessionController_.isServer() && sessionController_.hasPeer()) {
        appendLog(QStringLiteral("TCP control already connected"));
        controlRefreshAvailable_ = false;
        updateJamSessionHeaderStatus(sessionController_.snapshot());
        return;
    }
    if (!sessionController_.isServer() && sessionController_.isConnected()) {
        appendLog(QStringLiteral("TCP control already connected"));
        controlRefreshAvailable_ = false;
        updateJamSessionHeaderStatus(sessionController_.snapshot());
        return;
    }
    controlRefreshAvailable_ = false;
    setSessionHeaderStatus(
        QStringLiteral("REFRESHING"),
        QStringLiteral("Refreshing connection"),
        {QStringLiteral("Re-establishing the authenticated control connection.")});
    appendLog(QStringLiteral("refreshing TCP control"));
    sessionController_.setReconnectEnabled(true);
    sessionController_.refresh();
}

void MainWindow::applyCreateDefaultsToControls()
{
    sessionRuntimeDraft_.configuration = preferences_.create;
    sessionRuntimeDraft_.audio = preferences_.createAudio();
}

void MainWindow::applyJoinDefaultsToControls()
{
    const JoinPreference& preference = preferences_.join;
    CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    configuration.bindHost = preference.bindHost;
    configuration.port = preference.port;
    configuration.tuning = preference.tuning;
    configuration.runtime = preference.runtime;
    sessionRuntimeDraft_.audio = preferences_.joinAudio();
    sessionRuntimeDraft_.joinProfileName = preference.tuning.profile;
}

void MainWindow::applyPreferencesToControls()
{
    const bool firstApplication = !preferencesInitialized_;
    if (preferences_.logging.folder.trimmed().isEmpty()) {
        preferences_.logging.folder = appReleaseFolderPath(QStringLiteral("logs"));
    }
    preferences_.create.runtime.logStatsFolder = preferences_.logging.folder;
    preferences_.join.runtime.logStatsFolder = preferences_.logging.folder;
    if (preferences_.recording.input.outputFolder.trimmed().isEmpty()) {
        preferences_.recording.input.outputFolder = appReleaseFolderPath(QStringLiteral("captures"));
    }
    if (preferences_.recording.loopback.outputFolder.trimmed().isEmpty()) {
        preferences_.recording.loopback.outputFolder = appReleaseFolderPath(QStringLiteral("captures"));
    }
    applyCreateDefaultsToControls();
    if (metronomeSoundBox_) {
        const QSignalBlocker blocker(metronomeSoundBox_);
        const int index = metronomeSoundBox_->findData(preferences_.metronome.sound);
        metronomeSoundBox_->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (metronomeModeBox_) {
        const int index = metronomeModeBox_->findText(preferences_.metronome.mode);
        metronomeModeBox_->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (metronomeCompensationMaxSpin_)
        metronomeCompensationMaxSpin_->setValue(preferences_.metronome.compensationMaxMs);
    if (metronomeCompensationSmoothingSpin_)
        metronomeCompensationSmoothingSpin_->setValue(preferences_.metronome.compensationSmoothingMs);
    if (metronomeCompensationDeadbandSpin_)
        metronomeCompensationDeadbandSpin_->setValue(preferences_.metronome.compensationDeadbandMs);
    if (metronomeCompensationSlewSpin_)
        metronomeCompensationSlewSpin_->setValue(preferences_.metronome.compensationSlewMsPerSecond);

    // These preferences are startup defaults. Reapplying them after an unrelated
    // settings change must not overwrite the live mixer state.
    if (firstApplication) {
        if (mixSendLevelSlider_) mixSendLevelSlider_->setValue(preferences_.levels.sendDb);
        if (mixMonitorCheck_) mixMonitorCheck_->setChecked(preferences_.levels.monitorInput);
        if (mixMonitorLevelSlider_) mixMonitorLevelSlider_->setValue(preferences_.levels.monitorDb);
        if (metronomeLevelSlider_) metronomeLevelSlider_->setValue(preferences_.levels.metronomeDb);
        if (masterOutputLevelSlider_) masterOutputLevelSlider_->setValue(preferences_.levels.masterDb);
        if (remoteLevelSlider_) remoteLevelSlider_->setValue(preferences_.levels.remotePeerDb);
        if (mixRemotePeerSlider_ && selectedPeerId_ == 0)
            mixRemotePeerSlider_->setValue(preferences_.levels.remotePeerDb);
    }

    if (performanceHome_) {
        performanceHome_->setChordPreviewVisible(
            preferences_.views.performanceChordPreview);
        performanceHome_->setBeatPreviewVisible(
            preferences_.views.performanceBeatPreview);
    }
    if (chordGrid_) chordGrid_->setFocusCurrentBar(preferences_.views.chordFocusCurrentBar);
    if (beatGrid_) beatGrid_->setFocusCurrentBar(preferences_.views.drumFocusCurrentBar);
    (void)chordModel_.setGuitarReference(
        preferences_.views.guitarStrings, preferences_.views.guitarDropTuning);
    looperProject_.setGridLockEnabled(preferences_.views.trackGridLock);
    if (trackGridLockCheck_) trackGridLockCheck_->setChecked(preferences_.views.trackGridLock);
    SharedTrackModel& track = trackController_.model();
    trackController_.setLoopEnabled(preferences_.views.trackLoop);
    track.speed = preferences_.views.trackSpeed;
    track.pitchCents = preferences_.views.trackPitch * 100;
    track.trackGainDb = preferences_.levels.backingTrackDb;
    track.focusEnabled = preferences_.views.focusFrequencyEnabled;
    track.focusPreset = preferences_.views.focusPreset;
    track.focusFrequencyHz = preferences_.views.focusFrequencyHz;
    updateTrackControls();

    if (!preferencesInitialized_) {
        JamSyncPolicy policy;
        policy.trackLanes = preferences_.sync.trackLanes;
        policy.autoShareWavs = preferences_.sync.autoShareWavs;
        policy.globalPlayback = preferences_.sync.globalPlayback;
        policy.generatedIdeas = static_cast<GeneratedIdeaSyncMode>(
            qBound(0, preferences_.sync.generatedIdeas, 3));
        policy.metronomeState = preferences_.sync.metronomeState;
        policy.recordings = preferences_.sync.recordings;
        applyJamSyncPolicy(policy, false);
        preferencesInitialized_ = true;
    }
    sessionRuntimeDraft_.joinProfileName = preferences_.join.tuning.profile;
    applyMetronomeCompensationToRunningJam();
}

void MainWindow::applyNewJamDefaults()
{
    const int numerator = preferences_.general.meterNumerator;
    const int denominator = preferences_.general.meterDenominator;
    const int pulseUnits = preferences_.general.tempoPulseUnits;
    const int division = preferences_.general.clickDivision;
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        LooperBankTiming timing;
        timing.bpm = preferences_.general.bpm;
        timing.beatsPerBar = numerator;
        timing.beatUnit = denominator;
        timing.tempoPulseUnits = pulseUnits;
        timing.division = division;
        timing.playMaskLow = 0;
        timing.playMaskHigh = 0;
        timing.accentMaskLow = 0;
        timing.accentMaskHigh = 0;
        const int steps = jam2::metronome::pattern_step_count(numerator, division);
        for (int step = 0; step < steps; ++step) {
            jam2::metronome::set_mask_enabled(
                timing.playMaskLow, timing.playMaskHigh, step, true);
        }
        jam2::metronome::set_mask_enabled(
            timing.accentMaskLow, timing.accentMaskHigh, 0, true);
        timing.inheritsBankA = bank > 0;
        (void)looperProject_.setTiming(bank, std::move(timing));
    }
    looperProject_.setGridLockEnabled(preferences_.views.trackGridLock);
    SharedTrackModel& track = trackController_.model();
    track.acceptedBpm = preferences_.general.bpm;
    trackController_.setLoopEnabled(preferences_.views.trackLoop);
    track.speed = preferences_.views.trackSpeed;
    track.pitchCents = preferences_.views.trackPitch * 100;
    track.trackGainDb = preferences_.levels.backingTrackDb;
    track.focusEnabled = preferences_.views.focusFrequencyEnabled;
    track.focusPreset = preferences_.views.focusPreset;
    track.focusFrequencyHz = preferences_.views.focusFrequencyHz;
    if (metronomeBpmSpin_) {
        applyMetronomePatternForBank(looperProject_.activeBankIndex(), false);
    }
    updateTrackControls();
}

void MainWindow::initializeStartupWorkflow()
{
    QString startupPage = preferences_.general.startupView;
    if (startupPage == QStringLiteral("drums")) startupPage = QStringLiteral("beats");
    if (startupPage == QStringLiteral("track")) startupPage = QStringLiteral("looper");
    openWorkspace(startupPage);
    if (!preferences_.general.generateIdeaOnStartup || sharedRecordingProtected()) return;

    jam2::practice::ChordIdeaRequest request;
    request.key = preferences_.ideas.key;
    request.styleId = preferences_.ideas.styleId;
    request.profileId = preferences_.ideas.profileId;
    request.parts = static_cast<jam2::practice::PracticeIdeaParts>(
        qBound(0, preferences_.ideas.parts, 2));
    request.targetSectionIndex = 0;
    request.meterId = preferences_.ideas.meterId;
    if (request.meterId.isEmpty()) {
        request.meterId = practiceMeterIdForPattern(bankMetronomePattern(0));
    }
    request.allowMeterOverride = !request.meterId.isEmpty() &&
        !jam2::practice::compatibleMeterIds(request.styleId, request.profileId)
            .contains(request.meterId);
    request.bpm = preferences_.ideas.exactBpm ? preferences_.ideas.bpm : 0;
    request.bars = preferences_.ideas.bars > 0 ? preferences_.ideas.bars : 8;
    request.harmonicComplexity = preferences_.ideas.complexity;
    request.rhythmicComplexity = preferences_.ideas.complexity;
    if (!applyPracticeIdea(request)) {
        appendLog(QStringLiteral("startup idea generation failed"));
        return;
    }
    if (!preferences_.ideas.renderWavsOnStartup) return;
    const QJsonObject renderRequest{
        {QStringLiteral("type"), QStringLiteral("practice.references.render")},
        {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("render_signature"),
            QString::fromLatin1(practiceReferenceRenderSignature())},
        {QStringLiteral("render_chords"), preferences_.ideas.renderChords},
        {QStringLiteral("render_drums"), preferences_.ideas.renderDrums},
        {QStringLiteral("render_melody"), preferences_.ideas.renderMelody},
        {QStringLiteral("render_bass"), preferences_.ideas.renderBass},
        {QStringLiteral("render_support"), preferences_.ideas.renderSupport},
        {QStringLiteral("chord_voicing"), preferences_.ideas.chordVoicing},
        {QStringLiteral("drum_kit"), preferences_.ideas.drumKit},
    };
    handlePracticeReferenceRenderRequest(renderRequest, QString{}, true);
}

void MainWindow::saveCreateDefaults()
{
    preferences_.create = sessionRuntimeDraft_.configuration;
    AudioDevicePreference& audio = preferences_.createAudio();
    audio = sessionRuntimeDraft_.audio;
    jam2::gui::storeSelectedDevicePreference(
        audio, sessionRuntimeDraft_.selectedDeviceId, availableDevices_);
    preferences_.logging.folder = preferences_.create.runtime.logStatsFolder;
    preferences_.join.runtime.logStatsFolder = preferences_.logging.folder;
    UserPreferencesStore::save(preferences_);
}

QString MainWindow::promptJamTasterSourceDisposition()
{
    return promptJamTasterSourceDispositionDialog(this);
}

void MainWindow::saveJoinDefaults()
{
    const CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    JoinPreference& preference = preferences_.join;
    preference.bindHost = configuration.bindHost;
    preference.port = configuration.port;
    preference.tuning = configuration.tuning;
    preference.tuning.profile = sessionRuntimeDraft_.joinProfileName;
    preference.runtime = configuration.runtime;
    AudioDevicePreference& audio = preferences_.joinAudio();
    audio = sessionRuntimeDraft_.audio;
    jam2::gui::storeSelectedDevicePreference(
        audio, sessionRuntimeDraft_.selectedDeviceId, availableDevices_);
    preferences_.logging.folder = preference.runtime.logStatsFolder;
    preferences_.create.runtime.logStatsFolder = preferences_.logging.folder;
    UserPreferencesStore::save(preferences_);
}

bool MainWindow::selectedDeviceSupportsSampleRate(int sampleRate)
{

    if (selectedDeviceId().isEmpty()) {
        return false;
    }
    try {
        const int deviceId = selectedDeviceId().toInt();
        const auto device = std::find_if(availableDevices_.begin(), availableDevices_.end(),
            [deviceId](const auto& item) { return item.id == deviceId; });
        const QString key = device != availableDevices_.end()
            ? jam2::gui::audioDevicePreferenceKey(*device)
            : QString::number(deviceId);
        jam2::audio::DeviceTestResult capabilities;
        if ((jam2_.isRunning() || jam2_.isNetworkRunning()) &&
            deviceCapabilitiesCache_.contains(key)) {
            capabilities = deviceCapabilitiesCache_.value(key);
        } else {
            capabilities = jam2::audio::test_device(deviceId);
            deviceCapabilitiesCache_.insert(key, capabilities);
        }
        const auto rate = std::find(
            jam2::audio::kTestSampleRates.begin(),
            jam2::audio::kTestSampleRates.end(),
            sampleRate);
        if (rate != jam2::audio::kTestSampleRates.end() &&
            capabilities.sample_rate_supported[static_cast<std::size_t>(
                std::distance(jam2::audio::kTestSampleRates.begin(), rate))]) {
            return true;
        }
        appendLog(QStringLiteral(
            "device sample-rate preflight failed: device '%1' (id=%2) does not support %3 Hz")
            .arg(selectedDeviceDescription())
            .arg(selectedDeviceId())
            .arg(sampleRate));
        return false;
    } catch (const std::exception& error) {
        appendLog(QStringLiteral("device sample-rate preflight failed: ") + QString::fromUtf8(error.what()));
        return false;
    }
}

void MainWindow::testDeviceSelection(
    QComboBox* device,
    QPushButton* button,
    QWidget* dialogParent)
{
    if (device == nullptr || device->currentData().toString().isEmpty()) {
        jam2::gui::showAudioDeviceTestMessage(
            dialogParent, QStringLiteral("Select a low-latency audio device first."));
        return;
    }
    bool ok = false;
    const int deviceId = device->currentData().toInt(&ok);
    if (!ok) {
        jam2::gui::showAudioDeviceTestMessage(
            dialogParent, QStringLiteral("The selected device id is invalid."));
        return;
    }
    const auto info = std::find_if(availableDevices_.begin(), availableDevices_.end(),
        [deviceId](const auto& item) { return item.id == deviceId; });
    const QString key = info != availableDevices_.end()
        ? jam2::gui::audioDevicePreferenceKey(*info)
        : QString::number(deviceId);
    if ((jam2_.isRunning() || jam2_.isNetworkRunning()) &&
        deviceCapabilitiesCache_.contains(key)) {
        jam2::gui::showAudioDeviceTestMessage(
            dialogParent,
            jam2::gui::audioDeviceCapabilitiesText(
                deviceCapabilitiesCache_.value(key)));
        return;
    }
    if (button != nullptr) button->setEnabled(false);
    auto result = std::make_shared<std::optional<jam2::audio::DeviceTestResult>>();
    const QPointer<QWidget> parentGuard(dialogParent);
    const QPointer<QPushButton> buttonGuard(button);
    const bool started = startFileWorkerTask(
        [deviceId, result] { *result = jam2::audio::test_device(deviceId); },
        [this, result, key, parentGuard, buttonGuard] {
            if (buttonGuard) buttonGuard->setEnabled(true);
            if (!*result) return;
            deviceCapabilitiesCache_.insert(key, **result);
            if (parentGuard) {
                jam2::gui::showAudioDeviceTestMessage(
                    parentGuard,
                    jam2::gui::audioDeviceCapabilitiesText(**result));
            }
        },
        [parentGuard, buttonGuard](const QString& error) {
            if (buttonGuard) buttonGuard->setEnabled(true);
            if (parentGuard) {
                jam2::gui::showAudioDeviceTestMessage(parentGuard, error);
            }
        });
    if (!started) {
        if (buttonGuard) buttonGuard->setEnabled(true);
        jam2::gui::showAudioDeviceTestMessage(dialogParent,
            QStringLiteral("Device testing is temporarily busy; try again."));
    }
}

QString MainWindow::meshPeerToken()
{
    if (meshPeerToken_.isEmpty()) {
        const auto key = jam2::random_key();
        meshPeerToken_ = keyToHex(key);
    }
    return meshPeerToken_;
}

QString MainWindow::localMeshEndpoint(bool createSession) const
{
    if (createSession && !activePublicEndpoint_.isEmpty()) {
        return activePublicEndpoint_;
    }
    const CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    QString host = createSession && !configuration.publicHost.trimmed().isEmpty()
        ? configuration.publicHost.trimmed()
        : (!configuration.bindHost.trimmed().isEmpty()
            ? configuration.bindHost.trimmed()
            : QStringLiteral("0.0.0.0"));
    return QStringLiteral("%1:%2").arg(host).arg(configuration.port);
}

QString MainWindow::meshBindEndpoint() const
{
    const CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    const QString host = !configuration.bindHost.trimmed().isEmpty()
        ? configuration.bindHost.trimmed()
        : QStringLiteral("0.0.0.0");
    return QStringLiteral("%1:%2").arg(host).arg(configuration.port);
}

void MainWindow::handleMeshPeerAuthenticated(const QString& token, const QJsonObject& message)
{
    if (!sessionController_.isServer() || token.isEmpty()) {
        return;
    }
    const QString tcpPeerHost = normalizedNetworkHost(
        message.value(QStringLiteral("tcp_peer_host")).toString());
    const bool localMachinePeer = isLocalMachineAddress(tcpPeerHost);
    if (localMachinePeer) {
        localMeshPeerTokens_.insert(token);
    } else {
        localMeshPeerTokens_.remove(token);
    }
    appendLog(QStringLiteral("mesh peer TCP source token=%1 host=%2 same_machine=%3")
        .arg(token.left(8), tcpPeerHost, localMachinePeer ? QStringLiteral("yes") : QStringLiteral("no")));
    if (sharedBankLaunch_.active()) {
        cancelSharedBankLaunch(
            true,
            QStringLiteral("peer joined while the bank was preparing; queue the Section again"));
    }
    sendJamSyncPolicy(token);
    if (jamSyncPolicy_.metronomeState) {
        (void)sessionController_.sendTo(token, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("jam.metronome.state.set")},
            {QStringLiteral("enabled"), metronomeTransport_.localRunning()},
        });
    }
    if (jamSyncPolicy_.trackLanes && jam2_.isRunning()) {
        // Publish the current arrangement to the newly authenticated peer. Its
        // prepared audio may attach later without holding global transport.
        sendSongSnapshot();
    }
    if (jam2_.isRunning() &&
        jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Off) {
        const SongSyncScope scope = jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Chords
            ? SongSyncScope::IdeaChords
            : jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Beats
                ? SongSyncScope::IdeaBeats : SongSyncScope::IdeaFull;
        sendSongSnapshot(std::nullopt, scope);
    }
    if (syncedRecordingsEnabled() && trackRecordingWorkflow_.laneArmed()) {
        publishLocalTrackRecordingState(
            localTrackRecordingPhase_,
            localTrackRecordingCountInRemaining_,
            true);
    }
    if (syncedRecordingsEnabled()) {
        if (!activeRecordingGroupStartMessage_.isEmpty()) {
            (void)sendControlTo(token, activeRecordingGroupStartMessage_);
        } else if (!lastRecordingGroupFinishMessage_.isEmpty()) {
            (void)sendControlTo(token, lastRecordingGroupFinishMessage_);
        }
    }
}

bool MainWindow::syncedRecordingsEnabled() const noexcept
{
    return jam2JamSyncAllows(jamSyncPolicy_, JamSyncRoute::Recording);
}

bool MainWindow::laneRecordingIsolationActive() const
{
    const auto isolates = [](const QString& phase) {
        return phase == QStringLiteral("ready") ||
            phase == QStringLiteral("waiting") ||
            phase == QStringLiteral("count-in") ||
            phase == QStringLiteral("recording") ||
            phase == QStringLiteral("complete") ||
            phase == QStringLiteral("finalizing");
    };
    return !activeRecordingGroupId_.isEmpty() ||
        isolates(localTrackRecordingPhase_) ||
        trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning() || pendingGroupTakeCompletion_.has_value();
}

void MainWindow::updateLaneRecordingIsolation()
{
    jam2_.setLaneRecordingIsolationEnabled(laneRecordingIsolationActive());
}

bool MainWindow::deferIncomingControlForLaneRecording(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    if (replayingDeferredRecordingControls_ || !laneRecordingIsolationActive()) {
        return false;
    }
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("looper.recording.state") ||
        type.startsWith(QStringLiteral("looper.recording.group.")) ||
        type.startsWith(QStringLiteral("looper.recording.resync.")) ||
        type.startsWith(QStringLiteral("session.")) ||
        type.startsWith(QStringLiteral("looper.asset."))) {
        return false;
    }
    const bool mutableControl =
        type == QStringLiteral("jam.sync.set") ||
        type == QStringLiteral("jam.sync.request") ||
        type.startsWith(QStringLiteral("jam.metronome.state.")) ||
        type.startsWith(QStringLiteral("bank.")) ||
        type == QStringLiteral("song.set") ||
        type == QStringLiteral("practice.references.render") ||
        type == QStringLiteral("looper.track.share.request") ||
        type == QStringLiteral("looper.track.batch.offer") ||
        type == QStringLiteral("looper.track.batch.complete") ||
        jam2::application::isEditorControlMessageType(type);
    if (!mutableControl) return false;

    QString key = sourcePeerToken + QLatin1Char(':') + type;
    if (type == QStringLiteral("song.set")) {
        key += QLatin1Char(':') + message.value(
            QStringLiteral("sync_scope")).toString(QStringLiteral("tracks"));
    } else if (jam2::application::isEditorControlMessageType(type)) {
        key += QStringLiteral(":%1:%2:%3:%4")
            .arg(message.value(QStringLiteral("section")).toInt())
            .arg(message.value(QStringLiteral("beat")).toInt())
            .arg(message.value(QStringLiteral("lane")).toString())
            .arg(message.value(QStringLiteral("step")).toInt(-1));
    } else if (type.contains(QStringLiteral("batch"))) {
        key += QLatin1Char(':') + message.value(QStringLiteral("batch_id")).toString();
    }
    for (qsizetype index = deferredRecordingControls_.size(); index-- > 0;) {
        if (deferredRecordingControls_.at(index).key == key) {
            deferredRecordingControls_.removeAt(index);
            break;
        }
    }
    constexpr qsizetype maximumDeferredControls = 4096;
    if (deferredRecordingControls_.size() >= maximumDeferredControls) {
        deferredRecordingControls_.removeFirst();
        if (!deferredRecordingControlsOverflowed_) {
            deferredRecordingControlsOverflowed_ = true;
            appendLog(QStringLiteral(
                "recording control hold reached 4096 entries; oldest changes will be replaced and an authoritative resync will follow"));
        }
    }
    deferredRecordingControls_.append(
        DeferredRecordingControl{key, message, sourcePeerToken});
    appendLog(QStringLiteral("held %1 until the local lane take is finalised")
        .arg(type));
    return true;
}

void MainWindow::releaseDeferredRecordingControls()
{
    updateLaneRecordingIsolation();
    if (laneRecordingIsolationActive()) return;

    auto deferred = std::move(deferredRecordingControls_);
    deferredRecordingControls_.clear();
    const bool overflowed = deferredRecordingControlsOverflowed_;
    deferredRecordingControlsOverflowed_ = false;
    QTimer::singleShot(0, this,
        [this, deferred = std::move(deferred), overflowed] {
        replayingDeferredRecordingControls_ = true;
        for (const DeferredRecordingControl& control : deferred) {
            if (sessionController_.onMessage) {
                sessionController_.onMessage(
                    control.sourcePeerToken, control.message);
            }
        }
        replayingDeferredRecordingControls_ = false;
        if (overflowed) {
            appendLog(QStringLiteral(
                "recording control hold overflow recovered through authoritative state reconciliation"));
        }
        applyPendingTrackContributions();
        applyPendingSongIfAssetsReady();
        if (sessionController_.isServer()) {
            sendJamSyncPolicy();
            sendSongSnapshot();
        } else if (jam2_.isNetworkRunning()) {
            sendControl(QJsonObject{
                {QStringLiteral("type"),
                    QStringLiteral("looper.recording.resync.request")},
            });
        }
    });
}

void MainWindow::applyPendingRecordingTransportResync()
{
    if (!pendingRecordingResyncPlaying_.has_value() ||
        laneRecordingIsolationActive()) {
        return;
    }
    if (preparedMixLifecycle_.workerRunning() || !pendingSongSet_.isEmpty() ||
        fileWorkerTasksActive_ > 0) {
        QTimer::singleShot(100, this,
            [this] { applyPendingRecordingTransportResync(); });
        return;
    }
    const bool shouldPlay = *pendingRecordingResyncPlaying_;
    pendingRecordingResyncPlaying_.reset();
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    if (!position.engineAnchored || position.sampleRate <= 0) return;
    if (shouldPlay) {
        trackController_.requestPlayback(true);
        if (trackRecordingWorkflow_.globalTransportPlaying()) {
            appendLog(QStringLiteral(
                "recording resync kept the existing global playback phase"));
            updateTrackPlaybackPresentation();
            return;
        }
        const bool hasPrepared = !preparedMixLifecycle_.active().path.isEmpty() &&
            preparedMixLifecycle_.active().error.isEmpty();
        const bool scheduled = hasPrepared
            ? trackRecordingWorkflow_.restartPrepared(position, true)
            : trackRecordingWorkflow_.restartGlobalTransport(position, true);
        if (!scheduled) {
            appendLog(QStringLiteral(
                "could not rejoin global playback after the local lane take"));
        }
    } else {
        (void)trackRecordingWorkflow_.stopPrepared(
            position.rawCurrentFrame, position.currentFrame, true);
        trackController_.requestPlayback(false);
    }
    updateTrackPlaybackPresentation();
}

bool MainWindow::automaticWavSharingEnabled() const noexcept
{
    return jam2JamSyncAllows(jamSyncPolicy_, JamSyncRoute::AutomaticWav);
}

bool MainWindow::leaderAudioModeActive() const noexcept
{
    if (jam2_.isRunning()) {
        return jam2_.engineSnapshot().metronome_mode ==
            jam2::EngineMetronomeMode::LeaderAudio;
    }
    return metronomeModeBox_ &&
        metronomeModeBox_->currentText() == QStringLiteral("leader-audio");
}

void MainWindow::showJamSyncDialog()
{
    const bool policyLocked = sharedRecordingProtected();
    JamSyncDialog dialog(
        jamSyncPolicy_, policyLocked, leaderAudioModeActive(), this);
    if (dialog.exec() != QDialog::Accepted) return;
    requestJamSyncPolicy(dialog.policy());
}

void MainWindow::updateJamSyncPresentation()
{
    if (jamSyncButton_) {
        const bool anyShared = jamSyncPolicy_.trackLanes || jamSyncPolicy_.autoShareWavs ||
            jamSyncPolicy_.globalPlayback ||
            jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Off ||
            jamSyncPolicy_.metronomeState || syncedRecordingsEnabled();
        jamSyncButton_->setText(anyShared
            ? QStringLiteral("\u25cf  JAM SYNC")
            : QStringLiteral("\u25cb  JAM SYNC"));
    }
    if (trackSharingStatusLabel_) {
        const QString laneState = jamSyncPolicy_.trackLanes
            ? QStringLiteral("SYNCED") : QStringLiteral("LOCAL");
        const QString wavState = automaticWavSharingEnabled()
            ? QStringLiteral("AUTOMATIC")
            : jamSyncPolicy_.autoShareWavs
                ? QStringLiteral("PAUSED") : QStringLiteral("MANUAL");
        trackSharingStatusLabel_->setText(
            QStringLiteral("LANES: %1  \u00b7  WAVS: %2").arg(laneState, wavState));
    }
    if (shareTracksButton_) {
        const bool protectedState = sharedRecordingProtected();
        shareTracksButton_->setEnabled(!protectedState && !automaticWavSharingEnabled());
        shareTracksButton_->setToolTip(automaticWavSharingEnabled()
            ? QStringLiteral("WAVs are already shared automatically with the jam")
            : QStringLiteral("Manually share the current tracks with the jam"));
    }
    updateSectionTrimControls();
}

void MainWindow::applyJamSyncPolicy(JamSyncPolicy policy, bool fromJam)
{
    policy = jam2NormalizeJamSyncPolicy(policy);
    const JamSyncPolicy previous = jamSyncPolicy_;
    const bool recordingSyncWasEnabled = syncedRecordingsEnabled();
    if (recordingSyncWasEnabled &&
        !(policy.recordings && policy.trackLanes && policy.globalPlayback)) {
        publishLocalTrackRecordingState(QStringLiteral("idle"), 0, true);
    }

    applyingJamSyncPolicy_ = true;
    jamSyncPolicy_ = policy;
    looperProject_.setTrackSyncEnabled(policy.trackLanes);
    trackController_.model().syncControls = policy.globalPlayback;
    jam2_.setTrackSyncEnabled(policy.globalPlayback);
    jam2_.setRecordingSyncEnabled(
        policy.recordings && policy.trackLanes && policy.globalPlayback);

    if (previous.trackLanes && !policy.trackLanes) {
        ++songAssetCheckRevision_;
        deferredSongSetMessage_ = {};
        deferredSongSetSourcePeerToken_.clear();
        songAssetCheckRetryTimer_.stop();
        pendingSongSet_ = {};
        pendingSongRevision_ = 0;
        trackWorkspace_.pendingSongBaseRevision = 0;
        pendingSongTrackRestart_ = false;
        pendingSongSourcePeerToken_.clear();
        pendingSongNeedsAuthoritativePublish_ = false;
        pendingLooperAssetHashes_.clear();
        if (incomingAssetWorkflow_ == IncomingAssetWorkflow::Arrangement) {
            assetTransfer_.cancel();
            incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
            incomingAssetHash_.clear();
            incomingAssetSourcePeerToken_.clear();
        }
    }
    if (previous.globalPlayback && !policy.globalPlayback) {
        cancelSharedBankLaunch(sessionController_.isServer(),
            QStringLiteral("global playback sync was disabled"));
        trackController_.requestPlayback(
            trackRecordingWorkflow_.globalTransportRequestedPlaying());
        trackController_.observeEnginePlaying(
            trackRecordingWorkflow_.globalTransportPlaying());
    }
    if (recordingSyncWasEnabled && !syncedRecordingsEnabled()) {
        peerTrackRecordingStates_.clear();
        peerTrackRecordingRevisions_.clear();
        localTrackRecordingPhase_ = trackRecordingWorkflow_.laneArmed()
            ? QStringLiteral("armed") : QStringLiteral("idle");
    }
    applyingJamSyncPolicy_ = false;

    updateJamSyncPresentation();
    updateTrackControls();
    updateTrackPlaybackPresentation();
    updateSharedRecordingPresentation();

    const bool publishLanes = jam2_.isNetworkRunning() &&
        policy.trackLanes && !previous.trackLanes;
    const bool publishWavs = jam2_.isNetworkRunning() &&
        automaticWavSharingEnabled() &&
        (!previous.autoShareWavs || !previous.trackLanes);
    if (publishLanes || publishWavs) {
        // Let the authoritative jam.sync.set frame enter the control stream
        // before any newly-enabled content path starts publishing.
        QTimer::singleShot(0, this, [this, publishLanes, publishWavs] {
            if (publishLanes && jamSyncPolicy_.trackLanes) sendSongSnapshot();
            if (publishWavs && automaticWavSharingEnabled()) shareLocalTracks();
        });
    }
    appendLog(QStringLiteral("jam sync policy %1: lanes=%2 wavs=%3 playback=%4 ideas=%5 metronome=%6 recordings=%7")
        .arg(fromJam ? QStringLiteral("applied") : QStringLiteral("changed"))
        .arg(policy.trackLanes ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(policy.autoShareWavs ? QStringLiteral("auto") : QStringLiteral("manual"))
        .arg(policy.globalPlayback ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(policy.generatedIdeas == GeneratedIdeaSyncMode::Full ? QStringLiteral("full") :
            policy.generatedIdeas == GeneratedIdeaSyncMode::Chords ? QStringLiteral("chords") :
            policy.generatedIdeas == GeneratedIdeaSyncMode::Beats ? QStringLiteral("beats") :
            QStringLiteral("off"))
        .arg(policy.metronomeState ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(policy.recordings ? QStringLiteral("on") : QStringLiteral("off")));
}

void MainWindow::requestJamSyncPolicy(JamSyncPolicy policy)
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("jam sync policy change ignored while a shared recording is active"));
        updateJamSyncPresentation();
        return;
    }
    policy = jam2NormalizeJamSyncPolicy(policy);
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (session.role == SharedSessionController::Role::Joiner) {
        sendControl(jam2JamSyncPolicyMessage(QStringLiteral("jam.sync.request"), policy));
        updateJamSyncPresentation();
        return;
    }
    JamSyncPolicyState state(jamSyncPolicy_);
    policy = state.order(policy);
    const bool beginMetronomeSync = policy.metronomeState &&
        !jamSyncPolicy_.metronomeState;
    applyJamSyncPolicy(policy, false);
    if (session.role == SharedSessionController::Role::Creator) {
        sendJamSyncPolicy();
        if (beginMetronomeSync) {
            sendMetronomeStateToJam(metronomeTransport_.localRunning());
        }
    }
}

void MainWindow::sendJamSyncPolicy(const QString& targetPeerToken)
{
    if (!sessionController_.isServer()) return;
    const QJsonObject message = jam2JamSyncPolicyMessage(
        QStringLiteral("jam.sync.set"), jamSyncPolicy_);
    if (targetPeerToken.isEmpty()) {
        (void)sessionController_.send(message);
    } else {
        (void)sessionController_.sendTo(targetPeerToken, message);
    }
}

void MainWindow::handleJamSyncMessage(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("jam.sync.request") && !sessionController_.isServer()) return;
    if (type == QStringLiteral("jam.sync.set") && sessionController_.isServer()) return;
    JamSyncPolicy policy;
    QString error;
    if (!jam2ParseJamSyncPolicyMessage(message, policy, error)) {
        appendLog(QStringLiteral("ignored invalid jam sync policy: ") + error);
        return;
    }
    if (type == QStringLiteral("jam.sync.request")) {
        if (sharedRecordingProtected()) {
            sendJamSyncPolicy(sourcePeerToken);
            return;
        }
        const bool beginMetronomeSync = policy.metronomeState &&
            !jamSyncPolicy_.metronomeState;
        JamSyncPolicyState state(jamSyncPolicy_);
        policy = state.order(policy);
        applyJamSyncPolicy(policy, true);
        sendJamSyncPolicy();
        if (beginMetronomeSync) {
            sendMetronomeStateToJam(metronomeTransport_.localRunning());
        }
        return;
    }
    JamSyncPolicyState state(jamSyncPolicy_);
    if (!state.adopt(policy)) return;
    policy = state.policy();
    applyJamSyncPolicy(policy, true);
}

bool MainWindow::jamSyncAllowsControlMessage(const QJsonObject& message) const
{
    return jam2JamSyncAllowsControlMessage(jamSyncPolicy_, message);
}

void MainWindow::sendControl(const QJsonObject& message)
{
    if (!jamSyncAllowsControlMessage(message)) {
        appendLog(QStringLiteral("suppressed control message by the jam sync policy: %1")
            .arg(message.value(QStringLiteral("type")).toString()));
        return;
    }
    sessionController_.send(message);
}

void MainWindow::updateRuntimeControls()
{
    // The performance recorder belongs to the local audio runtime, not to a
    // network session. Refresh it with the rest of the runtime controls so a
    // solo performer can record input and backing/arrangement playback as soon
    // as the local engine is ready.
    updateJamRecordingControls();
    submitEngineToggle(
        jam2::EngineCommandType::SetMetronomeTransportGated,
        true,
        QStringLiteral("metronome global transport gate"));
    if (!jam2_.isRunning()) {
        return;
    }
    const double metronomeLevel = gainFromDb(static_cast<double>(metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10));
    const double remoteLevel = gainFromDb(static_cast<double>(remoteLevelSlider_ ? remoteLevelSlider_->value() : 0));
    const double outputLevel = gainFromDb(static_cast<double>(
        masterOutputLevelSlider_ ? masterOutputLevelSlider_->value() : 0));
    submitEngineGain(jam2::EngineCommandType::SetMetronomeLevel, metronomeLevel, QStringLiteral("metronome level"));
    submitEngineGain(jam2::EngineCommandType::SetRemoteLevel, remoteLevel, QStringLiteral("remote level"));
    submitEngineGain(
        jam2::EngineCommandType::SetOutputLevel,
        outputLevel,
        QStringLiteral("master output level"));
    const double sendLevel = gainFromDb(static_cast<double>(mixSendLevelSlider_ ? mixSendLevelSlider_->value() : 0));
    const double monitorLevel = gainFromDb(static_cast<double>(mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : 0));
    submitEngineGain(jam2::EngineCommandType::SetSendLevel, sendLevel, QStringLiteral("send level"));
    submitEngineToggle(
        jam2::EngineCommandType::SetLocalMonitorEnabled,
        mixMonitorCheck_ && mixMonitorCheck_->isChecked(),
        QStringLiteral("local monitor"));
    submitEngineGain(jam2::EngineCommandType::SetLocalMonitorLevel, monitorLevel, QStringLiteral("monitor level"));
}

void MainWindow::auditWavCompatibilityForSession(int expectedSampleRate, bool showModal)
{
    if (expectedSampleRate <= 0) {
        return;
    }
    if (wavCompatibilityAuditRunning_) {
        pendingWavCompatibilityAuditRate_ = expectedSampleRate;
        return;
    }

    struct Input {
        int bank = -1;
        QString laneId;
        QString name;
        QString path;
        bool track = false;
    };
    struct Result {
        Input input;
        int sampleRate = 0;
        qint64 sourceFrames = 0;
        bool conversionAttempted = false;
        StagedPcm16Asset converted;
        QString error;
    };
    auto inputs = std::make_shared<QVector<Input>>();
    QStringList signatureParts{QString::number(expectedSampleRate)};
    for (int bankIndex = 0; bankIndex < looperProject_.banks().size(); ++bankIndex) {
        for (LooperLane& lane : looperProject_.banks()[bankIndex].lanes) {
            if (lane.assetPath.trimmed().isEmpty()) {
                lane.sampleRateCompatible = true;
                continue;
            }
            const QString path = looperAssetAbsolutePath(lane);
            inputs->append(Input{bankIndex, lane.id, lane.name, path, false});
            signatureParts.append(QStringLiteral("lane:%1:%2").arg(lane.id, QDir::cleanPath(path)));
        }
    }
    SharedTrackModel& track = trackController_.model();
    if (track.userProvidedSource && !track.filePath.trimmed().isEmpty()) {
        inputs->append(Input{-1, {}, track.fileName, track.filePath, true});
        signatureParts.append(QStringLiteral("track:%1").arg(QDir::cleanPath(track.filePath)));
    } else {
        track.sampleRateCompatible = true;
    }
    signatureParts.sort();
    const QString auditSignature = signatureParts.join(QLatin1Char('|'));
    refreshLooperLanes();
    updateTrackControls();
    if (inputs->isEmpty()) {
        lastWavCompatibilityAuditSignature_ = auditSignature;
        return;
    }
    if (auditSignature == lastWavCompatibilityAuditSignature_) {
        return;
    }

    auto results = std::make_shared<QVector<Result>>();
    results->reserve(inputs->size());
    const auto cleanupCreatedResults = [this, results] {
        QSet<QString> paths;
        for (const Result& result : std::as_const(*results)) {
            if (result.converted.stagedFileCreated &&
                !result.converted.stagedPath.isEmpty()) {
                paths.insert(result.converted.stagedPath);
            }
        }
        for (const QString& path : std::as_const(paths)) {
            projectPersistence_.registerTransientWav(path);
        }
        discardObsoleteReferenceWavs(paths);
    };
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    wavCompatibilityAuditRunning_ = true;
    const std::uint64_t generation = ++wavCompatibilityAuditGeneration_;
    const bool started = startFileWorkerTask(
        [inputs, results, stagingFolder, expectedSampleRate] {
            for (const Input& input : *inputs) {
                Result result;
                result.input = input;
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(input.path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                if (!inspected) {
                    result.error = QString::fromStdString(inspected.error);
                } else {
                    result.sampleRate = static_cast<int>(inspected.info.sample_rate);
                    result.sourceFrames = static_cast<qint64>(inspected.info.frames);
                    if (result.sampleRate != expectedSampleRate) {
                        result.conversionAttempted = true;
                        result.converted = stagePcm16Asset(
                            input.path, stagingFolder, expectedSampleRate);
                        if (!result.converted.error.isEmpty()) {
                            result.error = result.converted.error;
                        } else {
                            result.sampleRate = result.converted.metadata.sampleRate;
                            result.sourceFrames = result.converted.metadata.frames;
                        }
                    }
                }
                results->append(std::move(result));
            }
        },
        [this, expectedSampleRate, showModal, results, auditSignature,
         generation, cleanupCreatedResults] {
            wavCompatibilityAuditRunning_ = false;
            if (generation != wavCompatibilityAuditGeneration_) {
                cleanupCreatedResults();
                return;
            }
            if (pendingWavCompatibilityAuditRate_ > 0 &&
                pendingWavCompatibilityAuditRate_ != expectedSampleRate) {
                const int pending = pendingWavCompatibilityAuditRate_;
                pendingWavCompatibilityAuditRate_ = 0;
                cleanupCreatedResults();
                auditWavCompatibilityForSession(pending, showModal);
                return;
            }
            QStringList resamplingFailures;
            QStringList temporarilyUnavailable;
            QSet<QString> adoptedConvertedPaths;
            bool arrangementConverted = false;
            for (const Result& result : *results) {
                bool stillPresent = false;
                if (result.input.track) {
                    SharedTrackModel& current = trackController_.model();
                    stillPresent = current.filePath == result.input.path;
                    if (stillPresent && result.error.isEmpty()) {
                        SharedTrackModel updated = current;
                        if (result.converted.resampled) {
                            updated.filePath = result.converted.stagedPath;
                            updated.fileBytes = QFileInfo(
                                result.converted.stagedPath).size();
                            updated.durationMs = result.converted.metadata.durationMs;
                            updated.sha256 = result.converted.sha256;
                            appendLog(QStringLiteral(
                                "session WAV resampled %1: source_rate=%2 target_rate=%3")
                                .arg(result.input.name)
                                .arg(result.converted.sourceSampleRate)
                                .arg(result.converted.metadata.sampleRate));
                        }
                        updated.sampleRate = result.sampleRate;
                        updated.sampleRateCompatible = true;
                        trackController_.replaceModel(std::move(updated));
                        if (result.converted.resampled) {
                            adoptedConvertedPaths.insert(
                                result.converted.stagedPath);
                            registerTransientTrackWav(
                                result.converted.stagedPath);
                            if (QDir::cleanPath(result.input.path) !=
                                QDir::cleanPath(result.converted.stagedPath)) {
                                discardObsoleteReferenceWavs(
                                    QSet<QString>{result.input.path});
                            }
                        }
                    } else if (stillPresent && result.conversionAttempted) {
                        current.sampleRateCompatible = false;
                    }
                } else if (result.input.bank >= 0 &&
                           result.input.bank < looperProject_.banks().size()) {
                    const QVector<LooperLane>& lanes =
                        looperProject_.banks().at(result.input.bank).lanes;
                    for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
                        const LooperLane& current = lanes.at(laneIndex);
                        if (current.id == result.input.laneId &&
                            looperAssetAbsolutePath(current) == result.input.path) {
                            stillPresent = true;
                            LooperLane updated = current;
                            const QString oldHash = current.assetHash;
                            if (result.error.isEmpty()) {
                                if (result.converted.resampled) {
                                    updated.assetPath = result.converted.stagedPath;
                                    updated.assetHash = result.converted.sha256;
                                    appendLog(QStringLiteral(
                                        "session lane resampled %1: source_rate=%2 target_rate=%3")
                                        .arg(result.input.name)
                                        .arg(result.converted.sourceSampleRate)
                                        .arg(result.converted.metadata.sampleRate));
                                }
                                updated.sampleRate = result.sampleRate;
                                updated.sourceFrames = result.sourceFrames;
                                updated.sampleRateCompatible = true;
                            } else if (result.conversionAttempted) {
                                updated.sampleRateCompatible = false;
                            }
                            if (!looperProject_.replaceLane(
                                    result.input.bank,
                                    laneIndex,
                                    std::move(updated))) {
                                temporarilyUnavailable.append(QStringLiteral(
                                    "%1: checked lane update rejected")
                                    .arg(result.input.name));
                                break;
                            }
                            if (result.error.isEmpty() &&
                                result.converted.resampled) {
                                adoptedConvertedPaths.insert(
                                    result.converted.stagedPath);
                                registerTransientTrackWav(
                                    result.converted.stagedPath);
                                looperWaveformCache_.remove(result.input.path);
                                validatedTrackAssetHashes_.remove(oldHash);
                                validatedTrackAssetHashes_.insert(
                                    result.converted.sha256);
                                cancelUnreferencedLooperAssetTransfer(oldHash);
                                if (QDir::cleanPath(result.input.path) !=
                                    QDir::cleanPath(result.converted.stagedPath)) {
                                    discardObsoleteReferenceWavs(
                                        QSet<QString>{result.input.path});
                                }
                                arrangementConverted = true;
                            }
                            break;
                        }
                    }
                }
                if (!stillPresent) {
                    continue;
                }
                if (!result.error.isEmpty()) {
                    if (result.conversionAttempted) {
                        const QString identity = result.input.path + QLatin1Char('|') +
                            QString::number(expectedSampleRate) + QLatin1Char('|') +
                            result.error;
                        if (!reportedIncompatibleWavs_.contains(identity) &&
                            resamplingFailures.size() < 8) {
                            reportedIncompatibleWavs_.insert(identity);
                            resamplingFailures.append(QStringLiteral("%1: %2")
                                .arg(result.input.name, result.error));
                        }
                    } else if (temporarilyUnavailable.size() < 8) {
                        temporarilyUnavailable.append(
                            QStringLiteral("%1: %2")
                                .arg(result.input.name, result.error));
                    }
                    continue;
                }
            }
            QSet<QString> unadoptedCreatedPaths;
            for (const Result& result : std::as_const(*results)) {
                if (result.converted.stagedFileCreated &&
                    !result.converted.stagedPath.isEmpty() &&
                    !adoptedConvertedPaths.contains(
                        result.converted.stagedPath)) {
                    unadoptedCreatedPaths.insert(
                        result.converted.stagedPath);
                }
            }
            for (const QString& path : std::as_const(unadoptedCreatedPaths)) {
                projectPersistence_.registerTransientWav(path);
            }
            discardObsoleteReferenceWavs(unadoptedCreatedPaths);
            while (reportedIncompatibleWavs_.size() > 256) {
                reportedIncompatibleWavs_.erase(reportedIncompatibleWavs_.begin());
            }
            if (temporarilyUnavailable.isEmpty()) {
                lastWavCompatibilityAuditSignature_ = auditSignature;
            } else {
                appendLog(QStringLiteral(
                    "WAV compatibility audit left transient inspection failures unchanged: %1")
                    .arg(temporarilyUnavailable.join(QStringLiteral("; "))));
            }
            refreshLooperLanes();
            updateTrackControls();
            regeneratePreparedMix();
            if (arrangementConverted && jam2_.isNetworkRunning()) {
                syncLooperArrangement();
            }
            if (showModal && !resamplingFailures.isEmpty()) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("WAV Resampling Failed"),
                    QStringLiteral(
                        "These WAVs could not be converted to the jam sample rate "
                        "and will remain unavailable until replaced:\n\n") +
                        resamplingFailures.join(QStringLiteral("\n")));
            }
            if (pendingWavCompatibilityAuditRate_ > 0) {
                const int pending = pendingWavCompatibilityAuditRate_;
                pendingWavCompatibilityAuditRate_ = 0;
                auditWavCompatibilityForSession(pending, showModal);
            } else if (automaticWavSharingEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        },
        [this, cleanupCreatedResults](const QString& error) {
            wavCompatibilityAuditRunning_ = false;
            cleanupCreatedResults();
            appendLog(QStringLiteral("WAV compatibility audit failed: ") + error);
        });
    if (!started) {
        wavCompatibilityAuditRunning_ = false;
        appendLog(QStringLiteral("WAV compatibility audit could not be queued"));
    }
}

void MainWindow::selectViewedBank(int bankIndex)
{
    viewedBankIndex_ = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    selectedLooperLane_ = -1;
    if (chordGrid_) chordGrid_->setSelectedSectionIndex(viewedBankIndex_);
    if (beatGrid_) beatGrid_->setSelectedSectionIndex(viewedBankIndex_);
    if (lyricGrid_) lyricGrid_->setSelectedSectionIndex(viewedBankIndex_);
    refreshLooperLanes();
}

void MainWindow::addSongSection()
{
    if (sharedRecordingProtected() ||
        chordModel_.sections().size() >=
            jam2::application::limits::kMaximumSongSections) {
        return;
    }
    const int previousCount = chordModel_.sections().size();
    chordModel_.addSection();
    if (chordModel_.sections().size() != previousCount + 1 ||
        !looperProject_.addBank()) {
        if (chordModel_.sections().size() > previousCount) {
            chordModel_.deleteSection(chordModel_.sections().size() - 1);
        }
        return;
    }
    const int added = chordModel_.sections().size() - 1;
    preparedMixLifecycle_.clearBank(added);
    discardObsoletePreparedMixPaths();
    selectViewedBank(added);
    refreshBankPresentation();
    syncLooperArrangement();
    appendLog(QStringLiteral("added Section %1; section_count=%2")
        .arg(QChar(QLatin1Char('A').unicode() + added))
        .arg(chordModel_.sections().size()));
}

void MainWindow::removeLastSongSection()
{
    const int count = chordModel_.sections().size();
    if (sharedRecordingProtected() ||
        count <= jam2::application::limits::kMinimumSongSections ||
        looperProject_.banks().size() != count) {
        return;
    }
    const int removed = count - 1;
    const SongSection& section = chordModel_.section(removed);
    const QString expectedLabel(
        QChar(QLatin1Char('A').unicode() + removed));
    const bool pristineMetadata = section.generatedKind.isEmpty() &&
        section.label == expectedLabel &&
        section.name == QStringLiteral("Section %1").arg(expectedLabel);
    const bool arrangementReferencesSection = std::any_of(
        looperProject_.arrangement().steps.cbegin(),
        looperProject_.arrangement().steps.cend(),
        [removed](const ArrangementStep& step) {
            return step.bankIndex == removed;
        });
    const bool empty = pristineMetadata &&
        chordModel_.occupiedBeatCount(removed) == 0 &&
        looperProject_.banks().at(removed).lanes.isEmpty() &&
        !arrangementReferencesSection;
    if (!empty && QMessageBox::warning(
            this,
            QStringLiteral("Remove Section %1").arg(expectedLabel),
            QStringLiteral(
                "Section %1 contains notes, generated metadata, or tracks. "
                "Remove it and all of that section's content?").arg(expectedLabel),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    stopArrangement();
    ArrangementDefinition arrangement = looperProject_.arrangement();
    arrangement.steps.erase(
        std::remove_if(
            arrangement.steps.begin(), arrangement.steps.end(),
            [removed](const ArrangementStep& step) {
                return step.bankIndex == removed;
            }),
        arrangement.steps.end());
    if (!looperProject_.setArrangement(std::move(arrangement)) ||
        !looperProject_.removeLastBank()) {
        return;
    }
    chordModel_.deleteSection(removed);
    preparedMixLifecycle_.invalidateBank(removed);
    discardObsoletePreparedMixPaths();
    if (viewedBankIndex_ >= removed) {
        selectViewedBank(removed - 1);
    } else {
        refreshLooperLanes();
    }
    refreshBankPresentation();
    syncLooperArrangement();
    appendLog(QStringLiteral("removed Section %1; section_count=%2")
        .arg(expectedLabel)
        .arg(chordModel_.sections().size()));
}

void MainWindow::refreshBankPresentation()
{
    const int live = looperProject_.activeBankIndex();
    const int bankCount = looperProject_.banks().size();
    for (QPushButton* button : std::as_const(bankViewButtons_)) {
        if (!button) continue;
        const int bank = button->property("bankIndex").toInt();
        button->setVisible(bank < bankCount);
        if (bank >= bankCount) continue;
        button->setChecked(bank == viewedBankIndex_);
        QString suffix;
        if (bank == live) suffix = QStringLiteral("  LIVE");
        if (bank == pendingBankIndex_) suffix = QStringLiteral("  NEXT");
        button->setToolTip(QStringLiteral("Section %1%2")
            .arg(QChar(QLatin1Char('A').unicode() + bank), suffix));
        button->setStyleSheet(bank == viewedBankIndex_
            ? QStringLiteral("QPushButton { background:#502d5d;color:#fff8ea;border:1px solid #e8a44a;padding:4px; }")
            : bank == live
                ? QStringLiteral("QPushButton { background:#214b43;color:#f2fff9;border:1px solid #68d6bc;padding:4px; }")
                : QStringLiteral("QPushButton { background:#12141f;color:#c4bacb;border:1px solid #55465f;padding:4px; }"));
    }
    for (int bank = 0; bank < static_cast<int>(looperBankButtons_.size()); ++bank) {
        QPushButton* button = looperBankButtons_[bank];
        if (!button) continue;
        button->setVisible(bank < bankCount);
        if (bank >= bankCount) continue;
        button->setChecked(bank == viewedBankIndex_);
        QString suffix;
        if (bank == live) suffix = QStringLiteral("  LIVE");
        if (bank == pendingBankIndex_) suffix = QStringLiteral("  NEXT");
        button->setToolTip(QStringLiteral("Section %1%2")
            .arg(QChar(QLatin1Char('A').unicode() + bank), suffix));
        button->setStyleSheet(bank == viewedBankIndex_
            ? QStringLiteral("QPushButton { background:#502d5d;color:#fff8ea;border:1px solid #e8a44a;padding:4px; }")
            : bank == live
                ? QStringLiteral("QPushButton { background:#214b43;color:#f2fff9;border:1px solid #68d6bc;padding:4px; }")
                : QStringLiteral("QPushButton { background:#12141f;color:#c4bacb;border:1px solid #55465f;padding:4px; }"));
    }
    for (QPushButton* button : std::as_const(sectionAddButtons_)) {
        if (button) button->setEnabled(
            !sharedRecordingProtected() &&
            bankCount < jam2::application::limits::kMaximumSongSections);
    }
    for (QPushButton* button : std::as_const(sectionRemoveButtons_)) {
        if (button) button->setEnabled(
            !sharedRecordingProtected() &&
            bankCount > jam2::application::limits::kMinimumSongSections);
    }
    if (launchBankButton_) {
        const bool viewingLiveSection = viewedBankIndex_ == live;
        launchBankButton_->setEnabled(
            !sharedRecordingProtected() && !viewingLiveSection);
        launchBankButton_->setToolTip(viewingLiveSection
            ? QStringLiteral("This Section is already playing")
            : QStringLiteral("Queue the viewed Section for the next boundary"));
    }
    if (detailPositionLabel_) {
        const int currentWorkspace = workspaceStack_ ? workspaceStack_->currentIndex() : -1;
        const bool sectionNamedWorkspace = currentWorkspace == workspacePages_.value(QStringLiteral("chords"), -2) ||
            currentWorkspace == workspacePages_.value(QStringLiteral("beats"), -3) ||
            currentWorkspace == workspacePages_.value(QStringLiteral("lyrics"), -4) ||
            currentWorkspace == workspacePages_.value(QStringLiteral("looper"), -5);
        if (sectionNamedWorkspace && viewedBankIndex_ >= 0 && viewedBankIndex_ < chordModel_.sections().size()) {
            if (detailPositionLabel_->isReadOnly()) {
                const QString name = chordModel_.section(viewedBankIndex_).name.trimmed();
                detailPositionLabel_->setText(name.isEmpty()
                    ? QStringLiteral("Section %1").arg(
                        QChar(QLatin1Char('A').unicode() + viewedBankIndex_))
                    : name);
                detailPositionLabel_->setCursorPosition(0);
                detailPositionLabel_->deselect();
            }
        }
    }
    if (arrangementButton_) {
        arrangementButton_->setText(arrangementRunning_
            ? QStringLiteral("Arrangement On...")
            : arrangementArmed_ ? QStringLiteral("Arrangement Armed...")
                                : QStringLiteral("Arrangement..."));
    }
    updateSectionTrimControls();
}

void MainWindow::requestBankLaunch(int bankIndex)
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("section queue ignored while a synced recording is active"));
        return;
    }
    launchBank(qBound(0, bankIndex, looperProject_.banks().size() - 1), true);
}

void MainWindow::launchBank(
    int bankIndex,
    bool manualLaunch,
    std::optional<quint64> targetAbsoluteBeat)
{
    bankIndex = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    if (manualLaunch) {
        stopArrangement();
        selectViewedBank(bankIndex);
    }
    if (pendingBankIndex_ >= 0 && pendingBankAbsoluteBeat_ > 0) {
        appendLog(QStringLiteral("bank launch ignored while bank %1 is already scheduled")
            .arg(QChar(QLatin1Char('A').unicode() + pendingBankIndex_)));
        return;
    }
    if (!jamSyncPolicy_.globalPlayback || !jam2_.isNetworkRunning()) {
        schedulePreparedBankLaunch(bankIndex, targetAbsoluteBeat);
        return;
    }
    if (sessionController_.isServer()) {
        beginSharedBankLaunch(bankIndex, targetAbsoluteBeat);
    } else {
        pendingBankIndex_ = bankIndex;
        pendingBankAbsoluteBeat_ = 0;
        refreshBankPresentation();
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("bank.request")},
            {QStringLiteral("bank"), bankIndex},
            {QStringLiteral("target_abs_beat"), targetAbsoluteBeat.has_value()
                ? QString::number(*targetAbsoluteBeat) : QString{}},
        });
    }
}

void MainWindow::beginSharedBankLaunch(
    int bankIndex,
    std::optional<quint64> targetAbsoluteBeat)
{
    if (!sessionController_.isServer() || !jamSyncPolicy_.globalPlayback) return;
    if (pendingBankIndex_ >= 0 && pendingBankAbsoluteBeat_ > 0) return;
    cancelSharedBankLaunch(true, QStringLiteral("superseded by a newer bank request"));
    QSet<QString> expectedPeers;
    for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it) {
        expectedPeers.insert(it.key());
    }
    const int boundedBank = qBound(
        0, bankIndex, looperProject_.banks().size() - 1);
    const QString switchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!sharedBankLaunch_.beginHost(
            switchId,
            boundedBank,
            targetAbsoluteBeat.value_or(0),
            std::move(expectedPeers),
            meshPeerToken_)) {
        appendLog(QStringLiteral("bank preparation rejected invalid local state"));
        return;
    }
    const auto& launch = sharedBankLaunch_.snapshot();
    pendingBankIndex_ = launch.bankIndex;
    pendingBankAbsoluteBeat_ = 0;
    refreshBankPresentation();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("bank.prepare")},
        {QStringLiteral("switch_id"), launch.switchId},
        {QStringLiteral("bank"), launch.bankIndex},
        {QStringLiteral("target_abs_beat"), launch.requestedTargetBeat > 0
            ? QString::number(launch.requestedTargetBeat) : QString{}},
    });
    prepareSharedBankLaunch(launch.bankIndex, switchId);
    QPointer<MainWindow> self(this);
    QTimer::singleShot(30000, this, [self, switchId] {
        if (self && self->sharedBankLaunch_.snapshot().switchId == switchId) {
            self->cancelSharedBankLaunch(
                true, QStringLiteral("timed out waiting for every peer to prepare the bank"));
        }
    });
}

void MainWindow::prepareSharedBankLaunch(int bankIndex, const QString& switchId)
{
    bankIndex = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    if (switchId.isEmpty()) return;
    if (!sharedBankLaunch_.preparePeer(switchId, bankIndex)) return;
    pendingBankIndex_ = bankIndex;
    pendingBankAbsoluteBeat_ = 0;
    refreshBankPresentation();

    const bool hasSources = PreparedMixRenderer::hasRenderableSources(looperProject_, bankIndex);
    const PreparedMixResult& cached = preparedMixLifecycle_.cache(bankIndex);
    if (!hasSources || (!cached.path.isEmpty() && QFileInfo::exists(cached.path))) {
        noteSharedBankReady(bankIndex);
        return;
    }
    regeneratePreparedMix(bankIndex);
    appendLog(QStringLiteral("bank preparation rendering: bank=%1 switch=%2")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex), switchId.left(8)));
}

void MainWindow::noteSharedBankReady(int bankIndex)
{
    if (!sharedBankLaunch_.matches(
            sharedBankLaunch_.snapshot().switchId, bankIndex)) return;
    if (sessionController_.isServer()) {
        (void)sharedBankLaunch_.markHostReady(bankIndex);
        maybeCommitSharedBankLaunch();
        return;
    }
    const QString switchId = sharedBankLaunch_.snapshot().switchId;
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("bank.ready")},
        {QStringLiteral("switch_id"), switchId},
        {QStringLiteral("bank"), bankIndex},
    });
    appendLog(QStringLiteral("bank ready: bank=%1 switch=%2")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex), switchId.left(8)));
}

void MainWindow::handleSharedBankReady(
    int bankIndex,
    const QString& switchId,
    const QString& sourcePeerToken)
{
    const jam2::gui::SharedBankReadyResult result =
        sharedBankLaunch_.markPeerReady(
            bankIndex, switchId, sourcePeerToken, meshPeerToken_);
    if (result != jam2::gui::SharedBankReadyResult::Accepted &&
        result != jam2::gui::SharedBankReadyResult::Duplicate) {
        appendLog(QStringLiteral("ignored stale or non-member bank readiness"));
        return;
    }
    appendLog(QStringLiteral("bank ready on peer: bank=%1 peer=%2 switch=%3 duplicate=%4")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
        .arg(sourcePeerToken.left(8), switchId.left(8))
        .arg(result == jam2::gui::SharedBankReadyResult::Duplicate
            ? QStringLiteral("yes") : QStringLiteral("no")));
    maybeCommitSharedBankLaunch();
}

void MainWindow::maybeCommitSharedBankLaunch()
{
    if (!sessionController_.isServer() || !sharedBankLaunch_.readyToCommit()) return;

    const jam2::gui::SharedBankLaunchSnapshot launch = sharedBankLaunch_.take();
    const int bankIndex = launch.bankIndex;
    const QString switchId = launch.switchId;
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    const quint64 targetBeat = jam2::gui::sharedBankCommitTargetBeat(
        bankGridTimingDiffers(bankIndex),
        trackRecordingWorkflow_.globalTransportPlaying(),
        position.engineAnchored,
        position.sampleRate,
        position.secondsPerBeat,
        position.absoluteBeat,
        launch.requestedTargetBeat,
        currentMetronomePattern().beats_per_bar);
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("bank.switch")},
        {QStringLiteral("switch_id"), switchId},
        {QStringLiteral("bank"), bankIndex},
        {QStringLiteral("target_abs_beat"), QString::number(targetBeat)},
    });
    schedulePreparedBankLaunch(bankIndex, targetBeat);
    appendLog(QStringLiteral(
        "shared bank prepared: bank=%1 peers=%2 target_abs_beat=%3 switch=%4")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
        .arg(launch.expectedPeerTokens.size())
        .arg(targetBeat)
        .arg(switchId.left(8)));
}

void MainWindow::cancelSharedBankLaunch(bool broadcast, const QString& reason)
{
    if (!sharedBankLaunch_.active()) {
        if (pendingBankAbsoluteBeat_ == 0) {
            pendingBankIndex_ = -1;
            pendingBankRequestedTargetBeat_.reset();
            refreshBankPresentation();
        }
        return;
    }
    const jam2::gui::SharedBankLaunchSnapshot launch = sharedBankLaunch_.take();
    if (broadcast && sessionController_.isServer()) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("bank.cancel")},
            {QStringLiteral("switch_id"), launch.switchId},
            {QStringLiteral("bank"), launch.bankIndex},
        });
    }
    appendLog(QStringLiteral("bank preparation cancelled: bank=%1 switch=%2 reason=%3")
        .arg(QChar(QLatin1Char('A').unicode() + qMax(0, launch.bankIndex)))
        .arg(launch.switchId.left(8), reason));
    pendingBankIndex_ = -1;
    pendingBankAbsoluteBeat_ = 0;
    pendingBankRequestedTargetBeat_.reset();
    refreshBankPresentation();
}

void MainWindow::schedulePreparedBankLaunch(
    int bankIndex,
    std::optional<quint64> targetAbsoluteBeat)
{
    bankIndex = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    const bool hasSources = PreparedMixRenderer::hasRenderableSources(looperProject_, bankIndex);
    const PreparedMixResult& cached = preparedMixLifecycle_.cache(bankIndex);
    if (hasSources && (cached.path.isEmpty() || !QFileInfo::exists(cached.path))) {
        pendingBankIndex_ = bankIndex;
        pendingBankRequestedTargetBeat_ = targetAbsoluteBeat;
        regeneratePreparedMix(bankIndex);
        refreshBankPresentation();
        return;
    }

    const bool timingReset = bankGridTimingDiffers(bankIndex);
    if (timingReset) {
        if (trackRecordingWorkflow_.globalTransportRequestedPlaying() ||
            trackRecordingWorkflow_.globalTransportPlaying()) {
            stopTrackForPracticeIdeaGeneration();
        }
        targetAbsoluteBeat = 0;
    }

    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    if ((targetAbsoluteBeat.has_value() && *targetAbsoluteBeat == 0) ||
        !trackRecordingWorkflow_.globalTransportPlaying() || !position.engineAnchored ||
        position.sampleRate <= 0 || position.secondsPerBeat <= 0.0) {
        looperProject_.setActiveBankIndex(bankIndex);
        applyMetronomePatternForBank(bankIndex);
        pendingBankIndex_ = -1;
        pendingBankAbsoluteBeat_ = 0;
        pendingBankRequestedTargetBeat_.reset();
        if (hasSources) {
            adoptPreparedBankCache(bankIndex);
            loadPreparedMixIntoEngine();
        } else {
            adoptPreparedBankCache(bankIndex);
            if (jam2_.isRunning()) {
                jam2::EngineCommand stop;
                stop.type = jam2::EngineCommandType::PreparedStop;
                stop.frame = position.rawCurrentFrame;
                (void)submitEngineCommand(stop, QStringLiteral("empty bank silence"));
            }
        }
        refreshLooperLanes();
        appendLog(QStringLiteral("bank launch immediate: bank=%1 epoch_unchanged=yes")
            .arg(QChar(QLatin1Char('A').unicode() + bankIndex)));
        return;
    }

    const std::optional<jam2::gui::BankLaunchFrameSchedule> launchSchedule =
        jam2::gui::bankLaunchFrameSchedule(
            position.absoluteBeat,
            position.epochFrame,
            position.sampleRate,
            position.secondsPerBeat,
            position.renderOffsetFrames,
            targetAbsoluteBeat,
            currentMetronomePattern().beats_per_bar);
    if (!launchSchedule.has_value()) {
        appendLog(QStringLiteral("bank launch rejected invalid transport clock"));
        pendingBankIndex_ = -1;
        pendingBankAbsoluteBeat_ = 0;
        pendingBankRequestedTargetBeat_.reset();
        refreshBankPresentation();
        return;
    }
    const quint64 targetBeat = launchSchedule->targetAbsoluteBeat;
    const quint64 musicalFrame = launchSchedule->targetMusicalFrame;
    const quint64 targetFrame = launchSchedule->targetRawFrame;
    if (hasSources) {
        (void)preparedMixLifecycle_.activateCachedBank(
            bankIndex,
            looperProject_.banks().size(),
            QFileInfo::exists(cached.path));
        auto& track = trackController_.model();
        track.fileName = QStringLiteral("Prepared Section %1")
            .arg(QChar(QLatin1Char('A').unicode() + bankIndex));
        track.filePath = cached.path;
        track.fileBytes = cached.fileBytes;
        track.sampleRate = cached.sampleRate;
        track.durationMs = cached.durationMs;
        track.sha256 = cached.sha256;
        loadPreparedMixIntoEngine(targetFrame, 0, true);
    }
    if (!trackRecordingWorkflow_.scheduleBankRestart(
            targetFrame, musicalFrame, hasSources)) {
        appendLog(QStringLiteral("bank launch command queue unavailable"));
        return;
    }
    pendingBankIndex_ = bankIndex;
    pendingBankAbsoluteBeat_ = targetBeat;
    pendingBankRequestedTargetBeat_.reset();
    refreshBankPresentation();
    appendLog(QStringLiteral(
        "bank launch scheduled: bank=%1 target_abs_beat=%2 target_raw_frame=%3 epoch_frame=%4")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
        .arg(targetBeat)
        .arg(targetFrame)
        .arg(position.epochFrame));
}

void MainWindow::applyScheduledBankLaunch()
{
    if (pendingBankIndex_ < 0) return;
    const int bank = pendingBankIndex_;
    looperProject_.setActiveBankIndex(bank);
    applyMetronomePatternForBank(bank);
    adoptPreparedBankCache(bank);
    pendingBankIndex_ = -1;
    arrangementSectionStartBeat_ = pendingBankAbsoluteBeat_;
    pendingBankAbsoluteBeat_ = 0;
    pendingBankRequestedTargetBeat_.reset();
    if (arrangementRunning_ || arrangementArmed_) {
        selectViewedBank(bank);
    } else {
        refreshLooperLanes();
    }
    appendLog(QStringLiteral("bank launch committed: bank=%1 source=1.1 epoch_unchanged=yes")
        .arg(QChar(QLatin1Char('A').unicode() + bank)));
}

void MainWindow::stopArrangement()
{
    looperProject_.setArrangementEnabled(false);
    arrangementRunning_ = false;
    arrangementArmed_ = false;
    arrangementResetBankAfterStop_ = false;
    arrangementStepIndex_ = 0;
    arrangementStepRepeat_ = 0;
    refreshBankPresentation();
}

void MainWindow::updateArrangementPlayback(const PlaybackGrid::Position& position)
{
    const ArrangementDefinition& arrangement = looperProject_.arrangement();
    if (arrangementRunning_ && !trackRecordingWorkflow_.globalTransportRequestedPlaying()) {
        arrangementRunning_ = false;
        arrangementArmed_ = !arrangement.steps.isEmpty();
        arrangementResetBankAfterStop_ = arrangementArmed_;
        arrangementStepIndex_ = 0;
        arrangementStepRepeat_ = 0;
        if (sharedBankLaunch_.active()) {
            cancelSharedBankLaunch(
                sessionController_.isServer(),
                QStringLiteral("global playback stopped"));
        } else {
            pendingBankIndex_ = -1;
            pendingBankAbsoluteBeat_ = 0;
            pendingBankRequestedTargetBeat_.reset();
        }
        refreshBankPresentation();
        return;
    }
    if (arrangementResetBankAfterStop_ &&
        !trackRecordingWorkflow_.globalTransportPlaying()) {
        arrangementResetBankAfterStop_ = false;
        if (arrangementArmed_ && !arrangement.steps.isEmpty()) {
            schedulePreparedBankLaunch(arrangement.steps.front().bankIndex, quint64{0});
        }
    }
    if (pendingBankIndex_ >= 0 && pendingBankAbsoluteBeat_ > 0 &&
        trackRecordingWorkflow_.globalTransportRequestedPlaying() &&
        position.absoluteBeat >= pendingBankAbsoluteBeat_) {
        applyScheduledBankLaunch();
    }
    if (arrangementArmed_ &&
        trackRecordingWorkflow_.globalTransportRequestedPlaying() &&
        trackRecordingWorkflow_.globalTransportPlaying()) {
        arrangementArmed_ = false;
        arrangementRunning_ = true;
        arrangementStepIndex_ = 0;
        arrangementStepRepeat_ = 0;
        arrangementSectionStartBeat_ = position.absoluteBeat;
        refreshBankPresentation();
    } else if (arrangementRunning_ && pendingBankIndex_ < 0 &&
               arrangementStepIndex_ >= 0 && arrangementStepIndex_ < arrangement.steps.size()) {
        const int liveBank = looperProject_.activeBankIndex();
        const quint64 sectionBeats = liveBank < chordModel_.sections().size()
            ? static_cast<quint64>(qMax(1, chordModel_.section(liveBank).beats))
            : static_cast<quint64>(qMax(1, currentMetronomePattern().beats_per_bar));
        const quint64 sectionEndBeat = arrangementSectionStartBeat_ + sectionBeats;
        const quint64 leadBeats = std::min(
            sectionBeats,
            static_cast<quint64>(qMax(1, currentMetronomePattern().beats_per_bar)) * 2ULL);
        const quint64 queueBeat = sectionEndBeat > leadBeats
            ? sectionEndBeat - leadBeats
            : arrangementSectionStartBeat_;
        const ArrangementStep& current = arrangement.steps[arrangementStepIndex_];
        const bool hasAnotherRepeat = arrangementStepRepeat_ + 1 < current.repeats;
        int nextStepIndex = arrangementStepIndex_;
        int nextRepeat = arrangementStepRepeat_ + 1;
        bool hasNextSection = hasAnotherRepeat;
        if (!hasAnotherRepeat) {
            nextRepeat = 0;
            ++nextStepIndex;
            if (nextStepIndex < arrangement.steps.size()) {
                hasNextSection = true;
            } else if (arrangement.loop) {
                nextStepIndex = 0;
                hasNextSection = true;
            }
        }
        if (hasNextSection && position.absoluteBeat >= queueBeat) {
            arrangementStepIndex_ = nextStepIndex;
            arrangementStepRepeat_ = nextRepeat;
            appendLog(QStringLiteral(
                "arrangement transition queued: row=%1 repeat=%2 bank=%3 target_abs_beat=%4")
                .arg(arrangementStepIndex_ + 1)
                .arg(arrangementStepRepeat_ + 1)
                .arg(QChar(QLatin1Char('A').unicode() +
                    arrangement.steps[arrangementStepIndex_].bankIndex))
                .arg(sectionEndBeat));
            launchBank(
                arrangement.steps[arrangementStepIndex_].bankIndex,
                false,
                sectionEndBeat);
        } else if (!hasNextSection && position.absoluteBeat >= sectionEndBeat) {
            arrangementRunning_ = false;
            refreshBankPresentation();
        }
    }
}

void MainWindow::startArrangement()
{
    const ArrangementDefinition& definition = looperProject_.arrangement();
    if (definition.steps.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Arrangement"),
            QStringLiteral("Add at least one arrangement step first."));
        return;
    }
    looperProject_.setArrangementEnabled(true);
    arrangementStepIndex_ = 0;
    arrangementStepRepeat_ = 0;
    arrangementResetBankAfterStop_ = false;
    arrangementArmed_ = !trackRecordingWorkflow_.globalTransportPlaying();
    arrangementRunning_ = !arrangementArmed_;
    launchBank(definition.steps.front().bankIndex, false);
    refreshBankPresentation();
    appendLog(arrangementArmed_
        ? QStringLiteral("arrangement armed for next global Play")
        : QStringLiteral("arrangement started at row 1"));
}

qint64 MainWindow::bankExactOutputFrames(int bankIndex, int sampleRate) const
{
    const int boundedBank = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    const auto pattern = bankMetronomePattern(boundedBank);
    const int sectionBeats = boundedBank < chordModel_.sections().size()
        ? qMax(1, chordModel_.section(boundedBank).beats)
        : qMax(1, pattern.beats_per_bar);
    return qMax<qint64>(1, static_cast<qint64>(std::llround(
        static_cast<double>(sectionBeats) * 60.0 * qMax(1, sampleRate) /
        (std::max(1.0, static_cast<double>(pattern.bpm)) *
         qMax(1, pattern.tempo_pulse_units)))));
}

void MainWindow::exportLooperAudio()
{
    enum class Scope {
        CurrentBank = 0,
        AllBanks,
        Arrangement,
    };

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Export Track Audio"));
    dialog.setMinimumWidth(480);
    auto* form = new QFormLayout(&dialog);
    auto* scopeBox = new QComboBox(&dialog);
    scopeBox->addItem(
        QStringLiteral("Current Section %1")
            .arg(QChar(QLatin1Char('A').unicode() + viewedBankIndex_)),
        static_cast<int>(Scope::CurrentBank));
    scopeBox->addItem(
        QStringLiteral("All Sections (separate WAVs)"),
        static_cast<int>(Scope::AllBanks));
    scopeBox->addItem(
        QStringLiteral("Arrangement (one WAV)"),
        static_cast<int>(Scope::Arrangement));
    form->addRow(QStringLiteral("Export"), scopeBox);
    auto* explanation = new QLabel(&dialog);
    explanation->setWordWrap(true);
    form->addRow(QString(), explanation);
    const auto updateExplanation = [this, scopeBox, explanation] {
        const Scope scope = static_cast<Scope>(scopeBox->currentData().toInt());
        if (scope == Scope::CurrentBank) {
            explanation->setText(QStringLiteral(
                "Render the viewed section exactly as it sounds in the Track view."));
        } else if (scope == Scope::AllBanks) {
            explanation->setText(QStringLiteral(
                "Render Sections A–D into four separate WAV files. Empty sections are exported as correctly timed silence."));
        } else {
            explanation->setText(looperProject_.arrangement().steps.isEmpty()
                ? QStringLiteral("The arrangement has no rows. Configure it before exporting.")
                : QStringLiteral(
                    "Follow the arrangement rows and repeat counts once, producing one complete WAV. The arrangement's Loop setting is not repeated indefinitely."));
        }
    };
    QObject::connect(
        scopeBox, &QComboBox::currentIndexChanged, &dialog,
        [updateExplanation](int) { updateExplanation(); });
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Export"));
    jam2::gui::registerGuiControl(
        *scopeBox,
        QStringLiteral("looper.export-dialog.scope"),
        QStringLiteral("looper.export"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.export-dialog"));
    jam2::gui::registerGuiControl(
        *buttons->button(QDialogButtonBox::Save),
        QStringLiteral("looper.export-dialog.accept"),
        QStringLiteral("looper.export"),
        jam2::gui::GuiControlAvailability::FileDialog,
        QStringLiteral("looper.export-dialog"));
    jam2::gui::registerGuiControl(
        *buttons->button(QDialogButtonBox::Cancel),
        QStringLiteral("looper.export-dialog.cancel"),
        QStringLiteral("looper.export"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.export-dialog"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateExplanation();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const Scope scope = static_cast<Scope>(scopeBox->currentData().toInt());
    const ArrangementDefinition arrangement = looperProject_.arrangement();
    if (scope == Scope::Arrangement && arrangement.steps.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Arrangement"),
            QStringLiteral("Add at least one arrangement row before exporting."));
        return;
    }

    const QString slug = JamStorage::portableSlug(chordModel_.title()).isEmpty()
        ? QStringLiteral("Jam2") : JamStorage::portableSlug(chordModel_.title());
    const QString defaultFolder = appReleaseFolderPath(QString{});
    QStringList outputPaths;
    if (scope == Scope::CurrentBank) {
        const QString suggested = QDir(defaultFolder).absoluteFilePath(
            QStringLiteral("%1_Section_%2.wav")
                .arg(slug, QChar(QLatin1Char('A').unicode() + viewedBankIndex_)));
        QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Export Current Section"),
            suggested,
            QStringLiteral("WAV files (*.wav)"),
            nullptr,
            QFileDialog::Options{});
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) {
            path += QStringLiteral(".wav");
        }
        outputPaths.append(QFileInfo(path).absoluteFilePath());
    } else if (scope == Scope::AllBanks) {
        const QString folder = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("Export All Sections"),
            defaultFolder,
            QFileDialog::ShowDirsOnly);
        if (folder.isEmpty()) return;
        for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
            outputPaths.append(QDir(folder).absoluteFilePath(
                QStringLiteral("%1_Section_%2.wav")
                    .arg(slug, QChar(QLatin1Char('A').unicode() + bank))));
        }
        QStringList existing;
        for (const QString& path : std::as_const(outputPaths)) {
            if (QFileInfo::exists(path)) existing.append(QFileInfo(path).fileName());
        }
        if (!existing.isEmpty() && QMessageBox::question(
                this,
                QStringLiteral("Replace Section Exports"),
                QStringLiteral("These files already exist and will be replaced:\n\n%1")
                    .arg(existing.join(QStringLiteral("\n"))),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
    } else {
        const QString suggested = QDir(defaultFolder).absoluteFilePath(
            QStringLiteral("%1_Arrangement.wav").arg(slug));
        QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Export Arrangement"),
            suggested,
            QStringLiteral("WAV files (*.wav)"),
            nullptr,
            QFileDialog::Options{});
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) {
            path += QStringLiteral(".wav");
        }
        outputPaths.append(QFileInfo(path).absoluteFilePath());
    }

    const LooperProject project = looperProject_;
    const QString projectFolder = projectPersistence_.projectFolder();
    const SharedTrackModel track = trackController_.model();
    const int sampleRate = activeTrackSampleRate();
    const int exportBank = qBound(
        0, viewedBankIndex_, project.banks().size() - 1);
    QVector<qint64> exactFrames;
    exactFrames.reserve(project.banks().size());
    for (int bank = 0; bank < project.banks().size(); ++bank) {
        exactFrames.append(bankExactOutputFrames(bank, sampleRate));
    }
    struct ExportResult {
        QStringList paths;
        qint64 frames = 0;
        QString error;
    };
    auto result = std::make_shared<ExportResult>();
    QEventLoop waitLoop;
    const bool started = startFileWorkerTask(
        [
            scope,
            project,
            projectFolder,
            track,
            sampleRate,
            exactFrames,
            arrangement,
            outputPaths,
            exportBank,
            result
        ] {
            if (scope == Scope::CurrentBank) {
                const PreparedMixResult rendered = PreparedMixRenderer::renderSequence(
                    project,
                    projectFolder,
                    sampleRate,
                    outputPaths.front(),
                    track,
                    {{exportBank, 1, exactFrames.at(exportBank)}});
                if (!rendered.error.isEmpty()) {
                    result->error = rendered.error;
                    return;
                }
                result->paths.append(rendered.path);
                result->frames += rendered.frames;
                return;
            }
            if (scope == Scope::AllBanks) {
                for (int bank = 0; bank < project.banks().size(); ++bank) {
                    const PreparedMixResult rendered = PreparedMixRenderer::renderSequence(
                        project,
                        projectFolder,
                        sampleRate,
                        outputPaths.at(bank),
                        track,
                        {{bank, 1, exactFrames.at(bank)}});
                    if (!rendered.error.isEmpty()) {
                        result->error = QStringLiteral("Section %1: %2")
                            .arg(QChar(QLatin1Char('A').unicode() + bank), rendered.error);
                        return;
                    }
                    result->paths.append(rendered.path);
                    result->frames += rendered.frames;
                }
                return;
            }
            QVector<PreparedMixSequenceSegment> segments;
            segments.reserve(arrangement.steps.size());
            for (const ArrangementStep& step : arrangement.steps) {
                segments.append({
                    step.bankIndex,
                    step.repeats,
                    exactFrames.at(step.bankIndex),
                });
            }
            const PreparedMixResult rendered = PreparedMixRenderer::renderSequence(
                project,
                projectFolder,
                sampleRate,
                outputPaths.front(),
                track,
                segments);
            if (!rendered.error.isEmpty()) {
                result->error = rendered.error;
                return;
            }
            result->paths.append(rendered.path);
            result->frames = rendered.frames;
        },
        [&waitLoop] { waitLoop.quit(); },
        [&waitLoop, result](const QString& error) {
            result->error = error;
            waitLoop.quit();
        });
    if (!started) {
        QMessageBox::warning(
            this,
            QStringLiteral("Export Track Audio"),
            QStringLiteral("The bounded file worker is busy; try Export again."));
        return;
    }
    QProgressDialog progress(
        scope == Scope::Arrangement
            ? QStringLiteral("Exporting arrangement...")
            : scope == Scope::AllBanks
                ? QStringLiteral("Exporting all sections...")
                : QStringLiteral("Exporting current section..."),
        QString{},
        0,
        0,
        this);
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    waitLoop.exec();
    progress.close();
    if (!result->error.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Export Track Audio"), result->error);
        return;
    }

    appendLog(QStringLiteral("looper export completed: files=%1 frames=%2")
        .arg(result->paths.size())
        .arg(result->frames));
    QMessageBox complete(this);
    complete.setIcon(QMessageBox::Information);
    complete.setWindowTitle(QStringLiteral("Export Complete"));
    complete.setText(result->paths.size() == 1
        ? QStringLiteral("The WAV was exported successfully.")
        : QStringLiteral("%1 section WAVs were exported successfully.")
            .arg(result->paths.size()));
    complete.setInformativeText(result->paths.size() == 1
        ? QDir::toNativeSeparators(result->paths.front())
        : QStringLiteral("Files were saved in:\n%1")
            .arg(QDir::toNativeSeparators(QFileInfo(result->paths.front()).absolutePath())));
    complete.setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    complete.exec();
}

void MainWindow::showArrangementDialog()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("arrangement edit ignored while a synced recording is active"));
        return;
    }
    const bool arrangementWasActive = arrangementRunning_ || arrangementArmed_;
    ArrangementEditorDialog dialog(
        looperProject_.arrangement(),
        looperProject_.banks().size(),
        arrangementWasActive,
        this);
    if (dialog.exec() != QDialog::Accepted) return;
    ArrangementEditorDialog::Result result = dialog.result();
    if (!looperProject_.setArrangement(std::move(result.definition))) return;
    syncLooperArrangement();
    if (result.action == ArrangementEditorDialog::Action::Stop) {
        stopArrangement();
        appendLog(QStringLiteral("arrangement stopped from performance view"));
    } else if (result.action == ArrangementEditorDialog::Action::Start) {
        startArrangement();
    } else if (arrangementWasActive) {
        const ArrangementDefinition& updated = looperProject_.arrangement();
        if (updated.steps.isEmpty()) {
            stopArrangement();
        } else {
            arrangementStepIndex_ = qBound(
                0, arrangementStepIndex_, updated.steps.size() - 1);
            arrangementStepRepeat_ = qBound(
                0,
                arrangementStepRepeat_,
                updated.steps.at(arrangementStepIndex_).repeats - 1);
            refreshBankPresentation();
        }
    }
}

void MainWindow::refreshLooperLanes()
{
    const int activeBankIndex = looperProject_.activeBankIndex();
    if (trackRecordingWorkflow_.laneArmed() &&
        trackRecordingWorkflow_.armedBank() != activeBankIndex &&
        !trackRecordingWorkflow_.inputTakeActive() &&
        !loopbackRecorder_.isRunning()) {
        trackRecordingWorkflow_.disarmLane();
        publishLocalTrackRecordingState(QStringLiteral("idle"));
        appendLog(QStringLiteral(
            "lane recording disarmed because the active Section changed"));
    }
    const auto viewedPattern = bankMetronomePattern(viewedBankIndex_);
    const double currentBpm = viewedPattern.bpm;
    double mismatchedBackingBpm = 0.0;
    const LooperBank& viewedBank = looperProject_.banks().at(viewedBankIndex_);
    for (const LooperLane& lane : viewedBank.lanes) {
        if (!lane.referenceKind.isEmpty() && lane.referenceBpm > 0.0 &&
            std::abs(lane.referenceBpm - currentBpm) > 0.01) {
            mismatchedBackingBpm = lane.referenceBpm;
            break;
        }
    }
    if (performanceHome_) {
        performanceHome_->setTrackBpmMismatch(
            mismatchedBackingBpm > 0.0,
            mismatchedBackingBpm,
            currentBpm);
    }
    refreshBankPresentation();
    if (looperStack_ == nullptr) {
        return;
    }
    const LooperBank& bank = viewedBank;
    if (bank.lanes.isEmpty()) {
        selectedLooperLane_ = -1;
    } else {
        selectedLooperLane_ = qBound(0, selectedLooperLane_, bank.lanes.size() - 1);
    }

    QVector<LooperLaneStackWidget::LaneView> views;
    QStringList missingWaveforms;
    const int currentSampleRate = activeTrackSampleRate();
    const SongSection* generatedChord = viewedBankIndex_ < chordModel_.sections().size()
        ? &chordModel_.section(viewedBankIndex_) : nullptr;
    const SongSection* generatedBeat = viewedBankIndex_ < beatModel_.sections().size()
        ? &beatModel_.section(viewedBankIndex_) : nullptr;
    const QString currentIdeaSignature =
        jam2::practice::practiceIdeaSignature(
            generatedChord,
            generatedBeat);
    views.reserve(bank.lanes.size());
    for (const LooperLane& lane : bank.lanes) {
        LooperLaneStackWidget::LaneView view;
        view.lane = lane;
        view.sourceFrames = lane.sourceFrames;
        if (lane.referenceStale ||
            (!lane.referenceKind.isEmpty() && lane.referenceBpm > 0.0 &&
             std::abs(lane.referenceBpm - currentBpm) > 0.01) ||
            (!lane.referenceKind.isEmpty() && lane.sampleRate > 0 &&
             lane.sampleRate != currentSampleRate) ||
            (!lane.referenceKind.isEmpty() && !lane.referenceSourceSignature.isEmpty() &&
             lane.referenceSourceSignature != currentIdeaSignature)) {
            view.lane.name += QStringLiteral(" [stale]");
        }
        if (!lane.sampleRateCompatible) {
            view.lane.name += QStringLiteral(" [unavailable: WAV conversion failed]");
        }
        view.assetPath = looperAssetAbsolutePath(lane);
        if (!lane.assetPath.trimmed().isEmpty()) {
            const auto cached = looperWaveformCache_.constFind(view.assetPath);
            if (cached != looperWaveformCache_.cend()) {
                view.peaks = cached.value().peaks;
                view.sourceFrames = cached.value().sourceFrames;
            } else if (!missingWaveforms.contains(view.assetPath)) {

                missingWaveforms.append(view.assetPath);
            }
        }
        views.push_back(std::move(view));
    }
    const int armedLane = trackRecordingWorkflow_.armedBank() == viewedBankIndex_
        ? trackRecordingWorkflow_.armedLane() : -1;
    int markerRate = trackRecordingWorkflow_.preparedSampleRate();
    if (markerRate <= 0) {
        markerRate = 48000;
    }
    const int sectionBeats =
        chordModel_.sections().isEmpty()
        ? qMax(1, viewedPattern.beats_per_bar)
        : qMax(1, chordModel_.section(qBound(
            0, viewedBankIndex_, chordModel_.sections().size() - 1)).beats);
    const int tempoPulseUnits = viewedPattern.tempo_pulse_units;
    const qint64 sectionFrames = static_cast<qint64>(std::llround(
        static_cast<double>(markerRate) * 60.0 /
        qMax(1.0, static_cast<double>(viewedPattern.bpm)) *
        (1.0 / static_cast<double>(tempoPulseUnits)) *
        static_cast<double>(sectionBeats)));
    // A bank owns its ruler length. The active bank's prepared/global track
    // must not stretch a different bank while that bank is being edited.
    // Lanes in the viewed bank can still extend the calculated view naturally.
    const qint64 minimumViewFrames = qMax<qint64>(1, sectionFrames);
    looperStack_->setLanes(
        std::move(views),
        selectedLooperLane_,
        viewedBankIndex_,
        armedLane,
        markerRate,
        minimumViewFrames,
        viewedPattern.bpm,
        tempoPulseUnits,
        looperProject_.gridLockEnabled());
    QMap<QString, QStringList> remoteLabelsByLane;
    for (auto it = peerTrackRecordingStates_.cbegin();
         it != peerTrackRecordingStates_.cend(); ++it) {
        const PeerTrackRecordingState& state = it.value();
        if (state.bank != viewedBankIndex_ || state.laneId.isEmpty()) continue;
        remoteLabelsByLane[state.laneId].append(QStringLiteral("%1 %2")
            .arg(recordingPeerLabel(it.key()), state.phase.toUpper()));
    }
    QMap<QString, QString> remoteLabels;
    for (auto it = remoteLabelsByLane.cbegin(); it != remoteLabelsByLane.cend(); ++it) {
        remoteLabels[it.key()] = it.value().join(QStringLiteral(" · "));
    }
    looperStack_->setRemoteRecordingStates(
        std::move(remoteLabels), sharedRecordingProtected());

    if (missingWaveforms.isEmpty() || looperWaveformWorkerRunning_) {
        return;
    }
    looperWaveformWorkerRunning_ = true;
    auto previews = std::make_shared<std::vector<std::pair<QString, LooperWaveformPreview>>>();
    previews->reserve(static_cast<std::size_t>(missingWaveforms.size()));
    const bool started = startFileWorkerTask(
        [missingWaveforms, previews] {
            for (const QString& path : missingWaveforms) {
                LooperWaveformPreview preview;
                constexpr int peakCount = 512;
                preview.valid = readPcm16WaveformPeaks(
                    path,
                    peakCount,
                    preview.peaks,
                    &preview.sourceFrames);
                previews->emplace_back(path, std::move(preview));
            }
        },
        [this, previews] {
            looperWaveformWorkerRunning_ = false;
            constexpr qsizetype maxCachedWaveforms = 256;
            for (auto& [path, preview] : *previews) {
                if (!looperAssetPathIsReferenced(path)) {
                    looperWaveformCache_.remove(path);
                    (void)projectPersistence_.discardTransientWav(path);
                    continue;
                }
                while (looperWaveformCache_.size() >= maxCachedWaveforms &&
                       !looperWaveformCache_.contains(path)) {
                    looperWaveformCache_.erase(looperWaveformCache_.begin());

                }
                if (preview.sourceFrames > 0) {
                    for (LooperBank& bank : looperProject_.banks()) {
                        for (LooperLane& lane : bank.lanes) {
                            if (QDir::cleanPath(looperAssetAbsolutePath(lane)) ==
                                QDir::cleanPath(path)) {
                                lane.sourceFrames = preview.sourceFrames;
                            }
                        }
                    }
                }
                looperWaveformCache_.insert(path, std::move(preview));
            }
            refreshLooperLanes();
        },
        [this](const QString&) {
            looperWaveformWorkerRunning_ = false;
            QTimer::singleShot(100, this, [this] { refreshLooperLanes(); });
        });

    if (!started) {
        looperWaveformWorkerRunning_ = false;
        QTimer::singleShot(100, this, [this] { refreshLooperLanes(); });
    }
}

qint64 MainWindow::looperLaneTimelineEndFrame(
    const LooperLane& lane,
    int sampleRate,
    qint64* resolvedSourceFrames) const
{
    qint64 sourceFrames = lane.sourceFrames;
    if (sourceFrames <= 0 && lane.assetPath.trimmed().isEmpty()) {
        if (resolvedSourceFrames) *resolvedSourceFrames = 0;
        return jam2::gui::looperLaneTimelineEnd(lane, 0, sampleRate);
    }
    const QString path = looperAssetAbsolutePath(lane);
    const auto cached = looperWaveformCache_.constFind(path);
    if (sourceFrames <= 0 && cached != looperWaveformCache_.cend()) {
        sourceFrames = cached.value().sourceFrames;
    }
    if (sourceFrames <= 0) {
        const WavMetadata metadata = readWavMetadata(path);
        const qint64 bytesPerFrame = static_cast<qint64>(metadata.channels) *
            qMax(1, metadata.bitsPerSample / 8);
        if (bytesPerFrame > 0) sourceFrames = metadata.dataBytes / bytesPerFrame;
    }
    if (resolvedSourceFrames) *resolvedSourceFrames = qMax<qint64>(0, sourceFrames);
    return jam2::gui::looperLaneTimelineEnd(lane, sourceFrames, sampleRate);
}

int MainWindow::requiredSectionBeatsForTracks(int bankIndex) const
{
    if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) return 0;
    const int sampleRate = qMax(1, activeTrackSampleRate());
    qint64 endFrame = 0;
    for (const LooperLane& lane : looperProject_.banks().at(bankIndex).lanes) {
        endFrame = qMax(endFrame, looperLaneTimelineEndFrame(lane, sampleRate));
    }
    const auto timing = bankMetronomePattern(bankIndex);
    return jam2::gui::sectionBeatCountForTimelineEnd(
        endFrame,
        sampleRate,
        timing.bpm,
        timing.tempo_pulse_units,
        timing.beats_per_bar);
}

bool MainWindow::extendSectionToFitTracks(int bankIndex, bool showLimitWarning)
{
    if (bankIndex < 0 || bankIndex >= chordModel_.sections().size()) return false;
    const int currentBeats = chordModel_.section(bankIndex).beats;
    const int requiredBeats = requiredSectionBeatsForTracks(bankIndex);
    if (requiredBeats <= currentBeats) return false;

    chordModel_.resizeSection(bankIndex, requiredBeats);
    const int appliedBeats = chordModel_.section(bankIndex).beats;
    if (appliedBeats < requiredBeats) {
        const QString detail = QStringLiteral(
            "Section %1 reached the %2-beat limit; audio beyond that point cannot be included in the prepared Section.")
            .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
            .arg(appliedBeats);
        appendLog(detail);
        if (showLimitWarning) {
            QMessageBox::warning(this, QStringLiteral("Section length limit"), detail);
        }
    } else {
        appendLog(QStringLiteral("extended Section %1 from %2 to %3 beats for track audio")
            .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
            .arg(currentBeats)
            .arg(appliedBeats));
    }
    if (chordGrid_) chordGrid_->refresh();
    if (beatGrid_) beatGrid_->refresh();
    if (lyricGrid_) lyricGrid_->refresh();
    updateSectionTrimControls();
    return appliedBeats != currentBeats;
}

int MainWindow::safeTrimSectionBeats(int bankIndex) const
{
    if (bankIndex < 0 || bankIndex >= chordModel_.sections().size()) return 0;
    const int beatsPerBar = qMax(1, sectionBeatsPerBar(bankIndex));
    const int musicalBeats = chordModel_.occupiedBeatCount(bankIndex);
    const int roundedMusicalBeats = qMax(
        beatsPerBar,
        ((musicalBeats + beatsPerBar - 1) / beatsPerBar) * beatsPerBar);
    return qMax(roundedMusicalBeats, requiredSectionBeatsForTracks(bankIndex));
}

bool MainWindow::sectionHasUnknownTrackDuration(int bankIndex) const
{
    if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) return false;
    for (const LooperLane& lane : looperProject_.banks().at(bankIndex).lanes) {
        if (!lane.assetHash.isEmpty() && lane.assetPath.trimmed().isEmpty() &&
            lane.sourceFrames <= 0 && lane.stopFrame <= lane.startFrame) {
            return true;
        }
    }
    return false;
}

void MainWindow::rebuildSectionMixAfterLengthChange(int bankIndex)
{
    const bool activeSection = bankIndex == looperProject_.activeBankIndex();
    const bool restartPlayback = activeSection &&
        (trackRecordingWorkflow_.globalTransportRequestedPlaying() ||
         trackRecordingWorkflow_.globalTransportPlaying());

    // The prepared mix is a cache of both the lane audio and the Section
    // length.  Do not let its old source position continue under a shortened
    // musical timeline: unload it, establish a fresh transport origin, and
    // attach the rebuilt mix against that origin when rendering completes.
    if (activeSection) {
        discardPreparedMix(restartPlayback);
    }
    regeneratePreparedMix(bankIndex);

    if (!restartPlayback) return;
    trackController_.requestPlayback(true);
    preparedMixLifecycle_.setPlayWhenReady(
        PreparedMixRenderer::hasRenderableSources(looperProject_, bankIndex));
    if (!trackRecordingWorkflow_.restartGlobalTransport(
            metronomeTransport_.grid().position())) {
        appendLog(QStringLiteral(
            "engine command queue unavailable: restart playback after Section length change"));
    } else {
        appendLog(QStringLiteral(
            "restarted global playback after shortening active Section %1")
            .arg(QChar(QLatin1Char('A').unicode() + bankIndex)));
    }
    updateTrackPlaybackPresentation();
}

void MainWindow::shrinkSectionOneBar(int bankIndex)
{
    if (bankIndex < 0 || bankIndex >= chordModel_.sections().size() ||
        bankIndex >= looperProject_.banks().size()) {
        return;
    }
    if (trackRecordingWorkflow_.inputTakeActive() || loopbackRecorder_.isRunning() ||
        trackRecordingWorkflow_.laneArmed()) {
        QMessageBox::information(
            this,
            QStringLiteral("Remove One Bar"),
            QStringLiteral("Finish or disarm the current recording before changing the Section length."));
        return;
    }
    const int beatsPerBar = qMax(1, sectionBeatsPerBar(bankIndex));
    const int currentBeats = chordModel_.section(bankIndex).beats;
    const int targetBeats = qMax(beatsPerBar, currentBeats - beatsPerBar);
    if (targetBeats >= currentBeats) return;

    const int sampleRate = qMax(1, activeTrackSampleRate());
    const auto timing = bankMetronomePattern(bankIndex);
    const qint64 targetFrames = qMax<qint64>(1, static_cast<qint64>(std::llround(
        static_cast<double>(targetBeats) * 60.0 * sampleRate /
        (qMax(1, timing.bpm) * qMax(1, timing.tempo_pulse_units)))));
    const bool musicalConflict = chordModel_.occupiedBeatCount(bankIndex) > targetBeats;
    bool wavConflict = false;
    for (const LooperLane& lane : looperProject_.banks().at(bankIndex).lanes) {
        if (looperLaneTimelineEndFrame(lane, sampleRate) > targetFrames) {
            wavConflict = true;
            break;
        }
    }
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    const bool peerConflict = session.remotePeerCount > 0 &&
        jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Full;
    const bool unknownWavDuration = sectionHasUnknownTrackDuration(bankIndex);
    const bool hasConflict = musicalConflict || wavConflict || peerConflict || unknownWavDuration;
    if (hasConflict) {
        QStringList conflicts;
        if (musicalConflict) {
            conflicts.append(QStringLiteral(
                "Chord, beat, lyric, or generated-line content exists in the final bar."));
        }
        if (wavConflict) {
            conflicts.append(QStringLiteral(
                "One or more WAV placements extend into the final bar and will be cropped or removed from their track."));
        }
        if (unknownWavDuration) {
            conflicts.append(QStringLiteral(
                "A remote WAV has not supplied enough duration information to prove that the final bar is empty."));
        }
        if (peerConflict) {
            conflicts.append(QStringLiteral(
                "Peers may have custom idea content there because Whole idea sync is not enabled."));
        }
        QMessageBox dialog(
            QMessageBox::Warning,
            QStringLiteral("Remove One Bar"),
            QStringLiteral("Removing bar %1 from Section %2 will delete content.")
                .arg((currentBeats + beatsPerBar - 1) / beatsPerBar)
                .arg(QChar(QLatin1Char('A').unicode() + bankIndex)),
            QMessageBox::Cancel,
            this);
        dialog.setInformativeText(
            conflicts.join(QStringLiteral("\n\n")) +
            QStringLiteral(
                "\n\nOriginal imported files remain on disk, but this Section edit cannot be restored automatically."));
        QPushButton* deleteButton = dialog.addButton(
            QStringLiteral("Delete Anyway"), QMessageBox::DestructiveRole);
        jam2::gui::registerGuiControl(
            *deleteButton,
            QStringLiteral("song.section-shrink-dialog.confirm"),
            QStringLiteral("song.section-resize"),
            jam2::gui::GuiControlAvailability::Modal);
        QAbstractButton* cancelButton = dialog.button(QMessageBox::Cancel);
        jam2::gui::registerGuiControl(
            *cancelButton,
            QStringLiteral("song.section-shrink-dialog.cancel"),
            QStringLiteral("song.section-resize"),
            jam2::gui::GuiControlAvailability::Modal);
        dialog.setDefaultButton(QMessageBox::Cancel);
        dialog.exec();
        if (dialog.clickedButton() != deleteButton) return;
    }

    QSet<QString> removedAssets;
    QSet<QString> removedHashes;
    LooperProject croppedProject = looperProject_;
    if (wavConflict) {
        for (int laneIndex = 0;
             laneIndex < croppedProject.banks().at(bankIndex).lanes.size();
            ++laneIndex) {
            const LooperLane lane =
                croppedProject.banks().at(bankIndex).lanes.at(laneIndex);
            qint64 resolvedSourceFrames = 0;
            if (looperLaneTimelineEndFrame(
                    lane, sampleRate, &resolvedSourceFrames) <= targetFrames) continue;
            const LooperLaneTimelineCropResult crop =
                croppedProject.cropLaneToTimelineEnd(
                    bankIndex,
                    laneIndex,
                    resolvedSourceFrames,
                    sampleRate,
                    targetFrames);
            if (crop.status == LooperLaneTimelineCropStatus::Cleared) {
                removedAssets.insert(looperAssetAbsolutePath(lane));
                if (!crop.removedAssetHash.isEmpty()) {
                    removedHashes.insert(crop.removedAssetHash);
                }
                continue;
            }
            if (crop.status != LooperLaneTimelineCropStatus::Cropped) {
                appendLog(QStringLiteral(
                    "could not apply the bounded lane crop while shortening Section %1")
                    .arg(QChar(QLatin1Char('A').unicode() + bankIndex)));
                return;
            }
        }
    }
    looperProject_ = std::move(croppedProject);
    chordModel_.resizeSection(bankIndex, targetBeats);
    for (const QString& hash : std::as_const(removedHashes)) {
        cancelUnreferencedLooperAssetTransfer(hash);
    }
    if (!removedAssets.isEmpty()) discardObsoleteReferenceWavs(removedAssets);
    if (chordGrid_) chordGrid_->refresh();
    if (beatGrid_) beatGrid_->refresh();
    if (lyricGrid_) lyricGrid_->refresh();
    refreshLooperLanes();
    rebuildSectionMixAfterLengthChange(bankIndex);
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("grid.resize")},
        {QStringLiteral("revision"), chordModel_.revision()},
        {QStringLiteral("section"), bankIndex},
        {QStringLiteral("lane"), QStringLiteral("chord")},
        {QStringLiteral("beats"), targetBeats},
    });
    syncLooperArrangement();
    appendLog(QStringLiteral("removed one bar from Section %1%2")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
        .arg(hasConflict ? QStringLiteral(" with confirmed content deletion") : QString{}));
}

void MainWindow::trimViewedSection()
{
    const int bankIndex = viewedBankIndex_;
    if (bankIndex < 0 || bankIndex >= chordModel_.sections().size()) return;
    if (trackRecordingWorkflow_.inputTakeActive() || loopbackRecorder_.isRunning() ||
        trackRecordingWorkflow_.laneArmed()) {
        QMessageBox::information(
            this,
            QStringLiteral("Trim Section"),
            QStringLiteral("Finish or disarm the current recording before trimming this Section."));
        return;
    }
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (session.remotePeerCount > 0 &&
        jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Full) {
        QMessageBox::information(
            this,
            QStringLiteral("Trim Section"),
            QStringLiteral(
                "Whole idea sync must be enabled before trimming with peers connected, so custom chord and beat content on every peer is protected."));
        return;
    }
    if (sectionHasUnknownTrackDuration(bankIndex)) {
        QMessageBox::information(
            this,
            QStringLiteral("Trim Section"),
            QStringLiteral(
                "This Section contains a remote WAV whose duration is not known yet. Share or download that WAV before using safe Trim."));
        return;
    }
    const int currentBeats = chordModel_.section(bankIndex).beats;
    const int targetBeats = safeTrimSectionBeats(bankIndex);
    if (targetBeats <= 0 || targetBeats >= currentBeats) {
        QMessageBox::information(
            this,
            QStringLiteral("Trim Section"),
            QStringLiteral("There are no complete trailing empty bars to remove."));
        return;
    }
    const int beatsPerBar = qMax(1, sectionBeatsPerBar(bankIndex));
    const int currentBars = (currentBeats + beatsPerBar - 1) / beatsPerBar;
    const int targetBars = (targetBeats + beatsPerBar - 1) / beatsPerBar;
    QMessageBox dialog(
            QMessageBox::Question,
            QStringLiteral("Trim Section"),
            QStringLiteral(
                "Trim Section %1 from %2 to %3 bars?\n\n%4 trailing empty bars will be removed. Musical content and placed WAVs are protected.")
                .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
                .arg(currentBars)
                .arg(targetBars)
                .arg(currentBars - targetBars),
            QMessageBox::NoButton,
            this);
    QAbstractButton* trimButton = dialog.addButton(QMessageBox::Yes);
    QAbstractButton* cancelButton = dialog.addButton(QMessageBox::No);
    jam2::gui::registerGuiControl(
        *trimButton,
        QStringLiteral("song.section-trim-dialog.confirm"),
        QStringLiteral("song.section-structure"),
        jam2::gui::GuiControlAvailability::Modal);
    jam2::gui::registerGuiControl(
        *cancelButton,
        QStringLiteral("song.section-trim-dialog.cancel"),
        QStringLiteral("song.section-structure"),
        jam2::gui::GuiControlAvailability::Modal);
    dialog.setDefaultButton(QMessageBox::No);
    dialog.exec();
    if (dialog.clickedButton() != trimButton) {
        return;
    }
    chordModel_.resizeSection(bankIndex, targetBeats);
    if (chordGrid_) chordGrid_->refresh();
    if (beatGrid_) beatGrid_->refresh();
    if (lyricGrid_) lyricGrid_->refresh();
    refreshLooperLanes();
    rebuildSectionMixAfterLengthChange(bankIndex);
    syncLooperArrangement();
    updateSectionTrimControls();
}

void MainWindow::updateSectionTrimControls()
{
    const bool sectionValid = viewedBankIndex_ >= 0 &&
        viewedBankIndex_ < chordModel_.sections().size();
    const bool recordingBusy = trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    const bool peerIdeaConflict = session.remotePeerCount > 0 &&
        jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Full;
    const bool unknownWavDuration = sectionValid &&
        sectionHasUnknownTrackDuration(viewedBankIndex_);
    const bool canTrim = sectionValid && !recordingBusy && !peerIdeaConflict &&
        !unknownWavDuration &&
        safeTrimSectionBeats(viewedBankIndex_) <
            chordModel_.section(viewedBankIndex_).beats;
    QString tooltip = QStringLiteral(
        "Remove complete trailing bars that contain no chords, beats, lyrics, or placed WAV audio");
    if (recordingBusy) {
        tooltip = QStringLiteral("Finish or disarm recording before trimming the Section");
    } else if (peerIdeaConflict) {
        tooltip = QStringLiteral(
            "Enable Whole idea sync before trimming with peers connected");
    } else if (unknownWavDuration) {
        tooltip = QStringLiteral(
            "Download the pending remote WAV before using safe Trim");
    } else if (!canTrim) {
        tooltip = QStringLiteral("No complete trailing empty bars can be removed");
    }
    for (QPushButton* button : std::as_const(sectionTrimButtons_)) {
        if (!button) continue;
        button->setEnabled(canTrim);
        button->setToolTip(tooltip);
    }
}

void MainWindow::applySelectedLooperLaneRegion(qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame)
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int row = selectedLooperLane_;
    LooperBank& bank = looperProject_.banks()[viewedBankIndex_];
    if (row < 0 || row >= bank.lanes.size()) {
        return;
    }
    LooperLane& lane = bank.lanes[row];
    const QString assetPath = looperAssetAbsolutePath(lane);
    const auto preview = looperWaveformCache_.constFind(assetPath);
    if (preview == looperWaveformCache_.cend()) {
        appendLog(QStringLiteral("lane region edit deferred until bounded waveform inspection completes"));
        refreshLooperLanes();
        return;
    }
    const qint64 sourceFrames = preview.value().sourceFrames;
    if (sourceFrames <= 0) {
        refreshLooperLanes();
        return;
    }

    sourceStartFrame = qBound<qint64>(0, sourceStartFrame, sourceFrames - 1);
    sourceEndFrame = qBound<qint64>(sourceStartFrame + 1, sourceEndFrame, sourceFrames);
    const qint64 boundedStart = qMax<qint64>(0, startFrame);
    const qint64 visibleFrames = sourceEndFrame - sourceStartFrame;
    if (boundedStart >
        jam2::application::limits::kMaximumLooperTimelineFrames - visibleFrames) {
        appendLog(QStringLiteral(
            "lane region edit rejected because it exceeds the maintained timeline limit"));
        refreshLooperLanes();
        return;
    }
    const bool completeSource =
        sourceStartFrame == 0 && sourceEndFrame == sourceFrames;
    if (!looperProject_.setLaneRegion(
            viewedBankIndex_,
            row,
            LooperLaneRegion{
                boundedStart,
                boundedStart + visibleFrames,
                completeSource ? -1 : sourceStartFrame,
                completeSource ? -1 : sourceEndFrame,
                lane.loopEnabled})) {
        appendLog(QStringLiteral("lane region edit rejected by the project model"));
        refreshLooperLanes();
        return;
    }

    refreshLooperLanes();
    selectedLooperLane_ = row;
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::applyLooperLaneGain(int laneIndex, double gainDb)
{
    if (!looperProject_.setLaneGainDb(viewedBankIndex_, laneIndex, gainDb)) return;
    selectedLooperLane_ = laneIndex;
    refreshLooperLanes();
    regeneratePreparedMix();
}

void MainWindow::showJamTasterDialog(int laneIndex)
{
    if (!QDir().mkpath(jamStorage_.rootFolder())) {
        QMessageBox::warning(
            this, QStringLiteral("JamTaster"),
            QStringLiteral("Could not create this jam's working folder."));
        return;
    }
    QString sourcePath;
    QString sourceHash;
    QString displayName = chordModel_.title();
    const int selected = laneIndex >= 0 ? laneIndex : selectedLooperLane_;
    if (viewedBankIndex_ >= 0 && viewedBankIndex_ < looperProject_.banks().size()) {
        const QVector<LooperLane>& lanes = looperProject_.banks().at(viewedBankIndex_).lanes;
        if (selected >= 0 && selected < lanes.size() &&
            !lanes.at(selected).assetPath.trimmed().isEmpty()) {
            sourcePath = looperAssetAbsolutePath(lanes.at(selected));
            sourceHash = lanes.at(selected).assetHash;
            if (!lanes.at(selected).name.trimmed().isEmpty()) {
                displayName = lanes.at(selected).name;
            }
        }
    }
    if ((jamTaster_->taskActive() || sourcePath.isEmpty()) &&
        !jamTaster_->taskInputPath().isEmpty() &&
        QDir(jamTaster_->taskProjectRoot()).absolutePath() ==
            QDir(jamStorage_.rootFolder()).absolutePath()) {
        sourcePath = jamTaster_->taskInputPath();
        displayName = jamTaster_->taskDisplayName();
    }
    if (!jamTasterDialog_) {
        jamTasterDialog_ = std::make_unique<JamTasterDialog>(
            *jamTaster_,
            jamStorage_.rootFolder(),
            sourcePath,
            sourceHash,
            displayName,
            JamTasterDialog::Callbacks{
                [this](const QJsonObject& tempo, const QJsonObject& stems,
                       const QJsonObject& options, const QString& sourceHash) {
                    applyJamTasterQuick(tempo, stems, options, sourceHash);
                },
                [this](const QString& song, const QJsonObject& options) {
                    applyJamTasterConverted(song, options);
                },
                [this](const QString& song, const QString& sourcePath,
                       const QString& sourceHash) {
                    createJamTasterSong(song, sourcePath, sourceHash);
                },
            },
            this);
    } else if (!jamTaster_->taskActive()) {
        jamTasterDialog_->setSourceContext(
            jamStorage_.rootFolder(), sourcePath, sourceHash, displayName);
    }
    jamTasterDialog_->exec();
}

void MainWindow::applyJamTasterTempo(const QJsonObject& result)
{
    const int bpm = qBound(
        20,
        result.value(QStringLiteral("project_bpm")).toInt(
            qRound(result.value(QStringLiteral("bpm")).toDouble())),
        400);
    const int meter = qBound(
        2, result.value(QStringLiteral("beats_per_bar")).toInt(4), 12);
    if (bpm < 20) return;
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        LooperBankTiming timing = looperProject_.resolvedTiming(bank);
        timing.bpm = bpm;
        timing.beatsPerBar = meter;
        timing.inheritsBankA = bank > 0;
        (void)looperProject_.setTiming(bank, timing);
    }
    auto& track = trackController_.model();
    track.guessedBpm = result.value(QStringLiteral("bpm")).toDouble(bpm);
    track.acceptedBpm = bpm;
    applyMetronomePatternForBank(looperProject_.activeBankIndex(), true);
    updateTrackControls();
    regeneratePreparedMix();
    syncLooperArrangement();
    jamStorage_.markArtifactCreated();
    appendLog(QStringLiteral("JamTaster tempo applied: detected=%1 project=%2 meter=%3/4")
        .arg(track.guessedBpm, 0, 'f', 3)
        .arg(bpm)
        .arg(meter));
}

void MainWindow::applyJamTasterQuick(
    const QJsonObject& tempo,
    const QJsonObject& stemsResult,
    const QJsonObject& options,
    const QString& sourceHash)
{
    if (options.value(QStringLiteral("tempo")).toBool() && !tempo.isEmpty()) {
        applyJamTasterTempo(tempo);
    }
    if (!options.value(QStringLiteral("stems")).toBool() || stemsResult.isEmpty()) {
        return;
    }
    const QJsonObject stems = stemsResult.value(QStringLiteral("stems")).toObject();
    const QStringList order{
        QStringLiteral("drums"), QStringLiteral("bass"),
        QStringLiteral("other"), QStringLiteral("vocals")};
    const QMap<QString, QString> names{
        {QStringLiteral("drums"), QStringLiteral("Drums")},
        {QStringLiteral("bass"), QStringLiteral("Bass")},
        {QStringLiteral("other"), QStringLiteral("Chords / Other")},
        {QStringLiteral("vocals"), QStringLiteral("Vocals")},
    };
    QStringList paths;
    QStringList displayNames;
    for (const QString& stem : order) {
        const QString path = stems.value(stem).toString();
        if (QFileInfo(path).isFile()) {
            paths.append(path);
            displayNames.append(names.value(stem));
        }
    }
    const int bankIndex = viewedBankIndex_;
    if (paths.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("JamTaster"),
            QStringLiteral("The analysis has no saved stems to apply."));
        return;
    }
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = activeTrackSampleRate();
    auto assets = std::make_shared<std::vector<StagedPcm16Asset>>();
    auto error = std::make_shared<QString>();
    const bool started = startFileWorkerTask(
        [paths, stagingFolder, expectedSampleRate, assets, error] {
            for (const QString& path : paths) {
                StagedPcm16Asset asset = stagePcm16Asset(
                    path, stagingFolder, expectedSampleRate,
                    QStringLiteral("imported"));
                if (!asset.error.isEmpty()) {
                    *error = asset.error;
                    return;
                }
                assets->push_back(std::move(asset));
            }
        },
        [this, bankIndex, sourceHash, displayNames, assets, error] {
            if (!error->isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("JamTaster"), *error);
                return;
            }
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) return;
            bool changed = false;
            int reused = 0;
            int skippedForCapacity = 0;
            for (LooperLane& lane : looperProject_.banks()[bankIndex].lanes) {
                if (!sourceHash.isEmpty() && lane.assetHash == sourceHash) {
                    if (!lane.muted) {
                        lane.muted = true;
                        changed = true;
                    }
                }
            }
            for (std::size_t index = 0; index < assets->size(); ++index) {
                const StagedPcm16Asset& asset = assets->at(index);
                const QString expectedName = displayNames.value(
                    static_cast<int>(index), asset.displayName);
                const auto& existingLanes = looperProject_.banks()[bankIndex].lanes;
                const auto existing = std::find_if(
                    existingLanes.cbegin(), existingLanes.cend(),
                    [&asset](const LooperLane& lane) {
                        return !asset.sha256.isEmpty() &&
                            lane.assetHash.compare(asset.sha256, Qt::CaseInsensitive) == 0;
                    });
                if (existing != existingLanes.cend()) {
                    ++reused;
                    appendLog(QStringLiteral(
                        "JamTaster kept existing lane %1; WAV hash already matches %2")
                        .arg(existing->name.isEmpty() ? expectedName : existing->name,
                             asset.sha256));
                    continue;
                }
                if (existingLanes.size() >=
                    jam2::application::limits::kMaximumLooperLanesPerBank) {
                    ++skippedForCapacity;
                    continue;
                }
                LooperLane lane;
                lane.assetPath = asset.stagedPath;
                lane.assetHash = asset.sha256;
                lane.name = expectedName;
                lane.sampleRate = asset.metadata.sampleRate;
                lane.sampleRateCompatible = true;
                lane.sourceFrames = asset.metadata.frames;
                lane.originKind = QStringLiteral("imported");
                if (looperProject_.appendLane(bankIndex, std::move(lane))) {
                    registerTransientTrackWav(asset.stagedPath);
                    looperWaveformCache_.remove(asset.stagedPath);
                    changed = true;
                }
            }
            if (changed) {
                jamStorage_.markArtifactCreated();
                refreshLooperLanes();
                regeneratePreparedMix();
                syncLooperArrangement();
            }
            appendLog(reused > 0
                ? QStringLiteral(
                    "JamTaster stem apply reused %1 existing WAV lane(s); no duplicates added")
                    .arg(reused)
                : skippedForCapacity > 0 && !changed
                    ? QStringLiteral(
                        "JamTaster stems already fill the current section; no WAV lanes added")
                    : QStringLiteral(
                        "JamTaster stems added to the current section; source WAV muted"));
        });
    if (!started) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"),
                             QStringLiteral("The bounded file worker is busy."));
    }
}

void MainWindow::createJamTasterSong(
    const QString& convertedSong,
    const QString& sourcePath,
    const QString& sourceHash)
{
    Q_UNUSED(sourceHash)
    const QFileInfo source(convertedSong);
    if (!source.isDir()) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"),
                             QStringLiteral("The converted song folder is missing."));
        return;
    }
    QString suggestedName = source.fileName();
    suggestedName.replace(QLatin1Char('_'), QLatin1Char(' '));
    bool accepted = false;
    const QString displayName = QInputDialog::getText(
        this,
        QStringLiteral("Create New JamJar"),
        QStringLiteral("Song name"),
        QLineEdit::Normal,
        suggestedName,
        &accepted).trimmed();
    if (!accepted || displayName.isEmpty()) return;
    const QString sourceDisposition = promptJamTasterSourceDisposition();
    if (sourceDisposition.isEmpty()) return;
    const QString slug = JamStorage::portableSlug(displayName);
    const QString destination = QDir(appReleaseFolderPath(QStringLiteral("songs")))
        .absoluteFilePath(slug);
    if (QFileInfo::exists(destination)) {
        QMessageBox::warning(
            this, QStringLiteral("JamTaster"),
            QStringLiteral("A song named %1 already exists.").arg(displayName));
        return;
    }
    const QString partialDestination = QDir(QFileInfo(destination).absolutePath())
        .absoluteFilePath(QStringLiteral(".jamtaster-%1.partial")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    auto error = std::make_shared<QString>();
    const int expectedSampleRate = activeTrackSampleRate();
    const bool started = startFileWorkerTask(
        [sourcePath = source.absoluteFilePath(), destination, partialDestination,
         displayName, slug, error, sourceDisposition, originalSource = sourcePath,
         expectedSampleRate] {
            try {
                std::filesystem::copy(
                    nativeFilePath(sourcePath), nativeFilePath(partialDestination),
                    std::filesystem::copy_options::recursive);
                QDir staged(partialDestination);
                const QStringList jamjars = staged.entryList(
                    {QStringLiteral("*.jamjar")}, QDir::Files, QDir::Name);
                if (jamjars.size() != 1) {
                    throw std::runtime_error("copied JamTaster song has no unique JamJar");
                }
                QFile input(staged.absoluteFilePath(jamjars.front()));
                if (!input.open(QIODevice::ReadOnly)) {
                    throw std::runtime_error("could not read the copied JamJar");
                }
                QJsonDocument document = QJsonDocument::fromJson(input.readAll());
                if (!document.isObject()) {
                    throw std::runtime_error("the copied JamJar is invalid");
                }
                QJsonObject object = document.object();
                object.insert(QStringLiteral("title"), displayName);
                if (sourceDisposition == QStringLiteral("keep")) {
                    const StagedPcm16Asset asset = stagePcm16Asset(
                        originalSource, partialDestination, expectedSampleRate,
                        QStringLiteral("imported"));
                    if (!asset.error.isEmpty()) {
                        throw std::runtime_error(asset.error.toStdString());
                    }
                    QJsonObject lane{
                        {QStringLiteral("asset_path"),
                         QStringLiteral("imported/%1.wav").arg(asset.sha256)},
                        {QStringLiteral("asset_hash"), asset.sha256},
                        {QStringLiteral("sample_rate"), asset.metadata.sampleRate},
                        {QStringLiteral("source_frames"), QString::number(asset.metadata.frames)},
                        {QStringLiteral("gain_db"), 0.0},
                        {QStringLiteral("start_frame"), QStringLiteral("0")},
                        {QStringLiteral("stop_frame"), QStringLiteral("-1")},
                        {QStringLiteral("loop_start_frame"), QStringLiteral("-1")},
                        {QStringLiteral("loop_end_frame"), QStringLiteral("-1")},
                    };
                    QString referenceError;
                    if (!jam2::gui::appendJamTasterReferenceSection(
                            object, lane, referenceError)) {
                        throw std::runtime_error(referenceError.toStdString());
                    }
                }
                const QString renamedJamjar = staged.absoluteFilePath(
                    slug + QStringLiteral(".jamjar"));
                QSaveFile output(renamedJamjar);
                const QByteArray encoded = QJsonDocument(object).toJson(QJsonDocument::Indented);
                if (!output.open(QIODevice::WriteOnly) ||
                    output.write(encoded) != encoded.size() || !output.commit()) {
                    throw std::runtime_error("could not name the copied JamJar");
                }
                const QString originalJamjar = staged.absoluteFilePath(jamjars.front());
                if (QFileInfo(originalJamjar).absoluteFilePath() !=
                    QFileInfo(renamedJamjar).absoluteFilePath()) {
                    (void)QFile::remove(originalJamjar);
                }
                std::filesystem::rename(
                    nativeFilePath(partialDestination), nativeFilePath(destination));
            } catch (const std::exception& exception) {
                *error = QString::fromUtf8(exception.what());
                std::error_code ignored;
                std::filesystem::remove_all(nativeFilePath(partialDestination), ignored);
            }
        },
        [this, destination, error, sourceDisposition, sourcePath] {
            if (!error->isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("JamTaster"), *error);
                return;
            }
            appendLog(QStringLiteral("JamTaster song created: ") + destination);
            if (sourceDisposition == QStringLiteral("delete") &&
                QFileInfo(sourcePath).isFile() && !QFile::moveToTrash(sourcePath)) {
                QMessageBox::warning(
                    this, QStringLiteral("JamTaster"),
                    QStringLiteral(
                        "The new song was created, but the original source WAV could not "
                        "be moved to the Recycle Bin."));
            }
            QMessageBox::information(
                this, QStringLiteral("JamTaster"),
                QStringLiteral("Created %1. It can now be opened from Jam2.")
                    .arg(QFileInfo(destination).fileName()));
        });
    if (!started) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"),
                             QStringLiteral("The bounded file worker is busy."));
    }
}

void MainWindow::applyJamTasterConverted(
    const QString& convertedSong,
    const QJsonObject& options)
{
    const QDir converted(convertedSong);
    const QStringList jamjars = converted.entryList(
        {QStringLiteral("*.jamjar")}, QDir::Files, QDir::Name);
    if (jamjars.size() != 1) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"),
                             QStringLiteral("The converted JamJar result is incomplete."));
        return;
    }
    const bool applyStems = options.value(QStringLiteral("stems")).toBool();
    const bool applySections = options.value(QStringLiteral("sections")).toBool();
    const QString sourcePath = options.value(QStringLiteral("source_path")).toString();
    const QString sourceHash = options.value(QStringLiteral("source_hash")).toString();
    const QString sourceDisposition = applySections
        ? promptJamTasterSourceDisposition() : QString();
    if (applySections && sourceDisposition.isEmpty()) return;
    const QString jamjarPath = converted.absoluteFilePath(jamjars.front());
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = activeTrackSampleRate();
    const QJsonObject currentSnapshot = songToJson();
    auto generated = std::make_shared<QJsonObject>();
    auto error = std::make_shared<QString>();
    auto stagedPaths = std::make_shared<QStringList>();
    auto referenceLane = std::make_shared<QJsonObject>();
    const bool started = startFileWorkerTask(
        [jamjarPath, convertedSong, stagingFolder, expectedSampleRate,
         applyStems, applySections, currentSnapshot, generated, stagedPaths, error,
         sourceDisposition, sourcePath, sourceHash, referenceLane] {
            if (!ProjectPersistenceCoordinator::readSongJson(
                    jamjarPath, *generated, *error)) return;
            const auto sourceLaneMatches = [&sourceHash](const QJsonObject& lane) {
                return !sourceHash.isEmpty() &&
                    lane.value(QStringLiteral("asset_hash")).toString() == sourceHash;
            };
            if (sourceDisposition == QStringLiteral("keep")) {
                const QJsonArray existingBanks = currentSnapshot.value(QStringLiteral("looper"))
                    .toObject().value(QStringLiteral("banks")).toArray();
                for (const QJsonValue& bankValue : existingBanks) {
                    for (const QJsonValue& laneValue : bankValue.toObject()
                             .value(QStringLiteral("lanes")).toArray()) {
                        const QJsonObject lane = laneValue.toObject();
                        if (sourceLaneMatches(lane)) {
                            *referenceLane = lane;
                            break;
                        }
                    }
                    if (!referenceLane->isEmpty()) break;
                }
                if (referenceLane->isEmpty()) {
                    const StagedPcm16Asset asset = stagePcm16Asset(
                        sourcePath, stagingFolder, expectedSampleRate,
                        QStringLiteral("imported"));
                    if (!asset.error.isEmpty()) {
                        *error = asset.error;
                        return;
                    }
                    *referenceLane = QJsonObject{
                        {QStringLiteral("asset_path"), asset.stagedPath},
                        {QStringLiteral("asset_hash"), asset.sha256},
                        {QStringLiteral("sample_rate"), asset.metadata.sampleRate},
                        {QStringLiteral("source_frames"), QString::number(asset.metadata.frames)},
                        {QStringLiteral("gain_db"), 0.0},
                        {QStringLiteral("start_frame"), QStringLiteral("0")},
                        {QStringLiteral("stop_frame"), QStringLiteral("-1")},
                        {QStringLiteral("loop_start_frame"), QStringLiteral("-1")},
                        {QStringLiteral("loop_end_frame"), QStringLiteral("-1")},
                    };
                    stagedPaths->append(asset.stagedPath);
                }
            }
            if (!applyStems) return;
            QJsonObject looper = generated->value(QStringLiteral("looper")).toObject();
            QJsonArray banks = looper.value(QStringLiteral("banks")).toArray();
            const QJsonArray currentBanks = currentSnapshot.value(QStringLiteral("looper"))
                .toObject().value(QStringLiteral("banks")).toArray();
            constexpr int maxLanes =
                jam2::application::limits::kMaximumLooperLanesPerBank;
            for (int bankIndex = 0; bankIndex < banks.size(); ++bankIndex) {
                QJsonObject bank = banks.at(bankIndex).toObject();
                const QJsonArray candidates = bank.value(QStringLiteral("lanes")).toArray();
                QSet<QString> existingHashes;
                int currentLaneCount = 0;
                if (bankIndex < currentBanks.size()) {
                    for (const QJsonValue& lane : currentBanks.at(bankIndex).toObject()
                             .value(QStringLiteral("lanes")).toArray()) {
                        const QJsonObject currentLane = lane.toObject();
                        if (applySections && sourceLaneMatches(currentLane)) continue;
                        ++currentLaneCount;
                        const QString hash = currentLane.value(
                            QStringLiteral("asset_hash")).toString().toLower();
                        if (!hash.isEmpty()) existingHashes.insert(hash);
                    }
                }
                const int allowed = !applySections && bankIndex >= currentBanks.size()
                    ? 0 : qMax(0, maxLanes - currentLaneCount);
                QJsonArray lanes;
                for (const QJsonValue& candidate : candidates) {
                    if (lanes.size() >= allowed) break;
                    QJsonObject lane = candidate.toObject();
                    const QString relative = lane.value(QStringLiteral("asset_path")).toString();
                    const QString source = QDir(convertedSong).absoluteFilePath(relative);
                    const StagedPcm16Asset asset = stagePcm16Asset(
                        source, stagingFolder, expectedSampleRate,
                        QStringLiteral("imported"));
                    if (!asset.error.isEmpty()) {
                        *error = asset.error;
                        return;
                    }
                    const QString normalizedHash = asset.sha256.toLower();
                    if (!normalizedHash.isEmpty() && existingHashes.contains(normalizedHash)) {
                        continue;
                    }
                    lane.insert(QStringLiteral("asset_path"), asset.stagedPath);
                    lane.insert(QStringLiteral("asset_hash"), asset.sha256);
                    lane.insert(QStringLiteral("sample_rate"), asset.metadata.sampleRate);
                    lane.insert(QStringLiteral("source_frames"),
                                QString::number(asset.metadata.frames));
                    stagedPaths->append(asset.stagedPath);
                    lanes.append(lane);
                    if (!normalizedHash.isEmpty()) existingHashes.insert(normalizedHash);
                }
                bank.insert(QStringLiteral("lanes"), lanes);
                banks.replace(bankIndex, bank);
            }
            looper.insert(QStringLiteral("banks"), banks);
            generated->insert(QStringLiteral("looper"), looper);
        },
        [this, convertedSong, options, applySections, currentSnapshot,
         generated, stagedPaths, error, referenceLane, sourceDisposition,
         sourcePath, sourceHash] {
            if (!error->isEmpty()) {
                for (const QString& path : std::as_const(*stagedPaths)) {
                    (void)QFile::remove(path);
                }
                QMessageBox::warning(this, QStringLiteral("JamTaster"), *error);
                return;
            }
            QJsonObject next = currentSnapshot;
            const QJsonArray currentSections = next.value(QStringLiteral("sections")).toArray();
            const QJsonArray generatedSections = generated->value(QStringLiteral("sections")).toArray();
            const bool applyChords = options.value(QStringLiteral("chords")).toBool();
            const bool applyDrums = options.value(QStringLiteral("drums")).toBool();
            const bool applyBass = options.value(QStringLiteral("bass")).toBool();
            const bool applyStems = options.value(QStringLiteral("stems")).toBool();
            const bool muteSourceInPlace = !applySections &&
                (applyStems || applyChords || applyDrums || applyBass);
            const auto boundedArray = [](const QJsonValue& value, int maximum) {
                QJsonArray bounded;
                const QJsonArray source = value.toArray();
                for (int index = 0; index < qMin(source.size(), maximum); ++index) {
                    bounded.append(source.at(index));
                }
                return bounded;
            };
            const auto restSteps = [](int division) {
                QJsonArray steps;
                for (int step = 0; step < division; ++step) {
                    steps.append(QJsonObject{
                        {QStringLiteral("state"), QStringLiteral("rest")},
                        {QStringLiteral("value"), QString()},
                        {QStringLiteral("velocity"), 96},
                    });
                }
                return steps;
            };
            const auto compatibleSteps = [&restSteps](
                const QJsonObject& pattern, const QString& key, int division) {
                const QJsonArray steps = pattern.value(key).toArray();
                return steps.size() == division ? steps : restSteps(division);
            };

            QJsonArray mergedSections = applySections ? generatedSections : currentSections;
            const int count = qMin(mergedSections.size(), generatedSections.size());
            for (int index = 0; index < count; ++index) {
                QJsonObject target = mergedSections.at(index).toObject();
                const QJsonObject source = generatedSections.at(index).toObject();
                const QJsonObject previous = index < currentSections.size()
                    ? currentSections.at(index).toObject() : QJsonObject{};
                const int beats = target.value(QStringLiteral("beats")).toInt(8);
                if (applySections && !previous.isEmpty()) {
                    for (const QString& key : {
                             QStringLiteral("targets"), QStringLiteral("beat_notes"),
                             QStringLiteral("lyrics")}) {
                        target.insert(key, boundedArray(previous.value(key), beats));
                    }
                }
                if (applyChords) {
                    target.insert(QStringLiteral("chords"),
                                  boundedArray(source.value(QStringLiteral("chords")), beats));
                } else if (applySections) {
                    target.insert(QStringLiteral("chords"), boundedArray(
                        previous.value(QStringLiteral("chords")), beats));
                }
                if (applyDrums) {
                    target.insert(QStringLiteral("beat_patterns"),
                                  boundedArray(source.value(QStringLiteral("beat_patterns")), beats));
                    target.insert(QStringLiteral("drum_kit"),
                                  source.value(QStringLiteral("drum_kit")));
                } else if (applySections) {
                    target.insert(QStringLiteral("beat_patterns"), boundedArray(
                        previous.value(QStringLiteral("beat_patterns")), beats));
                    if (!previous.isEmpty()) {
                        target.insert(QStringLiteral("drum_kit"),
                                      previous.value(QStringLiteral("drum_kit")));
                    }
                }
                if (applyChords || applyBass) {
                    const QJsonArray basePatterns = !previous.isEmpty()
                        ? previous.value(QStringLiteral("musical_patterns")).toArray()
                        : (applySections ? QJsonArray{} : target.value(
                            QStringLiteral("musical_patterns")).toArray());
                    const QJsonArray sourcePatterns = source.value(
                        QStringLiteral("musical_patterns")).toArray();
                    QJsonArray targetPatterns = applySections
                        ? QJsonArray{} : boundedArray(basePatterns, beats);
                    for (int beat = 0; beat < qMin(sourcePatterns.size(), beats); ++beat) {
                        const QJsonObject sourcePattern = sourcePatterns.at(beat).toObject();
                        const QJsonObject basePattern = beat < basePatterns.size()
                            ? basePatterns.at(beat).toObject() : QJsonObject{};
                        const int division = sourcePattern.value(
                            QStringLiteral("division")).toInt(4);
                        QJsonObject targetPattern{
                            {QStringLiteral("division"), division},
                            {QStringLiteral("chords"), applyChords
                                ? sourcePattern.value(QStringLiteral("chords"))
                                : compatibleSteps(basePattern, QStringLiteral("chords"), division)},
                            {QStringLiteral("bass"), applyBass
                                ? sourcePattern.value(QStringLiteral("bass"))
                                : compatibleSteps(basePattern, QStringLiteral("bass"), division)},
                            {QStringLiteral("melody"), compatibleSteps(
                                basePattern, QStringLiteral("melody"), division)},
                            {QStringLiteral("support"), compatibleSteps(
                                basePattern, QStringLiteral("support"), division)},
                        };
                        if (beat < targetPatterns.size()) {
                            targetPatterns.replace(beat, targetPattern);
                        } else {
                            targetPatterns.append(targetPattern);
                        }
                    }
                    target.insert(QStringLiteral("musical_patterns"), targetPatterns);
                } else if (applySections) {
                    target.insert(QStringLiteral("musical_patterns"),
                                  boundedArray(previous.value(
                                      QStringLiteral("musical_patterns")), beats));
                }
                mergedSections.replace(index, target);
            }
            next.insert(QStringLiteral("sections"), mergedSections);

            QJsonObject currentLooper = next.value(QStringLiteral("looper")).toObject();
            const QJsonObject generatedLooper = generated->value(QStringLiteral("looper")).toObject();
            QJsonArray currentBanks = currentLooper.value(QStringLiteral("banks")).toArray();
            if (!sourceHash.isEmpty()) {
                for (int bank = 0; bank < currentBanks.size(); ++bank) {
                    QJsonObject currentBank = currentBanks.at(bank).toObject();
                    QJsonArray kept;
                    for (const QJsonValue& laneValue : currentBank.value(
                             QStringLiteral("lanes")).toArray()) {
                        QJsonObject lane = laneValue.toObject();
                        if (lane.value(QStringLiteral("asset_hash")).toString() == sourceHash) {
                            if (!applySections) {
                                if (muteSourceInPlace) {
                                    lane.insert(QStringLiteral("muted"), true);
                                }
                                kept.append(lane);
                            }
                        } else {
                            kept.append(laneValue);
                        }
                    }
                    currentBank.insert(QStringLiteral("lanes"), kept);
                    currentBanks.replace(bank, currentBank);
                }
            }
            const QJsonArray generatedBanks = generatedLooper.value(QStringLiteral("banks")).toArray();
            QJsonArray mergedBanks = applySections ? generatedBanks : currentBanks;
            for (int bank = 0; bank < mergedBanks.size(); ++bank) {
                QJsonObject mergedBank = mergedBanks.at(bank).toObject();
                const QJsonObject currentBank = bank < currentBanks.size()
                    ? currentBanks.at(bank).toObject() : QJsonObject{};
                const QJsonObject generatedBank = bank < generatedBanks.size()
                    ? generatedBanks.at(bank).toObject() : QJsonObject{};
                QJsonArray lanes = currentBank.value(QStringLiteral("lanes")).toArray();
                if (applyStems) {
                    for (const QJsonValue& lane : generatedBank.value(
                             QStringLiteral("lanes")).toArray()) {
                        lanes.append(lane);
                    }
                }
                mergedBank.insert(QStringLiteral("lanes"), lanes);
                if (!options.value(QStringLiteral("tempo")).toBool() &&
                    !currentBank.isEmpty()) {
                    mergedBank.insert(QStringLiteral("timing"),
                                      currentBank.value(QStringLiteral("timing")));
                }
                mergedBanks.replace(bank, mergedBank);
            }
            currentLooper.insert(QStringLiteral("banks"), mergedBanks);
            if (applySections) {
                currentLooper.insert(QStringLiteral("arrangement"),
                                     generatedLooper.value(QStringLiteral("arrangement")));
            }
            next.insert(QStringLiteral("looper"), currentLooper);
            next.insert(QStringLiteral("title"), chordModel_.title());
            if (sourceDisposition == QStringLiteral("keep") &&
                !jam2::gui::appendJamTasterReferenceSection(
                    next, *referenceLane, *error)) {
                for (const QString& path : std::as_const(*stagedPaths)) {
                    (void)QFile::remove(path);
                }
                QMessageBox::warning(this, QStringLiteral("JamTaster"), *error);
                return;
            }
            if (!loadSongJson(next)) {
                for (const QString& path : std::as_const(*stagedPaths)) {
                    (void)QFile::remove(path);
                }
                QMessageBox::warning(
                    this, QStringLiteral("JamTaster"),
                    QStringLiteral("The selected JamTaster results could not be merged into this jam."));
                return;
            }
            if (applyStems || sourceDisposition == QStringLiteral("keep")) {
                for (const QString& path : std::as_const(*stagedPaths)) {
                    registerTransientTrackWav(path);
                }
            }
            if (options.value(QStringLiteral("tempo")).toBool()) {
                QDir source(QFileInfo(convertedSong).absolutePath());
                if (source.cdUp()) {
                    QFile tempoFile(source.absoluteFilePath(QStringLiteral("tempo.json")));
                    if (tempoFile.open(QIODevice::ReadOnly)) {
                        const QJsonDocument tempo = QJsonDocument::fromJson(tempoFile.readAll());
                        if (tempo.isObject()) applyJamTasterTempo(tempo.object());
                    }
                }
            }
            jamStorage_.markArtifactCreated();
            refreshSongViews();
            regeneratePreparedMix();
            syncLooperArrangement();
            if (sourceDisposition == QStringLiteral("delete") &&
                QFileInfo(sourcePath).isFile() && !QFile::moveToTrash(sourcePath)) {
                QMessageBox::warning(
                    this, QStringLiteral("JamTaster"),
                    QStringLiteral(
                        "The results were applied, but the original source WAV could not "
                        "be moved to the Recycle Bin."));
            } else if (sourceDisposition == QStringLiteral("keep")) {
                QMessageBox::information(
                    this, QStringLiteral("JamTaster"),
                    QStringLiteral(
                        "The original source WAV is muted in the final Original Reference section."));
            }
            appendLog(QStringLiteral("selected JamTaster results applied to current jam"));
        });
    if (!started) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"),
                             QStringLiteral("The bounded file worker is busy."));
    }
}

void MainWindow::addLooperWavs()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("track import ignored while a synced recording is active"));
        return;
    }
    QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Add WAV lanes"),
        QString(),
        QStringLiteral("WAV files (*.wav *.WAV)"),
        nullptr,
        QFileDialog::Options{});
    if (paths.isEmpty()) {
        return;
    }
    constexpr int maxLanesPerBank =
        jam2::application::limits::kMaximumLooperLanesPerBank;
    const int bankIndex = viewedBankIndex_;
    const int remaining = qMax(0, maxLanesPerBank - looperProject_.banks().at(bankIndex).lanes.size());
    if (paths.size() > remaining) {
        appendLog(QStringLiteral("WAV import limited to %1 remaining lane slots; rejected=%2")
            .arg(remaining)
            .arg(paths.size() - remaining));
        paths = paths.mid(0, remaining);
    }
    if (paths.isEmpty()) {
        return;
    }
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = activeTrackSampleRate();
    auto results = std::make_shared<std::vector<StagedPcm16Asset>>();
    results->reserve(static_cast<std::size_t>(paths.size()));
    (void)startFileWorkerTask(
        [paths, stagingFolder, expectedSampleRate, results] {
            for (const QString& path : paths) {
                results->push_back(stagePcm16Asset(path, stagingFolder, expectedSampleRate));
            }
        },
        [this, bankIndex, results] {
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
                return;
            }
            int imported = 0;
            QStringList failures;
            for (const StagedPcm16Asset& asset : *results) {
                if (!asset.error.isEmpty()) {

                    appendLog(QStringLiteral("could not import WAV %1: %2")
                        .arg(asset.sourcePath, asset.error));
                    if (failures.size() < 8) {
                        failures.append(QFileInfo(asset.sourcePath).fileName() + QStringLiteral(": ") + asset.error);
                    }
                    continue;
                }
                if (asset.resampled) {
                    appendLog(QStringLiteral(
                        "WAV import resampled %1: source_rate=%2 target_rate=%3 source_frames=%4 output_frames=%5")
                        .arg(QFileInfo(asset.sourcePath).fileName())
                        .arg(asset.sourceSampleRate)
                        .arg(asset.metadata.sampleRate)
                        .arg(asset.sourceFrames)
                        .arg(asset.metadata.frames));
                }
                registerTransientTrackWav(asset.stagedPath);
                looperWaveformCache_.remove(asset.stagedPath);
                LooperLane lane;
                lane.assetPath = asset.stagedPath;
                lane.assetHash = asset.sha256;
                lane.name = asset.displayName;
                lane.sampleRate = asset.metadata.sampleRate;
                lane.sampleRateCompatible = true;
                lane.sourceFrames = asset.metadata.frames;
                lane.originKind = QStringLiteral("imported");
                if (!looperProject_.appendLane(bankIndex, std::move(lane))) {
                    appendLog(QStringLiteral("could not add WAV lane: ") + asset.sourcePath);
                    continue;
                }
                ++imported;
            }
            appendLog(QStringLiteral("WAV import worker completed: imported=%1 requested=%2 active=%3 high_water=%4 completed=%5 rejected=%6")
                .arg(imported)
                .arg(static_cast<qulonglong>(results->size()))
                .arg(fileWorkerTasksActive_)
                .arg(fileWorkerTasksHighWater_)
                .arg(fileWorkerTasksCompleted_)
                .arg(fileWorkerTasksRejected_));
            if (!failures.isEmpty()) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("WAV Import"),
                    QStringLiteral("Some WAVs were not loaded:\n\n") + failures.join(QStringLiteral("\n")));
            }
            refreshLooperLanes();
            regeneratePreparedMix();
            if (imported > 0) syncLooperArrangement();
            if (imported > 0 && automaticWavSharingEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        });
}

bool MainWindow::importWavIntoLooperLane(int laneIndex, const QString& sourcePath)
{
    if (sharedRecordingProtected()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Import WAV"),
            QStringLiteral("Audio cannot be imported while a synced recording is active."));
        return false;
    }
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        QMessageBox::warning(
            this, QStringLiteral("Import WAV"), QStringLiteral("The dropped file could not be found."));
        return false;
    }
    if (sourceInfo.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this, QStringLiteral("Import WAV"), QStringLiteral("Only WAV audio files can be imported into a track."));
        return false;
    }
    const int bankIndex = viewedBankIndex_;
    if (bankIndex < 0 || bankIndex >= looperProject_.banks().size() ||
        laneIndex < -1 || laneIndex >= looperProject_.banks().at(bankIndex).lanes.size()) {
        QMessageBox::warning(
            this, QStringLiteral("Import WAV"), QStringLiteral("The selected track lane is no longer available."));
        return false;
    }
    const QString targetLaneId = laneIndex >= 0
        ? looperProject_.banks().at(bankIndex).lanes.at(laneIndex).id
        : QString{};
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = activeTrackSampleRate();
    auto result = std::make_shared<StagedPcm16Asset>();
    const bool started = startFileWorkerTask(
        [sourcePath, stagingFolder, expectedSampleRate, result] {
            *result = stagePcm16Asset(sourcePath, stagingFolder, expectedSampleRate);
        },
        [this, bankIndex, targetLaneId, result] {
            if (!result->error.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Load WAV"), result->error);
                return;
            }
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
                return;
            }
            if (result->resampled) {
                appendLog(QStringLiteral(
                    "WAV import resampled %1: source_rate=%2 target_rate=%3 source_frames=%4 output_frames=%5")
                    .arg(QFileInfo(result->sourcePath).fileName())
                    .arg(result->sourceSampleRate)
                    .arg(result->metadata.sampleRate)
                    .arg(result->sourceFrames)
                    .arg(result->metadata.frames));
            }
            int laneIndex = -1;
            LooperLane candidate;
            QString replacedPath;
            QString replacedHash;
            if (!targetLaneId.isEmpty()) {
                const auto& currentLanes = looperProject_.banks().at(bankIndex).lanes;
                for (int index = 0; index < currentLanes.size(); ++index) {
                    if (currentLanes.at(index).id == targetLaneId) {
                        laneIndex = index;
                        break;
                    }
                }
                if (laneIndex < 0) {
                    QMessageBox::warning(this, QStringLiteral("Load WAV"), QStringLiteral("The selected track lane is no longer available."));
                    return;
                }
                candidate = looperProject_.banks().at(bankIndex).lanes.at(laneIndex);
                replacedPath = looperAssetAbsolutePath(candidate);
                replacedHash = candidate.assetHash;
            } else {
                candidate.name = result->displayName;
            }
            candidate.assetPath = result->stagedPath;
            candidate.assetHash = result->sha256;
            candidate.sampleRate = result->metadata.sampleRate;
            candidate.sampleRateCompatible = true;
            candidate.sourceFrames = result->metadata.frames;
            candidate.originKind = QStringLiteral("imported");
            candidate.referenceKind.clear();
            candidate.referenceSourceSignature.clear();
            candidate.referenceBpm = 0.0;
            candidate.referenceStale = false;
            candidate.localOnly = false;
            if (isDefaultEmptyTrackName(candidate.name) ||
                candidate.name.trimmed().isEmpty()) {
                candidate.name = result->displayName;
            }
            candidate.startFrame = 0;
            candidate.stopFrame = -1;
            candidate.loopStartFrame = -1;
            candidate.loopEndFrame = -1;
            candidate.loopEnabled = false;
            const bool attached = targetLaneId.isEmpty()
                ? looperProject_.appendLane(bankIndex, std::move(candidate))
                : looperProject_.replaceLane(
                    bankIndex, laneIndex, std::move(candidate));
            if (!attached) {
                QMessageBox::warning(this, QStringLiteral("Load WAV"),
                    targetLaneId.isEmpty()
                        ? QStringLiteral("Could not create a track lane.")
                        : QStringLiteral("The imported WAV state was invalid and was not attached."));
                return;
            }
            if (targetLaneId.isEmpty()) {
                laneIndex = looperProject_.banks().at(bankIndex).lanes.size() - 1;
            }
            registerTransientTrackWav(result->stagedPath);
            looperWaveformCache_.remove(result->stagedPath);
            if (replacedHash != result->sha256) {
                cancelUnreferencedLooperAssetTransfer(replacedHash);
            }
            if (!replacedPath.isEmpty() &&
                QDir::cleanPath(replacedPath) !=
                    QDir::cleanPath(result->stagedPath)) {
                discardObsoleteReferenceWavs(QSet<QString>{replacedPath});
            }
            selectedLooperLane_ = laneIndex;
            refreshLooperLanes();
            regeneratePreparedMix();
            syncLooperArrangement();
            if (automaticWavSharingEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        });
    if (!started) {
        QMessageBox::warning(
            this,
            QStringLiteral("Import WAV"),
            QStringLiteral("The audio worker is busy. Please try importing the WAV again."));
    }
    return started;
}

void MainWindow::addEmptyLooperLane()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("new track ignored while a synced recording is active"));
        return;
    }
    LooperLane lane;
    if (!looperProject_.appendLane(viewedBankIndex_, std::move(lane))) {
        appendLog(QStringLiteral("could not add empty lane"));
        return;
    }
    refreshLooperLanes();
    const int row = looperProject_.banks().at(viewedBankIndex_).lanes.size() - 1;
    selectedLooperLane_ = row;
    refreshLooperLanes();
    syncLooperArrangement();
}

bool MainWindow::armSelectedLooperLaneRecording()
{
    if (viewedBankIndex_ != looperProject_.activeBankIndex()) {
        QMessageBox::information(
            this,
            QStringLiteral("Arm Lane Recording"),
            QStringLiteral("Recording can only be armed in the currently active Section."));
        return false;
    }
    if (selectedLooperLane_ < 0) {
        if (looperProject_.banks().at(viewedBankIndex_).lanes.isEmpty()) {
            addEmptyLooperLane();
        } else {
            selectedLooperLane_ = 0;
        }
    }
    if (selectedLooperLane_ < 0) {
        return false;
    }

    const int bankIndex = viewedBankIndex_;
    const int laneIndex = selectedLooperLane_;
    const LooperLane& lane = looperProject_.banks().at(bankIndex).lanes.at(laneIndex);
    const QString bankId = looperProject_.banks().at(bankIndex).id;
    const QString laneId = lane.id;
    const QString laneName = lane.name;

    return showLaneRecordingDialog(bankId, laneId, laneName);
}


bool MainWindow::showLaneRecordingDialog(
    const QString& bankId,
    const QString& laneId,
    const QString& laneName)
{
    LaneRecordingDialog::Input input;
    input.laneName = laneName;
    input.preferredMode = preferences_.recording.preferredMode;
    input.engineRunning = jam2_.isRunning();
    input.leaderAudio = leaderAudioModeActive();
    input.inputSources.push_back({QStringLiteral("Combined My Send mix"), -1});
    const Jam2RuntimeOptions sourceOptions = runtimeOptions();
    if (auto* sourceRouter = jam2_.inputSourceRouter()) {
        for (std::size_t slot = 0; slot < sourceRouter->physical_channels() &&
             slot < audioPluginSources_.size(); ++slot) {
            const auto& source = audioPluginSources_[slot];
            if (source.firstChannel == jam2::audio::kNoInputChannel) continue;
            const int firstNumber = source.firstChannel <
                    sourceOptions.channel_selection.input.size()
                ? sourceOptions.channel_selection.input[source.firstChannel] + 1
                : static_cast<int>(source.firstChannel + 1);
            QString name = QStringLiteral("Input %1").arg(firstNumber);
            if (source.secondChannel != jam2::audio::kNoInputChannel) {
                const int secondNumber = source.secondChannel <
                        sourceOptions.channel_selection.input.size()
                    ? sourceOptions.channel_selection.input[source.secondChannel] + 1
                    : static_cast<int>(source.secondChannel + 1);
                name = QStringLiteral("Inputs %1 + %2 (plugin output -> mono)")
                    .arg(firstNumber).arg(secondNumber);
            }
            if (!source.name.isEmpty()) name += QStringLiteral(" - %1").arg(source.name);
            input.inputSources.push_back({name, static_cast<qulonglong>(slot)});
        }
    }
    for (const auto& source : midiPluginSources_) {
        if (!source || !source->host) continue;
        input.inputSources.push_back({
            QStringLiteral("%1 - %2")
                .arg(QString::fromStdString(source->deviceInfo.name), source->pluginName),
            static_cast<qulonglong>(source->routerSlot),
        });
    }
    input.selectedInputSource = recordingInputSourceSlot_;
    const auto loopbackChoices = [this] {
        QVector<LaneRecordingDialog::Choice> choices;
        choices.reserve(laneRecordingState_.loopbackSources.size());
        for (const LoopbackSourceChoice& source : laneRecordingState_.loopbackSources) {
            choices.push_back({source.label, source.id});
        }
        return choices;
    };
    input.loopbackSources = loopbackChoices();

    const QString laneFileName = safeFileName(laneName).isEmpty()
        ? QStringLiteral("lane") : safeFileName(laneName);
    input.inputOutputPath = timestampedCapturePath(
        laneFileName,
        resolvedManagedCaptureFolder(
            preferences_.recording.input.outputFolder,
            jamAssetFolder(JamStorage::AssetKind::Recorded)));
    input.loopbackOutputPath = timestampedCapturePath(
        laneFileName + QStringLiteral("-loopback"),
        resolvedManagedCaptureFolder(
            preferences_.recording.loopback.outputFolder,
            jamAssetFolder(JamStorage::AssetKind::Recorded)));
    input.jamMixOutputPath = timestampedCapturePath(
        laneFileName + QStringLiteral("-jam-mix"),
        jamAssetFolder(JamStorage::AssetKind::Recorded));
    input.input = preferences_.recording.input;
    input.loopback = preferences_.recording.loopback;
    input.jamMix = preferences_.recording.jamMixTrack;
    input.latencySummary = recordingLatencySummary();

    LaneRecordingDialog dialog(
        std::move(input),
        [this](const QString& current) {
            return QFileDialog::getSaveFileName(
                this,
                QStringLiteral("Take WAV"),
                isAutoCapturePath(current)
                    ? timestampedCapturePath(
                        QStringLiteral("take"),
                        jamAssetFolder(JamStorage::AssetKind::Recorded))
                    : current,
                QStringLiteral("WAV files (*.wav);;All files (*)"),
                nullptr,
                QFileDialog::Options{});
        },
        [this, loopbackChoices] {
            refreshLoopbackSources();
            return loopbackChoices();
        },
        this);
    if (dialog.exec() != QDialog::Accepted) return false;
    const LaneRecordingDialog::Result result = dialog.result();

    laneRecordingState_.outputPath = result.outputPath;
    const bool loopback = result.mode == QStringLiteral("loopback");
    const InputRecordingPreference& inputDraft = result.input;
    const LoopbackRecordingPreference& loopbackDraft = result.loopback;
    captureManualStopCheck_->setChecked(
        loopback ? loopbackDraft.recordUntilStopped : inputDraft.recordUntilStopped);
    captureDurationSpin_->setValue(
        loopback ? loopbackDraft.durationBars : inputDraft.durationBars);
    captureCountInCheck_->setChecked(inputDraft.countIn);
    captureCountInBarsSpin_->setValue(inputDraft.countInBars);
    captureCountInMetronomeCheck_->setChecked(inputDraft.countInMetronome);
    captureKeepMetronomeCheck_->setChecked(inputDraft.keepMetronome);
    const int previousLatencyAdjustment =
        laneRecordingState_.latencyAdjustmentFrames;
    laneRecordingState_.latencyAdjustmentFrames =
        qBound(-8192, inputDraft.latencyAdjustmentFrames, 8192);
    laneRecordingState_.silenceThresholdDb =
        qBound(-120.0, loopbackDraft.silenceThresholdDb, 0.0);
    laneRecordingState_.tailSilenceMs =
        qBound(0, loopbackDraft.tailSilenceMs, 30000);
    laneRecordingState_.trimLeading = loopbackDraft.trimLeading;
    laneRecordingState_.trimTrailing = loopbackDraft.trimTrailing;
    laneRecordingState_.loopbackSourceId = loopbackDraft.sourceId.trimmed();
    laneRecordingState_.loopbackSourceName = loopbackDraft.sourceName.trimmed();
    if (laneRecordingState_.loopbackSourceId.isEmpty()) {
        laneRecordingState_.loopbackSourceId =
            laneRecordingState_.loopbackSourceName.isEmpty()
            ? QStringLiteral("default")
            : laneRecordingState_.loopbackSourceName;
    }
    if (laneRecordingState_.loopbackSourceName.isEmpty()) {
        laneRecordingState_.loopbackSourceName =
            laneRecordingState_.loopbackSourceId == QStringLiteral("default")
            ? QStringLiteral("[default] System mix")
            : laneRecordingState_.loopbackSourceId;
    }
    if (jam2_.isRunning() &&
        laneRecordingState_.latencyAdjustmentFrames != previousLatencyAdjustment) {
        jam2::EngineCommand command;
        command.type = jam2::EngineCommandType::SetRecordingLatencyAdjustment;
        command.signed_value = laneRecordingState_.latencyAdjustmentFrames;
        (void)submitEngineCommand(
            command, QStringLiteral("recording latency adjustment"));
    }

    if (result.mode == QStringLiteral("input")) {
        recordingInputSourceSlot_ = result.inputSource;
        if (auto* sourceRouter = jam2_.inputSourceRouter()) {
            sourceRouter->set_recording_source(recordingInputSourceSlot_.value_or(
                jam2::audio::kCombinedInputSources));
        }
    }
    const TrackRecordingWorkflow::CaptureMode captureMode = loopback
        ? TrackRecordingWorkflow::CaptureMode::Loopback
        : result.mode == QStringLiteral("current-jam")
            ? TrackRecordingWorkflow::CaptureMode::CurrentJam
            : TrackRecordingWorkflow::CaptureMode::Input;
    const LooperLaneLocation resolved = findLooperLaneLocation(
        looperProject_, bankId, laneId);
    if (!resolved.valid() || viewedBankIndex_ != resolved.bank ||
        looperProject_.activeBankIndex() != resolved.bank) {
        refreshLooperLanes();
        appendLog(QStringLiteral(
            "lane recording was not armed because the shared arrangement changed while the dialog was open"));
        QMessageBox::warning(
            this,
            QStringLiteral("Arm Lane Recording"),
            QStringLiteral(
                "The selected lane changed while the shared arrangement was updating. Select it and arm again."));
        return false;
    }
    selectedLooperLane_ = resolved.lane;
    trackRecordingWorkflow_.armLane(
        resolved.bank,
        resolved.lane,
        captureMode,
        result.includeBackingTrack,
        result.includeMetronome);
    localRecordingTargetBankId_ = looperProject_.banks().at(resolved.bank).id;
    localRecordingTargetLaneId_ = looperProject_.banks().at(resolved.bank)
        .lanes.at(resolved.lane).id;
    recordedLaneImportStatus_ = QStringLiteral("armed");
    recordedLaneImportTargetId_ = localRecordingTargetLaneId_;
    recordedLaneImportLastHash_.clear();
    localTrackRecordingCountInBars_ = 0;
    publishLocalTrackRecordingState(QStringLiteral("armed"));
    refreshLooperLanes();
    appendLog(QStringLiteral("armed lane recording: bank=%1 lane=%2 mode=%3")
        .arg(looperProject_.banks().at(resolved.bank).id)
        .arg(looperProject_.banks().at(resolved.bank).lanes.at(resolved.lane).name)
        .arg(loopback ? QStringLiteral("loopback")
            : captureMode == TrackRecordingWorkflow::CaptureMode::CurrentJam
                ? QStringLiteral("current-jam") : QStringLiteral("input")));
    return true;
}

void MainWindow::startArmedLooperLaneRecording()
{
    if (loopbackRecordingStartFrame_ > 0) {
        cancelLoopbackCountIn();
        return;
    }
    const bool localCaptureActive = trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning();
    if (localCaptureActive) {
        runGridLockedEngineAction(
            QStringLiteral("track.record-stop"),
            [this](std::uint64_t targetFrame) { stopInputCapture(targetFrame); },
            true);
        return;
    }
    if (sharedRecordingProtected() && activeRecordingGroupId_.isEmpty()) {
        appendLog(QStringLiteral("recording start ignored while another synced take is active"));
        return;
    }
    if (!activeRecordingGroupId_.isEmpty()) {
        appendLog(QStringLiteral(
            "recording start ignored because the current synced group has already started"));
        return;
    }
    if (!trackRecordingWorkflow_.laneArmed()) {
        if (!armSelectedLooperLaneRecording()) {
            return;
        }
    }
    const bool engineCapture =
        trackRecordingWorkflow_.captureMode() != TrackRecordingWorkflow::CaptureMode::Loopback;
    localLaneTakeForcedLocal_ = !engineCapture;
    if (syncedRecordingsEnabled() && jam2_.isNetworkRunning() && engineCapture) {
        if (localTrackRecordingPhase_ == QStringLiteral("ready")) {
            localTrackRecordingCountInBars_ = 0;
            publishLocalTrackRecordingState(QStringLiteral("armed"));
            appendLog(QStringLiteral("cancelled readiness for the synced recording group"));
            return;
        }
        localTrackRecordingCountInBars_ = captureCountInCheck_ &&
                captureCountInCheck_->isChecked()
            ? qMax(1, captureCountInBarsSpin_ ? captureCountInBarsSpin_->value() : 1)
            : 0;
        publishLocalTrackRecordingState(QStringLiteral("ready"));
        appendLog(QStringLiteral(
            "ready for synced recording; waiting for every armed participant"));
        maybeStartReadyRecordingGroup();
        return;
    }
    if (captureCountInCheck_ && captureCountInCheck_->isChecked()) {
        const bool countInMetronome = captureCountInMetronomeCheck_ && captureCountInMetronomeCheck_->isChecked();
        const bool keepMetronome = captureKeepMetronomeCheck_ && captureKeepMetronomeCheck_->isChecked();
        const bool startedCountInClick = countInMetronome &&
            !metronomeTransport_.localRunning();
        if (startedCountInClick) {
            startTrackMetronome();
        }
        const int bars = qMax(1, captureCountInBarsSpin_ ? captureCountInBarsSpin_->value() : 1);
        const QString countInText = QStringLiteral("Recording: waiting for engine count-in schedule (%1 bar%2)")
            .arg(bars)
            .arg(bars == 1 ? QString{} : QStringLiteral("s"));
        if (gridScheduleLabel_) {
            gridScheduleLabel_->setText(countInText);
        }
        appendLog(countInText);
        if (engineCapture) {
            trackRecordingWorkflow_.waitForCountIn(
                0,
                startedCountInClick && !keepMetronome);
            startInputCapture(0, bars);
        } else {
            if (!scheduleLoopbackCountIn(
                    bars, startedCountInClick && !keepMetronome) &&
                startedCountInClick) {
                stopTrackMetronome();
            }
        }
    } else {
        if (engineCapture) {
            startInputCapture(0, 0);
        } else {
            startArmedLooperLaneRecordingNow(0);
        }
    }
}

void MainWindow::startArmedLooperLaneRecordingNow(std::uint64_t targetFrame)
{
    if (trackRecordingWorkflow_.captureMode() == TrackRecordingWorkflow::CaptureMode::Loopback) {
        startLoopbackCapture();
    } else {
        startInputCapture(targetFrame);
    }
}

void MainWindow::publishLocalTrackRecordingState(
    const QString& phase,
    int countInRemaining,
    bool force)
{
    static const QSet<QString> validPhases{
        QStringLiteral("idle"),
        QStringLiteral("armed"),
        QStringLiteral("ready"),
        QStringLiteral("waiting"),
        QStringLiteral("count-in"),
        QStringLiteral("recording"),
        QStringLiteral("complete"),
        QStringLiteral("finalizing"),
    };
    if (!validPhases.contains(phase)) {
        return;
    }
    if (!force && localTrackRecordingPhase_ == phase &&
        localTrackRecordingCountInRemaining_ == countInRemaining) {
        updateSharedRecordingPresentation();
        return;
    }
    localTrackRecordingPhase_ = phase;
    localTrackRecordingCountInRemaining_ = countInRemaining;
    localTrackRecordingStateRevision_ =
        localTrackRecordingStateRevision_ >= (std::numeric_limits<int>::max)()
        ? 1 : localTrackRecordingStateRevision_ + 1;

    int bank = qBound(
        0, trackRecordingWorkflow_.armedBank(), looperProject_.banks().size() - 1);
    QString laneId;
    QString laneName;
    if (trackRecordingWorkflow_.laneArmed() &&
        bank < looperProject_.banks().size() &&
        trackRecordingWorkflow_.armedLane() >= 0 &&
        trackRecordingWorkflow_.armedLane() < looperProject_.banks().at(bank).lanes.size()) {
        const LooperLane& lane = looperProject_.banks().at(bank).lanes.at(
            trackRecordingWorkflow_.armedLane());
        laneId = lane.id;
        laneName = lane.name;
    }
    updateSharedRecordingPresentation();
    updateLaneRecordingIsolation();
    if (localLaneTakeForcedLocal_ || !syncedRecordingsEnabled() ||
        !jam2_.isNetworkRunning()) {
        return;
    }
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("looper.recording.state")},
        {QStringLiteral("state_revision"), localTrackRecordingStateRevision_},
        {QStringLiteral("phase"), phase},
        {QStringLiteral("bank"), bank},
        {QStringLiteral("lane_id"), laneId},
        {QStringLiteral("lane_name"), laneName},
        {QStringLiteral("count_in_remaining"), qBound(0, countInRemaining, 4096)},
        {QStringLiteral("count_in_bars"), qBound(0, localTrackRecordingCountInBars_, 8)},
        {QStringLiteral("group_id"), activeRecordingGroupId_},
    });
    maybeStartReadyRecordingGroup();
    maybeFinishRecordingGroup();
}

void MainWindow::handleTrackRecordingState(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    if (!syncedRecordingsEnabled()) return;
    const QString routedSource = message.value(
        QStringLiteral("source_peer_token")).toString();
    const QString source = routedSource.isEmpty() ? sourcePeerToken : routedSource;
    const QString localToken = sessionController_.snapshot().localToken;
    if (source.isEmpty() || source == localToken) {
        return;
    }
    const int revision = message.value(QStringLiteral("state_revision")).toInt();
    if (revision <= peerTrackRecordingRevisions_.value(source, -1)) {
        return;
    }
    peerTrackRecordingRevisions_[source] = revision;
    const QString phase = message.value(QStringLiteral("phase")).toString();
    const QString messageGroupId = message.value(QStringLiteral("group_id")).toString();
    if (phase != QStringLiteral("idle") &&
        recoveredRecordingGroupIds_.contains(messageGroupId)) {
        return;
    }
    if (phase == QStringLiteral("idle")) {
        peerTrackRecordingStates_.remove(source);
    } else {
        PeerTrackRecordingState state;
        state.phase = phase;
        state.bank = message.value(QStringLiteral("bank")).toInt();
        state.laneId = message.value(QStringLiteral("lane_id")).toString();
        state.laneName = message.value(QStringLiteral("lane_name")).toString();
        state.countInRemaining = message.value(
            QStringLiteral("count_in_remaining")).toInt();
        state.countInBars = message.value(QStringLiteral("count_in_bars")).toInt();
        state.groupId = messageGroupId;
        state.revision = revision;
        peerTrackRecordingStates_[source] = std::move(state);
    }
    updateSharedRecordingPresentation();
    refreshLooperLanes();
    maybeStartReadyRecordingGroup();
    maybeFinishRecordingGroup();
}

void MainWindow::maybeStartReadyRecordingGroup()
{
    if (!sessionController_.isServer() || !syncedRecordingsEnabled() ||
        !jam2_.isNetworkRunning() || !activeRecordingGroupId_.isEmpty()) {
        return;
    }
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    QStringList participants;
    int countInBars = 0;
    bool hasArmedParticipant = false;
    bool everyoneReady = true;
    if (trackRecordingWorkflow_.laneArmed() &&
        (localTrackRecordingPhase_ == QStringLiteral("armed") ||
         localTrackRecordingPhase_ == QStringLiteral("ready"))) {
        hasArmedParticipant = true;
        if (localTrackRecordingPhase_ == QStringLiteral("ready")) {
            participants.append(session.localToken);
            countInBars = qMax(countInBars, localTrackRecordingCountInBars_);
        } else {
            everyoneReady = false;
        }
    }
    for (auto it = peerTrackRecordingStates_.cbegin();
         it != peerTrackRecordingStates_.cend(); ++it) {
        if (it->phase != QStringLiteral("armed") &&
            it->phase != QStringLiteral("ready")) {
            continue;
        }
        hasArmedParticipant = true;
        if (it->phase == QStringLiteral("ready")) {
            participants.append(it.key());
            countInBars = qMax(countInBars, it->countInBars);
        } else {
            everyoneReady = false;
        }
    }
    if (!hasArmedParticipant || !everyoneReady || participants.isEmpty()) return;

    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    const int beatsPerBar = qMax(1, currentMetronomePattern().beats_per_bar);
    if (!position.engineAnchored || !position.running ||
        position.sampleRate <= 0 || position.secondsPerBeat <= 0.0) {
        appendLog(QStringLiteral(
            "synced recording group is ready but the shared grid is not available"));
        return;
    }
    const quint64 countdownBeat = jam2::gui::synced_recording_countdown_beat(
        position, beatsPerBar);
    if (countdownBeat == 0) {
        appendLog(QStringLiteral(
            "synced recording group count-in target exceeds the engine clock range"));
        return;
    }
    const quint64 countInBeats = static_cast<quint64>(qMax(0, countInBars)) *
        static_cast<quint64>(beatsPerBar);
    if (countdownBeat > (std::numeric_limits<quint64>::max)() - countInBeats) {
        appendLog(QStringLiteral(
            "synced recording group target exceeds the engine clock range"));
        return;
    }
    const quint64 targetBeat = countdownBeat + countInBeats;
    const QString groupId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QJsonArray participantArray;
    for (const QString& token : participants) participantArray.append(token);
    const QJsonObject start{
        {QStringLiteral("type"),
            QStringLiteral("looper.recording.group.start")},
        {QStringLiteral("group_id"), groupId},
        {QStringLiteral("participants"), participantArray},
        {QStringLiteral("count_in_bars"), countInBars},
        {QStringLiteral("countdown_abs_beat"), QString::number(countdownBeat)},
        {QStringLiteral("target_abs_beat"), QString::number(targetBeat)},
    };
    activeRecordingGroupId_ = groupId;
    activeRecordingGroupParticipants_ = participants;
    activeRecordingGroupStartMessage_ = start;
    lastRecordingGroupFinishMessage_ = {};
    recordingGroupFinishSent_ = false;
    localLaneTakeForcedLocal_ = false;
    sendControl(start);
    handleRecordingGroupStart(start);
}

void MainWindow::handleRecordingGroupStart(const QJsonObject& message)
{
    const QString groupId = message.value(QStringLiteral("group_id")).toString();
    if (groupId.isEmpty()) return;
    if (recoveredRecordingGroupIds_.contains(groupId)) return;
    if (!activeRecordingGroupId_.isEmpty() && activeRecordingGroupId_ != groupId) {
        appendLog(QStringLiteral(
            "ignored overlapping synced recording group %1")
            .arg(groupId.left(8)));
        return;
    }
    QStringList participantsForGroup;
    const QJsonArray participants = message.value(
        QStringLiteral("participants")).toArray();
    for (const QJsonValue& value : participants) {
        participantsForGroup.append(value.toString());
    }
    const QString localToken = sessionController_.snapshot().localToken;
    if (activeRecordingGroupId_ == groupId &&
        (localTrackRecordingPhase_ == QStringLiteral("recording") ||
         localTrackRecordingPhase_ == QStringLiteral("complete") ||
         localTrackRecordingPhase_ == QStringLiteral("finalizing"))) {
        activeRecordingGroupParticipants_ = participantsForGroup;
        activeRecordingGroupStartMessage_ = message;
        return;
    }
    recordingGroupFinishSent_ = false;
    bool countdownOk = false;
    bool targetOk = false;
    const quint64 countdownBeat = message.value(
        QStringLiteral("countdown_abs_beat")).toString().toULongLong(&countdownOk);
    const quint64 targetBeat = message.value(
        QStringLiteral("target_abs_beat")).toString().toULongLong(&targetOk);
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    const std::uint64_t beatFrames = position.sampleRate > 0 &&
            position.secondsPerBeat > 0.0
        ? static_cast<std::uint64_t>(std::llround(
            position.secondsPerBeat * static_cast<double>(position.sampleRate)))
        : 0;
    if (!countdownOk || !targetOk || beatFrames == 0 ||
        countdownBeat > ((std::numeric_limits<std::uint64_t>::max)() -
            position.epochFrame) / beatFrames ||
        targetBeat > ((std::numeric_limits<std::uint64_t>::max)() -
            position.epochFrame) / beatFrames) {
        appendLog(QStringLiteral("could not map the synced recording group to the local grid"));
        activeRecordingGroupId_ = groupId;
        activeRecordingGroupParticipants_ = participantsForGroup;
        activeRecordingGroupStartMessage_ = message;
        updateLaneRecordingIsolation();
        if (participantsForGroup.contains(localToken) &&
            localTrackRecordingPhase_ == QStringLiteral("ready")) {
            publishLocalTrackRecordingState(QStringLiteral("complete"));
        } else if (!sessionController_.isServer()) {
            activeRecordingGroupId_.clear();
            activeRecordingGroupParticipants_.clear();
            activeRecordingGroupStartMessage_ = {};
            updateLaneRecordingIsolation();
        }
        return;
    }
    const std::uint64_t countdownMusical = position.epochFrame +
        countdownBeat * beatFrames;
    const std::uint64_t targetMusical = position.epochFrame + targetBeat * beatFrames;
    const std::uint64_t countdownFrame = rawFrameFromMusicalFrame(
        countdownMusical, position.renderOffsetFrames);
    const std::uint64_t targetFrame = rawFrameFromMusicalFrame(
        targetMusical, position.renderOffsetFrames);

    activeRecordingGroupId_ = groupId;
    activeRecordingGroupParticipants_ = participantsForGroup;
    activeRecordingGroupStartMessage_ = message;
    recordingGroupFinishSent_ = false;
    updateLaneRecordingIsolation();

    if (activeRecordingGroupParticipants_.contains(localToken) &&
        localTrackRecordingPhase_ == QStringLiteral("ready")) {
        startInputCaptureAtGroupSchedule(
            countdownFrame,
            targetFrame,
            targetMusical,
            message.value(QStringLiteral("count_in_bars")).toInt());
    } else if (!sessionController_.isServer()) {
        if (!trackRecordingWorkflow_.scheduleRecordingTransport(
                countdownFrame, targetFrame, targetMusical, true)) {
            appendLog(QStringLiteral(
                "could not adopt the synced recording group transport"));
        }
    }
    if (sessionController_.isServer()) {
        if (!trackRecordingWorkflow_.scheduleRecordingTransport(
                countdownFrame, targetFrame, targetMusical)) {
            appendLog(QStringLiteral(
                "could not publish the synced recording group transport"));
        }
    }
    updateSharedRecordingPresentation();
}

void MainWindow::maybeFinishRecordingGroup()
{
    if (!sessionController_.isServer() || activeRecordingGroupId_.isEmpty() ||
        recordingGroupFinishSent_ || activeRecordingGroupParticipants_.isEmpty()) {
        return;
    }
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    QSet<QString> connected;
    for (const SharedSessionController::PeerSnapshot& peer : session.peers) {
        connected.insert(peer.token);
    }
    for (const QString& token : std::as_const(activeRecordingGroupParticipants_)) {
        if (token == session.localToken) {
            if (localTrackRecordingPhase_ != QStringLiteral("complete")) return;
            continue;
        }
        if (!connected.contains(token)) continue;
        const auto state = peerTrackRecordingStates_.constFind(token);
        if (state == peerTrackRecordingStates_.cend() ||
            state->groupId != activeRecordingGroupId_ ||
            state->phase != QStringLiteral("complete")) {
            return;
        }
    }
    recordingGroupFinishSent_ = true;
    const QJsonObject finish{
        {QStringLiteral("type"),
            QStringLiteral("looper.recording.group.finish")},
        {QStringLiteral("group_id"), activeRecordingGroupId_},
    };
    lastRecordingGroupFinishMessage_ = finish;
    sendControl(finish);
    handleRecordingGroupFinish(finish);
}

void MainWindow::handleRecordingGroupFinish(const QJsonObject& message)
{
    const QString groupId = message.value(QStringLiteral("group_id")).toString();
    if (groupId.isEmpty() || groupId != activeRecordingGroupId_) return;
    const bool participant = activeRecordingGroupParticipants_.contains(
        sessionController_.snapshot().localToken);
    if (participant && pendingGroupTakeCompletion_.has_value()) {
        publishLocalTrackRecordingState(QStringLiteral("finalizing"));
        finalizePendingLaneTake();
        return;
    }
    if (participant) {
        finishLaneTakeFinalization();
        return;
    }
    activeRecordingGroupId_.clear();
    activeRecordingGroupParticipants_.clear();
    activeRecordingGroupStartMessage_ = {};
    recordingGroupFinishSent_ = false;
    updateLaneRecordingIsolation();
    updateSharedRecordingPresentation();
    releaseDeferredRecordingControls();
}

void MainWindow::requestRecordingGroupRecovery()
{
    if (activeRecordingGroupId_.isEmpty() || !syncedRecordingsEnabled()) return;
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this,
        QStringLiteral("Continue Take Locally"),
        QStringLiteral(
            "Release the shared recording lock for everyone? Any recording that is still running will continue locally and will not be shared automatically."),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    const QJsonObject recovery{
        {QStringLiteral("type"), sessionController_.isServer()
            ? QStringLiteral("looper.recording.group.recover")
            : QStringLiteral("looper.recording.group.recover.request")},
        {QStringLiteral("group_id"), activeRecordingGroupId_},
    };
    sendControl(recovery);
    if (sessionController_.isServer()) handleRecordingGroupRecovery(recovery);
}

void MainWindow::handleRecordingGroupRecovery(const QJsonObject& message)
{
    const QString groupId = message.value(QStringLiteral("group_id")).toString();
    if (groupId.isEmpty() || groupId != activeRecordingGroupId_) return;
    const bool localParticipant = activeRecordingGroupParticipants_.contains(
        sessionController_.snapshot().localToken);
    if (localParticipant &&
        (trackRecordingWorkflow_.laneArmed() ||
         trackRecordingWorkflow_.inputTakeActive() ||
         pendingGroupTakeCompletion_.has_value())) {
        localLaneTakeForcedLocal_ = true;
    } else if (!localParticipant && jam2_.isRunning()) {
        jam2::EngineCommand cancelTransport;
        cancelTransport.type = jam2::EngineCommandType::CancelTransport;
        (void)submitEngineCommand(
            cancelTransport,
            QStringLiteral("cancel stalled shared recording schedule"));
    }
    activeRecordingGroupId_.clear();
    activeRecordingGroupParticipants_.clear();
    activeRecordingGroupStartMessage_ = {};
    lastRecordingGroupFinishMessage_ = message;
    recoveredRecordingGroupIds_.insert(groupId);
    while (recoveredRecordingGroupIds_.size() > 16) {
        recoveredRecordingGroupIds_.erase(recoveredRecordingGroupIds_.begin());
    }
    recordingGroupFinishSent_ = false;
    peerTrackRecordingStates_.clear();
    peerTrackRecordingRevisions_.clear();
    appendLog(QStringLiteral(
        "released stalled synced recording group %1; active local takes continue locally")
        .arg(groupId.left(8)));
    if (localParticipant && pendingGroupTakeCompletion_.has_value()) {
        publishLocalTrackRecordingState(QStringLiteral("finalizing"));
        finalizePendingLaneTake();
        return;
    }
    updateLaneRecordingIsolation();
    updateSharedRecordingPresentation();
    releaseDeferredRecordingControls();
}

void MainWindow::finalizePendingLaneTake()
{
    if (!pendingGroupTakeCompletion_.has_value()) return;
    const TrackRecordingWorkflow::TrackTakeCompletion completion =
        *pendingGroupTakeCompletion_;
    pendingGroupTakeCompletion_.reset();
    if (!completion.ok || completion.wavPath.isEmpty()) {
        ++recordedLaneImportFailures_;
        recordedLaneImportStatus_ = QStringLiteral("take-error");
        appendLog(QStringLiteral("synced lane take could not be finalised: %1")
            .arg(completion.error));
        finishLaneTakeFinalization();
        return;
    }
    registerTransientTrackWav(completion.wavPath);
    recordedLaneImportStatus_ = QStringLiteral("staging-requested");
    importLastCaptureToArmedLane();
}

void MainWindow::finishLaneTakeFinalization()
{
    loopbackCountdownStartFrame_ = 0;
    loopbackRecordingStartFrame_ = 0;
    stopMetronomeAtLoopbackStart_ = false;
    trackRecordingWorkflow_.disarmLane();
    pendingGroupTakeCompletion_.reset();
    activeRecordingGroupId_.clear();
    activeRecordingGroupParticipants_.clear();
    activeRecordingGroupStartMessage_ = {};
    recordingGroupFinishSent_ = false;
    localLaneTakeForcedLocal_ = false;
    localTrackRecordingCountInBars_ = 0;
    localRecordingTargetBankId_.clear();
    localRecordingTargetLaneId_.clear();
    recordedLaneImportRetryAttempts_ = 0;
    publishLocalTrackRecordingState(QStringLiteral("idle"));
    refreshLooperLanes();
    releaseDeferredRecordingControls();
}

QString MainWindow::recordingPeerLabel(const QString& token) const
{
    const SharedSessionController::Snapshot snapshot = sessionController_.snapshot();
    for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
        if (peer.token != token) continue;
        const int ordinal = peerOrdinals_.value(peer.peerId, 0);
        return ordinal > 0
            ? QStringLiteral("Peer %1").arg(ordinal)
            : QStringLiteral("Peer %1").arg(peer.peerId);
    }
    return QStringLiteral("Peer %1").arg(token.left(8));
}

bool MainWindow::sharedRecordingProtected() const
{
    const auto protects = [](const QString& phase) {
        return phase == QStringLiteral("waiting") ||
            phase == QStringLiteral("count-in") ||
            phase == QStringLiteral("recording") ||
            phase == QStringLiteral("complete") ||
            phase == QStringLiteral("finalizing");
    };
    if (!activeRecordingGroupId_.isEmpty()) return true;
    if (protects(localTrackRecordingPhase_)) {
        return true;
    }
    for (const PeerTrackRecordingState& state : peerTrackRecordingStates_) {
        if (protects(state.phase)) return true;
    }
    return false;
}

void MainWindow::updateSharedRecordingPresentation()
{
    const bool sharedWorkflow = syncedRecordingsEnabled() &&
        !localLaneTakeForcedLocal_;
    const bool localVisible = localTrackRecordingPhase_ != QStringLiteral("idle") &&
        trackRecordingWorkflow_.laneArmed();
    const bool visible = localVisible || !peerTrackRecordingStates_.isEmpty();
    const bool protectedState = sharedRecordingProtected();
    const bool localActive = trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning() || loopbackRecordingStartFrame_ > 0;

    if (recordingContextFrame_) recordingContextFrame_->setVisible(visible);
    if (recordingCountdownLabel_) recordingCountdownLabel_->setVisible(visible);
    if (recoverRecordingGroupButton_) {
        recoverRecordingGroupButton_->setVisible(
            visible && !activeRecordingGroupId_.isEmpty() && syncedRecordingsEnabled());
    }
    QStringList entries;
    QString strongestPhase = localVisible ? localTrackRecordingPhase_ : QStringLiteral("idle");
    int strongestCount = localVisible ? localTrackRecordingCountInRemaining_ : 0;
    auto phaseRank = [](const QString& phase) {
        if (phase == QStringLiteral("finalizing")) return 6;
        if (phase == QStringLiteral("complete")) return 5;
        if (phase == QStringLiteral("recording")) return 4;
        if (phase == QStringLiteral("count-in")) return 3;
        if (phase == QStringLiteral("waiting")) return 2;
        if (phase == QStringLiteral("ready")) return 1;
        if (phase == QStringLiteral("armed")) return 1;
        return 0;
    };
    if (localVisible) {
        const int bank = trackRecordingWorkflow_.armedBank();
        const int lane = trackRecordingWorkflow_.armedLane();
        const QString laneName = bank >= 0 && bank < looperProject_.banks().size() &&
                lane >= 0 && lane < looperProject_.banks().at(bank).lanes.size()
            ? looperProject_.banks().at(bank).lanes.at(lane).name
            : QStringLiteral("Track");
        entries.append(QStringLiteral("You · %1Section %2 · %3 · %4")
            .arg(sharedWorkflow ? QString{} : QStringLiteral("LOCAL · "))
            .arg(QChar(QLatin1Char('A').unicode() + qBound(
                0, bank, static_cast<int>(looperProject_.banks().size()) - 1)))
            .arg(laneName.toHtmlEscaped(), localTrackRecordingPhase_.toUpper()));
    }
    for (auto it = peerTrackRecordingStates_.cbegin();
         it != peerTrackRecordingStates_.cend(); ++it) {
        const PeerTrackRecordingState& state = it.value();
        entries.append(QStringLiteral("%1 · Section %2 · %3 · %4")
            .arg(recordingPeerLabel(it.key()).toHtmlEscaped())
            .arg(QChar(QLatin1Char('A').unicode() + qBound(
                0, state.bank, static_cast<int>(looperProject_.banks().size()) - 1)))
            .arg(state.laneName.toHtmlEscaped(), state.phase.toUpper()));
        if (phaseRank(state.phase) > phaseRank(strongestPhase)) {
            strongestPhase = state.phase;
            strongestCount = state.countInRemaining;
        }
    }
    if (recordingPeerStatesLabel_) {
        recordingPeerStatesLabel_->setText(entries.join(QStringLiteral("   |   ")));
    }
    if (recordingContextTitle_) {
        if (!sharedWorkflow && localVisible && strongestPhase == QStringLiteral("recording")) {
            recordingContextTitle_->setText(QStringLiteral("Local recording active"));
        } else if (!sharedWorkflow && localVisible) {
            recordingContextTitle_->setText(QStringLiteral("Local track armed"));
        } else if (strongestPhase == QStringLiteral("recording")) {
            recordingContextTitle_->setText(QStringLiteral("Recording active"));
        } else if (strongestPhase == QStringLiteral("count-in")) {
            recordingContextTitle_->setText(strongestCount > 0
                ? QStringLiteral("Count-in · %1 beat%2 remaining")
                    .arg(strongestCount)
                    .arg(strongestCount == 1 ? QString{} : QStringLiteral("s"))
                : QStringLiteral("Count-in active"));
        } else if (strongestPhase == QStringLiteral("waiting")) {
            recordingContextTitle_->setText(QStringLiteral("Waiting for the recording boundary"));
        } else if (strongestPhase == QStringLiteral("ready")) {
            recordingContextTitle_->setText(QStringLiteral(
                "Ready · waiting for every armed participant"));
        } else if (strongestPhase == QStringLiteral("complete")) {
            recordingContextTitle_->setText(QStringLiteral(
                "Take complete · waiting for the recording group"));
        } else if (strongestPhase == QStringLiteral("finalizing")) {
            recordingContextTitle_->setText(QStringLiteral("Finalizing recorded takes"));
        } else {
            recordingContextTitle_->setText(entries.size() > 1
                ? QStringLiteral("%1 tracks armed").arg(entries.size())
                : QStringLiteral("Track armed"));
        }
    }
    if (recordingContextDetail_) {
        recordingContextDetail_->setText(!sharedWorkflow && localVisible
            ? QStringLiteral("Local only · other peers can continue playback or record independently.")
            : protectedState
            ? QStringLiteral("Shared playback and Track edits are protected until the take stops.")
            : QStringLiteral("Starting recording restarts playback for participating peers."));
    }
    if (recordingCountdownLabel_ && visible) {
        if (strongestPhase == QStringLiteral("recording")) {
            recordingCountdownLabel_->setText(
                QStringLiteral("ARMED  ✓  WAITING FOR BEAT  ✓  COUNT-IN  ✓  RECORDING  ●"));
        } else if (strongestPhase == QStringLiteral("count-in")) {
            recordingCountdownLabel_->setText(QStringLiteral(
                "ARMED  ✓  WAITING FOR BEAT  ✓  COUNT-IN  %1  ●  RECORDING")
                .arg(qMax(1, strongestCount)));
        } else if (strongestPhase == QStringLiteral("waiting")) {
            recordingCountdownLabel_->setText(
                QStringLiteral("ARMED  ✓  WAITING FOR BEAT  ●  COUNT-IN  ›  RECORDING"));
        } else {
            recordingCountdownLabel_->setText(
                QStringLiteral("ARMED  ●  WAITING FOR BEAT  ›  COUNT-IN  ›  RECORDING"));
        }
    }
    if (recordingGlobalControls_) {
        recordingGlobalControls_->setVisible(localVisible);
        recordingGlobalControls_->setEnabled(
            !protectedState && localTrackRecordingPhase_ != QStringLiteral("ready"));
    }
    if (startArmedLaneRecordingButton_) {
        startArmedLaneRecordingButton_->setVisible(localVisible);
        const bool ready = localTrackRecordingPhase_ == QStringLiteral("ready");
        const bool completed = localTrackRecordingPhase_ == QStringLiteral("complete") ||
            localTrackRecordingPhase_ == QStringLiteral("finalizing");
        startArmedLaneRecordingButton_->setText(localActive
            ? QStringLiteral("Stop My Recording")
            : ready ? QStringLiteral("Ready ✓ · Cancel")
            : completed ? QStringLiteral("Take Complete")
            : protectedState ? QStringLiteral("Recording Locked")
            : syncedRecordingsEnabled()
                ? QStringLiteral("Ready to Record")
                : QStringLiteral("Start Recording"));
        startArmedLaneRecordingButton_->setEnabled(
            localActive || ready || (!protectedState && !completed));
    }
    if (stopCaptureButton_) stopCaptureButton_->setVisible(false);

    const QList<QWidget*> protectedWidgets{
        playTrackButton_, stopTrackButton_, loopStartButton_, loopEndButton_,
        clearLoopButton_, loopEnabledCheck_, trackSpeedSlider_,
        trackSpeedSpin_, trackPitchSlider_, trackPitchSpin_, focusFrequencyCheck_,
        focusPresetBox_, loadWavButton_, arrangementButton_, songTitleEdit_,
        metronomeBpmSpin_, metronomeBeatsSpin_, metronomeBeatUnitBox_,
        metronomeTempoPulseBox_, metronomeDivisionBox_,
        metronomePatternWidget_, tapTrackMetronomeButton_,
    };
    for (QWidget* widget : protectedWidgets) {
        if (widget) widget->setEnabled(!protectedState);
    }
    if (chordGrid_) chordGrid_->setEditingProtected(protectedState);
    if (beatGrid_) beatGrid_->setEditingProtected(protectedState);
    if (lyricGrid_) lyricGrid_->setEditingProtected(protectedState);
    for (QPushButton* button : looperBankButtons_) {
        if (button) button->setEnabled(!protectedState);
    }
    if (launchBankButton_) {
        launchBankButton_->setEnabled(
            !protectedState &&
            viewedBankIndex_ != looperProject_.activeBankIndex());
    }
    if (shareTracksButton_) {
        const bool automaticSharing = automaticWavSharingEnabled();
        shareTracksButton_->setEnabled(!protectedState && !automaticSharing);
        shareTracksButton_->setToolTip(automaticSharing
            ? QStringLiteral("WAVs are already shared automatically with the jam")
            : QStringLiteral("Manually share the current tracks with the jam"));
    }
    const bool customFocus = isCustomFocusPreset(trackController_.model().focusPreset);
    if (focusFrequencySlider_) {
        focusFrequencySlider_->setEnabled(!protectedState && customFocus);
    }
    if (focusFrequencySpin_) {
        focusFrequencySpin_->setEnabled(!protectedState && customFocus);
    }
    if (looperStack_) looperStack_->setInteractionsProtected(protectedState);
    if (performanceTrackToggle_) {
        performanceTrackToggle_->setEnabled(!protectedState &&
            trackController_.model().sampleRateCompatible && jam2_.isRunning());
        performanceTrackToggle_->setToolTip(protectedState
            ? QStringLiteral("Playback is protected because a synced track recording is active")
            : QString{});
    }
    updateJamSyncPresentation();
}

void MainWindow::importLastCaptureToArmedLane()
{
    if (!trackRecordingWorkflow_.laneArmed()) {
        recordedLaneImportRetryAttempts_ = 0;
        ++recordedLaneImportFailures_;
        recordedLaneImportStatus_ = QStringLiteral("lane-not-armed");
        finishLaneTakeFinalization();
        return;
    }
    int bankIndex = -1;
    for (int index = 0; index < looperProject_.banks().size(); ++index) {
        if (looperProject_.banks().at(index).id == localRecordingTargetBankId_) {
            bankIndex = index;
            break;
        }
    }
    if (bankIndex < 0) {
        bankIndex = qBound(0, trackRecordingWorkflow_.armedBank(),
            looperProject_.banks().size() - 1);
    }
    const QString laneId = localRecordingTargetLaneId_;
    const QString sourcePath = trackRecordingWorkflow_.lastCapturePath();
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = activeTrackSampleRate();
    const bool keepTakeLocal = localLaneTakeForcedLocal_;
    auto result = std::make_shared<StagedPcm16Asset>();
    const bool started = startFileWorkerTask(
        [sourcePath, stagingFolder, expectedSampleRate, result] {
            *result = stagePcm16Asset(
                sourcePath,
                stagingFolder,
                expectedSampleRate,
                QStringLiteral("recorded"));
        },
        [this, bankIndex, laneId, result, keepTakeLocal] {
            if (!result->error.isEmpty()) {
                ++recordedLaneImportFailures_;
                recordedLaneImportStatus_ = QStringLiteral("staging-error");
                appendLog(QStringLiteral("recorded lane WAV not importable: ") + result->error);
                finishLaneTakeFinalization();
                return;
            }
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
                ++recordedLaneImportFailures_;
                recordedLaneImportStatus_ = QStringLiteral("target-bank-missing");
                finishLaneTakeFinalization();
                return;
            }
            if (result->resampled) {
                appendLog(QStringLiteral(
                    "recorded lane WAV resampled: source_rate=%1 target_rate=%2 source_frames=%3 output_frames=%4")
                    .arg(result->sourceSampleRate)
                    .arg(result->metadata.sampleRate)
                    .arg(result->sourceFrames)
                    .arg(result->metadata.frames));
            }
            int laneIndex = -1;
            const auto& lanes = looperProject_.banks().at(bankIndex).lanes;
            for (int index = 0; index < lanes.size(); ++index) {
                if (lanes.at(index).id == laneId) {
                    laneIndex = index;
                    break;
                }
            }
            LooperLane candidate;
            QString replacedPath;
            QString replacedHash;
            if (laneIndex < 0) {
                candidate.id = QUuid::createUuid()
                    .toString(QUuid::WithoutBraces).toLower();
                candidate.name = QStringLiteral("Recovered recorded take");
            } else {
                candidate = looperProject_.banks().at(bankIndex).lanes.at(laneIndex);
                replacedPath = looperAssetAbsolutePath(candidate);
                replacedHash = candidate.assetHash;
            }
            candidate.assetPath = result->stagedPath;
            candidate.assetHash = result->sha256;
            candidate.sampleRate = result->metadata.sampleRate;
            candidate.sampleRateCompatible = true;
            candidate.sourceFrames = result->metadata.frames;
            candidate.originKind = QStringLiteral("recorded");
            candidate.localOnly = keepTakeLocal;
            candidate.referenceKind.clear();
            candidate.referenceSourceSignature.clear();
            candidate.referenceBpm = 0.0;
            candidate.referenceStale = false;
            if (isDefaultEmptyTrackName(candidate.name) ||
                candidate.name.trimmed().isEmpty()) {
                candidate.name = result->displayName;
            }
            candidate.startFrame = 0;
            candidate.stopFrame = -1;
            candidate.loopStartFrame = -1;
            candidate.loopEndFrame = -1;
            candidate.loopEnabled = false;
            const bool recovered = laneIndex < 0;
            const bool attached = recovered
                ? looperProject_.appendLane(bankIndex, std::move(candidate))
                : looperProject_.replaceLane(
                    bankIndex, laneIndex, std::move(candidate));
            if (!attached) {
                ++recordedLaneImportFailures_;
                recordedLaneImportStatus_ = recovered
                    ? QStringLiteral("recovery-lane-failed")
                    : QStringLiteral("attachment-invalid");
                appendLog(recovered
                    ? QStringLiteral(
                        "recorded lane target was removed and a recovery lane could not be created")
                    : QStringLiteral(
                        "recorded lane WAV failed checked attachment; prior lane state was preserved"));
                finishLaneTakeFinalization();
                return;
            }
            if (recovered) {
                laneIndex = looperProject_.banks().at(bankIndex).lanes.size() - 1;
                appendLog(QStringLiteral(
                    "recorded lane target changed; preserved the WAV in a recovery lane"));
            }
            registerTransientTrackWav(result->stagedPath);
            looperWaveformCache_.remove(result->stagedPath);
            const LooperLane& lane =
                looperProject_.banks().at(bankIndex).lanes.at(laneIndex);
            recordedLaneImportStatus_ = QStringLiteral("attached");
            recordedLaneImportTargetId_ = lane.id;
            recordedLaneImportLastHash_ = lane.assetHash;
            if (replacedHash != result->sha256) {
                cancelUnreferencedLooperAssetTransfer(replacedHash);
            }
            if (!replacedPath.isEmpty() &&
                QDir::cleanPath(replacedPath) !=
                    QDir::cleanPath(result->stagedPath)) {
                discardObsoleteReferenceWavs(QSet<QString>{replacedPath});
            }
            if (!keepTakeLocal && jamSyncPolicy_.trackLanes && jam2_.isNetworkRunning()) {
                pendingRecordedLaneSyncId_ = lane.id;
                pendingRecordedLaneSyncHash_ = lane.assetHash;
                pendingRecordedLaneSyncAttempts_ = 0;
            }
            appendLog(QStringLiteral("recorded lane imported: %1").arg(lane.name));
            refreshLooperLanes();
            regeneratePreparedMix();
            if (!keepTakeLocal) syncLooperArrangement();
            if (!keepTakeLocal && sessionController_.isServer()) {
                pendingRecordedLaneSyncId_.clear();
                pendingRecordedLaneSyncHash_.clear();
                pendingRecordedLaneSyncAttempts_ = 0;
            }
            if (!keepTakeLocal && automaticWavSharingEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
            finishLaneTakeFinalization();
        },
        [this](const QString& error) {
            ++recordedLaneImportFailures_;
            recordedLaneImportStatus_ = QStringLiteral("worker-error");
            appendLog(QStringLiteral("recorded lane import failed: ") + error);
            finishLaneTakeFinalization();
        });
    if (!started) {
        constexpr int kMaximumRecordedLaneImportRetries = 200;
        constexpr int kRecordedLaneImportRetryDelayMs = 50;
        ++recordedLaneImportRetryAttempts_;
        ++recordedLaneImportBusyRetries_;
        if (recordedLaneImportRetryAttempts_ > kMaximumRecordedLaneImportRetries) {
            ++recordedLaneImportFailures_;
            recordedLaneImportStatus_ = QStringLiteral("worker-timeout");
            appendLog(QStringLiteral(
                "recorded lane import could not acquire a file worker after %1 retries; the captured WAV remains in managed storage")
                .arg(kMaximumRecordedLaneImportRetries));
            finishLaneTakeFinalization();
            return;
        }
        if (recordedLaneImportRetryAttempts_ == 1 ||
            (recordedLaneImportRetryAttempts_ % 20) == 0) {
            appendLog(QStringLiteral(
                "recorded lane import waiting for a file worker: retry=%1 active=%2")
                .arg(recordedLaneImportRetryAttempts_)
                .arg(fileWorkerTasksActive_));
        }
        recordedLaneImportStatus_ = QStringLiteral("waiting-worker");
        QTimer::singleShot(kRecordedLaneImportRetryDelayMs, this, [this] {
            if (!shuttingDown_) importLastCaptureToArmedLane();
        });
        return;
    }
    recordedLaneImportRetryAttempts_ = 0;
    recordedLaneImportStatus_ = QStringLiteral("staging");
}

void MainWindow::renameSelectedLooperLane()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int row = selectedLooperLane_;
    const LooperLane& lane = looperProject_.banks().at(viewedBankIndex_).lanes.at(row);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Rename lane"));
    auto* form = new QFormLayout(&dialog);
    auto* nameEdit = new QLineEdit(lane.name, &dialog);
    nameEdit->setMaxLength(
        jam2::application::limits::kMaximumLooperNameCharacters);
    form->addRow(QStringLiteral("Lane name"), nameEdit);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QPushButton* saveButton = buttons->button(QDialogButtonBox::Ok);
    QObject::connect(
        nameEdit,
        &QLineEdit::textChanged,
        saveButton,
        [saveButton](const QString& name) {
            saveButton->setEnabled(!name.trimmed().isEmpty());
        });
    saveButton->setEnabled(!nameEdit->text().trimmed().isEmpty());
    jam2::gui::registerGuiControl(
        *nameEdit,
        QStringLiteral("looper.lane-rename-dialog.name"),
        QStringLiteral("looper.lane-rename"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.lane-rename-dialog"));
    jam2::gui::registerGuiControl(
        *saveButton,
        QStringLiteral("looper.lane-rename-dialog.save"),
        QStringLiteral("looper.lane-rename"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.lane-rename-dialog"));
    jam2::gui::registerGuiControl(
        *buttons->button(QDialogButtonBox::Cancel),
        QStringLiteral("looper.lane-rename-dialog.cancel"),
        QStringLiteral("looper.lane-rename"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.lane-rename-dialog"));
    if (dialog.exec() == QDialog::Accepted &&
        looperProject_.renameLane(viewedBankIndex_, row, nameEdit->text())) {
        refreshLooperLanes();
        regeneratePreparedMix();
        syncLooperArrangement();
    }
}

void MainWindow::removeSelectedLooperLane()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int bankIndex = viewedBankIndex_;
    const int row = selectedLooperLane_;
    const LooperLane lane = looperProject_.banks().at(bankIndex).lanes.at(row);
    const QString assetPath = looperAssetAbsolutePath(lane);
    const QFileInfo assetInfo(assetPath);
    int references = 0;
    for (const LooperBank& bank : looperProject_.banks()) {
        for (const LooperLane& candidate : bank.lanes) {
            if ((!lane.assetHash.isEmpty() && candidate.assetHash == lane.assetHash) ||
                (!assetPath.isEmpty() && QDir::cleanPath(looperAssetAbsolutePath(candidate)) == QDir::cleanPath(assetPath))) {
                ++references;
            }
        }
    }
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Remove lane"),
        QStringLiteral("Remove '%1' from this section?").arg(lane.name), QMessageBox::Cancel, this);
    QPushButton* removeOnly = dialog.addButton(QStringLiteral("Remove lane only"), QMessageBox::AcceptRole);
    QPushButton* deleteAsset = dialog.addButton(QStringLiteral("Delete WAV from disk"), QMessageBox::DestructiveRole);
    jam2::gui::registerGuiControl(
        *removeOnly,
        QStringLiteral("looper.lane-remove-dialog.remove-only"),
        QStringLiteral("looper.lane-remove"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.lane-remove-dialog"));
    jam2::gui::registerGuiControl(
        *deleteAsset,
        QStringLiteral("looper.lane-remove-dialog.delete-wav"),
        QStringLiteral("looper.lane-remove-delete-wav"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.lane-remove-dialog"));
    if (QAbstractButton* cancel = dialog.button(QMessageBox::Cancel)) {
        jam2::gui::registerGuiControl(
            *cancel,
            QStringLiteral("looper.lane-remove-dialog.cancel"),
            QStringLiteral("looper.lane-remove-cancel"),
            jam2::gui::GuiControlAvailability::Modal,
            QStringLiteral("looper.lane-remove-dialog"));
    }
    const bool hasWav = assetInfo.exists() && assetInfo.isFile() &&
        assetInfo.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0;
    if (!hasWav) {
        deleteAsset->setEnabled(false);
        dialog.setInformativeText(QStringLiteral("The lane does not reference an existing WAV file."));
    } else if (references > 1) {

        dialog.setInformativeText(QStringLiteral(
            "This WAV is referenced by %1 lanes. Deleting it from disk will leave the other references without audio.")
            .arg(references));
    }
    dialog.exec();
    if (dialog.clickedButton() != removeOnly && dialog.clickedButton() != deleteAsset) {
        return;
    }
    if (dialog.clickedButton() == deleteAsset) {
        const bool deleted = hasWav && (
            projectPersistence_.ownsTransientWav(assetPath)
                ? projectPersistence_.discardTransientWav(assetPath)
                : QFile::remove(assetPath));
        if (!deleted) {
            QMessageBox::warning(this, QStringLiteral("Remove lane"), QStringLiteral("Could not delete the WAV file."));
            return;
        }
        validatedTrackAssetHashes_.remove(lane.assetHash);
        projectPersistence_.pruneEmptyWorkspaceDirectories();
    }
    const bool armedInBank = trackRecordingWorkflow_.laneArmed() &&
        trackRecordingWorkflow_.armedBank() == bankIndex;
    const int armedLaneIndex = trackRecordingWorkflow_.armedLane();
    const QString armedLaneId = armedInBank &&
            armedLaneIndex >= 0 &&
            armedLaneIndex < looperProject_.banks().at(bankIndex).lanes.size()
        ? looperProject_.banks().at(bankIndex).lanes.at(armedLaneIndex).id
        : QString{};
    const TrackRecordingWorkflow::CaptureMode armedCaptureMode =
        trackRecordingWorkflow_.captureMode();
    const bool armedIncludePrepared =
        trackRecordingWorkflow_.includePreparedInTake();
    const bool armedIncludeMetronome =
        trackRecordingWorkflow_.includeMetronomeInTake();
    looperProject_.removeLane(bankIndex, row);
    looperWaveformCache_.remove(assetPath);
    if (armedInBank && row == armedLaneIndex) {
        trackRecordingWorkflow_.disarmLane();
        publishLocalTrackRecordingState(QStringLiteral("idle"));
        appendLog(QStringLiteral("lane recording disarmed because its track was removed"));
    } else if (armedInBank && row < armedLaneIndex) {
        int relocatedLaneIndex = -1;
        const QVector<LooperLane>& remaining =
            looperProject_.banks().at(bankIndex).lanes;
        for (int index = 0; index < remaining.size(); ++index) {
            if (remaining.at(index).id == armedLaneId) {
                relocatedLaneIndex = index;
                break;
            }
        }
        if (relocatedLaneIndex >= 0) {
            trackRecordingWorkflow_.armLane(
                bankIndex,
                relocatedLaneIndex,
                armedCaptureMode,
                armedIncludePrepared,
                armedIncludeMetronome);
        } else {
            trackRecordingWorkflow_.disarmLane();
            publishLocalTrackRecordingState(QStringLiteral("idle"));
            appendLog(QStringLiteral(
                "lane recording disarmed because its target could not be resolved"));
        }
    }
    selectedLooperLane_ = qMin(row, looperProject_.banks().at(bankIndex).lanes.size() - 1);
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::revealLooperLaneWav(int laneIndex)
{
    if (viewedBankIndex_ < 0 || viewedBankIndex_ >= looperProject_.banks().size()) return;
    const LooperBank& bank = looperProject_.banks().at(viewedBankIndex_);
    if (laneIndex < 0 || laneIndex >= bank.lanes.size()) return;
    const QString path = QFileInfo(looperAssetAbsolutePath(bank.lanes.at(laneIndex))).absoluteFilePath();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Show WAV"),
            QStringLiteral("The WAV file could not be found:\n%1")
                .arg(QDir::toNativeSeparators(path)));
        return;
    }

    bool opened = false;
#if defined(_WIN32)
    opened = QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        {QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(path))});
#elif defined(Q_OS_MACOS)
    opened = QProcess::startDetached(
        QStringLiteral("open"),
        {QStringLiteral("-R"), path});
#else
    opened = QDesktopServices::openUrl(
        QUrl::fromLocalFile(info.absolutePath()));
#endif
    if (!opened) {
        QMessageBox::warning(
            this,
            QStringLiteral("Show WAV"),
            QStringLiteral("Could not open the WAV location:\n%1")
                .arg(QDir::toNativeSeparators(path)));
    }
}

void MainWindow::removeLooperLaneWav(int laneIndex)
{
    if (sharedRecordingProtected()) return;
    if (viewedBankIndex_ < 0 || viewedBankIndex_ >= looperProject_.banks().size()) return;
    LooperBank& bank = looperProject_.banks()[viewedBankIndex_];
    if (laneIndex < 0 || laneIndex >= bank.lanes.size()) return;
    LooperLane& lane = bank.lanes[laneIndex];
    if (lane.assetPath.trimmed().isEmpty()) return;

    const QString assetPath = looperAssetAbsolutePath(lane);
    const QString removedHash = lane.assetHash;
    const QString fileName = QFileInfo(assetPath).fileName();
    QMessageBox dialog(
        QMessageBox::Question,
        QStringLiteral("Remove WAV"),
        QStringLiteral("Remove '%1' from '%2'?")
            .arg(fileName.isEmpty() ? QStringLiteral("this WAV") : fileName,
                 lane.name),
        QMessageBox::Cancel,
        this);
    dialog.setInformativeText(QStringLiteral(
        "The track, its name and mixer settings will remain. An original imported file is not deleted."));
    QPushButton* removeButton = dialog.addButton(
        QStringLiteral("Remove WAV"), QMessageBox::DestructiveRole);
    jam2::gui::registerGuiControl(
        *removeButton,
        QStringLiteral("looper.wav-remove-dialog.accept"),
        QStringLiteral("looper.wav-remove-confirmation"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.wav-remove-dialog"));
    jam2::gui::registerGuiControl(
        *dialog.button(QMessageBox::Cancel),
        QStringLiteral("looper.wav-remove-dialog.cancel"),
        QStringLiteral("looper.wav-remove-confirmation"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("looper.wav-remove-dialog"));
    dialog.exec();
    if (dialog.clickedButton() != removeButton) return;

    if (!looperProject_.clearLaneAsset(viewedBankIndex_, laneIndex)) return;
    cancelUnreferencedLooperAssetTransfer(removedHash);

    selectedLooperLane_ = laneIndex;
    discardObsoleteReferenceWavs(QSet<QString>{assetPath});
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::cancelUnreferencedLooperAssetTransfer(const QString& hash)
{
    if (hash.isEmpty()) return;
    for (const LooperBank& projectBank : looperProject_.banks()) {
        for (const LooperLane& projectLane : projectBank.lanes) {
            if (projectLane.assetHash == hash) return;
        }
    }
    assetTransfer_.discardOutgoingHash(hash);
    validatedTrackAssetHashes_.remove(hash);
}

void MainWindow::toggleSelectedLooperLaneMute()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) return;
    if (viewedBankIndex_ < 0 || viewedBankIndex_ >= looperProject_.banks().size() ||
        selectedLooperLane_ >= looperProject_.banks().at(viewedBankIndex_).lanes.size()) return;
    const bool muted =
        looperProject_.banks().at(viewedBankIndex_).lanes.at(selectedLooperLane_).muted;
    if (!looperProject_.setLaneMuted(viewedBankIndex_, selectedLooperLane_, !muted)) return;
    refreshLooperLanes();
    regeneratePreparedMix();
}

void MainWindow::toggleSelectedLooperLaneSolo()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) return;
    if (viewedBankIndex_ < 0 || viewedBankIndex_ >= looperProject_.banks().size() ||
        selectedLooperLane_ >= looperProject_.banks().at(viewedBankIndex_).lanes.size()) return;
    const bool solo =
        looperProject_.banks().at(viewedBankIndex_).lanes.at(selectedLooperLane_).solo;
    if (!looperProject_.setLaneSolo(viewedBankIndex_, selectedLooperLane_, !solo)) return;
    refreshLooperLanes();
    regeneratePreparedMix();
}

void MainWindow::syncLooperArrangement()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (jamSyncPolicy_.trackLanes && jam2_.isRunning() &&
        (role == SharedSessionController::Role::Creator ||
         role == SharedSessionController::Role::Joiner)) {
        sendSongSnapshot();
    }
}

QString MainWindow::looperAssetAbsolutePath(const LooperLane& lane) const
{
    if (lane.assetPath.trimmed().isEmpty()) {
        return {};
    }
    if (QFileInfo(lane.assetPath).isAbsolute() || projectPersistence_.projectFolder().isEmpty()) {
        return lane.assetPath;
    }
    return QDir(projectPersistence_.projectFolder()).absoluteFilePath(lane.assetPath);
}

bool MainWindow::materializeLooperAssets(const QString& projectFolder)
{
    auto result = std::make_shared<
        jam2::gui::LooperAssetMaterializationResult>();
    const LooperProject sourceProject = looperProject_;
    const QString sourceProjectFolder = projectPersistence_.projectFolder();
    const QString targetProjectFolder = QDir(projectFolder).absolutePath();
    const QByteArray sourceSnapshot = currentProjectSnapshot();
    QEventLoop waitLoop;
    const bool started = startFileWorkerTask(
        [result, sourceProject, sourceProjectFolder, targetProjectFolder] {
            *result = jam2::gui::materializeLooperAssets(
                sourceProject,
                sourceProjectFolder,
                targetProjectFolder);
        },
        [&waitLoop] { waitLoop.quit(); },
        [&waitLoop, result](const QString& error) {
            result->error = error;
            waitLoop.quit();
        });
    if (!started) {
        QMessageBox::warning(this, QStringLiteral("Save JamJar"), QStringLiteral("The bounded file worker is busy; try Save again."));
        return false;
    }
    QProgressDialog progress(
        QStringLiteral("Verifying JamJar audio assets..."),
        QString{},
        0,
        0,
        this);
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    waitLoop.exec();
    progress.close();
    if (!result->error.isEmpty()) {
        for (const QString& path : std::as_const(result->createdPaths)) {
            registerTransientTrackWav(path);
        }
        QMessageBox::warning(this, QStringLiteral("Save JamJar"), result->error);
        return false;
    }
    if (currentProjectSnapshot() != sourceSnapshot) {
        const QStringList cleanupFailures =
            jam2::gui::rollbackLooperAssetMaterialization(
                result->createdPaths);
        for (const QString& path : cleanupFailures) {
            registerTransientTrackWav(path);
        }
        QString message = QStringLiteral(
            "The project changed while its WAVs were being verified; Save again.");
        if (!cleanupFailures.isEmpty()) {
            message += QStringLiteral(" Cleanup will be retried for: ") +
                cleanupFailures.join(QStringLiteral(", "));
        }
        QMessageBox::warning(this, QStringLiteral("Save JamJar"), message);
        return false;
    }
    looperProject_ = std::move(result->project);
    projectPersistence_.setProjectFolder(targetProjectFolder);
    for (const QString& path : std::as_const(result->createdPaths)) {
        registerTransientTrackWav(path);
    }
    return true;
}

void MainWindow::regeneratePreparedMix(int bankIndex)
{
    const int requestedBank = bankIndex < 0 ? viewedBankIndex_ : bankIndex;
    const int targetBank = qBound(0, requestedBank, looperProject_.banks().size() - 1);
    (void)extendSectionToFitTracks(targetBank);
    const int sharedBank = sharedBankLaunch_.snapshot().bankIndex;
    const jam2::gui::PreparedMixRequestDecision request =
        preparedMixLifecycle_.request(
            targetBank,
            looperProject_.banks().size(),
            PreparedMixRenderer::hasRenderableSources(looperProject_, targetBank),
            sharedBank);
    if (request.status == jam2::gui::PreparedMixRequestStatus::Coalesced) {
        return;
    }
    if (request.status == jam2::gui::PreparedMixRequestStatus::NoSources) {
        if (targetBank == looperProject_.activeBankIndex()) {
            discardPreparedMix(false);
        } else {
            discardObsoletePreparedMixPaths();
        }
        if (targetBank == sharedBank) {
            noteSharedBankReady(targetBank);
        }
        return;
    }
    if (request.status != jam2::gui::PreparedMixRequestStatus::StartWorker) {
        appendLog(QStringLiteral("prepared mix request rejected invalid bank state"));
        return;
    }

    const int sampleRate = activeTrackSampleRate();
    const QString cachePath = PreparedMixRenderer::outputPath(
        projectPersistence_.workspaceFolder(),
        targetBank,
        request.generation,
        QCoreApplication::applicationPid());
    const LooperProject project = looperProject_;
    const QString projectFolder = projectPersistence_.projectFolder();
    const SharedTrackModel track = trackController_.model();
    const qint64 exactFrames = bankExactOutputFrames(targetBank, sampleRate);
    QPointer<MainWindow> self(this);
    fileWorkerPool_.start(QRunnable::create([
        self,
        project,
        projectFolder,
        sampleRate,
        cachePath,
        track,
        targetBank,
        exactFrames,
        generation = request.generation
    ]() mutable {
        PreparedMixResult result;
        try {
            result = PreparedMixRenderer::render(
                project,
                projectFolder,
                sampleRate,
                cachePath,
                track,
                targetBank,
                exactFrames);
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        } catch (...) {
            result.error = QStringLiteral("unknown prepared-mix worker exception");
        }
        if (self.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(self, [
            self,
            result = std::move(result),
            generation
        ]() mutable {
            if (self.isNull()) {
                return;
            }
            self->retryObsoleteReferenceWavs();
            const jam2::gui::PreparedMixCompletionDecision completion =
                self->preparedMixLifecycle_.complete(generation, result.path);
            if (!completion.discardPath.isEmpty()) {
                (void)QFile::remove(completion.discardPath);
            }
            if (completion.status ==
                jam2::gui::PreparedMixCompletionStatus::Rerun) {
                self->regeneratePreparedMix(completion.rerunBankIndex);
                return;
            }
            if (completion.status !=
                jam2::gui::PreparedMixCompletionStatus::Apply) {
                return;
            }
            self->applyPreparedMixResult(std::move(result));
        }, Qt::QueuedConnection);
    }));
}

bool MainWindow::startFileWorkerTask(
    std::function<void()> work,
    std::function<void()> complete,
    std::function<void(const QString&)> failed)
{
    return trackWorkspace_.startFileTask(
        std::move(work),
        std::move(complete),
        std::move(failed));
}

void MainWindow::applyPreparedMixResult(PreparedMixResult result)

{
    for (const QString& warning : std::as_const(result.warnings)) {
        appendLog(QStringLiteral("prepared mix warning: ") + warning);
    }
    if (result.error.isEmpty() &&
        (result.path.trimmed().isEmpty() || !QFileInfo::exists(result.path))) {
        result.error = QStringLiteral(
            "prepared mix output is missing after worker completion");
    }
    const jam2::gui::PreparedMixCacheDecision cachedDecision =
        preparedMixLifecycle_.cacheResult(result, looperProject_.banks().size());
    if (cachedDecision.status != jam2::gui::PreparedMixCacheStatus::Cached) {
        const QString error = !result.error.isEmpty()
            ? result.error
            : QStringLiteral("prepared mix returned invalid metadata or bank identity");
        appendLog(QStringLiteral("prepared mix failed: %1 worker_requests=%2 worker_coalesced=%3 worker_failures=%4")
            .arg(error)
            .arg(preparedMixLifecycle_.requests())
            .arg(preparedMixLifecycle_.coalesced())
            .arg(preparedMixLifecycle_.failures()));
        if (!result.path.isEmpty()) (void)QFile::remove(result.path);
        return;
    }
    const int resultBank = cachedDecision.bankIndex;
    registerTransientTrackWav(result.path);
    const bool preparedForSharedBank =
        sharedBankLaunch_.active() &&
        sharedBankLaunch_.snapshot().bankIndex == resultBank;
    // A freshly loaded arrangement can request its already-active first bank
    // before that bank has a prepared cache.  Once the render completes it is
    // still a real pending launch: route it through the normal launch path so
    // pendingBankIndex_ is cleared.  Leaving it as "NEXT A" prevents the armed
    // arrangement state machine from ever entering its transition branch on
    // the first Play; Stop happened to clear it, which is why the second Play
    // worked.
    if (!preparedForSharedBank && pendingBankIndex_ == resultBank) {
        const std::optional<quint64> targetBeat = pendingBankRequestedTargetBeat_;
        pendingBankRequestedTargetBeat_.reset();
        schedulePreparedBankLaunch(resultBank, targetBeat);
        return;
    }
    if (resultBank != looperProject_.activeBankIndex()) {
        appendLog(QStringLiteral("prepared inactive bank %1: %2 frames in %3 ms")
            .arg(QChar(QLatin1Char('A').unicode() + resultBank))
            .arg(result.frames)
            .arg(result.renderMs));
        if (preparedForSharedBank) noteSharedBankReady(resultBank);
        return;
    }
    if (!preparedMixLifecycle_.activateCachedBank(
            resultBank,
            looperProject_.banks().size(),
            QFileInfo::exists(result.path))) {
        discardPreparedMix(false);
        discardObsoletePreparedMixPaths();
        preparedMixLifecycle_.noteMetadataFailure(
            QStringLiteral("prepared mix output disappeared before activation"));
        appendLog(QStringLiteral("prepared mix metadata failed: %1 worker_failures=%2")
            .arg(preparedMixLifecycle_.active().error)
            .arg(preparedMixLifecycle_.failures()));
        return;
    }
    registerTransientTrackWav(preparedMixLifecycle_.active().path);
    try {
        auto& track = trackController_.model();
        track.fileName = QStringLiteral("Prepared Section %1").arg(looperProject_.banks().at(looperProject_.activeBankIndex()).id);
        track.filePath = preparedMixLifecycle_.active().path;
        track.fileBytes = preparedMixLifecycle_.active().fileBytes;
        track.sampleRate = preparedMixLifecycle_.active().sampleRate;
        track.sampleRateCompatible = true;
        track.userProvidedSource = false;
        track.durationMs = preparedMixLifecycle_.active().durationMs;
        track.sha256 = preparedMixLifecycle_.active().sha256;
        // The performance playhead derives its relative song beat from this
        // value while a prepared WAV is active.  Refreshing the live bank must
        // update it just as adopting a bank cache does; otherwise a WAV
        // regenerated after a BPM change sounds at the new tempo while the
        // chord/beat display continues advancing at the previous tempo until
        // the user switches banks.
        track.acceptedBpm = bankMetronomePattern(resultBank).bpm;
        updateTrackControls();
        loadTrackWaveform();
        const TrackRecordingWorkflow::PreparedAttachPlan attachPlan =
            trackRecordingWorkflow_.preparedAttachPlan(
                metronomeTransport_.grid().position(),
                preparedMixLifecycle_.active().frames);
        loadPreparedMixIntoEngine(
            attachPlan.targetFrame,
            attachPlan.sourceFrame,
            attachPlan.alignToTransport);
        discardObsoletePreparedMixPaths();
        appendLog(QStringLiteral("prepared mix: %1 frames in %2 ms pre_master_peak=%3 output_peak=%4 master_pre_gain=%5 over_unity_samples=%6 worker_requests=%7 worker_coalesced=%8 worker_failures=%9")
            .arg(preparedMixLifecycle_.active().frames)
            .arg(preparedMixLifecycle_.active().renderMs)
            .arg(preparedMixLifecycle_.active().preMasterPeak, 0, 'f', 4)
            .arg(preparedMixLifecycle_.active().outputPeak, 0, 'f', 4)
            .arg(preparedMixLifecycle_.active().masterPreGain, 0, 'f', 3)
            .arg(preparedMixLifecycle_.active().overUnitySamples)
            .arg(preparedMixLifecycle_.requests())
            .arg(preparedMixLifecycle_.coalesced())
            .arg(preparedMixLifecycle_.failures()));
        if (preparedForSharedBank) noteSharedBankReady(resultBank);
        if (trackController_.playback().phase ==
            SharedTrackController::PlaybackPhase::PreparingMix) {
            trackController_.preparedForTransport(
                trackController_.playback().arrangementRevision);
            updateTrackPlaybackPresentation();
        }
        if (preparedMixLifecycle_.takePlayWhenReady()) {
            if (attachPlan.alignToTransport) {
                if (gridScheduleLabel_) {
                    gridScheduleLabel_->setText(
                        trackRecordingWorkflow_.globalTransportPlaying()
                        ? QStringLiteral("Generated track: joining running transport on next beat")
                        : QStringLiteral("Generated track: ready for the scheduled play boundary"));
                }
            } else {
                restartPreparedTrackQuantized();
                if (gridScheduleLabel_) {
                    gridScheduleLabel_->setText(
                        QStringLiteral("Track restart: waiting for engine beat schedule"));
                }
            }
        }
    } catch (const std::exception& error) {
        preparedMixLifecycle_.noteMetadataFailure(QString::fromUtf8(error.what()));
        appendLog(QStringLiteral("prepared mix metadata failed: %1 worker_failures=%2")
            .arg(preparedMixLifecycle_.active().error)
            .arg(preparedMixLifecycle_.failures()));
    }
}

void MainWindow::adoptPreparedBankCache(int bankIndex)
{
    bankIndex = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    const PreparedMixResult& cached = preparedMixLifecycle_.cache(bankIndex);
    if (!preparedMixLifecycle_.activateCachedBank(
            bankIndex,
            looperProject_.banks().size(),
            QFileInfo::exists(cached.path))) {
        discardObsoletePreparedMixPaths();
        auto& track = trackController_.model();
        if (!track.userProvidedSource) {
            track.fileName.clear();
            track.filePath.clear();
            track.fileBytes = 0;
            track.durationMs = 0;
            track.sha256.clear();
        }
        updateTrackControls();
        loadTrackWaveform();
        return;
    }

    discardObsoletePreparedMixPaths();
    auto& track = trackController_.model();
    track.fileName = QStringLiteral("Prepared Section %1")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex));
    track.filePath = cached.path;
    track.fileBytes = cached.fileBytes;
    track.sampleRate = cached.sampleRate;
    track.sampleRateCompatible = true;
    track.userProvidedSource = false;
    track.durationMs = cached.durationMs;
    track.sha256 = cached.sha256;
    track.acceptedBpm = bankMetronomePattern(bankIndex).bpm;
    updateTrackControls();
    loadTrackWaveform();
}

void MainWindow::loadPreparedMixIntoEngine(
    std::uint64_t targetFrame,
    std::uint64_t sourceFrame,
    bool alignToRunningTransport)
{
    if (!jam2_.isRunning() || preparedMixLifecycle_.active().path.isEmpty() ||
        !preparedMixLifecycle_.active().error.isEmpty()) {
        return;
    }
    jam2::EngineCommand load;
    load.type = jam2::EngineCommandType::LoadPreparedTrack;
    load.frame = alignToRunningTransport ? targetFrame : 0ULL;
    load.frame_end = alignToRunningTransport ? sourceFrame : 0ULL;
    load.enabled = alignToRunningTransport;
    if (!jam2::engine_command_set_text(
            load,
            QDir::toNativeSeparators(
                preparedMixLifecycle_.active().path).toStdString())) {
        appendLog(QStringLiteral("engine command text is too long: load prepared track"));
        return;
    }
    const bool loadQueued = submitEngineCommand(load, QStringLiteral("load prepared track"));
    appendLog(QStringLiteral(
        "prepared track load: queued=%1 scheduled=%2 target_frame=%3 source_frame=%4 "
        "frames=%5 path=%6")
        .arg(loadQueued)
        .arg(alignToRunningTransport)
        .arg(targetFrame)
        .arg(sourceFrame)
        .arg(preparedMixLifecycle_.active().frames)
        .arg(preparedMixLifecycle_.active().path));
    if (alignToRunningTransport && loadQueued) {
        trackRecordingWorkflow_.notePreparedAttachScheduled(targetFrame);
        appendLog(QStringLiteral(
            "prepared track attach scheduled: target_frame=%1 source_frame=%2")
            .arg(targetFrame)
            .arg(sourceFrame));
    } else if (alignToRunningTransport) {
        trackRecordingWorkflow_.cancelPreparedAttach();
    }
    const SharedTrackController::EffectiveLoop loop =
        trackController_.effectiveLoop(
            trackRecordingWorkflow_.preparedSampleRate(),
            preparedMixLifecycle_.active().frames);
    if (loop.enabled) {
        setPreparedTrackLoop(
            true,
            static_cast<std::uint64_t>(loop.startFrame),
            static_cast<std::uint64_t>(loop.endFrame));
    } else {
        setPreparedTrackLoop(false);
    }
    sendPreparedTrackLevel();
}

void MainWindow::sendPreparedTrackLevel()
{
    const double gain = gainFromDb(trackController_.model().trackGainDb);
    submitEngineGain(jam2::EngineCommandType::PreparedSetLevel, gain, QStringLiteral("prepared track level"));
}

void MainWindow::updateTrackControls()
{
    const auto& model = trackController_.model();
    updateTrackPlaybackPresentation();
    if (trackSpeedSpin_) {
        const QSignalBlocker blocker(trackSpeedSpin_);
        trackSpeedSpin_->setValue(model.speed);
    }
    if (trackSpeedSlider_) {

        const QSignalBlocker blocker(trackSpeedSlider_);
        trackSpeedSlider_->setValue(qBound(10, qRound(model.speed * 100.0), 200));
    }
    if (trackPitchSpin_) {
        const QSignalBlocker blocker(trackPitchSpin_);
        trackPitchSpin_->setValue(qBound(-12, model.pitchCents / 100, 12));
    }
    if (trackPitchSlider_) {
        const QSignalBlocker blocker(trackPitchSlider_);
        trackPitchSlider_->setValue(qBound(-12, model.pitchCents / 100, 12));
    }
    if (trackLevelDbLabel_ && trackLevelSlider_) {
        trackLevelDbLabel_->setText(dbText(trackController_.model().trackGainDb));
        const QSignalBlocker blocker(trackLevelSlider_);
        trackLevelSlider_->setValue(qBound(-60, qRound(trackController_.model().trackGainDb), 12));
    }
    if (focusFrequencySlider_) {
        const QSignalBlocker blocker(focusFrequencySlider_);
        focusFrequencySlider_->setValue(qBound(40, qRound(model.focusFrequencyHz), 8000));
    }
    if (focusFrequencySpin_) {
        const QSignalBlocker blocker(focusFrequencySpin_);
        focusFrequencySpin_->setValue(qBound(40, qRound(model.focusFrequencyHz), 8000));
    }
    if (focusFrequencyCheck_) {
        const QSignalBlocker blocker(focusFrequencyCheck_);
        focusFrequencyCheck_->setChecked(model.focusEnabled);
    }
    const bool knownFocusPreset = !focusPresetBox_ || focusPresetBox_->findData(model.focusPreset) >= 0;
    const bool customFocus = isCustomFocusPreset(model.focusPreset) || !knownFocusPreset;
    if (focusPresetBox_) {
        const QSignalBlocker blocker(focusPresetBox_);
        const int index = focusPresetBox_->findData(customFocus ? QStringLiteral("custom") : model.focusPreset);
        focusPresetBox_->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (focusFrequencySlider_) {
        focusFrequencySlider_->setEnabled(customFocus);
    }
    if (focusFrequencySpin_) {
        focusFrequencySpin_->setEnabled(customFocus);
    }
    if (trackWaveform_) {
        trackWaveform_->setBpm(model.acceptedBpm);
        trackWaveform_->setLoop(
            model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : -1,
            model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : -1);
    }
    refreshLooperLanes();
    if (loopEnabledCheck_) {
        const QSignalBlocker blocker(loopEnabledCheck_);
        loopEnabledCheck_->setChecked(model.loopEnabled);
        if (model.loopStartSeconds >= 0.0 && model.loopEndSeconds > model.loopStartSeconds) {
            loopEnabledCheck_->setText(QStringLiteral("Loop: %1.%2 s to %3.%4 s")
                .arg(static_cast<int>(model.loopStartSeconds))
                .arg(static_cast<int>(std::llround(model.loopStartSeconds * 10.0)) % 10)
                .arg(static_cast<int>(model.loopEndSeconds))
                .arg(static_cast<int>(std::llround(model.loopEndSeconds * 10.0)) % 10));
        } else if (model.loopStartSeconds >= 0.0) {
            loopEnabledCheck_->setText(QStringLiteral("Loop from %1.%2 s")
                .arg(static_cast<int>(model.loopStartSeconds))
                .arg(static_cast<int>(std::llround(model.loopStartSeconds * 10.0)) % 10));
        } else if (model.loopEndSeconds >= 0.0) {
            loopEnabledCheck_->setText(QStringLiteral("Loop to %1.%2 s")
                .arg(static_cast<int>(model.loopEndSeconds))
                .arg(static_cast<int>(std::llround(model.loopEndSeconds * 10.0)) % 10));
        } else {
            loopEnabledCheck_->setText(QStringLiteral("Loop whole track"));
        }
    }
    const bool trackCompatible = trackController_.model().sampleRateCompatible;
    if (playTrackButton_) {
        playTrackButton_->setEnabled(trackCompatible);
        playTrackButton_->setToolTip(trackCompatible ? QString{} : QStringLiteral(
            "This track is unavailable because WAV conversion failed"));
    }
    if (stopTrackButton_) {
        stopTrackButton_->setEnabled(trackCompatible);
        stopTrackButton_->setToolTip(playTrackButton_ ? playTrackButton_->toolTip() : QString{});
    }
    updateSharedRecordingPresentation();
}

void MainWindow::updateTrackPlaybackPresentation()
{
    if (trackNameLabel_) {
        trackNameLabel_->setText(QStringLiteral("Track: %1 | %2")
            .arg(
                trackController_.model().fileName,
                trackController_.playbackStatusText(jamSyncPolicy_.globalPlayback)));
    }
    if (performanceTrackToggle_) {
        const bool playing =
            trackRecordingWorkflow_.globalTransportRequestedPlaying();
        performanceTrackToggle_->setText(
            playing ? QStringLiteral("■") : QStringLiteral("▶"));
        performanceTrackToggle_->setProperty("active", playing);
        performanceTrackToggle_->style()->unpolish(performanceTrackToggle_);
        performanceTrackToggle_->style()->polish(performanceTrackToggle_);
        const bool protectedState = sharedRecordingProtected();
        performanceTrackToggle_->setEnabled(
            !protectedState && trackController_.model().sampleRateCompatible && jam2_.isRunning());
        performanceTrackToggle_->setToolTip(protectedState
            ? QStringLiteral("Playback is protected because a synced track recording is active")
            : QString{});
    }
}

void MainWindow::refreshLoopbackSources()
{
    const QString previous = laneRecordingState_.loopbackSourceId.isEmpty()
        ? laneRecordingState_.loopbackSourceName
        : laneRecordingState_.loopbackSourceId;
    QVector<LoopbackSourceChoice> choices;
    QString error;
    const QStringList sources = GuiLoopbackRecorder::listSources(&error);

    const QRegularExpression re(QStringLiteral("^\\s*\\[([^\\]]+)\\]\\s*(.*)$"));
    for (const QString& line : sources) {
        const QString trimmed = line.trimmed();
        const QRegularExpressionMatch match = re.match(trimmed);
        if (match.hasMatch()) {
            choices.push_back({trimmed, match.captured(1)});
        }
    }
    if (choices.isEmpty()) {
        choices.push_back({
            QStringLiteral("[default] System mix"),
            QStringLiteral("default"),
        });
        appendLog(error.isEmpty() ? QStringLiteral("no loopback sources returned") : error);
    } else {
        appendLog(QStringLiteral("loaded %1 loopback sources").arg(choices.size()));
    }

    const auto restored = std::find_if(
        choices.cbegin(), choices.cend(), [&previous](const LoopbackSourceChoice& choice) {
            return choice.id == previous || choice.label == previous;
        });
    const LoopbackSourceChoice selected = restored != choices.cend()
        ? *restored : choices.front();
    laneRecordingState_.loopbackSources = std::move(choices);
    laneRecordingState_.loopbackSourceId = selected.id;
    laneRecordingState_.loopbackSourceName = selected.label;
}

QString MainWindow::recordingLatencySummary() const
{
    const int sampleRate = qMax(1, trackRecordingWorkflow_.latencySampleRate());
    const qint64 adjustment = laneRecordingState_.latencyAdjustmentFrames;
    const auto milliseconds = [sampleRate](quint64 frames) {
        return static_cast<double>(frames) * 1000.0 /
            static_cast<double>(sampleRate);
    };
    return QStringLiteral(
        "Input %1 (%2 ms) | Output %3 (%4 ms) | Active processing %5 (%6 ms) | "
        "Manual %7 | Applied %8 (%9 ms)")
        .arg(trackRecordingWorkflow_.inputLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.inputLatencyFrames()), 0, 'f', 2)
        .arg(trackRecordingWorkflow_.outputLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.outputLatencyFrames()), 0, 'f', 2)
        .arg(trackRecordingWorkflow_.sourceLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.sourceLatencyFrames()), 0, 'f', 2)
        .arg(adjustment >= 0
            ? QStringLiteral("+%1").arg(adjustment)
            : QString::number(adjustment))
        .arg(trackRecordingWorkflow_.appliedLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.appliedLatencyFrames()), 0, 'f', 2);
}

void MainWindow::startInputCapture(std::uint64_t targetFrame, int countInBars)
{
    if (trackRecordingWorkflow_.inputTakeActive()) {
        return;
    }
    QString permissionError;
    if (!jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track Recording Microphone Access"), permissionError);
        appendLog(permissionError);
        return;
    }
    if (!jam2_.isRunning()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track Recording"), QStringLiteral("Input lane recording requires Perform mode so the engine can record the active ASIO input."));
        return;
    }
    int recordingSampleRate = 0;
    QString rateError;
    if (!recordingTargetSampleRate(recordingSampleRate, rateError)) {
        appendLog(QStringLiteral("could not start input take: ") + rateError);
        QMessageBox::warning(
            this, QStringLiteral("Jam2 Track Recording"), rateError);
        return;
    }
    QString output = laneRecordingState_.outputPath.trimmed();
    if (isAutoCapturePath(output)) {

        output = timestampedCapturePath(
            QStringLiteral("track-input"),
            jamAssetFolder(JamStorage::AssetKind::Recorded));
        laneRecordingState_.outputPath = output;
    }
    QString workflowError;
    const int durationBars = (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked())
        ? captureDurationSpin_->value()
        : 0;
    const auto pattern = currentMetronomePattern();
    const std::uint64_t durationFrames = jam2::gui::recording_frames_for_bars(
        durationBars,
        pattern.beats_per_bar,
        pattern.bpm,
        recordingSampleRate,
        pattern.tempo_pulse_units);
    // Close the small scheduling window before the visible recording phase is
    // published so a remote transport edge cannot land between arming the
    // engine take and enabling local isolation.
    jam2_.setLaneRecordingIsolationEnabled(true);
    if (!trackRecordingWorkflow_.startInputTake(
            output,
            !QFileInfo::exists(output),
            recordingSampleRate,
            targetFrame,
            durationFrames,
            countInBars >= 0 ? std::optional<int>(countInBars) : std::nullopt,
            metronomeTransport_.grid().position(),
            pattern.beats_per_bar,
            trackRecordingWorkflow_.includePreparedInTake(),
            trackRecordingWorkflow_.includeMetronomeInTake(),
            true,
            workflowError)) {
        const QString transientPath =
            trackRecordingWorkflow_.abandonPendingCapture();
        if (!transientPath.isEmpty()) {
            registerTransientTrackWav(transientPath);
        }
        appendLog(QStringLiteral("could not start input take: ") + workflowError);
        updateLaneRecordingIsolation();
        return;
    }
    if (countInBars >= 0 && recordingCountdownLabel_) {
        recordingCountdownLabel_->setText(
            QStringLiteral("ARMED  ✓  WAITING FOR BEAT  ●  COUNT-IN  ›  RECORDING"));
    }
    const QString limitText = durationBars > 0
        ? QStringLiteral(" duration_bars=%1 duration_frames=%2").arg(durationBars).arg(durationFrames)
        : QStringLiteral(" duration_bars=manual");
    const QString startText = (countInBars >= 0
        ? QStringLiteral("Recording: armed input take, sample_rate=%1 engine_quantized_count_in_bars=%2 latency_compensation_frames=%3 output=%4")
            .arg(recordingSampleRate)
            .arg(countInBars)
            .arg(trackRecordingWorkflow_.appliedLatencyFrames())
            .arg(output)
        : QStringLiteral("Recording: armed input take, sample_rate=%1 start_frame=%2 latency_compensation_frames=%3 output=%4")
            .arg(recordingSampleRate)
            .arg(targetFrame)
            .arg(trackRecordingWorkflow_.appliedLatencyFrames())
            .arg(output)) + limitText;
    if (gridScheduleLabel_) {
        gridScheduleLabel_->setText(startText);
    }
    appendLog(startText);
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
    publishLocalTrackRecordingState(
        countInBars >= 0 ? QStringLiteral("waiting") : QStringLiteral("recording"));
}

void MainWindow::startInputCaptureAtGroupSchedule(
    std::uint64_t countdownFrame,
    std::uint64_t targetFrame,
    std::uint64_t targetMusicalFrame,
    int countInBars)
{
    if (trackRecordingWorkflow_.inputTakeActive()) return;
    QString permissionError;
    if (!jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(
            this, QStringLiteral("Jam2 Track Recording Microphone Access"),
            permissionError);
        appendLog(permissionError);
        publishLocalTrackRecordingState(QStringLiteral("complete"));
        return;
    }
    int recordingSampleRate = 0;
    QString rateError;
    if (!jam2_.isRunning() ||
        !recordingTargetSampleRate(recordingSampleRate, rateError)) {
        appendLog(QStringLiteral("could not start synced input take: ") + rateError);
        publishLocalTrackRecordingState(QStringLiteral("complete"));
        return;
    }
    QString output = laneRecordingState_.outputPath.trimmed();
    if (isAutoCapturePath(output)) {
        output = timestampedCapturePath(
            QStringLiteral("track-input"),
            jamAssetFolder(JamStorage::AssetKind::Recorded));
        laneRecordingState_.outputPath = output;
    }
    const int durationBars = (!captureManualStopCheck_ ||
            !captureManualStopCheck_->isChecked())
        ? captureDurationSpin_->value() : 0;
    const auto pattern = currentMetronomePattern();
    const std::uint64_t durationFrames = jam2::gui::recording_frames_for_bars(
        durationBars,
        pattern.beats_per_bar,
        pattern.bpm,
        recordingSampleRate,
        pattern.tempo_pulse_units);
    const bool countInMetronome = countInBars > 0 &&
        captureCountInMetronomeCheck_ &&
        captureCountInMetronomeCheck_->isChecked();
    const bool keepMetronome = captureKeepMetronomeCheck_ &&
        captureKeepMetronomeCheck_->isChecked();
    const bool startedCountInClick = countInMetronome &&
        !metronomeTransport_.localRunning();
    if (startedCountInClick) startTrackMetronome();
    trackRecordingWorkflow_.waitForCountIn(
        0, startedCountInClick && !keepMetronome);

    QString workflowError;
    if (!trackRecordingWorkflow_.startInputTakeAtSchedule(
            output,
            !QFileInfo::exists(output),
            recordingSampleRate,
            countdownFrame,
            targetFrame,
            targetMusicalFrame,
            durationFrames,
            trackRecordingWorkflow_.includePreparedInTake(),
            trackRecordingWorkflow_.includeMetronomeInTake(),
            workflowError)) {
        const QString transientPath =
            trackRecordingWorkflow_.abandonPendingCapture();
        if (!transientPath.isEmpty()) {
            registerTransientTrackWav(transientPath);
        }
        appendLog(QStringLiteral("could not start synced input take: ") +
            workflowError);
        publishLocalTrackRecordingState(QStringLiteral("complete"));
        return;
    }
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
    localTrackRecordingCountInBars_ = qBound(0, countInBars, 8);
    publishLocalTrackRecordingState(QStringLiteral("waiting"));
    appendLog(QStringLiteral(
        "synced recording scheduled: group=%1 countdown_frame=%2 start_frame=%3 count_in_bars=%4")
        .arg(activeRecordingGroupId_.left(8))
        .arg(countdownFrame)
        .arg(targetFrame)
        .arg(countInBars));
}

void MainWindow::startLoopbackCapture()
{
    if (loopbackRecorder_.isRunning()) {
        return;
    }
    int recordingSampleRate = 0;
    QString rateError;
    if (!recordingTargetSampleRate(recordingSampleRate, rateError)) {
        appendLog(QStringLiteral("loopback recording failed to start: ") + rateError);
        publishLocalTrackRecordingState(QStringLiteral("armed"));
        QMessageBox::warning(
            this, QStringLiteral("Jam2 Loopback Recording"), rateError);
        return;
    }
    QString output = laneRecordingState_.outputPath.trimmed();
    if (isAutoCapturePath(output)) {
        output = timestampedCapturePath(
            QStringLiteral("loopback"),
            jamAssetFolder(JamStorage::AssetKind::Recorded));
        laneRecordingState_.outputPath = output;
    }
    trackRecordingWorkflow_.beginLoopbackCapture(
        output, !QFileInfo::exists(output), recordingSampleRate);
    QString source = laneRecordingState_.loopbackSourceId.trimmed();
    if (source.isEmpty()) {
        source = laneRecordingState_.loopbackSourceName.trimmed();
    }
    if (source.isEmpty()) {
        source = QStringLiteral("default");
    }

    GuiLoopbackOptions options;
    options.source = source;
    options.outputPath = output;
    options.targetSampleRate = recordingSampleRate;
    options.durationBars = (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked())
        ? captureDurationSpin_->value()
        : 0;
    const auto pattern = currentMetronomePattern();
    options.bpm = pattern.bpm;
    options.beatsPerBar = pattern.beats_per_bar;
    options.tempoPulseUnits = pattern.tempo_pulse_units;
    options.silenceThresholdDb = laneRecordingState_.silenceThresholdDb;
    options.tailSilenceMs = laneRecordingState_.tailSilenceMs;
    options.trimLeadingSilence = laneRecordingState_.trimLeading;
    options.trimTrailingSilence = laneRecordingState_.trimTrailing;

    QString error;
    appendLog(QStringLiteral(
        "starting internal loopback recording: target_sample_rate=%1 duration_bars=%2 bpm=%3 meter=%4/%5 silence_threshold_db=%6 trim_leading=%7 trim_trailing=%8 output=%9")
        .arg(recordingSampleRate)
        .arg(options.durationBars > 0 ? QString::number(options.durationBars) : QStringLiteral("manual"))
        .arg(options.bpm, 0, 'f', 3)
        .arg(options.beatsPerBar)
        .arg(pattern.beat_unit)
        .arg(options.silenceThresholdDb, 0, 'f', 1)
        .arg(options.trimLeadingSilence ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(options.trimTrailingSilence ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(output));
    jam2_.setLaneRecordingIsolationEnabled(true);
    if (!loopbackRecorder_.start(options, [this](
            bool ok,
            const QString& outputPath,
            const QString& errorText,
            const QString& diagnostics) {
            QMetaObject::invokeMethod(this, [this, ok, outputPath, errorText, diagnostics] {
                loopbackRecordingPreviewClock_.invalidate();
                if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
                const QString transientPath = ok
                    ? trackRecordingWorkflow_.finishLoopbackCapture(outputPath)
                    : trackRecordingWorkflow_.failLoopbackCapture();
                if (!diagnostics.isEmpty()) {
                    appendLog(diagnostics);
                }
                if (!ok) {
                    if (!transientPath.isEmpty() && QFileInfo::exists(transientPath)) {
                        registerTransientTrackWav(transientPath);
                    }
                    if (loadWavButton_) loadWavButton_->setEnabled(true);
                    appendLog(QStringLiteral("loopback recording failed: ") + errorText);
                    if (!automationSuppressDialogs_) {
                        QMessageBox::warning(
                            this,
                            QStringLiteral("Loopback Recording"),
                            errorText.isEmpty()
                                ? QStringLiteral("The loopback recording did not complete.")
                                : errorText);
                    }
                    finishLaneTakeFinalization();
                    return;
                }
                if (!transientPath.isEmpty()) {
                    registerTransientTrackWav(transientPath);
                }
                if (loadWavButton_) {
                    loadWavButton_->setEnabled(true);
                }
                if (trackRecordingWorkflow_.laneArmed()) {
                    publishLocalTrackRecordingState(QStringLiteral("finalizing"));
                    importLastCaptureToArmedLane();
                } else {
                    finishLaneTakeFinalization();
                }
            }, Qt::QueuedConnection);
        }, &error)) {
        const QString transientPath = trackRecordingWorkflow_.failLoopbackCapture();
        if (!transientPath.isEmpty() && QFileInfo::exists(transientPath)) {
            registerTransientTrackWav(transientPath);
        }
        if (loadWavButton_) loadWavButton_->setEnabled(true);
        appendLog(QStringLiteral("loopback recording failed to start: ") + error);
        finishLaneTakeFinalization();
        loopbackRecordingPreviewClock_.invalidate();
        return;

    }
    loopbackRecordingPreviewClock_.start();
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
    publishLocalTrackRecordingState(QStringLiteral("recording"));
}

bool MainWindow::scheduleLoopbackCountIn(
    int bars,
    bool stopMetronomeAtStart)
{
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    const int beatsPerBar = qMax(1, currentMetronomePattern().beats_per_bar);
    const std::uint64_t countdownBeat =
        jam2::gui::synced_recording_countdown_beat(position, beatsPerBar);
    const std::uint64_t beatFrames = position.sampleRate > 0 &&
            position.secondsPerBeat > 0.0
        ? static_cast<std::uint64_t>(std::llround(
            position.secondsPerBeat * static_cast<double>(position.sampleRate)))
        : 0;
    const std::uint64_t countInBeats =
        static_cast<std::uint64_t>(qMax(1, bars)) *
        static_cast<std::uint64_t>(beatsPerBar);
    if (countdownBeat == 0 || beatFrames == 0 ||
        countdownBeat > (std::numeric_limits<std::uint64_t>::max)() - countInBeats ||
        countdownBeat + countInBeats >
            ((std::numeric_limits<std::uint64_t>::max)() - position.epochFrame) /
                beatFrames) {
        appendLog(QStringLiteral(
            "loopback count-in is waiting for a valid engine grid"));
        publishLocalTrackRecordingState(QStringLiteral("armed"));
        return false;
    }
    const std::uint64_t targetBeat = countdownBeat + countInBeats;
    loopbackCountdownStartFrame_ = rawFrameFromMusicalFrame(
        position.epochFrame + countdownBeat * beatFrames,
        position.renderOffsetFrames);
    loopbackRecordingStartFrame_ = rawFrameFromMusicalFrame(
        position.epochFrame + targetBeat * beatFrames,
        position.renderOffsetFrames);
    stopMetronomeAtLoopbackStart_ = stopMetronomeAtStart;
    localTrackRecordingCountInBars_ = qBound(1, bars, 8);
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
    publishLocalTrackRecordingState(QStringLiteral("waiting"));
    appendLog(QStringLiteral(
        "loopback recording scheduled: countdown_frame=%1 start_frame=%2 count_in_bars=%3")
        .arg(loopbackCountdownStartFrame_)
        .arg(loopbackRecordingStartFrame_)
        .arg(localTrackRecordingCountInBars_));
    return true;
}

bool MainWindow::updateLoopbackCountIn(
    const PlaybackGrid::Position& position)
{
    if (loopbackRecordingStartFrame_ == 0) return false;
    if (position.rawCurrentFrame < loopbackCountdownStartFrame_) {
        if (recordingCountdownLabel_) {
            recordingCountdownLabel_->setText(QStringLiteral(
                "ARMED - WAITING FOR BEAT - COUNT-IN - RECORDING"));
        }
        publishLocalTrackRecordingState(QStringLiteral("waiting"));
        return true;
    }
    if (position.rawCurrentFrame < loopbackRecordingStartFrame_) {
        const std::uint64_t beatFrames = position.sampleRate > 0 &&
                position.secondsPerBeat > 0.0
            ? qMax<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(
                position.secondsPerBeat * static_cast<double>(position.sampleRate))))
            : qMax<std::uint64_t>(
                1, static_cast<std::uint64_t>(qMax(1, position.sampleRate)) / 2ULL);
        const std::uint64_t remainingFrames =
            loopbackRecordingStartFrame_ - position.rawCurrentFrame;
        const int remaining = static_cast<int>(qMin<std::uint64_t>(
            4096, (remainingFrames + beatFrames - 1ULL) / beatFrames));
        if (recordingCountdownLabel_) {
            recordingCountdownLabel_->setText(QStringLiteral(
                "ARMED - WAITING FOR BEAT - COUNT-IN %1 - RECORDING")
                .arg(remaining));
        }
        publishLocalTrackRecordingState(QStringLiteral("count-in"), remaining);
        return true;
    }

    const std::uint64_t targetFrame = loopbackRecordingStartFrame_;
    const std::uint64_t lateFrames = position.rawCurrentFrame - targetFrame;
    const double lateMs = position.sampleRate > 0
        ? static_cast<double>(lateFrames) * 1000.0 /
            static_cast<double>(position.sampleRate)
        : 0.0;
    const bool stopMetronome = stopMetronomeAtLoopbackStart_;
    loopbackCountdownStartFrame_ = 0;
    loopbackRecordingStartFrame_ = 0;
    stopMetronomeAtLoopbackStart_ = false;
    if (stopMetronome) stopTrackMetronome();
    appendLog(QStringLiteral(
        "loopback count-in completed: target_frame=%1 actual_frame=%2 late_frames=%3 late_ms=%4")
        .arg(targetFrame)
        .arg(position.rawCurrentFrame)
        .arg(lateFrames)
        .arg(lateMs, 0, 'f', 3));
    startLoopbackCapture();
    return true;
}

void MainWindow::cancelLoopbackCountIn()
{
    const bool stopMetronome = stopMetronomeAtLoopbackStart_;
    loopbackCountdownStartFrame_ = 0;
    loopbackRecordingStartFrame_ = 0;
    stopMetronomeAtLoopbackStart_ = false;
    localTrackRecordingCountInBars_ = 0;
    if (stopMetronome) stopTrackMetronome();
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
    if (loadWavButton_) loadWavButton_->setEnabled(true);
    publishLocalTrackRecordingState(QStringLiteral("armed"));
    appendLog(QStringLiteral("cancelled loopback recording count-in"));
}

void MainWindow::stopInputCapture(std::uint64_t targetFrame)
{
    if (loopbackRecordingStartFrame_ > 0) {
        cancelLoopbackCountIn();
        return;
    }
    if (trackRecordingWorkflow_.inputTakeActive()) {
        if (!trackRecordingWorkflow_.stopInputTake(targetFrame)) {
            appendLog(QStringLiteral("engine command queue unavailable: stop track take"));
            return;
        }
        const QString stopText = QStringLiteral("Recording: stop requested target_frame=%1").arg(targetFrame);
        if (gridScheduleLabel_) {
            gridScheduleLabel_->setText(stopText);
        }
        appendLog(stopText);
        return;
    }
    if (loopbackRecorder_.isRunning()) {
        loopbackRecorder_.stop();
    }
}

void MainWindow::loadTrackWaveform()
{
    const std::uint64_t revision = ++trackWaveformRevision_;
    const QString path = trackController_.model().filePath;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (trackWaveform_) {
            trackWaveform_->clear();
        }
        if (performanceHome_) {
            performanceHome_->setTrackWaveform({}, false);
        }
        return;
    }
    if (trackWaveformWorkerRunning_) {
        return;
    }
    trackWaveformWorkerRunning_ = true;
    auto peaks = std::make_shared<std::vector<float>>();
    auto valid = std::make_shared<bool>(false);
    const bool started = startFileWorkerTask(
        [path, peaks, valid] {
            constexpr int peakCount = 2048;
            *valid = readPcm16WaveformPeaks(path, peakCount, *peaks);
        },
        [this, path, revision, peaks, valid] {
            trackWaveformWorkerRunning_ = false;
            if (revision != trackWaveformRevision_ || path != trackController_.model().filePath) {
                if (!preparedMixLifecycle_.retainsPath(path)) {
                    (void)projectPersistence_.discardTransientWav(path);
                }
                loadTrackWaveform();
                return;
            }
            if (trackWaveform_) {
                trackWaveform_->setPeaks(*peaks, *valid);
                trackWaveform_->setDurationMs(trackController_.model().durationMs);
                trackWaveform_->setBpm(trackController_.model().acceptedBpm);
            }
            if (performanceHome_) {
                performanceHome_->setTrackWaveform(std::move(*peaks), *valid);
            }
        },
        [this, revision](const QString&) {
            trackWaveformWorkerRunning_ = false;
            QTimer::singleShot(100, this, [this, revision] {
                if (revision == trackWaveformRevision_) {
                    loadTrackWaveform();
                }
            });
        });
    if (!started) {
        trackWaveformWorkerRunning_ = false;
        QTimer::singleShot(100, this, [this, revision] {
            if (revision == trackWaveformRevision_) {
                loadTrackWaveform();
            }
        });
    }
}

void MainWindow::playTrack()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("global playback change ignored while a synced recording is active"));
        return;
    }
    if (!jam2_.isRunning()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Start the local engine before playing the prepared mix."));
        return;
    }
    publishStoppedTrackStateWhenApplied_ = false;
    trackController_.requestPlayback(true);
    updateTrackPlaybackPresentation();
    const int activeBank = looperProject_.activeBankIndex();
    const int countInBars = performanceCountInCheck_ &&
        performanceCountInCheck_->isChecked() ? 1 : 0;
    const int beatsPerBar = bankMetronomePattern(activeBank).beats_per_bar;
    const bool hasRenderableSources =
        PreparedMixRenderer::hasRenderableSources(looperProject_, activeBank);
    const PlaybackGrid::Position playPosition =
        metronomeTransport_.grid().position();
    const auto logScheduledPlay = [this, &playPosition, countInBars](
            QStringView source) {
        appendLog(QStringLiteral(
            "global play scheduled: source=%1 count_in_bars=%2 current_frame=%3 "
            "countdown_frame=%4 target_frame=%5")
            .arg(source.toString())
            .arg(countInBars)
            .arg(playPosition.rawCurrentFrame)
            .arg(trackRecordingWorkflow_.globalTransportCountdownStartFrame())
            .arg(trackRecordingWorkflow_.globalTransportTimelineStartFrame()));
    };
    if (!hasRenderableSources) {
        if (!preparedMixLifecycle_.active().path.isEmpty()) {
            discardPreparedMix(false);
        }
        if (!trackRecordingWorkflow_.restartGlobalTransport(
                playPosition,
                false,
                countInBars,
                beatsPerBar)) {
            trackController_.requestPlayback(false);
            updateTrackPlaybackPresentation();
            appendLog(QStringLiteral(
                "engine command queue unavailable: global transport restart"));
            return;
        }
        logScheduledPlay(QStringLiteral("empty-bank"));
        if (gridScheduleLabel_) {
            gridScheduleLabel_->setText(countInBars > 0
                ? QStringLiteral("Global play: one-bar count-in")
                : QStringLiteral("Global play: waiting for next grid beat"));
        }
        updateTrackPlaybackPresentation();
        return;
    }
    if (preparedMixLifecycle_.workerRunning() ||
        preparedMixLifecycle_.active().path.isEmpty() ||
        !preparedMixLifecycle_.active().error.isEmpty()) {
        preparedMixLifecycle_.setPlayWhenReady(true);
        regeneratePreparedMix(activeBank);
        if (!trackRecordingWorkflow_.restartGlobalTransport(
                playPosition,
                false,
                countInBars,
                beatsPerBar)) {
            preparedMixLifecycle_.setPlayWhenReady(false);
            trackController_.requestPlayback(false);
            updateTrackPlaybackPresentation();
            appendLog(QStringLiteral(
                "engine command queue unavailable: start transport while preparing mix"));
            return;
        }
        logScheduledPlay(QStringLiteral("preparing-mix"));
        if (gridScheduleLabel_) {
            gridScheduleLabel_->setText(countInBars > 0
                ? QStringLiteral("Global play: one-bar count-in; backing track is preparing")
                : QStringLiteral("Global play started; backing track will join when ready"));
        }
        updateTrackPlaybackPresentation();
        return;
    }
    loadPreparedMixIntoEngine();
    restartPreparedTrackQuantized();
    logScheduledPlay(QStringLiteral("prepared-mix"));
    if (gridScheduleLabel_) {
        gridScheduleLabel_->setText(countInBars > 0
            ? QStringLiteral("Track restart: one-bar count-in")
            : QStringLiteral("Track restart: waiting for next grid beat"));
    }
}

void MainWindow::stopTrack(std::uint64_t targetFrame)
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral("global playback stop ignored while a synced recording is active"));
        return;
    }
    if (!jam2_.isRunning()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Start the local engine before stopping the prepared mix."));
        return;
    }
    if (!trackRecordingWorkflow_.stopPrepared(
            targetFrame,
            metronomeTransport_.grid().position().currentFrame)) {
        appendLog(QStringLiteral("engine command queue unavailable: stop prepared track"));
        return;
    }
    trackController_.requestPlayback(false);
    updateTrackPlaybackPresentation();
    if (jamSyncPolicy_.globalPlayback && sessionController_.isServer()) {
        publishStoppedTrackStateWhenApplied_ = true;
    }
    updateTrackTimeline();
}

void MainWindow::setLoopStartAtCurrentPosition()
{
    if (sharedRecordingProtected()) return;
    if (!trackController_.setLoopStartAtMilliseconds(
            currentAudibleTrackPositionMs())) return;
    updateTrackControls();
    updateTrackTimeline();
    loadPreparedMixIntoEngine();
}

void MainWindow::setLoopEndAtCurrentPosition()
{
    if (sharedRecordingProtected()) return;
    if (!trackController_.setLoopEndAtMilliseconds(
            currentAudibleTrackPositionMs())) return;
    updateTrackControls();
    updateTrackTimeline();
    loadPreparedMixIntoEngine();
}

void MainWindow::clearTrackLoop()
{
    if (sharedRecordingProtected()) return;
    trackController_.clearLoop();
    updateTrackControls();
    updateTrackTimeline();
    loadPreparedMixIntoEngine();
}

qint64 MainWindow::currentAudibleTrackPositionMs() const
{
    return trackRecordingWorkflow_.currentAudiblePositionMs(
        metronomeTransport_.grid().position(), trackController_.model().durationMs);
}

void MainWindow::updateTrackTimeline()

{
    auto& model = trackController_.model();
    qint64 position = currentAudibleTrackPositionMs();
    if (trackWaveform_) {

        const qint64 duration = model.durationMs;
        trackWaveform_->setDurationMs(duration);
        trackWaveform_->setPlayheadMs(position);
        trackWaveform_->setBpm(model.acceptedBpm);
        trackWaveform_->setLoop(
            model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : -1,
            model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : -1);
    }
    if (looperStack_) {
        looperStack_->setPlaybackMarkers(
            position,
            model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : -1,
            model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : -1);
    }
    if (!jam2_.isRunning() && mixMetronomeMeter_) {
        mixMetronomeMeter_->setLevel(0.0);
    }
}

void MainWindow::sendMetronomeStateToJam(bool enabled)
{
    if (!jamSyncPolicy_.metronomeState || !jam2_.isNetworkRunning()) return;
    sendControl(QJsonObject{
        {QStringLiteral("type"), sessionController_.isServer()
            ? QStringLiteral("jam.metronome.state.set")
            : QStringLiteral("jam.metronome.state.request")},
        {QStringLiteral("enabled"), enabled},
    });
}

void MainWindow::setMetronomeEnabled(bool enabled, bool publishToJam)
{
    const bool followerUsingLeaderAudio = jam2_.isNetworkRunning() &&
        !sessionController_.isServer() && leaderAudioModeActive();
    const bool localEnabled = enabled && !followerUsingLeaderAudio;
    const bool changed = metronomeTransport_.localRunning() != localEnabled;
    metronomeTransport_.setLocalState(localEnabled);
    if (jam2_.isRunning() && changed) {
        submitEngineToggle(
            jam2::EngineCommandType::SetMetronomeEnabled,
            localEnabled,
            QStringLiteral("metronome enabled"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(!localEnabled);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(localEnabled);
    }
    if (publishToJam) sendMetronomeStateToJam(enabled);
    updateTrackControls();
}

void MainWindow::startTrackMetronome()
{
    if (jam2_.isRunning()) {
        if (metronomeTransport_.localRunning()) {
            // Record count-in and duplicate UI activation must not create a
            // second grid proposal while the shared clock is already running.
            if (startTrackMetronomeButton_) {
                startTrackMetronomeButton_->setEnabled(false);
            }
            if (stopTrackMetronomeButton_) {
                stopTrackMetronomeButton_->setEnabled(true);
            }
            return;
        }
        updateRuntimeControls();
        setMetronomeEnabled(true, true);
        return;
    }

    QMessageBox::warning(this, QStringLiteral("Jam2 Metronome"), QStringLiteral("Start the local engine before using the track metronome."));
}

void MainWindow::stopTrackMetronome()
{
    setMetronomeEnabled(false, true);
}

void MainWindow::tapTrackMetronomeTempo()
{
    if (!metronomeBpmSpin_) {
        return;
    }
    if (!tapTempoClock_.isValid()) {
        tapTempoClock_.start();
    }
    applyTapTrackMetronomeTempoAt(tapTempoClock_.elapsed());
}

void MainWindow::applyTapTrackMetronomeTempoAt(std::int64_t elapsedMs)
{
    if (const std::optional<int> bpm = tapTempoTracker_.tap(elapsedMs)) {
        metronomeBpmSpin_->setValue(*bpm);
    }
}

void MainWindow::updateTrackMetronomeInterval()
{
    if (bpmSpin_ && metronomeBpmSpin_) {
        bpmSpin_->setValue(metronomeBpmSpin_->value());
    }
    sendMetronomePatternToJam();
}

void MainWindow::rebuildMetronomePattern(bool resetToDivisionDefault)
{
    if (!metronomePatternWidget_) {
        return;
    }
    const int beats = metronomeBeatsSpin_ ? metronomeBeatsSpin_->currentData().toInt() : 4;
    const int division = metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1;
    const int steps = qMax(1, beats * qMax(1, division));
    QVector<bool> previousEnabled = metronomeEnabledSteps_;
    QVector<bool> previous = metronomeAccents_;
    metronomeEnabledSteps_.resize(steps);
    metronomeAccents_.resize(steps);
    for (int i = 0; i < steps; ++i) {

        metronomeEnabledSteps_[i] = !resetToDivisionDefault && i < previousEnabled.size()
            ? previousEnabled[i]
            : true;
        metronomeAccents_[i] = !resetToDivisionDefault && i < previous.size()
            ? previous[i]
            : i == 0;
    }

    const int tempoPulseUnits = metronomeTempoPulseBox_
        ? qMax(1, metronomeTempoPulseBox_->currentData().toInt()) : 1;
    metronomePatternWidget_->setPattern(
        beats,
        division,
        tempoPulseUnits,
        metronomeEnabledSteps_,
        metronomeAccents_);

    const int bpm = metronomeBpmSpin_ ? qMax(1, metronomeBpmSpin_->value()) : 80;
    const int beatUnit = metronomeBeatUnitBox_
        ? qMax(1, metronomeBeatUnitBox_->currentData().toInt()) : 4;
    const double intervalMs = 60000.0 /
        (static_cast<double>(bpm) * tempoPulseUnits * division);
    if (metronomeMeterReadout_) {
        metronomeMeterReadout_->setText(
            QStringLiteral("%1 / %2").arg(beats).arg(beatUnit));
    }
    if (metronomeIntervalReadout_) {
        metronomeIntervalReadout_->setText(
            QStringLiteral("%1 ms").arg(intervalMs, 0, 'f', 1));
    }
    if (metronomeNebula_) metronomeNebula_->setBpm(bpm);
    sendMetronomePatternToJam();
}

jam2::metronome::PatternSnapshot MainWindow::currentMetronomePattern() const
{
    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 80;
    pattern.beats_per_bar = metronomeBeatsSpin_ ? metronomeBeatsSpin_->currentData().toInt() : 4;
    pattern.beat_unit = metronomeBeatUnitBox_ ? metronomeBeatUnitBox_->currentData().toInt() : 4;
    pattern.tempo_pulse_units = metronomeTempoPulseBox_
        ? metronomeTempoPulseBox_->currentData().toInt() : 1;
    pattern.division = metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1;
    pattern.step_count = jam2::metronome::pattern_step_count(pattern.beats_per_bar, pattern.division);
    pattern.play_mask_low = 0;
    pattern.play_mask_high = 0;
    pattern.accent_mask_low = 0;
    pattern.accent_mask_high = 0;
    for (int step = 0; step < pattern.step_count; ++step) {
        const bool play = step < metronomeEnabledSteps_.size() ? metronomeEnabledSteps_[step] : true;
        const bool accent = step < metronomeAccents_.size()
            ? metronomeAccents_[step]
            : step == 0;
        jam2::metronome::set_mask_enabled(pattern.play_mask_low, pattern.play_mask_high, step, play);
        jam2::metronome::set_mask_enabled(pattern.accent_mask_low, pattern.accent_mask_high, step, accent);
    }
    return jam2::metronome::sanitize(pattern);
}

jam2::metronome::PatternSnapshot MainWindow::bankMetronomePattern(int bankIndex) const
{
    const LooperBankTiming timing = looperProject_.resolvedTiming(bankIndex);
    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = timing.bpm;
    pattern.beats_per_bar = timing.beatsPerBar;
    pattern.beat_unit = timing.beatUnit;
    pattern.tempo_pulse_units = timing.tempoPulseUnits;
    pattern.division = timing.division;
    pattern.play_mask_low = timing.playMaskLow;
    pattern.play_mask_high = timing.playMaskHigh;
    pattern.accent_mask_low = timing.accentMaskLow;
    pattern.accent_mask_high = timing.accentMaskHigh;
    return jam2::metronome::sanitize(pattern);
}

void MainWindow::storeCurrentMetronomePatternForBank(
    int bankIndex,
    bool inheritBankA)
{
    if (applyingBankTiming_) return;
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    LooperBankTiming timing;
    timing.bpm = pattern.bpm;
    timing.beatsPerBar = pattern.beats_per_bar;
    timing.beatUnit = pattern.beat_unit;
    timing.tempoPulseUnits = pattern.tempo_pulse_units;
    timing.division = pattern.division;
    timing.playMaskLow = pattern.play_mask_low;
    timing.playMaskHigh = pattern.play_mask_high;
    timing.accentMaskLow = pattern.accent_mask_low;
    timing.accentMaskHigh = pattern.accent_mask_high;
    timing.inheritsBankA = bankIndex > 0 && inheritBankA;
    (void)looperProject_.setTiming(bankIndex, std::move(timing));
}

void MainWindow::applyMetronomePatternForBank(int bankIndex, bool transmit)
{
    const jam2::metronome::PatternSnapshot pattern = bankMetronomePattern(bankIndex);
    applyingBankTiming_ = true;
    {
        const QSignalBlocker bpmBlocker(metronomeBpmSpin_);
        const QSignalBlocker beatsBlocker(metronomeBeatsSpin_);
        const QSignalBlocker beatUnitBlocker(metronomeBeatUnitBox_);
        const QSignalBlocker tempoPulseBlocker(metronomeTempoPulseBox_);
        const QSignalBlocker divisionBlocker(metronomeDivisionBox_);
        metronomeBpmSpin_->setValue(pattern.bpm);
        const int beatsIndex = metronomeBeatsSpin_->findData(pattern.beats_per_bar);
        if (beatsIndex >= 0) metronomeBeatsSpin_->setCurrentIndex(beatsIndex);
        const int beatUnitIndex = metronomeBeatUnitBox_->findData(pattern.beat_unit);
        if (beatUnitIndex >= 0) metronomeBeatUnitBox_->setCurrentIndex(beatUnitIndex);
        const int pulseIndex = metronomeTempoPulseBox_->findData(pattern.tempo_pulse_units);
        if (pulseIndex >= 0) metronomeTempoPulseBox_->setCurrentIndex(pulseIndex);
        const int divisionIndex = metronomeDivisionBox_->findData(pattern.division);
        if (divisionIndex >= 0) metronomeDivisionBox_->setCurrentIndex(divisionIndex);
    }
    metronomeEnabledSteps_.resize(pattern.step_count);
    metronomeAccents_.resize(pattern.step_count);
    for (int step = 0; step < pattern.step_count; ++step) {
        metronomeEnabledSteps_[step] = jam2::metronome::mask_enabled(
            pattern.play_mask_low, pattern.play_mask_high, step);
        metronomeAccents_[step] = jam2::metronome::mask_enabled(
            pattern.accent_mask_low, pattern.accent_mask_high, step);
    }
    rebuildMetronomePattern(false);
    applyingBankTiming_ = false;
    trackController_.model().acceptedBpm = pattern.bpm;
    if (bpmSpin_) {
        const QSignalBlocker blocker(bpmSpin_);
        bpmSpin_->setValue(pattern.bpm);
    }
    if (transmit) sendMetronomePatternToJam();
}

void MainWindow::initializeLegacyBankTiming()
{
    if (looperProject_.hasSerializedTiming()) {
        applyMetronomePatternForBank(looperProject_.activeBankIndex(), false);
        return;
    }
    const jam2::metronome::PatternSnapshot fallback = currentMetronomePattern();
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        LooperBankTiming timing;
        timing.bpm = fallback.bpm;
        timing.beatsPerBar = fallback.beats_per_bar;
        timing.beatUnit = fallback.beat_unit;
        timing.tempoPulseUnits = fallback.tempo_pulse_units;
        timing.division = fallback.division;
        timing.playMaskLow = fallback.play_mask_low;
        timing.playMaskHigh = fallback.play_mask_high;
        timing.accentMaskLow = fallback.accent_mask_low;
        timing.accentMaskHigh = fallback.accent_mask_high;
        timing.inheritsBankA = bank > 0;
        if (bank < chordModel_.sections().size()) {
            const auto& recipe = chordModel_.section(bank).generatedRecipe;
            if (recipe.isValid()) {
                timing.bpm = recipe.bpm;
                timing.beatsPerBar = recipe.meterNumerator;
                timing.beatUnit = recipe.meterDenominator;
                timing.tempoPulseUnits = recipe.tempoPulseUnits;
                timing.division = recipe.clickDivision;
                timing.playMaskLow = 0;
                timing.playMaskHigh = 0;
                timing.accentMaskLow = 0;
                timing.accentMaskHigh = 0;
                const int steps = jam2::metronome::pattern_step_count(
                    timing.beatsPerBar, timing.division);
                for (int step = 0; step < steps; ++step) {
                    jam2::metronome::set_mask_enabled(
                        timing.playMaskLow, timing.playMaskHigh, step, true);
                }
                jam2::metronome::set_mask_enabled(
                    timing.accentMaskLow, timing.accentMaskHigh, 0, true);
                timing.inheritsBankA = false;
            }
        }
        (void)looperProject_.setTiming(bank, std::move(timing));
    }
    applyMetronomePatternForBank(looperProject_.activeBankIndex(), false);
}

int MainWindow::sectionBeatsPerBar(int bankIndex) const
{
    return qMax(1, bankMetronomePattern(bankIndex).beats_per_bar);
}

bool MainWindow::bankGridTimingDiffers(int bankIndex) const
{
    const auto current = currentMetronomePattern();
    const auto target = bankMetronomePattern(bankIndex);
    return current.bpm != target.bpm ||
        current.beats_per_bar != target.beats_per_bar ||
        current.beat_unit != target.beat_unit ||
        current.tempo_pulse_units != target.tempo_pulse_units;
}

void MainWindow::sendMetronomeModeToJam()
{
    if (!jam2_.isRunning() || !metronomeModeBox_ ||
        !metronomeTransport_.localGridMutationAllowed()) {
        return;
    }
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::SetMetronomeMode;
    command.value = metronomeModeBox_->currentText() == QStringLiteral("leader-audio") ? 1 :
        metronomeModeBox_->currentText() == QStringLiteral("listener-compensated") ? 2 : 0;
    if (!metronomeTransport_.submit(command)) {
        appendLog(QStringLiteral("engine command rejected: metronome mode"));
    }
}

void MainWindow::sendMetronomeSoundToJam()
{
    if (!jam2_.isRunning() || !metronomeSoundBox_) {
        return;
    }
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::SetMetronomeSound;
    command.value = static_cast<int>(jam2::metronome::sanitize_click_sound(
        metronomeSoundBox_->currentData().toInt()));
    if (!jam2_.submit(command)) {
        appendLog(QStringLiteral("engine command rejected: metronome sound"));
    }
}

void MainWindow::showMetronomeCompensationDialog()
{
    ListenerCompensationDialog dialog(metronomeCompensationSettings(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const Jam2MetronomeCompensationSettings settings = dialog.settings();
    if (metronomeCompensationMaxSpin_) {
        metronomeCompensationMaxSpin_->setValue(settings.maximum_ms);
    }
    if (metronomeCompensationSmoothingSpin_) {
        metronomeCompensationSmoothingSpin_->setValue(settings.smoothing_ms);
    }
    if (metronomeCompensationDeadbandSpin_) {
        metronomeCompensationDeadbandSpin_->setValue(settings.deadband_ms);
    }
    if (metronomeCompensationSlewSpin_) {
        metronomeCompensationSlewSpin_->setValue(settings.slew_ms_per_second);
    }
    preferences_.metronome.compensationMaxMs = settings.maximum_ms;
    preferences_.metronome.compensationSmoothingMs = settings.smoothing_ms;
    preferences_.metronome.compensationDeadbandMs = settings.deadband_ms;
    preferences_.metronome.compensationSlewMsPerSecond = settings.slew_ms_per_second;
    UserPreferencesStore::save(preferences_);
    applyMetronomeCompensationToRunningJam();
    appendLog(QStringLiteral(
        "listener compensation saved: max_ms=%1 smoothing_ms=%2 "
        "deadband_ms=%3 slew_ms_per_sec=%4")
        .arg(settings.maximum_ms, 0, 'f', 1)
        .arg(settings.smoothing_ms, 0, 'f', 1)
        .arg(settings.deadband_ms, 0, 'f', 1)
        .arg(settings.slew_ms_per_second, 0, 'f', 1));
}

Jam2MetronomeCompensationSettings MainWindow::metronomeCompensationSettings() const
{
    return {
        metronomeCompensationMaxSpin_
            ? metronomeCompensationMaxSpin_->value()
            : preferences_.metronome.compensationMaxMs,
        metronomeCompensationSmoothingSpin_
            ? metronomeCompensationSmoothingSpin_->value()
            : preferences_.metronome.compensationSmoothingMs,
        metronomeCompensationDeadbandSpin_
            ? metronomeCompensationDeadbandSpin_->value()
            : preferences_.metronome.compensationDeadbandMs,
        metronomeCompensationSlewSpin_
            ? metronomeCompensationSlewSpin_->value()
            : preferences_.metronome.compensationSlewMsPerSecond,
    };
}

void MainWindow::applyMetronomeCompensationToRunningJam()
{
    if (!jam2_.isNetworkRunning()) {
        return;
    }
    if (!jam2_.setMetronomeCompensation(metronomeCompensationSettings())) {
        appendLog(QStringLiteral(
            "listener compensation update rejected by the running network session"));
    }
}

void MainWindow::updateMetronomeCompensationVisibility()
{
    if (!metronomeCompensationButton_ || !metronomeModeBox_) {
        return;
    }
    const QString mode = metronomeModeBox_->currentText();
    metronomeCompensationButton_->setVisible(
        mode == QStringLiteral("listener-compensated"));
    if (metronomeModeDescription_) {
        metronomeModeDescription_->setText(
            mode == QStringLiteral("leader-audio")
                ? QStringLiteral("The leader's rendered click audio is shared with every listener.")
                : mode == QStringLiteral("listener-compensated")
                    ? QStringLiteral("Each listener offsets the shared click using measured network timing.")
                    : QStringLiteral("Everyone renders the same shared timing grid locally."));
    }
}

void MainWindow::sendMetronomePatternToJam()
{
    if (applyingBankTiming_) return;
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral(
            "held metronome timing unchanged while a track take is active"));
        return;
    }
    const int activeBank = looperProject_.activeBankIndex();
    const jam2::metronome::PatternSnapshot previous =
        bankMetronomePattern(activeBank);
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    const bool gridTimingChanged =
        previous.bpm != pattern.bpm ||
        previous.beats_per_bar != pattern.beats_per_bar ||
        previous.beat_unit != pattern.beat_unit ||
        previous.tempo_pulse_units != pattern.tempo_pulse_units;
    if (gridTimingChanged) {
        stopTrackForPracticeIdeaGeneration();
        preparedMixLifecycle_.invalidateAll();
        discardPreparedMix(false);
        appendLog(QStringLiteral(
            "grid timing changed: stopped playback and invalidated prepared bank WAVs"));
    }
    storeCurrentMetronomePatternForBank(looperProject_.activeBankIndex());
    if (!jam2_.isRunning() ||
        !metronomeTransport_.localGridMutationAllowed()) {
        return;
    }
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::SetMetronomePattern;

    command.pattern = pattern;
    if (!metronomeTransport_.submit(command)) {
        appendLog(QStringLiteral("engine command rejected: metronome pattern"));
    }
}

void MainWindow::updateMetronomePresentationFromEngine(
    const jam2::EngineSnapshot& snapshot)
{
    const jam2::metronome::PatternSnapshot pattern =
        jam2::metronome::sanitize(snapshot.metronome_pattern);
    const QString mode = snapshot.metronome_mode == jam2::EngineMetronomeMode::LeaderAudio
        ? QStringLiteral("leader-audio")
        : snapshot.metronome_mode == jam2::EngineMetronomeMode::ListenerCompensated
            ? QStringLiteral("listener-compensated")
            : QStringLiteral("shared-grid");
    const jam2::metronome::PatternSnapshot shown = currentMetronomePattern();
    const bool patternChanged = pattern.bpm != shown.bpm ||
        pattern.beats_per_bar != shown.beats_per_bar ||
        pattern.beat_unit != shown.beat_unit ||
        pattern.tempo_pulse_units != shown.tempo_pulse_units ||
        pattern.division != shown.division ||
        pattern.play_mask_low != shown.play_mask_low ||
        pattern.play_mask_high != shown.play_mask_high ||
        pattern.accent_mask_low != shown.accent_mask_low ||
        pattern.accent_mask_high != shown.accent_mask_high;
    metronomeTransport_.setApplyingRemoteSettings(true);
    if (patternChanged && metronomeBpmSpin_) {
        const QSignalBlocker blocker(metronomeBpmSpin_);
        metronomeBpmSpin_->setValue(pattern.bpm);
    }
    if (patternChanged && bpmSpin_) {
        const QSignalBlocker blocker(bpmSpin_);
        bpmSpin_->setValue(pattern.bpm);
    }
    if (patternChanged && metronomeBeatsSpin_) {
        const QSignalBlocker blocker(metronomeBeatsSpin_);
        const int beatsIndex = metronomeBeatsSpin_->findData(pattern.beats_per_bar);
        if (beatsIndex >= 0) metronomeBeatsSpin_->setCurrentIndex(beatsIndex);
    }
    if (patternChanged && metronomeBeatUnitBox_) {
        const QSignalBlocker blocker(metronomeBeatUnitBox_);
        const int index = metronomeBeatUnitBox_->findData(pattern.beat_unit);
        if (index >= 0) {
            metronomeBeatUnitBox_->setCurrentIndex(index);
        }
    }
    if (patternChanged && metronomeTempoPulseBox_) {
        const QSignalBlocker blocker(metronomeTempoPulseBox_);
        const int index = metronomeTempoPulseBox_->findData(pattern.tempo_pulse_units);
        if (index >= 0) metronomeTempoPulseBox_->setCurrentIndex(index);
    }
    if (patternChanged && metronomeDivisionBox_) {
        const QSignalBlocker blocker(metronomeDivisionBox_);
        const int index = metronomeDivisionBox_->findData(pattern.division);
        if (index >= 0) {
            metronomeDivisionBox_->setCurrentIndex(index);
        }
    }
    if (metronomeModeBox_) {
        const QSignalBlocker blocker(metronomeModeBox_);
        const int index = metronomeModeBox_->findText(mode);
        if (index >= 0) {
            metronomeModeBox_->setCurrentIndex(index);
        }
    }

    if (patternChanged) {
        metronomeEnabledSteps_.resize(pattern.step_count);
        metronomeAccents_.resize(pattern.step_count);
        for (int step = 0; step < pattern.step_count; ++step) {
            metronomeEnabledSteps_[step] = jam2::metronome::mask_enabled(
                pattern.play_mask_low, pattern.play_mask_high, step);
            metronomeAccents_[step] = jam2::metronome::mask_enabled(
                pattern.accent_mask_low, pattern.accent_mask_high, step);
        }
        rebuildMetronomePattern(false);
        refreshLooperLanes();
    }
    metronomeTransport_.setLocalState(snapshot.metronome_enabled);
    metronomeTransport_.setApplyingRemoteSettings(false);
    updateMetronomeCompensationVisibility();
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(!snapshot.metronome_enabled);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(snapshot.metronome_enabled);
    }
}

void MainWindow::publishLocalTrackBatch(const QString& requestedBatchId)
{
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (!jam2_.isRunning() ||
        (session.role != SharedSessionController::Role::Creator &&
         session.role != SharedSessionController::Role::Joiner)) {
        return;
    }
    const QString batchId = requestedBatchId.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()
        : requestedBatchId.toLower();
    QJsonArray tracks;
    QSet<QString> batchHashes;
    for (int bankIndex = 0; bankIndex < looperProject_.banks().size(); ++bankIndex) {
        const LooperBank& bank = looperProject_.banks().at(bankIndex);
        for (const LooperLane& lane : bank.lanes) {
            if (lane.localOnly || !lane.sampleRateCompatible || lane.sampleRate <= 0 ||
                (session.contract.sampleRate > 0 &&
                 lane.sampleRate != session.contract.sampleRate) ||
                !isSha256Hex(lane.assetHash) || lane.assetPath.isEmpty() ||
                tracks.size() >= kMaxLooperTrackContributions) {
                continue;
            }
            const QByteArray contributionIdentity = QCryptographicHash::hash(
                QStringLiteral("%1:%2:%3")
                    .arg(bankIndex)
                    .arg(lane.id, lane.assetHash)
                    .toUtf8(),
                QCryptographicHash::Sha256).left(16);
            const QString contributionId = QUuid::fromRfc4122(contributionIdentity)
                .toString(QUuid::WithoutBraces).toLower();
            while (!trackOfferAssetPaths_.contains(lane.assetHash) &&
                   trackOfferAssetPaths_.size() >= kMaxLooperTrackContributions) {
                trackOfferAssetPaths_.erase(trackOfferAssetPaths_.begin());
            }
            const QJsonObject track{
                {QStringLiteral("recording_id"), contributionId},
                {QStringLiteral("bank"), bankIndex},
                {QStringLiteral("target_lane_id"), lane.id},
                {QStringLiteral("sha256"), lane.assetHash},
                {QStringLiteral("name"), lane.name.left(512)},
                {QStringLiteral("sample_rate"), lane.sampleRate},
                {QStringLiteral("source_frames"), QString::number(lane.sourceFrames)},
                {QStringLiteral("start_frame"), QString::number(lane.startFrame)},
                {QStringLiteral("stop_frame"), QString::number(lane.stopFrame)},
                {QStringLiteral("loop_start_frame"), QString::number(lane.loopStartFrame)},
                {QStringLiteral("loop_end_frame"), QString::number(lane.loopEndFrame)},
                {QStringLiteral("loop_enabled"), lane.loopEnabled},
            };
            tracks.append(track);
            batchHashes.insert(lane.assetHash);
            trackOfferAssetPaths_.insert(lane.assetHash, lane.assetPath);
            localTrackOffers_.insert(contributionId, track);
        }
    }
    const QJsonObject offer{
        {QStringLiteral("type"), QStringLiteral("looper.track.batch.offer")},
        {QStringLiteral("batch_id"), batchId},
        {QStringLiteral("tracks"), tracks},
    };
    if (!sessionController_.send(offer)) {
        appendLog(QStringLiteral("could not queue atomic Track Sync batch %1")
            .arg(batchId.left(8)));
        if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
        return;
    }
    QSet<QString> recipients;
    if (session.role == SharedSessionController::Role::Creator) {
        for (const SharedSessionController::PeerSnapshot& peer : session.peers) {
            if (!peer.token.isEmpty() && peer.token != session.localToken) {
                recipients.insert(peer.token);
            }
        }
    } else {
        recipients.insert(QString{});
    }
    trackWorkspace_.outgoingTrackSharePendingPeers.insert(batchId, recipients);
    trackWorkspace_.outgoingTrackShareBatchHashes.insert(batchId, batchHashes);
    trackWorkspace_.outgoingTrackShareLastProgressMs.insert(
        batchId, QDateTime::currentMSecsSinceEpoch());
    scheduleOutgoingTrackBatchExpiry(batchId);
    if (performanceHome_) {
        performanceHome_->setTrackTransferStatus(
            QStringLiteral("SHARING TRACKS\u2026  0 / %1").arg(tracks.size()));
    }
    appendLog(QStringLiteral("offered atomic Track Sync batch %1 with %2 track(s)")
        .arg(batchId.left(8)).arg(tracks.size()));
}

void MainWindow::shareLocalTracks(bool includeLocalOnly)
{
    if (!includeLocalOnly && !automaticWavSharingEnabled()) {
        return;
    }
    if (!jam2_.isRunning()) {
        appendLog(QStringLiteral("Share Tracks requires an active jam"));
        return;
    }
    int promoted = 0;
    if (includeLocalOnly) {
        for (LooperBank& bank : looperProject_.banks()) {
            for (LooperLane& lane : bank.lanes) {
                if (lane.localOnly) {
                    lane.localOnly = false;
                    ++promoted;
                }
            }
        }
    }
    if (promoted > 0) {
        refreshLooperLanes();
        appendLog(QStringLiteral(
            "Share Tracks promoted %1 local-only practice reference lane(s)")
            .arg(promoted));
    }
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (session.remotePeerCount <= 0) {
        appendLog(QStringLiteral("Share Tracks requires at least one connected peer"));
        if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
        return;
    }
    if (session.role != SharedSessionController::Role::Creator &&
        session.role != SharedSessionController::Role::Joiner) {
        return;
    }
    const QString requestedBatchId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("looper.track.share.request")},
        {QStringLiteral("batch_id"), requestedBatchId},
    });
    publishLocalTrackBatch({});
    appendLog(QStringLiteral(
        "started non-destructive two-way track share; peer_batch=%1")
        .arg(requestedBatchId.left(8)));
}

void MainWindow::handleTrackBatchOffer(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const bool validPath = sessionController_.isServer()
        ? !sourcePeerToken.isEmpty()
        : sourcePeerToken.isEmpty();
    if (!validPath) {
        appendLog(QStringLiteral("rejected track offer from an invalid control path"));
        return;
    }
    const QString batchId = message.value(QStringLiteral("batch_id")).toString().toLower();
    const QJsonArray tracks = message.value(QStringLiteral("tracks")).toArray();
    if (tracks.isEmpty()) {
        sendControlTo(sourcePeerToken, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.track.batch.complete")},
            {QStringLiteral("batch_id"), batchId},
            {QStringLiteral("tracks"), 0},
        });
        if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
        return;
    }
    int previouslyApplied = 0;
    for (const QJsonValue& trackValue : tracks) {
        const QJsonObject track = trackValue.toObject();
        const QString contributionId =
            track.value(QStringLiteral("recording_id")).toString().toLower();
        const QString contributionKey = sourcePeerToken + QLatin1Char(':') +
            batchId + QLatin1Char(':') + contributionId;
        if (appliedTrackContributionIds_.contains(contributionKey)) {
            ++previouslyApplied;
        }
    }
    if (previouslyApplied > 0) {
        if (previouslyApplied == tracks.size()) {
            appendLog(QStringLiteral(
                "acknowledged replay of completed Track Sync batch %1 with %2 track(s)")
                .arg(batchId.left(8)).arg(tracks.size()));
            sendControlTo(sourcePeerToken, QJsonObject{
                {QStringLiteral("type"), QStringLiteral("looper.track.batch.complete")},
                {QStringLiteral("batch_id"), batchId},
                {QStringLiteral("tracks"), tracks.size()},
            });
        } else {
            appendLog(QStringLiteral(
                "rejected Track Sync batch %1 mixing applied and new contribution identities")
                .arg(batchId.left(8)));
        }
        if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
        return;
    }
    if (pendingTrackContributions_.size() + tracks.size() > kMaxLooperTrackContributions) {
        appendLog(QStringLiteral("track contribution queue is full"));
        return;
    }
    const auto offerFrame = [](const QJsonObject& object, const QString& key, qint64 fallback) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) return fallback;
        if (value.isString()) {
            bool ok = false;
            const qint64 parsed = value.toString().toLongLong(&ok);
            return ok ? parsed : fallback;
        }
        return value.toInteger(fallback);
    };
    bool insertedAny = false;
    for (const QJsonValue& trackValue : tracks) {
        const QJsonObject track = trackValue.toObject();
        const QString contributionId =
            track.value(QStringLiteral("recording_id")).toString().toLower();
        const QString contributionKey = sourcePeerToken + QLatin1Char(':') +
            batchId + QLatin1Char(':') + contributionId;
        if (pendingTrackContributions_.contains(contributionKey)) {
            continue;
        }
        const QString hash = track.value(QStringLiteral("sha256")).toString().toLower();
        if (validatedTrackAssetHashes_.contains(hash)) {
            bool stillAvailable = QFileInfo::exists(looperAssetPathForHash(hash));
            for (const LooperBank& bank : looperProject_.banks()) {
                for (const LooperLane& lane : bank.lanes) {
                    if (lane.assetHash == hash &&
                        QFileInfo::exists(looperAssetAbsolutePath(lane))) {
                        stillAvailable = true;
                        break;
                    }
                }
                if (stillAvailable) break;
            }
            if (!stillAvailable) validatedTrackAssetHashes_.remove(hash);
        }
        pendingTrackContributions_.insert(contributionKey, PendingTrackContribution{
            sourcePeerToken,
            batchId,
            static_cast<int>(tracks.size()),
            contributionId,
            track.value(QStringLiteral("bank")).toInt(),
            track.value(QStringLiteral("target_lane_id")).toString(),
            hash,
            track.value(QStringLiteral("name")).toString(),
            track.value(QStringLiteral("sample_rate")).toInt(),
            offerFrame(track, QStringLiteral("source_frames"), 0),
            offerFrame(track, QStringLiteral("start_frame"), 0),
            offerFrame(track, QStringLiteral("stop_frame"), -1),
            offerFrame(track, QStringLiteral("loop_start_frame"), -1),
            offerFrame(track, QStringLiteral("loop_end_frame"), -1),
            track.value(QStringLiteral("loop_enabled")).toBool(),
        });
        insertedAny = true;
        if (!validatedTrackAssetHashes_.contains(hash)) {
            for (const LooperBank& bank : looperProject_.banks()) {
                for (const LooperLane& lane : bank.lanes) {
                    if (lane.assetHash == hash && QFileInfo::exists(looperAssetAbsolutePath(lane))) {
                        validatedTrackAssetHashes_.insert(hash);
                        break;
                    }
                }
                if (validatedTrackAssetHashes_.contains(hash)) {
                    break;
                }
            }
        }
    }
    if (insertedAny) {
        const QString activityKey = sourcePeerToken + QLatin1Char(':') + batchId;
        trackWorkspace_.incomingTrackShareLastProgressMs.insert(
            activityKey, QDateTime::currentMSecsSinceEpoch());
        scheduleIncomingTrackBatchExpiry(sourcePeerToken, batchId);
    }
    if (performanceHome_) {
        performanceHome_->setTrackTransferStatus(
            QStringLiteral("RECEIVING TRACKS\u2026  0 / %1").arg(tracks.size()));
    }
    if (automationOfferPauseActive_) return;
    if (automationOfferPauseArmed_) {
        automationOfferPauseArmed_ = false;
        automationOfferPauseActive_ = true;
        return;
    }
    applyPendingTrackContributions();
    requestNextPendingAsset();
}

void MainWindow::handleTrackBatchComplete(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const QString batchId = message.value(QStringLiteral("batch_id")).toString().toLower();
    auto pending = trackWorkspace_.outgoingTrackSharePendingPeers.find(batchId);
    if (pending == trackWorkspace_.outgoingTrackSharePendingPeers.end()) {
        appendLog(QStringLiteral("ignored stale Track Sync batch completion %1")
            .arg(batchId.left(8)));
        return;
    }
    pending->remove(sourcePeerToken);
    if (!pending->isEmpty()) {
        appendLog(QStringLiteral("Track Sync batch %1 is still pending for %2 peer(s)")
            .arg(batchId.left(8)).arg(pending->size()));
        return;
    }
    trackWorkspace_.outgoingTrackSharePendingPeers.erase(pending);
    trackWorkspace_.outgoingTrackShareBatchHashes.remove(batchId);
    trackWorkspace_.outgoingTrackShareLastProgressMs.remove(batchId);
    if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
    appendLog(QStringLiteral("Track Sync batch %1 completed with %2 track(s)")
        .arg(message.value(QStringLiteral("batch_id")).toString().left(8))
        .arg(message.value(QStringLiteral("tracks")).toInt()));
    releaseHeldTrackSnapshotIfReady();
}

void MainWindow::supersedePendingTrackBatches(const QString& sourcePeerToken)
{
    QMap<QString, int> supersededBatchSizes;
    QStringList supersededKeys;
    QSet<QString> supersededHashes;
    for (auto it = pendingTrackContributions_.cbegin();
         it != pendingTrackContributions_.cend(); ++it) {
        if (it->sourcePeerToken != sourcePeerToken) continue;
        supersededKeys.append(it.key());
        supersededHashes.insert(it->assetHash);
        supersededBatchSizes[it->batchId] = qMax(
            supersededBatchSizes.value(it->batchId), it->batchSize);
    }
    if (supersededKeys.isEmpty()) return;

    const bool discardActiveRequest =
        incomingAssetWorkflow_ == IncomingAssetWorkflow::TrackContribution &&
        incomingAssetSourcePeerToken_ == sourcePeerToken;
    for (const QString& key : supersededKeys) {
        pendingTrackContributions_.remove(key);
        appliedTrackContributionIds_.insert(key);
    }
    while (appliedTrackContributionIds_.size() > kMaxLooperTrackContributions * 2) {
        appliedTrackContributionIds_.erase(appliedTrackContributionIds_.begin());
    }
    for (auto it = supersededBatchSizes.cbegin();
         it != supersededBatchSizes.cend(); ++it) {
        trackWorkspace_.incomingTrackShareLastProgressMs.remove(
            sourcePeerToken + QLatin1Char(':') + it.key());
        sendControlTo(sourcePeerToken, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.track.batch.complete")},
            {QStringLiteral("batch_id"), it.key()},
            {QStringLiteral("tracks"), it.value()},
        });
        appendLog(QStringLiteral(
            "superseded pending Track Sync batch %1 with a newer arrangement from the same peer")
            .arg(it.key().left(8)));
    }
    for (const QString& hash : supersededHashes) {
        bool stillExpectedByTrack = false;
        for (const PendingTrackContribution& contribution :
             pendingTrackContributions_) {
            if (contribution.assetHash == hash) {
                stillExpectedByTrack = true;
                break;
            }
        }
        if (!stillExpectedByTrack) {
            pendingTrackAssetSources_.remove(hash);
            if (!pendingLooperAssetHashes_.contains(hash)) {
                trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
                trackWorkspace_.incomingAssetRetrySources.remove(hash);
            }
        }
    }
    if (discardActiveRequest) {
        ++trackWorkspace_.incomingAssetRequestGeneration;
        incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
        incomingAssetHash_.clear();
        incomingAssetSourcePeerToken_.clear();
        assetTransfer_.discardIncoming();
    }
    if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
}

void MainWindow::scheduleOutgoingTrackBatchExpiry(const QString& batchId)
{
    if (!trackWorkspace_.outgoingTrackSharePendingPeers.contains(batchId)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastProgress =
        trackWorkspace_.outgoingTrackShareLastProgressMs.value(batchId, now);
    const qint64 elapsed = qMax<qint64>(0, now - lastProgress);
    const int remaining = static_cast<int>(qMax<qint64>(
        1, kTrackBatchIdleTimeoutMs - elapsed));
    QTimer::singleShot(remaining, this,
        [this, batchId] {
            if (!trackWorkspace_.outgoingTrackSharePendingPeers.contains(batchId)) return;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 lastProgress =
                trackWorkspace_.outgoingTrackShareLastProgressMs.value(batchId, now);
            const qint64 elapsed = qMax<qint64>(0, now - lastProgress);
            if (elapsed < kTrackBatchIdleTimeoutMs) {
                scheduleOutgoingTrackBatchExpiry(batchId);
                return;
            }
            trackWorkspace_.outgoingTrackSharePendingPeers.remove(batchId);
            trackWorkspace_.outgoingTrackShareBatchHashes.remove(batchId);
            trackWorkspace_.outgoingTrackShareLastProgressMs.remove(batchId);
            if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
            appendLog(QStringLiteral(
                "Track Sync batch %1 was idle for 30 seconds; released held arrangement updates")
                .arg(batchId.left(8)));
            releaseHeldTrackSnapshotIfReady();
        });
}

void MainWindow::scheduleIncomingTrackBatchExpiry(
    const QString& sourcePeerToken,
    const QString& batchId)
{
    const QString activityKey = sourcePeerToken + QLatin1Char(':') + batchId;
    if (!trackWorkspace_.incomingTrackShareLastProgressMs.contains(activityKey)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastProgress =
        trackWorkspace_.incomingTrackShareLastProgressMs.value(activityKey, now);
    const qint64 elapsed = qMax<qint64>(0, now - lastProgress);
    const int remaining = static_cast<int>(qMax<qint64>(
        1, kTrackBatchIdleTimeoutMs - elapsed));
    QTimer::singleShot(remaining, this,
        [this, sourcePeerToken, batchId, activityKey] {
            if (!trackWorkspace_.incomingTrackShareLastProgressMs.contains(activityKey)) return;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 lastProgress =
                trackWorkspace_.incomingTrackShareLastProgressMs.value(activityKey, now);
            const qint64 elapsed = qMax<qint64>(0, now - lastProgress);
            if (elapsed < kTrackBatchIdleTimeoutMs) {
                scheduleIncomingTrackBatchExpiry(sourcePeerToken, batchId);
                return;
            }
            expirePendingTrackBatch(sourcePeerToken, batchId);
        });
}

void MainWindow::noteTrackAssetProgress(
    const QString& hash,
    const QString& peerToken,
    bool receiving)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (receiving) {
        for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
            if (contribution.assetHash == hash &&
                contribution.sourcePeerToken == peerToken) {
                trackWorkspace_.incomingTrackShareLastProgressMs[
                    contribution.sourcePeerToken + QLatin1Char(':') +
                    contribution.batchId] = now;
            }
        }
        return;
    }
    for (auto it = trackWorkspace_.outgoingTrackSharePendingPeers.cbegin();
         it != trackWorkspace_.outgoingTrackSharePendingPeers.cend(); ++it) {
        if (!it.value().contains(peerToken) ||
            !trackWorkspace_.outgoingTrackShareBatchHashes.value(it.key()).contains(hash)) {
            continue;
        }
        trackWorkspace_.outgoingTrackShareLastProgressMs[it.key()] = now;
    }
}

void MainWindow::releaseHeldTrackSnapshotIfReady()
{
    if (!trackWorkspace_.outgoingTrackSharePendingPeers.isEmpty()) return;
    const QJsonObject held = trackWorkspace_.heldTrackShareSongSet;
    const QString source = trackWorkspace_.heldTrackShareSongSourcePeerToken;
    trackWorkspace_.heldTrackShareSongSet = {};
    trackWorkspace_.heldTrackShareSongSourcePeerToken.clear();
    if (!held.isEmpty()) handleSongSet(held, source);
}

void MainWindow::requestNextPendingAsset()
{
    if (automationOfferPauseActive_) return;
    if (incomingAssetWorkflow_ != IncomingAssetWorkflow::None) {
        return;
    }
    QString hash;
    QString source;
    IncomingAssetWorkflow workflow = IncomingAssetWorkflow::None;
    for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
        if (!validatedTrackAssetHashes_.contains(contribution.assetHash)) {
            hash = contribution.assetHash;
            source = contribution.sourcePeerToken;
            workflow = IncomingAssetWorkflow::TrackContribution;
            break;
        }
    }
    if (workflow == IncomingAssetWorkflow::None &&
        !pendingSongSet_.isEmpty() && !pendingLooperAssetHashes_.isEmpty()) {
        hash = pendingLooperAssetHashes_.constFirst();
        source = pendingSongSourcePeerToken_;
        workflow = IncomingAssetWorkflow::Arrangement;
    }
    if (workflow == IncomingAssetWorkflow::None || !isSha256Hex(hash)) {
        return;
    }

    incomingAssetWorkflow_ = workflow;
    incomingAssetHash_ = hash;
    incomingAssetSourcePeerToken_ = source;
    const std::uint64_t requestGeneration =
        ++trackWorkspace_.incomingAssetRequestGeneration;
    if (workflow == IncomingAssetWorkflow::TrackContribution) {
        pendingTrackAssetSources_.insert(hash, source);
    }
    QJsonArray hashes;
    hashes.append(hash);
    const QJsonObject request{
        {QStringLiteral("type"), QStringLiteral("looper.asset.request")},
        {QStringLiteral("arrangement_revision"), pendingSongRevision_},
        {QStringLiteral("hashes"), hashes},
    };
    const bool sent = source.isEmpty()
        ? (jamSyncAllowsControlMessage(request) && sessionController_.send(request))
        : sendControlTo(source, request);
    if (!sent) {
        pendingTrackAssetSources_.remove(hash);
        incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
        incomingAssetHash_.clear();
        incomingAssetSourcePeerToken_.clear();
        appendLog(QStringLiteral(
            "could not request looper asset: workflow=%1 hash=%2 source=%3")
            .arg(workflow == IncomingAssetWorkflow::TrackContribution
                    ? QStringLiteral("track-share")
                    : QStringLiteral("arrangement"),
                hash,
                source.left(8)));
        retryOrFailIncomingAsset(hash, source);
        return;
    }
    appendLog(QStringLiteral(
        "requested looper asset: workflow=%1 hash=%2 source=%3 revision=%4")
        .arg(workflow == IncomingAssetWorkflow::TrackContribution
                ? QStringLiteral("track-share")
                : QStringLiteral("arrangement"),
            hash,
            source.left(8))
        .arg(pendingSongRevision_));
    QTimer::singleShot(kLooperAssetRequestStartHangTimeoutMs, this,
        [this, workflow, hash, source, requestGeneration] {
            handleAssetRequestStartTimeout(
                workflow,
                hash,
                source,
                requestGeneration,
                kLooperAssetRequestStartHangTimeoutMs);
        });
}

void MainWindow::handleAssetRequestStartTimeout(
    IncomingAssetWorkflow workflow,
    const QString& hash,
    const QString& source,
    std::uint64_t requestGeneration,
    int timeoutMilliseconds)
{
    if (trackWorkspace_.incomingAssetRequestGeneration != requestGeneration ||
        incomingAssetWorkflow_ != workflow || incomingAssetHash_ != hash ||
        incomingAssetSourcePeerToken_ != source ||
        assetTransfer_.incomingTransferActive()) {
        return;
    }
    // Callers may pass the current request members themselves. Preserve the
    // request identity before clearing those members so logging and retry
    // ownership cannot observe emptied aliases.
    const QString expiredHash = hash;
    const QString expiredSource = source;
    ++trackWorkspace_.assetRequestStartTimeouts;
    pendingTrackAssetSources_.remove(expiredHash);
    incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
    incomingAssetHash_.clear();
    incomingAssetSourcePeerToken_.clear();
    appendLog(timeoutMilliseconds > 0
        ? QStringLiteral(
            "looper asset request received no transfer start within %1 ms: "
            "hash=%2 source=%3")
            .arg(timeoutMilliseconds)
            .arg(expiredHash, expiredSource.left(8))
        : QStringLiteral(
            "looper asset request explicitly expired after no transfer start: "
            "hash=%1 source=%2")
            .arg(expiredHash, expiredSource.left(8)));
    retryOrFailIncomingAsset(expiredHash, expiredSource);
}

void MainWindow::retryOrFailIncomingAsset(
    const QString& hash,
    const QString& failedSourcePeerToken)
{
    if (!isSha256Hex(hash)) return;
    bool stillExpected = pendingLooperAssetHashes_.contains(hash);
    if (!stillExpected) {
        for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
            if (contribution.assetHash == hash) {
                stillExpected = true;
                break;
            }
        }
    }
    if (!stillExpected) {
        trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
        trackWorkspace_.incomingAssetRetrySources.remove(hash);
        return;
    }
    if (!trackWorkspace_.incomingAssetRetrySources.contains(hash) ||
        trackWorkspace_.incomingAssetRetrySources.value(hash) != failedSourcePeerToken) {
        trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
        trackWorkspace_.incomingAssetRetrySources.insert(hash, failedSourcePeerToken);
    }
    const int attempt = trackWorkspace_.incomingAssetRetryAttempts.value(hash) + 1;
    trackWorkspace_.incomingAssetRetryAttempts[hash] = attempt;
    if (attempt <= 3) {
        const int delayMs = 250 * attempt;
        appendLog(QStringLiteral(
            "retrying looper asset %1 after interruption: source=%2 attempt=%3")
            .arg(hash.left(8), failedSourcePeerToken.left(8))
            .arg(attempt));
        QTimer::singleShot(delayMs, this, [this, hash] {
            if (validatedTrackAssetHashes_.contains(hash) ||
                incomingAssetWorkflow_ != IncomingAssetWorkflow::None) return;
            requestNextPendingAsset();
        });
        return;
    }

    trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
    trackWorkspace_.incomingAssetRetrySources.remove(hash);
    QString failedSource;
    QString failedBatch;
    for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
        if (contribution.assetHash == hash &&
            contribution.sourcePeerToken == failedSourcePeerToken) {
            failedSource = contribution.sourcePeerToken;
            failedBatch = contribution.batchId;
            break;
        }
    }
    if (failedBatch.isEmpty()) {
        for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
            if (contribution.assetHash == hash) {
                failedSource = contribution.sourcePeerToken;
                failedBatch = contribution.batchId;
                break;
            }
        }
    }
    if (!failedBatch.isEmpty()) {
        appendLog(QStringLiteral("abandoned Track Sync batch %1 after repeated asset failures")
            .arg(failedBatch.left(8)));
        expirePendingTrackBatch(failedSource, failedBatch);
        return;
    }
    if (pendingLooperAssetHashes_.contains(hash)) {
        const bool publishCurrent = pendingSongNeedsAuthoritativePublish_ &&
            sessionController_.isServer();
        pendingSongSet_ = {};
        pendingSongRevision_ = 0;
        trackWorkspace_.pendingSongBaseRevision = 0;
        pendingSongTrackRestart_ = false;
        pendingSongSourcePeerToken_.clear();
        pendingSongNeedsAuthoritativePublish_ = false;
        pendingLooperAssetHashes_.clear();
        if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
        appendLog(QStringLiteral("abandoned pending arrangement after repeated WAV transfer failures"));
        if (publishCurrent) sendSongSnapshot();
    }
    requestNextPendingAsset();
}

void MainWindow::expirePendingTrackBatch(
    const QString& sourcePeerToken,
    const QString& batchId)
{
    trackWorkspace_.incomingTrackShareLastProgressMs.remove(
        sourcePeerToken + QLatin1Char(':') + batchId);
    QList<jam2::gui::track_asset_ownership::Claim> claims;
    claims.reserve(pendingTrackContributions_.size());
    for (auto it = pendingTrackContributions_.cbegin();
         it != pendingTrackContributions_.cend(); ++it) {
        claims.append({it.key(), it->sourcePeerToken, it->batchId, it->assetHash});
    }
    QSet<QString> pendingArrangementHashes;
    for (const QString& hash : pendingLooperAssetHashes_) {
        pendingArrangementHashes.insert(hash);
    }
    const QString activeHash = incomingAssetHash_;
    const auto plan = jam2::gui::track_asset_ownership::planBatchExpiry(
        claims,
        sourcePeerToken,
        batchId,
        incomingAssetWorkflow_ == IncomingAssetWorkflow::TrackContribution,
        activeHash,
        incomingAssetSourcePeerToken_,
        pendingArrangementHashes);
    if (!plan.found()) return;
    for (const QString& key : plan.removedKeys) pendingTrackContributions_.remove(key);

    if (plan.activeSourceDetached) {
        // Retry limits belong to the interrupted source attempt. A surviving
        // contribution with the same bytes but a different source gets its own
        // bounded retry opportunity.
        trackWorkspace_.incomingAssetRetryAttempts.remove(activeHash);
        trackWorkspace_.incomingAssetRetrySources.remove(activeHash);
        assetTransfer_.resetIncoming();
        // A request can be awaiting looper.asset.start without an active service
        // transfer. In that state resetIncoming() has no hash with which to notify
        // the workspace, so clear the matching expectation explicitly.
        if (incomingAssetWorkflow_ == IncomingAssetWorkflow::TrackContribution &&
            incomingAssetHash_ == activeHash &&
            incomingAssetSourcePeerToken_ == sourcePeerToken) {
            incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
            incomingAssetHash_.clear();
            incomingAssetSourcePeerToken_.clear();
        }
    }

    for (const QString& hash : plan.removedHashes) {
        const QString remainingTrackSource = plan.remainingTrackSources.value(hash);
        if (remainingTrackSource.isEmpty()) {
            pendingTrackAssetSources_.remove(hash);
        } else {
            pendingTrackAssetSources_.insert(hash, remainingTrackSource);
        }
        const bool retryOwnedByExpiredSource =
            trackWorkspace_.incomingAssetRetrySources.contains(hash) &&
            trackWorkspace_.incomingAssetRetrySources.value(hash) == sourcePeerToken;
        if (!plan.stillExpectedHashes.contains(hash) ||
            (retryOwnedByExpiredSource && remainingTrackSource != sourcePeerToken)) {
            trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
            trackWorkspace_.incomingAssetRetrySources.remove(hash);
        }
    }
    if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
    appendLog(QStringLiteral("expired incomplete Track Sync batch %1 from peer %2")
        .arg(batchId.left(8), sourcePeerToken.left(8)));
    applyPendingTrackContributions();
    requestNextPendingAsset();
}

void MainWindow::applyPendingTrackContributions()
{
    if (automationOfferPauseActive_) return;
    if (laneRecordingIsolationActive()) return;
    if (pendingTrackContributions_.isEmpty()) {
        return;
    }
    struct BatchProgress {
        QString source;
        QString batch;
        int expected = 0;
        int received = 0;
        int ready = 0;
    };
    QMap<QString, BatchProgress> batches;
    for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
        const QString key = contribution.sourcePeerToken + QLatin1Char(':') + contribution.batchId;
        BatchProgress& batch = batches[key];
        batch.source = contribution.sourcePeerToken;
        batch.batch = contribution.batchId;
        batch.expected = contribution.batchSize;
        ++batch.received;
        if (validatedTrackAssetHashes_.contains(contribution.assetHash)) {
            ++batch.ready;
        }
    }
    BatchProgress selected;
    bool haveReadyBatch = false;
    for (const BatchProgress& batch : std::as_const(batches)) {
        if (batch.expected > 0 && batch.received == batch.expected &&
            batch.ready == batch.expected) {
            selected = batch;
            haveReadyBatch = true;
            break;
        }
    }
    const BatchProgress display = haveReadyBatch ? selected : batches.constBegin().value();
    if (performanceHome_) {
        performanceHome_->setTrackTransferStatus(
            QStringLiteral("RECEIVING TRACKS\u2026  %1 / %2")
                .arg(display.ready).arg(display.expected));
    }
    if (!haveReadyBatch) {
        requestNextPendingAsset();
        return;
    }
    const QString batchId = selected.batch;
    const QString sourcePeerToken = selected.source;
    const int expected = selected.expected;
    LooperProject stagedProject = looperProject_;
    QStringList completedIds;
    bool arrangementChanged = false;
    for (auto it = pendingTrackContributions_.constBegin();
         it != pendingTrackContributions_.constEnd(); ++it) {
        const PendingTrackContribution& contribution = it.value();
        if (contribution.batchId != batchId ||
            contribution.sourcePeerToken != sourcePeerToken) {
            continue;
        }
        if (!validatedTrackAssetHashes_.contains(contribution.assetHash) ||
            contribution.bankIndex < 0 || contribution.bankIndex >= stagedProject.banks().size()) {
            continue;
        }
        QString assetPath = looperAssetPathForHash(contribution.assetHash);
        for (const LooperBank& bank : stagedProject.banks()) {
            for (const LooperLane& candidate : bank.lanes) {
                if (candidate.assetHash == contribution.assetHash &&
                    QFileInfo::exists(looperAssetAbsolutePath(candidate))) {
                    assetPath = candidate.assetPath;
                    break;
                }
            }
        }
        auto& lanes = stagedProject.banks()[contribution.bankIndex].lanes;
        int targetIndex = -1;
        for (int index = 0; index < lanes.size(); ++index) {
            if (lanes.at(index).id == contribution.targetLaneId) {
                targetIndex = index;
                break;
            }
        }
        int existingContributionIndex = -1;
        for (int index = 0; index < lanes.size(); ++index) {
            if (lanes.at(index).id == contribution.contributionId) {
                existingContributionIndex = index;
                break;
            }
        }
        const bool targetReservedForLocalRecording =
            trackRecordingWorkflow_.laneArmedAt(contribution.bankIndex, targetIndex);
        bool applied = false;
        int matchingHashIndex = -1;
        for (int index = 0; index < lanes.size(); ++index) {
            if (lanes.at(index).assetHash == contribution.assetHash) {
                matchingHashIndex = index;
                break;
            }
        }
        if (matchingHashIndex >= 0) {
            LooperLane& lane = lanes[matchingHashIndex];
            if (lane.assetPath.trimmed().isEmpty() ||
                !QFileInfo::exists(looperAssetAbsolutePath(lane))) {
                lane.assetPath = assetPath;
                lane.sampleRate = contribution.sampleRate;
                lane.sampleRateCompatible = true;
                lane.sourceFrames = contribution.sourceFrames;
                lane.originKind = QStringLiteral("peer");
                arrangementChanged = true;
            }
            if (lane.localOnly) {
                lane.localOnly = false;
                arrangementChanged = true;
            }
            appendLog(QStringLiteral(
                "reused matching local track for peer contribution: hash=%1 bank=%2")
                .arg(contribution.assetHash)
                .arg(contribution.bankIndex + 1));
            applied = true;
        } else if ((targetIndex >= 0 && lanes.at(targetIndex).assetHash == contribution.assetHash) ||
            (existingContributionIndex >= 0 &&
             lanes.at(existingContributionIndex).assetHash == contribution.assetHash)) {
            applied = true;
        } else if (targetIndex >= 0 && !targetReservedForLocalRecording &&
                   lanes.at(targetIndex).assetHash.isEmpty() && lanes.at(targetIndex).assetPath.isEmpty()) {
            LooperLane& lane = lanes[targetIndex];
            lane.assetPath = assetPath;
            lane.assetHash = contribution.assetHash;
            lane.sampleRate = contribution.sampleRate;
            lane.sampleRateCompatible = true;
            lane.sourceFrames = contribution.sourceFrames;
            lane.localOnly = false;
            lane.originKind = QStringLiteral("peer");
            if (!contribution.name.trimmed().isEmpty() &&
                (lane.name.trimmed().isEmpty() || isDefaultEmptyTrackName(lane.name))) {
                lane.name = contribution.name;
            }
            lane.startFrame = contribution.startFrame;
            lane.stopFrame = contribution.stopFrame;
            lane.loopStartFrame = contribution.loopStartFrame;
            lane.loopEndFrame = contribution.loopEndFrame;
            lane.loopEnabled = contribution.loopEnabled;
            arrangementChanged = true;
            applied = true;
        } else {
            LooperLane lane;
            lane.id = targetIndex < 0
                ? contribution.targetLaneId : contribution.contributionId;
            lane.assetPath = assetPath;
            lane.assetHash = contribution.assetHash;
            lane.name = contribution.name.trimmed().isEmpty()
                ? QStringLiteral("Peer track") : contribution.name;
            lane.sampleRate = contribution.sampleRate;
            lane.sampleRateCompatible = true;
            lane.sourceFrames = contribution.sourceFrames;
            lane.localOnly = false;
            lane.originKind = QStringLiteral("peer");
            lane.startFrame = contribution.startFrame;
            lane.stopFrame = contribution.stopFrame;
            lane.loopStartFrame = contribution.loopStartFrame;
            lane.loopEndFrame = contribution.loopEndFrame;
            lane.loopEnabled = contribution.loopEnabled;
            if (stagedProject.appendLane(contribution.bankIndex, std::move(lane))) {
                arrangementChanged = true;
                applied = true;
            } else {
                appendLog(QStringLiteral("could not append received peer track to bank %1")
                    .arg(contribution.bankIndex + 1));
            }
        }
        if (applied) {
            completedIds.append(it.key());
            appendLog(QStringLiteral("merged peer track into Track Sync: %1 hash=%2")
                .arg(contribution.name, contribution.assetHash));
        }
    }
    if (completedIds.size() != expected) {
        appendLog(QStringLiteral(
            "Track Sync batch %1 could not be applied atomically; arrangement was unchanged")
            .arg(batchId.left(8)));
        if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
        return;
    }
    if (arrangementChanged) {
        looperProject_ = std::move(stagedProject);
    }
    for (const QString& contributionId : completedIds) {
        pendingTrackContributions_.remove(contributionId);
        appliedTrackContributionIds_.insert(contributionId);
    }
    trackWorkspace_.incomingTrackShareLastProgressMs.remove(
        sourcePeerToken + QLatin1Char(':') + batchId);
    while (appliedTrackContributionIds_.size() > kMaxLooperTrackContributions * 2) {
        appliedTrackContributionIds_.erase(appliedTrackContributionIds_.begin());
    }
    if (arrangementChanged) {
        refreshLooperLanes();
        regeneratePreparedMix();
        if (sessionController_.isServer()) {
            sendSongSnapshot();
        }
    }
    if (sessionController_.isServer() && !automaticWavSharingEnabled()) {
        // A manual peer batch can make WAV bytes newly available at the mesh
        // creator without changing lane metadata: the hash-addressed received
        // path was already installed by the earlier arrangement snapshot.
        // Fan out after every accepted manual batch so the other mesh peers do
        // not depend on whether that local file arrival also changed the model.
        appendLog(QStringLiteral(
            "republishing accepted manual Track Sync batch across the full mesh"));
        QTimer::singleShot(0, this, [this] { publishLocalTrackBatch({}); });
    }
    sendControlTo(sourcePeerToken, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("looper.track.batch.complete")},
        {QStringLiteral("batch_id"), batchId},
        {QStringLiteral("tracks"), expected},
    });
    if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
    requestNextPendingAsset();
    // One asset completion or offer can make several concurrent batches ready,
    // particularly when those batches reuse hashes that are already local.
    // Drain another ready batch on the next event-loop turn instead of leaving
    // it without a future completion callback to trigger reconciliation.
    if (!pendingTrackContributions_.isEmpty()) {
        QTimer::singleShot(0, this, [this] { applyPendingTrackContributions(); });
    }
}

bool MainWindow::canQueueAssetTo(
    const QString& targetPeerToken,
    qint64 estimatedBytes) const
{
    return sessionController_.canQueueAssetTo(targetPeerToken, estimatedBytes);
}

bool MainWindow::sendAssetControlTo(
    const QString& targetPeerToken,
    const QJsonObject& message)
{
    return sessionController_.sendAssetTo(targetPeerToken, message);
}

bool MainWindow::sendAssetBinaryTo(
    const QString& targetPeerToken,
    const QByteArray& payload)
{
    return sessionController_.sendAssetBinaryTo(targetPeerToken, payload);
}

bool MainWindow::sendControlTo(const QString& targetPeerToken, const QJsonObject& message)
{
    if (!jamSyncAllowsControlMessage(message)) {
        appendLog(QStringLiteral("suppressed targeted control message by the jam sync policy: %1")
            .arg(message.value(QStringLiteral("type")).toString()));
        return false;
    }
    return sessionController_.sendTo(targetPeerToken, message);
}

void MainWindow::handleSongSet(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const QString syncScope = message.value(QStringLiteral("sync_scope")).toString(
        QStringLiteral("tracks"));
    QJsonObject policyProbe{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("sync_scope"), syncScope},
    };
    if (!jamSyncAllowsControlMessage(policyProbe)) {
        appendLog(QStringLiteral("ignored %1 snapshot because it is disabled by the jam sync policy")
            .arg(syncScope));
        return;
    }
    const SongSyncScope songScope = syncScope == QStringLiteral("idea.full")
        ? SongSyncScope::IdeaFull : syncScope == QStringLiteral("idea.chords")
            ? SongSyncScope::IdeaChords : syncScope == QStringLiteral("idea.beats")
                ? SongSyncScope::IdeaBeats : SongSyncScope::Tracks;
    const bool hostAuthoritative =
        message.value(QStringLiteral("host_authoritative")).toBool(false);
    const bool fromPeerProposal = sessionController_.isServer() &&
        !sourcePeerToken.isEmpty() && !hostAuthoritative;
    const quint64 proposalBaseRevision = fromPeerProposal
        ? static_cast<quint64>(message.value(
            QStringLiteral("base_arrangement_revision")).toInteger(-1))
        : 0;
    if (sessionController_.isServer() && !sourcePeerToken.isEmpty() &&
        hostAuthoritative) {
        appendLog(QStringLiteral("rejected peer attempt to mark an arrangement proposal authoritative"));
        return;
    }
    if (!hostAuthoritative && !fromPeerProposal) {
        appendLog(QStringLiteral("rejected arrangement outside the collaborative proposal or creator snapshot path"));
        return;
    }
    if (songScope == SongSyncScope::Tracks && hostAuthoritative &&
        !trackWorkspace_.outgoingTrackSharePendingPeers.isEmpty()) {
        const int incomingRevision = message.value(
            QStringLiteral("arrangement_revision")).toInt();
        const int heldRevision = trackWorkspace_.heldTrackShareSongSet.value(
            QStringLiteral("arrangement_revision")).toInt(-1);
        if (incomingRevision > heldRevision) {
            trackWorkspace_.heldTrackShareSongSet = message;
            trackWorkspace_.heldTrackShareSongSourcePeerToken = sourcePeerToken;
        }
        appendLog(QStringLiteral(
            "held arrangement revision %1 until %2 outgoing Track Sync batch(es) complete")
            .arg(incomingRevision)
            .arg(trackWorkspace_.outgoingTrackSharePendingPeers.size()));
        return;
    }
    if (fromPeerProposal) {
        const quint64 currentRevision = sessionController_.snapshot().arrangementRevision;
        if (proposalBaseRevision != currentRevision) {
            if (songScope == SongSyncScope::Tracks &&
                trackWorkspace_.authoritativeTrackHistory.contains(proposalBaseRevision)) {
                int mergedChanges = 0;
                int mergeConflicts = 0;
                QJsonObject rebased = message;
                rebased.insert(QStringLiteral("song"), mergeConcurrentLooperMetadata(
                    trackWorkspace_.authoritativeTrackHistory.value(proposalBaseRevision),
                    songToJson(true),
                    message.value(QStringLiteral("song")).toObject(),
                    &mergedChanges,
                    &mergeConflicts));
                rebased.insert(QStringLiteral("base_arrangement_revision"),
                    static_cast<qint64>(currentRevision));
                appendLog(QStringLiteral(
                    "rebased stale track metadata from peer %1: base=%2 current=%3 "
                    "merged_changes=%4 conflicts_kept_local=%5")
                    .arg(sourcePeerToken.left(8))
                    .arg(proposalBaseRevision)
                    .arg(currentRevision)
                    .arg(mergedChanges)
                    .arg(mergeConflicts));
                QTimer::singleShot(0, this,
                    [this, rebased, sourcePeerToken] {
                        handleSongSet(rebased, sourcePeerToken);
                    });
                return;
            }
            appendLog(QStringLiteral(
                "rejected stale collaborative arrangement from peer %1: base=%2 current=%3")
                .arg(sourcePeerToken.left(8))
                .arg(proposalBaseRevision)
                .arg(currentRevision));
            sendSongSnapshot(std::nullopt, songScope);
            return;
        }
    }
    if (sessionController_.isServer() && hostAuthoritative) {
        appendLog(QStringLiteral("ignored host-authoritative song snapshot while hosting"));
        return;
    }
    const int revision = message.value(QStringLiteral("arrangement_revision")).toInt(
        message.value(QStringLiteral("revision")).toInt(0));
    const bool restartTrack = jamSyncPolicy_.globalPlayback && (fromPeerProposal
        ? trackRecordingWorkflow_.globalTransportRequestedPlaying()
        : message.value(QStringLiteral("track_playing")).toBool(false));
    if (hostAuthoritative &&
        revision <= lastAppliedHostArrangementRevision_) {
        appendLog(QStringLiteral("ignored stale host arrangement revision %1").arg(revision));
        return;
    }
    if (songScope == SongSyncScope::Tracks) {
        // Control messages from one peer are ordered. A track arrangement
        // accepted after that peer's batch offer supersedes the still-pending
        // batch snapshot and must release any asset request it made obsolete.
        supersedePendingTrackBatches(sourcePeerToken);
    }
    QMap<QString, QString> localAssetCandidates;
    for (const LooperBank& bank : looperProject_.banks()) {
        for (const LooperLane& lane : bank.lanes) {
            const QString hash = lane.assetHash.toLower();
            const QString path = looperAssetAbsolutePath(lane);
            if (isSha256Hex(hash) && QFileInfo::exists(path) &&
                !localAssetCandidates.contains(hash)) {
                localAssetCandidates.insert(hash, path);
            }
        }
    }
    QJsonObject normalizedIncomingSong =
        normalizeLooperAssetPaths(message.value(QStringLiteral("song")).toObject());
    const QString pendingRecordingId = pendingRecordedLaneSyncId_;
    const QString pendingRecordingHash = pendingRecordedLaneSyncHash_;
    bool authoritativeContainsPendingRecording = false;
    if (hostAuthoritative && !pendingRecordingId.isEmpty()) {
        const QJsonArray pendingBanks = normalizedIncomingSong
            .value(QStringLiteral("looper")).toObject()
            .value(QStringLiteral("banks")).toArray();
        for (const QJsonValue& bankValue : pendingBanks) {
            for (const QJsonValue& laneValue : bankValue.toObject()
                     .value(QStringLiteral("lanes")).toArray()) {
                const QJsonObject lane = laneValue.toObject();
                const bool matches =
                    lane.value(QStringLiteral("id")).toString() ==
                        pendingRecordingId &&
                    (pendingRecordingHash.isEmpty() ||
                        lane.value(QStringLiteral("asset_hash")).toString() ==
                            pendingRecordingHash);
                if (matches) {
                    authoritativeContainsPendingRecording = true;
                    break;
                }
            }
            if (authoritativeContainsPendingRecording) break;
        }
    }
    if (hostAuthoritative && !pendingRecordingId.isEmpty() &&
        !authoritativeContainsPendingRecording) {
        const QJsonObject localLooper = looperProject_.toJson(true);
        const QJsonArray localBanks = localLooper.value(
            QStringLiteral("banks")).toArray();
        QString localBankId;
        QJsonObject localRecordedLane;
        for (const QJsonValue& bankValue : localBanks) {
            const QJsonObject bank = bankValue.toObject();
            for (const QJsonValue& laneValue : bank.value(
                     QStringLiteral("lanes")).toArray()) {
                const QJsonObject lane = laneValue.toObject();
                if (lane.value(QStringLiteral("id")).toString() == pendingRecordingId &&
                    lane.value(QStringLiteral("asset_hash")).toString() ==
                        pendingRecordingHash) {
                    localBankId = bank.value(QStringLiteral("id")).toString();
                    localRecordedLane = lane;
                    break;
                }
            }
            if (!localRecordedLane.isEmpty()) break;
        }
        if (!localRecordedLane.isEmpty()) {
            QJsonObject incomingLooper = normalizedIncomingSong.value(
                QStringLiteral("looper")).toObject();
            QJsonArray incomingBanks = incomingLooper.value(
                QStringLiteral("banks")).toArray();
            bool preserved = false;
            for (int bankIndex = 0; bankIndex < incomingBanks.size(); ++bankIndex) {
                QJsonObject bank = incomingBanks.at(bankIndex).toObject();
                if (bank.value(QStringLiteral("id")).toString() != localBankId) continue;
                QJsonArray lanes = bank.value(QStringLiteral("lanes")).toArray();
                for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
                    if (lanes.at(laneIndex).toObject().value(
                            QStringLiteral("id")).toString() != pendingRecordingId) {
                        continue;
                    }
                    lanes.replace(laneIndex, localRecordedLane);
                    preserved = true;
                    break;
                }
                if (!preserved) {
                    lanes.append(localRecordedLane);
                    preserved = true;
                }
                bank.insert(QStringLiteral("lanes"), lanes);
                incomingBanks.replace(bankIndex, bank);
                break;
            }
            if (preserved) {
                incomingLooper.insert(QStringLiteral("banks"), incomingBanks);
                normalizedIncomingSong.insert(
                    QStringLiteral("looper"), incomingLooper);
                appendLog(QStringLiteral(
                    "preserved pending recorded lane %1 while awaiting authoritative hash %2")
                    .arg(pendingRecordingId, pendingRecordingHash.left(12)));
            }
        }
    }
    QJsonObject normalizedSong = songToJson(true);
    if (songScope == SongSyncScope::Tracks) {
        normalizedSong.insert(
            QStringLiteral("looper"),
            normalizedIncomingSong.value(QStringLiteral("looper")));
        if (normalizedIncomingSong.contains(QStringLiteral("title"))) {
            normalizedSong.insert(
                QStringLiteral("title"),
                normalizedIncomingSong.value(QStringLiteral("title")));
        }
        QJsonArray localSections = normalizedSong.value(QStringLiteral("sections")).toArray();
        const QJsonArray incomingSections = normalizedIncomingSong
            .value(QStringLiteral("sections")).toArray();
        for (int section = 0; section < qMin(localSections.size(), incomingSections.size()); ++section) {
            QJsonObject localSection = localSections.at(section).toObject();
            const QJsonObject incomingSection = incomingSections.at(section).toObject();
            for (const QString& field : {
                     QStringLiteral("label"), QStringLiteral("name"), QStringLiteral("id"),
                     QStringLiteral("beats")}) {
                if (incomingSection.contains(field)) {
                    localSection.insert(field, incomingSection.value(field));
                }
            }
            const int structuralBeats = localSection.value(
                QStringLiteral("beats")).toInt(8);
            for (const QString& field : {
                     QStringLiteral("chords"), QStringLiteral("targets"),
                     QStringLiteral("beat_notes"), QStringLiteral("lyrics"),
                     QStringLiteral("beat_patterns"), QStringLiteral("musical_patterns")}) {
                QJsonArray values = localSection.value(field).toArray();
                while (values.size() > structuralBeats) values.removeLast();
                if (localSection.contains(field)) localSection.insert(field, values);
            }
            localSections.replace(section, localSection);
        }
        normalizedSong.insert(QStringLiteral("sections"), localSections);
    } else {
        if (songScope == SongSyncScope::IdeaFull) {
            const QJsonValue localLooper = normalizedSong.value(QStringLiteral("looper"));
            normalizedSong = normalizedIncomingSong;
            normalizedSong.insert(QStringLiteral("looper"), localLooper);
        } else {
            QJsonArray localSections = normalizedSong.value(QStringLiteral("sections")).toArray();
            const QJsonArray incomingSections = normalizedIncomingSong
                .value(QStringLiteral("sections")).toArray();
            const int sectionCount = qMin(localSections.size(), incomingSections.size());
            const QStringList sharedFields = songScope == SongSyncScope::IdeaChords
                ? QStringList{
                    QStringLiteral("label"), QStringLiteral("name"), QStringLiteral("id"),
                    QStringLiteral("beats"), QStringLiteral("chords"),
                    QStringLiteral("targets"), QStringLiteral("musical_patterns"),
                    QStringLiteral("generated_kind"), QStringLiteral("generated_recipe")}
                : QStringList{
                    QStringLiteral("label"), QStringLiteral("name"), QStringLiteral("id"),
                    QStringLiteral("beats"), QStringLiteral("beat_notes"),
                    QStringLiteral("beat_patterns"), QStringLiteral("drum_kit"),
                    QStringLiteral("generated_kind"), QStringLiteral("generated_recipe")};
            for (int section = 0; section < sectionCount; ++section) {
                QJsonObject localSection = localSections.at(section).toObject();
                const QJsonObject incomingSection = incomingSections.at(section).toObject();
                for (const QString& field : sharedFields) {
                    if (incomingSection.contains(field)) {
                        localSection.insert(field, incomingSection.value(field));
                    } else {
                        localSection.remove(field);
                    }
                }
                const int beats = localSection.value(QStringLiteral("beats")).toInt(8);
                for (const QString& field : {
                         QStringLiteral("chords"), QStringLiteral("targets"),
                         QStringLiteral("beat_notes"), QStringLiteral("lyrics"),
                         QStringLiteral("beat_patterns"), QStringLiteral("musical_patterns")}) {
                    QJsonArray values = localSection.value(field).toArray();
                    while (values.size() > beats) values.removeLast();
                    if (localSection.contains(field)) localSection.insert(field, values);
                }
                localSections.replace(section, localSection);
            }
            normalizedSong.insert(QStringLiteral("sections"), localSections);
            if (songScope == SongSyncScope::IdeaChords &&
                normalizedIncomingSong.contains(QStringLiteral("title"))) {
                normalizedSong.insert(
                    QStringLiteral("title"),
                    normalizedIncomingSong.value(QStringLiteral("title")));
            }
        }
        QJsonObject localLooper = normalizedSong.value(QStringLiteral("looper")).toObject();
        QJsonArray localBanks = localLooper.value(QStringLiteral("banks")).toArray();
        const QJsonArray incomingBanks = normalizedIncomingSong.value(QStringLiteral("looper"))
            .toObject().value(QStringLiteral("banks")).toArray();
        for (int bank = 0; bank < qMin(localBanks.size(), incomingBanks.size()); ++bank) {
            QJsonObject localBank = localBanks.at(bank).toObject();
            localBank.insert(
                QStringLiteral("timing"),
                incomingBanks.at(bank).toObject().value(QStringLiteral("timing")));
            localBanks.replace(bank, localBank);
        }
        localLooper.insert(QStringLiteral("banks"), localBanks);
        normalizedSong.insert(QStringLiteral("looper"), localLooper);
    }
    QStringList referencedHashes;
    const QJsonArray banks = normalizedSong.value(QStringLiteral("looper"))
        .toObject().value(QStringLiteral("banks")).toArray();
    for (const QJsonValue& bankValue : banks) {
        for (const QJsonValue& laneValue : bankValue.toObject().value(QStringLiteral("lanes")).toArray()) {
            const QString hash = laneValue.toObject().value(QStringLiteral("asset_hash")).toString().toLower();
            if (!hash.isEmpty() && !referencedHashes.contains(hash)) {
                referencedHashes.append(hash);
            }
        }
    }
    if (referencedHashes.size() > kMaxLooperAssetRequests) {
        appendLog(QStringLiteral("rejected arrangement with %1 unique assets; maximum=%2")
            .arg(referencedHashes.size())
            .arg(kMaxLooperAssetRequests));
        return;
    }
    const QJsonObject song = preserveLocalOnlyLanes(normalizedSong);
    const QString assetFolder = QDir(projectPersistence_.workspaceFolder()).absoluteFilePath(QStringLiteral("wavs"));
    const std::uint64_t checkRevision = ++songAssetCheckRevision_;
    auto missing = std::make_shared<QStringList>();
    auto resolvedPaths = std::make_shared<QMap<QString, QString>>();
    auto assetFailure = std::make_shared<QString>();
    const int expectedSampleRate = sessionController_.snapshot().contract.sampleRate;
    const bool started = startFileWorkerTask(
        [referencedHashes, assetFolder, localAssetCandidates, expectedSampleRate,
         missing, resolvedPaths, assetFailure] {
            for (const QString& hash : referencedHashes) {
                if (!isSha256Hex(hash)) {
                    missing->append(hash);
                    continue;
                }
                const QString canonicalPath =
                    QDir(assetFolder).absoluteFilePath(hash + QStringLiteral(".wav"));
                QStringList candidates{canonicalPath};
                const QString localPath = localAssetCandidates.value(hash);
                if (!localPath.isEmpty() && localPath != canonicalPath) {
                    candidates.append(localPath);
                }
                bool found = false;
                for (const QString& path : candidates) {
                    if (!QFileInfo::exists(path)) continue;
                    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                        nativeFilePath(path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                    if (!inspected || sha256FileHex(path) != hash) continue;
                    if (expectedSampleRate > 0 &&
                        inspected.info.sample_rate !=
                            static_cast<std::uint32_t>(expectedSampleRate)) {
                        *assetFailure = QStringLiteral(
                            "WAV asset %1 has sample rate %2 Hz; expected %3 Hz")
                            .arg(hash.left(12))
                            .arg(inspected.info.sample_rate)
                            .arg(expectedSampleRate);
                        return;
                    }
                    resolvedPaths->insert(hash, path);
                    found = true;
                    break;
                }
                if (!found) {
                    missing->append(hash);
                }
            }
        },
        [this, message, song, revision, restartTrack, hostAuthoritative, fromPeerProposal,
         proposalBaseRevision, songScope,
         sourcePeerToken, checkRevision, missing, resolvedPaths, assetFailure,
         pendingRecordingId, authoritativeContainsPendingRecording] {
            if (checkRevision != songAssetCheckRevision_) {
                return;
            }
            if (deferIncomingControlForLaneRecording(
                    message, sourcePeerToken)) {
                return;
            }
            if (!assetFailure->isEmpty()) {
                appendLog(QStringLiteral("rejected arrangement asset: ") + *assetFailure);
                return;
            }
            if (fromPeerProposal && proposalBaseRevision !=
                sessionController_.snapshot().arrangementRevision) {
                appendLog(QStringLiteral(
                    "discarded collaborative arrangement superseded during asset validation: base=%1 current=%2")
                    .arg(proposalBaseRevision)
                    .arg(sessionController_.snapshot().arrangementRevision));
                sendSongSnapshot(std::nullopt, songScope);
                return;
            }
            QJsonObject resolvedSong = song;
            QJsonObject resolvedLooper = resolvedSong.value(QStringLiteral("looper")).toObject();
            QJsonArray resolvedBanks = resolvedLooper.value(QStringLiteral("banks")).toArray();
            for (int bankIndex = 0; bankIndex < resolvedBanks.size(); ++bankIndex) {
                QJsonObject bank = resolvedBanks.at(bankIndex).toObject();
                QJsonArray lanes = bank.value(QStringLiteral("lanes")).toArray();
                for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
                    QJsonObject lane = lanes.at(laneIndex).toObject();
                    const QString hash =
                        lane.value(QStringLiteral("asset_hash")).toString().toLower();
                    if (resolvedPaths->contains(hash)) {
                        lane.insert(QStringLiteral("asset_path"), resolvedPaths->value(hash));
                    } else if (missing->contains(hash) && !automaticWavSharingEnabled()) {
                        lane.insert(QStringLiteral("asset_path"), QString{});
                    }
                    lanes.replace(laneIndex, lane);
                }
                bank.insert(QStringLiteral("lanes"), lanes);
                resolvedBanks.replace(bankIndex, bank);
            }
            resolvedLooper.insert(QStringLiteral("banks"), resolvedBanks);
            resolvedSong.insert(QStringLiteral("looper"), resolvedLooper);
            for (auto it = resolvedPaths->cbegin(); it != resolvedPaths->cend(); ++it) {
                validatedTrackAssetHashes_.insert(it.key());
                if (it.value() != looperAssetPathForHash(it.key())) {
                    appendLog(QStringLiteral(
                        "reused existing local looper asset: hash=%1 path=%2")
                        .arg(it.key(), it.value()));
                }
            }
            if (incomingAssetWorkflow_ == IncomingAssetWorkflow::Arrangement &&
                (incomingAssetSourcePeerToken_ != sourcePeerToken ||
                 !missing->contains(incomingAssetHash_))) {
                const QString supersededHash = incomingAssetHash_;
                ++trackWorkspace_.incomingAssetRequestGeneration;
                // The request may still be waiting for looper.asset.start, in
                // which case AssetTransferService has no active hash to clear.
                // Remove the obsolete arrangement expectation before resetting
                // an active transfer so its abandonment callback cannot retry a
                // hash that the newer arrangement no longer references.
                pendingLooperAssetHashes_.clear();
                assetTransfer_.resetIncoming();
                if (incomingAssetWorkflow_ == IncomingAssetWorkflow::Arrangement &&
                    incomingAssetHash_ == supersededHash) {
                    incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
                    incomingAssetHash_.clear();
                    incomingAssetSourcePeerToken_.clear();
                }
                bool expectedByTrackContribution = false;
                for (const PendingTrackContribution& contribution :
                     pendingTrackContributions_) {
                    if (contribution.assetHash == supersededHash) {
                        expectedByTrackContribution = true;
                        break;
                    }
                }
                if (!expectedByTrackContribution) {
                    trackWorkspace_.incomingAssetRetryAttempts.remove(supersededHash);
                    trackWorkspace_.incomingAssetRetrySources.remove(supersededHash);
                }
                appendLog(QStringLiteral(
                    "cancelled superseded arrangement WAV request: hash=%1")
                    .arg(supersededHash.left(12)));
            }
            if (!missing->isEmpty() && automaticWavSharingEnabled()) {
                pendingSongSet_ = resolvedSong;
                pendingSongRevision_ = revision;
                trackWorkspace_.pendingSongBaseRevision = proposalBaseRevision;
                pendingSongTrackRestart_ = restartTrack;
                pendingSongSourcePeerToken_ = sourcePeerToken;
                pendingSongNeedsAuthoritativePublish_ = fromPeerProposal;
                pendingLooperAssetHashes_ = *missing;
                trackController_.waitForAssets(
                    static_cast<quint64>(revision), restartTrack);
                updateTrackPlaybackPresentation();
                appendLog(QStringLiteral("requesting %1 looper asset(s) for arrangement revision %2")
                    .arg(missing->size())
                    .arg(revision));
                requestNextPendingAsset();
                return;
            }
            if (!missing->isEmpty()) {
                appendLog(QStringLiteral(
                    "applied shared lane metadata with %1 WAV asset(s) left local; automatic WAV sharing is off")
                    .arg(missing->size()));
            }
            projectPersistence_.useWorkspaceAsProjectFolderIfUnset();
            pendingSongSet_ = QJsonObject{};
            pendingSongRevision_ = 0;
            trackWorkspace_.pendingSongBaseRevision = 0;
            pendingSongTrackRestart_ = false;
            pendingSongSourcePeerToken_.clear();
            pendingSongNeedsAuthoritativePublish_ = false;
            pendingLooperAssetHashes_.clear();
            if (loadSongJson(resolvedSong)) {
                if (hostAuthoritative) {
                    lastAppliedHostArrangementRevision_ = revision;
                    if (!pendingRecordingId.isEmpty() &&
                        pendingRecordedLaneSyncId_ == pendingRecordingId) {
                        if (authoritativeContainsPendingRecording) {
                            pendingRecordedLaneSyncId_.clear();
                            pendingRecordedLaneSyncHash_.clear();
                            pendingRecordedLaneSyncAttempts_ = 0;
                        } else if (pendingRecordedLaneSyncAttempts_ < 4) {
                            const int retry = ++pendingRecordedLaneSyncAttempts_;
                            QTimer::singleShot(100 * retry, this, [this] {
                                if (!pendingRecordedLaneSyncId_.isEmpty() &&
                                    !laneRecordingIsolationActive()) {
                                    syncLooperArrangement();
                                }
                            });
                        }
                    }
                }
                refreshSongViews();
                refreshLooperLanes();
                preparedMixLifecycle_.setPlayWhenReady(restartTrack);
                trackController_.prepareMix(
                    static_cast<quint64>(revision), restartTrack);
                updateTrackPlaybackPresentation();
                regeneratePreparedMix();
                if (fromPeerProposal) {
                    appendLog(QStringLiteral("accepted collaborative arrangement edit from peer %1")
                        .arg(sourcePeerToken.left(8)));
                    sendSongSnapshot(restartTrack, songScope);
                }
                // Arrangement validation may have made every asset in an
                // earlier Track Sync offer ready without an asset-transfer
                // completion callback. Revisit that batch after the validated
                // arrangement is installed so it can complete atomically.
                applyPendingTrackContributions();
            }
        },
        [this, message, sourcePeerToken](const QString&) {
            deferredSongSetMessage_ = message;
            deferredSongSetSourcePeerToken_ = sourcePeerToken;
            if (!songAssetCheckRetryTimer_.isActive()) {
                songAssetCheckRetryTimer_.start(100);
            }
        });
    if (!started) {
        deferredSongSetMessage_ = message;
        deferredSongSetSourcePeerToken_ = sourcePeerToken;
        if (!songAssetCheckRetryTimer_.isActive()) {
            songAssetCheckRetryTimer_.start(100);
        }
        appendLog(QStringLiteral("arrangement asset validation deferred by saturated file worker"));
    }
}

void MainWindow::applyPendingSongIfAssetsReady()
{
    if (laneRecordingIsolationActive()) return;
    if (pendingSongSet_.isEmpty()) {
        return;
    }
    if (!pendingLooperAssetHashes_.isEmpty()) {
        return;
    }
    const QJsonObject song = pendingSongSet_;
    const int revision = pendingSongRevision_;
    const quint64 baseRevision = trackWorkspace_.pendingSongBaseRevision;
    const bool restartTrack = pendingSongTrackRestart_;
    const QString sourcePeerToken = pendingSongSourcePeerToken_;
    const bool publishAuthoritative = pendingSongNeedsAuthoritativePublish_;
    pendingSongSet_ = QJsonObject{};
    pendingSongRevision_ = 0;
    trackWorkspace_.pendingSongBaseRevision = 0;
    pendingSongTrackRestart_ = false;
    pendingSongSourcePeerToken_.clear();
    pendingSongNeedsAuthoritativePublish_ = false;
    projectPersistence_.useWorkspaceAsProjectFolderIfUnset();
    if (publishAuthoritative && baseRevision !=
        sessionController_.snapshot().arrangementRevision) {
        appendLog(QStringLiteral(
            "discarded collaborative arrangement superseded during asset transfer: base=%1 current=%2")
            .arg(baseRevision)
            .arg(sessionController_.snapshot().arrangementRevision));
        sendSongSnapshot();
        return;
    }
    if (loadSongJson(song)) {
        if (!publishAuthoritative) {
            lastAppliedHostArrangementRevision_ = qMax(
                lastAppliedHostArrangementRevision_, revision);
        }
        refreshSongViews();
        refreshLooperLanes();
        preparedMixLifecycle_.setPlayWhenReady(restartTrack);
        trackController_.prepareMix(static_cast<quint64>(revision), restartTrack);
        updateTrackPlaybackPresentation();
        regeneratePreparedMix();
        if (publishAuthoritative) {
            appendLog(QStringLiteral("accepted collaborative arrangement edit from peer %1 after asset sync")
                .arg(sourcePeerToken.left(8)));
            sendSongSnapshot(restartTrack);
        }
        appendLog(QStringLiteral("applied pending looper arrangement after asset sync"));
        applyPendingTrackContributions();
    }
}

QJsonObject MainWindow::normalizeLooperAssetPaths(QJsonObject song) const
{
    QJsonObject looper = song.value(QStringLiteral("looper")).toObject();
    QJsonArray banks = looper.value(QStringLiteral("banks")).toArray();
    for (int bankIndex = 0; bankIndex < banks.size(); ++bankIndex) {
        QJsonObject bank = banks.at(bankIndex).toObject();
        QJsonArray lanes = bank.value(QStringLiteral("lanes")).toArray();
        for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
            QJsonObject lane = lanes.at(laneIndex).toObject();
            const QString hash = lane.value(QStringLiteral("asset_hash")).toString();
            lane.remove(QStringLiteral("asset_path"));
            if (isSha256Hex(hash)) {
                lane.insert(
                    QStringLiteral("asset_path"),
                    QDir(projectPersistence_.workspaceFolder()).absoluteFilePath(
                        QStringLiteral("received/") + hash + QStringLiteral(".wav")));
            }

            lanes.replace(laneIndex, lane);
        }
        bank.insert(QStringLiteral("lanes"), lanes);
        banks.replace(bankIndex, bank);
    }
    looper.insert(QStringLiteral("banks"), banks);
    song.insert(QStringLiteral("looper"), looper);
    return song;
}

QJsonObject MainWindow::preserveLocalOnlyLanes(QJsonObject song)
{
    const int merged = mergeSynchronizedLooperLanes(song, looperProject_);
    const int localOnly = mergeLocalOnlyLooperLanes(song, looperProject_);
    const int preserved = merged + localOnly;
    if (preserved > 0) {
        appendLog(QStringLiteral(
            "preserved %1 local-only WAV lane(s) during arrangement merge")
            .arg(preserved));
    }
    return song;
}

QString MainWindow::looperAssetPathForHash(const QString& hash) const
{
    if (!isSha256Hex(hash)) {
        return {};
    }
    return QDir(projectPersistence_.workspaceFolder()).absoluteFilePath(
        QStringLiteral("received/") + hash + QStringLiteral(".wav"));
}

int MainWindow::activeTrackSampleRate() const
{
    const SharedSessionController::Snapshot session =
        sessionController_.snapshot();
    const jam2::EngineSnapshot engine = jam2_.engineSnapshot();
    return jam2::gui::resolve_active_sample_rate(
        session.contract.sampleRate,
        engine.sample_rate,
        sessionRuntimeDraft_.configuration.sampleRate);
}

bool MainWindow::recordingTargetSampleRate(
    int& sampleRate,
    QString& error) const
{
    const SharedSessionController::Snapshot session =
        sessionController_.snapshot();
    const jam2::EngineSnapshot engine = jam2_.engineSnapshot();
    sampleRate = jam2::gui::resolve_active_sample_rate(
        session.contract.sampleRate,
        engine.sample_rate,
        sessionRuntimeDraft_.configuration.sampleRate);
    if (session.contract.sampleRate > 0 &&
        !jam2::gui::sample_rate_matches_engine(
            session.contract.sampleRate, engine.sample_rate)) {
        error = engine.sample_rate > 0.0
            ? QStringLiteral(
                "The session requires %1 Hz but the active engine is %2 Hz. "
                "Recording was not started.")
                .arg(session.contract.sampleRate)
                .arg(engine.sample_rate, 0, 'f', 0)
            : QStringLiteral(
                "The %1 Hz session engine is not active yet. Recording was not started.")
                .arg(session.contract.sampleRate);
        return false;
    }
    if (sampleRate <= 0) {
        error = QStringLiteral(
            "No valid recording sample rate is available.");
        return false;
    }
    return true;
}

void MainWindow::retirePluginHost(
    std::unique_ptr<jam2::application::InputPluginHost> host)
{
    if (!host) return;
    // Never wait for a third-party process from the GUI thread. The shared
    // shutdown flag is enough for the worker to leave its loop; retained
    // ownership also keeps any bridge observed by the audio callback valid.
    host->requestRetire();
    retiredPluginHosts_.push_back(std::move(host));
}

void MainWindow::removeAudioPlugin(std::size_t slot)
{
    if (slot >= audioPluginSources_.size()) return;
    auto& source = audioPluginSources_[slot];
    if (auto* router = jam2_.inputSourceRouter()) {
        jam2::audio::InputSourceConfiguration configuration;
        configuration.kind = jam2::audio::InputSourceKind::Audio;
        configuration.first_channel = source.firstChannel;
        configuration.second_channel = source.secondChannel;
        configuration.level_ppm = source.levelPpm;
        configuration.enabled = source.included &&
            source.firstChannel != jam2::audio::kNoInputChannel;
        (void)router->configure(slot, configuration);
    }
    retirePluginHost(std::move(source.host));
    source.name.clear();
    source.bypassed = false;
    updateInputSourceButtons();
}

bool MainWindow::selectAndStartPluginAsync(
    std::size_t slot,
    jam2::audio::InputSourceKind kind,
    jam2::midi::EventQueue* midiQueue,
    PluginStartCallback completion,
    PluginLoadProgressCallback progress)
{
    if (!jam2_.isRunning()) {
        const QString error = QStringLiteral(
            "Start the local audio engine before loading an input plugin.");
        if (progress) progress(0, error);
        else QMessageBox::information(this, QStringLiteral("Input plugin"), error);
        return false;
    }
    Jam2RuntimeOptions options;
    try {
        options = runtimeOptions();
    } catch (const std::exception& error) {
        const QString message = QStringLiteral("The current audio configuration is invalid: %1")
            .arg(QString::fromUtf8(error.what()));
        if (progress) progress(0, message);
        else QMessageBox::warning(this, QStringLiteral("Input plugin"), message);
        return false;
    }
    const std::size_t blockFrames = static_cast<std::size_t>(
        options.audio_buffer_size > 0 ? options.audio_buffer_size : 256);
    const std::size_t sourceChannels = kind == jam2::audio::InputSourceKind::MidiInstrument
        ? 0U
        : (slot < audioPluginSources_.size() &&
           audioPluginSources_[slot].secondChannel != jam2::audio::kNoInputChannel ? 2U : 1U);
    jam2::application::InputPluginLoadRequest request;
    request.kind = kind;
    request.midiQueue = midiQueue;
    request.sampleRate = static_cast<double>(options.sample_rate);
    request.maximumFrames = blockFrames;
    request.sourceInputChannels = sourceChannels;
    auto observedCompletion = [this, completion = std::move(completion)](
        std::unique_ptr<jam2::application::InputPluginHost> host,
        QString name) mutable {
        ++automationInputPluginLoadCompletions_;
        if (completion) completion(std::move(host), std::move(name));
    };
    return inputPluginBackend_->selectAndStart(
        *this,
        fileWorkerPool_,
        thread(),
        request,
        std::move(observedCompletion),
        std::move(progress));
}

void MainWindow::refreshInputSourceRouting()
{
    auto* router = jam2_.inputSourceRouter();
    if (!router || router == attachedInputRouter_) return;
    attachedInputRouter_ = router;
    for (std::size_t slot = 0; slot < audioPluginSources_.size(); ++slot) {
        auto& source = audioPluginSources_[slot];
        if (slot < router->physical_channels() &&
            source.firstChannel == jam2::audio::kNoInputChannel &&
            !source.consumedByStereoGroup) {
            source.firstChannel = slot;
        }
        if (source.firstChannel == jam2::audio::kNoInputChannel) {
            router->clear(slot);
            continue;
        }
        jam2::audio::InputSourceConfiguration configuration;
        configuration.kind = jam2::audio::InputSourceKind::Audio;
        configuration.first_channel = source.firstChannel;
        configuration.second_channel = source.secondChannel;
        configuration.renderer = source.host ? source.host->renderer() : nullptr;
        configuration.level_ppm = source.levelPpm;
        configuration.enabled = source.included;
        (void)router->configure(slot, configuration);
    }
    for (auto& source : midiPluginSources_) {
        if (!source || source->routerSlot >= jam2::audio::kMaximumInputSources) continue;
        if (!source->host || !source->host->renderer()) {
            router->clear(source->routerSlot);
            continue;
        }
        jam2::audio::InputSourceConfiguration configuration;
        configuration.kind = jam2::audio::InputSourceKind::MidiInstrument;
        configuration.renderer = source->host ? source->host->renderer() : nullptr;
        configuration.level_ppm = source->levelPpm;
        configuration.enabled = source->included;
        if (source->host) source->host->setMidiMuted(source->muted);
        (void)router->configure(source->routerSlot, configuration);
    }
    router->set_recording_source(recordingInputSourceSlot_.value_or(
        jam2::audio::kCombinedInputSources));
    updateInputSourceButtons();
}

void MainWindow::updateInputSourceButtons()
{
    int plugins = 0;
    bool allBypassed = true;
    for (const auto& source : audioPluginSources_) {
        if (!source.host) continue;
        ++plugins;
        allBypassed = allBypassed && source.bypassed;
    }
    for (const auto& source : midiPluginSources_) {
        if (!source || !source->host) continue;
        ++plugins;
        allBypassed = allBypassed && source->muted;
    }
    if (performanceAudioInputsButton_) {
        const auto* router = jam2_.inputSourceRouter();
        const int count = router ? static_cast<int>(router->physical_channels()) : 0;
        performanceAudioInputsButton_->setText(QStringLiteral("AUDIO"));
        performanceAudioInputsButton_->setToolTip(QStringLiteral("Configure %1 selected audio input%2")
            .arg(count).arg(count == 1 ? QString{} : QStringLiteral("s")));
    }
    if (performanceMidiInputsButton_) {
        performanceMidiInputsButton_->setText(QStringLiteral("MIDI"));
        performanceMidiInputsButton_->setToolTip(QStringLiteral("Configure %1 MIDI device%2")
            .arg(midiPluginSources_.size())
            .arg(midiPluginSources_.size() == 1 ? QString{} : QStringLiteral("s")));
    }
    if (performancePluginsButton_) {
        performancePluginsButton_->setText(QStringLiteral("PLUGINS"));
        performancePluginsButton_->setToolTip(plugins > 0
            ? QStringLiteral("Manage %1 loaded input plugin%2")
                .arg(plugins).arg(plugins == 1 ? QString{} : QStringLiteral("s"))
            : QStringLiteral("Add plugins to audio and MIDI inputs"));
    }
    if (performancePluginBypassButton_) {
        QSignalBlocker blocker(performancePluginBypassButton_);
        performancePluginBypassButton_->setEnabled(plugins > 0);
        performancePluginBypassButton_->setChecked(plugins > 0 && allBypassed);
        performancePluginBypassButton_->setText(QStringLiteral("BYPASS"));
        performancePluginBypassButton_->setToolTip(plugins > 0
            ? QStringLiteral("Bypass every input plugin; audio inputs continue dry")
            : QStringLiteral("No input plugins are loaded"));
    }
}

void MainWindow::showAudioInputSources()
{
    refreshInputSourceRouting();
    if (!jam2_.inputSourceRouter()) {
        QMessageBox::information(this, QStringLiteral("Audio inputs"),
            QStringLiteral("Start the local audio engine to view its selected input channels."));
        return;
    }

    jam2::gui::AudioInputDialogCallbacks callbacks;
    callbacks.snapshot = [this] {
        jam2::gui::AudioInputDialogState state;
        state.topologyLocked = trackRecordingWorkflow_.inputTakeActive() ||
            loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();

        const auto options = runtimeOptions();
        const auto inputName = [&options](std::size_t channel) {
            const int number = channel < options.channel_selection.input.size()
                ? options.channel_selection.input[channel] + 1
                : static_cast<int>(channel + 1);
            return QStringLiteral("Input %1").arg(number);
        };

        const auto* current = jam2_.inputSourceRouter();
        if (!current) return state;
        for (std::size_t slot = 0; slot < current->physical_channels() &&
             slot < audioPluginSources_.size(); ++slot) {
            const auto& source = audioPluginSources_[slot];
            if (source.firstChannel == jam2::audio::kNoInputChannel) continue;

            jam2::gui::AudioInputDialogSource item;
            item.slot = slot;
            item.firstChannel = source.firstChannel;
            item.secondChannel = source.secondChannel;
            item.firstName = inputName(source.firstChannel);
            item.stereo =
                source.secondChannel != jam2::audio::kNoInputChannel;
            if (item.stereo) item.secondName = inputName(source.secondChannel);
            item.included = source.included;
            item.levelPpm = source.levelPpm;
            item.pluginLoaded = source.host != nullptr;
            state.sources.push_back(std::move(item));
        }
        return state;
    };
    callbacks.setIncluded = [this](std::size_t slot, bool included) {
        if (slot >= audioPluginSources_.size()) return;
        audioPluginSources_[slot].included = included;
        if (auto* current = jam2_.inputSourceRouter())
            (void)current->set_enabled(slot, included);
    };
    callbacks.setLevel = [this](std::size_t slot, int levelPpm) {
        if (slot >= audioPluginSources_.size()) return;
        audioPluginSources_[slot].levelPpm = levelPpm;
        if (auto* current = jam2_.inputSourceRouter())
            (void)current->set_level(slot, levelPpm);
    };
    callbacks.ungroup = [this](std::size_t slot) {
        if (slot >= audioPluginSources_.size()) return;
        auto& grouped = audioPluginSources_[slot];
        if (grouped.secondChannel == jam2::audio::kNoInputChannel) return;
        removeAudioPlugin(slot);
        const std::size_t restored = grouped.secondChannel;
        grouped.secondChannel = jam2::audio::kNoInputChannel;
        if (restored < audioPluginSources_.size()) {
            audioPluginSources_[restored].firstChannel = restored;
            audioPluginSources_[restored].consumedByStereoGroup = false;
        }
        attachedInputRouter_ = nullptr;
        refreshInputSourceRouting();
    };
    callbacks.group = [this](std::size_t left, std::size_t right) {
        if (left == right || left >= audioPluginSources_.size() ||
            right >= audioPluginSources_.size()) return;
        const auto isAvailableMono = [this](std::size_t slot) {
            const auto& source = audioPluginSources_[slot];
            return source.firstChannel == slot &&
                source.secondChannel == jam2::audio::kNoInputChannel &&
                !source.consumedByStereoGroup;
        };
        if (!isAvailableMono(left) || !isAvailableMono(right)) return;

        removeAudioPlugin(left);
        removeAudioPlugin(right);
        audioPluginSources_[left].firstChannel = left;
        audioPluginSources_[left].secondChannel = right;
        audioPluginSources_[left].consumedByStereoGroup = false;
        audioPluginSources_[right].firstChannel =
            jam2::audio::kNoInputChannel;
        audioPluginSources_[right].secondChannel =
            jam2::audio::kNoInputChannel;
        audioPluginSources_[right].consumedByStereoGroup = true;
        attachedInputRouter_ = nullptr;
        refreshInputSourceRouting();
    };

    jam2::gui::AudioInputSourcesDialog::run(std::move(callbacks), this);
    updateInputSourceButtons();
}

void MainWindow::showMidiInputSources()
{
    refreshInputSourceRouting();
    if (!jam2_.inputSourceRouter()) {
        QMessageBox::information(this, QStringLiteral("MIDI inputs"),
            QStringLiteral("Start the local audio engine before assigning a MIDI device."));
        return;
    }

    jam2::gui::MidiInputDialogCallbacks callbacks;
    callbacks.snapshot = [this] {
        jam2::gui::MidiInputDialogState state;
        state.topologyLocked = trackRecordingWorkflow_.inputTakeActive() ||
            loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();
        for (const auto& source : midiPluginSources_) {
            if (!source) continue;
            jam2::gui::MidiInputDialogSource item;
            item.routerSlot = source->routerSlot;
            item.deviceId = QString::fromStdString(source->deviceInfo.id);
            item.deviceName = QString::fromStdString(source->deviceInfo.name);
            item.mode = source->mode;
            item.included = source->included;
            item.levelPpm = source->levelPpm;
            item.deviceOpen = source->device != nullptr;
            item.pluginLoaded = source->host != nullptr;
            item.pluginName = source->pluginName;
            state.sources.push_back(std::move(item));
        }
        return state;
    };
    callbacks.setMode = [this](
        std::size_t slot, jam2::midi::InputMode mode) {
        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found == midiPluginSources_.end()) return;
        (*found)->mode = mode;
        if ((*found)->host) (*found)->host->requestMidiReset();
    };
    callbacks.setIncluded = [this](std::size_t slot, bool included) {
        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found == midiPluginSources_.end()) return;
        (*found)->included = included;
        if (auto* current = jam2_.inputSourceRouter())
            (void)current->set_enabled(slot, included);
    };
    callbacks.setLevel = [this](std::size_t slot, int levelPpm) {
        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found == midiPluginSources_.end()) return;
        (*found)->levelPpm = levelPpm;
        if (auto* current = jam2_.inputSourceRouter())
            (void)current->set_level(slot, levelPpm);
    };
    callbacks.remove = [this](std::size_t slot) {
        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found == midiPluginSources_.end()) return;
        const auto source = *found;
        if (auto* current = jam2_.inputSourceRouter()) current->clear(slot);
        if (source->host) {
            source->host->requestMidiReset();
            source->host->setMidiQueue(nullptr);
        }
        source->device.reset();
        if (source->host) source->host->requestRetire();
        retiredMidiSources_.push_back(source);
        midiPluginSources_.erase(found);
        attachedInputRouter_ = nullptr;
        refreshInputSourceRouting();
    };
    callbacks.discover = [this](
        jam2::gui::MidiInputDialogCallbacks::DiscoveryCompletion completion) {
        struct MidiDiscoveryOutcome {
            std::vector<jam2::midi::DeviceInfo> devices;
            QString error;
        };
        const auto outcome = std::make_shared<MidiDiscoveryOutcome>();
        const auto midiBackend = midiInputBackend_;
        QPointer<MainWindow> self(this);
        fileWorkerPool_.start(QRunnable::create([
            self, outcome, midiBackend, completion = std::move(completion)
        ]() mutable {
            try {
                outcome->devices = midiBackend->enumerate();
            } catch (const std::exception& error) {
                outcome->error = QString::fromUtf8(error.what());
            } catch (...) {
                outcome->error =
                    QStringLiteral("Unknown MIDI device discovery failure.");
            }
            if (self.isNull()) return;
            QMetaObject::invokeMethod(self, [
                self, outcome, completion = std::move(completion)
            ]() mutable {
                if (self.isNull() || !completion) return;
                ++self->automationMidiDiscoveryCompletions_;
                jam2::gui::MidiInputDiscoveryResult result;
                result.error = outcome->error;
                result.devices.reserve(
                    static_cast<qsizetype>(outcome->devices.size()));
                for (const auto& device : outcome->devices) {
                    result.devices.push_back({
                        QString::fromStdString(device.id),
                        QString::fromStdString(device.name),
                    });
                }
                completion(std::move(result));
            }, Qt::QueuedConnection);
        }));
    };
    callbacks.assign = [this](
        const jam2::gui::MidiInputDeviceChoice& device,
        jam2::midi::InputMode mode) {
        auto* currentRouter = jam2_.inputSourceRouter();
        if (!currentRouter)
            return jam2::gui::MidiInputAssignmentError::EngineStopped;

        std::array<bool, jam2::audio::kMaximumInputSources> used{};
        for (std::size_t audio = 0;
             audio < currentRouter->physical_channels() && audio < used.size();
             ++audio) {
            used[audio] = true;
        }
        for (const auto& existing : midiPluginSources_) {
            if (existing && existing->routerSlot < used.size())
                used[existing->routerSlot] = true;
        }
        std::size_t slot = currentRouter->physical_channels();
        while (slot < used.size() && used[slot]) ++slot;
        if (slot >= jam2::audio::kMaximumInputSources)
            return jam2::gui::MidiInputAssignmentError::SourceLimit;

        auto source = std::make_shared<MidiPluginSource>();
        source->deviceInfo = {
            device.id.toStdString(), device.name.toStdString()};
        source->mode = mode;
        source->routerSlot = slot;
        midiPluginSources_.push_back(std::move(source));
        attachedInputRouter_ = nullptr;
        refreshInputSourceRouting();
        return jam2::gui::MidiInputAssignmentError::None;
    };

    jam2::gui::MidiInputSourcesDialog::run(std::move(callbacks), this);
    updateInputSourceButtons();
}

void MainWindow::showInputPlugins()
{
    refreshInputSourceRouting();
    auto* router = jam2_.inputSourceRouter();
    if (!router) {
        QMessageBox::information(this, QStringLiteral("Input plugins"),
            QStringLiteral("Start the local audio engine before loading an input plugin."));
        return;
    }

    jam2::gui::InputPluginDialogCallbacks callbacks;
    callbacks.snapshot = [this] {
        jam2::gui::InputPluginDialogState state;
        state.topologyLocked = trackRecordingWorkflow_.inputTakeActive() ||
            loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();

        const auto toDialogStats = [](
            const jam2::application::InputPluginStats& source) {
            jam2::gui::InputPluginDialogStats result;
            result.deadlineMisses = source.deadlineMisses;
            result.deadlineConcealments = source.deadlineConcealments;
            result.workerLatencyFrames = source.workerLatencyFrames;
            result.negotiatedInputChannels = source.negotiatedInputChannels;
            result.negotiatedOutputChannels = source.negotiatedOutputChannels;
            result.isolationLatencyFrames = source.isolationLatencyFrames;
            result.workerProcessAverageUs = source.workerProcessAverageUs;
            result.workerProcessMaxUs = source.workerProcessMaxUs;
            result.midiDropped = source.midiDropped;
            result.midiQueueDepth = source.midiQueueDepth;
            result.midiQueueHighWater = source.midiQueueHighWater;
            return result;
        };
        const auto options = runtimeOptions();
        const auto inputName = [&options](std::size_t channel) {
            const int number = channel < options.channel_selection.input.size()
                ? options.channel_selection.input[channel] + 1
                : static_cast<int>(channel + 1);
            return QStringLiteral("Input %1").arg(number);
        };

        const auto* current = jam2_.inputSourceRouter();
        if (current) {
            for (std::size_t slot = 0;
                 slot < current->physical_channels() &&
                 slot < audioPluginSources_.size(); ++slot) {
                const auto& source = audioPluginSources_[slot];
                if (source.firstChannel == jam2::audio::kNoInputChannel) continue;
                jam2::gui::InputPluginDialogSource item;
                item.kind = jam2::gui::InputPluginDialogKind::Audio;
                item.slot = slot;
                item.sourceName =
                    source.secondChannel == jam2::audio::kNoInputChannel
                    ? inputName(source.firstChannel)
                    : QStringLiteral("%1 left + %2 right")
                        .arg(inputName(source.firstChannel),
                            inputName(source.secondChannel));
                item.loaded = source.host != nullptr;
                item.pluginName = source.name;
                item.healthy = source.host && source.host->healthy();
                item.bypassed = source.bypassed;
                if (source.host) item.stats = toDialogStats(source.host->stats());
                state.audioSources.push_back(std::move(item));
            }
        }
        for (const auto& source : midiPluginSources_) {
            if (!source) continue;
            jam2::gui::InputPluginDialogSource item;
            item.kind = jam2::gui::InputPluginDialogKind::MidiInstrument;
            item.slot = source->routerSlot;
            item.sourceName = QString::fromStdString(source->deviceInfo.name);
            item.loaded = source->host != nullptr;
            item.pluginName = source->pluginName;
            item.healthy = source->host && source->host->healthy();
            item.bypassed = source->muted;
            if (source->host) item.stats = toDialogStats(source->host->stats());
            state.midiSources.push_back(std::move(item));
        }
        return state;
    };
    callbacks.open = [this](
        jam2::gui::InputPluginDialogKind kind, std::size_t slot) {
        if (kind == jam2::gui::InputPluginDialogKind::Audio) {
            if (slot < audioPluginSources_.size() &&
                audioPluginSources_[slot].host) {
                audioPluginSources_[slot].host->openEditor();
            }
            return;
        }
        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found != midiPluginSources_.end() && (*found)->host)
            (*found)->host->openEditor();
    };
    callbacks.setBypassed = [this](
        jam2::gui::InputPluginDialogKind kind,
        std::size_t slot,
        bool bypassed) {
        if (kind == jam2::gui::InputPluginDialogKind::Audio) {
            if (slot >= audioPluginSources_.size()) return;
            auto& source = audioPluginSources_[slot];
            source.bypassed = bypassed;
            if (source.host) source.host->setAudioBypassed(bypassed);
        } else {
            const auto found = std::find_if(
                midiPluginSources_.begin(), midiPluginSources_.end(),
                [slot](const auto& source) {
                    return source && source->routerSlot == slot;
                });
            if (found == midiPluginSources_.end()) return;
            auto& source = *found;
            source->muted = bypassed;
            if (source->host) {
                if (bypassed) source->host->requestMidiReset();
                source->host->setMidiMuted(bypassed);
            }
        }
        updateInputSourceButtons();
    };
    callbacks.remove = [this](
        jam2::gui::InputPluginDialogKind kind, std::size_t slot) {
        if (kind == jam2::gui::InputPluginDialogKind::Audio) {
            removeAudioPlugin(slot);
            return;
        }
        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found == midiPluginSources_.end()) return;
        auto& source = *found;
        if (source->host) {
            source->host->requestMidiReset();
            source->host->setMidiQueue(nullptr);
        }
        retirePluginHost(std::move(source->host));
        source->device.reset();
        source->pluginName.clear();
        source->muted = false;
        if (auto* current = jam2_.inputSourceRouter())
            current->clear(source->routerSlot);
        attachedInputRouter_ = nullptr;
        refreshInputSourceRouting();
    };
    callbacks.load = [this](
        jam2::gui::InputPluginDialogKind kind,
        std::size_t slot,
        jam2::gui::InputPluginDialogCallbacks::LoadProgress progress,
        jam2::gui::InputPluginDialogCallbacks::LoadFinished finished) {
        if (kind == jam2::gui::InputPluginDialogKind::Audio) {
            if (slot >= audioPluginSources_.size()) {
                progress(0, QStringLiteral("The audio input source is no longer available."));
                finished();
                return false;
            }
            const std::size_t expectedFirst =
                audioPluginSources_[slot].firstChannel;
            const std::size_t expectedSecond =
                audioPluginSources_[slot].secondChannel;
            return selectAndStartPluginAsync(slot,
                jam2::audio::InputSourceKind::Audio, nullptr,
                [this, slot, expectedFirst, expectedSecond, progress, finished](
                    std::unique_ptr<jam2::application::InputPluginHost> host,
                    const QString& name) mutable {
                    if (trackRecordingWorkflow_.inputTakeActive() ||
                        loopbackRecorder_.isRunning() ||
                        trackRecordingWorkflow_.laneArmed()) {
                        if (host) host->requestRetire();
                        progress(0, QStringLiteral(
                            "The plugin loaded after recording was armed, so it was not attached."));
                        finished();
                        return;
                    }
                    auto& current = audioPluginSources_[slot];
                    if (current.firstChannel != expectedFirst ||
                        current.secondChannel != expectedSecond) {
                        if (host) host->requestRetire();
                        progress(0, QStringLiteral(
                            "The input grouping changed while the plugin loaded, so it was not attached."));
                        finished();
                        return;
                    }
                    retirePluginHost(std::move(current.host));
                    current.host = std::move(host);
                    current.name = name;
                    current.bypassed = false;
                    attachedInputRouter_ = nullptr;
                    refreshInputSourceRouting();
                    finished();
                }, progress);
        }

        const auto found = std::find_if(
            midiPluginSources_.begin(), midiPluginSources_.end(),
            [slot](const auto& source) {
                return source && source->routerSlot == slot;
            });
        if (found == midiPluginSources_.end()) {
            progress(0, QStringLiteral("The MIDI input source is no longer available."));
            finished();
            return false;
        }
        const auto source = *found;
        return selectAndStartPluginAsync(source->routerSlot,
            jam2::audio::InputSourceKind::MidiInstrument, &source->events,
            [this, source, progress, finished](
                std::unique_ptr<jam2::application::InputPluginHost> host,
                const QString& name) mutable {
                if (trackRecordingWorkflow_.inputTakeActive() ||
                    loopbackRecorder_.isRunning() ||
                    trackRecordingWorkflow_.laneArmed()) {
                    if (host) host->requestRetire();
                    progress(0, QStringLiteral(
                        "The instrument loaded after recording was armed, so it was not attached."));
                    finished();
                    return;
                }

                const auto attach = [this, source, progress, finished](
                    std::unique_ptr<jam2::application::InputPluginHost> readyHost,
                    const QString& readyName) {
                    const auto active = std::find(
                        midiPluginSources_.begin(), midiPluginSources_.end(), source);
                    if (active == midiPluginSources_.end()) {
                        if (readyHost) readyHost->requestRetire();
                        source->device.reset();
                        progress(0, QStringLiteral(
                            "The MIDI device was removed before its instrument finished loading."));
                        finished();
                        return;
                    }
                    if (source->host) {
                        source->host->requestMidiReset();
                        source->host->setMidiQueue(nullptr);
                    }
                    retirePluginHost(std::move(source->host));
                    source->host = std::move(readyHost);
                    source->pluginName = readyName;
                    source->muted = false;
                    attachedInputRouter_ = nullptr;
                    refreshInputSourceRouting();
                    progress(100, QStringLiteral(
                        "%1 loaded. You can open its interface now.").arg(readyName));
                    finished();
                };

                if (source->device) {
                    attach(std::move(host), name);
                    return;
                }
                progress(-1, QStringLiteral("Opening MIDI input device…"));
                struct MidiOpenOutcome {
                    std::unique_ptr<jam2::application::InputPluginHost> host;
                    std::unique_ptr<jam2::midi::InputDevice> device;
                    QString name;
                    std::string error;
                };
                const auto opened = std::make_shared<MidiOpenOutcome>();
                opened->host = std::move(host);
                opened->name = name;
                QPointer<MainWindow> self(this);
                const auto midiBackend = midiInputBackend_;
                fileWorkerPool_.start(QRunnable::create([
                    self, source, opened, attach, progress, finished,
                    midiBackend
                ]() mutable {
                    opened->device = midiBackend->open(
                        source->deviceInfo.id, source->events, opened->error);
                    if (self.isNull()) return;
                    QMetaObject::invokeMethod(self, [
                        self, source, opened, attach, progress, finished
                    ]() mutable {
                        if (self.isNull()) return;
                        if (!opened->device) {
                            if (opened->host) opened->host->requestRetire();
                            progress(0, QStringLiteral(
                                "Could not open the MIDI device: %1")
                                .arg(QString::fromStdString(opened->error)));
                            finished();
                            return;
                        }
                        source->device = std::move(opened->device);
                        attach(std::move(opened->host), opened->name);
                    }, Qt::QueuedConnection);
                }));
            }, progress);
    };

    jam2::gui::InputPluginsDialog::run(std::move(callbacks), this);
    updateInputSourceButtons();
}

Jam2RuntimeOptions MainWindow::runtimeOptions() const
{
    const CreatePreference& configuration = sessionRuntimeDraft_.configuration;
    const LocalTuningPreference& tuning = configuration.tuning;
    const RuntimePreference& runtime = configuration.runtime;
    Jam2RuntimeOptions options;
    options.bind = jam2::parse_bind_endpoint(meshBindEndpoint().toStdString());
    if (!configuration.stunServer.trimmed().isEmpty()) {
        options.stun_server = jam2::parse_endpoint(
            configuration.stunServer.trimmed().toStdString());
    }
    options.no_stun = configuration.noStun;
    options.stun_timeout_ms = configuration.stunTimeoutMs;
    options.stun_retries = configuration.stunRetries;
    options.wait_ms = runtime.waitMs;
    options.stream_ms = runtime.streamMs;
    options.stream_linger_ms = runtime.streamLingerMs;
    options.stats_enabled = runtime.diagnostics;
    // The GUI receives compact diagnostics through the in-process callback.
    // Logged GUI jams retain a hidden two-second CSV timeline for later analysis.
    options.stats_interval_ms = 0;
    options.stats_warmup_ms = runtime.diagnosticsWarmupMs;
    if (options.stats_enabled && !runtime.logStatsFolder.trimmed().isEmpty()) {
        options.log_stats_dir = nativeFilePath(runtime.logStatsFolder.trimmed());
        options.stats_interval_ms = 2000;
    }
    if (configuration.socketSendBuffer > 0) {
        options.socket_send_buffer = configuration.socketSendBuffer;
    }
    if (configuration.socketRecvBuffer > 0) {
        options.socket_recv_buffer = configuration.socketRecvBuffer;
    }
    options.sample_rate = configuration.sampleRate;
    options.frame_size = tuning.frameSize;
    const auto format = jam2::protocol::parse_audio_format(
        configuration.audioFormat.toStdString());
    if (!format) {
        throw std::runtime_error("select a supported network audio quality");
    }
    options.network_audio_format = *format;
    options.drift_correction = tuning.driftCorrection;
    options.drift_smoothing = tuning.driftSmoothing;
    options.drift_deadband_ppm = tuning.driftDeadbandPpm;
    options.drift_max_correction_ppm = tuning.driftMaxCorrectionPpm;
    options.metronome = metronomeTransport_.localRunning();
    options.metronome_transport_gated = true;
    options.bpm = metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value();
    options.metronome_level = gainFromDb(static_cast<double>(
        metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10));
    options.metronome_sound = jam2::metronome::sanitize_click_sound(
        metronomeSoundBox_ ? metronomeSoundBox_->currentData().toInt() : 0);
    const QString metronomeMode = metronomeModeBox_->currentText();
    options.metronome_mode = metronomeMode == QStringLiteral("leader-audio")
        ? Jam2MetronomeMode::LeaderAudio
        : metronomeMode == QStringLiteral("listener-compensated")
            ? Jam2MetronomeMode::ListenerCompensated
            : Jam2MetronomeMode::SharedGrid;
    options.metronome_compensation_max_ms =
        metronomeCompensationMaxSpin_ ? metronomeCompensationMaxSpin_->value() : 250.0;
    options.metronome_compensation_smoothing_ms =
        metronomeCompensationSmoothingSpin_ ? metronomeCompensationSmoothingSpin_->value() : 750.0;
    options.metronome_compensation_deadband_ms =
        metronomeCompensationDeadbandSpin_ ? metronomeCompensationDeadbandSpin_->value() : 1.0;
    options.metronome_compensation_slew_ms_per_sec =
        metronomeCompensationSlewSpin_ ? metronomeCompensationSlewSpin_->value() : 40.0;
    options.remote_level = gainFromDb(static_cast<double>(
        remoteLevelSlider_ ? remoteLevelSlider_->value() : 0));
    options.send_level = gainFromDb(static_cast<double>(
        mixSendLevelSlider_ ? mixSendLevelSlider_->value() : 0));
    options.output_level = gainFromDb(static_cast<double>(
        masterOutputLevelSlider_ ? masterOutputLevelSlider_->value() : 0));
    options.local_monitor = mixMonitorCheck_ && mixMonitorCheck_->isChecked();
    options.local_monitor_level = gainFromDb(static_cast<double>(
        mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : 0));
    options.sample_time_playout = tuning.sampleTimePlayout;
    options.playout_delay_frames =
        static_cast<std::size_t>(tuning.playoutDelayFrames);
    options.jitter_buffer_frames =
        static_cast<std::size_t>(tuning.jitterBufferFrames);
    options.jitter_buffer_max_frames =
        static_cast<std::size_t>(tuning.jitterBufferMaxFrames);
    options.adaptive_playback_cushion = tuning.adaptiveCushion;
    options.adaptive_playback_target_frames =
        static_cast<std::size_t>(tuning.adaptiveTargetFrames);
    options.adaptive_playback_min_frames =
        static_cast<std::size_t>(tuning.adaptiveMinFrames);
    options.adaptive_playback_max_frames =
        static_cast<std::size_t>(tuning.adaptiveMaxFrames);
    options.adaptive_playback_release_ppm = tuning.adaptiveReleasePpm;
    options.adaptive_playback_ratio_ramp_ms = tuning.adaptiveRatioRampMs;
    if (automationHeadlessAudio_) {
        options.audio_device_id.reset();
        options.headless_audio = true;
        options.audio_buffer_size = 128;
        options.test_input = automationTestInput_;
        options.os_priority = Jam2OsPriorityMode::Off;
    } else {
        bool deviceOk = false;
        const int device = sessionRuntimeDraft_.selectedDeviceId.toInt(&deviceOk);
        if (!deviceOk || device < 0) {
            throw std::runtime_error("select a valid low-latency audio device");
        }
        options.audio_device_id = device;
        options.audio_buffer_size = tuning.bufferSize;
    }
    options.profile_name = tuning.profile.toStdString();
    options.session_profile_name = options.profile_name;
    options.input_channels = jam2::audio::InputChannels::Mono;
    options.channel_selection.input = parseUiChannels(
        sessionRuntimeDraft_.audio.inputChannels, "input");
    options.channel_selection.output = parseUiChannels(
        sessionRuntimeDraft_.audio.outputChannels, "output");
    options.capture_ring_frames =
        static_cast<std::size_t>(tuning.captureRingFrames);
    options.playback_ring_frames =
        static_cast<std::size_t>(tuning.playbackRingFrames);
    options.playback_prefill_frames =
        static_cast<std::size_t>(tuning.prefillFrames);
    options.playback_max_frames =
        static_cast<std::size_t>(tuning.playbackMaxFrames);
    if (!automationHeadlessAudio_) {
        options.os_priority = runtime.osPriority == QStringLiteral("realtime")
            ? Jam2OsPriorityMode::Realtime
            : runtime.osPriority == QStringLiteral("off")
                ? Jam2OsPriorityMode::Off : Jam2OsPriorityMode::High;
    }
    return options;
}

Jam2RuntimeOptions MainWindow::networkRuntimeOptions(
    const SharedSessionController::Snapshot& snapshot) const
{
    Jam2RuntimeOptions options = runtimeOptions();
    options.session_id = sessionId_;
    options.session_key = sessionKey_;
    if (snapshot.localPeerId == 0 || snapshot.coordinatorPeerId == 0) {
        throw std::runtime_error("session membership is missing stable peer identities");
    }
    options.local_peer_id = snapshot.localPeerId;
    options.bootstrap_coordinator_peer_id = snapshot.coordinatorPeerId;
    options.bootstrap_role = snapshot.role == SharedSessionController::Role::Creator
        ? jam2::SessionBootstrapRole::Creator
        : jam2::SessionBootstrapRole::Joiner;
    if (snapshot.role == SharedSessionController::Role::Joiner) {
        options.profile_name = sessionRuntimeDraft_.joinProfileName.toStdString();
    }
    options.session_profile_name = snapshot.contract.profile.toStdString();
    options.sample_rate = snapshot.contract.sampleRate;
    options.frame_size = snapshot.contract.frameSize;
    const auto networkAudioFormat = jam2::protocol::parse_audio_format(
        snapshot.contract.audioFormat.toStdString());
    if (!networkAudioFormat) {
        throw std::runtime_error("session contract has an unsupported network audio format");
    }
    options.network_audio_format = *networkAudioFormat;
    options.mesh_peers_configured = true;
    for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
        if (peer.token == snapshot.localToken || peer.endpoint.isEmpty()) {
            continue;
        }
        options.mesh_peer_ids.push_back(peer.peerId);
        options.mesh_peers.push_back(jam2::parse_endpoint(peer.endpoint.toStdString()));
    }
    return options;
}

QString MainWindow::selectedDeviceId() const
{
    return sessionRuntimeDraft_.selectedDeviceId;
}

QString MainWindow::selectedDeviceDescription() const
{
    bool ok = false;
    const int id = sessionRuntimeDraft_.selectedDeviceId.toInt(&ok);
    if (!ok) return QStringLiteral("unknown");
    const auto device = std::find_if(
        availableDevices_.cbegin(), availableDevices_.cend(),
        [id](const auto& item) { return item.id == id; });
    if (device == availableDevices_.cend()) {
        return QStringLiteral("[%1] unknown").arg(id);
    }
    return QStringLiteral("[%1] %2 %3")
        .arg(device->id)
        .arg(QString::fromStdString(device->backend),
             QString::fromStdString(device->name));
}

QJsonObject MainWindow::trackToJson() const
{
    QJsonArray clickEnabled;
    QJsonArray clickAccents;
    for (bool enabled : metronomeEnabledSteps_) clickEnabled.append(enabled);
    for (bool accented : metronomeAccents_) clickAccents.append(accented);
    QJsonObject result = trackController_.projectJson();
    result.insert(QStringLiteral("metronome_bpm"),
        metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 80);
    result.insert(QStringLiteral("metronome_beats"),
        metronomeBeatsSpin_ ? metronomeBeatsSpin_->currentData().toInt() : 4);
    result.insert(QStringLiteral("metronome_beat_unit"),
        metronomeBeatUnitBox_ ? metronomeBeatUnitBox_->currentData().toInt() : 4);
    result.insert(QStringLiteral("metronome_tempo_pulse_units"),
        metronomeTempoPulseBox_ ? metronomeTempoPulseBox_->currentData().toInt() : 1);
    result.insert(QStringLiteral("metronome_division"),
        metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1);
    result.insert(QStringLiteral("metronome_click_enabled"), clickEnabled);
    result.insert(QStringLiteral("metronome_click_accents"), clickAccents);
    return result;
}

void MainWindow::loadTrackJson(
    const QJsonObject& object,
    SharedTrackModel validatedModel)
{
    validatedModel.syncControls = jamSyncPolicy_.globalPlayback;
    trackController_.replaceModel(std::move(validatedModel));
    const auto& model = trackController_.model();
    const int savedBpm = qBound(1,
        object.value(QStringLiteral("metronome_bpm")).toInt(static_cast<int>(std::lround(model.acceptedBpm))), 400);
    const int savedBeats = qBound(1, object.value(QStringLiteral("metronome_beats")).toInt(4), 16);
    const int savedBeatUnit = jam2::metronome::clamp_beat_unit(
        object.value(QStringLiteral("metronome_beat_unit")).toInt(4));
    const int savedTempoPulseUnits = jam2::metronome::clamp_tempo_pulse_units(
        object.value(QStringLiteral("metronome_tempo_pulse_units")).toInt(1));
    const int savedDivision = object.value(QStringLiteral("metronome_division")).toInt(1);
    {
        const QSignalBlocker bpmBlocker(metronomeBpmSpin_);
        const QSignalBlocker beatsBlocker(metronomeBeatsSpin_);
        const QSignalBlocker beatUnitBlocker(metronomeBeatUnitBox_);
        const QSignalBlocker tempoPulseBlocker(metronomeTempoPulseBox_);
        const QSignalBlocker divisionBlocker(metronomeDivisionBox_);
        metronomeBpmSpin_->setValue(savedBpm);
        const int beatsIndex = metronomeBeatsSpin_->findData(savedBeats);
        metronomeBeatsSpin_->setCurrentIndex(beatsIndex >= 0 ? beatsIndex : 3);
        const int beatUnitIndex = metronomeBeatUnitBox_->findData(savedBeatUnit);
        metronomeBeatUnitBox_->setCurrentIndex(beatUnitIndex >= 0 ? beatUnitIndex : 1);
        const int pulseIndex =
            metronomeTempoPulseBox_->findData(savedTempoPulseUnits);
        metronomeTempoPulseBox_->setCurrentIndex(
            pulseIndex >= 0 ? pulseIndex : 0);
        const int divisionIndex = metronomeDivisionBox_->findData(savedDivision);
        metronomeDivisionBox_->setCurrentIndex(divisionIndex >= 0 ? divisionIndex : 0);
    }
    const int expectedSteps = savedBeats * qMax(1, metronomeDivisionBox_->currentData().toInt());
    const QJsonArray savedEnabled = object.value(QStringLiteral("metronome_click_enabled")).toArray();
    const QJsonArray savedAccents = object.value(QStringLiteral("metronome_click_accents")).toArray();
    if (savedEnabled.size() == expectedSteps && savedAccents.size() == expectedSteps) {
        metronomeEnabledSteps_.resize(expectedSteps);
        metronomeAccents_.resize(expectedSteps);
        for (int step = 0; step < expectedSteps; ++step) {
            metronomeEnabledSteps_[step] = savedEnabled.at(step).toBool(true);
            metronomeAccents_[step] = savedAccents.at(step).toBool(false);
        }
        rebuildMetronomePattern(false);
    } else {
        rebuildMetronomePattern(true);
    }
    initializeLegacyBankTiming();
    updateTrackControls();
    loadTrackWaveform();
    if (PreparedMixRenderer::hasRenderableSources(
            looperProject_, looperProject_.activeBankIndex())) {
        regeneratePreparedMix(looperProject_.activeBankIndex());
    }
}

QJsonObject MainWindow::songToJson(bool syncCompatibleOnly) const
{
    QJsonObject root = chordModel_.toJson();
    root.insert(QStringLiteral("looper"), looperProject_.toJson(syncCompatibleOnly));
    return root;
}

bool MainWindow::loadSongJson(const QJsonObject& object)
{
    BeatGridModel loadedSong;
    LooperProject loadedLooper = looperProject_;
    if (!loadedSong.loadJson(object)) {
        return false;
    }

    const QJsonObject looperObject = object.value(QStringLiteral("looper")).toObject();
    if (!looperObject.isEmpty() && !loadedLooper.loadJson(looperObject)) {
        return false;
    }
    if (!looperObject.isEmpty() &&
        loadedSong.sections().size() != loadedLooper.banks().size()) {
        return false;
    }
    // A prepared mix is a derived cache of the old arrangement. Invalidate
    // and unload it before adopting any replacement song so a later remote
    // TrackRestart cannot revive stale PCM that is no longer represented in
    // the GUI or looper model.
    discardPreparedMix(false);
    chordModel_ = loadedSong;
    if (!jamStorage_.isSaved() &&
        chordModel_.title() != jamStorage_.displayName()) {
        const QString oldRoot = jamStorage_.rootFolder();
        QString renameError;
        if (jamStorage_.rename(chordModel_.title(), renameError)) {
            relocateManagedPaths(oldRoot, jamStorage_.rootFolder());
        } else {
            appendLog(QStringLiteral("could not adopt remote jam storage name: ") + renameError);
        }
    }
    const int expectedSampleRate = activeTrackSampleRate();
    bool needsCompatibilityAudit = false;
    for (LooperBank& bank : loadedLooper.banks()) {
        for (LooperLane& lane : bank.lanes) {
            if (lane.assetPath.trimmed().isEmpty()) {
                lane.sampleRateCompatible = true;
                continue;
            }
            lane.sampleRateCompatible = lane.sampleRate > 0 &&
                lane.sampleRate == expectedSampleRate;
            needsCompatibilityAudit = true;
        }
    }
    looperProject_ = std::move(loadedLooper);
    arrangementRunning_ = false;
    arrangementArmed_ = false;
    arrangementResetBankAfterStop_ = false;
    arrangementStepIndex_ = 0;
    arrangementStepRepeat_ = 0;
    pendingBankIndex_ = -1;
    pendingBankAbsoluteBeat_ = 0;
    pendingBankRequestedTargetBeat_.reset();
    preparedMixLifecycle_.invalidateAll();
    discardObsoletePreparedMixPaths();
    viewedBankIndex_ = looperProject_.activeBankIndex();
    if (looperProject_.hasSerializedTiming()) {
        applyMetronomePatternForBank(viewedBankIndex_, false);
    }
    if (chordGrid_) chordGrid_->setSelectedSectionIndex(viewedBankIndex_);
    if (beatGrid_) beatGrid_->setSelectedSectionIndex(viewedBankIndex_);
    if (lyricGrid_) lyricGrid_->setSelectedSectionIndex(viewedBankIndex_);
    refreshLooperLanes();
    if (looperProject_.arrangement().enabled &&
        !looperProject_.arrangement().steps.isEmpty()) {
        startArrangement();
    }
    startDeferredPracticeReferenceRenders();
    if (needsCompatibilityAudit) {
        QTimer::singleShot(0, this, [this, expectedSampleRate] {
            auditWavCompatibilityForSession(expectedSampleRate, true);
        });
    }
    return true;
}

void MainWindow::updatePlaybackGrid()
{
    const int liveBank = looperProject_.activeBankIndex();
    const auto livePattern = bankMetronomePattern(liveBank);
    const bool jamGrid = jam2_.isRunning();
    if (!jamGrid) {
        metronomeTransport_.grid().setPattern(
            livePattern.bpm,
            livePattern.beats_per_bar,
            livePattern.division,
            livePattern.tempo_pulse_units);
        metronomeTransport_.grid().clearEngine();
    }
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    updateArrangementPlayback(position);
    const int beatsPerBar = livePattern.beats_per_bar;
    const double backingBpm = qMax(1.0, trackController_.model().acceptedBpm);
    const int tempoPulseUnits = livePattern.tempo_pulse_units;
    const int visualDivision = qMax(1, livePattern.division);
    const qint64 transportPositionMs =
        trackRecordingWorkflow_.currentTransportPositionMs(
            position, trackController_.model().durationMs);
    const double stoppedTrackBeat =
        static_cast<double>(qMax<qint64>(0, transportPositionMs)) *
        backingBpm * tempoPulseUnits / 60000.0;
    quint64 visualAbsoluteBeat = static_cast<quint64>(std::floor(stoppedTrackBeat));
    int visualSubdivision =
        static_cast<int>(std::floor(stoppedTrackBeat * visualDivision)) % visualDivision;
    double beatPhase = stoppedTrackBeat - std::floor(stoppedTrackBeat);
    bool visualRunning = false;
    // The UDP epoch remains continuous. Song/track markers use a separate
    // relative origin so source frame zero can be shown as 1.1 without ever
    // replacing that session epoch.
    const std::uint64_t recordingStart =
        trackRecordingWorkflow_.recordingStartFrame();
    if (trackRecordingWorkflow_.inputTakeActive() && recordingStart > 0 &&
        position.rawCurrentFrame < recordingStart) {
        visualAbsoluteBeat = 0;
        visualSubdivision = 0;
        beatPhase = 0.0;
        visualRunning = false;
    } else if (trackRecordingWorkflow_.inputTakeActive() && recordingStart > 0 &&
               position.rawCurrentFrame >= recordingStart &&
               position.sampleRate > 0 && position.secondsPerBeat > 0.0) {
        const double trackBeat = static_cast<double>(
            position.rawCurrentFrame - recordingStart) /
            (position.secondsPerBeat * static_cast<double>(position.sampleRate));
        visualAbsoluteBeat = static_cast<quint64>(std::floor(trackBeat));
        visualSubdivision =
            static_cast<int>(std::floor(trackBeat * visualDivision)) % visualDivision;
        beatPhase = trackBeat - std::floor(trackBeat);
        visualRunning = true;
    } else if (loopbackRecorder_.isRunning() &&
               loopbackRecordingPreviewClock_.isValid()) {
        const double trackBeat =
            static_cast<double>(loopbackRecordingPreviewClock_.elapsed()) *
            qMax(1.0, static_cast<double>(livePattern.bpm)) *
            qMax(1, tempoPulseUnits) / 60000.0;
        visualAbsoluteBeat = static_cast<quint64>(std::floor(trackBeat));
        visualSubdivision =
            static_cast<int>(std::floor(trackBeat * visualDivision)) % visualDivision;
        beatPhase = trackBeat - std::floor(trackBeat);
        visualRunning = true;
    } else if (trackRecordingWorkflow_.globalTransportPlaying()) {
        const double transportBpm = trackController_.model().durationMs > 0
            ? backingBpm
            : qMax(1.0, metronomeTransport_.grid().bpm());
        const double trackBeat =
            static_cast<double>(qMax<qint64>(0, transportPositionMs)) *
            transportBpm * tempoPulseUnits / 60000.0;
        visualAbsoluteBeat = static_cast<quint64>(std::floor(trackBeat));
        visualSubdivision =
            static_cast<int>(std::floor(trackBeat * visualDivision)) % visualDivision;
        beatPhase = trackBeat - std::floor(trackBeat);
        visualRunning = true;
    }
    const int patternStepCount = qMax(
        1, livePattern.beats_per_bar * visualDivision);
    const bool metronomeVisualRunning = visualRunning;
    const int metronomeVisualStep = visualRunning
        ? static_cast<int>((
            (visualAbsoluteBeat % static_cast<std::uint64_t>(qMax(1, beatsPerBar))) *
                static_cast<std::uint64_t>(visualDivision) +
            static_cast<std::uint64_t>(visualSubdivision)) %
            static_cast<std::uint64_t>(patternStepCount))
        : static_cast<int>(
            position.absoluteStep % static_cast<std::uint64_t>(patternStepCount));
    double metronomePulsePhase = visualRunning
        ? std::fmod(beatPhase * visualDivision, 1.0)
        : position.secondsPerStep > 0.0
            ? std::fmod(position.secondsFromEpoch / position.secondsPerStep, 1.0)
            : 0.0;
    if (metronomePulsePhase < 0.0) metronomePulsePhase += 1.0;
    const bool currentStepEnabled = jam2::metronome::mask_enabled(
        livePattern.play_mask_low,
        livePattern.play_mask_high,
        metronomeVisualStep);
    const bool currentStepAccent = currentStepEnabled && jam2::metronome::mask_enabled(
        livePattern.accent_mask_low,
        livePattern.accent_mask_high,
        metronomeVisualStep);
    const int currentPulseState = currentStepAccent ? 2 : currentStepEnabled ? 1 : 0;
    const int subdivisionWithinBeat = visualRunning
        ? visualSubdivision
        : static_cast<int>(position.absoluteStep % static_cast<std::uint64_t>(visualDivision));
    const int primaryStep =
        (metronomeVisualStep - subdivisionWithinBeat + patternStepCount) % patternStepCount;
    const bool primaryStepEnabled = jam2::metronome::mask_enabled(
        livePattern.play_mask_low,
        livePattern.play_mask_high,
        primaryStep);
    const bool primaryStepAccent = primaryStepEnabled && jam2::metronome::mask_enabled(
        livePattern.accent_mask_low,
        livePattern.accent_mask_high,
        primaryStep);
    const int primaryPulseState =
        primaryStepAccent ? 2 : primaryStepEnabled ? 1 : 0;
    const double metronomeBeatPhase = visualRunning
        ? beatPhase
        : (static_cast<double>(subdivisionWithinBeat) + metronomePulsePhase) /
            static_cast<double>(visualDivision);
    const double metronomeStepsPerSecond =
        static_cast<double>(qMax(1, livePattern.bpm)) *
        static_cast<double>(qMax(1, tempoPulseUnits)) *
        static_cast<double>(visualDivision) / 60.0;
    if (metronomePatternWidget_) {
        metronomePatternWidget_->setCurrentStep(
            metronomeVisualStep,
            metronomeVisualRunning);
    }
    if (metronomeNebula_) {
        metronomeNebula_->setPulseState(
            currentPulseState,
            metronomePulsePhase,
            primaryPulseState,
            metronomeBeatPhase,
            metronomeStepsPerSecond,
            metronomeVisualRunning);
    }
    const bool editorMarkerRunning = visualRunning;
    const auto sectionBeatCount = [](const BeatGridModel& model, int section) {
        return model.sections().isEmpty()
            ? quint64{1}
            : static_cast<quint64>(qMax(1, model.section(
                qBound(0, section, model.sections().size() - 1)).beats));
    };
    const quint64 performanceSectionBeats = sectionBeatCount(chordModel_, liveBank);
    const quint64 performanceSectionBeat =
        visualAbsoluteBeat % performanceSectionBeats;
    const bool arrangementActive = arrangementRunning_ || arrangementArmed_;
    int arrangementUpcomingBank = -1;
    quint64 arrangementBeatsRemaining = 0;
    if (arrangementActive) {
        const ArrangementDefinition& arrangement = looperProject_.arrangement();
        if (pendingBankIndex_ >= 0) {
            arrangementUpcomingBank = pendingBankIndex_;
            arrangementBeatsRemaining = pendingBankAbsoluteBeat_ > position.absoluteBeat
                ? pendingBankAbsoluteBeat_ - position.absoluteBeat : 0;
        } else if (arrangementArmed_ && !arrangement.steps.isEmpty()) {
            arrangementUpcomingBank = arrangement.steps.front().bankIndex;
        } else if (arrangementRunning_ && arrangementStepIndex_ >= 0 &&
                   arrangementStepIndex_ < arrangement.steps.size()) {
            const ArrangementStep& current = arrangement.steps.at(arrangementStepIndex_);
            int nextStep = arrangementStepIndex_ + 1;
            if (nextStep >= arrangement.steps.size()) {
                nextStep = arrangement.loop ? 0 : -1;
            }
            if (nextStep >= 0) {
                arrangementUpcomingBank = arrangement.steps.at(nextStep).bankIndex;
                const quint64 currentEnd = arrangementSectionStartBeat_ +
                    performanceSectionBeats;
                const quint64 beatsToCurrentEnd = currentEnd > position.absoluteBeat
                    ? currentEnd - position.absoluteBeat : 0;
                const int repeatsAfterCurrent = qMax(
                    0, current.repeats - arrangementStepRepeat_ - 1);
                arrangementBeatsRemaining = beatsToCurrentEnd +
                    static_cast<quint64>(repeatsAfterCurrent) * performanceSectionBeats;
            }
        }
    }
    updateRecordingCountdown(position);
    const int viewedBeatsPerBar = sectionBeatsPerBar(viewedBankIndex_);
    if (chordGrid_) {
        chordGrid_->setBeatsPerBar(viewedBeatsPerBar);
        chordGrid_->setGridPosition(
            visualAbsoluteBeat % sectionBeatCount(chordModel_, viewedBankIndex_),
            visualSubdivision,
            editorMarkerRunning && viewedBankIndex_ == liveBank,
            beatPhase);
        chordGrid_->setUpcomingSection(
            arrangementUpcomingBank,
            arrangementBeatsRemaining,
            beatsPerBar,
            arrangementUpcomingBank >= 0
                ? bankMetronomePattern(arrangementUpcomingBank).beats_per_bar : beatsPerBar,
            arrangementActive,
            arrangementArmed_);
    }
    if (beatGrid_) {
        beatGrid_->setBeatsPerBar(viewedBeatsPerBar);
        beatGrid_->setGridPosition(
            visualAbsoluteBeat % sectionBeatCount(beatModel_, viewedBankIndex_),
            visualSubdivision,
            editorMarkerRunning && viewedBankIndex_ == liveBank,
            beatPhase);
        beatGrid_->setUpcomingSection(
            arrangementUpcomingBank,
            arrangementBeatsRemaining,
            beatsPerBar,
            arrangementUpcomingBank >= 0
                ? bankMetronomePattern(arrangementUpcomingBank).beats_per_bar : beatsPerBar,
            arrangementActive,
            arrangementArmed_);
    }
    if (lyricGrid_) {
        lyricGrid_->setBeatsPerBar(viewedBeatsPerBar);
        lyricGrid_->setGridPosition(
            visualAbsoluteBeat % sectionBeatCount(lyricModel_, viewedBankIndex_),
            visualSubdivision,
            editorMarkerRunning && viewedBankIndex_ == liveBank,
            beatPhase);
    }
    if (performanceHome_) {
        QString bankStatus;
        if (arrangementArmed_) {
            bankStatus = arrangementUpcomingBank >= 0
                ? QStringLiteral("NEXT %1  ON PLAY")
                    .arg(QChar(QLatin1Char('A').unicode() + arrangementUpcomingBank))
                : QStringLiteral("ARRANGEMENT ARMED");
        } else if (arrangementRunning_) {
            if (arrangementUpcomingBank >= 0) {
                const quint64 barBeats = static_cast<quint64>(qMax(1, beatsPerBar));
                const quint64 bars = qMax<quint64>(
                    1, (arrangementBeatsRemaining + barBeats - 1) / barBeats);
                bankStatus = QStringLiteral("NEXT %1  %2 BAR%3")
                    .arg(QChar(QLatin1Char('A').unicode() + arrangementUpcomingBank))
                    .arg(bars)
                    .arg(bars == 1 ? QString{} : QStringLiteral("S"));
            } else {
                bankStatus = QStringLiteral("ARRANGEMENT  FINAL SECTION");
            }
        } else if (pendingBankIndex_ >= 0) {
            const quint64 beatsRemaining = pendingBankAbsoluteBeat_ > position.absoluteBeat
                ? pendingBankAbsoluteBeat_ - position.absoluteBeat : 0;
            bankStatus = QStringLiteral("NEXT %1  %2 BEAT%3")
                .arg(QChar(QLatin1Char('A').unicode() + pendingBankIndex_))
                .arg(beatsRemaining)
                .arg(beatsRemaining == 1 ? QString{} : QStringLiteral("S"));
        }
        const int performanceUpcomingBank = arrangementActive
            ? arrangementUpcomingBank : pendingBankIndex_;
        const quint64 performanceUpcomingBeats = arrangementActive
            ? arrangementBeatsRemaining
            : pendingBankAbsoluteBeat_ > position.absoluteBeat
                ? pendingBankAbsoluteBeat_ - position.absoluteBeat : 0;
        performanceHome_->setBankState(
            liveBank,
            performanceUpcomingBank,
            performanceUpcomingBeats,
            performanceUpcomingBank >= 0
                ? bankMetronomePattern(performanceUpcomingBank).beats_per_bar : beatsPerBar,
            !jamSyncPolicy_.globalPlayback,
            bankStatus);
        performanceHome_->setArrangementState(arrangementRunning_, arrangementArmed_);
        performanceHome_->setTiming(
            performanceSectionBeat,
            visualSubdivision,
            beatsPerBar,
            beatPhase,
            visualRunning);
        performanceHome_->setTrackGainDb(trackController_.model().trackGainDb);
        if (rendererStatsLabel_) {
            rendererStatsLabel_->setText(performanceHome_->rendererStatsText());
        }
    }
    if (performancePositionLabel_) {
        const quint64 bar =
            performanceSectionBeat / static_cast<quint64>(qMax(1, beatsPerBar)) + 1;
        const quint64 beat =
            performanceSectionBeat % static_cast<quint64>(qMax(1, beatsPerBar)) + 1;
        performancePositionLabel_->setText(
            QStringLiteral("%1.%2\nBAR / BEAT")
                .arg(bar)
                .arg(beat, 2, 10, QLatin1Char('0')));
    }
    if (performanceTempoButton_) {
        const auto pattern = currentMetronomePattern();
        performanceTempoButton_->setText(QStringLiteral("%1 BPM").arg(pattern.bpm));
    }
    if (performanceMetronomeToggle_) {
        const bool enabled = metronomeTransport_.localRunning();
        performanceMetronomeToggle_->setText(
            enabled ? QStringLiteral("METRONOME ON") : QStringLiteral("METRONOME OFF"));
        performanceMetronomeToggle_->setProperty("active", enabled);
        performanceMetronomeToggle_->style()->unpolish(performanceMetronomeToggle_);
        performanceMetronomeToggle_->style()->polish(performanceMetronomeToggle_);
    }
    if (gridPositionLabel_) {
        if (position.running) {
            QString text = QStringLiteral(
                "Grid: beat %1.%2 abs_beat=%3 abs_step=%4 epoch_s=%5 beat_s=%6 step_s=%7 source=%8")
                .arg(position.beat + 1)
                .arg(position.subdivision + 1)
                .arg(position.absoluteBeat)
                .arg(position.absoluteStep)
                .arg(position.secondsFromEpoch, 0, 'f', 3)
                .arg(position.secondsPerBeat, 0, 'f', 6)
                .arg(position.secondsPerStep, 0, 'f', 6)
                .arg(position.engineAnchored ? QStringLiteral("engine") : QStringLiteral("none"));
        if (position.engineAnchored) {
                text += QStringLiteral(" musical_frame=%1 raw_frame=%2 epoch_frame=%3 offset_frames=%4 rate=%5")
                    .arg(position.currentFrame)
                    .arg(position.rawCurrentFrame)
                    .arg(position.epochFrame)
                    .arg(position.renderOffsetFrames)
                    .arg(position.sampleRate);
            }
            gridPositionLabel_->setText(text);
        } else if (position.engineAnchored) {
            gridPositionLabel_->setText(QStringLiteral("Grid: waiting at beat 1.1 for shared epoch"));
        } else {
            gridPositionLabel_->setText(QStringLiteral("Grid: stopped"));
        }
    }
    if (trackWaveform_) {
        trackWaveform_->setBpm(metronomeTransport_.grid().bpm());
        const bool trackPlaying =
            trackRecordingWorkflow_.globalTransportPlaying();
        const qint64 gridPositionMs = qMax<qint64>(0, transportPositionMs);
        trackWaveform_->setGridPosition(
            gridPositionMs,
            trackPlaying,
            currentMetronomePattern().beats_per_bar,
            currentMetronomePattern().tempo_pulse_units);
    }
    if (looperStack_) {
        qint64 gridPositionMs = qMax<qint64>(0, transportPositionMs);
        if (loopbackRecorder_.isRunning() &&
            loopbackRecordingPreviewClock_.isValid()) {
            gridPositionMs = loopbackRecordingPreviewClock_.elapsed();
        }
        const auto viewedPattern = bankMetronomePattern(viewedBankIndex_);
        qint64 recordingPreviewFrames = 0;
        if (trackRecordingWorkflow_.inputTakeActive() &&
            trackRecordingWorkflow_.armedBank() == viewedBankIndex_) {
            const double recordedBeats = static_cast<double>(visualAbsoluteBeat) +
                qBound(0.0, beatPhase, 0.999999);
            recordingPreviewFrames = static_cast<qint64>(std::llround(
                recordedBeats * qMax(1, activeTrackSampleRate()) * 60.0 /
                (qMax(1, viewedPattern.bpm) *
                 qMax(1, viewedPattern.tempo_pulse_units))));
        } else if (loopbackRecorder_.isRunning() &&
                   trackRecordingWorkflow_.armedBank() == viewedBankIndex_ &&
                   loopbackRecordingPreviewClock_.isValid()) {
            recordingPreviewFrames = loopbackRecordingPreviewClock_.elapsed() *
                static_cast<qint64>(qMax(1, activeTrackSampleRate())) / 1000;
        }
        looperStack_->setLiveRecordingEndFrame(recordingPreviewFrames);
        looperStack_->setGridPosition(
            gridPositionMs,
            editorMarkerRunning && viewedBankIndex_ == liveBank,
            viewedPattern.bpm,
            viewedPattern.beats_per_bar,
            viewedPattern.tempo_pulse_units);
    }
}

void MainWindow::updateRecordingCountdown(const PlaybackGrid::Position& position)
{
    if (!recordingCountdownLabel_) {
        return;
    }
    if (updateLoopbackCountIn(position)) {
        return;
    }
    const TrackRecordingWorkflow::CountdownPresentation countdown =
        trackRecordingWorkflow_.countdown(position);
    if (countdown.phase == TrackRecordingWorkflow::CountdownPhase::Hidden) {
        recordingCountdownLabel_->setText(
            QStringLiteral("ARMED  ›  WAITING FOR BEAT  ›  COUNT-IN  ›  RECORDING"));
        return;
    }
    if (countdown.phase == TrackRecordingWorkflow::CountdownPhase::WaitingForBeat) {
        recordingCountdownLabel_->setText(
            QStringLiteral("ARMED  ✓  WAITING FOR BEAT  ●  COUNT-IN  ›  RECORDING"));
        publishLocalTrackRecordingState(QStringLiteral("waiting"));
        return;
    }
    if (countdown.phase == TrackRecordingWorkflow::CountdownPhase::Counting) {
        recordingCountdownLabel_->setText(QStringLiteral(
            "ARMED  ✓  WAITING FOR BEAT  ✓  COUNT-IN  %1  ●  RECORDING")
            .arg(countdown.remainingBeats));
        publishLocalTrackRecordingState(
            QStringLiteral("count-in"), countdown.remainingBeats);
        return;
    }
    recordingCountdownLabel_->setText(
        QStringLiteral("ARMED  ✓  WAITING FOR BEAT  ✓  COUNT-IN  ✓  RECORDING  ●"));
    publishLocalTrackRecordingState(QStringLiteral("recording"));
    if (countdown.stopMetronome) {
        stopTrackMetronome();
    }
}

void MainWindow::runGridLockedEngineAction(
    const QString& actionName,
    const std::function<void(std::uint64_t)>& action,
    bool quantizeToBar)
{
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    if (!position.engineAnchored || position.sampleRate <= 0) {
        if (gridScheduleLabel_) {
            gridScheduleLabel_->setText(QStringLiteral("Grid lock: %1 waiting for engine").arg(actionName));
        }
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Start the local engine before using track transport."));
        return;
    }

    std::uint64_t targetFrame = position.rawCurrentFrame;
    QString targetText;
    if (looperProject_.gridLockEnabled() && position.running && position.secondsPerBeat > 0.0) {
        const std::uint64_t targetBeat = nextGridBoundaryBeat(
            position.absoluteBeat,
            currentMetronomePattern().beats_per_bar,
            quantizeToBar);
        const std::uint64_t targetStep = targetBeat * static_cast<std::uint64_t>(qMax(1, currentMetronomePattern().division));
        const std::uint64_t targetMusicalFrame = position.epochFrame +
            static_cast<std::uint64_t>(std::llround(static_cast<double>(targetBeat) * position.secondsPerBeat * static_cast<double>(position.sampleRate)));
        targetFrame = rawFrameFromMusicalFrame(targetMusicalFrame, position.renderOffsetFrames);
        const double delayMs = targetFrame > position.rawCurrentFrame
            ? (static_cast<double>(targetFrame - position.rawCurrentFrame) * 1000.0 / static_cast<double>(position.sampleRate))
            : 0.0;
        targetText = QStringLiteral(
            "Grid lock: %1 engine boundary=%2 target_beat=%3 target_step=%4 target_musical_frame=%5 target_raw_frame=%6 current_musical_frame=%7 current_raw_frame=%8 offset_frames=%9 delay_ms=%10")
            .arg(actionName)
            .arg(quantizeToBar ? QStringLiteral("bar") : QStringLiteral("beat"))
            .arg(targetBeat)
            .arg(targetStep)
            .arg(targetMusicalFrame)
            .arg(targetFrame)
            .arg(position.currentFrame)
            .arg(position.rawCurrentFrame)
            .arg(position.renderOffsetFrames)
            .arg(delayMs, 0, 'f', 3);
    } else {
        targetText = QStringLiteral("Grid lock: %1 engine immediate target_frame=%2").arg(actionName).arg(targetFrame);
    }
    if (gridScheduleLabel_) {
        gridScheduleLabel_->setText(targetText);
    }
    appendLog(targetText);
    action(targetFrame);
}

void MainWindow::newSong()
{
    bool discardWorkspace = false;
    if (hasUnsavedProjectChanges() || jamStorage_.hasArtifacts()) {
        QMessageBox dialog(
            QMessageBox::Question,
            QStringLiteral("New Jam"),
            QStringLiteral("Save this jam before starting a new one?"),
            QMessageBox::NoButton,
            this);
        QPushButton* save = dialog.addButton(QStringLiteral("Save JamJar"), QMessageBox::AcceptRole);
        QPushButton* discard = dialog.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        QPushButton* cancel = dialog.addButton(QMessageBox::Cancel);
        jam2::gui::registerGuiControl(
            *save, QStringLiteral("session.new-dialog.save"),
            QStringLiteral("session.persistence"),
            jam2::gui::GuiControlAvailability::Modal);
        jam2::gui::registerGuiControl(
            *discard, QStringLiteral("session.new-dialog.discard"),
            QStringLiteral("session.persistence"),
            jam2::gui::GuiControlAvailability::Modal);
        jam2::gui::registerGuiControl(
            *cancel, QStringLiteral("session.new-dialog.cancel"),
            QStringLiteral("session.persistence"),
            jam2::gui::GuiControlAvailability::Modal);
        dialog.exec();
        if (dialog.clickedButton() == cancel || dialog.clickedButton() == nullptr) return;
        if (dialog.clickedButton() == save && !saveSong()) return;
        discardWorkspace = dialog.clickedButton() == discard;
    }
    if (discardWorkspace && !jamStorage_.isSaved() &&
        QFileInfo::exists(jamStorage_.rootFolder())) {
        fileWorkerPool_.waitForDone();
        QString discardError;
        if (!jamStorage_.discardUnsaved(discardError)) {
            QMessageBox::warning(this, QStringLiteral("Jam2"), discardError);
            return;
        }
    }
    chordModel_.reset();
    (void)JamStorage::pruneEmptyUnsavedWorkspaces();
    const QString jamName = JamStorage::randomDisplayName();
    chordModel_.setTitle(jamName);
    jamStorage_.startNew(jamName);
    projectPersistence_.clearTransientTracking();
    projectPersistence_.initializeWorkspace(jamStorage_.rootFolder());
    stopTrackMetronome();
    trackController_ = SharedTrackController{};
    looperProject_ = LooperProject{};
    looperProject_.ensureInitialEmptyLanes();
    applyNewJamDefaults();
    applyMetronomePatternForBank(0, false);
    trackRecordingWorkflow_.clearProjectCapture();
    if (trackWaveform_) {
        trackWaveform_->clear();
    }
    updateTrackControls();
    refreshLooperLanes();
    refreshSongViews();
    projectPersistence_.acceptNewProject(currentProjectSnapshot());
}

void MainWindow::openSong()
{
    bool discardCurrent = false;
    const bool hasTransientWavs = projectPersistence_.hasExistingTransientWavs() ||
        !trackRecordingWorkflow_.pendingTransientCapturePath().isEmpty();
    if (hasUnsavedProjectChanges() || hasTransientWavs) {
        QMessageBox dialog(
            QMessageBox::Question,
            QStringLiteral("Open JamJar"),
            QStringLiteral("Save this project before opening another JamJar?"),
            QMessageBox::NoButton,
            this);
        QPushButton* save = dialog.addButton(
            QStringLiteral("Save Project"), QMessageBox::AcceptRole);
        QPushButton* discard = dialog.addButton(
            QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        QPushButton* cancel = dialog.addButton(QMessageBox::Cancel);
        dialog.exec();
        if (dialog.clickedButton() == cancel || dialog.clickedButton() == nullptr) return;
        if (dialog.clickedButton() == save && !saveSong()) return;
        discardCurrent = dialog.clickedButton() == discard;
    }
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open JamJar"),

        appReleaseFolderPath(QStringLiteral("songs")),
        QStringLiteral("JamJar (*.jamjar);;JSON files (*.json);;All files (*)"),
        nullptr,
        QFileDialog::Options{});
    if (path.isEmpty()) {
        return;
    }
    auto root = std::make_shared<QJsonObject>();
    auto error = std::make_shared<QString>();
    const bool previousWasUnsaved = !jamStorage_.isSaved();
    (void)startFileWorkerTask(
        [path, root, error] {
            (void)ProjectPersistenceCoordinator::readSongJson(path, *root, *error);
        },
        [this, path, root, error, discardCurrent, previousWasUnsaved] {
            BeatGridModel validatedSong;
            LooperProject validatedLooper = looperProject_;
            const QJsonObject looper = root->value(QStringLiteral("looper")).toObject();
            const SharedTrackController::ProjectDecodeResult validatedTrack =
                SharedTrackController::decodeProjectJson(
                    root->value(QStringLiteral("track")),
                    QFileInfo(path).absolutePath(),
                    jamSyncPolicy_.globalPlayback);
            const bool valid = error->isEmpty() && validatedSong.loadJson(*root) &&
                (looper.isEmpty() || validatedLooper.loadJson(looper)) &&
                (looper.isEmpty() ||
                 validatedSong.sections().size() == validatedLooper.banks().size()) &&
                validatedTrack.valid;
            if (!valid) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Jam2"),
                    !error->isEmpty() ? *error :
                        !validatedTrack.error.isEmpty() ? validatedTrack.error :
                            QStringLiteral("Invalid JamJar file."));
                return;
            }

            // Finish the old storage lifecycle before changing roots. Otherwise
            // prepared/reference renders from the old unsaved project become
            // unreachable non-empty folders under release/tracks.
            discardPreparedMix(false);
            fileWorkerPool_.waitForDone();
            if (discardCurrent && previousWasUnsaved &&
                QFileInfo::exists(jamStorage_.rootFolder())) {
                QString discardError;
                if (!jamStorage_.discardUnsaved(discardError)) {
                    QMessageBox::warning(this, QStringLiteral("Discard Jam"), discardError);
                    return;
                }
                projectPersistence_.clearTransientTracking();
            } else {
                cleanupTransientTrackWavs();
                fileWorkerPool_.waitForDone();
            }

            jamStorage_.openSaved(path, validatedSong.title());
            projectPersistence_.setProjectLocation(path);
            projectPersistence_.initializeWorkspace(jamStorage_.rootFolder());
            if (!loadSongJson(*root)) {
                QMessageBox::warning(
                    this, QStringLiteral("Jam2"), QStringLiteral("Invalid JamJar file."));
                return;
            }
            if (validatedTrack.normalized) {
                appendLog(QStringLiteral(
                    "normalized persisted track loop bounds while opening JamJar"));
            }
            loadTrackJson(
                root->value(QStringLiteral("track")).toObject(),
                validatedTrack.model);
            refreshSongViews();
            projectPersistence_.acceptOpenedProject(path, currentProjectSnapshot());
        });
}

bool MainWindow::saveSong()
{
    if (trackRecordingWorkflow_.jamRecordingActive() ||
        trackRecordingWorkflow_.inputTakeActive() || loopbackRecorder_.isRunning() ||
        fileWorkerTasksActive_ > 0) {
        QMessageBox::warning(this, QStringLiteral("Save JamJar"),
            QStringLiteral("Finish the active recording or file operation before saving."));
        return false;
    }
    if (!renameCurrentJam(songTitleEdit_->text())) return false;
    // Verify and make every lane portable while this is still an unsaved
    // workspace. A failed verification must not promote the workspace to a
    // saved JamJar or clear its artifact state.
    const QString materializationRoot = jamStorage_.rootFolder();
    if (!QDir().mkpath(materializationRoot)) {
        QMessageBox::warning(this, QStringLiteral("Save JamJar"),
            QStringLiteral("Could not create the song folder: %1")
                .arg(materializationRoot));
        return false;
    }
    if (!materializeLooperAssets(materializationRoot)) {
        return false;
    }
    const QString oldRoot = jamStorage_.rootFolder();
    QString moveError;
    if (!jamStorage_.moveToSongs(moveError)) {
        QMessageBox::warning(this, QStringLiteral("Save JamJar"), moveError);
        return false;
    }
    const QString songFolder = jamStorage_.rootFolder();
    if (QDir::cleanPath(oldRoot) != QDir::cleanPath(songFolder)) {
        relocateManagedPaths(oldRoot, songFolder);
    }
    const QString path = jamStorage_.projectFilePath();
    const QFileInfo songInfo(path);
    QJsonObject root = songToJson();
    QJsonObject savedTrack = trackToJson();
    const QString trackPath = savedTrack.value(QStringLiteral("file_path")).toString();
    const QString songPrefix = QDir(songFolder).absolutePath() + QLatin1Char('/');
    if (QFileInfo(trackPath).isAbsolute() &&
        QDir::cleanPath(trackPath).startsWith(songPrefix, Qt::CaseInsensitive)) {
        savedTrack.insert(
            QStringLiteral("file_path"),
            QDir(songFolder).relativeFilePath(trackPath));
    }
    root.insert(QStringLiteral("track"), savedTrack);
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    auto saveError = std::make_shared<QString>();
    QEventLoop saveWaitLoop;
    const bool saveStarted = startFileWorkerTask(
        [path, json, saveError] {
            (void)ProjectPersistenceCoordinator::writeSongJson(path, json, *saveError);
        },
        [&saveWaitLoop] { saveWaitLoop.quit(); },
        [&saveWaitLoop, saveError](const QString& error) {
            *saveError = error;
            saveWaitLoop.quit();
        });
    if (!saveStarted) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("The bounded file worker is busy; try Save again."));
        return false;
    }
    QProgressDialog saveProgress(
        QStringLiteral("Writing JamJar..."),
        QString{},
        0,
        0,
        this);
    saveProgress.setCancelButton(nullptr);
    saveProgress.setWindowModality(Qt::ApplicationModal);
    saveProgress.setMinimumDuration(0);
    saveWaitLoop.exec();
    saveProgress.close();
    if (!saveError->isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), *saveError);
        return false;
    }
    QSet<QString> persistentAssetPaths;
    for (const LooperBank& bank : std::as_const(looperProject_.banks())) {
        for (const LooperLane& lane : bank.lanes) {
            if (!lane.assetPath.trimmed().isEmpty()) {
                const QString savedAsset = QFileInfo(lane.assetPath).isAbsolute()
                    ? lane.assetPath
                    : QDir(songInfo.absolutePath()).absoluteFilePath(lane.assetPath);
                persistentAssetPaths.insert(savedAsset);
            }
        }
    }
    projectPersistence_.acceptSavedProject(
        songInfo.absoluteFilePath(), currentProjectSnapshot(), persistentAssetPaths);
    jamStorage_.clearArtifactState();
    return true;
}

QByteArray MainWindow::currentProjectSnapshot() const
{
    QJsonObject root = songToJson();
    root.insert(QStringLiteral("track"), trackToJson());
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool MainWindow::hasUnsavedProjectChanges() const
{
    return jamStorage_.hasArtifacts() ||
        projectPersistence_.hasUnsavedChanges(currentProjectSnapshot());
}

void MainWindow::registerTransientTrackWav(const QString& path)
{
    projectPersistence_.registerTransientWav(path);
    jamStorage_.markArtifactCreated();
}

QString MainWindow::jamAssetFolder(JamStorage::AssetKind kind) const
{
    return jamStorage_.assetFolder(kind);
}

void MainWindow::relocateManagedPaths(const QString& oldRoot, const QString& newRoot)
{
    const QString oldPrefix = QDir(oldRoot).absolutePath() + QLatin1Char('/');
    const auto relocated = [&oldPrefix, &newRoot](const QString& path) {
        if (path.trimmed().isEmpty() || !QFileInfo(path).isAbsolute()) return path;
        const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        return clean.startsWith(oldPrefix, Qt::CaseInsensitive)
            ? QDir(newRoot).absoluteFilePath(clean.mid(oldPrefix.size()))
            : path;
    };
    for (LooperBank& bank : looperProject_.banks()) {
        for (LooperLane& lane : bank.lanes) lane.assetPath = relocated(lane.assetPath);
    }
    trackController_.model().filePath = relocated(trackController_.model().filePath);
    preparedMixLifecycle_.relocatePaths(relocated);
    if (!trackRecordingWorkflow_.lastCapturePath().isEmpty()) {
        trackRecordingWorkflow_.setLastCapturePath(
            relocated(trackRecordingWorkflow_.lastCapturePath()));
    }
    for (auto it = trackOfferAssetPaths_.begin(); it != trackOfferAssetPaths_.end(); ++it) {
        it.value() = relocated(it.value());
    }
    looperWaveformCache_.clear();
    projectPersistence_.relocateWorkspace(newRoot);
}

bool MainWindow::renameCurrentJam(const QString& displayName)
{
    const QString trimmed = displayName.trimmed();
    if (trimmed == chordModel_.title()) return true;
    if (trackRecordingWorkflow_.jamRecordingActive() ||
        trackRecordingWorkflow_.inputTakeActive() || loopbackRecorder_.isRunning() ||
        fileWorkerTasksActive_ > 0 ||
        incomingAssetWorkflow_ != TrackWorkspaceController::IncomingAssetWorkflow::None) {
        if (songTitleEdit_) songTitleEdit_->setText(chordModel_.title());
        QMessageBox::warning(this, QStringLiteral("Rename Jam"),
            QStringLiteral("Finish the active recording, render, or transfer before renaming the jam."));
        return false;
    }
    const QString oldRoot = jamStorage_.rootFolder();
    QString error;
    if (!jamStorage_.rename(trimmed, error)) {
        if (songTitleEdit_) songTitleEdit_->setText(chordModel_.title());
        QMessageBox::warning(this, QStringLiteral("Rename Jam"), error);
        return false;
    }
    chordModel_.setTitle(trimmed);
    const QString newRoot = jamStorage_.rootFolder();
    if (QDir::cleanPath(oldRoot) != QDir::cleanPath(newRoot)) {
        relocateManagedPaths(oldRoot, newRoot);
    }
    if (songTitleEdit_) songTitleEdit_->setText(chordModel_.title());
    sendSongSnapshot();
    return true;
}

bool MainWindow::looperAssetPathIsReferenced(const QString& path) const
{
    const auto normalized = [](const QString& value) {
        const QFileInfo info(value);
        const QString canonical = info.canonicalFilePath();
        return QDir::cleanPath(
            canonical.isEmpty() ? info.absoluteFilePath() : canonical);
    };
    const QString expected = normalized(path);
    if (!trackController_.model().filePath.trimmed().isEmpty() &&
        normalized(trackController_.model().filePath).compare(
            expected, Qt::CaseInsensitive) == 0) {
        return true;
    }
    for (const LooperBank& bank : looperProject_.banks()) {
        for (const LooperLane& lane : bank.lanes) {
            const QString candidate = normalized(looperAssetAbsolutePath(lane));
            if (candidate.compare(expected, Qt::CaseInsensitive) == 0) return true;
        }
    }
    return false;
}

void MainWindow::discardObsoleteReferenceWavs(const QSet<QString>& paths)
{
    for (const QString& path : paths) {
        looperWaveformCache_.remove(path);
        if (!looperAssetPathIsReferenced(path) &&
            !projectPersistence_.discardTransientWav(path) &&
            projectPersistence_.ownsTransientWav(path)) {
            pendingObsoleteReferencePaths_.insert(path);
        }
    }
}

void MainWindow::retryObsoleteReferenceWavs()
{
    const QSet<QString> pending = std::move(pendingObsoleteReferencePaths_);
    pendingObsoleteReferencePaths_.clear();
    discardObsoleteReferenceWavs(pending);
}

void MainWindow::discardObsoletePreparedMixPaths()
{
    const QSet<QString> obsolete = preparedMixLifecycle_.takeObsoletePaths();
    for (const QString& path : obsolete) {
        if (!projectPersistence_.discardTransientWav(path) &&
            projectPersistence_.ownsTransientWav(path)) {
            preparedMixLifecycle_.retainObsoletePath(path);
        }
    }
}

void MainWindow::discardPreparedMix(bool replacementExpected)
{
    const bool globalTransportRequested =
        trackRecordingWorkflow_.globalTransportRequestedPlaying();
    const bool resumePlayback = replacementExpected &&
        (trackController_.playback().requestedPlaying || globalTransportRequested);
    if (replacementExpected) {
        trackController_.prepareMix(0, resumePlayback);
    } else {
        trackController_.requestPlayback(globalTransportRequested);
    }
    preparedMixLifecycle_.setPlayWhenReady(resumePlayback);
    trackRecordingWorkflow_.cancelPreparedAttach();
    if (jam2_.isRunning()) {
        jam2::EngineCommand unload;
        unload.type = jam2::EngineCommandType::UnloadPreparedTrack;
        if (!submitEngineCommand(unload, QStringLiteral("unload prepared track"))) {
            appendLog(QStringLiteral(
                "engine command queue unavailable: unload prepared track"));
        }
    }
    const int activeBank = looperProject_.activeBankIndex();
    const QString obsoletePath =
        preparedMixLifecycle_.clearActiveAndCache(activeBank);
    if (!obsoletePath.isEmpty()) {
        if (!projectPersistence_.discardTransientWav(obsoletePath) &&
            projectPersistence_.ownsTransientWav(obsoletePath)) {
            preparedMixLifecycle_.retainObsoletePath(obsoletePath);
        }
    }
    discardObsoletePreparedMixPaths();
    auto& track = trackController_.model();
    track.fileName = replacementExpected
        ? QStringLiteral("Preparing backing tracks")
        : QStringLiteral("No generated reference WAVs");
    track.filePath.clear();
    track.fileBytes = 0;
    track.durationMs = 0;
    track.sha256.clear();
    if (trackWaveform_) trackWaveform_->clear();
    updateTrackControls();
}

bool MainWindow::clearPracticeReferenceWavs(bool rebuildRemainingTracks, int bankIndex)
{
    QSet<QString> referencePaths;
    bool hadReferences = false;
    for (int index = 0; index < looperProject_.banks().size(); ++index) {
        if (bankIndex >= 0 && index != bankIndex) continue;
        const LooperBank& bank = looperProject_.banks().at(index);
        for (const LooperLane& lane : bank.lanes) {
            if (!isManagedPracticeReference(lane)) {
                continue;
            }
            hadReferences = true;
            if (!lane.assetPath.trimmed().isEmpty()) {
                referencePaths.insert(looperAssetAbsolutePath(lane));
            }
        }
    }
    if (!hadReferences) {
        return false;
    }
    if (bankIndex >= 0) {
        jam2::practice::PracticeIdeaController::clearReferences(looperProject_, bankIndex);
        preparedMixLifecycle_.invalidateBank(qBound(
            0, bankIndex, looperProject_.banks().size() - 1));
    } else {
        jam2::practice::PracticeIdeaController::clearReferences(looperProject_);
        preparedMixLifecycle_.invalidateAll();
    }
    discardObsoleteReferenceWavs(referencePaths);
    if (bankIndex >= 0 && bankIndex != looperProject_.activeBankIndex()) {
        if (rebuildRemainingTracks &&
            !looperProject_.banks().at(bankIndex).lanes.isEmpty()) {
            regeneratePreparedMix(bankIndex);
        }
        discardObsoletePreparedMixPaths();
        return true;
    }
    const bool hasRemainingActiveTracks =
        !looperProject_.banks().at(looperProject_.activeBankIndex()).lanes.isEmpty();
    if (rebuildRemainingTracks && hasRemainingActiveTracks) {
        discardPreparedMix(true);
        regeneratePreparedMix(looperProject_.activeBankIndex());
    } else {
        discardPreparedMix(false);
    }
    return true;
}

void MainWindow::cleanupTransientTrackWavs()
{
    if (!trackRecordingWorkflow_.lastCapturePath().isEmpty() &&
        !QFileInfo::exists(trackRecordingWorkflow_.lastCapturePath())) {
        trackRecordingWorkflow_.clearLastCapturePath();
    }
    projectPersistence_.scheduleTransientCleanup(fileWorkerPool_);
}


void MainWindow::sendSongSnapshot(
    std::optional<bool> trackPlayingOverride,
    SongSyncScope scope)
{
    const QString scopeName = scope == SongSyncScope::IdeaFull
        ? QStringLiteral("idea.full") : scope == SongSyncScope::IdeaChords
            ? QStringLiteral("idea.chords") : scope == SongSyncScope::IdeaBeats
                ? QStringLiteral("idea.beats") : QStringLiteral("tracks");
    QJsonObject policyProbe{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("sync_scope"), scopeName},
    };
    if (!jamSyncAllowsControlMessage(policyProbe)) {
        appendLog(QStringLiteral("suppressed %1 snapshot by the jam sync policy")
            .arg(scopeName));
        return;
    }
    const SharedSessionController::Snapshot before = sessionController_.snapshot();
    if (before.role != SharedSessionController::Role::Creator &&
        before.role != SharedSessionController::Role::Joiner) {
        return;
    }
    const QJsonObject snapshotSong = songToJson(true);
    if (sessionController_.isServer() &&
        !trackWorkspace_.authoritativeTrackHistory.contains(before.arrangementRevision)) {
        trackWorkspace_.authoritativeTrackHistory.insert(
            before.arrangementRevision, snapshotSong);
    }
    const int revision = looperArrangementRevision_ + 1;
    if (!sessionController_.isServer()) {
        const QJsonObject proposal{
            {QStringLiteral("type"), QStringLiteral("song.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("arrangement_revision"), 0},
            {QStringLiteral("base_arrangement_revision"),
                static_cast<qint64>(before.arrangementRevision)},
            {QStringLiteral("host_authoritative"), false},
            {QStringLiteral("sync_scope"), scopeName},
            {QStringLiteral("track_playing"), trackPlayingOverride.value_or(
                trackRecordingWorkflow_.globalTransportRequestedPlaying())},
            {QStringLiteral("song"), snapshotSong},
        };
        const qsizetype bytes = QJsonDocument(proposal).toJson(QJsonDocument::Compact).size();
        if (bytes > jam2::control_protocol::kMaxLargeJsonBytes) {
            appendLog(QStringLiteral(
                "collaborative arrangement exceeds the authenticated JSON limit: bytes=%1 limit=%2")
                .arg(bytes)
                .arg(jam2::control_protocol::kMaxLargeJsonBytes));
            return;
        }
        if (!sessionController_.send(proposal)) {
            appendLog(QStringLiteral(
                "could not queue collaborative arrangement edit: bytes=%1 base_revision=%2")
                .arg(bytes)
                .arg(before.arrangementRevision));
            return;
        }
        looperArrangementRevision_ = revision;
        appendLog(QStringLiteral(
            "submitted collaborative arrangement edit: bytes=%1 base_revision=%2")
            .arg(bytes)
            .arg(before.arrangementRevision));
        return;
    }
    const bool trackPlaying = trackPlayingOverride.value_or(
        trackRecordingWorkflow_.globalTransportRequestedPlaying());
    // Arrangement publication never gates the UDP transport. Peers without a
    // prepared WAV follow the same global playhead and attach their local
    // render on a later beat when it becomes available.

    const QJsonObject authoritative{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("arrangement_revision"), revision},
        {QStringLiteral("host_authoritative"), true},
        {QStringLiteral("sync_scope"), scopeName},
        {QStringLiteral("track_playing"), trackPlaying},
        {QStringLiteral("song"), snapshotSong},
    };
    const qsizetype bytes = QJsonDocument(authoritative)
        .toJson(QJsonDocument::Compact).size();
    if (bytes > jam2::control_protocol::kMaxLargeJsonBytes) {
        appendLog(QStringLiteral(
            "authoritative arrangement exceeds the authenticated JSON limit: bytes=%1 limit=%2")
            .arg(bytes)
            .arg(jam2::control_protocol::kMaxLargeJsonBytes));
        return;
    }
    (void)sessionController_.send(authoritative);
    const SharedSessionController::Snapshot after = sessionController_.snapshot();
    looperArrangementRevision_ = revision;
    trackWorkspace_.authoritativeTrackHistory.insert(
        static_cast<quint64>(revision), snapshotSong);
    while (trackWorkspace_.authoritativeTrackHistory.size() > 16) {
        trackWorkspace_.authoritativeTrackHistory.erase(
            trackWorkspace_.authoritativeTrackHistory.begin());
    }
    trackController_.requestPlayback(trackPlaying, after.arrangementRevision);
    updateTrackPlaybackPresentation();
}

void MainWindow::refreshSongViews()
{
    if (songTitleEdit_) {
        songTitleEdit_->setText(chordModel_.title());
    }
    if (chordGrid_) {
        chordGrid_->refresh();
    }
    if (beatGrid_) {
        beatGrid_->refresh();
    }
    if (lyricGrid_) {
        lyricGrid_->refresh();
    }
}

void MainWindow::refreshSongView(const QString& lane)
{
    (void)lane;
    refreshSongViews();
    refreshLooperLanes();
}

void MainWindow::generatePracticeIdea()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral(
            "idea generation is unavailable while a track take is active"));
        return;
    }
    jam2::practice::PracticeIdeaDialogDefaults defaults;
    defaults.parts = static_cast<jam2::practice::PracticeIdeaParts>(
        qBound(0, preferences_.ideas.parts, 2));
    defaults.key = preferences_.ideas.key;
    defaults.styleId = preferences_.ideas.styleId;
    defaults.profileId = preferences_.ideas.profileId;
    defaults.preferredMeterId = preferences_.ideas.meterId;
    defaults.preferredBars = preferences_.ideas.bars;
    defaults.exactBpm = preferences_.ideas.exactBpm;
    defaults.complexity = preferences_.ideas.complexity;
    defaults.targetSectionIndex = chordGrid_
        ? chordGrid_->selectedSectionIndex()
        : 0;
    if (defaults.targetSectionIndex < 0 ||
        defaults.targetSectionIndex >= chordModel_.sections().size()) {
        defaults.targetSectionIndex = 0;
    }
    const auto pattern = bankMetronomePattern(defaults.targetSectionIndex);
    defaults.bpm = pattern.bpm;
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        const auto bankPattern = bankMetronomePattern(bank);
        defaults.bankBpms.push_back(bankPattern.bpm);
        defaults.bankMeterIds.push_back(practiceMeterIdForPattern(bankPattern));
        const int beats = bank < chordModel_.sections().size()
            ? chordModel_.section(bank).beats : bankPattern.beats_per_bar;
        defaults.bankBars.push_back(qMax(
            1,
            (beats + bankPattern.beats_per_bar - 1) /
                bankPattern.beats_per_bar));
    }
    if (!chordModel_.sections().isEmpty()) {
        const auto& section = chordModel_.section(defaults.targetSectionIndex);
        defaults.meterId = practiceMeterIdForPattern(pattern);
        const int beats = section.beats;
        defaults.bars = qMax(
            1,
            (beats + qMax(1, pattern.beats_per_bar) - 1) /
                qMax(1, pattern.beats_per_bar));
    }
    if (preferences_.ideas.exactBpm) defaults.bpm = preferences_.ideas.bpm;
    const auto request = jam2::practice::askForPracticeIdea(this, defaults);
    if (!request) {
        return;
    }
    if (!applyPracticeIdea(*request)) {
        QMessageBox::warning(this, QStringLiteral("Generate Practice Idea"),
            QStringLiteral("The song has reached its section limit."));
    }
}

void MainWindow::browseCuratedIdeas()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral(
            "groove library importing is unavailable while a track take is active"));
        return;
    }
    QString catalogError;
    if (jam2::practice::loadCuratedIdeaCatalog(catalogError).isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Groove Library"),
            catalogError.isEmpty()
                ? QStringLiteral("The embedded groove library is empty.")
                : catalogError);
        return;
    }

    jam2::practice::CuratedIdeaDialogDefaults defaults;
    defaults.targetSectionIndex = qBound(
        0, viewedBankIndex_, looperProject_.banks().size() - 1);
    defaults.timing = preferences_.ideas.grooveUseIdeaTiming
        ? jam2::practice::CuratedIdeaTimingPolicy::UseIdeaTiming
        : jam2::practice::CuratedIdeaTimingPolicy::KeepSectionTiming;
    defaults.importBars = preferences_.ideas.grooveBars;
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        const auto pattern = bankMetronomePattern(bank);
        defaults.bankBpms.push_back(pattern.bpm);
        defaults.bankMeterNumerators.push_back(pattern.beats_per_bar);
        defaults.bankMeterDenominators.push_back(pattern.beat_unit);
        defaults.bankBeats.push_back(
            bank < chordModel_.sections().size()
                ? chordModel_.section(bank).beats
                : pattern.beats_per_bar);
    }

    const jam2::EngineSnapshot engine = jam2_.engineSnapshot();
    jam2::practice::CuratedIdeaPreviewCallbacks preview;
    preview.available = jam2_.isRunning() &&
        !engine.prepared_source_playing &&
        !sharedRecordingProtected();
    preview.unavailableReason = !jam2_.isRunning()
        ? QStringLiteral("Start audio to hear previews. Grooves can still be imported.")
        : engine.prepared_source_playing
            ? QStringLiteral("Stop backing-track playback to hear previews. Grooves can still be imported.")
            : QStringLiteral("Preview is unavailable while recording.");
    preview.play = [this](const jam2::practice::CuratedIdeaEntry& entry, QString& error) {
        return playCuratedIdeaPreview(entry, error);
    };
    preview.stop = [this] { stopCuratedIdeaPreview(); };

    const auto selection = jam2::practice::askForCuratedIdea(this, defaults, preview);
    if (!selection) return;

    jam2::practice::GeneratedPracticeIdea idea =
        jam2::practice::generateCoupledPracticeIdeaSeeded(
            selection->idea.generationRequest(selection->targetSectionIndex),
            selection->idea.seed);
    if (!idea.recipe.isValid() ||
        idea.recipe.generatorVersion != selection->idea.generatorVersion ||
        jam2::practice::generatedChordFingerprint(idea.chordSection) !=
            selection->idea.chordFingerprint ||
        jam2::practice::generatedBeatFingerprint(idea.beatSection) !=
            selection->idea.beatFingerprint) {
        QMessageBox::warning(
            this,
            QStringLiteral("Groove Library"),
            QStringLiteral(
                "This groove no longer matches its embedded preview. Regenerate the curated library before importing it."));
        appendLog(QStringLiteral("curated groove rejected: id=%1 generator=%2")
            .arg(selection->idea.id)
            .arg(idea.recipe.generatorVersion));
        return;
    }

    const int sourceBeats = idea.beatSection.beats;
    const int targetBeats = selection->importBars > 0
        ? selection->importBars * idea.meterNumerator
        : defaults.bankBeats.value(selection->targetSectionIndex, sourceBeats);
    idea.beatSection = jam2::practice::PracticeIdeaController::fitRepeatingDrums(
        std::move(idea.beatSection), targetBeats);

    const bool applied = applyPracticeIdea(
        std::move(idea),
        jam2::practice::PracticeIdeaParts::DrumsOnly,
        selection->targetSectionIndex,
        selection->timing == jam2::practice::CuratedIdeaTimingPolicy::UseIdeaTiming,
        true);
    if (!applied) {
        QMessageBox::warning(
            this,
            QStringLiteral("Groove Library"),
            QStringLiteral("The selected groove could not be imported into that section."));
        return;
    }
    appendLog(QStringLiteral(
        "curated groove imported: id=%1 section=%2 material=drums timing=%3 source_beats=%4 imported_beats=%5 requested_bars=%6")
        .arg(selection->idea.id)
        .arg(QChar(QLatin1Char('A').unicode() + selection->targetSectionIndex))
        .arg(selection->timing == jam2::practice::CuratedIdeaTimingPolicy::UseIdeaTiming
            ? QStringLiteral("idea") : QStringLiteral("kept"))
        .arg(sourceBeats)
        .arg(targetBeats)
        .arg(selection->importBars > 0
            ? QString::number(selection->importBars) : QStringLiteral("current")));
}

bool MainWindow::playCuratedIdeaPreview(
    const jam2::practice::CuratedIdeaEntry& idea,
    QString& error)
{
    error.clear();
    if (!jam2_.isRunning()) {
        error = QStringLiteral("Start audio to hear previews.");
        return false;
    }
    const jam2::EngineSnapshot engine = jam2_.engineSnapshot();
    if (!curatedIdeaPreviewActive_ && engine.prepared_source_playing) {
        error = QStringLiteral("Stop backing-track playback before previewing an idea.");
        return false;
    }
    stopCuratedIdeaPreview();

    QFile resource(idea.previewResource);
    if (!resource.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("The embedded preview could not be opened.");
        return false;
    }
    const QByteArray data = resource.readAll();
    const QString actualHash = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    if (actualHash.compare(idea.previewSha256, Qt::CaseInsensitive) != 0) {
        error = QStringLiteral("The embedded preview failed its SHA-256 check.");
        return false;
    }

    // Keep extracted previews with Jam2's other managed data. The private GUI
    // agent redirects this root to its build-local storage, so native tests do
    // not leave cache artifacts in the user's profile.
    const QString cacheRoot = appReleaseFolderPath(
        QStringLiteral("cache/idea-previews"));
    if (!QDir().mkpath(cacheRoot)) {
        error = QStringLiteral("The preview cache folder could not be created.");
        return false;
    }
    const QString embeddedPath = QDir(cacheRoot).absoluteFilePath(
        idea.id + QStringLiteral("-") + idea.previewSha256.left(12) + QStringLiteral(".wav"));
    if (!QFile::exists(embeddedPath)) {
        QSaveFile output(embeddedPath);
        if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() ||
            !output.commit()) {
            error = QStringLiteral("The embedded preview could not be extracted.");
            return false;
        }
    }
    const StagedPcm16Asset staged = stagePcm16Asset(
        embeddedPath,
        cacheRoot,
        activeTrackSampleRate(),
        QStringLiteral("active-rate"));
    if (!staged.error.isEmpty()) {
        error = staged.error;
        return false;
    }

    jam2::EngineCommand load;
    load.type = jam2::EngineCommandType::LoadPreparedTrack;
    load.enabled = true;
    if (!jam2::engine_command_set_text(
            load,
            QDir::toNativeSeparators(staged.stagedPath).toStdString()) ||
        !submitEngineCommand(load, QStringLiteral("load curated idea preview"))) {
        error = QStringLiteral("The audio engine could not queue the preview.");
        return false;
    }
    curatedIdeaPreviewActive_ = true;
    setPreparedTrackLoop(
        true,
        0,
        static_cast<std::uint64_t>(staged.metadata.frames));
    submitEngineGain(
        jam2::EngineCommandType::PreparedSetLevel,
        1.0,
        QStringLiteral("curated idea preview level"));
    appendLog(QStringLiteral(
        "curated preview loaded: id=%1 frames=%2 sample_rate=%3 resampled=%4 preview_level=1.0000 level_control=master-output-only")
        .arg(idea.id)
        .arg(staged.metadata.frames)
        .arg(staged.metadata.sampleRate)
        .arg(staged.resampled ? QStringLiteral("yes") : QStringLiteral("no")));
    return true;
}

void MainWindow::stopCuratedIdeaPreview()
{
    if (!curatedIdeaPreviewActive_) return;
    jam2::EngineCommand stop;
    stop.type = jam2::EngineCommandType::PreparedStop;
    (void)submitEngineCommand(stop, QStringLiteral("stop curated idea preview"));
    curatedIdeaPreviewActive_ = false;
    if (!preparedMixLifecycle_.active().path.isEmpty() &&
        preparedMixLifecycle_.active().error.isEmpty()) {
        loadPreparedMixIntoEngine();
    } else {
        jam2::EngineCommand unload;
        unload.type = jam2::EngineCommandType::UnloadPreparedTrack;
        (void)submitEngineCommand(unload, QStringLiteral("unload curated idea preview"));
    }
}

void MainWindow::continuePracticeIdea()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral(
            "idea continuation is unavailable while a track take is active"));
        return;
    }
    jam2::practice::ContinueIdeaDialogDefaults defaults;
    const int bankCount = looperProject_.banks().size();
    defaults.sourceSectionIndex = qBound(0, viewedBankIndex_, bankCount - 1);
    for (int bank = 0; bank < bankCount; ++bank) {
        const SongSection& section = chordModel_.section(bank);
        defaults.bankHasContent.push_back(
            jam2::practice::PracticeIdeaController::referenceLayers(section).any());
        defaults.bankNames.push_back(section.name);
    }
    defaults.targetSectionIndex = (defaults.sourceSectionIndex + 1) % bankCount;
    for (int offset = 1; offset < bankCount; ++offset) {
        const int candidate = (defaults.sourceSectionIndex + offset) % bankCount;
        if (!defaults.bankHasContent.value(candidate)) {
            defaults.targetSectionIndex = candidate;
            break;
        }
    }
    auto request = jam2::practice::askForIdeaContinuation(this, defaults);
    if (!request) return;

    const auto sourcePattern = bankMetronomePattern(request->sourceSectionIndex);
    request->bpm = sourcePattern.bpm;
    request->meterId = practiceMeterIdForPattern(sourcePattern);
    request->beatsPerBar = sourcePattern.beats_per_bar;
    request->beatUnit = sourcePattern.beat_unit;
    request->tempoPulseUnits = sourcePattern.tempo_pulse_units;
    const auto continuation =
        jam2::practice::PracticeIdeaController::generateContinuation(
            chordModel_, *request);
    if (!continuation) {
        QMessageBox::warning(this, QStringLiteral("Continue Idea"),
            QStringLiteral("The source section needs musical material and the target section must be different."));
        return;
    }

    ++practiceIdeaRevision_;
    const int targetBank = qBound(
        0, request->targetSectionIndex, looperProject_.banks().size() - 1);
    const bool affectsLiveBank = targetBank == looperProject_.activeBankIndex();
    if (affectsLiveBank) stopTrackForPracticeIdeaGeneration();
    clearPracticeReferenceWavs(false, targetBank);
    LooperBankTiming continuationTiming =
        looperProject_.resolvedTiming(request->sourceSectionIndex);
    continuationTiming.inheritsBankA = false;
    (void)looperProject_.setTiming(targetBank, std::move(continuationTiming));
    if (affectsLiveBank) {
        applyMetronomePatternForBank(targetBank);
        updateTrackMetronomeInterval();
    }
    selectViewedBank(targetBank);
    refreshSongViews();
    refreshLooperLanes();
    if (jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Off) {
        const SongSyncScope scope = jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Chords
            ? SongSyncScope::IdeaChords
            : jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Beats
                ? SongSyncScope::IdeaBeats : SongSyncScope::IdeaFull;
        sendSongSnapshot(
            affectsLiveBank ? std::optional<bool>(false) : std::nullopt,
            scope);
    }
    const auto& analysis = continuation->analysis;
    appendLog(QStringLiteral(
        "continued Section %1 into Section %2 as %3 using %4/%5; key confidence=%6%, profile=%7 (%8%); chord-root overlap=%9%, chord-symbol overlap=%10%, order contrast=%11%, first-four-position similarity=%12%, drums=%13%, melody rhythm=%14%, melody contour=%15%, bass contour=%16%, boundary=%17%, harmonic density=%18% (%19->%20 changes)")
        .arg(QChar(QLatin1Char('A').unicode() + request->sourceSectionIndex))
        .arg(QChar(QLatin1Char('A').unicode() + targetBank))
        .arg(analysis.continuationRoleName)
        .arg(analysis.relationshipId)
        .arg(analysis.harmonicPacingId)
        .arg(qRound(analysis.inferredTonalConfidence * 100.0))
        .arg(analysis.inferredProfileId)
        .arg(qRound(analysis.inferredProfileConfidence * 100.0))
        .arg(qRound(analysis.chordVocabularySimilarity * 100.0))
        .arg(qRound(analysis.chordQualityVocabularySimilarity * 100.0))
        .arg(qRound(analysis.chordOrderContrast * 100.0))
        .arg(qRound(analysis.openingChordPositionSimilarity * 100.0))
        .arg(qRound(analysis.drumSimilarity * 100.0))
        .arg(qRound(analysis.melodyRhythmSimilarity * 100.0))
        .arg(qRound(analysis.melodyContourSimilarity * 100.0))
        .arg(qRound(analysis.bassContourSimilarity * 100.0))
        .arg(qRound(analysis.boundaryVoiceLeading * 100.0))
        .arg(qRound(analysis.harmonicDensityRetention * 100.0))
        .arg(analysis.sourceChordEvents)
        .arg(analysis.continuationChordEvents));
}

void MainWindow::clearPracticeIdea()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral(
            "idea clearing is unavailable while a track take is active"));
        return;
    }
    const int viewedBank = qBound(
        0, viewedBankIndex_, looperProject_.banks().size() - 1);
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(QStringLiteral("Clear Idea"));
    prompt.setText(QStringLiteral("Clear Section %1 only, or clear ideas from every section?")
        .arg(QChar(QLatin1Char('A').unicode() + viewedBank)));
    prompt.setInformativeText(QStringLiteral(
        "Generated reference WAVs will also be removed. Custom tracks are retained."));
    QPushButton* currentBankButton = prompt.addButton(
        QStringLiteral("Current Section"), QMessageBox::AcceptRole);
    QPushButton* allBanksButton = prompt.addButton(
        QStringLiteral("All Sections"), QMessageBox::DestructiveRole);
    QAbstractButton* cancelButton = prompt.addButton(QMessageBox::Cancel);
    jam2::gui::registerGuiControl(
        *currentBankButton,
        QStringLiteral("idea.clear-dialog.current-section"),
        QStringLiteral("idea.clear"),
        jam2::gui::GuiControlAvailability::Modal);
    jam2::gui::registerGuiControl(
        *allBanksButton,
        QStringLiteral("idea.clear-dialog.all-sections"),
        QStringLiteral("idea.clear"),
        jam2::gui::GuiControlAvailability::Modal);
    jam2::gui::registerGuiControl(
        *cancelButton,
        QStringLiteral("idea.clear-dialog.cancel"),
        QStringLiteral("idea.clear"),
        jam2::gui::GuiControlAvailability::Modal);
    prompt.setDefaultButton(currentBankButton);
    prompt.exec();
    if (prompt.clickedButton() != currentBankButton &&
        prompt.clickedButton() != allBanksButton) {
        return;
    }

    ++practiceIdeaRevision_;
    const bool clearAllBanks = prompt.clickedButton() == allBanksButton;
    const int activeBank = looperProject_.activeBankIndex();
    if (clearAllBanks || viewedBank == activeBank) {
        stopTrackForPracticeIdeaGeneration();
    }
    if (clearAllBanks) {
        for (int bank = 0; bank < chordModel_.sections().size(); ++bank) {
            chordModel_.clearSection(bank);
        }
    } else {
        chordModel_.clearSection(viewedBank);
    }
    const bool removedReferences = clearPracticeReferenceWavs(
        true, clearAllBanks ? -1 : viewedBank);
    const int firstResetBank = clearAllBanks ? 1 : viewedBank;
    const int lastResetBank = clearAllBanks ? looperProject_.banks().size() - 1 : viewedBank;
    for (int bank = firstResetBank; bank <= lastResetBank; ++bank) {
        if (bank > 0 && looperProject_.banks().at(bank).lanes.isEmpty()) {
            LooperBankTiming inherited = looperProject_.resolvedTiming(0);
            inherited.inheritsBankA = true;
            (void)looperProject_.setTiming(bank, std::move(inherited));
            if (bank == activeBank) {
                applyMetronomePatternForBank(bank);
            }
        }
    }
    refreshSongViews();
    refreshLooperLanes();
    if (performanceHome_) {
        performanceHome_->update();
    }
    if (jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Off) {
        const SongSyncScope syncScope =
            jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Chords
            ? SongSyncScope::IdeaChords
            : jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Beats
                ? SongSyncScope::IdeaBeats : SongSyncScope::IdeaFull;
        sendSongSnapshot(std::nullopt, syncScope);
    }
    const QString scope = clearAllBanks
        ? QStringLiteral("all sections")
        : QStringLiteral("Section %1").arg(
            QChar(QLatin1Char('A').unicode() + viewedBank));
    appendLog(removedReferences
        ? QStringLiteral("cleared idea and generated reference tracks from %1; custom tracks retained").arg(scope)
        : QStringLiteral("cleared idea from %1; custom tracks retained").arg(scope));
}

bool MainWindow::applyPracticeIdea(const jam2::practice::ChordIdeaRequest& request)
{
    jam2::practice::GeneratedPracticeIdea idea =
        jam2::practice::generateCoupledPracticeIdea(request);
    return applyPracticeIdea(
        std::move(idea),
        request.parts,
        request.targetSectionIndex,
        true,
        true);
}

bool MainWindow::applyPracticeIdea(
    jam2::practice::GeneratedPracticeIdea idea,
    jam2::practice::PracticeIdeaParts parts,
    int targetSectionIndex,
    bool useIdeaTiming,
    bool matchIdeaLength)
{
    if (sharedRecordingProtected()) return false;
    const auto previousPattern = currentMetronomePattern();
    const auto appliedIdea = jam2::practice::PracticeIdeaController::applyCoupled(
        chordModel_,
        beatModel_,
        std::move(idea),
        parts,
        targetSectionIndex,
        matchIdeaLength);
    if (!appliedIdea) {
        return false;
    }
    ++practiceIdeaRevision_;
    const int targetBank = qBound(
        0, targetSectionIndex, looperProject_.banks().size() - 1);
    const bool affectsLiveBank = targetBank == looperProject_.activeBankIndex();
    const bool timingChanged = useIdeaTiming && affectsLiveBank &&
        (appliedIdea->bpm != previousPattern.bpm ||
         appliedIdea->meterNumerator != previousPattern.beats_per_bar ||
         appliedIdea->meterDenominator != previousPattern.beat_unit ||
         appliedIdea->tempoPulseUnits != previousPattern.tempo_pulse_units);
    if (affectsLiveBank || timingChanged) stopTrackForPracticeIdeaGeneration();
    clearPracticeReferenceWavs(false, targetBank);
    if (useIdeaTiming) {
        LooperBankTiming generatedTiming;
        generatedTiming.bpm = appliedIdea->bpm;
        generatedTiming.beatsPerBar = appliedIdea->meterNumerator;
        generatedTiming.beatUnit = appliedIdea->meterDenominator;
        generatedTiming.tempoPulseUnits = appliedIdea->tempoPulseUnits;
        generatedTiming.division = appliedIdea->clickDivision;
        generatedTiming.playMaskLow = 0;
        generatedTiming.playMaskHigh = 0;
        generatedTiming.accentMaskLow = 0;
        generatedTiming.accentMaskHigh = 0;
        for (int step = 0; step < appliedIdea->clickEnabled.size(); ++step) {
            jam2::metronome::set_mask_enabled(
                generatedTiming.playMaskLow,
                generatedTiming.playMaskHigh,
                step,
                appliedIdea->clickEnabled.at(step));
            jam2::metronome::set_mask_enabled(
                generatedTiming.accentMaskLow,
                generatedTiming.accentMaskHigh,
                step,
                step < appliedIdea->clickAccents.size() && appliedIdea->clickAccents.at(step));
        }
        generatedTiming.inheritsBankA = false;
        (void)looperProject_.setTiming(targetBank, std::move(generatedTiming));
    }
    if (useIdeaTiming && affectsLiveBank) {
        applyMetronomePatternForBank(targetBank);
        updateTrackMetronomeInterval();
    }
    selectViewedBank(targetBank);
    refreshSongViews();
    refreshLooperLanes();
    if (jamSyncPolicy_.generatedIdeas != GeneratedIdeaSyncMode::Off) {
        const SongSyncScope syncScope =
            jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Chords
            ? SongSyncScope::IdeaChords
            : jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Beats
                ? SongSyncScope::IdeaBeats : SongSyncScope::IdeaFull;
        sendSongSnapshot(
            affectsLiveBank || timingChanged
                ? std::optional<bool>(false) : std::nullopt,
            syncScope);
    }
    return true;
}

void MainWindow::stopTrackForPracticeIdeaGeneration()
{
    trackWorkspace_.cancelPendingTrackPlayback();
    if (jam2_.isRunning()) {
        jam2::EngineCommand cancelTransport;
        cancelTransport.type = jam2::EngineCommandType::CancelTransport;
        (void)submitEngineCommand(
            cancelTransport,
            QStringLiteral("cancel track transport before practice idea generation"));
        if (!trackRecordingWorkflow_.stopPrepared(
                0,
                metronomeTransport_.grid().position().currentFrame)) {
            appendLog(QStringLiteral(
                "engine command queue unavailable: stop track before practice idea generation"));
        }
    }
    updateTrackPlaybackPresentation();
    updateTrackTimeline();
}

void MainWindow::generatePracticeReferenceWavs()
{
    if (sharedRecordingProtected()) {
        appendLog(QStringLiteral(
            "reference WAV generation is unavailable while a track take is active"));
        return;
    }
    if (referenceWavGenerationRunning_) {
        appendLog(QStringLiteral("reference WAV generation is already running"));
        return;
    }

    const int bankCount = looperProject_.banks().size();
    const int sectionCount = qMin(chordModel_.sections().size(), bankCount);
    if (sectionCount <= 0) {
        QMessageBox::information(this, QStringLiteral("Generate Reference WAVs"),
            QStringLiteral("Add a song section first."));
        return;
    }
    if (chordModel_.sections().size() > bankCount) {
        QMessageBox::information(this, QStringLiteral("Generate Reference WAVs"),
            QStringLiteral(
                "Only Sections A-D can be rendered. Later sections will not be rendered yet."));
    }

    QVector<jam2::practice::ReferenceLayerAvailability> availableLayers;
    availableLayers.reserve(sectionCount);
    bool anyChords = false;
    bool anyDrums = false;
    bool anyMelody = false;
    bool anyBass = false;
    bool anySupport = false;
    int maxBeats = 0;
    for (int index = 0; index < sectionCount; ++index) {
        const SongSection& section = chordModel_.section(index);
        const auto available =
            jam2::practice::PracticeIdeaController::referenceLayers(section);
        availableLayers.push_back(available);
        anyChords = anyChords || available.chords;
        anyDrums = anyDrums || available.drums;
        anyMelody = anyMelody || available.melody;
        anyBass = anyBass || available.bass;
        anySupport = anySupport || available.support;
        maxBeats = qMax(maxBeats, section.beats);
    }
    if (!anyChords && !anyDrums && !anyMelody && !anyBass && !anySupport) {
        QMessageBox::information(this, QStringLiteral("Generate Reference WAVs"),
            QStringLiteral("Add chords, beat hits, melody, bass, or supporting-line notes to the first four sections."));
        return;
    }

    const auto pattern = currentMetronomePattern();
    jam2::practice::ReferenceRenderSettings defaults;
    defaults.renderChords = anyChords && preferences_.ideas.renderChords;
    defaults.renderDrums = anyDrums && preferences_.ideas.renderDrums;
    defaults.renderMelody = anyMelody && preferences_.ideas.renderMelody;
    defaults.renderBass = anyBass && preferences_.ideas.renderBass;
    defaults.renderSupport = anySupport && preferences_.ideas.renderSupport;
    defaults.voicing = static_cast<jam2::practice::ChordVoicing>(
        qBound(0, preferences_.ideas.chordVoicing, 3));
    defaults.drumKit = static_cast<jam2::practice::ReferenceDrumKit>(
        qBound(0, preferences_.ideas.drumKit, 2));
    defaults.bpm = pattern.bpm;
    defaults.sampleRate = activeTrackSampleRate();
    defaults.tempoPulseUnits = pattern.tempo_pulse_units;
    if (!chordModel_.sections().isEmpty() &&
        chordModel_.section(0).generatedRecipe.isValid()) {
        defaults.meterNumerator =
            chordModel_.section(0).generatedRecipe.meterNumerator;
        defaults.meterDenominator =
            chordModel_.section(0).generatedRecipe.meterDenominator;
        defaults.tempoPulseUnits =
            chordModel_.section(0).generatedRecipe.tempoPulseUnits;
    }
    const auto settings = jam2::practice::askForReferenceRender(
        this, defaults,
        anyChords ? maxBeats : 0,
        anyDrums ? maxBeats : 0,
        anyMelody ? maxBeats : 0,
        anyBass ? maxBeats : 0,
        anySupport ? maxBeats : 0,
        sectionCount);
    if (!settings) {
        return;
    }

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject request{
        {QStringLiteral("type"), QStringLiteral("practice.references.render")},
        {QStringLiteral("request_id"), requestId},
        {QStringLiteral("render_signature"),
            QString::fromLatin1(practiceReferenceRenderSignature())},
        {QStringLiteral("render_chords"), settings->renderChords},
        {QStringLiteral("render_drums"), settings->renderDrums},
        {QStringLiteral("render_melody"), settings->renderMelody},
        {QStringLiteral("render_bass"), settings->renderBass},
        {QStringLiteral("render_support"), settings->renderSupport},
        {QStringLiteral("chord_voicing"), static_cast<int>(settings->voicing)},
        {QStringLiteral("drum_kit"), static_cast<int>(settings->drumKit)},
    };
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (automaticWavSharingEnabled() && session.remotePeerCount > 0) {
        sendControl(request);
    }
    handlePracticeReferenceRenderRequest(request, QString{}, true);
}

QByteArray MainWindow::practiceReferenceRenderSignature() const
{
    QJsonObject state = chordModel_.toJson();
    QJsonArray timing;
    for (int bank = 0; bank < looperProject_.banks().size(); ++bank) {
        const LooperBankTiming value = looperProject_.resolvedTiming(bank);
        timing.append(QJsonObject{
            {QStringLiteral("bpm"), value.bpm},
            {QStringLiteral("beats_per_bar"), value.beatsPerBar},
            {QStringLiteral("beat_unit"), value.beatUnit},
            {QStringLiteral("tempo_pulse_units"), value.tempoPulseUnits},
        });
    }
    state.insert(QStringLiteral("render_timing"), timing);
    return QCryptographicHash::hash(
        QJsonDocument(state).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
}

void MainWindow::handlePracticeReferenceRenderRequest(
    const QJsonObject& message,
    const QString& sourcePeerToken,
    bool localRequest)
{
    if (!localRequest && !automaticWavSharingEnabled()) {
        appendLog(QStringLiteral(
            "ignored shared reference WAV render while automatic WAV sharing is disabled"));
        return;
    }
    const QString requestId = message.value(QStringLiteral("request_id")).toString();
    if (handledReferenceRenderRequests_.contains(requestId)) {
        return;
    }
    const QByteArray expectedSignature =
        message.value(QStringLiteral("render_signature")).toString().toLatin1();
    if (expectedSignature != practiceReferenceRenderSignature()) {
        if (deferredReferenceRenderRequests_.size() >= 16) {
            deferredReferenceRenderRequests_.erase(
                deferredReferenceRenderRequests_.begin());
        }
        deferredReferenceRenderRequests_.insert(
            requestId, qMakePair(message, sourcePeerToken));
        if (performanceHome_) {
            performanceHome_->setWavGenerationActive(true);
        }
        appendLog(QStringLiteral(
            "deferred shared reference WAV render %1 until its idea revision is applied")
            .arg(requestId.left(8)));
        return;
    }
    deferredReferenceRenderRequests_.remove(requestId);
    if (referenceWavGenerationRunning_) {
        appendLog(QStringLiteral(
            "ignored overlapping shared reference WAV render %1; a render is already running")
            .arg(requestId.left(8)));
        return;
    }
    if (handledReferenceRenderRequests_.size() >= 64) {
        handledReferenceRenderRequests_.erase(handledReferenceRenderRequests_.begin());
    }
    handledReferenceRenderRequests_.insert(requestId);

    if (!localRequest && sessionController_.isServer() && !sourcePeerToken.isEmpty()) {
        (void)sendControl(message);
    }

    jam2::practice::ReferenceRenderSettings settings;
    settings.renderChords = message.value(QStringLiteral("render_chords")).toBool();
    settings.renderDrums = message.value(QStringLiteral("render_drums")).toBool();
    settings.renderMelody = message.value(QStringLiteral("render_melody")).toBool();
    settings.renderBass = message.value(QStringLiteral("render_bass")).toBool();
    settings.renderSupport = message.value(QStringLiteral("render_support")).toBool();
    settings.voicing = static_cast<jam2::practice::ChordVoicing>(
        message.value(QStringLiteral("chord_voicing")).toInt());
    settings.drumKit = static_cast<jam2::practice::ReferenceDrumKit>(
        message.value(QStringLiteral("drum_kit")).toInt());
    settings.sampleRate = activeTrackSampleRate();
    startPracticeReferenceWavGeneration(settings, requestId);
}

void MainWindow::startDeferredPracticeReferenceRenders()
{
    const auto pending = deferredReferenceRenderRequests_;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (it.value().first.value(QStringLiteral("render_signature"))
                .toString().toLatin1() == practiceReferenceRenderSignature()) {
            handlePracticeReferenceRenderRequest(it.value().first, it.value().second);
            break;
        }
    }
    if (deferredReferenceRenderRequests_.isEmpty() &&
        !referenceWavGenerationRunning_ && performanceHome_) {
        performanceHome_->setWavGenerationActive(false);
    }
}

void MainWindow::startPracticeReferenceWavGeneration(
    const jam2::practice::ReferenceRenderSettings& requestedSettings,
    const QString& requestId)
{
    const int sectionCount = qMin(
        chordModel_.sections().size(), looperProject_.banks().size());
    QVector<jam2::practice::ReferenceLayerAvailability> availableLayers;
    availableLayers.reserve(sectionCount);
    for (int index = 0; index < sectionCount; ++index) {
        availableLayers.push_back(
            jam2::practice::PracticeIdeaController::referenceLayers(
                chordModel_.section(index)));
    }
    const auto settings = std::optional<jam2::practice::ReferenceRenderSettings>{
        requestedSettings};

    struct SectionRender {
        int bankIndex = 0;
        QString bankId;
        SongSection section;
        jam2::practice::ReferenceRenderSettings settings;
        jam2::practice::ReferenceRenderResult result;
    };
    struct RenderState {
        QVector<SectionRender> sections;
        QString error;
        std::uint64_t ideaRevision = 0;
        int songRevision = 0;
    };
    auto state = std::make_shared<RenderState>();
    state->sections.reserve(sectionCount);
    for (int index = 0; index < sectionCount; ++index) {
        SectionRender render;
        render.bankIndex = index;
        render.bankId = looperProject_.banks().at(index).id;
        if (render.bankId.trimmed().isEmpty()) {
            render.bankId = QString(QChar(static_cast<ushort>('A' + index)));
        }
        render.section = chordModel_.section(index);
        render.settings = *settings;
        const auto bankPattern = bankMetronomePattern(index);
        render.settings.bpm = bankPattern.bpm;
        render.settings.meterNumerator = bankPattern.beats_per_bar;
        render.settings.meterDenominator = bankPattern.beat_unit;
        render.settings.tempoPulseUnits = bankPattern.tempo_pulse_units;
        render.settings.renderChords =
            settings->renderChords && availableLayers[index].chords;
        render.settings.renderDrums =
            settings->renderDrums && availableLayers[index].drums;
        render.settings.renderMelody =
            settings->renderMelody && availableLayers[index].melody;
        render.settings.renderBass =
            settings->renderBass && availableLayers[index].bass;
        render.settings.renderSupport =
            settings->renderSupport && availableLayers[index].support;
        state->sections.push_back(std::move(render));
    }
    state->ideaRevision = practiceIdeaRevision_;
    state->songRevision = chordModel_.revision();
    const QString workspace = projectPersistence_.workspaceFolder();
    const bool started = startFileWorkerTask(
        [state, workspace] {
            for (SectionRender& render : state->sections) {
                if (!render.settings.renderChords &&
                    !render.settings.renderDrums &&
                    !render.settings.renderMelody &&
                    !render.settings.renderBass &&
                    !render.settings.renderSupport) {
                    continue;
                }
                render.result = jam2::practice::renderPracticeReferences(
                    render.settings.renderChords || render.settings.renderMelody ||
                        render.settings.renderBass || render.settings.renderSupport
                        ? &render.section : nullptr,
                    render.settings.renderDrums ? &render.section : nullptr,
                    render.settings,
                    workspace);
                if (!render.result.error.isEmpty()) {
                    state->error = QStringLiteral("Section %1 (%2): %3")
                        .arg(render.bankIndex + 1)
                        .arg(render.bankId, render.result.error);
                    break;
                }
            }
        },
        [this, state, requestId] {
            referenceWavGenerationRunning_ = false;
            if (performanceHome_) {
                performanceHome_->setWavGenerationActive(false);
            }
            const auto discardRenderedResults = [&state] {
                for (const SectionRender& render : state->sections) {
                    if (!render.result.chords.path.isEmpty()) QFile::remove(render.result.chords.path);
                    if (!render.result.drums.path.isEmpty()) QFile::remove(render.result.drums.path);
                    if (!render.result.melody.path.isEmpty()) QFile::remove(render.result.melody.path);
                    if (!render.result.bass.path.isEmpty()) QFile::remove(render.result.bass.path);
                    if (!render.result.support.path.isEmpty()) QFile::remove(render.result.support.path);
                }
            };
            if (state->ideaRevision != practiceIdeaRevision_ ||
                state->songRevision != chordModel_.revision()) {
                discardRenderedResults();
                appendLog(QStringLiteral(
                    "discarded reference WAV render superseded by a song edit"));
                return;
            }
            if (!state->error.isEmpty()) {
                discardRenderedResults();
                QMessageBox::warning(this, QStringLiteral("Generate Reference WAVs"), state->error);
                return;
            }

            QSet<QString> previousReferencePaths;
            for (const LooperBank& bank : looperProject_.banks()) {
                for (const LooperLane& lane : bank.lanes) {
                    if (isManagedPracticeReference(lane) &&
                        !lane.assetPath.trimmed().isEmpty()) {
                        previousReferencePaths.insert(looperAssetAbsolutePath(lane));
                    }
                }
            }

            LooperProject stagedProject = looperProject_;
            jam2::practice::PracticeIdeaController::clearReferences(stagedProject);
            for (const SectionRender& render : state->sections) {
                if (!render.settings.renderChords &&
                    !render.settings.renderDrums &&
                    !render.settings.renderMelody &&
                    !render.settings.renderBass &&
                    !render.settings.renderSupport) {
                    continue;
                }
                QString applyError;
                if (!jam2::practice::PracticeIdeaController::applyReferences(
                        stagedProject,
                        render.bankIndex,
                        render.settings,
                        render.result,
                        applyError,
                        QStringLiteral("Section %1").arg(render.bankId))) {
                    discardRenderedResults();
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Generate Reference WAVs"),
                        QStringLiteral("Section %1 (%2): %3")
                            .arg(render.bankIndex + 1)
                            .arg(render.bankId, applyError));
                    return;
                }
            }
            looperProject_ = std::move(stagedProject);
            for (const SectionRender& render : state->sections) {
                if (render.settings.renderChords) {
                    registerTransientTrackWav(render.result.chords.path);
                }
                if (render.settings.renderDrums) {
                    registerTransientTrackWav(render.result.drums.path);
                }
                if (render.settings.renderMelody) {
                    registerTransientTrackWav(render.result.melody.path);
                }
                if (render.settings.renderBass) {
                    registerTransientTrackWav(render.result.bass.path);
                }
                if (render.settings.renderSupport) {
                    registerTransientTrackWav(render.result.support.path);
                }
                if (!render.result.diagnostics.isEmpty()) {
                    appendLog(QStringLiteral("Section %1: %2")
                        .arg(render.bankId, render.result.diagnostics));
                }
            }
            discardObsoleteReferenceWavs(previousReferencePaths);
            preparedMixLifecycle_.invalidateAll();
            discardPreparedMix(true);
            refreshLooperLanes();
            regeneratePreparedMix(looperProject_.activeBankIndex());
            appendLog(QStringLiteral(
                "%1 song section(s) rendered into local Sections A-%2 for shared request %3")
                .arg(state->sections.size())
                .arg(state->sections.constLast().bankId)
                .arg(requestId.left(8)));
        },
        [this](const QString& error) {
            referenceWavGenerationRunning_ = false;
            if (performanceHome_) {
                performanceHome_->setWavGenerationActive(false);
            }
            QMessageBox::warning(
                this,
                QStringLiteral("Generate Reference WAVs"),
                QStringLiteral("Reference WAV generation failed: %1").arg(error));
        });
    if (started) {
        referenceWavGenerationRunning_ = true;
        if (performanceHome_) {
            performanceHome_->setWavGenerationActive(true);
        }
    }
    if (!started) {
        QMessageBox::warning(this, QStringLiteral("Generate Reference WAVs"),
            QStringLiteral("The reference render worker is currently busy."));
    }
}

void MainWindow::showPracticeIdeaDetails()
{
    const std::optional<SongSection> chord =
        jam2::practice::PracticeIdeaController::generatedSection(chordModel_, QStringLiteral("chord"));
    const std::optional<SongSection> beat =
        jam2::practice::PracticeIdeaController::generatedSection(beatModel_, QStringLiteral("beat"));
    const SongSection* source = chord ? &*chord : (beat ? &*beat : nullptr);
    if (!source || !source->generatedRecipe.isValid()) {
        QMessageBox::information(this, QStringLiteral("Idea Details"),
            QStringLiteral("Generate an idea first."));
        return;
    }
    const jam2::practice::GenerationRecipe& recipe = source->generatedRecipe;
    const bool changed =
        (chord && jam2::practice::generatedChordFingerprint(*chord) != recipe.chordFingerprint) ||
        (beat && jam2::practice::generatedBeatFingerprint(*beat) != recipe.beatFingerprint);
    jam2::practice::showIdeaDetails(this, recipe, changed);
}

QString MainWindow::sessionHex() const
{
    return sessionToHex(sessionId_);
}

QString MainWindow::keyHex() const
{
    return keyToHex(sessionKey_);
}

void MainWindow::generateSession()
{
    sessionId_ = jam2::random_u64();
    sessionKey_ = jam2::random_key();
}
