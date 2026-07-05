#include "audio_device.hpp"
#include "audio_ring.hpp"
#include "loopback_capture_macos.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace {

struct Options {
    int audio_device = -1;
    std::vector<int> input_channels{0};
    int sample_rate = 44100;
    long buffer_size = 128;
    int duration_ms = 0;
    std::filesystem::path output;
    std::string source = "default";
    bool trigger = false;
    double trigger_threshold_db = -45.0;
    int trigger_hold_ms = 50;
    int pre_roll_ms = 250;
    double tail_silence_db = -50.0;
    int tail_silence_ms = 1000;
    bool trim_leading_silence = true;
    bool trim_trailing_silence = true;
    bool summary_json = true;
};

const char* usage = R"(jam2-capture - Jam2 WAV capture utility

Usage:
  jam2-capture --help
  jam2-capture list-devices
  jam2-capture list-loopback-sources
  jam2-capture record-input --audio-device n [--input-channels 1[,2...]] [--sample-rate n] [--buffer-size n] [--duration-ms n] --output file.wav
  jam2-capture record-loopback [--source default|n] [--duration-ms n] --output file.wav

Capture options:
  --trigger off|on
  --trigger-threshold-db n
  --trigger-hold-ms n
  --pre-roll-ms n
  --tail-silence-db n
  --tail-silence-ms n
  --trim-leading-silence on|off
  --trim-trailing-silence on|off
  --summary-json on|off

Notes:
  record-input uses the existing Jam2 host audio backend and writes mono PCM16 WAV.
  selected input channels are mixed by the backend into the capture stream.
  record-loopback uses WASAPI loopback on Windows or CoreAudio process taps on macOS 14.2+, and writes mono PCM16 WAV.
)";

std::atomic<bool> g_stop_requested{false};

std::string_view require_value(int argc, char** argv, int& index, std::string_view option)
{
    if (index + 1 >= argc) {
        throw std::runtime_error("missing value for " + std::string(option));
    }
    ++index;
    return argv[index];
}

std::vector<int> parse_channels(std::string_view text)
{
    std::vector<int> channels;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t comma = text.find(',', pos);
        const std::string part{text.substr(pos, comma == std::string_view::npos ? text.size() - pos : comma - pos)};
        if (part.empty()) {
            throw std::runtime_error("--input-channels contains an empty channel");
        }
        const int one_based = std::stoi(part);
        if (one_based <= 0) {
            throw std::runtime_error("--input-channels uses positive 1-based channel numbers");
        }
        channels.push_back(one_based - 1);
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    std::sort(channels.begin(), channels.end());
    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
    return channels;
}

bool parse_on_off(std::string_view text, std::string_view option)
{
    if (text == "on") {
        return true;
    }
    if (text == "off") {
        return false;
    }
    throw std::runtime_error(std::string(option) + " must be on or off");
}

void parse_capture_option(Options& options, int argc, char** argv, int& i, std::string_view arg)
{
    if (arg == "--trigger") {
        options.trigger = parse_on_off(require_value(argc, argv, i, arg), arg);
    } else if (arg == "--trigger-threshold-db") {
        options.trigger_threshold_db = std::stod(std::string(require_value(argc, argv, i, arg)));
    } else if (arg == "--trigger-hold-ms") {
        options.trigger_hold_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
    } else if (arg == "--pre-roll-ms") {
        options.pre_roll_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
    } else if (arg == "--tail-silence-db") {
        options.tail_silence_db = std::stod(std::string(require_value(argc, argv, i, arg)));
    } else if (arg == "--tail-silence-ms") {
        options.tail_silence_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
    } else if (arg == "--trim-leading-silence") {
        options.trim_leading_silence = parse_on_off(require_value(argc, argv, i, arg), arg);
    } else if (arg == "--trim-trailing-silence") {
        options.trim_trailing_silence = parse_on_off(require_value(argc, argv, i, arg), arg);
    } else if (arg == "--summary-json") {
        options.summary_json = parse_on_off(require_value(argc, argv, i, arg), arg);
    } else {
        throw std::runtime_error("unknown option: " + std::string(arg));
    }
}

Options parse_record_input(int argc, char** argv)
{
    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--audio-device") {
            options.audio_device = std::stoi(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--input-channels") {
            options.input_channels = parse_channels(require_value(argc, argv, i, arg));
        } else if (arg == "--sample-rate") {
            options.sample_rate = std::stoi(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--buffer-size") {
            options.buffer_size = std::stol(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--duration-ms") {
            options.duration_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--output") {
            options.output = std::filesystem::path(std::string(require_value(argc, argv, i, arg)));
        } else {
            parse_capture_option(options, argc, argv, i, arg);
        }
    }
    if (options.audio_device < 0) {
        throw std::runtime_error("record-input requires --audio-device");
    }
    if (options.output.empty()) {
        throw std::runtime_error("record-input requires --output");
    }
    if (options.sample_rate <= 0 || options.buffer_size <= 0 || options.duration_ms < 0) {
        throw std::runtime_error("sample rate and buffer size must be positive, and duration must be non-negative");
    }
    return options;
}

Options parse_record_loopback(int argc, char** argv)
{
    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--source") {
            options.source = std::string(require_value(argc, argv, i, arg));
        } else if (arg == "--duration-ms") {
            options.duration_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--output") {
            options.output = std::filesystem::path(std::string(require_value(argc, argv, i, arg)));
        } else {
            parse_capture_option(options, argc, argv, i, arg);
        }
    }
    if (options.output.empty()) {
        throw std::runtime_error("record-loopback requires --output");
    }
    if (options.duration_ms < 0) {
        throw std::runtime_error("--duration-ms must be non-negative");
    }
    return options;
}

void write_u16(std::ofstream& out, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void write_u32(std::ofstream& out, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
        static_cast<char>((value >> 16) & 0xffU),
        static_cast<char>((value >> 24) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void write_wav_header(std::ofstream& out, int sample_rate, std::uint32_t frames)
{
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    const std::uint32_t data_bytes = frames * channels * (bits / 8);
    out.write("RIFF", 4);
    write_u32(out, 36U + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(out, 16);
    write_u16(out, 1);
    write_u16(out, channels);
    write_u32(out, static_cast<std::uint32_t>(sample_rate));
    write_u32(out, static_cast<std::uint32_t>(sample_rate * channels * (bits / 8)));
    write_u16(out, channels * (bits / 8));
    write_u16(out, bits);
    out.write("data", 4);
    write_u32(out, data_bytes);
}

void write_wav_file(const std::filesystem::path& output, int sample_rate, const std::vector<std::int16_t>& samples)
{
    const std::filesystem::path parent = output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open output WAV: " + output.string());
    }
    write_wav_header(out, sample_rate, static_cast<std::uint32_t>(samples.size()));
    for (const std::int16_t sample : samples) {
        write_u16(out, static_cast<std::uint16_t>(sample));
    }
}

std::int16_t to_i16(std::int32_t sample)
{
    const std::int32_t shifted = sample >> 16;
    return static_cast<std::int16_t>(std::clamp(shifted, -32768, 32767));
}

double amp_from_db(double db)
{
    return std::pow(10.0, db / 20.0);
}

double amp_i16(std::int16_t sample)
{
    return std::abs(static_cast<double>(sample)) / 32768.0;
}

struct CaptureResult {
    std::vector<std::int16_t> samples;
    std::uint64_t raw_frames = 0;
    std::uint64_t pre_roll_used_frames = 0;
    std::uint64_t trimmed_leading_frames = 0;
    std::uint64_t trimmed_trailing_frames = 0;
    bool trigger_enabled = false;
    bool trigger_fired = false;
    bool stopped_by_request = false;
    double peak = 0.0;
};

class CaptureAccumulator {
public:
    CaptureAccumulator(const Options& options, int sample_rate)
        : options_(options),
          sample_rate_(sample_rate),
          trigger_threshold_(amp_from_db(options.trigger_threshold_db)),
          tail_threshold_(amp_from_db(options.tail_silence_db)),
          trigger_hold_frames_(static_cast<std::uint64_t>(std::max(0, options.trigger_hold_ms)) * sample_rate / 1000),
          pre_roll_frames_(static_cast<std::size_t>(std::max(0, options.pre_roll_ms) * sample_rate / 1000)),
          tail_silence_frames_(static_cast<std::uint64_t>(std::max(0, options.tail_silence_ms)) * sample_rate / 1000)
    {
        result_.trigger_enabled = options.trigger;
        pre_roll_.reserve(pre_roll_frames_);
    }

    void push(std::int16_t sample)
    {
        ++result_.raw_frames;
        const double amp = amp_i16(sample);
        result_.peak = std::max(result_.peak, amp);
        if (options_.trigger && !result_.trigger_fired) {
            push_pre_roll(sample);
            if (amp >= trigger_threshold_) {
                ++trigger_hold_count_;
            } else {
                trigger_hold_count_ = 0;
            }
            if (trigger_hold_count_ >= std::max<std::uint64_t>(trigger_hold_frames_, 1)) {
                result_.trigger_fired = true;
                result_.pre_roll_used_frames = pre_roll_.size();
                append_pre_roll();
                pre_roll_.clear();
            }
            return;
        }
        result_.trigger_fired = true;
        result_.samples.push_back(sample);
    }

    CaptureResult finish(bool stopped_by_request)
    {
        result_.stopped_by_request = stopped_by_request;
        trim();
        return result_;
    }

private:
    void push_pre_roll(std::int16_t sample)
    {
        if (pre_roll_frames_ == 0) {
            return;
        }
        if (pre_roll_.size() < pre_roll_frames_) {
            pre_roll_.push_back(sample);
            return;
        }
        pre_roll_[pre_roll_index_] = sample;
        pre_roll_index_ = (pre_roll_index_ + 1) % pre_roll_.size();
    }

    void append_pre_roll()
    {
        if (pre_roll_.empty()) {
            return;
        }
        if (pre_roll_.size() < pre_roll_frames_) {
            result_.samples.insert(result_.samples.end(), pre_roll_.begin(), pre_roll_.end());
            return;
        }
        result_.samples.insert(result_.samples.end(), pre_roll_.begin() + static_cast<std::ptrdiff_t>(pre_roll_index_), pre_roll_.end());
        result_.samples.insert(result_.samples.end(), pre_roll_.begin(), pre_roll_.begin() + static_cast<std::ptrdiff_t>(pre_roll_index_));
    }

    void trim()
    {
        if (result_.samples.empty()) {
            return;
        }
        std::size_t first = 0;
        std::size_t last = result_.samples.size();
        if (options_.trim_leading_silence) {
            while (first < last && amp_i16(result_.samples[first]) < tail_threshold_) {
                ++first;
            }
        }
        if (options_.trim_trailing_silence) {
            std::uint64_t silent = 0;
            while (last > first) {
                if (amp_i16(result_.samples[last - 1]) < tail_threshold_) {
                    ++silent;
                    --last;
                    if (tail_silence_frames_ > 0 && silent >= tail_silence_frames_) {
                        while (last > first && amp_i16(result_.samples[last - 1]) < tail_threshold_) {
                            --last;
                        }
                        break;
                    }
                } else {
                    break;
                }
            }
        }
        result_.trimmed_leading_frames = first;
        result_.trimmed_trailing_frames = result_.samples.size() - last;
        if (first > 0 || last < result_.samples.size()) {
            result_.samples = std::vector<std::int16_t>(result_.samples.begin() + static_cast<std::ptrdiff_t>(first), result_.samples.begin() + static_cast<std::ptrdiff_t>(last));
        }
    }

    const Options& options_;
    int sample_rate_ = 0;
    double trigger_threshold_ = 0.0;
    double tail_threshold_ = 0.0;
    std::uint64_t trigger_hold_frames_ = 0;
    std::size_t pre_roll_frames_ = 0;
    std::uint64_t tail_silence_frames_ = 0;
    std::uint64_t trigger_hold_count_ = 0;
    std::vector<std::int16_t> pre_roll_;
    std::size_t pre_roll_index_ = 0;
    CaptureResult result_;
};

void start_stdin_stop_thread()
{
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "stop" || line == "quit" || line == "exit") {
                g_stop_requested.store(true, std::memory_order_relaxed);
                break;
            }
        }
    }).detach();
}

std::string json_escape(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

void write_summary(const Options& options, const std::string& mode, int sample_rate, const CaptureResult& result)
{
    const std::filesystem::path absolute = std::filesystem::absolute(options.output);
    const std::filesystem::path sidecar = absolute.string() + ".json";
    std::ostringstream json;
    json << "{"
         << "\"event\":\"capture.summary\","
         << "\"mode\":\"" << mode << "\","
         << "\"output\":\"" << json_escape(absolute.string()) << "\","
         << "\"sidecar\":\"" << json_escape(sidecar.string()) << "\","
         << "\"sample_rate\":" << sample_rate << ","
         << "\"channels\":1,"
         << "\"frames_written\":" << result.samples.size() << ","
         << "\"raw_frames\":" << result.raw_frames << ","
         << "\"duration_ms\":" << (result.samples.size() * 1000ULL / static_cast<unsigned long long>(std::max(sample_rate, 1))) << ","
         << "\"peak\":" << result.peak << ","
         << "\"trigger_enabled\":" << (result.trigger_enabled ? "true" : "false") << ","
         << "\"trigger_fired\":" << (result.trigger_fired ? "true" : "false") << ","
         << "\"stopped_by_request\":" << (result.stopped_by_request ? "true" : "false") << ","
         << "\"pre_roll_used_frames\":" << result.pre_roll_used_frames << ","
         << "\"trimmed_leading_frames\":" << result.trimmed_leading_frames << ","
         << "\"trimmed_trailing_frames\":" << result.trimmed_trailing_frames
         << "}";
    std::ofstream meta(sidecar, std::ios::binary | std::ios::trunc);
    meta << json.str() << "\n";
    if (options.summary_json) {
        std::cout << json.str() << "\n";
    }
}

int list_devices()
{
    const auto devices = jam2::audio::list_devices();
    if (devices.empty()) {
        std::cout << "No audio devices found for this backend.\n";
        return 0;
    }
    for (const auto& device : devices) {
        std::cout << "[" << device.id << "] " << device.backend << " " << device.name << "\n";
    }
    return 0;
}

int record_input(const Options& options)
{
    const std::uint64_t target_frames = options.duration_ms > 0
        ? static_cast<std::uint64_t>((static_cast<std::int64_t>(options.sample_rate) * options.duration_ms) / 1000)
        : std::numeric_limits<std::uint64_t>::max();
    g_stop_requested.store(false, std::memory_order_relaxed);
    start_stdin_stop_thread();

    jam2::audio::MonoRingBuffer capture_ring(static_cast<std::size_t>(options.sample_rate * 4));
    jam2::audio::MonoRingBuffer playback_ring(static_cast<std::size_t>(options.sample_rate));
    jam2::audio::StreamControl control;
    jam2::audio::ChannelSelection channels;
    channels.input = options.input_channels;
    channels.output = {0, 1};

    auto stream = jam2::audio::start_duplex_stream(
        options.audio_device,
        options.sample_rate,
        options.buffer_size,
        options.input_channels.size() >= 2 ? jam2::audio::InputChannels::Stereo : jam2::audio::InputChannels::Mono,
        channels,
        capture_ring,
        playback_ring,
        0,
        control);

    std::vector<std::int32_t> frames(static_cast<std::size_t>(std::max<long>(options.buffer_size, 256)));
    std::uint64_t raw_read = 0;
    CaptureAccumulator capture(options, options.sample_rate);
    while (raw_read < target_frames && !g_stop_requested.load(std::memory_order_relaxed)) {
        const std::size_t want = std::min<std::size_t>(frames.size(), target_frames - raw_read);
        const std::size_t got = capture_ring.pop(std::span<std::int32_t>(frames.data(), want));
        for (std::size_t i = 0; i < got; ++i) {
            capture.push(to_i16(frames[i]));
        }
        raw_read += static_cast<std::uint64_t>(got);
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    CaptureResult result = capture.finish(g_stop_requested.load(std::memory_order_relaxed));
    write_wav_file(options.output, options.sample_rate, result.samples);
    write_summary(options, "record-input", options.sample_rate, result);
    const auto ring_stats = capture_ring.stats();
    const auto info = stream->info();
    std::cout << "Mode: record-input\n";
    std::cout << "Device: [" << info.device.id << "] " << info.device.backend << " " << info.device.name << "\n";
    std::cout << "Sample rate: " << options.sample_rate << "\n";
    std::cout << "Buffer size: " << options.buffer_size << "\n";
    std::cout << "Duration ms: " << (options.duration_ms > 0 ? std::to_string(options.duration_ms) : std::string("manual")) << "\n";
    std::cout << "Frames written: " << result.samples.size() << "\n";
    std::cout << "Raw frames: " << result.raw_frames << "\n";
    std::cout << "Peak: " << result.peak << "\n";
    std::cout << "Trimmed leading frames: " << result.trimmed_leading_frames << "\n";
    std::cout << "Trimmed trailing frames: " << result.trimmed_trailing_frames << "\n";
    std::cout << "Capture ring overruns: " << ring_stats.overruns << "\n";
    std::cout << "Capture ring overrun events: " << ring_stats.overrun_events << "\n";
    std::cout << "Output: " << std::filesystem::absolute(options.output).string() << "\n";
    return 0;
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

void check_hr(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        std::ostringstream out;
        out << message << " hr=0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(out.str());
    }
}

struct ComApartment {
    ComApartment()
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            check_hr(hr, "CoInitializeEx failed");
        }
    }

    ~ComApartment()
    {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }

    HRESULT hr = S_OK;
};

std::wstring wide_from_prop(const PROPVARIANT& value)
{
    if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        return value.pwszVal;
    }
    return L"";
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring endpoint_name(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return L"Unknown";
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"Unknown";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value))) {
        name = wide_from_prop(value);
    }
    PropVariantClear(&value);
    return name;
}

ComPtr<IMMDeviceEnumerator> make_enumerator()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    check_hr(CoCreateInstance(
                 __uuidof(MMDeviceEnumerator),
                 nullptr,
                 CLSCTX_ALL,
                 __uuidof(IMMDeviceEnumerator),
                 reinterpret_cast<void**>(enumerator.GetAddressOf())),
        "failed to create WASAPI device enumerator");
    return enumerator;
}

ComPtr<IMMDevice> loopback_device_for_source(IMMDeviceEnumerator* enumerator, const std::string& source)
{
    if (source.empty() || source == "default") {
        ComPtr<IMMDevice> device;
        check_hr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "failed to get default render endpoint");
        return device;
    }
    std::size_t consumed = 0;
    const int index = std::stoi(source, &consumed);
    if (consumed != source.size() || index < 0) {
        throw std::runtime_error("--source must be default or a numeric source id from list-loopback-sources");
    }
    ComPtr<IMMDeviceCollection> collection;
    check_hr(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "failed to enumerate render endpoints");
    UINT count = 0;
    check_hr(collection->GetCount(&count), "failed to count render endpoints");
    if (static_cast<UINT>(index) >= count) {
        throw std::runtime_error("loopback source id out of range");
    }
    ComPtr<IMMDevice> device;
    check_hr(collection->Item(static_cast<UINT>(index), &device), "failed to get render endpoint");
    return device;
}

double read_sample(const BYTE* frame, WORD channels, WORD bits_per_sample, WORD format_tag, const GUID* sub_format, WORD channel)
{
    const BYTE* sample = frame + (static_cast<std::size_t>(channel) * bits_per_sample / 8);
    const bool is_float =
        format_tag == WAVE_FORMAT_IEEE_FLOAT ||
        (format_tag == WAVE_FORMAT_EXTENSIBLE && sub_format != nullptr && IsEqualGUID(*sub_format, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    const bool is_pcm =
        format_tag == WAVE_FORMAT_PCM ||
        (format_tag == WAVE_FORMAT_EXTENSIBLE && sub_format != nullptr && IsEqualGUID(*sub_format, KSDATAFORMAT_SUBTYPE_PCM));
    if (is_float && bits_per_sample == 32) {
        float value = 0.0F;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(static_cast<double>(value), -1.0, 1.0);
    }
    if (is_pcm && bits_per_sample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    if (is_pcm && bits_per_sample == 24) {
        std::int32_t value =
            static_cast<std::int32_t>(sample[0]) |
            (static_cast<std::int32_t>(sample[1]) << 8) |
            (static_cast<std::int32_t>(sample[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<std::int32_t>(0xff000000);
        }
        return static_cast<double>(value) / 8388608.0;
    }
    if (is_pcm && bits_per_sample == 32) {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 2147483648.0;
    }
    (void)channels;
    return 0.0;
}

std::int16_t double_to_i16(double value)
{
    const double clamped = std::clamp(value, -1.0, 1.0);
    return static_cast<std::int16_t>(std::lrint(clamped * 32767.0));
}

int list_loopback_sources()
{
    ComApartment apartment;
    auto enumerator = make_enumerator();
    auto default_device = loopback_device_for_source(enumerator.Get(), "default");
    std::cout << "[default] WASAPI " << narrow(endpoint_name(default_device.Get())) << "\n";

    ComPtr<IMMDeviceCollection> collection;
    check_hr(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "failed to enumerate render endpoints");
    UINT count = 0;
    check_hr(collection->GetCount(&count), "failed to count render endpoints");
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        check_hr(collection->Item(i, &device), "failed to get render endpoint");
        std::cout << "[" << i << "] WASAPI " << narrow(endpoint_name(device.Get())) << "\n";
    }
    return 0;
}

int record_loopback(const Options& options)
{
    const std::filesystem::path parent = options.output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    ComApartment apartment;
    auto enumerator = make_enumerator();
    auto device = loopback_device_for_source(enumerator.Get(), options.source);

    ComPtr<IAudioClient> audio_client;
    check_hr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf())),
        "failed to activate WASAPI audio client");

    WAVEFORMATEX* raw_format = nullptr;
    check_hr(audio_client->GetMixFormat(&raw_format), "failed to get WASAPI mix format");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix_format(raw_format, CoTaskMemFree);
    const auto* extensible = mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_format.get())
        : nullptr;
    const GUID* sub_format = extensible != nullptr ? &extensible->SubFormat : nullptr;
    const int sample_rate = static_cast<int>(mix_format->nSamplesPerSec);
    const WORD channels = mix_format->nChannels;
    const WORD bits_per_sample = mix_format->wBitsPerSample;
    const std::uint64_t target_frames = options.duration_ms > 0
        ? static_cast<std::uint64_t>((static_cast<std::int64_t>(sample_rate) * options.duration_ms) / 1000)
        : std::numeric_limits<std::uint64_t>::max();
    g_stop_requested.store(false, std::memory_order_relaxed);
    start_stdin_stop_thread();

    REFERENCE_TIME buffer_duration = 10000000;
    check_hr(audio_client->Initialize(
                 AUDCLNT_SHAREMODE_SHARED,
                 AUDCLNT_STREAMFLAGS_LOOPBACK,
                 buffer_duration,
                 0,
                 mix_format.get(),
                 nullptr),
        "failed to initialize WASAPI loopback capture");

    ComPtr<IAudioCaptureClient> capture_client;
    check_hr(audio_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_client.GetAddressOf())),
        "failed to get WASAPI capture client");

    check_hr(audio_client->Start(), "failed to start WASAPI loopback capture");
    std::uint64_t written = 0;
    std::uint64_t silent_frames = 0;
    CaptureAccumulator capture(options, sample_rate);
    while (written < target_frames && !g_stop_requested.load(std::memory_order_relaxed)) {
        UINT32 packet_frames = 0;
        check_hr(capture_client->GetNextPacketSize(&packet_frames), "failed to read WASAPI packet size");
        if (packet_frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames_available = 0;
        DWORD flags = 0;
        check_hr(capture_client->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr), "failed to get WASAPI capture buffer");
        const UINT32 frames_to_write = static_cast<UINT32>(std::min<std::uint64_t>(frames_available, target_frames - written));
        for (UINT32 frame = 0; frame < frames_to_write; ++frame) {
            double mono = 0.0;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                const BYTE* frame_data = data + static_cast<std::size_t>(frame) * mix_format->nBlockAlign;
                for (WORD channel = 0; channel < channels; ++channel) {
                    mono += read_sample(frame_data, channels, bits_per_sample, mix_format->wFormatTag, sub_format, channel);
                }
                mono /= static_cast<double>(std::max<WORD>(channels, 1));
            } else {
                ++silent_frames;
            }
            capture.push(double_to_i16(mono));
        }
        written += frames_to_write;
        check_hr(capture_client->ReleaseBuffer(frames_available), "failed to release WASAPI capture buffer");
    }
    check_hr(audio_client->Stop(), "failed to stop WASAPI loopback capture");
    CaptureResult result = capture.finish(g_stop_requested.load(std::memory_order_relaxed));
    write_wav_file(options.output, sample_rate, result.samples);
    write_summary(options, "record-loopback", sample_rate, result);

    std::cout << "Mode: record-loopback\n";
    std::cout << "Source: " << options.source << " " << narrow(endpoint_name(device.Get())) << "\n";
    std::cout << "Mix sample rate: " << sample_rate << "\n";
    std::cout << "Mix channels: " << channels << "\n";
    std::cout << "Mix bits per sample: " << bits_per_sample << "\n";
    std::cout << "Duration ms: " << (options.duration_ms > 0 ? std::to_string(options.duration_ms) : std::string("manual")) << "\n";
    std::cout << "Frames written: " << result.samples.size() << "\n";
    std::cout << "Raw frames: " << result.raw_frames << "\n";
    std::cout << "Silent frames: " << silent_frames << "\n";
    std::cout << "Peak: " << result.peak << "\n";
    std::cout << "Trimmed leading frames: " << result.trimmed_leading_frames << "\n";
    std::cout << "Trimmed trailing frames: " << result.trimmed_trailing_frames << "\n";
    std::cout << "Output: " << std::filesystem::absolute(options.output).string() << "\n";
    return 0;
}

#elif defined(__APPLE__)

int list_loopback_sources()
{
    return list_loopback_sources_macos();
}

int record_loopback(const Options& options)
{
    g_stop_requested.store(false, std::memory_order_relaxed);
    start_stdin_stop_thread();
    MacLoopbackOptions mac_options;
    mac_options.source = options.source;
    mac_options.duration_ms = options.duration_ms;
    mac_options.stop_requested = &g_stop_requested;
    mac_options.output = options.output;
    mac_options.trigger = options.trigger;
    mac_options.trigger_threshold_db = options.trigger_threshold_db;
    mac_options.trigger_hold_ms = options.trigger_hold_ms;
    mac_options.pre_roll_ms = options.pre_roll_ms;
    mac_options.tail_silence_db = options.tail_silence_db;
    mac_options.tail_silence_ms = options.tail_silence_ms;
    mac_options.trim_leading_silence = options.trim_leading_silence;
    mac_options.trim_trailing_silence = options.trim_trailing_silence;
    mac_options.summary_json = options.summary_json;
    return record_loopback_macos(mac_options);
}

#else

int list_loopback_sources()
{
    std::cout << "Loopback capture is implemented only on Windows and macOS 14.2+ in this pass.\n";
    return 0;
}

int record_loopback(const Options&)
{
    std::cerr << "record-loopback is implemented only on Windows and macOS 14.2+ in this pass.\n";
    return 2;
}

#endif

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            std::cout << usage;
            return 0;
        }
        const std::string_view command = argv[1];
        if (command == "list-devices") {
            return list_devices();
        }
        if (command == "list-loopback-sources") {
            return list_loopback_sources();
        }
        if (command == "record-input") {
            return record_input(parse_record_input(argc, argv));
        }
        if (command == "record-loopback") {
            return record_loopback(parse_record_loopback(argc, argv));
        }
        throw std::runtime_error("unknown command: " + std::string(command));
    } catch (const std::exception& error) {
        std::cerr << "jam2-capture: " << error.what() << "\n";
        return 1;
    }
}
