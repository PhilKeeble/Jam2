#include "MidiInputBackend.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main()
{
    try {
        auto backend = jam2::application::makeSyntheticMidiInputBackend({
            {"fake-a", "Fake MIDI A"},
            {"fake-b", "Fake MIDI B"},
            {"fake-a", "Duplicate ignored"},
            {"", "Invalid ignored"},
        });
        const auto devices = backend->enumerate();
        require(devices.size() == 2, "synthetic MIDI inventory was not bounded/deduplicated");
        require(devices[0].id == "fake-a" && devices[0].name == "Fake MIDI A",
            "synthetic MIDI inventory changed first device");
        require(devices[1].id == "fake-b" && devices[1].name == "Fake MIDI B",
            "synthetic MIDI inventory changed second device");

        std::string error;
        require(backend->armAutomationCompletionGate(error) &&
                backend->automationCompletionGateState() ==
                    jam2::application::AutomationCompletionGateState::Armed,
            "synthetic MIDI enumeration gate did not expose its armed state");
        require(backend->releaseAutomationCompletionGate(error) &&
                backend->automationCompletionGateState() ==
                    jam2::application::AutomationCompletionGateState::Idle,
            "an unstarted MIDI enumeration gate could not be safely disarmed");

        jam2::midi::EventQueue queue;
        const jam2::midi::Event noteOn{1000, 0, 0x90, 60, 100, 3};
        require(!backend->inject("fake-a", noteOn, error),
            "closed synthetic MIDI input accepted an event");
        require(!error.empty(), "closed synthetic MIDI input did not explain rejection");

        error.clear();
        auto missing = backend->open("unknown", queue, error);
        require(!missing && !error.empty(), "unknown synthetic MIDI id opened");

        error.clear();
        auto device = backend->open("fake-a", queue, error);
        require(device != nullptr && error.empty(), "synthetic MIDI input did not open");
        require(device->info().id == "fake-a", "opened synthetic MIDI device identity changed");

        jam2::midi::EventQueue secondQueue;
        auto duplicateOpen = backend->open("fake-a", secondQueue, error);
        require(!duplicateOpen && !error.empty(), "synthetic MIDI input opened twice");

        error.clear();
        require(backend->inject("fake-a", noteOn, error) && error.empty(),
            "valid synthetic MIDI event was rejected");
        jam2::midi::Event delivered;
        require(queue.pop(delivered), "injected MIDI event did not reach its queue");
        require(delivered.status == noteOn.status && delivered.data1 == noteOn.data1 &&
                delivered.data2 == noteOn.data2 && delivered.size == noteOn.size,
            "injected MIDI event payload changed");
        require(device->short_messages() == 1 && device->unsupported_messages() == 0,
            "synthetic MIDI counters did not reflect the valid event");

        jam2::midi::Event invalid = noteOn;
        invalid.status = 0xf0;
        error.clear();
        require(!backend->inject("fake-a", invalid, error) && !error.empty(),
            "unsupported synthetic MIDI event was accepted");
        require(device->unsupported_messages() == 1,
            "synthetic MIDI unsupported counter did not advance");

        device.reset();
        error.clear();
        auto reopened = backend->open("fake-a", secondQueue, error);
        require(reopened != nullptr && error.empty(),
            "synthetic MIDI input did not release ownership on close");

        for (std::size_t index = 0;
             index + 1 < jam2::midi::kEventQueueCapacity;
             ++index) {
            error.clear();
            require(backend->inject("fake-a", noteOn, error) && error.empty(),
                "synthetic MIDI queue filled before its bounded capacity");
        }
        require(secondQueue.depth() + 1 == jam2::midi::kEventQueueCapacity,
            "synthetic MIDI queue did not reach its bounded capacity");
        error.clear();
        require(!backend->inject("fake-a", noteOn, error) && !error.empty(),
            "synthetic MIDI queue overflow falsely reported delivery");
        require(secondQueue.dropped() == 1,
            "synthetic MIDI queue overflow did not retain its drop diagnostic");

        // Exercise the production adapter without requiring a MIDI controller.
        // Enumeration is read-only and an impossible identifier must be rejected
        // before WinMM attempts to open any physical endpoint.
        auto systemBackend = jam2::application::makeSystemMidiInputBackend();
        require(systemBackend->automationCompletionGateState() ==
                jam2::application::AutomationCompletionGateState::Unsupported,
            "system MIDI backend unexpectedly exposed a private completion gate");
        error.clear();
        require(!systemBackend->armAutomationCompletionGate(error) && !error.empty(),
            "system MIDI backend accepted an automation completion gate");
        error.clear();
        require(!systemBackend->releaseAutomationCompletionGate(error) && !error.empty(),
            "system MIDI backend accepted completion-gate release");
        (void)systemBackend->enumerate();
        error.clear();
        auto invalidSystemDevice = systemBackend->open(
            "jam2-invalid-midi-device", queue, error);
        require(!invalidSystemDevice && !error.empty(),
            "system MIDI backend accepted an invalid device id");
        error.clear();
        require(!systemBackend->inject("0", noteOn, error) && !error.empty(),
            "system MIDI backend accepted synthetic event injection");

        std::cout << "Jam2 MIDI input backend tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MIDI input backend test failure: " << error.what() << '\n';
        return 1;
    }
}
