#include "SharedBankLaunchCoordinator.hpp"

#include <QCoreApplication>
#include <QSet>
#include <QString>

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void testFourPeerBarrierSnapshot()
{
    using jam2::gui::SharedBankLaunchCoordinator;
    using jam2::gui::SharedBankReadyResult;

    SharedBankLaunchCoordinator coordinator;
    require(!coordinator.active() && !coordinator.readyToCommit() &&
            !coordinator.beginHost({}, 1, 0, {}, QStringLiteral("creator")) &&
            !coordinator.beginHost(
                QStringLiteral("switch"), -1, 0, {}, QStringLiteral("creator")),
        "inactive and invalid shared-bank barriers must fail closed");

    QSet<QString> originalPeers{
        QStringLiteral("creator"),
        QStringLiteral("peer-2"),
        QStringLiteral("peer-3"),
        QStringLiteral("peer-4"),
        QString{},
    };
    require(coordinator.beginHost(
                QStringLiteral("switch-a"), 1, 24,
                originalPeers, QStringLiteral("creator")) &&
            coordinator.active() &&
            coordinator.matches(QStringLiteral("switch-a"), 1) &&
            !coordinator.matches(QStringLiteral("switch-a"), 2) &&
            coordinator.snapshot().expectedPeerTokens == QSet<QString>{
                QStringLiteral("peer-2"),
                QStringLiteral("peer-3"),
                QStringLiteral("peer-4")} &&
            coordinator.snapshot().requestedTargetBeat == 24,
        "host barrier must snapshot exactly the other three original peers");

    require(!coordinator.markHostReady(0) && coordinator.markHostReady(1) &&
            !coordinator.readyToCommit(),
        "host readiness must match the prepared bank and still wait for peers");
    require(coordinator.markPeerReady(
                1, QStringLiteral("stale"), QStringLiteral("peer-2"),
                QStringLiteral("creator")) == SharedBankReadyResult::Stale &&
            coordinator.markPeerReady(
                2, QStringLiteral("switch-a"), QStringLiteral("peer-2"),
                QStringLiteral("creator")) == SharedBankReadyResult::Stale &&
            coordinator.markPeerReady(
                1, QStringLiteral("switch-a"), {}, QStringLiteral("creator")) ==
                SharedBankReadyResult::InvalidPeer &&
            coordinator.markPeerReady(
                1, QStringLiteral("switch-a"), QStringLiteral("creator"),
                QStringLiteral("creator")) == SharedBankReadyResult::Self &&
            coordinator.markPeerReady(
                1, QStringLiteral("switch-a"), QStringLiteral("peer-5"),
                QStringLiteral("creator")) == SharedBankReadyResult::NotExpected,
        "stale, wrong-bank, empty, self, and unprepared readiness must reject");

    require(coordinator.markPeerReady(
                1, QStringLiteral("switch-a"), QStringLiteral("peer-4"),
                QStringLiteral("creator")) == SharedBankReadyResult::Accepted &&
            coordinator.markPeerReady(
                1, QStringLiteral("switch-a"), QStringLiteral("peer-2"),
                QStringLiteral("creator")) == SharedBankReadyResult::Accepted &&
            coordinator.markPeerReady(
                1, QStringLiteral("switch-a"), QStringLiteral("peer-2"),
                QStringLiteral("creator")) == SharedBankReadyResult::Duplicate &&
            !coordinator.readyToCommit(),
        "out-of-order and duplicate readiness must remain idempotent while one peer is due");

    require(!coordinator.removeExpectedPeer(QStringLiteral("missing")) &&
            coordinator.removeExpectedPeer(QStringLiteral("peer-3")) &&
            coordinator.readyToCommit(),
        "a departed original peer must stop blocking the remaining prepared peers");
    const auto completed = coordinator.take();
    require(completed.active() && completed.switchId == QStringLiteral("switch-a") &&
            completed.bankIndex == 1 && completed.requestedTargetBeat == 24 &&
            completed.expectedPeerTokens == QSet<QString>{
                QStringLiteral("peer-2"), QStringLiteral("peer-4")} &&
            completed.readyPeerTokens == completed.expectedPeerTokens &&
            !coordinator.active() && !coordinator.readyToCommit(),
        "taking a complete barrier must return its exact evidence and clear all live state");
}

void testPeerPreparationReplacementAndClear()
{
    jam2::gui::SharedBankLaunchCoordinator coordinator;
    require(!coordinator.preparePeer({}, 0) &&
            !coordinator.preparePeer(QStringLiteral("switch"), -1),
        "peer preparation must reject invalid identity and bank");
    require(coordinator.preparePeer(QStringLiteral("switch-a"), 2) &&
            coordinator.matches(QStringLiteral("switch-a"), 2) &&
            coordinator.snapshot().requestedTargetBeat == 0 &&
            coordinator.snapshot().expectedPeerTokens.isEmpty(),
        "peer preparation must establish one exact incoming barrier");
    require(coordinator.markHostReady(2) &&
            coordinator.preparePeer(QStringLiteral("switch-a"), 2) &&
            coordinator.snapshot().hostReady,
        "an exact duplicate prepare must preserve already-owned state");
    require(coordinator.preparePeer(QStringLiteral("switch-a"), 3) &&
            coordinator.matches(QStringLiteral("switch-a"), 3) &&
            !coordinator.snapshot().hostReady,
        "an inconsistent bank under the same switch identity must reset stale readiness");
    coordinator.clear();
    require(!coordinator.active() && coordinator.snapshot().bankIndex == -1 &&
            coordinator.snapshot().switchId.isEmpty() &&
            coordinator.snapshot().expectedPeerTokens.isEmpty() &&
            coordinator.snapshot().readyPeerTokens.isEmpty(),
        "explicit cancellation must clear every barrier field");
}

void testBoundaryBeatAndFrameSchedule()
{
    using jam2::gui::bankLaunchFrameSchedule;
    using jam2::gui::nextBankBoundaryBeat;
    using jam2::gui::sharedBankCommitTargetBeat;
    constexpr std::uint64_t maximum =
        (std::numeric_limits<std::uint64_t>::max)();

    require(nextBankBoundaryBeat(0, 4, true) == 4 &&
            nextBankBoundaryBeat(4, 4, true) == 8 &&
            nextBankBoundaryBeat(5, 4, true) == 8 &&
            nextBankBoundaryBeat(11, 3, true) == 12 &&
            nextBankBoundaryBeat(7, 0, true) == 8 &&
            nextBankBoundaryBeat(7, 4, false) == 8 &&
            nextBankBoundaryBeat(maximum, 4, false) == maximum &&
            nextBankBoundaryBeat(maximum, 4, true) == maximum,
        "next bank boundary must be exact and saturating across the full beat domain");

    require(sharedBankCommitTargetBeat(
                false, true, true, 48000, 0.5, 5, 24, 4) == 24 &&
            sharedBankCommitTargetBeat(
                false, true, true, 48000, 0.5, 24, 24, 4) == 28 &&
            sharedBankCommitTargetBeat(
                false, true, true, 48000, 0.5, 5, 0, 4) == 8,
        "valid shared commit must retain a future request or choose the next bar");
    require(sharedBankCommitTargetBeat(
                true, true, true, 48000, 0.5, 5, 24, 4) == 0 &&
            sharedBankCommitTargetBeat(
                false, false, true, 48000, 0.5, 5, 24, 4) == 0 &&
            sharedBankCommitTargetBeat(
                false, true, false, 48000, 0.5, 5, 24, 4) == 0 &&
            sharedBankCommitTargetBeat(
                false, true, true, 7999, 0.5, 5, 24, 4) == 0 &&
            sharedBankCommitTargetBeat(
                false, true, true, 48000,
                std::numeric_limits<double>::infinity(), 5, 24, 4) == 0,
        "timing changes, stopped/unanchored transport, and invalid clocks must commit immediately");

    const auto nextBar = bankLaunchFrameSchedule(
        5, 1000, 48000, 0.5, 500, std::nullopt, 4);
    require(nextBar.has_value() && nextBar->targetAbsoluteBeat == 8 &&
            nextBar->targetMusicalFrame == 193000 &&
            nextBar->targetRawFrame == 192500,
        "ordinary bank scheduling must map the next bar through the stable epoch exactly");
    const auto requested = bankLaunchFrameSchedule(
        5, 1000, 48000, 0.5, -500, std::uint64_t{12}, 4);
    require(requested.has_value() && requested->targetAbsoluteBeat == 12 &&
            requested->targetMusicalFrame == 289000 &&
            requested->targetRawFrame == 289500,
        "an explicit future beat must retain its exact musical/raw offset projection");

    for (int sampleRate : {0, 1, 7999, 384001}) {
        require(!bankLaunchFrameSchedule(
                    0, 0, sampleRate, 0.5, 0, std::nullopt, 4).has_value(),
            "unsupported bank schedule sample rates must reject");
    }
    for (double secondsPerBeat : {
            0.0,
            -1.0,
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        require(!bankLaunchFrameSchedule(
                    0, 0, 48000, secondsPerBeat, 0, std::nullopt, 4).has_value(),
            "nonpositive and non-finite beat intervals must reject");
    }
    require(bankLaunchFrameSchedule(
                0, 0, 8000, 1.0, 0, std::uint64_t{1}, 4).has_value() &&
            bankLaunchFrameSchedule(
                0, 0, 384000, 1.0, 0, std::uint64_t{1}, 4).has_value(),
        "maintained sample-rate endpoints must remain schedulable");

    const auto saturated = bankLaunchFrameSchedule(
        maximum,
        maximum - 10,
        384000,
        (std::numeric_limits<double>::max)(),
        (std::numeric_limits<std::int64_t>::min)(),
        maximum,
        16);
    require(saturated.has_value() &&
            saturated->targetAbsoluteBeat == maximum &&
            saturated->targetMusicalFrame == maximum &&
            saturated->targetRawFrame == maximum,
        "extreme beat/frame products, epoch addition, and render offsets must saturate");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        testFourPeerBarrierSnapshot();
        testPeerPreparationReplacementAndClear();
        testBoundaryBeatAndFrameSchedule();
        std::cout << "Shared bank launch coordinator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Shared bank launch coordinator tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
