#include "Export.hpp"
#include "Hash.hpp"
#include "Json.hpp"
#include "Pipeline.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Operation>
void requireThrows(Operation operation, const std::string& message)
{
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

jamtaster::native::AudioBuffer syntheticMusic(
    int sampleRate, double seconds, int channels = 1)
{
    jamtaster::native::AudioBuffer audio;
    audio.sampleRate = sampleRate;
    audio.channels = channels;
    const std::size_t frames = static_cast<std::size_t>(
        std::llround(sampleRate * seconds));
    audio.samples.assign(frames * static_cast<std::size_t>(channels), 0.0F);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) / sampleRate;
        float value = static_cast<float>(
            0.24 * std::sin(2.0 * pi * 110.0 * time) +
            0.17 * std::sin(2.0 * pi * 164.813778 * time) +
            0.11 * std::sin(2.0 * pi * 220.0 * time));
        const double beatPhase = std::fmod(time, 0.5);
        if (beatPhase < 0.012) {
            value += static_cast<float>(0.65 * (1.0 - beatPhase / 0.012) *
                std::sin(2.0 * pi * 1400.0 * time));
        }
        value = std::clamp(value, -1.0F, 1.0F);
        for (int channel = 0; channel < channels; ++channel) {
            audio.samples[frame * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(channel)] = value * (1.0F - 0.1F * channel);
        }
    }
    return audio;
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not read " + path.string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path& path, const std::string& value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    if (!output) throw std::runtime_error("could not write " + path.string());
}

} // namespace

int main(int argc, char** argv)
{
    try {
        using namespace jamtaster::native;
        if (argc != 2) {
            throw std::runtime_error("usage: jam2_jamtaster_pipeline_tests <model-directory>");
        }
        const std::filesystem::path models = argv[1];
        const auto root = std::filesystem::temp_directory_path() /
            "pipeline";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);

        requireThrows([] { (void)runPipeline({}); },
            "pipeline rejects absent input, project, and model roots");
        const auto inputPath = root / "input.wav";
        writeWavPcm16(inputPath, syntheticMusic(22050, 6.0));

        PipelineOptions options;
        options.input = inputPath;
        options.projectRoot = root / "project";
        options.modelsRoot = models;
        options.name = "Pipeline Boundary";
        options.threads = 1;
        options.seed = 1234;
        options.requestedBpm = 120.0;
        options.requestedMeter = 4;
        options.reuseStems = true;
        options.timeStretch = false;
        options.arrangementLoop = true;
        options.exportJamJar = true;

        PipelineOptions missingModels = options;
        missingModels.modelsRoot = root / "missing-models";
        requireThrows([&] { (void)runPipeline(missingModels); },
            "pipeline rejects an incomplete staged model bundle");
        PipelineOptions longName = options;
        longName.name.assign(513, 'x');
        requireThrows([&] { (void)runPipeline(longName); },
            "pipeline rejects a display name beyond 512 characters");
        const std::string sourceHash = sha256File(inputPath);
        const auto sourceRoot = options.projectRoot / "analysis" / "sources" / sourceHash;
        const auto stemsRoot = sourceRoot / "stems";
        std::filesystem::create_directories(stemsRoot);
        const auto stemAudio = syntheticMusic(22050, 6.0);
        for (const std::string& stem : {"drums", "bass", "other", "vocals"}) {
            writeWavPcm16(stemsRoot / (stem + ".wav"), stemAudio);
        }

        std::vector<std::pair<int, std::string>> progress;
        const PipelineResult result = runPipeline(options,
            [&](int percent, const std::string& stage) {
                progress.emplace_back(percent, stage);
            });
        require(!result.cached && std::filesystem::is_regular_file(result.analysisReport) &&
                std::filesystem::is_regular_file(result.jamjar) &&
                std::filesystem::is_regular_file(sourceRoot / "manifest.json") &&
                std::filesystem::is_regular_file(sourceRoot / "progress.json") &&
                std::filesystem::is_regular_file(sourceRoot / "tempo.json") &&
                std::filesystem::is_regular_file(sourceRoot / "stems.json") &&
                std::filesystem::is_regular_file(sourceRoot / "chord-source.wav"),
            "pipeline commits its analysis, progress, tempo, stem, manifest, and JamJar outputs");
        require(!progress.empty() && progress.front() == std::pair{2, std::string("input_validation")} &&
                progress.back() == std::pair{100, std::string("complete")} &&
                std::is_sorted(progress.begin(), progress.end(),
                    [](const auto& left, const auto& right) {
                        return left.first < right.first;
                    }),
            "pipeline reports every checkpoint in monotonic completion order");
        require(result.analysis.bpm == 120.0 && result.analysis.beatsPerBar == 4 &&
                result.analysis.beats.size() >= 4 && !result.analysis.chords.empty() &&
                !result.sections.empty() && result.timings.contains("total_seconds") &&
                result.timings.contains("separation_realtime_factor") &&
                result.timings.at("audio_duration_seconds") == 6.0,
            "pipeline retains requested timing and complete musical/timing analysis");

        const Json stemsReport = Json::parse(readText(sourceRoot / "stems.json"));
        const Json analysisReport = Json::parse(readText(result.analysisReport));
        const Json complete = Json::parse(readText(sourceRoot / "progress.json"));
        const Json manifest = Json::parse(readText(sourceRoot / "manifest.json"));
        require(stemsReport.get("cached").boolValue() &&
                analysisReport.get("format").stringValue() == kAnalysisFormat &&
                complete.get("status").stringValue() == "complete" &&
                complete.get("percent").integerValue() == 100 &&
                manifest.get("format").stringValue() == "jamtaster-manifest-v1" &&
                manifest.get("jamjar_bytes").numberValue() > 0.0,
            "pipeline reports a cached separation and current complete output formats");

        const auto stereoPath = root / "stereo.wav";
        writeWavPcm16(stereoPath, syntheticMusic(22050, 0.1, 2));
        PipelineOptions stereo = options;
        stereo.input = stereoPath;
        const auto stereoRoot = stereo.projectRoot / "analysis" / "sources" /
            sha256File(stereoPath);
        const auto stereoSongRoot = stereoRoot / "converted" /
            result.songRoot.filename();
        std::filesystem::create_directories(stereoSongRoot);
        std::filesystem::copy_file(
            result.analysisReport, stereoRoot / "analysis.json",
            std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(
            result.jamjar, stereoSongRoot / result.jamjar.filename(),
            std::filesystem::copy_options::overwrite_existing);
        const PipelineResult stereoResult = runPipeline(stereo);
        require(stereoResult.cached &&
                std::filesystem::is_regular_file(stereoResult.analysisReport) &&
                std::filesystem::is_regular_file(stereoResult.jamjar),
            "pipeline downmixes stereo WAV input instead of rejecting analysis");

        std::vector<std::pair<int, std::string>> cachedProgress;
        const PipelineResult cached = runPipeline(options,
            [&](int percent, const std::string& stage) {
                cachedProgress.emplace_back(percent, stage);
            });
        require(cached.cached && cached.analysisReport == result.analysisReport &&
                cached.jamjar == result.jamjar &&
                cachedProgress == std::vector<std::pair<int, std::string>>{
                    {100, "cached"}},
            "current analysis and JamJar return through the one-step cache path");

        writeText(result.analysisReport, "{malformed");
        std::vector<std::pair<int, std::string>> refreshedProgress;
        const PipelineResult refreshed = runPipeline(options,
            [&](int percent, const std::string& stage) {
                refreshedProgress.emplace_back(percent, stage);
            });
        require(!refreshed.cached && !refreshedProgress.empty() &&
                refreshedProgress.front().second == "input_validation" &&
                refreshedProgress.back().second == "complete" &&
                Json::parse(readText(refreshed.analysisReport))
                    .get("format").stringValue() == kAnalysisFormat,
            "malformed cache evidence forces a complete stem-reusing refresh");

        std::filesystem::remove_all(root, ignored);
        std::cout << "JamTaster pipeline boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster pipeline boundary test failed: " << error.what() << '\n';
        return 1;
    }
}
