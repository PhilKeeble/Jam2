#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jam2::audio {

inline constexpr std::size_t kMaximumInputSources = 16;
inline constexpr std::size_t kMaximumSourceInputChannels = 2;
inline constexpr std::size_t kNoInputChannel = static_cast<std::size_t>(-1);
inline constexpr std::size_t kCombinedInputSources = static_cast<std::size_t>(-1);

enum class InputSourceKind : std::uint8_t {
    Audio,
    MidiInstrument,
};

struct InputSourceRenderRequest {
    std::array<const std::int32_t*, kMaximumSourceInputChannels> inputs{};
    std::size_t input_channels = 0;
    std::size_t frames = 0;
    std::uint64_t engine_frame = 0;
    double sample_rate = 0.0;
};

// Implemented by app/pluginhost. Instances connected to a running router must
// outlive that audio stream. The callback never owns or destroys a renderer.
class InputSourceRenderer {
public:
    virtual ~InputSourceRenderer() = default;
    virtual bool render_mono(
        const InputSourceRenderRequest& request,
        std::span<std::int32_t> output) noexcept = 0;
};

struct InputSourceConfiguration {
    InputSourceKind kind = InputSourceKind::Audio;
    std::size_t first_channel = kNoInputChannel;
    std::size_t second_channel = kNoInputChannel;
    int level_ppm = 1000000;
    bool enabled = false;
    InputSourceRenderer* renderer = nullptr;
};

struct InputSourceRouterStats {
    std::size_t configured_sources = 0;
    std::uint64_t rendered_blocks = 0;
    std::uint64_t renderer_failures = 0;
    std::uint64_t invalid_configurations = 0;
    int peak_ppm = 0;
};

class InputSourceRouter final {
public:
    InputSourceRouter(std::size_t maximum_frames, std::size_t physical_channels);

    bool configure(std::size_t slot, const InputSourceConfiguration& source) noexcept;
    bool set_enabled(std::size_t slot, bool enabled) noexcept;
    bool set_level(std::size_t slot, int level_ppm) noexcept;
    void clear(std::size_t slot) noexcept;
    void set_recording_source(std::size_t slot) noexcept;
    // Called only by the device callback immediately after process(). Returns
    // false when recording should use the combined canonical output.
    bool copy_recording_source(
        std::size_t frames, std::span<std::int32_t> output) const noexcept;

    // physical_inputs contains one planar Int32LSB pointer for every selected
    // engine channel. Missing pointers are treated as silence. The result is
    // always one canonical mono block.
    bool process(
        std::span<const std::int32_t* const> physical_inputs,
        std::size_t frames,
        std::uint64_t engine_frame,
        double sample_rate,
        std::span<std::int32_t> mono_output) noexcept;

    InputSourceRouterStats stats() const noexcept;
    std::size_t maximum_frames() const noexcept { return maximum_frames_; }
    std::size_t physical_channels() const noexcept { return physical_channels_; }

private:
    struct Slot {
        std::atomic<InputSourceKind> kind{InputSourceKind::Audio};
        std::atomic<std::size_t> first_channel{kNoInputChannel};
        std::atomic<std::size_t> second_channel{kNoInputChannel};
        std::atomic<int> level_ppm{1000000};
        std::atomic<InputSourceRenderer*> renderer{nullptr};
        std::atomic<bool> configured{false};
        std::atomic<bool> enabled{false};
    };

    bool valid(const InputSourceConfiguration& source) const noexcept;

    std::size_t maximum_frames_ = 0;
    std::size_t physical_channels_ = 0;
    std::array<Slot, kMaximumInputSources> slots_{};
    std::vector<std::int32_t> source_scratch_;
    std::vector<std::int64_t> mix_scratch_;
    std::vector<std::int32_t> recording_scratch_;
    std::atomic<std::size_t> recording_slot_{kCombinedInputSources};
    std::atomic<bool> recording_source_ready_{false};
    std::atomic<std::uint64_t> rendered_blocks_{0};
    std::atomic<std::uint64_t> renderer_failures_{0};
    std::atomic<std::uint64_t> invalid_configurations_{0};
    std::atomic<int> peak_ppm_{0};
};

} // namespace jam2::audio
