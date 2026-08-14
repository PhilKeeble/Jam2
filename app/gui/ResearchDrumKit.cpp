#include "ResearchDrumKit.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace jam2::practice {
namespace {

constexpr double kPi = 3.14159265358979323846;

float number(
    const QJsonObject& object,
    const char* key,
    float fallback)
{
    const QJsonValue value =
        object.value(QString::fromLatin1(key));
    return value.isDouble()
        ? static_cast<float>(value.toDouble())
        : fallback;
}

QString text(
    const QJsonObject& object,
    const char* key,
    const QString& fallback = {})
{
    const QJsonValue value =
        object.value(QString::fromLatin1(key));
    return value.isString() ? value.toString() : fallback;
}

ResearchDrumVelocityBand velocityBand(
    const QJsonObject& object,
    const char* key,
    ResearchDrumVelocityBand fallback)
{
    const QJsonObject band =
        object.value(QString::fromLatin1(key)).toObject();
    return {
        static_cast<int>(number(
            band, "minimum", fallback.minimum)),
        static_cast<int>(number(
            band, "maximum", fallback.maximum)),
    };
}

ResearchDrumDetailResponse detailResponse(
    const QJsonObject& object)
{
    ResearchDrumDetailResponse response;
    response.velocityCurve = number(
        object, "velocityCurve", response.velocityCurve);
    response.ghostGain = number(
        object, "ghostGain", response.ghostGain);
    response.normalGain = number(
        object, "normalGain", response.normalGain);
    response.accentGain = number(
        object, "accentGain", response.accentGain);
    response.roomSend = number(
        object, "roomSend", response.roomSend);
    return response;
}

ResearchDrumPiece parsePiece(const QJsonObject& object)
{
    ResearchDrumPiece piece;
    piece.intendedIdentity =
        text(object, "intendedIdentity");
    piece.source = text(object, "source");
    piece.secondSource =
        text(object, "secondSource", QStringLiteral("off"));
    piece.blend = number(object, "blend", piece.blend);
    piece.frequencyHz =
        number(object, "frequencyHz", piece.frequencyHz);
    piece.decay = number(object, "decay", piece.decay);
    piece.tone = number(object, "tone", piece.tone);
    piece.colour = number(object, "colour", piece.colour);
    piece.fmAmount =
        number(object, "fmAmount", piece.fmAmount);
    piece.level = number(object, "level", piece.level);
    piece.onsetSofteningSeconds = number(
        object, "onsetSofteningSeconds", piece.onsetSofteningSeconds);
    piece.sourceLayerGain = number(
        object, "sourceLayerGain", piece.sourceLayerGain);
    piece.roomSend =
        number(object, "roomSend", piece.roomSend);
    const QJsonArray modalBands =
        object.value(QStringLiteral("modalBands")).toArray();
    piece.modalBands.reserve(modalBands.size());
    for (const QJsonValue& value : modalBands) {
        if (!value.isObject()) continue;
        const QJsonObject band = value.toObject();
        piece.modalBands.push_back({
            number(band, "frequencyHz", 0.0f),
            number(band, "detuneCents", 0.0f),
            number(band, "level", 0.0f),
            number(band, "decaySeconds", 0.0f),
            number(band, "attackSeconds", 0.001f),
            number(band, "delaySeconds", 0.0f),
            number(band, "highpassHz", 0.0f),
            number(band, "phaseCycles", -1.0f),
            detailResponse(band),
        });
    }
    const QJsonArray noiseBands =
        object.value(QStringLiteral("noiseBands")).toArray();
    piece.noiseBands.reserve(noiseBands.size());
    for (const QJsonValue& value : noiseBands) {
        if (!value.isObject()) continue;
        const QJsonObject band = value.toObject();
        piece.noiseBands.push_back({
            number(band, "frequencyHz", 4000.0f),
            number(band, "q", 1.0f),
            number(band, "level", 0.0f),
            number(band, "decaySeconds", 0.15f),
            number(band, "attackSeconds", 0.001f),
            number(band, "delaySeconds", 0.0f),
            number(band, "highpassHz", 0.0f),
            detailResponse(band),
        });
    }
    const QJsonObject colourStage =
        object.value(QStringLiteral("colourStage")).toObject();
    piece.voiceDrive =
        number(colourStage, "voiceDrive", piece.voiceDrive);
    piece.digitalSampleRateHz = number(
        colourStage,
        "sampleRateHz",
        piece.digitalSampleRateHz);
    piece.digitalBitDepth = static_cast<int>(number(
        colourStage,
        "bitDepth",
        static_cast<float>(piece.digitalBitDepth)));
    piece.reconstructionLowpassHz = number(
        colourStage,
        "reconstructionLowpassHz",
        piece.reconstructionLowpassHz);
    piece.dynamicFilterAmount = number(
        colourStage,
        "dynamicFilterAmount",
        piece.dynamicFilterAmount);

    const QJsonObject transient =
        object.value(QStringLiteral("transient")).toObject();
    piece.transientType =
        text(transient, "type", QStringLiteral("off"));
    piece.transientLevel =
        number(transient, "level", piece.transientLevel);
    piece.transientTone =
        number(transient, "tone", piece.transientTone);
    piece.transientDecaySeconds = number(
        transient,
        "decaySeconds",
        piece.transientDecaySeconds);

    const QJsonObject texture =
        object.value(QStringLiteral("texture")).toObject();
    piece.textureType =
        text(texture, "type", QStringLiteral("off"));
    piece.textureLevel =
        number(texture, "level", piece.textureLevel);
    piece.textureTone =
        number(texture, "tone", piece.textureTone);
    piece.textureDensity =
        number(texture, "density", piece.textureDensity);
    piece.textureDecaySeconds = number(
        texture,
        "decaySeconds",
        piece.textureDecaySeconds);

    const QJsonObject velocity =
        object.value(QStringLiteral("velocity")).toObject();
    piece.ghost = velocityBand(
        velocity, "ghost", {14, 42});
    piece.normal = velocityBand(
        velocity, "normal", {46, 100});
    piece.accent = velocityBand(
        velocity, "accent", {96, 127});
    piece.excitationCurve = number(
        velocity, "excitationCurve", piece.excitationCurve);
    piece.outputCurve =
        number(velocity, "outputCurve", piece.outputCurve);
    piece.brightnessAmount = number(
        velocity,
        "brightnessAmount",
        piece.brightnessAmount);
    piece.decayAmount =
        number(velocity, "decayAmount", piece.decayAmount);
    piece.driveAmount =
        number(velocity, "driveAmount", piece.driveAmount);

    const QJsonObject relationship =
        object.value(QStringLiteral("relationship")).toObject();
    piece.chokeGroup = text(relationship, "chokeGroup");
    piece.chokeSeconds =
        number(relationship, "chokeSeconds", piece.chokeSeconds);

    const QJsonObject synth =
        object.value(QStringLiteral("synthLayer")).toObject();
    piece.synthSource =
        text(synth, "source", QStringLiteral("off"));
    piece.synthMidiNote =
        static_cast<int>(number(
            synth, "midiNote", piece.synthMidiNote));
    piece.synthLevel =
        number(synth, "level", piece.synthLevel);
    piece.synthGateSeconds = number(
        synth, "gateSeconds", piece.synthGateSeconds);
    piece.synthAttackSeconds = number(
        synth, "attackSeconds", piece.synthAttackSeconds);
    piece.synthDecaySeconds = number(
        synth, "decaySeconds", piece.synthDecaySeconds);
    piece.synthSustain =
        number(synth, "sustain", piece.synthSustain);
    piece.synthReleaseSeconds = number(
        synth, "releaseSeconds", piece.synthReleaseSeconds);
    piece.synthNoiseMix =
        number(synth, "noiseMix", piece.synthNoiseMix);
    piece.synthFilterCutoffHz = number(
        synth,
        "filterCutoffHz",
        piece.synthFilterCutoffHz);
    return piece;
}

QHash<QString, ResearchDrumKit> loadKits()
{
    QFile file(QStringLiteral(
        ":/jam2/drums/researched-kits.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return {};
    const QJsonObject root = document.object();
    const int revision =
        root.value(QStringLiteral("revision")).toInt(1);
    QHash<QString, ResearchDrumKit> kits;
    const auto loadCollection =
        [&kits, revision](const QJsonObject& collection, bool profiles) {
      for (auto entry = collection.begin();
           entry != collection.end(); ++entry) {
        const QJsonObject object = entry.value().toObject();
        ResearchDrumKit kit;
        kit.profileId = profiles ? entry.key() : QString{};
        kit.baseKitId = text(object, "base_kit_id", entry.key());
        kit.treatmentId = text(object, "treatment_id");
        const QString candidateId =
            object.value(QStringLiteral("kit_id")).toString();
        kit.id = QStringLiteral("%1:%2:r%3")
            .arg(profiles ? kit.profileId : QStringLiteral("base"), candidateId)
            .arg(revision);
        kit.name =
            object.value(QStringLiteral("kit_name")).toString();
        kit.researchFamily =
            object.value(
                QStringLiteral("research_family")).toString();
        kit.revision = revision;
        const QJsonObject bus =
            object.value(QStringLiteral("bus")).toObject();
        kit.bus.drive =
            number(bus, "drive", kit.bus.drive);
        kit.bus.lowpassHz =
            number(bus, "lowpassHz", kit.bus.lowpassHz);
        kit.bus.compressorThreshold = number(
            bus,
            "compressorThreshold",
            kit.bus.compressorThreshold);
        kit.bus.compressorRatio = number(
            bus,
            "compressorRatio",
            kit.bus.compressorRatio);
        kit.bus.compressorReleaseMs = number(
            bus,
            "compressorReleaseMs",
            kit.bus.compressorReleaseMs);
        kit.bus.roomMix =
            number(bus, "roomMix", kit.bus.roomMix);
        kit.bus.roomSizeMs =
            number(bus, "roomSizeMs", kit.bus.roomSizeMs);
        kit.bus.roomDamping = number(
            bus, "roomDamping", kit.bus.roomDamping);
        const QJsonObject pieces =
            object.value(QStringLiteral("pieces")).toObject();
        for (auto piece = pieces.begin();
             piece != pieces.end();
             ++piece) {
            kit.pieces.insert(
                piece.key(),
                parsePiece(piece.value().toObject()));
        }
        if (!candidateId.isEmpty() && kit.pieces.size() == 10 &&
            (kit.baseKitId == QStringLiteral("acoustic") ||
             kit.baseKitId == QStringLiteral("electronic"))) {
            kits.insert(
                (profiles ? QStringLiteral("profile:") : QStringLiteral("base:")) +
                    entry.key(),
                std::move(kit));
        }
      }
    };
    loadCollection(
        root.value(QStringLiteral("base_kits")).toObject(), false);
    loadCollection(
        root.value(QStringLiteral("profiles")).toObject(), true);
    return kits;
}

const QHash<QString, ResearchDrumKit>& kits()
{
    static const QHash<QString, ResearchDrumKit> value =
        loadKits();
    return value;
}

double noiseAt(std::uint32_t seed, qint64 index)
{
    std::uint32_t value =
        seed ^ static_cast<std::uint32_t>(
            index * 747796405ULL);
    value ^= value >> 16;
    value *= 2246822519U;
    value ^= value >> 13;
    return static_cast<double>(value) /
            2147483648.0 -
        1.0;
}

double highNoise(std::uint32_t seed, qint64 age)
{
    return noiseAt(seed, age) -
        0.72 * noiseAt(seed, qMax<qint64>(0, age - 1));
}

double envelope(double seconds, double duration, double curve = 1.0)
{
    if (duration <= 0.0) return 0.0;
    return std::exp(
        -std::max(0.2, curve) * 6.907755 *
        seconds / duration);
}

double inharmonic(
    double frequency,
    double seconds,
    std::initializer_list<double> ratios,
    double rolloff)
{
    double value = 0.0;
    double gain = 1.0;
    for (double ratio : ratios) {
        value += gain *
            std::sin(
                2.0 * kPi * frequency * ratio * seconds);
        gain *= rolloff;
    }
    return value;
}

double sourceSample(
    const QString& source,
    const QString& articulation,
    const ResearchDrumPiece& piece,
    double excitation,
    double brightness,
    double decayScale,
    std::uint32_t seed,
    qint64 age,
    int sampleRate)
{
    if (source.isEmpty() || source == QStringLiteral("off")) {
        return 0.0;
    }
    const double t = static_cast<double>(age) / sampleRate;
    const double frequency =
        std::clamp<double>(
            piece.frequencyHz * (0.96 + 0.08 * excitation) *
                std::pow(2.0, brightness * 0.16),
            25.0,
            sampleRate * 0.42);
    const double decay =
        std::max(0.015, static_cast<double>(piece.decay) *
            decayScale);
    const double noise = noiseAt(seed, age);
    const double brightNoise = highNoise(seed, age);

    if (source.contains(QStringLiteral("kick"))) {
        const double sweep =
            frequency *
            (1.0 + (0.55 + 2.2 * piece.fmAmount) *
                std::exp(-t * 34.0));
        const double body =
            std::sin(2.0 * kPi * sweep * t) *
            envelope(t, 0.12 + 1.25 * decay, 0.72);
        const double sub =
            std::sin(2.0 * kPi * frequency * 0.72 * t) *
            envelope(t, 0.18 + 1.60 * decay, 0.62);
        const double click =
            (0.18 + 0.30 * piece.tone) * brightNoise *
            envelope(t, 0.004 + 0.014 * piece.colour);
        return 0.72 * body + 0.34 * sub + click;
    }
    if (source.contains(QStringLiteral("snare"))) {
        const double shell =
            (0.70 * std::sin(2.0 * kPi * frequency * t) +
             0.18 * std::sin(
                 2.0 * kPi * frequency * 1.79 * t) +
             0.08 * std::sin(
                 2.0 * kPi * frequency * 2.34 * t)) *
            envelope(t, 0.045 + 0.42 * decay, 1.0);
        const double wire =
            (0.66 * brightNoise + 0.34 * noise) *
            envelope(t, 0.035 + 0.62 * decay, 0.9);
        const double impact =
            brightNoise * envelope(t, 0.006, 1.2);
        const bool syntheticLead =
            source.startsWith(QStringLiteral("daisy-"));
        return (syntheticLead ? 0.40 : 0.60) * shell +
            (syntheticLead ? 0.86 : 0.58) *
                piece.colour * wire +
            0.38 * impact;
    }
    if (source.contains(QStringLiteral("shell-tom"))) {
        const double pitchEnvelope =
            1.0 + (0.018 + 0.085 * piece.fmAmount) *
                std::exp(-t * 42.0);
        const double shell =
            (0.72 * std::sin(
                 2.0 * kPi * frequency *
                     pitchEnvelope * t) +
             0.10 * std::sin(
                 2.0 * kPi * frequency * 1.031 * t) +
             (0.12 + 0.16 * piece.tone) *
                 std::sin(
                     2.0 * kPi * frequency * 1.593 * t) +
             0.055 * std::sin(
                 2.0 * kPi * frequency * 2.136 * t)) *
            envelope(t, 0.075 + 0.82 * decay, 0.92);
        const double head =
            (0.58 * noise + 0.24 * brightNoise) *
            envelope(t, 0.006 + 0.024 * piece.tone);
        return shell + 0.22 * head;
    }
    if (source.contains(QStringLiteral("cross-stick")) ||
        source.contains(QStringLiteral("wood-block"))) {
        const double wooden =
            inharmonic(
                frequency,
                t,
                {1.0, 2.37, 3.91},
                0.48) *
            envelope(t, 0.007 + 0.075 * decay, 1.2);
        return 0.62 * wooden +
            0.30 * brightNoise * envelope(t, 0.007);
    }
    if (source.contains(QStringLiteral("hand-clap"))) {
        const auto burst = [t](double start, double width) {
            const double local = t - start;
            return local >= 0.0 && local < width
                ? std::exp(-4.2 * local / width)
                : 0.0;
        };
        const double bursts = std::max({
            burst(0.0, 0.006),
            0.86 * burst(0.010, 0.006),
            0.72 * burst(0.019, 0.006),
        });
        return (0.78 * brightNoise + 0.22 * noise) *
            (bursts +
             0.24 * envelope(t, 0.05 + 0.62 * decay));
    }
    if (source.contains(QStringLiteral("crash-cymbal"))) {
        const double metal =
            inharmonic(
                frequency,
                t,
                {0.61, 0.93, 1.28, 1.67, 2.19, 2.83, 3.71},
                0.69) *
            envelope(t, 0.22 + 1.55 * decay);
        const double wash =
            (0.64 * brightNoise + 0.22 * noise) *
            envelope(t, 1.05 + 4.0 * decay, 0.95);
        return 0.13 * metal + 0.55 * wash;
    }
    if (source.contains(QStringLiteral("ride-cymbal"))) {
        const bool bell =
            articulation.contains(QStringLiteral("bell"));
        const bool edge =
            articulation.contains(QStringLiteral("edge"));
        const double strike =
            bell ? 1.22 : edge ? 0.82 : 1.0;
        const double ringGain =
            bell ? 0.18 : edge ? 0.055 : 0.095;
        const double metal =
            inharmonic(
                frequency * strike,
                t,
                {0.92, 1.37, 1.89, 2.62, 3.54},
                0.48) *
            envelope(
                t,
                (bell ? 0.52 : edge ? 0.18 : 0.44) +
                    0.78 * decay);
        const double wash =
            (0.22 * noise + 0.12 * brightNoise) *
            envelope(
                t,
                (bell ? 0.82 : edge ? 0.30 : 0.65) +
                    1.45 * decay);
        return ringGain * metal + wash;
    }
    if (source.contains(QStringLiteral("ring-metal")) ||
        source.contains(QStringLiteral("metal"))) {
        const double metal =
            inharmonic(
                frequency,
                t,
                {1.0, 1.342, 1.811, 2.438, 3.167, 4.109},
                0.66);
        return (0.34 * metal + 0.54 * brightNoise) *
            envelope(t, 0.025 + 0.82 * decay, 1.15);
    }
    // A safe skin/noise fallback for any future dedicated source.
    return (0.58 *
                std::sin(2.0 * kPi * frequency * t) +
            0.24 * noise) *
        envelope(t, 0.08 + 0.72 * decay);
}

double transientSample(
    const ResearchDrumPiece& piece,
    std::uint32_t seed,
    qint64 age,
    int sampleRate)
{
    if (piece.transientType.isEmpty() ||
        piece.transientType == QStringLiteral("off") ||
        piece.transientLevel <= 0.0f) {
        return 0.0;
    }
    const double t = static_cast<double>(age) / sampleRate;
    const double duration =
        std::max(0.002, static_cast<double>(
            piece.transientDecaySeconds));
    const double noise = highNoise(seed ^ 0x8da6b343U, age);
    double frequency =
        1100.0 + 5200.0 * piece.transientTone;
    double tonal = 0.0;
    double noiseMix = 0.75;
    if (piece.transientType.contains(QStringLiteral("beater"))) {
        frequency =
            piece.transientType.startsWith(
                QStringLiteral("soft"))
            ? 110.0 + 820.0 * piece.transientTone
            : 480.0 + 2600.0 * piece.transientTone;
        noiseMix = 0.42;
    } else if (
        piece.transientType == QStringLiteral("rim")) {
        frequency =
            650.0 + 3400.0 * piece.transientTone;
        noiseMix = 0.28;
    } else if (
        piece.transientType == QStringLiteral("head-strike")) {
        frequency =
            280.0 + 1200.0 * piece.transientTone;
        noiseMix = 0.68;
    }
    tonal = std::sin(2.0 * kPi * frequency * t);
    return piece.transientLevel *
        ((1.0 - noiseMix) * tonal + noiseMix * noise) *
        envelope(t, duration, 1.15);
}

double textureSample(
    const ResearchDrumPiece& piece,
    std::uint32_t seed,
    qint64 age,
    int sampleRate)
{
    if (piece.textureType.isEmpty() ||
        piece.textureType == QStringLiteral("off") ||
        piece.textureLevel <= 0.0f) {
        return 0.0;
    }
    const double t = static_cast<double>(age) / sampleRate;
    const double duration =
        std::max(0.008, static_cast<double>(
            piece.textureDecaySeconds));
    const double noise =
        highNoise(seed ^ 0xc2b2ae35U, age);
    double value = noise;
    if (piece.textureType == QStringLiteral("wire")) {
        value =
            0.72 * noise -
            0.30 * highNoise(
                seed ^ 0xc2b2ae35U,
                qMax<qint64>(0, age - 53));
    } else if (
        piece.textureType == QStringLiteral("dust")) {
        const std::uint32_t valueHash =
            static_cast<std::uint32_t>(
                seed + age * 2654435761ULL);
        value =
            (valueHash & 255U) <
                    static_cast<std::uint32_t>(
                        8 + 96 * piece.textureDensity)
                ? noise
                : 0.0;
    } else if (
        piece.textureType ==
        QStringLiteral("metal-wash")) {
        value =
            0.62 * noise +
            0.38 *
                std::sin(
                    2.0 * kPi *
                    (1300.0 +
                     4600.0 * piece.textureTone) *
                    t);
    }
    return piece.textureLevel * value *
        envelope(t, duration);
}

QString pieceKeyForLane(const QString& laneId)
{
    QString key = laneId;
    key.replace(QLatin1Char('_'), QLatin1Char('-'));
    return key;
}

} // namespace

const ResearchDrumKit* researchDrumKitForProfile(
    const QString& profileId)
{
    const auto found = kits().constFind(
        QStringLiteral("profile:") + profileId);
    return found == kits().cend() ? nullptr : &found.value();
}

const ResearchDrumKit* researchDrumKitForBase(
    const QString& baseKitId)
{
    const auto found = kits().constFind(
        QStringLiteral("base:") + baseKitId);
    return found == kits().cend() ? nullptr : &found.value();
}

const ResearchDrumKit* researchDrumKitById(
    const QString& kitId)
{
    for (auto kit = kits().cbegin(); kit != kits().cend(); ++kit) {
        if (kit.value().id == kitId) return &kit.value();
    }
    return nullptr;
}

const ResearchDrumPiece* researchDrumPiece(
    const ResearchDrumKit& kit,
    const QString& laneId)
{
    const auto found =
        kit.pieces.constFind(pieceKeyForLane(laneId));
    return found == kit.pieces.cend()
        ? nullptr
        : &found.value();
}

bool researchDrumSourceSupportsLane(
    const QString& laneId,
    const QString& source)
{
    if (source.isEmpty() || source == QStringLiteral("off")) {
        return false;
    }
    if (laneId == QStringLiteral("kick")) {
        return source == QStringLiteral("daisy-analog-kick") ||
            source == QStringLiteral("daisy-synthetic-kick");
    }
    if (laneId == QStringLiteral("snare")) {
        return source == QStringLiteral("jam2-shell-snare") ||
            source == QStringLiteral("daisy-analog-snare") ||
            source == QStringLiteral("daisy-synthetic-snare") ||
            source == QStringLiteral("jam2-hand-clap");
    }
    if (laneId == QStringLiteral("closed_hat") ||
        laneId == QStringLiteral("open_hat")) {
        return source == QStringLiteral("daisy-metal") ||
            source == QStringLiteral("daisy-ring-metal");
    }
    if (laneId.endsWith(QStringLiteral("_tom"))) {
        return source == QStringLiteral("jam2-shell-tom") ||
            source == QStringLiteral("daisy-synthetic-kick");
    }
    if (laneId == QStringLiteral("crash")) {
        return source == QStringLiteral("jam2-crash-cymbal");
    }
    if (laneId == QStringLiteral("ride")) {
        return source == QStringLiteral("jam2-ride-cymbal") ||
            source == QStringLiteral("jam2-wood-block");
    }
    if (laneId == QStringLiteral("cross_stick")) {
        return source == QStringLiteral("jam2-cross-stick") ||
            source == QStringLiteral("jam2-wood-block");
    }
    return false;
}

double researchDrumTailSeconds(
    const ResearchDrumPiece& piece,
    const QString& articulation)
{
    double tail = 0.12 + 1.25 * piece.decay;
    if (piece.source.contains(QStringLiteral("kick"))) {
        tail = 0.22 + 2.2 * piece.decay;
    } else if (
        piece.source.contains(QStringLiteral("crash-cymbal"))) {
        tail = 0.85 + 4.4 * piece.decay;
    } else if (
        piece.source.contains(QStringLiteral("ride-cymbal"))) {
        tail =
            articulation.contains(QStringLiteral("edge"))
            ? 0.38 + 1.0 * piece.decay
            : articulation.contains(QStringLiteral("bell"))
                ? 0.95 + 3.2 * piece.decay
                : 0.68 + 2.1 * piece.decay;
    } else if (
        piece.source.contains(QStringLiteral("ring-metal")) ||
        piece.source.contains(QStringLiteral("metal"))) {
        tail = 0.08 + 1.8 * piece.decay;
    } else if (
        piece.source.contains(QStringLiteral("cross-stick")) ||
        piece.source.contains(QStringLiteral("wood-block"))) {
        tail = 0.06 + 0.62 * piece.decay;
    } else if (
        piece.source.contains(QStringLiteral("shell-tom"))) {
        tail = 0.16 + 1.10 * piece.decay;
    }
    tail = std::max(
        tail,
        0.04 + 1.15 * piece.transientDecaySeconds);
    tail = std::max(
        tail,
        0.04 + 1.15 * piece.textureDecaySeconds);
    if (piece.synthSource != QStringLiteral("off")) {
        tail = std::max(
            tail,
            static_cast<double>(
                piece.synthDecaySeconds +
                piece.synthReleaseSeconds));
    }
    return std::clamp(tail, 0.04, 6.0);
}

double researchDrumSample(
    const ResearchDrumPiece& piece,
    const QString& articulation,
    int velocity,
    std::uint32_t seed,
    qint64 age,
    int sampleRate)
{
    const double normalized =
        std::clamp(velocity / 127.0, 0.001, 1.0);
    const double excitation =
        std::pow(normalized, piece.excitationCurve);
    const double output =
        std::pow(normalized, piece.outputCurve);
    const double brightness =
        (normalized - 0.5) * piece.brightnessAmount;
    const double decayScale =
        std::clamp(
            1.0 + (normalized - 0.5) *
                piece.decayAmount,
            0.65,
            1.35);
    const double drive =
        std::max(
            0.5,
            static_cast<double>(piece.voiceDrive) *
                (1.0 +
                 (normalized - 0.5) *
                     piece.driveAmount));
    const double first = sourceSample(
        piece.source,
        articulation,
        piece,
        excitation,
        brightness,
        decayScale,
        seed,
        age,
        sampleRate);
    double base = first;
    if (!piece.secondSource.isEmpty() &&
        piece.secondSource != QStringLiteral("off") &&
        piece.blend > 0.0f) {
        const double second = sourceSample(
            piece.secondSource,
            articulation,
            piece,
            excitation,
            brightness,
            decayScale,
            seed ^ 0x9e3779b9U,
            age,
            sampleRate);
        const double angle =
            std::clamp<double>(piece.blend, 0.0, 1.0) *
            kPi * 0.5;
        base =
            std::cos(angle) * first +
            std::sin(angle) * second;
    }
    double synth = 0.0;
    if (piece.synthSource != QStringLiteral("off") &&
        piece.synthLevel > 0.0f) {
        const double t = static_cast<double>(age) / sampleRate;
        const double frequency =
            440.0 * std::pow(
                2.0,
                (piece.synthMidiNote - 69) / 12.0);
        const double attack =
            std::clamp(
                t / std::max(
                    0.0001,
                    static_cast<double>(
                        piece.synthAttackSeconds)),
                0.0,
                1.0);
        synth =
            piece.synthLevel * attack *
            std::sin(2.0 * kPi * frequency * t) *
            envelope(
                t,
                piece.synthDecaySeconds +
                    piece.synthReleaseSeconds,
                0.75);
    }
    const double combined =
        base +
        transientSample(piece, seed, age, sampleRate) +
        textureSample(piece, seed, age, sampleRate) +
        synth;
    return piece.level * output *
        std::tanh(drive * combined) /
        std::max(1.0, drive * 0.72);
}

} // namespace jam2::practice
