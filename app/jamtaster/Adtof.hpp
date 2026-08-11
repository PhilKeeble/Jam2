#pragma once

#include "Wav.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jamtaster::native {

struct DrumEvent {
    double time = 0.0;
    int midi = 0;
    std::string lane;
    float confidence = 0.0F;
};

struct DrumAnalysis {
    std::vector<DrumEvent> hits;
    std::size_t activationFrames = 0;
    double preprocessingSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double postprocessingSeconds = 0.0;
};

class Adtof {
public:
    Adtof(const std::filesystem::path& modelPath, int threads);
    ~Adtof();
    [[nodiscard]] DrumAnalysis analyze(
        const AudioBuffer& audio,
        const std::array<float, 5>& thresholds = {0.22F, 0.24F, 0.32F, 0.22F, 0.30F});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jamtaster::native
