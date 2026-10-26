#pragma once

#include "ConcurrentLooperMerge.hpp"
#include "LooperProject.hpp"

#include <QJsonObject>
#include <QString>
#include <QSet>
#include <QVector>

// Classification belongs to the track even when its WAV arrives through a
// generic share batch rather than an authoritative song snapshot.
struct TrackReferenceMetadata {
    QString kind;
    QString sourceSignature;
    double bpm = 0;
    bool stale = false;
    static TrackReferenceMetadata fromJson(const QJsonObject& object);
    void writeTo(QJsonObject& object) const;
    void applyTo(LooperLane& lane) const;
};

// A presentation overlay only: pending bytes never enter the playing project.
struct PendingTrackLane {
    LooperLane lane;
    bool contribution = false;
    QString fallbackId;
};

enum class TrackWavAvailability { Ready, Waiting, Receiving, Validating, WaitingForBatch };

struct TrackLanePresentation {
    LooperLane lane;
    TrackWavAvailability availability = TrackWavAvailability::Ready;
    qint64 receivedBytes = 0;
    qint64 totalBytes = 0;
    bool pending() const noexcept { return availability != TrackWavAvailability::Ready; }
};

QVector<TrackLanePresentation> presentPendingTrackLanes(
    const QVector<LooperLane>& applied,
    const QVector<PendingTrackLane>& pending,
    const QSet<QString>& validatedHashes,
    const QString& incomingHash,
    qint64 receivedBytes,
    qint64 totalBytes);

struct WavMetadata {
    int audioFormat = 0;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataBytes = 0;
    qint64 frames = 0;
    int durationMs = 0;
    QString sha256;
};

struct StagedPcm16Asset {
    QString sourcePath;
    QString stagedPath;
    QString displayName;
    QString sha256;
    WavMetadata metadata;
    int sourceSampleRate = 0;
    qint64 sourceFrames = 0;
    bool resampled = false;
    // True only when this call committed new bytes at stagedPath. Reusing an
    // already-valid content-addressed file or its owned recording source is
    // false, allowing abandoned async work to clean up without deleting reuse.
    bool stagedFileCreated = false;
    QString error;
};

WavMetadata readWavMetadata(const QString& path);
QJsonObject readTrackSidecarJson(const QString& wavPath);
StagedPcm16Asset stagePcm16Asset(
    const QString& sourcePath,
    const QString& stagingFolder,
    int expectedSampleRate = 0,
    const QString& assetFolder = QStringLiteral("imported"));
int mergeSynchronizedLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject);
int mergeLocalOnlyLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject);

// Apply shared section/bank membership together, retaining local content only
// for matching identities. Scope-specific content is merged by the caller.
bool reconcileSynchronizedSongStructure(
    QJsonObject& localSong,
    const QJsonObject& incomingSong);

// One composed Server proposal survives superseding asynchronous validation.
// The authority revision and validation generation prevent reuse after commit,
// cancellation, session reset, or unrelated local changes.
enum class SharedSongScope { Tracks, IdeaFull, IdeaChords, IdeaBeats };

class ServerSongProposalStaging {
public:
    QJsonObject base(const QJsonObject& applied, quint64 authorityRevision,
        quint64 validationGeneration) const;
    void stage(const QJsonObject& composed, quint64 authorityRevision,
        quint64 validationGeneration, SharedSongScope scope = SharedSongScope::Tracks);
    QVector<SharedSongScope> scopes(quint64 authorityRevision, quint64 validationGeneration) const;
    void reject(quint64 validationGeneration);
private:
    QJsonObject song_;
    quint64 authorityRevision_ = 0;
    quint64 validationGeneration_ = 0;
    QVector<SharedSongScope> scopes_;
};
