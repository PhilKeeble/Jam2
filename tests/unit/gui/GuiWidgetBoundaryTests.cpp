#include "BeatGridWidget.hpp"
#include "ActionTileRow.hpp"
#include "IdeaHeaderRow.hpp"
#include "AudioDeviceUiSupport.hpp"
#include "ConnectionGuidance.hpp"
#include "GuiControlContract.hpp"
#include "GuiInteractionPolicy.hpp"
#include "PerformanceWidgets.hpp"
#include "SectionTimeline.hpp"
#include "SessionStartupDialogs.hpp"
#include "TrackWidgets.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QListWidget>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTextEdit>
#include <QThreadPool>
#include <QTimer>
#include <QToolTip>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPointer>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <vector>
#include <ctime>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

double processCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        expect(false, "native process CPU time must be available");
        return 0.0;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
#else
    const auto ticks = std::clock();
    expect(ticks != static_cast<std::clock_t>(-1), "process CPU clock must be available");
    return static_cast<double>(ticks) / CLOCKS_PER_SEC;
#endif
}

void processPaint(QWidget& widget)
{
    widget.show();
    QApplication::processEvents();
    const QPixmap image = widget.grab();
    expect(!image.isNull(), "widget renders to an offscreen image");
    QApplication::processEvents();
}

void sendMouse(
    QWidget& widget,
    QEvent::Type type,
    const QPointF& local,
    Qt::MouseButton button,
    Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF global(widget.mapToGlobal(local.toPoint()));
    QMouseEvent event(type, local, local, global, button, buttons, modifiers);
    QApplication::sendEvent(&widget, &event);
}

void sendWheel(QWidget& widget, const QPointF& local, int delta)
{
    const QPointF global(widget.mapToGlobal(local.toPoint()));
    QWheelEvent event(
        local,
        global,
        QPoint(),
        QPoint(0, delta),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(&widget, &event);
}

QObject* findControl(QObject& root, const QString& id)
{
    if (jam2::gui::guiControlId(root) == id) return &root;
    const QList<QObject*> descendants = root.findChildren<QObject*>();
    QObject* fallback = nullptr;
    for (QObject* object : descendants) {
        if (object == nullptr || jam2::gui::guiControlId(*object) != id) continue;
        fallback = object;
        auto* widget = qobject_cast<QWidget*>(object);
        if (widget == nullptr || widget->isVisible()) return object;
    }
    return fallback;
}

bool clickControl(QObject& root, const QString& id)
{
    auto* button = qobject_cast<QAbstractButton*>(findControl(root, id));
    if (button == nullptr) return false;
    button->click();
    QApplication::processEvents();
    return true;
}

void testIdeaHeaderRows()
{
    using Row = jam2::gui::IdeaHeaderRow;
    const QStringList labels{QStringLiteral("GENERATE NEW IDEA"), QStringLiteral("CONTINUE IDEA"),
        QStringLiteral("IDEA DETAILS"), QStringLiteral("GENERATE WAV"), QStringLiteral("GROOVE LIBRARY"), QStringLiteral("JAMTASTER")};
    const QStringList tooltips{
        QStringLiteral("Generate a new procedural idea to populate the chord and beat views"),
        QStringLiteral("Pick a source section and generate a related idea in another section to spark new ideas"),
        QStringLiteral("Explore how this section's generated idea works, including its chords, groove and musical parts"),
        QStringLiteral("Render the selected sections and musical parts into WAV backing tracks"),
        QStringLiteral("Browse and preview drum grooves, then import one into the active section"),
        QStringLiteral("Analyse a WAV to detect tempo, separate stems and extract chords, beats, bass and song structure")};
    BeatGridModel chords, beats;
    chords.addSection(); beats.addSection();
    auto& recipe = chords.section(0).generatedRecipe;
    recipe.styleId = QStringLiteral("rock"); recipe.profileId = QStringLiteral("rock");
    recipe.variationId = QStringLiteral("standard"); recipe.tonic = QStringLiteral("C"); recipe.mode = QStringLiteral("Major");
    recipe.progressionId = QStringLiteral("tonic"); recipe.grooveId = QStringLiteral("straight"); recipe.grooveFeelName = QStringLiteral("Straight");
    recipe.formSections = {{QStringLiteral("A"), 1, 16, QStringLiteral("main"), {}}};
    jam2::practice::DrumPerformanceEvent kick;
    kick.laneId = QStringLiteral("kick");
    kick.velocity = 88;
    recipe.drumEvents = {kick};
    expect(recipe.isValid(), "idea header fixture contains valid generated metadata");
    for (bool includeDetails : {true, false}) {
        Row row(nullptr, includeDetails);
        row.setAttribute(Qt::WA_DontShowOnScreen, true);
        expect(row.findChildren<QPushButton*>().size() == (includeDetails ? 6 : 5),
            "Track omits Idea Details without creating an orphaned button");
        row.updateSection(chords, beats, 0);
        for (int width : {1000, 760, 1100}) {
            row.resize(width, 42); processPaint(row);
            int previousRight = -1;
            for (int i = 0; i < 6; ++i) {
                auto* button = row.button(static_cast<Row::Action>(i));
                if (i == 2 && !includeDetails) { expect(!button, "Track has no Idea Details action"); continue; }
                expect(button && button->accessibleName() == labels[i] && button->toolTip() == tooltips[i] &&
                        !button->toolTip().endsWith(QLatin1Char('.')),
                    "real view buttons use approved descriptive tooltips without full stops");
                if (!button) continue;
                const QRect bounds(button->mapTo(&row, QPoint()), button->size());
                expect(bounds.left() > previousRight && row.rect().contains(bounds),
                    "Chord, Beat and Track retain requested non-overlapping action order");
                previousRight = bounds.right();
            }
        }
        if (!includeDetails) continue;
        auto* details = row.button(Row::Action::Details);
        int opened = 0;
        QObject::connect(details, &QPushButton::clicked, &row, [&] { ++opened; });
        details->click();
        expect(opened == 1 && details->isEnabled(), "Idea Details activates for the selected generated section");
        row.updateSection(chords, beats, 1);
        details->click();
        expect(!details->isEnabled() && opened == 1 &&
                details->toolTip() == QStringLiteral("Generate an idea in this section to view its details"),
            "an empty selected section disables Details even when another section has an idea");
        beats.section(1).generatedRecipe = recipe;
        row.updateSection(chords, beats, 1);
        expect(details->isEnabled() && jam2::gui::ideaDetailsSection(chords, beats, 1) == &beats.section(1),
            "beat-only generated metadata enables Details for that same section");
        beats.clearSection(1); row.updateSection(chords, beats, 1);
        expect(!details->isEnabled(), "clearing the section disables Idea Details again");
        row.updateSection(chords, beats, -1);
        expect(!details->isEnabled() && !jam2::gui::ideaDetailsSection(chords, beats, 99),
            "invalid section selection cannot display another section's idea");
        row.hide();
    }
}

void testActionTileRow()
{
    const QVector<jam2::gui::ActionTileSpec> actions{
        {QStringLiteral("GENERATE NEW IDEA"), 154},
        {QStringLiteral("GROOVE LIBRARY"), 132},
        {QStringLiteral("CONTINUE IDEA"), 124},
        {QStringLiteral("GENERATE WAV"), 116},
        {QStringLiteral("JAMTASTER"), 132},
        {QStringLiteral("IDEA DETAILS"), 112},
    };
    QWidget header;
    header.setAttribute(Qt::WA_DontShowOnScreen, true);
    header.setStyleSheet(QStringLiteral("QWidget { font-size:10.5pt;font-family:Arial; }"));
    auto* layout = new QHBoxLayout(&header);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* section = new QPushButton(QStringLiteral("SECTION A B C D"), &header);
    section->setFixedWidth(180);
    auto* row = new jam2::gui::ActionTileRow(&header);
    auto* pagination = new QPushButton(QStringLiteral("1 / 4"), &header);
    pagination->setFixedWidth(80);
    layout->addWidget(section);
    layout->addWidget(row, 1);
    layout->addWidget(pagination);
    QVector<QPushButton*> buttons;
    std::array<int, 6> clicks{};
    for (qsizetype i = 0; i < actions.size(); ++i) {
        auto* button = new QPushButton(actions[i].label, &header);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { border:1px solid #e2ac53;border-radius:4px;color:#f3e6cf;background:#231b14; }"));
        row->addButton(button, actions[i].preferredWidth);
        QObject::connect(button, &QPushButton::clicked, &header, [&, i] { ++clicks[i]; });
        buttons.push_back(button);
    }
    for (int width : {1440, 1100, 900, 761, 760, 759, 1100, 1440}) {
        header.resize(width, 42);
        processPaint(header);
        expect(header.width() == width, "action row does not force a wide parent minimum");
        int previousRight = -1;
        int rowY = -1;
        for (qsizetype i = 0; i < buttons.size(); ++i) {
            auto* button = buttons[i];
            const QRect bounds(button->mapTo(row, QPoint()), button->size());
            if (rowY < 0) rowY = bounds.y();
            expect(row->rect().contains(bounds) && bounds.left() > previousRight && bounds.y() == rowY,
                "sub-view action tiles remain inside one non-overlapping row");
            expect(button->font().pointSizeF() >= 7.99 && button->font().pointSizeF() <= 9.01 &&
                    button->accessibleName() == actions[i].label && button->toolTip() == actions[i].label,
                "compact tiles retain readable fonts and full accessible labels");
            const int textWidth = bounds.width() - 2 *
                jam2::gui::layoutActionTiles(row->rect(), actions).padding - 2;
            if (QFontMetrics(button->font()).horizontalAdvance(button->text()) > textWidth)
                std::cerr << "tile header=" << width << " row=" << row->width() << " index=" << i
                    << " text=" << button->text().toStdString() << " available=" << textWidth
                    << " measured=" << QFontMetrics(button->font()).horizontalAdvance(button->text())
                    << " font=" << button->font().toString().toStdString() << '\n';
            expect(QFontMetrics(button->font()).horizontalAdvance(button->text()) <= textWidth,
                "displayed tile labels fit the actual selected platform font");
            button->click();
            previousRight = bounds.right();
        }
        if (width == 1440) {
            if (buttons.front()->text() != actions.front().label) {
                std::cerr << "wide row=" << row->width() << " button=" << buttons.front()->width()
                          << " text=" << buttons.front()->text().toStdString()
                          << " font=" << buttons.front()->font().pointSizeF()
                          << " measured=" << QFontMetrics(buttons.front()->font())
                              .horizontalAdvance(actions.front().label) << '\n';
            }
            expect(buttons.front()->text() == actions.front().label,
                "full labels restore when the header grows again");
        }
        const QString previewRoot = qEnvironmentVariable("JAM2_TEST_LAYOUT_PREVIEW_ROOT");
        if (!previewRoot.isEmpty()) {
            expect(QDir().mkpath(previewRoot) && header.grab().save(QDir(previewRoot).filePath(
                QStringLiteral("action-header-%1.png").arg(width))),
                "requested single-row header preview is written");
        }
    }
    for (int count : clicks) expect(count == 8, "responsive Qt buttons retain their original actions");

    QFont widerFallback = jam2::gui::actionTileFont();
    widerFallback.setFamily(QStringLiteral("Arial"));
    widerFallback.setStretch(125);
    for (int width : {0, 32, 360, 640, 900}) {
        const auto sizing = jam2::gui::layoutActionTiles(QRect(11, 7, width, 31), actions, widerFallback);
        int right = 10;
        for (const QRect& bounds : sizing.bounds) {
            expect(bounds.left() >= 11 && bounds.right() < 11 + width && bounds.left() > right &&
                    bounds.top() == 7 && bounds.width() >= 0,
                "font-aware sizing stays bounded without wrapping, including tiny widths");
            right = bounds.right();
        }
    }
}

void testTimelineHelpers()
{
    expect(approximatelyEqual(jam2::gui::beatOverviewHitPhase(1, 2, 4, 4), 0.375),
        "beat overview phase includes subdivision");
    expect(jam2::gui::beatOverviewHitPhase(-2, 99, 0, 0) == 0.0,
        "beat overview phase bounds invalid inputs");

    const jam2::gui::ChordDetailGroup empty =
        jam2::gui::chordDetailGroupForWidths({}, 0, 100);
    expect(empty.start == 0 && empty.count == 1,
        "empty chord widths have one safe detail group");
    const QVector<int> widths{100, 100, 100, 100, 100};
    const jam2::gui::ChordDetailGroup first =
        jam2::gui::chordDetailGroupForWidths(widths, 1, 225, 4, 8);
    const jam2::gui::ChordDetailGroup last =
        jam2::gui::chordDetailGroupForWidths(widths, 4, 225, 4, 8);
    expect(first.start == 0 && first.count == 2 && last.start == 4 && last.count == 1,
        "chord detail grouping follows width and selected bar");

    expect(jam2::gui::sectionOverviewPageCount(0) == 1 &&
            jam2::gui::sectionOverviewPageCount(33) == 2 &&
            jam2::gui::sectionOverviewPageForBar(32, 33) == 1 &&
            jam2::gui::sectionOverviewPageCount(
                (std::numeric_limits<int>::max)()) == 67108864,
        "section overview pagination handles its boundary without integer wrap");
    expect(jam2::gui::sectionBeatCountForTimelineEnd(0, 48000, 120.0, 1, 4) == 4 &&
            jam2::gui::sectionBeatCountForTimelineEnd(96001, 48000, 120.0, 1, 4) == 4 &&
            jam2::gui::sectionBeatCountForTimelineEnd(96002, 48000, 120.0, 1, 4) == 8,
        "section end converts to complete bar counts with frame tolerance");
    const jam2::gui::SectionTimelineCrop removed =
        jam2::gui::sectionTimelineCropForEnd(100, 0, 48000, 48000, 100);
    const jam2::gui::SectionTimelineCrop retained =
        jam2::gui::sectionTimelineCropForEnd(20, 10, 24000, 48000, 120);
    expect(removed.removePlacement && retained.stopFrame == 120 &&
            retained.sourceStartFrame == 10 && retained.sourceEndFrame == 60,
        "section crop distinguishes removal and sample-rate-adjusted retention");
    const qint64 maximumFrame = (std::numeric_limits<qint64>::max)();
    const jam2::gui::SectionTimelineCrop saturated =
        jam2::gui::sectionTimelineCropForEnd(
            0,
            maximumFrame - 2,
            (std::numeric_limits<int>::max)(),
            1,
            maximumFrame);
    expect(jam2::gui::sectionBeatCountForTimelineEnd(
                maximumFrame, 1, 1.0e300, 1, 4) ==
                (std::numeric_limits<int>::max)() &&
            jam2::gui::sectionBeatCountForTimelineEnd(
                maximumFrame,
                48000,
                std::numeric_limits<double>::quiet_NaN(),
                1,
                4) == 4 &&
            !saturated.removePlacement &&
            saturated.sourceStartFrame == maximumFrame - 2 &&
            saturated.sourceEndFrame == maximumFrame,
        "section frame conversion rejects non-finite clocks and saturates extreme domains");

    expect(jam2::gui::trackTimelineBarNumber(0, 4) == 1 &&
            jam2::gui::trackTimelineBarNumber(4, 4) == 2 &&
            jam2::gui::trackTimelineBarNumber(3, 4) == 0,
        "track timeline labels only bar boundaries");
    expect(jam2::gui::looperTimelineViewFrames(48000, 10, 20, 1000, 2000) == 96000,
        "looper timeline includes the furthest millisecond marker");
    expect(jam2::gui::stableLiveTimelineExtentFrames(0, 100, 101) == 200 &&
            jam2::gui::stableLiveTimelineExtentFrames(200, 100, 150) == 200 &&
            jam2::gui::stableLiveTimelineExtentFrames(200, 100, 0) == 0,
        "live timeline extent grows geometrically and resets when inactive");
    const qint64 maximum = (std::numeric_limits<qint64>::max)();
    expect(jam2::gui::stableLiveTimelineExtentFrames(maximum / 2 + 1, 1, maximum) == maximum,
        "live timeline extent saturates rather than overflowing");
    for (const double gain : {-60.0, -30.0, 0.0, 12.0}) {
        expect(approximatelyEqual(
            jam2::gui::trackGainDb(jam2::gui::trackGainPosition(gain)), gain),
            "track gain position round trips boundary gain");
    }
}

void testGuiInteractionPolicy()
{
    QWidget plain;
    QWidget plainChild(&plain);
    QLineEdit lineEdit;
    QWidget lineEditChild(&lineEdit);
    QPlainTextEdit plainText;
    QTextEdit richText;
    QSpinBox spin;
    QComboBox combo;
    combo.addItems({QStringLiteral("one"), QStringLiteral("two")});
    expect(!jam2::gui::explicitValueEditorHasFocus(nullptr) &&
            !jam2::gui::explicitValueEditorHasFocus(&plainChild) &&
            jam2::gui::explicitValueEditorHasFocus(&lineEditChild) &&
            jam2::gui::explicitValueEditorHasFocus(&plainText) &&
            jam2::gui::explicitValueEditorHasFocus(&richText) &&
            jam2::gui::explicitValueEditorHasFocus(&spin) &&
            jam2::gui::explicitValueEditorHasFocus(&combo),
        "explicit value-editor focus follows editor ownership");

    QSlider slider;
    QWidget sliderChild(&slider);
    QPushButton button;
    QListWidget list;
    QScrollArea scrollArea;
    QTabBar tabs;
    expect(!jam2::gui::blocksIncidentalNavigationKey(nullptr) &&
            !jam2::gui::blocksIncidentalNavigationKey(&plainChild) &&
            jam2::gui::blocksIncidentalNavigationKey(&sliderChild) &&
            jam2::gui::blocksIncidentalNavigationKey(&button) &&
            jam2::gui::blocksIncidentalNavigationKey(&list) &&
            jam2::gui::blocksIncidentalNavigationKey(&scrollArea) &&
            jam2::gui::blocksIncidentalNavigationKey(&tabs),
        "navigation-key policy protects interactive control families");

    QObject spinChild(&spin);
    expect(!jam2::gui::isWheelValueEditor(&plain) &&
            jam2::gui::isWheelValueEditor(&spinChild) &&
            jam2::gui::isWheelValueEditor(&slider) &&
            jam2::gui::isWheelValueEditor(&combo),
        "wheel value-editor policy follows composite ownership");
    expect(!jam2::gui::isComboBoxPopupObject(&plain) &&
            jam2::gui::isComboBoxPopupObject(combo.view()),
        "combo popup policy recognizes Qt's owned list view");

    scrollArea.horizontalScrollBar()->setRange(0, 100);
    scrollArea.verticalScrollBar()->setRange(0, 100);
    scrollArea.horizontalScrollBar()->setValue(50);
    scrollArea.verticalScrollBar()->setValue(50);
    QWidget viewportChild(scrollArea.viewport());
    expect(jam2::gui::parentScrollArea(&viewportChild, Qt::Horizontal) ==
                &scrollArea &&
            jam2::gui::parentScrollArea(&viewportChild, Qt::Vertical) ==
                &scrollArea &&
            jam2::gui::parentScrollArea(&plainChild, Qt::Vertical) == nullptr,
        "parent scroll-area lookup requires an actually scrollable axis");

    const auto wheel = [](QPoint pixel, QPoint angle) {
        return QWheelEvent(
            QPointF(1, 1), QPointF(1, 1), pixel, angle,
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    };
    QWheelEvent horizontalPixel = wheel(QPoint(5, 0), QPoint());
    expect(jam2::gui::scrollAreaByWheel(
               scrollArea, horizontalPixel, Qt::Horizontal) &&
            scrollArea.horizontalScrollBar()->value() == 45,
        "horizontal pixel wheel moves the owning scroll area");
    QWheelEvent shiftedVerticalPixel = wheel(QPoint(0, 7), QPoint());
    expect(jam2::gui::scrollAreaByWheel(
               scrollArea, shiftedVerticalPixel, Qt::Horizontal, true) &&
            scrollArea.horizontalScrollBar()->value() == 38,
        "shifted vertical pixel wheel drives horizontal scrolling");
    QWheelEvent verticalAngle = wheel(QPoint(), QPoint(0, 120));
    expect(jam2::gui::scrollAreaByWheel(
               scrollArea, verticalAngle, Qt::Vertical) &&
            scrollArea.verticalScrollBar()->value() == 35,
        "angle-wheel fallback drives vertical scrolling");
    QWheelEvent zero = wheel(QPoint(), QPoint());
    expect(!jam2::gui::scrollAreaByWheel(
               scrollArea, zero, Qt::Vertical),
        "zero-delta wheel is not consumed");
    scrollArea.verticalScrollBar()->setRange(0, 0);
    expect(!jam2::gui::scrollAreaByWheel(
               scrollArea, verticalAngle, Qt::Vertical),
        "non-scrollable axes do not consume wheel input");
}

void testLocalEngineDialogState()
{
    jam2::gui::SessionAudioDeviceList devices;
    devices.devices = {
        {QStringLiteral("[17] ASIO First"), QStringLiteral("17")},
        {QStringLiteral("[19] ASIO Second"), QStringLiteral("19")},
    };
    for (auto& device : devices.devices) {
#ifdef Q_OS_MACOS
        device.backend = QStringLiteral("CoreAudio");
#else
        device.backend = QStringLiteral("ASIO");
#endif
        device.stableId = device.id;
        device.inputChannels = 8;
        device.outputChannels = 8;
    }
    devices.selectedId = QStringLiteral("19");
    int deviceTestCalls = 0;
    jam2::gui::LocalEngineDialog dialog(
        {
            QStringLiteral("19"),
            44100,
            128,
            QStringLiteral("1,2"),
            QStringLiteral("3,4"),
            false,
        },
        devices,
        {[&deviceTestCalls](const jam2::gui::AudioDeviceTestRequest& request, QPushButton*, QWidget*) {
            if (request.outputDeviceId == 17) {
                ++deviceTestCalls;
            }
        }});

    const jam2::gui::LocalEngineDialogState initial = dialog.state();
    expect(initial.selectedDeviceId == QStringLiteral("19") &&
            initial.sampleRate == 44100 && initial.bufferSize == 128 &&
            initial.inputChannels == QStringLiteral("1,2") &&
            initial.outputChannels == QStringLiteral("3,4") &&
            !initial.saveDefaults,
        "local engine dialog exposes its complete typed initial state");

    auto* device = qobject_cast<QComboBox*>(
        findControl(dialog, QStringLiteral("application.local-engine.device")));
    auto* sampleRate = qobject_cast<QComboBox*>(findControl(
        dialog, QStringLiteral("application.local-engine.sample-rate")));
    auto* bufferSize = qobject_cast<QComboBox*>(findControl(
        dialog, QStringLiteral("application.local-engine.buffer-size")));
    auto* inputs = qobject_cast<QLineEdit*>(findControl(
        dialog, QStringLiteral("application.local-engine.input-channels")));
    auto* outputs = qobject_cast<QLineEdit*>(findControl(
        dialog, QStringLiteral("application.local-engine.output-channels")));
    auto* saveDefaults = qobject_cast<QCheckBox*>(findControl(
        dialog, QStringLiteral("application.local-engine.save-defaults")));
    expect(device != nullptr && sampleRate != nullptr && bufferSize != nullptr &&
            inputs != nullptr && outputs != nullptr && saveDefaults != nullptr,
        "local engine dialog registers every typed editor");
    if (device == nullptr || sampleRate == nullptr || bufferSize == nullptr ||
        inputs == nullptr || outputs == nullptr || saveDefaults == nullptr) {
        return;
    }

    device->setCurrentIndex(device->findData(QStringLiteral("17")));
    sampleRate->setCurrentIndex(sampleRate->findData(48000));
    bufferSize->setCurrentIndex(bufferSize->findData(32));
    inputs->setText(QStringLiteral(" 5,6 "));
    outputs->setText(QStringLiteral(" 7,8 "));
    saveDefaults->setChecked(true);
    expect(clickControl(
               dialog, QStringLiteral("application.local-engine.test-device")) &&
            deviceTestCalls == 1,
        "local engine Test Device action receives the selected typed device");

    const jam2::gui::LocalEngineDialogState edited = dialog.state();
    expect(edited.selectedDeviceId == QStringLiteral("17"),
        "local engine dialog returns the edited device id");
    expect(edited.sampleRate == 48000,
        "local engine dialog returns the edited sample rate");
    expect(edited.bufferSize == 32,
        "local engine dialog returns the edited buffer size");
    expect(edited.inputChannels == QStringLiteral("5,6") &&
            edited.outputChannels == QStringLiteral("7,8"),
        "local engine dialog trims both edited channel lists");
    expect(edited.saveDefaults,
        "local engine dialog returns the edited persistence choice");
}

void testSessionDefaultsAndPriorityControls()
{
    const auto hasFastPlaybackDefaults = [](const LocalTuningPreference& tuning) {
        return tuning.prefillFrames == 64 && tuning.frameSize == 64 && tuning.driftDeadbandPpm == 0 &&
            tuning.playoutDelayFrames == 64 &&
            tuning.jitterBufferFrames == 64 &&
            tuning.jitterBufferMaxFrames == 512 &&
            tuning.adaptiveTargetFrames == 64 &&
            tuning.adaptiveMinFrames == 64 &&
            tuning.adaptiveMaxFrames == 512;
    };
    jam2::gui::StartJamDialogState startInitial;
    jam2::gui::StartJamDialog startDialog(startInitial, {}, {});
    auto* strategy = qobject_cast<QComboBox*>(findControl(startDialog, QStringLiteral("session.dialog.connection-strategy")));
    auto* publicHost = qobject_cast<QWidget*>(findControl(startDialog, QStringLiteral("session.dialog.public-host")));
    auto* stunServer = qobject_cast<QWidget*>(findControl(startDialog, QStringLiteral("session.dialog.stun-server")));
    auto* maximum = qobject_cast<QWidget*>(findControl(startDialog, QStringLiteral("session.dialog.maximum-peers")));
    expect(strategy && strategy->count() == 3 && publicHost && stunServer && maximum,
        "connection strategy controls are present");
    if (strategy && publicHost && stunServer && maximum) {
        expect(publicHost->isHidden() && stunServer->isHidden() && !maximum->isHidden(),
            "Server strategy hides public/STUN rows while retaining peer limit");
        strategy->setCurrentIndex(strategy->findData(static_cast<int>(jam2::ConnectionStrategy::Stun)));
        expect(publicHost->isHidden() && !stunServer->isHidden() &&
            startDialog.state().create.strategy == jam2::ConnectionStrategy::Stun,
            "STUN strategy shows only its discovery fields");
        strategy->setCurrentIndex(strategy->findData(static_cast<int>(jam2::ConnectionStrategy::Direct)));
        expect(!publicHost->isHidden() && stunServer->isHidden() && startDialog.state().create.noStun,
            "Direct strategy shows only the public IPv4 field");
        for (auto* form : startDialog.findChildren<QFormLayout*>()) {
            if (auto* label = form->labelForField(stunServer)) expect(label->isHidden(), "hidden STUN field also hides its label");
            if (auto* label = form->labelForField(publicHost)) expect(!label->isHidden(), "Direct public field label is visible");
        }
        strategy->setCurrentIndex(strategy->findData(static_cast<int>(jam2::ConnectionStrategy::Jam2Server)));
    }
    auto* startProfile = qobject_cast<QComboBox*>(findControl(
        startDialog, QStringLiteral("session.dialog.create-profile")));
    auto* startPriority = qobject_cast<QComboBox*>(findControl(
        startDialog, QStringLiteral("session.dialog.os-priority")));
    expect(startProfile != nullptr &&
            hasFastPlaybackDefaults(startDialog.state().create.tuning),
        "create dialog opens with the fully coupled Fast playback defaults");
    if (startProfile != nullptr) {
        for (const auto& name : {QStringLiteral("moderate"), QStringLiteral("safe")}) {
            startProfile->setCurrentIndex(startProfile->findData(name));
            expect(startDialog.state().create.tuning.driftDeadbandPpm == 0,
                "create profile selector disables drift deadband for every maintained preset");
        }
        startProfile->setCurrentIndex(startProfile->findData(QStringLiteral("fast")));
        expect(hasFastPlaybackDefaults(startDialog.state().create.tuning),
            "selecting Fast reapplies every create-side playback default");
    }
    expect(startPriority != nullptr && startPriority->count() == 2 &&
            startPriority->currentData().toString() == QStringLiteral("high") &&
            startPriority->findData(QStringLiteral("realtime")) < 0,
        "create dialog exposes only High and Off scheduling choices");
    if (startPriority != nullptr) {
        startPriority->setCurrentIndex(startPriority->findData(QStringLiteral("off")));
        expect(startDialog.state().create.runtime.osPriority == QStringLiteral("off"),
            "create dialog returns the selected scheduling mode");
    }

    jam2::gui::JoinJamDialogState joinInitial;
    jam2::gui::JoinJamDialog joinDialog(joinInitial, {}, {});
    auto* joinProfile = qobject_cast<QComboBox*>(findControl(
        joinDialog, QStringLiteral("session.join-dialog.profile")));
    auto* joinPriority = qobject_cast<QComboBox*>(findControl(
        joinDialog, QStringLiteral("session.dialog.os-priority")));
    expect(joinProfile != nullptr &&
            hasFastPlaybackDefaults(joinDialog.state().join.tuning),
        "join dialog opens with the fully coupled Fast playback defaults");
    if (joinProfile != nullptr) {
        for (const auto& name : {QStringLiteral("moderate"), QStringLiteral("safe")}) {
            joinProfile->setCurrentIndex(joinProfile->findData(name));
            expect(joinDialog.state().join.tuning.driftDeadbandPpm == 0,
                "join profile selector disables drift deadband for every maintained preset");
        }
        joinProfile->setCurrentIndex(joinProfile->findData(QStringLiteral("fast")));
        expect(hasFastPlaybackDefaults(joinDialog.state().join.tuning),
            "selecting Fast reapplies every join-side playback default");
    }
    expect(joinPriority != nullptr && joinPriority->count() == 2 &&
            joinPriority->currentData().toString() == QStringLiteral("high") &&
            joinPriority->findData(QStringLiteral("realtime")) < 0,
        "join dialog exposes only High and Off scheduling choices");
    if (joinPriority != nullptr) {
        joinPriority->setCurrentIndex(joinPriority->findData(QStringLiteral("off")));
        expect(joinDialog.state().join.runtime.osPriority == QStringLiteral("off"),
            "join dialog returns the selected scheduling mode");
    }
}

void testAudioDeviceUiSupport()
{
    const std::vector<jam2::audio::DeviceInfo> devices{
        {17, "ASIO", "First Device", "{FIRST}", "first.dll"},
        {19, "ASIO", "Second Device", "", "second.dll"},
    };
    expect(jam2::gui::audioDevicePreferenceKey(devices[0]) ==
                QStringLiteral("ASIO|{FIRST}") &&
            jam2::gui::audioDevicePreferenceKey(devices[1]) ==
                QStringLiteral("ASIO|Second Device"),
        "audio device preference keys favor stable driver ids and fall back to names");

    AudioDevicePreference preference;
    expect(!jam2::gui::storeSelectedDevicePreference(
               preference, QStringLiteral("invalid"), devices) &&
            preference.backend.isEmpty() &&
            !jam2::gui::storeSelectedDevicePreference(
               preference, QStringLiteral("999"), devices),
        "audio device preference mapping rejects malformed and unknown ids");
    expect(jam2::gui::storeSelectedDevicePreference(
               preference, QStringLiteral("17"), devices) &&
            preference.backend == QStringLiteral("ASIO") &&
            preference.stableId == QStringLiteral("{FIRST}") &&
            preference.name == QStringLiteral("First Device"),
        "audio device preference mapping stores a stable selected identity");

    QComboBox combo;
    combo.addItem(QStringLiteral("First"), QStringLiteral("17"));
    combo.addItem(QStringLiteral("Second"), QStringLiteral("19"));
    combo.setCurrentIndex(1);
    expect(jam2::gui::storeSelectedDevicePreference(
               preference, &combo, devices) &&
            preference.stableId == QStringLiteral("Second Device") &&
            !jam2::gui::storeSelectedDevicePreference(
               preference, static_cast<const QComboBox*>(nullptr), devices),
        "combo-owned device preference mapping handles selection and null ownership");

    jam2::audio::DeviceTestResult capabilities;
    capabilities.device = devices[0];
    capabilities.device.input_channel_count = 4;
    capabilities.device.output_channel_count = 8;
    capabilities.current_sample_rate = 48000.0;
    capabilities.sample_rate_supported = {true, false};
    capabilities.buffer_size_supported = {true, true, false, false};
    const QString text = jam2::gui::audioDeviceCapabilitiesText(capabilities);
    expect(text.contains(QStringLiteral("Device: ASIO First Device")) &&
            text.contains(QStringLiteral("Input channels: 1,2,3,4\n")) &&
            text.contains(QStringLiteral("Output channels: 1,2,3,4,5,6,7,8\n")) &&
            text.contains(QStringLiteral("44100 Hz: supported")) &&
            text.contains(QStringLiteral("48000 Hz: not supported")) &&
            text.contains(QStringLiteral("32 frames: supported")) &&
            text.contains(QStringLiteral("256 frames: not supported")),
        "audio device capability text reports every exact probed result");

    for (const auto direction : {jam2::audio::EndpointDirection::Input, jam2::audio::EndpointDirection::Output}) {
        auto endpoint = capabilities;
        endpoint.device.backend = "WASAPI";
        endpoint.device.direction = direction;
        endpoint.device.channel_count = 6;
        const auto endpointText = jam2::gui::audioDeviceCapabilitiesText(endpoint);
        expect(endpointText.contains(direction == jam2::audio::EndpointDirection::Input
                   ? QStringLiteral("Input channels: 1,2,3,4,5,6\nOutput channels: None\n")
                   : QStringLiteral("Input channels: None\nOutput channels: 1,2,3,4,5,6\n")),
            "WASAPI capability counts follow the tested endpoint direction");
    }
    struct ChannelCase { unsigned inputs, outputs; QString expected; };
    for (const auto& entry : {
             ChannelCase{0, 2, QStringLiteral("Input channels: None\nOutput channels: 1,2\n")},
             ChannelCase{1, 0, QStringLiteral("Input channels: 1\nOutput channels: None\n")},
             ChannelCase{12, 24, QStringLiteral("Input channels: 1,2,3,4,5,6,7,8,9,10,11,12\n"
                 "Output channels: 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24\n")}}) {
        auto coreaudio = capabilities;
        coreaudio.device.backend = "CoreAudio";
        coreaudio.device.input_channel_count = entry.inputs;
        coreaudio.device.output_channel_count = entry.outputs;
        const auto coreText = jam2::gui::audioDeviceCapabilitiesText(coreaudio);
        expect(coreText.contains(entry.expected),
            "CoreAudio results enumerate one-based channels without truncation and identify unsupported directions");
    }

    bool inspected = false;
    QTimer::singleShot(0, [&inspected, &text] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr || dialog->windowTitle() != QStringLiteral("Test Device")) {
            return;
        }
        auto* message = dialog->findChild<QLabel*>(
            QStringLiteral("AudioDeviceTestMessage"));
        inspected = message != nullptr && message->text() == text &&
            clickControl(
                *dialog, QStringLiteral("application.device-test-dialog.ok"));
    });
    jam2::gui::showAudioDeviceTestMessage(nullptr, text);
    expect(inspected,
        "audio device result dialog exposes exact text and its registered close action");
}

void testConnectionGuidance()
{
    const QString creator = jam2::gui::creatorFirewallGuidance();
    const QString joiner = jam2::gui::joinerFirewallGuidance();
#if defined(_WIN32)
    expect(creator.contains(QStringLiteral("Windows Firewall")) &&
            creator.contains(QStringLiteral("before authentication")),
        "creator diagnostics provide exact Windows firewall recovery guidance");
#else
    expect(!creator.trimmed().isEmpty(),
        "creator diagnostics provide platform firewall recovery guidance");
#endif
    expect(joiner.contains(QStringLiteral("No authenticated UDP")) &&
            joiner.contains(QStringLiteral("invite address")) &&
            joiner.contains(QStringLiteral("both computers")),
        "joiner diagnostics distinguish coordinator and local firewall checks");
}

void testMeterWidget()
{
    LevelMeterWidget meter;
    meter.resize(220, 18);
    meter.setLevel(-1.0);
    meter.setLevel(0.004);
    meter.setLevel(0.6);
    meter.setLevel(1.5);
    processPaint(meter);
    meter.setEnabled(false);
    meter.setLevel(0.25);
    processPaint(meter);
}

QVector<LooperLaneStackWidget::LaneView> looperLanes()
{
    QVector<LooperLaneStackWidget::LaneView> lanes;
    LooperLaneStackWidget::LaneView imported;
    imported.lane.id = QStringLiteral("lane-imported");
    imported.lane.name = QStringLiteral("Imported guitar");
    imported.lane.assetPath = QStringLiteral("C:/audio/guitar.wav");
    imported.lane.sampleRate = 48000;
    imported.lane.sampleRateCompatible = true;
    imported.lane.sourceFrames = 48000;
    imported.lane.startFrame = 2400;
    imported.lane.gainDb = -6.0;
    imported.lane.originKind = QStringLiteral("imported");
    imported.assetPath = imported.lane.assetPath;
    imported.sourceFrames = imported.lane.sourceFrames;
    imported.peaks = {0.1f, 0.5f, 1.0f, 0.3f};
    lanes.push_back(imported);

    LooperLaneStackWidget::LaneView remote;
    remote.lane.id = QStringLiteral("lane-remote");
    remote.lane.name = QStringLiteral("Remote drums");
    remote.lane.assetHash = QStringLiteral("abc123");
    remote.lane.sampleRate = 44100;
    remote.lane.sampleRateCompatible = false;
    remote.lane.sourceFrames = 44100;
    remote.lane.startFrame = 12000;
    remote.lane.muted = true;
    remote.lane.originKind = QStringLiteral("peer");
    remote.sourceFrames = remote.lane.sourceFrames;
    lanes.push_back(remote);

    LooperLaneStackWidget::LaneView generated;
    generated.lane.id = QStringLiteral("lane-generated");
    generated.lane.name = QStringLiteral("Generated bass");
    generated.lane.assetPath = QStringLiteral("C:/audio/bass.wav");
    generated.lane.sampleRate = 48000;
    generated.lane.sampleRateCompatible = true;
    generated.lane.sourceFrames = 24000;
    generated.lane.solo = true;
    generated.lane.referenceKind = QStringLiteral("bass");
    generated.lane.originKind = QStringLiteral("generated");
    generated.assetPath = generated.lane.assetPath;
    generated.sourceFrames = generated.lane.sourceFrames;
    lanes.push_back(generated);
    return lanes;
}

void testLooperWaveformGridAlignment()
{
    // Compare the actual painted waveform with a silent version of the same
    // lane so grid lines, labels, borders and handle colours cancel out.
    for (const int width : {1200, 2200}) {
        for (const bool cropped : {false, true}) {
            LooperLaneStackWidget widget;
            widget.resize(width, 260);
            auto lane = looperLanes().front();
            lane.sourceFrames = lane.lane.sourceFrames = 48000;
            lane.lane.startFrame = cropped ? 6000 : 0;
            lane.lane.loopStartFrame = cropped ? 12000 : -1;
            lane.lane.loopEndFrame = cropped ? 48000 : -1;
            lane.peaks.assign(512, 0.0f);
            widget.setLanes({lane}, -1, 0, -1, 48000, 96000, 120.0, 1, true);
            widget.show();
            QApplication::processEvents();
            const QImage silent = widget.grab().toImage();
            for (const int bucket : {0, 128, 384}) {
                std::fill_n(lane.peaks.begin() + bucket, 6, 1.0f);
            }
            widget.setLanes({lane}, -1, 0, -1, 48000, 96000, 120.0, 1, true);
            const QImage pulses = widget.grab().toImage();
            const QString artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
            if (!artifacts.isEmpty()) {
                expect(pulses.save(QDir(artifacts).filePath(
                    QStringLiteral("waveform-%1-%2.png").arg(width).arg(cropped ? "trimmed" : "full"))),
                    "painted waveform evidence is saved in the test artifact directory");
            }
            const double scale = pulses.devicePixelRatio();
            std::vector<int> starts;
            bool previous = false;
            for (int x = 294; x < widget.width() - 10; ++x) {
                const QPoint point(qRound(x * scale), qRound(85 * scale));
                const bool changed = silent.pixel(point) != pulses.pixel(point);
                if (changed && !previous) starts.push_back(x);
                previous = changed;
            }
            const std::vector<qint64> frames = cropped
                ? std::vector<qint64>{6000, 30000}
                : std::vector<qint64>{0, 12000, 36000};
            expect(starts.size() == frames.size(),
                "painted waveform retains the first attack and excludes cropped source attacks");
            for (std::size_t i = 0; i < (std::min)(starts.size(), frames.size()); ++i) {
                const int expected = 296 + qRound(
                    static_cast<double>(frames[i]) / 96000 * (widget.width() - 306));
                expect(std::abs(starts[i] - expected) <= 2,
                    "first and later painted attacks share the grid scale across resize, placement and crop");
            }
        }
    }
}

void testPendingWavWidget()
{
    LooperLaneStackWidget widget;
    widget.resize(1200, 320);
    auto pending = looperLanes()[1];
    pending.transfer = {pending.lane, TrackWavAvailability::Waiting};
    auto playing = looperLanes()[0];
    widget.setLanes({playing, pending}, 0, 0, -1, 48000, 96000, 120, 1, true);
    int mutations = 0;
    widget.onRemove = [&](int) { ++mutations; };
    widget.onWavDropped = [&](int, const QString&) { ++mutations; };
    widget.onRegionCommitted = [&](int, qint64, qint64, qint64) { ++mutations; };
    widget.show();
    QApplication::processEvents();
    const QImage waiting = widget.grab().toImage();
    QString error;
    expect(!widget.invokeGuiVirtualControl("looper.lane.1.remove", "click", {}, error) &&
        !widget.invokeGuiVirtualControl("looper.lane.1.wav.drop", "drop-file", "other.wav", error) &&
        !widget.invokeGuiVirtualControl("looper.lane.1.region", "set-region",
            QVariantMap{{"start_frame", 0}, {"source_start_frame", 0}, {"source_end_frame", 100}}, error) &&
        mutations == 0, "pending WAV controls cannot mutate the playing project");
    widget.setWavTransferProgress(pending.lane.assetHash, 25, 100);
    const QImage quarter = widget.grab().toImage();
    const QString artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    if (!artifacts.isEmpty()) expect(quarter.save(QDir(artifacts).filePath("pending-wav-progress.png")),
        "pending WAV presentation is saved for visual inspection");
    expect(widget.laneViews()[1].transfer.receivedBytes == 25 &&
        widget.laneViews()[1].transfer.availability == TrackWavAvailability::Receiving &&
        waiting != quarter, "accepted progress is visible inside the pending clip");
    widget.setWavTransferProgress("unrelated", 90, 100);
    expect(widget.laneViews()[1].transfer.receivedBytes == 25,
        "another WAV cannot update this clip's progress");
    widget.setWavTransferProgress(pending.lane.assetHash, 100, 100);
    expect(widget.laneViews()[1].transfer.availability == TrackWavAvailability::Validating &&
        widget.grab().toImage() != quarter, "100 percent displays validation before waveform installation");
    expect(widget.invokeGuiVirtualControl("looper.lane.0.remove", "click", {}, error) && mutations == 1,
        "existing lanes remain editable while another WAV is pending");
    widget.setLanes({playing}, 0, 0, -1, 48000, 96000, 120, 1, true);
    expect(widget.laneViews().size() == 1 && !widget.laneViews()[0].transfer.pending(),
        "removing pending metadata removes its visible placeholder");
}

void testLooperLaneWidget()
{
    LooperLaneStackWidget widget;
    widget.resize(1200, 520);
    QVector<LooperLaneStackWidget::LaneView> lanes = looperLanes();

    int selected = -1;
    int addLane = 0;
    int addWav = 0;
    int mute = -1;
    int solo = -1;
    int arm = -1;
    int rename = -1;
    int remove = -1;
    int reveal = -1;
    int removeWav = -1;
    int analyze = -1;
    int gainLane = -1;
    double gainValue = 0.0;
    int regionLane = -1;
    qint64 regionStart = -1;
    qint64 regionSourceStart = -1;
    qint64 regionSourceEnd = -1;
    QString droppedPath;
    int droppedLane = -2;
    widget.onSelected = [&](int value) { selected = value; };
    widget.onAddLane = [&] { ++addLane; };
    widget.onAddWav = [&] { ++addWav; };
    widget.onMute = [&](int value) { mute = value; };
    widget.onSolo = [&](int value) { solo = value; };
    widget.onArm = [&](int value) { arm = value; };
    widget.onRename = [&](int value) { rename = value; };
    widget.onRemove = [&](int value) { remove = value; };
    widget.onRevealWav = [&](int value) { reveal = value; };
    widget.onRemoveWav = [&](int value) { removeWav = value; };
    widget.onAnalyzeWav = [&](int value) { analyze = value; };
    widget.onGainChanged = [&](int lane, double gain) {
        gainLane = lane;
        gainValue = gain;
    };
    widget.onRegionCommitted = [&](int lane, qint64 start, qint64 sourceStart, qint64 sourceEnd) {
        regionLane = lane;
        regionStart = start;
        regionSourceStart = sourceStart;
        regionSourceEnd = sourceEnd;
    };
    widget.onWavDropped = [&](int lane, const QString& path) {
        droppedLane = lane;
        droppedPath = path;
    };

    widget.setLanes(lanes, 0, -3, 0, 0, 96000, 0.0, 3, true);
    widget.setGridPosition(-1, true, 0.0, 0, 3);
    widget.setGridPosition(500, true, 120.0, 4, 1);
    widget.setGridPosition(750, true, 120.0, 4, 1);
    widget.setPlaybackMarkers(-1, -1, -1);
    widget.setPlaybackMarkers(500, 250, 1500);
    widget.setPlaybackMarkers(750, 250, 1500);
    widget.setRemoteRecordingStates(
        {{QStringLiteral("lane-remote"), QStringLiteral("Peer recording")}}, false);
    widget.setLiveRecordingEndFrame(0);
    widget.setLiveRecordingEndFrame(100000);
    widget.setLiveRecordingEndFrame(100000);
    widget.setTimelineZoomLevel(-1, 900);
    expect(widget.timelineZoomLevel() == 0 && widget.minimumWidth() == 0,
        "looper timeline zoom bounds to fit mode");
    widget.setTimelineZoomLevel(99, 900);
    expect(widget.timelineZoomLevel() == 6 && widget.minimumWidth() > 900,
        "looper timeline zoom bounds to maximum expansion");
    widget.setTimelineZoomLevel(0, 900);
    widget.resize(1200, 520);
    processPaint(widget);

    const QVector<jam2::gui::GuiVirtualControl> controls = widget.guiVirtualControls();
    expect(controls.size() >= 30,
        "looper inventory exposes toolbar and repeated lane actions");
    QString error;
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.add-empty"), QStringLiteral("set-value"), {}, error),
        "looper toolbar rejects non-click operation");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.add-empty"), QStringLiteral("click"), {}, error) &&
            widget.invokeGuiVirtualControl(
               QStringLiteral("looper.wav.import"), QStringLiteral("click"), {}, error),
        "looper toolbar virtual actions dispatch");
    expect(addLane == 1 && addWav == 1, "looper toolbar callbacks are exact");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.select"), QStringLiteral("click"), {}, error) && selected == 0,
        "looper selection virtual action dispatches");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.select"), QStringLiteral("set-value"), {}, error) &&
            !widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.99.select"), QStringLiteral("click"), {}, error),
        "looper rejects invalid selection operation and stale lane");

    const QVector<QPair<QString, int*>> clicks{
        {QStringLiteral("mute"), &mute},
        {QStringLiteral("solo"), &solo},
        {QStringLiteral("arm"), &arm},
        {QStringLiteral("rename"), &rename},
        {QStringLiteral("remove"), &remove},
        {QStringLiteral("wav.reveal"), &reveal},
        {QStringLiteral("wav.remove"), &removeWav},
        {QStringLiteral("wav.analyze"), &analyze},
    };
    for (const auto& action : clicks) {
        error.clear();
        expect(widget.invokeGuiVirtualControl(
                   QStringLiteral("looper.lane.0.") + action.first,
                   QStringLiteral("click"), {}, error),
            "looper lane click virtual action dispatches");
        expect(*action.second == 0, "looper lane click callback receives exact lane");
    }
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.gain"), QStringLiteral("set-value"), -12.5, error) &&
            gainLane == 0 && approximatelyEqual(gainValue, -12.5),
        "looper gain virtual action validates and dispatches");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.gain"), QStringLiteral("set-value"), 13.0, error) &&
            !widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.gain"), QStringLiteral("set-value"),
               std::numeric_limits<double>::quiet_NaN(), error),
        "looper gain rejects range and non-finite values");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.wav.drop"), QStringLiteral("drop-file"),
               QStringLiteral("C:/audio/replacement.wav"), error) &&
            droppedLane == 0 && droppedPath.endsWith(QStringLiteral("replacement.wav")),
        "looper virtual WAV drop dispatches a complete local path");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.wav.drop"), QStringLiteral("drop-file"), QString(), error),
        "looper virtual WAV drop rejects an empty path");
    const QVariantMap validRegion{
        {QStringLiteral("start_frame"), 1200},
        {QStringLiteral("source_start_frame"), 100},
        {QStringLiteral("source_end_frame"), 12000},
    };
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.region"), QStringLiteral("set-region"), validRegion, error) &&
            regionLane == 0 && regionStart == 1200 &&
            regionSourceStart == 100 && regionSourceEnd == 12000,
        "looper virtual region dispatches validated frame bounds");
    QVariantMap invalidRegion = validRegion;
    invalidRegion.insert(QStringLiteral("source_end_frame"), 60000);
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.region"), QStringLiteral("set-region"), invalidRegion, error) &&
            !widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.unknown"), QStringLiteral("click"), {}, error),
        "looper rejects out-of-source region and unavailable action");

    widget.setInteractionsProtected(true);
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.0.mute"), QStringLiteral("click"), {}, error) &&
            !widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.add-empty"), QStringLiteral("click"), {}, error),
        "protected looper rejects mutations");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("looper.lane.1.select"), QStringLiteral("click"), {}, error) && selected == 1,
        "protected looper still permits selection");
    widget.setInteractionsProtected(false);

    // Exercise the same real custom-painted hit targets used by the mouse UI.
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(40, 112), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(95, 112), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(166, 112), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(180, 82), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(240, 82), Qt::LeftButton, Qt::LeftButton);
    expect(mute == 0 && solo == 0 && arm == 0 && rename == 0 && remove == 0,
        "looper painted lane buttons dispatch through mouse coordinates");

    analyze = -1;
    reveal = -1;
    removeWav = -1;
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(941, 64), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(1053, 64), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(1139, 64), Qt::LeftButton, Qt::LeftButton);
    expect(analyze == 0 && reveal == 0 && removeWav == 0,
        "looper painted WAV actions dispatch through mouse coordinates");

    gainLane = -1;
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(100, 144), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, QPointF(200, 144), Qt::NoButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, QPointF(200, 144), Qt::LeftButton, Qt::NoButton);
    expect(gainLane == 0 && gainValue > -10.0,
        "looper painted gain drag commits bounded gain");

    regionLane = -1;
    lanes = looperLanes();
    widget.setLanes(lanes, 0, 0, -1, 48000, 96000, 120.0, 1, true);
    processPaint(widget);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(520, 104), Qt::LeftButton, Qt::LeftButton);
    // Refreshing with the same stable lane id must not erase an in-flight preview.
    widget.setLanes(looperLanes(), 0, 0, -1, 48000, 96000, 120.0, 1, true);
    sendMouse(widget, QEvent::MouseMove, QPointF(570, 104), Qt::NoButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, QPointF(570, 104), Qt::LeftButton, Qt::NoButton);
    expect(regionLane == 0 && regionStart >= 0 && regionSourceEnd > regionSourceStart,
        "looper timeline drag survives a matching-id refresh and commits a valid region");

    // End the earlier live-recording simulation so edge coordinates use the
    // deterministic 96,000-frame minimum view configured below.
    widget.setLiveRecordingEndFrame(0);
    regionLane = -1;
    widget.setLanes(looperLanes(), 0, 0, -1, 48000, 96000, 120.0, 1, true);
    processPaint(widget);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(318, 104), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, QPointF(500, 104), Qt::NoButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, QPointF(500, 104), Qt::LeftButton, Qt::NoButton);
    expect(regionLane == 0 && regionStart > 2400 && regionSourceStart > 0 &&
            regionSourceEnd == 48000,
        "looper left-edge drag trims the source and advances the arrangement start");

    regionLane = -1;
    widget.setLanes(looperLanes(), 0, 0, -1, 48000, 96000, 120.0, 1, true);
    processPaint(widget);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(765, 104), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, QPointF(600, 104), Qt::NoButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, QPointF(600, 104), Qt::LeftButton, Qt::NoButton);
    expect(regionLane == 0 && regionStart == 2400 && regionSourceStart == 0 &&
            regionSourceEnd > 0 && regionSourceEnd < 48000,
        "looper right-edge drag shortens the source without moving its arrangement start");

    // The plus row is below three 140-pixel lanes.
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(250, 474), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(900, 474), Qt::LeftButton, Qt::LeftButton);
    expect(addLane >= 2 && addWav >= 2,
        "looper painted add/import actions dispatch");

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/audio/drop.wav"))});
    QDragEnterEvent enter(QPoint(500, 104), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &enter);
    expect(enter.isAccepted(), "looper accepts one local-file drag");
    QDragMoveEvent move(QPoint(500, 104), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    QDropEvent drop(QPointF(500, 104), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &drop);
    expect(drop.isAccepted() && droppedLane == 0 && droppedPath.endsWith(QStringLiteral("drop.wav")),
        "looper local-file drop resolves the painted lane");
    QDragLeaveEvent leave;
    QApplication::sendEvent(&widget, &leave);
    expect(leave.isAccepted(), "looper drag leave clears preview state");

    widget.setInteractionsProtected(true);
    QDragEnterEvent protectedEnter(
        QPoint(500, 104), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &protectedEnter);
    expect(!protectedEnter.isAccepted(), "protected looper rejects local-file drag entry");
    widget.setInteractionsProtected(false);

    QMimeData multipleMime;
    multipleMime.setUrls({
        QUrl::fromLocalFile(QStringLiteral("C:/audio/one.wav")),
        QUrl::fromLocalFile(QStringLiteral("C:/audio/two.wav")),
    });
    QDragEnterEvent multipleEnter(
        QPoint(500, 104), Qt::CopyAction, &multipleMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &multipleEnter);
    expect(!multipleEnter.isAccepted(), "looper rejects ambiguous multi-file drag entry");
    processPaint(widget);
}

void testMetronomeWidgets()
{
    MetronomeNebulaWidget nebula;
    nebula.resize(640, 150);
    nebula.setBpm(-1);
    nebula.setBpm(500);
    nebula.setPulseState(-1, -1.0, 3, 2.0, 100.0, true);
    nebula.setPulseState(2, 0.5, 1, 0.25, 12.0, true);
    nebula.setPulseState(2, 0.5, 1, 0.25, 12.0, true);
    processPaint(nebula);
    nebula.hide();
    QApplication::processEvents();
    nebula.show();
    nebula.setPulseState(0, 0.0, 0, 0.0, 0.0, false);
    processPaint(nebula);

    MetronomePatternWidget pattern;
    pattern.resize(720, 154);
    int changedStep = -1;
    bool changedEnabled = false;
    bool changedAccent = false;
    int changes = 0;
    pattern.onStepChanged = [&](int step, bool enabled, bool accent) {
        changedStep = step;
        changedEnabled = enabled;
        changedAccent = accent;
        ++changes;
    };
    pattern.setPattern(2, 2, 1, {true, false, true, true, true}, {true, false});
    pattern.setCurrentStep(99, true);
    processPaint(pattern);
    const auto controls = pattern.guiVirtualControls();
    expect(controls.size() == 4 && controls.at(3).state.value(QStringLiteral("current")).toBool(),
        "metronome pattern exposes each bounded step and current state");
    QString error;
    expect(pattern.invokeGuiVirtualControl(
               QStringLiteral("metronome.pattern.step.1"), QStringLiteral("cycle"), {}, error) &&
            changedStep == 1 && changedEnabled && !changedAccent,
        "metronome virtual cycle advances muted to hit");
    expect(pattern.invokeGuiVirtualControl(
               QStringLiteral("metronome.pattern.step.1"), QStringLiteral("set-state"), 2, error) &&
            changedAccent,
        "metronome virtual set-state applies accent");
    expect(!pattern.invokeGuiVirtualControl(
               QStringLiteral("metronome.pattern.bad"), QStringLiteral("cycle"), {}, error) &&
            !pattern.invokeGuiVirtualControl(
               QStringLiteral("metronome.pattern.step.99"), QStringLiteral("cycle"), {}, error) &&
            !pattern.invokeGuiVirtualControl(
               QStringLiteral("metronome.pattern.step.0"), QStringLiteral("set-state"), 4, error),
        "metronome virtual controls reject invalid id, stale step, and state");
    const int beforeMouse = changes;
    sendMouse(pattern, QEvent::MouseButtonPress, QPointF(190, 90), Qt::LeftButton, Qt::LeftButton);
    expect(changes == beforeMouse + 1,
        "metronome painted step cycles through a real mouse event");
    sendMouse(pattern, QEvent::MouseButtonPress, QPointF(5, 5), Qt::LeftButton, Qt::LeftButton);
    sendMouse(pattern, QEvent::MouseButtonPress, QPointF(190, 90), Qt::MiddleButton, Qt::MiddleButton);
    pattern.setCurrentStep(0, false);
    processPaint(pattern);
}

void testBeatGridWidgets()
{
    BeatGridModel model;
    model.resizeSection(0, 132);
    model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7/G"));
    model.setCell(0, QStringLiteral("chord"), 4, QStringLiteral("Fmaj7"));
    model.setCell(0, QStringLiteral("target"), 0, QStringLiteral("E4"));
    model.setCell(0, QStringLiteral("lyric"), 0, QStringLiteral("opening line"));
    model.setBeatHit(0, 0, 0, QStringLiteral("xag."));
    model.section(3).generatedKind = QStringLiteral("practice");

    QWidget paginationHost;
    QVBoxLayout paginationLayout(&paginationHost);
    BeatGridWidget chord(&model, QStringLiteral("chord"));
    chord.resize(1000, 700);
    QWidget* pagination = chord.createOverviewPagination(&paginationHost);
    paginationLayout.addWidget(pagination);
    paginationHost.show();
    int selected = -1;
    int resizedSection = -1;
    int resizedBeats = 0;
    int shrinkSection = -1;
    int structureChanges = 0;
    chord.onSelectedSectionChanged = [&](int value) { selected = value; };
    chord.onGridResized = [&](int section, int beats, int) {
        resizedSection = section;
        resizedBeats = beats;
    };
    chord.onShrinkRequested = [&](int section) { shrinkSection = section; };
    chord.onStructureChanged = [&] { ++structureChanges; };
    expect(&chord.model() == &model, "beat-grid widget exposes its supplied model");
    chord.setSelectedSectionIndex(99);
    expect(chord.selectedSectionIndex() == 3 && selected == 3,
        "beat-grid selection bounds to the final section");
    chord.focusGeneratedSection(QStringLiteral("chord"));
    expect(chord.selectedSectionIndex() == 3,
        "practice generated section matches chord focus alias");
    chord.focusGeneratedSection(QStringLiteral("missing"));
    chord.setSelectedSectionIndex(0);
    chord.setBeatsPerBar(0);
    chord.setBeatsPerBar(4);
    chord.setFocusCurrentBar(true);
    chord.setGridPosition(131, 3, true, 1.5);
    chord.toggleFocusCurrentBar();
    chord.toggleFocusCurrentBar();
    chord.setGridPosition(130, 2, true, 0.5);
    chord.setUpcomingSection(-1, 0, 0, 0, true, false);
    chord.setUpcomingSection(1, 0, 4, 4, true, false);
    chord.setUpcomingSection(1, 9, 4, 4, true, false);
    chord.setUpcomingSection(1, 8, 4, 4, true, false);
    chord.setUpcomingSection(1, 2, 4, 4, true, false);
    chord.setUpcomingSection(1, 2, 4, 4, true, true);
    chord.setEditingProtected(true);
    chord.setEditingProtected(false);
    chord.applyRemoteCell(0, QStringLiteral("chord"), 1, QStringLiteral("Dm7"));
    processPaint(chord);

    expect(clickControl(chord, QStringLiteral("grid.chord.section.expand")),
        "beat-grid expand control is discoverable");
    expect(resizedSection == 0 && resizedBeats == 136 && model.section(0).beats == 136,
        "beat-grid expand adds exactly one current bar");
    expect(clickControl(chord, QStringLiteral("grid.chord.section.shrink")) && shrinkSection == 0,
        "beat-grid shrink delegates conflict-aware decision to owner");
    expect(clickControl(*pagination, QStringLiteral("grid.chord.overview.page.last")) &&
            clickControl(*pagination, QStringLiteral("grid.chord.overview.page.previous")) &&
            clickControl(*pagination, QStringLiteral("grid.chord.overview.page.next")) &&
            clickControl(*pagination, QStringLiteral("grid.chord.overview.page.first")),
        "beat-grid overview pagination dispatches every direction");

    auto* strings = qobject_cast<QComboBox*>(findControl(
        chord, QStringLiteral("grid.chord.section.0.chord-reference.strings")));
    auto* tuning = qobject_cast<QComboBox*>(findControl(
        chord, QStringLiteral("grid.chord.section.0.chord-reference.tuning")));
    expect(strings != nullptr && tuning != nullptr,
        "beat-grid chord reference exposes strings and tuning controls");
    if (strings != nullptr && tuning != nullptr) {
        strings->setCurrentIndex(strings->findData(7));
        tuning->setCurrentIndex(tuning->findData(true));
        QApplication::processEvents();
    }
    expect(structureChanges == 2 && model.guitarStringCount() == 7 && model.guitarDropTuning(),
        "chord reference changes emit structure updates and persist model state");
    expect(clickControl(
               chord, QStringLiteral("grid.chord.section.0.musical-lines.toggle")) &&
            clickControl(
               chord, QStringLiteral("grid.chord.section.0.chord-reference.toggle")),
        "chord detail visibility controls rebuild safely");
    processPaint(chord);

    BeatGridWidget beat(&model, QStringLiteral("beat"));
    beat.resize(1000, 700);
    QWidget beatPaginationHost;
    QVBoxLayout beatPaginationLayout(&beatPaginationHost);
    beatPaginationLayout.addWidget(beat.createOverviewPagination(&beatPaginationHost));
    int beatDivisionEdits = 0;
    int beatHitEdits = 0;
    beat.onBeatDivisionChanged = [&](int section, int beatIndex, int division, int) {
        if (section == 0 && beatIndex == 0 && division > 0) ++beatDivisionEdits;
    };
    beat.onBeatHitEdited = [&](int section, int beatIndex, int lane, const QString&, int) {
        if (section == 0 && beatIndex == 0 && lane == 0) ++beatHitEdits;
    };
    beat.setFocusCurrentBar(true);
    beat.setGridPosition(132, 0, true, 0.25);
    beat.setUpcomingSection(1, 5, 4, 4, true, false);
    beat.applyRemoteCell(0, QStringLiteral("beat"), 2, QStringLiteral("remote note"));
    processPaint(beat);
    expect(findControl(
               beat, QStringLiteral("grid.beat.section.0.beat.132.division")) != nullptr,
        "beat-grid focus follows a high live bar");
    beat.setGridPosition(0, 0, true, 0.25);
    QApplication::processEvents();
    expect(clickControl(
               beat, QStringLiteral("grid.beat.section.0.beat.0.division")) &&
            beatDivisionEdits == 1,
        "beat-grid division button advances the real model");
    expect(clickControl(
               beat, QStringLiteral("grid.beat.section.0.beat.0.lane.0.step.0")) &&
            beatHitEdits == 1,
        "beat-grid painted step button advances hit state");
    processPaint(beat);
    QVector<QPair<QPointer<QWidget>, bool>> hiddenPlayheads;
    for (QWidget* child : beat.findChildren<QWidget*>()) {
        if (child->property("authoringBeat").isValid())
            hiddenPlayheads.push_back({child, child->property("playhead").toBool()});
    }
    expect(!hiddenPlayheads.isEmpty(), "beat editor exposes real playhead controls");
    beat.hide();
    beat.setGridPosition(1, 0, true, 0.25);
    beat.setUpcomingSection(1, 2, 4, 3, true, false);
    for (const auto& state : hiddenPlayheads) {
        expect(state.first && state.first->property("playhead").toBool() == state.second,
            "hidden editor preserves its controls without styling each beat update");
    }
    processPaint(beat);
    bool resumedPlayhead = false;
    for (QWidget* child : beat.findChildren<QWidget*>()) {
        if (child->property("authoringBeat").isValid() &&
            child->property("authoringBeat").toInt() == 1 && child->property("playhead").toBool())
            resumedPlayhead = true;
    }
    expect(resumedPlayhead, "shown editor immediately applies the latest hidden transport state");

    BeatGridWidget lyrics(&model, QStringLiteral("lyric"));
    lyrics.resize(900, 650);
    QWidget lyricPaginationHost;
    QVBoxLayout lyricPaginationLayout(&lyricPaginationHost);
    QWidget* lyricPagination = lyrics.createOverviewPagination(&lyricPaginationHost);
    lyricPaginationLayout.addWidget(lyricPagination);
    int lyricEdits = 0;
    lyrics.onCellEdited = [&](int, const QString& lane, int, const QString&, int) {
        if (lane == QStringLiteral("lyric")) ++lyricEdits;
    };
    lyrics.setFocusCurrentBar(true);
    lyrics.setGridPosition(8, 0, true, 0.0);
    processPaint(lyrics);
    expect(!lyricPagination->isVisible(), "lyrics intentionally omit overview pagination");
    auto* lyric = qobject_cast<QPlainTextEdit*>(findControl(
        lyrics, QStringLiteral("grid.lyric.section.0.bar.0.lyric")));
    expect(lyric != nullptr, "lyric bar editor is discoverable");
    if (lyric != nullptr) {
        QApplication::clipboard()->setText(QStringLiteral("first line\nsecond line"));
        QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QApplication::sendEvent(lyric, &paste);
        QApplication::processEvents();
        QApplication::processEvents();
    }
    expect(model.section(0).lyrics.at(0) == QStringLiteral("first line") &&
            model.section(0).lyrics.at(4) == QStringLiteral("second line"),
        "multiline lyric paste distributes one line per bar");
    lyric = qobject_cast<QPlainTextEdit*>(findControl(
        lyrics, QStringLiteral("grid.lyric.section.0.bar.0.lyric")));
    if (lyric != nullptr) {
        QKeyEvent advance(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(lyric, &advance);
        QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
        QApplication::sendEvent(lyric, &newline);
    }
    expect(clickControl(
               lyrics, QStringLiteral("grid.lyric.section.0.bar.0.lyric.clear")),
        "lyric clear control is discoverable");
    lyrics.applyRemoteCell(0, QStringLiteral("lyric"), 0, QStringLiteral("remote lyric"));
    expect(model.section(0).lyrics.at(0) == QStringLiteral("remote lyric") && lyricEdits >= 3,
        "lyric local edits and remote application converge in the model");
    processPaint(lyrics);
}

void testResponsivePerformanceActions()
{
    BeatGridModel model;
    model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
    PerformanceHomeWidget widget;
    widget.setAttribute(Qt::WA_DontShowOnScreen, true);
    widget.setSongModel(&model);
    std::array<int, 6> clicks{};
    widget.onGenerateIdea = [&] { ++clicks[0]; };
    widget.onBrowseIdeas = [&] { ++clicks[4]; };
    widget.onContinueIdea = [&] { ++clicks[1]; };
    widget.onClearIdea = [&] { ++clicks[2]; };
    widget.onGenerateWav = [&] { ++clicks[3]; };
    widget.onJamTaster = [&] { ++clicks[5]; };
    const QStringList actionIds{
        QStringLiteral("performance.home.idea.generate"),
        QStringLiteral("performance.home.idea.continue"),
        QStringLiteral("performance.home.idea.clear"),
        QStringLiteral("performance.home.idea.generate-wav"),
        QStringLiteral("performance.home.idea.browse"),
        QStringLiteral("performance.home.idea.jam-taster"),
    };
    const auto boundsFor = [](const QVector<jam2::gui::GuiVirtualControl>& controls, const QString& id,
                             const QString& field = QStringLiteral("bounds")) {
        for (const auto& control : controls) {
            if (control.id != id) continue;
            const auto bounds = control.state.value(field).toMap();
            return QRect(bounds.value(QStringLiteral("x")).toInt(), bounds.value(QStringLiteral("y")).toInt(),
                bounds.value(QStringLiteral("width")).toInt(), bounds.value(QStringLiteral("height")).toInt());
        }
        return QRect{};
    };
    int cases = 0;
    int wrappedCases = 0;
    int unwrappedCases = 0;
    std::map<int, QSize> coreSizes;
    for (const QSize size : {QSize(1680, 950), QSize(1280, 900), QSize(1280, 720), QSize(1280, 500),
             QSize(1100, 620), QSize(900, 500), QSize(850, 650), QSize(850, 500),
             QSize(820, 500), QSize(1440, 900), QSize(1280, 900)}) {
        QRect idleChord;
        bool idleWrapped = false;
        for (bool generating : {false, true}) {
            widget.resize(size);
            widget.setWavGenerationActive(generating);
            processPaint(widget);
            const auto controls = widget.guiVirtualControls();
            const QRect chord = boundsFor(controls, QStringLiteral("performance.home.open.chords.current"));
            const QRect runway = boundsFor(controls, QStringLiteral("performance.home.open.chords.runway"));
            const QRect beatPreview = boundsFor(controls, QStringLiteral("performance.home.open.beats.current"));
            const QRect arrangement = boundsFor(controls, QStringLiteral("performance.home.arrangement"));
            expect(!chord.isEmpty() && widget.rect().contains(chord),
                "central chord visualiser stays inside the resized main view");
            expect(chord.width() >= 184, "short windows retain a full-size core chord display");
            expect(!chord.intersects(runway) && !chord.intersects(beatPreview),
                "upcoming chords and beat previews do not cover the preserved core chord display");
            const auto [core, inserted] = coreSizes.emplace(size.width(), chord.size());
            expect(inserted || core->second == chord.size(),
                "reducing window height never shrinks the current-chord display");
            const auto wav = std::find_if(controls.cbegin(), controls.cend(), [&](const auto& control) {
                return control.id == actionIds[3];
            });
            expect(wav != controls.cend(), "WAV action exposes its layout state");
            const bool wrapped = wav != controls.cend() &&
                wav->state.value(QStringLiteral("actions_wrapped")).toBool();
            const int upperY = boundsFor(controls, actionIds[0]).top();
            const int lowerY = boundsFor(controls, actionIds[3]).top();
            if (wrapped) {
                ++wrappedCases;
                expect(lowerY > upperY,
                    "wrapped tiles keep the first three actions above the remaining three");
            } else {
                ++unwrappedCases;
                expect(lowerY == upperY, "unwrapped main-view actions occupy one row");
            }
            if (!generating) {
                idleChord = chord;
                idleWrapped = wrapped;
            } else {
                expect(chord == idleChord && wrapped == idleWrapped,
                    "starting WAV generation preserves chord size, position and tile wrapping");
            }
            QVector<QRect> actionBounds;
            for (qsizetype i = 0; i < actionIds.size(); ++i) {
                const QRect bounds = boundsFor(controls, actionIds[i]);
                const bool lowerAction = i >= 3;
                const int expectedY = wrapped && lowerAction ? lowerY : upperY;
                expect(!bounds.isEmpty() && widget.rect().contains(bounds) && bounds.y() == expectedY &&
                        !bounds.intersects(chord) && !bounds.intersects(runway) &&
                        !bounds.intersects(arrangement),
                    "painted action tiles stay clear of the chord visualiser and section controls");
                for (const QRect& other : actionBounds) expect(!bounds.intersects(other),
                    "painted action tiles have non-overlapping click targets");
                actionBounds.push_back(bounds);
                const int before = clicks[i];
                sendMouse(widget, QEvent::MouseButtonPress, bounds.center(), Qt::LeftButton, Qt::LeftButton);
                expect(clicks[i] == before + 1, "resized painted tile dispatches through its actual click target");
            }
            const QRect status = boundsFor(controls, actionIds[3], QStringLiteral("generation_status_bounds"));
            if (generating) {
                const int anchor = actionBounds[5].bottom();
                expect(status.top() == anchor + 8 && status.right() == widget.width() - 25 &&
                        widget.rect().contains(status) && !status.intersects(chord) && !status.intersects(runway),
                    "WAV generation indicator stays below both wrapped rows or in its existing unwrapped position");
                for (const QRect& bounds : actionBounds) expect(!status.intersects(bounds),
                    "generation indicator never covers an action tile");
            } else {
                expect(status.isEmpty(), "inactive WAV generation has no status tile");
            }
            for (qsizetype i = 1; i < actionBounds.size(); ++i) {
                expect(actionBounds[i].top() > actionBounds[i - 1].top() ||
                        (actionBounds[i].top() == actionBounds[i - 1].top() && actionBounds[i].left() > actionBounds[i - 1].left()),
                    "main Jam actions retain the requested reading order at every tested size");
            }
            expect(std::none_of(controls.cbegin(), controls.cend(), [](const auto& control) {
                return control.id == QStringLiteral("performance.home.idea.details");
            }), "main Jam view has Clear Idea and no Idea Details action");
            QToolTip::hideText();
            for (const auto& bounds : actionBounds) {
                QHelpEvent help(QEvent::ToolTip, bounds.center(), widget.mapToGlobal(bounds.center()));
                QApplication::sendEvent(&widget, &help);
                expect(QToolTip::text().isEmpty(), "main Jam idea actions show no tooltips");
            }
            const QString previewRoot = qEnvironmentVariable("JAM2_TEST_LAYOUT_PREVIEW_ROOT");
            if (!previewRoot.isEmpty()) {
                expect(QDir().mkpath(previewRoot) && widget.grab().save(QDir(previewRoot).filePath(
                    QStringLiteral("performance-%1x%2-generating-%3.png")
                        .arg(size.width()).arg(size.height()).arg(generating))),
                    "requested responsive-layout preview is written");
            }
            ++cases;
        }
    }
    expect(wrappedCases > 0 && unwrappedCases > 0,
        "vertical resize coverage exercises wrapping and restoring the main-view row");
    for (int count : clicks) expect(count == cases, "every main-view action survives repeated shrink and grow");
    widget.hide();
    expect(QThreadPool::globalInstance()->waitForDone(10000),
        "responsive main-view fixture drains its background render work");
}

void testPerformanceWidget()
{
    BeatGridModel model;
    model.resizeAllSections(8);
    model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
    model.setCell(0, QStringLiteral("chord"), 2, QStringLiteral("Am7"));
    model.setCell(0, QStringLiteral("lyric"), 0, QStringLiteral("one"));
    model.setCell(0, QStringLiteral("lyric"), 4, QStringLiteral("two"));
    model.setBeatHit(0, 0, 0, QStringLiteral("x..."));
    model.setBeatHit(0, 0, 1, QStringLiteral(".a.."));

    PerformanceHomeWidget widget;
    widget.resize(1200, 760);
    widget.setSongModel(&model);
    widget.setChordPreviewVisible(false);
    widget.setChordPreviewVisible(true);
    widget.setBeatPreviewVisible(false);
    widget.setBeatPreviewVisible(true);
    widget.setTiming(9, -1, 0, -1.0, true);
    widget.setTiming(10, 2, 4, 0.75, true);
    jam2::EngineGuiPeakSnapshot peaks;
    peaks.input_peak_ppm = 400000;
    peaks.remote_peak_ppm = 600000;
    peaks.prepared_track_peak_ppm = 800000;
    widget.setAudioPeaks(peaks);
    widget.setTrackGainDb(-100.0);
    widget.setTrackGainDb(100.0);
    widget.setTrackWaveform({0.1f, 0.8f, 0.4f}, true);
    widget.setTrackBpmMismatch(true, 100.0, 120.0);
    widget.setTrackTransferStatus(QStringLiteral("receiving 50%"));
    widget.setWavGenerationActive(true);
    widget.setJamTasterTaskStatus(true, 150);
    widget.setJamRecordingState(false, false, QString());
    widget.setBankState(99, 2, 2, 0, true, QStringLiteral("queued"));
    widget.setArrangementState(true, true);

    QVector<PerformancePeerPresentation> peers;
    for (std::uint64_t peer = 1; peer <= 14; ++peer) {
        peers.push_back(PerformancePeerPresentation{
            peer,
            QStringLiteral("Peer %1").arg(peer),
            peer % 2 == 0,
            -static_cast<double>(peer),
            peer == 2,
        });
    }
    widget.setPeers(peers);
    widget.setSelectedPeer(2);
    widget.setSelectedPeer(2);

    int actionCount = 0;
    int recordingToggles = 0;
    int launchedBank = -1;
    std::uint64_t selectedPeer = 999;
    double trackGain = 99.0;
    QStringList opened;
    bool tunerState = false;
    widget.onGenerateIdea = [&] { ++actionCount; };
    widget.onBrowseIdeas = [&] { ++actionCount; };
    widget.onContinueIdea = [&] { ++actionCount; };
    widget.onClearIdea = [&] { ++actionCount; };
    widget.onGenerateWav = [&] { ++actionCount; };
    widget.onJamTaster = [&] { ++actionCount; };
    widget.onManageArrangement = [&] { ++actionCount; };
    widget.onAddSection = [&] { ++actionCount; };
    widget.onRemoveSection = [&] { ++actionCount; };
    widget.onJamRecordingToggle = [&] { ++recordingToggles; };
    widget.onBankLaunch = [&](int bank) { launchedBank = bank; };
    widget.onPeerSelected = [&](std::uint64_t peer) { selectedPeer = peer; };
    widget.onTrackGainChanged = [&](double gain) { trackGain = gain; };
    widget.onOpenDetail = [&](const QString& detail) { opened.push_back(detail); };
    widget.onTunerEnabledChanged = [&](bool enabled) { tunerState = enabled; };

    processPaint(widget);
    const auto inventory = widget.guiVirtualControls();
    expect(inventory.size() >= 37,
        "performance inventory exposes fixed actions, four banks, local peer, and remote peers");
    QString error;
    const QStringList actions{
        QStringLiteral("performance.home.idea.generate"),
        QStringLiteral("performance.home.idea.browse"),
        QStringLiteral("performance.home.idea.continue"),
        QStringLiteral("performance.home.idea.clear"),
        QStringLiteral("performance.home.idea.generate-wav"),
        QStringLiteral("performance.home.idea.jam-taster"),
        QStringLiteral("performance.home.arrangement"),
        QStringLiteral("performance.home.section.add"),
        QStringLiteral("performance.home.section.remove"),
    };
    for (const QString& id : actions) {
        error.clear();
        expect(widget.invokeGuiVirtualControl(id, QStringLiteral("click"), {}, error),
            "performance fixed virtual action dispatches");
    }
    expect(actionCount == actions.size(), "performance fixed callbacks are exact");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.recording"), QStringLiteral("click"), {}, error),
        "performance recording rejects while disabled");
    widget.setJamRecordingState(true, false, QStringLiteral("take-01"));
    expect(widget.jamRecordingEnabled() && !widget.jamRecordingActive() &&
            widget.jamRecordingTake() == QStringLiteral("take-01") &&
            widget.invokeGuiVirtualControl(
                QStringLiteral("performance.home.recording"), QStringLiteral("click"), {}, error) &&
            recordingToggles == 1,
        "performance recording state and action are consistent");
    const QStringList navigation{
        QStringLiteral("performance.home.open.chords.current"),
        QStringLiteral("performance.home.open.chords.runway"),
        QStringLiteral("performance.home.open.lyrics"),
        QStringLiteral("performance.home.open.beats.current"),
        QStringLiteral("performance.home.open.beats.next"),
        QStringLiteral("performance.home.open.looper"),
    };
    for (const QString& id : navigation) {
        expect(widget.invokeGuiVirtualControl(id, QStringLiteral("click"), {}, error),
            "performance navigation virtual action dispatches");
    }
    expect(opened.count(QStringLiteral("chords")) == 2 &&
            opened.count(QStringLiteral("beats")) == 2 &&
            opened.contains(QStringLiteral("lyrics")) && opened.contains(QStringLiteral("looper")),
        "performance navigation maps hit targets to exact workspaces");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.section.queue.3"), QStringLiteral("click"), {}, error) &&
            launchedBank == 3,
        "performance bank launch dispatches valid bank");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.section.queue.99"), QStringLiteral("click"), {}, error),
        "performance bank launch rejects stale bank");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.peer.0"), QStringLiteral("click"), {}, error) &&
            selectedPeer == 0 &&
            widget.invokeGuiVirtualControl(
                QStringLiteral("performance.home.peer.2"), QStringLiteral("click"), {}, error) &&
            selectedPeer == 2,
        "performance peer selection includes local and known remote peers");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.peer.999"), QStringLiteral("click"), {}, error),
        "performance peer selection rejects unknown peer");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.track-gain"), QStringLiteral("set-value"), -9.5, error) &&
            approximatelyEqual(trackGain, -9.5),
        "performance virtual track gain dispatches validated dB");
    expect(!widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.track-gain"), QStringLiteral("set-value"), 13.0, error) &&
            !widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.idea.generate"), QStringLiteral("set-value"), {}, error) &&
            !widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.invalid"), QStringLiteral("click"), {}, error),
        "performance virtual controls reject invalid value, operation, and id");

    jam2::EnginePitchSnapshot tuner;
    tuner.enabled = true;
    tuner.callback_tap_enabled = true;
    tuner.valid = true;
    tuner.frequency_hz = 440.0;
    tuner.cents = -3.0;
    tuner.confidence = 0.9;
    tuner.midi_note = 69;
    tuner.input_hops = 11;
    tuner.analyzed_windows = 4;
    tuner.rejected_windows = 1;
    tuner.processing_time_sum_us = 100;
    tuner.processing_time_max_us = 40;
    tuner.ring_depth_frames = 32;
    tuner.ring_capacity_frames = 128;
    tuner.ring.overruns = 2;
    widget.setTunerSnapshot(tuner);
    processPaint(widget);
    expect(widget.rendererStatsText().contains(QStringLiteral("tuner tap on")) &&
            widget.rendererStatsText().contains(QStringLiteral("avg 25.0 us")),
        "performance renderer statistics expose raw tuner counters and timing");
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.tuner.open"), QStringLiteral("click"), {}, error),
        "performance tuner opens through its virtual hit target");
    processPaint(widget);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&widget, &escape);
    expect(widget.invokeGuiVirtualControl(
               QStringLiteral("performance.home.tuner.disable"), QStringLiteral("click"), {}, error) &&
            !tunerState,
        "performance tuner closes by keyboard and dispatches disable");

    // Exercise real painted track-slider and peer-rail mouse/wheel paths.
    trackGain = 99.0;
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(970, 719), Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, QPointF(1080, 719), Qt::NoButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, QPointF(1110, 719), Qt::LeftButton, Qt::NoButton);
    expect(trackGain >= -60.0 && trackGain <= 12.0,
        "performance painted track slider drag dispatches bounded dB");
    const double beforeWheel = trackGain;
    sendWheel(widget, QPointF(1030, 719), 120);
    expect(trackGain >= beforeWheel, "performance track-slider wheel raises gain");
    sendWheel(widget, QPointF(50, 500), -120);
    sendWheel(widget, QPointF(50, 500), 120);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(4, 4), Qt::RightButton, Qt::RightButton);
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(4, 4), Qt::LeftButton, Qt::LeftButton);

    tuner.enabled = false;
    tuner.valid = false;
    widget.setTunerSnapshot(tuner);
    processPaint(widget);
    tunerState = false;
    sendMouse(widget, QEvent::MouseButtonPress, QPointF(1100, 596), Qt::LeftButton, Qt::LeftButton);
    expect(tunerState, "performance painted tuner-enable target dispatches");
    widget.resize(850, 650);
    processPaint(widget);
    sendWheel(widget, QPointF(400, 140), -120);
    QKeyEvent ordinary(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(&widget, &ordinary);
    widget.setTunerSnapshot({});
    expect(widget.rendererStatsText().endsWith(QStringLiteral("tuner off")),
        "performance renderer statistics mark tuner-off state");
    widget.hide();
    expect(QThreadPool::globalInstance()->waitForDone(10000),
        "performance background layer task completes before fixture teardown");
}

void testPerformanceBackgroundPresentation()
{
    BeatGridModel model;
    model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
    PerformanceHomeWidget widget;
    widget.setAttribute(Qt::WA_DontShowOnScreen, true);
    widget.setSongModel(&model);
    widget.resize(1200, 760);
    processPaint(widget);
    expect(QThreadPool::globalInstance()->waitForDone(10000),
        "background layers complete through the real asynchronous worker");
    QApplication::processEvents();
    const auto firstImage = widget.grab();
    const auto initial = widget.backgroundPresentationStats();
    const auto foreground = widget.foregroundStats();
    expect(foreground.animationIntervalMs == 50 && !foreground.backgroundMotionEnabled,
        "resource-saving renderer uses a measured 20 Hz cadence and frozen decorative background");
    const double ratio = widget.devicePixelRatioF();
    expect(!firstImage.isNull() && initial.pixelSize == QSize(
               static_cast<int>(std::ceil(widget.width() * ratio)),
               static_cast<int>(std::ceil(widget.height() * ratio))) &&
            initial.devicePixelRatio == ratio && initial.bytes > 0 &&
            initial.bytes <= initial.maximumBytes && initial.refreshes > 0,
        "real view creates a bounded native-resolution background after asynchronous layer delivery");
    const auto controls = widget.guiVirtualControls();
    const auto target = std::find_if(controls.begin(), controls.end(), [](const auto& control) {
        return control.id == QStringLiteral("performance.home.open.chords.current");
    });
    expect(target != controls.end(), "cached view exposes the real chord control");
    QString opened;
    widget.onOpenDetail = [&](const QString& page) { opened = page; };
    if (target != controls.end()) {
        const auto bounds = target->state.value(QStringLiteral("bounds")).toMap();
        const QRect rect(bounds.value(QStringLiteral("x")).toInt(), bounds.value(QStringLiteral("y")).toInt(),
            bounds.value(QStringLiteral("width")).toInt(), bounds.value(QStringLiteral("height")).toInt());
        sendMouse(widget, QEvent::MouseButtonPress, rect.center(), Qt::LeftButton, Qt::LeftButton);
    }
    expect(opened == QStringLiteral("chords"), "cached view dispatches the actual chord hit target");
    widget.setWavGenerationActive(true);
    widget.setTiming(1, 0, 4, 0.75, true);
    const auto changedImage = widget.grab();
    widget.grab();
    const auto reused = widget.backgroundPresentationStats();
    expect(changedImage.toImage() != firstImage.toImage() && reused.reusedPaints > initial.reusedPaints &&
            reused.fallbackPaints == 0,
        "foreground state visibly changes while ordinary paints reuse the native background");
    for (const QSize size : {QSize(850, 650), QSize(1680, 950), QSize(1200, 760)}) {
        widget.resize(size);
        const auto image = widget.grab();
        const auto stats = widget.backgroundPresentationStats();
        expect(!image.isNull() && stats.pixelSize == QSize(
                   static_cast<int>(std::ceil(widget.width() * ratio)),
                   static_cast<int>(std::ceil(widget.height() * ratio))) &&
                stats.bytes <= stats.maximumBytes && stats.fallbackPaints == 0,
            "compact and wide layouts replace the cache at the actual pixel dimensions");
    }
    if (ratio >= 2.0) {
        // Just beyond 64 MiB at 2x, without allocating an additional cached image.
        widget.resize(8500, 500);
        expect(!widget.grab().isNull() && widget.backgroundPresentationStats().bytes == 0 &&
                widget.backgroundPresentationStats().fallbackPaints > 0,
            "oversized presentation falls back to the original rendering path within the cache budget");
        expect(widget.foregroundStats().fallbackPaints > 0 &&
                widget.foregroundStats().bytes <= widget.foregroundStats().maximumBytes,
            "oversized beat layers fall back without exceeding the foreground memory bound");
        widget.resize(1200, 760);
        widget.grab();
        expect(widget.backgroundPresentationStats().bytes > 0,
            "normal rendering recovers after an oversized view");
    }
    expect(widget.rendererStatsText().contains(QStringLiteral("native background")) &&
            widget.rendererStatsText().contains(QStringLiteral("MiB/64 MiB")),
        "renderer diagnostics expose cache size, bound and reuse counters");
    widget.hide();
    QApplication::processEvents();
    const auto hidden = widget.backgroundPresentationStats();
    QEventLoop hiddenWait;
    QTimer::singleShot(160, &hiddenWait, &QEventLoop::quit);
    hiddenWait.exec();
    expect(widget.backgroundPresentationStats().refreshes == hidden.refreshes,
        "hidden view performs no new presentation compositions");

    widget.show();
    widget.setTiming(0, 0, 4, 0.0, false);
    widget.grab();
    const auto stopped = widget.foregroundStats();
    widget.grab();
    expect(widget.foregroundStats().refreshes == stopped.refreshes &&
            widget.foregroundStats().reusedPaints >= stopped.reusedPaints + 4 &&
            stopped.bytes > 0 && stopped.bytes <= stopped.maximumBytes,
        "stopped chord and beat layers reuse all four bounded native-resolution images");
    widget.setTiming(0, 0, 4, 0.25, true);
    const auto phaseBefore = widget.grab().toImage();
    const auto playing = widget.foregroundStats();
    widget.setTiming(0, 1, 4, 0.75, true);
    const auto phaseAfter = widget.grab().toImage();
    expect(widget.foregroundStats().refreshes == playing.refreshes && phaseBefore != phaseAfter,
        "phase-only updates keep static layers while the progress arc remains live");
    widget.setTiming(1, 0, 4, 0.0, true);
    widget.grab();
    const auto beat = widget.foregroundStats();
    expect(beat.refreshes[0] > playing.refreshes[0] && beat.refreshes[1] > playing.refreshes[1] &&
            beat.refreshes[2] > playing.refreshes[2] && beat.refreshes[3] == playing.refreshes[3],
        "active beat changes refresh chord labels and current grid without rebuilding the next bar");
    widget.setTiming(4, 0, 4, 0.0, true);
    widget.grab();
    const auto bar = widget.foregroundStats();
    expect(bar.refreshes[3] > beat.refreshes[3], "bar changes refresh the next grid");
    model.setBeatHit(0, 4, 0, QStringLiteral("xag."));
    model.setCell(0, QStringLiteral("chord"), 4, QStringLiteral("F#m7"));
    widget.grab();
    const auto edit = widget.foregroundStats();
    expect(edit.refreshes[0] > bar.refreshes[0] && edit.refreshes[2] > bar.refreshes[2] &&
            edit.refreshes[3] > bar.refreshes[3], "model revisions invalidate cached musical content");
    QFont changedFont = widget.font();
    changedFont.setPointSizeF(15.0);
    widget.setFont(changedFont);
    widget.grab();
    expect(widget.foregroundStats().refreshes[2] > edit.refreshes[2],
        "font changes regenerate cached text");
    widget.setBankState(1, 2, 1, 3, false, QString{});
    widget.setTiming(7, 0, 4, 0.0, true);
    widget.grab();
    const auto bank = widget.foregroundStats();
    expect(bank.refreshes[3] > edit.refreshes[3], "pending bank and meter changes update the preview");
    for (const auto& control : widget.guiVirtualControls()) {
        if (control.id != QStringLiteral("performance.home.open.beats.current")) continue;
        const auto bounds = control.state.value(QStringLiteral("bounds")).toMap();
        const QRect rect(bounds.value(QStringLiteral("x")).toInt(), bounds.value(QStringLiteral("y")).toInt(),
            bounds.value(QStringLiteral("width")).toInt(), bounds.value(QStringLiteral("height")).toInt());
        sendMouse(widget, QEvent::MouseButtonPress, rect.center(), Qt::LeftButton, Qt::LeftButton);
    }
    expect(opened == QStringLiteral("beats"), "cached beat preview retains the actual mouse action");
    widget.setChordPreviewVisible(false);
    widget.setBeatPreviewVisible(false);
    widget.grab();
    const auto disabled = widget.foregroundStats();
    widget.setTiming(20, 0, 4, 0.5, true);
    widget.grab();
    expect(disabled.bytes == 0 && widget.foregroundStats().refreshes == disabled.refreshes,
        "disabled previews release their layers and perform no chord or beat drawing");
    widget.setChordPreviewVisible(true);
    widget.setBeatPreviewVisible(true);
    widget.setSongModel(nullptr);
    widget.grab();
    widget.setSongModel(&model);
    widget.resize(850, 650);
    widget.grab();
    const auto compact = widget.foregroundStats();
    widget.grab();
    expect(compact.bytes > 0 && compact.bytes <= compact.maximumBytes &&
            widget.foregroundStats().refreshes == compact.refreshes,
        "compact layout and model replacement recover and reuse current musical content");
    widget.hide();
}

void benchmarkPerformanceRendering(bool staticViews = false)
{
    BeatGridModel model;
    model.setCell(0, QStringLiteral("chord"), 0, QStringLiteral("Cmaj7"));
    PerformanceHomeWidget widget;
    widget.setAttribute(Qt::WA_DontShowOnScreen, true);
    widget.resize(1920, 1080);
    widget.setSongModel(&model);
    widget.setTiming(0, 0, 4, 0.25, true);
    widget.setPeers({{1, QStringLiteral("Alex"), true, -3.0, false},
        {2, QStringLiteral("Blair"), true, -6.0, false},
        {3, QStringLiteral("Casey"), true, -9.0, false}});
    jam2::EngineGuiPeakSnapshot peaks;
    peaks.input_peak_ppm = 300000;
    peaks.remote_peak_ppm = 600000;
    widget.setAudioPeaks(peaks);
    class PaintCounter final : public QObject {
    public:
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::Paint) ++count;
            return false;
        }
    } paints;
    widget.installEventFilter(&paints);
    widget.show();
    // Measure the steady renderer after its real asynchronous layer build and
    // five-second reveal, independently of platform/CPU startup speed.
    QElapsedTimer startup; startup.start();
    while ((!widget.backgroundPresentationStats().layersReady ||
            widget.backgroundPresentationStats().revealActive) && startup.elapsed() < 15000) {
        QEventLoop warmup;
        QTimer::singleShot(50, &warmup, &QEventLoop::quit);
        warmup.exec();
    }
    const auto ready = widget.backgroundPresentationStats();
    expect(ready.layersReady && !ready.revealActive, "renderer startup completes before CPU measurements");
    if (!ready.layersReady || ready.revealActive) return;
    if (staticViews) {
        QJsonArray scenarios;
        QElapsedTimer transportClock;
        QTimer transport;
        transport.setInterval(16);
        QObject::connect(&transport, &QTimer::timeout, &widget, [&] {
            const double beats = transportClock.elapsed() / 500.0;
            widget.setTiming(static_cast<quint64>(beats), 0, 4, beats - std::floor(beats), true);
        });
        for (const QString& scenario : {QStringLiteral("stopped"), QStringLiteral("playing"),
                 QStringLiteral("previews_disabled"), QStringLiteral("hidden")}) {
            const bool playing = scenario != QStringLiteral("stopped");
            const bool previews = scenario != QStringLiteral("previews_disabled");
            widget.setChordPreviewVisible(previews);
            widget.setBeatPreviewVisible(previews);
            if (scenario == QStringLiteral("hidden")) widget.hide();
            widget.setTiming(0, 0, 4, 0.0, playing);
            transportClock.restart();
            if (playing) transport.start();
            const int beforePaints = paints.count;
            const double beforeCpu = processCpuSeconds();
            QElapsedTimer elapsed;
            elapsed.start();
            QEventLoop sample;
            QTimer::singleShot(6000, &sample, &QEventLoop::quit);
            sample.exec();
            transport.stop();
            const double seconds = elapsed.nsecsElapsed() / 1e9;
            const double cpu = processCpuSeconds() - beforeCpu;
            const QJsonObject metric{{QStringLiteral("scenario"), scenario},
                {QStringLiteral("cpu_one_core_percent"), cpu / seconds * 100.0},
                {QStringLiteral("wall_seconds"), seconds},
                {QStringLiteral("paint_events"), paints.count - beforePaints},
                {QStringLiteral("renderer"), widget.rendererStatsText()}};
            scenarios.append(metric);
            std::cout << "METRIC static_views " << QJsonDocument(metric).toJson(QJsonDocument::Compact).constData() << '\n';
            expect(scenario == QStringLiteral("hidden") ? paints.count == beforePaints
                : paints.count - beforePaints > 10, "visible animation runs and hidden views do not paint");
        }
        const QString artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
        if (!artifacts.isEmpty()) {
            QFile file(QDir(artifacts).filePath(QStringLiteral("static-views.json")));
            const auto bytes = QJsonDocument(scenarios).toJson();
            expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                "static view benchmark retains all scenarios");
        }
        widget.hide();
        expect(QThreadPool::globalInstance()->waitForDone(10000), "static view benchmark drains workers");
        return;
    }
    const auto settledBackground = widget.backgroundPresentationStats().refreshes;
    const int initialPaints = paints.count;
    const double initialCpu = processCpuSeconds();
    QElapsedTimer wall;
    wall.start();
    QEventLoop measurement;
    QTimer::singleShot(6000, &measurement, &QEventLoop::quit);
    measurement.exec();
    const double wallSeconds = wall.nsecsElapsed() / 1000000000.0;
    const double cpuSeconds = processCpuSeconds() - initialCpu;
    const QJsonObject metrics{
        {QStringLiteral("wall_seconds"), wallSeconds},
        {QStringLiteral("process_cpu_seconds"), cpuSeconds},
        {QStringLiteral("cpu_one_core_percent"), cpuSeconds / wallSeconds * 100.0},
        {QStringLiteral("paint_events"), paints.count - initialPaints},
        {QStringLiteral("device_pixel_ratio"), widget.devicePixelRatioF()},
        {QStringLiteral("renderer"), widget.rendererStatsText()},
        {QStringLiteral("background_cache_bytes"), widget.backgroundPresentationStats().bytes},
        {QStringLiteral("background_cache_refreshes"), static_cast<qint64>(widget.backgroundPresentationStats().refreshes)},
        {QStringLiteral("background_cache_reused_paints"), static_cast<qint64>(widget.backgroundPresentationStats().reusedPaints)},
    };
    const auto bytes = QJsonDocument(metrics).toJson(QJsonDocument::Compact);
    std::cout << "METRIC performance_rendering " << bytes.constData() << '\n';
    const QString artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    if (!artifacts.isEmpty()) {
        QFile file(QDir(artifacts).filePath(QStringLiteral("performance-rendering.json")));
        expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "performance rendering benchmark retains its measurements");
        expect(widget.grab().save(QDir(artifacts).filePath(QStringLiteral("performance-rendering.png"))),
            "performance rendering benchmark retains the real rendered view");
    }
    expect(paints.count - initialPaints > 10, "performance animation continues during measurement");
    expect(widget.backgroundPresentationStats().refreshes == settledBackground &&
            widget.rendererStatsText().contains(QStringLiteral("nebula motion frozen")),
        "temporary frozen-nebula preview retains the artwork without background recomposition after startup");
    QString opened;
    widget.onOpenDetail = [&](const QString& page) { opened = page; };
    for (const auto& control : widget.guiVirtualControls()) {
        if (control.id != QStringLiteral("performance.home.open.chords.current")) continue;
        const auto bounds = control.state.value(QStringLiteral("bounds")).toMap();
        const QRect target(bounds.value(QStringLiteral("x")).toInt(), bounds.value(QStringLiteral("y")).toInt(),
            bounds.value(QStringLiteral("width")).toInt(), bounds.value(QStringLiteral("height")).toInt());
        sendMouse(widget, QEvent::MouseButtonPress, target.center(), Qt::LeftButton, Qt::LeftButton);
    }
    expect(opened == QStringLiteral("chords"), "performance view retains its real painted navigation action");
    widget.hide();
    expect(QThreadPool::globalInstance()->waitForDone(10000),
        "performance rendering benchmark drains asynchronous background work");
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--performance-background")) {
        testPerformanceBackgroundPresentation();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--static-views")) {
        benchmarkPerformanceRendering(true);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--performance-rendering")) {
        benchmarkPerformanceRendering();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--looper-waveform")) {
        testLooperWaveformGridAlignment();
        return failures == 0 ? 0 : 1;
    }
    testActionTileRow();
    testIdeaHeaderRows();
    testResponsivePerformanceActions();
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--responsive-actions")) {
        return failures == 0 ? 0 : 1;
    }
    testTimelineHelpers();
    testGuiInteractionPolicy();
    testLocalEngineDialogState();
    testSessionDefaultsAndPriorityControls();
    testAudioDeviceUiSupport();
    testConnectionGuidance();
    testMeterWidget();
    testLooperLaneWidget();
    testPendingWavWidget();
    testLooperWaveformGridAlignment();
    testMetronomeWidgets();
    testBeatGridWidgets();
    testPerformanceWidget();
    testPerformanceBackgroundPresentation();
    if (failures == 0) {
        std::cout << "GUI widget boundary tests passed\n";
        return 0;
    }
    std::cerr << failures << " GUI widget boundary assertion(s) failed\n";
    return 1;
}
