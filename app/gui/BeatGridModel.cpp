#include "BeatGridModel.hpp"
#include "ContentLimits.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

namespace {

constexpr int kDefaultBeatDivision = 4;
constexpr int kMinBeatsPerSection = jam2::application::limits::kMinimumBeatsPerSection;
constexpr int kMaxBeatsPerSection = jam2::application::limits::kMaximumBeatsPerSection;
constexpr int kMaxSections = jam2::application::limits::kMaximumSongSections;
constexpr int kMaxCellCharacters = jam2::application::limits::kMaximumCellCharacters;
constexpr int kMaxTitleCharacters = jam2::application::limits::kMaximumTitleCharacters;
constexpr int kMaxLyricsCharacters = jam2::application::limits::kMaximumLyricsCharacters;

bool validTextArray(const QJsonArray& values, int maximumCount)
{
    if (values.size() > maximumCount) {
        return false;
    }
    for (const QJsonValue& value : values) {
        if (!value.isString() || value.toString().size() > kMaxCellCharacters) {
            return false;
        }
    }
    return true;
}

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
    const QString trimmed = title.trimmed().left(kMaxTitleCharacters);
    title_ = trimmed.isEmpty() ? QStringLiteral("Untitled Jam") : trimmed;
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
        section.chords[beat] = text.left(kMaxCellCharacters);
    } else if (lane == QStringLiteral("beat")) {
        section.beatNotes[beat] = text.left(kMaxCellCharacters);
    } else if (lane == QStringLiteral("lyric")) {
        section.lyrics[beat] = text.left(kMaxCellCharacters);
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
    section.beatPatterns[beat].lanes[lane] = text.left(kMaxCellCharacters);
    ++revision_;
}

QString BeatGridModel::lyricsText() const
{
    return lyricsText_;
}

void BeatGridModel::setLyricsText(const QString& text)
{
    lyricsText_ = text.left(kMaxLyricsCharacters);
    ++revision_;
}

void BeatGridModel::resizeSection(int sectionIndex, int beats)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size()) {
        return;
    }
    SongSection& section = sections_[sectionIndex];
    section.beats = qBound(kMinBeatsPerSection, beats, kMaxBeatsPerSection);
    normalize(section);
    ++revision_;
}

void BeatGridModel::resizeAllSections(int beats)
{
    const int clampedBeats = qBound(kMinBeatsPerSection, beats, kMaxBeatsPerSection);
    for (SongSection& section : sections_) {
        section.beats = clampedBeats;
        normalize(section);
    }
    ++revision_;
}

void BeatGridModel::addSection(int beats)
{
    if (sections_.size() >= kMaxSections) {
        return;
    }
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
    if (index < 0 || index >= sections_.size() || sections_.size() >= kMaxSections) {
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
    sections_[index].label = label.trimmed().isEmpty() ? QStringLiteral("A") : label.trimmed().left(kMaxCellCharacters);
    sections_[index].name = name.trimmed().isEmpty() ? QStringLiteral("Section") : name.trimmed().left(kMaxCellCharacters);
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
    if (!object.value(QStringLiteral("sections")).isArray() ||
        (!object.value(QStringLiteral("title")).isUndefined() && !object.value(QStringLiteral("title")).isString()) ||
        (!object.value(QStringLiteral("lyrics_text")).isUndefined() && !object.value(QStringLiteral("lyrics_text")).isString())) {
        return false;
    }
    QVector<SongSection> loaded;
    const QJsonArray sections = object.value(QStringLiteral("sections")).toArray();
    if (sections.isEmpty() || sections.size() > kMaxSections ||
        object.value(QStringLiteral("title")).toString().size() > kMaxTitleCharacters ||
        object.value(QStringLiteral("lyrics_text")).toString().size() > kMaxLyricsCharacters) {
        return false;
    }
    for (const QJsonValue& value : sections) {
        if (!value.isObject()) {
            return false;
        }
        const QJsonObject item = value.toObject();
        if ((!item.value(QStringLiteral("label")).isUndefined() && !item.value(QStringLiteral("label")).isString()) ||
            (!item.value(QStringLiteral("name")).isUndefined() && !item.value(QStringLiteral("name")).isString()) ||
            (!item.value(QStringLiteral("beats")).isUndefined() && !item.value(QStringLiteral("beats")).isDouble())) {
            return false;
        }
        SongSection section;
        section.label = item.value(QStringLiteral("label")).toString(QStringLiteral("A"));
        section.name = item.value(QStringLiteral("name")).toString(QStringLiteral("Section"));
        section.beats = item.value(QStringLiteral("beats")).toInt(8);
        const double beatsValue = item.value(QStringLiteral("beats")).toDouble(8.0);
        if (!std::isfinite(beatsValue) || std::floor(beatsValue) != beatsValue ||
            section.label.size() > kMaxCellCharacters || section.name.size() > kMaxCellCharacters ||
            section.beats < kMinBeatsPerSection || section.beats > kMaxBeatsPerSection) {
            return false;
        }
        normalize(section);

        const QJsonArray chords = item.value(QStringLiteral("chords")).toArray();
        const QJsonArray beatNotes = item.value(QStringLiteral("beat_notes")).toArray();
        const QJsonArray lyrics = item.value(QStringLiteral("lyrics")).toArray();
        const QJsonArray beatPatterns = item.value(QStringLiteral("beat_patterns")).toArray();
        for (const QString& key : {
                QStringLiteral("chords"), QStringLiteral("beat_notes"),
                QStringLiteral("lyrics"), QStringLiteral("beat_patterns")}) {
            const QJsonValue array = item.value(key);
            if (!array.isUndefined() && !array.isArray()) {
                return false;
            }
        }
        if (!validTextArray(chords, section.beats) || !validTextArray(beatNotes, section.beats) ||
            !validTextArray(lyrics, section.beats) || beatPatterns.size() > section.beats) {
            return false;
        }
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
            if (!beatPatterns[i].isObject()) {
                return false;
            }
            const QJsonObject pattern = beatPatterns[i].toObject();
            if ((!pattern.value(QStringLiteral("division")).isUndefined() &&
                 !pattern.value(QStringLiteral("division")).isDouble()) ||
                (!pattern.value(QStringLiteral("lanes")).isUndefined() &&
                 !pattern.value(QStringLiteral("lanes")).isArray())) {
                return false;
            }
            const double divisionValue = pattern.value(QStringLiteral("division")).toDouble(kDefaultBeatDivision);
            if (!std::isfinite(divisionValue) || std::floor(divisionValue) != divisionValue) {
                return false;
            }
            section.beatPatterns[i].division = normalizedDivision(pattern.value(QStringLiteral("division")).toInt(kDefaultBeatDivision));
            const QJsonArray lanes = pattern.value(QStringLiteral("lanes")).toArray();
            if (!validTextArray(lanes, section.beatPatterns[i].lanes.size())) {
                return false;
            }
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
    section.beats = qBound(kMinBeatsPerSection, section.beats, kMaxBeatsPerSection);
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
