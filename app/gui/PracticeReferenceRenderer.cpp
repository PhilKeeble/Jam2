#include "PracticeReferenceRenderer.hpp"

#include "GeneratedDrumMixPolicy.hpp"
#include "MusicTheory.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "ResearchDrumKit.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace jam2::practice {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaximumSeconds = 300.0;
constexpr int kBlockFrames = 32768;
constexpr int kTicksPerBeat = 12;

double deterministicUnit(std::uint32_t seed, std::uint32_t salt);

struct DrumEvent {
    enum class Instrument {
        Kick, Snare, ClosedHat, OpenHat, HighTom, MidTom, FloorTom,
        Crash, Ride,
        CrossStick, Shaker, HandPercussion
    };
    qint64 frame = 0;
    Instrument instrument = Instrument::Kick;
    double level = 0.7;
    std::uint32_t noiseSeed = 1;
    int kit = 0;
    int velocity = 96;
    QString articulation;
    QString laneId;
    ResearchDrumPiece researchedPiece;
    bool useResearchedPiece = false;
    qint64 chokeAge = std::numeric_limits<qint64>::max();
};

enum class Wave { Sine, Triangle, Saw, Pulse, Fm };

struct SynthPatch {
    Wave wave = Wave::Sine;
    int layers = 1;
    double detuneCents = 0.0;
    double secondHarmonic = 0.10;
    double drive = 1.0;
    double filter = 0.35;
    double delayMix = 0.0;
    double transientAmount = 0.0;
    double decayRate = 0.0;
    double cabinet = 0.0;
};

SynthPatch synthPatch(const SongSection& section, const QString& role)
{
    const bool melody = role == QStringLiteral("melody");
    const bool bass = role == QStringLiteral("bass");
    const QString id = role == QStringLiteral("melody")
        ? section.generatedRecipe.melodyPatchId
        : role == QStringLiteral("bass")
        ? section.generatedRecipe.bassPatchId
        : role == QStringLiteral("support")
        ? section.generatedRecipe.supportPatchId
        : section.generatedRecipe.chordPatchId;
    SynthPatch patch;
    if (id.contains(QStringLiteral("supersaw")) || id.contains(QStringLiteral("bright-layer")))
        patch = {Wave::Saw, 3, 8.0, 0.05, 1.35, 0.52, 0.08};
    else if (id.contains(QStringLiteral("clav")) || id.contains(QStringLiteral("pluck")))
        patch = {Wave::Pulse, 2, 3.0, 0.15, 1.20, 0.42, 0.03};
    else if (id.contains(QStringLiteral("organ")))
        patch = {Wave::Fm, 3, 1.5, 0.24, 1.12, 0.32, 0.02};
    else if (id.contains(QStringLiteral("ep")) || id.contains(QStringLiteral("keys")))
        patch = {Wave::Fm, 2, 2.0, 0.18, 1.08, 0.26, 0.05};
    else if (id.contains(QStringLiteral("pad")))
        patch = {Wave::Triangle, 3, 6.0, 0.10, 1.02, 0.18, 0.12};
    else if (id.contains(QStringLiteral("bell")))
        patch = {Wave::Fm, 2, 0.8, 0.30, 1.10, 0.40, 0.10};
    else if (id.contains(QStringLiteral("square")) || id.contains(QStringLiteral("mono")))
        patch = {Wave::Pulse, 2, 2.0, 0.12, 1.25, 0.48, 0.04};
    else
        patch = {Wave::Triangle, 2, 3.0, 0.12, 1.08, 0.34, 0.04};
    const GenerationRecipe& recipe = section.generatedRecipe;
    if (recipe.variationBrightness > 0)
        patch.filter = qMin(0.72, patch.filter + 0.16);
    else if (recipe.variationBrightness < 0)
        patch.filter *= 0.68;
    if (recipe.variationSpace > 0) {
        patch.detuneCents += 4.0;
        patch.delayMix += 0.10;
    } else if (recipe.variationSpace < 0) {
        patch.delayMix *= 0.5;
    }
    if (recipe.variationArticulation < 0) patch.decayRate *= 1.35;
    else if (recipe.variationArticulation > 0) patch.decayRate *= 0.78;
    if (melody) {
        // Melody references are deliberately warm and playable: never expose a
        // raw saw or pulse as the main oscillator.
        patch.wave = id.contains(QStringLiteral("bell")) ? Wave::Sine : Wave::Triangle;
        patch.layers = qMin(2, patch.layers);
        patch.detuneCents = qMin(2.5, patch.detuneCents);
        patch.secondHarmonic = qMin(0.12, patch.secondHarmonic);
        patch.drive = qMin(1.08, patch.drive);
        patch.filter = qMin(0.46, patch.filter);
    }
    if (bass) {
        patch.wave = id.contains(QStringLiteral("808")) ? Wave::Sine : Wave::Triangle;
        patch.layers = 1;
        patch.detuneCents = 0.0;
        patch.secondHarmonic = id.contains(QStringLiteral("driven")) ||
                id.contains(QStringLiteral("split")) ? 0.24 : 0.10;
        patch.filter = qMin(0.30, patch.filter);
        patch.delayMix = 0.0;
    }
    for (const SynthVoiceRecipe& voice : section.generatedRecipe.synthVoices) {
        if (voice.roleId != role) continue;
        if (voice.oscillator.contains(QStringLiteral("sine"))) patch.wave = Wave::Sine;
        else if (voice.oscillator.contains(QStringLiteral("pulse"))) patch.wave = Wave::Pulse;
        else if (voice.oscillator.contains(QStringLiteral("saw"))) patch.wave = Wave::Saw;
        else if (voice.engine == QStringLiteral("fm-additive")) patch.wave = Wave::Fm;
        else patch.wave = Wave::Triangle;
        patch.detuneCents = voice.detuneCents;
        patch.drive = 1.0 + voice.drive * 2.2;
        patch.filter = std::clamp((voice.cutoffHz - 550.0) / 6200.0, 0.05, 0.90);
        if (voice.effects.contains(QStringLiteral("tempo-delay"))) {
            patch.delayMix = qMax(patch.delayMix, 0.12);
        }
        if (voice.engine == QStringLiteral("plucked-excitation")) {
            patch.transientAmount = 0.20;
            patch.decayRate = 0.9;
        }
        if (voice.effects.contains(QStringLiteral("cabinet-filter"))) {
            patch.cabinet = 0.72;
            patch.filter = qMin(patch.filter, 0.48);
            patch.transientAmount = qMax(patch.transientAmount, 0.12);
            patch.decayRate = qMax(patch.decayRate, 1.4);
        }
        break;
    }
    return patch;
}

double framesPerMusicalBeat(const SongSection& section, const ReferenceRenderSettings& settings)
{
    const int pulseUnits = section.generatedRecipe.isValid()
        ? section.generatedRecipe.tempoPulseUnits : settings.tempoPulseUnits;
    return settings.sampleRate * 60.0 / settings.bpm /
        qMax(1, pulseUnits);
}

const LaneTimingRecipe* laneTimingRecipe(const SongSection& section, const QString& laneId)
{
    for (const LaneTimingRecipe& policy : section.generatedRecipe.laneTiming) {
        if (policy.laneId == laneId) return &policy;
    }
    return nullptr;
}

double automationValue(
    const SongSection& section,
    const QString& target,
    qint64 tick,
    double fallback)
{
    const qint64 cycleTicks = qMax<qint64>(1, section.beats * kTicksPerBeat);
    const int localTick = static_cast<int>(tick % cycleTicks);
    for (const AutomationRecipeEvent& event : section.generatedRecipe.automationEvents) {
        if (event.target != target || localTick < event.startTick ||
            localTick > event.endTick) continue;
        double amount = static_cast<double>(localTick - event.startTick) /
            qMax(1, event.endTick - event.startTick);
        amount = std::clamp(amount, 0.0, 1.0);
        if (event.curve == QStringLiteral("ease-in-out")) {
            amount = amount * amount * (3.0 - 2.0 * amount);
        }
        return event.startValue + (event.endValue - event.startValue) * amount;
    }
    return fallback;
}

double filterCoefficientHz(double cutoffHz, int sampleRate)
{
    return 1.0 - std::exp(
        -2.0 * kPi * std::clamp(cutoffHz, 20.0, 24000.0) / qMax(1, sampleRate));
}

double filterCoefficient(const SynthPatch& patch, int sampleRate)
{
    const double brightness = std::clamp(patch.filter, 0.05, 0.90);
    const double cutoffHz = 550.0 + brightness * 6200.0;
    return 1.0 - std::exp(-2.0 * kPi * cutoffHz / qMax(1, sampleRate));
}

double oscillator(Wave wave, double phase)
{
    const double wrapped = phase / (2.0 * kPi) - std::floor(phase / (2.0 * kPi));
    switch (wave) {
    case Wave::Sine: return std::sin(phase);
    case Wave::Triangle: return 1.0 - 4.0 * std::abs(wrapped - 0.5);
    case Wave::Saw: return 2.0 * wrapped - 1.0;
    case Wave::Pulse: return wrapped < 0.42 ? 1.0 : -1.0;
    case Wave::Fm: return std::sin(phase + 1.4 * std::sin(phase * 2.0));
    }
    return 0.0;
}

double patchTone(
    const SynthPatch& patch,
    double frequency,
    double seconds,
    double noteAgeSeconds = -1.0)
{
    double result = 0.0;
    for (int layer = 0; layer < patch.layers; ++layer) {
        const double centered = layer - (patch.layers - 1) * 0.5;
        const double ratio = std::pow(2.0, centered * patch.detuneCents / 1200.0);
        const double phase = 2.0 * kPi * frequency * ratio * seconds;
        result += oscillator(patch.wave, phase) + patch.secondHarmonic * std::sin(phase * 2.0);
    }
    result /= patch.layers;
    if (noteAgeSeconds >= 0.0) {
        const double pick = std::exp(-noteAgeSeconds * 70.0) *
            std::sin(2.0 * kPi * frequency * 3.1 * noteAgeSeconds);
        result += patch.transientAmount * pick;
        if (patch.decayRate > 0.0) result *= std::exp(-noteAgeSeconds * patch.decayRate);
    }
    double driven = std::tanh(patch.drive * result);
    if (patch.cabinet > 0.0) {
        driven = (1.0 - patch.cabinet * 0.35) * driven +
            patch.cabinet * 0.18 * std::tanh(driven * 2.4);
    }
    return driven;
}

void writeWavHeader(QIODevice& device, int sampleRate, qint64 frames)
{
    QDataStream stream(&device);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = static_cast<quint32>(frames * 2);
    stream.writeRawData("RIFF", 4);
    stream << static_cast<quint32>(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << static_cast<quint32>(16) << static_cast<quint16>(1) << static_cast<quint16>(1)
           << static_cast<quint32>(sampleRate) << static_cast<quint32>(sampleRate * 2)
           << static_cast<quint16>(2) << static_cast<quint16>(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
}

QString fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) hash.addData(file.read(256 * 1024));
    return QString::fromLatin1(hash.result().toHex());
}

bool writeBlock(QIODevice& file, const QVector<float>& values)
{
    QByteArray pcm(values.size() * 2, Qt::Uninitialized);
    for (int index = 0; index < values.size(); ++index) {
        const float limited = std::clamp(values[index], -0.98f, 0.98f);
        const qint16 sample = static_cast<qint16>(std::lround(limited * 32767.0f));
        qToLittleEndian<qint16>(sample, reinterpret_cast<uchar*>(pcm.data() + index * 2));
    }
    return file.write(pcm) == pcm.size();
}

struct DrumMakeupMetrics {
    float preMakeupPeak = 0.0f;
    qint64 limitedSamples = 0;
};

DrumMakeupMetrics applyGeneratedDrumMakeup(
    QVector<float>& rendered,
    bool generated)
{
    DrumMakeupMetrics metrics;
    for (float sample : rendered) {
        metrics.preMakeupPeak =
            qMax(metrics.preMakeupPeak, std::abs(sample));
    }
    if (!generated) {
        return metrics;
    }

    constexpr double knee =
        jam2::practice::kGeneratedDrumSoftLimitThreshold;
    constexpr double ceiling =
        jam2::practice::kGeneratedDrumSoftLimitCeiling;
    constexpr double kneeRange = ceiling - knee;
    for (float& sample : rendered) {
        const double driven =
            sample *
            jam2::practice::kGeneratedDrumStemMakeupLinear;
        const double magnitude = std::abs(driven);
        if (magnitude <= knee) {
            sample = static_cast<float>(driven);
            continue;
        }
        ++metrics.limitedSamples;
        const double aboveKnee = magnitude - knee;
        // A rational soft knee keeps a positive slope even for unusually
        // hot native-kit overlaps. Unlike an asymptotic tanh evaluated in
        // float precision, it does not turn those transients into a short
        // run of identical samples at the ceiling.
        const double limited =
            knee +
            aboveKnee /
                (1.0 + aboveKnee / kneeRange);
        sample = static_cast<float>(
            std::copysign(limited, driven));
    }
    return metrics;
}

QVector<int> voicedNotes(const ParsedChord& chord, ChordVoicing voicing, double previousCenter)
{
    QVector<int> notes;
    QVector<int> essential;
    const auto addEssential = [&](int interval) {
        if (essential.size() < 4 && chord.intervals.contains(interval) &&
            !essential.contains(interval)) {
            essential.push_back(interval);
        }
    };
    const bool hasSeventh =
        chord.intervals.contains(10) || chord.intervals.contains(11);
    if (voicing == ChordVoicing::VoiceLed && hasSeventh) {
        addEssential(3);
        addEssential(4);
        addEssential(10);
        addEssential(11);
        for (int interval : chord.intervals) {
            if (interval > 11) addEssential(interval);
        }
        addEssential(6);
        addEssential(7);
    } else {
        for (int interval : chord.intervals) {
            if (essential.size() >= 4) break;
            if (interval == 0 || interval == 3 || interval == 4 || interval == 10 ||
                interval == 11 || interval == chord.intervals.back()) {
                addEssential(interval);
            }
        }
        for (int interval : chord.intervals) {
            if (essential.size() >= 4) break;
            if (interval != 7) addEssential(interval);
        }
        if (essential.size() < 3) addEssential(7);
    }
    int root = 48 + chord.root;
    if (voicing == ChordVoicing::Spread) root -= 12;
    for (int interval : essential) {
        int note = root + interval;
        if (voicing == ChordVoicing::VoiceLed && hasSeventh &&
            (interval == 10 || interval == 11)) {
            note -= 12;
        }
        if (voicing == ChordVoicing::Spread && interval > 0) note += interval < 7 ? 12 : 0;
        notes.push_back(note);
    }
    if (voicing == ChordVoicing::VoiceLed) {
        const double center =
            std::accumulate(notes.cbegin(), notes.cend(), 0.0) / notes.size();
        const double targetCenter =
            previousCenter > 0.0 ? previousCenter : 56.0;
        int octaveShift = 0;
        double bestDistance = std::numeric_limits<double>::max();
        for (int candidate = -24; candidate <= 24; candidate += 12) {
            const double candidateCenter = center + candidate;
            if (candidateCenter < 50.0 || candidateCenter > 62.0) continue;
            const double distance =
                std::abs(candidateCenter - targetCenter);
            if (distance < bestDistance) {
                bestDistance = distance;
                octaveShift = candidate;
            }
        }
        for (int& note : notes) note += octaveShift;
    }
    if (chord.bass >= 0 && voicing != ChordVoicing::VoiceLed) {
        int bass = 36 + chord.bass;
        while (bass >= 48) bass -= 12;
        if (voicing == ChordVoicing::Spread) {
            for (int& note : notes) {
                while (note - bass < 5) note += 12;
            }
        } else if (!notes.isEmpty()) {
            const int lowest =
                *std::min_element(notes.cbegin(), notes.cend());
            while (lowest - bass < 5) bass -= 12;
        }
        notes.prepend(bass);
    }
    return notes;
}

bool resolvedChordVoicings(
    const SongSection& section,
    ChordVoicing voicing,
    QVector<QVector<int>>& out,
    QString& error)
{
    if (voicing == ChordVoicing::StyleDefault) {
        const QString style = section.generatedRecipe.styleId;
        const QString profile = section.generatedRecipe.profileId;
        voicing = style == QStringLiteral("jazz") ||
                style == QStringLiteral("rnb-soul") ||
                style == QStringLiteral("bossa-nova") ||
                style == QStringLiteral("blues") ||
                style == QStringLiteral("funk") ||
                profile == QStringLiteral("hiphop_boom_bap") ||
                profile == QStringLiteral("rock_shuffle_blues")
            ? ChordVoicing::VoiceLed
            : style == QStringLiteral("modal-jam") ||
                    style == QStringLiteral("electronic")
                ? ChordVoicing::Spread : ChordVoicing::Close;
    }
    out.resize(section.beats * kTicksPerBeat);
    QVector<int> current;
    double previousCenter = 0.0;
    for (int beat = 0; beat < section.beats; ++beat) {
        const bool timed = beat < section.musicalPatterns.size() &&
            section.musicalPatterns[beat].division > 0 &&
            section.musicalPatterns[beat].chords.size() == section.musicalPatterns[beat].division;
        const int division = timed ? section.musicalPatterns[beat].division : 1;
        for (int tick = 0; tick < kTicksPerBeat; ++tick) {
            if (tick % (kTicksPerBeat / division) == 0) {
                const int stepIndex = tick * division / kTicksPerBeat;
                MusicalStep step;
                if (timed) step = section.musicalPatterns[beat].chords[stepIndex];
                else {
                    const QString legacy = section.chords.value(beat).trimmed();
                    step.state = legacy.isEmpty() ? MusicalStepState::Hold
                        : legacy == QStringLiteral("-") ? MusicalStepState::Rest
                                                        : MusicalStepState::Onset;
                    step.value = legacy;
                }
                if (step.state == MusicalStepState::Rest) {
                    current.clear();
                } else if (step.state == MusicalStepState::Onset) {
                    const ParsedChord parsed = parseChord(step.value);
                    if (!parsed.valid) {
                        error = QStringLiteral("Unsupported chord symbol at beat %1, step %2: %3")
                            .arg(beat + 1).arg(stepIndex + 1).arg(step.value);
                        return false;
                    }
                    if (parsed.rest) current.clear();
                    else {
                        current = voicedNotes(parsed, voicing, previousCenter);
                        double total = 0.0;
                        for (int note : current) total += note;
                        previousCenter = current.isEmpty() ? previousCenter : total / current.size();
                    }
                }
            }
            out[beat * kTicksPerBeat + tick] = current;
        }
    }
    return true;
}

ReferenceWav renderChords(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& path,
    QString& error)
{
    QVector<QVector<int>> voicings;
    if (!resolvedChordVoicings(section, settings.voicing, voicings, error)) return {};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create chord reference WAV.");
        return {};
    }
    writeWavHeader(file, settings.sampleRate, totalFrames);
    const SynthPatch patch = synthPatch(section, QStringLiteral("chords"));
    const LaneTimingRecipe* compTiming =
        laneTimingRecipe(section, QStringLiteral("comping"));
    const double filterAmount = filterCoefficient(patch, settings.sampleRate);
    const double framesPerBeat = framesPerMusicalBeat(section, settings);
    const qint64 attackFrames = static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0);
    const qint64 releaseFrames = static_cast<qint64>(settings.releaseMs * settings.sampleRate / 1000.0);
    double filtered = 0.0;
    QVector<float> delay(qMax(1, static_cast<int>(settings.sampleRate * 0.125)), 0.0f);
    float renderedPeak = 0.0f;
    double renderedEnergy = 0.0;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (int offset = 0; offset < count; ++offset) {
            const qint64 frame = first + offset;
            const qint64 roughTick = qMin(commonBeats * kTicksPerBeat - 1,
                static_cast<qint64>(frame * kTicksPerBeat / framesPerBeat));
            double timingShift = 0.0;
            if (compTiming) {
                timingShift = compTiming->offsetMs * settings.sampleRate / 1000.0;
                timingShift += deterministicUnit(
                    section.generatedRecipe.seed,
                    static_cast<std::uint32_t>(roughTick * 193 + 0x4a31U)) *
                    compTiming->varianceMs * settings.sampleRate / 1000.0;
            }
            const qint64 musicalFrame = qBound<qint64>(
                0,
                static_cast<qint64>(std::llround(frame - timingShift)),
                totalFrames - 1);
            const qint64 absoluteTick = qMin(commonBeats * kTicksPerBeat - 1,
                static_cast<qint64>(musicalFrame * kTicksPerBeat / framesPerBeat));
            const int tick = static_cast<int>(absoluteTick % (section.beats * kTicksPerBeat));
            const QVector<int>& notes = voicings.at(tick);
            if (notes.isEmpty()) continue;
            const qint64 tickStart = static_cast<qint64>(absoluteTick * framesPerBeat / kTicksPerBeat);
            const qint64 tickEnd = static_cast<qint64>((absoluteTick + 1) * framesPerBeat / kTicksPerBeat);
            const QVector<int>& previous = voicings.at((tick - 1 + voicings.size()) % voicings.size());
            const QVector<int>& next = voicings.at((tick + 1) % voicings.size());
            double envelope = 1.0;
            if (notes != previous && attackFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(musicalFrame - tickStart) / attackFrames);
            }
            if (notes != next && releaseFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(tickEnd - musicalFrame) / releaseFrames);
            }
            double value = 0.0;
            const double seconds = static_cast<double>(frame) / settings.sampleRate;
            const double noteAge = static_cast<double>(frame - tickStart) / settings.sampleRate;
            for (int note : notes) {
                value += patchTone(patch, midiFrequency(note), seconds, noteAge);
            }
            value = settings.chordLevel * envelope * value / notes.size();
            const double automatedCutoff = automationValue(
                section,
                QStringLiteral("chords.cutoff_hz"),
                absoluteTick,
                -1.0);
            const double currentFilterAmount = automatedCutoff > 0.0
                ? filterCoefficientHz(automatedCutoff, settings.sampleRate)
                : filterAmount;
            filtered += currentFilterAmount * (value - filtered);
            const int delayIndex = static_cast<int>(frame % delay.size());
            const double effected = filtered + patch.delayMix * delay.at(delayIndex);
            delay[delayIndex] = static_cast<float>(filtered);
            block[offset] = static_cast<float>(effected);
            renderedPeak = qMax(renderedPeak, std::abs(block[offset]));
        }
        for (float sample : block) {
            renderedEnergy +=
                static_cast<double>(sample) * sample;
        }
        if (!writeBlock(file, block)) {
            error = QStringLiteral("Cannot write chord reference WAV.");
            return {};
        }
    }
    if (!file.commit()) {
        error = QStringLiteral("Cannot finalize chord reference WAV.");
        return {};
    }
    int eventCount = 0;
    for (const MusicalBeatPattern& pattern : section.musicalPatterns) {
        for (const MusicalStep& step : pattern.chords)
            if (step.state == MusicalStepState::Onset) ++eventCount;
    }
    if (section.musicalPatterns.isEmpty())
        for (const QString& chord : section.chords) if (!chord.trimmed().isEmpty()) ++eventCount;
    return {
        path,
        fileHash(path),
        totalFrames,
        renderedPeak,
        eventCount,
        static_cast<float>(
            std::sqrt(renderedEnergy / qMax<qint64>(1, totalFrames))),
    };
}

bool resolvedMelodyNotes(const SongSection& section, QVector<int>& notes, QString& error)
{
    notes.resize(section.beats);
    int current = -1;
    bool found = false;
    for (int beat = 0; beat < section.beats; ++beat) {
        const QString value = section.targets.value(beat).trimmed();
        if (value == QStringLiteral("-")) {
            current = -1;
        } else if (!value.isEmpty()) {
            const std::optional<int> parsed = parseMidiNote(value);
            if (!parsed) {
                error = QStringLiteral("Unsupported melody note at beat %1: %2")
                    .arg(beat + 1).arg(value);
                return false;
            }
            current = *parsed;
            found = true;
        }
        notes[beat] = current;
    }
    if (!found) {
        error = QStringLiteral("The generated section contains no renderable melody notes.");
        return false;
    }
    return true;
}

ReferenceWav renderLegacyMelody(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& path,
    QString& error)
{
    QVector<int> notes;
    if (!resolvedMelodyNotes(section, notes, error)) return {};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create melody reference WAV.");
        return {};
    }
    writeWavHeader(file, settings.sampleRate, totalFrames);
    const SynthPatch patch = synthPatch(section, QStringLiteral("melody"));
    const double framesPerBeat = framesPerMusicalBeat(section, settings);
    const qint64 attackFrames = static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0);
    const qint64 releaseFrames = static_cast<qint64>(settings.releaseMs * settings.sampleRate / 1000.0);
    double filtered = 0.0;
    QVector<float> delay(qMax(1, static_cast<int>(settings.sampleRate * 0.14)), 0.0f);
    float renderedPeak = 0.0f;
    double renderedEnergy = 0.0;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (int offset = 0; offset < count; ++offset) {
            const qint64 frame = first + offset;
            const qint64 absoluteBeat =
                qMin(commonBeats - 1, static_cast<qint64>(frame / framesPerBeat));
            const int beat = static_cast<int>(absoluteBeat % section.beats);
            const int note = notes.at(beat);
            if (note < 0) continue;
            const int previous = notes.at((beat - 1 + section.beats) % section.beats);
            const int next = notes.at((beat + 1) % section.beats);
            const qint64 beatStart = static_cast<qint64>(absoluteBeat * framesPerBeat);
            const qint64 beatEnd = static_cast<qint64>((absoluteBeat + 1) * framesPerBeat);
            double envelope = 1.0;
            if (note != previous && attackFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(frame - beatStart) / attackFrames);
            }
            if (note != next && releaseFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(beatEnd - frame) / releaseFrames);
            }
            const double seconds = static_cast<double>(frame) / settings.sampleRate;
            const double noteAge = static_cast<double>(frame - beatStart) / settings.sampleRate;
            const double value = settings.melodyLevel * envelope *
                patchTone(patch, midiFrequency(note), seconds, noteAge);
            filtered += std::clamp(patch.filter, 0.05, 0.90) * (value - filtered);
            const int delayIndex = static_cast<int>(frame % delay.size());
            block[offset] = static_cast<float>(filtered + patch.delayMix * delay.at(delayIndex));
            delay[delayIndex] = static_cast<float>(filtered);
            renderedPeak = qMax(renderedPeak, std::abs(block[offset]));
        }
        for (float sample : block) {
            renderedEnergy +=
                static_cast<double>(sample) * sample;
        }
        if (!writeBlock(file, block)) {
            error = QStringLiteral("Cannot write melody reference WAV.");
            return {};
        }
    }
    if (!file.commit()) {
        error = QStringLiteral("Cannot finalize melody reference WAV.");
        return {};
    }
    int eventCount = 0;
    for (const QString& note : section.targets) if (!note.trimmed().isEmpty() && note != QStringLiteral("-")) ++eventCount;
    return {
        path,
        fileHash(path),
        totalFrames,
        renderedPeak,
        eventCount,
        static_cast<float>(
            std::sqrt(renderedEnergy / qMax<qint64>(1, totalFrames))),
    };
}

struct MelodyRenderEvent {
    qint64 frame = 0;
    qint64 durationFrames = 1;
    int midi = 60;
    int velocity = 88;
    QString articulation;
};

bool noteLaneRenderEvents(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    const QString& laneId,
    QVector<MelodyRenderEvent>& output,
    QString& error)
{
    struct TickEvent {
        int tick;
        int duration;
        int midi;
        int velocity;
        QString articulation;
    };
    QVector<TickEvent> base;
    int active = -1;
    const auto closeAt = [&base, &active](int tick) {
        if (active >= 0) {
            base[active].duration = qMax(1, tick - base[active].tick);
            active = -1;
        }
    };
    for (int beat = 0; beat < section.beats; ++beat) {
        const QVector<MusicalStep>* laneSteps = nullptr;
        if (beat < section.musicalPatterns.size()) {
            const MusicalBeatPattern& pattern = section.musicalPatterns[beat];
            laneSteps = laneId == QStringLiteral("bass") ? &pattern.bass
                : laneId == QStringLiteral("support") ? &pattern.support
                : &pattern.melody;
        }
        const bool timed = beat < section.musicalPatterns.size() &&
            section.musicalPatterns[beat].division > 0 &&
            laneSteps && laneSteps->size() == section.musicalPatterns[beat].division;
        const int division = timed ? section.musicalPatterns[beat].division : 1;
        for (int stepIndex = 0; stepIndex < division; ++stepIndex) {
            const int tick = beat * kTicksPerBeat + stepIndex * kTicksPerBeat / division;
            MusicalStep step;
            if (timed) step = laneSteps->at(stepIndex);
            else if (laneId == QStringLiteral("melody")) {
                const QString legacy = section.targets.value(beat).trimmed();
                step.state = legacy.isEmpty() ? MusicalStepState::Hold
                    : legacy == QStringLiteral("-") ? MusicalStepState::Rest
                                                    : MusicalStepState::Onset;
                step.value = legacy;
            } else {
                step.state = MusicalStepState::Rest;
            }
            if (step.state == MusicalStepState::Rest) {
                closeAt(tick);
            } else if (step.state == MusicalStepState::Onset) {
                closeAt(tick);
                const std::optional<int> midi = parseMidiNote(step.value);
                if (!midi) {
                    error = QStringLiteral("Unsupported %1 note at beat %2, step %3: %4")
                        .arg(laneId).arg(beat + 1).arg(stepIndex + 1).arg(step.value);
                    return false;
                }
                base.push_back({
                    tick, 1, *midi, qBound(1, step.velocity, 127), step.articulation});
                active = base.size() - 1;
            }
        }
    }
    closeAt(section.beats * kTicksPerBeat);
    if (base.isEmpty()) {
        error = QStringLiteral("The generated section contains no renderable %1 notes.")
            .arg(laneId);
        return false;
    }
    const double framesPerTick =
        framesPerMusicalBeat(section, settings) / kTicksPerBeat;
    const LaneTimingRecipe* timing = laneTimingRecipe(section, laneId);
    for (qint64 cycleBeat = 0; cycleBeat < commonBeats; cycleBeat += section.beats) {
        const qint64 cycleTick = cycleBeat * kTicksPerBeat;
        for (const TickEvent& event : base) {
            double frame = (cycleTick + event.tick) * framesPerTick;
            if (timing) {
                frame += timing->offsetMs * settings.sampleRate / 1000.0;
                frame += deterministicUnit(
                    section.generatedRecipe.seed,
                    static_cast<std::uint32_t>(event.tick * 131 + laneId.size() * 17)) *
                    timing->varianceMs * settings.sampleRate / 1000.0;
            }
            output.push_back({
                qMax<qint64>(0, static_cast<qint64>(std::llround(frame))),
                qMax<qint64>(1, static_cast<qint64>(event.duration * framesPerTick)),
                event.midi,
                event.velocity,
                event.articulation,
            });
        }
    }
    return true;
}

ReferenceWav renderNoteLane(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& laneId,
    double level,
    const QString& path,
    QString& error)
{
    QVector<MelodyRenderEvent> events;
    if (!noteLaneRenderEvents(section, settings, commonBeats, laneId, events, error)) return {};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create %1 reference WAV.").arg(laneId);
        return {};
    }
    writeWavHeader(file, settings.sampleRate, totalFrames);
    const SynthPatch patch = synthPatch(section, laneId);
    const double filterAmount = filterCoefficient(patch, settings.sampleRate);
    const qint64 attackFrames = qMax<qint64>(1, qMin<qint64>(
        settings.sampleRate * 20 / 1000,
        static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0)));
    const qint64 decayFrames = settings.sampleRate * 55 / 1000;
    const qint64 requestedRelease = static_cast<qint64>(
        settings.releaseMs * settings.sampleRate / 1000.0);
    double filtered = 0.0;
    QVector<float> delay(qMax(1, static_cast<int>(settings.sampleRate * 0.14)), 0.0f);
    float renderedPeak = 0.0f;
    double renderedEnergy = 0.0;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (const MelodyRenderEvent& event : events) {
            if (event.frame >= first + count) break;
            if (event.frame + event.durationFrames <= first) continue;
            const qint64 begin = qMax(first, event.frame);
            const qint64 end = qMin(first + count, event.frame + event.durationFrames);
            const qint64 releaseFrames = qMax<qint64>(1,
                qMin(requestedRelease, event.durationFrames / 3));
            for (qint64 frame = begin; frame < end; ++frame) {
                const qint64 age = frame - event.frame;
                const qint64 remaining = event.frame + event.durationFrames - frame;
                double envelope = age < attackFrames ? static_cast<double>(age) / attackFrames : 1.0;
                if (age >= attackFrames && age < attackFrames + decayFrames) {
                    const double decay = static_cast<double>(age - attackFrames) / qMax<qint64>(1, decayFrames);
                    envelope *= 1.0 - decay * 0.22;
                } else if (age >= attackFrames + decayFrames) {
                    envelope *= 0.78;
                }
                if (remaining < releaseFrames) envelope *= static_cast<double>(remaining) / releaseFrames;
                if (event.articulation.contains(QStringLiteral("short")) ||
                    event.articulation.contains(QStringLiteral("choke")) ||
                    event.articulation.contains(QStringLiteral("muted"))) {
                    envelope *= std::exp(
                        -static_cast<double>(age) /
                        qMax<qint64>(1, settings.sampleRate / 18));
                } else if (event.articulation.contains(QStringLiteral("sustain")) ||
                           event.articulation.contains(QStringLiteral("legato"))) {
                    envelope = qMax(envelope, remaining < releaseFrames
                        ? static_cast<double>(remaining) / releaseFrames : 0.88);
                }
                const double seconds = static_cast<double>(age) / settings.sampleRate;
                const double vibratoRamp = laneId == QStringLiteral("melody")
                    ? std::clamp((seconds - 0.12) / 0.16, 0.0, 1.0) : 0.0;
                const double vibratoCents = vibratoRamp * 4.0 * std::sin(2.0 * kPi * 5.1 * seconds);
                const double frequency = midiFrequency(event.midi) * std::pow(2.0, vibratoCents / 1200.0);
                const double roleLevel = laneId == QStringLiteral("support")
                    ? automationValue(
                        section,
                        QStringLiteral("support.level"),
                        static_cast<qint64>(frame * kTicksPerBeat /
                            framesPerMusicalBeat(section, settings)),
                        1.0)
                    : 1.0;
                block[static_cast<int>(frame - first)] += static_cast<float>(
                    level * roleLevel * (event.velocity / 127.0) * envelope *
                    patchTone(patch, frequency, seconds, seconds));
            }
        }
        for (int offset = 0; offset < block.size(); ++offset) {
            filtered += filterAmount * (block[offset] - filtered);
            const qint64 frame = first + offset;
            const int delayIndex = static_cast<int>(frame % delay.size());
            const float effected = static_cast<float>(filtered + patch.delayMix * delay.at(delayIndex));
            delay[delayIndex] = static_cast<float>(filtered);
            block[offset] = effected;
            renderedPeak = qMax(renderedPeak, std::abs(effected));
        }
        for (float sample : block) {
            renderedEnergy +=
                static_cast<double>(sample) * sample;
        }
        if (!writeBlock(file, block)) {
            error = QStringLiteral("Cannot write %1 reference WAV.").arg(laneId);
            return {};
        }
    }
    if (!file.commit()) {
        error = QStringLiteral("Cannot finalize %1 reference WAV.").arg(laneId);
        return {};
    }
    return {
        path,
        fileHash(path),
        totalFrames,
        renderedPeak,
        static_cast<int>(events.size()),
        static_cast<float>(
            std::sqrt(renderedEnergy / qMax<qint64>(1, totalFrames))),
    };
}

double stateLevel(QChar state)
{
    if (state == QLatin1Char('g')) return 0.30;
    if (state == QLatin1Char('a')) return 1.0;
    return 0.70;
}

std::optional<DrumEvent::Instrument> drumInstrument(const QString& name)
{
    if (name == QStringLiteral("Kick")) return DrumEvent::Instrument::Kick;
    if (name == QStringLiteral("Snare")) return DrumEvent::Instrument::Snare;
    if (name == QStringLiteral("Closed HH")) return DrumEvent::Instrument::ClosedHat;
    if (name == QStringLiteral("Open HH")) return DrumEvent::Instrument::OpenHat;
    if (name == QStringLiteral("High Tom")) return DrumEvent::Instrument::HighTom;
    if (name == QStringLiteral("Mid Tom") ||
        name == QStringLiteral("Tom")) {
        return DrumEvent::Instrument::MidTom;
    }
    if (name == QStringLiteral("Floor Tom")) return DrumEvent::Instrument::FloorTom;
    if (name == QStringLiteral("Crash")) return DrumEvent::Instrument::Crash;
    if (name == QStringLiteral("Ride")) return DrumEvent::Instrument::Ride;
    if (name == QStringLiteral("Cross-stick / Rim")) return DrumEvent::Instrument::CrossStick;
    if (name == QStringLiteral("Shaker")) return DrumEvent::Instrument::Shaker;
    if (name == QStringLiteral("Hand Percussion")) return DrumEvent::Instrument::HandPercussion;
    return std::nullopt;
}

std::optional<DrumEvent::Instrument> drumInstrumentId(
    const QString& id)
{
    if (id == QStringLiteral("kick")) {
        return DrumEvent::Instrument::Kick;
    }
    if (id == QStringLiteral("snare")) {
        return DrumEvent::Instrument::Snare;
    }
    if (id == QStringLiteral("closed_hat")) {
        return DrumEvent::Instrument::ClosedHat;
    }
    if (id == QStringLiteral("open_hat")) {
        return DrumEvent::Instrument::OpenHat;
    }
    if (id == QStringLiteral("high_tom")) {
        return DrumEvent::Instrument::HighTom;
    }
    if (id == QStringLiteral("mid_tom")) {
        return DrumEvent::Instrument::MidTom;
    }
    if (id == QStringLiteral("floor_tom")) {
        return DrumEvent::Instrument::FloorTom;
    }
    if (id == QStringLiteral("crash")) {
        return DrumEvent::Instrument::Crash;
    }
    if (id == QStringLiteral("ride")) {
        return DrumEvent::Instrument::Ride;
    }
    if (id == QStringLiteral("cross_stick")) {
        return DrumEvent::Instrument::CrossStick;
    }
    if (id == QStringLiteral("shaker")) {
        return DrumEvent::Instrument::Shaker;
    }
    if (id == QStringLiteral("hand_percussion")) {
        return DrumEvent::Instrument::HandPercussion;
    }
    return std::nullopt;
}

double deterministicUnit(std::uint32_t seed, std::uint32_t salt)
{
    std::uint32_t value = seed ^ salt;
    value ^= value >> 16;
    value *= 2246822519U;
    value ^= value >> 13;
    return static_cast<double>(value) / 2147483647.5 - 1.0;
}

QVector<DrumEvent> drumEvents(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats)
{
    QVector<DrumEvent> events;
    const double framesPerBeat = framesPerMusicalBeat(section, settings);
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const GenerationRecipe* feel = section.generatedRecipe.isValid()
        ? &section.generatedRecipe : nullptr;
    const int swingPercent = feel ? feel->swingPercent : 50;
    const int snareOffsetMs = feel ? feel->snareOffsetMs : 0;
    const LaneTimingRecipe* drumTiming = laneTimingRecipe(section, QStringLiteral("drums"));
    const int laneOffsetMs = drumTiming ? drumTiming->offsetMs : 0;
    const int timingVariationMs = drumTiming
        ? drumTiming->varianceMs
        : feel ? feel->timingVariationMs : 0;
    const int velocityVariationPercent = feel ? feel->velocityVariationPercent : 0;
    const qint64 maximumFrame = qMax<qint64>(0,
        static_cast<qint64>(std::ceil(commonBeats * framesPerBeat)) - 1);
    int kit = 0;
    const QString kitId = section.generatedRecipe.drumPatchId;
    if (kitId.contains(QStringLiteral("808"))) kit = 3;
    else if (kitId.contains(QStringLiteral("bossa")) || kitId.contains(QStringLiteral("percussion"))) kit = 4;
    else if (kitId.contains(QStringLiteral("reggae"))) kit = 5;
    else if (kitId.contains(QStringLiteral("metal"))) kit = 6;
    else if (kitId.contains(QStringLiteral("electronic")) || kitId.contains(QStringLiteral("punch"))) kit = 2;
    else if (kitId.contains(QStringLiteral("brush")) || kitId.contains(QStringLiteral("pocket"))) kit = 1;
    const bool usePerformance =
        feel && !feel->drumEvents.isEmpty() &&
        generatedBeatFingerprint(section) == feel->beatFingerprint;
    if (usePerformance) {
        const qint64 cycleTicks =
            qMax<qint64>(1, section.beats * kTicksPerBeat);
        const double generatedGain =
            std::pow(10.0, feel->drumMixGainDb / 20.0);
        const ResearchDrumKit* researchedKit =
            researchDrumKitById(feel->drumPatchId);
        for (qint64 cycleStart = 0;
             cycleStart < commonBeats * kTicksPerBeat;
             cycleStart += cycleTicks) {
            for (const DrumPerformanceEvent& performed :
                 feel->drumEvents) {
                const qint64 absoluteTick =
                    cycleStart + performed.tick;
                if (absoluteTick >= commonBeats * kTicksPerBeat) break;
                const auto instrument =
                    drumInstrumentId(performed.laneId);
                if (!instrument) continue;
                DrumEvent event;
                event.instrument = *instrument;
                event.noiseSeed = static_cast<std::uint32_t>(
                    (absoluteTick + 1) * 2654435761ULL +
                    performed.velocity * 131 +
                    performed.repeatGroup * 17);
                double beatPosition =
                    static_cast<double>(absoluteTick) /
                    kTicksPerBeat;
                const int within =
                    static_cast<int>(absoluteTick % kTicksPerBeat);
                int laneSwing = swingPercent;
                if (*instrument == DrumEvent::Instrument::Kick) {
                    laneSwing =
                        50 + (swingPercent - 50) / 2;
                } else if (
                    *instrument == DrumEvent::Instrument::Snare ||
                    *instrument == DrumEvent::Instrument::CrossStick) {
                    laneSwing = 50;
                }
                const int pairStart =
                    within >= 6 ? 6 : 0;
                if (within == pairStart + 3) {
                    beatPosition +=
                        (laneSwing - 50) * 0.06 / 50.0;
                } else if (within == 6) {
                    beatPosition +=
                        (laneSwing - 50) * 0.12 / 50.0;
                } else if (within == 9) {
                    beatPosition +=
                        (laneSwing - 50) * 0.06 / 50.0;
                }
                double frame = beatPosition * framesPerBeat;
                frame +=
                    static_cast<double>(performed.offsetMs) *
                    settings.sampleRate / 1000.0;
                event.frame = qBound<qint64>(
                    0,
                    static_cast<qint64>(std::llround(frame)),
                    maximumFrame);
                event.level =
                    settings.drumLevel * generatedGain;
                event.kit = kit;
                event.velocity = performed.velocity;
                event.articulation = performed.articulation;
                event.laneId = performed.laneId;
                if (researchedKit) {
                    if (const ResearchDrumPiece* piece =
                            researchDrumPiece(
                                *researchedKit,
                                performed.laneId)) {
                        // Reggae's accepted construction deliberately keeps
                        // native Jam2 pieces and replaces only the crash.
                        if (piece->source !=
                            QStringLiteral("jam2-native")) {
                            event.researchedPiece = *piece;
                            event.useResearchedPiece = true;
                        }
                    }
                }
                if (!event.useResearchedPiece) {
                    event.level *=
                        performed.velocity / 127.0;
                }
                events.push_back(std::move(event));
            }
        }
        std::sort(
            events.begin(),
            events.end(),
            [](const DrumEvent& a, const DrumEvent& b) {
                return a.frame < b.frame;
            });
        int openHat = -1;
        for (int eventIndex = 0;
             eventIndex < events.size();
             ++eventIndex) {
            DrumEvent& event = events[eventIndex];
            if (event.laneId == QStringLiteral("open_hat")) {
                openHat = eventIndex;
            } else if (
                event.laneId == QStringLiteral("closed_hat") &&
                openHat >= 0) {
                DrumEvent& opened = events[openHat];
                const double chokeSeconds =
                    opened.useResearchedPiece
                    ? opened.researchedPiece.chokeSeconds
                    : 0.012;
                opened.chokeAge = qMax<qint64>(
                    0,
                    event.frame - opened.frame +
                        static_cast<qint64>(
                            chokeSeconds *
                            settings.sampleRate));
                openHat = -1;
            }
        }
        return events;
    }
    for (qint64 absoluteBeat = 0; absoluteBeat < commonBeats; ++absoluteBeat) {
        const BeatPattern& pattern = section.beatPatterns.at(static_cast<int>(absoluteBeat % section.beats));
        for (int lane = 0; lane < pattern.lanes.size() && lane < lanes.size(); ++lane) {
            const auto instrument = drumInstrument(lanes.at(lane));
            if (!instrument) continue;
            const QString states = pattern.lanes.at(lane).trimmed().toLower();
            for (int step = 0; step < pattern.division && step < states.size(); ++step) {
                const QChar state = states.at(step);
                if (state != QLatin1Char('x') && state != QLatin1Char('a') && state != QLatin1Char('g')) continue;
                DrumEvent event;
                event.instrument = *instrument;
                event.noiseSeed = static_cast<std::uint32_t>((absoluteBeat + 1) * 2654435761ULL + lane * 131 + step);
                double stepPosition = static_cast<double>(step) / pattern.division;
                if ((pattern.division == 2 || pattern.division == 4 || pattern.division == 8) && (step & 1)) {
                    int laneSwing = swingPercent;
                    if (*instrument == DrumEvent::Instrument::Kick) {
                        laneSwing = 50 + (swingPercent - 50) / 2;
                    } else if (*instrument == DrumEvent::Instrument::Snare ||
                               *instrument == DrumEvent::Instrument::CrossStick) {
                        laneSwing = 50;
                    }
                    const int pairStart = step - 1;
                    stepPosition = (pairStart + 2.0 * laneSwing / 100.0) / pattern.division;
                }
                double frame = (absoluteBeat + stepPosition) * framesPerBeat;
                frame += static_cast<double>(laneOffsetMs) * settings.sampleRate / 1000.0;
                if (*instrument == DrumEvent::Instrument::Snare) {
                    frame += static_cast<double>(snareOffsetMs) * settings.sampleRate / 1000.0;
                }
                frame += deterministicUnit(event.noiseSeed, 0x91e10da5U) *
                    timingVariationMs * settings.sampleRate / 1000.0;
                event.frame = qBound<qint64>(0, static_cast<qint64>(std::llround(frame)), maximumFrame);
                const double velocityScale = 1.0 +
                    deterministicUnit(event.noiseSeed, 0x7f4a7c15U) * velocityVariationPercent / 100.0;
                event.level = stateLevel(state) * settings.drumLevel * velocityScale;
                event.kit = kit;
                events.push_back(event);
            }
        }
    }
    std::sort(events.begin(), events.end(), [](const DrumEvent& a, const DrumEvent& b) { return a.frame < b.frame; });
    return events;
}

double noiseAt(std::uint32_t seed, qint64 index)
{
    std::uint32_t value = seed ^ static_cast<std::uint32_t>(index * 747796405ULL);
    value ^= value >> 16;
    value *= 2246822519U;
    value ^= value >> 13;
    return static_cast<double>(value) / 2147483648.0 - 1.0;
}

double drumSample(const DrumEvent& event, qint64 age, int sampleRate)
{
    if (age >= event.chokeAge) return 0.0;
    if (event.useResearchedPiece) {
        if (static_cast<double>(age) / sampleRate >
            researchDrumTailSeconds(
                event.researchedPiece,
                event.articulation)) {
            return 0.0;
        }
        return researchDrumSample(
            event.researchedPiece,
            event.laneId,
            event.articulation,
            event.velocity,
            event.noiseSeed,
            age,
            sampleRate);
    }
    const double t = static_cast<double>(age) / sampleRate;
    switch (event.instrument) {
    case DrumEvent::Instrument::Kick:
        return std::sin(2.0 * kPi * ((event.kit == 3 ? 48.0 :
            event.kit == 6 ? 78.0 : event.kit == 4 ? 54.0 :
            event.kit == 2 ? 68.0 : 62.0) -
            (event.kit == 6 ? 38.0 : 24.0) * qMin(1.0, t * 8.0)) * t) *
            std::exp(-t * (event.kit == 3 ? 4.5 : event.kit == 4 ? 18.0 :
                event.kit == 6 ? 20.0 : 11.0));
    case DrumEvent::Instrument::Snare:
        if (event.kit == 4) {
            return (0.28 * noiseAt(event.noiseSeed, age) +
                0.42 * std::sin(2.0 * kPi * 430.0 * t)) * std::exp(-t * 34.0);
        }
        if (event.kit == 5) {
            return (0.24 * noiseAt(event.noiseSeed, age) +
                0.58 * std::sin(2.0 * kPi * 760.0 * t)) * std::exp(-t * 29.0);
        }
        return ((event.kit == 1 ? 0.52 : event.kit == 6 ? 0.88 : 0.78) *
            noiseAt(event.noiseSeed, age) +
            (event.kit == 2 ? 0.34 : event.kit == 6 ? 0.42 : 0.22) *
            std::sin(2.0 * kPi * (event.kit == 2 ? 220.0 :
                event.kit == 6 ? 205.0 : 180.0) * t)) *
            std::exp(-t * (event.kit == 1 ? 25.0 : event.kit == 6 ? 24.0 : 18.0));
    case DrumEvent::Instrument::ClosedHat:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.6) * std::exp(-t * 55.0);
    case DrumEvent::Instrument::OpenHat:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.6) * std::exp(-t * 10.0);
    case DrumEvent::Instrument::HighTom:
    case DrumEvent::Instrument::MidTom:
    case DrumEvent::Instrument::FloorTom: {
        const double frequency =
            event.instrument == DrumEvent::Instrument::HighTom ? 168.0 :
            event.instrument == DrumEvent::Instrument::FloorTom ? 82.0 :
            118.0;
        const double damping =
            event.instrument == DrumEvent::Instrument::HighTom ? 12.0 :
            event.instrument == DrumEvent::Instrument::FloorTom ? 7.5 :
            9.5;
        return std::sin(2.0 * kPi * frequency * t) *
            std::exp(-t * damping);
    }
    case DrumEvent::Instrument::Crash:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.4) * std::exp(-t * 3.5);
    case DrumEvent::Instrument::Ride: {
        const double metal =
            0.035 * std::sin(2.0 * kPi * 1433.0 * t) +
            0.024 * std::sin(2.0 * kPi * 2179.0 * t) +
            0.016 * std::sin(2.0 * kPi * 3313.0 * t);
        const double wash = 0.30 * noiseAt(event.noiseSeed, age) *
            ((age & 1) ? 1.0 : -0.35);
        return wash * std::exp(-t * 8.5) +
            metal * std::exp(-t * 18.0);
    }
    case DrumEvent::Instrument::CrossStick:
        return (0.46 * std::sin(2.0 * kPi *
                    (event.kit == 5 ? 620.0 :
                     event.kit == 4 ? 480.0 : 680.0) * t) +
            0.30 * noiseAt(event.noiseSeed, age)) * std::exp(-t * 62.0);
    case DrumEvent::Instrument::Shaker:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 0.8 : -0.55) *
            std::exp(-t * (event.kit == 4 ? 72.0 : 60.0));
    case DrumEvent::Instrument::HandPercussion:
        return (0.45 * std::sin(2.0 * kPi * 235.0 * t) +
            0.28 * noiseAt(event.noiseSeed, age)) * std::exp(-t * 24.0);
    }
    return 0.0;
}

qint64 drumTailFrames(const DrumEvent& event, int sampleRate)
{
    double seconds = 1.0;
    if (event.useResearchedPiece) {
        seconds = researchDrumTailSeconds(
            event.researchedPiece,
            event.articulation);
    } else {
        switch (event.instrument) {
        case DrumEvent::Instrument::Kick:
            seconds = event.kit == 3 ? 2.5 : 1.0;
            break;
        case DrumEvent::Instrument::Snare:
            seconds = 1.2;
            break;
        case DrumEvent::Instrument::ClosedHat:
            seconds = 0.35;
            break;
        case DrumEvent::Instrument::OpenHat:
            seconds = 1.5;
            break;
        case DrumEvent::Instrument::HighTom:
        case DrumEvent::Instrument::MidTom:
        case DrumEvent::Instrument::FloorTom:
            seconds = 1.8;
            break;
        case DrumEvent::Instrument::Crash:
            seconds = 5.0;
            break;
        case DrumEvent::Instrument::Ride:
            seconds = 4.0;
            break;
        case DrumEvent::Instrument::CrossStick:
            seconds = 0.45;
            break;
        case DrumEvent::Instrument::Shaker:
            seconds = 0.4;
            break;
        case DrumEvent::Instrument::HandPercussion:
            seconds = 1.2;
            break;
        }
    }
    const qint64 naturalTail = qMax<qint64>(
        1,
        static_cast<qint64>(
            std::ceil(seconds * sampleRate)));
    return qMin(naturalTail, event.chokeAge);
}

ReferenceWav renderDrums(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& path,
    QString& error)
{
    const QVector<DrumEvent> events = drumEvents(section, settings, commonBeats);
    if (events.isEmpty()) {
        error = QStringLiteral("The generated beat contains no renderable drum hits.");
        return {};
    }
    const ResearchDrumKit* researchedKit =
        section.generatedRecipe.isValid()
        ? researchDrumKitById(
              section.generatedRecipe.drumPatchId)
        : nullptr;
    const ResearchDrumBus bus =
        researchedKit
        ? researchedKit->bus
        : ResearchDrumBus{};

    QVector<float> rendered(
        static_cast<qsizetype>(totalFrames),
        0.0f);
    QVector<float> researchRoomSend(
        static_cast<qsizetype>(totalFrames),
        0.0f);
    QVector<ResearchDrumRenderEvent> researchEvents;
    if (researchedKit) {
        researchEvents.reserve(events.size());
        for (const DrumEvent& event : events) {
            if (!event.useResearchedPiece) continue;
            researchEvents.push_back({
                event.frame,
                event.laneId,
                event.articulation,
                event.velocity,
                0,
                event.noiseSeed,
            });
        }
        const ResearchDrumRenderResult research =
            renderResearchDrumVoices(
                *researchedKit,
                researchEvents,
                totalFrames,
                settings.sampleRate);
        const double researchScale =
            std::find_if(
                events.cbegin(),
                events.cend(),
                [](const DrumEvent& event) {
                    return event.useResearchedPiece;
                }) != events.cend()
            ? std::find_if(
                  events.cbegin(),
                  events.cend(),
                  [](const DrumEvent& event) {
                      return event.useResearchedPiece;
                  })->level
            : 1.0;
        for (qsizetype frame = 0;
             frame < rendered.size() &&
             frame < research.dry.size();
             ++frame) {
            rendered[frame] += static_cast<float>(
                researchScale * research.dry.at(frame));
            if (frame < research.roomSend.size()) {
                researchRoomSend[frame] += static_cast<float>(
                    researchScale *
                    research.roomSend.at(frame));
            }
        }
    }

    // Accepted Jam2-native pieces (currently the Reggae construction) are
    // deliberately retained and enter the same exact Drum Kit Lab bus.
    for (const DrumEvent& event : events) {
        if (event.useResearchedPiece) continue;
        const qint64 tailFrames =
            drumTailFrames(event, settings.sampleRate);
        const qint64 begin = qMax<qint64>(0, event.frame);
        const qint64 end = qMin(
            totalFrames,
            event.frame + tailFrames);
        for (qint64 frame = begin; frame < end; ++frame) {
            const qint64 age = frame - event.frame;
            double chokeGain = 1.0;
            if (event.chokeAge !=
                std::numeric_limits<qint64>::max()) {
                const qint64 fadeFrames =
                    qMax<qint64>(
                        1,
                        static_cast<qint64>(
                            settings.sampleRate * 0.010));
                if (age > event.chokeAge - fadeFrames) {
                    chokeGain = std::clamp(
                        static_cast<double>(
                            event.chokeAge - age) /
                            fadeFrames,
                        0.0,
                        1.0);
                }
            }
            rendered[static_cast<qsizetype>(frame)] +=
                static_cast<float>(
                    event.level * chokeGain *
                    drumSample(
                        event,
                        age,
                        settings.sampleRate));
        }
    }
    applyResearchDrumBus(
        rendered,
        researchRoomSend,
        bus,
        settings.sampleRate);
    const bool generated =
        section.generatedRecipe.isValid();
    const DrumMakeupMetrics makeup =
        applyGeneratedDrumMakeup(rendered, generated);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create drum reference WAV.");
        return {};
    }
    writeWavHeader(file, settings.sampleRate, totalFrames);
    float renderedPeak = 0.0f;
    double renderedEnergy = 0.0;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (int offset = 0; offset < count; ++offset) {
            float& sample = block[offset];
            sample = rendered.at(
                static_cast<qsizetype>(first + offset));
            renderedPeak =
                qMax(renderedPeak, std::abs(sample));
            renderedEnergy +=
                static_cast<double>(sample) * sample;
        }
        if (!writeBlock(file, block)) {
            error = QStringLiteral("Cannot write drum reference WAV.");
            return {};
        }
    }
    if (renderedPeak < 0.001f) {
        file.cancelWriting();
        error = QStringLiteral("The generated drum reference was silent.");
        return {};
    }
    if (!file.commit()) {
        error = QStringLiteral("Cannot finalize drum reference WAV.");
        return {};
    }
    return {
        path,
        fileHash(path),
        totalFrames,
        renderedPeak,
        static_cast<int>(events.size()),
        static_cast<float>(
            std::sqrt(renderedEnergy / qMax<qint64>(1, totalFrames))),
        makeup.preMakeupPeak,
        generated ? kGeneratedDrumStemMakeupDb : 0.0,
        makeup.limitedSamples,
    };
}

QJsonObject sectionSignature(const SongSection* section)
{
    if (!section) return {};
    QJsonArray chords;
    QJsonArray melody;
    QJsonArray patterns;
    QJsonArray musicalPatterns;
    for (const QString& chord : section->chords) chords.append(chord);
    for (const QString& note : section->targets) melody.append(note);
    for (const BeatPattern& pattern : section->beatPatterns) {
        QJsonArray lanes;
        for (const QString& lane : pattern.lanes) lanes.append(lane);
        patterns.append(QJsonObject{{QStringLiteral("division"), pattern.division}, {QStringLiteral("lanes"), lanes}});
    }
    for (const MusicalBeatPattern& pattern : section->musicalPatterns) {
        QJsonArray chordSteps;
        QJsonArray melodySteps;
        QJsonArray bassSteps;
        QJsonArray supportSteps;
        for (const MusicalStep& step : pattern.chords) {
            chordSteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
        }
        for (const MusicalStep& step : pattern.melody) {
            melodySteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
        }
        for (const MusicalStep& step : pattern.bass) {
            bassSteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
        }
        for (const MusicalStep& step : pattern.support) {
            supportSteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
        }
        musicalPatterns.append(QJsonObject{{QStringLiteral("division"), pattern.division},
            {QStringLiteral("chords"), chordSteps}, {QStringLiteral("melody"), melodySteps},
            {QStringLiteral("bass"), bassSteps}, {QStringLiteral("support"), supportSteps}});
    }
    return {{QStringLiteral("id"), section->id}, {QStringLiteral("beats"), section->beats},
        {QStringLiteral("chords"), chords}, {QStringLiteral("melody"), melody},
        {QStringLiteral("patterns"), patterns}, {QStringLiteral("musical_patterns"), musicalPatterns},
        {QStringLiteral("generated_recipe"), section->generatedRecipe.isValid()
            ? generationRecipeToJson(section->generatedRecipe) : QJsonObject{}}};
}

} // namespace

QJsonArray practiceChordVoicingDiagnostics(
    const SongSection& chordSection,
    ChordVoicing voicing,
    QString& error)
{
    QVector<QVector<int>> resolved;
    if (!resolvedChordVoicings(chordSection, voicing, resolved, error)) {
        return {};
    }
    QJsonArray result;
    for (int beat = 0; beat < chordSection.beats; ++beat) {
        const bool timed = beat < chordSection.musicalPatterns.size() &&
            chordSection.musicalPatterns[beat].division > 0 &&
            chordSection.musicalPatterns[beat].chords.size() ==
                chordSection.musicalPatterns[beat].division;
        const int division = timed ? chordSection.musicalPatterns[beat].division : 1;
        for (int stepIndex = 0; stepIndex < division; ++stepIndex) {
            MusicalStep step;
            if (timed) {
                step = chordSection.musicalPatterns[beat].chords[stepIndex];
            } else if (stepIndex == 0) {
                const QString legacy = chordSection.chords.value(beat).trimmed();
                step.state = legacy.isEmpty() ? MusicalStepState::Hold
                    : legacy == QStringLiteral("-") ? MusicalStepState::Rest
                                                    : MusicalStepState::Onset;
                step.value = legacy;
            }
            if (step.state != MusicalStepState::Onset) continue;
            const ParsedChord chord = parseChord(step.value);
            if (!chord.valid || chord.rest) continue;
            const int tick = beat * kTicksPerBeat +
                stepIndex * kTicksPerBeat / division;
            const QVector<int> midi = resolved.value(tick);
            QJsonArray midiJson;
            for (int note : midi) midiJson.append(note);
            bool closeRootSeventh = false;
            for (int low = 0; low < midi.size(); ++low) {
                for (int high = low + 1; high < midi.size(); ++high) {
                    if (std::abs(midi[high] - midi[low]) != 1) continue;
                    const int lowFromRoot = (midi[low] - chord.root + 120) % 12;
                    const int highFromRoot = (midi[high] - chord.root + 120) % 12;
                    closeRootSeventh = closeRootSeventh ||
                        (lowFromRoot == 0 && (highFromRoot == 10 || highFromRoot == 11)) ||
                        (highFromRoot == 0 && (lowFromRoot == 10 || lowFromRoot == 11));
                }
            }
            result.append(QJsonObject{
                {QStringLiteral("tick"), tick},
                {QStringLiteral("beat"), static_cast<double>(tick) / kTicksPerBeat},
                {QStringLiteral("symbol"), step.value},
                {QStringLiteral("root_pitch_class"), chord.root},
                {QStringLiteral("slash_bass_pitch_class"), chord.bass},
                {QStringLiteral("midi"), midiJson},
                {QStringLiteral("close_root_seventh"), closeRootSeventh},
                {QStringLiteral("articulation"), step.articulation},
            });
        }
    }
    return result;
}

QString practiceReferenceSignature(
    const SongSection* chordSection,
    const SongSection* beatSection,
    const ReferenceRenderSettings& settings)
{
    const QJsonObject root{
        {QStringLiteral("idea"), practiceIdeaSignature(chordSection, beatSection)},
        {QStringLiteral("render_chords"), settings.renderChords},
        {QStringLiteral("render_drums"), settings.renderDrums},
        {QStringLiteral("render_melody"), settings.renderMelody},
        {QStringLiteral("render_bass"), settings.renderBass},
        {QStringLiteral("render_support"), settings.renderSupport},
        {QStringLiteral("bpm"), settings.bpm},
        {QStringLiteral("meter_numerator"), settings.meterNumerator},
        {QStringLiteral("meter_denominator"), settings.meterDenominator},
        {QStringLiteral("tempo_pulse_units"), settings.tempoPulseUnits},
        {QStringLiteral("rate"), settings.sampleRate},
        {QStringLiteral("voicing"), static_cast<int>(settings.voicing)},
        {QStringLiteral("chord_level"), settings.chordLevel},
        {QStringLiteral("drum_level"), settings.drumLevel},
        {QStringLiteral("melody_level"), settings.melodyLevel},
        {QStringLiteral("bass_level"), settings.bassLevel},
        {QStringLiteral("support_level"), settings.supportLevel},
        {QStringLiteral("attack_ms"), settings.attackMs},
        {QStringLiteral("release_ms"), settings.releaseMs},
        {QStringLiteral("generated_drum_stem_makeup_db"),
         kGeneratedDrumStemMakeupDb},
        {QStringLiteral("generated_drum_soft_limit_threshold"),
         kGeneratedDrumSoftLimitThreshold},
        {QStringLiteral("generated_drum_soft_limit_ceiling"),
         kGeneratedDrumSoftLimitCeiling},
    };
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(root).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QString practiceIdeaSignature(
    const SongSection* chordSection,
    const SongSection* beatSection)
{
    const QJsonObject root{
        {QStringLiteral("chord"), sectionSignature(chordSection)},
        {QStringLiteral("beat"), sectionSignature(beatSection)},
    };
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(root).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

ReferenceRenderResult renderPracticeReferences(
    const SongSection* chordSection,
    const SongSection* beatSection,
    const ReferenceRenderSettings& settings,
    const QString& workspaceFolder)
{
    ReferenceRenderResult result;
    QElapsedTimer elapsed;
    elapsed.start();
    if ((!settings.renderChords && !settings.renderDrums && !settings.renderMelody &&
         !settings.renderBass && !settings.renderSupport) ||
        settings.sampleRate < 8000 ||
        settings.bpm < 20.0 || settings.bpm > 400.0 ||
        (settings.renderChords && (!chordSection || chordSection->beats <= 0)) ||
        (settings.renderDrums && (!beatSection || beatSection->beats <= 0)) ||
        (settings.renderMelody && (!chordSection || chordSection->beats <= 0)) ||
        (settings.renderBass && (!chordSection || chordSection->beats <= 0)) ||
        (settings.renderSupport && (!chordSection || chordSection->beats <= 0))) {
        result.error = QStringLiteral("Reference render settings or source layers are invalid.");
        return result;
    }
    const qint64 chordBeats = settings.renderChords ? chordSection->beats : 0;
    const qint64 beatBeats = settings.renderDrums ? beatSection->beats : 0;
    const qint64 melodyBeats = settings.renderMelody ? chordSection->beats : 0;
    const qint64 bassBeats = settings.renderBass ? chordSection->beats : 0;
    const qint64 supportBeats = settings.renderSupport ? chordSection->beats : 0;
    qint64 commonBeats = 0;
    for (qint64 beats : {chordBeats, beatBeats, melodyBeats, bassBeats, supportBeats}) {
        if (beats <= 0) continue;
        commonBeats = commonBeats > 0 ? std::lcm(commonBeats, beats) : beats;
    }
    const SongSection* timingSection = chordSection ? chordSection : beatSection;
    const double seconds = commonBeats *
        framesPerMusicalBeat(*timingSection, settings) / settings.sampleRate;
    if (commonBeats <= 0 || seconds > kMaximumSeconds) {
        result.error = QStringLiteral("The common reference length exceeds the five-minute limit.");
        return result;
    }
    const qint64 frames = static_cast<qint64>(std::ceil(seconds * settings.sampleRate));
    const QString folder = QDir(workspaceFolder).absoluteFilePath(QStringLiteral("wavs"));
    if (!QDir().mkpath(folder)) {
        result.error = QStringLiteral(
            "Cannot create the reference WAV folder: %1").arg(folder);
        return result;
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString chordPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-chords-%1.wav").arg(token));
    const QString drumPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-drums-%1.wav").arg(token));
    const QString melodyPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-melody-%1.wav").arg(token));
    const QString bassPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-bass-%1.wav").arg(token));
    const QString supportPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-support-%1.wav").arg(token));
    result.sourceSignature = practiceIdeaSignature(chordSection, beatSection);
    if (settings.renderChords) {
        result.chords = renderChords(*chordSection, settings, commonBeats, frames, chordPath, result.error);
    }
    if (result.error.isEmpty() && settings.renderDrums) {
        result.drums = renderDrums(*beatSection, settings, commonBeats, frames, drumPath, result.error);
    }
    if (result.error.isEmpty() && settings.renderMelody) {
        result.melody =
            renderNoteLane(*chordSection, settings, commonBeats, frames,
                QStringLiteral("melody"), settings.melodyLevel, melodyPath, result.error);
    }
    if (result.error.isEmpty() && settings.renderBass) {
        result.bass =
            renderNoteLane(*chordSection, settings, commonBeats, frames,
                QStringLiteral("bass"), settings.bassLevel, bassPath, result.error);
    }
    if (result.error.isEmpty() && settings.renderSupport) {
        result.support =
            renderNoteLane(*chordSection, settings, commonBeats, frames,
                QStringLiteral("support"), settings.supportLevel, supportPath, result.error);
    }
    if (!result.error.isEmpty() || (settings.renderChords && result.chords.sha256.isEmpty()) ||
        (settings.renderDrums && result.drums.sha256.isEmpty()) ||
        (settings.renderMelody && result.melody.sha256.isEmpty()) ||
        (settings.renderBass && result.bass.sha256.isEmpty()) ||
        (settings.renderSupport && result.support.sha256.isEmpty())) {
        if (!result.chords.path.isEmpty()) QFile::remove(result.chords.path);
        if (!result.drums.path.isEmpty()) QFile::remove(result.drums.path);
        if (!result.melody.path.isEmpty()) QFile::remove(result.melody.path);
        if (!result.bass.path.isEmpty()) QFile::remove(result.bass.path);
        if (!result.support.path.isEmpty()) QFile::remove(result.support.path);
        if (result.error.isEmpty()) result.error = QStringLiteral("A rendered WAV could not be validated.");
    } else {
        const GenerationRecipe* recipe = chordSection && chordSection->generatedRecipe.isValid()
            ? &chordSection->generatedRecipe
            : beatSection && beatSection->generatedRecipe.isValid() ? &beatSection->generatedRecipe : nullptr;
        const float peak = qMax(
            qMax(result.chords.peak, result.drums.peak),
            qMax(result.melody.peak, qMax(result.bass.peak, result.support.peak)));
        int maximumChordVoices = 0;
        if (chordSection) {
            for (const QString& symbol : chordSection->chords) {
                const ParsedChord parsed = parseChord(symbol);
                if (parsed.valid && !parsed.rest)
                    maximumChordVoices = qMax(maximumChordVoices,
                        qMin(4, parsed.intervals.size()) + (parsed.bass >= 0 ? 1 : 0));
            }
        }
        result.diagnostics = QStringLiteral(
            "reference render: chord_patch=%1 melody_patch=%2 bass_patch=%3 support_patch=%4 drum_patch=%5 "
            "profile=%6 groove=%7 swing_percent=%8 snare_offset_ms=%9 timing_variation_ms=%10 "
            "velocity_variation_percent=%11 max_chord_voices=%12 chord_events=%13 "
            "melody_events=%14 bass_events=%15 support_events=%16 drum_events=%17 elapsed_ms=%18 peak=%19 drum_rms=%20 "
            "drum_pre_makeup_peak=%21 drum_makeup_db=%22 drum_limited_samples=%23")
            .arg(recipe ? recipe->chordPatchId : QStringLiteral("manual"),
                 recipe ? recipe->melodyPatchId : QStringLiteral("manual"),
                 recipe ? recipe->bassPatchId : QStringLiteral("manual"),
                 recipe ? recipe->supportPatchId : QStringLiteral("manual"),
                 recipe ? recipe->drumPatchId : QStringLiteral("manual"),
                 recipe ? recipe->profileId : QStringLiteral("manual"),
                 recipe ? recipe->grooveId : QStringLiteral("manual"))
            .arg(recipe ? recipe->swingPercent : 50)
            .arg(recipe ? recipe->snareOffsetMs : 0)
            .arg(recipe ? recipe->timingVariationMs : 0)
            .arg(recipe ? recipe->velocityVariationPercent : 0)
            .arg(maximumChordVoices).arg(result.chords.eventCount)
            .arg(result.melody.eventCount).arg(result.bass.eventCount)
            .arg(result.support.eventCount).arg(result.drums.eventCount)
            .arg(elapsed.elapsed()).arg(peak, 0, 'f', 4)
            .arg(result.drums.rms, 0, 'f', 5)
            .arg(result.drums.preMakeupPeak, 0, 'f', 4)
            .arg(result.drums.makeupGainDb, 0, 'f', 1)
            .arg(result.drums.limitedSamples);
    }
    return result;
}

} // namespace jam2::practice
