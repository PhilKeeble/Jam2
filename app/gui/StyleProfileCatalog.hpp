#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace jam2::practice {

struct MeterDefinition {
    QString id;
    QString name;
    int numerator = 4;
    int denominator = 4;
    QVector<int> grouping{4};
    QString subdivisionFamily = QStringLiteral("straight-eighth");
    QString perceivedTime = QStringLiteral("normal");
    int clickDivision = 1;
    int tempoPulseUnits = 1;
    QString tempoPulseName = QStringLiteral("quarter note");
};

struct NativeFormDefinition {
    QString id;
    QString name;
    int bars = 16;
    QString meterId = QStringLiteral("4-4");
    int phraseBars = 4;
    QString description;
};

struct StyleDefinition {
    QString id;
    QString name;
    QString summary;
    QStringList profileIds;
};

struct ProfileDefinition {
    QString id;
    QString styleId;
    QString name;
    QString grammarId;
    int minimumBpm = 80;
    int maximumBpm = 140;
    QString teachingSummary;
    QString jamGuidance;
    QStringList tonalCollections;
    QStringList progressionFamilies;
    QStringList grooveFamilies;
    QString bassGrammar;
    QStringList supportingRoles;
    QString motifGrammar;
    QString chordPatchId;
    QString melodyPatchId;
    QString bassPatchId;
    QString supportPatchId;
    QString drumPatchId;
    QStringList meterIds;
    QVector<NativeFormDefinition> forms;
    QStringList compatibleProductionFamilies;
    bool experimental = false;
};

struct ComplexityLevelDefinition {
    int level = 1;
    QString id;
    QString name;
    QString teachingSummary;
    QStringList unlockedTools;
};

struct ProductionFamilyDefinition {
    QString id;
    QString name;
    QString teachingSummary;
    QStringList compatibleStyleIds;
};

const QVector<StyleDefinition>& styleCatalog();
const QVector<ProfileDefinition>& profileCatalog(bool includeExperimental = false);
const QVector<MeterDefinition>& meterCatalog();
const QVector<ComplexityLevelDefinition>& complexityCatalog();
const QVector<ProductionFamilyDefinition>& productionFamilyCatalog();

const StyleDefinition* findStyle(const QString& id);
const ProfileDefinition* findProfile(const QString& id, bool includeExperimental = true);
const MeterDefinition* findMeter(const QString& id);
const NativeFormDefinition* findNativeForm(const ProfileDefinition& profile, const QString& id);
const ComplexityLevelDefinition* findComplexityLevel(int level);
const ProductionFamilyDefinition* findProductionFamily(const QString& id);

QVector<const ProfileDefinition*> profilesForStyle(
    const QString& styleId,
    bool includeExperimental = false);
QStringList profileNamesForStyle(const QString& styleId, bool includeExperimental = false);
QStringList profileIdsForStyle(const QString& styleId, bool includeExperimental = false);
QStringList compatibleProductionFamilyIds(const ProfileDefinition& profile);

} // namespace jam2::practice
