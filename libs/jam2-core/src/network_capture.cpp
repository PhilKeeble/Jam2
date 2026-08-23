#include "audio_device.hpp"
#include "udp_socket.hpp"

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
        } else {
            control.network_capture_wake_frames.store(0, std::memory_order_relaxed);
            control.network_capture_wake_signal.store(nullptr, std::memory_order_release);
        }
        control.network_capture_enabled.store(requested_enabled, std::memory_order_relaxed);
        control.network_capture_generation_applied.store(requested, std::memory_order_release);
    }
    return control.network_capture_enabled.load(std::memory_order_acquire) &&
        requested == control.network_capture_generation_applied.load(std::memory_order_acquire);
}

std::size_t push_network_capture_callback(
    StreamControl& control,
    MonoRingBuffer& capture_ring,
    std::span<const std::int32_t> input,
    std::uint64_t ready_time_us) noexcept
{
    const std::size_t pushed = capture_ring.push(input);
    const std::size_t wake_frames =
        control.network_capture_wake_frames.load(std::memory_order_relaxed);
    RealtimeWakeSignal* const wake_signal =
        control.network_capture_wake_signal.load(std::memory_order_acquire);
    if (pushed > 0 && wake_signal != nullptr && wake_frames > 0 &&
        capture_ring.available_read() >= wake_frames) {
        wake_signal->signal(ready_time_us);
    }
    return pushed;
}

} // namespace jam2::audio
