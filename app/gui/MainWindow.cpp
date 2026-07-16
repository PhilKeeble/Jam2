#include "MainWindow.hpp"
#include "MainWindowPages.hpp"
#include "TrackWidgets.hpp"
#include "TrackWorkspaceSupport.hpp"
#include "GuiPresentation.hpp"
#include "GuiControlMessageRouter.hpp"

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
#include "tuning_profile.hpp"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <QList>
#include <QMessageBox>
#include <QMetaObject>
#include <QNetworkInterface>
#include <QIODevice>
#include <QProgressDialog>
#include <QPointer>
#include <QRunnable>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QUrl>
#include <QSlider>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QScrollArea>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryFile>
#include <QTimer>
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
constexpr int kFirewallGuidanceDisconnectThreshold = 3;
constexpr int kFirewallGuidanceWindowMs = 10000;

QString creatorFirewallGuidance()
{
#if defined(__APPLE__)
    return QStringLiteral(
        "Jam2 accepted incoming TCP connections, but they closed before authentication began. "
        "macOS Firewall may be blocking Jam2.\n\n"
        "Open System Settings > Network > Firewall > Options and set Jam2 to Allow incoming "
        "connections, then ask the peer to retry.");
#elif defined(_WIN32)
    return QStringLiteral(
        "Jam2 accepted incoming TCP connections, but they closed before authentication began. "
        "Windows Firewall or other network security software on either computer may be blocking "
        "the connection.\n\n"
        "Open Windows Security > Firewall & network protection > Allow an app through firewall, "
        "allow Jam2 on the active network, then ask the peer to retry.");
#else
    return QStringLiteral(
        "Jam2 accepted incoming TCP connections, but they closed before authentication began. "
        "Check firewall and network security settings on both computers, then ask the peer to retry.");
#endif
}

QString joinerFirewallGuidance()
{
    return QStringLiteral(
        "\n\nNo authenticated TCP control connection was established. Confirm that the creator is "
        "still hosting and that the invite address is correct. Also check that Jam2 is allowed "
        "through macOS Firewall or Windows Firewall on the creator's computer and through any "
        "third-party network security software on both computers.");
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








QString timestampedCapturePath(const QString& prefix)
{
    return appReleaseFilePath(
        QStringLiteral("captures"),
        QStringLiteral("%1-%2-%3.wav")
            .arg(
                prefix,
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")),
                QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));
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

QString timestampedJamRecordingFolder()
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    return appReleaseFolderPath(QStringLiteral("jam_recordings/%1").arg(stamp));
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
    if (renderOffsetFrames >= 0) {
        const std::uint64_t offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return musicalFrame > offset ? musicalFrame - offset : 0ULL;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(-(renderOffsetFrames + 1)) + 1ULL;
    return musicalFrame > (std::numeric_limits<std::uint64_t>::max)() - offset
        ? (std::numeric_limits<std::uint64_t>::max)()
        : musicalFrame + offset;
}

std::uint64_t musicalFrameFromRawFrame(std::uint64_t rawFrame, std::int64_t renderOffsetFrames)
{
    if (renderOffsetFrames >= 0) {
        const std::uint64_t offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return rawFrame > (std::numeric_limits<std::uint64_t>::max)() - offset
            ? (std::numeric_limits<std::uint64_t>::max)()
            : rawFrame + offset;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(-(renderOffsetFrames + 1)) + 1ULL;
    return rawFrame > offset ? rawFrame - offset : 0ULL;
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

bool promptFrame(QWidget* parent, const QString& title, const QString& label, qint64 current, qint64& out)
{
    bool accepted = false;
    const QString text = QInputDialog::getText(
        parent,
        title,
        label,
        QLineEdit::Normal,
        current >= 0 ? QString::number(current) : QString(),
        &accepted);
    if (!accepted) {
        return false;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {

        out = -1;
        return true;
    }
    bool ok = false;
    const qint64 parsed = trimmed.toLongLong(&ok);
    if (!ok || parsed < 0) {
        QMessageBox::warning(parent, title, QStringLiteral("Frame values must be empty or non-negative integers."));
        return false;
    }
    out = parsed;
    return true;
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






QJsonObject readSidecarJson(const QString& wavPath)
{
    QFile file(wavPath + QStringLiteral(".json"));
    constexpr qint64 kMaxSidecarBytes = 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > kMaxSidecarBytes) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};

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



bool isWheelValueEditor(QObject* object)
{
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        if (qobject_cast<QAbstractSpinBox*>(current) ||
            qobject_cast<QAbstractSlider*>(current) ||
            qobject_cast<QComboBox*>(current) ||
            className.contains(QStringLiteral("QComboBox"))) {
            return true;
        }
    }
    return false;
}

QScrollArea* parentScrollArea(QObject* object)
{
    auto* widget = qobject_cast<QWidget*>(object);
    while (widget != nullptr) {
        if (auto* scrollArea = qobject_cast<QScrollArea*>(widget)) {
            return scrollArea;
        }
        widget = widget->parentWidget();
    }
    return nullptr;
}

void scrollAreaByWheel(QScrollArea& scrollArea, QWheelEvent& wheel)
{
    QScrollBar* bar = scrollArea.verticalScrollBar();
    if (bar == nullptr) {
        return;
    }
    int delta = wheel.pixelDelta().y();
    if (delta == 0) {
        delta = wheel.angleDelta().y() / 8;
    }
    if (delta == 0) {
        return;
    }
    bar->setValue(bar->value() - delta);
}

} // namespace


MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
    , trackWorkspace_(jam2_, this)
    , trackController_(trackWorkspace_.trackController)
    , looperProject_(trackWorkspace_.looperProject)
    , assetTransfer_(trackWorkspace_.assetTransfer)
    , metronomeTransport_(jam2_)
    , trackRecordingWorkflow_(trackWorkspace_.recordingWorkflow)
    , chordModel_(trackWorkspace_.chordModel)
    , beatModel_(trackWorkspace_.beatModel)
    , lyricModel_(trackWorkspace_.lyricModel)
    , projectPersistence_(trackWorkspace_.persistence)
    , preparedMix_(trackWorkspace_.preparedMix)
    , fileWorkerPool_(trackWorkspace_.fileWorkers)
    , preparedMixWorkerRunning_(trackWorkspace_.preparedMixWorkerRunning)
    , preparedMixRerunPending_(trackWorkspace_.preparedMixRerunPending)
    , playPreparedMixWhenReady_(trackWorkspace_.playPreparedMixWhenReady)
    , pendingPreparedTrackReadyRevision_(trackWorkspace_.pendingPreparedTrackReadyRevision)
    , pendingSharedTrackRevision_(trackWorkspace_.pendingSharedTrackRevision)
    , pendingSharedTrackHostReady_(trackWorkspace_.pendingSharedTrackHostReady)
    , pendingSharedTrackReadyTokens_(trackWorkspace_.pendingSharedTrackReadyTokens)
    , publishStoppedTrackStateWhenApplied_(trackWorkspace_.publishStoppedTrackStateWhenApplied)
    , preparedMixRequests_(trackWorkspace_.preparedMixRequests)
    , preparedMixCoalesced_(trackWorkspace_.preparedMixCoalesced)
    , preparedMixFailures_(trackWorkspace_.preparedMixFailures)
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
        [this](const QString& message) { appendLog(message); },
        [this](const QString& token, qint64 bytes) { return canQueueControlTo(token, bytes); },
        [this](const QString& token, const QJsonObject& message) {
            return sendControlTo(token, message);
        },
        [this](const QString& token, const QByteArray& payload) {
            return sendBinaryControlTo(token, payload);
        },
        [this] {
            applyPendingTrackContributions();
            requestNextPendingTrackAsset();
            applyPendingSongIfAssetsReady();
        },
    });
    installJam2Style();
    generateSession();
    projectPersistence_.initializeWorkspace(appReleaseFolderPath(
        QStringLiteral("song_staging/") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    MainWindowPages::build(*this);
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
    jam2_.onNetworkSnapshot = [this](const jam2::NetworkSessionSnapshot& snapshot) {
        handleNetworkSnapshot(snapshot);
    };
    jam2_.onStartup = [this](const Jam2RuntimeStartup& startup) {
        jamStartupPending_ = false;
        const QString local = QString::fromStdString(jam2::endpoint_to_string(startup.local_endpoint));
        appendLog(QStringLiteral("native UDP session ready: ") + local);
        if (startup.public_candidate) {
            activePublicEndpoint_ = QString::fromStdString(jam2::endpoint_to_string(*startup.public_candidate));
            if (publicHostEdit_) {
                publicHostEdit_->setText(QString::fromStdString(startup.public_candidate->host));

            }
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
            sendMetronomeSettingsToPeer();
            if (looperProject_.trackSyncEnabled()) {
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
        GuiControlMessageRouter::dispatch({
            [this] { return looperProject_.trackSyncEnabled(); },
            [this](const QString& text) { appendLog(text); },
            [this](const QString& text) {
                QMessageBox::warning(this, QStringLiteral("Jam2"), text);
            },
            [this](const QJsonObject& value) { applyRemoteMetronomeSettings(value); },
            &chordModel_,
            &beatModel_,
            &lyricModel_,
            [this](const QString& lane) { refreshSongView(lane); },
            [this](const QJsonObject& value, const QString& source) {
                handleSongSet(value, source);
            },
            [this](const QJsonObject& value, const QString& source) {
                handleTrackReady(value, source);
            },
            [this] { shareLocalTracks(); },
            [this](const QJsonObject& value, const QString& source) {
                handleTrackOffer(value, source);
            },
            &assetTransfer_,
        }, message, sourcePeerToken);
    };
    sessionController_.onBinaryMessage = [this](
        const QString& sourcePeerToken,
        const QByteArray& payload) {
        assetTransfer_.receiveChunk(payload, sourcePeerToken);
    };
    sessionController_.onPeerAuthenticated = [this](const QString& token, const QJsonObject& message) {
        handleMeshPeerAuthenticated(token, message);
    };
    sessionController_.onPeerDisconnected = [this](const QString& token) {
        assetTransfer_.peerDisconnected(token);
        localMeshPeerTokens_.remove(token);
        updateMixRemotePeers();
    };
    sessionController_.onContract = [this](const SharedSessionController::SessionContract& contract) {
        appendLog(QStringLiteral("session contract protocol=%1 format=%2 profile=%3 sample_rate=%4 frame_size=%5")
            .arg(contract.protocolVersion)
            .arg(contract.audioFormat, contract.profile)
            .arg(contract.sampleRate)
            .arg(contract.frameSize));
        if (sessionController_.snapshot().role == SharedSessionController::Role::Joiner) {
            if (profileBox_) {
                const QSignalBlocker blocker(profileBox_);

                const int index = profileBox_->findData(contract.profile);
                if (index >= 0) {
                    profileBox_->setCurrentIndex(index);
                }
            }
            sampleRateSpin_->setValue(contract.sampleRate);
            frameSizeSpin_->setValue(contract.frameSize);
            if (networkAudioFormatBox_) {
                const QSignalBlocker blocker(networkAudioFormatBox_);
                const int index = networkAudioFormatBox_->findData(contract.audioFormat);
                if (index >= 0) {
                    networkAudioFormatBox_->setCurrentIndex(index);
                }
            }
        }
        auditWavCompatibilityForSession(contract.sampleRate, true);
    };
    sessionController_.onSnapshot = [this](const SharedSessionController::Snapshot& snapshot) {
        applySessionSnapshot(snapshot);
    };
    sessionController_.bindRuntime(
        jam2_,
        [this](const SharedSessionController::Snapshot& snapshot) {
            if (snapshot.role == SharedSessionController::Role::Joiner &&
                !selectedDeviceSupportsSampleRate(snapshot.contract.sampleRate)) {
                throw std::runtime_error(
                    QStringLiteral("Selected audio device does not support session sample rate %1")
                        .arg(snapshot.contract.sampleRate).toStdString());
            }
            return networkRuntimeOptions(snapshot);
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
    QObject::connect(&playbackGridTimer_, &QTimer::timeout, this, [this] { updatePlaybackGrid(); });
    playbackGridTimer_.start();
    trackTimelineTimer_.setInterval(33);
    QObject::connect(&trackTimelineTimer_, &QTimer::timeout, this, [this] { updateTrackTimeline(); });
    trackTimelineTimer_.start();
}

MainWindow::~MainWindow()
{
    shuttingDown_ = true;
    fileWorkerPool_.waitForDone();
    QApplication::instance()->removeEventFilter(this);
    stopJam(false);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
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
    }
    shuttingDown_ = true;
    stopJam(false);
    if (loopbackRecorder_.isRunning()) {
        loopbackRecorder_.stop();
    }
    if (!trackRecordingWorkflow_.pendingTransientCapturePath().isEmpty()) {
        registerTransientTrackWav(trackRecordingWorkflow_.abandonPendingCapture());
    }
    cleanupTransientTrackWavs();
    event->accept();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel && isWheelValueEditor(watched)) {
        if (auto* scrollArea = parentScrollArea(watched)) {
            scrollAreaByWheel(*scrollArea, *static_cast<QWheelEvent*>(event));
            return true;
        }
        return false;
    }
    return QWidget::eventFilter(watched, event);
}













void MainWindow::updateMixControls()

{
    const bool jamActive = jam2_.isRunning();
    auto setModeEnabled = [](QWidget* widget, bool enabled) {
        if (widget != nullptr) {
            widget->setEnabled(enabled);
        }
    };

    setModeEnabled(mixJamRecordingRow_, jamActive);
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
}

void MainWindow::setMixRemotePeerVisible(bool visible)
{
    (void)visible;
    updateMixRemotePeers();
    updateTrackControls();
}

void MainWindow::updateMixRemotePeers()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    const bool networkSession = role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner;
    QStringList peers;
    if (networkSession) {
        peers = meshPeerEndpointsExcludingSelf();
    }

    if (mixRemotePeerListLayout_) {
        while (QLayoutItem* item = mixRemotePeerListLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (int index = 0; index < peers.size(); ++index) {
            auto* label = new QLabel(
                networkSession
                    ? QStringLiteral("Peer %1 - %2").arg(index + 1).arg(peers.at(index))
                    : peers.at(index),
                mixRemotePeerRow_);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            mixRemotePeerListLayout_->addWidget(label);
        }
    }

    const bool visible = !peers.isEmpty();
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
    const QString folder = timestampedJamRecordingFolder();
    QDir().mkpath(folder);
    if (!trackRecordingWorkflow_.startJamRecording(folder)) {
        appendLog(QStringLiteral("engine command queue unavailable: start jam recording"));
        return;
    }
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
    if (jamRecordingButton_) {
        jamRecordingButton_->setEnabled(jam2_.isRunning());
        jamRecordingButton_->setText(trackRecordingWorkflow_.jamRecordingActive()

            ? QStringLiteral("Stop Recording Jam")
            : QStringLiteral("Start Recording Jam"));
    }
    if (jamRecordingLabel_) {
        if (trackRecordingWorkflow_.jamRecordingActive() ||
            !trackRecordingWorkflow_.jamRecordingFolder().isEmpty()) {
            jamRecordingLabel_->setText(
                QFileInfo(trackRecordingWorkflow_.jamRecordingFolder()).fileName());
        } else {
            jamRecordingLabel_->setText(QStringLiteral("Stopped"));
        }
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
    const QString host = publicHostEdit_ && !publicHostEdit_->text().trimmed().isEmpty()
        ? publicHostEdit_->text().trimmed()
        : (bindHostEdit_ && !bindHostEdit_->text().trimmed().isEmpty()
            ? bindHostEdit_->text().trimmed()
            : QStringLiteral("127.0.0.1"));
    jam2::SessionInfo info;
    info.endpoint = {host.toStdString(), static_cast<std::uint16_t>(portSpin_ ? portSpin_->value() : 49000)};
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
    showJamReadyInviteDialog(this, inviteUrl);
}

void MainWindow::updateConnectionControlState()
{
    const bool manualEndpoint = noStunCheck_ && noStunCheck_->isChecked();
    if (publicHostEdit_) {
        publicHostEdit_->setEnabled(manualEndpoint);
    }
    if (stunServerEdit_) {
        stunServerEdit_->setEnabled(!manualEndpoint);
    }
    if (stunTimeoutSpin_) {
        stunTimeoutSpin_->setEnabled(!manualEndpoint);
    }
    if (stunRetriesSpin_) {
        stunRetriesSpin_->setEnabled(!manualEndpoint);
    }
}

void MainWindow::showLocalPerformSetup()
{
    if (jam2_.isRunning()) {
        return;
    }
    refreshDevices();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Start Local Engine"));
    auto* form = new QFormLayout(&dialog);
    auto* device = new QComboBox(&dialog);
    for (int index = 0; deviceBox_ && index < deviceBox_->count(); ++index) {
        device->addItem(deviceBox_->itemText(index), deviceBox_->itemData(index));
    }
    if (deviceBox_) {
        device->setCurrentIndex(qMax(0, device->findData(deviceBox_->currentData())));
    }
    auto* sampleRate = new QSpinBox(&dialog);
    sampleRate->setRange(
        jam2::application::limits::kMinimumSampleRate,
        jam2::application::limits::kMaximumSampleRate);
    sampleRate->setValue(sampleRateSpin_ ? sampleRateSpin_->value() : 48000);
    auto* inputChannels = new QLineEdit(inputChannelsEdit_ ? inputChannelsEdit_->text() : QStringLiteral("0"), &dialog);
    auto* outputChannels = new QLineEdit(outputChannelsEdit_ ? outputChannelsEdit_->text() : QStringLiteral("0,1"), &dialog);
    form->addRow(QStringLiteral("Low-latency device"), device);
    form->addRow(QStringLiteral("Sample rate"), sampleRate);
    form->addRow(QStringLiteral("Input channels"), inputChannels);
    form->addRow(QStringLiteral("Output channels"), outputChannels);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Start Engine"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (device->currentData().toString().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Perform"), QStringLiteral("Select a low-latency audio device first."));
        return;
    }
    deviceBox_->setCurrentIndex(qMax(0, deviceBox_->findData(device->currentData())));
    sampleRateSpin_->setValue(sampleRate->value());
    inputChannelsEdit_->setText(inputChannels->text().trimmed());
    outputChannelsEdit_->setText(outputChannels->text().trimmed());
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
    if (engineModeLabel_) {
        engineModeLabel_->setText(QStringLiteral("Local"));
    }
    pendingJamRuntimeError_.clear();
    try {
        launchLocalRuntime(runtimeOptions());
    } catch (const std::exception& error) {
        const QString detail = pendingJamRuntimeError_.isEmpty()
            ? QString::fromUtf8(error.what())
            : pendingJamRuntimeError_;
        pendingJamRuntimeError_.clear();
        QMessageBox::warning(this, QStringLiteral("Local engine failed"), detail);
        QTimer::singleShot(0, this, [this] {
            if (!shuttingDown_ && !jam2_.isRunning()) {
                showLocalPerformSetup();
            }
        });
    }
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
    if (engineModeLabel_) {
        engineModeLabel_->setText(QStringLiteral("Network"));
    }
    jamStartupPending_ = true;
    jamStartupCreating_ = createSession;
    pendingJamRuntimeError_.clear();
    lastJamFailureDialog_.clear();
    queuedJamFailureDetail_.clear();
    jamFailureDialogQueued_ = false;
    preAuthenticationDisconnectWindow_.invalidate();
    preAuthenticationDisconnectCount_ = 0;
    firewallGuidanceShown_ = false;
    jam2_.setTrackSyncEnabled(looperProject_.trackSyncEnabled());
    activePublicEndpoint_.clear();
    lastLoggedSessionSummary_.clear();
    localMeshPeerTokens_.clear();

    pendingTrackContributions_.clear();
    appliedTrackContributionIds_.clear();
    localTrackOffers_.clear();
    trackOfferAssetPaths_.clear();
    pendingTrackAssetSources_.clear();
    validatedTrackAssetHashes_.clear();
    prepareNetworkRuntimePresentation(createSession);
    try {
        if (createSession) {
            meshPeerEndpoints_.clear();
            meshPeerEndpoints_[meshPeerToken()] = localMeshEndpoint(true);
            if (!sessionController_.startCreator(SharedSessionController::CreatorConfig{
                    static_cast<quint16>(portSpin_->value()),
                    sessionHex(),
                    keyHex(),
                    meshPeerToken(),
                    localMeshEndpoint(true),
                    meshMaxPeersSpin_ ? meshMaxPeersSpin_->value() : 0,
                    SharedSessionController::SessionContract{
                        jam2::protocol::kProtocolVersion,
                        networkAudioFormatBox_
                            ? networkAudioFormatBox_->currentData().toString()
                            : QStringLiteral("pcm24-mono"),
                        profileBox_ ? profileBox_->currentData().toString() : QStringLiteral("fast"),
                        sampleRateSpin_->value(),
                        frameSizeSpin_->value(),
                    }})) {
                const QString error = QStringLiteral("control server failed: ") + sessionController_.errorString();
                appendLog(error);
                showJamFailure(error);
                stopJam(true);
                return;
            }
            pendingMeshInvitePopup_ = true;
        } else {
            const std::string url = connectUrlEdit_->text().toStdString();
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
        if (connectionLabel_) {
            connectionLabel_->setText(action);
        }
        QMessageBox::critical(this, action, lastJamFailureDialog_);
    });
}

void MainWindow::launchLocalRuntime(Jam2RuntimeOptions options)
{
    if (sessionTopologyLabel_) {
        sessionTopologyLabel_->setText(QStringLiteral("Remote Peers 0"));
        sessionTopologyLabel_->setToolTip(QString{});
    }
    if (!sessionController_.startLocal(options)) {
        throw std::runtime_error("local engine failed to start");
    }
    appendLog(QStringLiteral("local engine active through typed runtime"));
    startButton_->setEnabled(true);
    joinButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    if (networkAudioFormatBox_) {
        networkAudioFormatBox_->setEnabled(true);
    }
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(false);
    }
    connectionLabel_->setText(QStringLiteral("Starting Perform"));
    QTimer::singleShot(250, this, [this] {
        updateRuntimeControls();
        sendMetronomeModeToJam();
        sendMetronomePatternToJam();
    });
}

void MainWindow::prepareNetworkRuntimePresentation(bool createSession)
{
    if (createSession) {
        metronomeTransport_.setLocalState(false, false);
    }
    metronomeTransport_.clearEngine();
    if (createSession && trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Jam metronome stopped"));
    }
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
    if (networkAudioFormatBox_) {
        networkAudioFormatBox_->setEnabled(false);
    }
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(true);
    }
    trackRecordingWorkflow_.clearJamRecordingState();
    updateJamRecordingControls();
    updateMixControls();
    connectionLabel_->setText(
        sessionController_.isConnected()
            ? QStringLiteral("TCP peer authenticated")
            : QStringLiteral("Starting"));
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

void MainWindow::submitEngineFrame(
    jam2::EngineCommandType type,
    std::uint64_t frame,
    const QString& context)
{
    jam2::EngineCommand command;
    command.type = type;
    command.frame = frame;
    (void)submitEngineCommand(command, context);
}

void MainWindow::submitEngineText(
    jam2::EngineCommandType type,
    const QString& text,
    const QString& context)
{
    jam2::EngineCommand command;
    command.type = type;
    if (!jam2::engine_command_set_text(command, text.toStdString())) {
        appendLog(QStringLiteral("engine command text is too long: ") + context);
        return;
    }
    (void)submitEngineCommand(command, context);
}

void MainWindow::seekPreparedTrack(std::uint64_t sourceFrame, std::uint64_t targetFrame)
{
    if (!trackRecordingWorkflow_.seekPrepared(sourceFrame, targetFrame)) {
        appendLog(QStringLiteral("engine command queue unavailable: prepared track seek"));
    }
}

void MainWindow::setPreparedTrackLoop(bool enabled, std::uint64_t startFrame, std::uint64_t endFrame)
{
    if (!trackRecordingWorkflow_.setPreparedLoop(enabled, startFrame, endFrame)) {
        appendLog(QStringLiteral("engine command queue unavailable: prepared track loop"));
    }
}

std::uint64_t MainWindow::quantizedEngineTarget(int countInBars) const
{
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    if (!position.engineAnchored || position.sampleRate <= 0) {
        return 0;
    }
    const int beatsPerBar = qMax(1, currentMetronomePattern().beats_per_bar);
    const std::uint64_t nextBarBeat =
        ((position.absoluteBeat / static_cast<std::uint64_t>(beatsPerBar)) +
         1ULL + static_cast<std::uint64_t>(qMax(0, countInBars))) *
        static_cast<std::uint64_t>(beatsPerBar);
    const std::uint64_t musical = position.epochFrame + static_cast<std::uint64_t>(std::llround(
        static_cast<double>(nextBarBeat) * position.secondsPerBeat * static_cast<double>(position.sampleRate)));
    return rawFrameFromMusicalFrame(musical, position.renderOffsetFrames);
}

void MainWindow::restartPreparedTrackQuantized()
{
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    if (!trackRecordingWorkflow_.restartPrepared(
            position,
            currentMetronomePattern().beats_per_bar,
            looperProject_.trackSyncEnabled())) {
        appendLog(QStringLiteral("engine command queue unavailable: prepared track restart"));
    }
}

void MainWindow::handleEngineSnapshot(const jam2::EngineSnapshot& snapshot)
{
    const MetronomeTransportController::SnapshotUpdate transportUpdate =
        metronomeTransport_.consume(snapshot);
    trackRecordingWorkflow_.consumeSnapshot(snapshot, transportUpdate);
    if (trackController_.observeEnginePlaying(trackRecordingWorkflow_.preparedPlaying())) {
        updateTrackPlaybackPresentation();
    }
    if (recordingLatencyAdjustmentSpin_) {
        const QSignalBlocker blocker(recordingLatencyAdjustmentSpin_);
        recordingLatencyAdjustmentSpin_->setValue(static_cast<int>(qBound<qint64>(
            static_cast<qint64>(recordingLatencyAdjustmentSpin_->minimum()),
            snapshot.recording_latency_adjustment_frames,
            static_cast<qint64>(recordingLatencyAdjustmentSpin_->maximum()))));
    }
    updateRecordingLatencyDisplay();
    if (const std::optional<int> bars = trackRecordingWorkflow_.takeReadyPendingCountIn(snapshot)) {
        startInputCapture(0, *bars);
    }
    if (publishStoppedTrackStateWhenApplied_ && !trackRecordingWorkflow_.preparedPlaying() &&
        sessionController_.isServer()) {
        publishStoppedTrackStateWhenApplied_ = false;
        sendSongSnapshot(false);
    }

    updateMixMeters(mixerStatsViewModel_.consume(
        jam2_.consumeGuiPeaks(),
        static_cast<double>(snapshot.send_level_ppm) / 1000000.0,
        snapshot.output_clipped_samples));

    updatePlaybackGrid();
}

void MainWindow::handleEngineEvent(const jam2::EngineEvent& event)
{
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
    if (trackRecordingWorkflow_.consumeJamRecordingEvent(event)) {
        updateJamRecordingControls();
    }
    const TrackRecordingWorkflow::TrackTakeCompletion completion =
        trackRecordingWorkflow_.consumeTrackTakeEvent(event);
    if (completion.handled) {
        if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
        if (recordingCountdownLabel_) recordingCountdownLabel_->hide();
        if (completion.ok && !completion.wavPath.isEmpty()) {
            registerTransientTrackWav(completion.wavPath);
            importLastCaptureToArmedLane();
        }
        if (loadWavButton_) loadWavButton_->setEnabled(true);
        appendLog(completion.ok
            ? QStringLiteral("track take stopped: take_id=%1 wav=%2 frames=%3")
                .arg(completion.takeId, completion.wavPath).arg(completion.frames)
            : QStringLiteral("track take error: take_id=%1 reason=%2")
                .arg(completion.takeId, completion.error));
    }
}


void MainWindow::handleNetworkSnapshot(const jam2::NetworkSessionSnapshot& snapshot)
{
    QJsonObject stats{
        {QStringLiteral("event"), QStringLiteral("mesh_stats")},
        {QStringLiteral("peer_count"), static_cast<double>(snapshot.peers.size())},
        {QStringLiteral("network_active_peer_count"), static_cast<double>(snapshot.mix.active_peers)},
        {QStringLiteral("mix_contributing_peers"), static_cast<double>(snapshot.mix.contributing_peers)},
        {QStringLiteral("mix_released_slots"), static_cast<double>(snapshot.mix.released_slots)},
        {QStringLiteral("mix_deadline_slots"), static_cast<double>(snapshot.mix.deadline_slots)},
        {QStringLiteral("mix_missing_peer_frames"), static_cast<double>(snapshot.mix.missing_peer_frames)},

        {QStringLiteral("mix_capacity_drops"), static_cast<double>(snapshot.mix.capacity_drops)},
    };
    std::uint64_t lost = 0;
    std::uint64_t jitterSum = 0;
    std::uint64_t jitterSamples = 0;
    std::uint64_t jitterMax = 0;
    std::uint64_t rttSum = 0;
    std::uint64_t rttSamples = 0;

    for (const jam2::NetworkPeerSnapshot& peer : snapshot.peers) {
        lost += peer.stream.sequence.lost;
        jitterSum += peer.stream.jitter_sum_us;
        jitterSamples += peer.stream.jitter_samples;
        jitterMax = std::max(jitterMax, peer.stream.jitter_max_us);
        rttSum += peer.stream.rtt_sum_us;
        rttSamples += peer.stream.rtt_samples;
        const SharedSessionController::EdgeState edge =
            peer.descriptor.endpoint_state == jam2::PeerEndpointState::Active
                ? SharedSessionController::EdgeState::Active
                : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Probing
                    ? SharedSessionController::EdgeState::Probing
                    : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Failed
                        ? SharedSessionController::EdgeState::Failed
                        : SharedSessionController::EdgeState::Candidate;
        const QString proof = peer.descriptor.endpoint_state == jam2::PeerEndpointState::Active
            ? QStringLiteral("active") : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Probing
                ? QStringLiteral("probing") : peer.descriptor.endpoint_state == jam2::PeerEndpointState::Failed
                    ? QStringLiteral("failed") : QStringLiteral("candidate");
        sessionController_.updatePeerEdgeState(
            peer.descriptor.peer_id.value,
            edge,
            proof,
            peer.stream.expected_remote_sample_time > 0 ? QStringLiteral("receiving") : QStringLiteral("waiting"));
    }
    stats.insert(QStringLiteral("sequence_lost"), static_cast<double>(lost));
    stats.insert(QStringLiteral("jitter_avg_ms"), jitterSamples > 0 ? static_cast<double>(jitterSum) / jitterSamples / 1000.0 : 0.0);
    stats.insert(QStringLiteral("jitter_max_ms"), static_cast<double>(jitterMax) / 1000.0);
    stats.insert(QStringLiteral("rtt_avg_ms"), rttSamples > 0 ? static_cast<double>(rttSum) / rttSamples / 1000.0 : 0.0);
    updateStatsDisplay(stats);
}

void MainWindow::showStartJamDialog()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Start Jam"));
    dialog.resize(760, 620);
    activePublicEndpoint_.clear();
    updateConnectionControlState();

    auto* content = new QWidget(&dialog);
    auto* layout = new QVBoxLayout(content);
    const QList<QWidget*> visibleWidgets{
        bindHostEdit_, portSpin_, publicHostEdit_,
        stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, noStunCheck_, profileBox_, deviceBox_,
        inputChannelsEdit_, outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_, networkAudioFormatBox_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, meshMaxPeersSpin_,
        statsWarmupMsSpin_, logStatsEdit_, socketSendBufferSpin_,
        socketRecvBufferSpin_, osPriorityBox_, driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_,
        driftMaxCorrectionSpin_, sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_,
        jitterBufferMaxSpin_, adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_,
        adaptiveMaxSpin_, adaptiveReleaseSpin_, adaptiveRatioRampSpin_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }

    auto* sessionForm = new QFormLayout();
    sessionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    sessionForm->addRow(QStringLiteral("Bind"), bindHostEdit_);
    sessionForm->addRow(QStringLiteral("Port"), portSpin_);
    sessionForm->addRow(QStringLiteral("Public endpoint host"), publicHostEdit_);
    sessionForm->addRow(QStringLiteral("STUN server"), stunServerEdit_);
    sessionForm->addRow(QStringLiteral("STUN timeout ms"), stunTimeoutSpin_);
    sessionForm->addRow(QStringLiteral("STUN retries"), stunRetriesSpin_);
    sessionForm->addRow(QString(), noStunCheck_);
    sessionForm->addRow(QStringLiteral("Maximum peers (0 = unlimited)"), meshMaxPeersSpin_);
    auto* sessionBox = new QGroupBox(QStringLiteral("Connection"), content);
    sessionBox->setLayout(sessionForm);
    layout->addWidget(sessionBox);

    auto* audioForm = new QFormLayout();
    audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    audioForm->addRow(QStringLiteral("Profile"), profileBox_);
    audioForm->addRow(QStringLiteral("Audio device"), deviceBox_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Sample rate"), sampleRateSpin_);
    audioForm->addRow(QStringLiteral("Audio buffer size"), bufferSizeSpin_);
    audioForm->addRow(QStringLiteral("Frame size"), frameSizeSpin_);
    audioForm->addRow(QStringLiteral("Audio quality"), networkAudioFormatBox_);
    audioForm->addRow(QStringLiteral("Playback prefill frames"), prefillSpin_);
    audioForm->addRow(QStringLiteral("Playback max frames"), playbackMaxSpin_);
    audioForm->addRow(QStringLiteral("Capture ring frames"), captureRingSpin_);
    audioForm->addRow(QStringLiteral("Playback ring frames"), playbackRingSpin_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    layout->addWidget(audioBox);

    auto* advancedForm = new QFormLayout();
    advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advancedForm->addRow(QStringLiteral("Wait ms"), waitMsSpin_);
    advancedForm->addRow(QStringLiteral("Stream ms"), streamMsSpin_);
    advancedForm->addRow(QStringLiteral("Stream linger ms"), streamLingerMsSpin_);
    advancedForm->addRow(QString(), statsCheck_);
    advancedForm->addRow(QStringLiteral("Stats warmup ms"), statsWarmupMsSpin_);
    advancedForm->addRow(QStringLiteral("Log stats folder"), logStatsEdit_);
    advancedForm->addRow(QStringLiteral("Socket send buffer"), socketSendBufferSpin_);
    advancedForm->addRow(QStringLiteral("Socket recv buffer"), socketRecvBufferSpin_);
    advancedForm->addRow(QStringLiteral("OS priority"), osPriorityBox_);
    advancedForm->addRow(QString(), driftCorrectionCheck_);
    advancedForm->addRow(QStringLiteral("Drift smoothing"), driftSmoothingSpin_);
    advancedForm->addRow(QStringLiteral("Drift deadband ppm"), driftDeadbandSpin_);
    advancedForm->addRow(QStringLiteral("Drift max correction ppm"), driftMaxCorrectionSpin_);
    advancedForm->addRow(QString(), sampleTimePlayoutCheck_);
    advancedForm->addRow(QStringLiteral("Playout delay frames"), playoutDelaySpin_);
    advancedForm->addRow(QStringLiteral("Jitter buffer frames"), jitterBufferSpin_);
    advancedForm->addRow(QStringLiteral("Jitter buffer max frames"), jitterBufferMaxSpin_);
    advancedForm->addRow(QString(), adaptiveCushionCheck_);
    advancedForm->addRow(QStringLiteral("Adaptive target frames"), adaptiveTargetSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive min frames"), adaptiveMinSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive max frames"), adaptiveMaxSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive release ppm"), adaptiveReleaseSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive ratio ramp ms"), adaptiveRatioRampSpin_);
    auto* advancedBox = new QGroupBox(QStringLiteral("Engine Options"), content);
    advancedBox->setLayout(advancedForm);
    layout->addWidget(advancedBox);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* start = buttons->addButton(QStringLiteral("Start"), QDialogButtonBox::AcceptRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    auto* regen = buttons->addButton(QStringLiteral("New Session"), QDialogButtonBox::ActionRole);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] {
        refreshDevices();
    });
    QObject::connect(regen, &QPushButton::clicked, this, [this] {
        generateSession();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(noStunCheck_, &QCheckBox::toggled, &dialog, [this] {
        updateConnectionControlState();
    });

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    start->setDefault(true);

    const int result = dialog.exec();
    const QList<QWidget*> startWidgets{
        bindHostEdit_, portSpin_, publicHostEdit_,

        stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, noStunCheck_, profileBox_, deviceBox_,
        inputChannelsEdit_, outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_, networkAudioFormatBox_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, meshMaxPeersSpin_,
        statsWarmupMsSpin_, logStatsEdit_, socketSendBufferSpin_,
        socketRecvBufferSpin_, osPriorityBox_, driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_,
        driftMaxCorrectionSpin_, sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_,
        jitterBufferMaxSpin_, adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_,
        adaptiveMaxSpin_, adaptiveReleaseSpin_, adaptiveRatioRampSpin_,
    };
    for (QWidget* widget : startWidgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        connectionLabel_->setText(QStringLiteral("Starting listener"));
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

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Join Jam"));
    dialog.resize(680, 520);

    auto* content = new QWidget(&dialog);
    auto* layout = new QVBoxLayout(content);
    const QList<QWidget*> visibleWidgets{
        connectUrlEdit_, bindHostEdit_, portSpin_, deviceBox_, inputChannelsEdit_, outputChannelsEdit_,
        statsCheck_, statsWarmupMsSpin_, logStatsEdit_, osPriorityBox_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }

    auto* sessionForm = new QFormLayout();
    sessionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    sessionForm->addRow(QStringLiteral("jam2 URL"), connectUrlEdit_);
    sessionForm->addRow(QStringLiteral("Local UDP bind host"), bindHostEdit_);
    sessionForm->addRow(QStringLiteral("Local UDP bind port"), portSpin_);
    auto* sessionBox = new QGroupBox(QStringLiteral("Connection"), content);
    sessionBox->setLayout(sessionForm);
    layout->addWidget(sessionBox);

    auto* audioForm = new QFormLayout();
    audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    audioForm->addRow(QStringLiteral("Audio device"), deviceBox_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannelsEdit_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    layout->addWidget(audioBox);

    auto* statsForm = new QFormLayout();
    statsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    statsForm->addRow(QString(), statsCheck_);
    statsForm->addRow(QStringLiteral("Stats warmup ms"), statsWarmupMsSpin_);
    statsForm->addRow(QStringLiteral("Log stats folder"), logStatsEdit_);
    statsForm->addRow(QStringLiteral("OS priority"), osPriorityBox_);
    auto* statsBox = new QGroupBox(QStringLiteral("Local Stats"), content);
    statsBox->setLayout(statsForm);
    layout->addWidget(statsBox);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* join = buttons->addButton(QStringLiteral("Join"), QDialogButtonBox::AcceptRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] {
        refreshDevices();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    join->setDefault(true);

    const int result = dialog.exec();
    const QList<QWidget*> joinWidgets{
        connectUrlEdit_, bindHostEdit_, portSpin_, deviceBox_, inputChannelsEdit_, outputChannelsEdit_,
        statsCheck_, statsWarmupMsSpin_, logStatsEdit_, osPriorityBox_,
    };
    for (QWidget* widget : joinWidgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        connectionLabel_->setText(QStringLiteral("Joining"));
        startJam(false);
    }
}

void MainWindow::stopJam(bool returnToLocal)
{
    const bool shouldReturnToLocal = returnToLocal && !shuttingDown_;
    jamStartupPending_ = false;
    assetTransfer_.cancel();
    sessionController_.close();
    // The Engine intentionally survives Leave so Perform can resume without a
    // device restart. Stop the old session click after detaching the network;
    // otherwise the audio callback keeps rendering it while the local UI has
    // already changed back to the stopped state.
    if (jam2_.isRunning()) {
        submitEngineToggle(
            jam2::EngineCommandType::SetLeaderAudioLocalClick,
            false,
            QStringLiteral("leave metronome leader"));
        submitEngineToggle(
            jam2::EngineCommandType::SetMetronomeEnabled,
            false,
            QStringLiteral("leave metronome enabled"));
    }
    if (sessionTopologyLabel_) {
        sessionTopologyLabel_->setText(QStringLiteral("Remote Peers 0"));
        sessionTopologyLabel_->setToolTip(QString{});
    }
    localMeshPeerTokens_.clear();

    meshPeerEndpoints_.clear();
    lastLoggedSessionSummary_.clear();
    pendingMeshInvitePopup_ = false;
    activePublicEndpoint_.clear();
    pendingPreparedTrackReadyRevision_ = 0;
    pendingSongSet_ = {};
    pendingSongRevision_ = 0;
    pendingSongTrackRestart_ = false;
    pendingSongSourcePeerToken_.clear();
    pendingSongNeedsAuthoritativePublish_ = false;
    pendingLooperAssetHashes_.clear();
    deferredSongSetMessage_ = {};
    deferredSongSetSourcePeerToken_.clear();
    songAssetCheckRetryTimer_.stop();
    pendingSharedTrackRevision_ = 0;
    pendingSharedTrackHostReady_ = true;
    pendingSharedTrackReadyTokens_.clear();
    publishStoppedTrackStateWhenApplied_ = false;
    trackRecordingWorkflow_.clearSessionSchedule();
    if (recordingCountdownLabel_) recordingCountdownLabel_->hide();
    metronomeTransport_.setLocalState(false, false);
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Local metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }

    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
    startButton_->setEnabled(true);
    joinButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    if (networkAudioFormatBox_) {
        networkAudioFormatBox_->setEnabled(true);
    }
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
            return;
        }
        if (engineModeLabel_) engineModeLabel_->setText(QStringLiteral("Local"));
        connectionLabel_->setText(QStringLiteral("Local"));
        updateRuntimeControls();
        sendMetronomeModeToJam();
        sendMetronomePatternToJam();
    }
}

void MainWindow::refreshDevices()
{
    deviceBox_->clear();
    try {
        for (const jam2::audio::DeviceInfo& device : jam2::audio::list_devices()) {
            const QString text = QStringLiteral("[%1] %2 %3")
                .arg(device.id)
                .arg(QString::fromStdString(device.backend), QString::fromStdString(device.name));
            deviceBox_->addItem(text, QString::number(device.id));
        }
    } catch (const std::exception& error) {
        appendLog(QStringLiteral("device refresh failed: ") + QString::fromUtf8(error.what()));
    }
    if (deviceBox_->count() == 0) {
        appendLog(QStringLiteral("no audio devices returned by the local engine"));
    } else {
        appendLog(QStringLiteral("loaded %1 audio devices").arg(deviceBox_->count()));
    }
}

void MainWindow::appendLog(const QString& line)
{
    if (logEdit_) {
        logEdit_->appendPlainText(line);
    }
    if (!guiLogFile_) {
        QString root = logStatsEdit_ ? logStatsEdit_->text().trimmed() : QString{};
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

void MainWindow::updateStatsDisplay(const QJsonObject& stats)
{
    const MixerStatsLabels labels = mixerStatsViewModel_.present(stats);
    latencyLabel_->setText(labels.latency);
    jitterLabel_->setText(labels.jitter);
    lossLabel_->setText(labels.loss);
    depthLabel_->setText(labels.depth);
    ringDepthLabel_->setText(labels.reorder);
    underrunLabel_->setText(labels.underrun);
    missingFramesLabel_->setText(labels.stalls);
    driftLabel_->setText(labels.drift);
    diagnosisLabel_->setText(labels.diagnosis);

    if (!jam2_.isRunning()) {
        updateMixMeters(mixerStatsViewModel_.consumeStructured(stats));
    }
}

void MainWindow::updateMixMeters(const MixerMeterLevels& levels)
{
    if (mixInputMeter_) mixInputMeter_->setLevel(levels.input);
    if (mixSendMeter_) mixSendMeter_->setLevel(levels.send);
    if (mixMonitorMeter_) mixMonitorMeter_->setLevel(levels.monitor);
    if (mixRemotePeerMeter_) mixRemotePeerMeter_->setLevel(levels.remote);
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
    maybeScheduleSharedTrackRestart();

    if (snapshot.membershipRevision > 0) {
        const QString summary = QStringLiteral(
            "session snapshot membership_revision=%1 total_peers=%2 remote_peers=%3 "
            "coordinator=%4 grid_authority=%5 grid_revision=%6 "
            "arrangement_authority=%7 arrangement_revision=%8")
            .arg(snapshot.membershipRevision)
            .arg(snapshot.totalPeerCount)
            .arg(snapshot.remotePeerCount)
            .arg(snapshot.coordinatorToken.left(8), snapshot.gridAuthorityToken.left(8))
            .arg(snapshot.gridRevision)
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
    if (sessionTopologyLabel_) {
        int validRemotePeers = 0;
        for (const SharedSessionController::PeerSnapshot& peer : snapshot.peers) {
            if (peer.token != snapshot.localToken &&
                peer.edgeState == SharedSessionController::EdgeState::Active) {
                ++validRemotePeers;
            }
        }
        const QString quality = snapshot.contract.audioFormat == QStringLiteral("pcm16-mono")
            ? QStringLiteral("16-bit PCM")
            : snapshot.contract.audioFormat == QStringLiteral("pcm24-mono")
                ? QStringLiteral("24-bit PCM") : QStringLiteral("unknown PCM");
        sessionTopologyLabel_->setText(
            QStringLiteral("Remote Peers %1 · %2").arg(validRemotePeers).arg(quality));
        sessionTopologyLabel_->setToolTip(
            QStringLiteral("Authenticated TCP peers with an active UDP connection"));
    }
    updateMixRemotePeers();
    updateTrackControls();
}

void MainWindow::handleControlEvent(
    const jam2::control_protocol::TransportEvent& event,
    bool serverSide)
{

    using jam2::control_protocol::TransportEventType;
    QString displayState = event.detail;
    switch (event.type) {
    case TransportEventType::Listening:
        displayState = QStringLiteral("Waiting for peers");
        break;
    case TransportEventType::Connecting:
    case TransportEventType::Connected:
    case TransportEventType::ChallengeSent:
    case TransportEventType::ProofSent:
        displayState = QStringLiteral("Connecting");
        break;
    case TransportEventType::Authenticated:
        displayState = QStringLiteral("TCP peer authenticated");
        break;
    case TransportEventType::Disconnected:
        displayState = serverSide ? QStringLiteral("Waiting for peers") : QStringLiteral("Reconnecting");
        break;
    case TransportEventType::AlreadyConnected:
        displayState = QStringLiteral("TCP peer authenticated");
        break;
    case TransportEventType::RefreshRequested:
        displayState = QStringLiteral("Refreshing");
        break;
    case TransportEventType::ReconnectScheduled:
    case TransportEventType::ReconnectAttempt:
        displayState = QStringLiteral("Reconnecting");
        break;
    case TransportEventType::SessionEnded:
        displayState = QStringLiteral("Local");
        break;
    case TransportEventType::Failure:
        if (event.retryable) {
            displayState = QStringLiteral("Reconnecting");
        }
        break;
    }
    connectionLabel_->setText(displayState);
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
                "pre_authentication_disconnects=%16")
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
                .arg(stats.preAuthenticationDisconnects));
        } else {
            const ControlClient::Stats stats = sessionController_.clientStats();
            appendLog(QStringLiteral(
                "control_stats side=client auth_rejects=%1 auth_timeouts=%2 frame_rejects=%3 frame_timeouts=%4 "
                "tag_or_sequence_rejects=%5 output_rejects=%6 input_high_water=%7 output_high_water=%8 "
                "connection_attempts=%9 completed_connections=%10 disconnected_connections=%11")
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
                .arg(stats.disconnectedConnections));
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
        if (looperProject_.trackSyncEnabled()) {
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
            detail += joinerFirewallGuidance();
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
                creatorFirewallGuidance());
        }
    });
}

void MainWindow::refreshControlConnection()
{
    if (sessionController_.isServer() && sessionController_.hasPeer()) {
        appendLog(QStringLiteral("TCP control already connected"));
        return;
    }
    if (!sessionController_.isServer() && sessionController_.isConnected()) {
        appendLog(QStringLiteral("TCP control already connected"));
        return;
    }
    appendLog(QStringLiteral("refreshing TCP control"));
    sessionController_.setReconnectEnabled(true);
    sessionController_.refresh();
}

void MainWindow::applyTuningProfileName(const QString& name)
{
    if (profileBox_) {
        const int index = profileBox_->findData(name);
        if (index >= 0 && profileBox_->currentIndex() != index) {
            const QSignalBlocker blocker(profileBox_);
            profileBox_->setCurrentIndex(index);
        }
    }

    const QByteArray utf8 = name.toUtf8();
    const jam2::TuningProfile* profile = jam2::find_tuning_profile(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (profile == nullptr) {
        profile = &jam2::default_tuning_profile();
    }

    sampleRateSpin_->setValue(profile->sample_rate);
    bufferSizeSpin_->setValue(static_cast<int>(profile->audio_buffer_size));
    frameSizeSpin_->setValue(profile->frame_size);
    prefillSpin_->setValue(static_cast<int>(profile->playback_prefill_frames));
    playbackMaxSpin_->setValue(static_cast<int>(profile->playback_max_frames));
    captureRingSpin_->setValue(static_cast<int>(profile->capture_ring_frames));
    playbackRingSpin_->setValue(static_cast<int>(profile->playback_ring_frames));
    driftCorrectionCheck_->setChecked(profile->drift_correction);
    driftSmoothingSpin_->setValue(profile->drift_smoothing);
    driftDeadbandSpin_->setValue(profile->drift_deadband_ppm);
    driftMaxCorrectionSpin_->setValue(profile->drift_max_correction_ppm);
    sampleTimePlayoutCheck_->setChecked(profile->sample_time_playout);
    playoutDelaySpin_->setValue(static_cast<int>(profile->playout_delay_frames));
    jitterBufferSpin_->setValue(static_cast<int>(profile->jitter_buffer_frames));
    jitterBufferMaxSpin_->setValue(static_cast<int>(profile->jitter_buffer_max_frames));
    adaptiveCushionCheck_->setChecked(profile->adaptive_playback_cushion);
    adaptiveTargetSpin_->setValue(static_cast<int>(profile->adaptive_playback_target_frames));
    adaptiveMinSpin_->setValue(static_cast<int>(profile->adaptive_playback_min_frames));
    adaptiveMaxSpin_->setValue(static_cast<int>(profile->adaptive_playback_max_frames));
    adaptiveReleaseSpin_->setValue(profile->adaptive_playback_release_ppm);
    adaptiveRatioRampSpin_->setValue(profile->adaptive_playback_ratio_ramp_ms);
}

bool MainWindow::selectedDeviceSupportsSampleRate(int sampleRate)
{

    if (selectedDeviceId().isEmpty()) {
        return false;
    }
    try {
        const auto probe = jam2::audio::probe_device(selectedDeviceId().toInt(), sampleRate);
        if (probe.requested_sample_rate_supported) {
            return true;
        }
        appendLog(QStringLiteral("device sample-rate preflight failed: device %1 does not support %2 Hz")
            .arg(selectedDeviceId()).arg(sampleRate));
        return false;
    } catch (const std::exception& error) {
        appendLog(QStringLiteral("device sample-rate preflight failed: ") + QString::fromUtf8(error.what()));
        return false;
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
    QString host = createSession && publicHostEdit_ && !publicHostEdit_->text().trimmed().isEmpty()
        ? publicHostEdit_->text().trimmed()
        : (bindHostEdit_ && !bindHostEdit_->text().trimmed().isEmpty()
            ? bindHostEdit_->text().trimmed()
            : QStringLiteral("0.0.0.0"));
    return QStringLiteral("%1:%2").arg(host).arg(portSpin_ ? portSpin_->value() : 49000);
}

QString MainWindow::meshBindEndpoint() const
{
    const QString host = bindHostEdit_ && !bindHostEdit_->text().trimmed().isEmpty()
        ? bindHostEdit_->text().trimmed()
        : QStringLiteral("0.0.0.0");
    return QStringLiteral("%1:%2").arg(host).arg(portSpin_ ? portSpin_->value() : 49000);
}

QStringList MainWindow::meshPeerEndpointsExcludingSelf() const
{
    QStringList peers;
    const QString self = meshPeerToken_;
    for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it) {
        if (it.key() == self || it.value().isEmpty()) {
            continue;
        }
        if (!peers.contains(it.value())) {
            peers << it.value();
        }
    }
    return peers;
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
    if (looperProject_.trackSyncEnabled() && jam2_.isRunning()) {
        // Publish a fresh arrangement revision so a late or reconnected peer
        // participates in the same asset-readiness barrier and transport target
        // as every already-present peer.
        sendSongSnapshot();
    }
}

void MainWindow::sendControl(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("track.ready") &&
        jam2::application::isTrackSyncControlMessageType(type) &&
        !looperProject_.trackSyncEnabled()) {
        appendLog(QStringLiteral("suppressed local track sync while sync is disabled"));
        return;
    }
    sessionController_.send(message);
}

void MainWindow::updateRuntimeControls()
{
    if (localEngineButton_) {
        localEngineButton_->setEnabled(!jam2_.isRunning() && !jam2_.isNetworkRunning());
    }
    if (!jam2_.isRunning()) {
        return;
    }
    const double metronomeLevel = gainFromDb(static_cast<double>(metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10));
    const double remoteLevel = gainFromDb(static_cast<double>(remoteLevelSlider_ ? remoteLevelSlider_->value() : 0));
    submitEngineGain(jam2::EngineCommandType::SetMetronomeLevel, metronomeLevel, QStringLiteral("metronome level"));
    submitEngineGain(jam2::EngineCommandType::SetRemoteLevel, remoteLevel, QStringLiteral("remote level"));
    const double sendLevel = gainFromDb(static_cast<double>(mixSendLevelSlider_ ? mixSendLevelSlider_->value() : 0));
    const double monitorLevel = gainFromDb(static_cast<double>(mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : -18));
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
        QString error;
    };
    auto inputs = std::make_shared<QVector<Input>>();
    for (int bankIndex = 0; bankIndex < looperProject_.banks().size(); ++bankIndex) {
        for (LooperLane& lane : looperProject_.banks()[bankIndex].lanes) {
            if (lane.assetPath.trimmed().isEmpty()) {
                lane.sampleRateCompatible = true;
                continue;
            }
            lane.sampleRateCompatible = lane.sampleRate > 0 &&
                lane.sampleRate == expectedSampleRate;
            inputs->append(Input{
                bankIndex, lane.id, lane.name, looperAssetAbsolutePath(lane), false});
        }
    }
    SharedTrackModel& track = trackController_.model();
    if (!track.filePath.trimmed().isEmpty()) {
        track.sampleRateCompatible = track.sampleRate > 0 &&
            track.sampleRate == expectedSampleRate;
        inputs->append(Input{-1, {}, track.fileName, track.filePath, true});
    } else {
        track.sampleRateCompatible = true;
    }
    refreshLooperLanes();
    updateTrackControls();
    regeneratePreparedMix();
    if (inputs->isEmpty()) {
        return;
    }

    auto results = std::make_shared<QVector<Result>>();
    results->reserve(inputs->size());
    wavCompatibilityAuditRunning_ = true;
    const bool started = startFileWorkerTask(
        [inputs, results] {
            for (const Input& input : *inputs) {
                Result result;
                result.input = input;
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(input.path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                if (!inspected) {
                    result.error = QString::fromStdString(inspected.error);
                } else {
                    result.sampleRate = static_cast<int>(inspected.info.sample_rate);
                }
                results->append(std::move(result));
            }
        },
        [this, expectedSampleRate, showModal, results] {
            wavCompatibilityAuditRunning_ = false;
            QStringList newlyQuarantined;
            for (const Result& result : *results) {
                bool stillPresent = false;
                if (result.input.track) {
                    SharedTrackModel& current = trackController_.model();
                    stillPresent = current.filePath == result.input.path;
                    if (stillPresent) {
                        current.sampleRate = result.sampleRate;
                        current.sampleRateCompatible = result.error.isEmpty() &&
                            result.sampleRate == expectedSampleRate;
                    }
                } else if (result.input.bank >= 0 &&
                           result.input.bank < looperProject_.banks().size()) {
                    for (LooperLane& lane : looperProject_.banks()[result.input.bank].lanes) {
                        if (lane.id == result.input.laneId &&
                            looperAssetAbsolutePath(lane) == result.input.path) {
                            stillPresent = true;
                            lane.sampleRate = result.sampleRate;
                            lane.sampleRateCompatible = result.error.isEmpty() &&
                                result.sampleRate == expectedSampleRate;
                            break;
                        }
                    }
                }
                if (!stillPresent ||
                    (result.error.isEmpty() && result.sampleRate == expectedSampleRate)) {
                    continue;
                }
                const QString identity = result.input.path + QLatin1Char('|') +
                    QString::number(expectedSampleRate) + QLatin1Char('|') +
                    QString::number(result.sampleRate);
                if (!reportedIncompatibleWavs_.contains(identity) && newlyQuarantined.size() < 8) {
                    reportedIncompatibleWavs_.insert(identity);
                    newlyQuarantined.append(result.error.isEmpty()
                        ? QStringLiteral("%1 — expected %2 Hz, actual %3 Hz")
                            .arg(result.input.name)
                            .arg(expectedSampleRate)
                            .arg(result.sampleRate)
                        : QStringLiteral("%1 — expected %2 Hz, WAV could not be inspected: %3")
                            .arg(result.input.name)
                            .arg(expectedSampleRate)
                            .arg(result.error));
                }
            }
            while (reportedIncompatibleWavs_.size() > 256) {
                reportedIncompatibleWavs_.erase(reportedIncompatibleWavs_.begin());
            }
            refreshLooperLanes();
            updateTrackControls();
            regeneratePreparedMix();
            if (showModal && !newlyQuarantined.isEmpty()) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("WAV Sample-Rate Mismatch"),
                    QStringLiteral(
                        "These WAVs are quarantined from playback and Track Sync. "
                        "Unload or replace them with files matching the jam:\n\n") +
                        newlyQuarantined.join(QStringLiteral("\n")));
            }
            if (pendingWavCompatibilityAuditRate_ > 0) {
                const int pending = pendingWavCompatibilityAuditRate_;
                pendingWavCompatibilityAuditRate_ = 0;
                auditWavCompatibilityForSession(pending, showModal);
            } else if (looperProject_.trackSyncEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        },
        [this, expectedSampleRate, showModal](const QString&) {
            wavCompatibilityAuditRunning_ = false;
            pendingWavCompatibilityAuditRate_ = expectedSampleRate;
            QTimer::singleShot(100, this, [this, showModal] {
                const int pending = pendingWavCompatibilityAuditRate_;
                pendingWavCompatibilityAuditRate_ = 0;
                auditWavCompatibilityForSession(pending, showModal);
            });
        });
    if (!started) {
        wavCompatibilityAuditRunning_ = false;
        pendingWavCompatibilityAuditRate_ = expectedSampleRate;
        QTimer::singleShot(100, this, [this, showModal] {
            const int pending = pendingWavCompatibilityAuditRate_;
            pendingWavCompatibilityAuditRate_ = 0;
            auditWavCompatibilityForSession(pending, showModal);
        });
    }
}

void MainWindow::refreshLooperLanes()
{
    for (int i = 0; i < static_cast<int>(looperBankButtons_.size()); ++i) {
        if (looperBankButtons_[i]) {
            looperBankButtons_[i]->setChecked(i == looperProject_.activeBankIndex());
            looperBankButtons_[i]->setStyleSheet(i == looperProject_.activeBankIndex()
                ? QStringLiteral("QPushButton { background: #4a7496; color: #e6eef4; border: 1px solid #6e8fa8; padding: 4px; }")
                : QStringLiteral("QPushButton { background: #272d33; color: #aab2ba; border: 1px solid #4a525a; padding: 4px; }"));
        }
    }
    if (looperStack_ == nullptr) {
        return;
    }
    const LooperBank& bank = looperProject_.banks().at(looperProject_.activeBankIndex());
    if (bank.lanes.isEmpty()) {
        selectedLooperLane_ = -1;
    } else {
        selectedLooperLane_ = qBound(0, selectedLooperLane_, bank.lanes.size() - 1);
    }

    QVector<LooperLaneStackWidget::LaneView> views;
    QStringList missingWaveforms;
    views.reserve(bank.lanes.size());
    for (const LooperLane& lane : bank.lanes) {
        LooperLaneStackWidget::LaneView view;
        view.lane = lane;
        if (!lane.sampleRateCompatible) {
            view.lane.name += QStringLiteral(" [quarantined: sample-rate mismatch]");
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
    const int armedLane = trackRecordingWorkflow_.armedBank() == looperProject_.activeBankIndex()
        ? trackRecordingWorkflow_.armedLane() : -1;
    int markerRate = trackRecordingWorkflow_.preparedSampleRate();
    if (markerRate <= 0) {
        markerRate = 48000;
    }
    looperStack_->setLanes(
        std::move(views),
        selectedLooperLane_,
        looperProject_.activeBankIndex(),
        armedLane,
        markerRate,
        metronomeTransport_.grid().bpm(),
        looperProject_.gridLockEnabled());

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
                while (looperWaveformCache_.size() >= maxCachedWaveforms &&
                       !looperWaveformCache_.contains(path)) {
                    looperWaveformCache_.erase(looperWaveformCache_.begin());

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

void MainWindow::applySelectedLooperLaneRegion(qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame)
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int row = selectedLooperLane_;
    LooperBank& bank = looperProject_.banks()[looperProject_.activeBankIndex()];
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
    lane.startFrame = qMax<qint64>(0, startFrame);
    lane.stopFrame = lane.startFrame + (sourceEndFrame - sourceStartFrame);
    if (sourceStartFrame == 0 && sourceEndFrame == sourceFrames) {
        lane.loopStartFrame = -1;
        lane.loopEndFrame = -1;
    } else {
        lane.loopStartFrame = sourceStartFrame;
        lane.loopEndFrame = sourceEndFrame;
    }

    refreshLooperLanes();
    selectedLooperLane_ = row;
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::applyLooperLaneGain(int laneIndex, double gainDb)
{
    LooperBank& bank = looperProject_.banks()[looperProject_.activeBankIndex()];
    if (laneIndex < 0 || laneIndex >= bank.lanes.size()) {
        return;
    }
    selectedLooperLane_ = laneIndex;
    bank.lanes[laneIndex].gainDb = qBound(-60.0, gainDb, 12.0);
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::addLooperWavs()
{
    QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add WAV lanes"), QString(), QStringLiteral("WAV files (*.wav *.WAV)"));
    if (paths.isEmpty()) {
        return;
    }
    constexpr int maxLanesPerBank =
        jam2::application::limits::kMaximumLooperLanesPerBank;
    const int bankIndex = looperProject_.activeBankIndex();
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
    const int expectedSampleRate = sampleRateSpin_ ? sampleRateSpin_->value() : 48000;
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
                registerTransientTrackWav(asset.stagedPath);
                looperWaveformCache_.remove(asset.stagedPath);
                LooperLane lane;
                lane.assetPath = asset.stagedPath;
                lane.assetHash = asset.sha256;
                lane.name = asset.displayName;
                lane.sampleRate = asset.metadata.sampleRate;
                lane.sampleRateCompatible = true;
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
            if (imported > 0 && looperProject_.trackSyncEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        });
}

void MainWindow::loadWavIntoLooperLane()
{
    const int bankIndex = looperProject_.activeBankIndex();
    const LooperBank& bank = looperProject_.banks().at(bankIndex);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Load WAV"));
    auto* form = new QFormLayout(&dialog);
    auto* laneBox = new QComboBox(&dialog);
    for (int laneIndex = 0; laneIndex < bank.lanes.size(); ++laneIndex) {
        laneBox->addItem(bank.lanes.at(laneIndex).name, laneIndex);
    }
    if (bank.lanes.isEmpty()) {
        laneBox->addItem(QStringLiteral("Empty Track 1"), -1);
    } else if (selectedLooperLane_ >= 0 && selectedLooperLane_ < laneBox->count()) {
        laneBox->setCurrentIndex(selectedLooperLane_);
    }

    auto* pathEdit = new QLineEdit(&dialog);
    pathEdit->setReadOnly(true);
    auto* browse = new QPushButton(QStringLiteral("Browse"), &dialog);
    auto* pathRow = new QHBoxLayout();
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browse);
    form->addRow(QStringLiteral("Track lane"), laneBox);
    form->addRow(QStringLiteral("WAV"), pathRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Load WAV"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    form->addRow(buttons);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [this, pathEdit, buttons] {
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Load WAV"),
            QString(),
            QStringLiteral("WAV files (*.wav *.WAV)"));
        if (!path.isEmpty()) {
            pathEdit->setText(QDir::toNativeSeparators(path));
            buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString sourcePath = QDir::fromNativeSeparators(pathEdit->text());
    const int selectedLaneIndex = laneBox->currentData().toInt();
    const QString targetLaneId = selectedLaneIndex >= 0 && selectedLaneIndex < bank.lanes.size()
        ? bank.lanes.at(selectedLaneIndex).id
        : QString{};
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = sampleRateSpin_ ? sampleRateSpin_->value() : 48000;
    auto result = std::make_shared<StagedPcm16Asset>();
    (void)startFileWorkerTask(
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
            int laneIndex = -1;
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
            } else {
                if (!looperProject_.appendLane(bankIndex, LooperLane{})) {
                    QMessageBox::warning(this, QStringLiteral("Load WAV"), QStringLiteral("Could not create a track lane."));
                    return;
                }
                laneIndex = looperProject_.banks().at(bankIndex).lanes.size() - 1;
            }
            registerTransientTrackWav(result->stagedPath);
            looperWaveformCache_.remove(result->stagedPath);
            LooperLane& lane = looperProject_.banks()[bankIndex].lanes[laneIndex];
            lane.assetPath = result->stagedPath;
            lane.assetHash = result->sha256;
            lane.sampleRate = result->metadata.sampleRate;
            lane.sampleRateCompatible = true;
            if (isDefaultEmptyTrackName(lane.name) || lane.name.trimmed().isEmpty()) {
                lane.name = result->displayName;
            }
            lane.startFrame = 0;
            lane.stopFrame = -1;
            lane.loopStartFrame = -1;
            lane.loopEndFrame = -1;
            lane.loopEnabled = false;
            selectedLooperLane_ = laneIndex;
            refreshLooperLanes();
            regeneratePreparedMix();
            if (looperProject_.trackSyncEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        });
}

void MainWindow::addEmptyLooperLane()
{
    LooperLane lane;
    if (!looperProject_.appendLane(looperProject_.activeBankIndex(), std::move(lane))) {
        appendLog(QStringLiteral("could not add empty lane"));
        return;
    }
    refreshLooperLanes();
    const int row = looperProject_.banks().at(looperProject_.activeBankIndex()).lanes.size() - 1;
    selectedLooperLane_ = row;
    refreshLooperLanes();
    syncLooperArrangement();
}

bool MainWindow::armSelectedLooperLaneRecording()

{
    if (selectedLooperLane_ < 0) {
        if (looperProject_.banks().at(looperProject_.activeBankIndex()).lanes.isEmpty()) {
            addEmptyLooperLane();
        } else {
            selectedLooperLane_ = 0;
        }
    }
    if (selectedLooperLane_ < 0) {
        return false;
    }

    const int bankIndex = looperProject_.activeBankIndex();
    const int laneIndex = selectedLooperLane_;
    const LooperLane& lane = looperProject_.banks().at(bankIndex).lanes.at(laneIndex);
    const QString bankId = looperProject_.banks().at(bankIndex).id;
    const QString laneId = lane.id;
    const QString laneName = lane.name;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Arm Lane Recording"));
    dialog.resize(760, 560);
    auto* content = new QWidget(&dialog);
    content->setMinimumWidth(700);
    auto* form = new QFormLayout(content);

    auto* modeBox = new QComboBox(content);
    modeBox->addItem(QStringLiteral("Input"), QStringLiteral("input"));
    modeBox->addItem(QStringLiteral("Loopback"), QStringLiteral("loopback"));
    applyMutedEditorStyle(modeBox);

    const QList<QWidget*> widgets{
        captureOutputEdit_, deviceBox_, inputChannelsEdit_, loopbackSourceBox_,
        sampleRateSpin_, bufferSizeSpin_, captureManualStopCheck_, captureDurationSpin_,
        captureCountInCheck_, captureCountInMetronomeCheck_, captureKeepMetronomeCheck_, captureCountInBarsSpin_, captureTriggerCheck_, triggerThresholdSpin_,
        triggerHoldSpin_, preRollSpin_, tailThresholdSpin_, tailSilenceSpin_, trimLeadingCheck_,
        trimTrailingCheck_, recordingLatencyLabel_, recordingLatencyAdjustmentSpin_,
    };
    for (QWidget* widget : widgets) {
        widget->show();
    }
    const QString laneFileName = safeFileName(laneName);
    captureOutputEdit_->setText(timestampedCapturePath(laneFileName.isEmpty() ? QStringLiteral("lane") : laneFileName));

    auto* outputLayout = new QHBoxLayout();
    outputLayout->addWidget(captureOutputEdit_, 1);

    auto* browse = new QPushButton(QStringLiteral("Browse"), content);
    outputLayout->addWidget(browse);
    auto* sourceLayout = new QHBoxLayout();
    sourceLayout->addWidget(loopbackSourceBox_, 1);
    auto* refreshSources = new QPushButton(QStringLiteral("Refresh Sources"), content);
    sourceLayout->addWidget(refreshSources);
    auto* countInLayout = new QHBoxLayout();
    countInLayout->addWidget(captureCountInCheck_);

    countInLayout->addWidget(captureCountInBarsSpin_);
    countInLayout->addStretch(1);
    auto* metronomeLayout = new QHBoxLayout();
    metronomeLayout->addWidget(captureCountInMetronomeCheck_);
    metronomeLayout->addWidget(captureKeepMetronomeCheck_);
    metronomeLayout->addStretch(1);
    auto* latencyLayout = new QHBoxLayout();
    latencyLayout->addWidget(recordingLatencyLabel_, 1);
    latencyLayout->addWidget(recordingLatencyAdjustmentSpin_);

    auto refreshMode = [this, modeBox] {
        const bool inputMode = modeBox->currentData().toString() == QStringLiteral("input");
        deviceBox_->setVisible(inputMode);
        inputChannelsEdit_->setVisible(inputMode);
        sampleRateSpin_->setVisible(inputMode);
        bufferSizeSpin_->setVisible(inputMode);
        recordingLatencyLabel_->setVisible(inputMode);
        recordingLatencyAdjustmentSpin_->setVisible(inputMode);
        loopbackSourceBox_->setVisible(!inputMode);
    };

    form->addRow(QStringLiteral("Lane"), new QLabel(laneName, content));
    form->addRow(QStringLiteral("Source"), modeBox);
    form->addRow(QStringLiteral("Take WAV"), outputLayout);
    form->addRow(QStringLiteral("Audio device"), deviceBox_);
    form->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    form->addRow(QStringLiteral("Loopback source"), sourceLayout);
    form->addRow(QStringLiteral("Sample rate"), sampleRateSpin_);
    form->addRow(QStringLiteral("Buffer size"), bufferSizeSpin_);
    form->addRow(QStringLiteral("Recording alignment"), latencyLayout);
    form->addRow(captureManualStopCheck_);
    auto* durationLabel = new QLabel(QStringLiteral("Duration limit"), content);
    form->addRow(durationLabel, captureDurationSpin_);
    form->addRow(QStringLiteral("Count-in"), countInLayout);
    form->addRow(QStringLiteral("Metronome"), metronomeLayout);
    form->addRow(captureTriggerCheck_);
    form->addRow(QStringLiteral("Trigger threshold"), triggerThresholdSpin_);
    form->addRow(QStringLiteral("Trigger hold"), triggerHoldSpin_);
    form->addRow(QStringLiteral("Pre-roll"), preRollSpin_);
    form->addRow(QStringLiteral("Tail threshold"), tailThresholdSpin_);
    form->addRow(QStringLiteral("Tail silence"), tailSilenceSpin_);
    form->addRow(trimLeadingCheck_);
    form->addRow(trimTrailingCheck_);

    QObject::connect(modeBox, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, refreshMode);
    QObject::connect(browse, &QPushButton::clicked, this, [this] { chooseCaptureFolder(); });
    QObject::connect(refreshSources, &QPushButton::clicked, this, [this] { refreshLoopbackSources(); });
    QObject::connect(captureManualStopCheck_, &QCheckBox::toggled, &dialog, [this, durationLabel] {
        updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_, durationLabel);
    });
    updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_, durationLabel);
    refreshMode();

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* arm = buttons->addButton(QStringLiteral("Arm"), QDialogButtonBox::AcceptRole);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    arm->setDefault(true);

    const int result = dialog.exec();
    for (QWidget* widget : widgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result != QDialog::Accepted) {
        return false;
    }
    const TrackRecordingWorkflow::CaptureMode captureMode =
        modeBox->currentData().toString() == QStringLiteral("loopback")
        ? TrackRecordingWorkflow::CaptureMode::Loopback
        : TrackRecordingWorkflow::CaptureMode::Input;
    const LooperLaneLocation resolved = findLooperLaneLocation(
        looperProject_, bankId, laneId);
    if (!resolved.valid() ||
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
    trackRecordingWorkflow_.armLane(resolved.bank, resolved.lane, captureMode);
    refreshLooperLanes();
    appendLog(QStringLiteral("armed lane recording: bank=%1 lane=%2 mode=%3")
        .arg(looperProject_.banks().at(resolved.bank).id)
        .arg(looperProject_.banks().at(resolved.bank).lanes.at(resolved.lane).name)
        .arg(captureMode == TrackRecordingWorkflow::CaptureMode::Loopback
            ? QStringLiteral("loopback") : QStringLiteral("input")));
    return true;
}

void MainWindow::startArmedLooperLaneRecording()
{
    if (trackRecordingWorkflow_.inputTakeActive() || loopbackRecorder_.isRunning()) {
        return;
    }
    if (!trackRecordingWorkflow_.laneArmed()) {
        if (!armSelectedLooperLaneRecording()) {
            return;
        }
    }
    const bool engineInput =
        trackRecordingWorkflow_.captureMode() == TrackRecordingWorkflow::CaptureMode::Input;
    if (captureCountInCheck_ && captureCountInCheck_->isChecked()) {
        const bool countInMetronome = captureCountInMetronomeCheck_ && captureCountInMetronomeCheck_->isChecked();
        const bool keepMetronome = captureKeepMetronomeCheck_ && captureKeepMetronomeCheck_->isChecked();
        const PlaybackGrid::Position initialGridPosition =
            metronomeTransport_.grid().position();
        const bool startingFreshGrid =
            engineInput && countInMetronome && !initialGridPosition.running;

        if (startingFreshGrid) {
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
        if (engineInput) {
            trackRecordingWorkflow_.waitForCountIn(0, countInMetronome && !keepMetronome);
            if (!initialGridPosition.running) {
                trackRecordingWorkflow_.waitForCountIn(
                    bars,
                    countInMetronome && !keepMetronome,
                    startingFreshGrid,
                    initialGridPosition.epochFrame);
                if (recordingCountdownLabel_) {
                    recordingCountdownLabel_->setText(QStringLiteral("WAITING FOR NEXT BAR..."));
                    recordingCountdownLabel_->show();
                }
            } else {
                startInputCapture(0, bars);
            }
        } else {
            startLoopbackCapture();
        }
    } else {
        trackRecordingWorkflow_.waitForCountIn(0, false);
        if (engineInput) {
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

void MainWindow::importLastCaptureToArmedLane()
{
    if (!trackRecordingWorkflow_.laneArmed() ||
        trackRecordingWorkflow_.armedBank() >= looperProject_.banks().size() ||
        trackRecordingWorkflow_.armedLane() >=
            looperProject_.banks().at(trackRecordingWorkflow_.armedBank()).lanes.size()) {
        return;
    }
    const int bankIndex = trackRecordingWorkflow_.armedBank();
    const QString laneId = looperProject_.banks().at(bankIndex)
        .lanes.at(trackRecordingWorkflow_.armedLane()).id;
    const QString sourcePath = trackRecordingWorkflow_.lastCapturePath();
    const QString stagingFolder = projectPersistence_.workspaceFolder();
    const int expectedSampleRate = sampleRateSpin_ ? sampleRateSpin_->value() : 48000;
    auto result = std::make_shared<StagedPcm16Asset>();
    (void)startFileWorkerTask(
        [sourcePath, stagingFolder, expectedSampleRate, result] {
            *result = stagePcm16Asset(sourcePath, stagingFolder, expectedSampleRate);
        },
        [this, bankIndex, laneId, result] {
            if (!result->error.isEmpty()) {
                appendLog(QStringLiteral("recorded lane WAV not importable: ") + result->error);
                return;
            }
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
                return;
            }
            int laneIndex = -1;
            const auto& lanes = looperProject_.banks().at(bankIndex).lanes;
            for (int index = 0; index < lanes.size(); ++index) {
                if (lanes.at(index).id == laneId) {
                    laneIndex = index;
                    break;
                }
            }
            if (laneIndex < 0) {
                appendLog(QStringLiteral("recorded lane target was removed before import completed"));
                return;
            }
            registerTransientTrackWav(result->stagedPath);
            looperWaveformCache_.remove(result->stagedPath);
            LooperLane& lane = looperProject_.banks()[bankIndex].lanes[laneIndex];
            lane.assetPath = result->stagedPath;
            lane.assetHash = result->sha256;
            lane.sampleRate = result->metadata.sampleRate;
            lane.sampleRateCompatible = true;
            if (isDefaultEmptyTrackName(lane.name) || lane.name.trimmed().isEmpty()) {
                lane.name = result->displayName;
            }
            lane.startFrame = 0;
            lane.stopFrame = -1;
            lane.loopStartFrame = -1;
            lane.loopEndFrame = -1;
            lane.loopEnabled = false;
            appendLog(QStringLiteral("recorded lane imported: %1").arg(lane.name));
            trackRecordingWorkflow_.disarmLane();
            refreshLooperLanes();
            regeneratePreparedMix();
            if (looperProject_.trackSyncEnabled() && jam2_.isNetworkRunning()) {
                shareLocalTracks();
            }
        });
}

void MainWindow::renameSelectedLooperLane()
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int row = selectedLooperLane_;
    const LooperLane& lane = looperProject_.banks().at(looperProject_.activeBankIndex()).lanes.at(row);
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename lane"), QStringLiteral("Lane name"),
        QLineEdit::Normal, lane.name, &accepted);
    if (accepted && looperProject_.renameLane(looperProject_.activeBankIndex(), row, name)) {
        refreshLooperLanes();
        regeneratePreparedMix();
        syncLooperArrangement();
    }
}

void MainWindow::removeSelectedLooperLane()
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int bankIndex = looperProject_.activeBankIndex();
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
        QStringLiteral("Remove '%1' from this bank?").arg(lane.name), QMessageBox::Cancel, this);
    QPushButton* removeOnly = dialog.addButton(QStringLiteral("Remove lane only"), QMessageBox::AcceptRole);
    QPushButton* deleteAsset = dialog.addButton(QStringLiteral("Delete WAV from disk"), QMessageBox::DestructiveRole);
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
    if (dialog.clickedButton() == deleteAsset && (!hasWav || !QFile::remove(assetPath))) {
        QMessageBox::warning(this, QStringLiteral("Remove lane"), QStringLiteral("Could not delete the WAV file."));
        return;
    }
    looperProject_.removeLane(bankIndex, row);
    selectedLooperLane_ = qMin(row, looperProject_.banks().at(bankIndex).lanes.size() - 1);
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::moveSelectedLooperLane(int delta)
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int from = selectedLooperLane_;
    const int to = from + delta;
    if (looperProject_.moveLane(looperProject_.activeBankIndex(), from, to)) {
        selectedLooperLane_ = to;
        refreshLooperLanes();
        regeneratePreparedMix();
        syncLooperArrangement();
    }
}

void MainWindow::toggleSelectedLooperLaneMute()
{
    if (selectedLooperLane_ < 0) return;
    LooperLane& lane = looperProject_.banks()[looperProject_.activeBankIndex()].lanes[selectedLooperLane_];
    lane.muted = !lane.muted;
    refreshLooperLanes();
    regeneratePreparedMix();

    syncLooperArrangement();
}

void MainWindow::toggleSelectedLooperLaneSolo()
{
    if (selectedLooperLane_ < 0) return;
    LooperLane& lane = looperProject_.banks()[looperProject_.activeBankIndex()].lanes[selectedLooperLane_];

    lane.solo = !lane.solo;
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::setSelectedLooperLaneGain()
{
    if (selectedLooperLane_ < 0) return;
    LooperLane& lane = looperProject_.banks()[looperProject_.activeBankIndex()].lanes[selectedLooperLane_];
    bool accepted = false;
    const double gain = QInputDialog::getDouble(this, QStringLiteral("Lane gain"), QStringLiteral("Gain (dB)"), lane.gainDb, -60.0, 12.0, 1, &accepted);
    if (!accepted) return;
    lane.gainDb = gain;
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::editSelectedLooperLaneRegion()
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    LooperLane& lane = looperProject_.banks()[looperProject_.activeBankIndex()].lanes[selectedLooperLane_];
    qint64 startFrame = lane.startFrame;
    qint64 stopFrame = lane.stopFrame;
    qint64 loopStartFrame = lane.loopStartFrame;
    qint64 loopEndFrame = lane.loopEndFrame;

    if (!promptFrame(this, QStringLiteral("Lane region"), QStringLiteral("Timeline start frame"), startFrame, startFrame)) {
        return;
    }
    if (startFrame < 0) {
        QMessageBox::warning(this, QStringLiteral("Lane region"), QStringLiteral("Timeline start frame is required."));
        return;
    }
    if (!promptFrame(this, QStringLiteral("Lane region"), QStringLiteral("Timeline stop frame (empty for source end)"), stopFrame, stopFrame)) {
        return;
    }
    if (stopFrame >= 0 && stopFrame < startFrame) {
        QMessageBox::warning(this, QStringLiteral("Lane region"), QStringLiteral("Timeline stop frame must be after start frame."));
        return;
    }
    if (!promptFrame(this, QStringLiteral("Lane region"), QStringLiteral("Source crop start frame (empty for source start)"), loopStartFrame, loopStartFrame)) {
        return;
    }
    if (!promptFrame(this, QStringLiteral("Lane region"), QStringLiteral("Source crop end frame (empty for source end)"), loopEndFrame, loopEndFrame)) {
        return;
    }
    if ((loopStartFrame < 0) != (loopEndFrame < 0)) {
        QMessageBox::warning(this, QStringLiteral("Lane region"), QStringLiteral("Source crop start and end must both be set, or both left empty."));
        return;
    }
    if (loopStartFrame >= 0 && loopEndFrame <= loopStartFrame) {
        QMessageBox::warning(this, QStringLiteral("Lane region"), QStringLiteral("Source crop end frame must be after the source crop start frame."));
        return;
    }
    const bool loopEnabled = QMessageBox::question(
        this,
        QStringLiteral("Lane region"),
        QStringLiteral("Loop the source crop range for this lane?"),
        QMessageBox::Yes | QMessageBox::No,
        lane.loopEnabled ? QMessageBox::Yes : QMessageBox::No) == QMessageBox::Yes;
    if (loopStartFrame < 0) {
        loopStartFrame = -1;
        loopEndFrame = -1;
    }

    lane.startFrame = startFrame;
    lane.stopFrame = stopFrame;
    lane.loopEnabled = loopEnabled;
    lane.loopStartFrame = loopStartFrame;
    lane.loopEndFrame = loopEndFrame;
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::syncLooperArrangement()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (looperProject_.trackSyncEnabled() && jam2_.isRunning() &&
        (role == SharedSessionController::Role::Creator ||
         role == SharedSessionController::Role::Joiner)) {
        sendSongSnapshot();
    }
}

QString MainWindow::looperAssetAbsolutePath(const LooperLane& lane) const
{
    if (QFileInfo(lane.assetPath).isAbsolute() || projectPersistence_.projectFolder().isEmpty()) {
        return lane.assetPath;
    }
    return QDir(projectPersistence_.projectFolder()).absoluteFilePath(lane.assetPath);
}

bool MainWindow::materializeLooperAssets(const QString& projectFolder)
{
    struct MaterializeResult {
        LooperProject project;

        QString projectFolder;
        QString error;
    };
    auto result = std::make_shared<MaterializeResult>();
    result->project = looperProject_;
    result->projectFolder = QDir(projectFolder).absolutePath();
    const QString sourceProjectFolder = projectPersistence_.projectFolder();
    const QByteArray sourceSnapshot = currentProjectSnapshot();
    QEventLoop waitLoop;
    const bool started = startFileWorkerTask(
        [result, sourceProjectFolder] {
            QDir folder(result->projectFolder);
            if (!folder.mkpath(QStringLiteral("wavs"))) {
                result->error = QStringLiteral("Could not create the project's wavs folder.");
                return;
            }
            for (LooperBank& bank : result->project.banks()) {
                for (LooperLane& lane : bank.lanes) {
                    if (lane.assetPath.trimmed().isEmpty()) {
                        continue;
                    }
                    const QString source = QFileInfo(lane.assetPath).isAbsolute() || sourceProjectFolder.isEmpty()
                        ? lane.assetPath
                        : QDir(sourceProjectFolder).absoluteFilePath(lane.assetPath);
                    if (!isSha256Hex(lane.assetHash) || !QFileInfo::exists(source)) {
                        result->error = QStringLiteral("A lane WAV is missing or has an invalid hash: %1").arg(lane.name);
                        return;
                    }
                    const StagedPcm16Asset materialized = stagePcm16Asset(source, result->projectFolder);
                    if (!materialized.error.isEmpty()) {
                        result->error = QStringLiteral("Could not materialize WAV %1: %2")
                            .arg(lane.name, materialized.error);
                        return;
                    }
                    if (materialized.sha256 != lane.assetHash) {
                        result->error = QStringLiteral("A lane WAV does not match its content hash: %1").arg(lane.name);
                        return;
                    }
                    lane.assetPath = QStringLiteral("wavs/") + lane.assetHash + QStringLiteral(".wav");
                }
            }
        },
        [&waitLoop] { waitLoop.quit(); },
        [&waitLoop, result](const QString& error) {
            result->error = error;
            waitLoop.quit();
        });
    if (!started) {
        QMessageBox::warning(this, QStringLiteral("Save Jam2 Song"), QStringLiteral("The bounded file worker is busy; try Save again."));
        return false;
    }
    QProgressDialog progress(
        QStringLiteral("Verifying and materializing project WAVs..."),
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
        QMessageBox::warning(this, QStringLiteral("Save Jam2 Song"), result->error);
        return false;
    }
    if (currentProjectSnapshot() != sourceSnapshot) {
        QMessageBox::warning(this, QStringLiteral("Save Jam2 Song"), QStringLiteral("The project changed while its WAVs were being materialized; Save again."));
        return false;
    }
    looperProject_ = std::move(result->project);
    projectPersistence_.setProjectFolder(result->projectFolder);
    return true;
}

void MainWindow::regeneratePreparedMix()
{
    ++preparedMixRequests_;
    if (preparedMixWorkerRunning_) {
        preparedMixRerunPending_ = true;
        ++preparedMixCoalesced_;
        return;
    }

    const int sampleRate = sampleRateSpin_ != nullptr ? sampleRateSpin_->value() : 48000;
    const QString cachePath = appReleaseFilePath(
        QStringLiteral("prepared_mixes"),
        QStringLiteral("active-bank-%1.wav").arg(looperProject_.activeBankIndex()));
    const LooperProject project = looperProject_;
    const QString projectFolder = projectPersistence_.projectFolder();
    const SharedTrackModel track = trackController_.model();
    QPointer<MainWindow> self(this);
    preparedMixWorkerRunning_ = true;
    fileWorkerPool_.start(QRunnable::create([
        self,
        project,
        projectFolder,
        sampleRate,
        cachePath,
        track
    ]() mutable {
        PreparedMixResult result;
        try {
            result = PreparedMixRenderer::render(
                project,
                projectFolder,
                sampleRate,
                cachePath,
                track);
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        } catch (...) {
            result.error = QStringLiteral("unknown prepared-mix worker exception");
        }
        if (self.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, result = std::move(result)]() mutable {
            if (self.isNull()) {
                return;
            }
            self->preparedMixWorkerRunning_ = false;
            if (self->preparedMixRerunPending_) {
                self->preparedMixRerunPending_ = false;
                self->regeneratePreparedMix();
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
    preparedMix_ = std::move(result);
    if (!preparedMix_.error.isEmpty()) {
        ++preparedMixFailures_;
        appendLog(QStringLiteral("prepared mix failed: %1 worker_requests=%2 worker_coalesced=%3 worker_failures=%4")
            .arg(preparedMix_.error)
            .arg(preparedMixRequests_)
            .arg(preparedMixCoalesced_)
            .arg(preparedMixFailures_));
        playPreparedMixWhenReady_ = false;
        return;
    }
    try {
        auto& track = trackController_.model();
        track.fileName = QStringLiteral("Prepared Bank %1").arg(looperProject_.banks().at(looperProject_.activeBankIndex()).id);
        track.filePath = preparedMix_.path;
        track.fileBytes = preparedMix_.fileBytes;
        track.sampleRate = preparedMix_.sampleRate;
        track.durationMs = preparedMix_.durationMs;
        track.sha256 = preparedMix_.sha256;
        updateTrackControls();
        loadTrackWaveform();
        loadPreparedMixIntoEngine();
        appendLog(QStringLiteral("prepared mix: %1 frames in %2 ms worker_requests=%3 worker_coalesced=%4 worker_failures=%5")
            .arg(preparedMix_.frames)
            .arg(preparedMix_.renderMs)
            .arg(preparedMixRequests_)
            .arg(preparedMixCoalesced_)
            .arg(preparedMixFailures_));
        if (trackController_.playback().phase ==
            SharedTrackController::PlaybackPhase::PreparingMix) {
            trackController_.preparedForTransport(
                trackController_.playback().arrangementRevision);
            updateTrackPlaybackPresentation();
        }
        if (sessionController_.isServer() && pendingSharedTrackRevision_ > 0) {
            pendingSharedTrackHostReady_ = true;
            maybeScheduleSharedTrackRestart();
        }
        if (pendingPreparedTrackReadyRevision_ > 0) {
            const int revision = pendingPreparedTrackReadyRevision_;
            pendingPreparedTrackReadyRevision_ = 0;
            playPreparedMixWhenReady_ = false;
            trackController_.preparedForTransport(static_cast<quint64>(revision));
            updateTrackPlaybackPresentation();
            sendControl(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("track.ready")},
                {QStringLiteral("arrangement_revision"), revision},
            });
            appendLog(QStringLiteral("prepared synchronized track revision %1; waiting for authority target")
                .arg(revision));
        } else if (playPreparedMixWhenReady_) {
            playPreparedMixWhenReady_ = false;
            restartPreparedTrackQuantized();
            if (gridScheduleLabel_) {
                gridScheduleLabel_->setText(QStringLiteral("Track restart: waiting for engine bar schedule"));
            }
        }
    } catch (const std::exception& error) {
        preparedMix_.error = QString::fromUtf8(error.what());
        ++preparedMixFailures_;
        appendLog(QStringLiteral("prepared mix metadata failed: %1 worker_failures=%2")
            .arg(preparedMix_.error)
            .arg(preparedMixFailures_));
        playPreparedMixWhenReady_ = false;
    }
}

void MainWindow::loadPreparedMixIntoEngine()
{
    if (!jam2_.isRunning() || preparedMix_.path.isEmpty() || !preparedMix_.error.isEmpty()) {
        return;
    }
    submitEngineText(
        jam2::EngineCommandType::LoadPreparedTrack,
        QDir::toNativeSeparators(preparedMix_.path),
        QStringLiteral("load prepared track"));
    const auto& model = trackController_.model();
    if (model.loopEnabled) {
        const qint64 loopStartFrame = model.loopStartSeconds >= 0.0
            ? qMax<qint64>(0, static_cast<qint64>(std::llround(
                model.loopStartSeconds * trackRecordingWorkflow_.preparedSampleRate())))
            : 0;
        const qint64 loopEndFrame = model.loopEndSeconds > model.loopStartSeconds
            ? qMax<qint64>(loopStartFrame + 1, static_cast<qint64>(std::llround(
                model.loopEndSeconds * trackRecordingWorkflow_.preparedSampleRate())))
            : qMax<qint64>(loopStartFrame + 1, preparedMix_.frames);
        setPreparedTrackLoop(
            true,
            static_cast<std::uint64_t>(loopStartFrame),
            static_cast<std::uint64_t>(loopEndFrame));
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
    if (trackSyncCheck_) {
        const QSignalBlocker blocker(trackSyncCheck_);
        trackSyncCheck_->setChecked(looperProject_.trackSyncEnabled());
    }
    const bool trackCompatible = trackController_.model().sampleRateCompatible;
    if (playTrackButton_) {
        playTrackButton_->setEnabled(trackCompatible);
        playTrackButton_->setToolTip(trackCompatible ? QString{} : QStringLiteral(
            "This track is quarantined because its sample rate does not match the jam"));
    }
    if (stopTrackButton_) {
        stopTrackButton_->setEnabled(trackCompatible);
        stopTrackButton_->setToolTip(playTrackButton_ ? playTrackButton_->toolTip() : QString{});
    }
}

void MainWindow::updateTrackPlaybackPresentation()
{
    if (!trackNameLabel_) {
        return;
    }
    trackNameLabel_->setText(QStringLiteral("Track: %1 | %2")
        .arg(
            trackController_.model().fileName,
            trackController_.playbackStatusText(looperProject_.trackSyncEnabled())));
}

void MainWindow::loadTrackMetadata()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Load WAV"),
        appReleaseFolderPath(QStringLiteral("captures")),
        QStringLiteral("WAV files (*.wav);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    auto metadata = std::make_shared<WavMetadata>();
    auto sidecar = std::make_shared<QJsonObject>();
    auto error = std::make_shared<QString>();
    (void)startFileWorkerTask(
        [path, metadata, sidecar, error] {
            try {
                *metadata = readWavMetadata(path);
                *sidecar = readSidecarJson(path);
            } catch (const std::exception& exception) {
                *error = QString::fromUtf8(exception.what());
            }
        },
        [this, info, metadata, sidecar, error] {
            if (!error->isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Jam2 Track"), *error);
                return;
            }
            const int expectedSampleRate = sampleRateSpin_ ? sampleRateSpin_->value() : 48000;
            if (metadata->sampleRate != expectedSampleRate) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Jam2 Track"),
                    QStringLiteral(
                        "Sample-rate mismatch: this jam uses %1 Hz but the WAV is %2 Hz. "
                        "The track was not loaded; convert it or use a %1 Hz source.")
                        .arg(expectedSampleRate)
                        .arg(metadata->sampleRate));
                return;
            }
            trackController_.model().fileName = info.fileName();
            trackController_.model().filePath = info.absoluteFilePath();
            trackController_.model().fileBytes = info.size();
            trackController_.model().sampleRate = metadata->sampleRate;
            trackController_.model().sampleRateCompatible = true;
            trackController_.model().durationMs = sidecar->value(QStringLiteral("duration_ms")).toInt(metadata->durationMs);
            trackController_.model().sha256 = metadata->sha256;
            trackController_.model().guessedBpm = 0.0;
            trackController_.model().acceptedBpm = sidecar->value(QStringLiteral("accepted_bpm")).toDouble(120.0);
            trackController_.model().key = QStringLiteral("Unknown");
            trackController_.model().loopEnabled = true;
            trackController_.model().loopStartSeconds = -1.0;
            trackController_.model().loopEndSeconds = -1.0;
            updateTrackControls();
            loadTrackWaveform();
        });
}

void MainWindow::chooseCaptureFolder()
{
    const QString current = captureOutputEdit_->text().trimmed();
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Take WAV"),
        isAutoCapturePath(current) ? timestampedCapturePath(QStringLiteral("take")) : current,
        QStringLiteral("WAV files (*.wav);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (!path.isEmpty()) {
        captureOutputEdit_->setText(path);
    }
}

void MainWindow::refreshLoopbackSources()
{
    const QString previous = loopbackSourceBox_->currentData().toString().isEmpty()
        ? loopbackSourceBox_->currentText()
        : loopbackSourceBox_->currentData().toString();
    loopbackSourceBox_->clear();
    QString error;
    const QStringList sources = GuiLoopbackRecorder::listSources(&error);

    const QRegularExpression re(QStringLiteral("^\\s*\\[([^\\]]+)\\]\\s*(.*)$"));
    for (const QString& line : sources) {
        const QString trimmed = line.trimmed();
        const QRegularExpressionMatch match = re.match(trimmed);
        if (match.hasMatch()) {
            loopbackSourceBox_->addItem(trimmed, match.captured(1));
        }
    }
    if (loopbackSourceBox_->count() == 0) {
        loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
        appendLog(error.isEmpty() ? QStringLiteral("no loopback sources returned") : error);
    } else {
        appendLog(QStringLiteral("loaded %1 loopback sources").arg(loopbackSourceBox_->count()));
    }
    const int restore = loopbackSourceBox_->findData(previous);
    if (restore >= 0) {
        loopbackSourceBox_->setCurrentIndex(restore);
    }
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
    QString output = captureOutputEdit_->text().trimmed();
    if (isAutoCapturePath(output)) {

        output = timestampedCapturePath(QStringLiteral("track-input"));
        captureOutputEdit_->setText(output);
    }
    QString workflowError;
    if (!trackRecordingWorkflow_.startInputTake(
            output,
            !QFileInfo::exists(output),
            targetFrame,
            countInBars >= 0 ? std::optional<int>(countInBars) : std::nullopt,
            metronomeTransport_.grid().position(),
            currentMetronomePattern().beats_per_bar,
            workflowError)) {
        appendLog(QStringLiteral("could not start input take: ") + workflowError);
        return;
    }
    if (countInBars >= 0 && recordingCountdownLabel_) {
        recordingCountdownLabel_->setText(QStringLiteral("WAITING FOR NEXT BAR..."));
        recordingCountdownLabel_->show();
    }
    const QString startText = countInBars >= 0
        ? QStringLiteral("Recording: armed input take, engine_quantized_count_in_bars=%1 latency_compensation_frames=%2 output=%3")
            .arg(countInBars)
            .arg(trackRecordingWorkflow_.appliedLatencyFrames())
            .arg(output)
        : QStringLiteral("Recording: armed input take, start_frame=%1 latency_compensation_frames=%2 output=%3")
            .arg(targetFrame)
            .arg(trackRecordingWorkflow_.appliedLatencyFrames())
            .arg(output);
    if (gridScheduleLabel_) {
        gridScheduleLabel_->setText(startText);
    }
    appendLog(startText);
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
    if (countInBars < 0 && (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked())) {
        QTimer::singleShot(captureDurationSpin_->value() * 1000, this, [this] {
            if (trackRecordingWorkflow_.inputTakeActive()) {
                runGridLockedEngineAction(
                    QStringLiteral("record.stop"),
                    [this](std::uint64_t stopFrame) { stopInputCapture(stopFrame); },
                    true);
            }
        });
    }
}

void MainWindow::startLoopbackCapture()
{
    if (loopbackRecorder_.isRunning()) {
        return;
    }
    QString output = captureOutputEdit_->text().trimmed();
    if (isAutoCapturePath(output)) {
        output = timestampedCapturePath(QStringLiteral("loopback"));
        captureOutputEdit_->setText(output);
    }
    trackRecordingWorkflow_.beginLoopbackCapture(output, !QFileInfo::exists(output));
    QString source = loopbackSourceBox_->currentData().toString();
    if (source.isEmpty()) {
        source = loopbackSourceBox_->currentText().trimmed();
    }
    if (source.isEmpty()) {
        source = QStringLiteral("default");
    }

    GuiLoopbackOptions options;
    options.source = source;
    options.outputPath = output;
    options.durationMs = (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked()) ? captureDurationSpin_->value() * 1000 : 0;
    options.trigger = captureTriggerCheck_ && captureTriggerCheck_->isChecked();
    options.triggerThresholdDb = triggerThresholdSpin_ ? triggerThresholdSpin_->value() : -45.0;
    options.triggerHoldMs = triggerHoldSpin_ ? triggerHoldSpin_->value() : 50;
    options.preRollMs = preRollSpin_ ? preRollSpin_->value() : 250;
    options.tailSilenceDb = tailThresholdSpin_ ? tailThresholdSpin_->value() : -50.0;
    options.tailSilenceMs = tailSilenceSpin_ ? tailSilenceSpin_->value() : 1000;
    options.trimLeadingSilence = trimLeadingCheck_ && trimLeadingCheck_->isChecked();
    options.trimTrailingSilence = trimTrailingCheck_ && trimTrailingCheck_->isChecked();

    QString error;
    appendLog(QStringLiteral("starting internal loopback recording: %1").arg(output));
    if (!loopbackRecorder_.start(options, [this](bool ok, const QString& outputPath, const QString& errorText) {
            QMetaObject::invokeMethod(this, [this, ok, outputPath, errorText] {
                if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
                const QString transientPath = trackRecordingWorkflow_.finishLoopbackCapture(outputPath);
                if (!ok) {
                    if (!transientPath.isEmpty() && QFileInfo::exists(transientPath)) {
                        registerTransientTrackWav(transientPath);
                    }
                    if (loadWavButton_) loadWavButton_->setEnabled(true);
                    appendLog(QStringLiteral("loopback recording failed: ") + errorText);
                    return;
                }
                if (!transientPath.isEmpty()) {
                    registerTransientTrackWav(transientPath);
                }
                if (loadWavButton_) {
                    loadWavButton_->setEnabled(true);
                }
                if (trackRecordingWorkflow_.laneArmed()) {
                    importLastCaptureToArmedLane();
                }
            }, Qt::QueuedConnection);
        }, &error)) {
        const QString transientPath = trackRecordingWorkflow_.abandonPendingCapture();
        if (!transientPath.isEmpty() && QFileInfo::exists(transientPath)) {
            registerTransientTrackWav(transientPath);
        }
        if (loadWavButton_) loadWavButton_->setEnabled(true);
        appendLog(QStringLiteral("loopback recording failed to start: ") + error);
        return;

    }
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
}

void MainWindow::stopInputCapture(std::uint64_t targetFrame)
{
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
                loadTrackWaveform();
                return;
            }
            if (trackWaveform_) {
                trackWaveform_->setPeaks(std::move(*peaks), *valid);
                trackWaveform_->setDurationMs(trackController_.model().durationMs);
                trackWaveform_->setBpm(trackController_.model().acceptedBpm);
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
    if (!jam2_.isRunning()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Start the local engine before playing the prepared mix."));
        return;
    }
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    publishStoppedTrackStateWhenApplied_ = false;
    trackController_.requestPlayback(true);
    updateTrackPlaybackPresentation();
    const bool sharedAuthority = looperProject_.trackSyncEnabled() &&
        session.role == SharedSessionController::Role::Creator && session.remotePeerCount > 0;
    if (preparedMixWorkerRunning_ || preparedMix_.path.isEmpty() || !preparedMix_.error.isEmpty()) {
        if (sharedAuthority) {
            playPreparedMixWhenReady_ = false;
            regeneratePreparedMix();
            sendSongSnapshot(true);
            if (gridScheduleLabel_) {
                gridScheduleLabel_->setText(QStringLiteral("Track play: waiting for shared asset readiness"));
            }
            return;
        }
        playPreparedMixWhenReady_ = true;
        regeneratePreparedMix();
        return;
    }
    if (sharedAuthority) {
        sendSongSnapshot(true);
        if (gridScheduleLabel_) {
            gridScheduleLabel_->setText(QStringLiteral("Track play: waiting for shared asset readiness"));
        }
        return;
    }
    loadPreparedMixIntoEngine();
    restartPreparedTrackQuantized();
    if (gridScheduleLabel_) {
        gridScheduleLabel_->setText(QStringLiteral("Track restart: waiting for engine bar schedule"));
    }
}

void MainWindow::stopTrack(std::uint64_t targetFrame)
{
    if (!jam2_.isRunning()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Start the local engine before stopping the prepared mix."));
        return;
    }
    if (!trackRecordingWorkflow_.stopPrepared(
            targetFrame,
            metronomeTransport_.grid().position().currentFrame,
            looperProject_.trackSyncEnabled())) {
        appendLog(QStringLiteral("engine command queue unavailable: stop prepared track"));
        return;
    }
    trackController_.requestPlayback(false);
    updateTrackPlaybackPresentation();
    if (looperProject_.trackSyncEnabled() && sessionController_.isServer()) {
        pendingSharedTrackRevision_ = 0;
        pendingSharedTrackHostReady_ = true;
        pendingSharedTrackReadyTokens_.clear();
        publishStoppedTrackStateWhenApplied_ = true;
    }
    updateTrackTimeline();
}

void MainWindow::setLoopStartAtCurrentPosition()
{
    const qint64 duration = trackController_.model().durationMs;
    const qint64 position = qBound<qint64>(0, currentAudibleTrackPositionMs(), duration);
    auto& model = trackController_.model();
    model.loopStartSeconds = static_cast<double>(position) / 1000.0;
    model.loopEnabled = true;
    updateTrackControls();
    updateTrackTimeline();
    loadPreparedMixIntoEngine();
}

void MainWindow::setLoopEndAtCurrentPosition()
{
    const qint64 duration = trackController_.model().durationMs;
    const qint64 position = qBound<qint64>(0, currentAudibleTrackPositionMs(), duration);
    auto& model = trackController_.model();
    model.loopEndSeconds = static_cast<double>(position) / 1000.0;
    model.loopEnabled = true;

    updateTrackControls();
    updateTrackTimeline();
    loadPreparedMixIntoEngine();
}

void MainWindow::clearTrackLoop()
{
    auto& model = trackController_.model();
    model.loopEnabled = false;
    model.loopStartSeconds = -1.0;
    model.loopEndSeconds = -1.0;
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
    if (mixTrackMeter_) {
        mixTrackMeter_->setLevel(0.0);
    }
    if (!jam2_.isRunning() && mixMetronomeMeter_) {
        mixMetronomeMeter_->setLevel(0.0);
    }
}

void MainWindow::startTrackMetronome()
{
    if (jam2_.isRunning()) {
        if (metronomeTransport_.grid().position().running) {
            // Record count-in and duplicate UI activation must not create a
            // second grid proposal while the shared clock is already running.
            if (trackMetronomeLabel_) {
                trackMetronomeLabel_->setText(QStringLiteral("Jam metronome running"));
            }
            if (startTrackMetronomeButton_) {
                startTrackMetronomeButton_->setEnabled(false);
            }
            if (stopTrackMetronomeButton_) {
                stopTrackMetronomeButton_->setEnabled(true);
            }
            return;
        }
        metronomeTransport_.setLocalState(true, true);
        updateRuntimeControls();
        submitEngineToggle(
            jam2::EngineCommandType::SetLeaderAudioLocalClick,
            true,
            QStringLiteral("metronome leader"));
        submitEngineToggle(
            jam2::EngineCommandType::SetMetronomeEnabled,
            true,
            QStringLiteral("metronome enabled"));
        sendMetronomeSettingsToPeer();
        if (trackMetronomeLabel_) {
            trackMetronomeLabel_->setText(QStringLiteral("Jam metronome running"));
        }
        if (startTrackMetronomeButton_) {
            startTrackMetronomeButton_->setEnabled(false);
        }
        if (stopTrackMetronomeButton_) {
            stopTrackMetronomeButton_->setEnabled(true);
        }
        return;
    }

    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Start local engine for metronome"));
    }
    QMessageBox::warning(this, QStringLiteral("Jam2 Metronome"), QStringLiteral("Start the local engine before using the track metronome."));
}

void MainWindow::stopTrackMetronome()
{
    const bool jamMetronomeWasRunning = jam2_.isRunning() && metronomeTransport_.localRunning();
    metronomeTransport_.setLocalState(false, false);
    if (jamMetronomeWasRunning) {
        submitEngineToggle(
            jam2::EngineCommandType::SetLeaderAudioLocalClick,
            false,
            QStringLiteral("metronome leader"));
        submitEngineToggle(
            jam2::EngineCommandType::SetMetronomeEnabled,
            false,
            QStringLiteral("metronome enabled"));
        sendMetronomeSettingsToPeer();
    }
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(jam2_.isRunning()
            ? QStringLiteral("Jam metronome stopped")
            : QStringLiteral("Local metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
}

void MainWindow::updateTrackMetronomeInterval()
{
    if (bpmSpin_ && metronomeBpmSpin_) {
        bpmSpin_->setValue(metronomeBpmSpin_->value());
    }
    sendMetronomePatternToJam();
    sendMetronomeSettingsToPeer();
}

void MainWindow::rebuildMetronomePattern(bool resetToDivisionDefault)
{
    if (!metronomePatternTable_) {
        return;
    }
    const int beats = metronomeBeatsSpin_ ? metronomeBeatsSpin_->value() : 4;
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
            : (division <= 1 ? i == 0 : i % division == 0);
    }

    metronomePatternTable_->clear();
    metronomePatternTable_->setRowCount(2);
    metronomePatternTable_->setColumnCount(steps);
    QStringList headers;
    headers.reserve(steps);
    for (int step = 0; step < steps; ++step) {
        headers << QStringLiteral("%1").arg(step + 1);
        auto* playCell = new QWidget(metronomePatternTable_);
        auto* playLayout = new QHBoxLayout(playCell);
        playLayout->setContentsMargins(0, 0, 0, 0);
        playLayout->setAlignment(Qt::AlignCenter);
        auto* playCheck = new QCheckBox(playCell);
        playCheck->setChecked(metronomeEnabledSteps_[step]);
        QObject::connect(playCheck, &QCheckBox::toggled, this, [this, step](bool checked) {
            if (step >= 0 && step < metronomeEnabledSteps_.size()) {
                metronomeEnabledSteps_[step] = checked;
                sendMetronomePatternToJam();
                sendMetronomeSettingsToPeer();
            }
        });
        playLayout->addWidget(playCheck);
        metronomePatternTable_->setCellWidget(0, step, playCell);

        auto* accentCell = new QWidget(metronomePatternTable_);
        auto* accentLayout = new QHBoxLayout(accentCell);
        accentLayout->setContentsMargins(0, 0, 0, 0);
        accentLayout->setAlignment(Qt::AlignCenter);
        auto* accentCheck = new QCheckBox(accentCell);
        accentCheck->setChecked(metronomeAccents_[step]);
        QObject::connect(accentCheck, &QCheckBox::toggled, this, [this, step](bool checked) {
            if (step >= 0 && step < metronomeAccents_.size()) {
                metronomeAccents_[step] = checked;
                sendMetronomePatternToJam();
                sendMetronomeSettingsToPeer();
            }
        });
        accentLayout->addWidget(accentCheck);
        metronomePatternTable_->setCellWidget(1, step, accentCell);
    }
    metronomePatternTable_->setHorizontalHeaderLabels(headers);
    metronomePatternTable_->setVerticalHeaderLabels(QStringList{QStringLiteral("Play"), QStringLiteral("Accent")});
    sendMetronomePatternToJam();
    sendMetronomeSettingsToPeer();
}

jam2::metronome::PatternSnapshot MainWindow::currentMetronomePattern() const
{
    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 120;
    pattern.beats_per_bar = metronomeBeatsSpin_ ? metronomeBeatsSpin_->value() : 4;
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
            : (pattern.division <= 1 ? step == 0 : step % pattern.division == 0);
        jam2::metronome::set_mask_enabled(pattern.play_mask_low, pattern.play_mask_high, step, play);
        jam2::metronome::set_mask_enabled(pattern.accent_mask_low, pattern.accent_mask_high, step, accent);
    }
    return jam2::metronome::sanitize(pattern);
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

void MainWindow::showMetronomeCompensationDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Listener Compensation"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto makeSpin = [&dialog](double value, double max, const QString& suffix) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(0.0, max);
        spin->setDecimals(1);
        spin->setSuffix(suffix);
        spin->setValue(value);
        applyMutedEditorStyle(spin);
        return spin;
    };

    auto* maxSpin = makeSpin(metronomeCompensationMaxSpin_ ? metronomeCompensationMaxSpin_->value() : 250.0, 1000.0, QStringLiteral(" ms"));
    auto* smoothingSpin = makeSpin(metronomeCompensationSmoothingSpin_ ? metronomeCompensationSmoothingSpin_->value() : 750.0, 10000.0, QStringLiteral(" ms"));
    auto* deadbandSpin = makeSpin(metronomeCompensationDeadbandSpin_ ? metronomeCompensationDeadbandSpin_->value() : 1.0, 1000.0, QStringLiteral(" ms"));
    auto* slewSpin = makeSpin(metronomeCompensationSlewSpin_ ? metronomeCompensationSlewSpin_->value() : 40.0, 10000.0, QStringLiteral(" ms/s"));

    form->addRow(QStringLiteral("Max offset"), maxSpin);
    form->addRow(QStringLiteral("Smoothing"), smoothingSpin);
    form->addRow(QStringLiteral("Deadband"), deadbandSpin);
    form->addRow(QStringLiteral("Slew limit"), slewSpin);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (metronomeCompensationMaxSpin_) {
        metronomeCompensationMaxSpin_->setValue(maxSpin->value());
    }
    if (metronomeCompensationSmoothingSpin_) {
        metronomeCompensationSmoothingSpin_->setValue(smoothingSpin->value());
    }
    if (metronomeCompensationDeadbandSpin_) {
        metronomeCompensationDeadbandSpin_->setValue(deadbandSpin->value());
    }
    if (metronomeCompensationSlewSpin_) {
        metronomeCompensationSlewSpin_->setValue(slewSpin->value());
    }
}

void MainWindow::updateMetronomeCompensationVisibility()
{
    if (!metronomeCompensationButton_ || !metronomeModeBox_) {
        return;
    }
    metronomeCompensationButton_->setVisible(
        metronomeModeBox_->currentText() == QStringLiteral("listener-compensated"));
}

void MainWindow::sendMetronomePatternToJam()
{
    if (!jam2_.isRunning() ||
        !metronomeTransport_.localGridMutationAllowed()) {
        return;
    }
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::SetMetronomePattern;

    command.pattern = pattern;
    if (!metronomeTransport_.submit(command)) {
        appendLog(QStringLiteral("engine command rejected: metronome pattern"));
    }
}

void MainWindow::sendMetronomeSettingsToPeer()
{
    if (metronomeTransport_.applyingRemoteSettings() || !jam2_.isRunning()) {
        return;
    }
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("metronome.settings")},
        {QStringLiteral("running"), metronomeTransport_.localRunning()},
        {QStringLiteral("leader"), metronomeTransport_.localLeader()},
        {QStringLiteral("mode"), metronomeModeBox_ ? metronomeModeBox_->currentText() : QStringLiteral("shared-grid")},
        {QStringLiteral("bpm"), pattern.bpm},
        {QStringLiteral("beats"), pattern.beats_per_bar},
        {QStringLiteral("division"), pattern.division},
        {QStringLiteral("play_mask_low"), QString::number(static_cast<qulonglong>(pattern.play_mask_low), 16)},
        {QStringLiteral("play_mask_high"), QString::number(static_cast<qulonglong>(pattern.play_mask_high), 16)},
        {QStringLiteral("accent_mask_low"), QString::number(static_cast<qulonglong>(pattern.accent_mask_low), 16)},
        {QStringLiteral("accent_mask_high"), QString::number(static_cast<qulonglong>(pattern.accent_mask_high), 16)},
    });
}

void MainWindow::applyRemoteMetronomeSettings(const QJsonObject& message)
{
    auto parseMask = [](const QJsonObject& object, const QString& key) {
        bool ok = false;
        const QString text = object.value(key).toString();
        const qulonglong value = text.toULongLong(&ok, 16);
        return ok ? static_cast<std::uint64_t>(value) : 0ULL;
    };



    const bool running = message.value(QStringLiteral("running")).toBool(metronomeTransport_.localRunning());
    const bool remoteLeader = message.value(QStringLiteral("leader")).toBool(running);
    const QString mode = message.value(QStringLiteral("mode")).toString(metronomeModeBox_
        ? metronomeModeBox_->currentText()
        : QStringLiteral("shared-grid"));
    const int bpm = qBound(1, message.value(QStringLiteral("bpm")).toInt(metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 120), 400);
    const int beats = qBound(1, message.value(QStringLiteral("beats")).toInt(metronomeBeatsSpin_ ? metronomeBeatsSpin_->value() : 4), 16);
    const int division = qMax(1, message.value(QStringLiteral("division")).toInt(metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1));
    const std::uint64_t playLow = parseMask(message, QStringLiteral("play_mask_low"));
    const std::uint64_t playHigh = parseMask(message, QStringLiteral("play_mask_high"));
    const std::uint64_t accentLow = parseMask(message, QStringLiteral("accent_mask_low"));
    const std::uint64_t accentHigh = parseMask(message, QStringLiteral("accent_mask_high"));

    metronomeTransport_.setApplyingRemoteSettings(true);
    if (metronomeBpmSpin_) {
        const QSignalBlocker blocker(metronomeBpmSpin_);
        metronomeBpmSpin_->setValue(bpm);
    }
    if (bpmSpin_) {
        const QSignalBlocker blocker(bpmSpin_);
        bpmSpin_->setValue(bpm);
    }
    if (metronomeBeatsSpin_) {
        const QSignalBlocker blocker(metronomeBeatsSpin_);
        metronomeBeatsSpin_->setValue(beats);
    }
    if (metronomeDivisionBox_) {
        const QSignalBlocker blocker(metronomeDivisionBox_);
        const int index = metronomeDivisionBox_->findData(division);
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

    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = bpm;
    pattern.beats_per_bar = beats;
    pattern.division = division;
    pattern.step_count = jam2::metronome::pattern_step_count(beats, division);
    pattern.play_mask_low = playLow;
    pattern.play_mask_high = playHigh;
    pattern.accent_mask_low = accentLow;
    pattern.accent_mask_high = accentHigh;
    pattern = jam2::metronome::sanitize(pattern);

    metronomeEnabledSteps_.resize(pattern.step_count);
    metronomeAccents_.resize(pattern.step_count);
    for (int step = 0; step < pattern.step_count; ++step) {
        metronomeEnabledSteps_[step] = jam2::metronome::mask_enabled(pattern.play_mask_low, pattern.play_mask_high, step);
        metronomeAccents_[step] = jam2::metronome::mask_enabled(pattern.accent_mask_low, pattern.accent_mask_high, step);
    }
    rebuildMetronomePattern();
    bool localLeader = metronomeTransport_.localLeader();
    if (!running || remoteLeader) {
        localLeader = false;
    }
    metronomeTransport_.setLocalState(running, localLeader);
    metronomeTransport_.setApplyingRemoteSettings(false);
    // This authenticated TCP message updates presentation only. The native UDP
    // authority state applies the run state, pattern, mode, mapped epoch, and
    // local authority role to the engine. Re-submitting them here would create
    // a competing local grid proposal and can replace the starter's fresh epoch.
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(running
            ? QStringLiteral("Jam metronome running")
            : QStringLiteral("Jam metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(!running);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(running);
    }
}

void MainWindow::publishLocalTrackOffer(
    int bankIndex,
    const QString& targetLaneId,
    const LooperLane& lane)
{
    if (!looperProject_.trackSyncEnabled() || !jam2_.isRunning() ||
        sessionController_.snapshot().role != SharedSessionController::Role::Joiner ||
        bankIndex < 0 || bankIndex >= looperProject_.banks().size() || targetLaneId.isEmpty() ||
        !lane.sampleRateCompatible || lane.sampleRate <= 0 ||
        !isSha256Hex(lane.assetHash) || lane.assetPath.isEmpty()) {
        return;
    }
    QByteArray contributionIdentity = QCryptographicHash::hash(
        QStringLiteral("%1:%2:%3")
            .arg(bankIndex)
            .arg(targetLaneId, lane.assetHash)
            .toUtf8(),
        QCryptographicHash::Sha256).left(16);
    const QString contributionId = QUuid::fromRfc4122(contributionIdentity)
        .toString(QUuid::WithoutBraces).toLower();
    if (!localTrackOffers_.contains(contributionId) &&
        localTrackOffers_.size() >= kMaxLooperTrackContributions) {
        appendLog(QStringLiteral("Track Sync contribution limit reached; track not offered: ") + lane.name);
        return;
    }
    while (!trackOfferAssetPaths_.contains(lane.assetHash) &&
           trackOfferAssetPaths_.size() >= kMaxLooperTrackContributions) {
        trackOfferAssetPaths_.erase(trackOfferAssetPaths_.begin());
    }
    const QJsonObject offer{
        {QStringLiteral("type"), QStringLiteral("looper.recording.offer")},
        {QStringLiteral("recording_id"), contributionId},

        {QStringLiteral("bank"), bankIndex},
        {QStringLiteral("target_lane_id"), targetLaneId},
        {QStringLiteral("sha256"), lane.assetHash},
        {QStringLiteral("name"), lane.name.left(512)},
        {QStringLiteral("sample_rate"), lane.sampleRate},
    };
    localTrackOffers_.insert(contributionId, offer);
    trackOfferAssetPaths_.insert(lane.assetHash, lane.assetPath);
    sendControl(offer);
    appendLog(QStringLiteral("offered local track for additive Track Sync: %1 hash=%2")
        .arg(lane.name, lane.assetHash));
}

void MainWindow::shareLocalTracks()
{
    if (!looperProject_.trackSyncEnabled() || !jam2_.isRunning()) {
        appendLog(QStringLiteral("Share Tracks requires an active jam with Sync track controls enabled"));
        return;
    }
    if (sessionController_.isServer()) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.track.share.request")},
        });
        sendSongSnapshot();
        appendLog(QStringLiteral("shared creator tracks and requested additive peer contributions"));
        return;
    }
    if (sessionController_.snapshot().role != SharedSessionController::Role::Joiner) {
        return;
    }
    int offered = 0;
    for (int bankIndex = 0; bankIndex < looperProject_.banks().size(); ++bankIndex) {
        const LooperBank& bank = looperProject_.banks().at(bankIndex);
        for (const LooperLane& lane : bank.lanes) {
            if (!lane.sampleRateCompatible || lane.sampleRate <= 0 ||
                !isSha256Hex(lane.assetHash) || lane.assetPath.isEmpty()) {
                continue;
            }
            publishLocalTrackOffer(bankIndex, lane.id, lane);
            ++offered;
        }
    }
    appendLog(QStringLiteral("Share Tracks offered %1 local asset-backed track(s) additively")
        .arg(offered));
}

void MainWindow::handleTrackOffer(const QJsonObject& message, const QString& sourcePeerToken)
{
    if (!sessionController_.isServer() || sourcePeerToken.isEmpty()) {
        appendLog(QStringLiteral("rejected track offer from a non-joiner control path"));
        return;
    }
    const QString contributionId = message.value(QStringLiteral("recording_id")).toString().toLower();
    if (appliedTrackContributionIds_.contains(contributionId)) {
        return;
    }
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    if (!pendingTrackContributions_.contains(contributionId)) {
        if (pendingTrackContributions_.size() >= kMaxLooperTrackContributions) {
            appendLog(QStringLiteral("track contribution queue is full"));
            return;
        }
        pendingTrackContributions_.insert(contributionId, PendingTrackContribution{
            sourcePeerToken,
            message.value(QStringLiteral("bank")).toInt(),
            message.value(QStringLiteral("target_lane_id")).toString(),
            hash,
            message.value(QStringLiteral("name")).toString(),
            message.value(QStringLiteral("sample_rate")).toInt(),
        });
    }
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
    if (validatedTrackAssetHashes_.contains(hash)) {
        applyPendingTrackContributions();
    } else {
        requestNextPendingTrackAsset();
    }
}

void MainWindow::requestNextPendingTrackAsset()
{
    if (!sessionController_.isServer() || !pendingTrackAssetSources_.isEmpty()) {
        return;
    }
    for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
        if (validatedTrackAssetHashes_.contains(contribution.assetHash)) {
            continue;
        }
        if (!pendingLooperAssetHashes_.contains(contribution.assetHash)) {
            pendingLooperAssetHashes_.append(contribution.assetHash);
        }
        pendingTrackAssetSources_.insert(
            contribution.assetHash,
            contribution.sourcePeerToken);
        QJsonArray hashes;
        hashes.append(contribution.assetHash);
        if (!sendControlTo(contribution.sourcePeerToken, QJsonObject{
                {QStringLiteral("type"), QStringLiteral("looper.asset.request")},
                {QStringLiteral("hashes"), hashes},
            })) {
            pendingTrackAssetSources_.remove(contribution.assetHash);
            pendingLooperAssetHashes_.removeAll(contribution.assetHash);
            appendLog(QStringLiteral("could not request offered track asset from peer ") +
                contribution.sourcePeerToken.left(8));
        }
        return;
    }
}

void MainWindow::applyPendingTrackContributions()
{
    if (!sessionController_.isServer()) {
        return;
    }
    QStringList completedIds;
    bool arrangementChanged = false;
    for (auto it = pendingTrackContributions_.constBegin();
         it != pendingTrackContributions_.constEnd(); ++it) {
        const PendingTrackContribution& contribution = it.value();
        if (!validatedTrackAssetHashes_.contains(contribution.assetHash) ||
            contribution.bankIndex < 0 || contribution.bankIndex >= looperProject_.banks().size()) {
            continue;
        }
        const QString assetPath = looperAssetPathForHash(contribution.assetHash);
        auto& lanes = looperProject_.banks()[contribution.bankIndex].lanes;
        int targetIndex = -1;
        for (int index = 0; index < lanes.size(); ++index) {
            if (lanes.at(index).id == contribution.targetLaneId) {
                targetIndex = index;
                break;
            }
        }
        int existingContributionIndex = -1;
        for (int index = 0; index < lanes.size(); ++index) {
            if (lanes.at(index).id == it.key()) {
                existingContributionIndex = index;
                break;
            }
        }
        const bool targetReservedForLocalRecording =
            trackRecordingWorkflow_.laneArmedAt(contribution.bankIndex, targetIndex);
        bool applied = false;
        if ((targetIndex >= 0 && lanes.at(targetIndex).assetHash == contribution.assetHash) ||
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
            if (!contribution.name.trimmed().isEmpty() &&
                (lane.name.trimmed().isEmpty() || isDefaultEmptyTrackName(lane.name))) {
                lane.name = contribution.name;
            }
            lane.startFrame = 0;
            lane.stopFrame = -1;
            lane.loopStartFrame = -1;
            lane.loopEndFrame = -1;
            lane.loopEnabled = false;
            arrangementChanged = true;
            applied = true;
        } else {
            LooperLane lane;
            lane.id = targetIndex < 0 ? contribution.targetLaneId : it.key();
            lane.assetPath = assetPath;
            lane.assetHash = contribution.assetHash;
            lane.name = contribution.name.trimmed().isEmpty()
                ? QStringLiteral("Peer track") : contribution.name;
            lane.sampleRate = contribution.sampleRate;
            lane.sampleRateCompatible = true;
            if (looperProject_.appendLane(contribution.bankIndex, std::move(lane))) {
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
    for (const QString& contributionId : completedIds) {
        pendingTrackContributions_.remove(contributionId);
        appliedTrackContributionIds_.insert(contributionId);
    }
    while (appliedTrackContributionIds_.size() > kMaxLooperTrackContributions * 2) {
        appliedTrackContributionIds_.erase(appliedTrackContributionIds_.begin());
    }
    if (arrangementChanged) {
        refreshLooperLanes();
        regeneratePreparedMix();
        if (looperProject_.trackSyncEnabled() && jam2_.isRunning()) {
            sendSongSnapshot();
        }
    }
    requestNextPendingTrackAsset();
}

bool MainWindow::canQueueControlTo(const QString& targetPeerToken, qint64 estimatedBytes) const
{
    return sessionController_.canQueueTo(targetPeerToken, estimatedBytes);
}

bool MainWindow::sendControlTo(const QString& targetPeerToken, const QJsonObject& message)
{
    if (jam2::application::isTrackSyncControlMessageType(
            message.value(QStringLiteral("type")).toString()) &&
        !looperProject_.trackSyncEnabled()) {
        appendLog(QStringLiteral("suppressed local track sync while sync is disabled"));
        return false;
    }
    return sessionController_.sendTo(targetPeerToken, message);
}

bool MainWindow::sendBinaryControlTo(
    const QString& targetPeerToken,
    const QByteArray& payload)
{
    if (!looperProject_.trackSyncEnabled()) {
        appendLog(QStringLiteral("suppressed local asset sync while sync is disabled"));
        return false;
    }
    return sessionController_.sendBinaryTo(targetPeerToken, payload);
}

void MainWindow::handleSongSet(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const bool hostAuthoritative =
        message.value(QStringLiteral("host_authoritative")).toBool(false);
    const bool fromPeerProposal = sessionController_.isServer() &&
        !sourcePeerToken.isEmpty() && !hostAuthoritative;
    if (sessionController_.isServer() && !sourcePeerToken.isEmpty() &&
        hostAuthoritative) {
        appendLog(QStringLiteral("rejected peer attempt to mark an arrangement proposal authoritative"));
        return;
    }
    if (!hostAuthoritative && !fromPeerProposal) {
        appendLog(QStringLiteral("rejected arrangement outside the collaborative proposal or creator snapshot path"));
        return;
    }
    if (!looperProject_.trackSyncEnabled()) {
        appendLog(fromPeerProposal
            ? QStringLiteral("ignored collaborative arrangement proposal while Track Sync is disabled")
            : QStringLiteral("ignored creator arrangement while Track Sync is disabled"));
        const qint64 revision = message.value(QStringLiteral("arrangement_revision")).toInteger();
        if (hostAuthoritative &&
            message.value(QStringLiteral("track_playing")).toBool(false) &&
            revision > 0) {
            // This readiness acknowledgement does not apply or originate a
            // track action. It only keeps an opted-out peer from blocking the
            // creator's bounded shared-asset barrier.
            sendControl(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("track.ready")},
                {QStringLiteral("arrangement_revision"), revision},
            });
        }
        return;
    }
    if (sessionController_.isServer() && hostAuthoritative) {
        appendLog(QStringLiteral("ignored host-authoritative song snapshot while hosting"));
        return;
    }
    const int revision = message.value(QStringLiteral("arrangement_revision")).toInt(
        message.value(QStringLiteral("revision")).toInt(0));
    const bool restartTrack = fromPeerProposal
        ? trackRecordingWorkflow_.preparedPlaying()
        : message.value(QStringLiteral("track_playing")).toBool(false);
    if (hostAuthoritative &&
        revision <= lastAppliedHostArrangementRevision_) {
        appendLog(QStringLiteral("ignored stale host arrangement revision %1").arg(revision));
        return;
    }
    const QJsonObject normalizedSong =
        normalizeLooperAssetPaths(message.value(QStringLiteral("song")).toObject());
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
    const QJsonObject song = preserveQuarantinedLocalLanes(normalizedSong);
    const QString assetFolder = QDir(projectPersistence_.workspaceFolder()).absoluteFilePath(QStringLiteral("wavs"));
    const std::uint64_t checkRevision = ++songAssetCheckRevision_;
    auto missing = std::make_shared<QStringList>();
    auto assetFailure = std::make_shared<QString>();
    const int expectedSampleRate = sessionController_.snapshot().contract.sampleRate;
    const bool started = startFileWorkerTask(
        [referencedHashes, assetFolder, expectedSampleRate, missing, assetFailure] {
            for (const QString& hash : referencedHashes) {
                if (!isSha256Hex(hash)) {
                    missing->append(hash);
                    continue;
                }
                const QString path = QDir(assetFolder).absoluteFilePath(hash + QStringLiteral(".wav"));
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                if (!QFileInfo::exists(path) || !inspected || sha256FileHex(path) != hash) {
                    missing->append(hash);
                } else if (expectedSampleRate > 0 &&
                           inspected.info.sample_rate !=
                               static_cast<std::uint32_t>(expectedSampleRate)) {
                    *assetFailure = QStringLiteral(
                        "WAV asset %1 has sample rate %2 Hz; expected %3 Hz")
                        .arg(hash.left(12))
                        .arg(inspected.info.sample_rate)
                        .arg(expectedSampleRate);
                    return;
                }
            }
        },
        [this, song, revision, restartTrack, hostAuthoritative, fromPeerProposal,
         sourcePeerToken, checkRevision, missing, assetFailure] {
            if (checkRevision != songAssetCheckRevision_) {
                return;
            }
            if (!assetFailure->isEmpty()) {
                appendLog(QStringLiteral("rejected arrangement asset: ") + *assetFailure);
                return;
            }
            if (!missing->isEmpty()) {
                pendingSongSet_ = song;
                pendingSongRevision_ = revision;
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
                QJsonArray hashes;
                for (const QString& hash : *missing) {

                    hashes.append(hash);
                }
                const QJsonObject request{
                    {QStringLiteral("type"), QStringLiteral("looper.asset.request")},
                    {QStringLiteral("arrangement_revision"), revision},
                    {QStringLiteral("hashes"), hashes},
                };
                if (fromPeerProposal) {
                    (void)sendControlTo(sourcePeerToken, request);
                } else {
                    sendControl(request);
                }
                return;
            }
            projectPersistence_.useWorkspaceAsProjectFolderIfUnset();
            pendingSongSet_ = QJsonObject{};
            pendingSongRevision_ = 0;
            pendingSongTrackRestart_ = false;
            pendingSongSourcePeerToken_.clear();
            pendingSongNeedsAuthoritativePublish_ = false;
            pendingLooperAssetHashes_.clear();
            if (loadSongJson(song)) {
                if (hostAuthoritative) {
                    lastAppliedHostArrangementRevision_ = revision;
                }
                refreshSongViews();
                refreshLooperLanes();
                pendingPreparedTrackReadyRevision_ = !fromPeerProposal &&
                    restartTrack && jam2_.isRunning() ? revision : 0;
                trackController_.prepareMix(
                    static_cast<quint64>(revision), restartTrack);
                updateTrackPlaybackPresentation();
                regeneratePreparedMix();
                if (fromPeerProposal) {
                    appendLog(QStringLiteral("accepted collaborative arrangement edit from peer %1")
                        .arg(sourcePeerToken.left(8)));
                    sendSongSnapshot(restartTrack);
                } else if (pendingPreparedTrackReadyRevision_ > 0) {
                    appendLog(QStringLiteral("preparing synchronized track revision %1 before requesting an authority target")
                        .arg(revision));
                }
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

void MainWindow::handleTrackReady(
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    if (!sessionController_.isServer() || sourcePeerToken.isEmpty()) {
        appendLog(QStringLiteral("rejected track readiness outside the arrangement authority"));
        return;
    }
    const quint64 revision = static_cast<quint64>(
        message.value(QStringLiteral("arrangement_revision")).toInteger());
    if (revision == 0 || revision != pendingSharedTrackRevision_) {
        appendLog(QStringLiteral("ignored stale track readiness revision %1 from %2")
            .arg(revision)
            .arg(sourcePeerToken.left(8)));
        return;
    }
    if (!meshPeerEndpoints_.contains(sourcePeerToken) || sourcePeerToken == meshPeerToken_) {
        appendLog(QStringLiteral("rejected track readiness from non-member %1")
            .arg(sourcePeerToken.left(8)));
        return;
    }
    pendingSharedTrackReadyTokens_.insert(sourcePeerToken);
    appendLog(QStringLiteral("track revision %1 ready on peer %2")
        .arg(revision)
        .arg(sourcePeerToken.left(8)));
    maybeScheduleSharedTrackRestart();
}

void MainWindow::maybeScheduleSharedTrackRestart()
{
    if (!sessionController_.isServer() || pendingSharedTrackRevision_ == 0) {
        return;
    }
    QSet<QString> expected;
    for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it) {
        if (it.key() != meshPeerToken_) {
            expected.insert(it.key());
        }
    }
    if (expected.isEmpty()) {
        pendingSharedTrackRevision_ = 0;
        pendingSharedTrackHostReady_ = true;
        pendingSharedTrackReadyTokens_.clear();
        return;
    }
    if (!pendingSharedTrackHostReady_) {
        return;
    }
    for (const QString& token : expected) {
        if (!pendingSharedTrackReadyTokens_.contains(token)) {
            return;
        }
    }
    const quint64 revision = pendingSharedTrackRevision_;
    pendingSharedTrackRevision_ = 0;
    pendingSharedTrackHostReady_ = true;
    pendingSharedTrackReadyTokens_.clear();
    restartPreparedTrackQuantized();
    appendLog(QStringLiteral("arrangement authority scheduled shared track revision %1 after %2 peer readiness acknowledgements")
        .arg(revision)
        .arg(expected.size()));
}

void MainWindow::applyPendingSongIfAssetsReady()
{
    if (pendingSongSet_.isEmpty()) {
        return;
    }
    if (!pendingLooperAssetHashes_.isEmpty()) {
        return;
    }
    const QJsonObject song = pendingSongSet_;
    const int revision = pendingSongRevision_;
    const bool restartTrack = pendingSongTrackRestart_;
    const QString sourcePeerToken = pendingSongSourcePeerToken_;
    const bool publishAuthoritative = pendingSongNeedsAuthoritativePublish_;
    pendingSongSet_ = QJsonObject{};
    pendingSongRevision_ = 0;
    pendingSongTrackRestart_ = false;
    pendingSongSourcePeerToken_.clear();
    pendingSongNeedsAuthoritativePublish_ = false;
    projectPersistence_.useWorkspaceAsProjectFolderIfUnset();
    if (loadSongJson(song)) {
        if (!publishAuthoritative) {
            lastAppliedHostArrangementRevision_ = qMax(
                lastAppliedHostArrangementRevision_, revision);
        }
        refreshSongViews();
        refreshLooperLanes();
        pendingPreparedTrackReadyRevision_ = !publishAuthoritative &&
            restartTrack && jam2_.isRunning() ? revision : 0;
        trackController_.prepareMix(static_cast<quint64>(revision), restartTrack);
        updateTrackPlaybackPresentation();
        regeneratePreparedMix();
        if (publishAuthoritative) {
            appendLog(QStringLiteral("accepted collaborative arrangement edit from peer %1 after asset sync")
                .arg(sourcePeerToken.left(8)));
            sendSongSnapshot(restartTrack);
        } else if (pendingPreparedTrackReadyRevision_ > 0) {
            appendLog(QStringLiteral("preparing received track revision %1 before requesting an authority target")
                .arg(revision));
        }
        appendLog(QStringLiteral("applied pending looper arrangement after asset sync"));
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
                        QStringLiteral("wavs/") + hash + QStringLiteral(".wav")));
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

QJsonObject MainWindow::preserveQuarantinedLocalLanes(QJsonObject song)
{
    const int expectedSampleRate = sessionController_.snapshot().contract.sampleRate;
    const int preserved = mergeQuarantinedLocalLanes(
        song, looperProject_, expectedSampleRate);
    if (preserved > 0) {
        appendLog(QStringLiteral(
            "preserved %1 incompatible local WAV lane(s) as quarantined during arrangement sync")
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
        QStringLiteral("wavs/") + hash + QStringLiteral(".wav"));
}

Jam2RuntimeOptions MainWindow::runtimeOptions() const
{
    Jam2RuntimeOptions options;
    options.bind = jam2::parse_bind_endpoint(meshBindEndpoint().toStdString());
    if (stunServerEdit_ && !stunServerEdit_->text().trimmed().isEmpty()) {
        options.stun_server = jam2::parse_endpoint(stunServerEdit_->text().trimmed().toStdString());

    }
    options.no_stun = noStunCheck_ && noStunCheck_->isChecked();

    options.stun_timeout_ms = stunTimeoutSpin_ ? stunTimeoutSpin_->value() : 1000;
    options.stun_retries = stunRetriesSpin_ ? stunRetriesSpin_->value() : 3;
    options.wait_ms = waitMsSpin_ ? waitMsSpin_->value() : 0;
    options.stream_ms = streamMsSpin_ ? streamMsSpin_->value() : 0;
    options.stream_linger_ms = streamLingerMsSpin_ ? streamLingerMsSpin_->value() : 100;
    options.stats_enabled = statsCheck_ && statsCheck_->isChecked();
    options.stats_interval_ms = options.stats_enabled ? 1000 : 0;
    options.stats_warmup_ms = statsWarmupMsSpin_ ? statsWarmupMsSpin_->value() : 3000;
    if (options.stats_enabled && logStatsEdit_ && !logStatsEdit_->text().trimmed().isEmpty()) {
        options.log_stats_dir = nativeFilePath(logStatsEdit_->text().trimmed());
    }
    if (socketSendBufferSpin_ && socketSendBufferSpin_->value() > 0) {
        options.socket_send_buffer = socketSendBufferSpin_->value();
    }
    if (socketRecvBufferSpin_ && socketRecvBufferSpin_->value() > 0) {
        options.socket_recv_buffer = socketRecvBufferSpin_->value();
    }
    options.sample_rate = sampleRateSpin_->value();
    options.frame_size = frameSizeSpin_->value();
    if (networkAudioFormatBox_) {
        const auto format = jam2::protocol::parse_audio_format(
            networkAudioFormatBox_->currentData().toString().toStdString());
        if (!format) {
            throw std::runtime_error("select a supported network audio quality");
        }
        options.network_audio_format = *format;
    }
    options.drift_correction = driftCorrectionCheck_->isChecked();
    options.drift_smoothing = driftSmoothingSpin_->value();
    options.drift_deadband_ppm = driftDeadbandSpin_->value();
    options.drift_max_correction_ppm = driftMaxCorrectionSpin_->value();
    options.metronome = false;
    options.bpm = metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value();
    options.metronome_level = gainFromDb(static_cast<double>(metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10));
    const QString metronomeMode = metronomeModeBox_->currentText();
    options.metronome_mode = metronomeMode == QStringLiteral("leader-audio")
        ? Jam2MetronomeMode::LeaderAudio
        : metronomeMode == QStringLiteral("listener-compensated")
            ? Jam2MetronomeMode::ListenerCompensated
            : Jam2MetronomeMode::SharedGrid;
    options.metronome_compensation_max_ms = metronomeCompensationMaxSpin_ ? metronomeCompensationMaxSpin_->value() : 250.0;
    options.metronome_compensation_smoothing_ms = metronomeCompensationSmoothingSpin_ ? metronomeCompensationSmoothingSpin_->value() : 750.0;
    options.metronome_compensation_deadband_ms = metronomeCompensationDeadbandSpin_ ? metronomeCompensationDeadbandSpin_->value() : 1.0;
    options.metronome_compensation_slew_ms_per_sec = metronomeCompensationSlewSpin_ ? metronomeCompensationSlewSpin_->value() : 40.0;
    options.remote_level = gainFromDb(static_cast<double>(remoteLevelSlider_ ? remoteLevelSlider_->value() : 0));
    options.send_level = gainFromDb(static_cast<double>(mixSendLevelSlider_ ? mixSendLevelSlider_->value() : 0));
    options.local_monitor = mixMonitorCheck_ && mixMonitorCheck_->isChecked();
    options.local_monitor_level = gainFromDb(static_cast<double>(mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : -18));
    options.sample_time_playout = sampleTimePlayoutCheck_->isChecked();
    options.playout_delay_frames = static_cast<std::size_t>(playoutDelaySpin_->value());
    options.jitter_buffer_frames = static_cast<std::size_t>(jitterBufferSpin_->value());
    options.jitter_buffer_max_frames = static_cast<std::size_t>(jitterBufferMaxSpin_->value());
    options.adaptive_playback_cushion = adaptiveCushionCheck_->isChecked();
    options.adaptive_playback_target_frames = static_cast<std::size_t>(adaptiveTargetSpin_->value());
    options.adaptive_playback_min_frames = static_cast<std::size_t>(adaptiveMinSpin_->value());
    options.adaptive_playback_max_frames = static_cast<std::size_t>(adaptiveMaxSpin_->value());
    options.adaptive_playback_release_ppm = adaptiveReleaseSpin_->value();
    options.adaptive_playback_ratio_ramp_ms = adaptiveRatioRampSpin_->value();
    bool deviceOk = false;
    const int device = selectedDeviceId().toInt(&deviceOk);
    if (!deviceOk || device < 0) {
        throw std::runtime_error("select a valid low-latency audio device");
    }
    options.audio_device_id = device;
    options.profile_name = (profileBox_ ? profileBox_->currentData().toString() : QStringLiteral("fast")).toStdString();
    options.audio_buffer_size = bufferSizeSpin_->value();
    options.input_channels = jam2::audio::InputChannels::Mono;
    options.channel_selection.input = parseUiChannels(inputChannelsEdit_->text(), "input");
    options.channel_selection.output = parseUiChannels(outputChannelsEdit_->text(), "output");
    options.capture_ring_frames = static_cast<std::size_t>(captureRingSpin_->value());
    options.playback_ring_frames = static_cast<std::size_t>(playbackRingSpin_->value());
    options.playback_prefill_frames = static_cast<std::size_t>(prefillSpin_->value());
    options.playback_max_frames = static_cast<std::size_t>(playbackMaxSpin_->value());
    const QString priority = osPriorityBox_ ? osPriorityBox_->currentData().toString() : QStringLiteral("high");
    options.os_priority = priority == QStringLiteral("realtime")
        ? Jam2OsPriorityMode::Realtime
        : priority == QStringLiteral("off") ? Jam2OsPriorityMode::Off : Jam2OsPriorityMode::High;
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
    const QString data = deviceBox_->currentData().toString();
    return data.isEmpty() ? deviceId(deviceBox_->currentText()) : data;
}

QJsonObject MainWindow::trackToJson() const
{
    const auto& model = trackController_.model();
    return QJsonObject{
        {QStringLiteral("file_name"), model.fileName},
        {QStringLiteral("file_path"), model.filePath},
        {QStringLiteral("file_bytes"), model.fileBytes},
        {QStringLiteral("sample_rate"), model.sampleRate},
        {QStringLiteral("duration_ms"), model.durationMs},
        {QStringLiteral("sha256"), model.sha256},
        {QStringLiteral("guessed_bpm"), model.guessedBpm},
        {QStringLiteral("accepted_bpm"), model.acceptedBpm},
        {QStringLiteral("key"), model.key},
        {QStringLiteral("speed"), model.speed},

        {QStringLiteral("pitch_cents"), model.pitchCents},
        {QStringLiteral("track_gain_db"), model.trackGainDb},
        {QStringLiteral("loop_enabled"), model.loopEnabled},
        {QStringLiteral("loop_start_seconds"), model.loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), model.loopEndSeconds},
        {QStringLiteral("focus_enabled"), model.focusEnabled},
        {QStringLiteral("focus_preset"), model.focusPreset},
        {QStringLiteral("focus_frequency_hz"), model.focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), model.focusGainDb},
        {QStringLiteral("focus_q"), model.focusQ},
        {QStringLiteral("highpass_hz"), model.highpassHz},
        {QStringLiteral("lowpass_hz"), model.lowpassHz},
    };
}

void MainWindow::loadTrackJson(const QJsonObject& object)
{
    if (object.isEmpty()) {
        return;
    }
    auto& model = trackController_.model();
    model.fileName = object.value(QStringLiteral("file_name")).toString(model.fileName);
    model.filePath = object.value(QStringLiteral("file_path")).toString(model.filePath);
    model.fileBytes = static_cast<qint64>(object.value(QStringLiteral("file_bytes")).toDouble(model.fileBytes));
    model.sampleRate = object.value(QStringLiteral("sample_rate")).toInt(model.sampleRate);
    model.durationMs = object.value(QStringLiteral("duration_ms")).toInt(model.durationMs);
    model.sha256 = object.value(QStringLiteral("sha256")).toString(model.sha256);
    model.guessedBpm = object.value(QStringLiteral("guessed_bpm")).toDouble(model.guessedBpm);
    model.acceptedBpm = object.value(QStringLiteral("accepted_bpm")).toDouble(model.acceptedBpm);
    model.key = object.value(QStringLiteral("key")).toString(model.key);
    model.speed = object.value(QStringLiteral("speed")).toDouble(model.speed);
    model.pitchCents = object.value(QStringLiteral("pitch_cents")).toInt(model.pitchCents);
    model.trackGainDb = object.value(QStringLiteral("track_gain_db")).toDouble(model.trackGainDb);
    model.loopEnabled = object.value(QStringLiteral("loop_enabled")).toBool(model.loopEnabled);
    model.loopStartSeconds = object.value(QStringLiteral("loop_start_seconds")).toDouble(model.loopStartSeconds);
    model.loopEndSeconds = object.value(QStringLiteral("loop_end_seconds")).toDouble(model.loopEndSeconds);
    model.syncControls = looperProject_.trackSyncEnabled();
    model.focusEnabled = object.value(QStringLiteral("focus_enabled")).toBool(model.focusEnabled);
    model.focusPreset = object.value(QStringLiteral("focus_preset")).toString(model.focusPreset);
    model.focusFrequencyHz = object.value(QStringLiteral("focus_frequency_hz")).toDouble(model.focusFrequencyHz);
    model.focusGainDb = object.value(QStringLiteral("focus_gain_db")).toDouble(model.focusGainDb);
    model.focusQ = object.value(QStringLiteral("focus_q")).toDouble(model.focusQ);
    model.highpassHz = object.value(QStringLiteral("highpass_hz")).toDouble(model.highpassHz);
    model.lowpassHz = object.value(QStringLiteral("lowpass_hz")).toDouble(model.lowpassHz);
    updateTrackControls();
    loadTrackWaveform();
}

QJsonObject MainWindow::songToJson(bool syncCompatibleOnly) const
{
    QJsonObject root = chordModel_.toJson();
    root.insert(QStringLiteral("independent_views"), true);
    root.insert(QStringLiteral("beat_view"), beatModel_.toJson());
    root.insert(QStringLiteral("lyric_view"), lyricModel_.toJson());
    root.insert(QStringLiteral("looper"), looperProject_.toJson(syncCompatibleOnly));
    return root;
}

bool MainWindow::loadSongJson(const QJsonObject& object)
{
    BeatGridModel loadedChord;
    BeatGridModel loadedBeat;
    BeatGridModel loadedLyric;
    LooperProject loadedLooper = looperProject_;
    if (!loadedChord.loadJson(object)) {
        return false;
    }
    const QJsonObject beatObject = object.value(QStringLiteral("beat_view")).toObject();
    if (!beatObject.isEmpty()) {
        if (!loadedBeat.loadJson(beatObject)) {
            return false;
        }
    } else if (!loadedBeat.loadJson(object)) {
        return false;
    }
    const QJsonObject lyricObject = object.value(QStringLiteral("lyric_view")).toObject();
    if (!lyricObject.isEmpty()) {
        if (!loadedLyric.loadJson(lyricObject)) {
            return false;
        }
    } else if (!loadedLyric.loadJson(object)) {
        return false;
    }

    const QJsonObject looperObject = object.value(QStringLiteral("looper")).toObject();
    if (!looperObject.isEmpty() && !loadedLooper.loadJson(looperObject)) {
        return false;
    }
    const QString title = loadedChord.title();
    loadedBeat.setTitle(title);
    loadedLyric.setTitle(title);
    chordModel_ = loadedChord;
    beatModel_ = loadedBeat;
    lyricModel_ = loadedLyric;
    const int expectedSampleRate = sessionController_.snapshot().contract.sampleRate > 0
        ? sessionController_.snapshot().contract.sampleRate
        : (sampleRateSpin_ ? sampleRateSpin_->value() : 48000);
    bool needsCompatibilityAudit = false;
    for (LooperBank& bank : loadedLooper.banks()) {
        for (LooperLane& lane : bank.lanes) {
            if (lane.assetPath.trimmed().isEmpty()) {
                lane.sampleRateCompatible = true;
                continue;
            }
            lane.sampleRateCompatible = lane.sampleRate > 0 &&
                lane.sampleRate == expectedSampleRate;
            needsCompatibilityAudit = needsCompatibilityAudit ||
                !lane.sampleRateCompatible;
        }
    }
    looperProject_ = std::move(loadedLooper);
    refreshLooperLanes();
    if (needsCompatibilityAudit) {
        QTimer::singleShot(0, this, [this, expectedSampleRate] {
            auditWavCompatibilityForSession(expectedSampleRate, true);
        });
    }
    return true;
}

void MainWindow::updatePlaybackGrid()
{
    const bool jamGrid = jam2_.isRunning();
    if (!jamGrid) {
        const auto pattern = currentMetronomePattern();
        metronomeTransport_.grid().setPattern(pattern.bpm, pattern.beats_per_bar, pattern.division);
        metronomeTransport_.grid().clearEngine();
    }
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    const bool showMarkerReference = metronomeMarkerReferenceCheck_ == nullptr ||
        metronomeMarkerReferenceCheck_->isChecked();
    const bool markerRunning = position.engineAnchored && showMarkerReference;
    updateRecordingCountdown(position);
    if (chordGrid_) {
        chordGrid_->setGridPosition(position.absoluteBeat, position.subdivision, markerRunning);
    }
    if (beatGrid_) {
        beatGrid_->setGridPosition(position.absoluteBeat, position.subdivision, markerRunning);
    }
    if (lyricGrid_) {
        lyricGrid_->setGridPosition(position.absoluteBeat, position.subdivision, markerRunning);
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
        const qint64 gridPositionMs = static_cast<qint64>(std::llround(
            qMax(0.0, position.secondsFromEpoch) * 1000.0));
        trackWaveform_->setGridPosition(
            gridPositionMs,
            markerRunning,
            currentMetronomePattern().beats_per_bar);
    }
    if (looperStack_) {

        const qint64 gridPositionMs = static_cast<qint64>(std::llround(
            qMax(0.0, position.secondsFromEpoch) * 1000.0));
        looperStack_->setGridPosition(
            gridPositionMs,
            markerRunning,
            metronomeTransport_.grid().bpm(),
            currentMetronomePattern().beats_per_bar);
    }
}

void MainWindow::updateRecordingCountdown(const PlaybackGrid::Position& position)
{
    if (!recordingCountdownLabel_) {
        return;
    }
    const TrackRecordingWorkflow::CountdownPresentation countdown =
        trackRecordingWorkflow_.countdown(position);
    if (countdown.phase == TrackRecordingWorkflow::CountdownPhase::Hidden) {
        recordingCountdownLabel_->hide();
        return;
    }
    if (countdown.phase == TrackRecordingWorkflow::CountdownPhase::WaitingForBar) {
        recordingCountdownLabel_->setText(QStringLiteral("WAITING FOR NEXT BAR..."));
        recordingCountdownLabel_->show();
        return;
    }
    if (countdown.phase == TrackRecordingWorkflow::CountdownPhase::Counting) {
        recordingCountdownLabel_->setText(QString::number(countdown.remainingBeats));
        recordingCountdownLabel_->show();
        return;
    }
    recordingCountdownLabel_->setText(QStringLiteral("RECORDING"));
    recordingCountdownLabel_->show();
    if (countdown.stopMetronome) {
        stopTrackMetronome();
    }
}

void MainWindow::updateRecordingLatencyDisplay()
{
    if (!recordingLatencyLabel_) {
        return;
    }
    const int sampleRate = qMax(1, trackRecordingWorkflow_.latencySampleRate());
    const qint64 adjustment = recordingLatencyAdjustmentSpin_
        ? recordingLatencyAdjustmentSpin_->value()
        : 0;
    const auto milliseconds = [sampleRate](quint64 frames) {
        return static_cast<double>(frames) * 1000.0 / static_cast<double>(sampleRate);
    };
    recordingLatencyLabel_->setText(QStringLiteral(
        "Input %1 (%2 ms) | Output %3 (%4 ms) | Manual %5 | Applied %6 (%7 ms)")
        .arg(trackRecordingWorkflow_.inputLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.inputLatencyFrames()), 0, 'f', 2)
        .arg(trackRecordingWorkflow_.outputLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.outputLatencyFrames()), 0, 'f', 2)
        .arg(adjustment >= 0 ? QStringLiteral("+%1").arg(adjustment) : QString::number(adjustment))
        .arg(trackRecordingWorkflow_.appliedLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.appliedLatencyFrames()), 0, 'f', 2));
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
    chordModel_.reset();
    beatModel_.reset();
    lyricModel_.reset();
    stopTrackMetronome();
    trackController_ = SharedTrackController{};
    looperProject_ = LooperProject{};
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
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open Jam2 Song"),

        appReleaseFolderPath(QStringLiteral("songs")),
        QStringLiteral("Jam2 song (*.jam2song *.json);;JSON files (*.json);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) {
        return;
    }
    auto root = std::make_shared<QJsonObject>();
    auto error = std::make_shared<QString>();
    (void)startFileWorkerTask(
        [path, root, error] {
            (void)ProjectPersistenceCoordinator::readSongJson(path, *root, *error);
        },
        [this, path, root, error] {
            if (!error->isEmpty() || !loadSongJson(*root)) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Jam2"),
                    error->isEmpty() ? QStringLiteral("Invalid Jam2 song file.") : *error);
                return;
            }
            projectPersistence_.setProjectLocation(path);
            loadTrackJson(root->value(QStringLiteral("track")).toObject());
            refreshSongViews();
            projectPersistence_.acceptOpenedProject(path, currentProjectSnapshot());
        });
}

bool MainWindow::saveSong()
{
    chordModel_.setTitle(songTitleEdit_->text());
    beatModel_.setTitle(songTitleEdit_->text());
    lyricModel_.setTitle(songTitleEdit_->text());
    QString path = projectPersistence_.projectFilePath();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Save Jam2 Song"),
            appReleaseFilePath(QStringLiteral("songs"), chordModel_.title() + QStringLiteral(".jam2song")),
            QStringLiteral("Jam2 song (*.jam2song);;JSON files (*.json);;All files (*)"),
            nullptr,
            QFileDialog::DontUseNativeDialog);
    }
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo songInfo(path);
    if (!materializeLooperAssets(songInfo.absolutePath())) {
        return false;
    }
    QJsonObject root = songToJson();
    root.insert(QStringLiteral("track"), trackToJson());
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
        QStringLiteral("Writing Jam2 song..."),
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
    return projectPersistence_.hasUnsavedChanges(currentProjectSnapshot());
}

void MainWindow::registerTransientTrackWav(const QString& path)
{
    projectPersistence_.registerTransientWav(path);
}

void MainWindow::cleanupTransientTrackWavs()
{
    if (!trackRecordingWorkflow_.lastCapturePath().isEmpty() &&
        !QFileInfo::exists(trackRecordingWorkflow_.lastCapturePath())) {
        trackRecordingWorkflow_.clearLastCapturePath();
    }
    projectPersistence_.scheduleTransientCleanup(fileWorkerPool_);
}


void MainWindow::sendSongSnapshot(std::optional<bool> trackPlayingOverride)
{
    if (!looperProject_.trackSyncEnabled()) {
        appendLog(QStringLiteral("suppressed local track sync while sync is disabled"));
        return;
    }
    const SharedSessionController::Snapshot before = sessionController_.snapshot();
    if (before.role != SharedSessionController::Role::Creator &&
        before.role != SharedSessionController::Role::Joiner) {
        return;
    }
    const int revision = ++looperArrangementRevision_;
    if (!sessionController_.isServer()) {
        (void)sessionController_.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("song.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("arrangement_revision"), 0},
            {QStringLiteral("host_authoritative"), false},
            {QStringLiteral("track_playing"), trackPlayingOverride.value_or(
                trackRecordingWorkflow_.preparedPlaying())},
            {QStringLiteral("song"), songToJson(true)},
        });
        appendLog(QStringLiteral("submitted collaborative arrangement edit"));
        return;
    }
    const bool trackPlaying = trackPlayingOverride.value_or(
        trackRecordingWorkflow_.preparedPlaying());
    const bool coordinateTrackRestart = sessionController_.isServer() &&
        before.remotePeerCount > 0 && trackPlaying;
    const quint64 expectedArrangementRevision = before.arrangementRevision + 1;
    if (coordinateTrackRestart) {
        pendingSharedTrackRevision_ = expectedArrangementRevision;
        pendingSharedTrackHostReady_ = !preparedMixWorkerRunning_;
        pendingSharedTrackReadyTokens_.clear();
        appendLog(QStringLiteral(
            "arrangement authority waiting for track revision %1 readiness host_ready=%2 remote_peers=%3")
            .arg(expectedArrangementRevision)
            .arg(pendingSharedTrackHostReady_ ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(before.remotePeerCount));
    } else {

        pendingSharedTrackRevision_ = 0;
        pendingSharedTrackHostReady_ = true;
        pendingSharedTrackReadyTokens_.clear();
    }

    (void)sessionController_.send(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("arrangement_revision"), revision},
        {QStringLiteral("host_authoritative"), true},
        {QStringLiteral("track_playing"), trackPlaying},
        {QStringLiteral("song"), songToJson(true)},
    });
    const SharedSessionController::Snapshot after = sessionController_.snapshot();
    if (coordinateTrackRestart && after.arrangementRevision != expectedArrangementRevision) {
        pendingSharedTrackRevision_ = 0;
        pendingSharedTrackHostReady_ = true;
        pendingSharedTrackReadyTokens_.clear();
        appendLog(QStringLiteral("could not publish shared track arrangement revision"));
        return;
    }
    trackController_.requestPlayback(trackPlaying, after.arrangementRevision);
    updateTrackPlaybackPresentation();
    maybeScheduleSharedTrackRestart();
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
    if (lane == QStringLiteral("chord")) {
        if (chordGrid_) {
            chordGrid_->refresh();
        }
    } else if (lane == QStringLiteral("lyric")) {
        if (lyricGrid_) {
            lyricGrid_->refresh();
        }
    } else {
        if (beatGrid_) {
            beatGrid_->refresh();
        }
    }
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
