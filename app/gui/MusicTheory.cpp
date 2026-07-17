#include "MusicTheory.hpp"

#include <QHash>

#include <cmath>

namespace jam2::practice {
namespace {

int pitchClass(const QString& root)
{
    static const QHash<QString, int> values{
        {QStringLiteral("C"), 0}, {QStringLiteral("B#"), 0},
        {QStringLiteral("C#"), 1}, {QStringLiteral("Db"), 1},
        {QStringLiteral("D"), 2}, {QStringLiteral("D#"), 3},
        {QStringLiteral("Eb"), 3}, {QStringLiteral("E"), 4},
        {QStringLiteral("Fb"), 4}, {QStringLiteral("E#"), 5},
        {QStringLiteral("F"), 5}, {QStringLiteral("F#"), 6},
        {QStringLiteral("Gb"), 6}, {QStringLiteral("G"), 7},
        {QStringLiteral("G#"), 8}, {QStringLiteral("Ab"), 8},
        {QStringLiteral("A"), 9}, {QStringLiteral("A#"), 10},
        {QStringLiteral("Bb"), 10}, {QStringLiteral("B"), 11},
        {QStringLiteral("Cb"), 11},
    };
    return values.value(root, -1);
}

QVector<int> qualityIntervals(QString suffix)
{
    suffix = suffix.trimmed().toLower();
    if (suffix.isEmpty() || suffix == QStringLiteral("maj")) return {0, 4, 7};
    if (suffix == QStringLiteral("m") || suffix == QStringLiteral("min")) return {0, 3, 7};
    if (suffix == QStringLiteral("5") || suffix == QStringLiteral("power")) return {0, 7};
    if (suffix == QStringLiteral("sus2")) return {0, 2, 7};
    if (suffix == QStringLiteral("sus4") || suffix == QStringLiteral("sus")) return {0, 5, 7};
    if (suffix == QStringLiteral("dim")) return {0, 3, 6};
    if (suffix == QStringLiteral("aug") || suffix == QStringLiteral("+")) return {0, 4, 8};
    if (suffix == QStringLiteral("6")) return {0, 4, 7, 9};
    if (suffix == QStringLiteral("m6")) return {0, 3, 7, 9};
    if (suffix == QStringLiteral("7") || suffix == QStringLiteral("dom7")) return {0, 4, 7, 10};
    if (suffix == QStringLiteral("maj7") || suffix == QStringLiteral("ma7")) return {0, 4, 7, 11};
    if (suffix == QStringLiteral("m7") || suffix == QStringLiteral("min7")) return {0, 3, 7, 10};
    if (suffix == QStringLiteral("m7b5")) return {0, 3, 6, 10};
    if (suffix == QStringLiteral("dim7")) return {0, 3, 6, 9};
    if (suffix == QStringLiteral("add9")) return {0, 4, 7, 14};
    if (suffix == QStringLiteral("madd9")) return {0, 3, 7, 14};
    return {};
}

} // namespace

ParsedChord parseChord(const QString& symbol)
{
    const QString text = symbol.trimmed();
    if (text == QStringLiteral("-")) {
        ParsedChord chord;
        chord.rest = true;
        chord.valid = true;
        return chord;
    }
    if (text.isEmpty() || text.size() > 16 || text.at(0) < QLatin1Char('A') || text.at(0) > QLatin1Char('G')) {
        return {};
    }
    int rootLength = 1;
    if (text.size() > 1 && (text.at(1) == QLatin1Char('#') || text.at(1) == QLatin1Char('b'))) {
        rootLength = 2;
    }
    ParsedChord chord;
    chord.root = pitchClass(text.left(rootLength));
    chord.suffix = text.mid(rootLength);
    chord.intervals = qualityIntervals(chord.suffix);
    chord.valid = chord.root >= 0 && !chord.intervals.isEmpty();
    return chord;
}

QString noteName(int pitchClassValue, bool preferFlats)
{
    static const QStringList sharps{
        QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("D"), QStringLiteral("D#"),
        QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("G"),
        QStringLiteral("G#"), QStringLiteral("A"), QStringLiteral("A#"), QStringLiteral("B")};
    static const QStringList flats{
        QStringLiteral("C"), QStringLiteral("Db"), QStringLiteral("D"), QStringLiteral("Eb"),
        QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("Gb"), QStringLiteral("G"),
        QStringLiteral("Ab"), QStringLiteral("A"), QStringLiteral("Bb"), QStringLiteral("B")};
    const int normalized = ((pitchClassValue % 12) + 12) % 12;
    return (preferFlats ? flats : sharps).at(normalized);
}

QString chordSymbol(int root, const QString& suffix, bool preferFlats)
{
    return noteName(root, preferFlats) + suffix;
}

QString chordToneNames(const QString& symbol)
{
    const ParsedChord chord = parseChord(symbol);
    if (!chord.valid || chord.rest) {
        return {};
    }
    QStringList names;
    for (int interval : chord.intervals) {
        names.push_back(noteName(chord.root + interval, symbol.contains(QLatin1Char('b'))));
    }
    return names.join(QStringLiteral(" "));
}

std::optional<int> parseMidiNote(const QString& note)
{
    const QString text = note.trimmed();
    if (text.size() < 2 || text.size() > 5 ||
        text.at(0) < QLatin1Char('A') || text.at(0) > QLatin1Char('G')) {
        return std::nullopt;
    }
    int rootLength = 1;
    if (text.size() > 1 &&
        (text.at(1) == QLatin1Char('#') || text.at(1) == QLatin1Char('b'))) {
        rootLength = 2;
    }
    const int root = pitchClass(text.left(rootLength));
    bool octaveOk = false;
    const int octave = text.mid(rootLength).toInt(&octaveOk);
    if (root < 0 || !octaveOk) return std::nullopt;
    const int midi = (octave + 1) * 12 + root;
    if (midi < 0 || midi > 127) return std::nullopt;
    return midi;
}

double midiFrequency(int midiNote)
{
    return 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
}

} // namespace jam2::practice
