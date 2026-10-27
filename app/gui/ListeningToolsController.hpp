#pragma once

#include "PlaybackGrid.hpp"
#include "PreparedMixRenderer.hpp"
#include "SharedTrackModel.hpp"
#include "engine.hpp"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>
#include <optional>
#include <map>

namespace jam2::gui {

struct ListeningToolsSettings {
    int speedPpm = 1000000;
    int pitchCents = 0;
    bool focusEnabled = false;
    QString focusPreset = QStringLiteral("custom");
    double focusHz = 120.0;
    double focusGainDb = 12.0;
    double focusQ = 6.0;
    friend bool operator==(const ListeningToolsSettings&, const ListeningToolsSettings&) = default;
    static ListeningToolsSettings fromModel(const SharedTrackModel& model);
    void applyTo(SharedTrackModel& model) const;
};

QJsonObject listeningSettingsJson(const ListeningToolsSettings& settings);
bool parseListeningSettings(const QJsonObject& object, ListeningToolsSettings& settings, QString& error);
bool validateListeningMessage(const QJsonObject& message, QString& error, bool serverBinary = false);

// GUI/control-thread owner. Preparation runs through the existing file worker;
// only fixed-shape staged swaps and tempo commands enter the audio engine.
class ListeningToolsController final : public QObject {
public:
    enum class Phase { Idle, Preparing, Loading, Ready, Scheduled };
    struct Context {
        bool sync = true;
        bool network = false;
        bool creator = true;
        bool serverBinary = false;
        bool engineRunning = false;
        bool protectedRecording = false;
        bool transportChanging = false;
        bool playing = false;
        bool hasSources = false;
        bool contentReady = true;
        double sourceSeconds = 0.0;
        bool loopEnabled = true;
        double loopStartSourceSeconds = -1.0;
        double loopEndSourceSeconds = -1.0;
        int bank = 0;
        QString localToken;
        QSet<QString> peers;
        QString sourceSignature;
        PlaybackGrid::Position position;
        jam2::EngineSnapshot engine;
    };
    struct Callbacks {
        std::function<Context()> context;
        std::function<bool(const QJsonObject&, const QString&)> send;
        std::function<bool(const ListeningToolsSettings&, int, int)> prepare;
        std::function<bool(const jam2::EngineCommand&)> submit;
        std::function<void(const ListeningToolsSettings&, const std::optional<PreparedMixResult>&)> applied;
        std::function<void(const ListeningToolsSettings&)> selected;
        std::function<void(const ListeningToolsSettings&, const QString&)> rejected;
        std::function<void(const QString&)> log;
    };
    static constexpr int kDebounceMs = 200;
    static constexpr int kCommitLeadMs = 750;
    static constexpr int kAudioPublicationGraceMs = 200;
    static constexpr int kPreparationTimeoutMs = 15000;
    explicit ListeningToolsController(QObject* parent = nullptr);
    ~ListeningToolsController() override;
    void setCallbacks(Callbacks callbacks);
    void initialize(const ListeningToolsSettings& settings);
    void edit(ListeningToolsSettings settings, bool commitNow = false);
    void flushEdits();
    void policyChanged(bool previouslySynced);
    void receive(const QJsonObject& message, const QString& source);
    void prepared(int revision, PreparedMixResult result);
    bool engineEvent(const jam2::EngineEvent& event);
    void sendCurrent(const QString& peer);
    void reset();
    void tick();
    // Content owner calls before clearing/replacing the prepared source. A
    // Server selection survives invalidation even after its planned boundary.
    void sourceWillChange();
    const ListeningToolsSettings& appliedSettings() const noexcept { return applied_; }
    const ListeningToolsSettings& selectedSettings() const noexcept { return draft_; }
    bool playbackReady() const noexcept {
        return phase_ == Phase::Idle && !debounce_.isActive() && !queuedEdit_ && draft_ == applied_;
    }
    bool pending() const noexcept { return phase_ != Phase::Idle; }
    bool pendingForBank(int bank) const noexcept { return pending() && bank_ == bank; }
    QJsonObject stats() const;
private:
    struct ServerSelection {
        ListeningToolsSettings settings;
        int revision = 0, bank = 0;
        jam2::PlaybackTempo tempo;
        std::uint64_t target = 0, epoch = 0;
        double epochPosition = 0.0, sourceSeconds = 0.0;
        bool playing = false;
    };
    bool publishServerSelection(quint8 changedGroups, const ListeningToolsSettings& proposed);
    void adoptServerSelection(const ServerSelection& selection);
    bool deferServerSelectionForContentChange();
    QJsonObject serverSelectionMessage(const ServerSelection& selection) const;
    std::optional<ServerSelection> serverSelection_, deferredServerSelection_;
    quint64 serverLateReschedules_ = 0;
    quint64 serverContentDeferrals_ = 0;
    quint64 serverSourceInvalidations_ = 0, serverPostBoundaryDeferrals_ = 0;
    quint8 serverEditGroups_ = 0;
    quint64 serverPeerFailures_ = 0;
    struct ServerApplication { int revision = 0; std::uint64_t frame = 0; };
    std::optional<ServerApplication> pendingServerApplication_;
    std::optional<ServerApplication> lastServerApplication_;
    std::map<QString, ServerApplication> serverApplications_;
    void request();
    void begin(int revision, ListeningToolsSettings settings, int bank, bool shared);
    void ready();
    void maybeCommit();
    bool schedule(const QJsonObject& message);
    void cancel(const QString& error, bool broadcast, bool reportFailure = true);
    void applyReset(const QJsonObject& message);
    QJsonObject resetMessage(const Context& context) const;
    Context context() const;
    bool send(QJsonObject message, const QString& peer = {});

    Callbacks callbacks_;
    QTimer debounce_;
    QTimer poll_;
    QElapsedTimer preparationTime_;
    Phase phase_ = Phase::Idle;
    ListeningToolsSettings draft_;
    ListeningToolsSettings applied_;
    ListeningToolsSettings preparing_;
    int revision_ = 0;
    int appliedRevision_ = 0;
    int appliedSharedRevision_ = 0;
    int highestAuthoritativeRevision_ = 0;
    int prepareGeneration_ = 0;
    int bank_ = 0;
    bool shared_ = false;
    bool coupled_ = true;
    bool queuedEdit_ = false;
    QString sourceSignature_;
    QSet<QString> expected_;
    QSet<QString> ready_;
    std::optional<PreparedMixResult> prepared_;
    std::optional<QJsonObject> deferredState_;
    std::optional<jam2::EngineCommand> pendingReset_;
    std::uint64_t stageCookie_ = 0;
    std::uint64_t stageSerial_ = 0;
    std::uint64_t commitRawFrame_ = 0;
    jam2::PlaybackTempo committedTempo_{};
    QString error_;
    qint64 renderMs_ = 0;
};

} // namespace jam2::gui
