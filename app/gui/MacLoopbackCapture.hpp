#pragma once

#include "GuiLoopbackRecorder.hpp"

#include <CoreAudio/CoreAudio.h>

namespace jam2::gui {

struct MacLoopbackOutput {
    AudioDeviceID device = kAudioObjectUnknown;
    QString uid;
    QString name;
};

// Narrow HAL boundary: the recorder, callback, queue, and finalization are shared
// by native capture and deterministic tests. Only OS calls are replaceable.
class MacLoopbackApi {
public:
    virtual ~MacLoopbackApi() = default;
    virtual std::vector<MacLoopbackOutput> outputs() = 0;
    virtual AudioDeviceID defaultOutput() = 0;
    virtual AudioObjectID createTap(const MacLoopbackOutput& output) = 0;
    virtual AudioStreamBasicDescription tapFormat(AudioObjectID tap) = 0;
    virtual AudioDeviceID createAggregate(AudioObjectID tap) = 0;
    virtual AudioDeviceIOProcID createIoProc(
        AudioDeviceID device, AudioDeviceIOProc callback, void* context) = 0;
    virtual void start(AudioDeviceID device, AudioDeviceIOProcID proc) = 0;
    virtual bool outputAlive(AudioDeviceID device) = 0;
    virtual OSStatus stop(AudioDeviceID device, AudioDeviceIOProcID proc) noexcept = 0;
    virtual OSStatus destroyIoProc(AudioDeviceID device, AudioDeviceIOProcID proc) noexcept = 0;
    virtual OSStatus destroyAggregate(AudioDeviceID device) noexcept = 0;
    virtual OSStatus destroyTap(AudioObjectID tap) noexcept = 0;
};

QStringList mac_loopback_sources(QString* error);
QStringList mac_loopback_sources(MacLoopbackApi& api);
GuiLoopbackCaptureResult capture_mac_loopback(
    const GuiLoopbackOptions& options, const std::atomic<bool>& stopRequested);
GuiLoopbackCaptureResult capture_mac_loopback(
    const GuiLoopbackOptions& options, const std::atomic<bool>& stopRequested,
    MacLoopbackApi& api);

} // namespace jam2::gui
