#include "BeatGridModel.hpp"
#include "GuiPresentation.hpp"
#include "MixerStatsViewModel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool approximatelyEqual(double actual, double expected, double tolerance = 0.000001)
{
    return std::abs(actual - expected) <= tolerance;
}

void testBeatGridEditing()
{
    BeatGridModel model;
    expect(model.sections().size() == 4, "beat grid starts with four sections");
    expect(model.hasOnlyPristineSection(), "new beat grid is pristine");
    expect(model.occupiedBeatCount(-1) == 0, "invalid section has no occupied beats");

    model.setTitle(QStringLiteral("  Boundary song  "));
    expect(model.title() == QStringLiteral("Boundary song"), "title is trimmed");
    model.setTitle(QStringLiteral("   "));
    expect(model.title() == QStringLiteral("Untitled Jam"), "blank title uses default");
    expect(!model.setGuitarReference(5, false), "invalid guitar string count rejects");
    expect(!model.setGuitarReference(6, false), "unchanged guitar reference is a no-op");
    expect(model.setGuitarReference(7, true), "seven-string dropped guitar reference applies");
    expect(model.guitarStringCount() == 7 && model.guitarDropTuning(),
        "guitar reference is observable");

    const int beforeInvalid = model.revision();
    model.setCell(-1, QStringLiteral("chord"), 0, QStringLiteral("ignored"));
    model.setCell(0, QStringLiteral("unknown"), 0, QStringLiteral("ignored"));
    model.setCell(0, QStringLiteral("chord"), 99, QStringLiteral("ignored"));
    expect(model.revision() == beforeInvalid, "invalid cells do not mutate revision");

    model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
    model.setCell(0, QStringLiteral("target"), 0, QStringLiteral("E4"));
    model.setCell(0, QStringLiteral("beat"), 0, QStringLiteral("push"));
    model.setCell(0, QStringLiteral("lyric"), 7, QStringLiteral("line"));
    expect(!model.hasOnlyPristineSection(), "edited beat grid is not pristine");
    expect(model.occupiedBeatCount(0) == 8, "occupied beat count follows furthest content");

    model.setBeatDivision(0, 0, 3);
    expect(model.section(0).beatPatterns.at(0).division == 3,
        "valid triplet beat division applies");
    model.setBeatDivision(0, 0, 99);
    expect(model.section(0).beatPatterns.at(0).division == 4,
        "invalid beat division normalizes to sixteenth notes");
    model.setBeatHit(0, 0, 0, QStringLiteral("x"));
    expect(model.section(0).beatPatterns.at(0).lanes.at(0) == QStringLiteral("x"),
        "drum hit applies to requested lane");
    const int beforeInvalidHit = model.revision();
    model.setBeatHit(0, 0, -1, QStringLiteral("x"));
    model.setBeatHit(0, 0, BeatGridModel::beatLaneNames().size(), QStringLiteral("x"));
    expect(model.revision() == beforeInvalidHit, "invalid drum lanes do not mutate revision");

    model.setMusicalDivision(0, 0, 4);
    const MusicalBeatPattern& expanded = model.section(0).musicalPatterns.at(0);
    expect(expanded.division == 4 && expanded.chords.size() == 4,
        "musical division expands every lane");
    expect(expanded.chords.at(0).state == MusicalStepState::Onset &&
            expanded.chords.at(1).state == MusicalStepState::Hold,
        "expanded onset becomes one onset followed by holds");
    model.setMusicalStep(0, 0, 0, QStringLiteral("chord"), QStringLiteral("-"));
    model.setMusicalStep(0, 0, 1, QStringLiteral("chord"), QStringLiteral("-"));
    model.setMusicalStep(0, 0, 2, QStringLiteral("chord"), QStringLiteral("-"));
    model.setMusicalStep(0, 0, 3, QStringLiteral("chord"), QStringLiteral("-"));
    expect(model.section(0).chords.at(0) == QStringLiteral("-"),
        "entirely resting chord lane has rest summary");
    model.setMusicalStep(0, 0, 2, QStringLiteral("chord"), QStringLiteral("Dm7"));
    expect(model.section(0).chords.at(0) == QStringLiteral("Dm7"),
        "first chord onset supplies harmonic summary");
    model.setMusicalStep(0, 0, 0, QStringLiteral("melody"), QStringLiteral("~"));
    expect(model.section(0).targets.at(0).isEmpty(), "melody hold clears legacy target");
    model.setMusicalStep(0, 0, 0, QStringLiteral("melody"), QStringLiteral("A4"));
    model.setMusicalStep(0, 0, 1, QStringLiteral("bass"), QStringLiteral("D2"));
    model.setMusicalStep(0, 0, 1, QStringLiteral("support"), QStringLiteral("F4"));
    expect(model.section(0).targets.at(0) == QStringLiteral("A4"),
        "melody onset updates legacy target");
    const int beforeInvalidStep = model.revision();
    model.setMusicalStep(0, 0, 4, QStringLiteral("chord"), QStringLiteral("bad"));
    model.setMusicalStep(0, 0, 0, QStringLiteral("unknown"), QStringLiteral("bad"));
    expect(model.revision() == beforeInvalidStep, "invalid musical steps do not mutate revision");
    model.setMusicalDivision(0, 0, 2);
    expect(model.section(0).musicalPatterns.at(0).division == 2,
        "musical division contracts deterministically");
    model.setMusicalDivision(0, 0, 99);
    expect(model.section(0).musicalPatterns.at(0).division == 1,
        "invalid musical division normalizes to quarter notes");
    const int beforeSameDivision = model.revision();
    model.setMusicalDivision(0, 0, 1);
    expect(model.revision() == beforeSameDivision,
        "unchanged musical division is a no-op");

    expect(BeatGridModel::beatLaneSchemaVersion() == 3 &&
            BeatGridModel::beatLaneNames().size() == 10 &&
            BeatGridModel::beatVisualLaneNames().front() == BeatGridModel::beatLaneNames().back(),
        "beat lane schema and visual reversal remain explicit");
    expect(BeatGridModel::beatDivisionValues().contains(6) &&
            BeatGridModel::beatDivisionLabel(1) == QStringLiteral("Quarter") &&
            BeatGridModel::beatDivisionLabel(2) == QStringLiteral("Eighth") &&
            BeatGridModel::beatDivisionLabel(3) == QStringLiteral("Triplet") &&
            BeatGridModel::beatDivisionLabel(6) == QStringLiteral("6th") &&
            BeatGridModel::beatDivisionLabel(8) == QStringLiteral("32nd") &&
            BeatGridModel::beatDivisionLabel(99) == QStringLiteral("16th") &&
            BeatGridModel::musicalDivisionValues().contains(3) &&
            BeatGridModel::musicalDivisionLabel(99) == QStringLiteral("Quarter"),
        "beat and musical division labels cover supported and fallback values");

    model.resizeAllSections(6);
    for (const SongSection& section : model.sections()) {
        expect(section.beats == 6, "resize all applies to every section");
    }
    model.addSection();
    expect(model.sections().size() == 5 && model.section(4).beats == 6,
        "new section inherits prior length");
    model.renameSection(4, QStringLiteral(" X "), QStringLiteral(" Bridge "));
    expect(model.section(4).label == QStringLiteral("X") &&
            model.section(4).name == QStringLiteral("Bridge"),
        "section rename trims label and name");
    const QString destinationId = model.section(4).id;
    expect(model.copySection(0, 4), "copy between distinct valid sections succeeds");
    expect(model.section(4).id == destinationId &&
            model.section(4).beatPatterns.at(0).lanes.at(0) == QStringLiteral("x"),
        "copy retains destination identity and copies content");
    expect(!model.copySection(0, 0) && !model.copySection(-1, 0),
        "invalid section copies reject");

    const QString originalId = model.section(0).id;
    model.section(0).generatedKind = QStringLiteral("generated");
    model.duplicateSection(0);
    expect(model.sections().size() == 6 && model.section(1).id != originalId,
        "duplicate creates a new section identity");
    expect(model.section(1).generatedKind.isEmpty(),
        "duplicate intentionally becomes ordinary editable content");
    model.section(0).generatedKind.clear();
    model.section(0).generatedRecipe = {};
    model.moveSection(1, 5);
    expect(model.section(5).id != originalId, "move reorders section identity");
    const int beforeInvalidMove = model.revision();
    model.moveSection(0, 0);
    model.moveSection(-1, 2);
    expect(model.revision() == beforeInvalidMove, "invalid section moves are no-ops");
    model.deleteSection(5);
    expect(model.sections().size() == 5, "delete removes a movable section");

    expect(!model.setDrumKit(-1, QStringLiteral("electronic")),
        "invalid drum-kit section rejects");
    expect(!model.setDrumKit(0, QStringLiteral("orchestral")),
        "unknown drum kit rejects");
    expect(model.setDrumKit(0, QStringLiteral("electronic")) &&
            model.setDrumKit(0, QStringLiteral("electronic")),
        "valid and unchanged drum kit requests succeed");

    SongSection replacement;
    replacement.label = QStringLiteral("R");
    replacement.name = QStringLiteral("Replacement");
    replacement.beats = 5;
    replacement.drumKitId = QStringLiteral("invalid");
    const QString replaceId = model.section(2).id;
    expect(model.replaceSection(2, replacement), "replace section accepts valid index");
    expect(model.section(2).id == replaceId && model.section(2).beats == 5 &&
            model.section(2).drumKitId == QStringLiteral("acoustic"),
        "replace retains identity and normalizes bounds and kit");
    expect(model.clearSection(2), "clear section accepts valid index");
    expect(model.section(2).id == replaceId && model.occupiedBeatCount(2) == 0,
        "clear retains identity and removes content");

    const QJsonObject saved = model.toJson();
    BeatGridModel restored;
    expect(restored.loadJson(saved), "current beat-grid JSON round trips");
    expect(restored.title() == model.title() &&
            restored.sections().size() == model.sections().size(),
        "round trip retains title and section count");
    QJsonObject obsolete = saved;
    obsolete.insert(QStringLiteral("beat_lane_schema"), BeatGridModel::beatLaneSchemaVersion() - 1);
    expect(!restored.loadJson(obsolete), "obsolete beat-lane schema rejects explicitly");
}

void testGeneratedSectionOwnership()
{
    BeatGridModel model;
    SongSection generated;
    generated.label = QStringLiteral("G");
    generated.name = QStringLiteral("Generated");
    generated.beats = 8;
    expect(model.replaceGeneratedSection(QString(), generated) == -1,
        "blank generated kind rejects");
    const QString pristineId = model.section(0).id;
    expect(model.replaceGeneratedSection(QStringLiteral("idea"), generated) == 0,
        "first generated section replaces pristine first section");
    expect(model.section(0).id == pristineId &&
            model.section(0).generatedKind == QStringLiteral("idea"),
        "generated replacement retains stable identity");

    SongSection refreshed = generated;
    refreshed.name = QStringLiteral("Refreshed");
    expect(model.replaceGeneratedSection(QStringLiteral("idea"), refreshed) == 0 &&
            model.section(0).name == QStringLiteral("Refreshed"),
        "same generated kind refreshes in place");
    SongSection second = generated;
    second.name = QStringLiteral("Second kind");
    expect(model.replaceGeneratedSection(QStringLiteral("drums"), second) == 4,
        "distinct generated kind appends after non-pristine content");

    while (model.sections().size() < 12) model.addSection(4);
    expect(model.replaceGeneratedSection(QStringLiteral("overflow"), generated) == -1,
        "generated append respects section capacity");
    const int atCapacity = model.sections().size();
    model.addSection();
    model.duplicateSection(0);
    expect(model.sections().size() == atCapacity, "ordinary append respects section capacity");
    for (int index = model.sections().size() - 1; index >= 4; --index) model.deleteSection(index);
    const int minimum = model.sections().size();
    model.deleteSection(0);
    expect(model.sections().size() == minimum, "delete respects minimum section count");
}

ConnectionDiagnosticsSnapshot baseDiagnostics()
{
    ConnectionDiagnosticsSnapshot stats;
    stats.received_packets = 1000;
    stats.packet_gap_samples = 1000;
    return stats;
}

void testMixerPresentation()
{
    MixerStatsViewModel model;
    const MixerStatsLabels absent = model.present(nullptr);
    expect(absent.latency == QStringLiteral("RTT -") &&
            absent.diagnosis == QStringLiteral("Diagnosis -"),
        "missing connection statistics have explicit placeholders");

    ConnectionDiagnosticsSnapshot stats = baseDiagnostics();
    for (int index = 0; index < 5; ++index) {
        Jam2PeerDiagnostics peer;
        peer.peer_id = static_cast<std::uint64_t>(index + 1);
        peer.rtt_ms = 10.0 + index;
        peer.has_rtt = index != 1;
        stats.peers.push_back(peer);
    }
    const MixerStatsLabels normal = model.present(&stats);
    expect(normal.latency.contains(QStringLiteral("P1 10.0 ms")) &&
            normal.latency.contains(QStringLiteral("+1")),
        "latency summary shows four peers and overflow count");
    expect(normal.latencyTooltip.count(QLatin1Char('\n')) == 4,
        "latency tooltip retains every peer");
    expect(normal.diagnosis == QStringLiteral("Diagnosis OK"),
        "healthy measurements report an explicit OK diagnosis");

    stats.output_underrun_events = 2;
    stats.output_underrun_frames = 480;
    stats.output_underrun_ms = 10.0;
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("output underruns")),
        "underruns take diagnostic priority");
    stats = baseDiagnostics();
    stats.packet_loss_percent = 0.5;
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("packet loss")),
        "packet-loss boundary diagnoses loss");
    stats = baseDiagnostics();
    stats.packet_gap_over_4x = 5;
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("jitter/reordering")),
        "burst-gap boundary diagnoses jitter pressure");
    stats = baseDiagnostics();
    stats.reordered_or_late_packets = 3;
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("jitter/reordering")),
        "late-packet boundary diagnoses reordering pressure");
    stats = baseDiagnostics();
    stats.callback_gap_over_2x = 1;
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("callback gaps")),
        "callback gaps have direct diagnosis");
    stats = baseDiagnostics();
    stats.drift_abs_ppm_max = 200.0;
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("clock drift")),
        "drift boundary diagnoses correction pressure");
    stats = baseDiagnostics();
    Jam2PeerDiagnostics distant;
    distant.has_rtt = true;
    distant.rtt_ms = 100.0;
    stats.peers.push_back(distant);
    expect(model.present(&stats).diagnosis.contains(QStringLiteral("high network RTT")),
        "high-RTT boundary diagnoses physical latency");

    jam2::EngineGuiPeakSnapshot peaks;
    peaks.input_peak_ppm = 1000000;
    peaks.monitor_peak_ppm = 500000;
    peaks.remote_peak_ppm = 250000;
    peaks.prepared_track_peak_ppm = 125000;
    peaks.metronome_peak_ppm = 750000;
    peaks.output_peak_ppm = 1500000;
    MixerMeterLevels levels = model.consume(peaks, 0.5, 9);
    expect(approximatelyEqual(levels.input, 1.0) && approximatelyEqual(levels.send, 0.5) &&
            approximatelyEqual(levels.monitor, 0.5) && approximatelyEqual(levels.remote, 0.25) &&
            approximatelyEqual(levels.track, 0.125) && approximatelyEqual(levels.metronome, 0.75) &&
            approximatelyEqual(levels.output, 1.0) && levels.outputClippedSamples == 9,
        "meter presentation normalizes, clamps, and applies send gain");
    peaks = {};
    levels = model.consume(peaks, 0.0, 0);
    expect(approximatelyEqual(levels.input, 0.78) && approximatelyEqual(levels.output, 0.78),
        "meter levels decay predictably between polls");
    model.reset();
    levels = model.consume(peaks, 0.0, 0);
    expect(approximatelyEqual(levels.input, 0.0) && approximatelyEqual(levels.output, 0.0),
        "meter reset removes retained peaks");
}

void testGuiPresentation(QApplication& app)
{
    expect(approximatelyEqual(focusPresetForKey(QStringLiteral("bass")).frequencyHz, 95.0) &&
            approximatelyEqual(focusPresetForKey(QStringLiteral("guitar")).frequencyHz, 850.0) &&
            approximatelyEqual(focusPresetForKey(QStringLiteral("vocals")).frequencyHz, 1800.0) &&
            approximatelyEqual(focusPresetForKey(QStringLiteral("drums")).frequencyHz, 3200.0) &&
            approximatelyEqual(focusPresetForKey(QStringLiteral("unknown")).frequencyHz, 120.0),
        "focus preset mapping covers every named and fallback preset");
    expect(isCustomFocusPreset(QString()) && isCustomFocusPreset(QStringLiteral("custom")) &&
            !isCustomFocusPreset(QStringLiteral("bass")),
        "custom focus preset classification is exact");

    QCheckBox manual;
    QSpinBox duration;
    QLabel label;
    manual.setChecked(true);
    updateCaptureDurationControl(&manual, &duration, &label);
    expect(!duration.isEnabled() && !label.isEnabled(),
        "manual stop disables duration editor and label");
    manual.setChecked(false);
    updateCaptureDurationControl(&manual, &duration);
    updateCaptureDurationControl(&manual, &duration, &label);
    expect(duration.isEnabled() && label.isEnabled(),
        "timed capture enables duration editor and label");
    updateCaptureDurationControl(nullptr, &duration);
    manual.setChecked(true);
    label.setEnabled(true);
    updateCaptureDurationControl(&manual, nullptr, &label);
    expect(!label.isEnabled(), "duration label still follows manual stop without a spin box");

    QSlider slider(Qt::Horizontal);
    applyJamSliderStyle(&slider);
    expect(!slider.styleSheet().isEmpty(), "Jam slider style is installed");
    applyJamSliderStyle(nullptr);
    applyMutedEditorStyle(&duration);
    expect(duration.property("jam2MutedEditor").toBool(),
        "muted editor styling marks the widget");
    applyMutedEditorStyle(nullptr);

    expect(dbText(2.25) == QStringLiteral("+2.3 dB") &&
            dbText(-2.25) == QStringLiteral("-2.3 dB"),
        "decibel text preserves sign and one decimal place");
    expect(approximatelyEqual(gainFromDb(0.0), 1.0) &&
            approximatelyEqual(gainFromDb(-6.0), std::pow(10.0, -0.3)) &&
            gainFromDb(-60.0) == 0.0,
        "decibel conversion handles unity, attenuation, and silence");
    expect(metronomeStepLabel(0, 4) == QStringLiteral("1.1") &&
            metronomeStepLabel(1, 2) == QStringLiteral("1.3") &&
            metronomeStepLabel(-1, 0) == QStringLiteral("1.1"),
        "metronome step labels normalize division and step bounds");

    QTemporaryDir releaseRoot;
    expect(releaseRoot.isValid(), "temporary release root exists");
    QString error;
    expect(!setAppReleaseRootForTesting(QStringLiteral("relative"), error) && !error.isEmpty(),
        "relative release-root override rejects");
    expect(setAppReleaseRootForTesting(releaseRoot.path(), error) && error.isEmpty(),
        "absolute release-root override applies");
    expect(appReleaseFolderPath(QStringLiteral("songs")).startsWith(releaseRoot.path()) &&
            appReleaseFilePath(QStringLiteral("captures"), QStringLiteral("one.wav")).startsWith(releaseRoot.path()),
        "release storage paths honor the test root");

    installJam2Style();
    installJam2Style();
    installCompactDialogPolicy(app);
    const QString invite = QStringLiteral("jam2://join/127.0.0.1:5000/session-key");
    showJamReadyInviteDialog(nullptr, invite);
    app.processEvents();
    QDialog* inviteDialog = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog != nullptr && dialog->windowTitle() == QStringLiteral("Jam Ready")) {
            inviteDialog = dialog;
            break;
        }
    }
    expect(inviteDialog != nullptr, "jam-ready invite dialog is visible");
    expect(QApplication::clipboard()->text() == invite,
        "jam-ready invite is copied when the dialog opens");
    if (inviteDialog != nullptr) {
        const auto edits = inviteDialog->findChildren<QLineEdit*>();
        expect(edits.size() == 1 && edits.front()->isReadOnly() && edits.front()->text() == invite,
            "invite dialog exposes one read-only complete URL");
        const auto buttons = inviteDialog->findChildren<QPushButton*>();
        QApplication::clipboard()->clear();
        bool copied = false;
        for (QPushButton* button : buttons) {
            if (button->text() == QStringLiteral("Copy URL")) {
                button->click();
                copied = true;
            }
        }
        expect(copied && QApplication::clipboard()->text() == invite,
            "invite copy button restores the complete URL");
        inviteDialog->close();
        app.processEvents();
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    testBeatGridEditing();
    testGeneratedSectionOwnership();
    testMixerPresentation();
    testGuiPresentation(app);
    if (failures == 0) {
        std::cout << "GUI model boundary tests passed\n";
        return 0;
    }
    std::cerr << failures << " GUI model boundary assertion(s) failed\n";
    return 1;
}
