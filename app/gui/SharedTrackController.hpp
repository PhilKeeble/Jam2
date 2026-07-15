#pragma once

#include "SharedTrackModel.hpp"

#include <QJsonObject>

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

    SharedTrackModel& model();
    const SharedTrackModel& model() const;
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
