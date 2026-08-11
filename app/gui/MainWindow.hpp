#pragma once

#include "ApplicationRuntime.hpp"
#include "AssetTransferService.hpp"
#include "BeatGridWidget.hpp"
#include "GuiLoopbackRecorder.hpp"
#include "JamStorage.hpp"
#include "LooperProject.hpp"
#include "MixerStatsViewModel.hpp"
#include "MetronomeTransportController.hpp"
#include "PlaybackGrid.hpp"
#include "PerformanceWidgets.hpp"
#include "PreparedMixRenderer.hpp"
#include "ProjectPersistenceCoordinator.hpp"
#include "SharedTrackController.hpp"
#include "SharedSessionController.hpp"
#include "TrackRecordingWorkflow.hpp"
#include "TrackWorkspaceController.hpp"
#include "UserPreferences.hpp"

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
#include <QStringList>
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
class QFrame;
class QScrollArea;
class QStackedWidget;
class QTableWidget;
class QToolButton;
class QVBoxLayout;
class WaveformWidget;
class LooperLaneStackWidget;
class LevelMeterWidget;
class JamTasterService;
class JamTasterDialog;

namespace jam2::practice {
struct ChordIdeaRequest;
struct CuratedIdeaEntry;
struct GeneratedPracticeIdea;
struct ReferenceRenderSettings;
enum class PracticeIdeaParts;
}

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    friend class MainWindowPages;
    enum class GeneratedIdeaSyncMode {
        Off,
        Full,
        Chords,
        Beats,
    };
    enum class SongSyncScope {
        Tracks,
        IdeaFull,
        IdeaChords,
        IdeaBeats,
    };
    struct JamSyncPolicy {
        bool trackLanes = true;
        bool autoShareWavs = true;
        bool globalPlayback = true;
        GeneratedIdeaSyncMode generatedIdeas = GeneratedIdeaSyncMode::Full;
        bool metronomeState = false;
        bool recordings = true;
        int revision = 0;
    };
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    void startJam(bool createSession);
    void showLocalPerformSetup();
    void startLocalPerform();
    void showStartJamDialog();
    void showJoinJamDialog();
    void showSettingsDialog();
    void showJamTasterDialog(int laneIndex = -1);
    void applyJamTasterTempo(const QJsonObject& result);
    void applyJamTasterQuick(
        const QJsonObject& tempo,
        const QJsonObject& stems,
        const QJsonObject& options,
        const QString& sourceHash);
    void applyJamTasterConverted(
        const QString& convertedSong,
        const QJsonObject& options);
    void createJamTasterSong(
        const QString& convertedSong,
        const QString& sourcePath,
        const QString& sourceHash);
    void setSessionHeaderStatus(
        const QString& text,
        const QString& title,
        const QStringList& lines = {},
        bool issue = false,
        bool actionable = false);
    void showLocalSessionHeaderStatus();
    void showAudioOffSessionHeaderStatus();
    void showJamTasterSessionHeaderStatus();
    void restoreSessionHeaderStatus();
    void updateJamSessionHeaderStatus(
        const SharedSessionController::Snapshot& snapshot);
    void stopJam(bool returnToLocal = true);
    void showJamFailure(const QString& detail);
    void refreshDevices();
    void appendLog(const QString& line);
    void updateStatsDisplay(const ConnectionDiagnosticsSnapshot* stats);
    void openWorkspace(const QString& page);
    void toggleDataDrawer();
    void updatePerformancePeers();
    void selectPerformancePeer(std::uint64_t peerId);
    void applySelectedPeerGain(int db);
    void updateMixMeters(const MixerMeterLevels& levels);
    void sendControl(const QJsonObject& message);
    void requestJamSyncPolicy(JamSyncPolicy policy);
    void applyJamSyncPolicy(JamSyncPolicy policy, bool fromJam);
    void handleJamSyncMessage(
        const QJsonObject& message,
        const QString& sourcePeerToken);
    void sendJamSyncPolicy(const QString& targetPeerToken = {});
    void showJamSyncDialog();
    void updateJamSyncPresentation();
    bool jamSyncAllowsControlMessage(const QJsonObject& message) const;
    bool syncedRecordingsEnabled() const noexcept;
    bool automaticWavSharingEnabled() const noexcept;
    void setMetronomeEnabled(bool enabled, bool publishToJam);
    void sendMetronomeStateToJam(bool enabled);
    void handleControlEvent(
        const jam2::control_protocol::TransportEvent& event,
        bool serverSide);
    void notePreAuthenticationDisconnect();
    void applySessionSnapshot(const SharedSessionController::Snapshot& snapshot);
    void refreshControlConnection();
    void handleMeshPeerAuthenticated(const QString& token, const QJsonObject& message);
    QStringList meshPeerEndpointsExcludingSelf() const;
    QString meshBindEndpoint() const;
    QString localMeshEndpoint(bool createSession) const;
    QString meshPeerToken();
    bool selectedDeviceSupportsSampleRate(int sampleRate);
    void testDeviceSelection(QComboBox* device, QPushButton* button, QWidget* dialogParent);
    void applyJoinProfileName(const QString& name);
    void applyPreferencesToControls();
    void applyNewJamDefaults();
    void initializeStartupWorkflow();
    void applyCreateDefaultsToControls();
    void applyJoinDefaultsToControls();
    void saveCreateDefaults();
    void saveJoinDefaults();
    void prepareNetworkRuntimePresentation(bool createSession);
    void launchLocalRuntime(Jam2RuntimeOptions options);
    bool submitEngineCommand(jam2::EngineCommand command, const QString& context);
    void submitEngineGain(jam2::EngineCommandType type, double gain, const QString& context);
    void submitEngineToggle(jam2::EngineCommandType type, bool enabled, const QString& context);
    void submitEngineFrame(jam2::EngineCommandType type, std::uint64_t frame, const QString& context);
    void submitEngineText(jam2::EngineCommandType type, const QString& text, const QString& context);
    void seekPreparedTrack(std::uint64_t sourceFrame, std::uint64_t targetFrame);
    void setPreparedTrackLoop(bool enabled, std::uint64_t startFrame = 0, std::uint64_t endFrame = 0);
    void restartPreparedTrackQuantized();
    void handleEngineSnapshot(const jam2::EngineSnapshot& snapshot);
    void handleEngineEvent(const jam2::EngineEvent& event);
    void setTunerEnabled(bool enabled);
    void handleNetworkSnapshot(const Jam2NetworkOperationalSnapshot& snapshot);
    void handleConnectionDiagnostics(const ConnectionDiagnosticsSnapshot& snapshot);
    void updateRuntimeControls();
    void updateMixControls();
    void setMixRemotePeerVisible(bool visible);
    void updateMixRemotePeers();
    void startJamRecording();
    void stopJamRecording();
    void updateJamRecordingControls();
    void showJamRecordingFinished(const QString& folder);
    void showJamRecordingImportDialog(const QString& folder);
    QString meshInviteUrl() const;
    void showPendingMeshInviteUrl();
    void updateConnectionControlState();
    void updateTrackControls();
    void updateTrackPlaybackPresentation();
    void refreshLooperLanes();
    qint64 looperLaneTimelineEndFrame(const LooperLane& lane, int sampleRate) const;
    int requiredSectionBeatsForTracks(int bankIndex) const;
    bool extendSectionToFitTracks(int bankIndex, bool showLimitWarning = false);
    int safeTrimSectionBeats(int bankIndex) const;
    bool sectionHasUnknownTrackDuration(int bankIndex) const;
    void rebuildSectionMixAfterLengthChange(int bankIndex);
    void shrinkSectionOneBar(int bankIndex);
    void trimViewedSection();
    void updateSectionTrimControls();
    void addSongSection();
    void removeLastSongSection();
    void selectViewedBank(int bankIndex);
    void refreshBankPresentation();
    void requestBankLaunch(int bankIndex);
    void launchBank(
        int bankIndex,
        bool manualLaunch,
        std::optional<quint64> targetAbsoluteBeat = std::nullopt);
    void beginSharedBankLaunch(
        int bankIndex,
        std::optional<quint64> targetAbsoluteBeat = std::nullopt);
    void prepareSharedBankLaunch(int bankIndex, const QString& switchId);
    void noteSharedBankReady(int bankIndex);
    void handleSharedBankReady(
        int bankIndex,
        const QString& switchId,
        const QString& sourcePeerToken);
    void maybeCommitSharedBankLaunch();
    void cancelSharedBankLaunch(bool broadcast, const QString& reason = {});
    void schedulePreparedBankLaunch(
        int bankIndex,
        std::optional<quint64> targetAbsoluteBeat = std::nullopt);
    void applyScheduledBankLaunch();
    void showArrangementDialog();
    void startArrangement();
    void stopArrangement();
    void updateArrangementPlayback(const PlaybackGrid::Position& position);
    void exportLooperAudio();
    qint64 bankExactOutputFrames(int bankIndex, int sampleRate) const;
    void addLooperWavs();
    void loadWavIntoLooperLane();
    void importWavIntoLooperLane(int laneIndex, const QString& sourcePath);
    void shareLocalTracks(bool includeLocalOnly = false);
    void addEmptyLooperLane();
    void removeSelectedLooperLane();
    void revealLooperLaneWav(int laneIndex);
    void removeLooperLaneWav(int laneIndex);
    void renameSelectedLooperLane();
    void moveSelectedLooperLane(int delta);
    bool armSelectedLooperLaneRecording();
    void startArmedLooperLaneRecording();
    void startArmedLooperLaneRecordingNow(std::uint64_t targetFrame);
    void publishLocalTrackRecordingState(
        const QString& phase,
        int countInRemaining = 0,
        bool force = false);
    void handleTrackRecordingState(
        const QJsonObject& message,
        const QString& sourcePeerToken);
    void maybeStartReadyRecordingGroup();
    void handleRecordingGroupStart(const QJsonObject& message);
    void handleRecordingGroupFinish(const QJsonObject& message);
    void requestRecordingGroupRecovery();
    void handleRecordingGroupRecovery(const QJsonObject& message);
    void maybeFinishRecordingGroup();
    void finalizePendingLaneTake();
    void finishLaneTakeFinalization();
    bool laneRecordingIsolationActive() const;
    void updateLaneRecordingIsolation();
    bool deferIncomingControlForLaneRecording(
        const QJsonObject& message,
        const QString& sourcePeerToken);
    void releaseDeferredRecordingControls();
    void applyPendingRecordingTransportResync();
    void updateSharedRecordingPresentation();
    bool sharedRecordingProtected() const;
    QString recordingPeerLabel(const QString& token) const;
    void importLastCaptureToArmedLane();
    void toggleSelectedLooperLaneMute();
    void toggleSelectedLooperLaneSolo();
    void setSelectedLooperLaneGain();
    void editSelectedLooperLaneRegion();
    void applySelectedLooperLaneRegion(qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame);
    void applyLooperLaneGain(int laneIndex, double gainDb);
    void regeneratePreparedMix(int bankIndex = -1);
    void applyPreparedMixResult(PreparedMixResult result);
    void adoptPreparedBankCache(int bankIndex);
    bool startFileWorkerTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed = {});
    void loadPreparedMixIntoEngine(
        std::uint64_t targetFrame = 0,
        std::uint64_t sourceFrame = 0,
        bool alignToRunningTransport = false);
    void sendPreparedTrackLevel();
    void syncLooperArrangement();
    QString looperAssetAbsolutePath(const LooperLane& lane) const;
    bool materializeLooperAssets(const QString& projectFolder);
    void loadTrackMetadata();
    QString selectedDeviceId() const;
    void chooseCaptureFolder();
    void refreshLoopbackSources();
    void startInputCapture(std::uint64_t targetFrame, int countInBars = -1);
    void startInputCaptureAtGroupSchedule(
        std::uint64_t countdownFrame,
        std::uint64_t targetFrame,
        std::uint64_t targetMusicalFrame,
        int countInBars);
    void startLoopbackCapture();
    bool scheduleLoopbackCountIn(int bars, bool stopMetronomeAtStart);
    bool updateLoopbackCountIn(const PlaybackGrid::Position& position);
    void cancelLoopbackCountIn();
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
    void tapTrackMetronomeTempo();
    void updateTrackMetronomeInterval();
    void rebuildMetronomePattern(bool resetToDivisionDefault = false);
    jam2::metronome::PatternSnapshot currentMetronomePattern() const;
    jam2::metronome::PatternSnapshot bankMetronomePattern(int bankIndex) const;
    void storeCurrentMetronomePatternForBank(int bankIndex, bool inheritBankA = false);
    void applyMetronomePatternForBank(int bankIndex, bool transmit = true);
    void initializeLegacyBankTiming();
    int sectionBeatsPerBar(int bankIndex) const;
    bool bankGridTimingDiffers(int bankIndex) const;
    void sendMetronomeModeToJam();
    void sendMetronomeSoundToJam();
    void sendMetronomePatternToJam();
    void updateMetronomePresentationFromEngine(const jam2::EngineSnapshot& snapshot);
    void showMetronomeCompensationDialog();
    void updateMetronomeCompensationVisibility();
    void publishLocalTrackBatch(const QString& batchId);
    void handleTrackBatchOffer(const QJsonObject& message, const QString& sourcePeerToken);
    void handleTrackBatchComplete(const QJsonObject& message, const QString& sourcePeerToken);
    void expirePendingTrackBatch(const QString& sourcePeerToken, const QString& batchId);
    void scheduleOutgoingTrackBatchExpiry(const QString& batchId);
    void scheduleIncomingTrackBatchExpiry(
        const QString& sourcePeerToken,
        const QString& batchId);
    void noteTrackAssetProgress(
        const QString& hash,
        const QString& peerToken,
        bool receiving);
    void retryOrFailIncomingAsset(const QString& hash);
    void releaseHeldTrackSnapshotIfReady();
    void requestNextPendingAsset();
    void applyPendingTrackContributions();
    bool sendControlTo(const QString& targetPeerToken, const QJsonObject& message);
    bool sendAssetControlTo(const QString& targetPeerToken, const QJsonObject& message);
    bool sendAssetBinaryTo(const QString& targetPeerToken, const QByteArray& payload);
    bool canQueueAssetTo(const QString& targetPeerToken, qint64 estimatedBytes) const;
    void handleSongSet(const QJsonObject& message, const QString& sourcePeerToken);
    void applyPendingSongIfAssetsReady();
    QJsonObject normalizeLooperAssetPaths(QJsonObject song) const;
    QJsonObject preserveQuarantinedLocalLanes(QJsonObject song);
    QString looperAssetPathForHash(const QString& hash) const;
    QJsonObject trackToJson() const;
    void loadTrackJson(const QJsonObject& object);
    QJsonObject songToJson(bool syncCompatibleOnly = false) const;
    void auditWavCompatibilityForSession(int expectedSampleRate, bool showModal);
    bool loadSongJson(const QJsonObject& object);
    void newSong();
    void openSong();
    bool saveSong();
    QByteArray currentProjectSnapshot() const;
    bool hasUnsavedProjectChanges() const;
    void registerTransientTrackWav(const QString& path);
    bool looperAssetPathIsReferenced(const QString& path) const;
    void discardObsoleteReferenceWavs(const QSet<QString>& paths);
    void retryObsoleteReferenceWavs();
    void discardPreparedMix(bool replacementExpected);
    bool clearPracticeReferenceWavs(bool rebuildRemainingTracks = false, int bankIndex = -1);
    void cleanupTransientTrackWavs();
    QString jamAssetFolder(JamStorage::AssetKind kind) const;
    bool renameCurrentJam(const QString& displayName);
    void relocateManagedPaths(const QString& oldRoot, const QString& newRoot);
    void refreshSongViews();
    void refreshSongView(const QString& lane);
    void generatePracticeIdea();
    void browseCuratedIdeas();
    void continuePracticeIdea();
    void clearPracticeIdea();
    bool applyPracticeIdea(const jam2::practice::ChordIdeaRequest& request);
    bool applyPracticeIdea(
        jam2::practice::GeneratedPracticeIdea idea,
        jam2::practice::PracticeIdeaParts parts,
        int targetSectionIndex,
        bool useIdeaTiming,
        bool matchIdeaLength);
    bool playCuratedIdeaPreview(
        const jam2::practice::CuratedIdeaEntry& idea,
        QString& error);
    void stopCuratedIdeaPreview();
    void stopTrackForPracticeIdeaGeneration();
    void generatePracticeReferenceWavs();
    void startPracticeReferenceWavGeneration(
        const jam2::practice::ReferenceRenderSettings& settings,
        const QString& requestId);
    void handlePracticeReferenceRenderRequest(
        const QJsonObject& message,
        const QString& sourcePeerToken,
        bool localRequest = false);
    QByteArray practiceReferenceRenderSignature() const;
    void startDeferredPracticeReferenceRenders();
    void showPracticeIdeaDetails();
    void updatePlaybackGrid();
    void updateRecordingCountdown(const PlaybackGrid::Position& position);
    void updateRecordingLatencyDisplay();
    void runGridLockedEngineAction(
        const QString& actionName,
        const std::function<void(std::uint64_t)>& action,
        bool quantizeToBar = false);
    void sendSongSnapshot(
        std::optional<bool> trackPlayingOverride = std::nullopt,
        SongSyncScope scope = SongSyncScope::Tracks);

    Jam2RuntimeOptions runtimeOptions() const;
    Jam2RuntimeOptions networkRuntimeOptions(
        const SharedSessionController::Snapshot& snapshot) const;
    int activeTrackSampleRate() const;
    bool recordingTargetSampleRate(int& sampleRate, QString& error) const;
    void applyTuningProfileName(const QString& name);
    QString sessionHex() const;
    QString keyHex() const;
    void generateSession();

    ApplicationRuntime jam2_;
    UserPreferences preferences_;
    bool preferencesInitialized_ = false;
    std::vector<jam2::audio::DeviceInfo> availableDevices_;
    QMap<QString, jam2::audio::DeviceTestResult> deviceCapabilitiesCache_;
    QString joinProfileName_ = QStringLiteral("fast");
    GuiLoopbackRecorder loopbackRecorder_;
    SharedSessionController sessionController_;
    TrackWorkspaceController trackWorkspace_;
    SharedTrackController& trackController_;
    MixerStatsViewModel mixerStatsViewModel_;
    LooperProject& looperProject_;
    AssetTransferService& assetTransfer_;
    MetronomeTransportController metronomeTransport_;
    TrackRecordingWorkflow& trackRecordingWorkflow_;
    BeatGridModel& chordModel_;
    BeatGridModel& beatModel_;
    BeatGridModel& lyricModel_;

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
    QComboBox* networkAudioFormatBox_ = nullptr;
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
    QComboBox* metronomeSoundBox_ = nullptr;
    QSlider* metronomeLevelSlider_ = nullptr;
    QSlider* remoteLevelSlider_ = nullptr;
    QSlider* masterOutputLevelSlider_ = nullptr;
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
    QSpinBox* adaptiveRatioRampSpin_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QToolButton* jamSyncButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* leaveJamButton_ = nullptr;
    QPushButton* refreshControlButton_ = nullptr;
    QString sessionHeaderRtt_;
    QLabel* connectionLabel_ = nullptr;
    QLabel* jitterLabel_ = nullptr;
    QLabel* lossLabel_ = nullptr;
    QLabel* underrunLabel_ = nullptr;
    QLabel* latencyLabel_ = nullptr;
    QLabel* diagnosisLabel_ = nullptr;
    QLabel* diagnosisEvidenceLabel_ = nullptr;
    QLabel* rendererStatsLabel_ = nullptr;
    QLabel* trackNameLabel_ = nullptr;
    QLabel* gridPositionLabel_ = nullptr;
    QLabel* gridScheduleLabel_ = nullptr;
    QLabel* recordingCountdownLabel_ = nullptr;
    QFrame* recordingContextFrame_ = nullptr;
    QLabel* recordingContextTitle_ = nullptr;
    QLabel* recordingContextDetail_ = nullptr;
    QLabel* recordingPeerStatesLabel_ = nullptr;
    QPushButton* recoverRecordingGroupButton_ = nullptr;
    QLabel* trackSharingStatusLabel_ = nullptr;
    QWidget* recordingGlobalControls_ = nullptr;
    LooperLaneStackWidget* looperStack_ = nullptr;
    WaveformWidget* trackWaveform_ = nullptr;
    QLineEdit* songTitleEdit_ = nullptr;
    QLineEdit* captureOutputEdit_ = nullptr;
    QComboBox* loopbackSourceBox_ = nullptr;
    QCheckBox* captureManualStopCheck_ = nullptr;
    QSpinBox* captureDurationSpin_ = nullptr;
    QCheckBox* trimLeadingCheck_ = nullptr;
    QCheckBox* trimTrailingCheck_ = nullptr;
    QDoubleSpinBox* silenceThresholdSpin_ = nullptr;
    QSpinBox* tailSilenceSpin_ = nullptr;
    QDoubleSpinBox* trackSpeedSpin_ = nullptr;
    QSpinBox* trackPitchSpin_ = nullptr;
    QSlider* trackSpeedSlider_ = nullptr;
    QSlider* trackPitchSlider_ = nullptr;
    QSpinBox* metronomeBpmSpin_ = nullptr;
    QComboBox* metronomeBeatsSpin_ = nullptr;
    QComboBox* metronomeBeatUnitBox_ = nullptr;
    QComboBox* metronomeTempoPulseBox_ = nullptr;
    QComboBox* metronomeDivisionBox_ = nullptr;
    QSlider* localMetronomeLevelSlider_ = nullptr;
    QPushButton* startTrackMetronomeButton_ = nullptr;
    QPushButton* stopTrackMetronomeButton_ = nullptr;
    QPushButton* tapTrackMetronomeButton_ = nullptr;
    MetronomeNebulaWidget* metronomeNebula_ = nullptr;
    MetronomePatternWidget* metronomePatternWidget_ = nullptr;
    QLabel* metronomeMeterReadout_ = nullptr;
    QLabel* metronomeIntervalReadout_ = nullptr;
    QLabel* metronomeModeDescription_ = nullptr;
    QPushButton* playTrackButton_ = nullptr;
    QPushButton* stopTrackButton_ = nullptr;
    QPushButton* loopStartButton_ = nullptr;
    QPushButton* loopEndButton_ = nullptr;
    QPushButton* clearLoopButton_ = nullptr;
    QCheckBox* loopEnabledCheck_ = nullptr;
    QCheckBox* trackGridLockCheck_ = nullptr;
    QSlider* trackLevelSlider_ = nullptr;
    QLabel* trackLevelDbLabel_ = nullptr;
    QSlider* mixTrackLevelSlider_ = nullptr;
    QLabel* mixTrackLevelLabel_ = nullptr;
    LevelMeterWidget* mixTrackMeter_ = nullptr;
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
    QLabel* masterOutputLevelLabel_ = nullptr;
    QWidget* mixRemotePeerRow_ = nullptr;
    QVBoxLayout* mixRemotePeerListLayout_ = nullptr;
    QSlider* mixRemotePeerSlider_ = nullptr;
    QLabel* mixRemotePeerLevelLabel_ = nullptr;
    LevelMeterWidget* mixRemotePeerMeter_ = nullptr;
    QLabel* mixOutputClipLabel_ = nullptr;
    QCheckBox* focusFrequencyCheck_ = nullptr;
    QComboBox* focusPresetBox_ = nullptr;
    QSlider* focusFrequencySlider_ = nullptr;
    QSpinBox* focusFrequencySpin_ = nullptr;
    QPushButton* stopCaptureButton_ = nullptr;
    QPushButton* loadWavButton_ = nullptr;
    QPushButton* shareTracksButton_ = nullptr;
    QPushButton* startArmedLaneRecordingButton_ = nullptr;
    std::array<QPushButton*, 12> looperBankButtons_{};
    QVector<QPushButton*> bankViewButtons_;
    QVector<QPushButton*> sectionAddButtons_;
    QVector<QPushButton*> sectionRemoveButtons_;
    QVector<QPushButton*> sectionTrimButtons_;
    QPushButton* arrangementButton_ = nullptr;
    QPushButton* launchBankButton_ = nullptr;
    QCheckBox* captureCountInCheck_ = nullptr;
    QCheckBox* captureCountInMetronomeCheck_ = nullptr;
    QCheckBox* captureKeepMetronomeCheck_ = nullptr;
    QSpinBox* captureCountInBarsSpin_ = nullptr;
    QLabel* recordingLatencyLabel_ = nullptr;
    QSpinBox* recordingLatencyAdjustmentSpin_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    std::unique_ptr<QFile> guiLogFile_;
    QString guiLogPath_;
    BeatGridWidget* chordGrid_ = nullptr;
    BeatGridWidget* beatGrid_ = nullptr;
    BeatGridWidget* lyricGrid_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QStackedWidget* performanceStageStack_ = nullptr;
    QStackedWidget* workspaceStack_ = nullptr;
    QMap<QString, int> workspacePages_;
    PerformanceHomeWidget* performanceHome_ = nullptr;
    QWidget* dataOverlay_ = nullptr;
    QFrame* dataDrawer_ = nullptr;
    QPushButton* performanceTrackToggle_ = nullptr;
    QPushButton* performanceMetronomeToggle_ = nullptr;
    QPushButton* performanceTempoButton_ = nullptr;
    QLabel* performancePositionLabel_ = nullptr;
    QLineEdit* detailPositionLabel_ = nullptr;
    QSlider* selectedPeerGainSlider_ = nullptr;
    QLabel* selectedPeerNameLabel_ = nullptr;
    QLabel* selectedPeerGainLabel_ = nullptr;
    QLabel* performanceLeftTitle_ = nullptr;
    QLabel* performanceRightTitle_ = nullptr;
    QWidget* performanceLocalControls_ = nullptr;
    QWidget* performancePeerControls_ = nullptr;
    QWidget* performanceMasterOutputControls_ = nullptr;
    QWidget* detailIdentityPanel_ = nullptr;
    QWidget* peerGainListContent_ = nullptr;
    QVBoxLayout* peerGainListLayout_ = nullptr;
    QScrollArea* peerGainScroll_ = nullptr;
    QLabel* diagnosticSampleRateValue_ = nullptr;
    QLabel* diagnosticDriftValue_ = nullptr;
    QLabel* diagnosticMissingAudioValue_ = nullptr;
    QLabel* diagnosticOutputUnderrunsValue_ = nullptr;
    QLabel* diagnosticPacketsValue_ = nullptr;
    QLabel* diagnosticLateValue_ = nullptr;
    QLabel* diagnosticLossEventsValue_ = nullptr;
    QLabel* diagnosticBurstGapsValue_ = nullptr;
    QTableWidget* diagnosticPeerTable_ = nullptr;

    std::uint64_t sessionId_ = 0;
    std::array<std::uint8_t, 16> sessionKey_{};
    TapTempoTracker tapTempoTracker_;
    QElapsedTimer tapTempoClock_;
    QElapsedTimer loopbackRecordingPreviewClock_;
    std::uint64_t loopbackCountdownStartFrame_ = 0;
    std::uint64_t loopbackRecordingStartFrame_ = 0;
    bool stopMetronomeAtLoopbackStart_ = false;
    int selectedLooperLane_ = -1;
    struct PeerTrackRecordingState {
        QString phase;
        int bank = 0;
        QString laneId;
        QString laneName;
        int countInRemaining = 0;
        int countInBars = 0;
        QString groupId;
        int revision = 0;
    };
    QMap<QString, PeerTrackRecordingState> peerTrackRecordingStates_;
    QMap<QString, int> peerTrackRecordingRevisions_;
    QString localTrackRecordingPhase_ = QStringLiteral("idle");
    int localTrackRecordingCountInRemaining_ = 0;
    int localTrackRecordingCountInBars_ = 0;
    int localTrackRecordingStateRevision_ = 0;
    QString localRecordingTargetBankId_;
    QString localRecordingTargetLaneId_;
    QString activeRecordingGroupId_;
    QStringList activeRecordingGroupParticipants_;
    QSet<QString> recoveredRecordingGroupIds_;
    QJsonObject activeRecordingGroupStartMessage_;
    QJsonObject lastRecordingGroupFinishMessage_;
    bool recordingGroupFinishSent_ = false;
    bool localLaneTakeForcedLocal_ = false;
    QString pendingRecordedLaneSyncId_;
    QString pendingRecordedLaneSyncHash_;
    int pendingRecordedLaneSyncAttempts_ = 0;
    std::optional<TrackRecordingWorkflow::TrackTakeCompletion>
        pendingGroupTakeCompletion_;
    struct DeferredRecordingControl {
        QString key;
        QJsonObject message;
        QString sourcePeerToken;
    };
    QVector<DeferredRecordingControl> deferredRecordingControls_;
    bool deferredRecordingControlsOverflowed_ = false;
    bool replayingDeferredRecordingControls_ = false;
    std::optional<bool> pendingRecordingResyncPlaying_;
    JamSyncPolicy jamSyncPolicy_;
    bool applyingJamSyncPolicy_ = false;
    bool applyingSharedMetronomeState_ = false;
    int viewedBankIndex_ = 0;
    int pendingBankIndex_ = -1;
    quint64 pendingBankAbsoluteBeat_ = 0;
    QString sharedBankSwitchId_;
    int sharedBankSwitchIndex_ = -1;
    quint64 sharedBankTargetAbsoluteBeat_ = 0;
    bool sharedBankHostReady_ = false;
    QSet<QString> sharedBankReadyTokens_;
    std::optional<quint64> pendingBankRequestedTargetBeat_;
    bool arrangementRunning_ = false;
    bool arrangementArmed_ = false;
    bool arrangementResetBankAfterStop_ = false;
    int arrangementStepIndex_ = 0;
    int arrangementStepRepeat_ = 0;
    quint64 arrangementSectionStartBeat_ = 0;
    std::array<PreparedMixResult, 12> preparedMixByBank_{};
    bool referenceWavGenerationRunning_ = false;
    QSet<QString> handledReferenceRenderRequests_;
    QMap<QString, QPair<QJsonObject, QString>> deferredReferenceRenderRequests_;
    ProjectPersistenceCoordinator& projectPersistence_;
    PreparedMixResult& preparedMix_;
    QThreadPool& fileWorkerPool_;
    bool& preparedMixWorkerRunning_;
    bool& preparedMixRerunPending_;
    int preparedMixRerunBank_ = -1;
    std::uint64_t preparedMixRevision_ = 0;
    bool& playPreparedMixWhenReady_;
    bool& publishStoppedTrackStateWhenApplied_;
    std::uint64_t& preparedMixRequests_;
    std::uint64_t& preparedMixCoalesced_;
    std::uint64_t& preparedMixFailures_;
    int& fileWorkerTasksActive_;
    int& fileWorkerTasksHighWater_;
    std::uint64_t& fileWorkerTasksCompleted_;
    std::uint64_t& fileWorkerTasksRejected_;
    bool& trackWaveformWorkerRunning_;
    std::uint64_t& trackWaveformRevision_;
    using LooperWaveformPreview = TrackWorkspaceController::LooperWaveformPreview;
    QMap<QString, LooperWaveformPreview>& looperWaveformCache_;
    QSet<QString> obsoletePreparedMixPaths_;
    QSet<QString> pendingObsoleteReferencePaths_;
    bool& looperWaveformWorkerRunning_;
    bool& wavCompatibilityAuditRunning_;
    int& pendingWavCompatibilityAuditRate_;
    QSet<QString>& reportedIncompatibleWavs_;
    QString lastWavCompatibilityAuditSignature_;
    std::uint64_t wavCompatibilityAuditGeneration_ = 0;
    using PendingTrackContribution = TrackWorkspaceController::PendingTrackContribution;
    QMap<QString, PendingTrackContribution>& pendingTrackContributions_;
    QSet<QString>& appliedTrackContributionIds_;
    QMap<QString, QJsonObject>& localTrackOffers_;
    QMap<QString, QString>& trackOfferAssetPaths_;
    QMap<QString, QString>& pendingTrackAssetSources_;
    QSet<QString>& validatedTrackAssetHashes_;
    using IncomingAssetWorkflow = TrackWorkspaceController::IncomingAssetWorkflow;
    IncomingAssetWorkflow& incomingAssetWorkflow_;
    QString& incomingAssetHash_;
    QString& incomingAssetSourcePeerToken_;
    QJsonObject& pendingSongSet_;
    QStringList& pendingLooperAssetHashes_;
    int& pendingSongRevision_;
    bool& pendingSongTrackRestart_;
    QString& pendingSongSourcePeerToken_;
    bool& pendingSongNeedsAuthoritativePublish_;
    std::uint64_t& songAssetCheckRevision_;
    QJsonObject& deferredSongSetMessage_;
    QString& deferredSongSetSourcePeerToken_;
    QTimer songAssetCheckRetryTimer_;
    int& looperArrangementRevision_;
    int& lastAppliedHostArrangementRevision_;
    QTimer trackTimelineTimer_;
    QTimer playbackGridTimer_;
    bool shuttingDown_ = false;
    bool pendingMeshInvitePopup_ = false;
    bool jamStartupPending_ = false;
    bool jamStartupCreating_ = false;
    QString pendingJamRuntimeError_;
    QString lastJamFailureDialog_;
    QString queuedJamFailureDetail_;
    bool jamFailureDialogQueued_ = false;
    QElapsedTimer preAuthenticationDisconnectWindow_;
    int preAuthenticationDisconnectCount_ = 0;
    bool firewallGuidanceShown_ = false;
    bool controlRefreshAvailable_ = false;
    QString activePublicEndpoint_;
    QString meshPeerToken_;
    QString lastLoggedSessionSummary_;
    QVector<Jam2OperationalPeer> operationalPeers_;
    QMap<std::uint64_t, int> peerOrdinals_;
    QMap<std::uint64_t, double> desiredPeerGainDb_;
    QMap<std::uint64_t, QSlider*> peerGainSliders_;
    QMap<std::uint64_t, QLabel*> peerGainValueLabels_;
    QString peerMembershipSignature_;
    std::uint64_t selectedPeerId_ = 0;
    int nextPeerOrdinal_ = 1;
    std::optional<ConnectionDiagnosticsSnapshot> lastDiagnostics_;
    QSet<QString> localMeshPeerTokens_;
    QMap<QString, QString> meshPeerEndpoints_;
    std::uint64_t engineCommandCookie_ = 0;
    std::uint64_t tunerCommandCookie_ = 0;
    bool tunerRequestedEnabled_ = true;
    std::uint64_t practiceIdeaRevision_ = 0;
    bool curatedIdeaPreviewActive_ = false;
    QVector<bool> metronomeEnabledSteps_;
    QVector<bool> metronomeAccents_;
    bool applyingBankTiming_ = false;
    std::unique_ptr<JamTasterService> jamTaster_;
    std::unique_ptr<JamTasterDialog> jamTasterDialog_;
    quint64 jamTasterStatusRevision_ = 0;
    JamStorage jamStorage_;
};
