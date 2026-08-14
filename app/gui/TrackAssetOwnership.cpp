#include "TrackAssetOwnership.hpp"

namespace jam2::gui::track_asset_ownership {

ExpiryPlan planBatchExpiry(
    const QList<Claim>& claims,
    const QString& expiredSourcePeerToken,
    const QString& expiredBatchId,
    bool activeTrackContribution,
    const QString& activeHash,
    const QString& activeSourcePeerToken,
    const QSet<QString>& pendingArrangementHashes)
{
    ExpiryPlan result;
    QList<Claim> remaining;
    remaining.reserve(claims.size());
    for (const Claim& claim : claims) {
        if (claim.sourcePeerToken == expiredSourcePeerToken &&
            claim.batchId == expiredBatchId) {
            result.removedKeys.append(claim.key);
            result.removedHashes.insert(claim.assetHash);
        } else {
            remaining.append(claim);
        }
    }
    if (!result.found()) return result;

    bool activeStillExpectedFromSource = false;
    for (const Claim& claim : remaining) {
        if (claim.assetHash == activeHash &&
            claim.sourcePeerToken == activeSourcePeerToken) {
            activeStillExpectedFromSource = true;
        }
        if (result.removedHashes.contains(claim.assetHash) &&
            !result.remainingTrackSources.contains(claim.assetHash)) {
            result.remainingTrackSources.insert(claim.assetHash, claim.sourcePeerToken);
        }
    }
    for (const QString& hash : result.removedHashes) {
        if (result.remainingTrackSources.contains(hash) ||
            pendingArrangementHashes.contains(hash)) {
            result.stillExpectedHashes.insert(hash);
        }
    }
    result.activeSourceDetached = activeTrackContribution &&
        activeSourcePeerToken == expiredSourcePeerToken &&
        result.removedHashes.contains(activeHash) &&
        !activeStillExpectedFromSource;
    return result;
}

} // namespace jam2::gui::track_asset_ownership
