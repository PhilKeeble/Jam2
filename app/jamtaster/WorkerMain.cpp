#include "BeatThis.hpp"
#include "DemucsAdapter.hpp"
#include "FileSystem.hpp"
#include "Hash.hpp"
#include "Json.hpp"
#include "Pipeline.hpp"
#include "Postprocess.hpp"
#include "Wav.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using jamtaster::native::Json;

constexpr int protocolVersion = 1;

double elapsed(Clock::time_point started)
{
    return std::chrono::duration<double>(Clock::now() - started).count();
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(jamtaster::native::filesystemIoPath(path), std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());
    std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("could not read " + path.string());
    return result;
}

void writeText(const std::filesystem::path& path, const std::string& value)
{
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path partial = path.string() + ".partial";
    {
        std::ofstream output(
            jamtaster::native::filesystemIoPath(partial),
            std::ios::binary | std::ios::trunc);
        output << value << '\n';
        if (!output) throw std::runtime_error("could not write " + path.string());
    }
    std::error_code error;
    std::filesystem::remove(jamtaster::native::filesystemIoPath(path), error);
    error.clear();
    std::filesystem::rename(
        jamtaster::native::filesystemIoPath(partial),
        jamtaster::native::filesystemIoPath(path), error);
    if (error) throw std::runtime_error("could not publish " + path.string() + ": " + error.message());
}

Json readJson(const std::filesystem::path& path)
{
    return Json::parse(readText(path));
}

void emitEvent(Json event)
{
    event["protocol"] = protocolVersion;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    event["timestamp"] = std::chrono::duration<double>(now).count();
    std::cout << event.dump(-1) << '\n' << std::flush;
}

void emitProgress(int percent, const std::string& stage, const std::string& message)
{
    Json event = Json::object();
    event["type"] = "progress";
    event["stage"] = stage;
    event["percent"] = percent;
    event["message"] = message;
    emitEvent(std::move(event));
}

std::filesystem::path absolutePath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto result = std::filesystem::absolute(path, error);
    if (error) throw std::runtime_error("could not resolve " + path.string() + ": " + error.message());
    return result.lexically_normal();
}

std::filesystem::path modelsRoot(const std::filesystem::path& executable)
{
    const auto besideWorker = executable.parent_path() / "models";
    if (std::filesystem::is_directory(jamtaster::native::filesystemIoPath(besideWorker))) {
        return besideWorker;
    }
    const auto appResources = executable.parent_path().parent_path() /
        "Resources" / "jamtaster" / "models";
    if (std::filesystem::is_directory(jamtaster::native::filesystemIoPath(appResources))) {
        return appResources;
    }
    throw std::runtime_error("the bundled JamTaster models folder is missing");
}

std::vector<std::filesystem::path> demucsModels(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> result;
    for (int index = 0; index < 4; ++index) {
        result.push_back(root / ("htdemucs_ft_" + std::to_string(index) + ".onnx"));
    }
    return result;
}

void requireFiles(const std::vector<std::filesystem::path>& files)
{
    for (const auto& path : files) {
        if (!std::filesystem::is_regular_file(jamtaster::native::filesystemIoPath(path))) {
            throw std::runtime_error("the bundled model is missing: " + path.filename().string());
        }
    }
}

int threadCount(const Json& request)
{
    const int requested = request.get("options").get("threads").integerValue(
        request.get("threads").integerValue(0));
    if (requested > 0) return requested;
    return std::max(1U, std::thread::hardware_concurrency());
}

std::filesystem::path sourceRoot(
    const std::filesystem::path& projectRoot,
    const std::string& sourceHash)
{
    const auto root = projectRoot / "analysis" / "sources" / sourceHash;
    std::filesystem::create_directories(jamtaster::native::filesystemIoPath(root));
    return root;
}

void updateIndex(
    const std::filesystem::path& projectRoot,
    const std::string& sourceHash,
    const std::filesystem::path& input,
    const std::vector<std::pair<std::string, std::filesystem::path>>& results)
{
    const auto path = projectRoot / "analysis" / "index.json";
    Json index = Json::object();
    if (std::filesystem::is_regular_file(jamtaster::native::filesystemIoPath(path))) {
        try {
            index = readJson(path);
        } catch (const std::exception&) {
            index = Json::object();
        }
    }
    if (!index.isObject()) index = Json::object();
    index["format"] = "jamtaster-analysis-index-v1";
    Json& source = index["sources"][sourceHash];
    source["input_path"] = absolutePath(input).generic_string();
    source["source_sha256"] = sourceHash;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    source["updated_at"] = std::chrono::duration<double>(now).count();
    for (const auto& [name, result] : results) {
        source["results"][name] = absolutePath(result).generic_string();
    }
    writeText(path, index.dump(2));
}

Json detectTempo(
    const std::filesystem::path& input,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& models,
    const Json& request)
{
    const auto started = Clock::now();
    const std::string sourceHash = jamtaster::native::sha256File(input);
    const auto root = sourceRoot(projectRoot, sourceHash);
    const auto resultPath = root / "tempo.json";
    const bool force = request.get("force").boolValue(
        request.get("options").get("force").boolValue(false));
    if (!force && std::filesystem::is_regular_file(jamtaster::native::filesystemIoPath(resultPath))) {
        emitProgress(100, "tempo", "Using saved tempo analysis");
        return readJson(resultPath);
    }

    const auto model = models / "beat_this.onnx";
    requireFiles({model});
    emitProgress(5, "tempo", "Detecting tempo and downbeats");
    const auto audio = jamtaster::native::readWav(input);
    jamtaster::native::BeatThis analyzer(model, threadCount(request));
    const auto analysis = analyzer.analyze(audio);
    const std::vector<double> beats(analysis.beats.begin(), analysis.beats.end());
    const std::vector<double> downbeats(analysis.downbeats.begin(), analysis.downbeats.end());
    const double bpm = jamtaster::native::estimateBpm(beats);

    Json result = Json::object();
    result["format"] = "jamtaster-tempo-v1";
    result["action"] = "detect_bpm";
    result["input_path"] = absolutePath(input).generic_string();
    result["source_sha256"] = sourceHash;
    result["bpm"] = bpm;
    result["project_bpm"] = std::round(bpm);
    result["beats_per_bar"] = jamtaster::native::inferMeter(beats, downbeats);
    result["beats"] = jamtaster::native::jsonNumbers(beats);
    result["downbeats"] = jamtaster::native::jsonNumbers(downbeats);
    result["elapsed_seconds"] = elapsed(started);
    writeText(resultPath, result.dump(2));
    updateIndex(projectRoot, sourceHash, input, {{"tempo", resultPath}});
    emitProgress(100, "tempo", "Tempo analysis complete");
    return result;
}

Json splitStems(
    const std::filesystem::path& input,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& models,
    const Json& request)
{
    const auto started = Clock::now();
    const std::string sourceHash = jamtaster::native::sha256File(input);
    const auto root = sourceRoot(projectRoot, sourceHash);
    const auto stemsRoot = root / "stems";
    const auto resultPath = root / "stems.json";
    const std::vector<std::pair<std::string, std::filesystem::path>> stems{
        {"drums", stemsRoot / "drums.wav"},
        {"bass", stemsRoot / "bass.wav"},
        {"other", stemsRoot / "other.wav"},
        {"vocals", stemsRoot / "vocals.wav"},
    };
    const bool force = request.get("force").boolValue(
        request.get("options").get("force").boolValue(false));
    const bool complete = std::all_of(stems.begin(), stems.end(), [](const auto& stem) {
        return std::filesystem::is_regular_file(jamtaster::native::filesystemIoPath(stem.second));
    });
    if (!force && complete &&
        std::filesystem::is_regular_file(jamtaster::native::filesystemIoPath(resultPath))) {
        emitProgress(100, "separation", "Using saved stem analysis");
        return readJson(resultPath);
    }

    const auto demucs = demucsModels(models);
    requireFiles(demucs);
    const auto audio = jamtaster::native::readWav(input);
    emitProgress(1, "separation", "Splitting drums, bass, vocals and other");
    const auto separated = jamtaster::native::runDemucsEnsemble(
        audio, demucs, stemsRoot, threadCount(request), 0,
        [](float progress, const std::string& message) {
            emitProgress(
                std::clamp(static_cast<int>(progress * 95.0F), 1, 95),
                "separation", message);
        });

    Json result = Json::object();
    result["format"] = "jamtaster-stems-v1";
    result["action"] = "split_stems";
    result["input_path"] = absolutePath(input).generic_string();
    result["source_sha256"] = sourceHash;
    result["device"] = "cpu";
    for (const auto& [name, path] : stems) result["stems"][name] = absolutePath(path).generic_string();
    result["elapsed_seconds"] = elapsed(started);
    writeText(resultPath, result.dump(2));
    updateIndex(projectRoot, sourceHash, input, {{"stems", resultPath}});
    emitProgress(100, "separation", "Stem separation complete");
    return result;
}

std::string stageMessage(const std::string& stage)
{
    if (stage.starts_with("separation:")) return stage.substr(12);
    if (stage == "input_validation") return "Inspecting source WAV";
    if (stage == "separation") return "Stem separation complete";
    if (stage == "beat_tracking") return "Tempo and beat grid complete";
    if (stage == "chord_source_preparation") return "Preparing harmonic analysis";
    if (stage == "chords") return "Chord analysis complete";
    if (stage == "drums") return "Drum analysis complete";
    if (stage == "drum_repair") return "Repairing repeated drum patterns";
    if (stage == "drum_dynamics") return "Restoring drum dynamics";
    if (stage == "bass") return "Bass analysis complete";
    if (stage == "structure") return "Finding meaningful section changes";
    if (stage == "context_postprocessing") return "Applying musical context";
    if (stage == "section_selection") return "Section arrangement complete";
    if (stage == "jamjar_export") return "Writing JamJar and stem lanes";
    if (stage == "cached") return "Using saved JamTaster analysis";
    if (stage == "complete") return "JamTaster analysis complete";
    return stage;
}

Json analyzeAll(
    const std::string& action,
    const std::filesystem::path& input,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& models,
    const Json& request)
{
    const Json& options = request.get("options");
    jamtaster::native::PipelineOptions pipeline;
    pipeline.input = input;
    pipeline.projectRoot = projectRoot;
    pipeline.modelsRoot = models;
    pipeline.name = request.get("display_name").stringValue(input.stem().string());
    pipeline.threads = threadCount(request);
    pipeline.seed = options.get("seed").integerValue(0);
    pipeline.drumDivision = options.get("drum_division").integerValue(4);
    pipeline.minimumBassMidi = options.get("bass_min_midi").integerValue(28);
    pipeline.maximumBassMidi = options.get("bass_max_midi").integerValue(60);
    pipeline.requestedMeter = options.get("meter").integerValue(0);
    pipeline.requestedBpm = options.get("bpm").numberValue(0.0);
    pipeline.force = request.get("force").boolValue(options.get("force").boolValue(false));
    pipeline.reuseStems = options.get("reuse_stems").boolValue(false);
    pipeline.timeStretch = !options.get("no_time_stretch").boolValue(false);
    pipeline.arrangementLoop = !options.get("no_arrangement_loop").boolValue(false);
    pipeline.exportJamJar = true;

    const auto output = jamtaster::native::runPipeline(
        pipeline,
        [](int percent, const std::string& stage) {
            emitProgress(percent, stage, stageMessage(stage));
        });
    const std::string sourceHash = jamtaster::native::sha256File(input);
    updateIndex(projectRoot, sourceHash, input, {
        {"analysis", output.analysisReport},
        {"manifest", output.analysisRoot / "manifest.json"},
        {"converted_song", output.songRoot},
    });

    Json result = Json::object();
    result["format"] = "jamtaster-job-result-v1";
    result["action"] = action;
    result["input_path"] = absolutePath(input).generic_string();
    result["project_root"] = absolutePath(projectRoot).generic_string();
    result["converted_song"] = absolutePath(output.songRoot).generic_string();
    return result;
}

Json runRequest(
    const Json& request,
    const std::filesystem::path& models)
{
    const int protocol = request.get("protocol").integerValue(0);
    if (protocol != protocolVersion) {
        throw std::runtime_error(
            "unsupported JamTaster protocol " + std::to_string(protocol) +
            "; expected " + std::to_string(protocolVersion));
    }
    const std::string action = request.get("action").stringValue();
    const std::string inputValue = request.get("input_path").stringValue();
    const std::string projectValue = request.get("project_root").stringValue();
    if (inputValue.empty()) throw std::runtime_error("input_path is required");
    if (projectValue.empty()) throw std::runtime_error("project_root is required");
    const auto input = absolutePath(inputValue);
    const auto projectRoot = absolutePath(projectValue);
    if (!std::filesystem::is_regular_file(jamtaster::native::filesystemIoPath(input))) {
        throw std::runtime_error("input_path is not an existing WAV");
    }
    std::filesystem::create_directories(jamtaster::native::filesystemIoPath(projectRoot / "analysis"));

    if (action == "detect_bpm") return detectTempo(input, projectRoot, models, request);
    if (action == "split_stems") return splitStems(input, projectRoot, models, request);
    if (action == "analyze_all" || action == "convert_song") {
        return analyzeAll(action, input, projectRoot, models, request);
    }
    throw std::runtime_error("unsupported JamTaster action: " + action);
}

void usage()
{
    std::cerr << "Usage: jamtaster-worker worker --request <request.json> [--models <folder>]\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 4 || std::string(argv[1]) != "worker") {
            usage();
            return 2;
        }
        std::filesystem::path requestPath;
        std::filesystem::path explicitModels;
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--request" && index + 1 < argc) requestPath = argv[++index];
            else if (argument == "--models" && index + 1 < argc) explicitModels = argv[++index];
            else throw std::runtime_error("unknown worker argument: " + argument);
        }
        if (requestPath.empty()) throw std::runtime_error("--request is required");
        const auto executable = absolutePath(argv[0]);
        const auto models = explicitModels.empty() ? modelsRoot(executable) : absolutePath(explicitModels);
        const Json result = runRequest(readJson(requestPath), models);
        Json event = Json::object();
        event["type"] = "result";
        event["result"] = result;
        emitEvent(std::move(event));
        return 0;
    } catch (const Ort::Exception& error) {
        Json event = Json::object();
        event["type"] = "error";
        event["message"] = std::string("ONNX Runtime error: ") + error.what();
        emitEvent(std::move(event));
        return 3;
    } catch (const std::exception& error) {
        Json event = Json::object();
        event["type"] = "error";
        event["message"] = error.what();
        emitEvent(std::move(event));
        return 1;
    }
}
