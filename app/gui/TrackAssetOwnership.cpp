#include "TrackAssetOwnership.hpp"

namespace jam2::gui::track_asset_ownership {

SupersessionPlan planPeerSupersession(
    const QList<Claim>& claims,
    const QString& supersededSourcePeerToken,
    bool activeTrackContribution,
    const QString& activeHash,
    const QString& activeSourcePeerToken,
    const QSet<QString>& replacementArrangementHashes)
{
    SupersessionPlan result;
    for (const Claim& claim : claims) {
        if (claim.sourcePeerToken != supersededSourcePeerToken) continue;
        result.removedKeys.append(claim.key);
        result.removedHashes.insert(claim.assetHash);
        result.batchSizes[claim.batchId] = qMax(
            result.batchSizes.value(claim.batchId), claim.batchSize);
    }
    // A sender may already be streaming the one requested WAV. Let that bounded
    // transfer finish so its hash-addressed result can satisfy the replacement
    // arrangement; discarding it makes the remaining sender frames look
    // unsolicited and starts a retry storm.
    result.preserveActiveTransfer = result.found() && activeTrackContribution &&
        activeSourcePeerToken == supersededSourcePeerToken &&
        replacementArrangementHashes.contains(activeHash);
    return result;
}

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
