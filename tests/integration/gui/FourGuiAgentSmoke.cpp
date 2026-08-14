#include "FourPeerCoordinator.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSet>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void fail(const QString& message)
{
    std::cerr << "FAIL: " << message.toStdString() << '\n';
    ++failures;
}

bool receive(AutomationProcess& process, const QString& expected, QJsonObject& event)
{
    QString error;
    if (!process.readEvent(event, 20s, error)) {
        fail(QStringLiteral("reading %1: %2").arg(expected, error));
        return false;
    }
    if (event.value(QStringLiteral("event")).toString() != expected) {
        fail(QStringLiteral("expected %1, received %2").arg(
            expected, event.value(QStringLiteral("event")).toString()));
        std::cerr << "  event: "
                  << QJsonDocument(event).toJson(QJsonDocument::Compact).toStdString()
                  << '\n';
        return false;
    }
    return true;
}

bool send(AutomationProcess& process, QJsonObject command)
{
    QString error;
    if (process.send(std::move(command), error)) return true;
    fail(QStringLiteral("sending automation command: ") + error);
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: jam2_four_gui_agent_smoke <release-jam2>\n";
        return 2;
    }

    const QString executable = QString::fromLocal8Bit(argv[1]);
    const bool show = qEnvironmentVariableIntValue("JAM2_TEST_SHOW_GUI") == 1;
    std::array<QStringList, FourPeerCoordinator::kPeerCount> arguments;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        arguments[index] << QStringLiteral("debug") << QStringLiteral("gui-agent")
            << QStringLiteral("--instance-id")
            << QStringLiteral("peer-%1").arg(index + 1);
        if (show) arguments[index] << QStringLiteral("--show-gui");
    }

    FourPeerCoordinator coordinator;
    QString launchError;
    if (!coordinator.launch(executable, arguments, launchError)) {
        fail(QStringLiteral("launching four GUI peers: ") + launchError);
        return 1;
    }

    const QSet<QString> expectedStableIds{
        QStringLiteral("application.data"),
        QStringLiteral("application.settings"),
        QStringLiteral("grid.beat.section.0.beat.0.lane.0.step.0"),
        QStringLiteral("grid.chord.section.0.beat.0.chord"),
        QStringLiteral("grid.chord.section.0.beat.0.melody.step.0"),
        QStringLiteral("grid.lyric.section.0.bar.0.lyric"),
        QStringLiteral("looper.lane.add-empty"),
        QStringLiteral("looper.wav.import"),
        QStringLiteral("metronome.bpm"),
        QStringLiteral("metronome.pattern.step.0"),
        QStringLiteral("metronome.tap"),
        QStringLiteral("performance.audio-inputs"),
        QStringLiteral("performance.count-in"),
        QStringLiteral("performance.metronome-toggle"),
        QStringLiteral("performance.midi-inputs"),
        QStringLiteral("performance.plugin-bypass"),
        QStringLiteral("performance.plugins"),
        QStringLiteral("performance.home.track-gain"),
        QStringLiteral("performance.tempo"),
        QStringLiteral("performance.track-toggle"),
        QStringLiteral("session.join"),
        QStringLiteral("session.jam-sync"),
        QStringLiteral("session.leave"),
        QStringLiteral("session.new"),
        QStringLiteral("session.open"),
        QStringLiteral("session.save"),
        QStringLiteral("session.song-title"),
        QStringLiteral("session.start"),
        QStringLiteral("looper.section.add"),
        QStringLiteral("looper.section.remove"),
        QStringLiteral("workspace.close"),
        QStringLiteral("workspace.open.beats"),
        QStringLiteral("workspace.open.chords"),
        QStringLiteral("workspace.open.looper"),
        QStringLiteral("workspace.open.lyrics"),
        QStringLiteral("workspace.open.metronome"),
    };

    QJsonArray firstPeerInventory;
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        auto& peer = coordinator.peer(index);
        QJsonObject hello;
        if (!receive(peer, QStringLiteral("hello"), hello)) continue;
        if (hello.value(QStringLiteral("protocol")).toString() !=
            QStringLiteral("jam2-gui-test-agent")) {
            fail(QStringLiteral("peer %1 reported the wrong GUI protocol").arg(index + 1));
        }
        if (hello.value(QStringLiteral("instance_id")).toString() !=
            QStringLiteral("peer-%1").arg(index + 1)) {
            fail(QStringLiteral("peer %1 reported the wrong identity").arg(index + 1));
        }
        if (hello.value(QStringLiteral("duplicate_control_id_count")).toInt(-1) != 0) {
            fail(QStringLiteral("peer %1 has duplicate stable control ids").arg(index + 1));
        }
        if (hello.value(QStringLiteral("incomplete_registered_control_count")).toInt(-1) != 0) {
            fail(QStringLiteral("peer %1 has incomplete registered control contracts").arg(index + 1));
        }
        if (hello.value(QStringLiteral("unclassified_control_count")).toInt(-1) != 0 ||
            hello.value(QStringLiteral("unnamed_control_count")).toInt(-1) != 0 ||
            hello.value(QStringLiteral("classified_control_count")).toInt(-1) !=
                hello.value(QStringLiteral("control_count")).toInt(-2)) {
            fail(QStringLiteral("peer %1 has an unclassified semantic control")
                .arg(index + 1));
        }
        // The exact number is intentionally not a product contract: ownership
        // refactors should remove obsolete hidden controls and their exclusions.
        // Keep only the semantic requirement that explicit non-user targets are
        // represented when they exist (for example custom hidden mirrors).
        if (hello.value(QStringLiteral("excluded_control_count")).toInt(-1) < 1) {
            fail(QStringLiteral("peer %1 omitted explicit non-user control exclusions")
                .arg(index + 1));
        }
        if (hello.value(QStringLiteral("virtual_control_count")).toInt(-1) < 2) {
            fail(QStringLiteral("peer %1 omitted custom-painted control targets")
                .arg(index + 1));
        }
        if (hello.value(QStringLiteral("stable_control_count")).toInt() < expectedStableIds.size()) {
            fail(QStringLiteral("peer %1 did not expose the foundation control ids").arg(index + 1));
        }
        std::cout << "peer-" << index + 1
                  << " controls=" << hello.value(QStringLiteral("control_count")).toInt()
                  << " stable=" << hello.value(QStringLiteral("stable_control_count")).toInt()
                  << " classified=" << hello.value(QStringLiteral("classified_control_count")).toInt()
                  << " unclassified=" << hello.value(QStringLiteral("unclassified_control_count")).toInt()
                  << " unnamed=" << hello.value(QStringLiteral("unnamed_control_count")).toInt()
                  << '\n';

        QSet<QString> observedIds;
        int cursor = 0;
        for (;;) {
            const QString requestId = QStringLiteral("inventory-%1-%2").arg(index + 1).arg(cursor);
            if (!send(peer, {
                    {QStringLiteral("type"), QStringLiteral("inventory")},
                    {QStringLiteral("id"), requestId},
                    {QStringLiteral("cursor"), cursor},
                })) break;
            QJsonObject inventory;
            if (!receive(peer, QStringLiteral("inventory"), inventory)) break;
            if (inventory.value(QStringLiteral("id")).toString() != requestId) {
                fail(QStringLiteral("peer %1 inventory response id mismatch").arg(index + 1));
            }
            for (const QJsonValue& value : inventory.value(QStringLiteral("controls")).toArray()) {
                const QJsonObject item = value.toObject();
                if (index == 0) firstPeerInventory.push_back(item);
                const QString id = item.value(QStringLiteral("test_id")).toString();
                if (!id.isEmpty()) observedIds.insert(id);
                if (!id.isEmpty() &&
                    (!item.value(QStringLiteral("classified")).toBool() ||
                     item.value(QStringLiteral("contract")).toString().isEmpty() ||
                     item.value(QStringLiteral("availability")).toString().isEmpty())) {
                    fail(QStringLiteral("peer %1 emitted an incomplete registered control contract")
                        .arg(index + 1));
                }
                if (!item.value(QStringLiteral("operations")).isArray() ||
                    item.value(QStringLiteral("kind")).toString().isEmpty()) {
                    fail(QStringLiteral("peer %1 emitted an untyped inventory item").arg(index + 1));
                }
                if (id == QStringLiteral("session.song-title") &&
                    item.value(QStringLiteral("operations")).toArray().contains(
                        QStringLiteral("set-text"))) {
                    fail(QStringLiteral("peer %1 advertised a write operation for the read-only song title")
                        .arg(index + 1));
                }
            }
            cursor = inventory.value(QStringLiteral("next_cursor")).toInt(-1);
            if (cursor < 0) break;
        }
        for (const QString& id : expectedStableIds) {
            if (!observedIds.contains(id)) {
                fail(QStringLiteral("peer %1 inventory omitted %2").arg(index + 1).arg(id));
            }
        }

        const QString exactId = QStringLiteral("exact-present-%1").arg(index + 1);
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), exactId},
                {QStringLiteral("control"), QStringLiteral("metronome.bpm")},
            })) {
            QJsonObject exact;
            if (receive(peer, QStringLiteral("snapshot"), exact) &&
                (exact.value(QStringLiteral("id")).toString() != exactId ||
                 exact.value(QStringLiteral("requested_control")).toString() !=
                     QStringLiteral("metronome.bpm") ||
                 !exact.value(QStringLiteral("control_found")).toBool() ||
                 exact.value(QStringLiteral("control")).toObject()
                     .value(QStringLiteral("test_id")).toString() !=
                     QStringLiteral("metronome.bpm") ||
                 !exact.value(QStringLiteral("control")).toObject()
                     .value(QStringLiteral("state")).isObject())) {
                fail(QStringLiteral("peer %1 exact snapshot omitted its classified control")
                    .arg(index + 1));
            }
        }
        const QString absentId = QStringLiteral("exact-absent-%1").arg(index + 1);
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), absentId},
                {QStringLiteral("control"), QStringLiteral("missing.control")},
            })) {
            QJsonObject exact;
            if (receive(peer, QStringLiteral("snapshot"), exact) &&
                (exact.value(QStringLiteral("id")).toString() != absentId ||
                 exact.value(QStringLiteral("control_found")).toBool() ||
                 exact.contains(QStringLiteral("control")))) {
                fail(QStringLiteral("peer %1 exact snapshot did not report clean absence")
                    .arg(index + 1));
            }
        }
        if (index == 0 && send(peer, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), QStringLiteral("exact-mixed-fields")},
                {QStringLiteral("control"), QStringLiteral("metronome.bpm")},
                {QStringLiteral("cursor"), 0},
            })) {
            QJsonObject rejected;
            if (receive(peer, QStringLiteral("command_rejected"), rejected) &&
                rejected.value(QStringLiteral("reason")).toString().isEmpty()) {
                fail(QStringLiteral("exact snapshot mixed-field rejection omitted a reason"));
            }
        }

        QString initialBeatFingerprint;
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), QStringLiteral("initial-grid-%1").arg(index + 1)},
                {QStringLiteral("cursor"), 0},
            })) {
            QJsonObject snapshot;
            if (receive(peer, QStringLiteral("snapshot"), snapshot)) {
                initialBeatFingerprint = snapshot.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("first_section")).toObject()
                    .value(QStringLiteral("beat_fingerprint")).toString();
            }
        }

        const int bpm = 121 + static_cast<int>(index);
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("set-bpm-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("metronome.bpm")},
                {QStringLiteral("operation"), QStringLiteral("set-value")},
                {QStringLiteral("value"), bpm},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("mute-lane-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("looper.lane.0.mute")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("solo-lane-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("looper.lane.0.solo")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }

        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("set-lane-gain-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("looper.lane.0.gain")},
                {QStringLiteral("operation"), QStringLiteral("set-value")},
                {QStringLiteral("value"), -3.5},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("add-lane-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("looper.lane.add-empty")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("set-home-track-gain-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("performance.home.track-gain")},
                {QStringLiteral("operation"), QStringLiteral("set-value")},
                {QStringLiteral("value"), -6.0},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("accent-step-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("metronome.pattern.step.0")},
                {QStringLiteral("operation"), QStringLiteral("set-state")},
                {QStringLiteral("value"), 2},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        const auto invokeText = [&](const QString& suffix, const QString& control, const QString& value) {
            if (!send(peer, {
                    {QStringLiteral("type"), QStringLiteral("invoke")},
                    {QStringLiteral("id"), suffix + QString::number(index + 1)},
                    {QStringLiteral("control"), control},
                    {QStringLiteral("operation"), QStringLiteral("set-text")},
                    {QStringLiteral("value"), value},
                })) return;
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        };
        invokeText(
            QStringLiteral("chord-cell-"),
            QStringLiteral("grid.chord.section.0.beat.0.chord"),
            QStringLiteral("Cmaj7"));
        invokeText(
            QStringLiteral("melody-step-"),
            QStringLiteral("grid.chord.section.0.beat.0.melody.step.0"),
            QStringLiteral("C4"));
        invokeText(
            QStringLiteral("lyric-bar-"),
            QStringLiteral("grid.lyric.section.0.bar.0.lyric"),
            QStringLiteral("four-peer lyric"));
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("beat-step-%1").arg(index + 1)},
                {QStringLiteral("control"),
                    QStringLiteral("grid.beat.section.0.beat.0.lane.0.step.0")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }

        bool bpmObserved = false;
        bool laneGainObserved = false;
        bool laneMuteObserved = false;
        bool laneSoloObserved = false;
        bool addedLaneObserved = false;
        bool homeTrackGainObserved = false;
        bool metronomeAccentObserved = false;
        bool chordCellObserved = false;
        bool musicalStepObserved = false;
        bool gridContentObserved = false;
        cursor = 0;
        for (;;) {
            if (!send(peer, {
                    {QStringLiteral("type"), QStringLiteral("snapshot")},
                    {QStringLiteral("id"), QStringLiteral("snapshot-%1-%2").arg(index + 1).arg(cursor)},
                    {QStringLiteral("cursor"), cursor},
                })) break;
            QJsonObject snapshot;
            if (!receive(peer, QStringLiteral("snapshot"), snapshot)) break;
            if (!snapshot.value(QStringLiteral("window")).isObject()) {
                fail(QStringLiteral("peer %1 snapshot omitted window state").arg(index + 1));
            }
            const QJsonObject firstSection = snapshot.value(QStringLiteral("content")).toObject()
                .value(QStringLiteral("first_section")).toObject();
            gridContentObserved = gridContentObserved ||
                firstSection.value(QStringLiteral("chord_0")).toString() == QStringLiteral("Cmaj7") &&
                firstSection.value(QStringLiteral("lyric_0")).toString() ==
                    QStringLiteral("four-peer lyric") &&
                !initialBeatFingerprint.isEmpty() &&
                firstSection.value(QStringLiteral("beat_fingerprint")).toString() !=
                    initialBeatFingerprint;
            for (const QJsonValue& value : snapshot.value(QStringLiteral("controls")).toArray()) {
                const QJsonObject item = value.toObject();
                if (!item.value(QStringLiteral("classified")).toBool()) {
                    fail(QStringLiteral("peer %1 produced an unclassified dynamic control")
                        .arg(index + 1));
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("metronome.bpm")) {
                    bpmObserved = item.value(QStringLiteral("state")).toObject()
                        .value(QStringLiteral("value")).toInt() == bpm;
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("looper.lane.0.gain")) {
                    const QJsonObject state = item.value(QStringLiteral("state")).toObject();
                    laneGainObserved = std::abs(
                        state.value(QStringLiteral("gain_db")).toDouble() + 3.5) < 0.001;
                    laneMuteObserved = state.value(QStringLiteral("muted")).toBool();
                    laneSoloObserved = state.value(QStringLiteral("solo")).toBool();
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("looper.lane.1.select")) {
                    addedLaneObserved = true;
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("performance.home.track-gain")) {
                    homeTrackGainObserved = std::abs(item.value(QStringLiteral("state")).toObject()
                        .value(QStringLiteral("gain_db")).toDouble() + 6.0) < 0.001;
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("metronome.pattern.step.0")) {
                    const QJsonObject state = item.value(QStringLiteral("state")).toObject();
                    metronomeAccentObserved = state.value(QStringLiteral("hit")).toBool() &&
                        state.value(QStringLiteral("accent")).toBool();
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("grid.chord.section.0.beat.0.chord")) {
                    chordCellObserved = item.value(QStringLiteral("state")).toObject()
                        .value(QStringLiteral("text")).toString() == QStringLiteral("Cmaj7");
                }
                if (item.value(QStringLiteral("test_id")).toString() ==
                    QStringLiteral("grid.chord.section.0.beat.0.melody.step.0")) {
                    musicalStepObserved = item.value(QStringLiteral("state")).toObject()
                        .value(QStringLiteral("text")).toString() == QStringLiteral("C4");
                }
            }
            cursor = snapshot.value(QStringLiteral("next_cursor")).toInt(-1);
            if (cursor < 0) break;
        }
        if (!bpmObserved) {
            fail(QStringLiteral("peer %1 did not read back the real BPM state").arg(index + 1));
        }
        if (!laneGainObserved) {
            fail(QStringLiteral("peer %1 did not apply the painted lane gain target")
                .arg(index + 1));
        }
        if (!laneMuteObserved || !laneSoloObserved) {
            fail(QStringLiteral("peer %1 did not apply the painted lane mute/solo targets")
                .arg(index + 1));
        }
        if (!addedLaneObserved) {
            fail(QStringLiteral("peer %1 did not apply the painted add-lane target")
                .arg(index + 1));
        }
        if (!homeTrackGainObserved) {
            fail(QStringLiteral("peer %1 did not apply the painted home track gain target")
                .arg(index + 1));
        }
        if (!metronomeAccentObserved) {
            fail(QStringLiteral("peer %1 did not apply the painted metronome step target")
                .arg(index + 1));
        }
        if (!chordCellObserved || !musicalStepObserved || !gridContentObserved) {
            fail(QStringLiteral("peer %1 did not apply all generated grid-family actions")
                .arg(index + 1));
        }

        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("add-section-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("looper.section.add")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), QStringLiteral("added-section-%1").arg(index + 1)},
                {QStringLiteral("cursor"), 0},
            })) {
            QJsonObject snapshot;
            if (receive(peer, QStringLiteral("snapshot"), snapshot)) {
                const QJsonObject content = snapshot.value(
                    QStringLiteral("content")).toObject();
                if (content.value(QStringLiteral("section_count")).toInt() != 5 ||
                    content.value(QStringLiteral("section_ids")).toArray().size() != 5) {
                    fail(QStringLiteral(
                        "peer %1 did not atomically add the fifth song/looper section")
                        .arg(index + 1));
                }
            }
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("remove-section-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("looper.section.remove")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject applied;
            (void)receive(peer, QStringLiteral("command_applied"), applied);
        }
        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), QStringLiteral("removed-section-%1").arg(index + 1)},
                {QStringLiteral("cursor"), 0},
            })) {
            QJsonObject snapshot;
            if (receive(peer, QStringLiteral("snapshot"), snapshot)) {
                const QJsonObject content = snapshot.value(
                    QStringLiteral("content")).toObject();
                if (content.value(QStringLiteral("section_count")).toInt() != 4 ||
                    content.value(QStringLiteral("section_ids")).toArray().size() != 4) {
                    fail(QStringLiteral(
                        "peer %1 did not atomically remove the pristine fifth section")
                        .arg(index + 1));
                }
            }
        }

        if (send(peer, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("reject-%1").arg(index + 1)},
                {QStringLiteral("control"), QStringLiteral("missing.control")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            })) {
            QJsonObject rejected;
            (void)receive(peer, QStringLiteral("command_rejected"), rejected);
        }
    }

    const QString inventoryPath = qEnvironmentVariable("JAM2_TEST_INVENTORY_PATH");
    if (!inventoryPath.isEmpty()) {
        QFile file(inventoryPath);
        const QByteArray bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("jam2-gui-control-inventory")},
            {QStringLiteral("controls"), firstPeerInventory},
        }).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(bytes) != bytes.size() || !file.flush()) {
            fail(QStringLiteral("writing requested GUI inventory: ") + file.errorString());
        }
    }

    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        (void)send(coordinator.peer(index), {
            {QStringLiteral("type"), QStringLiteral("shutdown")},
            {QStringLiteral("id"), QStringLiteral("shutdown-%1").arg(index + 1)},
        });
    }
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        auto& peer = coordinator.peer(index);
        QJsonObject applied;
        QJsonObject shutdown;
        (void)receive(peer, QStringLiteral("command_applied"), applied);
        (void)receive(peer, QStringLiteral("shutdown"), shutdown);
        int exitCode = -1;
        QString waitError;
        if (!peer.waitForExit(20s, exitCode, waitError)) {
            fail(QStringLiteral("peer %1 exit: %2").arg(index + 1).arg(waitError));
        } else if (exitCode != 0) {
            fail(QStringLiteral("peer %1 returned %2").arg(index + 1).arg(exitCode));
        }
    }

    if (failures != 0) {
        std::cerr << failures << " four-peer GUI agent checks failed\n";
        return 1;
    }
    coordinator.markSuccessful();
    std::cout << "four-peer GUI agent checks passed\n";
    return 0;
}
