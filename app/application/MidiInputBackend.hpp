#pragma once

#include "midi.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace jam2::application {

// Owns the platform seam used by the GUI's asynchronous MIDI discovery/open
// workflow. Product startup uses the system backend; the private GUI agent can
// inject a deterministic backend without changing platform enumeration.
class MidiInputBackend {
public:
    virtual ~MidiInputBackend() = default;

    virtual std::vector<midi::DeviceInfo> enumerate() = 0;
    virtual std::unique_ptr<midi::InputDevice> open(
        const std::string& id,
        midi::EventQueue& destination,
        std::string& error) noexcept = 0;

    // System devices are producer-driven and reject local injection. A
    // synthetic backend accepts bounded channel-voice events for automation.
    virtual bool inject(
        const std::string& id,
        const midi::Event& event,
        std::string& error) noexcept = 0;
};

std::unique_ptr<MidiInputBackend> makeSystemMidiInputBackend();
std::unique_ptr<MidiInputBackend> makeSyntheticMidiInputBackend(
    std::vector<midi::DeviceInfo> devices,
    std::chrono::milliseconds enumerationDelay = {});

} // namespace jam2::application
