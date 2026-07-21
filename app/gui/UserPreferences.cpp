#include "UserPreferences.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {

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
    v.trigger = s.value(QStringLiteral("trigger"), v.trigger).toBool();
    v.triggerThresholdDb = s.value(QStringLiteral("trigger_threshold_db"), v.triggerThresholdDb).toDouble();
    v.triggerHoldMs = s.value(QStringLiteral("trigger_hold_ms"), v.triggerHoldMs).toInt();
    v.preRollMs = s.value(QStringLiteral("pre_roll_ms"), v.preRollMs).toInt();
    v.tailThresholdDb = s.value(QStringLiteral("tail_threshold_db"), v.tailThresholdDb).toDouble();
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
    s.setValue(QStringLiteral("trigger"), v.trigger);
    s.setValue(QStringLiteral("trigger_threshold_db"), v.triggerThresholdDb);
    s.setValue(QStringLiteral("trigger_hold_ms"), v.triggerHoldMs);
    s.setValue(QStringLiteral("pre_roll_ms"), v.preRollMs);
    s.setValue(QStringLiteral("tail_threshold_db"), v.tailThresholdDb);
    s.setValue(QStringLiteral("tail_silence_ms"), v.tailSilenceMs);
    s.setValue(QStringLiteral("trim_leading"), v.trimLeading);
    s.setValue(QStringLiteral("trim_trailing"), v.trimTrailing);
}

} // namespace

QString UserPreferencesStore::filePath()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(root).filePath(QStringLiteral("preferences.ini"));
}

UserPreferences UserPreferencesStore::load()
{
    UserPreferences out;
    QSettings settings(filePath(), QSettings::IniFormat);
    const int schemaVersion = settings.value(QStringLiteral("schema_version"), 1).toInt();
    if (schemaVersion < 1 || schemaVersion > UserPreferences::kSchemaVersion) {
        return out;
    }
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
    out.create.bindHost = settings.value(QStringLiteral("bind_host"), out.create.bindHost).toString();
    out.create.port = settings.value(QStringLiteral("port"), out.create.port).toInt();
    out.create.publicHost = settings.value(QStringLiteral("public_host"), out.create.publicHost).toString();
    out.create.stunServer = settings.value(QStringLiteral("stun_server"), out.create.stunServer).toString();
    out.create.stunTimeoutMs = settings.value(QStringLiteral("stun_timeout_ms"), out.create.stunTimeoutMs).toInt();
    out.create.stunRetries = settings.value(QStringLiteral("stun_retries"), out.create.stunRetries).toInt();
    out.create.noStun = settings.value(QStringLiteral("no_stun"), out.create.noStun).toBool();
    out.create.maxPeers = settings.value(QStringLiteral("max_peers"), out.create.maxPeers).toInt();
    out.create.sampleRate = settings.value(QStringLiteral("sample_rate"), out.create.sampleRate).toInt();
    out.create.audioFormat = settings.value(QStringLiteral("audio_format"), out.create.audioFormat).toString();
    out.create.socketSendBuffer = settings.value(QStringLiteral("socket_send_buffer"), out.create.socketSendBuffer).toInt();
    out.create.socketRecvBuffer = settings.value(QStringLiteral("socket_recv_buffer"), out.create.socketRecvBuffer).toInt();
    settings.beginGroup(QStringLiteral("tuning")); loadTuning(settings, out.create.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); loadRuntime(settings, out.create.runtime); settings.endGroup();
    settings.endGroup();
    settings.beginGroup(QStringLiteral("join"));
    out.join.bindHost = settings.value(QStringLiteral("bind_host"), out.join.bindHost).toString();
    out.join.port = settings.value(QStringLiteral("port"), out.join.port).toInt();
    settings.beginGroup(QStringLiteral("tuning")); loadTuning(settings, out.join.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); loadRuntime(settings, out.join.runtime); settings.endGroup();
    settings.endGroup();
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
        settings.endGroup();
    }
    if (out.logging.folder.isEmpty()) {
        out.logging.folder = !out.create.runtime.logStatsFolder.isEmpty()
            ? out.create.runtime.logStatsFolder
            : out.join.runtime.logStatsFolder;
    }
    return out;
}

void UserPreferencesStore::save(const UserPreferences& p)
{
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QSettings settings(filePath(), QSettings::IniFormat);
    settings.clear();
    settings.setValue(QStringLiteral("schema_version"), UserPreferences::kSchemaVersion);
    settings.beginGroup(QStringLiteral("local_audio")); saveAudio(settings, p.localAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("network_audio"));
    saveAudio(settings, p.networkAudio);
    settings.setValue(QStringLiteral("split_by_role"), p.splitNetworkAudioByRole);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("create_jam_audio")); saveAudio(settings, p.createJamAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("join_jam_audio")); saveAudio(settings, p.joinJamAudio); settings.endGroup();
    settings.beginGroup(QStringLiteral("create"));
    settings.setValue(QStringLiteral("bind_host"), p.create.bindHost);
    settings.setValue(QStringLiteral("port"), p.create.port);
    settings.setValue(QStringLiteral("public_host"), p.create.publicHost);
    settings.setValue(QStringLiteral("stun_server"), p.create.stunServer);
    settings.setValue(QStringLiteral("stun_timeout_ms"), p.create.stunTimeoutMs);
    settings.setValue(QStringLiteral("stun_retries"), p.create.stunRetries);
    settings.setValue(QStringLiteral("no_stun"), p.create.noStun);
    settings.setValue(QStringLiteral("max_peers"), p.create.maxPeers);
    settings.setValue(QStringLiteral("sample_rate"), p.create.sampleRate);
    settings.setValue(QStringLiteral("audio_format"), p.create.audioFormat);
    settings.setValue(QStringLiteral("socket_send_buffer"), p.create.socketSendBuffer);
    settings.setValue(QStringLiteral("socket_recv_buffer"), p.create.socketRecvBuffer);
    settings.beginGroup(QStringLiteral("tuning")); saveTuning(settings, p.create.tuning); settings.endGroup();
    settings.beginGroup(QStringLiteral("runtime")); saveRuntime(settings, p.create.runtime); settings.endGroup();
    settings.endGroup();
    settings.beginGroup(QStringLiteral("join"));
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
    settings.endGroup();
    settings.sync();
}
