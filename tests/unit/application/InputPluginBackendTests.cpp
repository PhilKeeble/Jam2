#include "InputPluginBackend.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::unique_ptr<jam2::application::InputPluginHost> load(
    jam2::application::InputPluginBackend& backend,
    QWidget& parent,
    QThreadPool& workers,
    const jam2::application::InputPluginLoadRequest& request,
    QString& name)
{
    std::unique_ptr<jam2::application::InputPluginHost> result;
    QStringList progressMessages;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const bool started = backend.selectAndStart(
        parent, workers, parent.thread(), request,
        [&result, &name, &loop](
            std::unique_ptr<jam2::application::InputPluginHost> host,
            QString loadedName) {
            result = std::move(host);
            name = std::move(loadedName);
            loop.quit();
        },
        [&progressMessages](int, const QString& text) {
            progressMessages.push_back(text);
        });
    require(started, "synthetic plugin load did not start");
    timeout.start(2000);
    loop.exec();
    require(timeout.isActive(), "synthetic plugin load timed out");
    require(result != nullptr, "synthetic plugin load omitted its host");
    require(progressMessages.size() == 2,
        "synthetic plugin load did not report bounded start/completion progress");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        QWidget parent;
        QThreadPool workers;
        workers.setMaxThreadCount(2);
        auto backend = jam2::application::makeSyntheticInputPluginBackend(5ms);

        jam2::application::InputPluginLoadRequest audioRequest;
        audioRequest.kind = jam2::audio::InputSourceKind::Audio;
        audioRequest.sampleRate = 48000.0;
        audioRequest.maximumFrames = 16;
        audioRequest.sourceInputChannels = 1;
        QString audioName;
        auto audio = load(*backend, parent, workers, audioRequest, audioName);
        require(audioName.startsWith(QStringLiteral("Automation Audio Effect")),
            "synthetic audio plugin identity changed");
        require(audio->healthy() && audio->renderer() != nullptr,
            "synthetic audio plugin was not healthy");
        require(audio->statusText().contains(QStringLiteral("active")) &&
                audio->errorText().isEmpty(),
            "synthetic audio plugin diagnostics changed");

        std::array<std::int32_t, 16> input{};
        std::array<std::int32_t, 16> output{};
        input.fill(1000);
        jam2::audio::InputSourceRenderRequest render;
        render.inputs[0] = input.data();
        render.input_channels = 1;
        render.frames = input.size();
        render.sample_rate = 48000.0;
        require(audio->renderer()->render_mono(render, output),
            "synthetic audio plugin did not render");
        require(std::all_of(output.cbegin(), output.cend(),
                [](std::int32_t sample) { return sample == -500; }),
            "synthetic audio effect did not apply its deterministic transform");
        audio->setAudioBypassed(true);
        require(audio->renderer()->render_mono(render, output) &&
                std::all_of(output.cbegin(), output.cend(),
                    [](std::int32_t sample) { return sample == 1000; }),
            "synthetic audio bypass did not restore dry audio");
        audio->openEditor();
        require(audio->editorOpen(), "synthetic audio editor did not open");
        audio->closeEditor();
        require(!audio->editorOpen(), "synthetic audio editor did not close");
        audio->openEditor();
        const auto audioStats = audio->stats();
        require(audioStats.submittedBlocks == 2 && audioStats.completedBlocks == 2 &&
                audioStats.negotiatedInputChannels == 1 &&
                audioStats.negotiatedOutputChannels == 1,
            "synthetic audio plugin diagnostics changed");
        audio->requestRetire();
        require(!audio->healthy() && !audio->editorOpen() &&
                !audio->renderer()->render_mono(render, output) &&
                audio->statusText().contains(QStringLiteral("retired")),
            "retired synthetic audio plugin remained active");

        jam2::application::InputPluginLoadRequest stereoRequest = audioRequest;
        stereoRequest.sourceInputChannels = 2;
        QString stereoName;
        auto stereo = load(*backend, parent, workers, stereoRequest, stereoName);
        std::array<std::int32_t, 16> rightInput{};
        rightInput.fill(3000);
        render.inputs[0] = input.data();
        render.inputs[1] = rightInput.data();
        render.input_channels = 2;
        require(stereo->renderer()->render_mono(render, output) &&
                std::all_of(output.cbegin(), output.cend(),
                    [](std::int32_t sample) { return sample == -1000; }),
            "synthetic stereo effect did not mix both injected channels");
        require(stereo->stats().negotiatedInputChannels == 2,
            "synthetic stereo diagnostics omitted the negotiated input shape");

        jam2::midi::EventQueue midiQueue;
        jam2::application::InputPluginLoadRequest midiRequest;
        midiRequest.kind = jam2::audio::InputSourceKind::MidiInstrument;
        midiRequest.midiQueue = &midiQueue;
        midiRequest.sampleRate = 48000.0;
        midiRequest.maximumFrames = 16;
        QString midiName;
        auto midi = load(*backend, parent, workers, midiRequest, midiName);
        require(midiName.startsWith(QStringLiteral("Automation MIDI Instrument")),
            "synthetic MIDI plugin identity changed");
        (void)midiQueue.push({1000, 0, 0x90, 60, 100, 3});
        render.inputs[0] = nullptr;
        render.input_channels = 0;
        require(midi->renderer()->render_mono(render, output) &&
                std::all_of(output.cbegin(), output.cend(),
                    [](std::int32_t sample) { return sample != 0; }),
            "synthetic MIDI note did not render audio");
        midi->setMidiMuted(true);
        require(midi->renderer()->render_mono(render, output) &&
                std::all_of(output.cbegin(), output.cend(),
                    [](std::int32_t sample) { return sample == 0; }),
            "synthetic MIDI mute did not silence its renderer");
        midi->setMidiMuted(false);
        (void)midiQueue.push({2000, 0, 0x90, 62, 100, 3});
        require(midi->renderer()->render_mono(render, output),
            "synthetic MIDI renderer did not consume its second note");
        midi->requestMidiReset();
        require(midi->renderer()->render_mono(render, output) &&
                std::all_of(output.cbegin(), output.cend(),
                    [](std::int32_t sample) { return sample == 0; }),
            "synthetic MIDI reset did not clear held notes");
        require(midi->stats().midiEventsConsumed == 2,
            "synthetic MIDI diagnostics did not count injected events");

        jam2::application::InputPluginLoadRequest invalid = audioRequest;
        invalid.maximumFrames = 0;
        QString invalidProgress;
        require(!backend->selectAndStart(parent, workers, parent.thread(), invalid,
                [](auto, auto) {},
                [&invalidProgress](int, const QString& text) {
                    invalidProgress = text;
                }) && !invalidProgress.isEmpty(),
            "invalid synthetic plugin request was accepted without diagnostics");
        invalid = audioRequest;
        invalid.sourceInputChannels = 0;
        require(!backend->selectAndStart(parent, workers, parent.thread(), invalid,
                [](auto, auto) {}, {}),
            "zero-input audio plugin request was accepted");
        invalid = audioRequest;
        invalid.midiQueue = &midiQueue;
        require(!backend->selectAndStart(parent, workers, parent.thread(), invalid,
                [](auto, auto) {}, {}),
            "audio plugin request with a MIDI queue was accepted");
        invalid = midiRequest;
        invalid.midiQueue = nullptr;
        require(!backend->selectAndStart(parent, workers, parent.thread(), invalid,
                [](auto, auto) {}, {}),
            "MIDI instrument request without an event queue was accepted");

        bool lateCompletion = false;
        auto transientParent = std::make_unique<QWidget>();
        auto delayed = jam2::application::makeSyntheticInputPluginBackend(50ms);
        require(delayed->selectAndStart(*transientParent, workers,
                transientParent->thread(), audioRequest,
                [&lateCompletion](auto, auto) { lateCompletion = true; }, {}),
            "transient synthetic plugin load did not start");
        transientParent.reset();
        QEventLoop cancellationLoop;
        QTimer::singleShot(100, &cancellationLoop, &QEventLoop::quit);
        cancellationLoop.exec();
        require(!lateCompletion,
            "destroyed synthetic plugin owner received a late completion");

        std::cout << "Jam2 input plugin backend tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Input plugin backend test failure: " << error.what() << '\n';
        return 1;
    }
}
