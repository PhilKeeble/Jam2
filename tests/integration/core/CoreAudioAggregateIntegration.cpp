#include "audio_device.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace jam2::audio;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::size_t privateAggregates() {
    AudioObjectPropertyAddress property{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    require(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, nullptr, &size) == noErr, "enumerate HAL devices");
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    require(AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &size, ids.data()) == noErr, "read HAL devices");
    std::size_t count = 0;
    for (auto id : ids) {
        AudioObjectPropertyAddress uidProperty{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef uid = nullptr; UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(id, &uidProperty, 0, nullptr, &uidSize, &uid) == noErr && uid) {
            if (CFStringHasPrefix(uid, CFSTR("org.jam2.private."))) {
                ++count;
                AudioObjectPropertyAddress compositionProperty{kAudioAggregateDevicePropertyComposition, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
                CFDictionaryRef composition = nullptr; UInt32 compositionSize = sizeof(composition);
                const auto status = AudioObjectGetPropertyData(id, &compositionProperty, 0, nullptr, &compositionSize, &composition);
                bool isPrivate = false;
                if (status == noErr && composition) {
                    auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(composition, CFSTR(kAudioAggregateDeviceIsPrivateKey)));
                    int enabled = 0;
                    isPrivate = value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberIntType, &enabled) && enabled == 1;
                    CFRelease(composition);
                }
                CFRelease(uid);
                require(isPrivate, "Jam2 aggregate must be process private");
            } else CFRelease(uid);
        }
    }
    return count;
}
void released(std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (privateAggregates() != count && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(privateAggregates() == count, "aggregate must be destroyed on stop or startup failure");
}
}
int main() {
    const auto* outputUid = std::getenv("JAM2_COREAUDIO_OUTPUT_UID");
    const auto* inputUid = std::getenv("JAM2_COREAUDIO_INPUT_UID");
    if (!outputUid || !inputUid) {
        std::cout << "Set JAM2_COREAUDIO_OUTPUT_UID and JAM2_COREAUDIO_INPUT_UID for explicit hardware validation.\n";
        return 77;
    }
    try {
        const auto devices = list_devices();
        const auto output = std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return stable_device_id(device) == outputUid; });
        require(output != devices.end(), "profiled output must be available");
        require(output->output_channel_count > 0, "profiled device must support output");
        CoreAudioConfig config; config.setup = AudioDeviceSetup::SeparateInputOutput;
        config.output_device_uid = outputUid; config.input_device_uid = inputUid;
        ChannelSelection channels{{0}, output->output_channel_count == 1 ? std::vector<int>{0} : std::vector<int>{0, 1}};
        const auto initialCount = privateAggregates();
        const bool split = config.input_device_uid != config.output_device_uid;
        for (bool input : {true, false}) {
            MonoRingBuffer capture(8192), pitch(8192), playback(8192); StreamControl control;
            if (!input) config.input_device_uid.clear();
            auto stream = start_duplex_stream(output->id, 48000, 0, InputChannels::Mono, channels,
                capture, pitch, playback, 0, control, nullptr, nullptr, {}, config);
            const auto info = stream->info();
            require(info.private_aggregate == (input && split) && info.input_drift_compensation == info.private_aggregate,
                "two different devices require an aggregate and input drift correction");
            require(info.channels.input == (input ? channels.input : std::vector<int>{}) && info.channels.output == channels.output,
                "public channel selection must stay relative to each physical device");
            require(stable_device_id(info.device) == outputUid, "aggregate must retain the physical output identity");
            require(privateAggregates() == initialCount + (info.private_aggregate ? 1 : 0), "only one private aggregate may be owned");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (stream->callbacks() < 10 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(stream->callbacks() >= 10, "CoreAudio callbacks must advance");
            stream.reset(); released(initialCount);
        }
        config.input_device_uid = inputUid;
        MonoRingBuffer capture(8192), pitch(8192), playback(8192); StreamControl control;
        bool failed = false;
        try {
            auto stream = start_duplex_stream(output->id, 48000, 0, InputChannels::Mono, channels,
                capture, pitch, playback, playback.capacity() + 1, control, nullptr, nullptr, {}, config);
        } catch (const std::exception&) { failed = true; }
        require(failed, "invalid prefill must fail after aggregate creation"); released(initialCount);
        std::cout << "CoreAudio private aggregate, playback-only callbacks and rollback cleanup passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
