#include "PreparedMixLifecycle.hpp"

#include <utility>

namespace jam2::gui {

bool PreparedMixLifecycle::validBank(int bankIndex, int bankCount) noexcept
{
    return bankCount > 0 && bankCount <= kBankCapacity &&
        bankIndex >= 0 && bankIndex < bankCount;
}

PreparedMixRequestDecision PreparedMixLifecycle::request(
    int bankIndex,
    int bankCount,
    bool hasSources,
    int priorityRerunBankIndex) noexcept
{
    if (!validBank(bankIndex, bankCount)) return {};

    ++requests_;
    const std::uint64_t generation = ++revision_;
    if (workerRunning_) {
        rerunPending_ = true;
        rerunBankIndex_ = validBank(priorityRerunBankIndex, bankCount)
            ? priorityRerunBankIndex
            : bankIndex;
        ++coalesced_;
        return {
            PreparedMixRequestStatus::Coalesced,
            bankIndex,
            generation};
    }
    if (!hasSources) {
        clearBank(bankIndex);
        playWhenReady_ = false;
        return {
            PreparedMixRequestStatus::NoSources,
            bankIndex,
            generation};
    }

    workerRunning_ = true;
    workerBankIndex_ = bankIndex;
    workerGeneration_ = generation;
    return {
        PreparedMixRequestStatus::StartWorker,
        bankIndex,
        generation};
}

PreparedMixCompletionDecision PreparedMixLifecycle::complete(
    std::uint64_t generation,
    const QString& renderedPath)
{
    if (!workerRunning_ || generation != workerGeneration_) {
        return {
            PreparedMixCompletionStatus::Rejected,
            -1,
            renderedPath};
    }

    workerRunning_ = false;
    workerBankIndex_ = -1;
    workerGeneration_ = 0;
    if (rerunPending_) {
        const int rerunBank = rerunBankIndex_;
        rerunPending_ = false;
        rerunBankIndex_ = -1;
        return {
            PreparedMixCompletionStatus::Rerun,
            rerunBank,
            renderedPath};
    }
    if (generation != revision_) {
        return {
            PreparedMixCompletionStatus::Stale,
            -1,
            renderedPath};
    }
    return {PreparedMixCompletionStatus::Apply, -1, {}};
}

PreparedMixCacheDecision PreparedMixLifecycle::cacheResult(
    const PreparedMixResult& result,
    int bankCount)
{
    if (!validBank(result.bankIndex, bankCount)) {
        ++failures_;
        playWhenReady_ = false;
        return {PreparedMixCacheStatus::Rejected, -1};
    }
    if (!result.error.isEmpty()) {
        ++failures_;
        playWhenReady_ = false;
        return {PreparedMixCacheStatus::Failed, result.bankIndex};
    }
    if (result.path.trimmed().isEmpty() || result.frames <= 0 ||
        result.sampleRate <= 0 || result.fileBytes <= 0) {
        ++failures_;
        playWhenReady_ = false;
        return {PreparedMixCacheStatus::Rejected, result.bankIndex};
    }
    cacheByBank_[static_cast<std::size_t>(result.bankIndex)] = result;
    return {PreparedMixCacheStatus::Cached, result.bankIndex};
}

const PreparedMixResult& PreparedMixLifecycle::active() const noexcept
{
    return active_;
}

const PreparedMixResult& PreparedMixLifecycle::cache(int bankIndex) const noexcept
{
    static const PreparedMixResult empty;
    if (bankIndex < 0 || bankIndex >= kBankCapacity) return empty;
    return cacheByBank_[static_cast<std::size_t>(bankIndex)];
}

bool PreparedMixLifecycle::activateCachedBank(
    int bankIndex,
    int bankCount,
    bool pathExists)
{
    if (!validBank(bankIndex, bankCount)) return false;
    const PreparedMixResult& cached = cacheByBank_[static_cast<std::size_t>(bankIndex)];
    if (!pathExists || cached.path.trimmed().isEmpty() ||
        !cached.error.isEmpty()) {
        if (!active_.path.isEmpty()) obsoletePaths_.insert(active_.path);
        active_ = {};
        return false;
    }
    if (!active_.path.isEmpty() && active_.path != cached.path) {
        obsoletePaths_.insert(active_.path);
    }
    active_ = cached;
    return true;
}

QString PreparedMixLifecycle::clearActiveAndCache(int activeBankIndex) noexcept
{
    const QString path = active_.path;
    active_ = {};
    obsoletePaths_.remove(path);
    if (activeBankIndex >= 0 && activeBankIndex < kBankCapacity) {
        cacheByBank_[static_cast<std::size_t>(activeBankIndex)] = {};
    }
    return path;
}

void PreparedMixLifecycle::clearBank(int bankIndex) noexcept
{
    if (bankIndex < 0 || bankIndex >= kBankCapacity) return;
    PreparedMixResult& cached = cacheByBank_[static_cast<std::size_t>(bankIndex)];
    if (!cached.path.isEmpty() && cached.path != active_.path) {
        obsoletePaths_.insert(cached.path);
    }
    cached = {};
}

void PreparedMixLifecycle::invalidateBank(int bankIndex) noexcept
{
    if (bankIndex < 0 || bankIndex >= kBankCapacity) return;
    clearBank(bankIndex);
    if (workerRunning_ && workerBankIndex_ == bankIndex) ++revision_;
}

void PreparedMixLifecycle::invalidateAll() noexcept
{
    ++revision_;
    for (int bankIndex = 0; bankIndex < kBankCapacity; ++bankIndex) {
        clearBank(bankIndex);
    }
    rerunPending_ = false;
    rerunBankIndex_ = -1;
}

bool PreparedMixLifecycle::workerRunning() const noexcept { return workerRunning_; }
int PreparedMixLifecycle::workerBankIndex() const noexcept { return workerBankIndex_; }
bool PreparedMixLifecycle::rerunPending() const noexcept { return rerunPending_; }
int PreparedMixLifecycle::rerunBankIndex() const noexcept { return rerunBankIndex_; }
std::uint64_t PreparedMixLifecycle::revision() const noexcept { return revision_; }

bool PreparedMixLifecycle::playWhenReady() const noexcept { return playWhenReady_; }
void PreparedMixLifecycle::setPlayWhenReady(bool play) noexcept { playWhenReady_ = play; }

bool PreparedMixLifecycle::takePlayWhenReady() noexcept
{
    const bool play = playWhenReady_;
    playWhenReady_ = false;
    return play;
}

std::uint64_t PreparedMixLifecycle::requests() const noexcept { return requests_; }
std::uint64_t PreparedMixLifecycle::coalesced() const noexcept { return coalesced_; }
std::uint64_t PreparedMixLifecycle::failures() const noexcept { return failures_; }

void PreparedMixLifecycle::noteMetadataFailure(const QString& error)
{
    active_.error = error;
    ++failures_;
    playWhenReady_ = false;
}

bool PreparedMixLifecycle::retainsPath(const QString& path) const noexcept
{
    if (path.isEmpty()) return false;
    if (active_.path == path) return true;
    for (const PreparedMixResult& cached : cacheByBank_) {
        if (cached.path == path) return true;
    }
    return false;
}

QSet<QString> PreparedMixLifecycle::takeObsoletePaths()
{
    return std::exchange(obsoletePaths_, {});
}

void PreparedMixLifecycle::retainObsoletePath(const QString& path)
{
    if (!path.isEmpty() && path != active_.path) obsoletePaths_.insert(path);
}

void PreparedMixLifecycle::relocatePaths(
    const std::function<QString(const QString&)>& relocate)
{
    if (!relocate) return;
    active_.path = relocate(active_.path);
    for (PreparedMixResult& cached : cacheByBank_) {
        cached.path = relocate(cached.path);
    }
    QSet<QString> relocated;
    for (const QString& path : std::as_const(obsoletePaths_)) {
        const QString moved = relocate(path);
        if (!moved.isEmpty() && moved != active_.path) relocated.insert(moved);
    }
    obsoletePaths_ = std::move(relocated);
}

} // namespace jam2::gui
