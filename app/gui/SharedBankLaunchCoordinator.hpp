#pragma once

#include <QSet>
#include <QString>

#include <cstdint>
#include <optional>

namespace jam2::gui {

enum class SharedBankReadyResult {
    Accepted,
    Duplicate,
    Stale,
    InvalidPeer,
    Self,
    NotExpected,
};

struct SharedBankLaunchSnapshot {
    QString switchId;
    int bankIndex = -1;
    std::uint64_t requestedTargetBeat = 0;
    bool hostReady = false;
    QSet<QString> expectedPeerTokens;
    QSet<QString> readyPeerTokens;

    bool active() const noexcept
    {
        return !switchId.isEmpty() && bankIndex >= 0;
    }
};

class SharedBankLaunchCoordinator {
public:
    const SharedBankLaunchSnapshot& snapshot() const noexcept;
    bool active() const noexcept;
    bool matches(const QString& switchId, int bankIndex) const noexcept;

    bool beginHost(
        QString switchId,
        int bankIndex,
        std::uint64_t requestedTargetBeat,
        QSet<QString> expectedPeerTokens,
        const QString& selfPeerToken);
    bool preparePeer(QString switchId, int bankIndex);
    bool markHostReady(int bankIndex) noexcept;
    SharedBankReadyResult markPeerReady(
        int bankIndex,
        const QString& switchId,
        const QString& sourcePeerToken,
        const QString& selfPeerToken);
    bool removeExpectedPeer(const QString& peerToken);
    bool readyToCommit() const noexcept;
    SharedBankLaunchSnapshot take();
    void clear() noexcept;

private:
    SharedBankLaunchSnapshot state_;
};

std::uint64_t nextBankBoundaryBeat(
    std::uint64_t absoluteBeat,
    int beatsPerBar,
    bool quantizeToBar) noexcept;

std::uint64_t sharedBankCommitTargetBeat(
    bool bankTimingDiffers,
    bool transportPlaying,
    bool engineAnchored,
    int sampleRate,
    double secondsPerBeat,
    std::uint64_t currentAbsoluteBeat,
    std::uint64_t requestedTargetBeat,
    int beatsPerBar) noexcept;

struct BankLaunchFrameSchedule {
    std::uint64_t targetAbsoluteBeat = 0;
    std::uint64_t targetMusicalFrame = 0;
    std::uint64_t targetRawFrame = 0;
};

std::optional<BankLaunchFrameSchedule> bankLaunchFrameSchedule(
    std::uint64_t currentAbsoluteBeat,
    std::uint64_t epochMusicalFrame,
    int sampleRate,
    double secondsPerBeat,
    std::int64_t renderOffsetFrames,
    std::optional<std::uint64_t> requestedTargetBeat,
    int beatsPerBar) noexcept;

} // namespace jam2::gui
