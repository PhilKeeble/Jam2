#include "MainWindow.hpp"

#include "Jam2MacPermissions.hpp"
#include "SessionController.hpp"
#include "ControlProtocol.hpp"

#include "common.hpp"
#include "audio_device.hpp"
#include "gui_control_protocol.hpp"
#include "pcm16_wav.hpp"
#include "tuning_profile.hpp"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
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
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QIODevice>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QProcess>
#include <QProgressDialog>
#include <QPointer>
#include <QRunnable>
#include <QProxyStyle>
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
#include <QStyleOption>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QUuid>

#include <algorithm>
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
#include <utility>
#include <vector>

namespace {

constexpr qint64 kMaxLooperAssetBytes = 512LL * 1024LL * 1024LL;
constexpr int kLooperAssetChunkBytes = 24 * 1024;
constexpr int kMaxLooperAssetRequests = 64;
constexpr qint64 kLooperAssetFrameQueueEstimate = 48 * 1024;
constexpr int kLooperAssetProgressDeadlineMs = 10000;

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

QString endpointWithHost(const QString& endpoint, const QString& host)
{
    const jam2::Endpoint parsed = jam2::parse_endpoint(endpoint.toStdString());
    return QString::fromStdString(jam2::endpoint_to_string({host.toStdString(), parsed.port}));
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

bool isBoundedInteger(const QJsonValue& value, qint64 minimum, qint64 maximum)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number &&
        number >= static_cast<double>(minimum) && number <= static_cast<double>(maximum);
}

bool isBoundedString(const QJsonValue& value, qsizetype maximum)
{
    return value.isString() && value.toString().size() <= maximum;
}

bool validateRemoteSongAssets(const QJsonObject& song, QString& reason)
{
    const QJsonValue looperValue = song.value(QStringLiteral("looper"));
    if (looperValue.isUndefined()) {
        return true;
    }
    if (!looperValue.isObject()) {
        reason = QStringLiteral("song looper field is not an object");
        return false;
    }
    const QJsonArray banks = looperValue.toObject().value(QStringLiteral("banks")).toArray();
    if (banks.size() > 4) {
        reason = QStringLiteral("song contains too many looper banks");
        return false;
    }
    for (const QJsonValue& bankValue : banks) {
        if (!bankValue.isObject()) {
            reason = QStringLiteral("song looper bank is not an object");
            return false;
        }
        const QJsonArray lanes = bankValue.toObject().value(QStringLiteral("lanes")).toArray();
        if (lanes.size() > 128) {
            reason = QStringLiteral("song looper bank contains too many lanes");
            return false;
        }
        for (const QJsonValue& laneValue : lanes) {
            if (!laneValue.isObject()) {
                reason = QStringLiteral("song looper lane is not an object");
                return false;
            }
            const QString hash = laneValue.toObject().value(QStringLiteral("asset_hash")).toString();
            if (!hash.isEmpty() && !isSha256Hex(hash)) {
                reason = QStringLiteral("song contains an invalid asset hash");
                return false;
            }
        }
    }
    return true;
}

bool validateControlMessageShape(const QJsonObject& message, QString& reason)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type.isEmpty() || type.size() > 64) {
        reason = QStringLiteral("message type is missing or too long");
        return false;
    }
    const QJsonValue revision = message.value(QStringLiteral("revision"));
    if (!revision.isUndefined() && !isBoundedInteger(revision, 0, std::numeric_limits<int>::max())) {
        reason = QStringLiteral("message revision is invalid");
        return false;
    }
    if (type == QStringLiteral("session.settings")) {
        const QJsonValue settingsValue = message.value(QStringLiteral("settings"));
        if (!settingsValue.isObject()) {
            reason = QStringLiteral("session settings shape is invalid");
            return false;
        }
        const QJsonObject settings = settingsValue.toObject();
        if (settings.size() > 40) {
            reason = QStringLiteral("session settings shape is invalid");
            return false;
        }
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            if (it.key().size() > 64 ||
                !(it.value().isBool() ||
                  (it.value().isString() && it.value().toString().size() <= 64) ||
                  (it.value().isDouble() && std::isfinite(it.value().toDouble()) &&
                   std::abs(it.value().toDouble()) <= 1.0e9))) {
                reason = QStringLiteral("session setting value is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("session.membership")) {
        const QJsonValue peersValue = message.value(QStringLiteral("peers"));
        if (!peersValue.isArray() || peersValue.toArray().size() > 4096) {
            reason = QStringLiteral("network membership list is invalid or exceeds its safety bound");
            return false;
        }
        static const QRegularExpression tokenExpression(QStringLiteral("^[0-9a-f]{32}$"));
        if (!tokenExpression.match(message.value(QStringLiteral("coordinator_token")).toString()).hasMatch()) {
            reason = QStringLiteral("mesh coordinator token is invalid");
            return false;
        }
        for (const QJsonValue& peerValue : peersValue.toArray()) {
            const QJsonObject peer = peerValue.toObject();
            if (!peerValue.isObject() ||
                !tokenExpression.match(peer.value(QStringLiteral("token")).toString()).hasMatch() ||
                !isBoundedString(peer.value(QStringLiteral("endpoint")), 255)) {
                reason = QStringLiteral("mesh peer entry is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("session.error")) {
        return isBoundedString(message.value(QStringLiteral("message")), 4096)
            ? true : (reason = QStringLiteral("session error text is invalid"), false);
    }
    if (type == QStringLiteral("metronome.settings")) {
        static const QStringList maskKeys{
            QStringLiteral("play_mask_low"), QStringLiteral("play_mask_high"),
            QStringLiteral("accent_mask_low"), QStringLiteral("accent_mask_high")};
        if (!message.value(QStringLiteral("running")).isBool() || !message.value(QStringLiteral("leader")).isBool() ||
            !isBoundedString(message.value(QStringLiteral("mode")), 32) ||
            !isBoundedInteger(message.value(QStringLiteral("bpm")), 1, 400) ||
            !isBoundedInteger(message.value(QStringLiteral("beats")), 1, 16) ||
            !isBoundedInteger(message.value(QStringLiteral("division")), 1, 16)) {
            reason = QStringLiteral("metronome settings are invalid");
            return false;
        }
        static const QRegularExpression maskExpression(QStringLiteral("^[0-9a-fA-F]{1,16}$"));
        for (const QString& key : maskKeys) {
            if (!maskExpression.match(message.value(key).toString()).hasMatch()) {
                reason = QStringLiteral("metronome mask is invalid");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("beat.set")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        return isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beat")), 0, 511) &&
            (lane == QStringLiteral("chord") || lane == QStringLiteral("beat") || lane == QStringLiteral("lyric")) &&
            isBoundedString(message.value(QStringLiteral("text")), 4096)
            ? true : (reason = QStringLiteral("beat cell update is invalid"), false);
    }
    if (type == QStringLiteral("grid.resize")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        return (lane == QStringLiteral("chord") || lane == QStringLiteral("beat")) &&
            isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beats")), 4, 512)
            ? true : (reason = QStringLiteral("grid resize is invalid"), false);
    }
    if (type == QStringLiteral("beat.hit")) {
        return isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beat")), 0, 511) &&
            isBoundedInteger(message.value(QStringLiteral("lane")), 0, 10) &&
            isBoundedString(message.value(QStringLiteral("text")), 4096)
            ? true : (reason = QStringLiteral("beat hit is invalid"), false);
    }
    if (type == QStringLiteral("beat.division")) {
        const int division = message.value(QStringLiteral("division")).toInt(-1);
        return isBoundedInteger(message.value(QStringLiteral("section")), 0, 63) &&
            isBoundedInteger(message.value(QStringLiteral("beat")), 0, 511) &&
            QList<int>{1, 2, 3, 4, 6, 8}.contains(division)
            ? true : (reason = QStringLiteral("beat division is invalid"), false);
    }
    if (type == QStringLiteral("lyrics.set")) {
        return isBoundedString(message.value(QStringLiteral("text")), 64 * 1024)
            ? true : (reason = QStringLiteral("lyrics update is invalid"), false);
    }
    if (type == QStringLiteral("song.set")) {
        const QJsonValue songValue = message.value(QStringLiteral("song"));
        if (!songValue.isObject() ||
            !isBoundedInteger(message.value(QStringLiteral("arrangement_revision")), 0, std::numeric_limits<int>::max()) ||
            (!message.value(QStringLiteral("host_authoritative")).isUndefined() &&
             !message.value(QStringLiteral("host_authoritative")).isBool()) ||
            (!message.value(QStringLiteral("track_playing")).isUndefined() &&
             !message.value(QStringLiteral("track_playing")).isBool())) {
            reason = QStringLiteral("song snapshot or revision is invalid");
            return false;
        }
        return validateRemoteSongAssets(songValue.toObject(), reason);
    }
    if (type == QStringLiteral("looper.recording.offer")) {
        static const QRegularExpression recordingIdExpression(
            QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
        const QString recordingId = message.value(QStringLiteral("recording_id")).toString();
        const QString targetLaneId = message.value(QStringLiteral("target_lane_id")).toString();
        const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
        return recordingIdExpression.match(recordingId).hasMatch() &&
            isBoundedInteger(message.value(QStringLiteral("bank")), 0, 3) &&
            !targetLaneId.isEmpty() && targetLaneId.size() <= 80 &&
            isSha256Hex(hash) &&
            isBoundedString(message.value(QStringLiteral("name")), 512)
            ? true : (reason = QStringLiteral("recording offer is invalid"), false);
    }
    if (type == QStringLiteral("looper.asset.request")) {
        const QJsonValue arrangementRevision = message.value(QStringLiteral("arrangement_revision"));
        return message.value(QStringLiteral("hashes")).isArray() &&
            (arrangementRevision.isUndefined() ||
             isBoundedInteger(arrangementRevision, 0, std::numeric_limits<int>::max()));
    }
    if (type == QStringLiteral("looper.asset.start") || type == QStringLiteral("looper.asset.chunk") ||
        type == QStringLiteral("looper.asset.done")) {
        return true; // Transfer-specific strict checks run before any file mutation.
    }
    reason = QStringLiteral("unknown message type");
    return false;
}

QDir appReleaseDir()
{
    QDir appDir(QCoreApplication::applicationDirPath());

    QDir bundleDir(appDir);
    if (bundleDir.dirName() == QStringLiteral("MacOS") &&
        bundleDir.cdUp() &&
        bundleDir.dirName() == QStringLiteral("Contents") &&
        bundleDir.cdUp() &&
        bundleDir.dirName().endsWith(QStringLiteral(".app")) &&
        bundleDir.cdUp()) {
        return bundleDir;
    }

    if (appDir.dirName() == QStringLiteral("release")) {
        return appDir;
    }

    QDir probe(appDir);
    while (true) {
        if (probe.exists(QStringLiteral("release"))) {
            return QDir(probe.absoluteFilePath(QStringLiteral("release")));
        }
        if (!probe.cdUp()) {
            break;
        }
    }

    return appDir;
}

QString appReleaseFilePath(const QString& folder, const QString& fileName)
{
    QDir dir = appReleaseDir();
    dir.mkpath(folder);
    return dir.absoluteFilePath(folder + QLatin1Char('/') + fileName);
}

QString appReleaseFolderPath(const QString& folder)
{
    QDir dir = appReleaseDir();
    dir.mkpath(folder);
    return dir.absoluteFilePath(folder);
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

void appendGuiU16(QByteArray& out, quint16 value)
{
    out.append(char(value & 0xffU));
    out.append(char((value >> 8) & 0xffU));
}

void appendGuiU32(QByteArray& out, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.append(char((value >> shift) & 0xffU));
    }
}

void appendGuiU64(QByteArray& out, quint64 value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        out.append(char((value >> shift) & 0xffU));
    }
}

void appendGuiI64(QByteArray& out, qint64 value)
{
    appendGuiU64(out, static_cast<quint64>(value));
}

void appendGuiF64(QByteArray& out, double value)
{
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    appendGuiU64(out, bits);
}

quint16 guiU16(const QByteArray& data, qsizetype offset)
{
    return quint16(uchar(data[offset])) | quint16(uchar(data[offset + 1]) << 8);
}

quint32 guiU32(const QByteArray& data, qsizetype offset)
{
    quint32 value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= quint32(uchar(data[offset + shift / 8])) << shift;
    }
    return value;
}

quint64 guiU64(const QByteArray& data, qsizetype offset)
{
    quint64 value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= quint64(uchar(data[offset + shift / 8])) << shift;
    }
    return value;
}

qint64 guiI64(const QByteArray& data, qsizetype offset)
{
    return static_cast<qint64>(guiU64(data, offset));
}

double guiF64(const QByteArray& data, qsizetype offset)
{
    const quint64 bits = guiU64(data, offset);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct GuiBinaryCommand {
    jam2::gui_control::CommandOpcode opcode = jam2::gui_control::CommandOpcode::None;
    quint16 flags = 0;
    std::array<qint64, 8> i{};
    std::array<double, 4> d{};
    QByteArray text;
};

QByteArray encodeGuiCommandPayload(const GuiBinaryCommand& command)
{
    QByteArray payload;
    appendGuiU16(payload, static_cast<quint16>(command.opcode));
    appendGuiU16(payload, command.flags);
    appendGuiU32(payload, static_cast<quint32>(command.text.size()));
    for (const qint64 value : command.i) {
        appendGuiI64(payload, value);
    }
    for (const double value : command.d) {
        appendGuiF64(payload, value);
    }
    payload.append(command.text);
    return payload;
}

bool parseDoubleToken(const QString& value, double& out)
{
    bool ok = false;
    out = value.toDouble(&ok);
    return ok;
}

bool parseI64Token(const QString& value, qint64& out)
{
    bool ok = false;
    out = value.toLongLong(&ok, 0);
    return ok;
}

bool parseU64Token(const QString& value, qint64& out)
{
    bool ok = false;
    out = static_cast<qint64>(value.toULongLong(&ok, 0));
    return ok;
}

bool encodeJamTextCommand(const QString& line, GuiBinaryCommand& command)
{
    const QString trimmed = line.trimmed();
    const QStringList parts = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }
    if (parts[0] == QStringLiteral("metro") && parts.size() >= 2) {
        if (parts[1] == QStringLiteral("on") || parts[1] == QStringLiteral("off")) {
            command.opcode = jam2::gui_control::CommandOpcode::MetronomeEnabled;
            command.flags = parts[1] == QStringLiteral("on") ? 1 : 0;
            return true;
        }
        if (parts[1] == QStringLiteral("leader") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::MetronomeLeader;
            command.flags = parts[2] == QStringLiteral("on") ? 1 : 0;
            return true;
        }
        if (parts[1] == QStringLiteral("mode") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::MetronomeMode;
            command.text = parts[2].toUtf8();
            return true;
        }
        if (parts[1] == QStringLiteral("level") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::MetronomeLevel;
            return parseDoubleToken(parts[2], command.d[0]);
        }
        if (parts[1] == QStringLiteral("pattern") && parts.size() >= 9) {
            command.opcode = jam2::gui_control::CommandOpcode::MetronomePattern;
            for (int i = 0; i < 3; ++i) {
                if (!parseI64Token(parts[2 + i], command.i[static_cast<std::size_t>(i)])) return false;
            }
            for (int i = 0; i < 4; ++i) {
                if (!parseU64Token(parts[5 + i], command.i[static_cast<std::size_t>(3 + i)])) return false;
            }
            return true;
        }
    }
    if ((parts[0] == QStringLiteral("remote") || parts[0] == QStringLiteral("send")) &&
        parts.size() >= 3 &&
        parts[1] == QStringLiteral("level")) {
        command.opcode = parts[0] == QStringLiteral("remote")
            ? jam2::gui_control::CommandOpcode::RemoteLevel
            : jam2::gui_control::CommandOpcode::SendLevel;
        return parseDoubleToken(parts[2], command.d[0]);
    }
    if (parts[0] == QStringLiteral("monitor") && parts.size() >= 2) {
        if (parts[1] == QStringLiteral("on") || parts[1] == QStringLiteral("off")) {
            command.opcode = jam2::gui_control::CommandOpcode::MonitorEnabled;
            command.flags = parts[1] == QStringLiteral("on") ? 1 : 0;
            return true;
        }
        if (parts[1] == QStringLiteral("level") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::MonitorLevel;
            return parseDoubleToken(parts[2], command.d[0]);
        }
    }
    if (parts[0] == QStringLiteral("bpm") && parts.size() >= 2) {
        command.opcode = jam2::gui_control::CommandOpcode::Bpm;
        return parseI64Token(parts[1], command.i[0]);
    }
    if (trimmed.startsWith(QStringLiteral("record jam start "))) {
        command.opcode = jam2::gui_control::CommandOpcode::RecordJamStart;
        command.text = trimmed.mid(QStringLiteral("record jam start ").size()).toUtf8();
        return !command.text.isEmpty();
    }
    if (trimmed == QStringLiteral("record jam stop")) {
        command.opcode = jam2::gui_control::CommandOpcode::RecordJamStop;
        return true;
    }
    if (parts[0] == QStringLiteral("record") &&
        parts.size() >= 3 &&
        parts[1] == QStringLiteral("latency_adjust")) {
        command.opcode = jam2::gui_control::CommandOpcode::RecordingLatencyAdjustment;
        return parseI64Token(parts[2], command.i[0]);
    }
    if (parts[0] == QStringLiteral("track") && parts.size() >= 2) {
        if (trimmed.startsWith(QStringLiteral("track load "))) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackLoad;
            command.text = trimmed.mid(QStringLiteral("track load ").size()).toUtf8();
            return !command.text.isEmpty();
        }
        if (parts[1] == QStringLiteral("restart")) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackRestartQuantized;
            return true;
        }
        if (parts[1] == QStringLiteral("play") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackPlay;
            return parseI64Token(parts[2], command.i[0]);
        }
        if (parts[1] == QStringLiteral("stop") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackStop;
            return parseI64Token(parts[2], command.i[0]);
        }
        if (parts[1] == QStringLiteral("seek") && parts.size() >= 4) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackSeek;
            return parseI64Token(parts[2], command.i[0]) && parseI64Token(parts[3], command.i[1]);
        }
        if (parts[1] == QStringLiteral("level") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackLevel;
            return parseDoubleToken(parts[2], command.d[0]);
        }
        if (parts[1] == QStringLiteral("loop") && parts.size() >= 3) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackLoop;
            command.flags = parts[2] == QStringLiteral("on") ? 1 : 0;
            if (parts.size() >= 5) {
                return parseI64Token(parts[3], command.i[0]) && parseI64Token(parts[4], command.i[1]);
            }
            return true;
        }
    }
    if (trimmed.startsWith(QStringLiteral("track_take arm input "))) {
        const QString rest = trimmed.mid(QStringLiteral("track_take arm input ").size());
        const int split = rest.indexOf(QLatin1Char(' '));
        if (split <= 0) return false;
        command.opcode = jam2::gui_control::CommandOpcode::TrackTakeArmInput;
        command.text = rest.left(split).toUtf8();
        command.text.append('\n');
        command.text.append(rest.mid(split + 1).toUtf8());
        return true;
    }
    if (parts[0] == QStringLiteral("track_take") && parts.size() >= 3) {
        if (parts[1] == QStringLiteral("start_quantized") && parts.size() >= 4) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackTakeStartQuantized;
            command.text = parts[2].toUtf8();
            return parseI64Token(parts[3], command.i[0]);
        }
        if (parts[1] == QStringLiteral("start") && parts.size() >= 4) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackTakeStart;
            command.text = parts[2].toUtf8();
            return parseI64Token(parts[3], command.i[0]);
        }
        if (parts[1] == QStringLiteral("stop") && parts.size() >= 4) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackTakeStop;
            command.text = parts[2].toUtf8();
            return parseI64Token(parts[3], command.i[0]);
        }
        if (parts[1] == QStringLiteral("cancel")) {
            command.opcode = jam2::gui_control::CommandOpcode::TrackTakeCancel;
            return true;
        }
    }
    return false;
}

std::uint64_t rawFrameFromMusicalFrame(std::uint64_t musicalFrame, std::int64_t renderOffsetFrames)
{
    if (renderOffsetFrames >= 0) {
        const std::uint64_t offset = static_cast<std::uint64_t>(renderOffsetFrames);
        return musicalFrame > offset ? musicalFrame - offset : 0ULL;
    }
    return musicalFrame + static_cast<std::uint64_t>(-renderOffsetFrames);
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

class WaveformWidget : public QWidget {
public:
    explicit WaveformWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(240);
        setMouseTracking(true);
    }

    std::function<void(qint64)> onSeekMs;

    void clear()
    {
        peaks_.clear();
        label_ = QStringLiteral("No WAV loaded");
        durationMs_ = 0;
        playheadMs_ = 0;
        loopStartMs_ = -1;
        loopEndMs_ = -1;
        update();
    }

    void setDurationMs(qint64 durationMs)
    {
        durationMs_ = qMax<qint64>(0, durationMs);
        playheadMs_ = qBound<qint64>(0, playheadMs_, durationMs_);
        update();
    }

    void setBpm(double bpm)
    {
        bpm_ = qBound(1.0, bpm, 400.0);
        update();
    }

    void setGridPosition(std::uint64_t absoluteBeat, bool running)
    {
        gridAbsoluteBeat_ = absoluteBeat;
        gridRunning_ = running;
        update();
    }

    void setPlayheadMs(qint64 positionMs)
    {
        playheadMs_ = qBound<qint64>(0, positionMs, qMax<qint64>(durationMs_, 0));
        update();
    }

    void setLoop(qint64 startMs, qint64 endMs)
    {
        loopStartMs_ = startMs >= 0 ? startMs : -1;
        loopEndMs_ = endMs >= 0 ? endMs : -1;
        update();
    }

    void setPeaks(std::vector<float> peaks, bool valid)
    {
        peaks_ = std::move(peaks);
        label_ = valid ? QString{} : QStringLiteral("Waveform preview supports PCM16 WAV");
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(18, 20, 22));
        painter.setRenderHint(QPainter::Antialiasing, false);

        if (durationMs_ > 0 && bpm_ > 0.0) {
            const double beatMs = 60000.0 / bpm_;
            const int beats = static_cast<int>(std::floor(static_cast<double>(durationMs_) / beatMs));
            for (int beat = 0; beat <= beats + 1; ++beat) {
                const qint64 beatPosition = static_cast<qint64>(std::llround(beat * beatMs));
                const int x = xForMs(beatPosition);
                const bool bar = beat % 4 == 0;
                painter.setPen(bar ? QColor(76, 86, 96) : QColor(42, 48, 54));
                painter.drawLine(x, 0, x, height());
                if (bar) {
                    painter.setPen(QColor(150, 158, 166));
                    painter.drawText(x + 4, 18, QString::number(beat + 1));
                }
            }
            const int totalBeats = qMax(1, static_cast<int>(std::ceil(static_cast<double>(durationMs_) / beatMs)));
            if (gridRunning_) {
                const int currentBeat = static_cast<int>(gridAbsoluteBeat_ % static_cast<std::uint64_t>(totalBeats));
                const qint64 beatPosition = static_cast<qint64>(std::llround(currentBeat * beatMs));
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(102, 198, 166));
                painter.drawEllipse(QPoint(xForMs(beatPosition), 7), 4, 4);
            }
        }

        painter.setPen(QColor(58, 64, 70));
        painter.drawLine(0, height() / 2, width(), height() / 2);
        painter.setPen(QColor(102, 198, 166));
        if (!peaks_.empty()) {
            for (int x = 0; x < width(); ++x) {
                const int index = qBound(0, x * static_cast<int>(peaks_.size()) / qMax(1, width()), static_cast<int>(peaks_.size()) - 1);
                const int half = qMax(2, static_cast<int>(peaks_[index] * (height() / 2 - 14)));
                painter.drawLine(x, height() / 2 - half, x, height() / 2 + half);
            }
        }
        painter.setPen(QColor(220, 224, 226));
        painter.drawText(rect().adjusted(12, 8, -12, -8), Qt::AlignLeft | Qt::AlignTop, label_);

        drawLoopMarker(painter, loopStartMs_, QColor(82, 170, 255), QStringLiteral("Loop Start"));
        drawLoopMarker(painter, loopEndMs_, QColor(255, 184, 82), QStringLiteral("Loop End"));

        if (loopStartMs_ >= 0 && loopEndMs_ > loopStartMs_) {
            const int startX = xForMs(loopStartMs_);
            const int endX = xForMs(loopEndMs_);
            painter.fillRect(QRect(QPoint(startX, 0), QPoint(endX, height())).normalized(), QColor(86, 132, 210, 28));
        }

        if (durationMs_ > 0) {
            const int playheadX = xForMs(playheadMs_);
            painter.setPen(QPen(QColor(255, 92, 92), 2));
            painter.drawLine(playheadX, 0, playheadX, height());
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || durationMs_ <= 0) {
            QWidget::mousePressEvent(event);
            return;
        }
        seekToX(event->position().x());
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || durationMs_ <= 0) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        seekToX(event->position().x());
    }

private:
    int xForMs(qint64 ms) const
    {
        if (durationMs_ <= 0 || width() <= 1) {
            return 0;
        }
        return qBound(0, static_cast<int>((static_cast<double>(ms) / static_cast<double>(durationMs_)) * width()), width() - 1);
    }

    qint64 msForX(double x) const
    {
        if (durationMs_ <= 0 || width() <= 1) {
            return 0;
        }
        const double clampedX = qBound(0.0, x, static_cast<double>(width() - 1));
        return qBound<qint64>(0, static_cast<qint64>(std::llround((clampedX / width()) * durationMs_)), durationMs_);
    }

    void seekToX(double x)
    {
        const qint64 position = msForX(x);
        setPlayheadMs(position);
        if (onSeekMs) {
            onSeekMs(position);
        }
    }

    void drawLoopMarker(QPainter& painter, qint64 positionMs, const QColor& color, const QString& text)
    {
        if (positionMs < 0 || durationMs_ <= 0) {
            return;
        }
        const int x = xForMs(positionMs);
        painter.setPen(QPen(color, 2));
        painter.drawLine(x, 0, x, height());
        painter.setBrush(color);
        QPolygon tag;
        tag << QPoint(x, 0) << QPoint(x + 9, 0) << QPoint(x, 12);
        painter.drawPolygon(tag);
        painter.setPen(color.lighter(135));
        painter.drawText(x + 6, height() - 10, text);
    }

    std::vector<float> peaks_;
    QString label_ = QStringLiteral("No WAV loaded");
    qint64 durationMs_ = 0;
    qint64 playheadMs_ = 0;
    qint64 loopStartMs_ = -1;
    qint64 loopEndMs_ = -1;
    double bpm_ = 120.0;
    std::uint64_t gridAbsoluteBeat_ = 0;
    bool gridRunning_ = false;
};

class LooperLaneStackWidget : public QWidget {
public:
    struct LaneView {
        LooperLane lane;
        QString assetPath;
        qint64 sourceFrames = 0;
        std::vector<float> peaks;
    };

    explicit LooperLaneStackWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(260);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }

    std::function<void(int)> onSelected;
    std::function<void()> onAddLane;
    std::function<void()> onAddWav;
    std::function<void(int)> onMute;
    std::function<void(int)> onSolo;
    std::function<void(int)> onArm;
    std::function<void(int)> onRename;
    std::function<void(int)> onRemove;
    std::function<void(int, double)> onGainChanged;
    std::function<void(int, qint64, qint64, qint64)> onRegionCommitted;
    std::function<void(int)> onBankSelected;
    std::function<void(qint64)> onSeekFrame;

    void setLanes(
        QVector<LaneView> lanes,
        int selected,
        int activeBank,
        int armedLane,
        int sampleRate,
        double bpm,
        bool gridLockEnabled)
    {
        lanes_ = std::move(lanes);
        selectedLane_ = selected;
        activeBank_ = qBound(0, activeBank, 3);
        armedLane_ = armedLane;
        markerSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        bpm_ = qBound(1.0, bpm, 400.0);
        gridLockEnabled_ = gridLockEnabled;
        dragMode_ = DragMode::None;
        updateMinimumHeight();
        update();
    }

    void setGridPosition(std::uint64_t absoluteBeat, bool running, double bpm)
    {
        gridAbsoluteBeat_ = absoluteBeat;
        gridRunning_ = running;
        bpm_ = qBound(1.0, bpm, 400.0);
        update();
    }

    void setPlaybackMarkers(qint64 positionMs, qint64 loopStartMs, qint64 loopEndMs)
    {
        playheadMs_ = positionMs >= 0 ? positionMs : -1;
        loopStartMs_ = loopStartMs >= 0 ? loopStartMs : -1;
        loopEndMs_ = loopEndMs >= 0 ? loopEndMs : -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(15, 17, 19));
        painter.setRenderHint(QPainter::Antialiasing, false);

        drawToolbar(painter);

        const int laneCount = visualLaneCount();
        for (int row = 0; row < laneCount; ++row) {
            drawLane(painter, row);
        }

        drawOverlays(painter);

        const QRect plus = plusRect();
        painter.fillRect(plus, QColor(28, 32, 36));
        painter.setPen(QColor(86, 94, 102));
        painter.drawRect(plus.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(220, 224, 226));
        painter.drawText(plus.adjusted(0, 0, 0, 0), Qt::AlignCenter, QStringLiteral("+"));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        const QPoint pos = event->position().toPoint();
        if (plusRect().contains(pos)) {
            if (onAddLane) onAddLane();
            event->accept();
            return;
        }

        const int laneIndex = laneAt(pos);
        if (laneIndex < 0) {
            QWidget::mousePressEvent(event);
            return;
        }
        const QString control = controlAt(laneIndex, pos);
        selectLane(laneIndex);
        if (laneIndex >= lanes_.size()) {
            if (onAddLane) onAddLane();
            if (control == QStringLiteral("arm") && onArm) {
                onArm(laneIndex);
            }
            event->accept();
            return;
        }

        if (control == QStringLiteral("mute")) {
            if (onMute) onMute(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("solo")) {
            if (onSolo) onSolo(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("arm")) {
            if (onArm) onArm(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("rename")) {
            if (onRename) onRename(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("remove")) {
            if (onRemove) onRemove(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("gain")) {
            beginGainDrag(laneIndex, pos.y());
            event->accept();
            return;
        }

        if (laneTimelineRect(laneIndex).contains(pos) && onSeekFrame) {
            onSeekFrame(frameForX(pos.x()));
        }
        if (laneIndex >= lanes_.size() || lanes_[laneIndex].sourceFrames <= 0) {
            event->accept();
            return;
        }
        const QRect clip = clipRect(laneIndex);
        if (!clip.adjusted(-4, -6, 4, 6).contains(pos)) {
            event->accept();
            return;
        }
        constexpr int kEdgePx = 10;
        if (std::abs(pos.x() - clip.left()) <= kEdgePx) {
            dragMode_ = DragMode::LeftEdge;
        } else if (std::abs(pos.x() - clip.right()) <= kEdgePx) {
            dragMode_ = DragMode::RightEdge;
        } else {
            dragMode_ = DragMode::Move;
        }
        dragLane_ = laneIndex;
        dragStartX_ = pos.x();
        const LooperLane& lane = lanes_[laneIndex].lane;
        dragStartFrame_ = lane.startFrame;
        dragSourceStartFrame_ = sourceStart(laneIndex);
        dragSourceEndFrame_ = sourceEnd(laneIndex);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const QPoint pos = event->position().toPoint();
        if (dragMode_ == DragMode::Gain && dragLane_ >= 0 && dragLane_ < lanes_.size()) {
            const QRect slider = gainRect(dragLane_);
            const double t = 1.0 - qBound(0.0, static_cast<double>(pos.y() - slider.top()) / qMax(1, slider.height()), 1.0);
            pendingGainDb_ = -60.0 + t * 72.0;
            lanes_[dragLane_].lane.gainDb = pendingGainDb_;
            update();
            event->accept();
            return;
        }
        if (dragMode_ != DragMode::None && dragLane_ >= 0 && dragLane_ < lanes_.size()) {
            applyDragPreview(pos.x());
            update();
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || dragMode_ == DragMode::None) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const int lane = dragLane_;
        const DragMode mode = dragMode_;
        dragMode_ = DragMode::None;
        dragLane_ = -1;
        if (lane >= 0 && lane < lanes_.size()) {
            if (mode == DragMode::Gain) {
                if (onGainChanged) onGainChanged(lane, pendingGainDb_);
            } else if (onRegionCommitted) {
                onRegionCommitted(lane, lanes_[lane].lane.startFrame, sourceStart(lane), sourceEnd(lane));
            }
        }
        event->accept();
    }

private:
    enum class DragMode { None, Move, LeftEdge, RightEdge, Gain };

    static constexpr int kToolbarHeight = 34;
    static constexpr int kLaneHeight = 112;
    static constexpr int kHeaderWidth = 176;
    static constexpr int kPlusHeight = 34;

    int visualLaneCount() const
    {
        return qMax(1, lanes_.size());
    }

    void updateMinimumHeight()
    {
        setMinimumHeight(kToolbarHeight + visualLaneCount() * kLaneHeight + kPlusHeight + 12);
    }

    QRect timelineRect() const
    {
        return rect().adjusted(kHeaderWidth, kToolbarHeight, -1, -kPlusHeight - 8);
    }

    QRect laneRect(int row) const
    {
        return QRect(0, kToolbarHeight + row * kLaneHeight, width(), kLaneHeight);
    }

    QRect laneTimelineRect(int row) const
    {
        return laneRect(row).adjusted(kHeaderWidth, 0, -1, 0);
    }

    QRect plusRect() const
    {
        return QRect(0, kToolbarHeight + visualLaneCount() * kLaneHeight, width(), kPlusHeight);
    }

    qint64 sourceStart(int laneIndex) const
    {
        if (laneIndex < 0 || laneIndex >= lanes_.size()) return 0;
        const LooperLane& lane = lanes_[laneIndex].lane;
        return lane.loopStartFrame >= 0 ? qBound<qint64>(0, lane.loopStartFrame, qMax<qint64>(0, lanes_[laneIndex].sourceFrames - 1)) : 0;
    }

    qint64 sourceEnd(int laneIndex) const
    {
        if (laneIndex < 0 || laneIndex >= lanes_.size()) return 0;
        const qint64 start = sourceStart(laneIndex);
        const qint64 frames = lanes_[laneIndex].sourceFrames;
        return laneIndex < lanes_.size() && lanes_[laneIndex].lane.loopEndFrame > start
            ? qBound<qint64>(start + 1, lanes_[laneIndex].lane.loopEndFrame, frames)
            : frames;
    }

    qint64 visibleFrames(int laneIndex) const
    {
        return qMax<qint64>(1, sourceEnd(laneIndex) - sourceStart(laneIndex));
    }

    qint64 viewFrames() const
    {
        const int markerRate = markerSampleRate();
        qint64 frames = static_cast<qint64>(markerRate) * 8;
        if (playheadMs_ >= 0) frames = qMax(frames, playheadMs_ * markerRate / 1000);
        if (loopStartMs_ >= 0) frames = qMax(frames, loopStartMs_ * markerRate / 1000);
        if (loopEndMs_ >= 0) frames = qMax(frames, loopEndMs_ * markerRate / 1000);
        for (int i = 0; i < lanes_.size(); ++i) {
            frames = qMax(frames, lanes_[i].lane.startFrame + visibleFrames(i));
        }
        return qMax<qint64>(1, frames);
    }

    int markerSampleRate() const
    {
        return markerSampleRate_ > 0 ? markerSampleRate_ : 48000;
    }

    int xForFrame(qint64 frame) const
    {
        const QRect area = timelineRect();
        return area.left() + qBound(0, static_cast<int>(std::llround((static_cast<double>(frame) / viewFrames()) * area.width())), qMax(0, area.width()));
    }

    qint64 frameDeltaForX(double dx) const
    {
        const QRect area = timelineRect();
        return area.width() > 1 ? static_cast<qint64>(std::llround((dx / area.width()) * viewFrames())) : 0;
    }

    qint64 snapTimelineFrame(qint64 frame) const
    {
        frame = qMax<qint64>(0, frame);
        if (!gridLockEnabled_) {
            return frame;
        }
        const double beatFrames = static_cast<double>(markerSampleRate()) * 60.0 / bpm_;
        const qint64 beat = static_cast<qint64>(std::llround(static_cast<double>(frame) / beatFrames));
        return qMax<qint64>(0, static_cast<qint64>(std::llround(static_cast<double>(beat) * beatFrames)));
    }

    qint64 frameForX(double x) const
    {
        const QRect area = timelineRect();
        if (area.width() <= 1) {
            return 0;
        }
        const double clamped = qBound(static_cast<double>(area.left()), x, static_cast<double>(area.right()));
        return qBound<qint64>(0, static_cast<qint64>(std::llround(((clamped - area.left()) / area.width()) * viewFrames())), viewFrames());
    }

    QRect clipRect(int laneIndex) const
    {
        const QRect area = laneTimelineRect(laneIndex).adjusted(0, 12, 0, -12);
        const int left = xForFrame(lanes_[laneIndex].lane.startFrame);
        const int right = qMax(left + 1, xForFrame(lanes_[laneIndex].lane.startFrame + visibleFrames(laneIndex)));
        return QRect(QPoint(left, area.top()), QPoint(right, area.bottom())).normalized();
    }

    void drawToolbar(QPainter& painter)
    {
        painter.fillRect(QRect(0, 0, width(), kToolbarHeight), QColor(25, 29, 33));
        painter.setPen(QColor(72, 80, 88));
        painter.drawLine(0, kToolbarHeight - 1, width(), kToolbarHeight - 1);
        if (bpm_ > 0.0) {
            const QRect area = QRect(kHeaderWidth, 0, width() - kHeaderWidth, kToolbarHeight);
            const double beatFrames = static_cast<double>(markerSampleRate()) * 60.0 / bpm_;
            for (int beat = 0; beat < 128; ++beat) {
                const int x = xForFrame(static_cast<qint64>(std::llround(beat * beatFrames)));
                if (x >= area.right()) break;
                painter.setPen(beat % 4 == 0 ? QColor(90, 100, 110) : QColor(56, 62, 68));
                const int gridBottom = kToolbarHeight + visualLaneCount() * kLaneHeight - 1;
                painter.drawLine(x, 0, x, gridBottom);
                if (beat % 4 == 0) {
                    painter.setPen(QColor(168, 176, 184));
                    painter.drawText(x + 4, 20, QString::number(beat / 4 + 1));
                }
            }
        }
    }

    void drawOverlays(QPainter& painter)
    {
        const int top = kToolbarHeight;
        const int bottom = kToolbarHeight + visualLaneCount() * kLaneHeight - 1;
        if (bottom <= top) {
            return;
        }
        const int rate = markerSampleRate();
        auto drawMarker = [&](qint64 ms, const QColor& color, int width) {
            if (ms < 0) {
                return;
            }
            const int x = xForFrame(ms * rate / 1000);
            painter.setPen(QPen(color, width));
            painter.drawLine(x, top, x, bottom);
        };
        if (loopStartMs_ >= 0) {
            drawMarker(loopStartMs_, QColor(86, 210, 124), 2);
        }
        if (loopEndMs_ >= 0) {
            drawMarker(loopEndMs_, QColor(86, 210, 124), 2);
        }
        if (gridRunning_) {
            const double beatFrames = static_cast<double>(rate) * 60.0 / qMax(1.0, bpm_);
            const std::uint64_t totalBeats = qMax<std::uint64_t>(1ULL, static_cast<std::uint64_t>(std::ceil(static_cast<double>(viewFrames()) / beatFrames)));
            const qint64 currentBeatFrame = static_cast<qint64>(std::llround(static_cast<double>(gridAbsoluteBeat_ % totalBeats) * beatFrames));
            const int x = xForFrame(currentBeatFrame);
            painter.setPen(QPen(QColor(102, 198, 166, 150), 1));
            painter.drawLine(x, top, x, bottom);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(102, 198, 166));
            painter.drawEllipse(QPoint(x, kToolbarHeight - 8), 4, 4);
        }
        drawMarker(playheadMs_, QColor(232, 64, 64), 2);
    }

    void drawLane(QPainter& painter, int row)
    {
        const bool realLane = row < lanes_.size();
        const LooperLane lane = realLane ? lanes_[row].lane : LooperLane{QString(), QString(), QString(), QStringLiteral("Empty Track 1")};
        const QRect rowRect = laneRect(row);
        const bool selected = row == selectedLane_;
        painter.fillRect(rowRect.adjusted(0, 0, 0, -1), selected ? QColor(38, 45, 52) : QColor(31, 36, 41));
        painter.fillRect(rowRect.adjusted(kHeaderWidth, 0, 0, -1), QColor(18, 20, 22));
        painter.setPen(QColor(52, 60, 68));
        painter.drawLine(0, rowRect.bottom(), width(), rowRect.bottom());
        painter.drawLine(kHeaderWidth, rowRect.top(), kHeaderWidth, rowRect.bottom());

        drawLaneHeader(painter, row, lane, realLane);
        drawLaneWaveform(painter, row, realLane);
    }

    QRect controlRect(int row, const QString& control) const
    {
        const QRect lane = laneRect(row);
        if (control == QStringLiteral("mute")) return QRect(30, lane.top() + 36, 70, 20);
        if (control == QStringLiteral("solo")) return QRect(30, lane.top() + 59, 70, 20);
        if (control == QStringLiteral("arm")) return QRect(30, lane.top() + 82, 70, 20);
        if (control == QStringLiteral("rename")) return QRect(kHeaderWidth - 58, lane.top() + 9, 20, 20);
        if (control == QStringLiteral("remove")) return QRect(kHeaderWidth - 30, lane.top() + 9, 20, 20);
        return {};
    }

    QRect gainRect(int row) const
    {
        const QRect lane = laneRect(row);
        return QRect(8, lane.top() + 42, 12, lane.height() - 54);
    }

    QString controlAt(int row, const QPoint& pos) const
    {
        for (const QString& control : {QStringLiteral("mute"), QStringLiteral("solo"), QStringLiteral("arm"), QStringLiteral("rename"), QStringLiteral("remove")}) {
            if (controlRect(row, control).contains(pos)) return control;
        }
        if (gainRect(row).adjusted(-6, -2, 6, 2).contains(pos)) return QStringLiteral("gain");
        return {};
    }

    void drawLaneHeader(QPainter& painter, int row, const LooperLane& lane, bool realLane)
    {
        const QRect r = laneRect(row);
        painter.setPen(QColor(210, 216, 222));
        painter.drawText(
            QRect(28, r.top() + 8, kHeaderWidth - 92, 22),
            Qt::AlignLeft | Qt::AlignVCenter,
            lane.name.isEmpty() ? QStringLiteral("Empty Track %1").arg(row + 1) : lane.name);

        drawButton(painter, controlRect(row, QStringLiteral("mute")), QStringLiteral("Mute"), realLane && lane.muted ? QColor(188, 150, 72) : QColor(50, 56, 62));
        drawButton(painter, controlRect(row, QStringLiteral("solo")), QStringLiteral("Solo"), realLane && lane.solo ? QColor(84, 148, 112) : QColor(50, 56, 62));
        drawButton(painter, controlRect(row, QStringLiteral("arm")), QStringLiteral("Record"), row == armedLane_ ? QColor(190, 70, 70) : QColor(50, 56, 62));
        drawIconButton(painter, controlRect(row, QStringLiteral("rename")), QStringLiteral("pencil"), QColor(42, 48, 54));
        drawButton(painter, controlRect(row, QStringLiteral("remove")), QStringLiteral("X"), QColor(78, 38, 38));

        const QRect slider = gainRect(row);
        painter.fillRect(slider, QColor(18, 20, 22));
        painter.setPen(QColor(76, 86, 96));
        painter.drawRect(slider.adjusted(0, 0, -1, -1));
        const double t = qBound(0.0, (lane.gainDb + 60.0) / 72.0, 1.0);
        const int y = slider.bottom() - static_cast<int>(std::llround(t * slider.height()));
        painter.fillRect(QRect(slider.left(), y, slider.width(), slider.bottom() - y + 1), QColor(102, 198, 166));
    }

    void drawButton(QPainter& painter, const QRect& rect, const QString& text, const QColor& fill)
    {
        painter.fillRect(rect, fill);
        painter.setPen(QColor(94, 104, 114));
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(230, 234, 236));
        painter.drawText(rect, Qt::AlignCenter, text);
    }

    void drawIconButton(QPainter& painter, const QRect& rect, const QString& icon, const QColor& fill)
    {
        painter.fillRect(rect, fill);
        painter.setPen(QColor(94, 104, 114));
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        painter.setPen(QPen(QColor(230, 234, 236), 2));
        if (icon == QStringLiteral("pencil")) {
            painter.drawLine(rect.left() + 5, rect.bottom() - 5, rect.right() - 5, rect.top() + 5);
            painter.drawLine(rect.left() + 4, rect.bottom() - 4, rect.left() + 7, rect.bottom() - 3);
        }
    }

    void drawLaneWaveform(QPainter& painter, int row, bool realLane)
    {
        const QRect area = laneTimelineRect(row).adjusted(0, 12, -1, -12);
        painter.setPen(QColor(44, 50, 56));
        painter.drawLine(area.left(), area.center().y(), area.right(), area.center().y());
        if (!realLane || row >= lanes_.size() || lanes_[row].sourceFrames <= 0 || lanes_[row].peaks.empty()) {
            painter.setPen(QColor(90, 98, 106));
            painter.drawLine(area.left(), area.center().y(), area.right(), area.center().y());
            return;
        }
        const QRect clip = clipRect(row);
        painter.fillRect(clip, QColor(36, 62, 92));
        painter.setPen(QColor(132, 205, 180));
        const auto& peaks = lanes_[row].peaks;
        const qint64 firstSourceFrame = sourceStart(row);
        const qint64 croppedFrames = visibleFrames(row);
        const qint64 sourceFrames = lanes_[row].sourceFrames;
        for (int x = clip.left(); x <= clip.right(); ++x) {
            const qint64 frameOffset = static_cast<qint64>(x - clip.left()) * croppedFrames /
                qMax(1, clip.width());
            const qint64 sourceFrame = qMin(sourceFrames - 1, firstSourceFrame + frameOffset);
            const int index = static_cast<int>(qBound<qint64>(
                0,
                sourceFrame * static_cast<qint64>(peaks.size()) / sourceFrames,
                static_cast<qint64>(peaks.size()) - 1));
            const int half = qMax(2, static_cast<int>(peaks[index] * (clip.height() / 2 - 4)));
            painter.drawLine(x, clip.center().y() - half, x, clip.center().y() + half);
        }
        painter.fillRect(QRect(clip.left(), clip.top(), 6, clip.height()), QColor(115, 190, 255, 150));
        painter.fillRect(QRect(clip.right() - 5, clip.top(), 6, clip.height()), QColor(115, 190, 255, 150));
        painter.setPen(QColor(190, 218, 255));
        painter.drawText(clip.adjusted(6, 2, -6, -2), Qt::AlignLeft | Qt::AlignTop, lanes_[row].lane.name);
    }

    int laneAt(const QPoint& pos) const
    {
        if (pos.y() < kToolbarHeight || pos.y() >= kToolbarHeight + visualLaneCount() * kLaneHeight) return -1;
        return qBound(0, (pos.y() - kToolbarHeight) / kLaneHeight, visualLaneCount() - 1);
    }

    void selectLane(int lane)
    {
        selectedLane_ = lane;
        if (onSelected) onSelected(lane);
        update();
    }

    void beginGainDrag(int lane, int)
    {
        dragMode_ = DragMode::Gain;
        dragLane_ = lane;
        pendingGainDb_ = lanes_.value(lane).lane.gainDb;
    }

    void applyDragPreview(double x)
    {
        if (dragLane_ < 0 || dragLane_ >= lanes_.size()) return;
        const qint64 delta = frameDeltaForX(x - dragStartX_);
        LooperLane& lane = lanes_[dragLane_].lane;
        if (dragMode_ == DragMode::Move) {
            lane.startFrame = snapTimelineFrame(dragStartFrame_ + delta);
        } else if (dragMode_ == DragMode::LeftEdge) {
            const qint64 minSourceStart = qMax<qint64>(0, dragSourceStartFrame_ - dragStartFrame_);
            const qint64 snappedTimelineStart = snapTimelineFrame(dragStartFrame_ + delta);
            const qint64 snappedSourceStart = dragSourceStartFrame_ + snappedTimelineStart - dragStartFrame_;
            const qint64 next = qBound<qint64>(minSourceStart, snappedSourceStart, dragSourceEndFrame_ - 1);
            lane.loopStartFrame = next == 0 && dragSourceEndFrame_ == lanes_[dragLane_].sourceFrames ? -1 : next;
            lane.loopEndFrame = next == 0 && dragSourceEndFrame_ == lanes_[dragLane_].sourceFrames ? -1 : dragSourceEndFrame_;
            lane.startFrame = dragStartFrame_ + (next - dragSourceStartFrame_);
        } else if (dragMode_ == DragMode::RightEdge) {
            const qint64 originalTimelineEnd = dragStartFrame_ + (dragSourceEndFrame_ - dragSourceStartFrame_);
            const qint64 snappedTimelineEnd = snapTimelineFrame(originalTimelineEnd + delta);
            const qint64 snappedSourceEnd = dragSourceStartFrame_ + snappedTimelineEnd - dragStartFrame_;
            const qint64 next = qBound<qint64>(dragSourceStartFrame_ + 1, snappedSourceEnd, lanes_[dragLane_].sourceFrames);
            lane.loopStartFrame = dragSourceStartFrame_ == 0 && next == lanes_[dragLane_].sourceFrames ? -1 : dragSourceStartFrame_;
            lane.loopEndFrame = dragSourceStartFrame_ == 0 && next == lanes_[dragLane_].sourceFrames ? -1 : next;
        }
        lane.stopFrame = lane.startFrame + visibleFrames(dragLane_);
    }

    QVector<LaneView> lanes_;
    int selectedLane_ = -1;
    int activeBank_ = 0;
    int armedLane_ = -1;
    double bpm_ = 120.0;
    bool gridLockEnabled_ = true;
    std::uint64_t gridAbsoluteBeat_ = 0;
    bool gridRunning_ = false;
    qint64 playheadMs_ = -1;
    qint64 loopStartMs_ = -1;
    qint64 loopEndMs_ = -1;
    int markerSampleRate_ = 48000;
    DragMode dragMode_ = DragMode::None;
    int dragLane_ = -1;
    double dragStartX_ = 0.0;
    qint64 dragStartFrame_ = 0;
    qint64 dragSourceStartFrame_ = 0;
    qint64 dragSourceEndFrame_ = 0;
    double pendingGainDb_ = 0.0;
};

class LevelMeterWidget : public QWidget {
public:
    explicit LevelMeterWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumWidth(180);
        setFixedHeight(18);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setLevel(double level)
    {
        const double next = qBound(0.0, level, 1.0);
        if (!isEnabled()) {
            level_ = next;
            update();
            return;
        }
        const double delta = next - level_;
        if (std::abs(delta) < 0.008) {
            return;
        }
        const double smoothing = delta > 0.0 ? 0.45 : 0.10;
        level_ += delta * smoothing;
        if (level_ < 0.001 && next <= 0.001) {
            level_ = 0.0;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRect bar = rect().adjusted(0, 2, 0, -2);
        const bool active = isEnabled();
        painter.fillRect(bar, active ? QColor(28, 32, 36) : QColor(34, 36, 38));
        const int safeEnd = bar.left() + static_cast<int>(bar.width() * 0.50);
        const int warnEnd = bar.left() + static_cast<int>(bar.width() * 0.89);
        painter.fillRect(QRect(QPoint(safeEnd, bar.top()), QPoint(warnEnd, bar.bottom())),
            active ? QColor(72, 56, 24) : QColor(45, 42, 38));
        painter.fillRect(QRect(QPoint(warnEnd, bar.top()), QPoint(bar.right(), bar.bottom())),
            active ? QColor(76, 30, 30) : QColor(46, 38, 38));

        const int fillWidth = qBound(0, static_cast<int>(std::llround(level_ * static_cast<double>(bar.width()))), bar.width());
        QColor fill = active ? QColor(94, 188, 132) : QColor(82, 88, 92);
        if (active && level_ >= 0.891) {
            fill = QColor(224, 74, 74);
        } else if (active && level_ >= 0.501) {
            fill = QColor(230, 151, 55);
        }
        painter.fillRect(QRect(bar.left(), bar.top(), fillWidth, bar.height()), fill);
        painter.setPen(active ? QColor(86, 94, 102) : QColor(58, 62, 66));
        painter.drawRect(bar.adjusted(0, 0, -1, -1));
    }

private:
    double level_ = 0.0;
};

namespace {

class Jam2Style final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget = nullptr) const override
    {
        if (element != PE_IndicatorCheckBox) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }

        const QRect box = option->rect.adjusted(1, 1, -1, -1);
        const bool enabled = (option->state & State_Enabled) != 0;
        const bool checked = (option->state & State_On) != 0;
        const bool hovered = (option->state & State_MouseOver) != 0;
        const bool focused = (option->state & State_HasFocus) != 0;
        const QColor border = enabled
            ? (hovered || focused ? QColor(QStringLiteral("#66c6a6")) : QColor(QStringLiteral("#8a97a1")))
            : QColor(QStringLiteral("#4d565d"));
        const QColor fill = checked ? QColor(QStringLiteral("#1b3b33")) : QColor(QStringLiteral("#101214"));
        const QColor tick = enabled ? QColor(QStringLiteral("#f2f5f7")) : QColor(QStringLiteral("#7a858c"));

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(border, 1.25));
        painter->setBrush(fill);
        painter->drawRect(box);

        if (checked) {
            painter->setPen(QPen(tick, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath path;
            path.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.54);
            path.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
            path.lineTo(box.left() + box.width() * 0.77, box.top() + box.height() * 0.30);
            painter->drawPath(path);
        }
        painter->restore();
    }
};

void installJam2Style()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    QApplication::setStyle(new Jam2Style(QApplication::style()));
    installed = true;
}

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

QString doubleText(const QJsonObject& object, const QString& key, const QString& suffix, int precision = 1)
{
    return QString::number(object.value(key).toDouble(), 'f', precision) + suffix;
}

bool hasNumber(const QJsonObject& object, const QString& key)
{
    return object.contains(key) && object.value(key).isDouble();
}

QString numberText(const QJsonObject& object, const QString& key, const QString& suffix = QString(), int precision = 1)
{
    if (!hasNumber(object, key)) {
        return QStringLiteral("-");
    }
    return QString::number(object.value(key).toDouble(), 'f', precision) + suffix;
}

QString integerText(const QJsonObject& object, const QString& key, const QString& fallbackKey = QString())
{
    QString actualKey = key;
    if (!hasNumber(object, actualKey) && !fallbackKey.isEmpty()) {
        actualKey = fallbackKey;
    }
    if (!hasNumber(object, actualKey)) {
        return QStringLiteral("-");
    }
    return QString::number(static_cast<qlonglong>(object.value(actualKey).toDouble()));
}

QString durationText(double ms)
{
    if (ms >= 1000.0) {
        return QString::number(ms / 1000.0, 'f', 2) + QStringLiteral(" s");
    }
    return QString::number(ms, 'f', 1) + QStringLiteral(" ms");
}

QString metricText(
    const QJsonObject& object,
    const QString& key,
    const QString& suffix = QString(),
    int precision = 1,
    const QString& fallbackKey = QString())
{
    QString actualKey = key;
    if (!hasNumber(object, actualKey) && !fallbackKey.isEmpty()) {
        actualKey = fallbackKey;
    }
    return numberText(object, actualKey, suffix, precision);
}

double metricValue(const QJsonObject& object, const QString& key, double fallback = 0.0)
{
    return hasNumber(object, key) ? object.value(key).toDouble() : fallback;
}

double percentOf(double part, double whole)
{
    return whole > 0.0 ? part * 100.0 / whole : 0.0;
}

double perMinute(double count, double elapsedMs)
{
    return elapsedMs > 0.0 ? count * 60000.0 / elapsedMs : 0.0;
}

QString percentText(double value, int precision = 2)
{
    return QString::number(value, 'f', precision) + QStringLiteral("%");
}

QString diagnoseStats(const QJsonObject& stats)
{
    if (!hasNumber(stats, QStringLiteral("elapsed_ms"))) {
        return QStringLiteral("Diagnosis -");
    }

    QStringList findings;
    const double elapsedMs = metricValue(stats, QStringLiteral("elapsed_ms"));
    const double recvPackets = metricValue(stats, QStringLiteral("recv_packets"));
    const double frameSize = metricValue(stats, QStringLiteral("frame_size"));
    const double receivedFrames = recvPackets * frameSize;
    const double packetGapSamples = metricValue(stats, QStringLiteral("audio_packet_gap_samples"));
    const double lossPercent = metricValue(stats, QStringLiteral("sequence_loss_percent"));
    const double underrunMs = metricValue(stats, QStringLiteral("playback_ring_underrun_time_ms"));
    const double underrunEvents = metricValue(stats, QStringLiteral("playback_ring_underrun_events"));
    const double underrunBurstMaxMs = metricValue(stats, QStringLiteral("playback_ring_underrun_burst_max_ms"));
    const double gapOver4x = metricValue(stats, QStringLiteral("audio_packet_gap_over_4x_count"));
    const double reorderedLost = metricValue(stats, QStringLiteral("reordered_lost"));
    const double sequenceLate = metricValue(stats, QStringLiteral("sequence_late"));
    const double lateFrames = metricValue(stats, QStringLiteral("late_audio_frames_dropped"));
    const double missingFrames = metricValue(stats, QStringLiteral("missing_audio_frames_inserted"));
    const double driftPpm = std::abs(metricValue(stats, QStringLiteral("drift_ppm")));

    const double underrunPercent = percentOf(underrunMs, elapsedMs);
    const double underrunEventsPerMinute = perMinute(underrunEvents, elapsedMs);
    const double gapOver4xPercent = percentOf(gapOver4x, packetGapSamples);
    const double gapOver4xPerMinute = perMinute(gapOver4x, elapsedMs);
    const double reorderLatePercent = percentOf(reorderedLost + sequenceLate, recvPackets);
    const double lateFramePercent = percentOf(lateFrames, lateFrames + missingFrames + receivedFrames);
    const double missingPressurePercent = percentOf(missingFrames, missingFrames + lateFrames + receivedFrames);

    if (underrunPercent >= 0.10 || underrunEventsPerMinute >= 2.0 || underrunBurstMaxMs >= 10.0) {
        findings << QStringLiteral("Underrun %1: +prefill/+max").arg(percentText(underrunPercent));
    }
    if (gapOver4xPercent >= 0.50 || gapOver4xPerMinute >= 2.0) {
        findings << QStringLiteral("Bursts %1: +prefill/+adaptive max").arg(percentText(gapOver4xPercent));
    }
    if (lossPercent >= 0.50) {
        findings << QStringLiteral("Loss %1: +frame/wired").arg(metricText(stats, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2));
    }
    if (reorderLatePercent >= 0.25 || lateFramePercent >= 0.25) {
        findings << QStringLiteral("Late/reorder %1: +playout").arg(percentText(qMax(reorderLatePercent, lateFramePercent)));
    }
    if (missingPressurePercent >= 0.25 && findings.size() < 2) {
        findings << QStringLiteral("Missing %1: +playout/prefill").arg(percentText(missingPressurePercent));
    }
    if (driftPpm >= 200.0 && findings.size() < 2) {
        findings << QStringLiteral("Drift %1ppm: +correction").arg(metricText(stats, QStringLiteral("drift_ppm"), QString{}, 0));
    }

    if (findings.isEmpty()) {
        return QStringLiteral("Diagnosis OK");
    }
    while (findings.size() > 2) {
        findings.removeLast();
    }
    return QStringLiteral("Diagnosis ") + findings.join(QStringLiteral(" | "));
}

QString jamSliderStyle();
void applyJamSliderStyle(QSlider* slider);

struct FocusPreset {
    double frequencyHz = 120.0;
    double gainDb = 12.0;
    double q = 6.0;
};

FocusPreset focusPresetForKey(const QString& key)
{
    if (key == QStringLiteral("bass")) {
        return {95.0, 14.0, 7.0};
    }
    if (key == QStringLiteral("guitar")) {
        return {850.0, 12.0, 5.0};
    }
    if (key == QStringLiteral("vocals")) {
        return {1800.0, 12.0, 4.5};
    }
    if (key == QStringLiteral("drums")) {
        return {3200.0, 10.0, 3.5};
    }
    return {120.0, 12.0, 6.0};
}

bool isCustomFocusPreset(const QString& key)
{
    return key.isEmpty() || key == QStringLiteral("custom");
}

QString trackValueEditorStyle()
{
    return QStringLiteral(
        "QAbstractSpinBox { border: 1px solid #52616c; background: #101214; color: #f2f5f7; padding: 2px 28px 2px 6px; }"
        "QComboBox, QLineEdit { border: 1px solid #52616c; background: #101214; color: #f2f5f7; padding: 2px 6px; }"
        "QAbstractSpinBox:focus, QComboBox:focus, QLineEdit:focus { border: 1px solid #66c6a6; }"
        "QAbstractSpinBox:disabled, QComboBox:disabled, QLineEdit:disabled { border: 1px solid #343c42; background: #171a1d; color: #6f7a82; }");
}

void applyMutedEditorStyle(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->setAttribute(Qt::WA_MacShowFocusRect, false);
    widget->setStyleSheet(trackValueEditorStyle());
    if (auto* combo = qobject_cast<QComboBox*>(widget); combo != nullptr && combo->lineEdit() != nullptr) {
        combo->lineEdit()->setAttribute(Qt::WA_MacShowFocusRect, false);
        combo->lineEdit()->setStyleSheet(trackValueEditorStyle());
    }
}

void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin)
{
    if (manualStopCheck == nullptr || durationSpin == nullptr) {
        return;
    }
    durationSpin->setEnabled(!manualStopCheck->isChecked());
}

void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin, QLabel* durationLabel)
{
    updateCaptureDurationControl(manualStopCheck, durationSpin);
    if (durationLabel != nullptr && manualStopCheck != nullptr) {
        durationLabel->setEnabled(!manualStopCheck->isChecked());
    }
}

QString deviceId(const QString& text)
{
    const QRegularExpression re(QStringLiteral("^\\s*\\[?(\\d+)\\]?"));
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1) : text.trimmed();
}

struct WavMetadata {
    int audioFormat = 0;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataBytes = 0;
    int durationMs = 0;
    QString sha256;
};

WavMetadata readWavMetadata(const QString& path)
{
    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(nativeFilePath(path));
    if (!inspected) {
        throw std::runtime_error(inspected.error);
    }
    WavMetadata meta;
    meta.audioFormat = 1;
    meta.sampleRate = static_cast<int>(inspected.info.sample_rate);
    meta.channels = static_cast<int>(inspected.info.channels);
    meta.bitsPerSample = 16;
    meta.dataBytes = static_cast<qint64>(inspected.info.data_bytes);
    const std::uint64_t duration_ms = inspected.info.frames * 1000ULL / inspected.info.sample_rate;
    meta.durationMs = static_cast<int>(std::min<std::uint64_t>(duration_ms, std::numeric_limits<int>::max()));
    meta.sha256 = sha256FileHex(path);
    if (meta.sha256.isEmpty()) {
        throw std::runtime_error("failed to hash WAV");
    }
    return meta;
}

struct StagedPcm16Asset {
    QString sourcePath;
    QString stagedPath;
    QString displayName;
    QString sha256;
    WavMetadata metadata;
    QString error;
};

StagedPcm16Asset stagePcm16Asset(const QString& sourcePath, const QString& stagingFolder)
{
    StagedPcm16Asset result;
    const QFileInfo sourceInfo(sourcePath);
    result.sourcePath = sourceInfo.absoluteFilePath();
    result.displayName = sourceInfo.completeBaseName();
    try {
        result.metadata = readWavMetadata(result.sourcePath);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        return result;
    }
    if (result.metadata.dataBytes <= 0) {
        result.error = QStringLiteral("WAV contains no audio frames");
        return result;
    }
    result.sha256 = result.metadata.sha256;
    result.stagedPath = QDir(stagingFolder).absoluteFilePath(
        QStringLiteral("wavs/") + result.sha256 + QStringLiteral(".wav"));
    if (!QDir().mkpath(QFileInfo(result.stagedPath).absolutePath())) {
        result.error = QStringLiteral("could not create the WAV staging folder");
        return result;
    }
    if (QFileInfo::exists(result.stagedPath) && sha256FileHex(result.stagedPath) == result.sha256) {
        return result;
    }

    QFile source(result.sourcePath);
    QSaveFile destination(result.stagedPath);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("could not open the WAV for atomic staging");
        return result;
    }
    constexpr qint64 copyBlockBytes = 1024 * 1024;
    while (!source.atEnd()) {
        const QByteArray block = source.read(copyBlockBytes);
        if (block.isEmpty() && source.error() != QFileDevice::NoError) {
            result.error = QStringLiteral("failed while reading the WAV for staging");
            return result;
        }
        if (destination.write(block) != block.size()) {
            result.error = QStringLiteral("failed while writing the staged WAV");
            return result;
        }
    }
    if (!destination.commit()) {
        result.error = QStringLiteral("could not atomically commit the staged WAV");
        return result;
    }
    return result;
}

QString jamSliderStyle()
{
    return QStringLiteral(
        "QSlider::groove:horizontal { height: 6px; background: #2a3035; border: 1px solid #3d464d; }"
        "QSlider::sub-page:horizontal { background: #66c6a6; border: 1px solid #66c6a6; }"
        "QSlider::add-page:horizontal { background: #1b1f23; border: 1px solid #3d464d; }"
        "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; background: #f2f5f7; border: 1px solid #66c6a6; }"
        "QSlider::groove:horizontal:disabled { background: #2e3235; border: 1px solid #454b50; }"
        "QSlider::sub-page:horizontal:disabled { background: #5e666c; border: 1px solid #5e666c; }"
        "QSlider::add-page:horizontal:disabled { background: #25282b; border: 1px solid #454b50; }"
        "QSlider::handle:horizontal:disabled { background: #8a9298; border: 1px solid #6b7379; }");
}

void applyJamSliderStyle(QSlider* slider)
{
    if (slider != nullptr) {
        slider->setStyleSheet(jamSliderStyle());
    }
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

QString dbText(double db)
{
    return QStringLiteral("%1%2 dB")
        .arg(db >= 0.0 ? QStringLiteral("+") : QString())
        .arg(db, 0, 'f', 1);
}

double gainFromDb(double db)
{
    if (db <= -60.0) {
        return 0.0;
    }
    return std::pow(10.0, db / 20.0);
}

double dbFromGain(double gain)
{
    if (gain <= 0.0) {
        return -60.0;
    }
    return qBound(-60.0, 20.0 * std::log10(gain), 12.0);
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
    QJsonObject decoded;
    record(QStringLiteral("control.valid-authenticated-frame"),
        framed && decodeAuthenticated(body, directionKey, 7, decoded, error) && decoded == controlMessage,
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

    QString modelError;
    record(QStringLiteral("model.accept-valid-beat"),
        validateControlMessageShape(controlMessage, modelError), modelError);
    QJsonObject invalidBeat = controlMessage;
    invalidBeat.insert(QStringLiteral("beat"), 512);
    modelError.clear();
    record(QStringLiteral("model.reject-out-of-range-beat"),
        !validateControlMessageShape(invalidBeat, modelError), modelError);
    modelError.clear();
    record(QStringLiteral("authorization.reject-unknown-family"),
        !validateControlMessageShape(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("debug.set-remote-state")}},
            modelError),
        modelError);
    QJsonArray invalidBanks;
    for (int index = 0; index < 5; ++index) {
        invalidBanks.append(QJsonObject{{QStringLiteral("lanes"), QJsonArray{}}});
    }
    const QJsonObject invalidSong{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("arrangement_revision"), 1},
        {QStringLiteral("song"), QJsonObject{
            {QStringLiteral("looper"), QJsonObject{{QStringLiteral("banks"), invalidBanks}}}}},
    };
    modelError.clear();
    record(QStringLiteral("asset-model.reject-excessive-banks"),
        !validateControlMessageShape(invalidSong, modelError), modelError);

    if (fixtureSpecs.size() > 32) {
        record(QStringLiteral("wav.fixture-count-bound"), false, QStringLiteral("too many fixture specs"));
    } else {
        for (const QString& spec : fixtureSpecs) {
            const qsizetype separator = spec.indexOf(QLatin1Char(':'));
            const QString expectation = separator > 0 ? spec.left(separator) : QString();
            const QString path = separator > 0 ? spec.mid(separator + 1) : QString();
            if ((expectation != QStringLiteral("valid") && expectation != QStringLiteral("invalid")) ||
                path.isEmpty() || path.toUtf8().size() > 4096) {
                record(QStringLiteral("wav.fixture-spec-bound"), false, QStringLiteral("invalid fixture spec"));
                continue;
            }
            const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                nativeFilePath(path), 8ULL * 1024ULL * 1024ULL);
            const bool expectedValid = expectation == QStringLiteral("valid");
            record(
                QStringLiteral("wav.%1.%2").arg(expectation, QFileInfo(path).fileName()),
                static_cast<bool>(inspected) == expectedValid,
                QString::fromStdString(inspected.error));
        }
    }

    return QJsonObject{
        {QStringLiteral("event"), QStringLiteral("debug_boundary_result")},
        {QStringLiteral("schema"), 1},
        {QStringLiteral("ok"), allOk},
        {QStringLiteral("cases"), cases},
    };
}

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    fileWorkerPool_.setMaxThreadCount(2);
    fileWorkerPool_.setExpiryTimeout(30000);
    installJam2Style();
    generateSession();
    transientProjectFolder_ = appReleaseFolderPath(
        QStringLiteral("song_staging/") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QDir().mkpath(QDir(transientProjectFolder_).absoluteFilePath(QStringLiteral("wavs")));
    buildUi();
    savedProjectSnapshot_ = currentProjectSnapshot();
    QApplication::instance()->installEventFilter(this);

    jam2_.onOutputLine = [this](const QString& line) { handleOutputLine(line); };
    jam2_.onErrorLine = [this](const QString& line) { appendLog(QStringLiteral("stderr: ") + line); };
    jam2_.onControlFrame = [this](quint16 type, const QByteArray& payload) {
        handleGuiControlFrame(type, payload);
    };
    jam2_.onFinished = [this](int code) {
        const bool endedJam = !localEngineActive_;
        appendLog(QStringLiteral("jam2 exited rc=%1").arg(code));
        localEngineActive_ = false;
        if (replacingLocalEngine_) {
            replacingLocalEngine_ = false;
            return;
        }
        if (meshRestarting_) {
            meshRestarting_ = false;
            return;
        }
        connectionLabel_->setText(QStringLiteral("Stopped"));
        controlReconnectEnabled_ = false;
        controlReconnectTimer_.stop();
        controlServer_.close();
        controlClient_.close();
        pendingJoinLaunch_ = false;
        pendingJoinSettingsReady_ = false;
        pendingJoinMembershipReady_ = false;
        pendingJoinBaseArgs_.clear();
        meshActive_ = false;
        localMeshPeerTokens_.clear();
        meshPeerEndpoints_.clear();
        currentMeshPeers_.clear();
        pendingMeshInvitePopup_ = false;
        activePublicEndpoint_.clear();
        remotePeerConnected_ = false;
        localMetronomeRunning_ = false;
        localMetronomeLeader_ = false;
        playbackGrid_.clearEngine();
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
        if (refreshControlButton_) {
            refreshControlButton_->setEnabled(false);
        }
        jamRecordingActive_ = false;
        updateJamRecordingControls();
        updateMixControls();
        setMixRemotePeerVisible(false);
        const bool restoreLocal = !shuttingDown_ && (returnToLocalAfterStop_ || endedJam);
        returnToLocalAfterStop_ = false;
        if (restoreLocal) {
            QTimer::singleShot(0, this, [this] {
                if (!shuttingDown_ && !jam2_.isRunning()) {
                    startLocalPerform();
                }
            });
        }
    };
    controlServer_.onState = [this](const QString& state) { handleControlState(state, true); };
    controlServer_.onMessage = [this](const QString& sourcePeerToken, const QJsonObject& message) {
        handleControlMessage(message, sourcePeerToken);
    };
    controlServer_.onAuthenticated = [this](const QString& token, const QJsonObject& message) {
        handleMeshPeerAuthenticated(token, message);
    };
    controlServer_.onDisconnected = [this](const QString& token) {
        localMeshPeerTokens_.remove(token);
        if (meshActive_ && meshPeerEndpoints_.remove(token) > 0) {
            broadcastMeshPeerList();
            updateMixRemotePeers();
            restartMeshEngineFromPeerList();
        }
    };
    controlClient_.onState = [this](const QString& state) { handleControlState(state, false); };
    controlClient_.onMessage = [this](const QJsonObject& message) { handleControlMessage(message); };
    controlReconnectTimer_.setInterval(2000);
    QObject::connect(&controlReconnectTimer_, &QTimer::timeout, this, [this] { refreshControlConnection(); });
    outgoingLooperAssetTimer_.setInterval(5);
    QObject::connect(&outgoingLooperAssetTimer_, &QTimer::timeout, this, [this] { continueLooperAssetSend(); });
    songAssetCheckRetryTimer_.setSingleShot(true);
    QObject::connect(&songAssetCheckRetryTimer_, &QTimer::timeout, this, [this] {
        const QJsonObject message = deferredSongSetMessage_;
        deferredSongSetMessage_ = QJsonObject{};
        if (!message.isEmpty()) {
            handleSongSet(message);
        }
    });
    incomingLooperAssetTimer_.setSingleShot(true);
    QObject::connect(&incomingLooperAssetTimer_, &QTimer::timeout, this, [this] {
        appendLog(QStringLiteral("looper asset receive progress timeout"));
        resetIncomingLooperAsset();
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
    bool hasTransientWavs = false;
    for (const QString& path : std::as_const(transientTrackWavs_)) {
        if (QFileInfo::exists(path)) {
            hasTransientWavs = true;
            break;
        }
    }
    hasTransientWavs = hasTransientWavs || !pendingTransientCapturePath_.isEmpty();
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
    if (!pendingTransientCapturePath_.isEmpty()) {
        registerTransientTrackWav(pendingTransientCapturePath_);
        pendingTransientCapturePath_.clear();
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

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Jam2"));
    setMinimumSize(1280, 720);

    titleLabel_ = new QLabel(QStringLiteral("Jam2"), this);
    titleLabel_->setObjectName(QStringLiteral("AppTitle"));
    connectionLabel_ = new QLabel(QStringLiteral("Idle"), this);
    connectionLabel_->setObjectName(QStringLiteral("StatusPill"));
    jitterLabel_ = new QLabel(QStringLiteral("Jitter -"), this);
    jitterLabel_->setObjectName(QStringLiteral("StatusPill"));
    lossLabel_ = new QLabel(QStringLiteral("Loss -"), this);
    lossLabel_->setObjectName(QStringLiteral("StatusPill"));

    auto* header = new QHBoxLayout();
    header->addWidget(titleLabel_);
    header->addStretch(1);
    engineModeLabel_ = new QLabel(QStringLiteral("Local"), this);
    engineModeLabel_->setObjectName(QStringLiteral("StatusPill"));
    engineModeLabel_->setAlignment(Qt::AlignCenter);
    engineModeLabel_->setMinimumWidth(72);
    header->addWidget(engineModeLabel_);
    header->addWidget(connectionLabel_);

    songTitleEdit_ = new QLineEdit(chordModel_.title(), this);
    songTitleEdit_->setMinimumWidth(300);
    auto* newSongButton = new QPushButton(QStringLiteral("New Song"), this);
    auto* openSongButton = new QPushButton(QStringLiteral("Open"), this);
    auto* saveSongButton = new QPushButton(QStringLiteral("Save"), this);
    auto* library = new QHBoxLayout();
    library->addWidget(new QLabel(QStringLiteral("Song"), this));
    library->addWidget(songTitleEdit_, 1);
    library->addWidget(newSongButton);
    library->addWidget(openSongButton);
    library->addWidget(saveSongButton);

    QObject::connect(songTitleEdit_, &QLineEdit::editingFinished, this, [this] {
        chordModel_.setTitle(songTitleEdit_->text());
        beatModel_.setTitle(songTitleEdit_->text());
        lyricModel_.setTitle(songTitleEdit_->text());
        sendSongSnapshot();
    });
    QObject::connect(newSongButton, &QPushButton::clicked, this, [this] { newSong(); });
    QObject::connect(openSongButton, &QPushButton::clicked, this, [this] { openSong(); });
    QObject::connect(saveSongButton, &QPushButton::clicked, this, [this] { saveSong(); });

    QWidget* sessionPage = buildSessionPage();

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildSongPage(), QStringLiteral("Chord View"));
    beatGrid_ = new BeatGridWidget(&beatModel_, QStringLiteral("beat"), this);
    tabs_->addTab(beatGrid_, QStringLiteral("Beat View"));
    lyricGrid_ = new BeatGridWidget(&lyricModel_, QStringLiteral("lyric"), this);
    tabs_->addTab(lyricGrid_, QStringLiteral("Lyrics"));
    tabs_->addTab(buildTrackPage(), QStringLiteral("Track"));
    tabs_->addTab(buildMetronomePage(), QStringLiteral("Metronome"));
    tabs_->addTab(buildMixPage(), QStringLiteral("Mix"));

    auto sendCellEdit = [this](int section, const QString& lane, int beat, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    beatGrid_->onCellEdited = sendCellEdit;
    lyricGrid_->onCellEdited = sendCellEdit;
    beatGrid_->onBeatHitEdited = [this](int section, int beat, int lane, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.hit")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("text"), text},
        });
    };
    beatGrid_->onBeatDivisionChanged = [this](int section, int beat, int division, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.division")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("division"), division},
        });
    };
    lyricGrid_->onLyricsEdited = [this](const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("lyrics.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("text"), text},
        });
    };
    beatGrid_->onStructureChanged = [this] {
        sendSongSnapshot();
    };
    lyricGrid_->onStructureChanged = [this] {
        sendSongSnapshot();
    };
    beatGrid_->onGridResized = [this](int section, int beats, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("beat")},
            {QStringLiteral("beats"), beats},
        });
    };
    lyricGrid_->onGridResized = beatGrid_->onGridResized;

    latencyLabel_ = new QLabel(QStringLiteral("Latency -"), this);
    latencyLabel_->setObjectName(QStringLiteral("StatusPill"));
    depthLabel_ = new QLabel(QStringLiteral("Depth -"), this);
    depthLabel_->setObjectName(QStringLiteral("StatusPill"));
    ringDepthLabel_ = new QLabel(QStringLiteral("Ring -"), this);
    ringDepthLabel_->setObjectName(QStringLiteral("StatusPill"));
    underrunLabel_ = new QLabel(QStringLiteral("Underrun -"), this);
    underrunLabel_->setObjectName(QStringLiteral("StatusPill"));
    missingFramesLabel_ = new QLabel(QStringLiteral("Missing -"), this);
    missingFramesLabel_->setObjectName(QStringLiteral("StatusPill"));
    driftLabel_ = new QLabel(QStringLiteral("Drift -"), this);
    driftLabel_->setObjectName(QStringLiteral("StatusPill"));
    diagnosisLabel_ = new QLabel(QStringLiteral("Diagnosis -"), this);
    diagnosisLabel_->setObjectName(QStringLiteral("StatusPill"));
    diagnosisLabel_->setMinimumWidth(260);
    diagnosisLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* footer = new QHBoxLayout();
    footer->addWidget(latencyLabel_);
    footer->addWidget(jitterLabel_);
    footer->addWidget(lossLabel_);
    footer->addWidget(depthLabel_);
    footer->addWidget(ringDepthLabel_);
    footer->addWidget(underrunLabel_);
    footer->addWidget(missingFramesLabel_);
    footer->addWidget(driftLabel_);
    footer->addStretch(1);
    footer->addWidget(diagnosisLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addLayout(header);
    layout->addLayout(library);
    layout->addWidget(sessionPage);
    layout->addWidget(tabs_, 1);

    logEdit_ = new QPlainTextEdit(this);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumBlockCount(2000);
    logEdit_->setMaximumHeight(150);
    layout->addWidget(logEdit_);
    layout->addLayout(footer);

    QTimer::singleShot(0, this, [this] {
        refreshDevices();
        refreshLoopbackSources();
        if (!jam2_.isRunning()) {
            showLocalPerformSetup();
        }
    });
}

QWidget* MainWindow::buildSessionPage()
{
    auto* page = new QWidget(this);
    bindHostEdit_ = new QLineEdit(SessionController::defaultBindHost(), page);
    portSpin_ = new QSpinBox(page);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(49000);
    publicHostEdit_ = new QLineEdit(SessionController::defaultPublicHost(), page);
    connectUrlEdit_ = new QLineEdit(page);
    stunServerEdit_ = new QLineEdit(QStringLiteral("stun.l.google.com:19302"), page);
    stunTimeoutSpin_ = new QSpinBox(page);
    stunTimeoutSpin_->setRange(1, 60000);
    stunTimeoutSpin_->setValue(1000);
    stunRetriesSpin_ = new QSpinBox(page);
    stunRetriesSpin_->setRange(1, 100);
    stunRetriesSpin_->setValue(3);
    waitMsSpin_ = new QSpinBox(page);
    waitMsSpin_->setRange(0, 24 * 60 * 60 * 1000);
    waitMsSpin_->setValue(0);
    streamMsSpin_ = new QSpinBox(page);
    streamMsSpin_->setRange(0, 24 * 60 * 60 * 1000);
    streamMsSpin_->setValue(0);
    streamLingerMsSpin_ = new QSpinBox(page);
    streamLingerMsSpin_->setRange(0, 60000);
    streamLingerMsSpin_->setValue(100);
    statsCheck_ = new QCheckBox(QStringLiteral("Periodic stats"), page);
    statsCheck_->setChecked(true);
    meshMaxPeersSpin_ = new QSpinBox(page);
    meshMaxPeersSpin_->setRange(0, 64);
    meshMaxPeersSpin_->setValue(0);
    statsWarmupMsSpin_ = new QSpinBox(page);
    statsWarmupMsSpin_->setRange(0, 600000);
    statsWarmupMsSpin_->setValue(3000);
    logStatsEdit_ = new QLineEdit(page);
    logStatsEdit_->setText(appReleaseFolderPath(QStringLiteral("logs")));
    socketSendBufferSpin_ = new QSpinBox(page);
    socketSendBufferSpin_->setRange(0, std::numeric_limits<int>::max());
    socketSendBufferSpin_->setValue(0);
    socketRecvBufferSpin_ = new QSpinBox(page);
    socketRecvBufferSpin_->setRange(0, std::numeric_limits<int>::max());
    socketRecvBufferSpin_->setValue(0);

    profileBox_ = new QComboBox(page);
    for (const jam2::TuningProfile& profile : jam2::tuning_profiles()) {
        profileBox_->addItem(QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
                             QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
    }
    osPriorityBox_ = new QComboBox(page);
    osPriorityBox_->addItem(QStringLiteral("Realtime"), QStringLiteral("realtime"));
    osPriorityBox_->addItem(QStringLiteral("High"), QStringLiteral("high"));
    osPriorityBox_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
    deviceBox_ = new QComboBox(page);
    deviceBox_->setEditable(true);
    inputChannelsEdit_ = new QLineEdit(QStringLiteral("1"), page);
    outputChannelsEdit_ = new QLineEdit(QStringLiteral("1,2"), page);
    sampleRateSpin_ = new QSpinBox(page);
    sampleRateSpin_->setRange(8000, 384000);
    sampleRateSpin_->setValue(44100);
    bufferSizeSpin_ = new QSpinBox(page);
    bufferSizeSpin_->setRange(16, 4096);
    bufferSizeSpin_->setValue(128);
    frameSizeSpin_ = new QSpinBox(page);
    frameSizeSpin_->setRange(32, 256);
    frameSizeSpin_->setValue(128);
    prefillSpin_ = new QSpinBox(page);
    prefillSpin_->setRange(0, 65536);
    prefillSpin_->setValue(1536);
    playbackMaxSpin_ = new QSpinBox(page);
    playbackMaxSpin_->setRange(0, 65536);
    playbackMaxSpin_->setValue(0);
    captureRingSpin_ = new QSpinBox(page);
    captureRingSpin_->setRange(1, 1048576);
    captureRingSpin_->setValue(4096);
    playbackRingSpin_ = new QSpinBox(page);
    playbackRingSpin_->setRange(1, 1048576);
    playbackRingSpin_->setValue(4096);
    driftCorrectionCheck_ = new QCheckBox(QStringLiteral("Drift correction"), page);
    driftCorrectionCheck_->setChecked(true);
    driftSmoothingSpin_ = new QDoubleSpinBox(page);
    driftSmoothingSpin_->setRange(0.0, 1.0);
    driftSmoothingSpin_->setDecimals(3);
    driftSmoothingSpin_->setSingleStep(0.005);
    driftSmoothingSpin_->setValue(0.02);
    driftDeadbandSpin_ = new QSpinBox(page);
    driftDeadbandSpin_->setRange(0, 50000);
    driftDeadbandSpin_->setValue(25);
    driftMaxCorrectionSpin_ = new QSpinBox(page);
    driftMaxCorrectionSpin_->setRange(0, 50000);
    driftMaxCorrectionSpin_->setValue(500);
    noStunCheck_ = new QCheckBox(QStringLiteral("No STUN"), page);
    bpmSpin_ = new QSpinBox(page);
    bpmSpin_->setRange(1, 400);
    bpmSpin_->setValue(120);
    bpmSpin_->hide();
    sampleTimePlayoutCheck_ = new QCheckBox(QStringLiteral("Sample-time playout"), page);
    sampleTimePlayoutCheck_->setChecked(true);
    playoutDelaySpin_ = new QSpinBox(page);
    playoutDelaySpin_->setRange(0, 1048576);
    playoutDelaySpin_->setValue(0);
    jitterBufferSpin_ = new QSpinBox(page);
    jitterBufferSpin_->setRange(0, 1048576);
    jitterBufferSpin_->setValue(0);
    jitterBufferMaxSpin_ = new QSpinBox(page);
    jitterBufferMaxSpin_->setRange(0, 1048576);
    jitterBufferMaxSpin_->setValue(0);
    adaptiveCushionCheck_ = new QCheckBox(QStringLiteral("Adaptive cushion"), page);
    adaptiveTargetSpin_ = new QSpinBox(page);
    adaptiveTargetSpin_->setRange(0, 1048576);
    adaptiveMinSpin_ = new QSpinBox(page);
    adaptiveMinSpin_->setRange(0, 1048576);
    adaptiveMaxSpin_ = new QSpinBox(page);
    adaptiveMaxSpin_->setRange(0, 1048576);
    adaptiveReleaseSpin_ = new QSpinBox(page);
    adaptiveReleaseSpin_->setRange(0, 1000000);
    adaptiveReleaseSpin_->setValue(1000);
    extraArgsEdit_ = new QLineEdit(page);
    startButton_ = new QPushButton(QStringLiteral("Start Jam"), page);
    joinButton_ = new QPushButton(QStringLiteral("Join Jam"), page);
    stopButton_ = new QPushButton(QStringLiteral("End Jam"), page);
    refreshControlButton_ = new QPushButton(QStringLiteral("Refresh Control"), page);
    stopButton_->setEnabled(false);
    refreshControlButton_->setEnabled(false);

    connectUrlEdit_->setMinimumWidth(420);
    deviceBox_->setEditable(false);
    deviceBox_->setMinimumWidth(280);
    const QList<QWidget*> sessionEditors{
        bindHostEdit_, portSpin_, publicHostEdit_, connectUrlEdit_,
        stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsWarmupMsSpin_, logStatsEdit_,
        meshMaxPeersSpin_,
        socketSendBufferSpin_, socketRecvBufferSpin_, profileBox_, osPriorityBox_, deviceBox_, inputChannelsEdit_,
        outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_, prefillSpin_,
        playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, driftSmoothingSpin_,
        driftDeadbandSpin_, driftMaxCorrectionSpin_, playoutDelaySpin_, jitterBufferSpin_,
        jitterBufferMaxSpin_, adaptiveTargetSpin_, adaptiveMinSpin_, adaptiveMaxSpin_,
        adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : sessionEditors) {
        applyMutedEditorStyle(widget);
    }
    const QList<QWidget*> sessionDialogWidgets{
        bindHostEdit_, portSpin_, publicHostEdit_, connectUrlEdit_,
        stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, statsWarmupMsSpin_, logStatsEdit_,
        meshMaxPeersSpin_,
        socketSendBufferSpin_, socketRecvBufferSpin_, profileBox_, osPriorityBox_, deviceBox_, inputChannelsEdit_,
        outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_, prefillSpin_,
        playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, driftCorrectionCheck_,
        driftSmoothingSpin_, driftDeadbandSpin_, driftMaxCorrectionSpin_, noStunCheck_,
        sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_, jitterBufferMaxSpin_,
        adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_, adaptiveMaxSpin_,
        adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : sessionDialogWidgets) {
        widget->hide();
    }

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(startButton_);
    buttons->addWidget(joinButton_);
    buttons->addWidget(stopButton_);
    buttons->addWidget(refreshControlButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(buttons);

    QObject::connect(startButton_, &QPushButton::clicked, this, [this] { showStartJamDialog(); });
    QObject::connect(joinButton_, &QPushButton::clicked, this, [this] { showJoinJamDialog(); });
    QObject::connect(stopButton_, &QPushButton::clicked, this, [this] { stopJam(); });
    QObject::connect(refreshControlButton_, &QPushButton::clicked, this, [this] { refreshControlConnection(); });
    QObject::connect(profileBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        applyTuningProfileName(profileBox_->currentData().toString());
    });
    applyTuningProfileName(QStringLiteral("fast"));
    QObject::connect(noStunCheck_, &QCheckBox::toggled, this, [this] {
        updateConnectionControlState();
    });
    updateConnectionControlState();

    return page;
}

QWidget* MainWindow::buildSongPage()
{
    auto* page = new QWidget(this);
    chordGrid_ = new BeatGridWidget(&chordModel_, QStringLiteral("chord"), page);

    auto* top = new QHBoxLayout();
    top->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(chordGrid_, 1);

    chordGrid_->onCellEdited = [this](int section, const QString& lane, int beat, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    chordGrid_->onGridResized = [this](int section, int beats, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("chord")},
            {QStringLiteral("beats"), beats},
        });
    };
    chordGrid_->onStructureChanged = [this] {
        sendSongSnapshot();
    };

    return page;
}

QWidget* MainWindow::buildTrackPage()
{
    auto* page = new QWidget(this);
    trackNameLabel_ = new QLabel(QStringLiteral("Track: No track loaded"), page);
    gridPositionLabel_ = new QLabel(QStringLiteral("Grid: stopped"), page);
    gridScheduleLabel_ = new QLabel(QStringLiteral("Grid lock: idle"), page);
    recordingCountdownLabel_ = new QLabel(page);
    recordingCountdownLabel_->setAlignment(Qt::AlignCenter);
    recordingCountdownLabel_->setMinimumHeight(72);
    recordingCountdownLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background: #171b1e; border: 1px solid #66c6a6; color: #f2f5f4; font-size: 34px; font-weight: 700; }"));
    recordingCountdownLabel_->hide();

    trackWaveform_ = nullptr;
    looperStack_ = new LooperLaneStackWidget(page);
    trackSpeedSlider_ = new QSlider(Qt::Horizontal, page);
    trackSpeedSlider_->setRange(10, 200);
    trackSpeedSlider_->setValue(100);
    applyJamSliderStyle(trackSpeedSlider_);
    trackSpeedSlider_->setMinimumWidth(220);
    trackSpeedSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trackSpeedSpin_ = new QDoubleSpinBox(page);
    trackSpeedSpin_->setRange(0.10, 2.00);
    trackSpeedSpin_->setSingleStep(0.01);
    trackSpeedSpin_->setDecimals(2);
    trackSpeedSpin_->setValue(1.0);
    trackSpeedSpin_->setFixedWidth(92);
    applyMutedEditorStyle(trackSpeedSpin_);
    trackPitchSlider_ = new QSlider(Qt::Horizontal, page);
    trackPitchSlider_->setRange(-12, 12);
    trackPitchSlider_->setValue(0);
    applyJamSliderStyle(trackPitchSlider_);
    trackPitchSlider_->setMinimumWidth(220);
    trackPitchSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trackPitchSpin_ = new QSpinBox(page);
    trackPitchSpin_->setRange(-12, 12);
    trackPitchSpin_->setSingleStep(1);
    trackPitchSpin_->setSuffix(QStringLiteral(" semitones"));
    trackPitchSpin_->setFixedWidth(128);
    applyMutedEditorStyle(trackPitchSpin_);
    focusFrequencySlider_ = new QSlider(Qt::Horizontal, page);
    focusFrequencySlider_->setRange(40, 8000);
    focusFrequencySlider_->setValue(120);
    applyJamSliderStyle(focusFrequencySlider_);
    focusFrequencySlider_->setMinimumWidth(220);
    focusFrequencySlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    focusFrequencySpin_ = new QSpinBox(page);
    focusFrequencySpin_->setRange(40, 8000);
    focusFrequencySpin_->setValue(120);
    focusFrequencySpin_->setSuffix(QStringLiteral(" Hz"));
    focusFrequencySpin_->setFixedWidth(108);
    applyMutedEditorStyle(focusFrequencySpin_);
    trackSyncCheck_ = new QCheckBox(QStringLiteral("Sync track controls"), page);
    trackSyncCheck_->setChecked(looperProject_.trackSyncEnabled());
    auto* gridLockBox = new QCheckBox(QStringLiteral("Lock to grid"), page);
    gridLockBox->setChecked(looperProject_.gridLockEnabled());
    captureOutputEdit_ = new QLineEdit(page);
    captureOutputEdit_->setMinimumWidth(420);
    applyMutedEditorStyle(captureOutputEdit_);
    loopbackSourceBox_ = new QComboBox(page);
    loopbackSourceBox_->setEditable(true);
    loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
    loopbackSourceBox_->setMinimumWidth(360);
    applyMutedEditorStyle(loopbackSourceBox_);
    captureManualStopCheck_ = new QCheckBox(QStringLiteral("Record until stopped"), page);
    captureManualStopCheck_->setChecked(true);
    captureCountInCheck_ = new QCheckBox(QStringLiteral("Count-in"), page);
    captureCountInCheck_->setChecked(true);
    captureCountInMetronomeCheck_ = new QCheckBox(QStringLiteral("Metronome during count-in"), page);
    captureCountInMetronomeCheck_->setChecked(true);
    captureKeepMetronomeCheck_ = new QCheckBox(QStringLiteral("Keep metronome on while recording"), page);
    captureKeepMetronomeCheck_->setChecked(false);
    captureCountInBarsSpin_ = new QSpinBox(page);
    captureCountInBarsSpin_->setRange(1, 8);
    captureCountInBarsSpin_->setValue(1);
    captureCountInBarsSpin_->setSuffix(QStringLiteral(" bars"));
    captureCountInBarsSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(captureCountInBarsSpin_);
    recordingLatencyLabel_ = new QLabel(QStringLiteral("Waiting for engine latency data"), page);
    recordingLatencyLabel_->setWordWrap(true);
    recordingLatencyAdjustmentSpin_ = new QSpinBox(page);
    recordingLatencyAdjustmentSpin_->setRange(-8192, 8192);
    recordingLatencyAdjustmentSpin_->setValue(0);
    recordingLatencyAdjustmentSpin_->setSuffix(QStringLiteral(" frames"));
    recordingLatencyAdjustmentSpin_->setMinimumWidth(132);
    applyMutedEditorStyle(recordingLatencyAdjustmentSpin_);
    captureDurationSpin_ = new QSpinBox(page);
    captureDurationSpin_->setRange(1, 600);
    captureDurationSpin_->setValue(30);
    captureDurationSpin_->setSuffix(QStringLiteral(" s"));
    captureDurationSpin_->setEnabled(false);
    captureDurationSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(captureDurationSpin_);
    captureTriggerCheck_ = new QCheckBox(QStringLiteral("Trigger on signal"), page);
    trimLeadingCheck_ = new QCheckBox(QStringLiteral("Trim leading silence"), page);
    trimTrailingCheck_ = new QCheckBox(QStringLiteral("Trim trailing silence"), page);
    trimLeadingCheck_->setChecked(true);
    trimTrailingCheck_->setChecked(true);
    triggerThresholdSpin_ = new QDoubleSpinBox(page);
    triggerThresholdSpin_->setRange(-120.0, 0.0);
    triggerThresholdSpin_->setDecimals(1);
    triggerThresholdSpin_->setValue(-45.0);
    triggerThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    triggerThresholdSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(triggerThresholdSpin_);
    tailThresholdSpin_ = new QDoubleSpinBox(page);
    tailThresholdSpin_->setRange(-120.0, 0.0);
    tailThresholdSpin_->setDecimals(1);
    tailThresholdSpin_->setValue(-50.0);
    tailThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    tailThresholdSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(tailThresholdSpin_);
    preRollSpin_ = new QSpinBox(page);
    preRollSpin_->setRange(0, 10000);
    preRollSpin_->setValue(250);
    preRollSpin_->setSuffix(QStringLiteral(" ms"));
    preRollSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(preRollSpin_);
    triggerHoldSpin_ = new QSpinBox(page);
    triggerHoldSpin_->setRange(1, 5000);
    triggerHoldSpin_->setValue(50);
    triggerHoldSpin_->setSuffix(QStringLiteral(" ms"));
    triggerHoldSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(triggerHoldSpin_);
    tailSilenceSpin_ = new QSpinBox(page);
    tailSilenceSpin_->setRange(0, 30000);
    tailSilenceSpin_->setValue(1000);
    tailSilenceSpin_->setSuffix(QStringLiteral(" ms"));
    tailSilenceSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(tailSilenceSpin_);
    playTrackButton_ = new QPushButton(QStringLiteral("Play Track"), page);
    stopTrackButton_ = new QPushButton(QStringLiteral("Stop Track"), page);
    loopStartButton_ = new QPushButton(QStringLiteral("Loop Start"), page);
    loopEndButton_ = new QPushButton(QStringLiteral("Loop End"), page);
    clearLoopButton_ = new QPushButton(QStringLiteral("Clear Loop"), page);
    loopEnabledCheck_ = new QCheckBox(QStringLiteral("Loop whole track"), page);
    loopEnabledCheck_->setChecked(trackController_.model().loopEnabled);
    trackController_.model().trackGainDb = 0.0;
    stopCaptureButton_ = new QPushButton(QStringLiteral("Stop Recording"), page);
    stopCaptureButton_->setEnabled(false);
    loadWavButton_ = new QPushButton(QStringLiteral("Load WAV"), page);
    startArmedLaneRecordingButton_ = new QPushButton(QStringLiteral("Start Recording"), page);

    captureOutputEdit_->setText(appReleaseFilePath(QStringLiteral("captures"), QStringLiteral("take.wav")));
    const QList<QWidget*> captureDialogWidgets{
        captureOutputEdit_, loopbackSourceBox_, captureDurationSpin_,
        captureManualStopCheck_, captureCountInCheck_, captureCountInMetronomeCheck_, captureKeepMetronomeCheck_, captureCountInBarsSpin_,
        recordingLatencyLabel_, recordingLatencyAdjustmentSpin_,
        captureTriggerCheck_, trimLeadingCheck_, trimTrailingCheck_, triggerThresholdSpin_,
        tailThresholdSpin_, preRollSpin_, triggerHoldSpin_, tailSilenceSpin_,
    };
    for (QWidget* widget : captureDialogWidgets) {
        widget->hide();
    }
    auto* speedControl = new QWidget(page);
    speedControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* speedLayout = new QHBoxLayout(speedControl);
    speedLayout->setContentsMargins(0, 0, 0, 0);
    speedLayout->addWidget(trackSpeedSlider_, 1);
    speedLayout->addWidget(trackSpeedSpin_);
    auto* pitchControl = new QWidget(page);
    pitchControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* pitchLayout = new QHBoxLayout(pitchControl);
    pitchLayout->setContentsMargins(0, 0, 0, 0);
    pitchLayout->addWidget(trackPitchSlider_, 1);
    pitchLayout->addWidget(trackPitchSpin_);
    auto* focusControl = new QWidget(page);
    focusControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* focusLayout = new QHBoxLayout(focusControl);
    focusLayout->setContentsMargins(0, 0, 0, 0);
    focusFrequencyCheck_ = new QCheckBox(page);
    focusPresetBox_ = new QComboBox(page);
    focusPresetBox_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    focusPresetBox_->addItem(QStringLiteral("Bass"), QStringLiteral("bass"));
    focusPresetBox_->addItem(QStringLiteral("Guitar"), QStringLiteral("guitar"));
    focusPresetBox_->addItem(QStringLiteral("Vocals"), QStringLiteral("vocals"));
    focusPresetBox_->addItem(QStringLiteral("Drums"), QStringLiteral("drums"));
    focusPresetBox_->setFixedWidth(108);
    applyMutedEditorStyle(focusPresetBox_);
    focusLayout->addWidget(focusFrequencyCheck_);
    focusLayout->addWidget(focusPresetBox_);
    focusLayout->addWidget(focusFrequencySlider_, 1);
    focusLayout->addWidget(focusFrequencySpin_);
    auto* loopOptionsControl = new QWidget(page);
    auto* loopOptionsLayout = new QGridLayout(loopOptionsControl);
    loopOptionsLayout->setContentsMargins(0, 0, 0, 0);
    loopOptionsLayout->setHorizontalSpacing(24);
    loopOptionsLayout->addWidget(gridLockBox, 0, 0);
    loopOptionsLayout->addWidget(trackSyncCheck_, 0, 1);
    loopOptionsLayout->addWidget(loopEnabledCheck_, 1, 0);
    loopOptionsLayout->setColumnStretch(0, 1);
    loopOptionsLayout->setColumnStretch(1, 1);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(trackNameLabel_);
    form->addRow(gridPositionLabel_);
    form->addRow(gridScheduleLabel_);
    form->addRow(QStringLiteral("Speed"), speedControl);
    form->addRow(QStringLiteral("Pitch"), pitchControl);
    form->addRow(QStringLiteral("Focus frequency"), focusControl);
    form->addRow(loopOptionsControl);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(playTrackButton_);
    buttons->addWidget(stopTrackButton_);
    buttons->addWidget(loopStartButton_);
    buttons->addWidget(loopEndButton_);
    buttons->addWidget(clearLoopButton_);
    buttons->addWidget(startArmedLaneRecordingButton_);
    buttons->addWidget(stopCaptureButton_);
    buttons->addWidget(loadWavButton_);
    for (int i = 0; i < 4; ++i) {
        auto* bankButton = new QPushButton(QString(QChar(QLatin1Char(static_cast<char>('A' + i)))), page);
        bankButton->setFixedWidth(34);
        bankButton->setCheckable(true);
        looperBankButtons_[i] = bankButton;
        buttons->addWidget(bankButton);
        QObject::connect(bankButton, &QPushButton::clicked, this, [this, i] {
            looperProject_.setActiveBankIndex(i);
            selectedLooperLane_ = -1;
            refreshLooperLanes();
            regeneratePreparedMix();
            syncLooperArrangement();
        });
    }
    buttons->addStretch(1);

    auto* laneScroll = new QScrollArea(page);
    laneScroll->setWidgetResizable(true);
    laneScroll->setFrameShape(QFrame::NoFrame);
    laneScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    laneScroll->setMinimumHeight(280);
    laneScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    laneScroll->setWidget(looperStack_);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(buttons);
    layout->addWidget(recordingCountdownLabel_);
    layout->addWidget(laneScroll, 1);
    layout->addLayout(form);

    QObject::connect(trackSpeedSlider_, &QSlider::valueChanged, this, [this](int value) {
        const double speed = static_cast<double>(value) / 100.0;
        trackController_.model().speed = speed;
        if (trackSpeedSpin_) {
            const QSignalBlocker blocker(trackSpeedSpin_);
            trackSpeedSpin_->setValue(speed);
        }
    });
    QObject::connect(trackSpeedSlider_, &QSlider::sliderReleased, this, [this] {
        regeneratePreparedMix();
    });
    QObject::connect(trackSpeedSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        trackController_.model().speed = value;
        if (trackSpeedSlider_) {
            const QSignalBlocker blocker(trackSpeedSlider_);
            trackSpeedSlider_->setValue(qBound(10, qRound(value * 100.0), 200));
        }
        regeneratePreparedMix();
    });
    QObject::connect(trackPitchSlider_, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().pitchCents = value * 100;
        if (trackPitchSpin_) {
            const QSignalBlocker blocker(trackPitchSpin_);
            trackPitchSpin_->setValue(value);
        }
    });
    QObject::connect(trackPitchSlider_, &QSlider::sliderReleased, this, [this] {
        regeneratePreparedMix();
    });
    QObject::connect(trackPitchSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        trackController_.model().pitchCents = value * 100;
        if (trackPitchSlider_) {
            const QSignalBlocker blocker(trackPitchSlider_);
            trackPitchSlider_->setValue(value);
        }
        regeneratePreparedMix();
    });
    if (trackWaveform_) {
        trackWaveform_->onSeekMs = [this](qint64 positionMs) {
            const qint64 frame = positionMs * qMax(1, preparedSourceSampleRate_) / 1000;
            runGridLockedEngineAction(QStringLiteral("track.seek"), [this, frame](std::uint64_t targetFrame) {
                sendJamCommand(QStringLiteral("track seek %1 %2").arg(frame).arg(targetFrame));
                preparedSourceFrame_ = frame;
                preparedSourceEngineFrame_ = static_cast<qint64>(playbackGrid_.position().rawCurrentFrame);
                updateTrackTimeline();
            });
        };
    }
    QObject::connect(playTrackButton_, &QPushButton::clicked, this, [this] {
        playTrack();
    });
    QObject::connect(stopTrackButton_, &QPushButton::clicked, this, [this] {
        runGridLockedEngineAction(QStringLiteral("track.stop"), [this](std::uint64_t targetFrame) { stopTrack(targetFrame); });
    });
    QObject::connect(loopStartButton_, &QPushButton::clicked, this, [this] {
        setLoopStartAtCurrentPosition();
    });
    QObject::connect(loopEndButton_, &QPushButton::clicked, this, [this] {
        setLoopEndAtCurrentPosition();
    });
    QObject::connect(clearLoopButton_, &QPushButton::clicked, this, [this] { clearTrackLoop(); });
    QObject::connect(loopEnabledCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().loopEnabled = checked;
        updateTrackControls();
        loadPreparedMixIntoEngine();
    });
    QObject::connect(focusFrequencyCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().focusEnabled = checked;
        regeneratePreparedMix();
    });
    QObject::connect(focusPresetBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        QString key = focusPresetBox_->currentData().toString();
        if (key.isEmpty()) {
            key = QStringLiteral("custom");
        }
        auto& model = trackController_.model();
        model.focusPreset = key;
        if (!isCustomFocusPreset(key)) {
            const FocusPreset preset = focusPresetForKey(key);
            model.focusEnabled = true;
            model.focusFrequencyHz = preset.frequencyHz;
            model.focusGainDb = preset.gainDb;
            model.focusQ = preset.q;
        }
        updateTrackControls();
        regeneratePreparedMix();
    });
    QObject::connect(focusFrequencySlider_, &QSlider::valueChanged, this, [this](int value) {
        auto& model = trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (focusPresetBox_) {
            const QSignalBlocker blocker(focusPresetBox_);
            focusPresetBox_->setCurrentIndex(qMax(0, focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (focusFrequencySlider_) {
            focusFrequencySlider_->setEnabled(true);
        }
        if (focusFrequencySpin_) {
            focusFrequencySpin_->setEnabled(true);
        }
        if (focusFrequencySpin_) {
            const QSignalBlocker blocker(focusFrequencySpin_);
            focusFrequencySpin_->setValue(value);
        }
    });
    QObject::connect(focusFrequencySlider_, &QSlider::sliderReleased, this, [this] {
        regeneratePreparedMix();
    });
    QObject::connect(focusFrequencySpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        auto& model = trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (focusPresetBox_) {
            const QSignalBlocker blocker(focusPresetBox_);
            focusPresetBox_->setCurrentIndex(qMax(0, focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (focusFrequencySlider_) {
            focusFrequencySlider_->setEnabled(true);
        }
        if (focusFrequencySpin_) {
            focusFrequencySpin_->setEnabled(true);
        }
        if (focusFrequencySlider_) {
            const QSignalBlocker blocker(focusFrequencySlider_);
            focusFrequencySlider_->setValue(value);
        }
        regeneratePreparedMix();
    });
    QObject::connect(trackSyncCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        looperProject_.setTrackSyncEnabled(checked);
        trackController_.model().syncControls = checked;
    });
    QObject::connect(gridLockBox, &QCheckBox::toggled, this, [this](bool checked) {
        looperProject_.setGridLockEnabled(checked);
        refreshLooperLanes();
    });
    QObject::connect(recordingLatencyAdjustmentSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int frames) {
        updateRecordingLatencyDisplay();
        if (jam2_.isRunning()) {
            sendJamCommand(QStringLiteral("record latency_adjust %1").arg(frames));
        }
    });
    QObject::connect(captureManualStopCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        (void)checked;
        updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_);
    });
    QObject::connect(stopCaptureButton_, &QPushButton::clicked, this, [this] {
        if (loopbackRecorder_.isRunning()) {
            loopbackRecorder_.stop();
        } else {
            runGridLockedEngineAction(QStringLiteral("record.stop"), [this](std::uint64_t targetFrame) { stopInputCapture(targetFrame); });
        }
    });
    QObject::connect(loadWavButton_, &QPushButton::clicked, this, [this] { loadWavIntoLooperLane(); });
    QObject::connect(startArmedLaneRecordingButton_, &QPushButton::clicked, this, [this] { startArmedLooperLaneRecording(); });
    looperStack_->onSelected = [this](int lane) { selectedLooperLane_ = lane; };
    looperStack_->onAddLane = [this] { addEmptyLooperLane(); };
    looperStack_->onAddWav = [this] { addLooperWavs(); };
    looperStack_->onMute = [this](int lane) { selectedLooperLane_ = lane; toggleSelectedLooperLaneMute(); };
    looperStack_->onSolo = [this](int lane) { selectedLooperLane_ = lane; toggleSelectedLooperLaneSolo(); };
    looperStack_->onArm = [this](int lane) {
        selectedLooperLane_ = lane;
        if (armedRecordBank_ == looperProject_.activeBankIndex() && armedRecordLane_ == lane) {
            armedRecordBank_ = -1;
            armedRecordLane_ = -1;
            armedRecordMode_.clear();
            refreshLooperLanes();
            appendLog(QStringLiteral("disarmed lane recording"));
            return;
        }
        (void)armSelectedLooperLaneRecording();
    };
    looperStack_->onRename = [this](int lane) { selectedLooperLane_ = lane; renameSelectedLooperLane(); };
    looperStack_->onRemove = [this](int lane) { selectedLooperLane_ = lane; removeSelectedLooperLane(); };
    looperStack_->onGainChanged = [this](int lane, double gainDb) { applyLooperLaneGain(lane, gainDb); };
    looperStack_->onRegionCommitted = [this](int lane, qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame) {
        selectedLooperLane_ = lane;
        applySelectedLooperLaneRegion(startFrame, sourceStartFrame, sourceEndFrame);
    };
    looperStack_->onSeekFrame = [this](qint64 frame) {
        runGridLockedEngineAction(QStringLiteral("track.seek"), [this, frame](std::uint64_t targetFrame) {
            sendJamCommand(QStringLiteral("track seek %1 %2").arg(frame).arg(targetFrame));
            preparedSourceFrame_ = frame;
            preparedSourceEngineFrame_ = static_cast<qint64>(playbackGrid_.position().rawCurrentFrame);
            updateTrackTimeline();
        });
    };
    looperStack_->onBankSelected = [this](int index) {
        looperProject_.setActiveBankIndex(index);
        selectedLooperLane_ = -1;
        refreshLooperLanes();
        regeneratePreparedMix();
        syncLooperArrangement();
    };
    refreshLooperLanes();
    regeneratePreparedMix();
    syncLooperArrangement();

    return page;
}

QWidget* MainWindow::buildMetronomePage()
{
    auto* page = new QWidget(this);

    metronomeBpmSpin_ = new QSpinBox(page);
    metronomeBpmSpin_->setRange(1, 400);
    metronomeBpmSpin_->setValue(qBound(1, static_cast<int>(std::lround(trackController_.model().acceptedBpm)), 400));
    applyMutedEditorStyle(metronomeBpmSpin_);

    metronomeBeatsSpin_ = new QSpinBox(page);
    metronomeBeatsSpin_->setRange(1, 16);
    metronomeBeatsSpin_->setValue(4);
    applyMutedEditorStyle(metronomeBeatsSpin_);

    metronomeDivisionBox_ = new QComboBox(page);
    metronomeDivisionBox_->addItem(QStringLiteral("Quarter"), 1);
    metronomeDivisionBox_->addItem(QStringLiteral("Eighth"), 2);
    metronomeDivisionBox_->addItem(QStringLiteral("Triplet"), 3);
    metronomeDivisionBox_->addItem(QStringLiteral("16th"), 4);
    metronomeDivisionBox_->addItem(QStringLiteral("6th"), 6);
    metronomeDivisionBox_->addItem(QStringLiteral("32nd"), 8);
    metronomeDivisionBox_->setCurrentIndex(metronomeDivisionBox_->findData(1));
    metronomeDivisionBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(metronomeDivisionBox_);

    metronomeModeBox_ = new QComboBox(page);
    metronomeModeBox_->addItems({
        QStringLiteral("shared-grid"),
        QStringLiteral("leader-audio"),
        QStringLiteral("listener-compensated"),
    });
    metronomeModeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(metronomeModeBox_);

    metronomeCompensationButton_ = new QPushButton(QStringLiteral("Advanced"), page);
    metronomeCompensationButton_->setVisible(false);

    metronomeCompensationMaxSpin_ = new QDoubleSpinBox(page);
    metronomeCompensationMaxSpin_->setRange(0.0, 1000.0);
    metronomeCompensationMaxSpin_->setDecimals(1);
    metronomeCompensationMaxSpin_->setSuffix(QStringLiteral(" ms"));
    metronomeCompensationMaxSpin_->setValue(250.0);
    metronomeCompensationSmoothingSpin_ = new QDoubleSpinBox(page);
    metronomeCompensationSmoothingSpin_->setRange(0.0, 10000.0);
    metronomeCompensationSmoothingSpin_->setDecimals(1);
    metronomeCompensationSmoothingSpin_->setSuffix(QStringLiteral(" ms"));
    metronomeCompensationSmoothingSpin_->setValue(750.0);
    metronomeCompensationDeadbandSpin_ = new QDoubleSpinBox(page);
    metronomeCompensationDeadbandSpin_->setRange(0.0, 1000.0);
    metronomeCompensationDeadbandSpin_->setDecimals(1);
    metronomeCompensationDeadbandSpin_->setSuffix(QStringLiteral(" ms"));
    metronomeCompensationDeadbandSpin_->setValue(1.0);
    metronomeCompensationSlewSpin_ = new QDoubleSpinBox(page);
    metronomeCompensationSlewSpin_->setRange(0.0, 10000.0);
    metronomeCompensationSlewSpin_->setDecimals(1);
    metronomeCompensationSlewSpin_->setSuffix(QStringLiteral(" ms/s"));
    metronomeCompensationSlewSpin_->setValue(40.0);
    applyMutedEditorStyle(metronomeCompensationMaxSpin_);
    applyMutedEditorStyle(metronomeCompensationSmoothingSpin_);
    applyMutedEditorStyle(metronomeCompensationDeadbandSpin_);
    applyMutedEditorStyle(metronomeCompensationSlewSpin_);
    metronomeCompensationMaxSpin_->hide();
    metronomeCompensationSmoothingSpin_->hide();
    metronomeCompensationDeadbandSpin_->hide();
    metronomeCompensationSlewSpin_->hide();

    trackMetronomeLabel_ = new QLabel(QStringLiteral("Local metronome stopped"), page);
    startTrackMetronomeButton_ = new QPushButton(QStringLiteral("Start"), page);
    stopTrackMetronomeButton_ = new QPushButton(QStringLiteral("Stop"), page);
    stopTrackMetronomeButton_->setEnabled(false);
    metronomeMarkerReferenceCheck_ = new QCheckBox(QStringLiteral("Show marker reference"), page);
    metronomeMarkerReferenceCheck_->setChecked(true);

    metronomePatternTable_ = new QTableWidget(page);
    metronomePatternTable_->setRowCount(2);
    metronomePatternTable_->verticalHeader()->setVisible(true);
    metronomePatternTable_->setVerticalHeaderLabels(QStringList{QStringLiteral("Play"), QStringLiteral("Accent")});
    metronomePatternTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    metronomePatternTable_->setSelectionMode(QAbstractItemView::NoSelection);
    metronomePatternTable_->setMinimumHeight(110);
    metronomePatternTable_->setStyleSheet(QStringLiteral(
        "QTableWidget::item { padding: 3px 6px; }"
        "QCheckBox::indicator { width: 15px; height: 15px; border: 1px solid #52616c; background: #1f2428; }"
        "QCheckBox::indicator:checked { border: 1px solid #66c6a6; background: #66c6a6; }"));

    auto makeControlPair = [page](const QString& label, QWidget* editor) {
        auto* pair = new QWidget(page);
        auto* pairLayout = new QHBoxLayout(pair);
        pairLayout->setContentsMargins(0, 0, 0, 0);
        pairLayout->setSpacing(6);
        pairLayout->addWidget(new QLabel(label, pair));
        pairLayout->addWidget(editor, 0, Qt::AlignLeft);
        pairLayout->addStretch(1);
        return pair;
    };
    auto* controls = new QGridLayout();
    controls->setHorizontalSpacing(24);
    controls->setVerticalSpacing(10);
    controls->addWidget(makeControlPair(QStringLiteral("BPM"), metronomeBpmSpin_), 0, 0);
    controls->addWidget(makeControlPair(QStringLiteral("Beats"), metronomeBeatsSpin_), 0, 1);
    controls->addWidget(makeControlPair(QStringLiteral("Division"), metronomeDivisionBox_), 0, 2);
    auto* modeRow = new QWidget(page);
    auto* modeLayout = new QHBoxLayout(modeRow);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(6);
    modeLayout->addWidget(new QLabel(QStringLiteral("Mode"), modeRow));
    modeLayout->addWidget(metronomeModeBox_, 0, Qt::AlignLeft);
    modeLayout->addWidget(metronomeCompensationButton_, 0, Qt::AlignLeft);
    modeLayout->addStretch(1);
    controls->addWidget(modeRow, 0, 3);
    controls->addWidget(new QLabel(QStringLiteral("Pattern"), page), 1, 0);
    controls->addWidget(trackMetronomeLabel_, 1, 1);
    controls->addWidget(metronomeMarkerReferenceCheck_, 1, 2, 1, 2);
    controls->setColumnStretch(1, 1);
    controls->setColumnStretch(3, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(startTrackMetronomeButton_);
    buttons->addWidget(stopTrackMetronomeButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(controls);
    layout->addLayout(buttons);
    layout->addWidget(metronomePatternTable_, 1);

    QObject::connect(startTrackMetronomeButton_, &QPushButton::clicked, this, [this] { startTrackMetronome(); });
    QObject::connect(stopTrackMetronomeButton_, &QPushButton::clicked, this, [this] { stopTrackMetronome(); });
    QObject::connect(metronomeMarkerReferenceCheck_, &QCheckBox::toggled, this, [this] {
        updatePlaybackGrid();
    });
    QObject::connect(metronomeBpmSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        updateTrackMetronomeInterval();
    });
    QObject::connect(metronomeModeBox_, &QComboBox::currentTextChanged, this, [this] {
        updateMetronomeCompensationVisibility();
        sendMetronomeModeToJam();
        sendMetronomeSettingsToPeer();
    });
    QObject::connect(metronomeCompensationButton_, &QPushButton::clicked, this, [this] {
        showMetronomeCompensationDialog();
    });
    QObject::connect(metronomeBeatsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        rebuildMetronomePattern();
        updateTrackMetronomeInterval();
    });
    QObject::connect(metronomeDivisionBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        rebuildMetronomePattern(true);
        updateTrackMetronomeInterval();
    });

    rebuildMetronomePattern();
    updateMetronomeCompensationVisibility();
    return page;
}

QWidget* MainWindow::buildMixPage()
{
    auto* page = new QWidget(this);
    auto* content = new QWidget(page);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto makeValueLabel = [page](const QString& text) {
        auto* label = new QLabel(text, page);
        label->setFrameShape(QFrame::StyledPanel);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(82);
        return label;
    };
    auto makeRow = [page](const QString& name, QSlider* slider, QLabel* valueLabel) {
        auto* row = new QWidget(page);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setMinimumWidth(120);
        rowLayout->addWidget(nameLabel);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(valueLabel);
        return row;
    };
    auto makeMeterRow = [page](const QString& name, LevelMeterWidget* meter, QLabel* valueLabel = nullptr) {
        auto* row = new QWidget(page);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setMinimumWidth(120);
        rowLayout->addWidget(nameLabel);
        rowLayout->addWidget(meter, 1);
        if (valueLabel != nullptr) {
            rowLayout->addWidget(valueLabel);
        }
        return row;
    };
    auto makeSectionLabel = [content](const QString& text) {
        auto* label = new QLabel(text, content);
        label->setStyleSheet(QStringLiteral("font-weight: 600; margin-top: 8px;"));
        return label;
    };
    mixJamRecordingRow_ = new QWidget(content);
    auto* recordingLayout = new QHBoxLayout(mixJamRecordingRow_);
    recordingLayout->setContentsMargins(0, 0, 0, 0);
    recordingLayout->setSpacing(10);
    auto* recordingName = new QLabel(QStringLiteral("Jam recording"), mixJamRecordingRow_);
    recordingName->setMinimumWidth(120);
    jamRecordingButton_ = new QPushButton(QStringLiteral("Start Recording Jam"), mixJamRecordingRow_);
    jamRecordingLabel_ = new QLabel(QStringLiteral("Stopped"), mixJamRecordingRow_);
    jamRecordingLabel_->setFrameShape(QFrame::StyledPanel);
    jamRecordingLabel_->setMinimumWidth(180);
    recordingLayout->addWidget(recordingName);
    recordingLayout->addWidget(jamRecordingButton_);
    recordingLayout->addWidget(jamRecordingLabel_, 1);
    layout->addWidget(mixJamRecordingRow_);

    mixInputMeter_ = new LevelMeterWidget(page);
    mixSendMeter_ = new LevelMeterWidget(page);
    mixMonitorMeter_ = new LevelMeterWidget(page);
    mixTrackMeter_ = new LevelMeterWidget(page);
    mixMetronomeMeter_ = new LevelMeterWidget(page);
    mixOutputMeter_ = new LevelMeterWidget(page);
    mixRemotePeerMeter_ = new LevelMeterWidget(page);
    mixOutputClipLabel_ = makeValueLabel(QStringLiteral("clip 0"));

    mixSendLevelSlider_ = new QSlider(Qt::Horizontal, page);
    mixSendLevelSlider_->setRange(-60, 12);
    mixSendLevelSlider_->setValue(0);
    applyJamSliderStyle(mixSendLevelSlider_);
    mixSendLevelSlider_->setMinimumWidth(220);
    mixSendLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mixSendLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    mixMonitorCheck_ = new QCheckBox(QStringLiteral("Monitor input"), page);
    mixMonitorCheck_->setChecked(false);
    mixMonitorLevelSlider_ = new QSlider(Qt::Horizontal, page);
    mixMonitorLevelSlider_->setRange(-60, 12);
    mixMonitorLevelSlider_->setValue(-18);
    applyJamSliderStyle(mixMonitorLevelSlider_);
    mixMonitorLevelSlider_->setMinimumWidth(220);
    mixMonitorLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mixMonitorLevelLabel_ = makeValueLabel(QStringLiteral("-18.0 dB"));

    trackLevelSlider_ = new QSlider(Qt::Horizontal, page);
    trackLevelSlider_->setRange(-60, 12);
    trackLevelSlider_->setValue(0);
    applyJamSliderStyle(trackLevelSlider_);
    trackLevelSlider_->setMinimumWidth(220);
    trackLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trackLevelDbLabel_ = makeValueLabel(dbText(trackController_.model().trackGainDb));

    metronomeLevelSlider_ = new QSlider(Qt::Horizontal, page);
    metronomeLevelSlider_->setRange(-60, 12);
    metronomeLevelSlider_->setValue(-10);
    applyJamSliderStyle(metronomeLevelSlider_);
    metronomeLevelSlider_->setMinimumWidth(220);
    metronomeLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    localMetronomeLevelSlider_ = metronomeLevelSlider_;
    mixMetronomeLevelSlider_ = metronomeLevelSlider_;
    mixMetronomeLevelLabel_ = makeValueLabel(QStringLiteral("-10.0 dB"));

    remoteLevelSlider_ = new QSlider(Qt::Horizontal, page);
    remoteLevelSlider_->setRange(-60, 12);
    remoteLevelSlider_->setValue(0);
    applyJamSliderStyle(remoteLevelSlider_);
    remoteLevelSlider_->setMinimumWidth(220);
    remoteLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mixRemotePeerSlider_ = remoteLevelSlider_;
    mixRemotePeerLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    mixTrackLevelSlider_ = trackLevelSlider_;
    mixTrackLevelLabel_ = trackLevelDbLabel_;

    mixLocalInputSection_ = makeSectionLabel(QStringLiteral("Local input"));
    mixInputMeterRow_ = makeMeterRow(QStringLiteral("Input"), mixInputMeter_);
    mixSendRow_ = makeRow(QStringLiteral("Send"), mixSendLevelSlider_, mixSendLevelLabel_);
    mixSendMeterRow_ = makeMeterRow(QStringLiteral("Post-send"), mixSendMeter_);
    layout->addWidget(mixLocalInputSection_);
    layout->addWidget(mixInputMeterRow_);
    layout->addWidget(mixSendRow_);
    layout->addWidget(mixSendMeterRow_);

    mixMonitorEnableRow_ = new QWidget(page);
    auto* monitorEnableLayout = new QHBoxLayout(mixMonitorEnableRow_);
    monitorEnableLayout->setContentsMargins(0, 0, 0, 0);
    monitorEnableLayout->setSpacing(10);
    auto* monitorName = new QLabel(QStringLiteral("Monitoring"), mixMonitorEnableRow_);
    monitorName->setMinimumWidth(120);
    monitorEnableLayout->addWidget(monitorName);
    monitorEnableLayout->addWidget(mixMonitorCheck_, 1);
    mixMonitorRow_ = makeRow(QStringLiteral("Monitor"), mixMonitorLevelSlider_, mixMonitorLevelLabel_);
    mixMonitorMeterRow_ = makeMeterRow(QStringLiteral("Monitor meter"), mixMonitorMeter_);
    layout->addWidget(mixMonitorEnableRow_);
    layout->addWidget(mixMonitorRow_);
    layout->addWidget(mixMonitorMeterRow_);

    mixTrackSection_ = makeSectionLabel(QStringLiteral("Track"));
    mixTrackRow_ = makeRow(QStringLiteral("Track"), trackLevelSlider_, trackLevelDbLabel_);
    mixTrackMeterRow_ = makeMeterRow(QStringLiteral("Track meter"), mixTrackMeter_);
    layout->addWidget(mixTrackSection_);
    layout->addWidget(mixTrackRow_);
    layout->addWidget(mixTrackMeterRow_);

    mixMetronomeSection_ = makeSectionLabel(QStringLiteral("Metronome"));
    mixMetronomeRow_ = makeRow(QStringLiteral("Metronome"), metronomeLevelSlider_, mixMetronomeLevelLabel_);
    mixMetronomeMeterRow_ = makeMeterRow(QStringLiteral("Click meter"), mixMetronomeMeter_);
    layout->addWidget(mixMetronomeSection_);
    layout->addWidget(mixMetronomeRow_);
    layout->addWidget(mixMetronomeMeterRow_);

    mixOutputSection_ = makeSectionLabel(QStringLiteral("Output"));
    mixOutputMeterRow_ = makeMeterRow(QStringLiteral("Main output"), mixOutputMeter_, mixOutputClipLabel_);
    layout->addWidget(mixOutputSection_);
    layout->addWidget(mixOutputMeterRow_);

    mixRemotePeersSection_ = makeSectionLabel(QStringLiteral("Remote peers"));
    layout->addWidget(mixRemotePeersSection_);
    mixRemotePeerRow_ = new QWidget(page);
    auto* remoteLayout = new QVBoxLayout(mixRemotePeerRow_);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(6);
    remoteLayout->addWidget(makeRow(QStringLiteral("Remote mix"), remoteLevelSlider_, mixRemotePeerLevelLabel_));
    remoteLayout->addWidget(makeMeterRow(QStringLiteral("Remote meter"), mixRemotePeerMeter_));
    auto* peerList = new QWidget(mixRemotePeerRow_);
    mixRemotePeerListLayout_ = new QVBoxLayout(peerList);
    mixRemotePeerListLayout_->setContentsMargins(120, 0, 0, 0);
    mixRemotePeerListLayout_->setSpacing(4);
    remoteLayout->addWidget(peerList);
    layout->addWidget(mixRemotePeerRow_);
    layout->addStretch(1);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);

    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll, 1);

    QObject::connect(trackLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().trackGainDb = static_cast<double>(value);
        if (trackLevelDbLabel_) {
            trackLevelDbLabel_->setText(dbText(trackController_.model().trackGainDb));
        }
        if (jam2_.isRunning()) {
            sendPreparedTrackLevel();
        }
    });
    QObject::connect(mixSendLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (mixSendLevelLabel_) {
            mixSendLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        updateRuntimeControls();
    });
    QObject::connect(mixMonitorCheck_, &QCheckBox::toggled, this, [this] {
        updateMixControls();
        updateRuntimeControls();
    });
    QObject::connect(mixMonitorLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (mixMonitorLevelLabel_) {
            mixMonitorLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        updateRuntimeControls();
    });
    QObject::connect(metronomeLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (mixMetronomeLevelLabel_) {
            mixMetronomeLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        if (jam2_.isRunning()) {
            const double metronomeLevel = gainFromDb(static_cast<double>(value));
            sendJamCommand(QStringLiteral("metro level %1").arg(metronomeLevel, 0, 'f', 3));
        }
    });
    QObject::connect(remoteLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (mixRemotePeerLevelLabel_) {
            mixRemotePeerLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        updateRuntimeControls();
    });
    QObject::connect(jamRecordingButton_, &QPushButton::clicked, this, [this] {
        if (jamRecordingActive_) {
            stopJamRecording();
        } else {
            startJamRecording();
        }
    });
    updateMixControls();
    updateMixRemotePeers();
    updateJamRecordingControls();
    return page;
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
    if (!meshActive_) {
        remotePeerConnected_ = visible;
    }
    updateMixRemotePeers();
}

void MainWindow::updateMixRemotePeers()
{
    QStringList peers;
    if (meshActive_) {
        peers = meshPeerEndpointsExcludingSelf();
    } else if (remotePeerConnected_) {
        peers << QStringLiteral("Connected peer");
    }

    if (mixRemotePeerListLayout_) {
        while (QLayoutItem* item = mixRemotePeerListLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (int index = 0; index < peers.size(); ++index) {
            auto* label = new QLabel(
                meshActive_
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
    jamRecordingFolder_ = timestampedJamRecordingFolder();
    QDir().mkpath(jamRecordingFolder_);
    sendJamCommand(QStringLiteral("record jam start %1").arg(jamRecordingFolder_));
    appendLog(QStringLiteral("record jam start: ") + jamRecordingFolder_);
    jamRecordingActive_ = true;
    updateJamRecordingControls();
}

void MainWindow::stopJamRecording()
{
    if (!jam2_.isRunning()) {
        return;
    }
    sendJamCommand(QStringLiteral("record jam stop"));
    appendLog(QStringLiteral("record jam stop"));
    jamRecordingActive_ = false;
    updateJamRecordingControls();
}

void MainWindow::updateJamRecordingControls()
{
    if (jamRecordingButton_) {
        jamRecordingButton_->setEnabled(jam2_.isRunning());
        jamRecordingButton_->setText(jamRecordingActive_
            ? QStringLiteral("Stop Recording Jam")
            : QStringLiteral("Start Recording Jam"));
    }
    if (jamRecordingLabel_) {
        if (jamRecordingActive_ || !jamRecordingFolder_.isEmpty()) {
            jamRecordingLabel_->setText(QFileInfo(jamRecordingFolder_).fileName());
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
    if (!pendingMeshInvitePopup_ || !meshActive_ || !controlServerMode_) {
        return;
    }
    pendingMeshInvitePopup_ = false;
    const QString inviteUrl = meshInviteUrl();
    QApplication::clipboard()->setText(inviteUrl);
    appendLog(QStringLiteral("jam ready; invite URL copied to clipboard"));

    QMessageBox message(this);
    message.setWindowTitle(QStringLiteral("Jam Ready"));
    message.setIcon(QMessageBox::Information);
    message.setText(QStringLiteral("Send this URL to the people joining your jam."));
    message.setInformativeText(
        QStringLiteral("%1\n\nThe URL has been copied to your clipboard.").arg(inviteUrl));
    message.setTextFormat(Qt::PlainText);
    message.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    message.setStandardButtons(QMessageBox::Ok);
    message.exec();
}

void MainWindow::updateConnectionControlState()
{
    if (publicHostEdit_) {
        publicHostEdit_->setEnabled(true);
    }
    if (stunServerEdit_) {
        stunServerEdit_->setEnabled(false);
    }
    if (stunTimeoutSpin_) {
        stunTimeoutSpin_->setEnabled(false);
    }
    if (stunRetriesSpin_) {
        stunRetriesSpin_->setEnabled(false);
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
    sampleRate->setRange(8000, 384000);
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
    QStringList args{QStringLiteral("local")};
    args << commonJamArgs();
    if (engineModeLabel_) {
        engineModeLabel_->setText(QStringLiteral("Local"));
    }
    launchLocalPerformProcess(args);
}

void MainWindow::startJam()
{
    if (jam2_.isRunning() && !localEngineActive_) {
        return;
    }
    QString permissionError;
    if (!jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Microphone Access"), permissionError);
        appendLog(permissionError);
        return;
    }
    if (localEngineActive_) {
        replacingLocalEngine_ = true;
        jam2_.stop();
        localEngineActive_ = false;
    }
    QStringList args;
    if (engineModeLabel_) {
        engineModeLabel_->setText(QStringLiteral("Network"));
    }
    meshActive_ = true;
    activePublicEndpoint_.clear();
    pendingJoinLaunch_ = false;
    pendingJoinSettingsReady_ = false;
    pendingJoinMembershipReady_ = false;
    pendingJoinBaseArgs_.clear();
    currentMeshPeers_.clear();
    meshCoordinatorToken_.clear();
    localMeshPeerTokens_.clear();
    pendingRecordingContributions_.clear();
    appliedRecordingContributionIds_.clear();
    localRecordingOffers_.clear();
    recordingOfferAssetPaths_.clear();
    pendingRecordingAssetSources_.clear();
    validatedRecordingAssetHashes_.clear();
    try {
        if (sessionCreator_) {
            meshPeerEndpoints_.clear();
            meshCoordinatorToken_ = meshPeerToken();
            meshPeerEndpoints_[meshPeerToken()] = localMeshEndpoint();
            args << QStringLiteral("network") << QStringLiteral("_run")
                 << QStringLiteral("--bind") << meshBindEndpoint()
                 << QStringLiteral("--session-id") << sessionHex()
                 << QStringLiteral("--session-key") << keyHex()
                 << QStringLiteral("--bootstrap-role") << QStringLiteral("creator")
                 << QStringLiteral("--local-peer-id") << QString::number(meshPeerIdForToken(meshPeerToken()))
                 << QStringLiteral("--bootstrap-coordinator-peer-id") << QString::number(meshPeerIdForToken(meshCoordinatorToken_))
                 << QStringLiteral("--peers") << QString();
            if (!controlServer_.listen(static_cast<quint16>(portSpin_->value()), sessionHex(), keyHex())) {
                const QString error = QStringLiteral("control server failed: ") + controlServer_.errorString();
                appendLog(error);
                meshActive_ = false;
                QMessageBox::warning(this, QStringLiteral("Jam2"), error);
                return;
            }
            pendingMeshInvitePopup_ = true;
            controlServerMode_ = true;
            controlHost_ = bindHostEdit_->text();
            controlPort_ = static_cast<quint16>(portSpin_->value());
            controlSessionHex_ = sessionHex();
            controlKeyHex_ = keyHex();
        } else {
            const std::string url = connectUrlEdit_->text().toStdString();
            const jam2::SessionInfo info = jam2::parse_jam_url(url);
            sessionId_ = info.session_id;
            sessionKey_ = info.key;
            meshPeerEndpoints_.clear();
            meshPeerEndpoints_[meshPeerToken()] = localMeshEndpoint();
            pendingJoinLaunch_ = true;
            controlServerMode_ = false;
            controlHost_ = QString::fromStdString(info.endpoint.host);
            controlPort_ = info.endpoint.port;
            controlSessionHex_ = sessionHex();
            controlKeyHex_ = keyHex();
            controlClient_.connectToHost(
                QString::fromStdString(info.endpoint.host),
                info.endpoint.port,
                sessionHex(),
                keyHex(),
                meshPeerToken(),
                localMeshEndpoint());
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QString::fromUtf8(error.what()));
        return;
    }

    controlReconnectEnabled_ = true;
    controlReconnectAttempts_ = 0;
    if (!sessionCreator_) {
        appendLog(QStringLiteral("waiting for leader settings and membership before launching Jam2"));
        startButton_->setEnabled(false);
        joinButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        if (refreshControlButton_) {
            refreshControlButton_->setEnabled(true);
        }
        scheduleControlReconnect();
        return;
    }
    args << commonJamArgs();
    currentMeshPeers_ = meshPeerSpecsExcludingSelf();
    launchJamProcess(args);
}

void MainWindow::launchLocalPerformProcess(QStringList args)
{
    controlServer_.close();
    controlClient_.close();
    controlReconnectEnabled_ = false;
    meshActive_ = false;
    localEngineActive_ = true;
    jam2_.start(QCoreApplication::applicationFilePath(), args);
    appendLog(QStringLiteral("starting embedded local engine: %1").arg(args.join(QLatin1Char(' '))));
    startButton_->setEnabled(true);
    joinButton_->setEnabled(true);
    stopButton_->setEnabled(false);
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

void MainWindow::launchJamProcess(QStringList args)
{
    QString permissionError;
    if (!jam2EnsureMicrophonePermission(&permissionError)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Microphone Access"), permissionError);
        appendLog(permissionError);
        return;
    }

    if (controlServerMode_) {
        localMetronomeRunning_ = false;
        localMetronomeLeader_ = false;
    }
    playbackGrid_.clearEngine();
    if (controlServerMode_ && trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Jam metronome stopped"));
    }
    if (controlServerMode_ && startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }
    if (controlServerMode_ && stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
    localEngineActive_ = false;
    jam2_.start(QCoreApplication::applicationFilePath(), args);
    appendLog(QStringLiteral("starting embedded network session"));
    startButton_->setEnabled(false);
    joinButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(true);
    }
    jamRecordingActive_ = false;
    updateJamRecordingControls();
    updateMixControls();
    connectionLabel_->setText(
        (!controlServerMode_ && controlClient_.isConnected()) ||
                (controlServerMode_ && controlServer_.hasPeer())
            ? QStringLiteral("TCP peer authenticated")
            : QStringLiteral("Starting"));
}

void MainWindow::sendJamCommand(const QString& line)
{
    GuiBinaryCommand command;
    if (!encodeJamTextCommand(line, command)) {
        appendLog(QStringLiteral("embedded engine unsupported command: ") + line);
        return;
    }
    if (!jam2_.sendControl(encodeGuiCommandPayload(command))) {
        appendLog(QStringLiteral("embedded engine command unavailable: ") + line);
    }
}

void MainWindow::handleGuiControlFrame(quint16 type, const QByteArray& payload)
{
    const auto messageType = static_cast<jam2::gui_control::MessageType>(type);
    if (messageType == jam2::gui_control::MessageType::Hello) {
        return;
    }
    if (messageType == jam2::gui_control::MessageType::ClockState) {
        if (payload.size() < 60) {
            return;
        }
        const quint64 rawFrame = guiU64(payload, 0);
        const quint64 musicalFrame = guiU64(payload, 8);
        const quint64 epochFrame = guiU64(payload, 16);
        const qint64 offsetFrames = guiI64(payload, 24);
        const int sampleRate = static_cast<int>(guiU32(payload, 32));
        const int bpm = static_cast<int>(guiU32(payload, 36));
        const int beats = static_cast<int>(guiU32(payload, 40));
        const int division = static_cast<int>(guiU32(payload, 44));
        const bool running = guiU16(payload, 56) != 0;
        playbackGrid_.setPattern(bpm, beats, division);
        playbackGrid_.updateEngine(rawFrame, musicalFrame, epochFrame, offsetFrames, sampleRate, running);
        if (payload.size() >= 84) {
            recordingInputLatencyFrames_ = guiU32(payload, 60);
            recordingOutputLatencyFrames_ = guiU32(payload, 64);
            const qint64 adjustmentFrames = guiI64(payload, 68);
            recordingAppliedLatencyFrames_ = guiU64(payload, 76);
            recordingLatencySampleRate_ = qMax(1, sampleRate);
            if (recordingLatencyAdjustmentSpin_) {
                const QSignalBlocker blocker(recordingLatencyAdjustmentSpin_);
                recordingLatencyAdjustmentSpin_->setValue(static_cast<int>(qBound<qint64>(
                    static_cast<qint64>(recordingLatencyAdjustmentSpin_->minimum()),
                    adjustmentFrames,
                    static_cast<qint64>(recordingLatencyAdjustmentSpin_->maximum()))));
            }
            updateRecordingLatencyDisplay();
        }
        updatePlaybackGrid();
        return;
    }
    if (messageType == jam2::gui_control::MessageType::Meters) {
        if (payload.size() < 56) {
            return;
        }
        QJsonObject stats;
        stats.insert(QStringLiteral("event"), QStringLiteral("meters"));
        stats.insert(QStringLiteral("input_peak"), guiF64(payload, 0));
        stats.insert(QStringLiteral("send_peak"), guiF64(payload, 8));
        stats.insert(QStringLiteral("monitor_peak"), guiF64(payload, 16));
        stats.insert(QStringLiteral("remote_peak"), guiF64(payload, 24));
        stats.insert(QStringLiteral("metronome_peak"), guiF64(payload, 32));
        stats.insert(QStringLiteral("output_peak"), guiF64(payload, 40));
        stats.insert(QStringLiteral("output_clipped_samples"), static_cast<double>(guiU64(payload, 48)));
        updateMixMeters(stats);
        return;
    }
    if (messageType == jam2::gui_control::MessageType::TransportState) {
        if (payload.size() < 40) {
            return;
        }
        const quint64 revision = guiU64(payload, 0);
        const quint64 targetRawFrame = guiU64(payload, 8);
        const quint64 targetMusicalFrame = guiU64(payload, 16);
        const quint64 countdownStartFrame = guiU64(payload, 24);
        const auto action = static_cast<jam2::gui_control::TransportAction>(guiU16(payload, 32));
        if (action == jam2::gui_control::TransportAction::TrackRestart && revision > 0) {
            playbackGrid_.scheduleEpoch(targetRawFrame, targetMusicalFrame, revision);
            if (gridScheduleLabel_) {
                gridScheduleLabel_->setText(QStringLiteral("Track restart: engine target_frame=%1 revision=%2")
                    .arg(targetRawFrame)
                    .arg(revision));
            }
        } else if (action == jam2::gui_control::TransportAction::RecordStart &&
                   revision > recordingScheduleRevision_) {
            playbackGrid_.scheduleEpoch(targetRawFrame, targetMusicalFrame, revision);
            recordingScheduleRevision_ = revision;
            recordingCountdownStartFrame_ = countdownStartFrame;
            recordingStartFrame_ = targetRawFrame;
            if ((!captureManualStopCheck_ || !captureManualStopCheck_->isChecked()) &&
                captureDurationSpin_) {
                const PlaybackGrid::Position position = playbackGrid_.position();
                const quint64 framesUntilStart = targetRawFrame > position.rawCurrentFrame
                    ? targetRawFrame - position.rawCurrentFrame
                    : 0ULL;
                const int delayMs = position.sampleRate > 0
                    ? static_cast<int>(framesUntilStart * 1000ULL / static_cast<quint64>(position.sampleRate))
                    : 0;
                const QString takeId = activeTrackTakeId_;
                QTimer::singleShot(delayMs + captureDurationSpin_->value() * 1000, this, [this, takeId] {
                    if (trackTakeRecordingActive_ && activeTrackTakeId_ == takeId) {
                        runGridLockedEngineAction(QStringLiteral("record.stop"), [this](std::uint64_t stopFrame) {
                            stopInputCapture(stopFrame);
                        });
                    }
                });
            }
            if (recordingCountdownLabel_) {
                recordingCountdownLabel_->setText(QStringLiteral("WAITING FOR NEXT BAR"));
                recordingCountdownLabel_->show();
            }
            if (gridScheduleLabel_) {
                gridScheduleLabel_->setText(QStringLiteral("Recording: engine countdown_start_frame=%1 start_frame=%2 revision=%3")
                    .arg(countdownStartFrame)
                    .arg(targetRawFrame)
                    .arg(revision));
            }
        }
        return;
    }
    if (messageType == jam2::gui_control::MessageType::TrackState) {
        if (payload.size() < 56) {
            return;
        }
        preparedSourceEngineFrame_ = static_cast<qint64>(guiU64(payload, 0));
        preparedSourceFrame_ = static_cast<qint64>(guiU64(payload, 8));
        const int sampleRate = static_cast<int>(guiU32(payload, 48));
        preparedSourcePlaying_ = guiU16(payload, 52) != 0;
        if (sampleRate > 0) {
            preparedSourceSampleRate_ = sampleRate;
        }
        return;
    }
    if (messageType == jam2::gui_control::MessageType::TrackTakeEvent) {
        if (payload.size() < 60) {
            return;
        }
        const quint16 eventType = guiU16(payload, 0);
        const quint32 takeSize = guiU32(payload, 4);
        const quint32 pathSize = guiU32(payload, 8);
        const quint32 errorSize = guiU32(payload, 12);
        const quint64 startFrame = guiU64(payload, 20);
        const quint64 stopFrame = guiU64(payload, 28);
        const quint64 framesWritten = guiU64(payload, 36);
        const quint64 droppedFrames = guiU64(payload, 44);
        const quint64 writerErrors = guiU64(payload, 52);
        const qsizetype total = 60 + static_cast<qsizetype>(takeSize + pathSize + errorSize);
        if (payload.size() < total) {
            return;
        }
        qsizetype offset = 60;
        const QString takeId = QString::fromUtf8(payload.mid(offset, static_cast<qsizetype>(takeSize)));
        offset += static_cast<qsizetype>(takeSize);
        const QString wav = QString::fromUtf8(payload.mid(offset, static_cast<qsizetype>(pathSize)));
        offset += static_cast<qsizetype>(pathSize);
        const QString error = QString::fromUtf8(payload.mid(offset, static_cast<qsizetype>(errorSize)));
        if (takeId == activeTrackTakeId_) {
            trackTakeRecordingActive_ = false;
            activeTrackTakeId_.clear();
            if (stopCaptureButton_) stopCaptureButton_->setEnabled(false);
            recordingStartFrame_ = 0;
            recordingCountdownStartFrame_ = 0;
            stopMetronomeAtRecordingStart_ = false;
            if (recordingCountdownLabel_) recordingCountdownLabel_->hide();
            if (eventType == static_cast<quint16>(jam2::gui_control::TrackTakeEventType::Stopped) && !wav.isEmpty()) {
                lastCapturePath_ = QDir::fromNativeSeparators(wav);
                if (!pendingTransientCapturePath_.isEmpty()) {
                    registerTransientTrackWav(pendingTransientCapturePath_);
                }
                pendingTransientCapturePath_.clear();
                importLastCaptureToArmedLane();
            } else {
                if (!pendingTransientCapturePath_.isEmpty() && QFileInfo::exists(pendingTransientCapturePath_)) {
                    registerTransientTrackWav(pendingTransientCapturePath_);
                }
                pendingTransientCapturePath_.clear();
            }
            if (loadWavButton_) loadWavButton_->setEnabled(true);
        }
        if (eventType == static_cast<quint16>(jam2::gui_control::TrackTakeEventType::Stopped)) {
            appendLog(QStringLiteral("track take stopped: take_id=%1 wav=%2 start_frame=%3 stop_frame=%4 frames=%5 dropped_frames=%6 writer_errors=%7")
                .arg(takeId)
                .arg(wav)
                .arg(startFrame)
                .arg(stopFrame)
                .arg(framesWritten)
                .arg(droppedFrames)
                .arg(writerErrors));
        } else {
            appendLog(QStringLiteral("track take error: take_id=%1 reason=%2").arg(takeId, error));
        }
        return;
    }
}

void MainWindow::showStartJamDialog()
{
    if (jam2_.isRunning() && !localEngineActive_) {
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
        inputChannelsEdit_, outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, meshMaxPeersSpin_,
        statsWarmupMsSpin_, logStatsEdit_, socketSendBufferSpin_,
        socketRecvBufferSpin_, osPriorityBox_, driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_,
        driftMaxCorrectionSpin_, sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_,
        jitterBufferMaxSpin_, adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_,
        adaptiveMaxSpin_, adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }

    auto* sessionForm = new QFormLayout();
    sessionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    sessionForm->addRow(QStringLiteral("Bind"), bindHostEdit_);
    sessionForm->addRow(QStringLiteral("Port"), portSpin_);
    sessionForm->addRow(QStringLiteral("Public endpoint host"), publicHostEdit_);
    sessionForm->addRow(QString(), new QLabel(
        QStringLiteral("The shareable jam2 URL will appear after the network and audio engine are ready."),
        content));
    sessionForm->addRow(QStringLiteral("STUN server"), stunServerEdit_);
    sessionForm->addRow(QStringLiteral("STUN timeout ms"), stunTimeoutSpin_);
    sessionForm->addRow(QStringLiteral("STUN retries"), stunRetriesSpin_);
    sessionForm->addRow(QString(), noStunCheck_);
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
    advancedForm->addRow(QStringLiteral("Extra jam2 args"), extraArgsEdit_);
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
        inputChannelsEdit_, outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, meshMaxPeersSpin_,
        statsWarmupMsSpin_, logStatsEdit_, socketSendBufferSpin_,
        socketRecvBufferSpin_, osPriorityBox_, driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_,
        driftMaxCorrectionSpin_, sampleTimePlayoutCheck_, playoutDelaySpin_, jitterBufferSpin_,
        jitterBufferMaxSpin_, adaptiveCushionCheck_, adaptiveTargetSpin_, adaptiveMinSpin_,
        adaptiveMaxSpin_, adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : startWidgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        sessionCreator_ = true;
        connectionLabel_->setText(QStringLiteral("Starting listener"));
        startJam();
    }
}

void MainWindow::showJoinJamDialog()
{
    if (jam2_.isRunning() && !localEngineActive_) {
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
        sessionCreator_ = false;
        connectionLabel_->setText(QStringLiteral("Joining"));
        startJam();
    }
}

void MainWindow::stopJam(bool returnToLocal)
{
    returnToLocalAfterStop_ = returnToLocal && !shuttingDown_;
    const bool processWasRunning = jam2_.isRunning();
    controlReconnectEnabled_ = false;
    controlReconnectTimer_.stop();
    controlServer_.close();
    controlClient_.close();
    pendingJoinLaunch_ = false;
    pendingJoinSettingsReady_ = false;
    pendingJoinMembershipReady_ = false;
    pendingJoinBaseArgs_.clear();
    meshActive_ = false;
    localMeshPeerTokens_.clear();
    meshPeerEndpoints_.clear();
    meshCoordinatorToken_.clear();
    currentMeshPeers_.clear();
    pendingMeshInvitePopup_ = false;
    activePublicEndpoint_.clear();
    remotePeerConnected_ = false;
    jam2_.stop();
    localMetronomeRunning_ = false;
    localMetronomeLeader_ = false;
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
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(false);
    }
    jamRecordingActive_ = false;
    updateJamRecordingControls();
    updateMixControls();
    setMixRemotePeerVisible(false);
    if (returnToLocalAfterStop_ && !processWasRunning) {
        returnToLocalAfterStop_ = false;
        QTimer::singleShot(0, this, [this] {
            if (!shuttingDown_ && !jam2_.isRunning()) {
                startLocalPerform();
            }
        });
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
}

void MainWindow::handleOutputLine(const QString& line)
{
    if (!line.startsWith(QStringLiteral("{\"event\":\"status\"")) &&
        !line.startsWith(QStringLiteral("{\"event\":\"mesh_stats\"")) &&
        !line.startsWith(QStringLiteral("mesh_stats ")) &&
        !line.startsWith(QStringLiteral("stats "))) {
        appendLog(line);
    }
    if (line.startsWith(QStringLiteral("Public UDP candidate: "))) {
        const QString endpoint = line.mid(QStringLiteral("Public UDP candidate: ").size()).trimmed();
        try {
            const jam2::Endpoint parsed = jam2::parse_endpoint(endpoint.toStdString());
            activePublicEndpoint_ = QString::fromStdString(jam2::endpoint_to_string(parsed));
            if (publicHostEdit_) {
                publicHostEdit_->setText(QString::fromStdString(parsed.host));
            }
            if (meshActive_ && controlServerMode_) {
                meshPeerEndpoints_[meshPeerToken()] = activePublicEndpoint_;
            }
        } catch (const std::exception& error) {
            appendLog(QStringLiteral("ignored invalid public UDP candidate: ") + QString::fromUtf8(error.what()));
        }
    }
    if (line == QStringLiteral("Embedded network controls ready")) {
        updateRuntimeControls();
        sendJamCommand(localMetronomeLeader_
            ? QStringLiteral("metro leader on")
            : QStringLiteral("metro leader off"));
        sendMetronomeModeToJam();
        sendMetronomePatternToJam();
        sendJamCommand(localMetronomeRunning_
            ? QStringLiteral("metro on")
            : QStringLiteral("metro off"));
        showPendingMeshInviteUrl();
    }
    handleStatsLine(line);
}

void MainWindow::handleStatsLine(const QString& line)
{
    const bool statsLine = line.startsWith(QStringLiteral("stats "));
    const bool meshStatsLine = line.startsWith(QStringLiteral("mesh_stats "));
    if (!statsLine && !meshStatsLine) {
        return;
    }
    QJsonObject stats;
    stats.insert(QStringLiteral("event"), statsLine ? QStringLiteral("stats") : QStringLiteral("mesh_stats"));
    const QRegularExpression fieldRe(QStringLiteral("(\\S+)=([^\\s]+)"));
    auto it = fieldRe.globalMatch(line.mid(statsLine ? 6 : 11));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString key = match.captured(1);
        const QString value = match.captured(2);
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (ok) {
            stats.insert(key, number);
        } else {
            stats.insert(key, value);
        }
    }
    if (stats.contains(QStringLiteral("sent")) && !stats.contains(QStringLiteral("sent_packets"))) {
        stats.insert(QStringLiteral("sent_packets"), stats.value(QStringLiteral("sent")).toDouble());
    }
    if (stats.contains(QStringLiteral("recv")) && !stats.contains(QStringLiteral("recv_packets"))) {
        stats.insert(QStringLiteral("recv_packets"), stats.value(QStringLiteral("recv")).toDouble());
    }
    if (meshStatsLine) {
        updateMixMeters(stats);
    }
    updateStatsDisplay(stats);
}

void MainWindow::updateStatsDisplay(const QJsonObject& stats)
{
    QString latency = metricText(stats, QStringLiteral("estimated_one_way_ms"), QStringLiteral(" ms"));
    if (latency == QStringLiteral("-")) {
        latency = metricText(stats, QStringLiteral("rtt_avg_ms"), QStringLiteral(" ms"));
    }
    latencyLabel_->setText(QStringLiteral("Latency ") + latency);
    const QString jitterAvg = metricText(stats, QStringLiteral("jitter_avg_ms"), QStringLiteral(" ms"));
    const QString jitterMax = metricText(stats, QStringLiteral("jitter_max_ms"), QStringLiteral(" ms"));
    const QString packetGapMax = metricText(stats, QStringLiteral("audio_packet_gap_max_ms"), QStringLiteral(" ms"));
    jitterLabel_->setText(QStringLiteral("Jitter %1/%2 gap %3").arg(jitterAvg, jitterMax, packetGapMax));
    const QString lostPackets = integerText(stats, QStringLiteral("sequence_lost"));
    const QString lossEvents = integerText(stats, QStringLiteral("sequence_loss_events"));
    const QString lossMaxGap = integerText(stats, QStringLiteral("sequence_loss_max_gap"));
    lossLabel_->setText(QStringLiteral("Loss %1 lost %2 ev %3 max %4p")
        .arg(metricText(stats, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2))
        .arg(lostPackets)
        .arg(lossEvents)
        .arg(lossMaxGap));
    depthLabel_->setText(QStringLiteral("Depth ") + metricText(
        stats,
        QStringLiteral("playback_depth_avg_ms"),
        QStringLiteral(" ms"),
        1,
        QStringLiteral("playback_depth_ms")));
    QString outOfOrder = integerText(stats, QStringLiteral("sequence_out_of_order"));
    if (outOfOrder == QStringLiteral("-")) {
        outOfOrder = integerText(stats, QStringLiteral("out_of_order"));
    }
    QString latePackets = integerText(stats, QStringLiteral("sequence_late"));
    if (latePackets == QStringLiteral("-")) {
        latePackets = integerText(stats, QStringLiteral("late"));
    }
    ringDepthLabel_->setText(QStringLiteral("Reorder oo %1 rec %2 lost %3 late %4 max %5p")
        .arg(outOfOrder)
        .arg(integerText(stats, QStringLiteral("reordered_recovered")))
        .arg(integerText(stats, QStringLiteral("reordered_lost")))
        .arg(latePackets)
        .arg(integerText(stats, QStringLiteral("reordered_max_distance_packets"))));
    QString underrunText = QStringLiteral("-");
    if (hasNumber(stats, QStringLiteral("playback_ring_underrun_time_ms"))) {
        const double underrunMs = stats.value(QStringLiteral("playback_ring_underrun_time_ms")).toDouble();
        underrunText = durationText(underrunMs);
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_events"))) {
            underrunText += QStringLiteral(" ev %1").arg(integerText(stats, QStringLiteral("playback_ring_underrun_events")));
        }
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_event_max_ms"))) {
            underrunText += QStringLiteral(" max %1").arg(metricText(
                stats,
                QStringLiteral("playback_ring_underrun_event_max_ms"),
                QStringLiteral(" ms")));
        }
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_burst_max_ms"))) {
            underrunText += QStringLiteral(" burst %1").arg(metricText(
                stats,
                QStringLiteral("playback_ring_underrun_burst_max_ms"),
                QStringLiteral(" ms")));
        }
    } else if (hasNumber(stats, QStringLiteral("playback_ring_underruns"))) {
        underrunText = integerText(stats, QStringLiteral("playback_ring_underruns")) + QStringLiteral(" fr");
    }
    underrunLabel_->setText(QStringLiteral("Underrun ") + underrunText);
    missingFramesLabel_->setText(QStringLiteral("Stall loop %1 burst %2p gap>2x %3 missing %4 latefr %5")
        .arg(metricText(stats, QStringLiteral("receive_loop_gap_max_ms"), QStringLiteral(" ms")))
        .arg(integerText(stats, QStringLiteral("receive_burst_packets_max")))
        .arg(integerText(stats, QStringLiteral("audio_packet_gap_over_2x_count")))
        .arg(integerText(stats, QStringLiteral("missing_audio_frames_inserted")))
        .arg(integerText(stats, QStringLiteral("late_audio_frames_dropped"))));
    driftLabel_->setText(QStringLiteral("Drift ") + metricText(stats, QStringLiteral("drift_ppm"), QStringLiteral(" ppm")));
    const bool guiMeterFeedActive = jam2_.isRunning();
    if (!guiMeterFeedActive) {
        updateMixMeters(stats);
    }
    diagnosisLabel_->setText(diagnoseStats(stats));
}

void MainWindow::updateMixMeters(const QJsonObject& stats)
{
    if (mixInputMeter_ && stats.contains(QStringLiteral("input_peak"))) {
        mixInputMeter_->setLevel(stats.value(QStringLiteral("input_peak")).toDouble(0.0));
    }
    if (mixSendMeter_ && stats.contains(QStringLiteral("send_peak"))) {
        mixSendMeter_->setLevel(stats.value(QStringLiteral("send_peak")).toDouble(0.0));
    }
    if (mixMonitorMeter_ && stats.contains(QStringLiteral("monitor_peak"))) {
        mixMonitorMeter_->setLevel(stats.value(QStringLiteral("monitor_peak")).toDouble(0.0));
    }
    if (mixRemotePeerMeter_ && stats.contains(QStringLiteral("remote_peak"))) {
        mixRemotePeerMeter_->setLevel(stats.value(QStringLiteral("remote_peak")).toDouble(0.0));
    }
    if (mixMetronomeMeter_ && stats.contains(QStringLiteral("metronome_peak"))) {
        mixMetronomeMeter_->setLevel(stats.value(QStringLiteral("metronome_peak")).toDouble(0.0));
    }
    if (mixOutputMeter_ && stats.contains(QStringLiteral("output_peak"))) {
        mixOutputMeter_->setLevel(stats.value(QStringLiteral("output_peak")).toDouble(0.0));
    }
    if (mixOutputClipLabel_ && stats.contains(QStringLiteral("output_clipped_samples"))) {
        mixOutputClipLabel_->setText(QStringLiteral("clip %1").arg(static_cast<qulonglong>(
            stats.value(QStringLiteral("output_clipped_samples")).toDouble(0.0))));
    }
}

void MainWindow::handleControlMessage(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    QString validationError;
    if (!validateControlMessageShape(message, validationError)) {
        appendLog(QStringLiteral("rejected control message: ") + validationError);
        return;
    }
    const bool fromJoinerConnection = !sourcePeerToken.isEmpty();
    if (fromJoinerConnection &&
        (type == QStringLiteral("session.settings") || type == QStringLiteral("session.membership"))) {
        appendLog(QStringLiteral("rejected unauthorized control family from peer ") +
            sourcePeerToken.left(8) + QStringLiteral(": ") + type);
        return;
    }
    const bool trackMessage =
        type == QStringLiteral("looper.recording.offer") ||
        type == QStringLiteral("looper.asset.request") ||
        type == QStringLiteral("looper.asset.start") ||
        type == QStringLiteral("looper.asset.chunk") ||
        type == QStringLiteral("looper.asset.done");
    if (trackMessage && !trackController_.model().syncControls) {
        appendLog(QStringLiteral("ignored remote track sync while local sync is disabled"));
        return;
    }
    if (type == QStringLiteral("session.settings")) {
        applyLeaderSettings(message.value(QStringLiteral("settings")).toObject());
        pendingJoinSettingsReady_ = true;
        launchPendingJoin();
    } else if (type == QStringLiteral("session.membership")) {
        applyMeshPeerList(message);
    } else if (type == QStringLiteral("session.error")) {
        const QString text = message.value(QStringLiteral("message")).toString(QStringLiteral("Session error"));
        appendLog(QStringLiteral("peer session error: ") + text);
        QMessageBox::warning(this, QStringLiteral("Jam2"), text);
    } else if (type == QStringLiteral("metronome.settings")) {
        applyRemoteMetronomeSettings(message);
    } else if (type == QStringLiteral("beat.set")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        BeatGridModel* model = &beatModel_;
        if (lane == QStringLiteral("chord")) {
            model = &chordModel_;
        } else if (lane == QStringLiteral("lyric")) {
            model = &lyricModel_;
        }
        model->setCell(
            message.value(QStringLiteral("section")).toInt(),
            lane,
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("text")).toString());
        refreshSongView(lane);
    } else if (type == QStringLiteral("grid.resize")) {
        const QString lane = message.value(QStringLiteral("lane")).toString(QStringLiteral("beat"));
        BeatGridModel* model = lane == QStringLiteral("chord") ? &chordModel_ : &beatModel_;
        model->resizeSection(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beats")).toInt(8));
        refreshSongView(lane);
    } else if (type == QStringLiteral("beat.hit")) {
        beatModel_.setBeatHit(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("lane")).toInt(),
            message.value(QStringLiteral("text")).toString());
        refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("beat.division")) {
        beatModel_.setBeatDivision(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("division")).toInt(4));
        refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("lyrics.set")) {
        lyricModel_.setLyricsText(message.value(QStringLiteral("text")).toString());
        refreshSongView(QStringLiteral("lyric"));
    } else if (type == QStringLiteral("song.set")) {
        handleSongSet(message);
    } else if (type == QStringLiteral("looper.recording.offer")) {
        handleRecordingOffer(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.request")) {
        handleLooperAssetRequest(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.start")) {
        receiveLooperAssetStart(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.chunk")) {
        receiveLooperAssetChunk(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.done")) {
        receiveLooperAssetDone(message, sourcePeerToken);
    }
}

void MainWindow::handleControlState(const QString& state, bool serverSide)
{
    const QString displayState = !serverSide && state == QStringLiteral("TCP control authenticated")
        ? QStringLiteral("TCP peer authenticated")
        : state;
    connectionLabel_->setText(displayState);
    appendLog(QStringLiteral("control: ") + state);
    if (state.contains(QStringLiteral("disconnected"), Qt::CaseInsensitive) ||
        state.contains(QStringLiteral("rejected"), Qt::CaseInsensitive) ||
        state.contains(QStringLiteral("timeout"), Qt::CaseInsensitive) ||
        state.contains(QStringLiteral("high-water"), Qt::CaseInsensitive)) {
        if (serverSide) {
            const ControlServer::Stats stats = controlServer_.stats();
            appendLog(QStringLiteral(
                "control_stats side=server accepted=%1 auth_rejects=%2 auth_timeouts=%3 frame_rejects=%4 "
                "frame_timeouts=%5 tag_or_sequence_rejects=%6 output_rejects=%7 input_high_water=%8 output_high_water=%9")
                .arg(stats.acceptedConnections)
                .arg(stats.authenticationRejects)
                .arg(stats.authenticationTimeouts)
                .arg(stats.frameRejects)
                .arg(stats.frameTimeouts)
                .arg(stats.sequenceOrTagRejects)
                .arg(stats.outputHighWaterRejects)
                .arg(stats.maxBufferedInputBytes)
                .arg(stats.maxQueuedOutputBytes));
        } else {
            const ControlClient::Stats stats = controlClient_.stats();
            appendLog(QStringLiteral(
                "control_stats side=client auth_rejects=%1 auth_timeouts=%2 frame_rejects=%3 frame_timeouts=%4 "
                "tag_or_sequence_rejects=%5 output_rejects=%6 input_high_water=%7 output_high_water=%8")
                .arg(stats.authenticationRejects)
                .arg(stats.authenticationTimeouts)
                .arg(stats.frameRejects)
                .arg(stats.frameTimeouts)
                .arg(stats.sequenceOrTagRejects)
                .arg(stats.outputHighWaterRejects)
                .arg(stats.maxBufferedInputBytes)
                .arg(stats.maxQueuedOutputBytes));
        }
    }
    if (serverSide && state == QStringLiteral("TCP peer authenticated")) {
        remotePeerConnected_ = true;
        controlReconnectAttempts_ = 0;
        controlReconnectTimer_.stop();
        setMixRemotePeerVisible(true);
        sendLeaderSettings();
        sendMetronomeSettingsToPeer();
        if (looperProject_.trackSyncEnabled()) {
            sendSongSnapshot();
        }
    } else if (!serverSide && state == QStringLiteral("TCP control authenticated")) {
        remotePeerConnected_ = true;
        controlReconnectAttempts_ = 0;
        controlReconnectTimer_.stop();
        setMixRemotePeerVisible(true);
        for (const QJsonObject& offer : localRecordingOffers_) {
            sendControl(offer);
        }
    } else if (state.startsWith(QStringLiteral("TCP auth failed:"))) {
        remotePeerConnected_ = false;
        controlReconnectEnabled_ = false;
        controlReconnectTimer_.stop();
        pendingJoinLaunch_ = false;
        pendingJoinSettingsReady_ = false;
        pendingJoinMembershipReady_ = false;
        pendingJoinBaseArgs_.clear();
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
        QMessageBox::warning(this, QStringLiteral("Jam2"), state);
    } else if ((jam2_.isRunning() || pendingJoinLaunch_) && controlReconnectEnabled_ &&
        (state == QStringLiteral("TCP control disconnected") || state == QStringLiteral("TCP peer disconnected"))) {
        remotePeerConnected_ = false;
        setMixRemotePeerVisible(false);
        if (serverSide) {
            connectionLabel_->setText(QStringLiteral("Waiting for peers"));
            return;
        }
        if (meshActive_) {
            meshPeerEndpoints_.clear();
            meshPeerEndpoints_[meshPeerToken()] = localMeshEndpoint();
            meshCoordinatorToken_.clear();
            pendingJoinSettingsReady_ = false;
            pendingJoinMembershipReady_ = false;
        }
        scheduleControlReconnect();
    }
}

void MainWindow::refreshControlConnection()
{
    if (!controlReconnectEnabled_ || controlHost_.isEmpty() || controlPort_ == 0) {
        return;
    }
    if (controlServerMode_ && controlServer_.hasPeer()) {
        sendLeaderSettings();
        appendLog(QStringLiteral("TCP control already connected; resent leader settings"));
        return;
    }
    if (!controlServerMode_ && controlClient_.isConnected()) {
        appendLog(QStringLiteral("TCP control already connected"));
        return;
    }
    ++controlReconnectAttempts_;
    if (connectionLabel_) {
        connectionLabel_->setText(QStringLiteral("Refreshing TCP control (%1)").arg(controlReconnectAttempts_));
    }
    appendLog(QStringLiteral("refreshing TCP control attempt %1").arg(controlReconnectAttempts_));
    if (controlServerMode_) {
        if (!controlServer_.listen(controlPort_, controlSessionHex_, controlKeyHex_)) {
            appendLog(QStringLiteral("control server refresh failed: ") + controlServer_.errorString());
        }
    } else {
        controlClient_.connectToHost(
            controlHost_,
            controlPort_,
            controlSessionHex_,
            controlKeyHex_,
            meshActive_ ? meshPeerToken() : QString(),
            meshActive_ ? localMeshEndpoint() : QString());
    }
    if (controlReconnectAttempts_ >= 15) {
        controlReconnectTimer_.stop();
        if (connectionLabel_) {
            connectionLabel_->setText(QStringLiteral("TCP refresh required"));
        }
        appendLog(QStringLiteral("TCP control auto reconnect paused; use Refresh Control to retry"));
    }
}

void MainWindow::scheduleControlReconnect()
{
    if (!controlReconnectEnabled_ || controlReconnectTimer_.isActive()) {
        return;
    }
    controlReconnectAttempts_ = 0;
    controlReconnectTimer_.start();
}

void MainWindow::sendLeaderSettings()
{
    if (!controlServer_.hasPeer()) {
        return;
    }
    controlServer_.send(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session.settings")},
        {QStringLiteral("settings"), leaderSettingsMessage()},
    });
}

QJsonObject MainWindow::leaderSettingsMessage() const
{
    return QJsonObject{
        {QStringLiteral("profile"), profileBox_ ? profileBox_->currentData().toString() : QStringLiteral("fast")},
        {QStringLiteral("sample_rate"), sampleRateSpin_->value()},
        {QStringLiteral("audio_buffer_size"), bufferSizeSpin_->value()},
        {QStringLiteral("frame_size"), frameSizeSpin_->value()},
        {QStringLiteral("playback_prefill_frames"), prefillSpin_->value()},
        {QStringLiteral("playback_max_frames"), playbackMaxSpin_->value()},
        {QStringLiteral("capture_ring_frames"), captureRingSpin_->value()},
        {QStringLiteral("playback_ring_frames"), playbackRingSpin_->value()},
        {QStringLiteral("stats_warmup_ms"), statsWarmupMsSpin_->value()},
        {QStringLiteral("socket_send_buffer"), socketSendBufferSpin_->value()},
        {QStringLiteral("socket_recv_buffer"), socketRecvBufferSpin_->value()},
        {QStringLiteral("drift_correction"), driftCorrectionCheck_->isChecked()},
        {QStringLiteral("drift_smoothing"), driftSmoothingSpin_->value()},
        {QStringLiteral("drift_deadband_ppm"), driftDeadbandSpin_->value()},
        {QStringLiteral("drift_max_correction_ppm"), driftMaxCorrectionSpin_->value()},
        {QStringLiteral("bpm"), metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value()},
        {QStringLiteral("remote_level"), gainFromDb(static_cast<double>(remoteLevelSlider_->value()))},
        {QStringLiteral("metronome_mode"), metronomeModeBox_->currentText()},
        {QStringLiteral("metronome_compensation_max_ms"), metronomeCompensationMaxSpin_ ? metronomeCompensationMaxSpin_->value() : 250.0},
        {QStringLiteral("metronome_compensation_smoothing_ms"), metronomeCompensationSmoothingSpin_ ? metronomeCompensationSmoothingSpin_->value() : 750.0},
        {QStringLiteral("metronome_compensation_deadband_ms"), metronomeCompensationDeadbandSpin_ ? metronomeCompensationDeadbandSpin_->value() : 1.0},
        {QStringLiteral("metronome_compensation_slew_ms_per_sec"), metronomeCompensationSlewSpin_ ? metronomeCompensationSlewSpin_->value() : 40.0},
        {QStringLiteral("sample_time_playout"), sampleTimePlayoutCheck_->isChecked()},
        {QStringLiteral("playout_delay_frames"), playoutDelaySpin_->value()},
        {QStringLiteral("jitter_buffer_frames"), jitterBufferSpin_->value()},
        {QStringLiteral("jitter_buffer_max_frames"), jitterBufferMaxSpin_->value()},
        {QStringLiteral("adaptive_playback_cushion"), adaptiveCushionCheck_->isChecked()},
        {QStringLiteral("adaptive_target_frames"), adaptiveTargetSpin_->value()},
        {QStringLiteral("adaptive_min_frames"), adaptiveMinSpin_->value()},
        {QStringLiteral("adaptive_max_frames"), adaptiveMaxSpin_->value()},
        {QStringLiteral("adaptive_release_ppm"), adaptiveReleaseSpin_->value()},
    };
}

void MainWindow::applyLeaderSettings(const QJsonObject& settings)
{
    applyTuningProfileName(settings.value(QStringLiteral("profile")).toString(
        profileBox_ ? profileBox_->currentData().toString() : QStringLiteral("fast")));
    sampleRateSpin_->setValue(settings.value(QStringLiteral("sample_rate")).toInt(sampleRateSpin_->value()));
    bufferSizeSpin_->setValue(settings.value(QStringLiteral("audio_buffer_size")).toInt(bufferSizeSpin_->value()));
    frameSizeSpin_->setValue(settings.value(QStringLiteral("frame_size")).toInt(frameSizeSpin_->value()));
    prefillSpin_->setValue(settings.value(QStringLiteral("playback_prefill_frames")).toInt(prefillSpin_->value()));
    playbackMaxSpin_->setValue(settings.value(QStringLiteral("playback_max_frames")).toInt(playbackMaxSpin_->value()));
    captureRingSpin_->setValue(settings.value(QStringLiteral("capture_ring_frames")).toInt(captureRingSpin_->value()));
    playbackRingSpin_->setValue(settings.value(QStringLiteral("playback_ring_frames")).toInt(playbackRingSpin_->value()));
    statsWarmupMsSpin_->setValue(settings.value(QStringLiteral("stats_warmup_ms")).toInt(statsWarmupMsSpin_->value()));
    socketSendBufferSpin_->setValue(settings.value(QStringLiteral("socket_send_buffer")).toInt(socketSendBufferSpin_->value()));
    socketRecvBufferSpin_->setValue(settings.value(QStringLiteral("socket_recv_buffer")).toInt(socketRecvBufferSpin_->value()));
    driftCorrectionCheck_->setChecked(settings.value(QStringLiteral("drift_correction")).toBool(driftCorrectionCheck_->isChecked()));
    driftSmoothingSpin_->setValue(settings.value(QStringLiteral("drift_smoothing")).toDouble(driftSmoothingSpin_->value()));
    driftDeadbandSpin_->setValue(settings.value(QStringLiteral("drift_deadband_ppm")).toInt(driftDeadbandSpin_->value()));
    driftMaxCorrectionSpin_->setValue(settings.value(QStringLiteral("drift_max_correction_ppm")).toInt(driftMaxCorrectionSpin_->value()));
    if (metronomeBpmSpin_) {
        metronomeBpmSpin_->setValue(settings.value(QStringLiteral("bpm")).toInt(metronomeBpmSpin_->value()));
    }
    remoteLevelSlider_->setValue(qBound(-60, qRound(dbFromGain(settings.value(QStringLiteral("remote_level")).toDouble(
        gainFromDb(static_cast<double>(remoteLevelSlider_->value()))))), 12));
    const QString mode = settings.value(QStringLiteral("metronome_mode")).toString(metronomeModeBox_->currentText());
    const int modeIndex = metronomeModeBox_->findText(mode);
    if (modeIndex >= 0) {
        metronomeModeBox_->setCurrentIndex(modeIndex);
    }
    if (metronomeCompensationMaxSpin_) {
        metronomeCompensationMaxSpin_->setValue(settings.value(QStringLiteral("metronome_compensation_max_ms")).toDouble(metronomeCompensationMaxSpin_->value()));
    }
    if (metronomeCompensationSmoothingSpin_) {
        metronomeCompensationSmoothingSpin_->setValue(settings.value(QStringLiteral("metronome_compensation_smoothing_ms")).toDouble(metronomeCompensationSmoothingSpin_->value()));
    }
    if (metronomeCompensationDeadbandSpin_) {
        metronomeCompensationDeadbandSpin_->setValue(settings.value(QStringLiteral("metronome_compensation_deadband_ms")).toDouble(metronomeCompensationDeadbandSpin_->value()));
    }
    if (metronomeCompensationSlewSpin_) {
        metronomeCompensationSlewSpin_->setValue(settings.value(QStringLiteral("metronome_compensation_slew_ms_per_sec")).toDouble(metronomeCompensationSlewSpin_->value()));
    }
    updateMetronomeCompensationVisibility();
    sampleTimePlayoutCheck_->setChecked(settings.value(QStringLiteral("sample_time_playout")).toBool(sampleTimePlayoutCheck_->isChecked()));
    playoutDelaySpin_->setValue(settings.value(QStringLiteral("playout_delay_frames")).toInt(playoutDelaySpin_->value()));
    jitterBufferSpin_->setValue(settings.value(QStringLiteral("jitter_buffer_frames")).toInt(jitterBufferSpin_->value()));
    jitterBufferMaxSpin_->setValue(settings.value(QStringLiteral("jitter_buffer_max_frames")).toInt(jitterBufferMaxSpin_->value()));
    adaptiveCushionCheck_->setChecked(settings.value(QStringLiteral("adaptive_playback_cushion")).toBool(adaptiveCushionCheck_->isChecked()));
    adaptiveTargetSpin_->setValue(settings.value(QStringLiteral("adaptive_target_frames")).toInt(adaptiveTargetSpin_->value()));
    adaptiveMinSpin_->setValue(settings.value(QStringLiteral("adaptive_min_frames")).toInt(adaptiveMinSpin_->value()));
    adaptiveMaxSpin_->setValue(settings.value(QStringLiteral("adaptive_max_frames")).toInt(adaptiveMaxSpin_->value()));
    adaptiveReleaseSpin_->setValue(settings.value(QStringLiteral("adaptive_release_ppm")).toInt(adaptiveReleaseSpin_->value()));
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

QString MainWindow::localMeshEndpoint() const
{
    if (sessionCreator_ && !activePublicEndpoint_.isEmpty()) {
        return activePublicEndpoint_;
    }
    const bool listenMode = sessionCreator_;
    QString host = listenMode && publicHostEdit_ && !publicHostEdit_->text().trimmed().isEmpty()
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

quint64 MainWindow::meshPeerIdForToken(const QString& token) const
{
    bool ok = false;
    const quint64 value = token.left(16).toULongLong(&ok, 16);
    return ok && value != 0 ? value : 1ULL;
}

QStringList MainWindow::meshPeerSpecsExcludingSelf() const
{
    QStringList peers;
    const QString self = meshPeerToken_;
    for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it) {
        if (it.key() == self || it.value().isEmpty()) {
            continue;
        }
        const QString spec = QStringLiteral("%1@%2")
            .arg(meshPeerIdForToken(it.key()))
            .arg(it.value());
        if (!peers.contains(spec)) {
            peers << spec;
        }
    }
    return peers;
}

void MainWindow::handleMeshPeerAuthenticated(const QString& token, const QJsonObject& message)
{
    if (!meshActive_ || token.isEmpty()) {
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
    QString endpoint = message.value(QStringLiteral("udp_endpoint")).toString();
    if (endpoint.isEmpty()) {
        appendLog(QStringLiteral("mesh peer authenticated without UDP endpoint token=") + token.left(8));
        return;
    }
    const int separator = endpoint.lastIndexOf(QLatin1Char(':'));
    const QString host = separator > 0 ? endpoint.left(separator) : endpoint;
    if (host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::") || host == QStringLiteral("[::]")) {
        if (tcpPeerHost.isEmpty()) {
            appendLog(QStringLiteral("mesh peer advertised wildcard UDP endpoint without TCP peer host token=") + token.left(8));
            return;
        }
        endpoint = tcpPeerHost + endpoint.mid(separator);
        appendLog(QStringLiteral("mesh peer wildcard endpoint resolved via TCP host: %1").arg(endpoint));
    }
    if (meshMaxPeersSpin_ && meshMaxPeersSpin_->value() > 0 &&
        !meshPeerEndpoints_.contains(token) &&
        meshPeerEndpoints_.size() >= meshMaxPeersSpin_->value() + 1) {
        controlServer_.sendTo(token, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.error")},
            {QStringLiteral("message"), QStringLiteral("Mesh peer cap reached")},
        }, true);
        appendLog(QStringLiteral("mesh peer rejected by max peer cap"));
        return;
    }
    const bool changed = meshPeerEndpoints_.value(token) != endpoint;
    meshPeerEndpoints_[token] = endpoint;
    updateMixRemotePeers();
    if (changed) {
        appendLog(QStringLiteral("mesh peer endpoint %1 -> %2").arg(token.left(8), endpoint));
        broadcastMeshPeerList();
        restartMeshEngineFromPeerList();
    }
}

void MainWindow::broadcastMeshPeerList()
{
    const QString selfToken = meshPeerToken();
    for (auto observer = meshPeerEndpoints_.cbegin(); observer != meshPeerEndpoints_.cend(); ++observer) {
        if (observer.key() == selfToken) {
            continue;
        }
        const bool observerIsLocal = localMeshPeerTokens_.contains(observer.key());
        QJsonArray peers;
        int index = 1;
        for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it, ++index) {
            QString endpoint = it.value();
            if (observerIsLocal && it.key() == selfToken) {
                endpoint = endpointWithHost(endpoint, QStringLiteral("127.0.0.1"));
            }
            peers.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("peer%1").arg(index)},
                {QStringLiteral("token"), it.key()},
                {QStringLiteral("endpoint"), endpoint},
            });
        }
        controlServer_.sendTo(observer.key(), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.membership")},
            {QStringLiteral("session"), sessionHex()},
            {QStringLiteral("coordinator_token"), meshCoordinatorToken_},
            {QStringLiteral("peers"), peers},
        });
    }
}

void MainWindow::applyMeshPeerList(const QJsonObject& message)
{
    const QString coordinatorToken = message.value(QStringLiteral("coordinator_token")).toString();
    QMap<QString, QString> endpoints;
    const QJsonArray peers = message.value(QStringLiteral("peers")).toArray();
    for (const QJsonValue& value : peers) {
        const QJsonObject peer = value.toObject();
        const QString token = peer.value(QStringLiteral("token")).toString();
        const QString endpoint = peer.value(QStringLiteral("endpoint")).toString();
        if (!token.isEmpty() && !endpoint.isEmpty()) {
            endpoints[token] = endpoint;
        }
    }
    const QString selfToken = meshPeerToken();
    if (coordinatorToken.isEmpty() || coordinatorToken == selfToken ||
        !endpoints.contains(selfToken) || !endpoints.contains(coordinatorToken)) {
        pendingJoinMembershipReady_ = false;
        appendLog(QStringLiteral("rejected incomplete network membership; waiting for coordinator and local peer entries"));
        return;
    }
    meshCoordinatorToken_ = coordinatorToken;
    meshPeerEndpoints_ = std::move(endpoints);
    pendingJoinMembershipReady_ = true;
    updateMixRemotePeers();
    if (pendingJoinLaunch_) {
        launchPendingJoin();
    } else {
        restartMeshEngineFromPeerList();
    }
}

void MainWindow::restartMeshEngineFromPeerList()
{
    if (!meshActive_) {
        return;
    }
    const QString selfToken = meshPeerToken();
    if (!controlServerMode_ &&
        (meshCoordinatorToken_.isEmpty() || meshCoordinatorToken_ == selfToken ||
         !meshPeerEndpoints_.contains(meshCoordinatorToken_))) {
        appendLog(QStringLiteral("network launch deferred until coordinator membership is complete"));
        return;
    }
    const QStringList peers = meshPeerSpecsExcludingSelf();
    updateMixRemotePeers();
    if (jam2_.isRunning() && peers == currentMeshPeers_) {
        return;
    }
    currentMeshPeers_ = peers;
    if (jam2_.isRunning() && jam2_.updatePeers(peers)) {
        appendLog(QStringLiteral("updated embedded network membership without restarting the audio engine"));
        return;
    }
    QStringList args;
    args << QStringLiteral("network") << QStringLiteral("_run")
         << QStringLiteral("--bind") << meshBindEndpoint()
         << QStringLiteral("--session-id") << sessionHex()
         << QStringLiteral("--session-key") << keyHex()
         << QStringLiteral("--bootstrap-role")
         << (controlServerMode_ ? QStringLiteral("creator") : QStringLiteral("joiner"))
         << QStringLiteral("--local-peer-id") << QString::number(meshPeerIdForToken(meshPeerToken()))
         << QStringLiteral("--bootstrap-coordinator-peer-id") << QString::number(meshPeerIdForToken(meshCoordinatorToken_))
         << QStringLiteral("--peers") << peers.join(QLatin1Char(','));
    args << commonJamArgs(false);
    if (controlServerMode_) {
        args << QStringLiteral("--grid-coordinator") << QStringLiteral("on");
    }
    if (jam2_.isRunning()) {
        appendLog(QStringLiteral("embedded membership update unavailable; restarting only the network adapter"));
        meshRestarting_ = true;
        jam2_.stop();
    }
    pendingJoinLaunch_ = false;
    pendingJoinBaseArgs_.clear();
    launchJamProcess(args);
}

void MainWindow::launchPendingJoin()
{
    if (!pendingJoinLaunch_ || jam2_.isRunning()) {
        return;
    }
    if (!pendingJoinSettingsReady_ || !pendingJoinMembershipReady_) {
        return;
    }
    const QString selfToken = meshPeerToken();
    if (meshCoordinatorToken_.isEmpty() || meshCoordinatorToken_ == selfToken ||
        !meshPeerEndpoints_.contains(selfToken) || !meshPeerEndpoints_.contains(meshCoordinatorToken_)) {
        pendingJoinMembershipReady_ = false;
        appendLog(QStringLiteral("network launch deferred because membership does not identify both joiner and coordinator"));
        return;
    }
    if (!selectedDeviceSupportsSampleRate(sampleRateSpin_->value())) {
        const QString message = QStringLiteral("Selected joiner audio device does not support leader sample rate %1").arg(sampleRateSpin_->value());
        appendLog(message);
        sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("session.error")}, {QStringLiteral("message"), message}});
        QMessageBox::warning(this, QStringLiteral("Jam2"), message);
        pendingJoinLaunch_ = false;
        return;
    }
    if (meshActive_) {
        restartMeshEngineFromPeerList();
        return;
    }
    QStringList args = pendingJoinBaseArgs_;
    args << commonJamArgs(false);
    pendingJoinLaunch_ = false;
    pendingJoinBaseArgs_.clear();
    launchJamProcess(args);
}

void MainWindow::sendControl(const QJsonObject& message)
{
    controlServer_.send(message);
    controlClient_.send(message);
}

void MainWindow::updateRuntimeControls()
{
    if (!jam2_.isRunning()) {
        return;
    }
    const double metronomeLevel = gainFromDb(static_cast<double>(metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10));
    const double remoteLevel = gainFromDb(static_cast<double>(remoteLevelSlider_ ? remoteLevelSlider_->value() : 0));
    sendJamCommand(QStringLiteral("metro level %1").arg(metronomeLevel, 0, 'f', 3));
    sendJamCommand(QStringLiteral("remote level %1").arg(remoteLevel, 0, 'f', 3));
    const double sendLevel = gainFromDb(static_cast<double>(mixSendLevelSlider_ ? mixSendLevelSlider_->value() : 0));
    const double monitorLevel = gainFromDb(static_cast<double>(mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : -18));
    sendJamCommand(QStringLiteral("send level %1").arg(sendLevel, 0, 'f', 3));
    sendJamCommand(QStringLiteral("monitor %1").arg(mixMonitorCheck_ && mixMonitorCheck_->isChecked() ? QStringLiteral("on") : QStringLiteral("off")));
    sendJamCommand(QStringLiteral("monitor level %1").arg(monitorLevel, 0, 'f', 3));
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
    const int armedLane = armedRecordBank_ == looperProject_.activeBankIndex() ? armedRecordLane_ : -1;
    int markerRate = preparedSourceSampleRate_;
    if (markerRate <= 0) {
        markerRate = 48000;
    }
    looperStack_->setLanes(
        std::move(views),
        selectedLooperLane_,
        looperProject_.activeBankIndex(),
        armedLane,
        markerRate,
        playbackGrid_.bpm(),
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
    constexpr int maxLanesPerBank = 128;
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
    const QString stagingFolder = transientProjectFolder_;
    auto results = std::make_shared<std::vector<StagedPcm16Asset>>();
    results->reserve(static_cast<std::size_t>(paths.size()));
    (void)startFileWorkerTask(
        [paths, stagingFolder, results] {
            for (const QString& path : paths) {
                results->push_back(stagePcm16Asset(path, stagingFolder));
            }
        },
        [this, bankIndex, results] {
            if (bankIndex < 0 || bankIndex >= looperProject_.banks().size()) {
                return;
            }
            int imported = 0;
            for (const StagedPcm16Asset& asset : *results) {
                if (!asset.error.isEmpty()) {
                    appendLog(QStringLiteral("could not import WAV %1: %2")
                        .arg(asset.sourcePath, asset.error));
                    continue;
                }
                registerTransientTrackWav(asset.stagedPath);
                looperWaveformCache_.remove(asset.stagedPath);
                LooperLane lane;
                lane.assetPath = asset.stagedPath;
                lane.assetHash = asset.sha256;
                lane.name = asset.displayName;
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
            refreshLooperLanes();
            regeneratePreparedMix();
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
    const QString stagingFolder = transientProjectFolder_;
    auto result = std::make_shared<StagedPcm16Asset>();
    (void)startFileWorkerTask(
        [sourcePath, stagingFolder, result] {
            *result = stagePcm16Asset(sourcePath, stagingFolder);
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
    const QString laneFileName = safeFileName(lane.name);
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

    form->addRow(QStringLiteral("Lane"), new QLabel(lane.name, content));
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
    armedRecordBank_ = bankIndex;
    armedRecordLane_ = laneIndex;
    armedRecordMode_ = modeBox->currentData().toString();
    refreshLooperLanes();
    appendLog(QStringLiteral("armed lane recording: bank=%1 lane=%2 mode=%3")
        .arg(looperProject_.banks().at(bankIndex).id)
        .arg(lane.name)
        .arg(armedRecordMode_));
    return true;
}

void MainWindow::startArmedLooperLaneRecording()
{
    if (trackTakeRecordingActive_ || loopbackRecorder_.isRunning()) {
        return;
    }
    if (armedRecordBank_ < 0 || armedRecordLane_ < 0) {
        if (!armSelectedLooperLaneRecording()) {
            return;
        }
    }
    const bool engineInput = armedRecordMode_ != QStringLiteral("loopback");
    if (captureCountInCheck_ && captureCountInCheck_->isChecked()) {
        const bool countInMetronome = captureCountInMetronomeCheck_ && captureCountInMetronomeCheck_->isChecked();
        const bool keepMetronome = captureKeepMetronomeCheck_ && captureKeepMetronomeCheck_->isChecked();
        if (engineInput && countInMetronome) {
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
            stopMetronomeAtRecordingStart_ = countInMetronome && !keepMetronome;
            startInputCapture(0, bars);
        } else {
            startLoopbackCapture();
        }
    } else {
        if (engineInput) {
            startInputCapture(0, 0);
        } else {
            startArmedLooperLaneRecordingNow(0);
        }
    }
}

void MainWindow::startArmedLooperLaneRecordingNow(std::uint64_t targetFrame)
{
    if (armedRecordMode_ == QStringLiteral("loopback")) {
        startLoopbackCapture();
    } else {
        startInputCapture(targetFrame);
    }
}

void MainWindow::importLastCaptureToArmedLane()
{
    if (armedRecordBank_ < 0 || armedRecordLane_ < 0 ||
        armedRecordBank_ >= looperProject_.banks().size() ||
        armedRecordLane_ >= looperProject_.banks().at(armedRecordBank_).lanes.size()) {
        return;
    }
    const int bankIndex = armedRecordBank_;
    const QString laneId = looperProject_.banks().at(bankIndex).lanes.at(armedRecordLane_).id;
    const QString sourcePath = lastCapturePath_;
    const QString stagingFolder = transientProjectFolder_;
    auto result = std::make_shared<StagedPcm16Asset>();
    (void)startFileWorkerTask(
        [sourcePath, stagingFolder, result] {
            *result = stagePcm16Asset(sourcePath, stagingFolder);
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
            if (isDefaultEmptyTrackName(lane.name) || lane.name.trimmed().isEmpty()) {
                lane.name = result->displayName;
            }
            lane.startFrame = 0;
            lane.stopFrame = -1;
            lane.loopStartFrame = -1;
            lane.loopEndFrame = -1;
            lane.loopEnabled = false;
            appendLog(QStringLiteral("recorded lane imported: %1").arg(lane.name));
            armedRecordBank_ = -1;
            armedRecordLane_ = -1;
            armedRecordMode_.clear();
            refreshLooperLanes();
            regeneratePreparedMix();
            publishLocalRecordingOffer(bankIndex, laneId, lane);
            syncLooperArrangement();
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
    if (looperProject_.trackSyncEnabled() && jam2_.isRunning() && controlServerMode_) {
        sendSongSnapshot();
    } else if (looperProject_.trackSyncEnabled() && jam2_.isRunning() && !controlServerMode_) {
        appendLog(QStringLiteral("local arrangement edit kept local; host owns Track Sync revisions"));
    }
}

QString MainWindow::looperAssetAbsolutePath(const LooperLane& lane) const
{
    if (QFileInfo(lane.assetPath).isAbsolute() || projectFolder_.isEmpty()) {
        return lane.assetPath;
    }
    return QDir(projectFolder_).absoluteFilePath(lane.assetPath);
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
    const QString sourceProjectFolder = projectFolder_;
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
    projectFolder_ = result->projectFolder;
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
    const QString projectFolder = projectFolder_;
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
    constexpr int maxFileWorkerTasks = 2;
    if (fileWorkerTasksActive_ >= maxFileWorkerTasks) {
        ++fileWorkerTasksRejected_;
        appendLog(QStringLiteral("file worker saturated: active=%1 capacity=%2 rejected=%3")
            .arg(fileWorkerTasksActive_)
            .arg(maxFileWorkerTasks)
            .arg(fileWorkerTasksRejected_));
        return false;
    }
    ++fileWorkerTasksActive_;
    fileWorkerTasksHighWater_ = qMax(fileWorkerTasksHighWater_, fileWorkerTasksActive_);
    QPointer<MainWindow> self(this);
    auto unexpectedError = std::make_shared<QString>();
    fileWorkerPool_.start(QRunnable::create([
        self,
        work = std::move(work),
        complete = std::move(complete),
        failed = std::move(failed),
        unexpectedError
    ]() mutable {
        try {
            work();
        } catch (const std::exception& error) {
            *unexpectedError = QString::fromUtf8(error.what());
        } catch (...) {
            *unexpectedError = QStringLiteral("unknown worker exception");
        }
        if (self.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(self, [
            self,
            complete = std::move(complete),
            failed = std::move(failed),
            unexpectedError
        ]() mutable {
            if (self.isNull()) {
                return;
            }
            self->fileWorkerTasksActive_ = qMax(0, self->fileWorkerTasksActive_ - 1);
            ++self->fileWorkerTasksCompleted_;
            if (!unexpectedError->isEmpty()) {
                self->appendLog(QStringLiteral("file worker failed: %1 active=%2 high_water=%3 completed=%4 rejected=%5")
                    .arg(*unexpectedError)
                    .arg(self->fileWorkerTasksActive_)
                    .arg(self->fileWorkerTasksHighWater_)
                    .arg(self->fileWorkerTasksCompleted_)
                    .arg(self->fileWorkerTasksRejected_));
                if (failed) {
                    failed(*unexpectedError);
                }
                return;
            }
            complete();
        }, Qt::QueuedConnection);
    }));
    return true;
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
        restartPreparedMixWhenReady_ = false;
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
        if (playPreparedMixWhenReady_) {
            playPreparedMixWhenReady_ = false;
            sendJamCommand(QStringLiteral("track restart"));
            if (gridScheduleLabel_) {
                gridScheduleLabel_->setText(QStringLiteral("Track restart: waiting for engine bar schedule"));
            }
        } else if (restartPreparedMixWhenReady_) {
            restartPreparedMixWhenReady_ = false;
            sendJamCommand(QStringLiteral("track restart"));
        }
    } catch (const std::exception& error) {
        preparedMix_.error = QString::fromUtf8(error.what());
        ++preparedMixFailures_;
        appendLog(QStringLiteral("prepared mix metadata failed: %1 worker_failures=%2")
            .arg(preparedMix_.error)
            .arg(preparedMixFailures_));
        playPreparedMixWhenReady_ = false;
        restartPreparedMixWhenReady_ = false;
    }
}

void MainWindow::loadPreparedMixIntoEngine()
{
    if (!jam2_.isRunning() || preparedMix_.path.isEmpty() || !preparedMix_.error.isEmpty()) {
        return;
    }
    sendJamCommand(QStringLiteral("track load %1").arg(QDir::toNativeSeparators(preparedMix_.path)));
    const auto& model = trackController_.model();
    if (model.loopEnabled) {
        const qint64 loopStartFrame = model.loopStartSeconds >= 0.0
            ? qMax<qint64>(0, static_cast<qint64>(std::llround(model.loopStartSeconds * preparedSourceSampleRate_)))
            : 0;
        const qint64 loopEndFrame = model.loopEndSeconds > model.loopStartSeconds
            ? qMax<qint64>(loopStartFrame + 1, static_cast<qint64>(std::llround(model.loopEndSeconds * preparedSourceSampleRate_)))
            : qMax<qint64>(loopStartFrame + 1, preparedMix_.frames);
        sendJamCommand(QStringLiteral("track loop on %1 %2").arg(loopStartFrame).arg(loopEndFrame));
    } else {
        sendJamCommand(QStringLiteral("track loop off"));
    }
    sendPreparedTrackLevel();
}

void MainWindow::sendPreparedTrackLevel()
{
    const double gain = gainFromDb(trackController_.model().trackGainDb);
    sendJamCommand(QStringLiteral("track level %1").arg(gain, 0, 'f', 6));
}

void MainWindow::updateTrackControls()
{
    const auto& model = trackController_.model();
    trackNameLabel_->setText(QStringLiteral("Track: %1").arg(model.fileName));
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
            trackController_.model().fileName = info.fileName();
            trackController_.model().filePath = info.absoluteFilePath();
            trackController_.model().fileBytes = info.size();
            trackController_.model().sampleRate = sidecar->value(QStringLiteral("sample_rate")).toInt(metadata->sampleRate);
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
    if (trackTakeRecordingActive_) {
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
    pendingTransientCapturePath_ = QFileInfo::exists(output) ? QString{} : output;
    lastCapturePath_ = output;
    lastCaptureSummary_ = QJsonObject{};
    activeTrackTakeId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sendJamCommand(QStringLiteral("track_take arm input %1 %2").arg(activeTrackTakeId_, QDir::toNativeSeparators(output)));
    if (countInBars >= 0) {
        sendJamCommand(QStringLiteral("track_take start_quantized %1 %2").arg(activeTrackTakeId_).arg(countInBars));
    } else {
        sendJamCommand(QStringLiteral("track_take start %1 %2").arg(activeTrackTakeId_).arg(targetFrame));
    }
    trackTakeRecordingActive_ = true;
    const QString startText = countInBars >= 0
        ? QStringLiteral("Recording: armed input take, engine_quantized_count_in_bars=%1 latency_compensation_frames=%2 output=%3")
            .arg(countInBars)
            .arg(recordingAppliedLatencyFrames_)
            .arg(output)
        : QStringLiteral("Recording: armed input take, start_frame=%1 latency_compensation_frames=%2 output=%3")
            .arg(targetFrame)
            .arg(recordingAppliedLatencyFrames_)
            .arg(output);
    if (gridScheduleLabel_) {
        gridScheduleLabel_->setText(startText);
    }
    appendLog(startText);
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
    if (countInBars < 0 && (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked())) {
        QTimer::singleShot(captureDurationSpin_->value() * 1000, this, [this] {
            if (trackTakeRecordingActive_) {
                runGridLockedEngineAction(QStringLiteral("record.stop"), [this](std::uint64_t stopFrame) { stopInputCapture(stopFrame); });
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
    pendingTransientCapturePath_ = QFileInfo::exists(output) ? QString{} : output;
    lastCapturePath_ = output;
    lastCaptureSummary_ = QJsonObject{};
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
                lastCapturePath_ = outputPath;
                if (!ok) {
                    if (!pendingTransientCapturePath_.isEmpty() && QFileInfo::exists(pendingTransientCapturePath_)) {
                        registerTransientTrackWav(pendingTransientCapturePath_);
                    }
                    pendingTransientCapturePath_.clear();
                    if (loadWavButton_) loadWavButton_->setEnabled(true);
                    appendLog(QStringLiteral("loopback recording failed: ") + errorText);
                    return;
                }
                if (!pendingTransientCapturePath_.isEmpty()) {
                    registerTransientTrackWav(pendingTransientCapturePath_);
                }
                pendingTransientCapturePath_.clear();
                if (loadWavButton_) {
                    loadWavButton_->setEnabled(true);
                }
                if (armedRecordBank_ >= 0 && armedRecordLane_ >= 0) {
                    importLastCaptureToArmedLane();
                }
            }, Qt::QueuedConnection);
        }, &error)) {
        if (!pendingTransientCapturePath_.isEmpty() && QFileInfo::exists(pendingTransientCapturePath_)) {
            registerTransientTrackWav(pendingTransientCapturePath_);
        }
        pendingTransientCapturePath_.clear();
        if (loadWavButton_) loadWavButton_->setEnabled(true);
        appendLog(QStringLiteral("loopback recording failed to start: ") + error);
        return;
    }
    if (stopCaptureButton_) stopCaptureButton_->setEnabled(true);
    if (loadWavButton_) loadWavButton_->setEnabled(false);
}

void MainWindow::stopInputCapture(std::uint64_t targetFrame)
{
    if (trackTakeRecordingActive_ && !activeTrackTakeId_.isEmpty()) {
        sendJamCommand(QStringLiteral("track_take stop %1 %2").arg(activeTrackTakeId_).arg(targetFrame));
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
    if (preparedMixWorkerRunning_ || preparedMix_.path.isEmpty() || !preparedMix_.error.isEmpty()) {
        playPreparedMixWhenReady_ = true;
        regeneratePreparedMix();
        return;
    }
    loadPreparedMixIntoEngine();
    sendJamCommand(QStringLiteral("track restart"));
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
    sendJamCommand(QStringLiteral("track stop %1").arg(targetFrame));
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
    if (preparedSourceSampleRate_ <= 0) {
        return 0;
    }
    qint64 sourceFrame = preparedSourceFrame_;
    if (preparedSourcePlaying_) {
        const PlaybackGrid::Position enginePosition = playbackGrid_.position();
        if (enginePosition.engineAnchored &&
            static_cast<qint64>(enginePosition.rawCurrentFrame) >= preparedSourceEngineFrame_) {
            sourceFrame += static_cast<qint64>(enginePosition.rawCurrentFrame) - preparedSourceEngineFrame_;
        }
    }
    const qint64 positionMs = sourceFrame * 1000 / preparedSourceSampleRate_;
    const qint64 durationMs = trackController_.model().durationMs;
    return qBound<qint64>(0, positionMs, durationMs);
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
        localMetronomeRunning_ = true;
        localMetronomeLeader_ = true;
        updateRuntimeControls();
        sendJamCommand(QStringLiteral("metro leader on"));
        sendJamCommand(QStringLiteral("metro on"));
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
    const bool jamMetronomeWasRunning = jam2_.isRunning() && localMetronomeRunning_;
    localMetronomeRunning_ = false;
    localMetronomeLeader_ = false;
    if (jamMetronomeWasRunning) {
        sendJamCommand(QStringLiteral("metro leader off"));
        sendJamCommand(QStringLiteral("metro off"));
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
    if (!jam2_.isRunning() || !metronomeModeBox_) {
        return;
    }
    sendJamCommand(QStringLiteral("metro mode %1").arg(metronomeModeBox_->currentText()));
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
    if (!jam2_.isRunning()) {
        return;
    }
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    sendJamCommand(QStringLiteral("metro pattern %1 %2 %3 0x%4 0x%5 0x%6 0x%7")
        .arg(pattern.bpm)
        .arg(pattern.beats_per_bar)
        .arg(pattern.division)
        .arg(QString::number(static_cast<qulonglong>(pattern.play_mask_low), 16))
        .arg(QString::number(static_cast<qulonglong>(pattern.play_mask_high), 16))
        .arg(QString::number(static_cast<qulonglong>(pattern.accent_mask_low), 16))
        .arg(QString::number(static_cast<qulonglong>(pattern.accent_mask_high), 16)));
}

void MainWindow::sendMetronomeSettingsToPeer()
{
    if (applyingRemoteMetronomeSettings_ || !jam2_.isRunning()) {
        return;
    }
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("metronome.settings")},
        {QStringLiteral("running"), localMetronomeRunning_},
        {QStringLiteral("leader"), localMetronomeLeader_},
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

    const bool running = message.value(QStringLiteral("running")).toBool(localMetronomeRunning_);
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

    applyingRemoteMetronomeSettings_ = true;
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
    localMetronomeRunning_ = running;
    if (!running || remoteLeader) {
        localMetronomeLeader_ = false;
    }
    applyingRemoteMetronomeSettings_ = false;

    if (jam2_.isRunning()) {
        sendJamCommand(localMetronomeLeader_ ? QStringLiteral("metro leader on") : QStringLiteral("metro leader off"));
        sendMetronomeModeToJam();
        sendMetronomePatternToJam();
        sendJamCommand(running ? QStringLiteral("metro on") : QStringLiteral("metro off"));
    }
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

void MainWindow::publishLocalRecordingOffer(
    int bankIndex,
    const QString& targetLaneId,
    const LooperLane& lane)
{
    if (!looperProject_.trackSyncEnabled() || !jam2_.isRunning() || controlServerMode_ ||
        bankIndex < 0 || bankIndex >= looperProject_.banks().size() || targetLaneId.isEmpty() ||
        !isSha256Hex(lane.assetHash) || lane.assetPath.isEmpty()) {
        return;
    }
    while (localRecordingOffers_.size() >= kMaxLooperAssetRequests) {
        localRecordingOffers_.erase(localRecordingOffers_.begin());
    }
    while (recordingOfferAssetPaths_.size() >= kMaxLooperAssetRequests * 2) {
        recordingOfferAssetPaths_.erase(recordingOfferAssetPaths_.begin());
    }
    const QString recordingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject offer{
        {QStringLiteral("type"), QStringLiteral("looper.recording.offer")},
        {QStringLiteral("recording_id"), recordingId},
        {QStringLiteral("bank"), bankIndex},
        {QStringLiteral("target_lane_id"), targetLaneId},
        {QStringLiteral("sha256"), lane.assetHash},
        {QStringLiteral("name"), lane.name.left(512)},
    };
    localRecordingOffers_.insert(recordingId, offer);
    recordingOfferAssetPaths_.insert(lane.assetHash, lane.assetPath);
    sendControl(offer);
    appendLog(QStringLiteral("offered recorded lane for Track Sync: %1 hash=%2")
        .arg(lane.name, lane.assetHash));
}

void MainWindow::handleRecordingOffer(const QJsonObject& message, const QString& sourcePeerToken)
{
    if (!controlServerMode_ || sourcePeerToken.isEmpty()) {
        appendLog(QStringLiteral("rejected recording offer from a non-joiner control path"));
        return;
    }
    const QString recordingId = message.value(QStringLiteral("recording_id")).toString().toLower();
    if (appliedRecordingContributionIds_.contains(recordingId)) {
        return;
    }
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    if (!pendingRecordingContributions_.contains(recordingId)) {
        if (pendingRecordingContributions_.size() >= kMaxLooperAssetRequests) {
            appendLog(QStringLiteral("recording contribution queue is full"));
            return;
        }
        pendingRecordingContributions_.insert(recordingId, PendingRecordingContribution{
            sourcePeerToken,
            message.value(QStringLiteral("bank")).toInt(),
            message.value(QStringLiteral("target_lane_id")).toString(),
            hash,
            message.value(QStringLiteral("name")).toString(),
        });
    }
    if (validatedRecordingAssetHashes_.contains(hash)) {
        applyPendingRecordingContributions();
        return;
    }
    if (!pendingLooperAssetHashes_.contains(hash)) {
        pendingLooperAssetHashes_.append(hash);
    }
    if (!pendingRecordingAssetSources_.contains(hash)) {
        pendingRecordingAssetSources_.insert(hash, sourcePeerToken);
    }
    QJsonArray hashes;
    hashes.append(hash);
    if (!sendControlTo(sourcePeerToken, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.asset.request")},
            {QStringLiteral("hashes"), hashes},
        })) {
        appendLog(QStringLiteral("could not request offered recording asset from peer ") +
            sourcePeerToken.left(8));
    }
}

void MainWindow::applyPendingRecordingContributions()
{
    if (!controlServerMode_) {
        return;
    }
    QStringList completedIds;
    bool arrangementChanged = false;
    for (auto it = pendingRecordingContributions_.constBegin();
         it != pendingRecordingContributions_.constEnd(); ++it) {
        const PendingRecordingContribution& contribution = it.value();
        if (!validatedRecordingAssetHashes_.contains(contribution.assetHash) ||
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
        const bool targetReservedForLocalRecording =
            contribution.bankIndex == armedRecordBank_ && targetIndex == armedRecordLane_;
        bool applied = false;
        if (targetIndex >= 0 && lanes.at(targetIndex).assetHash == contribution.assetHash) {
            applied = true;
        } else if (targetIndex >= 0 && !targetReservedForLocalRecording &&
                   lanes.at(targetIndex).assetHash.isEmpty() && lanes.at(targetIndex).assetPath.isEmpty()) {
            LooperLane& lane = lanes[targetIndex];
            lane.assetPath = assetPath;
            lane.assetHash = contribution.assetHash;
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
            lane.assetPath = assetPath;
            lane.assetHash = contribution.assetHash;
            lane.name = contribution.name.trimmed().isEmpty()
                ? QStringLiteral("Peer recording") : contribution.name;
            if (looperProject_.appendLane(contribution.bankIndex, std::move(lane))) {
                arrangementChanged = true;
                applied = true;
            } else {
                appendLog(QStringLiteral("could not append received recording to bank %1")
                    .arg(contribution.bankIndex + 1));
            }
        }
        if (applied) {
            completedIds.append(it.key());
            appendLog(QStringLiteral("added peer recording to Track Sync: %1 hash=%2")
                .arg(contribution.name, contribution.assetHash));
        }
    }
    for (const QString& recordingId : completedIds) {
        pendingRecordingContributions_.remove(recordingId);
        appliedRecordingContributionIds_.insert(recordingId);
    }
    while (appliedRecordingContributionIds_.size() > kMaxLooperAssetRequests * 2) {
        appliedRecordingContributionIds_.erase(appliedRecordingContributionIds_.begin());
    }
    if (arrangementChanged) {
        refreshLooperLanes();
        regeneratePreparedMix();
        if (looperProject_.trackSyncEnabled() && jam2_.isRunning()) {
            sendSongSnapshot();
        }
    }
}

void MainWindow::handleLooperAssetRequest(const QJsonObject& message, const QString& sourcePeerToken)
{
    if (!looperProject_.trackSyncEnabled()) {
        return;
    }
    const QJsonArray hashes = message.value(QStringLiteral("hashes")).toArray();
    if (hashes.isEmpty() || hashes.size() > kMaxLooperAssetRequests) {
        appendLog(QStringLiteral("rejected looper asset request with invalid hash count"));
        return;
    }
    for (const QJsonValue& value : hashes) {
        const QString hash = value.toString().toLower();
        if (!isSha256Hex(hash)) {
            appendLog(QStringLiteral("rejected looper asset request with invalid hash"));
            return;
        }
        sendLooperAsset(hash, sourcePeerToken);
    }
}

void MainWindow::sendLooperAsset(const QString& hash, const QString& targetPeerToken)
{
    const QPair<QString, QString> request{hash, targetPeerToken};
    if ((outgoingLooperAssetHash_ == hash && outgoingLooperAssetTargetToken_ == targetPeerToken) ||
        (outgoingLooperAssetPendingHash_ == hash && outgoingLooperAssetPendingTargetToken_ == targetPeerToken) ||
        outgoingLooperAssetQueue_.contains(request)) {
        return;
    }
    if (outgoingLooperAssetQueue_.size() >= kMaxLooperAssetRequests) {
        appendLog(QStringLiteral("looper asset send queue is full"));
        return;
    }
    outgoingLooperAssetQueue_.append(request);
    if (!outgoingLooperAssetTimer_.isActive()) {
        outgoingLooperAssetTimer_.start();
    }
    continueLooperAssetSend();
}

bool MainWindow::canQueueControlTo(const QString& targetPeerToken, qint64 estimatedBytes) const
{
    return targetPeerToken.isEmpty()
        ? controlClient_.canQueue(estimatedBytes)
        : controlServer_.canQueueTo(targetPeerToken, estimatedBytes);
}

bool MainWindow::sendControlTo(const QString& targetPeerToken, const QJsonObject& message)
{
    return targetPeerToken.isEmpty()
        ? controlClient_.send(message)
        : controlServer_.sendTo(targetPeerToken, message);
}

void MainWindow::continueLooperAssetSend()
{
    if (!outgoingLooperAssetFile_) {
        if (outgoingLooperAssetValidationPending_) {
            return;
        }
        if (outgoingLooperAssetQueue_.isEmpty()) {
            outgoingLooperAssetTimer_.stop();
            return;
        }
        const QPair<QString, QString> request = outgoingLooperAssetQueue_.takeFirst();
        const QString hash = request.first;
        const QString targetPeerToken = request.second;
        if (!isSha256Hex(hash)) {
            return;
        }

        QString path = recordingOfferAssetPaths_.value(hash);
        for (const LooperBank& bank : looperProject_.banks()) {
            if (!path.isEmpty()) {
                break;
            }
            for (const LooperLane& lane : bank.lanes) {
                if (lane.assetHash == hash) {
                    path = looperAssetAbsolutePath(lane);
                    break;
                }
            }
            if (!path.isEmpty()) {
                break;
            }
        }
        struct ValidationResult {
            qint64 bytes = 0;
            QString error;
        };
        auto validation = std::make_shared<ValidationResult>();
        outgoingLooperAssetValidationPending_ = true;
        outgoingLooperAssetPendingHash_ = hash;
        outgoingLooperAssetPendingTargetToken_ = targetPeerToken;
        const bool started = startFileWorkerTask(
            [path, hash, validation] {
                const QFileInfo workerInfo(path);
                if (path.isEmpty() || !workerInfo.isFile() || workerInfo.size() <= 0 ||
                    workerInfo.size() > kMaxLooperAssetBytes || sha256FileHex(path) != hash) {
                    validation->error = QStringLiteral("asset unavailable, hash-mismatched, or outside transfer bounds");
                    return;
                }
                const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                    nativeFilePath(path), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
                if (!inspected) {
                    validation->error = QStringLiteral("WAV validation failed: ") + QString::fromStdString(inspected.error);
                    return;
                }
                validation->bytes = workerInfo.size();
            },
            [this, path, hash, targetPeerToken, validation] {
                outgoingLooperAssetValidationPending_ = false;
                outgoingLooperAssetPendingHash_.clear();
                outgoingLooperAssetPendingTargetToken_.clear();
                if (!validation->error.isEmpty()) {
                    appendLog(QStringLiteral("looper asset validation failed hash=%1: %2")
                        .arg(hash, validation->error));
                    continueLooperAssetSend();
                    return;
                }
                auto file = std::make_unique<QFile>(path);
                if (!file->open(QIODevice::ReadOnly)) {
                    appendLog(QStringLiteral("could not open looper asset for sync: ") + path);
                    continueLooperAssetSend();
                    return;
                }
                outgoingLooperAssetFile_ = std::move(file);
                outgoingLooperAssetHash_ = hash;
                outgoingLooperAssetTargetToken_ = targetPeerToken;
                outgoingLooperAssetBytes_ = validation->bytes;
                outgoingLooperAssetNextChunk_ = -1;
                outgoingLooperAssetProgress_.start();
                continueLooperAssetSend();
            },
            [this, request](const QString&) {
                outgoingLooperAssetValidationPending_ = false;
                outgoingLooperAssetPendingHash_.clear();
                outgoingLooperAssetPendingTargetToken_.clear();
                outgoingLooperAssetQueue_.prepend(request);
            });
        if (!started) {
            outgoingLooperAssetValidationPending_ = false;
            outgoingLooperAssetPendingHash_.clear();
            outgoingLooperAssetPendingTargetToken_.clear();
            outgoingLooperAssetQueue_.prepend(request);
        }
        return;
    }

    if (!canQueueControlTo(outgoingLooperAssetTargetToken_, kLooperAssetFrameQueueEstimate)) {
        if (outgoingLooperAssetProgress_.isValid() &&
            outgoingLooperAssetProgress_.elapsed() > kLooperAssetProgressDeadlineMs) {
            appendLog(QStringLiteral("looper asset send progress timeout: ") + outgoingLooperAssetHash_);
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            outgoingLooperAssetTargetToken_.clear();
        }
        return;
    }
    if (outgoingLooperAssetNextChunk_ < 0) {
        if (!sendControlTo(outgoingLooperAssetTargetToken_, QJsonObject{
                {QStringLiteral("type"), QStringLiteral("looper.asset.start")},
                {QStringLiteral("sha256"), outgoingLooperAssetHash_},
                {QStringLiteral("file_bytes"), static_cast<double>(outgoingLooperAssetBytes_)},
                {QStringLiteral("chunk_size"), kLooperAssetChunkBytes},
            })) {
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            return;
        }
        outgoingLooperAssetNextChunk_ = 0;
        outgoingLooperAssetProgress_.restart();
        return;
    }

    if (!outgoingLooperAssetFile_->atEnd()) {
        const QByteArray bytes = outgoingLooperAssetFile_->read(kLooperAssetChunkBytes);
        if (bytes.isEmpty()) {
            appendLog(QStringLiteral("failed while reading looper asset for sync: ") + outgoingLooperAssetHash_);
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            return;
        }
        const QJsonObject chunk{
            {QStringLiteral("type"), QStringLiteral("looper.asset.chunk")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("index"), outgoingLooperAssetNextChunk_},
            {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
        };
        if (!sendControlTo(outgoingLooperAssetTargetToken_, chunk)) {
            outgoingLooperAssetFile_.reset();
            outgoingLooperAssetHash_.clear();
            return;
        }
        ++outgoingLooperAssetNextChunk_;
        outgoingLooperAssetProgress_.restart();
        return;
    }

    if (!sendControlTo(outgoingLooperAssetTargetToken_, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("looper.asset.done")},
            {QStringLiteral("sha256"), outgoingLooperAssetHash_},
            {QStringLiteral("chunks"), outgoingLooperAssetNextChunk_},
        })) {
        outgoingLooperAssetFile_.reset();
        outgoingLooperAssetHash_.clear();
        return;
    }
    appendLog(QStringLiteral("sent looper asset: %1 bytes hash=%2")
        .arg(outgoingLooperAssetBytes_)
        .arg(outgoingLooperAssetHash_));
    outgoingLooperAssetFile_.reset();
    outgoingLooperAssetHash_.clear();
    outgoingLooperAssetTargetToken_.clear();
    outgoingLooperAssetBytes_ = 0;
    outgoingLooperAssetNextChunk_ = 0;
    outgoingLooperAssetProgress_.invalidate();
}

void MainWindow::resetIncomingLooperAsset()
{
    incomingLooperAssetTimer_.stop();
    incomingLooperAssetFile_.reset();
    incomingLooperAssetHasher_.reset();
    incomingLooperAssetHash_.clear();
    incomingLooperAssetSourceToken_.clear();
    incomingLooperAssetBytesExpected_ = 0;
    incomingLooperAssetBytesReceived_ = 0;
    incomingLooperAssetChunkSize_ = 0;
    incomingLooperAssetNextChunk_ = 0;
}

void MainWindow::receiveLooperAssetStart(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    const QString expectedSource = pendingRecordingAssetSources_.value(hash);
    const double declaredBytes = message.value(QStringLiteral("file_bytes")).toDouble(-1.0);
    const int chunkSize = message.value(QStringLiteral("chunk_size")).toInt(-1);
    if (incomingLooperAssetFile_ || !isSha256Hex(hash) || !pendingLooperAssetHashes_.contains(hash) ||
        (!expectedSource.isEmpty() && expectedSource != sourcePeerToken) ||
        !std::isfinite(declaredBytes) || std::floor(declaredBytes) != declaredBytes ||
        declaredBytes < 44.0 || declaredBytes > static_cast<double>(kMaxLooperAssetBytes) ||
        chunkSize <= 0 || chunkSize > kLooperAssetChunkBytes) {
        appendLog(QStringLiteral("rejected unsolicited or invalid looper asset start"));
        return;
    }

    const QString output = looperAssetPathForHash(hash);
    QFileInfo outputInfo(output);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        appendLog(QStringLiteral("failed to create looper asset cache folder"));
        return;
    }
    auto file = std::make_unique<QTemporaryFile>(output + QStringLiteral(".partial.XXXXXX"));
    file->setAutoRemove(true);
    if (!file->open()) {
        appendLog(QStringLiteral("failed to create temporary looper asset"));
        return;
    }
    incomingLooperAssetFile_ = std::move(file);
    incomingLooperAssetHasher_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    incomingLooperAssetHash_ = hash;
    incomingLooperAssetSourceToken_ = sourcePeerToken;
    incomingLooperAssetBytesExpected_ = static_cast<qint64>(declaredBytes);
    incomingLooperAssetBytesReceived_ = 0;
    incomingLooperAssetChunkSize_ = chunkSize;
    incomingLooperAssetNextChunk_ = 0;
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
    appendLog(QStringLiteral("receiving looper asset: %1 bytes hash=%2")
        .arg(incomingLooperAssetBytesExpected_)
        .arg(incomingLooperAssetHash_));
}

void MainWindow::receiveLooperAssetChunk(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString hash = message.value(QStringLiteral("sha256")).toString().toLower();
    const int index = message.value(QStringLiteral("index")).toInt(-1);
    const QString encodedText = message.value(QStringLiteral("data")).toString();
    const QByteArray encoded = encodedText.toLatin1();
    static const QRegularExpression base64Expression(QStringLiteral("^[A-Za-z0-9+/]*={0,2}$"));
    const int maxEncodedBytes = ((incomingLooperAssetChunkSize_ + 2) / 3) * 4;
    if (incomingLooperAssetFile_ && sourcePeerToken != incomingLooperAssetSourceToken_) {
        appendLog(QStringLiteral("looper asset chunk rejected from unexpected peer"));
        return;
    }
    if (!incomingLooperAssetFile_ || !incomingLooperAssetHasher_ || hash != incomingLooperAssetHash_ ||
        index != incomingLooperAssetNextChunk_ || encodedText.isEmpty() ||
        encoded.size() > maxEncodedBytes || encoded.size() % 4 != 0 ||
        !base64Expression.match(encodedText).hasMatch()) {
        appendLog(QStringLiteral("looper asset chunk rejected"));
        resetIncomingLooperAsset();
        return;
    }
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    if (decoded.isEmpty() || decoded.size() > incomingLooperAssetChunkSize_ ||
        decoded.toBase64() != encoded ||
        incomingLooperAssetBytesReceived_ > incomingLooperAssetBytesExpected_ - decoded.size() ||
        incomingLooperAssetFile_->write(decoded) != decoded.size()) {
        appendLog(QStringLiteral("looper asset chunk decoding or write failed"));
        resetIncomingLooperAsset();
        return;
    }
    incomingLooperAssetHasher_->addData(decoded);
    incomingLooperAssetBytesReceived_ += decoded.size();
    ++incomingLooperAssetNextChunk_;
    incomingLooperAssetTimer_.start(kLooperAssetProgressDeadlineMs);
}

void MainWindow::receiveLooperAssetDone(const QJsonObject& message, const QString& sourcePeerToken)
{
    const QString expectedHash = message.value(QStringLiteral("sha256")).toString().toLower();
    const int chunks = message.value(QStringLiteral("chunks")).toInt(-1);
    if (incomingLooperAssetFile_ && sourcePeerToken != incomingLooperAssetSourceToken_) {
        appendLog(QStringLiteral("looper asset completion rejected from unexpected peer"));
        return;
    }
    if (!incomingLooperAssetFile_ || !incomingLooperAssetHasher_ || expectedHash != incomingLooperAssetHash_ ||
        chunks != incomingLooperAssetNextChunk_ || incomingLooperAssetBytesExpected_ != incomingLooperAssetBytesReceived_ ||
        QString::fromLatin1(incomingLooperAssetHasher_->result().toHex()) != expectedHash) {
        appendLog(QStringLiteral("received looper asset failed size, sequence, or hash verification"));
        resetIncomingLooperAsset();
        return;
    }
    incomingLooperAssetFile_->flush();
    incomingLooperAssetFile_->close();
    incomingLooperAssetTimer_.stop();
    const QString temporaryPath = incomingLooperAssetFile_->fileName();
    const QString output = looperAssetPathForHash(expectedHash);
    auto validationError = std::make_shared<QString>();
    const bool started = startFileWorkerTask(
        [temporaryPath, output, expectedHash, validationError] {
            const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(
                nativeFilePath(temporaryPath), static_cast<std::uint64_t>(kMaxLooperAssetBytes));
            if (!inspected) {
                *validationError = QStringLiteral("WAV validation failed: ") +
                    QString::fromStdString(inspected.error);
                return;
            }
            if (QFileInfo::exists(output) && sha256FileHex(output) == expectedHash) {
                return;
            }
            QFile source(temporaryPath);
            QSaveFile destination(output);
            if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
                *validationError = QStringLiteral("could not open asset for atomic commit");
                return;
            }
            constexpr qint64 copyBlockBytes = 1024 * 1024;
            while (!source.atEnd()) {
                const QByteArray block = source.read(copyBlockBytes);
                if (block.isEmpty() && source.error() != QFileDevice::NoError) {
                    *validationError = QStringLiteral("failed while reading validated asset");
                    return;
                }
                if (destination.write(block) != block.size()) {
                    *validationError = QStringLiteral("failed while writing validated asset");
                    return;
                }
            }
            if (!destination.commit()) {
                *validationError = QStringLiteral("failed to atomically commit validated asset");
            }
        },
        [this, output, expectedHash, validationError] {
            if (!validationError->isEmpty()) {
                appendLog(QStringLiteral("received looper asset validation failed: ") + *validationError);
                resetIncomingLooperAsset();
                return;
            }
            registerTransientTrackWav(output);
            looperWaveformCache_.remove(output);
            appendLog(QStringLiteral("received looper asset: ") + output);
            pendingLooperAssetHashes_.removeAll(expectedHash);
            if (pendingRecordingAssetSources_.remove(expectedHash) > 0) {
                validatedRecordingAssetHashes_.insert(expectedHash);
            }
            resetIncomingLooperAsset();
            applyPendingRecordingContributions();
            applyPendingSongIfAssetsReady();
        },
        [this](const QString&) { resetIncomingLooperAsset(); });
    if (!started) {
        appendLog(QStringLiteral("received looper asset validation rejected by saturated file worker"));
        resetIncomingLooperAsset();
    }
}

void MainWindow::handleSongSet(const QJsonObject& message)
{
    if (message.value(QStringLiteral("host_authoritative")).toBool(false) &&
        !looperProject_.trackSyncEnabled()) {
        appendLog(QStringLiteral("ignored host arrangement while Track Sync is disabled"));
        return;
    }
    if (controlServerMode_ && message.value(QStringLiteral("host_authoritative")).toBool(false)) {
        appendLog(QStringLiteral("ignored host-authoritative song snapshot while hosting"));
        return;
    }
    const int revision = message.value(QStringLiteral("arrangement_revision")).toInt(
        message.value(QStringLiteral("revision")).toInt(0));
    const bool restartTrack = message.value(QStringLiteral("track_playing")).toBool(false);
    if (message.value(QStringLiteral("host_authoritative")).toBool(false) &&
        revision <= lastAppliedHostArrangementRevision_) {
        appendLog(QStringLiteral("ignored stale host arrangement revision %1").arg(revision));
        return;
    }
    const bool hostAuthoritative = message.value(QStringLiteral("host_authoritative")).toBool(false);
    const QJsonObject song = normalizeLooperAssetPaths(message.value(QStringLiteral("song")).toObject());
    QStringList referencedHashes;
    const QJsonArray banks = song.value(QStringLiteral("looper")).toObject().value(QStringLiteral("banks")).toArray();
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
    const QString assetFolder = QDir(transientProjectFolder_).absoluteFilePath(QStringLiteral("wavs"));
    const std::uint64_t checkRevision = ++songAssetCheckRevision_;
    auto missing = std::make_shared<QStringList>();
    const bool started = startFileWorkerTask(
        [referencedHashes, assetFolder, missing] {
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
                }
            }
        },
        [this, song, revision, restartTrack, hostAuthoritative, checkRevision, missing] {
            if (checkRevision != songAssetCheckRevision_) {
                return;
            }
            if (!missing->isEmpty()) {
                pendingSongSet_ = song;
                pendingSongRevision_ = revision;
                pendingSongTrackRestart_ = restartTrack;
                pendingLooperAssetHashes_ = *missing;
                appendLog(QStringLiteral("requesting %1 looper asset(s) for arrangement revision %2")
                    .arg(missing->size())
                    .arg(revision));
                QJsonArray hashes;
                for (const QString& hash : *missing) {
                    hashes.append(hash);
                }
                sendControl(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("looper.asset.request")},
                    {QStringLiteral("arrangement_revision"), revision},
                    {QStringLiteral("hashes"), hashes},
                });
                return;
            }
            if (projectFolder_.isEmpty()) {
                projectFolder_ = transientProjectFolder_;
            }
            pendingSongSet_ = QJsonObject{};
            pendingSongRevision_ = 0;
            pendingSongTrackRestart_ = false;
            pendingLooperAssetHashes_.clear();
            if (loadSongJson(song)) {
                if (hostAuthoritative) {
                    lastAppliedHostArrangementRevision_ = revision;
                }
                refreshSongViews();
                refreshLooperLanes();
                restartPreparedMixWhenReady_ = restartTrack && jam2_.isRunning();
                regeneratePreparedMix();
                if (restartPreparedMixWhenReady_) {
                    appendLog(QStringLiteral("scheduled synchronized track restart for arrangement revision %1").arg(revision));
                }
            }
        },
        [this, message](const QString&) {
            deferredSongSetMessage_ = message;
            if (!songAssetCheckRetryTimer_.isActive()) {
                songAssetCheckRetryTimer_.start(100);
            }
        });
    if (!started) {
        deferredSongSetMessage_ = message;
        if (!songAssetCheckRetryTimer_.isActive()) {
            songAssetCheckRetryTimer_.start(100);
        }
        appendLog(QStringLiteral("arrangement asset validation deferred by saturated file worker"));
    }
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
    pendingSongSet_ = QJsonObject{};
    pendingSongRevision_ = 0;
    pendingSongTrackRestart_ = false;
    if (projectFolder_.isEmpty()) {
        projectFolder_ = transientProjectFolder_;
    }
    if (loadSongJson(song)) {
        lastAppliedHostArrangementRevision_ = qMax(lastAppliedHostArrangementRevision_, revision);
        refreshSongViews();
        refreshLooperLanes();
        restartPreparedMixWhenReady_ = restartTrack && jam2_.isRunning();
        regeneratePreparedMix();
        if (restartPreparedMixWhenReady_) {
            appendLog(QStringLiteral("scheduled synchronized track restart for received arrangement revision %1").arg(revision));
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
                    QDir(transientProjectFolder_).absoluteFilePath(
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

QString MainWindow::looperAssetPathForHash(const QString& hash) const
{
    if (!isSha256Hex(hash)) {
        return {};
    }
    return QDir(transientProjectFolder_).absoluteFilePath(
        QStringLiteral("wavs/") + hash + QStringLiteral(".wav"));
}

QStringList MainWindow::commonJamArgs(bool includeExtraArgs) const
{
    QStringList args;
    args << QStringLiteral("--audio-device") << selectedDeviceId()
         << QStringLiteral("--profile") << (profileBox_ ? profileBox_->currentData().toString() : QStringLiteral("fast"))
         << QStringLiteral("--sample-rate") << QString::number(sampleRateSpin_->value())
         << QStringLiteral("--audio-buffer-size") << QString::number(bufferSizeSpin_->value())
         << QStringLiteral("--frame-size") << QString::number(frameSizeSpin_->value())
         << QStringLiteral("--os-priority") << (osPriorityBox_ ? osPriorityBox_->currentData().toString() : QStringLiteral("high"))
         << QStringLiteral("--playback-prefill-frames") << QString::number(prefillSpin_->value())
         << QStringLiteral("--capture-ring-frames") << QString::number(captureRingSpin_->value())
         << QStringLiteral("--playback-ring-frames") << QString::number(playbackRingSpin_->value())
         << QStringLiteral("--input-channels") << inputChannelsEdit_->text()
         << QStringLiteral("--output-channels") << outputChannelsEdit_->text()
         << QStringLiteral("--stats") << (statsCheck_->isChecked() ? QStringLiteral("enabled") : QStringLiteral("disabled"))
         << QStringLiteral("--machine-readable-startup") << QStringLiteral("off")
         << QStringLiteral("--status-format") << QStringLiteral("text")
         << QStringLiteral("--metronome") << QStringLiteral("off")
         << QStringLiteral("--bpm") << QString::number(metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value())
         << QStringLiteral("--metronome-level") << QString::number(gainFromDb(static_cast<double>(metronomeLevelSlider_ ? metronomeLevelSlider_->value() : -10)), 'f', 3)
         << QStringLiteral("--remote-level") << QString::number(gainFromDb(static_cast<double>(remoteLevelSlider_ ? remoteLevelSlider_->value() : 0)), 'f', 3)
         << QStringLiteral("--send-level") << QString::number(gainFromDb(static_cast<double>(mixSendLevelSlider_ ? mixSendLevelSlider_->value() : 0)), 'f', 3)
         << QStringLiteral("--local-monitor") << onOff(mixMonitorCheck_ && mixMonitorCheck_->isChecked())
         << QStringLiteral("--local-monitor-level") << QString::number(gainFromDb(static_cast<double>(mixMonitorLevelSlider_ ? mixMonitorLevelSlider_->value() : -18)), 'f', 3)
         << QStringLiteral("--metronome-mode") << metronomeModeBox_->currentText()
         << QStringLiteral("--metronome-compensation-max-ms") << QString::number(metronomeCompensationMaxSpin_ ? metronomeCompensationMaxSpin_->value() : 250.0, 'f', 1)
         << QStringLiteral("--metronome-compensation-smoothing-ms") << QString::number(metronomeCompensationSmoothingSpin_ ? metronomeCompensationSmoothingSpin_->value() : 750.0, 'f', 1)
         << QStringLiteral("--metronome-compensation-deadband-ms") << QString::number(metronomeCompensationDeadbandSpin_ ? metronomeCompensationDeadbandSpin_->value() : 1.0, 'f', 1)
         << QStringLiteral("--metronome-compensation-slew-ms-per-sec") << QString::number(metronomeCompensationSlewSpin_ ? metronomeCompensationSlewSpin_->value() : 40.0, 'f', 1)
         << QStringLiteral("--sample-time-playout") << onOff(sampleTimePlayoutCheck_->isChecked())
         << QStringLiteral("--adaptive-playback-cushion") << onOff(adaptiveCushionCheck_->isChecked())
         << QStringLiteral("--adaptive-playback-release-ppm") << QString::number(adaptiveReleaseSpin_->value())
         << QStringLiteral("--drift-correction") << onOff(driftCorrectionCheck_->isChecked())
         << QStringLiteral("--drift-smoothing") << QString::number(driftSmoothingSpin_->value(), 'f', 3)
         << QStringLiteral("--drift-deadband-ppm") << QString::number(driftDeadbandSpin_->value())
         << QStringLiteral("--drift-max-correction-ppm") << QString::number(driftMaxCorrectionSpin_->value())
         << QStringLiteral("--stream-linger-ms") << QString::number(streamLingerMsSpin_->value());
    if (statsCheck_->isChecked()) {
        args << QStringLiteral("--stats-interval-ms") << QStringLiteral("1000")
             << QStringLiteral("--stats-warmup-ms") << QString::number(statsWarmupMsSpin_->value());
    }
    if (playbackMaxSpin_->value() > 0) {
        args << QStringLiteral("--playback-max-frames") << QString::number(playbackMaxSpin_->value());
    }
    if (waitMsSpin_->value() > 0) {
        args << QStringLiteral("--wait-ms") << QString::number(waitMsSpin_->value());
    }
    if (streamMsSpin_->value() > 0) {
        args << QStringLiteral("--stream-ms") << QString::number(streamMsSpin_->value());
    }
    if (statsCheck_->isChecked() && !logStatsEdit_->text().trimmed().isEmpty()) {
        args << QStringLiteral("--log-stats") << logStatsEdit_->text().trimmed();
    }
    if (socketSendBufferSpin_->value() > 0) {
        args << QStringLiteral("--socket-send-buffer") << QString::number(socketSendBufferSpin_->value());
    }
    if (socketRecvBufferSpin_->value() > 0) {
        args << QStringLiteral("--socket-recv-buffer") << QString::number(socketRecvBufferSpin_->value());
    }
    if (playoutDelaySpin_->value() > 0) {
        args << QStringLiteral("--playout-delay-frames") << QString::number(playoutDelaySpin_->value());
    }
    if (jitterBufferSpin_->value() > 0) {
        args << QStringLiteral("--jitter-buffer-frames") << QString::number(jitterBufferSpin_->value());
    }
    if (jitterBufferSpin_->value() > 0 && jitterBufferMaxSpin_->value() > 0) {
        args << QStringLiteral("--jitter-buffer-max-frames") << QString::number(jitterBufferMaxSpin_->value());
    }
    if (adaptiveTargetSpin_->value() > 0) {
        args << QStringLiteral("--adaptive-playback-target-frames") << QString::number(adaptiveTargetSpin_->value());
    }
    if (adaptiveMinSpin_->value() > 0) {
        args << QStringLiteral("--adaptive-playback-min-frames") << QString::number(adaptiveMinSpin_->value());
    }
    if (adaptiveMaxSpin_->value() > 0) {
        args << QStringLiteral("--adaptive-playback-max-frames") << QString::number(adaptiveMaxSpin_->value());
    }
    if (noStunCheck_->isChecked()) {
        args << QStringLiteral("--no-stun");
    }
    if (includeExtraArgs && !extraArgsEdit_->text().trimmed().isEmpty()) {
        args << QProcess::splitCommand(extraArgsEdit_->text().trimmed());
    }
    return args;
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
        {QStringLiteral("sync_controls"), model.syncControls},
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
    model.syncControls = object.value(QStringLiteral("sync_controls")).toBool(model.syncControls);
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

QJsonObject MainWindow::songToJson() const
{
    QJsonObject root = chordModel_.toJson();
    root.insert(QStringLiteral("independent_views"), true);
    root.insert(QStringLiteral("beat_view"), beatModel_.toJson());
    root.insert(QStringLiteral("lyric_view"), lyricModel_.toJson());
    root.insert(QStringLiteral("looper"), looperProject_.toJson());
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
    looperProject_ = std::move(loadedLooper);
    refreshLooperLanes();
    return true;
}

void MainWindow::updatePlaybackGrid()
{
    const bool jamGrid = jam2_.isRunning();
    if (!jamGrid) {
        const auto pattern = currentMetronomePattern();
        playbackGrid_.setPattern(pattern.bpm, pattern.beats_per_bar, pattern.division);
        playbackGrid_.clearEngine();
    }
    const PlaybackGrid::Position position = playbackGrid_.position();
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
        trackWaveform_->setBpm(playbackGrid_.bpm());
        trackWaveform_->setGridPosition(position.absoluteBeat, markerRunning);
    }
    if (looperStack_) {
        looperStack_->setGridPosition(position.absoluteBeat, markerRunning, playbackGrid_.bpm());
    }
}

void MainWindow::updateRecordingCountdown(const PlaybackGrid::Position& position)
{
    if (!recordingCountdownLabel_ || recordingStartFrame_ == 0 || !trackTakeRecordingActive_) {
        return;
    }
    if (position.rawCurrentFrame < recordingCountdownStartFrame_) {
        recordingCountdownLabel_->setText(QStringLiteral("WAITING FOR NEXT BAR"));
        recordingCountdownLabel_->show();
        return;
    }
    if (position.rawCurrentFrame < recordingStartFrame_) {
        const double beatFrames = position.secondsPerBeat > 0.0 && position.sampleRate > 0
            ? position.secondsPerBeat * static_cast<double>(position.sampleRate)
            : static_cast<double>(qMax(1, position.sampleRate)) * 0.5;
        const quint64 remainingFrames = recordingStartFrame_ - position.rawCurrentFrame;
        const int remainingBeats = qMax(1, static_cast<int>(std::ceil(static_cast<double>(remainingFrames) / beatFrames)));
        recordingCountdownLabel_->setText(QString::number(remainingBeats));
        recordingCountdownLabel_->show();
        return;
    }
    recordingCountdownLabel_->setText(QStringLiteral("RECORDING"));
    recordingCountdownLabel_->show();
    if (stopMetronomeAtRecordingStart_) {
        stopMetronomeAtRecordingStart_ = false;
        stopTrackMetronome();
    }
}

void MainWindow::updateRecordingLatencyDisplay()
{
    if (!recordingLatencyLabel_) {
        return;
    }
    const int sampleRate = qMax(1, recordingLatencySampleRate_);
    const qint64 adjustment = recordingLatencyAdjustmentSpin_
        ? recordingLatencyAdjustmentSpin_->value()
        : 0;
    const auto milliseconds = [sampleRate](quint64 frames) {
        return static_cast<double>(frames) * 1000.0 / static_cast<double>(sampleRate);
    };
    recordingLatencyLabel_->setText(QStringLiteral(
        "Input %1 (%2 ms) | Output %3 (%4 ms) | Manual %5 | Applied %6 (%7 ms)")
        .arg(recordingInputLatencyFrames_)
        .arg(milliseconds(recordingInputLatencyFrames_), 0, 'f', 2)
        .arg(recordingOutputLatencyFrames_)
        .arg(milliseconds(recordingOutputLatencyFrames_), 0, 'f', 2)
        .arg(adjustment >= 0 ? QStringLiteral("+%1").arg(adjustment) : QString::number(adjustment))
        .arg(recordingAppliedLatencyFrames_)
        .arg(milliseconds(recordingAppliedLatencyFrames_), 0, 'f', 2));
}

void MainWindow::runGridLockedEngineAction(const QString& actionName, const std::function<void(std::uint64_t)>& action)
{
    const PlaybackGrid::Position position = playbackGrid_.position();
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
        const std::uint64_t targetBeat = position.absoluteBeat + 1ULL;
        const std::uint64_t targetStep = targetBeat * static_cast<std::uint64_t>(qMax(1, currentMetronomePattern().division));
        const std::uint64_t targetMusicalFrame = position.epochFrame +
            static_cast<std::uint64_t>(std::llround(static_cast<double>(targetBeat) * position.secondsPerBeat * static_cast<double>(position.sampleRate)));
        targetFrame = rawFrameFromMusicalFrame(targetMusicalFrame, position.renderOffsetFrames);
        const double delayMs = targetFrame > position.rawCurrentFrame
            ? (static_cast<double>(targetFrame - position.rawCurrentFrame) * 1000.0 / static_cast<double>(position.sampleRate))
            : 0.0;
        targetText = QStringLiteral(
            "Grid lock: %1 engine target_beat=%2 target_step=%3 target_musical_frame=%4 target_raw_frame=%5 current_musical_frame=%6 current_raw_frame=%7 offset_frames=%8 delay_ms=%9")
            .arg(actionName)
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
    lastCaptureSummary_ = QJsonObject{};
    lastCapturePath_.clear();
    if (trackWaveform_) {
        trackWaveform_->clear();
    }
    updateTrackControls();
    refreshLooperLanes();
    refreshSongViews();
    projectFilePath_.clear();
    projectFolder_.clear();
    savedProjectSnapshot_ = currentProjectSnapshot();
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
            QFile file(path);
            constexpr qint64 maxSongFileBytes = 4LL * 1024LL * 1024LL;
            if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maxSongFileBytes) {
                *error = QStringLiteral("Could not open song file within the 4 MiB limit.");
                return;
            }
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            if (!document.isObject()) {
                *error = QStringLiteral("Invalid Jam2 song JSON.");
                return;
            }
            *root = document.object();
        },
        [this, path, root, error] {
            if (!error->isEmpty() || !loadSongJson(*root)) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Jam2"),
                    error->isEmpty() ? QStringLiteral("Invalid Jam2 song file.") : *error);
                return;
            }
            projectFolder_ = QFileInfo(path).absolutePath();
            projectFilePath_ = QFileInfo(path).absoluteFilePath();
            loadTrackJson(root->value(QStringLiteral("track")).toObject());
            refreshSongViews();
            savedProjectSnapshot_ = currentProjectSnapshot();
        });
}

bool MainWindow::saveSong()
{
    chordModel_.setTitle(songTitleEdit_->text());
    beatModel_.setTitle(songTitleEdit_->text());
    lyricModel_.setTitle(songTitleEdit_->text());
    QString path = projectFilePath_;
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
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
                *saveError = QStringLiteral("Could not atomically write the song file.");
            }
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
    projectFilePath_ = songInfo.absoluteFilePath();
    projectFolder_ = songInfo.absolutePath();
    for (const LooperBank& bank : std::as_const(looperProject_.banks())) {
        for (const LooperLane& lane : bank.lanes) {
            if (!lane.assetPath.trimmed().isEmpty()) {
                const QString savedAsset = QFileInfo(lane.assetPath).isAbsolute()
                    ? lane.assetPath
                    : QDir(projectFolder_).absoluteFilePath(lane.assetPath);
                transientTrackWavs_.remove(QDir::cleanPath(QFileInfo(savedAsset).absoluteFilePath()));
            }
        }
    }
    savedProjectSnapshot_ = currentProjectSnapshot();
    deferredTransientCleanupWavs_.unite(transientTrackWavs_);
    transientTrackWavs_.clear();
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
    return currentProjectSnapshot() != savedProjectSnapshot_;
}

void MainWindow::registerTransientTrackWav(const QString& path)
{
    if (!path.trimmed().isEmpty()) {
        transientTrackWavs_.insert(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    }
}

void MainWindow::cleanupTransientTrackWavs()
{
    QSet<QString> paths = transientTrackWavs_;
    paths.unite(deferredTransientCleanupWavs_);
    const QString workspacePath = transientProjectFolder_;
    transientTrackWavs_.clear();
    deferredTransientCleanupWavs_.clear();
    if (!lastCapturePath_.isEmpty() && !QFileInfo::exists(lastCapturePath_)) {
        lastCapturePath_.clear();
    }
    fileWorkerPool_.start(QRunnable::create([paths, workspacePath] {
        for (const QString& path : paths) {
            const QFileInfo info(path);
            if (info.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0 && info.exists()) {
                (void)QFile::remove(info.absoluteFilePath());
            }
        }
        QDir workspace(workspacePath);
        workspace.rmdir(QStringLiteral("wavs"));
        QDir parent = workspace;
        if (parent.cdUp()) {
            parent.rmdir(workspace.dirName());
        }
    }));
}

void MainWindow::sendSongSnapshot()
{
    const int revision = ++looperArrangementRevision_;
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("arrangement_revision"), revision},
        {QStringLiteral("host_authoritative"), true},
        {QStringLiteral("track_playing"), preparedSourcePlaying_},
        {QStringLiteral("song"), songToJson()},
    });
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
