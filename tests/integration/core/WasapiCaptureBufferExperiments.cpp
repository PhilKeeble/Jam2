#include "wasapi_processing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool okay, const char* reason) { if (!okay) throw std::runtime_error(reason); }

struct EndpointPair {
    double driftPpm;
    double phaseUs;
};

struct Result {
    std::uint64_t underruns = 0, overruns = 0;
    std::size_t minimumDepth = std::numeric_limits<std::size_t>::max(), maximumDepth = 0;
    int correctionPpm = 0;
    std::vector<double> ageMs;
};

Result simulate(int targetMs, EndpointPair pair, int impairment)
{
    using namespace jam2::audio;
    constexpr int rate = 48000, packet = 480;
    constexpr double origin = 1000000., durationUs = 180000000.;
    WasapiConfig config; config.capture_target_ms = targetMs;
    wasapi::CaptureBuffer bridge; bridge.configure(1, rate, config);
    std::array<float, packet> samples; samples.fill(.25f);
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(samples.data()), sizeof(samples));
    std::array<std::int32_t, 128> scratch{};
    const std::array<std::int32_t*, 1> output{scratch.data()};
    const double capturePeriod = 10000. / (1. + pair.driftPpm / 1000000.);
    const auto captureArrival = [&](std::uint64_t index) {
        // Delivery latency is positive; no timestamp is moved into the future.
        const double jitter = impairment == 0 ? 0. : static_cast<double>((index * 37) % 11) * 200.;
        const double late = impairment == 2 && index % 997 == 0 ? 6000. : 0.;
        return pair.phaseUs + index * capturePeriod + jitter + late;
    };
    Result result;
    std::uint64_t captureIndex = 1, warmUnderruns = 0, warmOverruns = 0;
    for (std::uint64_t renderIndex = 1; renderIndex * 10000. <= durationUs; ++renderIndex) {
        // A whole output period becomes available at once. The live backend
        // drains capture first, then consumes 128/128/128/96 processing blocks.
        const double renderTime = renderIndex * 10000.;
        while (captureArrival(captureIndex) <= renderTime) {
            const auto firstTime = static_cast<std::uint64_t>(origin + pair.phaseUs +
                (captureIndex - 1) * capturePeriod);
            bridge.push(bytes, packet, {wasapi::SampleEncoding::Float32, 1, 32, 0},
                false, false, firstTime);
            ++captureIndex;
        }
        for (std::size_t offset = 0; offset < packet;) {
            const auto frames = std::min<std::size_t>(scratch.size(), packet - offset);
            bridge.pull(output, frames);
            if (renderTime > 10000000.) {
                const auto status = bridge.status();
                result.minimumDepth = std::min(result.minimumDepth, status.capture_queued_frames);
                result.maximumDepth = std::max(result.maximumDepth, status.capture_queued_frames);
                const auto source = bridge.source_time_us();
                if (source != 0) result.ageMs.push_back((origin + renderTime - source) / 1000.);
            }
            offset += frames;
        }
        if (renderIndex == 1000) {
            warmUnderruns = bridge.status().capture_underrun_frames;
            warmOverruns = bridge.status().capture_overrun_frames;
        }
    }
    const auto status = bridge.status();
    result.underruns = status.capture_underrun_frames - warmUnderruns;
    result.overruns = status.capture_overrun_frames - warmOverruns;
    result.correctionPpm = status.capture_correction_ppm;
    std::sort(result.ageMs.begin(), result.ageMs.end());
    require(!result.ageMs.empty(), "capture experiment did not consume timestamped input");
    require(result.maximumDepth <= bridge.capacity_frames(), "capture queue must remain bounded");
    require(result.overruns == 0, "bounded test delivery must not overflow the capture queue");
    return result;
}
}

int main(int argc, char** argv)
{
    try {
        const auto root = argc > 1 && *argv[1] ? std::filesystem::path(argv[1]) :
            std::filesystem::temp_directory_path() / "jam2-capture-buffer";
        std::filesystem::create_directories(root);
        std::ofstream csv(root / "capture-buffer-simulation.csv");
        require(csv.good(), "create capture-buffer simulation artifact");
        csv << "target_ms,impairment,endpoint,drift_ppm,phase_us,steady_seconds,underrun_frames,overrun_frames,"
            "minimum_depth_frames,maximum_depth_frames,final_correction_ppm,age_min_ms,age_median_ms,age_p95_ms,age_max_ms\n";
        // Exactly four independent endpoint pairs for every target/impairment.
        constexpr std::array<EndpointPair, 4> pairs{{{-500, 0}, {-100, 1000}, {200, 4500}, {650, 9000}}};
        bool lowTargetFailed = false;
        for (const int target : {20, 18, 17, 16, 15, 10, 5, 1}) for (int impairment = 0; impairment < 3; ++impairment)
            for (std::size_t peer = 0; peer < pairs.size(); ++peer) {
                const auto result = simulate(target, pairs[peer], impairment);
                if (target == 20 && impairment == 0)
                    require(result.underruns == 0, "default target must sustain clean independent 10ms clocks after warmup");
                if (target == 1 && result.underruns > 0) lowTargetFailed = true;
                csv << target << ',' << impairment << ',' << peer << ',' << pairs[peer].driftPpm << ',' <<
                    pairs[peer].phaseUs << ",170," << result.underruns << ',' << result.overruns << ',' <<
                    result.minimumDepth << ',' << result.maximumDepth << ',' << result.correctionPpm << ',' <<
                    result.ageMs.front() << ',' << result.ageMs[result.ageMs.size() / 2] << ',' <<
                    result.ageMs[(result.ageMs.size() - 1) * 95 / 100] << ',' << result.ageMs.back() << '\n';
            }
        require(lowTargetFailed, "experiment must expose insufficient runway at the 1ms target");
        csv.flush(); require(csv.good(), "retain capture-buffer simulation measurements");
        std::cout << "Capture-buffer production bridge sweep passed; measurements: " <<
            (root / "capture-buffer-simulation.csv").string() << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
