#include "MainWindow.hpp"
#include "MainWindowPages.hpp"
#include "TrackWidgets.hpp"
#include "TrackWorkspaceSupport.hpp"
#include "GuiPresentation.hpp"
#include "JamTasterDialog.hpp"
#include "../jamtaster/JamTasterService.hpp"
#include "GuiControlMessageRouter.hpp"
#include "CuratedIdeaCatalog.hpp"
#include "CuratedIdeaDialog.hpp"
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
#include <QStandardPaths>
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
constexpr int kFirewallGuidanceDisconnectThreshold = 3;
constexpr int kFirewallGuidanceWindowMs = 10000;

QString promptJamTasterSourceDisposition(QWidget* parent)
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

bool appendJamTasterReferenceSection(
    QJsonObject& song,
    QJsonObject lane,
    QString& error)
{
    QJsonArray sections = song.value(QStringLiteral("sections")).toArray();
    QJsonObject looper = song.value(QStringLiteral("looper")).toObject();
    QJsonArray banks = looper.value(QStringLiteral("banks")).toArray();
    if (sections.isEmpty() || banks.isEmpty() ||
        sections.size() >= jam2::application::limits::kMaximumSongSections) {
        error = QStringLiteral(
            "There is no free section for the original reference WAV. Remove a section "
            "or choose to delete the source instead.");
        return false;
    }
    const QJsonObject firstBank = banks.first().toObject();
    QJsonObject timing = firstBank.value(QStringLiteral("timing")).toObject();
    const int bpm = qBound(20, timing.value(QStringLiteral("bpm")).toInt(120), 400);
    const int meter = qBound(
        2, timing.value(QStringLiteral("beats_per_bar")).toInt(4), 12);
    const qint64 frames = lane.value(QStringLiteral("source_frames"))
        .toVariant().toLongLong();
    const int sampleRate = lane.value(QStringLiteral("sample_rate")).toInt(48000);
    const double duration = sampleRate > 0 ? frames / static_cast<double>(sampleRate) : 0.0;
    const int rawBeats = qMax(
        jam2::application::limits::kMinimumBeatsPerSection,
        static_cast<int>(std::ceil(duration * bpm / 60.0)));
    const int beats = ((rawBeats + meter - 1) / meter) * meter;
    if (beats > 512) {
        error = QStringLiteral(
            "The original recording is longer than Jam2's 512-beat section limit.");
        return false;
    }

    QJsonArray strings;
    QJsonArray beatPatterns;
    QJsonArray musicalPatterns;
    QJsonArray emptyDrumLanes;
    for (int drumLane = 0; drumLane < 10; ++drumLane) emptyDrumLanes.append(QString());
    const auto restSteps = [] {
        QJsonArray result;
        for (int index = 0; index < 4; ++index) {
            result.append(QJsonObject{
                {QStringLiteral("state"), QStringLiteral("rest")},
                {QStringLiteral("value"), QString()},
                {QStringLiteral("velocity"), 96},
            });
        }
        return result;
    };
    for (int beat = 0; beat < beats; ++beat) {
        strings.append(QString());
        beatPatterns.append(QJsonObject{
            {QStringLiteral("division"), 4},
            {QStringLiteral("lanes"), emptyDrumLanes},
        });
        musicalPatterns.append(QJsonObject{
            {QStringLiteral("division"), 4},
            {QStringLiteral("chords"), restSteps()},
            {QStringLiteral("bass"), restSteps()},
            {QStringLiteral("melody"), restSteps()},
            {QStringLiteral("support"), restSteps()},
        });
    }
    const int index = sections.size();
    const QString label(QChar(QLatin1Char('A').unicode() + index));
    QJsonObject section{
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("label"), label},
        {QStringLiteral("name"), QStringLiteral("Original Reference")},
        {QStringLiteral("beats"), beats},
        {QStringLiteral("targets"), strings},
        {QStringLiteral("beat_notes"), strings},
        {QStringLiteral("lyrics"), strings},
        {QStringLiteral("chords"), strings},
        {QStringLiteral("beat_patterns"), beatPatterns},
        {QStringLiteral("musical_patterns"), musicalPatterns},
        {QStringLiteral("drum_kit"), QStringLiteral("acoustic")},
        {QStringLiteral("generated_kind"), QString()},
    };
    sections.append(section);

    lane.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    lane.insert(QStringLiteral("name"), QStringLiteral("Original Source (muted)"));
    lane.insert(QStringLiteral("muted"), true);
    lane.insert(QStringLiteral("solo"), false);
    lane.insert(QStringLiteral("loop_enabled"), false);
    lane.insert(QStringLiteral("local_only"), false);
    lane.insert(QStringLiteral("origin_kind"), QStringLiteral("imported"));
    timing.insert(QStringLiteral("bpm"), bpm);
    timing.insert(QStringLiteral("beats_per_bar"), meter);
    timing.insert(QStringLiteral("inherits_bank_a"), true);
    QJsonObject bank{
        {QStringLiteral("id"), label},
        {QStringLiteral("lanes"), QJsonArray{lane}},
        {QStringLiteral("timing"), timing},
    };
    banks.append(bank);
    looper.insert(QStringLiteral("banks"), banks);
    song.insert(QStringLiteral("sections"), sections);
    song.insert(QStringLiteral("looper"), looper);
    return true;
}

bool explicitValueEditorHasFocus(QWidget* focus)
{
    for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
        if (qobject_cast<QLineEdit*>(widget) ||
            qobject_cast<QPlainTextEdit*>(widget) ||
            qobject_cast<QTextEdit*>(widget) ||
            qobject_cast<QAbstractSpinBox*>(widget) ||
            qobject_cast<QComboBox*>(widget)) {
            return true;
        }
    }
    return false;
}

bool blocksIncidentalNavigationKey(QWidget* focus)
{
    for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
        if (qobject_cast<QAbstractSlider*>(widget) ||
            qobject_cast<QAbstractButton*>(widget) ||
            qobject_cast<QAbstractItemView*>(widget) ||
            qobject_cast<QAbstractScrollArea*>(widget) ||
            qobject_cast<QTabBar*>(widget)) {
            return true;
        }
    }
    return false;
}

void addInitialEmptyLooperLanes(LooperProject& project)
{
    for (int bankIndex = 0; bankIndex < project.banks().size(); ++bankIndex) {
        if (project.banks().at(bankIndex).lanes.isEmpty()) {
            (void)project.appendLane(bankIndex, LooperLane{});
        }
    }
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

QString portableFileStem(QString value, const QString& fallback)
{
    value = value.trimmed();
    value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral(R"([. ]+$)")), QString{});
    if (value.isEmpty()) {
        value = fallback;
    }
    static const QRegularExpression windowsDeviceName(QStringLiteral(
        R"(^(con|prn|aux|nul|com[1-9]|lpt[1-9])$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (windowsDeviceName.match(value).hasMatch()) {
        value.prepend(QLatin1Char('_'));
    }
    return value.left(120);
}

QComboBox* fixedSampleRateCombo(QWidget* parent, int value)
{
    auto* combo = new QComboBox(parent);
    for (const int rate : jam2::audio::kTestSampleRates) {
        combo->addItem(QString::number(rate), rate);
    }
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : combo->findData(48000));
    return combo;
}

QComboBox* fixedBufferSizeCombo(QWidget* parent, int value)
{
    auto* combo = new QComboBox(parent);
    for (const long size : jam2::audio::kTestBufferSizes) {
        combo->addItem(QString::number(size), static_cast<int>(size));
    }
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : combo->findData(64));
    return combo;
}

QString devicePreferenceKey(const jam2::audio::DeviceInfo& device)
{
    return QString::fromStdString(device.backend) + QLatin1Char('|') +
        QString::fromStdString(device.clsid.empty() ? device.name : device.clsid);
}

int preferredDeviceIndex(
    const QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices,
    const AudioDevicePreference& preference)
{
    if (combo == nullptr) return -1;
    for (const auto& device : devices) {
        const QString stable = QString::fromStdString(
            device.clsid.empty() ? device.name : device.clsid);
        if (QString::fromStdString(device.backend) == preference.backend &&
            stable == preference.stableId) {
            return combo->findData(QString::number(device.id));
        }
    }
    return -1;
}

void selectPreferredDevice(
    QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices,
    const AudioDevicePreference& preference)
{
    const int index = preferredDeviceIndex(combo, devices, preference);
    if (index >= 0) combo->setCurrentIndex(index);
}

void storeSelectedDevice(
    AudioDevicePreference& preference,
    const QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices)
{
    if (combo == nullptr) return;
    bool ok = false;
    const int id = combo->currentData().toInt(&ok);
    if (!ok) return;
    const auto selected = std::find_if(devices.begin(), devices.end(),
        [id](const auto& item) { return item.id == id; });
    if (selected == devices.end()) return;
    preference.backend = QString::fromStdString(selected->backend);
    preference.stableId = QString::fromStdString(
        selected->clsid.empty() ? selected->name : selected->clsid);
    preference.name = QString::fromStdString(selected->name);
}

QString deviceCapabilitiesText(const jam2::audio::DeviceTestResult& capabilities)
{
    QStringList lines{
        QStringLiteral("Device: %1 %2")
            .arg(QString::fromStdString(capabilities.device.backend),
                 QString::fromStdString(capabilities.device.name)),
        QStringLiteral("Current device sample rate: %1 Hz")
            .arg(capabilities.current_sample_rate, 0, 'f', 0),
        QStringLiteral(""),
        QStringLiteral("Sample rates:"),
    };
    for (std::size_t index = 0; index < jam2::audio::kTestSampleRates.size(); ++index) {
        lines.append(QStringLiteral("  %1 Hz: %2")
            .arg(jam2::audio::kTestSampleRates[index])
            .arg(capabilities.sample_rate_supported[index]
                ? QStringLiteral("supported") : QStringLiteral("not supported")));
    }
    lines.append(QStringLiteral(""));
    lines.append(QStringLiteral("Buffer sizes:"));
    for (std::size_t index = 0; index < jam2::audio::kTestBufferSizes.size(); ++index) {
        lines.append(QStringLiteral("  %1 frames: %2")
            .arg(jam2::audio::kTestBufferSizes[index])
            .arg(capabilities.buffer_size_supported[index]
                ? QStringLiteral("supported") : QStringLiteral("not supported")));
    }
    return lines.join(QLatin1Char('\n'));
}

void showQuietDeviceMessage(QWidget* parent, const QString& text)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Test Device"));
    dialog.setModal(true);
    auto* message = new QLabel(text, &dialog);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    message->setWordWrap(true);
    message->setMinimumWidth(430);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(message);
    layout->addWidget(buttons);
    dialog.exec();
}

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

bool isComboBoxPopupObject(QObject* object)
{
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        if (className.contains(QStringLiteral("QComboBoxListView")) ||
            className.contains(QStringLiteral("QComboBoxPrivateContainer"))) {
            return true;
        }
    }
    return false;
}

QAbstractScrollArea* parentScrollArea(
    QObject* object,
    Qt::Orientation orientation)
{
    auto* widget = qobject_cast<QWidget*>(object);
    while (widget != nullptr) {
        if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(widget)) {
            QScrollBar* bar = orientation == Qt::Horizontal
                ? scrollArea->horizontalScrollBar()
                : scrollArea->verticalScrollBar();
            if (bar && bar->maximum() > bar->minimum()) return scrollArea;
        }
        widget = widget->parentWidget();
    }
    return nullptr;
}

bool scrollAreaByWheel(
    QAbstractScrollArea& scrollArea,
    QWheelEvent& wheel,
    Qt::Orientation orientation,
    bool useVerticalAxis = false)
{
    QScrollBar* bar = orientation == Qt::Horizontal
        ? scrollArea.horizontalScrollBar()
        : scrollArea.verticalScrollBar();
    if (!bar || bar->maximum() <= bar->minimum()) return false;
    const QPoint pixelDelta = wheel.pixelDelta();
    const QPoint angleDelta = wheel.angleDelta();
    int delta = orientation == Qt::Horizontal && !useVerticalAxis
        ? pixelDelta.x() : pixelDelta.y();
    if (delta == 0) {
        delta = (orientation == Qt::Horizontal && !useVerticalAxis
            ? angleDelta.x() : angleDelta.y()) / 8;
    }
    if (delta == 0) return false;
    bar->setValue(bar->value() - delta);
    return true;
}

} // namespace


MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
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
    , preparedMix_(trackWorkspace_.preparedMix)
    , fileWorkerPool_(trackWorkspace_.fileWorkers)
    , preparedMixWorkerRunning_(trackWorkspace_.preparedMixWorkerRunning)
    , preparedMixRerunPending_(trackWorkspace_.preparedMixRerunPending)
    , playPreparedMixWhenReady_(trackWorkspace_.playPreparedMixWhenReady)
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
        [this](const QString& hash) { retryOrFailIncomingAsset(hash); },
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
    addInitialEmptyLooperLanes(looperProject_);
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
            if (!sessionController_.isServer() && switchId == sharedBankSwitchId_) {
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
                if (targetOk && switchId == sharedBankSwitchId_) {
                    sharedBankSwitchId_.clear();
                    sharedBankSwitchIndex_ = -1;
                    sharedBankHostReady_ = false;
                    sharedBankReadyTokens_.clear();
                    schedulePreparedBankLaunch(
                        message.value(QStringLiteral("bank")).toInt(), targetBeat);
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
        if (sessionController_.isServer() && !sharedBankSwitchId_.isEmpty()) {
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
                    QStringLiteral("Selected audio device '%1' does not support session sample rate %2 Hz")
                        .arg(deviceBox_ ? deviceBox_->currentText() : QStringLiteral("unknown"))
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
        const bool valueEditorFocused = explicitValueEditorHasFocus(focus);
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
            if (navigationKey && blocksIncidentalNavigationKey(focus)) {
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
        if (!isComboBoxPopupObject(watched)) {
            const bool directHorizontal =
                wheel->pixelDelta().x() != 0 || wheel->angleDelta().x() != 0;
            const bool shiftedHorizontal =
                wheel->modifiers().testFlag(Qt::ShiftModifier) &&
                (wheel->pixelDelta().y() != 0 || wheel->angleDelta().y() != 0);
            if (directHorizontal || shiftedHorizontal) {
                if (auto* scrollArea = parentScrollArea(watched, Qt::Horizontal)) {
                    if (scrollAreaByWheel(
                            *scrollArea,
                            *wheel,
                            Qt::Horizontal,
                            shiftedHorizontal && !directHorizontal)) {
                        return true;
                    }
                }
            }
        }
        if (isWheelValueEditor(watched)) {
            if (isComboBoxPopupObject(watched)) return false;
            if (auto* scrollArea = parentScrollArea(watched, Qt::Vertical)) {
                if (scrollAreaByWheel(
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
    if (preferences_.recording.jam.promptForName) {
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
        : sampleRateSpin_ ? sampleRateSpin_->value() : preferences_.localAudio.sampleRate;
    const int buffer = bufferSizeSpin_
        ? bufferSizeSpin_->value() : preferences_.localAudio.bufferSize;
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
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Start Local Engine"));
    dialog.setObjectName(QStringLiteral("LocalEngineDialog"));
    dialog.setModal(true);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setSizeGripEnabled(false);
    dialog.setWindowFlag(Qt::WindowMaximizeButtonHint, false);
    auto* form = new QFormLayout(&dialog);
    form->setSizeConstraint(QLayout::SetFixedSize);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setFormAlignment(Qt::AlignHCenter | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setContentsMargins(22, 20, 22, 18);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(12);
    auto* device = new QComboBox(&dialog);
    device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    device->setMinimumContentsLength(42);
    device->setMinimumWidth(420);
    device->setMaximumWidth(520);
    for (int index = 0; deviceBox_ && index < deviceBox_->count(); ++index) {
        device->addItem(deviceBox_->itemText(index), deviceBox_->itemData(index));
    }
    int preferredDevice = -1;
    for (const auto& info : availableDevices_) {
        const QString stable = QString::fromStdString(info.clsid.empty() ? info.name : info.clsid);
        if (QString::fromStdString(info.backend) == preferences_.localAudio.backend &&
            stable == preferences_.localAudio.stableId) {
            preferredDevice = device->findData(QString::number(info.id));
            break;
        }
    }
    if (preferredDevice < 0 && deviceBox_) preferredDevice = device->findData(deviceBox_->currentData());
    device->setCurrentIndex(qMax(0, preferredDevice));
    auto* sampleRate = fixedSampleRateCombo(&dialog, preferences_.localAudio.sampleRate);
    auto* bufferSize = fixedBufferSizeCombo(&dialog, preferences_.localAudio.bufferSize);
    auto* inputChannels = new QLineEdit(preferences_.localAudio.inputChannels, &dialog);
    auto* outputChannels = new QLineEdit(preferences_.localAudio.outputChannels, &dialog);
    inputChannels->setMinimumWidth(320);
    outputChannels->setMinimumWidth(320);
    auto* testDevice = new QPushButton(QStringLiteral("Test Device"), &dialog);
    auto* saveDefaults = new QCheckBox(QStringLiteral("Save as Local defaults"), &dialog);
    form->addRow(QStringLiteral("Low-latency device"), device);
    form->addRow(QStringLiteral("Sample rate"), sampleRate);
    form->addRow(QStringLiteral("Buffer size"), bufferSize);
    form->addRow(QStringLiteral("Input channels"), inputChannels);
    form->addRow(QStringLiteral("Output channels"), outputChannels);
    form->addRow(QString(), testDevice);
    form->addRow(QString(), saveDefaults);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Start Engine"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(testDevice, &QPushButton::clicked, this, [this, device, testDevice, &dialog] {
        testDeviceSelection(device, testDevice, &dialog);
    });
    dialog.adjustSize();
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
    if (device->currentData().toString().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Perform"), QStringLiteral("Select a low-latency audio device first."));
        return;
    }
    deviceBox_->setCurrentIndex(qMax(0, deviceBox_->findData(device->currentData())));
    sampleRateSpin_->setValue(sampleRate->currentData().toInt());
    bufferSizeSpin_->setValue(bufferSize->currentData().toInt());
    inputChannelsEdit_->setText(inputChannels->text().trimmed());
    outputChannelsEdit_->setText(outputChannels->text().trimmed());
    if (saveDefaults->isChecked()) {
        preferences_.localAudio.sampleRate = sampleRateSpin_->value();
        preferences_.localAudio.bufferSize = bufferSizeSpin_->value();
        preferences_.localAudio.inputChannels = inputChannelsEdit_->text();
        preferences_.localAudio.outputChannels = outputChannelsEdit_->text();
        bool idOk = false;
        const int id = device->currentData().toInt(&idOk);
        const auto info = std::find_if(availableDevices_.begin(), availableDevices_.end(),
            [id](const auto& item) { return item.id == id; });
        if (idOk && info != availableDevices_.end()) {
            preferences_.localAudio.backend = QString::fromStdString(info->backend);
            preferences_.localAudio.stableId = QString::fromStdString(
                info->clsid.empty() ? info->name : info->clsid);
            preferences_.localAudio.name = QString::fromStdString(info->name);
        }
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
        jamSyncPolicy_.revision = qMax(1, jamSyncPolicy_.revision);
    } else {
        // The creator's first authenticated policy snapshot must supersede any
        // local choices made before joining this jam.
        jamSyncPolicy_.revision = -1;
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
    activeRecordingGroupId_.clear();
    activeRecordingGroupParticipants_.clear();
    recoveredRecordingGroupIds_.clear();
    activeRecordingGroupStartMessage_ = {};
    lastRecordingGroupFinishMessage_ = {};
    deferredRecordingControls_.clear();
    deferredRecordingControlsOverflowed_ = false;
    incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
    incomingAssetHash_.clear();
    incomingAssetSourcePeerToken_.clear();
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
        setSessionHeaderStatus(
            QStringLiteral("CONNECTION ISSUE"),
            action,
            {lastJamFailureDialog_.left(160)},
            true,
            true);
        QMessageBox::critical(this, action, lastJamFailureDialog_);
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
    if (networkAudioFormatBox_) {
        networkAudioFormatBox_->setEnabled(true);
    }
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
    if (networkAudioFormatBox_) {
        networkAudioFormatBox_->setEnabled(false);
    }
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
        if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
        if (!activeRecordingGroupId_.isEmpty()) {
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

void MainWindow::showStartJamDialog()
{
    const SharedSessionController::Role role = sessionController_.snapshot().role;
    if (role == SharedSessionController::Role::Creator ||
        role == SharedSessionController::Role::Joiner) {
        return;
    }

    const int sampleRateBeforeDialog =
        sampleRateSpin_ ? sampleRateSpin_->value() : 48000;
    applyCreateDefaultsToControls();
    refreshDevices();
    selectPreferredDevice(deviceBox_, availableDevices_, preferences_.createAudio());

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
        inputChannelsEdit_, outputChannelsEdit_, frameSizeSpin_, networkAudioFormatBox_,
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
    auto* sampleRate = fixedSampleRateCombo(content, sampleRateSpin_->value());
    auto* bufferSize = fixedBufferSizeCombo(content, bufferSizeSpin_->value());
    auto* testDevice = new QPushButton(QStringLiteral("Test Device"), content);
    audioForm->addRow(QStringLiteral("Profile"), profileBox_);
    audioForm->addRow(QStringLiteral("Audio device"), deviceBox_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Sample rate"), sampleRate);
    audioForm->addRow(QStringLiteral("Audio buffer size"), bufferSize);
    audioForm->addRow(QString(), testDevice);
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
    auto* saveDefaults = buttons->addButton(QStringLiteral("Save Defaults"), QDialogButtonBox::ActionRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    auto* regen = buttons->addButton(QStringLiteral("New Session"), QDialogButtonBox::ActionRole);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] {
        refreshDevices();
    });
    QObject::connect(regen, &QPushButton::clicked, this, [this] {
        generateSession();
    });
    QObject::connect(testDevice, &QPushButton::clicked, this, [this, testDevice, &dialog] {
        testDeviceSelection(deviceBox_, testDevice, &dialog);
    });
    QObject::connect(profileBox_, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [this, sampleRate, bufferSize] {
            sampleRate->setCurrentIndex(qMax(0, sampleRate->findData(sampleRateSpin_->value())));
            bufferSize->setCurrentIndex(qMax(0, bufferSize->findData(bufferSizeSpin_->value())));
        });
    QObject::connect(saveDefaults, &QPushButton::clicked, &dialog,
        [this, sampleRate, bufferSize] {
            sampleRateSpin_->setValue(sampleRate->currentData().toInt());
            bufferSizeSpin_->setValue(bufferSize->currentData().toInt());
            saveCreateDefaults();
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
        inputChannelsEdit_, outputChannelsEdit_, frameSizeSpin_, networkAudioFormatBox_,
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
        sampleRateSpin_->setValue(sampleRate->currentData().toInt());
        bufferSizeSpin_->setValue(bufferSize->currentData().toInt());
        startJam(true);
    } else if (sampleRateSpin_) {
        sampleRateSpin_->setValue(sampleRateBeforeDialog);
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
    selectPreferredDevice(deviceBox_, availableDevices_, preferences_.joinAudio());

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Join Jam"));
    dialog.resize(680, 520);

    auto* content = new QWidget(&dialog);
    auto* layout = new QVBoxLayout(content);
    const QList<QWidget*> visibleWidgets{
        connectUrlEdit_, bindHostEdit_, portSpin_, deviceBox_, inputChannelsEdit_, outputChannelsEdit_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_, streamMsSpin_,
        streamLingerMsSpin_, statsCheck_, statsWarmupMsSpin_, logStatsEdit_, osPriorityBox_,
        driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_, driftMaxCorrectionSpin_,
        sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_, jitterBufferMaxSpin_,
        adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_, adaptiveMaxSpin_,
        adaptiveReleaseSpin_, adaptiveRatioRampSpin_,
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
    auto* joinProfile = new QComboBox(content);
    for (const jam2::JoinProfile& profile : jam2::join_profiles()) {
        joinProfile->addItem(
            QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
            QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
    }
    joinProfile->setCurrentIndex(qMax(0, joinProfile->findData(joinProfileName_)));
    auto* bufferSize = fixedBufferSizeCombo(content, bufferSizeSpin_->value());
    auto* testDevice = new QPushButton(QStringLiteral("Test Device"), content);
    audioForm->addRow(QStringLiteral("Join profile"), joinProfile);
    audioForm->addRow(QStringLiteral("Audio device"), deviceBox_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Audio buffer size"), bufferSize);
    audioForm->addRow(QString(), new QLabel(
        QStringLiteral("The creator supplies the session sample rate and frame size."), content));
    audioForm->addRow(QString(), testDevice);
    audioForm->addRow(QStringLiteral("Playback prefill frames"), prefillSpin_);
    audioForm->addRow(QStringLiteral("Playback max frames"), playbackMaxSpin_);
    audioForm->addRow(QStringLiteral("Capture ring frames"), captureRingSpin_);
    audioForm->addRow(QStringLiteral("Playback ring frames"), playbackRingSpin_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    layout->addWidget(audioBox);

    auto* statsForm = new QFormLayout();
    statsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    statsForm->addRow(QString(), statsCheck_);
    statsForm->addRow(QStringLiteral("Stats warmup ms"), statsWarmupMsSpin_);
    statsForm->addRow(QStringLiteral("Log stats folder"), logStatsEdit_);
    statsForm->addRow(QStringLiteral("OS priority"), osPriorityBox_);
    statsForm->addRow(QStringLiteral("Wait ms"), waitMsSpin_);
    statsForm->addRow(QStringLiteral("Stream ms"), streamMsSpin_);
    statsForm->addRow(QStringLiteral("Stream linger ms"), streamLingerMsSpin_);
    auto* statsBox = new QGroupBox(QStringLiteral("Local Stats"), content);
    statsBox->setLayout(statsForm);
    layout->addWidget(statsBox);

    auto* tuningForm = new QFormLayout();
    tuningForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    tuningForm->addRow(QString(), driftCorrectionCheck_);
    tuningForm->addRow(QStringLiteral("Drift smoothing"), driftSmoothingSpin_);
    tuningForm->addRow(QStringLiteral("Drift deadband ppm"), driftDeadbandSpin_);
    tuningForm->addRow(QStringLiteral("Drift max correction ppm"), driftMaxCorrectionSpin_);
    tuningForm->addRow(QString(), sampleTimePlayoutCheck_);
    tuningForm->addRow(QStringLiteral("Playout delay frames"), playoutDelaySpin_);
    tuningForm->addRow(QStringLiteral("Jitter buffer frames"), jitterBufferSpin_);
    tuningForm->addRow(QStringLiteral("Jitter buffer max frames"), jitterBufferMaxSpin_);
    tuningForm->addRow(QString(), adaptiveCushionCheck_);
    tuningForm->addRow(QStringLiteral("Adaptive target frames"), adaptiveTargetSpin_);
    tuningForm->addRow(QStringLiteral("Adaptive min frames"), adaptiveMinSpin_);
    tuningForm->addRow(QStringLiteral("Adaptive max frames"), adaptiveMaxSpin_);
    tuningForm->addRow(QStringLiteral("Adaptive release ppm"), adaptiveReleaseSpin_);
    tuningForm->addRow(QStringLiteral("Adaptive ratio ramp ms"), adaptiveRatioRampSpin_);
    auto* tuningBox = new QGroupBox(QStringLiteral("Local Engine Options"), content);
    tuningBox->setLayout(tuningForm);
    layout->addWidget(tuningBox);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* join = buttons->addButton(QStringLiteral("Join"), QDialogButtonBox::AcceptRole);
    auto* saveDefaults = buttons->addButton(QStringLiteral("Save Defaults"), QDialogButtonBox::ActionRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] {
        refreshDevices();
    });
    QObject::connect(testDevice, &QPushButton::clicked, this, [this, testDevice, &dialog] {
        testDeviceSelection(deviceBox_, testDevice, &dialog);
    });
    QObject::connect(joinProfile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [this, joinProfile, bufferSize] {
            applyJoinProfileName(joinProfile->currentData().toString());
            bufferSize->setCurrentIndex(qMax(0, bufferSize->findData(bufferSizeSpin_->value())));
        });
    QObject::connect(saveDefaults, &QPushButton::clicked, &dialog,
        [this, joinProfile, bufferSize] {
            joinProfileName_ = joinProfile->currentData().toString();
            bufferSizeSpin_->setValue(bufferSize->currentData().toInt());
            saveJoinDefaults();
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
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_, streamMsSpin_,
        streamLingerMsSpin_, statsCheck_, statsWarmupMsSpin_, logStatsEdit_, osPriorityBox_,
        driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_, driftMaxCorrectionSpin_,
        sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_, jitterBufferMaxSpin_,
        adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_, adaptiveMaxSpin_,
        adaptiveReleaseSpin_, adaptiveRatioRampSpin_,
    };
    for (QWidget* widget : joinWidgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        joinProfileName_ = joinProfile->currentData().toString();
        bufferSizeSpin_->setValue(bufferSize->currentData().toInt());
        startJam(false);
    }
}

void MainWindow::showSettingsDialog()
{
    refreshDevices();
    refreshLoopbackSources();
    const bool networkActive = jam2_.isNetworkRunning() ||
        sessionController_.snapshot().role == SharedSessionController::Role::Creator ||
        sessionController_.snapshot().role == SharedSessionController::Role::Joiner;
    const bool localActive = jam2_.isRunning() && !networkActive;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    const QRect availableSettingsGeometry = dialog.screen()->availableGeometry();
    const int settingsWidth = qMax(820, qMin(1180, availableSettingsGeometry.width() - 80));
    const int settingsHeight = qMax(620, qMin(820, availableSettingsGeometry.height() - 80));
    dialog.setMinimumSize(qMin(1000, settingsWidth), qMin(680, settingsHeight));
    dialog.resize(settingsWidth, settingsHeight);

    auto makeSpin = [&dialog](int value, int minimum, int maximum) {
        auto* spin = new QSpinBox(&dialog);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setAttribute(Qt::WA_MacShowFocusRect, false);
        return spin;
    };
    auto makeDoubleSpin = [&dialog](double value, double minimum, double maximum, int decimals) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(minimum, maximum);
        spin->setDecimals(decimals);
        spin->setValue(value);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setAttribute(Qt::WA_MacShowFocusRect, false);
        return spin;
    };
    auto makePriority = [&dialog](const QString& value) {
        auto* combo = new QComboBox(&dialog);
        combo->addItem(QStringLiteral("Realtime"), QStringLiteral("realtime"));
        combo->addItem(QStringLiteral("High"), QStringLiteral("high"));
        combo->addItem(QStringLiteral("Off"), QStringLiteral("off"));
        combo->setCurrentIndex(qMax(0, combo->findData(value)));
        return combo;
    };
    auto makeScrollTab = [&dialog](QWidget* content) {
        auto* scroll = new QScrollArea(&dialog);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        scroll->setWidget(content);
        return scroll;
    };

    auto makeDeviceCombo = [this, &dialog](const AudioDevicePreference& preference) {
        auto* combo = new QComboBox(&dialog);
        for (int index = 0; deviceBox_ && index < deviceBox_->count(); ++index) {
            combo->addItem(deviceBox_->itemText(index), deviceBox_->itemData(index));
        }
        selectPreferredDevice(combo, availableDevices_, preference);
        if (combo->currentIndex() < 0 && combo->count() > 0) combo->setCurrentIndex(0);
        return combo;
    };

    AudioDevicePreference localInitial = preferences_.localAudio;
    if (localActive) {
        localInitial.sampleRate = sampleRateSpin_->value();
        localInitial.bufferSize = bufferSizeSpin_->value();
        localInitial.inputChannels = inputChannelsEdit_->text();
        localInitial.outputChannels = outputChannelsEdit_->text();
        storeSelectedDevice(localInitial, deviceBox_, availableDevices_);
    }

    auto* localDevice = makeDeviceCombo(localInitial);
    auto* localSampleRate = fixedSampleRateCombo(&dialog, localInitial.sampleRate);
    auto* localBufferSize = fixedBufferSizeCombo(&dialog, localInitial.bufferSize);
    auto* localInput = new QLineEdit(localInitial.inputChannels, &dialog);
    auto* localOutput = new QLineEdit(localInitial.outputChannels, &dialog);
    auto* localTest = new QPushButton(QStringLiteral("Test Device"), &dialog);
    auto* localApply = new QPushButton(QStringLiteral("Apply Audio"), &dialog);
    auto* localActions = new QWidget(&dialog);
    auto* localActionsLayout = new QHBoxLayout(localActions);
    localActionsLayout->setContentsMargins(0, 0, 0, 0);
    localActionsLayout->setSpacing(8);
    localActionsLayout->addWidget(localTest, 1);
    localActionsLayout->addWidget(localApply, 1);
    auto* localForm = new QFormLayout();
    localForm->addRow(QStringLiteral("Device"), localDevice);
    localForm->addRow(QStringLiteral("Sample rate"), localSampleRate);
    localForm->addRow(QStringLiteral("Buffer size"), localBufferSize);
    localForm->addRow(QStringLiteral("Input channels"), localInput);
    localForm->addRow(QStringLiteral("Output channels"), localOutput);
    localForm->addRow(QString(), localActions);
    auto* localBox = new QGroupBox(QStringLiteral("Local Audio"), &dialog);
    localBox->setLayout(localForm);

    struct NetworkAudioEditors {
        QComboBox* device = nullptr;
        QLineEdit* input = nullptr;
        QLineEdit* output = nullptr;
        QPushButton* test = nullptr;
        QGroupBox* box = nullptr;
    };
    auto makeNetworkAudioEditors = [&](const QString& title, const AudioDevicePreference& preference) {
        NetworkAudioEditors editors;
        editors.device = makeDeviceCombo(preference);
        editors.input = new QLineEdit(preference.inputChannels, &dialog);
        editors.output = new QLineEdit(preference.outputChannels, &dialog);
        editors.test = new QPushButton(QStringLiteral("Test Device"), &dialog);
        auto* form = new QFormLayout();
        form->addRow(QStringLiteral("Device"), editors.device);
        form->addRow(QStringLiteral("Input channels"), editors.input);
        form->addRow(QStringLiteral("Output channels"), editors.output);
        form->addRow(QString(), editors.test);
        editors.box = new QGroupBox(title, &dialog);
        editors.box->setLayout(form);
        return editors;
    };
    const NetworkAudioEditors networkAudio = makeNetworkAudioEditors(
        QStringLiteral("Network Audio"), preferences_.networkAudio);
    const NetworkAudioEditors createJamAudio = makeNetworkAudioEditors(
        QStringLiteral("Create Jam Audio"), preferences_.createJamAudio);
    const NetworkAudioEditors joinJamAudio = makeNetworkAudioEditors(
        QStringLiteral("Join Jam Audio"), preferences_.joinJamAudio);
    auto* splitNetworkAudio = new QCheckBox(
        QStringLiteral("Use different audio devices and channels for Create and Join"), &dialog);
    splitNetworkAudio->setChecked(preferences_.splitNetworkAudioByRole);

    auto* audioContent = new QWidget(&dialog);
    auto* audioLayout = new QVBoxLayout(audioContent);
    audioLayout->addWidget(localBox);
    audioLayout->addWidget(splitNetworkAudio);
    audioLayout->addWidget(networkAudio.box);
    audioLayout->addWidget(createJamAudio.box);
    audioLayout->addWidget(joinJamAudio.box);
    audioLayout->addStretch(1);
    auto audioFromEditors = [this](
        const AudioDevicePreference& original,
        const NetworkAudioEditors& editors)
    {
        AudioDevicePreference value = original;
        value.inputChannels = editors.input->text().trimmed();
        value.outputChannels = editors.output->text().trimmed();
        storeSelectedDevice(value, editors.device, availableDevices_);
        return value;
    };
    auto applyAudioToEditors = [this](
        const AudioDevicePreference& value,
        const NetworkAudioEditors& editors)
    {
        selectPreferredDevice(editors.device, availableDevices_, value);
        editors.input->setText(value.inputChannels);
        editors.output->setText(value.outputChannels);
    };
    auto updateNetworkAudioVisibility = [=] {
        const bool split = splitNetworkAudio->isChecked();
        networkAudio.box->setVisible(!split);
        createJamAudio.box->setVisible(split);
        joinJamAudio.box->setVisible(split);
    };
    QObject::connect(splitNetworkAudio, &QCheckBox::toggled, &dialog,
        [=, this, splitInitialized = preferences_.splitNetworkAudioByRole](bool checked) mutable {
            if (checked && !splitInitialized) {
                const AudioDevicePreference shared = audioFromEditors(
                    preferences_.networkAudio, networkAudio);
                applyAudioToEditors(shared, createJamAudio);
                applyAudioToEditors(shared, joinJamAudio);
                splitInitialized = true;
            }
            updateNetworkAudioVisibility();
        });
    updateNetworkAudioVisibility();

    auto* createBind = new QLineEdit(preferences_.create.bindHost, &dialog);
    auto* createPort = makeSpin(preferences_.create.port, 1, 65535);
    auto* createManualEndpoint = new QCheckBox(QStringLiteral("Use manual public endpoint (disable STUN)"), &dialog);
    createManualEndpoint->setChecked(preferences_.create.noStun);
    auto* createPublicHost = new QLineEdit(preferences_.create.publicHost, &dialog);
    auto* createStun = new QLineEdit(preferences_.create.stunServer, &dialog);
    auto* createStunTimeout = makeSpin(preferences_.create.stunTimeoutMs, 1, 60000);
    auto* createStunRetries = makeSpin(preferences_.create.stunRetries, 0, 100);
    auto* createMaxPeers = makeSpin(preferences_.create.maxPeers, 0, 1024);
    auto* createSocketSend = makeSpin(preferences_.create.socketSendBuffer, 0, 16777216);
    auto* createSocketReceive = makeSpin(preferences_.create.socketRecvBuffer, 0, 16777216);
    auto* connectionContent = new QWidget(&dialog);
    auto* connectionForm = new QFormLayout(connectionContent);
    connectionForm->addRow(QStringLiteral("Bind host"), createBind);
    connectionForm->addRow(QStringLiteral("Port"), createPort);
    connectionForm->addRow(QString(), createManualEndpoint);
    connectionForm->addRow(QStringLiteral("Public endpoint host"), createPublicHost);
    connectionForm->addRow(QStringLiteral("STUN server"), createStun);
    connectionForm->addRow(QStringLiteral("STUN timeout ms"), createStunTimeout);
    connectionForm->addRow(QStringLiteral("STUN retries"), createStunRetries);
    connectionForm->addRow(QStringLiteral("Maximum peers (0 = unlimited)"), createMaxPeers);
    connectionForm->addRow(QStringLiteral("Socket send buffer (0 = system)"), createSocketSend);
    connectionForm->addRow(QStringLiteral("Socket receive buffer (0 = system)"), createSocketReceive);
    auto updateDiscoveryControls = [=] {
        const bool manual = createManualEndpoint->isChecked();
        createPublicHost->setEnabled(manual);
        createStun->setEnabled(!manual);
        createStunTimeout->setEnabled(!manual);
        createStunRetries->setEnabled(!manual);
    };
    QObject::connect(createManualEndpoint, &QCheckBox::toggled, &dialog, updateDiscoveryControls);
    updateDiscoveryControls();

    struct TuningEditors {
        QComboBox* profile = nullptr;
        QComboBox* buffer = nullptr;
        QSpinBox* frame = nullptr;
        QSpinBox* prefill = nullptr;
        QSpinBox* playbackMax = nullptr;
        QSpinBox* captureRing = nullptr;
        QSpinBox* playbackRing = nullptr;
        QCheckBox* drift = nullptr;
        QDoubleSpinBox* driftSmoothing = nullptr;
        QSpinBox* driftDeadband = nullptr;
        QSpinBox* driftMax = nullptr;
        QCheckBox* sampleTime = nullptr;
        QSpinBox* playout = nullptr;
        QSpinBox* jitter = nullptr;
        QSpinBox* jitterMax = nullptr;
        QCheckBox* adaptive = nullptr;
        QSpinBox* adaptiveTarget = nullptr;
        QSpinBox* adaptiveMin = nullptr;
        QSpinBox* adaptiveMax = nullptr;
        QSpinBox* adaptiveRelease = nullptr;
        QSpinBox* adaptiveRamp = nullptr;
    };
    struct RuntimeEditors {
        QCheckBox* diagnostics = nullptr;
        QSpinBox* warmup = nullptr;
        QComboBox* priority = nullptr;
        QSpinBox* wait = nullptr;
        QSpinBox* stream = nullptr;
        QSpinBox* linger = nullptr;
    };
    auto addTuning = [&](QFormLayout* form, const LocalTuningPreference& p, bool creator) {
        TuningEditors e;
        e.profile = new QComboBox(&dialog);
        if (creator) {
            for (const jam2::CreateProfile& profile : jam2::create_profiles()) {
                e.profile->addItem(
                    QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
                    QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
            }
        } else {
            for (const jam2::JoinProfile& profile : jam2::join_profiles()) {
                e.profile->addItem(
                    QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
                    QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
            }
        }
        e.profile->setCurrentIndex(qMax(0, e.profile->findData(p.profile)));
        e.buffer = fixedBufferSizeCombo(&dialog, p.bufferSize);
        if (creator) {
            e.frame = makeSpin(p.frameSize, 32, 256);
        }
        e.prefill = makeSpin(p.prefillFrames, 0, 1048576);
        e.playbackMax = makeSpin(p.playbackMaxFrames, 0, 1048576);
        e.captureRing = makeSpin(p.captureRingFrames, 1, 1048576);
        e.playbackRing = makeSpin(p.playbackRingFrames, 1, 1048576);
        e.drift = new QCheckBox(QStringLiteral("Drift correction"), &dialog); e.drift->setChecked(p.driftCorrection);
        e.driftSmoothing = makeDoubleSpin(p.driftSmoothing, 0.0, 1.0, 3); e.driftSmoothing->setSingleStep(0.005);
        e.driftDeadband = makeSpin(p.driftDeadbandPpm, 0, 50000);
        e.driftMax = makeSpin(p.driftMaxCorrectionPpm, 0, 50000);
        e.sampleTime = new QCheckBox(QStringLiteral("Sample-time playout"), &dialog); e.sampleTime->setChecked(p.sampleTimePlayout);
        e.playout = makeSpin(p.playoutDelayFrames, 0, 1048576);
        e.jitter = makeSpin(p.jitterBufferFrames, 0, 1048576);
        e.jitterMax = makeSpin(p.jitterBufferMaxFrames, 0, 1048576);
        e.adaptive = new QCheckBox(QStringLiteral("Adaptive playback cushion"), &dialog); e.adaptive->setChecked(p.adaptiveCushion);
        e.adaptiveTarget = makeSpin(p.adaptiveTargetFrames, 0, 1048576);
        e.adaptiveMin = makeSpin(p.adaptiveMinFrames, 0, 1048576);
        e.adaptiveMax = makeSpin(p.adaptiveMaxFrames, 0, 1048576);
        e.adaptiveRelease = makeSpin(p.adaptiveReleasePpm, 0, 1000000);
        e.adaptiveRamp = makeSpin(p.adaptiveRatioRampMs, 0, 60000);
        form->addRow(QStringLiteral("Profile"), e.profile);
        form->addRow(QStringLiteral("Local device buffer"), e.buffer);
        if (e.frame != nullptr) {
            form->addRow(QStringLiteral("Session frame size"), e.frame);
        }
        form->addRow(QStringLiteral("Playback prefill frames"), e.prefill);
        form->addRow(QStringLiteral("Playback max frames"), e.playbackMax);
        form->addRow(QStringLiteral("Capture ring frames"), e.captureRing);
        form->addRow(QStringLiteral("Playback ring frames"), e.playbackRing);
        form->addRow(QString(), e.drift);
        form->addRow(QStringLiteral("Drift smoothing"), e.driftSmoothing);
        form->addRow(QStringLiteral("Drift deadband ppm"), e.driftDeadband);
        form->addRow(QStringLiteral("Drift max correction ppm"), e.driftMax);
        form->addRow(QString(), e.sampleTime);
        form->addRow(QStringLiteral("Playout delay frames"), e.playout);
        form->addRow(QStringLiteral("Jitter target frames"), e.jitter);
        form->addRow(QStringLiteral("Jitter max frames"), e.jitterMax);
        form->addRow(QString(), e.adaptive);
        form->addRow(QStringLiteral("Adaptive target frames"), e.adaptiveTarget);
        form->addRow(QStringLiteral("Adaptive minimum frames"), e.adaptiveMin);
        form->addRow(QStringLiteral("Adaptive maximum frames"), e.adaptiveMax);
        form->addRow(QStringLiteral("Adaptive release ppm"), e.adaptiveRelease);
        form->addRow(QStringLiteral("Adaptive ratio ramp ms"), e.adaptiveRamp);
        return e;
    };
    auto addRuntime = [&](QFormLayout* form, const RuntimePreference& p) {
        RuntimeEditors e;
        e.diagnostics = new QCheckBox(QStringLiteral("Connection diagnostics and CSV logging"), &dialog);
        e.diagnostics->setChecked(p.diagnostics);
        e.warmup = makeSpin(p.diagnosticsWarmupMs, 0, 3600000);
        e.priority = makePriority(p.osPriority);
        e.wait = makeSpin(p.waitMs, 0, 86400000);
        e.stream = makeSpin(p.streamMs, 0, 86400000);
        e.linger = makeSpin(p.streamLingerMs, 0, 3600000);
        form->addRow(QString(), e.diagnostics);
        form->addRow(QStringLiteral("Stats warmup ms"), e.warmup);
        form->addRow(QStringLiteral("OS priority"), e.priority);
        form->addRow(QStringLiteral("Wait ms"), e.wait);
        form->addRow(QStringLiteral("Stream limit ms (0 = unlimited)"), e.stream);
        form->addRow(QStringLiteral("Stream linger ms"), e.linger);
        return e;
    };

    auto* createContent = new QWidget(&dialog);
    auto* createForm = new QFormLayout(createContent);
    auto* createRate = fixedSampleRateCombo(&dialog, preferences_.create.sampleRate);
    auto* createQuality = new QComboBox(&dialog);
    createQuality->addItem(QStringLiteral("16-bit PCM"), QStringLiteral("pcm16-mono"));
    createQuality->addItem(QStringLiteral("24-bit PCM"), QStringLiteral("pcm24-mono"));
    createQuality->setCurrentIndex(qMax(0, createQuality->findData(preferences_.create.audioFormat)));
    createForm->addRow(QStringLiteral("Session sample rate"), createRate);
    createForm->addRow(QStringLiteral("Audio quality"), createQuality);
    TuningEditors createTuning = addTuning(createForm, preferences_.create.tuning, true);
    RuntimeEditors createRuntime = addRuntime(createForm, preferences_.create.runtime);

    auto* joinContent = new QWidget(&dialog);
    auto* joinForm = new QFormLayout(joinContent);
    auto* joinBind = new QLineEdit(preferences_.join.bindHost, &dialog);
    auto* joinPort = makeSpin(preferences_.join.port, 1, 65535);
    joinForm->addRow(QStringLiteral("Local bind host"), joinBind);
    joinForm->addRow(QStringLiteral("Local bind port"), joinPort);
    auto* joinContractNotice = new QLabel(
        QStringLiteral("The creator supplies sample rate, frame size, and audio quality."), &dialog);
    joinContractNotice->setWordWrap(true);
    joinForm->addRow(QString(), joinContractNotice);
    TuningEditors joinTuning = addTuning(joinForm, preferences_.join.tuning, false);
    RuntimeEditors joinRuntime = addRuntime(joinForm, preferences_.join.runtime);

    auto applyJoinProfile = [](TuningEditors& e, const jam2::JoinProfile& p) {
        e.buffer->setCurrentIndex(qMax(0, e.buffer->findData(static_cast<int>(p.audio_buffer_size))));
        e.prefill->setValue(static_cast<int>(p.playback_prefill_frames));
        e.playbackMax->setValue(static_cast<int>(p.playback_max_frames));
        e.captureRing->setValue(static_cast<int>(p.capture_ring_frames));
        e.playbackRing->setValue(static_cast<int>(p.playback_ring_frames));
        e.drift->setChecked(p.drift_correction); e.driftSmoothing->setValue(p.drift_smoothing);
        e.driftDeadband->setValue(p.drift_deadband_ppm); e.driftMax->setValue(p.drift_max_correction_ppm);
        e.sampleTime->setChecked(p.sample_time_playout); e.playout->setValue(static_cast<int>(p.playout_delay_frames));
        e.jitter->setValue(static_cast<int>(p.jitter_buffer_frames)); e.jitterMax->setValue(static_cast<int>(p.jitter_buffer_max_frames));
        e.adaptive->setChecked(p.adaptive_playback_cushion);
        e.adaptiveTarget->setValue(static_cast<int>(p.adaptive_playback_target_frames));
        e.adaptiveMin->setValue(static_cast<int>(p.adaptive_playback_min_frames));
        e.adaptiveMax->setValue(static_cast<int>(p.adaptive_playback_max_frames));
        e.adaptiveRelease->setValue(p.adaptive_playback_release_ppm);
        e.adaptiveRamp->setValue(p.adaptive_playback_ratio_ramp_ms);
    };
    QObject::connect(createTuning.profile, qOverload<int>(&QComboBox::activated), &dialog,
        [=, &createTuning](int) {
            const auto* p = jam2::find_create_profile(createTuning.profile->currentData().toString().toStdString());
            if (!p || !p->local) return;
            createRate->setCurrentIndex(qMax(0, createRate->findData(p->sample_rate)));
            createTuning.frame->setValue(p->frame_size);
            applyJoinProfile(createTuning, *p->local);
        });
    QObject::connect(joinTuning.profile, qOverload<int>(&QComboBox::activated), &dialog,
        [=, &joinTuning](int) {
            const auto* p = jam2::find_join_profile(joinTuning.profile->currentData().toString().toStdString());
            if (p) applyJoinProfile(joinTuning, *p);
        });

    auto* logContent = new QWidget(&dialog);
    auto* logForm = new QFormLayout(logContent);
    auto* logFolder = new QLineEdit(preferences_.logging.folder, &dialog);
    auto* browseLogs = new QPushButton(QStringLiteral("Browse"), &dialog);
    auto* logRow = new QWidget(&dialog); auto* logRowLayout = new QHBoxLayout(logRow);
    logRowLayout->setContentsMargins(0, 0, 0, 0); logRowLayout->addWidget(logFolder, 1); logRowLayout->addWidget(browseLogs);
    logForm->addRow(QStringLiteral("GUI and CSV log folder"), logRow);
    auto* logNote = new QLabel(
        QStringLiteral("GUI jams write hidden 2-second CSV samples plus a final row. The GUI display remains compact and stdout remains quiet."),
        &dialog);
    logNote->setWordWrap(true); logForm->addRow(QString(), logNote);
    QObject::connect(browseLogs, &QPushButton::clicked, &dialog, [&dialog, logFolder] {
        const QString folder = QFileDialog::getExistingDirectory(
            &dialog,
            QStringLiteral("Log Folder"),
            logFolder->text(),
            QFileDialog::ShowDirsOnly);
        if (!folder.isEmpty()) logFolder->setText(QDir::toNativeSeparators(folder));
    });

    auto* recordingContent = new QWidget(&dialog);
    auto* recordingLayout = new QVBoxLayout(recordingContent);
    recordingLayout->setSpacing(12);
    auto* recordingTiles = new QWidget(recordingContent);
    auto* recordingTilesLayout = new QHBoxLayout(recordingTiles);
    recordingTilesLayout->setContentsMargins(0, 0, 0, 0);
    recordingTilesLayout->setSpacing(10);
    auto* jamRecordingTile = new QPushButton(QStringLiteral("JAM RECORDING\nGlobal multi-stem capture"), recordingTiles);
    auto* trackRecordingTile = new QPushButton(QStringLiteral("TRACK RECORDING\nInput, Jam Mix, or loopback"), recordingTiles);
    for (QPushButton* tile : {jamRecordingTile, trackRecordingTile}) {
        tile->setCheckable(true);
        tile->setMinimumHeight(62);
        tile->setStyleSheet(QStringLiteral(
            "QPushButton { text-align:left; color:#bdc8c6; background:#11191b; "
            "border:1px solid #344245; border-radius:5px; padding:9px 14px; }"
            "QPushButton:hover { color:#eef2ef; background:#182224; border-color:#506164; }"
            "QPushButton:checked { color:#f2c66d; background:#211b12; "
            "border-color:#8a6835; font-weight:600; }"));
        recordingTilesLayout->addWidget(tile, 1);
    }
    auto* recordingTileGroup = new QButtonGroup(recordingTiles);
    recordingTileGroup->setExclusive(true);
    recordingTileGroup->addButton(jamRecordingTile, 0);
    recordingTileGroup->addButton(trackRecordingTile, 1);
    auto* recordingPages = new QStackedWidget(recordingContent);
    auto* jamRecordingPage = new QWidget(recordingPages);
    auto* jamRecordingPageLayout = new QVBoxLayout(jamRecordingPage);
    jamRecordingPageLayout->setContentsMargins(0, 0, 0, 0);
    auto* trackRecordingPage = new QWidget(recordingPages);
    auto* trackRecordingPageLayout = new QVBoxLayout(trackRecordingPage);
    trackRecordingPageLayout->setContentsMargins(0, 0, 0, 0);
    recordingPages->addWidget(jamRecordingPage);
    recordingPages->addWidget(trackRecordingPage);
    QObject::connect(recordingTileGroup, &QButtonGroup::idClicked,
        recordingPages, &QStackedWidget::setCurrentIndex);
    jamRecordingTile->setChecked(true);
    recordingPages->setCurrentIndex(0);
    recordingLayout->addWidget(recordingTiles);
    recordingLayout->addWidget(recordingPages, 1);

    auto* preferredMode = new QComboBox(&dialog);
    preferredMode->addItem(QStringLiteral("Local Input (instrument)"), QStringLiteral("input"));
    preferredMode->addItem(QStringLiteral("Jam Mix (Jam2 input + peers)"), QStringLiteral("current-jam"));
    preferredMode->addItem(QStringLiteral("System Loopback (desktop audio)"), QStringLiteral("loopback"));
    preferredMode->setCurrentIndex(qMax(0, preferredMode->findData(preferences_.recording.preferredMode)));
    auto* preferredForm = new QFormLayout(); preferredForm->addRow(QStringLiteral("Preferred recording mode"), preferredMode);
    trackRecordingPageLayout->addLayout(preferredForm);

    auto* jamMixIncludeBacking = new QCheckBox(
        QStringLiteral("Include the Section backing track"), &dialog);
    auto* jamMixIncludeMetronome = new QCheckBox(
        QStringLiteral("Include the metronome in the WAV"), &dialog);
    jamMixIncludeBacking->setChecked(
        preferences_.recording.jamMixTrack.includeBackingTrack);
    jamMixIncludeMetronome->setChecked(
        preferences_.recording.jamMixTrack.includeMetronome);
    auto* jamMixNote = new QLabel(QStringLiteral(
        "Jam Mix records your local input together with audio received from Jam2 peers. It is internal Jam2 audio, not the operating-system loopback source."),
        &dialog);
    jamMixNote->setWordWrap(true);
    jamMixNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    auto* jamMixForm = new QFormLayout();
    jamMixForm->addRow(QString(), jamMixIncludeBacking);
    jamMixForm->addRow(QString(), jamMixIncludeMetronome);
    jamMixForm->addRow(QString(), jamMixNote);
    auto* jamMixBox = new QGroupBox(QStringLiteral("Jam Mix source"), &dialog);
    jamMixBox->setLayout(jamMixForm);
    trackRecordingPageLayout->addWidget(jamMixBox);

    auto* jamPromptName = new QCheckBox(
        QStringLiteral("Ask for a recording name when starting"), &dialog);
    jamPromptName->setChecked(preferences_.recording.jam.promptForName);
    auto* jamCompletion = new QComboBox(&dialog);
    jamCompletion->addItem(QStringLiteral("Ask whether to import"), QStringLiteral("ask"));
    jamCompletion->addItem(QStringLiteral("Open the import dialog"), QStringLiteral("import"));
    jamCompletion->addItem(QStringLiteral("Only show where files were saved"), QStringLiteral("notify"));
    jamCompletion->setCurrentIndex(qMax(0,
        jamCompletion->findData(preferences_.recording.jam.completionAction)));
    auto* jamImportMix = new QCheckBox(QStringLiteral("Mix (what you heard)"), &dialog);
    auto* jamImportMyInput = new QCheckBox(QStringLiteral("My Input"), &dialog);
    auto* jamImportTheirInput = new QCheckBox(QStringLiteral("Their Input"), &dialog);
    auto* jamImportInputsMix = new QCheckBox(QStringLiteral("Inputs Mix"), &dialog);
    auto* jamImportMetronome = new QCheckBox(QStringLiteral("Metronome"), &dialog);
    jamImportMix->setChecked(preferences_.recording.jam.importMix);
    jamImportMyInput->setChecked(preferences_.recording.jam.importMyInput);
    jamImportTheirInput->setChecked(preferences_.recording.jam.importTheirInput);
    jamImportInputsMix->setChecked(preferences_.recording.jam.importInputsMix);
    jamImportMetronome->setChecked(preferences_.recording.jam.importMetronome);
    auto* jamImportParts = new QWidget(&dialog);
    auto* jamImportPartsLayout = new QVBoxLayout(jamImportParts);
    jamImportPartsLayout->setContentsMargins(0, 0, 0, 0);
    jamImportPartsLayout->setSpacing(4);
    for (QCheckBox* check : {jamImportMix, jamImportMyInput, jamImportTheirInput,
             jamImportInputsMix, jamImportMetronome}) {
        jamImportPartsLayout->addWidget(check);
    }
    auto* jamRecordingNote = new QLabel(QStringLiteral(
        "Jam recordings are stored inside the current JamJar. The engine always captures the complete stem set; these choices control which stems are preselected when importing them into a Section."),
        &dialog);
    jamRecordingNote->setWordWrap(true);
    jamRecordingNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    auto* jamRecordingForm = new QFormLayout();
    jamRecordingForm->addRow(QString(), jamPromptName);
    jamRecordingForm->addRow(QStringLiteral("When recording finishes"), jamCompletion);
    jamRecordingForm->addRow(QStringLiteral("Default import tracks"), jamImportParts);
    jamRecordingForm->addRow(QString(), jamRecordingNote);
    auto* jamRecordingBox = new QGroupBox(QStringLiteral("Jam Recording"), &dialog);
    jamRecordingBox->setLayout(jamRecordingForm);
    jamRecordingPageLayout->addWidget(jamRecordingBox);
    jamRecordingPageLayout->addStretch(1);

    auto makeFolderRow = [&](const QString& value) {
        auto* edit = new QLineEdit(value, &dialog);
        auto* browse = new QPushButton(QStringLiteral("Browse"), &dialog);
        auto* row = new QWidget(&dialog); auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(edit, 1); layout->addWidget(browse);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, edit] {
            const QString folder = QFileDialog::getExistingDirectory(
                &dialog,
                QStringLiteral("Recording Folder"),
                edit->text(),
                QFileDialog::ShowDirsOnly);
            if (!folder.isEmpty()) edit->setText(QDir::toNativeSeparators(folder));
        });
        return std::pair<QLineEdit*, QWidget*>{edit, row};
    };
    const auto inputFolderRow = makeFolderRow(preferences_.recording.input.outputFolder);
    auto* inputUntilStopped = new QCheckBox(QStringLiteral("Record until stopped"), &dialog);
    inputUntilStopped->setChecked(preferences_.recording.input.recordUntilStopped);
    auto* inputDuration = makeSpin(preferences_.recording.input.durationBars, 1, 128);
    auto* inputCountIn = new QCheckBox(QStringLiteral("Count-in"), &dialog); inputCountIn->setChecked(preferences_.recording.input.countIn);
    auto* inputCountBars = makeSpin(preferences_.recording.input.countInBars, 1, 8);
    auto* inputCountMetro = new QCheckBox(QStringLiteral("Metronome during count-in"), &dialog); inputCountMetro->setChecked(preferences_.recording.input.countInMetronome);
    auto* inputKeepMetro = new QCheckBox(QStringLiteral("Keep metronome on while recording"), &dialog); inputKeepMetro->setChecked(preferences_.recording.input.keepMetronome);
    auto* inputLatency = makeSpin(preferences_.recording.input.latencyAdjustmentFrames, -8192, 8192);
    auto* inputForm = new QFormLayout();
    inputForm->addRow(QStringLiteral("Output folder"), inputFolderRow.second);
    inputForm->addRow(QString(), inputUntilStopped); inputForm->addRow(QStringLiteral("Duration bars"), inputDuration);
    inputForm->addRow(QString(), inputCountIn); inputForm->addRow(QStringLiteral("Count-in bars"), inputCountBars);
    inputForm->addRow(QString(), inputCountMetro); inputForm->addRow(QString(), inputKeepMetro);
    inputForm->addRow(QStringLiteral("Manual latency adjustment frames"), inputLatency);
    auto* inputBox = new QGroupBox(QStringLiteral("Local Input Recording"), &dialog); inputBox->setLayout(inputForm);
    trackRecordingPageLayout->addWidget(inputBox);
    inputDuration->setEnabled(!inputUntilStopped->isChecked());
    QObject::connect(inputUntilStopped, &QCheckBox::toggled, inputDuration, [=](bool checked) { inputDuration->setEnabled(!checked); });

    const auto loopFolderRow = makeFolderRow(preferences_.recording.loopback.outputFolder);
    auto* loopSource = new QComboBox(&dialog); loopSource->setEditable(true);
    auto populateLoopSources = [=, this] {
        const QString wanted = loopSource->currentData().toString().isEmpty()
            ? preferences_.recording.loopback.sourceId : loopSource->currentData().toString();
        loopSource->clear();
        for (int i = 0; loopbackSourceBox_ && i < loopbackSourceBox_->count(); ++i) {
            loopSource->addItem(loopbackSourceBox_->itemText(i), loopbackSourceBox_->itemData(i));
        }
        int index = loopSource->findData(wanted);
        if (index < 0) index = loopSource->findText(preferences_.recording.loopback.sourceName);
        if (index < 0) index = loopSource->findData(QStringLiteral("default"));
        loopSource->setCurrentIndex(qMax(0, index));
    };
    populateLoopSources();
    auto* refreshLoopSources = new QPushButton(QStringLiteral("Refresh Sources"), &dialog);
    auto* loopSourceRow = new QWidget(&dialog); auto* loopSourceLayout = new QHBoxLayout(loopSourceRow);
    loopSourceLayout->setContentsMargins(0, 0, 0, 0); loopSourceLayout->addWidget(loopSource, 1); loopSourceLayout->addWidget(refreshLoopSources);
    QObject::connect(refreshLoopSources, &QPushButton::clicked, &dialog, [=, this] { refreshLoopbackSources(); populateLoopSources(); });
    auto* loopUntilStopped = new QCheckBox(QStringLiteral("Record until stopped"), &dialog); loopUntilStopped->setChecked(preferences_.recording.loopback.recordUntilStopped);
    auto* loopDuration = makeSpin(preferences_.recording.loopback.durationBars, 1, 128);
    auto* loopSilenceThreshold = makeDoubleSpin(preferences_.recording.loopback.silenceThresholdDb, -120.0, 0.0, 1);
    auto* loopTailSilence = makeSpin(preferences_.recording.loopback.tailSilenceMs, 0, 30000);
    auto* loopTrimLeading = new QCheckBox(QStringLiteral("Trim leading silence"), &dialog); loopTrimLeading->setChecked(preferences_.recording.loopback.trimLeading);
    auto* loopTrimTrailing = new QCheckBox(QStringLiteral("Trim trailing silence"), &dialog); loopTrimTrailing->setChecked(preferences_.recording.loopback.trimTrailing);
    auto* loopForm = new QFormLayout();
    loopForm->addRow(QStringLiteral("Output folder"), loopFolderRow.second); loopForm->addRow(QStringLiteral("Loopback source"), loopSourceRow);
    loopForm->addRow(QString(), loopUntilStopped); loopForm->addRow(QStringLiteral("Duration bars"), loopDuration);
    loopForm->addRow(QStringLiteral("Silence threshold dB"), loopSilenceThreshold); loopForm->addRow(QStringLiteral("Tail silence ms"), loopTailSilence);
    loopForm->addRow(QString(), loopTrimLeading); loopForm->addRow(QString(), loopTrimTrailing);
    auto* loopBox = new QGroupBox(QStringLiteral("System Loopback Recording"), &dialog); loopBox->setLayout(loopForm);
    trackRecordingPageLayout->addWidget(loopBox); trackRecordingPageLayout->addStretch(1);
    loopDuration->setEnabled(!loopUntilStopped->isChecked());
    QObject::connect(loopUntilStopped, &QCheckBox::toggled, loopDuration, [=](bool checked) { loopDuration->setEnabled(!checked); });

    auto* generalContent = new QWidget(&dialog);
    auto* generalLayout = new QVBoxLayout(generalContent);
    auto* startupBox = new QGroupBox(QStringLiteral("When Jam2 opens"), generalContent);
    auto* startupForm = new QFormLayout(startupBox);
    auto* startupView = new QComboBox(startupBox);
    for (const auto& view : QList<QPair<QString, QString>>{
             {QStringLiteral("Performance"), QStringLiteral("performance")},
             {QStringLiteral("Chords"), QStringLiteral("chords")},
             {QStringLiteral("Drums"), QStringLiteral("drums")},
             {QStringLiteral("Lyrics"), QStringLiteral("lyrics")},
             {QStringLiteral("Metronome"), QStringLiteral("metronome")},
             {QStringLiteral("Track"), QStringLiteral("track")}}) {
        startupView->addItem(view.first, view.second);
    }
    startupView->setCurrentIndex(qMax(0, startupView->findData(preferences_.general.startupView)));
    auto* defaultBpm = makeSpin(preferences_.general.bpm, 20, 400);
    defaultBpm->setSuffix(QStringLiteral(" BPM"));
    auto* defaultMeter = new QComboBox(startupBox);
    for (const auto& meter : jam2::practice::meterCatalog()) {
        defaultMeter->addItem(meter.name, meter.id);
    }
    QString currentDefaultMeter;
    for (const auto& meter : jam2::practice::meterCatalog()) {
        if (meter.numerator == preferences_.general.meterNumerator &&
            meter.denominator == preferences_.general.meterDenominator &&
            meter.tempoPulseUnits == preferences_.general.tempoPulseUnits) {
            currentDefaultMeter = meter.id;
            break;
        }
    }
    defaultMeter->setCurrentIndex(qMax(0, defaultMeter->findData(currentDefaultMeter)));
    auto* defaultDivision = new QComboBox(startupBox);
    for (const auto& division : QList<QPair<QString, int>>{
             {QStringLiteral("Quarter"), 1}, {QStringLiteral("Eighth"), 2},
             {QStringLiteral("Triplet"), 3}, {QStringLiteral("16th"), 4},
             {QStringLiteral("6th"), 6}, {QStringLiteral("32nd"), 8}}) {
        defaultDivision->addItem(division.first, division.second);
    }
    defaultDivision->setCurrentIndex(qMax(0,
        defaultDivision->findData(preferences_.general.clickDivision)));
    auto* generateOnStartup = new QCheckBox(
        QStringLiteral("Generate an idea automatically"), startupBox);
    generateOnStartup->setChecked(preferences_.general.generateIdeaOnStartup);
    startupForm->addRow(QStringLiteral("Opening view"), startupView);
    startupForm->addRow(QStringLiteral("Default tempo"), defaultBpm);
    startupForm->addRow(QStringLiteral("Default meter"), defaultMeter);
    startupForm->addRow(QStringLiteral("Click division"), defaultDivision);
    startupForm->addRow(QString(), generateOnStartup);
    generalLayout->addWidget(startupBox);
    auto* generalNote = new QLabel(QStringLiteral(
        "These defaults initialise a new empty jam. Saved JamJars retain their own song timing and content."),
        generalContent);
    generalNote->setWordWrap(true);
    generalNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    generalLayout->addWidget(generalNote);
    generalLayout->addStretch(1);

    auto* ideaContent = new QWidget(&dialog);
    auto* ideaLayout = new QVBoxLayout(ideaContent);
    auto* generationBox = new QGroupBox(QStringLiteral("Generate Idea defaults"), ideaContent);
    auto* generationForm = new QFormLayout(generationBox);
    auto* ideaParts = new QComboBox(generationBox);
    ideaParts->addItem(QStringLiteral("Full arrangement"), 0);
    ideaParts->addItem(QStringLiteral("Chords, Bass & Melody Only"), 1);
    ideaParts->addItem(QStringLiteral("Drums Only"), 2);
    ideaParts->setCurrentIndex(qMax(0, ideaParts->findData(preferences_.ideas.parts)));
    auto* ideaKey = new QComboBox(generationBox);
    ideaKey->addItem(QStringLiteral("Random"), -1);
    const QStringList ideaKeyNames = jam2::practice::keyNames();
    for (int index = 0; index < ideaKeyNames.size(); ++index) {
        ideaKey->addItem(ideaKeyNames.at(index), index);
    }
    ideaKey->setCurrentIndex(qMax(0, ideaKey->findData(preferences_.ideas.key)));
    auto* ideaStyle = new QComboBox(generationBox);
    ideaStyle->addItem(QStringLiteral("Random"), QString());
    const QStringList ideaStyleNames = jam2::practice::chordStyleNames();
    const QStringList ideaStyleIds = jam2::practice::styleIds();
    for (int index = 0; index < ideaStyleNames.size(); ++index) {
        ideaStyle->addItem(ideaStyleNames.at(index), ideaStyleIds.value(index));
    }
    ideaStyle->addItem(QStringLiteral("Experimental - Modern Progressive Metalcore"),
        QStringLiteral("metal-experimental"));
    ideaStyle->setCurrentIndex(qMax(0, ideaStyle->findData(preferences_.ideas.styleId)));
    auto* ideaProfile = new QComboBox(generationBox);
    const auto refreshIdeaProfiles = [ideaStyle, ideaProfile](const QString& preferred = QString()) {
        ideaProfile->clear();
        ideaProfile->addItem(QStringLiteral("Random profile"), QString());
        const QString styleId = ideaStyle->currentData().toString();
        if (styleId == QStringLiteral("metal-experimental")) {
            ideaProfile->addItem(QStringLiteral("Modern Progressive Metalcore (sound test)"),
                QStringLiteral("metal_modern_progressive"));
        } else {
            const QStringList ids = jam2::practice::profileIds(styleId);
            const QStringList names = jam2::practice::profileNames(styleId);
            for (int index = 0; index < names.size(); ++index) {
                ideaProfile->addItem(names.at(index), ids.value(index));
            }
        }
        ideaProfile->setCurrentIndex(qMax(0, ideaProfile->findData(preferred)));
    };
    refreshIdeaProfiles(preferences_.ideas.profileId);
    QObject::connect(ideaStyle, &QComboBox::currentIndexChanged, &dialog,
        [refreshIdeaProfiles](int) { refreshIdeaProfiles(); });
    auto* ideaMeter = new QComboBox(generationBox);
    ideaMeter->addItem(QStringLiteral("Random compatible / use jam meter at startup"), QString());
    for (const auto& meter : jam2::practice::meterCatalog()) {
        ideaMeter->addItem(meter.name, meter.id);
    }
    ideaMeter->setCurrentIndex(qMax(0, ideaMeter->findData(preferences_.ideas.meterId)));
    auto* ideaLength = new QComboBox(generationBox);
    ideaLength->addItem(QStringLiteral("Current section / compatible default"), 0);
    for (int bars : {4, 8, 12, 16, 24, 32}) {
        ideaLength->addItem(QStringLiteral("%1 bars").arg(bars), bars);
    }
    ideaLength->setCurrentIndex(qMax(0, ideaLength->findData(preferences_.ideas.bars)));
    auto* ideaExactBpm = new QCheckBox(QStringLiteral("Use exact BPM"), generationBox);
    ideaExactBpm->setChecked(preferences_.ideas.exactBpm);
    auto* ideaBpm = makeSpin(preferences_.ideas.bpm, 20, 400);
    ideaBpm->setSuffix(QStringLiteral(" BPM"));
    ideaBpm->setEnabled(ideaExactBpm->isChecked());
    QObject::connect(ideaExactBpm, &QCheckBox::toggled, ideaBpm, &QSpinBox::setEnabled);
    auto* ideaTempoRow = new QWidget(generationBox);
    auto* ideaTempoLayout = new QHBoxLayout(ideaTempoRow);
    ideaTempoLayout->setContentsMargins(0, 0, 0, 0);
    ideaTempoLayout->addWidget(ideaExactBpm);
    ideaTempoLayout->addWidget(ideaBpm, 1);
    auto* ideaComplexity = makeSpin(preferences_.ideas.complexity, 1, 8);
    generationForm->addRow(QStringLiteral("Parts"), ideaParts);
    generationForm->addRow(QStringLiteral("Key"), ideaKey);
    generationForm->addRow(QStringLiteral("Style"), ideaStyle);
    generationForm->addRow(QStringLiteral("Profile"), ideaProfile);
    generationForm->addRow(QStringLiteral("Meter"), ideaMeter);
    generationForm->addRow(QStringLiteral("Length"), ideaLength);
    generationForm->addRow(QStringLiteral("Tempo"), ideaTempoRow);
    generationForm->addRow(QStringLiteral("Complexity"), ideaComplexity);
    ideaLayout->addWidget(generationBox);

    auto* wavBox = new QGroupBox(QStringLiteral("Reference WAV defaults"), ideaContent);
    auto* wavForm = new QFormLayout(wavBox);
    auto* startupWavs = new QCheckBox(
        QStringLiteral("Render reference WAVs for the startup idea"), wavBox);
    startupWavs->setChecked(preferences_.ideas.renderWavsOnStartup);
    startupWavs->setEnabled(generateOnStartup->isChecked());
    startupWavs->setToolTip(QStringLiteral(
        "Requires Generate an idea automatically in General"));
    QObject::connect(generateOnStartup, &QCheckBox::toggled,
        startupWavs, &QCheckBox::setEnabled);
    auto* renderChords = new QCheckBox(QStringLiteral("Chords"), wavBox);
    auto* renderDrums = new QCheckBox(QStringLiteral("Drums"), wavBox);
    auto* renderMelody = new QCheckBox(QStringLiteral("Melody"), wavBox);
    auto* renderBass = new QCheckBox(QStringLiteral("Bass"), wavBox);
    auto* renderSupport = new QCheckBox(QStringLiteral("Supporting line"), wavBox);
    renderChords->setChecked(preferences_.ideas.renderChords);
    renderDrums->setChecked(preferences_.ideas.renderDrums);
    renderMelody->setChecked(preferences_.ideas.renderMelody);
    renderBass->setChecked(preferences_.ideas.renderBass);
    renderSupport->setChecked(preferences_.ideas.renderSupport);
    auto* renderPartsRow = new QWidget(wavBox);
    auto* renderPartsLayout = new QHBoxLayout(renderPartsRow);
    renderPartsLayout->setContentsMargins(0, 0, 0, 0);
    for (QCheckBox* check : {renderChords, renderDrums, renderMelody, renderBass, renderSupport}) {
        renderPartsLayout->addWidget(check);
    }
    auto* chordVoicing = new QComboBox(wavBox);
    chordVoicing->addItems({QStringLiteral("Style default"), QStringLiteral("Close"),
        QStringLiteral("Spread"), QStringLiteral("Voice-led")});
    chordVoicing->setCurrentIndex(qBound(0, preferences_.ideas.chordVoicing, 3));
    auto* drumKit = new QComboBox(wavBox);
    drumKit->addItems({QStringLiteral("Style default"), QStringLiteral("Acoustic"),
        QStringLiteral("Electronic")});
    drumKit->setCurrentIndex(qBound(0, preferences_.ideas.drumKit, 2));
    wavForm->addRow(QString(), startupWavs);
    wavForm->addRow(QStringLiteral("Render parts"), renderPartsRow);
    wavForm->addRow(QStringLiteral("Chord voicing"), chordVoicing);
    wavForm->addRow(QStringLiteral("Drum kit"), drumKit);
    ideaLayout->addWidget(wavBox);

    auto* grooveBox = new QGroupBox(QStringLiteral("Groove Library defaults"), ideaContent);
    auto* grooveForm = new QFormLayout(grooveBox);
    auto* grooveTiming = new QComboBox(grooveBox);
    grooveTiming->addItem(QStringLiteral("Use groove timing"), true);
    grooveTiming->addItem(QStringLiteral("Keep section timing"), false);
    grooveTiming->setCurrentIndex(qMax(0,
        grooveTiming->findData(preferences_.ideas.grooveUseIdeaTiming)));
    auto* grooveLength = new QComboBox(grooveBox);
    grooveLength->addItem(QStringLiteral("Current section length"), 0);
    for (int bars : {8, 12, 16, 24, 32}) {
        grooveLength->addItem(QStringLiteral("%1 bars").arg(bars), bars);
    }
    grooveLength->setCurrentIndex(qMax(0,
        grooveLength->findData(preferences_.ideas.grooveBars)));
    grooveForm->addRow(QStringLiteral("Timing"), grooveTiming);
    grooveForm->addRow(QStringLiteral("Groove length"), grooveLength);
    ideaLayout->addWidget(grooveBox);
    ideaLayout->addStretch(1);

    auto* levelsContent = new QWidget(&dialog);
    auto* levelsForm = new QFormLayout(levelsContent);
    auto makeDbSpin = [&makeSpin](int value) {
        QSpinBox* spin = makeSpin(value, -60, 12);
        spin->setSuffix(QStringLiteral(" dB"));
        return spin;
    };
    auto* defaultSend = makeDbSpin(preferences_.levels.sendDb);
    auto* defaultMonitorEnabled = new QCheckBox(QStringLiteral("Monitor input by default"), levelsContent);
    defaultMonitorEnabled->setChecked(preferences_.levels.monitorInput);
    auto* defaultMonitor = makeDbSpin(preferences_.levels.monitorDb);
    auto* defaultMetronomeLevel = makeDbSpin(preferences_.levels.metronomeDb);
    auto* defaultMaster = makeDbSpin(preferences_.levels.masterDb);
    auto* defaultRemote = makeDbSpin(preferences_.levels.remotePeerDb);
    auto* defaultBacking = makeDbSpin(preferences_.levels.backingTrackDb);
    levelsForm->addRow(QStringLiteral("Local send"), defaultSend);
    levelsForm->addRow(QString(), defaultMonitorEnabled);
    levelsForm->addRow(QStringLiteral("Input monitor"), defaultMonitor);
    levelsForm->addRow(QStringLiteral("Metronome"), defaultMetronomeLevel);
    levelsForm->addRow(QStringLiteral("Master output"), defaultMaster);
    levelsForm->addRow(QStringLiteral("New remote peers"), defaultRemote);
    levelsForm->addRow(QStringLiteral("New backing tracks"), defaultBacking);

    auto* metronomeContent = new QWidget(&dialog);
    auto* metronomeLayout = new QVBoxLayout(metronomeContent);
    auto* metronomeGeneralBox = new QGroupBox(
        QStringLiteral("General metronome"), metronomeContent);
    auto* metronomeGeneralForm = new QFormLayout(metronomeGeneralBox);
    auto* defaultClickSound = new QComboBox(metronomeGeneralBox);
    defaultClickSound->addItems({QStringLiteral("Classic"), QStringLiteral("Woodblock"),
        QStringLiteral("Rim Click"), QStringLiteral("Digital Tick")});
    defaultClickSound->setCurrentIndex(qBound(0, preferences_.metronome.sound, 3));
    auto* defaultMetronomeMode = new QComboBox(metronomeGeneralBox);
    defaultMetronomeMode->addItem(
        QStringLiteral("Shared Grid"), QStringLiteral("shared-grid"));
    defaultMetronomeMode->addItem(
        QStringLiteral("Leader Audio"), QStringLiteral("leader-audio"));
    defaultMetronomeMode->addItem(
        QStringLiteral("Listener Compensated"), QStringLiteral("listener-compensated"));
    defaultMetronomeMode->setCurrentIndex(qMax(0,
        defaultMetronomeMode->findData(preferences_.metronome.mode)));
    metronomeGeneralForm->addRow(QStringLiteral("Click sound"), defaultClickSound);
    metronomeGeneralForm->addRow(QStringLiteral("Sync mode"), defaultMetronomeMode);
    metronomeLayout->addWidget(metronomeGeneralBox);

    auto* listenerTimingBox = new QGroupBox(
        QStringLiteral("Listener-compensated timing"), metronomeContent);
    auto* listenerTimingForm = new QFormLayout(listenerTimingBox);
    auto* compensationMax = makeDoubleSpin(preferences_.metronome.compensationMaxMs, 0, 1000, 1);
    auto* compensationSmoothing = makeDoubleSpin(preferences_.metronome.compensationSmoothingMs, 0, 10000, 1);
    auto* compensationDeadband = makeDoubleSpin(preferences_.metronome.compensationDeadbandMs, 0, 1000, 1);
    auto* compensationSlew = makeDoubleSpin(preferences_.metronome.compensationSlewMsPerSecond, 0, 10000, 1);
    compensationMax->setSuffix(QStringLiteral(" ms"));
    compensationSmoothing->setSuffix(QStringLiteral(" ms"));
    compensationDeadband->setSuffix(QStringLiteral(" ms"));
    compensationSlew->setSuffix(QStringLiteral(" ms/s"));
    auto* listenerTimingNote = new QLabel(
        QStringLiteral(
            "These saved defaults are used only when the sync mode is Listener Compensated."),
        listenerTimingBox);
    listenerTimingNote->setWordWrap(true);
    listenerTimingNote->setStyleSheet(QStringLiteral("color:#9eaaa8;"));
    listenerTimingForm->addRow(QString(), listenerTimingNote);
    listenerTimingForm->addRow(QStringLiteral("Maximum compensation"), compensationMax);
    listenerTimingForm->addRow(QStringLiteral("Compensation smoothing"), compensationSmoothing);
    listenerTimingForm->addRow(QStringLiteral("Compensation deadband"), compensationDeadband);
    listenerTimingForm->addRow(QStringLiteral("Compensation slew"), compensationSlew);
    metronomeLayout->addWidget(listenerTimingBox);
    metronomeLayout->addStretch(1);

    auto* viewsContent = new QWidget(&dialog);
    auto* viewsLayout = new QVBoxLayout(viewsContent);
    auto* performanceDefaultsBox = new QGroupBox(
        QStringLiteral("Performance view"), viewsContent);
    auto* performanceDefaultsForm = new QFormLayout(performanceDefaultsBox);
    auto* performanceChordPreview = new QCheckBox(
        QStringLiteral("Show chord preview"), performanceDefaultsBox);
    auto* performanceBeatPreview = new QCheckBox(
        QStringLiteral("Show beat preview"), performanceDefaultsBox);
    performanceChordPreview->setChecked(preferences_.views.performanceChordPreview);
    performanceBeatPreview->setChecked(preferences_.views.performanceBeatPreview);
    auto* performancePreviewNote = new QLabel(
        QStringLiteral(
            "Hide either preview to leave more open visual space when you only need the jam audio."),
        performanceDefaultsBox);
    performancePreviewNote->setWordWrap(true);
    performancePreviewNote->setStyleSheet(QStringLiteral("color:#9eaaa8;"));
    performanceDefaultsForm->addRow(QString(), performanceChordPreview);
    performanceDefaultsForm->addRow(QString(), performanceBeatPreview);
    performanceDefaultsForm->addRow(QString(), performancePreviewNote);
    viewsLayout->addWidget(performanceDefaultsBox);
    auto* gridDefaultsBox = new QGroupBox(QStringLiteral("Chord and Drum views"), viewsContent);
    auto* gridDefaultsForm = new QFormLayout(gridDefaultsBox);
    auto* chordFollow = new QCheckBox(QStringLiteral("Focus current bar in Chords"), gridDefaultsBox);
    auto* drumFollow = new QCheckBox(QStringLiteral("Focus current bar in Drums"), gridDefaultsBox);
    chordFollow->setChecked(preferences_.views.chordFocusCurrentBar);
    drumFollow->setChecked(preferences_.views.drumFocusCurrentBar);
    auto* guitarStrings = new QComboBox(gridDefaultsBox);
    for (int strings : {6, 7, 8}) guitarStrings->addItem(QString::number(strings), strings);
    guitarStrings->setCurrentIndex(qMax(0, guitarStrings->findData(preferences_.views.guitarStrings)));
    auto* guitarTuning = new QComboBox(gridDefaultsBox);
    guitarTuning->addItem(QStringLiteral("Standard"), false);
    guitarTuning->addItem(QStringLiteral("Dropped"), true);
    guitarTuning->setCurrentIndex(qMax(0,
        guitarTuning->findData(preferences_.views.guitarDropTuning)));
    gridDefaultsForm->addRow(QString(), chordFollow);
    gridDefaultsForm->addRow(QString(), drumFollow);
    gridDefaultsForm->addRow(QStringLiteral("Guitar strings"), guitarStrings);
    gridDefaultsForm->addRow(QStringLiteral("Guitar tuning"), guitarTuning);
    viewsLayout->addWidget(gridDefaultsBox);
    auto* trackDefaultsBox = new QGroupBox(QStringLiteral("Track view"), viewsContent);
    auto* trackDefaultsForm = new QFormLayout(trackDefaultsBox);
    auto* defaultGridLock = new QCheckBox(QStringLiteral("Lock transport actions to the grid"), trackDefaultsBox);
    auto* defaultTrackLoop = new QCheckBox(QStringLiteral("Loop whole track"), trackDefaultsBox);
    defaultGridLock->setChecked(preferences_.views.trackGridLock);
    defaultTrackLoop->setChecked(preferences_.views.trackLoop);
    auto* defaultTrackSpeed = makeDoubleSpin(preferences_.views.trackSpeed, 0.1, 2.0, 2);
    auto* defaultTrackPitch = makeSpin(preferences_.views.trackPitch, -12, 12);
    auto* defaultFocusEnabled = new QCheckBox(QStringLiteral("Enable focus frequency"), trackDefaultsBox);
    defaultFocusEnabled->setChecked(preferences_.views.focusFrequencyEnabled);
    auto* defaultFocusPreset = new QComboBox(trackDefaultsBox);
    for (const QString& preset : {QStringLiteral("custom"), QStringLiteral("bass"),
             QStringLiteral("guitar"), QStringLiteral("vocals"), QStringLiteral("drums")}) {
        defaultFocusPreset->addItem(preset.front().toUpper() + preset.mid(1), preset);
    }
    defaultFocusPreset->setCurrentIndex(qMax(0,
        defaultFocusPreset->findData(preferences_.views.focusPreset)));
    auto* defaultFocusFrequency = makeSpin(preferences_.views.focusFrequencyHz, 40, 8000);
    defaultFocusFrequency->setSuffix(QStringLiteral(" Hz"));
    trackDefaultsForm->addRow(QString(), defaultGridLock);
    trackDefaultsForm->addRow(QString(), defaultTrackLoop);
    trackDefaultsForm->addRow(QStringLiteral("Playback speed"), defaultTrackSpeed);
    trackDefaultsForm->addRow(QStringLiteral("Pitch semitones"), defaultTrackPitch);
    trackDefaultsForm->addRow(QString(), defaultFocusEnabled);
    trackDefaultsForm->addRow(QStringLiteral("Focus preset"), defaultFocusPreset);
    trackDefaultsForm->addRow(QStringLiteral("Focus frequency"), defaultFocusFrequency);
    viewsLayout->addWidget(trackDefaultsBox);
    viewsLayout->addStretch(1);

    auto* syncContent = new QWidget(&dialog);
    auto* syncForm = new QFormLayout(syncContent);
    auto* syncTrackLanes = new QCheckBox(QStringLiteral("Sync Track Lanes"), syncContent);
    auto* syncWavs = new QCheckBox(QStringLiteral("Sync WAVs Automatically"), syncContent);
    auto* syncPlayback = new QCheckBox(QStringLiteral("Sync Global Playback"), syncContent);
    auto* syncMetronome = new QCheckBox(QStringLiteral("Sync Metronome State"), syncContent);
    auto* syncRecordings = new QCheckBox(QStringLiteral("Sync Recordings"), syncContent);
    syncTrackLanes->setChecked(preferences_.sync.trackLanes);
    syncWavs->setChecked(preferences_.sync.autoShareWavs);
    syncPlayback->setChecked(preferences_.sync.globalPlayback);
    syncMetronome->setChecked(preferences_.sync.metronomeState);
    syncRecordings->setChecked(preferences_.sync.recordings);
    auto* syncIdeas = new QComboBox(syncContent);
    syncIdeas->addItem(QStringLiteral("Disabled"), 0);
    syncIdeas->addItem(QStringLiteral("Whole Idea"), 1);
    syncIdeas->addItem(QStringLiteral("Chords Only"), 2);
    syncIdeas->addItem(QStringLiteral("Beats Only"), 3);
    syncIdeas->setCurrentIndex(qMax(0, syncIdeas->findData(preferences_.sync.generatedIdeas)));
    const auto updateSyncDefaults = [=] {
        syncWavs->setEnabled(syncTrackLanes->isChecked());
        const bool recordingDependencies = syncTrackLanes->isChecked() &&
            syncPlayback->isChecked();
        syncRecordings->setEnabled(recordingDependencies);
        if (!recordingDependencies) syncRecordings->setChecked(false);
    };
    QObject::connect(syncTrackLanes, &QCheckBox::toggled, &dialog,
        [updateSyncDefaults](bool) { updateSyncDefaults(); });
    QObject::connect(syncPlayback, &QCheckBox::toggled, &dialog,
        [updateSyncDefaults](bool) { updateSyncDefaults(); });
    updateSyncDefaults();
    syncForm->addRow(syncTrackLanes);
    syncForm->addRow(syncWavs);
    syncForm->addRow(QStringLiteral("Generated ideas"), syncIdeas);
    syncForm->addRow(syncPlayback);
    syncForm->addRow(syncMetronome);
    syncForm->addRow(syncRecordings);
    auto* syncNote = new QLabel(QStringLiteral(
        "These defaults initialise new local, created, and joined workflows. The jam creator can still apply a different policy to an active jam."),
        syncContent);
    syncNote->setWordWrap(true);
    syncNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    syncForm->addRow(syncNote);

    auto* keybindContent = new QWidget(&dialog);
    auto* keybindLayout = new QVBoxLayout(keybindContent);
    keybindLayout->setSpacing(12);
    auto* keybindNote = new QLabel(
        QStringLiteral(
            "Jam2 shortcuts are fixed and work from the main workflow views. "
            "They pause while you are typing, editing a numeric value, choosing from a list, "
            "or using a popup dialog."),
        keybindContent);
    keybindNote->setWordWrap(true);
    keybindNote->setStyleSheet(QStringLiteral("color:#9eaaa9; padding:2px 0 6px 0;"));
    keybindLayout->addWidget(keybindNote);

    const auto addKeybindGroup = [keybindContent, keybindLayout](
        const QString& title,
        const QList<QPair<QString, QString>>& bindings) {
        auto* group = new QGroupBox(title, keybindContent);
        auto* form = new QFormLayout(group);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(9);
        for (const auto& binding : bindings) {
            auto* shortcut = new QLabel(binding.first, group);
            shortcut->setMinimumWidth(112);
            shortcut->setTextInteractionFlags(Qt::TextSelectableByMouse);
            shortcut->setStyleSheet(QStringLiteral(
                "color:#d9ad58; font-weight:600; padding:2px 8px; "
                "background:#11191b; border:1px solid #344245; border-radius:4px;"));
            auto* action = new QLabel(binding.second, group);
            action->setWordWrap(true);
            action->setStyleSheet(QStringLiteral("color:#d8dfdd;"));
            form->addRow(shortcut, action);
        }
        keybindLayout->addWidget(group);
    };

    addKeybindGroup(QStringLiteral("Views"), {
        {QStringLiteral("1"), QStringLiteral("Performance")},
        {QStringLiteral("2"), QStringLiteral("Chords")},
        {QStringLiteral("3"), QStringLiteral("Drums")},
        {QStringLiteral("4"), QStringLiteral("Lyrics")},
        {QStringLiteral("5"), QStringLiteral("Metronome")},
        {QStringLiteral("6"), QStringLiteral("Track")},
        {QStringLiteral("Tab"), QStringLiteral("No action in the main views")},
        {QStringLiteral("Shift+Tab"), QStringLiteral("Cycle to the next view in the order above")},
        {QStringLiteral("Home"), QStringLiteral("Return to Performance")},
    });
    addKeybindGroup(QStringLiteral("Playback and sections"), {
        {QStringLiteral("Space"), QStringLiteral("Start or stop global playback")},
        {QStringLiteral("Shift+Space"), QStringLiteral("Queue the next section while playback is running")},
        {QStringLiteral("M"), QStringLiteral("Toggle the metronome")},
        {QStringLiteral("F"), QStringLiteral("Toggle Focus current bar in Chords or Drums")},
    });
    addKeybindGroup(QStringLiteral("Track recording"), {
        {QStringLiteral("Ctrl+A"), QStringLiteral("Arm the selected lane in Track view")},
        {QStringLiteral("Ctrl+R"), QStringLiteral("Start or stop recording the selected lane in Track view")},
    });
    addKeybindGroup(QStringLiteral("Dialogs and editing"), {
        {QStringLiteral("Enter"), QStringLiteral("Confirm the active dialog; in Lyrics, move to the next bar")},
        {QStringLiteral("Shift+Enter"), QStringLiteral("Insert a new line in Lyrics")},
        {QStringLiteral("Ctrl+V"), QStringLiteral("Paste multiple lyric lines into consecutive bars")},
        {QStringLiteral("Escape"), QStringLiteral("Cancel or close the active dialog, rename, or expanded tuner")},
    });
    keybindLayout->addStretch(1);

    if (networkActive) {
        localBox->setEnabled(false);
        splitNetworkAudio->setEnabled(false);
        networkAudio.box->setEnabled(false);
        createJamAudio.box->setEnabled(false);
        joinJamAudio.box->setEnabled(false);
    }

    auto* notice = new QLabel(
        networkActive
            ? QStringLiteral("Leave the active jam before changing audio hardware settings.")
            : QStringLiteral(
                "Test Device checks the selection without changing playback. "
                "Apply Audio starts or restarts local audio and saves that selection."),
        &dialog);
    notice->setWordWrap(true);

    audioLayout->addWidget(notice);
    auto* tabs = new QWidget(&dialog);
    auto* tabLayout = new QHBoxLayout(tabs);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(12);
    auto* settingsNavigation = new QListWidget(tabs);
    settingsNavigation->setObjectName(QStringLiteral("SettingsNavigation"));
    settingsNavigation->setFixedWidth(172);
    settingsNavigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsNavigation->setSpacing(2);
    settingsNavigation->setStyleSheet(QStringLiteral(
        "QListWidget#SettingsNavigation { background:#101718; border:1px solid #344245; "
        "border-radius:5px; padding:6px; outline:none; }"
        "QListWidget#SettingsNavigation::item { color:#bdc8c6; min-height:22px; "
        "padding:7px 10px; border:1px solid transparent; border-radius:4px; }"
        "QListWidget#SettingsNavigation::item:hover { color:#eef2ef; background:#182224; }"
        "QListWidget#SettingsNavigation::item:selected { color:#f2c66d; background:#211b12; "
        "border-color:#8a6835; font-weight:600; }"));
    auto* settingsPages = new QStackedWidget(tabs);
    const auto addSettingsPage = [=](const QString& title, QWidget* page) {
        settingsNavigation->addItem(title);
        settingsPages->addWidget(page);
    };
    addSettingsPage(QStringLiteral("Audio"), makeScrollTab(audioContent));
    addSettingsPage(QStringLiteral("Create Connection"), makeScrollTab(connectionContent));
    addSettingsPage(QStringLiteral("Create Defaults"), makeScrollTab(createContent));
    addSettingsPage(QStringLiteral("Join Defaults"), makeScrollTab(joinContent));
    addSettingsPage(QStringLiteral("Jam Sync"), makeScrollTab(syncContent));
    addSettingsPage(QStringLiteral("Ideas & WAVs"), makeScrollTab(ideaContent));
    addSettingsPage(QStringLiteral("Levels"), makeScrollTab(levelsContent));
    addSettingsPage(QStringLiteral("Metronome"), makeScrollTab(metronomeContent));
    addSettingsPage(QStringLiteral("Startup"), makeScrollTab(generalContent));
    addSettingsPage(QStringLiteral("Views & Tracks"), makeScrollTab(viewsContent));
    addSettingsPage(QStringLiteral("Logs"), makeScrollTab(logContent));
    addSettingsPage(QStringLiteral("Recording"), makeScrollTab(recordingContent));
    addSettingsPage(QStringLiteral("Keybinds"), makeScrollTab(keybindContent));
    QObject::connect(settingsNavigation, &QListWidget::currentRowChanged,
        settingsPages, &QStackedWidget::setCurrentIndex);
    settingsNavigation->setCurrentRow(0);
    tabLayout->addWidget(settingsNavigation);
    tabLayout->addWidget(settingsPages, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(localTest, &QPushButton::clicked, this, [this, localDevice, localTest, &dialog] {
        testDeviceSelection(localDevice, localTest, &dialog);
    });
    QObject::connect(localApply, &QPushButton::clicked, this,
        [this, &dialog, &localInitial, localDevice, localSampleRate,
         localBufferSize, localInput, localOutput, localApply, networkActive] {
            if (networkActive) return;
            if (localDevice->currentData().toString().isEmpty()) {
                QMessageBox::warning(
                    &dialog,
                    QStringLiteral("Local Audio"),
                    QStringLiteral("Select a local audio device first."));
                return;
            }

            AudioDevicePreference desired = preferences_.localAudio;
            desired.sampleRate = localSampleRate->currentData().toInt();
            desired.bufferSize = localBufferSize->currentData().toInt();
            desired.inputChannels = localInput->text().trimmed();
            desired.outputChannels = localOutput->text().trimmed();
            storeSelectedDevice(desired, localDevice, availableDevices_);

            const bool wasLocalActive = jam2_.isRunning() && !jam2_.isNetworkRunning();
            QString permissionError;
            if (!wasLocalActive && !jam2EnsureMicrophonePermission(&permissionError)) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("Jam2 Microphone Access"), permissionError);
                return;
            }

            const QString previousDevice = selectedDeviceId();
            const QString previousInput = inputChannelsEdit_->text();
            const QString previousOutput = outputChannelsEdit_->text();
            const int previousRate = sampleRateSpin_->value();
            const int previousBuffer = bufferSizeSpin_->value();
            std::optional<Jam2RuntimeOptions> previousOptions;
            try {
                if (wasLocalActive) previousOptions = runtimeOptions();
                const int deviceIndex = deviceBox_->findData(localDevice->currentData());
                if (deviceIndex < 0) {
                    throw std::runtime_error("the selected local audio device is unavailable");
                }
                deviceBox_->setCurrentIndex(deviceIndex);
                inputChannelsEdit_->setText(desired.inputChannels);
                outputChannelsEdit_->setText(desired.outputChannels);
                sampleRateSpin_->setValue(desired.sampleRate);
                bufferSizeSpin_->setValue(desired.bufferSize);
                if (wasLocalActive) {
                    if (!sessionController_.startLocal(runtimeOptions())) {
                        throw std::runtime_error("the new local audio configuration did not start");
                    }
                    showLocalSessionHeaderStatus();
                } else {
                    launchLocalRuntime(runtimeOptions());
                }
            } catch (const std::exception& error) {
                deviceBox_->setCurrentIndex(qMax(0, deviceBox_->findData(previousDevice)));
                inputChannelsEdit_->setText(previousInput);
                outputChannelsEdit_->setText(previousOutput);
                sampleRateSpin_->setValue(previousRate);
                bufferSizeSpin_->setValue(previousBuffer);
                const bool restored = !wasLocalActive ||
                    (previousOptions && sessionController_.startLocal(*previousOptions));
                if (restored && wasLocalActive) showLocalSessionHeaderStatus();
                else if (!wasLocalActive) showAudioOffSessionHeaderStatus();
                QMessageBox::warning(
                    &dialog,
                    QStringLiteral("Local Audio Not Applied"),
                    QStringLiteral("%1\n\nPrevious local audio settings %2.")
                        .arg(QString::fromUtf8(error.what()),
                             restored ? QStringLiteral("were restored")
                                      : QStringLiteral("could not be restored")));
                return;
            }

            preferences_.localAudio = desired;
            localInitial = desired;
            UserPreferencesStore::save(preferences_);
            localApply->setText(QStringLiteral("Applied"));
            QTimer::singleShot(1400, localApply, [localApply] {
                localApply->setText(QStringLiteral("Apply Audio"));
            });
            appendLog(QStringLiteral("local audio settings applied"));
        });
    auto connectDeviceTest = [this, &dialog](const NetworkAudioEditors& editors) {
        QObject::connect(editors.test, &QPushButton::clicked, this, [this, editors, &dialog] {
            testDeviceSelection(editors.device, editors.test, &dialog);
        });
    };
    connectDeviceTest(networkAudio);
    connectDeviceTest(createJamAudio);
    connectDeviceTest(joinJamAudio);
    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(tabs, 1);
    outer->addWidget(buttons);
    for (QFormLayout* form : dialog.findChildren<QFormLayout*>()) {
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    for (QWidget* editor : dialog.findChildren<QWidget*>()) {
        if (qobject_cast<QAbstractSpinBox*>(editor) ||
            qobject_cast<QLineEdit*>(editor) ||
            qobject_cast<QComboBox*>(editor)) {
            editor->setAttribute(Qt::WA_MacShowFocusRect, false);
        }
    }

    if (dialog.exec() != QDialog::Accepted) return;

    UserPreferences updated = preferences_;
    updated.localAudio.sampleRate = localSampleRate->currentData().toInt();
    updated.localAudio.bufferSize = localBufferSize->currentData().toInt();
    updated.localAudio.inputChannels = localInput->text().trimmed();
    updated.localAudio.outputChannels = localOutput->text().trimmed();
    storeSelectedDevice(updated.localAudio, localDevice, availableDevices_);
    updated.splitNetworkAudioByRole = splitNetworkAudio->isChecked();
    updated.networkAudio = audioFromEditors(updated.networkAudio, networkAudio);
    if (updated.splitNetworkAudioByRole) {
        updated.createJamAudio = audioFromEditors(updated.createJamAudio, createJamAudio);
        updated.joinJamAudio = audioFromEditors(updated.joinJamAudio, joinJamAudio);
    } else {
        updated.createJamAudio = updated.networkAudio;
        updated.joinJamAudio = updated.networkAudio;
    }
    updated.create.bindHost = createBind->text().trimmed(); updated.create.port = createPort->value();
    updated.create.noStun = createManualEndpoint->isChecked(); updated.create.publicHost = createPublicHost->text().trimmed();
    updated.create.stunServer = createStun->text().trimmed(); updated.create.stunTimeoutMs = createStunTimeout->value();
    updated.create.stunRetries = createStunRetries->value(); updated.create.maxPeers = createMaxPeers->value();
    updated.create.socketSendBuffer = createSocketSend->value(); updated.create.socketRecvBuffer = createSocketReceive->value();
    updated.create.tuning.profile = createTuning.profile->currentData().toString();
    updated.create.sampleRate = createRate->currentData().toInt();
    updated.create.audioFormat = createQuality->currentData().toString();
    updated.join.bindHost = joinBind->text().trimmed(); updated.join.port = joinPort->value();
    auto storeTuning = [](LocalTuningPreference& p, const TuningEditors& e, bool creator) {
        p.profile = e.profile->currentData().toString(); p.bufferSize = e.buffer->currentData().toInt();
        if (creator) p.frameSize = e.frame->value();
        p.prefillFrames = e.prefill->value(); p.playbackMaxFrames = e.playbackMax->value();
        p.captureRingFrames = e.captureRing->value(); p.playbackRingFrames = e.playbackRing->value();
        p.driftCorrection = e.drift->isChecked(); p.driftSmoothing = e.driftSmoothing->value();
        p.driftDeadbandPpm = e.driftDeadband->value(); p.driftMaxCorrectionPpm = e.driftMax->value();
        p.sampleTimePlayout = e.sampleTime->isChecked(); p.playoutDelayFrames = e.playout->value();
        p.jitterBufferFrames = e.jitter->value(); p.jitterBufferMaxFrames = e.jitterMax->value();
        p.adaptiveCushion = e.adaptive->isChecked(); p.adaptiveTargetFrames = e.adaptiveTarget->value();
        p.adaptiveMinFrames = e.adaptiveMin->value(); p.adaptiveMaxFrames = e.adaptiveMax->value();
        p.adaptiveReleasePpm = e.adaptiveRelease->value(); p.adaptiveRatioRampMs = e.adaptiveRamp->value();
    };
    auto storeRuntime = [](RuntimePreference& p, const RuntimeEditors& e) {
        p.diagnostics = e.diagnostics->isChecked(); p.diagnosticsWarmupMs = e.warmup->value();
        p.osPriority = e.priority->currentData().toString(); p.waitMs = e.wait->value();
        p.streamMs = e.stream->value(); p.streamLingerMs = e.linger->value();
    };
    storeTuning(updated.create.tuning, createTuning, true); storeRuntime(updated.create.runtime, createRuntime);
    storeTuning(updated.join.tuning, joinTuning, false); storeRuntime(updated.join.runtime, joinRuntime);
    updated.logging.folder = logFolder->text().trimmed();
    if (updated.logging.folder.isEmpty()) {
        updated.logging.folder = appReleaseFolderPath(QStringLiteral("logs"));
    }
    updated.create.runtime.logStatsFolder = updated.logging.folder;
    updated.join.runtime.logStatsFolder = updated.logging.folder;
    updated.recording.preferredMode = preferredMode->currentData().toString();
    updated.recording.jam.promptForName = jamPromptName->isChecked();
    updated.recording.jam.completionAction = jamCompletion->currentData().toString();
    updated.recording.jam.importMix = jamImportMix->isChecked();
    updated.recording.jam.importMyInput = jamImportMyInput->isChecked();
    updated.recording.jam.importTheirInput = jamImportTheirInput->isChecked();
    updated.recording.jam.importInputsMix = jamImportInputsMix->isChecked();
    updated.recording.jam.importMetronome = jamImportMetronome->isChecked();
    updated.recording.jamMixTrack.includeBackingTrack =
        jamMixIncludeBacking->isChecked();
    updated.recording.jamMixTrack.includeMetronome =
        jamMixIncludeMetronome->isChecked();
    updated.recording.input.outputFolder = inputFolderRow.first->text().trimmed();
    if (updated.recording.input.outputFolder.isEmpty()) {
        updated.recording.input.outputFolder = appReleaseFolderPath(QStringLiteral("captures"));
    }
    updated.recording.input.recordUntilStopped = inputUntilStopped->isChecked();
    updated.recording.input.durationBars = inputDuration->value();
    updated.recording.input.countIn = inputCountIn->isChecked(); updated.recording.input.countInBars = inputCountBars->value();
    updated.recording.input.countInMetronome = inputCountMetro->isChecked(); updated.recording.input.keepMetronome = inputKeepMetro->isChecked();
    updated.recording.input.latencyAdjustmentFrames = inputLatency->value();
    updated.recording.loopback.outputFolder = loopFolderRow.first->text().trimmed();
    if (updated.recording.loopback.outputFolder.isEmpty()) {
        updated.recording.loopback.outputFolder = appReleaseFolderPath(QStringLiteral("captures"));
    }
    updated.recording.loopback.sourceId = loopSource->currentData().toString().isEmpty()
        ? loopSource->currentText().trimmed() : loopSource->currentData().toString();
    updated.recording.loopback.sourceName = loopSource->currentText().trimmed();
    updated.recording.loopback.recordUntilStopped = loopUntilStopped->isChecked();
    updated.recording.loopback.durationBars = loopDuration->value();
    updated.recording.loopback.silenceThresholdDb = loopSilenceThreshold->value();
    updated.recording.loopback.tailSilenceMs = loopTailSilence->value(); updated.recording.loopback.trimLeading = loopTrimLeading->isChecked();
    updated.recording.loopback.trimTrailing = loopTrimTrailing->isChecked();

    updated.general.startupView = startupView->currentData().toString();
    updated.general.bpm = defaultBpm->value();
    if (const auto* meter = jam2::practice::findMeter(
            defaultMeter->currentData().toString())) {
        updated.general.meterNumerator = meter->numerator;
        updated.general.meterDenominator = meter->denominator;
        updated.general.tempoPulseUnits = meter->tempoPulseUnits;
    }
    updated.general.clickDivision = defaultDivision->currentData().toInt();
    updated.general.generateIdeaOnStartup = generateOnStartup->isChecked();

    updated.ideas.parts = ideaParts->currentData().toInt();
    updated.ideas.key = ideaKey->currentData().toInt();
    updated.ideas.styleId = ideaStyle->currentData().toString();
    updated.ideas.profileId = ideaProfile->currentData().toString();
    updated.ideas.meterId = ideaMeter->currentData().toString();
    updated.ideas.bars = ideaLength->currentData().toInt();
    updated.ideas.exactBpm = ideaExactBpm->isChecked();
    updated.ideas.bpm = ideaBpm->value();
    updated.ideas.complexity = ideaComplexity->value();
    updated.ideas.renderWavsOnStartup = startupWavs->isChecked();
    updated.ideas.renderChords = renderChords->isChecked();
    updated.ideas.renderDrums = renderDrums->isChecked();
    updated.ideas.renderMelody = renderMelody->isChecked();
    updated.ideas.renderBass = renderBass->isChecked();
    updated.ideas.renderSupport = renderSupport->isChecked();
    updated.ideas.chordVoicing = chordVoicing->currentIndex();
    updated.ideas.drumKit = drumKit->currentIndex();
    updated.ideas.grooveUseIdeaTiming = grooveTiming->currentData().toBool();
    updated.ideas.grooveBars = grooveLength->currentData().toInt();

    updated.levels.sendDb = defaultSend->value();
    updated.levels.monitorInput = defaultMonitorEnabled->isChecked();
    updated.levels.monitorDb = defaultMonitor->value();
    updated.levels.metronomeDb = defaultMetronomeLevel->value();
    updated.levels.masterDb = defaultMaster->value();
    updated.levels.remotePeerDb = defaultRemote->value();
    updated.levels.backingTrackDb = defaultBacking->value();

    updated.metronome.sound = defaultClickSound->currentIndex();
    updated.metronome.mode = defaultMetronomeMode->currentData().toString();
    updated.metronome.compensationMaxMs = compensationMax->value();
    updated.metronome.compensationSmoothingMs = compensationSmoothing->value();
    updated.metronome.compensationDeadbandMs = compensationDeadband->value();
    updated.metronome.compensationSlewMsPerSecond = compensationSlew->value();

    updated.views.performanceChordPreview = performanceChordPreview->isChecked();
    updated.views.performanceBeatPreview = performanceBeatPreview->isChecked();
    updated.views.chordFocusCurrentBar = chordFollow->isChecked();
    updated.views.drumFocusCurrentBar = drumFollow->isChecked();
    updated.views.guitarStrings = guitarStrings->currentData().toInt();
    updated.views.guitarDropTuning = guitarTuning->currentData().toBool();
    updated.views.trackGridLock = defaultGridLock->isChecked();
    updated.views.trackLoop = defaultTrackLoop->isChecked();
    updated.views.trackSpeed = defaultTrackSpeed->value();
    updated.views.trackPitch = defaultTrackPitch->value();
    updated.views.focusFrequencyEnabled = defaultFocusEnabled->isChecked();
    updated.views.focusPreset = defaultFocusPreset->currentData().toString();
    updated.views.focusFrequencyHz = defaultFocusFrequency->value();

    updated.sync.trackLanes = syncTrackLanes->isChecked();
    updated.sync.autoShareWavs = syncWavs->isChecked();
    updated.sync.globalPlayback = syncPlayback->isChecked();
    updated.sync.generatedIdeas = syncIdeas->currentData().toInt();
    updated.sync.metronomeState = syncMetronome->isChecked();
    updated.sync.recordings = syncRecordings->isChecked();

    const bool localChanged = !networkActive && (
        localInitial.backend != updated.localAudio.backend ||
        localInitial.stableId != updated.localAudio.stableId ||
        localInitial.sampleRate != updated.localAudio.sampleRate ||
        localInitial.bufferSize != updated.localAudio.bufferSize ||
        localInitial.inputChannels != updated.localAudio.inputChannels ||
        localInitial.outputChannels != updated.localAudio.outputChannels);
    if (jam2_.isRunning() && !jam2_.isNetworkRunning() && localChanged) {
        std::optional<Jam2RuntimeOptions> previousOptions;
        const QString previousDevice = selectedDeviceId();
        const QString previousInput = inputChannelsEdit_->text();
        const QString previousOutput = outputChannelsEdit_->text();
        const int previousRate = sampleRateSpin_->value();
        const int previousBuffer = bufferSizeSpin_->value();
        bool restartAttempted = false;
        try {
            previousOptions = runtimeOptions();
            deviceBox_->setCurrentIndex(qMax(0, deviceBox_->findData(localDevice->currentData())));
            inputChannelsEdit_->setText(updated.localAudio.inputChannels);
            outputChannelsEdit_->setText(updated.localAudio.outputChannels);
            sampleRateSpin_->setValue(updated.localAudio.sampleRate);
            bufferSizeSpin_->setValue(updated.localAudio.bufferSize);
            restartAttempted = true;
            if (!sessionController_.startLocal(runtimeOptions())) {
                throw std::runtime_error("the new local audio configuration did not start");
            }
            showLocalSessionHeaderStatus();
        } catch (const std::exception& error) {
            deviceBox_->setCurrentIndex(qMax(0, deviceBox_->findData(previousDevice)));
            inputChannelsEdit_->setText(previousInput);
            outputChannelsEdit_->setText(previousOutput);
            sampleRateSpin_->setValue(previousRate);
            bufferSizeSpin_->setValue(previousBuffer);
            const bool restored = !restartAttempted ||
                (previousOptions && sessionController_.startLocal(*previousOptions));
            QMessageBox::warning(this, QStringLiteral("Settings not applied"),
                QStringLiteral("%1\n\nPrevious local audio settings %2.")
                    .arg(QString::fromUtf8(error.what()),
                         restored ? QStringLiteral("were restored") : QStringLiteral("could not be restored")));
            return;
        }
    }

    preferences_ = std::move(updated);
    joinProfileName_ = preferences_.join.tuning.profile;
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
    pendingSongSet_ = {};
    pendingSongRevision_ = 0;
    trackWorkspace_.pendingSongBaseRevision = 0;
    pendingSongTrackRestart_ = false;
    pendingSongSourcePeerToken_.clear();
    pendingSongNeedsAuthoritativePublish_ = false;
    pendingLooperAssetHashes_.clear();
    incomingAssetWorkflow_ = IncomingAssetWorkflow::None;
    incomingAssetHash_.clear();
    incomingAssetSourcePeerToken_.clear();
    deferredSongSetMessage_ = {};
    deferredSongSetSourcePeerToken_.clear();
    handledReferenceRenderRequests_.clear();
    deferredReferenceRenderRequests_.clear();
    songAssetCheckRetryTimer_.stop();
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
    const QString previous = deviceBox_->currentData().toString();
    deviceBox_->clear();
    availableDevices_.clear();
    try {
        availableDevices_ = jam2::audio::list_devices();
        for (const jam2::audio::DeviceInfo& device : availableDevices_) {
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
        int selected = deviceBox_->findData(previous);
        if (selected < 0) {
            for (const auto& device : availableDevices_) {
                const QString stable = QString::fromStdString(
                    device.clsid.empty() ? device.name : device.clsid);
                if (QString::fromStdString(device.backend) == preferences_.networkAudio.backend &&
                    stable == preferences_.networkAudio.stableId) {
                    selected = deviceBox_->findData(QString::number(device.id));
                    break;
                }
            }
        }
        deviceBox_->setCurrentIndex(selected >= 0 ? selected : 0);
        appendLog(QStringLiteral("loaded %1 audio devices").arg(deviceBox_->count()));
    }
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
    const jam2::CreateProfile* profile = jam2::find_create_profile(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (profile == nullptr) {
        profile = &jam2::default_create_profile();
    }

    const jam2::JoinProfile& local = *profile->local;

    sampleRateSpin_->setValue(profile->sample_rate);
    bufferSizeSpin_->setValue(static_cast<int>(local.audio_buffer_size));
    frameSizeSpin_->setValue(profile->frame_size);
    prefillSpin_->setValue(static_cast<int>(local.playback_prefill_frames));
    playbackMaxSpin_->setValue(static_cast<int>(local.playback_max_frames));
    captureRingSpin_->setValue(static_cast<int>(local.capture_ring_frames));
    playbackRingSpin_->setValue(static_cast<int>(local.playback_ring_frames));
    driftCorrectionCheck_->setChecked(local.drift_correction);
    driftSmoothingSpin_->setValue(local.drift_smoothing);
    driftDeadbandSpin_->setValue(local.drift_deadband_ppm);
    driftMaxCorrectionSpin_->setValue(local.drift_max_correction_ppm);
    sampleTimePlayoutCheck_->setChecked(local.sample_time_playout);
    playoutDelaySpin_->setValue(static_cast<int>(local.playout_delay_frames));
    jitterBufferSpin_->setValue(static_cast<int>(local.jitter_buffer_frames));
    jitterBufferMaxSpin_->setValue(static_cast<int>(local.jitter_buffer_max_frames));
    adaptiveCushionCheck_->setChecked(local.adaptive_playback_cushion);
    adaptiveTargetSpin_->setValue(static_cast<int>(local.adaptive_playback_target_frames));
    adaptiveMinSpin_->setValue(static_cast<int>(local.adaptive_playback_min_frames));
    adaptiveMaxSpin_->setValue(static_cast<int>(local.adaptive_playback_max_frames));
    adaptiveReleaseSpin_->setValue(local.adaptive_playback_release_ppm);
    adaptiveRatioRampSpin_->setValue(local.adaptive_playback_ratio_ramp_ms);
}

void MainWindow::applyJoinProfileName(const QString& name)
{
    const QByteArray utf8 = name.toUtf8();
    const jam2::JoinProfile* profile = jam2::find_join_profile(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (profile == nullptr) profile = &jam2::default_join_profile();
    joinProfileName_ = QString::fromUtf8(
        profile->name.data(), static_cast<qsizetype>(profile->name.size()));
    bufferSizeSpin_->setValue(static_cast<int>(profile->audio_buffer_size));
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

void MainWindow::applyCreateDefaultsToControls()
{
    const auto& p = preferences_.create;
    applyTuningProfileName(p.tuning.profile);
    bindHostEdit_->setText(p.bindHost);
    portSpin_->setValue(p.port);
    publicHostEdit_->setText(p.publicHost);
    stunServerEdit_->setText(p.stunServer);
    stunTimeoutSpin_->setValue(p.stunTimeoutMs);
    stunRetriesSpin_->setValue(p.stunRetries);
    noStunCheck_->setChecked(p.noStun);
    meshMaxPeersSpin_->setValue(p.maxPeers);
    sampleRateSpin_->setValue(p.sampleRate);
    bufferSizeSpin_->setValue(p.tuning.bufferSize);
    frameSizeSpin_->setValue(p.tuning.frameSize);
    prefillSpin_->setValue(p.tuning.prefillFrames);
    playbackMaxSpin_->setValue(p.tuning.playbackMaxFrames);
    captureRingSpin_->setValue(p.tuning.captureRingFrames);
    playbackRingSpin_->setValue(p.tuning.playbackRingFrames);
    driftCorrectionCheck_->setChecked(p.tuning.driftCorrection);
    driftSmoothingSpin_->setValue(p.tuning.driftSmoothing);
    driftDeadbandSpin_->setValue(p.tuning.driftDeadbandPpm);
    driftMaxCorrectionSpin_->setValue(p.tuning.driftMaxCorrectionPpm);
    sampleTimePlayoutCheck_->setChecked(p.tuning.sampleTimePlayout);
    playoutDelaySpin_->setValue(p.tuning.playoutDelayFrames);
    jitterBufferSpin_->setValue(p.tuning.jitterBufferFrames);
    jitterBufferMaxSpin_->setValue(p.tuning.jitterBufferMaxFrames);
    adaptiveCushionCheck_->setChecked(p.tuning.adaptiveCushion);
    adaptiveTargetSpin_->setValue(p.tuning.adaptiveTargetFrames);
    adaptiveMinSpin_->setValue(p.tuning.adaptiveMinFrames);
    adaptiveMaxSpin_->setValue(p.tuning.adaptiveMaxFrames);
    adaptiveReleaseSpin_->setValue(p.tuning.adaptiveReleasePpm);
    adaptiveRatioRampSpin_->setValue(p.tuning.adaptiveRatioRampMs);
    statsCheck_->setChecked(p.runtime.diagnostics);
    statsWarmupMsSpin_->setValue(p.runtime.diagnosticsWarmupMs);
    if (!p.runtime.logStatsFolder.isEmpty()) logStatsEdit_->setText(p.runtime.logStatsFolder);
    const int priority = osPriorityBox_->findData(p.runtime.osPriority);
    if (priority >= 0) osPriorityBox_->setCurrentIndex(priority);
    waitMsSpin_->setValue(p.runtime.waitMs);
    streamMsSpin_->setValue(p.runtime.streamMs);
    streamLingerMsSpin_->setValue(p.runtime.streamLingerMs);
    socketSendBufferSpin_->setValue(p.socketSendBuffer);
    socketRecvBufferSpin_->setValue(p.socketRecvBuffer);
    const int format = networkAudioFormatBox_->findData(p.audioFormat);
    if (format >= 0) networkAudioFormatBox_->setCurrentIndex(format);
    inputChannelsEdit_->setText(preferences_.createAudio().inputChannels);
    outputChannelsEdit_->setText(preferences_.createAudio().outputChannels);
    updateConnectionControlState();
}

void MainWindow::applyJoinDefaultsToControls()
{
    const auto& p = preferences_.join;
    applyJoinProfileName(p.tuning.profile);
    bindHostEdit_->setText(p.bindHost);
    portSpin_->setValue(p.port);
    bufferSizeSpin_->setValue(p.tuning.bufferSize);
    prefillSpin_->setValue(p.tuning.prefillFrames);
    playbackMaxSpin_->setValue(p.tuning.playbackMaxFrames);
    captureRingSpin_->setValue(p.tuning.captureRingFrames);
    playbackRingSpin_->setValue(p.tuning.playbackRingFrames);
    driftCorrectionCheck_->setChecked(p.tuning.driftCorrection);
    driftSmoothingSpin_->setValue(p.tuning.driftSmoothing);
    driftDeadbandSpin_->setValue(p.tuning.driftDeadbandPpm);
    driftMaxCorrectionSpin_->setValue(p.tuning.driftMaxCorrectionPpm);
    sampleTimePlayoutCheck_->setChecked(p.tuning.sampleTimePlayout);
    playoutDelaySpin_->setValue(p.tuning.playoutDelayFrames);
    jitterBufferSpin_->setValue(p.tuning.jitterBufferFrames);
    jitterBufferMaxSpin_->setValue(p.tuning.jitterBufferMaxFrames);
    adaptiveCushionCheck_->setChecked(p.tuning.adaptiveCushion);
    adaptiveTargetSpin_->setValue(p.tuning.adaptiveTargetFrames);
    adaptiveMinSpin_->setValue(p.tuning.adaptiveMinFrames);
    adaptiveMaxSpin_->setValue(p.tuning.adaptiveMaxFrames);
    adaptiveReleaseSpin_->setValue(p.tuning.adaptiveReleasePpm);
    adaptiveRatioRampSpin_->setValue(p.tuning.adaptiveRatioRampMs);
    statsCheck_->setChecked(p.runtime.diagnostics);
    statsWarmupMsSpin_->setValue(p.runtime.diagnosticsWarmupMs);
    if (!p.runtime.logStatsFolder.isEmpty()) logStatsEdit_->setText(p.runtime.logStatsFolder);
    const int priority = osPriorityBox_->findData(p.runtime.osPriority);
    if (priority >= 0) osPriorityBox_->setCurrentIndex(priority);
    waitMsSpin_->setValue(p.runtime.waitMs);
    streamMsSpin_->setValue(p.runtime.streamMs);
    streamLingerMsSpin_->setValue(p.runtime.streamLingerMs);
    inputChannelsEdit_->setText(preferences_.joinAudio().inputChannels);
    outputChannelsEdit_->setText(preferences_.joinAudio().outputChannels);
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
    track.loopEnabled = preferences_.views.trackLoop;
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
    joinProfileName_ = preferences_.join.tuning.profile;
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
    track.loopEnabled = preferences_.views.trackLoop;
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
    auto& p = preferences_.create;
    p.bindHost = bindHostEdit_->text().trimmed(); p.port = portSpin_->value();
    p.publicHost = publicHostEdit_->text().trimmed(); p.stunServer = stunServerEdit_->text().trimmed();
    p.stunTimeoutMs = stunTimeoutSpin_->value(); p.stunRetries = stunRetriesSpin_->value();
    p.noStun = noStunCheck_->isChecked(); p.maxPeers = meshMaxPeersSpin_->value();
    p.sampleRate = sampleRateSpin_->value(); p.audioFormat = networkAudioFormatBox_->currentData().toString();
    p.socketSendBuffer = socketSendBufferSpin_->value(); p.socketRecvBuffer = socketRecvBufferSpin_->value();
    p.tuning.profile = profileBox_->currentData().toString();
    p.tuning.bufferSize = bufferSizeSpin_->value(); p.tuning.frameSize = frameSizeSpin_->value();
    p.tuning.prefillFrames = prefillSpin_->value(); p.tuning.playbackMaxFrames = playbackMaxSpin_->value();
    p.tuning.captureRingFrames = captureRingSpin_->value(); p.tuning.playbackRingFrames = playbackRingSpin_->value();
    p.tuning.driftCorrection = driftCorrectionCheck_->isChecked(); p.tuning.driftSmoothing = driftSmoothingSpin_->value();
    p.tuning.driftDeadbandPpm = driftDeadbandSpin_->value(); p.tuning.driftMaxCorrectionPpm = driftMaxCorrectionSpin_->value();
    p.tuning.sampleTimePlayout = sampleTimePlayoutCheck_->isChecked(); p.tuning.playoutDelayFrames = playoutDelaySpin_->value();
    p.tuning.jitterBufferFrames = jitterBufferSpin_->value(); p.tuning.jitterBufferMaxFrames = jitterBufferMaxSpin_->value();
    p.tuning.adaptiveCushion = adaptiveCushionCheck_->isChecked(); p.tuning.adaptiveTargetFrames = adaptiveTargetSpin_->value();
    p.tuning.adaptiveMinFrames = adaptiveMinSpin_->value(); p.tuning.adaptiveMaxFrames = adaptiveMaxSpin_->value();
    p.tuning.adaptiveReleasePpm = adaptiveReleaseSpin_->value(); p.tuning.adaptiveRatioRampMs = adaptiveRatioRampSpin_->value();
    p.runtime.diagnostics = statsCheck_->isChecked(); p.runtime.diagnosticsWarmupMs = statsWarmupMsSpin_->value();
    p.runtime.logStatsFolder = logStatsEdit_->text().trimmed(); p.runtime.osPriority = osPriorityBox_->currentData().toString();
    p.runtime.waitMs = waitMsSpin_->value(); p.runtime.streamMs = streamMsSpin_->value(); p.runtime.streamLingerMs = streamLingerMsSpin_->value();
    AudioDevicePreference& audio = preferences_.createAudio();
    audio.inputChannels = inputChannelsEdit_->text().trimmed();
    audio.outputChannels = outputChannelsEdit_->text().trimmed();
    preferences_.logging.folder = p.runtime.logStatsFolder;
    preferences_.join.runtime.logStatsFolder = preferences_.logging.folder;
    bool deviceOk = false;
    const int deviceId = selectedDeviceId().toInt(&deviceOk);
    if (deviceOk) {
        const auto device = std::find_if(availableDevices_.begin(), availableDevices_.end(),
            [deviceId](const auto& item) { return item.id == deviceId; });
        if (device != availableDevices_.end()) {
            audio.backend = QString::fromStdString(device->backend);
            audio.stableId = QString::fromStdString(
                device->clsid.empty() ? device->name : device->clsid);
            audio.name = QString::fromStdString(device->name);
        }
    }
    UserPreferencesStore::save(preferences_);
}

void MainWindow::saveJoinDefaults()
{
    auto& p = preferences_.join;
    p.bindHost = bindHostEdit_->text().trimmed(); p.port = portSpin_->value();
    p.tuning.profile = joinProfileName_; p.tuning.bufferSize = bufferSizeSpin_->value();
    p.tuning.prefillFrames = prefillSpin_->value(); p.tuning.playbackMaxFrames = playbackMaxSpin_->value();
    p.tuning.captureRingFrames = captureRingSpin_->value(); p.tuning.playbackRingFrames = playbackRingSpin_->value();
    p.tuning.driftCorrection = driftCorrectionCheck_->isChecked(); p.tuning.driftSmoothing = driftSmoothingSpin_->value();
    p.tuning.driftDeadbandPpm = driftDeadbandSpin_->value(); p.tuning.driftMaxCorrectionPpm = driftMaxCorrectionSpin_->value();
    p.tuning.sampleTimePlayout = sampleTimePlayoutCheck_->isChecked(); p.tuning.playoutDelayFrames = playoutDelaySpin_->value();
    p.tuning.jitterBufferFrames = jitterBufferSpin_->value(); p.tuning.jitterBufferMaxFrames = jitterBufferMaxSpin_->value();
    p.tuning.adaptiveCushion = adaptiveCushionCheck_->isChecked(); p.tuning.adaptiveTargetFrames = adaptiveTargetSpin_->value();
    p.tuning.adaptiveMinFrames = adaptiveMinSpin_->value(); p.tuning.adaptiveMaxFrames = adaptiveMaxSpin_->value();
    p.tuning.adaptiveReleasePpm = adaptiveReleaseSpin_->value(); p.tuning.adaptiveRatioRampMs = adaptiveRatioRampSpin_->value();
    p.runtime.diagnostics = statsCheck_->isChecked(); p.runtime.diagnosticsWarmupMs = statsWarmupMsSpin_->value();
    p.runtime.logStatsFolder = logStatsEdit_->text().trimmed(); p.runtime.osPriority = osPriorityBox_->currentData().toString();
    p.runtime.waitMs = waitMsSpin_->value(); p.runtime.streamMs = streamMsSpin_->value(); p.runtime.streamLingerMs = streamLingerMsSpin_->value();
    AudioDevicePreference& audio = preferences_.joinAudio();
    audio.inputChannels = inputChannelsEdit_->text().trimmed();
    audio.outputChannels = outputChannelsEdit_->text().trimmed();
    preferences_.logging.folder = p.runtime.logStatsFolder;
    preferences_.create.runtime.logStatsFolder = preferences_.logging.folder;
    bool deviceOk = false;
    const int deviceId = selectedDeviceId().toInt(&deviceOk);
    if (deviceOk) {
        const auto device = std::find_if(availableDevices_.begin(), availableDevices_.end(),
            [deviceId](const auto& item) { return item.id == deviceId; });
        if (device != availableDevices_.end()) {
            audio.backend = QString::fromStdString(device->backend);
            audio.stableId = QString::fromStdString(
                device->clsid.empty() ? device->name : device->clsid);
            audio.name = QString::fromStdString(device->name);
        }
    }
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
            ? devicePreferenceKey(*device)
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
            .arg(deviceBox_ ? deviceBox_->currentText() : QStringLiteral("unknown"))
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
        showQuietDeviceMessage(dialogParent, QStringLiteral("Select a low-latency audio device first."));
        return;
    }
    bool ok = false;
    const int deviceId = device->currentData().toInt(&ok);
    if (!ok) {
        showQuietDeviceMessage(dialogParent, QStringLiteral("The selected device id is invalid."));
        return;
    }
    const auto info = std::find_if(availableDevices_.begin(), availableDevices_.end(),
        [deviceId](const auto& item) { return item.id == deviceId; });
    const QString key = info != availableDevices_.end()
        ? devicePreferenceKey(*info)
        : QString::number(deviceId);
    if ((jam2_.isRunning() || jam2_.isNetworkRunning()) &&
        deviceCapabilitiesCache_.contains(key)) {
        showQuietDeviceMessage(dialogParent, deviceCapabilitiesText(deviceCapabilitiesCache_.value(key)));
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
                showQuietDeviceMessage(parentGuard, deviceCapabilitiesText(**result));
            }
        },
        [parentGuard, buttonGuard](const QString& error) {
            if (buttonGuard) buttonGuard->setEnabled(true);
            if (parentGuard) {
                showQuietDeviceMessage(parentGuard, error);
            }
        });
    if (!started) {
        if (buttonGuard) buttonGuard->setEnabled(true);
        showQuietDeviceMessage(dialogParent,
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
    return jamSyncPolicy_.recordings && jamSyncPolicy_.trackLanes &&
        jamSyncPolicy_.globalPlayback;
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
    if (preparedMixWorkerRunning_ || !pendingSongSet_.isEmpty() ||
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
        const bool hasPrepared = !preparedMix_.path.isEmpty() &&
            preparedMix_.error.isEmpty();
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
    return jamSyncPolicy_.autoShareWavs && jamSyncPolicy_.trackLanes;
}

void MainWindow::showJamSyncDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Jam Sync"));
    dialog.setMinimumWidth(500);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto* introduction = new QLabel(QStringLiteral(
        "Choose which changes Jam2 shares. Nothing changes until you apply the complete set to the jam."),
        &dialog);
    introduction->setWordWrap(true);
    introduction->setStyleSheet(QStringLiteral("color:#9ca9ab;"));
    layout->addWidget(introduction);

    auto* contentGroup = new QGroupBox(QStringLiteral("Content Sharing"), &dialog);
    auto* contentLayout = new QFormLayout(contentGroup);
    contentLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    contentLayout->setHorizontalSpacing(18);
    contentLayout->setVerticalSpacing(10);

    auto* trackLanes = new QCheckBox(QStringLiteral("Sync Track Lanes"), contentGroup);
    trackLanes->setChecked(jamSyncPolicy_.trackLanes);
    trackLanes->setToolTip(QStringLiteral(
        "Share lane creation, removal, names, positions and track metadata"));
    contentLayout->addRow(trackLanes);

    auto* autoShareWavs = new QCheckBox(QStringLiteral("Sync WAVs Automatically"), contentGroup);
    autoShareWavs->setChecked(jamSyncPolicy_.autoShareWavs);
    autoShareWavs->setToolTip(QStringLiteral(
        "Automatically transfer audio files belonging to shared lanes"));
    contentLayout->addRow(autoShareWavs);

    auto* generatedIdeas = new QComboBox(contentGroup);
    generatedIdeas->addItem(QStringLiteral("Whole Idea"),
        static_cast<int>(GeneratedIdeaSyncMode::Full));
    generatedIdeas->addItem(QStringLiteral("Chords Only"),
        static_cast<int>(GeneratedIdeaSyncMode::Chords));
    generatedIdeas->addItem(QStringLiteral("Beats Only"),
        static_cast<int>(GeneratedIdeaSyncMode::Beats));
    generatedIdeas->addItem(QStringLiteral("Disabled"),
        static_cast<int>(GeneratedIdeaSyncMode::Off));
    generatedIdeas->setCurrentIndex(qMax(0, generatedIdeas->findData(
        static_cast<int>(jamSyncPolicy_.generatedIdeas))));
    generatedIdeas->setToolTip(QStringLiteral(
        "Choose which parts of newly generated and continued ideas are shared"));
    contentLayout->addRow(QStringLiteral("Sync Generated Ideas"), generatedIdeas);
    layout->addWidget(contentGroup);

    auto* performanceGroup = new QGroupBox(QStringLiteral("Performance Sync"), &dialog);
    auto* performanceLayout = new QFormLayout(performanceGroup);
    performanceLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    performanceLayout->setHorizontalSpacing(18);
    performanceLayout->setVerticalSpacing(10);

    auto* globalPlayback = new QCheckBox(
        QStringLiteral("Sync Global Playback"), performanceGroup);
    globalPlayback->setChecked(jamSyncPolicy_.globalPlayback);
    globalPlayback->setToolTip(QStringLiteral(
        "Share play, stop, restart and queued Section changes"));
    performanceLayout->addRow(globalPlayback);

    auto* metronomeState = new QCheckBox(
        QStringLiteral("Sync Metronome State"), performanceGroup);
    metronomeState->setChecked(jamSyncPolicy_.metronomeState);
    metronomeState->setToolTip(QStringLiteral(
        "Share whether the metronome is turned on or off"));
    performanceLayout->addRow(metronomeState);

    auto* recordings = new QCheckBox(
        QStringLiteral("Sync Recordings"), performanceGroup);
    recordings->setChecked(syncedRecordingsEnabled());
    performanceLayout->addRow(recordings);

    auto* dependencyNote = new QLabel(performanceGroup);
    dependencyNote->setWordWrap(true);
    dependencyNote->setStyleSheet(QStringLiteral("color:#9ca9ab;"));
    performanceLayout->addRow(dependencyNote);
    layout->addWidget(performanceGroup);

    const bool policyLocked = sharedRecordingProtected();
    const bool leaderAudio = metronomeModeBox_ &&
        metronomeModeBox_->currentText() == QStringLiteral("leader-audio");
    const auto updateDependencies = [=] {
        const bool lanesEnabled = trackLanes->isChecked();
        const bool recordingDependencies = lanesEnabled && globalPlayback->isChecked();
        autoShareWavs->setEnabled(lanesEnabled && !policyLocked);
        recordings->setEnabled(recordingDependencies && !policyLocked);
        if (!recordingDependencies) recordings->setChecked(false);
        metronomeState->setEnabled(!leaderAudio && !policyLocked);
        autoShareWavs->setToolTip(lanesEnabled
            ? QStringLiteral("Automatically transfer audio files belonging to shared lanes")
            : QStringLiteral("Requires Sync Track Lanes"));
        recordings->setToolTip(recordingDependencies
            ? QStringLiteral("Coordinate arm, count-in, recording and playback protection")
            : QStringLiteral("Requires Sync Track Lanes and Sync Global Playback"));
        dependencyNote->setText(policyLocked
            ? QStringLiteral("Jam Sync cannot be changed while a shared recording is active.")
            : leaderAudio
                ? QStringLiteral("Recording sync requires Track Lanes and Global Playback. "
                    "Metronome state is supplied by the leader in Leader Audio mode.")
                : QStringLiteral("Recording sync requires Track Lanes and Global Playback."));
    };
    QObject::connect(trackLanes, &QCheckBox::toggled, &dialog, updateDependencies);
    QObject::connect(globalPlayback, &QCheckBox::toggled, &dialog, updateDependencies);

    const std::array<QWidget*, 6> policyControls{
        trackLanes, autoShareWavs, generatedIdeas,
        globalPlayback, metronomeState, recordings,
    };
    for (QWidget* control : policyControls) {
        if (policyLocked) control->setEnabled(false);
    }
    updateDependencies();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* apply = buttons->addButton(
        QStringLiteral("Apply to Jam"), QDialogButtonBox::AcceptRole);
    apply->setEnabled(!policyLocked);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    JamSyncPolicy policy = jamSyncPolicy_;
    policy.trackLanes = trackLanes->isChecked();
    policy.autoShareWavs = autoShareWavs->isChecked();
    policy.globalPlayback = globalPlayback->isChecked();
    policy.generatedIdeas = static_cast<GeneratedIdeaSyncMode>(
        generatedIdeas->currentData().toInt());
    policy.metronomeState = metronomeState->isChecked();
    policy.recordings = recordings->isChecked();
    requestJamSyncPolicy(policy);
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
    policy.recordings = policy.recordings && policy.trackLanes && policy.globalPlayback;
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
    policy.recordings = policy.recordings && policy.trackLanes && policy.globalPlayback;
    const SharedSessionController::Snapshot session = sessionController_.snapshot();
    if (session.role == SharedSessionController::Role::Joiner) {
        const auto ideas = policy.generatedIdeas == GeneratedIdeaSyncMode::Full
            ? QStringLiteral("full") : policy.generatedIdeas == GeneratedIdeaSyncMode::Chords
                ? QStringLiteral("chords") : policy.generatedIdeas == GeneratedIdeaSyncMode::Beats
                    ? QStringLiteral("beats") : QStringLiteral("off");
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("jam.sync.request")},
            {QStringLiteral("track_lanes"), policy.trackLanes},
            {QStringLiteral("auto_share_wavs"), policy.autoShareWavs},
            {QStringLiteral("global_playback"), policy.globalPlayback},
            {QStringLiteral("generated_ideas"), ideas},
            {QStringLiteral("metronome_state"), policy.metronomeState},
            {QStringLiteral("recordings"), policy.recordings},
        });
        updateJamSyncPresentation();
        return;
    }
    policy.revision = jamSyncPolicy_.revision >= (std::numeric_limits<int>::max)()
        ? 1 : jamSyncPolicy_.revision + 1;
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
    const QString ideas = jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Full
        ? QStringLiteral("full") : jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Chords
            ? QStringLiteral("chords") : jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Beats
                ? QStringLiteral("beats") : QStringLiteral("off");
    const QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("jam.sync.set")},
        {QStringLiteral("revision"), jamSyncPolicy_.revision},
        {QStringLiteral("track_lanes"), jamSyncPolicy_.trackLanes},
        {QStringLiteral("auto_share_wavs"), jamSyncPolicy_.autoShareWavs},
        {QStringLiteral("global_playback"), jamSyncPolicy_.globalPlayback},
        {QStringLiteral("generated_ideas"), ideas},
        {QStringLiteral("metronome_state"), jamSyncPolicy_.metronomeState},
        {QStringLiteral("recordings"), syncedRecordingsEnabled()},
    };
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
    policy.trackLanes = message.value(QStringLiteral("track_lanes")).toBool();
    policy.autoShareWavs = message.value(QStringLiteral("auto_share_wavs")).toBool();
    policy.globalPlayback = message.value(QStringLiteral("global_playback")).toBool();
    const QString ideas = message.value(QStringLiteral("generated_ideas")).toString();
    policy.generatedIdeas = ideas == QStringLiteral("chords")
        ? GeneratedIdeaSyncMode::Chords : ideas == QStringLiteral("beats")
            ? GeneratedIdeaSyncMode::Beats : ideas == QStringLiteral("off")
                ? GeneratedIdeaSyncMode::Off : GeneratedIdeaSyncMode::Full;
    policy.metronomeState = message.value(QStringLiteral("metronome_state")).toBool();
    policy.recordings = message.value(QStringLiteral("recordings")).toBool();
    if (type == QStringLiteral("jam.sync.request")) {
        if (sharedRecordingProtected()) {
            sendJamSyncPolicy(sourcePeerToken);
            return;
        }
        const bool beginMetronomeSync = policy.metronomeState &&
            !jamSyncPolicy_.metronomeState;
        policy.revision = jamSyncPolicy_.revision >= (std::numeric_limits<int>::max)()
            ? 1 : jamSyncPolicy_.revision + 1;
        applyJamSyncPolicy(policy, true);
        sendJamSyncPolicy();
        if (beginMetronomeSync) {
            sendMetronomeStateToJam(metronomeTransport_.localRunning());
        }
        return;
    }
    policy.revision = message.value(QStringLiteral("revision")).toInt();
    if (policy.revision <= jamSyncPolicy_.revision) return;
    applyJamSyncPolicy(policy, true);
}

bool MainWindow::jamSyncAllowsControlMessage(const QJsonObject& message) const
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("song.set")) {
        const QString scope = message.value(QStringLiteral("sync_scope")).toString(
            QStringLiteral("tracks"));
        if (scope == QStringLiteral("tracks")) return jamSyncPolicy_.trackLanes;
        if (scope == QStringLiteral("idea.full")) {
            return jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Full;
        }
        if (scope == QStringLiteral("idea.chords")) {
            return jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Full ||
                jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Chords;
        }
        if (scope == QStringLiteral("idea.beats")) {
            return jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Full ||
                jamSyncPolicy_.generatedIdeas == GeneratedIdeaSyncMode::Beats;
        }
        return false;
    }
    if (type == QStringLiteral("practice.references.render")) {
        return automaticWavSharingEnabled();
    }
    if (type == QStringLiteral("looper.recording.state")) {
        return syncedRecordingsEnabled();
    }
    if (type == QStringLiteral("looper.recording.group.start") ||
        type == QStringLiteral("looper.recording.group.finish") ||
        type == QStringLiteral("looper.recording.group.recover.request") ||
        type == QStringLiteral("looper.recording.group.recover")) {
        return syncedRecordingsEnabled();
    }
    if (type == QStringLiteral("looper.recording.resync.request") ||
        type == QStringLiteral("looper.recording.resync.state")) {
        return jamSyncPolicy_.globalPlayback;
    }
    if (type == QStringLiteral("jam.metronome.state.request") ||
        type == QStringLiteral("jam.metronome.state.set")) {
        return jamSyncPolicy_.metronomeState;
    }
    if (type == QStringLiteral("bank.request") ||
        type == QStringLiteral("bank.prepare") ||
        type == QStringLiteral("bank.ready") ||
        type == QStringLiteral("bank.cancel") ||
        type == QStringLiteral("bank.switch")) {
        return jamSyncPolicy_.globalPlayback;
    }
    return true;
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
        [this, expectedSampleRate, showModal, results, auditSignature, generation] {
            wavCompatibilityAuditRunning_ = false;
            if (generation != wavCompatibilityAuditGeneration_) {
                return;
            }
            if (pendingWavCompatibilityAuditRate_ > 0 &&
                pendingWavCompatibilityAuditRate_ != expectedSampleRate) {
                const int pending = pendingWavCompatibilityAuditRate_;
                pendingWavCompatibilityAuditRate_ = 0;
                auditWavCompatibilityForSession(pending, showModal);
                return;
            }
            QStringList resamplingFailures;
            QStringList temporarilyUnavailable;
            bool arrangementConverted = false;
            for (const Result& result : *results) {
                bool stillPresent = false;
                if (result.input.track) {
                    SharedTrackModel& current = trackController_.model();
                    stillPresent = current.filePath == result.input.path;
                    if (stillPresent && result.error.isEmpty()) {
                        if (result.converted.resampled) {
                            registerTransientTrackWav(result.converted.stagedPath);
                            current.filePath = result.converted.stagedPath;
                            current.fileBytes = QFileInfo(result.converted.stagedPath).size();
                            current.durationMs = result.converted.metadata.durationMs;
                            current.sha256 = result.converted.sha256;
                            appendLog(QStringLiteral(
                                "session WAV resampled %1: source_rate=%2 target_rate=%3")
                                .arg(result.input.name)
                                .arg(result.converted.sourceSampleRate)
                                .arg(result.converted.metadata.sampleRate));
                        }
                        current.sampleRate = result.sampleRate;
                        current.sampleRateCompatible = true;
                    } else if (stillPresent && result.conversionAttempted) {
                        current.sampleRateCompatible = false;
                    }
                } else if (result.input.bank >= 0 &&
                           result.input.bank < looperProject_.banks().size()) {
                    for (LooperLane& lane : looperProject_.banks()[result.input.bank].lanes) {
                        if (lane.id == result.input.laneId &&
                            looperAssetAbsolutePath(lane) == result.input.path) {
                            stillPresent = true;
                            if (result.error.isEmpty()) {
                                if (result.converted.resampled) {
                                    const QString oldHash = lane.assetHash;
                                    registerTransientTrackWav(result.converted.stagedPath);
                                    looperWaveformCache_.remove(result.input.path);
                                    lane.assetPath = result.converted.stagedPath;
                                    lane.assetHash = result.converted.sha256;
                                    validatedTrackAssetHashes_.remove(oldHash);
                                    validatedTrackAssetHashes_.insert(lane.assetHash);
                                    arrangementConverted = true;
                                    appendLog(QStringLiteral(
                                        "session lane resampled %1: source_rate=%2 target_rate=%3")
                                        .arg(result.input.name)
                                        .arg(result.converted.sourceSampleRate)
                                        .arg(result.converted.metadata.sampleRate));
                                }
                                lane.sampleRate = result.sampleRate;
                                lane.sourceFrames = result.sourceFrames;
                                lane.sampleRateCompatible = true;
                            } else if (result.conversionAttempted) {
                                lane.sampleRateCompatible = false;
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
        [this](const QString& error) {
            wavCompatibilityAuditRunning_ = false;
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
    preparedMixByBank_[static_cast<std::size_t>(added)] = {};
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
    preparedMixByBank_[static_cast<std::size_t>(removed)] = {};
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
    sharedBankSwitchId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sharedBankSwitchIndex_ = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    sharedBankTargetAbsoluteBeat_ = targetAbsoluteBeat.value_or(0);
    sharedBankHostReady_ = false;
    sharedBankReadyTokens_.clear();
    pendingBankIndex_ = sharedBankSwitchIndex_;
    pendingBankAbsoluteBeat_ = 0;
    refreshBankPresentation();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("bank.prepare")},
        {QStringLiteral("switch_id"), sharedBankSwitchId_},
        {QStringLiteral("bank"), sharedBankSwitchIndex_},
        {QStringLiteral("target_abs_beat"), sharedBankTargetAbsoluteBeat_ > 0
            ? QString::number(sharedBankTargetAbsoluteBeat_) : QString{}},
    });
    const QString switchId = sharedBankSwitchId_;
    prepareSharedBankLaunch(sharedBankSwitchIndex_, switchId);
    QPointer<MainWindow> self(this);
    QTimer::singleShot(30000, this, [self, switchId] {
        if (self && self->sharedBankSwitchId_ == switchId) {
            self->cancelSharedBankLaunch(
                true, QStringLiteral("timed out waiting for every peer to prepare the bank"));
        }
    });
}

void MainWindow::prepareSharedBankLaunch(int bankIndex, const QString& switchId)
{
    bankIndex = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    if (switchId.isEmpty()) return;
    if (sharedBankSwitchId_ != switchId) {
        sharedBankReadyTokens_.clear();
        sharedBankHostReady_ = false;
    }
    sharedBankSwitchId_ = switchId;
    sharedBankSwitchIndex_ = bankIndex;
    pendingBankIndex_ = bankIndex;
    pendingBankAbsoluteBeat_ = 0;
    refreshBankPresentation();

    const bool hasSources = PreparedMixRenderer::hasRenderableSources(looperProject_, bankIndex);
    const PreparedMixResult& cached = preparedMixByBank_[static_cast<std::size_t>(bankIndex)];
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
    if (sharedBankSwitchId_.isEmpty() || bankIndex != sharedBankSwitchIndex_) return;
    if (sessionController_.isServer()) {
        sharedBankHostReady_ = true;
        maybeCommitSharedBankLaunch();
        return;
    }
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("bank.ready")},
        {QStringLiteral("switch_id"), sharedBankSwitchId_},
        {QStringLiteral("bank"), bankIndex},
    });
    appendLog(QStringLiteral("bank ready: bank=%1 switch=%2")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex), sharedBankSwitchId_.left(8)));
}

void MainWindow::handleSharedBankReady(
    int bankIndex,
    const QString& switchId,
    const QString& sourcePeerToken)
{
    if (switchId != sharedBankSwitchId_ || bankIndex != sharedBankSwitchIndex_ ||
        sourcePeerToken.isEmpty() || sourcePeerToken == meshPeerToken_ ||
        !meshPeerEndpoints_.contains(sourcePeerToken)) {
        appendLog(QStringLiteral("ignored stale or non-member bank readiness"));
        return;
    }
    sharedBankReadyTokens_.insert(sourcePeerToken);
    appendLog(QStringLiteral("bank ready on peer: bank=%1 peer=%2 switch=%3")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
        .arg(sourcePeerToken.left(8), switchId.left(8)));
    maybeCommitSharedBankLaunch();
}

void MainWindow::maybeCommitSharedBankLaunch()
{
    if (!sessionController_.isServer() || sharedBankSwitchId_.isEmpty() ||
        !sharedBankHostReady_) return;
    QSet<QString> expected;
    for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it) {
        if (it.key() != meshPeerToken_) expected.insert(it.key());
    }
    for (const QString& token : expected) {
        if (!sharedBankReadyTokens_.contains(token)) return;
    }

    const int bankIndex = sharedBankSwitchIndex_;
    const QString switchId = sharedBankSwitchId_;
    const PlaybackGrid::Position position = metronomeTransport_.grid().position();
    quint64 targetBeat = 0;
    if (!bankGridTimingDiffers(bankIndex) &&
        trackRecordingWorkflow_.globalTransportPlaying() && position.engineAnchored &&
        position.sampleRate > 0 && position.secondsPerBeat > 0.0) {
        const int beatsPerBar = qMax(1, currentMetronomePattern().beats_per_bar);
        targetBeat = sharedBankTargetAbsoluteBeat_ > position.absoluteBeat
            ? sharedBankTargetAbsoluteBeat_
            : nextGridBoundaryBeat(position.absoluteBeat, beatsPerBar, true);
    }
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("bank.switch")},
        {QStringLiteral("switch_id"), switchId},
        {QStringLiteral("bank"), bankIndex},
        {QStringLiteral("target_abs_beat"), QString::number(targetBeat)},
    });
    sharedBankSwitchId_.clear();
    sharedBankSwitchIndex_ = -1;
    sharedBankTargetAbsoluteBeat_ = 0;
    sharedBankHostReady_ = false;
    sharedBankReadyTokens_.clear();
    schedulePreparedBankLaunch(bankIndex, targetBeat);
    appendLog(QStringLiteral(
        "shared bank prepared: bank=%1 peers=%2 target_abs_beat=%3 switch=%4")
        .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
        .arg(expected.size())
        .arg(targetBeat)
        .arg(switchId.left(8)));
}

void MainWindow::cancelSharedBankLaunch(bool broadcast, const QString& reason)
{
    if (sharedBankSwitchId_.isEmpty()) {
        if (pendingBankAbsoluteBeat_ == 0) {
            pendingBankIndex_ = -1;
            pendingBankRequestedTargetBeat_.reset();
            refreshBankPresentation();
        }
        return;
    }
    if (broadcast && sessionController_.isServer()) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("bank.cancel")},
            {QStringLiteral("switch_id"), sharedBankSwitchId_},
            {QStringLiteral("bank"), sharedBankSwitchIndex_},
        });
    }
    appendLog(QStringLiteral("bank preparation cancelled: bank=%1 switch=%2 reason=%3")
        .arg(QChar(QLatin1Char('A').unicode() + qMax(0, sharedBankSwitchIndex_)))
        .arg(sharedBankSwitchId_.left(8), reason));
    sharedBankSwitchId_.clear();
    sharedBankSwitchIndex_ = -1;
    sharedBankTargetAbsoluteBeat_ = 0;
    sharedBankHostReady_ = false;
    sharedBankReadyTokens_.clear();
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
    const PreparedMixResult& cached = preparedMixByBank_[static_cast<std::size_t>(bankIndex)];
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

    const quint64 targetBeat = targetAbsoluteBeat.value_or(nextGridBoundaryBeat(
        position.absoluteBeat,
        currentMetronomePattern().beats_per_bar,
        true));
    const quint64 musicalFrame = position.epochFrame + static_cast<quint64>(std::llround(
        static_cast<double>(targetBeat) * position.secondsPerBeat * position.sampleRate));
    const quint64 targetFrame = rawFrameFromMusicalFrame(
        musicalFrame, position.renderOffsetFrames);
    if (hasSources) {
        preparedMix_ = cached;
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
        if (!sharedBankSwitchId_.isEmpty()) {
            cancelSharedBankLaunch(
                sessionController_.isServer(),
                QStringLiteral("global playback stopped"));
        } else {
            pendingBankIndex_ = -1;
            pendingBankAbsoluteBeat_ = 0;
            pendingBankRequestedTargetBeat_.reset();
            sharedBankTargetAbsoluteBeat_ = 0;
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
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Arrangement"));
    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(2);
    table->setShowGrid(false);
    table->setHorizontalHeaderLabels({QStringLiteral("Section"), QStringLiteral("Repeats")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setHighlightSections(false);
    table->verticalHeader()->setHighlightSections(false);
    constexpr int kArrangementMinimumRowHeight = 46;
    table->verticalHeader()->setMinimumSectionSize(kArrangementMinimumRowHeight);
    table->verticalHeader()->setDefaultSectionSize(kArrangementMinimumRowHeight);
    const auto bankAt = [table](int row) {
        QWidget* cell = table->cellWidget(row, 0);
        if (auto* bank = qobject_cast<QComboBox*>(cell)) return bank;
        return cell ? cell->findChild<QComboBox*>() : nullptr;
    };
    const auto repeatsAt = [table](int row) {
        QWidget* cell = table->cellWidget(row, 1);
        if (auto* repeats = qobject_cast<QSpinBox*>(cell)) return repeats;
        return cell ? cell->findChild<QSpinBox*>() : nullptr;
    };
    const int arrangementBankCount = looperProject_.banks().size();
    const auto appendRow = [table, kArrangementMinimumRowHeight, arrangementBankCount](ArrangementStep step) {
        if (table->rowCount() >= 64) return;
        const int row = table->rowCount();
        table->insertRow(row);
        auto* bank = new QComboBox(table);
        for (int index = 0; index < arrangementBankCount; ++index) {
            bank->addItem(QStringLiteral("Section %1")
                .arg(QChar(QLatin1Char('A').unicode() + index)), index);
        }
        bank->setCurrentIndex(qBound(0, step.bankIndex, arrangementBankCount - 1));
        auto* repeats = new QSpinBox(table);
        repeats->setRange(1, 64);
        repeats->setValue(qBound(1, step.repeats, 64));
        bank->ensurePolished();
        repeats->ensurePolished();
        const int editorHeight = std::max({
            bank->sizeHint().height(),
            repeats->sizeHint().height(),
            bank->fontMetrics().height() + 20,
            repeats->fontMetrics().height() + 20});
        bank->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        repeats->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        table->setRowHeight(
            row,
            qMax(kArrangementMinimumRowHeight, editorHeight));
        table->setCellWidget(row, 0, bank);
        table->setCellWidget(row, 1, repeats);
    };
    for (const ArrangementStep& step : looperProject_.arrangement().steps) appendRow(step);
    if (table->rowCount() == 0) appendRow(ArrangementStep{});

    auto* add = new QPushButton(QStringLiteral("Add"), &dialog);
    auto* remove = new QPushButton(QStringLiteral("Remove"), &dialog);
    auto* up = new QPushButton(QStringLiteral("Up"), &dialog);
    auto* down = new QPushButton(QStringLiteral("Down"), &dialog);
    auto* loop = new QCheckBox(QStringLiteral("Loop Arrangement"), &dialog);
    loop->setChecked(looperProject_.arrangement().loop);
    auto* rowButtons = new QHBoxLayout();
    rowButtons->addWidget(add);
    rowButtons->addWidget(remove);
    rowButtons->addWidget(up);
    rowButtons->addWidget(down);
    rowButtons->addStretch(1);
    rowButtons->addWidget(loop);
    QObject::connect(add, &QPushButton::clicked, &dialog, [appendRow] { appendRow(ArrangementStep{}); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [table] {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    const auto moveRow = [table, appendRow, bankAt, repeatsAt](int direction) {
        const int row = table->currentRow();
        const int target = row + direction;
        if (row < 0 || target < 0 || target >= table->rowCount()) return;
        auto read = [bankAt, repeatsAt](int r) {
            auto* bank = bankAt(r);
            auto* repeats = repeatsAt(r);
            return ArrangementStep{bank ? bank->currentData().toInt() : 0,
                repeats ? repeats->value() : 1};
        };
        QVector<ArrangementStep> values;
        for (int r = 0; r < table->rowCount(); ++r) values.push_back(read(r));
        values.swapItemsAt(row, target);
        table->setRowCount(0);
        for (const ArrangementStep& value : values) appendRow(value);
        table->setCurrentCell(target, 0);
    };
    QObject::connect(up, &QPushButton::clicked, &dialog, [moveRow] { moveRow(-1); });
    QObject::connect(down, &QPushButton::clicked, &dialog, [moveRow] { moveRow(1); });

    bool startAfterSave = false;
    bool stopAfterSave = false;
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto* start = buttons->addButton(
        arrangementWasActive ? QStringLiteral("Stop") : QStringLiteral("Save + Start"),
        QDialogButtonBox::ActionRole);
    QObject::connect(start, &QPushButton::clicked, &dialog,
        [&dialog, &startAfterSave, &stopAfterSave, arrangementWasActive] {
        startAfterSave = !arrangementWasActive;
        stopAfterSave = arrangementWasActive;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        QStringLiteral("Each repeat plays the selected section for one complete loop."), &dialog));
    layout->addWidget(table, 1);
    layout->addLayout(rowButtons);
    layout->addWidget(buttons);
    dialog.resize(480, 420);
    if (dialog.exec() != QDialog::Accepted) return;
    ArrangementDefinition definition;
    definition.loop = loop->isChecked();
    definition.enabled = arrangementWasActive;
    for (int row = 0; row < table->rowCount(); ++row) {
        auto* bank = bankAt(row);
        auto* repeats = repeatsAt(row);
        definition.steps.push_back(ArrangementStep{
            bank ? bank->currentData().toInt() : 0,
            repeats ? repeats->value() : 1});
    }
    if (!looperProject_.setArrangement(std::move(definition))) return;
    syncLooperArrangement();
    if (stopAfterSave) {
        stopArrangement();
        appendLog(QStringLiteral("arrangement stopped from performance view"));
    } else if (startAfterSave) {
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
    int sampleRate) const
{
    sampleRate = qMax(1, sampleRate);
    if (lane.stopFrame > lane.startFrame) {
        return lane.stopFrame;
    }

    qint64 sourceFrames = lane.sourceFrames;
    if (sourceFrames <= 0 && lane.assetPath.trimmed().isEmpty()) {
        return qMax<qint64>(0, lane.startFrame);
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
    if (sourceFrames <= 0) return qMax<qint64>(0, lane.startFrame);

    const qint64 sourceStart = lane.loopStartFrame >= 0
        ? qBound<qint64>(0, lane.loopStartFrame, sourceFrames - 1) : 0;
    const qint64 sourceEnd = lane.loopEndFrame > sourceStart
        ? qBound<qint64>(sourceStart + 1, lane.loopEndFrame, sourceFrames)
        : sourceFrames;
    qint64 visibleFrames = sourceEnd - sourceStart;
    if (lane.sampleRate > 0 && lane.sampleRate != sampleRate) {
        visibleFrames = static_cast<qint64>(std::llround(
            static_cast<double>(visibleFrames) * sampleRate / lane.sampleRate));
    }
    return qMax<qint64>(0, lane.startFrame) + qMax<qint64>(1, visibleFrames);
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
    playPreparedMixWhenReady_ = PreparedMixRenderer::hasRenderableSources(
        looperProject_, bankIndex);
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
        dialog.setDefaultButton(QMessageBox::Cancel);
        dialog.exec();
        if (dialog.clickedButton() != deleteButton) return;
    }

    QSet<QString> removedAssets;
    if (wavConflict) {
        for (LooperLane& lane : looperProject_.banks()[bankIndex].lanes) {
            if (looperLaneTimelineEndFrame(lane, sampleRate) <= targetFrames) continue;
            const jam2::gui::SectionTimelineCrop crop =
                jam2::gui::sectionTimelineCropForEnd(
                    lane.startFrame,
                    lane.loopStartFrame,
                    lane.sampleRate,
                    sampleRate,
                    targetFrames);
            if (crop.removePlacement) {
                removedAssets.insert(looperAssetAbsolutePath(lane));
                lane.assetPath.clear();
                lane.assetHash.clear();
                lane.sampleRate = 0;
                lane.sampleRateCompatible = true;
                lane.sourceFrames = 0;
                lane.startFrame = 0;
                lane.stopFrame = -1;
                lane.loopStartFrame = -1;
                lane.loopEndFrame = -1;
                lane.loopEnabled = false;
                lane.referenceKind.clear();
                lane.referenceSourceSignature.clear();
                lane.referenceBpm = 0.0;
                lane.referenceStale = false;
                lane.localOnly = false;
                lane.originKind.clear();
                continue;
            }
            lane.stopFrame = crop.stopFrame;
            lane.loopStartFrame = crop.sourceStartFrame;
            lane.loopEndFrame = crop.sourceEndFrame;
        }
    }
    chordModel_.resizeSection(bankIndex, targetBeats);
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
    if (QMessageBox::question(
            this,
            QStringLiteral("Trim Section"),
            QStringLiteral(
                "Trim Section %1 from %2 to %3 bars?\n\n%4 trailing empty bars will be removed. Musical content and placed WAVs are protected.")
                .arg(QChar(QLatin1Char('A').unicode() + bankIndex))
                .arg(currentBars)
                .arg(targetBars)
                .arg(currentBars - targetBars),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
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
    LooperBank& bank = looperProject_.banks()[viewedBankIndex_];
    if (laneIndex < 0 || laneIndex >= bank.lanes.size()) {
        return;
    }
    selectedLooperLane_ = laneIndex;
    bank.lanes[laneIndex].gainDb = qBound(-60.0, gainDb, 12.0);
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
    const QString sourceDisposition = promptJamTasterSourceDisposition(this);
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
                    if (!appendJamTasterReferenceSection(object, lane, referenceError)) {
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
        ? promptJamTasterSourceDisposition(this) : QString();
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
                !appendJamTasterReferenceSection(next, *referenceLane, *error)) {
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

void MainWindow::loadWavIntoLooperLane()
{
    const int bankIndex = viewedBankIndex_;
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
            QStringLiteral("WAV files (*.wav *.WAV)"),
            nullptr,
            QFileDialog::Options{});
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
    importWavIntoLooperLane(selectedLaneIndex, sourcePath);
}

void MainWindow::importWavIntoLooperLane(int laneIndex, const QString& sourcePath)
{
    if (sharedRecordingProtected()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Import WAV"),
            QStringLiteral("Audio cannot be imported while a synced recording is active."));
        return;
    }
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        QMessageBox::warning(
            this, QStringLiteral("Import WAV"), QStringLiteral("The dropped file could not be found."));
        return;
    }
    if (sourceInfo.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this, QStringLiteral("Import WAV"), QStringLiteral("Only WAV audio files can be imported into a track."));
        return;
    }
    const int bankIndex = viewedBankIndex_;
    if (bankIndex < 0 || bankIndex >= looperProject_.banks().size() ||
        laneIndex < -1 || laneIndex >= looperProject_.banks().at(bankIndex).lanes.size()) {
        QMessageBox::warning(
            this, QStringLiteral("Import WAV"), QStringLiteral("The selected track lane is no longer available."));
        return;
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
            lane.sourceFrames = result->metadata.frames;
            lane.originKind = QStringLiteral("imported");
            lane.referenceKind.clear();
            lane.referenceSourceSignature.clear();
            lane.referenceBpm = 0.0;
            lane.referenceStale = false;
            lane.localOnly = false;
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

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Arm Lane Recording"));
    dialog.resize(720, 430);
    auto* content = new QWidget(&dialog);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* form = new QFormLayout(content);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setFormAlignment(Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setVerticalSpacing(10);

    auto* modeBox = new QComboBox(content);
    modeBox->addItem(
        QStringLiteral("Local Input (instrument)"), QStringLiteral("input"));
    modeBox->addItem(
        QStringLiteral("Jam Mix (Jam2 input + peers)"), QStringLiteral("current-jam"));
    modeBox->addItem(
        QStringLiteral("System Loopback (desktop audio)"), QStringLiteral("loopback"));
    modeBox->setCurrentIndex(qMax(0, modeBox->findData(preferences_.recording.preferredMode)));
    applyMutedEditorStyle(modeBox);
    auto* inputSourceBox = new QComboBox(content);
    inputSourceBox->addItem(QStringLiteral("Combined My Send mix"), -1);
    const auto sourceOptions = runtimeOptions();
    if (auto* sourceRouter = jam2_.inputSourceRouter()) {
        for (std::size_t slot = 0; slot < sourceRouter->physical_channels() &&
             slot < audioPluginSources_.size(); ++slot) {
            const auto& source = audioPluginSources_[slot];
            if (source.firstChannel == jam2::audio::kNoInputChannel) continue;
            const int firstNumber = source.firstChannel < sourceOptions.channel_selection.input.size()
                ? sourceOptions.channel_selection.input[source.firstChannel] + 1
                : static_cast<int>(source.firstChannel + 1);
            QString name = QStringLiteral("Input %1").arg(firstNumber);
            if (source.secondChannel != jam2::audio::kNoInputChannel) {
                const int secondNumber = source.secondChannel < sourceOptions.channel_selection.input.size()
                    ? sourceOptions.channel_selection.input[source.secondChannel] + 1
                    : static_cast<int>(source.secondChannel + 1);
                name = QStringLiteral("Inputs %1 + %2 (plugin output -> mono)")
                    .arg(firstNumber).arg(secondNumber);
            }
            if (!source.name.isEmpty()) name += QStringLiteral(" - %1").arg(source.name);
            inputSourceBox->addItem(name, static_cast<qulonglong>(slot));
        }
    }
    for (const auto& source : midiPluginSources_) {
        if (!source || !source->host) continue;
        inputSourceBox->addItem(
            QStringLiteral("%1 - %2")
                .arg(QString::fromStdString(source->deviceInfo.name), source->pluginName),
            static_cast<qulonglong>(source->routerSlot));
    }
    if (recordingInputSourceSlot_) {
        const int current = inputSourceBox->findData(
            static_cast<qulonglong>(*recordingInputSourceSlot_));
        if (current >= 0) inputSourceBox->setCurrentIndex(current);
    }
    applyMutedEditorStyle(inputSourceBox);

    const QList<QWidget*> widgets{
        captureOutputEdit_, loopbackSourceBox_,
        silenceThresholdSpin_, tailSilenceSpin_, trimLeadingCheck_,
        trimTrailingCheck_, recordingLatencyLabel_, recordingLatencyAdjustmentSpin_,
    };
    for (QWidget* widget : widgets) {
        widget->show();
    }
    const QString laneFileName = safeFileName(laneName).isEmpty()
        ? QStringLiteral("lane") : safeFileName(laneName);
    InputRecordingPreference inputDraft = preferences_.recording.input;
    LoopbackRecordingPreference loopbackDraft = preferences_.recording.loopback;
    QString inputOutput = timestampedCapturePath(
        laneFileName,
        resolvedManagedCaptureFolder(
            inputDraft.outputFolder,
            jamAssetFolder(JamStorage::AssetKind::Recorded)));
    QString loopbackOutput = timestampedCapturePath(
        laneFileName + QStringLiteral("-loopback"),
        resolvedManagedCaptureFolder(
            loopbackDraft.outputFolder,
            jamAssetFolder(JamStorage::AssetKind::Recorded)));
    QString currentJamOutput = timestampedCapturePath(
        laneFileName + QStringLiteral("-jam-mix"),
        jamAssetFolder(JamStorage::AssetKind::Recorded));
    auto* includeBackingCheck = new QCheckBox(QStringLiteral("Include backing track"), content);
    auto* includeMetronomeCheck = new QCheckBox(QStringLiteral("Include metronome in WAV"), content);
    includeBackingCheck->setChecked(
        preferences_.recording.jamMixTrack.includeBackingTrack);
    includeMetronomeCheck->setChecked(
        preferences_.recording.jamMixTrack.includeMetronome);
    auto* leaderAudioWarning = new QLabel(
        QStringLiteral(
            "Leader-audio note: a click embedded in received peer audio cannot be removed from this recording."),
        content);
    leaderAudioWarning->setWordWrap(true);

    auto* outputRow = new QWidget(content);
    auto* outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->addWidget(captureOutputEdit_, 1);
    auto* browse = new QPushButton(QStringLiteral("Browse"), content);
    outputLayout->addWidget(browse);

    auto* sourceRow = new QWidget(content);
    auto* sourceLayout = new QHBoxLayout(sourceRow);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(loopbackSourceBox_, 1);
    auto* refreshSources = new QPushButton(QStringLiteral("Refresh Sources"), content);
    sourceLayout->addWidget(refreshSources);
    loopbackSourceBox_->ensurePolished();
    const int loopbackSourceHeight = qMax(
        loopbackSourceBox_->sizeHint().height(),
        loopbackSourceBox_->fontMetrics().height() + 20);
    loopbackSourceBox_->setMinimumHeight(loopbackSourceHeight);
    refreshSources->setMinimumHeight(loopbackSourceHeight);
    sourceRow->setMinimumHeight(loopbackSourceHeight);

    auto* latencyRow = new QWidget(content);
    latencyRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* latencyLayout = new QHBoxLayout(latencyRow);
    latencyLayout->setContentsMargins(0, 0, 0, 0);
    latencyLayout->setAlignment(Qt::AlignTop);
    const int detailLineHeight = content->fontMetrics().lineSpacing();
    recordingLatencyLabel_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Minimum);
    recordingLatencyLabel_->setMinimumHeight(qMax(48, detailLineHeight * 3));
    recordingLatencyLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    latencyLayout->addWidget(recordingLatencyLabel_, 1);
    latencyLayout->addWidget(recordingLatencyAdjustmentSpin_, 0, Qt::AlignTop);
    latencyRow->setMinimumHeight(qMax(
        recordingLatencyLabel_->minimumHeight(),
        recordingLatencyAdjustmentSpin_->sizeHint().height()));
    auto* engineStatus = new QLabel(content);
    engineStatus->setWordWrap(true);
    engineStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    engineStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    engineStatus->setText(jam2_.isRunning()
        ? QStringLiteral("Records the input of the currently loaded engine.")
        : QStringLiteral("Start Perform or a jam before recording engine input."));
    auto* inputLabel = new QLabel(QStringLiteral("Input"), content);
    inputLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* advancedToggle = new QPushButton(content);
    advancedToggle->setText(QStringLiteral("▸  ADVANCED"));
    advancedToggle->setCheckable(true);
    advancedToggle->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:12px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
        "QPushButton:hover { color:#ffd68a; }"));
    auto* advancedContent = new QWidget(content);
    auto* advancedForm = new QFormLayout(advancedContent);
    advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advancedForm->setRowWrapPolicy(QFormLayout::DontWrapRows);
    advancedForm->setContentsMargins(14, 0, 0, 0);
    advancedForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    advancedForm->addRow(QStringLiteral("Recording alignment"), latencyRow);
    advancedForm->addRow(QStringLiteral("Silence threshold"), silenceThresholdSpin_);
    advancedForm->addRow(QStringLiteral("Tail silence"), tailSilenceSpin_);
    advancedForm->addRow(trimLeadingCheck_);
    advancedForm->addRow(trimTrailingCheck_);
    advancedContent->hide();

    form->addRow(QStringLiteral("Lane"), new QLabel(laneName, content));
    form->addRow(QStringLiteral("Source"), modeBox);
    form->addRow(QStringLiteral("Input source"), inputSourceBox);
    form->addRow(QStringLiteral("Take WAV"), outputRow);
    form->addRow(inputLabel, engineStatus);
    form->addRow(QStringLiteral("Loopback source"), sourceRow);
    form->addRow(QStringLiteral("Jam Mix options"), includeBackingCheck);
    form->addRow(QString{}, includeMetronomeCheck);
    form->addRow(QString{}, leaderAudioWarning);
    form->addRow(advancedToggle);
    form->addRow(advancedContent);

    QObject::connect(browse, &QPushButton::clicked, this, [this] { chooseCaptureFolder(); });

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* arm = buttons->addButton(QStringLiteral("Arm"), QDialogButtonBox::AcceptRole);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    arm->setDefault(true);

    auto selectLoopbackSource = [this](const QString& id, const QString& name) {
        int index = loopbackSourceBox_->findData(id);
        if (index < 0) index = loopbackSourceBox_->findText(name);
        if (index < 0) index = loopbackSourceBox_->findData(QStringLiteral("default"));
        loopbackSourceBox_->setCurrentIndex(qMax(0, index));
        if (index < 0 && loopbackSourceBox_->isEditable()) loopbackSourceBox_->setEditText(name);
    };
    auto storeDraft = [&](const QString& mode) {
        if (mode != QStringLiteral("loopback")) {
            if (mode == QStringLiteral("current-jam")) {
                currentJamOutput = captureOutputEdit_->text().trimmed();
            } else {
                inputOutput = captureOutputEdit_->text().trimmed();
            }
            inputDraft.recordUntilStopped = captureManualStopCheck_->isChecked();
            inputDraft.durationBars = captureDurationSpin_->value();
            inputDraft.countIn = captureCountInCheck_->isChecked();
            inputDraft.countInBars = captureCountInBarsSpin_->value();
            inputDraft.countInMetronome = captureCountInMetronomeCheck_->isChecked();
            inputDraft.keepMetronome = captureKeepMetronomeCheck_->isChecked();
            inputDraft.latencyAdjustmentFrames = recordingLatencyAdjustmentSpin_->value();
        } else {
            loopbackOutput = captureOutputEdit_->text().trimmed();
            loopbackDraft.sourceId = loopbackSourceBox_->currentData().toString().isEmpty()
                ? loopbackSourceBox_->currentText().trimmed()
                : loopbackSourceBox_->currentData().toString();
            loopbackDraft.sourceName = loopbackSourceBox_->currentText().trimmed();
            loopbackDraft.recordUntilStopped = captureManualStopCheck_->isChecked();
            loopbackDraft.durationBars = captureDurationSpin_->value();
            loopbackDraft.silenceThresholdDb = silenceThresholdSpin_->value();
            loopbackDraft.tailSilenceMs = tailSilenceSpin_->value();
            loopbackDraft.trimLeading = trimLeadingCheck_->isChecked();
            loopbackDraft.trimTrailing = trimTrailingCheck_->isChecked();
        }
    };
    auto loadDraft = [&](const QString& mode) {
        if (mode != QStringLiteral("loopback")) {
            captureOutputEdit_->setText(
                mode == QStringLiteral("current-jam") ? currentJamOutput : inputOutput);
            captureManualStopCheck_->setChecked(inputDraft.recordUntilStopped);
            captureDurationSpin_->setValue(inputDraft.durationBars);
            captureCountInCheck_->setChecked(inputDraft.countIn);
            captureCountInBarsSpin_->setValue(inputDraft.countInBars);
            captureCountInMetronomeCheck_->setChecked(inputDraft.countInMetronome);
            captureKeepMetronomeCheck_->setChecked(inputDraft.keepMetronome);
            recordingLatencyAdjustmentSpin_->setValue(inputDraft.latencyAdjustmentFrames);
        } else {
            captureOutputEdit_->setText(loopbackOutput);
            selectLoopbackSource(loopbackDraft.sourceId, loopbackDraft.sourceName);
            captureManualStopCheck_->setChecked(loopbackDraft.recordUntilStopped);
            captureDurationSpin_->setValue(loopbackDraft.durationBars);
            silenceThresholdSpin_->setValue(loopbackDraft.silenceThresholdDb);
            tailSilenceSpin_->setValue(loopbackDraft.tailSilenceMs);
            trimLeadingCheck_->setChecked(loopbackDraft.trimLeading);
            trimTrailingCheck_->setChecked(loopbackDraft.trimTrailing);
        }
        updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_);
    };
    auto setRowVisible = [form](QWidget* field, bool visible) {
        field->setVisible(visible);
        if (QWidget* label = form->labelForField(field)) label->setVisible(visible);
    };
    auto setAdvancedRowVisible = [advancedForm](QWidget* field, bool visible) {
        field->setVisible(visible);
        if (QWidget* label = advancedForm->labelForField(field)) {
            label->setVisible(visible);
        }
    };
    QString activeMode = modeBox->currentData().toString();
    auto refreshMode = [&] {
        const bool inputMode = activeMode == QStringLiteral("input");
        const bool currentJamMode = activeMode == QStringLiteral("current-jam");
        const bool engineMode = inputMode || currentJamMode;
        const bool loopbackMode = !engineMode;
        const bool advancedAvailable = inputMode || loopbackMode;
        engineStatus->setText(currentJamMode
            ? QStringLiteral(
                "Jam Mix records your local input plus received Jam2 peers. "
                "It does not capture other desktop applications.")
            : jam2_.isRunning()
                ? QStringLiteral("Records only your local Jam2 input.")
                : QStringLiteral("Start Perform or a jam before recording engine input."));
        setRowVisible(engineStatus, engineMode);
        setRowVisible(inputSourceBox, inputMode);
        setRowVisible(sourceRow, !engineMode);
        setRowVisible(includeBackingCheck, currentJamMode);
        setRowVisible(includeMetronomeCheck, currentJamMode);
        setRowVisible(leaderAudioWarning, currentJamMode &&
            metronomeModeBox_ &&
            metronomeModeBox_->currentText() == QStringLiteral("leader-audio"));
        setAdvancedRowVisible(latencyRow, inputMode);
        setAdvancedRowVisible(silenceThresholdSpin_, loopbackMode);
        setAdvancedRowVisible(tailSilenceSpin_, loopbackMode);
        setAdvancedRowVisible(trimLeadingCheck_, loopbackMode);
        setAdvancedRowVisible(trimTrailingCheck_, loopbackMode);
        setRowVisible(advancedToggle, advancedAvailable);
        setRowVisible(
            advancedContent,
            advancedAvailable && advancedToggle->isChecked());
        arm->setEnabled(!engineMode || jam2_.isRunning());
    };
    QObject::connect(advancedToggle, &QPushButton::toggled, &dialog,
        [&](bool open) {
            advancedToggle->setText(open
                ? QStringLiteral("▾  ADVANCED")
                : QStringLiteral("▸  ADVANCED"));
            refreshMode();
        });
    loadDraft(activeMode);
    refreshMode();
    QObject::connect(modeBox, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) {
        storeDraft(activeMode);
        activeMode = modeBox->currentData().toString();
        loadDraft(activeMode);
        advancedToggle->setChecked(false);
        refreshMode();
    });
    QObject::connect(refreshSources, &QPushButton::clicked, this, [&] {
        if (activeMode == QStringLiteral("loopback")) storeDraft(activeMode);
        refreshLoopbackSources();
        if (activeMode == QStringLiteral("loopback")) loadDraft(activeMode);
    });

    const int result = dialog.exec();
    storeDraft(activeMode);
    for (QWidget* widget : widgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result != QDialog::Accepted) {
        return false;
    }
    if (activeMode == QStringLiteral("input")) {
        const qlonglong selectedSource = inputSourceBox->currentData().toLongLong();
        recordingInputSourceSlot_ = selectedSource >= 0
            ? std::optional<std::size_t>(static_cast<std::size_t>(selectedSource))
            : std::nullopt;
        if (auto* sourceRouter = jam2_.inputSourceRouter()) {
            sourceRouter->set_recording_source(recordingInputSourceSlot_.value_or(
                jam2::audio::kCombinedInputSources));
        }
    }
    const TrackRecordingWorkflow::CaptureMode captureMode =
        activeMode == QStringLiteral("loopback")
        ? TrackRecordingWorkflow::CaptureMode::Loopback
        : activeMode == QStringLiteral("current-jam")
            ? TrackRecordingWorkflow::CaptureMode::CurrentJam
            : TrackRecordingWorkflow::CaptureMode::Input;
    const LooperLaneLocation resolved = findLooperLaneLocation(
        looperProject_, bankId, laneId);
    if (!resolved.valid() ||
        viewedBankIndex_ != resolved.bank ||
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
        includeBackingCheck->isChecked(),
        includeMetronomeCheck->isChecked());
    localRecordingTargetBankId_ = looperProject_.banks().at(resolved.bank).id;
    localRecordingTargetLaneId_ = looperProject_.banks().at(resolved.bank)
        .lanes.at(resolved.lane).id;
    localTrackRecordingCountInBars_ = 0;
    publishLocalTrackRecordingState(QStringLiteral("armed"));
    refreshLooperLanes();
    appendLog(QStringLiteral("armed lane recording: bank=%1 lane=%2 mode=%3")
        .arg(looperProject_.banks().at(resolved.bank).id)
        .arg(looperProject_.banks().at(resolved.bank).lanes.at(resolved.lane).name)
        .arg(captureMode == TrackRecordingWorkflow::CaptureMode::Loopback
            ? QStringLiteral("loopback")
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
        appendLog(QStringLiteral("synced lane take could not be finalised: %1")
            .arg(completion.error));
        finishLaneTakeFinalization();
        return;
    }
    registerTransientTrackWav(completion.wavPath);
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
                appendLog(QStringLiteral("recorded lane WAV not importable: ") + result->error);
                finishLaneTakeFinalization();
                return;
            }
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
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
            if (laneIndex < 0) {
                LooperLane recovery;
                recovery.id = QUuid::createUuid()
                    .toString(QUuid::WithoutBraces).toLower();
                recovery.name = QStringLiteral("Recovered recorded take");
                recovery.originKind = QStringLiteral("recorded");
                if (!looperProject_.appendLane(bankIndex, std::move(recovery))) {
                    appendLog(QStringLiteral(
                        "recorded lane target was removed and a recovery lane could not be created"));
                    finishLaneTakeFinalization();
                    return;
                }
                laneIndex = looperProject_.banks().at(bankIndex).lanes.size() - 1;
                appendLog(QStringLiteral(
                    "recorded lane target changed; preserved the WAV in a recovery lane"));
            }
            registerTransientTrackWav(result->stagedPath);
            looperWaveformCache_.remove(result->stagedPath);
            LooperLane& lane = looperProject_.banks()[bankIndex].lanes[laneIndex];
            lane.assetPath = result->stagedPath;
            lane.assetHash = result->sha256;
            lane.sampleRate = result->metadata.sampleRate;
            lane.sampleRateCompatible = true;
            lane.sourceFrames = result->metadata.frames;
            lane.originKind = QStringLiteral("recorded");
            lane.localOnly = keepTakeLocal;
            if (isDefaultEmptyTrackName(lane.name) || lane.name.trimmed().isEmpty()) {
                lane.name = result->displayName;
            }
            lane.startFrame = 0;
            lane.stopFrame = -1;
            lane.loopStartFrame = -1;
            lane.loopEndFrame = -1;
            lane.loopEnabled = false;
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
            appendLog(QStringLiteral("recorded lane import failed: ") + error);
            finishLaneTakeFinalization();
        });
    if (!started) {
        appendLog(QStringLiteral(
            "recorded lane import was deferred because the file worker is busy"));
        finishLaneTakeFinalization();
    }
}

void MainWindow::renameSelectedLooperLane()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int row = selectedLooperLane_;
    const LooperLane& lane = looperProject_.banks().at(viewedBankIndex_).lanes.at(row);
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename lane"), QStringLiteral("Lane name"),
        QLineEdit::Normal, lane.name, &accepted);
    if (accepted && looperProject_.renameLane(viewedBankIndex_, row, name)) {
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
    dialog.exec();
    if (dialog.clickedButton() != removeButton) return;

    lane.assetPath.clear();
    lane.assetHash.clear();
    lane.sampleRate = 0;
    lane.sourceFrames = 0;
    lane.sampleRateCompatible = true;
    lane.stopFrame = -1;
    lane.loopStartFrame = -1;
    lane.loopEndFrame = -1;
    lane.loopEnabled = false;
    lane.referenceKind.clear();
    lane.referenceSourceSignature.clear();
    lane.referenceBpm = 0.0;
    lane.referenceStale = false;
    lane.localOnly = false;
    lane.originKind.clear();
    validatedTrackAssetHashes_.remove(removedHash);

    selectedLooperLane_ = laneIndex;
    discardObsoleteReferenceWavs(QSet<QString>{assetPath});
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();
}

void MainWindow::moveSelectedLooperLane(int delta)
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) {
        return;
    }
    const int from = selectedLooperLane_;
    const int to = from + delta;
    if (looperProject_.moveLane(viewedBankIndex_, from, to)) {
        selectedLooperLane_ = to;
        refreshLooperLanes();
        regeneratePreparedMix();
        syncLooperArrangement();
    }
}

void MainWindow::toggleSelectedLooperLaneMute()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) return;
    LooperLane& lane = looperProject_.banks()[viewedBankIndex_].lanes[selectedLooperLane_];
    lane.muted = !lane.muted;
    refreshLooperLanes();
    regeneratePreparedMix();
}

void MainWindow::toggleSelectedLooperLaneSolo()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) return;
    LooperLane& lane = looperProject_.banks()[viewedBankIndex_].lanes[selectedLooperLane_];

    lane.solo = !lane.solo;
    refreshLooperLanes();
    regeneratePreparedMix();
}

void MainWindow::setSelectedLooperLaneGain()
{
    if (sharedRecordingProtected()) return;
    if (selectedLooperLane_ < 0) return;
    LooperLane& lane = looperProject_.banks()[viewedBankIndex_].lanes[selectedLooperLane_];
    bool accepted = false;
    const double gain = QInputDialog::getDouble(this, QStringLiteral("Lane gain"), QStringLiteral("Gain (dB)"), lane.gainDb, -60.0, 12.0, 1, &accepted);
    if (!accepted) return;
    lane.gainDb = gain;
    refreshLooperLanes();
    regeneratePreparedMix();
}

void MainWindow::editSelectedLooperLaneRegion()
{
    if (selectedLooperLane_ < 0) {
        return;
    }
    LooperLane& lane = looperProject_.banks()[viewedBankIndex_].lanes[selectedLooperLane_];
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
    if (jamSyncPolicy_.trackLanes && jam2_.isRunning() &&
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
            if (!folder.mkpath(QStringLiteral("imported"))) {
                result->error = QStringLiteral("Could not create the project's imported folder.");
                return;
            }
            QMap<QString, QString> relativePathByHash;
            QSet<QString> usedFileNames;
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
                    const WavMetadata metadata = readWavMetadata(source);
                    if (metadata.sha256 != lane.assetHash) {
                        result->error = QStringLiteral("A lane WAV does not match its content hash: %1").arg(lane.name);
                        return;
                    }
                    QString relativePath = relativePathByHash.value(lane.assetHash);
                    if (relativePath.isEmpty()) {
                        const QString projectPrefix = result->projectFolder + QLatin1Char('/');
                        const QString cleanSource = QDir::cleanPath(QFileInfo(source).absoluteFilePath());
                        if (cleanSource.startsWith(projectPrefix, Qt::CaseInsensitive)) {
                            relativePath = QDir(result->projectFolder).relativeFilePath(cleanSource);
                            relativePathByHash.insert(lane.assetHash, relativePath);
                            lane.assetPath = relativePath;
                            continue;
                        }
                        const QString base = portableFileStem(lane.name, QStringLiteral("Track"));
                        QString fileName = base + QStringLiteral(".wav");
                        int suffix = 2;
                        while (usedFileNames.contains(fileName.toLower())) {
                            fileName = QStringLiteral("%1-%2.wav").arg(base).arg(suffix++);
                        }
                        usedFileNames.insert(fileName.toLower());
                        relativePath = QStringLiteral("imported/") + fileName;
                        const QString destination = folder.absoluteFilePath(relativePath);
                        if (QDir::cleanPath(source) != QDir::cleanPath(destination)) {
                            QFile input(source);
                            QSaveFile output(destination);
                            if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
                                result->error = QStringLiteral("Could not open WAV %1 for saving.").arg(lane.name);
                                return;
                            }
                            while (!input.atEnd()) {
                                const QByteArray block = input.read(1024 * 1024);
                                if ((block.isEmpty() && input.error() != QFileDevice::NoError) ||
                                    output.write(block) != block.size()) {
                                    result->error = QStringLiteral("Could not copy WAV %1 while saving.").arg(lane.name);
                                    return;
                                }
                            }
                            if (!output.commit()) {
                                result->error = QStringLiteral("Could not atomically save WAV %1.").arg(lane.name);
                                return;
                            }
                        }
                        relativePathByHash.insert(lane.assetHash, relativePath);
                    }
                    lane.assetPath = relativePath;
                }
            }
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
        QMessageBox::warning(this, QStringLiteral("Save JamJar"), result->error);
        return false;
    }
    if (currentProjectSnapshot() != sourceSnapshot) {
        QMessageBox::warning(this, QStringLiteral("Save JamJar"), QStringLiteral("The project changed while its WAVs were being verified; Save again."));
        return false;
    }
    looperProject_ = std::move(result->project);
    projectPersistence_.setProjectFolder(result->projectFolder);
    return true;
}

void MainWindow::regeneratePreparedMix(int bankIndex)
{
    const int requestedBank = bankIndex < 0 ? viewedBankIndex_ : bankIndex;
    const int targetBank = qBound(0, requestedBank, looperProject_.banks().size() - 1);
    (void)extendSectionToFitTracks(targetBank);
    ++preparedMixRequests_;
    const std::uint64_t generation = ++preparedMixRevision_;
    if (preparedMixWorkerRunning_) {
        preparedMixRerunPending_ = true;
        preparedMixRerunBank_ = sharedBankSwitchIndex_ >= 0
            ? sharedBankSwitchIndex_ : targetBank;
        ++preparedMixCoalesced_;
        return;
    }
    if (!PreparedMixRenderer::hasRenderableSources(looperProject_, targetBank)) {
        preparedMixByBank_[static_cast<std::size_t>(targetBank)] = {};
        playPreparedMixWhenReady_ = false;
        if (targetBank == looperProject_.activeBankIndex()) {
            discardPreparedMix(false);
        }
        if (targetBank == sharedBankSwitchIndex_) noteSharedBankReady(targetBank);
        return;
    }

    const int sampleRate = activeTrackSampleRate();
    const QString cachePath = PreparedMixRenderer::outputPath(
        projectPersistence_.workspaceFolder(),
        targetBank,
        generation,
        QCoreApplication::applicationPid());
    const LooperProject project = looperProject_;
    const QString projectFolder = projectPersistence_.projectFolder();
    const SharedTrackModel track = trackController_.model();
    const qint64 exactFrames = bankExactOutputFrames(targetBank, sampleRate);
    QPointer<MainWindow> self(this);
    preparedMixWorkerRunning_ = true;
    fileWorkerPool_.start(QRunnable::create([
        self,
        project,
        projectFolder,
        sampleRate,
        cachePath,
        track,
        targetBank,
        exactFrames,
        generation
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
            self->preparedMixWorkerRunning_ = false;
            self->retryObsoleteReferenceWavs();
            if (self->preparedMixRerunPending_) {
                self->preparedMixRerunPending_ = false;
                const int rerunBank = self->preparedMixRerunBank_;
                self->preparedMixRerunBank_ = -1;
                if (!result.path.isEmpty()) {
                    (void)QFile::remove(result.path);
                }
                self->regeneratePreparedMix(rerunBank);
                return;
            }
            if (generation != self->preparedMixRevision_) {
                if (!result.path.isEmpty()) {
                    (void)QFile::remove(result.path);
                }
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
    if (!result.error.isEmpty()) {
        ++preparedMixFailures_;
        appendLog(QStringLiteral("prepared mix failed: %1 worker_requests=%2 worker_coalesced=%3 worker_failures=%4")
            .arg(result.error)
            .arg(preparedMixRequests_)
            .arg(preparedMixCoalesced_)
            .arg(preparedMixFailures_));
        playPreparedMixWhenReady_ = false;
        return;
    }
    const int resultBank = qBound(
        0, result.bankIndex, looperProject_.banks().size() - 1);
    preparedMixByBank_[static_cast<std::size_t>(resultBank)] = result;
    registerTransientTrackWav(result.path);
    const bool preparedForSharedBank =
        !sharedBankSwitchId_.isEmpty() && sharedBankSwitchIndex_ == resultBank;
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
    if (!preparedMix_.path.isEmpty() && preparedMix_.path != result.path) {
        obsoletePreparedMixPaths_.insert(preparedMix_.path);
    }
    preparedMix_ = std::move(result);
    registerTransientTrackWav(preparedMix_.path);
    try {
        auto& track = trackController_.model();
        track.fileName = QStringLiteral("Prepared Section %1").arg(looperProject_.banks().at(looperProject_.activeBankIndex()).id);
        track.filePath = preparedMix_.path;
        track.fileBytes = preparedMix_.fileBytes;
        track.sampleRate = preparedMix_.sampleRate;
        track.sampleRateCompatible = true;
        track.userProvidedSource = false;
        track.durationMs = preparedMix_.durationMs;
        track.sha256 = preparedMix_.sha256;
        // The performance playhead derives its relative song beat from this
        // value while a prepared WAV is active.  Refreshing the live bank must
        // update it just as adopting a bank cache does; otherwise a WAV
        // regenerated after a BPM change sounds at the new tempo while the
        // chord/beat display continues advancing at the previous tempo until
        // the user switches banks.
        track.acceptedBpm = bankMetronomePattern(resultBank).bpm;
        updateTrackControls();
        loadTrackWaveform();
        bool attachToRunningTransport =
            playPreparedMixWhenReady_ &&
            trackRecordingWorkflow_.globalTransportRequestedPlaying();
        std::uint64_t attachTargetFrame = 0;
        std::uint64_t attachSourceFrame = 0;
        if (attachToRunningTransport) {
            const PlaybackGrid::Position position = metronomeTransport_.grid().position();
            const std::uint64_t songStart =
                trackRecordingWorkflow_.globalTransportTimelineStartFrame();
            attachTargetFrame = trackRecordingWorkflow_.globalTransportPlaying()
                ? jam2::gui::next_safe_grid_beat_raw_frame(position)
                : songStart;
            if (attachTargetFrame <= position.rawCurrentFrame) {
                attachTargetFrame = jam2::gui::next_safe_grid_beat_raw_frame(position);
            }
            if (attachTargetFrame == 0 || preparedMix_.frames <= 0) {
                attachToRunningTransport = false;
            } else {
                const std::uint64_t elapsed = attachTargetFrame >= songStart
                    ? attachTargetFrame - songStart
                    : 0ULL;
                attachSourceFrame = elapsed %
                    static_cast<std::uint64_t>(preparedMix_.frames);
            }
        }
        loadPreparedMixIntoEngine(
            attachTargetFrame,
            attachSourceFrame,
            attachToRunningTransport);
        for (const QString& obsoletePath : std::as_const(obsoletePreparedMixPaths_)) {
            if (obsoletePath != preparedMix_.path) {
                (void)projectPersistence_.discardTransientWav(obsoletePath);
            }
        }
        obsoletePreparedMixPaths_.clear();
        appendLog(QStringLiteral("prepared mix: %1 frames in %2 ms pre_master_peak=%3 output_peak=%4 master_pre_gain=%5 over_unity_samples=%6 worker_requests=%7 worker_coalesced=%8 worker_failures=%9")
            .arg(preparedMix_.frames)
            .arg(preparedMix_.renderMs)
            .arg(preparedMix_.preMasterPeak, 0, 'f', 4)
            .arg(preparedMix_.outputPeak, 0, 'f', 4)
            .arg(preparedMix_.masterPreGain, 0, 'f', 3)
            .arg(preparedMix_.overUnitySamples)
            .arg(preparedMixRequests_)
            .arg(preparedMixCoalesced_)
            .arg(preparedMixFailures_));
        if (preparedForSharedBank) noteSharedBankReady(resultBank);
        if (trackController_.playback().phase ==
            SharedTrackController::PlaybackPhase::PreparingMix) {
            trackController_.preparedForTransport(
                trackController_.playback().arrangementRevision);
            updateTrackPlaybackPresentation();
        }
        if (playPreparedMixWhenReady_) {
            playPreparedMixWhenReady_ = false;
            if (attachToRunningTransport) {
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
        preparedMix_.error = QString::fromUtf8(error.what());
        ++preparedMixFailures_;
        appendLog(QStringLiteral("prepared mix metadata failed: %1 worker_failures=%2")
            .arg(preparedMix_.error)
            .arg(preparedMixFailures_));
        playPreparedMixWhenReady_ = false;
    }
}

void MainWindow::adoptPreparedBankCache(int bankIndex)
{
    bankIndex = qBound(0, bankIndex, looperProject_.banks().size() - 1);
    const PreparedMixResult& cached =
        preparedMixByBank_[static_cast<std::size_t>(bankIndex)];
    if (cached.path.isEmpty() || !cached.error.isEmpty() ||
        !QFileInfo::exists(cached.path)) {
        preparedMix_ = {};
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

    preparedMix_ = cached;
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
    if (!jam2_.isRunning() || preparedMix_.path.isEmpty() || !preparedMix_.error.isEmpty()) {
        return;
    }
    jam2::EngineCommand load;
    load.type = jam2::EngineCommandType::LoadPreparedTrack;
    load.frame = alignToRunningTransport ? targetFrame : 0ULL;
    load.frame_end = alignToRunningTransport ? sourceFrame : 0ULL;
    load.enabled = alignToRunningTransport;
    if (!jam2::engine_command_set_text(
            load,
            QDir::toNativeSeparators(preparedMix_.path).toStdString())) {
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
        .arg(preparedMix_.frames)
        .arg(preparedMix_.path));
    if (alignToRunningTransport && loadQueued) {
        trackRecordingWorkflow_.notePreparedAttachScheduled(targetFrame);
        appendLog(QStringLiteral(
            "prepared track attach scheduled: target_frame=%1 source_frame=%2")
            .arg(targetFrame)
            .arg(sourceFrame));
    } else if (alignToRunningTransport) {
        trackRecordingWorkflow_.cancelPreparedAttach();
    }
    const auto& model = trackController_.model();
    if (model.loopEnabled) {
        const qint64 requestedLoopStartFrame = model.loopStartSeconds >= 0.0
            ? qMax<qint64>(0, static_cast<qint64>(std::llround(
                model.loopStartSeconds * trackRecordingWorkflow_.preparedSampleRate())))
            : 0;
        const qint64 loopStartFrame = qBound<qint64>(
            0, requestedLoopStartFrame, qMax<qint64>(0, preparedMix_.frames - 1));
        const qint64 requestedLoopEndFrame = model.loopEndSeconds > model.loopStartSeconds
            ? qMax<qint64>(loopStartFrame + 1, static_cast<qint64>(std::llround(
                model.loopEndSeconds * trackRecordingWorkflow_.preparedSampleRate())))
            : preparedMix_.frames;
        const qint64 loopEndFrame = qBound<qint64>(
            loopStartFrame + 1,
            requestedLoopEndFrame,
            qMax<qint64>(loopStartFrame + 1, preparedMix_.frames));
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

void MainWindow::loadTrackMetadata()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Load WAV"),
        jamAssetFolder(JamStorage::AssetKind::Imported),
        QStringLiteral("WAV files (*.wav);;All files (*)"),
        nullptr,
        QFileDialog::Options{});
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
            const int expectedSampleRate = activeTrackSampleRate();
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
            trackController_.model().userProvidedSource = true;
            trackController_.model().durationMs = sidecar->value(QStringLiteral("duration_ms")).toInt(metadata->durationMs);
            trackController_.model().sha256 = metadata->sha256;
            trackController_.model().guessedBpm = 0.0;
            trackController_.model().acceptedBpm = sidecar->value(
                QStringLiteral("accepted_bpm")).toDouble(
                    trackController_.model().acceptedBpm);
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
        isAutoCapturePath(current)
            ? timestampedCapturePath(
                QStringLiteral("take"),
                jamAssetFolder(JamStorage::AssetKind::Recorded))
            : current,
        QStringLiteral("WAV files (*.wav);;All files (*)"),
        nullptr,
        QFileDialog::Options{});
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
    int recordingSampleRate = 0;
    QString rateError;
    if (!recordingTargetSampleRate(recordingSampleRate, rateError)) {
        appendLog(QStringLiteral("could not start input take: ") + rateError);
        QMessageBox::warning(
            this, QStringLiteral("Jam2 Track Recording"), rateError);
        return;
    }
    QString output = captureOutputEdit_->text().trimmed();
    if (isAutoCapturePath(output)) {

        output = timestampedCapturePath(
            QStringLiteral("track-input"),
            jamAssetFolder(JamStorage::AssetKind::Recorded));
        captureOutputEdit_->setText(output);
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
    QString output = captureOutputEdit_->text().trimmed();
    if (isAutoCapturePath(output)) {
        output = timestampedCapturePath(
            QStringLiteral("track-input"),
            jamAssetFolder(JamStorage::AssetKind::Recorded));
        captureOutputEdit_->setText(output);
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
    QString output = captureOutputEdit_->text().trimmed();
    if (isAutoCapturePath(output)) {
        output = timestampedCapturePath(
            QStringLiteral("loopback"),
            jamAssetFolder(JamStorage::AssetKind::Recorded));
        captureOutputEdit_->setText(output);
    }
    trackRecordingWorkflow_.beginLoopbackCapture(
        output, !QFileInfo::exists(output), recordingSampleRate);
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
    options.targetSampleRate = recordingSampleRate;
    options.durationBars = (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked())
        ? captureDurationSpin_->value()
        : 0;
    const auto pattern = currentMetronomePattern();
    options.bpm = pattern.bpm;
    options.beatsPerBar = pattern.beats_per_bar;
    options.tempoPulseUnits = pattern.tempo_pulse_units;
    options.silenceThresholdDb = silenceThresholdSpin_ ? silenceThresholdSpin_->value() : -50.0;
    options.tailSilenceMs = tailSilenceSpin_ ? tailSilenceSpin_->value() : 1000;
    options.trimLeadingSilence = trimLeadingCheck_ && trimLeadingCheck_->isChecked();
    options.trimTrailingSilence = trimTrailingCheck_ && trimTrailingCheck_->isChecked();

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
                const QString transientPath = trackRecordingWorkflow_.finishLoopbackCapture(outputPath);
                if (!diagnostics.isEmpty()) {
                    appendLog(diagnostics);
                }
                if (!ok) {
                    if (!transientPath.isEmpty() && QFileInfo::exists(transientPath)) {
                        registerTransientTrackWav(transientPath);
                    }
                    if (loadWavButton_) loadWavButton_->setEnabled(true);
                    appendLog(QStringLiteral("loopback recording failed: ") + errorText);
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
        const QString transientPath = trackRecordingWorkflow_.abandonPendingCapture();
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
                const bool retainedBankCache = std::any_of(
                    preparedMixByBank_.cbegin(),
                    preparedMixByBank_.cend(),
                    [&path](const PreparedMixResult& cached) {
                        return !cached.path.isEmpty() && cached.path == path;
                    });
                if (!retainedBankCache && path != preparedMix_.path) {
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
        if (!preparedMix_.path.isEmpty()) {
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
    if (preparedMixWorkerRunning_ || preparedMix_.path.isEmpty() || !preparedMix_.error.isEmpty()) {
        playPreparedMixWhenReady_ = true;
        regeneratePreparedMix(activeBank);
        if (!trackRecordingWorkflow_.restartGlobalTransport(
                playPosition,
                false,
                countInBars,
                beatsPerBar)) {
            playPreparedMixWhenReady_ = false;
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
    if (sharedRecordingProtected()) return;
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
    if (sharedRecordingProtected()) return;
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
        !sessionController_.isServer() && metronomeModeBox_ &&
        metronomeModeBox_->currentText() == QStringLiteral("leader-audio");
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
    if (const std::optional<int> bpm = tapTempoTracker_.tap(tapTempoClock_.elapsed())) {
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
        ++preparedMixRevision_;
        preparedMixRerunPending_ = false;
        preparedMixRerunBank_ = -1;
        preparedMixByBank_ = {};
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

void MainWindow::scheduleOutgoingTrackBatchExpiry(const QString& batchId)
{
    QTimer::singleShot(static_cast<int>(kTrackBatchIdleTimeoutMs), this,
        [this, batchId] {
            if (!trackWorkspace_.outgoingTrackSharePendingPeers.contains(batchId)) return;
            const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() -
                trackWorkspace_.outgoingTrackShareLastProgressMs.value(batchId);
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
    QTimer::singleShot(static_cast<int>(kTrackBatchIdleTimeoutMs), this,
        [this, sourcePeerToken, batchId, activityKey] {
            if (!trackWorkspace_.incomingTrackShareLastProgressMs.contains(activityKey)) return;
            const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() -
                trackWorkspace_.incomingTrackShareLastProgressMs.value(activityKey);
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
        retryOrFailIncomingAsset(hash);
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
}

void MainWindow::retryOrFailIncomingAsset(const QString& hash)
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
    if (!stillExpected) return;
    const int attempt = trackWorkspace_.incomingAssetRetryAttempts.value(hash) + 1;
    trackWorkspace_.incomingAssetRetryAttempts[hash] = attempt;
    if (attempt <= 3) {
        const int delayMs = 250 * attempt;
        appendLog(QStringLiteral("retrying looper asset %1 after interruption: attempt=%2")
            .arg(hash.left(8)).arg(attempt));
        QTimer::singleShot(delayMs, this, [this, hash] {
            if (validatedTrackAssetHashes_.contains(hash) ||
                incomingAssetWorkflow_ != IncomingAssetWorkflow::None) return;
            requestNextPendingAsset();
        });
        return;
    }

    trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
    QString failedSource;
    QString failedBatch;
    for (const PendingTrackContribution& contribution : pendingTrackContributions_) {
        if (contribution.assetHash == hash) {
            failedSource = contribution.sourcePeerToken;
            failedBatch = contribution.batchId;
            break;
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
    QStringList removedKeys;
    QSet<QString> removedHashes;
    for (auto it = pendingTrackContributions_.cbegin();
         it != pendingTrackContributions_.cend(); ++it) {
        if (it->sourcePeerToken == sourcePeerToken && it->batchId == batchId) {
            removedKeys.append(it.key());
            removedHashes.insert(it->assetHash);
        }
    }
    if (removedKeys.isEmpty()) return;
    const bool resetActive = incomingAssetWorkflow_ == IncomingAssetWorkflow::TrackContribution &&
        removedHashes.contains(incomingAssetHash_);
    for (const QString& key : removedKeys) pendingTrackContributions_.remove(key);
    for (const QString& hash : removedHashes) {
        pendingTrackAssetSources_.remove(hash);
        trackWorkspace_.incomingAssetRetryAttempts.remove(hash);
    }
    if (resetActive) assetTransfer_.resetIncoming();
    if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
    appendLog(QStringLiteral("expired incomplete Track Sync batch %1 from peer %2")
        .arg(batchId.left(8), sourcePeerToken.left(8)));
    applyPendingTrackContributions();
    requestNextPendingAsset();
}

void MainWindow::applyPendingTrackContributions()
{
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
            if (!automaticWavSharingEnabled()) {
                QTimer::singleShot(0, this, [this] { publishLocalTrackBatch({}); });
            }
        }
    }
    sendControlTo(sourcePeerToken, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("looper.track.batch.complete")},
        {QStringLiteral("batch_id"), batchId},
        {QStringLiteral("tracks"), expected},
    });
    if (performanceHome_) performanceHome_->setTrackTransferStatus(QString{});
    requestNextPendingAsset();
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
    const QJsonObject normalizedIncomingSong =
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
                const bool matches = !pendingRecordingHash.isEmpty()
                    ? lane.value(QStringLiteral("asset_hash")).toString() ==
                        pendingRecordingHash
                    : lane.value(QStringLiteral("id")).toString() ==
                        pendingRecordingId;
                if (matches) {
                    authoritativeContainsPendingRecording = true;
                    break;
                }
            }
            if (authoritativeContainsPendingRecording) break;
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
                assetTransfer_.resetIncoming();
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
                playPreparedMixWhenReady_ = restartTrack;
                trackController_.prepareMix(
                    static_cast<quint64>(revision), restartTrack);
                updateTrackPlaybackPresentation();
                regeneratePreparedMix();
                if (fromPeerProposal) {
                    appendLog(QStringLiteral("accepted collaborative arrangement edit from peer %1")
                        .arg(sourcePeerToken.left(8)));
                    sendSongSnapshot(restartTrack, songScope);
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
        playPreparedMixWhenReady_ = restartTrack;
        trackController_.prepareMix(static_cast<quint64>(revision), restartTrack);
        updateTrackPlaybackPresentation();
        regeneratePreparedMix();
        if (publishAuthoritative) {
            appendLog(QStringLiteral("accepted collaborative arrangement edit from peer %1 after asset sync")
                .arg(sourcePeerToken.left(8)));
            sendSongSnapshot(restartTrack);
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
        sampleRateSpin_ ? sampleRateSpin_->value() : 48000);
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
        sampleRateSpin_ ? sampleRateSpin_->value() : 48000);
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
    std::unique_ptr<jam2::pluginhost::PluginHostService> host)
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
#ifdef Q_OS_MACOS
    const QString pluginPath = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select VST3 plugin"),
        QDir::homePath() + QStringLiteral("/Library/Audio/Plug-Ins/VST3"));
#else
    const QString pluginPath = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select VST3 plugin"),
        QStringLiteral("C:/Program Files/Common Files/VST3"),
        QStringLiteral("VST3 plugins (*.vst3)"));
#endif
    if (pluginPath.isEmpty()) return false;
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
    const QString workerPath = jam2::pluginhost::PluginHostService::workerExecutablePath();
    const QString resultPath = QDir::temp().absoluteFilePath(
        QStringLiteral("jam2-vst3-probe-%1.txt")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    if (progress) progress(-1, QStringLiteral("Scanning VST3 in an isolated process…"));

    struct ScanOutcome {
        QList<QStringList> classes;
        QString error;
    };
    const auto scan = std::make_shared<ScanOutcome>();
    QPointer<MainWindow> self(this);
    QThread* const guiThread = thread();
    fileWorkerPool_.start(QRunnable::create([
        self, pluginPath, resultPath, workerPath, scan,
        kind, midiQueue, blockFrames, sourceChannels, options, completion,
        progress, guiThread
    ]() mutable {
        try {
            constexpr int maximumProbeAttempts = 12;
            for (int attempt = 0; attempt < maximumProbeAttempts &&
                 scan->classes.isEmpty(); ++attempt) {
                (void)QFile::remove(resultPath);
                QProcess probe;
                probe.setProcessChannelMode(QProcess::ForwardedChannels);
                probe.setInputChannelMode(QProcess::ForwardedInputChannel);
                probe.start(workerPath,
                    {QStringLiteral("--probe-file"), pluginPath, resultPath});
                if (!probe.waitForStarted(5000) || !probe.waitForFinished(30000) ||
                    probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
                    if (attempt + 1 == maximumProbeAttempts) {
                        scan->error = QStringLiteral(
                            "The isolated scanner could not load this VST3 plugin after "
                            "12 bounded attempts. Jam2 was not exposed to the plugin.");
                    }
                    continue;
                }
                QFile result(resultPath);
                if (!result.open(QIODevice::ReadOnly)) {
                    scan->error = QStringLiteral("Could not read the private plugin scan result.");
                    break;
                }
                const QList<QByteArray> lines = result.readAll().split('\n');
                for (const QByteArray& line : lines) {
                    const QList<QByteArray> fields = line.trimmed().split('\t');
                    if (fields.size() < 2) continue;
                    QStringList values;
                    for (const auto& field : fields)
                        values.push_back(QString::fromUtf8(field));
                    scan->classes.push_back(values);
                }
            }
            if (scan->classes.isEmpty() && scan->error.isEmpty())
                scan->error = QStringLiteral(
                    "No VST3 audio or instrument class was found after 12 isolated attempts.");
        } catch (const std::exception& error) {
            scan->error = QString::fromUtf8(error.what());
        } catch (...) {
            scan->error = QStringLiteral("Unknown isolated VST3 scan failure.");
        }
        (void)QFile::remove(resultPath);
        if (self.isNull()) return;
        QMetaObject::invokeMethod(self, [
            self, pluginPath, scan, kind, midiQueue, blockFrames,
            sourceChannels, options, completion, progress, guiThread
        ]() mutable {
            if (self.isNull()) return;
            if (!scan->error.isEmpty()) {
                if (progress) progress(0, scan->error);
                else QMessageBox::warning(self, QStringLiteral("Input plugin"), scan->error);
                return;
            }
            QStringList classNames;
            for (const auto& pluginClass : scan->classes)
                classNames.push_back(pluginClass[1]);
            int selected = 0;
            if (scan->classes.size() > 1) {
                bool accepted = false;
                const QString choice = QInputDialog::getItem(self,
                    QStringLiteral("Plugin class"), QStringLiteral("Class"),
                    classNames, 0, false, &accepted);
                if (!accepted) {
                    if (progress) progress(0, QStringLiteral("Plugin selection cancelled."));
                    return;
                }
                selected = classNames.indexOf(choice);
            }
            if (progress) progress(-1, QStringLiteral("Starting isolated plugin worker…"));

            struct StartOutcome {
                std::unique_ptr<jam2::pluginhost::PluginHostService> host;
                QString error;
            };
            const auto started = std::make_shared<StartOutcome>();
            const QStringList selectedClass = scan->classes[selected];
            self->fileWorkerPool_.start(QRunnable::create([
                self, pluginPath, selectedClass, kind, midiQueue,
                blockFrames, sourceChannels, options, completion, started,
                progress, guiThread
            ]() mutable {
                try {
                    started->host = std::make_unique<jam2::pluginhost::PluginHostService>();
                    started->host->start(pluginPath.toStdString(),
                        selectedClass[0].toStdString(),
                        static_cast<double>(options.sample_rate), blockFrames,
                        kind, sourceChannels);
                    if (midiQueue && started->host->bridge())
                        started->host->bridge()->set_midi_queue(midiQueue);
                    started->host->moveProcessToThread(guiThread);
                } catch (const std::exception& error) {
                    started->error = QString::fromUtf8(error.what());
                } catch (...) {
                    started->error = QStringLiteral("Unknown isolated plugin worker failure.");
                }
                if (self.isNull()) return;
                QMetaObject::invokeMethod(self, [
                    self, selectedClass, completion, started, progress
                ]() mutable {
                    if (self.isNull()) return;
                    if (!started->error.isEmpty()) {
                        const QString message = QStringLiteral(
                            "The isolated plugin worker rejected the plugin: %1")
                                .arg(started->error);
                        if (progress) progress(0, message);
                        else QMessageBox::warning(self, QStringLiteral("Input plugin"), message);
                        return;
                    }
                    if (progress) progress(100, QStringLiteral("%1 loaded. You can open its interface now.")
                        .arg(selectedClass[1]));
                    completion(std::move(started->host), selectedClass[1]);
                }, Qt::QueuedConnection);
            }));
        }, Qt::QueuedConnection);
    }));
    return true;
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
        configuration.renderer = source.host ? source.host->bridge() : nullptr;
        configuration.level_ppm = source.levelPpm;
        configuration.enabled = source.included;
        (void)router->configure(slot, configuration);
    }
    for (auto& source : midiPluginSources_) {
        if (!source || source->routerSlot >= jam2::audio::kMaximumInputSources) continue;
        if (!source->host || !source->host->bridge()) {
            router->clear(source->routerSlot);
            continue;
        }
        jam2::audio::InputSourceConfiguration configuration;
        configuration.kind = jam2::audio::InputSourceKind::MidiInstrument;
        configuration.renderer = source->host ? source->host->bridge() : nullptr;
        configuration.level_ppm = source->levelPpm;
        configuration.enabled = source->included;
        if (source->host && source->host->bridge())
            source->host->bridge()->set_muted(source->muted);
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
    auto* router = jam2_.inputSourceRouter();
    if (!router) {
        QMessageBox::information(this, QStringLiteral("Audio inputs"),
            QStringLiteral("Start the local audio engine to view its selected input channels."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Audio inputs"));
    dialog.setMinimumSize(900, 610);
    dialog.setProperty("jam2MaximumDialogHeight", 760);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    const bool topologyLocked = trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();
    auto* intro = new QLabel(QStringLiteral(
        "These are the input channels selected when the audio engine opened. "
        "Choose which sources enter My Send, set their mix, or combine any two "
        "available mono inputs by assigning an explicit left and right channel."), content);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    const auto options = runtimeOptions();
    const auto inputName = [&options](std::size_t channel) {
        const int number = channel < options.channel_selection.input.size()
            ? options.channel_selection.input[channel] + 1
            : static_cast<int>(channel + 1);
        return QStringLiteral("Input %1").arg(number);
    };

    auto* sourcePanels = new QWidget(content);
    auto* sourcePanelsLayout = new QVBoxLayout(sourcePanels);
    sourcePanelsLayout->setContentsMargins(0, 0, 0, 0);
    sourcePanelsLayout->setSpacing(14);
    layout->addWidget(sourcePanels);

    std::function<void()> rebuildSourcePanels;
    rebuildSourcePanels = [this, &dialog, router, sourcePanels,
        sourcePanelsLayout, topologyLocked, inputName, &rebuildSourcePanels] {
        while (QLayoutItem* item = sourcePanelsLayout->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }

        auto* inputs = new QGroupBox(
            QStringLiteral("Inputs selected for this engine"), sourcePanels);
        auto* inputsLayout = new QVBoxLayout(inputs);
        inputsLayout->setSpacing(10);
        int visibleSources = 0;
        for (std::size_t slot = 0; slot < router->physical_channels() &&
             slot < audioPluginSources_.size(); ++slot) {
            auto& source = audioPluginSources_[slot];
            if (source.firstChannel == jam2::audio::kNoInputChannel) continue;
            ++visibleSources;
            const bool stereo = source.secondChannel != jam2::audio::kNoInputChannel;
            const QString title = stereo
                ? QStringLiteral("Stereo  ·  %1 left  +  %2 right")
                    .arg(inputName(source.firstChannel), inputName(source.secondChannel))
                : QStringLiteral("Mono  ·  %1").arg(inputName(source.firstChannel));
            auto* tile = new QGroupBox(title, inputs);
            auto* tileLayout = new QVBoxLayout(tile);
            auto* description = new QLabel(stereo
                ? QStringLiteral("This pair is treated as one source. Its left/right layout is preserved for a stereo-capable plugin, then mixed to mono for My Send.")
                : QStringLiteral("This engine-selected channel is treated as one mono source."), tile);
            description->setWordWrap(true);
            tileLayout->addWidget(description);
            auto* controls = new QHBoxLayout();
            auto* include = new QCheckBox(QStringLiteral("Send to Jam"), tile);
            include->setChecked(source.included);
            controls->addWidget(include);
            QObject::connect(include, &QCheckBox::toggled, &dialog,
                [this, slot](bool value) {
                    audioPluginSources_[slot].included = value;
                    if (auto* current = jam2_.inputSourceRouter())
                        (void)current->set_enabled(slot, value);
                });
            controls->addSpacing(16);
            controls->addWidget(new QLabel(QStringLiteral("Send Mix"), tile));
            auto* level = new QSpinBox(tile);
            level->setRange(0, 200);
            level->setSuffix(QStringLiteral("%"));
            level->setValue(source.levelPpm / 10000);
            level->setToolTip(QStringLiteral("Source level before the mono My Send mix"));
            controls->addWidget(level);
            QObject::connect(level, qOverload<int>(&QSpinBox::valueChanged), &dialog,
                [this, slot](int percent) {
                    audioPluginSources_[slot].levelPpm = percent * 10000;
                    if (auto* current = jam2_.inputSourceRouter())
                        (void)current->set_level(slot, percent * 10000);
                });
            controls->addStretch(1);
            if (stereo) {
                auto* ungroup = new QPushButton(
                    QStringLiteral("Ungroup to Mono Inputs"), tile);
                ungroup->setEnabled(!topologyLocked);
                controls->addWidget(ungroup);
                QObject::connect(ungroup, &QPushButton::clicked, &dialog,
                    [this, &dialog, slot, &rebuildSourcePanels] {
                        auto& grouped = audioPluginSources_[slot];
                        if (grouped.host) {
                            const auto answer = QMessageBox::question(&dialog,
                                QStringLiteral("Ungroup stereo input"),
                                QStringLiteral(
                                    "Ungrouping changes the plugin input layout and removes "
                                    "the plugin currently loaded on this pair. Continue?"),
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel);
                            if (answer != QMessageBox::Yes) return;
                            removeAudioPlugin(slot);
                        }
                        const std::size_t restored = grouped.secondChannel;
                        grouped.secondChannel = jam2::audio::kNoInputChannel;
                        if (restored < audioPluginSources_.size()) {
                            audioPluginSources_[restored].firstChannel = restored;
                            audioPluginSources_[restored].consumedByStereoGroup = false;
                        }
                        attachedInputRouter_ = nullptr;
                        refreshInputSourceRouting();
                        rebuildSourcePanels();
                    });
            }
            tileLayout->addLayout(controls);
            inputsLayout->addWidget(tile);
        }
        if (visibleSources == 0) {
            inputsLayout->addWidget(new QLabel(QStringLiteral(
                "No input channels were selected when the engine opened."), inputs));
        }
        sourcePanelsLayout->addWidget(inputs);

        auto* grouping = new QGroupBox(
            QStringLiteral("Create a stereo pair"), sourcePanels);
        auto* groupingLayout = new QVBoxLayout(grouping);
        auto* groupingHelp = new QLabel(QStringLiteral(
            "Assign the physical channel that carries the left side and the channel "
            "that carries the right side. The pair becomes one source and is mixed "
            "to Jam2's mono send after plugin processing."), grouping);
        groupingHelp->setWordWrap(true);
        groupingLayout->addWidget(groupingHelp);
        auto* channelRow = new QHBoxLayout();
        auto* leftChannel = new QComboBox(grouping);
        auto* rightChannel = new QComboBox(grouping);
        std::size_t availableCount = 0;
        for (std::size_t index = 0;
             index < router->physical_channels() &&
             index < audioPluginSources_.size(); ++index) {
            const auto& candidate = audioPluginSources_[index];
            if (candidate.firstChannel != index ||
                candidate.secondChannel != jam2::audio::kNoInputChannel ||
                candidate.consumedByStereoGroup) continue;
            ++availableCount;
            leftChannel->addItem(inputName(index), static_cast<qulonglong>(index));
            rightChannel->addItem(inputName(index), static_cast<qulonglong>(index));
        }
        if (rightChannel->count() > 1) rightChannel->setCurrentIndex(1);
        channelRow->addWidget(new QLabel(QStringLiteral("Left"), grouping));
        channelRow->addWidget(leftChannel, 1);
        channelRow->addSpacing(12);
        channelRow->addWidget(new QLabel(QStringLiteral("Right"), grouping));
        channelRow->addWidget(rightChannel, 1);
        auto* createPair = new QPushButton(
            QStringLiteral("Create Stereo Pair"), grouping);
        channelRow->addWidget(createPair);
        groupingLayout->addLayout(channelRow);
        const auto updateCreatePair = [=] {
            createPair->setEnabled(!topologyLocked && availableCount >= 2 &&
                leftChannel->currentData() != rightChannel->currentData());
        };
        QObject::connect(leftChannel, qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog, [=](int) { updateCreatePair(); });
        QObject::connect(rightChannel, qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog, [=](int) { updateCreatePair(); });
        updateCreatePair();
        if (topologyLocked) {
            groupingHelp->setText(groupingHelp->text() + QStringLiteral(
                " Input grouping is locked while a recording is armed or active."));
        } else if (availableCount < 2) {
            groupingHelp->setText(groupingHelp->text() + QStringLiteral(
                " Ungroup an existing pair to make two mono inputs available."));
        }
        QObject::connect(createPair, &QPushButton::clicked, &dialog,
            [this, &dialog, leftChannel, rightChannel, &rebuildSourcePanels] {
                const std::size_t left = static_cast<std::size_t>(
                    leftChannel->currentData().toULongLong());
                const std::size_t right = static_cast<std::size_t>(
                    rightChannel->currentData().toULongLong());
                if (left == right || left >= audioPluginSources_.size() ||
                    right >= audioPluginSources_.size()) return;
                if (audioPluginSources_[left].host || audioPluginSources_[right].host) {
                    const auto answer = QMessageBox::question(&dialog,
                        QStringLiteral("Create stereo pair"),
                        QStringLiteral(
                            "Creating this pair changes both input sources and removes "
                            "their currently loaded plugins. Continue?"),
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel);
                    if (answer != QMessageBox::Yes) return;
                }
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
                rebuildSourcePanels();
            });
        sourcePanelsLayout->addWidget(grouping);
    };
    rebuildSourcePanels();
    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* footer = new QWidget(&dialog);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    footerLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(footer);
    dialog.exec();
    updateInputSourceButtons();
}

void MainWindow::showMidiInputSources()
{
    refreshInputSourceRouting();
    auto* router = jam2_.inputSourceRouter();
    if (!router) {
        QMessageBox::information(this, QStringLiteral("MIDI inputs"),
            QStringLiteral("Start the local audio engine before assigning a MIDI device."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("MIDI inputs"));
    dialog.setMinimumSize(860, 580);
    dialog.setProperty("jam2MaximumDialogHeight", 740);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    const bool topologyLocked = trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();
    auto* intro = new QLabel(QStringLiteral(
        "Assign local MIDI controllers and choose how each one is interpreted. "
        "Standard MIDI and MPE messages stay local; the Plugins panel attaches "
        "an instrument and Jam2 sends only that instrument's mono audio output."), content);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* devices = new QGroupBox(QStringLiteral("Devices"), content);
    auto* devicesLayout = new QVBoxLayout(devices);
    devicesLayout->setSpacing(10);
    auto* empty = new QLabel(QStringLiteral(
        "No MIDI devices are assigned. Add a controller, choose Standard MIDI "
        "or MPE, then attach its instrument in Plugins."), devices);
    empty->setWordWrap(true);
    empty->setVisible(midiPluginSources_.empty());
    devicesLayout->addWidget(empty);
    QPushButton* add = nullptr;
    std::function<void(const std::shared_ptr<MidiPluginSource>&)> addDeviceTile;
    addDeviceTile = [this, &dialog, devices, devicesLayout, empty, topologyLocked,
        &add](
        const std::shared_ptr<MidiPluginSource>& source) {
        if (!source) return;
        auto* tile = new QGroupBox(
            QString::fromStdString(source->deviceInfo.name), devices);
        auto* tileLayout = new QVBoxLayout(tile);
        auto* status = new QLabel(source->host && source->device
            ? QStringLiteral("Device open · %1")
                .arg(source->pluginName.isEmpty() ? QStringLiteral("instrument active") : source->pluginName)
            : QStringLiteral("Assigned · choose an instrument from Plugins to open this device"), tile);
        status->setWordWrap(true);
        tileLayout->addWidget(status);
        auto* controls = new QHBoxLayout();
        controls->addWidget(new QLabel(QStringLiteral("Mode"), tile));
        auto* mode = new QComboBox(tile);
        mode->addItem(QStringLiteral("Standard MIDI"), static_cast<int>(jam2::midi::InputMode::Standard));
        mode->addItem(QStringLiteral("MPE"), static_cast<int>(jam2::midi::InputMode::Mpe));
        mode->setCurrentIndex(qMax(0, mode->findData(static_cast<int>(source->mode))));
        mode->setEnabled(!topologyLocked);
        controls->addWidget(mode);
        QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog, [source, mode](int) {
                source->mode = static_cast<jam2::midi::InputMode>(mode->currentData().toInt());
                if (source->host && source->host->bridge())
                    source->host->bridge()->request_midi_reset();
            });
        controls->addSpacing(16);
        auto* include = new QCheckBox(QStringLiteral("Send instrument audio to Jam"), tile);
        include->setChecked(source->included);
        include->setToolTip(QStringLiteral(
            "Controls whether audio rendered by this device's instrument enters My Send."));
        controls->addWidget(include);
        QObject::connect(include, &QCheckBox::toggled, &dialog, [this, source](bool value) {
            source->included = value;
            if (auto* current = jam2_.inputSourceRouter())
                (void)current->set_enabled(source->routerSlot, value);
        });
        controls->addSpacing(16);
        auto* levelLabel = new QLabel(QStringLiteral("Instrument Audio Level"), tile);
        levelLabel->setToolTip(QStringLiteral(
            "Sets how much audio rendered by the instrument contributes to My Send. "
            "The MIDI device itself does not produce audio."));
        controls->addWidget(levelLabel);
        auto* level = new QSpinBox(tile);
        level->setRange(0, 200);
        level->setSuffix(QStringLiteral("%"));
        level->setValue(source->levelPpm / 10000);
        level->setToolTip(levelLabel->toolTip());
        controls->addWidget(level);
        QObject::connect(level, qOverload<int>(&QSpinBox::valueChanged), &dialog,
            [this, source](int percent) {
                source->levelPpm = percent * 10000;
                if (auto* current = jam2_.inputSourceRouter())
                    (void)current->set_level(source->routerSlot, percent * 10000);
            });
        controls->addStretch(1);
        auto* remove = new QPushButton(QStringLiteral("Remove Device"), tile);
        remove->setObjectName(QStringLiteral("PluginRemoveAction"));
        remove->setEnabled(!topologyLocked);
        controls->addWidget(remove);
        QObject::connect(remove, &QPushButton::clicked, &dialog,
            [this, source, tile, empty] {
            if (auto* current = jam2_.inputSourceRouter()) current->clear(source->routerSlot);
            if (source->host && source->host->bridge()) {
                source->host->bridge()->request_midi_reset();
                source->host->bridge()->set_midi_queue(nullptr);
            }
            source->device.reset();
            if (source->host) source->host->requestRetire();
            retiredMidiSources_.push_back(source);
            const auto found = std::find(midiPluginSources_.begin(),
                midiPluginSources_.end(), source);
            if (found != midiPluginSources_.end()) midiPluginSources_.erase(found);
            attachedInputRouter_ = nullptr;
            refreshInputSourceRouting();
            tile->deleteLater();
            empty->setVisible(midiPluginSources_.empty());
        });
        tileLayout->addLayout(controls);
        if (add) devicesLayout->insertWidget(devicesLayout->indexOf(add), tile);
        else devicesLayout->addWidget(tile);
        empty->hide();
    };
    for (const auto& source : midiPluginSources_) addDeviceTile(source);
    add = new QPushButton(QStringLiteral("Add MIDI Device…"), devices);
    add->setEnabled(!topologyLocked);
    devicesLayout->addWidget(add, 0, Qt::AlignLeft);
    layout->addWidget(devices);
    layout->addStretch(1);
    QObject::connect(add, &QPushButton::clicked, &dialog,
        [this, &dialog, addDeviceTile] {
        auto* discovery = new QProgressDialog(
            QStringLiteral("Finding MIDI input devices..."),
            QStringLiteral("Cancel"), 0, 0, this);
        discovery->setWindowTitle(QStringLiteral("MIDI input"));
        discovery->setWindowModality(Qt::WindowModal);
        discovery->setMinimumDuration(0);
        discovery->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(discovery, &QProgressDialog::canceled,
            discovery, &QObject::deleteLater);
        discovery->show();

        struct MidiDiscoveryOutcome {
            std::vector<jam2::midi::DeviceInfo> devices;
            QString error;
        };
        const auto outcome = std::make_shared<MidiDiscoveryOutcome>();
        QPointer<MainWindow> self(this);
        QPointer<QDialog> dialogGuard(&dialog);
        QPointer<QProgressDialog> discoveryGuard(discovery);
        fileWorkerPool_.start(QRunnable::create([
            self, dialogGuard, discoveryGuard, outcome, addDeviceTile
        ] {
            try {
                outcome->devices = jam2::midi::enumerate_input_devices();
            } catch (const std::exception& error) {
                outcome->error = QString::fromUtf8(error.what());
            } catch (...) {
                outcome->error = QStringLiteral("Unknown MIDI device discovery failure.");
            }
            if (self.isNull()) return;
            QMetaObject::invokeMethod(self, [
                self, dialogGuard, discoveryGuard, outcome, addDeviceTile
            ]() mutable {
                if (self.isNull() || discoveryGuard.isNull()) return;
                discoveryGuard->close();
                QWidget* parent = dialogGuard.isNull()
                    ? static_cast<QWidget*>(self.data())
                    : static_cast<QWidget*>(dialogGuard.data());
                if (!outcome->error.isEmpty()) {
                    QMessageBox::warning(parent, QStringLiteral("MIDI input"),
                        QStringLiteral("Could not enumerate MIDI inputs: %1")
                            .arg(outcome->error));
                    return;
                }
                if (outcome->devices.empty()) {
                    QMessageBox::information(parent, QStringLiteral("MIDI inputs"),
                        QStringLiteral("No MIDI input devices are currently available."));
                    return;
                }
                std::vector<jam2::midi::DeviceInfo> available;
                for (const auto& device : outcome->devices) {
                    bool assigned = false;
                    for (const auto& existing : self->midiPluginSources_) {
                        if (existing && existing->deviceInfo.id == device.id) {
                            assigned = true;
                            break;
                        }
                    }
                    if (!assigned) available.push_back(device);
                }
                if (available.empty()) {
                    QMessageBox::information(parent, QStringLiteral("MIDI inputs"),
                        QStringLiteral("Every available MIDI input is already assigned."));
                    return;
                }

                QDialog configuration(parent);
                configuration.setWindowTitle(QStringLiteral("Add MIDI device"));
                configuration.setMinimumSize(700, 500);
                auto* configurationLayout = new QVBoxLayout(&configuration);
                auto* configureIntro = new QLabel(QStringLiteral(
                    "Choose one controller and how Jam2 should interpret its channel messages. "
                    "The device will open when an instrument is attached in PLUGINS."),
                    &configuration);
                configureIntro->setWordWrap(true);
                configurationLayout->addWidget(configureIntro);
                auto* deviceBox = new QGroupBox(QStringLiteral("Available devices"), &configuration);
                auto* deviceLayout = new QVBoxLayout(deviceBox);
                auto* deviceList = new QListWidget(deviceBox);
                for (const auto& device : available)
                    deviceList->addItem(QString::fromStdString(device.name));
                deviceList->setCurrentRow(0);
                deviceLayout->addWidget(deviceList);
                configurationLayout->addWidget(deviceBox, 1);
                auto* modeBox = new QGroupBox(QStringLiteral("Device configuration"), &configuration);
                auto* modeLayout = new QFormLayout(modeBox);
                auto* inputMode = new QComboBox(modeBox);
                inputMode->addItem(QStringLiteral("Standard MIDI"),
                    static_cast<int>(jam2::midi::InputMode::Standard));
                inputMode->addItem(QStringLiteral("MPE"),
                    static_cast<int>(jam2::midi::InputMode::Mpe));
                modeLayout->addRow(QStringLiteral("Message mode"), inputMode);
                auto* modeHelp = new QLabel(QStringLiteral(
                    "MPE preserves per-note channel expression. Standard MIDI keeps "
                    "ordinary channel-voice behaviour."), modeBox);
                modeHelp->setWordWrap(true);
                modeLayout->addRow(modeHelp);
                configurationLayout->addWidget(modeBox);
                auto* configurationButtons = new QDialogButtonBox(
                    QDialogButtonBox::Cancel, &configuration);
                auto* assign = configurationButtons->addButton(
                    QStringLiteral("Add MIDI Device"), QDialogButtonBox::AcceptRole);
                QObject::connect(assign, &QPushButton::clicked,
                    &configuration, &QDialog::accept);
                QObject::connect(configurationButtons, &QDialogButtonBox::rejected,
                    &configuration, &QDialog::reject);
                configurationLayout->addWidget(configurationButtons);
                if (configuration.exec() != QDialog::Accepted ||
                    deviceList->currentRow() < 0) return;

                auto* currentRouter = self->jam2_.inputSourceRouter();
                if (!currentRouter) {
                    QMessageBox::information(parent, QStringLiteral("MIDI input"),
                        QStringLiteral("The audio engine stopped while MIDI inputs were being found."));
                    return;
                }
                std::array<bool, jam2::audio::kMaximumInputSources> used{};
                for (std::size_t audio = 0;
                     audio < currentRouter->physical_channels() && audio < used.size(); ++audio)
                    used[audio] = true;
                for (const auto& existing : self->midiPluginSources_)
                    if (existing && existing->routerSlot < used.size())
                        used[existing->routerSlot] = true;
                std::size_t slot = currentRouter->physical_channels();
                while (slot < used.size() && used[slot]) ++slot;
                if (slot >= jam2::audio::kMaximumInputSources) {
                    QMessageBox::warning(parent, QStringLiteral("MIDI inputs"),
                        QStringLiteral("Jam2's 16 local input-source limit has been reached."));
                    return;
                }

                auto source = std::make_shared<MidiPluginSource>();
                source->deviceInfo = available[static_cast<std::size_t>(deviceList->currentRow())];
                source->mode = static_cast<jam2::midi::InputMode>(
                    inputMode->currentData().toInt());
                source->routerSlot = slot;
                self->midiPluginSources_.push_back(source);
                self->attachedInputRouter_ = nullptr;
                self->refreshInputSourceRouting();
                if (!dialogGuard.isNull()) addDeviceTile(source);
            }, Qt::QueuedConnection);
        }));
    });
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* footer = new QWidget(&dialog);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    footerLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(footer);
    dialog.exec();
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

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Input plugins"));
    dialog.setMinimumSize(980, 680);
    dialog.setProperty("jam2MaximumDialogHeight", 800);
    const bool topologyLocked = trackRecordingWorkflow_.inputTakeActive() ||
        loopbackRecorder_.isRunning() || trackRecordingWorkflow_.laneArmed();

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto* intro = new QLabel(QStringLiteral(
        "Manage the plugin attached to each audio or MIDI source. Audio effects "
        "fall back to latency-aligned dry audio when bypassed; bypassed MIDI "
        "instruments are silent."), content);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* loading = new QGroupBox(QStringLiteral("Plugin loading"), content);
    auto* loadingLayout = new QVBoxLayout(loading);
    auto* loadingStatus = new QLabel(QStringLiteral("Ready"), loading);
    loadingStatus->setWordWrap(true);
    auto* loadingBar = new QProgressBar(loading);
    loadingBar->setObjectName(QStringLiteral("JamTasterProgress"));
    loadingBar->setRange(0, 100);
    loadingBar->setValue(0);
    loadingLayout->addWidget(loadingStatus);
    loadingLayout->addWidget(loadingBar);
    layout->addWidget(loading);

    auto pluginLoadBusy = std::make_shared<bool>(false);
    QPointer<QLabel> loadingStatusGuard(loadingStatus);
    QPointer<QProgressBar> loadingBarGuard(loadingBar);
    const auto progress = [loadingStatusGuard, loadingBarGuard, pluginLoadBusy](
        int percent, const QString& text) {
        *pluginLoadBusy = percent < 0;
        if (loadingStatusGuard) loadingStatusGuard->setText(text);
        if (loadingBarGuard) {
            if (percent < 0) {
                loadingBarGuard->setRange(0, 0);
            } else {
                loadingBarGuard->setRange(0, 100);
                loadingBarGuard->setValue(qBound(0, percent, 100));
            }
        }
    };

    const auto options = runtimeOptions();
    const auto inputName = [&options](std::size_t channel) {
        const int number = channel < options.channel_selection.input.size()
            ? options.channel_selection.input[channel] + 1
            : static_cast<int>(channel + 1);
        return QStringLiteral("Input %1").arg(number);
    };

    auto* audioBox = new QGroupBox(QStringLiteral("Audio input effects"), content);
    auto* audioLayout = new QVBoxLayout(audioBox);
    audioLayout->setSpacing(10);
    int audioSources = 0;
    for (std::size_t slot = 0;
         slot < router->physical_channels() && slot < audioPluginSources_.size(); ++slot) {
        auto& source = audioPluginSources_[slot];
        if (source.firstChannel == jam2::audio::kNoInputChannel) continue;
        ++audioSources;
        const QString sourceName = source.secondChannel == jam2::audio::kNoInputChannel
            ? inputName(source.firstChannel)
            : QStringLiteral("%1 left + %2 right")
                .arg(inputName(source.firstChannel), inputName(source.secondChannel));
        auto* tile = new QGroupBox(sourceName, audioBox);
        auto* tileLayout = new QVBoxLayout(tile);
        auto* pluginStatus = new QLabel(tile);
        pluginStatus->setWordWrap(true);
        if (!source.host) {
            pluginStatus->setText(QStringLiteral("No plugin loaded · This source is dry"));
        } else if (!source.host->healthy()) {
            pluginStatus->setText(QStringLiteral("%1 · worker stopped · latency-aligned dry fallback")
                .arg(source.name));
        } else {
            const auto stats = source.host->bridge()->stats();
            pluginStatus->setText(QStringLiteral(
                "%1 · I/O %2 → %3 → mono · latency %4 + %5 transport frames · "
                "process %6/%7 µs avg/max · misses %8 · concealed %9")
                .arg(source.name)
                .arg(stats.negotiated_input_channels)
                .arg(stats.negotiated_output_channels)
                .arg(stats.worker_latency_frames)
                .arg(stats.isolation_latency_frames)
                .arg(stats.worker_process_average_us)
                .arg(stats.worker_process_max_us)
                .arg(stats.deadline_misses)
                .arg(stats.deadline_concealments));
        }
        tileLayout->addWidget(pluginStatus);
        auto* actions = new QHBoxLayout();
        actions->addStretch(1);
        auto* open = new QPushButton(QStringLiteral("Open"), tile);
        auto* replace = new QPushButton(
            source.host ? QStringLiteral("Replace") : QStringLiteral("Add Plugin"), tile);
        auto* bypass = new QPushButton(QStringLiteral("Bypass"), tile);
        bypass->setObjectName(QStringLiteral("PluginBypassAction"));
        bypass->setCheckable(true);
        bypass->setChecked(source.bypassed);
        auto* remove = new QPushButton(QStringLiteral("Remove"), tile);
        remove->setObjectName(QStringLiteral("PluginRemoveAction"));
        open->setEnabled(source.host != nullptr);
        replace->setEnabled(!topologyLocked);
        bypass->setEnabled(source.host != nullptr);
        remove->setEnabled(source.host != nullptr && !topologyLocked);
        actions->addWidget(open);
        actions->addWidget(replace);
        actions->addWidget(bypass);
        actions->addWidget(remove);
        tileLayout->addLayout(actions);
        audioLayout->addWidget(tile);

        QObject::connect(open, &QPushButton::clicked, &dialog, [this, slot] {
            if (audioPluginSources_[slot].host)
                audioPluginSources_[slot].host->openEditor();
        });
        QObject::connect(bypass, &QPushButton::toggled, &dialog,
            [this, slot](bool value) {
                auto& current = audioPluginSources_[slot];
                current.bypassed = value;
                if (current.host && current.host->bridge())
                    current.host->bridge()->set_bypassed(value);
                updateInputSourceButtons();
            });
        QPointer<QLabel> statusGuard(pluginStatus);
        QPointer<QPushButton> openGuard(open);
        QPointer<QPushButton> replaceGuard(replace);
        QPointer<QPushButton> bypassGuard(bypass);
        QPointer<QPushButton> removeGuard(remove);
        QObject::connect(remove, &QPushButton::clicked, &dialog,
            [this, slot, statusGuard, openGuard, replaceGuard, bypassGuard,
             removeGuard] {
                removeAudioPlugin(slot);
                if (statusGuard) statusGuard->setText(
                    QStringLiteral("No plugin loaded · This source is dry"));
                if (openGuard) openGuard->setEnabled(false);
                if (replaceGuard) replaceGuard->setText(QStringLiteral("Add Plugin"));
                if (bypassGuard) {
                    QSignalBlocker blocker(bypassGuard);
                    bypassGuard->setChecked(false);
                    bypassGuard->setEnabled(false);
                }
                if (removeGuard) removeGuard->setEnabled(false);
            });
        QObject::connect(replace, &QPushButton::clicked, &dialog,
            [this, slot, topologyLocked, progress, pluginLoadBusy, statusGuard,
             openGuard, replaceGuard, bypassGuard, removeGuard] {
                if (*pluginLoadBusy) {
                    progress(-1, QStringLiteral(
                        "Finish the current plugin load before starting another."));
                    return;
                }
                const auto tileProgress = [this, slot, progress, openGuard,
                    replaceGuard, bypassGuard, removeGuard, topologyLocked](
                    int percent, const QString& text) {
                    progress(percent, text);
                    const bool busy = percent < 0;
                    const bool loaded = audioPluginSources_[slot].host != nullptr;
                    if (openGuard) openGuard->setEnabled(!busy && loaded);
                    if (replaceGuard) replaceGuard->setEnabled(!busy && !topologyLocked);
                    if (bypassGuard) bypassGuard->setEnabled(!busy && loaded);
                    if (removeGuard) removeGuard->setEnabled(
                        !busy && loaded && !topologyLocked);
                };
                const std::size_t expectedFirst = audioPluginSources_[slot].firstChannel;
                const std::size_t expectedSecond = audioPluginSources_[slot].secondChannel;
                (void)selectAndStartPluginAsync(slot,
                    jam2::audio::InputSourceKind::Audio, nullptr,
                    [this, slot, progress, statusGuard, openGuard,
                     replaceGuard, bypassGuard, removeGuard, expectedFirst,
                     expectedSecond](
                        std::unique_ptr<jam2::pluginhost::PluginHostService> host,
                        const QString& name) mutable {
                        if (trackRecordingWorkflow_.inputTakeActive() ||
                            loopbackRecorder_.isRunning() ||
                            trackRecordingWorkflow_.laneArmed()) {
                            host->requestRetire();
                            progress(0, QStringLiteral(
                                "The plugin loaded after recording was armed, so it was not attached."));
                            return;
                        }
                        auto& current = audioPluginSources_[slot];
                        if (current.firstChannel != expectedFirst ||
                            current.secondChannel != expectedSecond) {
                            host->requestRetire();
                            progress(0, QStringLiteral(
                                "The input grouping changed while the plugin loaded, so it was not attached."));
                            return;
                        }
                        retirePluginHost(std::move(current.host));
                        current.host = std::move(host);
                        current.name = name;
                        current.bypassed = false;
                        attachedInputRouter_ = nullptr;
                        refreshInputSourceRouting();
                        if (statusGuard) statusGuard->setText(
                            QStringLiteral("%1 loaded · ready to open").arg(name));
                        if (openGuard) openGuard->setEnabled(true);
                        if (replaceGuard) replaceGuard->setText(QStringLiteral("Replace"));
                        if (bypassGuard) {
                            QSignalBlocker blocker(bypassGuard);
                            bypassGuard->setChecked(false);
                            bypassGuard->setEnabled(true);
                        }
                        if (removeGuard) removeGuard->setEnabled(true);
                    }, tileProgress);
            });
    }
    if (audioSources == 0) {
        auto* empty = new QLabel(QStringLiteral(
            "No audio input channels were selected when the engine opened."), audioBox);
        empty->setWordWrap(true);
        audioLayout->addWidget(empty);
    }
    layout->addWidget(audioBox);

    auto* midiBox = new QGroupBox(QStringLiteral("MIDI instruments"), content);
    auto* midiLayout = new QVBoxLayout(midiBox);
    midiLayout->setSpacing(10);
    for (const auto& source : midiPluginSources_) {
        if (!source) continue;
        auto* tile = new QGroupBox(
            QString::fromStdString(source->deviceInfo.name), midiBox);
        auto* tileLayout = new QVBoxLayout(tile);
        auto* pluginStatus = new QLabel(tile);
        pluginStatus->setWordWrap(true);
        if (!source->host) {
            pluginStatus->setText(QStringLiteral("No instrument loaded · MIDI source is silent"));
        } else if (!source->host->healthy()) {
            pluginStatus->setText(QStringLiteral("%1 · worker stopped · instrument silent")
                .arg(source->pluginName));
        } else {
            const auto stats = source->host->bridge()->stats();
            pluginStatus->setText(QStringLiteral(
                "%1 · MIDI → %2 → mono · latency %3 + %4 transport frames · "
                "process %5/%6 µs avg/max · misses %7 · concealed %8 · "
                "queue %9/%10 · high %11 · drops %12")
                .arg(source->pluginName)
                .arg(stats.negotiated_output_channels)
                .arg(stats.worker_latency_frames)
                .arg(stats.isolation_latency_frames)
                .arg(stats.worker_process_average_us)
                .arg(stats.worker_process_max_us)
                .arg(stats.deadline_misses)
                .arg(stats.deadline_concealments)
                .arg(stats.midi_queue_depth)
                .arg(jam2::midi::kEventQueueCapacity)
                .arg(stats.midi_queue_high_water)
                .arg(stats.midi_dropped));
        }
        tileLayout->addWidget(pluginStatus);
        auto* actions = new QHBoxLayout();
        actions->addStretch(1);
        auto* open = new QPushButton(QStringLiteral("Open"), tile);
        auto* replace = new QPushButton(
            source->host ? QStringLiteral("Replace") : QStringLiteral("Add Instrument"), tile);
        auto* bypass = new QPushButton(QStringLiteral("Bypass"), tile);
        bypass->setObjectName(QStringLiteral("PluginBypassAction"));
        bypass->setCheckable(true);
        bypass->setChecked(source->muted);
        auto* remove = new QPushButton(QStringLiteral("Remove"), tile);
        remove->setObjectName(QStringLiteral("PluginRemoveAction"));
        open->setEnabled(source->host != nullptr);
        replace->setEnabled(!topologyLocked);
        bypass->setEnabled(source->host != nullptr);
        remove->setEnabled(source->host != nullptr && !topologyLocked);
        actions->addWidget(open);
        actions->addWidget(replace);
        actions->addWidget(bypass);
        actions->addWidget(remove);
        tileLayout->addLayout(actions);
        midiLayout->addWidget(tile);

        QObject::connect(open, &QPushButton::clicked, &dialog, [source] {
            if (source->host) source->host->openEditor();
        });
        QObject::connect(bypass, &QPushButton::toggled, &dialog,
            [this, source](bool value) {
                source->muted = value;
                if (source->host && source->host->bridge()) {
                    if (value) source->host->bridge()->request_midi_reset();
                    source->host->bridge()->set_muted(value);
                }
                updateInputSourceButtons();
            });
        QPointer<QLabel> statusGuard(pluginStatus);
        QPointer<QPushButton> openGuard(open);
        QPointer<QPushButton> replaceGuard(replace);
        QPointer<QPushButton> bypassGuard(bypass);
        QPointer<QPushButton> removeGuard(remove);
        QObject::connect(remove, &QPushButton::clicked, &dialog,
            [this, source, statusGuard, openGuard, replaceGuard, bypassGuard,
             removeGuard] {
                if (source->host && source->host->bridge()) {
                    source->host->bridge()->request_midi_reset();
                    source->host->bridge()->set_midi_queue(nullptr);
                }
                retirePluginHost(std::move(source->host));
                source->device.reset();
                source->pluginName.clear();
                source->muted = false;
                if (auto* current = jam2_.inputSourceRouter())
                    current->clear(source->routerSlot);
                attachedInputRouter_ = nullptr;
                refreshInputSourceRouting();
                if (statusGuard) statusGuard->setText(
                    QStringLiteral("No instrument loaded · MIDI source is silent"));
                if (openGuard) openGuard->setEnabled(false);
                if (replaceGuard) replaceGuard->setText(QStringLiteral("Add Instrument"));
                if (bypassGuard) {
                    QSignalBlocker blocker(bypassGuard);
                    bypassGuard->setChecked(false);
                    bypassGuard->setEnabled(false);
                }
                if (removeGuard) removeGuard->setEnabled(false);
            });
        QObject::connect(replace, &QPushButton::clicked, &dialog,
            [this, source, topologyLocked, progress, pluginLoadBusy, statusGuard,
             openGuard, replaceGuard, bypassGuard, removeGuard] {
                if (*pluginLoadBusy) {
                    progress(-1, QStringLiteral(
                        "Finish the current plugin load before starting another."));
                    return;
                }
                const auto tileProgress = [source, progress, openGuard,
                    replaceGuard, bypassGuard, removeGuard, topologyLocked](
                    int percent, const QString& text) {
                    progress(percent, text);
                    const bool busy = percent < 0;
                    const bool loaded = source->host != nullptr;
                    if (openGuard) openGuard->setEnabled(!busy && loaded);
                    if (replaceGuard) replaceGuard->setEnabled(!busy && !topologyLocked);
                    if (bypassGuard) bypassGuard->setEnabled(!busy && loaded);
                    if (removeGuard) removeGuard->setEnabled(
                        !busy && loaded && !topologyLocked);
                };
                (void)selectAndStartPluginAsync(source->routerSlot,
                    jam2::audio::InputSourceKind::MidiInstrument, &source->events,
                    [this, source, progress, statusGuard, openGuard,
                     replaceGuard, bypassGuard, removeGuard](
                        std::unique_ptr<jam2::pluginhost::PluginHostService> host,
                        const QString& name) mutable {
                        if (trackRecordingWorkflow_.inputTakeActive() ||
                            loopbackRecorder_.isRunning() ||
                            trackRecordingWorkflow_.laneArmed()) {
                            host->requestRetire();
                            progress(0, QStringLiteral(
                                "The instrument loaded after recording was armed, so it was not attached."));
                            return;
                        }

                        const auto attach = [this, source, progress,
                            statusGuard, openGuard, replaceGuard, bypassGuard,
                            removeGuard](
                                std::unique_ptr<jam2::pluginhost::PluginHostService> readyHost,
                                const QString& readyName) {
                            const auto active = std::find(
                                midiPluginSources_.begin(), midiPluginSources_.end(), source);
                            if (active == midiPluginSources_.end()) {
                                if (readyHost) readyHost->requestRetire();
                                source->device.reset();
                                progress(0, QStringLiteral(
                                    "The MIDI device was removed before its instrument finished loading."));
                                return;
                            }
                            if (source->host && source->host->bridge()) {
                                source->host->bridge()->request_midi_reset();
                                source->host->bridge()->set_midi_queue(nullptr);
                            }
                            retirePluginHost(std::move(source->host));
                            source->host = std::move(readyHost);
                            source->pluginName = readyName;
                            source->muted = false;
                            attachedInputRouter_ = nullptr;
                            refreshInputSourceRouting();
                            if (statusGuard) statusGuard->setText(
                                QStringLiteral("%1 loaded · ready to open").arg(readyName));
                            if (openGuard) openGuard->setEnabled(true);
                            if (replaceGuard) replaceGuard->setText(QStringLiteral("Replace"));
                            if (bypassGuard) {
                                QSignalBlocker blocker(bypassGuard);
                                bypassGuard->setChecked(false);
                                bypassGuard->setEnabled(true);
                            }
                            if (removeGuard) removeGuard->setEnabled(true);
                            progress(100, QStringLiteral(
                                "%1 loaded. You can open its interface now.").arg(readyName));
                        };

                        if (source->device) {
                            attach(std::move(host), name);
                            return;
                        }
                        progress(-1, QStringLiteral("Opening MIDI input device…"));
                        struct MidiOpenOutcome {
                            std::unique_ptr<jam2::pluginhost::PluginHostService> host;
                            std::unique_ptr<jam2::midi::InputDevice> device;
                            QString name;
                            std::string error;
                        };
                        const auto opened = std::make_shared<MidiOpenOutcome>();
                        opened->host = std::move(host);
                        opened->name = name;
                        QPointer<MainWindow> self(this);
                        fileWorkerPool_.start(QRunnable::create([
                            self, source, opened, attach, progress
                        ]() mutable {
                            opened->device = jam2::midi::open_input_device(
                                source->deviceInfo.id, source->events, opened->error);
                            if (self.isNull()) return;
                            QMetaObject::invokeMethod(self, [
                                self, source, opened, attach, progress
                            ]() mutable {
                                if (self.isNull()) return;
                                if (!opened->device) {
                                    if (opened->host) opened->host->requestRetire();
                                    progress(0, QStringLiteral("Could not open the MIDI device: %1")
                                        .arg(QString::fromStdString(opened->error)));
                                    return;
                                }
                                source->device = std::move(opened->device);
                                attach(std::move(opened->host), opened->name);
                            }, Qt::QueuedConnection);
                        }));
                    }, tileProgress);
            });
    }
    if (midiPluginSources_.empty()) {
        auto* empty = new QLabel(QStringLiteral(
            "No MIDI devices are assigned. Add one from MIDI first."), midiBox);
        empty->setWordWrap(true);
        midiLayout->addWidget(empty);
    }
    layout->addWidget(midiBox);
    layout->addStretch(1);

    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* footer = new QWidget(&dialog);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    footerLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(footer);
    dialog.exec();
    updateInputSourceButtons();
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
    // The GUI receives its compact diagnostics through the in-process callback.
    // Logged GUI jams retain a hidden two-second CSV timeline for later analysis.
    options.stats_interval_ms = 0;
    options.stats_warmup_ms = statsWarmupMsSpin_ ? statsWarmupMsSpin_->value() : 3000;
    if (options.stats_enabled && logStatsEdit_ && !logStatsEdit_->text().trimmed().isEmpty()) {
        options.log_stats_dir = nativeFilePath(logStatsEdit_->text().trimmed());
        options.stats_interval_ms = 2000;
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
    options.metronome = metronomeTransport_.localRunning();
    options.metronome_transport_gated = true;
    options.bpm = metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value();
    options.metronome_level = gainFromDb(static_cast<double>(metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10));
    options.metronome_sound = jam2::metronome::sanitize_click_sound(
        metronomeSoundBox_ ? metronomeSoundBox_->currentData().toInt() : 0);
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
    options.output_level = gainFromDb(static_cast<double>(
        masterOutputLevelSlider_ ? masterOutputLevelSlider_->value() : 0));
    options.local_monitor = mixMonitorCheck_ && mixMonitorCheck_->isChecked();
    options.local_monitor_level = gainFromDb(static_cast<double>(mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : 0));
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
    options.session_profile_name = options.profile_name;
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
    if (snapshot.role == SharedSessionController::Role::Joiner) {
        options.profile_name = joinProfileName_.toStdString();
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
    const QString data = deviceBox_->currentData().toString();
    return data.isEmpty() ? deviceId(deviceBox_->currentText()) : data;
}

QJsonObject MainWindow::trackToJson() const
{
    const auto& model = trackController_.model();
    QJsonArray clickEnabled;
    QJsonArray clickAccents;
    for (bool enabled : metronomeEnabledSteps_) clickEnabled.append(enabled);
    for (bool accented : metronomeAccents_) clickAccents.append(accented);
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
        {QStringLiteral("metronome_bpm"), metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 80},
        {QStringLiteral("metronome_beats"), metronomeBeatsSpin_ ? metronomeBeatsSpin_->currentData().toInt() : 4},
        {QStringLiteral("metronome_beat_unit"), metronomeBeatUnitBox_ ? metronomeBeatUnitBox_->currentData().toInt() : 4},
        {QStringLiteral("metronome_tempo_pulse_units"), metronomeTempoPulseBox_ ? metronomeTempoPulseBox_->currentData().toInt() : 1},
        {QStringLiteral("metronome_division"), metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1},
        {QStringLiteral("metronome_click_enabled"), clickEnabled},
        {QStringLiteral("metronome_click_accents"), clickAccents},
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
    if (!model.filePath.isEmpty() && !QFileInfo(model.filePath).isAbsolute() &&
        !projectPersistence_.projectFolder().isEmpty()) {
        model.filePath = QDir(projectPersistence_.projectFolder()).absoluteFilePath(model.filePath);
    }
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
    model.syncControls = jamSyncPolicy_.globalPlayback;
    model.focusEnabled = object.value(QStringLiteral("focus_enabled")).toBool(model.focusEnabled);
    model.focusPreset = object.value(QStringLiteral("focus_preset")).toString(model.focusPreset);
    model.focusFrequencyHz = object.value(QStringLiteral("focus_frequency_hz")).toDouble(model.focusFrequencyHz);
    model.focusGainDb = object.value(QStringLiteral("focus_gain_db")).toDouble(model.focusGainDb);
    model.focusQ = object.value(QStringLiteral("focus_q")).toDouble(model.focusQ);
    model.highpassHz = object.value(QStringLiteral("highpass_hz")).toDouble(model.highpassHz);
    model.lowpassHz = object.value(QStringLiteral("lowpass_hz")).toDouble(model.lowpassHz);
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
    preparedMixByBank_ = {};
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
        "Input %1 (%2 ms) | Output %3 (%4 ms) | Active processing %5 (%6 ms) | "
        "Manual %7 | Applied %8 (%9 ms)")
        .arg(trackRecordingWorkflow_.inputLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.inputLatencyFrames()), 0, 'f', 2)
        .arg(trackRecordingWorkflow_.outputLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.outputLatencyFrames()), 0, 'f', 2)
        .arg(trackRecordingWorkflow_.sourceLatencyFrames())
        .arg(milliseconds(trackRecordingWorkflow_.sourceLatencyFrames()), 0, 'f', 2)
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
    addInitialEmptyLooperLanes(looperProject_);
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
            const bool valid = error->isEmpty() && validatedSong.loadJson(*root) &&
                (looper.isEmpty() || validatedLooper.loadJson(looper)) &&
                (looper.isEmpty() ||
                 validatedSong.sections().size() == validatedLooper.banks().size());
            if (!valid) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Jam2"),
                    error->isEmpty() ? QStringLiteral("Invalid JamJar file.") : *error);
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
            loadTrackJson(root->value(QStringLiteral("track")).toObject());
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
    if (!QDir().mkpath(songFolder)) {
        QMessageBox::warning(this, QStringLiteral("Save JamJar"),
            QStringLiteral("Could not create the song folder: %1").arg(songFolder));
        return false;
    }
    const QString path = jamStorage_.projectFilePath();
    const QFileInfo songInfo(path);
    if (!materializeLooperAssets(songInfo.absolutePath())) {
        return false;
    }
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
    preparedMix_.path = relocated(preparedMix_.path);
    for (PreparedMixResult& cached : preparedMixByBank_) {
        cached.path = relocated(cached.path);
    }
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
    const QString expected = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const LooperBank& bank : looperProject_.banks()) {
        for (const LooperLane& lane : bank.lanes) {
            const QString candidate =
                QDir::cleanPath(QFileInfo(looperAssetAbsolutePath(lane)).absoluteFilePath());
            if (candidate == expected) return true;
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
    playPreparedMixWhenReady_ = resumePlayback;
    trackRecordingWorkflow_.cancelPreparedAttach();
    if (jam2_.isRunning()) {
        jam2::EngineCommand unload;
        unload.type = jam2::EngineCommandType::UnloadPreparedTrack;
        if (!submitEngineCommand(unload, QStringLiteral("unload prepared track"))) {
            appendLog(QStringLiteral(
                "engine command queue unavailable: unload prepared track"));
        }
    }
    if (!preparedMix_.path.isEmpty()) {
        const QString obsoletePath = preparedMix_.path;
        if (!projectPersistence_.discardTransientWav(obsoletePath) &&
            projectPersistence_.ownsTransientWav(obsoletePath)) {
            obsoletePreparedMixPaths_.insert(obsoletePath);
        }
    }
    preparedMix_ = {};
    const int activeBank = looperProject_.activeBankIndex();
    if (activeBank >= 0 && activeBank < static_cast<int>(preparedMixByBank_.size())) {
        preparedMixByBank_[static_cast<std::size_t>(activeBank)] = {};
    }
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
        preparedMixByBank_[static_cast<std::size_t>(qBound(
            0, bankIndex, looperProject_.banks().size() - 1))] = {};
    } else {
        jam2::practice::PracticeIdeaController::clearReferences(looperProject_);
        preparedMixByBank_ = {};
    }
    discardObsoleteReferenceWavs(referencePaths);
    if (preparedMixWorkerRunning_) {
        preparedMixRerunPending_ = true;
    }
    if (bankIndex >= 0 && bankIndex != looperProject_.activeBankIndex()) {
        if (rebuildRemainingTracks &&
            !looperProject_.banks().at(bankIndex).lanes.isEmpty()) {
            regeneratePreparedMix(bankIndex);
        }
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

    const QString cacheRoot = QDir(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .absoluteFilePath(QStringLiteral("idea-previews"));
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
    if (!preparedMix_.path.isEmpty() && preparedMix_.error.isEmpty()) {
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
    prompt.addButton(QMessageBox::Cancel);
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
            preparedMixByBank_ = {};
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
