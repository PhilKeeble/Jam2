#include "midi.hpp"

#include "common.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <charconv>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace jam2::midi {
namespace {

std::string winmm_error(MMRESULT code)
{
    char text[MAXERRORLENGTH]{};
    if (midiInGetErrorTextA(code, text, static_cast<UINT>(sizeof(text))) == MMSYSERR_NOERROR) {
        return text;
    }
    return "WinMM error " + std::to_string(code);
}

bool parse_id(const std::string& text, UINT& value) noexcept
{
    unsigned long parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed >= midiInGetNumDevs()) {
        return false;
    }
    value = static_cast<UINT>(parsed);
    return true;
}

class WinMidiInput final : public InputDevice {
public:
    WinMidiInput(UINT id, EventQueue& destination, std::string& error)
        : destination_(destination)
    {
        MIDIINCAPSA capabilities{};
        MMRESULT result = midiInGetDevCapsA(id, &capabilities, sizeof(capabilities));
        if (result != MMSYSERR_NOERROR) {
            error = "Cannot inspect MIDI input: " + winmm_error(result);
            return;
        }
        info_.id = std::to_string(id);
        info_.name = capabilities.szPname;
        result = midiInOpen(
            &handle_,
            id,
            reinterpret_cast<DWORD_PTR>(&WinMidiInput::callback),
            reinterpret_cast<DWORD_PTR>(this),
            CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            handle_ = nullptr;
            error = "Cannot open MIDI input '" + info_.name + "': " + winmm_error(result);
            return;
        }
        result = midiInStart(handle_);
        if (result != MMSYSERR_NOERROR) {
            error = "Cannot start MIDI input '" + info_.name + "': " + winmm_error(result);
            midiInClose(handle_);
            handle_ = nullptr;
        }
    }

    ~WinMidiInput() override
    {
        if (handle_ != nullptr) {
            midiInStop(handle_);
            midiInReset(handle_);
            midiInClose(handle_);
        }
    }

    WinMidiInput(const WinMidiInput&) = delete;
    WinMidiInput& operator=(const WinMidiInput&) = delete;

    bool valid() const noexcept { return handle_ != nullptr; }
    const DeviceInfo& info() const noexcept override { return info_; }
    std::uint64_t short_messages() const noexcept override
    {
        return short_messages_.load(std::memory_order_relaxed);
    }
    std::uint64_t unsupported_messages() const noexcept override
    {
        return unsupported_messages_.load(std::memory_order_relaxed);
    }

private:
    static void CALLBACK callback(
        HMIDIIN,
        UINT message,
        DWORD_PTR instance,
        DWORD_PTR parameter1,
        DWORD_PTR)
    {
        auto* self = reinterpret_cast<WinMidiInput*>(instance);
        if (self == nullptr) {
            return;
        }
        if (message != MIM_DATA && message != MIM_MOREDATA) {
            if (message == MIM_LONGDATA || message == MIM_LONGERROR || message == MIM_ERROR) {
                self->unsupported_messages_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        const auto packed = static_cast<std::uint32_t>(parameter1);
        const std::uint8_t status_byte = static_cast<std::uint8_t>(packed & 0xffU);
        if (status_byte < 0x80U || status_byte >= 0xf0U) {
            self->unsupported_messages_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint8_t kind = status_byte & 0xf0U;
        Event event;
        event.monotonic_us = jam2::monotonic_us();
        event.status = status_byte;
        event.data1 = static_cast<std::uint8_t>((packed >> 8U) & 0x7fU);
        event.data2 = static_cast<std::uint8_t>((packed >> 16U) & 0x7fU);
        event.size = (kind == 0xc0U || kind == 0xd0U) ? 2U : 3U;
        self->destination_.push(event);
        self->short_messages_.fetch_add(1, std::memory_order_relaxed);
    }

    EventQueue& destination_;
    DeviceInfo info_;
    HMIDIIN handle_ = nullptr;
    std::atomic<std::uint64_t> short_messages_{0};
    std::atomic<std::uint64_t> unsupported_messages_{0};
};

} // namespace

std::vector<DeviceInfo> enumerate_input_devices()
{
    std::vector<DeviceInfo> result;
    const UINT count = midiInGetNumDevs();
    result.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        MIDIINCAPSA capabilities{};
        if (midiInGetDevCapsA(index, &capabilities, sizeof(capabilities)) == MMSYSERR_NOERROR) {
            result.push_back({std::to_string(index), capabilities.szPname});
        }
    }
    return result;
}

std::unique_ptr<InputDevice> open_input_device(
    const std::string& id,
    EventQueue& destination,
    std::string& error) noexcept
{
    try {
        UINT numeric_id = 0;
        if (!parse_id(id, numeric_id)) {
            error = "MIDI input id is invalid or no longer available";
            return {};
        }
        auto device = std::make_unique<WinMidiInput>(numeric_id, destination, error);
        if (!device->valid()) {
            return {};
        }
        return device;
    } catch (const std::exception& exception) {
        error = std::string("MIDI input open failed: ") + exception.what();
    } catch (...) {
        error = "MIDI input open failed";
    }
    return {};
}

} // namespace jam2::midi
