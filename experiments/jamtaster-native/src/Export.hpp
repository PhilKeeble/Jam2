#pragma once

#include "Json.hpp"
#include "PipelineTypes.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace jamtaster::native {

std::string portableSlug(const std::string& name);
Json analysisJson(const Analysis& analysis, const std::vector<SectionChoice>& sections,
    const std::map<std::string, double>& timings, const std::filesystem::path& input,
    const std::string& sourceHash);
std::filesystem::path exportJamJar(const std::filesystem::path& stagingRoot,
    const std::string& displayName, const std::string& sourceHash,
    const std::map<std::string, std::filesystem::path>& stemPaths,
    const Analysis& analysis, const std::vector<SectionChoice>& choices,
    bool arrangementLoop, bool timeStretch);

} // namespace jamtaster::native
