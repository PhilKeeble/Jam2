#include "DaisyShowcase.hpp"

#include "Control/adsr.h"
#include "Effects/chorus.h"
#include "Effects/overdrive.h"
#include "Effects/wavefolder.h"
#include "Filters/ladder.h"
#include "Filters/svf.h"
#include "PhysicalModeling/stringvoice.h"
#include "Synthesis/fm2.h"
#include "Synthesis/harmonic_osc.h"
#include "Synthesis/oscillator.h"
#include "Synthesis/variableshapeosc.h"

#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace jam2::experiment {
namespace {

constexpr float kSampleRate = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;

struct StereoSample {
    float left = 0.0f;
    float right = 0.0f;
};

using StereoAudio = std::vector<StereoSample>;

float midiFrequency(int midi)
{
    return 440.0f * std::pow(2.0f, (static_cast<float>(midi) - 69.0f) / 12.0f);
}

float smoothstep(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float edgeFade(std::size_t frame, std::size_t frames)
{
    const std::size_t fadeFrames = static_cast<std::size_t>(kSampleRate * 0.025f);
    const float in = std::min(1.0f, static_cast<float>(frame) / fadeFrames);
    const float out = std::min(
        1.0f,
        static_cast<float>(frames - std::min(frame, frames)) / fadeFrames);
    return smoothstep(std::min(in, out));
}

void normalize(
    StereoAudio& audio,
    float targetPeak = 0.86f,
    float targetRms = 0.18f)
{
    float peak = 0.0f;
    double sumSquares = 0.0;
    for (const StereoSample sample : audio) {
        peak = std::max(peak, std::abs(sample.left));
        peak = std::max(peak, std::abs(sample.right));
        sumSquares +=
            static_cast<double>(sample.left) * sample.left +
            static_cast<double>(sample.right) * sample.right;
    }
    if (peak < 1.0e-7f) return;
    const float rms = static_cast<float>(
        std::sqrt(sumSquares / static_cast<double>(audio.size() * 2)));
    const float gain = std::min({
        8.0f,
        targetPeak / peak,
        rms > 1.0e-7f ? targetRms / rms : 8.0f,
    });
    for (StereoSample& sample : audio) {
        sample.left *= gain;
        sample.right *= gain;
    }
}

void writeStereoWav(const QString& path, StereoAudio audio)
{
    normalize(audio);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(
            QStringLiteral("Cannot write %1").arg(path).toStdString());
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = static_cast<quint32>(audio.size() * 4);
    stream.writeRawData("RIFF", 4);
    stream << static_cast<quint32>(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << static_cast<quint32>(16)
           << static_cast<quint16>(1)
           << static_cast<quint16>(2)
           << static_cast<quint32>(kSampleRate)
           << static_cast<quint32>(kSampleRate * 4)
           << static_cast<quint16>(4)
           << static_cast<quint16>(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    for (const StereoSample sample : audio) {
        const auto toPcm = [](float value) {
            return static_cast<qint16>(std::lround(
                std::clamp(value, -0.98f, 0.98f) * 32767.0f));
        };
        stream << toPcm(sample.left) << toPcm(sample.right);
    }
    if (!file.commit()) {
        throw std::runtime_error(
            QStringLiteral("Cannot finalize %1").arg(path).toStdString());
    }
}

StereoAudio renderDiagnostic(
    float seconds,
    const std::function<float(std::size_t, float)>& sample)
{
    const std::size_t frames =
        static_cast<std::size_t>(std::lround(seconds * kSampleRate));
    StereoAudio audio(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float time = static_cast<float>(frame) / kSampleRate;
        const float value = sample(frame, time) * edgeFade(frame, frames);
        audio[frame] = {value, value};
    }
    return audio;
}

void setFreeRunningFrequency(
    daisysp::VariableShapeOscillator& oscillator,
    float frequency)
{
    // Daisy's VariableShapeOscillator names the free-running audible
    // oscillator "slave". SetFreq() controls only the sync master.
    oscillator.SetFreq(frequency);
    oscillator.SetSyncFreq(frequency);
}

StereoAudio variableShapeDemo()
{
    daisysp::VariableShapeOscillator oscillator;
    oscillator.Init(kSampleRate);
    setFreeRunningFrequency(oscillator, midiFrequency(48));
    return renderDiagnostic(7.0f, [&oscillator](std::size_t, float time) {
        const float section = std::fmod(time, 2.2f) / 2.2f;
        oscillator.SetWaveshape(std::clamp(time / 7.0f, 0.0f, 1.0f));
        oscillator.SetPW(0.08f + 0.84f * smoothstep(section));
        return 0.42f * oscillator.Process();
    });
}

StereoAudio pulseWidthDemo()
{
    daisysp::VariableShapeOscillator oscillator;
    oscillator.Init(kSampleRate);
    setFreeRunningFrequency(oscillator, midiFrequency(43));
    oscillator.SetWaveshape(1.0f);
    return renderDiagnostic(7.0f, [&oscillator](std::size_t, float time) {
        oscillator.SetPW(0.5f + 0.42f * std::sin(2.0f * kPi * 0.16f * time));
        return 0.38f * oscillator.Process();
    });
}

StereoAudio hardSyncDemo()
{
    daisysp::VariableShapeOscillator oscillator;
    oscillator.Init(kSampleRate);
    const float root = midiFrequency(43);
    oscillator.SetFreq(root);
    oscillator.SetWaveshape(0.16f);
    oscillator.SetPW(0.42f);
    oscillator.SetSync(true);
    return renderDiagnostic(7.0f, [&oscillator, root](std::size_t, float time) {
        const float sweep = 1.0f + 7.0f *
            smoothstep(0.5f + 0.5f * std::sin(2.0f * kPi * 0.075f * time - 1.2f));
        oscillator.SetSyncFreq(root * sweep);
        return 0.38f * oscillator.Process();
    });
}

StereoAudio fmDemo()
{
    daisysp::Fm2 fm;
    fm.Init(kSampleRate);
    fm.SetFrequency(midiFrequency(57));
    return renderDiagnostic(8.0f, [&fm](std::size_t, float time) {
        const int section = std::min(3, static_cast<int>(time / 2.0f));
        constexpr std::array<float, 4> ratios{1.0f, 2.0f, 3.0f, 1.4142f};
        const float local = std::fmod(time, 2.0f) / 2.0f;
        fm.SetRatio(ratios[static_cast<std::size_t>(section)]);
        fm.SetIndex(0.08f + 4.4f * smoothstep(local));
        return 0.44f * fm.Process();
    });
}

StereoAudio harmonicDemo()
{
    daisysp::HarmonicOscillator<16> oscillator;
    oscillator.Init(kSampleRate);
    oscillator.SetFreq(midiFrequency(48));
    return renderDiagnostic(8.0f, [&oscillator](std::size_t frame, float time) {
        if (frame % 64 == 0) {
            std::array<float, 16> amplitudes{};
            const float focus = 1.0f + 8.0f *
                (0.5f + 0.5f * std::sin(2.0f * kPi * 0.085f * time));
            float total = 0.0f;
            for (std::size_t harmonic = 0; harmonic < amplitudes.size(); ++harmonic) {
                const float distance =
                    (static_cast<float>(harmonic) + 1.0f - focus) / 2.7f;
                amplitudes[harmonic] =
                    std::exp(-distance * distance) /
                    (1.0f + 0.17f * static_cast<float>(harmonic));
                total += amplitudes[harmonic];
            }
            for (float& amplitude : amplitudes) amplitude *= 0.82f / total;
            oscillator.SetAmplitudes(amplitudes.data());
        }
        return 0.58f * oscillator.Process();
    });
}

StereoAudio wavefoldDemo()
{
    daisysp::Oscillator oscillator;
    daisysp::Wavefolder folder;
    oscillator.Init(kSampleRate);
    oscillator.SetWaveform(daisysp::Oscillator::WAVE_SIN);
    oscillator.SetFreq(midiFrequency(48));
    oscillator.SetAmp(0.72f);
    folder.Init();
    return renderDiagnostic(7.0f, [&oscillator, &folder](std::size_t, float time) {
        const float sweep = 0.4f + 5.2f *
            smoothstep(0.5f + 0.5f * std::sin(2.0f * kPi * 0.075f * time - 1.1f));
        folder.SetGain(sweep);
        folder.SetOffset(0.08f * std::sin(2.0f * kPi * 0.11f * time));
        return 0.55f * folder.Process(oscillator.Process());
    });
}

StereoAudio ladderDemo()
{
    daisysp::VariableShapeOscillator oscillator;
    daisysp::LadderFilter filter;
    oscillator.Init(kSampleRate);
    setFreeRunningFrequency(oscillator, midiFrequency(43));
    oscillator.SetWaveshape(0.0f);
    oscillator.SetPW(0.78f);
    filter.Init(kSampleRate);
    filter.SetFilterMode(daisysp::LadderFilter::FilterMode::LP24);
    filter.SetRes(0.86f);
    filter.SetInputDrive(2.0f);
    filter.SetPassbandGain(0.25f);
    return renderDiagnostic(8.0f, [&oscillator, &filter](std::size_t, float time) {
        const float sweep = smoothstep(
            0.5f + 0.5f * std::sin(2.0f * kPi * 0.095f * time - 1.1f));
        filter.SetFreq(90.0f + 7200.0f * sweep);
        return 0.58f * filter.Process(0.7f * oscillator.Process());
    });
}

StereoAudio stateVariableDemo()
{
    daisysp::VariableShapeOscillator oscillator;
    daisysp::Svf filter;
    oscillator.Init(kSampleRate);
    setFreeRunningFrequency(oscillator, midiFrequency(48));
    oscillator.SetWaveshape(0.18f);
    oscillator.SetPW(0.68f);
    filter.Init(kSampleRate);
    filter.SetRes(0.72f);
    filter.SetDrive(0.35f);
    return renderDiagnostic(8.0f, [&oscillator, &filter](std::size_t, float time) {
        const float sweep = smoothstep(
            0.5f + 0.5f * std::sin(2.0f * kPi * 0.11f * time - 1.0f));
        filter.SetFreq(130.0f + 5200.0f * sweep);
        filter.Process(0.65f * oscillator.Process());
        const float morph =
            0.5f + 0.5f * std::sin(2.0f * kPi * 0.055f * time);
        return 0.72f * ((1.0f - morph) * filter.Low() + morph * filter.Band());
    });
}

enum class PatchKind {
    AnalogPoly,
    SubBass,
    SyncLead,
    EvolvingPad,
    PhysicalPluck,
    FmKeys,
};

struct NoteEvent {
    float start = 0.0f;
    float duration = 1.0f;
    int midi = 60;
    float velocity = 0.8f;
};

struct ShowcaseVoice {
    bool active = false;
    bool trigger = false;
    std::size_t startFrame = 0;
    std::size_t endFrame = 0;
    float frequency = 440.0f;
    float velocity = 0.8f;
    PatchKind kind = PatchKind::AnalogPoly;

    daisysp::VariableShapeOscillator oscillatorA;
    daisysp::VariableShapeOscillator oscillatorB;
    daisysp::Oscillator sub;
    daisysp::Fm2 fm;
    daisysp::HarmonicOscillator<16> harmonic;
    daisysp::StringVoice string;
    daisysp::Adsr amplitude;
    daisysp::Adsr modulation;
    daisysp::LadderFilter ladder;
    daisysp::Svf body;
    daisysp::Wavefolder folder;

    void start(const NoteEvent& event, PatchKind patch, std::size_t frame)
    {
        active = true;
        trigger = true;
        startFrame = frame;
        endFrame = frame + static_cast<std::size_t>(
            std::lround(event.duration * kSampleRate));
        frequency = midiFrequency(event.midi);
        velocity = event.velocity;
        kind = patch;

        oscillatorA.Init(kSampleRate);
        oscillatorB.Init(kSampleRate);
        sub.Init(kSampleRate);
        fm.Init(kSampleRate);
        harmonic.Init(kSampleRate);
        string.Init(kSampleRate);
        amplitude.Init(kSampleRate);
        modulation.Init(kSampleRate);
        ladder.Init(kSampleRate);
        body.Init(kSampleRate);
        folder.Init();

        setFreeRunningFrequency(oscillatorA, frequency);
        setFreeRunningFrequency(oscillatorB, frequency * 1.0035f);
        sub.SetFreq(frequency * 0.5f);
        sub.SetAmp(1.0f);
        sub.SetWaveform(daisysp::Oscillator::WAVE_SIN);
        fm.SetFrequency(frequency);
        harmonic.SetFreq(frequency);
        string.SetFreq(frequency);
        string.SetAccent(velocity);
        body.SetFreq(std::clamp(frequency * 2.2f, 120.0f, 4200.0f));
        body.SetRes(0.48f);
        body.SetDrive(0.18f);
        ladder.SetFilterMode(daisysp::LadderFilter::FilterMode::LP24);
        ladder.SetPassbandGain(0.22f);

        amplitude.SetAttackTime(0.006f);
        amplitude.SetDecayTime(0.20f);
        amplitude.SetSustainLevel(0.68f);
        amplitude.SetReleaseTime(0.35f);
        modulation.SetAttackTime(0.003f);
        modulation.SetDecayTime(0.30f);
        modulation.SetSustainLevel(0.04f);
        modulation.SetReleaseTime(0.24f);

        switch (kind) {
        case PatchKind::AnalogPoly:
            oscillatorA.SetWaveshape(0.10f);
            oscillatorA.SetPW(0.69f);
            oscillatorB.SetWaveshape(0.72f);
            oscillatorB.SetPW(0.46f);
            amplitude.SetAttackTime(0.012f);
            amplitude.SetDecayTime(0.24f);
            amplitude.SetSustainLevel(0.62f);
            amplitude.SetReleaseTime(0.46f);
            ladder.SetRes(0.44f);
            ladder.SetInputDrive(1.55f);
            break;
        case PatchKind::SubBass:
            oscillatorA.SetWaveshape(0.52f);
            oscillatorA.SetPW(0.36f);
            oscillatorB.SetWaveshape(1.0f);
            oscillatorB.SetPW(0.51f);
            amplitude.SetAttackTime(0.003f);
            amplitude.SetDecayTime(0.16f);
            amplitude.SetSustainLevel(0.72f);
            amplitude.SetReleaseTime(0.12f);
            modulation.SetDecayTime(0.14f);
            ladder.SetRes(0.52f);
            ladder.SetInputDrive(2.1f);
            folder.SetGain(1.45f);
            break;
        case PatchKind::SyncLead:
            oscillatorA.SetWaveshape(0.16f);
            oscillatorA.SetPW(0.44f);
            oscillatorA.SetSync(true);
            oscillatorB.SetWaveshape(0.0f);
            oscillatorB.SetPW(0.72f);
            amplitude.SetAttackTime(0.018f);
            amplitude.SetDecayTime(0.18f);
            amplitude.SetSustainLevel(0.78f);
            amplitude.SetReleaseTime(0.32f);
            ladder.SetRes(0.67f);
            ladder.SetInputDrive(1.7f);
            break;
        case PatchKind::EvolvingPad: {
            oscillatorA.SetWaveshape(0.05f);
            oscillatorA.SetPW(0.63f);
            oscillatorB.SetWaveshape(0.24f);
            oscillatorB.SetPW(0.38f);
            amplitude.SetAttackTime(0.62f);
            amplitude.SetDecayTime(1.2f);
            amplitude.SetSustainLevel(0.76f);
            amplitude.SetReleaseTime(1.45f);
            modulation.SetAttackTime(1.4f);
            modulation.SetDecayTime(1.8f);
            modulation.SetSustainLevel(0.42f);
            modulation.SetReleaseTime(1.0f);
            ladder.SetRes(0.38f);
            ladder.SetInputDrive(1.25f);
            std::array<float, 16> amplitudes{};
            amplitudes[0] = 0.44f;
            amplitudes[1] = 0.18f;
            amplitudes[2] = 0.11f;
            amplitudes[4] = 0.06f;
            amplitudes[6] = 0.035f;
            harmonic.SetAmplitudes(amplitudes.data());
            break;
        }
        case PatchKind::PhysicalPluck:
            string.SetStructure(0.38f);
            string.SetBrightness(0.58f + 0.24f * velocity);
            string.SetDamping(0.58f);
            string.SetSustain(false);
            amplitude.SetAttackTime(0.001f);
            amplitude.SetDecayTime(0.65f);
            amplitude.SetSustainLevel(0.04f);
            amplitude.SetReleaseTime(0.20f);
            break;
        case PatchKind::FmKeys:
            fm.SetRatio(2.0f);
            fm.SetIndex(3.4f);
            amplitude.SetAttackTime(0.004f);
            amplitude.SetDecayTime(0.72f);
            amplitude.SetSustainLevel(0.34f);
            amplitude.SetReleaseTime(0.66f);
            modulation.SetDecayTime(0.48f);
            modulation.SetSustainLevel(0.025f);
            ladder.SetRes(0.18f);
            ladder.SetInputDrive(1.18f);
            break;
        }
        amplitude.Retrigger(true);
        modulation.Retrigger(true);
    }

    float process(std::size_t frame, bool designed)
    {
        if (!active) return 0.0f;
        const bool gate = frame < endFrame;
        const float amp = amplitude.Process(gate);
        const float mod = modulation.Process(gate);
        const float age =
            static_cast<float>(frame - startFrame) / kSampleRate;
        const float vibrato = age > 0.13f
            ? 0.0024f * smoothstep((age - 0.13f) / 0.24f) *
                std::sin(2.0f * kPi * 5.15f * age)
            : 0.0f;
        float value = 0.0f;

        switch (kind) {
        case PatchKind::AnalogPoly: {
            setFreeRunningFrequency(
                oscillatorA,
                frequency * (1.0f + 0.0008f * std::sin(
                    2.0f * kPi * 0.27f * age)));
            setFreeRunningFrequency(oscillatorB, frequency * 1.0042f);
            value = designed
                ? 0.56f * oscillatorA.Process() + 0.44f * oscillatorB.Process()
                : oscillatorA.Process();
            if (designed) {
                ladder.SetFreq(
                    420.0f + frequency * 1.4f + mod * (2700.0f + 1500.0f * velocity));
                value = ladder.Process(0.78f * value);
            }
            break;
        }
        case PatchKind::SubBass:
            setFreeRunningFrequency(oscillatorA, frequency);
            setFreeRunningFrequency(oscillatorB, frequency * 2.002f);
            value = designed
                ? 0.56f * oscillatorA.Process() +
                    0.31f * sub.Process() +
                    0.13f * oscillatorB.Process()
                : oscillatorA.Process();
            if (designed) {
                value = folder.Process(0.72f * value);
                ladder.SetFreq(95.0f + frequency * 1.15f + mod * 1450.0f);
                value = ladder.Process(value);
            }
            break;
        case PatchKind::SyncLead:
            oscillatorA.SetFreq(frequency * (1.0f + vibrato));
            oscillatorA.SetSyncFreq(frequency * (2.0f + 4.2f * mod));
            setFreeRunningFrequency(oscillatorB, frequency * 0.501f);
            value = designed
                ? 0.74f * oscillatorA.Process() + 0.26f * oscillatorB.Process()
                : oscillatorA.Process();
            if (designed) {
                ladder.SetFreq(680.0f + frequency * 1.6f + mod * 4200.0f);
                value = ladder.Process(0.82f * value);
            }
            break;
        case PatchKind::EvolvingPad: {
            const float motion =
                0.5f + 0.5f * std::sin(2.0f * kPi * 0.085f * age);
            setFreeRunningFrequency(oscillatorA, frequency * 0.9972f);
            setFreeRunningFrequency(oscillatorB, frequency * 1.0038f);
            oscillatorA.SetPW(0.34f + 0.32f * motion);
            value = designed
                ? 0.38f * oscillatorA.Process() +
                    0.38f * oscillatorB.Process() +
                    0.24f * harmonic.Process()
                : oscillatorA.Process();
            if (designed) {
                ladder.SetFreq(
                    340.0f + frequency * 1.6f + mod * 1800.0f + motion * 900.0f);
                value = ladder.Process(0.68f * value);
            }
            break;
        }
        case PatchKind::PhysicalPluck: {
            const float stringValue = string.Process(trigger);
            trigger = false;
            if (designed) {
                body.SetFreq(std::clamp(frequency * 2.1f, 180.0f, 3600.0f));
                body.Process(stringValue);
                value = 0.76f * stringValue + 0.24f * body.Band();
            } else {
                value = stringValue;
            }
            break;
        }
        case PatchKind::FmKeys:
            fm.SetFrequency(frequency);
            fm.SetIndex(designed
                ? 0.42f + mod * (2.6f + 1.4f * velocity)
                : 1.15f);
            value = fm.Process();
            if (designed) {
                const float tremolo =
                    0.90f + 0.10f * std::sin(2.0f * kPi * 4.7f * age);
                ladder.SetFreq(1850.0f + 3400.0f * velocity);
                value = tremolo * ladder.Process(value);
            }
            break;
        }

        if (!gate && !amplitude.IsRunning()) active = false;
        return amp * velocity * value;
    }
};

std::vector<NoteEvent> chordSequence(
    const std::vector<std::vector<int>>& chords,
    float chordSeconds,
    float duration)
{
    std::vector<NoteEvent> events;
    for (std::size_t chord = 0; chord < chords.size(); ++chord) {
        for (std::size_t note = 0; note < chords[chord].size(); ++note) {
            events.push_back({
                static_cast<float>(chord) * chordSeconds,
                duration,
                chords[chord][note],
                0.68f + 0.07f * static_cast<float>((note + chord) % 3),
            });
        }
    }
    return events;
}

std::vector<NoteEvent> patchEvents(PatchKind kind)
{
    switch (kind) {
    case PatchKind::AnalogPoly:
        return chordSequence({
            {48, 55, 58, 63}, {44, 51, 55, 60},
            {51, 58, 62, 67}, {46, 53, 58, 62}},
            1.75f, 1.48f);
    case PatchKind::SubBass:
        return {
            {0.00f, .42f, 36, .95f}, {.48f, .24f, 36, .72f},
            {.78f, .34f, 39, .82f}, {1.22f, .24f, 43, .78f},
            {1.55f, .48f, 34, .94f}, {2.08f, .28f, 34, .72f},
            {2.44f, .34f, 41, .86f}, {2.88f, .36f, 39, .77f},
            {3.36f, .44f, 32, .96f}, {3.88f, .24f, 39, .76f},
            {4.20f, .32f, 44, .84f}, {4.64f, .26f, 43, .76f},
            {5.02f, .48f, 34, .94f}, {5.56f, .24f, 41, .74f},
            {5.88f, .34f, 39, .84f}, {6.30f, .42f, 36, .91f},
        };
    case PatchKind::SyncLead:
        return {
            {0.00f, .62f, 67, .78f}, {.66f, .36f, 70, .84f},
            {1.08f, .82f, 72, .92f}, {1.96f, .42f, 75, .80f},
            {2.44f, .92f, 74, .88f}, {3.42f, .42f, 70, .76f},
            {3.90f, .68f, 67, .84f}, {4.64f, .34f, 65, .74f},
            {5.04f, .46f, 67, .82f}, {5.56f, 1.10f, 70, .94f},
        };
    case PatchKind::EvolvingPad:
        return chordSequence({
            {48, 55, 58, 63, 67}, {44, 51, 55, 60, 63},
            {41, 48, 55, 58, 63}},
            2.7f, 2.45f);
    case PatchKind::PhysicalPluck: {
        constexpr std::array<int, 24> notes{
            48, 55, 63, 67, 58, 67, 63, 55,
            44, 51, 60, 63, 55, 63, 60, 51,
            46, 53, 62, 65, 58, 65, 62, 53};
        std::vector<NoteEvent> events;
        for (std::size_t index = 0; index < notes.size(); ++index) {
            events.push_back({
                static_cast<float>(index) * 0.285f,
                0.24f,
                notes[index],
                0.62f + 0.26f * static_cast<float>((index % 4) == 0),
            });
        }
        return events;
    }
    case PatchKind::FmKeys:
        return chordSequence({
            {50, 57, 60, 64, 67}, {47, 54, 57, 62, 65},
            {43, 50, 53, 57, 60}, {45, 52, 55, 59, 64}},
            1.8f, 1.54f);
    }
    return {};
}

float patchDuration(PatchKind kind)
{
    return kind == PatchKind::EvolvingPad ? 10.2f : 8.4f;
}

StereoAudio renderPatch(PatchKind kind, bool designed)
{
    const std::vector<NoteEvent> events = patchEvents(kind);
    const std::size_t frames =
        static_cast<std::size_t>(std::lround(patchDuration(kind) * kSampleRate));
    StereoAudio output(frames);
    std::array<ShowcaseVoice, 32> voices;
    std::size_t eventIndex = 0;

    daisysp::Chorus chorus;
    chorus.Init(kSampleRate);
    chorus.SetDelayMs(kind == PatchKind::EvolvingPad ? 18.0f : 11.0f);
    chorus.SetLfoDepth(kind == PatchKind::EvolvingPad ? 0.48f : 0.24f);
    chorus.SetLfoFreq(0.18f, 0.23f);
    chorus.SetFeedback(kind == PatchKind::EvolvingPad ? 0.16f : 0.06f);
    chorus.SetPan(0.18f, 0.82f);

    daisysp::Overdrive overdrive;
    overdrive.Init();
    overdrive.SetDrive(kind == PatchKind::SubBass ? 0.28f : 0.12f);

    const std::size_t delayLeftFrames =
        static_cast<std::size_t>(kSampleRate * 0.31f);
    const std::size_t delayRightFrames =
        static_cast<std::size_t>(kSampleRate * 0.43f);
    std::vector<float> delayLeft(delayLeftFrames, 0.0f);
    std::vector<float> delayRight(delayRightFrames, 0.0f);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float time = static_cast<float>(frame) / kSampleRate;
        while (eventIndex < events.size() &&
               events[eventIndex].start <= time + 0.5f / kSampleRate) {
            auto voice = std::find_if(
                voices.begin(), voices.end(),
                [](const ShowcaseVoice& candidate) { return !candidate.active; });
            if (voice == voices.end()) {
                voice = std::min_element(
                    voices.begin(), voices.end(),
                    [](const ShowcaseVoice& left, const ShowcaseVoice& right) {
                        return left.startFrame < right.startFrame;
                    });
            }
            voice->start(events[eventIndex], kind, frame);
            ++eventIndex;
        }

        float mono = 0.0f;
        int active = 0;
        for (ShowcaseVoice& voice : voices) {
            if (!voice.active) continue;
            mono += voice.process(frame, designed);
            ++active;
        }
        if (active > 0) mono /= std::sqrt(static_cast<float>(active));
        mono *= kind == PatchKind::PhysicalPluck ? 0.72f : 0.52f;

        if (!designed) {
            const float value = mono * edgeFade(frame, frames);
            output[frame] = {value, value};
            continue;
        }

        if (kind == PatchKind::SubBass || kind == PatchKind::SyncLead) {
            mono = overdrive.Process(mono);
        }

        float left = mono;
        float right = mono;
        const bool useChorus =
            kind == PatchKind::AnalogPoly ||
            kind == PatchKind::EvolvingPad ||
            kind == PatchKind::FmKeys;
        if (useChorus) {
            chorus.Process(mono);
            const float wet = kind == PatchKind::EvolvingPad ? 0.48f : 0.28f;
            left = (1.0f - wet) * mono + wet * chorus.GetLeft();
            right = (1.0f - wet) * mono + wet * chorus.GetRight();
        }

        const bool useDelay =
            kind == PatchKind::SyncLead ||
            kind == PatchKind::EvolvingPad ||
            kind == PatchKind::PhysicalPluck;
        if (useDelay) {
            const std::size_t leftIndex = frame % delayLeft.size();
            const std::size_t rightIndex = frame % delayRight.size();
            const float delayedLeft = delayLeft[leftIndex];
            const float delayedRight = delayRight[rightIndex];
            const float feedback = kind == PatchKind::EvolvingPad ? 0.38f : 0.24f;
            delayLeft[leftIndex] = left + feedback * delayedRight;
            delayRight[rightIndex] = right + feedback * delayedLeft;
            const float wet = kind == PatchKind::EvolvingPad ? 0.24f : 0.16f;
            left += wet * delayedLeft;
            right += wet * delayedRight;
        }

        const float fade = edgeFade(frame, frames);
        output[frame] = {
            fade * std::tanh(1.08f * left),
            fade * std::tanh(1.08f * right),
        };
    }
    return output;
}

QJsonObject clip(
    const QString& title,
    const QString& model,
    const QString& path,
    const QString& listenFor)
{
    return {
        {QStringLiteral("title"), title},
        {QStringLiteral("model"), model},
        {QStringLiteral("path"), path},
        {QStringLiteral("listenFor"), listenFor},
    };
}

} // namespace

void renderDaisyShowcase(const QDir& site)
{
    const QString relativeAudioRoot = QStringLiteral("audio/daisy-showcase");
    QDir audio(site.absoluteFilePath(relativeAudioRoot));
    if (!audio.exists() && !site.mkpath(relativeAudioRoot)) {
        throw std::runtime_error("Cannot create Daisy showcase audio directory.");
    }

    const auto render = [&audio](
        const QString& fileName,
        StereoAudio (*renderer)()) {
        writeStereoWav(audio.absoluteFilePath(fileName), renderer());
    };
    render(QStringLiteral("01-variable-shape.wav"), variableShapeDemo);
    render(QStringLiteral("02-pulse-width.wav"), pulseWidthDemo);
    render(QStringLiteral("03-hard-sync.wav"), hardSyncDemo);
    render(QStringLiteral("04-fm-spectrum.wav"), fmDemo);
    render(QStringLiteral("05-additive-harmonics.wav"), harmonicDemo);
    render(QStringLiteral("06-wavefolding.wav"), wavefoldDemo);
    render(QStringLiteral("07-ladder-filter.wav"), ladderDemo);
    render(QStringLiteral("08-state-variable-filter.wav"), stateVariableDemo);

    struct PatchDescription {
        PatchKind kind;
        QString id;
        QString title;
        QString sourceModel;
        QString designedModel;
        QString explanation;
        QString listenFor;
    };
    const std::array<PatchDescription, 6> patches{{
        {
            PatchKind::AnalogPoly,
            QStringLiteral("analog-poly"),
            QStringLiteral("Shapeable analog poly"),
            QStringLiteral("One VariableShape oscillator per note"),
            QStringLiteral("Two shapeable oscillators, velocity filter envelope, ladder drive and stereo chorus"),
            QStringLiteral("A compact Drift-like subtractive topology for house, pop, synthwave, fusion and supporting chord layers."),
            QStringLiteral("Compare the static source with the attack brightness, filter movement, detuned body and stereo placement."),
        },
        {
            PatchKind::SubBass,
            QStringLiteral("sub-bass"),
            QStringLiteral("Driven sub bass"),
            QStringLiteral("One VariableShape oscillator per note"),
            QStringLiteral("Shapeable oscillator, sine sub, octave colour, wavefolding, ladder envelope and overdrive"),
            QStringLiteral("A layered mono-style bass assembled from Daisy building blocks rather than a finished bass module."),
            QStringLiteral("Listen for weight below the main oscillator, controlled brightness on attacks and harmonic audibility on small speakers."),
        },
        {
            PatchKind::SyncLead,
            QStringLiteral("sync-lead"),
            QStringLiteral("Expressive sync lead"),
            QStringLiteral("Hard-synced VariableShape oscillator"),
            QStringLiteral("Envelope-driven sync ratio, sub layer, resonant ladder filter, delayed vibrato, drive and stereo delay"),
            QStringLiteral("An example of Jam2 articulation data becoming continuous timbral movement rather than only note volume."),
            QStringLiteral("Notice how each note opens differently, then develops vibrato and a changing sync spectrum."),
        },
        {
            PatchKind::EvolvingPad,
            QStringLiteral("evolving-pad"),
            QStringLiteral("Evolving harmonic pad"),
            QStringLiteral("One slowly moving VariableShape oscillator per note"),
            QStringLiteral("Two drifting shape oscillators, additive harmonics, slow filter envelope, chorus and cross-delay"),
            QStringLiteral("An Equator-inspired layered source idea kept to a small fixed topology."),
            QStringLiteral("Listen for gradual spectral motion inside sustained chords and the distinction between source complexity and effects."),
        },
        {
            PatchKind::PhysicalPluck,
            QStringLiteral("physical-pluck"),
            QStringLiteral("Physical-model pluck"),
            QStringLiteral("Daisy StringVoice alone"),
            QStringLiteral("Velocity-shaped StringVoice, resonant body emphasis and restrained stereo delay"),
            QStringLiteral("The physical path is useful for synthetic plucks and hybrid body textures, not presented as a finished nylon guitar."),
            QStringLiteral("Compare the raw string excitation with the more focused body resonance and sense of space."),
        },
        {
            PatchKind::FmKeys,
            QStringLiteral("fm-keys"),
            QStringLiteral("Velocity-sensitive FM keys"),
            QStringLiteral("FM2 at a fixed modulation index"),
            QStringLiteral("Velocity-scaled FM index envelope, dynamic filtering, tremolo and stereo chorus"),
            QStringLiteral("A small electric-key topology showing why a fixed FM block in the original experiment understated the available range."),
            QStringLiteral("Listen for harder velocities producing brighter attacks while the body settles into a warmer sustained tone."),
        },
    }};

    QJsonArray voiceGroups;
    for (const PatchDescription& patch : patches) {
        const QString sourceName = patch.id + QStringLiteral("-source.wav");
        const QString designedName = patch.id + QStringLiteral("-designed.wav");
        writeStereoWav(
            audio.absoluteFilePath(sourceName),
            renderPatch(patch.kind, false));
        writeStereoWav(
            audio.absoluteFilePath(designedName),
            renderPatch(patch.kind, true));
        voiceGroups.append(QJsonObject{
            {QStringLiteral("title"), patch.title},
            {QStringLiteral("explanation"), patch.explanation},
            {QStringLiteral("listenFor"), patch.listenFor},
            {QStringLiteral("clips"), QJsonArray{
                clip(
                    QStringLiteral("Source only"),
                    patch.sourceModel,
                    relativeAudioRoot + QStringLiteral("/") + sourceName,
                    QStringLiteral("The minimally shaped source using the same notes.")),
                clip(
                    QStringLiteral("Designed voice"),
                    patch.designedModel,
                    relativeAudioRoot + QStringLiteral("/") + designedName,
                    patch.listenFor),
            }},
        });
    }

    const QJsonArray diagnosticGroups{
        QJsonObject{
            {QStringLiteral("title"), QStringLiteral("Oscillator shape and spectrum")},
            {QStringLiteral("summary"), QStringLiteral(
                "Single sustained notes expose the source material before patches, arrangement or effects disguise it.")},
            {QStringLiteral("clips"), QJsonArray{
                clip(
                    QStringLiteral("Continuous shape morph"),
                    QStringLiteral("VariableShapeOscillator: ramp/triangle into pulse family"),
                    relativeAudioRoot + QStringLiteral("/01-variable-shape.wav"),
                    QStringLiteral("The waveform moves continuously rather than switching between a short fixed list.")),
                clip(
                    QStringLiteral("Pulse-width movement"),
                    QStringLiteral("VariableShapeOscillator pulse with slow width modulation"),
                    relativeAudioRoot + QStringLiteral("/02-pulse-width.wav"),
                    QStringLiteral("Hear the hollow-to-nasal motion available from one oscillator.")),
                clip(
                    QStringLiteral("Hard-sync sweep"),
                    QStringLiteral("VariableShapeOscillator with a swept internal sync ratio"),
                    relativeAudioRoot + QStringLiteral("/03-hard-sync.wav"),
                    QStringLiteral("The pitch remains stable while upper harmonics sweep through the note.")),
                clip(
                    QStringLiteral("FM ratio and index"),
                    QStringLiteral("FM2: four ratios, each moving from near-sine to a dense spectrum"),
                    relativeAudioRoot + QStringLiteral("/04-fm-spectrum.wav"),
                    QStringLiteral("The first three sections remain harmonic; the final irrational ratio becomes metallic.")),
                clip(
                    QStringLiteral("Moving additive focus"),
                    QStringLiteral("HarmonicOscillator with energy moving across sixteen partials"),
                    relativeAudioRoot + QStringLiteral("/05-additive-harmonics.wav"),
                    QStringLiteral("Listen to the spectral focus travel without a subtractive filter.")),
            }},
        },
        QJsonObject{
            {QStringLiteral("title"), QStringLiteral("Shaping and filter character")},
            {QStringLiteral("summary"), QStringLiteral(
                "These are deliberately obvious sweeps. In a patch the same processes would move over much smaller, musically controlled ranges.")},
            {QStringLiteral("clips"), QJsonArray{
                clip(
                    QStringLiteral("Wavefolding"),
                    QStringLiteral("Sine oscillator into Daisy Wavefolder with moving gain and asymmetry"),
                    relativeAudioRoot + QStringLiteral("/06-wavefolding.wav"),
                    QStringLiteral("A pure sine develops bright, complex harmonics without changing oscillator type.")),
                clip(
                    QStringLiteral("Driven ladder filter"),
                    QStringLiteral("Shape oscillator into 24 dB ladder filter with resonance and input drive"),
                    relativeAudioRoot + QStringLiteral("/07-ladder-filter.wav"),
                    QStringLiteral("Hear resonance, saturation and changing harmonic emphasis rather than a plain one-pole darkening.")),
                clip(
                    QStringLiteral("State-variable morph"),
                    QStringLiteral("Driven SVF crossfading from low-pass to band-pass while cutoff moves"),
                    relativeAudioRoot + QStringLiteral("/08-state-variable-filter.wav"),
                    QStringLiteral("The response can change character as well as cutoff frequency.")),
            }},
        },
    };

    const QJsonObject manifest{
        {QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("sampleRate"), static_cast<int>(kSampleRate)},
        {QStringLiteral("diagnostics"), diagnosticGroups},
        {QStringLiteral("voices"), voiceGroups},
    };
    QFile file(site.absoluteFilePath(QStringLiteral("showcase-manifest.js")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("Cannot write showcase-manifest.js.");
    }
    file.write("window.DAISY_SHOWCASE_MANIFEST = ");
    file.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    file.write(";\n");
}

} // namespace jam2::experiment
