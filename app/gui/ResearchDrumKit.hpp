#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>

namespace jam2::practice {

struct ResearchDrumVelocityBand {
    int minimum = 1;
    int maximum = 127;
};

struct ResearchDrumPiece {
    QString intendedIdentity;
    QString source;
    QString secondSource;
    float blend = 0.0f;
    float frequencyHz = 100.0f;
    float decay = 0.2f;
    float tone = 0.5f;
    float colour = 0.5f;
    float fmAmount = 0.0f;
    float level = 0.5f;
    float voiceDrive = 1.0f;
    float digitalSampleRateHz = 48000.0f;
    int digitalBitDepth = 24;
    float reconstructionLowpassHz = 20000.0f;
    float dynamicFilterAmount = 0.0f;
    float roomSend = 0.0f;
    QString transientType;
    float transientLevel = 0.0f;
    float transientTone = 0.5f;
    float transientDecaySeconds = 0.01f;
    QString textureType;
    float textureLevel = 0.0f;
    float textureTone = 0.5f;
    float textureDensity = 0.5f;
    float textureDecaySeconds = 0.1f;
    ResearchDrumVelocityBand ghost;
    ResearchDrumVelocityBand normal;
    ResearchDrumVelocityBand accent;
    float excitationCurve = 0.7f;
    float outputCurve = 1.25f;
    float brightnessAmount = 0.2f;
    float decayAmount = 0.12f;
    float driveAmount = 0.14f;
    QString chokeGroup;
    float chokeSeconds = 0.012f;
    QString synthSource;
    int synthMidiNote = 60;
    float synthLevel = 0.0f;
    float synthGateSeconds = 0.08f;
    float synthAttackSeconds = 0.001f;
    float synthDecaySeconds = 0.08f;
    float synthSustain = 0.01f;
    float synthReleaseSeconds = 0.08f;
    float synthNoiseMix = 0.0f;
    float synthFilterCutoffHz = 8000.0f;
};

struct ResearchDrumBus {
    float drive = 1.0f;
    float lowpassHz = 18000.0f;
    float compressorThreshold = 0.2f;
    float compressorRatio = 2.0f;
    float compressorReleaseMs = 60.0f;
    float roomMix = 0.0f;
    float roomSizeMs = 28.0f;
    float roomDamping = 0.6f;
};

struct ResearchDrumKit {
    QString profileId;
    QString id;
    QString name;
    QString researchFamily;
    int revision = 1;
    ResearchDrumBus bus;
    QHash<QString, ResearchDrumPiece> pieces;
};

struct ResearchDrumRenderEvent {
    qint64 frame = 0;
    QString laneId;
    QString articulation;
    int velocity = 90;
    int repeatIndex = 0;
    std::uint32_t seed = 0;
};

struct ResearchDrumRenderResult {
    QVector<float> dry;
    QVector<float> roomSend;
};

const ResearchDrumKit* researchDrumKitForProfile(
    const QString& profileId);
const ResearchDrumKit* researchDrumKitById(const QString& kitId);
const ResearchDrumPiece* researchDrumPiece(
    const ResearchDrumKit& kit,
    const QString& laneId);
bool researchDrumSourceSupportsLane(
    const QString& laneId,
    const QString& source);

double researchDrumTailSeconds(
    const ResearchDrumPiece& piece,
    const QString& articulation);
double researchDrumSample(
    const ResearchDrumPiece& piece,
    const QString& laneId,
    const QString& articulation,
    int velocity,
    std::uint32_t seed,
    qint64 age,
    int sampleRate);

ResearchDrumRenderResult renderResearchDrumVoices(
    const ResearchDrumKit& kit,
    const QVector<ResearchDrumRenderEvent>& events,
    qint64 totalFrames,
    int sampleRate);
void applyResearchDrumBus(
    QVector<float>& audio,
    const QVector<float>& roomSend,
    const ResearchDrumBus& bus,
    int sampleRate);

} // namespace jam2::practice
