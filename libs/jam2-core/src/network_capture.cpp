#include "audio_device.hpp"

namespace jam2::audio {

bool prepare_network_capture_callback(
    StreamControl& control,
    MonoRingBuffer& capture_ring,
    std::uint64_t callback_frame) noexcept
{
    const std::uint64_t requested =
        control.network_capture_generation_requested.load(std::memory_order_acquire);
    const std::uint64_t applied =
        control.network_capture_generation_applied.load(std::memory_order_relaxed);
    if (requested != applied) {
        const bool requested_enabled =
            control.network_capture_requested_enabled.load(std::memory_order_acquire);
        control.network_capture_stale_frames_discarded.fetch_add(
            capture_ring.discard_all(),
            std::memory_order_relaxed);
        if (requested_enabled) {
            control.network_capture_epoch_frame.store(callback_frame, std::memory_order_relaxed);
        }
        control.network_capture_enabled.store(requested_enabled, std::memory_order_relaxed);
        control.network_capture_generation_applied.store(requested, std::memory_order_release);
    }
    return control.network_capture_enabled.load(std::memory_order_acquire) &&
        requested == control.network_capture_generation_applied.load(std::memory_order_acquire);
}

} // namespace jam2::audio
