#pragma once

#include <QtGlobal>

#include <chrono>
#include <limits>

namespace jam2::test {

inline int timeoutScale() noexcept
{
    bool valid = false;
    const int scale = qEnvironmentVariableIntValue("JAM2_TEST_TIMEOUT_SCALE", &valid);
    return valid && scale >= 1 && scale <= 10 ? scale : 1;
}

inline std::chrono::milliseconds scaledTimeout(
    std::chrono::milliseconds timeout) noexcept
{
    const auto scale = timeoutScale();
    const auto maximum = std::chrono::milliseconds::max().count();
    if (timeout.count() > maximum / scale) {
        return std::chrono::milliseconds::max();
    }
    return timeout * scale;
}

} // namespace jam2::test
