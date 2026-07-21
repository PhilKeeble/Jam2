#include "PracticeIdeaGenerator.hpp"

#include "MusicTheory.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace jam2::practice {
namespace {

using Rng = std::mt19937;

struct ProgressionDef {
    QString id;
    QString name;
    QStringList romans;
};

struct StyleDef {
    QString id;
    QString name;
    int minimumBpm;
    int maximumBpm;
    QString chordPatch;
    QString chordPatchName;
    QString melodyPatch;
    QString melodyPatchName;
    QString drumPatch;
    QString drumPatchName;
    QVector<ProgressionDef> progressions;
};

struct MoodDef {
    QString id;
    QString name;
    int tempoOffset;
};

struct ModeDef {
    QString name;
    std::array<int, 7> intervals;
    bool minor;
};

struct PlannedEvent {
    int beat = 0;
    int duration = 1;
    QString roman;
    QString chord;
};

template <typename T>
const T& choose(const QVector<T>& values, Rng& rng)
{
    return values.at(std::uniform_int_distribution<int>(0, values.size() - 1)(rng));
}

ProgressionDef progression(const char* id, const char* name, std::initializer_list<const char*> romans)
{
    ProgressionDef result{QString::fromLatin1(id), QString::fromUtf8(name), {}};
    for (const char* roman : romans) result.romans.push_back(QString::fromUtf8(roman));
    return result;
}

const QVector<StyleDef>& styles()
{
    static const QVector<StyleDef> values{
        {QStringLiteral("pop"), QStringLiteral("Pop"), 92, 132,
            QStringLiteral("pop-polysynth"), QStringLiteral("Pop Polysynth"),
            QStringLiteral("pop-lead"), QStringLiteral("Clean Pop Lead"),
            QStringLiteral("pop-tight"), QStringLiteral("Tight Pop Kit"), {
            progression("pop-1564", "I–V–vi–IV", {"I", "V", "vi", "IV"}),
            progression("pop-6415", "vi–IV–I–V", {"vi", "IV", "I", "V"}),
            progression("pop-1645", "I–vi–IV–V", {"I", "vi", "IV", "V"}),
            progression("pop-1465", "I–IV–vi–V", {"I", "IV", "vi", "V"}),
            progression("pop-134m", "I–iii–IV–iv", {"I", "iii", "IV", "iv"}),
            progression("pop-1541", "I–V–IV–I", {"I", "V", "IV", "I"}),
        }},
        {QStringLiteral("indie"), QStringLiteral("Indie"), 82, 126,
            QStringLiteral("indie-chorus-pad"), QStringLiteral("Indie Chorus Pad"),
            QStringLiteral("indie-pluck"), QStringLiteral("Worn Indie Pluck"),
            QStringLiteral("indie-room"), QStringLiteral("Loose Room Kit"), {
            progression("indie-134m", "I–III–IV–iv", {"I", "III", "IV", "iv"}),
            progression("indie-1b74", "I–♭VII–IV", {"I", "bVII", "IV", "I"}),
            progression("indie-minor-loop", "i–♭VI–♭III–♭VII", {"i", "bVI", "bIII", "bVII"}),
            progression("indie-1524", "I–V–ii–IV", {"I", "V", "ii", "IV"}),
            progression("indie-1624", "I–vi–ii–IV", {"I", "vi", "ii", "IV"}),
            progression("indie-pedal", "Tonic-pedal colour loop", {"Iadd9", "IV/I", "ii/I", "Iadd9"}),
        }},
        {QStringLiteral("rock"), QStringLiteral("Rock"), 96, 152,
            QStringLiteral("rock-organ-pluck"), QStringLiteral("Rock Organ / Pluck"),
            QStringLiteral("rock-round-lead"), QStringLiteral("Round Rock Lead"),
            QStringLiteral("rock-live"), QStringLiteral("Live Rock Kit"), {
            progression("rock-1b74", "I–♭VII–IV", {"I", "bVII", "IV", "I"}),
            progression("rock-minor-loop", "i–♭VI–♭III–♭VII", {"i", "bVI", "bIII", "bVII"}),
            progression("rock-1454", "I–IV–V–IV", {"I", "IV", "V", "IV"}),
            progression("rock-1b34", "I–♭III–IV", {"I", "bIII", "IV", "I"}),
            progression("rock-154", "I–V–IV", {"I", "V", "IV", "I"}),
            progression("rock-minor-descent", "i–♭VII–♭VI–♭VII", {"i", "bVII", "bVI", "bVII"}),
        }},
        {QStringLiteral("jazz"), QStringLiteral("Jazz"), 92, 168,
            QStringLiteral("jazz-ep"), QStringLiteral("Jazz Electric Piano"),
            QStringLiteral("jazz-bell"), QStringLiteral("Round Jazz Bell"),
            QStringLiteral("jazz-brush"), QStringLiteral("Brush / Ride Kit"), {
            progression("jazz-2516", "ii7–V7–Imaj7–vi7", {"ii7", "V7", "Imaj7", "vi7"}),
            progression("jazz-1625", "Imaj7–vi7–ii7–V7", {"Imaj7", "vi7", "ii7", "V7"}),
            progression("jazz-3625", "iii7–VI7–ii7–V7", {"iii7", "VI7", "ii7", "V7"}),
            progression("jazz-minor-251", "iiø–V–i", {"iiø", "V7", "i", "i"}),
            progression("jazz-backdoor", "Backdoor iv–♭VII–I", {"iv7", "bVII7", "Imaj7", "Imaj7"}),
            progression("jazz-circle", "Circle-dominant", {"III7", "VI7", "II7", "V7"}),
        }},
        {QStringLiteral("modal-vamp"), QStringLiteral("Modal Vamp"), 68, 116,
            QStringLiteral("modal-ambient-pad"), QStringLiteral("Modal Ambient Pad"),
            QStringLiteral("modal-air-lead"), QStringLiteral("Airy Modal Lead"),
            QStringLiteral("modal-spacious"), QStringLiteral("Spacious Modal Kit"), {
            progression("modal-mixolydian", "Mixolydian I–♭VII–IV", {"I", "bVII", "IV", "I"}),
            progression("modal-dorian", "Dorian i–IV", {"i", "IV", "i", "IV"}),
            progression("modal-lydian", "Lydian I–II", {"I", "II", "I", "II"}),
            progression("modal-aeolian", "Aeolian i–♭VI–♭VII", {"i", "bVI", "bVII", "i"}),
            progression("modal-phrygian", "Phrygian i–♭II", {"i", "bII", "i", "bII"}),
            progression("modal-pedal", "Tonic pedal / modal colours", {"Iadd9", "bVII/I", "II/I", "Iadd9"}),
        }},
        {QStringLiteral("blues"), QStringLiteral("Blues"), 66, 124,
            QStringLiteral("blues-organ"), QStringLiteral("Blues Organ"),
            QStringLiteral("blues-reed"), QStringLiteral("Blues Reed Lead"),
            QStringLiteral("blues-shuffle"), QStringLiteral("Shuffle Kit"), {
            progression("blues-12-slow", "12-bar slow change", {"I7", "I7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "V7"}),
            progression("blues-12-quick", "12-bar quick change", {"I7", "IV7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "V7"}),
            progression("blues-8", "8-bar blues", {"I7", "V7", "IV7", "IV7", "I7", "V7", "I7", "V7"}),
            progression("blues-minor", "Minor blues", {"i7", "iv7", "i7", "i7", "iv7", "iv7", "i7", "VI7", "iiø", "V7", "i7", "V7"}),
            progression("blues-jazz", "Jazz blues", {"I7", "IV7", "I7", "vi7", "ii7", "V7", "I7", "VI7", "ii7", "V7", "I7", "V7"}),
            progression("blues-cycle", "Cycle-dominant turnaround", {"I7", "III7", "VI7", "II7", "V7", "IV7", "I7", "V7"}),
        }},
        {QStringLiteral("anime-jpop"), QStringLiteral("Anime / J-Pop"), 112, 174,
            QStringLiteral("anime-bright-layer"), QStringLiteral("Anime / J-Pop Bright Layer"),
            QStringLiteral("anime-bell-lead"), QStringLiteral("Bright Bell Lead"),
            QStringLiteral("anime-punch"), QStringLiteral("Punchy J-Pop Kit"), {
            progression("anime-4536", "IV–V–iii–vi", {"IV", "V", "iii", "vi"}),
            progression("anime-4561", "IV–V–vi–I/iii", {"IV", "V", "vi", "I/iii"}),
            progression("anime-1564", "I–V/vi–vi–IV", {"I", "V/vi", "vi", "IV"}),
            progression("anime-6453", "vi–IV–V–iii", {"vi", "IV", "V", "iii"}),
            progression("anime-134m", "I–III–IV–iv", {"I", "III", "IV", "iv"}),
            progression("anime-descending", "Descending-bass circle", {"I", "V/vii", "vi", "iii", "IV", "I/iii", "ii", "V"}),
        }},
        {QStringLiteral("country"), QStringLiteral("Country"), 84, 144,
            QStringLiteral("country-pluck"), QStringLiteral("Country Pluck"),
            QStringLiteral("country-whistle"), QStringLiteral("Country Whistle Lead"),
            QStringLiteral("country-train"), QStringLiteral("Country Train Kit"), {
            progression("country-145", "I–IV–V", {"I", "IV", "V", "I"}),
            progression("country-154", "I–V–IV", {"I", "V", "IV", "I"}),
            progression("country-1645", "I–vi–IV–V", {"I", "vi", "IV", "V"}),
            progression("country-1341", "I–iii–IV–I", {"I", "iii", "IV", "I"}),
            progression("country-6415", "vi–IV–I–V", {"vi", "IV", "I", "V"}),
            progression("country-1v2", "I–V/ii–ii–V", {"I", "V/ii", "ii", "V"}),
        }},
        {QStringLiteral("edm"), QStringLiteral("EDM"), 112, 140,
            QStringLiteral("edm-supersaw"), QStringLiteral("EDM Supersaw / Pluck"),
            QStringLiteral("edm-pluck-lead"), QStringLiteral("EDM Pluck Lead"),
            QStringLiteral("edm-electronic"), QStringLiteral("Electronic Dance Kit"), {
            progression("edm-6415", "vi–IV–I–V", {"vi", "IV", "I", "V"}),
            progression("edm-minor-loop", "i–♭VI–♭III–♭VII", {"i", "bVI", "bIII", "bVII"}),
            progression("edm-descent", "i–♭VII–♭VI–♭VII", {"i", "bVII", "bVI", "bVII"}),
            progression("edm-1564", "I–V–vi–IV", {"I", "V", "vi", "IV"}),
            progression("edm-4156", "IV–I–V–vi", {"IV", "I", "V", "vi"}),
            progression("edm-pedal", "Tonic / drop pedal", {"i", "bVI/i", "bVII/i", "i"}),
        }},
        {QStringLiteral("rnb-soul"), QStringLiteral("R&B / Soul"), 68, 116,
            QStringLiteral("rnb-soft-ep"), QStringLiteral("R&B Soft Electric Piano"),
            QStringLiteral("rnb-sine-lead"), QStringLiteral("Soft Soul Lead"),
            QStringLiteral("rnb-pocket"), QStringLiteral("Deep Pocket Kit"), {
            progression("rnb-1625", "Imaj7–vi7–ii7–V7", {"Imaj7", "vi7", "ii7", "V7"}),
            progression("rnb-4362", "IVmaj7–iii7–vi7–ii7", {"IVmaj7", "iii7", "vi7", "ii7"}),
            progression("rnb-251", "ii7–V7–Imaj7", {"ii7", "V7", "Imaj7", "Imaj7"}),
            progression("rnb-minor4", "i7–IV7", {"i7", "IV7", "i7", "IV7"}),
            progression("rnb-backdoor", "Imaj7–iii7–iv7–♭VII7", {"Imaj7", "iii7", "iv7", "bVII7"}),
            progression("rnb-plagal", "IV–iv–I descending bass", {"IVmaj7", "iv7", "I/iii", "Imaj7"}),
        }},
        {QStringLiteral("funk"), QStringLiteral("Funk"), 86, 122,
            QStringLiteral("funk-clav"), QStringLiteral("Funk Clav"),
            QStringLiteral("funk-warm-lead"), QStringLiteral("Warm Funk Lead"),
            QStringLiteral("funk-dry"), QStringLiteral("Dry Funk Kit"), {
            progression("funk-static-1", "Static I7", {"I7", "I7", "I7", "I7"}),
            progression("funk-static-minor", "Static i7", {"i7", "i7", "i7", "i7"}),
            progression("funk-14", "I7–IV7", {"I7", "I7", "IV7", "IV7"}),
            progression("funk-minor-b7", "i7–♭VII7", {"i7", "bVII7", "i7", "bVII7"}),
            progression("funk-25", "ii7–V7", {"ii7", "V7", "ii7", "V7"}),
            progression("funk-chromatic", "Dominant pedal chromatic", {"I7", "bII7/I", "II7/I", "I7"}),
        }},
        {QStringLiteral("hiphop-trap"), QStringLiteral("Hip-Hop / Trap"), 62, 152,
            QStringLiteral("trap-dark-keys"), QStringLiteral("Hip-Hop / Trap Dark Keys / Bell"),
            QStringLiteral("trap-bell-lead"), QStringLiteral("Dark Bell Lead"),
            QStringLiteral("trap-808"), QStringLiteral("808-like Kit"), {
            progression("trap-1b6b7", "i–♭VI–♭VII", {"i", "bVI", "bVII", "i"}),
            progression("trap-1b3b74", "i–♭III–♭VII–iv", {"i", "bIII", "bVII", "iv"}),
            progression("trap-14b65", "i–iv–♭VI–V", {"i", "iv", "bVI", "V"}),
            progression("trap-descent", "i–♭VII–♭VI", {"i", "bVII", "bVI", "i"}),
            progression("trap-phrygian", "i–♭II", {"i", "bII", "i", "bII"}),
            progression("trap-sparse", "Sparse descending minor", {"i", "bVII", "bVI", "v"}),
        }},
    };
    return values;
}

const QVector<MoodDef>& moods()
{
    static const QVector<MoodDef> values{
        {QStringLiteral("bright"), QStringLiteral("Bright"), 8},
        {QStringLiteral("dark"), QStringLiteral("Dark"), -8},
        {QStringLiteral("dreamy"), QStringLiteral("Dreamy"), -10},
        {QStringLiteral("romantic"), QStringLiteral("Romantic"), -6},
        {QStringLiteral("peaceful"), QStringLiteral("Peaceful"), -16},
        {QStringLiteral("intense"), QStringLiteral("Intense"), 14},
        {QStringLiteral("nostalgic"), QStringLiteral("Nostalgic"), -4},
        {QStringLiteral("cinematic"), QStringLiteral("Cinematic"), -2},
    };
    return values;
}

const StyleDef& resolvedStyle(const QString& id, Rng& rng)
{
    for (const StyleDef& style : styles()) if (style.id == id) return style;
    return choose(styles(), rng);
}

const MoodDef& resolvedMood(const QString& id, Rng& rng)
{
    for (const MoodDef& mood : moods()) if (mood.id == id) return mood;
    return choose(moods(), rng);
}

bool preferFlats(int key)
{
    return key == 1 || key == 3 || key == 5 || key == 8 || key == 10;
}

ModeDef resolvedMode(const StyleDef& style, const MoodDef& mood, const ProgressionDef& progression, Rng& rng)
{
    const ModeDef major{QStringLiteral("Major"), {0, 2, 4, 5, 7, 9, 11}, false};
    const ModeDef minor{QStringLiteral("Natural Minor"), {0, 2, 3, 5, 7, 8, 10}, true};
    if (style.id == QStringLiteral("blues"))
        return {QStringLiteral("Blues"), {0, 3, 5, 6, 7, 10, 0}, true};
    if (style.id == QStringLiteral("modal-vamp")) {
        if (progression.id.contains(QStringLiteral("dorian"))) return {QStringLiteral("Dorian"), {0,2,3,5,7,9,10}, true};
        if (progression.id.contains(QStringLiteral("lydian"))) return {QStringLiteral("Lydian"), {0,2,4,6,7,9,11}, false};
        if (progression.id.contains(QStringLiteral("phrygian"))) return {QStringLiteral("Phrygian"), {0,1,3,5,7,8,10}, true};
        if (progression.id.contains(QStringLiteral("mixolydian"))) return {QStringLiteral("Mixolydian"), {0,2,4,5,7,9,10}, false};
        if (progression.id.contains(QStringLiteral("aeolian"))) return minor;
        const QVector<ModeDef> modal{{QStringLiteral("Lydian"), {0,2,4,6,7,9,11}, false},
            {QStringLiteral("Dorian"), {0,2,3,5,7,9,10}, true},
            {QStringLiteral("Mixolydian"), {0,2,4,5,7,9,10}, false}};
        return choose(modal, rng);
    }
    if (mood.id == QStringLiteral("dark") || mood.id == QStringLiteral("intense") ||
        style.id == QStringLiteral("hiphop-trap") || progression.name.startsWith(QLatin1Char('i'))) return minor;
    if (mood.id == QStringLiteral("dreamy") &&
        style.id != QStringLiteral("blues") && style.id != QStringLiteral("country"))
        return {QStringLiteral("Lydian"), {0,2,4,6,7,9,11}, false};
    if (mood.id == QStringLiteral("nostalgic"))
        return {QStringLiteral("Mixolydian"), {0,2,4,5,7,9,10}, false};
    if (mood.id == QStringLiteral("cinematic"))
        return {QStringLiteral("Dorian"), {0,2,3,5,7,9,10}, true};
    return major;
}

int romanDegree(const QString& numeral)
{
    const QString upper = numeral.toUpper();
    if (upper.startsWith(QStringLiteral("VII"))) return 6;
    if (upper.startsWith(QStringLiteral("VI"))) return 5;
    if (upper.startsWith(QStringLiteral("IV"))) return 3;
    if (upper.startsWith(QStringLiteral("V"))) return 4;
    if (upper.startsWith(QStringLiteral("III"))) return 2;
    if (upper.startsWith(QStringLiteral("II"))) return 1;
    return 0;
}

int numeralLength(const QString& numeral)
{
    const QString upper = numeral.toUpper();
    if (upper.startsWith(QStringLiteral("VII")) || upper.startsWith(QStringLiteral("III"))) return 3;
    if (upper.startsWith(QStringLiteral("VI")) || upper.startsWith(QStringLiteral("IV")) || upper.startsWith(QStringLiteral("II"))) return 2;
    return 1;
}

int degreePitch(const QString& token, int key, const ModeDef& mode)
{
    QString value = token;
    int accidental = 0;
    if (value.startsWith(QStringLiteral("b")) || value.startsWith(QString::fromUtf8("♭"))) { accidental = -1; value.remove(0, 1); }
    else if (value.startsWith(QLatin1Char('#'))) { accidental = 1; value.remove(0, 1); }
    return key + mode.intervals.at(romanDegree(value)) + accidental;
}

QString realizeRoman(QString token, int key, const ModeDef& mode, bool flats)
{
    token.replace(QString::fromUtf8("♭"), QStringLiteral("b"));
    const QStringList slash = token.split(QLatin1Char('/'));
    QString main = slash.front();
    QString bassRoman;
    if (slash.size() == 2) {
        const QString target = slash.back();
        if (main.compare(QStringLiteral("V"), Qt::CaseInsensitive) == 0) {
            return chordSymbol(degreePitch(target, key, mode) + 7, QStringLiteral("7"), flats);
        }
        bassRoman = target;
    }
    int prefix = (main.startsWith(QLatin1Char('b')) || main.startsWith(QLatin1Char('#'))) ? 1 : 0;
    const QString numeral = main.mid(prefix);
    const int length = numeralLength(numeral);
    const QString letters = numeral.left(length);
    QString suffix = numeral.mid(length);
    if (suffix == QString::fromUtf8("ø")) suffix = QStringLiteral("m7b5");
    else if (suffix.startsWith(QString::fromUtf8("°"))) suffix.replace(0, 1, QStringLiteral("dim"));
    else if (letters == letters.toLower() && !suffix.startsWith(QLatin1Char('m'))) suffix.prepend(QLatin1Char('m'));
    const int root = degreePitch(main, key, mode);
    QString chord = chordSymbol(root, suffix, flats);
    if (!bassRoman.isEmpty()) chord += QLatin1Char('/') + noteName(degreePitch(bassRoman, key, mode), flats);
    return chord;
}

QString contentFingerprint(const SongSection& section, bool beats)
{
    QJsonArray primary;
    for (const QString& value : beats ? section.beatNotes : section.chords) primary.append(value);
    QJsonArray secondary;
    if (beats) {
        for (const BeatPattern& pattern : section.beatPatterns) {
            QJsonArray lanes;
            for (const QString& lane : pattern.lanes) lanes.append(lane);
            secondary.append(QJsonObject{{QStringLiteral("division"), pattern.division}, {QStringLiteral("lanes"), lanes}});
        }
    } else {
        for (const MusicalBeatPattern& pattern : section.musicalPatterns) {
            QJsonArray chords;
            QJsonArray melody;
            for (const MusicalStep& step : pattern.chords) {
                chords.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                    {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
            }
            for (const MusicalStep& step : pattern.melody) {
                melody.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                    {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity}});
            }
            secondary.append(QJsonObject{{QStringLiteral("division"), pattern.division},
                {QStringLiteral("chords"), chords}, {QStringLiteral("melody"), melody}});
        }
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(QJsonObject{{QStringLiteral("primary"), primary}, {QStringLiteral("secondary"), secondary}})
            .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

void setEvent(QVector<PlannedEvent>& events, PlannedEvent event)
{
    for (PlannedEvent& existing : events) {
        if (existing.beat == event.beat) { existing = std::move(event); return; }
    }
    events.push_back(std::move(event));
}

QVector<PlannedEvent> basePlan(
    const ProgressionDef& progression,
    int bars,
    int beatsPerBar,
    int key,
    const ModeDef& mode,
    bool flats)
{
    QVector<PlannedEvent> events;
    const int phrase = qMin(4, bars);
    for (int bar = 0; bar < bars; ++bar) {
        int source = bar % progression.romans.size();
        const int section = bar / phrase;
        if (section == 2 && progression.romans.size() >= 4) source = (source + 2) % progression.romans.size();
        QString roman = progression.romans.at(source);
        if (bar == bars - 1 && progression.id != QStringLiteral("blues-12-slow") &&
            progression.id != QStringLiteral("blues-12-quick")) roman = mode.minor ? QStringLiteral("i") : QStringLiteral("I");
        events.push_back({bar * beatsPerBar, beatsPerBar, roman, realizeRoman(roman, key, mode, flats)});
    }
    return events;
}

void applyMoodHarmony(
    QVector<PlannedEvent>& events,
    GenerationRecipe& recipe,
    const MoodDef& mood,
    int key,
    const ModeDef& mode,
    bool flats)
{
    if (events.isEmpty()) return;
    PlannedEvent& cadence = events.back();
    if (mood.id == QStringLiteral("bright")) {
        cadence.chord = chordSymbol(key, QStringLiteral("6"), flats);
        recipe.moodDecisions << QStringLiteral("Bright harmony gives the tonic cadence an open added-sixth colour.");
    } else if (mood.id == QStringLiteral("dark")) {
        cadence.chord = chordSymbol(key, QStringLiteral("m7"), flats);
        recipe.moodDecisions << QStringLiteral("Dark harmony uses the natural-minor collection and a low-colour minor-seventh tonic.");
    } else if (mood.id == QStringLiteral("dreamy")) {
        cadence.chord = chordSymbol(key, mode.minor ? QStringLiteral("madd9") : QStringLiteral("add9"), flats);
        recipe.moodDecisions << QStringLiteral("Dreamy harmony uses Lydian colour and an unresolved added ninth at the cadence.");
    } else if (mood.id == QStringLiteral("romantic")) {
        cadence.chord = chordSymbol(key, mode.minor ? QStringLiteral("m7") : QStringLiteral("maj7"), flats);
        recipe.moodDecisions << QStringLiteral("Romantic harmony softens the tonic with a singing seventh rather than a plain triad.");
    } else if (mood.id == QStringLiteral("peaceful")) {
        cadence.chord = chordSymbol(key, mode.minor ? QStringLiteral("madd9") : QStringLiteral("add9"), flats);
        recipe.moodDecisions << QStringLiteral("Peaceful harmony holds a consonant added ninth over the tonic for a less final arrival.");
    } else if (mood.id == QStringLiteral("intense")) {
        if (events.size() >= 2) {
            events[events.size() - 2].roman = QStringLiteral("V7");
            events[events.size() - 2].chord = chordSymbol(key + 7, QStringLiteral("7"), flats);
        }
        recipe.moodDecisions << QStringLiteral("Intense harmony sharpens the final approach with an explicit dominant into the minor tonic.");
    } else if (mood.id == QStringLiteral("nostalgic")) {
        cadence.chord = chordSymbol(key, QStringLiteral("7"), flats);
        recipe.moodDecisions << QStringLiteral("Nostalgic harmony uses Mixolydian colour and a tonic dominant seventh associated with older roots styles.");
    } else {
        cadence.chord = chordSymbol(key, QStringLiteral("sus2"), flats);
        recipe.moodDecisions << QStringLiteral("Cinematic harmony uses Dorian colour and a suspended tonic that can support a scene-like continuation.");
    }
}

void addTheory(
    QVector<PlannedEvent>& events,
    GenerationRecipe& recipe,
    int key,
    const ModeDef& mode,
    bool flats,
    Rng& rng)
{
    static constexpr std::array<int, 8> perEight{0, 1, 1, 2, 2, 3, 3, 4};
    int budget = perEight.at(recipe.complexity - 1) * ((recipe.bars + 7) / 8);
    if (budget > 0 && std::uniform_int_distribution<int>(0, 4)(rng) == 0) --budget;
    QVector<int> techniques;
    for (int tier = 2; tier <= recipe.complexity; ++tier) techniques.push_back(tier);
    const auto weight = [&](int tier, int copies) {
        if (tier > recipe.complexity) return;
        for (int copy = 0; copy < copies; ++copy) techniques.push_back(tier);
    };
    if (recipe.styleId == QStringLiteral("jazz") || recipe.styleId == QStringLiteral("rnb-soul")) {
        weight(2, 2); weight(4, 3); weight(6, 2); weight(7, 2);
    } else if (recipe.styleId == QStringLiteral("modal-vamp") || recipe.styleId == QStringLiteral("indie")) {
        weight(2, 2); weight(3, 4); weight(5, 1);
    } else if (recipe.styleId == QStringLiteral("blues") || recipe.styleId == QStringLiteral("funk")) {
        weight(2, 1); weight(4, 2); weight(5, 3); weight(6, 2);
    } else if (recipe.styleId == QStringLiteral("hiphop-trap") || recipe.styleId == QStringLiteral("edm")) {
        weight(2, 2); weight(3, 2); weight(5, 2); weight(8, 1);
    } else {
        weight(2, 3); weight(3, 2); weight(4, 3); weight(5, 1);
    }
    const int half = qMax(1, recipe.beatsPerBar / 2);
    int previousTechnique = 0;
    for (int operation = 0; operation < budget; ++operation) {
        const int bar = 1 + ((operation * 3 + std::uniform_int_distribution<int>(0, qMax(1, recipe.bars - 3))(rng)) % qMax(1, recipe.bars - 2));
        const int targetBeat = bar * recipe.beatsPerBar;
        auto target = std::find_if(events.begin(), events.end(), [targetBeat](const PlannedEvent& event) { return event.beat == targetBeat; });
        if (target == events.end()) continue;
        TheoryDecision decision;
        decision.beat = targetBeat;
        decision.beforeChord = target->chord;
        const ParsedChord parsed = parseChord(target->chord);
        int technique = techniques.isEmpty() ? 0 : choose(techniques, rng);
        for (int retry = 0; retry < 3 && technique == previousTechnique && techniques.size() > 1; ++retry)
            technique = choose(techniques, rng);
        previousTechnique = technique;
        if (technique == 2) {
            if (!parsed.valid || parsed.intervals.size() < 2 || parsed.bass >= 0) continue;
            target->chord += QLatin1Char('/') + noteName(parsed.root + parsed.intervals.at(1), flats);
            target->roman += QStringLiteral(" (first inversion)");
            decision.kind = QStringLiteral("inversion");
            decision.afterChord = target->chord;
            decision.analysis = QStringLiteral("First inversion");
            decision.resolutionTarget = target->chord;
            decision.explanation = QStringLiteral("The third is placed in the bass to make the bass line move more smoothly without making the chord denser.");
        } else if (technique == 3) {
            target->roman = QStringLiteral("iv");
            target->chord = chordSymbol(key + 5, QStringLiteral("m"), flats);
            decision.kind = QStringLiteral("modal-interchange");
            decision.afterChord = target->chord;
            decision.analysis = QStringLiteral("iv borrowed from the parallel minor");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("The minor subdominant borrows darker colour from the parallel minor while preserving the phrase shape.");
        } else if (technique == 4) {
            if (!parsed.valid) continue;
            const QString dominant = chordSymbol(parsed.root + 7, QStringLiteral("7"), flats);
            const int setupBeat = qMax(0, targetBeat - half);
            setEvent(events, {setupBeat, half, QStringLiteral("V/%1").arg(target->roman), dominant});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("secondary-dominant");
            decision.afterChord = dominant;
            decision.analysis = QStringLiteral("V/%1").arg(target->roman);
            decision.resolutionTarget = target->chord;
            decision.explanation = QStringLiteral("%1 is the dominant of %2, briefly tonicising that destination before resolving to it.")
                .arg(dominant, target->chord);
        } else if (technique == 5) {
            if (!parsed.valid) continue;
            const QString passing = chordSymbol(parsed.root - 1, QStringLiteral("dim7"), flats);
            const int setupBeat = qMax(0, targetBeat - 1);
            setEvent(events, {setupBeat, 1, QStringLiteral("chromatic °7"), passing});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("passing-diminished");
            decision.afterChord = passing;
            decision.analysis = QStringLiteral("Chromatic passing diminished seventh");
            decision.resolutionTarget = target->chord;
            decision.explanation = QStringLiteral("A diminished chord a semitone below the target creates a short chromatic bass approach.");
        } else if (technique == 6) {
            const QString backdoor = chordSymbol(key + 10, QStringLiteral("7"), flats);
            const int setupBeat = qMax(0, targetBeat - half);
            setEvent(events, {setupBeat, half, QStringLiteral("♭VII7"), backdoor});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("backdoor-dominant");
            decision.afterChord = backdoor;
            decision.analysis = QStringLiteral("♭VII7 backdoor dominant");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("The borrowed flat-seven dominant gives a softer dominant-to-tonic arrival through shared and descending tones.");
        } else if (technique == 7) {
            const QString substitute = chordSymbol(key + 1, QStringLiteral("7"), flats);
            const int setupBeat = qMax(0, targetBeat - half);
            setEvent(events, {setupBeat, half, QStringLiteral("tritone sub V"), substitute});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("tritone-substitution");
            decision.afterChord = substitute;
            decision.analysis = QStringLiteral("Tritone substitute for V7");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("The dominant is replaced by the chord a tritone away, retaining its guide-tone pull while adding chromatic bass motion.");
        } else if (technique == 8) {
            if (!parsed.valid) continue;
            const QString shifted = chordSymbol(parsed.root + 2,
                mode.minor ? QStringLiteral("m7") : QStringLiteral("maj7"), flats);
            target->chord = shifted;
            decision.kind = QStringLiteral("temporary-modulation");
            decision.afterChord = shifted;
            decision.analysis = QStringLiteral("Temporary whole-step tonal excursion");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("The contrasting phrase briefly treats the key a whole step above as a local tonic before the return phrase restores the home key.");
        } else {
            continue;
        }
        recipe.theoryDecisions.push_back(std::move(decision));
    }
    std::sort(events.begin(), events.end(), [](const PlannedEvent& left, const PlannedEvent& right) { return left.beat < right.beat; });
    for (int index = 0; index < events.size(); ++index) {
        events[index].duration = (index + 1 < events.size())
            ? qMax(1, events[index + 1].beat - events[index].beat)
            : qMax(1, recipe.bars * recipe.beatsPerBar - events[index].beat);
    }
}

int nearestMidi(int pitchClass, int previous)
{
    int best = 60 + ((pitchClass % 12 + 12) % 12);
    int distance = std::abs(best - previous);
    for (int midi = 55; midi <= 79; ++midi) {
        if (midi % 12 != (pitchClass % 12 + 12) % 12) continue;
        const int candidate = std::abs(midi - previous);
        if (candidate < distance) { best = midi; distance = candidate; }
    }
    while (best - previous > 7) best -= 12;
    while (previous - best > 7) best += 12;
    return std::clamp(best, 48, 84);
}

void generateLegacyMotif(SongSection& section, GenerationRecipe& recipe, int key, const ModeDef& mode, bool flats, Rng& rng)
{
    const int cellLength = std::uniform_int_distribution<int>(2, 4)(rng);
    QVector<int> degrees;
    QString rhythm;
    const QString mood = recipe.moodId;
    int degree = std::uniform_int_distribution<int>(
        mood == QStringLiteral("dark") || mood == QStringLiteral("nostalgic") ? 2 : 0,
        mood == QStringLiteral("bright") || mood == QStringLiteral("cinematic") ? 5 : 4)(rng);
    for (int index = 0; index < cellLength; ++index) {
        int minimumStep = -2;
        int maximumStep = 2;
        if (mood == QStringLiteral("bright")) minimumStep = -1;
        else if (mood == QStringLiteral("dark") || mood == QStringLiteral("nostalgic")) maximumStep = 1;
        else if (mood == QStringLiteral("romantic") || mood == QStringLiteral("peaceful")) {
            minimumStep = -1;
            maximumStep = 1;
        } else if (mood == QStringLiteral("intense") || mood == QStringLiteral("cinematic")) {
            minimumStep = -3;
            maximumStep = 3;
        }
        if (index > 0) degree = std::clamp(degree + std::uniform_int_distribution<int>(minimumStep, maximumStep)(rng), 0, 6);
        degrees.push_back(degree);
        const int roll = std::uniform_int_distribution<int>(0, 9)(rng);
        const int onsetLimit = mood == QStringLiteral("peaceful") ? 5 :
            mood == QStringLiteral("dreamy") ? 6 : mood == QStringLiteral("intense") ? 8 : 7;
        rhythm += index == 0 || roll < onsetLimit ? QLatin1Char('O') :
            (roll < 9 ? QLatin1Char('H') : QLatin1Char('R'));
    }
    QStringList degreeText;
    for (int value : degrees) degreeText << QString::number(value + 1);
    recipe.motifCell = degreeText.join(QStringLiteral("–"));
    recipe.motifRhythm = rhythm + QStringLiteral(" (O=onset, H=hold, R=rest)");
    QVector<int> phraseTranspose{0, 1, 3, 0};
    QVector<int> phraseRotation{0, 0, 1, 0};
    if (recipe.bars <= 4) {
        recipe.motifForm = QStringLiteral("A");
    } else if (recipe.bars <= 8) {
        const bool answerFirst = std::uniform_int_distribution<int>(0, 2)(rng) == 0;
        recipe.motifForm = answerFirst ? QStringLiteral("A–B") : QStringLiteral("A–A′");
        phraseTranspose[1] = answerFirst ? 3 : 1;
        phraseRotation[1] = answerFirst ? 1 : 0;
    } else if (recipe.bars <= 12) {
        const bool returnEarly = std::uniform_int_distribution<int>(0, 2)(rng) == 0;
        recipe.motifForm = returnEarly ? QStringLiteral("A–B–A′") : QStringLiteral("A–A′–B");
        if (returnEarly) {
            phraseTranspose = {0, 3, 1, 0};
            phraseRotation = {0, 1, 0, 0};
        }
    } else {
        const int variant = std::uniform_int_distribution<int>(0, 3)(rng);
        if (variant == 1) {
            recipe.motifForm = QStringLiteral("A–B–A′–A″");
            phraseTranspose = {0, 3, 1, 0};
            phraseRotation = {0, 1, 0, 0};
        } else if (variant == 2) {
            recipe.motifForm = QStringLiteral("A–A′–B–B′");
            phraseTranspose = {0, 1, 3, 4};
            phraseRotation = {0, 0, 1, 2};
        } else if (variant == 3) {
            recipe.motifForm = QStringLiteral("A–B–C–A′");
            phraseTranspose = {0, 3, 5, 1};
            phraseRotation = {0, 1, 2, 0};
        } else {
            recipe.motifForm = QStringLiteral("A–A′–B–A″");
        }
    }
    recipe.motifTransformations << QStringLiteral("A states the seed-specific %1-beat cell.").arg(cellLength);
    recipe.moodDecisions << QStringLiteral("%1 motif shaping changes the cell's contour range and its balance of onsets, holds, and rests.").arg(recipe.moodName);
    if (recipe.bars >= 8) recipe.motifTransformations << QStringLiteral("Repeated material may be sequenced or answered from a different scale degree instead of being copied literally.");
    if (recipe.bars >= 12) recipe.motifTransformations << QStringLiteral("Contrasting phrases rotate the seed contour and start at a different point in the cell.");
    if (recipe.bars >= 16) recipe.motifTransformations << QStringLiteral("This seed chose %1; the last phrase changes its cadence even when earlier material returns.").arg(recipe.motifForm);
    int previous = 67;
    for (int beat = 0; beat < section.beats; ++beat) {
        const int bar = beat / recipe.beatsPerBar;
        const int formSection = bar / 4;
        int cellIndex = (beat + phraseRotation.value(formSection, 0)) % cellLength;
        const int transform = phraseTranspose.value(formSection, 0);
        const QChar state = rhythm.at(cellIndex);
        if (state == QLatin1Char('R')) { section.targets[beat] = QStringLiteral("-"); continue; }
        if (state == QLatin1Char('H')) { section.targets[beat].clear(); continue; }
        int motifDegree = (degrees.at(cellIndex) + transform) % 7;
        if (beat == section.beats - 1) motifDegree = 0;
        previous = nearestMidi(key + mode.intervals.at(motifDegree), previous);
        section.targets[beat] = noteName(previous % 12, flats) + QString::number(previous / 12 - 1);
    }
}

int pitchClass(int value)
{
    return (value % 12 + 12) % 12;
}

bool includesPitchClass(const QVector<int>& values, int value)
{
    const int wanted = pitchClass(value);
    return std::any_of(values.cbegin(), values.cend(), [wanted](int candidate) {
        return pitchClass(candidate) == wanted;
    });
}

QVector<int> chordPitchClasses(const ParsedChord& chord)
{
    QVector<int> result;
    if (!chord.valid || chord.rest) return result;
    for (int interval : chord.intervals) {
        const int value = pitchClass(chord.root + interval);
        if (!result.contains(value)) result.push_back(value);
    }
    return result;
}

QString chordRole(const ParsedChord& chord, int midi)
{
    if (!chord.valid || chord.rest) return QStringLiteral("No sounding chord");
    const int interval = pitchClass(midi - chord.root);
    if (interval == 0) return QStringLiteral("Root");
    if (interval == 3 || interval == 4) return QStringLiteral("Third defining the chord quality");
    if (interval == 7) return QStringLiteral("Fifth stabilising the chord");
    if (interval == 10 || interval == 11) return QStringLiteral("Seventh / guide tone");
    if (includesPitchClass(chord.intervals, interval)) return QStringLiteral("Chord extension / colour tone");
    return QStringLiteral("Intentional non-chord tone");
}

const TheoryDecision* theoryAtBeat(const GenerationRecipe& recipe, int beat)
{
    for (const TheoryDecision& decision : recipe.theoryDecisions) {
        if (decision.beat == beat) return &decision;
    }
    return nullptr;
}

int musicalDivisionForBeat(const StyleDef& style, int beat, int beatsPerBar, Rng& rng)
{
    if (style.id == QStringLiteral("jazz") || style.id == QStringLiteral("blues")) return 3;
    if (style.id == QStringLiteral("funk")) return 4;
    if (style.id == QStringLiteral("edm") || style.id == QStringLiteral("anime-jpop") ||
        style.id == QStringLiteral("hiphop-trap")) {
        return beat % beatsPerBar == beatsPerBar - 1 ||
            std::uniform_int_distribution<int>(0, 4)(rng) == 0 ? 4 : 2;
    }
    if (style.id == QStringLiteral("rock") || style.id == QStringLiteral("country") ||
        style.id == QStringLiteral("rnb-soul")) return 2;
    if (style.id == QStringLiteral("modal-vamp")) return 1;
    return beat % (beatsPerBar * 2) == beatsPerBar * 2 - 1 ? 2 : 1;
}

void generateChordRhythm(
    SongSection& section,
    GenerationRecipe& recipe,
    const StyleDef& style,
    const MoodDef& mood,
    Rng& rng)
{
    section.musicalPatterns.resize(section.beats);
    QString activeChord;
    const bool restrained = mood.id == QStringLiteral("peaceful") || mood.id == QStringLiteral("dreamy");
    for (int beat = 0; beat < section.beats; ++beat) {
        const QString written = section.chords.value(beat).trimmed();
        if (!written.isEmpty() && written != QStringLiteral("-")) activeChord = written;
        else if (written == QStringLiteral("-")) activeChord.clear();
        MusicalBeatPattern& pattern = section.musicalPatterns[beat];
        pattern.division = musicalDivisionForBeat(style, beat, recipe.beatsPerBar, rng);
        pattern.chords.fill(MusicalStep{}, pattern.division);
        pattern.melody.fill(MusicalStep{}, pattern.division);
        for (MusicalStep& step : pattern.chords) step.state = MusicalStepState::Hold;
        if (!written.isEmpty()) {
            pattern.chords[0] = {written == QStringLiteral("-") ? MusicalStepState::Rest : MusicalStepState::Onset,
                written == QStringLiteral("-") ? QString() : written, 96};
        }
        if (activeChord.isEmpty() || restrained) continue;
        const int within = beat % recipe.beatsPerBar;
        if (style.id == QStringLiteral("funk")) {
            pattern.chords[0] = {MusicalStepState::Onset, activeChord, within == 0 ? 105 : 94};
            pattern.chords[1].state = MusicalStepState::Rest;
            if (within == 1 || within == 3) {
                pattern.chords[2] = {MusicalStepState::Onset, activeChord, 86};
                pattern.chords[3].state = MusicalStepState::Rest;
            }
        } else if (style.id == QStringLiteral("edm")) {
            pattern.chords[0] = {MusicalStepState::Onset, activeChord, within == 0 ? 104 : 91};
            if (pattern.division > 1) pattern.chords[1].state = MusicalStepState::Rest;
        } else if ((style.id == QStringLiteral("jazz") || style.id == QStringLiteral("blues")) &&
                   (within == 1 || within == 3)) {
            pattern.chords[2] = {MusicalStepState::Onset, activeChord,
                style.id == QStringLiteral("jazz") ? 78 : 84};
        } else if ((style.id == QStringLiteral("rock") || style.id == QStringLiteral("country")) &&
                   pattern.division > 1) {
            pattern.chords[0] = {MusicalStepState::Onset, activeChord, within == 0 ? 105 : 91};
            pattern.chords[1].state = style.id == QStringLiteral("rock")
                ? MusicalStepState::Rest : MusicalStepState::Hold;
        }
    }
    if (style.id == QStringLiteral("pop") || style.id == QStringLiteral("indie") ||
        style.id == QStringLiteral("modal-vamp") || restrained) {
        recipe.grooveDecisions << QStringLiteral(
            "Chord rhythm mostly sustains the harmony so the melody and groove have space.");
    } else {
        recipe.grooveDecisions << QStringLiteral(
            "Chord attacks use the shared musical subdivision for %1-style comping.").arg(style.name);
    }
}

QString activeChordAtBeat(const SongSection& section, int wantedBeat)
{
    QString active;
    for (int beat = 0; beat <= wantedBeat && beat < section.beats; ++beat) {
        const QString symbol = section.chords.value(beat).trimmed();
        if (symbol == QStringLiteral("-")) active.clear();
        else if (!symbol.isEmpty()) active = symbol;
    }
    return active;
}

bool grooveAccent(const SongSection& beatSection, int beat, int step, int division)
{
    if (beat < 0 || beat >= beatSection.beatPatterns.size()) return false;
    const BeatPattern& pattern = beatSection.beatPatterns[beat];
    if (pattern.division <= 0) return false;
    const int drumStep = qBound(0,
        static_cast<int>(std::lround(static_cast<double>(step) * pattern.division / division)),
        pattern.division - 1);
    for (const QString& lane : pattern.lanes) {
        if (drumStep < lane.size() &&
            (lane[drumStep] == QLatin1Char('a') || lane[drumStep] == QLatin1Char('x'))) return true;
    }
    return false;
}

QString steps(int division, std::initializer_list<std::pair<int, QChar>> hits)
{
    QString value(division, QLatin1Char('.'));
    for (const auto& [step, state] : hits) if (step >= 0 && step < division) value[step] = state;
    return value;
}

void lane(BeatPattern& pattern, const QString& name, const QString& value)
{
    const int index = BeatGridModel::beatLaneNames().indexOf(name);
    if (index >= 0) pattern.lanes[index] = value;
}

bool addHit(BeatPattern& pattern, const QString& name, int step, QChar state)
{
    const int index = BeatGridModel::beatLaneNames().indexOf(name);
    if (index < 0 || step < 0 || step >= pattern.division) return false;
    if (pattern.lanes[index].size() != pattern.division)
        pattern.lanes[index] = QString(pattern.division, QLatin1Char('.'));
    if (pattern.lanes[index][step] != QLatin1Char('.')) return false;
    pattern.lanes[index][step] = state;
    return true;
}

struct GrooveBarDef {
    QString kick;
    QString snare;
    QString closedHat;
    QString openHat;
    QString ride;
    QString tom;
};

struct GrooveDef {
    QString styleId;
    QString id;
    QString name;
    QString core;
    int division = 2;
    GrooveBarDef first;
    GrooveBarDef second;
    QString feelName;
    int swingPercent = 50;
    int snareOffsetMs = 0;
    int timingVariationMs = 2;
    int velocityVariationPercent = 5;
};

GrooveBarDef grooveBar(
    const char* kick,
    const char* snare,
    const char* closedHat,
    const char* openHat = "",
    const char* ride = "",
    const char* tom = "")
{
    return {QString::fromLatin1(kick), QString::fromLatin1(snare),
        QString::fromLatin1(closedHat), QString::fromLatin1(openHat),
        QString::fromLatin1(ride), QString::fromLatin1(tom)};
}

GrooveDef groove(
    const char* styleId,
    const char* id,
    const char* name,
    const char* core,
    int division,
    GrooveBarDef first,
    GrooveBarDef second,
    const char* feelName,
    int swingPercent,
    int snareOffsetMs,
    int timingVariationMs,
    int velocityVariationPercent)
{
    return {QString::fromLatin1(styleId), QString::fromLatin1(id),
        QString::fromLatin1(name), QString::fromLatin1(core), division,
        std::move(first), std::move(second), QString::fromLatin1(feelName),
        swingPercent, snareOffsetMs, timingVariationMs, velocityVariationPercent};
}

const QVector<GrooveDef>& grooveFamilies()
{
    static const QVector<GrooveDef> values{
        groove("pop", "pop-straight-eighth", "Straight Eighth", "Backbeat, eighth-note hats, and a stable two-bar kick answer.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x.x..", "..a...a.", "xgxgxgxa"), "Straight pop pocket", 50, 0, 2, 5),
        groove("pop", "pop-four-floor", "Four-on-the-Floor", "Quarter-note kick under a bright pop backbeat.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".x.x.x.x"), grooveBar("a.x.x.xx", "..a...a.", "x.x.x.x.", ".x.x.x.x"), "Forward dance-pop", 50, -2, 2, 6),
        groove("pop", "pop-syncopated-kick", "Syncopated Kick", "A familiar pop backbeat with answering off-beat kicks.", 2,
            grooveBar("a..x.x..", "..a...a.", "xgxgxgxg"), grooveBar("a.x..x.x", "..a...a.", "xgxgxgxa"), "Syncopated straight", 50, 1, 3, 7),
        groove("pop", "pop-half-time", "Half-Time Pop", "Beat-three backbeat with spacious kick pickups.", 2,
            grooveBar("a..x....", "....a...", "xgxgxgxg"), grooveBar("a....x..", "....a...", "xgxgxgxa"), "Wide half-time", 50, 5, 3, 6),
        groove("pop", "pop-disco", "Disco Pop", "Four-on-the-floor kick with alternating closed and open hats.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".a.a.a.a"), grooveBar("a.x.x.xx", "..a...a.", "a.x.x.x.", ".x.x.x.a"), "Lifted disco", 50, -1, 2, 7),

        groove("indie", "indie-loose-backbeat", "Loose Backbeat", "Asymmetric kick answers and lightly broken eighth-note hats.", 2,
            grooveBar("a....x..", "..a...a.", "xgxx.gxg"), grooveBar("a..x...x", "..a...a.", "x.xgxgxa"), "Loose room pocket", 52, 5, 4, 9),
        groove("indie", "indie-motorik", "Motorik", "Steady quarter-note kick with an unwavering eighth-note pulse.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "xgxgxgxg"), grooveBar("a.x.x.x.", "..a...a.", "agxgxgxg"), "Motorik straight", 50, 0, 2, 5),
        groove("indie", "indie-floor-tom", "Floor-Tom Pulse", "Floor-tom answers colour a restrained indie backbeat.", 2,
            grooveBar("a...x...", "..a...a.", "x.x.x.x.", "", "", "....x..."), grooveBar("a....x..", "..a...a.", "x.x...x.", "", "", "x.....x."), "Tom-led room feel", 50, 4, 4, 8),
        groove("indie", "indie-half-time", "Indie Half-Time", "A soft beat-three backbeat with displaced kick movement.", 2,
            grooveBar("a..x....", "....a...", "x.xgx.xg"), grooveBar("a....x.x", "....a...", "xgx.xgxa"), "Laid-back half-time", 55, 9, 4, 8),
        groove("indie", "indie-garage-sync", "Garage Syncopation", "Dry sixteenth-note energy with uneven kick answers.", 4,
            grooveBar("a......xx.......", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a...x.....x...x.", "....a.......a...", "ag.xxg.xag.x.g.x"), "Urgent garage push", 51, -4, 3, 10),

        groove("rock", "rock-driving-eighth", "Driving Eighth", "Firm backbeat, eighth hats, and forward kick movement.", 2,
            grooveBar("a...x.x.", "..a...a.", "xaxgxaxg"), grooveBar("a..x.x..", "..a...a.", "xaxgxaxa"), "Driving straight", 50, -3, 2, 6),
        groove("rock", "rock-four-floor", "Four-on-the-Floor Rock", "Quarter-note kick beneath a live rock backbeat.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "xgxgxgxg"), grooveBar("a.x.x.xx", "..a...a.", "agxgxgxg"), "Big straight rock", 50, -2, 3, 7),
        groove("rock", "rock-half-time", "Half-Time Rock", "Heavy beat-three snare with kick pickups into the backbeat.", 2,
            grooveBar("a..x....", "....a...", "xaxgxaxg"), grooveBar("a....x.x", "....a...", "xaxgxaxa"), "Heavy half-time", 50, 4, 3, 7),
        groove("rock", "rock-alt-sync", "Alternative Syncopation", "Sixteenth-note kick displacement around a stable backbeat.", 4,
            grooveBar("a.....x.x...x...", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a.......a...", "ag.xxg.xag.xxg.x"), "Tight alternative rock", 50, 0, 3, 8),
        groove("rock", "rock-shuffle", "Rock Shuffle", "Triplet skip pulse with a broad two-and-four backbeat.", 3,
            grooveBar("a.....x.....", "...a.....a..", "x.gx.gx.gx.g"), grooveBar("a..x..x..x..", "...a.....a..", "a.gx.gx.gx.g"), "Hard shuffle", 67, 1, 3, 7),

        groove("jazz", "jazz-medium-swing", "Medium Swing Ride", "Spang-a-lang Ride with feathered kick and soft comping.", 3,
            grooveBar("g..g..g..g..", "...g.....g..", "", "", "a.gx.gx.gx.g"), grooveBar("g.....g.....", "..g...g..g..", "", "", "x.gx.gx.ga.g"), "Medium swing", 67, 8, 4, 10),
        groove("jazz", "jazz-two-feel", "Two-Feel Swing", "Two-beat bass-drum support with spacious snare comping.", 3,
            grooveBar("x.....x.....", ".....g...g..", "", "", "a.gx.gx.gx.g"), grooveBar("x........x..", "..g.....g...", "", "", "x.gx.ga.gx.g"), "Relaxed two-feel", 67, 10, 4, 9),
        groove("jazz", "jazz-brush-ballad", "Brush Ballad", "Soft triplet sweep suggestion and restrained backbeat taps.", 3,
            grooveBar("g.....g.....", "...x.....x..", "", "", "x.gx.gx.gx.g"), grooveBar("g........g..", "..g...x..x..", "", "", "x.gx.ga.gx.g"), "Brush ballad drag", 64, 15, 5, 12),
        groove("jazz", "jazz-uptempo-ride", "Up-Tempo Ride", "Continuous Ride skip pattern with active feathering and comping.", 3,
            grooveBar("g..g..g..g..", "..g...g..g..", "", "", "a.gx.ag.gx.g"), grooveBar("g..g..g..g..", "...g..g..g..", "", "", "x.ag.gx.ga.g"), "Up-tempo swing", 64, -4, 2, 8),
        groove("jazz", "jazz-funk", "Jazz-Funk", "Straight sixteenth hat texture over a syncopated pocket.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a.......a...", "ag.xxg.xag.xxg.x"), "Straight jazz-funk", 54, 2, 3, 9),

        groove("modal-vamp", "modal-sparse-half", "Sparse Half-Time", "A wide beat-three backbeat leaving space around the vamp.", 2,
            grooveBar("a.......", "....a...", "x.x.x.x."), grooveBar("a....x..", "....a...", "x.x...x."), "Spacious half-time", 52, 10, 4, 7),
        groove("modal-vamp", "modal-tom-ostinato", "Tom Ostinato", "Repeating tom punctuation supports a sparse modal pulse.", 2,
            grooveBar("a.......", "....a...", "x.x...x.", "", "", "..x...x."), grooveBar("a....x..", "....a...", "x...x.x.", "", "", "x...x..."), "Grounded tom ostinato", 50, 7, 4, 8),
        groove("modal-vamp", "modal-straight-pulse", "Straight Pulse", "Even eighth-note motion with restrained kick and backbeat anchors.", 2,
            grooveBar("a...x...", "..x...a.", "xgxgxgxg"), grooveBar("a....x..", "..x...a.", "xgxgxgxa"), "Steady modal pulse", 50, 4, 3, 6),
        groove("modal-vamp", "modal-triplet-cross", "Triplet Cross-Pulse", "Triplet cymbal cells and tom answers create a modal cross-pulse.", 3,
            grooveBar("a.....x.....", "......a.....", "", "", "x.gx.gx.gx.g", "..x.....x..."), grooveBar("a........x..", "......a.....", "", "", "a.gx.gx.ga.g", ".....x.....x"), "Triplet modal cycle", 67, 6, 4, 9),
        groove("modal-vamp", "modal-ride-led", "Ride-Led Vamp", "A restrained Ride pulse opens space between kick and snare statements.", 2,
            grooveBar("a.......", "....a...", "", "", "xgxgxgxg"), grooveBar("a....x..", "....a...", "", "", "agxgxgxa"), "Floating Ride pocket", 55, 8, 4, 8),

        groove("blues", "blues-slow-shuffle", "Slow Shuffle", "Triplet skip hats with a grounded two-and-four backbeat.", 3,
            grooveBar("a.....x.....", "...a.....a..", "x.gx.gx.gx.g"), grooveBar("a........x..", "...a.....a..", "a.gx.gx.ga.g"), "Slow shuffle", 67, 8, 4, 9),
        groove("blues", "blues-texas-shuffle", "Texas Shuffle", "Active shuffled kick and hats push against the backbeat.", 3,
            grooveBar("a..x..x..x..", "...a.....a..", "a.gx.gx.gx.g"), grooveBar("a.....x..x..", "...a.....a..", "x.ga.gx.ga.g"), "Driving Texas shuffle", 67, -2, 3, 8),
        groove("blues", "blues-straight-eighth", "Straight-Eighth Blues", "Straight eighth hats and a simple blues backbeat.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x.x..", "..a...a.", "agxgxgxg"), "Straight blues", 50, 3, 3, 7),
        groove("blues", "blues-half-time", "Half-Time Blues", "Triplet pulse around a heavy beat-three snare.", 3,
            grooveBar("a..x........", "......a.....", "x.gx.gx.gx.g"), grooveBar("a.....x..x..", "......a.....", "a.gx.gx.ga.g"), "Half-time shuffle", 67, 10, 4, 8),
        groove("blues", "blues-train-shuffle", "Train Shuffle", "Alternating kick and snare ghosts suggest a train-beat shuffle.", 3,
            grooveBar("a.....x.....", ".g.a.g...a.g", "x.gx.gx.gx.g"), grooveBar("a..x..x.....", ".g.a..g..a.g", "a.gx.gx.ga.g"), "Rolling train shuffle", 67, 1, 4, 10),

        groove("anime-jpop", "anime-driving-pop-rock", "Driving Pop-Rock", "Bright eighth-note drive with energetic kick answers.", 2,
            grooveBar("a...x.x.", "..a...a.", "xaxgxaxg"), grooveBar("a..x.x.x", "..a...a.", "aaxgxaxa"), "Bright driving straight", 50, -4, 2, 7),
        groove("anime-jpop", "anime-double-time", "Double-Time Chorus", "Sixteenth hats and active kicks lift a chorus-like backbeat.", 4,
            grooveBar("a...x...x.x.....", "....a.......a...", "xgxaagxgxgxaagxg"), grooveBar("a.....x.x...x.x.", "....a.......a...", "agxgxgxaagxgxgxa"), "Double-time lift", 50, -6, 2, 9),
        groove("anime-jpop", "anime-syncopated-kick", "Syncopated Kick", "Off-beat kick answers energise a clean pop-rock backbeat.", 4,
            grooveBar("a.....x.x.....x.", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x......x.x...", "....a.......a...", "ag.xxg.xag.xxg.x"), "Punchy syncopation", 50, -3, 3, 9),
        groove("anime-jpop", "anime-dance-rock", "Dance-Rock", "Four-on-the-floor kick combines with bright off-beat openings.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".a.a.a.a"), grooveBar("a.x.x.xx", "..a...a.", "a.x.x.x.", ".x.x.x.a"), "Dance-rock lift", 50, -4, 2, 8),
        groove("anime-jpop", "anime-dramatic-half", "Dramatic Half-Time", "Wide half-time snare with rising kick and cymbal answers.", 2,
            grooveBar("a..x....", "....a...", "xgxgxgxg"), grooveBar("a....x.x", "....a...", "xgxgx.x.", ".......x"), "Dramatic half-time", 52, 5, 3, 9),

        groove("country", "country-boom-chick", "Boom-Chick", "Alternating bass-drum and snare anchors with an even pulse.", 2,
            grooveBar("a...x...", "..a...a.", "x.x.x.x."), grooveBar("a....x..", "..a...a.", "a.x.x.x."), "Boom-chick straight", 50, 0, 2, 5),
        groove("country", "country-train", "Train Beat", "Continuous snare ghosts drive accented two-and-four train beats.", 4,
            grooveBar("a.......x.......", "g.g.a.g.g.g.a.g.", "x...x...x...x..."), grooveBar("a.....x.x.......", "g.g.a.g.g.g.a.ga", "a...x...x...x..."), "Driving train beat", 50, -5, 3, 9),
        groove("country", "country-two-step", "Two-Step", "Clear bass-drum steps and light backbeat support.", 2,
            grooveBar("a...x...", "..x...a.", "xgxgxgxg"), grooveBar("a.x...x.", "..x...a.", "agxgxgxg"), "Light two-step", 50, -1, 2, 6),
        groove("country", "country-rock", "Country Rock", "Rock backbeat with country kick pickups and open-hat lifts.", 2,
            grooveBar("a..x.x..", "..a...a.", "xgxgxgxg"), grooveBar("a...x.xx", "..a...a.", "xgxgx.x.", ".......x"), "Country-rock drive", 50, -3, 3, 8),
        groove("country", "country-shuffle", "Country Shuffle", "Triplet skip pulse with alternating boom-chick anchors.", 3,
            grooveBar("a.....x.....", "...a.....a..", "x.gx.gx.gx.g"), grooveBar("a..x..x.....", "...a.....a..", "a.gx.gx.ga.g"), "Country shuffle", 67, 1, 3, 7),

        groove("edm", "edm-house", "House", "Four-on-the-floor kick with off-beat open hats.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".a.a.a.a"), grooveBar("a.x.x.x.", "..a...a.", "a.x.x.x.", ".x.x.x.a"), "House grid", 50, 0, 1, 4),
        groove("edm", "edm-disco-house", "Disco House", "Four-on-the-floor foundation with busier sixteenth hat movement.", 4,
            grooveBar("a...x...x...x...", "....a.......a...", "xg.xxg.xxg.xxg.x", "..a...a...a...a."), grooveBar("a...x...x...x.x.", "....a.......a...", "ag.xxg.xxg.xxg.x", "..x...a...x...a."), "Disco-house push", 50, -1, 2, 7),
        groove("edm", "edm-techno", "Techno Pulse", "Rigid quarter kicks with rotating sixteenth hat accents.", 4,
            grooveBar("a...x...x...x...", "....x.......a...", "ag.x.g.xag.x.g.x"), grooveBar("a...x...x...x...", "....a.......x...", "x.g.xg.x.xg.xg.x"), "Machine-tight techno", 50, 0, 1, 5),
        groove("edm", "edm-synthwave", "Synthwave", "Straight gated backbeat and restrained eighth-note kick movement.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x...x", "..a...a.", "agxgxgxa"), "Gated synthwave", 50, 3, 2, 6),
        groove("edm", "edm-breakbeat", "Breakbeat", "Syncopated kick and snare cells break away from four-on-the-floor.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Broken straight beat", 52, 1, 3, 8),

        groove("rnb-soul", "rnb-laid-back", "Laid-Back Pocket", "Sparse kick movement and a deep, late backbeat.", 4,
            grooveBar("a.....x.........", "........a.......", "x...xg..x...xg.."), grooveBar("a.........x...x.", "........a.......", "xg..x...x...xg.."), "Laid-back pocket", 57, 18, 5, 10),
        groove("rnb-soul", "rnb-neo-soul", "Neo-Soul Swing", "Swung sixteenth hats frame syncopated kick and ghost-snare answers.", 4,
            grooveBar("a......x..x.....", "....a..g....a...", "xg.xag.xxg.xag.x"), grooveBar("a..x......x...x.", "....a......ga...", "ag.xxg.xag.xxg.x"), "Neo-soul sixteenth swing", 60, 16, 5, 12),
        groove("rnb-soul", "rnb-motown", "Motown", "Bright eighth-note drive with strong two-and-four backbeats.", 2,
            grooveBar("a.x.x...", "..a...a.", "xaxgxaxg"), grooveBar("a...x.x.", "..a...a.", "aaxgxaxa"), "Forward soul", 52, -2, 3, 8),
        groove("rnb-soul", "rnb-half-ballad", "Half-Time Ballad", "Soft beat-three backbeat and widely spaced kick pickups.", 2,
            grooveBar("a.......", "....a...", "x.xgx.xg"), grooveBar("a....x..", "....a...", "xgx.xgxa"), "Soft half-time", 56, 20, 5, 10),
        groove("rnb-soul", "rnb-funk-soul", "Funk-Soul", "Dry sixteenth hats and syncopated kick under a warm backbeat.", 4,
            grooveBar("a.....x.x...x...", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x......x...x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Pocketed funk-soul", 55, 7, 4, 10),

        groove("funk", "funk-the-one", "The One", "A strong first beat anchors clipped sixteenth-note syncopation.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "ag.x.g.xxg.x.g.x"), grooveBar("a..x....x.....x.", "....a.......a...", "x.g.xg.xag.xxg.x"), "On-the-one funk", 54, -5, 3, 10),
        groove("funk", "funk-sync-pocket", "Syncopated Pocket", "Kick and hat answers interlock around a stable backbeat.", 4,
            grooveBar("a..x....x...x...", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a.....x...x...x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Deep syncopated funk", 57, 2, 4, 11),
        groove("funk", "funk-linear", "Linear Funk", "Kick, snare, and hat voices answer rather than constantly stacking.", 4,
            grooveBar("a.....x.....x...", "....a.......a...", ".g.x..x..g.x..x."), grooveBar("a..x......x...x.", "....a..g....a...", ".x..g.x..x..g.x."), "Linear sixteenth pocket", 55, -2, 3, 10),
        groove("funk", "funk-disco", "Disco-Funk", "Four-on-the-floor kick and open-hat lifts with syncopated closed hats.", 4,
            grooveBar("a...x...x...x...", "....a.......a...", "xg.xxg.xxg.xxg.x", "..a...a...a...a."), grooveBar("a...x...x...x.x.", "....a.......a...", "ag.xxg.xxg.xxg.x", "..x...a...x...a."), "Disco-funk lift", 52, -3, 3, 9),
        groove("funk", "funk-half-time", "Half-Time Funk", "Beat-three snare leaves room for syncopated kick and hat cells.", 4,
            grooveBar("a.....x.........", "........a.......", "xg.xag.xxg.xag.x"), grooveBar("a..x......x...x.", "........a..g....", "ag.xxg.xag.xxg.x"), "Half-time funk", 58, 6, 4, 11),

        groove("hiphop-trap", "hiphop-boom-bap", "Boom-Bap", "Swung hats, hard backbeat, and answering kick syncopation.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Boom-bap swing", 58, 8, 4, 10),
        groove("hiphop-trap", "hiphop-lofi", "Lo-Fi Swing", "Sparse kick and softened hats sit behind the backbeat.", 4,
            grooveBar("a.........x.....", "....x.......a...", "x...xg..x...xg.."), grooveBar("a......x......x.", "....x..g....a...", "xg..x...x...xg.."), "Lo-fi laid-back swing", 61, 18, 5, 12),
        groove("hiphop-trap", "trap-sparse", "Sparse Trap", "Half-time snare, minimal kick, and steady sixteenth hats.", 4,
            grooveBar("a.......x.......", "........a.......", "xg.xag.xxg.xag.x"), grooveBar("a.....x.......x.", "........a.......", "ag.xxg.xag.xxg.x"), "Sparse trap half-time", 50, 4, 3, 8),
        groove("hiphop-trap", "trap-rolling", "Rolling Trap", "Half-time anchors with recurring hat-roll and kick answers.", 4,
            grooveBar("a.....x...x.....", "........a.......", "xgxaagxgxgxaagxg"), grooveBar("a..x......x...x.", ".......ga.......", "agxgxgxaagxgxgxa"), "Rolling trap grid", 50, 1, 3, 10),
        groove("hiphop-trap", "trap-half-time-808", "Half-Time 808", "Deep displaced kick movement under a clear beat-three snare.", 4,
            grooveBar("a......x........", "........a.......", "x...xg..x...xg.."), grooveBar("a..x......x...x.", "........a..g....", "xg..x...x...xg.."), "Deep 808 half-time", 54, 8, 4, 11),
    };
    return values;
}

std::uint32_t grooveSeed(std::uint32_t seed, const QString& styleId, const QString& moodId)
{
    std::uint32_t value = seed ^ 0x6d2b79f5U;
    const QString text = styleId + QLatin1Char(':') + moodId;
    for (QChar character : text) {
        value ^= character.unicode();
        value *= 16777619U;
    }
    return value;
}

int familyWeight(const GrooveDef& family, const MoodDef& mood)
{
    int weight = 10;
    const QString id = family.id;
    if (mood.id == QStringLiteral("bright")) {
        if (id.contains(QStringLiteral("driving")) || id.contains(QStringLiteral("four-floor")) ||
            id.contains(QStringLiteral("disco")) || id.contains(QStringLiteral("dance")) ||
            id.contains(QStringLiteral("train"))) weight += 8;
        if (id.contains(QStringLiteral("half")) || id.contains(QStringLiteral("sparse"))) weight -= 4;
    } else if (mood.id == QStringLiteral("dark")) {
        if (id.contains(QStringLiteral("half")) || id.contains(QStringLiteral("sparse")) ||
            id.contains(QStringLiteral("lofi")) || id.contains(QStringLiteral("break"))) weight += 8;
    } else if (mood.id == QStringLiteral("dreamy")) {
        if (id.contains(QStringLiteral("ride")) || id.contains(QStringLiteral("ballad")) ||
            id.contains(QStringLiteral("synthwave")) || id.contains(QStringLiteral("sparse"))) weight += 8;
    } else if (mood.id == QStringLiteral("romantic")) {
        if (id.contains(QStringLiteral("ballad")) || id.contains(QStringLiteral("neo-soul")) ||
            id.contains(QStringLiteral("laid-back"))) weight += 10;
    } else if (mood.id == QStringLiteral("peaceful")) {
        if (id.contains(QStringLiteral("sparse")) || id.contains(QStringLiteral("ballad")) ||
            id.contains(QStringLiteral("two-feel")) || id.contains(QStringLiteral("half"))) weight += 10;
        if (id.contains(QStringLiteral("driving")) || id.contains(QStringLiteral("double"))) weight -= 5;
    } else if (mood.id == QStringLiteral("intense")) {
        if (id.contains(QStringLiteral("driving")) || id.contains(QStringLiteral("double")) ||
            id.contains(QStringLiteral("rolling")) || id.contains(QStringLiteral("techno"))) weight += 10;
        if (id.contains(QStringLiteral("ballad")) || id.contains(QStringLiteral("sparse"))) weight -= 5;
    } else if (mood.id == QStringLiteral("nostalgic")) {
        if (id.contains(QStringLiteral("motown")) || id.contains(QStringLiteral("shuffle")) ||
            id.contains(QStringLiteral("synthwave")) || id.contains(QStringLiteral("boom-bap"))) weight += 10;
    } else if (mood.id == QStringLiteral("cinematic")) {
        if (id.contains(QStringLiteral("tom")) || id.contains(QStringLiteral("dramatic")) ||
            id.contains(QStringLiteral("half")) || id.contains(QStringLiteral("cross"))) weight += 9;
    }
    return qMax(2, weight);
}

const GrooveDef& chooseGroove(const StyleDef& style, const MoodDef& mood, Rng& rng)
{
    QVector<const GrooveDef*> matching;
    int totalWeight = 0;
    for (const GrooveDef& family : grooveFamilies()) {
        if (family.styleId != style.id) continue;
        matching.push_back(&family);
        totalWeight += familyWeight(family, mood);
    }
    int draw = std::uniform_int_distribution<int>(1, qMax(1, totalWeight))(rng);
    for (const GrooveDef* family : matching) {
        draw -= familyWeight(*family, mood);
        if (draw <= 0) return *family;
    }
    return *matching.constLast();
}

QString normalizedBarLane(const QString& source, int steps)
{
    QString result = source.left(steps);
    if (result.size() < steps) result += QString(steps - result.size(), QLatin1Char('.'));
    for (int index = 0; index < result.size(); ++index) {
        const QChar lower = result[index].toLower();
        result[index] = lower == QLatin1Char('x') || lower == QLatin1Char('a') || lower == QLatin1Char('g')
            ? lower : QLatin1Char('.');
    }
    return result;
}

QString beatFromBarLane(const QString& source, int sourceBeat, int division)
{
    return normalizedBarLane(source, 4 * division).mid(sourceBeat * division, division);
}

int sourceBeatForMeter(int beatWithinBar, int beatsPerBar)
{
    if (beatsPerBar == 4) return beatWithinBar;
    return qBound(0, beatWithinBar * 4 / qMax(1, beatsPerBar), 3);
}

int beatHitCount(const SongSection& section)
{
    int result = 0;
    for (const BeatPattern& pattern : section.beatPatterns) {
        for (const QString& laneText : pattern.lanes) {
            for (QChar state : laneText) if (state == QLatin1Char('x') || state == QLatin1Char('a') || state == QLatin1Char('g')) ++result;
        }
    }
    return result;
}

void resizeBeatPattern(BeatPattern& pattern, int division)
{
    if (pattern.division == division) return;
    const int previousDivision = qMax(1, pattern.division);
    QVector<QString> resized;
    resized.fill(QString(division, QLatin1Char('.')), BeatGridModel::beatLaneNames().size());
    for (int laneIndex = 0; laneIndex < pattern.lanes.size() && laneIndex < resized.size(); ++laneIndex) {
        const QString previous = normalizedBarLane(pattern.lanes[laneIndex], previousDivision);
        for (int step = 0; step < previousDivision; ++step) {
            if (previous[step] == QLatin1Char('.')) continue;
            const int nextStep = qBound(0,
                static_cast<int>(std::lround(static_cast<double>(step) * division / previousDivision)),
                division - 1);
            resized[laneIndex][nextStep] = previous[step];
        }
    }
    pattern.division = division;
    pattern.lanes = std::move(resized);
}

void clearCymbalsAt(BeatPattern& pattern, int step)
{
    for (const QString& name : {QStringLiteral("Closed HH"), QStringLiteral("Open HH"), QStringLiteral("Ride")}) {
        const int index = BeatGridModel::beatLaneNames().indexOf(name);
        if (index >= 0 && index < pattern.lanes.size() && step >= 0 && step < pattern.lanes[index].size())
            pattern.lanes[index][step] = QLatin1Char('.');
    }
}

bool addWithinBudget(
    SongSection& section,
    BeatPattern& pattern,
    const QString& laneName,
    int step,
    QChar state,
    int maximumHits)
{
    if (beatHitCount(section) >= maximumHits) return false;
    int activeVoices = 0;
    for (const QString& laneText : pattern.lanes) {
        if (step >= 0 && step < laneText.size() && laneText[step] != QLatin1Char('.')) ++activeVoices;
    }
    if (activeVoices >= 3) return false;
    if (laneName == QStringLiteral("Closed HH") || laneName == QStringLiteral("Open HH") ||
        laneName == QStringLiteral("Ride") || laneName == QStringLiteral("Crash")) {
        for (const QString& cymbal : {QStringLiteral("Closed HH"), QStringLiteral("Open HH"),
                QStringLiteral("Ride"), QStringLiteral("Crash")}) {
            const int cymbalLane = BeatGridModel::beatLaneNames().indexOf(cymbal);
            if (cymbalLane >= 0 && cymbalLane < pattern.lanes.size() && step >= 0 &&
                step < pattern.lanes[cymbalLane].size() &&
                pattern.lanes[cymbalLane][step] != QLatin1Char('.')) return false;
        }
    }
    return addHit(pattern, laneName, step, state);
}

bool chance(int percent, Rng& rng)
{
    return std::uniform_int_distribution<int>(1, 100)(rng) <= percent;
}

QString preferredCymbal(const GrooveDef& family, const MoodDef& mood)
{
    if (mood.id == QStringLiteral("nostalgic") || family.id.contains(QStringLiteral("ride")) ||
        family.styleId == QStringLiteral("jazz")) return QStringLiteral("Ride");
    return QStringLiteral("Open HH");
}

void applyAdvancedCell(
    SongSection& section,
    int firstBar,
    int beatWithin,
    int beatsPerBar,
    int cell,
    const GrooveDef& family,
    const MoodDef& mood,
    int maximumHits,
    QString& description)
{
    const QString cymbal = preferredCymbal(family, mood);
    const QStringList names{
        QStringLiteral("repeating triplet cymbal answer"),
        QStringLiteral("repeated sixteenth kick cell"),
        QStringLiteral("three-over-two cymbal accent"),
        QStringLiteral("answering sixteenth hat roll"),
        QStringLiteral("open-cymbal call and response"),
        QStringLiteral("repeated syncopated snare pickup"),
    };
    for (int occurrence = 0; occurrence < 2; ++occurrence) {
        const int bar = firstBar + occurrence;
        const int beat = bar * beatsPerBar + beatWithin;
        if (beat < 0 || beat >= section.beatPatterns.size()) continue;
        BeatPattern& pattern = section.beatPatterns[beat];
        if (cell == 0) {
            resizeBeatPattern(pattern, 3);
            for (int step = 0; step < 3; ++step) clearCymbalsAt(pattern, step);
            addWithinBudget(section, pattern, cymbal, 0, QLatin1Char('x'), maximumHits);
            addWithinBudget(section, pattern, cymbal, 1, QLatin1Char('g'), maximumHits);
            addWithinBudget(section, pattern, cymbal, 2, occurrence == 0 ? QLatin1Char('a') : QLatin1Char('x'), maximumHits);
        } else if (cell == 1) {
            resizeBeatPattern(pattern, 4);
            addWithinBudget(section, pattern, QStringLiteral("Kick"), occurrence == 0 ? 3 : 1, QLatin1Char('x'), maximumHits);
            const int cymbalStep = occurrence == 0 ? 1 : 3;
            clearCymbalsAt(pattern, cymbalStep);
            addWithinBudget(section, pattern, QStringLiteral("Closed HH"), cymbalStep, QLatin1Char('a'), maximumHits);
        } else if (cell == 2) {
            resizeBeatPattern(pattern, 6);
            for (int step = 0; step < 6; ++step) clearCymbalsAt(pattern, step);
            for (int step : {0, 2, 4})
                addWithinBudget(section, pattern, cymbal, step, step == 4 ? QLatin1Char('a') : QLatin1Char('x'), maximumHits);
        } else if (cell == 3) {
            resizeBeatPattern(pattern, 4);
            for (int step = 0; step < 4; ++step) clearCymbalsAt(pattern, step);
            for (int step = 0; step < 4; ++step)
                addWithinBudget(section, pattern, QStringLiteral("Closed HH"), step,
                    step == (occurrence == 0 ? 3 : 2) ? QLatin1Char('a') : step % 2 ? QLatin1Char('g') : QLatin1Char('x'), maximumHits);
        } else if (cell == 4) {
            resizeBeatPattern(pattern, 4);
            for (int step = 0; step < 4; ++step) clearCymbalsAt(pattern, step);
            const QString voice = occurrence == 0 ? QStringLiteral("Closed HH") : QStringLiteral("Open HH");
            addWithinBudget(section, pattern, voice, 0, QLatin1Char('x'), maximumHits);
            addWithinBudget(section, pattern, voice, occurrence == 0 ? 2 : 3, QLatin1Char('a'), maximumHits);
        } else {
            resizeBeatPattern(pattern, 4);
            addWithinBudget(section, pattern, QStringLiteral("Snare"), occurrence == 0 ? 3 : 2, QLatin1Char('g'), maximumHits);
            addWithinBudget(section, pattern, QStringLiteral("Kick"), occurrence == 0 ? 1 : 3, QLatin1Char('x'), maximumHits);
        }
    }
    description = names.value(cell);
}

void generateGroove(
    SongSection& section,
    GenerationRecipe& recipe,
    const StyleDef& style,
    const MoodDef& mood,
    std::uint32_t seed)
{
    Rng rng(grooveSeed(seed, style.id, mood.id));
    const GrooveDef& family = chooseGroove(style, mood, rng);
    recipe.grooveId = family.id;
    recipe.grooveName = family.name;
    recipe.grooveCore = family.core;
    recipe.grooveFeelName = family.feelName;
    recipe.swingPercent = family.swingPercent;
    recipe.snareOffsetMs = qBound(-20, family.snareOffsetMs +
        (mood.id == QStringLiteral("intense") || mood.id == QStringLiteral("bright") ? -2 :
         mood.id == QStringLiteral("dreamy") || mood.id == QStringLiteral("romantic") ||
         mood.id == QStringLiteral("peaceful") ? 3 : 0), 25);
    recipe.timingVariationMs = qBound(0, family.timingVariationMs +
        (mood.id == QStringLiteral("intense") ? -1 : mood.id == QStringLiteral("dreamy") ? 1 : 0), 5);
    recipe.velocityVariationPercent = qBound(0, family.velocityVariationPercent +
        (mood.id == QStringLiteral("intense") ? 2 : mood.id == QStringLiteral("peaceful") ? -2 : 0), 12);
    if (mood.id == QStringLiteral("nostalgic")) recipe.swingPercent = qMin(67, recipe.swingPercent + 2);

    for (int beat = 0; beat < section.beats; ++beat) {
        const int within = beat % recipe.beatsPerBar;
        const int bar = beat / recipe.beatsPerBar;
        const int sourceBeat = sourceBeatForMeter(within, recipe.beatsPerBar);
        const GrooveBarDef& source = bar % 2 == 0 ? family.first : family.second;
        BeatPattern& pattern = section.beatPatterns[beat];
        pattern.division = family.division;
        pattern.lanes.fill(QString(pattern.division, QLatin1Char('.')), BeatGridModel::beatLaneNames().size());
        lane(pattern, QStringLiteral("Kick"), beatFromBarLane(source.kick, sourceBeat, family.division));
        lane(pattern, QStringLiteral("Snare"), beatFromBarLane(source.snare, sourceBeat, family.division));
        lane(pattern, QStringLiteral("Closed HH"), beatFromBarLane(source.closedHat, sourceBeat, family.division));
        lane(pattern, QStringLiteral("Open HH"), beatFromBarLane(source.openHat, sourceBeat, family.division));
        lane(pattern, QStringLiteral("Ride"), beatFromBarLane(source.ride, sourceBeat, family.division));
        lane(pattern, QStringLiteral("Tom"), beatFromBarLane(source.tom, sourceBeat, family.division));
        if (beat == 0) {
            clearCymbalsAt(pattern, 0);
            addHit(pattern, QStringLiteral("Crash"), 0, QLatin1Char('a'));
        }
    }

    const int kickLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Kick"));
    const int snareLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Snare"));
    const int closedLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Closed HH"));
    const int openLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Open HH"));
    const int rideLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Ride"));
    for (int beat = 0; beat < section.beats; ++beat) {
        BeatPattern& pattern = section.beatPatterns[beat];
        if (mood.id == QStringLiteral("bright") && closedLane >= 0 && !pattern.lanes[closedLane].isEmpty()) {
            if (pattern.lanes[closedLane][0] != QLatin1Char('.')) pattern.lanes[closedLane][0] = QLatin1Char('a');
        } else if (mood.id == QStringLiteral("dark") && snareLane >= 0) {
            pattern.lanes[snareLane].replace(QLatin1Char('a'), QLatin1Char('x'));
        } else if (mood.id == QStringLiteral("dreamy") && beat % (recipe.beatsPerBar * 2) == recipe.beatsPerBar * 2 - 1) {
            if (closedLane >= 0 && openLane >= 0 && !pattern.lanes[closedLane].isEmpty()) {
                const int last = pattern.division - 1;
                if (pattern.lanes[closedLane][last] != QLatin1Char('.')) {
                    clearCymbalsAt(pattern, last);
                    pattern.lanes[openLane][last] = QLatin1Char('x');
                }
            }
        } else if (mood.id == QStringLiteral("romantic") && snareLane >= 0) {
            pattern.lanes[snareLane].replace(QLatin1Char('a'), QLatin1Char('x'));
        } else if (mood.id == QStringLiteral("peaceful") && closedLane >= 0) {
            for (int step = 1; step < pattern.lanes[closedLane].size(); ++step)
                pattern.lanes[closedLane][step] = QLatin1Char('.');
        } else if (mood.id == QStringLiteral("intense") && kickLane >= 0 && !pattern.lanes[kickLane].isEmpty()) {
            if (pattern.lanes[kickLane][0] != QLatin1Char('.')) pattern.lanes[kickLane][0] = QLatin1Char('a');
        } else if (mood.id == QStringLiteral("nostalgic") && closedLane >= 0 && rideLane >= 0 &&
            (beat / recipe.beatsPerBar) % 2 == 0) {
            if (!pattern.lanes[closedLane].isEmpty() && pattern.lanes[closedLane][0] != QLatin1Char('.')) {
                pattern.lanes[closedLane][0] = QLatin1Char('.');
                pattern.lanes[rideLane][0] = QLatin1Char('x');
            }
        } else if (mood.id == QStringLiteral("cinematic") && beat % (recipe.beatsPerBar * 4) == 0) {
            clearCymbalsAt(pattern, 0);
            addHit(pattern, QStringLiteral("Crash"), 0, QLatin1Char('a'));
        }
    }
    recipe.moodDecisions << QStringLiteral("%1 groove shaping changes articulation and phrase emphasis while retaining the %2 anchors.")
        .arg(mood.name, style.name);

    static constexpr std::array<int, 8> kickChance{35, 42, 49, 56, 60, 64, 68, 72};
    static constexpr std::array<int, 8> ghostChance{25, 32, 39, 46, 50, 54, 58, 62};
    static constexpr std::array<int, 8> cymbalChance{30, 37, 44, 51, 55, 59, 63, 67};
    static constexpr std::array<int, 8> fillChance{25, 32, 39, 46, 50, 54, 58, 62};
    static constexpr std::array<int, 8> advancedChance{0, 0, 0, 0, 40, 55, 70, 85};
    const int level = recipe.complexity - 1;
    const int baseHits = beatHitCount(section);
    const int maximumHits = qMax(baseHits, static_cast<int>(std::floor(baseHits * 1.35)));
    int kickVariations = 0;
    int ghosts = 0;
    int cymbalChanges = 0;
    int fills = 0;
    for (int firstBar = 0; firstBar < recipe.bars; firstBar += 2) {
        const int phraseFirstBeat = firstBar * recipe.beatsPerBar;
        const int phraseBeatCount = qMin(section.beats - phraseFirstBeat, recipe.beatsPerBar * 2);
        if (phraseBeatCount <= 0) break;
        if (chance(kickChance[level], rng)) {
            const int beat = phraseFirstBeat + std::uniform_int_distribution<int>(0, phraseBeatCount - 1)(rng);
            BeatPattern& pattern = section.beatPatterns[beat];
            const int step = pattern.division > 1
                ? (std::uniform_int_distribution<int>(0, 1)(rng) ? pattern.division - 1 : 1)
                : 0;
            if (addWithinBudget(section, pattern, QStringLiteral("Kick"), step, QLatin1Char('x'), maximumHits)) ++kickVariations;
        }
        if (chance(ghostChance[level], rng)) {
            int backbeat = -1;
            for (int offset = 1; offset < phraseBeatCount; ++offset) {
                const BeatPattern& candidate = section.beatPatterns[phraseFirstBeat + offset];
                if (snareLane >= 0 && candidate.lanes.value(snareLane).contains(QLatin1Char('a'))) {
                    backbeat = phraseFirstBeat + offset;
                    break;
                }
            }
            if (backbeat > phraseFirstBeat) {
                BeatPattern& pickup = section.beatPatterns[backbeat - 1];
                if (addWithinBudget(section, pickup, QStringLiteral("Snare"), pickup.division - 1,
                        QLatin1Char('g'), maximumHits)) ++ghosts;
            }
        }
        if (chance(cymbalChance[level], rng)) {
            const int beat = qMin(section.beats - 1, phraseFirstBeat + phraseBeatCount - 1);
            BeatPattern& pattern = section.beatPatterns[beat];
            const int step = pattern.division - 1;
            const int before = beatHitCount(section);
            clearCymbalsAt(pattern, step);
            if (addWithinBudget(section, pattern, preferredCymbal(family, mood), step,
                    QLatin1Char('x'), maximumHits) || beatHitCount(section) != before) ++cymbalChanges;
        }
    }
    for (int boundaryBar = 4; boundaryBar <= recipe.bars; boundaryBar += 4) {
        if (!chance(fillChance[level], rng)) continue;
        const int beat = boundaryBar * recipe.beatsPerBar - 1;
        if (beat < 0 || beat >= section.beatPatterns.size()) continue;
        BeatPattern& pattern = section.beatPatterns[beat];
        const int last = pattern.division - 1;
        if (style.id == QStringLiteral("jazz")) {
            if (addWithinBudget(section, pattern, QStringLiteral("Snare"), last, QLatin1Char('g'), maximumHits)) ++fills;
            clearCymbalsAt(pattern, last);
            addWithinBudget(section, pattern, QStringLiteral("Ride"), last, QLatin1Char('a'), maximumHits);
        } else if (style.id == QStringLiteral("hiphop-trap")) {
            resizeBeatPattern(pattern, 4);
            if (addWithinBudget(section, pattern, QStringLiteral("Closed HH"), 1, QLatin1Char('g'), maximumHits)) ++fills;
            addWithinBudget(section, pattern, QStringLiteral("Closed HH"), 3, QLatin1Char('a'), maximumHits);
        } else if (style.id == QStringLiteral("edm")) {
            clearCymbalsAt(pattern, last);
            if (addWithinBudget(section, pattern, QStringLiteral("Open HH"), last, QLatin1Char('a'), maximumHits)) ++fills;
            addWithinBudget(section, pattern, QStringLiteral("Snare"), last, QLatin1Char('x'), maximumHits);
        } else {
            clearCymbalsAt(pattern, qMax(0, last - 1));
            clearCymbalsAt(pattern, last);
            if (addWithinBudget(section, pattern, QStringLiteral("Tom"), qMax(0, last - 1), QLatin1Char('x'), maximumHits)) ++fills;
            addWithinBudget(section, pattern, QStringLiteral("Tom"), last, QLatin1Char('a'), maximumHits);
        }
    }

    int advancedCells = 0;
    QSet<int> usedCells;
    const int maximumCellTypes = recipe.complexity >= 7 ? 2 : recipe.complexity >= 5 ? 1 : 0;
    for (int segmentBar = 0; segmentBar < recipe.bars && advancedCells < maximumCellTypes; segmentBar += 8) {
        if (!chance(advancedChance[level], rng)) continue;
        int cell = std::uniform_int_distribution<int>(0, 5)(rng);
        for (int attempts = 0; attempts < 6 && usedCells.contains(cell); ++attempts) cell = (cell + 1) % 6;
        usedCells.insert(cell);
        const int latestFirstBar = qMax(segmentBar, qMin(recipe.bars - 2, segmentBar + 6));
        const int firstBar = qMin(latestFirstBar, segmentBar + (recipe.bars - segmentBar >= 6 ? 4 : 0));
        const int beatWithin = recipe.beatsPerBar > 1
            ? std::uniform_int_distribution<int>(1, recipe.beatsPerBar - 1)(rng) : 0;
        QString description;
        applyAdvancedCell(section, firstBar, beatWithin, recipe.beatsPerBar,
            cell, family, mood, maximumHits, description);
        recipe.grooveDecisions << QStringLiteral("Advanced cell at bars %1-%2: %3; the second occurrence answers the first.")
            .arg(firstBar + 1).arg(firstBar + 2).arg(description);
        ++advancedCells;
    }

    recipe.grooveDecisions.prepend(QStringLiteral(
        "Complexity %1 uses the shared basic-technique band: kick %2%, ghosts %3%, cymbal changes %4%, fills %5%.")
        .arg(recipe.complexity).arg(kickChance[level]).arg(ghostChance[level])
        .arg(cymbalChance[level]).arg(fillChance[level]));
    recipe.grooveDecisions.prepend(QStringLiteral("Selected %1 from five %2 groove families; its two authored bars form the core A/A′ pocket.")
        .arg(family.name, style.name));
    recipe.kickVariationCount = kickVariations;
    recipe.ghostVariationCount = ghosts;
    recipe.cymbalVariationCount = cymbalChanges;
    recipe.fillCount = fills;
    recipe.advancedCellCount = advancedCells;
    recipe.grooveDecisions << QStringLiteral(
        "Seeded phrase changes used %1 kick variation(s), %2 ghost pickup(s), %3 cymbal change(s), and %4 fill boundary event(s).")
        .arg(kickVariations).arg(ghosts).arg(cymbalChanges).arg(fills);
    if (recipe.complexity < 5) {
        recipe.grooveDecisions << QStringLiteral("Complexity levels 1-4 share fills, ghosts, kick alternatives, and cymbal articulation; only their probabilities rise.");
    } else {
        recipe.grooveDecisions << QStringLiteral("The advanced band selected %1 repeating rhythmic cell type(s), never an isolated subdivision change.")
            .arg(advancedCells);
    }
    recipe.grooveDecisions << QStringLiteral("Final hit count %1 stays within the 35% density ceiling over the %2-hit core.")
        .arg(beatHitCount(section)).arg(baseHits);
    if (mood.id == QStringLiteral("intense")) recipe.grooveDecisions << QStringLiteral("Intense mood strengthens downbeat and phrase accents.");
    else if (mood.id == QStringLiteral("peaceful")) recipe.grooveDecisions << QStringLiteral("Peaceful mood keeps fills soft and leaves the core groove spacious.");
    else if (mood.id == QStringLiteral("cinematic")) recipe.grooveDecisions << QStringLiteral("Cinematic mood emphasizes section entrances and the contrasting B phrase.");
}

struct MelodyCandidate {
    QVector<QVector<MusicalStep>> steps;
    QVector<MelodyRecipeEvent> events;
    QVector<MelodyPhraseRecipe> phrases;
    QString cell;
    QString rhythm;
    QString form;
    QStringList transformations;
    double score = -std::numeric_limits<double>::infinity();
};

QString formForBars(int bars, int variant)
{
    if (bars <= 4) return QStringLiteral("A");
    if (bars <= 8) return variant % 2 ? QStringLiteral("A-B") : QStringLiteral("A-A'");
    if (bars <= 12) return variant % 2 ? QStringLiteral("A-B-A'") : QStringLiteral("A-A'-B");
    switch (variant % 4) {
    case 1: return QStringLiteral("A-B-A'-A''");
    case 2: return QStringLiteral("A-A'-B-B'");
    case 3: return QStringLiteral("A-B-C-A'");
    default: return QStringLiteral("A-A'-B-A''");
    }
}

QVector<int> homePitchClasses(int key, const ModeDef& mode)
{
    QVector<int> result;
    for (int interval : mode.intervals) {
        const int value = pitchClass(key + interval);
        if (!result.contains(value)) result.push_back(value);
    }
    return result;
}

int definingInterval(const TheoryDecision* decision, const ParsedChord& chord, Rng& rng)
{
    if (!decision || !chord.valid || chord.intervals.isEmpty()) return -1;
    QVector<int> preferred;
    if (decision->kind == QStringLiteral("secondary-dominant") ||
        decision->kind == QStringLiteral("tritone-substitution") ||
        decision->kind == QStringLiteral("backdoor-dominant")) preferred = {4, 10};
    else if (decision->kind == QStringLiteral("passing-diminished")) preferred = {3, 6, 9};
    else if (decision->kind == QStringLiteral("modal-interchange")) preferred = {3, 8, 10};
    else if (decision->kind == QStringLiteral("temporary-modulation")) preferred = {4, 11, 0};
    for (int interval : preferred) {
        if (includesPitchClass(chord.intervals, interval)) return interval;
    }
    return chord.intervals.at(std::uniform_int_distribution<int>(0, chord.intervals.size() - 1)(rng));
}

int chooseMelodyMidi(
    const QVector<int>& allowed,
    int previous,
    int desired,
    int repeatCount,
    Rng& rng)
{
    QVector<int> choices;
    for (int midi = 57; midi <= 76; ++midi) {
        if (!allowed.contains(pitchClass(midi))) continue;
        if (previous >= 0 && std::abs(midi - previous) > 7) continue;
        if (repeatCount >= 2 && midi == previous) continue;
        choices.push_back(midi);
    }
    if (choices.isEmpty()) {
        for (int midi = 52; midi <= 81; ++midi) {
            if (allowed.contains(pitchClass(midi)) &&
                (previous < 0 || std::abs(midi - previous) <= 7)) choices.push_back(midi);
        }
    }
    if (choices.isEmpty()) return std::clamp(previous < 0 ? 64 : previous, 52, 81);
    int best = choices.front();
    double bestScore = -1.0e9;
    for (int midi : choices) {
        double score = -1.2 * std::abs(midi - desired) - 0.08 * std::abs(midi - 66) +
            std::uniform_real_distribution<double>(-1.2, 1.2)(rng);
        if (midi >= 57 && midi <= 76) score += 2.0;
        if (previous >= 0 && std::abs(midi - previous) <= 2) score += 1.1;
        if (score > bestScore) { bestScore = score; best = midi; }
    }
    return best;
}

MelodyCandidate planMelodyCandidate(
    const SongSection& chordSection,
    const SongSection& beatSection,
    const GenerationRecipe& recipe,
    int key,
    const ModeDef& mode,
    bool flats,
    int candidateIndex)
{
    Rng rng(recipe.seed ^ (0x9e3779b9U * static_cast<std::uint32_t>(candidateIndex + 1)));
    MelodyCandidate result;
    result.score = 0.0;
    result.steps.resize(chordSection.beats);
    const QVector<int> home = homePitchClasses(key, mode);
    const int cellLength = std::uniform_int_distribution<int>(3, 5)(rng);
    QVector<int> contour;
    QStringList contourText;
    for (int index = 0; index < cellLength; ++index) {
        int movement = index == 0 ? 0 : std::uniform_int_distribution<int>(-2, 2)(rng);
        if (index > 0 && movement == 0 && contour.back() == 0) movement = index % 2 ? 1 : -1;
        contour.push_back(movement);
        contourText << (movement > 0 ? QStringLiteral("+%1").arg(movement) : QString::number(movement));
    }
    result.cell = contourText.join(QLatin1Char(' '));
    result.form = formForBars(recipe.bars, candidateIndex);
    result.transformations << QStringLiteral(
        "The unique contour is reharmonised against each chord rather than copied as fixed pitches.")
        << QStringLiteral(
        "Four-bar returns vary rotation, register, rhythm, or cadence according to %1.").arg(result.form);

    int previous = 64 + std::uniform_int_distribution<int>(-2, 2)(rng);
    int repeatCount = 0;
    int onsetOrdinal = 0;
    bool sounding = false;
    int holdUntilTick = -1;
    int low = 127;
    int high = 0;
    int strongChordTones = 0;
    int strongNotes = 0;
    int grooveAligned = 0;
    int resolvedApproaches = 0;
    bool previousApproach = false;
    QString rhythmText;

    for (int beat = 0; beat < chordSection.beats; ++beat) {
        const MusicalBeatPattern& source = chordSection.musicalPatterns[beat];
        result.steps[beat].fill(MusicalStep{}, source.division);
        const QString symbol = activeChordAtBeat(chordSection, beat);
        const ParsedChord chord = parseChord(symbol);
        const QVector<int> chordTones = chordPitchClasses(chord);
        const bool chordChange = !chordSection.chords.value(beat).trimmed().isEmpty();
        const TheoryDecision* decision = theoryAtBeat(recipe, beat);
        for (int step = 0; step < source.division; ++step) {
            MusicalStep& output = result.steps[beat][step];
            const int tick = beat * 12 + step * 12 / source.division;
            const bool strong = step == 0;
            double chance = strong ? 0.64 : 0.24;
            if (recipe.styleId == QStringLiteral("funk") ||
                recipe.styleId == QStringLiteral("anime-jpop")) chance += 0.12;
            if (recipe.styleId == QStringLiteral("modal-vamp") ||
                recipe.moodId == QStringLiteral("peaceful")) chance -= 0.16;
            if (recipe.moodId == QStringLiteral("intense")) chance += 0.10;
            const bool onGroove = grooveAccent(beatSection, beat, step, source.division);
            if (onGroove) chance += 0.10;
            const bool phraseStart = beat % (recipe.beatsPerBar * 4) == 0 && step == 0;
            const bool finalArrival = beat == chordSection.beats - 1 && step == 0;
            bool onset = phraseStart || finalArrival || (chordChange && strong) ||
                std::uniform_real_distribution<double>(0.0, 1.0)(rng) < chance;
            if (onsetOrdinal > 0 && onsetOrdinal % (cellLength + 2) == cellLength + 1 && !chordChange)
                onset = false;
            if (!onset || chordTones.isEmpty()) {
                output.state = sounding && tick < holdUntilTick
                    ? MusicalStepState::Hold : MusicalStepState::Rest;
                if (output.state == MusicalStepState::Rest) sounding = false;
                rhythmText += output.state == MusicalStepState::Hold ? QLatin1Char('H') : QLatin1Char('R');
                continue;
            }

            int tier = std::uniform_int_distribution<int>(1, recipe.complexity)(rng);
            QVector<int> allowed = chordTones;
            QString melodicRole = chordChange ? QStringLiteral("Chord-change target")
                                               : QStringLiteral("Chord-tone motif note");
            const int defined = decision && step == 0 ? definingInterval(decision, chord, rng) : -1;
            bool approach = false;
            if (defined >= 0) {
                allowed = {pitchClass(chord.root + defined)};
                melodicRole = QStringLiteral("Defines %1 before resolving to %2")
                    .arg(decision->analysis, decision->resolutionTarget);
            } else if (!strong && tier <= 2) {
                allowed = home;
                melodicRole = tier == 1 ? QStringLiteral("Diatonic passing / neighbour tone")
                                         : QStringLiteral("Pentatonic or diatonic approach");
            } else if (!strong && tier == 3) {
                allowed = home;
                for (int value : chordTones) if (!allowed.contains(value)) allowed.push_back(value);
                melodicRole = QStringLiteral("Modal or borrowed chord colour");
            } else if (!strong && tier >= 4 &&
                       std::uniform_int_distribution<int>(0, 3)(rng) == 0) {
                const QVector<int> nextTones = chordPitchClasses(parseChord(
                    activeChordAtBeat(chordSection, qMin(chordSection.beats - 1, beat + 1))));
                if (!nextTones.isEmpty()) {
                    const int target = choose(nextTones, rng);
                    allowed = {pitchClass(target +
                        (std::uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1))};
                    approach = true;
                    melodicRole = tier >= 7 ? QStringLiteral("Outside semitone approach")
                        : tier >= 5 ? QStringLiteral("Chromatic enclosure / passing approach")
                                    : QStringLiteral("Chromatic approach to the next guide tone");
                }
            } else if (tier >= 6 && chord.intervals.size() > 3) {
                allowed.clear();
                for (int interval : chord.intervals) {
                    if (interval > 7) allowed.push_back(pitchClass(chord.root + interval));
                }
                if (allowed.isEmpty()) allowed = chordTones;
                melodicRole = tier >= 8 ? QStringLiteral("Local chord-scale colour")
                                         : QStringLiteral("Upper chord extension");
            }

            const int phrase = beat / (recipe.beatsPerBar * 4);
            const int rotation = phrase == 1 ? 1 : phrase == 2 ? 2 : 0;
            int movement = contour.at((onsetOrdinal + rotation) % contour.size());
            if (phrase == 2 && result.form.contains(QLatin1Char('B'))) movement = -movement;
            const double progress = static_cast<double>(beat % (recipe.beatsPerBar * 4)) /
                qMax(1, recipe.beatsPerBar * 4 - 1);
            const int arc = static_cast<int>(std::lround(3.0 * std::sin(progress * 3.14159265358979323846)));
            const int desired = previous + movement + arc;
            int midi = chooseMelodyMidi(allowed, previous, desired, repeatCount, rng);
            if (finalArrival && repeatCount < 2)
                midi = chooseMelodyMidi({pitchClass(key)}, previous, key + 60, repeatCount, rng);
            if (repeatCount >= 2 && midi == previous) {
                midi = chooseMelodyMidi(chordTones, previous, desired, repeatCount, rng);
                melodicRole = QStringLiteral("Chord-tone variation prevents a static repeated pitch");
            }
            repeatCount = midi == previous ? repeatCount + 1 : 1;
            if (std::abs(midi - previous) > 7) result.score -= 30.0;
            previous = midi;
            low = qMin(low, midi);
            high = qMax(high, midi);
            const int velocity = qBound(58,
                76 + (strong ? 12 : 0) + (onGroove ? 7 : 0) +
                    std::uniform_int_distribution<int>(-5, 5)(rng), 112);
            output = {MusicalStepState::Onset,
                noteName(midi % 12, flats) + QString::number(midi / 12 - 1), velocity};
            const int stepTicks = 12 / source.division;
            const bool legato = recipe.moodId == QStringLiteral("romantic") ||
                recipe.moodId == QStringLiteral("dreamy") || tier == 2;
            const int duration = qMax(1, stepTicks * (legato ? 2 : 1));
            holdUntilTick = tick + duration;
            sounding = true;
            result.events.push_back({tick, duration, midi, velocity, output.value, symbol,
                chordRole(chord, midi), melodicRole});
            rhythmText += QLatin1Char('O');
            if (strong) {
                ++strongNotes;
                if (chordTones.contains(pitchClass(midi))) ++strongChordTones;
            }
            if (onGroove) ++grooveAligned;
            if (previousApproach && chordTones.contains(pitchClass(midi))) ++resolvedApproaches;
            previousApproach = approach;
            ++onsetOrdinal;
        }
    }
    for (int index = 0; index + 1 < result.events.size(); ++index) {
        result.events[index].durationTicks = qMax(1, qMin(
            result.events[index].durationTicks,
            result.events[index + 1].tick - result.events[index].tick));
    }
    result.rhythm = rhythmText.left(24) +
        (rhythmText.size() > 24 ? QStringLiteral("...") : QString());
    result.score += strongChordTones * 5.0 - (strongNotes - strongChordTones) * 12.0;
    result.score += grooveAligned * 0.7 + resolvedApproaches * 1.5;
    result.score -= std::abs(result.events.size() - chordSection.beats * 3 / 4) * 0.22;
    if (low >= 57 && high <= 76) result.score += 12.0;
    if (!result.events.isEmpty() && pitchClass(result.events.back().midi) == pitchClass(key))
        result.score += 8.0;
    const int phraseCount = (recipe.bars + 3) / 4;
    for (int phrase = 0; phrase < phraseCount; ++phrase) {
        MelodyPhraseRecipe summary;
        summary.startBar = phrase * 4 + 1;
        summary.endBar = qMin(recipe.bars, summary.startBar + 3);
        summary.label = result.form.split(QLatin1Char('-')).value(
            phrase, QStringLiteral("Phrase %1").arg(phrase + 1));
        summary.summary = phrase == 0
            ? QStringLiteral("Introduces a chord-aware contour with space between gestures.")
            : phrase == phraseCount - 1
                ? QStringLiteral("Recalls earlier contour material and redirects it into a tonic cadence.")
                : QStringLiteral("Varies the contour through rhythmic displacement, rotation, or register.");
        result.phrases.push_back(std::move(summary));
    }
    return result;
}

void generateMelody(
    SongSection& chordSection,
    const SongSection& beatSection,
    GenerationRecipe& recipe,
    int key,
    const ModeDef& mode,
    bool flats)
{
    MelodyCandidate best;
    for (int candidate = 0; candidate < 16; ++candidate) {
        MelodyCandidate planned = planMelodyCandidate(
            chordSection, beatSection, recipe, key, mode, flats, candidate);
        if (planned.score > best.score) best = std::move(planned);
    }
    if (best.steps.size() != chordSection.beats || best.events.isEmpty()) {
        // A valid harmonic plan always produces candidates, but retain a safe,
        // inspectable rest pattern if future style rules make one impossible.
        best.steps.resize(chordSection.beats);
        for (int beat = 0; beat < chordSection.beats; ++beat) {
            best.steps[beat].fill(MusicalStep{}, chordSection.musicalPatterns[beat].division);
        }
        best.form = QStringLiteral("Rest");
        best.cell = QStringLiteral("No eligible contour");
        best.rhythm = QStringLiteral("R");
    }
    chordSection.targets.fill(QString(), chordSection.beats);
    for (int beat = 0; beat < chordSection.beats; ++beat) {
        chordSection.musicalPatterns[beat].melody = best.steps[beat];
        const MusicalStep& first = best.steps[beat].front();
        chordSection.targets[beat] = first.state == MusicalStepState::Onset ? first.value
            : first.state == MusicalStepState::Rest ? QStringLiteral("-") : QString();
    }
    recipe.motifCell = best.cell;
    recipe.motifRhythm = best.rhythm + QStringLiteral(" (O=onset, H=hold, R=rest)");
    recipe.motifForm = best.form;
    recipe.motifTransformations = best.transformations;
    recipe.motifTransformations << QStringLiteral(
        "Sixteen deterministic candidates were scored for chord fit, singable range, contour, repetition, cadence, and groove alignment.");
    recipe.melodyEvents = best.events;
    recipe.melodyPhrases = best.phrases;
    int low = 127;
    int high = 0;
    for (const MelodyRecipeEvent& event : best.events) {
        low = qMin(low, event.midi);
        high = qMax(high, event.midi);
    }
    recipe.melodyRange = best.events.isEmpty() ? QStringLiteral("No melody")
        : QStringLiteral("%1 to %2 (core tessitura A3-E5)")
            .arg(noteName(low % 12, flats) + QString::number(low / 12 - 1),
                 noteName(high % 12, flats) + QString::number(high / 12 - 1));
    recipe.moodDecisions << QStringLiteral(
        "%1 shapes melodic density, articulation, and phrase arc without replacing chord-aware note selection.")
        .arg(recipe.moodName);
}

QStringList moodPatchModifiers(const MoodDef& mood)
{
    if (mood.id == QStringLiteral("bright")) return {QStringLiteral("Bright: faster attack, higher cutoff, and a little more upper harmonic energy.")};
    if (mood.id == QStringLiteral("dark")) return {QStringLiteral("Dark: lower cutoff, softer attack, and stronger low-mid weight.")};
    if (mood.id == QStringLiteral("dreamy")) return {QStringLiteral("Dreamy: slower attack with wider chorus and a short audible delay tail.")};
    if (mood.id == QStringLiteral("romantic")) return {QStringLiteral("Romantic: warm filtering, gentle saturation, and a singing lead envelope.")};
    if (mood.id == QStringLiteral("peaceful")) return {QStringLiteral("Peaceful: soft transients, reduced brightness, and longer releases.")};
    if (mood.id == QStringLiteral("intense")) return {QStringLiteral("Intense: harder transients, more saturation, and stronger drum accents.")};
    if (mood.id == QStringLiteral("nostalgic")) return {QStringLiteral("Nostalgic: mild detune, darker top end, and restrained modulation.")};
    return {QStringLiteral("Cinematic: wider layers, a longer envelope, and stronger section-entry accents.")};
}

GeneratedPracticeIdea coupledIdea(ChordIdeaRequest request, std::uint32_t seed)
{
    Rng rng(seed);
    const StyleDef& style = resolvedStyle(request.styleId, rng);
    const MoodDef& mood = resolvedMood(request.characterId, rng);
    const ProgressionDef& progressionDef = choose(style.progressions, rng);
    const int key = request.key >= 0 && request.key < 12 ? request.key : std::uniform_int_distribution<int>(0, 11)(rng);
    const bool flats = preferFlats(key);
    const int bars = request.bars == 4 || request.bars == 8 || request.bars == 12 || request.bars == 16
        ? request.bars : choose(QVector<int>{4, 8, 12, 16}, rng);
    const int beatsPerBar = std::clamp(request.beatsPerBar, 1, 12);
    const int complexity = std::clamp(qMax(request.harmonicComplexity, request.rhythmicComplexity), 1, 8);
    const ModeDef mode = resolvedMode(style, mood, progressionDef, rng);

    GenerationRecipe recipe;
    recipe.seed = seed;
    recipe.styleId = style.id;
    recipe.styleName = style.name;
    recipe.moodId = mood.id;
    recipe.moodName = mood.name;
    recipe.tonic = noteName(key, flats);
    recipe.mode = mode.name;
    recipe.beatsPerBar = beatsPerBar;
    recipe.bars = bars;
    recipe.complexity = complexity;
    recipe.progressionId = progressionDef.id;
    recipe.progressionName = progressionDef.name;
    recipe.chordPatchId = style.chordPatch;
    recipe.chordPatchName = style.chordPatchName;
    recipe.melodyPatchId = style.melodyPatch;
    recipe.melodyPatchName = style.melodyPatchName;
    recipe.drumPatchId = style.drumPatch;
    recipe.drumPatchName = style.drumPatchName;
    recipe.patchModifiers = moodPatchModifiers(mood);

    QVector<PlannedEvent> events = basePlan(progressionDef, bars, beatsPerBar, key, mode, flats);
    for (const PlannedEvent& event : events)
        recipe.baseHarmony.push_back({event.beat, event.duration, event.roman, event.chord});
    applyMoodHarmony(events, recipe, mood, key, mode, flats);
    addTheory(events, recipe, key, mode, flats, rng);

    SongSection chord;
    chord.label = QStringLiteral("Generated");
    chord.name = QStringLiteral("%1 %2 — %3 — %4").arg(recipe.tonic, recipe.mode, style.name, mood.name);
    chord.beats = bars * beatsPerBar;
    chord.generatedKind = QStringLiteral("chord");
    chord.chords.resize(chord.beats);
    chord.targets.resize(chord.beats);
    for (const PlannedEvent& event : events) {
        if (event.beat >= 0 && event.beat < chord.beats) chord.chords[event.beat] = event.chord;
        recipe.finalChordPlan << QStringLiteral("%1:%2 %3")
            .arg(event.beat / beatsPerBar + 1).arg(event.beat % beatsPerBar + 1).arg(event.chord);
    }
    SongSection beat;
    beat.label = QStringLiteral("Generated");
    beat.name = chord.name;
    beat.beats = chord.beats;
    beat.generatedKind = QStringLiteral("beat");
    beat.beatNotes.resize(beat.beats);
    beat.beatPatterns.resize(beat.beats);
    generateGroove(beat, recipe, style, mood, seed);
    generateChordRhythm(chord, recipe, style, mood, rng);
    generateMelody(chord, beat, recipe, key, mode, flats);

    const int low = qMax(40, style.minimumBpm + mood.tempoOffset);
    const int high = qMax(low, style.maximumBpm + mood.tempoOffset);
    recipe.bpm = std::uniform_int_distribution<int>((low + 1) / 2, high / 2)(rng) * 2;
    recipe.moodDecisions << QStringLiteral("%1 tempo bias selected %2 BPM within the %3 style range.")
        .arg(mood.name).arg(recipe.bpm).arg(style.name);
    recipe.moodDecisions << QStringLiteral("%1 sound shaping changes envelope, filtering, saturation, modulation, and delay parameters for the selected patches.")
        .arg(mood.name);
    recipe.chordFingerprint = contentFingerprint(chord, false);
    recipe.beatFingerprint = contentFingerprint(beat, true);
    chord.generatedRecipe = recipe;
    beat.generatedRecipe = recipe;

    GeneratedPracticeIdea result;
    result.chordSection = std::move(chord);
    result.beatSection = std::move(beat);
    result.recipe = recipe;
    result.bpm = recipe.bpm;
    result.clickDivision = 1;
    const int clickSteps = beatsPerBar * result.clickDivision;
    result.clickEnabled.fill(true, clickSteps);
    result.clickAccents.fill(false, clickSteps);
    if (!result.clickAccents.isEmpty()) result.clickAccents[0] = true;
    return result;
}

} // namespace

QStringList chordStyleNames()
{
    QStringList result;
    for (const StyleDef& style : styles()) result << style.name;
    return result;
}

QStringList beatStyleNames()
{
    return chordStyleNames();
}

QStringList styleIds()
{
    QStringList result;
    for (const StyleDef& style : styles()) result << style.id;
    return result;
}

QStringList grooveFamilyIds(const QString& styleId)
{
    QStringList result;
    for (const GrooveDef& family : grooveFamilies()) {
        if (family.styleId == styleId) result << family.id;
    }
    return result;
}

QStringList grooveFamilyNames(const QString& styleId)
{
    QStringList result;
    for (const GrooveDef& family : grooveFamilies()) {
        if (family.styleId == styleId) result << family.name;
    }
    return result;
}

QStringList characterNames()
{
    QStringList result;
    for (const MoodDef& mood : moods()) result << mood.name;
    return result;
}

QStringList characterIds()
{
    QStringList result;
    for (const MoodDef& mood : moods()) result << mood.id;
    return result;
}

QString styleNameForId(const QString& id)
{
    for (const StyleDef& style : styles()) if (style.id == id) return style.name;
    return {};
}

QStringList keyNames()
{
    return {QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("D"), QStringLiteral("Eb"),
        QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("G"),
        QStringLiteral("Ab"), QStringLiteral("A"), QStringLiteral("Bb"), QStringLiteral("B")};
}

QString generatedChordFingerprint(const SongSection& section)
{
    return contentFingerprint(section, false);
}

QString generatedBeatFingerprint(const SongSection& section)
{
    return contentFingerprint(section, true);
}

GeneratedPracticeIdea generateCoupledPracticeIdea(const ChordIdeaRequest& request)
{
    std::random_device device;
    return coupledIdea(request, device());
}

SongSection generateChordIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed)
{
    return coupledIdea(request, seed).chordSection;
}

SongSection generateBeatIdeaForTest(const BeatIdeaRequest& request, std::uint32_t seed)
{
    ChordIdeaRequest coupled;
    coupled.styleId = request.styleId;
    coupled.characterId = request.characterId;
    coupled.bars = request.bars;
    coupled.beatsPerBar = request.beatsPerBar;
    coupled.harmonicComplexity = request.rhythmicComplexity;
    coupled.rhythmicComplexity = request.rhythmicComplexity;
    return coupledIdea(coupled, seed).beatSection;
}

GeneratedPracticeIdea generateCoupledPracticeIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed)
{
    return coupledIdea(request, seed);
}

} // namespace jam2::practice
