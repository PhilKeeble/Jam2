#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace jam2::practice {

struct ParsedChord {
    int root = 0;
    int bass = -1;
    QString suffix;
    QVector<int> intervals;
    bool rest = false;
    bool valid = false;
};

ParsedChord parseChord(const QString& symbol);
QString noteName(int pitchClass, bool preferFlats = false);
QString chordSymbol(int root, const QString& suffix, bool preferFlats = false);
QString chordToneNames(const QString& symbol);
std::optional<int> parseMidiNote(const QString& note);
double midiFrequency(int midiNote);

} // namespace jam2::practice
