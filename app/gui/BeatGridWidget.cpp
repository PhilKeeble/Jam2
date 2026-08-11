#include "BeatGridWidget.hpp"
#include "MusicTheory.hpp"
#include "SectionTimeline.hpp"

#include <QAction>
#include <QColor>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QPolygonF>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <limits>
#include <utility>

namespace {

QString musicalStepText(const MusicalStep& step)
{
    if (step.state == MusicalStepState::Onset) return step.value;
    if (step.state == MusicalStepState::Hold) return QStringLiteral("~");
    return QStringLiteral("-");
}

QString sectionTitle(const SongSection& section)
{
    return section.name.trimmed().isEmpty() ? QStringLiteral("Section") : section.name.trimmed();
}

QString normalizedHitText(const QString& text, int division)
{
    QString out;
    out.reserve(division);
    const QString trimmed = text.trimmed();
    for (int i = 0; i < division; ++i) {
        const QChar value = i < trimmed.size() ? trimmed[i] : QChar('.');
        const QChar lower = value.toLower();
        out.append(lower == QLatin1Char('a') ? QLatin1Char('a')
            : lower == QLatin1Char('g') ? QLatin1Char('g')
            : lower == QLatin1Char('x') || value == QLatin1Char('1') ? QLatin1Char('x')
            : QLatin1Char('.'));
    }
    return out;
}

QIcon sectionPaginationIcon(bool forward, bool edge)
{
    const auto pixmapFor = [forward, edge](const QColor& color) {
        QPixmap pixmap(28, 28);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(color);
        const QPolygonF triangle = forward
            ? QPolygonF{QPointF(6, 3), QPointF(22, 14), QPointF(6, 25)}
            : QPolygonF{QPointF(22, 3), QPointF(6, 14), QPointF(22, 25)};
        painter.drawPolygon(triangle);
        if (edge) {
            const qreal x = forward ? 25.0 : 3.0;
            painter.drawLine(QPointF(x, 3), QPointF(x, 25));
        }
        return pixmap;
    };
    QIcon icon;
    icon.addPixmap(
        pixmapFor(QColor(QStringLiteral("#f2edf7"))),
        QIcon::Normal,
        QIcon::Off);
    icon.addPixmap(
        pixmapFor(QColor(QStringLiteral("#626d70"))),
        QIcon::Disabled,
        QIcon::Off);
    icon.addPixmap(
        pixmapFor(QColor(QStringLiteral("#ffd68a"))),
        QIcon::Active,
        QIcon::Off);
    return icon;
}

QString withNextHitState(const QString& text, int division, int index)
{
    QString out = normalizedHitText(text, division);
    if (index >= 0 && index < out.size()) {
        const QChar current = out[index].toLower();
        out[index] = current == QLatin1Char('.') ? QChar('x')
            : current == QLatin1Char('x') ? QChar('a')
            : current == QLatin1Char('a') ? QChar('g')
            : QChar('.');
    }
    return out;
}

struct GrooveOverviewHit {
    double phase = 0.0;
    int lane = 0;
    QChar state = QLatin1Char('x');
};

struct ChordOverviewSpan {
    double startPhase = 0.0;
    double endPhase = 1.0;
    QString chord;
    bool onset = false;
};

struct ChordOverviewData {
    QVector<ChordOverviewSpan> spans;
};

ChordOverviewData chordOverviewData(
    const SongSection& section,
    int bar,
    int beatsPerBar)
{
    ChordOverviewData data;
    beatsPerBar = qMax(1, beatsPerBar);
    const int firstBeat = bar * beatsPerBar;
    const int endBeat = qMin(section.beats, firstBeat + beatsPerBar);
    QString activeChord;

    for (int beat = 0; beat < firstBeat; ++beat) {
        const QString chord = section.chords.value(beat).trimmed();
        if (chord == QStringLiteral("-")) activeChord.clear();
        else if (!chord.isEmpty() && chord != QStringLiteral("~")) activeChord = chord;
    }

    double spanStart = 0.0;
    bool spanOnset = false;
    const auto closeSpan = [&data, &activeChord, &spanStart, &spanOnset](double phase) {
        if (!activeChord.isEmpty() && phase > spanStart) {
            data.spans.push_back({spanStart, phase, activeChord, spanOnset});
        }
    };

    for (int beat = firstBeat; beat < endBeat; ++beat) {
        const QString chord = section.chords.value(beat).trimmed();
        if (chord.isEmpty() || chord == QStringLiteral("~")) continue;
        const double phase = qBound(
            0.0,
            static_cast<double>(beat - firstBeat) / beatsPerBar,
            1.0);
        closeSpan(phase);
        if (chord == QStringLiteral("-")) {
            activeChord.clear();
            spanOnset = false;
        } else {
            activeChord = chord;
            spanOnset = true;
        }
        spanStart = phase;
    }
    const double barEndPhase = qBound(
        0.0,
        static_cast<double>(qMax(0, endBeat - firstBeat)) / beatsPerBar,
        1.0);
    closeSpan(barEndPhase);
    return data;
}

class ChordBarOverviewButton final : public QPushButton {
public:
    explicit ChordBarOverviewButton(QWidget* parent = nullptr)
        : QPushButton(parent)
    {
    }

    void setTimeline(int beatsPerBar, ChordOverviewData data)
    {
        beatsPerBar_ = qMax(1, beatsPerBar);
        spans_ = std::move(data.spans);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF graph(
            50.0,
            7.0,
            qMax(12.0, width() - 58.0),
            qMax(20.0, height() - 14.0));

        painter.save();
        painter.setClipRect(graph.adjusted(-1.0, -1.0, 1.0, 1.0));
        painter.setPen(QPen(QColor(89, 111, 116, 105), 1));
        painter.setBrush(QColor(102, 212, 207, 9));
        painter.drawRoundedRect(graph, 3.0, 3.0);

        const double beatSpacing = graph.width() / beatsPerBar_;
        painter.setPen(QPen(QColor(111, 138, 143, 62), 1));
        for (int beat = 1; beat < beatsPerBar_; ++beat) {
            const double x = graph.left() + beatSpacing * beat;
            painter.drawLine(QPointF(x, graph.top() + 2.0), QPointF(x, graph.bottom() - 2.0));
        }

        const QRectF lane = graph.adjusted(2.0, 11.0, -2.0, -3.0);
        painter.setPen(QPen(QColor(108, 127, 132, 90), 1, Qt::DashLine));
        painter.drawLine(
            QPointF(lane.left(), lane.center().y()),
            QPointF(lane.right(), lane.center().y()));

        QFont beatFont(QStringLiteral("Bahnschrift"));
        beatFont.setPixelSize(10);
        painter.setFont(beatFont);
        painter.setPen(QColor(188, 197, 202, 165));
        if (beatSpacing >= 13.0) {
            for (int beat = 0; beat < beatsPerBar_; ++beat) {
                const QRectF numberArea(
                    graph.left() + beatSpacing * beat + 2.0,
                    graph.top() + 1.0,
                    qMax(8.0, beatSpacing - 4.0),
                    10.0);
                painter.drawText(
                    numberArea,
                    Qt::AlignLeft | Qt::AlignVCenter,
                    QString::number(beat + 1));
            }
        }

        QFont chordFont(QStringLiteral("Georgia"));
        chordFont.setPixelSize(12);
        painter.setFont(chordFont);
        const QFontMetrics metrics(chordFont);
        for (const ChordOverviewSpan& span : spans_) {
            const double start = graph.left() +
                graph.width() * qBound(0.0, span.startPhase, 1.0);
            const double end = graph.left() +
                graph.width() * qBound(0.0, span.endPhase, 1.0);
            const double availableWidth = qMax(2.0, end - start - 2.0);
            QRectF block(start + 1.0, lane.top(), availableWidth, lane.height());
            QLinearGradient fill(block.topLeft(), block.topRight());
            fill.setColorAt(0.0, QColor(35, 64, 69, 238));
            fill.setColorAt(1.0, QColor(44, 36, 62, 238));
            painter.setPen(QPen(QColor(104, 167, 170, 165), 1));
            painter.setBrush(fill);
            painter.drawRoundedRect(block, 3.0, 3.0);
            if (span.onset) {
                painter.setPen(QPen(QColor(232, 164, 74, 230), 2, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(
                    QPointF(block.left() + 1.0, block.top() + 2.0),
                    QPointF(block.left() + 1.0, block.bottom() - 2.0));
            }
            const QRect textArea = block.adjusted(5.0, 0.0, -3.0, 0.0).toAlignedRect();
            if (textArea.width() >= 8) {
                painter.setPen(QColor(QStringLiteral("#f2edf7")));
                painter.drawText(
                    textArea,
                    Qt::AlignLeft | Qt::AlignVCenter,
                    metrics.elidedText(span.chord, Qt::ElideRight, textArea.width()));
            }
        }
        painter.restore();
    }

private:
    int beatsPerBar_ = 4;
    QVector<ChordOverviewSpan> spans_;
};

class BeatBarOverviewButton final : public QPushButton {
public:
    explicit BeatBarOverviewButton(QWidget* parent = nullptr)
        : QPushButton(parent)
    {
    }

    void setGroove(int beatsPerBar, QVector<GrooveOverviewHit> hits)
    {
        beatsPerBar_ = qMax(1, beatsPerBar);
        hits_ = std::move(hits);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF graph(
            50.0,
            7.0,
            qMax(12.0, width() - 58.0),
            qMax(20.0, height() - 14.0));
        painter.setPen(QPen(QColor(83, 101, 105, 105), 1));
        painter.drawLine(
            QPointF(graph.left() - 5.0, graph.top() + 1.0),
            QPointF(graph.left() - 5.0, graph.bottom() - 1.0));

        painter.save();
        painter.setClipRect(graph);
        painter.setPen(QPen(QColor(72, 90, 94, 52), 1));
        for (const double level : {0.275, 0.585, 0.775}) {
            const double y = graph.top() + graph.height() * level;
            painter.drawLine(QPointF(graph.left(), y), QPointF(graph.right(), y));
        }
        const double beatSpacing = graph.width() / beatsPerBar_;
        if (beatSpacing >= 3.0) {
            painter.setPen(QPen(QColor(232, 164, 74, 46), 1));
            for (int beat = 1; beat < beatsPerBar_; ++beat) {
                const double x = graph.left() + beatSpacing * beat;
                painter.drawLine(QPointF(x, graph.top()), QPointF(x, graph.bottom()));
            }
        }

        const auto laneLevel = [](int lane) {
            switch (lane) {
            case 0: return 0.90; // Kick
            case 1: return 0.69; // Snare
            case 9: return 0.63; // Cross-stick / rim
            case 8: return 0.53; // Floor tom
            case 7: return 0.43; // Mid tom
            case 6: return 0.34; // High tom
            case 2: return 0.22; // Closed hi-hat
            case 3: return 0.17; // Open hi-hat
            case 4: return 0.12; // Ride
            case 5: return 0.07; // Crash
            default: return 0.50;
            }
        };
        const auto laneColor = [](int lane) {
            if (lane == 0) return QColor(232, 129, 120);
            if (lane == 1 || lane == 9) return QColor(232, 164, 74);
            if (lane >= 6 && lane <= 8) return QColor(169, 135, 212);
            return QColor(102, 212, 207);
        };
        for (const GrooveOverviewHit& hit : hits_) {
            const QPointF center(
                graph.left() + 2.0 + hit.phase * qMax(1.0, graph.width() - 4.0),
                graph.top() + graph.height() * laneLevel(hit.lane));
            QColor color = laneColor(hit.lane);
            if (hit.state == QLatin1Char('a')) {
                QColor halo = color;
                halo.setAlpha(65);
                painter.setPen(Qt::NoPen);
                painter.setBrush(halo);
                painter.drawEllipse(center, 4.2, 4.2);
                color.setAlpha(245);
                painter.setBrush(color);
                painter.drawEllipse(center, 2.5, 2.5);
            } else if (hit.state == QLatin1Char('g')) {
                color.setAlpha(145);
                painter.setPen(QPen(color, 1.2));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(center, 1.8, 1.8);
            } else {
                color.setAlpha(220);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawEllipse(center, 2.1, 2.1);
            }
        }
        painter.restore();
    }

private:
    int beatsPerBar_ = 4;
    QVector<GrooveOverviewHit> hits_;
};

class LyricBarEdit final : public QPlainTextEdit {
public:
    explicit LyricBarEdit(QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
    {
        setTabChangesFocus(true);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        QObject::connect(this, &QPlainTextEdit::textChanged, this, [this] {
            if (!loading_ && onEdited) onEdited(toPlainText());
        });
    }

    void setInitialText(const QString& text)
    {
        loading_ = true;
        setPlainText(text);
        loading_ = false;
    }

    std::function<void(const QString&)> onEdited;
    std::function<void(const QStringList&)> onLinesPasted;
    std::function<void()> onAdvance;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->matches(QKeySequence::Paste)) {
            const QString pasted = QGuiApplication::clipboard()->text();
            if (pasted.contains(QLatin1Char('\n')) || pasted.contains(QLatin1Char('\r'))) {
                QString normalized = pasted;
                normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
                normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
                if (onLinesPasted) onLinesPasted(normalized.split(QLatin1Char('\n')));
                event->accept();
                return;
            }
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            !(event->modifiers() & Qt::ShiftModifier)) {
            if (onAdvance) onAdvance();
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

private:
    bool loading_ = false;
};

int normalizedPitchClass(int value)
{
    return ((value % 12) + 12) % 12;
}

QVector<int> guitarTuning(int strings, bool dropped)
{
    if (strings == 8) {
        return dropped ? QVector<int>{28, 35, 40, 45, 50, 55, 59, 64}
                       : QVector<int>{30, 35, 40, 45, 50, 55, 59, 64};
    }
    if (strings == 7) {
        return dropped ? QVector<int>{33, 40, 45, 50, 55, 59, 64}
                       : QVector<int>{35, 40, 45, 50, 55, 59, 64};
    }
    return dropped ? QVector<int>{38, 45, 50, 55, 59, 64}
                   : QVector<int>{40, 45, 50, 55, 59, 64};
}

int frettingFingerCount(const QVector<int>& frets)
{
    int fingers = 0;
    QSet<int> counted;
    for (int string = 0; string < frets.size(); ++string) {
        const int fret = frets[string];
        if (fret <= 0 || counted.contains(string)) continue;
        ++fingers;
        counted.insert(string);
        for (int next = string + 1; next < frets.size() && frets[next] == fret; ++next)
            counted.insert(next);
    }
    return fingers;
}

QVector<int> guitarPosition(
    const jam2::practice::ParsedChord& chord,
    const QVector<int>& tuning,
    int firstFret)
{
    QSet<int> chordTones;
    for (const int interval : chord.intervals)
        chordTones.insert(normalizedPitchClass(chord.root + interval));
    const int bass = chord.bass >= 0 ? chord.bass : chord.root;
    const int lastFret = firstFret + 4;
    QSet<int> definingTones{chord.root};
    QSet<int> extensionTones;
    if (chord.intervals.size() <= 3) {
        for (const int interval : chord.intervals)
            definingTones.insert(normalizedPitchClass(chord.root + interval));
    }
    for (const int interval : chord.intervals) {
        if (interval == 2 || interval == 3 || interval == 4 || interval == 5 ||
            interval == 9 || interval == 10 || interval == 11)
            definingTones.insert(normalizedPitchClass(chord.root + interval));
        if (interval >= 13)
            extensionTones.insert(normalizedPitchClass(chord.root + interval));
    }
    for (int index = chord.intervals.size() - 1; index >= 0; --index) {
        if (chord.intervals[index] >= 13) {
            definingTones.insert(normalizedPitchClass(chord.root + chord.intervals[index]));
            break;
        }
    }
    if (chord.intervals.size() == 2)
        definingTones.insert(normalizedPitchClass(chord.root + chord.intervals.back()));

    QVector<int> best(tuning.size(), -1);
    int bestScore = std::numeric_limits<int>::min();
    QVector<int> current(tuning.size(), -1);
    const int extraStrings = qMax(0, tuning.size() - 6);
    std::function<void(int, bool, int, int)> search =
        [&](int string, bool hasBass, int sounded, int previousPitch) {
            if (sounded > 5) return;
            if (string == tuning.size()) {
                if (!hasBass || sounded < 3) return;
                int lowestString = -1;
                int lowestPitch = -1;
                int secondPitch = -1;
                int minFret = std::numeric_limits<int>::max();
                int maxFret = -1;
                QSet<int> present;
                for (int index = 0; index < current.size(); ++index) {
                    const int fret = current[index];
                    if (fret < 0) continue;
                    const int pitch = tuning[index] + fret;
                    if (lowestString < 0) {
                        lowestString = index;
                        lowestPitch = pitch;
                    } else if (secondPitch < 0) {
                        secondPitch = pitch;
                    }
                    present.insert(normalizedPitchClass(pitch));
                    if (fret > 0) {
                        minFret = qMin(minFret, fret);
                        maxFret = qMax(maxFret, fret);
                    }
                }
                if (normalizedPitchClass(lowestPitch) != bass || secondPitch < 0) return;
                if (maxFret >= 0 && minFret < std::numeric_limits<int>::max() && maxFret - minFret > 3) return;
                const int fingers = frettingFingerCount(current);
                if (fingers > 4) return;
                const int bassGap = secondPitch - lowestPitch;
                if (extraStrings > 0 && lowestString < extraStrings && bassGap < 9) return;

                int score = sounded * 5 - fingers * 3;
                for (const int tone : definingTones)
                    score += present.contains(tone) ? 32 : -38;
                for (const int tone : extensionTones)
                    score += present.contains(tone) ? 13 : -5;
                score += present.size() * 7 - (sounded - present.size()) * 8;
                if (extraStrings > 0) {
                    if (lowestString < extraStrings)
                        score += 55 - lowestString * 12 + qMin(24, bassGap);
                    else
                        score -= 18;
                } else {
                    score += qMax(0, 12 - lowestString * 2);
                }
                if (bassGap >= 12) score += 16;
                if (sounded == 4) score += 14;
                else if (sounded == 5) score += 8;
                if (maxFret >= 0 && minFret < std::numeric_limits<int>::max())
                    score -= (maxFret - minFret) * 3;
                if (score > bestScore) {
                    bestScore = score;
                    best = current;
                }
                return;
            }

            current[string] = -1;
            search(string + 1, hasBass, sounded, previousPitch);
            for (int fret = firstFret; fret <= lastFret; ++fret) {
                const int absolutePitch = tuning[string] + fret;
                const int pitchClass = normalizedPitchClass(absolutePitch);
                if (!hasBass) {
                    if (pitchClass != bass) continue;
                } else if (!chordTones.contains(pitchClass)) {
                    continue;
                }
                if (previousPitch >= 0 && absolutePitch <= previousPitch) continue;
                current[string] = fret;
                search(string + 1, true, sounded + 1, absolutePitch);
            }
            current[string] = -1;
        };
    search(0, false, 0, -1);
    return best;
}

class ChordReferenceCanvas final : public QWidget {
public:
    explicit ChordReferenceCanvas(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(158);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setReference(const QString& chord, int strings, bool dropped)
    {
        if (chord_ == chord && strings_ == strings && dropped_ == dropped) return;
        chord_ = chord;
        strings_ = strings;
        dropped_ = dropped;
        parsed_ = jam2::practice::parseChord(chord_);
        tuning_ = guitarTuning(strings_, dropped_);
        guitarPositions_.clear();
        if (parsed_.valid && !parsed_.rest) {
            for (const int firstFret : std::array<int, 5>{0, 3, 6, 9, 12})
                guitarPositions_.push_back(guitarPosition(parsed_, tuning_, firstFret));
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#0c1213")));
        if (!parsed_.valid || parsed_.rest) {
            painter.setPen(QColor(QStringLiteral("#ddd7e8")));
            painter.drawText(rect().adjusted(12, 10, -12, -10), Qt::AlignLeft | Qt::AlignTop,
                QStringLiteral("Select or enter a chord to see piano and guitar references."));
            return;
        }

        const int split = qBound(430, static_cast<int>(width() * 0.40), qMax(430, width() - 620));
        painter.setPen(QPen(QColor(QStringLiteral("#344145")), 1));
        painter.drawLine(split, 6, split, height() - 6);
        drawPiano(painter, QRect(10, 4, split - 20, height() - 8), parsed_);
        drawGuitar(painter, QRect(split + 10, 4, width() - split - 20, height() - 8), parsed_);
    }

private:
    void drawPiano(QPainter& painter, const QRect& area, const jam2::practice::ParsedChord& chord)
    {
        const bool preferFlats = chord.rootName.contains(QLatin1Char('b')) ||
            chord.bassName.contains(QLatin1Char('b'));
        QStringList upperNames;
        QSet<int> chordTones;
        for (const int interval : chord.intervals) {
            const int pitch = normalizedPitchClass(chord.root + interval);
            chordTones.insert(pitch);
            const QString name = jam2::practice::noteName(pitch, preferFlats);
            if (!upperNames.contains(name)) upperNames.push_back(name);
        }
        const int bass = chord.bass >= 0 ? chord.bass : chord.root;
        QFont heading(QStringLiteral("Bahnschrift"), 10, QFont::DemiBold);
        painter.setFont(heading);
        painter.setPen(QColor(QStringLiteral("#ffd68a")));
        painter.drawText(QRect(area.left(), area.top(), area.width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("PIANO  ·  %1").arg(chord_));
        QFont detail(QStringLiteral("Bahnschrift"), 9);
        painter.setFont(detail);
        painter.setPen(QColor(QStringLiteral("#ddd7e8")));
        painter.drawText(QRect(area.left(), area.top() + 18, area.width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("Bass %1    Chord %2")
                .arg(chord.bassName.isEmpty() ? chord.rootName : chord.bassName, upperNames.join(QLatin1Char(' '))));

        const QRect keyboard(area.left(), area.top() + 40, area.width(), qMax(76, area.height() - 42));
        constexpr int whiteKeyCount = 15;
        const double whiteWidth = static_cast<double>(keyboard.width()) / whiteKeyCount;
        const QSet<int> blackPitchClasses{1, 3, 6, 8, 10};
        int whiteIndex = 0;
        QFont keyFont(QStringLiteral("Bahnschrift"), 8, QFont::DemiBold);
        painter.setFont(keyFont);
        for (int semitone = 0; semitone <= 24; ++semitone) {
            const int pitch = semitone % 12;
            if (blackPitchClasses.contains(pitch)) continue;
            const QRectF key(
                keyboard.left() + whiteIndex * whiteWidth,
                keyboard.top(), whiteWidth + 0.5, keyboard.height());
            const bool bassOn = semitone < 12 && pitch == bass;
            const bool chordOn = semitone >= 12 && semitone < 24 && chordTones.contains(pitch);
            painter.fillRect(key, bassOn ? QColor(QStringLiteral("#66d4cf"))
                : chordOn ? QColor(QStringLiteral("#e8a44a")) : QColor(QStringLiteral("#e2e1d9")));
            painter.setPen(QColor(QStringLiteral("#263033")));
            painter.drawRect(key);
            painter.drawText(key.adjusted(1, 1, -1, -4), Qt::AlignHCenter | Qt::AlignBottom,
                jam2::practice::noteName(pitch, preferFlats));
            ++whiteIndex;
        }
        const double blackWidth = whiteWidth * 0.62;
        const double blackHeight = keyboard.height() * 0.60;
        for (int semitone = 0; semitone < 24; ++semitone) {
            const int pitch = semitone % 12;
            if (!blackPitchClasses.contains(pitch)) continue;
            int whitesBefore = 0;
            for (int candidate = 0; candidate < semitone; ++candidate)
                if (!blackPitchClasses.contains(candidate % 12)) ++whitesBefore;
            const QRectF key(
                keyboard.left() + whitesBefore * whiteWidth - blackWidth / 2.0,
                keyboard.top(), blackWidth, blackHeight);
            const bool bassOn = semitone < 12 && pitch == bass;
            const bool chordOn = semitone >= 12 && chordTones.contains(pitch);
            painter.fillRect(key, bassOn ? QColor(QStringLiteral("#66d4cf"))
                : chordOn ? QColor(QStringLiteral("#e8a44a")) : QColor(QStringLiteral("#171c1d")));
            painter.setPen(QColor(QStringLiteral("#050707")));
            painter.drawRect(key);
            painter.setPen(chordOn || bassOn ? QColor(QStringLiteral("#182022")) : QColor(QStringLiteral("#f3f1e9")));
            painter.drawText(key.adjusted(0, 3, 0, -2), Qt::AlignHCenter | Qt::AlignBottom,
                jam2::practice::noteName(pitch, preferFlats));
        }
    }

    void drawGuitar(QPainter& painter, const QRect& area, const jam2::practice::ParsedChord& chord)
    {
        const QVector<int>& tuning = tuning_;
        const bool preferFlats = chord.rootName.contains(QLatin1Char('b')) ||
            chord.bassName.contains(QLatin1Char('b'));
        const int bass = chord.bass >= 0 ? chord.bass : chord.root;
        QFont heading(QStringLiteral("Bahnschrift"), 10, QFont::DemiBold);
        painter.setFont(heading);
        painter.setPen(QColor(QStringLiteral("#ffd68a")));
        painter.drawText(QRect(area.left(), area.top(), area.width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("GUITAR  ·  FIVE NECK POSITIONS"));
        const int columnWidth = qMax(80, area.width() / 5);
        const std::array<int, 5> firstFrets{0, 3, 6, 9, 12};
        for (int position = 0; position < 5; ++position) {
            const QRect column(area.left() + position * columnWidth, area.top() + 20,
                position == 4 ? area.right() - (area.left() + position * columnWidth) + 1 : columnWidth,
                area.height() - 20);
            const QVector<int> frets = guitarPositions_.value(position);
            drawGuitarPosition(painter, column.adjusted(5, 0, -5, 0), position, firstFrets[position],
                frets, tuning, chord.root, bass, preferFlats);
        }
    }

    void drawGuitarPosition(
        QPainter& painter,
        const QRect& area,
        int position,
        int firstFret,
        const QVector<int>& frets,
        const QVector<int>& tuning,
        int root,
        int bass,
        bool preferFlats)
    {
        QFont labelFont(QStringLiteral("Bahnschrift"), 9, QFont::DemiBold);
        painter.setFont(labelFont);
        painter.setPen(QColor(QStringLiteral("#ddd7e8")));
        painter.drawText(QRect(area.left(), area.top(), area.width(), 16), Qt::AlignHCenter | Qt::AlignVCenter,
            QStringLiteral("POSITION %1").arg(position + 1));
        const int gridTop = area.top() + 27;
        const int gridBottom = area.bottom() - 22;
        const int stringSpacing = qBound(12, (gridBottom - gridTop) / 5, 15);
        const int gridWidth = qMax(1, (tuning.size() - 1) * stringSpacing);
        const int gridLeft = area.center().x() - gridWidth / 2;
        const int gridRight = gridLeft + gridWidth;
        const double stringGap = stringSpacing;
        const double fretGap = static_cast<double>(gridBottom - gridTop) / 5.0;
        painter.setPen(QPen(QColor(QStringLiteral("#697678")), 1));
        for (int string = 0; string < tuning.size(); ++string) {
            const int x = qRound(gridLeft + string * stringGap);
            painter.drawLine(x, gridTop, x, gridBottom);
        }
        for (int fret = 0; fret <= 5; ++fret) {
            const int y = qRound(gridTop + fret * fretGap);
            painter.setPen(QPen(QColor(QStringLiteral("#697678")), firstFret == 0 && fret == 0 ? 3 : 1));
            painter.drawLine(gridLeft, y, gridRight, y);
        }
        painter.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
        painter.setPen(QColor(QStringLiteral("#9ca7a6")));
        painter.drawText(QRect(gridLeft - 25, gridTop, 22, 14), Qt::AlignRight | Qt::AlignVCenter,
            firstFret == 0 ? QStringLiteral("nut") : QString::number(firstFret));
        bool hasVoicing = false;
        for (const int fret : frets) hasVoicing = hasVoicing || fret >= 0;
        if (!hasVoicing) {
            painter.setPen(QColor(QStringLiteral("#9ca7a6")));
            painter.drawText(QRect(gridLeft, gridTop, gridRight - gridLeft, gridBottom - gridTop),
                Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("No practical\nvoicing"));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#b58a50")), 8, Qt::SolidLine, Qt::RoundCap));
        for (int string = 0; string + 1 < frets.size();) {
            const int fret = frets[string];
            if (fret <= 0 || frets[string + 1] != fret) {
                ++string;
                continue;
            }
            int lastString = string + 1;
            while (lastString + 1 < frets.size() && frets[lastString + 1] == fret) ++lastString;
            const double relative = firstFret == 0 ? fret - 1 : fret - firstFret;
            const int y = qRound(gridTop + (relative + 0.5) * fretGap);
            painter.drawLine(
                QPoint(qRound(gridLeft + string * stringGap), y),
                QPoint(qRound(gridLeft + lastString * stringGap), y));
            string = lastString + 1;
        }
        bool foundLowest = false;
        for (int string = 0; string < tuning.size(); ++string) {
            const int x = qRound(gridLeft + string * stringGap);
            const int fret = frets.value(string, -1);
            if (fret < 0) {
                painter.setPen(QColor(QStringLiteral("#7d8888")));
                painter.drawText(QRect(x - 6, gridTop - 15, 12, 12), Qt::AlignCenter, QStringLiteral("×"));
                continue;
            }
            const int pitch = normalizedPitchClass(tuning[string] + fret);
            const bool lowest = !foundLowest;
            foundLowest = true;
            const QColor dot = lowest && pitch == bass ? QColor(QStringLiteral("#66d4cf"))
                : pitch == root ? QColor(QStringLiteral("#e8a44a")) : QColor(QStringLiteral("#ddd7e8"));
            if (fret == 0) {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(dot, 2));
                painter.drawEllipse(QPoint(x, gridTop - 8), 5, 5);
            } else {
                const double relative = firstFret == 0 ? fret - 1 : fret - firstFret;
                const int y = qRound(gridTop + (relative + 0.5) * fretGap);
                painter.setPen(Qt::NoPen);
                painter.setBrush(dot);
                painter.drawEllipse(QPoint(x, y), 6, 6);
            }
        }
        painter.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
        for (int string = 0; string < tuning.size(); ++string) {
            const int x = qRound(gridLeft + string * stringGap);
            painter.setPen(QColor(QStringLiteral("#ddd7e8")));
            painter.drawText(QRect(x - 7, gridBottom + 3, 14, 14), Qt::AlignCenter,
                jam2::practice::noteName(tuning[string], preferFlats));
        }
    }

    QString chord_;
    int strings_ = 8;
    bool dropped_ = false;
    jam2::practice::ParsedChord parsed_;
    QVector<int> tuning_;
    QVector<QVector<int>> guitarPositions_;
};

}

BeatGridWidget::BeatGridWidget(BeatGridModel* model, const QString& lane, QWidget* parent)
    : QWidget(parent)
    , model_(model ? model : &ownedModel_)
    , fixedLane_(lane)
{
    authoringContent_ = new QWidget(this);
    authoringContent_->setFocusPolicy(Qt::StrongFocus);
    authoringContent_->setStyleSheet(QStringLiteral(
        "QLabel { background:transparent; color:#ddd7e8; font-size:11px; }"
        "QPushButton { font-size:11px; }"));
    authoringLayout_ = new QVBoxLayout(authoringContent_);
    authoringLayout_->setContentsMargins(2, 2, 2, 10);
    authoringLayout_->setSpacing(13);
    authoringScroll_ = new QScrollArea(this);
    authoringScroll_->setFrameShape(QFrame::NoFrame);
    authoringScroll_->setWidgetResizable(true);
    authoringScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    authoringScroll_->setWidget(authoringContent_);
    upcomingPreview_ = new QLabel(this);
    upcomingPreview_->setWordWrap(true);
    upcomingPreview_->setMinimumHeight(50);
    upcomingPreview_->setStyleSheet(QStringLiteral(
        "QLabel { background: #11131d; color: #ddd7e8; border: 1px solid #70588b; "
        "border-radius: 4px; padding: 7px 10px; }"));
    upcomingPreview_->hide();

    duplicateButton_ = new QPushButton(QStringLiteral("Copy Section..."), this);
    deleteButton_ = new QPushButton(QStringLiteral("Clear Section"), this);
    expandButton_ = new QPushButton(QStringLiteral("+1 Bar"), this);
    shrinkButton_ = new QPushButton(QStringLiteral("−1 Bar"), this);

    auto* top = new QHBoxLayout();
    top->addWidget(duplicateButton_, 1);
    top->addWidget(deleteButton_, 1);
    top->addWidget(expandButton_, 1);
    top->addWidget(shrinkButton_, 1);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(upcomingPreview_);
    layout->addWidget(authoringScroll_, 1);
    QObject::connect(duplicateButton_, &QPushButton::clicked, this, [this] {
        const int selected = selectedSectionIndex();
        if (selected < 0) return;
        QStringList destinations;
        for (int bank = 0; bank < model_->sections().size(); ++bank) {
            if (bank != selected) destinations.push_back(
                QStringLiteral("Section %1").arg(QChar(QLatin1Char('A').unicode() + bank)));
        }
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(
            this,
            QStringLiteral("Copy Section"),
            QStringLiteral("Replace the musical content in:"),
            destinations,
            0,
            false,
            &accepted);
        if (!accepted) return;
        const int destination = chosen.right(1).at(0).unicode() - QLatin1Char('A').unicode();
        if (!model_->copySection(selected, destination)) return;
        refresh();
        emitStructureChanged();
    });
    QObject::connect(deleteButton_, &QPushButton::clicked, this, [this] {
        const int selected = selectedSectionIndex();
        if (selected < 0) return;
        if (QMessageBox::question(
                this,
                QStringLiteral("Clear Section"),
                QStringLiteral("Clear the musical and generated content in Section %1? Custom tracks are kept.")
                    .arg(QChar(QLatin1Char('A').unicode() + selected))) != QMessageBox::Yes) return;
        if (!model_->clearSection(selected)) return;
        refresh();
        emitStructureChanged();
    });
    QObject::connect(expandButton_, &QPushButton::clicked, this, [this] { expandCurrent(); });
    QObject::connect(shrinkButton_, &QPushButton::clicked, this, [this] {
        const int section = selectedSectionIndex();
        if (section >= 0 && onShrinkRequested) onShrinkRequested(section);
    });
    editingBlocker_ = new QWidget(this);
    editingBlocker_->setToolTip(QStringLiteral(
        "Editing is protected while a track take is active"));
    editingBlocker_->hide();
    selectedSection_ = 0;
    refresh();
}

BeatGridModel& BeatGridWidget::model()
{
    return *model_;
}

void BeatGridWidget::setEditingProtected(bool protectedState)
{
    if (!editingBlocker_) return;
    editingBlocker_->setGeometry(rect());
    editingBlocker_->setVisible(protectedState);
    if (protectedState) editingBlocker_->raise();
}

QWidget* BeatGridWidget::createOverviewPagination(QWidget* parent)
{
    if (overviewPagination_) return overviewPagination_;
    overviewPagination_ = new QWidget(parent);
    auto* layout = new QHBoxLayout(overviewPagination_);
    layout->setContentsMargins(6, 0, 0, 0);
    layout->setSpacing(3);
    focusCurrentBarCheck_ = new QCheckBox(
        QStringLiteral("Focus current bar"), overviewPagination_);
    focusCurrentBarCheck_->setChecked(false);
    focusCurrentBarCheck_->setToolTip(QStringLiteral(
        "Select the playing bar and follow it through the section while playback is running"));
    focusCurrentBarCheck_->setStyleSheet(QStringLiteral(
        "QCheckBox { color:#cbd3d1; font:11px Bahnschrift; spacing:7px; padding:4px 8px 4px 0; }"
        "QCheckBox:hover { color:#f1eee5; }"));
    layout->addWidget(focusCurrentBarCheck_);

    overviewPageControls_ = new QWidget(overviewPagination_);
    auto* pageLayout = new QHBoxLayout(overviewPageControls_);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(3);
    overviewPageLabel_ = new QLabel(overviewPageControls_);
    overviewPageLabel_->setObjectName(QStringLiteral("SectionPageLabel"));
    overviewPageLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background:transparent; color:#899497; font:11px Bahnschrift; padding:0 6px; }"));
    pageLayout->addWidget(overviewPageLabel_);

    const auto makeButton = [this, pageLayout](const QIcon& icon, const QString& tooltip) {
        auto* button = new QToolButton(overviewPageControls_);
        button->setIcon(icon);
        button->setIconSize(QSize(24, 24));
        button->setFixedSize(32, 32);
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        button->setStyleSheet(QStringLiteral(
            "QToolButton { border:1px solid transparent; border-radius:3px; background:transparent; color:#ddd7e8; }"
            "QToolButton:hover { border-color:#58686c; background:#121a1c; }"
            "QToolButton:disabled { color:#4d5759; }"));
        pageLayout->addWidget(button);
        return button;
    };
    overviewFirstButton_ = makeButton(
        sectionPaginationIcon(false, true), QStringLiteral("First 32 bars"));
    overviewPreviousButton_ = makeButton(
        sectionPaginationIcon(false, false), QStringLiteral("Previous 32 bars"));
    overviewNextButton_ = makeButton(
        sectionPaginationIcon(true, false), QStringLiteral("Next 32 bars"));
    overviewLastButton_ = makeButton(
        sectionPaginationIcon(true, true), QStringLiteral("Last bars"));
    QObject::connect(overviewFirstButton_, &QToolButton::clicked, this,
        [this] { setOverviewPage(0); });
    QObject::connect(overviewPreviousButton_, &QToolButton::clicked, this,
        [this] { setOverviewPage(overviewPage_ - 1); });
    QObject::connect(overviewNextButton_, &QToolButton::clicked, this,
        [this] { setOverviewPage(overviewPage_ + 1); });
    QObject::connect(overviewLastButton_, &QToolButton::clicked, this, [this] {
        if (model_->sections().isEmpty()) return;
        const SongSection& section = model_->section(
            qBound(0, selectedSection_, model_->sections().size() - 1));
        const int bars = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
        setOverviewPage(jam2::gui::sectionOverviewPageCount(bars) - 1);
    });
    QObject::connect(focusCurrentBarCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        focusCurrentBar_ = enabled;
        if (!enabled || !gridRunning_ || model_->sections().isEmpty()) return;
        const int sectionIndex = selectedSectionIndex();
        if (sectionIndex < 0 || sectionIndex >= model_->sections().size()) return;
        const int beats = qMax(1, model_->section(sectionIndex).beats);
        const int liveBeat = static_cast<int>(gridBeat_ % static_cast<quint64>(beats));
        focusLivePosition(liveBeat / beatsPerBar_, liveBeat, beats);
    });
    layout->addWidget(overviewPageControls_);
    updateOverviewPagination();
    return overviewPagination_;
}

void BeatGridWidget::toggleFocusCurrentBar()
{
    if (focusCurrentBarCheck_) {
        focusCurrentBarCheck_->setChecked(!focusCurrentBarCheck_->isChecked());
    }
}

void BeatGridWidget::setFocusCurrentBar(bool enabled)
{
    if (focusCurrentBarCheck_) focusCurrentBarCheck_->setChecked(enabled);
}

void BeatGridWidget::setOverviewPage(int page)
{
    if (model_->sections().isEmpty()) return;
    const SongSection& section = model_->section(
        qBound(0, selectedSection_, model_->sections().size() - 1));
    const int bars = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    const int bounded = qBound(0, page, jam2::gui::sectionOverviewPageCount(bars) - 1);
    if (overviewPage_ == bounded) {
        updateOverviewPagination();
        return;
    }
    overviewPage_ = bounded;
    selectedBar_ = qMin(bars - 1, overviewPage_ * jam2::gui::kSectionOverviewBarsPerPage);
    selectedChordBeat_ = qMin(
        qMax(0, section.beats - 1), selectedBar_ * beatsPerBar_);
    rebuildAuthoringView();
}

void BeatGridWidget::updateOverviewPagination()
{
    if (!overviewPagination_) return;
    if (model_->sections().isEmpty() || mode() == Mode::Lyrics) {
        overviewPagination_->hide();
        return;
    }
    overviewPagination_->show();
    const SongSection& section = model_->section(
        qBound(0, selectedSection_, model_->sections().size() - 1));
    const int bars = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    const int pages = jam2::gui::sectionOverviewPageCount(bars);
    overviewPage_ = qBound(0, overviewPage_, pages - 1);
    const bool paginated = bars > jam2::gui::kSectionOverviewBarsPerPage;
    overviewPageControls_->setVisible(paginated);
    if (!paginated) return;
    const int first = overviewPage_ * jam2::gui::kSectionOverviewBarsPerPage + 1;
    const int last = qMin(
        bars, first + jam2::gui::kSectionOverviewBarsPerPage - 1);
    overviewPageLabel_->setText(QStringLiteral("BARS %1–%2 OF %3")
        .arg(first).arg(last).arg(bars));
    overviewFirstButton_->setEnabled(overviewPage_ > 0);
    overviewPreviousButton_->setEnabled(overviewPage_ > 0);
    overviewNextButton_->setEnabled(overviewPage_ + 1 < pages);
    overviewLastButton_->setEnabled(overviewPage_ + 1 < pages);
}

void BeatGridWidget::focusGeneratedSection(const QString& kind)
{
    for (int index = 0; index < model_->sections().size(); ++index) {
        const QString generatedKind = model_->section(index).generatedKind;
        if (generatedKind == kind ||
            (generatedKind == QStringLiteral("practice") &&
             (kind == QStringLiteral("chord") ||
              kind == QStringLiteral("beat") ||
              kind == QStringLiteral("lyric")))) {
            selectedSection_ = index;
            refresh();
            return;
        }
    }
    refresh();
}

void BeatGridWidget::setSelectedSectionIndex(int section)
{
    const int bounded = model_->sections().isEmpty()
        ? -1
        : qBound(0, section, model_->sections().size() - 1);
    if (selectedSection_ == bounded) return;
    selectedSection_ = bounded;
    overviewPage_ = 0;
    selectedBar_ = 0;
    refresh();
    if (onSelectedSectionChanged) onSelectedSectionChanged(selectedSection_);
}

void BeatGridWidget::refresh()
{
    if (selectedSection_ >= model_->sections().size()) {
        selectedSection_ = -1;
    }
    guitarStringCount_ = model_->guitarStringCount();
    guitarDropTuning_ = model_->guitarDropTuning();
    rebuildAuthoringView();
    updateActionButtons();
    updateOverviewPagination();
    updateUpcomingPreview();
    const quint64 beat = gridBeat_;
    const int subdivision = gridSubdivision_;
    const double beatPhase = gridBeatPhase_;
    const bool running = gridRunning_;
    gridBeat_ = (std::numeric_limits<quint64>::max)();
    setGridPosition(beat, subdivision, running, beatPhase);
}

void BeatGridWidget::setUpcomingSection(
    int section,
    quint64 beatsRemaining,
    int countdownBeatsPerBar,
    int targetBeatsPerBar,
    bool arrangementActive,
    bool arrangementArmed)
{
    upcomingSection_ = section;
    upcomingBeatsRemaining_ = beatsRemaining;
    upcomingCountdownBeatsPerBar_ = qMax(1, countdownBeatsPerBar);
    upcomingTargetBeatsPerBar_ = qMax(1, targetBeatsPerBar);
    upcomingArrangementActive_ = arrangementActive;
    upcomingArrangementArmed_ = arrangementArmed;
    updateUpcomingPreview();
}

void BeatGridWidget::updateUpcomingPreview()
{
    if (upcomingPreview_ == nullptr) return;
    const Mode currentMode = mode();
    const bool supportedMode = currentMode == Mode::Chord || currentMode == Mode::Beat;
    if (!upcomingArrangementActive_ || !supportedMode ||
        upcomingSection_ < 0 || upcomingSection_ >= model_->sections().size()) {
        upcomingPreview_->hide();
        return;
    }

    const SongSection& section = model_->section(upcomingSection_);
    QString when;
    if (upcomingArrangementArmed_) {
        when = QStringLiteral("ON PLAY");
    } else if (upcomingBeatsRemaining_ == 0) {
        when = QStringLiteral("NEXT BEAT");
    } else {
        const quint64 bars = upcomingBeatsRemaining_ /
            static_cast<quint64>(upcomingCountdownBeatsPerBar_);
        const quint64 beats = upcomingBeatsRemaining_ %
            static_cast<quint64>(upcomingCountdownBeatsPerBar_);
        if (bars > 0 && beats > 0) {
            when = QStringLiteral("IN %1 BAR%2 + %3 BEAT%4")
                .arg(bars).arg(bars == 1 ? QString{} : QStringLiteral("S"))
                .arg(beats).arg(beats == 1 ? QString{} : QStringLiteral("S"));
        } else if (bars > 0) {
            when = QStringLiteral("IN %1 BAR%2")
                .arg(bars).arg(bars == 1 ? QString{} : QStringLiteral("S"));
        } else {
            when = QStringLiteral("IN %1 BEAT%2")
                .arg(beats).arg(beats == 1 ? QString{} : QStringLiteral("S"));
        }
    }
    const QString heading = QStringLiteral("UP NEXT · SECTION %1 · %2 · %3")
        .arg(QChar(QLatin1Char('A').unicode() + upcomingSection_))
        .arg(sectionTitle(section).toUpper(), when);
    const int previewBeats = qMin(
        section.beats, qMax(upcomingTargetBeatsPerBar_, upcomingTargetBeatsPerBar_ * 2));

    QString detail;
    if (currentMode == Mode::Chord) {
        QStringList harmony;
        for (int beat = 0; beat < previewBeats && harmony.size() < 8; ++beat) {
            QString chord = section.chords.value(beat).trimmed();
            if (chord.isEmpty() && beat < section.musicalPatterns.size()) {
                for (const MusicalStep& step : section.musicalPatterns.at(beat).chords) {
                    if (step.state == MusicalStepState::Onset && !step.value.trimmed().isEmpty()) {
                        chord = step.value.trimmed();
                        break;
                    }
                }
            }
            if (!chord.isEmpty() && chord != QStringLiteral("-")) {
                harmony << QStringLiteral("%1.%2 %3")
                    .arg(beat / upcomingTargetBeatsPerBar_ + 1)
                    .arg(beat % upcomingTargetBeatsPerBar_ + 1)
                    .arg(chord);
            }
        }
        const auto notesFor = [&section, previewBeats](const QString& lane) {
            QStringList values;
            for (int beat = 0; beat < previewBeats && values.size() < 5; ++beat) {
                if (beat >= section.musicalPatterns.size()) continue;
                const MusicalBeatPattern& pattern = section.musicalPatterns.at(beat);
                const QVector<MusicalStep>* steps = lane == QStringLiteral("melody")
                    ? &pattern.melody : &pattern.bass;
                for (const MusicalStep& step : *steps) {
                    if (step.state == MusicalStepState::Onset && !step.value.trimmed().isEmpty()) {
                        values << step.value.trimmed();
                        if (values.size() >= 5) break;
                    }
                }
            }
            return values;
        };
        detail = QStringLiteral("CHORDS  %1")
            .arg(harmony.isEmpty() ? QStringLiteral("—") : harmony.join(QStringLiteral("  ·  ")));
        const QStringList melody = notesFor(QStringLiteral("melody"));
        const QStringList bass = notesFor(QStringLiteral("bass"));
        if (!melody.isEmpty()) detail += QStringLiteral("    MELODY  ") + melody.join(QLatin1Char(' '));
        if (!bass.isEmpty()) detail += QStringLiteral("    BASS  ") + bass.join(QLatin1Char(' '));
    } else {
        QStringList lanes;
        const QStringList laneNames = BeatGridModel::beatLaneNames();
        for (int lane = 0; lane < laneNames.size() && lanes.size() < 5; ++lane) {
            QStringList hits;
            int totalHits = 0;
            for (int beat = 0; beat < previewBeats; ++beat) {
                if (beat >= section.beatPatterns.size()) continue;
                const BeatPattern& pattern = section.beatPatterns.at(beat);
                const QString cells = pattern.lanes.value(lane);
                for (int step = 0; step < cells.size(); ++step) {
                    const QChar state = cells.at(step).toLower();
                    if (state != QLatin1Char('x') && state != QLatin1Char('a') &&
                        state != QLatin1Char('g')) continue;
                    ++totalHits;
                    if (hits.size() < 6) {
                        hits << QStringLiteral("%1.%2.%3")
                            .arg(beat / upcomingTargetBeatsPerBar_ + 1)
                            .arg(beat % upcomingTargetBeatsPerBar_ + 1)
                            .arg(step + 1);
                    }
                }
            }
            if (totalHits > 0) {
                QString laneText = laneNames.at(lane).toUpper() + QLatin1Char(' ') +
                    hits.join(QLatin1Char(' '));
                if (totalHits > hits.size()) {
                    laneText += QStringLiteral(" +%1").arg(totalHits - hits.size());
                }
                lanes << laneText;
            }
        }
        detail = lanes.isEmpty() ? QStringLiteral("DRUMS  —")
                                 : lanes.join(QStringLiteral("    "));
    }
    upcomingPreview_->setText(heading + QLatin1Char('\n') + detail);
    upcomingPreview_->show();
}

void BeatGridWidget::setBeatsPerBar(int beatsPerBar)
{
    const int bounded = qMax(1, beatsPerBar);
    if (beatsPerBar_ == bounded) {
        return;
    }
    beatsPerBar_ = bounded;
    refresh();
}

QVector<int> BeatGridWidget::chordDetailBarWidths(const SongSection& section) const
{
    QFont chordFont(QStringLiteral("Georgia"));
    chordFont.setPixelSize(16);
    const QFontMetrics chordMetrics(chordFont);
    QFont stepFont(QStringLiteral("Bahnschrift"));
    stepFont.setPixelSize(13);
    const QFontMetrics stepMetrics(stepFont);
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    QVector<int> widths;
    widths.reserve(barCount);
    for (int bar = 0; bar < barCount; ++bar) {
        int chordWidth = 0;
        int generatedWidth = 0;
        const int firstBeat = bar * beatsPerBar_;
        const int endBeat = qMin(section.beats, firstBeat + beatsPerBar_);
        for (int beat = firstBeat; beat < endBeat; ++beat) {
            QString chord = section.chords.value(beat).trimmed();
            if (chord.isEmpty()) chord = QStringLiteral("Chord");
            chordWidth += qBound(
                38,
                chordMetrics.horizontalAdvance(chord) + 18,
                156);

            if (!musicalLinesVisible_ || beat >= section.musicalPatterns.size()) continue;
            const MusicalBeatPattern& pattern = section.musicalPatterns.at(beat);
            int stepWidth = 30;
            const auto measure = [&stepMetrics, &stepWidth](const QVector<MusicalStep>& steps) {
                for (const MusicalStep& step : steps) {
                    stepWidth = qMax(
                        stepWidth,
                        stepMetrics.horizontalAdvance(musicalStepText(step)) + 12);
                }
            };
            measure(pattern.melody);
            measure(pattern.bass);
            measure(pattern.support);
            stepWidth = qMin(stepWidth, 82);
            const int division = qMax(1, pattern.division);
            generatedWidth += division * stepWidth + qMax(0, division - 1);
        }
        widths.push_back(qMax(140, qMax(chordWidth, generatedWidth)));
    }
    return widths;
}

int BeatGridWidget::chordDetailAvailableWidth() const
{
    const int viewportWidth = authoringScroll_ && authoringScroll_->viewport()
        ? authoringScroll_->viewport()->width() : width();
    // The focused card has 14 px inner margins. Expanded musical lines also
    // reserve their 92 px lane label and 8 px gap.
    const int reserved = 32 + (musicalLinesVisible_ ? 100 : 0);
    return qMax(240, viewportWidth - reserved);
}

jam2::gui::ChordDetailGroup BeatGridWidget::currentChordDetailGroup(
    const SongSection& section) const
{
    return jam2::gui::chordDetailGroupForWidths(
        chordDetailBarWidths(section),
        selectedBar_,
        chordDetailAvailableWidth());
}

void BeatGridWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (editingBlocker_) editingBlocker_->setGeometry(rect());
    scheduleResponsiveChordRebuild();
}

void BeatGridWidget::scheduleResponsiveChordRebuild()
{
    if (responsiveChordRebuildPending_ || mode() != Mode::Chord ||
        !authoringScroll_ || model_->sections().isEmpty()) return;
    responsiveChordRebuildPending_ = true;
    QTimer::singleShot(0, this, [this] {
        responsiveChordRebuildPending_ = false;
        if (mode() != Mode::Chord || model_->sections().isEmpty()) return;
        const SongSection& section = model_->section(
            qBound(0, selectedSection_, model_->sections().size() - 1));
        const jam2::gui::ChordDetailGroup group = currentChordDetailGroup(section);
        if (group.start != visibleChordGroupStart_ ||
            group.count != visibleChordGroupCount_) {
            rebuildAuthoringView();
        }
    });
}

void BeatGridWidget::setGridPosition(quint64 absoluteBeat, int subdivision, bool running, double beatPhase)
{
    gridBeat_ = absoluteBeat;
    gridSubdivision_ = subdivision;
    gridBeatPhase_ = qBound(0.0, beatPhase, 0.999999);
    gridRunning_ = running;
    const int sectionIndex = selectedSectionIndex();
    if (sectionIndex < 0 || sectionIndex >= model_->sections().size()) return;

    const int beats = qMax(1, model_->section(sectionIndex).beats);
    const int sectionBeat = static_cast<int>(absoluteBeat % static_cast<quint64>(beats));
    const int nextLiveBar = running ? sectionBeat / beatsPerBar_ : -1;
    const int nextLiveBeat = running ? sectionBeat : -1;
    if (nextLiveBar == authoringLiveBar_ && nextLiveBeat == authoringLiveBeat_) return;

    if (running && focusCurrentBar_) {
        focusLivePosition(nextLiveBar, nextLiveBeat, beats);
    }

    authoringLiveBar_ = nextLiveBar;
    authoringLiveBeat_ = nextLiveBeat;
    const QList<QWidget*> widgets = authoringContent_->findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
        bool changed = false;
        if (widget->property("authoringBar").isValid()) {
            const bool live = widget->property("authoringBar").toInt() == nextLiveBar;
            widget->setProperty("live", live);
            if (auto* button = qobject_cast<QPushButton*>(widget);
                button && button->property("barBaseText").isValid()) {
                button->setText(button->property("barBaseText").toString());
            }
            changed = true;
        }
        if (widget->property("authoringBeat").isValid()) {
            widget->setProperty("playhead", widget->property("authoringBeat").toInt() == nextLiveBeat);
            changed = true;
        }
        if (changed) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
            widget->update();
        }
    }
}

void BeatGridWidget::focusLivePosition(int liveBar, int liveBeat, int beats)
{
    if (!focusCurrentBar_ || liveBar < 0 || mode() == Mode::Lyrics) return;
    const int barCount = qMax(1, (qMax(1, beats) + beatsPerBar_ - 1) / beatsPerBar_);
    const int boundedBar = qBound(0, liveBar, barCount - 1);
    const int livePage = jam2::gui::sectionOverviewPageForBar(boundedBar, barCount);
    if (selectedBar_ != boundedBar || overviewPage_ != livePage) {
        selectedBar_ = boundedBar;
        overviewPage_ = livePage;
        selectedChordBeat_ = qBound(0, liveBeat, qMax(0, beats - 1));
        rebuildAuthoringView();
    }
    if (mode() == Mode::Chord) {
        selectFocusedChordBar(boundedBar, qBound(0, liveBeat, qMax(0, beats - 1)));
    }
}
void BeatGridWidget::applyRemoteCell(int section, const QString& lane, int beat, const QString& text)
{
    model_->setCell(section, lane, beat, text);
    rebuildAuthoringView();
}

BeatGridWidget::Mode BeatGridWidget::mode() const
{
    if (fixedLane_ == QStringLiteral("beat")) return Mode::Beat;
    if (fixedLane_ == QStringLiteral("lyric")) return Mode::Lyrics;
    return Mode::Chord;
}

void BeatGridWidget::rebuildChordReference()
{
    auto* canvas = static_cast<ChordReferenceCanvas*>(chordReferenceCanvas_);
    if (!canvas || mode() != Mode::Chord || model_->sections().isEmpty()) return;
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    int beat = qBound(0, selectedChordBeat_, qMax(0, section.beats - 1));
    QString chord;
    for (int candidate = beat; candidate >= 0; --candidate) {
        const QString value = section.chords.value(candidate).trimmed();
        if (value == QStringLiteral("-")) break;
        if (!value.isEmpty()) {
            chord = value.contains(QStringLiteral(" / "))
                ? value.section(QStringLiteral(" / "), 0, 0).trimmed() : value;
            break;
        }
    }
    canvas->setReference(chord, guitarStringCount_, guitarDropTuning_);
}

void BeatGridWidget::rebuildAuthoringView()
{
    chordReferenceCanvas_ = nullptr;
    guitarStringCountBox_ = nullptr;
    guitarTuningBox_ = nullptr;
    while (QLayoutItem* item = authoringLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    if (model_->sections().isEmpty()) return;
    const SongSection& section = model_->section(qBound(0, selectedSection_, model_->sections().size() - 1));
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    selectedBar_ = qBound(0, selectedBar_, barCount - 1);
    overviewPage_ = qBound(
        0,
        overviewPage_,
        jam2::gui::sectionOverviewPageCount(barCount) - 1);
    if (mode() == Mode::Chord) rebuildChordCards();
    else if (mode() == Mode::Beat) rebuildBeatSequencer();
    else rebuildLyricRows();
    updateOverviewPagination();
}

QWidget* BeatGridWidget::buildAuthoringOverview(bool chords)
{
    auto* overview = new QWidget(authoringContent_);
    auto* grid = new QGridLayout(overview);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    const int firstOverviewBar = overviewPage_ * jam2::gui::kSectionOverviewBarsPerPage;
    const int lastOverviewBar = qMin(
        barCount,
        firstOverviewBar + jam2::gui::kSectionOverviewBarsPerPage);
    for (int bar = firstOverviewBar; bar < lastOverviewBar; ++bar) {
        auto* chordButton = chords ? new ChordBarOverviewButton(overview) : nullptr;
        auto* grooveButton = chords ? nullptr : new BeatBarOverviewButton(overview);
        QPushButton* button = chordButton
            ? static_cast<QPushButton*>(chordButton)
            : grooveButton
            ? static_cast<QPushButton*>(grooveButton)
            : new QPushButton(overview);
        button->setObjectName(QStringLiteral("AuthoringBarButton"));
        button->setProperty("authoringBar", bar);
        button->setProperty(
            "focusedGroup",
            chords && bar >= visibleChordGroupStart_ &&
                bar < visibleChordGroupStart_ + visibleChordGroupCount_);
        button->setProperty("live", gridRunning_ && section.beats > 0 &&
            static_cast<int>(gridBeat_ % static_cast<quint64>(section.beats)) / beatsPerBar_ == bar);
        button->setCheckable(true);
        button->setChecked(bar == selectedBar_);
        button->setMinimumHeight(54);
        QString baseText;
        const int firstBeat = bar * beatsPerBar_;
        const int endBeat = qMin(section.beats, firstBeat + beatsPerBar_);
        if (chords) {
            chordButton->setTimeline(
                beatsPerBar_,
                chordOverviewData(section, bar, beatsPerBar_));
            baseText = QStringLiteral("BAR %1").arg(bar + 1);
        } else {
            QVector<GrooveOverviewHit> hits;
            for (int beat = firstBeat; beat < endBeat; ++beat) {
                const BeatPattern& pattern = section.beatPatterns[beat];
                for (int lane = 0; lane < pattern.lanes.size(); ++lane) {
                    const QString normalized = normalizedHitText(
                        pattern.lanes.at(lane), pattern.division);
                    for (int step = 0; step < normalized.size(); ++step) {
                        const QChar state = normalized.at(step);
                        if (state == QLatin1Char('.')) continue;
                        hits.push_back(GrooveOverviewHit{
                            jam2::gui::beatOverviewHitPhase(
                                beat - firstBeat,
                                step,
                                pattern.division,
                                beatsPerBar_),
                            lane,
                            state,
                        });
                    }
                }
            }
            grooveButton->setGroove(beatsPerBar_, std::move(hits));
            baseText = QStringLiteral("BAR %1").arg(bar + 1);
        }
        button->setProperty("barBaseText", baseText);
        button->setText(baseText);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { min-width: 92px; padding: 7px 9px; border: 1px solid #334044; border-radius: 3px; "
            "background: #0d1314; color: #ddd7e8; text-align: left; font: 11px Bahnschrift; }"
            "QPushButton:hover { border-color: #58686c; background: #121a1c; }"
            "QPushButton[focusedGroup='true'] { border-color:#80623b; background:#16140f; }"
            "QPushButton:checked { border:2px solid #e8a44a; color:#ffd68a; background:#241d14; "
            "border-bottom-width:3px; }"
            "QPushButton[live='true'] { border:2px solid #66d4cf; background:#122426; color:#efffff; }"));
        QObject::connect(button, &QPushButton::clicked, this, [this, bar, chords] {
            // The clicked overview button is deleted by the rebuild below.
            // Move focus to the persistent authoring host first so Qt does not
            // hand it to an unrelated control in the page header.
            authoringContent_->setFocus(Qt::MouseFocusReason);
            selectedBar_ = bar;
            if (chords) {
                const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
                const SongSection& section = model_->section(sectionIndex);
                const int firstBeat = bar * beatsPerBar_;
                const int endBeat = qMin(section.beats, firstBeat + beatsPerBar_);
                selectedChordBeat_ = firstBeat;
                for (int beat = firstBeat; beat < endBeat; ++beat) {
                    const QString chord = section.chords.value(beat).trimmed();
                    if (!chord.isEmpty() && chord != QStringLiteral("-")) {
                        selectedChordBeat_ = beat;
                        break;
                    }
                }
            }
            rebuildAuthoringView();
        });
        const int pageBar = bar - firstOverviewBar;
        grid->addWidget(button, pageBar / 8, pageBar % 8);
    }
    for (int column = 0; column < 8; ++column) grid->setColumnStretch(column, 1);
    return overview;
}

void BeatGridWidget::selectFocusedChordBar(int bar, int chordBeat)
{
    if (mode() != Mode::Chord || model_->sections().isEmpty()) return;
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    selectedBar_ = qBound(0, bar, barCount - 1);
    overviewPage_ = jam2::gui::sectionOverviewPageForBar(selectedBar_, barCount);
    if (chordBeat >= 0 && chordBeat < section.beats) {
        selectedChordBeat_ = chordBeat;
    } else {
        const int firstBeat = selectedBar_ * beatsPerBar_;
        const int endBeat = qMin(section.beats, firstBeat + beatsPerBar_);
        selectedChordBeat_ = firstBeat;
        for (int beat = firstBeat; beat < endBeat; ++beat) {
            const QString chord = section.chords.value(beat).trimmed();
            if (!chord.isEmpty() && chord != QStringLiteral("-")) {
                selectedChordBeat_ = beat;
                break;
            }
        }
    }
    for (QPushButton* button : authoringContent_->findChildren<QPushButton*>()) {
        if (button->objectName() == QStringLiteral("AuthoringBarButton")) {
            button->setChecked(button->property("authoringBar").toInt() == selectedBar_);
        }
        if (button->property("chordBarHeader").toBool()) {
            const int candidate = button->property("authoringBar").toInt();
            button->setText(QStringLiteral("BAR %1%2")
                .arg(candidate + 1)
                .arg(candidate == selectedBar_ ? QStringLiteral("     SELECTED") : QString()));
        }
    }
    for (QFrame* frame : authoringContent_->findChildren<QFrame*>()) {
        if (!frame->property("chordBarCard").toBool()) continue;
        frame->setProperty("selected", frame->property("authoringBar").toInt() == selectedBar_);
        frame->style()->unpolish(frame);
        frame->style()->polish(frame);
        frame->update();
    }
    rebuildChordReference();
}

void BeatGridWidget::rebuildChordCards()
{
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    const QVector<int> barWidths = chordDetailBarWidths(section);
    const jam2::gui::ChordDetailGroup group =
        jam2::gui::chordDetailGroupForWidths(
            barWidths, selectedBar_, chordDetailAvailableWidth());
    visibleChordGroupStart_ = group.start;
    visibleChordGroupCount_ = group.count;
    const int groupStartBar = group.start;
    const int groupEndBar = qMin(barCount, group.start + group.count);
    authoringLayout_->addWidget(buildAuthoringOverview(true));
    const int selectedBarFirstBeat = selectedBar_ * beatsPerBar_;
    const int selectedBarEndBeat = qMin(section.beats, selectedBarFirstBeat + beatsPerBar_);
    selectedChordBeat_ = selectedBarFirstBeat;
    for (int beat = selectedBarFirstBeat; beat < selectedBarEndBeat; ++beat) {
        const QString chord = section.chords.value(beat).trimmed();
        if (!chord.isEmpty() && chord != QStringLiteral("-")) {
            selectedChordBeat_ = beat;
            break;
        }
    }

    auto* workspace = new QWidget(authoringContent_);
    auto* workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(12);
    auto* focus = new QFrame(workspace);
    focus->setStyleSheet(QStringLiteral("QFrame { border: 1px solid #344145; border-radius: 4px; background: #0c1213; }"));
    auto* focusLayout = new QVBoxLayout(focus);
    focusLayout->setContentsMargins(14, 14, 14, 14);
    focusLayout->setSpacing(10);
    focusLayout->setAlignment(Qt::AlignTop);
    auto* bars = new QWidget(focus);
    bars->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bars->setFixedHeight(142);
    auto* barsLayout = new QHBoxLayout(bars);
    barsLayout->setContentsMargins(0, 0, 0, 0);
    barsLayout->setSpacing(8);
    for (int bar = groupStartBar; bar < groupEndBar; ++bar) {
        auto* card = new QFrame(bars);
        card->setProperty("authoringBar", bar);
        card->setProperty("chordBarCard", true);
        card->setProperty("live", gridRunning_ && section.beats > 0 &&
            static_cast<int>(gridBeat_ % static_cast<quint64>(section.beats)) / beatsPerBar_ == bar);
        card->setMinimumWidth(0);
        card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        card->setFixedHeight(142);
        card->setProperty("selected", bar == selectedBar_);
        card->setStyleSheet(QStringLiteral(
            "QFrame { border:1px solid #303c3f;border-radius:3px;background:#111719; }"
            "QFrame[selected='true'] { border-color:#e8a44a; }"
            "QFrame[live='true'] { border:2px solid #66d4cf;background:#122022; }"));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(0);
        auto* barLabel = new QPushButton(
            QStringLiteral("BAR %1%2").arg(bar + 1).arg(bar == selectedBar_ ? QStringLiteral("     SELECTED") : QString()), card);
        barLabel->setProperty("chordBarHeader", true);
        barLabel->setProperty("authoringBar", bar);
        barLabel->setStyleSheet(QStringLiteral(
            "QPushButton { border:0;border-bottom:1px solid #303c3f;background:transparent;color:#ddd7e8;"
            "font:11px Bahnschrift;padding:7px;text-align:left; }"
            "QPushButton:hover { color:#ffd68a;background:#181711; }"));
        QObject::connect(barLabel, &QPushButton::clicked, this, [this, bar] {
            selectFocusedChordBar(bar);
        });
        cardLayout->addWidget(barLabel);
        auto* beatRow = new QWidget(card);
        beatRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* beatLayout = new QHBoxLayout(beatRow);
        beatLayout->setContentsMargins(0, 0, 0, 0);
        beatLayout->setSpacing(0);
        for (int localBeat = 0; localBeat < beatsPerBar_; ++localBeat) {
            const int beat = bar * beatsPerBar_ + localBeat;
            if (beat >= section.beats) break;
            const MusicalBeatPattern& pattern = section.musicalPatterns[beat];
            auto* beatBox = new QWidget(beatRow);
            beatBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            beatBox->setStyleSheet(QStringLiteral("border:0;border-right:1px solid #2a3538;background:#111719;"));
            auto* beatBoxLayout = new QVBoxLayout(beatBox);
            beatBoxLayout->setContentsMargins(0, 0, 0, 0);
            beatBoxLayout->setSpacing(0);
            auto* beatNumber = new QLabel(QString::number(localBeat + 1), beatBox);
            beatNumber->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:10px Bahnschrift;padding:4px 6px 0;"));
            auto* edit = new QLineEdit(section.chords.value(beat), beatBox);
            edit->setAlignment(Qt::AlignCenter);
            edit->setFrame(false);
            edit->setFixedHeight(54);
            edit->setMinimumWidth(0);
            edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            if (!edit->text().trimmed().isEmpty()) edit->setToolTip(edit->text());
            edit->setPlaceholderText(localBeat == 0 ? QStringLiteral("Chord") : QStringLiteral("hold"));
            edit->setStyleSheet(QStringLiteral(
                "QLineEdit { border:0;background:transparent;color:#8eede4;font:16px Georgia;padding:4px; }"
                "QLineEdit:focus { background:#172426;border-bottom:2px solid #66d4cf; }"));
            QObject::connect(edit, &QLineEdit::textChanged, this,
                [edit, beatLayout, beatBox](const QString& text) {
                    QFont chordFont(QStringLiteral("Georgia"));
                    chordFont.setPixelSize(16);
                    const QString measured = text.trimmed().isEmpty()
                        ? QStringLiteral("Chord") : text.trimmed();
                    const int weight = qBound(
                        38,
                        QFontMetrics(chordFont).horizontalAdvance(measured) + 18,
                        156);
                    beatLayout->setStretchFactor(beatBox, weight);
                    edit->setToolTip(text.trimmed());
                });
            const auto commitChord = [this, sectionIndex, beat, edit] {
                const QString value = edit->text().trimmed();
                if (model_->section(sectionIndex).chords.value(beat).trimmed() == value) return;
                model_->setCell(sectionIndex, QStringLiteral("chord"), beat, value);
                selectedChordBeat_ = beat;
                if (onCellEdited) onCellEdited(
                    sectionIndex, QStringLiteral("chord"), beat, value, model_->revision());
                const int bar = beat / beatsPerBar_;
                const SongSection& updatedSection = model_->section(sectionIndex);
                for (QPushButton* button : authoringContent_->findChildren<QPushButton*>()) {
                    if (button->property("authoringBar").toInt() == bar &&
                        button->property("barBaseText").isValid()) {
                        const QString baseText = QStringLiteral("BAR %1").arg(bar + 1);
                        button->setProperty("barBaseText", baseText);
                        button->setText(baseText);
                        if (auto* chordOverview = dynamic_cast<ChordBarOverviewButton*>(button)) {
                            chordOverview->setTimeline(
                                beatsPerBar_,
                                chordOverviewData(updatedSection, bar, beatsPerBar_));
                        }
                    }
                }
                rebuildChordReference();
            };
            QObject::connect(edit, &QLineEdit::editingFinished, this, commitChord);
            QObject::connect(edit, &QLineEdit::cursorPositionChanged, this, [this, bar, beat](int, int) {
                selectFocusedChordBar(bar, beat);
            });
            edit->setContextMenuPolicy(Qt::CustomContextMenu);
            QObject::connect(edit, &QLineEdit::customContextMenuRequested, this,
                [this, edit, commitChord](const QPoint& point) {
                    QMenu menu(this);
                    QAction* sustain = menu.addAction(QStringLiteral("Hold previous (~)"));
                    sustain->setData(QStringLiteral("~"));
                    QAction* rest = menu.addAction(QStringLiteral("Rest (-)"));
                    rest->setData(QStringLiteral("-"));
                    menu.addSeparator();
                    const QStringList roots{
                        QStringLiteral("A"), QStringLiteral("A#"),
                        QStringLiteral("B"), QStringLiteral("C"),
                        QStringLiteral("C#"), QStringLiteral("D"),
                        QStringLiteral("D#"), QStringLiteral("E"),
                        QStringLiteral("F"), QStringLiteral("F#"),
                        QStringLiteral("G"), QStringLiteral("G#")};
                    const QString currentSymbol = edit->text().trimmed();
                    const jam2::practice::ParsedChord parsed =
                        jam2::practice::parseChord(currentSymbol);
                    const bool hasBaseChord = parsed.valid && !parsed.rest &&
                        !parsed.rootName.isEmpty();
                    if (hasBaseChord) {
                        const QString baseChord = currentSymbol.section(QLatin1Char('/'), 0, 0);
                        QMenu* slashMenu = menu.addMenu(QStringLiteral("Slash bass"));
                        QAction* noSlash = slashMenu->addAction(QStringLiteral("No slash bass"));
                        noSlash->setData(baseChord);
                        noSlash->setEnabled(parsed.bass >= 0);
                        slashMenu->addSeparator();
                        for (const QString& bass : roots) {
                            QAction* action = slashMenu->addAction(bass);
                            action->setData(baseChord + QLatin1Char('/') + bass);
                            action->setCheckable(true);
                            action->setChecked(parsed.bassName == bass);
                        }
                    } else {
                        QMenu* slashMenu = menu.addMenu(
                            QStringLiteral("Slash bass — select a chord first"));
                        slashMenu->setEnabled(false);
                    }
                    menu.addSeparator();
                    const QStringList suffixes{
                        QString(), QStringLiteral("m"), QStringLiteral("5"),
                        QStringLiteral("sus2"), QStringLiteral("sus4"), QStringLiteral("dim"),
                        QStringLiteral("aug"), QStringLiteral("6"), QStringLiteral("m6"),
                        QStringLiteral("7"), QStringLiteral("maj7"), QStringLiteral("m7"),
                        QStringLiteral("m7b5"), QStringLiteral("dim7"), QStringLiteral("add9"),
                        QStringLiteral("madd9"), QStringLiteral("9"), QStringLiteral("maj9"),
                        QStringLiteral("m9"), QStringLiteral("13"), QStringLiteral("7b9"),
                        QStringLiteral("7#9"), QStringLiteral("alt"), QStringLiteral("#11"),
                        QStringLiteral("maj7#11"), QStringLiteral("maj9#11")};
                    for (const QString& root : roots) {
                        QMenu* rootMenu = menu.addMenu(root);
                        for (const QString& suffix : suffixes) {
                            const QString symbol = root + suffix;
                            QAction* action = rootMenu->addAction(symbol);
                            action->setData(symbol);
                        }
                    }
                    if (QAction* chosen = menu.exec(edit->mapToGlobal(point))) {
                        edit->setText(chosen->data().toString());
                        commitChord();
                    }
                });
            auto* division = new QPushButton(
                QStringLiteral("%1 step%2").arg(pattern.division).arg(pattern.division == 1 ? QString() : QStringLiteral("s")), beatBox);
            division->setMinimumWidth(0);
            division->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            division->setStyleSheet(QStringLiteral(
                "QPushButton { border:0;border-top:1px solid #2a3538;background:#0d1314;color:#ddd7e8;font:10px Bahnschrift;min-height:24px; }"
                "QPushButton:hover { color:#ffd68a;background:#211a12; }"));
            QObject::connect(division, &QPushButton::clicked, this, [this, sectionIndex, beat] {
                selectedBar_ = beat / beatsPerBar_;
                const QList<int> values = BeatGridModel::musicalDivisionValues();
                const int current = model_->section(sectionIndex).musicalPatterns[beat].division;
                const int next = values[(values.indexOf(current) + 1) % values.size()];
                model_->setMusicalDivision(sectionIndex, beat, next);
                if (onMusicalDivisionChanged) onMusicalDivisionChanged(sectionIndex, beat, next, model_->revision());
                rebuildAuthoringView();
            });
            beatBoxLayout->addWidget(beatNumber);
            beatBoxLayout->addWidget(edit, 1);
            beatBoxLayout->addWidget(division);
            const QString chordText = section.chords.value(beat).trimmed();
            QFont chordFont(QStringLiteral("Georgia"));
            chordFont.setPixelSize(16);
            const int chordWeight = qBound(
                38,
                QFontMetrics(chordFont).horizontalAdvance(
                    chordText.isEmpty() ? QStringLiteral("Chord") : chordText) + 18,
                156);
            beatLayout->addWidget(beatBox, chordWeight);
        }
        cardLayout->addWidget(beatRow);
        barsLayout->addWidget(card, qMax(1, barWidths.value(bar, 1)));
    }
    focusLayout->addWidget(bars);
    auto* tip = new QLabel(QStringLiteral(
        "Right click on a beat to select a chord, or left click and type chord directly. "
        "~ for hold, - for rest."), focus);
    tip->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:12px Bahnschrift;"));
    focusLayout->addWidget(tip);
    const int referenceInsertIndex = focusLayout->count();

    auto* lineHeading = new QPushButton(focus);
    lineHeading->setText(QStringLiteral("%1  GENERATED MUSICAL LINES     onset note · ~ hold · - rest")
        .arg(musicalLinesVisible_ ? QStringLiteral("▾") : QStringLiteral("▸")));
    lineHeading->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:12px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
        "QPushButton:hover { color:#ffd68a; }"));
    focusLayout->addWidget(lineHeading);
    for (const auto& lane : QList<QPair<QString, QString>>{
            {QStringLiteral("Melody"), QStringLiteral("melody")},
            {QStringLiteral("Bass"), QStringLiteral("bass")},
            {QStringLiteral("Supporting line"), QStringLiteral("support")}}) {
        auto* row = new QWidget(focus);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto* label = new QLabel(lane.first + QStringLiteral("\ngenerated"), row);
        label->setFixedWidth(92);
        label->setStyleSheet(QStringLiteral("border:1px solid #303c3f;background:#111719;color:#ddd7e8;font:12px Bahnschrift;padding:6px;"));
        rowLayout->addWidget(label);
        for (int bar = groupStartBar; bar < groupEndBar; ++bar) {
            auto* lineBar = new QFrame(row);
            lineBar->setMinimumWidth(0);
            lineBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            lineBar->setStyleSheet(QStringLiteral("QFrame { border:1px solid #303c3f;background:#0e1415; }"));
            auto* lineBarLayout = new QVBoxLayout(lineBar);
            lineBarLayout->setContentsMargins(0, 0, 0, 0);
            lineBarLayout->setSpacing(0);
            auto* lineBarLabel = new QLabel(QStringLiteral("BAR %1").arg(bar + 1), lineBar);
            lineBarLabel->setStyleSheet(QStringLiteral("border:0;border-bottom:1px solid #2c373a;color:#ddd7e8;font:11px Bahnschrift;padding:3px 5px;"));
            lineBarLayout->addWidget(lineBarLabel);
            auto* beatTrack = new QWidget(lineBar);
            auto* beatTrackLayout = new QHBoxLayout(beatTrack);
            beatTrackLayout->setContentsMargins(0, 0, 0, 0);
            beatTrackLayout->setSpacing(1);
            for (int localBeat = 0; localBeat < beatsPerBar_; ++localBeat) {
                const int beat = bar * beatsPerBar_ + localBeat;
                if (beat >= section.beats) break;
                const MusicalBeatPattern& pattern = section.musicalPatterns[beat];
                const QVector<MusicalStep>& steps = lane.second == QStringLiteral("melody") ? pattern.melody
                    : lane.second == QStringLiteral("bass") ? pattern.bass : pattern.support;
                auto* stepHost = new QWidget(beatTrack);
                stepHost->setMinimumWidth(0);
                stepHost->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                auto* stepLayout = new QHBoxLayout(stepHost);
                stepLayout->setContentsMargins(0, 0, 0, 0);
                stepLayout->setSpacing(1);
                for (int step = 0; step < steps.size(); ++step) {
                    auto* edit = new QLineEdit(musicalStepText(steps[step]), stepHost);
                    edit->setAlignment(Qt::AlignCenter);
                    edit->setFrame(false);
                    edit->setMinimumHeight(32);
                    edit->setMinimumWidth(0);
                    edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
                    if (!edit->text().trimmed().isEmpty() &&
                        edit->text() != QStringLiteral("~") &&
                        edit->text() != QStringLiteral("-")) {
                        edit->setToolTip(edit->text());
                    }
                    const QString color = lane.second == QStringLiteral("melody") ? QStringLiteral("#c1a7e8")
                        : lane.second == QStringLiteral("bass") ? QStringLiteral("#eeb15e") : QStringLiteral("#7ec3ee");
                    edit->setStyleSheet(QStringLiteral(
                        "QLineEdit { border:0;border-right:1px solid #293437;background:#111719;color:%1;font:13px Bahnschrift;padding:1px; }"
                        "QLineEdit:focus { background:#172426;border-bottom:2px solid #66d4cf; }").arg(color));
                    QObject::connect(edit, &QLineEdit::editingFinished, this,
                        [this, sectionIndex, beat, step, laneName = lane.second, edit] {
                            const QString value = edit->text().trimmed();
                            const QString canonical = value.isEmpty() ? QStringLiteral("~") : value;
                            const MusicalBeatPattern& pattern = model_->section(sectionIndex).musicalPatterns[beat];
                            const QVector<MusicalStep>& current = laneName == QStringLiteral("melody") ? pattern.melody
                                : laneName == QStringLiteral("bass") ? pattern.bass : pattern.support;
                            if (musicalStepText(current.value(step)) == canonical) return;
                            model_->setMusicalStep(sectionIndex, beat, step, laneName, value);
                            if (onMusicalStepEdited) onMusicalStepEdited(
                                sectionIndex, beat, step, laneName, value, model_->revision());
                        });
                    stepLayout->addWidget(edit, 1);
                }
                beatTrackLayout->addWidget(stepHost, qMax(1, steps.size()));
            }
            lineBarLayout->addWidget(beatTrack);
            rowLayout->addWidget(lineBar, qMax(1, barWidths.value(bar, 1)));
        }
        row->setVisible(musicalLinesVisible_);
        focusLayout->addWidget(row);
    }
    QObject::connect(lineHeading, &QPushButton::clicked, this, [this] {
        musicalLinesVisible_ = !musicalLinesVisible_;
        rebuildAuthoringView();
    });

    auto* referenceSection = new QWidget(focus);
    referenceSection->setObjectName(QStringLiteral("ChordReferenceSection"));
    auto* referenceSectionLayout = new QVBoxLayout(referenceSection);
    referenceSectionLayout->setContentsMargins(0, 0, 0, 0);
    referenceSectionLayout->setSpacing(5);
    auto* referenceToggle = new QPushButton(referenceSection);
    referenceToggle->setText(QStringLiteral("%1  CHORD REFERENCE")
        .arg(chordReferenceVisible_ ? QStringLiteral("▾") : QStringLiteral("▸")));
    referenceToggle->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:12px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
        "QPushButton:hover { color:#ffd68a; }"));
    referenceSectionLayout->addWidget(referenceToggle);
    auto* inspector = new QFrame(referenceSection);
    inspector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    inspector->setMinimumHeight(218);
    inspector->setMaximumHeight(236);
    inspector->setStyleSheet(QStringLiteral("QFrame { border:1px solid #344145;border-radius:4px;background:#0c1213; }"));
    auto* inspectorLayout = new QVBoxLayout(inspector);
    inspectorLayout->setContentsMargins(10, 8, 10, 8);
    inspectorLayout->setSpacing(4);
    auto* controls = new QWidget(inspector);
    controls->setObjectName(QStringLiteral("ChordReferenceControls"));
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(7);
    auto* explanation = new QLabel(
        QStringLiteral("Cyan = bass    Gold = root    White = other chord tones"), controls);
    explanation->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:11px Bahnschrift;"));
    controlsLayout->addWidget(explanation);
    controlsLayout->addStretch(1);
    auto* stringsLabel = new QLabel(QStringLiteral("Strings"), controls);
    stringsLabel->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:11px Bahnschrift;"));
    controlsLayout->addWidget(stringsLabel);
    guitarStringCountBox_ = new QComboBox(controls);
    for (const int strings : {6, 7, 8})
        guitarStringCountBox_->addItem(QString::number(strings), strings);
    guitarStringCountBox_->setCurrentIndex(guitarStringCountBox_->findData(guitarStringCount_));
    guitarStringCountBox_->setFixedWidth(62);
    controlsLayout->addWidget(guitarStringCountBox_);
    auto* tuningLabel = new QLabel(QStringLiteral("Tuning"), controls);
    tuningLabel->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:11px Bahnschrift;"));
    controlsLayout->addWidget(tuningLabel);
    guitarTuningBox_ = new QComboBox(controls);
    guitarTuningBox_->addItem(QStringLiteral("Standard"), false);
    guitarTuningBox_->addItem(QStringLiteral("Dropped"), true);
    guitarTuningBox_->setCurrentIndex(guitarTuningBox_->findData(guitarDropTuning_));
    guitarTuningBox_->setMinimumWidth(104);
    controlsLayout->addWidget(guitarTuningBox_);
    inspectorLayout->addWidget(controls);
    chordReferenceCanvas_ = new ChordReferenceCanvas(inspector);
    inspectorLayout->addWidget(chordReferenceCanvas_);
    inspector->setVisible(chordReferenceVisible_);
    referenceSectionLayout->addWidget(inspector);
    QObject::connect(referenceToggle, &QPushButton::clicked, this, [this, referenceToggle, inspector] {
        chordReferenceVisible_ = !chordReferenceVisible_;
        inspector->setVisible(chordReferenceVisible_);
        referenceToggle->setText(QStringLiteral("%1  CHORD REFERENCE")
            .arg(chordReferenceVisible_ ? QStringLiteral("▾") : QStringLiteral("▸")));
    });
    QObject::connect(guitarStringCountBox_, &QComboBox::currentIndexChanged, this, [this] {
        guitarStringCount_ = guitarStringCountBox_->currentData().toInt();
        if (model_->setGuitarReference(guitarStringCount_, guitarDropTuning_)) emitStructureChanged();
        rebuildChordReference();
    });
    QObject::connect(guitarTuningBox_, &QComboBox::currentIndexChanged, this, [this] {
        guitarDropTuning_ = guitarTuningBox_->currentData().toBool();
        if (model_->setGuitarReference(guitarStringCount_, guitarDropTuning_)) emitStructureChanged();
        rebuildChordReference();
    });
    workspaceLayout->addWidget(focus, 1);
    focusLayout->insertWidget(referenceInsertIndex, referenceSection);
    workspace->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    authoringLayout_->addWidget(workspace, 1);
    rebuildChordReference();
}

void BeatGridWidget::rebuildBeatSequencer()
{
    authoringLayout_->addWidget(buildAuthoringOverview(false));
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    const int firstBeat = selectedBar_ * beatsPerBar_;
    auto* panel = new QFrame(authoringContent_);
    panel->setProperty("authoringBar", selectedBar_);
    panel->setProperty("live", gridRunning_ && section.beats > 0 &&
        static_cast<int>(gridBeat_ % static_cast<quint64>(section.beats)) / beatsPerBar_ == selectedBar_);
    panel->setStyleSheet(QStringLiteral(
        "QFrame { border:1px solid #344145;border-radius:4px;background:#0c1213; }"
        "QFrame[live='true'] { border:2px solid #66d4cf;background:#101d1f; }"));
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(14, 14, 14, 14);
    panelLayout->setSpacing(8);
    auto* divisionRow = new QWidget(panel);
    auto* divisionLayout = new QHBoxLayout(divisionRow);
    divisionLayout->setContentsMargins(0, 0, 0, 0);
    divisionLayout->setSpacing(1);
    auto* divisionLabel = new QLabel(QStringLiteral("DIVISION"), divisionRow);
    divisionLabel->setFixedWidth(116);
    divisionLabel->setStyleSheet(QStringLiteral("border:1px solid #334044;background:#111719;color:#ddd7e8;font:11px Bahnschrift;padding:7px;"));
    divisionLayout->addWidget(divisionLabel);
    for (int localBeat = 0; localBeat < beatsPerBar_; ++localBeat) {
        const int beat = firstBeat + localBeat;
        if (beat >= section.beats) break;
        auto* button = new QPushButton(
            QStringLiteral("Beat %1 · %2").arg(localBeat + 1).arg(BeatGridModel::beatDivisionLabel(section.beatPatterns[beat].division)), divisionRow);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { border:1px solid #334044;border-left:2px solid #e8a44a;"
            "background:#101617;color:#ddd7e8;font:11px Bahnschrift;min-height:31px; }"
            "QPushButton:hover { color:#ffd68a;border-color:#6f5937; }"));
        QObject::connect(button, &QPushButton::clicked, this, [this, sectionIndex, beat] {
            const QList<int> values = BeatGridModel::beatDivisionValues();
            const int current = model_->section(sectionIndex).beatPatterns[beat].division;
            const int next = values[(values.indexOf(current) + 1) % values.size()];
            model_->setBeatDivision(sectionIndex, beat, next);
            if (onBeatDivisionChanged) onBeatDivisionChanged(sectionIndex, beat, next, model_->revision());
            rebuildAuthoringView();
        });
        divisionLayout->addWidget(button, 1);
    }
    panelLayout->addWidget(divisionRow);

    const QStringList lanes = BeatGridModel::beatLaneNames();
    for (const QString& laneName : BeatGridModel::beatVisualLaneNames()) {
        const int lane = lanes.indexOf(laneName);
        if (lane < 0) continue;
        auto* row = new QWidget(panel);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(1);
        auto* label = new QLabel(laneName, row);
        label->setFixedWidth(116);
        label->setStyleSheet(QStringLiteral("border:1px solid #334044;background:#111719;color:#ddd7e8;font:11px Bahnschrift;padding:8px;"));
        rowLayout->addWidget(label);
        for (int localBeat = 0; localBeat < beatsPerBar_; ++localBeat) {
            const int beat = firstBeat + localBeat;
            if (beat >= section.beats) break;
            const BeatPattern& pattern = section.beatPatterns[beat];
            const QString states = normalizedHitText(pattern.lanes[lane], pattern.division);
            auto* beatGroup = new QWidget(row);
            beatGroup->setObjectName(QStringLiteral("BeatStepGroup"));
            beatGroup->setStyleSheet(QStringLiteral(
                "QWidget#BeatStepGroup { border:0;border-left:2px solid #e8a44a;background:transparent; }"));
            auto* beatLayout = new QHBoxLayout(beatGroup);
            beatLayout->setContentsMargins(2, 0, 0, 0);
            beatLayout->setSpacing(1);
            for (int step = 0; step < pattern.division; ++step) {
                auto* cell = new QPushButton(beatGroup);
                cell->setProperty("authoringBeat", beat);
                cell->setProperty("authoringStep", step);
                cell->setProperty("playhead", gridRunning_ && section.beats > 0 &&
                    static_cast<int>(gridBeat_ % static_cast<quint64>(section.beats)) == beat);
                cell->setMinimumHeight(38);
                const auto applyState = [cell](QChar state) {
                    const QString color = state == QLatin1Char('a') ? QStringLiteral("#e8a44a")
                        : state == QLatin1Char('g') ? QStringLiteral("#70d49d")
                        : state == QLatin1Char('x') ? QStringLiteral("#66d4cf") : QStringLiteral("#151c1e");
                    const QString radius = state == QLatin1Char('g') ? QStringLiteral("10px") : QStringLiteral("1px");
                    cell->setStyleSheet(QStringLiteral(
                        "QPushButton { border:1px solid #334044;background:#101617;min-width:22px; }"
                        "QPushButton::after { background:%1; }"
                        "QPushButton { color:%1; font-size:20px; border-radius:%2; }"
                        "QPushButton[playhead='true'] { background:#17282a;border-color:#66d4cf; }").arg(color, radius));
                    cell->setText(state == QLatin1Char('.') ? QStringLiteral("□")
                        : state == QLatin1Char('g') ? QStringLiteral("●") : QStringLiteral("■"));
                };
                applyState(states[step]);
                QObject::connect(cell, &QPushButton::clicked, this,
                    [this, sectionIndex, beat, lane, step, cell, applyState] {
                        const BeatPattern& currentPattern = model_->section(sectionIndex).beatPatterns[beat];
                        const QString current = normalizedHitText(currentPattern.lanes[lane], currentPattern.division);
                        const QString updated = withNextHitState(
                            current, currentPattern.division, step);
                        model_->setBeatHit(sectionIndex, beat, lane, updated);
                        applyState(updated[step]);
                        if (onBeatHitEdited) onBeatHitEdited(sectionIndex, beat, lane, updated, model_->revision());
                    });
                cell->setContextMenuPolicy(Qt::CustomContextMenu);
                QObject::connect(cell, &QPushButton::customContextMenuRequested, this,
                    [this, sectionIndex, beat, lane, step, cell, applyState](const QPoint& point) {
                        QMenu menu(this);
                        const BeatPattern& currentPattern =
                            model_->section(sectionIndex).beatPatterns[beat];
                        const QString current = normalizedHitText(
                            currentPattern.lanes[lane], currentPattern.division);
                        for (const auto& choice : QList<QPair<QString, QChar>>{
                                {QStringLiteral("Hit"), QLatin1Char('x')},
                                {QStringLiteral("Accent"), QLatin1Char('a')},
                                {QStringLiteral("Ghost"), QLatin1Char('g')},
                                {QStringLiteral("Empty"), QLatin1Char('.')}}) {
                            QAction* action = menu.addAction(choice.first);
                            action->setData(QString(choice.second));
                            action->setCheckable(true);
                            action->setChecked(current[step] == choice.second);
                        }
                        QAction* chosen = menu.exec(cell->mapToGlobal(point));
                        if (!chosen) return;
                        QString updated = normalizedHitText(currentPattern.lanes[lane], currentPattern.division);
                        updated[step] = chosen->data().toString()[0];
                        model_->setBeatHit(sectionIndex, beat, lane, updated);
                        applyState(updated[step]);
                        if (onBeatHitEdited) onBeatHitEdited(sectionIndex, beat, lane, updated, model_->revision());
                    });
                beatLayout->addWidget(cell, 1);
            }
            rowLayout->addWidget(beatGroup, 1);
        }
        panelLayout->addWidget(row);
    }
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    authoringLayout_->addWidget(panel, 1);
}

void BeatGridWidget::rebuildLyricRows()
{
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    auto* cueSheet = new QWidget(authoringContent_);
    cueSheet->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* cueLayout = new QVBoxLayout(cueSheet);
    cueLayout->setContentsMargins(0, 0, 0, 0);
    cueLayout->setSpacing(5);
    for (int bar = 0; bar < barCount; ++bar) {
        const int beat = qMin(bar * beatsPerBar_, section.beats - 1);
        auto* row = new QFrame(cueSheet);
        row->setProperty("authoringBar", bar);
        row->setProperty("live", gridRunning_ && section.beats > 0 &&
            static_cast<int>(gridBeat_ % static_cast<quint64>(section.beats)) / beatsPerBar_ == bar);
        row->setStyleSheet(QStringLiteral(
            "QFrame { border:1px solid #303c3f;border-radius:3px;background:#0d1314; }"
            "QFrame[live='true'] { border:2px solid #66d4cf;background:#112022; }"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 6, 0);
        rowLayout->setSpacing(0);
        auto* number = new QLabel(QStringLiteral("BAR %1").arg(bar + 1), row);
        number->setFixedWidth(64);
        number->setAlignment(Qt::AlignCenter);
        number->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:11px Bahnschrift;"));
        auto* edit = new LyricBarEdit(row);
        edit->setProperty("lyricBar", bar);
        edit->setInitialText(section.lyrics.value(beat));
        edit->setPlaceholderText(QStringLiteral("Write a lyric cue for this bar…"));
        edit->setMinimumHeight(56);
        edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        edit->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { border:0;border-left:1px solid #2e393c;background:transparent;color:#ddd9d0;"
            "font:16px Georgia;padding:10px 12px; }"
            "QPlainTextEdit:focus { border-left:2px solid #66d4cf;background:#11191a; }"));
        edit->onEdited = [this, sectionIndex, beat](const QString& text) {
            model_->setCell(sectionIndex, QStringLiteral("lyric"), beat, text.trimmed());
            if (onCellEdited) onCellEdited(sectionIndex, QStringLiteral("lyric"), beat, text.trimmed(), model_->revision());
        };
        edit->onLinesPasted = [this, sectionIndex, bar, barCount](const QStringList& lines) {
            const int count = qMin(lines.size(), barCount - bar);
            for (int offset = 0; offset < count; ++offset) {
                const int beat = (bar + offset) * beatsPerBar_;
                model_->setCell(sectionIndex, QStringLiteral("lyric"), beat, lines[offset].trimmed());
                if (onCellEdited) onCellEdited(
                    sectionIndex, QStringLiteral("lyric"), beat, lines[offset].trimmed(), model_->revision());
            }
            const int targetBar = qMin(barCount - 1, bar + count - 1);
            QTimer::singleShot(0, this, [this, targetBar] {
                rebuildAuthoringView();
                for (QPlainTextEdit* plain : authoringContent_->findChildren<QPlainTextEdit*>()) {
                    if (plain->property("lyricBar").toInt() == targetBar) plain->setFocus();
                }
            });
        };
        edit->onAdvance = [this, bar] {
            for (QPlainTextEdit* plain : authoringContent_->findChildren<QPlainTextEdit*>()) {
                if (plain->property("lyricBar").toInt() == bar + 1) plain->setFocus();
            }
        };
        auto* clear = new QPushButton(QStringLiteral("×"), row);
        clear->setFixedSize(28, 28);
        clear->setToolTip(QStringLiteral("Clear bar %1 lyric").arg(bar + 1));
        QObject::connect(clear, &QPushButton::clicked, this, [this, sectionIndex, beat, edit] {
            edit->setInitialText(QString());
            model_->setCell(sectionIndex, QStringLiteral("lyric"), beat, QString());
            if (onCellEdited) onCellEdited(sectionIndex, QStringLiteral("lyric"), beat, QString(), model_->revision());
        });
        rowLayout->addWidget(number);
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(clear);
        cueLayout->addWidget(row);
    }
    auto* centered = new QWidget(authoringContent_);
    auto* centeredLayout = new QHBoxLayout(centered);
    centeredLayout->setContentsMargins(0, 0, 0, 0);
    centeredLayout->addWidget(cueSheet, 1);
    centered->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    authoringLayout_->addWidget(centered, 1);
}

void BeatGridWidget::expandCurrent()
{
    if (model_->sections().isEmpty()) {
        return;
    }
    const int section = selectedSectionIndex();
    if (section < 0 || section >= model_->sections().size()) {
        return;
    }
    const int beats = model_->section(section).beats + beatsPerBar_;
    model_->resizeSection(section, beats);
    refresh();
    if (onGridResized) {
        onGridResized(section, beats, model_->revision());
    }
}

int BeatGridWidget::selectedSectionIndex() const
{
    return selectedSection_ >= 0 && selectedSection_ < model_->sections().size()
        ? selectedSection_ : -1;
}

void BeatGridWidget::updateActionButtons()
{
    const bool selected = selectedSectionIndex() >= 0;
    if (duplicateButton_) duplicateButton_->setEnabled(selected);
    if (deleteButton_) deleteButton_->setEnabled(selected);
    if (shrinkButton_) {
        const bool canRemoveBar = selected &&
            model_->section(selectedSectionIndex()).beats > beatsPerBar_;
        shrinkButton_->setEnabled(canRemoveBar);
        shrinkButton_->setToolTip(canRemoveBar
            ? QStringLiteral("Remove one bar; Jam2 will warn before deleting conflicting content")
            : QStringLiteral("A Section must keep at least one bar"));
    }
}
void BeatGridWidget::emitStructureChanged()
{
    if (onStructureChanged) {
        onStructureChanged();
    }
}
