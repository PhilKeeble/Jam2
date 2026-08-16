#include "FourPeerCoordinator.hpp"
#include "TestTiming.hpp"
#include "LoopbackPortReservations.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void fail(const QString& message)
{
    std::cerr << "FAIL: " << message.toStdString() << '\n';
    ++failures;
}

bool send(AutomationProcess& process, QJsonObject command)
{
    QString error;
    if (process.send(std::move(command), error)) return true;
    fail(QStringLiteral("sending automation command: ") + error);
    return false;
}

bool receive(AutomationProcess& process, const QString& expected, QJsonObject& event)
{
    QString error;
    if (!process.readEvent(event, 20s, error)) {
        fail(QStringLiteral("reading %1: %2").arg(expected, error));
        return false;
    }
    if (event.value(QStringLiteral("event")).toString() != expected) {
        fail(QStringLiteral("expected %1, received %2: %3")
            .arg(expected,
                 event.value(QStringLiteral("event")).toString(),
                 QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))));
        return false;
    }
    return true;
}

bool receiveApplied(
    AutomationProcess& process,
    const QString& id,
    QJsonObject* applied = nullptr)
{
    QJsonObject event;
    QString error;
    if (!process.readEvent(event, 20s, error)) {
        fail(QStringLiteral("reading command_applied for %1: %2").arg(id, error));
        return false;
    }
    if (event.value(QStringLiteral("event")).toString() !=
        QStringLiteral("command_applied")) {
        fail(QStringLiteral("expected command_applied for %1, received %2: %3")
            .arg(id,
                 event.value(QStringLiteral("event")).toString(),
                 QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))));
        return false;
    }
    if (event.value(QStringLiteral("id")).toString() == id) {
        if (applied != nullptr) *applied = event;
        return true;
    }
    fail(QStringLiteral("expected applied id %1, received %2")
        .arg(id, event.value(QStringLiteral("id")).toString()));
    return false;
}

bool invokeAndReceive(
    AutomationProcess& process,
    const QString& id,
    const QString& control,
    const QString& operation,
    QJsonValue value = {})
{
    QJsonObject command{
        {QStringLiteral("type"), QStringLiteral("invoke")},
        {QStringLiteral("id"), id},
        {QStringLiteral("control"), control},
        {QStringLiteral("operation"), operation},
    };
    if (!value.isUndefined()) command.insert(QStringLiteral("value"), std::move(value));
    return send(process, std::move(command)) && receiveApplied(process, id);
}

bool closeWindowAndReceive(AutomationProcess& process, const QString& id)
{
    return send(process, {
        {QStringLiteral("type"), QStringLiteral("window.close")},
        {QStringLiteral("id"), id},
    }) && receiveApplied(process, id);
}

struct Snapshot {
    QHash<QString, QJsonObject> controls;
    QJsonObject jam;
    QJsonObject content;
    QJsonObject performance;
    bool activeModal = false;
};

Snapshot snapshotAll(AutomationProcess& process, const QString& prefix)
{
    Snapshot result;
    int cursor = 0;
    int expectedTotal = -1;
    for (;;) {
        const QString id = prefix + QStringLiteral("-") + QString::number(cursor);
        if (!send(process, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), id},
                {QStringLiteral("cursor"), cursor},
            })) break;
        QJsonObject event;
        if (!receive(process, QStringLiteral("snapshot"), event)) break;
        if (event.value(QStringLiteral("id")).toString() != id) {
            fail(QStringLiteral("snapshot response id mismatch for %1").arg(prefix));
        }
        result.activeModal = result.activeModal || event.value(QStringLiteral("window"))
            .toObject().value(QStringLiteral("active_modal")).toBool();
        if (result.jam.isEmpty()) result.jam = event.value(QStringLiteral("jam")).toObject();
        if (result.content.isEmpty()) {
            result.content = event.value(QStringLiteral("content")).toObject();
        }
        if (result.performance.isEmpty()) {
            result.performance = event.value(
                QStringLiteral("performance")).toObject();
        }
        const int total = event.value(QStringLiteral("total_controls")).toInt(-1);
        if (expectedTotal < 0) expectedTotal = total;
        else if (total != expectedTotal) {
            fail(QStringLiteral("control inventory changed while paging %1").arg(prefix));
        }
        for (const QJsonValue& value : event.value(QStringLiteral("controls")).toArray()) {
            const QJsonObject control = value.toObject();
            const QString controlId = control.value(QStringLiteral("test_id")).toString();
            if (controlId.isEmpty() || !control.value(QStringLiteral("classified")).toBool()) {
                fail(QStringLiteral("%1 exposed an unclassified control at %2")
                    .arg(prefix, control.value(QStringLiteral("diagnostic_path")).toString()));
                continue;
            }
            if (result.controls.contains(controlId)) {
                fail(QStringLiteral("%1 exposed duplicate control id %2").arg(prefix, controlId));
            }
            result.controls.insert(controlId, control);
        }
        cursor = event.value(QStringLiteral("next_cursor")).toInt(-1);
        if (cursor < 0) break;
    }
    if (expectedTotal != result.controls.size()) {
        fail(QStringLiteral("%1 inventoried %2 of %3 controls")
            .arg(prefix).arg(result.controls.size()).arg(expectedTotal));
    }
    return result;
}

Snapshot snapshotControl(
    AutomationProcess& process,
    const QString& id,
    const QString& requestedControl)
{
    Snapshot result;
    if (!send(process, {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), id},
            {QStringLiteral("control"), requestedControl},
        })) {
        return result;
    }
    QJsonObject event;
    if (!receive(process, QStringLiteral("snapshot"), event)) return result;
    if (event.value(QStringLiteral("id")).toString() != id ||
        event.value(QStringLiteral("requested_control")).toString() != requestedControl ||
        !event.value(QStringLiteral("jam")).isObject() ||
        !event.value(QStringLiteral("content")).isObject() ||
        !event.value(QStringLiteral("performance")).isObject()) {
        fail(QStringLiteral("exact control snapshot %1 omitted required state").arg(id));
        return result;
    }
    result.activeModal = event.value(QStringLiteral("window")).toObject()
        .value(QStringLiteral("active_modal")).toBool();
    result.jam = event.value(QStringLiteral("jam")).toObject();
    result.content = event.value(QStringLiteral("content")).toObject();
    result.performance = event.value(QStringLiteral("performance")).toObject();
    if (!event.value(QStringLiteral("control_found")).toBool()) return result;

    const QJsonObject control = event.value(QStringLiteral("control")).toObject();
    const QString controlId = control.value(QStringLiteral("test_id")).toString();
    if (controlId != requestedControl ||
        !control.value(QStringLiteral("classified")).toBool()) {
        fail(QStringLiteral("exact control snapshot %1 returned an invalid match").arg(id));
        return result;
    }
    result.controls.insert(controlId, control);
    return result;
}

void requireControls(
    const Snapshot& snapshot,
    const QString& workflow,
    std::initializer_list<const char*> expected)
{
    if (!snapshot.activeModal) fail(workflow + QStringLiteral(" did not expose an active modal"));
    for (const char* id : expected) {
        const QString stableId = QString::fromLatin1(id);
        if (!snapshot.controls.contains(stableId)) {
            fail(QStringLiteral("%1 omitted %2").arg(workflow, stableId));
        }
    }
}

void openModal(AutomationProcess& process, const QString& id, const QString& control)
{
    (void)invokeAndReceive(process, id, control, QStringLiteral("click-async"));
}

void closeModal(
    AutomationProcess& process,
    const QString& closeId,
    const QString& closeControl)
{
    (void)invokeAndReceive(
        process, closeId, closeControl, QStringLiteral("click"));
}

QJsonObject controlState(const Snapshot& snapshot, const QString& id)
{
    return snapshot.controls.value(id).value(QStringLiteral("state")).toObject();
}

QJsonObject inputSourceSlot(const Snapshot& snapshot, int requestedSlot)
{
    const QJsonArray sourceSlots = snapshot.performance
        .value(QStringLiteral("input_source_router")).toObject()
        .value(QStringLiteral("slots")).toArray();
    for (const QJsonValue& value : sourceSlots) {
        const QJsonObject slot = value.toObject();
        if (slot.value(QStringLiteral("slot")).toInt(-1) == requestedSlot) {
            return slot;
        }
    }
    return {};
}

QJsonObject inputPlugin(
    const QJsonObject& performance,
    const QString& kind,
    int requestedSlot)
{
    const QJsonArray plugins = performance
        .value(QStringLiteral("input_plugins")).toArray();
    for (const QJsonValue& value : plugins) {
        const QJsonObject plugin = value.toObject();
        if (plugin.value(QStringLiteral("kind")).toString() == kind &&
            plugin.value(QStringLiteral("slot")).toInt(-1) == requestedSlot) {
            return plugin;
        }
    }
    return {};
}

QJsonObject inputPlugin(
    const Snapshot& snapshot,
    const QString& kind,
    int requestedSlot)
{
    return inputPlugin(snapshot.performance, kind, requestedSlot);
}

Snapshot waitForControl(
    AutomationProcess& process,
    const QString& prefix,
    const QString& control,
    std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(timeout);
    int attempt = 0;
    Snapshot latest;
    while (std::chrono::steady_clock::now() < deadline) {
        latest = snapshotControl(process,
            prefix + QStringLiteral("-%1").arg(attempt++), control);
        if (latest.controls.contains(control)) return latest;
        QThread::msleep(50);
    }
    fail(QStringLiteral("timed out waiting for control %1 in %2")
        .arg(control, prefix));
    return latest;
}

Snapshot waitForControlAbsent(
    AutomationProcess& process,
    const QString& prefix,
    const QString& control,
    std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(timeout);
    int attempt = 0;
    Snapshot latest;
    while (std::chrono::steady_clock::now() < deadline) {
        latest = snapshotControl(process,
            prefix + QStringLiteral("-%1").arg(attempt++), control);
        if (!latest.controls.contains(control)) return latest;
        QThread::msleep(50);
    }
    fail(QStringLiteral("timed out waiting for control %1 to disappear in %2")
        .arg(control, prefix));
    return latest;
}

template <typename Predicate>
Snapshot waitForControlState(
    AutomationProcess& process,
    const QString& prefix,
    const QString& control,
    Predicate predicate,
    std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(timeout);
    int attempt = 0;
    Snapshot latest;
    while (std::chrono::steady_clock::now() < deadline) {
        latest = snapshotControl(process,
            prefix + QStringLiteral("-%1").arg(attempt++), control);
        if (latest.controls.contains(control) &&
            predicate(controlState(latest, control), latest.performance)) {
            return latest;
        }
        QThread::msleep(50);
    }
    fail(QStringLiteral("timed out waiting for control state %1 in %2")
        .arg(control, prefix));
    return latest;
}

std::optional<QJsonObject> runtimeSnapshot(
    AutomationProcess& process,
    const QString& id)
{
    if (!send(process, {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), id},
            {QStringLiteral("cursor"), 0},
        })) {
        return std::nullopt;
    }
    QJsonObject event;
    if (!receive(process, QStringLiteral("snapshot"), event)) {
        return std::nullopt;
    }
    if (event.value(QStringLiteral("id")).toString() != id ||
        !event.value(QStringLiteral("jam")).isObject() ||
        !event.value(QStringLiteral("performance")).isObject()) {
        fail(QStringLiteral("runtime snapshot %1 omitted required state").arg(id));
        return std::nullopt;
    }
    return event;
}

template <typename Predicate>
bool waitForAll(
    FourPeerCoordinator& coordinator,
    const QString& description,
    Predicate predicate,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    std::chrono::milliseconds timeout = 40s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(timeout);
    int sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (std::size_t peer = 0; peer < states.size(); ++peer) {
            const auto state = runtimeSnapshot(
                coordinator.peer(peer),
                QStringLiteral("dialog-runtime-%1-%2")
                    .arg(peer + 1).arg(sequence));
            if (!state) return false;
            states[peer] = *state;
            ready = predicate(peer, *state) && ready;
        }
        if (ready) return true;
        ++sequence;
        QThread::msleep(75);
    }
    fail(QStringLiteral("timed out waiting for ") + description);
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject jam = states[peer].value(QStringLiteral("jam")).toObject();
        const QJsonObject performance = states[peer]
            .value(QStringLiteral("performance")).toObject();
        std::cerr << "  peer-" << peer + 1
                  << " role=" << jam.value(QStringLiteral("role")).toString().toStdString()
                  << " remotes=" << jam.value(QStringLiteral("remote_peer_count")).toInt(-1)
                  << " active=" << jam.value(QStringLiteral("active_remote_peer_count")).toInt(-1)
                  << " attached=" << jam.value(QStringLiteral("network_attachment_ready")).toBool()
                  << " network=" << jam.value(QStringLiteral("network_running")).toBool()
                  << " engine=" << performance.value(QStringLiteral("engine_running")).toBool()
                  << " callbacks=" << performance.value(QStringLiteral("callbacks")).toInteger()
                  << " failure=" << jam.value(QStringLiteral("failure")).toString().toStdString()
                  << " startup_failure=" << jam.value(
                        QStringLiteral("last_startup_failure")).toString().toStdString()
                  << '\n';
    }
    return false;
}

bool arrangementMatches(
    const Snapshot& snapshot,
    std::initializer_list<std::pair<int, int>> expectedSteps,
    bool loop,
    bool enabled,
    bool running,
    bool armed,
    const QString& context)
{
    const QJsonObject arrangement = snapshot.content
        .value(QStringLiteral("arrangement")).toObject();
    const QJsonArray steps = arrangement.value(QStringLiteral("steps")).toArray();
    bool matches = steps.size() == static_cast<qsizetype>(expectedSteps.size()) &&
        arrangement.value(QStringLiteral("loop")).toBool() == loop &&
        arrangement.value(QStringLiteral("enabled")).toBool() == enabled &&
        arrangement.value(QStringLiteral("running")).toBool() == running &&
        arrangement.value(QStringLiteral("armed")).toBool() == armed;
    qsizetype index = 0;
    for (const auto& [bank, repeats] : expectedSteps) {
        if (index >= steps.size()) {
            matches = false;
            break;
        }
        const QJsonObject step = steps.at(index).toObject();
        matches = matches &&
            step.value(QStringLiteral("bank")).toInt(-1) == bank &&
            step.value(QStringLiteral("repeats")).toInt(-1) == repeats;
        ++index;
    }
    if (!matches) {
        fail(QStringLiteral("%1 arrangement mismatch: %2")
            .arg(context,
                QString::fromUtf8(QJsonDocument(arrangement).toJson(
                    QJsonDocument::Compact))));
    }
    return matches;
}

void setArrangementRow(
    AutomationProcess& process,
    const QString& prefix,
    int row,
    int bank,
    int repeats)
{
    (void)invokeAndReceive(
        process,
        prefix + QStringLiteral("-section-%1").arg(row),
        QStringLiteral("looper.arrangement-dialog.row.%1.section").arg(row),
        QStringLiteral("set-index"),
        bank);
    (void)invokeAndReceive(
        process,
        prefix + QStringLiteral("-repeats-%1").arg(row),
        QStringLiteral("looper.arrangement-dialog.row.%1.repeats").arg(row),
        QStringLiteral("set-value"),
        repeats);
}

bool exerciseActiveCreatorWorkflows(
    FourPeerCoordinator& coordinator,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states)
{
    auto& creator = coordinator.peer(0);

    (void)invokeAndReceive(creator, QStringLiteral("coverage-data-open"),
        QStringLiteral("application.data"), QStringLiteral("click"));
    const Snapshot dataDrawer = snapshotControl(
        creator, QStringLiteral("coverage-data-state"),
        QStringLiteral("application.data.close"));
    if (!dataDrawer.controls.contains(QStringLiteral("application.data.close")) ||
        !controlState(dataDrawer, QStringLiteral("application.data.close"))
            .value(QStringLiteral("visible")).toBool()) {
        fail(QStringLiteral("Data drawer did not expose its close action"));
    }
    (void)invokeAndReceive(creator, QStringLiteral("coverage-data-close"),
        QStringLiteral("application.data.close"), QStringLiteral("click"));

    (void)invokeAndReceive(creator, QStringLiteral("coverage-open-metronome"),
        QStringLiteral("workspace.open.metronome"), QStringLiteral("click"));
    const Snapshot sound = snapshotControl(
        creator, QStringLiteral("coverage-metronome-sound-state"),
        QStringLiteral("metronome.sound"));
    const QJsonObject soundState = controlState(sound, QStringLiteral("metronome.sound"));
    const int soundCount = soundState.value(QStringLiteral("count")).toInt();
    const int soundIndex = soundState.value(QStringLiteral("index")).toInt(-1);
    if (soundCount < 2 || soundIndex < 0) {
        fail(QStringLiteral("Metronome sound choices were not testable"));
    } else {
        (void)invokeAndReceive(creator, QStringLiteral("coverage-metronome-sound"),
            QStringLiteral("metronome.sound"), QStringLiteral("set-index"),
            (soundIndex + 1) % soundCount);
    }
    (void)invokeAndReceive(creator, QStringLiteral("coverage-metronome-tap-1"),
        QStringLiteral("metronome.tap"), QStringLiteral("click"));
    QThread::msleep(300);
    (void)invokeAndReceive(creator, QStringLiteral("coverage-metronome-tap-2"),
        QStringLiteral("metronome.tap"), QStringLiteral("click"));

    const Snapshot peers = snapshotAll(creator, QStringLiteral("coverage-peer-list"));
    QString remotePeerControl;
    for (auto it = peers.controls.cbegin(); it != peers.controls.cend(); ++it) {
        if (it.key().startsWith(QStringLiteral("performance.home.peer.")) &&
            !it.key().endsWith(QStringLiteral(".0"))) {
            remotePeerControl = it.key();
            break;
        }
    }
    if (remotePeerControl.isEmpty()) {
        fail(QStringLiteral("Performance home did not expose a remote peer selector"));
    } else {
        (void)invokeAndReceive(creator, QStringLiteral("coverage-select-peer"),
            remotePeerControl, QStringLiteral("click"));
        (void)invokeAndReceive(creator, QStringLiteral("coverage-peer-gain"),
            QStringLiteral("performance.peer-gain"), QStringLiteral("set-value"), -7);
    }

    (void)invokeAndReceive(creator, QStringLiteral("coverage-open-chords"),
        QStringLiteral("workspace.open.chords"), QStringLiteral("click"));
    openModal(creator, QStringLiteral("coverage-generate-open"),
        QStringLiteral("chords.idea.generate"));
    Snapshot generate = snapshotAll(creator, QStringLiteral("coverage-generate-dialog"));
    requireControls(generate, QStringLiteral("Generate Practice Idea"), {
        "idea.generate-dialog.target-section",
        "idea.generate-dialog.parts",
        "idea.generate-dialog.style",
        "idea.generate-dialog.length",
        "idea.generate-dialog.exact-bpm",
        "idea.generate-dialog.bpm",
        "idea.generate-dialog.complexity",
        "idea.generate-dialog.accept",
        "idea.generate-dialog.cancel",
    });
    (void)invokeAndReceive(creator, QStringLiteral("coverage-generate-exact-bpm"),
        QStringLiteral("idea.generate-dialog.exact-bpm"),
        QStringLiteral("set-checked"), true);
    (void)invokeAndReceive(creator, QStringLiteral("coverage-generate-bpm"),
        QStringLiteral("idea.generate-dialog.bpm"), QStringLiteral("set-value"), 123);
    (void)invokeAndReceive(creator, QStringLiteral("coverage-generate-length"),
        QStringLiteral("idea.generate-dialog.length"), QStringLiteral("set-index"), 1);
    closeModal(creator, QStringLiteral("coverage-generate-accept"),
        QStringLiteral("idea.generate-dialog.accept"));

    const auto generated = runtimeSnapshot(
        creator, QStringLiteral("coverage-generated-state"));
    if (!generated) return false;
    const QString generatedSong = generated->value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("song_model_sha256")).toString();
    if (generatedSong.isEmpty() || !waitForAll(
            coordinator, QStringLiteral("modal-generated idea convergence"),
            [&generatedSong](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return content.value(QStringLiteral("song_model_sha256")).toString() ==
                        generatedSong &&
                    !content.value(QStringLiteral("first_section")).toObject()
                        .value(QStringLiteral("generated_kind")).toString().isEmpty();
            }, states)) {
        return false;
    }

    openModal(creator, QStringLiteral("coverage-details-open"),
        QStringLiteral("chords.idea.details"));
    const Snapshot details = snapshotAll(
        creator, QStringLiteral("coverage-details-dialog"));
    requireControls(details, QStringLiteral("Idea Details"), {
        "idea.details-dialog.toggle",
        "idea.details-dialog.close",
    });
    (void)invokeAndReceive(creator, QStringLiteral("coverage-details-toggle"),
        QStringLiteral("idea.details-dialog.toggle"), QStringLiteral("click"));
    closeModal(creator, QStringLiteral("coverage-details-close"),
        QStringLiteral("idea.details-dialog.close"));

    openModal(creator, QStringLiteral("coverage-continue-open"),
        QStringLiteral("chords.idea.continue"));
    const Snapshot continuation = snapshotAll(
        creator, QStringLiteral("coverage-continue-dialog"));
    requireControls(continuation, QStringLiteral("Continue Idea"), {
        "idea.continue-dialog.source",
        "idea.continue-dialog.target",
        "idea.continue-dialog.accept",
        "idea.continue-dialog.cancel",
    });
    (void)invokeAndReceive(creator, QStringLiteral("coverage-continue-source"),
        QStringLiteral("idea.continue-dialog.source"), QStringLiteral("set-index"), 0);
    (void)invokeAndReceive(creator, QStringLiteral("coverage-continue-target"),
        QStringLiteral("idea.continue-dialog.target"), QStringLiteral("set-index"), 1);
    closeModal(creator, QStringLiteral("coverage-continue-accept"),
        QStringLiteral("idea.continue-dialog.accept"));

    openModal(creator, QStringLiteral("coverage-catalog-open"),
        QStringLiteral("chords.idea.browse"));
    const Snapshot catalog = snapshotAll(
        creator, QStringLiteral("coverage-catalog-dialog"));
    requireControls(catalog, QStringLiteral("Groove Library"), {
        "idea.catalog-dialog.style",
        "idea.catalog-dialog.profile",
        "idea.catalog-dialog.catalog",
        "idea.catalog-dialog.target-section",
        "idea.catalog-dialog.length",
        "idea.catalog-dialog.preview",
        "idea.catalog-dialog.accept",
        "idea.catalog-dialog.cancel",
    });
    const QJsonObject preview = controlState(
        catalog, QStringLiteral("idea.catalog-dialog.preview"));
    if (preview.value(QStringLiteral("enabled")).toBool()) {
        (void)invokeAndReceive(creator, QStringLiteral("coverage-preview-play"),
            QStringLiteral("idea.catalog-dialog.preview"), QStringLiteral("click"));
        QThread::msleep(150);
        (void)invokeAndReceive(creator, QStringLiteral("coverage-preview-stop"),
            QStringLiteral("idea.catalog-dialog.preview"), QStringLiteral("click"));
    }
    closeModal(creator, QStringLiteral("coverage-catalog-accept"),
        QStringLiteral("idea.catalog-dialog.accept"));

    (void)invokeAndReceive(creator, QStringLiteral("coverage-section-expand-for-trim"),
        QStringLiteral("grid.chord.section.expand"), QStringLiteral("click"));
    openModal(creator, QStringLiteral("coverage-section-trim-open"),
        QStringLiteral("grid.chords.section.trim"));
    const Snapshot trim = snapshotAll(
        creator, QStringLiteral("coverage-section-trim-dialog"));
    requireControls(trim, QStringLiteral("Trim Section"), {
        "song.section-trim-dialog.confirm",
        "song.section-trim-dialog.cancel",
    });
    closeModal(creator, QStringLiteral("coverage-section-trim-confirm"),
        QStringLiteral("song.section-trim-dialog.confirm"));

    openModal(creator, QStringLiteral("coverage-section-shrink-open"),
        QStringLiteral("grid.chord.section.shrink"));
    const Snapshot shrink = snapshotAll(
        creator, QStringLiteral("coverage-section-shrink-dialog"));
    requireControls(shrink, QStringLiteral("Remove One Bar"), {
        "song.section-shrink-dialog.confirm",
        "song.section-shrink-dialog.cancel",
    });
    closeModal(creator, QStringLiteral("coverage-section-shrink-confirm"),
        QStringLiteral("song.section-shrink-dialog.confirm"));

    openModal(creator, QStringLiteral("coverage-reference-open"),
        QStringLiteral("chords.idea.generate-wav"));
    const Snapshot reference = snapshotAll(
        creator, QStringLiteral("coverage-reference-dialog"));
    requireControls(reference, QStringLiteral("Generate Reference WAVs"), {
        "idea.reference-dialog.drums",
        "idea.reference-dialog.chords",
        "idea.reference-dialog.voicing",
        "idea.reference-dialog.drum-kit",
        "idea.reference-dialog.accept",
        "idea.reference-dialog.cancel",
    });
    closeModal(creator, QStringLiteral("coverage-reference-accept"),
        QStringLiteral("idea.reference-dialog.accept"));

    if (!waitForAll(coordinator, QStringLiteral("reference WAV render and sharing"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                if (content.value(QStringLiteral("file_tasks_active")).toInt() != 0 ||
                    content.value(QStringLiteral("lane_count")).toInt() <= 0 ||
                    content.value(QStringLiteral("pending_asset_count")).toInt() != 0) {
                    return false;
                }
                const QJsonArray available =
                    content.value(QStringLiteral("asset_available")).toArray();
                return std::any_of(available.cbegin(), available.cend(),
                    [](const QJsonValue& value) { return value.toBool(); });
            }, states, 60s)) {
        return false;
    }

    (void)invokeAndReceive(creator, QStringLiteral("coverage-open-looper"),
        QStringLiteral("workspace.open.looper"), QStringLiteral("click"));
    const Snapshot looper = snapshotAll(
        creator, QStringLiteral("coverage-looper-with-reference"));
    QString regionControl;
    for (auto it = looper.controls.cbegin(); it != looper.controls.cend(); ++it) {
        if (it.key().startsWith(QStringLiteral("looper.lane.")) &&
            it.key().endsWith(QStringLiteral(".region"))) {
            regionControl = it.key();
            break;
        }
    }
    if (regionControl.isEmpty()) {
        fail(QStringLiteral("Rendered reference WAV did not expose a lane region control"));
    } else {
        const QString lanePrefix = regionControl.left(
            regionControl.size() - QStringLiteral("region").size());
        (void)invokeAndReceive(creator, QStringLiteral("coverage-lane-select"),
            lanePrefix + QStringLiteral("select"), QStringLiteral("click"));
        (void)invokeAndReceive(creator, QStringLiteral("coverage-lane-gain"),
            lanePrefix + QStringLiteral("gain"), QStringLiteral("set-value"), -3.0);
        (void)invokeAndReceive(creator, QStringLiteral("coverage-lane-region"),
            regionControl, QStringLiteral("set-region"),
            QJsonObject{
                {QStringLiteral("start_frame"), 64},
                {QStringLiteral("source_start_frame"), 0},
                {QStringLiteral("source_end_frame"), 64},
            });
    }

    openModal(creator, QStringLiteral("coverage-idea-clear-open"),
        QStringLiteral("performance.home.idea.clear"));
    const Snapshot clearIdea = snapshotAll(
        creator, QStringLiteral("coverage-idea-clear-dialog"));
    requireControls(clearIdea, QStringLiteral("Clear Idea"), {
        "idea.clear-dialog.current-section",
        "idea.clear-dialog.all-sections",
        "idea.clear-dialog.cancel",
    });
    closeModal(creator, QStringLiteral("coverage-idea-clear-confirm"),
        QStringLiteral("idea.clear-dialog.all-sections"));

    openModal(creator, QStringLiteral("coverage-new-jam-open"),
        QStringLiteral("session.new"));
    const Snapshot newJam = snapshotAll(
        creator, QStringLiteral("coverage-new-jam-dialog"));
    requireControls(newJam, QStringLiteral("New Jam"), {
        "session.new-dialog.save",
        "session.new-dialog.discard",
        "session.new-dialog.cancel",
    });
    closeModal(creator, QStringLiteral("coverage-new-jam-cancel"),
        QStringLiteral("session.new-dialog.cancel"));

    (void)closeWindowAndReceive(
        creator, QStringLiteral("coverage-window-close-open"));
    const Snapshot closeWindow = snapshotAll(
        creator, QStringLiteral("coverage-window-close-dialog"));
    requireControls(closeWindow, QStringLiteral("Close Jam2"), {
        "application.close-dialog.save",
        "application.close-dialog.discard",
        "application.close-dialog.cancel",
    });
    closeModal(creator, QStringLiteral("coverage-window-close-cancel"),
        QStringLiteral("application.close-dialog.cancel"));
    return failures == 0;
}

bool exerciseLocalFakeAudioWorkflow(AutomationProcess& peer)
{
    const QString prefix = QStringLiteral("local-fake-audio-");
    const QString prepareId = prefix + QStringLiteral("prepare");
    if (!send(peer, {
            {QStringLiteral("type"), QStringLiteral("jam.dialog-runtime.prepare")},
            {QStringLiteral("id"), prepareId},
            {QStringLiteral("test_input"), QStringLiteral("tone-440")},
        }) || !receiveApplied(peer, prepareId)) {
        return false;
    }
    const QString openId = prefix + QStringLiteral("open");
    if (!send(peer, {
            {QStringLiteral("type"), QStringLiteral("application.local-dialog.open")},
            {QStringLiteral("id"), openId},
        }) || !receiveApplied(peer, openId)) {
        return false;
    }
    const Snapshot start = waitForControl(peer, prefix + QStringLiteral("dialog"),
        QStringLiteral("application.local-engine.start"));
    const Snapshot device = snapshotControl(peer, prefix + QStringLiteral("device"),
        QStringLiteral("application.local-engine.device"));
    if (!start.controls.contains(QStringLiteral("application.local-engine.start")) ||
        controlState(device, QStringLiteral("application.local-engine.device"))
            .value(QStringLiteral("count")).toInt() != 1 ||
        !controlState(device, QStringLiteral("application.local-engine.device"))
            .value(QStringLiteral("text")).toString()
            .contains(QStringLiteral("Headless fake audio device"))) {
        fail(QStringLiteral(
            "automation local runtime did not expose its deterministic fake device"));
        return false;
    }
    closeModal(peer, prefix + QStringLiteral("start"),
        QStringLiteral("application.local-engine.start"));
    const Snapshot running = waitForControlState(
        peer, prefix + QStringLiteral("running"),
        QStringLiteral("workspace.open.looper"),
        [](const QJsonObject&, const QJsonObject& performance) {
            return performance.value(QStringLiteral("headless_audio")).toBool() &&
                performance.value(QStringLiteral("callbacks")).toInteger() > 0;
        });
    if (!running.performance.value(QStringLiteral("headless_audio")).toBool() ||
        running.performance.value(QStringLiteral("callbacks")).toInteger() <= 0) {
        return false;
    }

    for (const auto& [rate, expected] :
         std::array<std::pair<int, bool>, 2>{{{48000, true}, {96000, false}}}) {
        const QString id = prefix + QStringLiteral("preflight-%1").arg(rate);
        QJsonObject applied;
        if (!send(peer, {
                {QStringLiteral("type"), QStringLiteral("audio.device-preflight")},
                {QStringLiteral("id"), id},
                {QStringLiteral("sample_rate"), rate},
            }) || !receiveApplied(peer, id, &applied)) {
            return false;
        }
        if (applied.value(QStringLiteral("supported")).toBool() != expected ||
            !applied.value(QStringLiteral("device")).toString()
                .contains(QStringLiteral("Headless fake audio device"))) {
            fail(QStringLiteral(
                "fake-device preflight result did not preserve exact rate support"));
        }
    }

    openModal(peer, prefix + QStringLiteral("settings-open"),
        QStringLiteral("application.settings"));
    (void)waitForControl(peer, prefix + QStringLiteral("settings-ready"),
        QStringLiteral("application.settings-dialog.audio.local.apply"));
    const Snapshot settings = snapshotAll(peer, prefix + QStringLiteral("settings-state"));
    const QString rateControl = QStringLiteral(
        "application.settings-dialog.audio.local.sample-rate");
    const QJsonObject rateState = controlState(settings, rateControl);
    const int rateIndex = rateState.value(QStringLiteral("index")).toInt(-1);
    const int rateCount = rateState.value(QStringLiteral("count")).toInt();
    if (rateIndex < 0 || rateCount < 2) {
        fail(QStringLiteral("local Settings sample rates are not testable"));
    } else {
        (void)invokeAndReceive(peer, prefix + QStringLiteral("settings-rate"),
            rateControl, QStringLiteral("set-index"), (rateIndex + 1) % rateCount);
        (void)invokeAndReceive(peer, prefix + QStringLiteral("settings-apply"),
            QStringLiteral("application.settings-dialog.audio.local.apply"),
            QStringLiteral("click"));
    }

    openModal(peer, prefix + QStringLiteral("device-test-open"),
        QStringLiteral("application.settings-dialog.audio.local.test-device"));
    const Snapshot result = waitForControl(peer, prefix + QStringLiteral("device-test"),
        QStringLiteral("application.device-test-dialog.ok"));
    if (!result.controls.contains(QStringLiteral("application.device-test-dialog.ok"))) {
        return false;
    }
    closeModal(peer, prefix + QStringLiteral("device-test-close"),
        QStringLiteral("application.device-test-dialog.ok"));
    closeModal(peer, prefix + QStringLiteral("settings-close"),
        QStringLiteral("application.settings-dialog.cancel"));

    for (const QString& action : {
             QStringLiteral("session-maintenance"),
             QStringLiteral("track-reload"),
             QStringLiteral("file-dialog-cancels"),
             QStringLiteral("jamtaster-dialog-cancel"),
             QStringLiteral("jamtaster-source-disposition"),
             QStringLiteral("jamtaster-apply"),
             QStringLiteral("save-session-defaults"),
             QStringLiteral("recording-loopback"),
             QStringLiteral("jamtaster-error-boundaries"),
             QStringLiteral("recording-schedule"),
             QStringLiteral("failure-presentation"),
         }) {
        const QString id = prefix + action;
        QJsonObject applied;
        if (!send(peer, {
                {QStringLiteral("type"), QStringLiteral("application.boundary")},
                {QStringLiteral("id"), id},
                {QStringLiteral("action"), action},
            }) || !receiveApplied(peer, id, &applied)) {
            return false;
        }
        if (applied.value(QStringLiteral("action")).toString() != action ||
            !applied.value(QStringLiteral("jam")).isObject() ||
            !applied.value(QStringLiteral("content")).isObject() ||
            !applied.value(QStringLiteral("performance")).isObject()) {
            fail(QStringLiteral("boundary action omitted typed result state: ") + action);
        }
        if (action == QStringLiteral("jamtaster-apply") &&
            applied.value(QStringLiteral("performance")).toObject()
                .value(QStringLiteral("metronome_bpm")).toInt() != 137) {
            fail(QStringLiteral("JamTaster tempo boundary did not reach the live metronome"));
        }
    }

    const auto loopbackDeadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(5s);
    bool loopbackCompleted = false;
    int loopbackAttempt = 0;
    while (std::chrono::steady_clock::now() < loopbackDeadline) {
        const auto state = runtimeSnapshot(
            peer, prefix + QStringLiteral("loopback-%1").arg(loopbackAttempt++));
        if (!state) return false;
        const QJsonObject content = state->value(QStringLiteral("content")).toObject();
        if (!content.value(QStringLiteral("loopback_recording_running")).toBool() &&
            content.value(QStringLiteral("loopback_capture_available")).toBool()) {
            loopbackCompleted = true;
            break;
        }
        QThread::msleep(20);
    }
    if (!loopbackCompleted) {
        fail(QStringLiteral(
            "MainWindow fake loopback capture did not finish with a WAV artifact"));
    }

    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(5s);
    bool failureVisible = false;
    int attempt = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = runtimeSnapshot(
            peer, prefix + QStringLiteral("failure-%1").arg(attempt++));
        if (!state) return false;
        if (state->value(QStringLiteral("jam")).toObject()
                .value(QStringLiteral("last_startup_failure")).toString() ==
            QStringLiteral("automation failure presentation boundary")) {
            failureVisible = true;
            break;
        }
        QThread::msleep(20);
    }
    if (!failureVisible) {
        fail(QStringLiteral("asynchronous jam failure presentation was not observable"));
    }
    return failures == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: jam2_four_gui_modal_integration <release-jam2> [startup-only]\n";
        return 2;
    }
    const bool startupOnly = argc == 3 &&
        QString::fromLocal8Bit(argv[2]) == QStringLiteral("startup-only");
    if (argc == 3 && !startupOnly) {
        std::cerr << "unknown modal test mode\n";
        return 2;
    }

    LoopbackPortReservations portReservations;
    std::array<quint16, FourPeerCoordinator::kPeerCount> ports{};
    if (startupOnly) {
        QString portError;
        if (!portReservations.reserve(FourPeerCoordinator::kPeerCount, portError)) {
            fail(QStringLiteral("reserving four dialog-startup ports: ") + portError);
            return 1;
        }
        for (std::size_t peer = 0; peer < ports.size(); ++peer) {
            ports[peer] = portReservations.port(peer);
        }
    }

    std::array<QStringList, FourPeerCoordinator::kPeerCount> arguments;
    const bool show = qEnvironmentVariableIntValue("JAM2_TEST_SHOW_GUI") == 1;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        arguments[index] << QStringLiteral("debug") << QStringLiteral("gui-agent")
            << QStringLiteral("--instance-id")
            << QStringLiteral("modal-peer-%1").arg(index + 1);
        if (show) arguments[index] << QStringLiteral("--show-gui");
    }
    FourPeerCoordinator coordinator;
    QString error;
    if (!coordinator.launch(QString::fromLocal8Bit(argv[1]), arguments, error)) {
        fail(QStringLiteral("launching four modal GUI peers: ") + error);
        return 1;
    }
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        QJsonObject hello;
        if (!receive(coordinator.peer(index), QStringLiteral("hello"), hello)) {
            return 1;
        }
    }

    if (!startupOnly) {
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        auto& peer = coordinator.peer(index);
        const QString peerPrefix = QStringLiteral("peer-%1-").arg(index + 1);

        Snapshot local = snapshotAll(peer, peerPrefix + QStringLiteral("local-engine"));
        requireControls(local, QStringLiteral("Local Engine"), {
            "application.local-engine.cancel",
            "application.local-engine.sample-rate",
            "application.local-engine.buffer-size",
            "application.local-engine.input-channels",
            "application.local-engine.output-channels",
            "application.local-engine.save-defaults",
        });
        const QString localSampleRate = QStringLiteral(
            "application.local-engine.sample-rate");
        const QString localBufferSize = QStringLiteral(
            "application.local-engine.buffer-size");
        const QString localInputChannels = QStringLiteral(
            "application.local-engine.input-channels");
        const QString localOutputChannels = QStringLiteral(
            "application.local-engine.output-channels");
        const int originalLocalSampleRate = controlState(local, localSampleRate)
            .value(QStringLiteral("index")).toInt(-1);
        const int localSampleRateCount = controlState(local, localSampleRate)
            .value(QStringLiteral("count")).toInt();
        const int originalLocalBufferSize = controlState(local, localBufferSize)
            .value(QStringLiteral("index")).toInt(-1);
        const int localBufferSizeCount = controlState(local, localBufferSize)
            .value(QStringLiteral("count")).toInt();
        const QString originalLocalInput = controlState(local, localInputChannels)
            .value(QStringLiteral("text")).toString();
        const QString originalLocalOutput = controlState(local, localOutputChannels)
            .value(QStringLiteral("text")).toString();
        if (originalLocalSampleRate < 0 || localSampleRateCount < 2 ||
            originalLocalBufferSize < 0 || localBufferSizeCount < 2) {
            fail(QStringLiteral("Local Engine choices are not testable"));
        } else {
            (void)invokeAndReceive(
                peer,
                peerPrefix + QStringLiteral("change-local-rate"),
                localSampleRate,
                QStringLiteral("set-index"),
                (originalLocalSampleRate + 1) % localSampleRateCount);
            (void)invokeAndReceive(
                peer,
                peerPrefix + QStringLiteral("change-local-buffer"),
                localBufferSize,
                QStringLiteral("set-index"),
                (originalLocalBufferSize + 1) % localBufferSizeCount);
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-local-input"),
            localInputChannels,
            QStringLiteral("set-text"),
            QStringLiteral("2,3"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-local-output"),
            localOutputChannels,
            QStringLiteral("set-text"),
            QStringLiteral("4,5"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-local-save-defaults"),
            QStringLiteral("application.local-engine.save-defaults"),
            QStringLiteral("set-checked"),
            true);
        local = snapshotAll(
            peer, peerPrefix + QStringLiteral("changed-local-engine"));
        if (controlState(local, localInputChannels)
                .value(QStringLiteral("text")).toString() != QStringLiteral("2,3") ||
            controlState(local, localOutputChannels)
                .value(QStringLiteral("text")).toString() != QStringLiteral("4,5") ||
            !controlState(local, QStringLiteral("application.local-engine.save-defaults"))
                .value(QStringLiteral("checked")).toBool()) {
            fail(QStringLiteral("Local Engine edits were not reflected before Cancel"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("cancel-local"),
            QStringLiteral("application.local-engine.cancel"),
            QStringLiteral("click"));

        const QString startOpen = peerPrefix + QStringLiteral("open-start");
        openModal(peer, startOpen, QStringLiteral("session.start"));
        Snapshot start = snapshotAll(peer, peerPrefix + QStringLiteral("start"));
        requireControls(start, QStringLiteral("Start Jam"), {
            "session.dialog.bind-host",
            "session.dialog.port",
            "session.dialog.create-profile",
            "session.start-dialog.sample-rate",
            "session.start-dialog.buffer-size",
            "session.start-dialog.test-device",
            "session.start-dialog.accept",
            "session.start-dialog.save-defaults",
            "session.start-dialog.refresh-devices",
            "session.start-dialog.new-session",
            "session.start-dialog.cancel",
        });
        const int originalSampleRateIndex = controlState(
            start, QStringLiteral("session.start-dialog.sample-rate"))
            .value(QStringLiteral("index")).toInt(-1);
        const int sampleRateCount = controlState(
            start, QStringLiteral("session.start-dialog.sample-rate"))
            .value(QStringLiteral("count")).toInt();
        const int changedSampleRateIndex = originalSampleRateIndex == 0 ? 1 : 0;
        if (originalSampleRateIndex < 0 || changedSampleRateIndex >= sampleRateCount) {
            fail(QStringLiteral("Start Jam sample-rate choices are not testable"));
        } else {
            (void)invokeAndReceive(
                peer,
                peerPrefix + QStringLiteral("change-start-rate"),
                QStringLiteral("session.start-dialog.sample-rate"),
                QStringLiteral("set-index"),
                changedSampleRateIndex);
            start = snapshotAll(peer, peerPrefix + QStringLiteral("changed-start"));
            if (controlState(start, QStringLiteral("session.start-dialog.sample-rate"))
                    .value(QStringLiteral("index")).toInt(-1) != changedSampleRateIndex) {
                fail(QStringLiteral("Start Jam sample-rate edit was not reflected"));
            }
        }
        const QString originalStartBind = controlState(
            start, QStringLiteral("session.dialog.bind-host"))
            .value(QStringLiteral("text")).toString();
        const int originalStartPort = controlState(
            start, QStringLiteral("session.dialog.port"))
            .value(QStringLiteral("value")).toInt();
        const int originalCreateProfile = controlState(
            start, QStringLiteral("session.dialog.create-profile"))
            .value(QStringLiteral("index")).toInt(-1);
        const int createProfileCount = controlState(
            start, QStringLiteral("session.dialog.create-profile"))
            .value(QStringLiteral("count")).toInt();
        const int originalStartBuffer = controlState(
            start, QStringLiteral("session.start-dialog.buffer-size"))
            .value(QStringLiteral("index")).toInt(-1);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-start-bind"),
            QStringLiteral("session.dialog.bind-host"),
            QStringLiteral("set-text"),
            QStringLiteral("198.51.100.19"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-start-port"),
            QStringLiteral("session.dialog.port"),
            QStringLiteral("set-value"),
            50001);
        if (originalCreateProfile < 0 || createProfileCount < 2) {
            fail(QStringLiteral("Start Jam profile choices are not testable"));
        } else {
            (void)invokeAndReceive(
                peer,
                peerPrefix + QStringLiteral("change-create-profile"),
                QStringLiteral("session.dialog.create-profile"),
                QStringLiteral("set-index"),
                (originalCreateProfile + 1) % createProfileCount);
            start = snapshotAll(
                peer, peerPrefix + QStringLiteral("changed-start-profile"));
            if (controlState(start, QStringLiteral("session.start-dialog.buffer-size"))
                    .value(QStringLiteral("index")).toInt(-1) == originalStartBuffer) {
                fail(QStringLiteral(
                    "Start Jam profile edit did not update its owned tuning draft"));
            }
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-start"),
            QStringLiteral("session.start-dialog.cancel"));

        const QString startReopen = peerPrefix + QStringLiteral("reopen-start");
        openModal(peer, startReopen, QStringLiteral("session.start"));
        start = snapshotAll(peer, peerPrefix + QStringLiteral("reopened-start"));
        if (controlState(start, QStringLiteral("session.start-dialog.sample-rate"))
                .value(QStringLiteral("index")).toInt(-1) != originalSampleRateIndex) {
            fail(QStringLiteral("Start Jam cancel did not restore the sample rate"));
        }
        if (controlState(start, QStringLiteral("session.dialog.bind-host"))
                .value(QStringLiteral("text")).toString() != originalStartBind ||
            controlState(start, QStringLiteral("session.dialog.port"))
                .value(QStringLiteral("value")).toInt() != originalStartPort ||
            controlState(start, QStringLiteral("session.dialog.create-profile"))
                .value(QStringLiteral("index")).toInt(-1) != originalCreateProfile ||
            controlState(start, QStringLiteral("session.start-dialog.buffer-size"))
                .value(QStringLiteral("index")).toInt(-1) != originalStartBuffer) {
            fail(QStringLiteral(
                "Start Jam cancel leaked connection or profile draft state"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-reopened-start"),
            QStringLiteral("session.start-dialog.cancel"));

        const QString joinOpen = peerPrefix + QStringLiteral("open-join");
        openModal(peer, joinOpen, QStringLiteral("session.join"));
        Snapshot join = snapshotAll(peer, peerPrefix + QStringLiteral("join"));
        requireControls(join, QStringLiteral("Join Jam"), {
            "session.dialog.invite-url",
            "session.dialog.bind-host",
            "session.dialog.port",
            "session.join-dialog.profile",
            "session.join-dialog.buffer-size",
            "session.join-dialog.test-device",
            "session.join-dialog.accept",
            "session.join-dialog.save-defaults",
            "session.join-dialog.refresh-devices",
            "session.join-dialog.cancel",
        });
        const QString originalJoinBind = controlState(
            join, QStringLiteral("session.dialog.bind-host"))
            .value(QStringLiteral("text")).toString();
        const int originalJoinPort = controlState(
            join, QStringLiteral("session.dialog.port"))
            .value(QStringLiteral("value")).toInt();
        const int originalJoinProfile = controlState(
            join, QStringLiteral("session.join-dialog.profile"))
            .value(QStringLiteral("index")).toInt(-1);
        const int joinProfileCount = controlState(
            join, QStringLiteral("session.join-dialog.profile"))
            .value(QStringLiteral("count")).toInt();
        const int originalJoinBuffer = controlState(
            join, QStringLiteral("session.join-dialog.buffer-size"))
            .value(QStringLiteral("index")).toInt(-1);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-invite"),
            QStringLiteral("session.dialog.invite-url"),
            QStringLiteral("set-text"),
            QStringLiteral("jam2://modal-cancel-proof"));
        join = snapshotAll(peer, peerPrefix + QStringLiteral("changed-join"));
        if (controlState(join, QStringLiteral("session.dialog.invite-url"))
                .value(QStringLiteral("text")).toString() !=
            QStringLiteral("jam2://modal-cancel-proof")) {
            fail(QStringLiteral("Join Jam invite edit was not reflected"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-join-bind"),
            QStringLiteral("session.dialog.bind-host"),
            QStringLiteral("set-text"),
            QStringLiteral("203.0.113.27"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-join-port"),
            QStringLiteral("session.dialog.port"),
            QStringLiteral("set-value"),
            50002);
        if (originalJoinProfile < 0 || joinProfileCount < 2) {
            fail(QStringLiteral("Join Jam profile choices are not testable"));
        } else {
            (void)invokeAndReceive(
                peer,
                peerPrefix + QStringLiteral("change-join-profile"),
                QStringLiteral("session.join-dialog.profile"),
                QStringLiteral("set-index"),
                (originalJoinProfile + 1) % joinProfileCount);
            join = snapshotAll(
                peer, peerPrefix + QStringLiteral("changed-join-profile"));
            if (controlState(join, QStringLiteral("session.join-dialog.buffer-size"))
                    .value(QStringLiteral("index")).toInt(-1) == originalJoinBuffer) {
                fail(QStringLiteral(
                    "Join Jam profile edit did not update its owned tuning draft"));
            }
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-join"),
            QStringLiteral("session.join-dialog.cancel"));

        const QString joinReopen = peerPrefix + QStringLiteral("reopen-join");
        openModal(peer, joinReopen, QStringLiteral("session.join"));
        join = snapshotAll(peer, peerPrefix + QStringLiteral("reopened-join"));
        if (controlState(join, QStringLiteral("session.dialog.invite-url"))
                .value(QStringLiteral("text")).toString() !=
            QStringLiteral("jam2://modal-cancel-proof")) {
            fail(QStringLiteral("Join Jam cancel did not preserve the unsubmitted invite draft"));
        }
        if (controlState(join, QStringLiteral("session.dialog.bind-host"))
                .value(QStringLiteral("text")).toString() != originalJoinBind ||
            controlState(join, QStringLiteral("session.dialog.port"))
                .value(QStringLiteral("value")).toInt() != originalJoinPort ||
            controlState(join, QStringLiteral("session.join-dialog.profile"))
                .value(QStringLiteral("index")).toInt(-1) != originalJoinProfile ||
            controlState(join, QStringLiteral("session.join-dialog.buffer-size"))
                .value(QStringLiteral("index")).toInt(-1) != originalJoinBuffer) {
            fail(QStringLiteral(
                "Join Jam cancel preserved state beyond the documented invite draft"));
        }
        if (join.jam.value(QStringLiteral("role")).toString() != QStringLiteral("inactive") ||
            join.jam.value(QStringLiteral("network_running")).toBool()) {
            fail(QStringLiteral("Join Jam cancel started or retained a network session"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-reopened-join"),
            QStringLiteral("session.join-dialog.cancel"));

        const QString syncOpen = peerPrefix + QStringLiteral("open-sync");
        openModal(peer, syncOpen, QStringLiteral("session.jam-sync"));
        Snapshot sync = snapshotAll(peer, peerPrefix + QStringLiteral("jam-sync"));
        requireControls(sync, QStringLiteral("Jam Sync"), {
            "session.jam-sync-dialog.track-lanes",
            "session.jam-sync-dialog.automatic-wavs",
            "session.jam-sync-dialog.generated-ideas",
            "session.jam-sync-dialog.global-playback",
            "session.jam-sync-dialog.metronome-state",
            "session.jam-sync-dialog.recordings",
            "session.jam-sync-dialog.apply",
            "session.jam-sync-dialog.cancel",
        });
        const bool originalTrackLanes = controlState(
            sync, QStringLiteral("session.jam-sync-dialog.track-lanes"))
            .value(QStringLiteral("checked")).toBool();
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-track-policy"),
            QStringLiteral("session.jam-sync-dialog.track-lanes"),
            QStringLiteral("set-checked"),
            !originalTrackLanes);
        sync = snapshotAll(peer, peerPrefix + QStringLiteral("changed-sync"));
        if (controlState(sync, QStringLiteral("session.jam-sync-dialog.track-lanes"))
                .value(QStringLiteral("checked")).toBool() == originalTrackLanes) {
            fail(QStringLiteral("Jam Sync policy edit was not reflected"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-sync"),
            QStringLiteral("session.jam-sync-dialog.cancel"));

        const QString syncReopen = peerPrefix + QStringLiteral("reopen-sync");
        openModal(peer, syncReopen, QStringLiteral("session.jam-sync"));
        sync = snapshotAll(peer, peerPrefix + QStringLiteral("reopened-sync"));
        if (controlState(sync, QStringLiteral("session.jam-sync-dialog.track-lanes"))
                .value(QStringLiteral("checked")).toBool() != originalTrackLanes) {
            fail(QStringLiteral("Jam Sync cancel changed the active policy"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-reopened-sync"),
            QStringLiteral("session.jam-sync-dialog.cancel"));

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-settings"),
            QStringLiteral("application.settings"));
        Snapshot settings = snapshotAll(
            peer, peerPrefix + QStringLiteral("settings"));
        requireControls(settings, QStringLiteral("Settings"), {
            "application.settings-dialog.navigation",
            "application.settings-dialog.audio.local.sample-rate",
            "application.settings-dialog.audio.local.buffer-size",
            "application.settings-dialog.audio.local.input-channels",
            "application.settings-dialog.audio.local.output-channels",
            "application.settings-dialog.audio.local.split-network-by-role",
            "application.settings-dialog.audio.network.input-channels",
            "application.settings-dialog.audio.create.input-channels",
            "application.settings-dialog.create-connection.bind-host",
            "application.settings-dialog.create-connection.manual-endpoint",
            "application.settings-dialog.create-connection.public-host",
            "application.settings-dialog.create-connection.stun-server",
            "application.settings-dialog.create.audio-quality",
            "application.settings-dialog.create.tuning.profile",
            "application.settings-dialog.create.tuning.playback-prefill",
            "application.settings-dialog.create.runtime.diagnostics",
            "application.settings-dialog.join.bind-host",
            "application.settings-dialog.join.tuning.profile",
            "application.settings-dialog.join.tuning.playback-prefill",
            "application.settings-dialog.join.runtime.diagnostics",
            "application.settings-dialog.logs.folder",
            "application.settings-dialog.recording.input-until-stopped",
            "application.settings-dialog.recording.input-duration",
            "application.settings-dialog.startup.generate-idea",
            "application.settings-dialog.ideas.exact-bpm",
            "application.settings-dialog.ideas.bpm",
            "application.settings-dialog.ideas.render-on-startup",
            "application.settings-dialog.levels.monitor-enabled",
            "application.settings-dialog.metronome.compensation-deadband",
            "application.settings-dialog.views.performance-chord-preview",
            "application.settings-dialog.sync.track-lanes",
            "application.settings-dialog.sync.automatic-wavs",
            "application.settings-dialog.sync.global-playback",
            "application.settings-dialog.sync.metronome-state",
            "application.settings-dialog.sync.recordings",
            "application.settings-dialog.save",
            "application.settings-dialog.cancel",
        });
        if (controlState(settings, QStringLiteral(
                "application.settings-dialog.audio.local.sample-rate"))
                .value(QStringLiteral("index")).toInt(-1) != originalLocalSampleRate ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.audio.local.buffer-size"))
                .value(QStringLiteral("index")).toInt(-1) != originalLocalBufferSize ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.audio.local.input-channels"))
                .value(QStringLiteral("text")).toString() != originalLocalInput ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.audio.local.output-channels"))
                .value(QStringLiteral("text")).toString() != originalLocalOutput) {
            fail(QStringLiteral(
                "Local Engine Cancel leaked its draft into persisted audio defaults"));
        }

        struct SettingsEdit {
            QString control;
            QString operation;
            QString stateKey;
            QJsonValue original;
            QJsonValue changed;
        };
        std::vector<SettingsEdit> additionalSettingsEdits;
        const auto addBooleanEdit = [&](const QString& control) {
            const bool original = controlState(settings, control)
                .value(QStringLiteral("checked")).toBool();
            additionalSettingsEdits.push_back({
                control,
                QStringLiteral("set-checked"),
                QStringLiteral("checked"),
                original,
                !original,
            });
        };
        const auto addTextEdit = [&](const QString& control, const QString& changed) {
            additionalSettingsEdits.push_back({
                control,
                QStringLiteral("set-text"),
                QStringLiteral("text"),
                controlState(settings, control).value(QStringLiteral("text")),
                changed,
            });
        };
        addTextEdit(
            QStringLiteral("application.settings-dialog.create-connection.bind-host"),
            QStringLiteral("127.0.0.2"));
        addBooleanEdit(QStringLiteral(
            "application.settings-dialog.create.tuning.drift-correction"));
        addBooleanEdit(QStringLiteral(
            "application.settings-dialog.create.runtime.diagnostics"));
        const QString createQuality = QStringLiteral(
            "application.settings-dialog.create.audio-quality");
        const int originalQuality = controlState(settings, createQuality)
            .value(QStringLiteral("index")).toInt();
        const int qualityCount = controlState(settings, createQuality)
            .value(QStringLiteral("count")).toInt();
        if (qualityCount < 2) {
            fail(QStringLiteral("Settings audio-quality editor lacks both formats"));
        } else {
            additionalSettingsEdits.push_back({
                createQuality,
                QStringLiteral("set-index"),
                QStringLiteral("index"),
                originalQuality,
                (originalQuality + 1) % qualityCount,
            });
        }
        addTextEdit(
            QStringLiteral("application.settings-dialog.join.bind-host"),
            QStringLiteral("127.0.0.3"));
        addBooleanEdit(QStringLiteral(
            "application.settings-dialog.join.tuning.drift-correction"));
        addBooleanEdit(QStringLiteral(
            "application.settings-dialog.join.runtime.diagnostics"));
        const QString logsFolder = QStringLiteral(
            "application.settings-dialog.logs.folder");
        const QString originalLogsFolder = controlState(settings, logsFolder)
            .value(QStringLiteral("text")).toString();
        addTextEdit(logsFolder, originalLogsFolder + QStringLiteral("-modal-test"));
        addBooleanEdit(QStringLiteral("application.settings-dialog.ideas.exact-bpm"));
        addBooleanEdit(QStringLiteral("application.settings-dialog.levels.monitor-enabled"));
        addBooleanEdit(QStringLiteral(
            "application.settings-dialog.views.performance-chord-preview"));
        addBooleanEdit(QStringLiteral("application.settings-dialog.sync.metronome-state"));

        const auto applyAdditionalSettings = [&](const QString& phase, bool changed) {
            for (std::size_t index = 0; index < additionalSettingsEdits.size(); ++index) {
                const SettingsEdit& edit = additionalSettingsEdits[index];
                (void)invokeAndReceive(
                    peer,
                    peerPrefix + phase + QString::number(index),
                    edit.control,
                    edit.operation,
                    changed ? edit.changed : edit.original);
            }
        };
        const auto additionalSettingsMatch = [&](const Snapshot& snapshot, bool changed) {
            for (const SettingsEdit& edit : additionalSettingsEdits) {
                const QJsonValue actual = controlState(snapshot, edit.control)
                    .value(edit.stateKey);
                if (actual != (changed ? edit.changed : edit.original)) {
                    fail(QStringLiteral("Settings state mismatch for %1")
                        .arg(edit.control));
                    return false;
                }
            }
            return true;
        };
        const int originalStartupBpm = controlState(settings,
            QStringLiteral("application.settings-dialog.startup.bpm"))
            .value(QStringLiteral("value")).toInt();
        const double originalCompensationDeadband = controlState(settings,
            QStringLiteral("application.settings-dialog.metronome.compensation-deadband"))
            .value(QStringLiteral("value")).toDouble();
        const bool originalAutomaticWavs = controlState(settings,
            QStringLiteral("application.settings-dialog.sync.automatic-wavs"))
            .value(QStringLiteral("checked")).toBool();
        const bool originalInputCountIn = controlState(settings,
            QStringLiteral("application.settings-dialog.recording.input-count-in"))
            .value(QStringLiteral("checked")).toBool();
        const int changedStartupBpm = originalStartupBpm == 137 ? 138 : 137;
        const double changedCompensationDeadband =
            originalCompensationDeadband == 17.5 ? 18.5 : 17.5;
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("settings-bpm"),
            QStringLiteral("application.settings-dialog.startup.bpm"),
            QStringLiteral("set-value"), changedStartupBpm);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("settings-compensation"),
            QStringLiteral(
                "application.settings-dialog.metronome.compensation-deadband"),
            QStringLiteral("set-value"), changedCompensationDeadband);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("settings-wavs"),
            QStringLiteral("application.settings-dialog.sync.automatic-wavs"),
            QStringLiteral("set-checked"), !originalAutomaticWavs);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("settings-count-in"),
            QStringLiteral("application.settings-dialog.recording.input-count-in"),
            QStringLiteral("set-checked"), !originalInputCountIn);
        applyAdditionalSettings(QStringLiteral("settings-extra-"), true);
        settings = snapshotAll(peer, peerPrefix + QStringLiteral("changed-settings"));
        if (controlState(settings, QStringLiteral("application.settings-dialog.startup.bpm"))
                .value(QStringLiteral("value")).toInt() != changedStartupBpm ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.metronome.compensation-deadband"))
                .value(QStringLiteral("value")).toDouble() !=
                    changedCompensationDeadband ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.sync.automatic-wavs"))
                .value(QStringLiteral("checked")).toBool() == originalAutomaticWavs ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.recording.input-count-in"))
                .value(QStringLiteral("checked")).toBool() == originalInputCountIn) {
            fail(QStringLiteral("Settings representative edits were not reflected"));
        }
        (void)additionalSettingsMatch(settings, true);

        const auto checked = [&](const Snapshot& snapshot, const QString& control) {
            return controlState(snapshot, control)
                .value(QStringLiteral("checked")).toBool();
        };
        const auto enabled = [&](const Snapshot& snapshot, const QString& control) {
            return controlState(snapshot, control)
                .value(QStringLiteral("enabled")).toBool();
        };
        const auto visible = [&](const Snapshot& snapshot, const QString& control) {
            return controlState(snapshot, control)
                .value(QStringLiteral("visible")).toBool();
        };

        const QString splitAudio = QStringLiteral(
            "application.settings-dialog.audio.local.split-network-by-role");
        const bool originalSplitAudio = checked(settings, splitAudio);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-split-audio"),
            splitAudio,
            QStringLiteral("set-checked"),
            !originalSplitAudio);
        Snapshot dependencies = snapshotAll(
            peer, peerPrefix + QStringLiteral("settings-split-state"));
        if (visible(dependencies, QStringLiteral(
                "application.settings-dialog.audio.network.input-channels")) ==
                !originalSplitAudio ||
            visible(dependencies, QStringLiteral(
                "application.settings-dialog.audio.create.input-channels")) ==
                originalSplitAudio) {
            fail(QStringLiteral("Settings split-audio visibility did not switch ownership"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-restore-split-audio"),
            splitAudio,
            QStringLiteral("set-checked"),
            originalSplitAudio);

        const QString manualEndpoint = QStringLiteral(
            "application.settings-dialog.create-connection.manual-endpoint");
        const bool originalManualEndpoint = checked(settings, manualEndpoint);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-manual-endpoint"),
            manualEndpoint,
            QStringLiteral("set-checked"),
            !originalManualEndpoint);
        dependencies = snapshotAll(
            peer, peerPrefix + QStringLiteral("settings-discovery-state"));
        if (enabled(dependencies, QStringLiteral(
                "application.settings-dialog.create-connection.public-host")) !=
                !originalManualEndpoint ||
            enabled(dependencies, QStringLiteral(
                "application.settings-dialog.create-connection.stun-server")) ==
                !originalManualEndpoint) {
            fail(QStringLiteral("Settings STUN/manual-endpoint dependency did not switch"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-restore-manual-endpoint"),
            manualEndpoint,
            QStringLiteral("set-checked"),
            originalManualEndpoint);

        const QString inputUntilStopped = QStringLiteral(
            "application.settings-dialog.recording.input-until-stopped");
        const bool originalInputUntilStopped = checked(settings, inputUntilStopped);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-input-duration-mode"),
            inputUntilStopped,
            QStringLiteral("set-checked"),
            !originalInputUntilStopped);
        dependencies = snapshotAll(
            peer, peerPrefix + QStringLiteral("settings-input-duration-state"));
        if (enabled(dependencies, QStringLiteral(
                "application.settings-dialog.recording.input-duration")) ==
                !originalInputUntilStopped) {
            fail(QStringLiteral("Settings input-duration dependency did not switch"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-restore-input-duration-mode"),
            inputUntilStopped,
            QStringLiteral("set-checked"),
            originalInputUntilStopped);

        const QString generateOnStartup = QStringLiteral(
            "application.settings-dialog.startup.generate-idea");
        const bool originalGenerateOnStartup = checked(settings, generateOnStartup);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-generate-on-startup"),
            generateOnStartup,
            QStringLiteral("set-checked"),
            !originalGenerateOnStartup);
        dependencies = snapshotAll(
            peer, peerPrefix + QStringLiteral("settings-startup-wav-state"));
        if (enabled(dependencies, QStringLiteral(
                "application.settings-dialog.ideas.render-on-startup")) !=
                !originalGenerateOnStartup) {
            fail(QStringLiteral("Settings startup-WAV dependency did not switch"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("settings-restore-generate-on-startup"),
            generateOnStartup,
            QStringLiteral("set-checked"),
            originalGenerateOnStartup);

        const QString ideaExactBpm = QStringLiteral(
            "application.settings-dialog.ideas.exact-bpm");
        const bool changedIdeaExactBpm = checked(settings, ideaExactBpm);
        dependencies = snapshotAll(
            peer, peerPrefix + QStringLiteral("settings-exact-bpm-state"));
        if (enabled(dependencies, QStringLiteral(
                "application.settings-dialog.ideas.bpm")) != changedIdeaExactBpm) {
            fail(QStringLiteral("Settings exact-BPM dependency did not follow its editor"));
        }

        const auto proveProfileActivation = [&](const QString& prefix,
                                                 const QStringList& dependentControls) {
            const QString profile = QStringLiteral("application.settings-dialog.") +
                prefix + QStringLiteral(".profile");
            const QJsonObject profileState = controlState(settings, profile);
            const int originalIndex = profileState.value(QStringLiteral("index")).toInt();
            const int count = profileState.value(QStringLiteral("count")).toInt();
            if (count < 2) {
                fail(QStringLiteral("Settings %1 lacks a second profile").arg(prefix));
                return;
            }
            QJsonArray before;
            for (const QString& control : dependentControls) {
                before.append(controlState(settings, control));
            }
            (void)invokeAndReceive(
                peer,
                peerPrefix + QStringLiteral("settings-activate-") + prefix,
                profile,
                QStringLiteral("activate-index"),
                (originalIndex + 1) % count);
            const Snapshot activated = snapshotAll(
                peer, peerPrefix + QStringLiteral("settings-profile-state-") + prefix);
            bool dependentChanged = false;
            for (qsizetype index = 0; index < dependentControls.size(); ++index) {
                if (before[index].toObject() !=
                    controlState(activated, dependentControls[index])) {
                    dependentChanged = true;
                }
            }
            if (controlState(activated, profile)
                    .value(QStringLiteral("index")).toInt() == originalIndex ||
                !dependentChanged) {
                fail(QStringLiteral("Settings %1 activation did not apply its profile")
                    .arg(prefix));
            }
        };
        proveProfileActivation(QStringLiteral("create.tuning"), {
            QStringLiteral("application.settings-dialog.create.tuning.playback-prefill"),
            QStringLiteral("application.settings-dialog.create.tuning.frame-size"),
            QStringLiteral("application.settings-dialog.create.tuning.jitter-target"),
        });
        proveProfileActivation(QStringLiteral("join.tuning"), {
            QStringLiteral("application.settings-dialog.join.tuning.playback-prefill"),
            QStringLiteral("application.settings-dialog.join.tuning.buffer-size"),
            QStringLiteral("application.settings-dialog.join.tuning.jitter-target"),
        });
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-settings"),
            QStringLiteral("application.settings-dialog.cancel"));

        openModal(peer, peerPrefix + QStringLiteral("reopen-settings"),
            QStringLiteral("application.settings"));
        settings = snapshotAll(peer, peerPrefix + QStringLiteral("reopened-settings"));
        if (controlState(settings, QStringLiteral("application.settings-dialog.startup.bpm"))
                .value(QStringLiteral("value")).toInt() != originalStartupBpm ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.metronome.compensation-deadband"))
                .value(QStringLiteral("value")).toDouble() !=
                    originalCompensationDeadband ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.sync.automatic-wavs"))
                .value(QStringLiteral("checked")).toBool() != originalAutomaticWavs ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.recording.input-count-in"))
                .value(QStringLiteral("checked")).toBool() != originalInputCountIn) {
            fail(QStringLiteral("Settings cancel leaked unsaved preferences"));
        }
        (void)additionalSettingsMatch(settings, false);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("save-settings-bpm"),
            QStringLiteral("application.settings-dialog.startup.bpm"),
            QStringLiteral("set-value"), changedStartupBpm);
        (void)invokeAndReceive(peer,
            peerPrefix + QStringLiteral("save-settings-compensation"),
            QStringLiteral(
                "application.settings-dialog.metronome.compensation-deadband"),
            QStringLiteral("set-value"), changedCompensationDeadband);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("save-settings-wavs"),
            QStringLiteral("application.settings-dialog.sync.automatic-wavs"),
            QStringLiteral("set-checked"), !originalAutomaticWavs);
        (void)invokeAndReceive(peer,
            peerPrefix + QStringLiteral("save-settings-count-in"),
            QStringLiteral("application.settings-dialog.recording.input-count-in"),
            QStringLiteral("set-checked"), !originalInputCountIn);
        applyAdditionalSettings(QStringLiteral("save-settings-extra-"), true);
        closeModal(peer, peerPrefix + QStringLiteral("save-settings"),
            QStringLiteral("application.settings-dialog.save"));

        openModal(peer, peerPrefix + QStringLiteral("reopen-saved-settings"),
            QStringLiteral("application.settings"));
        settings = snapshotAll(peer, peerPrefix + QStringLiteral("saved-settings"));
        if (controlState(settings, QStringLiteral("application.settings-dialog.startup.bpm"))
                .value(QStringLiteral("value")).toInt() != changedStartupBpm ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.metronome.compensation-deadband"))
                .value(QStringLiteral("value")).toDouble() !=
                    changedCompensationDeadband ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.sync.automatic-wavs"))
                .value(QStringLiteral("checked")).toBool() == originalAutomaticWavs ||
            controlState(settings, QStringLiteral(
                "application.settings-dialog.recording.input-count-in"))
                .value(QStringLiteral("checked")).toBool() == originalInputCountIn) {
            fail(QStringLiteral("Settings Save did not persist representative preferences"));
        }
        (void)additionalSettingsMatch(settings, true);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("restore-settings-bpm"),
            QStringLiteral("application.settings-dialog.startup.bpm"),
            QStringLiteral("set-value"), originalStartupBpm);
        (void)invokeAndReceive(peer,
            peerPrefix + QStringLiteral("restore-settings-compensation"),
            QStringLiteral(
                "application.settings-dialog.metronome.compensation-deadband"),
            QStringLiteral("set-value"), originalCompensationDeadband);
        (void)invokeAndReceive(peer, peerPrefix + QStringLiteral("restore-settings-wavs"),
            QStringLiteral("application.settings-dialog.sync.automatic-wavs"),
            QStringLiteral("set-checked"), originalAutomaticWavs);
        (void)invokeAndReceive(peer,
            peerPrefix + QStringLiteral("restore-settings-count-in"),
            QStringLiteral("application.settings-dialog.recording.input-count-in"),
            QStringLiteral("set-checked"), originalInputCountIn);
        applyAdditionalSettings(QStringLiteral("restore-settings-extra-"), false);
        closeModal(peer, peerPrefix + QStringLiteral("restore-settings"),
            QStringLiteral("application.settings-dialog.save"));

        Snapshot main = snapshotAll(
            peer, peerPrefix + QStringLiteral("before-compensation"));
        const int originalModeIndex = controlState(
            main, QStringLiteral("metronome.mode"))
            .value(QStringLiteral("index")).toInt(-1);
        if (originalModeIndex < 0) {
            fail(QStringLiteral("Metronome mode is not addressable"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("listener-mode"),
            QStringLiteral("metronome.mode"),
            QStringLiteral("set-index"),
            2);
        main = snapshotAll(peer, peerPrefix + QStringLiteral("listener-main"));
        if (!main.controls.contains(QStringLiteral("metronome.compensation")) ||
            controlState(main, QStringLiteral("metronome.mode"))
                .value(QStringLiteral("index")).toInt(-1) != 2) {
            fail(QStringLiteral("Listener compensation opener did not become available"));
        }

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-compensation"),
            QStringLiteral("metronome.compensation"));
        Snapshot compensation = snapshotAll(
            peer, peerPrefix + QStringLiteral("compensation"));
        requireControls(compensation, QStringLiteral("Listener Compensation"), {
            "metronome.compensation-dialog.maximum",
            "metronome.compensation-dialog.smoothing",
            "metronome.compensation-dialog.deadband",
            "metronome.compensation-dialog.slew",
            "metronome.compensation-dialog.apply",
            "metronome.compensation-dialog.cancel",
        });
        const double originalMaximum = controlState(
            compensation, QStringLiteral("metronome.compensation-dialog.maximum"))
            .value(QStringLiteral("value")).toDouble();
        const double originalSmoothing = controlState(
            compensation, QStringLiteral("metronome.compensation-dialog.smoothing"))
            .value(QStringLiteral("value")).toDouble();
        const double originalDeadband = controlState(
            compensation, QStringLiteral("metronome.compensation-dialog.deadband"))
            .value(QStringLiteral("value")).toDouble();
        const double originalSlew = controlState(
            compensation, QStringLiteral("metronome.compensation-dialog.slew"))
            .value(QStringLiteral("value")).toDouble();
        const double changedMaximum = originalMaximum == 321.1 ? 322.1 : 321.1;
        const double changedSmoothing = originalSmoothing == 1234.5 ? 1235.5 : 1234.5;
        const double changedDeadband = originalDeadband == 12.3 ? 13.3 : 12.3;
        const double changedSlew = originalSlew == 77.7 ? 78.7 : 77.7;
        const auto setCompensation = [&](const QString& prefix,
                                         double maximum,
                                         double smoothing,
                                         double deadband,
                                         double slew) {
            (void)invokeAndReceive(peer, prefix + QStringLiteral("-maximum"),
                QStringLiteral("metronome.compensation-dialog.maximum"),
                QStringLiteral("set-value"), maximum);
            (void)invokeAndReceive(peer, prefix + QStringLiteral("-smoothing"),
                QStringLiteral("metronome.compensation-dialog.smoothing"),
                QStringLiteral("set-value"), smoothing);
            (void)invokeAndReceive(peer, prefix + QStringLiteral("-deadband"),
                QStringLiteral("metronome.compensation-dialog.deadband"),
                QStringLiteral("set-value"), deadband);
            (void)invokeAndReceive(peer, prefix + QStringLiteral("-slew"),
                QStringLiteral("metronome.compensation-dialog.slew"),
                QStringLiteral("set-value"), slew);
        };
        setCompensation(
            peerPrefix + QStringLiteral("cancel-compensation"),
            changedMaximum,
            changedSmoothing,
            changedDeadband,
            changedSlew);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-compensation"),
            QStringLiteral("metronome.compensation-dialog.cancel"));

        openModal(peer, peerPrefix + QStringLiteral("reopen-compensation"),
            QStringLiteral("metronome.compensation"));
        compensation = snapshotAll(
            peer, peerPrefix + QStringLiteral("reopened-compensation"));
        if (controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.maximum"))
                    .value(QStringLiteral("value")).toDouble() != originalMaximum ||
            controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.smoothing"))
                    .value(QStringLiteral("value")).toDouble() != originalSmoothing ||
            controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.deadband"))
                    .value(QStringLiteral("value")).toDouble() != originalDeadband ||
            controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.slew"))
                    .value(QStringLiteral("value")).toDouble() != originalSlew) {
            fail(QStringLiteral("Listener Compensation Cancel leaked edits"));
        }
        setCompensation(
            peerPrefix + QStringLiteral("apply-compensation"),
            changedMaximum,
            changedSmoothing,
            changedDeadband,
            changedSlew);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("apply-compensation"),
            QStringLiteral("metronome.compensation-dialog.apply"));

        main = snapshotAll(peer, peerPrefix + QStringLiteral("applied-compensation"));
        if (controlState(main, QStringLiteral("metronome.compensation.maximum"))
                .value(QStringLiteral("value")).toDouble() != changedMaximum ||
            controlState(main, QStringLiteral("metronome.compensation.smoothing"))
                .value(QStringLiteral("value")).toDouble() != changedSmoothing ||
            controlState(main, QStringLiteral("metronome.compensation.deadband"))
                .value(QStringLiteral("value")).toDouble() != changedDeadband ||
            controlState(main, QStringLiteral("metronome.compensation.slew"))
                .value(QStringLiteral("value")).toDouble() != changedSlew) {
            fail(QStringLiteral("Listener Compensation Apply did not update backing values"));
        }

        openModal(peer, peerPrefix + QStringLiteral("reopen-applied-compensation"),
            QStringLiteral("metronome.compensation"));
        compensation = snapshotAll(
            peer, peerPrefix + QStringLiteral("persisted-compensation"));
        if (controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.maximum"))
                    .value(QStringLiteral("value")).toDouble() != changedMaximum ||
            controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.smoothing"))
                    .value(QStringLiteral("value")).toDouble() != changedSmoothing ||
            controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.deadband"))
                    .value(QStringLiteral("value")).toDouble() != changedDeadband ||
            controlState(compensation,
                QStringLiteral("metronome.compensation-dialog.slew"))
                    .value(QStringLiteral("value")).toDouble() != changedSlew) {
            fail(QStringLiteral("Listener Compensation Apply did not persist values"));
        }
        setCompensation(
            peerPrefix + QStringLiteral("restore-compensation"),
            originalMaximum,
            originalSmoothing,
            originalDeadband,
            originalSlew);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("restore-compensation"),
            QStringLiteral("metronome.compensation-dialog.apply"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("restore-metronome-mode"),
            QStringLiteral("metronome.mode"),
            QStringLiteral("set-index"),
            originalModeIndex);

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-export-cancel"),
            QStringLiteral("looper.export"));
        const Snapshot exportDialog = snapshotAll(
            peer, peerPrefix + QStringLiteral("export-cancel"));
        requireControls(exportDialog, QStringLiteral("Export Track Audio"), {
            "looper.export-dialog.scope",
            "looper.export-dialog.accept",
            "looper.export-dialog.cancel",
        });
        if (controlState(exportDialog,
                QStringLiteral("looper.export-dialog.scope"))
                    .value(QStringLiteral("index")).toInt(-1) != 0 ||
            !controlState(exportDialog,
                QStringLiteral("looper.export-dialog.accept"))
                    .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral(
                "Export dialog did not expose its default current-section action"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-export"),
            QStringLiteral("looper.export-dialog.cancel"));

        Snapshot beforeArrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("before-arrangement"));
        const QJsonObject originalArrangement = beforeArrangement.content
            .value(QStringLiteral("arrangement")).toObject();
        if (originalArrangement.value(QStringLiteral("steps")).toArray().size() != 0 ||
            originalArrangement.value(QStringLiteral("enabled")).toBool() ||
            originalArrangement.value(QStringLiteral("running")).toBool() ||
            originalArrangement.value(QStringLiteral("armed")).toBool()) {
            fail(QStringLiteral("Arrangement modal fixture did not start from an inactive empty model"));
        }
        const bool originalArrangementLoop = originalArrangement
            .value(QStringLiteral("loop")).toBool();

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-arrangement-cancel"),
            QStringLiteral("looper.arrangement"));
        Snapshot arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-cancel"));
        requireControls(arrangement, QStringLiteral("Arrangement"), {
            "looper.arrangement-dialog.rows",
            "looper.arrangement-dialog.row.0.section",
            "looper.arrangement-dialog.row.0.repeats",
            "looper.arrangement-dialog.add",
            "looper.arrangement-dialog.remove",
            "looper.arrangement-dialog.up",
            "looper.arrangement-dialog.down",
            "looper.arrangement-dialog.loop",
            "looper.arrangement-dialog.save",
            "looper.arrangement-dialog.toggle-active",
            "looper.arrangement-dialog.cancel",
        });
        setArrangementRow(
            peer, peerPrefix + QStringLiteral("cancel-row-0"), 0, 1, 2);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("cancel-add"),
            QStringLiteral("looper.arrangement-dialog.add"),
            QStringLiteral("click"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-new-row-selected"));
        requireControls(arrangement, QStringLiteral("Arrangement row creation"), {
            "looper.arrangement-dialog.row.1.section",
            "looper.arrangement-dialog.row.1.repeats",
        });
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("remove-new-row-without-edit"),
            QStringLiteral("looper.arrangement-dialog.remove"),
            QStringLiteral("click"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-new-row-removed"));
        if (arrangement.controls.contains(
                QStringLiteral("looper.arrangement-dialog.row.1.section"))) {
            fail(QStringLiteral("Arrangement Add did not select its new row for Remove"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("cancel-readd"),
            QStringLiteral("looper.arrangement-dialog.add"),
            QStringLiteral("click"));
        setArrangementRow(
            peer, peerPrefix + QStringLiteral("cancel-row-1"), 1, 2, 3);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("cancel-loop"),
            QStringLiteral("looper.arrangement-dialog.loop"),
            QStringLiteral("set-checked"),
            !originalArrangementLoop);
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-two-rows"));

        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("arrangement-up"),
            QStringLiteral("looper.arrangement-dialog.up"),
            QStringLiteral("click"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-after-up"));
        if (controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.0.section"))
                    .value(QStringLiteral("index")).toInt(-1) != 2 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.0.repeats"))
                    .value(QStringLiteral("value")).toInt(-1) != 3 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.1.section"))
                    .value(QStringLiteral("index")).toInt(-1) != 1 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.1.repeats"))
                    .value(QStringLiteral("value")).toInt(-1) != 2) {
            fail(QStringLiteral("Arrangement Up did not preserve and reorder both row values"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("arrangement-down"),
            QStringLiteral("looper.arrangement-dialog.down"),
            QStringLiteral("click"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-after-down"));
        if (controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.0.section"))
                    .value(QStringLiteral("index")).toInt(-1) != 1 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.1.section"))
                    .value(QStringLiteral("index")).toInt(-1) != 2) {
            fail(QStringLiteral("Arrangement Down did not restore row order"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("arrangement-remove"),
            QStringLiteral("looper.arrangement-dialog.remove"),
            QStringLiteral("click"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("arrangement-after-remove"));
        if (arrangement.controls.contains(
                QStringLiteral("looper.arrangement-dialog.row.1.section"))) {
            fail(QStringLiteral("Arrangement Remove retained the selected second row"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("arrangement-readd"),
            QStringLiteral("looper.arrangement-dialog.add"),
            QStringLiteral("click"));
        setArrangementRow(
            peer, peerPrefix + QStringLiteral("cancel-readded-row-1"), 1, 2, 3);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-arrangement"),
            QStringLiteral("looper.arrangement-dialog.cancel"));

        Snapshot afterArrangementCancel = snapshotAll(
            peer, peerPrefix + QStringLiteral("after-arrangement-cancel"));
        if (afterArrangementCancel.content.value(QStringLiteral("arrangement")).toObject() !=
            originalArrangement) {
            fail(QStringLiteral("Arrangement Cancel changed the maintained model or lifecycle"));
        }

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-arrangement-save"),
            QStringLiteral("looper.arrangement"));
        setArrangementRow(
            peer, peerPrefix + QStringLiteral("save-row-0"), 0, 0, 2);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("save-add"),
            QStringLiteral("looper.arrangement-dialog.add"),
            QStringLiteral("click"));
        setArrangementRow(
            peer, peerPrefix + QStringLiteral("save-row-1"), 1, 2, 3);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("save-loop"),
            QStringLiteral("looper.arrangement-dialog.loop"),
            QStringLiteral("set-checked"),
            false);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("save-arrangement"),
            QStringLiteral("looper.arrangement-dialog.save"));
        Snapshot savedArrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("saved-arrangement"));
        (void)arrangementMatches(
            savedArrangement, {{0, 2}, {2, 3}}, false, false, false, false,
            QStringLiteral("saved"));

        openModal(
            peer,
            peerPrefix + QStringLiteral("reopen-saved-arrangement"),
            QStringLiteral("looper.arrangement"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("persisted-arrangement"));
        if (controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.0.section"))
                    .value(QStringLiteral("index")).toInt(-1) != 0 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.0.repeats"))
                    .value(QStringLiteral("value")).toInt(-1) != 2 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.1.section"))
                    .value(QStringLiteral("index")).toInt(-1) != 2 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.row.1.repeats"))
                    .value(QStringLiteral("value")).toInt(-1) != 3 ||
            controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.loop"))
                    .value(QStringLiteral("checked")).toBool()) {
            fail(QStringLiteral("Arrangement Save did not repopulate the full editor state"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("start-arrangement"),
            QStringLiteral("looper.arrangement-dialog.toggle-active"));
        Snapshot startedArrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("started-arrangement"));
        (void)arrangementMatches(
            startedArrangement, {{0, 2}, {2, 3}}, false, true, false, true,
            QStringLiteral("started"));

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-active-arrangement"),
            QStringLiteral("looper.arrangement"));
        arrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("active-arrangement"));
        if (controlState(arrangement,
                QStringLiteral("looper.arrangement-dialog.toggle-active"))
                    .value(QStringLiteral("text")).toString() != QStringLiteral("Stop")) {
            fail(QStringLiteral("Active Arrangement dialog did not expose Stop"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("stop-arrangement"),
            QStringLiteral("looper.arrangement-dialog.toggle-active"));
        Snapshot stoppedArrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("stopped-arrangement"));
        (void)arrangementMatches(
            stoppedArrangement, {{0, 2}, {2, 3}}, false, false, false, false,
            QStringLiteral("stopped"));

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-restore-arrangement"),
            QStringLiteral("looper.arrangement"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("select-restore-row-1"),
            QStringLiteral("looper.arrangement-dialog.row.1.repeats"),
            QStringLiteral("set-value"),
            4);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("remove-restore-row-1"),
            QStringLiteral("looper.arrangement-dialog.remove"),
            QStringLiteral("click"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("select-restore-row-0"),
            QStringLiteral("looper.arrangement-dialog.row.0.repeats"),
            QStringLiteral("set-value"),
            3);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("remove-restore-row-0"),
            QStringLiteral("looper.arrangement-dialog.remove"),
            QStringLiteral("click"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("restore-arrangement-loop"),
            QStringLiteral("looper.arrangement-dialog.loop"),
            QStringLiteral("set-checked"),
            originalArrangementLoop);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("restore-arrangement"),
            QStringLiteral("looper.arrangement-dialog.save"));
        Snapshot restoredArrangement = snapshotAll(
            peer, peerPrefix + QStringLiteral("restored-arrangement"));
        if (restoredArrangement.content.value(QStringLiteral("arrangement")).toObject() !=
            originalArrangement) {
            fail(QStringLiteral("Arrangement workflow did not restore the original model"));
        }

        const Snapshot beforeRename = snapshotAll(
            peer, peerPrefix + QStringLiteral("before-lane-rename"));
        const QJsonArray originalLaneNames =
            beforeRename.content.value(QStringLiteral("lane_names")).toArray();
        const QString originalLaneName = originalLaneNames.isEmpty()
            ? QString{} : originalLaneNames.first().toString();
        const QString renamedLane = QStringLiteral("Renamed lane %1").arg(index + 1);
        openModal(
            peer,
            peerPrefix + QStringLiteral("open-lane-rename-cancel"),
            QStringLiteral("looper.lane.0.rename"));
        Snapshot renameLane = snapshotAll(
            peer, peerPrefix + QStringLiteral("lane-rename-cancel"));
        requireControls(renameLane, QStringLiteral("Rename lane"), {
            "looper.lane-rename-dialog.name",
            "looper.lane-rename-dialog.save",
            "looper.lane-rename-dialog.cancel",
        });
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-cancelled-lane-name"),
            QStringLiteral("looper.lane-rename-dialog.name"),
            QStringLiteral("set-text"),
            renamedLane + QStringLiteral(" cancelled"));
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-lane-rename"),
            QStringLiteral("looper.lane-rename-dialog.cancel"));
        const Snapshot cancelledRename = snapshotAll(
            peer, peerPrefix + QStringLiteral("cancelled-lane-rename"));
        const QJsonArray cancelledLaneNames =
            cancelledRename.content.value(QStringLiteral("lane_names")).toArray();
        if (cancelledLaneNames.isEmpty() ||
            cancelledLaneNames.first().toString() != originalLaneName) {
            fail(QStringLiteral("Rename lane Cancel changed the model"));
        }

        openModal(
            peer,
            peerPrefix + QStringLiteral("open-lane-rename-save"),
            QStringLiteral("looper.lane.0.rename"));
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("change-saved-lane-name"),
            QStringLiteral("looper.lane-rename-dialog.name"),
            QStringLiteral("set-text"),
            renamedLane);
        closeModal(
            peer,
            peerPrefix + QStringLiteral("save-lane-rename"),
            QStringLiteral("looper.lane-rename-dialog.save"));
        const Snapshot savedRename = snapshotAll(
            peer, peerPrefix + QStringLiteral("saved-lane-rename"));
        const QJsonArray savedLaneNames =
            savedRename.content.value(QStringLiteral("lane_names")).toArray();
        if (savedLaneNames.isEmpty() ||
            savedLaneNames.first().toString() != renamedLane) {
            fail(QStringLiteral("Rename lane Save did not update the model"));
        }

        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("add-recording-lane"),
            QStringLiteral("looper.lane.add-empty"),
            QStringLiteral("click"));
        openModal(
            peer,
            peerPrefix + QStringLiteral("open-arm-recording"),
            QStringLiteral("looper.lane.0.arm"));
        Snapshot recording = snapshotAll(
            peer, peerPrefix + QStringLiteral("arm-recording"));
        requireControls(recording, QStringLiteral("Arm Lane Recording"), {
            "looper.recording-dialog.mode",
            "looper.recording-dialog.input-source",
            "looper.recording.output-path",
            "looper.recording-dialog.include-backing",
            "looper.recording-dialog.include-metronome",
            "looper.recording-dialog.browse-output",
            "looper.recording.loopback-source",
            "looper.recording-dialog.refresh-loopback",
            "looper.recording-dialog.advanced",
            "looper.recording.latency-adjustment",
            "looper.recording.silence-threshold",
            "looper.recording.tail-silence",
            "looper.recording.trim-leading",
            "looper.recording.trim-trailing",
            "looper.recording-dialog.arm",
            "looper.recording-dialog.cancel",
        });
        const int originalRecordingMode = controlState(
            recording, QStringLiteral("looper.recording-dialog.mode"))
            .value(QStringLiteral("index")).toInt(-1);
        const double originalSilence = controlState(
            recording, QStringLiteral("looper.recording.silence-threshold"))
            .value(QStringLiteral("value")).toDouble();
        const int originalTail = controlState(
            recording, QStringLiteral("looper.recording.tail-silence"))
            .value(QStringLiteral("value")).toInt();
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("recording-loopback-mode"),
            QStringLiteral("looper.recording-dialog.mode"),
            QStringLiteral("set-index"),
            2);
        recording = snapshotAll(
            peer, peerPrefix + QStringLiteral("recording-loopback"));
        if (!controlState(recording, QStringLiteral("looper.recording-dialog.arm"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral("Loopback recording could not be armed without an engine"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("recording-open-advanced"),
            QStringLiteral("looper.recording-dialog.advanced"),
            QStringLiteral("click"));
        const double changedSilence = originalSilence == -37.5 ? -38.5 : -37.5;
        const int changedTail = originalTail == 2345 ? 2346 : 2345;
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("recording-change-silence"),
            QStringLiteral("looper.recording.silence-threshold"),
            QStringLiteral("set-value"),
            changedSilence);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("recording-change-tail"),
            QStringLiteral("looper.recording.tail-silence"),
            QStringLiteral("set-value"),
            changedTail);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("recording-input-mode"),
            QStringLiteral("looper.recording-dialog.mode"),
            QStringLiteral("set-index"),
            0);
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("recording-return-loopback"),
            QStringLiteral("looper.recording-dialog.mode"),
            QStringLiteral("set-index"),
            2);
        recording = snapshotAll(
            peer, peerPrefix + QStringLiteral("recording-loopback-draft"));
        if (controlState(recording, QStringLiteral("looper.recording.silence-threshold"))
                .value(QStringLiteral("value")).toDouble() != changedSilence ||
            controlState(recording, QStringLiteral("looper.recording.tail-silence"))
                .value(QStringLiteral("value")).toInt() != changedTail) {
            fail(QStringLiteral("Arm Lane Recording did not preserve its per-mode draft"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-arm-recording"),
            QStringLiteral("looper.recording-dialog.cancel"));

        openModal(
            peer,
            peerPrefix + QStringLiteral("reopen-arm-recording"),
            QStringLiteral("looper.lane.0.arm"));
        recording = snapshotAll(
            peer, peerPrefix + QStringLiteral("reopened-arm-recording"));
        if (controlState(recording, QStringLiteral("looper.recording-dialog.mode"))
                .value(QStringLiteral("index")).toInt(-1) != originalRecordingMode) {
            fail(QStringLiteral("Arm Lane Recording Cancel changed the preferred mode"));
        }
        (void)invokeAndReceive(
            peer,
            peerPrefix + QStringLiteral("reopened-recording-loopback"),
            QStringLiteral("looper.recording-dialog.mode"),
            QStringLiteral("set-index"),
            2);
        recording = snapshotAll(
            peer, peerPrefix + QStringLiteral("reopened-recording-loopback-state"));
        if (
            controlState(recording, QStringLiteral("looper.recording.silence-threshold"))
                .value(QStringLiteral("value")).toDouble() != originalSilence ||
            controlState(recording, QStringLiteral("looper.recording.tail-silence"))
                .value(QStringLiteral("value")).toInt() != originalTail) {
            fail(QStringLiteral("Arm Lane Recording Cancel leaked its transient draft"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("cancel-reopened-arm-recording"),
            QStringLiteral("looper.recording-dialog.cancel"));
        openModal(
            peer,
            peerPrefix + QStringLiteral("open-remove-recording-lane"),
            QStringLiteral("looper.lane.0.remove"));
        Snapshot removeLane = snapshotAll(
            peer, peerPrefix + QStringLiteral("remove-recording-lane"));
        requireControls(removeLane, QStringLiteral("Remove lane"), {
            "looper.lane-remove-dialog.remove-only",
            "looper.lane-remove-dialog.delete-wav",
            "looper.lane-remove-dialog.cancel",
        });
        if (controlState(removeLane, QStringLiteral("looper.lane-remove-dialog.delete-wav"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral("Delete WAV was enabled for an empty lane"));
        }
        closeModal(
            peer,
            peerPrefix + QStringLiteral("remove-recording-lane-only"),
            QStringLiteral("looper.lane-remove-dialog.remove-only"));
    }
    }

    if (!startupOnly &&
        !exerciseLocalFakeAudioWorkflow(coordinator.peer(0))) {
        return 1;
    }

    if (startupOnly) {
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        const QString id = QStringLiteral("prepare-dialog-runtime-%1").arg(index + 1);
        if (!send(coordinator.peer(index), {
                {QStringLiteral("type"),
                    QStringLiteral("jam.dialog-runtime.prepare")},
                {QStringLiteral("id"), id},
                {QStringLiteral("test_input"), QStringLiteral("tone-440")},
            }) || !receiveApplied(coordinator.peer(index), id)) {
            return 1;
        }
    }
    portReservations.release();

    auto& creator = coordinator.peer(0);
    openModal(creator, QStringLiteral("real-create-open"),
        QStringLiteral("session.start"));
    (void)invokeAndReceive(creator, QStringLiteral("real-create-no-stun"),
        QStringLiteral("session.dialog.no-stun"), QStringLiteral("set-checked"), true);
    (void)invokeAndReceive(creator, QStringLiteral("real-create-bind"),
        QStringLiteral("session.dialog.bind-host"), QStringLiteral("set-text"),
        QStringLiteral("127.0.0.1"));
    (void)invokeAndReceive(creator, QStringLiteral("real-create-public"),
        QStringLiteral("session.dialog.public-host"), QStringLiteral("set-text"),
        QStringLiteral("127.0.0.1"));
    (void)invokeAndReceive(creator, QStringLiteral("real-create-port"),
        QStringLiteral("session.dialog.port"), QStringLiteral("set-value"),
        static_cast<int>(ports[0]));
    (void)invokeAndReceive(creator, QStringLiteral("real-create-inputs"),
        QStringLiteral("session.dialog.input-channels"),
        QStringLiteral("set-text"), QStringLiteral("1,2"));
    (void)invokeAndReceive(creator, QStringLiteral("real-create-peer-limit"),
        QStringLiteral("session.dialog.maximum-peers"),
        QStringLiteral("set-value"), 4);
    (void)invokeAndReceive(creator, QStringLiteral("real-create-diagnostics"),
        QStringLiteral("session.dialog.diagnostics"),
        QStringLiteral("set-checked"), false);
    closeModal(creator, QStringLiteral("real-create-accept"),
        QStringLiteral("session.start-dialog.accept"));

    const auto creatorState = runtimeSnapshot(
        creator, QStringLiteral("real-create-state"));
    if (!creatorState) return 1;
    const QJsonObject creatorJam = creatorState->value(
        QStringLiteral("jam")).toObject();
    const QString invite = creatorJam.value(QStringLiteral("invite_url")).toString();
    if (creatorJam.value(QStringLiteral("role")).toString() !=
            QStringLiteral("creator") || invite.isEmpty()) {
        fail(QStringLiteral("real Start Jam accept did not create an invite-bearing session"));
        return 1;
    }

    for (std::size_t peer = 1; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        auto& joiner = coordinator.peer(peer);
        const QString prefix = QStringLiteral("real-join-%1").arg(peer + 1);
        openModal(joiner, prefix + QStringLiteral("-open"),
            QStringLiteral("session.join"));
        (void)invokeAndReceive(joiner, prefix + QStringLiteral("-bind"),
            QStringLiteral("session.dialog.bind-host"),
            QStringLiteral("set-text"), QStringLiteral("127.0.0.1"));
        (void)invokeAndReceive(joiner, prefix + QStringLiteral("-port"),
            QStringLiteral("session.dialog.port"), QStringLiteral("set-value"),
            static_cast<int>(ports[peer]));
        (void)invokeAndReceive(joiner, prefix + QStringLiteral("-inputs"),
            QStringLiteral("session.dialog.input-channels"),
            QStringLiteral("set-text"), QStringLiteral("1,2"));
        (void)invokeAndReceive(joiner, prefix + QStringLiteral("-invite"),
            QStringLiteral("session.dialog.invite-url"),
            QStringLiteral("set-text"), invite);
        (void)invokeAndReceive(joiner, prefix + QStringLiteral("-diagnostics"),
            QStringLiteral("session.dialog.diagnostics"),
            QStringLiteral("set-checked"), false);
        closeModal(joiner, prefix + QStringLiteral("-accept"),
            QStringLiteral("session.join-dialog.accept"));
    }

    std::array<QJsonObject, FourPeerCoordinator::kPeerCount> runtimeStates;
    if (!waitForAll(coordinator, QStringLiteral(
            "four-peer jam created and joined through real dialogs"),
            [](std::size_t peer, const QJsonObject& state) {
                const QJsonObject jam = state.value(
                    QStringLiteral("jam")).toObject();
                const QJsonObject performance = state.value(
                    QStringLiteral("performance")).toObject();
                return jam.value(QStringLiteral("role")).toString() ==
                        (peer == 0 ? QStringLiteral("creator") :
                            QStringLiteral("joiner")) &&
                    jam.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
                    jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
                    jam.value(QStringLiteral("network_attachment_ready")).toBool() &&
                    jam.value(QStringLiteral("network_running")).toBool() &&
                    jam.value(QStringLiteral("failure")).toString().isEmpty() &&
                    performance.value(QStringLiteral("engine_running")).toBool() &&
                    performance.value(QStringLiteral("network_running")).toBool() &&
                    performance.value(QStringLiteral("headless_audio")).toBool() &&
                    performance.value(QStringLiteral("test_input")).toString() ==
                        QStringLiteral("tone-440") &&
                    performance.value(QStringLiteral("callbacks")).toInteger() > 0;
            }, runtimeStates)) {
        return 1;
    }

    if (!exerciseActiveCreatorWorkflows(coordinator, runtimeStates)) {
        return 1;
    }

    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        auto& process = coordinator.peer(peer);
        const QString prefix = QStringLiteral("input-routing-%1").arg(peer + 1);
        const QString source0Include = QStringLiteral(
            "performance.audio-input-dialog.source.0.include");
        const QString source0Level = QStringLiteral(
            "performance.audio-input-dialog.source.0.level");
        const QString source1Include = QStringLiteral(
            "performance.audio-input-dialog.source.1.include");
        const QString source1Level = QStringLiteral(
            "performance.audio-input-dialog.source.1.level");

        openModal(process, prefix + QStringLiteral("-audio-open"),
            QStringLiteral("performance.audio-inputs"));
        Snapshot audio = snapshotAll(
            process, prefix + QStringLiteral("-audio-initial"));
        requireControls(audio, QStringLiteral("Audio Inputs"), {
            "performance.audio-input-dialog.source.0.include",
            "performance.audio-input-dialog.source.0.level",
            "performance.audio-input-dialog.source.1.include",
            "performance.audio-input-dialog.source.1.level",
            "performance.audio-input-dialog.pair.left",
            "performance.audio-input-dialog.pair.right",
            "performance.audio-input-dialog.pair.create",
            "performance.audio-input-dialog.close",
        });
        const QJsonObject initialRouter = audio.performance
            .value(QStringLiteral("input_source_router")).toObject();
        const QJsonObject initialSlot0 = inputSourceSlot(audio, 0);
        const QJsonObject initialSlot1 = inputSourceSlot(audio, 1);
        if (!initialRouter.value(QStringLiteral("available")).toBool() ||
            initialRouter.value(QStringLiteral("physical_channels")).toInt() != 2 ||
            initialRouter.value(QStringLiteral("configured_sources")).toInt() != 2 ||
            initialRouter.value(QStringLiteral("rendered_blocks")).toInteger() <= 0 ||
            initialRouter.value(QStringLiteral("peak_ppm")).toInt() <= 0 ||
            initialRouter.value(QStringLiteral("invalid_configurations")).toInteger() != 0 ||
            !initialSlot0.value(QStringLiteral("configured")).toBool() ||
            !initialSlot1.value(QStringLiteral("configured")).toBool() ||
            initialSlot0.value(QStringLiteral("first_channel")).toInt(-1) != 0 ||
            initialSlot0.value(QStringLiteral("second_channel")).toInt(-2) != -1 ||
            initialSlot1.value(QStringLiteral("first_channel")).toInt(-1) != 1 ||
            initialSlot1.value(QStringLiteral("second_channel")).toInt(-2) != -1) {
            fail(QStringLiteral(
                "Audio Inputs did not expose a healthy two-source live router"));
            std::cerr << "  router="
                      << QJsonDocument(initialRouter).toJson(QJsonDocument::Compact)
                            .toStdString()
                      << '\n';
        }
        const bool originalIncluded = controlState(audio, source0Include)
            .value(QStringLiteral("checked")).toBool();
        const int originalLevel = controlState(audio, source0Level)
            .value(QStringLiteral("value")).toInt(-1);
        const int changedLevel = originalLevel == 73 ? 74 : 73;
        if (controlState(audio, QStringLiteral(
                "performance.audio-input-dialog.pair.left"))
                .value(QStringLiteral("count")).toInt() != 2 ||
            controlState(audio, QStringLiteral(
                "performance.audio-input-dialog.pair.right"))
                .value(QStringLiteral("count")).toInt() != 2 ||
            !controlState(audio, QStringLiteral(
                "performance.audio-input-dialog.pair.create"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral("Audio Inputs did not expose two pairable fake inputs"));
        }
        (void)invokeAndReceive(
            process, prefix + QStringLiteral("-audio-include"),
            source0Include, QStringLiteral("set-checked"), !originalIncluded);
        (void)invokeAndReceive(
            process, prefix + QStringLiteral("-audio-level"),
            source0Level, QStringLiteral("set-value"), changedLevel);
        closeModal(process, prefix + QStringLiteral("-audio-close"),
            QStringLiteral("performance.audio-input-dialog.close"));

        openModal(process, prefix + QStringLiteral("-audio-reopen"),
            QStringLiteral("performance.audio-inputs"));
        audio = snapshotAll(process, prefix + QStringLiteral("-audio-persisted"));
        if (controlState(audio, source0Include)
                .value(QStringLiteral("checked")).toBool() == originalIncluded ||
            controlState(audio, source0Level)
                .value(QStringLiteral("value")).toInt(-1) != changedLevel ||
            inputSourceSlot(audio, 0).value(QStringLiteral("enabled")).toBool() ==
                originalIncluded ||
            inputSourceSlot(audio, 0).value(QStringLiteral("level_ppm")).toInt() !=
                changedLevel * 10000) {
            fail(QStringLiteral(
                "Audio Inputs did not apply and retain live router edits after Close/reopen"));
        }
        (void)invokeAndReceive(
            process, prefix + QStringLiteral("-audio-restore-include"),
            source0Include, QStringLiteral("set-checked"), originalIncluded);
        (void)invokeAndReceive(
            process, prefix + QStringLiteral("-audio-restore-level"),
            source0Level, QStringLiteral("set-value"), originalLevel);
        (void)invokeAndReceive(
            process, prefix + QStringLiteral("-audio-pair"),
            QStringLiteral("performance.audio-input-dialog.pair.create"),
            QStringLiteral("click"));
        audio = snapshotAll(process, prefix + QStringLiteral("-audio-grouped"));
        requireControls(audio, QStringLiteral("Grouped Audio Inputs"), {
            "performance.audio-input-dialog.source.0.include",
            "performance.audio-input-dialog.source.0.level",
            "performance.audio-input-dialog.source.0.ungroup",
            "performance.audio-input-dialog.pair.left",
            "performance.audio-input-dialog.pair.right",
            "performance.audio-input-dialog.pair.create",
            "performance.audio-input-dialog.close",
        });
        if (audio.controls.contains(source1Include) ||
            audio.controls.contains(source1Level) ||
            controlState(audio, QStringLiteral(
                "performance.audio-input-dialog.pair.create"))
                .value(QStringLiteral("enabled")).toBool() ||
            audio.performance.value(QStringLiteral("input_source_router")).toObject()
                .value(QStringLiteral("configured_sources")).toInt() != 1 ||
            inputSourceSlot(audio, 0).value(QStringLiteral("second_channel"))
                .toInt(-1) != 1 ||
            inputSourceSlot(audio, 1).value(QStringLiteral("configured")).toBool()) {
            fail(QStringLiteral(
                "Audio Inputs stereo grouping retained a second live router source"));
        }
        (void)invokeAndReceive(
            process, prefix + QStringLiteral("-audio-ungroup"),
            QStringLiteral("performance.audio-input-dialog.source.0.ungroup"),
            QStringLiteral("click"));
        audio = snapshotAll(process, prefix + QStringLiteral("-audio-ungrouped"));
        requireControls(audio, QStringLiteral("Ungrouped Audio Inputs"), {
            "performance.audio-input-dialog.source.0.include",
            "performance.audio-input-dialog.source.0.level",
            "performance.audio-input-dialog.source.1.include",
            "performance.audio-input-dialog.source.1.level",
            "performance.audio-input-dialog.pair.create",
            "performance.audio-input-dialog.close",
        });
        if (audio.performance.value(QStringLiteral("input_source_router")).toObject()
                .value(QStringLiteral("configured_sources")).toInt() != 2 ||
            inputSourceSlot(audio, 0).value(QStringLiteral("second_channel"))
                .toInt(-2) != -1 ||
            !inputSourceSlot(audio, 1).value(QStringLiteral("configured")).toBool() ||
            inputSourceSlot(audio, 1).value(QStringLiteral("first_channel"))
                .toInt(-1) != 1) {
            fail(QStringLiteral(
                "Audio Inputs ungroup did not restore two live router sources"));
        }
        closeModal(process, prefix + QStringLiteral("-audio-final-close"),
            QStringLiteral("performance.audio-input-dialog.close"));

        openModal(process, prefix + QStringLiteral("-midi-open"),
            QStringLiteral("performance.midi-inputs"));
        Snapshot midi = snapshotAll(
            process, prefix + QStringLiteral("-midi"));
        requireControls(midi, QStringLiteral("MIDI Inputs"), {
            "performance.midi-input-dialog.add",
            "performance.midi-input-dialog.close",
        });
        if (!controlState(midi, QStringLiteral(
                "performance.midi-input-dialog.add"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral("MIDI input assignment was unexpectedly locked"));
        }
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-discovery-cancel-open"),
            QStringLiteral("performance.midi-input-dialog.add"),
            QStringLiteral("click"));
        Snapshot midiDiscovery = waitForControl(
            process, prefix + QStringLiteral("-midi-discovery"),
            QStringLiteral("performance.midi-discovery.cancel"));
        requireControls(midiDiscovery, QStringLiteral("MIDI Discovery"), {
            "performance.midi-discovery.cancel",
        });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-discovery-cancel"),
            QStringLiteral("performance.midi-discovery.cancel"),
            QStringLiteral("click"));
        midi = waitForControl(
            process, prefix + QStringLiteral("-midi-discovery-cancelled"),
            QStringLiteral("performance.midi-input-dialog.add"));
        QThread::msleep(1100);
        midi = snapshotAll(
            process, prefix + QStringLiteral("-midi-discovery-late-check"));
        if (!midi.performance.value(QStringLiteral("midi_input_sources"))
                .toArray().isEmpty() ||
            midi.controls.contains(QStringLiteral(
                "performance.midi-add-dialog.accept"))) {
            fail(QStringLiteral(
                "cancelled MIDI discovery applied a late completion"));
        }
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-add-cancel-open"),
            QStringLiteral("performance.midi-input-dialog.add"),
            QStringLiteral("click"));
        Snapshot midiAdd = waitForControl(
            process, prefix + QStringLiteral("-midi-add-cancel"),
            QStringLiteral("performance.midi-add-dialog.accept"));
        midiAdd = snapshotAll(
            process, prefix + QStringLiteral("-midi-add-cancel-state"));
        requireControls(midiAdd, QStringLiteral("Add MIDI Device Cancel"), {
            "performance.midi-add-dialog.device",
            "performance.midi-add-dialog.mode",
            "performance.midi-add-dialog.accept",
            "performance.midi-add-dialog.cancel",
        });
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-cancel-device"),
            QStringLiteral("performance.midi-add-dialog.device"),
            QStringLiteral("set-current-row"), 1);
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-cancel-mode"),
            QStringLiteral("performance.midi-add-dialog.mode"),
            QStringLiteral("set-index"), 1);
        closeModal(process, prefix + QStringLiteral("-midi-add-cancel"),
            QStringLiteral("performance.midi-add-dialog.cancel"));
        midi = waitForControl(
            process, prefix + QStringLiteral("-midi-after-cancel"),
            QStringLiteral("performance.midi-input-dialog.add"));
        if (!midi.performance.value(QStringLiteral("midi_input_sources"))
                .toArray().isEmpty()) {
            fail(QStringLiteral("cancelled MIDI assignment created a source"));
        }

        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-add-open"),
            QStringLiteral("performance.midi-input-dialog.add"),
            QStringLiteral("click"));
        midiAdd = waitForControl(
            process, prefix + QStringLiteral("-midi-add"),
            QStringLiteral("performance.midi-add-dialog.accept"));
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-add-device"),
            QStringLiteral("performance.midi-add-dialog.device"),
            QStringLiteral("set-current-row"), static_cast<int>(peer % 2));
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-add-mode"),
            QStringLiteral("performance.midi-add-dialog.mode"),
            QStringLiteral("set-index"), 1);
        closeModal(process, prefix + QStringLiteral("-midi-add-accept"),
            QStringLiteral("performance.midi-add-dialog.accept"));

        const QString midiSourcePrefix = QStringLiteral(
            "performance.midi-input-dialog.source.2");
        midi = waitForControl(
            process, prefix + QStringLiteral("-midi-assigned"),
            midiSourcePrefix + QStringLiteral(".mode"));
        midi = snapshotAll(
            process, prefix + QStringLiteral("-midi-assigned-state"));
        requireControls(midi, QStringLiteral("Assigned MIDI Input"), {
            "performance.midi-input-dialog.source.2.mode",
            "performance.midi-input-dialog.source.2.include",
            "performance.midi-input-dialog.source.2.level",
            "performance.midi-input-dialog.source.2.remove",
            "performance.midi-input-dialog.add",
            "performance.midi-input-dialog.close",
        });
        const QJsonArray assignedSources = midi.performance
            .value(QStringLiteral("midi_input_sources")).toArray();
        const QString expectedDeviceId = QStringLiteral("automation-midi-%1")
            .arg(peer % 2);
        if (assignedSources.size() != 1 ||
            assignedSources.at(0).toObject().value(QStringLiteral("device_id"))
                .toString() != expectedDeviceId ||
            assignedSources.at(0).toObject().value(QStringLiteral("router_slot"))
                .toInt(-1) != 2 ||
            assignedSources.at(0).toObject().value(QStringLiteral("mode"))
                .toString() != QStringLiteral("mpe") ||
            assignedSources.at(0).toObject().value(QStringLiteral("device_open"))
                .toBool() ||
            assignedSources.at(0).toObject().value(QStringLiteral("plugin_loaded"))
                .toBool()) {
            fail(QStringLiteral("assigned MIDI source state did not match its fake device"));
        }

        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-standard"),
            midiSourcePrefix + QStringLiteral(".mode"),
            QStringLiteral("activate-index"), 0);
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-include"),
            midiSourcePrefix + QStringLiteral(".include"),
            QStringLiteral("set-checked"), false);
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-level"),
            midiSourcePrefix + QStringLiteral(".level"),
            QStringLiteral("set-value"), 37);
        closeModal(process, prefix + QStringLiteral("-midi-close"),
            QStringLiteral("performance.midi-input-dialog.close"));

        openModal(process, prefix + QStringLiteral("-midi-reopen"),
            QStringLiteral("performance.midi-inputs"));
        midi = waitForControl(
            process, prefix + QStringLiteral("-midi-persisted"),
            midiSourcePrefix + QStringLiteral(".mode"));
        midi = snapshotAll(
            process, prefix + QStringLiteral("-midi-persisted-state"));
        const QJsonObject persistedSource = midi.performance
            .value(QStringLiteral("midi_input_sources")).toArray()
            .at(0).toObject();
        if (controlState(midi, midiSourcePrefix + QStringLiteral(".mode"))
                .value(QStringLiteral("index")).toInt(-1) != 0 ||
            controlState(midi, midiSourcePrefix + QStringLiteral(".include"))
                .value(QStringLiteral("checked")).toBool() ||
            controlState(midi, midiSourcePrefix + QStringLiteral(".level"))
                .value(QStringLiteral("value")).toInt(-1) != 37 ||
            persistedSource.value(QStringLiteral("mode")).toString() !=
                QStringLiteral("standard") ||
            persistedSource.value(QStringLiteral("included")).toBool() ||
            persistedSource.value(QStringLiteral("level_ppm")).toInt(-1) != 370000) {
            fail(QStringLiteral("MIDI source edits did not persist across reopen"));
        }
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-include-for-plugin"),
            midiSourcePrefix + QStringLiteral(".include"),
            QStringLiteral("set-checked"), true);
        closeModal(process, prefix + QStringLiteral("-midi-persisted-close"),
            QStringLiteral("performance.midi-input-dialog.close"));

        openModal(process, prefix + QStringLiteral("-plugins-open"),
            QStringLiteral("performance.plugins"));
        Snapshot plugins = snapshotAll(
            process, prefix + QStringLiteral("-plugins"));
        requireControls(plugins, QStringLiteral("Input Plugins"), {
            "performance.plugin-dialog.audio.0.open",
            "performance.plugin-dialog.audio.0.load",
            "performance.plugin-dialog.audio.0.bypass",
            "performance.plugin-dialog.audio.0.remove",
            "performance.plugin-dialog.audio.1.open",
            "performance.plugin-dialog.audio.1.load",
            "performance.plugin-dialog.audio.1.bypass",
            "performance.plugin-dialog.audio.1.remove",
            "performance.plugin-dialog.midi.2.open",
            "performance.plugin-dialog.midi.2.load",
            "performance.plugin-dialog.midi.2.bypass",
            "performance.plugin-dialog.midi.2.remove",
            "performance.plugin-dialog.close",
        });
        for (int source = 0; source < 2; ++source) {
            const QString pluginPrefix = QStringLiteral(
                "performance.plugin-dialog.audio.%1.").arg(source);
            if (controlState(plugins, pluginPrefix + QStringLiteral("open"))
                    .value(QStringLiteral("enabled")).toBool() ||
                !controlState(plugins, pluginPrefix + QStringLiteral("load"))
                    .value(QStringLiteral("enabled")).toBool() ||
                controlState(plugins, pluginPrefix + QStringLiteral("bypass"))
                    .value(QStringLiteral("enabled")).toBool() ||
                controlState(plugins, pluginPrefix + QStringLiteral("remove"))
                    .value(QStringLiteral("enabled")).toBool()) {
                fail(QStringLiteral(
                    "Input Plugins exposed the wrong empty-source action state"));
            }
        }
        const QString midiPluginPrefix = QStringLiteral(
            "performance.plugin-dialog.midi.2.");
        if (controlState(plugins, midiPluginPrefix + QStringLiteral("open"))
                .value(QStringLiteral("enabled")).toBool() ||
            !controlState(plugins, midiPluginPrefix + QStringLiteral("load"))
                .value(QStringLiteral("enabled")).toBool() ||
            controlState(plugins, midiPluginPrefix + QStringLiteral("bypass"))
                .value(QStringLiteral("enabled")).toBool() ||
            controlState(plugins, midiPluginPrefix + QStringLiteral("remove"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral(
                "Input Plugins exposed the wrong uninstrumented MIDI action state"));
        }

        const QString audioPluginPrefix = QStringLiteral(
            "performance.plugin-dialog.audio.0.");
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-load"),
            audioPluginPrefix + QStringLiteral("load"), QStringLiteral("click"));
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-concurrent-load"),
            QStringLiteral("performance.plugin-dialog.audio.1.load"),
            QStringLiteral("click"));
        plugins = waitForControlState(process,
            prefix + QStringLiteral("-audio-plugin-loaded"),
            audioPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                const QJsonObject plugin = inputPlugin(
                    performance, QStringLiteral("audio"), 0);
                return state.value(QStringLiteral("enabled")).toBool() &&
                    plugin.value(QStringLiteral("loaded")).toBool() &&
                    plugin.value(QStringLiteral("healthy")).toBool();
            });
        plugins = snapshotAll(
            process, prefix + QStringLiteral("-audio-plugin-loaded-state"));
        QJsonObject audioPlugin = inputPlugin(
            plugins, QStringLiteral("audio"), 0);
        if (audioPlugin.value(QStringLiteral("name")).toString().isEmpty() ||
            inputPlugin(plugins, QStringLiteral("audio"), 1)
                .value(QStringLiteral("loaded")).toBool() ||
            !inputSourceSlot(plugins, 0).value(
                QStringLiteral("renderer_attached")).toBool() ||
            !controlState(plugins, audioPluginPrefix + QStringLiteral("load"))
                .value(QStringLiteral("enabled")).toBool() ||
            !controlState(plugins, audioPluginPrefix + QStringLiteral("bypass"))
                .value(QStringLiteral("enabled")).toBool() ||
            !controlState(plugins, audioPluginPrefix + QStringLiteral("remove"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral("audio plugin did not attach to its live source"));
        }
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-open"),
            audioPluginPrefix + QStringLiteral("open"), QStringLiteral("click"));
        plugins = waitForControlState(process,
            prefix + QStringLiteral("-audio-plugin-editor"),
            audioPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject&, const QJsonObject& performance) {
                return inputPlugin(performance, QStringLiteral("audio"), 0)
                    .value(QStringLiteral("editor_open")).toBool();
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-bypass"),
            audioPluginPrefix + QStringLiteral("bypass"),
            QStringLiteral("set-checked"), true);
        (void)waitForControlState(process,
            prefix + QStringLiteral("-audio-plugin-bypassed"),
            audioPluginPrefix + QStringLiteral("bypass"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                return state.value(QStringLiteral("checked")).toBool() &&
                    inputPlugin(performance, QStringLiteral("audio"), 0)
                        .value(QStringLiteral("bypassed")).toBool();
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-unbypass"),
            audioPluginPrefix + QStringLiteral("bypass"),
            QStringLiteral("set-checked"), false);
        const QString firstAudioPlugin = audioPlugin
            .value(QStringLiteral("name")).toString();
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-replace"),
            audioPluginPrefix + QStringLiteral("load"), QStringLiteral("click"));
        plugins = waitForControlState(process,
            prefix + QStringLiteral("-audio-plugin-replaced"),
            audioPluginPrefix + QStringLiteral("open"),
            [&firstAudioPlugin](const QJsonObject& state,
                                const QJsonObject& performance) {
                const QJsonObject plugin = inputPlugin(
                    performance, QStringLiteral("audio"), 0);
                return state.value(QStringLiteral("enabled")).toBool() &&
                    plugin.value(QStringLiteral("loaded")).toBool() &&
                    plugin.value(QStringLiteral("name")).toString() != firstAudioPlugin;
            });

        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-plugin-load"),
            midiPluginPrefix + QStringLiteral("load"), QStringLiteral("click"));
        plugins = waitForControlState(process,
            prefix + QStringLiteral("-midi-plugin-loaded"),
            midiPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                const QJsonObject plugin = inputPlugin(
                    performance, QStringLiteral("midi-instrument"), 2);
                return state.value(QStringLiteral("enabled")).toBool() &&
                    plugin.value(QStringLiteral("loaded")).toBool() &&
                    plugin.value(QStringLiteral("healthy")).toBool() &&
                    plugin.value(QStringLiteral("device_open")).toBool();
            });
        plugins = snapshotAll(
            process, prefix + QStringLiteral("-midi-plugin-loaded-state"));
        QJsonObject midiPlugin = inputPlugin(
            plugins, QStringLiteral("midi-instrument"), 2);
        if (midiPlugin.value(QStringLiteral("name")).toString().isEmpty() ||
            !inputSourceSlot(plugins, 2).value(QStringLiteral("configured")).toBool() ||
            !inputSourceSlot(plugins, 2).value(
                QStringLiteral("renderer_attached")).toBool()) {
            fail(QStringLiteral("MIDI plugin/device did not attach to its live source"));
        }
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-plugin-open"),
            midiPluginPrefix + QStringLiteral("open"), QStringLiteral("click"));
        (void)waitForControlState(process,
            prefix + QStringLiteral("-midi-plugin-editor"),
            midiPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject&, const QJsonObject& performance) {
                return inputPlugin(performance,
                    QStringLiteral("midi-instrument"), 2)
                    .value(QStringLiteral("editor_open")).toBool();
            });
        const QString injectId = prefix + QStringLiteral("-midi-inject");
        if (send(process, {
                {QStringLiteral("type"), QStringLiteral("midi.inject")},
                {QStringLiteral("id"), injectId},
                {QStringLiteral("device"), expectedDeviceId},
                {QStringLiteral("status"), 0x90},
                {QStringLiteral("data1"), 60 + static_cast<int>(peer)},
                {QStringLiteral("data2"), 100},
                {QStringLiteral("size"), 3},
            })) {
            (void)receiveApplied(process, injectId);
        }
        (void)waitForControlState(process,
            prefix + QStringLiteral("-midi-plugin-rendered-event"),
            midiPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject&, const QJsonObject& performance) {
                return inputPlugin(performance,
                    QStringLiteral("midi-instrument"), 2)
                    .value(QStringLiteral("stats")).toObject()
                    .value(QStringLiteral("midi_events_consumed")).toInteger() >= 1;
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-plugin-bypass"),
            midiPluginPrefix + QStringLiteral("bypass"),
            QStringLiteral("set-checked"), true);
        (void)waitForControlState(process,
            prefix + QStringLiteral("-midi-plugin-bypassed"),
            midiPluginPrefix + QStringLiteral("bypass"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                return state.value(QStringLiteral("checked")).toBool() &&
                    inputPlugin(performance, QStringLiteral("midi-instrument"), 2)
                        .value(QStringLiteral("bypassed")).toBool();
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-plugin-unbypass"),
            midiPluginPrefix + QStringLiteral("bypass"),
            QStringLiteral("set-checked"), false);
        const QString firstMidiPlugin = midiPlugin
            .value(QStringLiteral("name")).toString();
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-plugin-replace"),
            midiPluginPrefix + QStringLiteral("load"), QStringLiteral("click"));
        (void)waitForControlState(process,
            prefix + QStringLiteral("-midi-plugin-replaced"),
            midiPluginPrefix + QStringLiteral("open"),
            [&firstMidiPlugin](const QJsonObject& state,
                               const QJsonObject& performance) {
                const QJsonObject plugin = inputPlugin(
                    performance, QStringLiteral("midi-instrument"), 2);
                return state.value(QStringLiteral("enabled")).toBool() &&
                    plugin.value(QStringLiteral("device_open")).toBool() &&
                    plugin.value(QStringLiteral("name")).toString() != firstMidiPlugin;
            });

        closeModal(process, prefix + QStringLiteral("-plugins-close-for-global"),
            QStringLiteral("performance.plugin-dialog.close"));
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-plugins-global-bypass"),
            QStringLiteral("performance.plugin-bypass"),
            QStringLiteral("set-checked"), true);
        (void)waitForControlState(process,
            prefix + QStringLiteral("-plugins-global-bypassed"),
            QStringLiteral("performance.plugin-bypass"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                return state.value(QStringLiteral("checked")).toBool() &&
                    inputPlugin(performance, QStringLiteral("audio"), 0)
                        .value(QStringLiteral("bypassed")).toBool() &&
                    inputPlugin(performance, QStringLiteral("midi-instrument"), 2)
                        .value(QStringLiteral("bypassed")).toBool();
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-plugins-global-unbypass"),
            QStringLiteral("performance.plugin-bypass"),
            QStringLiteral("set-checked"), false);
        (void)waitForControlState(process,
            prefix + QStringLiteral("-plugins-global-unbypassed"),
            QStringLiteral("performance.plugin-bypass"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                return !state.value(QStringLiteral("checked")).toBool() &&
                    !inputPlugin(performance, QStringLiteral("audio"), 0)
                        .value(QStringLiteral("bypassed")).toBool() &&
                    !inputPlugin(performance, QStringLiteral("midi-instrument"), 2)
                        .value(QStringLiteral("bypassed")).toBool();
            });

        openModal(process, prefix + QStringLiteral("-plugins-remove-open"),
            QStringLiteral("performance.plugins"));
        (void)waitForControlState(process,
            prefix + QStringLiteral("-plugins-remove-ready"),
            audioPluginPrefix + QStringLiteral("remove"),
            [](const QJsonObject& state, const QJsonObject&) {
                return state.value(QStringLiteral("enabled")).toBool();
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-remove"),
            audioPluginPrefix + QStringLiteral("remove"), QStringLiteral("click"));
        (void)waitForControlState(process,
            prefix + QStringLiteral("-audio-plugin-removed"),
            audioPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                return !state.value(QStringLiteral("enabled")).toBool() &&
                    !inputPlugin(performance, QStringLiteral("audio"), 0)
                        .value(QStringLiteral("loaded")).toBool();
            });
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-plugin-remove"),
            midiPluginPrefix + QStringLiteral("remove"), QStringLiteral("click"));
        plugins = waitForControlState(process,
            prefix + QStringLiteral("-midi-plugin-removed"),
            midiPluginPrefix + QStringLiteral("open"),
            [](const QJsonObject& state, const QJsonObject& performance) {
                const QJsonObject plugin = inputPlugin(
                    performance, QStringLiteral("midi-instrument"), 2);
                return !state.value(QStringLiteral("enabled")).toBool() &&
                    !plugin.value(QStringLiteral("loaded")).toBool() &&
                    !plugin.value(QStringLiteral("device_open")).toBool();
            });
        if (inputSourceSlot(plugins, 0).value(
                QStringLiteral("renderer_attached")).toBool() ||
            inputSourceSlot(plugins, 2).value(QStringLiteral("configured")).toBool()) {
            fail(QStringLiteral("removed input plugins retained live renderer topology"));
        }

        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-plugin-late-group-load"),
            QStringLiteral("performance.plugin-dialog.audio.1.load"),
            QStringLiteral("click"));
        closeModal(process, prefix + QStringLiteral("-plugins-late-group-close"),
            QStringLiteral("performance.plugin-dialog.close"));
        openModal(process, prefix + QStringLiteral("-audio-late-group-open"),
            QStringLiteral("performance.audio-inputs"));
        (void)waitForControl(process,
            prefix + QStringLiteral("-audio-late-group-ready"),
            QStringLiteral("performance.audio-input-dialog.pair.create"));
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-late-group"),
            QStringLiteral("performance.audio-input-dialog.pair.create"),
            QStringLiteral("click"));
        QThread::msleep(1100);
        Snapshot lateGrouped = snapshotAll(
            process, prefix + QStringLiteral("-audio-late-grouped-state"));
        if (inputPlugin(lateGrouped, QStringLiteral("audio"), 0)
                .value(QStringLiteral("loaded")).toBool() ||
            lateGrouped.performance.value(QStringLiteral("input_source_router"))
                .toObject().value(QStringLiteral("configured_sources")).toInt() != 1 ||
            inputSourceSlot(lateGrouped, 0).value(
                QStringLiteral("renderer_attached")).toBool()) {
            fail(QStringLiteral(
                "late audio plugin completion attached after source grouping changed"));
        }
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-audio-late-ungroup"),
            QStringLiteral("performance.audio-input-dialog.source.0.ungroup"),
            QStringLiteral("click"));
        closeModal(process, prefix + QStringLiteral("-audio-late-group-close"),
            QStringLiteral("performance.audio-input-dialog.close"));

        openModal(process, prefix + QStringLiteral("-midi-late-plugin-open"),
            QStringLiteral("performance.plugins"));
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-late-plugin-load"),
            midiPluginPrefix + QStringLiteral("load"), QStringLiteral("click"));
        closeModal(process, prefix + QStringLiteral("-midi-late-plugin-close"),
            QStringLiteral("performance.plugin-dialog.close"));

        openModal(process, prefix + QStringLiteral("-midi-remove-open"),
            QStringLiteral("performance.midi-inputs"));
        (void)waitForControl(process, prefix + QStringLiteral("-midi-remove-ready"),
            midiSourcePrefix + QStringLiteral(".remove"));
        (void)invokeAndReceive(process, prefix + QStringLiteral("-midi-remove"),
            midiSourcePrefix + QStringLiteral(".remove"), QStringLiteral("click"));
        midi = waitForControlAbsent(
            process, prefix + QStringLiteral("-midi-removed"),
            midiSourcePrefix + QStringLiteral(".mode"));
        if (!midi.performance.value(QStringLiteral("midi_input_sources"))
                .toArray().isEmpty() ||
            inputSourceSlot(midi, 2).value(QStringLiteral("configured")).toBool()) {
            fail(QStringLiteral("removed MIDI source retained state or router topology"));
        }
        QThread::msleep(1100);
        midi = snapshotAll(
            process, prefix + QStringLiteral("-midi-late-plugin-state"));
        if (!midi.performance.value(QStringLiteral("midi_input_sources"))
                .toArray().isEmpty() ||
            !inputPlugin(midi, QStringLiteral("midi-instrument"), 2).isEmpty() ||
            inputSourceSlot(midi, 2).value(QStringLiteral("configured")).toBool()) {
            fail(QStringLiteral(
                "late MIDI plugin/device completion survived source removal"));
        }

        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-reuse-open"),
            QStringLiteral("performance.midi-input-dialog.add"),
            QStringLiteral("click"));
        (void)waitForControl(process,
            prefix + QStringLiteral("-midi-reuse-add"),
            QStringLiteral("performance.midi-add-dialog.accept"));
        const int reusedDevice = static_cast<int>((peer + 1) % 2);
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-reuse-device"),
            QStringLiteral("performance.midi-add-dialog.device"),
            QStringLiteral("set-current-row"), reusedDevice);
        closeModal(process, prefix + QStringLiteral("-midi-reuse-accept"),
            QStringLiteral("performance.midi-add-dialog.accept"));
        (void)waitForControl(process,
            prefix + QStringLiteral("-midi-reused"),
            midiSourcePrefix + QStringLiteral(".mode"));
        midi = snapshotAll(
            process, prefix + QStringLiteral("-midi-reused-state"));
        const QJsonArray reusedSources = midi.performance
            .value(QStringLiteral("midi_input_sources")).toArray();
        if (reusedSources.size() != 1 ||
            reusedSources.at(0).toObject().value(QStringLiteral("device_id"))
                .toString() != QStringLiteral("automation-midi-%1").arg(reusedDevice) ||
            reusedSources.at(0).toObject().value(QStringLiteral("router_slot"))
                .toInt(-1) != 2 ||
            reusedSources.at(0).toObject().value(QStringLiteral("mode"))
                .toString() != QStringLiteral("standard") ||
            controlState(midi, midiSourcePrefix + QStringLiteral(".mode"))
                .value(QStringLiteral("index")).toInt(-1) != 0) {
            fail(QStringLiteral(
                "reassigned MIDI source did not safely reuse the first free slot"));
        }
        (void)invokeAndReceive(process,
            prefix + QStringLiteral("-midi-reuse-remove"),
            midiSourcePrefix + QStringLiteral(".remove"), QStringLiteral("click"));
        midi = waitForControlAbsent(
            process, prefix + QStringLiteral("-midi-reuse-removed"),
            midiSourcePrefix + QStringLiteral(".mode"));
        if (!midi.performance.value(QStringLiteral("midi_input_sources"))
                .toArray().isEmpty() ||
            inputSourceSlot(midi, 2).value(QStringLiteral("configured")).toBool()) {
            fail(QStringLiteral(
                "reused MIDI source retained state or router topology after removal"));
        }
        closeModal(process, prefix + QStringLiteral("-midi-remove-close"),
            QStringLiteral("performance.midi-input-dialog.close"));
    }

    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        auto& process = coordinator.peer(peer);
        const QString prefix = QStringLiteral("active-settings-%1").arg(peer + 1);
        openModal(process, prefix + QStringLiteral("-open"),
            QStringLiteral("application.settings"));
        const Snapshot settings = snapshotAll(
            process, prefix + QStringLiteral("-state"));
        requireControls(settings, QStringLiteral("active-jam Settings"), {
            "application.settings-dialog.audio.local.sample-rate",
            "application.settings-dialog.audio.local.split-network-by-role",
            "application.settings-dialog.audio.network.input-channels",
            "application.settings-dialog.audio.create.input-channels",
            "application.settings-dialog.audio.join.input-channels",
            "application.settings-dialog.create-connection.bind-host",
            "application.settings-dialog.startup.bpm",
            "application.settings-dialog.save",
            "application.settings-dialog.cancel",
        });
        for (const char* id : {
                 "application.settings-dialog.audio.local.sample-rate",
                 "application.settings-dialog.audio.local.split-network-by-role",
                 "application.settings-dialog.audio.network.input-channels",
                 "application.settings-dialog.audio.create.input-channels",
                 "application.settings-dialog.audio.join.input-channels"}) {
            if (controlState(settings, QString::fromLatin1(id))
                    .value(QStringLiteral("enabled")).toBool()) {
                fail(QStringLiteral("active jam left Settings audio editor enabled: %1")
                    .arg(QString::fromLatin1(id)));
            }
        }
        if (!controlState(settings, QStringLiteral(
                "application.settings-dialog.create-connection.bind-host"))
                .value(QStringLiteral("enabled")).toBool() ||
            !controlState(settings, QStringLiteral(
                "application.settings-dialog.startup.bpm"))
                .value(QStringLiteral("enabled")).toBool()) {
            fail(QStringLiteral(
                "active jam disabled non-audio Settings defaults"));
        }
        closeModal(process, prefix + QStringLiteral("-cancel"),
            QStringLiteral("application.settings-dialog.cancel"));
    }

    if (!waitForAll(coordinator, QStringLiteral(
            "four-peer jam remained active after Settings cancel"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject jam = state.value(
                    QStringLiteral("jam")).toObject();
                return jam.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
                    jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
                    jam.value(QStringLiteral("network_running")).toBool();
            }, runtimeStates)) {
        return 1;
    }

    for (std::size_t peer = FourPeerCoordinator::kPeerCount; peer-- > 0;) {
        (void)invokeAndReceive(
            coordinator.peer(peer),
            QStringLiteral("real-leave-%1").arg(peer + 1),
            QStringLiteral("session.leave"),
            QStringLiteral("click"));
    }
    if (!waitForAll(coordinator, QStringLiteral("real dialog jam leave cleanup"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject jam = state.value(
                    QStringLiteral("jam")).toObject();
                const QJsonObject performance = state.value(
                    QStringLiteral("performance")).toObject();
                return jam.value(QStringLiteral("role")).toString() ==
                        QStringLiteral("inactive") &&
                    !jam.value(QStringLiteral("network_running")).toBool() &&
                    !performance.value(QStringLiteral("network_running")).toBool();
            }, runtimeStates, 20s)) {
        return 1;
    }
    }

    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        const QDir root(coordinator.storageRoot(index));
        const QString preferencesPath = root.filePath(
            QStringLiteral("config/preferences.ini"));
        const QDir logs(root.filePath(QStringLiteral("logs")));
        if (!startupOnly && !QFileInfo::exists(preferencesPath)) {
            fail(QStringLiteral(
                "peer %1 did not keep saved preferences inside its isolated storage root")
                .arg(index + 1));
        }
        if (logs.entryList({QStringLiteral("jam2_gui_*.log")}, QDir::Files).isEmpty()) {
            fail(QStringLiteral(
                "peer %1 did not keep GUI logs inside its isolated storage root")
                .arg(index + 1));
        }
    }

    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        (void)send(coordinator.peer(index), {
            {QStringLiteral("type"), QStringLiteral("shutdown")},
            {QStringLiteral("id"), QStringLiteral("shutdown-%1").arg(index + 1)},
        });
    }
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        QJsonObject applied;
        QJsonObject shutdown;
        (void)receive(coordinator.peer(index), QStringLiteral("command_applied"), applied);
        (void)receive(coordinator.peer(index), QStringLiteral("shutdown"), shutdown);
        int exitCode = -1;
        QString waitError;
        if (!coordinator.peer(index).waitForExit(20s, exitCode, waitError)) {
            fail(QStringLiteral("peer %1 exit: %2").arg(index + 1).arg(waitError));
        } else if (exitCode != 0) {
            fail(QStringLiteral("peer %1 returned %2").arg(index + 1).arg(exitCode));
        }
    }

    if (failures != 0) {
        std::cerr << failures << " four-peer GUI modal checks failed\n";
        return 1;
    }
    coordinator.markSuccessful();
    std::cout << "four-peer GUI modal checks passed\n";
    return 0;
}
