#include "grid_assignment_delivery.hpp"
#include <algorithm>
#include <limits>

namespace jam2 {
void GridAssignmentDelivery::begin(std::uint64_t revision, std::uint64_t token) noexcept
{
    reset();
    if (revision != 0 && token != 0) { revision_ = revision; token_ = token; }
}
bool GridAssignmentDelivery::due(std::uint64_t now_us) const noexcept
{
    return pending() && now_us >= next_retry_us_;
}
void GridAssignmentDelivery::attempted(std::uint64_t now_us) noexcept
{
    if (!pending()) return;
    ++attempts_;
    next_retry_us_ = now_us > (std::numeric_limits<std::uint64_t>::max)() - retry_us_
        ? (std::numeric_limits<std::uint64_t>::max)() : now_us + retry_us_;
    retry_us_ = (std::min)(retry_us_ * 2, kMaximumRetryUs);
}
bool GridAssignmentDelivery::acknowledge(std::uint64_t revision, std::uint64_t token) noexcept
{
    if (revision_ == 0 || attempts_ == 0 || revision != revision_ || token != token_) return false;
    applied_ = true;
    return true;
}
} // namespace jam2
