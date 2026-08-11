#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace jamtaster::native {

std::string sha256(std::string_view value);
std::string sha256File(const std::filesystem::path& path);

} // namespace jamtaster::native
