#pragma once

#include "BeatGridModel.hpp"
#include "GuiControlContract.hpp"
#include "RuntimeContracts.hpp"
#include "engine.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <array>
#include <functional>
#include <vector>

class QPainter;
class QKeyEvent;

class MetronomeNebulaWidget final : public QWidget {
public:
    explicit MetronomeNebulaWidget(QWidget* parent = nullptr);

    void setBpm(int bpm);
    void setPulseState(
        int state,
        double phase,
        int beatState,
        double beatPhase,
        double stepsPerSecond,
        bool active);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QTimer animationTimer_;
    QElapsedTimer animationClock_;
    int bpm_ = 120;
    int pulseState_ = 0;
    int beatPulseState_ = 0;
    double pulsePhase_ = 0.0;
    double beatPulsePhase_ = 0.0;
    double stepsPerSecond_ = 2.0;
    double stateEnvelope_ = 0.22;
    double pulseEnvelope_ = 0.0;
    qint64 lastPaintMs_ = 0;
    bool active_ = false;
};

class MetronomePatternWidget final : public QWidget, public jam2::gui::GuiVirtualControlProvider {
public:
    explicit MetronomePatternWidget(QWidget* parent = nullptr);

    void setPattern(
        int beats,
        int division,
        int tempoPulseUnits,
        const QVector<bool>& enabled,
        const QVector<bool>& accents);
    void setCurrentStep(int step, bool active);
    QVector<jam2::gui::GuiVirtualControl> guiVirtualControls() const override;
    bool invokeGuiVirtualControl(
        const QString& id,
        const QString& operation,
        const QVariant& value,
        QString& error) override;

    std::function<void(int step, bool enabled, bool accent)> onStepChanged;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    int stepAt(const QPoint& point) const;
    void setStepState(int step, int state);

    int beats_ = 4;
    int division_ = 1;
    int tempoPulseUnits_ = 1;
    int currentStep_ = -1;
    bool active_ = false;
    QVector<bool> enabled_;
    QVector<bool> accents_;
};

struct PerformancePeerPresentation {
    std::uint64_t peerId = 0;
    QString label;
    bool receiving = false;
    double gainDb = 0.0;
    bool selected = false;
};

class PerformanceHomeWidget final : public QWidget, public jam2::gui::GuiVirtualControlProvider {
public:
    explicit PerformanceHomeWidget(QWidget* parent = nullptr);

    void setSongModel(const BeatGridModel* model);
    void setChordPreviewVisible(bool visible);
    void setBeatPreviewVisible(bool visible);
    void setTiming(
        quint64 absoluteBeat,
        int subdivision,
        int beatsPerBar,
        double beatPhase,
        bool running);
    void setAudioPeaks(const jam2::EngineGuiPeakSnapshot& peaks);
    void setTunerSnapshot(const jam2::EnginePitchSnapshot& snapshot);
    void setPeers(QVector<PerformancePeerPresentation> peers);
    void setSelectedPeer(std::uint64_t peerId);
    void setTrackGainDb(double gainDb);
    void setTrackWaveform(std::vector<float> peaks, bool valid);
    void setTrackBpmMismatch(bool mismatched, double backingBpm, double sessionBpm);
    void setTrackTransferStatus(const QString& status);
    const QString& trackTransferStatus() const noexcept { return trackTransferStatus_; }
    void setWavGenerationActive(bool active);
    void setJamTasterTaskStatus(bool active, int percent);
    void setJamRecordingState(bool enabled, bool active, const QString& takeName);
    bool jamRecordingEnabled() const noexcept { return jamRecordingEnabled_; }
    bool jamRecordingActive() const noexcept { return jamRecordingActive_; }
    const QString& jamRecordingTake() const noexcept { return jamRecordingTake_; }
    void setBankState(
        int liveBank,
        int pendingBank,
        quint64 beatsUntilSwitch,
        int pendingBeatsPerBar,
        bool localOnly,
        const QString& status);
    void setArrangementState(bool running, bool armed);
    QString rendererStatsText() const;
    QVector<jam2::gui::GuiVirtualControl> guiVirtualControls() const override;
    bool invokeGuiVirtualControl(
        const QString& id,
        const QString& operation,
        const QVariant& value,
        QString& error) override;

    std::function<void(const QString&)> onOpenDetail;
    std::function<void(std::uint64_t)> onPeerSelected;
    std::function<void(double)> onTrackGainChanged;
    std::function<void()> onGenerateIdea;
    std::function<void()> onBrowseIdeas;
    std::function<void()> onContinueIdea;
    std::function<void()> onClearIdea;
    std::function<void()> onGenerateWav;
    std::function<void()> onJamTaster;
    std::function<void(bool)> onTunerEnabledChanged;
    std::function<void()> onJamRecordingToggle;
    std::function<void(int)> onBankLaunch;
    std::function<void()> onAddSection;
    std::function<void()> onRemoveSection;
    std::function<void()> onManageArrangement;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct SongPosition {
        int section = 0;
        int sectionBeat = 0;
        quint64 songBeat = 0;
        quint64 totalBeats = 0;
    };

    SongPosition songPosition(quint64 absoluteBeat) const;
    SongPosition songPositionFromSongBeat(quint64 songBeat) const;
    SongPosition songPositionForBankBeat(int bankIndex, quint64 songBeat) const;
    QString chordAt(const SongPosition& position) const;
    QVector<QPair<QString, QString>> upcomingChords(const SongPosition& position) const;
    QPair<QString, QString> lyricLines(const SongPosition& position) const;
    int peerVisibleCapacity() const;
    void rebuildBackground();
    void advanceAnimation();
    void updateAnimatedForeground();
    void paintNebulaFields(
        QPainter& painter,
        double seconds,
        double participantComplexity);
    void paintHtmlStage();
    void paintBeatPreview(
        QPainter& painter,
        const QRect& bounds,
        int bankIndex,
        int previewBeatsPerBar,
        quint64 barStart,
        bool current);
    void paintGenerationActions(QPainter& painter, int top, int right);
    void paintPeerRail(QPainter& painter, const QRect& bounds);
    void paintVerticalPeerRail(QPainter& painter, const QRect& bounds);
    void paintJamRecordingButton(QPainter& painter, const QRect& bounds);
    void paintBankStrip(QPainter& painter, int left, int top);
    void paintLooperLaunch(QPainter& painter, const QRect& bounds);
    void paintTuner(QPainter& painter, const QRect& bounds, bool expanded);
    QString tunerNoteText() const;
    void applyTrackSliderPosition(int x);

    const BeatGridModel* model_ = nullptr;
    int liveBank_ = 0;
    int pendingBank_ = -1;
    quint64 pendingBankBeatsRemaining_ = 0;
    int pendingBankBeatsPerBar_ = 4;
    bool bankLocalOnly_ = false;
    bool arrangementRunning_ = false;
    bool arrangementArmed_ = false;
    QString bankTransitionStatus_;
    quint64 absoluteBeat_ = 0;
    int subdivision_ = 0;
    int beatsPerBar_ = 4;
    double beatPhase_ = 0.0;
    bool running_ = false;
    bool chordPreviewVisible_ = true;
    bool beatPreviewVisible_ = true;
    double targetEnergy_ = 0.0;
    double envelope_ = 0.0;
    QVector<double> history_;
    QVector<PerformancePeerPresentation> peers_;
    std::uint64_t selectedPeerId_ = 0;
    double trackGainDb_ = -3.0;
    jam2::EnginePitchSnapshot tuner_;
    double displayedTunerCents_ = 0.0;
    double tunerOrbOpacity_ = 0.0;
    bool tunerExpanded_ = false;
    std::vector<float> trackWaveformPeaks_;
    bool trackWaveformValid_ = false;
    bool trackBpmMismatch_ = false;
    double backingBpm_ = 0.0;
    double sessionBpm_ = 0.0;
    QString trackTransferStatus_;
    bool wavGenerationActive_ = false;
    bool jamTasterTaskActive_ = false;
    int jamTasterTaskProgress_ = 0;
    bool jamRecordingEnabled_ = false;
    bool jamRecordingActive_ = false;
    QString jamRecordingTake_;
    QString rendererStats_ = QStringLiteral("Visualizer: waiting for first frame");
    QImage authoredNebulaSource_;
    QImage spaceCache_;
    QImage nebulaCache_;
    QImage nebulaMorphCache_;
    QImage filamentCache_;
    QImage dustCache_;
    QImage backgroundFrameCache_;
    qint64 backgroundBuildNanoseconds_ = 0;
    qsizetype backgroundCacheBytes_ = 0;
    qint64 lastBackgroundCompositeMs_ = -1;
    qint64 backgroundLayersReadyMs_ = -1;
    bool backgroundLayerBuildPending_ = false;
    bool backgroundLayersReady_ = false;
    QTimer animationTimer_;
    QElapsedTimer animationClock_;
    QElapsedTimer renderWindow_;
    int renderedFrames_ = 0;
    qint64 renderTotalNanoseconds_ = 0;
    qint64 renderMaximumNanoseconds_ = 0;
    qint64 lastAnimationMs_ = 0;
    qint64 lastBackgroundRepaintMs_ = -1;
    qint64 novaEligibleMs_ = 15000;
    qint64 novaStartMs_ = -1;
    QPointF novaPosition_{0.78, 0.24};
    int peerScrollOffset_ = 0;
    bool trackSliderDragging_ = false;
    QRect chordHitRect_;
    QRect chordRunwayRect_;
    QRect lyricsHitRect_;
    QRect generateIdeaHitRect_;
    QRect browseIdeasHitRect_;
    QRect continueIdeaHitRect_;
    QRect clearIdeaHitRect_;
    QRect generateWavHitRect_;
    QRect jamTasterHitRect_;
    QRect currentBeatHitRect_;
    QRect nextBeatHitRect_;
    QRect peerRailRect_;
    QRect jamRecordingHitRect_;
    QRect arrangementHitRect_;
    QRect addSectionHitRect_;
    QRect removeSectionHitRect_;
    QRect looperHitRect_;
    QRect tunerHitRect_;
    QRect tunerEnableHitRect_;
    QRect tunerOffHitRect_;
    QRect tunerOverlayRect_;
    QRect tunerOverlayCloseHitRect_;
    QRect tunerOverlayOffHitRect_;
    QRect trackSliderRect_;
    QVector<QPair<QRect, std::uint64_t>> peerHitRects_;
    QVector<QRect> bankHitRects_;
};
