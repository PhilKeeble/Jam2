#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace jam2::wav {

inline constexpr std::uint64_t kDefaultMaxFileBytes = 512ULL * 1024ULL * 1024ULL;

struct Pcm16Info {
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t block_align = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_bytes = 0;
    std::uint64_t frames = 0;
};

struct InspectResult {
    Pcm16Info info;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

// Strictly inspects a bounded, integer PCM16 RIFF/WAVE file. The parser performs
// no whole-file allocation and rejects duplicate/truncated structural chunks,
// inconsistent byte rates/block alignment, trailing bytes, and unchecked sizes.
InspectResult inspect_pcm16_file(
    const std::filesystem::path& path,
    std::uint64_t max_file_bytes = kDefaultMaxFileBytes) noexcept;

} // namespace jam2::wav
