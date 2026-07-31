#include "ResearchDrumKit.hpp"

#include "daisysp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace jam2::practice {
namespace {

constexpr float kPiF = 3.14159265358979323846f;
constexpr double kPi = 3.14159265358979323846;

enum class Model {
    AnalogKick,
    SyntheticKick,
    AnalogSnare,
    SyntheticSnare,
    ShellSnare,
    Hat,
    RingHat,
    ShellTom,
    RimWood,
    CollisionShaker,
    SkinHandDrum,
    HandClap,
    WoodBlock,
    Tambourine,
    CrashCymbal,
    RideCymbal,
};

enum class Transient {
    Off,
    SoftBeater,
    HardBeater,
    Stick,
    HeadStrike,
    Rim,
    Click,
    Brush,
    Clap,
};

enum class Texture {
    Off,
    Wire,
    Dust,
    Air,
    MetalWash,
};

enum class Strength {
    Ghost,
    Normal,
    Accent,
};

struct RealisedHit {
    qint64 frame = 0;
    QString laneId;
    QString articulation;
    int velocity = 90;
    int repeatIndex = 0;
    std::uint32_t seed = 0;
    Strength strength = Strength::Normal;
    float excitation = 0.7f;
    float outputGain = 0.7f;
    float brightnessOffset = 0.0f;
    float decayScale = 1.0f;
    float driveScale = 1.0f;
};

Strength strengthFor(const ResearchDrumRenderEvent& event)
{
    if (event.articulation.contains(QStringLiteral("ghost")) ||
        event.articulation == QStringLiteral("edge")) {
        return Strength::Ghost;
    }
    if (event.articulation.contains(QStringLiteral("accent")) ||
        event.articulation.contains(QStringLiteral("rimshot")) ||
        event.articulation == QStringLiteral("firm") ||
        event.articulation == QStringLiteral("bell")) {
        return Strength::Accent;
    }
    return Strength::Normal;
}

RealisedHit realise(
    const ResearchDrumRenderEvent& event,
    const ResearchDrumPiece& piece)
{
    RealisedHit result;
    result.frame = event.frame;
    result.laneId = event.laneId;
    result.articulation = event.articulation;
    result.velocity = std::clamp(event.velocity, 1, 127);
    result.repeatIndex = event.repeatIndex;
    result.seed = event.seed;
    result.strength = strengthFor(event);
    const float velocity =
        static_cast<float>(result.velocity) / 127.0f;
    result.excitation = std::clamp(
        0.04f + 0.96f * std::pow(
            velocity, piece.excitationCurve),
        0.0f,
        1.0f);
    result.outputGain = std::clamp(
        0.025f + 0.975f * std::pow(
            velocity, piece.outputCurve),
        0.0f,
        1.0f);
    const float centered = velocity - 0.55f;
    result.brightnessOffset =
        centered * piece.brightnessAmount;
    result.decayScale = std::clamp(
        1.0f + centered * piece.decayAmount,
        0.35f,
        1.8f);
    result.driveScale = std::clamp(
        1.0f + centered * piece.driveAmount,
        0.5f,
        2.0f);
    return result;
}

Transient transientKind(const QString& value)
{
    if (value == QStringLiteral("soft-beater")) {
        return Transient::SoftBeater;
    }
    if (value == QStringLiteral("hard-beater")) {
        return Transient::HardBeater;
    }
    if (value == QStringLiteral("stick")) return Transient::Stick;
    if (value == QStringLiteral("head-strike")) {
        return Transient::HeadStrike;
    }
    if (value == QStringLiteral("rim")) return Transient::Rim;
    if (value == QStringLiteral("click")) return Transient::Click;
    if (value == QStringLiteral("brush")) return Transient::Brush;
    if (value == QStringLiteral("clap")) return Transient::Clap;
    return Transient::Off;
}

Texture textureKind(const QString& value)
{
    if (value == QStringLiteral("wire")) return Texture::Wire;
    if (value == QStringLiteral("dust")) return Texture::Dust;
    if (value == QStringLiteral("air")) return Texture::Air;
    if (value == QStringLiteral("metal-wash")) {
        return Texture::MetalWash;
    }
    return Texture::Off;
}

struct Voice {
    Model model = Model::AnalogKick;
    std::unique_ptr<daisysp::AnalogBassDrum> analogKick;
    std::unique_ptr<daisysp::SyntheticBassDrum> syntheticKick;
    std::unique_ptr<daisysp::AnalogSnareDrum> analogSnare;
    std::unique_ptr<daisysp::SyntheticSnareDrum> syntheticSnare;
    std::unique_ptr<daisysp::HiHat<>> hat;
    std::unique_ptr<
        daisysp::HiHat<daisysp::RingModNoise>> ringHat;
    std::unique_ptr<daisysp::Svf> identityFilterA;
    std::unique_ptr<daisysp::Svf> identityFilterB;
    std::unique_ptr<daisysp::Svf> identityFilterC;
    std::array<float, 12> identityPhase{};
    std::array<float, 12> identityFrequency{};
    std::array<float, 12> identityGain{};
    std::array<float, 12> identityEnvelope{};
    std::array<float, 12> identityDecay{};
    int identityModeCount = 0;
    float identityNoiseEnvelope = 0.0f;
    float identityNoiseDecay = 0.999f;
    float identityPitchEnvelope = 0.0f;
    float identityPitchDecay = 0.99f;
    float identityPitchSweep = 0.0f;
    float identityCollisionRate = 0.0f;
    float identityCollisionEnvelope = 0.0f;
    float identityCollisionDecay = 0.92f;
    std::array<float, 3> identityBandEnvelope{1.0f, 1.0f, 1.0f};
    std::array<float, 3> identityBandDecay{0.999f, 0.999f, 0.999f};
    qint64 endFrame = 1;
    int fadeStartAge = std::numeric_limits<int>::max();
    int endAge = std::numeric_limits<int>::max();
    float gain = 0.3f;
    std::uint32_t noiseState = 0x91e10da5U;
    Transient transient = Transient::Off;
    Texture texture = Texture::Off;
    std::unique_ptr<daisysp::Svf> transientFilter;
    std::unique_ptr<daisysp::Svf> textureFilter;
    float transientEnvelope = 0.0f;
    float transientDecay = 0.99f;
    float transientTone = 0.5f;
    float transientLevel = 0.0f;
    float textureEnvelope = 0.0f;
    float textureDecay = 0.999f;
    float textureTone = 0.5f;
    float textureLevel = 0.0f;
    float textureDensity = 0.5f;
    std::array<float, 192> textureDelay{};
    int textureDelayIndex = 0;
    float overlayPhase = 0.0f;
    int age = 0;
    float drive = 1.0f;
    float digitalRate = 1.0f;
    float digitalPhase = 1.0f;
    float digitalHeld = 0.0f;
    float quantizationLevels = 8388608.0f;
    float reconstructionCoefficient = 1.0f;
    float reconstructed = 0.0f;
    float dynamicFilterAmount = 0.0f;
    float roomSend = 0.0f;
    QString chokeGroup;
    float chokeGain = 1.0f;
    float chokeDecay = 1.0f;
    float sampleRate = 48000.0f;
    bool trigger = true;

    float processIdentityModes(float pitchScale = 1.0f)
    {
        float value = 0.0f;
        for (int index = 0; index < identityModeCount; ++index) {
            identityPhase[index] +=
                identityFrequency[index] * pitchScale / sampleRate;
            identityPhase[index] -= std::floor(identityPhase[index]);
            value +=
                std::sin(2.0f * kPiF * identityPhase[index]) *
                identityGain[index] *
                identityEnvelope[index];
            identityEnvelope[index] *= identityDecay[index];
        }
        return value;
    }

    float process()
    {
        const bool fire = std::exchange(trigger, false);
        noiseState = noiseState * 1664525U + 1013904223U;
        const float noise =
            static_cast<float>(
                static_cast<std::int32_t>(noiseState)) /
            2147483648.0f;
        float base = 0.0f;
        switch (model) {
        case Model::AnalogKick:
            base = analogKick->Process(fire);
            break;
        case Model::SyntheticKick:
            base = syntheticKick->Process(fire);
            break;
        case Model::AnalogSnare:
            base = analogSnare->Process(fire);
            break;
        case Model::SyntheticSnare:
            base = syntheticSnare->Process(fire);
            break;
        case Model::ShellSnare:
            identityFilterA->Process(noise);
            base =
                processIdentityModes() +
                0.075f *
                    (identityFilterA->Low() +
                     0.26f * identityFilterA->Band()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        case Model::Hat:
            base = hat->Process(fire);
            break;
        case Model::RingHat:
            base = ringHat->Process(fire);
            break;
        case Model::ShellTom: {
            const float pitchScale =
                1.0f + identityPitchSweep * identityPitchEnvelope;
            identityFilterA->Process(noise);
            base =
                processIdentityModes(pitchScale) +
                0.22f *
                    (0.72f * identityFilterA->Low() +
                     0.58f * identityFilterA->Band()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            identityPitchEnvelope *= identityPitchDecay;
            break;
        }
        case Model::RimWood:
            identityFilterA->Process(noise);
            base =
                processIdentityModes() +
                0.42f *
                    (identityFilterA->Band() +
                     0.40f * identityFilterA->High()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        case Model::CollisionShaker: {
            const float randomUnit =
                static_cast<float>(noiseState & 0x00ffffffU) /
                static_cast<float>(0x01000000U);
            if (randomUnit < identityCollisionRate / sampleRate) {
                identityCollisionEnvelope =
                    0.55f + 0.45f * std::abs(noise);
            }
            identityCollisionEnvelope *= identityCollisionDecay;
            identityFilterA->Process(
                noise * identityCollisionEnvelope);
            identityFilterB->Process(
                noise * identityCollisionEnvelope);
            base =
                (0.58f * identityFilterA->Band() +
                 0.42f * identityFilterB->High()) *
                identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        }
        case Model::SkinHandDrum: {
            const float pitchScale =
                1.0f + identityPitchSweep * identityPitchEnvelope;
            identityFilterA->Process(noise);
            base =
                processIdentityModes(pitchScale) +
                0.13f *
                    (identityFilterA->Band() +
                     0.20f * identityFilterA->High()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            identityPitchEnvelope *= identityPitchDecay;
            break;
        }
        case Model::HandClap: {
            identityFilterA->Process(noise);
            identityFilterB->Process(noise);
            const auto burstEnvelope =
                [this](int start, int width, float level) {
                    const int local = age - start;
                    if (local < 0 || local >= width) return 0.0f;
                    return level * std::exp(
                        -4.2f * static_cast<float>(local) /
                        std::max(1, width));
                };
            const int width =
                static_cast<int>(0.005f * sampleRate);
            const float early = std::max({
                burstEnvelope(0, width, 1.0f),
                burstEnvelope(
                    static_cast<int>(0.010f * sampleRate),
                    width,
                    0.86f),
                burstEnvelope(
                    static_cast<int>(0.019f * sampleRate),
                    width,
                    0.72f),
            });
            const float clapNoise =
                0.58f *
                    (identityFilterA->Band() +
                     0.18f * identityFilterA->High()) +
                0.42f *
                    (identityFilterB->Band() +
                     0.12f * identityFilterB->High());
            base =
                clapNoise *
                (0.82f * early +
                 0.22f * identityNoiseEnvelope);
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        }
        case Model::WoodBlock:
            identityFilterA->Process(noise);
            base =
                processIdentityModes() +
                0.10f * identityFilterA->High() *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        case Model::Tambourine: {
            const float randomUnit =
                static_cast<float>(noiseState & 0x00ffffffU) /
                static_cast<float>(0x01000000U);
            if (randomUnit < identityCollisionRate / sampleRate) {
                identityCollisionEnvelope =
                    0.45f + 0.55f * std::abs(noise);
            }
            identityCollisionEnvelope *= identityCollisionDecay;
            identityFilterA->Process(
                noise * identityCollisionEnvelope);
            identityFilterB->Process(
                noise * identityCollisionEnvelope);
            base =
                0.22f * processIdentityModes() +
                (0.50f * identityFilterA->Band() +
                 0.32f * identityFilterB->High()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        }
        case Model::CrashCymbal:
            identityFilterA->Process(noise);
            identityFilterB->Process(noise);
            identityFilterC->Process(noise);
            base =
                identityNoiseEnvelope * (
                    0.38f * identityBandEnvelope[0] *
                        identityFilterA->Band() +
                    0.34f * identityBandEnvelope[1] *
                        identityFilterB->Band() +
                    0.16f * identityBandEnvelope[2] *
                        identityFilterC->High()) +
                0.13f * processIdentityModes();
            identityNoiseEnvelope *= identityNoiseDecay;
            for (int band = 0; band < 3; ++band) {
                identityBandEnvelope[band] *=
                    identityBandDecay[band];
            }
            break;
        case Model::RideCymbal:
            identityFilterA->Process(noise);
            identityFilterB->Process(noise);
            identityFilterC->Process(noise);
            base =
                processIdentityModes() +
                identityNoiseEnvelope *
                    (0.16f * identityBandEnvelope[0] *
                         identityFilterA->Band() +
                     0.075f * identityBandEnvelope[1] *
                         identityFilterB->Band() +
                     0.010f * identityBandEnvelope[2] *
                         identityFilterC->Band());
            identityNoiseEnvelope *= identityNoiseDecay;
            for (int band = 0; band < 3; ++band) {
                identityBandEnvelope[band] *=
                    identityBandDecay[band];
            }
            break;
        }

        float transientValue = 0.0f;
        if (transient != Transient::Off &&
            transientEnvelope > 0.000001f) {
            const float frequency =
                transient == Transient::SoftBeater
                    ? 110.0f + 820.0f * transientTone
                    : transient == Transient::HardBeater
                        ? 480.0f + 2600.0f * transientTone
                        : transient == Transient::Rim
                            ? 650.0f + 3400.0f * transientTone
                            : 1100.0f + 5200.0f * transientTone;
            overlayPhase += frequency / sampleRate;
            overlayPhase -= std::floor(overlayPhase);
            const float tone =
                std::sin(2.0f * kPiF * overlayPhase);
            transientFilter->Process(noise);
            const float brightNoise =
                transientFilter->High() +
                0.35f * transientFilter->Band();
            const float bodyNoise =
                0.78f * transientFilter->Band() +
                0.22f * transientFilter->Low();
            switch (transient) {
            case Transient::SoftBeater:
                transientValue =
                    0.58f * tone + 0.42f * bodyNoise;
                break;
            case Transient::HardBeater:
                transientValue =
                    0.42f * tone +
                    0.38f * bodyNoise +
                    0.20f * brightNoise;
                break;
            case Transient::Stick:
                transientValue =
                    0.34f * tone +
                    0.42f * transientFilter->Band() +
                    0.24f * brightNoise;
                break;
            case Transient::HeadStrike:
                transientValue =
                    0.10f * tone +
                    0.68f * bodyNoise +
                    0.22f * transientFilter->Band();
                break;
            case Transient::Rim:
                transientValue =
                    0.68f * tone +
                    0.26f * transientFilter->Band() +
                    0.06f * brightNoise;
                break;
            case Transient::Click:
                transientValue = brightNoise;
                break;
            case Transient::Brush:
                transientValue =
                    0.70f * transientFilter->Band() +
                    0.30f * brightNoise;
                break;
            case Transient::Clap: {
                const int first =
                    static_cast<int>(0.009f * sampleRate);
                const int second =
                    static_cast<int>(0.018f * sampleRate);
                const int width =
                    static_cast<int>(0.004f * sampleRate);
                const bool burst =
                    age < width ||
                    (age >= first && age < first + width) ||
                    (age >= second && age < second + width);
                transientValue =
                    brightNoise * (burst ? 1.0f : 0.18f);
                break;
            }
            case Transient::Off:
                break;
            }
            transientValue *= transientLevel * transientEnvelope;
            transientEnvelope *= transientDecay;
        }

        float textureValue = 0.0f;
        if (texture != Texture::Off &&
            textureEnvelope > 0.000001f) {
            textureFilter->Process(noise);
            switch (texture) {
            case Texture::Wire: {
                const int first =
                    (textureDelayIndex + 192 - 53) % 192;
                const int second =
                    (textureDelayIndex + 192 - 79) % 192;
                const int third =
                    (textureDelayIndex + 192 - 113) % 192;
                const float filtered =
                    0.82f * textureFilter->Band() +
                    0.18f * textureFilter->High();
                textureDelay[textureDelayIndex] = filtered;
                textureValue =
                    0.68f * filtered -
                    0.34f * textureDelay[first] +
                    0.23f * textureDelay[second] -
                    0.12f * textureDelay[third];
                textureDelayIndex =
                    (textureDelayIndex + 1) % 192;
                break;
            }
            case Texture::Dust: {
                const std::uint32_t threshold =
                    static_cast<std::uint32_t>(
                        50000000.0f * textureDensity);
                textureValue =
                    (noiseState & 0x00ffffffU) < threshold
                        ? textureFilter->High() : 0.0f;
                break;
            }
            case Texture::Air:
                textureValue = textureFilter->High();
                break;
            case Texture::MetalWash:
                textureValue =
                    0.62f * textureFilter->Band() +
                    0.38f * std::sin(
                        2.0f * kPiF *
                        overlayPhase * 1.618f);
                break;
            case Texture::Off:
                break;
            }
            textureValue *= textureLevel * textureEnvelope;
            textureEnvelope *= textureDecay;
        }

        float value = base + transientValue + textureValue;
        value = std::tanh(drive * value) /
            std::max(1.0f, 0.82f * drive);
        digitalPhase += digitalRate;
        if (digitalPhase >= 1.0f) {
            digitalPhase -= std::floor(digitalPhase);
            digitalHeld =
                std::round(value * quantizationLevels) /
                quantizationLevels;
        }
        const float dynamicCoefficient =
            std::clamp(
                reconstructionCoefficient *
                    (1.0f + dynamicFilterAmount *
                        std::min(1.0f, std::abs(digitalHeld))),
                0.0001f,
                1.0f);
        reconstructed +=
            dynamicCoefficient * (digitalHeld - reconstructed);
        float lifetimeGain = 1.0f;
        if (age >= fadeStartAge) {
            lifetimeGain = std::clamp(
                static_cast<float>(endAge - age) /
                    std::max(1, endAge - fadeStartAge),
                0.0f,
                1.0f);
        }
        chokeGain *= chokeDecay;
        ++age;
        return reconstructed * chokeGain * lifetimeGain;
    }
};

void initialiseFilter(
    std::unique_ptr<daisysp::Svf>& filter,
    float frequency,
    float resonance,
    float sampleRate)
{
    filter = std::make_unique<daisysp::Svf>();
    filter->Init(sampleRate);
    filter->SetFreq(std::clamp(frequency, 80.0f, 19000.0f));
    filter->SetRes(std::clamp(resonance, 0.05f, 0.92f));
}

void setMode(
    Voice& voice,
    const RealisedHit& hit,
    int index,
    float frequency,
    float gain,
    float decaySeconds)
{
    const std::uint32_t variation =
        0x9e3779b9U *
            static_cast<std::uint32_t>(1 + hit.repeatIndex) ^
        0x85ebca6bU *
            static_cast<std::uint32_t>(index + 3);
    const float unit =
        static_cast<float>(variation & 0xffffU) / 65535.0f;
    const float signedUnit = 2.0f * unit - 1.0f;
    voice.identityModeCount =
        std::max(voice.identityModeCount, index + 1);
    voice.identityFrequency[index] = std::clamp(
        frequency * (1.0f + 0.0025f * signedUnit),
        20.0f,
        19000.0f);
    voice.identityPhase[index] = 0.012f + 0.16f * unit;
    voice.identityGain[index] =
        gain * (0.97f + 0.06f * unit);
    voice.identityEnvelope[index] =
        0.42f + 0.58f * hit.excitation;
    voice.identityDecay[index] = std::exp(
        -6.907755f /
        std::max(
            1.0f,
            decaySeconds * hit.decayScale *
                voice.sampleRate));
}

void setBandDecay(
    Voice& voice,
    const RealisedHit& hit,
    int index,
    float decaySeconds)
{
    voice.identityBandEnvelope[index] =
        0.48f + 0.52f * hit.excitation;
    voice.identityBandDecay[index] = std::exp(
        -6.907755f /
        std::max(
            1.0f,
            decaySeconds * hit.decayScale *
                voice.sampleRate));
}

void configureComponents(
    Voice& voice,
    const ResearchDrumPiece& piece,
    const RealisedHit& hit)
{
    voice.transient = transientKind(piece.transientType);
    voice.texture = textureKind(piece.textureType);
    voice.transientLevel =
        piece.transientLevel *
        (0.50f + 0.50f * hit.excitation);
    voice.transientTone = std::clamp(
        piece.transientTone + hit.brightnessOffset,
        0.0f,
        1.0f);
    voice.transientEnvelope = hit.excitation;
    voice.transientDecay = std::exp(
        -6.907755f /
        std::max(
            1.0f,
            piece.transientDecaySeconds *
                hit.decayScale * voice.sampleRate));
    voice.textureLevel =
        piece.textureLevel *
        (0.62f + 0.38f * hit.excitation);
    voice.textureTone = std::clamp(
        piece.textureTone + 0.65f * hit.brightnessOffset,
        0.0f,
        1.0f);
    voice.textureEnvelope =
        0.38f + 0.62f * hit.excitation;
    voice.textureDecay = std::exp(
        -6.907755f /
        std::max(
            1.0f,
            piece.textureDecaySeconds *
                hit.decayScale * voice.sampleRate));
    voice.textureDensity = piece.textureDensity;
    voice.transientFilter = std::make_unique<daisysp::Svf>();
    voice.transientFilter->Init(voice.sampleRate);
    const float transientFilterHz =
        voice.transient == Transient::SoftBeater
            ? 420.0f + 1500.0f * voice.transientTone
            : voice.transient == Transient::HardBeater
                ? 850.0f + 4400.0f * voice.transientTone
                : voice.transient == Transient::HeadStrike
                    ? 620.0f + 2600.0f * voice.transientTone
                    : voice.transient == Transient::Rim
                        ? 900.0f + 4800.0f *
                            voice.transientTone
                        : 1600.0f + 8200.0f *
                            voice.transientTone;
    voice.transientFilter->SetFreq(transientFilterHz);
    voice.transientFilter->SetRes(0.18f);
    voice.textureFilter = std::make_unique<daisysp::Svf>();
    voice.textureFilter->Init(voice.sampleRate);
    voice.textureFilter->SetFreq(
        450.0f + 12500.0f * voice.textureTone);
    voice.textureFilter->SetRes(
        0.18f + 0.55f * piece.textureDensity);
    voice.drive = piece.voiceDrive * hit.driveScale;
    voice.digitalRate = std::clamp(
        piece.digitalSampleRateHz / voice.sampleRate,
        0.001f,
        1.0f);
    voice.quantizationLevels = std::pow(
        2.0f,
        static_cast<float>(
            std::max(1, piece.digitalBitDepth - 1)));
    voice.reconstructionCoefficient = static_cast<float>(
        1.0 -
        std::exp(
            -2.0 * kPi *
            std::clamp(
                piece.reconstructionLowpassHz,
                200.0f,
                20000.0f) /
            voice.sampleRate));
    voice.dynamicFilterAmount = piece.dynamicFilterAmount;
    voice.roomSend = piece.roomSend;
    voice.chokeGroup = piece.chokeGroup;
    voice.chokeDecay = 1.0f;
    voice.noiseState ^= hit.seed;
}

std::optional<Model> modelForSource(const QString& source)
{
    if (source == QStringLiteral("daisy-analog-kick")) {
        return Model::AnalogKick;
    }
    if (source == QStringLiteral("daisy-synthetic-kick")) {
        return Model::SyntheticKick;
    }
    if (source == QStringLiteral("daisy-analog-snare")) {
        return Model::AnalogSnare;
    }
    if (source == QStringLiteral("daisy-synthetic-snare")) {
        return Model::SyntheticSnare;
    }
    if (source == QStringLiteral("jam2-shell-snare")) {
        return Model::ShellSnare;
    }
    if (source == QStringLiteral("daisy-metal")) return Model::Hat;
    if (source == QStringLiteral("daisy-ring-metal")) {
        return Model::RingHat;
    }
    if (source == QStringLiteral("jam2-shell-tom")) {
        return Model::ShellTom;
    }
    if (source == QStringLiteral("jam2-cross-stick")) {
        return Model::RimWood;
    }
    if (source == QStringLiteral("jam2-shaker")) {
        return Model::CollisionShaker;
    }
    if (source == QStringLiteral("jam2-hand-drum")) {
        return Model::SkinHandDrum;
    }
    if (source == QStringLiteral("jam2-hand-clap")) {
        return Model::HandClap;
    }
    if (source == QStringLiteral("jam2-wood-block")) {
        return Model::WoodBlock;
    }
    if (source == QStringLiteral("jam2-tambourine")) {
        return Model::Tambourine;
    }
    if (source == QStringLiteral("jam2-crash-cymbal")) {
        return Model::CrashCymbal;
    }
    if (source == QStringLiteral("jam2-ride-cymbal")) {
        return Model::RideCymbal;
    }
    return std::nullopt;
}

Voice makeVoice(
    const ResearchDrumPiece& piece,
    const RealisedHit& hit,
    const QString& source,
    float layerGain,
    qint64 totalFrames,
    int sampleRate)
{
    Voice voice;
    voice.sampleRate = static_cast<float>(sampleRate);
    const std::optional<Model> selected = modelForSource(source);
    if (!selected) {
        voice.endFrame = hit.frame;
        return voice;
    }
    voice.model = *selected;
    switch (voice.model) {
    case Model::AnalogKick:
        voice.analogKick =
            std::make_unique<daisysp::AnalogBassDrum>();
        voice.analogKick->Init(voice.sampleRate);
        voice.analogKick->SetFreq(piece.frequencyHz);
        voice.analogKick->SetAccent(hit.excitation);
        voice.analogKick->SetTone(std::clamp(
            piece.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.analogKick->SetDecay(std::clamp(
            piece.decay * hit.decayScale, 0.001f, 1.0f));
        voice.analogKick->SetAttackFmAmount(piece.fmAmount);
        voice.analogKick->SetSelfFmAmount(piece.colour);
        break;
    case Model::SyntheticKick:
        voice.syntheticKick =
            std::make_unique<daisysp::SyntheticBassDrum>();
        voice.syntheticKick->Init(voice.sampleRate);
        voice.syntheticKick->SetFreq(piece.frequencyHz);
        voice.syntheticKick->SetAccent(hit.excitation);
        voice.syntheticKick->SetTone(std::clamp(
            piece.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.syntheticKick->SetDecay(std::clamp(
            piece.decay * hit.decayScale, 0.001f, 1.0f));
        voice.syntheticKick->SetDirtiness(piece.colour);
        voice.syntheticKick->SetFmEnvelopeAmount(piece.fmAmount);
        voice.syntheticKick->SetFmEnvelopeDecay(std::clamp(
            0.08f + 0.45f * piece.decay, 0.0f, 1.0f));
        break;
    case Model::AnalogSnare:
        voice.analogSnare =
            std::make_unique<daisysp::AnalogSnareDrum>();
        voice.analogSnare->Init(voice.sampleRate);
        voice.analogSnare->SetFreq(piece.frequencyHz);
        voice.analogSnare->SetAccent(hit.excitation);
        voice.analogSnare->SetTone(std::clamp(
            piece.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.analogSnare->SetDecay(std::clamp(
            piece.decay * hit.decayScale, 0.001f, 1.0f));
        voice.analogSnare->SetSnappy(piece.colour);
        break;
    case Model::SyntheticSnare:
        voice.syntheticSnare =
            std::make_unique<daisysp::SyntheticSnareDrum>();
        voice.syntheticSnare->Init(voice.sampleRate);
        voice.syntheticSnare->SetFreq(piece.frequencyHz);
        voice.syntheticSnare->SetAccent(hit.excitation);
        voice.syntheticSnare->SetFmAmount(piece.fmAmount);
        voice.syntheticSnare->SetDecay(std::clamp(
            piece.decay * hit.decayScale, 0.001f, 1.0f));
        voice.syntheticSnare->SetSnappy(piece.colour);
        break;
    case Model::Hat:
        voice.hat = std::make_unique<daisysp::HiHat<>>();
        voice.hat->Init(voice.sampleRate);
        voice.hat->SetFreq(piece.frequencyHz);
        voice.hat->SetAccent(hit.excitation);
        voice.hat->SetTone(std::clamp(
            piece.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.hat->SetDecay(std::clamp(
            piece.decay * hit.decayScale, 0.001f, 1.0f));
        voice.hat->SetNoisiness(piece.colour);
        break;
    case Model::RingHat:
        voice.ringHat = std::make_unique<
            daisysp::HiHat<daisysp::RingModNoise>>();
        voice.ringHat->Init(voice.sampleRate);
        voice.ringHat->SetFreq(piece.frequencyHz);
        voice.ringHat->SetAccent(hit.excitation);
        voice.ringHat->SetTone(std::clamp(
            piece.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.ringHat->SetDecay(std::clamp(
            piece.decay * hit.decayScale, 0.001f, 1.0f));
        voice.ringHat->SetNoisiness(piece.colour);
        break;
    case Model::ShellSnare: {
        const float shellSeconds =
            0.045f + 0.30f * piece.decay;
        setMode(voice, hit, 0, piece.frequencyHz, 0.72f, shellSeconds);
        setMode(
            voice,
            hit,
            1,
            piece.frequencyHz * 1.79f,
            0.12f + 0.10f * piece.tone,
            shellSeconds * 0.58f);
        setMode(
            voice,
            hit,
            2,
            piece.frequencyHz * 2.34f,
            0.035f + 0.055f * piece.tone,
            shellSeconds * 0.37f);
        initialiseFilter(
            voice.identityFilterA,
            720.0f + 2100.0f * piece.tone,
            0.28f,
            voice.sampleRate);
        voice.identityNoiseEnvelope =
            0.30f + 0.30f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.008f + 0.022f * piece.tone) *
                    voice.sampleRate));
        break;
    }
    case Model::ShellTom: {
        const float shellSeconds =
            0.075f + 0.82f * piece.decay;
        setMode(voice, hit, 0, piece.frequencyHz, 0.72f, shellSeconds);
        setMode(
            voice, hit, 1, piece.frequencyHz * 1.031f,
            0.10f, shellSeconds * 0.76f);
        setMode(
            voice, hit, 2, piece.frequencyHz * 1.593f,
            0.12f + 0.16f * piece.tone,
            shellSeconds * 0.62f);
        setMode(
            voice, hit, 3, piece.frequencyHz * 2.136f,
            0.045f + 0.075f * piece.tone,
            shellSeconds * 0.38f);
        setMode(
            voice, hit, 4, piece.frequencyHz * 2.296f,
            0.020f + 0.040f * piece.tone,
            shellSeconds * 0.29f);
        initialiseFilter(
            voice.identityFilterA,
            900.0f + 3900.0f * piece.tone,
            0.34f,
            voice.sampleRate);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.006f + 0.024f * piece.tone) *
                    voice.sampleRate));
        voice.identityPitchEnvelope = hit.excitation;
        voice.identityPitchSweep =
            0.018f + 0.085f * piece.fmAmount;
        voice.identityPitchDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.012f + 0.040f * piece.decay) *
                    voice.sampleRate));
        break;
    }
    case Model::RimWood: {
        const float clickSeconds =
            0.007f + 0.055f * piece.decay;
        setMode(voice, hit, 0, piece.frequencyHz, 0.52f, clickSeconds);
        setMode(
            voice, hit, 1, piece.frequencyHz * 2.37f,
            0.31f, clickSeconds * 0.70f);
        setMode(
            voice, hit, 2, piece.frequencyHz * 3.91f,
            0.17f, clickSeconds * 0.48f);
        initialiseFilter(
            voice.identityFilterA,
            2100.0f + 6200.0f * piece.tone,
            0.28f,
            voice.sampleRate);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(1.0f, 0.0045f * voice.sampleRate));
        break;
    }
    case Model::CollisionShaker:
        initialiseFilter(
            voice.identityFilterA,
            piece.frequencyHz * 0.72f,
            0.30f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterB,
            piece.frequencyHz * 1.24f,
            0.24f,
            voice.sampleRate);
        voice.identityNoiseEnvelope =
            0.45f + 0.55f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.025f + 0.48f * piece.decay) *
                    hit.decayScale * voice.sampleRate));
        voice.identityCollisionRate =
            900.0f + 5200.0f * piece.colour;
        voice.identityCollisionDecay =
            0.82f + 0.16f * piece.tone;
        break;
    case Model::SkinHandDrum: {
        const float skinSeconds =
            0.045f + 0.72f * piece.decay;
        setMode(voice, hit, 0, piece.frequencyHz, 0.86f, skinSeconds);
        setMode(
            voice, hit, 1, piece.frequencyHz * 1.47f,
            0.08f + 0.14f * piece.tone,
            skinSeconds * 0.54f);
        setMode(
            voice, hit, 2, piece.frequencyHz * 2.09f,
            0.03f + 0.08f * piece.tone,
            skinSeconds * 0.34f);
        initialiseFilter(
            voice.identityFilterA,
            1100.0f + 4200.0f * piece.tone,
            0.31f,
            voice.sampleRate);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.005f + 0.020f * piece.tone) *
                    voice.sampleRate));
        voice.identityPitchEnvelope = hit.excitation;
        voice.identityPitchSweep =
            0.025f + 0.12f * piece.fmAmount;
        voice.identityPitchDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.010f + 0.025f * piece.decay) *
                    voice.sampleRate));
        break;
    }
    case Model::HandClap:
        initialiseFilter(
            voice.identityFilterA,
            900.0f + 1200.0f * piece.tone,
            0.30f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterB,
            2600.0f + 3600.0f * piece.tone,
            0.22f,
            voice.sampleRate);
        voice.identityNoiseEnvelope =
            0.42f + 0.58f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.050f + 0.62f * piece.decay) *
                    hit.decayScale * voice.sampleRate));
        break;
    case Model::WoodBlock: {
        const float seconds = 0.018f + 0.12f * piece.decay;
        setMode(voice, hit, 0, piece.frequencyHz, 0.64f, seconds);
        setMode(
            voice, hit, 1, piece.frequencyHz * 1.71f,
            0.25f, seconds * 0.72f);
        setMode(
            voice, hit, 2, piece.frequencyHz * 2.64f,
            0.11f, seconds * 0.48f);
        initialiseFilter(
            voice.identityFilterA,
            3200.0f + 4600.0f * piece.tone,
            0.22f,
            voice.sampleRate);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(1.0f, 0.0035f * voice.sampleRate));
        break;
    }
    case Model::Tambourine: {
        const std::array<float, 5> ratios{
            0.73f, 1.0f, 1.31f, 1.79f, 2.41f};
        for (int index = 0;
             index < static_cast<int>(ratios.size());
             ++index) {
            setMode(
                voice,
                hit,
                index,
                piece.frequencyHz * ratios[index],
                0.055f,
                0.035f + 0.16f * piece.decay *
                    (1.0f - 0.09f * index));
        }
        initialiseFilter(
            voice.identityFilterA,
            piece.frequencyHz * 0.78f,
            0.26f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterB,
            piece.frequencyHz * 1.42f,
            0.20f,
            voice.sampleRate);
        voice.identityNoiseEnvelope =
            0.42f + 0.58f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.06f + 0.62f * piece.decay) *
                    hit.decayScale * voice.sampleRate));
        voice.identityCollisionRate =
            1500.0f + 6200.0f * piece.colour;
        voice.identityCollisionDecay =
            0.86f + 0.12f * piece.tone;
        break;
    }
    case Model::CrashCymbal: {
        const float centre = piece.frequencyHz;
        initialiseFilter(
            voice.identityFilterA,
            centre * 0.38f,
            0.20f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterB,
            centre * 1.08f,
            0.17f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterC,
            centre * 2.38f,
            0.12f,
            voice.sampleRate);
        const std::array<float, 7> ratios{
            0.61f, 0.93f, 1.28f, 1.67f,
            2.19f, 2.83f, 3.71f};
        for (int index = 0;
             index < static_cast<int>(ratios.size());
             ++index) {
            setMode(
                voice,
                hit,
                index,
                centre * ratios[index],
                0.016f + 0.006f * piece.colour,
                0.22f + piece.decay *
                    (0.72f + 0.08f * index));
        }
        voice.identityNoiseEnvelope =
            0.30f + 0.42f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (1.05f + 4.0f * piece.decay) *
                    hit.decayScale * voice.sampleRate));
        setBandDecay(
            voice, hit, 0, 0.82f + 3.8f * piece.decay);
        setBandDecay(
            voice, hit, 1, 0.56f + 2.8f * piece.decay);
        setBandDecay(
            voice, hit, 2, 0.24f + 1.40f * piece.decay);
        break;
    }
    case Model::RideCymbal: {
        const float centre = piece.frequencyHz;
        const bool ghost = hit.strength == Strength::Ghost;
        const bool accent = hit.strength == Strength::Accent;
        const float pitchedRingScale =
            ghost
                ? 1.0f
                : std::clamp(
                    0.70f + 1.20f * piece.colour,
                    0.65f,
                    1.20f);
        const float strikePosition =
            accent ? 1.22f : ghost ? 0.82f : 1.0f;
        initialiseFilter(
            voice.identityFilterA,
            centre * (ghost ? 0.58f : 0.76f),
            0.18f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterB,
            centre * (accent ? 1.42f : 1.18f),
            0.15f,
            voice.sampleRate);
        initialiseFilter(
            voice.identityFilterC,
            centre * (accent ? 2.46f : 2.08f),
            0.10f,
            voice.sampleRate);
        const std::array<float, 5> ratios{
            0.92f, 1.37f, 1.89f, 2.62f, 3.54f};
        constexpr std::array<float, 5> gainRollOff{
            1.0f, 0.78f, 0.54f, 0.34f, 0.20f};
        constexpr std::array<float, 5> tailRollOff{
            1.0f, 0.82f, 0.65f, 0.50f, 0.38f};
        for (int index = 0;
             index < static_cast<int>(ratios.size());
             ++index) {
            const float primary =
                accent ? 0.23f : ghost ? 0.055f : 0.11f;
            const float overtone =
                accent ? 0.050f : ghost ? 0.014f : 0.028f;
            const float articulationTail =
                index == 0
                    ? (accent ? 0.34f : ghost ? 0.055f : 0.34f)
                    : (accent ? 0.21f : ghost ? 0.040f : 0.19f);
            const float modeDecay =
                articulationTail +
                piece.decay *
                    ((index == 0 ? 0.40f : 0.44f) +
                     (accent && index == 0 ? 0.08f : 0.0f));
            setMode(
                voice,
                hit,
                index,
                centre * ratios[index] * strikePosition,
                (index == 0 ? primary : overtone) *
                    pitchedRingScale * gainRollOff[index],
                modeDecay * tailRollOff[index]);
        }
        voice.identityNoiseEnvelope =
            (accent ? 0.13f : ghost ? 0.14f : 0.22f) +
            0.12f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                ((accent ? 0.82f : ghost ? 0.30f : 0.65f) +
                 (accent ? 2.6f : ghost ? 0.75f : 2.05f) *
                     piece.decay) *
                    hit.decayScale * voice.sampleRate));
        setBandDecay(
            voice,
            hit,
            0,
            (ghost ? 0.16f : accent ? 0.58f : 0.48f) +
                (ghost ? 0.65f : accent ? 1.85f : 1.50f) *
                    piece.decay);
        setBandDecay(
            voice,
            hit,
            1,
            (ghost ? 0.10f : accent ? 0.38f : 0.30f) +
                (ghost ? 0.42f : accent ? 1.20f : 0.98f) *
                    piece.decay);
        setBandDecay(
            voice,
            hit,
            2,
            (ghost ? 0.055f : accent ? 0.19f : 0.15f) +
                (ghost ? 0.24f : accent ? 0.64f : 0.52f) *
                    piece.decay);
        break;
    }
    }

    configureComponents(voice, piece, hit);
    voice.gain =
        hit.outputGain * piece.level * layerGain;
    float tailSeconds = 0.22f + 3.0f * piece.decay;
    switch (voice.model) {
    case Model::AnalogSnare:
    case Model::SyntheticSnare:
    case Model::ShellSnare:
        tailSeconds = 0.10f + 1.15f * piece.decay;
        break;
    case Model::ShellTom:
        tailSeconds = 0.18f + 1.10f * piece.decay;
        break;
    case Model::RimWood:
    case Model::WoodBlock:
        tailSeconds = 0.055f + 0.18f * piece.decay;
        break;
    case Model::CollisionShaker:
        tailSeconds = 0.08f + 0.62f * piece.decay;
        break;
    case Model::SkinHandDrum:
        tailSeconds = 0.12f + 0.90f * piece.decay;
        break;
    case Model::HandClap:
        tailSeconds = 0.12f + 0.82f * piece.decay;
        break;
    case Model::Tambourine:
        tailSeconds = 0.12f + 0.86f * piece.decay;
        break;
    case Model::CrashCymbal:
        tailSeconds = 0.85f + 4.4f * piece.decay;
        break;
    case Model::RideCymbal:
        tailSeconds =
            hit.strength == Strength::Accent
                ? 0.95f + 3.2f * piece.decay
                : hit.strength == Strength::Ghost
                    ? 0.38f + 1.0f * piece.decay
                    : 0.68f + 2.1f * piece.decay;
        break;
    default:
        break;
    }
    if (voice.transient != Transient::Off) {
        tailSeconds = std::max(
            tailSeconds,
            0.04f + 1.15f * piece.transientDecaySeconds);
    }
    if (voice.texture != Texture::Off) {
        tailSeconds = std::max(
            tailSeconds,
            0.04f + 1.15f * piece.textureDecaySeconds);
    }
    voice.endAge = std::max(
        1,
        static_cast<int>(tailSeconds * sampleRate));
    const int fadeSamples = std::min(
        voice.endAge / 4,
        static_cast<int>(
            (voice.model == Model::CrashCymbal ||
             voice.model == Model::RideCymbal
                ? 0.080f
                : 0.025f) *
            sampleRate));
    voice.fadeStartAge =
        std::max(0, voice.endAge - fadeSamples);
    voice.endFrame = std::min<qint64>(
        totalFrames,
        hit.frame +
            static_cast<qint64>(tailSeconds * sampleRate));
    return voice;
}

struct SubVoice {
    bool active = false;
    qint64 startFrame = 0;
    qint64 endFrame = 1;
    float velocity = 0.8f;
    float level = 0.0f;
    float roomSend = 0.0f;
    float noiseMix = 0.0f;
    std::uint32_t noiseState = 1;
    daisysp::Oscillator oscillator;
    daisysp::Adsr envelope;
    daisysp::Svf filter;

    void begin(
        const RealisedHit& hit,
        const ResearchDrumPiece& piece,
        int sampleRate)
    {
        active = true;
        startFrame = hit.frame;
        endFrame =
            startFrame +
            std::max<qint64>(
                1,
                static_cast<qint64>(
                    piece.synthGateSeconds * sampleRate));
        velocity =
            static_cast<float>(hit.velocity) / 127.0f;
        level = piece.synthLevel;
        roomSend = piece.roomSend;
        noiseMix = piece.synthNoiseMix;
        noiseState = hit.seed ^ 0x62a9d9edU;
        const float frequency =
            440.0f * std::pow(
                2.0f,
                (piece.synthMidiNote - 69) / 12.0f);
        oscillator.Init(static_cast<float>(sampleRate));
        oscillator.SetFreq(frequency);
        oscillator.SetAmp(1.0f);
        oscillator.SetWaveform(
            daisysp::Oscillator::WAVE_SIN);
        envelope.Init(static_cast<float>(sampleRate));
        envelope.SetAttackTime(piece.synthAttackSeconds);
        envelope.SetDecayTime(piece.synthDecaySeconds);
        envelope.SetSustainLevel(std::clamp(
            piece.synthSustain, 0.01f, 1.0f));
        envelope.SetReleaseTime(piece.synthReleaseSeconds);
        envelope.Retrigger(true);
        filter.Init(static_cast<float>(sampleRate));
        filter.SetFreq(std::clamp(
            piece.synthFilterCutoffHz +
                frequency * 0.72f,
            35.0f,
            15500.0f));
        filter.SetRes(0.16f);
        filter.SetDrive(0.08f);
    }

    float process(qint64 frame)
    {
        const bool gate = frame < endFrame;
        const float amp = envelope.Process(gate);
        noiseState = noiseState * 1664525U + 1013904223U;
        const float noise =
            static_cast<float>(
                static_cast<std::int32_t>(noiseState)) /
            2147483648.0f;
        const float value =
            oscillator.Process() + noiseMix * noise;
        filter.Process(value);
        if (!gate && !envelope.IsRunning()) active = false;
        return amp * velocity * filter.Low();
    }
};

} // namespace

ResearchDrumRenderResult renderResearchDrumVoices(
    const ResearchDrumKit& kit,
    const QVector<ResearchDrumRenderEvent>& events,
    qint64 totalFrames,
    int sampleRate)
{
    ResearchDrumRenderResult result;
    if (totalFrames <= 0 || sampleRate <= 0) return result;
    // DaisySP's accepted analogue and metallic drum models use rand() for
    // their excitation noise. Reset it at this offline render boundary so a
    // generation recipe remains byte-for-byte reproducible.
    std::srand(0x4a324452);
    result.dry.fill(0.0f, static_cast<qsizetype>(totalFrames));
    result.roomSend.fill(
        0.0f,
        static_cast<qsizetype>(totalFrames));

    struct Pending {
        ResearchDrumRenderEvent event;
        const ResearchDrumPiece* piece = nullptr;
        int repeatIndex = 0;
    };
    std::vector<Pending> pending;
    pending.reserve(static_cast<std::size_t>(events.size()));
    QHash<QString, int> repeatCounts;
    for (const ResearchDrumRenderEvent& event : events) {
        const ResearchDrumPiece* piece =
            researchDrumPiece(kit, event.laneId);
        if (!piece) continue;
        Pending item;
        item.event = event;
        item.piece = piece;
        item.repeatIndex =
            event.repeatIndex > 0
            ? event.repeatIndex
            : repeatCounts.value(event.laneId, 0);
        repeatCounts.insert(
            event.laneId,
            repeatCounts.value(event.laneId, 0) + 1);
        pending.push_back(std::move(item));
    }
    std::stable_sort(
        pending.begin(),
        pending.end(),
        [](const Pending& left, const Pending& right) {
            return left.event.frame < right.event.frame;
        });

    std::vector<Voice> active;
    std::array<SubVoice, 32> synthVoices;
    std::size_t next = 0;
    for (qint64 frame = 0; frame < totalFrames; ++frame) {
        while (next < pending.size() &&
               pending[next].event.frame <= frame) {
            const Pending& item = pending[next];
            ResearchDrumRenderEvent event = item.event;
            event.repeatIndex = item.repeatIndex;
            const RealisedHit hit = realise(event, *item.piece);
            if (!item.piece->chokeGroup.isEmpty()) {
                const float chokeDecay = std::exp(
                    -1.0f /
                    std::max(
                        1.0f,
                        item.piece->chokeSeconds * sampleRate));
                for (Voice& voice : active) {
                    if (voice.chokeGroup ==
                        item.piece->chokeGroup) {
                        voice.chokeDecay = chokeDecay;
                    }
                }
            }
            const bool hasSecond =
                item.piece->secondSource !=
                    QStringLiteral("off") &&
                item.piece->blend > 0.0001f;
            const float firstGain =
                hasSecond
                ? std::sqrt(1.0f - item.piece->blend)
                : 1.0f;
            const float secondGain =
                hasSecond
                ? std::sqrt(item.piece->blend)
                : 0.0f;
            if (item.piece->source !=
                    QStringLiteral("jam2-native")) {
                Voice voice = makeVoice(
                    *item.piece,
                    hit,
                    item.piece->source,
                    firstGain,
                    totalFrames,
                    sampleRate);
                if (voice.endFrame > frame) {
                    active.push_back(std::move(voice));
                }
            }
            if (hasSecond &&
                item.piece->secondSource !=
                    QStringLiteral("jam2-native")) {
                Voice voice = makeVoice(
                    *item.piece,
                    hit,
                    item.piece->secondSource,
                    secondGain,
                    totalFrames,
                    sampleRate);
                if (voice.endFrame > frame) {
                    active.push_back(std::move(voice));
                }
            }
            if (item.piece->synthSource ==
                    QStringLiteral("sine-fundamental") &&
                item.piece->synthLevel > 0.0001f) {
                auto synth = std::find_if(
                    synthVoices.begin(),
                    synthVoices.end(),
                    [](const SubVoice& voice) {
                        return !voice.active;
                    });
                if (synth == synthVoices.end()) {
                    synth = std::min_element(
                        synthVoices.begin(),
                        synthVoices.end(),
                        [](const SubVoice& left,
                           const SubVoice& right) {
                            return left.startFrame <
                                right.startFrame;
                        });
                }
                synth->begin(hit, *item.piece, sampleRate);
            }
            ++next;
        }

        float dry = 0.0f;
        float send = 0.0f;
        for (Voice& voice : active) {
            const float sample = voice.gain * voice.process();
            dry += sample;
            send += voice.roomSend * sample;
        }
        float synth = 0.0f;
        int synthCount = 0;
        float synthSend = 0.0f;
        for (SubVoice& voice : synthVoices) {
            if (!voice.active) continue;
            const float sample = voice.process(frame);
            synth += voice.level * sample;
            synthSend +=
                voice.roomSend * voice.level * sample;
            ++synthCount;
        }
        if (synthCount > 0) {
            const float scale =
                0.62f /
                std::sqrt(static_cast<float>(synthCount));
            dry += scale * synth;
            send += scale * synthSend;
        }
        result.dry[static_cast<qsizetype>(frame)] = dry;
        result.roomSend[static_cast<qsizetype>(frame)] = send;
        active.erase(
            std::remove_if(
                active.begin(),
                active.end(),
                [frame](const Voice& voice) {
                    return frame >= voice.endFrame;
                }),
            active.end());
    }
    return result;
}

void applyResearchDrumBus(
    QVector<float>& audio,
    const QVector<float>& roomSend,
    const ResearchDrumBus& bus,
    int sampleRate)
{
    if (audio.isEmpty() || sampleRate <= 0) return;
    if (!roomSend.isEmpty() && bus.roomMix > 0.00001f) {
        const std::array<double, 3> ratios{0.73, 1.0, 1.37};
        std::array<QVector<float>, 3> delays;
        std::array<qsizetype, 3> positions{};
        std::array<double, 3> damped{};
        for (std::size_t index = 0; index < delays.size(); ++index) {
            const qsizetype length = std::max<qsizetype>(
                17,
                static_cast<qsizetype>(
                    std::llround(
                        0.001 * bus.roomSizeMs * ratios[index] *
                        sampleRate)));
            delays[index].fill(0.0f, length);
        }
        const double damping =
            std::clamp<double>(bus.roomDamping, 0.0, 0.98);
        const double feedback =
            0.28 + 0.34 * (1.0 - damping);
        for (qsizetype frame = 0; frame < audio.size(); ++frame) {
            double wet = 0.0;
            const float input =
                frame < roomSend.size()
                ? roomSend.at(frame)
                : 0.0f;
            for (std::size_t index = 0;
                 index < delays.size();
                 ++index) {
                float& cell = delays[index][positions[index]];
                damped[index] +=
                    (0.08 + 0.78 * (1.0 - damping)) *
                    (cell - damped[index]);
                wet += damped[index];
                cell = static_cast<float>(
                    input + feedback * damped[index]);
                positions[index] =
                    (positions[index] + 1) %
                    delays[index].size();
            }
            audio[frame] += static_cast<float>(
                bus.roomMix * wet / delays.size());
        }
    }

    const double coefficient =
        1.0 -
        std::exp(
            -2.0 * kPi * bus.lowpassHz / sampleRate);
    const double envelopeRelease = std::exp(
        -1.0 /
        (0.001 * bus.compressorReleaseMs * sampleRate));
    double filtered = 0.0;
    double envelope = 0.0;
    for (float& sample : audio) {
        const double value =
            std::tanh(bus.drive * sample);
        filtered += coefficient * (value - filtered);
        envelope = std::max(
            std::abs(filtered),
            envelope * envelopeRelease);
        double gain = 1.0;
        if (envelope > bus.compressorThreshold &&
            bus.compressorThreshold > 0.0f) {
            const double over =
                envelope / bus.compressorThreshold;
            gain = std::pow(
                over,
                1.0 / bus.compressorRatio - 1.0);
        }
        sample = static_cast<float>(gain * filtered);
    }
}

} // namespace jam2::practice
