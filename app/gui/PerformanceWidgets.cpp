#include "PerformanceWidgets.hpp"

#include "GuiPresentation.hpp"
#include "GuiTheme.hpp"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

namespace theme = jam2::gui::theme;

constexpr int kPeerVisibleCount = 10;
constexpr int kPeerChipWidth = 104;

QColor nebulaRed()
{
    return QColor(190, 52, 91);
}

QColor nebulaPurple()
{
    return QColor(119, 83, 190);
}

QColor nebulaCyan()
{
    return QColor(61, 187, 190);
}

QColor gold()
{
    return QColor(226, 172, 83);
}

QString sectionName(const SongSection& section)
{
    const QString name = section.name.trimmed();
    return name.isEmpty() ? QStringLiteral("Section") : name;
}

QString normalizedHits(const QString& text, int division)
{
    QString result;
    result.reserve(division);
    const QString trimmed = text.trimmed();
    for (int index = 0; index < division; ++index) {
        const QChar value = index < trimmed.size() ? trimmed.at(index).toLower() : QChar('.');
        result.append(
            value == QLatin1Char('a') || value == QLatin1Char('g') ||
                value == QLatin1Char('x') || value == QLatin1Char('1')
            ? (value == QLatin1Char('1') ? QChar('x') : value)
            : QChar('.'));
    }
    return result;
}

} // namespace

PerformanceHomeWidget::PerformanceHomeWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(820, 500);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    history_.fill(0.0, 72);
    stars_.reserve(150);
    QRandomGenerator random(0x4a414d32U);
    for (int index = 0; index < 150; ++index) {
        stars_.push_back(QPointF(random.generateDouble(), random.generateDouble()));
    }
    animationClock_.start();
    renderWindow_.start();
    animationTimer_.setInterval(33);
    QObject::connect(&animationTimer_, &QTimer::timeout, this, [this] {
        advanceAnimation();
    });
    animationTimer_.start();
}

void PerformanceHomeWidget::setSongModel(const BeatGridModel* model)
{
    model_ = model;
    update();
}

void PerformanceHomeWidget::setTiming(
    quint64 absoluteBeat,
    int subdivision,
    int beatsPerBar,
    double beatPhase,
    bool running)
{
    absoluteBeat_ = absoluteBeat;
    subdivision_ = qMax(0, subdivision);
    beatsPerBar_ = qMax(1, beatsPerBar);
    beatPhase_ = qBound(0.0, beatPhase, 0.999999);
    running_ = running;
    update();
}

void PerformanceHomeWidget::setAudioPeaks(const jam2::EngineGuiPeakSnapshot& peaks)
{
    const auto normalized = [](int ppm) {
        const double value = qBound(0.0, static_cast<double>(ppm) / 1000000.0, 1.0);
        return value < 0.001
            ? 0.0
            : qMin(1.0, std::pow(value, 0.48) * 1.45);
    };
    const double input = normalized(peaks.input_peak_ppm);
    const double remote = normalized(peaks.remote_peak_ppm);
    const double track = normalized(peaks.prepared_track_peak_ppm);
    targetEnergy_ = qBound(
        0.0,
        1.0 - (1.0 - input) * (1.0 - remote) * (1.0 - track),
        1.0);
}

void PerformanceHomeWidget::setTunerSnapshot(const jam2::EnginePitchSnapshot& snapshot)
{
    tuner_ = snapshot;
    if (!tuner_.enabled) {
        tunerExpanded_ = false;
    }
    update();
}

void PerformanceHomeWidget::setPeers(QVector<PerformancePeerPresentation> peers)
{
    peers_ = std::move(peers);
    const int maximumOffset = qMax(0, peers_.size() - peerVisibleCapacity());
    peerScrollOffset_ = qBound(0, peerScrollOffset_, maximumOffset);
    update();
}

void PerformanceHomeWidget::setSelectedPeer(std::uint64_t peerId)
{
    if (selectedPeerId_ == peerId) {
        return;
    }
    selectedPeerId_ = peerId;
    update();
}

void PerformanceHomeWidget::setTrackGainDb(double gainDb)
{
    const double bounded = qBound(-60.0, gainDb, 12.0);
    if (qFuzzyCompare(trackGainDb_, bounded)) {
        return;
    }
    trackGainDb_ = bounded;
    update();
}

void PerformanceHomeWidget::setTrackWaveform(std::vector<float> peaks, bool valid)
{
    trackWaveformPeaks_ = std::move(peaks);
    trackWaveformValid_ = valid && !trackWaveformPeaks_.empty();
    update();
}

void PerformanceHomeWidget::setTechnicalSummary(
    const QString& rtt,
    const QString& jitter,
    const QString& loss,
    const QString& xruns)
{
    rtt_ = rtt;
    jitter_ = jitter;
    loss_ = loss;
    xruns_ = xruns;
    update();
}

QString PerformanceHomeWidget::rendererStatsText() const
{
    if (!tuner_.enabled) {
        return rendererStats_ + QStringLiteral(" | tuner off");
    }
    const double averageUs = tuner_.analyzed_windows > 0
        ? static_cast<double>(tuner_.processing_time_sum_us) /
            static_cast<double>(tuner_.analyzed_windows)
        : 0.0;
    return rendererStats_ + QStringLiteral(
        " | tuner tap %1 hops %2 windows %3 rejected %4 avg %5 us max %6 us ring %7/%8 overruns %9")
        .arg(tuner_.callback_tap_enabled ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(static_cast<qulonglong>(tuner_.input_hops))
        .arg(static_cast<qulonglong>(tuner_.analyzed_windows))
        .arg(static_cast<qulonglong>(tuner_.rejected_windows))
        .arg(averageUs, 0, 'f', 1)
        .arg(static_cast<qulonglong>(tuner_.processing_time_max_us))
        .arg(tuner_.ring_depth_frames)
        .arg(tuner_.ring_capacity_frames)
        .arg(static_cast<qulonglong>(tuner_.ring.overruns));
}

PerformanceHomeWidget::SongPosition PerformanceHomeWidget::songPosition(
    quint64 absoluteBeat) const
{
    const quint64 total =
        model_ != nullptr && !model_->sections().isEmpty()
        ? static_cast<quint64>(qMax(0, model_->section(0).beats))
        : 0;
    if (total == 0) {
        return {};
    }
    SongPosition result = songPositionFromSongBeat(absoluteBeat % total);
    result.totalBeats = total;
    return result;
}

PerformanceHomeWidget::SongPosition PerformanceHomeWidget::songPositionFromSongBeat(
    quint64 songBeat) const
{
    SongPosition result;
    if (model_ == nullptr || model_->sections().isEmpty()) {
        return result;
    }
    const quint64 total =
        static_cast<quint64>(qMax(0, model_->section(0).beats));
    result.totalBeats = total;
    if (total == 0) {
        return result;
    }
    result.songBeat = songBeat % total;
    result.section = 0;
    result.sectionBeat = static_cast<int>(result.songBeat);
    return result;
}

QString PerformanceHomeWidget::chordAt(const SongPosition& position) const
{
    if (model_ == nullptr || position.totalBeats == 0) {
        return QStringLiteral("—");
    }
    for (quint64 distance = 0; distance < position.totalBeats; ++distance) {
        const quint64 candidateBeat =
            (position.songBeat + position.totalBeats - distance) % position.totalBeats;
        const SongPosition candidate = songPositionFromSongBeat(candidateBeat);
        const QString value =
            model_->section(candidate.section).chords.value(candidate.sectionBeat).trimmed();
        if (value == QStringLiteral("-")) {
            return QStringLiteral("—");
        }
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QStringLiteral("—");
}

QVector<QPair<QString, QString>> PerformanceHomeWidget::upcomingChords(
    const SongPosition& position) const
{
    QVector<QPair<QString, QString>> result;
    if (model_ == nullptr || position.totalBeats == 0) {
        return result;
    }
    QString previous = chordAt(position);
    for (quint64 distance = 1;
         distance <= position.totalBeats && result.size() < 3;
         ++distance) {
        const SongPosition candidate =
            songPositionFromSongBeat((position.songBeat + distance) % position.totalBeats);
        QString value =
            model_->section(candidate.section).chords.value(candidate.sectionBeat).trimmed();
        if (value.isEmpty()) {
            continue;
        }
        if (value == QStringLiteral("-")) {
            value = QStringLiteral("—");
        }
        if (value == previous) {
            continue;
        }
        const QString location = QStringLiteral("%1.%2")
            .arg(candidate.songBeat / static_cast<quint64>(beatsPerBar_) + 1)
            .arg(candidate.songBeat % static_cast<quint64>(beatsPerBar_) + 1);
        result.push_back({value, location});
        previous = value;
    }
    return result;
}

QPair<QString, QString> PerformanceHomeWidget::lyricLines(
    const SongPosition& position) const
{
    if (model_ == nullptr || position.totalBeats == 0) {
        return {};
    }
    QString current;
    for (quint64 beat = 0; beat <= position.songBeat; ++beat) {
        const SongPosition candidate = songPositionFromSongBeat(beat);
        const QString cue =
            model_->section(candidate.section).lyrics.value(candidate.sectionBeat).trimmed();
        if (!cue.isEmpty()) {
            current = cue;
        }
    }
    QString next;
    for (quint64 beat = position.songBeat + 1; beat < position.totalBeats; ++beat) {
        const SongPosition candidate = songPositionFromSongBeat(beat);
        const QString cue =
            model_->section(candidate.section).lyrics.value(candidate.sectionBeat).trimmed();
        if (!cue.isEmpty()) {
            next = cue;
            break;
        }
    }
    return {current, next};
}

int PerformanceHomeWidget::peerVisibleCapacity() const
{
    if (width() >= 900) {
        return 10;
    }
    return qBound(
        1,
        qMax(1, width() - 48) / kPeerChipWidth,
        kPeerVisibleCount);
}

void PerformanceHomeWidget::rebuildBackground()
{
    if (width() <= 0 || height() <= 0) {
        nebulaCache_ = {};
        return;
    }
    const QSize cacheSize(
        qMax(1, static_cast<int>(std::lround(width() * 0.65))),
        qMax(1, static_cast<int>(std::lround(height() * 0.65))));
    nebulaCache_ = QImage(cacheSize, QImage::Format_ARGB32_Premultiplied);
    nebulaCache_.fill(QColor(4, 5, 13));
    QPainter painter(&nebulaCache_);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto cloud = [&painter, cacheSize](
                           QPointF center,
                           double radius,
                           QColor color,
                           int alpha) {
        const QPointF point(center.x() * cacheSize.width(), center.y() * cacheSize.height());
        const double pixels = radius * std::max(cacheSize.width(), cacheSize.height());
        color.setAlpha(alpha);
        QRadialGradient gradient(point, pixels);
        gradient.setColorAt(0.0, color);
        QColor transparent = color;
        transparent.setAlpha(0);
        gradient.setColorAt(0.72, QColor(color.red(), color.green(), color.blue(), alpha / 3));
        gradient.setColorAt(1.0, transparent);
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawEllipse(point, pixels, pixels * 0.72);
    };
    cloud(QPointF(0.43, 0.42), 0.38, nebulaPurple(), 178);
    cloud(QPointF(0.58, 0.48), 0.34, nebulaRed(), 166);
    cloud(QPointF(0.50, 0.36), 0.24, nebulaCyan(), 105);
    cloud(QPointF(0.34, 0.56), 0.20, QColor(231, 116, 97), 90);
}

void PerformanceHomeWidget::advanceAnimation()
{
    if (!isVisible()) {
        return;
    }
    const qint64 now = animationClock_.elapsed();
    const double delta = qBound(0.001, static_cast<double>(now - lastAnimationMs_) / 1000.0, 0.1);
    lastAnimationMs_ = now;
    const double timeConstant = targetEnergy_ > envelope_ ? 0.045 : 0.78;
    const double coefficient = 1.0 - std::exp(-delta / timeConstant);
    envelope_ += (targetEnergy_ - envelope_) * coefficient;
    if (tuner_.valid) {
        displayedTunerCents_ +=
            (qBound(-50.0, tuner_.cents, 50.0) - displayedTunerCents_) * 0.28;
        tunerOrbOpacity_ += (1.0 - tunerOrbOpacity_) * 0.32;
    } else {
        tunerOrbOpacity_ *= 0.78;
    }
    if (history_.isEmpty()) {
        history_.fill(0.0, 72);
    }
    history_.removeFirst();
    history_.push_back(envelope_);
    if (running_ && now >= nextNovaMs_ && novaStartMs_ < 0) {
        novaStartMs_ = now;
        novaPosition_ = QPointF(
            0.12 + QRandomGenerator::global()->generateDouble() * 0.76,
            0.10 + QRandomGenerator::global()->generateDouble() * 0.48);
        nextNovaMs_ = now + 45000 +
            static_cast<qint64>(QRandomGenerator::global()->bounded(75001));
    }
    if (novaStartMs_ >= 0 && now - novaStartMs_ > 1800) {
        novaStartMs_ = -1;
    }
    update();
}

void PerformanceHomeWidget::paintNebulaFields(
    QPainter& painter,
    double seconds,
    double participantComplexity)
{
    const QRectF viewport(rect());
    const double overscanX = width() * 0.18;
    const double overscanY = height() * 0.18;
    const QRectF field = viewport.adjusted(
        -overscanX, -overscanY, overscanX, overscanY);
    const double motion = 0.30 + envelope_ * 0.70;
    const QPointF primaryOffset(
        std::sin(seconds * (0.19 + participantComplexity * 0.15)) *
            width() * 0.035 * motion,
        std::cos(seconds * (0.15 + participantComplexity * 0.12)) *
            height() * 0.028 * motion);
    const QPointF counterOffset(
        std::cos(seconds * (0.13 + participantComplexity * 0.11)) *
            width() * 0.045 * motion,
        std::sin(seconds * (0.17 + participantComplexity * 0.13)) *
            height() * 0.035 * motion);

    painter.save();
    painter.setClipRect(viewport);
    painter.setOpacity(0.62 + envelope_ * 0.16);
    painter.drawImage(field.translated(primaryOffset), nebulaCache_);

    if (envelope_ > 0.015) {
        painter.setOpacity(qMin(
            0.22,
            envelope_ * (0.10 + participantComplexity * 0.12)));
        painter.drawImage(
            field.adjusted(-overscanX * 0.22, -overscanY * 0.18,
                           overscanX * 0.22, overscanY * 0.18)
                .translated(counterOffset),
            nebulaCache_);
    }
    painter.restore();
}

void PerformanceHomeWidget::paintHtmlStage()
{
    QElapsedTimer paintClock;
    paintClock.start();
    if (nebulaCache_.isNull()) {
        rebuildBackground();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(3, 5, 6));
    painter.setPen(Qt::NoPen);
    for (int index = 0; index < stars_.size(); ++index) {
        const QPointF point(stars_[index].x() * width(), stars_[index].y() * height());
        const int alpha = 48 + (index % 5) * 20;
        painter.setBrush(QColor(220, 226, 225, alpha));
        const double radius = index % 13 == 0 ? 1.5 : 0.75;
        painter.drawEllipse(point, radius, radius);
    }

    const double seconds = static_cast<double>(animationClock_.elapsed()) / 1000.0;
    const double participantComplexity =
        qBound(0.0, static_cast<double>(peers_.size()) / 8.0, 1.0);
    paintNebulaFields(painter, seconds, participantComplexity);

    if (novaStartMs_ >= 0) {
        const double age =
            static_cast<double>(animationClock_.elapsed() - novaStartMs_) / 1800.0;
        const double alpha = std::sin(qBound(0.0, age, 1.0) * 3.14159265358979323846);
        const QPointF center(novaPosition_.x() * width(), novaPosition_.y() * height());
        QRadialGradient nova(center, 42.0 + 24.0 * age);
        nova.setColorAt(0.0, QColor(255, 245, 221, static_cast<int>(150 * alpha)));
        nova.setColorAt(0.2, QColor(230, 117, 107, static_cast<int>(80 * alpha)));
        nova.setColorAt(1.0, QColor(230, 117, 107, 0));
        painter.setBrush(nova);
        painter.drawEllipse(center, 70, 70);
    }

    const int margin = 24;
    const SongPosition position = songPosition(absoluteBeat_);
    const SongSection* section =
        model_ != nullptr && position.section < model_->sections().size()
        ? &model_->section(position.section)
        : nullptr;

    QFont micro(QStringLiteral("Bahnschrift"));
    micro.setPointSizeF(8.5);
    micro.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    QFont sectionFont(QStringLiteral("Georgia"));
    sectionFont.setPointSizeF(18.0);
    painter.setFont(sectionFont);
    painter.setPen(QColor(233, 230, 220));
    painter.drawText(
        QRect(margin + 3, 15, width() / 3, 42),
        Qt::AlignLeft | Qt::AlignVCenter,
        section != nullptr ? sectionName(*section) : QStringLiteral("No song section"));

    const QPair<QString, QString> lyrics = lyricLines(position);
    lyricsHitRect_ = QRect(
        margin + 4,
        62,
        qMin(390, width() / 3),
        lyrics.first.isEmpty() && lyrics.second.isEmpty() ? 0 : 76);
    if (lyricsHitRect_.height() > 0) {
        QLinearGradient panel(lyricsHitRect_.left(), 0, lyricsHitRect_.right(), 0);
        panel.setColorAt(0.0, QColor(12, 17, 18, 220));
        panel.setColorAt(0.74, QColor(12, 17, 18, 80));
        panel.setColorAt(1.0, QColor(12, 17, 18, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(panel);
        painter.drawRect(lyricsHitRect_);
        painter.fillRect(
            QRect(lyricsHitRect_.left(), lyricsHitRect_.top(), 2, lyricsHitRect_.height()),
            QColor(176, 139, 228));
        QFont lyricFont(QStringLiteral("Georgia"));
        lyricFont.setPointSizeF(16.0);
        painter.setFont(lyricFont);
        painter.setPen(QColor(233, 230, 220));
        painter.drawText(
            lyricsHitRect_.adjusted(14, 7, -8, -36),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(lyricFont).elidedText(
                lyrics.first, Qt::ElideRight, lyricsHitRect_.width() - 28));
        lyricFont.setPointSizeF(14.0);
        painter.setFont(lyricFont);
        painter.setPen(QColor(181, 192, 193));
        painter.drawText(
            lyricsHitRect_.adjusted(14, 36, -8, -7),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(lyricFont).elidedText(
                lyrics.second, Qt::ElideRight, lyricsHitRect_.width() - 28));
    }

    const QStringList technical{rtt_, jitter_, loss_, xruns_};
    const QStringList labels{
        QStringLiteral("PEER RTT"),
        QStringLiteral("JITTER"),
        QStringLiteral("LOSS"),
        QStringLiteral("XRUNS")};
    const int statWidth = 90;
    for (int index = 0; index < technical.size(); ++index) {
        const QRect cell(
            width() - margin - statWidth * (technical.size() - index),
            17,
            statWidth - 6,
            45);
        painter.setPen(QPen(QColor(96, 116, 121, 120), 1));
        painter.setBrush(QColor(7, 11, 12, 190));
        painter.drawRoundedRect(cell, 3, 3);
        QFont valueFont(QStringLiteral("Bahnschrift"));
        valueFont.setPointSizeF(10.0);
        painter.setFont(valueFont);
        painter.setPen(QColor(225, 229, 224));
        painter.drawText(cell.adjusted(8, 5, -5, -19), Qt::AlignLeft, technical.at(index));
        QFont labelFont(QStringLiteral("Bahnschrift"));
        labelFont.setPointSizeF(8.0);
        labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
        painter.setFont(labelFont);
        painter.setPen(QColor(156, 169, 171));
        painter.drawText(
            cell.adjusted(8, 23, -5, -4),
            Qt::AlignLeft | Qt::AlignVCenter,
            labels.at(index));
    }
    paintGenerationActions(painter, 72, width() - margin);

    const int previewGap = 8;
    const int previewHeight = qBound(160, height() / 3, 260);
    const int previewTop = height() - margin - previewHeight;
    const int peerWidth = 112;
    const int looperWidth = width() >= 1100 ? 220 : 184;
    const int previewLeft = margin + peerWidth + 18;
    const int previewRight = width() - margin - looperWidth - 18;
    const int previewWidth = qMax(
        180,
        (previewRight - previewLeft - previewGap) / 2);

    const double orbitCenterY = qBound(
        150.0,
        static_cast<double>(previewTop - 210),
        height() * 0.40);
    const QPointF orbitCenter(width() * 0.50, orbitCenterY);
    const double orbitRadius = qBound(
        128.0,
        qMin(width() * 0.16, qMax(128.0, (previewTop - 96.0) * 0.46)),
        175.0);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(102, 212, 207, 30), 18));
    painter.drawEllipse(orbitCenter, orbitRadius, orbitRadius);
    painter.setPen(QPen(QColor(80, 96, 100), 1));
    painter.drawEllipse(orbitCenter, orbitRadius + 3, orbitRadius + 3);
    painter.setPen(QPen(QColor(38, 52, 56), 1));
    painter.drawEllipse(orbitCenter, orbitRadius * 0.86, orbitRadius * 0.86);
    painter.drawEllipse(orbitCenter, orbitRadius * 0.68, orbitRadius * 0.68);

    for (int tick = 0; tick < 64; ++tick) {
        const double angle = static_cast<double>(tick) / 64.0 *
            6.283185307179586 - 1.5707963267948966;
        const double length = tick % 8 == 0 ? 8.0 : 4.0;
        const QPointF outer(
            orbitCenter.x() + std::cos(angle) * (orbitRadius + 3.0),
            orbitCenter.y() + std::sin(angle) * (orbitRadius + 3.0));
        const QPointF inner(
            orbitCenter.x() + std::cos(angle) * (orbitRadius + 3.0 - length),
            orbitCenter.y() + std::sin(angle) * (orbitRadius + 3.0 - length));
        painter.setPen(QPen(
            tick % 8 == 0 ? QColor(87, 104, 108) : QColor(52, 67, 70),
            1));
        painter.drawLine(inner, outer);
    }

    QPainterPath activity;
    for (int index = 0; index < history_.size(); ++index) {
        const double angle =
            static_cast<double>(index) / history_.size() * 6.283185307179586;
        const double energy = history_.at(index);
        const double flow =
            std::sin(
                angle * (2.0 + qMin(6, peers_.size())) +
                seconds * (0.7 + participantComplexity)) *
            participantComplexity * (0.018 + energy * 0.07);
        const double radius = orbitRadius * (0.78 + energy * 0.36 + flow);
        const QPointF point(
            orbitCenter.x() + std::cos(angle) * radius,
            orbitCenter.y() + std::sin(angle) * radius);
        if (index == 0) activity.moveTo(point);
        else activity.lineTo(point);
        const QPointF spokeStart(
            orbitCenter.x() + std::cos(angle) * orbitRadius * 0.79,
            orbitCenter.y() + std::sin(angle) * orbitRadius * 0.79);
        const QColor spokeColor =
            index % 3 == 0 ? QColor(112, 234, 215, 55 + static_cast<int>(energy * 150))
            : index % 3 == 1 ? QColor(176, 139, 228, 45 + static_cast<int>(energy * 135))
            : QColor(255, 125, 134, 40 + static_cast<int>(energy * 125));
        painter.setPen(QPen(spokeColor, energy > 0.6 ? 2.0 : 1.0));
        painter.drawLine(spokeStart, point);
    }
    activity.closeSubpath();
    painter.setPen(QPen(QColor(102, 212, 207, 150), 1.5));
    painter.drawPath(activity);

    const int sourceCount = qMin(10, peers_.size() + 1);
    for (int source = 0; source < sourceCount; ++source) {
        const double angle = static_cast<double>(source) /
            qMax(1, sourceCount) * 6.283185307179586 - 1.5707963267948966;
        const QPointF node(
            orbitCenter.x() + std::cos(angle) * (orbitRadius + 3.0),
            orbitCenter.y() + std::sin(angle) * (orbitRadius + 3.0));
        const QColor nodeColor =
            source == 0 ? QColor(232, 164, 74)
            : source % 4 == 1 ? QColor(102, 212, 207)
            : source % 4 == 2 ? QColor(160, 146, 218)
            : source % 4 == 3 ? QColor(223, 107, 97)
            : QColor(112, 212, 157);
        painter.setPen(QPen(QColor(235, 237, 230, 120), 1));
        painter.setBrush(nodeColor);
        painter.drawEllipse(node, source == 0 ? 4.5 : 3.5, source == 0 ? 4.5 : 3.5);
    }

    const int lensDiameter = qBound(184, static_cast<int>(orbitRadius * 1.27), 224);
    chordHitRect_ = QRect(
        static_cast<int>(orbitCenter.x() - lensDiameter / 2.0),
        static_cast<int>(orbitCenter.y() - lensDiameter / 2.0),
        lensDiameter,
        lensDiameter);
    QRadialGradient lens(
        QPointF(chordHitRect_.center().x() - 5, chordHitRect_.center().y() - 9),
        chordHitRect_.width() / 2.0);
    lens.setColorAt(0.0, QColor(32, 56, 59, 245));
    lens.setColorAt(0.48, QColor(17, 29, 31, 245));
    lens.setColorAt(1.0, QColor(7, 11, 12, 245));
    painter.setBrush(lens);
    painter.setPen(QPen(QColor(91, 107, 110), 1));
    painter.drawEllipse(chordHitRect_);
    painter.setPen(QPen(QColor(47, 59, 62, 118), 8));
    painter.drawEllipse(chordHitRect_.adjusted(8, 8, -8, -8));

    const QRect progressRect = chordHitRect_.adjusted(-11, -11, 11, 11);
    const double songProgress = position.totalBeats > 0
        ? (static_cast<double>(position.songBeat) + beatPhase_) /
            static_cast<double>(position.totalBeats)
        : 0.0;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(89, 107, 111, 90), 3));
    painter.drawEllipse(progressRect);
    painter.setPen(QPen(QColor(232, 164, 74), 4, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(
        progressRect,
        90 * 16,
        -static_cast<int>(songProgress * 360.0 * 16.0));

    QFont lensLabel(QStringLiteral("Bahnschrift"));
    lensLabel.setPointSizeF(8.0);
    lensLabel.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);
    painter.setFont(lensLabel);
    painter.setPen(QColor(102, 212, 207));
    painter.drawText(
        chordHitRect_.adjusted(0, 25, 0, 0),
        Qt::AlignHCenter | Qt::AlignTop,
        QStringLiteral("CURRENT CHORD"));

    const QString currentChord = chordAt(position);
    QFont chordFont(QStringLiteral("Georgia"));
    chordFont.setWeight(QFont::Normal);
    double chordPointSize = 58.0;
    const int maximumChordWidth = chordHitRect_.width() - 38;
    while (chordPointSize > 25.0) {
        chordFont.setPointSizeF(chordPointSize);
        if (QFontMetricsF(chordFont).horizontalAdvance(currentChord) <= maximumChordWidth) {
            break;
        }
        chordPointSize -= 2.0;
    }
    chordFont.setPointSizeF(chordPointSize);
    QPainterPath chordPath;
    const QRectF chordTextRect = chordHitRect_.adjusted(18, 58, -18, -64);
    const QFontMetricsF chordMetrics(chordFont);
    const double chordX = chordTextRect.center().x() -
        chordMetrics.horizontalAdvance(currentChord) / 2.0;
    const double chordY = chordTextRect.center().y() +
        (chordMetrics.ascent() - chordMetrics.descent()) / 2.0;
    chordPath.addText(QPointF(chordX, chordY), chordFont, currentChord);
    QLinearGradient chordGradient(chordTextRect.left(), 0, chordTextRect.right(), 0);
    chordGradient.setColorAt(0.0, QColor(112, 234, 215));
    chordGradient.setColorAt(0.48, QColor(176, 139, 228));
    chordGradient.setColorAt(0.76, QColor(255, 125, 134));
    chordGradient.setColorAt(1.0, QColor(201, 47, 88));
    painter.setPen(Qt::NoPen);
    painter.setBrush(chordGradient);
    painter.drawPath(chordPath);

    const quint64 totalBars = position.totalBeats > 0
        ? (position.totalBeats + static_cast<quint64>(beatsPerBar_) - 1) /
            static_cast<quint64>(beatsPerBar_)
        : 1;
    const quint64 currentBar = position.totalBeats > 0
        ? position.songBeat / static_cast<quint64>(beatsPerBar_) + 1
        : 1;
    QFont positionFont(QStringLiteral("Bahnschrift"));
    positionFont.setPointSizeF(9.0);
    positionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    painter.setFont(positionFont);
    painter.setPen(QColor(174, 184, 183));
    painter.drawText(
        chordHitRect_.adjusted(0, 0, 0, -39),
        Qt::AlignHCenter | Qt::AlignBottom,
        QStringLiteral("BAR %1 / %2").arg(currentBar).arg(totalBars));

    const int activeBeat = position.totalBeats > 0
        ? position.sectionBeat % qMax(1, beatsPerBar_)
        : 0;
    const int dotGap = 13;
    const int dotStart =
        chordHitRect_.center().x() - ((beatsPerBar_ - 1) * dotGap) / 2;
    for (int beat = 0; beat < beatsPerBar_; ++beat) {
        const QPoint center(dotStart + beat * dotGap, chordHitRect_.bottom() - 25);
        painter.setPen(QPen(
            beat == activeBeat ? QColor(255, 214, 142) : QColor(92, 105, 108),
            1));
        painter.setBrush(
            beat == activeBeat ? QColor(255, 214, 142) : QColor(17, 23, 25));
        painter.drawEllipse(center, 3, 3);
    }

    const QVector<QPair<QString, QString>> upcoming = upcomingChords(position);
    const int runwayWidth = qMin(500, qMax(390, width() / 2));
    constexpr int runwayHeight = 60;
    const int runwayTop = qMin(
        previewTop - runwayHeight - 8,
        chordHitRect_.bottom() + 17);
    chordRunwayRect_ = QRect(
        static_cast<int>(orbitCenter.x() - runwayWidth / 2.0),
        runwayTop,
        runwayWidth,
        runwayHeight);
    QVector<QPair<QString, QString>> chordCells;
    chordCells.push_back({
        currentChord,
        QStringLiteral("%1.%2")
            .arg(position.songBeat / static_cast<quint64>(qMax(1, beatsPerBar_)) + 1)
            .arg(position.songBeat % static_cast<quint64>(qMax(1, beatsPerBar_)) + 1)});
    for (int index = 0; index < 3; ++index) {
        chordCells.push_back(
            index < upcoming.size()
                ? upcoming.at(index)
                : QPair<QString, QString>{QStringLiteral("—"), QString{}});
    }
    const int chordCellWidth = chordRunwayRect_.width() / 4;
    for (int index = 0; index < 4; ++index) {
        const QRect cell(
            chordRunwayRect_.left() + index * chordCellWidth,
            chordRunwayRect_.top(),
            chordCellWidth - 6,
            chordRunwayRect_.height());
        painter.setPen(QPen(
            index == 0 ? QColor(140, 102, 127) : QColor(76, 91, 95, 160),
            1));
        painter.setBrush(
            index == 0 ? QColor(22, 18, 23, 224) : QColor(8, 12, 13, 214));
        painter.drawRoundedRect(cell, 3, 3);
        painter.setFont(micro);
        painter.setPen(QColor(156, 169, 171));
        painter.drawText(
            cell.adjusted(10, 5, -8, 0),
            Qt::AlignLeft | Qt::AlignTop,
            index == 0 ? QStringLiteral("NOW")
                : index == 1 ? QStringLiteral("NEXT") : QStringLiteral("THEN"));
        painter.setPen(QColor(108, 120, 122));
        painter.drawText(
            cell.adjusted(0, 5, -8, 0),
            Qt::AlignRight | Qt::AlignTop,
            chordCells.at(index).second);
        QFont runwayFont(QStringLiteral("Georgia"));
        double runwaySize = index == 0 ? 20.0 : 18.0;
        while (runwaySize > 11.0) {
            runwayFont.setPointSizeF(runwaySize);
            if (QFontMetricsF(runwayFont).horizontalAdvance(chordCells.at(index).first) <
                cell.width() - 18) {
                break;
            }
            runwaySize -= 1.0;
        }
        painter.setFont(runwayFont);
        painter.setPen(index == 0 ? QColor(184, 255, 248) : QColor(233, 230, 220));
        painter.drawText(
            cell.adjusted(9, 19, -8, -4),
            Qt::AlignLeft | Qt::AlignVCenter,
            chordCells.at(index).first);
    }

    const int visiblePeople = qMin(10, peers_.size()) + 1;
    const int railHeight = visiblePeople * 29;
    peerRailRect_ = QRect(
        margin,
        previewTop + previewHeight - railHeight,
        peerWidth,
        railHeight);
    paintVerticalPeerRail(painter, peerRailRect_);

    currentBeatHitRect_ = QRect(previewLeft, previewTop, previewWidth, previewHeight);
    nextBeatHitRect_ = QRect(
        previewLeft + previewWidth + previewGap,
        previewTop,
        previewWidth,
        previewHeight);
    looperHitRect_ = QRect(
        width() - margin - looperWidth,
        height() - margin - qMin(118, previewHeight),
        looperWidth,
        qMin(118, previewHeight));
    if (tuner_.enabled) {
        tunerHitRect_ = QRect(
            looperHitRect_.left(),
            looperHitRect_.top() - 10 - looperHitRect_.height(),
            looperHitRect_.width(),
            looperHitRect_.height());
        tunerEnableHitRect_ = {};
    } else {
        tunerHitRect_ = {};
        const int enableWidth = qMin(150, looperHitRect_.width());
        tunerEnableHitRect_ = QRect(
            looperHitRect_.right() - enableWidth + 1,
            looperHitRect_.top() - 34,
            enableWidth,
            24);
    }
    const quint64 currentBarStart =
        position.totalBeats > 0
        ? position.songBeat -
            position.songBeat % static_cast<quint64>(beatsPerBar_)
        : 0;
    paintBeatPreview(painter, currentBeatHitRect_, currentBarStart, true);
    paintBeatPreview(
        painter,
        nextBeatHitRect_,
        position.totalBeats > 0
            ? (currentBarStart + static_cast<quint64>(beatsPerBar_)) %
                position.totalBeats
            : 0,
        false);
    paintLooperLaunch(painter, looperHitRect_);
    if (tuner_.enabled) {
        paintTuner(painter, tunerHitRect_, false);
    } else {
        tunerOffHitRect_ = {};
        painter.setPen(QPen(QColor(100, 121, 125, 205), 1));
        painter.setBrush(QColor(7, 11, 12, 225));
        painter.drawRoundedRect(tunerEnableHitRect_, 12, 12);
        QFont enableFont(QStringLiteral("Bahnschrift"));
        enableFont.setPointSizeF(7.5);
        enableFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
        painter.setFont(enableFont);
        painter.setPen(QColor(184, 255, 248));
        painter.drawText(
            tunerEnableHitRect_,
            Qt::AlignCenter,
            QStringLiteral("TUNER \u00b7 ENABLE"));
    }

    painter.setPen(QPen(QColor(77, 91, 95), 1));
    painter.setBrush(QColor(23, 32, 35));
    const QVector<QPoint> screws{
        QPoint(11, 11), QPoint(width() - 11, 11),
        QPoint(11, height() - 11), QPoint(width() - 11, height() - 11)};
    for (const QPoint& screw : screws) {
        painter.drawEllipse(screw, 4, 4);
        painter.drawLine(screw + QPoint(-2, 1), screw + QPoint(2, -1));
    }

    if (tunerExpanded_ && tuner_.enabled) {
        painter.fillRect(rect(), QColor(1, 3, 5, 150));
        QSize overlaySize = currentBeatHitRect_.size();
        overlaySize.setWidth(qBound(360, overlaySize.width(), width() - 96));
        overlaySize.setHeight(qBound(210, overlaySize.height(), height() - 96));
        tunerOverlayRect_ = QRect(QPoint(0, 0), overlaySize);
        tunerOverlayRect_.moveCenter(rect().center());
        paintTuner(painter, tunerOverlayRect_, true);
        tunerOverlayCloseHitRect_ = QRect(
            tunerOverlayRect_.right() - 34,
            tunerOverlayRect_.top() + 10,
            24,
            24);
        painter.setPen(QPen(QColor(164, 181, 184), 1.5));
        painter.drawLine(
            tunerOverlayCloseHitRect_.topLeft() + QPoint(7, 7),
            tunerOverlayCloseHitRect_.bottomRight() - QPoint(6, 6));
        painter.drawLine(
            tunerOverlayCloseHitRect_.topRight() + QPoint(-7, 7),
            tunerOverlayCloseHitRect_.bottomLeft() + QPoint(6, -6));
    } else {
        tunerOverlayRect_ = {};
        tunerOverlayCloseHitRect_ = {};
        tunerOverlayOffHitRect_ = {};
    }

    const qint64 elapsed = paintClock.nsecsElapsed();
    ++renderedFrames_;
    renderTotalNanoseconds_ += elapsed;
    renderMaximumNanoseconds_ = qMax(renderMaximumNanoseconds_, elapsed);
    const qint64 windowMs = renderWindow_.elapsed();
    if (windowMs >= 1000) {
        const double fps = static_cast<double>(renderedFrames_) * 1000.0 /
            static_cast<double>(qMax<qint64>(1, windowMs));
        const double averageMs = static_cast<double>(renderTotalNanoseconds_) /
            static_cast<double>(qMax(1, renderedFrames_)) / 1000000.0;
        const double maximumMs =
            static_cast<double>(renderMaximumNanoseconds_) / 1000000.0;
        rendererStats_ = QStringLiteral(
            "Visualizer: %1 fps target 30 | paint avg %2 ms | max %3 ms | cached 65% nebula")
            .arg(fps, 0, 'f', 1)
            .arg(averageMs, 0, 'f', 2)
            .arg(maximumMs, 0, 'f', 2);
        renderWindow_.restart();
        renderedFrames_ = 0;
        renderTotalNanoseconds_ = 0;
        renderMaximumNanoseconds_ = 0;
    }
}

void PerformanceHomeWidget::paintEvent(QPaintEvent*)
{
    if (width() >= 900) {
        paintHtmlStage();
        return;
    }
    QElapsedTimer paintClock;
    paintClock.start();
    if (nebulaCache_.isNull()) {
        rebuildBackground();
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(4, 5, 13));
    painter.setPen(Qt::NoPen);
    for (int index = 0; index < stars_.size(); ++index) {
        const QPointF point(stars_[index].x() * width(), stars_[index].y() * height());
        const int alpha = 55 + (index % 5) * 22;
        painter.setBrush(QColor(220, 226, 242, alpha));
        const double radius = index % 13 == 0 ? 1.5 : 0.8;
        painter.drawEllipse(point, radius, radius);
    }

    const double seconds = static_cast<double>(animationClock_.elapsed()) / 1000.0;
    const double participantComplexity =
        qBound(0.0, static_cast<double>(peers_.size()) / 8.0, 1.0);
    paintNebulaFields(painter, seconds, participantComplexity);

    if (novaStartMs_ >= 0) {
        const double age =
            static_cast<double>(animationClock_.elapsed() - novaStartMs_) / 1800.0;
        const double alpha = std::sin(qBound(0.0, age, 1.0) * 3.14159265358979323846);
        const QPointF center(novaPosition_.x() * width(), novaPosition_.y() * height());
        QRadialGradient nova(center, 42.0 + 24.0 * age);
        nova.setColorAt(0.0, QColor(255, 245, 221, static_cast<int>(150 * alpha)));
        nova.setColorAt(0.2, QColor(230, 117, 107, static_cast<int>(80 * alpha)));
        nova.setColorAt(1.0, QColor(230, 117, 107, 0));
        painter.setBrush(nova);
        painter.drawEllipse(center, 70, 70);
    }

    const int margin = 24;
    const SongPosition position = songPosition(absoluteBeat_);
    const SongSection* section =
        model_ != nullptr && position.section < model_->sections().size()
        ? &model_->section(position.section)
        : nullptr;

    painter.setPen(QColor(205, 198, 213));
    QFont microFont = font();
    microFont.setPointSizeF(qMax(9.5, font().pointSizeF() - 0.5));
    painter.setFont(microFont);
    painter.drawText(
        QRect(margin, 18, width() / 3, 24),
        Qt::AlignLeft | Qt::AlignVCenter,
        section != nullptr ? sectionName(*section) : QStringLiteral("No song section"));

    const QPair<QString, QString> lyrics = lyricLines(position);
    lyricsHitRect_ = QRect(margin, 48, width() / 3, lyrics.first.isEmpty() && lyrics.second.isEmpty() ? 0 : 66);
    if (lyricsHitRect_.height() > 0) {
        painter.setPen(theme::textStrong);
        QFont lyricFont = font();
        lyricFont.setPointSizeF(font().pointSizeF() + 2.0);
        lyricFont.setWeight(QFont::DemiBold);
        painter.setFont(lyricFont);
        painter.drawText(
            lyricsHitRect_.adjusted(0, 0, 0, -28),
            Qt::AlignLeft | Qt::AlignVCenter,
            lyrics.first);
        painter.setPen(QColor(196, 188, 205));
        lyricFont.setPointSizeF(font().pointSizeF() + 0.5);
        lyricFont.setWeight(QFont::Normal);
        painter.setFont(lyricFont);
        painter.drawText(
            lyricsHitRect_.adjusted(0, 30, 0, 0),
            Qt::AlignLeft | Qt::AlignVCenter,
            lyrics.second);
    }

    const int technicalWidth = 104;
    const QStringList technical{rtt_, jitter_, loss_, xruns_};
    for (int index = 0; index < technical.size(); ++index) {
        const QRect cell(
            width() - margin - technicalWidth * (technical.size() - index),
            18,
            technicalWidth - 8,
            30);
        painter.setPen(QPen(QColor(120, 105, 137, 135), 1));
        painter.setBrush(QColor(12, 12, 25, 150));
        painter.drawRoundedRect(cell, 6, 6);
        painter.setPen(QColor(221, 214, 226));
        painter.setFont(microFont);
        painter.drawText(cell, Qt::AlignCenter, technical.at(index));
    }
    paintGenerationActions(painter, 57, width() - margin);

    const int previewGap = 14;
    const int previewHeight = qBound(150, height() / 3, 320);
    const int previewTop = height() - margin - previewHeight;
    const int previewWidth = (width() - margin * 2 - previewGap) / 2;
    const QPointF orbitCenter(
        width() * 0.50,
        (170.0 + static_cast<double>(previewTop)) / 2.0);
    const double orbitRadius = qMin(
        width() * 0.20,
        qMax(70.0, (static_cast<double>(previewTop) - 178.0) * 0.47));
    painter.setPen(QPen(QColor(203, 176, 220, 55), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(orbitCenter, orbitRadius * 1.18, orbitRadius * 1.18);
    painter.drawEllipse(orbitCenter, orbitRadius * 0.92, orbitRadius * 0.92);
    painter.drawEllipse(orbitCenter, orbitRadius * 0.68, orbitRadius * 0.68);
    QPainterPath activity;
    for (int index = 0; index < history_.size(); ++index) {
        const double angle =
            static_cast<double>(index) / history_.size() * 6.283185307179586;
        const double energy = history_.at(index);
        const double flow =
            std::sin(
                angle * (2.0 + qMin(6, peers_.size())) +
                seconds * (0.7 + participantComplexity)) *
            participantComplexity * (0.015 + energy * 0.055);
        const double radius =
            orbitRadius * (0.92 + energy * 0.30 + flow);
        const QPointF point(
            orbitCenter.x() + std::cos(angle) * radius,
            orbitCenter.y() + std::sin(angle) * radius);
        if (index == 0) {
            activity.moveTo(point);
        } else {
            activity.lineTo(point);
        }
    }
    activity.closeSubpath();
    painter.setPen(QPen(QColor(91, 214, 208, 180), 2));
    painter.drawPath(activity);

    chordHitRect_ = QRect(
        static_cast<int>(orbitCenter.x() - orbitRadius * 0.62),
        static_cast<int>(orbitCenter.y() - orbitRadius * 0.62),
        static_cast<int>(orbitRadius * 1.24),
        static_cast<int>(orbitRadius * 1.24));
    QRadialGradient lens(chordHitRect_.center(), chordHitRect_.width() / 2.0);
    lens.setColorAt(0.0, QColor(36, 21, 47, 230));
    lens.setColorAt(0.72, QColor(18, 14, 34, 230));
    lens.setColorAt(1.0, QColor(8, 9, 20, 210));
    painter.setBrush(lens);
    painter.setPen(QPen(gold(), 2));
    painter.drawEllipse(chordHitRect_);
    painter.setPen(QColor(213, 204, 218));
    painter.setFont(microFont);
    painter.drawText(
        chordHitRect_.adjusted(0, 14, 0, 0),
        Qt::AlignHCenter | Qt::AlignTop,
        QStringLiteral("CURRENT CHORD"));
    QFont chordFont = font();
    chordFont.setPointSizeF(qMax(28.0, orbitRadius * 0.28));
    chordFont.setWeight(QFont::Bold);
    painter.setFont(chordFont);
    painter.setPen(QColor(247, 237, 224));
    painter.drawText(chordHitRect_, Qt::AlignCenter, chordAt(position));
    painter.setFont(microFont);
    painter.setPen(gold());
    painter.drawText(
        chordHitRect_.adjusted(0, 0, 0, -14),
        Qt::AlignHCenter | Qt::AlignBottom,
        position.totalBeats > 0
            ? QStringLiteral("%1.%2")
                .arg(position.songBeat / static_cast<quint64>(beatsPerBar_) + 1)
                .arg(position.songBeat % static_cast<quint64>(beatsPerBar_) + 1)
            : QStringLiteral("1.1"));

    const QVector<QPair<QString, QString>> upcoming = upcomingChords(position);
    const int runwayWidth = qMin(430, width() / 3);
    const QRect runway(
        static_cast<int>(orbitCenter.x() + orbitRadius * 0.86),
        static_cast<int>(orbitCenter.y() - 46),
        runwayWidth,
        92);
    const int chordCellWidth = runway.width() / 3;
    for (int index = 0; index < 3; ++index) {
        const QRect cell(
            runway.left() + index * chordCellWidth,
            runway.top(),
            chordCellWidth - 8,
            runway.height());
        painter.setPen(QPen(QColor(129, 99, 151, 150), 1));
        painter.setBrush(QColor(13, 12, 28, 190));
        painter.drawRoundedRect(cell, 8, 8);
        painter.setPen(QColor(190, 181, 199));
        painter.setFont(microFont);
        painter.drawText(
            cell.adjusted(0, 8, 0, 0),
            Qt::AlignHCenter | Qt::AlignTop,
            index == 0 ? QStringLiteral("NEXT") : QStringLiteral("THEN"));
        QFont nextFont = font();
        nextFont.setPointSizeF(font().pointSizeF() + 4.0);
        nextFont.setWeight(QFont::DemiBold);
        painter.setFont(nextFont);
        painter.setPen(QColor(235, 226, 234));
        painter.drawText(
            cell.adjusted(0, 18, 0, -14),
            Qt::AlignCenter,
            index < upcoming.size() ? upcoming.at(index).first : QStringLiteral("—"));
        painter.setFont(microFont);
        painter.setPen(QColor(187, 149, 200));
        painter.drawText(
            cell.adjusted(0, 0, 0, -7),
            Qt::AlignHCenter | Qt::AlignBottom,
            index < upcoming.size() ? upcoming.at(index).second : QString{});
    }

    peerRailRect_ = QRect(margin, 118, qMin(width() - margin * 2, 1080), 48);
    paintPeerRail(painter, peerRailRect_);

    currentBeatHitRect_ = QRect(margin, previewTop, previewWidth, previewHeight);
    nextBeatHitRect_ = QRect(
        margin + previewWidth + previewGap,
        previewTop,
        previewWidth,
        previewHeight);
    const quint64 currentBarStart =
        position.totalBeats > 0
        ? position.songBeat -
            position.songBeat % static_cast<quint64>(beatsPerBar_)
        : 0;
    paintBeatPreview(painter, currentBeatHitRect_, currentBarStart, true);
    paintBeatPreview(
        painter,
        nextBeatHitRect_,
        position.totalBeats > 0
            ? (currentBarStart + static_cast<quint64>(beatsPerBar_)) %
                position.totalBeats
            : 0,
        false);

    const qint64 elapsed = paintClock.nsecsElapsed();
    ++renderedFrames_;
    renderTotalNanoseconds_ += elapsed;
    renderMaximumNanoseconds_ = qMax(renderMaximumNanoseconds_, elapsed);
    const qint64 windowMs = renderWindow_.elapsed();
    if (windowMs >= 1000) {
        const double fps = static_cast<double>(renderedFrames_) * 1000.0 /
            static_cast<double>(qMax<qint64>(1, windowMs));
        const double averageMs = static_cast<double>(renderTotalNanoseconds_) /
            static_cast<double>(qMax(1, renderedFrames_)) / 1000000.0;
        const double maximumMs =
            static_cast<double>(renderMaximumNanoseconds_) / 1000000.0;
        rendererStats_ = QStringLiteral(
            "Visualizer: %1 fps target 30 | paint avg %2 ms | max %3 ms | cached 65% nebula")
            .arg(fps, 0, 'f', 1)
            .arg(averageMs, 0, 'f', 2)
            .arg(maximumMs, 0, 'f', 2);
        renderWindow_.restart();
        renderedFrames_ = 0;
        renderTotalNanoseconds_ = 0;
        renderMaximumNanoseconds_ = 0;
    }
}

void PerformanceHomeWidget::paintBeatPreview(
    QPainter& painter,
    const QRect& bounds,
    quint64 barStart,
    bool current)
{
    painter.setPen(QPen(current ? gold() : QColor(135, 91, 164), 1));
    painter.setBrush(QColor(9, 10, 23, 215));
    painter.drawRoundedRect(bounds, 9, 9);
    QFont heading = font();
    heading.setWeight(QFont::DemiBold);
    painter.setFont(heading);
    painter.setPen(current ? QColor(236, 205, 147) : QColor(208, 181, 220));
    painter.drawText(
        bounds.adjusted(12, 7, -12, 0),
        Qt::AlignLeft | Qt::AlignTop,
        current ? QStringLiteral("CURRENT BAR") : QStringLiteral("NEXT BAR"));
    const int headerHeight = 35;
    const int laneCount = 6;
    const QStringList laneNames{
        QStringLiteral("Tom"),
        QStringLiteral("Crash"),
        QStringLiteral("Open HH"),
        QStringLiteral("Closed HH"),
        QStringLiteral("Snare"),
        QStringLiteral("Kick"),
    };
    QFont laneFont = font();
    laneFont.setPointSizeF(qMax(8.0, font().pointSizeF() - 1.0));
    const QFontMetricsF laneMetrics(laneFont);
    int laneLabelWidth = 0;
    for (const QString& laneName : laneNames) {
        laneLabelWidth = qMax(
            laneLabelWidth,
            static_cast<int>(std::ceil(laneMetrics.horizontalAdvance(laneName))));
    }
    laneLabelWidth = qMax(52, laneLabelWidth + 3);
    const QRect grid = bounds.adjusted(laneLabelWidth + 18, headerHeight, -10, -10);
    const int laneHeight = qMax(12, grid.height() / laneCount);
    const int uniformIconSize = qBound(
        5,
        qMin(
            laneHeight - 4,
            grid.width() / qMax(1, beatsPerBar_ * 8) - 3),
        24);
    if (current) {
        QFont legendFont(QStringLiteral("Bahnschrift"));
        legendFont.setPointSizeF(9.5);
        painter.setFont(legendFont);
        const QRect legendRect(
            bounds.left() + 12,
            bounds.top() - 24,
            226,
            27);
        painter.setPen(QPen(gold(), 1));
        painter.setBrush(QColor(9, 10, 23, 238));
        painter.drawRoundedRect(legendRect, 4, 4);
        const QStringList legendLabels{
            QStringLiteral("Hit"),
            QStringLiteral("Accent"),
            QStringLiteral("Ghost")};
        const QVector<QColor> legendColors{
            QColor(232, 92, 101),
            QColor(86, 164, 244),
            QColor(164, 111, 218)};
        const QVector<int> legendOffsets{10, 66, 150};
        for (int item = 0; item < legendLabels.size(); ++item) {
            const int legendX = legendRect.left() + legendOffsets.at(item);
            const QRect symbol(legendX, legendRect.center().y() - 4, 9, 9);
            painter.setPen(Qt::NoPen);
            painter.setBrush(legendColors.at(item));
            if (item == 2) {
                painter.drawEllipse(symbol);
            } else {
                painter.drawRect(symbol);
            }
            painter.setPen(QColor(196, 192, 195));
            painter.drawText(
                QRect(
                    symbol.right() + 4,
                    legendRect.top() + 1,
                    item == 1 ? 57 : 47,
                    legendRect.height() - 2),
                Qt::AlignLeft | Qt::AlignVCenter,
                legendLabels.at(item));
        }
    }
    const QVector<int> laneIndices{5, 4, 3, 2, 1, 0};
    painter.setFont(laneFont);
    for (int lane = 0; lane < laneCount; ++lane) {
        painter.setPen(QColor(190, 181, 198));
        painter.drawText(
            QRect(
                bounds.left() + 8,
                grid.top() + lane * laneHeight,
                laneLabelWidth,
                laneHeight),
            Qt::AlignRight | Qt::AlignVCenter,
            laneNames.at(lane));
    }
    for (int beat = 0; beat < beatsPerBar_; ++beat) {
        const int left = grid.left() + beat * grid.width() / beatsPerBar_;
        const int right = grid.left() + (beat + 1) * grid.width() / beatsPerBar_;
        const SongPosition beatPosition = songPositionFromSongBeat(
            barStart + static_cast<quint64>(beat));
        const BeatPattern* pattern = nullptr;
        if (model_ != nullptr && beatPosition.totalBeats > 0) {
            pattern = &model_->section(beatPosition.section)
                .beatPatterns[beatPosition.sectionBeat];
        }
        painter.setPen(QColor(151, 136, 165));
        painter.drawText(
            QRect(left, bounds.top() + 7, right - left, 22),
            Qt::AlignCenter,
            QStringLiteral("%1.%2")
                .arg((barStart + static_cast<quint64>(beat)) /
                        static_cast<quint64>(beatsPerBar_) +
                    1)
                .arg(beat + 1));
        painter.setPen(QColor(91, 76, 108, 150));
        painter.drawLine(left, grid.top(), left, grid.bottom());
        if (pattern == nullptr) {
            continue;
        }
        const int division = qMax(1, pattern->division);
        for (int lane = 0; lane < laneCount; ++lane) {
            const QString hits = normalizedHits(
                pattern->lanes.value(laneIndices.at(lane)),
                division);
            const int cellWidth = qMax(1, right - left);
            for (int step = 0; step < division; ++step) {
                const int x =
                    left + (step * cellWidth + cellWidth / 2) / division;
                const int y = grid.top() + lane * laneHeight + laneHeight / 2;
                const QChar state = hits.at(step);
                const bool hit = state != QLatin1Char('.');
                const QColor color =
                    state == QLatin1Char('a') ? QColor(86, 164, 244)
                    : state == QLatin1Char('g') ? QColor(164, 111, 218)
                    : QColor(232, 92, 101);
                painter.setPen(QPen(hit ? color : QColor(80, 70, 93), 1));
                painter.setBrush(hit ? color : QColor(18, 17, 31));
                const QRect icon(
                    x - uniformIconSize / 2,
                    y - uniformIconSize / 2,
                    uniformIconSize,
                    uniformIconSize);
                if (state == QLatin1Char('g')) {
                    painter.drawEllipse(icon);
                } else {
                    painter.drawRoundedRect(icon, 2, 2);
                }
            }
        }
    }
    if (current && running_) {
        const double beatInBar =
            static_cast<double>(absoluteBeat_ % static_cast<quint64>(beatsPerBar_)) +
            beatPhase_;
        const int x = grid.left() +
            static_cast<int>(beatInBar / beatsPerBar_ * grid.width());
        painter.setPen(Qt::NoPen);
        painter.setBrush(gold());
        painter.drawEllipse(QPoint(x, bounds.top() + 14), 5, 5);
    }
}

void PerformanceHomeWidget::paintGenerationActions(
    QPainter& painter,
    int top,
    int right)
{
    constexpr int height = 31;
    constexpr int gap = 8;
    constexpr int wavWidth = 116;
    constexpr int ideaWidth = 164;
    generateWavHitRect_ = QRect(right - wavWidth, top, wavWidth, height);
    generateIdeaHitRect_ = QRect(
        generateWavHitRect_.left() - gap - ideaWidth,
        top,
        ideaWidth,
        height);

    QFont actionFont(QStringLiteral("Bahnschrift"));
    actionFont.setPointSizeF(9.0);
    actionFont.setWeight(QFont::DemiBold);
    actionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.45);
    painter.setFont(actionFont);

    const auto drawAction = [&painter](
                                const QRect& bounds,
                                const QString& text,
                                const QColor& outline,
                                const QColor& fill) {
        painter.setPen(QPen(outline, 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(bounds, 4, 4);
        painter.setPen(QColor(243, 230, 207));
        painter.drawText(
            bounds.adjusted(9, 1, -9, -1),
            Qt::AlignCenter,
            text);
    };
    drawAction(
        generateIdeaHitRect_,
        QStringLiteral("GENERATE NEW IDEA"),
        gold(),
        QColor(35, 27, 20, 232));
    drawAction(
        generateWavHitRect_,
        QStringLiteral("GENERATE WAV"),
        QColor(176, 139, 228),
        QColor(27, 20, 37, 232));
}

void PerformanceHomeWidget::paintVerticalPeerRail(
    QPainter& painter,
    const QRect& bounds)
{
    peerHitRects_.clear();
    QFont micro(QStringLiteral("Bahnschrift"));
    micro.setPointSizeF(7.5);
    micro.setLetterSpacing(QFont::AbsoluteSpacing, 0.65);
    QFont nameFont(QStringLiteral("Bahnschrift"));
    nameFont.setPointSizeF(9.0);
    const int rowHeight = 27;
    const int remoteCapacity = qMax(0, qMin(10, bounds.height() / rowHeight - 1));

    const auto drawPerson = [&](const QRect& row,
                                const QString& name,
                                const QString& detail,
                                const QColor& color,
                                double activity,
                                bool selected) {
        painter.setPen(QPen(
            selected ? QColor(255, 214, 142) : QColor(78, 95, 99, 175),
            selected ? 1.5 : 1.0));
        painter.setBrush(selected ? QColor(29, 26, 23, 232) : QColor(7, 11, 12, 216));
        painter.drawRoundedRect(row, 3, 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPoint(row.left() + 10, row.center().y()), 3, 3);
        painter.setFont(nameFont);
        painter.setPen(QColor(232, 231, 223));
        painter.drawText(
            detail.isEmpty()
                ? row.adjusted(18, 0, -29, 0)
                : row.adjusted(18, 2, -29, -10),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(nameFont).elidedText(
                name, Qt::ElideRight, qMax(12, row.width() - 49)));
        if (!detail.isEmpty()) {
            painter.setFont(micro);
            painter.setPen(QColor(155, 168, 170));
            painter.drawText(
                row.adjusted(18, 12, -27, -1),
                Qt::AlignLeft | Qt::AlignVCenter,
                detail);
        }
        const QRect activityTrack(row.right() - 21, row.top() + 5, 3, row.height() - 10);
        painter.fillRect(activityTrack, QColor(45, 57, 60));
        const int fillHeight =
            qRound(activityTrack.height() * qBound(0.03, activity, 1.0));
        painter.fillRect(
            QRect(
                activityTrack.left(),
                activityTrack.bottom() - fillHeight + 1,
                activityTrack.width(),
                fillHeight),
            color);
    };

    const QRect localRow(
        bounds.left(),
        bounds.bottom() - rowHeight + 1,
        bounds.width(),
        rowHeight - 3);
    drawPerson(
        localRow,
        QStringLiteral("You"),
        QString{},
        gold(),
        qMax(0.08, envelope_),
        false);
    peerHitRects_.push_back({localRow, 0});

    const QVector<QColor> peerColors{
        QColor(102, 212, 207),
        QColor(160, 146, 218),
        QColor(223, 107, 97),
        QColor(112, 212, 157)};
    const int shown = qMax(
        0,
        qMin(remoteCapacity, peers_.size() - peerScrollOffset_));
    for (int slot = 0; slot < shown; ++slot) {
        const int peerIndex = peerScrollOffset_ + slot;
        const PerformancePeerPresentation& peer = peers_.at(peerIndex);
        const QRect row(
            bounds.left(),
            localRow.top() - (slot + 1) * rowHeight,
            bounds.width(),
            rowHeight - 3);
        peerHitRects_.push_back({row, peer.peerId});
        const double activity = peer.receiving
            ? qBound(0.18, envelope_ * (0.64 + (peerIndex % 4) * 0.11), 1.0)
            : 0.04;
        drawPerson(
            row,
            peer.label,
            peer.receiving ? dbText(peer.gainDb) : QStringLiteral("WAITING"),
            peerColors.at(peerIndex % peerColors.size()),
            activity,
            peer.selected || peer.peerId == selectedPeerId_);
    }

    if (peers_.size() > remoteCapacity) {
        painter.setFont(micro);
        painter.setPen(QColor(255, 214, 142));
        painter.drawText(
            QRect(bounds.left(), bounds.top(), bounds.width(), 15),
            Qt::AlignCenter,
            QStringLiteral("%1 PEERS · SCROLL").arg(peers_.size()));
    }
}

void PerformanceHomeWidget::paintLooperLaunch(QPainter& painter, const QRect& bounds)
{
    painter.setPen(QPen(QColor(78, 94, 98, 195), 1));
    painter.setBrush(QColor(7, 11, 12, 225));
    painter.drawRoundedRect(bounds, 4, 4);
    painter.fillRect(
        QRect(bounds.left(), bounds.top(), 3, bounds.height()),
        QColor(176, 139, 228));

    QFont micro(QStringLiteral("Bahnschrift"));
    micro.setPointSizeF(7.5);
    micro.setLetterSpacing(QFont::AbsoluteSpacing, 0.75);
    painter.setFont(micro);
    painter.setPen(QColor(156, 169, 171));
    painter.drawText(
        bounds.adjusted(12, 7, -8, 0),
        Qt::AlignLeft | Qt::AlignTop,
        QStringLiteral("BACKING TRACKS"));
    painter.setPen(QColor(102, 212, 207));
    painter.drawText(
        bounds.adjusted(0, 7, -10, 0),
        Qt::AlignRight | Qt::AlignTop,
        QStringLiteral("BANK A"));

    const QRect wave = bounds.adjusted(12, 29, -12, -39);
    painter.setPen(QPen(QColor(54, 68, 72), 1));
    painter.drawLine(wave.left(), wave.center().y(), wave.right(), wave.center().y());
    QLinearGradient waveformGradient(wave.left(), 0, wave.right(), 0);
    waveformGradient.setColorAt(0.0, QColor(102, 212, 207));
    waveformGradient.setColorAt(0.42, QColor(176, 139, 228));
    waveformGradient.setColorAt(0.78, QColor(255, 125, 134));
    waveformGradient.setColorAt(1.0, QColor(201, 47, 88));
    if (trackWaveformValid_) {
        painter.setPen(QPen(QBrush(waveformGradient), 1.0));
        for (int x = 0; x <= wave.width(); ++x) {
            const int index = qBound(
                0,
                x * static_cast<int>(trackWaveformPeaks_.size()) /
                    qMax(1, wave.width()),
                static_cast<int>(trackWaveformPeaks_.size()) - 1);
            const int amplitude = qMax(
                1,
                qRound(qBound(0.0f, trackWaveformPeaks_.at(index), 1.0f) *
                    wave.height() * 0.44));
            painter.drawLine(
                wave.left() + x,
                wave.center().y() - amplitude,
                wave.left() + x,
                wave.center().y() + amplitude);
        }
    } else {
        painter.setFont(micro);
        painter.setPen(QColor(132, 143, 146));
        painter.drawText(
            wave,
            Qt::AlignCenter,
            QStringLiteral("NO CACHED BANK WAVEFORM"));
    }

    trackSliderRect_ = QRect(
        bounds.left() + 12,
        bounds.bottom() - 26,
        qMax(20, bounds.width() - 72),
        16);
    const int trackY = trackSliderRect_.center().y();
    const double normalized = (trackGainDb_ + 60.0) / 72.0;
    const int handleX =
        trackSliderRect_.left() + qRound(qBound(0.0, normalized, 1.0) *
            trackSliderRect_.width());
    painter.setPen(QPen(QColor(55, 69, 72), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(trackSliderRect_.left(), trackY, trackSliderRect_.right(), trackY);
    painter.setPen(QPen(gold(), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(trackSliderRect_.left(), trackY, handleX, trackY);
    painter.setPen(QPen(QColor(255, 225, 171), 1));
    painter.setBrush(gold());
    painter.drawEllipse(QPoint(handleX, trackY), 5, 5);
    painter.setFont(micro);
    painter.setPen(QColor(255, 214, 142));
    painter.drawText(
        QRect(bounds.right() - 59, bounds.bottom() - 29, 50, 20),
        Qt::AlignRight | Qt::AlignVCenter,
        dbText(trackGainDb_));
}

QString PerformanceHomeWidget::tunerNoteText() const
{
    if (!tuner_.valid || tuner_.midi_note < 0 || tuner_.midi_note > 127) {
        return QString(QChar(0x2014));
    }
    static const std::array<const char*, 12> names{
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B",
    };
    const int pitchClass = tuner_.midi_note % 12;
    const int octave = tuner_.midi_note / 12 - 1;
    return QStringLiteral("%1%2")
        .arg(QString::fromLatin1(names[static_cast<std::size_t>(pitchClass)]))
        .arg(octave);
}

void PerformanceHomeWidget::paintTuner(
    QPainter& painter,
    const QRect& bounds,
    bool expanded)
{
    painter.save();
    painter.setPen(QPen(QColor(78, 94, 98, 210), 1));
    QLinearGradient panel(bounds.topLeft(), bounds.bottomRight());
    panel.setColorAt(0.0, QColor(6, 10, 13, 245));
    panel.setColorAt(0.55, QColor(11, 12, 22, 242));
    panel.setColorAt(1.0, QColor(17, 8, 25, 242));
    painter.setBrush(panel);
    painter.drawRoundedRect(bounds, expanded ? 10 : 4, expanded ? 10 : 4);
    painter.fillRect(
        QRect(bounds.left(), bounds.top(), expanded ? 4 : 3, bounds.height()),
        QColor(119, 83, 190));

    QRect& offRect = expanded ? tunerOverlayOffHitRect_ : tunerOffHitRect_;
    offRect = expanded
        ? QRect(bounds.left() + 14, bounds.top() + 12, 25, 25)
        : QRect(bounds.right() - 29, bounds.top() + 7, 21, 21);
    const QPointF powerCenter = offRect.center();
    const double powerRadius = expanded ? 7.0 : 5.5;
    painter.setPen(QPen(QColor(145, 164, 168), expanded ? 1.7 : 1.3));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(
        QRectF(
            powerCenter.x() - powerRadius,
            powerCenter.y() - powerRadius,
            powerRadius * 2.0,
            powerRadius * 2.0),
        45 * 16,
        270 * 16);
    painter.drawLine(
        QPointF(powerCenter.x(), powerCenter.y() - powerRadius - 2.0),
        QPointF(powerCenter.x(), powerCenter.y() + 1.0));

    QFont noteFont(QStringLiteral("Bahnschrift"));
    noteFont.setWeight(QFont::DemiBold);
    noteFont.setPointSizeF(expanded ? 52.0 : 27.0);
    noteFont.setLetterSpacing(QFont::AbsoluteSpacing, expanded ? 1.5 : 0.7);
    painter.setFont(noteFont);
    painter.setPen(tuner_.valid ? QColor(232, 237, 234) : QColor(91, 104, 107));
    const QRect noteRect = expanded
        ? bounds.adjusted(44, 24, -44, -bounds.height() / 2)
        : bounds.adjusted(12, 7, -12, -bounds.height() / 2);
    painter.drawText(noteRect, Qt::AlignHCenter | Qt::AlignVCenter, tunerNoteText());

    const int railMargin = expanded ? 42 : 18;
    const int railY = expanded
        ? bounds.top() + qRound(bounds.height() * 0.72)
        : bounds.bottom() - 27;
    const int railLeft = bounds.left() + railMargin;
    const int railRight = bounds.right() - railMargin;
    const int railCenter = (railLeft + railRight) / 2;
    const int railWidth = qMax(1, railRight - railLeft);

    QLinearGradient railGradient(railLeft, railY, railRight, railY);
    railGradient.setColorAt(0.0, QColor(119, 83, 190, tuner_.valid ? 180 : 70));
    railGradient.setColorAt(0.48, QColor(61, 187, 190, tuner_.valid ? 210 : 80));
    railGradient.setColorAt(0.52, QColor(61, 187, 190, tuner_.valid ? 210 : 80));
    railGradient.setColorAt(1.0, QColor(190, 52, 91, tuner_.valid ? 180 : 70));
    painter.setPen(QPen(QBrush(railGradient), expanded ? 3.0 : 2.0,
        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(railLeft, railY, railRight, railY);

    painter.setPen(QPen(QColor(110, 132, 136, tuner_.valid ? 105 : 45), 1));
    const int tickHeight = expanded ? 7 : 4;
    for (int tick = -4; tick <= 4; ++tick) {
        if (tick == 0) {
            continue;
        }
        const int x = railCenter + tick * railWidth / 10;
        painter.drawLine(x, railY - tickHeight, x, railY + tickHeight);
    }

    const bool inTune = tuner_.valid && std::abs(tuner_.cents) <= 5.0;
    if (inTune) {
        QRadialGradient lockGlow(QPointF(railCenter, railY), expanded ? 42.0 : 25.0);
        lockGlow.setColorAt(0.0, QColor(255, 222, 145, 185));
        lockGlow.setColorAt(0.30, QColor(226, 172, 83, 85));
        lockGlow.setColorAt(1.0, QColor(226, 172, 83, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(lockGlow);
        const int radius = expanded ? 42 : 25;
        painter.drawEllipse(QPointF(railCenter, railY), radius, radius);
    }

    painter.setPen(QPen(
        inTune ? QColor(255, 222, 145) : QColor(174, 218, 214),
        expanded ? 2.4 : 1.7));
    painter.setBrush(QColor(5, 9, 12, 235));
    const int targetRadius = expanded ? 9 : 6;
    painter.drawEllipse(QPointF(railCenter, railY), targetRadius, targetRadius);

    if (tunerOrbOpacity_ > 0.02) {
        painter.setOpacity(qBound(0.0, tunerOrbOpacity_, 1.0));
        const double normalized =
            qBound(-50.0, displayedTunerCents_, 50.0) / 100.0 + 0.5;
        const double orbX = static_cast<double>(railLeft) +
            normalized * static_cast<double>(railWidth);
        const QColor orbCore =
            inTune ? QColor(255, 220, 139) : QColor(151, 119, 227);
        painter.setPen(QPen(
            QColor(orbCore.red(), orbCore.green(), orbCore.blue(), 80),
            expanded ? 4.0 : 2.5,
            Qt::SolidLine,
            Qt::RoundCap));
        painter.drawLine(QPointF(railCenter, railY), QPointF(orbX, railY));
        QRadialGradient orb(QPointF(orbX, railY), expanded ? 18.0 : 11.0);
        orb.setColorAt(0.0, QColor(255, 251, 235));
        orb.setColorAt(0.20, orbCore);
        orb.setColorAt(
            0.58,
            QColor(orbCore.red(), orbCore.green(), orbCore.blue(), 110));
        orb.setColorAt(
            1.0,
            QColor(orbCore.red(), orbCore.green(), orbCore.blue(), 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(orb);
        const int orbRadius = expanded ? 18 : 11;
        painter.drawEllipse(QPointF(orbX, railY), orbRadius, orbRadius);
    }
    painter.restore();
}

void PerformanceHomeWidget::paintPeerRail(QPainter& painter, const QRect& bounds)
{
    painter.setPen(QColor(188, 179, 198));
    QFont labelFont = font();
    labelFont.setPointSizeF(qMax(8.5, font().pointSizeF() - 1.0));
    painter.setFont(labelFont);
    const int count = peers_.size();
    const int capacity = peerVisibleCapacity();
    const int shown = qMin(capacity, count);
    for (int slot = 0; slot < shown; ++slot) {
        const int peerIndex = slot + peerScrollOffset_;
        if (peerIndex >= count) {
            break;
        }
        const QRect chip(
            bounds.left() + slot * kPeerChipWidth,
            bounds.top(),
            kPeerChipWidth - 7,
            bounds.height());
        const PerformancePeerPresentation& peer = peers_.at(peerIndex);
        painter.setPen(QPen(
            peer.receiving ? QColor(74, 198, 176) : QColor(104, 91, 116),
            1));
        painter.setBrush(QColor(12, 12, 26, 205));
        painter.drawRoundedRect(chip, 7, 7);
        painter.setPen(peer.receiving ? QColor(226, 237, 232) : QColor(190, 181, 198));
        painter.drawText(
            chip.adjusted(8, 4, -8, -20),
            Qt::AlignLeft | Qt::AlignVCenter,
            peer.label);
        painter.setPen(QColor(217, 170, 99));
        painter.drawText(
            chip.adjusted(8, 22, -8, -3),
            Qt::AlignLeft | Qt::AlignVCenter,
            dbText(peer.gainDb));
    }
    if (count > capacity) {
        painter.setPen(QColor(198, 187, 207));
        painter.drawText(
            QRect(bounds.right() - 62, bounds.top(), 62, bounds.height()),
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1–%2/%3")
                .arg(peerScrollOffset_ + 1)
                .arg(peerScrollOffset_ + shown)
                .arg(count));
    }
}

void PerformanceHomeWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    peerScrollOffset_ = qBound(
        0,
        peerScrollOffset_,
        qMax(0, peers_.size() - peerVisibleCapacity()));
    rebuildBackground();
}

void PerformanceHomeWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint point = event->position().toPoint();
    if (tunerExpanded_) {
        if (tunerOverlayCloseHitRect_.contains(point) ||
            !tunerOverlayRect_.contains(point)) {
            tunerExpanded_ = false;
            update();
            return;
        }
        if (tunerOverlayOffHitRect_.contains(point)) {
            tunerExpanded_ = false;
            if (onTunerEnabledChanged) onTunerEnabledChanged(false);
            return;
        }
        return;
    }
    if (tunerEnableHitRect_.contains(point)) {
        if (onTunerEnabledChanged) onTunerEnabledChanged(true);
        return;
    }
    if (tunerOffHitRect_.contains(point)) {
        if (onTunerEnabledChanged) onTunerEnabledChanged(false);
        return;
    }
    if (tunerHitRect_.contains(point)) {
        tunerExpanded_ = true;
        update();
        return;
    }
    if (generateIdeaHitRect_.contains(point)) {
        if (onGenerateIdea) onGenerateIdea();
        return;
    }
    if (generateWavHitRect_.contains(point)) {
        if (onGenerateWav) onGenerateWav();
        return;
    }
    if (chordHitRect_.contains(point) || chordRunwayRect_.contains(point)) {
        if (onOpenDetail) onOpenDetail(QStringLiteral("chords"));
        return;
    }
    if (lyricsHitRect_.contains(point)) {
        if (onOpenDetail) onOpenDetail(QStringLiteral("lyrics"));
        return;
    }
    if (currentBeatHitRect_.contains(point) || nextBeatHitRect_.contains(point)) {
        if (onOpenDetail) onOpenDetail(QStringLiteral("beats"));
        return;
    }
    if (trackSliderRect_.contains(point)) {
        trackSliderDragging_ = true;
        applyTrackSliderPosition(point.x());
        return;
    }
    if (looperHitRect_.contains(point)) {
        if (onOpenDetail) onOpenDetail(QStringLiteral("looper"));
        return;
    }
    for (const auto& peerHit : std::as_const(peerHitRects_)) {
        if (peerHit.first.contains(point)) {
            if (onPeerSelected) {
                onPeerSelected(peerHit.second);
            }
            return;
        }
    }
}

void PerformanceHomeWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (trackSliderDragging_) {
        applyTrackSliderPosition(event->position().toPoint().x());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PerformanceHomeWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && trackSliderDragging_) {
        trackSliderDragging_ = false;
        applyTrackSliderPosition(event->position().toPoint().x());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PerformanceHomeWidget::applyTrackSliderPosition(int x)
{
    if (trackSliderRect_.width() <= 0) {
        return;
    }
    const double normalized = qBound(
        0.0,
        static_cast<double>(x - trackSliderRect_.left()) /
            static_cast<double>(trackSliderRect_.width()),
        1.0);
    const double value = std::round((-60.0 + normalized * 72.0) * 2.0) / 2.0;
    setTrackGainDb(value);
    if (onTrackGainChanged) {
        onTrackGainChanged(value);
    }
}

void PerformanceHomeWidget::wheelEvent(QWheelEvent* event)
{
    if (trackSliderRect_.contains(event->position().toPoint())) {
        const int delta =
            event->angleDelta().y() != 0
            ? event->angleDelta().y()
            : event->angleDelta().x();
        const double value = qBound(
            -60.0,
            trackGainDb_ + (delta > 0 ? 1.0 : -1.0),
            12.0);
        setTrackGainDb(value);
        if (onTrackGainChanged) onTrackGainChanged(value);
        event->accept();
        return;
    }
    const int capacity = peerVisibleCapacity();
    if (!peerRailRect_.contains(event->position().toPoint()) ||
        peers_.size() <= capacity) {
        QWidget::wheelEvent(event);
        return;
    }
    const int delta =
        event->angleDelta().y() != 0
        ? event->angleDelta().y()
        : event->angleDelta().x();
    const int maximumOffset = peers_.size() - capacity;
    peerScrollOffset_ = qBound(
        0,
        peerScrollOffset_ + (delta < 0 ? 1 : -1),
        maximumOffset);
    event->accept();
    update();
}

void PerformanceHomeWidget::keyPressEvent(QKeyEvent* event)
{
    if (tunerExpanded_ && event->key() == Qt::Key_Escape) {
        tunerExpanded_ = false;
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
