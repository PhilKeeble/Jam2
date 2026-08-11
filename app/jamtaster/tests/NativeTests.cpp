#include "Dsp.hpp"
#include "Export.hpp"
#include "Hash.hpp"
#include "Json.hpp"
#include "Postprocess.hpp"
#include "Wav.hpp"

#if JAMTASTER_NATIVE_HAS_DEMUCS
#include "../third_party/demucs_onnx/dsp.hpp"
#endif

#include <cmath>
#include <complex>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
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

void testPartialRecordingLateSectionChange()
{
    using namespace jamtaster::native;
    Analysis analysis;
    analysis.beatsPerBar = 4;
    constexpr int barCount = 12;
    constexpr double beatSeconds = 0.5;
    constexpr double barSeconds = beatSeconds * 4.0;

    // Include the final downbeat so twelve complete bars are available.
    for (int beat = 0; beat <= barCount * analysis.beatsPerBar; ++beat)
        analysis.beats.push_back(beat * beatSeconds);
    for (int bar = 0; bar <= barCount; ++bar)
        analysis.downbeats.push_back(bar * barSeconds);

    // This is an excerpt with a real transition at bar nine and only three bars
    // after it. Chord, groove and bass evidence all change at the same downbeat.
    for (int bar = 0; bar < barCount; ++bar) {
        const double start = bar * barSeconds;
        const bool secondPattern = bar >= 9;
        analysis.chords.push_back({start, start + barSeconds,
            secondPattern ? "C" : "Am", 1.0});
        analysis.bass.push_back({start, start + 0.4,
            secondPattern ? 48 : 45, 100, 1.0});
        for (int beat = 0; beat < analysis.beatsPerBar; ++beat) {
            analysis.drums.push_back({start + beat * beatSeconds,
                secondPattern ? "Ride" : "HiHat", 100, 1.0, 1.0, "test"});
        }
    }

    const auto sections = inferSongSections(analysis, barCount * barSeconds, {});
    require(sections.size() == 2, "partial recording should have one late section change");
    require(std::abs(sections.front().end - 9.0 * barSeconds) < 1.0e-9,
        "late section change must follow evidence rather than an assumed outro length");
    require(std::abs(sections.back().start - 9.0 * barSeconds) < 1.0e-9,
        "late section must start on its detected downbeat");
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
        testPipelineUtilities();
        testNativeStretchLength();
        testPartialRecordingLateSectionChange();
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
