#include "MainWindowPages.hpp"

#include "MainWindow.hpp"

#include "GuiPresentation.hpp"
#include "SessionController.hpp"
#include "TrackWidgets.hpp"

#include "tuning_profile.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <limits>

void MainWindowPages::build(MainWindow& w)
{
    w.setWindowTitle(QStringLiteral("Jam2"));
    w.setMinimumSize(1280, 720);

    w.titleLabel_ = new QLabel(QStringLiteral("Jam2"), &w);
    w.titleLabel_->setObjectName(QStringLiteral("AppTitle"));
    w.connectionLabel_ = new QLabel(QStringLiteral("Idle"), &w);
    w.connectionLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.jitterLabel_ = new QLabel(QStringLiteral("Jitter -"), &w);
    w.jitterLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.lossLabel_ = new QLabel(QStringLiteral("Loss -"), &w);
    w.lossLabel_->setObjectName(QStringLiteral("StatusPill"));

    auto* header = new QHBoxLayout();
    header->addWidget(w.titleLabel_);
    header->addStretch(1);
    w.engineModeLabel_ = new QLabel(QStringLiteral("Local"), &w);
    w.engineModeLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.engineModeLabel_->setAlignment(Qt::AlignCenter);
    w.engineModeLabel_->setMinimumWidth(72);
    w.sessionTopologyLabel_ = new QLabel(QStringLiteral("Remote Peers 0"), &w);
    w.sessionTopologyLabel_->setObjectName(QStringLiteral("StatusPill"));
    header->addWidget(w.engineModeLabel_);
    header->addWidget(w.connectionLabel_);
    header->addWidget(w.sessionTopologyLabel_);

    w.songTitleEdit_ = new QLineEdit(w.chordModel_.title(), &w);
    w.songTitleEdit_->setMinimumWidth(300);
    auto* newSongButton = new QPushButton(QStringLiteral("New Song"), &w);
    auto* openSongButton = new QPushButton(QStringLiteral("Open"), &w);
    auto* saveSongButton = new QPushButton(QStringLiteral("Save"), &w);
    auto* library = new QHBoxLayout();
    library->addWidget(new QLabel(QStringLiteral("Song"), &w));
    library->addWidget(w.songTitleEdit_, 1);
    library->addWidget(newSongButton);
    library->addWidget(openSongButton);
    library->addWidget(saveSongButton);

    QObject::connect(w.songTitleEdit_, &QLineEdit::editingFinished, &w, [&w] {
        w.chordModel_.setTitle(w.songTitleEdit_->text());
        w.beatModel_.setTitle(w.songTitleEdit_->text());
        w.lyricModel_.setTitle(w.songTitleEdit_->text());
        w.sendSongSnapshot();
    });
    QObject::connect(newSongButton, &QPushButton::clicked, &w, [&w] { w.newSong(); });
    QObject::connect(openSongButton, &QPushButton::clicked, &w, [&w] { w.openSong(); });
    QObject::connect(saveSongButton, &QPushButton::clicked, &w, [&w] { w.saveSong(); });

    QWidget* sessionPage = buildSessionPage(w);

    w.tabs_ = new QTabWidget(&w);
    w.tabs_->addTab(buildSongPage(w), QStringLiteral("Chord View"));
    w.beatGrid_ = new BeatGridWidget(&w.beatModel_, QStringLiteral("beat"), &w);
    w.tabs_->addTab(w.beatGrid_, QStringLiteral("Beat View"));
    w.lyricGrid_ = new BeatGridWidget(&w.lyricModel_, QStringLiteral("lyric"), &w);
    w.tabs_->addTab(w.lyricGrid_, QStringLiteral("Lyrics"));
    w.tabs_->addTab(buildTrackPage(w), QStringLiteral("Track"));
    w.tabs_->addTab(buildMetronomePage(w), QStringLiteral("Metronome"));
    w.tabs_->addTab(buildMixPage(w), QStringLiteral("Mix"));

    auto sendCellEdit = [&w](int section, const QString& lane, int beat, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    w.beatGrid_->onCellEdited = sendCellEdit;
    w.lyricGrid_->onCellEdited = sendCellEdit;
    w.beatGrid_->onBeatHitEdited = [&w](int section, int beat, int lane, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.hit")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("text"), text},
        });
    };
    w.beatGrid_->onBeatDivisionChanged = [&w](int section, int beat, int division, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.division")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("division"), division},
        });
    };
    w.lyricGrid_->onLyricsEdited = [&w](const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("lyrics.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("text"), text},
        });
    };
    w.beatGrid_->onStructureChanged = [&w] {
        w.sendSongSnapshot();
    };
    w.lyricGrid_->onStructureChanged = [&w] {
        w.sendSongSnapshot();
    };
    w.beatGrid_->onGridResized = [&w](int section, int beats, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("beat")},
            {QStringLiteral("beats"), beats},
        });
    };
    w.lyricGrid_->onGridResized = w.beatGrid_->onGridResized;

    w.latencyLabel_ = new QLabel(QStringLiteral("Latency -"), &w);
    w.latencyLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.depthLabel_ = new QLabel(QStringLiteral("Depth -"), &w);
    w.depthLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.ringDepthLabel_ = new QLabel(QStringLiteral("Ring -"), &w);
    w.ringDepthLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.underrunLabel_ = new QLabel(QStringLiteral("Underrun -"), &w);
    w.underrunLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.missingFramesLabel_ = new QLabel(QStringLiteral("Missing -"), &w);
    w.missingFramesLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.driftLabel_ = new QLabel(QStringLiteral("Drift -"), &w);
    w.driftLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.diagnosisLabel_ = new QLabel(QStringLiteral("Diagnosis -"), &w);
    w.diagnosisLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.diagnosisLabel_->setMinimumWidth(260);
    w.diagnosisLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* footer = new QHBoxLayout();
    footer->addWidget(w.latencyLabel_);
    footer->addWidget(w.jitterLabel_);
    footer->addWidget(w.lossLabel_);
    footer->addWidget(w.depthLabel_);
    footer->addWidget(w.ringDepthLabel_);
    footer->addWidget(w.underrunLabel_);
    footer->addWidget(w.missingFramesLabel_);
    footer->addWidget(w.driftLabel_);
    footer->addStretch(1);
    footer->addWidget(w.diagnosisLabel_);

    auto* layout = new QVBoxLayout(&w);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addLayout(header);
    layout->addLayout(library);
    layout->addWidget(sessionPage);
    layout->addWidget(w.tabs_, 1);

    w.logEdit_ = new QPlainTextEdit(&w);
    w.logEdit_->setReadOnly(true);
    w.logEdit_->setMaximumBlockCount(2000);
    w.logEdit_->setMaximumHeight(150);
    layout->addWidget(w.logEdit_);
    layout->addLayout(footer);

    QTimer::singleShot(0, &w, [&w] {
        w.refreshDevices();
        w.refreshLoopbackSources();
        if (!w.jam2_.isRunning()) {
            w.showLocalPerformSetup();
        }
    });
}

QWidget* MainWindowPages::buildSessionPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    w.bindHostEdit_ = new QLineEdit(SessionController::defaultBindHost(), page);
    w.portSpin_ = new QSpinBox(page);
    w.portSpin_->setRange(1, 65535);
    w.portSpin_->setValue(49000);
    w.publicHostEdit_ = new QLineEdit(SessionController::defaultPublicHost(), page);
    w.connectUrlEdit_ = new QLineEdit(page);
    w.stunServerEdit_ = new QLineEdit(QStringLiteral("stun.l.google.com:19302"), page);
    w.stunTimeoutSpin_ = new QSpinBox(page);
    w.stunTimeoutSpin_->setRange(1, 60000);
    w.stunTimeoutSpin_->setValue(1000);
    w.stunRetriesSpin_ = new QSpinBox(page);
    w.stunRetriesSpin_->setRange(1, 100);
    w.stunRetriesSpin_->setValue(3);
    w.waitMsSpin_ = new QSpinBox(page);
    w.waitMsSpin_->setRange(0, 24 * 60 * 60 * 1000);
    w.waitMsSpin_->setValue(0);
    w.streamMsSpin_ = new QSpinBox(page);
    w.streamMsSpin_->setRange(0, 24 * 60 * 60 * 1000);

    w.streamMsSpin_->setValue(0);
    w.streamLingerMsSpin_ = new QSpinBox(page);
    w.streamLingerMsSpin_->setRange(0, 60000);
    w.streamLingerMsSpin_->setValue(100);
    w.statsCheck_ = new QCheckBox(QStringLiteral("Periodic stats"), page);
    w.statsCheck_->setChecked(true);
    w.meshMaxPeersSpin_ = new QSpinBox(page);
    w.meshMaxPeersSpin_->setRange(0, 1000000);
    w.meshMaxPeersSpin_->setValue(0);
    w.statsWarmupMsSpin_ = new QSpinBox(page);
    w.statsWarmupMsSpin_->setRange(0, 600000);
    w.statsWarmupMsSpin_->setValue(3000);
    w.logStatsEdit_ = new QLineEdit(page);
    w.logStatsEdit_->setText(appReleaseFolderPath(QStringLiteral("logs")));
    w.socketSendBufferSpin_ = new QSpinBox(page);
    w.socketSendBufferSpin_->setRange(0, std::numeric_limits<int>::max());
    w.socketSendBufferSpin_->setValue(0);
    w.socketRecvBufferSpin_ = new QSpinBox(page);
    w.socketRecvBufferSpin_->setRange(0, std::numeric_limits<int>::max());
    w.socketRecvBufferSpin_->setValue(0);

    w.profileBox_ = new QComboBox(page);
    for (const jam2::TuningProfile& profile : jam2::tuning_profiles()) {
        w.profileBox_->addItem(QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
                             QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
    }
    w.osPriorityBox_ = new QComboBox(page);
    w.osPriorityBox_->addItem(QStringLiteral("Realtime"), QStringLiteral("realtime"));
    w.osPriorityBox_->addItem(QStringLiteral("High"), QStringLiteral("high"));
    w.osPriorityBox_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
    w.deviceBox_ = new QComboBox(page);
    w.deviceBox_->setEditable(true);
    w.inputChannelsEdit_ = new QLineEdit(QStringLiteral("1"), page);
    w.outputChannelsEdit_ = new QLineEdit(QStringLiteral("1,2"), page);
    w.sampleRateSpin_ = new QSpinBox(page);
    w.sampleRateSpin_->setRange(8000, 384000);
    w.sampleRateSpin_->setValue(44100);
    w.bufferSizeSpin_ = new QSpinBox(page);
    w.bufferSizeSpin_->setRange(16, 4096);
    w.bufferSizeSpin_->setValue(128);
    w.frameSizeSpin_ = new QSpinBox(page);
    w.frameSizeSpin_->setRange(32, 256);
    w.frameSizeSpin_->setValue(128);
    w.prefillSpin_ = new QSpinBox(page);
    w.prefillSpin_->setRange(0, 65536);
    w.prefillSpin_->setValue(1536);
    w.playbackMaxSpin_ = new QSpinBox(page);
    w.playbackMaxSpin_->setRange(0, 65536);
    w.playbackMaxSpin_->setValue(0);
    w.captureRingSpin_ = new QSpinBox(page);
    w.captureRingSpin_->setRange(1, 1048576);
    w.captureRingSpin_->setValue(4096);
    w.playbackRingSpin_ = new QSpinBox(page);
    w.playbackRingSpin_->setRange(1, 1048576);
    w.playbackRingSpin_->setValue(4096);
    w.driftCorrectionCheck_ = new QCheckBox(QStringLiteral("Drift correction"), page);
    w.driftCorrectionCheck_->setChecked(true);
    w.driftSmoothingSpin_ = new QDoubleSpinBox(page);
    w.driftSmoothingSpin_->setRange(0.0, 1.0);
    w.driftSmoothingSpin_->setDecimals(3);
    w.driftSmoothingSpin_->setSingleStep(0.005);
    w.driftSmoothingSpin_->setValue(0.02);
    w.driftDeadbandSpin_ = new QSpinBox(page);
    w.driftDeadbandSpin_->setRange(0, 50000);
    w.driftDeadbandSpin_->setValue(25);
    w.driftMaxCorrectionSpin_ = new QSpinBox(page);
    w.driftMaxCorrectionSpin_->setRange(0, 50000);
    w.driftMaxCorrectionSpin_->setValue(500);
    w.noStunCheck_ = new QCheckBox(QStringLiteral("No STUN"), page);
    w.bpmSpin_ = new QSpinBox(page);
    w.bpmSpin_->setRange(1, 400);
    w.bpmSpin_->setValue(120);
    w.bpmSpin_->hide();
    w.sampleTimePlayoutCheck_ = new QCheckBox(QStringLiteral("Sample-time playout"), page);

    w.sampleTimePlayoutCheck_->setChecked(true);
    w.playoutDelaySpin_ = new QSpinBox(page);
    w.playoutDelaySpin_->setRange(0, 1048576);
    w.playoutDelaySpin_->setValue(0);
    w.jitterBufferSpin_ = new QSpinBox(page);
    w.jitterBufferSpin_->setRange(0, 1048576);
    w.jitterBufferSpin_->setValue(0);
    w.jitterBufferMaxSpin_ = new QSpinBox(page);
    w.jitterBufferMaxSpin_->setRange(0, 1048576);
    w.jitterBufferMaxSpin_->setValue(0);
    w.adaptiveCushionCheck_ = new QCheckBox(QStringLiteral("Adaptive cushion"), page);
    w.adaptiveTargetSpin_ = new QSpinBox(page);
    w.adaptiveTargetSpin_->setRange(0, 1048576);
    w.adaptiveMinSpin_ = new QSpinBox(page);
    w.adaptiveMinSpin_->setRange(0, 1048576);
    w.adaptiveMaxSpin_ = new QSpinBox(page);
    w.adaptiveMaxSpin_->setRange(0, 1048576);
    w.adaptiveReleaseSpin_ = new QSpinBox(page);
    w.adaptiveReleaseSpin_->setRange(0, 1000000);
    w.adaptiveReleaseSpin_->setValue(1000);
    w.startButton_ = new QPushButton(QStringLiteral("Start Jam"), page);
    w.joinButton_ = new QPushButton(QStringLiteral("Join Jam"), page);
    w.stopButton_ = new QPushButton(QStringLiteral("End Jam"), page);
    w.refreshControlButton_ = new QPushButton(QStringLiteral("Refresh Control"), page);
    w.stopButton_->setEnabled(false);
    w.refreshControlButton_->setEnabled(false);

    w.connectUrlEdit_->setMinimumWidth(420);
    w.deviceBox_->setEditable(false);
    w.deviceBox_->setMinimumWidth(280);
    const QList<QWidget*> sessionEditors{
        w.bindHostEdit_, w.portSpin_, w.publicHostEdit_, w.connectUrlEdit_,
        w.stunServerEdit_, w.stunTimeoutSpin_, w.stunRetriesSpin_, w.waitMsSpin_,
        w.streamMsSpin_, w.streamLingerMsSpin_, w.statsWarmupMsSpin_, w.logStatsEdit_,
        w.meshMaxPeersSpin_,
        w.socketSendBufferSpin_, w.socketRecvBufferSpin_, w.profileBox_, w.osPriorityBox_, w.deviceBox_, w.inputChannelsEdit_,
        w.outputChannelsEdit_, w.sampleRateSpin_, w.bufferSizeSpin_, w.frameSizeSpin_, w.prefillSpin_,
        w.playbackMaxSpin_, w.captureRingSpin_, w.playbackRingSpin_, w.driftSmoothingSpin_,
        w.driftDeadbandSpin_, w.driftMaxCorrectionSpin_, w.playoutDelaySpin_, w.jitterBufferSpin_,
        w.jitterBufferMaxSpin_, w.adaptiveTargetSpin_, w.adaptiveMinSpin_, w.adaptiveMaxSpin_,
        w.adaptiveReleaseSpin_,
    };
    for (QWidget* widget : sessionEditors) {
        applyMutedEditorStyle(widget);
    }
    const QList<QWidget*> sessionDialogWidgets{
        w.bindHostEdit_, w.portSpin_, w.publicHostEdit_, w.connectUrlEdit_,
        w.stunServerEdit_, w.stunTimeoutSpin_, w.stunRetriesSpin_, w.waitMsSpin_,
        w.streamMsSpin_, w.streamLingerMsSpin_, w.statsCheck_, w.statsWarmupMsSpin_, w.logStatsEdit_,
        w.meshMaxPeersSpin_,
        w.socketSendBufferSpin_, w.socketRecvBufferSpin_, w.profileBox_, w.osPriorityBox_, w.deviceBox_, w.inputChannelsEdit_,
        w.outputChannelsEdit_, w.sampleRateSpin_, w.bufferSizeSpin_, w.frameSizeSpin_, w.prefillSpin_,
        w.playbackMaxSpin_, w.captureRingSpin_, w.playbackRingSpin_, w.driftCorrectionCheck_,
        w.driftSmoothingSpin_, w.driftDeadbandSpin_, w.driftMaxCorrectionSpin_, w.noStunCheck_,
        w.sampleTimePlayoutCheck_, w.playoutDelaySpin_, w.jitterBufferSpin_, w.jitterBufferMaxSpin_,
        w.adaptiveCushionCheck_, w.adaptiveTargetSpin_, w.adaptiveMinSpin_, w.adaptiveMaxSpin_,
        w.adaptiveReleaseSpin_,
    };
    for (QWidget* widget : sessionDialogWidgets) {
        widget->hide();
    }

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(w.startButton_);
    buttons->addWidget(w.joinButton_);
    buttons->addWidget(w.stopButton_);
    buttons->addWidget(w.refreshControlButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(buttons);

    QObject::connect(w.startButton_, &QPushButton::clicked, &w, [&w] { w.showStartJamDialog(); });
    QObject::connect(w.joinButton_, &QPushButton::clicked, &w, [&w] { w.showJoinJamDialog(); });
    QObject::connect(w.stopButton_, &QPushButton::clicked, &w, [&w] { w.stopJam(); });
    QObject::connect(w.refreshControlButton_, &QPushButton::clicked, &w, [&w] { w.refreshControlConnection(); });
    QObject::connect(w.profileBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.applyTuningProfileName(w.profileBox_->currentData().toString());
    });
    w.applyTuningProfileName(QStringLiteral("fast"));
    QObject::connect(w.noStunCheck_, &QCheckBox::toggled, &w, [&w] {
        w.updateConnectionControlState();
    });
    w.updateConnectionControlState();

    return page;
}

QWidget* MainWindowPages::buildSongPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    w.chordGrid_ = new BeatGridWidget(&w.chordModel_, QStringLiteral("chord"), page);

    auto* top = new QHBoxLayout();
    top->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(w.chordGrid_, 1);

    w.chordGrid_->onCellEdited = [&w](int section, const QString& lane, int beat, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    w.chordGrid_->onGridResized = [&w](int section, int beats, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("chord")},
            {QStringLiteral("beats"), beats},
        });
    };
    w.chordGrid_->onStructureChanged = [&w] {
        w.sendSongSnapshot();
    };

    return page;
}

QWidget* MainWindowPages::buildTrackPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    w.trackNameLabel_ = new QLabel(QStringLiteral("Track: No track loaded"), page);
    w.gridPositionLabel_ = new QLabel(QStringLiteral("Grid: stopped"), page);
    w.gridScheduleLabel_ = new QLabel(QStringLiteral("Grid lock: idle"), page);
    w.recordingCountdownLabel_ = new QLabel(page);
    w.recordingCountdownLabel_->setAlignment(Qt::AlignCenter);
    w.recordingCountdownLabel_->setMinimumHeight(72);
    w.recordingCountdownLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background: #171b1e; border: 1px solid #66c6a6; color: #f2f5f4; font-size: 34px; font-weight: 700; }"));
    w.recordingCountdownLabel_->hide();

    w.trackWaveform_ = nullptr;
    w.looperStack_ = new LooperLaneStackWidget(page);
    w.trackSpeedSlider_ = new QSlider(Qt::Horizontal, page);
    w.trackSpeedSlider_->setRange(10, 200);
    w.trackSpeedSlider_->setValue(100);
    applyJamSliderStyle(w.trackSpeedSlider_);
    w.trackSpeedSlider_->setMinimumWidth(220);
    w.trackSpeedSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.trackSpeedSpin_ = new QDoubleSpinBox(page);
    w.trackSpeedSpin_->setRange(0.10, 2.00);
    w.trackSpeedSpin_->setSingleStep(0.01);
    w.trackSpeedSpin_->setDecimals(2);
    w.trackSpeedSpin_->setValue(1.0);
    w.trackSpeedSpin_->setFixedWidth(92);
    applyMutedEditorStyle(w.trackSpeedSpin_);
    w.trackPitchSlider_ = new QSlider(Qt::Horizontal, page);
    w.trackPitchSlider_->setRange(-12, 12);
    w.trackPitchSlider_->setValue(0);
    applyJamSliderStyle(w.trackPitchSlider_);
    w.trackPitchSlider_->setMinimumWidth(220);
    w.trackPitchSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.trackPitchSpin_ = new QSpinBox(page);
    w.trackPitchSpin_->setRange(-12, 12);
    w.trackPitchSpin_->setSingleStep(1);
    w.trackPitchSpin_->setSuffix(QStringLiteral(" semitones"));
    w.trackPitchSpin_->setFixedWidth(128);
    applyMutedEditorStyle(w.trackPitchSpin_);
    w.focusFrequencySlider_ = new QSlider(Qt::Horizontal, page);
    w.focusFrequencySlider_->setRange(40, 8000);
    w.focusFrequencySlider_->setValue(120);
    applyJamSliderStyle(w.focusFrequencySlider_);
    w.focusFrequencySlider_->setMinimumWidth(220);
    w.focusFrequencySlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.focusFrequencySpin_ = new QSpinBox(page);
    w.focusFrequencySpin_->setRange(40, 8000);
    w.focusFrequencySpin_->setValue(120);
    w.focusFrequencySpin_->setSuffix(QStringLiteral(" Hz"));
    w.focusFrequencySpin_->setFixedWidth(108);
    applyMutedEditorStyle(w.focusFrequencySpin_);
    w.trackSyncCheck_ = new QCheckBox(QStringLiteral("Sync track controls"), page);
    w.trackSyncCheck_->setChecked(w.looperProject_.trackSyncEnabled());
    auto* gridLockBox = new QCheckBox(QStringLiteral("Lock to grid"), page);
    gridLockBox->setChecked(w.looperProject_.gridLockEnabled());
    w.captureOutputEdit_ = new QLineEdit(page);
    w.captureOutputEdit_->setMinimumWidth(420);
    applyMutedEditorStyle(w.captureOutputEdit_);
    w.loopbackSourceBox_ = new QComboBox(page);
    w.loopbackSourceBox_->setEditable(true);
    w.loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
    w.loopbackSourceBox_->setMinimumWidth(360);
    applyMutedEditorStyle(w.loopbackSourceBox_);
    w.captureManualStopCheck_ = new QCheckBox(QStringLiteral("Record until stopped"), page);
    w.captureManualStopCheck_->setChecked(true);
    w.captureCountInCheck_ = new QCheckBox(QStringLiteral("Count-in"), page);
    w.captureCountInCheck_->setChecked(true);
    w.captureCountInMetronomeCheck_ = new QCheckBox(QStringLiteral("Metronome during count-in"), page);
    w.captureCountInMetronomeCheck_->setChecked(true);
    w.captureKeepMetronomeCheck_ = new QCheckBox(QStringLiteral("Keep metronome on while recording"), page);
    w.captureKeepMetronomeCheck_->setChecked(false);
    w.captureCountInBarsSpin_ = new QSpinBox(page);
    w.captureCountInBarsSpin_->setRange(1, 8);
    w.captureCountInBarsSpin_->setValue(1);
    w.captureCountInBarsSpin_->setSuffix(QStringLiteral(" bars"));
    w.captureCountInBarsSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.captureCountInBarsSpin_);
    w.recordingLatencyLabel_ = new QLabel(QStringLiteral("Waiting for engine latency data"), page);
    w.recordingLatencyLabel_->setWordWrap(true);
    w.recordingLatencyAdjustmentSpin_ = new QSpinBox(page);
    w.recordingLatencyAdjustmentSpin_->setRange(-8192, 8192);
    w.recordingLatencyAdjustmentSpin_->setValue(0);
    w.recordingLatencyAdjustmentSpin_->setSuffix(QStringLiteral(" frames"));
    w.recordingLatencyAdjustmentSpin_->setMinimumWidth(132);
    applyMutedEditorStyle(w.recordingLatencyAdjustmentSpin_);
    w.captureDurationSpin_ = new QSpinBox(page);
    w.captureDurationSpin_->setRange(1, 600);
    w.captureDurationSpin_->setValue(30);
    w.captureDurationSpin_->setSuffix(QStringLiteral(" s"));
    w.captureDurationSpin_->setEnabled(false);
    w.captureDurationSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.captureDurationSpin_);
    w.captureTriggerCheck_ = new QCheckBox(QStringLiteral("Trigger on signal"), page);
    w.trimLeadingCheck_ = new QCheckBox(QStringLiteral("Trim leading silence"), page);
    w.trimTrailingCheck_ = new QCheckBox(QStringLiteral("Trim trailing silence"), page);
    w.trimLeadingCheck_->setChecked(true);
    w.trimTrailingCheck_->setChecked(true);
    w.triggerThresholdSpin_ = new QDoubleSpinBox(page);

    w.triggerThresholdSpin_->setRange(-120.0, 0.0);
    w.triggerThresholdSpin_->setDecimals(1);
    w.triggerThresholdSpin_->setValue(-45.0);
    w.triggerThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    w.triggerThresholdSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.triggerThresholdSpin_);
    w.tailThresholdSpin_ = new QDoubleSpinBox(page);
    w.tailThresholdSpin_->setRange(-120.0, 0.0);
    w.tailThresholdSpin_->setDecimals(1);
    w.tailThresholdSpin_->setValue(-50.0);
    w.tailThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    w.tailThresholdSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.tailThresholdSpin_);
    w.preRollSpin_ = new QSpinBox(page);
    w.preRollSpin_->setRange(0, 10000);
    w.preRollSpin_->setValue(250);
    w.preRollSpin_->setSuffix(QStringLiteral(" ms"));
    w.preRollSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.preRollSpin_);
    w.triggerHoldSpin_ = new QSpinBox(page);
    w.triggerHoldSpin_->setRange(1, 5000);
    w.triggerHoldSpin_->setValue(50);
    w.triggerHoldSpin_->setSuffix(QStringLiteral(" ms"));
    w.triggerHoldSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.triggerHoldSpin_);
    w.tailSilenceSpin_ = new QSpinBox(page);
    w.tailSilenceSpin_->setRange(0, 30000);
    w.tailSilenceSpin_->setValue(1000);
    w.tailSilenceSpin_->setSuffix(QStringLiteral(" ms"));
    w.tailSilenceSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.tailSilenceSpin_);
    w.playTrackButton_ = new QPushButton(QStringLiteral("Play Track"), page);
    w.stopTrackButton_ = new QPushButton(QStringLiteral("Stop Track"), page);
    w.loopStartButton_ = new QPushButton(QStringLiteral("Loop Start"), page);
    w.loopEndButton_ = new QPushButton(QStringLiteral("Loop End"), page);
    w.clearLoopButton_ = new QPushButton(QStringLiteral("Clear Loop"), page);
    w.loopEnabledCheck_ = new QCheckBox(QStringLiteral("Loop whole track"), page);
    w.loopEnabledCheck_->setChecked(w.trackController_.model().loopEnabled);
    w.trackController_.model().trackGainDb = 0.0;
    w.stopCaptureButton_ = new QPushButton(QStringLiteral("Stop Recording"), page);
    w.stopCaptureButton_->setEnabled(false);
    w.loadWavButton_ = new QPushButton(QStringLiteral("Load WAV"), page);
    w.shareTracksButton_ = new QPushButton(QStringLiteral("Share Tracks"), page);
    w.startArmedLaneRecordingButton_ = new QPushButton(QStringLiteral("Start Recording"), page);

    w.captureOutputEdit_->setText(appReleaseFilePath(QStringLiteral("captures"), QStringLiteral("take.wav")));
    const QList<QWidget*> captureDialogWidgets{
        w.captureOutputEdit_, w.loopbackSourceBox_, w.captureDurationSpin_,
        w.captureManualStopCheck_, w.captureCountInCheck_, w.captureCountInMetronomeCheck_, w.captureKeepMetronomeCheck_, w.captureCountInBarsSpin_,
        w.recordingLatencyLabel_, w.recordingLatencyAdjustmentSpin_,
        w.captureTriggerCheck_, w.trimLeadingCheck_, w.trimTrailingCheck_, w.triggerThresholdSpin_,
        w.tailThresholdSpin_, w.preRollSpin_, w.triggerHoldSpin_, w.tailSilenceSpin_,
    };
    for (QWidget* widget : captureDialogWidgets) {
        widget->hide();
    }
    auto* speedControl = new QWidget(page);
    speedControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* speedLayout = new QHBoxLayout(speedControl);
    speedLayout->setContentsMargins(0, 0, 0, 0);
    speedLayout->addWidget(w.trackSpeedSlider_, 1);
    speedLayout->addWidget(w.trackSpeedSpin_);
    auto* pitchControl = new QWidget(page);
    pitchControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* pitchLayout = new QHBoxLayout(pitchControl);
    pitchLayout->setContentsMargins(0, 0, 0, 0);
    pitchLayout->addWidget(w.trackPitchSlider_, 1);
    pitchLayout->addWidget(w.trackPitchSpin_);
    auto* focusControl = new QWidget(page);
    focusControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* focusLayout = new QHBoxLayout(focusControl);
    focusLayout->setContentsMargins(0, 0, 0, 0);
    w.focusFrequencyCheck_ = new QCheckBox(page);
    w.focusPresetBox_ = new QComboBox(page);

    w.focusPresetBox_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    w.focusPresetBox_->addItem(QStringLiteral("Bass"), QStringLiteral("bass"));
    w.focusPresetBox_->addItem(QStringLiteral("Guitar"), QStringLiteral("guitar"));
    w.focusPresetBox_->addItem(QStringLiteral("Vocals"), QStringLiteral("vocals"));
    w.focusPresetBox_->addItem(QStringLiteral("Drums"), QStringLiteral("drums"));
    w.focusPresetBox_->setFixedWidth(108);
    applyMutedEditorStyle(w.focusPresetBox_);
    focusLayout->addWidget(w.focusFrequencyCheck_);
    focusLayout->addWidget(w.focusPresetBox_);
    focusLayout->addWidget(w.focusFrequencySlider_, 1);
    focusLayout->addWidget(w.focusFrequencySpin_);
    auto* loopOptionsControl = new QWidget(page);
    auto* loopOptionsLayout = new QGridLayout(loopOptionsControl);
    loopOptionsLayout->setContentsMargins(0, 0, 0, 0);
    loopOptionsLayout->setHorizontalSpacing(24);
    loopOptionsLayout->addWidget(gridLockBox, 0, 0);
    loopOptionsLayout->addWidget(w.trackSyncCheck_, 0, 1);
    loopOptionsLayout->addWidget(w.loopEnabledCheck_, 1, 0);
    loopOptionsLayout->setColumnStretch(0, 1);
    loopOptionsLayout->setColumnStretch(1, 1);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(w.trackNameLabel_);
    form->addRow(w.gridPositionLabel_);
    form->addRow(w.gridScheduleLabel_);
    form->addRow(QStringLiteral("Speed"), speedControl);
    form->addRow(QStringLiteral("Pitch"), pitchControl);
    form->addRow(QStringLiteral("Focus frequency"), focusControl);
    form->addRow(loopOptionsControl);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(w.playTrackButton_);
    buttons->addWidget(w.stopTrackButton_);
    buttons->addWidget(w.loopStartButton_);
    buttons->addWidget(w.loopEndButton_);
    buttons->addWidget(w.clearLoopButton_);
    buttons->addWidget(w.startArmedLaneRecordingButton_);
    buttons->addWidget(w.stopCaptureButton_);
    buttons->addWidget(w.loadWavButton_);
    buttons->addWidget(w.shareTracksButton_);
    for (int i = 0; i < 4; ++i) {
        auto* bankButton = new QPushButton(QString(QChar(QLatin1Char(static_cast<char>('A' + i)))), page);
        bankButton->setFixedWidth(34);
        bankButton->setCheckable(true);
        w.looperBankButtons_[i] = bankButton;
        buttons->addWidget(bankButton);
        QObject::connect(bankButton, &QPushButton::clicked, &w, [&w, i] {
            w.looperProject_.setActiveBankIndex(i);
            w.selectedLooperLane_ = -1;
            w.refreshLooperLanes();
            w.regeneratePreparedMix();
            w.syncLooperArrangement();
        });
    }
    buttons->addStretch(1);

    auto* laneScroll = new QScrollArea(page);
    laneScroll->setWidgetResizable(true);
    laneScroll->setFrameShape(QFrame::NoFrame);
    laneScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    laneScroll->setMinimumHeight(280);
    laneScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    laneScroll->setWidget(w.looperStack_);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(buttons);
    layout->addWidget(w.recordingCountdownLabel_);
    layout->addWidget(laneScroll, 1);
    layout->addLayout(form);

    QObject::connect(w.trackSpeedSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        const double speed = static_cast<double>(value) / 100.0;
        w.trackController_.model().speed = speed;
        if (w.trackSpeedSpin_) {
            const QSignalBlocker blocker(w.trackSpeedSpin_);
            w.trackSpeedSpin_->setValue(speed);
        }
    });
    QObject::connect(w.trackSpeedSlider_, &QSlider::sliderReleased, &w, [&w] {
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackSpeedSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), &w, [&w](double value) {
        w.trackController_.model().speed = value;
        if (w.trackSpeedSlider_) {
            const QSignalBlocker blocker(w.trackSpeedSlider_);
            w.trackSpeedSlider_->setValue(qBound(10, qRound(value * 100.0), 200));
        }
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackPitchSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        w.trackController_.model().pitchCents = value * 100;
        if (w.trackPitchSpin_) {
            const QSignalBlocker blocker(w.trackPitchSpin_);
            w.trackPitchSpin_->setValue(value);
        }
    });
    QObject::connect(w.trackPitchSlider_, &QSlider::sliderReleased, &w, [&w] {
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackPitchSpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w](int value) {
        w.trackController_.model().pitchCents = value * 100;
        if (w.trackPitchSlider_) {
            const QSignalBlocker blocker(w.trackPitchSlider_);
            w.trackPitchSlider_->setValue(value);
        }
        w.regeneratePreparedMix();
    });
    if (w.trackWaveform_) {
        w.trackWaveform_->onSeekMs = [&w](qint64 positionMs) {
            const qint64 frame = positionMs *
                qMax(1, w.trackRecordingWorkflow_.preparedSampleRate()) / 1000;
            w.runGridLockedEngineAction(QStringLiteral("track.seek"), [&w, frame](std::uint64_t targetFrame) {
                w.seekPreparedTrack(static_cast<std::uint64_t>(qMax<qint64>(0, frame)), targetFrame);
                w.trackRecordingWorkflow_.noteManualPreparedSeek(
                    frame,
                    static_cast<qint64>(w.metronomeTransport_.grid().position().rawCurrentFrame));
                w.updateTrackTimeline();
            });
        };
    }
    QObject::connect(w.playTrackButton_, &QPushButton::clicked, &w, [&w] {
        w.playTrack();
    });
    QObject::connect(w.stopTrackButton_, &QPushButton::clicked, &w, [&w] {
        w.runGridLockedEngineAction(QStringLiteral("track.stop"), [&w](std::uint64_t targetFrame) { w.stopTrack(targetFrame); });
    });
    QObject::connect(w.loopStartButton_, &QPushButton::clicked, &w, [&w] {
        w.setLoopStartAtCurrentPosition();
    });
    QObject::connect(w.loopEndButton_, &QPushButton::clicked, &w, [&w] {
        w.setLoopEndAtCurrentPosition();
    });
    QObject::connect(w.clearLoopButton_, &QPushButton::clicked, &w, [&w] { w.clearTrackLoop(); });
    QObject::connect(w.loopEnabledCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.trackController_.model().loopEnabled = checked;
        w.updateTrackControls();
        w.loadPreparedMixIntoEngine();
    });
    QObject::connect(w.focusFrequencyCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.trackController_.model().focusEnabled = checked;
        w.regeneratePreparedMix();
    });
    QObject::connect(w.focusPresetBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w](int) {
        QString key = w.focusPresetBox_->currentData().toString();
        if (key.isEmpty()) {
            key = QStringLiteral("custom");
        }
        auto& model = w.trackController_.model();
        model.focusPreset = key;
        if (!isCustomFocusPreset(key)) {
            const FocusPreset preset = focusPresetForKey(key);
            model.focusEnabled = true;
            model.focusFrequencyHz = preset.frequencyHz;
            model.focusGainDb = preset.gainDb;
            model.focusQ = preset.q;
        }
        w.updateTrackControls();
        w.regeneratePreparedMix();
    });
    QObject::connect(w.focusFrequencySlider_, &QSlider::valueChanged, &w, [&w](int value) {
        auto& model = w.trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (w.focusPresetBox_) {
            const QSignalBlocker blocker(w.focusPresetBox_);
            w.focusPresetBox_->setCurrentIndex(qMax(0, w.focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (w.focusFrequencySlider_) {
            w.focusFrequencySlider_->setEnabled(true);
        }
        if (w.focusFrequencySpin_) {
            w.focusFrequencySpin_->setEnabled(true);
        }
        if (w.focusFrequencySpin_) {
            const QSignalBlocker blocker(w.focusFrequencySpin_);
            w.focusFrequencySpin_->setValue(value);
        }
    });
    QObject::connect(w.focusFrequencySlider_, &QSlider::sliderReleased, &w, [&w] {
        w.regeneratePreparedMix();
    });
    QObject::connect(w.focusFrequencySpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w](int value) {
        auto& model = w.trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (w.focusPresetBox_) {
            const QSignalBlocker blocker(w.focusPresetBox_);
            w.focusPresetBox_->setCurrentIndex(qMax(0, w.focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (w.focusFrequencySlider_) {
            w.focusFrequencySlider_->setEnabled(true);
        }
        if (w.focusFrequencySpin_) {
            w.focusFrequencySpin_->setEnabled(true);
        }
        if (w.focusFrequencySlider_) {
            const QSignalBlocker blocker(w.focusFrequencySlider_);
            w.focusFrequencySlider_->setValue(value);
        }
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackSyncCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.looperProject_.setTrackSyncEnabled(checked);
        w.trackController_.model().syncControls = checked;
        w.jam2_.setTrackSyncEnabled(checked);
        if (!checked) {
            ++w.songAssetCheckRevision_;
            w.deferredSongSetMessage_ = {};
            w.songAssetCheckRetryTimer_.stop();
            w.pendingSongSet_ = {};
            w.pendingSongRevision_ = 0;
            w.pendingSongTrackRestart_ = false;
            w.pendingSongSourcePeerToken_.clear();
            w.pendingSongNeedsAuthoritativePublish_ = false;
            w.pendingLooperAssetHashes_.clear();
            w.deferredSongSetSourcePeerToken_.clear();
            w.pendingPreparedTrackReadyRevision_ = 0;
            w.assetTransfer_.resetIncoming();
            w.trackController_.requestPlayback(
                w.trackRecordingWorkflow_.preparedPlaying());
            w.trackController_.observeEnginePlaying(
                w.trackRecordingWorkflow_.preparedPlaying());
            if (w.sessionController_.isServer()) {
                w.pendingSharedTrackRevision_ = 0;
                w.pendingSharedTrackHostReady_ = true;
                w.pendingSharedTrackReadyTokens_.clear();
            }
        } else if (w.jam2_.isNetworkRunning()) {
            w.shareLocalTracks();
        }
        w.updateTrackControls();
        w.updateTrackPlaybackPresentation();
    });
    QObject::connect(gridLockBox, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.looperProject_.setGridLockEnabled(checked);
        w.refreshLooperLanes();
    });
    QObject::connect(w.recordingLatencyAdjustmentSpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w](int frames) {
        w.updateRecordingLatencyDisplay();
        if (w.jam2_.isRunning()) {
            jam2::EngineCommand command;
            command.type = jam2::EngineCommandType::SetRecordingLatencyAdjustment;
            command.signed_value = frames;
            (void)w.submitEngineCommand(command, QStringLiteral("recording latency adjustment"));
        }
    });
    QObject::connect(w.captureManualStopCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        (void)checked;
        updateCaptureDurationControl(w.captureManualStopCheck_, w.captureDurationSpin_);
    });

    QObject::connect(w.stopCaptureButton_, &QPushButton::clicked, &w, [&w] {
        if (w.loopbackRecorder_.isRunning()) {
            w.loopbackRecorder_.stop();
        } else {
            w.runGridLockedEngineAction(
                QStringLiteral("record.stop"),
                [&w](std::uint64_t targetFrame) { w.stopInputCapture(targetFrame); },
                true);
        }
    });
    QObject::connect(w.loadWavButton_, &QPushButton::clicked, &w, [&w] { w.loadWavIntoLooperLane(); });
    QObject::connect(w.shareTracksButton_, &QPushButton::clicked, &w, [&w] { w.shareLocalTracks(); });
    QObject::connect(w.startArmedLaneRecordingButton_, &QPushButton::clicked, &w, [&w] { w.startArmedLooperLaneRecording(); });
    w.looperStack_->onSelected = [&w](int lane) { w.selectedLooperLane_ = lane; };
    w.looperStack_->onAddLane = [&w] { w.addEmptyLooperLane(); };
    w.looperStack_->onAddWav = [&w] { w.addLooperWavs(); };
    w.looperStack_->onMute = [&w](int lane) { w.selectedLooperLane_ = lane; w.toggleSelectedLooperLaneMute(); };
    w.looperStack_->onSolo = [&w](int lane) { w.selectedLooperLane_ = lane; w.toggleSelectedLooperLaneSolo(); };
    w.looperStack_->onArm = [&w](int lane) {
        w.selectedLooperLane_ = lane;
        if (w.trackRecordingWorkflow_.laneArmedAt(w.looperProject_.activeBankIndex(), lane)) {
            w.trackRecordingWorkflow_.disarmLane();
            w.refreshLooperLanes();
            w.appendLog(QStringLiteral("disarmed lane recording"));
            return;
        }
        (void)w.armSelectedLooperLaneRecording();
    };
    w.looperStack_->onRename = [&w](int lane) { w.selectedLooperLane_ = lane; w.renameSelectedLooperLane(); };
    w.looperStack_->onRemove = [&w](int lane) { w.selectedLooperLane_ = lane; w.removeSelectedLooperLane(); };
    w.looperStack_->onGainChanged = [&w](int lane, double gainDb) { w.applyLooperLaneGain(lane, gainDb); };
    w.looperStack_->onRegionCommitted = [&w](int lane, qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame) {
        w.selectedLooperLane_ = lane;
        w.applySelectedLooperLaneRegion(startFrame, sourceStartFrame, sourceEndFrame);
    };
    w.looperStack_->onSeekFrame = [&w](qint64 frame) {
        w.runGridLockedEngineAction(QStringLiteral("track.seek"), [&w, frame](std::uint64_t targetFrame) {
            w.seekPreparedTrack(static_cast<std::uint64_t>(qMax<qint64>(0, frame)), targetFrame);
            w.trackRecordingWorkflow_.noteManualPreparedSeek(
                frame,
                static_cast<qint64>(w.metronomeTransport_.grid().position().rawCurrentFrame));
            w.updateTrackTimeline();
        });
    };
    w.looperStack_->onBankSelected = [&w](int index) {
        w.looperProject_.setActiveBankIndex(index);
        w.selectedLooperLane_ = -1;
        w.refreshLooperLanes();
        w.regeneratePreparedMix();
        w.syncLooperArrangement();
    };
    w.refreshLooperLanes();
    w.regeneratePreparedMix();
    w.syncLooperArrangement();

    return page;
}

QWidget* MainWindowPages::buildMetronomePage(MainWindow& w)
{
    auto* page = new QWidget(&w);

    w.metronomeBpmSpin_ = new QSpinBox(page);
    w.metronomeBpmSpin_->setRange(1, 400);
    w.metronomeBpmSpin_->setValue(qBound(1, static_cast<int>(std::lround(w.trackController_.model().acceptedBpm)), 400));
    applyMutedEditorStyle(w.metronomeBpmSpin_);

    w.metronomeBeatsSpin_ = new QSpinBox(page);
    w.metronomeBeatsSpin_->setRange(1, 16);
    w.metronomeBeatsSpin_->setValue(4);
    applyMutedEditorStyle(w.metronomeBeatsSpin_);


    w.metronomeDivisionBox_ = new QComboBox(page);
    w.metronomeDivisionBox_->addItem(QStringLiteral("Quarter"), 1);
    w.metronomeDivisionBox_->addItem(QStringLiteral("Eighth"), 2);
    w.metronomeDivisionBox_->addItem(QStringLiteral("Triplet"), 3);
    w.metronomeDivisionBox_->addItem(QStringLiteral("16th"), 4);
    w.metronomeDivisionBox_->addItem(QStringLiteral("6th"), 6);
    w.metronomeDivisionBox_->addItem(QStringLiteral("32nd"), 8);
    w.metronomeDivisionBox_->setCurrentIndex(w.metronomeDivisionBox_->findData(1));
    w.metronomeDivisionBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(w.metronomeDivisionBox_);

    w.metronomeModeBox_ = new QComboBox(page);
    w.metronomeModeBox_->addItems({
        QStringLiteral("shared-grid"),
        QStringLiteral("leader-audio"),
        QStringLiteral("listener-compensated"),
    });
    w.metronomeModeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(w.metronomeModeBox_);

    w.metronomeCompensationButton_ = new QPushButton(QStringLiteral("Advanced"), page);
    w.metronomeCompensationButton_->setVisible(false);

    w.metronomeCompensationMaxSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationMaxSpin_->setRange(0.0, 1000.0);
    w.metronomeCompensationMaxSpin_->setDecimals(1);
    w.metronomeCompensationMaxSpin_->setSuffix(QStringLiteral(" ms"));
    w.metronomeCompensationMaxSpin_->setValue(250.0);
    w.metronomeCompensationSmoothingSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationSmoothingSpin_->setRange(0.0, 10000.0);
    w.metronomeCompensationSmoothingSpin_->setDecimals(1);
    w.metronomeCompensationSmoothingSpin_->setSuffix(QStringLiteral(" ms"));
    w.metronomeCompensationSmoothingSpin_->setValue(750.0);
    w.metronomeCompensationDeadbandSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationDeadbandSpin_->setRange(0.0, 1000.0);
    w.metronomeCompensationDeadbandSpin_->setDecimals(1);
    w.metronomeCompensationDeadbandSpin_->setSuffix(QStringLiteral(" ms"));
    w.metronomeCompensationDeadbandSpin_->setValue(1.0);
    w.metronomeCompensationSlewSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationSlewSpin_->setRange(0.0, 10000.0);
    w.metronomeCompensationSlewSpin_->setDecimals(1);
    w.metronomeCompensationSlewSpin_->setSuffix(QStringLiteral(" ms/s"));
    w.metronomeCompensationSlewSpin_->setValue(40.0);
    applyMutedEditorStyle(w.metronomeCompensationMaxSpin_);
    applyMutedEditorStyle(w.metronomeCompensationSmoothingSpin_);
    applyMutedEditorStyle(w.metronomeCompensationDeadbandSpin_);
    applyMutedEditorStyle(w.metronomeCompensationSlewSpin_);
    w.metronomeCompensationMaxSpin_->hide();
    w.metronomeCompensationSmoothingSpin_->hide();
    w.metronomeCompensationDeadbandSpin_->hide();
    w.metronomeCompensationSlewSpin_->hide();

    w.trackMetronomeLabel_ = new QLabel(QStringLiteral("Local metronome stopped"), page);
    w.startTrackMetronomeButton_ = new QPushButton(QStringLiteral("Start"), page);
    w.stopTrackMetronomeButton_ = new QPushButton(QStringLiteral("Stop"), page);
    w.stopTrackMetronomeButton_->setEnabled(false);
    w.metronomeMarkerReferenceCheck_ = new QCheckBox(QStringLiteral("Show marker reference"), page);
    w.metronomeMarkerReferenceCheck_->setChecked(true);

    w.metronomePatternTable_ = new QTableWidget(page);
    w.metronomePatternTable_->setRowCount(2);
    w.metronomePatternTable_->verticalHeader()->setVisible(true);
    w.metronomePatternTable_->setVerticalHeaderLabels(QStringList{QStringLiteral("Play"), QStringLiteral("Accent")});
    w.metronomePatternTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    w.metronomePatternTable_->setSelectionMode(QAbstractItemView::NoSelection);
    w.metronomePatternTable_->setMinimumHeight(110);
    w.metronomePatternTable_->setStyleSheet(QStringLiteral(
        "QTableWidget::item { padding: 3px 6px; }"
        "QCheckBox::indicator { width: 15px; height: 15px; border: 1px solid #52616c; background: #1f2428; }"
        "QCheckBox::indicator:checked { border: 1px solid #66c6a6; background: #66c6a6; }"));

    auto makeControlPair = [page](const QString& label, QWidget* editor) {
        auto* pair = new QWidget(page);
        auto* pairLayout = new QHBoxLayout(pair);
        pairLayout->setContentsMargins(0, 0, 0, 0);
        pairLayout->setSpacing(6);
        pairLayout->addWidget(new QLabel(label, pair));
        pairLayout->addWidget(editor, 0, Qt::AlignLeft);
        pairLayout->addStretch(1);
        return pair;
    };
    auto* controls = new QGridLayout();
    controls->setHorizontalSpacing(24);
    controls->setVerticalSpacing(10);
    controls->addWidget(makeControlPair(QStringLiteral("BPM"), w.metronomeBpmSpin_), 0, 0);
    controls->addWidget(makeControlPair(QStringLiteral("Beats"), w.metronomeBeatsSpin_), 0, 1);
    controls->addWidget(makeControlPair(QStringLiteral("Division"), w.metronomeDivisionBox_), 0, 2);
    auto* modeRow = new QWidget(page);
    auto* modeLayout = new QHBoxLayout(modeRow);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(6);
    modeLayout->addWidget(new QLabel(QStringLiteral("Mode"), modeRow));
    modeLayout->addWidget(w.metronomeModeBox_, 0, Qt::AlignLeft);
    modeLayout->addWidget(w.metronomeCompensationButton_, 0, Qt::AlignLeft);
    modeLayout->addStretch(1);
    controls->addWidget(modeRow, 0, 3);
    controls->addWidget(new QLabel(QStringLiteral("Pattern"), page), 1, 0);
    controls->addWidget(w.trackMetronomeLabel_, 1, 1);
    controls->addWidget(w.metronomeMarkerReferenceCheck_, 1, 2, 1, 2);
    controls->setColumnStretch(1, 1);
    controls->setColumnStretch(3, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(w.startTrackMetronomeButton_);
    buttons->addWidget(w.stopTrackMetronomeButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(controls);
    layout->addLayout(buttons);
    layout->addWidget(w.metronomePatternTable_, 1);

    QObject::connect(w.startTrackMetronomeButton_, &QPushButton::clicked, &w, [&w] { w.startTrackMetronome(); });
    QObject::connect(w.stopTrackMetronomeButton_, &QPushButton::clicked, &w, [&w] { w.stopTrackMetronome(); });
    QObject::connect(w.metronomeMarkerReferenceCheck_, &QCheckBox::toggled, &w, [&w] {
        w.updatePlaybackGrid();
    });
    QObject::connect(w.metronomeBpmSpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w] {
        w.updateTrackMetronomeInterval();
    });
    QObject::connect(w.metronomeModeBox_, &QComboBox::currentTextChanged, &w, [&w] {
        w.updateMetronomeCompensationVisibility();
        w.sendMetronomeModeToJam();
        w.sendMetronomeSettingsToPeer();
    });
    QObject::connect(w.metronomeCompensationButton_, &QPushButton::clicked, &w, [&w] {
        w.showMetronomeCompensationDialog();
    });
    QObject::connect(w.metronomeBeatsSpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w] {
        w.rebuildMetronomePattern();
        w.updateTrackMetronomeInterval();
    });
    QObject::connect(w.metronomeDivisionBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.rebuildMetronomePattern(true);
        w.updateTrackMetronomeInterval();
    });

    w.rebuildMetronomePattern();
    w.updateMetronomeCompensationVisibility();
    return page;
}

QWidget* MainWindowPages::buildMixPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    auto* content = new QWidget(page);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto makeValueLabel = [page](const QString& text) {
        auto* label = new QLabel(text, page);
        label->setFrameShape(QFrame::StyledPanel);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(82);
        return label;
    };
    auto makeRow = [page](const QString& name, QSlider* slider, QLabel* valueLabel) {
        auto* row = new QWidget(page);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setMinimumWidth(120);
        rowLayout->addWidget(nameLabel);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(valueLabel);
        return row;
    };
    auto makeMeterRow = [page](const QString& name, LevelMeterWidget* meter, QLabel* valueLabel = nullptr) {
        auto* row = new QWidget(page);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setMinimumWidth(120);
        rowLayout->addWidget(nameLabel);
        rowLayout->addWidget(meter, 1);
        if (valueLabel != nullptr) {
            rowLayout->addWidget(valueLabel);
        }
        return row;
    };
    auto makeSectionLabel = [content](const QString& text) {
        auto* label = new QLabel(text, content);
        label->setStyleSheet(QStringLiteral("font-weight: 600; margin-top: 8px;"));
        return label;
    };
    w.mixJamRecordingRow_ = new QWidget(content);
    auto* recordingLayout = new QHBoxLayout(w.mixJamRecordingRow_);
    recordingLayout->setContentsMargins(0, 0, 0, 0);
    recordingLayout->setSpacing(10);
    auto* recordingName = new QLabel(QStringLiteral("Jam recording"), w.mixJamRecordingRow_);
    recordingName->setMinimumWidth(120);
    w.jamRecordingButton_ = new QPushButton(QStringLiteral("Start Recording Jam"), w.mixJamRecordingRow_);
    w.jamRecordingLabel_ = new QLabel(QStringLiteral("Stopped"), w.mixJamRecordingRow_);
    w.jamRecordingLabel_->setFrameShape(QFrame::StyledPanel);
    w.jamRecordingLabel_->setMinimumWidth(180);
    recordingLayout->addWidget(recordingName);
    recordingLayout->addWidget(w.jamRecordingButton_);
    recordingLayout->addWidget(w.jamRecordingLabel_, 1);
    layout->addWidget(w.mixJamRecordingRow_);

    w.mixInputMeter_ = new LevelMeterWidget(page);
    w.mixSendMeter_ = new LevelMeterWidget(page);
    w.mixMonitorMeter_ = new LevelMeterWidget(page);
    w.mixTrackMeter_ = new LevelMeterWidget(page);
    w.mixMetronomeMeter_ = new LevelMeterWidget(page);
    w.mixOutputMeter_ = new LevelMeterWidget(page);
    w.mixRemotePeerMeter_ = new LevelMeterWidget(page);
    w.mixOutputClipLabel_ = makeValueLabel(QStringLiteral("clip 0"));

    w.mixSendLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.mixSendLevelSlider_->setRange(-60, 12);
    w.mixSendLevelSlider_->setValue(0);
    applyJamSliderStyle(w.mixSendLevelSlider_);
    w.mixSendLevelSlider_->setMinimumWidth(220);
    w.mixSendLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.mixSendLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    w.mixMonitorCheck_ = new QCheckBox(QStringLiteral("Monitor input"), page);
    w.mixMonitorCheck_->setChecked(false);
    w.mixMonitorLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.mixMonitorLevelSlider_->setRange(-60, 12);
    w.mixMonitorLevelSlider_->setValue(-18);
    applyJamSliderStyle(w.mixMonitorLevelSlider_);
    w.mixMonitorLevelSlider_->setMinimumWidth(220);
    w.mixMonitorLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    w.mixMonitorLevelLabel_ = makeValueLabel(QStringLiteral("-18.0 dB"));

    w.trackLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.trackLevelSlider_->setRange(-60, 12);
    w.trackLevelSlider_->setValue(0);
    applyJamSliderStyle(w.trackLevelSlider_);
    w.trackLevelSlider_->setMinimumWidth(220);
    w.trackLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.trackLevelDbLabel_ = makeValueLabel(dbText(w.trackController_.model().trackGainDb));

    w.metronomeLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.metronomeLevelSlider_->setRange(-60, 12);
    w.metronomeLevelSlider_->setValue(-10);
    applyJamSliderStyle(w.metronomeLevelSlider_);
    w.metronomeLevelSlider_->setMinimumWidth(220);
    w.metronomeLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.localMetronomeLevelSlider_ = w.metronomeLevelSlider_;
    w.mixMetronomeLevelSlider_ = w.metronomeLevelSlider_;
    w.mixMetronomeLevelLabel_ = makeValueLabel(QStringLiteral("-10.0 dB"));

    w.remoteLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.remoteLevelSlider_->setRange(-60, 12);
    w.remoteLevelSlider_->setValue(0);
    applyJamSliderStyle(w.remoteLevelSlider_);
    w.remoteLevelSlider_->setMinimumWidth(220);
    w.remoteLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.mixRemotePeerSlider_ = w.remoteLevelSlider_;
    w.mixRemotePeerLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    w.mixTrackLevelSlider_ = w.trackLevelSlider_;
    w.mixTrackLevelLabel_ = w.trackLevelDbLabel_;

    w.mixLocalInputSection_ = makeSectionLabel(QStringLiteral("Local input"));
    w.mixInputMeterRow_ = makeMeterRow(QStringLiteral("Input"), w.mixInputMeter_);
    w.mixSendRow_ = makeRow(QStringLiteral("Send"), w.mixSendLevelSlider_, w.mixSendLevelLabel_);
    w.mixSendMeterRow_ = makeMeterRow(QStringLiteral("Post-send"), w.mixSendMeter_);
    layout->addWidget(w.mixLocalInputSection_);
    layout->addWidget(w.mixInputMeterRow_);
    layout->addWidget(w.mixSendRow_);
    layout->addWidget(w.mixSendMeterRow_);

    w.mixMonitorEnableRow_ = new QWidget(page);
    auto* monitorEnableLayout = new QHBoxLayout(w.mixMonitorEnableRow_);
    monitorEnableLayout->setContentsMargins(0, 0, 0, 0);
    monitorEnableLayout->setSpacing(10);
    auto* monitorName = new QLabel(QStringLiteral("Monitoring"), w.mixMonitorEnableRow_);
    monitorName->setMinimumWidth(120);
    monitorEnableLayout->addWidget(monitorName);
    monitorEnableLayout->addWidget(w.mixMonitorCheck_, 1);
    w.mixMonitorRow_ = makeRow(QStringLiteral("Monitor"), w.mixMonitorLevelSlider_, w.mixMonitorLevelLabel_);
    w.mixMonitorMeterRow_ = makeMeterRow(QStringLiteral("Monitor meter"), w.mixMonitorMeter_);
    layout->addWidget(w.mixMonitorEnableRow_);
    layout->addWidget(w.mixMonitorRow_);
    layout->addWidget(w.mixMonitorMeterRow_);

    w.mixTrackSection_ = makeSectionLabel(QStringLiteral("Track"));
    w.mixTrackRow_ = makeRow(QStringLiteral("Track"), w.trackLevelSlider_, w.trackLevelDbLabel_);
    w.mixTrackMeterRow_ = makeMeterRow(QStringLiteral("Track meter"), w.mixTrackMeter_);
    layout->addWidget(w.mixTrackSection_);
    layout->addWidget(w.mixTrackRow_);
    layout->addWidget(w.mixTrackMeterRow_);

    w.mixMetronomeSection_ = makeSectionLabel(QStringLiteral("Metronome"));
    w.mixMetronomeRow_ = makeRow(QStringLiteral("Metronome"), w.metronomeLevelSlider_, w.mixMetronomeLevelLabel_);
    w.mixMetronomeMeterRow_ = makeMeterRow(QStringLiteral("Click meter"), w.mixMetronomeMeter_);
    layout->addWidget(w.mixMetronomeSection_);
    layout->addWidget(w.mixMetronomeRow_);
    layout->addWidget(w.mixMetronomeMeterRow_);

    w.mixOutputSection_ = makeSectionLabel(QStringLiteral("Output"));
    w.mixOutputMeterRow_ = makeMeterRow(QStringLiteral("Main output"), w.mixOutputMeter_, w.mixOutputClipLabel_);

    layout->addWidget(w.mixOutputSection_);
    layout->addWidget(w.mixOutputMeterRow_);

    w.mixRemotePeersSection_ = makeSectionLabel(QStringLiteral("Remote peers"));
    layout->addWidget(w.mixRemotePeersSection_);
    w.mixRemotePeerRow_ = new QWidget(page);
    auto* remoteLayout = new QVBoxLayout(w.mixRemotePeerRow_);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(6);
    remoteLayout->addWidget(makeRow(QStringLiteral("Remote mix"), w.remoteLevelSlider_, w.mixRemotePeerLevelLabel_));
    remoteLayout->addWidget(makeMeterRow(QStringLiteral("Remote meter"), w.mixRemotePeerMeter_));
    auto* peerList = new QWidget(w.mixRemotePeerRow_);
    w.mixRemotePeerListLayout_ = new QVBoxLayout(peerList);
    w.mixRemotePeerListLayout_->setContentsMargins(120, 0, 0, 0);
    w.mixRemotePeerListLayout_->setSpacing(4);
    remoteLayout->addWidget(peerList);
    layout->addWidget(w.mixRemotePeerRow_);
    layout->addStretch(1);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);

    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll, 1);

    QObject::connect(w.trackLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        w.trackController_.model().trackGainDb = static_cast<double>(value);
        if (w.trackLevelDbLabel_) {
            w.trackLevelDbLabel_->setText(dbText(w.trackController_.model().trackGainDb));
        }
        if (w.jam2_.isRunning()) {
            w.sendPreparedTrackLevel();
        }
    });
    QObject::connect(w.mixSendLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixSendLevelLabel_) {
            w.mixSendLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        w.updateRuntimeControls();
    });
    QObject::connect(w.mixMonitorCheck_, &QCheckBox::toggled, &w, [&w] {
        w.updateMixControls();
        w.updateRuntimeControls();
    });
    QObject::connect(w.mixMonitorLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixMonitorLevelLabel_) {
            w.mixMonitorLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        w.updateRuntimeControls();
    });
    QObject::connect(w.metronomeLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixMetronomeLevelLabel_) {
            w.mixMetronomeLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        if (w.jam2_.isRunning()) {
            const double metronomeLevel = gainFromDb(static_cast<double>(value));
            w.submitEngineGain(
                jam2::EngineCommandType::SetMetronomeLevel,
                metronomeLevel,
                QStringLiteral("metronome level"));
        }
    });
    QObject::connect(w.remoteLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixRemotePeerLevelLabel_) {
            w.mixRemotePeerLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        w.updateRuntimeControls();
    });
    QObject::connect(w.jamRecordingButton_, &QPushButton::clicked, &w, [&w] {
        if (w.trackRecordingWorkflow_.jamRecordingActive()) {
            w.stopJamRecording();
        } else {
            w.startJamRecording();
        }
    });
    w.updateMixControls();
    w.updateMixRemotePeers();
    w.updateJamRecordingControls();
    return page;
}
