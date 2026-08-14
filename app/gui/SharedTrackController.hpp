#pragma once

#include "SharedTrackModel.hpp"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class SharedTrackController {
public:
    enum class PlaybackPhase {
        Stopped,
        WaitingForAssets,
        PreparingMix,
        WaitingForTransport,
        Playing,
    };

    struct PlaybackState {
        bool requestedPlaying = false;
        bool actualPlaying = false;
        quint64 arrangementRevision = 0;
        PlaybackPhase phase = PlaybackPhase::Stopped;

        bool operator==(const PlaybackState&) const = default;
    };

    struct EffectiveLoop {
        bool enabled = false;
        qint64 startFrame = 0;
        qint64 endFrame = 0;
    };

    struct ProjectDecodeResult {
        SharedTrackModel model;
        bool valid = false;
        bool normalized = false;
        QString error;
    };

    SharedTrackModel& model();
    const SharedTrackModel& model() const;
    void replaceModel(SharedTrackModel model) noexcept;
    void setLoopEnabled(bool enabled) noexcept;
    bool setLoopStartAtMilliseconds(qint64 positionMs) noexcept;
    bool setLoopEndAtMilliseconds(qint64 positionMs) noexcept;
    void setWholeTrackLoop() noexcept;
    void clearLoop() noexcept;
    EffectiveLoop effectiveLoop(int sampleRate, qint64 frames) const noexcept;
    QJsonObject projectJson() const;
    static ProjectDecodeResult decodeProjectJson(
        const QJsonValue& value,
        const QString& projectFolder,
        bool syncControls);
    const PlaybackState& playback() const noexcept { return playback_; }
    void requestPlayback(bool playing, quint64 arrangementRevision = 0) noexcept;
    void waitForAssets(quint64 arrangementRevision, bool playing) noexcept;
    void prepareMix(quint64 arrangementRevision, bool playing) noexcept;
    void preparedForTransport(quint64 arrangementRevision) noexcept;
    bool observeEnginePlaying(bool playing) noexcept;
    QString playbackStatusText(bool syncEnabled) const;
    QJsonObject processingMessage() const;
    void applyProcessingMessage(const QJsonObject& message);

private:
    SharedTrackModel model_;
    PlaybackState playback_;
};
