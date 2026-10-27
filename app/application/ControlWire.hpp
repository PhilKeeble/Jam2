#pragma once
#include "ControlWireVersion.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>

#include <deque>
#include <functional>
#include <memory>

namespace jam2::application::control_wire {

inline constexpr int kVersion = kControlWireVersion;
inline constexpr qsizetype kRecordHeaderBytes = 32;
inline constexpr qsizetype kMaximumDocumentBytes = 4 * 1024 * 1024;
inline constexpr qsizetype kMaximumQueuedBytes = 5 * 1024 * 1024;
inline constexpr qsizetype kStreamQueueBytes = 256 * 1024;

// Stable assignments. These are control/WAV operations, never media opcodes.
enum class Operation : quint16 {
    Contract = 1, Membership, Heartbeat, HeartbeatReply, Introduce, Repair,
    PairFailure, Endpoint, End, Error,
    SyncSet = 16, SyncRequest, MetronomeSet, MetronomeRequest,
    ListeningRequest = 24, ListeningFailed = 27, ListeningCancel = 29,
    ListeningState, ListeningReset,
    ListeningApplied = 32,
    Cell = 40, Hit, BeatDivision, MusicDivision, MusicStep, Resize, Song, Render,
    BankRequest = 56, BankPrepare, BankReady, BankCancel, BankSwitch,
    ShareRequest = 64, ShareOffer, ShareComplete, RecordingState, GroupStart,
    GroupFinish, GroupRecoverRequest, GroupRecover, ResyncRequest, ResyncState,
    AssetRequest = 80, AssetStart, AssetProgress, AssetDone, AssetUnavailable,
    AssetChunk = 96, Receipt, Binary,
};

enum class ReceiptStage : quint8 { Dispatched = 1, Rejected = 2 };
enum class Rejection : quint8 { None = 0, InvalidValue = 1, Unauthorized = 2 };
struct DispatchDecision {
    Rejection rejection = Rejection::None;
    QString reason;
};

struct Stats {
    quint64 recordsSent = 0, recordsReceived = 0, wireBytesSent = 0;
    quint64 receiptsSent = 0, receiptsReceived = 0, lastDispatched = 0;
    quint64 checkpointsSent = 0, deltasSent = 0, savedSnapshotBytes = 0;
    quint64 assetHeaderBytesSaved = 0, queueRejects = 0, pendingReceipts = 0;
    quint64 assetMetadataBytesSaved = 0;
    quint64 queuedBytes = 0, queuedHighWaterBytes = 0;
    quint64 encodingRejects = 0, unresolvedReceipts = 0;
    quint64 operationsRejected = 0, rejectionResultsReceived = 0;
    quint64 resultLatencySamples = 0, resultLatencySumUs = 0, resultLatencyMaxUs = 0;
    quint64 compressionAttempts = 0, compressionSkipped = 0;
    quint64 transportWrites = 0, batchedRecords = 0, coalescedUpdates = 0, cumulativeReceipts = 0;
    quint64 independentRecordsSent = 0, independentRecordsReceived = 0, independentQueuedBytes = 0;
    quint64 assignmentDelaySamples = 0, assignmentDelaySumUs = 0, assignmentDelayMaxUs = 0;
    quint64 typedManifests = 0, manifestDocumentFallbacks = 0;
    quint64 typedRegions = 0, typedStructures = 0;
    quint64 peerDefinitions = 0, peerReferences = 0;
    QString lastRejection;
};
void accumulate(Stats& target, const Stats& value) noexcept;
QJsonObject diagnostics(const Stats& value);

// Serialization boundary for existing Qt models. Ordinary messages have explicit
// operation schemas; structured snapshots use bounded CBOR only at this boundary.
// Each authenticated logical stream owns its own delta and transfer bindings.
class Codec {
public:
    struct Options {
        bool numericDocuments = true;
        qsizetype compressionMinimumBytes = 128;
        bool typedRenames = true, typedManifests = true, typedRegions = true, typedStructures = true;
        bool peerHandles = true;
    };
    struct Encoded {
        Operation operation{};
        QByteArray payload;
        bool capacityExceeded = false;
    };
    struct Decoded {
        QJsonObject message;
        QByteArray asset;
        quint64 id = 0;
        bool receipt = false;
        ReceiptStage stage = ReceiptStage::Dispatched;
        Rejection rejection = Rejection::None;
        QString reason;
    };

    Codec();
    explicit Codec(Options options);
    ~Codec();
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    bool encode(const QJsonObject& message, Encoded& out, QString& error,
        qsizetype maximumWireBytes = kMaximumQueuedBytes);
    bool encodeAsset(const QByteArray& legacyChunk, Encoded& out, QString& error);
    bool decode(Operation operation, const QByteArray& payload, Decoded& out, QString& error);
    const Stats& stats() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

QByteArray authenticate(Operation operation, quint64 sequence,
    const QByteArray& payload, const QByteArray& key, quint8 flags = 0);
bool verify(const QByteArray& body, quint64 sequence, const QByteArray& key,
    Operation& operation, quint8& flags, QByteArray& payload, QString& error);

// Bounded application output and receipt tracking above an unchanged reliable
// stream. A receipt says dispatch completed, never that a domain action committed.
class Channel final : public QObject {
public:
    using Write = std::function<bool(const QByteArray&)>;
    using Capacity = std::function<qint64()>;
    Channel(QByteArray sendKey, QByteArray receiveKey, Write write, Capacity pending,
        QObject* parent = nullptr);
    struct Options { bool batch = true; bool coalesce = true; bool cumulativeReceipts = true; };
    Channel(QByteArray sendKey, QByteArray receiveKey, Write write, Capacity pending,
        Options options, QObject* parent = nullptr);
    bool send(const QJsonObject& message);
    bool sendAsset(const QByteArray& payload);
    bool sendBinary(const QByteArray& payload);
    bool receive(const QByteArray& body, QString& error);
    bool canQueue(qint64 bytes) const noexcept;
    void closeAfterDrain(std::function<void()> close);
    void enableIndependent(Write write, Capacity pending);
    bool receiveIndependent(const QByteArray& bytes, QString& error);
    Stats stats() const;
    std::function<void(const QJsonObject&)> onMessage;
    std::function<DispatchDecision(const QJsonObject&)> validateMessage;
    std::function<void(const QByteArray&)> onAsset;
    std::function<void(const QString&)> onFailure;
private:
    bool enqueue(const Codec::Encoded& encoded);
    void drainIntents();
    bool sendReceipt(quint64 id, const DispatchDecision& decision);
    void flush();
    Codec codec_;
    QByteArray sendKey_, receiveKey_;
    Write write_;
    Capacity pending_;
    QTimer flushTimer_;
    QTimer assemblyTimer_;
    QTimer turnTimer_;
    struct Intent { Codec::Encoded encoded; QString key; qint64 queuedNs; };
    std::deque<Intent> intents_;
    qsizetype intentBytes_ = 0;
    Options options_;
    quint64 pendingSuccess_ = 0;
    std::deque<QByteArray> output_;
    struct PendingReceipt { quint64 id; qint64 queuedNs; };
    std::deque<PendingReceipt> awaitingReceipt_;
    QElapsedTimer receiptClock_;
    quint64 sendSequence_ = 1, receiveSequence_ = 1;
    quint64 highestMessage_ = 0, highestReceipt_ = 0;
    Stats stats_;
    QByteArray assembly_;
    quint64 assemblyId_ = 0;
    quint32 assemblyTotal_ = 0;
    Operation assemblyOperation_{};
    bool failed_ = false;
    bool independentOnly_ = false;
    std::unique_ptr<Channel> independent_;
    QByteArray independentBuffer_;
    QTimer independentFrameTimer_;
    std::function<void()> closeAfterDrain_;
};

} // namespace jam2::application::control_wire
