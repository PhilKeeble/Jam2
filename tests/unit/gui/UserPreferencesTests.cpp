#include "UserPreferences.hpp"

#include <QCoreApplication>
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

        std::cout << "Jam2 user preference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "User preference test failure: " << error.what() << '\n';
        return 1;
    }
}
