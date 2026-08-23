#pragma once

#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

namespace jam2::gui::track_asset_ownership {

struct Claim {
    QString key;
    QString sourcePeerToken;
    QString batchId;
    QString assetHash;
    int batchSize = 0;
};

struct SupersessionPlan {
    QStringList removedKeys;
    QSet<QString> removedHashes;
    QMap<QString, int> batchSizes;
    bool preserveActiveTransfer = false;

    bool found() const noexcept { return !removedKeys.isEmpty(); }
};

struct ExpiryPlan {
    QStringList removedKeys;
    QSet<QString> removedHashes;
    QMap<QString, QString> remainingTrackSources;
    QSet<QString> stillExpectedHashes;
    bool activeSourceDetached = false;

    bool found() const noexcept { return !removedKeys.isEmpty(); }
};

ExpiryPlan planBatchExpiry(
    const QList<Claim>& claims,
    const QString& expiredSourcePeerToken,
    const QString& expiredBatchId,
    bool activeTrackContribution,
    const QString& activeHash,
    const QString& activeSourcePeerToken,
    const QSet<QString>& pendingArrangementHashes);

SupersessionPlan planPeerSupersession(
    const QList<Claim>& claims,
    const QString& supersededSourcePeerToken,
    bool activeTrackContribution,
    const QString& activeHash,
    const QString& activeSourcePeerToken,
    const QSet<QString>& replacementArrangementHashes);

} // namespace jam2::gui::track_asset_ownership
