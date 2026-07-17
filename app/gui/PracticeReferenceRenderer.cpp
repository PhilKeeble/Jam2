#include "PracticeReferenceRenderer.hpp"

#include "MusicTheory.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
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

struct DrumEvent {
    enum class Instrument { Kick, Snare, ClosedHat, OpenHat, Tom, Crash };
    qint64 frame = 0;
    Instrument instrument = Instrument::Kick;
    double level = 0.7;
    std::uint32_t noiseSeed = 1;
};

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
    int root = 48 + chord.root;
    if (voicing == ChordVoicing::Spread) root -= 12;
    for (int interval : chord.intervals) {
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
    return notes;
}

bool resolvedChordVoicings(
    const SongSection& section,
    ChordVoicing voicing,
    QVector<QVector<int>>& out,
    QString& error)
{
    out.resize(section.beats);
    QVector<int> current;
    double previousCenter = 0.0;
    for (int beat = 0; beat < section.beats; ++beat) {
        const QString symbol = section.chords.value(beat).trimmed();
        if (symbol.isEmpty()) {
            out[beat] = current;
            continue;
        }
        const ParsedChord parsed = parseChord(symbol);
        if (!parsed.valid) {
            error = QStringLiteral("Unsupported chord symbol at beat %1: %2").arg(beat + 1).arg(symbol);
            return false;
        }
        if (parsed.rest) {
            current.clear();
        } else {
            current = voicedNotes(parsed, voicing, previousCenter);
            double total = 0.0;
            for (int note : current) total += note;
            previousCenter = current.isEmpty() ? previousCenter : total / current.size();
        }
        out[beat] = current;
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
    const double framesPerBeat = settings.sampleRate * 60.0 / settings.bpm;
    const qint64 attackFrames = static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0);
    const qint64 releaseFrames = static_cast<qint64>(settings.releaseMs * settings.sampleRate / 1000.0);
    for (qint64 first = 0; first < totalFrames; first += kBlockFrames) {
        const int count = static_cast<int>(qMin<qint64>(kBlockFrames, totalFrames - first));
        QVector<float> block(count, 0.0f);
        for (int offset = 0; offset < count; ++offset) {
            const qint64 frame = first + offset;
            const qint64 absoluteBeat = qMin(commonBeats - 1, static_cast<qint64>(frame / framesPerBeat));
            const int beat = static_cast<int>(absoluteBeat % section.beats);
            const QVector<int>& notes = voicings.at(beat);
            if (notes.isEmpty()) continue;
            const qint64 beatStart = static_cast<qint64>(absoluteBeat * framesPerBeat);
            const qint64 beatEnd = static_cast<qint64>((absoluteBeat + 1) * framesPerBeat);
            const QVector<int>& previous = voicings.at((beat - 1 + section.beats) % section.beats);
            const QVector<int>& next = voicings.at((beat + 1) % section.beats);
            double envelope = 1.0;
            if (notes != previous && attackFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(frame - beatStart) / attackFrames);
            }
            if (notes != next && releaseFrames > 0) {
                envelope = qMin(envelope, static_cast<double>(beatEnd - frame) / releaseFrames);
            }
            double value = 0.0;
            const double seconds = static_cast<double>(frame) / settings.sampleRate;
            for (int note : notes) {
                const double phase = 2.0 * kPi * midiFrequency(note) * seconds;
                value += std::sin(phase) + 0.18 * std::sin(phase * 2.0);
            }
            block[offset] = static_cast<float>(settings.chordLevel * envelope * value / notes.size());
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
    return {path, fileHash(path), totalFrames};
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

ReferenceWav renderMelody(
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
    const double framesPerBeat = settings.sampleRate * 60.0 / settings.bpm;
    const qint64 attackFrames = static_cast<qint64>(settings.attackMs * settings.sampleRate / 1000.0);
    const qint64 releaseFrames = static_cast<qint64>(settings.releaseMs * settings.sampleRate / 1000.0);
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
            const double phase = 2.0 * kPi * midiFrequency(note) * seconds;
            const double value = std::sin(phase) + 0.12 * std::sin(phase * 2.0);
            block[offset] = static_cast<float>(settings.melodyLevel * envelope * value);
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
    return {path, fileHash(path), totalFrames};
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
    if (name == QStringLiteral("Crash") || name == QStringLiteral("Splash") ||
        name == QStringLiteral("Cymbal")) return DrumEvent::Instrument::Crash;
    return std::nullopt;
}

QVector<DrumEvent> drumEvents(
    const SongSection& section,
    const ReferenceRenderSettings& settings,
    qint64 commonBeats)
{
    QVector<DrumEvent> events;
    const double framesPerBeat = settings.sampleRate * 60.0 / settings.bpm;
    const QStringList lanes = BeatGridModel::beatLaneNames();
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
                event.frame = static_cast<qint64>((absoluteBeat + static_cast<double>(step) / pattern.division) * framesPerBeat);
                event.instrument = *instrument;
                event.level = stateLevel(state) * settings.drumLevel;
                event.noiseSeed = static_cast<std::uint32_t>((absoluteBeat + 1) * 2654435761ULL + lane * 131 + step);
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
        return std::sin(2.0 * kPi * (62.0 - 24.0 * qMin(1.0, t * 8.0)) * t) * std::exp(-t * 11.0);
    case DrumEvent::Instrument::Snare:
        return (0.78 * noiseAt(event.noiseSeed, age) + 0.22 * std::sin(2.0 * kPi * 180.0 * t)) * std::exp(-t * 18.0);
    case DrumEvent::Instrument::ClosedHat:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.6) * std::exp(-t * 55.0);
    case DrumEvent::Instrument::OpenHat:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.6) * std::exp(-t * 10.0);
    case DrumEvent::Instrument::Tom:
        return std::sin(2.0 * kPi * 115.0 * t) * std::exp(-t * 9.0);
    case DrumEvent::Instrument::Crash:
        return noiseAt(event.noiseSeed, age) * ((age & 1) ? 1.0 : -0.4) * std::exp(-t * 3.5);
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
    return {path, fileHash(path), totalFrames};
}

QJsonObject sectionSignature(const SongSection* section)
{
    if (!section) return {};
    QJsonArray chords;
    QJsonArray melody;
    QJsonArray patterns;
    for (const QString& chord : section->chords) chords.append(chord);
    for (const QString& note : section->targets) melody.append(note);
    for (const BeatPattern& pattern : section->beatPatterns) {
        QJsonArray lanes;
        for (const QString& lane : pattern.lanes) lanes.append(lane);
        patterns.append(QJsonObject{{QStringLiteral("division"), pattern.division}, {QStringLiteral("lanes"), lanes}});
    }
    return {{QStringLiteral("id"), section->id}, {QStringLiteral("beats"), section->beats},
        {QStringLiteral("chords"), chords}, {QStringLiteral("melody"), melody},
        {QStringLiteral("patterns"), patterns}};
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
    }
    return result;
}

} // namespace jam2::practice
