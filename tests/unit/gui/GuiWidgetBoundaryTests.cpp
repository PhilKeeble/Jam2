#include "BeatGridWidget.hpp"
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
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

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
        {[&deviceTestCalls](QComboBox* device, QPushButton*, QWidget*) {
            if (device != nullptr &&
                device->currentData().toString() == QStringLiteral("17")) {
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
    capabilities.current_sample_rate = 48000.0;
    capabilities.sample_rate_supported = {true, false};
    capabilities.buffer_size_supported = {true, true, false, false};
    const QString text = jam2::gui::audioDeviceCapabilitiesText(capabilities);
    expect(text.contains(QStringLiteral("Device: ASIO First Device")) &&
            text.contains(QStringLiteral("44100 Hz: supported")) &&
            text.contains(QStringLiteral("48000 Hz: not supported")) &&
            text.contains(QStringLiteral("32 frames: supported")) &&
            text.contains(QStringLiteral("256 frames: not supported")),
        "audio device capability text reports every exact probed result");

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
    expect(joiner.contains(QStringLiteral("No authenticated TCP")) &&
            joiner.contains(QStringLiteral("invite address")) &&
            joiner.contains(QStringLiteral("both computers")),
        "joiner diagnostics distinguish coordinator and local firewall checks");
}

void testWaveformAndMeterWidgets()
{
    WaveformWidget waveform;
    waveform.resize(640, 240);
    qint64 seek = -1;
    int seekCount = 0;
    waveform.onSeekMs = [&](qint64 value) {
        seek = value;
        ++seekCount;
    };
    waveform.clear();
    waveform.setDurationMs(-10);
    waveform.setPlayheadMs(100);
    waveform.setBpm(0.0);
    waveform.setGridPosition(-5, true, 0, 3);
    processPaint(waveform);

    waveform.setDurationMs(4000);
    waveform.setDurationMs(4000);
    waveform.setBpm(500.0);
    waveform.setBpm(120.0);
    waveform.setGridPosition(4500, true, 4, 1);
    waveform.setGridPosition(4750, true, 4, 1);
    waveform.setGridPosition(4750, true, 3, 3);
    waveform.setPlayheadMs(5000);
    waveform.setPlayheadMs(4000);
    waveform.setLoop(-1, -1);
    waveform.setLoop(1000, 3000);
    waveform.setPeaks({0.0f, 0.25f, 1.0f, 0.5f}, true);
    processPaint(waveform);

    sendMouse(
        waveform, QEvent::MouseButtonPress, QPointF(160, 120),
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(
        waveform, QEvent::MouseMove, QPointF(480, 120),
        Qt::NoButton, Qt::LeftButton);
    sendMouse(
        waveform, QEvent::MouseButtonPress, QPointF(10, 10),
        Qt::RightButton, Qt::RightButton);
    expect(seekCount == 2 && seek > 2900 && seek < 3100,
        "waveform press and drag seek through bounded millisecond coordinates");
    waveform.setPeaks({}, false);
    waveform.clear();
    processPaint(waveform);

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

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    testTimelineHelpers();
    testGuiInteractionPolicy();
    testLocalEngineDialogState();
    testAudioDeviceUiSupport();
    testConnectionGuidance();
    testWaveformAndMeterWidgets();
    testLooperLaneWidget();
    testMetronomeWidgets();
    testBeatGridWidgets();
    testPerformanceWidget();
    if (failures == 0) {
        std::cout << "GUI widget boundary tests passed\n";
        return 0;
    }
    std::cerr << failures << " GUI widget boundary assertion(s) failed\n";
    return 1;
}
