#include "DemucsAdapter.hpp"

#include "Dsp.hpp"
#include "demucs.hpp"

#include <Eigen/Dense>

#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace jamtaster::native {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

AudioBuffer stereo44100(const AudioBuffer& source)
{
    AudioBuffer converted = source.sampleRate == 44100 ? source : resampleSinc(source, 44100);
    if (converted.channels == 2) return converted;
    AudioBuffer stereo;
    stereo.sampleRate = 44100;
    stereo.channels = 2;
    stereo.samples.resize(converted.frames() * 2);
    for (std::size_t frame = 0; frame < converted.frames(); ++frame) {
        double sum = 0.0;
        for (int channel = 0; channel < converted.channels; ++channel) {
            sum += converted.samples[frame * static_cast<std::size_t>(converted.channels) +
                static_cast<std::size_t>(channel)];
        }
        const float mono = static_cast<float>(sum / converted.channels);
        stereo.samples[frame * 2] = mono;
        stereo.samples[frame * 2 + 1] = mono;
    }
    return stereo;
}

} // namespace

DemucsResult runDemucsEnsemble(
    const AudioBuffer& source,
    const std::vector<std::filesystem::path>& models,
    const std::filesystem::path& outputDirectory,
    int threads,
    std::uint32_t shiftSeed,
    Progress progress)
{
    if (models.empty()) throw std::runtime_error("at least one Demucs ONNX model is required");
    DemucsResult result;
    auto started = Clock::now();
    const AudioBuffer input = stereo44100(source);
    Eigen::MatrixXf matrix(2, static_cast<Eigen::Index>(input.frames()));
    for (std::size_t frame = 0; frame < input.frames(); ++frame) {
        matrix(0, static_cast<Eigen::Index>(frame)) = input.samples[frame * 2];
        matrix(1, static_cast<Eigen::Index>(frame)) = input.samples[frame * 2 + 1];
    }
    result.preprocessingSeconds = elapsed(started);

    started = Clock::now();
    Eigen::Tensor3dXf accumulated;
    int sourceCount = 0;
    for (std::size_t member = 0; member < models.size(); ++member) {
        demucsonnx::demucs_model model;
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetInterOpNumThreads(1);
        if (threads > 0) options.SetIntraOpNumThreads(threads);
        if (!demucsonnx::load_model(models[member], model, options)) {
            throw std::runtime_error("Demucs ONNX graph has incompatible outputs: " +
                models[member].string());
        }
        int shiftOffset = 0;
        auto memberOutput = demucsonnx::demucs_inference(
            model, matrix,
            [&](float amount, const std::string& message) {
                if (progress) {
                    progress((static_cast<float>(member) + amount) /
                        static_cast<float>(models.size()), message);
                }
            }, shiftSeed + static_cast<std::uint32_t>(member), shiftOffset);
        result.shiftOffsets.push_back(shiftOffset);
        if (member == 0) {
            accumulated = memberOutput;
            accumulated.setZero();
            sourceCount = model.nb_sources;
        } else {
            if (model.nb_sources != sourceCount || memberOutput.dimensions() != accumulated.dimensions()) {
                throw std::runtime_error("Demucs ensemble members do not have matching output shapes");
            }
        }
        if (models.size() == 1) {
            accumulated = memberOutput;
        } else {
            // htdemucs_ft is a four-member bag with one-hot per-source
            // weights: f7e0=drums, d123=bass, 92cf=other, 0457=vocals.
            if (models.size() != 4 || member >= static_cast<std::size_t>(sourceCount)) {
                throw std::runtime_error("htdemucs_ft requires its four ordered ensemble members");
            }
            for (int channel = 0; channel < 2; ++channel) {
                for (Eigen::Index frame = 0; frame < memberOutput.dimension(2); ++frame) {
                    accumulated(static_cast<Eigen::Index>(member), channel, frame) =
                        memberOutput(static_cast<Eigen::Index>(member), channel, frame);
                }
            }
        }
    }
    result.inferenceSeconds = elapsed(started);
    result.ensembleMembers = static_cast<int>(models.size());

    static constexpr std::array<const char*, 4> names{
        "drums.wav", "bass.wav", "other.wav", "vocals.wav"};
    if (sourceCount != static_cast<int>(names.size())) {
        throw std::runtime_error("JamTaster requires a four-source Demucs graph");
    }
    started = Clock::now();
    std::filesystem::create_directories(outputDirectory);
    for (int stem = 0; stem < sourceCount; ++stem) {
        AudioBuffer audio;
        audio.sampleRate = 44100;
        audio.channels = 1;
        audio.samples.resize(input.frames());
        for (std::size_t frame = 0; frame < input.frames(); ++frame) {
            audio.samples[frame] = 0.5F * (
                accumulated(stem, 0, static_cast<Eigen::Index>(frame)) +
                accumulated(stem, 1, static_cast<Eigen::Index>(frame)));
        }
        if (source.sampleRate != 44100) audio = resampleSinc(audio, source.sampleRate);
        // Round-trip sample-rate conversion can differ by one frame. Imported
        // stems must retain the exact source length so Jam2 lanes stay aligned.
        audio.samples.resize(source.frames(), 0.0F);
        const auto path = outputDirectory / names[static_cast<std::size_t>(stem)];
        writeWavPcm16(path, audio);
        result.stems.push_back(path);
    }
    result.writingSeconds = elapsed(started);
    return result;
}

} // namespace jamtaster::native
