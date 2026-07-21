#include "GenerationRecipe.hpp"

#include <QJsonArray>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace jam2::practice {
namespace {

constexpr int kMaximumId = 64;
constexpr int kMaximumName = 96;
constexpr int kMaximumExplanation = 256;
constexpr int kMaximumHarmonyEvents = 128;
constexpr int kMaximumTheoryDecisions = 16;
constexpr int kMaximumGrooveDecisions = 16;
constexpr int kMaximumMotifTransforms = 8;
constexpr int kMaximumPatchModifiers = 8;
constexpr int kMaximumMelodyEvents = 256;
constexpr int kMaximumMelodyPhrases = 8;

bool bounded(const QString& text, int maximum)
{
    return text.size() <= maximum;
}

bool boundedStrings(const QStringList& values, int count, int length)
{
    if (values.size() > count) return false;
    return std::all_of(values.cbegin(), values.cend(),
        [length](const QString& value) { return bounded(value, length); });
}

QJsonArray stringsToJson(const QStringList& values)
{
    QJsonArray result;
    for (const QString& value : values) result.append(value);
    return result;
}

bool stringsFromJson(
    const QJsonValue& value,
    QStringList& result,
    int maximumCount,
    int maximumLength)
{
    if (!value.isArray() || value.toArray().size() > maximumCount) return false;
    QStringList next;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString() || !bounded(item.toString(), maximumLength)) return false;
        next.push_back(item.toString());
    }
    result = std::move(next);
    return true;
}

bool exactInteger(const QJsonValue& value, int minimum, int maximum)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return number >= minimum && number <= maximum && number == static_cast<int>(number);
}

bool exactUnsigned32(const QJsonValue& value)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return number >= 0.0 && number <= 4294967295.0 && number == std::floor(number);
}

QString heading(const QString& value)
{
    return value.isEmpty() ? QStringLiteral("—") : value;
}

bool validMelodyEvents(const QVector<MelodyRecipeEvent>& events)
{
    if (events.size() > kMaximumMelodyEvents) return false;
    return std::all_of(events.cbegin(), events.cend(), [](const MelodyRecipeEvent& event) {
        return event.tick >= 0 && event.tick <= 8192 && event.durationTicks >= 1 &&
            event.durationTicks <= 8192 && event.midi >= 0 && event.midi <= 127 &&
            event.velocity >= 1 && event.velocity <= 127 && bounded(event.note, kMaximumName) &&
            bounded(event.chord, kMaximumName) && bounded(event.chordRole, kMaximumName) &&
            bounded(event.melodicRole, kMaximumName);
    });
}

bool validMelodyPhrases(const QVector<MelodyPhraseRecipe>& phrases)
{
    if (phrases.size() > kMaximumMelodyPhrases) return false;
    return std::all_of(phrases.cbegin(), phrases.cend(), [](const MelodyPhraseRecipe& phrase) {
        return phrase.startBar >= 1 && phrase.endBar >= phrase.startBar && phrase.endBar <= 16 &&
            bounded(phrase.label, kMaximumName) && bounded(phrase.summary, kMaximumExplanation);
    });
}

} // namespace

bool GenerationRecipe::isValid() const
{
    return generatorVersion == 4 && bars >= 4 && bars <= 16 &&
        beatsPerBar >= 1 && beatsPerBar <= 12 && bpm >= 20 && bpm <= 400 &&
        complexity >= 1 && complexity <= 8 && !styleId.isEmpty() && !moodId.isEmpty() &&
        !tonic.isEmpty() && !mode.isEmpty() && !progressionId.isEmpty() && !grooveId.isEmpty() &&
        !grooveFeelName.isEmpty() && swingPercent >= 50 && swingPercent <= 67 &&
        snareOffsetMs >= -20 && snareOffsetMs <= 25 && timingVariationMs >= 0 &&
        timingVariationMs <= 5 && velocityVariationPercent >= 0 && velocityVariationPercent <= 12 &&
        kickVariationCount >= 0 && kickVariationCount <= 64 &&
        ghostVariationCount >= 0 && ghostVariationCount <= 64 &&
        cymbalVariationCount >= 0 && cymbalVariationCount <= 64 &&
        fillCount >= 0 && fillCount <= 64 && advancedCellCount >= 0 && advancedCellCount <= 8 &&
        bounded(styleId, kMaximumId) && bounded(moodId, kMaximumId) &&
        bounded(progressionId, kMaximumId) && bounded(grooveId, kMaximumId) &&
        bounded(chordPatchId, kMaximumId) && bounded(melodyPatchId, kMaximumId) &&
        bounded(drumPatchId, kMaximumId) && bounded(styleName, kMaximumName) &&
        bounded(moodName, kMaximumName) && bounded(progressionName, kMaximumName) &&
        bounded(grooveName, kMaximumName) && bounded(grooveFeelName, kMaximumName) &&
        baseHarmony.size() <= kMaximumHarmonyEvents &&
        finalChordPlan.size() <= kMaximumHarmonyEvents &&
        theoryDecisions.size() <= kMaximumTheoryDecisions &&
        boundedStrings(moodDecisions, kMaximumGrooveDecisions, kMaximumExplanation) &&
        boundedStrings(motifTransformations, kMaximumMotifTransforms, kMaximumExplanation) &&
        validMelodyEvents(melodyEvents) && validMelodyPhrases(melodyPhrases) &&
        bounded(melodyRange, kMaximumName) &&
        boundedStrings(grooveDecisions, kMaximumGrooveDecisions, kMaximumExplanation) &&
        boundedStrings(patchModifiers, kMaximumPatchModifiers, kMaximumExplanation);
}

QJsonObject generationRecipeToJson(const GenerationRecipe& recipe)
{
    QJsonArray base;
    for (const HarmonicRecipeEvent& event : recipe.baseHarmony) {
        base.append(QJsonObject{
            {QStringLiteral("beat"), event.beat},
            {QStringLiteral("duration_beats"), event.durationBeats},
            {QStringLiteral("roman"), event.roman},
            {QStringLiteral("chord"), event.chord},
        });
    }
    QJsonArray theory;
    for (const TheoryDecision& decision : recipe.theoryDecisions) {
        theory.append(QJsonObject{
            {QStringLiteral("beat"), decision.beat},
            {QStringLiteral("kind"), decision.kind},
            {QStringLiteral("before"), decision.beforeChord},
            {QStringLiteral("after"), decision.afterChord},
            {QStringLiteral("analysis"), decision.analysis},
            {QStringLiteral("resolution_target"), decision.resolutionTarget},
            {QStringLiteral("explanation"), decision.explanation},
        });
    }
    QJsonArray melodyEvents;
    for (const MelodyRecipeEvent& event : recipe.melodyEvents) {
        melodyEvents.append(QJsonObject{
            {QStringLiteral("tick"), event.tick},
            {QStringLiteral("duration_ticks"), event.durationTicks},
            {QStringLiteral("midi"), event.midi},
            {QStringLiteral("velocity"), event.velocity},
            {QStringLiteral("note"), event.note},
            {QStringLiteral("chord"), event.chord},
            {QStringLiteral("chord_role"), event.chordRole},
            {QStringLiteral("melodic_role"), event.melodicRole},
        });
    }
    QJsonArray melodyPhrases;
    for (const MelodyPhraseRecipe& phrase : recipe.melodyPhrases) {
        melodyPhrases.append(QJsonObject{
            {QStringLiteral("start_bar"), phrase.startBar},
            {QStringLiteral("end_bar"), phrase.endBar},
            {QStringLiteral("label"), phrase.label},
            {QStringLiteral("summary"), phrase.summary},
        });
    }
    return {
        {QStringLiteral("generator_version"), recipe.generatorVersion},
        {QStringLiteral("seed"), static_cast<qint64>(recipe.seed)},
        {QStringLiteral("style"), QJsonObject{{QStringLiteral("id"), recipe.styleId}, {QStringLiteral("name"), recipe.styleName}}},
        {QStringLiteral("mood"), QJsonObject{{QStringLiteral("id"), recipe.moodId}, {QStringLiteral("name"), recipe.moodName}}},
        {QStringLiteral("tonic"), recipe.tonic},
        {QStringLiteral("mode"), recipe.mode},
        {QStringLiteral("mood_decisions"), stringsToJson(recipe.moodDecisions)},
        {QStringLiteral("bpm"), recipe.bpm},
        {QStringLiteral("beats_per_bar"), recipe.beatsPerBar},
        {QStringLiteral("bars"), recipe.bars},
        {QStringLiteral("complexity"), recipe.complexity},
        {QStringLiteral("progression"), QJsonObject{
            {QStringLiteral("id"), recipe.progressionId},
            {QStringLiteral("name"), recipe.progressionName},
            {QStringLiteral("base_harmony"), base},
            {QStringLiteral("final_chord_plan"), stringsToJson(recipe.finalChordPlan)},
            {QStringLiteral("theory_decisions"), theory},
        }},
        {QStringLiteral("motif"), QJsonObject{
            {QStringLiteral("cell"), recipe.motifCell},
            {QStringLiteral("rhythm"), recipe.motifRhythm},
            {QStringLiteral("form"), recipe.motifForm},
            {QStringLiteral("transformations"), stringsToJson(recipe.motifTransformations)},
            {QStringLiteral("events"), melodyEvents},
            {QStringLiteral("phrases"), melodyPhrases},
            {QStringLiteral("range"), recipe.melodyRange},
        }},
        {QStringLiteral("groove"), QJsonObject{
            {QStringLiteral("id"), recipe.grooveId},
            {QStringLiteral("name"), recipe.grooveName},
            {QStringLiteral("core"), recipe.grooveCore},
            {QStringLiteral("feel_name"), recipe.grooveFeelName},
            {QStringLiteral("swing_percent"), recipe.swingPercent},
            {QStringLiteral("snare_offset_ms"), recipe.snareOffsetMs},
            {QStringLiteral("timing_variation_ms"), recipe.timingVariationMs},
            {QStringLiteral("velocity_variation_percent"), recipe.velocityVariationPercent},
            {QStringLiteral("variation_counts"), QJsonObject{
                {QStringLiteral("kick"), recipe.kickVariationCount},
                {QStringLiteral("ghost"), recipe.ghostVariationCount},
                {QStringLiteral("cymbal"), recipe.cymbalVariationCount},
                {QStringLiteral("fill"), recipe.fillCount},
                {QStringLiteral("advanced_cells"), recipe.advancedCellCount},
            }},
            {QStringLiteral("decisions"), stringsToJson(recipe.grooveDecisions)},
        }},
        {QStringLiteral("patches"), QJsonObject{
            {QStringLiteral("chord_id"), recipe.chordPatchId},
            {QStringLiteral("chord_name"), recipe.chordPatchName},
            {QStringLiteral("melody_id"), recipe.melodyPatchId},
            {QStringLiteral("melody_name"), recipe.melodyPatchName},
            {QStringLiteral("drum_id"), recipe.drumPatchId},
            {QStringLiteral("drum_name"), recipe.drumPatchName},
            {QStringLiteral("modifiers"), stringsToJson(recipe.patchModifiers)},
        }},
        {QStringLiteral("fingerprints"), QJsonObject{
            {QStringLiteral("chord"), recipe.chordFingerprint},
            {QStringLiteral("beat"), recipe.beatFingerprint},
        }},
    };
}

bool generationRecipeFromJson(const QJsonObject& object, GenerationRecipe& recipe)
{
    if (!exactInteger(object.value(QStringLiteral("generator_version")), 4, 4) ||
        !exactUnsigned32(object.value(QStringLiteral("seed"))) ||
        !exactInteger(object.value(QStringLiteral("bpm")), 20, 400) ||
        !exactInteger(object.value(QStringLiteral("beats_per_bar")), 1, 12) ||
        !exactInteger(object.value(QStringLiteral("bars")), 4, 16) ||
        !exactInteger(object.value(QStringLiteral("complexity")), 1, 8) ||
        !object.value(QStringLiteral("style")).isObject() ||
        !object.value(QStringLiteral("mood")).isObject() ||
        !object.value(QStringLiteral("progression")).isObject() ||
        !object.value(QStringLiteral("motif")).isObject() ||
        !object.value(QStringLiteral("groove")).isObject() ||
        !object.value(QStringLiteral("patches")).isObject() ||
        !object.value(QStringLiteral("fingerprints")).isObject()) {
        return false;
    }
    GenerationRecipe next;
    next.generatorVersion = 4;
    next.seed = static_cast<std::uint32_t>(object.value(QStringLiteral("seed")).toInteger());
    next.bpm = object.value(QStringLiteral("bpm")).toInt();
    next.beatsPerBar = object.value(QStringLiteral("beats_per_bar")).toInt();
    next.bars = object.value(QStringLiteral("bars")).toInt();
    next.complexity = object.value(QStringLiteral("complexity")).toInt();
    const QJsonObject style = object.value(QStringLiteral("style")).toObject();
    const QJsonObject mood = object.value(QStringLiteral("mood")).toObject();
    next.styleId = style.value(QStringLiteral("id")).toString();
    next.styleName = style.value(QStringLiteral("name")).toString();
    next.moodId = mood.value(QStringLiteral("id")).toString();
    next.moodName = mood.value(QStringLiteral("name")).toString();
    next.tonic = object.value(QStringLiteral("tonic")).toString();
    next.mode = object.value(QStringLiteral("mode")).toString();
    if (!stringsFromJson(object.value(QStringLiteral("mood_decisions")), next.moodDecisions,
            kMaximumGrooveDecisions, kMaximumExplanation)) return false;
    const QJsonObject progression = object.value(QStringLiteral("progression")).toObject();
    next.progressionId = progression.value(QStringLiteral("id")).toString();
    next.progressionName = progression.value(QStringLiteral("name")).toString();
    const QJsonValue baseValue = progression.value(QStringLiteral("base_harmony"));
    if (!baseValue.isArray() || baseValue.toArray().size() > kMaximumHarmonyEvents) return false;
    for (const QJsonValue& value : baseValue.toArray()) {
        if (!value.isObject()) return false;
        const QJsonObject event = value.toObject();
        if (!exactInteger(event.value(QStringLiteral("beat")), 0, 4096) ||
            !exactInteger(event.value(QStringLiteral("duration_beats")), 1, 4096)) return false;
        HarmonicRecipeEvent parsed;
        parsed.beat = event.value(QStringLiteral("beat")).toInt();
        parsed.durationBeats = event.value(QStringLiteral("duration_beats")).toInt();
        parsed.roman = event.value(QStringLiteral("roman")).toString();
        parsed.chord = event.value(QStringLiteral("chord")).toString();
        if (!bounded(parsed.roman, kMaximumName) || !bounded(parsed.chord, kMaximumName)) return false;
        next.baseHarmony.push_back(std::move(parsed));
    }
    if (!stringsFromJson(progression.value(QStringLiteral("final_chord_plan")),
            next.finalChordPlan, kMaximumHarmonyEvents, kMaximumName)) return false;
    const QJsonValue theoryValue = progression.value(QStringLiteral("theory_decisions"));
    if (!theoryValue.isArray() || theoryValue.toArray().size() > kMaximumTheoryDecisions) return false;
    for (const QJsonValue& value : theoryValue.toArray()) {
        if (!value.isObject()) return false;
        const QJsonObject item = value.toObject();
        if (!exactInteger(item.value(QStringLiteral("beat")), 0, 4096)) return false;
        TheoryDecision decision;
        decision.beat = item.value(QStringLiteral("beat")).toInt();
        decision.kind = item.value(QStringLiteral("kind")).toString();
        decision.beforeChord = item.value(QStringLiteral("before")).toString();
        decision.afterChord = item.value(QStringLiteral("after")).toString();
        decision.analysis = item.value(QStringLiteral("analysis")).toString();
        decision.resolutionTarget = item.value(QStringLiteral("resolution_target")).toString();
        decision.explanation = item.value(QStringLiteral("explanation")).toString();
        if (!bounded(decision.kind, kMaximumId) || !bounded(decision.beforeChord, kMaximumName) ||
            !bounded(decision.afterChord, kMaximumName) || !bounded(decision.analysis, kMaximumName) ||
            !bounded(decision.resolutionTarget, kMaximumName) ||
            !bounded(decision.explanation, kMaximumExplanation)) return false;
        next.theoryDecisions.push_back(std::move(decision));
    }
    const QJsonObject motif = object.value(QStringLiteral("motif")).toObject();
    next.motifCell = motif.value(QStringLiteral("cell")).toString();
    next.motifRhythm = motif.value(QStringLiteral("rhythm")).toString();
    next.motifForm = motif.value(QStringLiteral("form")).toString();
    if (!stringsFromJson(motif.value(QStringLiteral("transformations")), next.motifTransformations,
            kMaximumMotifTransforms, kMaximumExplanation)) return false;
    const QJsonValue melodyEventsValue = motif.value(QStringLiteral("events"));
    if (!melodyEventsValue.isArray() || melodyEventsValue.toArray().size() > kMaximumMelodyEvents) return false;
    for (const QJsonValue& value : melodyEventsValue.toArray()) {
        if (!value.isObject()) return false;
        const QJsonObject item = value.toObject();
        if (!exactInteger(item.value(QStringLiteral("tick")), 0, 8192) ||
            !exactInteger(item.value(QStringLiteral("duration_ticks")), 1, 8192) ||
            !exactInteger(item.value(QStringLiteral("midi")), 0, 127) ||
            !exactInteger(item.value(QStringLiteral("velocity")), 1, 127)) return false;
        MelodyRecipeEvent event;
        event.tick = item.value(QStringLiteral("tick")).toInt();
        event.durationTicks = item.value(QStringLiteral("duration_ticks")).toInt();
        event.midi = item.value(QStringLiteral("midi")).toInt();
        event.velocity = item.value(QStringLiteral("velocity")).toInt();
        event.note = item.value(QStringLiteral("note")).toString();
        event.chord = item.value(QStringLiteral("chord")).toString();
        event.chordRole = item.value(QStringLiteral("chord_role")).toString();
        event.melodicRole = item.value(QStringLiteral("melodic_role")).toString();
        next.melodyEvents.push_back(std::move(event));
    }
    const QJsonValue phrasesValue = motif.value(QStringLiteral("phrases"));
    if (!phrasesValue.isArray() || phrasesValue.toArray().size() > kMaximumMelodyPhrases) return false;
    for (const QJsonValue& value : phrasesValue.toArray()) {
        if (!value.isObject()) return false;
        const QJsonObject item = value.toObject();
        if (!exactInteger(item.value(QStringLiteral("start_bar")), 1, 16) ||
            !exactInteger(item.value(QStringLiteral("end_bar")), 1, 16)) return false;
        MelodyPhraseRecipe phrase;
        phrase.startBar = item.value(QStringLiteral("start_bar")).toInt();
        phrase.endBar = item.value(QStringLiteral("end_bar")).toInt();
        phrase.label = item.value(QStringLiteral("label")).toString();
        phrase.summary = item.value(QStringLiteral("summary")).toString();
        next.melodyPhrases.push_back(std::move(phrase));
    }
    next.melodyRange = motif.value(QStringLiteral("range")).toString();
    const QJsonObject groove = object.value(QStringLiteral("groove")).toObject();
    next.grooveId = groove.value(QStringLiteral("id")).toString();
    next.grooveName = groove.value(QStringLiteral("name")).toString();
    next.grooveCore = groove.value(QStringLiteral("core")).toString();
    next.grooveFeelName = groove.value(QStringLiteral("feel_name")).toString();
    if (!exactInteger(groove.value(QStringLiteral("swing_percent")), 50, 67) ||
        !exactInteger(groove.value(QStringLiteral("snare_offset_ms")), -20, 25) ||
        !exactInteger(groove.value(QStringLiteral("timing_variation_ms")), 0, 5) ||
        !exactInteger(groove.value(QStringLiteral("velocity_variation_percent")), 0, 12)) return false;
    next.swingPercent = groove.value(QStringLiteral("swing_percent")).toInt();
    next.snareOffsetMs = groove.value(QStringLiteral("snare_offset_ms")).toInt();
    next.timingVariationMs = groove.value(QStringLiteral("timing_variation_ms")).toInt();
    next.velocityVariationPercent = groove.value(QStringLiteral("velocity_variation_percent")).toInt();
    const QJsonObject variationCounts = groove.value(QStringLiteral("variation_counts")).toObject();
    if (!groove.value(QStringLiteral("variation_counts")).isObject() ||
        !exactInteger(variationCounts.value(QStringLiteral("kick")), 0, 64) ||
        !exactInteger(variationCounts.value(QStringLiteral("ghost")), 0, 64) ||
        !exactInteger(variationCounts.value(QStringLiteral("cymbal")), 0, 64) ||
        !exactInteger(variationCounts.value(QStringLiteral("fill")), 0, 64) ||
        !exactInteger(variationCounts.value(QStringLiteral("advanced_cells")), 0, 8)) return false;
    next.kickVariationCount = variationCounts.value(QStringLiteral("kick")).toInt();
    next.ghostVariationCount = variationCounts.value(QStringLiteral("ghost")).toInt();
    next.cymbalVariationCount = variationCounts.value(QStringLiteral("cymbal")).toInt();
    next.fillCount = variationCounts.value(QStringLiteral("fill")).toInt();
    next.advancedCellCount = variationCounts.value(QStringLiteral("advanced_cells")).toInt();
    if (!stringsFromJson(groove.value(QStringLiteral("decisions")), next.grooveDecisions,
            kMaximumGrooveDecisions, kMaximumExplanation)) return false;
    const QJsonObject patches = object.value(QStringLiteral("patches")).toObject();
    next.chordPatchId = patches.value(QStringLiteral("chord_id")).toString();
    next.chordPatchName = patches.value(QStringLiteral("chord_name")).toString();
    next.melodyPatchId = patches.value(QStringLiteral("melody_id")).toString();
    next.melodyPatchName = patches.value(QStringLiteral("melody_name")).toString();
    next.drumPatchId = patches.value(QStringLiteral("drum_id")).toString();
    next.drumPatchName = patches.value(QStringLiteral("drum_name")).toString();
    if (!stringsFromJson(patches.value(QStringLiteral("modifiers")), next.patchModifiers,
            kMaximumPatchModifiers, kMaximumExplanation)) return false;
    const QJsonObject fingerprints = object.value(QStringLiteral("fingerprints")).toObject();
    next.chordFingerprint = fingerprints.value(QStringLiteral("chord")).toString();
    next.beatFingerprint = fingerprints.value(QStringLiteral("beat")).toString();
    if (!next.isValid()) return false;
    recipe = std::move(next);
    return true;
}

QString generationRecipeDetails(const GenerationRecipe& recipe, bool contentChanged)
{
    QStringList lines;
    lines << QStringLiteral("%1 %2 — %3 — %4")
        .arg(recipe.tonic, recipe.mode, recipe.styleName, recipe.moodName);
    lines << QStringLiteral("Seed: %1 (0x%2)    Generator: v%3")
        .arg(recipe.seed).arg(recipe.seed, 8, 16, QLatin1Char('0')).arg(recipe.generatorVersion);
    lines << QStringLiteral("Meter: %1/4    Tempo: %2 BPM    Length: %3 bars    Complexity: %4")
        .arg(recipe.beatsPerBar).arg(recipe.bpm).arg(recipe.bars).arg(recipe.complexity);
    if (contentChanged) {
        lines << QString() << QStringLiteral("Warning: the current grid differs from the generated output stored in this recipe.");
    }
    lines << QString() << QStringLiteral("MOOD SHAPING");
    for (const QString& decision : recipe.moodDecisions) lines << QStringLiteral("  %1").arg(decision);
    lines << QString() << QStringLiteral("HARMONY")
          << QStringLiteral("Base family: %1").arg(heading(recipe.progressionName));
    for (const HarmonicRecipeEvent& event : recipe.baseHarmony) {
        lines << QStringLiteral("Bar %1, beat %2: %3 → %4")
            .arg(event.beat / recipe.beatsPerBar + 1)
            .arg(event.beat % recipe.beatsPerBar + 1)
            .arg(event.roman, event.chord);
    }
    lines << QStringLiteral("Final plan: %1").arg(recipe.finalChordPlan.join(QStringLiteral(" | ")));
    if (recipe.theoryDecisions.isEmpty()) {
        lines << QStringLiteral("Theory changes: none; the grounding progression was kept intact.");
    } else {
        lines << QStringLiteral("Theory changes:");
        for (const TheoryDecision& decision : recipe.theoryDecisions) {
            lines << QStringLiteral("  Bar %1, beat %2 — %3 → %4; %5. %6")
                .arg(decision.beat / recipe.beatsPerBar + 1)
                .arg(decision.beat % recipe.beatsPerBar + 1)
                .arg(decision.beforeChord, decision.afterChord,
                    decision.analysis, decision.explanation);
        }
    }
    lines << QString() << QStringLiteral("MOTIF")
          << QStringLiteral("Cell: %1").arg(heading(recipe.motifCell))
          << QStringLiteral("Rhythm: %1").arg(heading(recipe.motifRhythm))
          << QStringLiteral("Form: %1").arg(heading(recipe.motifForm));
    for (const QString& transformation : recipe.motifTransformations) {
        lines << QStringLiteral("  %1").arg(transformation);
    }
    lines << QStringLiteral("Range: %1").arg(heading(recipe.melodyRange));
    for (const MelodyPhraseRecipe& phrase : recipe.melodyPhrases) {
        lines << QStringLiteral("  %1 (bars %2–%3): %4")
            .arg(phrase.label).arg(phrase.startBar).arg(phrase.endBar).arg(phrase.summary);
    }
    lines << QStringLiteral("Complexity palette: levels 1-%1 were available; complexity broadens the theory choices rather than merely adding notes.")
        .arg(recipe.complexity);
    if (!recipe.melodyEvents.isEmpty()) {
        lines << QStringLiteral("Note choices:");
        for (const MelodyRecipeEvent& event : recipe.melodyEvents) {
            const int beat = event.tick / 12;
            lines << QStringLiteral("  Bar %1, beat %2 + %3/12: %4 over %5 - %6; %7")
                .arg(beat / recipe.beatsPerBar + 1)
                .arg(beat % recipe.beatsPerBar + 1)
                .arg(event.tick % 12)
                .arg(event.note, event.chord, event.chordRole, event.melodicRole);
        }
    }
    lines << QString() << QStringLiteral("GROOVE")
          << QStringLiteral("Family: %1").arg(heading(recipe.grooveName))
          << QStringLiteral("Core: %1").arg(heading(recipe.grooveCore))
          << QStringLiteral("Feel: %1    Swing: %2%    Snare offset: %3 ms")
                .arg(heading(recipe.grooveFeelName)).arg(recipe.swingPercent).arg(recipe.snareOffsetMs)
          << QStringLiteral("Human variation: timing ±%1 ms    velocity ±%2%")
                .arg(recipe.timingVariationMs).arg(recipe.velocityVariationPercent)
          << QStringLiteral("Generated changes: kick %1    ghost %2    cymbal %3    fills %4    advanced cells %5")
                .arg(recipe.kickVariationCount).arg(recipe.ghostVariationCount)
                .arg(recipe.cymbalVariationCount).arg(recipe.fillCount).arg(recipe.advancedCellCount);
    for (const QString& decision : recipe.grooveDecisions) lines << QStringLiteral("  %1").arg(decision);
    lines << QString() << QStringLiteral("SOUND")
          << QStringLiteral("Chords: %1    Melody: %2    Drums: %3")
                .arg(recipe.chordPatchName, recipe.melodyPatchName, recipe.drumPatchName);
    for (const QString& modifier : recipe.patchModifiers) lines << QStringLiteral("  %1").arg(modifier);
    return lines.join(QLatin1Char('\n'));
}

} // namespace jam2::practice
