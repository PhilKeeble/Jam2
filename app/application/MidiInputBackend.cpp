#include "MidiInputBackend.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace jam2::application {
namespace {

class SystemMidiInputBackend final : public MidiInputBackend {
public:
    std::vector<midi::DeviceInfo> enumerate() override
    {
        return midi::enumerate_input_devices();
    }

    std::unique_ptr<midi::InputDevice> open(
        const std::string& id,
        midi::EventQueue& destination,
        std::string& error) noexcept override
    {
        return midi::open_input_device(id, destination, error);
    }

    bool inject(
        const std::string&,
        const midi::Event&,
        std::string& error) noexcept override
    {
        error = "the system MIDI backend does not accept injected events";
        return false;
    }
};

struct SyntheticEndpoint {
    explicit SyntheticEndpoint(midi::DeviceInfo value)
        : info(std::move(value))
    {
    }

    midi::DeviceInfo info;
    std::mutex mutex;
    midi::EventQueue* destination = nullptr;
    std::atomic<std::uint64_t> short_messages{0};
    std::atomic<std::uint64_t> unsupported_messages{0};
};

class SyntheticMidiInputDevice final : public midi::InputDevice {
public:
    SyntheticMidiInputDevice(
        std::shared_ptr<SyntheticEndpoint> endpoint,
        midi::EventQueue& destination,
        std::string& error) noexcept
        : endpoint_(std::move(endpoint))
    {
        std::lock_guard<std::mutex> lock(endpoint_->mutex);
        if (endpoint_->destination != nullptr) {
            error = "synthetic MIDI input is already open";
            return;
        }
        endpoint_->destination = &destination;
        opened_ = true;
    }

    ~SyntheticMidiInputDevice() override
    {
        if (!opened_) return;
        std::lock_guard<std::mutex> lock(endpoint_->mutex);
        endpoint_->destination = nullptr;
    }

    bool opened() const noexcept { return opened_; }
    const midi::DeviceInfo& info() const noexcept override { return endpoint_->info; }
    std::uint64_t short_messages() const noexcept override
    {
        return endpoint_->short_messages.load(std::memory_order_relaxed);
    }
    std::uint64_t unsupported_messages() const noexcept override
    {
        return endpoint_->unsupported_messages.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<SyntheticEndpoint> endpoint_;
    bool opened_ = false;
};

class SyntheticMidiInputBackend final : public MidiInputBackend {
public:
    SyntheticMidiInputBackend(
        std::vector<midi::DeviceInfo> devices,
        std::chrono::milliseconds enumerationDelay)
        : enumerationDelay_(enumerationDelay)
    {
        endpoints_.reserve(devices.size());
        for (midi::DeviceInfo& device : devices) {
            if (device.id.empty() || device.name.empty()) continue;
            const auto duplicate = std::find_if(
                endpoints_.cbegin(), endpoints_.cend(),
                [&device](const auto& endpoint) {
                    return endpoint->info.id == device.id;
                });
            if (duplicate == endpoints_.cend()) {
                endpoints_.push_back(
                    std::make_shared<SyntheticEndpoint>(std::move(device)));
            }
        }
    }

    std::vector<midi::DeviceInfo> enumerate() override
    {
        if (enumerationDelay_.count() > 0)
            std::this_thread::sleep_for(enumerationDelay_);
        std::vector<midi::DeviceInfo> result;
        result.reserve(endpoints_.size());
        for (const auto& endpoint : endpoints_) result.push_back(endpoint->info);
        return result;
    }

    std::unique_ptr<midi::InputDevice> open(
        const std::string& id,
        midi::EventQueue& destination,
        std::string& error) noexcept override
    {
        try {
            const auto endpoint = find(id);
            if (!endpoint) {
                error = "synthetic MIDI input id is unknown";
                return {};
            }
            auto device = std::make_unique<SyntheticMidiInputDevice>(
                endpoint, destination, error);
            if (!device->opened()) return {};
            return device;
        } catch (const std::exception& exception) {
            error = std::string("synthetic MIDI input open failed: ") + exception.what();
        } catch (...) {
            error = "synthetic MIDI input open failed";
        }
        return {};
    }

    bool inject(
        const std::string& id,
        const midi::Event& event,
        std::string& error) noexcept override
    {
        const auto endpoint = find(id);
        if (!endpoint) {
            error = "synthetic MIDI input id is unknown";
            return false;
        }
        const std::uint8_t kind = event.status & 0xf0U;
        const bool valid = event.status >= 0x80U && event.status < 0xf0U &&
            event.data1 <= 0x7fU && event.data2 <= 0x7fU &&
            event.size == ((kind == 0xc0U || kind == 0xd0U) ? 2U : 3U);
        if (!valid) {
            endpoint->unsupported_messages.fetch_add(1, std::memory_order_relaxed);
            error = "synthetic MIDI event is not a complete channel-voice message";
            return false;
        }
        std::lock_guard<std::mutex> lock(endpoint->mutex);
        if (endpoint->destination == nullptr) {
            error = "synthetic MIDI input is not open";
            return false;
        }
        endpoint->short_messages.fetch_add(1, std::memory_order_relaxed);
        if (!endpoint->destination->push(event)) {
            error = "synthetic MIDI input queue is full";
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<SyntheticEndpoint> find(const std::string& id) const noexcept
    {
        const auto found = std::find_if(
            endpoints_.cbegin(), endpoints_.cend(),
            [&id](const auto& endpoint) { return endpoint->info.id == id; });
        return found == endpoints_.cend() ? nullptr : *found;
    }

    std::vector<std::shared_ptr<SyntheticEndpoint>> endpoints_;
    std::chrono::milliseconds enumerationDelay_{};
};

} // namespace

std::unique_ptr<MidiInputBackend> makeSystemMidiInputBackend()
{
    return std::make_unique<SystemMidiInputBackend>();
}

std::unique_ptr<MidiInputBackend> makeSyntheticMidiInputBackend(
    std::vector<midi::DeviceInfo> devices,
    std::chrono::milliseconds enumerationDelay)
{
    return std::make_unique<SyntheticMidiInputBackend>(
        std::move(devices), enumerationDelay);
}

} // namespace jam2::application
