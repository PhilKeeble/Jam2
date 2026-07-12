#pragma once

#include "BeatGridWidget.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "GuiLoopbackRecorder.hpp"
#include "Jam2Process.hpp"
#include "LooperProject.hpp"
#include "PlaybackGrid.hpp"
#include "PreparedMixRenderer.hpp"
#include "SharedTrackController.hpp"

#include "metronome.hpp"

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
#include <QSet>
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
#include <functional>

class QEvent;
class QCloseEvent;
class QGroupBox;
class QTableWidget;
class QVBoxLayout;
class WaveformWidget;
class LooperLaneStackWidget;
class LevelMeterWidget;

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    void buildUi();
    QWidget* buildSessionPage();
    QWidget* buildSongPage();
    QWidget* buildTrackPage();
    QWidget* buildMetronomePage();
    QWidget* buildMixPage();

    void startJam();
    void showLocalPerformSetup();
    void startLocalPerform();
    void showStartJamDialog();
    void showJoinJamDialog();
    void stopJam(bool returnToLocal = true);
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
    void launchLocalPerformProcess(QStringList args);
    void launchPendingJoin();
    bool startGuiControlServer(QStringList& args);
    void stopGuiControlServer();
    void sendJamCommand(const QString& line);
    void readGuiControlSocket();
    void handleGuiControlFrame(quint16 type, const QByteArray& payload);
    void updateRuntimeControls();
    void updateMixControls();
    void setMixRemotePeerVisible(bool visible);
    void updateMixRemotePeers();
    void startJamRecording();
    void stopJamRecording();
    void updateJamRecordingControls();
    void refreshGeneratedUrl();
    QString meshInviteUrl() const;
    void showPendingMeshInviteUrl();
    void updateConnectionControlState();
    void updateTrackControls();
    void refreshLooperLanes();
    void addLooperWavs();
    void loadWavIntoLooperLane();
    void addEmptyLooperLane();
    void removeSelectedLooperLane();
    void renameSelectedLooperLane();
    void moveSelectedLooperLane(int delta);
    bool armSelectedLooperLaneRecording();
    void startArmedLooperLaneRecording();
    void startArmedLooperLaneRecordingNow(std::uint64_t targetFrame);
    void importLastCaptureToArmedLane();
    void toggleSelectedLooperLaneMute();
    void toggleSelectedLooperLaneSolo();
    void setSelectedLooperLaneGain();
    void editSelectedLooperLaneRegion();
    void applySelectedLooperLaneRegion(qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame);
    void applyLooperLaneGain(int laneIndex, double gainDb);
    void regeneratePreparedMix();
    void loadPreparedMixIntoEngine();
    void sendPreparedTrackLevel();
    void syncLooperArrangement();
    QString looperAssetAbsolutePath(const LooperLane& lane) const;
    bool materializeLooperAssets(const QString& projectFolder);
    void loadTrackMetadata();
    QString selectedDeviceId() const;
    void chooseCaptureFolder();
    void refreshLoopbackSources();
    void startInputCapture(std::uint64_t targetFrame, int countInBars = -1);
    void startLoopbackCapture();
    void stopInputCapture(std::uint64_t targetFrame);
    void loadTrackWaveform();
    void playTrack();
    void stopTrack(std::uint64_t targetFrame);
    void setLoopStartAtCurrentPosition();
    void setLoopEndAtCurrentPosition();
    void clearTrackLoop();
    qint64 currentAudibleTrackPositionMs() const;
    void updateTrackTimeline();
    void startTrackMetronome();
    void stopTrackMetronome();
    void updateTrackMetronomeInterval();
    void rebuildMetronomePattern(bool resetToDivisionDefault = false);
    jam2::metronome::PatternSnapshot currentMetronomePattern() const;
    void sendMetronomeModeToJam();
    void sendMetronomePatternToJam();
    void sendMetronomeSettingsToPeer();
    void applyRemoteMetronomeSettings(const QJsonObject& message);
    void showMetronomeCompensationDialog();
    void updateMetronomeCompensationVisibility();
    void handleLooperAssetRequest(const QJsonObject& message);
    void sendLooperAsset(const QString& hash);
    void receiveLooperAssetStart(const QJsonObject& message);
    void receiveLooperAssetChunk(const QJsonObject& message);
    void receiveLooperAssetDone(const QJsonObject& message);
    void handleSongSet(const QJsonObject& message);
    void applyPendingSongIfAssetsReady();
    QStringList missingLooperAssetHashes(const QJsonObject& song) const;
    QJsonObject normalizeLooperAssetPaths(QJsonObject song) const;
    QString looperAssetPathForHash(const QString& hash) const;
    QJsonObject trackToJson() const;
    void loadTrackJson(const QJsonObject& object);
    QJsonObject songToJson() const;
    bool loadSongJson(const QJsonObject& object);
    void newSong();
    void openSong();
    bool saveSong();
    QByteArray currentProjectSnapshot() const;
    bool hasUnsavedProjectChanges() const;
    void registerTransientTrackWav(const QString& path);
    void cleanupTransientTrackWavs();
    void refreshSongViews();
    void refreshSongView(const QString& lane);
    void updatePlaybackGrid();
    void updateRecordingCountdown(const PlaybackGrid::Position& position);
    void updateRecordingLatencyDisplay();
    void runGridLockedEngineAction(const QString& actionName, const std::function<void(std::uint64_t)>& action);
    void sendSongSnapshot();

    QStringList commonJamArgs(bool includeExtraArgs = true) const;
    void applyTuningProfileName(const QString& name);
    QString sessionHex() const;
    QString keyHex() const;
    void generateSession();

    Jam2Process jam2_;
    GuiLoopbackRecorder loopbackRecorder_;
    ControlServer controlServer_;
    ControlClient controlClient_;
    SharedTrackController trackController_;
    LooperProject looperProject_;
    PlaybackGrid playbackGrid_;
    BeatGridModel chordModel_;
    BeatGridModel beatModel_;
    BeatGridModel lyricModel_;

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
    QPushButton* metronomeCompensationButton_ = nullptr;
    QDoubleSpinBox* metronomeCompensationMaxSpin_ = nullptr;
    QDoubleSpinBox* metronomeCompensationSmoothingSpin_ = nullptr;
    QDoubleSpinBox* metronomeCompensationDeadbandSpin_ = nullptr;
    QDoubleSpinBox* metronomeCompensationSlewSpin_ = nullptr;
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
    QLabel* engineModeLabel_ = nullptr;
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
    QLabel* gridPositionLabel_ = nullptr;
    QLabel* gridScheduleLabel_ = nullptr;
    QLabel* recordingCountdownLabel_ = nullptr;
    LooperLaneStackWidget* looperStack_ = nullptr;
    WaveformWidget* trackWaveform_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLineEdit* songTitleEdit_ = nullptr;
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
    QCheckBox* metronomeMarkerReferenceCheck_ = nullptr;
    QLabel* trackMetronomeLabel_ = nullptr;
    QTableWidget* metronomePatternTable_ = nullptr;
    QPushButton* playTrackButton_ = nullptr;
    QPushButton* stopTrackButton_ = nullptr;
    QPushButton* loopStartButton_ = nullptr;
    QPushButton* loopEndButton_ = nullptr;
    QPushButton* clearLoopButton_ = nullptr;
    QCheckBox* loopEnabledCheck_ = nullptr;
    QCheckBox* trackSyncCheck_ = nullptr;
    QSlider* trackLevelSlider_ = nullptr;
    QLabel* trackLevelDbLabel_ = nullptr;
    QSlider* mixTrackLevelSlider_ = nullptr;
    QLabel* mixTrackLevelLabel_ = nullptr;
    LevelMeterWidget* mixTrackMeter_ = nullptr;
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
    QVBoxLayout* mixRemotePeerListLayout_ = nullptr;
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
    QPushButton* stopCaptureButton_ = nullptr;
    QPushButton* loadWavButton_ = nullptr;
    QPushButton* startArmedLaneRecordingButton_ = nullptr;
    std::array<QPushButton*, 4> looperBankButtons_{};
    QCheckBox* captureCountInCheck_ = nullptr;
    QCheckBox* captureCountInMetronomeCheck_ = nullptr;
    QCheckBox* captureKeepMetronomeCheck_ = nullptr;
    QSpinBox* captureCountInBarsSpin_ = nullptr;
    QLabel* recordingLatencyLabel_ = nullptr;
    QSpinBox* recordingLatencyAdjustmentSpin_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    BeatGridWidget* chordGrid_ = nullptr;
    BeatGridWidget* beatGrid_ = nullptr;
    BeatGridWidget* lyricGrid_ = nullptr;
    QTabWidget* tabs_ = nullptr;

    std::uint64_t sessionId_ = 0;
    std::array<std::uint8_t, 16> sessionKey_{};
    QString lastCapturePath_;
    QString pendingTransientCapturePath_;
    int selectedLooperLane_ = -1;
    int armedRecordBank_ = -1;
    int armedRecordLane_ = -1;
    QString armedRecordMode_;
    qint64 preparedSourceFrame_ = 0;
    qint64 preparedSourceEngineFrame_ = 0;
    bool preparedSourcePlaying_ = false;
    int preparedSourceSampleRate_ = 48000;
    bool trackTakeRecordingActive_ = false;
    QString activeTrackTakeId_;
    QString projectFolder_;
    QString projectFilePath_;
    QString transientProjectFolder_;
    QByteArray savedProjectSnapshot_;
    QSet<QString> transientTrackWavs_;
    PreparedMixResult preparedMix_;
    QString jamRecordingFolder_;
    bool jamRecordingActive_ = false;
    QJsonObject lastCaptureSummary_;
    QByteArray incomingLooperAssetBytes_;
    QString incomingLooperAssetHash_;
    qint64 incomingLooperAssetBytesExpected_ = 0;
    int incomingLooperAssetNextChunk_ = 0;
    QJsonObject pendingSongSet_;
    QStringList pendingLooperAssetHashes_;
    int pendingSongRevision_ = 0;
    bool pendingSongTrackRestart_ = false;
    int looperArrangementRevision_ = 0;
    int lastAppliedHostArrangementRevision_ = 0;
    QTimer trackTimelineTimer_;
    QTimer playbackGridTimer_;
    QTimer controlReconnectTimer_;
    QTcpServer guiControlServer_;
    QTcpSocket* guiControlSocket_ = nullptr;
    QByteArray guiControlBuffer_;
    quint32 guiControlSequence_ = 1;
    quint64 recordingScheduleRevision_ = 0;
    quint64 recordingCountdownStartFrame_ = 0;
    quint64 recordingStartFrame_ = 0;
    bool stopMetronomeAtRecordingStart_ = false;
    quint32 recordingInputLatencyFrames_ = 0;
    quint32 recordingOutputLatencyFrames_ = 0;
    quint64 recordingAppliedLatencyFrames_ = 0;
    int recordingLatencySampleRate_ = 48000;
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
    bool localEngineActive_ = false;
    bool replacingLocalEngine_ = false;
    bool returnToLocalAfterStop_ = false;
    bool shuttingDown_ = false;
    bool remotePeerConnected_ = false;
    bool meshRestarting_ = false;
    bool pendingMeshInvitePopup_ = false;
    QString meshPeerToken_;
    QMap<QString, QString> meshPeerEndpoints_;
    QStringList currentMeshPeers_;
    bool localMetronomeRunning_ = false;
    bool localMetronomeLeader_ = false;
    bool applyingRemoteMetronomeSettings_ = false;
    QVector<bool> metronomeEnabledSteps_;
    QVector<bool> metronomeAccents_;
};
