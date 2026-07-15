#pragma once

#include "ApplicationRuntime.hpp"
#include "BeatGridWidget.hpp"
#include "GuiLoopbackRecorder.hpp"
#include "LooperProject.hpp"
#include "PlaybackGrid.hpp"
#include "PreparedMixRenderer.hpp"
#include "SharedTrackController.hpp"
#include "SharedSessionController.hpp"

#include "metronome.hpp"

#include <QCheckBox>
#include <QByteArray>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QThreadPool>
#include <QVector>
#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QFile;
class QCryptographicHash;
class QTemporaryFile;

class QEvent;
class QCloseEvent;
class QGroupBox;
class QTableWidget;
class QVBoxLayout;
class WaveformWidget;
class LooperLaneStackWidget;
class LevelMeterWidget;

QJsonObject jam2RunBoundaryValidation(const QStringList& fixtureSpecs);

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

    void startJam(bool createSession);
    void showLocalPerformSetup();
    void startLocalPerform();
    void showStartJamDialog();
    void showJoinJamDialog();
    void stopJam(bool returnToLocal = true);
    void refreshDevices();
    void appendLog(const QString& line);
    void updateStatsDisplay(const QJsonObject& stats);
    void updateMixMeters(const QJsonObject& stats);
    void handleControlMessage(const QJsonObject& message, const QString& sourcePeerToken = QString());
    void sendControl(const QJsonObject& message);
    void handleControlEvent(
        const jam2::control_protocol::TransportEvent& event,
        bool serverSide);
    void applySessionSnapshot(const SharedSessionController::Snapshot& snapshot);
    void refreshControlConnection();
    void handleMeshPeerAuthenticated(const QString& token, const QJsonObject& message);
    QStringList meshPeerEndpointsExcludingSelf() const;
    quint64 meshPeerIdForToken(const QString& token) const;
    QString meshBindEndpoint() const;
    QString localMeshEndpoint(bool createSession) const;
    QString meshPeerToken();
    bool selectedDeviceSupportsSampleRate(int sampleRate);
    void prepareNetworkRuntimePresentation(bool createSession);
    void launchLocalRuntime(Jam2RuntimeOptions options);
    bool submitEngineCommand(jam2::EngineCommand command, const QString& context);
    void submitEngineGain(jam2::EngineCommandType type, double gain, const QString& context);
    void submitEngineToggle(jam2::EngineCommandType type, bool enabled, const QString& context);
    void submitEngineFrame(jam2::EngineCommandType type, std::uint64_t frame, const QString& context);
    void submitEngineText(jam2::EngineCommandType type, const QString& text, const QString& context);
    void seekPreparedTrack(std::uint64_t sourceFrame, std::uint64_t targetFrame);
    void setPreparedTrackLoop(bool enabled, std::uint64_t startFrame = 0, std::uint64_t endFrame = 0);
    std::uint64_t quantizedEngineTarget(int countInBars) const;
    void restartPreparedTrackQuantized();
    void armTrackTake(const QString& id, const QString& output);
    void startTrackTake(std::uint64_t targetFrame);
    void startTrackTakeQuantized(int countInBars);
    void stopTrackTake(std::uint64_t targetFrame);
    void handleEngineSnapshot(const jam2::EngineSnapshot& snapshot);
    void handleEngineEvent(const jam2::EngineEvent& event);
    void handleNetworkSnapshot(const jam2::NetworkSessionSnapshot& snapshot);
    void updateRuntimeControls();
    void updateMixControls();
    void setMixRemotePeerVisible(bool visible);
    void updateMixRemotePeers();
    void startJamRecording();
    void stopJamRecording();
    void updateJamRecordingControls();
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
    void applyPreparedMixResult(PreparedMixResult result);
    bool startFileWorkerTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed = {});
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
    void publishLocalRecordingOffer(int bankIndex, const QString& targetLaneId, const LooperLane& lane);
    void handleRecordingOffer(const QJsonObject& message, const QString& sourcePeerToken);
    void applyPendingRecordingContributions();
    void handleLooperAssetRequest(const QJsonObject& message, const QString& sourcePeerToken);
    void sendLooperAsset(const QString& hash, const QString& targetPeerToken);
    void continueLooperAssetSend();
    void resetIncomingLooperAsset();
    bool sendControlTo(const QString& targetPeerToken, const QJsonObject& message);
    bool canQueueControlTo(const QString& targetPeerToken, qint64 estimatedBytes) const;
    void receiveLooperAssetStart(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveLooperAssetChunk(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveLooperAssetDone(const QJsonObject& message, const QString& sourcePeerToken);
    void handleSongSet(const QJsonObject& message);
    void handleTrackReady(const QJsonObject& message, const QString& sourcePeerToken);
    void maybeScheduleSharedTrackRestart();
    void applyPendingSongIfAssetsReady();
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
    void runGridLockedEngineAction(
        const QString& actionName,
        const std::function<void(std::uint64_t)>& action,
        bool quantizeToBar = false);
    void sendSongSnapshot(std::optional<bool> trackPlayingOverride = std::nullopt);

    Jam2RuntimeOptions runtimeOptions() const;
    Jam2RuntimeOptions networkRuntimeOptions(
        const SharedSessionController::Snapshot& snapshot) const;
    void applyTuningProfileName(const QString& name);
    QString sessionHex() const;
    QString keyHex() const;
    void generateSession();

    ApplicationRuntime jam2_;
    GuiLoopbackRecorder loopbackRecorder_;
    SharedSessionController sessionController_;
    SharedTrackController trackController_;
    LooperProject looperProject_;
    PlaybackGrid playbackGrid_;
    BeatGridModel chordModel_;
    BeatGridModel beatModel_;
    BeatGridModel lyricModel_;

    QLineEdit* bindHostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* publicHostEdit_ = nullptr;
    QLineEdit* connectUrlEdit_ = nullptr;
    QLineEdit* stunServerEdit_ = nullptr;
    QSpinBox* stunTimeoutSpin_ = nullptr;
    QSpinBox* stunRetriesSpin_ = nullptr;
    QSpinBox* waitMsSpin_ = nullptr;
    QSpinBox* streamMsSpin_ = nullptr;
    QSpinBox* streamLingerMsSpin_ = nullptr;
    QCheckBox* statsCheck_ = nullptr;
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
    QPushButton* startButton_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* refreshControlButton_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* engineModeLabel_ = nullptr;
    QLabel* sessionTopologyLabel_ = nullptr;
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
    QSet<QString> deferredTransientCleanupWavs_;
    PreparedMixResult preparedMix_;
    QThreadPool fileWorkerPool_;
    bool preparedMixWorkerRunning_ = false;
    bool preparedMixRerunPending_ = false;
    bool playPreparedMixWhenReady_ = false;
    int pendingPreparedTrackReadyRevision_ = 0;
    quint64 pendingSharedTrackRevision_ = 0;
    bool pendingSharedTrackHostReady_ = true;
    QSet<QString> pendingSharedTrackReadyTokens_;
    bool publishStoppedTrackStateWhenApplied_ = false;
    std::uint64_t preparedMixRequests_ = 0;
    std::uint64_t preparedMixCoalesced_ = 0;
    std::uint64_t preparedMixFailures_ = 0;
    int fileWorkerTasksActive_ = 0;
    int fileWorkerTasksHighWater_ = 0;
    std::uint64_t fileWorkerTasksCompleted_ = 0;
    std::uint64_t fileWorkerTasksRejected_ = 0;
    bool trackWaveformWorkerRunning_ = false;
    std::uint64_t trackWaveformRevision_ = 0;
    struct LooperWaveformPreview {
        std::vector<float> peaks;
        qint64 sourceFrames = 0;
        bool valid = false;
    };
    QMap<QString, LooperWaveformPreview> looperWaveformCache_;
    bool looperWaveformWorkerRunning_ = false;
    QString jamRecordingFolder_;
    bool jamRecordingActive_ = false;
    QJsonObject lastCaptureSummary_;
    struct PendingRecordingContribution {
        QString sourcePeerToken;
        int bankIndex = 0;
        QString targetLaneId;
        QString assetHash;
        QString name;
    };
    QMap<QString, PendingRecordingContribution> pendingRecordingContributions_;
    QSet<QString> appliedRecordingContributionIds_;
    QMap<QString, QJsonObject> localRecordingOffers_;
    QMap<QString, QString> recordingOfferAssetPaths_;
    QMap<QString, QString> pendingRecordingAssetSources_;
    QSet<QString> validatedRecordingAssetHashes_;
    std::unique_ptr<QTemporaryFile> incomingLooperAssetFile_;
    std::unique_ptr<QCryptographicHash> incomingLooperAssetHasher_;
    QString incomingLooperAssetHash_;
    QString incomingLooperAssetSourceToken_;
    qint64 incomingLooperAssetBytesExpected_ = 0;
    qint64 incomingLooperAssetBytesReceived_ = 0;
    int incomingLooperAssetChunkSize_ = 0;
    int incomingLooperAssetNextChunk_ = 0;
    QTimer incomingLooperAssetTimer_;
    QList<QPair<QString, QString>> outgoingLooperAssetQueue_;
    std::unique_ptr<QFile> outgoingLooperAssetFile_;
    QString outgoingLooperAssetHash_;
    QString outgoingLooperAssetTargetToken_;
    QString outgoingLooperAssetPendingHash_;
    QString outgoingLooperAssetPendingTargetToken_;
    bool outgoingLooperAssetValidationPending_ = false;
    qint64 outgoingLooperAssetBytes_ = 0;
    int outgoingLooperAssetNextChunk_ = 0;
    QElapsedTimer outgoingLooperAssetProgress_;
    QTimer outgoingLooperAssetTimer_;
    QJsonObject pendingSongSet_;
    QStringList pendingLooperAssetHashes_;
    int pendingSongRevision_ = 0;
    bool pendingSongTrackRestart_ = false;
    std::uint64_t songAssetCheckRevision_ = 0;
    QJsonObject deferredSongSetMessage_;
    QTimer songAssetCheckRetryTimer_;
    int looperArrangementRevision_ = 0;
    int lastAppliedHostArrangementRevision_ = 0;
    QTimer trackTimelineTimer_;
    QTimer playbackGridTimer_;
    quint64 recordingScheduleRevision_ = 0;
    quint64 recordingCountdownStartFrame_ = 0;
    quint64 recordingStartFrame_ = 0;
    int pendingCountInBars_ = 0;
    bool stopMetronomeAtRecordingStart_ = false;
    quint32 recordingInputLatencyFrames_ = 0;
    quint32 recordingOutputLatencyFrames_ = 0;
    quint64 recordingAppliedLatencyFrames_ = 0;
    int recordingLatencySampleRate_ = 48000;
    bool shuttingDown_ = false;
    bool pendingMeshInvitePopup_ = false;
    QString activePublicEndpoint_;
    QString meshPeerToken_;
    QString lastLoggedSessionSummary_;
    QSet<QString> localMeshPeerTokens_;
    QMap<QString, QString> meshPeerEndpoints_;
    bool localMetronomeRunning_ = false;
    bool localMetronomeLeader_ = false;
    bool applyingRemoteMetronomeSettings_ = false;
    std::uint64_t engineCommandCookie_ = 0;
    QVector<bool> metronomeEnabledSteps_;
    QVector<bool> metronomeAccents_;
};
