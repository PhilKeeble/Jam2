#include "midi.hpp"

#include "common.hpp"

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <charconv>
#include <memory>
#include <string>
#include <vector>

namespace jam2::midi {
namespace {

std::string cf_string(CFStringRef value)
{
    if (value == nullptr) return {};
    char buffer[512]{};
    if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return buffer;
    }
    return {};
}

std::string endpoint_name(MIDIEndpointRef endpoint)
{
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &value) != noErr) {
        return "MIDI input";
    }
    std::string result = cf_string(value);
    if (value != nullptr) CFRelease(value);
    return result.empty() ? "MIDI input" : result;
}

bool parse_id(const std::string& text, ItemCount& value) noexcept
{
    unsigned long parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed >= MIDIGetNumberOfSources()) {
        return false;
    }
    value = static_cast<ItemCount>(parsed);
    return true;
}

class MacMidiInput final : public InputDevice {
public:
    MacMidiInput(ItemCount index, EventQueue& destination, std::string& error)
        : destination_(destination)
    {
        endpoint_ = MIDIGetSource(index);
        if (endpoint_ == 0) {
            error = "MIDI input is no longer available";
            return;
        }
        info_.id = std::to_string(index);
        info_.name = endpoint_name(endpoint_);
        OSStatus status = MIDIClientCreate(
            CFSTR("Jam2 MIDI Input"), nullptr, nullptr, &client_);
        if (status == noErr) {
            status = MIDIInputPortCreate(
                client_, CFSTR("Jam2 MIDI Capture"), &MacMidiInput::read, this, &port_);
        }
        if (status == noErr) {
            status = MIDIPortConnectSource(port_, endpoint_, nullptr);
        }
        if (status != noErr) {
            error = "Cannot open MIDI input '" + info_.name + "' (CoreMIDI " +
                std::to_string(status) + ")";
            close();
        }
    }

    ~MacMidiInput() override { close(); }
    MacMidiInput(const MacMidiInput&) = delete;
    MacMidiInput& operator=(const MacMidiInput&) = delete;

    bool valid() const noexcept { return client_ != 0 && port_ != 0; }
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
    static void read(
        const MIDIPacketList* packets,
        void* context,
        void*)
    {
        auto* self = static_cast<MacMidiInput*>(context);
        if (self == nullptr || packets == nullptr) return;
        const MIDIPacket* packet = &packets->packet[0];
        for (UInt32 packet_index = 0; packet_index < packets->numPackets; ++packet_index) {
            std::size_t offset = 0;
            std::uint8_t running_status = 0;
            while (offset < packet->length) {
                std::uint8_t status = packet->data[offset];
                if ((status & 0x80U) != 0U) {
                    ++offset;
                    if (status >= 0xf0U) {
                        running_status = 0;
                        ++self->unsupported_messages_;
                        while (offset < packet->length && (packet->data[offset] & 0x80U) == 0U) ++offset;
                        continue;
                    }
                    running_status = status;
                } else if (running_status != 0U) {
                    status = running_status;
                } else {
                    ++offset;
                    ++self->unsupported_messages_;
                    continue;
                }
                const std::uint8_t kind = status & 0xf0U;
                const std::size_t data_size = (kind == 0xc0U || kind == 0xd0U) ? 1U : 2U;
                if (offset + data_size > packet->length) {
                    ++self->unsupported_messages_;
                    break;
                }
                Event event;
                event.monotonic_us = jam2::monotonic_us();
                event.status = status;
                event.data1 = packet->data[offset] & 0x7fU;
                event.data2 = data_size == 2U ? packet->data[offset + 1U] & 0x7fU : 0U;
                event.size = static_cast<std::uint8_t>(data_size + 1U);
                self->destination_.push(event);
                self->short_messages_.fetch_add(1, std::memory_order_relaxed);
                offset += data_size;
            }
            packet = MIDIPacketNext(packet);
        }
    }

    void close() noexcept
    {
        if (port_ != 0 && endpoint_ != 0) MIDIPortDisconnectSource(port_, endpoint_);
        if (port_ != 0) MIDIPortDispose(port_);
        if (client_ != 0) MIDIClientDispose(client_);
        port_ = 0;
        client_ = 0;
        endpoint_ = 0;
    }

    EventQueue& destination_;
    DeviceInfo info_;
    MIDIClientRef client_ = 0;
    MIDIPortRef port_ = 0;
    MIDIEndpointRef endpoint_ = 0;
    std::atomic<std::uint64_t> short_messages_{0};
    std::atomic<std::uint64_t> unsupported_messages_{0};
};

} // namespace

std::vector<DeviceInfo> enumerate_input_devices()
{
    std::vector<DeviceInfo> result;
    const ItemCount count = MIDIGetNumberOfSources();
    result.reserve(count);
    for (ItemCount index = 0; index < count; ++index) {
        const MIDIEndpointRef endpoint = MIDIGetSource(index);
        if (endpoint != 0) result.push_back({std::to_string(index), endpoint_name(endpoint)});
    }
    return result;
}

std::unique_ptr<InputDevice> open_input_device(
    const std::string& id,
    EventQueue& destination,
    std::string& error) noexcept
{
    try {
        ItemCount index = 0;
        if (!parse_id(id, index)) {
            error = "MIDI input id is invalid or no longer available";
            return {};
        }
        auto device = std::make_unique<MacMidiInput>(index, destination, error);
        if (!device->valid()) return {};
        return device;
    } catch (const std::exception& exception) {
        error = std::string("MIDI input open failed: ") + exception.what();
    } catch (...) {
        error = "MIDI input open failed";
    }
    return {};
}

} // namespace jam2::midi
