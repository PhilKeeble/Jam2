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
                tuning.prefillFrames == 64 &&
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

        std::cout << "Jam2 user preference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "User preference test failure: " << error.what() << '\n';
        return 1;
    }
}
