#include "PracticeIdeaGenerator.hpp"

#include "MusicTheory.hpp"

#include <QHash>
#include <QVector>

#include <algorithm>
#include <array>
#include <random>

namespace jam2::practice {
namespace {

using Rng = std::mt19937;

template <typename T>
const T& choose(const QVector<T>& values, Rng& rng)
{
    return values.at(std::uniform_int_distribution<int>(0, values.size() - 1)(rng));
}

int resolveIndex(int requested, int count, Rng& rng)
{
    return requested >= 0 && requested < count
        ? requested : std::uniform_int_distribution<int>(0, count - 1)(rng);
}

int resolveBars(int requested, int style, bool beat, Rng& rng)
{
    if (requested > 0) return requested;
    QVector<int> choices{2, 4, 8};
    if ((!beat && style == static_cast<int>(ChordStyle::BluesForm)) ||
        (beat && style == static_cast<int>(BeatStyle::BluesShuffle))) {
        choices = {4, 8, 12};
    } else if (!beat && style == static_cast<int>(ChordStyle::JazzTurnaround)) {
        choices = {4, 8};
    }
    return choose(choices, rng);
}

QString resolveCharacter(const QString& requested, const QStringList& values, Rng& rng)
{
    return values.contains(requested)
        ? requested : values.at(std::uniform_int_distribution<int>(0, values.size() - 1)(rng));
}

bool preferFlatsForKey(int key)
{
    return key == 1 || key == 3 || key == 5 || key == 8 || key == 10;
}

QString diatonicChord(int key, int degree, bool minorKey, bool seventh, bool flats)
{
    static constexpr std::array<int, 7> majorSteps{0, 2, 4, 5, 7, 9, 11};
    static constexpr std::array<int, 7> minorSteps{0, 2, 3, 5, 7, 8, 10};
    static const std::array<QString, 7> majorTriads{
        QString(), QStringLiteral("m"), QStringLiteral("m"), QString(),
        QString(), QStringLiteral("m"), QStringLiteral("dim")};
    static const std::array<QString, 7> minorTriads{
        QStringLiteral("m"), QStringLiteral("dim"), QString(), QStringLiteral("m"),
        QStringLiteral("m"), QString(), QString()};
    static const std::array<QString, 7> majorSevenths{
        QStringLiteral("maj7"), QStringLiteral("m7"), QStringLiteral("m7"), QStringLiteral("maj7"),
        QStringLiteral("7"), QStringLiteral("m7"), QStringLiteral("m7b5")};
    static const std::array<QString, 7> minorSevenths{
        QStringLiteral("m7"), QStringLiteral("m7b5"), QStringLiteral("maj7"), QStringLiteral("m7"),
        QStringLiteral("m7"), QStringLiteral("maj7"), QStringLiteral("7")};
    const int index = ((degree % 7) + 7) % 7;
    const auto& steps = minorKey ? minorSteps : majorSteps;
    const QString suffix = seventh
        ? (minorKey ? minorSevenths : majorSevenths).at(index)
        : (minorKey ? minorTriads : majorTriads).at(index);
    return chordSymbol(key + steps.at(index), suffix, flats);
}

void placeChord(SongSection& section, int beat, const QString& chord)
{
    if (beat < 0 || beat >= section.beats) return;
    section.chords[beat] = chord;
}

int nearestMidiForPitchClass(int pitchClass, int previousMidi)
{
    const int center = previousMidi >= 0 ? previousMidi : 67;
    int best = 60 + ((pitchClass - 0 + 12) % 12);
    int bestDistance = std::abs(best - center);
    for (int midi = pitchClass; midi <= 127; midi += 12) {
        if (midi < 55 || midi > 79) continue;
        const int distance = std::abs(midi - center);
        if (distance < bestDistance) {
            best = midi;
            bestDistance = distance;
        }
    }
    return best;
}

enum class ScaleKind {
    Major,
    NaturalMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    Blues,
    Altered,
    WholeTone,
    Diminished,
};

enum class HarmonicFunction {
    Tonic,
    Predominant,
    Dominant,
    Passing,
    Pedal,
    Colour,
};

struct HarmonicEvent {
    int beat = 0;
    int durationBeats = 1;
    QString chord;
    int localTonic = 0;
    ScaleKind scale = ScaleKind::Major;
    HarmonicFunction function = HarmonicFunction::Tonic;
    int tension = 1;
    int resolutionEvent = -1;
};

struct HarmonicPlan {
    QVector<HarmonicEvent> events;
};

QVector<int> scaleIntervals(ScaleKind scale)
{
    switch (scale) {
    case ScaleKind::Major: return {0, 2, 4, 5, 7, 9, 11};
    case ScaleKind::NaturalMinor: return {0, 2, 3, 5, 7, 8, 10};
    case ScaleKind::Dorian: return {0, 2, 3, 5, 7, 9, 10};
    case ScaleKind::Phrygian: return {0, 1, 3, 5, 7, 8, 10};
    case ScaleKind::Lydian: return {0, 2, 4, 6, 7, 9, 11};
    case ScaleKind::Mixolydian: return {0, 2, 4, 5, 7, 9, 10};
    case ScaleKind::Locrian: return {0, 1, 3, 5, 6, 8, 10};
    case ScaleKind::Blues: return {0, 3, 5, 6, 7, 10};
    case ScaleKind::Altered: return {0, 1, 3, 4, 6, 8, 10};
    case ScaleKind::WholeTone: return {0, 2, 4, 6, 8, 10};
    case ScaleKind::Diminished: return {0, 1, 3, 4, 6, 7, 9, 10};
    }
    return {0, 2, 4, 5, 7, 9, 11};
}

ScaleKind scaleForCharacter(int style, bool minor, const QString& character)
{
    if (style == static_cast<int>(ChordStyle::BluesForm)) return ScaleKind::Blues;
    if (character == QStringLiteral("Dorian")) return ScaleKind::Dorian;
    if (character == QStringLiteral("Phrygian")) return ScaleKind::Phrygian;
    if (character == QStringLiteral("Lydian")) return ScaleKind::Lydian;
    if (character == QStringLiteral("Mixolydian")) return ScaleKind::Mixolydian;
    if (character == QStringLiteral("Locrian")) return ScaleKind::Locrian;
    if (character == QStringLiteral("Ionian")) return ScaleKind::Major;
    return minor ? ScaleKind::NaturalMinor : ScaleKind::Major;
}

int diatonicPitch(int key, int degree, bool minor)
{
    static constexpr std::array<int, 7> majorSteps{0, 2, 4, 5, 7, 9, 11};
    static constexpr std::array<int, 7> minorSteps{0, 2, 3, 5, 7, 8, 10};
    const int index = ((degree % 7) + 7) % 7;
    return key + (minor ? minorSteps : majorSteps).at(index);
}

void setHarmonicEvent(
    HarmonicPlan& plan,
    int beat,
    const QString& chord,
    int localTonic,
    ScaleKind scale,
    HarmonicFunction function,
    int tension)
{
    HarmonicEvent event;
    event.beat = beat;
    event.chord = chord;
    event.localTonic = ((localTonic % 12) + 12) % 12;
    event.scale = scale;
    event.function = function;
    event.tension = std::clamp(tension, 1, 8);
    for (HarmonicEvent& existing : plan.events) {
        if (existing.beat == beat) {
            existing = std::move(event);
            return;
        }
    }
    plan.events.push_back(std::move(event));
}

void finishHarmonicPlan(HarmonicPlan& plan, int totalBeats)
{
    std::sort(plan.events.begin(), plan.events.end(),
        [](const HarmonicEvent& a, const HarmonicEvent& b) { return a.beat < b.beat; });
    for (int index = 0; index < plan.events.size(); ++index) {
        HarmonicEvent& event = plan.events[index];
        const int endBeat = index + 1 < plan.events.size()
            ? plan.events[index + 1].beat : totalBeats;
        event.durationBeats = qMax(1, endBeat - event.beat);
        if (index + 1 < plan.events.size() &&
            (event.function == HarmonicFunction::Dominant ||
             event.function == HarmonicFunction::Passing ||
             event.function == HarmonicFunction::Colour)) {
            event.resolutionEvent = index + 1;
        }
    }
}

QString invertedChord(const QString& chord, int interval, bool flats)
{
    const ParsedChord parsed = parseChord(chord);
    if (!parsed.valid || parsed.rest) return chord;
    return chord + QLatin1Char('/') + noteName(parsed.root + interval, flats);
}

HarmonicPlan buildHarmonicPlan(
    int key,
    int style,
    const QString& character,
    int bars,
    int beatsPerBar,
    int complexity,
    bool minor,
    bool flats,
    Rng& rng)
{
    HarmonicPlan plan;
    const ScaleKind homeScale = scaleForCharacter(style, minor, character);
    auto barBeat = [beatsPerBar](int bar) { return bar * beatsPerBar; };
    auto add = [&](int beat, int root, const QString& suffix, int localTonic,
                   ScaleKind scale, HarmonicFunction function, int tension) {
        setHarmonicEvent(plan, beat, chordSymbol(root, suffix, flats),
            localTonic, scale, function, tension);
    };
    auto addDegree = [&](int beat, int degree, bool seventh,
                         HarmonicFunction function, int tension) {
        setHarmonicEvent(plan, beat,
            diatonicChord(key, degree, minor, seventh, flats),
            key, homeScale, function, tension);
    };

    if (style == static_cast<int>(ChordStyle::FunctionalPop)) {
        if (complexity == 8) {
            const bool thirds = std::uniform_int_distribution<int>(0, 1)(rng) == 0;
            const QVector<int> roots = thirds
                ? QVector<int>{0, 4, 8, 0}
                : QVector<int>{0, 2, 5, 3};
            for (int bar = 0; bar < bars; ++bar) {
                const int offset = bar == bars - 1 ? 0 : roots.at(bar % roots.size());
                add(barBeat(bar), key + offset,
                    thirds ? QStringLiteral("maj9") : QStringLiteral("maj9#11"),
                    key + offset, thirds ? ScaleKind::Major : ScaleKind::Lydian,
                    bar == bars - 1 ? HarmonicFunction::Tonic : HarmonicFunction::Colour, 8);
            }
        } else if (complexity == 7) {
            const QVector<int> roots{0, 8, 9, 1};
            for (int bar = 0; bar < bars; ++bar) {
                const int offset = bar == bars - 1 ? 0 : roots.at(bar % roots.size());
                add(barBeat(bar), key + offset, QStringLiteral("maj9"),
                    key + offset, ScaleKind::Major,
                    bar == bars - 1 ? HarmonicFunction::Tonic : HarmonicFunction::Colour, 7);
            }
        } else {
            const QVector<int> grounded{0, 3, 4, 0};
            const QVector<int> diatonic{0, 5, 1, 4};
            const QVector<int>& degrees = complexity == 1 ? grounded : diatonic;
            for (int bar = 0; bar < bars; ++bar) {
                const int degree = bar == bars - 1 ? 0 : degrees.at(bar % degrees.size());
                addDegree(barBeat(bar), degree, complexity >= 3,
                    degree == 0 || degree == 5 ? HarmonicFunction::Tonic :
                    degree == 4 ? HarmonicFunction::Dominant : HarmonicFunction::Predominant,
                    qMin(complexity, 3));
                if (complexity >= 3 && bar % 3 == 1) {
                    HarmonicEvent& event = plan.events.back();
                    const ParsedChord parsed = parseChord(event.chord);
                    if (parsed.valid && parsed.intervals.size() >= 3) {
                        event.chord = invertedChord(event.chord, parsed.intervals.at(1), flats);
                    }
                }
            }
            if (complexity >= 4 && bars >= 4) {
                add(barBeat(qMax(1, bars - 2)), key + 5, QStringLiteral("m6"),
                    key, homeScale, HarmonicFunction::Colour, 4);
            }
            if (complexity >= 5 && bars >= 4) {
                setHarmonicEvent(plan, qMax(1, beatsPerBar / 2),
                    chordSymbol(key + 4, QStringLiteral("7"), flats),
                    key + 9, ScaleKind::NaturalMinor, HarmonicFunction::Dominant, 5);
                setHarmonicEvent(plan, barBeat(1),
                    chordSymbol(key + 9, QStringLiteral("m7"), flats),
                    key + 9, ScaleKind::NaturalMinor, HarmonicFunction::Tonic, 4);
            }
            if (complexity >= 6 && bars >= 8) {
                const int excursion = bars / 2;
                add(barBeat(excursion), key + 7, QStringLiteral("maj9"),
                    key + 7, ScaleKind::Major, HarmonicFunction::Tonic, 6);
                add(barBeat(excursion + 1), key + 2, QStringLiteral("9"),
                    key + 7, ScaleKind::Mixolydian, HarmonicFunction::Dominant, 6);
                add(barBeat(qMin(bars - 2, excursion + 2)), key + 7, QStringLiteral("13"),
                    key, homeScale, HarmonicFunction::Dominant, 6);
            }
        }
    } else if (style == static_cast<int>(ChordStyle::BluesForm)) {
        const QVector<int> form{0,0,0,0, 5,5,0,0, 7,5,0,7};
        for (int bar = 0; bar < bars; ++bar) {
            const int offset = bar == bars - 1 ? 0 : form.at(bar % form.size());
            QString suffix;
            if (complexity >= 6) suffix = offset == 7 ? QStringLiteral("13") : QStringLiteral("9");
            else if (complexity >= 3) suffix = QStringLiteral("7");
            add(barBeat(bar), key + offset, suffix, key, ScaleKind::Blues,
                offset == 0 ? HarmonicFunction::Tonic :
                offset == 7 ? HarmonicFunction::Dominant : HarmonicFunction::Predominant,
                qMin(complexity, 6));
        }
        if (complexity >= 4 && bars >= 4) {
            add(barBeat(1), key + 5, QStringLiteral("7"),
                key, ScaleKind::Blues, HarmonicFunction::Predominant, 4);
        }
        if (complexity >= 5 && bars >= 4) {
            const int finalBar = bars - 1;
            add(barBeat(finalBar) - qMax(1, beatsPerBar / 2), key + 2, QStringLiteral("dim7"),
                key, ScaleKind::Diminished, HarmonicFunction::Passing, 5);
            add(barBeat(finalBar), key, QStringLiteral("9"),
                key, ScaleKind::Blues, HarmonicFunction::Tonic, 3);
        }
        if (complexity == 7 && bars >= 4) {
            add(barBeat(bars - 1) - 1, key + 1, QStringLiteral("9"),
                key, ScaleKind::Altered, HarmonicFunction::Dominant, 7);
        } else if (complexity == 8 && bars >= 4) {
            const QVector<int> roots{4, 9, 2, 7};
            const QVector<QString> suffixes{
                QStringLiteral("7#9"), QStringLiteral("7b9"),
                QStringLiteral("13"), QStringLiteral("alt")};
            const int totalBeats = bars * beatsPerBar;
            const int start = qMax(0, barBeat(bars - 2));
            const int spacing = qMax(1, (totalBeats - start - 1) / roots.size());
            for (int index = 0; index < roots.size(); ++index) {
                add(start + index * spacing, key + roots.at(index), suffixes.at(index),
                    key, index == roots.size() - 1 ? ScaleKind::Altered : ScaleKind::Blues,
                    HarmonicFunction::Dominant, 8);
            }
            add(totalBeats - 1, key, QStringLiteral("9"),
                key, ScaleKind::Blues, HarmonicFunction::Tonic, 4);
        }
    } else if (style == static_cast<int>(ChordStyle::ModalVamp)) {
        if (complexity == 8) {
            const QVector<int> roots{0, 2, 5, 3};
            for (int bar = 0; bar < bars; ++bar) {
                const int offset = bar == bars - 1 ? 0 : roots.at(bar % roots.size());
                add(barBeat(bar), key + offset, QStringLiteral("maj9#11"),
                    key + offset, ScaleKind::Lydian,
                    bar == bars - 1 ? HarmonicFunction::Pedal : HarmonicFunction::Colour, 8);
            }
        } else if (complexity == 7) {
            const QVector<int> roots{0, 1, 0, 6};
            for (int bar = 0; bar < bars; ++bar) {
                const int offset = bar == bars - 1 ? 0 : roots.at(bar % roots.size());
                add(barBeat(bar), key + offset, QStringLiteral("m9"),
                    key + offset, ScaleKind::Dorian,
                    offset == 0 ? HarmonicFunction::Pedal : HarmonicFunction::Colour, 7);
            }
        } else {
            QVector<int> degrees = complexity == 1
                ? QVector<int>{0, 3}
                : QVector<int>{0, 5, 3, 6};
            for (int bar = 0; bar < bars; ++bar) {
                const int degree = bar == bars - 1 ? 0 : degrees.at(bar % degrees.size());
                QString chord = diatonicChord(key, degree, minor, complexity >= 3, flats);
                if (complexity >= 3 && degree == 0) {
                    chord = chordSymbol(key, minor ? QStringLiteral("madd9") : QStringLiteral("add9"), flats);
                }
                setHarmonicEvent(plan, barBeat(bar), chord, key, homeScale,
                    degree == 0 ? HarmonicFunction::Pedal : HarmonicFunction::Colour,
                    qMin(complexity, 4));
            }
            if (complexity >= 4 && bars >= 4) {
                add(barBeat(bars / 2), key + (minor ? 5 : 10),
                    minor ? QString() : QStringLiteral("maj7"),
                    key, homeScale, HarmonicFunction::Colour, 4);
            }
            if (complexity >= 5 && bars >= 4) {
                add(barBeat(bars - 2), key + 2, QStringLiteral("m9"),
                    key + 2, ScaleKind::Dorian, HarmonicFunction::Pedal, 5);
            }
            if (complexity >= 6 && bars >= 8) {
                add(barBeat(bars / 2), key + 5, QStringLiteral("maj9#11"),
                    key + 5, ScaleKind::Lydian, HarmonicFunction::Pedal, 6);
                add(barBeat(bars / 2 + 1), key + 7, QStringLiteral("sus4"),
                    key + 5, ScaleKind::Mixolydian, HarmonicFunction::Colour, 6);
            }
        }
    } else if (style == static_cast<int>(ChordStyle::JazzTurnaround)) {
        if (complexity == 8) {
            const bool thirds = std::uniform_int_distribution<int>(0, 1)(rng) == 0;
            for (int bar = 0; bar < bars; ++bar) {
                const int offset = bar == bars - 1 ? 0 :
                    (thirds ? QVector<int>{0, 4, 8, 0}.at(bar % 4)
                            : QVector<int>{1, 6, 11, 4}.at(bar % 4));
                add(barBeat(bar), key + offset,
                    bar == bars - 1 ? QStringLiteral("maj9") : QStringLiteral("alt"),
                    key + offset, bar == bars - 1 ? ScaleKind::Major : ScaleKind::Altered,
                    bar == bars - 1 ? HarmonicFunction::Tonic : HarmonicFunction::Dominant, 8);
                if (bar != bars - 1 && beatsPerBar >= 2) {
                    add(barBeat(bar) + beatsPerBar / 2, key + offset + 5, QStringLiteral("m9"),
                        key + offset, ScaleKind::Dorian, HarmonicFunction::Predominant, 8);
                }
            }
        } else {
            const QVector<int> pattern = complexity == 1
                ? QVector<int>{0, 3, 4, 0}
                : QVector<int>{1, 4, 0, 5};
            for (int bar = 0; bar < bars; ++bar) {
                const int degree = bar == bars - 1 ? 0 : pattern.at(bar % pattern.size());
                addDegree(barBeat(bar), degree, complexity >= 3,
                    degree == 4 ? HarmonicFunction::Dominant :
                    degree == 0 || degree == 5 ? HarmonicFunction::Tonic : HarmonicFunction::Predominant,
                    qMin(complexity, 4));
            }
            if (complexity >= 4 && bars >= 4) {
                add(barBeat(bars - 2), key + 10, QStringLiteral("9"),
                    key, ScaleKind::Mixolydian, HarmonicFunction::Dominant, 4);
            }
            if (complexity >= 5 && bars >= 4 && beatsPerBar >= 2) {
                add(barBeat(1), key + 4, QStringLiteral("m7b5"),
                    key + 2, ScaleKind::Locrian, HarmonicFunction::Predominant, 5);
                add(barBeat(1) + beatsPerBar / 2, key + 9, QStringLiteral("7b9"),
                    key + 2, ScaleKind::Altered, HarmonicFunction::Dominant, 5);
            }
            if (complexity >= 6 && bars >= 8) {
                add(barBeat(bars / 2), key + 7, QStringLiteral("maj9"),
                    key + 7, ScaleKind::Major, HarmonicFunction::Tonic, 6);
                add(barBeat(bars / 2 + 1), key + 2, QStringLiteral("13"),
                    key + 7, ScaleKind::Mixolydian, HarmonicFunction::Dominant, 6);
            }
            if (complexity == 7 && bars >= 4) {
                add(barBeat(bars - 1) - 1, key + 1, QStringLiteral("7#9"),
                    key, ScaleKind::Altered, HarmonicFunction::Dominant, 7);
            }
        }
    } else {
        const int eventLength = complexity <= 2 ? beatsPerBar :
            (complexity <= 5 ? qMax(1, beatsPerBar / 2) : qMax(1, beatsPerBar / 4));
        QVector<int> riff;
        if (complexity <= 1) riff = {0, 5, 7, 0};
        else if (complexity <= 3) riff = {0, 1, 6, 0};
        else if (complexity <= 5) riff = {0, 3, 1, 6};
        else if (complexity == 6) riff = {0, 5, 8, 7};
        else if (complexity == 7) riff = {0, 4, 8, 1};
        else riff = std::uniform_int_distribution<int>(0, 1)(rng) == 0
            ? QVector<int>{0, 4, 8, 6}
            : QVector<int>{0, 2, 4, 6, 8, 10};
        int event = 0;
        for (int beat = 0; beat < bars * beatsPerBar; beat += eventLength, ++event) {
            const int offset = beat + eventLength >= bars * beatsPerBar
                ? 0 : riff.at(event % riff.size());
            QString suffix = complexity <= 2 ? QStringLiteral("5") :
                complexity == 3 ? (event % 3 == 0 ? QStringLiteral("sus2") : QStringLiteral("5")) :
                complexity <= 5 ? (event % 4 == 1 ? QStringLiteral("madd9") : QStringLiteral("5")) :
                complexity == 6 ? QStringLiteral("m9") :
                complexity == 7 ? QStringLiteral("7#9") : QStringLiteral("#11");
            ScaleKind eventScale = complexity == 8
                ? (riff.size() == 6 ? ScaleKind::WholeTone : ScaleKind::Diminished)
                : homeScale;
            add(beat, key + offset, suffix, complexity >= 6 ? key + offset : key,
                eventScale, offset == 0 ? HarmonicFunction::Pedal : HarmonicFunction::Colour,
                complexity);
        }
    }

    if (plan.events.isEmpty()) {
        add(0, key, minor ? QStringLiteral("m") : QString(),
            key, homeScale, HarmonicFunction::Tonic, 1);
    }
    finishHarmonicPlan(plan, bars * beatsPerBar);
    return plan;
}

void applyHarmonicPlan(SongSection& section, const HarmonicPlan& plan)
{
    for (const HarmonicEvent& event : plan.events) {
        placeChord(section, event.beat, event.chord);
    }
}

void generateMelody(
    SongSection& section,
    const HarmonicPlan& plan,
    int style,
    const QString& character,
    int complexity,
    bool flats,
    Rng& rng)
{
    ParsedChord activeChord;
    int activeEvent = -1;
    int previousMidi = -1;
    for (int beat = 0; beat < section.beats; ++beat) {
        while (activeEvent + 1 < plan.events.size() &&
               plan.events.at(activeEvent + 1).beat <= beat) {
            ++activeEvent;
        }
        if (activeEvent < 0) {
            section.targets[beat] = QStringLiteral("-");
            continue;
        }
        const HarmonicEvent& event = plan.events.at(activeEvent);
        const bool chordChange = event.beat == beat;
        if (chordChange) activeChord = parseChord(event.chord);
        if (!activeChord.valid || activeChord.rest) {
            section.targets[beat] = QStringLiteral("-");
            continue;
        }

        int pitchClass = activeChord.root;
        if (chordChange) {
            QVector<int> guideIntervals;
            if (complexity <= 2) {
                for (int interval : activeChord.intervals) {
                    const int simple = ((interval % 12) + 12) % 12;
                    if (simple == 0 || simple == 3 || simple == 4 || simple == 7) {
                        guideIntervals.push_back(interval);
                    }
                }
            } else if (activeChord.intervals.size() >= 4) {
                guideIntervals = {activeChord.intervals.at(1), activeChord.intervals.at(3)};
            } else if (activeChord.intervals.size() >= 3) {
                guideIntervals = {activeChord.intervals.at(1), activeChord.intervals.at(2)};
            }
            if (guideIntervals.isEmpty()) guideIntervals = activeChord.intervals;
            int bestMidi = -1;
            for (int interval : guideIntervals) {
                const int candidatePitch = (activeChord.root + interval) % 12;
                const int candidateMidi = nearestMidiForPitchClass(candidatePitch, previousMidi);
                if (bestMidi < 0 || previousMidi < 0 ||
                    std::abs(candidateMidi - previousMidi) < std::abs(bestMidi - previousMidi)) {
                    pitchClass = candidatePitch;
                    bestMidi = candidateMidi;
                }
            }
        } else {
            const int restChance = character.contains(QStringLiteral("Sparse"), Qt::CaseInsensitive) ||
                    character.contains(QStringLiteral("Spacious"), Qt::CaseInsensitive)
                ? 20 : (style == static_cast<int>(ChordStyle::JazzTurnaround) ? 5 : 9);
            const int roll = std::uniform_int_distribution<int>(0, 99)(rng);
            if (roll < restChance) {
                section.targets[beat] = QStringLiteral("-");
                continue;
            }
            const HarmonicEvent* nextEvent =
                activeEvent + 1 < plan.events.size() ? &plan.events.at(activeEvent + 1) : nullptr;
            if (complexity >= 5 && nextEvent && nextEvent->beat == beat + 1) {
                const ParsedChord nextChord = parseChord(nextEvent->chord);
                const int targetInterval = nextChord.intervals.size() >= 4
                    ? nextChord.intervals.at(3)
                    : nextChord.intervals.value(1, 0);
                const int target = (nextChord.root + targetInterval) % 12;
                pitchClass = (target +
                    (std::uniform_int_distribution<int>(0, 1)(rng) ? 1 : 11)) % 12;
            } else if (roll < restChance + 14 && previousMidi >= 0) {
                pitchClass = previousMidi % 12;
            } else {
                QVector<int> chordVocabulary = activeChord.intervals;
                if (complexity <= 2) {
                    chordVocabulary.erase(
                        std::remove_if(chordVocabulary.begin(), chordVocabulary.end(),
                            [](int interval) {
                                const int value = ((interval % 12) + 12) % 12;
                                return value != 0 && value != 3 && value != 4 && value != 7;
                            }),
                        chordVocabulary.end());
                    if (chordVocabulary.isEmpty()) chordVocabulary = activeChord.intervals;
                }
                const int chordToneChance = complexity <= 2 ? 84 :
                    complexity <= 4 ? 70 : complexity <= 6 ? 64 : 58;
                if (std::uniform_int_distribution<int>(0, 99)(rng) < chordToneChance) {
                    pitchClass = (activeChord.root + choose(chordVocabulary, rng)) % 12;
                } else {
                    const QVector<int> scale = scaleIntervals(event.scale);
                    pitchClass = (event.localTonic + choose(scale, rng)) % 12;
                }
            }
        }
        previousMidi = nearestMidiForPitchClass(pitchClass, previousMidi);
        section.targets[beat] = noteName(previousMidi % 12, flats) +
            QString::number(previousMidi / 12 - 1);
    }
}

QString makeSteps(int division, const QVector<QPair<int, QChar>>& hits)
{
    QString value(division, QLatin1Char('.'));
    for (const auto& hit : hits) {
        if (hit.first >= 0 && hit.first < division) value[hit.first] = hit.second;
    }
    return value;
}

void setLane(SongSection& section, int beat, const QString& laneName, const QString& value)
{
    const int lane = BeatGridModel::beatLaneNames().indexOf(laneName);
    if (lane >= 0) section.beatPatterns[beat].lanes[lane] = value;
}

SongSection chordIdea(ChordIdeaRequest request, Rng& rng)
{
    int style = request.style;
    if (style < 0 && !request.character.isEmpty()) {
        QVector<int> matching;
        for (int candidate = 0; candidate < chordStyleNames().size(); ++candidate) {
            if (chordCharacters(candidate).contains(request.character)) matching.push_back(candidate);
        }
        if (!matching.isEmpty()) style = choose(matching, rng);
    }
    style = resolveIndex(style, chordStyleNames().size(), rng);
    const int key = resolveIndex(request.key, 12, rng);
    const QString styleName = chordStyleNames().at(style);
    const QString character = resolveCharacter(request.character, chordCharacters(style), rng);
    const int bars = resolveBars(request.bars, style, false, rng);
    const int beatsPerBar = std::clamp(request.beatsPerBar, 1, 16);
    const int harmonicComplexity = std::clamp(request.harmonicComplexity, 1, 8);
    const int rhythmicComplexity = std::clamp(request.rhythmicComplexity, 1, 8);
    const bool flats = preferFlatsForKey(key);

    SongSection section;
    section.label = QStringLiteral("Generated");
    section.name = QStringLiteral("Generated — %1 / %2 / %3 / H%4 R%5 / %6 bars")
        .arg(noteName(key, flats), styleName, character)
        .arg(harmonicComplexity).arg(rhythmicComplexity).arg(bars);
    section.beats = bars * beatsPerBar;
    section.generatedKind = QStringLiteral("chord");
    section.generatedKey = noteName(key, flats);
    section.generatedStyle = styleName;
    section.generatedCharacter = character;
    section.generatedBars = bars;
    section.generatedHarmonicComplexity = harmonicComplexity;
    section.generatedRhythmicComplexity = rhythmicComplexity;
    section.chords.resize(section.beats);
    section.targets.resize(section.beats);

    const bool minor = character.contains(QStringLiteral("Minor"), Qt::CaseInsensitive) ||
        character.contains(QStringLiteral("Dark"), Qt::CaseInsensitive) ||
        character.contains(QStringLiteral("Brooding"), Qt::CaseInsensitive);
    const HarmonicPlan plan = buildHarmonicPlan(
        key, style, character, bars, beatsPerBar, harmonicComplexity, minor, flats, rng);
    applyHarmonicPlan(section, plan);
    generateMelody(section, plan, style, character, harmonicComplexity, flats, rng);
    return section;
}

SongSection beatIdea(BeatIdeaRequest request, Rng& rng, const SongSection* chordSection = nullptr)
{
    int style = request.style;
    if (style < 0 && !request.character.isEmpty()) {
        QVector<int> matching;
        for (int candidate = 0; candidate < beatStyleNames().size(); ++candidate) {
            if (beatCharacters(candidate).contains(request.character)) matching.push_back(candidate);
        }
        if (!matching.isEmpty()) style = choose(matching, rng);
    }
    style = resolveIndex(style, beatStyleNames().size(), rng);
    const QString styleName = beatStyleNames().at(style);
    const QString character = resolveCharacter(request.character, beatCharacters(style), rng);
    const int bars = resolveBars(request.bars, style, true, rng);
    const int beatsPerBar = std::clamp(request.beatsPerBar, 1, 16);
    const int complexity = std::clamp(request.rhythmicComplexity, 1, 8);
    SongSection section;
    section.label = QStringLiteral("Generated");
    section.name = QStringLiteral("Generated — %1 / %2 / R%3 / %4 bars")
        .arg(styleName, character).arg(complexity).arg(bars);
    section.beats = bars * beatsPerBar;
    section.generatedKind = QStringLiteral("beat");
    section.generatedStyle = styleName;
    section.generatedCharacter = character;
    section.generatedBars = bars;
    section.generatedRhythmicComplexity = complexity;
    section.beatPatterns.resize(section.beats);
    const bool shuffle = style == static_cast<int>(BeatStyle::BluesShuffle);
    const bool metal = style == static_cast<int>(BeatStyle::ModernMetal);
    const int snareBeat = style == static_cast<int>(BeatStyle::HalfTime) ? beatsPerBar / 2 : 1;
    QVector<int> phraseChoices{4, 8};
    if (style == static_cast<int>(BeatStyle::BluesShuffle)) phraseChoices = {4, 8, 12};
    if (metal) phraseChoices = {4, 8, 16};
    if (character == QStringLiteral("Sparse") || character == QStringLiteral("Spacious")) {
        phraseChoices = {8, 16};
    }
    if (complexity >= 7) phraseChoices = {3, 5, 7};
    const int phraseBars = choose(phraseChoices, rng);
    for (int beat = 0; beat < section.beats; ++beat) {
        const int within = beat % beatsPerBar;
        const int bar = beat / beatsPerBar;
        BeatPattern& pattern = section.beatPatterns[beat];
        if (shuffle) {
            pattern.division = complexity >= 5 && (bar % phraseBars == phraseBars - 1) ? 6 : 3;
        } else if (complexity == 8) {
            const QVector<int> divisions{4, 6, 8, 4};
            pattern.division = divisions.at(beat % divisions.size());
        } else if (complexity >= 5 && bar % phraseBars == phraseBars - 1 && within >= beatsPerBar - 2) {
            pattern.division = 6;
        } else {
            pattern.division = complexity <= 2 ? 2 : 4;
        }
        pattern.lanes.resize(BeatGridModel::beatLaneNames().size());
        const bool phraseBoundary = within == beatsPerBar - 1 &&
            ((bar + 1) % phraseBars == 0 || bar + 1 == bars);
        const int characterAdjustment = character == QStringLiteral("Sparse") ? -18 :
            character == QStringLiteral("Busy") || character == QStringLiteral("Blast") ? 16 : 0;
        const int fillChance = std::clamp(4 + complexity * 9 + characterAdjustment, 2, 90);
        const bool finalBeat = beat == section.beats - 1;
        const bool fill = (finalBeat && complexity >= 2) ||
            ((phraseBoundary || finalBeat) &&
             std::uniform_int_distribution<int>(0, 99)(rng) < fillChance);
        if (fill) {
            pattern.division = shuffle ? 6 : (complexity >= 5 ? 8 : 4);
            setLane(section, beat, QStringLiteral("Tom"),
                makeSteps(pattern.division, complexity <= 2
                    ? QVector<QPair<int,QChar>>{{pattern.division - 1, QLatin1Char('x')}}
                    : QVector<QPair<int,QChar>>{{0, QLatin1Char('x')},
                        {pattern.division / 2, QLatin1Char('a')},
                        {pattern.division - 1, QLatin1Char('x')}}));
            setLane(section, beat, QStringLiteral("Snare"),
                makeSteps(pattern.division, complexity >= 3
                    ? QVector<QPair<int,QChar>>{{pattern.division / 4, QLatin1Char('g')},
                        {pattern.division * 3 / 4, QLatin1Char('x')}}
                    : QVector<QPair<int,QChar>>{{0, QLatin1Char('x')}}));
        }
        QVector<QPair<int,QChar>> kicks;
        if (within == 0) kicks.push_back({0, QLatin1Char('a')});
        if (complexity >= 2 && within == 0) {
            kicks.push_back({pattern.division / 2, QLatin1Char('g')});
        }
        if (complexity >= 3 && within == 2 % beatsPerBar) {
            kicks.push_back({0, QLatin1Char('x')});
        }
        if (complexity >= 4 && within == beatsPerBar - 1) {
            kicks.push_back({pattern.division - 1, QLatin1Char('g')});
        }
        if (complexity >= 6 && (beat % qMax(1, beatsPerBar * 2)) == beatsPerBar + 1) {
            kicks.push_back({qMin(1, pattern.division - 1), QLatin1Char('x')});
        }
        if (metal && complexity >= 3 && within == 2 % beatsPerBar) {
            kicks.push_back({qMin(1, pattern.division - 1), QLatin1Char('x')});
        }
        setLane(section, beat, QStringLiteral("Kick"), makeSteps(pattern.division, kicks));
        if (!fill && (within == snareBeat || (!styleName.contains(QStringLiteral("Half")) && within == 3))) {
            setLane(section, beat, QStringLiteral("Snare"),
                makeSteps(pattern.division, {{0, QLatin1Char('a')}}));
        } else if (complexity >= 3 &&
                   (style == static_cast<int>(BeatStyle::Funk) || within == 2)) {
            setLane(section, beat, QStringLiteral("Snare"),
                makeSteps(pattern.division, {{pattern.division - 1, QLatin1Char('g')}}));
        }
        QVector<QPair<int,QChar>> hats;
        const int hatSpacing = pattern.division <= 3 ? 1 : 2;
        for (int step = 0; step < pattern.division; step += hatSpacing) {
            QChar strength = step == 0 ? QLatin1Char('x') : QLatin1Char('g');
            if (complexity >= 7 && ((beat * pattern.division + step) % 3) == 0) {
                strength = QLatin1Char('a');
            }
            hats.push_back({step, strength});
        }
        setLane(section, beat,
            complexity >= 3 && within == beatsPerBar - 1 &&
                (character == QStringLiteral("Open") || bar % 2 == 1)
            ? QStringLiteral("Open HH") : QStringLiteral("Closed HH"), makeSteps(pattern.division, hats));
        const bool harmonicAccent = chordSection &&
            !chordSection->chords.value(beat).trimmed().isEmpty();
        if (beat == 0 || (complexity >= 4 && harmonicAccent && within == 0)) {
            setLane(section, beat, QStringLiteral("Crash"), makeSteps(pattern.division, {{0, QLatin1Char('a')}}));
        }
        if (metal) {
            QVector<QPair<int,QChar>> pulse = within % 2 == 0
                ? QVector<QPair<int,QChar>>{{0, QLatin1Char('a')}, {1, QLatin1Char('x')}}
                : QVector<QPair<int,QChar>>{{0, QLatin1Char('x')}};
            if (complexity <= 2) pulse = {{0, QLatin1Char('x')}};
            if (complexity >= 6) pulse.push_back({pattern.division - 1, QLatin1Char('g')});
            setLane(section, beat, QStringLiteral("Guitar"), makeSteps(pattern.division, pulse));
        }
    }
    return section;
}

int matchingBeatStyle(int chordStyle)
{
    switch (static_cast<ChordStyle>(chordStyle)) {
    case ChordStyle::FunctionalPop: return static_cast<int>(BeatStyle::StraightRock);
    case ChordStyle::BluesForm: return static_cast<int>(BeatStyle::BluesShuffle);
    case ChordStyle::ModalVamp: return static_cast<int>(BeatStyle::HalfTime);
    case ChordStyle::JazzTurnaround: return static_cast<int>(BeatStyle::Funk);
    case ChordStyle::ModernMetal: return static_cast<int>(BeatStyle::ModernMetal);
    }
    return static_cast<int>(BeatStyle::StraightRock);
}

QString matchingBeatCharacter(int chordStyle, const QString& chordCharacter)
{
    switch (static_cast<ChordStyle>(chordStyle)) {
    case ChordStyle::FunctionalPop:
        return chordCharacter == QStringLiteral("Bright") ? QStringLiteral("Open") : QStringLiteral("Driving");
    case ChordStyle::BluesForm:
        return chordCharacter == QStringLiteral("Laid Back") ? QStringLiteral("Laid Back") : QStringLiteral("Driving");
    case ChordStyle::ModalVamp:
        return chordCharacter == QStringLiteral("Dark") ? QStringLiteral("Heavy") : QStringLiteral("Spacious");
    case ChordStyle::JazzTurnaround:
        return chordCharacter == QStringLiteral("Tense") ? QStringLiteral("Busy") : QStringLiteral("Tight");
    case ChordStyle::ModernMetal:
        return chordCharacter == QStringLiteral("Chugging") ? QStringLiteral("Chugging") : QStringLiteral("Syncopated");
    }
    return {};
}

GeneratedPracticeIdea coupledIdea(ChordIdeaRequest request, std::uint32_t seed)
{
    Rng chordRng(seed ^ 0x243f6a88U);
    Rng beatRng(seed ^ 0x85a308d3U);
    Rng tempoRng(seed ^ 0x13198a2eU);
    GeneratedPracticeIdea result;
    result.chordSection = chordIdea(request, chordRng);
    const int chordStyle = chordStyleNames().indexOf(result.chordSection.generatedStyle);
    BeatIdeaRequest beatRequest;
    beatRequest.style = matchingBeatStyle(qMax(0, chordStyle));
    beatRequest.character = matchingBeatCharacter(qMax(0, chordStyle), result.chordSection.generatedCharacter);
    beatRequest.bars = result.chordSection.generatedBars;
    beatRequest.beatsPerBar = request.beatsPerBar;
    beatRequest.rhythmicComplexity = request.rhythmicComplexity;
    result.beatSection = beatIdea(beatRequest, beatRng, &result.chordSection);
    result.beatSection.generatedHarmonicComplexity =
        result.chordSection.generatedHarmonicComplexity;

    int minimumBpm = 90;
    int maximumBpm = 130;
    switch (static_cast<ChordStyle>(qMax(0, chordStyle))) {
    case ChordStyle::FunctionalPop:
        minimumBpm = 92; maximumBpm = 132; result.clickDivision = 2;
        break;
    case ChordStyle::BluesForm:
        minimumBpm = 72; maximumBpm = 112; result.clickDivision = 3;
        break;
    case ChordStyle::ModalVamp:
        minimumBpm = 68; maximumBpm = 108; result.clickDivision = 1;
        break;
    case ChordStyle::JazzTurnaround:
        minimumBpm = 105; maximumBpm = 165; result.clickDivision = 2;
        break;
    case ChordStyle::ModernMetal:
        minimumBpm = 110; maximumBpm = 180;
        result.clickDivision = result.chordSection.generatedCharacter == QStringLiteral("Chugging") ? 4 : 2;
        break;
    }
    result.bpm = std::uniform_int_distribution<int>(
        minimumBpm / 2, maximumBpm / 2)(tempoRng) * 2;
    result.chordSection.name += QStringLiteral(" / %1 BPM").arg(result.bpm);
    result.beatSection.name += QStringLiteral(" / %1 BPM").arg(result.bpm);
    const int beatsPerBar = std::clamp(request.beatsPerBar, 1, 16);
    const int steps = beatsPerBar * result.clickDivision;
    result.clickEnabled.fill(true, steps);
    result.clickAccents.fill(false, steps);
    for (int step = 0; step < steps; ++step) {
        const int beat = step / result.clickDivision;
        const int withinBeat = step % result.clickDivision;
        if (chordStyle == static_cast<int>(ChordStyle::BluesForm)) {
            result.clickEnabled[step] = withinBeat == 0 || withinBeat == 2;
        } else if (chordStyle == static_cast<int>(ChordStyle::ModalVamp)) {
            result.clickEnabled[step] = true;
        }
        if (chordStyle == static_cast<int>(ChordStyle::JazzTurnaround)) {
            result.clickAccents[step] = withinBeat == 0 && (beat == 1 || beat == 3);
        } else {
            result.clickAccents[step] = step == 0 ||
                (chordStyle == static_cast<int>(ChordStyle::ModernMetal) && withinBeat == 0);
        }
    }
    return result;
}

} // namespace

QStringList chordStyleNames()
{
    return {QStringLiteral("Functional Pop"), QStringLiteral("Blues Form"), QStringLiteral("Modal Vamp"),
        QStringLiteral("Jazz Turnaround"), QStringLiteral("Modern Metal")};
}

QStringList beatStyleNames()
{
    return {QStringLiteral("Straight Rock"), QStringLiteral("Half-Time"), QStringLiteral("Funk"),
        QStringLiteral("Four-on-the-Floor"), QStringLiteral("Blues Shuffle"), QStringLiteral("Modern Metal")};
}

QStringList chordCharacters(int style)
{
    switch (static_cast<ChordStyle>(std::clamp(style, 0, static_cast<int>(chordStyleNames().size()) - 1))) {
    case ChordStyle::FunctionalPop: return {QStringLiteral("Bright"), QStringLiteral("Minor"), QStringLiteral("Anthemic")};
    case ChordStyle::BluesForm: return {QStringLiteral("Driving"), QStringLiteral("Laid Back"), QStringLiteral("Turnaround Heavy")};
    case ChordStyle::ModalVamp: return {QStringLiteral("Open"), QStringLiteral("Dark"), QStringLiteral("Floating")};
    case ChordStyle::JazzTurnaround: return {QStringLiteral("Smooth"), QStringLiteral("Tense"), QStringLiteral("Late Night")};
    case ChordStyle::ModernMetal:
        return {QStringLiteral("Chugging"), QStringLiteral("Brooding"), QStringLiteral("Dissonant"),
            QStringLiteral("Ionian"), QStringLiteral("Dorian"), QStringLiteral("Phrygian"),
            QStringLiteral("Lydian"), QStringLiteral("Mixolydian"), QStringLiteral("Aeolian"),
            QStringLiteral("Locrian")};
    }
    return {};
}

QStringList beatCharacters(int style)
{
    switch (static_cast<BeatStyle>(std::clamp(style, 0, static_cast<int>(beatStyleNames().size()) - 1))) {
    case BeatStyle::StraightRock: return {QStringLiteral("Driving"), QStringLiteral("Open"), QStringLiteral("Sparse")};
    case BeatStyle::HalfTime: return {QStringLiteral("Heavy"), QStringLiteral("Spacious"), QStringLiteral("Syncopated")};
    case BeatStyle::Funk: return {QStringLiteral("Tight"), QStringLiteral("Ghosted"), QStringLiteral("Busy")};
    case BeatStyle::FourOnTheFloor: return {QStringLiteral("Clean"), QStringLiteral("Open"), QStringLiteral("Pumping")};
    case BeatStyle::BluesShuffle: return {QStringLiteral("Laid Back"), QStringLiteral("Driving"), QStringLiteral("Rolling")};
    case BeatStyle::ModernMetal: return {QStringLiteral("Chugging"), QStringLiteral("Blast"), QStringLiteral("Syncopated")};
    }
    return {};
}

QStringList keyNames()
{
    return {QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("D"), QStringLiteral("Eb"),
        QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("G"),
        QStringLiteral("Ab"), QStringLiteral("A"), QStringLiteral("Bb"), QStringLiteral("B")};
}

GeneratedPracticeIdea generateCoupledPracticeIdea(const ChordIdeaRequest& request)
{
    return coupledIdea(request, std::random_device{}());
}

SongSection generateChordIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed)
{
    Rng rng(seed);
    return chordIdea(request, rng);
}

SongSection generateBeatIdeaForTest(const BeatIdeaRequest& request, std::uint32_t seed)
{
    Rng rng(seed);
    return beatIdea(request, rng);
}

GeneratedPracticeIdea generateCoupledPracticeIdeaForTest(
    const ChordIdeaRequest& request,
    std::uint32_t seed)
{
    return coupledIdea(request, seed);
}

} // namespace jam2::practice
