#include "pcm16_wav.hpp"
#include "runtime_limits.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace jam2::wav {
namespace {

std::uint16_t read_le16(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t read_le32(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8U) |
        (static_cast<std::uint32_t>(data[2]) << 16U) |
        (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool read_exact(std::ifstream& file, std::uint64_t offset, void* destination, std::size_t size)
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    file.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return file.gcount() == static_cast<std::streamsize>(size);
}

InspectResult failure(std::string message)
{
    InspectResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

InspectResult inspect_pcm16_file(const std::filesystem::path& path, std::uint64_t max_file_bytes) noexcept
{
    try {
        std::error_code size_error;
        const std::uint64_t file_size = std::filesystem::file_size(path, size_error);
        if (size_error) {
            return failure("cannot determine WAV file size");
        }
        if (file_size < 44U) {
            return failure("WAV is shorter than the minimum PCM16 header");
        }
        if (max_file_bytes == 0 || file_size > max_file_bytes) {
            return failure("WAV exceeds the configured file-size limit");
        }

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return failure("cannot open WAV file");
        }

        std::array<std::uint8_t, 12> riff{};
        if (!read_exact(file, 0, riff.data(), riff.size()) ||
            std::string_view(reinterpret_cast<const char*>(riff.data()), 4) != "RIFF" ||
            std::string_view(reinterpret_cast<const char*>(riff.data() + 8), 4) != "WAVE") {
            return failure("file is not RIFF/WAVE");
        }
        const std::uint64_t declared_file_size = static_cast<std::uint64_t>(read_le32(riff.data() + 4)) + 8U;
        if (declared_file_size != file_size) {
            return failure("RIFF size does not match the file size");
        }

        InspectResult result;
        bool have_fmt = false;
        bool have_data = false;
        std::size_t chunk_count = 0;
        std::uint64_t offset = 12U;
        while (offset < file_size) {
            constexpr std::size_t max_chunk_count = 1024;
            if (++chunk_count > max_chunk_count) {
                return failure("WAV exceeds the chunk-count limit");
            }
            if (file_size - offset < 8U) {
                return failure("WAV has a truncated chunk header");
            }
            std::array<std::uint8_t, 8> chunk{};
            if (!read_exact(file, offset, chunk.data(), chunk.size())) {
                return failure("cannot read WAV chunk header");
            }
            const std::uint32_t chunk_size = read_le32(chunk.data() + 4);
            const std::uint64_t payload = offset + 8U;
            const std::uint64_t padded_size = static_cast<std::uint64_t>(chunk_size) + (chunk_size & 1U);
            if (padded_size > file_size - payload) {
                return failure("WAV chunk extends beyond the file");
            }

            const std::string_view id(reinterpret_cast<const char*>(chunk.data()), 4);
            if (id == "fmt ") {
                if (have_fmt) {
                    return failure("WAV contains more than one format chunk");
                }
                if (chunk_size != 16U) {
                    return failure("PCM16 WAV format chunk must be exactly 16 bytes");
                }
                std::array<std::uint8_t, 16> format{};
                if (!read_exact(file, payload, format.data(), format.size())) {
                    return failure("cannot read WAV format chunk");
                }
                const std::uint16_t audio_format = read_le16(format.data());
                result.info.channels = read_le16(format.data() + 2);
                result.info.sample_rate = read_le32(format.data() + 4);
                const std::uint32_t byte_rate = read_le32(format.data() + 8);
                result.info.block_align = read_le16(format.data() + 12);
                const std::uint16_t bits_per_sample = read_le16(format.data() + 14);
                if (audio_format != 1U || bits_per_sample != 16U) {
                    return failure("WAV must be integer PCM16");
                }
                if (result.info.channels == 0U || result.info.channels > 8U) {
                    return failure("WAV channel count is outside the supported range 1..8");
                }
                if (!limits::valid_sample_rate(static_cast<int>(result.info.sample_rate))) {
                    return failure("WAV sample rate is outside the supported range");
                }
                const std::uint32_t expected_align = static_cast<std::uint32_t>(result.info.channels) * 2U;
                const std::uint64_t expected_rate = static_cast<std::uint64_t>(result.info.sample_rate) * expected_align;
                if (result.info.block_align != expected_align ||
                    expected_rate > std::numeric_limits<std::uint32_t>::max() ||
                    byte_rate != static_cast<std::uint32_t>(expected_rate)) {
                    return failure("WAV byte rate or block alignment is inconsistent");
                }
                have_fmt = true;
            } else if (id == "data") {
                if (have_data) {
                    return failure("WAV contains more than one data chunk");
                }
                result.info.data_offset = payload;
                result.info.data_bytes = chunk_size;
                have_data = true;
            }
            offset = payload + padded_size;
        }

        if (!have_fmt || !have_data) {
            return failure("WAV is missing its format or data chunk");
        }
        if (result.info.data_bytes == 0U) {
            return failure("WAV data chunk is empty");
        }
        if (result.info.data_bytes % result.info.block_align != 0U) {
            return failure("WAV data size is not frame aligned");
        }
        result.info.frames = result.info.data_bytes / result.info.block_align;
        return result;
    } catch (const std::exception& error) {
        return failure(std::string("WAV inspection failed: ") + error.what());
    } catch (...) {
        return failure("WAV inspection failed");
    }
}

} // namespace jam2::wav
