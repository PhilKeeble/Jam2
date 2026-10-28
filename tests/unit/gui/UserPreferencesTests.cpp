#include "UserPreferences.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary preference directory was unavailable");
        QString error;
        require(UserPreferencesStore::setFilePathForTesting(
                    directory.filePath(QStringLiteral("preferences.ini")), error),
            "test preference path was rejected");

        UserPreferences expected;
        require(UserPreferencesStore::load().localAudio.deviceSetup == jam2::audio::AudioDeviceSetup::SingleDevice,
            "missing device setup preferences must keep the single-device workflow");
        expected.localAudio.deviceSetup = jam2::audio::AudioDeviceSetup::SeparateInputOutput;
        expected.localAudio.coreAudioInputId = QStringLiteral("microphone-uid");
        expected.localAudio.coreAudioInputName = QStringLiteral("USB microphone");
        require(UserPreferencesStore::load().sync.listeningTools,
            "missing listening sync preference defaults on");
        require(UserPreferencesStore::load().create.strategy == jam2::ConnectionStrategy::Jam2Server,
            "new preferences default to Server strategy");
        expected.create.strategy = jam2::ConnectionStrategy::Direct;
        expected.create.name = QStringLiteral("Alice");
        expected.join.name = QStringLiteral("\u00c9lodie");
        expected.sync.listeningTools = false;
        expected.recording.preferredMode = QStringLiteral("loopback");
        expected.recording.input.outputFolder = QStringLiteral("input-folder");
        expected.recording.input.recordUntilStopped = false;
        expected.recording.input.durationBars = 13;
        expected.recording.input.countIn = false;
        expected.recording.input.countInBars = 3;
        expected.recording.input.countInMetronome = false;
        expected.recording.input.keepMetronome = true;
        expected.recording.input.latencyAdjustmentFrames = -127;
        expected.recording.loopback.outputFolder = QStringLiteral("loopback-folder");
        expected.recording.loopback.sourceId = QStringLiteral("loopback-id");
        expected.recording.loopback.sourceName = QStringLiteral("Loopback Name");
        expected.recording.loopback.recordUntilStopped = false;
        expected.recording.loopback.durationBars = 21;
        expected.recording.loopback.silenceThresholdDb = -43.5;
        expected.recording.loopback.tailSilenceMs = 875;
        expected.recording.loopback.trimLeading = false;
        expected.recording.loopback.trimTrailing = false;
        UserPreferencesStore::save(expected);

        const UserPreferences actual = UserPreferencesStore::load();
        require(actual.localAudio.deviceSetup == expected.localAudio.deviceSetup &&
            actual.localAudio.coreAudioInputId == expected.localAudio.coreAudioInputId &&
            actual.localAudio.coreAudioInputName == expected.localAudio.coreAudioInputName,
            "CoreAudio setup and stable input identity round-trip");
        require(actual.create.strategy == jam2::ConnectionStrategy::Direct && actual.create.noStun,
            "typed connection strategy round-trips");
        require(actual.create.name==expected.create.name && actual.join.name==expected.join.name,"create/join names did not round-trip");
        require(!actual.sync.listeningTools, "listening sync preference round-trips disabled state");
        require(actual.recording.preferredMode == expected.recording.preferredMode &&
                actual.recording.input.outputFolder == expected.recording.input.outputFolder &&
                actual.recording.input.recordUntilStopped ==
                    expected.recording.input.recordUntilStopped &&
                actual.recording.input.durationBars == expected.recording.input.durationBars &&
                actual.recording.input.countIn == expected.recording.input.countIn &&
                actual.recording.input.countInBars == expected.recording.input.countInBars &&
                actual.recording.input.countInMetronome ==
                    expected.recording.input.countInMetronome &&
                actual.recording.input.keepMetronome ==
                    expected.recording.input.keepMetronome &&
                actual.recording.input.latencyAdjustmentFrames ==
                    expected.recording.input.latencyAdjustmentFrames,
            "input recording preferences did not round-trip exactly");
        require(actual.recording.loopback.outputFolder ==
                    expected.recording.loopback.outputFolder &&
                actual.recording.loopback.sourceId == expected.recording.loopback.sourceId &&
                actual.recording.loopback.sourceName == expected.recording.loopback.sourceName &&
                actual.recording.loopback.recordUntilStopped ==
                    expected.recording.loopback.recordUntilStopped &&
                actual.recording.loopback.durationBars ==
                    expected.recording.loopback.durationBars &&
                actual.recording.loopback.silenceThresholdDb ==
                    expected.recording.loopback.silenceThresholdDb &&
                actual.recording.loopback.tailSilenceMs ==
                    expected.recording.loopback.tailSilenceMs &&
                actual.recording.loopback.trimLeading ==
                    expected.recording.loopback.trimLeading &&
                actual.recording.loopback.trimTrailing ==
                    expected.recording.loopback.trimTrailing,
            "loopback recording preferences did not round-trip exactly");

        const auto hasFastPlaybackDefaults = [](const LocalTuningPreference& tuning) {
            return tuning.profile == QStringLiteral("fast") &&
                tuning.prefillFrames == 64 && tuning.frameSize == 64 && tuning.driftDeadbandPpm == 0 &&
                tuning.playbackMaxFrames == 1024 &&
                tuning.playoutDelayFrames == 64 &&
                tuning.jitterBufferFrames == 64 &&
                tuning.jitterBufferMaxFrames == 512 &&
                tuning.adaptiveTargetFrames == 64 &&
                tuning.adaptiveMinFrames == 64 &&
                tuning.adaptiveMaxFrames == 512;
        };
        require(hasFastPlaybackDefaults(actual.create.tuning) &&
                hasFastPlaybackDefaults(actual.join.tuning) &&
                actual.create.runtime.osPriority == QStringLiteral("high") &&
                actual.join.runtime.osPriority == QStringLiteral("high"),
            "new create and join preferences must use the measured Fast playback defaults");

        const QString legacyPath = directory.filePath(QStringLiteral("legacy-preferences.ini"));
        {
            QSettings legacy(legacyPath, QSettings::IniFormat);
            legacy.setValue(QStringLiteral("schema_version"), 5);
            legacy.beginGroup(QStringLiteral("create/tuning"));
            legacy.setValue(QStringLiteral("profile"), QStringLiteral("fast"));
            legacy.setValue(QStringLiteral("jitter_buffer_frames"), 512);
            legacy.endGroup();
            legacy.beginGroup(QStringLiteral("join/tuning"));
            legacy.setValue(QStringLiteral("profile"), QStringLiteral("moderate"));
            legacy.setValue(QStringLiteral("jitter_buffer_frames"), 512);
            legacy.endGroup();
            legacy.beginGroup(QStringLiteral("create/runtime"));
            legacy.setValue(QStringLiteral("os_priority"), QStringLiteral("realtime"));
            legacy.endGroup();
            legacy.beginGroup(QStringLiteral("join/runtime"));
            legacy.setValue(QStringLiteral("os_priority"), QStringLiteral("realtime"));
            legacy.endGroup();
        }
        require(UserPreferencesStore::setFilePathForTesting(legacyPath, error),
            "legacy preference path was rejected");
        const UserPreferences migrated = UserPreferencesStore::load();
        require(migrated.create.tuning.jitterBufferFrames == 64,
            "legacy fast preferences did not migrate to the measured jitter target");
        require(migrated.join.tuning.jitterBufferFrames == 512,
            "legacy migration changed a non-fast numeric tuning value");
        require(migrated.create.runtime.osPriority == QStringLiteral("high") &&
                migrated.join.runtime.osPriority == QStringLiteral("high"),
            "removed realtime preferences did not migrate to high scheduling");

        const auto writeOldFastPlayback = [](QSettings& settings,
                                             const QString& group,
                                             int prefillFrames) {
            settings.beginGroup(group);
            settings.setValue(QStringLiteral("profile"), QStringLiteral("fast"));
            settings.setValue(QStringLiteral("prefill_frames"), prefillFrames);
            settings.setValue(QStringLiteral("playout_delay_frames"), 256);
            settings.setValue(QStringLiteral("jitter_buffer_frames"), 64);
            settings.setValue(QStringLiteral("jitter_buffer_max_frames"), 1024);
            settings.setValue(QStringLiteral("adaptive_target_frames"), 256);
            settings.setValue(QStringLiteral("adaptive_min_frames"), 256);
            settings.setValue(QStringLiteral("adaptive_max_frames"), 1536);
            settings.endGroup();
        };
        const QString oldFastPath = directory.filePath(QStringLiteral("old-fast-preferences.ini"));
        {
            QSettings oldFast(oldFastPath, QSettings::IniFormat);
            oldFast.setValue(QStringLiteral("schema_version"), 6);
            writeOldFastPlayback(oldFast, QStringLiteral("create/tuning"), 256);
            writeOldFastPlayback(oldFast, QStringLiteral("join/tuning"), 256);
            oldFast.beginGroup(QStringLiteral("join/tuning"));
            oldFast.setValue(QStringLiteral("buffer_size"), 64);
            oldFast.endGroup();
        }
        require(UserPreferencesStore::setFilePathForTesting(oldFastPath, error),
            "old Fast preference path was rejected");
        const UserPreferences migratedFast = UserPreferencesStore::load();
        require(hasFastPlaybackDefaults(migratedFast.create.tuning) &&
                hasFastPlaybackDefaults(migratedFast.join.tuning) &&
                migratedFast.join.tuning.bufferSize == 64,
            "untouched old Fast create/join defaults did not migrate without changing device tuning");

        const QString customFastPath = directory.filePath(QStringLiteral("custom-fast-preferences.ini"));
        {
            QSettings customFast(customFastPath, QSettings::IniFormat);
            customFast.setValue(QStringLiteral("schema_version"), 6);
            writeOldFastPlayback(customFast, QStringLiteral("create/tuning"), 192);
        }
        require(UserPreferencesStore::setFilePathForTesting(customFastPath, error),
            "custom Fast preference path was rejected");
        const UserPreferences customFast = UserPreferencesStore::load();
        require(customFast.create.tuning.prefillFrames == 192 &&
                customFast.create.tuning.playoutDelayFrames == 256 &&
                customFast.create.tuning.jitterBufferMaxFrames == 1024 &&
                customFast.create.tuning.adaptiveTargetFrames == 256 &&
                customFast.create.tuning.adaptiveMinFrames == 256 &&
                customFast.create.tuning.adaptiveMaxFrames == 1536,
            "Fast migration changed an explicitly customized latency tuple");

        const auto writePreviousFastMaximum = [](QSettings& settings,
                                                  const QString& group,
                                                  int playbackMaximum) {
            settings.beginGroup(group);
            settings.setValue(QStringLiteral("profile"), QStringLiteral("fast"));
            settings.setValue(QStringLiteral("prefill_frames"), 64);
            settings.setValue(QStringLiteral("playback_max_frames"), playbackMaximum);
            settings.setValue(QStringLiteral("playout_delay_frames"), 64);
            settings.setValue(QStringLiteral("jitter_buffer_frames"), 64);
            settings.setValue(QStringLiteral("jitter_buffer_max_frames"), 512);
            settings.setValue(QStringLiteral("adaptive_target_frames"), 64);
            settings.setValue(QStringLiteral("adaptive_min_frames"), 64);
            settings.setValue(QStringLiteral("adaptive_max_frames"), 512);
            settings.endGroup();
        };
        const QString previousMaximumPath =
            directory.filePath(QStringLiteral("previous-fast-maximum.ini"));
        {
            QSettings previousMaximum(previousMaximumPath, QSettings::IniFormat);
            previousMaximum.setValue(QStringLiteral("schema_version"), 7);
            writePreviousFastMaximum(
                previousMaximum, QStringLiteral("create/tuning"), 1536);
            writePreviousFastMaximum(
                previousMaximum, QStringLiteral("join/tuning"), 1408);
        }
        require(UserPreferencesStore::setFilePathForTesting(previousMaximumPath, error),
            "previous Fast maximum preference path was rejected");
        const UserPreferences migratedMaximum = UserPreferencesStore::load();
        require(migratedMaximum.create.tuning.playbackMaxFrames == 1024 &&
                migratedMaximum.join.tuning.playbackMaxFrames == 1408,
            "Fast maximum migration did not update the old default while preserving an explicit custom value");

        struct PreviousPreset {
            const char* name;
            int frame, prefill, playbackMax, captureRing, playbackRing;
            int playout, jitter, jitterMax, target, minimum, maximum;
        };
        const PreviousPreset previousPresets[]{
            {"fast", 64, 64, 1024, 4096, 4096, 64, 64, 512, 64, 64, 512},
            {"moderate", 128, 256, 4096, 4096, 8192, 512, 1536, 3072, 512, 512, 4096},
            {"safe", 256, 1024, 7168, 4096, 8192, 1024, 2048, 6144, 1024, 1024, 7168},
        };
        for (const auto& preset : previousPresets) {
            LocalTuningPreference old;
            old.profile = QString::fromLatin1(preset.name);
            old.bufferSize = 64;
            old.frameSize = preset.frame;
            old.prefillFrames = preset.prefill;
            old.playbackMaxFrames = preset.playbackMax;
            old.captureRingFrames = preset.captureRing;
            old.playbackRingFrames = preset.playbackRing;
            old.playoutDelayFrames = preset.playout;
            old.jitterBufferFrames = preset.jitter;
            old.jitterBufferMaxFrames = preset.jitterMax;
            old.adaptiveTargetFrames = preset.target;
            old.adaptiveMinFrames = preset.minimum;
            old.adaptiveMaxFrames = preset.maximum;
            old.driftDeadbandPpm = 25;
            const QString path = directory.filePath(old.profile + QStringLiteral("-clock-defaults.ini"));
            require(UserPreferencesStore::setFilePathForTesting(path, error), "profile migration test path accepted");
            UserPreferences previous;
            previous.create.tuning = previous.join.tuning = old;
            // A joiner's stored packet-size field did not follow the selected
            // profile: packet size is owned by the remote creator.
            previous.join.tuning.frameSize = 64;
            UserPreferencesStore::save(previous);
            {
                QSettings settings(path, QSettings::IniFormat);
                settings.setValue(QStringLiteral("schema_version"), 9);
            }
            const auto updated = UserPreferencesStore::load();
            LocalTuningPreference wanted = old;
            wanted.driftDeadbandPpm = 0;
            if (old.profile == QStringLiteral("fast")) {
                wanted.frameSize = 64;
                wanted.prefillFrames = 64;
            }
            if (!(updated.create.tuning == wanted && updated.join.tuning == wanted)) {
                std::cerr << "Migration profile=" << preset.name
                    << " create_match=" << (updated.create.tuning == wanted)
                    << " join_match=" << (updated.join.tuning == wanted)
                    << " create_frame=" << updated.create.tuning.frameSize
                    << " join_frame=" << updated.join.tuning.frameSize
                    << " create_prefill=" << updated.create.tuning.prefillFrames
                    << " join_prefill=" << updated.join.tuning.prefillFrames
                    << " create_deadband=" << updated.create.tuning.driftDeadbandPpm
                    << " join_deadband=" << updated.join.tuning.driftDeadbandPpm << '\n';
            }
            require(updated.create.tuning == wanted && updated.join.tuning == wanted,
                "previous default create/join presets migrate timing while preserving the device callback choice");
            UserPreferencesStore::save(updated);
            const auto repeated = UserPreferencesStore::load();
            require(repeated.create.tuning == wanted && repeated.join.tuning == wanted,
                "updated profile defaults survive a save/reload without a second migration");
            for (int custom = 0; custom < 4; ++custom) {
                UserPreferences customized;
                customized.create.tuning = customized.join.tuning = old;
                if (custom == 0) ++customized.create.tuning.prefillFrames;
                if (custom == 1) customized.create.tuning.frameSize += 32;
                if (custom == 2) customized.create.tuning.driftDeadbandPpm = 7;
                if (custom == 3) customized.create.tuning.playbackMaxFrames -= 128;
                customized.join.tuning = customized.create.tuning;
                // The join-side packet size is ignored; use a meaningful
                // local customization as well when testing that case.
                if (custom == 1) ++customized.join.tuning.prefillFrames;
                UserPreferencesStore::save(customized);
                {
                    QSettings settings(path, QSettings::IniFormat);
                    settings.setValue(QStringLiteral("schema_version"), 9);
                }
                const auto preserved = UserPreferencesStore::load();
                require(preserved.create.tuning == customized.create.tuning &&
                        preserved.join.tuning == customized.join.tuning,
                    "profile timing migration preserves customized numeric presets");
            }
        }

        const QString packetDefaultPath = directory.filePath(QStringLiteral("temporary-fast-128-default.ini"));
        require(UserPreferencesStore::setFilePathForTesting(packetDefaultPath, error),
            "previous Fast packet default test path accepted");
        UserPreferences previousPacketDefault;
        previousPacketDefault.create.tuning.frameSize = previousPacketDefault.join.tuning.frameSize = 128;
        previousPacketDefault.join.tuning.bufferSize = 64;
        UserPreferencesStore::save(previousPacketDefault);
        {
            QSettings settings(packetDefaultPath, QSettings::IniFormat);
            settings.setValue(QStringLiteral("schema_version"), 10);
        }
        const auto restoredPacketDefault = UserPreferencesStore::load();
        require(hasFastPlaybackDefaults(restoredPacketDefault.create.tuning) &&
                hasFastPlaybackDefaults(restoredPacketDefault.join.tuning) &&
                restoredPacketDefault.join.tuning.bufferSize == 64,
            "temporary 128-frame Fast defaults return to 64-frame packets without changing prefill or device choice");
        UserPreferencesStore::save(restoredPacketDefault);
        require(UserPreferencesStore::load().create.tuning == restoredPacketDefault.create.tuning,
            "restored Fast packet defaults survive save/reload");
        previousPacketDefault.create.tuning.prefillFrames = 192;
        previousPacketDefault.join.tuning.driftDeadbandPpm = 7;
        UserPreferencesStore::save(previousPacketDefault);
        {
            QSettings settings(packetDefaultPath, QSettings::IniFormat);
            settings.setValue(QStringLiteral("schema_version"), 10);
        }
        const auto customPacketDefault = UserPreferencesStore::load();
        require(customPacketDefault.create.tuning == previousPacketDefault.create.tuning &&
                customPacketDefault.join.tuning == previousPacketDefault.join.tuning,
            "restoring the Fast packet default preserves customized presets");

        for (const bool manual : {false, true}) {
            const auto path = directory.filePath(manual ? QStringLiteral("legacy-direct.ini") : QStringLiteral("legacy-stun.ini"));
            { QSettings legacy(path, QSettings::IniFormat); legacy.setValue(QStringLiteral("create/no_stun"), manual); }
            require(UserPreferencesStore::setFilePathForTesting(path, error), "legacy strategy path rejected");
            require(UserPreferencesStore::load().create.strategy == (manual ? jam2::ConnectionStrategy::Direct : jam2::ConnectionStrategy::Stun),
                "legacy no-STUN setting did not migrate to typed strategy");
        }
        std::cout << "Jam2 user preference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "User preference test failure: " << error.what() << '\n';
        return 1;
    }
}
