#include "pcm16_wav.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::filesystem::path filesystemIoPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    const std::filesystem::path absolute = std::filesystem::absolute(path);
    const std::wstring value = absolute.wstring();
    if (value.rfind(L"\\\\?\\", 0) == 0 || value.size() < 240U) return absolute;
    if (value.rfind(L"\\\\", 0) == 0) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + value.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + value);
#else
    return path;
#endif
}

void putLe16(std::array<std::uint8_t, 48>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void putLe32(std::array<std::uint8_t, 48>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

bool writeFixture(const std::filesystem::path& path)
{
    std::array<std::uint8_t, 48> bytes{};
    std::copy_n("RIFF", 4, reinterpret_cast<char*>(bytes.data()));
    putLe32(bytes, 4, 40);
    std::copy_n("WAVEfmt ", 8, reinterpret_cast<char*>(bytes.data() + 8));
    putLe32(bytes, 16, 16);
    putLe16(bytes, 20, 1);
    putLe16(bytes, 22, 1);
    putLe32(bytes, 24, 48000);
    putLe32(bytes, 28, 96000);
    putLe16(bytes, 32, 2);
    putLe16(bytes, 34, 16);
    std::copy_n("data", 4, reinterpret_cast<char*>(bytes.data() + 36));
    putLe32(bytes, 40, 4);

    std::ofstream output(filesystemIoPath(path), std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

} // namespace

int main()
{
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "pcm16-wav";
    for (int segment = 0; segment < 6; ++segment) {
        directory /= std::string(40, static_cast<char>('a' + segment));
    }
    const std::filesystem::path fixture = directory / "fixture.wav";

    std::error_code ignored;
    std::filesystem::remove_all(filesystemIoPath(
        std::filesystem::temp_directory_path() / "pcm16-wav"), ignored);
    expect(std::filesystem::create_directories(
        filesystemIoPath(directory), ignored), "long fixture directory must be created");
    expect(!ignored, "long fixture directory creation must not report an error");
    expect(writeFixture(fixture), "long PCM16 fixture must be written");

    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(fixture);
    expect(static_cast<bool>(inspected),
        "PCM16 inspection must support the same deeply nested path on every platform");
    expect(inspected.info.channels == 1, "PCM16 fixture channel count must be inspected");
    expect(inspected.info.sample_rate == 48000, "PCM16 fixture sample rate must be inspected");
    expect(inspected.info.frames == 2, "PCM16 fixture frame count must be inspected");

    std::filesystem::remove_all(filesystemIoPath(
        std::filesystem::temp_directory_path() / "pcm16-wav"), ignored);
    if (failures == 0) std::cout << "pcm16 WAV tests passed\n";
    return failures == 0 ? 0 : 1;
}
