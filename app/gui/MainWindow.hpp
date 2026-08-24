#pragma once

#include "ApplicationRuntime.hpp"
#include "JamSyncPolicy.hpp"
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
#include "SharedBankLaunchCoordinator.hpp"
#include "SharedTrackController.hpp"
#include "SharedSessionController.hpp"
#include "SessionStartupDialogs.hpp"
#include "TrackRecordingWorkflow.hpp"
#include "TrackWorkspaceController.hpp"
#include "UserPreferences.hpp"
#include "InputPluginBackend.hpp"

#include "metronome.hpp"
#include "midi.hpp"

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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
class GuiTestAgent;

namespace jam2::practice {
struct ChordIdeaRequest;
struct CuratedIdeaEntry;
struct GeneratedPracticeIdea;
struct ReferenceRenderSettings;
enum class PracticeIdeaParts;
}

namespace jam2::application {
class MidiInputBackend;
}

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    MainWindow(
        QWidget* parent,
        std::unique_ptr<jam2::application::MidiInputBackend> midiInputBackend);
    MainWindow(
        QWidget* parent,
        std::unique_ptr<jam2::application::MidiInputBackend> midiInputBackend,
        std::unique_ptr<jam2::application::InputPluginBackend> inputPluginBackend);
    ~MainWindow() override;

private:
    friend class MainWindowPages;
    friend class GuiTestAgent;
    enum class SongSyncScope {
        Tracks,
        IdeaFull,
        IdeaChords,
        IdeaBeats,
    };
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    void startJam(bool createSession);
    void resetTrackSyncSessionState();
    bool startAutomationJam(
        bool createSession,
        int localPort,
        const QString& inviteUrl,
        Jam2TestInputMode testInput,
        QString& error);
    bool prepareAutomationDialogJam(
        Jam2TestInputMode testInput,
        QString& error);
    QJsonObject automationJamSnapshot() const;
    QJsonObject automationContentSnapshot() const;
    QJsonObject automationPerformanceSnapshot() const;
    bool automationSetMetronome(bool enabled, int bpm, QString& error);
    bool automationSetGlobalPlayback(bool playing, QString& error);
    bool automationSetJamRecording(bool recording, QString& error);
    bool automationRenameSong(const QString& title, QString& error);
    bool automationEditSongCell(
        int section,
        const QString& lane,
        int beat,
        const QString& value,
        QString& error);
    bool automationResizeSongSection(int section, int beats, QString& error);
    bool automationGenerateIdea(
        jam2::practice::PracticeIdeaParts parts,
        std::uint32_t seed,
        QString& error);
    bool automationImportWav(
        int laneIndex,
        const QString& sourcePath,
        QString& error);
    bool automationShareTracks(QString& error);
    bool automationArmTransferPause(
        const QString& point, QString& error);
    bool automationReleaseTransferPause(QString& error);
    bool automationDropOutgoingAssetStarts(int count, QString& error);
    bool automationExpireAssetRequestStart(QString& error);
    bool automationHoldFileWorkers(QString& error);
    bool automationReleaseFileWorkers(QString& error);
    bool automationArmCompletionGate(const QString& target, QString& error);
    bool automationReleaseCompletionGate(const QString& target, QString& error);
    QJsonObject automationTransferSnapshot() const;
    void clearAutomationTransferPause();
    void showLocalPerformSetup();
    void startLocalPerform();
    void showStartJamDialog();
    void showJoinJamDialog();
    void showSettingsDialog();
    bool applyLocalAudioSettings(
        const AudioDevicePreference& desired,
        const QString& selectedDeviceId,
        QWidget* parent);
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
    bool leaderAudioModeActive() const noexcept;
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
    QString meshBindEndpoint() const;
    QString localMeshEndpoint(bool createSession) const;
    QString meshPeerToken();
    bool selectedDeviceSupportsSampleRate(int sampleRate);
    void testDeviceSelection(QComboBox* device, QPushButton* button, QWidget* dialogParent);
    jam2::gui::SessionAudioDeviceList sessionDialogDevices(
        const AudioDevicePreference& preference,
        const QString& requestedDeviceId) const;
    void applyStartJamDialogState(const jam2::gui::StartJamDialogState& state);
    void applyJoinJamDialogState(const jam2::gui::JoinJamDialogState& state);
    void applyPreferencesToControls();
    void applyNewJamDefaults();
    void initializeStartupWorkflow();
    void applyCreateDefaultsToControls();
    void applyJoinDefaultsToControls();
    void saveCreateDefaults();
    void saveJoinDefaults();
    QString promptJamTasterSourceDisposition();
    void prepareNetworkRuntimePresentation(bool createSession);
    void launchLocalRuntime(Jam2RuntimeOptions options);
    bool submitEngineCommand(jam2::EngineCommand command, const QString& context);
    void submitEngineGain(jam2::EngineCommandType type, double gain, const QString& context);
    void submitEngineToggle(jam2::EngineCommandType type, bool enabled, const QString& context);
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
    void updateTrackControls();
    void updateTrackPlaybackPresentation();
    void refreshLooperLanes();
    qint64 looperLaneTimelineEndFrame(
        const LooperLane& lane,
        int sampleRate,
        qint64* resolvedSourceFrames = nullptr) const;
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
    bool importWavIntoLooperLane(int laneIndex, const QString& sourcePath);
    void shareLocalTracks(bool includeLocalOnly = false);
    void addEmptyLooperLane();
    void removeSelectedLooperLane();
    void revealLooperLaneWav(int laneIndex);
    void removeLooperLaneWav(int laneIndex);
    void cancelUnreferencedLooperAssetTransfer(const QString& hash);
    void renameSelectedLooperLane();
    bool armSelectedLooperLaneRecording();
    bool showLaneRecordingDialog(
        const QString& bankId,
        const QString& laneId,
        const QString& laneName);
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
    QString selectedDeviceId() const;
    QString selectedDeviceDescription() const;
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
    void applyTapTrackMetronomeTempoAt(std::int64_t elapsedMs);
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
    Jam2MetronomeCompensationSettings metronomeCompensationSettings() const;
    void applyMetronomeCompensationToRunningJam();
    void sendMetronomePatternToJam();
    void updateMetronomePresentationFromEngine(const jam2::EngineSnapshot& snapshot);
    void showMetronomeCompensationDialog();
    void updateMetronomeCompensationVisibility();
    void publishLocalTrackBatch(const QString& batchId);
    void handleTrackBatchOffer(const QJsonObject& message, const QString& sourcePeerToken);
    void handleTrackBatchComplete(const QJsonObject& message, const QString& sourcePeerToken);
    void supersedePendingTrackBatches(
        const QString& sourcePeerToken,
        const QSet<QString>& replacementArrangementHashes);
    void expirePendingTrackBatch(const QString& sourcePeerToken, const QString& batchId);
    void scheduleOutgoingTrackBatchExpiry(const QString& batchId);
    void scheduleIncomingTrackBatchExpiry(
        const QString& sourcePeerToken,
        const QString& batchId);
    void noteTrackAssetProgress(
        const QString& hash,
        const QString& peerToken,
        bool receiving);
    void retryOrFailIncomingAsset(
        const QString& hash,
        const QString& failedSourcePeerToken);
    void handleAssetRequestStartTimeout(
        TrackWorkspaceController::IncomingAssetWorkflow workflow,
        const QString& hash,
        const QString& source,
        std::uint64_t requestGeneration,
        int timeoutMilliseconds);
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
    QJsonObject preserveLocalOnlyLanes(QJsonObject song);
    QString looperAssetPathForHash(const QString& hash) const;
    QJsonObject trackToJson() const;
    void loadTrackJson(
        const QJsonObject& object,
        SharedTrackModel validatedModel);
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
    void discardObsoletePreparedMixPaths();
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
    QString recordingLatencySummary() const;
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
    QString sessionHex() const;
    QString keyHex() const;
    void generateSession();
    void showAudioInputSources();
    void showMidiInputSources();
    void showInputPlugins();
    void refreshInputSourceRouting();
    void updateInputSourceButtons();
    using PluginStartCallback = std::function<void(
        std::unique_ptr<jam2::application::InputPluginHost>, const QString&)>;
    using PluginLoadProgressCallback = std::function<void(int, const QString&)>;
    bool selectAndStartPluginAsync(
        std::size_t slot,
        jam2::audio::InputSourceKind kind,
        jam2::midi::EventQueue* midiQueue,
        PluginStartCallback completion,
        PluginLoadProgressCallback progress);
    void removeAudioPlugin(std::size_t slot);
    void retirePluginHost(
        std::unique_ptr<jam2::application::InputPluginHost> host);

    struct AudioPluginSource {
        std::unique_ptr<jam2::application::InputPluginHost> host;
        QString name;
        bool bypassed = false;
        std::size_t firstChannel = jam2::audio::kNoInputChannel;
        std::size_t secondChannel = jam2::audio::kNoInputChannel;
        int levelPpm = 1000000;
        bool included = true;
        bool consumedByStereoGroup = false;
    };
    struct MidiPluginSource {
        jam2::midi::DeviceInfo deviceInfo;
        jam2::midi::InputMode mode = jam2::midi::InputMode::Standard;
        jam2::midi::EventQueue events;
        std::unique_ptr<jam2::midi::InputDevice> device;
        std::unique_ptr<jam2::application::InputPluginHost> host;
        QString pluginName;
        std::size_t routerSlot = 0;
        bool muted = false;
        int levelPpm = 1000000;
        bool included = true;
    };

    std::array<AudioPluginSource, jam2::audio::kMaximumInputSources> audioPluginSources_{};
    std::vector<std::shared_ptr<MidiPluginSource>> midiPluginSources_;
    std::vector<std::unique_ptr<jam2::application::InputPluginHost>> retiredPluginHosts_;
    std::vector<std::shared_ptr<MidiPluginSource>> retiredMidiSources_;
    std::shared_ptr<jam2::application::MidiInputBackend> midiInputBackend_;
    std::shared_ptr<jam2::application::InputPluginBackend> inputPluginBackend_;
    std::uint64_t automationMidiDiscoveryCompletions_ = 0;
    std::uint64_t automationInputPluginLoadCompletions_ = 0;
    jam2::audio::InputSourceRouter* attachedInputRouter_ = nullptr;
    std::optional<std::size_t> recordingInputSourceSlot_;

    ApplicationRuntime jam2_;
    UserPreferences preferences_;
    jam2::gui::SessionRuntimeDraft sessionRuntimeDraft_;
    bool preferencesInitialized_ = false;
    std::vector<jam2::audio::DeviceInfo> availableDevices_;
    QMap<QString, jam2::audio::DeviceTestResult> deviceCapabilitiesCache_;
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

    QSpinBox* bpmSpin_ = nullptr;
    QComboBox* metronomeModeBox_ = nullptr;
    QComboBox* metronomeSoundBox_ = nullptr;
    QSlider* metronomeLevelSlider_ = nullptr;
    QSlider* remoteLevelSlider_ = nullptr;
    QSlider* masterOutputLevelSlider_ = nullptr;
    QPushButton* metronomeCompensationButton_ = nullptr;
    QDoubleSpinBox* metronomeCompensationMaxSpin_ = nullptr;
    QDoubleSpinBox* metronomeCompensationSmoothingSpin_ = nullptr;
    QDoubleSpinBox* metronomeCompensationDeadbandSpin_ = nullptr;
    QDoubleSpinBox* metronomeCompensationSlewSpin_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QToolButton* jamSyncButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* leaveJamButton_ = nullptr;
    QPushButton* refreshControlButton_ = nullptr;
    QString sessionHeaderRtt_;
    QString sessionHeaderIncomingAudio_;
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
    QCheckBox* captureManualStopCheck_ = nullptr;
    QSpinBox* captureDurationSpin_ = nullptr;
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
    QCheckBox* performanceCountInCheck_ = nullptr;
    QLabel* performancePositionLabel_ = nullptr;
    QLineEdit* detailPositionLabel_ = nullptr;
    QSlider* selectedPeerGainSlider_ = nullptr;
    QLabel* selectedPeerNameLabel_ = nullptr;
    QLabel* selectedPeerGainLabel_ = nullptr;
    QLabel* performanceLeftTitle_ = nullptr;
    QLabel* performanceRightTitle_ = nullptr;
    QWidget* performanceLocalControls_ = nullptr;
    QPushButton* performanceAudioInputsButton_ = nullptr;
    QPushButton* performanceMidiInputsButton_ = nullptr;
    QPushButton* performancePluginsButton_ = nullptr;
    QPushButton* performancePluginBypassButton_ = nullptr;
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
    QLabel* diagnosticIncomingAudioValue_ = nullptr;
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
    struct LoopbackSourceChoice {
        QString label;
        QString id;
    };
    struct LaneRecordingRuntimeState {
        QString outputPath;
        QVector<LoopbackSourceChoice> loopbackSources{
            {QStringLiteral("[default] System mix"), QStringLiteral("default")},
        };
        QString loopbackSourceId = QStringLiteral("default");
        QString loopbackSourceName = QStringLiteral("[default] System mix");
        int latencyAdjustmentFrames = 0;
        double silenceThresholdDb = -50.0;
        int tailSilenceMs = 1000;
        bool trimLeading = true;
        bool trimTrailing = true;
    } laneRecordingState_;
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
    int recordedLaneImportRetryAttempts_ = 0;
    std::uint64_t recordedLaneImportBusyRetries_ = 0;
    std::uint64_t recordedLaneImportFailures_ = 0;
    QString recordedLaneImportStatus_ = QStringLiteral("idle");
    QString recordedLaneImportTargetId_;
    QString recordedLaneImportLastHash_;
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
    jam2::gui::SharedBankLaunchCoordinator sharedBankLaunch_;
    std::optional<quint64> pendingBankRequestedTargetBeat_;
    bool arrangementRunning_ = false;
    bool arrangementArmed_ = false;
    bool arrangementResetBankAfterStop_ = false;
    int arrangementStepIndex_ = 0;
    int arrangementStepRepeat_ = 0;
    quint64 arrangementSectionStartBeat_ = 0;
    bool referenceWavGenerationRunning_ = false;
    QSet<QString> handledReferenceRenderRequests_;
    QMap<QString, QPair<QJsonObject, QString>> deferredReferenceRenderRequests_;
    ProjectPersistenceCoordinator& projectPersistence_;
    jam2::gui::PreparedMixLifecycle& preparedMixLifecycle_;
    QThreadPool& fileWorkerPool_;
    bool& publishStoppedTrackStateWhenApplied_;
    int& fileWorkerTasksActive_;
    int& fileWorkerTasksHighWater_;
    std::uint64_t& fileWorkerTasksCompleted_;
    std::uint64_t& fileWorkerTasksRejected_;
    bool& trackWaveformWorkerRunning_;
    std::uint64_t& trackWaveformRevision_;
    using LooperWaveformPreview = TrackWorkspaceController::LooperWaveformPreview;
    QMap<QString, LooperWaveformPreview>& looperWaveformCache_;
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
    bool automationHeadlessAudio_ = false;
    Jam2TestInputMode automationTestInput_ = Jam2TestInputMode::Silence;
    bool automationSuppressDialogs_ = false;
    bool automationOfferPauseArmed_ = false;
    bool automationOfferPauseActive_ = false;
    quint64 automationOfferPauseGeneration_ = 0;
    struct AutomationFileWorkerGate {
        std::mutex mutex;
        std::condition_variable releasedCondition;
        bool released = false;
    };
    std::shared_ptr<AutomationFileWorkerGate> automationFileWorkerGate_;
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
