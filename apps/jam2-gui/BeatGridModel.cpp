#include "BeatGridModel.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {

constexpr int kDefaultBeatDivision = 4;

}

BeatGridModel::BeatGridModel()
{
    reset();
}

QString BeatGridModel::title() const
{
    return title_;
}

void BeatGridModel::setTitle(const QString& title)
{
    title_ = title.trimmed().isEmpty() ? QStringLiteral("Untitled Jam") : title.trimmed();
    ++revision_;
}

int BeatGridModel::revision() const
{
    return revision_;
}

const QVector<SongSection>& BeatGridModel::sections() const
{
    return sections_;
}

SongSection& BeatGridModel::section(int index)
{
    return sections_[index];
}

const SongSection& BeatGridModel::section(int index) const
{
    return sections_[index];
}

void BeatGridModel::setCell(int sectionIndex, const QString& lane, int beat, const QString& text)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size() || beat < 0) {
        return;
    }
    SongSection& section = sections_[sectionIndex];
    normalize(section);
    if (beat >= section.beats) {
        return;
    }
    if (lane == QStringLiteral("chord")) {
        section.chords[beat] = text;
    } else if (lane == QStringLiteral("beat")) {
        section.beatNotes[beat] = text;
    } else if (lane == QStringLiteral("lyric")) {
        section.lyrics[beat] = text;
    } else {
        return;
    }
    ++revision_;
}

void BeatGridModel::setBeatDivision(int sectionIndex, int beat, int division)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size() || beat < 0) {
        return;
    }
    SongSection& section = sections_[sectionIndex];
    normalize(section);
    if (beat >= section.beats) {
        return;
    }
    section.beatPatterns[beat].division = normalizedDivision(division);
    ++revision_;
}

void BeatGridModel::setBeatHit(int sectionIndex, int beat, int lane, const QString& text)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size() || beat < 0 || lane < 0 || lane >= beatLaneNames().size()) {
        return;
    }
    SongSection& section = sections_[sectionIndex];
    normalize(section);
    if (beat >= section.beats) {
        return;
    }
    section.beatPatterns[beat].lanes[lane] = text;
    ++revision_;
}

QString BeatGridModel::lyricsText() const
{
    return lyricsText_;
}

void BeatGridModel::setLyricsText(const QString& text)
{
    lyricsText_ = text;
    ++revision_;
}

void BeatGridModel::resizeSection(int sectionIndex, int beats)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size()) {
        return;
    }
    SongSection& section = sections_[sectionIndex];
    section.beats = qMax(4, beats);
    normalize(section);
    ++revision_;
}

void BeatGridModel::resizeAllSections(int beats)
{
    const int clampedBeats = qMax(4, beats);
    for (SongSection& section : sections_) {
        section.beats = clampedBeats;
        normalize(section);
    }
    ++revision_;
}

void BeatGridModel::addSection(int beats)
{
    SongSection section;
    section.label = QString(QChar(static_cast<ushort>('A' + (sections_.size() % 26))));
    section.name = QStringLiteral("Section");
    section.beats = beats > 0 ? beats : (sections_.isEmpty() ? 8 : sections_.back().beats);
    normalize(section);
    sections_.push_back(section);
    ++revision_;
}

void BeatGridModel::duplicateSection(int index)
{
    if (index < 0 || index >= sections_.size()) {
        return;
    }
    SongSection copy = sections_[index];
    copy.label += QStringLiteral(" copy");
    sections_.insert(index + 1, copy);
    ++revision_;
}

void BeatGridModel::deleteSection(int index)
{
    if (sections_.size() <= 1 || index < 0 || index >= sections_.size()) {
        return;
    }
    sections_.removeAt(index);
    ++revision_;
}

void BeatGridModel::renameSection(int index, const QString& label, const QString& name)
{
    if (index < 0 || index >= sections_.size()) {
        return;
    }
    sections_[index].label = label.trimmed().isEmpty() ? QStringLiteral("A") : label.trimmed();
    sections_[index].name = name.trimmed().isEmpty() ? QStringLiteral("Section") : name.trimmed();
    ++revision_;
}

void BeatGridModel::moveSection(int from, int to)
{
    if (from < 0 || from >= sections_.size() || to < 0 || to >= sections_.size() || from == to) {
        return;
    }
    sections_.move(from, to);
    ++revision_;
}

void BeatGridModel::reset()
{
    title_ = QStringLiteral("Untitled Jam");
    lyricsText_.clear();
    sections_.clear();
    SongSection section;
    normalize(section);
    sections_.push_back(section);
    revision_ = 0;
}

QJsonObject BeatGridModel::toJson() const
{
    QJsonArray sections;
    for (const SongSection& section : sections_) {
        QJsonArray chords;
        QJsonArray beatNotes;
        QJsonArray lyrics;
        QJsonArray beatPatterns;
        for (const QString& value : section.chords) {
            chords.append(value);
        }
        for (const QString& value : section.beatNotes) {
            beatNotes.append(value);
        }
        for (const QString& value : section.lyrics) {
            lyrics.append(value);
        }
        for (const BeatPattern& pattern : section.beatPatterns) {
            QJsonArray lanes;
            for (const QString& value : pattern.lanes) {
                lanes.append(value);
            }
            beatPatterns.append(QJsonObject{
                {QStringLiteral("division"), pattern.division},
                {QStringLiteral("lanes"), lanes},
            });
        }
        sections.append(QJsonObject{
            {QStringLiteral("label"), section.label},
            {QStringLiteral("name"), section.name},
            {QStringLiteral("beats"), section.beats},
            {QStringLiteral("chords"), chords},
            {QStringLiteral("beat_notes"), beatNotes},
            {QStringLiteral("lyrics"), lyrics},
            {QStringLiteral("beat_patterns"), beatPatterns},
        });
    }
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("jam2.song.v1")},
        {QStringLiteral("title"), title_},
        {QStringLiteral("lyrics_text"), lyricsText_},
        {QStringLiteral("sections"), sections},
    };
}

bool BeatGridModel::loadJson(const QJsonObject& object)
{
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("jam2.song.v1")) {
        return false;
    }
    QVector<SongSection> loaded;
    const QJsonArray sections = object.value(QStringLiteral("sections")).toArray();
    for (const QJsonValue& value : sections) {
        const QJsonObject item = value.toObject();
        SongSection section;
        section.label = item.value(QStringLiteral("label")).toString(QStringLiteral("A"));
        section.name = item.value(QStringLiteral("name")).toString(QStringLiteral("Section"));
        section.beats = item.value(QStringLiteral("beats")).toInt(8);
        normalize(section);

        const QJsonArray chords = item.value(QStringLiteral("chords")).toArray();
        const QJsonArray beatNotes = item.value(QStringLiteral("beat_notes")).toArray();
        const QJsonArray lyrics = item.value(QStringLiteral("lyrics")).toArray();
        const QJsonArray beatPatterns = item.value(QStringLiteral("beat_patterns")).toArray();
        for (int i = 0; i < section.beats && i < chords.size(); ++i) {
            section.chords[i] = chords[i].toString();
        }
        for (int i = 0; i < section.beats && i < beatNotes.size(); ++i) {
            section.beatNotes[i] = beatNotes[i].toString();
        }
        for (int i = 0; i < section.beats && i < lyrics.size(); ++i) {
            section.lyrics[i] = lyrics[i].toString();
        }
        for (int i = 0; i < section.beats && i < beatPatterns.size(); ++i) {
            const QJsonObject pattern = beatPatterns[i].toObject();
            section.beatPatterns[i].division = normalizedDivision(pattern.value(QStringLiteral("division")).toInt(kDefaultBeatDivision));
            const QJsonArray lanes = pattern.value(QStringLiteral("lanes")).toArray();
            for (int lane = 0; lane < section.beatPatterns[i].lanes.size() && lane < lanes.size(); ++lane) {
                section.beatPatterns[i].lanes[lane] = lanes[lane].toString();
            }
        }
        if (beatPatterns.isEmpty()) {
            const int specialLane = beatLaneNames().indexOf(QStringLiteral("Special"));
            for (int i = 0; specialLane >= 0 && i < section.beats && i < beatNotes.size(); ++i) {
                section.beatPatterns[i].lanes[specialLane] = beatNotes[i].toString();
            }
        }
        loaded.push_back(section);
    }
    if (loaded.isEmpty()) {
        return false;
    }
    title_ = object.value(QStringLiteral("title")).toString(QStringLiteral("Untitled Jam"));
    lyricsText_ = object.value(QStringLiteral("lyrics_text")).toString();
    if (lyricsText_.isEmpty()) {
        QStringList legacyLyrics;
        for (const SongSection& section : loaded) {
            for (const QString& value : section.lyrics) {
                if (!value.trimmed().isEmpty()) {
                    legacyLyrics << value;
                }
            }
        }
        lyricsText_ = legacyLyrics.join(QStringLiteral("\n"));
    }
    sections_ = loaded;
    ++revision_;
    return true;
}

QStringList BeatGridModel::beatLaneNames()
{
    return QStringList{
        QStringLiteral("Kick"),
        QStringLiteral("Snare"),
        QStringLiteral("Closed HH"),
        QStringLiteral("Open HH"),
        QStringLiteral("Crash"),
        QStringLiteral("Splash"),
        QStringLiteral("Cymbal"),
        QStringLiteral("Tom"),
        QStringLiteral("Special"),
        QStringLiteral("Guitar"),
        QStringLiteral("Bass"),
    };
}

QList<int> BeatGridModel::beatDivisionValues()
{
    return QList<int>{1, 2, 4, 8, 3, 6};
}

QString BeatGridModel::beatDivisionLabel(int division)
{
    switch (normalizedDivision(division)) {
    case 1:
        return QStringLiteral("Quarter");
    case 2:
        return QStringLiteral("Eighth");
    case 3:
        return QStringLiteral("Triplet");
    case 6:
        return QStringLiteral("6th");
    case 8:
        return QStringLiteral("32nd");
    case 4:
    default:
        return QStringLiteral("16th");
    }
}

int BeatGridModel::normalizedDivision(int division)
{
    return beatDivisionValues().contains(division) ? division : kDefaultBeatDivision;
}

void BeatGridModel::normalize(SongSection& section)
{
    section.beats = qMax(4, section.beats);
    section.chords.resize(section.beats);
    section.beatNotes.resize(section.beats);
    section.lyrics.resize(section.beats);
    section.beatPatterns.resize(section.beats);
    const int laneCount = beatLaneNames().size();
    for (BeatPattern& pattern : section.beatPatterns) {
        pattern.division = normalizedDivision(pattern.division);
        pattern.lanes.resize(laneCount);
    }
}
