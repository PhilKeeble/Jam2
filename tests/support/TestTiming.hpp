#pragma once

#include <chrono>

namespace jam2::test {

// A deadman bounds a genuinely stuck process; it is never part of a product
// acceptance rule and must not vary with the host or operating system.
inline constexpr std::chrono::milliseconds deadmanTimeout(
    std::chrono::milliseconds timeout) noexcept
{
    return timeout;
}

} // namespace jam2::test
