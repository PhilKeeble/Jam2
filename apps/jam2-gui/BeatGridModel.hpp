#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

struct SongSection {
    QString label = QStringLiteral("A");
    QString name = QStringLiteral("Verse");
    int beats = 8;
    QVector<QString> chords;
    QVector<QString> beatNotes;
    QVector<QString> lyrics;
};

class BeatGridModel {
public:
    BeatGridModel();

    QString title() const;
    void setTitle(const QString& title);
    int revision() const;
    const QVector<SongSection>& sections() const;
    SongSection& section(int index);
    const SongSection& section(int index) const;

    void setCell(int section, const QString& lane, int beat, const QString& text);
    void resizeSection(int section, int beats);
    void addSection();
    void duplicateSection(int index);
    void deleteSection(int index);
    void renameSection(int index, const QString& label, const QString& name);
    void moveSection(int from, int to);
    void reset();
    QJsonObject toJson() const;
    bool loadJson(const QJsonObject& object);

private:
    static void normalize(SongSection& section);

    QString title_ = QStringLiteral("Untitled Jam");
    int revision_ = 0;
    QVector<SongSection> sections_;
};
