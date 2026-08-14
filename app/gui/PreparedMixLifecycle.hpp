#pragma once

#include "PreparedMixRenderer.hpp"

#include <QSet>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>

namespace jam2::gui {

enum class PreparedMixRequestStatus {
    StartWorker,
    Coalesced,
    NoSources,
    Rejected,
};

struct PreparedMixRequestDecision {
    PreparedMixRequestStatus status = PreparedMixRequestStatus::Rejected;
    int bankIndex = -1;
    std::uint64_t generation = 0;
};

enum class PreparedMixCompletionStatus {
    Apply,
    Rerun,
    Stale,
    Rejected,
};

struct PreparedMixCompletionDecision {
    PreparedMixCompletionStatus status = PreparedMixCompletionStatus::Rejected;
    int rerunBankIndex = -1;
    QString discardPath;
};

enum class PreparedMixCacheStatus {
    Cached,
    Failed,
    Rejected,
};

struct PreparedMixCacheDecision {
    PreparedMixCacheStatus status = PreparedMixCacheStatus::Rejected;
    int bankIndex = -1;
};

// Owns the non-widget lifecycle of the prepared Section cache. Rendering and
// engine submission remain at their existing boundaries; this class makes
// request coalescing, stale completion rejection, cache identity, playback
// intent, and obsolete-path retention deterministic and directly testable.
class PreparedMixLifecycle {
public:
    static constexpr int kBankCapacity = 12;

    PreparedMixRequestDecision request(
        int bankIndex,
        int bankCount,
        bool hasSources,
        int priorityRerunBankIndex = -1) noexcept;
    PreparedMixCompletionDecision complete(
        std::uint64_t generation,
        const QString& renderedPath);
    PreparedMixCacheDecision cacheResult(
        const PreparedMixResult& result,
        int bankCount);

    const PreparedMixResult& active() const noexcept;
    const PreparedMixResult& cache(int bankIndex) const noexcept;
    bool activateCachedBank(int bankIndex, int bankCount, bool pathExists);
    QString clearActiveAndCache(int activeBankIndex) noexcept;
    void clearBank(int bankIndex) noexcept;
    void invalidateBank(int bankIndex) noexcept;
    void invalidateAll() noexcept;

    bool workerRunning() const noexcept;
    int workerBankIndex() const noexcept;
    bool rerunPending() const noexcept;
    int rerunBankIndex() const noexcept;
    std::uint64_t revision() const noexcept;

    bool playWhenReady() const noexcept;
    void setPlayWhenReady(bool play) noexcept;
    bool takePlayWhenReady() noexcept;

    std::uint64_t requests() const noexcept;
    std::uint64_t coalesced() const noexcept;
    std::uint64_t failures() const noexcept;
    void noteMetadataFailure(const QString& error);

    bool retainsPath(const QString& path) const noexcept;
    QSet<QString> takeObsoletePaths();
    void retainObsoletePath(const QString& path);
    void relocatePaths(const std::function<QString(const QString&)>& relocate);

private:
    static bool validBank(int bankIndex, int bankCount) noexcept;

    std::array<PreparedMixResult, kBankCapacity> cacheByBank_{};
    PreparedMixResult active_;
    bool workerRunning_ = false;
    int workerBankIndex_ = -1;
    std::uint64_t workerGeneration_ = 0;
    bool rerunPending_ = false;
    int rerunBankIndex_ = -1;
    std::uint64_t revision_ = 0;
    bool playWhenReady_ = false;
    std::uint64_t requests_ = 0;
    std::uint64_t coalesced_ = 0;
    std::uint64_t failures_ = 0;
    QSet<QString> obsoletePaths_;
};

} // namespace jam2::gui
