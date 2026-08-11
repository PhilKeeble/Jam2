#pragma once

#include "Wav.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace jamtaster::native {

struct PitchNote {
    double start = 0.0;
    double end = 0.0;
    int midi = 0;
    int velocity = 0;
    float confidence = 0.0F;
};

struct PitchAnalysis {
    std::vector<PitchNote> notes;
    std::size_t activationFrames = 0;
    double preprocessingSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double postprocessingSeconds = 0.0;
};

class BasicPitch {
public:
    BasicPitch(const std::filesystem::path& modelPath, int threads);
    ~BasicPitch();
    [[nodiscard]] PitchAnalysis analyze(
        const AudioBuffer& audio, int minimumMidi = 28, int maximumMidi = 64);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jamtaster::native
