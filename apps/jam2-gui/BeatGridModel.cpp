#include "BeatGridModel.hpp"

#include <QJsonArray>
#include <QJsonValue>

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

void BeatGridModel::addSection()
{
    SongSection section;
    section.label = QString(QChar(static_cast<ushort>('A' + (sections_.size() % 26))));
    section.name = QStringLiteral("Section");
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
        for (const QString& value : section.chords) {
            chords.append(value);
        }
        for (const QString& value : section.beatNotes) {
            beatNotes.append(value);
        }
        for (const QString& value : section.lyrics) {
            lyrics.append(value);
        }
        sections.append(QJsonObject{
            {QStringLiteral("label"), section.label},
            {QStringLiteral("name"), section.name},
            {QStringLiteral("beats"), section.beats},
            {QStringLiteral("chords"), chords},
            {QStringLiteral("beat_notes"), beatNotes},
            {QStringLiteral("lyrics"), lyrics},
        });
    }
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("jam2.song.v1")},
        {QStringLiteral("title"), title_},
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
        for (int i = 0; i < section.beats && i < chords.size(); ++i) {
            section.chords[i] = chords[i].toString();
        }
        for (int i = 0; i < section.beats && i < beatNotes.size(); ++i) {
            section.beatNotes[i] = beatNotes[i].toString();
        }
        for (int i = 0; i < section.beats && i < lyrics.size(); ++i) {
            section.lyrics[i] = lyrics[i].toString();
        }
        loaded.push_back(section);
    }
    if (loaded.isEmpty()) {
        return false;
    }
    title_ = object.value(QStringLiteral("title")).toString(QStringLiteral("Untitled Jam"));
    sections_ = loaded;
    ++revision_;
    return true;
}

void BeatGridModel::normalize(SongSection& section)
{
    section.beats = qMax(4, section.beats);
    section.chords.resize(section.beats);
    section.beatNotes.resize(section.beats);
    section.lyrics.resize(section.beats);
}
