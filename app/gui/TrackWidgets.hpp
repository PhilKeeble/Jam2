#pragma once

#include "GuiTheme.hpp"
#include "LooperProject.hpp"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPoint>
#include <QPolygon>
#include <QProxyStyle>
#include <QRect>
#include <QSizePolicy>
#include <QString>
#include <QStyleOption>
#include <QVector>
#include <QWidget>

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace jam2::gui {

inline int trackTimelineBeatNumber(int zeroBasedBeat) noexcept
{
    return qMax(0, zeroBasedBeat) + 1;
}

inline qint64 looperTimelineViewFrames(
    int markerSampleRate,
    qint64 arrangementEndFrame,
    qint64 loopStartMs,
    qint64 loopEndMs) noexcept
{
    const qint64 rate = qMax(1, markerSampleRate);
    qint64 frames = rate * 8;
    if (loopStartMs >= 0) frames = qMax(frames, loopStartMs * rate / 1000);
    if (loopEndMs >= 0) frames = qMax(frames, loopEndMs * rate / 1000);
    return qMax<qint64>(1, qMax(frames, arrangementEndFrame));
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
        durationMs_ = qMax<qint64>(0, durationMs);
        playheadMs_ = qBound<qint64>(0, playheadMs_, durationMs_);
        update();
    }

    void setBpm(double bpm)
    {
        bpm_ = qBound(1.0, bpm, 400.0);
        update();
    }

    void setGridPosition(qint64 positionMs, bool running, int beatsPerBar)
    {
        gridPositionMs_ = qMax<qint64>(0, positionMs);
        gridRunning_ = running;
        beatsPerBar_ = qMax(1, beatsPerBar);
        update();
    }

    void setPlayheadMs(qint64 positionMs)
    {
        playheadMs_ = qBound<qint64>(0, positionMs, qMax<qint64>(durationMs_, 0));
        update();
    }

    void setLoop(qint64 startMs, qint64 endMs)
    {
        loopStartMs_ = startMs >= 0 ? startMs : -1;
        loopEndMs_ = endMs >= 0 ? endMs : -1;
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
            const double beatMs = 60000.0 / bpm_;
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
                    QString::number(jam2::gui::trackTimelineBeatNumber(beat)));
            }
            if (gridRunning_) {
                const qint64 beatPosition = durationMs_ > 0 ? gridPositionMs_ % durationMs_ : 0;
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme::accent);
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
    std::function<void(int, double)> onGainChanged;
    std::function<void(int, qint64, qint64, qint64)> onRegionCommitted;
    std::function<void(int)> onBankSelected;
    std::function<void(qint64)> onSeekFrame;

    void setLanes(
        QVector<LaneView> lanes,
        int selected,
        int activeBank,
        int armedLane,
        int sampleRate,
        double bpm,
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
        activeBank_ = qBound(0, activeBank, 3);
        armedLane_ = armedLane;
        markerSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        bpm_ = qBound(1.0, bpm, 400.0);
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

    void setGridPosition(qint64 positionMs, bool running, double bpm, int beatsPerBar)
    {
        gridPositionMs_ = qMax<qint64>(0, positionMs);
        gridRunning_ = running;
        bpm_ = qBound(1.0, bpm, 400.0);
        beatsPerBar_ = qMax(1, beatsPerBar);
        update();
    }

    void setPlaybackMarkers(qint64 positionMs, qint64 loopStartMs, qint64 loopEndMs)
    {
        playheadMs_ = positionMs >= 0 ? positionMs : -1;
        loopStartMs_ = loopStartMs >= 0 ? loopStartMs : -1;
        loopEndMs_ = loopEndMs >= 0 ? loopEndMs : -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), theme::editorBg);
        painter.setRenderHint(QPainter::Antialiasing, false);

        drawToolbar(painter);

        const int laneCount = visualLaneCount();
        for (int row = 0; row < laneCount; ++row) {
            drawLane(painter, row);
        }

        drawOverlays(painter);

        const QRect plus = plusRect();
        painter.fillRect(plus, theme::buttonBg);
        painter.setPen(theme::borderStrong);
        painter.drawRect(plus.adjusted(0, 0, -1, -1));
        painter.setPen(theme::text);
        painter.drawText(plus.adjusted(0, 0, 0, 0), Qt::AlignCenter, QStringLiteral("+"));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        const QPoint pos = event->position().toPoint();
        if (plusRect().contains(pos)) {
            if (onAddLane) onAddLane();
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
        if (laneIndex >= lanes_.size()) {
            if (onAddLane) onAddLane();
            if (control == QStringLiteral("arm") && onArm) {
                onArm(laneIndex);
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
        if (control == QStringLiteral("gain")) {
            beginGainDrag(laneIndex, pos.y());
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
        if (laneTimelineRect(laneIndex).contains(pos) && onSeekFrame) {
            onSeekFrame(frameForX(pos.x()));
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const QPoint pos = event->position().toPoint();
        if (dragMode_ == DragMode::Gain && dragLane_ >= 0 && dragLane_ < lanes_.size()) {
            const QRect slider = gainRect(dragLane_);
            const double t = 1.0 - qBound(0.0, static_cast<double>(pos.y() - slider.top()) / qMax(1, slider.height()), 1.0);
            pendingGainDb_ = -60.0 + t * 72.0;
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

    static constexpr int kToolbarHeight = 34;
    static constexpr int kLaneHeight = 112;
    static constexpr int kHeaderWidth = 176;
    static constexpr int kPlusHeight = 34;

    int visualLaneCount() const
    {
        return qMax(1, lanes_.size());
    }

    void updateMinimumHeight()
    {
        setMinimumHeight(kToolbarHeight + visualLaneCount() * kLaneHeight + kPlusHeight + 12);
    }

    QRect timelineRect() const
    {
        return rect().adjusted(kHeaderWidth, kToolbarHeight, -1, -kPlusHeight - 8);
    }

    QRect laneRect(int row) const
    {
        return QRect(0, kToolbarHeight + row * kLaneHeight, width(), kLaneHeight);
    }

    QRect laneTimelineRect(int row) const
    {
        return laneRect(row).adjusted(kHeaderWidth, 0, -1, 0);
    }

    QRect plusRect() const
    {
        return QRect(0, kToolbarHeight + visualLaneCount() * kLaneHeight, width(), kPlusHeight);
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
        return jam2::gui::looperTimelineViewFrames(
            markerSampleRate(),
            arrangementEndFrame,
            loopStartMs_,
            loopEndMs_);
    }

    qint64 viewFrames() const
    {
        return timelineDragActive() && dragViewFrames_ > 0
            ? dragViewFrames_ : calculatedViewFrames();
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
        return area.width() > 1 ? static_cast<qint64>(std::llround((dx / area.width()) * viewFrames())) : 0;
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
        const double beatFrames = static_cast<double>(rate) * 60.0 / bpm;
        const qint64 beat = static_cast<qint64>(std::llround(static_cast<double>(frame) / beatFrames));
        return qMax<qint64>(0, static_cast<qint64>(std::llround(static_cast<double>(beat) * beatFrames)));
    }

    qint64 frameForX(double x) const
    {
        const QRect area = effectiveTimelineRect();
        if (area.width() <= 1) {
            return 0;
        }
        const double clamped = qBound(static_cast<double>(area.left()), x, static_cast<double>(area.right()));
        return qBound<qint64>(0, static_cast<qint64>(std::llround(((clamped - area.left()) / area.width()) * viewFrames())), viewFrames());
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
        if (bpm_ > 0.0) {
            const QRect area = QRect(kHeaderWidth, 0, width() - kHeaderWidth, kToolbarHeight);
            const double beatFrames = static_cast<double>(markerSampleRate()) * 60.0 / bpm_;
            const int beatCount = qBound(
                1,
                static_cast<int>(std::ceil(static_cast<double>(viewFrames()) / beatFrames)),
                2048);
            for (int beat = 0; beat < beatCount; ++beat) {
                const int x = xForFrame(static_cast<qint64>(std::llround(beat * beatFrames)));
                if (x >= area.right()) break;
                painter.setPen(beat % beatsPerBar_ == 0 ? theme::gridBar : theme::gridBeat);
                const int gridBottom = kToolbarHeight + visualLaneCount() * kLaneHeight - 1;
                painter.drawLine(x, 0, x, gridBottom);
                painter.setPen(beat % beatsPerBar_ == 0
                    ? theme::text : theme::textMuted);
                painter.drawText(
                    x + 4, 20,
                    QString::number(jam2::gui::trackTimelineBeatNumber(beat)));
            }
        }
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
            painter.setPen(QPen(theme::withAlpha(theme::accent, 150), 1));
            painter.drawLine(x, top, x, bottom);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::accent);
            painter.drawEllipse(QPoint(x, kToolbarHeight - 8), 4, 4);
        }
        drawMarker(playheadMs_, theme::playhead, 2);
    }

    void drawLane(QPainter& painter, int row)
    {
        const bool realLane = row < lanes_.size();
        const LooperLane lane = realLane ? lanes_[row].lane : LooperLane{QString(), QString(), QString(), QStringLiteral("Empty Track 1")};
        const QRect rowRect = laneRect(row);
        const bool selected = row == selectedLane_;
        painter.fillRect(rowRect.adjusted(0, 0, 0, -1), selected ? theme::selection : theme::panelBg);
        painter.fillRect(rowRect.adjusted(kHeaderWidth, 0, 0, -1), theme::editorBg);
        painter.setPen(theme::border);
        painter.drawLine(0, rowRect.bottom(), width(), rowRect.bottom());
        painter.drawLine(kHeaderWidth, rowRect.top(), kHeaderWidth, rowRect.bottom());

        drawLaneHeader(painter, row, lane, realLane);
        drawLaneWaveform(painter, row, realLane);
    }

    QRect controlRect(int row, const QString& control) const
    {
        const QRect lane = laneRect(row);
        if (control == QStringLiteral("mute")) return QRect(30, lane.top() + 36, 70, 20);
        if (control == QStringLiteral("solo")) return QRect(30, lane.top() + 59, 70, 20);
        if (control == QStringLiteral("arm")) return QRect(30, lane.top() + 82, 70, 20);
        if (control == QStringLiteral("rename")) return QRect(kHeaderWidth - 58, lane.top() + 9, 20, 20);
        if (control == QStringLiteral("remove")) return QRect(kHeaderWidth - 30, lane.top() + 9, 20, 20);
        return {};
    }

    QRect gainRect(int row) const
    {
        const QRect lane = laneRect(row);
        return QRect(8, lane.top() + 42, 12, lane.height() - 54);
    }

    QString controlAt(int row, const QPoint& pos) const
    {
        for (const QString& control : {QStringLiteral("mute"), QStringLiteral("solo"), QStringLiteral("arm"), QStringLiteral("rename"), QStringLiteral("remove")}) {
            if (controlRect(row, control).contains(pos)) return control;
        }
        if (gainRect(row).adjusted(-6, -2, 6, 2).contains(pos)) return QStringLiteral("gain");
        return {};
    }

    void drawLaneHeader(QPainter& painter, int row, const LooperLane& lane, bool realLane)
    {
        const QRect r = laneRect(row);
        painter.setPen(theme::textStrong);
        painter.drawText(
            QRect(28, r.top() + 8, kHeaderWidth - 92, 22),
            Qt::AlignLeft | Qt::AlignVCenter,
            lane.name.isEmpty() ? QStringLiteral("Empty Track %1").arg(row + 1) : lane.name);

        drawButton(painter, controlRect(row, QStringLiteral("mute")), QStringLiteral("Mute"), realLane && lane.muted ? theme::warning : theme::buttonBg);
        drawButton(painter, controlRect(row, QStringLiteral("solo")), QStringLiteral("Solo"), realLane && lane.solo ? theme::success : theme::buttonBg);
        drawButton(painter, controlRect(row, QStringLiteral("arm")), QStringLiteral("Record"), row == armedLane_ ? theme::record : theme::buttonBg);
        drawIconButton(painter, controlRect(row, QStringLiteral("rename")), QStringLiteral("pencil"), theme::buttonBg);
        drawButton(painter, controlRect(row, QStringLiteral("remove")), QStringLiteral("X"), theme::withAlpha(theme::danger, 96));

        const QRect slider = gainRect(row);
        painter.fillRect(slider, theme::meterBg);
        painter.setPen(theme::border);
        painter.drawRect(slider.adjusted(0, 0, -1, -1));
        const double t = qBound(0.0, (lane.gainDb + 60.0) / 72.0, 1.0);
        const int y = slider.bottom() - static_cast<int>(std::llround(t * slider.height()));
        painter.fillRect(QRect(slider.left(), y, slider.width(), slider.bottom() - y + 1), theme::accent);
    }

    void drawButton(QPainter& painter, const QRect& rect, const QString& text, const QColor& fill)
    {
        painter.fillRect(rect, fill);
        painter.setPen(theme::borderStrong);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        painter.setPen(theme::textStrong);
        painter.drawText(rect, Qt::AlignCenter, text);
    }

    void drawIconButton(QPainter& painter, const QRect& rect, const QString& icon, const QColor& fill)
    {
        painter.fillRect(rect, fill);
        painter.setPen(theme::borderStrong);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        painter.setPen(QPen(theme::textStrong, 2));
        if (icon == QStringLiteral("pencil")) {
            painter.drawLine(rect.left() + 5, rect.bottom() - 5, rect.right() - 5, rect.top() + 5);
            painter.drawLine(rect.left() + 4, rect.bottom() - 4, rect.left() + 7, rect.bottom() - 3);
        }
    }

    void drawLaneWaveform(QPainter& painter, int row, bool realLane)
    {
        const QRect area = laneTimelineRect(row).adjusted(0, 12, -1, -12);
        painter.setPen(theme::gridBeat);
        painter.drawLine(area.left(), area.center().y(), area.right(), area.center().y());
        if (!realLane || row >= lanes_.size() || lanes_[row].sourceFrames <= 0 || lanes_[row].peaks.empty()) {
            painter.setPen(theme::border);
            painter.drawLine(area.left(), area.center().y(), area.right(), area.center().y());
            return;
        }
        const QRect clip = clipRect(row);
        painter.fillRect(clip, theme::clipBg);
        painter.setPen(theme::waveform);
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
        painter.fillRect(QRect(clip.left(), clip.top(), 6, clip.height()), theme::withAlpha(theme::accent, 180));
        painter.fillRect(QRect(clip.right() - 5, clip.top(), 6, clip.height()), theme::withAlpha(theme::accent, 180));
        painter.setPen(theme::textStrong);
        painter.drawText(clip.adjusted(6, 2, -6, -2), Qt::AlignLeft | Qt::AlignTop, lanes_[row].lane.name);
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

    void beginGainDrag(int lane, int)
    {
        dragMode_ = DragMode::Gain;
        dragLane_ = lane;
        pendingGainDb_ = lanes_.value(lane).lane.gainDb;
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
    int selectedLane_ = -1;
    int activeBank_ = 0;
    int armedLane_ = -1;
    double bpm_ = 120.0;
    bool gridLockEnabled_ = true;
    qint64 gridPositionMs_ = 0;
    int beatsPerBar_ = 4;
    bool gridRunning_ = false;
    qint64 playheadMs_ = -1;
    qint64 loopStartMs_ = -1;
    qint64 loopEndMs_ = -1;
    int markerSampleRate_ = 48000;
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
    bool dragGridLockEnabled_ = true;
    double pendingGainDb_ = 0.0;
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
