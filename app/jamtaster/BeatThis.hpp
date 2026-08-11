#pragma once

#include "OnnxModel.hpp"
#include "Wav.hpp"

#include <filesystem>
#include <vector>

namespace jamtaster::native {

struct BeatAnalysis {
    std::vector<float> beats;
    std::vector<float> downbeats;
    std::vector<float> beatLogits;
    std::vector<float> downbeatLogits;
    double preprocessingSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double postprocessingSeconds = 0.0;
};

class BeatThis {
public:
    BeatThis(const std::filesystem::path& modelPath, int threads);
    BeatAnalysis analyze(const AudioBuffer& audio);

private:
    static std::vector<float> peakTimes(const std::vector<float>& logits);
    OnnxModel model_;
};

} // namespace jamtaster::native
