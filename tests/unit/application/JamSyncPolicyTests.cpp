#include "JamSyncPolicy.hpp"

#include <QCoreApplication>
#include <QJsonObject>

#include <array>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

JamSyncPolicy fullyLocalPolicy()
{
    JamSyncPolicy policy;
    policy.trackLanes = false;
    policy.autoShareWavs = false;
    policy.globalPlayback = false;
    policy.generatedIdeas = GeneratedIdeaSyncMode::Off;
    policy.metronomeState = false;
    policy.recordings = false;
    return policy;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    const JamSyncPolicy defaults;
    expect(defaults.trackLanes && defaults.autoShareWavs && defaults.globalPlayback &&
        defaults.generatedIdeas == GeneratedIdeaSyncMode::Full &&
        !defaults.metronomeState && defaults.recordings,
        "defaults preserve sharing while metronome state remains local");

    for (const bool lanes : {false, true}) {
        for (const bool playback : {false, true}) {
            JamSyncPolicy policy;
            policy.trackLanes = lanes;
            policy.globalPlayback = playback;
            policy.recordings = true;
            expect(jam2NormalizeJamSyncPolicy(policy).recordings == (lanes && playback),
                "recording sync requires lane and playback sync");
        }
    }
    for (const bool lanes : {false, true}) {
        for (const bool automatic : {false, true}) {
            JamSyncPolicy policy;
            policy.trackLanes = lanes;
            policy.autoShareWavs = automatic;
            expect(jam2JamSyncAllows(policy, JamSyncRoute::ManualWav),
                "manual WAV sharing is never policy-suppressed");
            expect(jam2JamSyncAllows(policy, JamSyncRoute::AutomaticWav) ==
                (lanes && automatic),
                "automatic WAV sharing follows both dependencies");
        }
    }

    const JamSyncPolicy local = fullyLocalPolicy();
    for (const JamSyncRoute route : {
             JamSyncRoute::TrackLanes, JamSyncRoute::AutomaticWav,
             JamSyncRoute::GlobalPlayback, JamSyncRoute::Recording,
             JamSyncRoute::MetronomeState, JamSyncRoute::IdeaFull,
             JamSyncRoute::IdeaChords, JamSyncRoute::IdeaBeats}) {
        expect(!jam2JamSyncAllows(local, route),
            "fully local policy suppresses automatic routes");
    }

    struct IdeaCase {
        GeneratedIdeaSyncMode mode;
        bool full;
        bool chords;
        bool beats;
    };
    for (const IdeaCase& item : std::array{
             IdeaCase{GeneratedIdeaSyncMode::Full, true, true, true},
             IdeaCase{GeneratedIdeaSyncMode::Chords, false, true, false},
             IdeaCase{GeneratedIdeaSyncMode::Beats, false, false, true},
             IdeaCase{GeneratedIdeaSyncMode::Off, false, false, false}}) {
        JamSyncPolicy policy;
        policy.generatedIdeas = item.mode;
        expect(jam2JamSyncAllows(policy, JamSyncRoute::IdeaFull) == item.full &&
            jam2JamSyncAllows(policy, JamSyncRoute::IdeaChords) == item.chords &&
            jam2JamSyncAllows(policy, JamSyncRoute::IdeaBeats) == item.beats,
            "generated idea mode routes only the selected content");
    }

    JamSyncPolicy serialized;
    serialized.trackLanes = false;
    serialized.recordings = true;
    serialized.generatedIdeas = GeneratedIdeaSyncMode::Chords;
    serialized.revision = 3;
    const QJsonObject set = jam2JamSyncPolicyMessage(QStringLiteral("jam.sync.set"), serialized);
    expect(set.value(QStringLiteral("revision")).toInt() == 3 &&
        !set.value(QStringLiteral("recordings")).toBool() &&
        set.value(QStringLiteral("generated_ideas")).toString() == QStringLiteral("chords"),
        "authoritative serialization carries revision and normalized values");
    JamSyncPolicy parsed;
    QString error;
    expect(jam2ParseJamSyncPolicyMessage(set, parsed, error) && parsed.revision == 3 &&
        parsed == jam2NormalizeJamSyncPolicy(serialized),
        "current authoritative message round-trips");

    QJsonObject missingRevision = set;
    missingRevision.remove(QStringLiteral("revision"));
    expect(!jam2ParseJamSyncPolicyMessage(missingRevision, parsed, error),
        "authoritative policy requires a revision");
    QJsonObject zeroRevision = set;
    zeroRevision.insert(QStringLiteral("revision"), 0);
    expect(!jam2ParseJamSyncPolicyMessage(zeroRevision, parsed, error),
        "authoritative policy rejects revision zero");
    QJsonObject request = jam2JamSyncPolicyMessage(QStringLiteral("jam.sync.request"), serialized);
    expect(!request.contains(QStringLiteral("revision")) &&
        jam2ParseJamSyncPolicyMessage(request, parsed, error),
        "policy request omits authority revision");
    request.insert(QStringLiteral("revision"), 3);
    expect(!jam2ParseJamSyncPolicyMessage(request, parsed, error),
        "policy request cannot assign authority revision");
    QJsonObject extra = set;
    extra.insert(QStringLiteral("legacy_mode"), true);
    expect(!jam2ParseJamSyncPolicyMessage(extra, parsed, error),
        "policy accepts only the one current field set");

    JamSyncPolicyState creator;
    creator.prepareCreatorSession();
    std::array<JamSyncPolicyState, 3> joiners;
    for (auto& joiner : joiners) joiner.prepareJoinerSession();
    JamSyncPolicy proposal = creator.policy();
    proposal.autoShareWavs = false;
    proposal.generatedIdeas = GeneratedIdeaSyncMode::Chords;
    proposal.metronomeState = true;
    const JamSyncPolicy ordered = creator.order(proposal);
    expect(ordered.revision == 2, "creator orders the first peer proposal monotonically");
    for (auto& joiner : joiners) {
        expect(joiner.adopt(ordered), "all three joiners adopt the authoritative policy");
        expect(joiner.policy() == ordered, "all four peers converge on one policy");
    }
    JamSyncPolicy stale = ordered;
    stale.revision = 1;
    stale.globalPlayback = false;
    expect(!creator.adopt(stale), "creator rejects stale authoritative policy");
    stale.revision = 0;
    expect(!JamSyncPolicyState{}.adopt(stale),
        "policy state rejects non-authoritative revision zero even outside parsing");
    stale.revision = 1;
    for (auto& joiner : joiners) {
        expect(!joiner.adopt(stale) && joiner.policy() == ordered,
            "joiners reject stale policy without changing state");
    }
    JamSyncPolicy secondProposal = ordered;
    secondProposal.globalPlayback = false;
    secondProposal.recordings = true;
    const JamSyncPolicy secondOrdered = creator.order(secondProposal);
    expect(secondOrdered.revision == 3 && !secondOrdered.recordings,
        "second peer proposal is ordered and normalized");
    for (auto& joiner : joiners) expect(joiner.adopt(secondOrdered),
        "four-peer reconciliation adopts the second ordered policy");

    JamSyncPolicy routes = fullyLocalPolicy();
    expect(!jam2JamSyncAllowsControlMessage(routes,
        {{QStringLiteral("type"), QStringLiteral("song.set")},
         {QStringLiteral("sync_scope"), QStringLiteral("tracks")}}),
        "track snapshot follows lane policy");
    expect(jam2JamSyncAllowsControlMessage(routes,
        {{QStringLiteral("type"), QStringLiteral("looper.track.share.request")}}),
        "manual track sharing remains available");
    expect(!jam2JamSyncAllowsControlMessage(routes,
        {{QStringLiteral("type"), QStringLiteral("bank.switch")}}),
        "bank switching follows playback policy");
    expect(!jam2JamSyncAllowsControlMessage(routes,
        {{QStringLiteral("type"), QStringLiteral("song.set")},
         {QStringLiteral("sync_scope"), QStringLiteral("unknown")}}),
        "unknown song sync scopes fail closed");

    if (failures != 0) {
        std::cerr << failures << " Jam Sync policy checks failed\n";
        return 1;
    }
    std::cout << "Jam Sync policy checks passed\n";
    return 0;
}
