#pragma once

#include "GenerationRecipe.hpp"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

struct BeatPattern {
    int division = 4;
    QVector<QString> lanes;
};

enum class MusicalStepState {
    Rest = 0,
    Hold = 1,
    Onset = 2,
};

struct MusicalStep {
    MusicalStepState state = MusicalStepState::Rest;
    QString value;
    int velocity = 88;
};

struct MusicalBeatPattern {
    int division = 1;
    QVector<MusicalStep> chords;
    QVector<MusicalStep> melody;
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
    QVector<MusicalBeatPattern> musicalPatterns;
    QString generatedKind;
    jam2::practice::GenerationRecipe generatedRecipe;
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
    void setMusicalDivision(int section, int beat, int division);
    void setMusicalStep(int section, int beat, int step, const QString& lane, const QString& text);
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
    static QStringList beatVisualLaneNames();
    static QList<int> beatDivisionValues();
    static QString beatDivisionLabel(int division);
    static QList<int> musicalDivisionValues();
    static QString musicalDivisionLabel(int division);

private:
    static QString sectionId(const QString& value = {});
    static int normalizedDivision(int division);
    static int normalizedMusicalDivision(int division);
    static void normalize(SongSection& section);

    QString title_ = QStringLiteral("Untitled Jam");
    int revision_ = 0;
    QVector<SongSection> sections_;
};
