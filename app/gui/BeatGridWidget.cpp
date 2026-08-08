#include "BeatGridWidget.hpp"
#include "MusicTheory.hpp"

#include <QAction>
#include <QColor>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>

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

QString withHitState(const QString& text, int division, int index, bool checked)
{
    QString out = normalizedHitText(text, division);
    if (index >= 0 && index < out.size()) {
        out[index] = checked ? QChar('x') : QChar('.');
    }
    return out;
}

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
        "QLabel { color:#ddd7e8; font-size:11px; }"
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
    shrinkButton_ = new QPushButton(QStringLiteral("-1 Bar"), this);

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
    QObject::connect(shrinkButton_, &QPushButton::clicked, this, [this] { shrinkCurrent(); });
    selectedSection_ = 0;
    refresh();
}

BeatGridModel& BeatGridWidget::model()
{
    return *model_;
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
    if (mode() == Mode::Chord) rebuildChordCards();
    else if (mode() == Mode::Beat) rebuildBeatSequencer();
    else rebuildLyricRows();
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
    for (int bar = 0; bar < barCount; ++bar) {
        auto* button = new QPushButton(overview);
        button->setObjectName(QStringLiteral("AuthoringBarButton"));
        button->setProperty("authoringBar", bar);
        button->setProperty("focusedGroup", chords && bar / 4 == selectedBar_ / 4);
        button->setProperty("live", gridRunning_ && section.beats > 0 &&
            static_cast<int>(gridBeat_ % static_cast<quint64>(section.beats)) / beatsPerBar_ == bar);
        button->setCheckable(true);
        button->setChecked(bar == selectedBar_);
        button->setMinimumHeight(54);
        QString summary;
        const int firstBeat = bar * beatsPerBar_;
        const int endBeat = qMin(section.beats, firstBeat + beatsPerBar_);
        if (chords) {
            QStringList values;
            for (int beat = firstBeat; beat < endBeat; ++beat) {
                const QString value = section.chords.value(beat).trimmed();
                if (!value.isEmpty()) values.push_back(value);
            }
            summary = values.isEmpty() ? QStringLiteral("Empty") : values.join(QStringLiteral("  ·  "));
        } else {
            QString dots(16, QChar(0x00b7));
            for (int beat = firstBeat; beat < endBeat; ++beat) {
                const BeatPattern& pattern = section.beatPatterns[beat];
                for (const QString& lane : pattern.lanes) {
                    const QString normalized = normalizedHitText(lane, pattern.division);
                    for (int step = 0; step < normalized.size(); ++step) {
                        if (normalized[step] == QLatin1Char('.')) continue;
                        const int localBeat = beat - firstBeat;
                        const int slot = qBound(0, localBeat * 4 + step * 4 / qMax(1, pattern.division), 15);
                        dots[slot] = QChar(0x25cf);
                    }
                }
            }
            summary = dots;
        }
        const QString baseText = QStringLiteral("BAR %1\n%2").arg(bar + 1).arg(summary);
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
        grid->addWidget(button, bar / 8, bar % 8);
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
    authoringLayout_->addWidget(buildAuthoringOverview(true));
    const int sectionIndex = qBound(0, selectedSection_, model_->sections().size() - 1);
    const SongSection& section = model_->section(sectionIndex);
    const int barCount = qMax(1, (section.beats + beatsPerBar_ - 1) / beatsPerBar_);
    const int groupStartBar = (selectedBar_ / 4) * 4;
    const int groupEndBar = qMin(barCount, groupStartBar + 4);
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
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
            edit->setPlaceholderText(localBeat == 0 ? QStringLiteral("Chord") : QStringLiteral("hold"));
            edit->setStyleSheet(QStringLiteral(
                "QLineEdit { border:0;background:transparent;color:#8eede4;font:16px Georgia;padding:4px; }"
                "QLineEdit:focus { background:#172426;border-bottom:2px solid #66d4cf; }"));
            const auto commitChord = [this, sectionIndex, beat, edit] {
                const QString value = edit->text().trimmed();
                if (model_->section(sectionIndex).chords.value(beat).trimmed() == value) return;
                model_->setCell(sectionIndex, QStringLiteral("chord"), beat, value);
                selectedChordBeat_ = beat;
                if (onCellEdited) onCellEdited(
                    sectionIndex, QStringLiteral("chord"), beat, value, model_->revision());
                const int bar = beat / beatsPerBar_;
                const SongSection& updatedSection = model_->section(sectionIndex);
                QStringList summary;
                for (int candidate = bar * beatsPerBar_;
                     candidate < qMin(updatedSection.beats, (bar + 1) * beatsPerBar_);
                     ++candidate) {
                    const QString chord = updatedSection.chords.value(candidate).trimmed();
                    if (!chord.isEmpty()) summary.push_back(chord);
                }
                const QString baseText = QStringLiteral("BAR %1\n%2")
                    .arg(bar + 1)
                    .arg(summary.isEmpty() ? QStringLiteral("Empty") : summary.join(QStringLiteral("  ·  ")));
                for (QPushButton* button : authoringContent_->findChildren<QPushButton*>()) {
                    if (button->property("authoringBar").toInt() == bar &&
                        button->property("barBaseText").isValid()) {
                        button->setProperty("barBaseText", baseText);
                        button->setText(baseText);
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
            beatLayout->addWidget(beatBox, 1);
        }
        cardLayout->addWidget(beatRow);
        barsLayout->addWidget(card, 1);
    }
    focusLayout->addWidget(bars);
    auto* tip = new QLabel(QStringLiteral(
        "Type directly and press Tab to advance. ~ holds and - rests. Right-click any chord for the picker."), focus);
    tip->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:11px Bahnschrift;"));
    focusLayout->addWidget(tip);
    const int referenceInsertIndex = focusLayout->count();

    auto* lineHeading = new QPushButton(focus);
    lineHeading->setText(QStringLiteral("%1  GENERATED MUSICAL LINES     onset note · ~ hold · - rest")
        .arg(musicalLinesVisible_ ? QStringLiteral("▾") : QStringLiteral("▸")));
    lineHeading->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:11px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
        "QPushButton:hover { color:#ffd68a; }"));
    focusLayout->addWidget(lineHeading);
    QList<QWidget*> generatedRows;
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
        label->setStyleSheet(QStringLiteral("border:1px solid #303c3f;background:#111719;color:#ddd7e8;font:11px Bahnschrift;padding:6px;"));
        rowLayout->addWidget(label);
        for (int bar = groupStartBar; bar < groupEndBar; ++bar) {
            auto* lineBar = new QFrame(row);
            lineBar->setStyleSheet(QStringLiteral("QFrame { border:1px solid #303c3f;background:#0e1415; }"));
            auto* lineBarLayout = new QVBoxLayout(lineBar);
            lineBarLayout->setContentsMargins(0, 0, 0, 0);
            lineBarLayout->setSpacing(0);
            auto* lineBarLabel = new QLabel(QStringLiteral("BAR %1").arg(bar + 1), lineBar);
            lineBarLabel->setStyleSheet(QStringLiteral("border:0;border-bottom:1px solid #2c373a;color:#ddd7e8;font:10px Bahnschrift;padding:3px 5px;"));
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
                auto* stepLayout = new QHBoxLayout(stepHost);
                stepLayout->setContentsMargins(0, 0, 0, 0);
                stepLayout->setSpacing(1);
                for (int step = 0; step < steps.size(); ++step) {
                    auto* edit = new QLineEdit(musicalStepText(steps[step]), stepHost);
                    edit->setAlignment(Qt::AlignCenter);
                    edit->setFrame(false);
                    edit->setMinimumHeight(32);
                    const QString color = lane.second == QStringLiteral("melody") ? QStringLiteral("#c1a7e8")
                        : lane.second == QStringLiteral("bass") ? QStringLiteral("#eeb15e") : QStringLiteral("#7ec3ee");
                    edit->setStyleSheet(QStringLiteral(
                        "QLineEdit { border:0;border-right:1px solid #293437;background:#111719;color:%1;font:12px Bahnschrift;padding:1px; }"
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
                beatTrackLayout->addWidget(stepHost, 1);
            }
            lineBarLayout->addWidget(beatTrack);
            rowLayout->addWidget(lineBar, 1);
        }
        row->setVisible(musicalLinesVisible_);
        generatedRows.push_back(row);
        focusLayout->addWidget(row);
    }
    QObject::connect(lineHeading, &QPushButton::clicked, this, [this, lineHeading, generatedRows] {
        musicalLinesVisible_ = !musicalLinesVisible_;
        lineHeading->setText(QStringLiteral("%1  GENERATED MUSICAL LINES     onset note · ~ hold · - rest")
            .arg(musicalLinesVisible_ ? QStringLiteral("▾") : QStringLiteral("▸")));
        for (QWidget* row : generatedRows) row->setVisible(musicalLinesVisible_);
    });

    auto* referenceSection = new QWidget(focus);
    auto* referenceSectionLayout = new QVBoxLayout(referenceSection);
    referenceSectionLayout->setContentsMargins(0, 0, 0, 0);
    referenceSectionLayout->setSpacing(5);
    auto* referenceToggle = new QPushButton(referenceSection);
    referenceToggle->setText(QStringLiteral("%1  CHORD REFERENCE")
        .arg(chordReferenceVisible_ ? QStringLiteral("▾") : QStringLiteral("▸")));
    referenceToggle->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:11px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
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
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(7);
    auto* explanation = new QLabel(
        QStringLiteral("Cyan = bass    Gold = root    White = other chord tones"), controls);
    explanation->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:10px Bahnschrift;"));
    controlsLayout->addWidget(explanation);
    controlsLayout->addStretch(1);
    auto* stringsLabel = new QLabel(QStringLiteral("Strings"), controls);
    stringsLabel->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:10px Bahnschrift;"));
    controlsLayout->addWidget(stringsLabel);
    guitarStringCountBox_ = new QComboBox(controls);
    for (const int strings : {6, 7, 8})
        guitarStringCountBox_->addItem(QString::number(strings), strings);
    guitarStringCountBox_->setCurrentIndex(guitarStringCountBox_->findData(guitarStringCount_));
    guitarStringCountBox_->setFixedWidth(62);
    controlsLayout->addWidget(guitarStringCountBox_);
    auto* tuningLabel = new QLabel(QStringLiteral("Tuning"), controls);
    tuningLabel->setStyleSheet(QStringLiteral("border:0;color:#ddd7e8;font:10px Bahnschrift;"));
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
                        const QString updated = withHitState(current, currentPattern.division, step, current[step] == QLatin1Char('.'));
                        model_->setBeatHit(sectionIndex, beat, lane, updated);
                        applyState(updated[step]);
                        if (onBeatHitEdited) onBeatHitEdited(sectionIndex, beat, lane, updated, model_->revision());
                    });
                cell->setContextMenuPolicy(Qt::CustomContextMenu);
                QObject::connect(cell, &QPushButton::customContextMenuRequested, this,
                    [this, sectionIndex, beat, lane, step, cell, applyState](const QPoint& point) {
                        QMenu menu(this);
                        for (const auto& choice : QList<QPair<QString, QChar>>{
                                {QStringLiteral("Empty"), QLatin1Char('.')}, {QStringLiteral("Normal"), QLatin1Char('x')},
                                {QStringLiteral("Accent"), QLatin1Char('a')}, {QStringLiteral("Ghost"), QLatin1Char('g')}}) {
                            QAction* action = menu.addAction(choice.first);
                            action->setData(QString(choice.second));
                        }
                        QAction* chosen = menu.exec(cell->mapToGlobal(point));
                        if (!chosen) return;
                        const BeatPattern& currentPattern = model_->section(sectionIndex).beatPatterns[beat];
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

void BeatGridWidget::shrinkCurrent()
{
    if (model_->sections().isEmpty()) {
        return;
    }
    const int section = selectedSectionIndex();
    if (section < 0 || section >= model_->sections().size()) {
        return;
    }
    const int beats = qMax(beatsPerBar_, model_->section(section).beats - beatsPerBar_);
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
}
void BeatGridWidget::emitStructureChanged()
{
    if (onStructureChanged) {
        onStructureChanged();
    }
}
