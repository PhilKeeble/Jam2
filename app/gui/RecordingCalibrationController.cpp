#include "RecordingCalibrationController.hpp"
#include "RecordingCalibrationDialog.hpp"
#include <QDateTime>
#include <algorithm>
#include <utility>

RecordingCalibrationController::RecordingCalibrationController(UserPreferences& p, Backend b)
    : preferences_(p), backend_(std::move(b)) {}
RecordingCalibrationKey RecordingCalibrationController::key(std::size_t channel) const
{
    const auto s = backend_.stream(); RecordingCalibrationKey k;
    k.backend = QString::fromStdString(s.device.backend);
    k.deviceId = QString::fromStdString(jam2::audio::stable_device_id(s.device));
    if (s.device.backend == "WASAPI") {
        k.inputDeviceId = QString::fromStdString(s.wasapi.input_endpoint_id);
        k.inputPeriodFrames = static_cast<int>(s.input_period_frames);
        k.inputMixRate = static_cast<int>(s.input_mix_sample_rate);
        k.outputMixRate = static_cast<int>(s.output_mix_sample_rate);
        k.captureTargetMs = s.wasapi.capture_target_ms;
        k.lowLatencyShared = s.low_latency_shared;
    }
    if (s.device.backend == "CoreAudio" && s.coreaudio.setup == jam2::audio::AudioDeviceSetup::SeparateInputOutput)
        k.inputDeviceId = QString::fromStdString(s.coreaudio.input_device_uid);
    if (std::isfinite(s.sample_rate) && s.sample_rate >= 8000 && s.sample_rate <= 384000)
        k.sampleRate = static_cast<int>(std::llround(s.sample_rate));
    if (s.buffer_size <= 65536) k.bufferFrames = static_cast<int>(s.buffer_size);
    for (int c : s.channels.input) k.inputChannels.push_back(c);
    for (int c : s.channels.output) k.outputChannels.push_back(c);
    if (channel < s.channels.input.size()) k.physicalInput = s.channels.input[channel];
    return k;
}
std::optional<std::size_t> RecordingCalibrationController::channelForSource(std::optional<std::size_t> slot) const
{
    for (std::size_t i = 0; i < jam2::audio::kMaximumInputSources; ++i) {
        if (slot && i != *slot) continue;
        const auto s = backend_.source(i);
        if (!s.configured || (!slot && !s.enabled)) continue;
        if (s.kind == jam2::audio::InputSourceKind::Audio &&
            s.first_channel < backend_.stream().channels.input.size()) return s.first_channel;
    }
    // The combined send can measure any physical input on the active device,
    // even if that input is not currently included in the send.
    if (!slot && !backend_.stream().channels.input.empty()) return 0;
    return {};
}
QString RecordingCalibrationController::status(std::optional<std::size_t> slot) const
{
    const auto channel = channelForSource(slot);
    if (!channel) return QStringLiteral("Device input calibration is not used for a MIDI-only recording. Select an audio input or the combined send mix.");
    const auto k = key(*channel);
    for (const auto& p : preferences_.recordingCalibrations) if (p.valid() && p.key.matchesDeviceConfiguration(k))
        return QStringLiteral("Device calibrated - %1 Hz / %2 frames - Adjustment %3 ms (all physical inputs)")
            .arg(k.sampleRate).arg(k.bufferFrames)
            .arg(p.adjustmentFrames * 1000. / k.sampleRate, 0, 'f', 2);
    return QStringLiteral("No calibration for this device and audio configuration. Using reported device timing.");
}
QString RecordingCalibrationController::unavailableReason() const
{
    const auto s = backend_.snapshot();
    if (!s.frame_clock_active || s.backend == jam2::EngineAudioBackend::Headless)
        return QStringLiteral("Start a physical audio device first.");
    if (backend_.networkRunning()) return QStringLiteral("Disconnect the jam before running calibration.");
    if (s.transport_playback_active || s.transport_pending || s.prepared_source_playing ||
        s.track_take.armed || s.jam_recording.active)
        return QStringLiteral("Stop playback and recording before running calibration.");
    return {};
}
bool RecordingCalibrationController::showDialog(QWidget* parent, std::optional<std::size_t> slot)
{
    const auto channel = channelForSource(slot);
    if (!channel) return false;
    RecordingCalibrationDialog dialog(*this, *channel, parent);
    return dialog.exec() == QDialog::Accepted;
}
std::shared_ptr<jam2::audio::RecordingCalibrationCapture> RecordingCalibrationController::begin(std::size_t channel, QString& error)
{
    error = unavailableReason();
    if (!error.isEmpty()) return {};
    if (!key(channel).valid()) { error = QStringLiteral("The active device identity or channel configuration is unavailable."); return {}; }
    const auto snapshot = backend_.snapshot();
    monitorBefore_ = snapshot.local_monitor_enabled; metronomeBefore_ = snapshot.metronome_enabled;
    testKey_ = key(channel); testActive_ = true;
    jam2::EngineCommand monitor; monitor.type = jam2::EngineCommandType::SetLocalMonitorEnabled; monitor.enabled = false;
    jam2::EngineCommand metronome; metronome.type = jam2::EngineCommandType::SetMetronomeEnabled; metronome.enabled = false;
    if (!backend_.submit(monitor) || !backend_.submit(metronome)) {
        finishTest(); error = QStringLiteral("The audio engine is busy. Retry calibration."); return {};
    }
    auto capture = backend_.begin(channel, error);
    if (!capture) finishTest();
    return capture;
}
bool RecordingCalibrationController::save(const RecordingCalibrationKey& k, RecordingCalibrationMethod method,
    const jam2::audio::RecordingCalibrationCapture::Result& r, QString& error)
{
    RecordingCalibrationProfile p{k, method, r.adjustment_frames, r.measured_frames,
        r.reported_frames, r.spread_frames, r.detected_clicks, QDateTime::currentDateTimeUtc().toString(Qt::ISODate)};
    const auto current = backend_.stream();
    const auto it = std::find(current.channels.input.begin(), current.channels.input.end(), k.physicalInput);
    if (!r.ok || !p.valid() || it == current.channels.input.end() ||
        key(static_cast<std::size_t>(it - current.channels.input.begin())) != k) {
        error = QStringLiteral("The measurement is invalid or the audio configuration changed. Retry calibration."); return false;
    }
    auto previous = preferences_.recordingCalibrations;
    auto& profiles = preferences_.recordingCalibrations;
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&](const auto& old) { return old.key.matchesDeviceConfiguration(k); }), profiles.end());
    if (profiles.size() >= 128) { preferences_.recordingCalibrations = previous; error = QStringLiteral("The calibration profile limit was reached. Reset an unused profile first."); return false; }
    profiles.push_back(p);
    if (!UserPreferencesStore::saveChecked(preferences_)) {
        preferences_.recordingCalibrations = std::move(previous);
        error = QStringLiteral("Could not save the calibration settings."); return false;
    }
    update(); return true;
}
bool RecordingCalibrationController::reset(const RecordingCalibrationKey& k, QString& error)
{
    auto previous = preferences_.recordingCalibrations; auto& profiles = preferences_.recordingCalibrations;
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&](const auto& p) { return p.key.matchesDeviceConfiguration(k); }), profiles.end());
    if (!UserPreferencesStore::saveChecked(preferences_)) {
        preferences_.recordingCalibrations = std::move(previous); error = QStringLiteral("Could not save calibration settings."); return false;
    }
    update(); return true;
}
void RecordingCalibrationController::update()
{
    (void)applyForDevice();
}
bool RecordingCalibrationController::prepareForSource(std::optional<std::size_t>)
{
    return applyForDevice();
}
bool RecordingCalibrationController::applyForDevice()
{
    const auto snapshot = backend_.snapshot();
    if (!snapshot.frame_clock_active) { appliedKey_.reset(); previousEngineFrame_ = 0; return false; }
    std::optional<RecordingCalibrationKey> wanted;
    std::int64_t adjustment = 0;
    const auto k = key(0);
    for (const auto& p : preferences_.recordingCalibrations) {
        if (p.valid() && p.key.matchesDeviceConfiguration(k)) { wanted = p.key; adjustment = p.adjustmentFrames; break; }
    }
    const bool restarted = snapshot.engine_frame < previousEngineFrame_;
    previousEngineFrame_ = snapshot.engine_frame;
    if (!restarted && wanted == appliedKey_ && adjustment == appliedFrames_ &&
        snapshot.device_recording_calibration_frames == adjustment) return true;
    jam2::EngineCommand c; c.type = jam2::EngineCommandType::SetRecordingCalibration;
    c.enabled = wanted.has_value(); c.signed_value = adjustment;
    if (!backend_.submit(c)) return false;
    appliedKey_ = wanted; appliedFrames_ = adjustment;
    return true;
}

void RecordingCalibrationController::finishTest()
{
    if (!testActive_) return;
    testActive_ = false;
    const auto stream = backend_.stream();
    const auto it = std::find(stream.channels.input.begin(), stream.channels.input.end(), testKey_.physicalInput);
    if (it == stream.channels.input.end() || key(static_cast<std::size_t>(it - stream.channels.input.begin())) != testKey_) return;
    jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetLocalMonitorEnabled;
    command.enabled = monitorBefore_; (void)backend_.submit(command);
    command.type = jam2::EngineCommandType::SetMetronomeEnabled;
    command.enabled = metronomeBefore_; (void)backend_.submit(command);
}
