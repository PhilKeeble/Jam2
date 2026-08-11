#pragma once

#include "Wav.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jamtaster::native {

struct ChordSegment {
    double start = 0.0;
    double end = 0.0;
    std::string label;
    float confidenceMargin = 0.0F;
};

struct ChordAnalysis {
    std::vector<ChordSegment> chords;
    std::size_t featureFrames = 0;
    double preprocessingSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double postprocessingSeconds = 0.0;
};

class ChordMini {
public:
    ChordMini(const std::filesystem::path& modelPath, int threads);
    ~ChordMini();
    [[nodiscard]] ChordAnalysis analyze(const AudioBuffer& audio);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jamtaster::native
