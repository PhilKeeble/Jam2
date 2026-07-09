#pragma once

#include "BeatGridWidget.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "Jam2Process.hpp"
#include "SharedTrackController.hpp"

#include "metronome.hpp"

#include <QAudioDevice>
#include <QCheckBox>
#include <QByteArray>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>

class QEvent;
class QGroupBox;
class QTableWidget;
class WaveformWidget;
class LevelMeterWidget;
class QAudioSink;
class LocalMetronomeDevice;
class TrackPlaybackDevice;

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    void buildUi();
    QWidget* buildSessionPage();
    QWidget* buildSongPage();
    QWidget* buildTrackPage();
    QWidget* buildMetronomePage();
    QWidget* buildMixPage();

    void startJam();
    void showStartJamDialog();
    void showJoinJamDialog();
    void stopJam();
    void refreshDevices();
    void appendLog(const QString& line);
    void handleOutputLine(const QString& line);
    void handleStatus(const QJsonObject& status);
    void handleStatsLine(const QString& line);
    void updateStatsDisplay(const QJsonObject& stats);
    void updateMixMeters(const QJsonObject& stats);
    void handleControlMessage(const QJsonObject& message);
    void sendControl(const QJsonObject& message);
    void handleControlState(const QString& state, bool serverSide);
    void refreshControlConnection();
    void scheduleControlReconnect();
    void sendLeaderSettings();
    QJsonObject leaderSettingsMessage() const;
    void applyLeaderSettings(const QJsonObject& settings);
    void handleMeshPeerAuthenticated(const QString& token, const QJsonObject& message);
    void broadcastMeshPeerList();
    void applyMeshPeerList(const QJsonObject& message);
    void restartMeshEngineFromPeerList();
    QStringList meshPeerEndpointsExcludingSelf() const;
    QString meshBindEndpoint() const;
    QString localMeshEndpoint() const;
    QString meshPeerToken();
    bool selectedDeviceSupportsSampleRate(int sampleRate);
    void launchJamProcess(QStringList args);
    void launchPendingJoin();
    bool startGuiControlServer(QStringList& args);
    void stopGuiControlServer();
    void sendJamCommand(const QString& line);
    void readGuiControlSocket();
    void handleGuiControlLine(const QString& line);
    void updateRuntimeControls();
    void updateMixControls();
    void setMixRemotePeerVisible(bool visible);
    void startJamRecording();
    void stopJamRecording();
    void updateJamRecordingControls();
    void refreshGeneratedUrl();
    QString meshInviteUrl() const;
    void showPendingMeshInviteUrl();
    void updateConnectionControlState();
    void updateTrackControls();
    void loadTrackMetadata();
    QString selectedDeviceId() const;
    QAudioDevice selectedLocalOutputDevice() const;
    QStringList captureOptionArgs() const;
    void handleCaptureOutputLine(const QString& line);
    void chooseCaptureFolder();
    void refreshLocalOutputs();
    void refreshLoopbackSources();
    void startInputCapture();
    void showInputCaptureDialog();
    void startLoopbackCapture();
    void showLoopbackCaptureDialog();
    void stopInputCapture();
    void importLastCapture();
    void loadTrackIntoPlayer();
    void applyTrackPlaybackSettings();
    void playTrack();
    void stopTrack();
    void setLoopStartAtCurrentPosition();
    void setLoopEndAtCurrentPosition();
    void clearTrackLoop();
    void updateTrackTimeline();
    void startTrackMetronome();
    void stopTrackMetronome();
    void updateTrackMetronomeInterval();
    void rebuildMetronomePattern();
    jam2::metronome::PatternSnapshot currentMetronomePattern() const;
    void applyMetronomePatternToLocalDevice();
    void sendMetronomePatternToJam();
    void sendMetronomeSettingsToPeer();
    void applyRemoteMetronomeSettings(const QJsonObject& message);
    void sendTrackFile();
    void receiveTrackFileStart(const QJsonObject& message);
    void receiveTrackFileChunk(const QJsonObject& message);
    void receiveTrackFileDone(const QJsonObject& message);
    QJsonObject trackMetadataMessage(const QString& type) const;
    QJsonObject trackToJson() const;
    void loadTrackJson(const QJsonObject& object);
    QJsonObject songToJson() const;
    bool loadSongJson(const QJsonObject& object);
    void newSong();
    void openSong();
    void saveSong();
    void refreshSongViews();
    void refreshSongView(const QString& lane);
    void sendSongSnapshot();

    QStringList commonJamArgs(bool includeExtraArgs = true) const;
    void applyTuningProfileName(const QString& name);
    QString sessionHex() const;
    QString keyHex() const;
    void generateSession();

    Jam2Process jam2_;
    QProcess captureProcess_;
    ControlServer controlServer_;
    ControlClient controlClient_;
    SharedTrackController trackController_;
    BeatGridModel chordModel_;
    BeatGridModel beatModel_;
    BeatGridModel lyricModel_;
    std::unique_ptr<QAudioSink> localMetronomeSink_;
    std::unique_ptr<LocalMetronomeDevice> localMetronomeDevice_;
    std::unique_ptr<QAudioSink> trackSink_;
    std::unique_ptr<TrackPlaybackDevice> trackDevice_;

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
    QCheckBox* statsCheck_ = nullptr;
    QCheckBox* guiControlCheck_ = nullptr;
    QCheckBox* meshModeCheck_ = nullptr;
    QSpinBox* meshMaxPeersSpin_ = nullptr;
    QSpinBox* statsWarmupMsSpin_ = nullptr;
    QLineEdit* logStatsEdit_ = nullptr;
    QSpinBox* socketSendBufferSpin_ = nullptr;
    QSpinBox* socketRecvBufferSpin_ = nullptr;
    QComboBox* profileBox_ = nullptr;
    QComboBox* osPriorityBox_ = nullptr;
    QComboBox* deviceBox_ = nullptr;
    QComboBox* localOutputBox_ = nullptr;
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
    QSpinBox* bpmSpin_ = nullptr;
    QComboBox* metronomeModeBox_ = nullptr;
    QSlider* metronomeLevelSlider_ = nullptr;
    QSlider* remoteLevelSlider_ = nullptr;
    QCheckBox* sampleTimePlayoutCheck_ = nullptr;
    QSpinBox* playoutDelaySpin_ = nullptr;
    QSpinBox* jitterBufferSpin_ = nullptr;
    QSpinBox* jitterBufferMaxSpin_ = nullptr;
    QCheckBox* adaptiveCushionCheck_ = nullptr;
    QSpinBox* adaptiveTargetSpin_ = nullptr;
    QSpinBox* adaptiveMinSpin_ = nullptr;
    QSpinBox* adaptiveMaxSpin_ = nullptr;
    QSpinBox* adaptiveReleaseSpin_ = nullptr;
    QLineEdit* extraArgsEdit_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* refreshControlButton_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* jitterLabel_ = nullptr;
    QLabel* lossLabel_ = nullptr;
    QLabel* depthLabel_ = nullptr;
    QLabel* underrunLabel_ = nullptr;
    QLabel* driftLabel_ = nullptr;
    QLabel* latencyLabel_ = nullptr;
    QLabel* ringDepthLabel_ = nullptr;
    QLabel* missingFramesLabel_ = nullptr;
    QLabel* diagnosisLabel_ = nullptr;
    QLabel* trackNameLabel_ = nullptr;
    WaveformWidget* trackWaveform_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLineEdit* songTitleEdit_ = nullptr;
    QLineEdit* capturePathEdit_ = nullptr;
    QLineEdit* captureOutputEdit_ = nullptr;
    QComboBox* loopbackSourceBox_ = nullptr;
    QCheckBox* captureManualStopCheck_ = nullptr;
    QSpinBox* captureDurationSpin_ = nullptr;
    QCheckBox* captureTriggerCheck_ = nullptr;
    QCheckBox* trimLeadingCheck_ = nullptr;
    QCheckBox* trimTrailingCheck_ = nullptr;
    QDoubleSpinBox* triggerThresholdSpin_ = nullptr;
    QDoubleSpinBox* tailThresholdSpin_ = nullptr;
    QSpinBox* preRollSpin_ = nullptr;
    QSpinBox* triggerHoldSpin_ = nullptr;
    QSpinBox* tailSilenceSpin_ = nullptr;
    QDoubleSpinBox* trackSpeedSpin_ = nullptr;
    QSpinBox* trackPitchSpin_ = nullptr;
    QSlider* trackSpeedSlider_ = nullptr;
    QSlider* trackPitchSlider_ = nullptr;
    QSpinBox* metronomeBpmSpin_ = nullptr;
    QSpinBox* metronomeBeatsSpin_ = nullptr;
    QComboBox* metronomeDivisionBox_ = nullptr;
    QSlider* localMetronomeLevelSlider_ = nullptr;
    QPushButton* startTrackMetronomeButton_ = nullptr;
    QPushButton* stopTrackMetronomeButton_ = nullptr;
    QLabel* trackMetronomeLabel_ = nullptr;
    QTableWidget* metronomePatternTable_ = nullptr;
    QPushButton* playTrackButton_ = nullptr;
    QPushButton* stopTrackButton_ = nullptr;
    QPushButton* loopStartButton_ = nullptr;
    QPushButton* loopEndButton_ = nullptr;
    QPushButton* clearLoopButton_ = nullptr;
    QCheckBox* loopEnabledCheck_ = nullptr;
    QCheckBox* waveformGridCheck_ = nullptr;
    QPushButton* shareTrackFileButton_ = nullptr;
    QSlider* trackLevelSlider_ = nullptr;
    QLabel* trackLevelDbLabel_ = nullptr;
    QSlider* mixTrackLevelSlider_ = nullptr;
    QLabel* mixTrackLevelLabel_ = nullptr;
    LevelMeterWidget* mixTrackMeter_ = nullptr;
    QWidget* mixStandaloneOutputRow_ = nullptr;
    QWidget* mixJamRecordingRow_ = nullptr;
    QWidget* mixLocalInputSection_ = nullptr;
    QWidget* mixInputMeterRow_ = nullptr;
    QWidget* mixSendRow_ = nullptr;
    QWidget* mixSendMeterRow_ = nullptr;
    QWidget* mixMonitorEnableRow_ = nullptr;
    QWidget* mixMonitorRow_ = nullptr;
    QWidget* mixMonitorMeterRow_ = nullptr;
    QWidget* mixTrackSection_ = nullptr;
    QWidget* mixTrackRow_ = nullptr;
    QWidget* mixTrackMeterRow_ = nullptr;
    QWidget* mixMetronomeSection_ = nullptr;
    QWidget* mixMetronomeRow_ = nullptr;
    QWidget* mixMetronomeMeterRow_ = nullptr;
    QWidget* mixOutputSection_ = nullptr;
    QWidget* mixOutputMeterRow_ = nullptr;
    QWidget* mixRemotePeersSection_ = nullptr;
    QSlider* mixSendLevelSlider_ = nullptr;
    QLabel* mixSendLevelLabel_ = nullptr;
    LevelMeterWidget* mixInputMeter_ = nullptr;
    LevelMeterWidget* mixSendMeter_ = nullptr;
    QCheckBox* mixMonitorCheck_ = nullptr;
    QSlider* mixMonitorLevelSlider_ = nullptr;
    QLabel* mixMonitorLevelLabel_ = nullptr;
    LevelMeterWidget* mixMonitorMeter_ = nullptr;
    QSlider* mixMetronomeLevelSlider_ = nullptr;
    QLabel* mixMetronomeLevelLabel_ = nullptr;
    LevelMeterWidget* mixMetronomeMeter_ = nullptr;
    LevelMeterWidget* mixOutputMeter_ = nullptr;
    QWidget* mixRemotePeerRow_ = nullptr;
    QSlider* mixRemotePeerSlider_ = nullptr;
    QLabel* mixRemotePeerLevelLabel_ = nullptr;
    LevelMeterWidget* mixRemotePeerMeter_ = nullptr;
    QLabel* mixOutputClipLabel_ = nullptr;
    QPushButton* jamRecordingButton_ = nullptr;
    QLabel* jamRecordingLabel_ = nullptr;
    QCheckBox* focusFrequencyCheck_ = nullptr;
    QComboBox* focusPresetBox_ = nullptr;
    QSlider* focusFrequencySlider_ = nullptr;
    QSpinBox* focusFrequencySpin_ = nullptr;
    QPushButton* captureButton_ = nullptr;
    QPushButton* loopbackCaptureButton_ = nullptr;
    QPushButton* stopCaptureButton_ = nullptr;
    QPushButton* importCaptureButton_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    BeatGridWidget* chordGrid_ = nullptr;
    BeatGridWidget* beatGrid_ = nullptr;
    BeatGridWidget* lyricGrid_ = nullptr;
    QTabWidget* tabs_ = nullptr;

    std::uint64_t sessionId_ = 0;
    std::array<std::uint8_t, 16> sessionKey_{};
    QString lastCapturePath_;
    QString jamRecordingFolder_;
    bool jamRecordingActive_ = false;
    QJsonObject lastCaptureSummary_;
    QByteArray incomingTrackBytes_;
    QString incomingTrackName_;
    QString incomingTrackSha256_;
    qint64 incomingTrackBytesExpected_ = 0;
    int incomingTrackNextChunk_ = 0;
    QTimer trackTimelineTimer_;
    QTimer controlReconnectTimer_;
    QTcpServer guiControlServer_;
    QTcpSocket* guiControlSocket_ = nullptr;
    QByteArray guiControlBuffer_;
    QString controlHost_;
    quint16 controlPort_ = 0;
    QString controlSessionHex_;
    QString controlKeyHex_;
    bool controlServerMode_ = false;
    bool controlReconnectEnabled_ = false;
    int controlReconnectAttempts_ = 0;
    QStringList pendingJoinBaseArgs_;
    bool pendingJoinLaunch_ = false;
    bool meshActive_ = false;
    bool meshRestarting_ = false;
    bool pendingMeshInvitePopup_ = false;
    QString meshPeerToken_;
    QMap<QString, QString> meshPeerEndpoints_;
    QStringList currentMeshPeers_;
    bool localMetronomeRunning_ = false;
    bool applyingRemoteMetronomeSettings_ = false;
    QVector<bool> metronomeEnabledSteps_;
    QVector<bool> metronomeAccents_;
};
