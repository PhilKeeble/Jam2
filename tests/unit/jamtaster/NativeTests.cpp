// Native unit coverage is owned by the repository-level test tree.
#include "Dsp.hpp"
#include "Export.hpp"
#include "Hash.hpp"
#include "Json.hpp"
#include "Postprocess.hpp"
#include "Wav.hpp"

#if JAMTASTER_NATIVE_HAS_DEMUCS
#include "third_party/demucs_onnx/dsp.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
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

jamtaster::native::AudioBuffer sine(int sampleRate, int channels, double seconds)
{
    jamtaster::native::AudioBuffer result;
    result.sampleRate = sampleRate;
    result.channels = channels;
    const auto frames = static_cast<std::size_t>(std::llround(sampleRate * seconds));
    result.samples.resize(frames * static_cast<std::size_t>(channels));
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float value = static_cast<float>(0.5 *
            std::sin(2.0 * pi * 440.0 * frame / sampleRate));
        for (int channel = 0; channel < channels; ++channel) {
            result.samples[frame * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(channel)] = value;
        }
    }
    return result;
}

void testAudioBuffer()
{
    const auto audio = sine(48000, 2, 0.1);
    require(audio.frames() == 4800, "stereo frame count");
    const auto mono = audio.mono();
    require(mono.size() == audio.frames(), "mono frame count");
    require(std::abs(mono[100] - audio.samples[200]) < 1.0e-6F, "mono average");
}

void testWorkerProtocolJson()
{
    const auto request = jamtaster::native::Json::parse(
        R"({"protocol":1,"action":"analyze_all","options":{"force":true},"name":"Caf\u00e9"})");
    require(request.get("protocol").integerValue() == 1, "worker protocol number");
    require(request.get("action").stringValue() == "analyze_all", "worker action");
    require(request.get("options").get("force").boolValue(), "worker nested option");
    require(request.get("name").stringValue() == "Caf\xC3\xA9", "worker Unicode request");

    auto values = jamtaster::native::jsonNumbers(std::vector<double>{1.25, -2.5});
    jamtaster::native::Json root = jamtaster::native::Json::object();
    root["values"] = values;
    root["finite"] = 3.5;
    root["non_finite"] = std::numeric_limits<double>::infinity();
    root["escaped"] = std::string("line\n\t\"");
    const std::string compact = root.dump(-1);
    const std::string pretty = root.dump(2);
    const auto reparsed = jamtaster::native::Json::parse(compact);
    require(reparsed.isObject() && reparsed.get("values").isArray() &&
            std::abs(reparsed.get("finite").numberValue() - 3.5) < 1.0e-9 &&
            reparsed.get("non_finite").stringValue("null") == "null" &&
            compact.find("\"non_finite\":null") != std::string::npos &&
            pretty.find('\n') != std::string::npos,
        "JSON array, numeric, non-finite, escaped, compact, and padded output");
    requireThrows(
        [] { (void)jamtaster::native::Json::parse("[1,]"); },
        "malformed JSON array must report a parser failure");
}

void testResampler()
{
    const auto input = sine(44100, 1, 1.0).samples;
    const auto output = jamtaster::native::resampleSinc(input, 44100, 22050);
    require(output.size() == 22050, "resampler output length");
    float peak = 0.0F;
    for (float value : output) peak = std::max(peak, std::abs(value));
    require(peak > 0.45F && peak < 0.55F, "resampler level");
}

void testMel()
{
    const auto input = sine(22050, 1, 1.0).samples;
    const auto mel = jamtaster::native::beatThisLogMel(input);
    require(mel.rows == 51, "Beat This frame count");
    require(mel.columns == 128, "Beat This mel band count");
    float maximum = 0.0F;
    for (float value : mel.values) {
        require(std::isfinite(value) && value >= 0.0F, "finite log-mel value");
        maximum = std::max(maximum, value);
    }
    require(maximum > 1.0F, "non-silent log-mel output");
}

void testAdtofFeatures()
{
    const auto input = sine(44100, 1, 1.0).samples;
    const auto features = jamtaster::native::adtofLogFilterbank(input);
    require(features.rows == 101, "ADTOF frame count");
    require(features.columns == 84, "ADTOF filter count");
    float maximum = 0.0F;
    for (float value : features.values) {
        require(std::isfinite(value) && value >= 0.0F, "finite ADTOF feature");
        maximum = std::max(maximum, value);
    }
    require(maximum > 0.1F, "non-silent ADTOF feature");
}

void testChordFeatures()
{
    const auto input = sine(22050, 1, 1.0).samples;
    const auto features = jamtaster::native::chordMiniLogCqt(input);
    require(features.rows == 11, "ChordMini frame count");
    require(features.columns == 144, "ChordMini CQT bin count");
    float maximum = -100.0F;
    for (float value : features.values) {
        require(std::isfinite(value), "finite ChordMini feature");
        maximum = std::max(maximum, value);
    }
    require(maximum > -3.0F, "non-silent ChordMini feature");
}

void testWavRoundTrip()
{
    const auto path = std::filesystem::temp_directory_path() /
        "jam2-jamtaster-native-wav-roundtrip.wav";
    const auto source = sine(48000, 2, 0.05);
    jamtaster::native::writeWavPcm16(path, source);
    const auto loaded = jamtaster::native::readWav(path);
    std::filesystem::remove(path);
    require(loaded.sampleRate == source.sampleRate, "WAV sample rate");
    require(loaded.channels == source.channels, "WAV channel count");
    require(loaded.frames() == source.frames(), "WAV frame count");
    require(std::abs(loaded.samples[100] - source.samples[100]) < 1.0e-4F,
        "WAV PCM16 roundtrip");
}

void testWavMixAndCrop()
{
    using namespace jamtaster::native;
    AudioBuffer first;
    first.sampleRate = 100;
    first.channels = 2;
    first.samples = {0.75F, 0.75F, -0.75F, -0.75F, 0.25F, 0.25F};
    AudioBuffer second;
    second.sampleRate = 100;
    second.channels = 1;
    second.samples = {0.75F, -0.75F};
    const AudioBuffer mixed = mixMono(first, second);
    require(mixed.channels == 1 && mixed.frames() == 3 &&
            mixed.samples[0] == 1.0F && mixed.samples[1] == -1.0F &&
            mixed.samples[2] == 0.25F,
        "mono mix saturates and zero-extends the shorter source");
    const AudioBuffer cropped = cropAudio(first, 0.01, 0.03);
    require(cropped.channels == 2 && cropped.frames() == 2 &&
            cropped.samples.front() == -0.75F,
        "audio crop preserves channels and rounded frame bounds");
    second.sampleRate = 200;
    requireThrows([&] { (void)mixMono(first, second); },
        "mix rejects mismatched sample rates");
    requireThrows([&] { (void)cropAudio(first, 0.03, 0.01); },
        "crop rejects a reversed interval");
}

void testPipelineUtilities()
{
    require(jamtaster::native::sha256("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 reference vector");
    require(jamtaster::native::portableSlug("  My / Song?!  ") == "My_Song",
        "portable song slug");
    require(jamtaster::native::normalizeChord("A:min7") == "Am7",
        "ChordMini to Jam2 chord normalization");
    const std::vector<double> beats{0.0, 0.5, 1.0, 1.5, 2.0};
    require(std::abs(jamtaster::native::estimateBpm(beats) - 120.0) < 1.0e-9,
        "native BPM wrapper");
    const std::vector<double> downbeats{0.0, 2.0, 4.0};
    const std::vector<double> meterBeats{0,.5,1,1.5,2,2.5,3,3.5,4};
    require(jamtaster::native::inferMeter(meterBeats, downbeats) == 4,
        "native meter wrapper");
    const std::vector<jamtaster::native::TimedLabel> upper{{0.0, 2.0, "C", 1.0}};
    const std::vector<jamtaster::native::NoteEvent> bass{{0.0, 1.0, 45, 100, 1.0}};
    const auto fused = jamtaster::native::fuseChordsWithBass(upper, bass);
    require(fused.size() == 1 && fused.front().label == "Am7",
        "bass-root chord fusion");
}

void testPostprocessBoundaries()
{
    using namespace jamtaster::native;

    const auto [chromaLabels, chromaEvidence] = analyzeChromaChords(
        sine(22050, 1, 2.0), {0.0, 0.5, 1.0, 1.5});
    require(!chromaLabels.empty() && chromaEvidence.size() == 4 &&
            !chromaEvidence.front().candidates.empty(),
        "chroma analysis emits one ranked evidence row per beat interval");

    std::vector<TimedLabel> unstable{
        {0.1, 0.5, "C:min", 0.8},
        {0.55, 1.2, "C:maj", 0.9},
        {1.3, 1.3, "G", 0.1},
    };
    const auto stable = stabilizeChords(std::move(unstable), 1.4);
    require(stable.size() == 2 && stable.front().start == 0.0 &&
            stable.front().end == stable.back().start &&
            stable.front().label == "C" && stable.back().end == 1.4,
        "chord stabilization removes empty events and closes short same-root gaps");

    const std::vector<TimedLabel> cMajor{{0.0, 1.0, "C", 1.0}};
    const auto bass = contextualizeBass({
        {0.0, 0.4, 43, 90, 0.8},
        {0.02, 0.5, 48, 100, 0.9},
        {0.4, 0.9, 52, 80, 0.7},
        {0.8, 0.83, 55, 127, 1.0},
    }, cMajor);
    require(bass.size() == 2 && bass.front().midi == 48 &&
            bass.front().end == bass.back().start,
        "bass context prefers the chord root, removes tiny notes, and resolves overlap");

    ChordEvidence evidence;
    evidence.start = 0.0;
    evidence.end = 2.0;
    evidence.label = "C";
    evidence.profile[0] = 1.0;
    evidence.profile[4] = 0.8;
    evidence.profile[7] = 0.7;
    evidence.candidates = {{"C", 0.9}, {"Am", 0.4}, {"G", 0.2}};
    const auto contextual = contextualizeChords(
        {{0.0, 0.5, "C", 0.9}, {0.5, 1.0, "G", 0.4},
         {1.0, 1.5, "C", 0.9}},
        {evidence}, bass, {0.0, 0.5, 1.0, 1.5}, 2.0);
    require(!contextual.empty() && contextual.front().start == 0.0 &&
            contextual.back().end == 2.0,
        "chord context spans all beat bounds using chroma, key, and bass evidence");

    const std::vector<double> beats{
        0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
    const std::vector<DrumHit> detected{
        {0.5, "Snare", 90, 0.9, 0.8, "detected"},
        {2.5, "Snare", 90, 0.9, 0.8, "detected"},
    };
    const auto repaired = repairDrums(detected,
        {{4.5, "Snare", 70, 0.4, 0.5, "candidate"},
         {4.0, "Kick", 100, 0.9, 0.8, "candidate"}},
        beats, 120.0, 4, 4, 2, 4, false);
    require(repaired.size() == 3 &&
            repaired.back().provenance == "repaired_repetition",
        "repetition repair adds supported snare candidates but not unrelated lanes");

    const auto rimmed = repairDrums({
        {0.0, "Kick", 80, 1.0, 1.0, "detected"},
        {2.0, "Kick", 80, 1.0, 1.0, "detected"},
        {4.0, "Kick", 80, 1.0, 1.0, "detected"},
        {0.01, "Cross-stick / Rim", 90, 1.0, 1.0, "detected"},
        {2.01, "Closed HH", 90, 1.0, 1.0, "detected"},
    }, {{1.0, "Snare", 80, 0.2, 0.2, "candidate"}},
        beats, 120.0, 4, 4, 2, 4, true);
    require(rimmed.size() == 3 &&
            std::all_of(rimmed.begin(), rimmed.end(), [](const DrumHit& hit) {
                return hit.lane == "Kick" && hit.velocity == 122;
            }),
        "rim mode retains and accents only the stable kick pattern");

    const auto shaped = shapeDrumDynamics({
        {0.0, "Snare", 90, 1.0, 1.0, "detected"},
        {0.5, "Snare", 90, 1.0, 1.0, "detected"},
        {1.0, "Kick", 90, 1.0, 1.0, "detected"},
    }, beats, {0.0, 2.0, 4.0}, 120.0, 4, 4);
    require(shaped[0].velocity == 38 && shaped[1].velocity == 122 &&
            shaped[2].velocity == 90,
        "snare dynamics distinguish backbeats from ghost positions");

    std::vector<DrumHit> cymbals;
    for (int index = 0; index < 5; ++index) {
        cymbals.push_back({index * 0.5, "Crash", 100, 1.0, 1.0, "detected"});
    }
    const auto classified = classifyCymbals(
        cymbals, {0.0, 0.5, 1.0, 1.5, 2.0}, {0.0, 2.0}, {});
    require(classified.front().lane == "Crash" &&
            classified[1].lane == "Ride" &&
            classified.back().lane == "Crash" &&
            classified[1].provenance.ends_with("_ride_context"),
        "regular cymbals become rides except strong downbeat crashes");

    Analysis sectioned;
    sectioned.bpm = 120.0;
    sectioned.beatsPerBar = 4;
    for (int index = 0; index <= 8; ++index) sectioned.beats.push_back(index * 0.5);
    sectioned.downbeats = {0.0, 2.0, 4.0};
    sectioned.structures = {
        {0.0, 2.0, "Intro", 1.0}, {2.0, 4.0, "Verse", 1.0}};
    const auto choices = chooseSections(sectioned, 4.0);
    require(choices.size() == 2 && choices.front().beats == 4 &&
            choices.back().firstBeat == 4,
        "section choice snaps measured structures to complete bars");
    sectioned.structures.clear();
    const auto fallback = chooseSections(sectioned, 4.0);
    require(fallback.size() == 1 && fallback.front().role == "full" &&
            fallback.front().beats == 8,
        "section choice falls back to a bounded full sample");
    requireThrows([] {
        Analysis invalid;
        invalid.bpm = 120.0;
        (void)chooseSections(invalid, 1.0);
    }, "section choice rejects missing beat evidence");
    requireThrows([] {
        Analysis tooLong;
        tooLong.bpm = 120.0;
        tooLong.beatsPerBar = 4;
        tooLong.beats = {0.0, 0.5};
        (void)chooseSections(tooLong, 300.0);
    }, "section choice rejects a full sample beyond the 512-beat limit");
}

void testNativeAnalysisAndJamJarExport()
{
    using namespace jamtaster::native;
    const auto root = std::filesystem::temp_directory_path() /
        "jam2-jamtaster-native-export-boundary";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root / "stems");

    Analysis analysis;
    analysis.detectedBpm = 119.8;
    analysis.bpm = 120.0;
    analysis.beatsPerBar = 4;
    analysis.drumDivision = 4;
    analysis.beats = {0.0, 0.5, 1.0, 1.5, 2.0};
    analysis.downbeats = {0.0, 2.0};
    analysis.structures = {{0.0, 2.0, "Verse", 0.95}};
    analysis.chords = {
        {0.0, 1.0, "C:maj", 0.9}, {1.0, 2.0, "A:min7", 0.8}};
    ChordEvidence evidence;
    evidence.start = 0.0;
    evidence.end = 0.5;
    evidence.label = "C";
    evidence.confidence = 0.7;
    evidence.profile[0] = 1.0;
    evidence.candidates = {{"C", 0.9}, {"Am", 0.4}};
    analysis.chordEvidence = {evidence};
    analysis.drums = {
        {0.0, "Kick", 110, 0.9, 1.0, "detected"},
        {0.25, "Snare", 30, 0.8, 0.5, "candidate"},
        {0.5, "Closed HH", 80, 0.7, 0.4, "detected"},
        {0.5, "Unknown", 127, 1.0, 1.0, "ignored"},
    };
    analysis.drumCandidates = {{0.25, "Snare", 30, 0.8, 0.5, "candidate"}};
    analysis.bass = {
        {0.0, 0.5, 48, 100, 0.9}, {1.0, 1.5, 45, 90, 0.8}};
    analysis.warnings = {"synthetic warning"};
    const SectionChoice choice{"verse", "Verse", 0.0, 2.0, 0, 4};

    Json quantization = Json::object();
    quantization["source"] = "native-test";
    const Json report = analysisJson(analysis, {choice}, {{"total", 0.25}},
        root / "source.wav", "source-hash", 8000, 16000, 1, quantization);
    const auto parsedReport = Json::parse(report.dump(-1));
    require(parsedReport.get("format").stringValue() == kAnalysisFormat &&
            parsedReport.get("input").get("duration_seconds").numberValue() == 2.0 &&
            parsedReport.get("analysis").get("warnings").isArray() &&
            parsedReport.get("arrangement").isArray(),
        "analysis JSON preserves input, musical evidence, warnings, and arrangement");

    std::map<std::string, std::filesystem::path> stems;
    for (const std::string& name : {"drums", "bass", "other", "vocals"}) {
        const auto path = root / "stems" / (name + ".wav");
        writeWavPcm16(path, sine(8000, 1, 4.0));
        stems[name] = path;
    }
    require(sha256File(stems.at("drums")).size() == 64,
        "file SHA-256 emits the complete lowercase digest");

    const auto imported = root / "Boundary_Song" / "imported";
    std::filesystem::create_directories(imported);
    writeWavPcm16(imported / "jamtaster-obsolete.wav", sine(8000, 1, 0.1));
    writeWavPcm16(imported / "user-owned.wav", sine(8000, 1, 0.1));
    const JamJarExport exported = exportJamJar(
        root, "Boundary Song", "source-hash", stems, analysis, {choice}, true, false);
    require(std::filesystem::exists(exported.path) && exported.jamjarBytes > 0 &&
            exported.assets.isArray() && exported.quantization.isObject() &&
            !std::filesystem::exists(imported / "jamtaster-obsolete.wav") &&
            std::filesystem::exists(imported / "user-owned.wav"),
        "JamJar export writes current assets and removes only obsolete owned WAVs");
    std::ifstream input(exported.path, std::ios::binary);
    const std::string encoded{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const Json jamjar = Json::parse(encoded);
    require(jamjar.get("beat_lane_schema").integerValue() == 3 &&
            jamjar.get("sections").isArray() && jamjar.get("looper").isObject() &&
            jamjar.get("track").get("metronome_bpm").integerValue() == 120,
        "exported JamJar parses with current schema, sections, looper, and timing");

    Analysis warped = analysis;
    warped.beats = {0.0, 0.45, 0.9, 1.35, 1.7, 2.3, 2.7, 3.1, 3.6};
    warped.downbeats = {0.0, 1.7, 3.6};
    warped.structures = {{0.0, 3.6, "Warped", 0.95}};
    warped.chords = {{0.0, 1.7, "C", 0.9}, {1.7, 3.6, "Am", 0.9}};
    warped.bass = {{0.0, 1.7, 48, 100, 0.9}, {1.7, 3.6, 45, 90, 0.8}};
    const SectionChoice warpedChoice{"warped", "Warped", 0.0, 3.6, 0, 8};
    const JamJarExport stretched = exportJamJar(
        root, "Warped Song", "warped-hash", stems, warped,
        {warpedChoice}, false, true);
    int stretchedAssets = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             root / "Warped_Song" / "imported")) {
        if (entry.path().extension() != ".wav") continue;
        const AudioBuffer rendered = readWav(entry.path());
        require(rendered.sampleRate == 8000 && rendered.frames() == 32000,
            "anchored JamJar export emits the exact fixed-grid frame count");
        ++stretchedAssets;
    }
    require(stretchedAssets == 4 &&
            stretched.quantization.dump(-1).find("\"time_stretch_enabled\":true") !=
                std::string::npos,
        "nonuniform eight-beat export anchors all four stems and reports stretching");

    std::vector<SectionChoice> tooMany(13, choice);
    requireThrows([&] {
        (void)exportJamJar(root, "Too Many", "source-hash", stems,
            analysis, tooMany, false, false);
    }, "JamJar export rejects more than twelve sections");
    auto missingStem = stems;
    missingStem.erase("vocals");
    requireThrows([&] {
        (void)exportJamJar(root, "Missing Stem", "source-hash", missingStem,
            analysis, {choice}, false, false);
    }, "JamJar export rejects an incomplete stem set");

    std::filesystem::remove_all(root, ignored);
}

void testNativeStretchLength()
{
    const auto source = sine(22050, 1, 0.8);
    const auto stretched = jamtaster::native::stretchAudio(source, 22050);
    require(stretched.sampleRate == source.sampleRate, "stretch sample rate");
    require(stretched.channels == 1, "stretch channel count");
    require(stretched.frames() == 22050, "stretch exact output length");
    double openingSquare = 0.0;
    constexpr std::size_t openingFrames = 1102;
    for (std::size_t frame = 0; frame < openingFrames; ++frame)
        openingSquare += stretched.samples[frame] * stretched.samples[frame];
    require(std::sqrt(openingSquare / openingFrames) > 0.1,
        "latency-compensated stretch must not add processor warm-up silence");
    double closingSquare = 0.0;
    for (std::size_t frame = stretched.frames() - openingFrames;
        frame < stretched.frames(); ++frame)
        closingSquare += stretched.samples[frame] * stretched.samples[frame];
    require(std::sqrt(closingSquare / openingFrames) > 0.1,
        "latency-compensated stretch must not taper the segment tail to silence");

    const auto barSource = sine(22050, 1, 1.6);
    const std::vector<std::size_t> inputBounds{0, 17640, 35280};
    const std::vector<std::size_t> outputBounds{0, 18000, 36000};
    const auto anchored = jamtaster::native::stretchAudioAnchored(
        barSource, inputBounds, outputBounds);
    require(anchored.frames() == outputBounds.back(),
        "bar-anchored stretch exact output length");
    constexpr std::size_t seamWindow = 512;
    double seamSquare = 0.0;
    for (std::size_t frame = outputBounds[1] - seamWindow;
        frame < outputBounds[1] + seamWindow; ++frame) {
        seamSquare += anchored.samples[frame] * anchored.samples[frame];
    }
    require(std::sqrt(seamSquare / (2 * seamWindow)) > 0.1,
        "bar-anchored stretch must retain audio across its join");
}

void testSectionSplittingRequiresLongRecording()
{
    using namespace jamtaster::native;
    constexpr double beatSeconds = 0.5;
    constexpr double barSeconds = beatSeconds * 4.0;

    const auto analysisWithChange = [&](int barCount, int transitionBar) {
        Analysis analysis;
        analysis.beatsPerBar = 4;
        for (int beat = 0; beat <= barCount * analysis.beatsPerBar; ++beat)
            analysis.beats.push_back(beat * beatSeconds);
        for (int bar = 0; bar <= barCount; ++bar)
            analysis.downbeats.push_back(bar * barSeconds);
        for (int bar = 0; bar < barCount; ++bar) {
            const double start = bar * barSeconds;
            const bool secondPattern = bar >= transitionBar;
            analysis.chords.push_back({start, start + barSeconds,
                secondPattern ? "C" : "Am", 1.0});
            analysis.bass.push_back({start, start + 0.4,
                secondPattern ? 48 : 45, 100, 1.0});
            for (int beat = 0; beat < analysis.beatsPerBar; ++beat) {
                analysis.drums.push_back({start + beat * beatSeconds,
                    secondPattern ? "Ride" : "HiHat", 100, 1.0, 1.0, "test"});
            }
        }
        return analysis;
    };

    const auto shortSections = inferSongSections(
        analysisWithChange(6, 3), 6 * barSeconds, {});
    require(shortSections.size() == 1 && shortSections.front().start == 0.0 &&
        std::abs(shortSections.front().end - 6 * barSeconds) < 1.0e-9,
        "six-bar Jam2 performance must remain one full section");

    const auto thresholdSections = inferSongSections(
        analysisWithChange(32, 16), 32 * barSeconds, {});
    require(thresholdSections.size() == 1,
        "thirty-two-bar recording must remain one section");

    constexpr int longBars = 33;
    constexpr int transitionBar = 20;
    const auto longAnalysis = analysisWithChange(longBars, transitionBar);
    const std::map<std::string, AudioBuffer> stems{
        {"drums", sine(1000, 1, longBars * barSeconds)},
        {"bass", sine(1000, 1, longBars * barSeconds)},
    };
    const auto longSections = inferSongSections(
        longAnalysis, longBars * barSeconds, stems);
    require(longSections.size() == 2,
        "recording above thirty-two bars remains eligible for section splitting");
    require(std::abs(longSections.front().end - transitionBar * barSeconds) < 1.0e-9 &&
        std::abs(longSections.back().start - transitionBar * barSeconds) < 1.0e-9,
        "long recording section split must follow measured musical change");
}

#if JAMTASTER_NATIVE_HAS_DEMUCS
void testDemucsStftRoundTrip()
{
    constexpr int frames = 8192;
    Eigen::MatrixXf source(2, frames);
    for (int frame = 0; frame < frames; ++frame) {
        source(0, frame) = 0.35F * std::sin(0.013F * static_cast<float>(frame)) +
            0.12F * std::cos(0.071F * static_cast<float>(frame));
        source(1, frame) = 0.28F * std::sin(0.019F * static_cast<float>(frame));
    }
    demucsonnx::stft_buffers buffers(frames);
    Eigen::Tensor3dXcf spectrum(2, buffers.nb_bins, buffers.nb_frames);
    demucsonnx::stft(buffers, source, spectrum);
    struct ExpectedBin {
        int frequency;
        int frame;
        float real;
        float imaginary;
    };
    static constexpr ExpectedBin expected[]{
        {0, 0, 0.847736597F, 0.0F},
        {1, 0, -0.859175801F, 2.98023224e-8F},
        {10, 1, 1.0207603F, -1.38595355F},
        {100, 2, 2.75608113e-6F, 2.30270757e-6F},
        {500, 4, -3.00348724e-9F, 5.03441271e-8F},
        {1024, 5, 1.57742761e-8F, 4.04543243e-8F},
        {2048, 8, -5.60879707e-5F, 0.0F},
    };
    for (const auto& bin : expected) {
        const auto actual = spectrum(0, bin.frequency, bin.frame);
        const float error = std::abs(actual - std::complex<float>(bin.real, bin.imaginary));
        require(error < 2.0e-5F,
            "Demucs STFT differs from PyTorch at bin " +
            std::to_string(bin.frequency) + ", frame " + std::to_string(bin.frame) +
            ": error=" + std::to_string(error));
    }
    Eigen::MatrixXf reconstructed(2, frames);
    demucsonnx::istft(buffers, spectrum, reconstructed);

    const float maximumError = (source - reconstructed).cwiseAbs().maxCoeff();
    const float sourceEnergy = source.squaredNorm();
    const float measuredGain = sourceEnergy > 0.0F
        ? source.cwiseProduct(reconstructed).sum() / sourceEnergy
        : 0.0F;
    require(maximumError < 2.0e-5F,
        "Demucs normalized STFT roundtrip: max_error=" +
        std::to_string(maximumError) + ", gain=" + std::to_string(measuredGain));
}
#endif

} // namespace

int main()
{
    try {
        testAudioBuffer();
        testWorkerProtocolJson();
        testResampler();
        testMel();
        testAdtofFeatures();
        testChordFeatures();
        testWavRoundTrip();
        testWavMixAndCrop();
        testPipelineUtilities();
        testPostprocessBoundaries();
        testNativeAnalysisAndJamJarExport();
        testNativeStretchLength();
        testSectionSplittingRequiresLongRecording();
#if JAMTASTER_NATIVE_HAS_DEMUCS
        testDemucsStftRoundTrip();
#endif
        std::cout << "JamTaster native unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster native unit test failed: " << error.what() << '\n';
        return 1;
    }
}
