#pragma once

#include "Json.hpp"
#include "PipelineTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace jamtaster::native {

inline constexpr std::string_view kAnalysisFormat = "jamtaster-analysis-v2";

struct JamJarExport {
    std::filesystem::path path;
    Json assets = Json::array();
    Json quantization = Json::object();
    std::size_t jamjarBytes = 0;
};

std::string portableSlug(const std::string& name);
Json analysisJson(const Analysis& analysis, const std::vector<SectionChoice>& sections,
    const std::map<std::string, double>& timings, const std::filesystem::path& input,
    const std::string& sourceHash, int sampleRate, std::size_t frames, int channels,
    const Json& quantization);
JamJarExport exportJamJar(const std::filesystem::path& stagingRoot,
    const std::string& displayName, const std::string& sourceHash,
    const std::map<std::string, std::filesystem::path>& stemPaths,
    const Analysis& analysis, const std::vector<SectionChoice>& choices,
    bool arrangementLoop, bool timeStretch);

} // namespace jamtaster::native
