#pragma once

#include "Wav.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jamtaster::native {

struct DemucsResult {
    std::vector<std::filesystem::path> stems;
    double preprocessingSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double writingSeconds = 0.0;
    int ensembleMembers = 0;
    std::vector<int> shiftOffsets;
};

using Progress = std::function<void(float, const std::string&)>;

DemucsResult runDemucsEnsemble(
    const AudioBuffer& source,
    const std::vector<std::filesystem::path>& models,
    const std::filesystem::path& outputDirectory,
    int threads,
    std::uint32_t shiftSeed,
    Progress progress);

} // namespace jamtaster::native
