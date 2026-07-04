#pragma once

#include "BeatGridWidget.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "Jam2Process.hpp"
#include "LeadCueModel.hpp"
#include "SharedTrackController.hpp"

#include <QCheckBox>
#include <QBuffer>
#include <QByteArray>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonObject>
#include <QAudioOutput>
#include <QProcess>
#include <QSoundEffect>
#include <QTimer>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QWidget>

#include <array>
#include <cstdint>

class QEvent;
class WaveformWidget;

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    void buildUi();
    QWidget* buildSessionPage();
    QWidget* buildSongPage();
    QWidget* buildArrangementPage();
    QWidget* buildTrackPage();
    QWidget* buildStatsPage();

    void startJam();
    void showStartJamDialog();
    void showJoinJamDialog();
    void stopJam();
    void refreshDevices();
    void appendLog(const QString& line);
    void handleStatus(const QJsonObject& status);
    void handleControlMessage(const QJsonObject& message);
    void sendControl(const QJsonObject& message);
    void updateRuntimeControls();
    void requestLeadSwap();
    void updateLeadLabels();
    void updateTrackControls();
    void loadTrackMetadata();
    QString selectedDeviceId() const;
    QStringList captureOptionArgs() const;
    void handleCaptureOutputLine(const QString& line);
    void chooseCaptureFolder();
    void refreshLoopbackSources();
    void startInputCapture();
    void showInputCaptureDialog();
    void startLoopbackCapture();
    void showLoopbackCaptureDialog();
    void stopInputCapture();
    void importLastCapture();
    void loadTrackIntoPlayer();
    void playTrack();
    void stopTrack();
    void setLoopStartAtCurrentPosition();
    void setLoopEndAtCurrentPosition();
    void clearTrackLoop();
    void updateTrackTimeline();
    void startTrackMetronome();
    void stopTrackMetronome();
    void updateTrackMetronomeInterval();
    void playTrackMetronomeClick();
    void prepareTrackMetronomeClick();
    void sendTrackFile();
    void receiveTrackFileStart(const QJsonObject& message);
    void receiveTrackFileChunk(const QJsonObject& message);
    void receiveTrackFileDone(const QJsonObject& message);
    QJsonObject trackMetadataMessage(const QString& type) const;
    QJsonObject trackToJson() const;
    void loadTrackJson(const QJsonObject& object);
    void newSong();
    void openSong();
    void saveSong();
    void refreshSongViews();

    QStringList commonJamArgs() const;
    QString sessionHex() const;
    QString keyHex() const;
    void generateSession();

    Jam2Process jam2_;
    QProcess captureProcess_;
    ControlServer controlServer_;
    ControlClient controlClient_;
    SharedTrackController trackController_;
    LeadCueModel leadCue_;
    BeatGridModel songModel_;
    QMediaPlayer trackPlayer_;
    QAudioOutput trackAudio_;
    QSoundEffect trackMetronomeClick_;
    QByteArray processedTrackBytes_;
    QBuffer processedTrackBuffer_;

    QComboBox* modeBox_ = nullptr;
    QLineEdit* jam2PathEdit_ = nullptr;
    QLineEdit* bindHostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* publicHostEdit_ = nullptr;
    QLineEdit* connectUrlEdit_ = nullptr;
    QLineEdit* generatedUrlEdit_ = nullptr;
    QLineEdit* stunServerEdit_ = nullptr;
    QSpinBox* stunTimeoutSpin_ = nullptr;
    QSpinBox* stunRetriesSpin_ = nullptr;
    QSpinBox* waitMsSpin_ = nullptr;
    QSpinBox* streamMsSpin_ = nullptr;
    QSpinBox* streamLingerMsSpin_ = nullptr;
    QSpinBox* statsWarmupMsSpin_ = nullptr;
    QLineEdit* logStatsEdit_ = nullptr;
    QSpinBox* socketSendBufferSpin_ = nullptr;
    QSpinBox* socketRecvBufferSpin_ = nullptr;
    QComboBox* deviceBox_ = nullptr;
    QLineEdit* inputChannelsEdit_ = nullptr;
    QLineEdit* outputChannelsEdit_ = nullptr;
    QSpinBox* sampleRateSpin_ = nullptr;
    QSpinBox* bufferSizeSpin_ = nullptr;
    QSpinBox* frameSizeSpin_ = nullptr;
    QSpinBox* prefillSpin_ = nullptr;
    QSpinBox* playbackMaxSpin_ = nullptr;
    QSpinBox* captureRingSpin_ = nullptr;
    QSpinBox* playbackRingSpin_ = nullptr;
    QCheckBox* driftCorrectionCheck_ = nullptr;
    QDoubleSpinBox* driftSmoothingSpin_ = nullptr;
    QSpinBox* driftDeadbandSpin_ = nullptr;
    QSpinBox* driftMaxCorrectionSpin_ = nullptr;
    QCheckBox* noStunCheck_ = nullptr;
    QCheckBox* metronomeCheck_ = nullptr;
    QSpinBox* bpmSpin_ = nullptr;
    QComboBox* metronomeModeBox_ = nullptr;
    QSlider* metronomeLevelSlider_ = nullptr;
    QSlider* remoteLevelSlider_ = nullptr;
    QCheckBox* sampleTimePlayoutCheck_ = nullptr;
    QSpinBox* playoutDelaySpin_ = nullptr;
    QCheckBox* adaptiveCushionCheck_ = nullptr;
    QSpinBox* adaptiveTargetSpin_ = nullptr;
    QSpinBox* adaptiveMinSpin_ = nullptr;
    QSpinBox* adaptiveMaxSpin_ = nullptr;
    QSpinBox* adaptiveReleaseSpin_ = nullptr;
    QLineEdit* extraArgsEdit_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* rttLabel_ = nullptr;
    QLabel* jitterLabel_ = nullptr;
    QLabel* lossLabel_ = nullptr;
    QLabel* depthLabel_ = nullptr;
    QLabel* underrunLabel_ = nullptr;
    QLabel* driftLabel_ = nullptr;
    QLabel* leadLabel_ = nullptr;
    QLabel* leadPendingLabel_ = nullptr;
    QLabel* trackNameLabel_ = nullptr;
    QLabel* trackAnalysisLabel_ = nullptr;
    WaveformWidget* trackWaveform_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLineEdit* songTitleEdit_ = nullptr;
    QLineEdit* capturePathEdit_ = nullptr;
    QLineEdit* captureOutputEdit_ = nullptr;
    QComboBox* loopbackSourceBox_ = nullptr;
    QSpinBox* captureDurationSpin_ = nullptr;
    QCheckBox* captureTriggerCheck_ = nullptr;
    QCheckBox* trimLeadingCheck_ = nullptr;
    QCheckBox* trimTrailingCheck_ = nullptr;
    QDoubleSpinBox* triggerThresholdSpin_ = nullptr;
    QDoubleSpinBox* tailThresholdSpin_ = nullptr;
    QSpinBox* preRollSpin_ = nullptr;
    QSpinBox* triggerHoldSpin_ = nullptr;
    QSpinBox* tailSilenceSpin_ = nullptr;
    QLabel* captureProgressLabel_ = nullptr;
    QLabel* trackPlaybackLabel_ = nullptr;
    QDoubleSpinBox* trackSpeedSpin_ = nullptr;
    QSpinBox* trackPitchSpin_ = nullptr;
    QSpinBox* trackBpmSpin_ = nullptr;
    QPushButton* startTrackMetronomeButton_ = nullptr;
    QPushButton* stopTrackMetronomeButton_ = nullptr;
    QLabel* trackMetronomeLabel_ = nullptr;
    QPushButton* playTrackButton_ = nullptr;
    QPushButton* stopTrackButton_ = nullptr;
    QPushButton* loopStartButton_ = nullptr;
    QPushButton* loopEndButton_ = nullptr;
    QPushButton* clearLoopButton_ = nullptr;
    QLabel* loopStatusLabel_ = nullptr;
    QPushButton* shareTrackFileButton_ = nullptr;
    QSlider* trackLevelSlider_ = nullptr;
    QPushButton* captureButton_ = nullptr;
    QPushButton* loopbackCaptureButton_ = nullptr;
    QPushButton* stopCaptureButton_ = nullptr;
    QPushButton* importCaptureButton_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    BeatGridWidget* chordGrid_ = nullptr;
    BeatGridWidget* beatGrid_ = nullptr;
    BeatGridWidget* lyricGrid_ = nullptr;
    QComboBox* arrangementSectionBox_ = nullptr;
    QTextEdit* arrangementEdit_ = nullptr;
    QTabWidget* tabs_ = nullptr;

    std::uint64_t sessionId_ = 0;
    std::array<std::uint8_t, 16> sessionKey_{};
    QString lastCapturePath_;
    QJsonObject lastCaptureSummary_;
    QByteArray incomingTrackBytes_;
    QString incomingTrackName_;
    QString incomingTrackSha256_;
    qint64 incomingTrackBytesExpected_ = 0;
    int incomingTrackNextChunk_ = 0;
    QTimer trackTimelineTimer_;
    QTimer trackMetronomeTimer_;
    QString trackMetronomeClickPath_;
    int trackMetronomeBeat_ = 0;
};
