#include "Vst3Host.hpp"

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/midiconvert.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/funknownimpl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#endif

namespace Steinberg {
FUnknown* gStandardPluginContext = new Vst::HostApplication();
}

namespace jam2::pluginhost {
namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

VST3::Hosting::ClassInfo find_class(
    const VST3::Hosting::PluginFactory& factory,
    const std::string& requested)
{
    for (const auto& info : factory.classInfos()) {
        if (info.category() != kVstAudioEffectClass) continue;
        if (requested.empty() || info.ID().toString() == requested) return info;
    }
    // A selected CID is sufficient for IPluginFactory::createInstance even if
    // a broken factory intermittently returns zero from countClasses(). The
    // isolated scanner supplied and validated this ID in a separate process.
    // Let the runtime worker try the exact class instead of rejecting it on
    // unreliable enumeration metadata.
    if (!requested.empty()) {
        auto id = VST3::UID::fromString(requested);
        if (id) {
            VST3::Hosting::ClassInfo fallback;
            fallback.get().classID = *id;
            fallback.get().cardinality = 1;
            fallback.get().category = kVstAudioEffectClass;
            fallback.get().name = requested;
            return fallback;
        }
    }
    throw std::runtime_error(requested.empty()
        ? "No VST3 audio-effect class was found in the module"
        : "The requested VST3 class was not found in the module");
}

std::size_t main_bus_channels(IComponent& component, BusDirection direction)
{
    if (component.getBusCount(kAudio, direction) <= 0) return 0;
    BusInfo info{};
    if (component.getBusInfo(kAudio, direction, 0, info) != kResultOk) return 0;
    return static_cast<std::size_t>(std::max<int32>(0, info.channelCount));
}

class EditorFrame final : public Steinberg::U::Implements<Steinberg::U::Directly<IPlugFrame>> {
public:
    explicit EditorFrame(void* window) : window_(window) {}

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override
    {
        if (!view || !size || size->getWidth() <= 0 || size->getHeight() <= 0) return kInvalidArgument;
#ifdef _WIN32
        RECT rectangle{0, 0, size->getWidth(), size->getHeight()};
        (void)AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        (void)SetWindowPos(static_cast<HWND>(window_), nullptr, 0, 0,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#elif defined(__APPLE__)
        NSWindow* window = (__bridge NSWindow*)window_;
        [window setContentSize:NSMakeSize(size->getWidth(), size->getHeight())];
#endif
        return view->onSize(size);
    }

private:
    void* window_ = nullptr;
};

#ifdef _WIN32
LRESULT CALLBACK editor_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

ATOM register_editor_window_class()
{
    static const ATOM result = [] {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = editor_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"Jam2IsolatedVst3Editor";
        return RegisterClassExW(&window_class);
    }();
    return result;
}

std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty()) return L"VST3 Plugin";
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"VST3 Plugin";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}
#endif

} // namespace

std::vector<PluginDescription> scan_vst3(const std::string& path)
{
    std::vector<PluginDescription> result;
    for (const auto& info : scan_vst3_factory_classes(path)) {
        if (info.category != kVstAudioEffectClass) continue;
        result.push_back({
            path,
            info.class_id,
            info.name,
            info.vendor,
            info.version,
            info.subcategories,
        });
    }
    return result;
}

std::vector<FactoryClassDescription> scan_vst3_factory_classes(const std::string& path)
{
    std::string error;
    const auto module = VST3::Hosting::Module::create(path, error);
    if (!module) throw std::runtime_error(error.empty() ? "Could not load VST3 module" : error);

    std::vector<FactoryClassDescription> result;
    for (const auto& info : module->getFactory().classInfos()) {
        result.push_back({
            info.category(),
            info.ID().toString(),
            info.name(),
            info.vendor(),
            info.version(),
            info.subCategoriesString(),
        });
    }
    return result;
}

class Vst3Instance::Impl final {
public:
    ~Impl() { reset(); }

    void reset() noexcept
    {
        close_editor();
        try {
            if (processor_) (void)processor_->setProcessing(false);
            if (component_) (void)component_->setActive(false);
        } catch (...) {
        }
        process_data_.unprepare();
        provider_ = nullptr;
        processor_ = nullptr;
        controller_ = nullptr;
        component_ = nullptr;
        module_.reset();
        description_ = {};
        input_channels_ = 0;
        output_channels_ = 0;
        configured_ = false;
    }

    bool open_editor() noexcept
    {
        editor_error_.clear();
        try {
            if (editor_view_) {
#ifdef _WIN32
                ShowWindow(editor_window_, SW_SHOW);
                SetForegroundWindow(editor_window_);
#elif defined(__APPLE__)
                [editor_window_ makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
#endif
                return true;
            }
            if (!controller_) { editor_error_ = "Plugin has no edit controller"; return false; }
            editor_view_ = Steinberg::owned(controller_->createView(ViewType::kEditor));
            if (!editor_view_) { editor_error_ = "Plugin did not create an editor view"; return false; }
            ViewRect rectangle{};
            if (editor_view_->getSize(&rectangle) != kResultOk ||
                rectangle.getWidth() <= 0 || rectangle.getHeight() <= 0) {
                rectangle = {0, 0, 640, 480};
            }
#ifdef _WIN32
            if (editor_view_->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue ||
                register_editor_window_class() == 0) {
                editor_error_ = "Plugin editor does not support a Win32 HWND or the host window class failed";
                editor_view_ = nullptr;
                return false;
            }
            RECT outer{0, 0, rectangle.getWidth(), rectangle.getHeight()};
            (void)AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
            const std::wstring title = utf8_to_wide(description_.name);
            editor_window_ = CreateWindowExW(0, L"Jam2IsolatedVst3Editor", title.c_str(),
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                outer.right - outer.left, outer.bottom - outer.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!editor_window_) {
                editor_error_ = "Could not create the isolated Win32 editor window (error " +
                    std::to_string(GetLastError()) + ")";
                editor_view_ = nullptr;
                return false;
            }
            editor_frame_ = Steinberg::owned(new EditorFrame(editor_window_));
            (void)editor_view_->setFrame(editor_frame_);
            if (editor_view_->attached(editor_window_, kPlatformTypeHWND) != kResultOk) {
                editor_error_ = "Plugin rejected attachment to the isolated Win32 editor window";
                close_editor();
                return false;
            }
            ShowWindow(editor_window_, SW_SHOW);
            UpdateWindow(editor_window_);
#elif defined(__APPLE__)
            if (editor_view_->isPlatformTypeSupported(kPlatformTypeNSView) != kResultTrue) {
                editor_error_ = "Plugin editor does not support an NSView";
                editor_view_ = nullptr;
                return false;
            }
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            NSRect frame = NSMakeRect(0, 0, rectangle.getWidth(), rectangle.getHeight());
            editor_window_ = [[NSWindow alloc] initWithContentRect:frame
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                backing:NSBackingStoreBuffered defer:NO];
            [editor_window_ setTitle:[NSString stringWithUTF8String:description_.name.c_str()]];
            editor_frame_ = Steinberg::owned(new EditorFrame((__bridge void*)editor_window_));
            (void)editor_view_->setFrame(editor_frame_);
            if (editor_view_->attached((__bridge void*)[editor_window_ contentView],
                    kPlatformTypeNSView) != kResultOk) {
                editor_error_ = "Plugin rejected attachment to the isolated macOS editor window";
                close_editor();
                return false;
            }
            [editor_window_ makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
#else
            editor_view_ = nullptr;
            return false;
#endif
            return true;
        } catch (...) {
            editor_error_ = "Plugin editor threw an exception while opening";
            close_editor();
            return false;
        }
    }

    void close_editor() noexcept
    {
        try {
            if (editor_view_) {
                (void)editor_view_->removed();
                (void)editor_view_->setFrame(nullptr);
            }
        } catch (...) {
        }
        editor_view_ = nullptr;
        editor_frame_ = nullptr;
#ifdef _WIN32
        if (editor_window_) DestroyWindow(editor_window_);
        editor_window_ = nullptr;
#elif defined(__APPLE__)
        if (editor_window_) {
            [editor_window_ close];
            [editor_window_ release];
        }
        editor_window_ = nil;
#endif
    }

    void pump_editor() noexcept
    {
        // Some plugins create internal message traffic during component
        // initialization. Do not let that non-audio work compete with the
        // transport loop unless Jam2 has actually opened the editor.
        if (!editor_view_) return;
#ifdef _WIN32
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
#elif defined(__APPLE__)
        @autoreleasepool {
            NSEvent* event = nil;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES])) {
                [NSApp sendEvent:event];
            }
        }
#endif
    }

    void load(const std::string& path, const std::string& class_id)
    {
        reset();
        std::string error;
        module_ = VST3::Hosting::Module::create(path, error);
        if (!module_) throw std::runtime_error(error.empty() ? "Could not load VST3 module" : error);

        const auto info = find_class(module_->getFactory(), class_id);
        PluginContextFactory::instance().setPluginContext(Steinberg::gStandardPluginContext);
        module_->getFactory().setHostContext(Steinberg::gStandardPluginContext);
        provider_ = Steinberg::owned(new PlugProvider(module_->getFactory(), info, true));
        if (!provider_ || !provider_->initialize()) {
            reset();
            throw std::runtime_error("VST3 component initialization failed");
        }
        component_ = provider_->getComponentPtr();
        controller_ = provider_->getControllerPtr();
        processor_ = Steinberg::FUnknownPtr<IAudioProcessor>(component_);
        if (!component_ || !processor_) {
            reset();
            throw std::runtime_error("VST3 class does not expose an audio processor");
        }

        description_ = {
            path, info.ID().toString(), info.name(), info.vendor(), info.version(),
            info.subCategoriesString(),
        };
        input_channels_ = main_bus_channels(*component_, kInput);
        output_channels_ = main_bus_channels(*component_, kOutput);
        if (input_channels_ > 2 || output_channels_ == 0 || output_channels_ > 2) {
            reset();
            throw std::runtime_error("Jam2 supports only zero, one, or two main VST3 input channels and one or two main output channels");
        }
        for (int32 bus = 0; bus < component_->getBusCount(kAudio, kInput); ++bus)
            (void)component_->activateBus(kAudio, kInput, bus, bus == 0 ? true : false);
        for (int32 bus = 0; bus < component_->getBusCount(kAudio, kOutput); ++bus)
            (void)component_->activateBus(kAudio, kOutput, bus, bus == 0 ? true : false);
        for (int32 bus = 0; bus < component_->getBusCount(kEvent, kInput); ++bus)
            (void)component_->activateBus(kEvent, kInput, bus, bus == 0 ? true : false);
    }

    bool set_main_arrangement(SpeakerArrangement input, SpeakerArrangement output)
    {
        const int32 input_busses = component_->getBusCount(kAudio, kInput);
        const int32 output_busses = component_->getBusCount(kAudio, kOutput);
        std::vector<SpeakerArrangement> inputs(static_cast<std::size_t>((std::max)(0, input_busses)));
        std::vector<SpeakerArrangement> outputs(static_cast<std::size_t>((std::max)(0, output_busses)));
        for (int32 bus = 0; bus < input_busses; ++bus)
            if (processor_->getBusArrangement(kInput, bus, inputs[static_cast<std::size_t>(bus)]) != kResultOk)
                inputs[static_cast<std::size_t>(bus)] = SpeakerArr::kEmpty;
        for (int32 bus = 0; bus < output_busses; ++bus)
            if (processor_->getBusArrangement(kOutput, bus, outputs[static_cast<std::size_t>(bus)]) != kResultOk)
                outputs[static_cast<std::size_t>(bus)] = SpeakerArr::kEmpty;
        if (!inputs.empty()) inputs[0] = input;
        if (!outputs.empty()) outputs[0] = output;
        return processor_->setBusArrangements(inputs.data(), input_busses,
            outputs.data(), output_busses) == kResultOk;
    }

    bool set_instrument_arrangement(SpeakerArrangement output)
    {
        const int32 input_busses = component_->getBusCount(kAudio, kInput);
        const int32 output_busses = component_->getBusCount(kAudio, kOutput);
        std::vector<SpeakerArrangement> inputs(static_cast<std::size_t>((std::max)(0, input_busses)));
        std::vector<SpeakerArrangement> outputs(static_cast<std::size_t>((std::max)(0, output_busses)));
        for (int32 bus = 0; bus < input_busses; ++bus)
            if (processor_->getBusArrangement(kInput, bus, inputs[static_cast<std::size_t>(bus)]) != kResultOk)
                inputs[static_cast<std::size_t>(bus)] = SpeakerArr::kEmpty;
        for (int32 bus = 0; bus < output_busses; ++bus)
            if (processor_->getBusArrangement(kOutput, bus, outputs[static_cast<std::size_t>(bus)]) != kResultOk)
                outputs[static_cast<std::size_t>(bus)] = SpeakerArr::kEmpty;
        if (!outputs.empty()) outputs[0] = output;
        return processor_->setBusArrangements(inputs.data(), input_busses,
            outputs.data(), output_busses) == kResultOk;
    }

    void configure(double sample_rate, std::size_t maximum_frames,
        std::size_t source_input_channels)
    {
        if (!component_ || !processor_) throw std::runtime_error("No VST3 plugin is loaded");
        if (!(sample_rate > 0.0) || maximum_frames == 0 || maximum_frames > 8192)
            throw std::invalid_argument("Invalid VST3 processing configuration");

        if (source_input_channels > 2) throw std::invalid_argument("Invalid VST3 source layout");
        const bool has_classification = !description_.subcategories.empty();
        const bool declared_instrument =
            description_.subcategories.find("Instrument") != std::string::npos;
        if (has_classification &&
            ((source_input_channels == 0 && !declared_instrument) ||
             (source_input_channels != 0 && declared_instrument)))
            throw std::runtime_error(source_input_channels == 0
                ? "The selected VST3 is an effect, not a MIDI instrument"
                : "The selected VST3 is an instrument, not an audio effect");

        bool arrangement = false;
        if (source_input_channels == 0) {
            // Instruments such as Surge XT expose an optional audio input bus.
            // Preserve its declared arrangement while negotiating only the
            // main output; the inactive input receives preallocated silence.
            arrangement = set_instrument_arrangement(SpeakerArr::kStereo) ||
                set_instrument_arrangement(SpeakerArr::kMono);
        } else if (source_input_channels == 1) {
            arrangement = set_main_arrangement(SpeakerArr::kMono, SpeakerArr::kMono) ||
                set_main_arrangement(SpeakerArr::kMono, SpeakerArr::kStereo) ||
                set_main_arrangement(SpeakerArr::kStereo, SpeakerArr::kStereo);
        } else {
            arrangement = set_main_arrangement(SpeakerArr::kStereo, SpeakerArr::kStereo) ||
                set_main_arrangement(SpeakerArr::kStereo, SpeakerArr::kMono);
        }
        if (!arrangement)
            throw std::runtime_error("VST3 plugin rejected Jam2's mono/stereo main-bus layouts");
        input_channels_ = main_bus_channels(*component_, kInput);
        output_channels_ = main_bus_channels(*component_, kOutput);
        if (input_channels_ > 2 || output_channels_ == 0 || output_channels_ > 2)
            throw std::runtime_error("VST3 main-bus layout changed outside Jam2's mono/stereo limits");

        process_data_.unprepare();
        // Buffer storage belongs to the worker transport. Passing zero asks
        // HostProcessData to allocate only bus/channel descriptors, preventing
        // it from ever deleting caller/shared-memory sample pointers.
        if (!process_data_.prepare(*component_, 0, kSample32))
            throw std::runtime_error("Could not prepare VST3 process buses");
        silent_input_.assign(maximum_frames, 0.0f);
        process_data_.inputEvents = &events_;
        process_data_.inputParameterChanges = &parameters_;
        process_data_.processContext = &context_;
        context_ = {};
        context_.sampleRate = sample_rate;
        context_.tempo = 120.0;
        context_.timeSigNumerator = 4;
        context_.timeSigDenominator = 4;
        context_.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;

        ProcessSetup setup{kRealtime, kSample32, static_cast<int32>(maximum_frames), sample_rate};
        if (processor_->setupProcessing(setup) != kResultOk) {
            reset();
            throw std::runtime_error("VST3 setupProcessing failed");
        }
        if (component_->setActive(true) != kResultOk) {
            reset();
            throw std::runtime_error("VST3 component activation failed");
        }
        // Steinberg's own AudioHost does not reject a plugin based on this
        // return value. Helix Native returns kResultFalse here despite being
        // active and processing correctly.
        (void)processor_->setProcessing(true);
        maximum_frames_ = maximum_frames;
        configured_ = true;
    }

    bool add_midi(const MidiMessage& message) noexcept
    {
        const auto status = static_cast<std::uint8_t>(message.status & 0xf0U);
        const int16 channel = static_cast<int16>(message.status & 0x0fU);
        Event event{};
        event.busIndex = 0;
        event.sampleOffset = static_cast<int32>(message.sample_offset);
        if (status == 0x90U && message.data2 != 0) {
            event.type = Event::kNoteOnEvent;
            event.noteOn.channel = channel;
            event.noteOn.pitch = static_cast<int16>(message.data1);
            event.noteOn.velocity = static_cast<float>(message.data2) / 127.0f;
            event.noteOn.noteId = -1;
        } else if (status == 0x80U || (status == 0x90U && message.data2 == 0)) {
            event.type = Event::kNoteOffEvent;
            event.noteOff.channel = channel;
            event.noteOff.pitch = static_cast<int16>(message.data1);
            event.noteOff.velocity = static_cast<float>(message.data2) / 127.0f;
            event.noteOff.noteId = -1;
        } else if (status == 0xa0U) {
            event.type = Event::kPolyPressureEvent;
            event.polyPressure.channel = channel;
            event.polyPressure.pitch = static_cast<int16>(message.data1);
            event.polyPressure.pressure = static_cast<float>(message.data2) / 127.0f;
            event.polyPressure.noteId = -1;
        } else {
            return add_controller(message, channel);
        }
        return events_.addEvent(event) == kResultOk;
    }

    bool add_controller(const MidiMessage& message, int16 channel) noexcept
    {
        if (!controller_) return false;
        const auto mapping = Steinberg::FUnknownPtr<IMidiMapping>(controller_);
        if (!mapping) return false;
        CtrlNumber number = static_cast<CtrlNumber>(0);
        ParamValue value = 0.0;
        const auto status = static_cast<std::uint8_t>(message.status & 0xf0U);
        if (status == 0xb0U) {
            number = static_cast<CtrlNumber>(message.data1);
            value = static_cast<double>(message.data2) / 127.0;
        } else if (status == 0xd0U) {
            number = kAfterTouch;
            value = static_cast<double>(message.data1) / 127.0;
        } else if (status == 0xc0U) {
            number = kCtrlProgramChange;
            value = static_cast<double>(message.data1) / 127.0;
        } else if (status == 0xe0U) {
            number = kPitchBend;
            value = static_cast<double>(static_cast<unsigned>(message.data1) |
                (static_cast<unsigned>(message.data2) << 7U)) / 16383.0;
        } else {
            return false;
        }
        ParamID id = kNoParamId;
        if (mapping->getMidiControllerAssignment(0, channel, number, id) != kResultOk)
            return false;
        int32 queue_index = 0;
        auto* queue = parameters_.addParameterData(id, queue_index);
        if (!queue) return false;
        int32 point_index = 0;
        return queue->addPoint(static_cast<int32>(message.sample_offset), value, point_index) == kResultOk;
    }

    bool process(
        std::span<const float> input_left,
        std::span<const float> input_right,
        std::span<const MidiMessage> midi,
        std::span<float> output_left,
        std::span<float> output_right) noexcept
    {
        if (!configured_ || !processor_ || output_left.empty() ||
            output_left.size() > maximum_frames_ || output_right.size() < output_left.size()) return false;
        try {
            const auto frames = output_left.size();
            // A plugin may mark an output channel silent without writing its
            // buffer. Zero first so silence never replays samples from the
            // preceding block as crackle.
            std::fill(output_left.begin(), output_left.begin() +
                static_cast<std::ptrdiff_t>(frames), 0.0f);
            std::fill(output_right.begin(), output_right.begin() +
                static_cast<std::ptrdiff_t>(frames), 0.0f);
            process_data_.numSamples = static_cast<int32>(frames);
            context_.continousTimeSamples = continuous_frame_;
            continuous_frame_ += static_cast<int64>(frames);
            events_.clear();
            parameters_.clearQueue();
            for (const auto& event : midi) {
                if (event.sample_offset < frames) (void)add_midi(event);
            }

            for (int32 channel = 0; channel < process_data_.numInputs; ++channel)
                process_data_.inputs[channel].silenceFlags = 0;
            for (int32 channel = 0; channel < process_data_.numOutputs; ++channel)
                process_data_.outputs[channel].silenceFlags = 0;

            if (process_data_.numInputs > 0) {
                const auto channels = process_data_.inputs[0].numChannels;
                if (channels > 0) {
                    auto* data = input_left.empty()
                        ? silent_input_.data() : const_cast<float*>(input_left.data());
                    process_data_.setChannelBuffer(kInput, 0, 0, data);
                }
                if (channels > 1) {
                    auto* data = input_right.empty()
                        ? (input_left.empty() ? silent_input_.data() : const_cast<float*>(input_left.data()))
                        : const_cast<float*>(input_right.data());
                    process_data_.setChannelBuffer(kInput, 0, 1, data);
                }
            }
            process_data_.setChannelBuffer(kOutput, 0, 0, output_left.data());
            if (process_data_.outputs[0].numChannels > 1)
                process_data_.setChannelBuffer(kOutput, 0, 1, output_right.data());
            else
                std::fill(output_right.begin(), output_right.begin() + static_cast<std::ptrdiff_t>(frames), 0.0f);

            if (processor_->process(process_data_) != kResultOk) return false;
            if (process_data_.numOutputs > 0) {
                const auto silence = process_data_.outputs[0].silenceFlags;
                if ((silence & 1ULL) != 0ULL)
                    std::fill(output_left.begin(), output_left.begin() +
                        static_cast<std::ptrdiff_t>(frames), 0.0f);
                if ((silence & 2ULL) != 0ULL)
                    std::fill(output_right.begin(), output_right.begin() +
                        static_cast<std::ptrdiff_t>(frames), 0.0f);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    VST3::Hosting::Module::Ptr module_;
    Steinberg::IPtr<PlugProvider> provider_;
    Steinberg::IPtr<IComponent> component_;
    Steinberg::IPtr<IEditController> controller_;
    Steinberg::IPtr<IAudioProcessor> processor_;
    PluginDescription description_;
    HostProcessData process_data_;
    EventList events_;
    ParameterChanges parameters_;
    ProcessContext context_{};
    std::size_t input_channels_ = 0;
    std::size_t output_channels_ = 0;
    std::size_t maximum_frames_ = 0;
    std::vector<float> silent_input_;
    int64 continuous_frame_ = 0;
    bool configured_ = false;
    std::string editor_error_;
    Steinberg::IPtr<IPlugView> editor_view_;
    Steinberg::IPtr<EditorFrame> editor_frame_;
#ifdef _WIN32
    HWND editor_window_ = nullptr;
#elif defined(__APPLE__)
    NSWindow* editor_window_ = nil;
#endif
};

Vst3Instance::Vst3Instance() : impl_(std::make_unique<Impl>()) {}
Vst3Instance::~Vst3Instance() = default;
void Vst3Instance::load(const std::string& path, const std::string& class_id) { impl_->load(path, class_id); }
void Vst3Instance::configure(double sample_rate, std::size_t maximum_frames,
    std::size_t source_input_channels)
{ impl_->configure(sample_rate, maximum_frames, source_input_channels); }
bool Vst3Instance::process(std::span<const float> left, std::span<const float> right,
    std::span<const MidiMessage> midi, std::span<float> output_left,
    std::span<float> output_right) noexcept
{ return impl_->process(left, right, midi, output_left, output_right); }
void Vst3Instance::reset() noexcept { impl_->reset(); }
bool Vst3Instance::open_editor() noexcept { return impl_->open_editor(); }
void Vst3Instance::close_editor() noexcept { impl_->close_editor(); }
void Vst3Instance::pump_editor() noexcept { impl_->pump_editor(); }
const std::string& Vst3Instance::editor_error() const noexcept { return impl_->editor_error_; }
const PluginDescription& Vst3Instance::description() const noexcept { return impl_->description_; }
std::size_t Vst3Instance::input_channels() const noexcept { return impl_->input_channels_; }
std::size_t Vst3Instance::output_channels() const noexcept { return impl_->output_channels_; }
std::uint32_t Vst3Instance::latency_samples() const noexcept
{ return impl_->processor_ ? impl_->processor_->getLatencySamples() : 0U; }

} // namespace jam2::pluginhost
