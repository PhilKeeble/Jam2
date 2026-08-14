#include "GenerationRecipe.hpp"

#include <QJsonArray>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

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
constexpr int kMaximumRoleEvents = 512;
constexpr int kMaximumDrumEvents = 8192;
constexpr int kMaximumDrumPhrases = 16;
constexpr int kMaximumTimingPolicies = 16;
constexpr int kMaximumComplexityTools = 64;
constexpr int kMaximumSynthVoices = 16;
constexpr int kMaximumAutomationEvents = 64;
constexpr int kMaximumFormSections = 16;

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
        return phrase.startBar >= 1 && phrase.endBar >= phrase.startBar && phrase.endBar <= 64 &&
            bounded(phrase.label, kMaximumName) && bounded(phrase.summary, kMaximumExplanation);
    });
}

bool validRoleEvents(const QVector<RoleRecipeEvent>& events)
{
    if (events.size() > kMaximumRoleEvents) return false;
    return std::all_of(events.cbegin(), events.cend(), [](const RoleRecipeEvent& event) {
        return event.tick >= 0 && event.tick <= 32768 && event.durationTicks >= 1 &&
            event.durationTicks <= 32768 && event.midi >= 0 && event.midi <= 127 &&
            event.velocity >= 1 && event.velocity <= 127 && bounded(event.note, kMaximumName) &&
            bounded(event.role, kMaximumId) && bounded(event.relationship, kMaximumExplanation) &&
            bounded(event.articulation, kMaximumName);
    });
}

bool validDrumEvents(
    const QVector<DrumPerformanceEvent>& events,
    int totalTicks)
{
    static const QStringList laneIds{
        QStringLiteral("kick"),
        QStringLiteral("snare"),
        QStringLiteral("closed_hat"),
        QStringLiteral("open_hat"),
        QStringLiteral("ride"),
        QStringLiteral("crash"),
        QStringLiteral("high_tom"),
        QStringLiteral("mid_tom"),
        QStringLiteral("floor_tom"),
        QStringLiteral("cross_stick"),
    };
    if (events.isEmpty() || events.size() > kMaximumDrumEvents) return false;
    int previousTick = -1;
    return std::all_of(
        events.cbegin(),
        events.cend(),
        [totalTicks, &previousTick](
            const DrumPerformanceEvent& event) {
            const bool valid =
                event.tick >= 0 && event.tick < totalTicks &&
                event.tick >= previousTick &&
                laneIds.contains(event.laneId) &&
                event.velocity >= 1 && event.velocity <= 127 &&
                event.offsetMs >= -40 && event.offsetMs <= 40 &&
                bounded(event.articulation, kMaximumName) &&
                bounded(event.role, kMaximumName) &&
                event.repeatGroup >= 0 && event.repeatGroup <= 4096;
            previousTick = event.tick;
            return valid;
        });
}

bool validDrumPhrases(
    const QVector<DrumPhraseRecipe>& phrases,
    int bars,
    int totalBeats)
{
    if (phrases.size() > kMaximumDrumPhrases) return false;
    return std::all_of(
        phrases.cbegin(),
        phrases.cend(),
        [bars, totalBeats](const DrumPhraseRecipe& phrase) {
            const bool noFill =
                phrase.fillStartBeat == -1 &&
                phrase.fillBeatCount == 0;
            const bool validFill =
                phrase.fillStartBeat >= 0 &&
                phrase.fillBeatCount >= 1 &&
                phrase.fillStartBeat + phrase.fillBeatCount <=
                    totalBeats;
            return phrase.startBar >= 1 &&
                phrase.endBar >= phrase.startBar &&
                phrase.endBar <= bars &&
                phrase.energy >= 0 && phrase.energy <= 3 &&
                bounded(phrase.label, kMaximumName) &&
                bounded(phrase.formRole, kMaximumName) &&
                bounded(phrase.development, kMaximumExplanation) &&
                bounded(phrase.transition, kMaximumExplanation) &&
                bounded(phrase.fillId, kMaximumId) &&
                (noFill || validFill);
        });
}

bool validTimingPolicies(const QVector<LaneTimingRecipe>& values)
{
    if (values.size() > kMaximumTimingPolicies) return false;
    return std::all_of(values.cbegin(), values.cend(), [](const LaneTimingRecipe& value) {
        return bounded(value.laneId, kMaximumId) && bounded(value.anchorGrid, kMaximumName) &&
            bounded(value.subdivisionMapping, kMaximumName) && value.offsetMs >= -50 &&
            value.offsetMs <= 50 && value.varianceMs >= 0 && value.varianceMs <= 20 &&
            bounded(value.velocityShape, kMaximumName);
    });
}

bool validComplexityTools(const QVector<ComplexityToolRecipe>& values)
{
    if (values.size() > kMaximumComplexityTools) return false;
    return std::all_of(values.cbegin(), values.cend(), [](const ComplexityToolRecipe& value) {
        return value.level >= 1 && value.level <= 8 && bounded(value.toolId, kMaximumId) &&
            bounded(value.name, kMaximumName) && bounded(value.explanation, kMaximumExplanation);
    });
}

bool finiteInRange(double value, double minimum, double maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool validSynthVoices(const QVector<SynthVoiceRecipe>& values)
{
    if (values.size() > kMaximumSynthVoices) return false;
    return std::all_of(values.cbegin(), values.cend(), [](const SynthVoiceRecipe& value) {
        return bounded(value.roleId, kMaximumId) && bounded(value.engine, kMaximumId) &&
            bounded(value.oscillator, kMaximumId) &&
            finiteInRange(value.attackMs, 0.0, 5000.0) &&
            finiteInRange(value.releaseMs, 1.0, 10000.0) &&
            finiteInRange(value.cutoffHz, 20.0, 24000.0) &&
            finiteInRange(value.resonance, 0.0, 1.0) &&
            finiteInRange(value.drive, 0.0, 1.0) &&
            finiteInRange(value.detuneCents, -100.0, 100.0) &&
            finiteInRange(value.noiseMix, 0.0, 1.0) &&
            boundedStrings(value.effects, 8, kMaximumName);
    });
}

bool validAutomationEvents(const QVector<AutomationRecipeEvent>& values)
{
    if (values.size() > kMaximumAutomationEvents) return false;
    return std::all_of(values.cbegin(), values.cend(), [](const AutomationRecipeEvent& value) {
        return value.startTick >= 0 && value.endTick > value.startTick &&
            value.endTick <= 32768 && bounded(value.target, kMaximumName) &&
            finiteInRange(value.startValue, -100000.0, 100000.0) &&
            finiteInRange(value.endValue, -100000.0, 100000.0) &&
            bounded(value.curve, kMaximumId) &&
            bounded(value.explanation, kMaximumExplanation);
    });
}

bool validFormSections(const QVector<FormSectionRecipe>& values, int totalBars)
{
    if (values.isEmpty() || values.size() > kMaximumFormSections) return false;
    return std::all_of(values.cbegin(), values.cend(),
        [totalBars](const FormSectionRecipe& value) {
            return value.startBar >= 1 && value.bars >= 1 &&
                value.startBar + value.bars - 1 <= totalBars &&
                bounded(value.label, kMaximumName) &&
                bounded(value.role, kMaximumName) &&
                bounded(value.relationship, kMaximumExplanation);
        });
}

QJsonArray integersToJson(const QVector<int>& values)
{
    QJsonArray result;
    for (int value : values) result.append(value);
    return result;
}

QJsonArray roleEventsToJson(const QVector<RoleRecipeEvent>& events)
{
    QJsonArray result;
    for (const RoleRecipeEvent& event : events) {
        result.append(QJsonObject{
            {QStringLiteral("tick"), event.tick},
            {QStringLiteral("duration_ticks"), event.durationTicks},
            {QStringLiteral("midi"), event.midi},
            {QStringLiteral("velocity"), event.velocity},
            {QStringLiteral("note"), event.note},
            {QStringLiteral("role"), event.role},
            {QStringLiteral("relationship"), event.relationship},
            {QStringLiteral("articulation"), event.articulation},
        });
    }
    return result;
}

} // namespace

bool GenerationRecipe::isValid() const
{
    return generatorVersion == 7 && bars >= 4 && bars <= 64 &&
        beatsPerBar >= 1 && beatsPerBar <= 12 && bpm >= 20 && bpm <= 400 &&
        complexity >= 1 && complexity <= 8 && !styleId.isEmpty() && !variationId.isEmpty() &&
        !tonic.isEmpty() && !mode.isEmpty() && !progressionId.isEmpty() && !grooveId.isEmpty() &&
        !grooveFeelName.isEmpty() && swingPercent >= 50 && swingPercent <= 67 &&
        snareOffsetMs >= -20 && snareOffsetMs <= 25 && timingVariationMs >= 0 &&
        timingVariationMs <= 5 && velocityVariationPercent >= 0 && velocityVariationPercent <= 12 &&
        kickVariationCount >= 0 && kickVariationCount <= 64 &&
        ghostVariationCount >= 0 && ghostVariationCount <= 64 &&
        cymbalVariationCount >= 0 && cymbalVariationCount <= 64 &&
        fillCount >= 0 && fillCount <= 64 && advancedCellCount >= 0 && advancedCellCount <= 8 &&
        bounded(styleId, kMaximumId) && bounded(variationId, kMaximumId) &&
        variationDensity >= -1 && variationDensity <= 1 &&
        variationRegister >= -1 && variationRegister <= 1 &&
        variationArticulation >= -1 && variationArticulation <= 1 &&
        variationBrightness >= -1 && variationBrightness <= 1 &&
        variationSpace >= -1 && variationSpace <= 1 &&
        variationTiming >= -1 && variationTiming <= 1 &&
        (!profileId.isEmpty() && bounded(profileId, kMaximumId) &&
            bounded(profileName, kMaximumName) && !meterId.isEmpty() &&
            meterNumerator >= 1 && meterNumerator <= 16 &&
            (meterDenominator == 2 || meterDenominator == 4 || meterDenominator == 8 ||
             meterDenominator == 16) &&
            beatUnit >= 1 && beatUnit <= 16 &&
            (tempoPulseUnits == 1 || tempoPulseUnits == 3) &&
            bounded(tempoPulseName, kMaximumName) && !beatGrouping.isEmpty() &&
            std::all_of(beatGrouping.cbegin(), beatGrouping.cend(),
                [](int group) { return group >= 1 && group <= 16; }) &&
            std::accumulate(beatGrouping.cbegin(), beatGrouping.cend(), 0) == meterNumerator &&
            clickDivision >= 1 && clickDivision <= 8 && phraseBars >= 1 &&
            phraseBars <= bars && bounded(formId, kMaximumId) &&
            bounded(formName, kMaximumName) && bounded(formDescription, kMaximumExplanation) &&
            validFormSections(formSections, bars)) &&
        bounded(progressionId, kMaximumId) && bounded(grooveId, kMaximumId) &&
        bounded(progressionFamilyId, kMaximumId) &&
        bounded(chordPatchId, kMaximumId) && bounded(melodyPatchId, kMaximumId) &&
        bounded(bassPatchId, kMaximumId) && bounded(supportPatchId, kMaximumId) &&
        bounded(drumPatchId, kMaximumId) && bounded(styleName, kMaximumName) &&
        bounded(variationSummary, kMaximumExplanation) &&
        bounded(progressionName, kMaximumName) &&
        bounded(grooveName, kMaximumName) && bounded(grooveFeelName, kMaximumName) &&
        baseHarmony.size() <= kMaximumHarmonyEvents &&
        finalChordPlan.size() <= kMaximumHarmonyEvents &&
        theoryDecisions.size() <= kMaximumTheoryDecisions &&
        boundedStrings(variationDecisions, kMaximumGrooveDecisions, kMaximumExplanation) &&
        boundedStrings(motifTransformations, kMaximumMotifTransforms, kMaximumExplanation) &&
        validMelodyEvents(melodyEvents) && validMelodyPhrases(melodyPhrases) &&
        validRoleEvents(bassEvents) && validRoleEvents(supportingEvents) &&
        validDrumPhrases(
            drumPhrases, bars, bars * beatsPerBar) &&
        validDrumEvents(drumEvents, bars * beatsPerBar * 12) &&
        boundedStrings(supportingRoles, 16, kMaximumId) &&
        boundedStrings(continuationStrategies, 16, kMaximumExplanation) &&
        boundedStrings(variationAxes, 16, kMaximumExplanation) &&
        validComplexityTools(complexityTools) &&
        bounded(melodyRange, kMaximumName) &&
        boundedStrings(grooveDecisions, kMaximumGrooveDecisions, kMaximumExplanation) &&
        boundedStrings(patchModifiers, kMaximumPatchModifiers, kMaximumExplanation) &&
        validTimingPolicies(laneTiming) && validSynthVoices(synthVoices) &&
        validAutomationEvents(automationEvents) &&
        drumPatchRevision >= 1 && drumPatchRevision <= 1024 &&
        finiteInRange(drumMixGainDb, -12.0, 12.0) &&
        bounded(teachingSummary, kMaximumExplanation) &&
        bounded(jamGuidance, kMaximumExplanation);
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
    QJsonArray timing;
    for (const LaneTimingRecipe& policy : recipe.laneTiming) {
        timing.append(QJsonObject{
            {QStringLiteral("lane_id"), policy.laneId},
            {QStringLiteral("anchor_grid"), policy.anchorGrid},
            {QStringLiteral("subdivision_mapping"), policy.subdivisionMapping},
            {QStringLiteral("offset_ms"), policy.offsetMs},
            {QStringLiteral("variance_ms"), policy.varianceMs},
            {QStringLiteral("velocity_shape"), policy.velocityShape},
        });
    }
    QJsonArray complexityTools;
    for (const ComplexityToolRecipe& tool : recipe.complexityTools) {
        complexityTools.append(QJsonObject{
            {QStringLiteral("level"), tool.level},
            {QStringLiteral("tool_id"), tool.toolId},
            {QStringLiteral("name"), tool.name},
            {QStringLiteral("selected"), tool.selected},
            {QStringLiteral("explanation"), tool.explanation},
        });
    }
    QJsonArray synthVoices;
    for (const SynthVoiceRecipe& voice : recipe.synthVoices) {
        synthVoices.append(QJsonObject{
            {QStringLiteral("role_id"), voice.roleId},
            {QStringLiteral("engine"), voice.engine},
            {QStringLiteral("oscillator"), voice.oscillator},
            {QStringLiteral("attack_ms"), voice.attackMs},
            {QStringLiteral("release_ms"), voice.releaseMs},
            {QStringLiteral("cutoff_hz"), voice.cutoffHz},
            {QStringLiteral("resonance"), voice.resonance},
            {QStringLiteral("drive"), voice.drive},
            {QStringLiteral("detune_cents"), voice.detuneCents},
            {QStringLiteral("noise_mix"), voice.noiseMix},
            {QStringLiteral("effects"), stringsToJson(voice.effects)},
        });
    }
    QJsonArray automation;
    for (const AutomationRecipeEvent& event : recipe.automationEvents) {
        automation.append(QJsonObject{
            {QStringLiteral("start_tick"), event.startTick},
            {QStringLiteral("end_tick"), event.endTick},
            {QStringLiteral("target"), event.target},
            {QStringLiteral("start_value"), event.startValue},
            {QStringLiteral("end_value"), event.endValue},
            {QStringLiteral("curve"), event.curve},
            {QStringLiteral("explanation"), event.explanation},
        });
    }
    QJsonArray formSections;
    for (const FormSectionRecipe& section : recipe.formSections) {
        formSections.append(QJsonObject{
            {QStringLiteral("label"), section.label},
            {QStringLiteral("start_bar"), section.startBar},
            {QStringLiteral("bars"), section.bars},
            {QStringLiteral("role"), section.role},
            {QStringLiteral("relationship"), section.relationship},
        });
    }
    QJsonArray drumEvents;
    for (const DrumPerformanceEvent& event : recipe.drumEvents) {
        drumEvents.append(QJsonObject{
            {QStringLiteral("tick"), event.tick},
            {QStringLiteral("lane"), event.laneId},
            {QStringLiteral("velocity"), event.velocity},
            {QStringLiteral("offset_ms"), event.offsetMs},
            {QStringLiteral("articulation"), event.articulation},
            {QStringLiteral("role"), event.role},
            {QStringLiteral("repeat_group"), event.repeatGroup},
            {QStringLiteral("fill"), event.fill},
        });
    }
    QJsonArray drumPhrases;
    for (const DrumPhraseRecipe& phrase : recipe.drumPhrases) {
        drumPhrases.append(QJsonObject{
            {QStringLiteral("start_bar"), phrase.startBar},
            {QStringLiteral("end_bar"), phrase.endBar},
            {QStringLiteral("label"), phrase.label},
            {QStringLiteral("form_role"), phrase.formRole},
            {QStringLiteral("energy"), phrase.energy},
            {QStringLiteral("development"), phrase.development},
            {QStringLiteral("transition"), phrase.transition},
            {QStringLiteral("fill_id"), phrase.fillId},
            {QStringLiteral("fill_start_beat"), phrase.fillStartBeat},
            {QStringLiteral("fill_beat_count"), phrase.fillBeatCount},
        });
    }
    return {
        {QStringLiteral("generator_version"), recipe.generatorVersion},
        {QStringLiteral("seed"), static_cast<qint64>(recipe.seed)},
        {QStringLiteral("style"), QJsonObject{{QStringLiteral("id"), recipe.styleId}, {QStringLiteral("name"), recipe.styleName}}},
        {QStringLiteral("profile"), QJsonObject{{QStringLiteral("id"), recipe.profileId}, {QStringLiteral("name"), recipe.profileName}}},
        {QStringLiteral("production_family"), QJsonObject{
            {QStringLiteral("id"), recipe.productionFamilyId},
            {QStringLiteral("name"), recipe.productionFamilyName},
        }},
        {QStringLiteral("variation"), QJsonObject{
            {QStringLiteral("id"), recipe.variationId},
            {QStringLiteral("summary"), recipe.variationSummary},
            {QStringLiteral("density"), recipe.variationDensity},
            {QStringLiteral("register"), recipe.variationRegister},
            {QStringLiteral("articulation"), recipe.variationArticulation},
            {QStringLiteral("brightness"), recipe.variationBrightness},
            {QStringLiteral("space"), recipe.variationSpace},
            {QStringLiteral("timing"), recipe.variationTiming},
        }},
        {QStringLiteral("tonic"), recipe.tonic},
        {QStringLiteral("mode"), recipe.mode},
        {QStringLiteral("variation_decisions"), stringsToJson(recipe.variationDecisions)},
        {QStringLiteral("bpm"), recipe.bpm},
        {QStringLiteral("beats_per_bar"), recipe.beatsPerBar},
        {QStringLiteral("bars"), recipe.bars},
        {QStringLiteral("complexity"), recipe.complexity},
        {QStringLiteral("time"), QJsonObject{
            {QStringLiteral("meter_id"), recipe.meterId},
            {QStringLiteral("numerator"), recipe.meterNumerator},
            {QStringLiteral("denominator"), recipe.meterDenominator},
            {QStringLiteral("beat_unit"), recipe.beatUnit},
            {QStringLiteral("tempo_pulse_units"), recipe.tempoPulseUnits},
            {QStringLiteral("tempo_pulse_name"), recipe.tempoPulseName},
            {QStringLiteral("grouping"), integersToJson(recipe.beatGrouping)},
            {QStringLiteral("subdivision_family"), recipe.subdivisionFamily},
            {QStringLiteral("perceived_time"), recipe.perceivedTime},
            {QStringLiteral("click_division"), recipe.clickDivision},
            {QStringLiteral("lane_timing"), timing},
        }},
        {QStringLiteral("native_form"), QJsonObject{
            {QStringLiteral("id"), recipe.formId},
            {QStringLiteral("name"), recipe.formName},
            {QStringLiteral("phrase_bars"), recipe.phraseBars},
            {QStringLiteral("description"), recipe.formDescription},
            {QStringLiteral("sections"), formSections},
        }},
        {QStringLiteral("complexity_tools"), complexityTools},
        {QStringLiteral("progression"), QJsonObject{
            {QStringLiteral("id"), recipe.progressionId},
            {QStringLiteral("name"), recipe.progressionName},
            {QStringLiteral("family_id"), recipe.progressionFamilyId},
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
        {QStringLiteral("roles"), QJsonObject{
            {QStringLiteral("bass_events"), roleEventsToJson(recipe.bassEvents)},
            {QStringLiteral("supporting_events"), roleEventsToJson(recipe.supportingEvents)},
            {QStringLiteral("bass_grammar"), recipe.bassGrammar},
            {QStringLiteral("supporting_roles"), stringsToJson(recipe.supportingRoles)},
            {QStringLiteral("continuation_strategies"), stringsToJson(recipe.continuationStrategies)},
            {QStringLiteral("variation_axes"), stringsToJson(recipe.variationAxes)},
        }},
        {QStringLiteral("automation"), automation},
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
            {QStringLiteral("phrase_plan"), drumPhrases},
            {QStringLiteral("performance_events"), drumEvents},
        }},
        {QStringLiteral("patches"), QJsonObject{
            {QStringLiteral("chord_id"), recipe.chordPatchId},
            {QStringLiteral("chord_name"), recipe.chordPatchName},
            {QStringLiteral("melody_id"), recipe.melodyPatchId},
            {QStringLiteral("melody_name"), recipe.melodyPatchName},
            {QStringLiteral("bass_id"), recipe.bassPatchId},
            {QStringLiteral("bass_name"), recipe.bassPatchName},
            {QStringLiteral("support_id"), recipe.supportPatchId},
            {QStringLiteral("support_name"), recipe.supportPatchName},
            {QStringLiteral("drum_id"), recipe.drumPatchId},
            {QStringLiteral("drum_name"), recipe.drumPatchName},
            {QStringLiteral("drum_revision"), recipe.drumPatchRevision},
            {QStringLiteral("drum_mix_gain_db"), recipe.drumMixGainDb},
            {QStringLiteral("modifiers"), stringsToJson(recipe.patchModifiers)},
            {QStringLiteral("synth_voices"), synthVoices},
        }},
        {QStringLiteral("teaching"), QJsonObject{
            {QStringLiteral("summary"), recipe.teachingSummary},
            {QStringLiteral("jam_guidance"), recipe.jamGuidance},
        }},
        {QStringLiteral("fingerprints"), QJsonObject{
            {QStringLiteral("chord"), recipe.chordFingerprint},
            {QStringLiteral("beat"), recipe.beatFingerprint},
        }},
    };
}

bool generationRecipeFromJson(const QJsonObject& object, GenerationRecipe& recipe)
{
    if (!exactInteger(object.value(QStringLiteral("generator_version")), 7, 7) ||
        !exactUnsigned32(object.value(QStringLiteral("seed"))) ||
        !exactInteger(object.value(QStringLiteral("bpm")), 20, 400) ||
        !exactInteger(object.value(QStringLiteral("beats_per_bar")), 1, 12) ||
        !exactInteger(object.value(QStringLiteral("complexity")), 1, 8) ||
        !object.value(QStringLiteral("style")).isObject() ||
        !object.value(QStringLiteral("variation")).isObject() ||
        !object.value(QStringLiteral("progression")).isObject() ||
        !object.value(QStringLiteral("motif")).isObject() ||
        !object.value(QStringLiteral("groove")).isObject() ||
        !object.value(QStringLiteral("patches")).isObject() ||
        !object.value(QStringLiteral("fingerprints")).isObject()) {
        return false;
    }
    if (!exactInteger(object.value(QStringLiteral("bars")), 4, 64)) {
        return false;
    }
    GenerationRecipe next;
    next.generatorVersion = 7;
    next.seed = static_cast<std::uint32_t>(object.value(QStringLiteral("seed")).toInteger());
    next.bpm = object.value(QStringLiteral("bpm")).toInt();
    next.beatsPerBar = object.value(QStringLiteral("beats_per_bar")).toInt();
    next.bars = object.value(QStringLiteral("bars")).toInt();
    next.complexity = object.value(QStringLiteral("complexity")).toInt();
    const QJsonObject style = object.value(QStringLiteral("style")).toObject();
    const QJsonObject variation = object.value(QStringLiteral("variation")).toObject();
    next.styleId = style.value(QStringLiteral("id")).toString();
    next.styleName = style.value(QStringLiteral("name")).toString();
    {
        if (!object.value(QStringLiteral("profile")).isObject() ||
            !object.value(QStringLiteral("time")).isObject() ||
            !object.value(QStringLiteral("native_form")).isObject() ||
            !object.value(QStringLiteral("roles")).isObject() ||
            !object.value(QStringLiteral("complexity_tools")).isArray()) {
            return false;
        }
        const QJsonObject profile = object.value(QStringLiteral("profile")).toObject();
        next.profileId = profile.value(QStringLiteral("id")).toString();
        next.profileName = profile.value(QStringLiteral("name")).toString();
        const QJsonObject production = object.value(QStringLiteral("production_family")).toObject();
        next.productionFamilyId = production.value(QStringLiteral("id")).toString();
        next.productionFamilyName = production.value(QStringLiteral("name")).toString();

        const QJsonObject time = object.value(QStringLiteral("time")).toObject();
        if (!exactInteger(time.value(QStringLiteral("numerator")), 1, 16) ||
            !exactInteger(time.value(QStringLiteral("denominator")), 2, 16) ||
            !exactInteger(time.value(QStringLiteral("beat_unit")), 1, 16) ||
            !exactInteger(time.value(QStringLiteral("tempo_pulse_units")), 1, 3) ||
            !exactInteger(time.value(QStringLiteral("click_division")), 1, 8) ||
            !time.value(QStringLiteral("grouping")).isArray()) {
            return false;
        }
        next.meterId = time.value(QStringLiteral("meter_id")).toString();
        next.meterNumerator = time.value(QStringLiteral("numerator")).toInt();
        next.meterDenominator = time.value(QStringLiteral("denominator")).toInt();
        next.beatUnit = time.value(QStringLiteral("beat_unit")).toInt();
        next.tempoPulseUnits = time.value(QStringLiteral("tempo_pulse_units")).toInt();
        next.tempoPulseName = time.value(QStringLiteral("tempo_pulse_name")).toString();
        next.subdivisionFamily = time.value(QStringLiteral("subdivision_family")).toString();
        next.perceivedTime = time.value(QStringLiteral("perceived_time")).toString();
        next.clickDivision = time.value(QStringLiteral("click_division")).toInt();
        const QJsonArray grouping = time.value(QStringLiteral("grouping")).toArray();
        if (grouping.isEmpty() || grouping.size() > 16) return false;
        next.beatGrouping.clear();
        for (const QJsonValue& value : grouping) {
            if (!exactInteger(value, 1, 16)) return false;
            next.beatGrouping.push_back(value.toInt());
        }
        const QJsonValue timingValue = time.value(QStringLiteral("lane_timing"));
        if (!timingValue.isArray() || timingValue.toArray().size() > kMaximumTimingPolicies) return false;
        for (const QJsonValue& value : timingValue.toArray()) {
            if (!value.isObject()) return false;
            const QJsonObject item = value.toObject();
            if (!exactInteger(item.value(QStringLiteral("offset_ms")), -50, 50) ||
                !exactInteger(item.value(QStringLiteral("variance_ms")), 0, 20)) return false;
            LaneTimingRecipe policy;
            policy.laneId = item.value(QStringLiteral("lane_id")).toString();
            policy.anchorGrid = item.value(QStringLiteral("anchor_grid")).toString();
            policy.subdivisionMapping = item.value(QStringLiteral("subdivision_mapping")).toString();
            policy.offsetMs = item.value(QStringLiteral("offset_ms")).toInt();
            policy.varianceMs = item.value(QStringLiteral("variance_ms")).toInt();
            policy.velocityShape = item.value(QStringLiteral("velocity_shape")).toString();
            next.laneTiming.push_back(std::move(policy));
        }

        const QJsonObject nativeForm = object.value(QStringLiteral("native_form")).toObject();
        if (!exactInteger(nativeForm.value(QStringLiteral("phrase_bars")), 1, 64)) return false;
        next.formId = nativeForm.value(QStringLiteral("id")).toString();
        next.formName = nativeForm.value(QStringLiteral("name")).toString();
        next.phraseBars = nativeForm.value(QStringLiteral("phrase_bars")).toInt();
        next.formDescription = nativeForm.value(QStringLiteral("description")).toString();
        const QJsonValue sectionsValue = nativeForm.value(QStringLiteral("sections"));
        if (!sectionsValue.isArray() ||
            sectionsValue.toArray().isEmpty() ||
            sectionsValue.toArray().size() > kMaximumFormSections) return false;
        for (const QJsonValue& raw : sectionsValue.toArray()) {
            if (!raw.isObject()) return false;
            const QJsonObject item = raw.toObject();
            if (!exactInteger(item.value(QStringLiteral("start_bar")), 1, 64) ||
                !exactInteger(item.value(QStringLiteral("bars")), 1, 64)) return false;
            next.formSections.push_back({
                item.value(QStringLiteral("label")).toString(),
                item.value(QStringLiteral("start_bar")).toInt(),
                item.value(QStringLiteral("bars")).toInt(),
                item.value(QStringLiteral("role")).toString(),
                item.value(QStringLiteral("relationship")).toString(),
            });
        }

        const QJsonArray complexityTools = object.value(QStringLiteral("complexity_tools")).toArray();
        if (complexityTools.size() > kMaximumComplexityTools) return false;
        for (const QJsonValue& value : complexityTools) {
            if (!value.isObject()) return false;
            const QJsonObject item = value.toObject();
            if (!exactInteger(item.value(QStringLiteral("level")), 1, 8) ||
                !item.value(QStringLiteral("selected")).isBool()) return false;
            ComplexityToolRecipe tool;
            tool.level = item.value(QStringLiteral("level")).toInt();
            tool.toolId = item.value(QStringLiteral("tool_id")).toString();
            tool.name = item.value(QStringLiteral("name")).toString();
            tool.selected = item.value(QStringLiteral("selected")).toBool();
            tool.explanation = item.value(QStringLiteral("explanation")).toString();
            next.complexityTools.push_back(std::move(tool));
        }
    }
    if (!exactInteger(variation.value(QStringLiteral("density")), -1, 1) ||
        !exactInteger(variation.value(QStringLiteral("register")), -1, 1) ||
        !exactInteger(variation.value(QStringLiteral("articulation")), -1, 1) ||
        !exactInteger(variation.value(QStringLiteral("brightness")), -1, 1) ||
        !exactInteger(variation.value(QStringLiteral("space")), -1, 1) ||
        !exactInteger(variation.value(QStringLiteral("timing")), -1, 1)) {
        return false;
    }
    next.variationId = variation.value(QStringLiteral("id")).toString();
    next.variationSummary = variation.value(QStringLiteral("summary")).toString();
    next.variationDensity = variation.value(QStringLiteral("density")).toInt();
    next.variationRegister = variation.value(QStringLiteral("register")).toInt();
    next.variationArticulation = variation.value(QStringLiteral("articulation")).toInt();
    next.variationBrightness = variation.value(QStringLiteral("brightness")).toInt();
    next.variationSpace = variation.value(QStringLiteral("space")).toInt();
    next.variationTiming = variation.value(QStringLiteral("timing")).toInt();
    next.tonic = object.value(QStringLiteral("tonic")).toString();
    next.mode = object.value(QStringLiteral("mode")).toString();
    if (!stringsFromJson(object.value(QStringLiteral("variation_decisions")), next.variationDecisions,
            kMaximumGrooveDecisions, kMaximumExplanation)) return false;
    const QJsonObject progression = object.value(QStringLiteral("progression")).toObject();
    next.progressionId = progression.value(QStringLiteral("id")).toString();
    next.progressionName = progression.value(QStringLiteral("name")).toString();
    next.progressionFamilyId = progression.value(QStringLiteral("family_id")).toString();
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
        if (!exactInteger(item.value(QStringLiteral("start_bar")), 1, 64) ||
            !exactInteger(item.value(QStringLiteral("end_bar")), 1, 64)) return false;
        MelodyPhraseRecipe phrase;
        phrase.startBar = item.value(QStringLiteral("start_bar")).toInt();
        phrase.endBar = item.value(QStringLiteral("end_bar")).toInt();
        phrase.label = item.value(QStringLiteral("label")).toString();
        phrase.summary = item.value(QStringLiteral("summary")).toString();
        next.melodyPhrases.push_back(std::move(phrase));
    }
    next.melodyRange = motif.value(QStringLiteral("range")).toString();
    {
        const QJsonObject roles = object.value(QStringLiteral("roles")).toObject();
        const auto parseRoleEvents = [](const QJsonValue& value, QVector<RoleRecipeEvent>& output) {
            if (!value.isArray() || value.toArray().size() > kMaximumRoleEvents) return false;
            for (const QJsonValue& raw : value.toArray()) {
                if (!raw.isObject()) return false;
                const QJsonObject item = raw.toObject();
                if (!exactInteger(item.value(QStringLiteral("tick")), 0, 32768) ||
                    !exactInteger(item.value(QStringLiteral("duration_ticks")), 1, 32768) ||
                    !exactInteger(item.value(QStringLiteral("midi")), 0, 127) ||
                    !exactInteger(item.value(QStringLiteral("velocity")), 1, 127)) return false;
                RoleRecipeEvent event;
                event.tick = item.value(QStringLiteral("tick")).toInt();
                event.durationTicks = item.value(QStringLiteral("duration_ticks")).toInt();
                event.midi = item.value(QStringLiteral("midi")).toInt();
                event.velocity = item.value(QStringLiteral("velocity")).toInt();
                event.note = item.value(QStringLiteral("note")).toString();
                event.role = item.value(QStringLiteral("role")).toString();
                event.relationship = item.value(QStringLiteral("relationship")).toString();
                event.articulation = item.value(QStringLiteral("articulation")).toString();
                output.push_back(std::move(event));
            }
            return true;
        };
        if (!parseRoleEvents(roles.value(QStringLiteral("bass_events")), next.bassEvents) ||
            !parseRoleEvents(roles.value(QStringLiteral("supporting_events")), next.supportingEvents)) return false;
        next.bassGrammar = roles.value(QStringLiteral("bass_grammar")).toString();
        if (!stringsFromJson(roles.value(QStringLiteral("supporting_roles")), next.supportingRoles,
                16, kMaximumId) ||
            !stringsFromJson(roles.value(QStringLiteral("continuation_strategies")), next.continuationStrategies,
                16, kMaximumExplanation) ||
            !stringsFromJson(roles.value(QStringLiteral("variation_axes")), next.variationAxes,
                16, kMaximumExplanation)) return false;
        const QJsonValue automationValue = object.value(QStringLiteral("automation"));
        if (!automationValue.isUndefined()) {
            if (!automationValue.isArray() ||
                automationValue.toArray().size() > kMaximumAutomationEvents) return false;
            for (const QJsonValue& raw : automationValue.toArray()) {
                if (!raw.isObject()) return false;
                const QJsonObject item = raw.toObject();
                if (!exactInteger(item.value(QStringLiteral("start_tick")), 0, 32767) ||
                    !exactInteger(item.value(QStringLiteral("end_tick")), 1, 32768) ||
                    !item.value(QStringLiteral("start_value")).isDouble() ||
                    !item.value(QStringLiteral("end_value")).isDouble()) return false;
                AutomationRecipeEvent event;
                event.startTick = item.value(QStringLiteral("start_tick")).toInt();
                event.endTick = item.value(QStringLiteral("end_tick")).toInt();
                event.target = item.value(QStringLiteral("target")).toString();
                event.startValue = item.value(QStringLiteral("start_value")).toDouble();
                event.endValue = item.value(QStringLiteral("end_value")).toDouble();
                event.curve = item.value(QStringLiteral("curve")).toString();
                event.explanation = item.value(QStringLiteral("explanation")).toString();
                next.automationEvents.push_back(std::move(event));
            }
        }
    }
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
    {
        const QJsonValue drumPhrasesValue =
            groove.value(QStringLiteral("phrase_plan"));
        if (!drumPhrasesValue.isUndefined() &&
            !drumPhrasesValue.isNull()) {
            if (!drumPhrasesValue.isArray() ||
                drumPhrasesValue.toArray().size() >
                    kMaximumDrumPhrases) {
                return false;
            }
            for (const QJsonValue& raw :
                 drumPhrasesValue.toArray()) {
                if (!raw.isObject()) return false;
                const QJsonObject item = raw.toObject();
                if (!exactInteger(
                        item.value(QStringLiteral("start_bar")),
                        1,
                        next.bars) ||
                    !exactInteger(
                        item.value(QStringLiteral("end_bar")),
                        1,
                        next.bars) ||
                    !exactInteger(
                        item.value(QStringLiteral("energy")),
                        0,
                        3) ||
                    !exactInteger(
                        item.value(
                            QStringLiteral("fill_start_beat")),
                        -1,
                        next.bars * next.beatsPerBar - 1) ||
                    !exactInteger(
                        item.value(
                            QStringLiteral("fill_beat_count")),
                        0,
                        next.bars * next.beatsPerBar)) {
                    return false;
                }
                DrumPhraseRecipe phrase;
                phrase.startBar =
                    item.value(
                        QStringLiteral("start_bar")).toInt();
                phrase.endBar =
                    item.value(
                        QStringLiteral("end_bar")).toInt();
                phrase.label =
                    item.value(QStringLiteral("label")).toString();
                phrase.formRole =
                    item.value(
                        QStringLiteral("form_role")).toString();
                phrase.energy =
                    item.value(QStringLiteral("energy")).toInt();
                phrase.development =
                    item.value(
                        QStringLiteral("development")).toString();
                phrase.transition =
                    item.value(
                        QStringLiteral("transition")).toString();
                phrase.fillId =
                    item.value(QStringLiteral("fill_id")).toString();
                phrase.fillStartBeat =
                    item.value(
                        QStringLiteral("fill_start_beat")).toInt();
                phrase.fillBeatCount =
                    item.value(
                        QStringLiteral("fill_beat_count")).toInt();
                next.drumPhrases.push_back(std::move(phrase));
            }
        }
    }
    {
        const QJsonValue eventsValue =
            groove.value(QStringLiteral("performance_events"));
        if (!eventsValue.isArray() ||
            eventsValue.toArray().isEmpty() ||
            eventsValue.toArray().size() > kMaximumDrumEvents) {
            return false;
        }
        for (const QJsonValue& raw : eventsValue.toArray()) {
            if (!raw.isObject()) return false;
            const QJsonObject item = raw.toObject();
            if (!exactInteger(
                    item.value(QStringLiteral("tick")),
                    0,
                    next.bars * next.beatsPerBar * 12 - 1) ||
                !exactInteger(
                    item.value(QStringLiteral("velocity")), 1, 127) ||
                !exactInteger(
                    item.value(QStringLiteral("offset_ms")), -40, 40) ||
                !exactInteger(
                    item.value(QStringLiteral("repeat_group")), 0, 4096) ||
                !item.value(QStringLiteral("fill")).isBool()) {
                return false;
            }
            DrumPerformanceEvent event;
            event.tick = item.value(QStringLiteral("tick")).toInt();
            event.laneId = item.value(QStringLiteral("lane")).toString();
            event.velocity =
                item.value(QStringLiteral("velocity")).toInt();
            event.offsetMs =
                item.value(QStringLiteral("offset_ms")).toInt();
            event.articulation =
                item.value(QStringLiteral("articulation")).toString();
            event.role = item.value(QStringLiteral("role")).toString();
            event.repeatGroup =
                item.value(QStringLiteral("repeat_group")).toInt();
            event.fill = item.value(QStringLiteral("fill")).toBool();
            next.drumEvents.push_back(std::move(event));
        }
    }
    const QJsonObject patches = object.value(QStringLiteral("patches")).toObject();
    next.chordPatchId = patches.value(QStringLiteral("chord_id")).toString();
    next.chordPatchName = patches.value(QStringLiteral("chord_name")).toString();
    next.melodyPatchId = patches.value(QStringLiteral("melody_id")).toString();
    next.melodyPatchName = patches.value(QStringLiteral("melody_name")).toString();
    next.bassPatchId = patches.value(QStringLiteral("bass_id")).toString();
    next.bassPatchName = patches.value(QStringLiteral("bass_name")).toString();
    next.supportPatchId = patches.value(QStringLiteral("support_id")).toString();
    next.supportPatchName = patches.value(QStringLiteral("support_name")).toString();
    next.drumPatchId = patches.value(QStringLiteral("drum_id")).toString();
    next.drumPatchName = patches.value(QStringLiteral("drum_name")).toString();
    if (!exactInteger(
            patches.value(QStringLiteral("drum_revision")), 1, 1024) ||
        !patches.value(QStringLiteral("drum_mix_gain_db")).isDouble()) {
        return false;
    }
    next.drumPatchRevision =
        patches.value(QStringLiteral("drum_revision")).toInt();
    next.drumMixGainDb =
        patches.value(QStringLiteral("drum_mix_gain_db")).toDouble();
    if (!stringsFromJson(patches.value(QStringLiteral("modifiers")), next.patchModifiers,
            kMaximumPatchModifiers, kMaximumExplanation)) return false;
    {
        const QJsonValue voicesValue = patches.value(QStringLiteral("synth_voices"));
        if (!voicesValue.isArray() || voicesValue.toArray().size() > kMaximumSynthVoices) return false;
        for (const QJsonValue& value : voicesValue.toArray()) {
            if (!value.isObject()) return false;
            const QJsonObject item = value.toObject();
            for (const QString& key : {
                    QStringLiteral("attack_ms"), QStringLiteral("release_ms"),
                    QStringLiteral("cutoff_hz"), QStringLiteral("resonance"),
                    QStringLiteral("drive"), QStringLiteral("detune_cents"),
                    QStringLiteral("noise_mix")}) {
                if (!item.value(key).isDouble()) return false;
            }
            SynthVoiceRecipe voice;
            voice.roleId = item.value(QStringLiteral("role_id")).toString();
            voice.engine = item.value(QStringLiteral("engine")).toString();
            voice.oscillator = item.value(QStringLiteral("oscillator")).toString();
            voice.attackMs = item.value(QStringLiteral("attack_ms")).toDouble();
            voice.releaseMs = item.value(QStringLiteral("release_ms")).toDouble();
            voice.cutoffHz = item.value(QStringLiteral("cutoff_hz")).toDouble();
            voice.resonance = item.value(QStringLiteral("resonance")).toDouble();
            voice.drive = item.value(QStringLiteral("drive")).toDouble();
            voice.detuneCents = item.value(QStringLiteral("detune_cents")).toDouble();
            voice.noiseMix = item.value(QStringLiteral("noise_mix")).toDouble();
            if (!stringsFromJson(item.value(QStringLiteral("effects")), voice.effects, 8, kMaximumName)) return false;
            next.synthVoices.push_back(std::move(voice));
        }
        const QJsonObject teaching = object.value(QStringLiteral("teaching")).toObject();
        next.teachingSummary = teaching.value(QStringLiteral("summary")).toString();
        next.jamGuidance = teaching.value(QStringLiteral("jam_guidance")).toString();
    }
    const QJsonObject fingerprints = object.value(QStringLiteral("fingerprints")).toObject();
    next.chordFingerprint = fingerprints.value(QStringLiteral("chord")).toString();
    next.beatFingerprint = fingerprints.value(QStringLiteral("beat")).toString();
    if (!next.isValid()) return false;
    recipe = std::move(next);
    return true;
}

QString generationRecipeTeaching(const GenerationRecipe& recipe, bool contentChanged)
{
    QStringList lines;
    lines << QStringLiteral("%1 - %2").arg(recipe.styleName, recipe.profileName)
          << QStringLiteral("%1 %2 | %3 | %4/%5 | %6 bars")
                .arg(recipe.tonic, recipe.mode)
                .arg(recipe.bpm)
                .arg(recipe.meterNumerator)
                .arg(recipe.meterDenominator)
                .arg(recipe.bars);
    if (!recipe.productionFamilyName.isEmpty()) {
        lines << QStringLiteral("Production family: %1").arg(recipe.productionFamilyName);
    }
    if (contentChanged) {
        lines << QString()
              << QStringLiteral(
                     "The grid has been edited since generation. The explanation below describes the original idea.");
    }
    lines << QString()
          << QStringLiteral("WHAT THIS STYLE IDEA IS")
          << QStringLiteral("  %1").arg(heading(recipe.teachingSummary))
          << QStringLiteral("  Seed-derived profile variation: %1.")
                .arg(heading(recipe.variationSummary))
          << QString()
          << QStringLiteral("HOW TO JAM WITH IT")
          << QStringLiteral("  %1").arg(heading(recipe.jamGuidance))
          << QString()
          << QStringLiteral("FORM AND FEEL")
          << QStringLiteral("  %1: %2").arg(heading(recipe.formName), heading(recipe.formDescription))
          << QStringLiteral("  Groove: %1 - %2")
                .arg(heading(recipe.grooveName), heading(recipe.grooveCore))
          << QStringLiteral("  Count %1/%2 as %3; the working subdivision is %4 and the perceived time is %5.")
                .arg(recipe.meterNumerator)
                .arg(recipe.meterDenominator)
                .arg([&recipe] {
                    QStringList groups;
                    for (int group : recipe.beatGrouping) groups << QString::number(group);
                    return groups.join(QLatin1Char('+'));
                }())
                .arg(heading(recipe.subdivisionFamily), heading(recipe.perceivedTime))
          << QStringLiteral("  Tempo is %1 %2 pulses per minute; each pulse spans %3 written beat unit(s).")
                .arg(recipe.bpm)
                .arg(heading(recipe.tempoPulseName))
                .arg(recipe.tempoPulseUnits)
          << QString()
          << QStringLiteral("WHAT EACH PART IS DOING")
          << QStringLiteral("  Harmony: %1 (%2).")
                .arg(heading(recipe.progressionName), heading(recipe.progressionFamilyId))
          << QStringLiteral("  Melody: %1; %2.")
                .arg(heading(recipe.motifForm), heading(recipe.melodyRange))
          << QStringLiteral("  Bass: %1").arg(heading(recipe.bassGrammar))
          << QStringLiteral("  Supporting roles: %1")
                .arg(recipe.supportingRoles.isEmpty()
                    ? QStringLiteral("none selected")
                    : recipe.supportingRoles.join(QStringLiteral(", ")))
          << QStringLiteral("  Drums: %1.").arg(heading(recipe.grooveFeelName))
          << QString()
          << QStringLiteral("COMPLEXITY %1").arg(recipe.complexity)
          << QStringLiteral(
                 "  A level unlocks tools; it does not force every available tool into the result.");
    for (int level = 1; level <= recipe.complexity; ++level) {
        QStringList selected;
        QStringList available;
        for (const ComplexityToolRecipe& tool : recipe.complexityTools) {
            if (tool.level != level) continue;
            (tool.selected ? selected : available) << tool.name;
        }
        QString summary = QStringLiteral("  Level %1").arg(level);
        if (!selected.isEmpty()) summary += QStringLiteral(" used: %1").arg(selected.join(QStringLiteral(", ")));
        if (!available.isEmpty()) {
            if (!selected.isEmpty()) summary += QLatin1Char(';');
            summary += QStringLiteral(" available but not used: %1").arg(available.join(QStringLiteral(", ")));
        }
        lines << summary;
    }
    lines << QString()
          << QStringLiteral("TRY IT YOURSELF")
          << QStringLiteral("  1. Play only the core groove and structural chord or riff attacks.")
          << QStringLiteral("  2. Add the bass as its own line; notice where it agrees with or leads the harmony.")
          << QStringLiteral("  3. Play the melody as a vocal phrase, including its rests.")
          << QStringLiteral("  4. Add one supporting role only where the lead leaves space.")
          << QStringLiteral("  5. Change one documented variation axis and keep the profile's core relationship audible.");
    return lines.join(QLatin1Char('\n'));
}

QString generationRecipeDetails(const GenerationRecipe& recipe, bool contentChanged)
{
    QStringList lines;
    lines << QStringLiteral("%1 %2 - %3 - %4/%5")
        .arg(recipe.tonic, recipe.mode, recipe.profileName)
        .arg(recipe.meterNumerator)
        .arg(recipe.meterDenominator);
    lines << QStringLiteral("Seed: %1 (0x%2)    Generator: v%3")
        .arg(recipe.seed).arg(recipe.seed, 8, 16, QLatin1Char('0')).arg(recipe.generatorVersion);
    QStringList rawGrouping;
    for (int group : recipe.beatGrouping) rawGrouping << QString::number(group);
    lines << QStringLiteral("Profile: %1 (%2)    Production: %3")
        .arg(heading(recipe.profileName), heading(recipe.profileId),
            heading(recipe.productionFamilyName));
    lines << QStringLiteral("Meter: %1/%2 (%3)    Tempo: %4 %5 BPM    Length: %6 bars    Complexity: %7")
        .arg(recipe.meterNumerator).arg(recipe.meterDenominator)
        .arg(rawGrouping.join(QLatin1Char('+'))).arg(recipe.bpm)
        .arg(heading(recipe.tempoPulseName)).arg(recipe.bars).arg(recipe.complexity);
    lines << QStringLiteral("Form: %1 (%2), phrase unit %3 bars - %4")
        .arg(heading(recipe.formName), heading(recipe.formId))
        .arg(recipe.phraseBars).arg(heading(recipe.formDescription));
    for (const FormSectionRecipe& section : recipe.formSections) {
        lines << QStringLiteral("  %1 bars %2-%3: %4 - %5")
            .arg(section.label).arg(section.startBar)
            .arg(section.startBar + section.bars - 1)
            .arg(section.role, section.relationship);
    }
    if (contentChanged) {
        lines << QString() << QStringLiteral("Warning: the current grid differs from the generated output stored in this recipe.");
    }
    lines << QString() << QStringLiteral("VARIATION PLAN");
    lines << QStringLiteral("  %1").arg(heading(recipe.variationSummary));
    lines << QStringLiteral(
        "  Axes: density %1, register %2, articulation %3, brightness %4, space %5, timing %6.")
        .arg(recipe.variationDensity)
        .arg(recipe.variationRegister)
        .arg(recipe.variationArticulation)
        .arg(recipe.variationBrightness)
        .arg(recipe.variationSpace)
        .arg(recipe.variationTiming);
    for (const QString& decision : recipe.variationDecisions)
        lines << QStringLiteral("  %1").arg(decision);
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
    lines << QString() << QStringLiteral("ROLE LAYERS")
          << QStringLiteral("Bass grammar: %1").arg(heading(recipe.bassGrammar))
          << QStringLiteral("Supporting roles: %1").arg(
                recipe.supportingRoles.isEmpty()
                    ? QStringLiteral("none") : recipe.supportingRoles.join(QStringLiteral(", ")))
          << QStringLiteral("Bass events: %1    Supporting events: %2")
                .arg(recipe.bassEvents.size()).arg(recipe.supportingEvents.size());
    for (const RoleRecipeEvent& event : recipe.bassEvents) {
        lines << QStringLiteral("  Bass tick %1: %2, %3, %4")
            .arg(event.tick).arg(event.note, event.articulation, event.relationship);
    }
    for (const RoleRecipeEvent& event : recipe.supportingEvents) {
        lines << QStringLiteral("  Support tick %1: %2 [%3], %4")
            .arg(event.tick).arg(event.note, event.role, event.relationship);
    }
    lines << QStringLiteral("Continuation strategies:");
    for (const QString& strategy : recipe.continuationStrategies) {
        lines << QStringLiteral("  %1").arg(strategy);
    }
    lines << QStringLiteral("Variation axes:");
    for (const QString& axis : recipe.variationAxes) {
        lines << QStringLiteral("  %1").arg(axis);
    }
    lines << QStringLiteral("Automation events: %1").arg(recipe.automationEvents.size());
    for (const AutomationRecipeEvent& event : recipe.automationEvents) {
        lines << QStringLiteral("  Ticks %1-%2: %3 %4 -> %5 (%6) - %7")
            .arg(event.startTick).arg(event.endTick).arg(event.target)
            .arg(event.startValue, 0, 'f', 2).arg(event.endValue, 0, 'f', 2)
            .arg(event.curve, event.explanation);
    }
    lines << QString() << QStringLiteral("COMPLEXITY TOOL LOG");
    for (const ComplexityToolRecipe& tool : recipe.complexityTools) {
        lines << QStringLiteral("  L%1 %2: %3 - %4")
            .arg(tool.level).arg(tool.name)
            .arg(tool.selected ? QStringLiteral("selected") : QStringLiteral("available"))
            .arg(tool.explanation);
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
    lines << QStringLiteral("Drummer phrase plan:");
    for (const DrumPhraseRecipe& phrase : recipe.drumPhrases) {
        QString fill = QStringLiteral("no fill");
        if (phrase.fillStartBeat >= 0) {
            fill = QStringLiteral("%1 at beat %2 for %3 beat(s)")
                .arg(heading(phrase.fillId))
                .arg(phrase.fillStartBeat + 1)
                .arg(phrase.fillBeatCount);
        }
        lines << QStringLiteral(
            "  Bars %1-%2 %3 [%4, energy %5]: %6; %7; %8")
            .arg(phrase.startBar)
            .arg(phrase.endBar)
            .arg(heading(phrase.label))
            .arg(heading(phrase.formRole))
            .arg(phrase.energy)
            .arg(heading(phrase.development))
            .arg(heading(phrase.transition))
            .arg(fill);
    }
    lines << QString() << QStringLiteral("SOUND")
          << QStringLiteral("Chords: %1    Melody: %2    Bass: %3")
                .arg(recipe.chordPatchName, recipe.melodyPatchName, recipe.bassPatchName)
          << QStringLiteral("Support: %1    Drums: %2")
                .arg(recipe.supportPatchName, recipe.drumPatchName);
    for (const QString& modifier : recipe.patchModifiers) lines << QStringLiteral("  %1").arg(modifier);
    lines << QStringLiteral("Lane timing:");
    for (const LaneTimingRecipe& policy : recipe.laneTiming) {
        lines << QStringLiteral("  %1: anchor=%2 mapping=%3 offset=%4 ms variance=%5 ms velocity=%6")
            .arg(policy.laneId, policy.anchorGrid, policy.subdivisionMapping)
            .arg(policy.offsetMs).arg(policy.varianceMs).arg(policy.velocityShape);
    }
    lines << QStringLiteral("Synthesis parameters:");
    for (const SynthVoiceRecipe& voice : recipe.synthVoices) {
        lines << QStringLiteral(
            "  %1: engine=%2 oscillator=%3 attack=%4 ms release=%5 ms cutoff=%6 Hz resonance=%7 drive=%8 detune=%9 cents noise=%10 effects=%11")
            .arg(voice.roleId, voice.engine, voice.oscillator)
            .arg(voice.attackMs, 0, 'f', 1).arg(voice.releaseMs, 0, 'f', 1)
            .arg(voice.cutoffHz, 0, 'f', 1).arg(voice.resonance, 0, 'f', 3)
            .arg(voice.drive, 0, 'f', 3).arg(voice.detuneCents, 0, 'f', 2)
            .arg(voice.noiseMix, 0, 'f', 3)
            .arg(voice.effects.join(QStringLiteral(",")));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace jam2::practice
