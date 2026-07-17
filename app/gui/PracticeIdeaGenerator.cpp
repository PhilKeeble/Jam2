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

QVector<int> melodyScale(int style, bool minor, const QString& character)
{
    if (style == static_cast<int>(ChordStyle::BluesForm)) return {0, 3, 5, 6, 7, 10};
    if (style == static_cast<int>(ChordStyle::ModernMetal)) {
        if (character == QStringLiteral("Phrygian")) return {0, 1, 3, 5, 7, 8, 10};
        if (character == QStringLiteral("Dorian")) return {0, 2, 3, 5, 7, 9, 10};
        if (character == QStringLiteral("Lydian")) return {0, 2, 4, 6, 7, 9, 11};
        if (character == QStringLiteral("Mixolydian")) return {0, 2, 4, 5, 7, 9, 10};
        if (character == QStringLiteral("Locrian")) return {0, 1, 3, 5, 6, 8, 10};
        if (character == QStringLiteral("Ionian")) return {0, 2, 4, 5, 7, 9, 11};
    }
    return minor ? QVector<int>{0, 2, 3, 5, 7, 8, 10}
                 : QVector<int>{0, 2, 4, 5, 7, 9, 11};
}

void generateMelody(
    SongSection& section,
    int key,
    int style,
    bool minor,
    const QString& character,
    bool flats,
    Rng& rng)
{
    const QVector<int> scale = melodyScale(style, minor, character);
    ParsedChord activeChord;
    int previousMidi = -1;
    for (int beat = 0; beat < section.beats; ++beat) {
        const QString symbol = section.chords.value(beat).trimmed();
        const bool chordChange = !symbol.isEmpty() && symbol != QStringLiteral("-");
        if (chordChange) activeChord = parseChord(symbol);
        if (symbol == QStringLiteral("-") || !activeChord.valid || activeChord.rest) {
            section.targets[beat] = QStringLiteral("-");
            continue;
        }

        // A new harmony always gets a voice-led guide tone so the chord movement
        // remains audible from the melody alone. Between changes, style controls
        // how often the line uses chord tones, scale colour, rests, or sustains.
        int pitchClass = activeChord.root;
        if (chordChange) {
            QVector<int> guideIntervals;
            if (activeChord.intervals.size() >= 4) {
                guideIntervals = {activeChord.intervals.at(1), activeChord.intervals.constLast()};
            } else if (activeChord.intervals.size() >= 3) {
                guideIntervals = {activeChord.intervals.at(1), activeChord.intervals.at(2)};
            } else {
                guideIntervals = activeChord.intervals;
            }
            int bestMidi = -1;
            for (int interval : guideIntervals) {
                const int candidatePitch = (activeChord.root + interval) % 12;
                const int candidateMidi = nearestMidiForPitchClass(candidatePitch, previousMidi);
                if (bestMidi < 0 ||
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
            if (roll < restChance + 16 && previousMidi >= 0) {
                pitchClass = previousMidi % 12;
            } else {
                const int chordToneChance =
                    style == static_cast<int>(ChordStyle::JazzTurnaround) ? 72 :
                    style == static_cast<int>(ChordStyle::FunctionalPop) ? 68 :
                    style == static_cast<int>(ChordStyle::ModernMetal) ? 64 : 58;
                if (std::uniform_int_distribution<int>(0, 99)(rng) < chordToneChance) {
                    pitchClass = (activeChord.root + choose(activeChord.intervals, rng)) % 12;
                } else {
                    pitchClass = (key + choose(scale, rng)) % 12;
                    if (style == static_cast<int>(ChordStyle::JazzTurnaround) &&
                        std::uniform_int_distribution<int>(0, 4)(rng) == 0 && previousMidi >= 0) {
                        pitchClass = (previousMidi +
                            (std::uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1) + 12) % 12;
                    }
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
    const bool flats = preferFlatsForKey(key);

    SongSection section;
    section.label = QStringLiteral("Generated");
    section.name = QStringLiteral("Generated — %1 / %2 / %3 / %4 bars")
        .arg(noteName(key, flats), styleName, character).arg(bars);
    section.beats = bars * beatsPerBar;
    section.generatedKind = QStringLiteral("chord");
    section.generatedKey = noteName(key, flats);
    section.generatedStyle = styleName;
    section.generatedCharacter = character;
    section.generatedBars = bars;
    section.chords.resize(section.beats);
    section.targets.resize(section.beats);

    const bool minor = character.contains(QStringLiteral("Minor"), Qt::CaseInsensitive) ||
        character.contains(QStringLiteral("Dark"), Qt::CaseInsensitive) ||
        character.contains(QStringLiteral("Brooding"), Qt::CaseInsensitive);
    if (style == static_cast<int>(ChordStyle::BluesForm)) {
        const QVector<int> form{0,0,0,0, 3,3,0,0, 4,3,0,4};
        for (int bar = 0; bar < bars; ++bar) {
            const int degree = form.at(bar % form.size());
            const int semitone = degree == 3 ? 5 : degree == 4 ? 7 : 0;
            placeChord(section, bar * beatsPerBar, chordSymbol(key + semitone, QStringLiteral("7"), flats));
        }
    } else if (style == static_cast<int>(ChordStyle::ModalVamp)) {
        const QVector<QVector<int>> patterns{{0, 3}, {0, 6}, {0, 1}, {0, 5, 3, 6}};
        const QVector<int> pattern = choose(patterns, rng);
        for (int bar = 0; bar < bars; ++bar) {
            const int degree = pattern.at(bar % pattern.size());
            placeChord(section, bar * beatsPerBar,
                diatonicChord(key, degree, minor, false, flats));
        }
    } else if (style == static_cast<int>(ChordStyle::JazzTurnaround)) {
        const QVector<int> pattern{1, 4, 0, 5};
        for (int bar = 0; bar < bars; ++bar) {
            const int degree = pattern.at(bar % pattern.size());
            placeChord(section, bar * beatsPerBar,
                diatonicChord(key, degree, minor, true, flats));
        }
    } else if (style == static_cast<int>(ChordStyle::ModernMetal)) {
        const QHash<QString, QVector<int>> modes{
            {QStringLiteral("Ionian"), {0, 2, 4, 5, 7, 9, 11}},
            {QStringLiteral("Dorian"), {0, 2, 3, 5, 7, 9, 10}},
            {QStringLiteral("Phrygian"), {0, 1, 3, 5, 7, 8, 10}},
            {QStringLiteral("Lydian"), {0, 2, 4, 6, 7, 9, 11}},
            {QStringLiteral("Mixolydian"), {0, 2, 4, 5, 7, 9, 10}},
            {QStringLiteral("Aeolian"), {0, 2, 3, 5, 7, 8, 10}},
            {QStringLiteral("Locrian"), {0, 1, 3, 5, 6, 8, 10}},
        };
        const QVector<QVector<int>> riffs{{0, 1, 6, 0}, {0, 3, 1, 6}, {0, 0, 8, 7}, {0, 2, 1, 5}};
        const QVector<int> riff = modes.contains(character) ? modes.value(character) : choose(riffs, rng);
        const QVector<QString> vocabulary{
            QStringLiteral("5"), QString(), QStringLiteral("m"), QStringLiteral("7"),
            QStringLiteral("maj7"), QStringLiteral("m7"), QStringLiteral("sus2"), QStringLiteral("sus4")};
        const int eventLength = character == QStringLiteral("Chugging") ? 1 : qMax(1, beatsPerBar / 2);
        int event = 0;
        for (int beat = 0; beat < section.beats; beat += eventLength, ++event) {
            const QString suffix = modes.contains(character)
                ? choose(vocabulary, rng)
                : (event % 5 == 4 ? QStringLiteral("sus2") : QStringLiteral("5"));
            placeChord(section, beat, chordSymbol(key + riff.at(event % riff.size()), suffix, flats));
        }
    } else {
        const QVector<QVector<int>> patterns{{0, 4, 5, 3}, {0, 5, 3, 4}, {5, 3, 0, 4}, {0, 3, 5, 4}};
        const QVector<int> pattern = choose(patterns, rng);
        for (int bar = 0; bar < bars; ++bar) {
            const int degree = pattern.at(bar % pattern.size());
            placeChord(section, bar * beatsPerBar,
                diatonicChord(key, degree, minor, false, flats));
        }
    }
    generateMelody(section, key, style, minor, character, flats, rng);
    return section;
}

SongSection beatIdea(BeatIdeaRequest request, Rng& rng)
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
    SongSection section;
    section.label = QStringLiteral("Generated");
    section.name = QStringLiteral("Generated — %1 / %2 / %3 bars").arg(styleName, character).arg(bars);
    section.beats = bars * beatsPerBar;
    section.generatedKind = QStringLiteral("beat");
    section.generatedStyle = styleName;
    section.generatedCharacter = character;
    section.generatedBars = bars;
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
    const int phraseBars = choose(phraseChoices, rng);
    for (int beat = 0; beat < section.beats; ++beat) {
        const int within = beat % beatsPerBar;
        const int bar = beat / beatsPerBar;
        BeatPattern& pattern = section.beatPatterns[beat];
        pattern.division = shuffle ? 3 : 4;
        pattern.lanes.resize(BeatGridModel::beatLaneNames().size());
        const bool phraseBoundary = within == beatsPerBar - 1 &&
            ((bar + 1) % phraseBars == 0 || bar + 1 == bars);
        const int fillChance = character == QStringLiteral("Sparse") ? 22 :
            character == QStringLiteral("Busy") || character == QStringLiteral("Blast") ? 72 : 52;
        const bool finalBeat = beat == section.beats - 1;
        const bool fill = finalBeat || (phraseBoundary &&
            std::uniform_int_distribution<int>(0, 99)(rng) < fillChance);
        if (fill) {
            pattern.division = shuffle ? 6 : 8;
            setLane(section, beat, QStringLiteral("Tom"),
                makeSteps(pattern.division, {{0, QLatin1Char('x')}, {pattern.division / 2, QLatin1Char('a')},
                    {pattern.division - 1, QLatin1Char('x')}}));
            setLane(section, beat, QStringLiteral("Snare"),
                makeSteps(pattern.division, {{pattern.division / 4, QLatin1Char('g')},
                    {pattern.division * 3 / 4, QLatin1Char('x')}}));
        }
        setLane(section, beat, QStringLiteral("Kick"),
            makeSteps(pattern.division, within == 0
                ? QVector<QPair<int,QChar>>{{0, QLatin1Char('a')}, {pattern.division / 2, QLatin1Char('g')}}
                : (metal && within == 2 ? QVector<QPair<int,QChar>>{{0, QLatin1Char('x')}, {1, QLatin1Char('x')}}
                                       : QVector<QPair<int,QChar>>{})));
        if (!fill && (within == snareBeat || (!styleName.contains(QStringLiteral("Half")) && within == 3))) {
            setLane(section, beat, QStringLiteral("Snare"),
                makeSteps(pattern.division, {{0, QLatin1Char('a')}}));
        } else if (style == static_cast<int>(BeatStyle::Funk) && within == 2) {
            setLane(section, beat, QStringLiteral("Snare"),
                makeSteps(pattern.division, {{pattern.division - 1, QLatin1Char('g')}}));
        }
        QVector<QPair<int,QChar>> hats;
        for (int step = 0; step < pattern.division; step += shuffle ? 2 : 2) {
            hats.push_back({step, step == 0 ? QLatin1Char('x') : QLatin1Char('g')});
        }
        setLane(section, beat, within == beatsPerBar - 1 && character == QStringLiteral("Open")
            ? QStringLiteral("Open HH") : QStringLiteral("Closed HH"), makeSteps(pattern.division, hats));
        if (beat == 0) {
            setLane(section, beat, QStringLiteral("Crash"), makeSteps(pattern.division, {{0, QLatin1Char('a')}}));
        }
        if (metal) {
            const QVector<QPair<int,QChar>> pulse = within % 2 == 0
                ? QVector<QPair<int,QChar>>{{0, QLatin1Char('a')}, {1, QLatin1Char('x')}}
                : QVector<QPair<int,QChar>>{{0, QLatin1Char('x')}};
            setLane(section, beat, QStringLiteral("Guitar"), makeSteps(pattern.division, pulse));
            setLane(section, beat, QStringLiteral("Bass"), makeSteps(pattern.division, pulse));
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

GeneratedPracticeIdea coupledIdea(ChordIdeaRequest request, Rng& rng)
{
    GeneratedPracticeIdea result;
    result.chordSection = chordIdea(request, rng);
    const int chordStyle = chordStyleNames().indexOf(result.chordSection.generatedStyle);
    BeatIdeaRequest beatRequest;
    beatRequest.style = matchingBeatStyle(qMax(0, chordStyle));
    beatRequest.character = matchingBeatCharacter(qMax(0, chordStyle), result.chordSection.generatedCharacter);
    beatRequest.bars = result.chordSection.generatedBars;
    beatRequest.beatsPerBar = request.beatsPerBar;
    result.beatSection = beatIdea(beatRequest, rng);

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
    result.bpm = std::uniform_int_distribution<int>(minimumBpm / 2, maximumBpm / 2)(rng) * 2;
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
    Rng rng(std::random_device{}());
    return coupledIdea(request, rng);
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
    Rng rng(seed);
    return coupledIdea(request, rng);
}

} // namespace jam2::practice
