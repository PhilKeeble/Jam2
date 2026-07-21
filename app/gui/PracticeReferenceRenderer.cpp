#include "PracticeReferenceRenderer.hpp"

#include "MusicTheory.hpp"

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
#include <numeric>

namespace jam2::practice {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaximumSeconds = 300.0;
constexpr int kBlockFrames = 32768;
constexpr int kTicksPerBeat = 12;

struct DrumEvent {
    enum class Instrument { Kick, Snare, ClosedHat, OpenHat, Tom, Crash, Ride };
    qint64 frame = 0;
    Instrument instrument = Instrument::Kick;
    double level = 0.7;
    std::uint32_t noiseSeed = 1;
    int kit = 0;
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
};

SynthPatch synthPatch(const SongSection& section, bool melody)
{
    const QString id = melody ? section.generatedRecipe.melodyPatchId : section.generatedRecipe.chordPatchId;
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
    const QString mood = section.generatedRecipe.moodId;
    if (mood == QStringLiteral("bright")) patch.filter = qMin(0.72, patch.filter + 0.16);
    if (mood == QStringLiteral("dark")) patch.filter *= 0.58;
    if (mood == QStringLiteral("dreamy")) { patch.detuneCents += 4.0; patch.delayMix += 0.10; patch.filter *= 0.80; }
    if (mood == QStringLiteral("intense")) { patch.drive += 0.35; patch.filter += 0.10; }
    if (mood == QStringLiteral("peaceful")) { patch.drive *= 0.85; patch.filter *= 0.72; }
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
    return patch;
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

double patchTone(const SynthPatch& patch, double frequency, double seconds)
{
    double result = 0.0;
    for (int layer = 0; layer < patch.layers; ++layer) {
        const double centered = layer - (patch.layers - 1) * 0.5;
        const double ratio = std::pow(2.0, centered * patch.detuneCents / 1200.0);
        const double phase = 2.0 * kPi * frequency * ratio * seconds;
        result += oscillator(patch.wave, phase) + patch.secondHarmonic * std::sin(phase * 2.0);
    }
    return std::tanh(patch.drive * result / patch.layers);
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

QVector<int> voicedNotes(const ParsedChord& chord, ChordVoicing voicing, double previousCenter)
{
    QVector<int> notes;
    QVector<int> essential;
    for (int interval : chord.intervals) {
        if (essential.size() >= 4) break;
        if (interval == 0 || interval == 3 || interval == 4 || interval == 10 || interval == 11 ||
            interval == chord.intervals.back()) essential.push_back(interval);
    }
    for (int interval : chord.intervals) {
        if (essential.size() >= 4) break;
        if (!essential.contains(interval) && interval != 7) essential.push_back(interval);
    }
    if (essential.size() < 3 && chord.intervals.contains(7)) essential.push_back(7);
    int root = 48 + chord.root;
    if (voicing == ChordVoicing::Spread) root -= 12;
    for (int interval : essential) {
        int note = root + interval;
        if (voicing == ChordVoicing::Spread && interval > 0) note += interval < 7 ? 12 : 0;
        notes.push_back(note);
    }
    if (voicing == ChordVoicing::VoiceLed && previousCenter > 0.0) {
        for (int& note : notes) {
            while (note - previousCenter > 6.0) note -= 12;
            while (previousCenter - note > 6.0) note += 12;
        }
    }
    if (chord.bass >= 0) {
        int bass = 36 + chord.bass;
        while (bass >= 48) bass -= 12;
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
        voicing = style == QStringLiteral("jazz") || style == QStringLiteral("rnb-soul")
            ? ChordVoicing::VoiceLed
            : style == QStringLiteral("modal-vamp") || style == QStringLiteral("edm")
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
    const SynthPatch patch = synthPatch(section, false);
    const double filterAmount = filterCoefficient(patch, settings.sampleRate);
    const double framesPerBeat = settings.sampleRate * 60.0 / settings.bpm;
    const qint64 attackFrames = static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0);
    const qint64 releaseFrames = static_cast<qint64>(settings.releaseMs * settings.sampleRate / 1000.0);
    double filtered = 0.0;
    QVector<float> delay(qMax(1, static_cast<int>(settings.sampleRate * 0.125)), 0.0f);
    float renderedPeak = 0.0f;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (int offset = 0; offset < count; ++offset) {
            const qint64 frame = first + offset;
            const qint64 absoluteTick = qMin(commonBeats * kTicksPerBeat - 1,
                static_cast<qint64>(frame * kTicksPerBeat / framesPerBeat));
            const int tick = static_cast<int>(absoluteTick % (section.beats * kTicksPerBeat));
            const QVector<int>& notes = voicings.at(tick);
            if (notes.isEmpty()) continue;
            const qint64 tickStart = static_cast<qint64>(absoluteTick * framesPerBeat / kTicksPerBeat);
            const qint64 tickEnd = static_cast<qint64>((absoluteTick + 1) * framesPerBeat / kTicksPerBeat);
            const QVector<int>& previous = voicings.at((tick - 1 + voicings.size()) % voicings.size());
            const QVector<int>& next = voicings.at((tick + 1) % voicings.size());
            double envelope = 1.0;
            if (notes != previous && attackFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(frame - tickStart) / attackFrames);
            }
            if (notes != next && releaseFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(tickEnd - frame) / releaseFrames);
            }
            double value = 0.0;
            const double seconds = static_cast<double>(frame) / settings.sampleRate;
            for (int note : notes) {
                value += patchTone(patch, midiFrequency(note), seconds);
            }
            value = settings.chordLevel * envelope * value / notes.size();
            filtered += filterAmount * (value - filtered);
            const int delayIndex = static_cast<int>(frame % delay.size());
            const double effected = filtered + patch.delayMix * delay.at(delayIndex);
            delay[delayIndex] = static_cast<float>(filtered);
            block[offset] = static_cast<float>(effected);
            renderedPeak = qMax(renderedPeak, std::abs(block[offset]));
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
    return {path, fileHash(path), totalFrames, renderedPeak, eventCount};
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
    const SynthPatch patch = synthPatch(section, true);
    const double framesPerBeat = settings.sampleRate * 60.0 / settings.bpm;
    const qint64 attackFrames = static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0);
    const qint64 releaseFrames = static_cast<qint64>(settings.releaseMs * settings.sampleRate / 1000.0);
    double filtered = 0.0;
    QVector<float> delay(qMax(1, static_cast<int>(settings.sampleRate * 0.14)), 0.0f);
    float renderedPeak = 0.0f;
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
            const double value = settings.melodyLevel * envelope * patchTone(patch, midiFrequency(note), seconds);
            filtered += std::clamp(patch.filter, 0.05, 0.90) * (value - filtered);
            const int delayIndex = static_cast<int>(frame % delay.size());
            block[offset] = static_cast<float>(filtered + patch.delayMix * delay.at(delayIndex));
            delay[delayIndex] = static_cast<float>(filtered);
            renderedPeak = qMax(renderedPeak, std::abs(block[offset]));
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
    return {path, fileHash(path), totalFrames, renderedPeak, eventCount};
}

struct MelodyRenderEvent {
    qint64 frame = 0;
    qint64 durationFrames = 1;
    int midi = 60;
    int velocity = 88;
};

bool melodyRenderEvents(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    QVector<MelodyRenderEvent>& output,
    QString& error)
{
    struct TickEvent { int tick; int duration; int midi; int velocity; };
    QVector<TickEvent> base;
    int active = -1;
    const auto closeAt = [&base, &active](int tick) {
        if (active >= 0) {
            base[active].duration = qMax(1, tick - base[active].tick);
            active = -1;
        }
    };
    for (int beat = 0; beat < section.beats; ++beat) {
        const bool timed = beat < section.musicalPatterns.size() &&
            section.musicalPatterns[beat].division > 0 &&
            section.musicalPatterns[beat].melody.size() == section.musicalPatterns[beat].division;
        const int division = timed ? section.musicalPatterns[beat].division : 1;
        for (int stepIndex = 0; stepIndex < division; ++stepIndex) {
            const int tick = beat * kTicksPerBeat + stepIndex * kTicksPerBeat / division;
            MusicalStep step;
            if (timed) step = section.musicalPatterns[beat].melody[stepIndex];
            else {
                const QString legacy = section.targets.value(beat).trimmed();
                step.state = legacy.isEmpty() ? MusicalStepState::Hold
                    : legacy == QStringLiteral("-") ? MusicalStepState::Rest
                                                    : MusicalStepState::Onset;
                step.value = legacy;
            }
            if (step.state == MusicalStepState::Rest) {
                closeAt(tick);
            } else if (step.state == MusicalStepState::Onset) {
                closeAt(tick);
                const std::optional<int> midi = parseMidiNote(step.value);
                if (!midi) {
                    error = QStringLiteral("Unsupported melody note at beat %1, step %2: %3")
                        .arg(beat + 1).arg(stepIndex + 1).arg(step.value);
                    return false;
                }
                base.push_back({tick, 1, *midi, qBound(1, step.velocity, 127)});
                active = base.size() - 1;
            }
        }
    }
    closeAt(section.beats * kTicksPerBeat);
    if (base.isEmpty()) {
        error = QStringLiteral("The generated section contains no renderable melody notes.");
        return false;
    }
    const double framesPerTick = settings.sampleRate * 60.0 /
        (settings.bpm * kTicksPerBeat);
    for (qint64 cycleBeat = 0; cycleBeat < commonBeats; cycleBeat += section.beats) {
        const qint64 cycleTick = cycleBeat * kTicksPerBeat;
        for (const TickEvent& event : base) {
            output.push_back({
                static_cast<qint64>((cycleTick + event.tick) * framesPerTick),
                qMax<qint64>(1, static_cast<qint64>(event.duration * framesPerTick)),
                event.midi,
                event.velocity,
            });
        }
    }
    return true;
}

ReferenceWav renderMelody(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats,
    qint64 totalFrames,
    const QString& path,
    QString& error)
{
    QVector<MelodyRenderEvent> events;
    if (!melodyRenderEvents(section, settings, commonBeats, events, error)) return {};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create melody reference WAV.");
        return {};
    }
    writeWavHeader(file, settings.sampleRate, totalFrames);
    const SynthPatch patch = synthPatch(section, true);
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
                const double seconds = static_cast<double>(age) / settings.sampleRate;
                const double vibratoRamp = std::clamp((seconds - 0.12) / 0.16, 0.0, 1.0);
                const double vibratoCents = vibratoRamp * 4.0 * std::sin(2.0 * kPi * 5.1 * seconds);
                const double frequency = midiFrequency(event.midi) * std::pow(2.0, vibratoCents / 1200.0);
                block[static_cast<int>(frame - first)] += static_cast<float>(
                    settings.melodyLevel * (event.velocity / 127.0) * envelope *
                    patchTone(patch, frequency, seconds));
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
        if (!writeBlock(file, block)) {
            error = QStringLiteral("Cannot write melody reference WAV.");
            return {};
        }
    }
    if (!file.commit()) {
        error = QStringLiteral("Cannot finalize melody reference WAV.");
        return {};
    }
    return {path, fileHash(path), totalFrames, renderedPeak, static_cast<int>(events.size())};
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
    if (name == QStringLiteral("Tom")) return DrumEvent::Instrument::Tom;
    if (name == QStringLiteral("Crash")) return DrumEvent::Instrument::Crash;
    if (name == QStringLiteral("Ride")) return DrumEvent::Instrument::Ride;
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
    const double framesPerBeat = settings.sampleRate * 60.0 / settings.bpm;
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const GenerationRecipe* feel = section.generatedRecipe.isValid()
        ? &section.generatedRecipe : nullptr;
    const int swingPercent = feel ? feel->swingPercent : 50;
    const int snareOffsetMs = feel ? feel->snareOffsetMs : 0;
    const int timingVariationMs = feel ? feel->timingVariationMs : 0;
    const int velocityVariationPercent = feel ? feel->velocityVariationPercent : 0;
    const qint64 maximumFrame = qMax<qint64>(0,
        static_cast<qint64>(std::ceil(commonBeats * framesPerBeat)) - 1);
    int kit = 0;
    const QString kitId = section.generatedRecipe.drumPatchId;
    if (kitId.contains(QStringLiteral("808"))) kit = 3;
    else if (kitId.contains(QStringLiteral("electronic")) || kitId.contains(QStringLiteral("punch"))) kit = 2;
    else if (kitId.contains(QStringLiteral("brush")) || kitId.contains(QStringLiteral("pocket"))) kit = 1;
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
                    const int pairStart = step - 1;
                    stepPosition = (pairStart + 2.0 * swingPercent / 100.0) / pattern.division;
                }
                double frame = (absoluteBeat + stepPosition) * framesPerBeat;
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
    const double t = static_cast<double>(age) / sampleRate;
    switch (event.instrument) {
    case DrumEvent::Instrument::Kick:
        return std::sin(2.0 * kPi * ((event.kit == 3 ? 48.0 : event.kit == 2 ? 68.0 : 62.0) -
            24.0 * qMin(1.0, t * 8.0)) * t) * std::exp(-t * (event.kit == 3 ? 4.5 : 11.0));
    case DrumEvent::Instrument::Snare:
        return ((event.kit == 1 ? 0.52 : 0.78) * noiseAt(event.noiseSeed, age) +
            (event.kit == 2 ? 0.34 : 0.22) * std::sin(2.0 * kPi * (event.kit == 2 ? 220.0 : 180.0) * t)) *
            std::exp(-t * (event.kit == 1 ? 25.0 : 18.0));
    case DrumEvent::Instrument::ClosedHat:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.6) * std::exp(-t * 55.0);
    case DrumEvent::Instrument::OpenHat:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.6) * std::exp(-t * 10.0);
    case DrumEvent::Instrument::Tom:
        return std::sin(2.0 * kPi * 115.0 * t) * std::exp(-t * 9.0);
    case DrumEvent::Instrument::Crash:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.4) * std::exp(-t * 3.5);
    case DrumEvent::Instrument::Ride: {
        const double bell = 0.18 * std::sin(2.0 * kPi * 920.0 * t) +
            0.08 * std::sin(2.0 * kPi * 1370.0 * t);
        const double wash = 0.14 * noiseAt(event.noiseSeed, age) *
            ((age & 1) ? 1.0 : -0.35);
        return (bell + wash) * std::exp(-t * 7.0);
    }
    }
    return 0.0;
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
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create drum reference WAV.");
        return {};
    }
    writeWavHeader(file, settings.sampleRate, totalFrames);
    const qint64 maximumTail = static_cast<qint64>(settings.sampleRate * 2.0);
    float renderedPeak = 0.0f;
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (const DrumEvent& event : events) {
            if (event.frame >= first + count) break;
            if (event.frame + maximumTail < first) continue;
            const qint64 begin = qMax(first, event.frame);
            const qint64 end = qMin(first + count, event.frame + maximumTail);
            for (qint64 frame = begin; frame < end; ++frame) {
                block[static_cast<int>(frame - first)] += static_cast<float>(
                    event.level * drumSample(event, frame - event.frame, settings.sampleRate));
            }
        }
        for (float sample : block) renderedPeak = qMax(renderedPeak, std::abs(sample));
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
    return {path, fileHash(path), totalFrames, renderedPeak, static_cast<int>(events.size())};
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
        for (const MusicalStep& step : pattern.chords) {
            chordSteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
        }
        for (const MusicalStep& step : pattern.melody) {
            melodySteps.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
        }
        musicalPatterns.append(QJsonObject{{QStringLiteral("division"), pattern.division},
            {QStringLiteral("chords"), chordSteps}, {QStringLiteral("melody"), melodySteps}});
    }
    return {{QStringLiteral("id"), section->id}, {QStringLiteral("beats"), section->beats},
        {QStringLiteral("chords"), chords}, {QStringLiteral("melody"), melody},
        {QStringLiteral("patterns"), patterns}, {QStringLiteral("musical_patterns"), musicalPatterns},
        {QStringLiteral("generated_recipe"), section->generatedRecipe.isValid()
            ? generationRecipeToJson(section->generatedRecipe) : QJsonObject{}}};
}

} // namespace

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
        {QStringLiteral("bpm"), settings.bpm},
        {QStringLiteral("rate"), settings.sampleRate},
        {QStringLiteral("voicing"), static_cast<int>(settings.voicing)},
        {QStringLiteral("chord_level"), settings.chordLevel},
        {QStringLiteral("drum_level"), settings.drumLevel},
        {QStringLiteral("melody_level"), settings.melodyLevel},
        {QStringLiteral("attack_ms"), settings.attackMs},
        {QStringLiteral("release_ms"), settings.releaseMs},
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
    if ((!settings.renderChords && !settings.renderDrums && !settings.renderMelody) ||
        settings.sampleRate < 8000 ||
        settings.bpm < 20.0 || settings.bpm > 400.0 ||
        (settings.renderChords && (!chordSection || chordSection->beats <= 0)) ||
        (settings.renderDrums && (!beatSection || beatSection->beats <= 0)) ||
        (settings.renderMelody && (!chordSection || chordSection->beats <= 0))) {
        result.error = QStringLiteral("Reference render settings or source layers are invalid.");
        return result;
    }
    const qint64 chordBeats = settings.renderChords ? chordSection->beats : 0;
    const qint64 beatBeats = settings.renderDrums ? beatSection->beats : 0;
    const qint64 melodyBeats = settings.renderMelody ? chordSection->beats : 0;
    qint64 commonBeats = 0;
    for (qint64 beats : {chordBeats, beatBeats, melodyBeats}) {
        if (beats <= 0) continue;
        commonBeats = commonBeats > 0 ? std::lcm(commonBeats, beats) : beats;
    }
    const double seconds = commonBeats * 60.0 / settings.bpm;
    if (commonBeats <= 0 || seconds > kMaximumSeconds) {
        result.error = QStringLiteral("The common reference length exceeds the five-minute limit.");
        return result;
    }
    const qint64 frames = static_cast<qint64>(std::ceil(seconds * settings.sampleRate));
    const QString folder = QDir(workspaceFolder).absoluteFilePath(QStringLiteral("wavs"));
    if (!QDir().mkpath(folder)) {
        result.error = QStringLiteral("Cannot create the reference WAV folder.");
        return result;
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString chordPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-chords-%1.wav").arg(token));
    const QString drumPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-drums-%1.wav").arg(token));
    const QString melodyPath = QDir(folder).absoluteFilePath(QStringLiteral("practice-melody-%1.wav").arg(token));
    result.sourceSignature = practiceIdeaSignature(chordSection, beatSection);
    if (settings.renderChords) {
        result.chords = renderChords(*chordSection, settings, commonBeats, frames, chordPath, result.error);
    }
    if (result.error.isEmpty() && settings.renderDrums) {
        result.drums = renderDrums(*beatSection, settings, commonBeats, frames, drumPath, result.error);
    }
    if (result.error.isEmpty() && settings.renderMelody) {
        result.melody =
            renderMelody(*chordSection, settings, commonBeats, frames, melodyPath, result.error);
    }
    if (!result.error.isEmpty() || (settings.renderChords && result.chords.sha256.isEmpty()) ||
        (settings.renderDrums && result.drums.sha256.isEmpty()) ||
        (settings.renderMelody && result.melody.sha256.isEmpty())) {
        if (!result.chords.path.isEmpty()) QFile::remove(result.chords.path);
        if (!result.drums.path.isEmpty()) QFile::remove(result.drums.path);
        if (!result.melody.path.isEmpty()) QFile::remove(result.melody.path);
        if (result.error.isEmpty()) result.error = QStringLiteral("A rendered WAV could not be validated.");
    } else {
        const GenerationRecipe* recipe = chordSection && chordSection->generatedRecipe.isValid()
            ? &chordSection->generatedRecipe
            : beatSection && beatSection->generatedRecipe.isValid() ? &beatSection->generatedRecipe : nullptr;
        const float peak = qMax(result.chords.peak, qMax(result.drums.peak, result.melody.peak));
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
            "reference render: chord_patch=%1 melody_patch=%2 drum_patch=%3 "
            "groove=%4 swing_percent=%5 snare_offset_ms=%6 timing_variation_ms=%7 "
            "velocity_variation_percent=%8 max_chord_voices=%9 chord_events=%10 "
            "melody_events=%11 drum_events=%12 elapsed_ms=%13 peak=%14")
            .arg(recipe ? recipe->chordPatchId : QStringLiteral("manual"),
                 recipe ? recipe->melodyPatchId : QStringLiteral("manual"),
                 recipe ? recipe->drumPatchId : QStringLiteral("manual"),
                 recipe ? recipe->grooveId : QStringLiteral("manual"))
            .arg(recipe ? recipe->swingPercent : 50)
            .arg(recipe ? recipe->snareOffsetMs : 0)
            .arg(recipe ? recipe->timingVariationMs : 0)
            .arg(recipe ? recipe->velocityVariationPercent : 0)
            .arg(maximumChordVoices).arg(result.chords.eventCount)
            .arg(result.melody.eventCount).arg(result.drums.eventCount)
            .arg(elapsed.elapsed()).arg(peak, 0, 'f', 4);
    }
    return result;
}

} // namespace jam2::practice
