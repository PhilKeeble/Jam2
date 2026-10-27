#include "UserPreferences.hpp"
#include "../application/ApplicationPaths.hpp"
#include "tuning_profile.hpp"

#include <QDebug>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace {

QString& preferencesFilePathOverride()
{
    static QString path;
    return path;
}

QVector<int> calibrationChannels(const QVariant& value)
{
    QVector<int> channels;
    for (const auto& item : value.toList()) {
        bool ok = false; const int channel = item.toInt(&ok);
        if (!ok || channel < 0 || channel >= 1024 || channels.contains(channel)) return {};
        channels.push_back(channel);
    }
    return channels;
}
QVariantList calibrationChannels(const QVector<int>& channels)
{
    QVariantList values; for (int channel : channels) values.push_back(channel); return values;
}
QVector<RecordingCalibrationProfile> loadCalibrations(QSettings& s)
{
    QVector<RecordingCalibrationProfile> profiles;
    const int count = qMin(128, s.beginReadArray(QStringLiteral("recording_calibrations")));
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i); RecordingCalibrationProfile p;
        p.key.backend = s.value("backend").toString(); p.key.deviceId = s.value("device_id").toString();
        p.key.sampleRate = s.value("sample_rate").toInt(); p.key.bufferFrames = s.value("buffer_frames").toInt();
        p.key.inputChannels = calibrationChannels(s.value("inputs")); p.key.outputChannels = calibrationChannels(s.value("outputs"));
        p.key.physicalInput = s.value("physical_input", -1).toInt();
        const int method = s.value("method", -1).toInt();
        if (method < 0 || method > 1) continue;
        p.method = static_cast<RecordingCalibrationMethod>(method);
        bool adjustmentOk=false, measuredOk=false, reportedOk=false, spreadOk=false, clicksOk=false;
        p.adjustmentFrames = s.value("adjustment_frames").toLongLong(&adjustmentOk);
        p.measuredFrames = s.value("measured_frames").toDouble(&measuredOk);
        p.reportedFrames = s.value("reported_frames").toDouble(&reportedOk);
        p.spreadFrames = s.value("spread_frames").toDouble(&spreadOk);
        p.clicks = s.value("clicks").toInt(&clicksOk);
        p.calibratedAt = s.value("calibrated_at").toString();
        if (adjustmentOk && measuredOk && reportedOk && spreadOk && clicksOk && p.valid()) {
            // Earlier versions saved one profile per measurement input. Retain
            // the latest measurement for each device configuration.
            const auto existing = std::find_if(profiles.begin(), profiles.end(),
                [&](const auto& old) { return old.key.matchesDeviceConfiguration(p.key); });
            if (existing == profiles.end()) profiles.push_back(std::move(p));
            else {
                const auto previousDate = QDateTime::fromString(existing->calibratedAt, Qt::ISODate);
                const auto date = QDateTime::fromString(p.calibratedAt, Qt::ISODate);
                if (!previousDate.isValid() || (date.isValid() && date >= previousDate))
                    *existing = std::move(p);
            }
        }
    }
    s.endArray(); return profiles;
}
void saveCalibrations(QSettings& s, const QVector<RecordingCalibrationProfile>& profiles)
{
    s.remove(QStringLiteral("recording_calibrations"));
    s.beginWriteArray(QStringLiteral("recording_calibrations")); int index = 0;
    for (const auto& p : profiles) {
        if (!p.valid() || index >= 128) continue;
        s.setArrayIndex(index++);
        s.setValue("backend", p.key.backend); s.setValue("device_id", p.key.deviceId);
        s.setValue("sample_rate", p.key.sampleRate); s.setValue("buffer_frames", p.key.bufferFrames);
        s.setValue("inputs", calibrationChannels(p.key.inputChannels)); s.setValue("outputs", calibrationChannels(p.key.outputChannels));
        s.setValue("physical_input", p.key.physicalInput); s.setValue("method", static_cast<int>(p.method));
        s.setValue("adjustment_frames", static_cast<qlonglong>(p.adjustmentFrames));
        s.setValue("measured_frames", p.measuredFrames); s.setValue("reported_frames", p.reportedFrames);
        s.setValue("spread_frames", p.spreadFrames); s.setValue("clicks", p.clicks); s.setValue("calibrated_at", p.calibratedAt);
    }
    s.endArray();
}

void loadAudio(QSettings& settings, AudioDevicePreference& value)
{
    value.backend = settings.value(QStringLiteral("backend"), value.backend).toString();
    value.stableId = settings.value(QStringLiteral("stable_id"), value.stableId).toString();
    value.name = settings.value(QStringLiteral("name"), value.name).toString();
    value.inputChannels = settings.value(QStringLiteral("input_channels"), value.inputChannels).toString();
    value.outputChannels = settings.value(QStringLiteral("output_channels"), value.outputChannels).toString();
    value.sampleRate = settings.value(QStringLiteral("sample_rate"), value.sampleRate).toInt();
    value.bufferSize = settings.value(QStringLiteral("buffer_size"), value.bufferSize).toInt();
}

void saveAudio(QSettings& settings, const AudioDevicePreference& value)
{
    settings.setValue(QStringLiteral("backend"), value.backend);
    settings.setValue(QStringLiteral("stable_id"), value.stableId);
    settings.setValue(QStringLiteral("name"), value.name);
    settings.setValue(QStringLiteral("input_channels"), value.inputChannels);
    settings.setValue(QStringLiteral("output_channels"), value.outputChannels);
    settings.setValue(QStringLiteral("sample_rate"), value.sampleRate);
    settings.setValue(QStringLiteral("buffer_size"), value.bufferSize);
}

void loadTuning(QSettings& s, LocalTuningPreference& v)
{
    v.profile = s.value(QStringLiteral("profile"), v.profile).toString();
    v.bufferSize = s.value(QStringLiteral("buffer_size"), v.bufferSize).toInt();
    v.frameSize = s.value(QStringLiteral("frame_size"), v.frameSize).toInt();
    v.prefillFrames = s.value(QStringLiteral("prefill_frames"), v.prefillFrames).toInt();
    v.playbackMaxFrames = s.value(QStringLiteral("playback_max_frames"), v.playbackMaxFrames).toInt();
    v.captureRingFrames = s.value(QStringLiteral("capture_ring_frames"), v.captureRingFrames).toInt();
    v.playbackRingFrames = s.value(QStringLiteral("playback_ring_frames"), v.playbackRingFrames).toInt();
    v.driftCorrection = s.value(QStringLiteral("drift_correction"), v.driftCorrection).toBool();
    v.driftSmoothing = s.value(QStringLiteral("drift_smoothing"), v.driftSmoothing).toDouble();
    v.driftDeadbandPpm = s.value(QStringLiteral("drift_deadband_ppm"), v.driftDeadbandPpm).toInt();
    v.driftMaxCorrectionPpm = s.value(QStringLiteral("drift_max_correction_ppm"), v.driftMaxCorrectionPpm).toInt();
    v.sampleTimePlayout = s.value(QStringLiteral("sample_time_playout"), v.sampleTimePlayout).toBool();
    v.playoutDelayFrames = s.value(QStringLiteral("playout_delay_frames"), v.playoutDelayFrames).toInt();
    v.jitterBufferFrames = s.value(QStringLiteral("jitter_buffer_frames"), v.jitterBufferFrames).toInt();
    v.jitterBufferMaxFrames = s.value(QStringLiteral("jitter_buffer_max_frames"), v.jitterBufferMaxFrames).toInt();
    v.adaptiveCushion = s.value(QStringLiteral("adaptive_cushion"), v.adaptiveCushion).toBool();
    v.adaptiveTargetFrames = s.value(QStringLiteral("adaptive_target_frames"), v.adaptiveTargetFrames).toInt();
    v.adaptiveMinFrames = s.value(QStringLiteral("adaptive_min_frames"), v.adaptiveMinFrames).toInt();
    v.adaptiveMaxFrames = s.value(QStringLiteral("adaptive_max_frames"), v.adaptiveMaxFrames).toInt();
    v.adaptiveReleasePpm = s.value(QStringLiteral("adaptive_release_ppm"), v.adaptiveReleasePpm).toInt();
    v.adaptiveRatioRampMs = s.value(QStringLiteral("adaptive_ratio_ramp_ms"), v.adaptiveRatioRampMs).toInt();
}

void migrateSchemaFiveFastJitter(LocalTuningPreference& value)
{
    // Schema 5 labelled the old 512-frame jitter preset as "fast". The
    // optimized core preset now uses 64 frames. Migrate only that exact old
    // value so other explicit numeric tuning remains under user control.
    if (value.profile == QStringLiteral("fast") && value.jitterBufferFrames == 512) {
        value.jitterBufferFrames = 64;
    }
}

void migrateLegacyFastPlaybackDefaults(LocalTuningPreference& value)
{
    // Migrate only the exact latency fields from the former Fast preset. A
    // different value in any changed field is an explicit user tuning choice.
    if (value.profile == QStringLiteral("fast") &&
        value.prefillFrames == 256 &&
        value.playoutDelayFrames == 256 &&
        value.jitterBufferFrames == 64 &&
        value.jitterBufferMaxFrames == 1024 &&
        value.adaptiveTargetFrames == 256 &&
        value.adaptiveMinFrames == 256 &&
        value.adaptiveMaxFrames == 1536) {
        value.prefillFrames = 64;
        value.playoutDelayFrames = 64;
        value.jitterBufferMaxFrames = 512;
        value.adaptiveTargetFrames = 64;
        value.adaptiveMinFrames = 64;
        value.adaptiveMaxFrames = 512;
    }
}

void migrateFastPlaybackMaximum(LocalTuningPreference& value)
{
    // Schema 7's current Fast tuple used a 1536-frame playback safety ceiling.
    // Update only that exact maintained tuple; any other numeric combination
    // remains an explicit user tuning choice.
    if (value.profile == QStringLiteral("fast") &&
        value.prefillFrames == 64 &&
        value.playbackMaxFrames == 1536 &&
        value.playoutDelayFrames == 64 &&
        value.jitterBufferFrames == 64 &&
        value.jitterBufferMaxFrames == 512 &&
        value.adaptiveTargetFrames == 64 &&
        value.adaptiveMinFrames == 64 &&
        value.adaptiveMaxFrames == 512) {
        value.playbackMaxFrames = 1024;
    }
}

void migrateProfileTimingDefaults(LocalTuningPreference& value, bool ownsPacketSize)
{
    const auto* create = jam2::find_create_profile(value.profile.toStdString());
    if (create == nullptr) return;
    const auto& profile = *create->local;
    LocalTuningPreference legacy;
    legacy.profile = value.profile;
    // Preserve the physical device callback selection. Migrate only the
    // complete maintained numerical preset, leaving custom tuning intact.
    legacy.bufferSize = value.bufferSize;
    legacy.frameSize = ownsPacketSize ? create->frame_size : value.frameSize;
    legacy.prefillFrames = static_cast<int>(profile.playback_prefill_frames);
    legacy.playbackMaxFrames = static_cast<int>(profile.playback_max_frames);
    legacy.captureRingFrames = static_cast<int>(profile.capture_ring_frames);
    legacy.playbackRingFrames = static_cast<int>(profile.playback_ring_frames);
    legacy.driftCorrection = profile.drift_correction;
    legacy.driftSmoothing = profile.drift_smoothing;
    legacy.driftDeadbandPpm = 25;
    legacy.driftMaxCorrectionPpm = profile.drift_max_correction_ppm;
    legacy.sampleTimePlayout = profile.sample_time_playout;
    legacy.playoutDelayFrames = static_cast<int>(profile.playout_delay_frames);
    legacy.jitterBufferFrames = static_cast<int>(profile.jitter_buffer_frames);
    legacy.jitterBufferMaxFrames = static_cast<int>(profile.jitter_buffer_max_frames);
    legacy.adaptiveCushion = profile.adaptive_playback_cushion;
    legacy.adaptiveTargetFrames = static_cast<int>(profile.adaptive_playback_target_frames);
    legacy.adaptiveMinFrames = static_cast<int>(profile.adaptive_playback_min_frames);
    legacy.adaptiveMaxFrames = static_cast<int>(profile.adaptive_playback_max_frames);
    legacy.adaptiveReleasePpm = profile.adaptive_playback_release_ppm;
    legacy.adaptiveRatioRampMs = profile.adaptive_playback_ratio_ramp_ms;
    if (value.profile == QStringLiteral("fast")) {
        if (ownsPacketSize) legacy.frameSize = 64;
        legacy.prefillFrames = 64;
    }
    if (value == legacy) {
        value.frameSize = create->frame_size;
        value.prefillFrames = static_cast<int>(profile.playback_prefill_frames);
        value.driftDeadbandPpm = profile.drift_deadband_ppm;
    }
}

void migrateFastPacketSizeDefault(LocalTuningPreference& value)
{
    LocalTuningPreference previous;
    previous.frameSize = 128;
    previous.bufferSize = value.bufferSize;
    if (value == previous) value.frameSize = 64;
}

void saveTuning(QSettings& s, const LocalTuningPreference& v)
{
    s.setValue(QStringLiteral("profile"), v.profile);
    s.setValue(QStringLiteral("buffer_size"), v.bufferSize);
    s.setValue(QStringLiteral("frame_size"), v.frameSize);
    s.setValue(QStringLiteral("prefill_frames"), v.prefillFrames);
    s.setValue(QStringLiteral("playback_max_frames"), v.playbackMaxFrames);
    s.setValue(QStringLiteral("capture_ring_frames"), v.captureRingFrames);
    s.setValue(QStringLiteral("playback_ring_frames"), v.playbackRingFrames);
    s.setValue(QStringLiteral("drift_correction"), v.driftCorrection);
    s.setValue(QStringLiteral("drift_smoothing"), v.driftSmoothing);
    s.setValue(QStringLiteral("drift_deadband_ppm"), v.driftDeadbandPpm);
    s.setValue(QStringLiteral("drift_max_correction_ppm"), v.driftMaxCorrectionPpm);
    s.setValue(QStringLiteral("sample_time_playout"), v.sampleTimePlayout);
    s.setValue(QStringLiteral("playout_delay_frames"), v.playoutDelayFrames);
    s.setValue(QStringLiteral("jitter_buffer_frames"), v.jitterBufferFrames);
    s.setValue(QStringLiteral("jitter_buffer_max_frames"), v.jitterBufferMaxFrames);
    s.setValue(QStringLiteral("adaptive_cushion"), v.adaptiveCushion);
    s.setValue(QStringLiteral("adaptive_target_frames"), v.adaptiveTargetFrames);
    s.setValue(QStringLiteral("adaptive_min_frames"), v.adaptiveMinFrames);
    s.setValue(QStringLiteral("adaptive_max_frames"), v.adaptiveMaxFrames);
    s.setValue(QStringLiteral("adaptive_release_ppm"), v.adaptiveReleasePpm);
    s.setValue(QStringLiteral("adaptive_ratio_ramp_ms"), v.adaptiveRatioRampMs);
}

void loadRuntime(QSettings& s, RuntimePreference& v)
{
    v.diagnostics = s.value(QStringLiteral("diagnostics"), v.diagnostics).toBool();
    v.diagnosticsWarmupMs = s.value(QStringLiteral("diagnostics_warmup_ms"), v.diagnosticsWarmupMs).toInt();
    v.logStatsFolder = s.value(QStringLiteral("log_stats_folder"), v.logStatsFolder).toString();
    v.osPriority = s.value(QStringLiteral("os_priority"), v.osPriority).toString();
    if (v.osPriority != QStringLiteral("off") && v.osPriority != QStringLiteral("high")) {
        v.osPriority = QStringLiteral("high");
    }
    v.waitMs = s.value(QStringLiteral("wait_ms"), v.waitMs).toInt();
    v.streamMs = s.value(QStringLiteral("stream_ms"), v.streamMs).toInt();
    v.streamLingerMs = s.value(QStringLiteral("stream_linger_ms"), v.streamLingerMs).toInt();
}

void saveRuntime(QSettings& s, const RuntimePreference& v)
{
    s.setValue(QStringLiteral("diagnostics"), v.diagnostics);
    s.setValue(QStringLiteral("diagnostics_warmup_ms"), v.diagnosticsWarmupMs);
    s.setValue(QStringLiteral("log_stats_folder"), v.logStatsFolder);
    s.setValue(QStringLiteral("os_priority"), v.osPriority);
    s.setValue(QStringLiteral("wait_ms"), v.waitMs);
    s.setValue(QStringLiteral("stream_ms"), v.streamMs);
    s.setValue(QStringLiteral("stream_linger_ms"), v.streamLingerMs);
}

void loadInputRecording(QSettings& s, InputRecordingPreference& v)
{
    v.outputFolder = s.value(QStringLiteral("output_folder"), v.outputFolder).toString();
    v.recordUntilStopped = s.value(QStringLiteral("record_until_stopped"), v.recordUntilStopped).toBool();
    v.durationBars = s.value(QStringLiteral("duration_bars"), v.durationBars).toInt();
    v.countIn = s.value(QStringLiteral("count_in"), v.countIn).toBool();
    v.countInBars = s.value(QStringLiteral("count_in_bars"), v.countInBars).toInt();
    v.countInMetronome = s.value(QStringLiteral("count_in_metronome"), v.countInMetronome).toBool();
    v.keepMetronome = s.value(QStringLiteral("keep_metronome"), v.keepMetronome).toBool();
    v.latencyAdjustmentFrames = s.value(
        QStringLiteral("latency_adjustment_frames"), v.latencyAdjustmentFrames).toInt();
}

void saveInputRecording(QSettings& s, const InputRecordingPreference& v)
{
    s.setValue(QStringLiteral("output_folder"), v.outputFolder);
    s.setValue(QStringLiteral("record_until_stopped"), v.recordUntilStopped);
    s.setValue(QStringLiteral("duration_bars"), v.durationBars);
    s.setValue(QStringLiteral("count_in"), v.countIn);
    s.setValue(QStringLiteral("count_in_bars"), v.countInBars);
    s.setValue(QStringLiteral("count_in_metronome"), v.countInMetronome);
    s.setValue(QStringLiteral("keep_metronome"), v.keepMetronome);
    s.setValue(QStringLiteral("latency_adjustment_frames"), v.latencyAdjustmentFrames);
}

void loadLoopbackRecording(QSettings& s, LoopbackRecordingPreference& v)
{
    v.outputFolder = s.value(QStringLiteral("output_folder"), v.outputFolder).toString();
    v.sourceId = s.value(QStringLiteral("source_id"), v.sourceId).toString();
    v.sourceName = s.value(QStringLiteral("source_name"), v.sourceName).toString();
    v.recordUntilStopped = s.value(QStringLiteral("record_until_stopped"), v.recordUntilStopped).toBool();
    v.durationBars = s.value(QStringLiteral("duration_bars"), v.durationBars).toInt();
    const double legacySilenceThreshold =
        s.value(QStringLiteral("tail_threshold_db"), v.silenceThresholdDb).toDouble();
    v.silenceThresholdDb =
        s.value(QStringLiteral("silence_threshold_db"), legacySilenceThreshold).toDouble();
    v.tailSilenceMs = s.value(QStringLiteral("tail_silence_ms"), v.tailSilenceMs).toInt();
    v.trimLeading = s.value(QStringLiteral("trim_leading"), v.trimLeading).toBool();
    v.trimTrailing = s.value(QStringLiteral("trim_trailing"), v.trimTrailing).toBool();
}

void saveLoopbackRecording(QSettings& s, const LoopbackRecordingPreference& v)
{
    s.setValue(QStringLiteral("output_folder"), v.outputFolder);
    s.setValue(QStringLiteral("source_id"), v.sourceId);
    s.setValue(QStringLiteral("source_name"), v.sourceName);
    s.setValue(QStringLiteral("record_until_stopped"), v.recordUntilStopped);
    s.setValue(QStringLiteral("duration_bars"), v.durationBars);
    s.setValue(QStringLiteral("silence_threshold_db"), v.silenceThresholdDb);
    s.setValue(QStringLiteral("tail_silence_ms"), v.tailSilenceMs);
    s.setValue(QStringLiteral("trim_leading"), v.trimLeading);
    s.setValue(QStringLiteral("trim_trailing"), v.trimTrailing);
}

} // namespace

QString UserPreferencesStore::filePath()
{
    if (!preferencesFilePathOverride().isEmpty()) {
        return preferencesFilePathOverride();
    }
    return appReleaseFilePath(QStringLiteral("settings"), QStringLiteral("preferences.ini"));
}

bool UserPreferencesStore::setFilePathForTesting(const QString& path, QString& error)
{
    error.clear();
    const QFileInfo info(path);
    if (!info.isAbsolute()) {
        error = QStringLiteral("test preferences path must be absolute");
        return false;
    }
    const QString clean = QDir::cleanPath(info.absoluteFilePath());
    const QString parent = QFileInfo(clean).absolutePath();
    if (clean.isEmpty() || parent.isEmpty() || !QDir().mkpath(parent)) {
        error = QStringLiteral("test preferences folder could not be created");
        return false;
    }
    preferencesFilePathOverride() = clean;
    return true;
}

UserPreferences UserPreferencesStore::load()
{
    UserPreferences out;
    const QString path = filePath();
    // Preserve existing users' preferences once, without replacing a portable
    // INI or reading the user's profile when a test has isolated its storage.
    if (preferencesFilePathOverride().isEmpty() && appReleaseRootOverride().isEmpty() &&
        !QFileInfo::exists(path)) {
        const QString legacyPath = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppConfigLocation)).filePath(QStringLiteral("preferences.ini"));
        if (QFileInfo(legacyPath).isFile()) {
            QFile legacy(legacyPath);
            if (!legacy.rename(path)) {
                qWarning().noquote() << "Could not move preferences from" << legacyPath
                    << "to" << path << ':' << legacy.errorString();
            }
        }
    }
    QSettings settings(path, QSettings::IniFormat);
    const int schemaVersion = settings.value(QStringLiteral("schema_version"), 1).toInt();
    if (schemaVersion < 1 || schemaVersion > UserPreferences::kSchemaVersion) {
        return out;
    }
    if (schemaVersion < 10) {
        out.create.tuning.frameSize = out.join.tuning.frameSize = 64;
        out.create.tuning.prefillFrames = out.join.tuning.prefillFrames = 64;
        out.create.tuning.driftDeadbandPpm = out.join.tuning.driftDeadbandPpm = 25;
    }
    settings.beginGroup(QStringLiteral("metronome"));
    out.metronome.sound = qBound(
        0, settings.value(QStringLiteral("sound"), out.metronome.sound).toInt(), 3);
    out.metronome.mode = settings.value(
        QStringLiteral("mode"), out.metronome.mode).toString();
    out.metronome.compensationMaxMs = settings.value(
        QStringLiteral("compensation_max_ms"), out.metronome.compensationMaxMs).toDouble();
    out.metronome.compensationSmoothingMs = settings.value(
        QStringLiteral("compensation_smoothing_ms"), out.metronome.compensationSmoothingMs).toDouble();
    out.metronome.compensationDeadbandMs = settings.value(
        QStringLiteral("compensation_deadband_ms"), out.metronome.compensationDeadbandMs).toDouble();
    out.metronome.compensationSlewMsPerSecond = settings.value(
        QStringLiteral("compensation_slew_ms_per_second"),
        out.metronome.compensationSlewMsPerSecond).toDouble();
    settings.endGroup();
    out.recordingCalibrations = loadCalibrations(settings);
    settings.beginGroup(QStringLiteral("local_audio")); loadAudio(settings, out.localAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("network_audio"));
    loadAudio(settings, out.networkAudio);
    if (schemaVersion >= 3) {
        out.splitNetworkAudioByRole = settings.value(
            QStringLiteral("split_by_role"), out.splitNetworkAudioByRole).toBool();
    }
    settings.endGroup();
    out.createJamAudio = out.networkAudio;
    out.joinJamAudio = out.networkAudio;
    if (schemaVersion >= 3) {
        settings.beginGroup(QStringLiteral("create_jam_audio"));
        loadAudio(settings, out.createJamAudio);
        settings.endGroup();
        settings.beginGroup(QStringLiteral("join_jam_audio"));
        loadAudio(settings, out.joinJamAudio);
        settings.endGroup();
    }
    settings.beginGroup(QStringLiteral("create"));
    out.create.name = settings.value(QStringLiteral("name"),out.create.name).toString();
    out.create.bindHost = settings.value(QStringLiteral("bind_host"), out.create.bindHost).toString();
    out.create.port = settings.value(QStringLiteral("port"), out.create.port).toInt();
    out.create.publicHost = settings.value(QStringLiteral("public_host"), out.create.publicHost).toString();
    out.create.stunServer = settings.value(QStringLiteral("stun_server"), out.create.stunServer).toString();
    out.create.stunTimeoutMs = settings.value(QStringLiteral("stun_timeout_ms"), out.create.stunTimeoutMs).toInt();
    out.create.stunRetries = settings.value(QStringLiteral("stun_retries"), out.create.stunRetries).toInt();
    out.create.noStun = settings.value(QStringLiteral("no_stun"), out.create.noStun).toBool();
    if (settings.contains(QStringLiteral("connection_strategy"))) {
        try { out.create.strategy = jam2::parse_connection_strategy(settings.value(QStringLiteral("connection_strategy")).toString().toStdString()); }
        catch (const std::exception&) { out.create.strategy = jam2::ConnectionStrategy::Jam2Server; }
    } else if (settings.contains(QStringLiteral("no_stun")) || settings.contains(QStringLiteral("stun_server"))) {
        out.create.strategy = out.create.noStun ? jam2::ConnectionStrategy::Direct : jam2::ConnectionStrategy::Stun;
    }
    out.create.noStun = out.create.strategy != jam2::ConnectionStrategy::Stun;
    out.create.maxPeers = settings.value(QStringLiteral("max_peers"), out.create.maxPeers).toInt();
    out.create.sampleRate = settings.value(QStringLiteral("sample_rate"), out.create.sampleRate).toInt();
    out.create.audioFormat = settings.value(QStringLiteral("audio_format"), out.create.audioFormat).toString();
    out.create.socketSendBuffer = settings.value(QStringLiteral("socket_send_buffer"), out.create.socketSendBuffer).toInt();
    out.create.socketRecvBuffer = settings.value(QStringLiteral("socket_recv_buffer"), out.create.socketRecvBuffer).toInt();
    settings.beginGroup(QStringLiteral("tuning")); loadTuning(settings, out.create.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); loadRuntime(settings, out.create.runtime); settings.endGroup();
    settings.endGroup();
    settings.beginGroup(QStringLiteral("join"));
    out.join.name = settings.value(QStringLiteral("name"),out.join.name).toString();
    out.join.bindHost = settings.value(QStringLiteral("bind_host"), out.join.bindHost).toString();
    out.join.port = settings.value(QStringLiteral("port"), out.join.port).toInt();
    settings.beginGroup(QStringLiteral("tuning")); loadTuning(settings, out.join.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); loadRuntime(settings, out.join.runtime); settings.endGroup();
    settings.endGroup();
    if (schemaVersion < 6) {
        migrateSchemaFiveFastJitter(out.create.tuning);
        migrateSchemaFiveFastJitter(out.join.tuning);
    }
    if (schemaVersion < 7) {
        migrateLegacyFastPlaybackDefaults(out.create.tuning);
        migrateLegacyFastPlaybackDefaults(out.join.tuning);
    }
    if (schemaVersion < 8) {
        migrateFastPlaybackMaximum(out.create.tuning);
        migrateFastPlaybackMaximum(out.join.tuning);
    }
    if (schemaVersion < 10) {
        migrateProfileTimingDefaults(out.create.tuning, true);
        migrateProfileTimingDefaults(out.join.tuning, false);
    }
    if (schemaVersion < 11) {
        migrateFastPacketSizeDefault(out.create.tuning);
        migrateFastPacketSizeDefault(out.join.tuning);
    }
    if (schemaVersion >= 2) {
        settings.beginGroup(QStringLiteral("logging"));
        out.logging.folder = settings.value(QStringLiteral("folder"), out.logging.folder).toString();
        settings.endGroup();
        settings.beginGroup(QStringLiteral("recording"));
        out.recording.preferredMode = settings.value(
            QStringLiteral("preferred_mode"), out.recording.preferredMode).toString();
        settings.beginGroup(QStringLiteral("input"));
        loadInputRecording(settings, out.recording.input);
        settings.endGroup();
        settings.beginGroup(QStringLiteral("loopback"));
        loadLoopbackRecording(settings, out.recording.loopback);
        settings.endGroup();
        settings.beginGroup(QStringLiteral("jam"));
        out.recording.jam.promptForName = settings.value(
            QStringLiteral("prompt_for_name"), out.recording.jam.promptForName).toBool();
        out.recording.jam.completionAction = settings.value(
            QStringLiteral("completion_action"),
            out.recording.jam.completionAction).toString();
        if (out.recording.jam.completionAction != QStringLiteral("ask") &&
            out.recording.jam.completionAction != QStringLiteral("import") &&
            out.recording.jam.completionAction != QStringLiteral("notify")) {
            out.recording.jam.completionAction = QStringLiteral("ask");
        }
        out.recording.jam.importMix = settings.value(
            QStringLiteral("import_mix"), out.recording.jam.importMix).toBool();
        out.recording.jam.importMyInput = settings.value(
            QStringLiteral("import_my_input"), out.recording.jam.importMyInput).toBool();
        out.recording.jam.importTheirInput = settings.value(
            QStringLiteral("import_their_input"), out.recording.jam.importTheirInput).toBool();
        out.recording.jam.importInputsMix = settings.value(
            QStringLiteral("import_inputs_mix"), out.recording.jam.importInputsMix).toBool();
        out.recording.jam.importMetronome = settings.value(
            QStringLiteral("import_metronome"), out.recording.jam.importMetronome).toBool();
        settings.endGroup();
        settings.beginGroup(QStringLiteral("track_jam_mix"));
        out.recording.jamMixTrack.includeBackingTrack = settings.value(
            QStringLiteral("include_backing_track"),
            out.recording.jamMixTrack.includeBackingTrack).toBool();
        out.recording.jamMixTrack.includeMetronome = settings.value(
            QStringLiteral("include_metronome"),
            out.recording.jamMixTrack.includeMetronome).toBool();
        settings.endGroup();
        settings.endGroup();
    }
    if (schemaVersion >= 4) {
        settings.beginGroup(QStringLiteral("general"));
        out.general.startupView = settings.value(
            QStringLiteral("startup_view"), out.general.startupView).toString();
        out.general.bpm = qBound(20,
            settings.value(QStringLiteral("bpm"), out.general.bpm).toInt(), 400);
        out.general.meterNumerator = qBound(1, settings.value(
            QStringLiteral("meter_numerator"), out.general.meterNumerator).toInt(), 16);
        out.general.meterDenominator = settings.value(
            QStringLiteral("meter_denominator"), out.general.meterDenominator).toInt();
        out.general.tempoPulseUnits = qBound(1, settings.value(
            QStringLiteral("tempo_pulse_units"), out.general.tempoPulseUnits).toInt(), 3);
        out.general.clickDivision = qBound(1, settings.value(
            QStringLiteral("click_division"), out.general.clickDivision).toInt(), 8);
        out.general.generateIdeaOnStartup = settings.value(
            QStringLiteral("generate_idea_on_startup"),
            out.general.generateIdeaOnStartup).toBool();
        settings.endGroup();

        settings.beginGroup(QStringLiteral("ideas"));
        out.ideas.parts = qBound(0, settings.value(
            QStringLiteral("parts"), out.ideas.parts).toInt(), 2);
        out.ideas.key = qBound(-1, settings.value(
            QStringLiteral("key"), out.ideas.key).toInt(), 11);
        out.ideas.styleId = settings.value(
            QStringLiteral("style_id"), out.ideas.styleId).toString();
        out.ideas.profileId = settings.value(
            QStringLiteral("profile_id"), out.ideas.profileId).toString();
        out.ideas.meterId = settings.value(
            QStringLiteral("meter_id"), out.ideas.meterId).toString();
        out.ideas.bars = qMax(0, settings.value(
            QStringLiteral("bars"), out.ideas.bars).toInt());
        out.ideas.exactBpm = settings.value(
            QStringLiteral("exact_bpm"), out.ideas.exactBpm).toBool();
        out.ideas.bpm = qBound(20, settings.value(
            QStringLiteral("bpm"), out.ideas.bpm).toInt(), 400);
        out.ideas.complexity = qBound(1, settings.value(
            QStringLiteral("complexity"), out.ideas.complexity).toInt(), 8);
        out.ideas.renderWavsOnStartup = settings.value(
            QStringLiteral("render_wavs_on_startup"),
            out.ideas.renderWavsOnStartup).toBool();
        out.ideas.renderChords = settings.value(
            QStringLiteral("render_chords"), out.ideas.renderChords).toBool();
        out.ideas.renderDrums = settings.value(
            QStringLiteral("render_drums"), out.ideas.renderDrums).toBool();
        out.ideas.renderMelody = settings.value(
            QStringLiteral("render_melody"), out.ideas.renderMelody).toBool();
        out.ideas.renderBass = settings.value(
            QStringLiteral("render_bass"), out.ideas.renderBass).toBool();
        out.ideas.renderSupport = settings.value(
            QStringLiteral("render_support"), out.ideas.renderSupport).toBool();
        out.ideas.chordVoicing = qBound(0, settings.value(
            QStringLiteral("chord_voicing"), out.ideas.chordVoicing).toInt(), 3);
        out.ideas.drumKit = qBound(0, settings.value(
            QStringLiteral("drum_kit"), out.ideas.drumKit).toInt(), 2);
        out.ideas.grooveUseIdeaTiming = settings.value(
            QStringLiteral("groove_use_idea_timing"),
            out.ideas.grooveUseIdeaTiming).toBool();
        out.ideas.grooveBars = qMax(0, settings.value(
            QStringLiteral("groove_bars"), out.ideas.grooveBars).toInt());
        settings.endGroup();

        settings.beginGroup(QStringLiteral("levels"));
        out.levels.sendDb = qBound(-60, settings.value(
            QStringLiteral("send_db"), out.levels.sendDb).toInt(), 12);
        out.levels.monitorInput = settings.value(
            QStringLiteral("monitor_input"), out.levels.monitorInput).toBool();
        out.levels.monitorDb = qBound(-60, settings.value(
            QStringLiteral("monitor_db"), out.levels.monitorDb).toInt(), 12);
        out.levels.metronomeDb = qBound(-60, settings.value(
            QStringLiteral("metronome_db"), out.levels.metronomeDb).toInt(), 12);
        out.levels.masterDb = qBound(-60, settings.value(
            QStringLiteral("master_db"), out.levels.masterDb).toInt(), 12);
        out.levels.remotePeerDb = qBound(-60, settings.value(
            QStringLiteral("remote_peer_db"), out.levels.remotePeerDb).toInt(), 12);
        out.levels.backingTrackDb = qBound(-60, settings.value(
            QStringLiteral("backing_track_db"), out.levels.backingTrackDb).toInt(), 12);
        settings.endGroup();

        settings.beginGroup(QStringLiteral("views"));
        out.views.performanceChordPreview = settings.value(
            QStringLiteral("performance_chord_preview"),
            out.views.performanceChordPreview).toBool();
        out.views.performanceBeatPreview = settings.value(
            QStringLiteral("performance_beat_preview"),
            out.views.performanceBeatPreview).toBool();
        out.views.chordFocusCurrentBar = settings.value(
            QStringLiteral("chord_focus_current_bar"), out.views.chordFocusCurrentBar).toBool();
        out.views.drumFocusCurrentBar = settings.value(
            QStringLiteral("drum_focus_current_bar"), out.views.drumFocusCurrentBar).toBool();
        out.views.guitarStrings = qBound(6, settings.value(
            QStringLiteral("guitar_strings"), out.views.guitarStrings).toInt(), 8);
        out.views.guitarDropTuning = settings.value(
            QStringLiteral("guitar_drop_tuning"), out.views.guitarDropTuning).toBool();
        out.views.trackGridLock = settings.value(
            QStringLiteral("track_grid_lock"), out.views.trackGridLock).toBool();
        out.views.trackLoop = settings.value(
            QStringLiteral("track_loop"), out.views.trackLoop).toBool();
        out.views.trackSpeed = qBound(0.1, settings.value(
            QStringLiteral("track_speed"), out.views.trackSpeed).toDouble(), 2.0);
        out.views.trackPitch = qBound(-12, settings.value(
            QStringLiteral("track_pitch"), out.views.trackPitch).toInt(), 12);
        out.views.focusFrequencyEnabled = settings.value(
            QStringLiteral("focus_frequency_enabled"),
            out.views.focusFrequencyEnabled).toBool();
        out.views.focusPreset = settings.value(
            QStringLiteral("focus_preset"), out.views.focusPreset).toString();
        out.views.focusFrequencyHz = qBound(40, settings.value(
            QStringLiteral("focus_frequency_hz"), out.views.focusFrequencyHz).toInt(), 8000);
        settings.endGroup();

        settings.beginGroup(QStringLiteral("sync"));
        out.sync.trackLanes = settings.value(
            QStringLiteral("track_lanes"), out.sync.trackLanes).toBool();
        out.sync.autoShareWavs = settings.value(
            QStringLiteral("auto_share_wavs"), out.sync.autoShareWavs).toBool();
        out.sync.globalPlayback = settings.value(
            QStringLiteral("global_playback"), out.sync.globalPlayback).toBool();
        out.sync.listeningTools = settings.value(QStringLiteral("listening_tools"), out.sync.listeningTools).toBool();
        out.sync.generatedIdeas = qBound(0, settings.value(
            QStringLiteral("generated_ideas"), out.sync.generatedIdeas).toInt(), 3);
        out.sync.metronomeState = settings.value(
            QStringLiteral("metronome_state"), out.sync.metronomeState).toBool();
        out.sync.recordings = settings.value(
            QStringLiteral("recordings"), out.sync.recordings).toBool();
        settings.endGroup();
    }
    if (out.logging.folder.isEmpty()) {
        out.logging.folder = !out.create.runtime.logStatsFolder.isEmpty()
            ? out.create.runtime.logStatsFolder
            : out.join.runtime.logStatsFolder;
    }
    return out;
}

bool UserPreferencesStore::saveChecked(const UserPreferences& p)
{
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QSettings settings(filePath(), QSettings::IniFormat);
    settings.clear();
    settings.setValue(QStringLiteral("schema_version"), UserPreferences::kSchemaVersion);
    settings.beginGroup(QStringLiteral("metronome"));
    settings.setValue(QStringLiteral("sound"), qBound(0, p.metronome.sound, 3));
    settings.setValue(QStringLiteral("mode"), p.metronome.mode);
    settings.setValue(QStringLiteral("compensation_max_ms"), p.metronome.compensationMaxMs);
    settings.setValue(QStringLiteral("compensation_smoothing_ms"), p.metronome.compensationSmoothingMs);
    settings.setValue(QStringLiteral("compensation_deadband_ms"), p.metronome.compensationDeadbandMs);
    settings.setValue(QStringLiteral("compensation_slew_ms_per_second"),
        p.metronome.compensationSlewMsPerSecond);
    settings.endGroup();
    saveCalibrations(settings, p.recordingCalibrations);
    settings.beginGroup(QStringLiteral("local_audio")); saveAudio(settings, p.localAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("network_audio"));
    saveAudio(settings, p.networkAudio);
    settings.setValue(QStringLiteral("split_by_role"), p.splitNetworkAudioByRole);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("create_jam_audio")); saveAudio(settings, p.createJamAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("join_jam_audio")); saveAudio(settings, p.joinJamAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("create"));
    settings.setValue(QStringLiteral("name"),p.create.name);
    settings.setValue(QStringLiteral("bind_host"), p.create.bindHost);
    settings.setValue(QStringLiteral("port"), p.create.port);
    settings.setValue(QStringLiteral("public_host"), p.create.publicHost);
    settings.setValue(QStringLiteral("stun_server"), p.create.stunServer);
    settings.setValue(QStringLiteral("stun_timeout_ms"), p.create.stunTimeoutMs);
    settings.setValue(QStringLiteral("stun_retries"), p.create.stunRetries);
    settings.setValue(QStringLiteral("no_stun"), p.create.strategy != jam2::ConnectionStrategy::Stun);
    settings.setValue(QStringLiteral("connection_strategy"), QString::fromLatin1(jam2::connection_strategy_text(p.create.strategy).data()));
    settings.setValue(QStringLiteral("max_peers"), p.create.maxPeers);
    settings.setValue(QStringLiteral("sample_rate"), p.create.sampleRate);
    settings.setValue(QStringLiteral("audio_format"), p.create.audioFormat);
    settings.setValue(QStringLiteral("socket_send_buffer"), p.create.socketSendBuffer);
    settings.setValue(QStringLiteral("socket_recv_buffer"), p.create.socketRecvBuffer);
    settings.beginGroup(QStringLiteral("tuning")); saveTuning(settings, p.create.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); saveRuntime(settings, p.create.runtime); settings.endGroup();
    settings.endGroup();
    settings.beginGroup(QStringLiteral("join"));
    settings.setValue(QStringLiteral("name"),p.join.name);
    settings.setValue(QStringLiteral("bind_host"), p.join.bindHost);
    settings.setValue(QStringLiteral("port"), p.join.port);
    settings.beginGroup(QStringLiteral("tuning")); saveTuning(settings, p.join.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); saveRuntime(settings, p.join.runtime); settings.endGroup();
    settings.endGroup();
    settings.beginGroup(QStringLiteral("logging"));
    settings.setValue(QStringLiteral("folder"), p.logging.folder);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("recording"));
    settings.setValue(QStringLiteral("preferred_mode"), p.recording.preferredMode);
    settings.beginGroup(QStringLiteral("input"));
    saveInputRecording(settings, p.recording.input);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("loopback"));
    saveLoopbackRecording(settings, p.recording.loopback);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("jam"));
    settings.setValue(QStringLiteral("prompt_for_name"), p.recording.jam.promptForName);
    settings.setValue(QStringLiteral("completion_action"), p.recording.jam.completionAction);
    settings.setValue(QStringLiteral("import_mix"), p.recording.jam.importMix);
    settings.setValue(QStringLiteral("import_my_input"), p.recording.jam.importMyInput);
    settings.setValue(QStringLiteral("import_their_input"), p.recording.jam.importTheirInput);
    settings.setValue(QStringLiteral("import_inputs_mix"), p.recording.jam.importInputsMix);
    settings.setValue(QStringLiteral("import_metronome"), p.recording.jam.importMetronome);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("track_jam_mix"));
    settings.setValue(QStringLiteral("include_backing_track"),
        p.recording.jamMixTrack.includeBackingTrack);
    settings.setValue(QStringLiteral("include_metronome"),
        p.recording.jamMixTrack.includeMetronome);
    settings.endGroup();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("general"));
    settings.setValue(QStringLiteral("startup_view"), p.general.startupView);
    settings.setValue(QStringLiteral("bpm"), p.general.bpm);
    settings.setValue(QStringLiteral("meter_numerator"), p.general.meterNumerator);
    settings.setValue(QStringLiteral("meter_denominator"), p.general.meterDenominator);
    settings.setValue(QStringLiteral("tempo_pulse_units"), p.general.tempoPulseUnits);
    settings.setValue(QStringLiteral("click_division"), p.general.clickDivision);
    settings.setValue(QStringLiteral("generate_idea_on_startup"), p.general.generateIdeaOnStartup);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("ideas"));
    settings.setValue(QStringLiteral("parts"), p.ideas.parts);
    settings.setValue(QStringLiteral("key"), p.ideas.key);
    settings.setValue(QStringLiteral("style_id"), p.ideas.styleId);
    settings.setValue(QStringLiteral("profile_id"), p.ideas.profileId);
    settings.setValue(QStringLiteral("meter_id"), p.ideas.meterId);
    settings.setValue(QStringLiteral("bars"), p.ideas.bars);
    settings.setValue(QStringLiteral("exact_bpm"), p.ideas.exactBpm);
    settings.setValue(QStringLiteral("bpm"), p.ideas.bpm);
    settings.setValue(QStringLiteral("complexity"), p.ideas.complexity);
    settings.setValue(QStringLiteral("render_wavs_on_startup"), p.ideas.renderWavsOnStartup);
    settings.setValue(QStringLiteral("render_chords"), p.ideas.renderChords);
    settings.setValue(QStringLiteral("render_drums"), p.ideas.renderDrums);
    settings.setValue(QStringLiteral("render_melody"), p.ideas.renderMelody);
    settings.setValue(QStringLiteral("render_bass"), p.ideas.renderBass);
    settings.setValue(QStringLiteral("render_support"), p.ideas.renderSupport);
    settings.setValue(QStringLiteral("chord_voicing"), p.ideas.chordVoicing);
    settings.setValue(QStringLiteral("drum_kit"), p.ideas.drumKit);
    settings.setValue(QStringLiteral("groove_use_idea_timing"), p.ideas.grooveUseIdeaTiming);
    settings.setValue(QStringLiteral("groove_bars"), p.ideas.grooveBars);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("levels"));
    settings.setValue(QStringLiteral("send_db"), p.levels.sendDb);
    settings.setValue(QStringLiteral("monitor_input"), p.levels.monitorInput);
    settings.setValue(QStringLiteral("monitor_db"), p.levels.monitorDb);
    settings.setValue(QStringLiteral("metronome_db"), p.levels.metronomeDb);
    settings.setValue(QStringLiteral("master_db"), p.levels.masterDb);
    settings.setValue(QStringLiteral("remote_peer_db"), p.levels.remotePeerDb);
    settings.setValue(QStringLiteral("backing_track_db"), p.levels.backingTrackDb);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("views"));
    settings.setValue(QStringLiteral("performance_chord_preview"),
        p.views.performanceChordPreview);
    settings.setValue(QStringLiteral("performance_beat_preview"),
        p.views.performanceBeatPreview);
    settings.setValue(QStringLiteral("chord_focus_current_bar"), p.views.chordFocusCurrentBar);
    settings.setValue(QStringLiteral("drum_focus_current_bar"), p.views.drumFocusCurrentBar);
    settings.setValue(QStringLiteral("guitar_strings"), p.views.guitarStrings);
    settings.setValue(QStringLiteral("guitar_drop_tuning"), p.views.guitarDropTuning);
    settings.setValue(QStringLiteral("track_grid_lock"), p.views.trackGridLock);
    settings.setValue(QStringLiteral("track_loop"), p.views.trackLoop);
    settings.setValue(QStringLiteral("track_speed"), p.views.trackSpeed);
    settings.setValue(QStringLiteral("track_pitch"), p.views.trackPitch);
    settings.setValue(QStringLiteral("focus_frequency_enabled"), p.views.focusFrequencyEnabled);
    settings.setValue(QStringLiteral("focus_preset"), p.views.focusPreset);
    settings.setValue(QStringLiteral("focus_frequency_hz"), p.views.focusFrequencyHz);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("sync"));
    settings.setValue(QStringLiteral("track_lanes"), p.sync.trackLanes);
    settings.setValue(QStringLiteral("auto_share_wavs"), p.sync.autoShareWavs);
    settings.setValue(QStringLiteral("global_playback"), p.sync.globalPlayback);
    settings.setValue(QStringLiteral("listening_tools"), p.sync.listeningTools);
    settings.setValue(QStringLiteral("generated_ideas"), p.sync.generatedIdeas);
    settings.setValue(QStringLiteral("metronome_state"), p.sync.metronomeState);
    settings.setValue(QStringLiteral("recordings"), p.sync.recordings);
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void UserPreferencesStore::save(const UserPreferences& preferences)
{
    (void)saveChecked(preferences);
}
