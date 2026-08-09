#include "PracticeIdeaController.hpp"

#include "ContentLimits.hpp"

#include <QUuid>

#include <algorithm>

namespace jam2::practice {
namespace {

QString managedReferenceKind(const LooperLane& lane)
{
    if (!lane.referenceKind.isEmpty()) {
        return lane.referenceKind;
    }
    if (lane.name == QStringLiteral("Practice Chords")) {
        return QStringLiteral("chord");
    }
    if (lane.name == QStringLiteral("Practice Drums")) {
        return QStringLiteral("drum");
    }
    if (lane.name == QStringLiteral("Practice Melody")) {
        return QStringLiteral("melody");
    }
    if (lane.name == QStringLiteral("Practice Bass")) {
        return QStringLiteral("bass");
    }
    if (lane.name == QStringLiteral("Practice Support")) {
        return QStringLiteral("support");
    }
    return {};
}

void removeDuplicateManagedLanes(QVector<LooperLane>& lanes, const QString& kind)
{
    bool found = false;
    for (int index = 0; index < lanes.size();) {
        if (managedReferenceKind(lanes[index]) != kind) {
            ++index;
            continue;
        }
        if (!found) {
            found = true;
            ++index;
        } else {
            lanes.removeAt(index);
        }
    }
}

bool hasManagedLane(const QVector<LooperLane>& lanes, const QString& kind)
{
    for (const LooperLane& lane : lanes) {
        if (managedReferenceKind(lane) == kind) return true;
    }
    return false;
}

int partialTargetIndex(const BeatGridModel& model, int requested)
{
    if (requested >= 0 && requested < model.sections().size()) return requested;
    for (int index = 0; index < model.sections().size(); ++index) {
        if (!model.section(index).generatedKind.isEmpty()) return index;
    }
    return model.sections().isEmpty() ? -1 : 0;
}

void replacePitchedParts(SongSection& destination, const SongSection& generated)
{
    destination.beats = generated.beats;
    destination.chords = generated.chords;
    destination.targets = generated.targets;
    destination.musicalPatterns = generated.musicalPatterns;
    destination.generatedRecipe = generated.generatedRecipe;
}

void replaceDrumParts(SongSection& destination, const SongSection& generated)
{
    destination.beats = generated.beats;
    destination.beatNotes = generated.beatNotes;
    destination.beatPatterns = generated.beatPatterns;
    destination.generatedRecipe = generated.generatedRecipe;
}

void upsert(
    QVector<LooperLane>& lanes,
    const QString& kind,
    const QString& name,
    const ReferenceWav& wav,
    const ReferenceRenderSettings& settings,
    const QString& signature)
{
    int index = -1;
    for (int candidate = 0; candidate < lanes.size(); ++candidate) {
        if (managedReferenceKind(lanes[candidate]) == kind) {
            index = candidate;
            break;
        }
    }
    LooperLane lane;
    if (index >= 0) lane = lanes[index];
    else {
        lane.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (kind == QStringLiteral("drum")) {
            // The researched stem already carries the accepted internal
            // drum-bus balance.  Give a newly generated drum lane the
            // additional playback presence requested for the Jam2 mix,
            // without driving the synthesis/bus harder or clipping the WAV.
            lane.gainDb = kGeneratedDrumLaneGainDb;
        }
    }
    lane.name = name;
    lane.assetPath = wav.path;
    lane.assetHash = wav.sha256;
    lane.sampleRate = settings.sampleRate;
    lane.sampleRateCompatible = true;
    lane.sourceFrames = wav.frames;
    lane.startFrame = 0;
    lane.stopFrame = wav.frames;
    lane.loopStartFrame = 0;
    lane.loopEndFrame = wav.frames;
    lane.loopEnabled = true;
    lane.referenceKind = kind;
    lane.referenceSourceSignature = signature;
    lane.referenceBpm = settings.bpm;
    lane.referenceStale = false;
    lane.localOnly = true;
    lane.originKind = QStringLiteral("generated");
    if (index >= 0) lanes[index] = std::move(lane);
    else lanes.push_back(std::move(lane));
}

} // namespace

std::optional<GeneratedPracticeIdea> PracticeIdeaController::generateCoupled(
    BeatGridModel& chordModel,
    BeatGridModel& beatModel,
    const ChordIdeaRequest& request)
{
    GeneratedPracticeIdea idea = generateCoupledPracticeIdea(request);
    if (request.parts != PracticeIdeaParts::FullArrangement) {
        const bool pitched = request.parts == PracticeIdeaParts::PitchedPartsOnly;
        if (&chordModel == &beatModel) {
            BeatGridModel next = chordModel;
            const int target = partialTargetIndex(next, request.targetSectionIndex);
            if (target < 0) return std::nullopt;
            SongSection merged = next.section(target);
            if (pitched) replacePitchedParts(merged, idea.chordSection);
            else replaceDrumParts(merged, idea.beatSection);
            merged.generatedKind = QStringLiteral("practice");
            if (!next.replaceSection(target, std::move(merged))) return std::nullopt;
            chordModel = std::move(next);
            return idea;
        }

        BeatGridModel& destinationModel = pitched ? chordModel : beatModel;
        BeatGridModel next = destinationModel;
        const int target = partialTargetIndex(next, request.targetSectionIndex);
        if (target < 0) return std::nullopt;
        SongSection merged = next.section(target);
        if (pitched) replacePitchedParts(merged, idea.chordSection);
        else replaceDrumParts(merged, idea.beatSection);
        merged.generatedKind = pitched
            ? QStringLiteral("chord")
            : QStringLiteral("beat");
        if (!next.replaceSection(target, std::move(merged))) return std::nullopt;
        destinationModel = std::move(next);
        return idea;
    }
    if (&chordModel == &beatModel) {
        SongSection combined = idea.chordSection;
        combined.beats = std::max(idea.chordSection.beats, idea.beatSection.beats);
        combined.beatNotes = idea.beatSection.beatNotes;
        combined.beatPatterns = idea.beatSection.beatPatterns;
        combined.generatedKind = QStringLiteral("practice");
        BeatGridModel next = chordModel;
        const int target = partialTargetIndex(next, request.targetSectionIndex);
        if (target < 0 || !next.replaceSection(target, std::move(combined))) {
            return std::nullopt;
        }
        chordModel = std::move(next);
        return idea;
    }
    BeatGridModel nextChord = chordModel;
    BeatGridModel nextBeat = beatModel;
    const int chordTarget = partialTargetIndex(nextChord, request.targetSectionIndex);
    const int beatTarget = partialTargetIndex(nextBeat, request.targetSectionIndex);
    idea.chordSection.generatedKind = QStringLiteral("chord");
    idea.beatSection.generatedKind = QStringLiteral("beat");
    if (chordTarget < 0 || beatTarget < 0 ||
        !nextChord.replaceSection(chordTarget, idea.chordSection) ||
        !nextBeat.replaceSection(beatTarget, idea.beatSection)) {
        return std::nullopt;
    }
    chordModel = std::move(nextChord);
    beatModel = std::move(nextBeat);
    return idea;
}

std::optional<GeneratedContinuationIdea> PracticeIdeaController::generateContinuation(
    BeatGridModel& model,
    const ContinueIdeaRequest& request)
{
    if (request.sourceSectionIndex < 0 ||
        request.sourceSectionIndex >= model.sections().size() ||
        request.targetSectionIndex < 0 ||
        request.targetSectionIndex >= model.sections().size() ||
        request.sourceSectionIndex == request.targetSectionIndex) {
        return std::nullopt;
    }
    const SongSection source = model.section(request.sourceSectionIndex);
    if (!referenceLayers(source).any()) return std::nullopt;

    GeneratedContinuationIdea continuation =
        generateContinuationPracticeIdea(source, request);
    SongSection combined = continuation.idea.chordSection;
    combined.beats = std::max(
        continuation.idea.chordSection.beats,
        continuation.idea.beatSection.beats);
    combined.beatNotes = continuation.idea.beatSection.beatNotes;
    combined.beatPatterns = continuation.idea.beatSection.beatPatterns;
    combined.generatedKind = QStringLiteral("practice");
    BeatGridModel next = model;
    if (!next.replaceSection(request.targetSectionIndex, std::move(combined))) {
        return std::nullopt;
    }
    model = std::move(next);
    return continuation;
}

std::optional<SongSection> PracticeIdeaController::generatedSection(
    const BeatGridModel& model,
    const QString& kind)
{
    for (const SongSection& section : model.sections()) {
        if (section.generatedKind == kind ||
            (section.generatedKind == QStringLiteral("practice") &&
             (kind == QStringLiteral("chord") || kind == QStringLiteral("beat")))) {
            return section;
        }
    }
    return std::nullopt;
}

ReferenceLayerAvailability PracticeIdeaController::referenceLayers(
    const SongSection& section)
{
    ReferenceLayerAvailability result;
    const auto hasOnset = [](const QVector<MusicalStep>& steps) {
        return std::any_of(steps.cbegin(), steps.cend(), [](const MusicalStep& step) {
            return step.state == MusicalStepState::Onset &&
                !step.value.trimmed().isEmpty();
        });
    };
    result.chords = std::any_of(
        section.chords.cbegin(),
        section.chords.cend(),
        [](const QString& value) {
            const QString text = value.trimmed();
            return !text.isEmpty() && text != QStringLiteral("-");
        });
    result.melody = std::any_of(
        section.targets.cbegin(),
        section.targets.cend(),
        [](const QString& value) {
            const QString text = value.trimmed();
            return !text.isEmpty() && text != QStringLiteral("-");
        });
    for (const MusicalBeatPattern& pattern : section.musicalPatterns) {
        result.chords = result.chords || hasOnset(pattern.chords);
        result.melody = result.melody || hasOnset(pattern.melody);
        result.bass = result.bass || hasOnset(pattern.bass);
        result.support = result.support || hasOnset(pattern.support);
    }
    for (const BeatPattern& pattern : section.beatPatterns) {
        for (const QString& lane : pattern.lanes) {
            if (std::any_of(lane.cbegin(), lane.cend(), [](QChar state) {
                    const QChar normalized = state.toLower();
                    return normalized == QLatin1Char('x') ||
                        normalized == QLatin1Char('a') ||
                        normalized == QLatin1Char('g');
                })) {
                result.drums = true;
                break;
            }
        }
        if (result.drums) {
            break;
        }
    }
    return result;
}

void PracticeIdeaController::clearReferences(LooperProject& project)
{
    for (LooperBank& bank : project.banks()) {
        for (int index = bank.lanes.size() - 1; index >= 0; --index) {
            if (!managedReferenceKind(bank.lanes[index]).isEmpty()) {
                bank.lanes.removeAt(index);
            }
        }
    }
}

void PracticeIdeaController::clearReferences(LooperProject& project, int bankIndex)
{
    if (bankIndex < 0 || bankIndex >= project.banks().size()) return;
    QVector<LooperLane>& lanes = project.banks()[bankIndex].lanes;
    for (int index = lanes.size() - 1; index >= 0; --index) {
        if (!managedReferenceKind(lanes[index]).isEmpty()) lanes.removeAt(index);
    }
}

bool PracticeIdeaController::applyReferences(
    LooperProject& project,
    int bankIndex,
    const ReferenceRenderSettings& settings,
    const ReferenceRenderResult& result,
    QString& error,
    const QString& lanePrefix)
{
    if (bankIndex < 0 || bankIndex >= project.banks().size() || !result.error.isEmpty() ||
        (settings.renderChords && (result.chords.path.isEmpty() || result.chords.sha256.isEmpty())) ||
        (settings.renderDrums && (result.drums.path.isEmpty() || result.drums.sha256.isEmpty())) ||
        (settings.renderMelody && (result.melody.path.isEmpty() || result.melody.sha256.isEmpty())) ||
        (settings.renderBass && (result.bass.path.isEmpty() || result.bass.sha256.isEmpty())) ||
        (settings.renderSupport && (result.support.path.isEmpty() || result.support.sha256.isEmpty()))) {
        error = result.error.isEmpty()
            ? QStringLiteral("The rendered reference assets are incomplete.") : result.error;
        return false;
    }
    QVector<LooperLane> lanes = project.banks().at(bankIndex).lanes;
    removeDuplicateManagedLanes(lanes, QStringLiteral("chord"));
    removeDuplicateManagedLanes(lanes, QStringLiteral("drum"));
    removeDuplicateManagedLanes(lanes, QStringLiteral("melody"));
    removeDuplicateManagedLanes(lanes, QStringLiteral("bass"));
    removeDuplicateManagedLanes(lanes, QStringLiteral("support"));
    const int newLaneCount =
        (settings.renderChords && !hasManagedLane(lanes, QStringLiteral("chord")) ? 1 : 0) +
        (settings.renderDrums && !hasManagedLane(lanes, QStringLiteral("drum")) ? 1 : 0) +
        (settings.renderMelody && !hasManagedLane(lanes, QStringLiteral("melody")) ? 1 : 0) +
        (settings.renderBass && !hasManagedLane(lanes, QStringLiteral("bass")) ? 1 : 0) +
        (settings.renderSupport && !hasManagedLane(lanes, QStringLiteral("support")) ? 1 : 0);
    if (lanes.size() + newLaneCount > jam2::application::limits::kMaximumLooperLanesPerBank) {
        error = QStringLiteral("The destination section has no room for the reference lanes.");
        return false;
    }
    const QString prefix = lanePrefix.trimmed().isEmpty()
        ? QStringLiteral("Practice")
        : lanePrefix.trimmed();
    if (settings.renderChords) {
        upsert(lanes, QStringLiteral("chord"), prefix + QStringLiteral(" Chords"),
            result.chords, settings, result.sourceSignature);
    }
    if (settings.renderDrums) {
        upsert(lanes, QStringLiteral("drum"), prefix + QStringLiteral(" Drums"),
            result.drums, settings, result.sourceSignature);
    }
    if (settings.renderMelody) {
        upsert(lanes, QStringLiteral("melody"), prefix + QStringLiteral(" Melody"),
            result.melody, settings, result.sourceSignature);
    }
    if (settings.renderBass) {
        upsert(lanes, QStringLiteral("bass"), prefix + QStringLiteral(" Bass"),
            result.bass, settings, result.sourceSignature);
    }
    if (settings.renderSupport) {
        upsert(lanes, QStringLiteral("support"), prefix + QStringLiteral(" Support"),
            result.support, settings, result.sourceSignature);
    }
    project.banks()[bankIndex].lanes = std::move(lanes);
    return true;
}

} // namespace jam2::practice
