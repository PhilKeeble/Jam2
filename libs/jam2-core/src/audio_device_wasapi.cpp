#include "wasapi_device.hpp"
#include "wasapi_processing.hpp"
#include "duplex_processing.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <future>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace jam2::audio {
namespace {
using Microsoft::WRL::ComPtr;

class WasapiError final : public std::runtime_error {
public:
    WasapiError(const char* operation, HRESULT hr)
        : std::runtime_error(std::string(operation) + " (WASAPI HRESULT " +
            std::to_string(static_cast<long>(hr)) + ")"), code(hr) {}
    HRESULT code;
};
void check(HRESULT hr, const char* operation) { if (FAILED(hr)) throw WasapiError(operation, hr); }

struct ComApartment {
    ComApartment() {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) return; // Enumeration may run on a Qt STA.
        check(hr, "Initialize COM"); initialized = true;
    }
    ~ComApartment() { if (initialized) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    bool initialized = false;
};

struct HandleCloser { void operator()(void* handle) const noexcept { if (handle) CloseHandle(handle); } };
using Handle = std::unique_ptr<void, HandleCloser>;
Handle event_handle() {
    Handle handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!handle) throw WasapiError("Create audio event", HRESULT_FROM_WIN32(GetLastError()));
    return handle;
}
struct TaskAllocation { void operator()(WAVEFORMATEX* value) const noexcept { CoTaskMemFree(value); } };
using WaveFormat = std::unique_ptr<WAVEFORMATEX, TaskAllocation>;

std::string utf8(const wchar_t* value) {
    if (!value) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw WasapiError("Convert endpoint name", HRESULT_FROM_WIN32(GetLastError()));
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), length, nullptr, nullptr))
        throw WasapiError("Convert endpoint name", HRESULT_FROM_WIN32(GetLastError()));
    result.pop_back(); return result;
}
std::wstring wide(const std::string& value) {
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (length <= 0) throw std::invalid_argument("invalid Windows audio endpoint ID");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), length))
        throw WasapiError("Convert endpoint ID", HRESULT_FROM_WIN32(GetLastError()));
    result.pop_back(); return result;
}
ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&result)), "Enumerate Windows audio");
    return result;
}
ComPtr<IMMDevice> endpoint(const std::string& id, EDataFlow direction) {
    auto devices = enumerator(); ComPtr<IMMDevice> result;
    check(devices->GetDevice(wide(id).c_str(), &result), "Find selected Windows audio endpoint");
    ComPtr<IMMEndpoint> information; check(result.As(&information), "Inspect Windows audio endpoint");
    EDataFlow flow{}; check(information->GetDataFlow(&flow), "Read endpoint direction");
    if (flow != direction) throw std::invalid_argument("Windows audio endpoint has the wrong direction");
    return result;
}
std::string endpoint_name(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    check(device->OpenPropertyStore(STGM_READ, &properties), "Read Windows audio device properties");
    PROPVARIANT value; PropVariantInit(&value);
    const auto hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::string result;
    try { check(hr, "Read Windows audio device name");
        if (value.vt == VT_LPWSTR) result = utf8(value.pwszVal);
    } catch (...) { PropVariantClear(&value); throw; }
    PropVariantClear(&value); return result.empty() ? "Windows audio endpoint" : result;
}

wasapi::SampleFormat sample_format(const WAVEFORMATEX& wave) {
    wasapi::SampleFormat result;
    result.channels = wave.nChannels; result.valid_bits = wave.wBitsPerSample;
    WORD tag = wave.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= 22) {
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        result.channel_mask = extended.dwChannelMask;
        result.valid_bits = extended.Samples.wValidBitsPerSample;
        if (extended.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (extended.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wave.wBitsPerSample == 32)
        result.encoding = wasapi::SampleEncoding::Float32;
    else if (tag == WAVE_FORMAT_PCM && wave.wBitsPerSample == 16)
        result.encoding = wasapi::SampleEncoding::Pcm16;
    else if (tag == WAVE_FORMAT_PCM && wave.wBitsPerSample == 24)
        result.encoding = wasapi::SampleEncoding::Pcm24;
    else if (tag == WAVE_FORMAT_PCM && wave.wBitsPerSample == 32)
        result.encoding = wasapi::SampleEncoding::Pcm32;
    else throw std::runtime_error("unsupported WASAPI endpoint sample format");
    if (!result.valid() || result.frame_bytes() != wave.nBlockAlign)
        throw std::runtime_error("invalid WASAPI endpoint channel/sample layout");
    return result;
}

struct EndpointStream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioClock> clock;
    WaveFormat wave;
    wasapi::SampleFormat format;
    Handle event = event_handle();
    UINT32 capacity = 0, period = 0;
    UINT64 frequency = 0;
    double mixRate = 0;
    REFERENCE_TIME latency = 0;
    bool lowLatency = false, started = false;
    HRESULT fallback = S_OK;
    ~EndpointStream() { if (started && client) (void)client->Stop(); }
    void activate() {
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(client.ReleaseAndGetAddressOf())), "Activate Windows audio client");
    }
    void open(const std::string& id, EDataFlow direction, int rate) {
        device = endpoint(id, direction); activate();
        WAVEFORMATEX* allocated = nullptr;
        check(client->GetMixFormat(&allocated), "Read Windows audio mix format"); wave.reset(allocated);
        format = sample_format(*wave); mixRate = wave->nSamplesPerSec;
        ComPtr<IAudioClient2> properties;
        if (SUCCEEDED(client.As(&properties))) {
            AudioClientProperties settings{}; settings.cbSize = sizeof(settings);
            settings.eCategory = AudioCategory_Media;
            check(properties->SetClientProperties(&settings), "Set music audio category");
        }
        ComPtr<IAudioClient3> low;
        HRESULT hr = client.As(&low);
        if (SUCCEEDED(hr) && rate == static_cast<int>(wave->nSamplesPerSec)) {
            UINT32 normal = 0, fundamental = 0, minimum = 0, maximum = 0;
            hr = low->GetSharedModeEnginePeriod(wave.get(), &normal, &fundamental, &minimum, &maximum);
            if (SUCCEEDED(hr) && fundamental != 0 && minimum > 0 && minimum <= maximum && maximum <= static_cast<UINT32>(rate * 2)) {
                // WASAPI is the simple shared-mode path: always request the
                // endpoint's minimum supported period, independently of the
                // engine processing block size or saved ASIO buffer settings.
                period = minimum;
                hr = low->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, wave.get(), nullptr);
                lowLatency = SUCCEEDED(hr);
            } else if (SUCCEEDED(hr)) hr = E_UNEXPECTED;
        } else if (SUCCEEDED(hr)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (!lowLatency) {
            fallback = hr;
            // A failed Initialize client is not reused. Windows' shared engine
            // performs nominal conversion when the jam differs from its mix rate.
            low.Reset(); properties.Reset(); client.Reset(); activate();
            wave->nSamplesPerSec = static_cast<DWORD>(rate);
            wave->nAvgBytesPerSec = wave->nSamplesPerSec * wave->nBlockAlign;
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, wave.get(), nullptr), "Initialize shared Windows audio");
            REFERENCE_TIME normal = 0, minimum = 0;
            check(client->GetDevicePeriod(&normal, &minimum), "Read Windows audio period");
            period = static_cast<UINT32>((normal * rate + 9999999) / 10000000);
        }
        check(client->GetBufferSize(&capacity), "Read Windows audio buffer size");
        if (capacity == 0 || capacity > static_cast<UINT32>(rate * 2) || period == 0)
            throw std::runtime_error("Windows audio reported an invalid buffer size");
        check(client->GetStreamLatency(&latency), "Read Windows audio stream latency");
        check(client->SetEventHandle(event.get()), "Set Windows audio event");
        check(client->GetService(IID_PPV_ARGS(&clock)), "Read Windows audio clock");
        check(clock->GetFrequency(&frequency), "Read Windows audio clock frequency");
        if (frequency == 0) throw std::runtime_error("Windows audio clock frequency is zero");
    }
};

class WasapiDeviceStream final : public DeviceStream {
public:
    WasapiDeviceStream(int id, int rate, long quantum, WasapiConfig config,
        MonoRingBuffer& capture, MonoRingBuffer& pitch, MonoRingBuffer& playback,
        std::size_t prefill, StreamControl& control, OutputRecorder* recorder, TrackTakeRecorder* take)
        : id_(id), rate_(rate), quantum_(quantum > 0 ? quantum : 128), config_(std::move(config)) {
        if (quantum_ > 8192) throw std::invalid_argument("WASAPI processing block exceeds 8192 frames");
        processing_.capture = &capture; processing_.pitch = &pitch; processing_.playback = &playback;
        processing_.control = &control; processing_.recorder = recorder; processing_.track_take_recorder = take;
        processing_.playback_prefill_frames = prefill;
    }
    ~WasapiDeviceStream() override {
        if (worker_.joinable()) { (void)SetEvent(stop_.get()); worker_.join(); }
    }
    void start() {
        std::promise<void> initialized; auto ready = initialized.get_future();
        worker_ = std::thread([this, initialized = std::move(initialized)]() mutable noexcept {
            bool published = false;
            try { run(initialized, published); }
            catch (const WasapiError& error) {
                error_.store(error.code, std::memory_order_relaxed);
                failed_.store(true, std::memory_order_release);
                if (!published) initialized.set_exception(std::current_exception());
            } catch (...) {
                error_.store(E_FAIL, std::memory_order_relaxed); failed_.store(true, std::memory_order_release);
                if (!published) initialized.set_exception(std::current_exception());
            }
        });
        ready.get();
    }
    long callbacks() const override { return processing_.callbacks.load(std::memory_order_relaxed); }
    bool playback_prefilled() const override { return processing_.playback_prefilled.load(std::memory_order_relaxed); }
    StreamInfo info() const override { return info_; }
    DeviceStreamStatus status() const noexcept override {
        DeviceStreamStatus result;
        result.failed = failed_.load(std::memory_order_acquire); result.error = error_.load();
        result.capture_discontinuities = discontinuities_.load(); result.invalid_timestamps = invalidTimestamps_.load();
        result.capture_underrun_frames = underruns_.load(); result.capture_overrun_frames = overruns_.load();
        result.capture_resets = resets_.load(); result.capture_queued_frames = queued_.load();
        result.render_padding_frames = padding_.load(); result.capture_correction_ppm = correction_.load();
        return result;
    }
    CallbackTimingStats callback_timing_stats() const override {
        const auto& s = intervals_;
        CallbackTimingStats result;
        result.interval_min_us = s.minimumUs.load(); result.interval_sum_us = s.sumUs.load();
        result.interval_max_us = s.maximumUs.load(); result.interval_samples = s.samples.load();
        result.gap_over_1_1x_count = s.gapsOver1_1x.load(); result.gap_over_1_5x_count = s.gapsOver1_5x.load();
        result.gap_over_2x_count = s.gapsOver2x.load(); result.work_min_us = s.workMinimumUs.load();
        result.work_sum_us = s.workSumUs.load(); result.work_max_us = s.workMaximumUs.load();
        result.work_samples = s.workSamples.load(); return result;
    }
private:
    void run(std::promise<void>& initialized, bool& published) {
        ComApartment apartment;
        EndpointStream output;
        output.open(config_.output_endpoint_id, eRender, rate_);
        std::unique_ptr<EndpointStream> input;
        ComPtr<IAudioCaptureClient> capture;
        if (!config_.input_endpoint_id.empty()) {
            input = std::make_unique<EndpointStream>();
            input->open(config_.input_endpoint_id, eCapture, rate_);
            check(input->client->GetService(IID_PPV_ARGS(&capture)), "Open Windows audio capture");
        }
        ComPtr<IAudioRenderClient> render;
        check(output.client->GetService(IID_PPV_ARGS(&render)), "Open Windows audio render");
        const auto channels = input ? input->format.channels : 0;
        wasapi::CaptureBuffer bridge;
        if (input) bridge.configure(channels, rate_, config_);
        std::vector<std::vector<std::int32_t>> planar(channels, std::vector<std::int32_t>(quantum_));
        std::vector<std::int32_t*> writable(channels);
        std::vector<const std::int32_t*> readable(channels);
        for (std::size_t i = 0; i < channels; ++i) writable[i] = planar[i].data(), readable[i] = planar[i].data();
        device_processing::prepare_duplex_processing(processing_, quantum_, channels, rate_);
        info_.device.id = id_; info_.device.backend = "WASAPI";
        info_.device.endpoint_id = config_.output_endpoint_id;
        info_.device.direction = EndpointDirection::Output;
        info_.device.name = endpoint_name(output.device.Get());
        info_.wasapi = config_; info_.input_device_name = input ? endpoint_name(input->device.Get()) : "None";
        info_.sample_rate = rate_; info_.buffer_size = output.period;
        info_.maximum_callback_frames = quantum_; info_.variable_callback_frames = true;
        info_.input_mix_sample_rate = input ? input->mixRate : 0; info_.output_mix_sample_rate = output.mixRate;
        info_.input_period_frames = input ? input->period : 0; info_.output_period_frames = output.period;
        info_.input_buffer_frames = input ? input->capacity : 0; info_.output_buffer_frames = output.capacity;
        info_.low_latency_shared = output.lowLatency && (!input || input->lowLatency);
        info_.shared_fallback_error = FAILED(output.fallback) ? output.fallback : (input ? input->fallback : 0);
        info_.channels.input.resize(channels); std::iota(info_.channels.input.begin(), info_.channels.input.end(), 0);
        info_.channels.output = output.format.channels == 1 ? std::vector<int>{0} : std::vector<int>{0, 1};
        info_.input_latency_frames = input ? static_cast<long>(input->latency * rate_ / 10000000) +
            static_cast<long>(bridge.target_frames()) : 0;
        info_.input_stream_latency_frames = input ? static_cast<long>(input->latency * rate_ / 10000000) : 0;
        info_.output_stream_latency_frames = static_cast<long>(output.latency * rate_ / 10000000);
        info_.output_latency_frames = std::max(info_.output_stream_latency_frames, static_cast<long>(output.capacity));
        info_.sample_format = "WASAPI shared; Windows mix-rate conversion; mono/front-stereo";
        auto& control = *processing_.control;
        control.input_latency_frames.store(static_cast<std::uint32_t>(info_.input_latency_frames));
        control.output_latency_frames.store(static_cast<std::uint32_t>(info_.output_latency_frames));
        control.recording_latency_compensation_frames.store(info_.input_latency_frames + info_.output_latency_frames);
        BYTE* initial = nullptr;
        check(render->GetBuffer(output.capacity, &initial), "Prime Windows audio output");
        check(render->ReleaseBuffer(output.capacity, AUDCLNT_BUFFERFLAGS_SILENT), "Release primed Windows audio output");
        struct Mmcss {
            DWORD index = 0; HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
            ~Mmcss() { if (task) AvRevertMmThreadCharacteristics(task); }
        } scheduling;
        info_.realtime_scheduling_active = scheduling.task != nullptr;
        info_.realtime_scheduling_error = scheduling.task ? 0 : static_cast<long>(GetLastError());
        if (input) { check(input->client->Start(), "Start Windows audio capture"); input->started = true; }
        check(output.client->Start(), "Start Windows audio output"); output.started = true;
        initialized.set_value(); published = true;
        HANDLE events[]{stop_.get(), output.event.get(), input ? input->event.get() : output.event.get()};
        while (true) {
            const auto waited = WaitForMultipleObjects(input ? 3 : 2, events, FALSE, 2000);
            if (waited == WAIT_OBJECT_0) return;
            if (waited == WAIT_FAILED) throw WasapiError("Wait for Windows audio", HRESULT_FROM_WIN32(GetLastError()));
            if (waited == WAIT_TIMEOUT) throw WasapiError("Windows audio stopped signalling", AUDCLNT_E_DEVICE_INVALIDATED);
            // This is native buffer servicing. Failures are published as fixed
            // codes; no exceptions, logging, allocation or locks in this pass.
            const auto hr = service(output, input.get(), capture.Get(), render.Get(), bridge, writable, readable);
            if (FAILED(hr)) { error_.store(hr); failed_.store(true, std::memory_order_release); return; }
        }
    }
    HRESULT service(EndpointStream& output, EndpointStream* input, IAudioCaptureClient* capture,
        IAudioRenderClient* render, wasapi::CaptureBuffer& bridge,
        std::span<std::int32_t* const> writable, std::span<const std::int32_t* const> readable) noexcept {
        HRESULT hr = S_OK;
        if (input) {
            // Drain the bounded endpoint buffer, including every available packet.
            UINT32 packets = 0;
            for (UINT32 serviced = 0; serviced <= input->capacity; ++serviced) {
                hr = capture->GetNextPacketSize(&packets); if (FAILED(hr)) return hr;
                if (packets == 0) break;
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 device = 0, time = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, &device, &time); if (FAILED(hr)) return hr;
                if (frames == 0) break;
                if (frames > input->capacity) {
                    (void)capture->ReleaseBuffer(frames); return E_UNEXPECTED;
                }
                if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) invalidTimestamps_.fetch_add(1);
                bridge.push(data ? std::span<const std::uint8_t>(data, frames * input->format.frame_bytes()) :
                    std::span<const std::uint8_t>{}, frames, input->format,
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0,
                    (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
                    (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 ? time / 10 : 0);
                hr = capture->ReleaseBuffer(frames); if (FAILED(hr)) return hr;
            }
        }
        UINT32 padding = 0; hr = output.client->GetCurrentPadding(&padding); if (FAILED(hr)) return hr;
        if (padding > output.capacity) return E_UNEXPECTED;
        padding_.store(padding);
        const auto available = output.capacity - padding;
        if (available == 0) return S_OK;
        BYTE* data = nullptr; hr = render->GetBuffer(available, &data); if (FAILED(hr)) return hr;
        const auto started = device_processing::callback_now_us();
        device_processing::observe_callback_interval(intervals_, started, output.period, rate_);
        UINT64 position = 0, qpc = 0;
        const auto clockHr = output.clock->GetPosition(&position, &qpc);
        if (FAILED(clockHr)) {
            (void)render->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
            return clockHr;
        }
        DeviceClockSample clock{};
        if (clockHr == S_OK) {
            // QPC timestamps are already in 100ns units, not raw QPC ticks.
            clock.frame = position / output.frequency * rate_ +
                position % output.frequency * rate_ / output.frequency;
            clock.time_us = qpc / 10;
        } else { invalidTimestamps_.fetch_add(1); }
        for (std::size_t offset = 0; offset < available;) {
            const auto frames = std::min<std::size_t>(quantum_, available - offset);
            if (input) bridge.pull(writable, frames);
            auto& control = *processing_.control;
            const auto sourceTime = bridge.source_time_us();
            const auto now = device_processing::callback_now_us();
            const auto inputDelay = input && sourceTime != 0 && now >= sourceTime && now - sourceTime <= 2000000
                ? static_cast<std::uint32_t>((now - sourceTime) * rate_ / 1000000ULL)
                : static_cast<std::uint32_t>(info_.input_latency_frames);
            // Keep Windows' reported maximum separate. Queue padding and packet
            // timestamps give observable per-block estimates without summing
            // overlapping latency figures or treating a reported zero as zero
            // queued output. Calibration covers unreported physical latency.
            const auto outputDelay = static_cast<std::uint32_t>(std::max<std::size_t>(
                static_cast<std::size_t>(info_.output_stream_latency_frames), padding + offset));
            control.input_latency_frames.store(inputDelay, std::memory_order_relaxed);
            control.output_latency_frames.store(outputDelay, std::memory_order_relaxed);
            control.recording_latency_compensation_frames.store(inputDelay + outputDelay, std::memory_order_relaxed);
            device_processing::process_duplex_block(processing_, readable, frames, true, clock, false);
            wasapi::encode_output(std::span<const std::int32_t>(processing_.playback_scratch.data(), frames),
                output.format, std::span<std::uint8_t>(data + offset * output.format.frame_bytes(),
                    frames * output.format.frame_bytes()));
            offset += frames;
        }
        hr = render->ReleaseBuffer(available, 0);
        device_processing::observe_callback_work(intervals_, started, device_processing::callback_now_us());
        const auto status = bridge.status();
        discontinuities_.store(status.capture_discontinuities); underruns_.store(status.capture_underrun_frames);
        overruns_.store(status.capture_overrun_frames); resets_.store(status.capture_resets);
        queued_.store(status.capture_queued_frames); correction_.store(status.capture_correction_ppm);
        return hr;
    }
    int id_, rate_; long quantum_; WasapiConfig config_;
    Handle stop_ = event_handle();
    device_processing::DuplexProcessingState processing_;
    device_processing::CallbackIntervalState intervals_;
    StreamInfo info_;
    std::thread worker_;
    std::atomic<bool> failed_{false}; std::atomic<long> error_{0};
    std::atomic<std::uint64_t> discontinuities_{0}, invalidTimestamps_{0}, underruns_{0}, overruns_{0}, resets_{0};
    std::atomic<std::size_t> queued_{0}, padding_{0}; std::atomic<int> correction_{0};
};
} // namespace

void append_wasapi_devices(std::vector<DeviceInfo>& devices) {
    ComApartment apartment; auto source = enumerator();
    for (const auto flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> endpoints;
        check(source->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &endpoints), "List Windows audio endpoints");
        UINT count = 0; check(endpoints->GetCount(&count), "Count Windows audio endpoints");
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device; check(endpoints->Item(i, &device), "Read Windows audio endpoint");
            LPWSTR id = nullptr; check(device->GetId(&id), "Read Windows audio endpoint ID");
            DeviceInfo entry;
            try { entry.endpoint_id = utf8(id); } catch (...) { CoTaskMemFree(id); throw; }
            CoTaskMemFree(id);
            entry.id = static_cast<int>(devices.size()); entry.backend = "WASAPI";
            entry.name = endpoint_name(device.Get());
            entry.direction = flow == eRender ? EndpointDirection::Output : EndpointDirection::Input;
            ComPtr<IAudioClient> client;
            check(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())), "Inspect Windows audio endpoint");
            WAVEFORMATEX* raw = nullptr; check(client->GetMixFormat(&raw), "Read endpoint channels");
            WaveFormat wave(raw); entry.channel_count = wave->nChannels;
            ComPtr<IAudioClient3> low;
            UINT32 normal = 0, fundamental = 0, minimum = 0, maximum = 0;
            if (SUCCEEDED(client.As(&low)) && SUCCEEDED(low->GetSharedModeEnginePeriod(wave.get(),
                    &normal, &fundamental, &minimum, &maximum))) {
                entry.minimum_period_frames = minimum; entry.default_period_frames = normal;
            }
            devices.push_back(std::move(entry));
        }
    }
}

DeviceTestResult test_wasapi_device(const DeviceInfo& device) {
    ComApartment apartment;
    auto selected = endpoint(device.endpoint_id, device.direction == EndpointDirection::Input ? eCapture : eRender);
    ComPtr<IAudioClient> client;
    check(selected->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf())), "Inspect Windows audio capabilities");
    WAVEFORMATEX* raw = nullptr; check(client->GetMixFormat(&raw), "Read Windows audio mix format");
    WaveFormat wave(raw); (void)sample_format(*wave);
    DeviceTestResult result; result.device = device;
    result.current_sample_rate = result.mix_sample_rate = wave->nSamplesPerSec;
    result.sample_rate_supported.fill(true); // Shared-mode Windows SRC supports the jam rates.
    ComPtr<IAudioClient3> low;
    UINT32 normal = 0, fundamental = 0, minimum = 0, maximum = 0;
    if (SUCCEEDED(client.As(&low)) && SUCCEEDED(low->GetSharedModeEnginePeriod(wave.get(),
            &normal, &fundamental, &minimum, &maximum)) && fundamental != 0) {
        result.minimum_period_frames = minimum; result.default_period_frames = normal;
        result.shared_period_frames.push_back(minimum);
        if (normal != minimum) result.shared_period_frames.push_back(normal);
        for (std::size_t i = 0; i < kTestBufferSizes.size(); ++i) {
            const auto size = kTestBufferSizes[i];
            result.buffer_size_supported[i] = size >= static_cast<long>(minimum) && size <= static_cast<long>(maximum) && size % fundamental == 0;
        }
    }
    return result;
}

std::unique_ptr<DeviceStream> start_wasapi_stream(int id, double rate, long period,
    const WasapiConfig& config, MonoRingBuffer& capture, MonoRingBuffer& pitch,
    MonoRingBuffer& playback, std::size_t prefill, StreamControl& control,
    OutputRecorder* recorder, TrackTakeRecorder* takeRecorder) {
    if (config.output_endpoint_id.empty()) throw std::invalid_argument("select a WASAPI output device");
    auto result = std::make_unique<WasapiDeviceStream>(id, static_cast<int>(rate), period, config,
        capture, pitch, playback, prefill, control, recorder, takeRecorder);
    result->start(); return result;
}
} // namespace jam2::audio
