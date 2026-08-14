#include "PreparedMixLifecycle.hpp"

#include <QCoreApplication>

#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

PreparedMixResult validResult(int bankIndex, const QString& path)
{
    PreparedMixResult result;
    result.bankIndex = bankIndex;
    result.path = path;
    result.frames = 48000;
    result.fileBytes = 96044;
    result.sampleRate = 48000;
    result.durationMs = 1000;
    result.sha256 = QString(64, QLatin1Char('a' + bankIndex));
    return result;
}

void testRequestAndCompletionOwnership()
{
    jam2::gui::PreparedMixLifecycle lifecycle;
    expect(!lifecycle.workerRunning() && lifecycle.workerBankIndex() == -1 &&
            lifecycle.requests() == 0 && lifecycle.coalesced() == 0 &&
            lifecycle.failures() == 0 && lifecycle.revision() == 0,
        "prepared lifecycle begins idle with zero diagnostics");

    const auto invalid = lifecycle.request(-1, 4, true);
    expect(invalid.status == jam2::gui::PreparedMixRequestStatus::Rejected &&
            lifecycle.requests() == 0 && lifecycle.revision() == 0,
        "invalid bank request rejects without changing lifecycle state");

    const auto first = lifecycle.request(0, 4, true);
    const auto coalesced = lifecycle.request(1, 4, true, 2);
    expect(first.status == jam2::gui::PreparedMixRequestStatus::StartWorker &&
            first.bankIndex == 0 && first.generation == 1 &&
            coalesced.status == jam2::gui::PreparedMixRequestStatus::Coalesced &&
            coalesced.generation == 2 && lifecycle.workerRunning() &&
            lifecycle.workerBankIndex() == 0 && lifecycle.rerunPending() &&
            lifecycle.rerunBankIndex() == 2 && lifecycle.requests() == 2 &&
            lifecycle.coalesced() == 1,
        "a running render coalesces to the priority bank with exact diagnostics");

    const auto wrong = lifecycle.complete(coalesced.generation, QStringLiteral("wrong.wav"));
    expect(wrong.status == jam2::gui::PreparedMixCompletionStatus::Rejected &&
            wrong.discardPath == QStringLiteral("wrong.wav") &&
            lifecycle.workerRunning() && lifecycle.rerunPending(),
        "a completion that does not own the running generation cannot consume it");

    const auto rerun = lifecycle.complete(first.generation, QStringLiteral("old.wav"));
    expect(rerun.status == jam2::gui::PreparedMixCompletionStatus::Rerun &&
            rerun.rerunBankIndex == 2 && rerun.discardPath == QStringLiteral("old.wav") &&
            !lifecycle.workerRunning() && !lifecycle.rerunPending(),
        "owned completion discards superseded output and returns one rerun bank");

    const auto replacement = lifecycle.request(2, 4, true);
    const auto apply = lifecycle.complete(
        replacement.generation, QStringLiteral("replacement.wav"));
    expect(replacement.status == jam2::gui::PreparedMixRequestStatus::StartWorker &&
            apply.status == jam2::gui::PreparedMixCompletionStatus::Apply &&
            apply.discardPath.isEmpty() && !lifecycle.workerRunning(),
        "latest replacement completion is the only output allowed to apply");
}

void testProjectReplacementRejectsStalePcm()
{
    jam2::gui::PreparedMixLifecycle lifecycle;
    const auto oldProject = lifecycle.request(0, 4, true);
    lifecycle.invalidateAll();
    const auto stale = lifecycle.complete(
        oldProject.generation, QStringLiteral("old-project.wav"));
    expect(stale.status == jam2::gui::PreparedMixCompletionStatus::Stale &&
            stale.discardPath == QStringLiteral("old-project.wav") &&
            !lifecycle.workerRunning() && lifecycle.revision() == 2,
        "replacing a project invalidates an in-flight old-project render");

    const auto current = lifecycle.request(0, 4, true);
    lifecycle.invalidateAll();
    const auto requestedAfterReplacement = lifecycle.request(1, 4, true);
    expect(requestedAfterReplacement.status ==
                jam2::gui::PreparedMixRequestStatus::Coalesced &&
            requestedAfterReplacement.generation == lifecycle.revision(),
        "a new-project request made during old work is retained as one rerun");
    const auto rerun = lifecycle.complete(
        current.generation, QStringLiteral("superseded-again.wav"));
    expect(rerun.status == jam2::gui::PreparedMixCompletionStatus::Rerun &&
            rerun.rerunBankIndex == 1,
        "old completion cannot publish and transfers ownership to the new-project bank");
}

void testCachePlaybackAndPathOwnership()
{
    jam2::gui::PreparedMixLifecycle lifecycle;
    lifecycle.setPlayWhenReady(true);
    PreparedMixResult invalid = validResult(4, QStringLiteral("outside.wav"));
    expect(lifecycle.cacheResult(invalid, 4).status ==
                jam2::gui::PreparedMixCacheStatus::Rejected &&
            lifecycle.failures() == 1 && !lifecycle.playWhenReady(),
        "out-of-range worker bank identity fails closed and cancels pending play");

    lifecycle.setPlayWhenReady(true);
    invalid = validResult(0, QString{});
    expect(lifecycle.cacheResult(invalid, 4).status ==
                jam2::gui::PreparedMixCacheStatus::Rejected &&
            lifecycle.failures() == 2 && !lifecycle.playWhenReady(),
        "empty output metadata cannot enter a bank cache");

    lifecycle.setPlayWhenReady(true);
    invalid = validResult(0, QStringLiteral("failed.wav"));
    invalid.error = QStringLiteral("render failed");
    expect(lifecycle.cacheResult(invalid, 4).status ==
                jam2::gui::PreparedMixCacheStatus::Failed &&
            lifecycle.failures() == 3 && !lifecycle.playWhenReady(),
        "renderer failure is counted without caching its path");

    const PreparedMixResult first = validResult(0, QStringLiteral("old-root/first.wav"));
    const PreparedMixResult second = validResult(1, QStringLiteral("old-root/second.wav"));
    expect(lifecycle.cacheResult(first, 4).status ==
                jam2::gui::PreparedMixCacheStatus::Cached &&
            lifecycle.cacheResult(second, 4).status ==
                jam2::gui::PreparedMixCacheStatus::Cached &&
            lifecycle.activateCachedBank(0, 4, true) &&
            lifecycle.active().path == first.path &&
            lifecycle.retainsPath(first.path) && lifecycle.retainsPath(second.path),
        "valid outputs are cached per bank and one cache becomes active");
    expect(!lifecycle.activateCachedBank(1, 4, false) &&
            lifecycle.active().path.isEmpty() &&
            lifecycle.takeObsoletePaths() == QSet<QString>{first.path},
        "a missing cached file clears active state and retires the prior active file");

    expect(lifecycle.activateCachedBank(0, 4, true) &&
            lifecycle.activateCachedBank(1, 4, true),
        "two existing bank caches can be adopted in sequence");
    QSet<QString> obsolete = lifecycle.takeObsoletePaths();
    expect(obsolete == QSet<QString>{first.path} &&
            lifecycle.active().path == second.path,
        "replacing an active mix reports exactly the superseded path");
    lifecycle.retainObsoletePath(first.path);
    lifecycle.relocatePaths([](const QString& path) {
        return path.isEmpty()
            ? path
            : QString(path).replace(QStringLiteral("old-root"), QStringLiteral("new-root"));
    });
    expect(lifecycle.active().path == QStringLiteral("new-root/second.wav") &&
            lifecycle.cache(0).path == QStringLiteral("new-root/first.wav") &&
            lifecycle.cache(1).path == QStringLiteral("new-root/second.wav") &&
            lifecycle.takeObsoletePaths() ==
                QSet<QString>{QStringLiteral("new-root/first.wav")},
        "managed relocation updates active, every cache, and retained cleanup paths");

    lifecycle.setPlayWhenReady(true);
    expect(lifecycle.takePlayWhenReady() && !lifecycle.takePlayWhenReady(),
        "pending playback intent is consumed exactly once");
    lifecycle.invalidateBank(0);
    expect(lifecycle.cache(0).path.isEmpty() &&
            lifecycle.takeObsoletePaths() ==
                QSet<QString>{QStringLiteral("new-root/first.wav")} &&
            lifecycle.active().path == QStringLiteral("new-root/second.wav"),
        "invalidating an inactive bank retires only its derived cache");
    const QString removed = lifecycle.clearActiveAndCache(1);
    expect(removed == QStringLiteral("new-root/second.wav") &&
            lifecycle.active().path.isEmpty() && lifecycle.cache(1).path.isEmpty() &&
            lifecycle.cache(0).path.isEmpty(),
        "discard returns the active path and clears only its bank cache");

    lifecycle.setPlayWhenReady(true);
    const auto noSources = lifecycle.request(0, 4, false);
    expect(noSources.status == jam2::gui::PreparedMixRequestStatus::NoSources &&
            lifecycle.cache(0).path.isEmpty() && !lifecycle.playWhenReady(),
        "a source-free request invalidates its bank cache and pending playback");
    lifecycle.noteMetadataFailure(QStringLiteral("metadata failure"));
    expect(lifecycle.active().error == QStringLiteral("metadata failure") &&
            lifecycle.failures() == 4,
        "post-cache metadata failure remains explicit in active diagnostics");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    testRequestAndCompletionOwnership();
    testProjectReplacementRejectsStalePcm();
    testCachePlaybackAndPathOwnership();
    if (failures != 0) {
        std::cerr << failures << " PreparedMixLifecycle checks failed\n";
        return 1;
    }
    std::cout << "PreparedMixLifecycle checks passed\n";
    return 0;
}
