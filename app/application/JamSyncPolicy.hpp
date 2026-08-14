#pragma once

#include <QJsonObject>
#include <QString>

#include <cstdint>

enum class GeneratedIdeaSyncMode : std::uint8_t {
    // Values are persisted in current user preferences.
    Off = 0,
    Full = 1,
    Chords = 2,
    Beats = 3,
};

enum class JamSyncRoute : std::uint8_t {
    TrackLanes,
    AutomaticWav,
    ManualWav,
    GlobalPlayback,
    Recording,
    MetronomeState,
    IdeaFull,
    IdeaChords,
    IdeaBeats,
};

struct JamSyncPolicy {
    bool trackLanes = true;
    bool autoShareWavs = true;
    bool globalPlayback = true;
    GeneratedIdeaSyncMode generatedIdeas = GeneratedIdeaSyncMode::Full;
    bool metronomeState = false;
    bool recordings = true;
    int revision = 0;

    friend bool operator==(const JamSyncPolicy&, const JamSyncPolicy&) = default;
};

JamSyncPolicy jam2NormalizeJamSyncPolicy(JamSyncPolicy policy) noexcept;
bool jam2JamSyncAllows(const JamSyncPolicy& policy, JamSyncRoute route) noexcept;
QString jam2GeneratedIdeaSyncModeText(GeneratedIdeaSyncMode mode);

// `jam.sync.set` is the authoritative current format and requires a positive
// revision. `jam.sync.request` is a proposal and must not contain a revision.
QJsonObject jam2JamSyncPolicyMessage(const QString& type, const JamSyncPolicy& policy);
bool jam2ParseJamSyncPolicyMessage(
    const QJsonObject& message,
    JamSyncPolicy& policy,
    QString& error);

bool jam2JamSyncAllowsControlMessage(
    const JamSyncPolicy& policy,
    const QJsonObject& message) noexcept;

int jam2NextJamSyncRevision(int current) noexcept;

class JamSyncPolicyState final {
public:
    explicit JamSyncPolicyState(JamSyncPolicy policy = {});

    const JamSyncPolicy& policy() const noexcept;
    void prepareCreatorSession() noexcept;
    void prepareJoinerSession() noexcept;
    JamSyncPolicy order(JamSyncPolicy proposal) noexcept;
    bool adopt(JamSyncPolicy authoritative) noexcept;

private:
    JamSyncPolicy policy_;
};
