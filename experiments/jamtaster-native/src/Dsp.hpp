#pragma once

#include "Wav.hpp"

#include <cstddef>
#include <vector>

namespace jamtaster::native {

struct Matrix {
    std::size_t rows = 0;
    std::size_t columns = 0;
    std::vector<float> values;

    float& at(std::size_t row, std::size_t column);
    [[nodiscard]] float at(std::size_t row, std::size_t column) const;
};

std::vector<float> resampleSinc(
    const std::vector<float>& input,
    int sourceSampleRate,
    int targetSampleRate);
AudioBuffer resampleSinc(const AudioBuffer& input, int targetSampleRate);
Matrix beatThisLogMel(const std::vector<float>& mono22050);
Matrix adtofLogFilterbank(const std::vector<float>& mono44100);
Matrix chordMiniLogCqt(const std::vector<float>& mono22050);

} // namespace jamtaster::native
