#include "PracticeIdeaController.hpp"

#include "ContentLimits.hpp"

#include <QUuid>

namespace jam2::practice {
namespace {

void removeDuplicateManagedLanes(QVector<LooperLane>& lanes, const QString& kind)
{
    bool found = false;
    for (int index = 0; index < lanes.size();) {
        if (lanes[index].referenceKind != kind) {
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
        if (lane.referenceKind == kind) return true;
    }
    return false;
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
        if (lanes[candidate].referenceKind == kind) {
            index = candidate;
            break;
        }
    }
    LooperLane lane;
    if (index >= 0) lane = lanes[index];
    else lane.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    lane.name = name;
    lane.assetPath = wav.path;
    lane.assetHash = wav.sha256;
    lane.sampleRate = settings.sampleRate;
    lane.sampleRateCompatible = true;
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
    BeatGridModel nextChord = chordModel;
    BeatGridModel nextBeat = beatModel;
    if (nextChord.replaceGeneratedSection(QStringLiteral("chord"), idea.chordSection) < 0 ||
        nextBeat.replaceGeneratedSection(QStringLiteral("beat"), idea.beatSection) < 0) {
        return std::nullopt;
    }
    chordModel = std::move(nextChord);
    beatModel = std::move(nextBeat);
    return idea;
}

std::optional<SongSection> PracticeIdeaController::generatedSection(
    const BeatGridModel& model,
    const QString& kind)
{
    for (const SongSection& section : model.sections()) {
        if (section.generatedKind == kind) return section;
    }
    return std::nullopt;
}

void PracticeIdeaController::clearReferences(LooperProject& project)
{
    for (LooperBank& bank : project.banks()) {
        for (int index = bank.lanes.size() - 1; index >= 0; --index) {
            if (!bank.lanes[index].referenceKind.isEmpty()) {
                bank.lanes.removeAt(index);
            }
        }
    }
}

bool PracticeIdeaController::applyReferences(
    LooperProject& project,
    int bankIndex,
    const ReferenceRenderSettings& settings,
    const ReferenceRenderResult& result,
    QString& error)
{
    if (bankIndex < 0 || bankIndex >= project.banks().size() || !result.error.isEmpty() ||
        (settings.renderChords && (result.chords.path.isEmpty() || result.chords.sha256.isEmpty())) ||
        (settings.renderDrums && (result.drums.path.isEmpty() || result.drums.sha256.isEmpty())) ||
        (settings.renderMelody && (result.melody.path.isEmpty() || result.melody.sha256.isEmpty()))) {
        error = result.error.isEmpty()
            ? QStringLiteral("The rendered reference assets are incomplete.") : result.error;
        return false;
    }
    QVector<LooperLane> lanes = project.banks().at(bankIndex).lanes;
    removeDuplicateManagedLanes(lanes, QStringLiteral("chord"));
    removeDuplicateManagedLanes(lanes, QStringLiteral("drum"));
    removeDuplicateManagedLanes(lanes, QStringLiteral("melody"));
    const int newLaneCount =
        (settings.renderChords && !hasManagedLane(lanes, QStringLiteral("chord")) ? 1 : 0) +
        (settings.renderDrums && !hasManagedLane(lanes, QStringLiteral("drum")) ? 1 : 0) +
        (settings.renderMelody && !hasManagedLane(lanes, QStringLiteral("melody")) ? 1 : 0);
    if (lanes.size() + newLaneCount > jam2::application::limits::kMaximumLooperLanesPerBank) {
        error = QStringLiteral("The active bank has no room for the reference lanes.");
        return false;
    }
    if (settings.renderChords) {
        upsert(lanes, QStringLiteral("chord"), QStringLiteral("Practice Chords"),
            result.chords, settings, result.sourceSignature);
    }
    if (settings.renderDrums) {
        upsert(lanes, QStringLiteral("drum"), QStringLiteral("Practice Drums"),
            result.drums, settings, result.sourceSignature);
    }
    if (settings.renderMelody) {
        upsert(lanes, QStringLiteral("melody"), QStringLiteral("Practice Melody"),
            result.melody, settings, result.sourceSignature);
    }
    project.banks()[bankIndex].lanes = std::move(lanes);
    return true;
}

} // namespace jam2::practice
