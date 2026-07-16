#pragma once

#include "ApplicationRuntime.hpp"
#include "AssetTransferService.hpp"
#include "BeatGridModel.hpp"
#include "LooperProject.hpp"
#include "PreparedMixRenderer.hpp"
#include "ProjectPersistenceCoordinator.hpp"
#include "SharedTrackController.hpp"
#include "TrackRecordingWorkflow.hpp"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <cstdint>
#include <functional>
#include <vector>

// Non-widget owner for the Track workspace's models, persistence, bounded file
// workers, contribution reconciliation state, and asset-transfer state machine.
// MainWindow remains a presentation/intent adapter over this object.
class TrackWorkspaceController final : public QObject, public AssetTransferContext {
public:
    struct Callbacks {
        std::function<int()> sessionSampleRate;
        std::function<void(const QString&)> log;
        std::function<bool(const QString&, qint64)> canQueueControl;
        std::function<bool(const QString&, const QJsonObject&)> sendControl;
        std::function<bool(const QString&, const QByteArray&)> sendBinary;
        std::function<void()> incomingAssetAccepted;
    };

    struct LooperWaveformPreview {
        std::vector<float> peaks;
        qint64 sourceFrames = 0;
        bool valid = false;
    };

    struct PendingTrackContribution {
        QString sourcePeerToken;
        int bankIndex = 0;
        QString targetLaneId;
        QString assetHash;
        QString name;
        int sampleRate = 0;
    };

    explicit TrackWorkspaceController(ApplicationRuntime& runtime, QObject* parent = nullptr);
    ~TrackWorkspaceController() override;

    void setCallbacks(Callbacks callbacks);
    bool startFileTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed = {});
    void waitForWorkers();

    QObject* dispatchContext() noexcept override;
    bool trackSyncEnabled() const noexcept override;
    int sessionSampleRate() const noexcept override;
    QString assetPathForSend(const QString& hash) const override;
    QString incomingAssetPath(const QString& hash) const override;
    bool incomingAssetExpected(
        const QString& hash,
        const QString& sourcePeerToken) const override;
    void abandonIncomingAsset(const QString& hash) override;
    void acceptIncomingAsset(const QString& hash, const QString& path) override;
    void appendAssetLog(const QString& message) override;
    bool startAssetFileTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed) override;
    bool canQueueAssetControl(const QString& peerToken, qint64 estimatedBytes) const override;
    bool sendAssetControl(const QString& peerToken, const QJsonObject& message) override;
    bool sendAssetBinary(const QString& peerToken, const QByteArray& payload) override;

    SharedTrackController trackController;
    LooperProject looperProject;
    TrackRecordingWorkflow recordingWorkflow;
    BeatGridModel chordModel;
    BeatGridModel beatModel;
    BeatGridModel lyricModel;
    ProjectPersistenceCoordinator persistence;
    PreparedMixResult preparedMix;
    QThreadPool fileWorkers;
    AssetTransferService assetTransfer;

    bool preparedMixWorkerRunning = false;
    bool preparedMixRerunPending = false;
    bool playPreparedMixWhenReady = false;
    int pendingPreparedTrackReadyRevision = 0;
    quint64 pendingSharedTrackRevision = 0;
    bool pendingSharedTrackHostReady = true;
    QSet<QString> pendingSharedTrackReadyTokens;
    bool publishStoppedTrackStateWhenApplied = false;
    std::uint64_t preparedMixRequests = 0;
    std::uint64_t preparedMixCoalesced = 0;
    std::uint64_t preparedMixFailures = 0;
    int fileWorkerTasksActive = 0;
    int fileWorkerTasksHighWater = 0;
    std::uint64_t fileWorkerTasksCompleted = 0;
    std::uint64_t fileWorkerTasksRejected = 0;
    bool trackWaveformWorkerRunning = false;
    std::uint64_t trackWaveformRevision = 0;
    QMap<QString, LooperWaveformPreview> looperWaveformCache;
    bool looperWaveformWorkerRunning = false;
    bool wavCompatibilityAuditRunning = false;
    int pendingWavCompatibilityAuditRate = 0;
    QSet<QString> reportedIncompatibleWavs;
    QMap<QString, PendingTrackContribution> pendingTrackContributions;
    QSet<QString> appliedTrackContributionIds;
    QMap<QString, QJsonObject> localTrackOffers;
    QMap<QString, QString> trackOfferAssetPaths;
    QMap<QString, QString> pendingTrackAssetSources;
    QSet<QString> validatedTrackAssetHashes;
    QJsonObject pendingSongSet;
    QStringList pendingLooperAssetHashes;
    int pendingSongRevision = 0;
    bool pendingSongTrackRestart = false;
    QString pendingSongSourcePeerToken;
    bool pendingSongNeedsAuthoritativePublish = false;
    std::uint64_t songAssetCheckRevision = 0;
    QJsonObject deferredSongSetMessage;
    QString deferredSongSetSourcePeerToken;
    int looperArrangementRevision = 0;
    int lastAppliedHostArrangementRevision = 0;

private:
    Callbacks callbacks_;
};
