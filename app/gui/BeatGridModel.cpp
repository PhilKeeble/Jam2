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
constexpr int kCurrentBeatLaneSchema = 3;
constexpr int kMinBeatsPerSection = jam2::application::limits::kMinimumBeatsPerSection;
constexpr int kMaxBeatsPerSection = jam2::application::limits::kMaximumBeatsPerSection;
constexpr int kMaxSections = jam2::application::limits::kMaximumSongSections;
constexpr int kMaxCellCharacters = jam2::application::limits::kMaximumCellCharacters;
constexpr int kMaxTitleCharacters = jam2::application::limits::kMaximumTitleCharacters;

SongSection defaultSection(int index, int beats = 32)
{
    SongSection section;
    section.label = QString(QChar(QLatin1Char('A').unicode() + qBound(0, index, 3)));
    section.name = QStringLiteral("Section %1").arg(section.label);
    section.beats = beats;
    return section;
}

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

bool exactInteger(const QJsonValue& value, int minimum, int maximum)
{
    if (!value.isDouble()) return false;
    const double raw = value.toDouble();
    return std::isfinite(raw) && raw == std::floor(raw) &&
        raw >= minimum && raw <= maximum;
}

QString musicalStateName(MusicalStepState state)
{
    switch (state) {
    case MusicalStepState::Onset:
        return QStringLiteral("onset");
    case MusicalStepState::Hold:
        return QStringLiteral("hold");
    case MusicalStepState::Rest:
    default:
        return QStringLiteral("rest");
    }
}

bool musicalStateFromName(const QString& name, MusicalStepState& state)
{
    if (name == QStringLiteral("onset")) state = MusicalStepState::Onset;
    else if (name == QStringLiteral("hold")) state = MusicalStepState::Hold;
    else if (name == QStringLiteral("rest")) state = MusicalStepState::Rest;
    else return false;
    return true;
}

MusicalStep stepFromLegacy(const QString& text)
{
    MusicalStep step;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed == QStringLiteral("~")) {
        step.state = MusicalStepState::Hold;
    } else if (trimmed == QStringLiteral("-")) {
        step.state = MusicalStepState::Rest;
    } else {
        step.state = MusicalStepState::Onset;
        step.value = text.left(kMaxCellCharacters);
    }
    return step;
}

QString legacyFromStep(const MusicalStep& step)
{
    if (step.state == MusicalStepState::Onset) return step.value.left(kMaxCellCharacters);
    if (step.state == MusicalStepState::Rest) return QStringLiteral("-");
    return {};
}

QString harmonicSummaryFromSteps(const QVector<MusicalStep>& steps)
{
    for (const MusicalStep& step : steps) {
        if (step.state == MusicalStepState::Onset &&
            !step.value.trimmed().isEmpty()) {
            return step.value.left(kMaxCellCharacters);
        }
    }
    const bool entirelyResting = !steps.isEmpty() &&
        std::all_of(steps.cbegin(), steps.cend(), [](const MusicalStep& step) {
            return step.state == MusicalStepState::Rest;
        });
    return entirelyResting ? QStringLiteral("-") : QString();
}

QJsonArray musicalStepsToJson(const QVector<MusicalStep>& steps)
{
    QJsonArray result;
    for (const MusicalStep& step : steps) {
        result.append(QJsonObject{
            {QStringLiteral("state"), musicalStateName(step.state)},
            {QStringLiteral("value"), step.value},
            {QStringLiteral("velocity"), step.velocity},
            {QStringLiteral("articulation"), step.articulation},
            {QStringLiteral("voicing"), step.voicing},
        });
    }
    return result;
}

bool musicalStepsFromJson(const QJsonValue& value, int division, QVector<MusicalStep>& output)
{
    if (!value.isArray() || value.toArray().size() != division) return false;
    QVector<MusicalStep> result;
    result.reserve(division);
    for (const QJsonValue& entry : value.toArray()) {
        if (!entry.isObject()) return false;
        const QJsonObject object = entry.toObject();
        if (!object.value(QStringLiteral("state")).isString() ||
            !object.value(QStringLiteral("value")).isString() ||
            !object.value(QStringLiteral("velocity")).isDouble()) return false;
        const double velocity = object.value(QStringLiteral("velocity")).toDouble();
        MusicalStep step;
        if (!musicalStateFromName(object.value(QStringLiteral("state")).toString(), step.state) ||
            object.value(QStringLiteral("value")).toString().size() > kMaxCellCharacters ||
            !std::isfinite(velocity) || velocity != std::floor(velocity) ||
            velocity < 1.0 || velocity > 127.0) return false;
        step.value = object.value(QStringLiteral("value")).toString();
        step.velocity = static_cast<int>(velocity);
        const QJsonValue articulation = object.value(QStringLiteral("articulation"));
        if (!articulation.isUndefined() &&
            (!articulation.isString() || articulation.toString().size() > 96)) return false;
        step.articulation = articulation.toString();
        const QJsonValue voicing = object.value(QStringLiteral("voicing"));
        if (!voicing.isUndefined() &&
            (!voicing.isString() || voicing.toString().size() > 32)) return false;
        step.voicing = voicing.toString();
        result.push_back(std::move(step));
    }
    output = std::move(result);
    return true;
}

QVector<MusicalStep> resizedMusicalSteps(const QVector<MusicalStep>& oldSteps, int oldDivision, int newDivision)
{
    QVector<MusicalStep> result(newDivision);
    if (oldSteps.isEmpty()) return result;
    for (int index = 0; index < newDivision; ++index) {
        const int source = qBound(0, (index * oldDivision) / newDivision, oldSteps.size() - 1);
        result[index] = oldSteps[source];
        if (result[index].state == MusicalStepState::Onset &&
            index * oldDivision != source * newDivision) {
            result[index].state = MusicalStepState::Hold;
            result[index].value.clear();
        }
    }
    for (int index = 0; index < oldSteps.size(); ++index) {
        if (oldSteps[index].state == MusicalStepState::Hold) continue;
        const int destination = qBound(0,
            static_cast<int>(std::lround(static_cast<double>(index) * newDivision / oldDivision)),
            newDivision - 1);
        result[destination] = oldSteps[index];
    }
    return result;
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

int BeatGridModel::guitarStringCount() const
{
    return guitarStringCount_;
}

bool BeatGridModel::guitarDropTuning() const
{
    return guitarDropTuning_;
}

bool BeatGridModel::setGuitarReference(int strings, bool dropped)
{
    if (strings != 6 && strings != 7 && strings != 8) return false;
    if (guitarStringCount_ == strings && guitarDropTuning_ == dropped) return false;
    guitarStringCount_ = strings;
    guitarDropTuning_ = dropped;
    ++revision_;
    return true;
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
    if (sections_.size() != kMaxSections) {
        return false;
    }
    const auto allEmpty = [](const QVector<QString>& values) {
        return std::all_of(values.cbegin(), values.cend(),
            [](const QString& value) { return value.trimmed().isEmpty(); });
    };
    for (int index = 0; index < sections_.size(); ++index) {
        const SongSection& section = sections_[index];
        const SongSection expected = defaultSection(index);
        if (!section.generatedKind.isEmpty() || section.label != expected.label ||
            section.name != expected.name || section.beats != 32 ||
            !allEmpty(section.chords) || !allEmpty(section.targets) ||
            !allEmpty(section.beatNotes) || !allEmpty(section.lyrics) ||
            !std::all_of(section.beatPatterns.cbegin(), section.beatPatterns.cend(),
                [](const BeatPattern& pattern) {
                    return std::all_of(pattern.lanes.cbegin(), pattern.lanes.cend(),
                        [](const QString& lane) { return lane.trimmed().isEmpty(); });
                })) {
            return false;
        }
    }
    return true;
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
        section.musicalPatterns[beat].chords[0] = stepFromLegacy(text);
    } else if (lane == QStringLiteral("target")) {
        section.targets[beat] = text.left(kMaxCellCharacters);
        section.musicalPatterns[beat].melody[0] = stepFromLegacy(text);
    } else if (lane == QStringLiteral("beat")) {
        section.beatNotes[beat] = text.left(kMaxCellCharacters);
    } else if (lane == QStringLiteral("lyric")) {
        section.lyrics[beat] = text.left(kMaxCellCharacters);
    } else {
        return;
    }
    ++revision_;
}

void BeatGridModel::setMusicalDivision(int sectionIndex, int beat, int division)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size() || beat < 0) return;
    SongSection& section = sections_[sectionIndex];
    normalize(section);
    if (beat >= section.beats) return;
    MusicalBeatPattern& pattern = section.musicalPatterns[beat];
    const int nextDivision = normalizedMusicalDivision(division);
    if (nextDivision == pattern.division) return;
    pattern.chords = resizedMusicalSteps(pattern.chords, pattern.division, nextDivision);
    pattern.melody = resizedMusicalSteps(pattern.melody, pattern.division, nextDivision);
    pattern.bass = resizedMusicalSteps(pattern.bass, pattern.division, nextDivision);
    pattern.support = resizedMusicalSteps(pattern.support, pattern.division, nextDivision);
    pattern.division = nextDivision;
    section.targets[beat] = legacyFromStep(pattern.melody.front());
    ++revision_;
}

void BeatGridModel::setMusicalStep(
    int sectionIndex,
    int beat,
    int stepIndex,
    const QString& lane,
    const QString& text)
{
    if (sectionIndex < 0 || sectionIndex >= sections_.size() || beat < 0 || stepIndex < 0) return;
    SongSection& section = sections_[sectionIndex];
    normalize(section);
    if (beat >= section.beats) return;
    MusicalBeatPattern& pattern = section.musicalPatterns[beat];
    QVector<MusicalStep>* steps = lane == QStringLiteral("chord") ? &pattern.chords
        : lane == QStringLiteral("melody") ? &pattern.melody
        : lane == QStringLiteral("bass") ? &pattern.bass
        : lane == QStringLiteral("support") ? &pattern.support : nullptr;
    if (!steps || stepIndex >= steps->size()) return;
    (*steps)[stepIndex] = stepFromLegacy(text);
    if (lane == QStringLiteral("chord")) {
        section.chords[beat] = harmonicSummaryFromSteps(*steps);
    } else if (stepIndex == 0) {
        section.targets[beat] = legacyFromStep((*steps)[0]);
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
    copy.generatedRecipe = {};
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

bool BeatGridModel::setDrumKit(int index, const QString& drumKitId)
{
    if (index < 0 || index >= sections_.size() ||
        (drumKitId != QStringLiteral("acoustic") &&
         drumKitId != QStringLiteral("electronic"))) {
        return false;
    }
    if (sections_[index].drumKitId == drumKitId) return true;
    sections_[index].drumKitId = drumKitId;
    ++revision_;
    return true;
}

void BeatGridModel::moveSection(int from, int to)
{
    if (from < 0 || from >= sections_.size() || to < 0 || to >= sections_.size() || from == to) {
        return;
    }
    sections_.move(from, to);
    ++revision_;
}

bool BeatGridModel::replaceSection(int index, SongSection section)
{
    if (index < 0 || index >= sections_.size()) {
        return false;
    }
    section.id = sections_[index].id;
    normalize(section);
    sections_[index] = std::move(section);
    ++revision_;
    return true;
}

bool BeatGridModel::clearSection(int index)
{
    if (index < 0 || index >= sections_.size()) return false;
    const int beats = sections_[index].beats;
    const QString drumKitId = sections_[index].drumKitId;
    SongSection cleared = defaultSection(index, beats);
    cleared.id = sections_[index].id;
    cleared.drumKitId = drumKitId;
    normalize(cleared);
    sections_[index] = std::move(cleared);
    ++revision_;
    return true;
}

bool BeatGridModel::copySection(int source, int destination)
{
    if (source < 0 || source >= sections_.size() ||
        destination < 0 || destination >= sections_.size() || source == destination) {
        return false;
    }
    SongSection copy = sections_[source];
    copy.id = sections_[destination].id;
    sections_[destination] = std::move(copy);
    ++revision_;
    return true;
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

void BeatGridModel::clearContent()
{
    sections_.clear();
    for (int index = 0; index < kMaxSections; ++index) {
        SongSection section = defaultSection(index);
        normalize(section);
        sections_.push_back(std::move(section));
    }
    ++revision_;
}

void BeatGridModel::reset()
{
    title_ = QStringLiteral("Untitled Jam");
    guitarStringCount_ = 6;
    guitarDropTuning_ = false;
    sections_.clear();
    for (int index = 0; index < kMaxSections; ++index) {
        SongSection section = defaultSection(index);
        normalize(section);
        sections_.push_back(std::move(section));
    }
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
        QJsonArray musicalPatterns;
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
        for (const MusicalBeatPattern& pattern : section.musicalPatterns) {
            musicalPatterns.append(QJsonObject{
                {QStringLiteral("division"), pattern.division},
                {QStringLiteral("chords"), musicalStepsToJson(pattern.chords)},
                {QStringLiteral("melody"), musicalStepsToJson(pattern.melody)},
                {QStringLiteral("bass"), musicalStepsToJson(pattern.bass)},
                {QStringLiteral("support"), musicalStepsToJson(pattern.support)},
            });
        }
        QJsonObject sectionObject{
            {QStringLiteral("id"), section.id},
            {QStringLiteral("label"), section.label},
            {QStringLiteral("name"), section.name},
            {QStringLiteral("beats"), section.beats},
            {QStringLiteral("chords"), chords},
            {QStringLiteral("targets"), targets},
            {QStringLiteral("beat_notes"), beatNotes},
            {QStringLiteral("lyrics"), lyrics},
            {QStringLiteral("beat_patterns"), beatPatterns},
            {QStringLiteral("musical_patterns"), musicalPatterns},
            {QStringLiteral("drum_kit"), section.drumKitId},
            {QStringLiteral("generated_kind"), section.generatedKind},
        };
        if (!section.generatedKind.isEmpty()) {
            sectionObject.insert(QStringLiteral("generated_recipe"),
                jam2::practice::generationRecipeToJson(section.generatedRecipe));
        }
        sections.append(sectionObject);
    }
    return QJsonObject{
        {QStringLiteral("beat_lane_schema"), kCurrentBeatLaneSchema},
        {QStringLiteral("title"), title_},
        {QStringLiteral("guitar_strings"), guitarStringCount_},
        {QStringLiteral("guitar_drop_tuning"), guitarDropTuning_},
        {QStringLiteral("sections"), sections},
    };
}

bool BeatGridModel::loadJson(const QJsonObject& object)
{
    const QStringList currentBeatLanes = beatLaneNames();
    const QJsonValue schemaValue = object.value(
        QStringLiteral("beat_lane_schema"));
    if (!exactInteger(
            schemaValue,
            kCurrentBeatLaneSchema,
            kCurrentBeatLaneSchema)) {
        return false;
    }
    if (!object.value(QStringLiteral("sections")).isArray() ||
        (!object.value(QStringLiteral("title")).isUndefined() && !object.value(QStringLiteral("title")).isString()) ||
        (!object.value(QStringLiteral("guitar_strings")).isUndefined() &&
            !exactInteger(object.value(QStringLiteral("guitar_strings")), 6, 8)) ||
        (!object.value(QStringLiteral("guitar_drop_tuning")).isUndefined() &&
            !object.value(QStringLiteral("guitar_drop_tuning")).isBool())) {
        return false;
    }
    const int loadedGuitarStrings = object.value(QStringLiteral("guitar_strings")).toInt(6);
    if (loadedGuitarStrings != 6 && loadedGuitarStrings != 7 && loadedGuitarStrings != 8) return false;
    QVector<SongSection> loaded;
    const QJsonArray sections = object.value(QStringLiteral("sections")).toArray();
    if (sections.isEmpty() || sections.size() > kMaxSections ||
        object.value(QStringLiteral("title")).toString().size() > kMaxTitleCharacters) {
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
            (!item.value(QStringLiteral("drum_kit")).isUndefined() && !item.value(QStringLiteral("drum_kit")).isString()) ||
            (!item.value(QStringLiteral("generated_kind")).isUndefined() && !item.value(QStringLiteral("generated_kind")).isString()) ||
            (!item.value(QStringLiteral("beats")).isUndefined() && !item.value(QStringLiteral("beats")).isDouble()) ||
            (!item.value(QStringLiteral("generated_recipe")).isUndefined() &&
                !item.value(QStringLiteral("generated_recipe")).isObject())) {
            return false;
        }
        SongSection section;
        section.id = sectionId(item.value(QStringLiteral("id")).toString());
        section.label = item.value(QStringLiteral("label")).toString(QStringLiteral("A"));
        section.name = item.value(QStringLiteral("name")).toString(QStringLiteral("Section"));
        section.beats = item.value(QStringLiteral("beats")).toInt(8);
        section.drumKitId = item.value(QStringLiteral("drum_kit")).toString(
            QStringLiteral("acoustic"));
        if (section.drumKitId != QStringLiteral("acoustic") &&
            section.drumKitId != QStringLiteral("electronic")) {
            return false;
        }
        section.generatedKind = item.value(QStringLiteral("generated_kind")).toString();
        if (!section.generatedKind.isEmpty() &&
            (!item.value(QStringLiteral("generated_recipe")).isObject() ||
             !jam2::practice::generationRecipeFromJson(
                 item.value(QStringLiteral("generated_recipe")).toObject(), section.generatedRecipe))) {
            return false;
        }
        const double beatsValue = item.value(QStringLiteral("beats")).toDouble(8.0);
        if (!std::isfinite(beatsValue) || std::floor(beatsValue) != beatsValue ||
            section.label.size() > kMaxCellCharacters || section.name.size() > kMaxCellCharacters ||
            section.id.size() > kMaxCellCharacters || section.generatedKind.size() > kMaxCellCharacters ||
            section.beats < kMinBeatsPerSection || section.beats > kMaxBeatsPerSection) {
            return false;
        }
        normalize(section);

        const QJsonArray chords = item.value(QStringLiteral("chords")).toArray();
        const QJsonArray targets = item.value(QStringLiteral("targets")).toArray();
        const QJsonArray beatNotes = item.value(QStringLiteral("beat_notes")).toArray();
        const QJsonArray lyrics = item.value(QStringLiteral("lyrics")).toArray();
        const QJsonArray beatPatterns = item.value(QStringLiteral("beat_patterns")).toArray();
        const QJsonArray musicalPatterns = item.value(QStringLiteral("musical_patterns")).toArray();
        for (const QString& key : {
                QStringLiteral("chords"), QStringLiteral("targets"), QStringLiteral("beat_notes"),
                QStringLiteral("lyrics"), QStringLiteral("beat_patterns"),
                QStringLiteral("musical_patterns")}) {
            const QJsonValue array = item.value(key);
            if (!array.isUndefined() && !array.isArray()) {
                return false;
            }
        }
        if (!validTextArray(chords, section.beats) || !validTextArray(targets, section.beats) ||
            !validTextArray(beatNotes, section.beats) ||
            !validTextArray(lyrics, section.beats) || beatPatterns.size() > section.beats ||
            musicalPatterns.size() > section.beats) {
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
            if (!validTextArray(lanes, currentBeatLanes.size())) {
                return false;
            }
            for (int lane = 0; lane < lanes.size(); ++lane) {
                section.beatPatterns[i].lanes[lane] =
                    lanes[lane].toString();
            }
        }
        if (!musicalPatterns.isEmpty()) {
            section.musicalPatterns.clear();
            section.musicalPatterns.resize(section.beats);
            for (int i = 0; i < musicalPatterns.size(); ++i) {
                if (!musicalPatterns[i].isObject()) return false;
                const QJsonObject pattern = musicalPatterns[i].toObject();
                if (!pattern.value(QStringLiteral("division")).isDouble()) return false;
                const double rawDivision = pattern.value(QStringLiteral("division")).toDouble();
                if (!std::isfinite(rawDivision) || rawDivision != std::floor(rawDivision) ||
                    !musicalDivisionValues().contains(static_cast<int>(rawDivision))) return false;
                MusicalBeatPattern parsed;
                parsed.division = static_cast<int>(rawDivision);
                if (!musicalStepsFromJson(pattern.value(QStringLiteral("chords")), parsed.division, parsed.chords) ||
                    !musicalStepsFromJson(pattern.value(QStringLiteral("melody")), parsed.division, parsed.melody)) return false;
                if (pattern.value(QStringLiteral("bass")).isUndefined()) {
                    parsed.bass.fill(MusicalStep{}, parsed.division);
                } else if (!musicalStepsFromJson(
                        pattern.value(QStringLiteral("bass")), parsed.division, parsed.bass)) {
                    return false;
                }
                if (pattern.value(QStringLiteral("support")).isUndefined()) {
                    parsed.support.fill(MusicalStep{}, parsed.division);
                } else if (!musicalStepsFromJson(
                        pattern.value(QStringLiteral("support")), parsed.division, parsed.support)) {
                    return false;
                }
                section.musicalPatterns[i] = std::move(parsed);
            }
        } else {
            section.musicalPatterns.clear();
        }
        normalize(section);
        loaded.push_back(section);
    }
    if (loaded.isEmpty()) return false;
    while (loaded.size() < kMaxSections) {
        SongSection section = defaultSection(loaded.size(), loaded.front().beats);
        normalize(section);
        loaded.push_back(std::move(section));
    }
    title_ = object.value(QStringLiteral("title")).toString(QStringLiteral("Untitled Jam"));
    guitarStringCount_ = loadedGuitarStrings;
    guitarDropTuning_ = object.value(QStringLiteral("guitar_drop_tuning")).toBool(false);
    sections_ = loaded;
    ++revision_;
    return true;
}

int BeatGridModel::beatLaneSchemaVersion()
{
    return kCurrentBeatLaneSchema;
}

QStringList BeatGridModel::beatLaneNames()
{
    return QStringList{
        QStringLiteral("Kick"),
        QStringLiteral("Snare"),
        QStringLiteral("Closed HH"),
        QStringLiteral("Open HH"),
        QStringLiteral("Ride"),
        QStringLiteral("Crash"),
        QStringLiteral("High Tom"),
        QStringLiteral("Mid Tom"),
        QStringLiteral("Floor Tom"),
        QStringLiteral("Cross-stick / Rim"),
    };
}

QStringList BeatGridModel::beatVisualLaneNames()
{
    QStringList lanes = beatLaneNames();
    std::reverse(lanes.begin(), lanes.end());
    return lanes;
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

QList<int> BeatGridModel::musicalDivisionValues()
{
    return QList<int>{1, 2, 4, 3, 6};
}

QString BeatGridModel::musicalDivisionLabel(int division)
{
    return beatDivisionLabel(normalizedMusicalDivision(division));
}

int BeatGridModel::normalizedMusicalDivision(int division)
{
    return musicalDivisionValues().contains(division) ? division : 1;
}

void BeatGridModel::normalize(SongSection& section)
{
    const bool deriveMusicalPatterns = section.musicalPatterns.isEmpty();
    section.id = sectionId(section.id);
    if (section.drumKitId != QStringLiteral("acoustic") &&
        section.drumKitId != QStringLiteral("electronic")) {
        section.drumKitId = QStringLiteral("acoustic");
    }
    section.beats = qBound(kMinBeatsPerSection, section.beats, kMaxBeatsPerSection);
    section.chords.resize(section.beats);
    section.targets.resize(section.beats);
    section.beatNotes.resize(section.beats);
    section.lyrics.resize(section.beats);
    section.beatPatterns.resize(section.beats);
    section.musicalPatterns.resize(section.beats);
    const int laneCount = beatLaneNames().size();
    for (BeatPattern& pattern : section.beatPatterns) {
        pattern.division = normalizedDivision(pattern.division);
        pattern.lanes.resize(laneCount);
    }
    for (int beat = 0; beat < section.musicalPatterns.size(); ++beat) {
        MusicalBeatPattern& pattern = section.musicalPatterns[beat];
        if (deriveMusicalPatterns) {
            pattern.division = 1;
            pattern.chords = {stepFromLegacy(section.chords[beat])};
            pattern.melody = {stepFromLegacy(section.targets[beat])};
            pattern.bass = {MusicalStep{}};
            pattern.support = {MusicalStep{}};
        } else {
            pattern.division = normalizedMusicalDivision(pattern.division);
            pattern.chords.resize(pattern.division);
            pattern.melody.resize(pattern.division);
            pattern.bass.resize(pattern.division);
            pattern.support.resize(pattern.division);
            section.targets[beat] = legacyFromStep(pattern.melody.front());
        }
    }
}

QString BeatGridModel::sectionId(const QString& value)
{
    return value.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : value.left(kMaxCellCharacters);
}
