#pragma once

#include "BeatGridModel.hpp"
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
#include <functional>
#include <vector>

class QPainter;

struct PerformancePeerPresentation {
    std::uint64_t peerId = 0;
    QString label;
    bool receiving = false;
    double gainDb = 0.0;
    bool selected = false;
};

class PerformanceHomeWidget final : public QWidget {
public:
    explicit PerformanceHomeWidget(QWidget* parent = nullptr);

    void setSongModel(const BeatGridModel* model);
    void setTiming(
        quint64 absoluteBeat,
        int subdivision,
        int beatsPerBar,
        double beatPhase,
        bool running);
    void setAudioPeaks(const jam2::EngineGuiPeakSnapshot& peaks);
    void setPeers(QVector<PerformancePeerPresentation> peers);
    void setSelectedPeer(std::uint64_t peerId);
    void setTrackGainDb(double gainDb);
    void setTrackWaveform(std::vector<float> peaks, bool valid);
    void setTechnicalSummary(
        const QString& rtt,
        const QString& jitter,
        const QString& loss,
        const QString& xruns);
    QString rendererStatsText() const;

    std::function<void(const QString&)> onOpenDetail;
    std::function<void(std::uint64_t)> onPeerSelected;
    std::function<void(double)> onTrackGainChanged;
    std::function<void()> onGenerateIdea;
    std::function<void()> onGenerateWav;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct SongPosition {
        int section = 0;
        int sectionBeat = 0;
        quint64 songBeat = 0;
        quint64 totalBeats = 0;
    };

    SongPosition songPosition(quint64 absoluteBeat) const;
    SongPosition songPositionFromSongBeat(quint64 songBeat) const;
    QString chordAt(const SongPosition& position) const;
    QVector<QPair<QString, QString>> upcomingChords(const SongPosition& position) const;
    QPair<QString, QString> lyricLines(const SongPosition& position) const;
    int peerVisibleCapacity() const;
    void rebuildBackground();
    void advanceAnimation();
    void paintHtmlStage();
    void paintBeatPreview(
        QPainter& painter,
        const QRect& bounds,
        quint64 barStart,
        bool current);
    void paintGenerationActions(QPainter& painter, int top, int right);
    void paintPeerRail(QPainter& painter, const QRect& bounds);
    void paintVerticalPeerRail(QPainter& painter, const QRect& bounds);
    void paintLooperLaunch(QPainter& painter, const QRect& bounds);
    void applyTrackSliderPosition(int x);

    const BeatGridModel* model_ = nullptr;
    quint64 absoluteBeat_ = 0;
    int subdivision_ = 0;
    int beatsPerBar_ = 4;
    double beatPhase_ = 0.0;
    bool running_ = false;
    double targetEnergy_ = 0.0;
    double envelope_ = 0.0;
    QVector<double> history_;
    QVector<QPointF> stars_;
    QVector<PerformancePeerPresentation> peers_;
    std::uint64_t selectedPeerId_ = 0;
    double trackGainDb_ = 0.0;
    std::vector<float> trackWaveformPeaks_;
    bool trackWaveformValid_ = false;
    QString rtt_ = QStringLiteral("-");
    QString jitter_ = QStringLiteral("-");
    QString loss_ = QStringLiteral("-");
    QString xruns_ = QStringLiteral("-");
    QString rendererStats_ = QStringLiteral("Visualizer: waiting for first frame");
    QImage nebulaCache_;
    QTimer animationTimer_;
    QElapsedTimer animationClock_;
    QElapsedTimer renderWindow_;
    int renderedFrames_ = 0;
    qint64 renderTotalNanoseconds_ = 0;
    qint64 renderMaximumNanoseconds_ = 0;
    qint64 lastAnimationMs_ = 0;
    qint64 nextNovaMs_ = 60000;
    qint64 novaStartMs_ = -1;
    QPointF novaPosition_{0.78, 0.24};
    int peerScrollOffset_ = 0;
    bool trackSliderDragging_ = false;
    QRect chordHitRect_;
    QRect chordRunwayRect_;
    QRect lyricsHitRect_;
    QRect generateIdeaHitRect_;
    QRect generateWavHitRect_;
    QRect currentBeatHitRect_;
    QRect nextBeatHitRect_;
    QRect peerRailRect_;
    QRect looperHitRect_;
    QRect trackSliderRect_;
    QVector<QPair<QRect, std::uint64_t>> peerHitRects_;
};
