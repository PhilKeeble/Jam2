#include "DaisyShowcase.hpp"
#include "MusicTheory.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "PracticeReferenceRenderer.hpp"
#include "ResearchDrumKit.hpp"
#include "StyleProfileCatalog.hpp"

#include "Control/adsr.h"
#include "Drums/analogbassdrum.h"
#include "Drums/analogsnaredrum.h"
#include "Drums/hihat.h"
#include "Drums/synthbassdrum.h"
#include "Drums/synthsnaredrum.h"
#include "Effects/overdrive.h"
#include "Effects/chorus.h"
#include "Effects/wavefolder.h"
#include "Filters/ladder.h"
#include "Filters/svf.h"
#include "Noise/particle.h"
#include "Noise/whitenoise.h"
#include "PhysicalModeling/modalvoice.h"
#include "PhysicalModeling/stringvoice.h"
#include "Synthesis/fm2.h"
#include "Synthesis/formantosc.h"
#include "Synthesis/harmonic_osc.h"
#include "Synthesis/oscillator.h"
#include "Synthesis/variablesawosc.h"
#include "Synthesis/variableshapeosc.h"
#include "Synthesis/vosim.h"
#include "Synthesis/zoscillator.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using jam2::practice::ChordIdeaRequest;
using jam2::practice::ChordVoicing;
using jam2::practice::GeneratedPracticeIdea;
using jam2::practice::LaneTimingRecipe;
using jam2::practice::ParsedChord;
using jam2::practice::ProfileDefinition;
using jam2::practice::ReferenceRenderSettings;
using jam2::practice::ResearchDrumPiece;
using jam2::practice::ResearchDrumKit;
using jam2::practice::ResearchDrumRenderEvent;
using jam2::practice::ResearchDrumRenderResult;
using jam2::practice::StyleDefinition;

constexpr int kSampleRate = 48000;
constexpr int kTicksPerBeat = 12;
constexpr double kPi = 3.14159265358979323846;
constexpr int kAuditionBars = 4;
constexpr std::array<const char*, 5> kRoleIds{
    "chords", "melody", "bass", "support", "drums"};

struct NoteEvent {
    qint64 start = 0;
    qint64 end = 1;
    int midi = 60;
    int velocity = 88;
    QString role;
    QString articulation;
};

struct StemSet {
    std::vector<float> chords;
    std::vector<float> melody;
    std::vector<float> bass;
    std::vector<float> support;
    std::vector<float> drums;
};

double midiFrequency(int midi)
{
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
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

double deterministicUnit(std::uint32_t seed, std::uint32_t salt)
{
    std::uint32_t value = seed ^ salt;
    value ^= value >> 16;
    value *= 2246822519U;
    value ^= value >> 13;
    return static_cast<double>(value) / 2147483647.5 - 1.0;
}

std::uint32_t stableSeed(const QString& text)
{
    std::uint32_t value = 2166136261U;
    for (const QChar character : text) {
        value ^= character.unicode();
        value *= 16777619U;
    }
    return value ^ 0x4a326b1dU;
}

double framesPerBeat(const GeneratedPracticeIdea& idea)
{
    return kSampleRate * 60.0 / idea.bpm /
        std::max(1, idea.tempoPulseUnits);
}

qint64 frameAtTick(const GeneratedPracticeIdea& idea, double tick)
{
    return static_cast<qint64>(
        std::llround(tick * framesPerBeat(idea) / kTicksPerBeat));
}

void trimSection(SongSection& section, int beats)
{
    section.beats = std::clamp(beats, 1, section.beats);
    section.chords.resize(
        std::min<qsizetype>(section.chords.size(), section.beats));
    section.targets.resize(
        std::min<qsizetype>(section.targets.size(), section.beats));
    section.beatNotes.resize(
        std::min<qsizetype>(section.beatNotes.size(), section.beats));
    section.lyrics.resize(
        std::min<qsizetype>(section.lyrics.size(), section.beats));
    section.beatPatterns.resize(
        std::min<qsizetype>(section.beatPatterns.size(), section.beats));
    section.musicalPatterns.resize(
        std::min<qsizetype>(section.musicalPatterns.size(), section.beats));
}

GeneratedPracticeIdea trimIdea(GeneratedPracticeIdea idea)
{
    const int beats = std::min(
        idea.chordSection.beats,
        std::max(1, idea.meterNumerator) * kAuditionBars);
    trimSection(idea.chordSection, beats);
    trimSection(idea.beatSection, beats);
    // Preserve the production recipe and its exact performance events while
    // limiting only the audible window. Re-fingerprint the trimmed sections
    // so both the Jam2 renderer and the editable Lab continue to use the
    // generated drummer data instead of falling back to the display grid.
    idea.recipe.chordFingerprint =
        jam2::practice::generatedChordFingerprint(
            idea.chordSection);
    idea.recipe.beatFingerprint =
        jam2::practice::generatedBeatFingerprint(
            idea.beatSection);
    idea.chordSection.generatedRecipe = idea.recipe;
    idea.beatSection.generatedRecipe = idea.recipe;
    return idea;
}

const LaneTimingRecipe* laneTiming(
    const GeneratedPracticeIdea& idea,
    const QString& lane)
{
    for (const LaneTimingRecipe& timing : idea.recipe.laneTiming) {
        if (timing.laneId == lane) return &timing;
    }
    return nullptr;
}

qint64 timingOffsetFrames(
    const GeneratedPracticeIdea& idea,
    const QString& lane,
    int tick)
{
    const LaneTimingRecipe* timing = laneTiming(idea, lane);
    if (!timing) return 0;
    const double milliseconds = timing->offsetMs +
        deterministicUnit(
            idea.recipe.seed,
            static_cast<std::uint32_t>(tick * 131 + lane.size() * 17)) *
            timing->varianceMs;
    return static_cast<qint64>(
        std::llround(milliseconds * kSampleRate / 1000.0));
}

std::vector<int> chordNotes(
    const ParsedChord& chord,
    const QString& styleId,
    double previousCenter)
{
    std::vector<int> essential;
    const bool spread =
        styleId == QStringLiteral("modal-jam") ||
        styleId == QStringLiteral("electronic");
    const bool voiceLed =
        styleId == QStringLiteral("jazz") ||
        styleId == QStringLiteral("rnb-soul") ||
        styleId == QStringLiteral("bossa-nova");
    const auto addEssential = [&](int interval) {
        if (essential.size() < 4 && chord.intervals.contains(interval) &&
            std::find(essential.begin(), essential.end(), interval) ==
                essential.end()) {
            essential.push_back(interval);
        }
    };
    const bool hasSeventh =
        chord.intervals.contains(10) || chord.intervals.contains(11);
    if (voiceLed && hasSeventh) {
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
            if (interval == 0 || interval == 3 || interval == 4 ||
                interval == 10 || interval == 11 ||
                interval == chord.intervals.back()) {
                addEssential(interval);
            }
        }
        for (int interval : chord.intervals) {
            if (essential.size() >= 4) break;
            if (interval != 7) addEssential(interval);
        }
        if (essential.size() < 3) addEssential(7);
    }

    const int root = 48 + chord.root - (spread ? 12 : 0);
    std::vector<int> notes;
    for (int interval : essential) {
        int note = root + interval;
        if (voiceLed && hasSeventh &&
            (interval == 10 || interval == 11)) {
            note -= 12;
        }
        if (spread && interval > 0 && interval < 7) note += 12;
        notes.push_back(note);
    }
    if (voiceLed) {
        const double center =
            std::accumulate(notes.cbegin(), notes.cend(), 0.0) / notes.size();
        const double targetCenter =
            previousCenter > 0.0 ? previousCenter : 56.0;
        int octaveShift = 0;
        double bestDistance =
            std::numeric_limits<double>::max();
        for (int candidate = -24;
             candidate <= 24;
             candidate += 12) {
            const double candidateCenter = center + candidate;
            if (candidateCenter < 50.0 ||
                candidateCenter > 62.0) {
                continue;
            }
            const double distance =
                std::abs(candidateCenter - targetCenter);
            if (distance < bestDistance) {
                bestDistance = distance;
                octaveShift = candidate;
            }
        }
        for (int& note : notes) note += octaveShift;
    }
    if (chord.bass >= 0 && !voiceLed) {
        int bass = 36 + chord.bass;
        while (bass >= 48) bass -= 12;
        if (spread) {
            for (int& note : notes) {
                while (note - bass < 5) note += 12;
            }
        } else if (!notes.empty()) {
            const int lowest =
                *std::min_element(notes.cbegin(), notes.cend());
            while (lowest - bass < 5) bass -= 12;
        }
        notes.insert(notes.begin(), bass);
    }
    return notes;
}

std::vector<NoteEvent> extractEvents(const GeneratedPracticeIdea& idea)
{
    const SongSection& section = idea.chordSection;
    std::vector<NoteEvent> events;

    const auto appendLane = [&](const QString& lane) {
        struct TickNote {
            int start = 0;
            int end = 1;
            int midi = 60;
            int velocity = 88;
            QString articulation;
        };
        std::vector<TickNote> notes;
        std::optional<std::size_t> active;
        const auto closeAt = [&](int tick) {
            if (!active) return;
            notes[*active].end = std::max(notes[*active].start + 1, tick);
            active.reset();
        };
        for (int beat = 0; beat < section.beats; ++beat) {
            if (beat >= section.musicalPatterns.size()) continue;
            const MusicalBeatPattern& pattern =
                section.musicalPatterns.at(beat);
            const QVector<MusicalStep>* steps =
                lane == QStringLiteral("melody") ? &pattern.melody :
                lane == QStringLiteral("bass") ? &pattern.bass :
                                                 &pattern.support;
            if (pattern.division <= 0 ||
                steps->size() != pattern.division) {
                continue;
            }
            for (int step = 0; step < pattern.division; ++step) {
                const int tick = beat * kTicksPerBeat +
                    step * kTicksPerBeat / pattern.division;
                const MusicalStep& value = steps->at(step);
                if (value.state == MusicalStepState::Rest) {
                    closeAt(tick);
                } else if (value.state == MusicalStepState::Onset) {
                    closeAt(tick);
                    const std::optional<int> midi =
                        jam2::practice::parseMidiNote(value.value);
                    if (!midi) continue;
                    notes.push_back({
                        tick,
                        tick + 1,
                        *midi,
                        std::clamp(value.velocity, 1, 127),
                        value.articulation,
                    });
                    active = notes.size() - 1;
                }
            }
        }
        closeAt(section.beats * kTicksPerBeat);
        for (const TickNote& note : notes) {
            const qint64 shift =
                timingOffsetFrames(idea, lane, note.start);
            events.push_back({
                std::max<qint64>(
                    0, frameAtTick(idea, note.start) + shift),
                std::max<qint64>(
                    1, frameAtTick(idea, note.end) + shift),
                note.midi,
                note.velocity,
                lane,
                note.articulation,
            });
        }
    };

    struct ActiveChord {
        int start = 0;
        std::vector<int> midi;
        int velocity = 88;
        QString articulation;
    };
    std::optional<ActiveChord> activeChord;
    double previousCenter = 0.0;
    const auto closeChord = [&](int tick) {
        if (!activeChord) return;
        const qint64 shift = timingOffsetFrames(
            idea, QStringLiteral("comping"), activeChord->start);
        for (int midi : activeChord->midi) {
            events.push_back({
                std::max<qint64>(
                    0, frameAtTick(idea, activeChord->start) + shift),
                std::max<qint64>(
                    1, frameAtTick(idea, tick) + shift),
                midi,
                activeChord->velocity,
                QStringLiteral("chords"),
                activeChord->articulation,
            });
        }
        activeChord.reset();
    };
    for (int beat = 0; beat < section.beats; ++beat) {
        if (beat >= section.musicalPatterns.size()) continue;
        const MusicalBeatPattern& pattern =
            section.musicalPatterns.at(beat);
        if (pattern.division <= 0 ||
            pattern.chords.size() != pattern.division) {
            continue;
        }
        for (int step = 0; step < pattern.division; ++step) {
            const int tick = beat * kTicksPerBeat +
                step * kTicksPerBeat / pattern.division;
            const MusicalStep& value = pattern.chords.at(step);
            if (value.state == MusicalStepState::Rest) {
                closeChord(tick);
            } else if (value.state == MusicalStepState::Onset) {
                closeChord(tick);
                const ParsedChord parsed =
                    jam2::practice::parseChord(value.value);
                if (!parsed.valid || parsed.rest) continue;
                std::vector<int> midi =
                    chordNotes(parsed, idea.recipe.styleId, previousCenter);
                if (!midi.empty()) {
                    double sum = 0.0;
                    for (int note : midi) sum += note;
                    previousCenter = sum / midi.size();
                }
                activeChord = ActiveChord{
                    tick,
                    std::move(midi),
                    std::clamp(value.velocity, 1, 127),
                    value.articulation,
                };
            }
        }
    }
    closeChord(section.beats * kTicksPerBeat);
    appendLane(QStringLiteral("melody"));
    appendLane(QStringLiteral("bass"));
    appendLane(QStringLiteral("support"));
    std::sort(
        events.begin(),
        events.end(),
        [](const NoteEvent& left, const NoteEvent& right) {
            return left.start < right.start;
        });
    return events;
}

std::vector<float> readMonoPcm16(const QString& path)
{
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            QStringLiteral("Cannot read %1").arg(path).toStdString());
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 44 || bytes.mid(0, 4) != "RIFF" ||
        bytes.mid(8, 4) != "WAVE") {
        throw std::runtime_error(
            QStringLiteral("Unsupported WAV %1").arg(path).toStdString());
    }
    int dataOffset = -1;
    quint32 dataSize = 0;
    int offset = 12;
    while (offset + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(offset, 4);
        const quint32 size = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(
                bytes.constData() + offset + 4));
        if (id == "data") {
            dataOffset = offset + 8;
            dataSize = std::min<quint32>(
                size, bytes.size() - dataOffset);
            break;
        }
        offset += 8 + static_cast<int>(size) + (size & 1U);
    }
    if (dataOffset < 0) {
        throw std::runtime_error("WAV data chunk is missing.");
    }
    std::vector<float> result(dataSize / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const qint16 sample = qFromLittleEndian<qint16>(
            reinterpret_cast<const uchar*>(
                bytes.constData() + dataOffset +
                static_cast<int>(index * 2)));
        result[index] = sample / 32768.0f;
    }
    return result;
}

void matchAuditionLevel(
    std::vector<float>& audio,
    const QString& role)
{
    if (audio.empty()) return;
    const double targetRms =
        role == QStringLiteral("drums") ? 0.17 :
        role == QStringLiteral("bass") ? 0.14 :
        role == QStringLiteral("support") ? 0.09 : 0.12;
    double sumSquares = 0.0;
    float peak = 0.0f;
    for (float value : audio) {
        sumSquares += static_cast<double>(value) * value;
        peak = std::max(peak, std::abs(value));
    }
    const double rms = std::sqrt(
        sumSquares / std::max<std::size_t>(1, audio.size()));
    if (rms < 1.0e-7 || peak < 1.0e-7f) return;
    const float gain = static_cast<float>(std::min({
        8.0,
        targetRms / rms,
        0.88 / peak,
    }));
    for (float& value : audio) value *= gain;
}

void blockSubAudibleDc(std::vector<float>& audio)
{
    const double pole =
        std::exp(-2.0 * kPi * 12.0 / kSampleRate);
    double previousInput = 0.0;
    double previousOutput = 0.0;
    for (float& sample : audio) {
        const double input = sample;
        const double output =
            input - previousInput + pole * previousOutput;
        previousInput = input;
        previousOutput = output;
        sample = static_cast<float>(output);
    }
}

bool writeMonoPcm16(
    const QString& path,
    std::vector<float> audio,
    const QString& role)
{
    matchAuditionLevel(audio, role);
    QSaveFile file(path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = static_cast<quint32>(audio.size() * 2);
    stream.writeRawData("RIFF", 4);
    stream << static_cast<quint32>(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << static_cast<quint32>(16)
           << static_cast<quint16>(1)
           << static_cast<quint16>(1)
           << static_cast<quint32>(kSampleRate)
           << static_cast<quint32>(kSampleRate * 2)
           << static_cast<quint16>(2)
           << static_cast<quint16>(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    for (float value : audio) {
        stream << static_cast<qint16>(std::lround(
            std::clamp(value, -0.98f, 0.98f) * 32767.0f));
    }
    return file.commit();
}

bool hasLane(
    const GeneratedPracticeIdea& idea,
    const QString& lane)
{
    for (const MusicalBeatPattern& pattern :
         idea.chordSection.musicalPatterns) {
        const QVector<MusicalStep>& steps =
            lane == QStringLiteral("melody") ? pattern.melody :
            lane == QStringLiteral("bass") ? pattern.bass :
                                             pattern.support;
        for (const MusicalStep& step : steps) {
            if (step.state == MusicalStepState::Onset) return true;
        }
    }
    return false;
}

StemSet renderJam2Stems(
    const GeneratedPracticeIdea& idea,
    const QString& workspace)
{
    ReferenceRenderSettings settings;
    settings.renderChords = true;
    settings.renderDrums = true;
    settings.renderMelody =
        hasLane(idea, QStringLiteral("melody"));
    settings.renderBass =
        hasLane(idea, QStringLiteral("bass"));
    settings.renderSupport =
        hasLane(idea, QStringLiteral("support"));
    settings.voicing = ChordVoicing::StyleDefault;
    settings.sampleRate = kSampleRate;
    settings.bpm = idea.bpm;
    settings.meterNumerator = idea.meterNumerator;
    settings.meterDenominator = idea.meterDenominator;
    settings.tempoPulseUnits = idea.tempoPulseUnits;
    const auto result = jam2::practice::renderPracticeReferences(
        &idea.chordSection,
        &idea.beatSection,
        settings,
        workspace);
    if (!result.error.isEmpty()) {
        throw std::runtime_error(
            QStringLiteral("Jam2 render failed: %1")
                .arg(result.error)
                .toStdString());
    }
    return {
        readMonoPcm16(result.chords.path),
        readMonoPcm16(result.melody.path),
        readMonoPcm16(result.bass.path),
        readMonoPcm16(result.support.path),
        readMonoPcm16(result.drums.path),
    };
}

enum class SourceKind {
    Jam2Native,
    Shape,
    VariableSaw,
    Fm,
    String,
    Harmonic,
    Sine,
    Formant,
    Vosim,
    Z,
};

std::optional<SourceKind> sourceKindFromName(const QString& source);

enum class FilterKind {
    Ladder,
    StateVariableLowpass,
    StateVariableBandpass,
    Direct,
};

struct PatchDesign {
    QString name;
    SourceKind source = SourceKind::Shape;
    bool secondSourceEnabled = false;
    SourceKind secondSource = SourceKind::Sine;
    float sourceBlend = 0.0f;
    int secondSourceTranspose = 0;
    float secondSourceDetuneCents = 0.0f;
    FilterKind filter = FilterKind::Ladder;
    int harmonicFamily = 0;
    float shape = 0.12f;
    float width = 0.58f;
    float oscillator2Mix = 0.30f;
    float detuneCents = 4.0f;
    float subMix = 0.0f;
    float fmRatio = 2.0f;
    float fmIndex = 2.2f;
    float formantRatio = 2.8f;
    float formantRatio2 = 4.2f;
    float formantHz = 0.0f;
    float formantHz2 = 0.0f;
    float spectralShape = 0.12f;
    float spectralMode = 0.0f;
    float stringStructure = 0.38f;
    float stringBrightness = 0.62f;
    float stringDamping = 0.56f;
    float stringDouble = 0.0f;
    float attack = 0.008f;
    float decay = 0.25f;
    float sustain = 0.62f;
    float release = 0.35f;
    float filterCutoff = 1800.0f;
    float filterEnvelope = 2600.0f;
    float resonance = 0.30f;
    float filterDrive = 1.35f;
    float wavefold = 0.0f;
    float noiseMix = 0.0f;
    float transientMix = 0.0f;
    float transientSeconds = 0.025f;
    float vibratoCents = 0.0f;
    float vibratoRate = 5.1f;
    float vibratoDelay = 0.14f;
    float tremoloDepth = 0.0f;
    float tremoloRate = 4.2f;
    float voiceDrive = 1.0f;
    float busDrive = 1.0f;
    float cabinet = 0.0f;
    float chorusMix = 0.0f;
    float chorusDepth = 0.16f;
    float chorusRate = 0.32f;
    float delayMix = 0.0f;
    float delaySeconds = 0.23f;
};

PatchDesign basePatch(const QString& role)
{
    PatchDesign patch;
    if (role == QStringLiteral("melody")) {
        patch.name = QStringLiteral("Shapeable vocal-like lead");
        patch.oscillator2Mix = 0.18f;
        patch.detuneCents = 2.0f;
        patch.attack = 0.015f;
        patch.sustain = 0.76f;
        patch.release = 0.28f;
        patch.filterCutoff = 2400.0f;
        patch.delayMix = 0.10f;
        patch.vibratoCents = 5.0f;
    } else if (role == QStringLiteral("bass")) {
        patch.name = QStringLiteral("Fundamental-plus-colour bass");
        patch.shape = 0.35f;
        patch.width = 0.44f;
        patch.oscillator2Mix = 0.10f;
        patch.detuneCents = 0.0f;
        patch.subMix = 0.38f;
        patch.attack = 0.003f;
        patch.decay = 0.18f;
        patch.sustain = 0.72f;
        patch.release = 0.14f;
        patch.filterCutoff = 430.0f;
        patch.filterEnvelope = 950.0f;
        patch.resonance = 0.22f;
        patch.vibratoCents = 0.0f;
    } else if (role == QStringLiteral("support")) {
        patch.name = QStringLiteral("Warm harmonic support");
        patch.source = SourceKind::Harmonic;
        patch.harmonicFamily = 0;
        patch.attack = 0.18f;
        patch.decay = 0.8f;
        patch.sustain = 0.74f;
        patch.release = 0.9f;
        patch.filterCutoff = 1300.0f;
        patch.filterEnvelope = 700.0f;
        patch.chorusMix = 0.18f;
        patch.chorusDepth = 0.22f;
        patch.delayMix = 0.12f;
    } else {
        patch.name = QStringLiteral("Shapeable polyphonic comp");
    }
    return patch;
}

PatchDesign patchFor(
    const ProfileDefinition& profile,
    const QString& role,
    const QString& patchId)
{
    PatchDesign patch = basePatch(role);
    const QString id = patchId.toLower();
    const QString style = profile.styleId;
    const bool bass = role == QStringLiteral("bass");

    if (id == QStringLiteral("pop-polysynth")) {
        patch.name = QStringLiteral(
            "Dual variable-saw Pop polysynth");
        patch.source = SourceKind::VariableSaw;
        patch.shape = 0.30f;
        patch.width = 0.58f;
        patch.oscillator2Mix = 0.34f;
        patch.detuneCents = 5.5f;
        patch.attack = 0.010f;
        patch.decay = 0.32f;
        patch.sustain = 0.62f;
        patch.release = 0.30f;
        patch.filterCutoff = 2600.0f;
        patch.filterEnvelope = 2100.0f;
        patch.resonance = 0.22f;
        patch.chorusMix = 0.14f;
        patch.chorusDepth = 0.18f;
    } else if (id.contains(QStringLiteral("driven")) ||
        id.contains(QStringLiteral("power-stack")) ||
        id.contains(QStringLiteral("garage-power"))) {
        patch.name = id.contains(QStringLiteral("metal"))
            ? QStringLiteral("Double physical string into split gain and cabinet")
            : QStringLiteral("Double physical string into driven cabinet");
        patch.source = SourceKind::String;
        patch.stringStructure = 0.44f;
        patch.stringBrightness = 0.72f;
        patch.stringDamping = 0.46f;
        patch.stringDouble = 0.82f;
        patch.attack = 0.001f;
        patch.decay = 0.32f;
        patch.sustain = 0.30f;
        patch.release = 0.10f;
        patch.filterCutoff = id.contains(QStringLiteral("metal"))
            ? 4300.0f : 5100.0f;
        patch.filterEnvelope = 400.0f;
        patch.resonance = 0.16f;
        patch.filterDrive = 1.5f;
        patch.voiceDrive = id.contains(QStringLiteral("metal"))
            ? 4.6f : 2.7f;
        patch.busDrive = id.contains(QStringLiteral("metal"))
            ? 2.4f : 1.75f;
        patch.cabinet = 0.82f;
        patch.transientMix = id.contains(QStringLiteral("metal"))
            ? 0.12f : 0.07f;
        patch.transientSeconds = 0.010f;
    } else if (id.contains(QStringLiteral("bright-layer")) ||
               id.contains(QStringLiteral("dance-layer")) ||
               id.contains(QStringLiteral("country-pop-layer"))) {
        patch.name = id.contains(QStringLiteral("country"))
            ? QStringLiteral("Compact pluck and soft poly layer")
            : QStringLiteral("Bright detuned Pop layer");
        patch.source = id.contains(QStringLiteral("country"))
            ? SourceKind::Shape : SourceKind::VariableSaw;
        // Daisy's VariableSaw waveshape runs from bright notch-saw at zero
        // toward triangle at one. Keep the deliberately bright J-Pop layers
        // near the saw end instead of accidentally making them darker than Pop.
        patch.shape =
            id.contains(QStringLiteral("dance")) ? 0.06f :
            id.contains(QStringLiteral("country")) ? 0.54f : 0.18f;
        patch.width =
            id.contains(QStringLiteral("dance")) ? 0.24f : 0.36f;
        patch.oscillator2Mix =
            id.contains(QStringLiteral("country")) ? 0.24f : 0.52f;
        patch.detuneCents =
            id.contains(QStringLiteral("country")) ? 3.0f : 7.0f;
        patch.attack = 0.004f;
        patch.decay = 0.34f;
        patch.sustain =
            id.contains(QStringLiteral("country")) ? 0.42f : 0.58f;
        patch.release = 0.26f;
        patch.filterCutoff =
            id.contains(QStringLiteral("country")) ? 3300.0f : 6800.0f;
        patch.filterEnvelope =
            id.contains(QStringLiteral("country")) ? 1200.0f : 2600.0f;
        patch.voiceDrive = 1.20f;
        patch.busDrive = 1.18f;
        patch.chorusMix =
            id.contains(QStringLiteral("dance")) ? 0.24f : 0.14f;
        patch.chorusDepth =
            id.contains(QStringLiteral("dance")) ? 0.28f : 0.16f;
        if (id.contains(QStringLiteral("country"))) {
            patch.filter = FilterKind::StateVariableLowpass;
            patch.attack = 0.002f;
            patch.decay = 0.25f;
            patch.sustain = 0.30f;
            patch.transientMix = 0.025f;
            patch.transientSeconds = 0.014f;
        }
    } else if (id.contains(QStringLiteral("nylon")) ||
               id.contains(QStringLiteral("country-pluck")) ||
               id.contains(QStringLiteral("modal-pluck")) ||
               id.contains(QStringLiteral("skank")) ||
               id.contains(QStringLiteral("pick-bass")) ||
               id.contains(QStringLiteral("upright")) ||
               id.contains(QStringLiteral("acoustic-bass")) ||
               id.contains(QStringLiteral("clean-lead")) ||
               id.contains(QStringLiteral("country-fill"))) {
        patch.source = SourceKind::String;
        patch.name = bass
            ? QStringLiteral("Damped physical-string bass")
            : id.contains(QStringLiteral("skank"))
                ? QStringLiteral("Choked physical-string skank")
                : QStringLiteral("Physical pluck with resonant body");
        patch.stringStructure =
            id.contains(QStringLiteral("nylon")) ? 0.31f : 0.39f;
        patch.stringBrightness =
            id.contains(QStringLiteral("upright")) ? 0.38f : 0.60f;
        patch.stringDamping =
            id.contains(QStringLiteral("skank")) ? 0.88f :
            bass ? 0.68f : 0.55f;
        patch.attack = 0.001f;
        patch.decay = bass ? 0.48f : 0.70f;
        patch.sustain =
            id.contains(QStringLiteral("skank")) ? 0.02f : 0.08f;
        patch.release = bass ? 0.20f : 0.28f;
        patch.filterCutoff = bass ? 1250.0f : 5200.0f;
        patch.filterEnvelope = bass ? 500.0f : 900.0f;
        patch.filter = bass
            ? FilterKind::StateVariableLowpass
            : FilterKind::Direct;
        patch.transientMix =
            id.contains(QStringLiteral("nylon")) ? 0.10f : 0.055f;
        patch.transientSeconds =
            id.contains(QStringLiteral("skank")) ? 0.008f : 0.018f;
        patch.cabinet = id.contains(QStringLiteral("pick-bass"))
            ? 0.36f : 0.12f;
        patch.voiceDrive = id.contains(QStringLiteral("pick-bass"))
            ? 1.55f : 1.05f;
        if (id.contains(QStringLiteral("pick-bass"))) {
            if (style == QStringLiteral("jpop-anisong")) {
                patch.name = QStringLiteral(
                    "Bright picked J-Pop electric-style bass");
                patch.stringBrightness = 0.78f;
                patch.stringDamping = 0.54f;
                patch.transientMix = 0.085f;
                patch.cabinet = 0.18f;
                patch.voiceDrive = 1.34f;
                patch.chorusMix = 0.055f;
            } else if (id == QStringLiteral("pick-bass")) {
                patch.name = QStringLiteral(
                    "Dry aggressive Punk picked bass");
                patch.stringBrightness = 0.70f;
                patch.stringDamping = 0.72f;
                patch.transientMix = 0.075f;
                patch.cabinet = 0.46f;
                patch.voiceDrive = 1.72f;
            } else {
                patch.name = QStringLiteral(
                    "Driven Rock picked-bass approximation");
                patch.stringBrightness = 0.63f;
                patch.stringDamping = 0.62f;
                patch.transientMix = 0.060f;
                patch.cabinet = 0.36f;
                patch.voiceDrive = 1.55f;
            }
        }
        if (id.contains(QStringLiteral("modal-pluck"))) {
            patch.name = QStringLiteral(
                "Spacious modal physical pluck");
            patch.stringBrightness = 0.52f;
            patch.stringDamping = 0.61f;
            patch.transientMix = 0.040f;
            patch.chorusMix = 0.11f;
            patch.delayMix = 0.14f;
        } else if (id.contains(QStringLiteral("country-pluck"))) {
            patch.name = QStringLiteral(
                "Dry bright Country physical pluck");
            patch.stringBrightness = 0.72f;
            patch.stringDamping = 0.58f;
            patch.transientMix = 0.090f;
            patch.cabinet = 0.08f;
            patch.delayMix = 0.0f;
        }
        if (id.contains(QStringLiteral("clean-lead"))) {
            if (style == QStringLiteral("metal-experimental")) {
                patch.name = QStringLiteral(
                    "Ambient clean physical-string contrast");
                patch.decay = 1.10f;
                patch.sustain = 0.12f;
                patch.release = 0.62f;
                patch.stringBrightness = 0.54f;
                patch.stringDamping = 0.50f;
                patch.chorusMix = 0.30f;
                patch.chorusDepth = 0.32f;
                patch.delayMix = 0.22f;
                patch.vibratoCents = 3.5f;
            } else {
                patch.name = QStringLiteral(
                    "Dry Country clean-fill physical pluck");
                patch.decay = 0.48f;
                patch.release = 0.20f;
                patch.stringBrightness = 0.69f;
                patch.stringDamping = 0.60f;
                patch.transientMix = 0.075f;
                patch.chorusMix = 0.04f;
                patch.delayMix = 0.045f;
                patch.vibratoCents = 2.0f;
            }
        }
    } else if (id.contains(QStringLiteral("organ"))) {
        patch.name = QStringLiteral("Additive drawbar-like organ");
        patch.source = SourceKind::Harmonic;
        patch.harmonicFamily =
            style == QStringLiteral("reggae") ? 1 : 2;
        patch.attack = 0.008f;
        patch.decay =
            style == QStringLiteral("reggae") ? 0.12f : 0.08f;
        patch.sustain =
            style == QStringLiteral("reggae") ? 0.34f : 0.92f;
        patch.release =
            style == QStringLiteral("reggae") ? 0.09f : 0.22f;
        patch.filterCutoff =
            style == QStringLiteral("reggae") ? 3300.0f : 4200.0f;
        patch.filterEnvelope = 0.0f;
        patch.voiceDrive = 1.18f;
        patch.filter = FilterKind::Direct;
        patch.tremoloDepth =
            style == QStringLiteral("reggae") ? 0.055f : 0.15f;
        patch.tremoloRate =
            style == QStringLiteral("reggae") ? 5.8f : 4.7f;
    } else if (id.contains(QStringLiteral("clav"))) {
        patch.name = QStringLiteral("Velocity-sensitive clipped FM clav");
        patch.source = SourceKind::Fm;
        patch.fmRatio = 3.0f;
        patch.fmIndex = 2.2f;
        patch.attack = 0.002f;
        patch.decay = 0.16f;
        patch.sustain = 0.16f;
        patch.release = 0.08f;
        patch.filterCutoff = 4200.0f;
        patch.filterEnvelope = 1300.0f;
        patch.voiceDrive = 1.65f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.transientMix = 0.015f;
        patch.transientSeconds = 0.012f;
    } else if (id.contains(QStringLiteral("ep")) ||
               id.contains(QStringLiteral("keys")) ||
               id.contains(QStringLiteral("piano"))) {
        patch.name =
            id.contains(QStringLiteral("bebop"))
                ? QStringLiteral("Compact FM struck-piano approximation") :
            id.contains(QStringLiteral("fusion"))
                ? QStringLiteral("Chorused FM tine keys") :
            id.contains(QStringLiteral("soul"))
                ? QStringLiteral("Warm tremolo FM electric keys") :
            id.contains(QStringLiteral("soft"))
                ? QStringLiteral("Soft chorused FM electric piano") :
            id.contains(QStringLiteral("boombap"))
                ? QStringLiteral("Filtered original-loop FM keys") :
            id.contains(QStringLiteral("dark"))
                ? QStringLiteral("Dark short FM keys")
                : QStringLiteral("FM tine electric piano");
        patch.source = SourceKind::Fm;
        patch.fmRatio =
            id.contains(QStringLiteral("bebop")) ? 1.0f :
            id.contains(QStringLiteral("fusion")) ? 1.5f :
            id.contains(QStringLiteral("boombap")) ? 1.5f : 2.0f;
        patch.fmIndex =
            id.contains(QStringLiteral("bebop")) ? 1.8f :
            id.contains(QStringLiteral("soft")) ? 0.8f :
            id.contains(QStringLiteral("boombap")) ? 1.0f :
            id.contains(QStringLiteral("soul")) ? 1.2f :
            id.contains(QStringLiteral("fusion")) ? 1.7f :
            id.contains(QStringLiteral("dark")) ? 1.5f : 1.35f;
        patch.attack = 0.004f;
        patch.decay =
            id.contains(QStringLiteral("bebop")) ? 0.42f :
            id.contains(QStringLiteral("boombap")) ? 0.36f : 0.68f;
        patch.sustain =
            id.contains(QStringLiteral("bebop")) ? 0.20f :
            id.contains(QStringLiteral("boombap")) ? 0.18f : 0.32f;
        patch.release =
            id.contains(QStringLiteral("bebop")) ? 0.32f :
            id.contains(QStringLiteral("boombap")) ? 0.24f : 0.58f;
        patch.filterCutoff =
            id.contains(QStringLiteral("bebop")) ? 3400.0f :
            id.contains(QStringLiteral("fusion")) ? 3600.0f :
            id.contains(QStringLiteral("soul")) ? 2800.0f :
            id.contains(QStringLiteral("soft")) ? 2300.0f :
            id.contains(QStringLiteral("dark")) ? 1500.0f :
            id.contains(QStringLiteral("boombap")) ? 2100.0f : 3200.0f;
        patch.filterEnvelope =
            id.contains(QStringLiteral("bebop")) ? 650.0f :
            id.contains(QStringLiteral("fusion")) ? 800.0f : 520.0f;
        patch.resonance = 0.16f;
        patch.delayMix = id.contains(QStringLiteral("boombap"))
            ? 0.05f : 0.08f;
        patch.filter = FilterKind::StateVariableLowpass;
        // The FM carrier already supplies the struck/tine attack. Broadband
        // noise here dominated the strongest listening window and turned keys
        // into hissy, cymbal-like events.
        patch.transientMix =
            id.contains(QStringLiteral("bebop")) ? 0.012f :
            id.contains(QStringLiteral("boombap")) ? 0.008f : 0.0f;
        patch.transientSeconds =
            id.contains(QStringLiteral("bebop")) ? 0.018f : 0.026f;
        patch.noiseMix =
            id.contains(QStringLiteral("boombap")) ? 0.018f : 0.0f;
        patch.tremoloDepth =
            id.contains(QStringLiteral("soul")) ? 0.09f :
            id.contains(QStringLiteral("rnb")) ? 0.045f : 0.0f;
        patch.tremoloRate =
            id.contains(QStringLiteral("soul")) ? 4.8f : 3.6f;
        patch.chorusMix =
            id.contains(QStringLiteral("fusion")) ? 0.18f :
            id.contains(QStringLiteral("rnb")) ? 0.12f : 0.0f;
    } else if (id.contains(QStringLiteral("bell"))) {
        patch.name = QStringLiteral("Sparse inharmonic FM bell");
        patch.source = SourceKind::Fm;
        patch.fmRatio = 3.5f;
        patch.fmIndex = 4.2f;
        patch.attack = 0.001f;
        patch.decay = 0.72f;
        patch.sustain = 0.04f;
        patch.release = 0.52f;
        patch.filterCutoff = 6800.0f;
        patch.filterEnvelope = 1400.0f;
        patch.delayMix = 0.16f;
        patch.delaySeconds = 0.31f;
        patch.filter = FilterKind::Direct;
        patch.transientMix = 0.035f;
        patch.transientSeconds = 0.010f;
    } else if (id.contains(QStringLiteral("808"))) {
        patch.name = QStringLiteral("Pitched sine 808 with driven upper colour");
        patch.source = SourceKind::Sine;
        patch.subMix = 0.26f;
        patch.attack = 0.002f;
        patch.decay = 0.42f;
        patch.sustain = 0.84f;
        patch.release = 0.72f;
        patch.filterCutoff = 720.0f;
        patch.filterEnvelope = 250.0f;
        patch.voiceDrive = 1.9f;
        patch.busDrive = 1.25f;
    } else if (id.contains(QStringLiteral("sub"))) {
        patch.name = QStringLiteral("Clean sub with restrained harmonic translation");
        patch.source = SourceKind::Sine;
        patch.subMix = 0.18f;
        patch.attack = 0.004f;
        patch.release = 0.30f;
        patch.filterCutoff = 520.0f;
        patch.filterEnvelope = 360.0f;
        patch.voiceDrive = 1.32f;
        if (id.contains(QStringLiteral("modal"))) {
            patch.name = QStringLiteral("Slow pure modal pedal sub");
            patch.subMix = 0.08f;
            patch.attack = 0.018f;
            patch.decay = 0.36f;
            patch.sustain = 0.84f;
            patch.release = 0.62f;
            patch.filterCutoff = 390.0f;
            patch.filterEnvelope = 90.0f;
            patch.voiceDrive = 1.05f;
        } else if (id.contains(QStringLiteral("rnb"))) {
            patch.name = QStringLiteral(
                "Round R&B sub-electric foundation");
            patch.subMix = 0.24f;
            patch.attack = 0.008f;
            patch.decay = 0.22f;
            patch.sustain = 0.78f;
            patch.release = 0.44f;
            patch.filterCutoff = 610.0f;
            patch.filterEnvelope = 560.0f;
            patch.voiceDrive = 1.42f;
        } else if (id.contains(QStringLiteral("house"))) {
            patch.name = QStringLiteral("Short translated House sub");
            patch.attack = 0.002f;
            patch.decay = 0.15f;
            patch.sustain = 0.62f;
            patch.release = 0.16f;
            patch.filterEnvelope = 720.0f;
            patch.voiceDrive = 1.46f;
        } else if (id.contains(QStringLiteral("techno"))) {
            patch.name = QStringLiteral("Sustained driven Techno sub");
            patch.subMix = 0.22f;
            patch.decay = 0.28f;
            patch.sustain = 0.76f;
            patch.release = 0.28f;
            patch.filterCutoff = 680.0f;
            patch.filterEnvelope = 480.0f;
            patch.voiceDrive = 1.58f;
        }
    } else if (id.contains(QStringLiteral("pad")) ||
               id.contains(QStringLiteral("strings")) ||
               id.contains(QStringLiteral("texture")) ||
               id.contains(QStringLiteral("ambient")) ||
               id.contains(QStringLiteral("group-voice"))) {
        const bool technoTexture =
            id.contains(QStringLiteral("techno-texture"));
        patch.name = technoTexture
            ? QStringLiteral("Slow Z-oscillator spectral texture")
            : id.contains(QStringLiteral("ambient"))
                ? QStringLiteral("Evolving harmonic atmosphere")
                : QStringLiteral("Layered harmonic pad");
        patch.source = technoTexture
            ? SourceKind::Z : SourceKind::Harmonic;
        patch.harmonicFamily =
            id.contains(QStringLiteral("strings")) ? 3 :
            id.contains(QStringLiteral("group-voice")) ? 4 :
            id.contains(QStringLiteral("modal-texture")) ? 5 :
            id.contains(QStringLiteral("rnb")) ? 2 : 0;
        patch.formantRatio = 1.86f;
        patch.spectralShape = 0.24f;
        patch.spectralMode = 0.58f;
        patch.attack =
            id.contains(QStringLiteral("ambient")) ? 0.72f : 0.24f;
        patch.decay = 1.1f;
        patch.sustain = 0.76f;
        patch.release =
            id.contains(QStringLiteral("ambient")) ? 1.4f : 0.82f;
        patch.filterCutoff =
            id.contains(QStringLiteral("dark")) ? 1100.0f : 1800.0f;
        patch.filterEnvelope = 850.0f;
        patch.wavefold = technoTexture ? 1.12f : 0.0f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.chorusMix = technoTexture ? 0.08f : 0.24f;
        patch.chorusDepth = technoTexture ? 0.12f : 0.30f;
        patch.chorusRate = technoTexture ? 0.11f : 0.24f;
        patch.delayMix = 0.18f;
        patch.delaySeconds = 0.37f;
        if (id.contains(QStringLiteral("rnb"))) {
            patch.name = QStringLiteral(
                "Warm chorused R&B harmonic pad");
            patch.attack = 0.34f;
            patch.decay = 0.92f;
            patch.sustain = 0.68f;
            patch.release = 1.12f;
            patch.filterCutoff = 1250.0f;
            patch.filterEnvelope = 480.0f;
            patch.chorusMix = 0.32f;
            patch.chorusDepth = 0.34f;
            patch.chorusRate = 0.19f;
            patch.delayMix = 0.10f;
        }
    } else if (id.contains(QStringLiteral("stab")) ||
               id.contains(QStringLiteral("pluck")) ||
               id.contains(QStringLiteral("sequence"))) {
        const bool technoSequence =
            id.contains(QStringLiteral("techno-sequence"));
        const bool house =
            id.contains(QStringLiteral("house"));
        const bool techno =
            id.contains(QStringLiteral("techno"));
        const bool breakbeat =
            id.contains(QStringLiteral("breakbeat"));
        patch.name = technoSequence
            ? QStringLiteral("Animated Z-oscillator Techno sequence")
            : breakbeat
                ? QStringLiteral("Angular pulse-saw Breakbeat stab")
                : house
                    ? QStringLiteral("Bright filtered House chord stab")
                    : QStringLiteral("Resonant Techno saw stab");
        patch.source = technoSequence
            ? SourceKind::Z :
            breakbeat ? SourceKind::Shape : SourceKind::VariableSaw;
        patch.shape =
            house ? 0.20f :
            techno ? 0.04f :
            breakbeat ? 0.78f : 0.30f;
        patch.width =
            techno ? 0.18f :
            breakbeat ? 0.34f : 0.38f;
        patch.oscillator2Mix =
            house ? 0.30f :
            breakbeat ? 0.18f : 0.22f;
        patch.formantRatio = 3.35f;
        patch.spectralShape = 0.58f;
        patch.spectralMode = 0.42f;
        patch.attack = 0.002f;
        patch.decay =
            technoSequence ? 0.12f :
            house ? 0.23f :
            breakbeat ? 0.19f : 0.11f;
        patch.sustain = house ? 0.08f : 0.12f;
        patch.release = house ? 0.14f : 0.10f;
        patch.filterCutoff =
            house ? 1050.0f :
            breakbeat ? 1450.0f : 720.0f;
        patch.filterEnvelope =
            house ? 6200.0f :
            breakbeat ? 3400.0f : 5200.0f;
        patch.resonance =
            house ? 0.40f :
            techno ? 0.66f : 0.48f;
        patch.filterDrive = 1.65f;
        patch.wavefold = technoSequence ? 1.35f : 0.0f;
        patch.transientMix =
            technoSequence ? 0.025f :
            breakbeat ? 0.035f : 0.018f;
        patch.transientSeconds = 0.008f;
        patch.busDrive =
            techno ? 1.34f :
            breakbeat ? 1.22f : 1.12f;
        patch.chorusMix = house ? 0.08f : 0.0f;
    } else if (id.contains(QStringLiteral("fusion-synth"))) {
        patch.name =
            QStringLiteral("Shapeable electric Fusion lead");
        patch.source = SourceKind::Shape;
        patch.shape = 0.60f;
        patch.width = 0.38f;
        patch.oscillator2Mix = 0.34f;
        patch.detuneCents = 4.0f;
        patch.attack = 0.010f;
        patch.decay = 0.26f;
        patch.sustain = 0.70f;
        patch.release = 0.24f;
        patch.filterCutoff = 3900.0f;
        patch.filterEnvelope = 1200.0f;
        patch.voiceDrive = 1.28f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.chorusMix = 0.13f;
        patch.chorusDepth = 0.18f;
        patch.vibratoCents = 6.0f;
    } else if (id.contains(QStringLiteral("boombap-lead"))) {
        patch.name =
            QStringLiteral("Filtered sample-like FM hook");
        patch.source = SourceKind::Fm;
        patch.fmRatio = 2.0f;
        patch.fmIndex = 1.6f;
        patch.attack = 0.008f;
        patch.decay = 0.34f;
        patch.sustain = 0.18f;
        patch.release = 0.24f;
        patch.filterCutoff = 1700.0f;
        patch.filterEnvelope = 650.0f;
        patch.delayMix = 0.04f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.noiseMix = 0.022f;
        patch.transientMix = 0.045f;
    } else if (id.contains(QStringLiteral("reggae-round-bass")) ||
               id.contains(QStringLiteral("boombap-bass"))) {
        const bool reggae =
            id.contains(QStringLiteral("reggae"));
        patch.name = reggae
            ? QStringLiteral("Deep sine-led Roots bass")
            : QStringLiteral("Dark fundamental Boom-Bap bass");
        patch.source = SourceKind::Sine;
        patch.shape = reggae ? 0.16f : 0.30f;
        patch.subMix = reggae ? 0.24f : 0.16f;
        patch.attack = 0.006f;
        patch.decay = reggae ? 0.30f : 0.20f;
        patch.sustain = reggae ? 0.82f : 0.68f;
        patch.release = reggae ? 0.30f : 0.18f;
        patch.filterCutoff = reggae ? 620.0f : 470.0f;
        patch.filterEnvelope = reggae ? 360.0f : 520.0f;
        patch.voiceDrive = reggae ? 1.18f : 1.38f;
    } else if (id.contains(QStringLiteral("funk-electric-bass"))) {
        patch.name = QStringLiteral(
            "Fast envelope variable-saw Funk bass");
        patch.source = SourceKind::VariableSaw;
        patch.shape = 0.72f;
        patch.width = 0.26f;
        patch.oscillator2Mix = 0.08f;
        patch.subMix = 0.24f;
        patch.attack = 0.001f;
        patch.decay = 0.11f;
        patch.sustain = 0.52f;
        patch.release = 0.09f;
        patch.filterCutoff = 360.0f;
        patch.filterEnvelope = 2500.0f;
        patch.resonance = 0.40f;
        patch.voiceDrive = 1.35f;
    } else if (id.contains(QStringLiteral("pop-electric-bass")) ||
               id.contains(QStringLiteral("jpop-synth-bass"))) {
        patch.name = QStringLiteral(
            "Controlled variable-saw Pop bass");
        patch.source = SourceKind::VariableSaw;
        patch.shape = id.contains(QStringLiteral("jpop")) ? 0.72f : 0.48f;
        patch.width = 0.38f;
        patch.oscillator2Mix = 0.10f;
        patch.subMix = 0.30f;
        patch.attack = 0.002f;
        patch.decay = 0.16f;
        patch.sustain = 0.68f;
        patch.release = 0.13f;
        patch.filterCutoff = 520.0f;
        patch.filterEnvelope =
            id.contains(QStringLiteral("jpop")) ? 1900.0f : 1250.0f;
    } else if (id.contains(QStringLiteral("fusion-electric-bass"))) {
        patch.name = QStringLiteral(
            "Bright articulated Fusion synth bass");
        patch.source = SourceKind::Shape;
        patch.shape = 0.58f;
        patch.width = 0.36f;
        patch.oscillator2Mix = 0.16f;
        patch.subMix = 0.22f;
        patch.attack = 0.002f;
        patch.decay = 0.14f;
        patch.sustain = 0.62f;
        patch.release = 0.12f;
        patch.filterCutoff = 680.0f;
        patch.filterEnvelope = 2100.0f;
        patch.voiceDrive = 1.28f;
    } else if (id.contains(QStringLiteral("modal-pedal-bass"))) {
        patch.name = QStringLiteral(
            "Fundamental modal pedal with slow colour");
        patch.source = SourceKind::Sine;
        patch.shape = 0.12f;
        patch.subMix = 0.10f;
        patch.attack = 0.010f;
        patch.decay = 0.26f;
        patch.sustain = 0.86f;
        patch.release = 0.38f;
        patch.filterCutoff = 640.0f;
        patch.filterEnvelope = 180.0f;
        patch.voiceDrive = 1.08f;
    } else if (id.contains(QStringLiteral("blues-electric-bass"))) {
        patch.name = QStringLiteral(
            "Round Blues electric-style bass");
        patch.source = SourceKind::Shape;
        patch.shape = 0.26f;
        patch.width = 0.48f;
        patch.oscillator2Mix = 0.08f;
        patch.subMix = 0.30f;
        patch.attack = 0.004f;
        patch.decay = 0.24f;
        patch.sustain = 0.76f;
        patch.release = 0.19f;
        patch.filterCutoff = 560.0f;
        patch.filterEnvelope = 820.0f;
        patch.voiceDrive = 1.12f;
    } else if (id.contains(QStringLiteral("country-electric-bass"))) {
        patch.name = QStringLiteral(
            "Muted Country physical-string bass");
        patch.source = SourceKind::String;
        patch.stringStructure = 0.36f;
        patch.stringBrightness = 0.47f;
        patch.stringDamping = 0.79f;
        patch.attack = 0.001f;
        patch.decay = 0.38f;
        patch.sustain = 0.05f;
        patch.release = 0.16f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.filterCutoff = 1050.0f;
        patch.filterEnvelope = 380.0f;
        patch.transientMix = 0.035f;
        patch.transientSeconds = 0.014f;
    } else if (id.contains(QStringLiteral("soul-electric-bass"))) {
        patch.name = QStringLiteral(
            "Melodic warm Soul electric-style bass");
        patch.source = SourceKind::Shape;
        patch.shape = 0.52f;
        patch.width = 0.40f;
        patch.oscillator2Mix = 0.10f;
        patch.subMix = 0.28f;
        patch.attack = 0.003f;
        patch.decay = 0.20f;
        patch.sustain = 0.66f;
        patch.release = 0.17f;
        patch.filterCutoff = 520.0f;
        patch.filterEnvelope = 1650.0f;
        patch.voiceDrive = 1.18f;
    } else if (bass || id.contains(QStringLiteral("bass"))) {
        patch.name = QStringLiteral("Articulated electric-style synth bass");
        patch.source = SourceKind::Shape;
        patch.shape = 0.38f;
        patch.width = 0.42f;
        patch.oscillator2Mix = 0.12f;
        patch.subMix = 0.34f;
        patch.attack = 0.003f;
        patch.decay = 0.17f;
        patch.sustain = 0.68f;
        patch.release = 0.15f;
        patch.filterCutoff = 520.0f;
        patch.filterEnvelope = 1200.0f;
        patch.voiceDrive =
            id.contains(QStringLiteral("split")) ? 1.9f : 1.25f;
        patch.cabinet =
            id.contains(QStringLiteral("split")) ? 0.44f : 0.0f;
    } else if (id.contains(QStringLiteral("reed"))) {
        patch.name = QStringLiteral(
            "Profile-tuned VOSIM reed-like lead");
        patch.source = SourceKind::Vosim;
        const bool rockBlues =
            style == QStringLiteral("rock");
        patch.formantRatio =
            id.contains(QStringLiteral("bebop")) ? 2.85f :
            id.contains(QStringLiteral("jazz")) ? 2.25f : 1.72f;
        patch.formantRatio2 =
            id.contains(QStringLiteral("bebop")) ? 4.20f :
            id.contains(QStringLiteral("jazz")) ? 3.35f : 2.65f;
        patch.spectralShape =
            id.contains(QStringLiteral("bebop")) ? 0.18f : -0.22f;
        patch.formantHz =
            id.contains(QStringLiteral("bebop")) ? 1120.0f :
            id.contains(QStringLiteral("jazz")) ? 860.0f :
            rockBlues ? 940.0f : 760.0f;
        patch.formantHz2 =
            id.contains(QStringLiteral("bebop")) ? 2860.0f :
            id.contains(QStringLiteral("jazz")) ? 2280.0f :
            rockBlues ? 2520.0f : 1880.0f;
        patch.attack = 0.018f;
        patch.decay = 0.20f;
        patch.sustain = 0.76f;
        patch.release = 0.24f;
        patch.filterCutoff =
            id.contains(QStringLiteral("bebop")) ? 4300.0f : 3200.0f;
        patch.filterEnvelope = 750.0f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.noiseMix =
            id.contains(QStringLiteral("bebop")) ? 0.012f : 0.020f;
        patch.vibratoCents =
            id.contains(QStringLiteral("bebop")) ? 5.5f : 7.0f;
        patch.vibratoRate =
            id.contains(QStringLiteral("bebop")) ? 5.8f : 5.1f;
        patch.vibratoDelay = 0.20f;
        patch.delayMix = 0.05f;
        if (rockBlues) {
            patch.name = QStringLiteral(
                "Driven reed-like Blues-Rock response");
            patch.filterCutoff = 3800.0f;
            patch.voiceDrive = 1.22f;
            patch.cabinet = 0.14f;
            patch.vibratoCents = 5.8f;
        }
    } else if (id.contains(QStringLiteral("flute")) ||
               id.contains(QStringLiteral("air-lead"))) {
        patch.name = QStringLiteral(
            "Airy phase-reset formant lead");
        patch.source = SourceKind::Formant;
        patch.formantRatio = id.contains(QStringLiteral("flute"))
            ? 5.4f : 4.15f;
        patch.formantHz = id.contains(QStringLiteral("flute"))
            ? 3150.0f : 2380.0f;
        patch.spectralShape = id.contains(QStringLiteral("flute"))
            ? 0.08f : -0.12f;
        patch.attack = 0.024f;
        patch.decay = 0.24f;
        patch.sustain = 0.78f;
        patch.release = 0.30f;
        patch.filterCutoff = id.contains(QStringLiteral("flute"))
            ? 5200.0f : 4300.0f;
        patch.filterEnvelope = 620.0f;
        patch.filter = FilterKind::StateVariableBandpass;
        patch.noiseMix = id.contains(QStringLiteral("flute"))
            ? 0.065f : 0.032f;
        patch.vibratoCents = id.contains(QStringLiteral("flute"))
            ? 4.0f : 6.5f;
        patch.vibratoRate = id.contains(QStringLiteral("flute"))
            ? 4.7f : 5.3f;
        patch.vibratoDelay = 0.24f;
        patch.delayMix = 0.12f;
    } else if (id.contains(QStringLiteral("horn"))) {
        patch.name = QStringLiteral(
            "Compact additive horn-like response");
        patch.source = SourceKind::Harmonic;
        patch.harmonicFamily = 4;
        patch.attack = 0.012f;
        patch.decay = 0.18f;
        patch.sustain = 0.52f;
        patch.release = 0.16f;
        patch.filterCutoff = 3000.0f;
        patch.filterEnvelope = 1400.0f;
        patch.filter = FilterKind::StateVariableBandpass;
        patch.transientMix = 0.035f;
        patch.vibratoCents = 2.5f;
        if (style == QStringLiteral("funk")) {
            patch.name = QStringLiteral(
                "Dry clipped Funk horn-like stab");
            patch.attack = 0.006f;
            patch.decay = 0.11f;
            patch.sustain = 0.28f;
            patch.release = 0.075f;
            patch.filterCutoff = 3700.0f;
            patch.transientMix = 0.050f;
            patch.voiceDrive = 1.18f;
        } else {
            patch.name = QStringLiteral(
                "Warm Soul horn-like response");
            patch.attack = 0.018f;
            patch.decay = 0.24f;
            patch.sustain = 0.58f;
            patch.release = 0.22f;
            patch.filterCutoff = 2700.0f;
            patch.transientMix = 0.020f;
        }
    } else if (id.contains(QStringLiteral("fiddle"))) {
        patch.name = QStringLiteral(
            "Bright additive bowed-string approximation");
        patch.source = SourceKind::Harmonic;
        patch.harmonicFamily = 3;
        patch.oscillator2Mix = 0.20f;
        patch.detuneCents = 2.5f;
        patch.attack = 0.035f;
        patch.decay = 0.18f;
        patch.sustain = 0.82f;
        patch.release = 0.22f;
        patch.filterCutoff = 4600.0f;
        patch.filterEnvelope = 700.0f;
        patch.filter = FilterKind::StateVariableLowpass;
        patch.noiseMix = 0.030f;
        patch.vibratoCents = 9.0f;
        patch.vibratoRate = 5.7f;
        patch.vibratoDelay = 0.10f;
    } else if (id.contains(QStringLiteral("vocal")) ||
               id == QStringLiteral("pop-lead")) {
        patch.name = QStringLiteral(
            "Profile-tuned formant vocal-like lead");
        patch.source = SourceKind::Formant;
        patch.formantRatio =
            id.contains(QStringLiteral("jpop")) ? 3.8f :
            id.contains(QStringLiteral("rnb")) ? 2.05f :
            id.contains(QStringLiteral("soul")) ? 2.35f :
            id.contains(QStringLiteral("reggae")) ? 1.82f : 2.9f;
        patch.formantHz =
            id.contains(QStringLiteral("jpop")) ? 1980.0f :
            id.contains(QStringLiteral("rnb")) ? 930.0f :
            id.contains(QStringLiteral("soul")) ? 1240.0f :
            id.contains(QStringLiteral("reggae")) ? 720.0f : 1540.0f;
        patch.spectralShape =
            id.contains(QStringLiteral("jpop")) ? 0.24f :
            id.contains(QStringLiteral("rnb")) ? -0.18f : 0.02f;
        patch.attack = 0.020f;
        patch.decay = 0.24f;
        patch.sustain = 0.76f;
        patch.release = 0.30f;
        patch.filterCutoff =
            id.contains(QStringLiteral("jpop")) ? 5400.0f :
            id.contains(QStringLiteral("rnb")) ? 3000.0f : 3900.0f;
        patch.filterEnvelope = 720.0f;
        patch.filter = id.contains(QStringLiteral("rnb"))
            ? FilterKind::StateVariableLowpass
            : FilterKind::Direct;
        patch.noiseMix =
            id.contains(QStringLiteral("rnb")) ? 0.045f :
            id.contains(QStringLiteral("soul")) ? 0.020f :
            id.contains(QStringLiteral("reggae")) ? 0.015f : 0.008f;
        patch.vibratoCents =
            id.contains(QStringLiteral("jpop")) ? 7.0f :
            id.contains(QStringLiteral("rnb")) ? 8.5f :
            id.contains(QStringLiteral("soul")) ? 7.5f :
            id.contains(QStringLiteral("reggae")) ? 4.0f : 5.0f;
        patch.vibratoRate =
            id.contains(QStringLiteral("jpop")) ? 6.0f :
            id.contains(QStringLiteral("rnb")) ? 4.8f : 5.2f;
        patch.vibratoDelay =
            id.contains(QStringLiteral("rnb")) ? 0.26f : 0.18f;
        patch.delayMix = 0.10f;
    } else if (id.contains(QStringLiteral("breakbeat-lead"))) {
        patch.name = QStringLiteral(
            "Angular Z-oscillator Breakbeat lead");
        patch.source = SourceKind::Z;
        patch.formantRatio = 2.65f;
        patch.spectralShape = 0.42f;
        patch.spectralMode = -0.24f;
        patch.attack = 0.006f;
        patch.decay = 0.20f;
        patch.sustain = 0.66f;
        patch.release = 0.18f;
        patch.filterCutoff = 4200.0f;
        patch.filterEnvelope = 1800.0f;
        patch.wavefold = 1.18f;
        patch.vibratoCents = 2.0f;
        patch.transientMix = 0.030f;
    } else if (role == QStringLiteral("melody")) {
        patch.name = id.contains(QStringLiteral("garage"))
            ? QStringLiteral("Raw variable-saw Garage lead")
            : id.contains(QStringLiteral("funk"))
                ? QStringLiteral("Warm rounded shape lead")
                : QStringLiteral("Rounded subtractive lead");
        patch.source = id.contains(QStringLiteral("garage"))
            ? SourceKind::VariableSaw : SourceKind::Shape;
        patch.shape = id.contains(QStringLiteral("funk")) ? 0.18f : 0.42f;
        patch.width = 0.46f;
        patch.oscillator2Mix =
            id.contains(QStringLiteral("garage")) ? 0.28f : 0.12f;
        patch.detuneCents =
            id.contains(QStringLiteral("garage")) ? 5.0f : 1.5f;
        patch.attack = 0.012f;
        patch.decay = 0.22f;
        patch.sustain = 0.72f;
        patch.release = 0.24f;
        patch.filterCutoff =
            id.contains(QStringLiteral("garage")) ? 4800.0f : 2800.0f;
        patch.filterEnvelope = 1100.0f;
        patch.voiceDrive =
            id.contains(QStringLiteral("garage")) ? 1.45f : 1.08f;
        patch.filter = id.contains(QStringLiteral("garage"))
            ? FilterKind::Ladder : FilterKind::StateVariableLowpass;
        patch.transientMix =
            id.contains(QStringLiteral("garage")) ? 0.055f : 0.0f;
        patch.vibratoCents =
            id.contains(QStringLiteral("garage")) ? 2.0f :
            id.contains(QStringLiteral("funk")) ? 4.0f : 5.0f;
    }

    if (style == QStringLiteral("jazz")) {
        patch.busDrive = std::min(patch.busDrive, 1.12f);
        patch.filterDrive = std::min(patch.filterDrive, 1.28f);
    } else if (style == QStringLiteral("electronic")) {
        patch.resonance = std::max(patch.resonance, 0.44f);
        patch.filterDrive = std::max(patch.filterDrive, 1.55f);
    } else if (style == QStringLiteral("reggae")) {
        patch.delayMix = role == QStringLiteral("support")
            ? 0.22f : patch.delayMix;
        patch.delaySeconds = 0.36f;
    } else if (style == QStringLiteral("bossa-nova")) {
        patch.voiceDrive = std::min(patch.voiceDrive, 1.08f);
        patch.busDrive = 1.0f;
    } else if (style == QStringLiteral("hiphop-trap") &&
               profile.id == QStringLiteral("hiphop_boom_bap")) {
        patch.filterCutoff *= 0.72f;
        patch.busDrive = 1.18f;
    }
    return patch;
}

struct ActiveVoice {
    bool active = false;
    bool trigger = false;
    qint64 startFrame = 0;
    qint64 endFrame = 1;
    float frequency = 440.0f;
    float velocity = 0.8f;
    PatchDesign patch;

    daisysp::VariableShapeOscillator oscillatorA;
    daisysp::VariableShapeOscillator oscillatorB;
    daisysp::VariableSawOscillator sawA;
    daisysp::VariableSawOscillator sawB;
    daisysp::Oscillator sub;
    daisysp::Fm2 fm;
    daisysp::FormantOscillator formant;
    daisysp::VosimOscillator vosim;
    daisysp::ZOscillator z;
    daisysp::HarmonicOscillator<16> harmonic;
    daisysp::StringVoice stringA;
    daisysp::StringVoice stringB;
    daisysp::WhiteNoise noise;
    daisysp::Adsr amplitude;
    daisysp::Adsr modulation;
    daisysp::LadderFilter ladder;
    daisysp::Svf body;
    daisysp::Svf tone;
    daisysp::Wavefolder folder;

    void begin(const NoteEvent& event, const PatchDesign& design)
    {
        active = true;
        trigger = true;
        startFrame = event.start;
        endFrame = event.end;
        frequency = static_cast<float>(midiFrequency(event.midi));
        velocity = event.velocity / 127.0f;
        patch = design;

        if (event.articulation.contains(QStringLiteral("short")) ||
            event.articulation.contains(QStringLiteral("muted")) ||
            event.articulation.contains(QStringLiteral("choke"))) {
            endFrame = std::min<qint64>(
                endFrame,
                startFrame + static_cast<qint64>(
                    kSampleRate * (event.articulation.contains(
                        QStringLiteral("choke")) ? 0.08 : 0.18)));
            patch.release = std::min(patch.release, 0.08f);
            patch.stringDamping =
                std::max(patch.stringDamping, 0.84f);
        }

        oscillatorA.Init(kSampleRate);
        oscillatorB.Init(kSampleRate);
        sawA.Init(kSampleRate);
        sawB.Init(kSampleRate);
        sub.Init(kSampleRate);
        fm.Init(kSampleRate);
        formant.Init(kSampleRate);
        vosim.Init(kSampleRate);
        z.Init(kSampleRate);
        harmonic.Init(kSampleRate);
        stringA.Init(kSampleRate);
        stringB.Init(kSampleRate);
        noise.Init();
        noise.SetSeed(static_cast<std::int32_t>(
            (event.start + 1) * 1103515245ULL +
            static_cast<std::uint64_t>(event.midi + 1) * 12345ULL));
        amplitude.Init(kSampleRate);
        modulation.Init(kSampleRate);
        ladder.Init(kSampleRate);
        body.Init(kSampleRate);
        tone.Init(kSampleRate);
        folder.Init();

        setFreeRunningFrequency(oscillatorA, frequency);
        oscillatorA.SetWaveshape(patch.shape);
        oscillatorA.SetPW(patch.width);
        setFreeRunningFrequency(
            oscillatorB,
            frequency * std::pow(2.0f, patch.detuneCents / 1200.0f));
        oscillatorB.SetWaveshape(
            std::clamp(patch.shape + 0.24f, 0.0f, 1.0f));
        oscillatorB.SetPW(
            std::clamp(1.0f - patch.width, 0.08f, 0.92f));
        sawA.SetFreq(frequency);
        sawA.SetPW(std::clamp(
            patch.width, 0.05f, 0.95f));
        sawA.SetWaveshape(patch.shape);
        sawB.SetFreq(
            frequency * std::pow(
                2.0f, patch.detuneCents / 1200.0f));
        sawB.SetPW(std::clamp(
            1.0f - patch.width, 0.05f, 0.95f));
        sawB.SetWaveshape(
            std::clamp(patch.shape + 0.18f, 0.0f, 1.0f));
        sub.SetFreq(
            patch.source == SourceKind::Sine
                ? frequency : frequency * 0.5f);
        sub.SetAmp(1.0f);
        sub.SetWaveform(daisysp::Oscillator::WAVE_SIN);
        fm.SetFrequency(frequency);
        fm.SetRatio(patch.fmRatio);
        fm.SetIndex(patch.fmIndex);
        formant.SetCarrierFreq(frequency);
        formant.SetFormantFreq(
            patch.formantHz > 0.0f
                ? patch.formantHz
                : frequency * patch.formantRatio);
        formant.SetPhaseShift(patch.spectralShape);
        vosim.SetFreq(frequency);
        vosim.SetForm1Freq(
            patch.formantHz > 0.0f
                ? patch.formantHz
                : frequency * patch.formantRatio);
        vosim.SetForm2Freq(
            patch.formantHz2 > 0.0f
                ? patch.formantHz2
                : frequency * patch.formantRatio2);
        vosim.SetShape(patch.spectralShape);
        z.SetFreq(frequency);
        z.SetFormantFreq(frequency * patch.formantRatio);
        z.SetShape(patch.spectralShape);
        z.SetMode(patch.spectralMode);
        harmonic.SetFreq(frequency);
        std::array<float, 16> amplitudes{};
        if (patch.harmonicFamily == 1) {
            amplitudes[0] = 0.34f;
            amplitudes[1] = 0.24f;
            amplitudes[2] = 0.10f;
            amplitudes[3] = 0.13f;
            amplitudes[5] = 0.08f;
            amplitudes[7] = 0.045f;
        } else if (patch.harmonicFamily == 2) {
            amplitudes[0] = 0.36f;
            amplitudes[1] = 0.20f;
            amplitudes[2] = 0.14f;
            amplitudes[3] = 0.09f;
            amplitudes[4] = 0.06f;
            amplitudes[5] = 0.04f;
        } else if (patch.harmonicFamily == 3) {
            amplitudes[0] = 0.30f;
            amplitudes[1] = 0.23f;
            amplitudes[2] = 0.16f;
            amplitudes[3] = 0.11f;
            amplitudes[4] = 0.075f;
            amplitudes[5] = 0.050f;
            amplitudes[6] = 0.035f;
            amplitudes[7] = 0.020f;
        } else if (patch.harmonicFamily == 4) {
            amplitudes[0] = 0.44f;
            amplitudes[1] = 0.27f;
            amplitudes[2] = 0.13f;
            amplitudes[3] = 0.07f;
            amplitudes[4] = 0.035f;
            amplitudes[5] = 0.018f;
        } else if (patch.harmonicFamily == 5) {
            amplitudes[0] = 0.38f;
            amplitudes[2] = 0.18f;
            amplitudes[4] = 0.11f;
            amplitudes[6] = 0.075f;
            amplitudes[9] = 0.040f;
            amplitudes[12] = 0.022f;
        } else {
            amplitudes[0] = 0.46f;
            amplitudes[1] = 0.16f;
            amplitudes[2] = 0.10f;
            amplitudes[4] = 0.06f;
            amplitudes[6] = 0.03f;
        }
        harmonic.SetAmplitudes(amplitudes.data());

        stringA.SetFreq(frequency);
        stringA.SetStructure(patch.stringStructure);
        stringA.SetBrightness(
            std::clamp(
                patch.stringBrightness +
                    0.14f * (velocity - 0.65f),
                0.0f,
                1.0f));
        stringA.SetDamping(patch.stringDamping);
        stringA.SetAccent(velocity);
        stringA.SetSustain(false);
        stringB.SetFreq(
            frequency * std::pow(
                2.0f, std::max(2.0f, patch.detuneCents) / 1200.0f));
        stringB.SetStructure(
            std::clamp(patch.stringStructure + 0.025f, 0.0f, 1.0f));
        stringB.SetBrightness(
            std::clamp(patch.stringBrightness - 0.04f, 0.0f, 1.0f));
        stringB.SetDamping(
            std::clamp(patch.stringDamping + 0.03f, 0.0f, 1.0f));
        stringB.SetAccent(velocity * 0.94f);
        stringB.SetSustain(false);

        amplitude.SetAttackTime(patch.attack);
        amplitude.SetDecayTime(patch.decay);
        amplitude.SetSustainLevel(
            std::clamp(patch.sustain, 0.01f, 1.0f));
        amplitude.SetReleaseTime(patch.release);
        modulation.SetAttackTime(std::min(0.025f, patch.attack));
        modulation.SetDecayTime(
            std::max(0.04f, std::min(0.85f, patch.decay)));
        modulation.SetSustainLevel(0.025f);
        modulation.SetReleaseTime(std::min(0.35f, patch.release));
        amplitude.Retrigger(true);
        modulation.Retrigger(true);

        ladder.SetFilterMode(
            daisysp::LadderFilter::FilterMode::LP24);
        ladder.SetRes(patch.resonance);
        ladder.SetInputDrive(patch.filterDrive);
        ladder.SetPassbandGain(0.22f);
        body.SetFreq(
            std::clamp(frequency * 2.1f, 140.0f, 3900.0f));
        body.SetRes(0.46f);
        body.SetDrive(0.18f);
        tone.SetRes(std::clamp(patch.resonance, 0.0f, 0.92f));
        tone.SetDrive(std::clamp(
            0.08f + 0.12f * (patch.filterDrive - 1.0f),
            0.0f,
            0.45f));
        folder.SetGain(std::max(1.0f, patch.wavefold));
    }

    float process(qint64 frame)
    {
        if (!active) return 0.0f;
        const bool gate = frame < endFrame;
        const float amp = amplitude.Process(gate);
        const float mod = modulation.Process(gate);
        const float age =
            static_cast<float>(frame - startFrame) / kSampleRate;
        const float pitchMod = age > patch.vibratoDelay &&
                patch.vibratoCents > 0.0f
            ? (std::pow(
                    2.0f,
                    patch.vibratoCents *
                        static_cast<float>(std::clamp(
                            (age - patch.vibratoDelay) / 0.24f,
                            0.0f,
                            1.0f)) *
                        std::sin(
                            2.0f * static_cast<float>(kPi) *
                            patch.vibratoRate * age) /
                        1200.0f) -
                1.0f)
            : 0.0f;
        const float tremolo = patch.tremoloDepth > 0.0f
            ? 1.0f - patch.tremoloDepth +
                patch.tremoloDepth *
                (0.5f + 0.5f *
                static_cast<float>(std::clamp(
                    std::sin(
                        2.0f * static_cast<float>(kPi) *
                        patch.tremoloRate * age),
                    -1.0f,
                    1.0f)))
            : 1.0f;
        float value = 0.0f;
        switch (patch.source) {
        case SourceKind::Jam2Native:
            // Jam2 is rendered by PracticeReferenceRenderer at the request
            // boundary, never inside the Daisy real-time voice.
            value = 0.0f;
            break;
        case SourceKind::Shape:
            setFreeRunningFrequency(
                oscillatorA,
                frequency * (1.0f + pitchMod));
            value = oscillatorA.Process();
            if (patch.oscillator2Mix > 0.0f) {
                setFreeRunningFrequency(
                    oscillatorB,
                    frequency *
                        std::pow(
                            2.0f,
                            patch.detuneCents / 1200.0f) *
                        (1.0f + 0.72f * pitchMod));
                value = (1.0f - patch.oscillator2Mix) * value +
                    patch.oscillator2Mix * oscillatorB.Process();
            }
            value += patch.subMix * sub.Process();
            break;
        case SourceKind::VariableSaw:
            sawA.SetFreq(
                frequency * (1.0f + pitchMod));
            value = sawA.Process();
            if (patch.oscillator2Mix > 0.0f) {
                sawB.SetFreq(
                    frequency *
                    std::pow(
                        2.0f,
                        patch.detuneCents / 1200.0f) *
                    (1.0f + 0.72f * pitchMod));
                value = (1.0f - patch.oscillator2Mix) * value +
                    patch.oscillator2Mix * sawB.Process();
            }
            value += patch.subMix * sub.Process();
            break;
        case SourceKind::Fm:
            fm.SetFrequency(frequency * (1.0f + pitchMod));
            fm.SetIndex(
                0.18f +
                patch.fmIndex * (0.22f + 0.78f * mod) *
                    (0.72f + 0.40f * velocity));
            value = fm.Process() + patch.subMix * sub.Process();
            break;
        case SourceKind::String: {
            stringA.SetFreq(frequency * (1.0f + pitchMod));
            stringB.SetFreq(
                frequency *
                std::pow(
                    2.0f,
                    std::max(2.0f, patch.detuneCents) / 1200.0f) *
                (1.0f + 0.65f * pitchMod));
            const float left = stringA.Process(trigger);
            const float right = stringB.Process(trigger);
            value = std::tanh(patch.voiceDrive * left);
            if (patch.stringDouble > 0.0f) {
                value = (1.0f - 0.42f * patch.stringDouble) * value +
                    0.42f * patch.stringDouble *
                        std::tanh(patch.voiceDrive * right);
            }
            body.SetFreq(
                std::clamp(frequency * 2.1f, 140.0f, 3900.0f));
            body.Process(value);
            value = 0.82f * value + 0.18f * body.Band();
            trigger = false;
            break;
        }
        case SourceKind::Harmonic:
            harmonic.SetFreq(frequency * (1.0f + pitchMod));
            value = harmonic.Process();
            if (patch.oscillator2Mix > 0.0f) {
                value += 0.24f * patch.oscillator2Mix *
                    oscillatorB.Process();
            }
            break;
        case SourceKind::Sine:
            sub.SetFreq(frequency * (1.0f + pitchMod));
            setFreeRunningFrequency(oscillatorA, frequency * 2.0f);
            value = sub.Process() +
                patch.subMix * 0.35f * oscillatorA.Process();
            break;
        case SourceKind::Formant:
            formant.SetCarrierFreq(
                frequency * (1.0f + pitchMod));
            formant.SetFormantFreq(
                (patch.formantHz > 0.0f
                    ? patch.formantHz
                    : frequency * patch.formantRatio) *
                (1.0f + 0.08f * mod));
            value = formant.Process();
            break;
        case SourceKind::Vosim:
            vosim.SetFreq(
                frequency * (1.0f + pitchMod));
            vosim.SetForm1Freq(
                (patch.formantHz > 0.0f
                    ? patch.formantHz
                    : frequency * patch.formantRatio) *
                (1.0f + 0.05f * mod));
            vosim.SetForm2Freq(
                (patch.formantHz2 > 0.0f
                    ? patch.formantHz2
                    : frequency * patch.formantRatio2) *
                (1.0f - 0.035f * mod));
            value = vosim.Process();
            break;
        case SourceKind::Z:
            z.SetFreq(
                frequency * (1.0f + pitchMod));
            z.SetFormantFreq(
                frequency * patch.formantRatio *
                (1.0f + 0.11f * mod));
            value = z.Process();
            break;
        }

        const float noiseValue = noise.Process();
        value += patch.noiseMix * noiseValue;
        if (patch.transientMix > 0.0f) {
            value += patch.transientMix * noiseValue *
                std::exp(
                    -age /
                    std::max(0.001f, patch.transientSeconds));
        }
        if (patch.wavefold > 1.0f) value = folder.Process(value);
        const float keyTrack =
            std::clamp(frequency * 0.72f, 40.0f, 2300.0f);
        const float cutoff = std::clamp(
            patch.filterCutoff + keyTrack +
                patch.filterEnvelope * mod *
                    (0.72f + 0.38f * velocity),
            35.0f,
            15500.0f);
        switch (patch.filter) {
        case FilterKind::Ladder:
            ladder.SetFreq(cutoff);
            value = ladder.Process(value);
            break;
        case FilterKind::StateVariableLowpass:
            tone.SetFreq(cutoff);
            tone.Process(value);
            value = tone.Low();
            break;
        case FilterKind::StateVariableBandpass:
            tone.SetFreq(cutoff);
            tone.Process(value);
            value = 0.72f * tone.Band() + 0.28f * tone.Low();
            break;
        case FilterKind::Direct:
            break;
        }
        if (patch.source != SourceKind::String &&
            patch.voiceDrive > 1.01f) {
            value = std::tanh(patch.voiceDrive * value);
        }
        if (!gate && !amplitude.IsRunning()) active = false;
        return tremolo * amp * velocity * value;
    }
};

void applyVoiceBus(
    std::vector<float>& audio,
    const PatchDesign& patch)
{
    if (audio.empty()) return;
    const double cabinetCutoff =
        patch.cabinet > 0.0f
            ? 3300.0 + (1.0 - patch.cabinet) * 4100.0
            : 15000.0;
    const double coefficient =
        1.0 - std::exp(-2.0 * kPi * cabinetCutoff / kSampleRate);
    const std::size_t delayFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(patch.delaySeconds * kSampleRate));
    std::vector<float> delay(delayFrames, 0.0f);
    daisysp::Chorus chorus;
    chorus.Init(kSampleRate);
    chorus.SetLfoDepth(
        std::clamp(patch.chorusDepth, 0.0f, 1.0f));
    chorus.SetLfoFreq(
        std::clamp(patch.chorusRate, 0.02f, 8.0f));
    chorus.SetDelayMs(11.0f, 17.0f);
    chorus.SetFeedback(0.08f, 0.05f);
    double low = 0.0;
    double previousLow = 0.0;
    for (std::size_t frame = 0; frame < audio.size(); ++frame) {
        double value = std::tanh(patch.busDrive * audio[frame]);
        low += coefficient * (value - low);
        if (patch.cabinet > 0.0f) {
            const double highPassed =
                low - previousLow + 0.985 * audio[frame];
            previousLow = low;
            value = (1.0 - 0.18 * patch.cabinet) * low +
                0.12 * patch.cabinet * highPassed;
        } else {
            value = low;
        }
        if (patch.chorusMix > 0.0f) {
            chorus.Process(static_cast<float>(value));
            const double wet =
                0.5 * (chorus.GetLeft() + chorus.GetRight());
            value =
                (1.0 - patch.chorusMix) * value +
                patch.chorusMix * wet;
        }
        const std::size_t delayIndex = frame % delay.size();
        const double delayed = delay[delayIndex];
        delay[delayIndex] = static_cast<float>(
            value + delayed * (0.16 + 0.34 * patch.delayMix));
        audio[frame] = static_cast<float>(
            value + patch.delayMix * delayed);
    }
}

std::vector<float> renderDaisyRole(
    const std::vector<NoteEvent>& events,
    std::size_t frames,
    const QString& role,
    const PatchDesign& patch,
    bool renderBus = true)
{
    std::vector<NoteEvent> roleEvents;
    for (const NoteEvent& event : events) {
        if (event.role == role) roleEvents.push_back(event);
    }
    std::vector<float> output(frames, 0.0f);
    std::array<ActiveVoice, 32> voices;
    std::size_t next = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        while (next < roleEvents.size() &&
               roleEvents[next].start <= static_cast<qint64>(frame)) {
            auto voice = std::find_if(
                voices.begin(),
                voices.end(),
                [](const ActiveVoice& candidate) {
                    return !candidate.active;
                });
            if (voice == voices.end()) {
                voice = std::min_element(
                    voices.begin(),
                    voices.end(),
                    [](const ActiveVoice& left,
                       const ActiveVoice& right) {
                        return left.startFrame < right.startFrame;
                    });
            }
            voice->begin(roleEvents[next], patch);
            ++next;
        }
        float value = 0.0f;
        int activeCount = 0;
        for (ActiveVoice& voice : voices) {
            if (!voice.active) continue;
            value += voice.process(static_cast<qint64>(frame));
            ++activeCount;
        }
        if (activeCount > 0) {
            value /= std::sqrt(static_cast<float>(activeCount));
        }
        output[frame] = 0.62f * value;
    }
    if (renderBus) applyVoiceBus(output, patch);
    return output;
}

enum class DrumKind {
    Kick,
    Snare,
    ClosedHat,
    OpenHat,
    HighTom,
    MidTom,
    FloorTom,
    Crash,
    Ride,
    CrossStick,
    Shaker,
    HandPercussion,
};

bool isTomKind(DrumKind kind)
{
    return kind == DrumKind::HighTom ||
        kind == DrumKind::MidTom ||
        kind == DrumKind::FloorTom;
}

struct DrumHit {
    enum class Strength {
        Ghost,
        Normal,
        Accent,
    };

    qint64 frame = 0;
    DrumKind kind = DrumKind::Kick;
    float level = 0.7f;
    Strength strength = Strength::Normal;
    int repeatIndex = 0;
    int midiVelocity = 90;
    float excitation = 0.7f;
    float outputGain = 0.7f;
    float brightnessOffset = 0.0f;
    float decayScale = 1.0f;
    float driveScale = 1.0f;
    bool exactPerformanceVelocity = false;
    QString articulation;
    bool fill = false;
};

std::optional<DrumKind> drumKind(const QString& lane)
{
    if (lane == QStringLiteral("Kick")) return DrumKind::Kick;
    if (lane == QStringLiteral("Snare")) return DrumKind::Snare;
    if (lane == QStringLiteral("Closed HH")) return DrumKind::ClosedHat;
    if (lane == QStringLiteral("Open HH")) return DrumKind::OpenHat;
    if (lane == QStringLiteral("High Tom")) return DrumKind::HighTom;
    if (lane == QStringLiteral("Mid Tom")) return DrumKind::MidTom;
    if (lane == QStringLiteral("Floor Tom")) return DrumKind::FloorTom;
    if (lane == QStringLiteral("Tom")) return DrumKind::MidTom;
    if (lane == QStringLiteral("Crash")) return DrumKind::Crash;
    if (lane == QStringLiteral("Ride")) return DrumKind::Ride;
    if (lane == QStringLiteral("Cross-stick / Rim")) {
        return DrumKind::CrossStick;
    }
    if (lane == QStringLiteral("Shaker")) return DrumKind::Shaker;
    if (lane == QStringLiteral("Hand Percussion")) {
        return DrumKind::HandPercussion;
    }
    return std::nullopt;
}

std::optional<DrumKind> performanceDrumKind(
    const QString& laneId)
{
    if (laneId == QStringLiteral("kick")) return DrumKind::Kick;
    if (laneId == QStringLiteral("snare")) return DrumKind::Snare;
    if (laneId == QStringLiteral("closed_hat")) {
        return DrumKind::ClosedHat;
    }
    if (laneId == QStringLiteral("open_hat")) {
        return DrumKind::OpenHat;
    }
    if (laneId == QStringLiteral("high_tom")) {
        return DrumKind::HighTom;
    }
    if (laneId == QStringLiteral("mid_tom")) {
        return DrumKind::MidTom;
    }
    if (laneId == QStringLiteral("floor_tom")) {
        return DrumKind::FloorTom;
    }
    if (laneId == QStringLiteral("crash")) return DrumKind::Crash;
    if (laneId == QStringLiteral("ride")) return DrumKind::Ride;
    if (laneId == QStringLiteral("cross_stick")) {
        return DrumKind::CrossStick;
    }
    if (laneId == QStringLiteral("shaker")) return DrumKind::Shaker;
    if (laneId == QStringLiteral("hand_percussion")) {
        return DrumKind::HandPercussion;
    }
    return std::nullopt;
}

QString performanceLaneName(const QString& laneId)
{
    if (laneId == QStringLiteral("kick")) {
        return QStringLiteral("Kick");
    }
    if (laneId == QStringLiteral("snare")) {
        return QStringLiteral("Snare");
    }
    if (laneId == QStringLiteral("closed_hat")) {
        return QStringLiteral("Closed HH");
    }
    if (laneId == QStringLiteral("open_hat")) {
        return QStringLiteral("Open HH");
    }
    if (laneId == QStringLiteral("high_tom")) {
        return QStringLiteral("High Tom");
    }
    if (laneId == QStringLiteral("mid_tom")) {
        return QStringLiteral("Mid Tom");
    }
    if (laneId == QStringLiteral("floor_tom")) {
        return QStringLiteral("Floor Tom");
    }
    if (laneId == QStringLiteral("crash")) {
        return QStringLiteral("Crash");
    }
    if (laneId == QStringLiteral("ride")) {
        return QStringLiteral("Ride");
    }
    if (laneId == QStringLiteral("cross_stick")) {
        return QStringLiteral("Cross-stick / Rim");
    }
    if (laneId == QStringLiteral("shaker")) {
        return QStringLiteral("Shaker");
    }
    if (laneId == QStringLiteral("hand_percussion")) {
        return QStringLiteral("Hand Percussion");
    }
    return {};
}

bool usesProductionDrumPerformance(
    const GeneratedPracticeIdea& idea)
{
    return !idea.recipe.drumEvents.isEmpty() &&
        jam2::practice::generatedBeatFingerprint(
            idea.beatSection) ==
            idea.recipe.beatFingerprint;
}

QChar performanceState(
    const GeneratedPracticeIdea& idea,
    const jam2::practice::DrumPerformanceEvent& event)
{
    const int beat = event.tick / kTicksPerBeat;
    const int within = event.tick % kTicksPerBeat;
    if (beat < 0 ||
        beat >= idea.beatSection.beatPatterns.size()) {
        return QLatin1Char('x');
    }
    const BeatPattern& pattern =
        idea.beatSection.beatPatterns.at(beat);
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const QString laneName =
        performanceLaneName(event.laneId);
    if (laneName.isEmpty() || pattern.division <= 0) {
        return QLatin1Char('x');
    }
    const int lane =
        lanes.indexOf(laneName);
    if (lane < 0 || lane >= pattern.lanes.size()) {
        return QLatin1Char('x');
    }
    const int step =
        within * pattern.division / kTicksPerBeat;
    if (step < 0 ||
        step >= pattern.lanes.at(lane).size() ||
        step * kTicksPerBeat / pattern.division != within) {
        return QLatin1Char('x');
    }
    return pattern.lanes.at(lane).at(step).toLower();
}

std::vector<DrumHit> extractDrumHits(
    const GeneratedPracticeIdea& idea,
    std::size_t frames,
    std::optional<DrumKind> selectedTom = std::nullopt)
{
    const QStringList lanes = BeatGridModel::beatLaneNames();
    std::vector<DrumHit> hits;
    std::array<int, 12> repeatCounts{};
    if (usesProductionDrumPerformance(idea)) {
        const int maximumTick =
            idea.beatSection.beats * kTicksPerBeat;
        for (const auto& event : idea.recipe.drumEvents) {
            if (event.tick < 0) continue;
            if (event.tick >= maximumTick) break;
            const std::optional<DrumKind> sourceKind =
                performanceDrumKind(event.laneId);
            if (!sourceKind) continue;
            const DrumKind kind =
                *sourceKind == DrumKind::MidTom &&
                        selectedTom && isTomKind(*selectedTom)
                    ? *selectedTom
                    : *sourceKind;
            double beatPosition =
                static_cast<double>(event.tick) /
                kTicksPerBeat;
            const int within =
                event.tick % kTicksPerBeat;
            int laneSwing = idea.recipe.swingPercent;
            if (kind == DrumKind::Kick) {
                laneSwing =
                    50 +
                    (idea.recipe.swingPercent - 50) / 2;
            } else if (
                kind == DrumKind::Snare ||
                kind == DrumKind::CrossStick) {
                laneSwing = 50;
            }
            const int pairStart = within >= 6 ? 6 : 0;
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
            double frame =
                beatPosition * framesPerBeat(idea);
            frame += event.offsetMs *
                kSampleRate / 1000.0;
            const qint64 bounded = std::clamp<qint64>(
                static_cast<qint64>(std::llround(frame)),
                0,
                static_cast<qint64>(
                    std::max<std::size_t>(1, frames) - 1));
            const QChar state =
                performanceState(idea, event);
            DrumHit hit;
            hit.frame = bounded;
            hit.kind = kind;
            hit.strength =
                state == QLatin1Char('a')
                    ? DrumHit::Strength::Accent
                    : state == QLatin1Char('g')
                        ? DrumHit::Strength::Ghost
                        : DrumHit::Strength::Normal;
            hit.repeatIndex =
                qMax(0, event.repeatGroup);
            hit.midiVelocity =
                std::clamp(event.velocity, 1, 127);
            hit.level =
                static_cast<float>(hit.midiVelocity) /
                127.0f;
            hit.excitation = hit.level;
            hit.outputGain = hit.level;
            hit.exactPerformanceVelocity = true;
            hit.articulation = event.articulation;
            hit.fill = event.fill;
            hits.push_back(hit);
        }
        std::stable_sort(
            hits.begin(),
            hits.end(),
            [](const DrumHit& left, const DrumHit& right) {
                return left.frame < right.frame;
            });
        return hits;
    }

    for (int beat = 0; beat < idea.beatSection.beats; ++beat) {
        if (beat >= idea.beatSection.beatPatterns.size()) continue;
        const BeatPattern& pattern =
            idea.beatSection.beatPatterns.at(beat);
        for (int laneIndex = 0;
             laneIndex < pattern.lanes.size() &&
             laneIndex < lanes.size();
             ++laneIndex) {
            const std::optional<DrumKind> laneKind =
                drumKind(lanes.at(laneIndex));
            if (!laneKind || pattern.division <= 0) continue;
            const DrumKind kind =
                *laneKind == DrumKind::MidTom &&
                        selectedTom && isTomKind(*selectedTom)
                    ? *selectedTom
                    : *laneKind;
            const QString states =
                pattern.lanes.at(laneIndex).trimmed().toLower();
            for (int step = 0;
                 step < pattern.division && step < states.size();
                 ++step) {
                const QChar state = states.at(step);
                if (state != QLatin1Char('x') &&
                    state != QLatin1Char('a') &&
                    state != QLatin1Char('g')) {
                    continue;
                }
                double position =
                    static_cast<double>(step) / pattern.division;
                if ((pattern.division == 2 ||
                     pattern.division == 4 ||
                     pattern.division == 8) &&
                    (step & 1)) {
                    const int swing =
                        kind == DrumKind::Kick
                            ? 50 + (idea.recipe.swingPercent - 50) / 2
                            : (kind == DrumKind::Snare ||
                               kind == DrumKind::CrossStick)
                                ? 50 : idea.recipe.swingPercent;
                    position =
                        (step - 1 + 2.0 * swing / 100.0) /
                        pattern.division;
                }
                double frame =
                    (beat + position) * framesPerBeat(idea);
                if (kind == DrumKind::Snare) {
                    frame += idea.recipe.snareOffsetMs *
                        kSampleRate / 1000.0;
                }
                const qint64 bounded = std::clamp<qint64>(
                    static_cast<qint64>(std::llround(frame)),
                    0,
                    static_cast<qint64>(
                        std::max<std::size_t>(1, frames) - 1));
                const float level =
                    state == QLatin1Char('a') ? 1.0f :
                    state == QLatin1Char('g') ? 0.32f : 0.72f;
                const DrumHit::Strength strength =
                    state == QLatin1Char('a')
                        ? DrumHit::Strength::Accent
                        : state == QLatin1Char('g')
                            ? DrumHit::Strength::Ghost
                            : DrumHit::Strength::Normal;
                const std::size_t kindIndex =
                    static_cast<std::size_t>(kind);
                DrumHit hit;
                hit.frame = bounded;
                hit.kind = kind;
                hit.level = level;
                hit.strength = strength;
                hit.repeatIndex = repeatCounts[kindIndex]++;
                hit.midiVelocity = std::clamp(
                    static_cast<int>(std::lround(level * 127.0f)),
                    1,
                    127);
                hit.excitation = level;
                hit.outputGain = level;
                hits.push_back(hit);
            }
        }
    }
    std::stable_sort(
        hits.begin(),
        hits.end(),
        [](const DrumHit& left, const DrumHit& right) {
            return left.frame < right.frame;
        });
    return hits;
}

enum class DaisyDrumModel {
    AnalogKick,
    SyntheticKick,
    AnalogSnare,
    SyntheticSnare,
    ShellSnare,
    Hat,
    RingHat,
    Modal,
    Particle,
    ShellTom,
    RimWood,
    CollisionShaker,
    SkinHandDrum,
    HandClap,
    WoodBlock,
    Tambourine,
    CrashCymbal,
    RideCymbal,
    Crash,
};

struct ActiveDrum {
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
        Particle,
        Air,
        MetalWash,
    };

    DaisyDrumModel model = DaisyDrumModel::AnalogKick;
    std::unique_ptr<daisysp::AnalogBassDrum> analogKick;
    std::unique_ptr<daisysp::SyntheticBassDrum> syntheticKick;
    std::unique_ptr<daisysp::AnalogSnareDrum> analogSnare;
    std::unique_ptr<daisysp::SyntheticSnareDrum> syntheticSnare;
    std::unique_ptr<daisysp::HiHat<>> hat;
    std::unique_ptr<
        daisysp::HiHat<daisysp::RingModNoise>> ringHat;
    std::unique_ptr<daisysp::ModalVoice> modal;
    std::unique_ptr<daisysp::Particle> particle;
    std::unique_ptr<daisysp::Oscillator> crashA;
    std::unique_ptr<daisysp::Oscillator> crashB;
    std::unique_ptr<daisysp::Oscillator> crashC;
    std::unique_ptr<daisysp::Svf> crashFilter;
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
    qint64 end = 1;
    int fadeStartAge = std::numeric_limits<int>::max();
    int endAge = std::numeric_limits<int>::max();
    float gain = 0.3f;
    float crashEnvelope = 1.0f;
    float crashDecay = 0.9999f;
    float crashMetalMix = 0.16f;
    std::uint32_t noiseState = 0x91e10da5U;
    Transient transient = Transient::Off;
    Texture texture = Texture::Off;
    std::unique_ptr<daisysp::Svf> transientFilter;
    std::unique_ptr<daisysp::Svf> textureFilter;
    std::unique_ptr<daisysp::Particle> textureParticle;
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
    int clapBurst = 0;
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
    bool trigger = true;

    float processIdentityModes(float pitchScale = 1.0f)
    {
        float value = 0.0f;
        for (int index = 0; index < identityModeCount; ++index) {
            identityPhase[index] +=
                identityFrequency[index] * pitchScale / kSampleRate;
            identityPhase[index] -=
                std::floor(identityPhase[index]);
            value +=
                std::sin(
                    static_cast<float>(2.0 * kPi) *
                    identityPhase[index]) *
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
        case DaisyDrumModel::AnalogKick:
            base = analogKick->Process(fire);
            break;
        case DaisyDrumModel::SyntheticKick:
            base = syntheticKick->Process(fire);
            break;
        case DaisyDrumModel::AnalogSnare:
            base = analogSnare->Process(fire);
            break;
        case DaisyDrumModel::SyntheticSnare:
            base = syntheticSnare->Process(fire);
            break;
        case DaisyDrumModel::ShellSnare:
            identityFilterA->Process(noise);
            base =
                processIdentityModes() +
                0.075f *
                    (identityFilterA->Low() +
                     0.26f * identityFilterA->Band()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        case DaisyDrumModel::Hat:
            base = hat->Process(fire);
            break;
        case DaisyDrumModel::RingHat:
            base = ringHat->Process(fire);
            break;
        case DaisyDrumModel::Modal:
            base = modal->Process(fire);
            break;
        case DaisyDrumModel::Particle:
            base = particle->Process() * textureEnvelope;
            if (texture == Texture::Off) {
                textureEnvelope *= textureDecay;
            }
            break;
        case DaisyDrumModel::ShellTom: {
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
        case DaisyDrumModel::RimWood: {
            identityFilterA->Process(noise);
            base =
                processIdentityModes() +
                0.42f *
                    (identityFilterA->Band() +
                     0.40f * identityFilterA->High()) *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        }
        case DaisyDrumModel::CollisionShaker: {
            const float randomUnit =
                static_cast<float>(noiseState & 0x00ffffffU) /
                static_cast<float>(0x01000000U);
            if (randomUnit <
                identityCollisionRate / kSampleRate) {
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
        case DaisyDrumModel::SkinHandDrum: {
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
        case DaisyDrumModel::HandClap: {
            identityFilterA->Process(noise);
            identityFilterB->Process(noise);
            const auto burstEnvelope =
                [this](int start, int width, float gain) {
                    const int local = age - start;
                    if (local < 0 || local >= width) return 0.0f;
                    return gain * std::exp(
                        -4.2f * static_cast<float>(local) /
                        std::max(1, width));
                };
            const int burstWidth =
                static_cast<int>(0.005f * kSampleRate);
            const float earlyBursts = std::max({
                burstEnvelope(0, burstWidth, 1.0f),
                burstEnvelope(
                    static_cast<int>(0.010f * kSampleRate),
                    burstWidth,
                    0.86f),
                burstEnvelope(
                    static_cast<int>(0.019f * kSampleRate),
                    burstWidth,
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
                (0.82f * earlyBursts +
                 0.22f * identityNoiseEnvelope);
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        }
        case DaisyDrumModel::WoodBlock: {
            identityFilterA->Process(noise);
            base =
                processIdentityModes() +
                0.10f * identityFilterA->High() *
                    identityNoiseEnvelope;
            identityNoiseEnvelope *= identityNoiseDecay;
            break;
        }
        case DaisyDrumModel::Tambourine: {
            const float randomUnit =
                static_cast<float>(noiseState & 0x00ffffffU) /
                static_cast<float>(0x01000000U);
            if (randomUnit <
                identityCollisionRate / kSampleRate) {
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
        case DaisyDrumModel::CrashCymbal:
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
        case DaisyDrumModel::RideCymbal:
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
        case DaisyDrumModel::Crash: {
            const float metal =
                (crashA->Process() +
                 crashB->Process() +
                 crashC->Process()) / 3.0f;
            crashFilter->Process(
                (1.0f - crashMetalMix) * noise +
                crashMetalMix * metal);
            const float value =
                (0.78f * crashFilter->High() +
                 0.22f * crashFilter->Band()) *
                crashEnvelope;
            crashEnvelope *= crashDecay;
            base = value;
            break;
        }
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
            overlayPhase += frequency / kSampleRate;
            overlayPhase -= std::floor(overlayPhase);
            const float tone =
                std::sin(
                    static_cast<float>(2.0 * kPi) * overlayPhase);
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
                const int first = static_cast<int>(
                    0.009 * kSampleRate);
                const int second = static_cast<int>(
                    0.018 * kSampleRate);
                const int width = static_cast<int>(
                    0.004 * kSampleRate);
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
            transientValue *=
                transientLevel * transientEnvelope;
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
            case Texture::Particle:
                textureValue = textureParticle->Process();
                break;
            case Texture::Air:
                textureValue = textureFilter->High();
                break;
            case Texture::MetalWash:
                textureValue =
                    0.62f * textureFilter->Band() +
                    0.38f * std::sin(
                        static_cast<float>(2.0 * kPi) *
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

ActiveDrum makeDrum(
    const ProfileDefinition& profile,
    const DrumHit& hit,
    std::size_t frames)
{
    ActiveDrum voice;
    const QString style = profile.styleId;
    const bool electronic =
        style == QStringLiteral("electronic") ||
        style == QStringLiteral("hiphop-trap");
    const bool polishedPop =
        style == QStringLiteral("pop") ||
        style == QStringLiteral("jpop-anisong");
    const bool metal =
        profile.id == QStringLiteral("metal_modern_progressive");
    const bool funk =
        profile.id == QStringLiteral("funk_static_pocket");
    const bool fusion =
        profile.id == QStringLiteral("jazz_fusion");
    const bool soft =
        style == QStringLiteral("bossa-nova") ||
        (style == QStringLiteral("jazz") && !fusion);
    const auto finishAt = [&](double seconds) {
        voice.end = std::min<qint64>(
            static_cast<qint64>(frames),
            hit.frame +
                static_cast<qint64>(seconds * kSampleRate));
    };
    if (hit.kind == DrumKind::Kick || isTomKind(hit.kind)) {
        const bool synthetic =
            electronic || polishedPop || metal || funk ||
            isTomKind(hit.kind);
        if (synthetic) {
            voice.model = DaisyDrumModel::SyntheticKick;
            voice.syntheticKick =
                std::make_unique<daisysp::SyntheticBassDrum>();
            voice.syntheticKick->Init(kSampleRate);
            voice.syntheticKick->SetFreq(
                hit.kind == DrumKind::HighTom ? 176.0f :
                hit.kind == DrumKind::FloorTom ? 82.0f :
                hit.kind == DrumKind::MidTom ? 124.0f :
                metal ? 64.0f :
                profile.id == QStringLiteral("hiphop_trap") ? 46.0f :
                profile.id == QStringLiteral("electronic_house") ? 50.0f :
                style == QStringLiteral("jpop-anisong") ? 63.0f :
                style == QStringLiteral("pop") ? 57.0f :
                56.0f);
            voice.syntheticKick->SetAccent(hit.excitation);
            voice.syntheticKick->SetTone(
                metal ? 0.68f :
                style == QStringLiteral("jpop-anisong") ? 0.62f :
                style == QStringLiteral("pop") ? 0.54f :
                electronic ? 0.56f : 0.48f);
            voice.syntheticKick->SetDecay(
                metal ? 0.30f :
                profile.id == QStringLiteral("hiphop_trap") ? 0.72f :
                polishedPop ? 0.34f :
                0.48f);
            voice.syntheticKick->SetDirtiness(
                metal ? 0.32f : funk ? 0.14f : 0.07f);
            voice.syntheticKick->SetFmEnvelopeAmount(
                isTomKind(hit.kind) ? 0.22f : 0.58f);
            voice.syntheticKick->SetFmEnvelopeDecay(0.27f);
        } else {
            voice.model = DaisyDrumModel::AnalogKick;
            voice.analogKick =
                std::make_unique<daisysp::AnalogBassDrum>();
            voice.analogKick->Init(kSampleRate);
            voice.analogKick->SetFreq(
                style == QStringLiteral("bossa-nova") ? 70.0f :
                style == QStringLiteral("jazz") ? 62.0f :
                style == QStringLiteral("reggae") ? 52.0f :
                style == QStringLiteral("country") ? 60.0f : 56.0f);
            voice.analogKick->SetAccent(hit.excitation);
            voice.analogKick->SetTone(soft ? 0.34f : 0.48f);
            voice.analogKick->SetDecay(soft ? 0.34f : 0.48f);
            voice.analogKick->SetAttackFmAmount(
                soft ? 0.25f : 0.44f);
            voice.analogKick->SetSelfFmAmount(
                soft ? 0.20f : 0.32f);
        }
        voice.gain = hit.outputGain * (soft ? 0.38f : 0.52f);
        finishAt(isTomKind(hit.kind) ? 1.0 : 1.5);
    } else if (hit.kind == DrumKind::Snare ||
               hit.kind == DrumKind::CrossStick ||
               hit.kind == DrumKind::HandPercussion) {
        const bool synthetic =
            (electronic || polishedPop || metal || funk) &&
            hit.kind == DrumKind::Snare;
        if (synthetic) {
            voice.model = DaisyDrumModel::SyntheticSnare;
            voice.syntheticSnare =
                std::make_unique<daisysp::SyntheticSnareDrum>();
            voice.syntheticSnare->Init(kSampleRate);
            voice.syntheticSnare->SetFreq(
                metal ? 205.0f :
                style == QStringLiteral("jpop-anisong") ? 218.0f :
                style == QStringLiteral("pop") ? 196.0f : 184.0f);
            voice.syntheticSnare->SetAccent(hit.excitation);
            voice.syntheticSnare->SetFmAmount(
                metal ? 0.44f : 0.26f);
            voice.syntheticSnare->SetDecay(
                metal ? 0.42f :
                polishedPop ? 0.24f : 0.30f);
            voice.syntheticSnare->SetSnappy(
                metal ? 0.74f :
                polishedPop ? 0.68f : 0.60f);
        } else {
            voice.model = DaisyDrumModel::AnalogSnare;
            voice.analogSnare =
                std::make_unique<daisysp::AnalogSnareDrum>();
            voice.analogSnare->Init(kSampleRate);
            voice.analogSnare->SetFreq(
                hit.kind == DrumKind::CrossStick ? 178.0f :
                hit.kind == DrumKind::HandPercussion ? 205.0f :
                soft ? 205.0f : 190.0f);
            voice.analogSnare->SetAccent(hit.excitation);
            voice.analogSnare->SetTone(
                hit.kind == DrumKind::CrossStick ? 0.22f :
                hit.kind == DrumKind::HandPercussion ? 0.30f :
                soft ? 0.42f : 0.56f);
            voice.analogSnare->SetDecay(
                hit.kind == DrumKind::CrossStick ? 0.035f :
                hit.kind == DrumKind::HandPercussion ? 0.11f :
                soft ? 0.20f : 0.34f);
            voice.analogSnare->SetSnappy(
                hit.kind == DrumKind::CrossStick ? 0.18f :
                hit.kind == DrumKind::HandPercussion ? 0.32f :
                soft ? 0.50f : 0.72f);
        }
        voice.gain = hit.outputGain *
            (hit.kind == DrumKind::CrossStick ? 0.28f :
             hit.kind == DrumKind::HandPercussion ? 0.30f :
             soft ? 0.35f : 0.48f);
        finishAt(
            hit.kind == DrumKind::CrossStick ? 0.32 : 1.05);
    } else if (hit.kind == DrumKind::Crash) {
        voice.model = DaisyDrumModel::Crash;
        voice.crashA = std::make_unique<daisysp::Oscillator>();
        voice.crashB = std::make_unique<daisysp::Oscillator>();
        voice.crashC = std::make_unique<daisysp::Oscillator>();
        voice.crashFilter = std::make_unique<daisysp::Svf>();
        for (daisysp::Oscillator* oscillator :
             {voice.crashA.get(),
              voice.crashB.get(),
              voice.crashC.get()}) {
            oscillator->Init(kSampleRate);
            oscillator->SetAmp(1.0f);
            oscillator->SetWaveform(
                daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
        }
        voice.crashA->SetFreq(1733.0f);
        voice.crashB->SetFreq(2411.0f);
        voice.crashC->SetFreq(3571.0f);
        voice.crashFilter->Init(kSampleRate);
        voice.crashFilter->SetFreq(soft ? 2050.0f : 2850.0f);
        voice.crashFilter->SetRes(0.18f);
        voice.crashFilter->SetDrive(0.08f);
        const double decaySeconds =
            profile.styleId == QStringLiteral("bossa-nova")
                ? 0.42 : metal ? 1.05 : 1.35;
        voice.crashDecay = static_cast<float>(
            std::exp(-1.0 / (decaySeconds * kSampleRate)));
        voice.crashMetalMix = soft ? 0.06f : metal ? 0.22f : 0.14f;
        voice.noiseState ^= stableSeed(profile.id) ^
            static_cast<std::uint32_t>(hit.frame);
        voice.gain = hit.outputGain * (soft ? 0.14f : 0.30f);
        finishAt(soft ? 1.2 : 2.8);
    } else {
        voice.model = DaisyDrumModel::Hat;
        voice.hat = std::make_unique<daisysp::HiHat<>>();
        voice.hat->Init(kSampleRate);
        const bool cymbal =
            hit.kind == DrumKind::Crash ||
            hit.kind == DrumKind::Ride;
        const bool shaker = hit.kind == DrumKind::Shaker;
        const bool open = hit.kind == DrumKind::OpenHat;
        voice.hat->SetFreq(
            cymbal ? 2200.0f :
            shaker
                ? (profile.styleId == QStringLiteral("bossa-nova")
                    ? 3300.0f : 5200.0f) :
            electronic ? 3900.0f : 3300.0f);
        voice.hat->SetAccent(hit.excitation);
        voice.hat->SetTone(
            hit.kind == DrumKind::Ride ? 0.50f :
            hit.kind == DrumKind::Crash ? 0.64f :
            shaker && profile.styleId == QStringLiteral("bossa-nova")
                ? 0.24f :
            soft ? 0.52f : 0.72f);
        voice.hat->SetDecay(
            hit.kind == DrumKind::Crash ? 0.92f :
            hit.kind == DrumKind::Ride &&
                    style == QStringLiteral("jazz") ? 0.82f :
            hit.kind == DrumKind::Ride ? 0.62f :
            fusion && hit.kind == DrumKind::ClosedHat ? 0.24f :
            open ? 0.54f :
            shaker
                ? (profile.styleId == QStringLiteral("bossa-nova")
                    ? 0.045f : 0.07f)
                : 0.16f);
        voice.hat->SetNoisiness(
            shaker ? 0.96f :
            hit.kind == DrumKind::Ride ? 0.40f :
            soft ? 0.62f : 0.76f);
        voice.gain = hit.outputGain *
            (shaker && profile.styleId == QStringLiteral("bossa-nova")
                ? 0.12f :
             hit.kind == DrumKind::Ride &&
                    style == QStringLiteral("jazz") ? 0.32f :
             fusion ? 0.38f :
             soft ? 0.20f :
             hit.kind == DrumKind::Crash ? 0.32f : 0.28f);
        finishAt(
            hit.kind == DrumKind::Crash ? 2.5 :
            hit.kind == DrumKind::Ride ? 1.8 :
            open ? 1.1 : 0.45);
    }
    return voice;
}

struct DrumBusDesign {
    double drive = 1.24;
    double cutoffHz = 14500.0;
    double compressorThreshold = 0.12;
    double compressorRatio = 2.4;
    double compressorReleaseMs = 55.0;
    double roomMix = 0.08;
    double roomSizeMs = 31.0;
    double roomDamping = 0.58;
};

DrumBusDesign drumBusDesign(const ProfileDefinition& profile)
{
    const bool soft =
        profile.styleId == QStringLiteral("bossa-nova") ||
        (profile.styleId == QStringLiteral("jazz") &&
         profile.id != QStringLiteral("jazz_fusion"));
    const bool dark =
        profile.id == QStringLiteral("hiphop_boom_bap") ||
        profile.id == QStringLiteral("reggae_roots");
    const bool heavy =
        profile.styleId == QStringLiteral("electronic") ||
        profile.styleId == QStringLiteral("hiphop-trap") ||
        profile.id == QStringLiteral("metal_modern_progressive");
    const bool fusion =
        profile.id == QStringLiteral("jazz_fusion");
    DrumBusDesign design;
    design.drive = soft ? 1.05 : heavy ? 1.55 : 1.24;
    design.cutoffHz =
        dark ? 7800.0 : soft ? 10500.0 : 14500.0;
    design.compressorThreshold =
        soft ? 0.075 :
        fusion ? 0.055 :
        dark ? 0.090 :
        heavy ? 0.16 : 0.12;
    design.compressorRatio =
        soft ? 3.2 :
        fusion ? 4.0 :
        dark ? 3.0 :
        heavy ? 2.0 : 2.4;
    design.compressorReleaseMs =
        soft ? 68.0 :
        fusion ? 62.0 :
        dark ? 72.0 : 48.0;
    design.roomMix =
        soft ? 0.10 : heavy ? 0.055 : dark ? 0.065 : 0.085;
    design.roomSizeMs =
        soft ? 38.0 : heavy ? 24.0 : dark ? 28.0 : 32.0;
    design.roomDamping =
        soft ? 0.72 : heavy ? 0.48 : dark ? 0.68 : 0.58;
    return design;
}

void applyDrumRoom(
    std::vector<float>& output,
    const std::vector<float>& send,
    const DrumBusDesign& design)
{
    if (output.empty() || send.empty() || design.roomMix <= 0.00001) {
        return;
    }
    const std::array<double, 3> ratios{0.73, 1.0, 1.37};
    std::array<std::vector<float>, 3> delays;
    std::array<std::size_t, 3> positions{};
    std::array<double, 3> damped{};
    for (std::size_t index = 0; index < delays.size(); ++index) {
        const std::size_t length = std::max<std::size_t>(
            17,
            static_cast<std::size_t>(
                std::llround(
                    0.001 * design.roomSizeMs * ratios[index] *
                    kSampleRate)));
        delays[index].assign(length, 0.0f);
    }
    const double damping = std::clamp(design.roomDamping, 0.0, 0.98);
    const double feedback = 0.28 + 0.34 * (1.0 - damping);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        double wet = 0.0;
        for (std::size_t index = 0; index < delays.size(); ++index) {
            float& cell = delays[index][positions[index]];
            damped[index] +=
                (0.08 + 0.78 * (1.0 - damping)) *
                (cell - damped[index]);
            wet += damped[index];
            cell = static_cast<float>(
                send[frame] + feedback * damped[index]);
            positions[index] =
                (positions[index] + 1) % delays[index].size();
        }
        output[frame] += static_cast<float>(
            design.roomMix * wet / delays.size());
    }
}

void applyDrumBus(
    std::vector<float>& audio,
    const DrumBusDesign& design)
{
    const double coefficient =
        1.0 - std::exp(
            -2.0 * kPi * design.cutoffHz / kSampleRate);
    const double envelopeRelease = std::exp(
        -1.0 /
        (0.001 * design.compressorReleaseMs * kSampleRate));
    double filtered = 0.0;
    double envelope = 0.0;
    for (float& sample : audio) {
        const double value = std::tanh(design.drive * sample);
        filtered += coefficient * (value - filtered);
        envelope = std::max(
            std::abs(filtered),
            envelope * envelopeRelease);
        double gain = 1.0;
        if (envelope > design.compressorThreshold) {
            const double over =
                envelope / design.compressorThreshold;
            gain = std::pow(
                over,
                1.0 / design.compressorRatio - 1.0);
        }
        sample = static_cast<float>(gain * filtered);
    }
}

struct DrumLabPatch {
    struct VelocityBand {
        int minimum = 40;
        int maximum = 78;
    };

    struct VelocityDesign {
        VelocityBand ghost{18, 46};
        VelocityBand normal{54, 96};
        VelocityBand accent{92, 124};
        float excitationCurve = 0.72f;
        float outputCurve = 1.35f;
        float brightnessAmount = 0.16f;
        float decayAmount = 0.08f;
        float driveAmount = 0.10f;
    };

    struct SynthLayer {
        QString source = QStringLiteral("off");
        int midiNote = 60;
        float level = 0.22f;
        float gateSeconds = 0.08f;
        float attack = 0.001f;
        float decay = 0.06f;
        float sustain = 0.01f;
        float release = 0.08f;
        float noiseMix = 0.0f;
        float filterCutoff = 8000.0f;
    };

    QString intendedIdentity;
    QString source = QStringLiteral("daisy-profile");
    QString secondSource = QStringLiteral("off");
    float blend = 0.0f;
    float frequency = 180.0f;
    float tone = 0.5f;
    float decay = 0.35f;
    float colour = 0.6f;
    float fmAmount = 0.3f;
    float level = 0.45f;
    QString transient = QStringLiteral("off");
    float transientLevel = 0.0f;
    float transientTone = 0.5f;
    float transientDecaySeconds = 0.014f;
    QString texture = QStringLiteral("off");
    float textureLevel = 0.0f;
    float textureTone = 0.5f;
    float textureDecaySeconds = 0.18f;
    float textureDensity = 0.5f;
    float voiceDrive = 1.0f;
    float digitalSampleRateHz = 48000.0f;
    int digitalBitDepth = 24;
    float reconstructionLowpassHz = 20000.0f;
    float dynamicFilterAmount = 0.0f;
    float roomSend = 0.08f;
    QString chokeGroup;
    float chokeSeconds = 0.012f;
    VelocityDesign velocity;
    SynthLayer synth;
};

constexpr std::array<DrumKind, 12> kDrumKinds{
    DrumKind::Kick,
    DrumKind::Snare,
    DrumKind::ClosedHat,
    DrumKind::OpenHat,
    DrumKind::HighTom,
    DrumKind::MidTom,
    DrumKind::FloorTom,
    DrumKind::Crash,
    DrumKind::Ride,
    DrumKind::CrossStick,
    DrumKind::Shaker,
    DrumKind::HandPercussion,
};

QString drumKindId(DrumKind kind)
{
    switch (kind) {
    case DrumKind::Kick: return QStringLiteral("kick");
    case DrumKind::Snare: return QStringLiteral("snare");
    case DrumKind::ClosedHat: return QStringLiteral("closed-hat");
    case DrumKind::OpenHat: return QStringLiteral("open-hat");
    case DrumKind::HighTom: return QStringLiteral("high-tom");
    case DrumKind::MidTom: return QStringLiteral("mid-tom");
    case DrumKind::FloorTom: return QStringLiteral("floor-tom");
    case DrumKind::Crash: return QStringLiteral("crash");
    case DrumKind::Ride: return QStringLiteral("ride");
    case DrumKind::CrossStick: return QStringLiteral("cross-stick");
    case DrumKind::Shaker: return QStringLiteral("shaker");
    case DrumKind::HandPercussion:
        return QStringLiteral("hand-percussion");
    }
    return QStringLiteral("unknown");
}

std::optional<DrumKind> drumKindFromId(const QString& id)
{
    if (id == QStringLiteral("tom")) {
        return DrumKind::MidTom;
    }
    for (DrumKind kind : kDrumKinds) {
        if (drumKindId(kind) == id) return kind;
    }
    return std::nullopt;
}

QString laneNameForDrumKind(DrumKind kind)
{
    switch (kind) {
    case DrumKind::Kick: return QStringLiteral("Kick");
    case DrumKind::Snare: return QStringLiteral("Snare");
    case DrumKind::ClosedHat: return QStringLiteral("Closed HH");
    case DrumKind::OpenHat: return QStringLiteral("Open HH");
    case DrumKind::HighTom: return QStringLiteral("High Tom");
    case DrumKind::MidTom: return QStringLiteral("Mid Tom");
    case DrumKind::FloorTom: return QStringLiteral("Floor Tom");
    case DrumKind::Crash: return QStringLiteral("Crash");
    case DrumKind::Ride: return QStringLiteral("Ride");
    case DrumKind::CrossStick:
        return QStringLiteral("Cross-stick / Rim");
    case DrumKind::Shaker: return QStringLiteral("Shaker");
    case DrumKind::HandPercussion:
        return QStringLiteral("Hand Percussion");
    }
    return {};
}

DrumLabPatch defaultDrumLabPatch(
    const ProfileDefinition& profile,
    DrumKind kind)
{
    const bool soft =
        profile.styleId == QStringLiteral("bossa-nova") ||
        (profile.styleId == QStringLiteral("jazz") &&
         profile.id != QStringLiteral("jazz_fusion"));
    const bool electronic =
        profile.styleId == QStringLiteral("electronic") ||
        profile.styleId == QStringLiteral("hiphop-trap");
    DrumLabPatch patch;
    switch (kind) {
    case DrumKind::Kick:
        patch.synth.midiNote = 36;
        patch.synth.gateSeconds = 0.12f;
        patch.synth.decay = 0.09f;
        patch.synth.release = 0.10f;
        patch.frequency =
            profile.id == QStringLiteral("hiphop_trap") ? 46.0f :
            profile.styleId == QStringLiteral("bossa-nova") ? 70.0f :
            profile.styleId == QStringLiteral("jazz") ? 62.0f :
            profile.styleId == QStringLiteral("reggae") ? 52.0f :
            electronic ? 50.0f : 57.0f;
        patch.tone = soft ? 0.34f : 0.54f;
        patch.decay =
            profile.id == QStringLiteral("hiphop_trap")
                ? 0.72f : soft ? 0.34f : 0.42f;
        patch.colour =
            profile.id == QStringLiteral("metal_modern_progressive")
                ? 0.32f : 0.08f;
        patch.fmAmount = soft ? 0.25f : 0.58f;
        patch.level = soft ? 0.38f : 0.52f;
        break;
    case DrumKind::Snare:
        patch.synth.midiNote = 60;
        patch.synth.gateSeconds = 0.08f;
        patch.frequency =
            profile.id == QStringLiteral("metal_modern_progressive")
                ? 205.0f : soft ? 205.0f : 196.0f;
        patch.tone = soft ? 0.42f : 0.56f;
        patch.decay = soft ? 0.20f : 0.30f;
        patch.colour = soft ? 0.50f : 0.70f;
        patch.fmAmount = 0.30f;
        patch.level = soft ? 0.35f : 0.48f;
        break;
    case DrumKind::HighTom:
    case DrumKind::MidTom:
    case DrumKind::FloorTom:
        patch.synth.midiNote =
            kind == DrumKind::HighTom ? 50 :
            kind == DrumKind::FloorTom ? 43 : 47;
        patch.synth.gateSeconds = 0.12f;
        patch.synth.decay = 0.09f;
        patch.frequency =
            kind == DrumKind::HighTom ? 176.0f :
            kind == DrumKind::FloorTom ? 82.0f : 124.0f;
        patch.tone = 0.48f;
        patch.decay = 0.34f;
        patch.colour = 0.08f;
        patch.fmAmount = 0.22f;
        patch.level = soft ? 0.32f : 0.48f;
        break;
    case DrumKind::ClosedHat:
        patch.synth.midiNote = 84;
        patch.synth.gateSeconds = 0.035f;
        patch.synth.decay = 0.025f;
        patch.synth.release = 0.035f;
        patch.frequency = electronic ? 3900.0f : 3300.0f;
        patch.tone = soft ? 0.52f : 0.72f;
        patch.decay = 0.16f;
        patch.colour = soft ? 0.62f : 0.76f;
        patch.level = soft ? 0.20f : 0.28f;
        break;
    case DrumKind::OpenHat:
        patch.synth.midiNote = 84;
        patch.synth.gateSeconds = 0.18f;
        patch.synth.decay = 0.14f;
        patch.synth.release = 0.18f;
        patch.frequency = electronic ? 3900.0f : 3300.0f;
        patch.tone = soft ? 0.52f : 0.72f;
        patch.decay = 0.54f;
        patch.colour = soft ? 0.62f : 0.76f;
        patch.level = soft ? 0.20f : 0.28f;
        break;
    case DrumKind::Crash:
        patch.synth.midiNote = 72;
        patch.synth.gateSeconds = 0.24f;
        patch.synth.decay = 0.18f;
        patch.synth.release = 0.30f;
        patch.frequency = soft ? 2050.0f : 2850.0f;
        patch.tone = soft ? 0.40f : 0.68f;
        patch.decay = soft ? 0.28f : 0.72f;
        patch.colour = soft ? 0.06f : 0.16f;
        patch.level = soft ? 0.14f : 0.30f;
        break;
    case DrumKind::Ride:
        patch.synth.midiNote = 76;
        patch.synth.gateSeconds = 0.16f;
        patch.synth.decay = 0.12f;
        patch.synth.release = 0.18f;
        patch.frequency = 2200.0f;
        patch.tone = 0.50f;
        patch.decay =
            profile.styleId == QStringLiteral("jazz") ? 0.82f : 0.62f;
        patch.colour = 0.40f;
        patch.level =
            profile.styleId == QStringLiteral("jazz") ? 0.32f : 0.26f;
        break;
    case DrumKind::CrossStick:
        patch.synth.midiNote = 72;
        patch.synth.gateSeconds = 0.025f;
        patch.synth.decay = 0.018f;
        patch.synth.release = 0.025f;
        patch.frequency = 178.0f;
        patch.tone = 0.22f;
        patch.decay = 0.035f;
        patch.colour = 0.18f;
        patch.level = 0.28f;
        break;
    case DrumKind::Shaker:
        patch.synth.midiNote = 84;
        patch.synth.gateSeconds = 0.04f;
        patch.synth.decay = 0.03f;
        patch.synth.release = 0.04f;
        patch.frequency =
            profile.styleId == QStringLiteral("bossa-nova")
                ? 3300.0f : 5200.0f;
        patch.tone =
            profile.styleId == QStringLiteral("bossa-nova")
                ? 0.24f : 0.52f;
        patch.decay =
            profile.styleId == QStringLiteral("bossa-nova")
                ? 0.045f : 0.07f;
        patch.colour = 0.96f;
        patch.level =
            profile.styleId == QStringLiteral("bossa-nova")
                ? 0.12f : 0.20f;
        break;
    case DrumKind::HandPercussion:
        patch.synth.midiNote = 67;
        patch.synth.gateSeconds = 0.07f;
        patch.synth.decay = 0.05f;
        patch.synth.release = 0.06f;
        patch.frequency = 205.0f;
        patch.tone = 0.30f;
        patch.decay = 0.11f;
        patch.colour = 0.32f;
        patch.level = 0.30f;
        break;
    }
    return patch;
}

enum class DrumPalette {
    SoftCircuit,
    CleanPcm,
    HybridPop,
    DryRock,
    OpenRock,
    Garage,
    BrushJazz,
    BopJazz,
    Fusion,
    HandModal,
    Atmosphere,
    BluesClub,
    DarkBlues,
    Anisong,
    GlossyDance,
    CountryDry,
    CountryPolished,
    House909,
    Techno,
    Break12,
    SoulDamped,
    NeoSoul,
    FunkDry,
    Boom12,
    Trap808,
    ReggaeDub,
    Bossa,
    MetalLayered,
    ElectronicMetal,
    OrganicHeavy,
    Simmons,
    RhythmBox,
    Industrial,
    Lofi,
};

struct ResearchKitCandidate {
    QString id;
    QString name;
    DrumPalette palette = DrumPalette::SoftCircuit;
    QString description;
    bool recommended = false;
    int variant = 0;
};

struct ResearchKitRow {
    const char* profileId;
    std::array<const char*, 3> names;
    std::array<DrumPalette, 3> palettes;
    int recommendedIndex = 0;
};

constexpr std::array<ResearchKitRow, 27> kResearchKitRows{{
    {"pop_loop",
     {"Hybrid Section Lift", "Soft Circuit Pocket", "Bright Gated Frame"},
     {DrumPalette::HybridPop, DrumPalette::SoftCircuit,
      DrumPalette::CleanPcm}},
    {"pop_sectional",
     {"Hybrid Section Lift", "Soft Circuit Pocket", "Bright Gated Frame"},
     {DrumPalette::HybridPop, DrumPalette::SoftCircuit,
      DrumPalette::CleanPcm}},
    {"rock_riff_modal",
     {"Dry Riff Room", "Machine Underlay", "Open Shell Room"},
     {DrumPalette::DryRock, DrumPalette::ElectronicMetal,
      DrumPalette::OpenRock}},
    {"rock_shuffle_blues",
     {"Open Maple Shuffle", "Vintage Damped Club", "Crunch Room"},
     {DrumPalette::OpenRock, DrumPalette::BluesClub,
      DrumPalette::DryRock},
     2},
    {"rock_punk_garage",
     {"Bright Brass Garage", "Raw Concrete Room", "Electronic Underlay"},
     {DrumPalette::Garage, DrumPalette::DryRock,
      DrumPalette::ElectronicMetal},
     1},
    {"jazz_swing_standards",
     {"Brushes and Dark Ride", "Small Club Sticks", "Simmons Colour"},
     {DrumPalette::BrushJazz, DrumPalette::BopJazz,
      DrumPalette::Simmons},
     2},
    {"jazz_bebop",
     {"Dry Bop Ride", "Bright Club Ride", "Simmons Colour"},
     {DrumPalette::BopJazz, DrumPalette::Fusion,
      DrumPalette::Simmons},
     2},
    {"jazz_fusion",
     {"Tight Hybrid Fusion", "Electric Open Kit", "Simmons Colour"},
     {DrumPalette::Fusion, DrumPalette::OpenRock,
      DrumPalette::Simmons},
     2},
    {"modal_groove",
     {"Resonant Hand-Kit", "Dry Frame Pocket", "Metallic Pulse Kit"},
     {DrumPalette::HandModal, DrumPalette::Bossa,
      DrumPalette::Industrial}},
    {"modal_atmospheric",
     {"Air and Skin Objects", "Glass Plate Kit", "Distant Pulse Kit"},
     {DrumPalette::Atmosphere, DrumPalette::Industrial,
      DrumPalette::HandModal}},
    {"blues_dominant",
     {"Greasy Club Shuffle", "Tight Bar Kit", "Vintage Rhythm-Box"},
     {DrumPalette::BluesClub, DrumPalette::DryRock,
      DrumPalette::RhythmBox},
     2},
    {"blues_minor",
     {"Dark Deep Shuffle", "Smoky Brush Kit", "Slow Crunch Room"},
     {DrumPalette::DarkBlues, DrumPalette::BrushJazz,
      DrumPalette::OpenRock},
     1},
    {"jpop_anisong_rock",
     {"Layered Arena Precision", "Bright Live Anime Kit", "Machine-Chase Hybrid"},
     {DrumPalette::Anisong, DrumPalette::OpenRock,
      DrumPalette::ElectronicMetal},
     1},
    {"jpop_idol_dance",
     {"Glossy PCM Circuit", "Future Bass Pop Kit", "Light 909 Pop"},
     {DrumPalette::GlossyDance, DrumPalette::HybridPop,
      DrumPalette::House909}},
    {"country_honky_tonk",
     {"Dry Train Kit", "Brush Two-Step", "Small Radio Kit"},
     {DrumPalette::CountryDry, DrumPalette::BrushJazz,
      DrumPalette::Lofi}},
    {"country_contemporary",
     {"Polished Wide Country", "Tight Nashville Pop", "Arena Country Hybrid"},
     {DrumPalette::CountryPolished, DrumPalette::HybridPop,
      DrumPalette::Anisong},
     2},
    {"electronic_house",
     {"Warm 909 Descendant", "Deep Organ House", "Clean Modern Club"},
     {DrumPalette::House909, DrumPalette::SoftCircuit,
      DrumPalette::GlossyDance}},
    {"electronic_techno",
     {"Driven Machine Core", "Hypnotic Low-Pulse", "Industrial FM"},
     {DrumPalette::Techno, DrumPalette::House909,
      DrumPalette::Industrial},
     1},
    {"electronic_breakbeat",
     {"Chopped 12-Bit Break", "Clean Nu-Break", "Dusty Live Chop"},
     {DrumPalette::Break12, DrumPalette::CleanPcm,
      DrumPalette::Lofi}},
    {"soul_classic_motown",
     {"Damped Studio Pocket", "Tambourine Radio Kit", "Warm Rhythm Box"},
     {DrumPalette::SoulDamped, DrumPalette::Lofi,
      DrumPalette::RhythmBox}},
    {"rnb_contemporary_neosoul",
     {"Damped Studio Pocket", "Electronic Rim Pocket", "Dusty Neo-Soul"},
     {DrumPalette::SoulDamped, DrumPalette::SoftCircuit,
      DrumPalette::Lofi}},
    {"funk_static_pocket",
     {"Dry Ghost Pocket", "Tight 606-Funk", "Live Break Funk"},
     {DrumPalette::FunkDry, DrumPalette::RhythmBox,
      DrumPalette::Break12}},
    {"hiphop_boom_bap",
     {"Filtered 12-Bit Pocket", "Soft Analogue Rap Kit", "Hard DMX Frame"},
     {DrumPalette::Boom12, DrumPalette::SoftCircuit,
      DrumPalette::CleanPcm}},
    {"hiphop_trap",
     {"Gliding Sub-808 Descendant", "Punchy Drill Sub", "Digital Bell Trap"},
     {DrumPalette::Trap808, DrumPalette::MetalLayered,
      DrumPalette::Industrial},
     1},
    {"reggae_roots",
     {"Jam2 Roots + Warm Crash", "Warm Rhythm-Box Roots", "Dry Rockers Kit"},
     {DrumPalette::ReggaeDub, DrumPalette::RhythmBox,
      DrumPalette::CountryDry}},
    {"bossa_songbook",
     {"Nylon-Room Percussion", "Brush Cafe Kit", "Soft CR-Latin"},
     {DrumPalette::Bossa, DrumPalette::BrushJazz,
      DrumPalette::RhythmBox},
     2},
    {"metal_modern_progressive",
     {"Modern Layered Impact", "Electronic Intro Kit", "Organic Heavy Room"},
     {DrumPalette::MetalLayered, DrumPalette::ElectronicMetal,
      DrumPalette::OrganicHeavy},
     2},
}};

struct ProfileKitCharacter {
    const char* profileId;
    float pitchScale;
    float tailScale;
    float metalBrightness;
    float transientScale;
    float roomScale;
    float driveScale;
    float handPitchScale;
};

// These are profile-level kit relationships, not random offsets. They keep
// shell tuning, top-end weight, transient scale and room perspective coherent
// across all three candidates while preventing a borrowed palette from
// becoming an exact cross-style duplicate.
constexpr std::array<ProfileKitCharacter, 27>
    kProfileKitCharacters{{
        {"pop_loop", 1.03f, 1.00f, 1.08f, 1.12f, 1.06f, 1.04f, 1.02f},
        {"pop_sectional", 1.03f, 1.00f, 1.08f, 1.12f, 1.06f, 1.04f, 1.02f},
        {"rock_riff_modal", 0.96f, 0.88f, 0.98f, 1.18f, 0.82f, 1.06f, 0.94f},
        {"rock_shuffle_blues", 0.92f, 1.12f, 0.92f, 0.92f, 1.14f, 1.00f, 0.90f},
        {"rock_punk_garage", 1.08f, 0.82f, 1.18f, 1.28f, 1.18f, 1.12f, 1.05f},
        {"jazz_swing_standards", 0.98f, 1.08f, 0.78f, 0.72f, 1.25f, 0.96f, 0.95f},
        {"jazz_bebop", 1.07f, 0.86f, 0.92f, 1.05f, 0.92f, 0.98f, 1.08f},
        {"jazz_fusion", 1.05f, 0.80f, 1.15f, 1.15f, 0.78f, 1.07f, 1.10f},
        {"modal_groove", 0.94f, 1.08f, 0.85f, 0.75f, 1.25f, 0.98f, 0.89f},
        {"modal_atmospheric", 0.82f, 1.35f, 0.72f, 0.58f, 1.60f, 0.94f, 0.80f},
        {"blues_dominant", 0.95f, 1.08f, 0.87f, 0.86f, 1.18f, 1.00f, 0.92f},
        {"blues_minor", 0.88f, 1.18f, 0.78f, 0.75f, 1.28f, 0.98f, 0.86f},
        {"jpop_anisong_rock", 1.08f, 0.82f, 1.20f, 1.30f, 1.05f, 1.08f, 1.12f},
        {"jpop_idol_dance", 1.12f, 0.74f, 1.28f, 1.35f, 0.72f, 1.05f, 1.18f},
        {"country_honky_tonk", 1.02f, 0.82f, 1.06f, 1.10f, 0.78f, 0.98f, 1.00f},
        {"country_contemporary", 1.02f, 0.96f, 1.13f, 1.15f, 1.18f, 1.03f, 0.98f},
        {"electronic_house", 0.89f, 0.96f, 1.14f, 1.10f, 0.48f, 1.12f, 1.04f},
        {"electronic_techno", 0.82f, 0.72f, 0.94f, 1.20f, 0.36f, 1.25f, 0.92f},
        {"electronic_breakbeat", 0.95f, 0.78f, 0.86f, 1.15f, 0.62f, 1.15f, 0.96f},
        {"soul_classic_motown", 0.91f, 0.70f, 0.74f, 0.72f, 0.72f, 0.95f, 0.88f},
        {"rnb_contemporary_neosoul", 0.84f, 0.66f, 0.72f, 0.62f, 0.55f, 0.94f, 0.82f},
        {"funk_static_pocket", 1.03f, 0.58f, 1.02f, 1.15f, 0.35f, 1.02f, 1.05f},
        {"hiphop_boom_bap", 0.88f, 0.72f, 0.74f, 0.95f, 0.52f, 1.10f, 0.90f},
        {"hiphop_trap", 0.76f, 0.92f, 1.16f, 1.18f, 0.28f, 1.18f, 1.10f},
        {"reggae_roots", 0.86f, 0.88f, 0.72f, 0.58f, 1.12f, 0.96f, 0.84f},
        {"bossa_songbook", 1.08f, 0.64f, 0.76f, 0.62f, 0.90f, 0.92f, 1.15f},
        {"metal_modern_progressive", 1.02f, 0.68f, 1.10f, 1.40f, 0.75f, 1.18f, 0.96f},
    }};

const ProfileKitCharacter& profileKitCharacter(
    const ProfileDefinition& profile)
{
    const auto found = std::find_if(
        kProfileKitCharacters.begin(),
        kProfileKitCharacters.end(),
        [&profile](const ProfileKitCharacter& value) {
            return profile.id ==
                QString::fromLatin1(value.profileId);
        });
    if (found == kProfileKitCharacters.end()) {
        throw std::runtime_error(
            "Research profile has no kit-character definition.");
    }
    return *found;
}

QString slugifyCandidate(QString value)
{
    value = value.toLower();
    QString result;
    bool dash = false;
    for (const QChar character : value) {
        if (character.isLetterOrNumber()) {
            result += character;
            dash = false;
        } else if (!dash && !result.isEmpty()) {
            result += QLatin1Char('-');
            dash = true;
        }
    }
    while (result.endsWith(QLatin1Char('-'))) result.chop(1);
    return result;
}

QString paletteDescription(DrumPalette palette)
{
    switch (palette) {
    case DrumPalette::SoftCircuit:
        return QStringLiteral(
            "Rounded circuit bodies, restrained transients and short dark texture.");
    case DrumPalette::CleanPcm:
        return QStringLiteral(
            "Early-digital style replay colour, defined attacks and compact tails.");
    case DrumPalette::HybridPop:
        return QStringLiteral(
            "Acoustic-adjacent bodies reinforced by precise electronic transients.");
    case DrumPalette::DryRock:
        return QStringLiteral(
            "Controlled shells, riff-readable attacks and a subordinate short room.");
    case DrumPalette::OpenRock:
        return QStringLiteral(
            "More resonant shells, wire movement and open acoustic-metal decay.");
    case DrumPalette::Garage:
        return QStringLiteral(
            "Fast bright impacts, rough voice drive and energetic brass-like metal.");
    case DrumPalette::BrushJazz:
        return QStringLiteral(
            "Brush excitation, feathered low drum and a dark stick-defined ride.");
    case DrumPalette::BopJazz:
        return QStringLiteral(
            "Fast-recovering small drums centred on an articulate stick-defined ride.");
    case DrumPalette::Fusion:
        return QStringLiteral(
            "Precise acoustic/synthetic bodies with brighter fast metal and toms.");
    case DrumPalette::HandModal:
        return QStringLiteral(
            "Recognisable skin, wood, rim and collision voices with hand-played dynamics.");
    case DrumPalette::Atmosphere:
        return QStringLiteral(
            "Sparse damped skin impacts with air and long low-density cymbal metal.");
    case DrumPalette::BluesClub:
    case DrumPalette::DarkBlues:
        return QStringLiteral(
            "Touch-responsive club shells, audible wire and an unhurried ride.");
    case DrumPalette::Anisong:
        return QStringLiteral(
            "Layered arena impact with consistent electronic definition.");
    case DrumPalette::GlossyDance:
        return QStringLiteral(
            "Clean global-pop PCM/circuit transients and crisp synthetic top.");
    case DrumPalette::CountryDry:
    case DrumPalette::CountryPolished:
        return QStringLiteral(
            "Acoustic-style beater, shell, cross-stick and cymbal relationships.");
    case DrumPalette::House909:
        return QStringLiteral(
            "Hybrid 909-descendant low drum and low-resolution choked metal.");
    case DrumPalette::Techno:
        return QStringLiteral(
            "Driven short machine bodies, darker metal and selective FM colour.");
    case DrumPalette::Break12:
    case DrumPalette::Boom12:
        return QStringLiteral(
            "Acoustic-derived components replayed through explicit 12-bit low-rate colour.");
    case DrumPalette::SoulDamped:
        return QStringLiteral(
            "Damped studio shells, narrow bandwidth and tambourine-like texture.");
    case DrumPalette::NeoSoul:
        return QStringLiteral(
            "Soft deep pocket, dark rim/wire detail and wide microdynamic response.");
    case DrumPalette::FunkDry:
        return QStringLiteral(
            "Very short bodies with high-resolution ghost articulation.");
    case DrumPalette::Trap808:
        return QStringLiteral(
            "Tuned long sub body, controlled pitch sweep and separable modern attack.");
    case DrumPalette::ReggaeDub:
        return QStringLiteral(
            "Deep non-clicky low drum, dry cross-stick and filtered percussion.");
    case DrumPalette::Bossa:
        return QStringLiteral(
            "Soft skin, wood, brush and shaker components with minimal compression.");
    case DrumPalette::MetalLayered:
    case DrumPalette::OrganicHeavy:
        return QStringLiteral(
            "Separated beater/sub/shell/wire/wash components for fast heavy arrangements.");
    case DrumPalette::ElectronicMetal:
        return QStringLiteral(
            "Machine pulse and synthetic metal designed as a deliberate underlay.");
    case DrumPalette::Simmons:
        return QStringLiteral(
            "Dynamic pitch-swept body with independent noise and click.");
    case DrumPalette::RhythmBox:
        return QStringLiteral(
            "Compact early rhythm-box bodies and shared narrow-band metallic colour.");
    case DrumPalette::Industrial:
        return QStringLiteral(
            "Identity-preserving machine drums, hard excitation and explicit saturation.");
    case DrumPalette::Lofi:
        return QStringLiteral(
            "Soft bandwidth-limited impacts with dust and modest early-digital colour.");
    }
    return {};
}

struct PaletteKitCharacter {
    float lowPitchScale;
    float shellPitchScale;
    float shellTailScale;
    float metalToneOffset;
    float metalTailScale;
    float transientScale;
    float roomScale;
    float driveScale;
};

PaletteKitCharacter paletteKitCharacter(DrumPalette palette)
{
    switch (palette) {
    case DrumPalette::SoftCircuit:
        return {0.98f, 0.95f, 0.80f, -0.10f, 0.78f, 0.78f, 0.68f, 1.06f};
    case DrumPalette::CleanPcm:
        return {1.03f, 1.05f, 0.74f, 0.08f, 0.68f, 1.24f, 0.58f, 1.12f};
    case DrumPalette::HybridPop:
        return {1.00f, 1.00f, 0.90f, 0.05f, 0.88f, 1.22f, 1.00f, 1.06f};
    case DrumPalette::DryRock:
        return {1.03f, 0.98f, 0.72f, -0.02f, 0.74f, 1.20f, 0.58f, 1.08f};
    case DrumPalette::OpenRock:
        return {0.95f, 0.92f, 1.18f, 0.04f, 1.20f, 0.90f, 1.38f, 1.04f};
    case DrumPalette::Garage:
        return {1.08f, 1.08f, 0.70f, 0.14f, 0.80f, 1.36f, 1.14f, 1.26f};
    case DrumPalette::BrushJazz:
        return {1.08f, 1.02f, 0.82f, -0.16f, 0.86f, 0.56f, 1.02f, 0.96f};
    case DrumPalette::BopJazz:
        return {1.05f, 1.10f, 0.70f, -0.04f, 0.76f, 1.12f, 0.72f, 0.98f};
    case DrumPalette::Fusion:
        return {1.08f, 1.12f, 0.64f, 0.12f, 0.68f, 1.28f, 0.62f, 1.12f};
    case DrumPalette::HandModal:
        return {0.95f, 1.08f, 1.08f, -0.08f, 1.08f, 0.76f, 1.28f, 0.98f};
    case DrumPalette::Atmosphere:
        return {0.82f, 0.84f, 1.32f, -0.18f, 1.32f, 0.48f, 1.85f, 0.94f};
    case DrumPalette::BluesClub:
        return {0.98f, 0.96f, 1.02f, -0.10f, 1.02f, 0.80f, 1.14f, 0.98f};
    case DrumPalette::DarkBlues:
        return {0.88f, 0.88f, 1.16f, -0.18f, 1.16f, 0.66f, 1.28f, 0.96f};
    case DrumPalette::Anisong:
        return {1.12f, 1.15f, 0.66f, 0.18f, 0.72f, 1.45f, 1.12f, 1.12f};
    case DrumPalette::GlossyDance:
        return {1.08f, 1.12f, 0.60f, 0.20f, 0.64f, 1.40f, 0.58f, 1.10f};
    case DrumPalette::CountryDry:
        return {1.06f, 1.08f, 0.66f, 0.02f, 0.68f, 1.12f, 0.58f, 0.98f};
    case DrumPalette::CountryPolished:
        return {1.02f, 1.00f, 0.88f, 0.08f, 0.92f, 1.14f, 1.25f, 1.02f};
    case DrumPalette::House909:
        return {0.87f, 0.95f, 0.82f, 0.10f, 0.64f, 1.10f, 0.38f, 1.22f};
    case DrumPalette::Techno:
        return {0.78f, 1.02f, 0.58f, -0.04f, 0.52f, 1.28f, 0.28f, 1.40f};
    case DrumPalette::Break12:
        return {0.94f, 0.96f, 0.64f, -0.08f, 0.72f, 1.26f, 0.48f, 1.22f};
    case DrumPalette::SoulDamped:
        return {0.92f, 0.90f, 0.60f, -0.18f, 0.68f, 0.60f, 0.48f, 0.96f};
    case DrumPalette::NeoSoul:
        return {0.82f, 0.88f, 0.62f, -0.20f, 0.76f, 0.56f, 0.38f, 0.94f};
    case DrumPalette::FunkDry:
        return {1.03f, 1.08f, 0.48f, 0.02f, 0.48f, 1.22f, 0.28f, 1.06f};
    case DrumPalette::Boom12:
        return {0.86f, 0.90f, 0.58f, -0.16f, 0.62f, 0.86f, 0.38f, 1.12f};
    case DrumPalette::Trap808:
        return {0.72f, 1.12f, 0.74f, 0.18f, 0.58f, 1.18f, 0.24f, 1.16f};
    case DrumPalette::ReggaeDub:
        return {0.86f, 0.90f, 0.82f, -0.16f, 0.86f, 0.52f, 1.02f, 0.96f};
    case DrumPalette::Bossa:
        return {1.12f, 1.18f, 0.58f, -0.14f, 0.60f, 0.54f, 0.64f, 0.92f};
    case DrumPalette::MetalLayered:
        return {1.06f, 1.10f, 0.54f, 0.12f, 0.72f, 1.52f, 0.68f, 1.20f};
    case DrumPalette::ElectronicMetal:
        return {0.82f, 1.05f, 0.48f, 0.02f, 0.54f, 1.36f, 0.24f, 1.42f};
    case DrumPalette::OrganicHeavy:
        return {0.98f, 0.92f, 1.08f, 0.05f, 1.08f, 1.24f, 1.48f, 1.10f};
    case DrumPalette::Simmons:
        return {1.04f, 1.22f, 0.74f, 0.08f, 0.64f, 1.26f, 0.48f, 1.18f};
    case DrumPalette::RhythmBox:
        return {1.02f, 1.08f, 0.58f, -0.10f, 0.54f, 0.82f, 0.34f, 1.14f};
    case DrumPalette::Industrial:
        return {0.78f, 0.92f, 0.50f, -0.08f, 0.58f, 1.40f, 0.24f, 1.52f};
    case DrumPalette::Lofi:
        return {0.92f, 0.88f, 0.74f, -0.22f, 0.68f, 0.58f, 0.58f, 1.00f};
    }
    return {1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};
}

QString paletteId(DrumPalette palette)
{
    switch (palette) {
    case DrumPalette::SoftCircuit: return QStringLiteral("soft-circuit");
    case DrumPalette::CleanPcm: return QStringLiteral("early-digital-pcm");
    case DrumPalette::HybridPop: return QStringLiteral("modern-hybrid-pop");
    case DrumPalette::DryRock: return QStringLiteral("dry-acoustic-rock");
    case DrumPalette::OpenRock: return QStringLiteral("open-acoustic-rock");
    case DrumPalette::Garage: return QStringLiteral("driven-garage");
    case DrumPalette::BrushJazz: return QStringLiteral("brush-jazz");
    case DrumPalette::BopJazz: return QStringLiteral("ride-led-bop");
    case DrumPalette::Fusion: return QStringLiteral("hybrid-fusion");
    case DrumPalette::HandModal: return QStringLiteral("skin-and-wood-hand-percussion");
    case DrumPalette::Atmosphere: return QStringLiteral("air-and-skin-objects");
    case DrumPalette::BluesClub: return QStringLiteral("club-shell-blues");
    case DrumPalette::DarkBlues: return QStringLiteral("dark-shell-blues");
    case DrumPalette::Anisong: return QStringLiteral("layered-anisong-rock");
    case DrumPalette::GlossyDance: return QStringLiteral("glossy-digital-dance");
    case DrumPalette::CountryDry: return QStringLiteral("dry-country");
    case DrumPalette::CountryPolished: return QStringLiteral("polished-country");
    case DrumPalette::House909: return QStringLiteral("909-descendant");
    case DrumPalette::Techno: return QStringLiteral("driven-techno-machine");
    case DrumPalette::Break12: return QStringLiteral("12-bit-break");
    case DrumPalette::SoulDamped: return QStringLiteral("damped-soul-studio");
    case DrumPalette::NeoSoul: return QStringLiteral("soft-neosoul-pocket");
    case DrumPalette::FunkDry: return QStringLiteral("dry-funk-ghost-pocket");
    case DrumPalette::Boom12: return QStringLiteral("12-bit-boom-bap");
    case DrumPalette::Trap808: return QStringLiteral("tuned-808-descendant");
    case DrumPalette::ReggaeDub: return QStringLiteral("one-drop-dub");
    case DrumPalette::Bossa: return QStringLiteral("skin-wood-shaker-bossa");
    case DrumPalette::MetalLayered: return QStringLiteral("layered-modern-metal");
    case DrumPalette::ElectronicMetal: return QStringLiteral("electronic-metal-underlay");
    case DrumPalette::OrganicHeavy: return QStringLiteral("organic-heavy-room");
    case DrumPalette::Simmons: return QStringLiteral("simmons-pitch-sweep");
    case DrumPalette::RhythmBox: return QStringLiteral("compact-rhythm-box");
    case DrumPalette::Industrial: return QStringLiteral("inharmonic-industrial");
    case DrumPalette::Lofi: return QStringLiteral("bandlimited-lofi");
    }
    return QStringLiteral("researched");
}

QStringList paletteReferences(DrumPalette palette)
{
    const QString daisy =
        QStringLiteral(
            "https://github.com/electro-smith/DaisySP/tree/master/Source/Drums");
    switch (palette) {
    case DrumPalette::SoftCircuit:
    case DrumPalette::RhythmBox:
        return {
            QStringLiteral(
                "https://articles.roland.com/cr-78-the-whole-story/"),
            QStringLiteral(
                "https://articles.roland.com/drumatix-the-perpetual-appeal-of-the-tr-606/"),
            daisy,
        };
    case DrumPalette::CleanPcm:
    case DrumPalette::Break12:
    case DrumPalette::Boom12:
    case DrumPalette::Lofi:
        return {
            QStringLiteral(
                "https://shop.rossum-electro.com/products/sp-1200"),
            QStringLiteral(
                "https://synthfool.com/docs/Oberheim/Oberheim_DMX_Owners_Manual.pdf"),
            QStringLiteral("https://www.akaipro.com/mpc-manuals"),
        };
    case DrumPalette::House909:
    case DrumPalette::Techno:
        return {
            QStringLiteral(
                "https://cdn.roland.com/assets/media/pdf/TR-909_OM.pdf"),
            QStringLiteral(
                "https://www.roland.com/uk/products/rc_tr-909/"),
            daisy,
        };
    case DrumPalette::Trap808:
        return {
            QStringLiteral(
                "https://www.roland.com/uk/promos/roland_tr-808/"),
            QStringLiteral(
                "https://www.sonicacademy.com/products/kick-2"),
            QStringLiteral(
                "https://github.com/Chowdhury-DSP/ChowKick"),
        };
    case DrumPalette::HandModal:
    case DrumPalette::Atmosphere:
    case DrumPalette::Bossa:
        return {
            QStringLiteral(
                "https://cdn.korg.com/us/support/download/files/bc29c774215caaa6e5ee12f7a1f21e47.pdf"),
            QStringLiteral(
                "https://www.applied-acoustics.com/chromaphone-3/manual/"),
            QStringLiteral(
                "https://faustlibraries.grame.fr/libs/physmodels/"),
        };
    case DrumPalette::Simmons:
    case DrumPalette::Industrial:
        return {
            QStringLiteral(
                "https://simmons.synth.net/sds8/docs/sds_8_user_guide.pdf"),
            QStringLiteral(
                "https://www.nordkeyboards.com/wt/documents/97/Nord%20Drum%203P%20English%20User%20Manual%20v1.x%20Edition%20C.pdf"),
            QStringLiteral(
                "https://www.korg.com/us/products/dj/volca_drum/"),
        };
    case DrumPalette::MetalLayered:
    case DrumPalette::ElectronicMetal:
    case DrumPalette::OrganicHeavy:
        return {
            QStringLiteral(
                "https://www.audiotechnology.com/features/recording-spiritbox"),
            QStringLiteral(
                "https://articles.roland.com/roland-engineering-understanding-prismatic-sound-modeling/"),
            QStringLiteral(
                "https://ca.yamaha.com/en/musical-instruments/drums/products/electronic-trigger-modules/dtxpro/index.html"),
        };
    case DrumPalette::BrushJazz:
    case DrumPalette::BopJazz:
    case DrumPalette::Fusion:
    case DrumPalette::DryRock:
    case DrumPalette::OpenRock:
    case DrumPalette::Garage:
    case DrumPalette::BluesClub:
    case DrumPalette::DarkBlues:
    case DrumPalette::CountryDry:
    case DrumPalette::CountryPolished:
        return {
            QStringLiteral(
                "https://doi.org/10.1109/TASL.2009.2036903"),
            QStringLiteral(
                "https://doi.org/10.11395/aem.4.0_205"),
            QStringLiteral(
                "https://www.dafx.de/paper-archive/details/qe8SzTmE_uIgihtLswWFFA"),
        };
    default:
        return {
            QStringLiteral(
                "https://downloads.sugar-bytes.de/manuals/DrumComputer_iPad.pdf"),
            QStringLiteral(
                "https://www.applied-acoustics.com/chromaphone-3/manual/"),
            daisy,
        };
    }
}

std::vector<ResearchKitCandidate> researchKitCandidates(
    const ProfileDefinition& profile)
{
    const auto row = std::find_if(
        kResearchKitRows.begin(),
        kResearchKitRows.end(),
        [&profile](const ResearchKitRow& value) {
            return profile.id == QString::fromLatin1(value.profileId);
        });
    if (row == kResearchKitRows.end()) {
        return {{
            QStringLiteral("researched-default"),
            QStringLiteral("Researched default"),
            DrumPalette::SoftCircuit,
            paletteDescription(DrumPalette::SoftCircuit),
            true,
            0,
        }};
    }
    std::vector<ResearchKitCandidate> result;
    for (int index = 0; index < 3; ++index) {
        const QString name =
            QString::fromUtf8(row->names[index]);
        result.push_back({
            slugifyCandidate(name),
            name,
            row->palettes[index],
            paletteDescription(row->palettes[index]),
            index == row->recommendedIndex,
            index,
        });
    }
    return result;
}

void setEvidenceVelocityBands(
    DrumLabPatch& patch,
    const ProfileDefinition& profile,
    DrumKind kind)
{
    const bool snareLike =
        kind == DrumKind::Snare ||
        kind == DrumKind::CrossStick;
    const QString style = profile.styleId;
    if (style == QStringLiteral("jazz")) {
        patch.velocity.ghost = snareLike
            ? DrumLabPatch::VelocityBand{13, 34}
            : DrumLabPatch::VelocityBand{25, 44};
        patch.velocity.normal = snareLike
            ? DrumLabPatch::VelocityBand{34, 76}
            : DrumLabPatch::VelocityBand{46, 88};
        patch.velocity.accent = {72, 112};
        patch.velocity.outputCurve = 1.48f;
    } else if (style == QStringLiteral("blues") ||
               style == QStringLiteral("reggae") ||
               style == QStringLiteral("bossa-nova")) {
        patch.velocity.ghost = {8, 30};
        patch.velocity.normal = {28, 72};
        patch.velocity.accent = {66, 108};
        patch.velocity.outputCurve = 1.38f;
    } else if (profile.id == QStringLiteral("rock_punk_garage")) {
        patch.velocity.ghost = {10, 38};
        patch.velocity.normal = {46, 102};
        patch.velocity.accent = {104, 127};
        patch.velocity.outputCurve = 1.08f;
    } else if (style == QStringLiteral("rock") ||
               profile.id == QStringLiteral("metal_modern_progressive")) {
        patch.velocity.ghost = {14, 42};
        patch.velocity.normal = {54, 108};
        patch.velocity.accent = {106, 127};
        patch.velocity.outputCurve = 1.16f;
    } else if (profile.id == QStringLiteral("hiphop_boom_bap") ||
               profile.id == QStringLiteral("hiphop_trap")) {
        patch.velocity.ghost = {13, 39};
        patch.velocity.normal = {48, 104};
        patch.velocity.accent = {100, 127};
        patch.velocity.outputCurve = 1.20f;
    } else {
        patch.velocity.ghost = {14, 42};
        patch.velocity.normal = {46, 100};
        patch.velocity.accent = {96, 127};
        patch.velocity.outputCurve = 1.25f;
    }
    patch.velocity.excitationCurve = 0.70f;
    patch.velocity.brightnessAmount =
        kind == DrumKind::Kick ? 0.10f : 0.20f;
    patch.velocity.decayAmount =
        kind == DrumKind::ClosedHat ? 0.04f : 0.12f;
    patch.velocity.driveAmount = 0.14f;
}

DrumLabPatch candidatePiecePatch(
    const ProfileDefinition& profile,
    const ResearchKitCandidate& candidate,
    DrumKind kind)
{
    DrumLabPatch patch = defaultDrumLabPatch(profile, kind);
    setEvidenceVelocityBands(patch, profile, kind);
    patch.intendedIdentity = laneNameForDrumKind(kind);
    patch.source = QStringLiteral("daisy-profile");
    patch.secondSource = QStringLiteral("off");
    patch.blend = 0.0f;
    patch.transient = QStringLiteral("off");
    patch.texture = QStringLiteral("off");
    patch.transientLevel = 0.0f;
    patch.textureLevel = 0.0f;
    patch.voiceDrive = 1.08f;
    patch.roomSend = 0.08f;
    patch.digitalSampleRateHz = 48000.0f;
    patch.digitalBitDepth = 24;
    patch.reconstructionLowpassHz = 20000.0f;
    patch.dynamicFilterAmount = 0.0f;
    patch.synth.source = QStringLiteral("off");
    switch (kind) {
    case DrumKind::Kick:
        patch.intendedIdentity =
            QStringLiteral("recognisable low-drum kick");
        patch.source = QStringLiteral("daisy-synthetic-kick");
        patch.secondSource = QStringLiteral("daisy-analog-kick");
        patch.blend = 0.18f;
        patch.frequency = 56.0f;
        patch.tone = 0.38f;
        patch.decay = 0.32f;
        patch.colour = 0.055f;
        patch.fmAmount = 0.26f;
        patch.level = 0.58f;
        patch.transient = QStringLiteral("soft-beater");
        patch.transientLevel = 0.12f;
        patch.transientTone = 0.28f;
        patch.transientDecaySeconds = 0.010f;
        patch.roomSend = 0.06f;
        break;
    case DrumKind::Snare:
        patch.intendedIdentity =
            QStringLiteral(
                "shell-and-stick snare with controlled wire grit");
        patch.source = QStringLiteral("jam2-shell-snare");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency = 178.0f;
        patch.tone = 0.28f;
        patch.decay = 0.11f;
        patch.colour = 0.48f;
        patch.fmAmount = 0.10f;
        patch.level = 0.48f;
        patch.transient = QStringLiteral("stick");
        patch.transientLevel = 0.20f;
        patch.transientTone = 0.46f;
        patch.transientDecaySeconds = 0.006f;
        patch.texture = QStringLiteral("wire");
        patch.textureLevel = 0.085f;
        patch.textureTone = 0.46f;
        patch.textureDecaySeconds = 0.070f;
        patch.textureDensity = 0.46f;
        patch.roomSend = 0.10f;
        break;
    case DrumKind::ClosedHat:
    case DrumKind::OpenHat:
        patch.intendedIdentity =
            kind == DrumKind::OpenHat
                ? QStringLiteral("sustaining open hi-hat")
                : QStringLiteral("short choked closed hi-hat");
        patch.source = QStringLiteral("daisy-ring-metal");
        patch.secondSource = QStringLiteral("daisy-metal");
        patch.blend = 0.18f;
        patch.frequency = 3050.0f;
        patch.tone = 0.48f;
        patch.decay =
            kind == DrumKind::OpenHat ? 0.40f : 0.075f;
        patch.colour = 0.78f;
        patch.level =
            kind == DrumKind::OpenHat ? 0.22f : 0.19f;
        patch.transient = QStringLiteral("stick");
        patch.transientLevel = 0.055f;
        patch.transientTone = 0.72f;
        patch.transientDecaySeconds = 0.004f;
        patch.texture = QStringLiteral("air");
        patch.textureLevel =
            kind == DrumKind::OpenHat ? 0.035f : 0.012f;
        patch.textureTone = 0.64f;
        patch.textureDecaySeconds =
            kind == DrumKind::OpenHat ? 0.34f : 0.055f;
        patch.chokeGroup = QStringLiteral("hats");
        patch.chokeSeconds = 0.010f;
        patch.roomSend = 0.055f;
        break;
    case DrumKind::HighTom:
    case DrumKind::MidTom:
    case DrumKind::FloorTom:
        patch.intendedIdentity =
            kind == DrumKind::HighTom
                ? QStringLiteral(
                    "high rack tom with a clear membrane strike")
                : kind == DrumKind::FloorTom
                    ? QStringLiteral(
                        "low floor tom with a deep shell response")
                    : QStringLiteral(
                        "mid rack tom with a rounded shell response");
        patch.source = QStringLiteral("jam2-shell-tom");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency =
            kind == DrumKind::HighTom ? 176.0f :
            kind == DrumKind::FloorTom ? 82.0f : 124.0f;
        patch.tone = 0.34f;
        patch.decay =
            kind == DrumKind::HighTom ? 0.25f :
            kind == DrumKind::FloorTom ? 0.38f : 0.31f;
        patch.colour = 0.06f;
        patch.fmAmount = 0.32f;
        patch.level =
            kind == DrumKind::FloorTom ? 0.48f : 0.44f;
        patch.transient = QStringLiteral("head-strike");
        patch.transientLevel = 0.16f;
        patch.transientTone =
            kind == DrumKind::HighTom ? 0.46f :
            kind == DrumKind::FloorTom ? 0.32f : 0.39f;
        patch.transientDecaySeconds = 0.006f;
        patch.roomSend = 0.10f;
        break;
    case DrumKind::Crash:
        patch.intendedIdentity =
            QStringLiteral("diffuse full-band crash cymbal");
        patch.source = QStringLiteral("jam2-crash-cymbal");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency = 3150.0f;
        patch.tone = 0.54f;
        patch.decay = 0.68f;
        patch.colour = 0.34f;
        patch.fmAmount = 0.12f;
        patch.level = 0.22f;
        patch.transient = QStringLiteral("stick");
        patch.transientLevel = 0.065f;
        patch.transientTone = 0.64f;
        patch.transientDecaySeconds = 0.006f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
        patch.roomSend = 0.11f;
        break;
    case DrumKind::Ride:
        patch.intendedIdentity =
            QStringLiteral("stick-defined ride cymbal with subdued wash");
        patch.source = QStringLiteral("jam2-ride-cymbal");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency = 2300.0f;
        patch.tone = 0.46f;
        patch.decay = 0.62f;
        patch.colour = 0.25f;
        patch.fmAmount = 0.10f;
        patch.level = 0.24f;
        patch.transient = QStringLiteral("stick");
        patch.transientLevel = 0.12f;
        patch.transientTone = 0.58f;
        patch.transientDecaySeconds = 0.005f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
        patch.roomSend = 0.09f;
        break;
    case DrumKind::CrossStick:
        patch.intendedIdentity =
            QStringLiteral("dry wooden cross-stick rim click");
        patch.source = QStringLiteral("jam2-cross-stick");
        patch.frequency = 640.0f;
        patch.tone = 0.43f;
        patch.decay = 0.10f;
        patch.colour = 0.08f;
        patch.fmAmount = 0.0f;
        patch.level = 0.29f;
        patch.transient = QStringLiteral("rim");
        patch.transientLevel = 0.032f;
        patch.transientTone = 0.34f;
        patch.transientDecaySeconds = 0.005f;
        patch.roomSend = 0.055f;
        break;
    case DrumKind::Shaker:
        patch.intendedIdentity =
            QStringLiteral("short seed-collision shaker");
        patch.source = QStringLiteral("jam2-shaker");
        patch.frequency = 4800.0f;
        patch.tone = 0.58f;
        patch.decay = 0.085f;
        patch.colour = 0.70f;
        patch.fmAmount = 0.0f;
        patch.level = 0.18f;
        patch.transient = QStringLiteral("brush");
        patch.transientLevel = 0.025f;
        patch.transientTone = 0.68f;
        patch.transientDecaySeconds = 0.008f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
        patch.roomSend = 0.045f;
        break;
    case DrumKind::HandPercussion:
        patch.intendedIdentity =
            QStringLiteral("muted conga-style skin hand drum");
        patch.source = QStringLiteral("jam2-hand-drum");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency = 215.0f;
        patch.tone = 0.30f;
        patch.decay = 0.16f;
        patch.colour = 0.12f;
        patch.fmAmount = 0.26f;
        patch.level = 0.31f;
        patch.transient = QStringLiteral("soft-beater");
        patch.transientLevel = 0.075f;
        patch.transientTone = 0.32f;
        patch.transientDecaySeconds = 0.008f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
        patch.roomSend = 0.075f;
        break;
    }

    const DrumPalette palette = candidate.palette;
    const bool kick = kind == DrumKind::Kick;
    const bool snare = kind == DrumKind::Snare;
    const bool hat =
        kind == DrumKind::ClosedHat ||
        kind == DrumKind::OpenHat;
    const bool metal =
        hat ||
        kind == DrumKind::Crash ||
        kind == DrumKind::Ride;
    if (palette == DrumPalette::SoftCircuit ||
        palette == DrumPalette::RhythmBox) {
        patch.tone *= 0.78f;
        patch.decay *= 0.78f;
        patch.voiceDrive = 1.18f;
        patch.roomSend *= 0.65f;
        if (kick) {
            patch.frequency = 58.0f;
            patch.source = QStringLiteral("daisy-analog-kick");
            patch.secondSource = QStringLiteral("daisy-synthetic-kick");
            patch.transientLevel = 0.10f;
        }
        if (snare) {
            patch.textureLevel = 0.055f;
        }
        if (hat) {
            patch.source = QStringLiteral("daisy-metal");
            patch.secondSource = QStringLiteral("off");
            patch.reconstructionLowpassHz = 9500.0f;
        }
    }
    if (palette == DrumPalette::CleanPcm ||
        palette == DrumPalette::GlossyDance) {
        patch.digitalSampleRateHz =
            palette == DrumPalette::CleanPcm ? 26040.0f : 32000.0f;
        patch.digitalBitDepth =
            palette == DrumPalette::CleanPcm ? 12 : 16;
        patch.reconstructionLowpassHz =
            palette == DrumPalette::CleanPcm ? 10800.0f : 15000.0f;
        patch.dynamicFilterAmount = 0.24f;
        patch.transientLevel *= 1.34f;
        patch.roomSend *= 0.62f;
        if (snare && palette == DrumPalette::GlossyDance) {
            patch.transient = QStringLiteral("clap");
            patch.transientLevel = 0.28f;
            patch.texture = QStringLiteral("air");
        }
    }
    if (palette == DrumPalette::HybridPop ||
        palette == DrumPalette::Anisong ||
        palette == DrumPalette::CountryPolished) {
        patch.transientLevel *=
            palette == DrumPalette::Anisong ? 1.55f : 1.25f;
        patch.textureLevel *= 0.90f;
        patch.roomSend *=
            palette == DrumPalette::CountryPolished ? 1.35f : 1.08f;
        if (kick) {
            patch.frequency =
                palette == DrumPalette::Anisong ? 63.0f : 59.0f;
            patch.transient =
                palette == DrumPalette::Anisong
                    ? QStringLiteral("hard-beater")
                    : QStringLiteral("soft-beater");
        }
        if (snare && palette == DrumPalette::HybridPop) {
            patch.secondSource =
                QStringLiteral("daisy-synthetic-snare");
            patch.blend = 0.30f;
        }
    }
    if (palette == DrumPalette::DryRock ||
        palette == DrumPalette::OpenRock ||
        palette == DrumPalette::Garage ||
        palette == DrumPalette::OrganicHeavy) {
        patch.roomSend *=
            palette == DrumPalette::DryRock ? 0.72f :
            palette == DrumPalette::Garage ? 1.22f : 1.55f;
        patch.voiceDrive =
            palette == DrumPalette::Garage ? 1.55f : 1.16f;
        if (kick) {
            patch.frequency =
                palette == DrumPalette::OrganicHeavy ? 62.0f : 60.0f;
            patch.decay =
                palette == DrumPalette::OpenRock ? 0.45f : 0.30f;
            patch.transient =
                palette == DrumPalette::Garage
                    ? QStringLiteral("hard-beater")
                    : QStringLiteral("soft-beater");
        }
        if (snare) {
            patch.decay =
                palette == DrumPalette::OpenRock ? 0.20f :
                palette == DrumPalette::OrganicHeavy ? 0.17f :
                palette == DrumPalette::Garage ? 0.11f : 0.14f;
            patch.textureLevel =
                palette == DrumPalette::OpenRock ? 0.14f : 0.10f;
            patch.transientTone =
                palette == DrumPalette::Garage ? 0.84f : 0.66f;
        }
    }
    if (palette == DrumPalette::BrushJazz ||
        palette == DrumPalette::BopJazz ||
        palette == DrumPalette::BluesClub ||
        palette == DrumPalette::DarkBlues ||
        palette == DrumPalette::CountryDry ||
        palette == DrumPalette::Bossa) {
        patch.voiceDrive = 1.02f;
        patch.velocity.outputCurve = 1.48f;
        if (kick) {
            patch.source = QStringLiteral("daisy-analog-kick");
            patch.secondSource = QStringLiteral("off");
            patch.blend = 0.0f;
            patch.frequency =
                palette == DrumPalette::Bossa ? 72.0f :
                palette == DrumPalette::DarkBlues ? 52.0f : 62.0f;
            patch.tone = 0.28f;
            patch.transientLevel =
                palette == DrumPalette::BrushJazz ? 0.035f : 0.09f;
        }
        if (snare) {
            if (palette == DrumPalette::BrushJazz ||
                palette == DrumPalette::Bossa) {
                patch.transient = QStringLiteral("brush");
                patch.transientLevel = 0.18f;
                patch.texture = QStringLiteral("wire");
                patch.textureLevel = 0.12f;
            }
            patch.tone *= 0.82f;
        }
        if (kind == DrumKind::Ride) {
            patch.intendedIdentity =
                QStringLiteral(
                    "dark stick-defined jazz ride cymbal");
            patch.source =
                QStringLiteral("jam2-ride-cymbal");
            patch.secondSource = QStringLiteral("off");
            patch.blend = 0.0f;
            patch.frequency = 2180.0f;
            patch.tone =
                palette == DrumPalette::BopJazz ? 0.52f : 0.38f;
            patch.decay = 0.86f;
            patch.transientLevel =
                palette == DrumPalette::BopJazz ? 0.22f : 0.14f;
        }
        if (palette == DrumPalette::Bossa &&
            (kind == DrumKind::Shaker ||
             kind == DrumKind::HandPercussion)) {
            patch.level *= 1.18f;
            patch.textureDensity *= 0.72f;
        }
    }
    if (palette == DrumPalette::Fusion ||
        palette == DrumPalette::Simmons) {
        patch.voiceDrive = 1.20f;
        patch.transientLevel *= 1.28f;
        patch.roomSend *= 0.82f;
        if (kick) {
            patch.frequency = 65.0f;
            patch.fmAmount = 0.68f;
        }
        if (isTomKind(kind)) {
            patch.intendedIdentity =
                palette == DrumPalette::Simmons
                    ? QStringLiteral(
                        "pitch-swept Simmons-style electronic tom")
                    : QStringLiteral(
                        "tight fusion shell tom");
            patch.source =
                palette == DrumPalette::Simmons
                    ? QStringLiteral("daisy-synthetic-kick")
                    : QStringLiteral("jam2-shell-tom");
            patch.secondSource = QStringLiteral("off");
            patch.fmAmount =
                palette == DrumPalette::Simmons ? 0.78f : 0.48f;
            patch.colour =
                palette == DrumPalette::Simmons ? 0.18f : 0.08f;
            patch.decay =
                palette == DrumPalette::Simmons ? 0.38f : 0.26f;
        }
    }
    if (palette == DrumPalette::HandModal ||
        palette == DrumPalette::Atmosphere) {
        patch.transient =
            palette == DrumPalette::Atmosphere
                ? QStringLiteral("soft-beater")
                : patch.transient;
        patch.transientLevel *= 0.62f;
        patch.texture =
            metal ? QStringLiteral("air")
                  : QStringLiteral("off");
        patch.textureLevel *=
            palette == DrumPalette::Atmosphere ? 0.72f : 0.55f;
        patch.textureDecaySeconds *=
            palette == DrumPalette::Atmosphere ? 1.75f : 1.10f;
        patch.roomSend *=
            palette == DrumPalette::Atmosphere ? 2.2f : 1.35f;
        if (kind == DrumKind::HandPercussion) {
            patch.intendedIdentity =
                palette == DrumPalette::Atmosphere
                    ? QStringLiteral(
                        "soft low hand drum in a long room")
                    : QStringLiteral(
                        "open conga-style hand drum");
            patch.source = QStringLiteral("jam2-hand-drum");
            patch.frequency =
                palette == DrumPalette::Atmosphere
                    ? 172.0f : 238.0f;
            patch.decay =
                palette == DrumPalette::Atmosphere
                    ? 0.22f : 0.17f;
        }
    }
    if (palette == DrumPalette::House909 ||
        palette == DrumPalette::Techno) {
        patch.roomSend *= 0.52f;
        patch.voiceDrive =
            palette == DrumPalette::Techno ? 1.82f : 1.36f;
        if (kick) {
            patch.source = QStringLiteral("daisy-synthetic-kick");
            patch.secondSource = QStringLiteral("daisy-analog-kick");
            patch.blend = 0.18f;
            patch.frequency =
                palette == DrumPalette::Techno ? 47.0f : 50.0f;
            patch.decay =
                palette == DrumPalette::Techno ? 0.36f : 0.50f;
            patch.transient =
                palette == DrumPalette::Techno
                    ? QStringLiteral("hard-beater")
                    : QStringLiteral("click");
        }
        if (snare) {
            patch.source =
                QStringLiteral("daisy-synthetic-snare");
            patch.transient =
                palette == DrumPalette::House909
                    ? QStringLiteral("clap")
                    : QStringLiteral("stick");
        }
        if (metal) {
            patch.digitalSampleRateHz = 18000.0f;
            patch.digitalBitDepth = 6;
            patch.reconstructionLowpassHz = 10500.0f;
        }
    }
    if (palette == DrumPalette::Break12 ||
        palette == DrumPalette::Boom12 ||
        palette == DrumPalette::Lofi ||
        palette == DrumPalette::SoulDamped) {
        patch.digitalSampleRateHz =
            palette == DrumPalette::Break12 ||
                    palette == DrumPalette::Boom12
                ? 26040.0f : 22050.0f;
        patch.digitalBitDepth =
            palette == DrumPalette::Lofi ? 10 : 12;
        patch.reconstructionLowpassHz =
            palette == DrumPalette::Boom12 ? 7600.0f :
            palette == DrumPalette::SoulDamped ? 6900.0f :
            palette == DrumPalette::Lofi ? 6200.0f : 9800.0f;
        patch.dynamicFilterAmount = 0.32f;
        patch.roomSend *= 0.72f;
        if (kick) {
            patch.decay =
                palette == DrumPalette::Boom12 ? 0.28f : 0.32f;
            patch.transientLevel *= 0.84f;
        }
        if (snare && palette == DrumPalette::SoulDamped) {
            patch.decay = 0.18f;
            patch.texture = QStringLiteral("wire");
            patch.textureLevel = 0.065f;
        }
    }
    if (palette == DrumPalette::NeoSoul ||
        palette == DrumPalette::FunkDry) {
        patch.roomSend *=
            palette == DrumPalette::FunkDry ? 0.35f : 0.62f;
        patch.decay *=
            palette == DrumPalette::FunkDry ? 0.62f : 0.78f;
        patch.velocity.outputCurve = 1.52f;
        patch.velocity.brightnessAmount *= 1.22f;
        if (kick) {
            patch.frequency =
                palette == DrumPalette::NeoSoul ? 48.0f : 61.0f;
            patch.transientLevel *= 0.62f;
        }
        if (snare) {
            patch.transient =
                palette == DrumPalette::NeoSoul
                    ? QStringLiteral("rim")
                    : QStringLiteral("stick");
            patch.textureLevel *= 0.78f;
        }
    }
    if (palette == DrumPalette::Trap808) {
        patch.roomSend *= 0.28f;
        if (kick) {
            patch.source = QStringLiteral("daisy-synthetic-kick");
            patch.secondSource = QStringLiteral("off");
            patch.blend = 0.0f;
            patch.frequency = 43.65f;
            patch.tone = 0.22f;
            patch.decay = 0.88f;
            patch.colour = 0.045f;
            patch.fmAmount = 0.72f;
            patch.transient = QStringLiteral("click");
            patch.transientLevel = 0.12f;
            patch.transientTone = 0.40f;
            patch.synth.source = QStringLiteral("sine-fundamental");
            patch.synth.midiNote = 29;
            patch.synth.level = 0.18f;
            patch.synth.gateSeconds = 0.42f;
            patch.synth.decay = 0.60f;
            patch.synth.release = 0.38f;
            patch.synth.filterCutoff = 1400.0f;
        }
        if (snare) {
            patch.source =
                QStringLiteral("daisy-synthetic-snare");
            patch.secondSource = QStringLiteral("off");
            patch.blend = 0.0f;
            patch.frequency = 174.0f;
            patch.tone = 0.22f;
            patch.colour = 0.90f;
            patch.fmAmount = 0.10f;
            patch.decay = 0.17f;
            patch.transient = QStringLiteral("clap");
            patch.textureLevel = 0.06f;
        }
        if (kind == DrumKind::ClosedHat) {
            patch.decay = 0.055f;
            patch.transientLevel = 0.14f;
        }
    }
    if (palette == DrumPalette::ReggaeDub) {
        patch.tone *= 0.70f;
        patch.reconstructionLowpassHz = 8800.0f;
        patch.roomSend *= 0.74f;
        if (kick) {
            patch.frequency = 49.0f;
            patch.source = QStringLiteral("daisy-analog-kick");
            patch.transientLevel = 0.025f;
            patch.decay = 0.42f;
        }
        if (snare) {
            patch.transient = QStringLiteral("rim");
            patch.transientLevel = 0.13f;
            patch.textureLevel = 0.10f;
        }
    }
    if (palette == DrumPalette::MetalLayered ||
        palette == DrumPalette::ElectronicMetal) {
        patch.voiceDrive =
            palette == DrumPalette::MetalLayered ? 1.48f : 1.76f;
        patch.roomSend *=
            palette == DrumPalette::MetalLayered ? 0.88f : 0.36f;
        patch.transientLevel *= 1.75f;
        if (kick) {
            patch.frequency =
                palette == DrumPalette::MetalLayered ? 58.0f : 50.0f;
            patch.decay = 0.25f;
            patch.transient = QStringLiteral("hard-beater");
            patch.transientTone = 0.82f;
            patch.synth.source =
                palette == DrumPalette::ElectronicMetal
                    ? QStringLiteral("off")
                    : QStringLiteral("sine-fundamental");
            patch.synth.midiNote = 36;
            patch.synth.level =
                palette == DrumPalette::ElectronicMetal
                    ? 0.0f : 0.08f;
        }
        if (snare) {
            patch.source =
                palette == DrumPalette::MetalLayered
                    ? QStringLiteral("jam2-shell-snare")
                    : QStringLiteral("daisy-synthetic-snare");
            patch.secondSource =
                palette == DrumPalette::MetalLayered
                    ? QStringLiteral("daisy-synthetic-snare")
                    : QStringLiteral("jam2-shell-snare");
            patch.blend =
                palette == DrumPalette::MetalLayered
                    ? 0.16f : 0.18f;
            patch.frequency =
                palette == DrumPalette::MetalLayered
                    ? 176.0f : 168.0f;
            patch.tone =
                palette == DrumPalette::MetalLayered
                    ? 0.20f : 0.28f;
            patch.colour =
                palette == DrumPalette::MetalLayered
                    ? 0.46f : 0.56f;
            patch.fmAmount =
                palette == DrumPalette::MetalLayered
                    ? 0.09f : 0.18f;
            patch.decay =
                palette == DrumPalette::MetalLayered
                    ? 0.070f : 0.12f;
            patch.transient = QStringLiteral("stick");
            patch.textureLevel =
                palette == DrumPalette::MetalLayered
                    ? 0.095f : 0.080f;
        }
        if (metal) {
            patch.textureLevel *= 1.35f;
            patch.textureDensity = 0.70f;
        }
    }
    if (palette == DrumPalette::Industrial) {
        patch.voiceDrive = 2.05f;
        patch.transient = QStringLiteral("click");
        patch.transientLevel *= 1.45f;
        patch.digitalSampleRateHz = 24000.0f;
        patch.digitalBitDepth = 10;
        patch.reconstructionLowpassHz = 11800.0f;
        if (kick) {
            patch.intendedIdentity =
                QStringLiteral(
                    "distorted short industrial kick");
            patch.source =
                QStringLiteral("daisy-synthetic-kick");
            patch.secondSource =
                QStringLiteral("daisy-analog-kick");
            patch.blend = 0.12f;
            patch.fmAmount = 0.68f;
        } else if (snare) {
            patch.intendedIdentity =
                QStringLiteral(
                    "bitten electronic industrial snare");
            patch.source =
                QStringLiteral("daisy-synthetic-snare");
            patch.secondSource = QStringLiteral("off");
            patch.blend = 0.0f;
            patch.frequency = 182.0f;
            patch.tone = 0.24f;
            patch.colour = 0.88f;
            patch.fmAmount = 0.16f;
        } else if (isTomKind(kind)) {
            patch.intendedIdentity =
                QStringLiteral(
                    "short pitch-swept industrial tom");
            patch.source =
                QStringLiteral("jam2-shell-tom");
            patch.fmAmount = 0.72f;
            patch.decay = 0.22f;
        } else if (kind == DrumKind::CrossStick) {
            patch.intendedIdentity =
                QStringLiteral(
                    "bit-crushed rim and wood cross-stick");
            patch.source =
                QStringLiteral("jam2-cross-stick");
        } else if (kind == DrumKind::HandPercussion) {
            patch.intendedIdentity =
                QStringLiteral(
                    "hard struck industrial wood block");
            patch.source =
                QStringLiteral("jam2-wood-block");
            patch.frequency = 760.0f;
            patch.decay = 0.12f;
        } else if (kind == DrumKind::Shaker) {
            patch.intendedIdentity =
                QStringLiteral(
                    "bit-crushed collision shaker");
            patch.source =
                QStringLiteral("jam2-shaker");
        } else if (metal) {
            patch.texture = QStringLiteral("air");
            patch.textureLevel *= 0.58f;
        }
    }

    // Profile identity remains authoritative when an alternative borrows a
    // palette from another family. The palette supplies colour; it must not
    // erase the profile's functional low drum or signature percussion.
    if (profile.id == QStringLiteral("hiphop_trap") && kick &&
        palette != DrumPalette::Trap808) {
        patch.source = QStringLiteral("daisy-synthetic-kick");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency =
            palette == DrumPalette::MetalLayered ? 46.25f : 43.65f;
        patch.decay =
            palette == DrumPalette::MetalLayered ? 0.64f : 0.78f;
        patch.tone =
            palette == DrumPalette::MetalLayered ? 0.32f : 0.24f;
        patch.fmAmount = 0.68f;
        patch.synth.source = QStringLiteral("sine-fundamental");
        patch.synth.midiNote =
            palette == DrumPalette::MetalLayered ? 30 : 29;
        patch.synth.level = 0.14f;
        patch.synth.gateSeconds = 0.34f;
        patch.synth.decay = 0.48f;
        patch.synth.release = 0.30f;
        patch.synth.filterCutoff = 1600.0f;
    }
    if (profile.id == QStringLiteral("electronic_house") && kick) {
        patch.frequency =
            palette == DrumPalette::SoftCircuit ? 48.0f : 50.0f;
        patch.decay = std::max(
            patch.decay,
            palette == DrumPalette::GlossyDance ? 0.42f : 0.50f);
    }
    if (profile.id == QStringLiteral("reggae_roots") && kick) {
        patch.frequency = 50.0f;
        patch.tone = std::min(patch.tone, 0.34f);
        patch.transientLevel =
            std::min(patch.transientLevel, 0.08f);
    }
    if (profile.id == QStringLiteral("soul_classic_motown") &&
        kind == DrumKind::Shaker) {
        patch.intendedIdentity =
            QStringLiteral(
                "bright Motown-style tambourine shake");
        patch.source = QStringLiteral("jam2-tambourine");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.frequency = 4300.0f;
        patch.tone = 0.58f;
        patch.decay = 0.18f;
        patch.colour = 0.72f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
        patch.level = 0.20f;
    }

    if (kind == DrumKind::HandPercussion &&
        palette != DrumPalette::HandModal &&
        palette != DrumPalette::Atmosphere &&
        palette != DrumPalette::Industrial) {
        switch (palette) {
        case DrumPalette::SoftCircuit:
        case DrumPalette::RhythmBox:
        case DrumPalette::House909:
        case DrumPalette::Techno:
        case DrumPalette::ElectronicMetal:
            patch.intendedIdentity =
                QStringLiteral(
                    "short electronic clave and wood block");
            patch.source = QStringLiteral("jam2-wood-block");
            patch.frequency = 720.0f;
            patch.tone = 0.46f;
            patch.decay = 0.12f;
            patch.transient = QStringLiteral("click");
            patch.transientLevel = 0.045f;
            break;
        case DrumPalette::CleanPcm:
        case DrumPalette::HybridPop:
        case DrumPalette::GlossyDance:
        case DrumPalette::Trap808:
            patch.intendedIdentity =
                QStringLiteral(
                    "short layered hand clap");
            patch.source = QStringLiteral("jam2-shaker");
            patch.frequency = 3900.0f;
            patch.tone = 0.52f;
            patch.decay = 0.10f;
            patch.colour = 0.50f;
            patch.transient = QStringLiteral("clap");
            patch.transientLevel = 0.34f;
            patch.transientTone = 0.58f;
            patch.transientDecaySeconds = 0.038f;
            patch.level = 0.26f;
            break;
        case DrumPalette::DryRock:
        case DrumPalette::OpenRock:
        case DrumPalette::Garage:
        case DrumPalette::Anisong:
        case DrumPalette::CountryPolished:
        case DrumPalette::MetalLayered:
        case DrumPalette::OrganicHeavy:
            patch.intendedIdentity =
                QStringLiteral(
                    "stick-struck tambourine");
            patch.source = QStringLiteral("jam2-tambourine");
            patch.frequency = 4100.0f;
            patch.tone = 0.55f;
            patch.decay = 0.19f;
            patch.colour = 0.64f;
            patch.transient = QStringLiteral("stick");
            patch.transientLevel = 0.065f;
            patch.level = 0.22f;
            break;
        case DrumPalette::CountryDry:
            patch.intendedIdentity =
                QStringLiteral(
                    "dry country wood block");
            patch.source = QStringLiteral("jam2-wood-block");
            patch.frequency = 690.0f;
            patch.tone = 0.40f;
            patch.decay = 0.10f;
            break;
        case DrumPalette::BrushJazz:
        case DrumPalette::BopJazz:
        case DrumPalette::Fusion:
        case DrumPalette::BluesClub:
        case DrumPalette::DarkBlues:
        case DrumPalette::Bossa:
        case DrumPalette::SoulDamped:
        case DrumPalette::NeoSoul:
        case DrumPalette::FunkDry:
        case DrumPalette::Boom12:
        case DrumPalette::Break12:
        case DrumPalette::ReggaeDub:
        case DrumPalette::Lofi:
            patch.intendedIdentity =
                palette == DrumPalette::Bossa
                    ? QStringLiteral(
                        "open high conga-style hand drum")
                    : palette == DrumPalette::ReggaeDub
                        ? QStringLiteral(
                            "muted reggae hand drum")
                        : QStringLiteral(
                            "muted conga-style hand drum");
            patch.source = QStringLiteral("jam2-hand-drum");
            patch.frequency =
                palette == DrumPalette::Bossa ? 286.0f :
                palette == DrumPalette::ReggaeDub ? 188.0f :
                220.0f;
            patch.tone =
                palette == DrumPalette::Bossa ? 0.38f : 0.28f;
            patch.decay =
                palette == DrumPalette::Bossa ? 0.13f : 0.16f;
            break;
        case DrumPalette::Simmons:
            patch.intendedIdentity =
                QStringLiteral(
                    "short Simmons-style auxiliary tom");
            patch.source =
                QStringLiteral("daisy-synthetic-kick");
            patch.frequency = 176.0f;
            patch.tone = 0.38f;
            patch.decay = 0.22f;
            patch.fmAmount = 0.72f;
            patch.transient = QStringLiteral("stick");
            patch.transientLevel = 0.08f;
            break;
        case DrumPalette::Industrial:
        case DrumPalette::HandModal:
        case DrumPalette::Atmosphere:
            break;
        }
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
    }
    if (kind == DrumKind::Kick ||
        isTomKind(kind) ||
        kind == DrumKind::Crash ||
        kind == DrumKind::Ride ||
        kind == DrumKind::CrossStick ||
        kind == DrumKind::Shaker) {
        // Core instrument models own their excitation and tail. Generic
        // palette noise must not reintroduce detached kick clicks, static
        // tom wash, computer-like rims, or sea-noise cymbal tails.
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
    }
    if (snare && patch.texture != QStringLiteral("wire")) {
        // A researched snare may colour its wire response, but unrelated
        // broadband air/dust is not a substitute for the shell and wires.
        patch.texture = QStringLiteral("wire");
        patch.textureLevel = std::min(
            patch.textureLevel,
            0.075f);
        patch.textureDecaySeconds = std::min(
            patch.textureDecaySeconds,
            0.095f);
    }
    if (snare) {
        patch.colour = std::min(patch.colour, 0.60f);
        patch.textureLevel = std::min(
            patch.textureLevel,
            0.12f);
        patch.textureDecaySeconds = std::min(
            patch.textureDecaySeconds,
            0.11f);
    }

    const PaletteKitCharacter paletteCharacter =
        paletteKitCharacter(palette);
    if (kind == DrumKind::Kick) {
        patch.frequency *= paletteCharacter.lowPitchScale;
        patch.decay *= paletteCharacter.shellTailScale;
    } else if (kind == DrumKind::Snare ||
               isTomKind(kind) ||
               kind == DrumKind::CrossStick ||
               kind == DrumKind::HandPercussion) {
        patch.frequency *= paletteCharacter.shellPitchScale;
        patch.decay *= paletteCharacter.shellTailScale;
    }
    if (metal || kind == DrumKind::Shaker ||
        patch.source == QStringLiteral("jam2-tambourine")) {
        patch.tone += paletteCharacter.metalToneOffset;
        patch.decay *= paletteCharacter.metalTailScale;
    }
    patch.transientLevel *= paletteCharacter.transientScale;
    patch.roomSend *= paletteCharacter.roomScale;
    patch.voiceDrive *= paletteCharacter.driveScale;

    const ProfileKitCharacter& character =
        profileKitCharacter(profile);
    if (kind == DrumKind::Kick ||
        kind == DrumKind::Snare ||
        isTomKind(kind) ||
        kind == DrumKind::CrossStick) {
        patch.frequency *= character.pitchScale;
    } else if (kind == DrumKind::HandPercussion) {
        patch.frequency *=
            character.pitchScale *
            character.handPitchScale;
    }
    if (metal || kind == DrumKind::Shaker ||
        patch.source == QStringLiteral("jam2-tambourine")) {
        patch.tone +=
            0.34f * (character.metalBrightness - 1.0f);
    }
    patch.decay *= character.tailScale;
    patch.transientLevel *= character.transientScale;
    patch.roomSend *= character.roomScale;
    patch.voiceDrive *= character.driveScale;

    // The current non-drum Daisy arrangements are not yet reliable mix
    // references. Apply only a modest per-hit lift outside the accepted Pop
    // kit, with extra support for the consistently inaudible Reggae set.
    if (profile.styleId != QStringLiteral("pop")) {
        patch.level *= 1.12f;
    }
    if (profile.styleId == QStringLiteral("reggae")) {
        patch.level *= 1.18f;
    }

    // Listening selected Smoky Brush Kit for Minor Blues, but its feathered
    // analogue kick was too quiet while Slow Crunch Room was too boomy.
    // Add a small synthetic body and a controlled medium decay between them.
    if (kind == DrumKind::Kick &&
        profile.id == QStringLiteral("blues_minor") &&
        palette == DrumPalette::BrushJazz) {
        patch.source = QStringLiteral("daisy-analog-kick");
        patch.secondSource = QStringLiteral("daisy-synthetic-kick");
        patch.blend = 0.14f;
        patch.frequency = 58.0f;
        patch.tone = 0.30f;
        patch.decay = 0.38f;
        patch.level = 0.64f;
        patch.transient = QStringLiteral("soft-beater");
        patch.transientLevel = 0.075f;
        patch.transientTone = 0.30f;
        patch.velocity.outputCurve = 1.20f;
    }

    // Listening review preferred the short, driven synthetic smack of the
    // Metallic Pulse snare to the shell-led Rock and Metal candidates. Keep
    // Daisy's synthetic snare in front and retain the acoustic shell as a
    // subordinate layer, with profile/candidate-specific tuning and room.
    const bool riffRockSnare =
        snare && profile.id == QStringLiteral("rock_riff_modal");
    const bool shuffleRockSnare =
        snare && profile.id == QStringLiteral("rock_shuffle_blues");
    const bool punkRockSnare =
        snare && profile.id == QStringLiteral("rock_punk_garage");
    const bool modernMetalSnare =
        snare &&
        profile.id == QStringLiteral("metal_modern_progressive");
    if (riffRockSnare || shuffleRockSnare ||
        punkRockSnare || modernMetalSnare) {
        patch.intendedIdentity =
            QStringLiteral(
                "hard synthetic-smack rock snare with shell reinforcement");
        patch.source = QStringLiteral("daisy-synthetic-snare");
        patch.secondSource = QStringLiteral("jam2-shell-snare");
        patch.transient = QStringLiteral("stick");
        patch.transientDecaySeconds = 0.006f;
        patch.texture = QStringLiteral("wire");
        patch.textureDecaySeconds = 0.065f;

        if (riffRockSnare) {
            const bool machine =
                palette == DrumPalette::ElectronicMetal;
            const bool open =
                palette == DrumPalette::OpenRock;
            patch.blend = machine ? 0.14f : open ? 0.30f : 0.22f;
            patch.frequency = machine ? 180.0f : open ? 170.0f : 180.0f;
            patch.decay = machine ? 0.050f : open ? 0.090f : 0.060f;
            patch.tone = open ? 0.30f : 0.27f;
            patch.colour = machine ? 0.72f : open ? 0.74f : 0.80f;
            patch.fmAmount = machine ? 0.20f : 0.16f;
            patch.level = 0.54f;
            patch.transientLevel =
                machine ? 0.38f : open ? 0.31f : 0.34f;
            patch.transientTone =
                machine ? 0.52f : open ? 0.47f : 0.49f;
            patch.textureLevel =
                machine ? 0.070f : open ? 0.090f : 0.075f;
            patch.roomSend =
                machine ? 0.012f : open ? 0.095f : 0.035f;
            patch.voiceDrive =
                machine ? 2.90f : open ? 2.35f : 2.55f;
            patch.reconstructionLowpassHz =
                machine ? 9500.0f : open ? 14200.0f : 13200.0f;
            patch.velocity.outputCurve = 1.10f;
        } else if (shuffleRockSnare) {
            const bool open =
                palette == DrumPalette::OpenRock;
            const bool dry =
                palette == DrumPalette::DryRock;
            patch.blend = open ? 0.34f : dry ? 0.25f : 0.30f;
            patch.frequency = open ? 164.0f : dry ? 170.0f : 168.0f;
            patch.decay = open ? 0.115f : dry ? 0.068f : 0.082f;
            patch.tone = open ? 0.30f : 0.28f;
            patch.colour = open ? 0.72f : dry ? 0.79f : 0.75f;
            patch.fmAmount = 0.15f;
            patch.level = 0.54f;
            patch.transientLevel = open ? 0.29f : dry ? 0.34f : 0.28f;
            patch.transientTone = open ? 0.46f : 0.49f;
            patch.textureLevel = open ? 0.095f : 0.080f;
            patch.roomSend = open ? 0.120f : dry ? 0.045f : 0.070f;
            patch.voiceDrive = open ? 2.25f : dry ? 2.45f : 2.20f;
            patch.reconstructionLowpassHz =
                open ? 14500.0f : dry ? 13500.0f : 13800.0f;
            patch.velocity.outputCurve = 1.12f;
        } else if (punkRockSnare) {
            const bool machine =
                palette == DrumPalette::ElectronicMetal;
            const bool dry =
                palette == DrumPalette::DryRock;
            patch.blend = machine ? 0.14f : dry ? 0.22f : 0.18f;
            patch.frequency = machine ? 190.0f : dry ? 188.0f : 190.0f;
            patch.decay = machine ? 0.043f : dry ? 0.058f : 0.052f;
            patch.tone = machine ? 0.34f : 0.32f;
            patch.colour = machine ? 0.74f : dry ? 0.82f : 0.72f;
            patch.fmAmount = machine ? 0.22f : 0.18f;
            patch.level = 0.55f;
            patch.transientLevel =
                machine ? 0.40f : dry ? 0.36f : 0.38f;
            patch.transientTone =
                machine ? 0.54f : dry ? 0.50f : 0.52f;
            patch.textureLevel = machine ? 0.065f : 0.075f;
            patch.roomSend =
                machine ? 0.010f : dry ? 0.045f : 0.070f;
            patch.voiceDrive = machine ? 3.00f : dry ? 2.65f : 2.80f;
            patch.reconstructionLowpassHz =
                machine ? 9000.0f : dry ? 10200.0f : 9800.0f;
            patch.velocity.outputCurve = 1.08f;
        } else {
            const bool electronic =
                palette == DrumPalette::ElectronicMetal;
            const bool organic =
                palette == DrumPalette::OrganicHeavy;
            patch.blend =
                electronic ? 0.12f : organic ? 0.24f : 0.16f;
            patch.frequency =
                electronic ? 178.0f : organic ? 172.0f : 182.0f;
            patch.decay =
                electronic ? 0.040f : organic ? 0.078f : 0.052f;
            patch.tone = electronic ? 0.30f : organic ? 0.27f : 0.29f;
            patch.colour =
                electronic ? 0.74f : organic ? 0.80f : 0.82f;
            patch.fmAmount =
                electronic ? 0.22f : organic ? 0.16f : 0.20f;
            patch.level = 0.56f;
            patch.transientLevel =
                electronic ? 0.42f : organic ? 0.34f : 0.40f;
            patch.transientTone =
                electronic ? 0.54f : organic ? 0.48f : 0.52f;
            patch.textureLevel =
                electronic ? 0.065f : organic ? 0.085f : 0.075f;
            patch.roomSend =
                electronic ? 0.010f : organic ? 0.075f : 0.040f;
            patch.voiceDrive =
                electronic ? 3.15f : organic ? 2.65f : 3.00f;
            patch.reconstructionLowpassHz =
                electronic ? 9000.0f : organic ? 13200.0f : 9500.0f;
            patch.velocity.outputCurve = 1.08f;
        }
    }

    // The shell-and-wire model supplies acoustic body and wire character, but
    // listening review showed it does not make a sufficiently complete snare
    // alone. Every researched snare therefore combines it with the Daisy
    // synthetic impact body. Existing deliberate blends, including the
    // harder Rock/Metal construction above, remain authoritative.
    if (snare &&
        patch.source == QStringLiteral("jam2-shell-snare") &&
        patch.secondSource == QStringLiteral("off")) {
        const bool softArticulation =
            patch.transient == QStringLiteral("brush") ||
            patch.transient == QStringLiteral("rim") ||
            patch.transient == QStringLiteral("soft-beater");
        const bool traditionalStyle =
            profile.styleId == QStringLiteral("jazz") ||
            profile.styleId == QStringLiteral("blues") ||
            profile.styleId == QStringLiteral("country") ||
            profile.styleId == QStringLiteral("rnb-soul") ||
            profile.styleId == QStringLiteral("reggae") ||
            profile.styleId == QStringLiteral("bossa-nova");
        patch.secondSource =
            QStringLiteral("daisy-synthetic-snare");
        patch.blend =
            softArticulation ? 0.14f :
            traditionalStyle ? 0.18f : 0.24f;
    } else if (
        snare &&
        patch.source == QStringLiteral("daisy-synthetic-snare") &&
        patch.secondSource == QStringLiteral("off")) {
        // Machine and industrial snares stay synthetic-led; a smaller shell
        // contribution adds the recognisable drum-body anchor.
        patch.secondSource = QStringLiteral("jam2-shell-snare");
        patch.blend =
            palette == DrumPalette::Industrial ? 0.10f :
            palette == DrumPalette::Trap808 ? 0.12f : 0.15f;
    }

    // Presence safeguards learned from the accepted Pop kit. These are floors
    // and caps rather than shared presets: tuning, envelopes, source families,
    // rooms and palette character remain specific to each researched kit.
    const bool gentleAcousticStyle =
        profile.styleId == QStringLiteral("jazz") ||
        profile.styleId == QStringLiteral("blues") ||
        profile.styleId == QStringLiteral("modal-jam") ||
        profile.styleId == QStringLiteral("rnb-soul") ||
        profile.styleId == QStringLiteral("reggae") ||
        profile.styleId == QStringLiteral("bossa-nova");
    const bool acceptedPopCandidate =
        profile.styleId == QStringLiteral("pop") &&
        candidate.palette == DrumPalette::HybridPop;
    if (kind == DrumKind::ClosedHat) {
        // Retain softer sticks in jazz-adjacent styles, but keep the hat above
        // the point where it disappears during isolated or groove audition.
        patch.level = std::max(
            patch.level,
            acceptedPopCandidate
                ? 0.36f
                : gentleAcousticStyle ? 0.46f : 0.48f);
        patch.transientLevel = std::max(
            patch.transientLevel,
            gentleAcousticStyle ? 0.090f : 0.105f);
        if (!acceptedPopCandidate) {
            // The earlier piece-level floor did not compensate for very short
            // Daisy hat envelopes and low style velocity bands. Keep a closed
            // articulation, but give it enough duration and semantic velocity
            // to remain audible beside the backing.
            patch.decay = std::max(patch.decay, 0.18f);
            patch.velocity.ghost.minimum =
                std::max(patch.velocity.ghost.minimum, 18);
            patch.velocity.ghost.maximum =
                std::max(patch.velocity.ghost.maximum, 44);
            patch.velocity.normal.minimum =
                std::max(patch.velocity.normal.minimum, 50);
            patch.velocity.normal.maximum =
                std::max(patch.velocity.normal.maximum, 96);
            patch.velocity.accent.minimum =
                std::max(patch.velocity.accent.minimum, 92);
            patch.velocity.accent.maximum =
                std::max(patch.velocity.accent.maximum, 120);
            patch.velocity.outputCurve = std::min(
                patch.velocity.outputCurve,
                1.0f);
        }
    } else if (kind == DrumKind::Ride) {
        // Colour drives the pitched normal/accent modes in the ride model.
        // Ride-led acoustic styles receive slightly more damping, while the
        // model itself supplies a smoother upper-partial decay for every kit.
        const bool rideLedStyle =
            profile.styleId == QStringLiteral("jazz") ||
            profile.styleId == QStringLiteral("blues") ||
            profile.styleId == QStringLiteral("reggae") ||
            profile.styleId == QStringLiteral("bossa-nova");
        patch.colour = std::min(
            patch.colour,
            rideLedStyle ? 0.15f : 0.17f);
        patch.transientTone = std::min(
            patch.transientTone,
            rideLedStyle ? 0.50f : 0.54f);
    } else if (kind == DrumKind::CrossStick) {
        // A wooden cross-stick must remain usable at ghost velocity without
        // forcing every style to share the same pitch, room or decay.
        patch.level = std::max(
            patch.level,
            gentleAcousticStyle ? 0.68f : 0.74f);
        patch.transientLevel = std::max(
            patch.transientLevel,
            gentleAcousticStyle ? 0.065f : 0.080f);
        patch.velocity.ghost.minimum =
            std::max(patch.velocity.ghost.minimum, 22);
        patch.velocity.ghost.maximum =
            std::max(patch.velocity.ghost.maximum, 48);
        patch.velocity.outputCurve = std::min(
            patch.velocity.outputCurve,
            gentleAcousticStyle ? 1.10f : 1.05f);
        // A high peak was masking extremely low time-integrated energy.
        // Lengthen the wooden modes and rim transient across every kit,
        // including the accepted Pop kit, instead of driving an already
        // peak-limited click harder.
        patch.decay = std::max(patch.decay, 0.22f);
        patch.transientDecaySeconds =
            std::max(patch.transientDecaySeconds, 0.008f);
        patch.velocity.normal.minimum =
            std::max(patch.velocity.normal.minimum, 48);
        patch.velocity.normal.maximum =
            std::max(patch.velocity.normal.maximum, 96);
        patch.velocity.accent.minimum =
            std::max(patch.velocity.accent.minimum, 92);
        patch.velocity.accent.maximum =
            std::max(patch.velocity.accent.maximum, 120);
        patch.velocity.outputCurve = std::min(
            patch.velocity.outputCurve,
            gentleAcousticStyle ? 0.95f : 1.0f);
    }

    if (acceptedPopCandidate) {
        if (kind == DrumKind::ClosedHat) {
            // Promote the former short open-hat character into a more audible
            // closed role. It remains choked by the shared hat group.
            patch.decay = 0.352f;
            patch.level = 0.36f;
            patch.transientLevel = 0.120f;
            patch.textureLevel = 0.032f;
            patch.textureDecaySeconds = 0.34f;
            patch.roomSend = 0.060f;
        } else if (kind == DrumKind::OpenHat) {
            // Same pair of metal sources, but enough envelope and plate air to
            // read as deliberately open rather than a second closed hat.
            patch.tone = 0.59f;
            patch.decay = 0.70f;
            patch.level = 0.31f;
            patch.transientLevel = 0.11f;
            patch.textureLevel = 0.052f;
            patch.textureDecaySeconds = 0.62f;
            patch.roomSend = 0.072f;
        } else if (isTomKind(kind)) {
            patch.tone = 0.30f;
            patch.fmAmount = 0.24f;
            patch.transientLevel *= 0.78f;
            patch.roomSend = 0.095f;
        } else if (kind == DrumKind::Ride) {
            // Preserve the ghost articulation while easing the pitched modes
            // of normal and accent hits by roughly ten percent.
            patch.colour = 0.17f;
        } else if (kind == DrumKind::CrossStick) {
            patch.level = 0.80f;
            patch.transientLevel = 0.140f;
            patch.velocity.ghost = {26, 50};
            patch.velocity.outputCurve = 0.85f;
        } else if (kind == DrumKind::HandPercussion) {
            patch.intendedIdentity =
                QStringLiteral("short layered Pop hand clap");
            patch.source = QStringLiteral("jam2-hand-clap");
            patch.secondSource = QStringLiteral("off");
            patch.blend = 0.0f;
            patch.frequency = 2100.0f;
            patch.tone = 0.48f;
            patch.decay = 0.12f;
            patch.colour = 0.42f;
            patch.fmAmount = 0.0f;
            patch.level = 0.32f;
            patch.transient = QStringLiteral("off");
            patch.transientLevel = 0.0f;
            patch.texture = QStringLiteral("off");
            patch.textureLevel = 0.0f;
            patch.roomSend = 0.075f;
        }
    }

    const bool nativeReggaeFocus =
        profile.id == QStringLiteral("reggae_roots") &&
        candidate.palette == DrumPalette::ReggaeDub;
    if (nativeReggaeFocus) {
        if (kind == DrumKind::Crash) {
            // Retain the accepted crash from the separate rhythm-box
            // candidate without replacing that complete alternative.
            ResearchKitCandidate warmCrash = candidate;
            warmCrash.id =
                QStringLiteral("warm-rhythm-box-roots-crash");
            warmCrash.name =
                QStringLiteral("Warm Rhythm-Box Roots crash");
            warmCrash.palette = DrumPalette::RhythmBox;
            warmCrash.recommended = false;
            warmCrash.variant = 1;
            return candidatePiecePatch(
                profile, warmCrash, DrumKind::Crash);
        }
        // Direct listening preferred the current Jam2 voices to any of the
        // researched Reggae constructions. Level must equal the default patch
        // so the native-lane renderer applies unity gain.
        const DrumLabPatch nativeDefault =
            defaultDrumLabPatch(profile, kind);
        patch.source = QStringLiteral("jam2-native");
        patch.secondSource = QStringLiteral("off");
        patch.blend = 0.0f;
        patch.level = nativeDefault.level;
        patch.transient = QStringLiteral("off");
        patch.transientLevel = 0.0f;
        patch.texture = QStringLiteral("off");
        patch.textureLevel = 0.0f;
        patch.synth.source = QStringLiteral("off");
        patch.synth.level = 0.0f;
        patch.roomSend = 0.0f;
    }

    const float variantOffset =
        1.0f + 0.025f * candidate.variant;
    patch.frequency = std::clamp(
        patch.frequency * variantOffset,
        20.0f,
        12000.0f);
    // Cymbal voices need enough envelope length for their evolving partials
    // to speak. Profile and palette scaling may shorten them, but never to an
    // implausible gated tick.
    if (kind == DrumKind::Crash) {
        patch.decay = std::max(patch.decay, 0.28f);
    } else if (kind == DrumKind::Ride) {
        patch.decay = std::max(patch.decay, 0.26f);
    }
    patch.level = std::clamp(patch.level, 0.0f, 1.5f);
    patch.tone = std::clamp(patch.tone, 0.0f, 1.0f);
    patch.decay = std::clamp(patch.decay, 0.001f, 1.0f);
    patch.transientLevel =
        std::clamp(patch.transientLevel, 0.0f, 1.5f);
    patch.textureLevel =
        std::clamp(patch.textureLevel, 0.0f, 1.5f);
    patch.roomSend = std::clamp(patch.roomSend, 0.0f, 1.0f);
    return patch;
}

DrumBusDesign candidateBusDesign(
    const ProfileDefinition& profile,
    const ResearchKitCandidate& candidate)
{
    DrumBusDesign bus = drumBusDesign(profile);
    switch (candidate.palette) {
    case DrumPalette::BrushJazz:
    case DrumPalette::BopJazz:
    case DrumPalette::Bossa:
        bus.drive = 1.02;
        bus.compressorRatio = 1.45;
        bus.compressorThreshold = 0.17;
        bus.roomMix = 0.12;
        bus.roomSizeMs = 42.0;
        bus.roomDamping = 0.68;
        break;
    case DrumPalette::Trap808:
        bus.drive = 1.20;
        bus.cutoffHz = 15800.0;
        bus.compressorRatio = 1.55;
        bus.compressorThreshold = 0.20;
        bus.roomMix = 0.025;
        bus.roomSizeMs = 18.0;
        break;
    case DrumPalette::House909:
        bus.drive = 1.48;
        bus.compressorRatio = 2.2;
        bus.roomMix = 0.055;
        bus.roomSizeMs = 24.0;
        break;
    case DrumPalette::Techno:
    case DrumPalette::Industrial:
        bus.drive = 1.88;
        bus.cutoffHz = 12600.0;
        bus.compressorRatio = 2.0;
        bus.roomMix = 0.04;
        bus.roomSizeMs = 21.0;
        bus.roomDamping = 0.46;
        break;
    case DrumPalette::Break12:
    case DrumPalette::Boom12:
    case DrumPalette::Lofi:
    case DrumPalette::SoulDamped:
        bus.drive = 1.34;
        bus.cutoffHz =
            candidate.palette == DrumPalette::SoulDamped
                ? 7600.0 : 9800.0;
        bus.compressorRatio = 2.8;
        bus.roomMix = 0.065;
        bus.roomSizeMs = 27.0;
        bus.roomDamping = 0.72;
        break;
    case DrumPalette::MetalLayered:
    case DrumPalette::OrganicHeavy:
        bus.drive = 1.56;
        bus.compressorRatio = 2.15;
        bus.compressorThreshold = 0.18;
        bus.roomMix = 0.075;
        bus.roomSizeMs = 32.0;
        bus.roomDamping = 0.48;
        break;
    case DrumPalette::Atmosphere:
        bus.drive = 1.04;
        bus.compressorRatio = 1.35;
        bus.compressorThreshold = 0.20;
        bus.roomMix = 0.22;
        bus.roomSizeMs = 78.0;
        bus.roomDamping = 0.76;
        break;
    default:
        break;
    }
    if (profile.id == QStringLiteral("reggae_roots") &&
        candidate.palette == DrumPalette::ReggaeDub) {
        // Preserve the native Jam2 body and dynamics. The selected warm crash
        // still passes through this very light common output stage so the
        // complete kit remains coherent.
        bus.drive = 1.0;
        bus.cutoffHz = 20000.0;
        bus.compressorRatio = 1.0;
        bus.compressorThreshold = 1.0;
        bus.roomMix = 0.0;
    }
    return bus;
}

QJsonObject drumLabPatchParameters(const DrumLabPatch& patch)
{
    const auto velocityBand = [](const DrumLabPatch::VelocityBand& band) {
        return QJsonObject{
            {QStringLiteral("minimum"), band.minimum},
            {QStringLiteral("maximum"), band.maximum},
        };
    };
    return {
        {QStringLiteral("intendedIdentity"),
         patch.intendedIdentity},
        {QStringLiteral("source"), patch.source},
        {QStringLiteral("secondSource"), patch.secondSource},
        {QStringLiteral("blend"), patch.blend},
        {QStringLiteral("frequencyHz"), patch.frequency},
        {QStringLiteral("tone"), patch.tone},
        {QStringLiteral("decay"), patch.decay},
        {QStringLiteral("colour"), patch.colour},
        {QStringLiteral("fmAmount"), patch.fmAmount},
        {QStringLiteral("level"), patch.level},
        {QStringLiteral("transient"), QJsonObject{
             {QStringLiteral("type"), patch.transient},
             {QStringLiteral("level"), patch.transientLevel},
             {QStringLiteral("tone"), patch.transientTone},
             {QStringLiteral("decaySeconds"),
              patch.transientDecaySeconds},
         }},
        {QStringLiteral("texture"), QJsonObject{
             {QStringLiteral("type"), patch.texture},
             {QStringLiteral("level"), patch.textureLevel},
             {QStringLiteral("tone"), patch.textureTone},
             {QStringLiteral("decaySeconds"),
              patch.textureDecaySeconds},
             {QStringLiteral("density"), patch.textureDensity},
         }},
        {QStringLiteral("colourStage"), QJsonObject{
             {QStringLiteral("voiceDrive"), patch.voiceDrive},
             {QStringLiteral("sampleRateHz"),
              patch.digitalSampleRateHz},
             {QStringLiteral("bitDepth"), patch.digitalBitDepth},
             {QStringLiteral("reconstructionLowpassHz"),
              patch.reconstructionLowpassHz},
             {QStringLiteral("dynamicFilterAmount"),
              patch.dynamicFilterAmount},
         }},
        {QStringLiteral("roomSend"), patch.roomSend},
        {QStringLiteral("relationship"), QJsonObject{
             {QStringLiteral("chokeGroup"), patch.chokeGroup},
             {QStringLiteral("chokeSeconds"), patch.chokeSeconds},
         }},
        {QStringLiteral("velocity"), QJsonObject{
             {QStringLiteral("ghost"),
              velocityBand(patch.velocity.ghost)},
             {QStringLiteral("normal"),
              velocityBand(patch.velocity.normal)},
             {QStringLiteral("accent"),
              velocityBand(patch.velocity.accent)},
             {QStringLiteral("excitationCurve"),
              patch.velocity.excitationCurve},
             {QStringLiteral("outputCurve"),
              patch.velocity.outputCurve},
             {QStringLiteral("brightnessAmount"),
              patch.velocity.brightnessAmount},
             {QStringLiteral("decayAmount"),
              patch.velocity.decayAmount},
             {QStringLiteral("driveAmount"),
              patch.velocity.driveAmount},
         }},
        {QStringLiteral("synthLayer"), QJsonObject{
             {QStringLiteral("source"), patch.synth.source},
             {QStringLiteral("midiNote"), patch.synth.midiNote},
             {QStringLiteral("level"), patch.synth.level},
             {QStringLiteral("gateSeconds"), patch.synth.gateSeconds},
             {QStringLiteral("attackSeconds"), patch.synth.attack},
             {QStringLiteral("decaySeconds"), patch.synth.decay},
             {QStringLiteral("sustain"), patch.synth.sustain},
             {QStringLiteral("releaseSeconds"), patch.synth.release},
             {QStringLiteral("noiseMix"), patch.synth.noiseMix},
             {QStringLiteral("filterCutoffHz"),
              patch.synth.filterCutoff},
         }},
    };
}

bool candidateSourceSupportsKind(
    DrumKind kind,
    const QString& source)
{
    // The native renderer isolates the corresponding Jam2 lane, so it
    // preserves the requested piece identity without a generic synth model.
    if (source == QStringLiteral("jam2-native")) return true;
    switch (kind) {
    case DrumKind::Kick:
        return source == QStringLiteral("daisy-analog-kick") ||
            source == QStringLiteral("daisy-synthetic-kick");
    case DrumKind::Snare:
        return source == QStringLiteral("jam2-shell-snare") ||
            source == QStringLiteral("daisy-analog-snare") ||
            source == QStringLiteral("daisy-synthetic-snare");
    case DrumKind::ClosedHat:
    case DrumKind::OpenHat:
        return source == QStringLiteral("daisy-metal") ||
            source == QStringLiteral("daisy-ring-metal");
    case DrumKind::HighTom:
    case DrumKind::MidTom:
    case DrumKind::FloorTom:
        return source == QStringLiteral("jam2-shell-tom") ||
            source == QStringLiteral("daisy-synthetic-kick");
    case DrumKind::Crash:
        return source ==
            QStringLiteral("jam2-crash-cymbal");
    case DrumKind::Ride:
        return source ==
            QStringLiteral("jam2-ride-cymbal");
    case DrumKind::CrossStick:
        return source ==
            QStringLiteral("jam2-cross-stick");
    case DrumKind::Shaker:
        return source == QStringLiteral("jam2-shaker") ||
            source == QStringLiteral("jam2-tambourine");
    case DrumKind::HandPercussion:
        return source == QStringLiteral("jam2-hand-drum") ||
            source == QStringLiteral("jam2-hand-clap") ||
            source == QStringLiteral("jam2-wood-block") ||
            source == QStringLiteral("jam2-tambourine") ||
            source == QStringLiteral("jam2-shaker") ||
            source == QStringLiteral("daisy-synthetic-kick");
    }
    return false;
}

QJsonObject drumKitParameters(
    const ProfileDefinition& profile,
    const ResearchKitCandidate& candidate)
{
    QJsonObject pieces;
    std::array<DrumLabPatch, kDrumKinds.size()> patches;
    for (std::size_t index = 0;
         index < kDrumKinds.size();
         ++index) {
        const DrumKind kind = kDrumKinds[index];
        DrumLabPatch patch =
            candidatePiecePatch(profile, candidate, kind);
        if (patch.intendedIdentity.trimmed().isEmpty()) {
            throw std::runtime_error(
                "Candidate drum piece has no intended identity.");
        }
        if (!candidateSourceSupportsKind(kind, patch.source) ||
            (patch.secondSource != QStringLiteral("off") &&
             !candidateSourceSupportsKind(
                 kind, patch.secondSource))) {
            throw std::runtime_error(
                QStringLiteral(
                    "Candidate source violates the %1 identity contract.")
                    .arg(drumKindId(kind))
                    .toStdString());
        }
        patches[index] = patch;
        pieces.insert(
            drumKindId(kind),
            drumLabPatchParameters(patch));
    }
    const DrumLabPatch& crash =
        patches[static_cast<std::size_t>(DrumKind::Crash)];
    const DrumLabPatch& ride =
        patches[static_cast<std::size_t>(DrumKind::Ride)];
    if (crash.source == ride.source) {
        throw std::runtime_error(
            "Candidate crash and ride share one primary model.");
    }
    const DrumBusDesign bus =
        candidateBusDesign(profile, candidate);
    const PaletteKitCharacter paletteCharacter =
        paletteKitCharacter(candidate.palette);
    const ProfileKitCharacter& profileCharacter =
        profileKitCharacter(profile);
    return {
        {QStringLiteral("candidateId"), candidate.id},
        {QStringLiteral("candidateName"), candidate.name},
        {QStringLiteral("recommended"), candidate.recommended},
        {QStringLiteral("description"), candidate.description},
        {QStringLiteral("researchFamily"),
         paletteId(candidate.palette)},
        {QStringLiteral("sourceReferences"),
         QJsonArray::fromStringList(
             paletteReferences(candidate.palette))},
        {QStringLiteral("kitCharacter"), QJsonObject{
             {QStringLiteral("paletteLowPitchScale"),
              paletteCharacter.lowPitchScale},
             {QStringLiteral("paletteShellPitchScale"),
              paletteCharacter.shellPitchScale},
             {QStringLiteral("paletteShellTailScale"),
              paletteCharacter.shellTailScale},
             {QStringLiteral("paletteMetalToneOffset"),
              paletteCharacter.metalToneOffset},
             {QStringLiteral("paletteMetalTailScale"),
              paletteCharacter.metalTailScale},
             {QStringLiteral("paletteTransientScale"),
              paletteCharacter.transientScale},
             {QStringLiteral("paletteRoomScale"),
              paletteCharacter.roomScale},
             {QStringLiteral("profilePitchScale"),
              profileCharacter.pitchScale},
             {QStringLiteral("profileTailScale"),
              profileCharacter.tailScale},
             {QStringLiteral("profileMetalBrightness"),
              profileCharacter.metalBrightness},
             {QStringLiteral("profileTransientScale"),
              profileCharacter.transientScale},
             {QStringLiteral("profileRoomScale"),
              profileCharacter.roomScale},
         }},
        {QStringLiteral("pieces"), pieces},
        {QStringLiteral("bus"), QJsonObject{
             {QStringLiteral("drive"), bus.drive},
             {QStringLiteral("lowpassHz"), bus.cutoffHz},
             {QStringLiteral("compressorThreshold"),
              bus.compressorThreshold},
             {QStringLiteral("compressorRatio"),
              bus.compressorRatio},
             {QStringLiteral("compressorReleaseMs"),
              bus.compressorReleaseMs},
             {QStringLiteral("roomMix"), bus.roomMix},
             {QStringLiteral("roomSizeMs"), bus.roomSizeMs},
             {QStringLiteral("roomDamping"), bus.roomDamping},
         }},
    };
}

QJsonObject drumKitParameters(const ProfileDefinition& profile)
{
    const std::vector<ResearchKitCandidate> candidates =
        researchKitCandidates(profile);
    const auto recommended = std::find_if(
        candidates.begin(),
        candidates.end(),
        [](const ResearchKitCandidate& candidate) {
            return candidate.recommended;
        });
    return drumKitParameters(
        profile,
        recommended != candidates.end()
            ? *recommended : candidates.front());
}

QJsonArray drumKitCandidatesJson(const ProfileDefinition& profile)
{
    QJsonArray result;
    for (const ResearchKitCandidate& candidate :
         researchKitCandidates(profile)) {
        result.append(QJsonObject{
            {QStringLiteral("id"), candidate.id},
            {QStringLiteral("name"), candidate.name},
            {QStringLiteral("recommended"), candidate.recommended},
            {QStringLiteral("description"), candidate.description},
            {QStringLiteral("researchFamily"),
             paletteId(candidate.palette)},
            {QStringLiteral("sourceReferences"),
             QJsonArray::fromStringList(
                 paletteReferences(candidate.palette))},
            {QStringLiteral("parameters"),
             drumKitParameters(profile, candidate)},
        });
    }
    return result;
}

bool isSupportedDrumSource(const QString& source)
{
    return source == QStringLiteral("off") ||
        source == QStringLiteral("jam2-native") ||
        source == QStringLiteral("daisy-profile") ||
        source == QStringLiteral("daisy-analog-kick") ||
        source == QStringLiteral("daisy-synthetic-kick") ||
        source == QStringLiteral("daisy-analog-snare") ||
        source == QStringLiteral("daisy-synthetic-snare") ||
        source == QStringLiteral("jam2-shell-snare") ||
        source == QStringLiteral("daisy-metal") ||
        source == QStringLiteral("daisy-ring-metal") ||
        source == QStringLiteral("daisy-modal") ||
        source == QStringLiteral("daisy-particle") ||
        source == QStringLiteral("jam2-shell-tom") ||
        source == QStringLiteral("jam2-cross-stick") ||
        source == QStringLiteral("jam2-shaker") ||
        source == QStringLiteral("jam2-hand-drum") ||
        source == QStringLiteral("jam2-hand-clap") ||
        source == QStringLiteral("jam2-wood-block") ||
        source == QStringLiteral("jam2-tambourine") ||
        source == QStringLiteral("jam2-crash-cymbal") ||
        source == QStringLiteral("jam2-ride-cymbal") ||
        source == QStringLiteral("daisy-cymbal");
}

bool isSupportedTransient(const QString& value)
{
    return value == QStringLiteral("off") ||
        value == QStringLiteral("soft-beater") ||
        value == QStringLiteral("hard-beater") ||
        value == QStringLiteral("stick") ||
        value == QStringLiteral("head-strike") ||
        value == QStringLiteral("rim") ||
        value == QStringLiteral("click") ||
        value == QStringLiteral("brush") ||
        value == QStringLiteral("clap");
}

bool isSupportedTexture(const QString& value)
{
    return value == QStringLiteral("off") ||
        value == QStringLiteral("wire") ||
        value == QStringLiteral("dust") ||
        value == QStringLiteral("particle") ||
        value == QStringLiteral("air") ||
        value == QStringLiteral("metal-wash");
}

float boundedPatchNumber(
    const QJsonObject& values,
    const QString& name,
    float fallback,
    float minimum,
    float maximum);

int boundedPatchInteger(
    const QJsonObject& values,
    const QString& name,
    int fallback,
    int minimum,
    int maximum);

DrumLabPatch drumLabPatchFromJson(
    const QJsonObject& values,
    DrumLabPatch patch)
{
    patch.intendedIdentity =
        values.value(QStringLiteral("intendedIdentity"))
            .toString(patch.intendedIdentity)
            .left(96);
    const QString source =
        values.value(QStringLiteral("source")).toString(patch.source);
    const QString secondSource =
        values.value(QStringLiteral("secondSource"))
            .toString(patch.secondSource);
    if (!isSupportedDrumSource(source) ||
        source == QStringLiteral("off") ||
        !isSupportedDrumSource(secondSource)) {
        throw std::runtime_error(
            "Unsupported Drum Kit Lab source architecture.");
    }
    patch.source = source;
    patch.secondSource = secondSource;
    patch.blend = boundedPatchNumber(
        values, QStringLiteral("blend"),
        patch.blend, 0.0f, 1.0f);
    patch.frequency = boundedPatchNumber(
        values, QStringLiteral("frequencyHz"),
        patch.frequency, 20.0f, 12000.0f);
    patch.tone = boundedPatchNumber(
        values, QStringLiteral("tone"),
        patch.tone, 0.0f, 1.0f);
    patch.decay = boundedPatchNumber(
        values, QStringLiteral("decay"),
        patch.decay, 0.001f, 1.0f);
    patch.colour = boundedPatchNumber(
        values, QStringLiteral("colour"),
        patch.colour, 0.0f, 1.0f);
    patch.fmAmount = boundedPatchNumber(
        values, QStringLiteral("fmAmount"),
        patch.fmAmount, 0.0f, 1.0f);
    patch.level = boundedPatchNumber(
        values, QStringLiteral("level"),
        patch.level, 0.0f, 1.5f);
    const QJsonObject transient =
        values.value(QStringLiteral("transient")).toObject();
    const QString transientType =
        transient.value(QStringLiteral("type"))
            .toString(patch.transient);
    if (!isSupportedTransient(transientType)) {
        throw std::runtime_error(
            "Unsupported Drum Kit Lab transient type.");
    }
    patch.transient = transientType;
    patch.transientLevel = boundedPatchNumber(
        transient, QStringLiteral("level"),
        patch.transientLevel, 0.0f, 1.5f);
    patch.transientTone = boundedPatchNumber(
        transient, QStringLiteral("tone"),
        patch.transientTone, 0.0f, 1.0f);
    patch.transientDecaySeconds = boundedPatchNumber(
        transient, QStringLiteral("decaySeconds"),
        patch.transientDecaySeconds, 0.001f, 0.25f);
    const QJsonObject texture =
        values.value(QStringLiteral("texture")).toObject();
    const QString textureType =
        texture.value(QStringLiteral("type"))
            .toString(patch.texture);
    if (!isSupportedTexture(textureType)) {
        throw std::runtime_error(
            "Unsupported Drum Kit Lab texture type.");
    }
    patch.texture = textureType;
    patch.textureLevel = boundedPatchNumber(
        texture, QStringLiteral("level"),
        patch.textureLevel, 0.0f, 1.5f);
    patch.textureTone = boundedPatchNumber(
        texture, QStringLiteral("tone"),
        patch.textureTone, 0.0f, 1.0f);
    patch.textureDecaySeconds = boundedPatchNumber(
        texture, QStringLiteral("decaySeconds"),
        patch.textureDecaySeconds, 0.003f, 4.0f);
    patch.textureDensity = boundedPatchNumber(
        texture, QStringLiteral("density"),
        patch.textureDensity, 0.0f, 1.0f);
    const QJsonObject colour =
        values.value(QStringLiteral("colourStage")).toObject();
    patch.voiceDrive = boundedPatchNumber(
        colour, QStringLiteral("voiceDrive"),
        patch.voiceDrive, 0.5f, 12.0f);
    patch.digitalSampleRateHz = boundedPatchNumber(
        colour, QStringLiteral("sampleRateHz"),
        patch.digitalSampleRateHz, 1000.0f, 48000.0f);
    patch.digitalBitDepth = boundedPatchInteger(
        colour, QStringLiteral("bitDepth"),
        patch.digitalBitDepth, 4, 24);
    patch.reconstructionLowpassHz = boundedPatchNumber(
        colour, QStringLiteral("reconstructionLowpassHz"),
        patch.reconstructionLowpassHz, 200.0f, 20000.0f);
    patch.dynamicFilterAmount = boundedPatchNumber(
        colour, QStringLiteral("dynamicFilterAmount"),
        patch.dynamicFilterAmount, 0.0f, 1.0f);
    patch.roomSend = boundedPatchNumber(
        values, QStringLiteral("roomSend"),
        patch.roomSend, 0.0f, 1.0f);
    const QJsonObject relationship =
        values.value(QStringLiteral("relationship")).toObject();
    patch.chokeGroup =
        relationship.value(QStringLiteral("chokeGroup"))
            .toString(patch.chokeGroup)
            .left(32);
    patch.chokeSeconds = boundedPatchNumber(
        relationship, QStringLiteral("chokeSeconds"),
        patch.chokeSeconds, 0.001f, 0.25f);
    const QJsonObject velocity =
        values.value(QStringLiteral("velocity")).toObject();
    const auto readBand = [&velocity](
        const QString& name,
        DrumLabPatch::VelocityBand fallback) {
        const QJsonObject band = velocity.value(name).toObject();
        DrumLabPatch::VelocityBand result;
        result.minimum = boundedPatchInteger(
            band, QStringLiteral("minimum"),
            fallback.minimum, 1, 127);
        result.maximum = boundedPatchInteger(
            band, QStringLiteral("maximum"),
            fallback.maximum, result.minimum, 127);
        return result;
    };
    patch.velocity.ghost =
        readBand(QStringLiteral("ghost"), patch.velocity.ghost);
    patch.velocity.normal =
        readBand(QStringLiteral("normal"), patch.velocity.normal);
    patch.velocity.accent =
        readBand(QStringLiteral("accent"), patch.velocity.accent);
    patch.velocity.excitationCurve = boundedPatchNumber(
        velocity, QStringLiteral("excitationCurve"),
        patch.velocity.excitationCurve, 0.2f, 3.0f);
    patch.velocity.outputCurve = boundedPatchNumber(
        velocity, QStringLiteral("outputCurve"),
        patch.velocity.outputCurve, 0.2f, 3.0f);
    patch.velocity.brightnessAmount = boundedPatchNumber(
        velocity, QStringLiteral("brightnessAmount"),
        patch.velocity.brightnessAmount, 0.0f, 1.0f);
    patch.velocity.decayAmount = boundedPatchNumber(
        velocity, QStringLiteral("decayAmount"),
        patch.velocity.decayAmount, -0.8f, 0.8f);
    patch.velocity.driveAmount = boundedPatchNumber(
        velocity, QStringLiteral("driveAmount"),
        patch.velocity.driveAmount, 0.0f, 1.5f);
    const QJsonValue synthValue =
        values.value(QStringLiteral("synthLayer"));
    if (!synthValue.isUndefined()) {
        if (!synthValue.isObject()) {
            throw std::runtime_error(
                "Drum synth layer must be an object.");
        }
        const QJsonObject synth = synthValue.toObject();
        const QString synthSource =
            synth.value(QStringLiteral("source"))
                .toString(patch.synth.source);
        if (synthSource != QStringLiteral("off")) {
            const std::optional<SourceKind> parsed =
                sourceKindFromName(synthSource);
            if (!parsed || *parsed == SourceKind::Jam2Native) {
                throw std::runtime_error(
                    "Unsupported Drum Kit Lab synth architecture.");
            }
        }
        patch.synth.source = synthSource;
        patch.synth.midiNote = boundedPatchInteger(
            synth, QStringLiteral("midiNote"),
            patch.synth.midiNote, 24, 96);
        patch.synth.level = boundedPatchNumber(
            synth, QStringLiteral("level"),
            patch.synth.level, 0.0f, 1.5f);
        patch.synth.gateSeconds = boundedPatchNumber(
            synth, QStringLiteral("gateSeconds"),
            patch.synth.gateSeconds, 0.005f, 2.0f);
        patch.synth.attack = boundedPatchNumber(
            synth, QStringLiteral("attackSeconds"),
            patch.synth.attack, 0.001f, 0.25f);
        patch.synth.decay = boundedPatchNumber(
            synth, QStringLiteral("decaySeconds"),
            patch.synth.decay, 0.005f, 2.0f);
        patch.synth.sustain = boundedPatchNumber(
            synth, QStringLiteral("sustain"),
            patch.synth.sustain, 0.01f, 1.0f);
        patch.synth.release = boundedPatchNumber(
            synth, QStringLiteral("releaseSeconds"),
            patch.synth.release, 0.005f, 3.0f);
        patch.synth.noiseMix = boundedPatchNumber(
            synth, QStringLiteral("noiseMix"),
            patch.synth.noiseMix, 0.0f, 1.0f);
        patch.synth.filterCutoff = boundedPatchNumber(
            synth, QStringLiteral("filterCutoffHz"),
            patch.synth.filterCutoff, 40.0f, 18000.0f);
    }
    return patch;
}

QString drumStrengthId(DrumHit::Strength strength)
{
    switch (strength) {
    case DrumHit::Strength::Ghost:
        return QStringLiteral("ghost");
    case DrumHit::Strength::Normal:
        return QStringLiteral("normal");
    case DrumHit::Strength::Accent:
        return QStringLiteral("accent");
    }
    return QStringLiteral("normal");
}

DrumHit realiseDrumHit(
    DrumHit hit,
    const ProfileDefinition& profile,
    const QString& candidateId,
    const DrumLabPatch& patch)
{
    const DrumLabPatch::VelocityBand band =
        hit.strength == DrumHit::Strength::Ghost
            ? patch.velocity.ghost
            : hit.strength == DrumHit::Strength::Accent
                ? patch.velocity.accent
                : patch.velocity.normal;
    const QString identity =
        profile.id + QLatin1Char('/') + candidateId +
        QLatin1Char('/') + drumKindId(hit.kind) +
        QLatin1Char('/') + QString::number(hit.frame) +
        QLatin1Char('/') + QString::number(hit.repeatIndex);
    std::uint32_t state = stableSeed(identity);
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const float randomUnit =
        static_cast<float>(state & 0x00ffffffU) /
        static_cast<float>(0x00ffffffU);
    const float unit =
        hit.strength == DrumHit::Strength::Ghost
            ? 0.05f + 0.75f * randomUnit
            : hit.strength == DrumHit::Strength::Accent
                ? 0.25f + 0.75f * std::sqrt(randomUnit)
                : 0.15f + 0.70f * randomUnit;
    if (hit.exactPerformanceVelocity) {
        hit.midiVelocity =
            std::clamp(hit.midiVelocity, 1, 127);
    } else {
        hit.midiVelocity = std::clamp(
            band.minimum +
                static_cast<int>(std::lround(
                    unit * (band.maximum - band.minimum))),
            1,
            127);
    }
    const float velocity =
        static_cast<float>(hit.midiVelocity) / 127.0f;
    hit.excitation = std::clamp(
        0.04f + 0.96f * std::pow(
            velocity, patch.velocity.excitationCurve),
        0.0f,
        1.0f);
    hit.outputGain = std::clamp(
        0.025f + 0.975f * std::pow(
            velocity, patch.velocity.outputCurve),
        0.0f,
        1.0f);
    const float centered = velocity - 0.55f;
    hit.brightnessOffset =
        centered * patch.velocity.brightnessAmount;
    hit.decayScale = std::clamp(
        1.0f + centered * patch.velocity.decayAmount,
        0.35f,
        1.8f);
    hit.driveScale = std::clamp(
        1.0f + centered * patch.velocity.driveAmount,
        0.5f,
        2.0f);
    hit.level = hit.outputGain;
    return hit;
}

ActiveDrum::Transient transientKind(const QString& value)
{
    if (value == QStringLiteral("soft-beater")) {
        return ActiveDrum::Transient::SoftBeater;
    }
    if (value == QStringLiteral("hard-beater")) {
        return ActiveDrum::Transient::HardBeater;
    }
    if (value == QStringLiteral("stick")) {
        return ActiveDrum::Transient::Stick;
    }
    if (value == QStringLiteral("head-strike")) {
        return ActiveDrum::Transient::HeadStrike;
    }
    if (value == QStringLiteral("rim")) {
        return ActiveDrum::Transient::Rim;
    }
    if (value == QStringLiteral("click")) {
        return ActiveDrum::Transient::Click;
    }
    if (value == QStringLiteral("brush")) {
        return ActiveDrum::Transient::Brush;
    }
    if (value == QStringLiteral("clap")) {
        return ActiveDrum::Transient::Clap;
    }
    return ActiveDrum::Transient::Off;
}

ActiveDrum::Texture textureKind(const QString& value)
{
    if (value == QStringLiteral("wire")) {
        return ActiveDrum::Texture::Wire;
    }
    if (value == QStringLiteral("dust")) {
        return ActiveDrum::Texture::Dust;
    }
    if (value == QStringLiteral("particle")) {
        return ActiveDrum::Texture::Particle;
    }
    if (value == QStringLiteral("air")) {
        return ActiveDrum::Texture::Air;
    }
    if (value == QStringLiteral("metal-wash")) {
        return ActiveDrum::Texture::MetalWash;
    }
    return ActiveDrum::Texture::Off;
}

void configureDrumComponents(
    ActiveDrum& voice,
    const DrumLabPatch& patch,
    const DrumHit& hit)
{
    voice.transient = transientKind(patch.transient);
    voice.texture = textureKind(patch.texture);
    voice.transientLevel =
        patch.transientLevel *
        (0.50f + 0.50f * hit.excitation);
    voice.transientTone = std::clamp(
        patch.transientTone + hit.brightnessOffset,
        0.0f,
        1.0f);
    voice.transientEnvelope = hit.excitation;
    voice.transientDecay = std::exp(
        -6.907755f /
        std::max(
            1.0f,
            patch.transientDecaySeconds *
                hit.decayScale * kSampleRate));
    voice.textureLevel =
        patch.textureLevel *
        (0.62f + 0.38f * hit.excitation);
    voice.textureTone = std::clamp(
        patch.textureTone + 0.65f * hit.brightnessOffset,
        0.0f,
        1.0f);
    voice.textureEnvelope =
        0.38f + 0.62f * hit.excitation;
    voice.textureDecay = std::exp(
        -6.907755f /
        std::max(
            1.0f,
            patch.textureDecaySeconds *
                hit.decayScale * kSampleRate));
    voice.textureDensity = patch.textureDensity;
    voice.transientFilter = std::make_unique<daisysp::Svf>();
    voice.transientFilter->Init(kSampleRate);
    const float transientFilterHz =
        voice.transient == ActiveDrum::Transient::SoftBeater
            ? 420.0f + 1500.0f * voice.transientTone
            : voice.transient == ActiveDrum::Transient::HardBeater
                ? 850.0f + 4400.0f * voice.transientTone
                : voice.transient == ActiveDrum::Transient::HeadStrike
                    ? 620.0f + 2600.0f * voice.transientTone
                : voice.transient == ActiveDrum::Transient::Rim
                    ? 900.0f + 4800.0f * voice.transientTone
                    : 1600.0f + 8200.0f * voice.transientTone;
    voice.transientFilter->SetFreq(transientFilterHz);
    voice.transientFilter->SetRes(0.18f);
    voice.textureFilter = std::make_unique<daisysp::Svf>();
    voice.textureFilter->Init(kSampleRate);
    voice.textureFilter->SetFreq(
        450.0f + 12500.0f * voice.textureTone);
    voice.textureFilter->SetRes(
        0.18f + 0.55f * patch.textureDensity);
    if (voice.texture == ActiveDrum::Texture::Particle) {
        voice.textureParticle =
            std::make_unique<daisysp::Particle>();
        voice.textureParticle->Init(kSampleRate);
        voice.textureParticle->SetFreq(
            650.0f + 8200.0f * voice.textureTone);
        voice.textureParticle->SetResonance(
            0.22f + 0.65f * patch.textureDensity);
        voice.textureParticle->SetDensity(
            0.02f + 0.70f * patch.textureDensity);
        voice.textureParticle->SetGain(0.8f);
        voice.textureParticle->SetSpread(
            0.20f + 1.8f * patch.textureDensity);
        voice.textureParticle->SetRandomFreq(
            4.0f + 36.0f * patch.textureDensity);
    }
    voice.drive = patch.voiceDrive * hit.driveScale;
    voice.digitalRate = std::clamp(
        patch.digitalSampleRateHz / kSampleRate,
        0.001f,
        1.0f);
    voice.quantizationLevels = std::pow(
        2.0f,
        static_cast<float>(
            std::max(1, patch.digitalBitDepth - 1)));
    voice.reconstructionCoefficient = static_cast<float>(
        1.0 -
        std::exp(
            -2.0 * kPi *
            std::clamp(
                patch.reconstructionLowpassHz,
                200.0f,
                20000.0f) /
            kSampleRate));
    voice.dynamicFilterAmount = patch.dynamicFilterAmount;
    voice.roomSend = patch.roomSend;
    voice.chokeGroup = patch.chokeGroup;
    voice.chokeDecay = 1.0f;
    voice.noiseState ^=
        stableSeed(
            QString::number(hit.frame) +
            drumKindId(hit.kind));
}

ActiveDrum makeConfiguredDaisyDrum(
    const ProfileDefinition& profile,
    const DrumHit& hit,
    std::size_t frames,
    const DrumLabPatch& patch,
    const QString& source,
    float layerGain)
{
    ActiveDrum voice;
    if (source == QStringLiteral("daisy-profile")) {
        voice = makeDrum(profile, hit, frames);
    } else if (source == QStringLiteral("daisy-analog-kick")) {
        voice.model = DaisyDrumModel::AnalogKick;
        voice.analogKick =
            std::make_unique<daisysp::AnalogBassDrum>();
        voice.analogKick->Init(kSampleRate);
    } else if (source == QStringLiteral("daisy-synthetic-kick")) {
        voice.model = DaisyDrumModel::SyntheticKick;
        voice.syntheticKick =
            std::make_unique<daisysp::SyntheticBassDrum>();
        voice.syntheticKick->Init(kSampleRate);
    } else if (source == QStringLiteral("daisy-analog-snare")) {
        voice.model = DaisyDrumModel::AnalogSnare;
        voice.analogSnare =
            std::make_unique<daisysp::AnalogSnareDrum>();
        voice.analogSnare->Init(kSampleRate);
    } else if (source == QStringLiteral("daisy-synthetic-snare")) {
        voice.model = DaisyDrumModel::SyntheticSnare;
        voice.syntheticSnare =
            std::make_unique<daisysp::SyntheticSnareDrum>();
        voice.syntheticSnare->Init(kSampleRate);
    } else if (source == QStringLiteral("jam2-shell-snare")) {
        voice.model = DaisyDrumModel::ShellSnare;
    } else if (source == QStringLiteral("daisy-metal")) {
        voice.model = DaisyDrumModel::Hat;
        voice.hat = std::make_unique<daisysp::HiHat<>>();
        voice.hat->Init(kSampleRate);
    } else if (source == QStringLiteral("daisy-ring-metal")) {
        voice.model = DaisyDrumModel::RingHat;
        voice.ringHat = std::make_unique<
            daisysp::HiHat<daisysp::RingModNoise>>();
        voice.ringHat->Init(kSampleRate);
    } else if (source == QStringLiteral("daisy-modal")) {
        voice.model = DaisyDrumModel::Modal;
        voice.modal = std::make_unique<daisysp::ModalVoice>();
        voice.modal->Init(kSampleRate);
    } else if (source == QStringLiteral("daisy-particle")) {
        voice.model = DaisyDrumModel::Particle;
        voice.particle = std::make_unique<daisysp::Particle>();
        voice.particle->Init(kSampleRate);
    } else if (source == QStringLiteral("jam2-shell-tom")) {
        voice.model = DaisyDrumModel::ShellTom;
    } else if (source == QStringLiteral("jam2-cross-stick")) {
        voice.model = DaisyDrumModel::RimWood;
    } else if (source == QStringLiteral("jam2-shaker")) {
        voice.model = DaisyDrumModel::CollisionShaker;
    } else if (source == QStringLiteral("jam2-hand-drum")) {
        voice.model = DaisyDrumModel::SkinHandDrum;
    } else if (source == QStringLiteral("jam2-hand-clap")) {
        voice.model = DaisyDrumModel::HandClap;
    } else if (source == QStringLiteral("jam2-wood-block")) {
        voice.model = DaisyDrumModel::WoodBlock;
    } else if (source == QStringLiteral("jam2-tambourine")) {
        voice.model = DaisyDrumModel::Tambourine;
    } else if (source == QStringLiteral("jam2-crash-cymbal")) {
        voice.model = DaisyDrumModel::CrashCymbal;
    } else if (source == QStringLiteral("jam2-ride-cymbal")) {
        voice.model = DaisyDrumModel::RideCymbal;
    } else if (source == QStringLiteral("daisy-cymbal")) {
        voice.model = DaisyDrumModel::Crash;
        voice.crashA = std::make_unique<daisysp::Oscillator>();
        voice.crashB = std::make_unique<daisysp::Oscillator>();
        voice.crashC = std::make_unique<daisysp::Oscillator>();
        voice.crashFilter = std::make_unique<daisysp::Svf>();
        for (daisysp::Oscillator* oscillator :
             {voice.crashA.get(),
              voice.crashB.get(),
              voice.crashC.get()}) {
            oscillator->Init(kSampleRate);
            oscillator->SetAmp(1.0f);
            oscillator->SetWaveform(
                daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
        }
        voice.crashA->SetFreq(patch.frequency * 0.79f);
        voice.crashB->SetFreq(patch.frequency * 1.10f);
        voice.crashC->SetFreq(patch.frequency * 1.63f);
        voice.crashFilter->Init(kSampleRate);
    } else {
        throw std::runtime_error("Invalid Daisy drum source.");
    }

    const auto initialiseIdentityFilter =
        [](std::unique_ptr<daisysp::Svf>& filter,
           float frequency,
           float resonance) {
            filter = std::make_unique<daisysp::Svf>();
            filter->Init(kSampleRate);
            filter->SetFreq(std::clamp(
                frequency, 80.0f, 19000.0f));
            filter->SetRes(std::clamp(
                resonance, 0.05f, 0.92f));
        };
    const auto setIdentityMode =
        [&voice, &hit](
            int index,
            float frequency,
            float gain,
            float decaySeconds) {
            const std::uint32_t variation =
                0x9e3779b9U *
                    static_cast<std::uint32_t>(
                        1 + hit.repeatIndex) ^
                0x85ebca6bU *
                    static_cast<std::uint32_t>(index + 3);
            const float unit =
                static_cast<float>(variation & 0xffffU) /
                65535.0f;
            const float signedUnit = 2.0f * unit - 1.0f;
            voice.identityModeCount =
                std::max(voice.identityModeCount, index + 1);
            voice.identityFrequency[index] =
                std::clamp(
                    frequency * (1.0f + 0.0025f * signedUnit),
                    20.0f,
                    19000.0f);
            voice.identityPhase[index] =
                0.012f + 0.16f * unit;
            voice.identityGain[index] =
                gain * (0.97f + 0.06f * unit);
            voice.identityEnvelope[index] =
                0.42f + 0.58f * hit.excitation;
            voice.identityDecay[index] = std::exp(
                -6.907755f /
                std::max(
                    1.0f,
                    decaySeconds * hit.decayScale *
                        kSampleRate));
        };
    const auto setIdentityBandDecay =
        [&voice, &hit](int index, float decaySeconds) {
            voice.identityBandEnvelope[index] =
                0.48f + 0.52f * hit.excitation;
            voice.identityBandDecay[index] = std::exp(
                -6.907755f /
                std::max(
                    1.0f,
                    decaySeconds * hit.decayScale *
                        kSampleRate));
        };

    switch (voice.model) {
    case DaisyDrumModel::AnalogKick:
        voice.analogKick->SetFreq(patch.frequency);
        voice.analogKick->SetAccent(hit.excitation);
        voice.analogKick->SetTone(std::clamp(
            patch.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.analogKick->SetDecay(std::clamp(
            patch.decay * hit.decayScale, 0.001f, 1.0f));
        voice.analogKick->SetAttackFmAmount(patch.fmAmount);
        voice.analogKick->SetSelfFmAmount(patch.colour);
        break;
    case DaisyDrumModel::SyntheticKick:
        voice.syntheticKick->SetFreq(patch.frequency);
        voice.syntheticKick->SetAccent(hit.excitation);
        voice.syntheticKick->SetTone(std::clamp(
            patch.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.syntheticKick->SetDecay(std::clamp(
            patch.decay * hit.decayScale, 0.001f, 1.0f));
        voice.syntheticKick->SetDirtiness(patch.colour);
        voice.syntheticKick->SetFmEnvelopeAmount(patch.fmAmount);
        voice.syntheticKick->SetFmEnvelopeDecay(
            std::clamp(0.08f + 0.45f * patch.decay, 0.0f, 1.0f));
        break;
    case DaisyDrumModel::AnalogSnare:
        voice.analogSnare->SetFreq(patch.frequency);
        voice.analogSnare->SetAccent(hit.excitation);
        voice.analogSnare->SetTone(std::clamp(
            patch.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.analogSnare->SetDecay(std::clamp(
            patch.decay * hit.decayScale, 0.001f, 1.0f));
        voice.analogSnare->SetSnappy(patch.colour);
        break;
    case DaisyDrumModel::SyntheticSnare:
        voice.syntheticSnare->SetFreq(patch.frequency);
        voice.syntheticSnare->SetAccent(hit.excitation);
        voice.syntheticSnare->SetFmAmount(patch.fmAmount);
        voice.syntheticSnare->SetDecay(std::clamp(
            patch.decay * hit.decayScale, 0.001f, 1.0f));
        voice.syntheticSnare->SetSnappy(patch.colour);
        break;
    case DaisyDrumModel::ShellSnare: {
        const float shellSeconds =
            0.045f + 0.30f * patch.decay;
        setIdentityMode(
            0, patch.frequency, 0.72f, shellSeconds);
        setIdentityMode(
            1, patch.frequency * 1.79f,
            0.12f + 0.10f * patch.tone,
            shellSeconds * 0.58f);
        setIdentityMode(
            2, patch.frequency * 2.34f,
            0.035f + 0.055f * patch.tone,
            shellSeconds * 0.37f);
        initialiseIdentityFilter(
            voice.identityFilterA,
            720.0f + 2100.0f * patch.tone,
            0.28f);
        voice.identityNoiseEnvelope =
            0.30f + 0.30f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.008f + 0.022f * patch.tone) *
                    kSampleRate));
        break;
    }
    case DaisyDrumModel::Hat:
        voice.hat->SetFreq(patch.frequency);
        voice.hat->SetAccent(hit.excitation);
        voice.hat->SetTone(std::clamp(
            patch.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.hat->SetDecay(std::clamp(
            patch.decay * hit.decayScale, 0.001f, 1.0f));
        voice.hat->SetNoisiness(patch.colour);
        break;
    case DaisyDrumModel::RingHat:
        voice.ringHat->SetFreq(patch.frequency);
        voice.ringHat->SetAccent(hit.excitation);
        voice.ringHat->SetTone(std::clamp(
            patch.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.ringHat->SetDecay(std::clamp(
            patch.decay * hit.decayScale, 0.001f, 1.0f));
        voice.ringHat->SetNoisiness(patch.colour);
        break;
    case DaisyDrumModel::Modal:
        voice.modal->SetFreq(patch.frequency);
        voice.modal->SetAccent(hit.excitation);
        voice.modal->SetStructure(patch.fmAmount);
        voice.modal->SetBrightness(std::clamp(
            patch.tone + hit.brightnessOffset, 0.0f, 1.0f));
        voice.modal->SetDamping(std::clamp(
            1.0f - patch.decay * hit.decayScale,
            0.0f,
            1.0f));
        break;
    case DaisyDrumModel::Particle:
        voice.particle->SetFreq(patch.frequency);
        voice.particle->SetResonance(
            0.18f + 0.72f * patch.tone);
        voice.particle->SetDensity(
            0.01f + 0.86f * patch.colour);
        voice.particle->SetGain(
            0.35f + 0.65f * hit.excitation);
        voice.particle->SetSpread(
            0.10f + 2.2f * patch.fmAmount);
        voice.particle->SetRandomFreq(
            2.0f + 48.0f * patch.colour);
        voice.textureEnvelope =
            0.38f + 0.62f * hit.excitation;
        voice.textureDecay = std::exp(
            -1.0f /
            std::max(
                1.0f,
                (0.01f + 2.5f * patch.decay) *
                    hit.decayScale * kSampleRate));
        break;
    case DaisyDrumModel::ShellTom: {
        const float shellSeconds =
            0.075f + 0.82f * patch.decay;
        setIdentityMode(
            0, patch.frequency, 0.72f, shellSeconds);
        setIdentityMode(
            1, patch.frequency * 1.031f,
            0.10f,
            shellSeconds * 0.76f);
        setIdentityMode(
            2, patch.frequency * 1.593f,
            0.12f + 0.16f * patch.tone,
            shellSeconds * 0.62f);
        setIdentityMode(
            3, patch.frequency * 2.136f,
            0.045f + 0.075f * patch.tone,
            shellSeconds * 0.38f);
        setIdentityMode(
            4, patch.frequency * 2.296f,
            0.020f + 0.040f * patch.tone,
            shellSeconds * 0.29f);
        initialiseIdentityFilter(
            voice.identityFilterA,
            900.0f + 3900.0f * patch.tone,
            0.34f);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.006f + 0.024f * patch.tone) *
                    kSampleRate));
        voice.identityPitchEnvelope = hit.excitation;
        voice.identityPitchSweep =
            0.018f + 0.085f * patch.fmAmount;
        voice.identityPitchDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.012f + 0.040f * patch.decay) *
                    kSampleRate));
        break;
    }
    case DaisyDrumModel::RimWood: {
        const float clickSeconds =
            0.007f + 0.055f * patch.decay;
        setIdentityMode(
            0, patch.frequency, 0.52f, clickSeconds);
        setIdentityMode(
            1, patch.frequency * 2.37f,
            0.31f, clickSeconds * 0.70f);
        setIdentityMode(
            2, patch.frequency * 3.91f,
            0.17f, clickSeconds * 0.48f);
        initialiseIdentityFilter(
            voice.identityFilterA,
            2100.0f + 6200.0f * patch.tone,
            0.28f);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(1.0f, 0.0045f * kSampleRate));
        break;
    }
    case DaisyDrumModel::CollisionShaker:
        initialiseIdentityFilter(
            voice.identityFilterA,
            patch.frequency * 0.72f,
            0.30f);
        initialiseIdentityFilter(
            voice.identityFilterB,
            patch.frequency * 1.24f,
            0.24f);
        voice.identityNoiseEnvelope =
            0.45f + 0.55f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.025f + 0.48f * patch.decay) *
                    hit.decayScale * kSampleRate));
        voice.identityCollisionRate =
            900.0f + 5200.0f * patch.colour;
        voice.identityCollisionDecay =
            0.82f + 0.16f * patch.tone;
        break;
    case DaisyDrumModel::SkinHandDrum: {
        const float skinSeconds =
            0.045f + 0.72f * patch.decay;
        setIdentityMode(
            0, patch.frequency, 0.86f, skinSeconds);
        setIdentityMode(
            1, patch.frequency * 1.47f,
            0.08f + 0.14f * patch.tone,
            skinSeconds * 0.54f);
        setIdentityMode(
            2, patch.frequency * 2.09f,
            0.03f + 0.08f * patch.tone,
            skinSeconds * 0.34f);
        initialiseIdentityFilter(
            voice.identityFilterA,
            1100.0f + 4200.0f * patch.tone,
            0.31f);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.005f + 0.020f * patch.tone) *
                    kSampleRate));
        voice.identityPitchEnvelope = hit.excitation;
        voice.identityPitchSweep =
            0.025f + 0.12f * patch.fmAmount;
        voice.identityPitchDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.010f + 0.025f * patch.decay) *
                    kSampleRate));
        break;
    }
    case DaisyDrumModel::HandClap:
        initialiseIdentityFilter(
            voice.identityFilterA,
            900.0f + 1200.0f * patch.tone,
            0.30f);
        initialiseIdentityFilter(
            voice.identityFilterB,
            2600.0f + 3600.0f * patch.tone,
            0.22f);
        voice.identityNoiseEnvelope =
            0.42f + 0.58f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.050f + 0.62f * patch.decay) *
                    hit.decayScale * kSampleRate));
        break;
    case DaisyDrumModel::WoodBlock: {
        const float blockSeconds =
            0.018f + 0.12f * patch.decay;
        setIdentityMode(
            0, patch.frequency, 0.64f, blockSeconds);
        setIdentityMode(
            1, patch.frequency * 1.71f,
            0.25f, blockSeconds * 0.72f);
        setIdentityMode(
            2, patch.frequency * 2.64f,
            0.11f, blockSeconds * 0.48f);
        initialiseIdentityFilter(
            voice.identityFilterA,
            3200.0f + 4600.0f * patch.tone,
            0.22f);
        voice.identityNoiseEnvelope = hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(1.0f, 0.0035f * kSampleRate));
        break;
    }
    case DaisyDrumModel::Tambourine: {
        const std::array<float, 5> ratios{
            0.73f, 1.0f, 1.31f, 1.79f, 2.41f};
        for (int index = 0;
             index < static_cast<int>(ratios.size());
             ++index) {
            setIdentityMode(
                index,
                patch.frequency * ratios[index],
                0.055f,
                0.035f + 0.16f * patch.decay *
                    (1.0f - 0.09f * index));
        }
        initialiseIdentityFilter(
            voice.identityFilterA,
            patch.frequency * 0.78f,
            0.26f);
        initialiseIdentityFilter(
            voice.identityFilterB,
            patch.frequency * 1.42f,
            0.20f);
        voice.identityNoiseEnvelope =
            0.42f + 0.58f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (0.06f + 0.62f * patch.decay) *
                    hit.decayScale * kSampleRate));
        voice.identityCollisionRate =
            1500.0f + 6200.0f * patch.colour;
        voice.identityCollisionDecay =
            0.86f + 0.12f * patch.tone;
        break;
    }
    case DaisyDrumModel::CrashCymbal: {
        const float centre = patch.frequency;
        initialiseIdentityFilter(
            voice.identityFilterA, centre * 0.38f, 0.20f);
        initialiseIdentityFilter(
            voice.identityFilterB, centre * 1.08f, 0.17f);
        initialiseIdentityFilter(
            voice.identityFilterC, centre * 2.38f, 0.12f);
        const std::array<float, 7> ratios{
            0.61f, 0.93f, 1.28f, 1.67f,
            2.19f, 2.83f, 3.71f};
        for (int index = 0;
             index < static_cast<int>(ratios.size());
             ++index) {
            setIdentityMode(
                index,
                centre * ratios[index],
                0.016f + 0.006f * patch.colour,
                0.22f + patch.decay *
                    (0.72f + 0.08f * index));
        }
        voice.identityNoiseEnvelope =
            0.30f + 0.42f * hit.excitation;
        voice.identityNoiseDecay = std::exp(
            -6.907755f /
            std::max(
                1.0f,
                (1.05f + 4.0f * patch.decay) *
                    hit.decayScale * kSampleRate));
        setIdentityBandDecay(
            0, 0.82f + 3.8f * patch.decay);
        setIdentityBandDecay(
            1, 0.56f + 2.8f * patch.decay);
        setIdentityBandDecay(
            2, 0.24f + 1.40f * patch.decay);
        break;
    }
    case DaisyDrumModel::RideCymbal: {
        const float centre = patch.frequency;
        const bool ghost =
            hit.strength == DrumHit::Strength::Ghost;
        const bool accent =
            hit.strength == DrumHit::Strength::Accent;
        const float pitchedRingScale =
            ghost
                ? 1.0f
                : std::clamp(
                    0.70f + 1.20f * patch.colour,
                    0.65f,
                    1.20f);
        const float strikePosition =
            accent ? 1.22f : ghost ? 0.82f : 1.0f;
        initialiseIdentityFilter(
            voice.identityFilterA,
            centre * (ghost ? 0.58f : 0.76f),
            0.18f);
        initialiseIdentityFilter(
            voice.identityFilterB,
            centre * (accent ? 1.42f : 1.18f),
            0.15f);
        initialiseIdentityFilter(
            voice.identityFilterC,
            centre * (accent ? 2.46f : 2.08f),
            0.10f);
        const std::array<float, 5> ratios{
            0.92f, 1.37f, 1.89f, 2.62f, 3.54f};
        constexpr std::array<float, 5> modeGainRollOff{
            1.0f, 0.78f, 0.54f, 0.34f, 0.20f};
        constexpr std::array<float, 5> modeTailRollOff{
            1.0f, 0.82f, 0.65f, 0.50f, 0.38f};
        for (int index = 0;
             index < static_cast<int>(ratios.size());
             ++index) {
            const float primaryGain =
                accent ? 0.23f : ghost ? 0.055f : 0.11f;
            const float overtoneGain =
                accent ? 0.050f : ghost ? 0.014f : 0.028f;
            const float articulationTail =
                index == 0
                    ? (accent ? 0.34f : ghost ? 0.055f : 0.34f)
                    : (accent ? 0.21f : ghost ? 0.040f : 0.19f);
            const float modeDecay =
                articulationTail +
                patch.decay *
                    ((index == 0 ? 0.40f : 0.44f) +
                     (accent && index == 0 ? 0.08f : 0.0f));
            setIdentityMode(
                index,
                centre * ratios[index] * strikePosition,
                (index == 0 ? primaryGain : overtoneGain) *
                    pitchedRingScale *
                    modeGainRollOff[index],
                modeDecay * modeTailRollOff[index]);
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
                     patch.decay) *
                    hit.decayScale * kSampleRate));
        setIdentityBandDecay(
            0,
            (ghost ? 0.16f : accent ? 0.58f : 0.48f) +
                (ghost ? 0.65f : accent ? 1.85f : 1.50f) *
                    patch.decay);
        setIdentityBandDecay(
            1,
            (ghost ? 0.10f : accent ? 0.38f : 0.30f) +
                (ghost ? 0.42f : accent ? 1.20f : 0.98f) *
                    patch.decay);
        setIdentityBandDecay(
            2,
            (ghost ? 0.055f : accent ? 0.19f : 0.15f) +
                (ghost ? 0.24f : accent ? 0.64f : 0.52f) *
                    patch.decay);
        break;
    }
    case DaisyDrumModel::Crash:
        voice.crashFilter->SetFreq(patch.frequency);
        voice.crashFilter->SetRes(
            std::clamp(0.05f + 0.35f * patch.tone, 0.0f, 0.95f));
        voice.crashFilter->SetDrive(0.08f);
        voice.crashDecay = static_cast<float>(
            std::exp(
                -1.0 /
                ((0.08 + 2.8 * patch.decay) * kSampleRate)));
        voice.crashMetalMix = patch.colour;
        voice.noiseState ^=
            stableSeed(profile.id) ^
            static_cast<std::uint32_t>(hit.frame);
        break;
    }
    configureDrumComponents(voice, patch, hit);
    voice.gain = hit.outputGain * patch.level * layerGain;
    float tailSeconds = 0.22f + 3.0f * patch.decay;
    switch (voice.model) {
    case DaisyDrumModel::AnalogSnare:
    case DaisyDrumModel::SyntheticSnare:
    case DaisyDrumModel::ShellSnare:
        tailSeconds = 0.10f + 1.15f * patch.decay;
        break;
    case DaisyDrumModel::ShellTom:
        tailSeconds = 0.18f + 1.10f * patch.decay;
        break;
    case DaisyDrumModel::RimWood:
    case DaisyDrumModel::WoodBlock:
        tailSeconds = 0.055f + 0.18f * patch.decay;
        break;
    case DaisyDrumModel::CollisionShaker:
        tailSeconds = 0.08f + 0.62f * patch.decay;
        break;
    case DaisyDrumModel::SkinHandDrum:
        tailSeconds = 0.12f + 0.90f * patch.decay;
        break;
    case DaisyDrumModel::HandClap:
        tailSeconds = 0.12f + 0.82f * patch.decay;
        break;
    case DaisyDrumModel::Tambourine:
        tailSeconds = 0.12f + 0.86f * patch.decay;
        break;
    case DaisyDrumModel::CrashCymbal:
        tailSeconds = 0.85f + 4.4f * patch.decay;
        break;
    case DaisyDrumModel::RideCymbal:
        tailSeconds =
            hit.strength == DrumHit::Strength::Accent
                ? 0.95f + 3.2f * patch.decay
                : hit.strength == DrumHit::Strength::Ghost
                    ? 0.38f + 1.0f * patch.decay
                    : 0.68f + 2.1f * patch.decay;
        break;
    default:
        break;
    }
    if (voice.transient != ActiveDrum::Transient::Off) {
        tailSeconds = std::max(
            tailSeconds,
            0.04f + 1.15f * patch.transientDecaySeconds);
    }
    if (voice.texture != ActiveDrum::Texture::Off) {
        tailSeconds = std::max(
            tailSeconds,
            0.04f + 1.15f * patch.textureDecaySeconds);
    }
    voice.endAge = std::max(
        1,
        static_cast<int>(tailSeconds * kSampleRate));
    const int fadeSamples = std::min(
        voice.endAge / 4,
        static_cast<int>(
            (voice.model == DaisyDrumModel::CrashCymbal ||
             voice.model == DaisyDrumModel::RideCymbal
                 ? 0.080f
                 : 0.025f) *
            kSampleRate));
    voice.fadeStartAge =
        std::max(0, voice.endAge - fadeSamples);
    voice.end = std::min<qint64>(
        static_cast<qint64>(frames),
        hit.frame +
            static_cast<qint64>(
                tailSeconds * kSampleRate));
    return voice;
}

GeneratedPracticeIdea isolateDrumLane(
    GeneratedPracticeIdea idea,
    DrumKind kind)
{
    const bool preservePerformance =
        usesProductionDrumPerformance(idea);
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const int wanted = lanes.indexOf(laneNameForDrumKind(kind));
    for (auto& pattern : idea.beatSection.beatPatterns) {
        for (int lane = 0; lane < pattern.lanes.size(); ++lane) {
            if (lane != wanted) {
                pattern.lanes[lane] =
                    QString(std::max(1, pattern.division),
                            QLatin1Char('.'));
            }
        }
    }
    if (preservePerformance) {
        QString wantedId = drumKindId(kind);
        wantedId.replace(QLatin1Char('-'), QLatin1Char('_'));
        idea.recipe.drumEvents.erase(
            std::remove_if(
                idea.recipe.drumEvents.begin(),
                idea.recipe.drumEvents.end(),
                [&wantedId](const auto& event) {
                    return event.laneId != wantedId;
                }),
            idea.recipe.drumEvents.end());
    } else {
        // One-shot, velocity and repeat auditions deliberately replace the
        // generated display grid. Do not revalidate the now-stale production
        // event list against that new grid; let the native renderer consume
        // the explicit audition pattern instead.
        idea.recipe.drumEvents.clear();
    }
    idea.recipe.beatFingerprint =
        jam2::practice::generatedBeatFingerprint(
            idea.beatSection);
    idea.chordSection.generatedRecipe = idea.recipe;
    idea.beatSection.generatedRecipe = idea.recipe;
    return idea;
}

bool ensureDrumAuditionHit(
    GeneratedPracticeIdea& idea,
    DrumKind kind,
    std::size_t frames)
{
    const std::vector<DrumHit> existing =
        extractDrumHits(idea, frames);
    if (std::any_of(
            existing.begin(),
            existing.end(),
            [kind](const DrumHit& hit) {
                return hit.kind == kind;
            })) {
        return false;
    }
    if (idea.beatSection.beatPatterns.isEmpty()) {
        throw std::runtime_error(
            "Generated profile has no beat available for a drum audition.");
    }
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const int lane = lanes.indexOf(laneNameForDrumKind(kind));
    if (lane < 0) {
        throw std::runtime_error(
            "Selected drum has no Jam2 beat lane.");
    }
    auto& pattern = idea.beatSection.beatPatterns[0];
    pattern.division = std::max(1, pattern.division);
    if (pattern.lanes.size() <= lane) {
        pattern.lanes.resize(lane + 1);
    }
    QString states(pattern.division, QLatin1Char('.'));
    states[0] = QLatin1Char('x');
    pattern.lanes[lane] = states;
    return true;
}

void prepareDrumOneShot(
    GeneratedPracticeIdea& idea,
    DrumKind kind)
{
    if (idea.beatSection.beatPatterns.isEmpty()) {
        throw std::runtime_error(
            "Generated profile has no beat available for a drum one-shot.");
    }
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const int wanted = lanes.indexOf(laneNameForDrumKind(kind));
    if (wanted < 0) {
        throw std::runtime_error(
            "Selected drum has no Jam2 beat lane.");
    }
    idea.beatSection.beats = 1;
    idea.beatSection.beatPatterns.resize(1);
    auto& pattern = idea.beatSection.beatPatterns[0];
    pattern.division = 1;
    pattern.lanes.resize(lanes.size());
    for (int lane = 0; lane < pattern.lanes.size(); ++lane) {
        pattern.lanes[lane] = QStringLiteral(".");
    }
    pattern.lanes[wanted] = QStringLiteral("x");
}

void prepareDrumVelocityLadder(
    GeneratedPracticeIdea& idea,
    DrumKind kind)
{
    if (idea.beatSection.beatPatterns.isEmpty()) {
        throw std::runtime_error(
            "Generated profile has no beat available for a velocity ladder.");
    }
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const int wanted = lanes.indexOf(laneNameForDrumKind(kind));
    if (wanted < 0) {
        throw std::runtime_error(
            "Selected drum has no Jam2 beat lane.");
    }
    const BeatPattern prototype =
        idea.beatSection.beatPatterns.first();
    idea.beatSection.beats = 4;
    idea.beatSection.beatPatterns.resize(4);
    const std::array<QChar, 4> strengths{
        QLatin1Char('g'),
        QLatin1Char('x'),
        QLatin1Char('a'),
        QLatin1Char('x'),
    };
    for (int beat = 0; beat < 4; ++beat) {
        auto& pattern = idea.beatSection.beatPatterns[beat];
        pattern = prototype;
        pattern.division = 1;
        pattern.lanes.resize(lanes.size());
        for (QString& lane : pattern.lanes) {
            lane = QStringLiteral(".");
        }
        pattern.lanes[wanted] = QString(strengths[beat]);
    }
}

void prepareDrumRepeatedHits(
    GeneratedPracticeIdea& idea,
    DrumKind kind)
{
    if (idea.beatSection.beatPatterns.isEmpty()) {
        throw std::runtime_error(
            "Generated profile has no beat available for repeated hits.");
    }
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const int wanted = lanes.indexOf(laneNameForDrumKind(kind));
    if (wanted < 0) {
        throw std::runtime_error(
            "Selected drum has no Jam2 beat lane.");
    }
    const BeatPattern prototype =
        idea.beatSection.beatPatterns.first();
    idea.beatSection.beats = 2;
    idea.beatSection.beatPatterns.resize(2);
    for (int beat = 0; beat < 2; ++beat) {
        auto& pattern = idea.beatSection.beatPatterns[beat];
        pattern = prototype;
        pattern.division = 8;
        pattern.lanes.resize(lanes.size());
        for (QString& lane : pattern.lanes) {
            lane = QStringLiteral("........");
        }
        pattern.lanes[wanted] =
            beat == 0
                ? QStringLiteral("xxxxxxxx")
                : QStringLiteral("gxgxaxgx");
    }
}

std::vector<float> renderJam2DrumLane(
    const GeneratedPracticeIdea& sourceIdea,
    DrumKind kind,
    const QString& workspace)
{
    const GeneratedPracticeIdea idea =
        isolateDrumLane(sourceIdea, kind);
    ReferenceRenderSettings settings;
    settings.renderChords = false;
    settings.renderDrums = true;
    settings.renderMelody = false;
    settings.renderBass = false;
    settings.renderSupport = false;
    settings.sampleRate = kSampleRate;
    settings.bpm = idea.bpm;
    settings.meterNumerator = idea.meterNumerator;
    settings.meterDenominator = idea.meterDenominator;
    settings.tempoPulseUnits = idea.tempoPulseUnits;
    const auto result = jam2::practice::renderPracticeReferences(
        &idea.chordSection,
        &idea.beatSection,
        settings,
        workspace);
    if (!result.error.isEmpty()) {
        throw std::runtime_error(
            QStringLiteral("Jam2 drum render failed: %1")
                .arg(result.error)
                .toStdString());
    }
    std::vector<float> audio = readMonoPcm16(result.drums.path);
    QFile::remove(result.drums.path);
    return audio;
}

std::vector<float> renderLaboratoryKit(
    const GeneratedPracticeIdea& idea,
    const ProfileDefinition& profile,
    std::size_t frames,
    const QMap<DrumKind, DrumLabPatch>& kit,
    std::optional<DrumKind> onlyKind,
    const DrumBusDesign& bus,
    const QString& jam2Workspace,
    const QString& candidateId)
{
    // The accepted Daisy drum models use rand() for excitation noise. Keep
    // both Lab auditions and the promoted production renderer deterministic.
    std::srand(0x4a324452);
    const std::optional<DrumKind> selectedTom =
        onlyKind && isTomKind(*onlyKind)
            ? onlyKind
            : std::nullopt;
    std::vector<DrumHit> hits =
        extractDrumHits(idea, frames, selectedTom);
    if (onlyKind) {
        std::erase_if(hits, [onlyKind](const DrumHit& hit) {
            return hit.kind != *onlyKind;
        });
    }
    ResearchDrumKit sharedKit;
    sharedKit.profileId = profile.id;
    sharedKit.id = candidateId;
    sharedKit.name = candidateId;
    sharedKit.bus.drive = static_cast<float>(bus.drive);
    sharedKit.bus.lowpassHz =
        static_cast<float>(bus.cutoffHz);
    sharedKit.bus.compressorThreshold =
        static_cast<float>(bus.compressorThreshold);
    sharedKit.bus.compressorRatio =
        static_cast<float>(bus.compressorRatio);
    sharedKit.bus.compressorReleaseMs =
        static_cast<float>(bus.compressorReleaseMs);
    sharedKit.bus.roomMix =
        static_cast<float>(bus.roomMix);
    sharedKit.bus.roomSizeMs =
        static_cast<float>(bus.roomSizeMs);
    sharedKit.bus.roomDamping =
        static_cast<float>(bus.roomDamping);
    for (DrumKind kind : kDrumKinds) {
        const DrumLabPatch patch =
            kit.value(
                kind,
                defaultDrumLabPatch(profile, kind));
        ResearchDrumPiece piece;
        piece.intendedIdentity = patch.intendedIdentity;
        piece.source = patch.source;
        piece.secondSource = patch.secondSource;
        piece.blend = patch.blend;
        piece.frequencyHz = patch.frequency;
        piece.decay = patch.decay;
        piece.tone = patch.tone;
        piece.colour = patch.colour;
        piece.fmAmount = patch.fmAmount;
        piece.level = patch.level;
        piece.voiceDrive = patch.voiceDrive;
        piece.digitalSampleRateHz =
            patch.digitalSampleRateHz;
        piece.digitalBitDepth = patch.digitalBitDepth;
        piece.reconstructionLowpassHz =
            patch.reconstructionLowpassHz;
        piece.dynamicFilterAmount =
            patch.dynamicFilterAmount;
        piece.roomSend = patch.roomSend;
        piece.transientType = patch.transient;
        piece.transientLevel = patch.transientLevel;
        piece.transientTone = patch.transientTone;
        piece.transientDecaySeconds =
            patch.transientDecaySeconds;
        piece.textureType = patch.texture;
        piece.textureLevel = patch.textureLevel;
        piece.textureTone = patch.textureTone;
        piece.textureDensity = patch.textureDensity;
        piece.textureDecaySeconds =
            patch.textureDecaySeconds;
        piece.ghost = {
            patch.velocity.ghost.minimum,
            patch.velocity.ghost.maximum};
        piece.normal = {
            patch.velocity.normal.minimum,
            patch.velocity.normal.maximum};
        piece.accent = {
            patch.velocity.accent.minimum,
            patch.velocity.accent.maximum};
        piece.excitationCurve =
            patch.velocity.excitationCurve;
        piece.outputCurve = patch.velocity.outputCurve;
        piece.brightnessAmount =
            patch.velocity.brightnessAmount;
        piece.decayAmount = patch.velocity.decayAmount;
        piece.driveAmount = patch.velocity.driveAmount;
        piece.chokeGroup = patch.chokeGroup;
        piece.chokeSeconds = patch.chokeSeconds;
        piece.synthSource = patch.synth.source;
        piece.synthMidiNote = patch.synth.midiNote;
        piece.synthLevel = patch.synth.level;
        piece.synthGateSeconds = patch.synth.gateSeconds;
        piece.synthAttackSeconds = patch.synth.attack;
        piece.synthDecaySeconds = patch.synth.decay;
        piece.synthSustain = patch.synth.sustain;
        piece.synthReleaseSeconds = patch.synth.release;
        piece.synthNoiseMix = patch.synth.noiseMix;
        piece.synthFilterCutoffHz =
            patch.synth.filterCutoff;
        sharedKit.pieces.insert(drumKindId(kind), piece);
    }
    QVector<ResearchDrumRenderEvent> sharedEvents;
    sharedEvents.reserve(static_cast<qsizetype>(hits.size()));
    for (const DrumHit& sourceHit : hits) {
        const DrumLabPatch patch =
            kit.value(
                sourceHit.kind,
                defaultDrumLabPatch(
                    profile,
                    sourceHit.kind));
        const DrumHit hit = realiseDrumHit(
            sourceHit,
            profile,
            candidateId,
            patch);
        sharedEvents.push_back({
            hit.frame,
            drumKindId(hit.kind),
            hit.articulation.isEmpty()
                ? hit.strength == DrumHit::Strength::Ghost
                    ? QStringLiteral("ghost")
                    : hit.strength == DrumHit::Strength::Accent
                        ? QStringLiteral("accent")
                        : QStringLiteral("normal")
                : hit.articulation,
            hit.midiVelocity,
            hit.repeatIndex,
            stableSeed(
                QString::number(hit.frame) +
                drumKindId(hit.kind)),
        });
    }
    ResearchDrumRenderResult shared =
        jam2::practice::renderResearchDrumVoices(
            sharedKit,
            sharedEvents,
            static_cast<qint64>(frames),
            kSampleRate);
    std::vector<float> daisy(
        shared.dry.cbegin(),
        shared.dry.cend());
    std::vector<float> roomSend(
        shared.roomSend.cbegin(),
        shared.roomSend.cend());
    for (DrumKind kind : kDrumKinds) {
        if (onlyKind && kind != *onlyKind) continue;
        if (std::none_of(
                hits.begin(),
                hits.end(),
                [kind](const DrumHit& hit) {
                    return hit.kind == kind;
                })) {
            continue;
        }
        const DrumLabPatch patch =
            kit.value(kind, defaultDrumLabPatch(profile, kind));
        const bool hasSecond =
            patch.secondSource != QStringLiteral("off") &&
            patch.blend > 0.0001f;
        float jam2Gain = 0.0f;
        if (patch.source == QStringLiteral("jam2-native")) {
            jam2Gain += hasSecond
                ? std::sqrt(1.0f - patch.blend) : 1.0f;
        }
        if (hasSecond &&
            patch.secondSource == QStringLiteral("jam2-native")) {
            jam2Gain += std::sqrt(patch.blend);
        }
        if (jam2Gain <= 0.0001f) continue;
        // ReferenceRenderResult already assigns UUID filenames. Reuse the
        // validated temporary workspace instead of adding a per-lane
        // directory: the deeper audit TEMP path can otherwise make native
        // Jam2-only pieces fail before the WAV is created on Windows.
        std::vector<float> native =
            renderJam2DrumLane(idea, kind, jam2Workspace);
        native.resize(frames, 0.0f);
        const DrumLabPatch defaultPatch =
            defaultDrumLabPatch(profile, kind);
        const float levelScale = std::clamp(
            patch.level /
                std::max(0.05f, defaultPatch.level),
            0.0f,
            2.5f);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            daisy[frame] +=
                jam2Gain * levelScale * native[frame];
        }
    }
    QVector<float> sharedOutput(
        daisy.cbegin(),
        daisy.cend());
    QVector<float> sharedRoom(
        roomSend.cbegin(),
        roomSend.cend());
    jam2::practice::applyResearchDrumBus(
        sharedOutput,
        sharedRoom,
        sharedKit.bus,
        kSampleRate);
    daisy.assign(
        sharedOutput.cbegin(),
        sharedOutput.cend());
    return daisy;
}

QJsonObject drumAudioMetrics(const std::vector<float>& audio)
{
    if (audio.empty()) return {};
    double sum = 0.0;
    double sumSquares = 0.0;
    float peak = 0.0f;
    std::size_t peakFrame = 0;
    qint64 clipped = 0;
    std::array<double, 5> bandSquares{};
    std::array<double, 4> lowpass{};
    const std::array<double, 4> cutoffs{80.0, 250.0, 2000.0, 8000.0};
    std::array<double, 4> coefficients{};
    for (std::size_t index = 0; index < cutoffs.size(); ++index) {
        coefficients[index] =
            1.0 - std::exp(
                -2.0 * kPi * cutoffs[index] / kSampleRate);
    }
    for (std::size_t frame = 0; frame < audio.size(); ++frame) {
        const double sample = audio[frame];
        sum += sample;
        sumSquares += sample * sample;
        if (std::abs(sample) > peak) {
            peak = static_cast<float>(std::abs(sample));
            peakFrame = frame;
        }
        if (std::abs(sample) >= 0.98) ++clipped;
        for (std::size_t index = 0; index < lowpass.size(); ++index) {
            lowpass[index] +=
                coefficients[index] * (sample - lowpass[index]);
        }
        const std::array<double, 5> bands{
            lowpass[0],
            lowpass[1] - lowpass[0],
            lowpass[2] - lowpass[1],
            lowpass[3] - lowpass[2],
            sample - lowpass[3],
        };
        for (std::size_t index = 0; index < bands.size(); ++index) {
            bandSquares[index] += bands[index] * bands[index];
        }
    }
    const double rms =
        std::sqrt(sumSquares / audio.size());
    const double totalBand =
        std::accumulate(
            bandSquares.begin(), bandSquares.end(), 0.0) +
        1.0e-20;
    QJsonObject bands;
    const std::array<const char*, 5> names{
        "sub_below_80_hz",
        "bass_80_250_hz",
        "low_mid_250_2000_hz",
        "high_mid_2000_8000_hz",
        "air_above_8000_hz",
    };
    for (std::size_t index = 0; index < names.size(); ++index) {
        bands.insert(
            QString::fromLatin1(names[index]),
            bandSquares[index] / totalBand);
    }
    const auto decayTime = [&](double thresholdDb) {
        const double threshold =
            peak * std::pow(10.0, thresholdDb / 20.0);
        const std::size_t hold =
            static_cast<std::size_t>(0.015 * kSampleRate);
        std::size_t below = 0;
        for (std::size_t frame = peakFrame;
             frame < audio.size();
             ++frame) {
            if (std::abs(audio[frame]) <= threshold) {
                ++below;
                if (below >= hold) {
                    return 1000.0 *
                        static_cast<double>(
                            frame - hold + 1 - peakFrame) /
                        kSampleRate;
                }
            } else {
                below = 0;
            }
        }
        return -1.0;
    };
    return {
        {QStringLiteral("peak"), peak},
        {QStringLiteral("rms"), rms},
        {QStringLiteral("crestFactor"),
         rms > 1.0e-12 ? peak / rms : 0.0},
        {QStringLiteral("dcOffset"), sum / audio.size()},
        {QStringLiteral("clippedSamples"), clipped},
        {QStringLiteral("peakFrame"),
         static_cast<qint64>(peakFrame)},
        {QStringLiteral("globalPeakTimeMs"),
         1000.0 * peakFrame / kSampleRate},
        {QStringLiteral("decayToMinus20DbMs"), decayTime(-20.0)},
        {QStringLiteral("decayToMinus40DbMs"), decayTime(-40.0)},
        {QStringLiteral("decayToMinus60DbMs"), decayTime(-60.0)},
        {QStringLiteral("bandEnergyRatio"), bands},
    };
}

QJsonArray realisedDrumHitAudit(
    const std::vector<DrumHit>& hits,
    const std::vector<float>& audio,
    const ProfileDefinition& profile,
    const QString& candidateId,
    const QMap<DrumKind, DrumLabPatch>& kit,
    std::optional<DrumKind> onlyKind)
{
    QJsonArray result;
    for (std::size_t hitIndex = 0;
         hitIndex < hits.size();
         ++hitIndex) {
        const DrumHit& source = hits[hitIndex];
        if (onlyKind && source.kind != *onlyKind) continue;
        const DrumLabPatch patch = kit.value(
            source.kind,
            defaultDrumLabPatch(profile, source.kind));
        const DrumHit hit = realiseDrumHit(
            source, profile, candidateId, patch);
        const qint64 analysisEnd = std::min<qint64>(
            static_cast<qint64>(audio.size()),
            hit.frame +
                static_cast<qint64>(2.0 * kSampleRate));
        qint64 firstFollowing = analysisEnd;
        for (std::size_t next = hitIndex + 1;
             next < hits.size();
             ++next) {
            if (hits[next].frame > hit.frame) {
                firstFollowing = std::min(
                    firstFollowing, hits[next].frame);
                break;
            }
        }
        const qint64 cleanEnd = std::max<qint64>(
            hit.frame + 1,
            std::min(analysisEnd, firstFollowing));
        double energy = 0.0;
        double onsetEnergy = 0.0;
        double bodyEnergy = 0.0;
        double tailEnergy = 0.0;
        qint64 onsetCount = 0;
        qint64 bodyCount = 0;
        qint64 tailCount = 0;
        float localPeak = 0.0f;
        qint64 localPeakFrame = hit.frame;
        const qint64 onsetEnd =
            hit.frame + static_cast<qint64>(0.020 * kSampleRate);
        const qint64 bodyEnd =
            hit.frame + static_cast<qint64>(0.120 * kSampleRate);
        const qint64 tailEnd =
            hit.frame + static_cast<qint64>(0.500 * kSampleRate);
        for (qint64 frame = hit.frame;
             frame < cleanEnd;
             ++frame) {
            const float sample =
                audio[static_cast<std::size_t>(frame)];
            const double square = sample * sample;
            energy += square;
            if (std::abs(sample) > localPeak) {
                localPeak = std::abs(sample);
                localPeakFrame = frame;
            }
            if (frame < onsetEnd) {
                onsetEnergy += square;
                ++onsetCount;
            } else if (frame < bodyEnd) {
                bodyEnergy += square;
                ++bodyCount;
            } else if (frame < tailEnd) {
                tailEnergy += square;
                ++tailCount;
            }
        }
        const auto rms = [](double squareSum, qint64 count) {
            return count > 0
                ? std::sqrt(squareSum / count)
                : 0.0;
        };
        result.append(QJsonObject{
            {QStringLiteral("piece"), drumKindId(hit.kind)},
            {QStringLiteral("frame"), hit.frame},
            {QStringLiteral("timeMs"),
             1000.0 * hit.frame / kSampleRate},
            {QStringLiteral("state"),
             drumStrengthId(hit.strength)},
            {QStringLiteral("articulation"),
             !hit.articulation.isEmpty()
                 ? hit.articulation
                 : hit.kind == DrumKind::Ride
                     ? hit.strength == DrumHit::Strength::Accent
                         ? QStringLiteral("bell")
                         : hit.strength == DrumHit::Strength::Ghost
                             ? QStringLiteral("edge")
                             : QStringLiteral("bow")
                     : QString()},
            {QStringLiteral("fill"), hit.fill},
            {QStringLiteral("repeatIndex"), hit.repeatIndex},
            {QStringLiteral("midiVelocity"), hit.midiVelocity},
            {QStringLiteral("modelExcitation"), hit.excitation},
            {QStringLiteral("outputGain"), hit.outputGain},
            {QStringLiteral("brightnessOffset"),
             hit.brightnessOffset},
            {QStringLiteral("decayScale"), hit.decayScale},
            {QStringLiteral("driveScale"), hit.driveScale},
            {QStringLiteral("analysis"), QJsonObject{
                 {QStringLiteral("windowMs"),
                  1000.0 * (cleanEnd - hit.frame) / kSampleRate},
                 {QStringLiteral("truncatedByFollowingHit"),
                  firstFollowing < analysisEnd},
                 {QStringLiteral("peak"), localPeak},
                 {QStringLiteral("onsetToPeakMs"),
                  1000.0 *
                      (localPeakFrame - hit.frame) /
                      kSampleRate},
                 {QStringLiteral("onsetRms0To20Ms"),
                  rms(onsetEnergy, onsetCount)},
                 {QStringLiteral("bodyRms20To120Ms"),
                  rms(bodyEnergy, bodyCount)},
                 {QStringLiteral("tailRms120To500Ms"),
                  rms(tailEnergy, tailCount)},
                 {QStringLiteral("energy"), energy},
             }},
        });
    }
    return result;
}

const std::vector<float>& stemForRole(
    const StemSet& stems,
    const QString& role)
{
    if (role == QStringLiteral("chords")) return stems.chords;
    if (role == QStringLiteral("melody")) return stems.melody;
    if (role == QStringLiteral("bass")) return stems.bass;
    if (role == QStringLiteral("support")) return stems.support;
    return stems.drums;
}

QString patchIdForRole(
    const ProfileDefinition& profile,
    const QString& role)
{
    if (role == QStringLiteral("chords")) return profile.chordPatchId;
    if (role == QStringLiteral("melody")) return profile.melodyPatchId;
    if (role == QStringLiteral("bass")) return profile.bassPatchId;
    if (role == QStringLiteral("support")) return profile.supportPatchId;
    return profile.drumPatchId;
}

QString roleName(const QString& role)
{
    if (role == QStringLiteral("chords")) return QStringLiteral("Chords / comping");
    if (role == QStringLiteral("melody")) return QStringLiteral("Melody / lead");
    if (role == QStringLiteral("bass")) return QStringLiteral("Bass");
    if (role == QStringLiteral("support")) return QStringLiteral("Supporting line");
    return QStringLiteral("Drums / percussion");
}

QString midiNoteName(int midi)
{
    return jam2::practice::noteName(midi) +
        QString::number(midi / 12 - 1);
}

QJsonArray integerArray(const std::vector<int>& values)
{
    QJsonArray result;
    for (int value : values) result.append(value);
    return result;
}

QJsonArray midiNameArray(const std::vector<int>& values)
{
    QJsonArray result;
    for (int value : values) result.append(midiNoteName(value));
    return result;
}

QJsonArray auditionChordVoicingAudit(
    const GeneratedPracticeIdea& idea)
{
    QJsonArray result;
    double previousCenter = 0.0;
    const SongSection& section = idea.chordSection;
    for (int beat = 0; beat < section.beats; ++beat) {
        if (beat >= section.musicalPatterns.size()) continue;
        const MusicalBeatPattern& pattern =
            section.musicalPatterns.at(beat);
        if (pattern.division <= 0 ||
            pattern.chords.size() != pattern.division) {
            continue;
        }
        for (int step = 0; step < pattern.division; ++step) {
            const MusicalStep& value = pattern.chords.at(step);
            if (value.state != MusicalStepState::Onset) continue;
            const ParsedChord parsed =
                jam2::practice::parseChord(value.value);
            if (!parsed.valid || parsed.rest) continue;
            std::vector<int> midi =
                chordNotes(parsed, idea.recipe.styleId, previousCenter);
            if (!midi.empty()) {
                double sum = 0.0;
                for (int note : midi) sum += note;
                previousCenter = sum / midi.size();
            }
            std::vector<int> sorted = midi;
            std::sort(sorted.begin(), sorted.end());
            QJsonArray closePairs;
            bool closeRootSeventh = false;
            for (std::size_t low = 0; low < sorted.size(); ++low) {
                for (std::size_t high = low + 1;
                     high < sorted.size();
                     ++high) {
                    const int semitones = sorted[high] - sorted[low];
                    if (semitones > 2) break;
                    closePairs.append(QJsonObject{
                        {QStringLiteral("low"),
                         midiNoteName(sorted[low])},
                        {QStringLiteral("high"),
                         midiNoteName(sorted[high])},
                        {QStringLiteral("semitones"), semitones},
                    });
                    const int lowFromRoot =
                        (sorted[low] - parsed.root + 120) % 12;
                    const int highFromRoot =
                        (sorted[high] - parsed.root + 120) % 12;
                    closeRootSeventh =
                        closeRootSeventh ||
                        ((lowFromRoot == 0 &&
                          (highFromRoot == 10 ||
                           highFromRoot == 11)) ||
                         (highFromRoot == 0 &&
                          (lowFromRoot == 10 ||
                           lowFromRoot == 11)));
                }
            }
            const int tick =
                beat * kTicksPerBeat +
                step * kTicksPerBeat / pattern.division;
            result.append(QJsonObject{
                {QStringLiteral("tick"), tick},
                {QStringLiteral("beat"),
                 static_cast<double>(tick) / kTicksPerBeat},
                {QStringLiteral("symbol"), value.value},
                {QStringLiteral("suffix"), parsed.suffix},
                {QStringLiteral("root_pitch_class"), parsed.root},
                {QStringLiteral("slash_bass_pitch_class"), parsed.bass},
                {QStringLiteral("midi"), integerArray(midi)},
                {QStringLiteral("notes"), midiNameArray(midi)},
                {QStringLiteral("sorted_notes"),
                 midiNameArray(sorted)},
                {QStringLiteral("span_semitones"),
                 sorted.empty() ? 0 :
                     sorted.back() - sorted.front()},
                {QStringLiteral("close_pairs"), closePairs},
                {QStringLiteral("close_root_seventh"),
                 closeRootSeventh},
                {QStringLiteral("articulation"),
                 value.articulation},
            });
        }
    }
    return result;
}

QJsonArray auditionLaneEventAudit(
    const GeneratedPracticeIdea& idea)
{
    QJsonArray result;
    const double beatFrames = framesPerBeat(idea);
    for (const NoteEvent& event : extractEvents(idea)) {
        if (event.role == QStringLiteral("chords")) continue;
        result.append(QJsonObject{
            {QStringLiteral("role"), event.role},
            {QStringLiteral("start_beat"),
             event.start / beatFrames},
            {QStringLiteral("duration_beats"),
             (event.end - event.start) / beatFrames},
            {QStringLiteral("midi"), event.midi},
            {QStringLiteral("note"),
             midiNoteName(event.midi)},
            {QStringLiteral("velocity"), event.velocity},
            {QStringLiteral("articulation"),
             event.articulation},
        });
    }
    return result;
}

QJsonArray auditionDrumHitAudit(
    const GeneratedPracticeIdea& idea)
{
    QJsonArray result;
    const QStringList laneNames =
        BeatGridModel::beatLaneNames();
    for (int beat = 0;
         beat < idea.beatSection.beatPatterns.size();
         ++beat) {
        const BeatPattern& pattern =
            idea.beatSection.beatPatterns.at(beat);
        if (pattern.division <= 0) continue;
        for (int lane = 0;
             lane < pattern.lanes.size() &&
             lane < laneNames.size();
             ++lane) {
            const QString& cells = pattern.lanes.at(lane);
            for (int step = 0;
                 step < cells.size();
                 ++step) {
                if (cells.at(step) == QLatin1Char('.')) {
                    continue;
                }
                const int tick =
                    beat * kTicksPerBeat +
                    step * kTicksPerBeat /
                        pattern.division;
                result.append(QJsonObject{
                    {QStringLiteral("lane"),
                     laneNames.at(lane)},
                    {QStringLiteral("state"),
                     QString(cells.at(step))},
                    {QStringLiteral("tick"), tick},
                    {QStringLiteral("beat"),
                     static_cast<double>(tick) /
                         kTicksPerBeat},
                });
            }
        }
    }
    return result;
}

QJsonObject seedAuditForProfile(
    const ProfileDefinition& profile)
{
    ChordIdeaRequest request;
    request.styleId = profile.styleId;
    request.profileId = profile.id;
    if (!profile.forms.isEmpty()) {
        request.formId = profile.forms.first().id;
    }
    request.harmonicComplexity = 4;
    request.rhythmicComplexity = 4;
    const std::uint32_t seed = stableSeed(profile.id);
    const GeneratedPracticeIdea fullIdea =
        jam2::practice::generateCoupledPracticeIdeaForTest(
            request, seed);
    const GeneratedPracticeIdea auditionIdea =
        trimIdea(fullIdea);
    return {
        {QStringLiteral("id"), profile.id},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("seed"), QString::number(seed)},
        {QStringLiteral("audition_bars"), kAuditionBars},
        {QStringLiteral("research_constraints"), QJsonObject{
             {QStringLiteral("grammar_id"),
              profile.grammarId},
             {QStringLiteral("minimum_bpm"),
              profile.minimumBpm},
             {QStringLiteral("maximum_bpm"),
              profile.maximumBpm},
             {QStringLiteral("tonal_collections"),
              QJsonArray::fromStringList(
                  profile.tonalCollections)},
             {QStringLiteral("progression_families"),
              QJsonArray::fromStringList(
                  profile.progressionFamilies)},
             {QStringLiteral("groove_families"),
              QJsonArray::fromStringList(
                  profile.grooveFamilies)},
             {QStringLiteral("meter_ids"),
              QJsonArray::fromStringList(
                  profile.meterIds)},
             {QStringLiteral("bass_grammar"),
              profile.bassGrammar},
             {QStringLiteral("motif_grammar"),
              profile.motifGrammar},
         }},
        {QStringLiteral("recipe"),
         jam2::practice::generationRecipeToJson(
             fullIdea.recipe)},
        {QStringLiteral("audition_chord_voicings"),
         auditionChordVoicingAudit(auditionIdea)},
        {QStringLiteral("audition_lane_events"),
         auditionLaneEventAudit(auditionIdea)},
        {QStringLiteral("audition_drum_hits"),
         auditionDrumHitAudit(auditionIdea)},
    };
}

void writeSeedAudit(const QDir& site)
{
    QJsonArray profiles;
    for (const ProfileDefinition& profile :
         jam2::practice::profileCatalog(true)) {
        profiles.append(seedAuditForProfile(profile));
    }
    const QJsonObject audit{
        {QStringLiteral("version"), 1},
        {QStringLiteral("generated_at"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("harmonic_complexity"), 4},
        {QStringLiteral("rhythmic_complexity"), 4},
        {QStringLiteral("profiles"), profiles},
    };
    QFile file(
        site.absoluteFilePath(
            QStringLiteral("seed-audit.json")));
    if (!file.open(
            QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(
            "Cannot write seed-audit.json.");
    }
    file.write(
        QJsonDocument(audit).toJson(
            QJsonDocument::Indented));
    QTextStream(stdout)
        << "Wrote " << profiles.size()
        << " profile seed audits to "
        << file.fileName() << "\n";
}

QString styleSoundBrief(const QString& styleId)
{
    if (styleId == QStringLiteral("pop")) {
        return QStringLiteral(
            "Clean or bright lead, compact polysynth/keys, controlled bass, "
            "layered but sparse support, tight kit and selective hook doubling.");
    }
    if (styleId == QStringLiteral("rock")) {
        return QStringLiteral(
            "Picked, muted and open guitar-like attacks, independent bass, "
            "transient-rich drums and contrast between driven and open spaces.");
    }
    if (styleId == QStringLiteral("jazz")) {
        return QStringLiteral(
            "Keys or restrained pluck comping, damped bass, ride/hat/brush "
            "colour and a resonant lead; Fusion adds electric drive and synthesis.");
    }
    if (styleId == QStringLiteral("modal-jam")) {
        return QStringLiteral(
            "Pedal bass, spacious pluck or pad, airy lead and slow spectral "
            "movement that exposes modal colour without forcing chord changes.");
    }
    if (styleId == QStringLiteral("blues")) {
        return QStringLiteral(
            "Electric/acoustic-like pluck or organ, round bass, resonant "
            "call-and-response lead and shuffle, straight or compound kit.");
    }
    if (styleId == QStringLiteral("jpop-anisong")) {
        return QStringLiteral(
            "Bright layered guitar/synth/keys, active bass, crisp drums, "
            "vocal-like lead and selective harmony or countermelody colour.");
    }
    if (styleId == QStringLiteral("country")) {
        return QStringLiteral(
            "Acoustic/electric-like pluck, alternating or muted bass, dry kit "
            "and explicitly simplified fiddle, steel or clean-fill responses.");
    }
    if (styleId == QStringLiteral("electronic")) {
        return QStringLiteral(
            "Dedicated kick architecture, hats/claps, synth bass and stabs, "
            "noise/texture and evolving filter, drive and envelope behaviour.");
    }
    if (styleId == QStringLiteral("rnb-soul")) {
        return QStringLiteral(
            "Electric piano or organ, melodic/sub-electric bass, muted pluck, "
            "warm pads, short ensemble responses and pocket-focused percussion.");
    }
    if (styleId == QStringLiteral("funk")) {
        return QStringLiteral(
            "Dry transient kit, envelope-aware bass, clipped pluck or clav, "
            "warm lead and short synthetic ensemble stabs with disciplined space.");
    }
    if (styleId == QStringLiteral("hiphop-trap")) {
        return QStringLiteral(
            "Original sample-like filtered keys for Boom-Bap; pitched 808, "
            "sharp hats/rims and sparse bell, pluck or pad for Trap.");
    }
    if (styleId == QStringLiteral("reggae")) {
        return QStringLiteral(
            "Deep round bass, short skank, additive organ colour, cross-stick "
            "and light percussion with role-aware dub echo rather than generic space.");
    }
    if (styleId == QStringLiteral("bossa-nova")) {
        return QStringLiteral(
            "Nylon-like physical pluck, soft independent bass, intimate lead "
            "and restrained cross-stick, brush and shaker colour.");
    }
    return QStringLiteral(
        "Low articulated double-string riff, split bass, modern transient kit "
        "and a genuinely contrasting clean or ambient supporting layer.");
}

QString styleSoundLimits(const QString& styleId)
{
    if (styleId == QStringLiteral("rock") ||
        styleId == QStringLiteral("metal-experimental")) {
        return QStringLiteral(
            "The physical-string and cabinet path is deliberately synthetic; "
            "continuous bends, feedback and recorded amp interaction remain approximate.");
    }
    if (styleId == QStringLiteral("jazz") ||
        styleId == QStringLiteral("rnb-soul") ||
        styleId == QStringLiteral("funk")) {
        return QStringLiteral(
            "Horn, reed, brush and ensemble colours are compact synthesis targets, "
            "so groove, articulation, voicing and role separation still carry identity.");
    }
    if (styleId == QStringLiteral("country")) {
        return QStringLiteral(
            "Fiddle and pedal-steel colours are explicitly '-like'; target motion "
            "and articulation are more important than claiming acoustic realism.");
    }
    if (styleId == QStringLiteral("bossa-nova")) {
        return QStringLiteral(
            "The nylon/body sound is approximate; independent bass/comping context "
            "and restrained articulation are the reliable teaching features.");
    }
    return QStringLiteral(
        "These are compact deterministic synthesis targets, not attempts to clone "
        "named commercial instruments, recordings or proprietary presets.");
}

QString sourceKindName(SourceKind source)
{
    switch (source) {
    case SourceKind::Jam2Native: return QStringLiteral("jam2-native");
    case SourceKind::Shape: return QStringLiteral("variable-shape");
    case SourceKind::VariableSaw: return QStringLiteral("variable-saw");
    case SourceKind::Fm: return QStringLiteral("fm2");
    case SourceKind::String: return QStringLiteral("string-physical-model");
    case SourceKind::Harmonic: return QStringLiteral("additive-harmonic");
    case SourceKind::Sine: return QStringLiteral("sine-fundamental");
    case SourceKind::Formant: return QStringLiteral("phase-reset-formant");
    case SourceKind::Vosim: return QStringLiteral("vosim-formant");
    case SourceKind::Z: return QStringLiteral("z-oscillator");
    }
    return QStringLiteral("unknown");
}

std::optional<SourceKind> sourceKindFromName(const QString& source)
{
    if (source == QStringLiteral("jam2-native")) {
        return SourceKind::Jam2Native;
    }
    if (source == QStringLiteral("variable-shape")) return SourceKind::Shape;
    if (source == QStringLiteral("variable-saw")) return SourceKind::VariableSaw;
    if (source == QStringLiteral("fm2")) return SourceKind::Fm;
    if (source == QStringLiteral("string-physical-model")) return SourceKind::String;
    if (source == QStringLiteral("additive-harmonic")) return SourceKind::Harmonic;
    if (source == QStringLiteral("sine-fundamental")) return SourceKind::Sine;
    if (source == QStringLiteral("phase-reset-formant")) return SourceKind::Formant;
    if (source == QStringLiteral("vosim-formant")) return SourceKind::Vosim;
    if (source == QStringLiteral("z-oscillator")) return SourceKind::Z;
    return std::nullopt;
}

QJsonObject patchParameters(const PatchDesign& patch)
{
    const auto filterName = [&]() {
        switch (patch.filter) {
        case FilterKind::Ladder:
            return QStringLiteral("ladder-lowpass");
        case FilterKind::StateVariableLowpass:
            return QStringLiteral("state-variable-lowpass");
        case FilterKind::StateVariableBandpass:
            return QStringLiteral("state-variable-bandpass");
        case FilterKind::Direct:
            return QStringLiteral("source-direct");
        }
        return QStringLiteral("unknown");
    };
    return {
        {QStringLiteral("source"), sourceKindName(patch.source)},
        {QStringLiteral("secondSource"),
         patch.secondSourceEnabled
             ? sourceKindName(patch.secondSource)
             : QStringLiteral("off")},
        {QStringLiteral("sourceBlend"), patch.sourceBlend},
        {QStringLiteral("secondSourceTranspose"),
         patch.secondSourceTranspose},
        {QStringLiteral("secondSourceDetuneCents"),
         patch.secondSourceDetuneCents},
        {QStringLiteral("filterArchitecture"), filterName()},
        {QStringLiteral("harmonicFamily"), patch.harmonicFamily},
        {QStringLiteral("shape"), patch.shape},
        {QStringLiteral("width"), patch.width},
        {QStringLiteral("oscillator2Mix"), patch.oscillator2Mix},
        {QStringLiteral("detuneCents"), patch.detuneCents},
        {QStringLiteral("subMix"), patch.subMix},
        {QStringLiteral("fmRatio"), patch.fmRatio},
        {QStringLiteral("fmIndex"), patch.fmIndex},
        {QStringLiteral("formantRatio"), patch.formantRatio},
        {QStringLiteral("formantRatio2"), patch.formantRatio2},
        {QStringLiteral("fixedFormantHz"), patch.formantHz},
        {QStringLiteral("fixedFormant2Hz"), patch.formantHz2},
        {QStringLiteral("spectralShape"), patch.spectralShape},
        {QStringLiteral("spectralMode"), patch.spectralMode},
        {QStringLiteral("stringStructure"), patch.stringStructure},
        {QStringLiteral("stringBrightness"), patch.stringBrightness},
        {QStringLiteral("stringDamping"), patch.stringDamping},
        {QStringLiteral("stringDouble"), patch.stringDouble},
        {QStringLiteral("attackSeconds"), patch.attack},
        {QStringLiteral("decaySeconds"), patch.decay},
        {QStringLiteral("sustain"), patch.sustain},
        {QStringLiteral("releaseSeconds"), patch.release},
        {QStringLiteral("filterCutoffHz"), patch.filterCutoff},
        {QStringLiteral("filterEnvelopeHz"), patch.filterEnvelope},
        {QStringLiteral("resonance"), patch.resonance},
        {QStringLiteral("filterDrive"), patch.filterDrive},
        {QStringLiteral("wavefold"), patch.wavefold},
        {QStringLiteral("noiseMix"), patch.noiseMix},
        {QStringLiteral("transientMix"), patch.transientMix},
        {QStringLiteral("transientSeconds"), patch.transientSeconds},
        {QStringLiteral("vibratoCents"), patch.vibratoCents},
        {QStringLiteral("vibratoRateHz"), patch.vibratoRate},
        {QStringLiteral("vibratoDelaySeconds"), patch.vibratoDelay},
        {QStringLiteral("tremoloDepth"), patch.tremoloDepth},
        {QStringLiteral("tremoloRateHz"), patch.tremoloRate},
        {QStringLiteral("voiceDrive"), patch.voiceDrive},
        {QStringLiteral("busDrive"), patch.busDrive},
        {QStringLiteral("cabinet"), patch.cabinet},
        {QStringLiteral("chorusMix"), patch.chorusMix},
        {QStringLiteral("chorusDepth"), patch.chorusDepth},
        {QStringLiteral("chorusRateHz"), patch.chorusRate},
        {QStringLiteral("delayMix"), patch.delayMix},
        {QStringLiteral("delaySeconds"), patch.delaySeconds},
    };
}

float boundedPatchNumber(
    const QJsonObject& values,
    const QString& name,
    float fallback,
    float minimum,
    float maximum)
{
    const QJsonValue value = values.value(name);
    if (value.isUndefined()) return fallback;
    const double number = value.toDouble(
        std::numeric_limits<double>::quiet_NaN());
    // Browser controls use exact decimal JSON while the native bounds are
    // stored as binary32. Accept one representable float step around each
    // boundary, then clamp to the authoritative native range. Without this,
    // a valid UI minimum such as 0.001 compares below 0.001000000047.
    const double acceptedMinimum = static_cast<double>(
        std::nextafter(
            minimum,
            -std::numeric_limits<float>::infinity()));
    const double acceptedMaximum = static_cast<double>(
        std::nextafter(
            maximum,
            std::numeric_limits<float>::infinity()));
    if (!value.isDouble() || !std::isfinite(number) ||
        number < acceptedMinimum || number > acceptedMaximum) {
        throw std::runtime_error(
            QStringLiteral("Patch parameter %1 is outside %2..%3.")
                .arg(name)
                .arg(minimum)
                .arg(maximum)
                .toStdString());
    }
    return std::clamp(
        static_cast<float>(number),
        minimum,
        maximum);
}

int boundedPatchInteger(
    const QJsonObject& values,
    const QString& name,
    int fallback,
    int minimum,
    int maximum)
{
    const QJsonValue value = values.value(name);
    if (value.isUndefined()) return fallback;
    const double number = value.toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) ||
        std::floor(number) != number ||
        number < minimum || number > maximum) {
        throw std::runtime_error(
            QStringLiteral("Patch parameter %1 is outside %2..%3.")
                .arg(name)
                .arg(minimum)
                .arg(maximum)
                .toStdString());
    }
    return static_cast<int>(number);
}

PatchDesign patchFromJson(
    const QJsonObject& values,
    PatchDesign patch)
{
    const QString source =
        values.value(QStringLiteral("source")).toString();
    if (const std::optional<SourceKind> parsed =
            sourceKindFromName(source)) {
        patch.source = *parsed;
    } else if (!source.isEmpty()) {
        throw std::runtime_error("Unsupported Daisy source architecture.");
    }
    const QString secondSource =
        values.value(QStringLiteral("secondSource")).toString();
    if (secondSource.isEmpty() || secondSource == QStringLiteral("off")) {
        patch.secondSourceEnabled = false;
    } else if (const std::optional<SourceKind> parsed =
                   sourceKindFromName(secondSource)) {
        patch.secondSourceEnabled = true;
        patch.secondSource = *parsed;
    } else {
        throw std::runtime_error(
            "Unsupported second Daisy source architecture.");
    }
    patch.sourceBlend = boundedPatchNumber(
        values, QStringLiteral("sourceBlend"),
        patch.sourceBlend, 0.0f, 1.0f);
    patch.secondSourceTranspose = boundedPatchInteger(
        values, QStringLiteral("secondSourceTranspose"),
        patch.secondSourceTranspose, -24, 24);
    patch.secondSourceDetuneCents = boundedPatchNumber(
        values, QStringLiteral("secondSourceDetuneCents"),
        patch.secondSourceDetuneCents, -100.0f, 100.0f);

    const QString filter =
        values.value(QStringLiteral("filterArchitecture")).toString();
    if (filter == QStringLiteral("ladder-lowpass")) {
        patch.filter = FilterKind::Ladder;
    } else if (filter == QStringLiteral("state-variable-lowpass")) {
        patch.filter = FilterKind::StateVariableLowpass;
    } else if (filter == QStringLiteral("state-variable-bandpass")) {
        patch.filter = FilterKind::StateVariableBandpass;
    } else if (filter == QStringLiteral("source-direct")) {
        patch.filter = FilterKind::Direct;
    } else if (!filter.isEmpty()) {
        throw std::runtime_error("Unsupported Daisy filter architecture.");
    }

    patch.harmonicFamily = boundedPatchInteger(
        values, QStringLiteral("harmonicFamily"),
        patch.harmonicFamily, 0, 5);
    patch.shape = boundedPatchNumber(
        values, QStringLiteral("shape"), patch.shape, 0.0f, 1.0f);
    patch.width = boundedPatchNumber(
        values, QStringLiteral("width"), patch.width, 0.05f, 0.95f);
    patch.oscillator2Mix = boundedPatchNumber(
        values, QStringLiteral("oscillator2Mix"),
        patch.oscillator2Mix, 0.0f, 1.0f);
    patch.detuneCents = boundedPatchNumber(
        values, QStringLiteral("detuneCents"),
        patch.detuneCents, -50.0f, 50.0f);
    patch.subMix = boundedPatchNumber(
        values, QStringLiteral("subMix"), patch.subMix, 0.0f, 1.0f);
    patch.fmRatio = boundedPatchNumber(
        values, QStringLiteral("fmRatio"), patch.fmRatio, 0.125f, 16.0f);
    patch.fmIndex = boundedPatchNumber(
        values, QStringLiteral("fmIndex"), patch.fmIndex, 0.0f, 16.0f);
    patch.formantRatio = boundedPatchNumber(
        values, QStringLiteral("formantRatio"),
        patch.formantRatio, 0.25f, 16.0f);
    patch.formantRatio2 = boundedPatchNumber(
        values, QStringLiteral("formantRatio2"),
        patch.formantRatio2, 0.25f, 16.0f);
    patch.formantHz = boundedPatchNumber(
        values, QStringLiteral("fixedFormantHz"),
        patch.formantHz, 0.0f, 12000.0f);
    patch.formantHz2 = boundedPatchNumber(
        values, QStringLiteral("fixedFormant2Hz"),
        patch.formantHz2, 0.0f, 12000.0f);
    patch.spectralShape = boundedPatchNumber(
        values, QStringLiteral("spectralShape"),
        patch.spectralShape, 0.0f, 1.0f);
    patch.spectralMode = boundedPatchNumber(
        values, QStringLiteral("spectralMode"),
        patch.spectralMode, 0.0f, 1.0f);
    patch.stringStructure = boundedPatchNumber(
        values, QStringLiteral("stringStructure"),
        patch.stringStructure, 0.0f, 1.0f);
    patch.stringBrightness = boundedPatchNumber(
        values, QStringLiteral("stringBrightness"),
        patch.stringBrightness, 0.0f, 1.0f);
    patch.stringDamping = boundedPatchNumber(
        values, QStringLiteral("stringDamping"),
        patch.stringDamping, 0.0f, 1.0f);
    patch.stringDouble = boundedPatchNumber(
        values, QStringLiteral("stringDouble"),
        patch.stringDouble, 0.0f, 1.0f);
    patch.attack = boundedPatchNumber(
        values, QStringLiteral("attackSeconds"),
        patch.attack, 0.001f, 5.0f);
    patch.decay = boundedPatchNumber(
        values, QStringLiteral("decaySeconds"),
        patch.decay, 0.005f, 5.0f);
    patch.sustain = boundedPatchNumber(
        values, QStringLiteral("sustain"), patch.sustain, 0.01f, 1.0f);
    patch.release = boundedPatchNumber(
        values, QStringLiteral("releaseSeconds"),
        patch.release, 0.005f, 8.0f);
    patch.filterCutoff = boundedPatchNumber(
        values, QStringLiteral("filterCutoffHz"),
        patch.filterCutoff, 40.0f, 18000.0f);
    patch.filterEnvelope = boundedPatchNumber(
        values, QStringLiteral("filterEnvelopeHz"),
        patch.filterEnvelope, -12000.0f, 12000.0f);
    patch.resonance = boundedPatchNumber(
        values, QStringLiteral("resonance"),
        patch.resonance, 0.0f, 0.95f);
    patch.filterDrive = boundedPatchNumber(
        values, QStringLiteral("filterDrive"),
        patch.filterDrive, 0.5f, 8.0f);
    patch.wavefold = boundedPatchNumber(
        values, QStringLiteral("wavefold"), patch.wavefold, 0.0f, 8.0f);
    patch.noiseMix = boundedPatchNumber(
        values, QStringLiteral("noiseMix"), patch.noiseMix, 0.0f, 1.0f);
    patch.transientMix = boundedPatchNumber(
        values, QStringLiteral("transientMix"),
        patch.transientMix, 0.0f, 1.0f);
    patch.transientSeconds = boundedPatchNumber(
        values, QStringLiteral("transientSeconds"),
        patch.transientSeconds, 0.001f, 0.25f);
    patch.vibratoCents = boundedPatchNumber(
        values, QStringLiteral("vibratoCents"),
        patch.vibratoCents, 0.0f, 100.0f);
    patch.vibratoRate = boundedPatchNumber(
        values, QStringLiteral("vibratoRateHz"),
        patch.vibratoRate, 0.05f, 15.0f);
    patch.vibratoDelay = boundedPatchNumber(
        values, QStringLiteral("vibratoDelaySeconds"),
        patch.vibratoDelay, 0.0f, 3.0f);
    patch.tremoloDepth = boundedPatchNumber(
        values, QStringLiteral("tremoloDepth"),
        patch.tremoloDepth, 0.0f, 1.0f);
    patch.tremoloRate = boundedPatchNumber(
        values, QStringLiteral("tremoloRateHz"),
        patch.tremoloRate, 0.02f, 20.0f);
    patch.voiceDrive = boundedPatchNumber(
        values, QStringLiteral("voiceDrive"),
        patch.voiceDrive, 0.5f, 10.0f);
    patch.busDrive = boundedPatchNumber(
        values, QStringLiteral("busDrive"),
        patch.busDrive, 0.5f, 10.0f);
    patch.cabinet = boundedPatchNumber(
        values, QStringLiteral("cabinet"), patch.cabinet, 0.0f, 1.0f);
    patch.chorusMix = boundedPatchNumber(
        values, QStringLiteral("chorusMix"),
        patch.chorusMix, 0.0f, 1.0f);
    patch.chorusDepth = boundedPatchNumber(
        values, QStringLiteral("chorusDepth"),
        patch.chorusDepth, 0.0f, 1.0f);
    patch.chorusRate = boundedPatchNumber(
        values, QStringLiteral("chorusRateHz"),
        patch.chorusRate, 0.02f, 8.0f);
    patch.delayMix = boundedPatchNumber(
        values, QStringLiteral("delayMix"),
        patch.delayMix, 0.0f, 0.75f);
    patch.delaySeconds = boundedPatchNumber(
        values, QStringLiteral("delaySeconds"),
        patch.delaySeconds, 0.03f, 1.5f);
    patch.name = QStringLiteral("Instrument Lab patch");
    return patch;
}

std::vector<NoteEvent> laboratoryEvents(
    const QString& audition,
    const QString& role,
    int rootMidi,
    const ProfileDefinition& profile,
    std::size_t& frames,
    std::optional<GeneratedPracticeIdea>* generatedIdea = nullptr)
{
    const auto event = [&role](
        double startSeconds,
        double durationSeconds,
        int midi,
        int velocity) {
        return NoteEvent{
            static_cast<qint64>(std::llround(startSeconds * kSampleRate)),
            static_cast<qint64>(std::llround(
                (startSeconds + durationSeconds) * kSampleRate)),
            midi,
            velocity,
            role,
            QString(),
        };
    };
    std::vector<NoteEvent> events;
    if (audition == QStringLiteral("note")) {
        events.push_back(event(0.20, 1.40, rootMidi, 92));
        frames = static_cast<std::size_t>(3.0 * kSampleRate);
    } else if (audition == QStringLiteral("velocity")) {
        events.push_back(event(0.20, 0.70, rootMidi - 7, 48));
        events.push_back(event(1.20, 0.70, rootMidi, 82));
        events.push_back(event(2.20, 0.70, rootMidi + 7, 118));
        frames = static_cast<std::size_t>(4.1 * kSampleRate);
    } else if (audition == QStringLiteral("chord")) {
        const std::array<int, 4> intervals{0, 4, 7, 11};
        for (int interval : intervals) {
            events.push_back(event(0.20, 1.55, rootMidi + interval, 86));
        }
        const std::array<int, 4> second{5, 9, 12, 16};
        for (int interval : second) {
            events.push_back(event(2.05, 1.25, rootMidi + interval, 82));
        }
        frames = static_cast<std::size_t>(4.5 * kSampleRate);
    } else if (audition == QStringLiteral("profile")) {
        ChordIdeaRequest request;
        request.styleId = profile.styleId;
        request.profileId = profile.id;
        if (!profile.forms.isEmpty()) {
            request.formId = profile.forms.first().id;
        }
        request.harmonicComplexity = 4;
        request.rhythmicComplexity = 4;
        GeneratedPracticeIdea idea =
            jam2::practice::generateCoupledPracticeIdeaForTest(
                request, stableSeed(profile.id));
        idea = trimIdea(std::move(idea));
        events = extractEvents(idea);
        frames = static_cast<std::size_t>(
            std::max<qint64>(
                1,
                frameAtTick(
                    idea,
                    idea.chordSection.beats * kTicksPerBeat) +
                    static_cast<qint64>(2.0 * kSampleRate)));
        if (generatedIdea) *generatedIdea = idea;
    } else {
        throw std::runtime_error("Unsupported Instrument Lab audition.");
    }
    std::erase_if(events, [&role](const NoteEvent& note) {
        return note.role != role;
    });
    if (events.empty()) {
        events.push_back(event(0.20, 1.40, rootMidi, 92));
        frames = static_cast<std::size_t>(3.0 * kSampleRate);
    }
    return events;
}

void renderDrumKitRequest(
    const QJsonObject& request,
    const ProfileDefinition& profile,
    const QString& outputPath)
{
    if (!request.value(QStringLiteral("kit")).isObject()) {
        throw std::runtime_error(
            "Drum Kit Lab request must contain a kit object.");
    }
    const QJsonObject kitObject =
        request.value(QStringLiteral("kit")).toObject();
    const QJsonObject pieces =
        kitObject.value(QStringLiteral("pieces")).toObject();
    const QString candidateId =
        kitObject.value(QStringLiteral("candidateId"))
            .toString(QStringLiteral("custom"))
            .left(96);
    QMap<DrumKind, DrumLabPatch> kit;
    for (DrumKind kind : kDrumKinds) {
        DrumLabPatch patch =
            defaultDrumLabPatch(profile, kind);
        const QJsonValue value =
            pieces.value(drumKindId(kind));
        if (value.isObject()) {
            patch = drumLabPatchFromJson(
                value.toObject(),
                patch);
        }
        kit.insert(kind, patch);
    }
    DrumBusDesign bus = drumBusDesign(profile);
    const QJsonObject busObject =
        kitObject.value(QStringLiteral("bus")).toObject();
    bus.drive = boundedPatchNumber(
        busObject, QStringLiteral("drive"),
        static_cast<float>(bus.drive), 0.5f, 8.0f);
    bus.cutoffHz = boundedPatchNumber(
        busObject, QStringLiteral("lowpassHz"),
        static_cast<float>(bus.cutoffHz), 200.0f, 20000.0f);
    bus.compressorThreshold = boundedPatchNumber(
        busObject, QStringLiteral("compressorThreshold"),
        static_cast<float>(bus.compressorThreshold), 0.01f, 1.0f);
    bus.compressorRatio = boundedPatchNumber(
        busObject, QStringLiteral("compressorRatio"),
        static_cast<float>(bus.compressorRatio), 1.0f, 20.0f);
    bus.compressorReleaseMs = boundedPatchNumber(
        busObject, QStringLiteral("compressorReleaseMs"),
        static_cast<float>(bus.compressorReleaseMs), 5.0f, 500.0f);
    bus.roomMix = boundedPatchNumber(
        busObject, QStringLiteral("roomMix"),
        static_cast<float>(bus.roomMix), 0.0f, 0.6f);
    bus.roomSizeMs = boundedPatchNumber(
        busObject, QStringLiteral("roomSizeMs"),
        static_cast<float>(bus.roomSizeMs), 5.0f, 140.0f);
    bus.roomDamping = boundedPatchNumber(
        busObject, QStringLiteral("roomDamping"),
        static_cast<float>(bus.roomDamping), 0.0f, 1.0f);

    ChordIdeaRequest ideaRequest;
    ideaRequest.styleId = profile.styleId;
    ideaRequest.profileId = profile.id;
    if (!profile.forms.isEmpty()) {
        ideaRequest.formId = profile.forms.first().id;
    }
    ideaRequest.harmonicComplexity = 4;
    ideaRequest.rhythmicComplexity = 4;
    GeneratedPracticeIdea idea =
        jam2::practice::generateCoupledPracticeIdeaForTest(
            ideaRequest,
            stableSeed(profile.id));
    idea = trimIdea(std::move(idea));
    std::size_t frames = static_cast<std::size_t>(
        std::max<qint64>(
            1,
            frameAtTick(
                idea,
                idea.chordSection.beats * kTicksPerBeat) +
                static_cast<qint64>(2.0 * kSampleRate)));
    const QString audition =
        request.value(QStringLiteral("audition")).toString();
    std::optional<DrumKind> onlyKind;
    bool injectedAuditionHit = false;
    if (audition == QStringLiteral("piece") ||
        audition == QStringLiteral("one-shot") ||
        audition == QStringLiteral("velocity-ladder") ||
        audition == QStringLiteral("repeated-hits")) {
        onlyKind = drumKindFromId(
            request.value(QStringLiteral("drumPiece")).toString());
        if (!onlyKind) {
            throw std::runtime_error(
                "Drum Kit Lab piece is unknown.");
        }
        if (audition == QStringLiteral("one-shot")) {
            prepareDrumOneShot(idea, *onlyKind);
            frames = static_cast<std::size_t>(
                3.0 * kSampleRate);
        } else if (
            audition == QStringLiteral("velocity-ladder")) {
            prepareDrumVelocityLadder(idea, *onlyKind);
            frames = static_cast<std::size_t>(
                std::max<qint64>(
                    1,
                    frameAtTick(
                        idea,
                        idea.beatSection.beats * kTicksPerBeat) +
                        static_cast<qint64>(
                            2.0 * kSampleRate)));
        } else if (
            audition == QStringLiteral("repeated-hits")) {
            prepareDrumRepeatedHits(idea, *onlyKind);
            frames = static_cast<std::size_t>(
                std::max<qint64>(
                    1,
                    frameAtTick(
                        idea,
                        idea.beatSection.beats * kTicksPerBeat) +
                        static_cast<qint64>(
                            2.0 * kSampleRate)));
        } else {
            injectedAuditionHit =
                ensureDrumAuditionHit(idea, *onlyKind, frames);
        }
    } else if (audition != QStringLiteral("profile")) {
        throw std::runtime_error(
            "Unsupported Drum Kit Lab audition.");
    }
    const bool productionPatternExact =
        usesProductionDrumPerformance(idea);
    int fillEvents = 0;
    int microtimedEvents = 0;
    if (productionPatternExact) {
        const int maximumTick =
            idea.beatSection.beats * kTicksPerBeat;
        for (const auto& event : idea.recipe.drumEvents) {
            if (event.tick >= maximumTick) break;
            if (event.fill) ++fillEvents;
            if (event.offsetMs != 0) {
                ++microtimedEvents;
            }
        }
    }
    const QString nativeWorkspace =
        QFileInfo(outputPath).absolutePath();
    std::vector<float> audio =
        renderLaboratoryKit(
            idea,
            profile,
            frames,
            kit,
            onlyKind,
            bus,
            nativeWorkspace,
            candidateId);
    blockSubAudibleDc(audio);
    matchAuditionLevel(audio, QStringLiteral("drums"));
    const QJsonObject metrics = drumAudioMetrics(audio);
    const std::vector<DrumHit> renderedHits =
        extractDrumHits(
            idea,
            frames,
            onlyKind && isTomKind(*onlyKind)
                ? onlyKind
                : std::nullopt);
    const QJsonArray hitAudit = realisedDrumHitAudit(
        renderedHits,
        audio,
        profile,
        candidateId,
        kit,
        onlyKind);
    if (!writeMonoPcm16(
            outputPath,
            std::move(audio),
            QStringLiteral("drums"))) {
        throw std::runtime_error(
            "Drum Kit Lab could not write its WAV.");
    }
    const int eventCount = onlyKind
        ? static_cast<int>(std::count_if(
              renderedHits.begin(),
              renderedHits.end(),
              [onlyKind](const DrumHit& hit) {
                  return hit.kind == *onlyKind;
              }))
        : static_cast<int>(renderedHits.size());
    QTextStream(stdout) << QJsonDocument(QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("schema"),
         QStringLiteral("jam2-instrument-render-result-v1")},
        {QStringLiteral("profileId"), profile.id},
        {QStringLiteral("role"), QStringLiteral("drums")},
        {QStringLiteral("audition"), audition},
        {QStringLiteral("drumPiece"),
         onlyKind ? drumKindId(*onlyKind) : QString()},
        {QStringLiteral("events"), eventCount},
        {QStringLiteral("injectedAuditionHit"),
         injectedAuditionHit},
        {QStringLiteral("productionPatternExact"),
         productionPatternExact},
        {QStringLiteral("patternSource"),
         productionPatternExact
             ? QStringLiteral("jam2-v7-performance")
             : QStringLiteral("diagnostic-audition-grid")},
        {QStringLiteral("patternSeed"),
         QString::number(idea.recipe.seed)},
        {QStringLiteral("patternFormId"),
         idea.recipe.formId},
        {QStringLiteral("fillEvents"), fillEvents},
        {QStringLiteral("microtimedEvents"),
         microtimedEvents},
        {QStringLiteral("frames"), static_cast<qint64>(frames)},
        {QStringLiteral("sampleRate"), kSampleRate},
        {QStringLiteral("candidateId"), candidateId},
        {QStringLiteral("metrics"), metrics},
        {QStringLiteral("realisedHits"), hitAudit},
        {QStringLiteral("kit"), kitObject},
    }).toJson(QJsonDocument::Compact) << "\n";
}

void renderInstrumentRequest(
    const QString& requestPath,
    const QString& outputPath)
{
    QFile requestFile(requestPath);
    if (!requestFile.open(QIODevice::ReadOnly) ||
        requestFile.size() <= 0 ||
        requestFile.size() > 65536) {
        throw std::runtime_error(
            "Instrument Lab request is missing or exceeds 64 KiB.");
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(requestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        throw std::runtime_error("Instrument Lab request is not valid JSON.");
    }
    const QJsonObject request = document.object();
    if (request.value(QStringLiteral("schema")).toString() !=
        QStringLiteral("jam2-instrument-patch-v1")) {
        throw std::runtime_error("Unsupported Instrument Lab request schema.");
    }
    const QString profileId =
        request.value(QStringLiteral("profileId")).toString();
    const ProfileDefinition* profile =
        jam2::practice::findProfile(profileId, true);
    if (!profile) {
        throw std::runtime_error("Instrument Lab profile is unknown.");
    }
    const QString role =
        request.value(QStringLiteral("role")).toString();
    if (role != QStringLiteral("chords") &&
        role != QStringLiteral("melody") &&
        role != QStringLiteral("bass") &&
        role != QStringLiteral("support") &&
        role != QStringLiteral("drums")) {
        throw std::runtime_error(
            "Instrument Lab role is unknown.");
    }
    if (role == QStringLiteral("drums")) {
        renderDrumKitRequest(request, *profile, outputPath);
        return;
    }
    const QString audition =
        request.value(QStringLiteral("audition")).toString();
    const int rootMidi = boundedPatchInteger(
        request,
        QStringLiteral("rootMidi"),
        role == QStringLiteral("bass") ? 40 : 60,
        24,
        96);
    if (!request.value(QStringLiteral("patch")).isObject()) {
        throw std::runtime_error("Instrument Lab patch must be an object.");
    }
    const QString patchId = patchIdForRole(*profile, role);
    const PatchDesign patch = patchFromJson(
        request.value(QStringLiteral("patch")).toObject(),
        patchFor(*profile, role, patchId));
    std::size_t frames = 0;
    std::optional<GeneratedPracticeIdea> generatedIdea;
    const std::vector<NoteEvent> events =
        laboratoryEvents(
            audition,
            role,
            rootMidi,
            *profile,
            frames,
            &generatedIdea);
    const bool primaryJam2 =
        patch.source == SourceKind::Jam2Native;
    const bool secondaryJam2 =
        patch.secondSourceEnabled &&
        patch.secondSource == SourceKind::Jam2Native;
    if ((primaryJam2 || secondaryJam2) && !generatedIdea) {
        throw std::runtime_error(
            "Jam2 native sources use the Generated style phrase audition.");
    }
    std::optional<StemSet> jam2;
    QTemporaryDir jam2Temporary;
    if (primaryJam2 || secondaryJam2) {
        if (!jam2Temporary.isValid()) {
            throw std::runtime_error(
                "Could not create temporary Jam2 render workspace.");
        }
        jam2 = renderJam2Stems(
            *generatedIdea,
            jam2Temporary.path());
    }
    const auto jam2Role = [&]() -> std::vector<float> {
        if (role == QStringLiteral("chords")) return jam2->chords;
        if (role == QStringLiteral("melody")) return jam2->melody;
        if (role == QStringLiteral("bass")) return jam2->bass;
        return jam2->support;
    };
    const bool blendSecond =
        patch.secondSourceEnabled &&
        patch.sourceBlend > 0.0001f;
    std::vector<float> audio = primaryJam2
        ? jam2Role()
        : renderDaisyRole(
              events,
              frames,
              role,
              patch,
              !blendSecond || secondaryJam2);
    audio.resize(frames, 0.0f);
    if (blendSecond) {
        PatchDesign second = patch;
        second.source = patch.secondSource;
        second.secondSourceEnabled = false;
        second.detuneCents = std::clamp(
            second.detuneCents + patch.secondSourceDetuneCents,
            -50.0f,
            50.0f);
        std::vector<NoteEvent> secondEvents = events;
        for (NoteEvent& event : secondEvents) {
            event.midi = std::clamp(
                event.midi + patch.secondSourceTranspose,
                0,
                127);
        }
        std::vector<float> secondary = secondaryJam2
            ? jam2Role()
            : renderDaisyRole(
                  secondEvents,
                  frames,
                  role,
                  second,
                  primaryJam2);
        secondary.resize(frames, 0.0f);
        const float blend =
            std::clamp(patch.sourceBlend, 0.0f, 1.0f);
        const float primaryGain = std::sqrt(1.0f - blend);
        const float secondaryGain = std::sqrt(blend);
        for (std::size_t frame = 0; frame < audio.size(); ++frame) {
            audio[frame] =
                primaryGain * audio[frame] +
                secondaryGain * secondary[frame];
        }
        if (!primaryJam2 && !secondaryJam2) {
            applyVoiceBus(audio, patch);
        }
    }
    blockSubAudibleDc(audio);
    if (!writeMonoPcm16(outputPath, std::move(audio), role)) {
        throw std::runtime_error(
            "Instrument Lab could not write its WAV.");
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("schema"),
         QStringLiteral("jam2-instrument-render-result-v1")},
        {QStringLiteral("profileId"), profileId},
        {QStringLiteral("role"), role},
        {QStringLiteral("audition"), audition},
        {QStringLiteral("events"), static_cast<int>(events.size())},
        {QStringLiteral("frames"), static_cast<qint64>(frames)},
        {QStringLiteral("sampleRate"), kSampleRate},
        {QStringLiteral("patch"), patchParameters(patch)},
    }).toJson(QJsonDocument::Compact) << "\n";
}

QJsonObject candidateJson(
    const QString& id,
    const QString& name,
    const QString& engine,
    const QString& model,
    const QString& path,
    const QString& rationale)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("engine"), engine},
        {QStringLiteral("model"), model},
        {QStringLiteral("path"), path},
        {QStringLiteral("rationale"), rationale},
    };
}

void ensureFrames(std::vector<float>& audio, std::size_t frames)
{
    audio.resize(frames, 0.0f);
}

struct HybridStemResult {
    std::vector<float> audio;
    int jamDelayFrames = 0;
    int daisyDelayFrames = 0;
};

HybridStemResult hybridStem(
    const std::vector<float>& jam2,
    const std::vector<float>& daisy,
    const ProfileDefinition& profile,
    const QString& role,
    const std::vector<NoteEvent>&)
{
    const std::size_t frames = std::max(jam2.size(), daisy.size());
    HybridStemResult result;
    result.audio.resize(frames, 0.0f);
    // Both engines are rendered from the same sample-accurate NoteEvents.
    // Their different attack envelopes are part of their timbres, not transport
    // latency. Moving either signal to align an energy landmark creates an
    // audible flam/delay in hybrids with a soft and a hard layer, so preserve
    // the shared event onset exactly.
    result.jamDelayFrames = 0;
    result.daisyDelayFrames = 0;
    const float daisyMix =
        profile.id == QStringLiteral("metal_modern_progressive") &&
        role == QStringLiteral("chords") ? 0.78f : 0.62f;
    const float jam2Mix = role == QStringLiteral("drums") ? 0.62f : 0.54f;
    double previous = 0.0;
    double jamLow = 0.0;
    double daisyLow = 0.0;
    const double bassCrossover =
        1.0 - std::exp(-2.0 * kPi * 185.0 / kSampleRate);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto delayedSample = [frame](
            const std::vector<float>& audio,
            int delay) {
            if (frame < static_cast<std::size_t>(delay)) return 0.0f;
            const std::size_t source =
                frame - static_cast<std::size_t>(delay);
            return source < audio.size() ? audio[source] : 0.0f;
        };
        const float left =
            delayedSample(jam2, result.jamDelayFrames);
        const float right =
            delayedSample(daisy, result.daisyDelayFrames);
        double value = 0.0;
        if (role == QStringLiteral("bass")) {
            jamLow += bassCrossover * (left - jamLow);
            daisyLow += bassCrossover * (right - daisyLow);
            const double daisyMid = right - daisyLow;
            value = 0.88 * jamLow + 0.72 * daisyMid;
        } else {
            value = jam2Mix * left + daisyMix * right;
        }
        if (role == QStringLiteral("drums")) {
            const double transient = value - previous;
            previous = value;
            value += 0.10 * transient;
        }
        result.audio[frame] =
            static_cast<float>(std::tanh(1.12 * value));
    }
    return result;
}

QJsonObject renderProfile(
    const ProfileDefinition& profile,
    const QDir& site,
    const QDir& temporary,
    QTextStream& console)
{
    console << "Rendering " << profile.name << "...\n";
    console.flush();
    ChordIdeaRequest request;
    request.styleId = profile.styleId;
    request.profileId = profile.id;
    if (!profile.forms.isEmpty()) {
        request.formId = profile.forms.first().id;
    }
    request.harmonicComplexity = 4;
    request.rhythmicComplexity = 4;
    const std::uint32_t seed = stableSeed(profile.id);
    GeneratedPracticeIdea idea =
        jam2::practice::generateCoupledPracticeIdeaForTest(
            request, seed);
    idea = trimIdea(std::move(idea));

    const QString temporaryProfile =
        temporary.absoluteFilePath(profile.id);
    QDir().mkpath(temporaryProfile);
    StemSet jam2 = renderJam2Stems(idea, temporaryProfile);
    std::size_t frames = 0;
    for (const char* roleValue : kRoleIds) {
        frames = std::max(
            frames,
            stemForRole(
                jam2, QString::fromLatin1(roleValue)).size());
    }
    if (frames == 0) {
        throw std::runtime_error(
            QStringLiteral("Profile %1 produced no audio.")
                .arg(profile.id)
                .toStdString());
    }
    ensureFrames(jam2.chords, frames);
    ensureFrames(jam2.melody, frames);
    ensureFrames(jam2.bass, frames);
    ensureFrames(jam2.support, frames);
    ensureFrames(jam2.drums, frames);
    const std::vector<NoteEvent> events = extractEvents(idea);
    const QString relativeRoot =
        QStringLiteral("audio/designs/") + profile.id;
    const QDir profileAudio(site.absoluteFilePath(relativeRoot));
    if (!profileAudio.exists() && !site.mkpath(relativeRoot)) {
        throw std::runtime_error("Cannot create profile audio folder.");
    }

    QJsonArray roles;
    for (const char* roleValue : kRoleIds) {
        const QString role = QString::fromLatin1(roleValue);
        if (role != QStringLiteral("chords") &&
            role != QStringLiteral("drums") &&
            !hasLane(idea, role)) {
            continue;
        }
        const QString patchId = patchIdForRole(profile, role);
        PatchDesign patch;
        std::vector<float> daisy;
        QJsonObject productionDrumParity;
        if (role == QStringLiteral("drums")) {
            const std::vector<ResearchKitCandidate> drumCandidates =
                researchKitCandidates(profile);
            const auto selected = std::find_if(
                drumCandidates.begin(),
                drumCandidates.end(),
                [](const ResearchKitCandidate& candidate) {
                    return candidate.recommended;
                });
            const ResearchKitCandidate candidate =
                selected != drumCandidates.end()
                    ? *selected : drumCandidates.front();
            patch.name =
                candidate.name +
                QStringLiteral(" researched component kit");
            QMap<DrumKind, DrumLabPatch> kit;
            for (DrumKind kind : kDrumKinds) {
                kit.insert(
                    kind,
                    candidatePiecePatch(
                        profile, candidate, kind));
            }
            const QString drumWorkspace =
                QDir(temporaryProfile).absoluteFilePath(
                    QStringLiteral("researched-drums"));
            QDir().mkpath(drumWorkspace);
            daisy = renderLaboratoryKit(
                idea,
                profile,
                frames,
                kit,
                std::nullopt,
                candidateBusDesign(profile, candidate),
                drumWorkspace,
                candidate.id);
            const ResearchDrumKit* productionKit =
                jam2::practice::researchDrumKitForProfile(
                    profile.id);
            const bool hasNativePiece =
                productionKit &&
                std::any_of(
                    productionKit->pieces.cbegin(),
                    productionKit->pieces.cend(),
                    [](const ResearchDrumPiece& piece) {
                        return piece.source ==
                                QStringLiteral("jam2-native") ||
                            piece.secondSource ==
                                QStringLiteral("jam2-native");
                    });
            if (productionKit && !hasNativePiece) {
                QVector<ResearchDrumRenderEvent> productionEvents;
                const std::vector<DrumHit> drumHits =
                    extractDrumHits(idea, frames);
                productionEvents.reserve(
                    static_cast<qsizetype>(drumHits.size()));
                for (const DrumHit& sourceHit : drumHits) {
                    const DrumLabPatch acceptedPatch =
                        kit.value(
                            sourceHit.kind,
                            defaultDrumLabPatch(
                                profile,
                                sourceHit.kind));
                    const DrumHit hit = realiseDrumHit(
                        sourceHit,
                        profile,
                        candidate.id,
                        acceptedPatch);
                    productionEvents.push_back({
                        hit.frame,
                        drumKindId(hit.kind),
                        !hit.articulation.isEmpty()
                            ? hit.articulation
                            : hit.strength ==
                                    DrumHit::Strength::Ghost
                                ? QStringLiteral("ghost")
                                : hit.strength ==
                                        DrumHit::Strength::Accent
                                    ? QStringLiteral("accent")
                                    : QStringLiteral("normal"),
                        hit.midiVelocity,
                        hit.repeatIndex,
                        stableSeed(
                            QString::number(hit.frame) +
                            drumKindId(hit.kind)),
                    });
                }
                ResearchDrumRenderResult promoted =
                    jam2::practice::renderResearchDrumVoices(
                        *productionKit,
                        productionEvents,
                        static_cast<qint64>(frames),
                        static_cast<int>(kSampleRate));
                jam2::practice::applyResearchDrumBus(
                    promoted.dry,
                    promoted.roomSend,
                    productionKit->bus,
                    static_cast<int>(kSampleRate));
                double errorSquares = 0.0;
                double referenceSquares = 0.0;
                double maximumError = 0.0;
                for (std::size_t frame = 0;
                     frame < frames;
                     ++frame) {
                    const double reference = daisy[frame];
                    const double promotedSample =
                        promoted.dry.at(
                            static_cast<qsizetype>(frame));
                    const double difference =
                        promotedSample - reference;
                    errorSquares += difference * difference;
                    referenceSquares += reference * reference;
                    maximumError = std::max(
                        maximumError,
                        std::abs(difference));
                }
                const double rmsError = std::sqrt(
                    errorSquares /
                    std::max<std::size_t>(1, frames));
                const double referenceRms = std::sqrt(
                    referenceSquares /
                    std::max<std::size_t>(1, frames));
                productionDrumParity = {
                    {QStringLiteral("comparable"), true},
                    {QStringLiteral("rmsError"), rmsError},
                    {QStringLiteral("maximumAbsoluteError"),
                     maximumError},
                    {QStringLiteral("referenceRms"), referenceRms},
                    {QStringLiteral("normalizedRmsError"),
                     referenceRms > 1.0e-12
                        ? rmsError / referenceRms
                        : 0.0},
                };
            } else {
                productionDrumParity = {
                    {QStringLiteral("comparable"), false},
                    {QStringLiteral("reason"),
                     hasNativePiece
                        ? QStringLiteral(
                              "accepted kit retains Jam2-native lanes")
                        : QStringLiteral(
                              "production researched kit missing")},
                };
            }
        } else {
            patch = patchFor(profile, role, patchId);
            daisy = renderDaisyRole(
                events, frames, role, patch);
        }
        blockSubAudibleDc(daisy);
        std::vector<float> jam = stemForRole(jam2, role);
        HybridStemResult hybrid =
            hybridStem(jam, daisy, profile, role, events);
        const QString jamName = role + QStringLiteral("-jam2.wav");
        const QString daisyName = role + QStringLiteral("-daisy.wav");
        const QString hybridName =
            role + QStringLiteral("-hybrid.wav");
        if (!writeMonoPcm16(
                profileAudio.absoluteFilePath(jamName), jam, role) ||
            !writeMonoPcm16(
                profileAudio.absoluteFilePath(daisyName), daisy, role) ||
            !writeMonoPcm16(
                profileAudio.absoluteFilePath(hybridName),
                hybrid.audio,
                role)) {
            throw std::runtime_error(
                QStringLiteral("Cannot write %1 sound candidates.")
                    .arg(profile.id)
                    .toStdString());
        }
        const QString rolePath = relativeRoot + QStringLiteral("/");
        const QString target = role == QStringLiteral("drums")
            ? QStringLiteral(
                "The generated groove is unchanged; only drum voice construction "
                "and bus character differ.")
            : QStringLiteral(
                "The same generated notes, velocities, durations and articulation "
                "drive all three candidates.");
        QJsonObject parameters =
            role == QStringLiteral("drums")
                ? [&]() {
                      QJsonObject result =
                          drumKitParameters(profile);
                      result.insert(
                          QStringLiteral("engine"),
                          QStringLiteral("editable-jam2-daisy-kit"));
                      result.insert(
                          QStringLiteral("profile"),
                          profile.id);
                      return result;
                  }()
                : patchParameters(patch);
        parameters.insert(
            QStringLiteral("hybridAlignment"),
            QJsonObject{
                {QStringLiteral("method"),
                     QStringLiteral("shared-event-onset-no-delay")},
                {QStringLiteral("maximumCorrectionMs"), 0.0},
                {QStringLiteral("jam2DelayMs"),
                 1000.0 * hybrid.jamDelayFrames / kSampleRate},
                {QStringLiteral("daisyDelayMs"),
                 1000.0 * hybrid.daisyDelayFrames / kSampleRate},
                {QStringLiteral("bassCombination"),
                 role == QStringLiteral("bass")
                     ? QStringLiteral(
                           "Jam2 low fundamental below 185 Hz plus "
                           "Daisy mid/high colour")
                     : QStringLiteral("aligned parallel blend")},
            });
        if (role == QStringLiteral("drums")) {
            parameters.insert(
                QStringLiteral("productionParity"),
                productionDrumParity);
        }
        roles.append(QJsonObject{
            {QStringLiteral("id"), role},
            {QStringLiteral("name"), roleName(role)},
            {QStringLiteral("targetPatchId"), patchId},
            {QStringLiteral("designName"), patch.name},
            {QStringLiteral("auditionPolicy"), target},
            {QStringLiteral("parameters"), parameters},
            {QStringLiteral("kitCandidates"),
             role == QStringLiteral("drums")
                 ? drumKitCandidatesJson(profile)
                 : QJsonArray{}},
            {QStringLiteral("defaults"), QJsonObject{
                 {QStringLiteral("candidate"), QStringLiteral("daisy")},
                 {QStringLiteral("level"),
                  role == QStringLiteral("support") ? 68 : 82},
                 {QStringLiteral("tone"), 100},
                 {QStringLiteral("drive"), 0},
                 {QStringLiteral("space"), 0},
             }},
            {QStringLiteral("candidates"), QJsonArray{
                 candidateJson(
                     QStringLiteral("jam2"),
                     QStringLiteral("Jam2 baseline"),
                     QStringLiteral("Jam2"),
                     QStringLiteral("Current compact renderer / %1")
                         .arg(patchId),
                     rolePath + jamName,
                     QStringLiteral(
                         "The exact current Jam2 synthesis is retained as the "
                         "reference rather than assumed to be inferior.")),
                 candidateJson(
                     QStringLiteral("daisy"),
                     QStringLiteral("Daisy design"),
                     QStringLiteral("DaisySP"),
                     patch.name,
                     rolePath + daisyName,
                     QStringLiteral(
                         "A fixed profile-aware voice built from Daisy modules, "
                         "Jam2 articulation and the research timbre target.")),
                 candidateJson(
                     QStringLiteral("hybrid"),
                     QStringLiteral("Jam2 + Daisy hybrid"),
                     QStringLiteral("Hybrid"),
                     QStringLiteral("Jam2 body/transient blended with %1")
                         .arg(patch.name),
                     rolePath + hybridName,
                     QStringLiteral(
                         "Tests whether the current Jam2 character and Daisy "
                         "spectral/articulation design complement one another.")),
             }},
        });
    }

    return {
        {QStringLiteral("id"), profile.id},
        {QStringLiteral("styleId"), profile.styleId},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("experimental"), profile.experimental},
        {QStringLiteral("bpm"), idea.bpm},
        {QStringLiteral("meter"),
         QStringLiteral("%1/%2")
             .arg(idea.meterNumerator)
             .arg(idea.meterDenominator)},
        {QStringLiteral("bars"), kAuditionBars},
        {QStringLiteral("seed"), QString::number(seed)},
        {QStringLiteral("teachingSummary"), profile.teachingSummary},
        {QStringLiteral("soundBrief"), styleSoundBrief(profile.styleId)},
        {QStringLiteral("limitations"), styleSoundLimits(profile.styleId)},
        {QStringLiteral("roles"), roles},
    };
}

QJsonArray styleManifest()
{
    QJsonArray styles;
    for (const StyleDefinition& style :
         jam2::practice::styleCatalog()) {
        QJsonArray profileIds;
        for (const QString& id : style.profileIds) {
            profileIds.append(id);
        }
        styles.append(QJsonObject{
            {QStringLiteral("id"), style.id},
            {QStringLiteral("name"), style.name},
            {QStringLiteral("summary"), style.summary},
            {QStringLiteral("soundBrief"),
             styleSoundBrief(style.id)},
            {QStringLiteral("profileIds"), profileIds},
        });
    }
    styles.append(QJsonObject{
        {QStringLiteral("id"),
         QStringLiteral("metal-experimental")},
        {QStringLiteral("name"),
         QStringLiteral("Modern Metal / Experimental")},
        {QStringLiteral("summary"),
         QStringLiteral(
             "Narrow modern progressive metalcore sound and articulation lab.")},
        {QStringLiteral("soundBrief"),
         styleSoundBrief(QStringLiteral("metal-experimental"))},
        {QStringLiteral("profileIds"), QJsonArray{
             QStringLiteral("metal_modern_progressive")}},
    });
    return styles;
}

void copyDaisyLicence(const QDir& site)
{
    const QString source =
        QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
            .absoluteFilePath(
                QStringLiteral("../../../libs/third_party/DaisySP/LICENSE"));
    const QString folder =
        site.absoluteFilePath(QStringLiteral("licenses"));
    QDir().mkpath(folder);
    const QString destination =
        QDir(folder).absoluteFilePath(
            QStringLiteral("DaisySP-LICENSE.txt"));
    QFile::remove(destination);
    if (!QFile::copy(source, destination)) {
        throw std::runtime_error("Cannot copy DaisySP licence.");
    }
}

void renderSoundDesignCatalog(const QDir& site)
{
    if (!QDir().mkpath(site.absolutePath()) ||
        !QDir().mkpath(
            site.absoluteFilePath(QStringLiteral("audio")))) {
        throw std::runtime_error(
            "Cannot create sound-design output folders.");
    }
    const QString designPath =
        site.absoluteFilePath(QStringLiteral("audio/designs"));
    QDir designFolder(designPath);
    if (designFolder.exists() &&
        !designFolder.removeRecursively()) {
        throw std::runtime_error(
            "Cannot replace generated design audio.");
    }
    if (!QDir().mkpath(designPath)) {
        throw std::runtime_error(
            "Cannot create generated design audio folder.");
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        throw std::runtime_error(
            "Cannot create temporary render folder.");
    }
    QTextStream console(stdout);
    QJsonArray profiles;
    for (const ProfileDefinition& profile :
         jam2::practice::profileCatalog(true)) {
        profiles.append(renderProfile(
            profile,
            site,
            QDir(temporary.path()),
            console));
    }
    const QJsonObject manifest{
        {QStringLiteral("version"), 1},
        {QStringLiteral("generatedAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("sampleRate"), kSampleRate},
        {QStringLiteral("auditionBars"), kAuditionBars},
        {QStringLiteral("styles"), styleManifest()},
        {QStringLiteral("profiles"), profiles},
    };
    QFile file(
        site.absoluteFilePath(
            QStringLiteral("sound-design-manifest.js")));
    if (!file.open(
            QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(
            "Cannot write sound-design-manifest.js.");
    }
    file.write("window.JAM2_SOUND_DESIGN_MANIFEST = ");
    file.write(
        QJsonDocument(manifest).toJson(
            QJsonDocument::Compact));
    file.write(";\n");
    copyDaisyLicence(site);
    console << "Rendered " << profiles.size()
            << " research-profile sound-design scenes to "
            << site.absolutePath() << "\n";
}

void verifyProductionDrumParity(const QDir& site)
{
    QJsonArray profilesJson;
    bool allComparablePassed = true;
    const QVector<ProfileDefinition>& profiles =
        jam2::practice::profileCatalog(true);
    for (const ProfileDefinition& profile : profiles) {
        const std::vector<ResearchKitCandidate> candidates =
            researchKitCandidates(profile);
        const auto selected = std::find_if(
            candidates.begin(),
            candidates.end(),
            [](const ResearchKitCandidate& candidate) {
                return candidate.recommended;
            });
        const ResearchKitCandidate candidate =
            selected != candidates.end()
            ? *selected
            : candidates.front();
        QMap<DrumKind, DrumLabPatch> kit;
        for (DrumKind kind : kDrumKinds) {
            kit.insert(
                kind,
                candidatePiecePatch(profile, candidate, kind));
        }
        const ResearchDrumKit* productionKit =
            jam2::practice::researchDrumKitForProfile(profile.id);
        QJsonArray piecesJson;
        for (DrumKind kind : kDrumKinds) {
            QJsonObject pieceJson{
                {QStringLiteral("piece"), drumKindId(kind)},
            };
            const ResearchDrumPiece* productionPiece =
                productionKit
                ? jam2::practice::researchDrumPiece(
                      *productionKit,
                      drumKindId(kind))
                : nullptr;
            if (!productionKit || !productionPiece) {
                pieceJson.insert(
                    QStringLiteral("comparable"),
                    false);
                pieceJson.insert(
                    QStringLiteral("reason"),
                    QStringLiteral("production piece missing"));
                allComparablePassed = false;
                piecesJson.append(pieceJson);
                continue;
            }
            if (productionPiece->source ==
                    QStringLiteral("jam2-native") ||
                productionPiece->secondSource ==
                    QStringLiteral("jam2-native")) {
                pieceJson.insert(
                    QStringLiteral("comparable"),
                    false);
                pieceJson.insert(
                    QStringLiteral("reason"),
                    QStringLiteral("Jam2-native lane"));
                piecesJson.append(pieceJson);
                continue;
            }

            ChordIdeaRequest request;
            request.styleId = profile.styleId;
            request.profileId = profile.id;
            if (!profile.forms.isEmpty()) {
                request.formId = profile.forms.first().id;
            }
            request.harmonicComplexity = 4;
            request.rhythmicComplexity = 4;
            GeneratedPracticeIdea idea =
                jam2::practice::generateCoupledPracticeIdeaForTest(
                    request,
                    stableSeed(profile.id));
            prepareDrumOneShot(idea, kind);
            const std::size_t frames =
                static_cast<std::size_t>(3 * kSampleRate);
            QTemporaryDir temporary;
            std::vector<float> laboratory =
                renderLaboratoryKit(
                    idea,
                    profile,
                    frames,
                    kit,
                    kind,
                    candidateBusDesign(profile, candidate),
                    temporary.path(),
                    candidate.id);
            const std::vector<DrumHit> sourceHits =
                extractDrumHits(
                    idea,
                    frames,
                    isTomKind(kind)
                        ? std::optional<DrumKind>(kind)
                        : std::nullopt);
            QVector<ResearchDrumRenderEvent> productionEvents;
            for (const DrumHit& sourceHit : sourceHits) {
                if (sourceHit.kind != kind) continue;
                const DrumHit hit = realiseDrumHit(
                    sourceHit,
                    profile,
                    candidate.id,
                    kit.value(kind));
                productionEvents.push_back({
                    hit.frame,
                    drumKindId(kind),
                    !hit.articulation.isEmpty()
                        ? hit.articulation
                        : hit.strength ==
                                DrumHit::Strength::Ghost
                            ? QStringLiteral("ghost")
                            : hit.strength ==
                                    DrumHit::Strength::Accent
                                ? QStringLiteral("accent")
                                : QStringLiteral("normal"),
                    hit.midiVelocity,
                    hit.repeatIndex,
                    stableSeed(
                        QString::number(hit.frame) +
                        drumKindId(kind)),
                });
            }
            ResearchDrumRenderResult production =
                jam2::practice::renderResearchDrumVoices(
                    *productionKit,
                    productionEvents,
                    static_cast<qint64>(frames),
                    kSampleRate);
            jam2::practice::applyResearchDrumBus(
                production.dry,
                production.roomSend,
                productionKit->bus,
                kSampleRate);
            double errorSquares = 0.0;
            double referenceSquares = 0.0;
            double maximumError = 0.0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const double reference = laboratory[frame];
                const double difference =
                    production.dry.at(
                        static_cast<qsizetype>(frame)) -
                    reference;
                errorSquares += difference * difference;
                referenceSquares += reference * reference;
                maximumError = std::max(
                    maximumError,
                    std::abs(difference));
            }
            const double rmsError =
                std::sqrt(errorSquares / frames);
            const double referenceRms =
                std::sqrt(referenceSquares / frames);
            const double normalized =
                referenceRms > 1.0e-12
                ? rmsError / referenceRms
                : 0.0;
            // Both paths execute the same renderer. The remaining sub-0.2%
            // residual is limited to float JSON round-tripping between the
            // editable Lab candidate and its embedded production catalogue.
            const bool passed = normalized <= 0.002;
            allComparablePassed =
                allComparablePassed && passed;
            pieceJson.insert(
                QStringLiteral("comparable"),
                true);
            pieceJson.insert(
                QStringLiteral("passed"),
                passed);
            pieceJson.insert(
                QStringLiteral("normalizedRmsError"),
                normalized);
            pieceJson.insert(
                QStringLiteral("maximumAbsoluteError"),
                maximumError);
            pieceJson.insert(
                QStringLiteral("referenceRms"),
                referenceRms);
            piecesJson.append(pieceJson);
        }
        profilesJson.append(QJsonObject{
            {QStringLiteral("profileId"), profile.id},
            {QStringLiteral("candidateId"), candidate.id},
            {QStringLiteral("pieces"), piecesJson},
        });
    }
    const QJsonDocument report(QJsonObject{
        {QStringLiteral("schema"),
         QStringLiteral("jam2-drum-production-parity-v1")},
        {QStringLiteral("ok"), allComparablePassed},
        {QStringLiteral("profiles"), profilesJson},
    });
    QSaveFile reportFile(
        site.absoluteFilePath(
            QStringLiteral("drum-production-parity.json")));
    if (!reportFile.open(QIODevice::WriteOnly) ||
        reportFile.write(report.toJson(QJsonDocument::Indented)) < 0 ||
        !reportFile.commit()) {
        throw std::runtime_error(
            "Could not write promoted drum parity evidence.");
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{
        {QStringLiteral("ok"), allComparablePassed},
        {QStringLiteral("schema"),
         QStringLiteral("jam2-drum-production-parity-v1")},
        {QStringLiteral("report"),
         reportFile.fileName()},
    }).toJson(QJsonDocument::Compact) << "\n";
    if (!allComparablePassed) {
        throw std::runtime_error(
            "Promoted drum engine parity verification failed.");
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTextStream error(stderr);
    try {
        const QStringList arguments = application.arguments();
        if (arguments.size() == 4 &&
            arguments.at(1) ==
                QStringLiteral("--render-instrument")) {
            renderInstrumentRequest(
                QFileInfo(arguments.at(2)).absoluteFilePath(),
                QFileInfo(arguments.at(3)).absoluteFilePath());
            return 0;
        }
        const QString output = arguments.size() > 1
            ? QDir(arguments.at(1)).absolutePath()
            : QDir(QCoreApplication::applicationDirPath())
                  .absoluteFilePath(QStringLiteral("../site"));
        const QDir site(output);
        if (!site.exists() && !QDir().mkpath(output)) {
            throw std::runtime_error(
                "Cannot create experiment output directory.");
        }
        if (arguments.contains(
                QStringLiteral("--verify-drum-parity"))) {
            verifyProductionDrumParity(site);
            return 0;
        }
        const bool showcaseOnly =
            arguments.contains(
                QStringLiteral("--showcase-only"));
        const bool catalogOnly =
            arguments.contains(
                QStringLiteral("--catalog-only"));
        const bool auditOnly =
            arguments.contains(
                QStringLiteral("--audit-only"));
        if (auditOnly) {
            writeSeedAudit(site);
            return 0;
        }
        if (!showcaseOnly) renderSoundDesignCatalog(site);
        if (!catalogOnly) {
            jam2::experiment::renderDaisyShowcase(site);
        }
        return 0;
    } catch (const std::exception& exception) {
        error << "Jam2 sound lab failed: "
              << exception.what() << "\n";
        return 1;
    }
}
