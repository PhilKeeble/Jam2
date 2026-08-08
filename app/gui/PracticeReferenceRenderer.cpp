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
#include <atomic>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>

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
        CrossStick
    };
    qint64 frame = 0;
    Instrument instrument = Instrument::Kick;
    double level = 0.7;
    std::uint32_t noiseSeed = 1;
    int velocity = 96;
    QString articulation;
    QString laneId;
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
        const double targetCenter =
            previousCenter > 0.0 ? previousCenter : 56.0;
        QVector<int> ordered = notes;
        std::sort(ordered.begin(), ordered.end());
        QVector<int> bestNotes = ordered;
        double bestDistance = std::numeric_limits<double>::max();
        for (int inversion = 0; inversion < ordered.size(); ++inversion) {
            QVector<int> candidateNotes = ordered;
            for (int voice = 0; voice < inversion; ++voice) {
                candidateNotes[voice] += 12;
            }
            std::sort(candidateNotes.begin(), candidateNotes.end());
            const double center = std::accumulate(
                candidateNotes.cbegin(), candidateNotes.cend(), 0.0) /
                candidateNotes.size();
            for (int shift = -24; shift <= 24; shift += 12) {
                const double candidateCenter = center + shift;
                if (candidateCenter < 50.0 || candidateCenter > 62.0) {
                    continue;
                }
                const double distance =
                    std::abs(candidateCenter - targetCenter);
                if (distance >= bestDistance) continue;
                bestDistance = distance;
                bestNotes = candidateNotes;
                for (int& note : bestNotes) note += shift;
            }
        }
        notes = std::move(bestNotes);
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

ChordVoicing styleDefaultVoicing(
    const SongSection& section,
    ChordVoicing voicing)
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
    return voicing;
}

ChordVoicing stepVoicing(
    const QString& name,
    ChordVoicing fallback)
{
    if (name == QStringLiteral("close")) return ChordVoicing::Close;
    if (name == QStringLiteral("spread")) return ChordVoicing::Spread;
    if (name == QStringLiteral("voice-led") ||
        name == QStringLiteral("inverted")) {
        return ChordVoicing::VoiceLed;
    }
    return fallback;
}

struct ResolvedChordEvent {
    int tick = 0;
    int durationTicks = 1;
    QVector<int> notes;
    int velocity = 88;
    QString articulation;
    QString voicing;
};

bool resolvedChordEvents(
    const SongSection& section,
    ChordVoicing requestedVoicing,
    QVector<ResolvedChordEvent>& out,
    QString& error)
{
    const ChordVoicing fallbackVoicing =
        styleDefaultVoicing(section, requestedVoicing);
    double previousCenter = 0.0;
    int active = -1;
    const auto closeAt = [&out, &active](int tick) {
        if (active >= 0) {
            out[active].durationTicks = qMax(
                1, tick - out[active].tick);
            active = -1;
        }
    };
    for (int beat = 0; beat < section.beats; ++beat) {
        const bool timed = beat < section.musicalPatterns.size() &&
            section.musicalPatterns[beat].division > 0 &&
            section.musicalPatterns[beat].chords.size() == section.musicalPatterns[beat].division;
        const int division = timed ? section.musicalPatterns[beat].division : 1;
        for (int stepIndex = 0; stepIndex < division; ++stepIndex) {
            const int tick = beat * kTicksPerBeat +
                stepIndex * kTicksPerBeat / division;
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
                closeAt(tick);
                continue;
            }
            if (step.state != MusicalStepState::Onset) continue;
            closeAt(tick);
            const ParsedChord parsed = parseChord(step.value);
            if (!parsed.valid) {
                error = QStringLiteral("Unsupported chord symbol at beat %1, step %2: %3")
                    .arg(beat + 1).arg(stepIndex + 1).arg(step.value);
                return false;
            }
            if (parsed.rest) continue;
            const ChordVoicing voicing = stepVoicing(
                step.voicing, fallbackVoicing);
            QVector<int> notes = voicedNotes(
                parsed, voicing, previousCenter);
            double total = 0.0;
            for (int note : notes) total += note;
            if (!notes.isEmpty()) {
                previousCenter = total / notes.size();
            }
            out.push_back({
                tick,
                1,
                std::move(notes),
                qBound(1, step.velocity, 127),
                step.articulation,
                step.voicing,
            });
            active = out.size() - 1;
        }
    }
    closeAt(section.beats * kTicksPerBeat);
    return true;
}

bool resolvedChordVoicings(
    const SongSection& section,
    ChordVoicing voicing,
    QVector<QVector<int>>& out,
    QString& error)
{
    QVector<ResolvedChordEvent> events;
    if (!resolvedChordEvents(section, voicing, events, error)) return false;
    out.fill(QVector<int>{}, section.beats * kTicksPerBeat);
    for (const ResolvedChordEvent& event : events) {
        const int end = qMin(out.size(), event.tick + event.durationTicks);
        for (int tick = event.tick; tick < end; ++tick) {
            out[tick] = event.notes;
        }
    }
    return true;
}

double chordGateRatio(const QString& articulation)
{
    const QString value = articulation.toLower();
    if (value.contains(QStringLiteral("choke")) ||
        value.contains(QStringLiteral("palm-muted"))) return 0.30;
    if (value.contains(QStringLiteral("staccato")) ||
        value.contains(QStringLiteral("short-muted")) ||
        value.contains(QStringLiteral("short-accented"))) return 0.42;
    if (value.contains(QStringLiteral("short")) ||
        value.contains(QStringLiteral("stab")) ||
        value.contains(QStringLiteral("chop")) ||
        value.contains(QStringLiteral("recut")) ||
        value.contains(QStringLiteral("pulse")) ||
        value.contains(QStringLiteral("detached"))) return 0.60;
    if (value.contains(QStringLiteral("driven"))) return 0.78;
    return 1.0;
}

double chordAttackScale(const QString& articulation)
{
    const QString value = articulation.toLower();
    if (value.contains(QStringLiteral("soft")) ||
        value.contains(QStringLiteral("pad"))) return 1.45;
    if (value.contains(QStringLiteral("accent")) ||
        value.contains(QStringLiteral("stab")) ||
        value.contains(QStringLiteral("chop")) ||
        value.contains(QStringLiteral("muted"))) return 0.55;
    return 1.0;
}

double chordReleaseScale(const QString& articulation)
{
    const QString value = articulation.toLower();
    if (value.contains(QStringLiteral("choke")) ||
        value.contains(QStringLiteral("muted")) ||
        value.contains(QStringLiteral("staccato"))) return 0.28;
    if (value.contains(QStringLiteral("short")) ||
        value.contains(QStringLiteral("stab")) ||
        value.contains(QStringLiteral("chop")) ||
        value.contains(QStringLiteral("detached"))) return 0.48;
    if (value.contains(QStringLiteral("sustain")) ||
        value.contains(QStringLiteral("open")) ||
        value.contains(QStringLiteral("pad")) ||
        value.contains(QStringLiteral("connected"))) return 1.35;
    return 1.0;
}

ReferenceWav renderChords(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& path,
    QString& error)
{
    QVector<ResolvedChordEvent> baseEvents;
    if (!resolvedChordEvents(
            section, settings.voicing, baseEvents, error)) return {};
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
    double generatedAttackMs = 8.0;
    double generatedReleaseMs = 100.0;
    for (const SynthVoiceRecipe& voice : section.generatedRecipe.synthVoices) {
        if (voice.roleId != QStringLiteral("chords")) continue;
        generatedAttackMs = voice.attackMs;
        generatedReleaseMs = voice.releaseMs;
        break;
    }
    const double baseAttackMs = generatedAttackMs * settings.attackMs / 8.0;
    const double baseReleaseMs = generatedReleaseMs * settings.releaseMs / 100.0;
    struct RenderEvent {
        qint64 frame = 0;
        qint64 durationFrames = 1;
        QVector<int> notes;
        int velocity = 88;
        QString articulation;
        qint64 absoluteTick = 0;
    };
    QVector<RenderEvent> events;
    const qint64 cycleTicks = qMax<qint64>(
        1, section.beats * kTicksPerBeat);
    const double framesPerTick = framesPerBeat / kTicksPerBeat;
    for (qint64 cycleStart = 0;
         cycleStart < commonBeats * kTicksPerBeat;
         cycleStart += cycleTicks) {
        for (const ResolvedChordEvent& event : baseEvents) {
            const qint64 absoluteTick = cycleStart + event.tick;
            if (absoluteTick >= commonBeats * kTicksPerBeat) break;
            double frame = absoluteTick * framesPerTick;
            if (compTiming) {
                frame += compTiming->offsetMs * settings.sampleRate / 1000.0;
                frame += deterministicUnit(
                    section.generatedRecipe.seed,
                    static_cast<std::uint32_t>(
                        absoluteTick * 193 + 0x4a31U)) *
                    compTiming->varianceMs * settings.sampleRate / 1000.0;
            }
            const int maximumTicks = static_cast<int>(
                commonBeats * kTicksPerBeat - absoluteTick);
            const int logicalTicks = qMax(
                1, qMin(event.durationTicks, maximumTicks));
            const int gateTicks = qMax(
                1,
                static_cast<int>(std::lround(
                    logicalTicks * chordGateRatio(event.articulation))));
            events.push_back({
                qMax<qint64>(0, static_cast<qint64>(std::llround(frame))),
                qMax<qint64>(1, static_cast<qint64>(
                    std::llround(gateTicks * framesPerTick))),
                event.notes,
                event.velocity,
                event.articulation,
                absoluteTick,
            });
        }
    }
    double filtered = 0.0;
    QVector<float> delay(qMax(1, static_cast<int>(settings.sampleRate * 0.125)), 0.0f);
    float renderedPeak = 0.0f;
    double renderedEnergy = 0.0;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (const RenderEvent& event : events) {
            if (event.frame >= first + count) break;
            if (event.frame + event.durationFrames <= first) continue;
            const qint64 begin = qMax(first, event.frame);
            const qint64 end = qMin(
                first + count, event.frame + event.durationFrames);
            const qint64 attackFrames = qMax<qint64>(
                1,
                static_cast<qint64>(
                    baseAttackMs * chordAttackScale(event.articulation) *
                    settings.sampleRate / 1000.0));
            const qint64 releaseFrames = qMax<qint64>(
                1,
                qMin<qint64>(
                    event.durationFrames / 2,
                    static_cast<qint64>(
                        baseReleaseMs * chordReleaseScale(event.articulation) *
                        settings.sampleRate / 1000.0)));
            for (qint64 frame = begin; frame < end; ++frame) {
                const qint64 age = frame - event.frame;
                const qint64 remaining =
                    event.frame + event.durationFrames - frame;
                double envelope = qMin(
                    1.0, static_cast<double>(age) / attackFrames);
                if (remaining < releaseFrames) {
                    envelope *= static_cast<double>(remaining) /
                        releaseFrames;
                }
                double value = 0.0;
                const double seconds =
                    static_cast<double>(age) / settings.sampleRate;
                for (int note : event.notes) {
                    value += patchTone(
                        patch, midiFrequency(note), seconds, seconds);
                }
                if (!event.notes.isEmpty()) value /= event.notes.size();
                block[static_cast<int>(frame - first)] +=
                    static_cast<float>(
                        settings.chordLevel *
                        (event.velocity / 127.0) * envelope * value);
            }
        }
        for (int offset = 0; offset < block.size(); ++offset) {
            const qint64 frame = first + offset;
            const qint64 absoluteTick = qMin(
                commonBeats * kTicksPerBeat - 1,
                static_cast<qint64>(
                    frame / qMax(1.0, framesPerTick)));
            const double automatedCutoff = automationValue(
                section, QStringLiteral("chords.cutoff_hz"),
                absoluteTick, -1.0);
            const double currentFilterAmount = automatedCutoff > 0.0
                ? filterCoefficientHz(automatedCutoff, settings.sampleRate)
                : filterAmount;
            filtered += currentFilterAmount *
                (block[offset] - filtered);
            const int delayIndex = static_cast<int>(frame % delay.size());
            const float effected = static_cast<float>(
                filtered + patch.delayMix * delay.at(delayIndex));
            delay[delayIndex] = static_cast<float>(filtered);
            block[offset] = effected;
            renderedPeak = qMax(renderedPeak, std::abs(effected));
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

int wrappedPitchClass(int value)
{
    const int remainder = value % 12;
    return remainder < 0 ? remainder + 12 : remainder;
}

QVector<int> modeAwareDrumTargetIntervals(const QString& modeName)
{
    const QString mode = modeName.trimmed().toLower();
    if (mode == QStringLiteral("major") || mode == QStringLiteral("ionian") ||
        mode == QStringLiteral("major pentatonic")) {
        return {0, 2, 4, 7, 9};
    }
    if (mode == QStringLiteral("natural minor") || mode == QStringLiteral("aeolian") ||
        mode == QStringLiteral("minor pentatonic")) {
        return {0, 3, 5, 7, 10};
    }
    if (mode == QStringLiteral("dorian")) return {0, 2, 3, 7, 9};
    if (mode == QStringLiteral("mixolydian")) return {0, 2, 4, 7, 10};
    if (mode == QStringLiteral("lydian")) return {0, 2, 4, 6, 9};
    if (mode == QStringLiteral("phrygian")) return {0, 1, 3, 7, 10};
    if (mode == QStringLiteral("dominant blues")) return {0, 3, 4, 7, 10};
    if (mode == QStringLiteral("blues") || mode == QStringLiteral("minor blues")) {
        return {0, 3, 5, 7, 10};
    }
    return {};
}

std::array<double, 12> drumHarmonyPitchWeights(const SongSection& section)
{
    std::array<double, 12> weights{};
    ParsedChord currentChord;
    for (const MusicalBeatPattern& pattern : section.musicalPatterns) {
        if (pattern.division <= 0 ||
            pattern.chords.size() != pattern.division) {
            continue;
        }
        const double stepWeight = 1.0 / pattern.division;
        for (const MusicalStep& step : pattern.chords) {
            if (step.state == MusicalStepState::Onset) {
                currentChord = parseChord(step.value);
            } else if (step.state == MusicalStepState::Rest) {
                currentChord = {};
            }
            if (!currentChord.valid || currentChord.rest) continue;
            weights[static_cast<std::size_t>(currentChord.root)] +=
                2.2 * stepWeight;
            for (int interval : currentChord.intervals) {
                weights[static_cast<std::size_t>(wrappedPitchClass(
                    currentChord.root + interval))] += stepWeight;
            }
            if (currentChord.bass >= 0) {
                weights[static_cast<std::size_t>(currentChord.bass)] +=
                    0.6 * stepWeight;
            }
        }
    }
    for (const MelodyRecipeEvent& event :
         section.generatedRecipe.melodyEvents) {
        const double duration = std::clamp(
            static_cast<double>(event.durationTicks) / kTicksPerBeat, 0.0, 2.0);
        weights[static_cast<std::size_t>(wrappedPitchClass(event.midi))] +=
            0.12 * duration;
    }
    for (const RoleRecipeEvent& event :
         section.generatedRecipe.bassEvents) {
        const double duration = std::clamp(
            static_cast<double>(event.durationTicks) / kTicksPerBeat, 0.0, 2.0);
        weights[static_cast<std::size_t>(wrappedPitchClass(event.midi))] +=
            0.16 * duration;
    }
    return weights;
}

double drumTargetHarmonyScore(
    int targetPitchClass,
    const std::array<double, 12>& weights)
{
    constexpr std::array<double, 7> relationship{
        3.0, -3.2, -0.35, 0.75, 0.45, 0.20, -2.2};
    double score = 0.0;
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass) {
        const int forward = wrappedPitchClass(targetPitchClass - pitchClass);
        const int distance = std::min(forward, 12 - forward);
        score += weights[static_cast<std::size_t>(pitchClass)] *
            relationship[static_cast<std::size_t>(distance)];
    }
    return score;
}

std::optional<double> drumResonanceShiftCents(
    double frequencyHz,
    int tonic,
    const QVector<int>& targetIntervals,
    const std::array<double, 12>& harmonyWeights)
{
    constexpr double maximumCents = 200.0;
    if (frequencyHz <= 0.0) return std::nullopt;
    const double sourceMidi = 69.0 + 12.0 * std::log2(frequencyHz / 440.0);
    double bestCents = 0.0;
    double bestScore = -std::numeric_limits<double>::infinity();
    bool found = false;
    for (int interval : targetIntervals) {
        const int pitchClass = wrappedPitchClass(tonic + interval);
        const double targetMidi = pitchClass + 12.0 * std::round(
            (sourceMidi - pitchClass) / 12.0);
        const double cents = 100.0 * (targetMidi - sourceMidi);
        if (std::abs(cents) > maximumCents + 0.001) continue;
        const double preference = interval == 0 ? 1.2 : interval == 7 ? 0.9 : 0.0;
        const double score = drumTargetHarmonyScore(pitchClass, harmonyWeights) +
            preference - 0.35 * std::abs(cents) / maximumCents;
        if (!found || score > bestScore + 1.0e-9 ||
            (std::abs(score - bestScore) <= 1.0e-9 &&
             std::abs(cents) < std::abs(bestCents))) {
            bestCents = cents;
            bestScore = score;
            found = true;
        }
    }
    return found ? std::optional<double>(bestCents) : std::nullopt;
}

ResearchDrumKit keyAwareDrumKit(
    const ResearchDrumKit& source,
    const SongSection& section)
{
    ResearchDrumKit kit = source;
    const GenerationRecipe& recipe = section.generatedRecipe;
    const ParsedChord tonic = parseChord(recipe.tonic);
    const QVector<int> targets = modeAwareDrumTargetIntervals(recipe.mode);
    if (!tonic.valid || targets.isEmpty()) return kit;
    const auto weights = drumHarmonyPitchWeights(section);
    const QStringList eligible = source.baseKitId == QStringLiteral("electronic")
        ? QStringList{QStringLiteral("kick"), QStringLiteral("snare"),
              QStringLiteral("high-tom"), QStringLiteral("mid-tom"),
              QStringLiteral("floor-tom"), QStringLiteral("ride")}
        : QStringList{QStringLiteral("kick"), QStringLiteral("high-tom"),
              QStringLiteral("mid-tom"), QStringLiteral("floor-tom"),
              QStringLiteral("ride")};
    for (const QString& pieceId : eligible) {
        auto found = kit.pieces.find(pieceId);
        if (found == kit.pieces.end() || found->modalBands.isEmpty()) continue;
        ResearchDrumPiece& piece = found.value();
        const auto anchor = std::max_element(
            piece.modalBands.cbegin(), piece.modalBands.cend(),
            [](const ResearchDrumModalBand& left, const ResearchDrumModalBand& right) {
                return left.level * std::sqrt(qMax(0.005f, left.decaySeconds)) <
                    right.level * std::sqrt(qMax(0.005f, right.decaySeconds));
            });
        const qsizetype anchorIndex = std::distance(
            piece.modalBands.cbegin(), anchor);
        const double anchorFrequency = anchor->frequencyHz *
            std::pow(2.0, anchor->detuneCents / 1200.0);
        const std::optional<double> cents = drumResonanceShiftCents(
            anchorFrequency, tonic.root, targets, weights);
        if (!cents) continue;
        const float ratio = static_cast<float>(std::pow(2.0, *cents / 1200.0));
        for (ResearchDrumModalBand& band : piece.modalBands) {
            band.detuneCents += static_cast<float>(*cents);
        }
        if (pieceId != QStringLiteral("ride") &&
            piece.sourceLayerGain > 0.0001f) {
            piece.frequencyHz *= ratio;
        }
        const double largeMove = std::clamp(
            (std::abs(*cents) - 100.0) / 100.0, 0.0, 1.0);
        ResearchDrumModalBand& movedAnchor =
            piece.modalBands[anchorIndex];
        movedAnchor.level *= static_cast<float>(1.0 - 0.12 * largeMove);
        movedAnchor.decaySeconds *= static_cast<float>(
            1.0 - 0.10 * largeMove);
    }
    return kit;
}

QVector<DrumEvent> drumEvents(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    const ResearchDrumKit& drumKit)
{
    QVector<DrumEvent> events;
    const double framesPerBeat = framesPerMusicalBeat(section, settings);
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const GenerationRecipe* feel = section.generatedRecipe.isValid() &&
        !section.generatedRecipe.drumEvents.isEmpty() &&
        generatedBeatFingerprint(section) ==
            section.generatedRecipe.beatFingerprint
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
    const bool usePerformance = feel;
    if (usePerformance) {
        const qint64 cycleTicks =
            qMax<qint64>(1, section.beats * kTicksPerBeat);
        const double generatedGain =
            std::pow(10.0, feel->drumMixGainDb / 20.0);
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
                event.velocity = performed.velocity;
                event.articulation = performed.articulation;
                event.laneId = performed.laneId;
                if (!researchDrumPiece(drumKit, performed.laneId)) continue;
                events.push_back(std::move(event));
            }
        }
        std::sort(
            events.begin(),
            events.end(),
            [](const DrumEvent& a, const DrumEvent& b) {
                return a.frame < b.frame;
            });
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
                event.laneId = BeatGridModel::beatLaneNames()
                    .value(lane).toLower();
                event.laneId.replace(QStringLiteral("closed hh"), QStringLiteral("closed_hat"));
                event.laneId.replace(QStringLiteral("open hh"), QStringLiteral("open_hat"));
                event.laneId.replace(QStringLiteral("high tom"), QStringLiteral("high_tom"));
                event.laneId.replace(QStringLiteral("mid tom"), QStringLiteral("mid_tom"));
                event.laneId.replace(QStringLiteral("floor tom"), QStringLiteral("floor_tom"));
                event.laneId.replace(QStringLiteral("cross-stick / rim"), QStringLiteral("cross_stick"));
                event.velocity = state == QLatin1Char('g') ? 38 :
                    state == QLatin1Char('a') ? 122 : 91;
                if (!researchDrumPiece(drumKit, event.laneId)) continue;
                events.push_back(event);
            }
        }
    }
    std::sort(events.begin(), events.end(), [](const DrumEvent& a, const DrumEvent& b) { return a.frame < b.frame; });
    return events;
}

ReferenceWav renderDrums(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& path,
    QString& error)
{
    const bool generatedPerformance =
        section.generatedRecipe.isValid() &&
        !section.generatedRecipe.drumEvents.isEmpty() &&
        generatedBeatFingerprint(section) ==
            section.generatedRecipe.beatFingerprint;
    const bool useStyleKit =
        settings.drumKit == ReferenceDrumKit::StyleDefault &&
        generatedPerformance &&
        researchDrumKitById(section.generatedRecipe.drumPatchId);
    const QString baseKitId =
        settings.drumKit == ReferenceDrumKit::Electronic
        ? QStringLiteral("electronic")
        : QStringLiteral("acoustic");
    const ResearchDrumKit* selectedKit = useStyleKit
        ? researchDrumKitById(section.generatedRecipe.drumPatchId)
        : researchDrumKitForBase(baseKitId);
    if (!selectedKit) {
        error = QStringLiteral("The selected drum kit is unavailable.");
        return {};
    }
    const ResearchDrumKit tunedKit = useStyleKit
        ? keyAwareDrumKit(*selectedKit, section)
        : *selectedKit;
    const QVector<DrumEvent> events = drumEvents(
        section, settings, commonBeats, tunedKit);
    if (events.isEmpty()) {
        error = QStringLiteral("The generated beat contains no renderable drum hits.");
        return {};
    }

    QVector<float> rendered(
        static_cast<qsizetype>(totalFrames),
        0.0f);
    QVector<float> researchRoomSend(
        static_cast<qsizetype>(totalFrames),
        0.0f);
    QVector<ResearchDrumRenderEvent> researchEvents;
    researchEvents.reserve(events.size());
    for (const DrumEvent& event : events) {
        researchEvents.push_back({
            event.frame,
            event.laneId,
            event.articulation,
            event.velocity,
            0,
            event.noiseSeed,
        });
    }
    const ResearchDrumRenderResult research = renderResearchDrumVoices(
        tunedKit, researchEvents, totalFrames, settings.sampleRate);
    const double generatedGain = generatedPerformance
        ? std::pow(10.0, section.generatedRecipe.drumMixGainDb / 20.0)
        : 1.0;
    const double researchScale = settings.drumLevel * generatedGain;
    for (qsizetype frame = 0;
         frame < rendered.size() && frame < research.dry.size(); ++frame) {
        rendered[frame] = static_cast<float>(
            researchScale * research.dry.at(frame));
        if (frame < research.roomSend.size()) {
            researchRoomSend[frame] = static_cast<float>(
                researchScale * research.roomSend.at(frame));
        }
    }
    applyResearchDrumBus(
        rendered,
        researchRoomSend,
        tunedKit.bus,
        settings.sampleRate);
    const bool generated = generatedPerformance;
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
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                {QStringLiteral("articulation"), step.articulation},
                {QStringLiteral("voicing"), step.voicing}});
        }
        for (const MusicalStep& step : pattern.melody) {
            melodySteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                {QStringLiteral("articulation"), step.articulation}});
        }
        for (const MusicalStep& step : pattern.bass) {
            bassSteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                {QStringLiteral("articulation"), step.articulation}});
        }
        for (const MusicalStep& step : pattern.support) {
            supportSteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                {QStringLiteral("articulation"), step.articulation}});
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

ResearchDrumKit keyAwareResearchDrumKit(
    const ResearchDrumKit& source,
    const SongSection& section)
{
    return keyAwareDrumKit(source, section);
}

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
                {QStringLiteral("voicing"), step.voicing},
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
        {QStringLiteral("drum_kit"), static_cast<int>(settings.drumKit)},
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
    const QString folder = QDir(workspaceFolder).absoluteFilePath(QStringLiteral("generated"));
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
    enum class Stem { Chords, Drums, Melody, Bass, Support };
    struct StemTask {
        Stem stem = Stem::Chords;
        std::function<ReferenceWav(QString&)> render;
    };
    struct StemOutcome {
        ReferenceWav wav;
        QString error;
        qint64 elapsedMs = 0;
    };
    std::vector<StemTask> tasks;
    tasks.reserve(5);
    if (settings.renderChords) {
        tasks.push_back({Stem::Chords, [=](QString& error) {
            return renderChords(
                *chordSection, settings, commonBeats, frames, chordPath, error);
        }});
    }
    if (settings.renderDrums) {
        tasks.push_back({Stem::Drums, [=](QString& error) {
            return renderDrums(
                *beatSection, settings, commonBeats, frames, drumPath, error);
        }});
    }
    if (settings.renderMelody) {
        tasks.push_back({Stem::Melody, [=](QString& error) {
            return renderNoteLane(
                *chordSection, settings, commonBeats, frames,
                QStringLiteral("melody"), settings.melodyLevel, melodyPath, error);
        }});
    }
    if (settings.renderBass) {
        tasks.push_back({Stem::Bass, [=](QString& error) {
            return renderNoteLane(
                *chordSection, settings, commonBeats, frames,
                QStringLiteral("bass"), settings.bassLevel, bassPath, error);
        }});
    }
    if (settings.renderSupport) {
        tasks.push_back({Stem::Support, [=](QString& error) {
            return renderNoteLane(
                *chordSection, settings, commonBeats, frames,
                QStringLiteral("support"), settings.supportLevel, supportPath, error);
        }});
    }

    std::vector<StemOutcome> outcomes(tasks.size());
    std::atomic_size_t nextTask{0};
    const unsigned reportedThreads = std::thread::hardware_concurrency();
    const std::size_t workerLimit = reportedThreads > 0
        ? std::min<std::size_t>(reportedThreads, 5)
        : 1;
    const std::size_t requestedWorkers =
        std::max<std::size_t>(1, std::min(tasks.size(), workerLimit));
    const auto runTasks = [&] {
        for (;;) {
            const std::size_t index = nextTask.fetch_add(
                1, std::memory_order_relaxed);
            if (index >= tasks.size()) return;
            QElapsedTimer stemElapsed;
            stemElapsed.start();
            try {
                outcomes[index].wav =
                    tasks[index].render(outcomes[index].error);
            } catch (const std::exception& exception) {
                outcomes[index].error = QStringLiteral(
                    "Reference stem renderer failed: %1")
                    .arg(QString::fromUtf8(exception.what()));
            } catch (...) {
                outcomes[index].error = QStringLiteral(
                    "Reference stem renderer failed with an unknown error.");
            }
            outcomes[index].elapsedMs = stemElapsed.elapsed();
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(requestedWorkers > 0 ? requestedWorkers - 1 : 0);
    for (std::size_t index = 1; index < requestedWorkers; ++index) {
        try {
            workers.emplace_back(runTasks);
        } catch (...) {
            break;
        }
    }
    const std::size_t renderWorkerCount = workers.size() + 1;
    runTasks();
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    std::array<qint64, 5> stemElapsedMs{-1, -1, -1, -1, -1};
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const StemTask& task = tasks[index];
        const StemOutcome& outcome = outcomes[index];
        stemElapsedMs[static_cast<std::size_t>(task.stem)] =
            outcome.elapsedMs;
        if (result.error.isEmpty() && !outcome.error.isEmpty()) {
            result.error = outcome.error;
        }
        switch (task.stem) {
        case Stem::Chords: result.chords = outcome.wav; break;
        case Stem::Drums: result.drums = outcome.wav; break;
        case Stem::Melody: result.melody = outcome.wav; break;
        case Stem::Bass: result.bass = outcome.wav; break;
        case Stem::Support: result.support = outcome.wav; break;
        }
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
        QString resolvedDrumPatch = QStringLiteral("none");
        if (beatSection) {
            const bool generatedPerformance =
                beatSection->generatedRecipe.isValid() &&
                !beatSection->generatedRecipe.drumEvents.isEmpty() &&
                generatedBeatFingerprint(*beatSection) ==
                    beatSection->generatedRecipe.beatFingerprint;
            const bool useStyleKit =
                settings.drumKit == ReferenceDrumKit::StyleDefault &&
                generatedPerformance &&
                researchDrumKitById(beatSection->generatedRecipe.drumPatchId);
            resolvedDrumPatch = useStyleKit
                ? beatSection->generatedRecipe.drumPatchId
                : settings.drumKit == ReferenceDrumKit::Electronic
                    ? QStringLiteral("base:electronic")
                    : QStringLiteral("base:acoustic");
        }
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
            "melody_events=%14 bass_events=%15 support_events=%16 drum_events=%17 "
            "render_workers=%18 reported_threads=%19 chord_ms=%20 drum_ms=%21 melody_ms=%22 "
            "bass_ms=%23 support_ms=%24 elapsed_ms=%25 peak=%26 drum_rms=%27 "
            "drum_pre_makeup_peak=%28 drum_makeup_db=%29 drum_limited_samples=%30")
            .arg(recipe ? recipe->chordPatchId : QStringLiteral("manual"),
                 recipe ? recipe->melodyPatchId : QStringLiteral("manual"),
                 recipe ? recipe->bassPatchId : QStringLiteral("manual"),
                 recipe ? recipe->supportPatchId : QStringLiteral("manual"),
                 resolvedDrumPatch,
                 recipe ? recipe->profileId : QStringLiteral("manual"),
                 recipe ? recipe->grooveId : QStringLiteral("manual"))
            .arg(recipe ? recipe->swingPercent : 50)
            .arg(recipe ? recipe->snareOffsetMs : 0)
            .arg(recipe ? recipe->timingVariationMs : 0)
            .arg(recipe ? recipe->velocityVariationPercent : 0)
            .arg(maximumChordVoices).arg(result.chords.eventCount)
            .arg(result.melody.eventCount).arg(result.bass.eventCount)
            .arg(result.support.eventCount).arg(result.drums.eventCount)
            .arg(renderWorkerCount).arg(reportedThreads)
            .arg(stemElapsedMs[static_cast<std::size_t>(Stem::Chords)])
            .arg(stemElapsedMs[static_cast<std::size_t>(Stem::Drums)])
            .arg(stemElapsedMs[static_cast<std::size_t>(Stem::Melody)])
            .arg(stemElapsedMs[static_cast<std::size_t>(Stem::Bass)])
            .arg(stemElapsedMs[static_cast<std::size_t>(Stem::Support)])
            .arg(elapsed.elapsed()).arg(peak, 0, 'f', 4)
            .arg(result.drums.rms, 0, 'f', 5)
            .arg(result.drums.preMakeupPeak, 0, 'f', 4)
            .arg(result.drums.makeupGainDb, 0, 'f', 1)
            .arg(result.drums.limitedSamples);
    }
    return result;
}

} // namespace jam2::practice
