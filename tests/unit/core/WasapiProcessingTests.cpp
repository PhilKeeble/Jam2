#include "wasapi_processing.hpp"
#include "duplex_processing.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void formats()
{
    using namespace jam2::audio::wasapi;
    for (auto encoding : {SampleEncoding::Float32, SampleEncoding::Pcm16,
            SampleEncoding::Pcm24, SampleEncoding::Pcm32}) {
        const unsigned bits = encoding == SampleEncoding::Pcm16 ? 16 :
            (encoding == SampleEncoding::Pcm24 ? 24 : 32);
        for (const auto count : {1U, 2U, 6U, 8U}) {
            SampleFormat format{encoding, count, bits, count > 2 ? (count == 6 ? 0x3fU : 0x63fU) : 0U};
            const std::array<std::int32_t, 4> mono{0, 1073741824, -1073741824, -2147483647 - 1};
            std::vector<std::uint8_t> output(mono.size() * format.frame_bytes(), 0xff);
            encode_output(mono, format, output);
            for (std::size_t frame = 0; frame < mono.size(); ++frame)
                for (std::size_t channel = 0; channel < count; ++channel) {
                    const auto decoded = decode_sample(output.data() +
                        (frame * count + channel) * format.bytes_per_sample(), format);
                    require(decoded == (channel < 2 ? mono[frame] : 0),
                        "mono/stereo/surround output must use only front channels in every supported PCM format");
                }
        }
    }
    SampleFormat fp{SampleEncoding::Float32, 1, 32, 0};
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        require(decode_sample(reinterpret_cast<const std::uint8_t*>(&value), fp) == 0,
            "nonfinite endpoint samples must become silence");
    }
    const std::array<float, 2> extremes{2.f, -2.f};
    require(decode_sample(reinterpret_cast<const std::uint8_t*>(&extremes[0]), fp) == INT32_MAX &&
        decode_sample(reinterpret_cast<const std::uint8_t*>(&extremes[1]), fp) == INT32_MIN,
        "endpoint float conversion must saturate");
    SampleFormat pcm{SampleEncoding::Pcm32, 1, 24, 0};
    const std::array<std::uint8_t, 4> padded{0xff, 0x56, 0x34, 0x12};
    require(decode_sample(padded.data(), pcm) == 0x12345600, "unused PCM container bits must not become signal");
    SampleFormat center{SampleEncoding::Float32, 3, 32, 0x1c}; // No front L/R speakers.
    std::array<std::uint8_t, 12> silent{}; const std::array<std::int32_t, 1> signal{123456};
    encode_output(signal, center, silent);
    require(std::all_of(silent.begin(), silent.end(), [](auto x) { return x == 0; }),
        "channel masks must not route mono into center/rear speakers");
}

void capture_lifecycle()
{
    using namespace jam2::audio;
    using namespace wasapi;
    WasapiConfig config; config.capture_target_ms = 1; config.capture_capacity_ms = 4;
    CaptureBuffer buffer; buffer.configure(2, 48000, config);
    SampleFormat format{SampleEncoding::Float32, 2, 32, 0};
    std::array<float, 256> input{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = i % 2 ? -.25f : .5f;
    const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), sizeof(input));
    std::array<std::int32_t, 32> left{}, right{};
    const std::array<std::int32_t*, 2> output{left.data(), right.data()};
    buffer.pull(output, left.size());
    require(buffer.status().capture_underrun_frames == 32 && left[0] == 0, "empty capture must return bounded silence");
    buffer.push(bytes, 128, format, false, false); buffer.pull(output, left.size());
    require(left[0] == 1073741824 && right[0] == -536870912, "capture bridge must preserve every input channel");
    buffer.push(bytes, 128, format, false, false); buffer.push(bytes, 128, format, false, false);
    require(buffer.status().capture_overrun_frames > 0 && buffer.status().capture_queued_frames <= buffer.capacity_frames(),
        "capture backlog must remain bounded with observable drops");
    buffer.push({}, 96, format, true, true); buffer.pull(output, left.size());
    require(left[0] == 0 && right[0] == 0 && buffer.status().capture_discontinuities == 1,
        "silent discontinuity must flush old samples and preserve explicit silence");
    const auto previous = buffer.status().capture_queued_frames;
    buffer.push(bytes.first(2), 128, format, false, false);
    require(buffer.status().capture_queued_frames == previous, "truncated input packet must not read out of bounds");
    buffer.reset(); require(buffer.status().capture_queued_frames == 0, "capture reset must discard old device audio");
    buffer.push(bytes, 128, format, false, false, 1000000);
    buffer.pull(output, left.size());
    require(buffer.source_time_us() == 1000000, "capture age must refer to the first resampled source frame's timestamp");
    buffer.pull(output, left.size());
    require(buffer.source_time_us() >= 1000600 && buffer.source_time_us() <= 1000700,
        "capture timestamps must follow consumption across render blocks");
    bool rejected = false;
    try { config.capture_capacity_ms = 1; buffer.configure(2, 48000, config); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid queue target/capacity must fail before device start");
}

void selection()
{
    using namespace jam2::audio;
    const std::array<DeviceInfo, 3> devices{{
        {1, "WASAPI", "Mono speaker", {}, {}, "mono", EndpointDirection::Output, 1},
        {2, "WASAPI", "Surround speaker", {}, {}, "surround", EndpointDirection::Output, 8},
        {3, "WASAPI", "Microphone", {}, {}, "input", EndpointDirection::Input, 4},
    }};
    WasapiConfig config; config.output_endpoint_id = "mono";
    auto channels = automatic_wasapi_channels(config, devices);
    require(channels.input.empty() && channels.output == std::vector<int>{0},
        "mono playback must not require an input or an unavailable second output channel");
    config.input_endpoint_id = "input"; config.output_endpoint_id = "surround";
    channels = automatic_wasapi_channels(config, devices);
    require(channels.input == std::vector<int>({0, 1, 2, 3}) && channels.output == std::vector<int>({0, 1}),
        "automatic channels must expose all capture channels and select front stereo only");
    config.input_endpoint_id = "mono";
    bool rejected = false;
    try { (void)automatic_wasapi_channels(config, devices); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "render endpoints must not be accepted as microphones");
    config.input_endpoint_id.clear(); config.output_endpoint_id = "unplugged";
    rejected = false;
    try { (void)automatic_wasapi_channels(config, devices); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "an unavailable saved output must fail without silently selecting another endpoint");
}

void independent_clocks()
{
    using namespace jam2::audio;
    using namespace wasapi;
    // Exactly four endpoint pairs: different packet periods and independent
    // oscillators. This runs the production bridge, without wall-clock sleeps.
    std::array<CaptureBuffer, 4> buffers;
    constexpr std::array<double, 4> drift{-500, -100, 200, 650};
    constexpr std::array<std::size_t, 4> packets{128, 240, 480, 96};
    std::array<double, 4> pending{};
    std::array<std::uint64_t, 4> warmUnderruns{};
    WasapiConfig config;
    for (auto& buffer : buffers) buffer.configure(1, 48000, config);
    std::array<float, 480> input{}; input.fill(.25f);
    const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), sizeof(input));
    std::array<std::int32_t, 64> scratch{};
    const std::array<std::int32_t*, 1> output{scratch.data()};
    for (int tick = 0; tick < 225000; ++tick) { // Five minutes, accelerated.
        for (std::size_t peer = 0; peer < buffers.size(); ++peer) {
            pending[peer] += 64 * (1. + drift[peer] / 1000000.);
            if (pending[peer] >= packets[peer]) {
                pending[peer] -= packets[peer];
                buffers[peer].push(bytes, packets[peer], {SampleEncoding::Float32, 1, 32, 0}, false, false);
            }
            buffers[peer].pull(output, scratch.size());
            if (tick == 7500) warmUnderruns[peer] = buffers[peer].status().capture_underrun_frames;
        }
    }
    for (std::size_t peer = 0; peer < buffers.size(); ++peer) {
        const auto status = buffers[peer].status();
        require(status.capture_overrun_frames == 0 && status.capture_underrun_frames == warmUnderruns[peer],
            "independent clocks must remain bounded without ongoing underruns/overruns after warmup");
        require(std::abs(status.capture_correction_ppm - drift[peer]) < 150,
            "capture bridge must track measured oscillator difference rather than run away");
    }
}

void duplex_integration()
{
    using namespace jam2::audio;
    using namespace device_processing;
    MonoRingBuffer capture(4096, true), pitch(4096), playback(4096);
    StreamControl control;
    control.network_capture_requested_enabled.store(true);
    control.network_capture_generation_requested.store(1);
    control.local_monitor_enabled.store(true); control.local_monitor_level_ppm.store(1000000);
    std::array<std::int32_t, 64> samples{}; samples.fill(1234567);
    capture.push(samples);
    DuplexProcessingState state;
    state.capture = &capture; state.pitch = &pitch; state.playback = &playback; state.control = &control;
    prepare_duplex_processing(state, 64, 1, 48000);
    const std::array<const std::int32_t*, 1> inputs{samples.data()};
    process_duplex_block(state, inputs, 64, true, {0, 1000});
    require(control.network_capture_generation_applied == 1 && capture.available_read() == 64 &&
        control.network_capture_stale_frames_discarded == 64, "real duplex processing must discard stale pre-join capture");
    require(state.playback_scratch[0] == samples[0] && control.engine_frame_counter == 64,
        "WASAPI/ASIO shared processing must monitor input on the output timeline");
    control.network_capture_requested_enabled.store(false); control.network_capture_generation_requested.store(2);
    process_duplex_block(state, inputs, 17, true, {64, 1333});
    require(!control.network_capture_enabled && capture.available_read() == 0 && control.engine_frame_counter == 81,
        "capture cancellation and a short render block must preserve the engine timeline");
    DuplexProcessingState listening;
    listening.capture = &capture; listening.pitch = &pitch; listening.playback = &playback; listening.control = &control;
    prepare_duplex_processing(listening, 64, 0, 48000);
    process_duplex_block(listening, {}, 64, true, {});
    require(std::all_of(listening.playback_scratch.begin(), listening.playback_scratch.end(), [](auto x) { return x == 0; }),
        "optional-input listening must render without stale physical input");
    class Instrument final : public InputSourceRenderer {
        bool render_mono(const InputSourceRenderRequest&, std::span<std::int32_t> output) noexcept override {
            std::fill(output.begin(), output.end(), 3333333); return true;
        }
    } instrument;
    InputSourceRouter router(64, 0);
    require(router.configure(0, {InputSourceKind::MidiInstrument, kNoInputChannel,
        kNoInputChannel, 1000000, true, &instrument}), "MIDI renderer configures without a microphone");
    control.input_source_router = &router;
    process_duplex_block(listening, {}, 64, true, {});
    require(listening.playback_scratch[0] == 3333333 && control.input_peak_ppm.load() > 0,
        "playback-only WASAPI must still render and meter MIDI instruments through the existing input graph");
}
} // namespace

int main()
{
    try { formats(); selection(); capture_lifecycle(); independent_clocks(); duplex_integration(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "WASAPI formats, capture clocks and duplex integration passed\n";
}
