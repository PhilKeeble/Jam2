#pragma once

#include <filesystem>

namespace jamtaster::native {

// MSVC's stream constructors can still meet the legacy MAX_PATH boundary even
// when std::filesystem created the parent successfully. Preserve JamTaster's
// hash-owned directory layout by using an explicit extended-length path for IO.
inline std::filesystem::path filesystemIoPath(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const std::filesystem::path absolute = std::filesystem::absolute(path);
    std::wstring value = absolute.wstring();
    if (value.rfind(L"\\\\?\\", 0) == 0 || value.size() < 240) return absolute;
    if (value.rfind(L"\\\\", 0) == 0)
        return std::filesystem::path(L"\\\\?\\UNC\\" + value.substr(2));
    return std::filesystem::path(L"\\\\?\\" + value);
#else
    return path;
#endif
}

} // namespace jamtaster::native
