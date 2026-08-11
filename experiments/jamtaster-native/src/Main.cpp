#include "Adtof.hpp"
#include "BasicPitch.hpp"
#include "BeatThis.hpp"
#include "ChordMini.hpp"
#include "OnnxModel.hpp"
#include "Pipeline.hpp"
#include "Wav.hpp"

#if JAMTASTER_NATIVE_HAS_DEMUCS
#include "DemucsAdapter.hpp"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using jamtaster::native::OnnxModel;

std::string escapeJson(std::string_view value)
{
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::string quote(const std::filesystem::path& path)
{
    return "\"" + escapeJson(path.generic_string()) + "\"";
}

template <typename T>
std::string numberArray(const std::vector<T>& values)
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) output << ',';
        output << values[index];
    }
    output << ']';
    return output.str();
}

class Arguments {
public:
    Arguments(int argc, char** argv)
    {
        for (int index = 0; index < argc; ++index) values_.emplace_back(argv[index]);
    }

    [[nodiscard]] bool has(std::string_view name) const
    {
        return std::find(values_.begin(), values_.end(), name) != values_.end();
    }

    [[nodiscard]] std::string value(std::string_view name, std::string fallback = {}) const
    {
        const auto found = std::find(values_.begin(), values_.end(), name);
        if (found == values_.end()) return fallback;
        const auto next = found + 1;
        if (next == values_.end() || next->starts_with("--")) {
            throw std::runtime_error("missing value for " + std::string(name));
        }
        return *next;
    }

    [[nodiscard]] int integer(std::string_view name, int fallback) const
    {
        const std::string text = value(name);
        if (text.empty()) return fallback;
        int parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed < 0) {
            throw std::runtime_error("invalid integer for " + std::string(name));
        }
        return parsed;
    }

    [[nodiscard]] double real(std::string_view name, double fallback) const
    {
        const std::string text = value(name);
        if (text.empty()) return fallback;
        std::size_t consumed = 0;
        const double parsed = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(parsed)) {
            throw std::runtime_error("invalid number for " + std::string(name));
        }
        return parsed;
    }

private:
    std::vector<std::string> values_;
};

int defaultThreads()
{
    return std::max(1U, std::thread::hardware_concurrency());
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".partial";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not create report: " + path.string());
        output << text << '\n';
        if (!output) throw std::runtime_error("could not write report: " + path.string());
    }
    std::filesystem::remove(path);
    std::filesystem::rename(temporary, path);
}

void writeFloats(const std::filesystem::path& path, const std::vector<float>& values)
{
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not create tensor dump: " + path.string());
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (!output) throw std::runtime_error("could not write tensor dump: " + path.string());
}

std::string describeTensor(const jamtaster::native::TensorDescription& tensor)
{
    std::ostringstream output;
    output << "{\"name\":\"" << escapeJson(tensor.name) << "\",\"shape\":"
        << numberArray(tensor.shape) << ",\"element_type\":"
        << static_cast<int>(tensor.elementType) << '}';
    return output.str();
}

std::string describeTensors(const std::vector<jamtaster::native::TensorDescription>& tensors)
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < tensors.size(); ++index) {
        if (index) output << ',';
        output << describeTensor(tensors[index]);
    }
    output << ']';
    return output.str();
}

std::string inspect(const Arguments& arguments)
{
    const auto modelPath = std::filesystem::path(arguments.value("--model"));
    if (modelPath.empty()) throw std::runtime_error("inspect requires --model");
    OnnxModel model(modelPath, arguments.integer("--threads", defaultThreads()));
    std::ostringstream output;
    output << "{\"format\":\"jamtaster-native-model-v1\",\"path\":" << quote(modelPath)
        << ",\"bytes\":" << std::filesystem::file_size(modelPath)
        << ",\"onnxruntime\":\"" << escapeJson(OnnxModel::runtimeVersion()) << "\""
        << ",\"inputs\":" << describeTensors(model.inputs())
        << ",\"outputs\":" << describeTensors(model.outputs()) << '}';
    return output.str();
}

std::string doctor(const Arguments& arguments)
{
    const auto rootText = arguments.value("--models", "models");
    const std::filesystem::path root(rootText);
    static const std::vector<std::string> files{
        "beat_this.onnx", "basic_pitch.onnx", "chordmini_btc.onnx", "adtof.onnx",
        "htdemucs_ft_0.onnx", "htdemucs_ft_1.onnx",
        "htdemucs_ft_2.onnx", "htdemucs_ft_3.onnx"};
    std::ostringstream models;
    models << '[';
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (index) models << ',';
        const auto path = root / files[index];
        const bool present = std::filesystem::is_regular_file(path);
        models << "{\"name\":\"" << files[index] << "\",\"present\":"
            << (present ? "true" : "false") << ",\"bytes\":"
            << (present ? std::filesystem::file_size(path) : 0) << '}';
    }
    models << ']';
    std::ostringstream output;
    output << "{\"format\":\"jamtaster-native-health-v1\","
        << "\"onnxruntime\":\"" << escapeJson(OnnxModel::runtimeVersion()) << "\","
        << "\"provider\":\"CPUExecutionProvider\","
        << "\"threads\":" << arguments.integer("--threads", defaultThreads()) << ','
        << "\"demucs_compiled\":" << (JAMTASTER_NATIVE_HAS_DEMUCS ? "true" : "false") << ','
        << "\"models\":" << models.str() << '}';
    return output.str();
}

std::string beat(const Arguments& arguments)
{
    const std::filesystem::path inputPath(arguments.value("--input"));
    const std::filesystem::path modelPath(arguments.value("--model"));
    if (inputPath.empty() || modelPath.empty()) {
        throw std::runtime_error("beat requires --input and --model");
    }
    const int threads = arguments.integer("--threads", defaultThreads());
    const auto audio = jamtaster::native::readWav(inputPath);
    jamtaster::native::BeatThis analyzer(modelPath, threads);
    const auto analysis = analyzer.analyze(audio);
    std::string tensorFields;
    const std::string dumpText = arguments.value("--dump-tensors");
    if (!dumpText.empty()) {
        const std::filesystem::path dumpRoot(dumpText);
        const auto beatPath = dumpRoot / "beat_logits.f32";
        const auto downbeatPath = dumpRoot / "downbeat_logits.f32";
        writeFloats(beatPath, analysis.beatLogits);
        writeFloats(downbeatPath, analysis.downbeatLogits);
        tensorFields = ",\"beat_logits\":" + quote(beatPath) +
            ",\"downbeat_logits\":" + quote(downbeatPath) +
            ",\"logit_frames\":" + std::to_string(analysis.beatLogits.size());
    }
    std::ostringstream output;
    output << std::setprecision(9)
        << "{\"format\":\"jamtaster-native-beats-v1\",\"input\":" << quote(inputPath)
        << ",\"model\":" << quote(modelPath)
        << ",\"provider\":\"CPUExecutionProvider\",\"threads\":" << threads
        << ",\"sample_rate\":" << audio.sampleRate
        << ",\"channels\":" << audio.channels
        << ",\"frames\":" << audio.frames()
        << ",\"beats\":" << numberArray(analysis.beats)
        << ",\"downbeats\":" << numberArray(analysis.downbeats)
        << ",\"timings\":{\"preprocessing\":" << analysis.preprocessingSeconds
        << ",\"inference\":" << analysis.inferenceSeconds
        << ",\"postprocessing\":" << analysis.postprocessingSeconds << '}'
        << tensorFields << '}';
    return output.str();
}

std::string basicPitch(const Arguments& arguments)
{
    const std::filesystem::path inputPath(arguments.value("--input"));
    const std::filesystem::path modelPath(arguments.value("--model"));
    if (inputPath.empty() || modelPath.empty()) {
        throw std::runtime_error("basic-pitch requires --input and --model");
    }
    const int threads = arguments.integer("--threads", defaultThreads());
    const int minimumMidi = arguments.integer("--min-midi", 28);
    const int maximumMidi = arguments.integer("--max-midi", 64);
    const auto audio = jamtaster::native::readWav(inputPath);
    const auto modelStarted = std::chrono::steady_clock::now();
    jamtaster::native::BasicPitch analyzer(modelPath, threads);
    const double modelLoadSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - modelStarted).count();
    const auto analysis = analyzer.analyze(audio, minimumMidi, maximumMidi);
    std::ostringstream notes;
    notes << std::setprecision(9) << '[';
    for (std::size_t index = 0; index < analysis.notes.size(); ++index) {
        if (index) notes << ',';
        const auto& note = analysis.notes[index];
        notes << "{\"start\":" << note.start << ",\"end\":" << note.end
            << ",\"midi\":" << note.midi << ",\"velocity\":" << note.velocity
            << ",\"confidence\":" << note.confidence << '}';
    }
    notes << ']';
    std::ostringstream output;
    output << std::setprecision(9)
        << "{\"format\":\"jamtaster-native-basic-pitch-v1\",\"input\":" << quote(inputPath)
        << ",\"model\":" << quote(modelPath)
        << ",\"provider\":\"CPUExecutionProvider\",\"threads\":" << threads
        << ",\"midi_range\":[" << minimumMidi << ',' << maximumMidi << ']'
        << ",\"activation_frames\":" << analysis.activationFrames
        << ",\"notes\":" << notes.str()
        << ",\"timings\":{\"model_load\":" << modelLoadSeconds
        << ",\"preprocessing\":" << analysis.preprocessingSeconds
        << ",\"inference\":" << analysis.inferenceSeconds
        << ",\"postprocessing\":" << analysis.postprocessingSeconds << "}}";
    return output.str();
}

std::string chords(const Arguments& arguments)
{
    const std::filesystem::path inputPath(arguments.value("--input"));
    const std::filesystem::path modelPath(arguments.value("--model"));
    if (inputPath.empty() || modelPath.empty()) {
        throw std::runtime_error("chords requires --input and --model");
    }
    const int threads = arguments.integer("--threads", defaultThreads());
    const auto audio = jamtaster::native::readWav(inputPath);
    const auto modelStarted = std::chrono::steady_clock::now();
    jamtaster::native::ChordMini analyzer(modelPath, threads);
    const double modelLoadSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - modelStarted).count();
    const auto analysis = analyzer.analyze(audio);
    std::ostringstream segments;
    segments << std::setprecision(9) << '[';
    for (std::size_t index = 0; index < analysis.chords.size(); ++index) {
        if (index) segments << ',';
        const auto& chord = analysis.chords[index];
        segments << "{\"start\":" << chord.start << ",\"end\":" << chord.end
            << ",\"label\":\"" << escapeJson(chord.label) << "\",\"confidence_margin\":"
            << chord.confidenceMargin << '}';
    }
    segments << ']';
    std::ostringstream output;
    output << std::setprecision(9)
        << "{\"format\":\"jamtaster-native-chords-v1\",\"input\":" << quote(inputPath)
        << ",\"model\":" << quote(modelPath)
        << ",\"provider\":\"CPUExecutionProvider\",\"threads\":" << threads
        << ",\"feature_frames\":" << analysis.featureFrames
        << ",\"chords\":" << segments.str()
        << ",\"timings\":{\"model_load\":" << modelLoadSeconds
        << ",\"preprocessing\":" << analysis.preprocessingSeconds
        << ",\"inference\":" << analysis.inferenceSeconds
        << ",\"postprocessing\":" << analysis.postprocessingSeconds << "}}";
    return output.str();
}

std::array<float, 5> drumThresholds(const std::string& text)
{
    std::array<float, 5> result{0.22F, 0.24F, 0.32F, 0.22F, 0.30F};
    if (text.empty()) return result;
    std::size_t start = 0;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto end = text.find(',', start);
        if ((end == std::string::npos) != (index + 1 == result.size())) {
            throw std::runtime_error("--thresholds requires five comma-separated values");
        }
        const auto part = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
        std::size_t consumed = 0;
        result[index] = std::stof(part, &consumed);
        if (consumed != part.size()) throw std::runtime_error("invalid --thresholds value");
        start = end == std::string::npos ? text.size() : end + 1;
    }
    return result;
}

std::string drums(const Arguments& arguments)
{
    const std::filesystem::path inputPath(arguments.value("--input"));
    const std::filesystem::path modelPath(arguments.value("--model"));
    if (inputPath.empty() || modelPath.empty()) {
        throw std::runtime_error("drums requires --input and --model");
    }
    const int threads = arguments.integer("--threads", defaultThreads());
    const auto thresholds = drumThresholds(arguments.value("--thresholds"));
    const auto audio = jamtaster::native::readWav(inputPath);
    const auto modelStarted = std::chrono::steady_clock::now();
    jamtaster::native::Adtof analyzer(modelPath, threads);
    const double modelLoadSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - modelStarted).count();
    const auto analysis = analyzer.analyze(audio, thresholds);
    std::ostringstream hits;
    hits << std::setprecision(9) << '[';
    for (std::size_t index = 0; index < analysis.hits.size(); ++index) {
        if (index) hits << ',';
        const auto& hit = analysis.hits[index];
        hits << "{\"time\":" << hit.time << ",\"midi\":" << hit.midi
            << ",\"lane\":\"" << escapeJson(hit.lane) << "\",\"confidence\":"
            << hit.confidence << '}';
    }
    hits << ']';
    std::ostringstream output;
    output << std::setprecision(9)
        << "{\"format\":\"jamtaster-native-drums-v1\",\"input\":" << quote(inputPath)
        << ",\"model\":" << quote(modelPath)
        << ",\"provider\":\"CPUExecutionProvider\",\"threads\":" << threads
        << ",\"activation_frames\":" << analysis.activationFrames
        << ",\"thresholds\":" << numberArray(std::vector<float>(thresholds.begin(), thresholds.end()))
        << ",\"hits\":" << hits.str()
        << ",\"timings\":{\"model_load\":" << modelLoadSeconds
        << ",\"preprocessing\":" << analysis.preprocessingSeconds
        << ",\"inference\":" << analysis.inferenceSeconds
        << ",\"postprocessing\":" << analysis.postprocessingSeconds << "}}";
    return output.str();
}

std::string taste(const Arguments& arguments)
{
    jamtaster::native::PipelineOptions options;
    options.input = arguments.value("--input");
    options.projectRoot = arguments.value("--project-root");
    options.modelsRoot = arguments.value("--models", "experiments/jamtaster-native/models");
    options.name = arguments.value("--name");
    options.threads = arguments.integer("--threads", defaultThreads());
    options.seed = arguments.integer("--seed", 0);
    options.drumDivision = arguments.integer("--drum-division", 4);
    options.minimumBassMidi = arguments.integer("--bass-min-midi", 28);
    options.maximumBassMidi = arguments.integer("--bass-max-midi", 60);
    options.requestedMeter = arguments.integer("--meter", 0);
    options.requestedBpm = arguments.real("--bpm", 0.0);
    options.force = arguments.has("--force");
    options.reuseStems = arguments.has("--reuse-stems");
    options.timeStretch = !arguments.has("--no-time-stretch");
    options.arrangementLoop = !arguments.has("--no-arrangement-loop");
    options.exportJamJar = !arguments.has("--analysis-only");
    const auto result = jamtaster::native::runPipeline(options,
        [](int percent, const std::string& stage) {
            std::cerr << percent << "% " << stage << '\n';
        });
    std::ostringstream output;
    output << "{\"format\":\"jamtaster-native-taste-v1\",\"cached\":"
        << (result.cached ? "true" : "false")
        << ",\"analysis_root\":" << quote(result.analysisRoot)
        << ",\"analysis\":" << quote(result.analysisReport)
        << ",\"song_root\":" << quote(result.songRoot)
        << ",\"jamjar\":" << quote(result.jamjar) << '}';
    return output.str();
}

#if JAMTASTER_NATIVE_HAS_DEMUCS
std::vector<std::filesystem::path> splitPaths(const std::string& text)
{
    std::vector<std::filesystem::path> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(';', start);
        const auto value = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
        if (!value.empty()) result.emplace_back(value);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

std::string demucs(const Arguments& arguments)
{
    const std::filesystem::path inputPath(arguments.value("--input"));
    const std::filesystem::path outputRoot(arguments.value("--output-dir"));
    const auto models = splitPaths(arguments.value("--models"));
    if (inputPath.empty() || outputRoot.empty() || models.empty()) {
        throw std::runtime_error("demucs requires --input, --models and --output-dir");
    }
    const int threads = arguments.integer("--threads", defaultThreads());
    const auto seed = static_cast<std::uint32_t>(arguments.integer("--seed", 0));
    const auto audio = jamtaster::native::readWav(inputPath);
    const auto result = jamtaster::native::runDemucsEnsemble(
        audio, models, outputRoot, threads, seed,
        [](float value, const std::string& message) {
            std::cerr << std::fixed << std::setprecision(1) << value * 100.0F
                << "% " << message << '\n';
        });
    std::ostringstream stems;
    stems << '[';
    for (std::size_t index = 0; index < result.stems.size(); ++index) {
        if (index) stems << ',';
        stems << quote(result.stems[index]);
    }
    stems << ']';
    std::ostringstream shifts;
    shifts << '[';
    for (std::size_t index = 0; index < result.shiftOffsets.size(); ++index) {
        if (index) shifts << ',';
        shifts << result.shiftOffsets[index];
    }
    shifts << ']';
    std::ostringstream output;
    output << "{\"format\":\"jamtaster-native-demucs-v1\",\"input\":" << quote(inputPath)
        << ",\"provider\":\"CPUExecutionProvider\",\"threads\":" << threads
        << ",\"shift_seed\":" << seed
        << ",\"shift_offsets\":" << shifts.str()
        << ",\"ensemble_members\":" << result.ensembleMembers
        << ",\"stems\":" << stems.str()
        << ",\"timings\":{\"preprocessing\":" << result.preprocessingSeconds
        << ",\"inference\":" << result.inferenceSeconds
        << ",\"writing\":" << result.writingSeconds << "}}";
    return output.str();
}
#endif

void usage()
{
    std::cerr
        << "JamTaster native feasibility lab\n"
        << "  jamtaster_native_lab doctor [--models DIR] [--threads N]\n"
        << "  jamtaster_native_lab inspect --model FILE [--threads N]\n"
        << "  jamtaster_native_lab beat --input WAV --model FILE [--threads N]"
           " [--dump-tensors DIR] [--output JSON]\n"
        << "  jamtaster_native_lab basic-pitch --input WAV --model FILE"
           " [--min-midi N] [--max-midi N] [--threads N] [--output JSON]\n"
        << "  jamtaster_native_lab chords --input WAV --model FILE"
           " [--threads N] [--output JSON]\n"
        << "  jamtaster_native_lab drums --input WAV --model FILE"
           " [--thresholds K,S,T,H,C] [--threads N] [--output JSON]\n"
        << "  jamtaster_native_lab demucs --input WAV --models A;B;C;D"
           " --output-dir DIR [--threads N] [--seed N] [--output JSON]\n";
    std::cerr
        << "  jamtaster_native_lab taste --input WAV --project-root DIR --name NAME"
           " [--models DIR] [--threads N] [--bpm N] [--meter N]"
           " [--drum-division N] [--bass-min-midi N] [--bass-max-midi N]"
           " [--force] [--reuse-stems] [--analysis-only] [--no-time-stretch]"
           " [--no-arrangement-loop] [--output JSON]\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2) {
            usage();
            return 2;
        }
        const Arguments arguments(argc - 2, argv + 2);
        const std::string command = argv[1];
        std::string result;
        if (command == "doctor") result = doctor(arguments);
        else if (command == "inspect") result = inspect(arguments);
        else if (command == "beat") result = beat(arguments);
        else if (command == "basic-pitch") result = basicPitch(arguments);
        else if (command == "chords") result = chords(arguments);
        else if (command == "drums") result = drums(arguments);
        else if (command == "taste") result = taste(arguments);
#if JAMTASTER_NATIVE_HAS_DEMUCS
        else if (command == "demucs") result = demucs(arguments);
#else
        else if (command == "demucs") {
            throw std::runtime_error("Demucs support was not compiled; configure Eigen and rebuild");
        }
#endif
        else {
            usage();
            return 2;
        }
        const std::string outputPath = arguments.value("--output");
        if (!outputPath.empty()) writeText(outputPath, result);
        std::cout << result << '\n';
        return 0;
    } catch (const Ort::Exception& error) {
        std::cerr << "ONNX Runtime error: " << error.what() << '\n';
        return 3;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster native error: " << error.what() << '\n';
        return 1;
    }
}
