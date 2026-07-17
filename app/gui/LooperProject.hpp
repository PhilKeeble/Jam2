#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

struct LooperLane {
    QString id;
    QString assetPath;
    QString assetHash;
    QString name;
    int sampleRate = 0;
    bool sampleRateCompatible = true;
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
};

struct LooperBank {
    QString id;
    QVector<LooperLane> lanes;
};

class LooperProject {
public:
    LooperProject();

    const QVector<LooperBank>& banks() const;
    QVector<LooperBank>& banks();
    int activeBankIndex() const;
    void setActiveBankIndex(int index);
    bool gridLockEnabled() const;
    void setGridLockEnabled(bool enabled);
    bool trackSyncEnabled() const;
    void setTrackSyncEnabled(bool enabled);

    // These are the only mutations the track UI and sync layer need.  Keeping
    // them here preserves the four-bank invariant and the lane order that is
    // used when rendering a prepared mix.
    bool appendLane(int bankIndex, LooperLane lane);
    bool removeLane(int bankIndex, int laneIndex);
    bool moveLane(int bankIndex, int from, int to);
    bool renameLane(int bankIndex, int laneIndex, const QString& name);

    QJsonObject toJson(bool syncCompatibleOnly = false) const;
    bool loadJson(const QJsonObject& object);

private:
    QVector<LooperBank> banks_;
    int activeBankIndex_ = 0;
    bool gridLockEnabled_ = true;
    bool trackSyncEnabled_ = true;
};
