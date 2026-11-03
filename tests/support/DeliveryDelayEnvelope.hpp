#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace jam2::test {
// Delay to the next delivery-window boundary. This models packet coalescing
// without loss, source-clock changes, or unbounded packet traces.
inline double deliveryBatchDelayMs(double elapsedMs,double delayMs,double intervalMs) noexcept {
    return intervalMs>0 ? (std::floor((elapsedMs+delayMs)/intervalMs)+1)*intervalMs-elapsedMs : delayMs;
}
struct DeliveryDelayPoint { double atMs = 0, delayMs = 0; };
// Test-network queueing delay, additional to the direction's fixed delay/jitter.
// Linear segments model slowing delivery followed by catch-up without inventing loss.
struct DeliveryDelayEnvelope {
    std::vector<DeliveryDelayPoint> points;
    double repeatMs = 0;
    bool valid() const noexcept {
        if (!std::isfinite(repeatMs) || repeatMs < 0 || repeatMs > 86400000) return false;
        double previous = -1;
        for (const auto& p : points) {
            if (!std::isfinite(p.atMs) || !std::isfinite(p.delayMs) ||
                p.atMs < 0 || p.atMs > 86400000 || p.atMs <= previous ||
                p.delayMs < 0 || p.delayMs > 10000) return false;
            previous = p.atMs;
        }
        return points.empty() || (points.front().atMs == 0 &&
            (repeatMs == 0 || (repeatMs > points.back().atMs &&
                points.front().delayMs == 0 && points.back().delayMs == 0)));
    }
    double delayMs(double elapsedMs) const noexcept {
        if (points.empty()) return 0;
        elapsedMs = std::max(0., elapsedMs);
        if (repeatMs > 0) elapsedMs = std::fmod(elapsedMs, repeatMs);
        for (std::size_t i = 1; i < points.size(); ++i) {
            if (elapsedMs <= points[i].atMs) {
                const auto& a = points[i-1]; const auto& b = points[i];
                return a.delayMs + (b.delayMs-a.delayMs) * (elapsedMs-a.atMs)/(b.atMs-a.atMs);
            }
        }
        return points.back().delayMs;
    }
};
} // namespace jam2::test
