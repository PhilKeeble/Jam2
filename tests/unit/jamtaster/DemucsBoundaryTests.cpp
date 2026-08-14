#include "DemucsAdapter.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

jamtaster::native::AudioBuffer sourceFixture()
{
    jamtaster::native::AudioBuffer audio;
    audio.sampleRate = 22050;
    audio.channels = 1;
    audio.samples.resize(5513);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < audio.frames(); ++frame) {
        const double time = static_cast<double>(frame) / audio.sampleRate;
        audio.samples[frame] = static_cast<float>(
            0.35 * std::sin(2.0 * pi * 110.0 * time) +
            0.20 * std::sin(2.0 * pi * 329.627556 * time));
    }
    return audio;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: jam2_jamtaster_demucs_tests <model-directory>");
        }
        const std::filesystem::path modelsRoot = argv[1];
        std::vector<std::filesystem::path> models;
        for (int index = 0; index < 4; ++index) {
            const auto path = modelsRoot /
                ("htdemucs_ft_" + std::to_string(index) + ".onnx");
            require(std::filesystem::is_regular_file(path),
                "staged Demucs ensemble member is missing");
            models.push_back(path);
        }

        const auto output = std::filesystem::temp_directory_path() /
            "jam2-demucs-four-member-boundary";
        std::error_code ignored;
        std::filesystem::remove_all(output, ignored);
        const auto source = sourceFixture();
        std::vector<float> progress;
        std::vector<std::string> messages;
        const auto result = jamtaster::native::runDemucsEnsemble(
            source, models, output, 1, 0x12345678U,
            [&](float amount, const std::string& message) {
                progress.push_back(amount);
                messages.push_back(message);
            });

        require(result.ensembleMembers == 4 && result.stems.size() == 4 &&
                result.shiftOffsets.size() == 4 &&
                result.preprocessingSeconds >= 0.0 &&
                result.inferenceSeconds > 0.0 && result.writingSeconds >= 0.0,
            "Demucs reports its complete four-member stage result");
        bool exactProgress = progress.size() == 4;
        for (std::size_t index = 0; index < progress.size(); ++index) {
            exactProgress = exactProgress &&
                std::abs(progress[index] - static_cast<float>(index + 1) / 4.0F) < 1.0e-6F;
        }
        require(progress.size() == 4 && messages.size() == progress.size() &&
                std::is_sorted(progress.begin(), progress.end()) &&
                progress.front() > 0.0F && progress.back() == 1.0F &&
                exactProgress &&
                std::all_of(messages.begin(), messages.end(), [](const std::string& message) {
                    return message == "Segment inference complete";
                }),
            "Demucs reports one monotonic segment completion per ensemble member");
        std::vector<int> expectedOffsets;
        for (std::uint32_t member = 0; member < 4; ++member) {
            std::mt19937 generator(0x12345678U + member);
            std::uniform_int_distribution<> distribution(0, 22050);
            expectedOffsets.push_back(distribution(generator));
        }
        require(result.shiftOffsets == expectedOffsets &&
                std::adjacent_find(result.shiftOffsets.begin(), result.shiftOffsets.end(),
                    std::not_equal_to<int>()) != result.shiftOffsets.end(),
            "Demucs shifts are deterministic, bounded, and member-specific");

        static const std::array<std::string, 4> names{
            "drums.wav", "bass.wav", "other.wav", "vocals.wav"};
        std::vector<double> reconstructed(source.frames(), 0.0);
        for (std::size_t index = 0; index < result.stems.size(); ++index) {
            require(result.stems[index].filename() == names[index] &&
                    std::filesystem::is_regular_file(result.stems[index]),
                "Demucs publishes the expected ordered stem path");
            const auto stem = jamtaster::native::readWav(result.stems[index]);
            double energy = 0.0;
            for (const float sample : stem.samples) {
                require(std::isfinite(sample), "Demucs stem samples must be finite");
                energy += static_cast<double>(sample) * sample;
            }
            require(stem.sampleRate == source.sampleRate && stem.channels == 1 &&
                    stem.frames() == source.frames() && energy > 1.0e-8,
                "Demucs stem retains exact source rate/length and nonzero audio");
            for (std::size_t frame = 0; frame < stem.frames(); ++frame) {
                reconstructed[frame] += stem.samples[frame];
            }
        }
        double dot = 0.0;
        double sourceSquare = 0.0;
        double reconstructedSquare = 0.0;
        for (std::size_t frame = 0; frame < source.frames(); ++frame) {
            dot += source.samples[frame] * reconstructed[frame];
            sourceSquare += source.samples[frame] * source.samples[frame];
            reconstructedSquare += reconstructed[frame] * reconstructed[frame];
        }
        const double correlation = dot /
            std::sqrt(sourceSquare * reconstructedSquare);
        require(std::isfinite(correlation) && correlation > 0.5,
            "the sum of separated stems remains correlated with the source mixture");

        std::filesystem::remove_all(output, ignored);
        std::cout << "JamTaster four-member Demucs boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster four-member Demucs boundary test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
