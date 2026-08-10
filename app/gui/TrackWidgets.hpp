#pragma once

#include "GuiTheme.hpp"
#include "LooperProject.hpp"
#include "SectionTimeline.hpp"

#include <QColor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMap>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QPoint>
#include <QPolygon>
#include <QProxyStyle>
#include <QRect>
#include <QRegion>
#include <QSizePolicy>
#include <QString>
#include <QStyleOption>
#include <QUrl>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace jam2::gui {

inline int trackTimelineBarNumber(int zeroBasedBeat, int beatsPerBar) noexcept
{
    if (zeroBasedBeat < 0 || beatsPerBar <= 0 || zeroBasedBeat % beatsPerBar != 0) {
        return 0;
    }
    return zeroBasedBeat / beatsPerBar + 1;
}

inline qint64 looperTimelineViewFrames(
    int markerSampleRate,
    qint64 minimumViewFrames,
    qint64 arrangementEndFrame,
    qint64 loopStartMs,
    qint64 loopEndMs) noexcept
{
    const qint64 rate = qMax(1, markerSampleRate);
    qint64 frames = qMax<qint64>(1, minimumViewFrames);
    if (loopStartMs >= 0) frames = qMax(frames, loopStartMs * rate / 1000);
    if (loopEndMs >= 0) frames = qMax(frames, loopEndMs * rate / 1000);
    return qMax<qint64>(1, qMax(frames, arrangementEndFrame));
}

inline qint64 stableLiveTimelineExtentFrames(
    qint64 currentExtentFrames,
    qint64 minimumExtentFrames,
    qint64 liveEndFrame) noexcept
{
    if (liveEndFrame <= 0) return 0;
    qint64 extent = qMax<qint64>(1,
        qMax(currentExtentFrames, minimumExtentFrames));
    constexpr qint64 maximum = (std::numeric_limits<qint64>::max)();
    while (extent < liveEndFrame) {
        if (extent > maximum / 2) {
            return maximum;
        }
        extent *= 2;
    }
    return extent;
}

inline double trackGainPosition(double gainDb) noexcept
{
    gainDb = qBound(-60.0, gainDb, 12.0);
    constexpr double deepAttenuationEnd = 0.125;
    constexpr double unityPosition = 0.625;
    if (gainDb <= -30.0) {
        return ((gainDb + 60.0) / 30.0) * deepAttenuationEnd;
    }
    if (gainDb <= 0.0) {
        return deepAttenuationEnd + ((gainDb + 30.0) / 30.0) *
            (unityPosition - deepAttenuationEnd);
    }
    return unityPosition + (gainDb / 12.0) * (1.0 - unityPosition);
}

inline double trackGainDb(double position) noexcept
{
    position = qBound(0.0, position, 1.0);
    constexpr double deepAttenuationEnd = 0.125;
    constexpr double unityPosition = 0.625;
    if (position <= deepAttenuationEnd) {
        return -60.0 + (position / deepAttenuationEnd) * 30.0;
    }
    if (position <= unityPosition) {
        return -30.0 + ((position - deepAttenuationEnd) /
            (unityPosition - deepAttenuationEnd)) * 30.0;
    }
    return ((position - unityPosition) / (1.0 - unityPosition)) * 12.0;
}

} // namespace jam2::gui

namespace theme = jam2::gui::theme;

class WaveformWidget : public QWidget {
public:
    explicit WaveformWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(240);
        setMouseTracking(true);
    }

    std::function<void(qint64)> onSeekMs;

    void clear()
    {
        peaks_.clear();
        label_ = QStringLiteral("No WAV loaded");
        durationMs_ = 0;
        playheadMs_ = 0;
        loopStartMs_ = -1;
        loopEndMs_ = -1;
        update();
    }

    void setDurationMs(qint64 durationMs)
    {
        const qint64 boundedDuration = qMax<qint64>(0, durationMs);
        if (durationMs_ == boundedDuration) return;
        durationMs_ = boundedDuration;
        playheadMs_ = qBound<qint64>(0, playheadMs_, durationMs_);
        update();
    }

    void setBpm(double bpm)
    {
        const double boundedBpm = qBound(1.0, bpm, 400.0);
        if (qAbs(bpm_ - boundedBpm) < 0.0001) return;
        bpm_ = boundedBpm;
        update();
    }

    void setGridPosition(
        qint64 positionMs,
        bool running,
        int beatsPerBar,
        int tempoPulseUnits)
    {
        const qint64 boundedPosition = qMax<qint64>(0, positionMs);
        const int boundedBeatsPerBar = qMax(1, beatsPerBar);
        const int boundedPulseUnits = tempoPulseUnits == 3 ? 3 : 1;
        const bool layoutChanged =
            beatsPerBar_ != boundedBeatsPerBar ||
            tempoPulseUnits_ != boundedPulseUnits;
        const int oldX = gridMarkerX(gridPositionMs_);
        const bool markerChanged =
            gridPositionMs_ != boundedPosition || gridRunning_ != running;

        gridPositionMs_ = boundedPosition;
        gridRunning_ = running;
        beatsPerBar_ = boundedBeatsPerBar;
        tempoPulseUnits_ = boundedPulseUnits;
        if (layoutChanged) {
            update();
        } else if (markerChanged) {
            QRegion damage(markerDamageAtX(oldX));
            damage += markerDamageAtX(gridMarkerX(gridPositionMs_));
            update(damage);
        }
    }

    void setPlayheadMs(qint64 positionMs)
    {
        const qint64 boundedPosition =
            qBound<qint64>(0, positionMs, qMax<qint64>(durationMs_, 0));
        if (playheadMs_ == boundedPosition) return;
        const int oldX = xForMs(playheadMs_);
        playheadMs_ = boundedPosition;
        QRegion damage(markerDamageAtX(oldX));
        damage += markerDamageAtX(xForMs(playheadMs_));
        update(damage);
    }

    void setLoop(qint64 startMs, qint64 endMs)
    {
        const qint64 boundedStart = startMs >= 0 ? startMs : -1;
        const qint64 boundedEnd = endMs >= 0 ? endMs : -1;
        if (loopStartMs_ == boundedStart && loopEndMs_ == boundedEnd) return;
        loopStartMs_ = boundedStart;
        loopEndMs_ = boundedEnd;
        update();
    }

    void setPeaks(std::vector<float> peaks, bool valid)
    {
        peaks_ = std::move(peaks);
        label_ = valid ? QString{} : QStringLiteral("Waveform preview supports PCM16 WAV");
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), theme::editorBg);
        painter.setRenderHint(QPainter::Antialiasing, false);

        if (durationMs_ > 0 && bpm_ > 0.0) {
            const double beatMs = 60000.0 / bpm_ / tempoPulseUnits_;
            const int beats = qMax(
                1, static_cast<int>(std::ceil(static_cast<double>(durationMs_) / beatMs)));
            for (int beat = 0; beat < beats; ++beat) {
                const qint64 beatPosition = static_cast<qint64>(std::llround(beat * beatMs));
                const int x = xForMs(beatPosition);
                const bool bar = beat % beatsPerBar_ == 0;
                painter.setPen(bar ? theme::gridBar : theme::gridBeat);
                painter.drawLine(x, 0, x, height());
                painter.setPen(bar ? theme::text : theme::textMuted);
                painter.drawText(
                    x + 4, 18,
                    QStringLiteral("%1.%2")
                        .arg(beat / beatsPerBar_ + 1)
                        .arg(beat % beatsPerBar_ + 1));
            }
            if (gridRunning_) {
                const qint64 beatPosition = durationMs_ > 0 ? gridPositionMs_ % durationMs_ : 0;
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme::playhead);
                painter.drawEllipse(QPoint(xForMs(beatPosition), 7), 4, 4);
            }
        }

        painter.setPen(theme::border);
        painter.drawLine(0, height() / 2, width(), height() / 2);
        painter.setPen(theme::waveform);
        if (!peaks_.empty()) {
            for (int x = 0; x < width(); ++x) {
                const int index = qBound(0, x * static_cast<int>(peaks_.size()) / qMax(1, width()), static_cast<int>(peaks_.size()) - 1);
                const int half = qMax(2, static_cast<int>(peaks_[index] * (height() / 2 - 14)));
                painter.drawLine(x, height() / 2 - half, x, height() / 2 + half);
            }
        }
        painter.setPen(theme::text);
        painter.drawText(rect().adjusted(12, 8, -12, -8), Qt::AlignLeft | Qt::AlignTop, label_);

        drawLoopMarker(painter, loopStartMs_, theme::success, QStringLiteral("Loop Start"));
        drawLoopMarker(painter, loopEndMs_, theme::warning, QStringLiteral("Loop End"));

        if (loopStartMs_ >= 0 && loopEndMs_ > loopStartMs_) {
            const int startX = xForMs(loopStartMs_);
            const int endX = xForMs(loopEndMs_);
            painter.fillRect(
                QRect(QPoint(startX, 0), QPoint(endX, height())).normalized(),
                theme::withAlpha(theme::selection, 72));
        }

        if (durationMs_ > 0) {
            const int playheadX = xForMs(playheadMs_);
            painter.setPen(QPen(theme::playhead, 2));
            painter.drawLine(playheadX, 0, playheadX, height());
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || durationMs_ <= 0) {
            QWidget::mousePressEvent(event);
            return;
        }
        seekToX(event->position().x());
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || durationMs_ <= 0) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        seekToX(event->position().x());
    }

private:
    QRect markerDamageAtX(int x) const
    {
        return QRect(x - 7, 0, 15, height());
    }

    int gridMarkerX(qint64 positionMs) const
    {
        const qint64 wrapped = durationMs_ > 0 ? positionMs % durationMs_ : 0;
        return xForMs(wrapped);
    }

    int xForMs(qint64 ms) const
    {
        if (durationMs_ <= 0 || width() <= 1) {
            return 0;
        }
        return qBound(0, static_cast<int>((static_cast<double>(ms) / static_cast<double>(durationMs_)) * width()), width() - 1);
    }

    qint64 msForX(double x) const
    {
        if (durationMs_ <= 0 || width() <= 1) {
            return 0;
        }
        const double clampedX = qBound(0.0, x, static_cast<double>(width() - 1));
        return qBound<qint64>(0, static_cast<qint64>(std::llround((clampedX / width()) * durationMs_)), durationMs_);
    }

    void seekToX(double x)
    {
        const qint64 position = msForX(x);
        setPlayheadMs(position);
        if (onSeekMs) {
            onSeekMs(position);
        }
    }

    void drawLoopMarker(QPainter& painter, qint64 positionMs, const QColor& color, const QString& text)
    {
        if (positionMs < 0 || durationMs_ <= 0) {
            return;
        }
        const int x = xForMs(positionMs);
        painter.setPen(QPen(color, 2));
        painter.drawLine(x, 0, x, height());
        painter.setBrush(color);
        QPolygon tag;
        tag << QPoint(x, 0) << QPoint(x + 9, 0) << QPoint(x, 12);
        painter.drawPolygon(tag);
        painter.setPen(color.lighter(135));
        painter.drawText(x + 6, height() - 10, text);
    }

    std::vector<float> peaks_;
    QString label_ = QStringLiteral("No WAV loaded");
    qint64 durationMs_ = 0;
    qint64 playheadMs_ = 0;
    qint64 loopStartMs_ = -1;
    qint64 loopEndMs_ = -1;
    double bpm_ = 120.0;
    qint64 gridPositionMs_ = 0;
    int beatsPerBar_ = 4;
    int tempoPulseUnits_ = 1;
    bool gridRunning_ = false;
};

class LooperLaneStackWidget : public QWidget {
public:
    struct LaneView {
        LooperLane lane;
        QString assetPath;
        qint64 sourceFrames = 0;
        std::vector<float> peaks;
    };

    explicit LooperLaneStackWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(260);
        setMouseTracking(true);
        setAcceptDrops(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }

    std::function<void(int)> onSelected;
    std::function<void()> onAddLane;
    std::function<void()> onAddWav;
    std::function<void(int)> onMute;
    std::function<void(int)> onSolo;
    std::function<void(int)> onArm;
    std::function<void(int)> onRename;
    std::function<void(int)> onRemove;
    std::function<void(int)> onRevealWav;
    std::function<void(int)> onRemoveWav;
    std::function<void(int, double)> onGainChanged;
    std::function<void(int, qint64, qint64, qint64)> onRegionCommitted;
    std::function<void(int)> onBankSelected;
    std::function<void(int, const QString&)> onWavDropped;

    void setLanes(
        QVector<LaneView> lanes,
        int selected,
        int activeBank,
        int armedLane,
        int sampleRate,
        qint64 minimumViewFrames,
        double bpm,
        int tempoPulseUnits,
        bool gridLockEnabled)
    {
        const bool preserveActiveDrag = dragMode_ != DragMode::None &&
            dragLane_ >= 0 && dragLane_ < lanes_.size() &&
            dragLane_ < lanes.size() &&
            !lanes_[dragLane_].lane.id.isEmpty() &&
            lanes_[dragLane_].lane.id == lanes[dragLane_].lane.id;
        LaneView activeDragPreview;
        if (preserveActiveDrag) {
            activeDragPreview = lanes_[dragLane_];
        }
        lanes_ = std::move(lanes);
        if (preserveActiveDrag) {
            lanes_[dragLane_] = std::move(activeDragPreview);
        }
        selectedLane_ = selected;
        activeBank_ = qMax(0, activeBank);
        armedLane_ = armedLane;
        markerSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        minimumViewFrames_ = qMax<qint64>(1, minimumViewFrames);
        bpm_ = qBound(1.0, bpm, 400.0);
        tempoPulseUnits_ = tempoPulseUnits == 3 ? 3 : 1;
        gridLockEnabled_ = gridLockEnabled;
        if (!preserveActiveDrag) {
            dragMode_ = DragMode::None;
            dragLane_ = -1;
            dragViewFrames_ = 0;
            dragTimelineRect_ = {};
        }
        updateMinimumHeight();
        update();
    }

    void setGridPosition(
        qint64 positionMs,
        bool running,
        double bpm,
        int beatsPerBar,
        int tempoPulseUnits)
    {
        const qint64 boundedPosition = qMax<qint64>(0, positionMs);
        const double boundedBpm = qBound(1.0, bpm, 400.0);
        const int boundedBeatsPerBar = qMax(1, beatsPerBar);
        const int boundedPulseUnits = tempoPulseUnits == 3 ? 3 : 1;
        const bool layoutChanged =
            qAbs(bpm_ - boundedBpm) >= 0.0001 ||
            beatsPerBar_ != boundedBeatsPerBar ||
            tempoPulseUnits_ != boundedPulseUnits;
        const int oldX = gridMarkerX(gridPositionMs_);
        const bool markerChanged =
            gridPositionMs_ != boundedPosition || gridRunning_ != running;

        gridPositionMs_ = boundedPosition;
        gridRunning_ = running;
        bpm_ = boundedBpm;
        beatsPerBar_ = boundedBeatsPerBar;
        tempoPulseUnits_ = boundedPulseUnits;
        if (layoutChanged) {
            update();
        } else if (markerChanged) {
            QRegion damage(markerDamageAtX(oldX));
            damage += markerDamageAtX(gridMarkerX(gridPositionMs_));
            update(damage);
        }
    }

    void setRemoteRecordingStates(
        QMap<QString, QString> states,
        bool interactionsProtected)
    {
        remoteRecordingStates_ = std::move(states);
        interactionsProtected_ = interactionsProtected;
        update();
    }

    void setInteractionsProtected(bool protectedState)
    {
        interactionsProtected_ = protectedState;
        update();
    }

    void setPlaybackMarkers(qint64 positionMs, qint64 loopStartMs, qint64 loopEndMs)
    {
        const qint64 boundedPlayhead = positionMs >= 0 ? positionMs : -1;
        const qint64 boundedLoopStart = loopStartMs >= 0 ? loopStartMs : -1;
        const qint64 boundedLoopEnd = loopEndMs >= 0 ? loopEndMs : -1;
        const bool loopChanged =
            loopStartMs_ != boundedLoopStart || loopEndMs_ != boundedLoopEnd;
        const int oldX = playbackMarkerX(playheadMs_);
        const bool playheadChanged = playheadMs_ != boundedPlayhead;

        playheadMs_ = boundedPlayhead;
        loopStartMs_ = boundedLoopStart;
        loopEndMs_ = boundedLoopEnd;
        if (loopChanged) {
            update();
        } else if (playheadChanged) {
            QRegion damage(markerDamageAtX(oldX));
            damage += markerDamageAtX(playbackMarkerX(playheadMs_));
            update(damage);
        }
    }

    void setLiveRecordingEndFrame(qint64 frame)
    {
        const qint64 bounded = qMax<qint64>(0, frame);
        const qint64 nextExtent = jam2::gui::stableLiveTimelineExtentFrames(
            liveRecordingViewExtentFrames_, minimumViewFrames_, bounded);
        if (liveRecordingEndFrame_ == bounded &&
            liveRecordingViewExtentFrames_ == nextExtent) return;
        liveRecordingEndFrame_ = bounded;
        liveRecordingViewExtentFrames_ = nextExtent;
        update();
    }

    int timelineZoomLevel() const
    {
        return timelineZoomLevel_;
    }

    void setTimelineZoomLevel(int level, int viewportWidth)
    {
        timelineZoomLevel_ = qBound(0, level, 6);
        if (timelineZoomLevel_ == 0) {
            setMinimumWidth(0);
        } else {
            const int fittedTimelineWidth = qMax(320, viewportWidth - kHeaderWidth - 20);
            const double scale = 1.0 + 0.5 * static_cast<double>(timelineZoomLevel_);
            setMinimumWidth(kHeaderWidth + 20 +
                static_cast<int>(std::llround(fittedTimelineWidth * scale)));
        }
        updateGeometry();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), theme::editorBg);
        painter.setRenderHint(QPainter::Antialiasing, true);

        drawToolbar(painter);

        const int laneCount = visualLaneCount();
        for (int row = 0; row < laneCount; ++row) {
            drawLane(painter, row);
        }

        drawSectionExtensionPreview(painter);
        drawTimelineGrid(painter);
        drawOverlays(painter);
        drawLaneAssetChrome(painter);

        const QRect plus = plusRect();
        const QRect emptyAction = emptyTrackActionRect();
        const QRect importAction = importAudioActionRect();
        painter.save();
        painter.fillRect(plus, theme::editorBg);
        painter.setBrush(theme::withAlpha(theme::nebulaBlue, 22));
        painter.setPen(QPen(theme::withAlpha(theme::nebulaBlue, 165), 1, Qt::DashLine));
        painter.drawRoundedRect(emptyAction, 5, 5);
        painter.setPen(theme::nebulaBlue);
        painter.drawText(emptyAction, Qt::AlignCenter, QStringLiteral("+  EMPTY TRACK"));
        painter.setBrush(theme::withAlpha(theme::accent, 20));
        painter.setPen(QPen(theme::withAlpha(theme::accent, 165), 1, Qt::DashLine));
        painter.drawRoundedRect(importAction, 5, 5);
        painter.setPen(theme::accent);
        painter.drawText(importAction, Qt::AlignCenter, QStringLiteral("IMPORT AUDIO\u2026"));
        painter.restore();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        const QPoint pos = event->position().toPoint();
        if (emptyTrackActionRect().contains(pos)) {
            if (!interactionsProtected_ && onAddLane) onAddLane();
            event->accept();
            return;
        }
        if (importAudioActionRect().contains(pos)) {
            if (!interactionsProtected_ && onAddWav) onAddWav();
            event->accept();
            return;
        }

        const int laneIndex = laneAt(pos);
        if (laneIndex < 0) {
            QWidget::mousePressEvent(event);
            return;
        }
        const QString control = controlAt(laneIndex, pos);
        selectLane(laneIndex);
        if (interactionsProtected_) {
            event->accept();
            return;
        }
        if (laneIndex >= lanes_.size()) {
            if (onAddLane) onAddLane();
            if (control == QStringLiteral("arm") && onArm) {
                onArm(laneIndex);
            } else if (control == QStringLiteral("rename") && onRename) {
                onRename(laneIndex);
            }
            event->accept();
            return;
        }

        if (control == QStringLiteral("mute")) {
            if (onMute) onMute(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("solo")) {
            if (onSolo) onSolo(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("arm")) {
            if (onArm) onArm(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("rename")) {
            if (onRename) onRename(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("remove")) {
            if (onRemove) onRemove(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("reveal_wav")) {
            if (onRevealWav) onRevealWav(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("remove_wav")) {
            if (onRemoveWav) onRemoveWav(laneIndex);
            event->accept();
            return;
        }
        if (control == QStringLiteral("gain")) {
            beginGainDrag(laneIndex, pos.x());
            event->accept();
            return;
        }

        if (laneIndex < lanes_.size() && lanes_[laneIndex].sourceFrames > 0) {
            const QRect clip = clipRect(laneIndex);
            if (clip.adjusted(-4, -6, 4, 6).contains(pos)) {
                constexpr int kEdgePx = 10;
                const int leftDistance = std::abs(pos.x() - clip.left());
                const int rightDistance = std::abs(pos.x() - clip.right());
                DragMode mode = DragMode::Move;
                if (leftDistance <= kEdgePx || rightDistance <= kEdgePx) {
                    mode = leftDistance <= rightDistance
                        ? DragMode::LeftEdge : DragMode::RightEdge;
                }
                beginTimelineDrag(laneIndex, pos.x(), mode);
                event->accept();
                return;
            }
        }
        event->accept();
    }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!interactionsProtected_ && event->mimeData()->hasUrls() &&
            event->mimeData()->urls().size() == 1 &&
            event->mimeData()->urls().front().isLocalFile()) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        const int nextLane = laneAt(event->position().toPoint());
        if (!interactionsProtected_ && nextLane >= 0 &&
            event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1) {
            if (dropLane_ != nextLane) {
                dropLane_ = nextLane;
                update();
            }
            event->acceptProposedAction();
            return;
        }
        if (dropLane_ != -1) {
            dropLane_ = -1;
            update();
        }
        event->ignore();
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override
    {
        dropLane_ = -1;
        update();
        event->accept();
    }

    void dropEvent(QDropEvent* event) override
    {
        const int lane = laneAt(event->position().toPoint());
        const QList<QUrl> urls = event->mimeData()->urls();
        dropLane_ = -1;
        update();
        if (interactionsProtected_ || lane < 0 || urls.size() != 1 ||
            !urls.front().isLocalFile() || !onWavDropped) {
            event->ignore();
            return;
        }
        onWavDropped(lane < lanes_.size() ? lane : -1, urls.front().toLocalFile());
        event->acceptProposedAction();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const QPoint pos = event->position().toPoint();
        if (dragMode_ == DragMode::Gain && dragLane_ >= 0 && dragLane_ < lanes_.size()) {
            const QRect slider = gainRect(dragLane_);
            const double t = qBound(
                0.0,
                static_cast<double>(pos.x() - slider.left()) /
                    qMax(1, slider.width()),
                1.0);
            pendingGainDb_ = jam2::gui::trackGainDb(t);
            lanes_[dragLane_].lane.gainDb = pendingGainDb_;
            update();
            event->accept();
            return;
        }
        if (dragMode_ != DragMode::None && dragLane_ >= 0 && dragLane_ < lanes_.size()) {
            applyDragPreview(pos.x());
            update();
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || dragMode_ == DragMode::None) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const int lane = dragLane_;
        const DragMode mode = dragMode_;
        dragMode_ = DragMode::None;
        dragLane_ = -1;
        dragViewFrames_ = 0;
        dragTimelineRect_ = {};
        if (lane >= 0 && lane < lanes_.size()) {
            if (mode == DragMode::Gain) {
                if (onGainChanged) onGainChanged(lane, pendingGainDb_);
            } else if (onRegionCommitted) {
                onRegionCommitted(lane, lanes_[lane].lane.startFrame, sourceStart(lane), sourceEnd(lane));
            }
        }
        event->accept();
    }

private:
    enum class DragMode { None, Move, LeftEdge, RightEdge, Gain };

    static constexpr int kToolbarHeight = 36;
    static constexpr int kLaneHeight = 140;
    static constexpr int kHeaderWidth = 286;
    static constexpr int kPlusHeight = 44;

    QRect markerDamageAtX(int x) const
    {
        if (x < 0) return {};
        const int bottom = kToolbarHeight + visualLaneCount() * kLaneHeight;
        return QRect(x - 7, kToolbarHeight - 14, 15, qMax(0, bottom - kToolbarHeight + 14));
    }

    int gridMarkerX(qint64 positionMs) const
    {
        const qint64 frames = viewFrames();
        if (frames <= 0) return -1;
        const qint64 markerFrame =
            (positionMs * static_cast<qint64>(markerSampleRate()) / 1000) % frames;
        return xForFrame(markerFrame);
    }

    int playbackMarkerX(qint64 positionMs) const
    {
        if (positionMs < 0) return -1;
        return xForFrame(positionMs * static_cast<qint64>(markerSampleRate()) / 1000);
    }

    int visualLaneCount() const
    {
        return lanes_.size();
    }

    void updateMinimumHeight()
    {
        setMinimumHeight(kToolbarHeight + visualLaneCount() * kLaneHeight + kPlusHeight + 12);
    }

    QRect timelineRect() const
    {
        return rect().adjusted(kHeaderWidth + 10, kToolbarHeight, -10, -kPlusHeight - 8);
    }

    QRect laneRect(int row) const
    {
        return QRect(0, kToolbarHeight + row * kLaneHeight, width(), kLaneHeight);
    }

    QRect laneTimelineRect(int row) const
    {
        return laneRect(row).adjusted(kHeaderWidth + 8, 7, -8, -7);
    }

    QRect plusRect() const
    {
        return QRect(0, kToolbarHeight + visualLaneCount() * kLaneHeight, width(), kPlusHeight);
    }

    QRect emptyTrackActionRect() const
    {
        const QRect area = plusRect().adjusted(8, 4, -8, -3);
        return QRect(area.left(), area.top(), (area.width() - 6) / 2, area.height());
    }

    QRect importAudioActionRect() const
    {
        const QRect area = plusRect().adjusted(8, 4, -8, -3);
        const int leftWidth = (area.width() - 6) / 2;
        return QRect(area.left() + leftWidth + 6, area.top(),
            area.width() - leftWidth - 6, area.height());
    }

    qint64 sourceStart(int laneIndex) const
    {
        if (laneIndex < 0 || laneIndex >= lanes_.size()) return 0;
        const LooperLane& lane = lanes_[laneIndex].lane;
        return lane.loopStartFrame >= 0 ? qBound<qint64>(0, lane.loopStartFrame, qMax<qint64>(0, lanes_[laneIndex].sourceFrames - 1)) : 0;
    }

    qint64 sourceEnd(int laneIndex) const
    {
        if (laneIndex < 0 || laneIndex >= lanes_.size()) return 0;
        const qint64 start = sourceStart(laneIndex);
        const qint64 frames = lanes_[laneIndex].sourceFrames;
        return laneIndex < lanes_.size() && lanes_[laneIndex].lane.loopEndFrame > start
            ? qBound<qint64>(start + 1, lanes_[laneIndex].lane.loopEndFrame, frames)
            : frames;
    }

    qint64 visibleFrames(int laneIndex) const
    {
        return qMax<qint64>(1, sourceEnd(laneIndex) - sourceStart(laneIndex));
    }

    bool timelineDragActive() const
    {
        return dragMode_ == DragMode::Move ||
            dragMode_ == DragMode::LeftEdge ||
            dragMode_ == DragMode::RightEdge;
    }

    qint64 calculatedViewFrames() const
    {
        qint64 arrangementEndFrame = 0;
        for (int i = 0; i < lanes_.size(); ++i) {
            arrangementEndFrame = qMax(
                arrangementEndFrame,
                lanes_[i].lane.startFrame + visibleFrames(i));
        }
        arrangementEndFrame = qMax(
            arrangementEndFrame, liveRecordingViewExtentFrames_);
        return jam2::gui::looperTimelineViewFrames(
            markerSampleRate(),
            minimumViewFrames_,
            arrangementEndFrame,
            loopStartMs_,
            loopEndMs_);
    }

    qint64 viewFrames() const
    {
        if (timelineDragActive() && dragViewFrames_ > 0) {
            const qint64 dragEnd = dragLane_ >= 0 && dragLane_ < lanes_.size()
                ? lanes_[dragLane_].lane.startFrame + visibleFrames(dragLane_) : 0;
            return jam2::gui::sectionExtensionPreviewFrames(
                dragViewFrames_, dragEnd);
        }
        return calculatedViewFrames();
    }

    QRect effectiveTimelineRect() const
    {
        return timelineDragActive() && dragTimelineRect_.isValid()
            ? dragTimelineRect_ : timelineRect();
    }

    int markerSampleRate() const
    {
        return markerSampleRate_ > 0 ? markerSampleRate_ : 48000;
    }

    int xForFrame(qint64 frame) const
    {
        const QRect area = effectiveTimelineRect();
        return area.left() + qBound(0, static_cast<int>(std::llround((static_cast<double>(frame) / viewFrames()) * area.width())), qMax(0, area.width()));
    }

    qint64 frameDeltaForX(double dx) const
    {
        const QRect area = effectiveTimelineRect();
        const qint64 transformFrames = timelineDragActive() && dragViewFrames_ > 0
            ? dragViewFrames_ : viewFrames();
        return area.width() > 1 ? static_cast<qint64>(std::llround(
            (dx / area.width()) * transformFrames)) : 0;
    }

    qint64 snapTimelineFrame(qint64 frame) const
    {
        frame = qMax<qint64>(0, frame);
        const bool gridLock = timelineDragActive() ? dragGridLockEnabled_ : gridLockEnabled_;
        if (!gridLock) {
            return frame;
        }
        const int rate = timelineDragActive() ? dragMarkerSampleRate_ : markerSampleRate();
        const double bpm = timelineDragActive() ? dragBpm_ : bpm_;
        const int tempoPulseUnits = timelineDragActive()
            ? dragTempoPulseUnits_ : tempoPulseUnits_;
        const double beatFrames =
            static_cast<double>(rate) * 60.0 / bpm / tempoPulseUnits;
        const qint64 beat = static_cast<qint64>(std::llround(static_cast<double>(frame) / beatFrames));
        return qMax<qint64>(0, static_cast<qint64>(std::llround(static_cast<double>(beat) * beatFrames)));
    }

    QRect clipRect(int laneIndex) const
    {
        const QRect area = laneTimelineRect(laneIndex).adjusted(0, 12, 0, -12);
        const int left = xForFrame(lanes_[laneIndex].lane.startFrame);
        const int right = qMax(left + 1, xForFrame(lanes_[laneIndex].lane.startFrame + visibleFrames(laneIndex)));
        return QRect(QPoint(left, area.top()), QPoint(right, area.bottom())).normalized();
    }

    void drawToolbar(QPainter& painter)
    {
        painter.fillRect(QRect(0, 0, width(), kToolbarHeight), theme::panelRaised);
        painter.setPen(theme::border);
        painter.drawLine(0, kToolbarHeight - 1, width(), kToolbarHeight - 1);
        QFont labelFont(QStringLiteral("Bahnschrift"));
        labelFont.setPixelSize(11);
        labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        painter.setFont(labelFont);
        painter.setPen(theme::textMuted);
        painter.drawText(
            QRect(18, 0, kHeaderWidth - 36, kToolbarHeight),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("TRACKS"));
        if (bpm_ > 0.0) {
            const QRect area = QRect(
                timelineRect().left(), 0, timelineRect().width(), kToolbarHeight);
            const double beatFrames =
                static_cast<double>(markerSampleRate()) * 60.0 / bpm_ /
                tempoPulseUnits_;
            const int beatCount = qBound(
                1,
                static_cast<int>(std::ceil(static_cast<double>(viewFrames()) / beatFrames)),
                2048);
            int lastLabelRight = area.left() - 20;
            for (int beat = 0; beat < beatCount; ++beat) {
                const int x = xForFrame(static_cast<qint64>(std::llround(beat * beatFrames)));
                if (x >= area.right()) break;
                const int barNumber = jam2::gui::trackTimelineBarNumber(beat, beatsPerBar_);
                if (barNumber > 0 && x >= lastLabelRight + 12) {
                    painter.setPen(theme::withAlpha(theme::gridBar, 150));
                    painter.drawLine(x, kToolbarHeight - 8, x, kToolbarHeight - 1);
                    painter.setPen(theme::text);
                    const QString text = QString::number(barNumber);
                    painter.drawText(x + 5, 0, 44, kToolbarHeight - 5,
                        Qt::AlignLeft | Qt::AlignVCenter, text);
                    lastLabelRight = x + painter.fontMetrics().horizontalAdvance(text) + 5;
                }
            }
        }
    }

    void drawTimelineGrid(QPainter& painter)
    {
        if (bpm_ <= 0.0 || lanes_.isEmpty()) return;
        const QRect area = timelineRect();
        const int bottom = kToolbarHeight + visualLaneCount() * kLaneHeight - 8;
        const double beatFrames =
            static_cast<double>(markerSampleRate()) * 60.0 / bpm_ /
            tempoPulseUnits_;
        const int beatCount = qBound(
            1,
            static_cast<int>(std::ceil(static_cast<double>(viewFrames()) / beatFrames)),
            4096);
        int previousX = area.left() - 10;
        for (int beat = 0; beat <= beatCount; ++beat) {
            const int x = xForFrame(static_cast<qint64>(std::llround(beat * beatFrames)));
            if (x > area.right()) break;
            const bool bar = jam2::gui::trackTimelineBarNumber(beat, beatsPerBar_) > 0;
            if (!bar && x - previousX < 5) continue;
            painter.setPen(QPen(
                bar ? theme::withAlpha(theme::gridBar, 76)
                    : theme::withAlpha(theme::gridBeat, 76),
                bar ? 1.2 : 1.0));
            painter.drawLine(x, kToolbarHeight + 8, x, bottom);
            previousX = x;
        }
    }

    void drawSectionExtensionPreview(QPainter& painter)
    {
        qint64 previewEnd = liveRecordingEndFrame_;
        if (timelineDragActive() && dragLane_ >= 0 && dragLane_ < lanes_.size()) {
            previewEnd = qMax(
                previewEnd,
                lanes_[dragLane_].lane.startFrame + visibleFrames(dragLane_));
        }
        if (previewEnd <= minimumViewFrames_ || lanes_.isEmpty()) return;
        const QRect area = timelineRect();
        const int left = xForFrame(minimumViewFrames_);
        const int right = qMax(left + 1, xForFrame(previewEnd));
        const QRect extension(
            left,
            kToolbarHeight,
            qMax(1, right - left),
            visualLaneCount() * kLaneHeight);
        painter.save();
        painter.fillRect(extension, theme::withAlpha(theme::accent, 18));
        painter.setPen(QPen(theme::withAlpha(theme::accent, 145), 1, Qt::DashLine));
        painter.drawLine(left, extension.top(), left, extension.bottom());
        const int beats = jam2::gui::sectionBeatCountForTimelineEnd(
            previewEnd,
            markerSampleRate(),
            bpm_,
            tempoPulseUnits_,
            beatsPerBar_);
        const int bars = qMax(1, (beats + beatsPerBar_ - 1) / beatsPerBar_);
        QFont font(QStringLiteral("Bahnschrift"));
        font.setPixelSize(11);
        painter.setFont(font);
        painter.setPen(theme::accent);
        painter.drawText(
            area.adjusted(8, 4, -8, 0),
            Qt::AlignRight | Qt::AlignTop,
            QStringLiteral("SECTION EXTENDS TO BAR %1").arg(bars));
        painter.restore();
    }

    void drawOverlays(QPainter& painter)
    {
        const int top = kToolbarHeight;
        const int bottom = kToolbarHeight + visualLaneCount() * kLaneHeight - 1;
        if (bottom <= top) {
            return;
        }
        const int rate = markerSampleRate();
        auto drawMarker = [&](qint64 ms, const QColor& color, int width) {
            if (ms < 0) {
                return;
            }
            const int x = xForFrame(ms * rate / 1000);
            painter.setPen(QPen(color, width));
            painter.drawLine(x, top, x, bottom);
        };
        if (loopStartMs_ >= 0) {
            drawMarker(loopStartMs_, theme::success, 2);
        }
        if (loopEndMs_ >= 0) {
            drawMarker(loopEndMs_, theme::success, 2);
        }
        if (gridRunning_) {
            const qint64 frames = viewFrames();
            const qint64 currentBeatFrame = frames > 0
                ? (gridPositionMs_ * static_cast<qint64>(rate) / 1000) % frames
                : 0;
            const int x = xForFrame(currentBeatFrame);
            painter.setPen(QPen(theme::withAlpha(theme::playhead, 210), 2));
            painter.drawLine(x, top, x, bottom);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::playhead);
            painter.drawEllipse(QPoint(x, kToolbarHeight - 8), 5, 5);
        }
        drawMarker(playheadMs_, theme::playhead, 2);
    }

    void drawLane(QPainter& painter, int row)
    {
        const bool realLane = row < lanes_.size();
        const LooperLane lane = realLane ? lanes_[row].lane : LooperLane{QString(), QString(), QString(), QStringLiteral("Empty Track 1")};
        const QRect rowRect = laneRect(row);
        const bool selected = row == selectedLane_;
        const bool armed = row == armedLane_;
        const QRect headerCard = rowRect.adjusted(8, 7, -(width() - kHeaderWidth + 8), -7);
        const QRect timelineCard = laneTimelineRect(row);
        painter.setPen(QPen(
            selected ? theme::withAlpha(theme::nebulaPurple, 145) : theme::border,
            selected ? 1.4 : 1.0));
        painter.setBrush(selected ? QColor(27, 21, 31) : theme::panelBg);
        painter.drawRoundedRect(headerCard, 7, 7);
        painter.setPen(QPen(selected ? theme::withAlpha(theme::nebulaPurple, 105) : theme::border));
        painter.setBrush(selected ? QColor(11, 13, 17) : theme::editorBg);
        painter.drawRoundedRect(timelineCard, 7, 7);
        if (selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::playhead);
            painter.drawRoundedRect(
                QRect(headerCard.left(), headerCard.top() + 8, 3, headerCard.height() - 16),
                2, 2);
        }

        drawLaneHeader(painter, row, lane, realLane);
        drawLaneWaveform(painter, row, realLane);
        if (row == dropLane_) {
            painter.setBrush(theme::withAlpha(theme::accent, 28));
            painter.setPen(QPen(theme::accent, 2, Qt::DashLine));
            painter.drawRoundedRect(timelineCard.adjusted(2, 2, -2, -2), 6, 6);
            painter.setPen(theme::textStrong);
            painter.drawText(
                timelineCard.adjusted(14, 0, -14, 0),
                Qt::AlignCenter,
                realLane
                    ? QStringLiteral("DROP WAV TO REPLACE THIS TRACK")
                    : QStringLiteral("DROP WAV TO CREATE THIS TRACK"));
        }
        if (armed) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme::playhead, 2));
            painter.drawRoundedRect(headerCard.adjusted(1, 1, -1, -1), 7, 7);
        }
    }

    QRect controlRect(int row, const QString& control) const
    {
        const QRect lane = laneRect(row);
        if (control == QStringLiteral("mute")) return QRect(18, lane.top() + 64, 48, 26);
        if (control == QStringLiteral("solo")) return QRect(73, lane.top() + 64, 48, 26);
        if (control == QStringLiteral("arm")) return QRect(128, lane.top() + 64, 76, 26);
        if (control == QStringLiteral("rename")) return QRect(151, lane.top() + 35, 58, 22);
        if (control == QStringLiteral("remove")) return QRect(215, lane.top() + 35, 52, 22);
        return {};
    }

    QRect gainRect(int row) const
    {
        const QRect lane = laneRect(row);
        return QRect(58, lane.top() + 102, 158, 12);
    }

    bool hasWav(int row) const
    {
        return row >= 0 && row < lanes_.size() &&
            !lanes_[row].lane.assetPath.trimmed().isEmpty();
    }

    QRect revealWavRect(int row) const
    {
        const QRect card = laneTimelineRect(row);
        return QRect(card.right() - 174, card.top() + 9, 72, 24);
    }

    QRect removeWavRect(int row) const
    {
        const QRect card = laneTimelineRect(row);
        return QRect(card.right() - 96, card.top() + 9, 88, 24);
    }

    QString controlAt(int row, const QPoint& pos) const
    {
        for (const QString& control : {QStringLiteral("mute"), QStringLiteral("solo"), QStringLiteral("arm"), QStringLiteral("rename"), QStringLiteral("remove")}) {
            if (controlRect(row, control).contains(pos)) return control;
        }
        if (row == selectedLane_ && hasWav(row)) {
            if (revealWavRect(row).contains(pos)) return QStringLiteral("reveal_wav");
            if (removeWavRect(row).contains(pos)) return QStringLiteral("remove_wav");
        }
        if (gainRect(row).adjusted(-4, -8, 4, 8).contains(pos)) return QStringLiteral("gain");
        return {};
    }

    bool laneIsAudible(int row) const
    {
        if (row < 0 || row >= lanes_.size()) return false;
        const bool anySolo = std::any_of(
            lanes_.cbegin(), lanes_.cend(), [](const LaneView& view) {
                return view.lane.sampleRateCompatible && view.lane.solo &&
                    !view.lane.muted;
            });
        const LooperLane& lane = lanes_[row].lane;
        return lane.sampleRateCompatible && !lane.muted &&
            (!anySolo || lane.solo);
    }

    QColor laneWaveformColor(int row) const
    {
        const QColor source = laneOriginColor(lanes_[row].lane);
        if (laneIsAudible(row)) return source;
        QColor dimmed = source.toHsl();
        dimmed.setHslF(
            dimmed.hslHueF(),
            qMax(0.0, dimmed.hslSaturationF()) * 0.22,
            qBound(0.0, dimmed.lightnessF() * 0.62, 1.0));
        return dimmed;
    }

    QString laneOriginText(const LooperLane& lane) const
    {
        if (lane.assetPath.trimmed().isEmpty() && !lane.assetHash.isEmpty()) {
            return QStringLiteral("Remote WAV not shared");
        }
        if (lane.assetPath.trimmed().isEmpty()) return QStringLiteral("Empty track");
        if (!lane.referenceKind.isEmpty() || lane.originKind == QStringLiteral("generated")) {
            const QString kind = lane.referenceKind.isEmpty()
                ? QStringLiteral("reference") : lane.referenceKind;
            return QStringLiteral("Generated %1").arg(kind);
        }
        if (lane.originKind == QStringLiteral("peer")) return QStringLiteral("Track from peer");
        if (lane.originKind == QStringLiteral("recorded")) return QStringLiteral("Recorded in Jam2");
        if (lane.originKind == QStringLiteral("imported")) return QStringLiteral("Imported audio");
        if (lane.localOnly) return QStringLiteral("Local track");
        return QStringLiteral("Track audio");
    }

    QColor laneOriginColor(const LooperLane& lane) const
    {
        if (!lane.referenceKind.isEmpty() || lane.originKind == QStringLiteral("generated")) {
            return theme::warning;
        }
        if (lane.originKind == QStringLiteral("peer")) return theme::nebulaPurple;
        if (lane.originKind == QStringLiteral("recorded")) return theme::record;
        if (lane.originKind == QStringLiteral("imported")) return theme::accent;
        return theme::textMuted;
    }

    void drawLaneHeader(QPainter& painter, int row, const LooperLane& lane, bool realLane)
    {
        const QRect laneArea = laneRect(row);
        const QRect name(18, laneArea.top() + 8, kHeaderWidth - 36, 27);
        QFont laneNameFont(QStringLiteral("Georgia"));
        laneNameFont.setPointSizeF(14.0);
        laneNameFont.setWeight(QFont::Normal);
        painter.setFont(laneNameFont);
        painter.setPen(theme::textStrong);
        painter.drawText(
            name,
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(laneNameFont).elidedText(
                lane.name.isEmpty() ? QStringLiteral("Empty Track %1").arg(row + 1) : lane.name,
                Qt::ElideRight,
                name.width()));

        QFont detailFont(QStringLiteral("Bahnschrift"));
        detailFont.setPixelSize(11);
        painter.setFont(detailFont);
        const QColor sourceColor = laneOriginColor(lane);
        painter.setPen(Qt::NoPen);
        painter.setBrush(sourceColor);
        painter.drawEllipse(QPoint(21, laneArea.top() + 46), 3, 3);
        painter.setPen(theme::textMuted);
        const QRect origin(29, laneArea.top() + 34, 116, 24);
        painter.drawText(
            origin,
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(detailFont).elidedText(
                laneOriginText(lane), Qt::ElideRight, origin.width()));

        drawActionButton(
            painter,
            controlRect(row, QStringLiteral("rename")),
            QStringLiteral("Rename"),
            theme::accent,
            theme::accentSoft,
            true);
        drawActionButton(
            painter,
            controlRect(row, QStringLiteral("remove")),
            QStringLiteral("Remove"),
            theme::danger,
            theme::withAlpha(theme::nebulaRed, 42),
            realLane);

        drawButton(
            painter,
            controlRect(row, QStringLiteral("mute")),
            QStringLiteral("M"),
            realLane && lane.muted ? theme::warning : theme::buttonBg);
        drawButton(
            painter,
            controlRect(row, QStringLiteral("solo")),
            QStringLiteral("S"),
            realLane && lane.solo ? theme::success : theme::buttonBg);

        const QRect arm = controlRect(row, QStringLiteral("arm"));
        const QString remoteState = remoteRecordingStates_.value(lane.id);
        const bool localArmed = row == armedLane_;
        const bool remoteRecording = remoteState.contains(
            QStringLiteral("RECORDING"), Qt::CaseInsensitive);
        painter.setBrush(
            remoteRecording ? QColor(68, 18, 31) :
                !remoteState.isEmpty() ? QColor(62, 46, 25) : theme::buttonBg);
        painter.setPen(QPen(
            remoteRecording ? theme::record :
                (localArmed || !remoteState.isEmpty()) ? theme::playhead : theme::borderStrong,
            localArmed || !remoteState.isEmpty() ? 2 : 1));
        painter.drawRoundedRect(arm.adjusted(0, 0, -1, -1), 4, 4);
        painter.setPen(remoteRecording ? QColor(255, 190, 196) : theme::textStrong);
        const QString armText = remoteRecording
            ? QStringLiteral("RECORDING")
            : !remoteState.isEmpty() || localArmed
                ? QStringLiteral("ARMED") : QStringLiteral("ARM");
        painter.drawText(
            arm.adjusted(4, 0, -4, 0),
            Qt::AlignCenter,
            QFontMetrics(painter.font()).elidedText(
                armText, Qt::ElideRight, arm.width() - 8));

        const QRect slider = gainRect(row);
        painter.save();
        const QRect groove(slider.left(), slider.center().y() - 3, slider.width(), 6);
        painter.setPen(QPen(theme::borderStrong, 1));
        painter.setBrush(QColor(23, 32, 35));
        painter.drawRoundedRect(groove, 3, 3);
        const double t = jam2::gui::trackGainPosition(lane.gainDb);
        const int x = slider.left() +
            static_cast<int>(std::llround(t * qMax(1, slider.width() - 1)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::withAlpha(theme::accent, 190));
        painter.drawRoundedRect(
            QRect(groove.left() + 1, groove.top() + 1,
                qMax(2, x - groove.left()), groove.height() - 2),
            2, 2);
        const int unityX = slider.left() + static_cast<int>(std::llround(
            jam2::gui::trackGainPosition(0.0) * slider.width()));
        painter.setPen(theme::withAlpha(theme::textMuted, 140));
        painter.drawLine(unityX, slider.top(), unityX, slider.bottom());
        painter.setPen(theme::borderStrong);
        painter.drawLine(slider.left(), slider.top() - 1, slider.left(), slider.bottom() + 1);
        painter.drawLine(slider.right(), slider.top() - 1, slider.right(), slider.bottom() + 1);
        painter.setBrush(theme::playhead);
        painter.setPen(QPen(QColor(255, 226, 170), 1));
        painter.drawEllipse(QPoint(x, slider.center().y()), 6, 6);
        painter.setPen(theme::textMuted);
        painter.drawText(
            QRect(18, laneArea.top() + 96, 42, 24),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("VOL"));
        painter.setPen(theme::text);
        painter.drawText(
            QRect(198, laneArea.top() + 96, 69, 24),
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1 dB").arg(lane.gainDb, 0, 'f', 1));
        painter.restore();
    }

    void drawButton(QPainter& painter, const QRect& rect, const QString& text, const QColor& fill)
    {
        painter.save();
        painter.setBrush(fill);
        painter.setPen(theme::borderStrong);
        painter.drawRoundedRect(rect.adjusted(0, 0, -1, -1), 4, 4);
        painter.setPen(theme::textStrong);
        painter.drawText(rect, Qt::AlignCenter, text);
        painter.restore();
    }

    void drawActionButton(
        QPainter& painter,
        const QRect& rect,
        const QString& text,
        const QColor& color,
        const QColor& fill,
        bool enabled)
    {
        painter.save();
        const QColor resolvedColor = enabled ? color : theme::textMuted;
        painter.setBrush(enabled ? fill : theme::panelRaised);
        painter.setPen(QPen(theme::withAlpha(resolvedColor, enabled ? 185 : 80), 1));
        painter.drawRoundedRect(rect.adjusted(0, 0, -1, -1), 4, 4);
        painter.setPen(resolvedColor);
        painter.drawText(rect, Qt::AlignCenter, text);
        painter.restore();
    }

    void drawIconButton(QPainter& painter, const QRect& rect, const QString& icon, const QColor& fill)
    {
        painter.save();
        painter.fillRect(rect, fill);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(theme::borderStrong);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        painter.setPen(QPen(theme::textStrong, 2));
        if (icon == QStringLiteral("pencil")) {
            painter.drawLine(rect.left() + 5, rect.bottom() - 5, rect.right() - 5, rect.top() + 5);
            painter.drawLine(rect.left() + 4, rect.bottom() - 4, rect.left() + 7, rect.bottom() - 3);
        }
        painter.restore();
    }

    void drawLaneWaveform(QPainter& painter, int row, bool realLane)
    {
        const QRect area = laneTimelineRect(row).adjusted(10, 11, -10, -11);
        painter.setPen(theme::withAlpha(theme::gridBeat, 115));
        painter.drawLine(area.left(), area.center().y(), area.right(), area.center().y());
        if (!realLane || row >= lanes_.size() || lanes_[row].sourceFrames <= 0) {
            painter.setPen(theme::withAlpha(theme::textMuted, 130));
            painter.drawText(
                area.adjusted(12, 0, -12, 0),
                Qt::AlignCenter,
                realLane && !laneOriginText(lanes_[row].lane).startsWith(QStringLiteral("Empty"))
                    ? QStringLiteral("Loading waveform\u2026")
                    : QStringLiteral("Import audio or arm this track to record"));
            return;
        }
        const QRect clip = clipRect(row).intersected(area.adjusted(-1, -1, 1, 1));
        if (!clip.isValid()) return;
        const bool audible = laneIsAudible(row);
        const QColor waveformColor = laneWaveformColor(row);
        if (lanes_[row].peaks.empty()) {
            const bool remoteMissing = lanes_[row].lane.assetPath.trimmed().isEmpty() &&
                !lanes_[row].lane.assetHash.isEmpty();
            if (!remoteMissing) {
                painter.setPen(theme::withAlpha(theme::textMuted, 130));
                painter.drawText(area.adjusted(12, 0, -12, 0), Qt::AlignCenter,
                    QStringLiteral("Loading waveform\u2026"));
                return;
            }
            painter.save();
            const QColor pendingColor = theme::nebulaPurple;
            painter.setBrush(theme::withAlpha(pendingColor, 24));
            painter.setPen(QPen(theme::withAlpha(pendingColor, 150), 1, Qt::DashLine));
            painter.drawRoundedRect(clip, 6, 6);
            painter.setClipRect(clip.adjusted(1, 1, -1, -1));
            painter.setPen(QPen(theme::withAlpha(pendingColor, 38), 1));
            for (int x = clip.left() - clip.height(); x < clip.right(); x += 18) {
                painter.drawLine(x, clip.bottom(), x + clip.height(), clip.top());
            }
            painter.setClipping(false);
            QFont pendingFont(QStringLiteral("Bahnschrift"));
            pendingFont.setPixelSize(11);
            pendingFont.setWeight(QFont::DemiBold);
            painter.setFont(pendingFont);
            painter.setPen(theme::withAlpha(theme::textStrong, 205));
            const int rate = qMax(1, lanes_[row].lane.sampleRate);
            const qint64 seconds = (lanes_[row].sourceFrames + rate / 2) / rate;
            const QString duration = QStringLiteral("%1:%2")
                .arg(seconds / 60)
                .arg(seconds % 60, 2, 10, QLatin1Char('0'));
            painter.drawText(
                clip.adjusted(10, 0, -10, 0),
                Qt::AlignCenter,
                QFontMetrics(pendingFont).elidedText(
                    QStringLiteral("REMOTE WAV  \u00b7  AUDIO NOT SHARED  \u00b7  %1").arg(duration),
                    Qt::ElideRight,
                    qMax(1, clip.width() - 20)));
            painter.restore();
            return;
        }
        painter.setBrush(audible ? QColor(31, 22, 37) : QColor(20, 21, 24));
        painter.setPen(QPen(theme::withAlpha(waveformColor, audible ? 150 : 82), 1));
        painter.drawRoundedRect(clip, 6, 6);
        painter.save();
        QPainterPath clipPath;
        clipPath.addRoundedRect(clip.adjusted(1, 1, -1, -1), 5, 5);
        painter.setClipPath(clipPath);
        painter.setPen(theme::withAlpha(waveformColor, audible ? 225 : 105));
        const auto& peaks = lanes_[row].peaks;
        const qint64 firstSourceFrame = sourceStart(row);
        const qint64 croppedFrames = visibleFrames(row);
        const qint64 sourceFrames = lanes_[row].sourceFrames;
        for (int x = clip.left(); x <= clip.right(); ++x) {
            const qint64 frameOffset = static_cast<qint64>(x - clip.left()) * croppedFrames /
                qMax(1, clip.width());
            const qint64 sourceFrame = qMin(sourceFrames - 1, firstSourceFrame + frameOffset);
            const int index = static_cast<int>(qBound<qint64>(
                0,
                sourceFrame * static_cast<qint64>(peaks.size()) / sourceFrames,
                static_cast<qint64>(peaks.size()) - 1));
            const int half = qMax(2, static_cast<int>(peaks[index] * (clip.height() / 2 - 4)));
            painter.drawLine(x, clip.center().y() - half, x, clip.center().y() + half);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::withAlpha(audible ? theme::accent : waveformColor, audible ? 185 : 90));
        painter.drawRoundedRect(QRect(clip.left(), clip.top(), 5, clip.height()), 3, 3);
        painter.setBrush(theme::withAlpha(audible ? theme::nebulaCoral : waveformColor, audible ? 185 : 90));
        painter.drawRoundedRect(QRect(clip.right() - 4, clip.top(), 5, clip.height()), 3, 3);
        painter.restore();
    }

    void drawLaneAssetChrome(QPainter& painter)
    {
        QFont assetFont(QStringLiteral("Bahnschrift"));
        assetFont.setPixelSize(11);
        painter.setFont(assetFont);
        for (int row = 0; row < lanes_.size(); ++row) {
            if (!hasWav(row)) continue;
            const QRect card = laneTimelineRect(row);
            const bool selected = row == selectedLane_;
            const int labelRight = selected ? revealWavRect(row).left() - 10 : card.right() - 10;
            const QRect label(
                card.left() + 12,
                card.top() + 9,
                qMax(1, labelRight - card.left() - 12),
                24);
            const QString fileName = QFileInfo(lanes_[row].assetPath).fileName();
            painter.setPen(laneIsAudible(row) ? theme::text : theme::textMuted);
            painter.drawText(
                label,
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(assetFont).elidedText(
                    QStringLiteral("WAV  %1").arg(fileName),
                    Qt::ElideMiddle,
                    label.width()));
            if (!selected) continue;
            drawActionButton(
                painter,
                revealWavRect(row),
                QStringLiteral("SHOW FILE"),
                theme::accent,
                theme::withAlpha(theme::accent, 24),
                !interactionsProtected_);
            drawActionButton(
                painter,
                removeWavRect(row),
                QStringLiteral("REMOVE WAV"),
                theme::danger,
                theme::withAlpha(theme::nebulaRed, 34),
                !interactionsProtected_);
        }
    }

    int laneAt(const QPoint& pos) const
    {
        if (pos.y() < kToolbarHeight || pos.y() >= kToolbarHeight + visualLaneCount() * kLaneHeight) return -1;
        return qBound(0, (pos.y() - kToolbarHeight) / kLaneHeight, visualLaneCount() - 1);
    }

    void selectLane(int lane)
    {
        selectedLane_ = lane;
        if (onSelected) onSelected(lane);
        update();
    }

    void beginGainDrag(int lane, int x)
    {
        dragMode_ = DragMode::Gain;
        dragLane_ = lane;
        const QRect slider = gainRect(lane);
        const double t = qBound(
            0.0,
            static_cast<double>(x - slider.left()) / qMax(1, slider.width()),
            1.0);
        pendingGainDb_ = jam2::gui::trackGainDb(t);
        if (lane >= 0 && lane < lanes_.size()) {
            lanes_[lane].lane.gainDb = pendingGainDb_;
        }
        update();
    }

    void beginTimelineDrag(int lane, double x, DragMode mode)
    {
        // Capture the full mouse-to-frame transform before edit state changes.
        // Moving playhead/grid markers may repaint during a drag, but cannot
        // alter its scale, bounds, sample rate, tempo, or snapping policy.
        dragViewFrames_ = calculatedViewFrames();
        dragTimelineRect_ = timelineRect();
        dragMarkerSampleRate_ = markerSampleRate();
        dragBpm_ = bpm_;
        dragTempoPulseUnits_ = tempoPulseUnits_;
        dragGridLockEnabled_ = gridLockEnabled_;
        dragMode_ = mode;
        dragLane_ = lane;
        dragStartX_ = x;
        const LooperLane& source = lanes_[lane].lane;
        dragStartFrame_ = source.startFrame;
        dragSourceStartFrame_ = sourceStart(lane);
        dragSourceEndFrame_ = sourceEnd(lane);
    }

    void applyDragPreview(double x)
    {
        if (dragLane_ < 0 || dragLane_ >= lanes_.size()) return;
        const qint64 delta = frameDeltaForX(x - dragStartX_);
        LooperLane& lane = lanes_[dragLane_].lane;
        if (dragMode_ == DragMode::Move) {
            lane.startFrame = snapTimelineFrame(dragStartFrame_ + delta);
        } else if (dragMode_ == DragMode::LeftEdge) {
            const qint64 minSourceStart = qMax<qint64>(0, dragSourceStartFrame_ - dragStartFrame_);
            const qint64 snappedTimelineStart = snapTimelineFrame(dragStartFrame_ + delta);
            const qint64 snappedSourceStart = dragSourceStartFrame_ + snappedTimelineStart - dragStartFrame_;
            const qint64 next = qBound<qint64>(minSourceStart, snappedSourceStart, dragSourceEndFrame_ - 1);
            lane.loopStartFrame = next == 0 && dragSourceEndFrame_ == lanes_[dragLane_].sourceFrames ? -1 : next;
            lane.loopEndFrame = next == 0 && dragSourceEndFrame_ == lanes_[dragLane_].sourceFrames ? -1 : dragSourceEndFrame_;
            lane.startFrame = dragStartFrame_ + (next - dragSourceStartFrame_);
        } else if (dragMode_ == DragMode::RightEdge) {
            const qint64 originalTimelineEnd = dragStartFrame_ + (dragSourceEndFrame_ - dragSourceStartFrame_);
            const qint64 snappedTimelineEnd = snapTimelineFrame(originalTimelineEnd + delta);
            const qint64 snappedSourceEnd = dragSourceStartFrame_ + snappedTimelineEnd - dragStartFrame_;
            const qint64 next = qBound<qint64>(dragSourceStartFrame_ + 1, snappedSourceEnd, lanes_[dragLane_].sourceFrames);
            lane.loopStartFrame = dragSourceStartFrame_ == 0 && next == lanes_[dragLane_].sourceFrames ? -1 : dragSourceStartFrame_;
            lane.loopEndFrame = dragSourceStartFrame_ == 0 && next == lanes_[dragLane_].sourceFrames ? -1 : next;
        }
        lane.stopFrame = lane.startFrame + visibleFrames(dragLane_);
    }

    QVector<LaneView> lanes_;
    QMap<QString, QString> remoteRecordingStates_;
    bool interactionsProtected_ = false;
    int selectedLane_ = -1;
    int activeBank_ = 0;
    int armedLane_ = -1;
    double bpm_ = 120.0;
    bool gridLockEnabled_ = true;
    qint64 gridPositionMs_ = 0;
    int beatsPerBar_ = 4;
    int tempoPulseUnits_ = 1;
    bool gridRunning_ = false;
    qint64 playheadMs_ = -1;
    qint64 loopStartMs_ = -1;
    qint64 loopEndMs_ = -1;
    int markerSampleRate_ = 48000;
    qint64 minimumViewFrames_ = 1;
    qint64 liveRecordingEndFrame_ = 0;
    qint64 liveRecordingViewExtentFrames_ = 0;
    DragMode dragMode_ = DragMode::None;
    int dragLane_ = -1;
    double dragStartX_ = 0.0;
    qint64 dragStartFrame_ = 0;
    qint64 dragSourceStartFrame_ = 0;
    qint64 dragSourceEndFrame_ = 0;
    qint64 dragViewFrames_ = 0;
    QRect dragTimelineRect_;
    int dragMarkerSampleRate_ = 48000;
    double dragBpm_ = 120.0;
    int dragTempoPulseUnits_ = 1;
    bool dragGridLockEnabled_ = true;
    double pendingGainDb_ = 0.0;
    int timelineZoomLevel_ = 0;
    int dropLane_ = -1;
};

class LevelMeterWidget : public QWidget {
public:
    explicit LevelMeterWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumWidth(180);
        setFixedHeight(18);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setLevel(double level)
    {
        const double next = qBound(0.0, level, 1.0);
        if (!isEnabled()) {
            level_ = next;
            update();
            return;
        }
        const double delta = next - level_;
        if (std::abs(delta) < 0.008) {
            return;
        }
        const double smoothing = delta > 0.0 ? 0.45 : 0.10;
        level_ += delta * smoothing;
        if (level_ < 0.001 && next <= 0.001) {
            level_ = 0.0;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRect bar = rect().adjusted(0, 2, 0, -2);
        const bool active = isEnabled();
        painter.fillRect(bar, active ? theme::meterBg : theme::panelRaised);
        const int safeEnd = bar.left() + static_cast<int>(bar.width() * 0.50);
        const int warnEnd = bar.left() + static_cast<int>(bar.width() * 0.89);
        painter.fillRect(QRect(QPoint(safeEnd, bar.top()), QPoint(warnEnd, bar.bottom())),
            active ? theme::withAlpha(theme::warning, 64) : theme::panelBg);
        painter.fillRect(QRect(QPoint(warnEnd, bar.top()), QPoint(bar.right(), bar.bottom())),
            active ? theme::withAlpha(theme::danger, 64) : theme::panelBg);

        const int fillWidth = qBound(0, static_cast<int>(std::llround(level_ * static_cast<double>(bar.width()))), bar.width());
        QColor fill = active ? theme::meterSafe : theme::textMuted;
        if (active && level_ >= 0.891) {
            fill = theme::danger;
        } else if (active && level_ >= 0.501) {
            fill = theme::meterWarn;
        }
        painter.fillRect(QRect(bar.left(), bar.top(), fillWidth, bar.height()), fill);
        painter.setPen(active ? theme::border : theme::gridBeat);
        painter.drawRect(bar.adjusted(0, 0, -1, -1));
    }

private:
    double level_ = 0.0;
};
