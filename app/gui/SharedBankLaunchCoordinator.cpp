#include "SharedBankLaunchCoordinator.hpp"

#include "runtime_limits.hpp"
#include "transport_timing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace jam2::gui {
namespace {

constexpr std::uint64_t kMaximumFrame =
    (std::numeric_limits<std::uint64_t>::max)();

std::uint64_t saturatingAdd(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    return left > kMaximumFrame - right ? kMaximumFrame : left + right;
}

std::uint64_t roundedFrameCount(long double exactFrames) noexcept
{
    if (std::isnan(exactFrames) || exactFrames <= 0.0L) {
        return 0;
    }
    if (std::isinf(exactFrames)) return kMaximumFrame;
    const long double maximum = static_cast<long double>(kMaximumFrame);
    if (exactFrames >= maximum - 0.5L) {
        return kMaximumFrame;
    }
    return static_cast<std::uint64_t>(std::floor(exactFrames + 0.5L));
}

} // namespace

const SharedBankLaunchSnapshot& SharedBankLaunchCoordinator::snapshot() const noexcept
{
    return state_;
}

bool SharedBankLaunchCoordinator::active() const noexcept
{
    return state_.active();
}

bool SharedBankLaunchCoordinator::matches(
    const QString& switchId,
    int bankIndex) const noexcept
{
    return state_.active() && state_.switchId == switchId &&
        state_.bankIndex == bankIndex;
}

bool SharedBankLaunchCoordinator::beginHost(
    QString switchId,
    int bankIndex,
    std::uint64_t requestedTargetBeat,
    QSet<QString> expectedPeerTokens,
    const QString& selfPeerToken)
{
    if (switchId.isEmpty() || bankIndex < 0) {
        return false;
    }
    expectedPeerTokens.remove(QString{});
    expectedPeerTokens.remove(selfPeerToken);
    state_ = {
        std::move(switchId),
        bankIndex,
        requestedTargetBeat,
        false,
        std::move(expectedPeerTokens),
        {},
    };
    return true;
}

bool SharedBankLaunchCoordinator::preparePeer(QString switchId, int bankIndex)
{
    if (switchId.isEmpty() || bankIndex < 0) {
        return false;
    }
    if (!matches(switchId, bankIndex)) {
        state_ = {
            std::move(switchId),
            bankIndex,
            0,
            false,
            {},
            {},
        };
    }
    return true;
}

bool SharedBankLaunchCoordinator::markHostReady(int bankIndex) noexcept
{
    if (!state_.active() || bankIndex != state_.bankIndex) {
        return false;
    }
    state_.hostReady = true;
    return true;
}

SharedBankReadyResult SharedBankLaunchCoordinator::markPeerReady(
    int bankIndex,
    const QString& switchId,
    const QString& sourcePeerToken,
    const QString& selfPeerToken)
{
    if (!matches(switchId, bankIndex)) {
        return SharedBankReadyResult::Stale;
    }
    if (sourcePeerToken.isEmpty()) {
        return SharedBankReadyResult::InvalidPeer;
    }
    if (sourcePeerToken == selfPeerToken) {
        return SharedBankReadyResult::Self;
    }
    if (!state_.expectedPeerTokens.contains(sourcePeerToken)) {
        return SharedBankReadyResult::NotExpected;
    }
    if (state_.readyPeerTokens.contains(sourcePeerToken)) {
        return SharedBankReadyResult::Duplicate;
    }
    state_.readyPeerTokens.insert(sourcePeerToken);
    return SharedBankReadyResult::Accepted;
}

bool SharedBankLaunchCoordinator::removeExpectedPeer(const QString& peerToken)
{
    state_.readyPeerTokens.remove(peerToken);
    return state_.expectedPeerTokens.remove(peerToken);
}

bool SharedBankLaunchCoordinator::readyToCommit() const noexcept
{
    if (!state_.active() || !state_.hostReady) {
        return false;
    }
    return std::all_of(
        state_.expectedPeerTokens.cbegin(),
        state_.expectedPeerTokens.cend(),
        [this](const QString& token) {
            return state_.readyPeerTokens.contains(token);
        });
}

SharedBankLaunchSnapshot SharedBankLaunchCoordinator::take()
{
    SharedBankLaunchSnapshot result = std::move(state_);
    state_ = {};
    return result;
}

void SharedBankLaunchCoordinator::clear() noexcept
{
    state_ = {};
}

std::uint64_t nextBankBoundaryBeat(
    std::uint64_t absoluteBeat,
    int beatsPerBar,
    bool quantizeToBar) noexcept
{
    if (!quantizeToBar) {
        return absoluteBeat == kMaximumFrame ? absoluteBeat : absoluteBeat + 1ULL;
    }
    const std::uint64_t barBeats = static_cast<std::uint64_t>(
        std::max(1, beatsPerBar));
    const std::uint64_t bar = absoluteBeat / barBeats;
    if (bar >= kMaximumFrame / barBeats) {
        return kMaximumFrame;
    }
    return (bar + 1ULL) * barBeats;
}

std::uint64_t sharedBankCommitTargetBeat(
    bool bankTimingDiffers,
    bool transportPlaying,
    bool engineAnchored,
    int sampleRate,
    double secondsPerBeat,
    std::uint64_t currentAbsoluteBeat,
    std::uint64_t requestedTargetBeat,
    int beatsPerBar) noexcept
{
    if (bankTimingDiffers || !transportPlaying || !engineAnchored ||
        !jam2::limits::valid_sample_rate(sampleRate) ||
        !std::isfinite(secondsPerBeat) || secondsPerBeat <= 0.0) {
        return 0;
    }
    return requestedTargetBeat > currentAbsoluteBeat
        ? requestedTargetBeat
        : nextBankBoundaryBeat(currentAbsoluteBeat, beatsPerBar, true);
}

std::optional<BankLaunchFrameSchedule> bankLaunchFrameSchedule(
    std::uint64_t currentAbsoluteBeat,
    std::uint64_t epochMusicalFrame,
    int sampleRate,
    double secondsPerBeat,
    std::int64_t renderOffsetFrames,
    std::optional<std::uint64_t> requestedTargetBeat,
    int beatsPerBar) noexcept
{
    if (!jam2::limits::valid_sample_rate(sampleRate) ||
        !std::isfinite(secondsPerBeat) || secondsPerBeat <= 0.0) {
        return std::nullopt;
    }
    const std::uint64_t targetBeat = requestedTargetBeat.value_or(
        nextBankBoundaryBeat(currentAbsoluteBeat, beatsPerBar, true));
    const long double exactFrames =
        static_cast<long double>(targetBeat) *
        static_cast<long double>(secondsPerBeat) *
        static_cast<long double>(sampleRate);
    const std::uint64_t targetMusicalFrame = saturatingAdd(
        epochMusicalFrame, roundedFrameCount(exactFrames));
    return BankLaunchFrameSchedule{
        targetBeat,
        targetMusicalFrame,
        jam2::transport_raw_frame_from_musical(
            targetMusicalFrame, renderOffsetFrames),
    };
}

} // namespace jam2::gui
