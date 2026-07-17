#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

struct BeatPattern {
    int division = 4;
    QVector<QString> lanes;
};

struct SongSection {
    QString id;
    QString label = QStringLiteral("A");
    QString name = QStringLiteral("Verse");
    int beats = 32;
    QVector<QString> chords;
    QVector<QString> targets;
    QVector<QString> beatNotes;
    QVector<QString> lyrics;
    QVector<BeatPattern> beatPatterns;
    QString generatedKind;
    QString generatedKey;
    QString generatedStyle;
    QString generatedCharacter;
    int generatedBars = 0;
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
    bool hasOnlyPristineSection() const;

    void setCell(int section, const QString& lane, int beat, const QString& text);
    void setBeatDivision(int section, int beat, int division);
    void setBeatHit(int section, int beat, int lane, const QString& text);
    QString lyricsText() const;
    void setLyricsText(const QString& text);
    void resizeSection(int section, int beats);
    void resizeAllSections(int beats);
    void addSection(int beats = -1);
    void duplicateSection(int index);
    void deleteSection(int index);
    void renameSection(int index, const QString& label, const QString& name);
    void moveSection(int from, int to);
    int replaceGeneratedSection(const QString& kind, SongSection section);
    void reset();
    QJsonObject toJson() const;
    bool loadJson(const QJsonObject& object);

    static QStringList beatLaneNames();
    static QList<int> beatDivisionValues();
    static QString beatDivisionLabel(int division);

private:
    static QString sectionId(const QString& value = {});
    static int normalizedDivision(int division);
    static void normalize(SongSection& section);

    QString title_ = QStringLiteral("Untitled Jam");
    QString lyricsText_;
    int revision_ = 0;
    QVector<SongSection> sections_;
};
