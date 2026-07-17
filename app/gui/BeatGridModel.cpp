#include "BeatGridModel.hpp"
#include "ContentLimits.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>

#include <algorithm>
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

bool BeatGridModel::hasOnlyPristineSection() const
{
    if (sections_.size() != 1) {
        return false;
    }
    const SongSection& section = sections_.front();
    if (!section.generatedKind.isEmpty() || section.label != QStringLiteral("A") ||
        section.name != QStringLiteral("Verse") || section.beats != 32) {
        return false;
    }
    const auto allEmpty = [](const QVector<QString>& values) {
        return std::all_of(values.cbegin(), values.cend(),
            [](const QString& value) { return value.trimmed().isEmpty(); });
    };
    if (!allEmpty(section.chords) || !allEmpty(section.targets) ||
        !allEmpty(section.beatNotes) || !allEmpty(section.lyrics)) {
        return false;
    }
    return std::all_of(section.beatPatterns.cbegin(), section.beatPatterns.cend(),
        [](const BeatPattern& pattern) {
            return std::all_of(pattern.lanes.cbegin(), pattern.lanes.cend(),
                [](const QString& lane) { return lane.trimmed().isEmpty(); });
        });
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
    } else if (lane == QStringLiteral("target")) {
        section.targets[beat] = text.left(kMaxCellCharacters);
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
    section.beats = beats > 0 ? beats : (sections_.isEmpty() ? 32 : sections_.back().beats);
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
    copy.id = sectionId();
    copy.generatedKind.clear();
    copy.generatedKey.clear();
    copy.generatedStyle.clear();
    copy.generatedCharacter.clear();
    copy.generatedBars = 0;
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

int BeatGridModel::replaceGeneratedSection(const QString& kind, SongSection section)
{
    const QString normalizedKind = kind.trimmed().left(kMaxCellCharacters);
    if (normalizedKind.isEmpty()) {
        return -1;
    }
    section.generatedKind = normalizedKind;
    normalize(section);
    for (int index = 0; index < sections_.size(); ++index) {
        if (sections_[index].generatedKind == normalizedKind) {
            section.id = sections_[index].id;
            sections_[index] = std::move(section);
            ++revision_;
            return index;
        }
    }
    if (hasOnlyPristineSection()) {
        section.id = sections_.front().id;
        sections_.front() = std::move(section);
        ++revision_;
        return 0;
    }
    if (sections_.size() >= kMaxSections) {
        return -1;
    }
    sections_.push_back(std::move(section));
    ++revision_;
    return sections_.size() - 1;
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
        QJsonArray targets;
        QJsonArray beatNotes;
        QJsonArray lyrics;
        QJsonArray beatPatterns;
        for (const QString& value : section.chords) {
            chords.append(value);
        }
        for (const QString& value : section.targets) {
            targets.append(value);
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
            {QStringLiteral("id"), section.id},
            {QStringLiteral("label"), section.label},
            {QStringLiteral("name"), section.name},
            {QStringLiteral("beats"), section.beats},
            {QStringLiteral("chords"), chords},
            {QStringLiteral("targets"), targets},
            {QStringLiteral("beat_notes"), beatNotes},
            {QStringLiteral("lyrics"), lyrics},
            {QStringLiteral("beat_patterns"), beatPatterns},
            {QStringLiteral("generated_kind"), section.generatedKind},
            {QStringLiteral("generated_key"), section.generatedKey},
            {QStringLiteral("generated_style"), section.generatedStyle},
            {QStringLiteral("generated_character"), section.generatedCharacter},
            {QStringLiteral("generated_bars"), section.generatedBars},
        });
    }
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("jam2.song.v2")},
        {QStringLiteral("title"), title_},
        {QStringLiteral("lyrics_text"), lyricsText_},
        {QStringLiteral("sections"), sections},
    };
}

bool BeatGridModel::loadJson(const QJsonObject& object)
{
    const QString format = object.value(QStringLiteral("format")).toString();
    if (format != QStringLiteral("jam2.song.v1") && format != QStringLiteral("jam2.song.v2")) {
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
        if ((!item.value(QStringLiteral("id")).isUndefined() && !item.value(QStringLiteral("id")).isString()) ||
            (!item.value(QStringLiteral("label")).isUndefined() && !item.value(QStringLiteral("label")).isString()) ||
            (!item.value(QStringLiteral("name")).isUndefined() && !item.value(QStringLiteral("name")).isString()) ||
            (!item.value(QStringLiteral("generated_kind")).isUndefined() && !item.value(QStringLiteral("generated_kind")).isString()) ||
            (!item.value(QStringLiteral("generated_key")).isUndefined() && !item.value(QStringLiteral("generated_key")).isString()) ||
            (!item.value(QStringLiteral("generated_style")).isUndefined() && !item.value(QStringLiteral("generated_style")).isString()) ||
            (!item.value(QStringLiteral("generated_character")).isUndefined() && !item.value(QStringLiteral("generated_character")).isString()) ||
            (!item.value(QStringLiteral("beats")).isUndefined() && !item.value(QStringLiteral("beats")).isDouble()) ||
            (!item.value(QStringLiteral("generated_bars")).isUndefined() && !item.value(QStringLiteral("generated_bars")).isDouble())) {
            return false;
        }
        SongSection section;
        section.id = sectionId(item.value(QStringLiteral("id")).toString());
        section.label = item.value(QStringLiteral("label")).toString(QStringLiteral("A"));
        section.name = item.value(QStringLiteral("name")).toString(QStringLiteral("Section"));
        section.beats = item.value(QStringLiteral("beats")).toInt(8);
        section.generatedKind = item.value(QStringLiteral("generated_kind")).toString();
        section.generatedKey = item.value(QStringLiteral("generated_key")).toString();
        section.generatedStyle = item.value(QStringLiteral("generated_style")).toString();
        section.generatedCharacter = item.value(QStringLiteral("generated_character")).toString();
        section.generatedBars = item.value(QStringLiteral("generated_bars")).toInt();
        const double beatsValue = item.value(QStringLiteral("beats")).toDouble(8.0);
        const double generatedBarsValue = item.value(QStringLiteral("generated_bars")).toDouble(0.0);
        if (!std::isfinite(beatsValue) || std::floor(beatsValue) != beatsValue ||
            !std::isfinite(generatedBarsValue) || std::floor(generatedBarsValue) != generatedBarsValue ||
            section.label.size() > kMaxCellCharacters || section.name.size() > kMaxCellCharacters ||
            section.id.size() > kMaxCellCharacters || section.generatedKind.size() > kMaxCellCharacters ||
            section.generatedKey.size() > kMaxCellCharacters || section.generatedStyle.size() > kMaxCellCharacters ||
            section.generatedCharacter.size() > kMaxCellCharacters ||
            section.generatedBars < 0 || section.generatedBars > kMaxBeatsPerSection ||
            section.beats < kMinBeatsPerSection || section.beats > kMaxBeatsPerSection) {
            return false;
        }
        normalize(section);

        const QJsonArray chords = item.value(QStringLiteral("chords")).toArray();
        const QJsonArray targets = item.value(QStringLiteral("targets")).toArray();
        const QJsonArray beatNotes = item.value(QStringLiteral("beat_notes")).toArray();
        const QJsonArray lyrics = item.value(QStringLiteral("lyrics")).toArray();
        const QJsonArray beatPatterns = item.value(QStringLiteral("beat_patterns")).toArray();
        for (const QString& key : {
                QStringLiteral("chords"), QStringLiteral("targets"), QStringLiteral("beat_notes"),
                QStringLiteral("lyrics"), QStringLiteral("beat_patterns")}) {
            const QJsonValue array = item.value(key);
            if (!array.isUndefined() && !array.isArray()) {
                return false;
            }
        }
        if (!validTextArray(chords, section.beats) || !validTextArray(targets, section.beats) ||
            !validTextArray(beatNotes, section.beats) ||
            !validTextArray(lyrics, section.beats) || beatPatterns.size() > section.beats) {
            return false;
        }
        for (int i = 0; i < section.beats && i < chords.size(); ++i) {
            section.chords[i] = chords[i].toString();
        }
        for (int i = 0; i < section.beats && i < targets.size(); ++i) {
            section.targets[i] = targets[i].toString();
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
    section.id = sectionId(section.id);
    section.beats = qBound(kMinBeatsPerSection, section.beats, kMaxBeatsPerSection);
    section.chords.resize(section.beats);
    section.targets.resize(section.beats);
    section.beatNotes.resize(section.beats);
    section.lyrics.resize(section.beats);
    section.beatPatterns.resize(section.beats);
    const int laneCount = beatLaneNames().size();
    for (BeatPattern& pattern : section.beatPatterns) {
        pattern.division = normalizedDivision(pattern.division);
        pattern.lanes.resize(laneCount);
    }
}

QString BeatGridModel::sectionId(const QString& value)
{
    return value.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : value.left(kMaxCellCharacters);
}
