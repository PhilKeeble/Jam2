#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace jamtaster::native {

struct AudioBuffer {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;

    [[nodiscard]] std::size_t frames() const noexcept;
    [[nodiscard]] std::vector<float> mono() const;
};

AudioBuffer readWav(const std::filesystem::path& path);
void writeWavPcm16(const std::filesystem::path& path, const AudioBuffer& audio);
AudioBuffer mixMono(const AudioBuffer& first, const AudioBuffer& second);
AudioBuffer cropAudio(const AudioBuffer& audio, double startSeconds, double endSeconds);
AudioBuffer stretchAudio(const AudioBuffer& audio, std::size_t outputFrames);
AudioBuffer stretchAudioAnchored(
    const AudioBuffer& audio,
    const std::vector<std::size_t>& inputBoundaries,
    const std::vector<std::size_t>& outputBoundaries);

} // namespace jamtaster::native
