#include "ConcurrentLooperMerge.hpp"
#include "ContentLimits.hpp"
#include "LooperProject.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QJsonArray firstBankLanes(const QJsonObject& project)
{
    return project.value(QStringLiteral("banks")).toArray().first().toObject()
        .value(QStringLiteral("lanes")).toArray();
}

QJsonObject laneObject(const QString& id, const QString& hash, const QString& name)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("asset_hash"), hash},
        {QStringLiteral("name"), name},
        {QStringLiteral("sample_rate"), hash.isEmpty() ? 0 : 48000},
        {QStringLiteral("source_frames"), QStringLiteral("1024")},
        {QStringLiteral("start_frame"), QStringLiteral("0")},
        {QStringLiteral("stop_frame"), QStringLiteral("-1")},
        {QStringLiteral("loop_start_frame"), QStringLiteral("-1")},
        {QStringLiteral("loop_end_frame"), QStringLiteral("-1")},
        {QStringLiteral("loop_enabled"), false},
        {QStringLiteral("reference_kind"), QString{}},
        {QStringLiteral("reference_source_signature"), QString{}},
        {QStringLiteral("reference_bpm"), 0.0},
        {QStringLiteral("reference_stale"), false},
    };
}

QJsonObject songWithLanes(const QJsonArray& lanes)
{
    return {{QStringLiteral("looper"), QJsonObject{
        {QStringLiteral("banks"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("bank-a")},
            {QStringLiteral("lanes"), lanes},
        }}},
        {QStringLiteral("active_bank"), 0},
    }}};
}

void expectLoadable(const LooperProject& project, const char* message)
{
    LooperProject loaded;
    expect(loaded.loadJson(project.toJson()), message);
}

void testBankAndArrangementOwnership()
{
    LooperProject project;
    expect(project.banks().size() == 4 && project.activeBankIndex() == 0 &&
            project.gridLockEnabled() && project.trackSyncEnabled() &&
            project.hasSerializedTiming(),
        "project defaults own four banks and current local timing state");
    project.setGridLockEnabled(false);
    project.setTrackSyncEnabled(false);
    project.setActiveBankIndex(-5);
    expect(!project.gridLockEnabled() && !project.trackSyncEnabled() &&
            project.activeBankIndex() == 0,
        "local project toggles and negative active-bank clamp are owned");

    LooperBankTiming bankA;
    bankA.bpm = 137;
    bankA.beatsPerBar = 7;
    bankA.beatUnit = 8;
    bankA.tempoPulseUnits = 2;
    bankA.division = 4;
    expect(project.setTiming(0, bankA) &&
            project.resolvedTiming(1).bpm == 137 &&
            project.resolvedTiming(1).beatsPerBar == 7 &&
            project.resolvedTiming(1).inheritsBankA,
        "inheriting banks resolve the exact Bank A timing");
    LooperBankTiming independent = bankA;
    independent.bpm = 93;
    independent.inheritsBankA = false;
    expect(project.setTiming(1, independent) &&
            project.resolvedTiming(1).bpm == 93 &&
            !project.resolvedTiming(1).inheritsBankA &&
            !project.setTiming(-1, independent) &&
            !project.setTiming(project.banks().size(), independent),
        "independent timing and invalid bank rejection are deterministic");

    while (project.addBank()) {}
    expect(project.banks().size() == 12 && !project.addBank(),
        "bank creation stops at the maintained twelve-Section limit");
    project.setActiveBankIndex(999);
    expect(project.activeBankIndex() == 11,
        "active bank clamps to the last available Section");
    ArrangementDefinition referenced;
    referenced.steps = {{11, 1}};
    expect(project.setArrangement(referenced) && !project.removeLastBank(),
        "a referenced final bank cannot be removed");
    expect(project.setArrangement({}) && project.removeLastBank(),
        "an unreferenced final bank can be removed");
    while (project.removeLastBank()) {}
    expect(project.banks().size() == 4 && !project.removeLastBank() &&
            project.activeBankIndex() == 3,
        "bank removal stops at four and clamps the active bank");
    project.setArrangementEnabled(true);
    expect(project.arrangement().enabled,
        "arrangement active state is owned independently of its rows");
    expectLoadable(project, "bank/timing state remains loadable after every mutation");
}

void testValidatedLaneMutations()
{
    LooperProject project;
    LooperLane lane;
    lane.id = QStringLiteral("editable-lane");
    lane.assetPath = QStringLiteral("recorded/editable.wav");
    lane.assetHash = QString(64, QLatin1Char('d'));
    lane.name = QStringLiteral("Editable");
    lane.sampleRate = 48000;
    lane.sourceFrames = 1000;
    lane.startFrame = 50;
    lane.originKind = QStringLiteral("recorded");
    expect(project.appendLane(0, lane), "valid editable lane is accepted");
    expect(!project.appendLane(0, lane) && project.banks().at(0).lanes.size() == 1,
        "duplicate nonblank lane identity is rejected without mutation");

    LooperLane replacement = lane;
    replacement.id = QStringLiteral("attempted-identity-change");
    replacement.assetPath = QStringLiteral("imported/replacement.wav");
    replacement.assetHash = QString(64, QLatin1Char('e'));
    replacement.name = QStringLiteral("Replacement");
    replacement.sourceFrames = 1000;
    replacement.originKind = QStringLiteral("imported");
    expect(project.replaceLane(0, 0, replacement) &&
            project.banks().at(0).lanes.at(0).id == QStringLiteral("editable-lane") &&
            project.banks().at(0).lanes.at(0).assetPath == replacement.assetPath &&
            project.banks().at(0).lanes.at(0).assetHash == replacement.assetHash &&
            project.banks().at(0).lanes.at(0).name == replacement.name,
        "checked lane replacement retains stable identity and adopts complete state");
    const QJsonObject beforeRejectedReplacement = project.toJson();
    replacement.gainDb = std::numeric_limits<double>::infinity();
    expect(!project.replaceLane(0, 0, replacement) &&
            !project.replaceLane(-1, 0, lane) &&
            !project.replaceLane(0, 99, lane) &&
            project.toJson() == beforeRejectedReplacement,
        "invalid lane replacement rejects atomically without partial mutation");

    LooperLane invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.gainDb = std::numeric_limits<double>::quiet_NaN();
    expect(!project.appendLane(0, invalid), "non-finite lane gain is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.sourceFrames = -1;
    expect(!project.appendLane(0, invalid), "negative source length is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.stopFrame = -2;
    expect(!project.appendLane(0, invalid), "noncanonical negative stop sentinel is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.stopFrame = invalid.startFrame;
    expect(!project.appendLane(0, invalid), "zero-length explicit timeline stop is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.loopStartFrame = 0;
    invalid.loopEndFrame = -1;
    expect(!project.appendLane(0, invalid), "half-specified source crop is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.loopStartFrame = 999;
    invalid.loopEndFrame = 1001;
    expect(!project.appendLane(0, invalid), "source crop beyond known WAV is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.sampleRate = jam2::application::limits::kMinimumSampleRate - 1;
    expect(!project.appendLane(0, invalid), "sub-8 kHz lane sample rate is rejected");
    invalid = lane;
    invalid.id = QStringLiteral("invalid");
    invalid.sampleRate = jam2::application::limits::kMaximumSampleRate + 1;
    expect(!project.appendLane(0, invalid), "sample rate beyond the maintained limit is rejected");

    LooperLane longFile;
    longFile.id = QStringLiteral("long-file");
    longFile.assetPath = QStringLiteral("C:/") + QString(600, QLatin1Char('x')) +
        QStringLiteral(".wav");
    expect(project.appendLane(0, longFile) &&
            project.banks().at(0).lanes.last().name.size() ==
                jam2::application::limits::kMaximumLooperNameCharacters,
        "default lane name is bounded even when a valid path has a longer filename");

    expect(project.setLaneGainDb(0, 0, -100.0) &&
            project.banks().at(0).lanes.at(0).gainDb == -60.0 &&
            project.setLaneGainDb(0, 0, 100.0) &&
            project.banks().at(0).lanes.at(0).gainDb == 12.0,
        "interactive lane gain clamps to the visible -60..12 dB range");
    const double retainedGain = project.banks().at(0).lanes.at(0).gainDb;
    expect(!project.setLaneGainDb(
                0, 0, std::numeric_limits<double>::infinity()) &&
            project.banks().at(0).lanes.at(0).gainDb == retainedGain,
        "non-finite gain edit rejects without mutation");
    expect(project.setLaneMuted(0, 0, true) &&
            project.setLaneSolo(0, 0, true) &&
            project.banks().at(0).lanes.at(0).muted &&
            project.banks().at(0).lanes.at(0).solo &&
            !project.setLaneMuted(-1, 0, false) &&
            !project.setLaneSolo(0, 99, false),
        "mute and solo edits validate lane identity");

    const LooperLaneRegion cropped{100, 700, 100, 700, true};
    expect(project.setLaneRegion(0, 0, cropped),
        "bounded timeline placement and source crop are accepted");
    const QJsonObject beforeInvalidRegion = project.toJson();
    expect(!project.setLaneRegion(0, 0, LooperLaneRegion{100, 100, 100, 700, true}) &&
            !project.setLaneRegion(0, 0, LooperLaneRegion{100, 700, -1, 700, true}) &&
            !project.setLaneRegion(0, 0, LooperLaneRegion{100, 700, 100, 1001, true}) &&
            !project.setLaneRegion(-1, 0, cropped) &&
            project.toJson() == beforeInvalidRegion,
        "zero-length, half-cropped, out-of-source, and invalid-index regions reject atomically");
    expect(project.setLaneRegion(
                0, 0, LooperLaneRegion{25, 525, -1, -1, false}) &&
            project.banks().at(0).lanes.at(0).loopStartFrame == -1 &&
            project.banks().at(0).lanes.at(0).loopEndFrame == -1,
        "a complete-source placement retains canonical crop sentinels");

    const QString retainedName = project.banks().at(0).lanes.at(0).name;
    expect(project.clearLaneAsset(0, 0) &&
            project.banks().at(0).lanes.at(0).assetPath.isEmpty() &&
            project.banks().at(0).lanes.at(0).assetHash.isEmpty() &&
            project.banks().at(0).lanes.at(0).sourceFrames == 0 &&
            project.banks().at(0).lanes.at(0).startFrame == 25 &&
            project.banks().at(0).lanes.at(0).gainDb == retainedGain &&
            project.banks().at(0).lanes.at(0).muted &&
            project.banks().at(0).lanes.at(0).solo &&
            project.banks().at(0).lanes.at(0).name == retainedName,
        "clearing WAV ownership preserves track name, mixer state, and placement start");
    expect(project.clearLaneAsset(0, 0, true) &&
            project.banks().at(0).lanes.at(0).startFrame == 0 &&
            !project.clearLaneAsset(99, 0),
        "destructive Section crop can explicitly reset placement start");

    expect(!project.renameLane(0, 0, QString(513, QLatin1Char('n'))) &&
            !project.renameLane(0, 0, QStringLiteral("  ")) &&
            project.renameLane(0, 0, QStringLiteral("  Renamed  ")) &&
            project.banks().at(0).lanes.at(0).name == QStringLiteral("Renamed"),
        "lane rename enforces the stored name limit and trims accepted text");
    expectLoadable(project, "all accepted lane edits serialize into a loadable current project");
}

void testLaneTimelineEnd()
{
    LooperLane lane;
    lane.startFrame = 10;
    lane.sampleRate = 44100;
    expect(jam2::gui::looperLaneTimelineEnd(lane, 441, 48000) == 490,
        "source duration converts to the prepared timeline rate");
    lane.loopStartFrame = 100;
    lane.loopEndFrame = 300;
    expect(jam2::gui::looperLaneTimelineEnd(lane, 441, 44100) == 210,
        "explicit source crop defines the natural timeline end");
    lane.stopFrame = 150;
    expect(jam2::gui::looperLaneTimelineEnd(lane, 441, 44100) == 150,
        "explicit timeline stop overrides natural source duration");
    lane.stopFrame = -1;
    lane.startFrame = jam2::application::limits::kMaximumLooperTimelineFrames;
    lane.loopStartFrame = -1;
    lane.loopEndFrame = -1;
    expect(jam2::gui::looperLaneTimelineEnd(
                lane,
                jam2::application::limits::kMaximumAssetFrames,
                jam2::application::limits::kMaximumSampleRate) ==
            jam2::application::limits::kMaximumLooperTimelineFrames,
        "natural timeline end saturates at the maintained frame limit");
    lane.startFrame = 123;
    expect(jam2::gui::looperLaneTimelineEnd(lane, 0, 48000) == 123 &&
            jam2::gui::looperLaneTimelineEnd(lane, 100, 0) == 123,
        "unknown source duration or invalid timeline clock retains the placement start");
}

void testSectionLaneCropping()
{
    LooperProject project;
    LooperLane natural;
    natural.id = QStringLiteral("natural-crop");
    natural.assetPath = QStringLiteral("imported/natural.wav");
    natural.assetHash = QString(64, QLatin1Char('a'));
    natural.name = QStringLiteral("Natural crop");
    natural.sampleRate = 48000;
    natural.sourceFrames = 1000;
    natural.startFrame = 100;

    LooperLane looped = natural;
    looped.id = QStringLiteral("looped-crop");
    looped.assetPath = QStringLiteral("imported/looped.wav");
    looped.assetHash = QString(64, QLatin1Char('b'));
    looped.name = QStringLiteral("Looped crop");
    looped.stopFrame = 900;
    looped.loopStartFrame = 100;
    looped.loopEndFrame = 300;
    looped.loopEnabled = true;

    LooperLane removed = natural;
    removed.id = QStringLiteral("removed-placement");
    removed.assetPath = QStringLiteral("imported/removed.wav");
    removed.assetHash = QString(64, QLatin1Char('c'));
    removed.name = QStringLiteral("Removed placement");
    removed.startFrame = 700;
    removed.gainDb = -9.0;
    removed.solo = true;
    expect(project.appendLane(0, natural) && project.appendLane(0, looped) &&
            project.appendLane(0, removed),
        "Section crop fixtures are valid");

    const LooperLaneTimelineCropResult naturalResult =
        project.cropLaneToTimelineEnd(0, 0, 1000, 48000, 600);
    const LooperLaneTimelineCropResult loopedResult =
        project.cropLaneToTimelineEnd(0, 1, 1000, 48000, 500);
    const LooperLaneTimelineCropResult removedResult =
        project.cropLaneToTimelineEnd(0, 2, 1000, 48000, 600);
    const LooperLane& naturalAfter = project.banks().at(0).lanes.at(0);
    const LooperLane& loopedAfter = project.banks().at(0).lanes.at(1);
    const LooperLane& removedAfter = project.banks().at(0).lanes.at(2);
    expect(naturalResult.status == LooperLaneTimelineCropStatus::Cropped &&
            naturalAfter.startFrame == 100 && naturalAfter.stopFrame == 600 &&
            naturalAfter.loopStartFrame == 0 && naturalAfter.loopEndFrame == 500 &&
            !naturalAfter.loopEnabled,
        "non-looping lane crop retains only source frames before the new Section end");
    expect(loopedResult.status == LooperLaneTimelineCropStatus::Cropped &&
            loopedAfter.stopFrame == 500 && loopedAfter.loopStartFrame == 100 &&
            loopedAfter.loopEndFrame == 300 && loopedAfter.loopEnabled,
        "shortening a looping placement preserves its source loop and changes only output stop");
    expect(removedResult.status == LooperLaneTimelineCropStatus::Cleared &&
            removedResult.removedAssetPath == QStringLiteral("imported/removed.wav") &&
            removedResult.removedAssetHash == QString(64, QLatin1Char('c')) &&
            removedAfter.assetPath.isEmpty() && removedAfter.assetHash.isEmpty() &&
            removedAfter.startFrame == 0 && removedAfter.name == QStringLiteral("Removed placement") &&
            removedAfter.gainDb == -9.0 && removedAfter.solo,
        "placement wholly after the new end reports and clears WAV ownership while retaining mixer identity");

    const QJsonObject beforeRejected = project.toJson();
    expect(project.cropLaneToTimelineEnd(0, 0, 1000, 48000, 700).status ==
                LooperLaneTimelineCropStatus::Unchanged &&
            project.cropLaneToTimelineEnd(0, 0, 1000, 7999, 500).status ==
                LooperLaneTimelineCropStatus::Rejected &&
            project.cropLaneToTimelineEnd(99, 0, 1000, 48000, 500).status ==
                LooperLaneTimelineCropStatus::Rejected &&
            project.toJson() == beforeRejected,
        "unchanged and invalid Section crops leave the complete project untouched");
    expectLoadable(project, "Section-cropped lane state remains current-format loadable");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    testBankAndArrangementOwnership();
    testValidatedLaneMutations();
    testLaneTimelineEnd();
    testSectionLaneCropping();

    LooperProject first;
    LooperProject second;
    first.ensureInitialEmptyLanes();
    second.ensureInitialEmptyLanes();
    expect(first.toJson(true) == second.toJson(true),
        "initial visible lanes have deterministic shared identities");
    const QJsonArray initial = firstBankLanes(first.toJson(true));
    expect(initial.size() == 1 &&
        initial.first().toObject().value(QStringLiteral("id")).toString() ==
            QStringLiteral("initial-empty-0"),
        "bank A exposes its stable initial placeholder");

    LooperProject project;
    LooperLane lane;
    lane.id = QStringLiteral("shared-audio");
    lane.assetPath = QStringLiteral("C:/machine-private/workspace/take.wav");
    lane.assetHash = QString(64, QLatin1Char('a'));
    lane.name = QStringLiteral("Shared take");
    lane.sampleRate = 48000;
    expect(project.appendLane(0, lane), "fixture lane is accepted");
    const QJsonObject persisted = firstBankLanes(project.toJson()).last().toObject();
    const QJsonObject shared = firstBankLanes(project.toJson(true)).last().toObject();
    expect(persisted.value(QStringLiteral("asset_path")).toString() == lane.assetPath,
        "local persistence retains the asset path");
    expect(!shared.contains(QStringLiteral("asset_path")) &&
        shared.value(QStringLiteral("asset_hash")).toString() == lane.assetHash,
        "shared serialization carries content identity without a machine-local path");
    LooperProject received;
    expect(received.loadJson(project.toJson(true)) &&
        received.banks().at(0).lanes.last().assetPath.isEmpty() &&
        received.banks().at(0).lanes.last().assetHash == lane.assetHash,
        "path-free shared serialization remains loadable and hash-addressed");

    ArrangementDefinition arrangement;
    arrangement.steps = {{1, 2}, {2, 3}};
    arrangement.loop = false;
    arrangement.enabled = true;
    expect(project.setArrangement(arrangement),
        "valid arrangement rows are accepted");
    LooperProject arrangementRoundTrip;
    expect(arrangementRoundTrip.loadJson(project.toJson()) &&
            arrangementRoundTrip.arrangement().steps.size() == 2 &&
            arrangementRoundTrip.arrangement().steps.at(0).bankIndex == 1 &&
            arrangementRoundTrip.arrangement().steps.at(0).repeats == 2 &&
            arrangementRoundTrip.arrangement().steps.at(1).bankIndex == 2 &&
            arrangementRoundTrip.arrangement().steps.at(1).repeats == 3 &&
            !arrangementRoundTrip.arrangement().loop &&
            arrangementRoundTrip.arrangement().enabled,
        "arrangement rows, order, loop, and local active state round-trip");
    const QJsonObject sharedArrangement = project.toJson(true)
        .value(QStringLiteral("arrangement")).toObject();
    expect(!sharedArrangement.contains(QStringLiteral("enabled")) &&
            sharedArrangement.value(QStringLiteral("steps")).toArray().size() == 2,
        "shared arrangement excludes machine-local playback state");

    ArrangementDefinition invalidArrangement = arrangement;
    invalidArrangement.steps[0].bankIndex = project.banks().size();
    expect(!project.setArrangement(invalidArrangement) &&
            project.arrangement().steps.size() == 2 &&
            project.arrangement().steps.at(0).bankIndex == 1,
        "invalid bank reference is rejected without mutating the arrangement");
    invalidArrangement = arrangement;
    invalidArrangement.steps[0].repeats = 0;
    expect(!project.setArrangement(invalidArrangement),
        "zero arrangement repeats are rejected");
    invalidArrangement = arrangement;
    invalidArrangement.steps[0].repeats = 65;
    expect(!project.setArrangement(invalidArrangement),
        "arrangement repeats above the editor bound are rejected");
    invalidArrangement = arrangement;
    invalidArrangement.steps.fill(ArrangementStep{}, 65);
    expect(!project.setArrangement(invalidArrangement),
        "more than 64 arrangement rows are rejected");

    const QString sameHash(64, QLatin1Char('b'));
    const QJsonObject emptyLane = laneObject(
        QStringLiteral("initial-empty-0"), {}, QStringLiteral("Empty Track 1"));
    const QJsonObject currentSame = laneObject(
        QStringLiteral("current-import"), sameHash, QStringLiteral("Same A"));
    const QJsonObject proposedSame = laneObject(
        QStringLiteral("proposed-import"), sameHash, QStringLiteral("Same B"));
    const QJsonObject baseSong = songWithLanes({emptyLane});
    const QJsonObject concurrentSame = mergeConcurrentLooperMetadata(
        baseSong,
        songWithLanes({emptyLane, currentSame}),
        songWithLanes({emptyLane, proposedSame}));
    const QJsonArray concurrentSameLanes = firstBankLanes(
        concurrentSame.value(QStringLiteral("looper")).toObject());
    expect(concurrentSameLanes.size() == 2 &&
            concurrentSameLanes.at(1).toObject().value(QStringLiteral("id")) ==
                QStringLiteral("current-import"),
        "concurrent branches introducing identical bytes retain one accepted lane");

    const QJsonObject baseWithSame = songWithLanes({emptyLane, currentSame});
    const QJsonObject deliberateLaterDuplicate = mergeConcurrentLooperMetadata(
        baseWithSame,
        songWithLanes({emptyLane, currentSame, proposedSame}),
        baseWithSame);
    expect(firstBankLanes(
               deliberateLaterDuplicate.value(QStringLiteral("looper")).toObject()).size() == 3,
        "deduplication does not collapse a duplicate introduced by a later sequential edit");

    const QJsonObject different = laneObject(
        QStringLiteral("different-import"), QString(64, QLatin1Char('c')),
        QStringLiteral("Different"));
    const QJsonObject concurrentDifferent = mergeConcurrentLooperMetadata(
        baseSong,
        songWithLanes({emptyLane, currentSame}),
        songWithLanes({emptyLane, different}));
    const QJsonArray concurrentDifferentLanes = firstBankLanes(
        concurrentDifferent.value(QStringLiteral("looper")).toObject());
    expect(concurrentDifferentLanes.size() == 3 &&
            concurrentDifferentLanes.at(1).toObject().value(QStringLiteral("id")) ==
                QStringLiteral("current-import") &&
            concurrentDifferentLanes.at(2).toObject().value(QStringLiteral("id")) ==
                QStringLiteral("different-import"),
        "different concurrent imports survive in stable accepted-then-rebased order");

    if (failures != 0) {
        std::cerr << failures << " LooperProject checks failed\n";
        return 1;
    }
    std::cout << "LooperProject checks passed\n";
    return 0;
}
