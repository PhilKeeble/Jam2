#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cstdint>

struct LooperLane {
    QString id;
    QString assetPath;
    QString assetHash;
    QString name;
    int sampleRate = 0;
    bool sampleRateCompatible = true;
    qint64 sourceFrames = 0;
    qint64 startFrame = 0;
    qint64 stopFrame = -1;
    qint64 loopStartFrame = -1;
    qint64 loopEndFrame = -1;
    double gainDb = 0.0;
    bool muted = false;
    bool solo = false;
    bool loopEnabled = false;
    QString referenceKind;
    QString referenceSourceSignature;
    double referenceBpm = 0.0;
    bool referenceStale = false;
    bool localOnly = false;
    QString originKind;
};

struct LooperLaneRegion {
    qint64 startFrame = 0;
    qint64 stopFrame = -1;
    qint64 sourceStartFrame = -1;
    qint64 sourceEndFrame = -1;
    bool loopEnabled = false;
};

enum class LooperLaneTimelineCropStatus {
    Unchanged,
    Cropped,
    Cleared,
    Rejected,
};

struct LooperLaneTimelineCropResult {
    LooperLaneTimelineCropStatus status = LooperLaneTimelineCropStatus::Rejected;
    QString removedAssetPath;
    QString removedAssetHash;
};

namespace jam2::gui {

// Resolves the lane's audible end on the prepared-mix timeline without file
// I/O. The caller supplies a known source length from the model, waveform
// cache, or strict WAV inspection. The result is always within the maintained
// looper timeline domain.
qint64 looperLaneTimelineEnd(
    const LooperLane& lane,
    qint64 resolvedSourceFrames,
    int timelineSampleRate) noexcept;

} // namespace jam2::gui

struct LooperBankTiming {
    int bpm = 80;
    int beatsPerBar = 4;
    int beatUnit = 4;
    int tempoPulseUnits = 1;
    int division = 1;
    std::uint64_t playMaskLow = 0x0fULL;
    std::uint64_t playMaskHigh = 0;
    std::uint64_t accentMaskLow = 0x01ULL;
    std::uint64_t accentMaskHigh = 0;
    bool inheritsBankA = false;
};

struct LooperBank {
    QString id;
    QVector<LooperLane> lanes;
    LooperBankTiming timing;
};

struct ArrangementStep {
    int bankIndex = 0;
    int repeats = 1;
};

struct ArrangementDefinition {
    QVector<ArrangementStep> steps;
    bool loop = true;
    bool enabled = false;
};

class LooperProject {
public:
    LooperProject();

    const QVector<LooperBank>& banks() const;
    QVector<LooperBank>& banks();
    bool addBank();
    bool removeLastBank();
    int activeBankIndex() const;
    void setActiveBankIndex(int index);
    bool gridLockEnabled() const;
    void setGridLockEnabled(bool enabled);
    bool trackSyncEnabled() const;
    void setTrackSyncEnabled(bool enabled);
    LooperBankTiming resolvedTiming(int bankIndex) const;
    bool setTiming(int bankIndex, LooperBankTiming timing);
    bool hasSerializedTiming() const;
    const ArrangementDefinition& arrangement() const;
    bool setArrangement(ArrangementDefinition arrangement);
    void setArrangementEnabled(bool enabled);
    void ensureInitialEmptyLanes();

    // These are the only mutations the track UI and sync layer need.  Keeping
    // them here preserves the four-bank invariant and the lane order that is
    // used when rendering a prepared mix.
    bool appendLane(int bankIndex, LooperLane lane);
    // Atomically replaces one lane's checked state while retaining its stable
    // identity. This is used when an asynchronous WAV import completes after
    // the UI has resolved the original lane by ID.
    bool replaceLane(int bankIndex, int laneIndex, LooperLane lane);
    bool removeLane(int bankIndex, int laneIndex);
    bool moveLane(int bankIndex, int from, int to);
    bool renameLane(int bankIndex, int laneIndex, const QString& name);
    bool setLaneGainDb(int bankIndex, int laneIndex, double gainDb);
    bool setLaneMuted(int bankIndex, int laneIndex, bool muted);
    bool setLaneSolo(int bankIndex, int laneIndex, bool solo);
    bool setLaneRegion(
        int bankIndex,
        int laneIndex,
        const LooperLaneRegion& region);
    bool clearLaneAsset(
        int bankIndex,
        int laneIndex,
        bool resetTimelineStart = false);
    LooperLaneTimelineCropResult cropLaneToTimelineEnd(
        int bankIndex,
        int laneIndex,
        qint64 resolvedSourceFrames,
        int timelineSampleRate,
        qint64 timelineEndFrame);

    QJsonObject toJson(bool syncCompatibleOnly = false) const;
    bool loadJson(const QJsonObject& object);

private:
    QVector<LooperBank> banks_;
    int activeBankIndex_ = 0;
    bool gridLockEnabled_ = true;
    bool trackSyncEnabled_ = true;
    bool hasSerializedTiming_ = true;
    ArrangementDefinition arrangement_;
};
