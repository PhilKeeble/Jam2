#include "MainWindowPages.hpp"

#include "MainWindow.hpp"

#include "DetailSectionEdit.hpp"
#include "GuiPresentation.hpp"
#include "GuiControlContract.hpp"
#include "GuiTheme.hpp"
#include "SessionController.hpp"
#include "TrackWidgets.hpp"
#include "ContentLimits.hpp"

#include "tuning_profile.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <cmath>
#include <limits>

namespace {

QIcon settingsIcon()
{
    constexpr int size = 32;
    constexpr int teeth = 12;
    constexpr double pi = 3.14159265358979323846;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainterPath path;
    QPolygonF outline;
    for (int point = 0; point < teeth * 2; ++point) {
        const double angle = (static_cast<double>(point) * pi / teeth) - (pi / 2.0);
        const double radius = point % 2 == 0 ? 13.0 : 10.0;
        outline.append(QPointF(
            size / 2.0 + std::cos(angle) * radius,
            size / 2.0 + std::sin(angle) * radius));
    }
    path.setFillRule(Qt::OddEvenFill);
    path.addPolygon(outline);
    path.closeSubpath();
    path.addEllipse(QPointF(size / 2.0, size / 2.0), 4.0, 4.0);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(path, jam2::gui::theme::text);
    return QIcon(pixmap);
}

enum class IdeaHeaderAction {
    Generate,
    Browse,
    Continue,
    Wav,
    Details,
};

void styleIdeaHeaderAction(QPushButton* button, IdeaHeaderAction action)
{
    if (!button) return;
    QString outline;
    QString fill;
    QString hover;
    int width = 116;
    switch (action) {
    case IdeaHeaderAction::Generate:
        outline = QStringLiteral("#e2ac53");
        fill = QStringLiteral("rgba(35,27,20,232)");
        hover = QStringLiteral("#302317");
        width = 154;
        break;
    case IdeaHeaderAction::Browse:
        outline = QStringLiteral("#9fb56f");
        fill = QStringLiteral("rgba(28,36,21,232)");
        hover = QStringLiteral("#26331c");
        width = 132;
        break;
    case IdeaHeaderAction::Continue:
        outline = QStringLiteral("#66d4cf");
        fill = QStringLiteral("rgba(13,42,43,232)");
        hover = QStringLiteral("#123638");
        width = 124;
        break;
    case IdeaHeaderAction::Wav:
        outline = QStringLiteral("#b08be4");
        fill = QStringLiteral("rgba(27,20,37,232)");
        hover = QStringLiteral("#261b34");
        width = 116;
        break;
    case IdeaHeaderAction::Details:
        outline = QStringLiteral("#56a4f4");
        fill = QStringLiteral("rgba(17,31,50,232)");
        hover = QStringLiteral("#172c46");
        width = 112;
        break;
    }
    button->setFixedSize(width, 31);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { color:#f3e6cf;background:%1;border:1px solid %2;border-radius:4px;"
        "padding:0 9px;font-family:Bahnschrift;font-size:9pt;font-weight:600;letter-spacing:0.45px; }"
        "QPushButton:hover { background:%3;border-color:%2; }"
        "QPushButton:pressed { background:#241c20;border-color:#ffd68e; }")
        .arg(fill, outline, hover));
}

class AnimatedBrandIcon final : public QWidget {
public:
    explicit AnimatedBrandIcon(QWidget* parent)
        : QWidget(parent),
          logo_(QStringLiteral(":/jam2/assets/logo-nebula.png"))
    {
        setFixedSize(82, 82);
        setAccessibleName(QStringLiteral("Jam2gether"));
        animation_.setInterval(50);
        QObject::connect(&animation_, &QTimer::timeout, this, [this] {
            phase_ += 0.045;
            if (phase_ >= 6.283185307179586) {
                phase_ -= 6.283185307179586;
            }
            update();
        });
        animation_.start();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor(5, 8, 9));
        if (logo_.isNull()) {
            painter.setPen(QColor(226, 172, 83));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("J2"));
            return;
        }

        const qreal pulse = (std::sin(phase_) + 1.0) * 0.5;
        const qreal scale = 0.986 + pulse * 0.014;
        const QSizeF targetSize(width() * scale, height() * scale);
        const QRectF target(
            (width() - targetSize.width()) * 0.5,
            (height() - targetSize.height()) * 0.5,
            targetSize.width(),
            targetSize.height());
        painter.setOpacity(0.94 + pulse * 0.06);
        painter.drawPixmap(target, logo_, logo_.rect());
    }

private:
    QPixmap logo_;
    QTimer animation_;
    qreal phase_ = 0.0;
};

class DoubleClickBpmSpin final : public QSpinBox {
public:
    explicit DoubleClickBpmSpin(QWidget* parent)
        : QSpinBox(parent)
    {
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setAlignment(Qt::AlignCenter);
        setKeyboardTracking(false);
        lineEdit()->setReadOnly(true);
        lineEdit()->installEventFilter(this);
        QObject::connect(this, &QAbstractSpinBox::editingFinished, this, [this] {
            lineEdit()->setReadOnly(true);
            lineEdit()->deselect();
        });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == lineEdit() && event->type() == QEvent::MouseButtonDblClick) {
            lineEdit()->setReadOnly(false);
            lineEdit()->setFocus(Qt::MouseFocusReason);
            lineEdit()->selectAll();
            return true;
        }
        return QSpinBox::eventFilter(watched, event);
    }
};

} // namespace

void MainWindowPages::build(MainWindow& w)
{
    w.setWindowTitle(QStringLiteral("Jam2gether"));
    w.setWindowIcon(QIcon(QStringLiteral(":/jam2/assets/logo-nebula.png")));
    w.setMinimumSize(1280, 720);

    auto* brandMark = new AnimatedBrandIcon(&w);
    w.connectionLabel_ = new QLabel(QStringLiteral("AUDIO OFF"), &w);
    w.connectionLabel_->setObjectName(QStringLiteral("StatusPill"));

    auto* header = new QHBoxLayout();
    header->setSpacing(10);
    header->addWidget(brandMark);
    auto* jamTitleEdit = new jam2::gui::DetailSectionEdit(&w);
    jamTitleEdit->setText(w.chordModel_.title());
    jamTitleEdit->setProperty("sectionEditable", true);
    jamTitleEdit->setToolTip(QStringLiteral("Double-click to rename this jam"));
    w.songTitleEdit_ = jamTitleEdit;
    w.songTitleEdit_->setObjectName(QStringLiteral("SongTitle"));
    jam2::gui::registerGuiControl(
        *w.songTitleEdit_, QStringLiteral("session.song-title"),
        QStringLiteral("session.rename"));
    w.songTitleEdit_->setMinimumWidth(240);
    w.songTitleEdit_->setMaximumWidth(520);
    header->addSpacing(20);
    header->addWidget(w.songTitleEdit_, 1);
    auto* startJamButton = new QPushButton(QStringLiteral("Start Jam"), &w);
    auto* joinJamButton = new QPushButton(QStringLiteral("Join Jam"), &w);
    w.leaveJamButton_ = new QPushButton(QStringLiteral("Leave Jam"), &w);
    jam2::gui::registerGuiControl(
        *startJamButton, QStringLiteral("session.start"),
        QStringLiteral("session.create-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    jam2::gui::registerGuiControl(
        *joinJamButton, QStringLiteral("session.join"),
        QStringLiteral("session.join-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    jam2::gui::registerGuiControl(
        *w.leaveJamButton_, QStringLiteral("session.leave"),
        QStringLiteral("session.lifecycle"),
        jam2::gui::GuiControlAvailability::StateGated);
    for (QPushButton* button : {startJamButton, joinJamButton, w.leaveJamButton_}) {
        button->setObjectName(QStringLiteral("DetailTool"));
        button->setFixedHeight(32);
        header->addWidget(button);
    }
    w.leaveJamButton_->setEnabled(false);
    QObject::connect(startJamButton, &QPushButton::clicked, &w, [&w] {
        w.showStartJamDialog();
    });
    QObject::connect(joinJamButton, &QPushButton::clicked, &w, [&w] {
        w.showJoinJamDialog();
    });
    QObject::connect(w.leaveJamButton_, &QPushButton::clicked, &w, [&w] {
        w.stopJam(true);
    });
    w.jamSyncButton_ = new QToolButton(&w);
    w.jamSyncButton_->setObjectName(QStringLiteral("JamSyncButton"));
    jam2::gui::registerGuiControl(
        *w.jamSyncButton_, QStringLiteral("session.jam-sync"),
        QStringLiteral("session.sync-policy-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    w.jamSyncButton_->setText(QStringLiteral("\u25cf  JAM SYNC"));
    w.jamSyncButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    w.jamSyncButton_->setFixedHeight(32);
    w.jamSyncButton_->setMinimumWidth(116);
    w.jamSyncButton_->setToolTip(QStringLiteral(
        "Choose which session changes are shared with every peer"));
    w.jamSyncButton_->setStyleSheet(QStringLiteral(
        "QToolButton { border:1px solid #5e5268;border-radius:5px;background:rgba(28,23,34,224);"
        "color:#e2ac53;font:600 9pt Bahnschrift;padding:4px 10px; }"
        "QToolButton:hover,QToolButton:pressed { border-color:#e2ac53;background:#302638; }"));
    header->addWidget(w.jamSyncButton_);
    QObject::connect(w.jamSyncButton_, &QToolButton::clicked, &w, [&w] {
        w.showJamSyncDialog();
    });
    w.updateJamSyncPresentation();
    w.connectionLabel_->setFixedHeight(32);
    w.connectionLabel_->setMinimumWidth(96);
    w.connectionLabel_->setMaximumWidth(240);
    w.connectionLabel_->setAlignment(Qt::AlignCenter);
    auto* settingsButton = new QToolButton(&w);
    settingsButton->setObjectName(QStringLiteral("SettingsButton"));
    jam2::gui::registerGuiControl(
        *settingsButton, QStringLiteral("application.settings"),
        QStringLiteral("application.settings-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    settingsButton->setIcon(settingsIcon());
    settingsButton->setIconSize(QSize(18, 18));
    settingsButton->setFixedSize(34, 34);
    settingsButton->setToolTip(QStringLiteral("Audio and jam defaults"));
    settingsButton->setAccessibleName(QStringLiteral("Settings"));
    QObject::connect(settingsButton, &QToolButton::clicked, &w, [&w] {
        w.showSettingsDialog();
    });
    header->addWidget(settingsButton);
    header->addWidget(w.connectionLabel_);
    w.showAudioOffSessionHeaderStatus();

    jamTitleEdit->onCommitted = [&w](const QString& name) {
        (void)w.renameCurrentJam(name);
    };
    QWidget* sessionPage = buildSessionPage(w);
    QWidget* chordPage = buildSongPage(w);
    QWidget* beatPage = buildBeatPage(w);
    auto* lyricPage = new QWidget(&w);
    w.lyricGrid_ = new BeatGridWidget(&w.lyricModel_, QStringLiteral("lyric"), lyricPage);
    auto* lyricTop = new QHBoxLayout();
    addBankControls(w, lyricPage, lyricTop, false, "lyrics");
    lyricTop->addStretch(1);
    auto* lyricLayout = new QVBoxLayout(lyricPage);
    lyricLayout->addLayout(lyricTop);
    lyricLayout->addWidget(w.lyricGrid_, 1);
    QWidget* trackPage = buildTrackPage(w);
    QWidget* metronomePage = buildMetronomePage(w);
    buildAudioControls(w);

    auto sendCellEdit = [&w](int section, const QString& lane, int beat, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    w.beatGrid_->onCellEdited = sendCellEdit;
    w.lyricGrid_->onCellEdited = sendCellEdit;
    w.beatGrid_->onBeatHitEdited = [&w](int section, int beat, int lane, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.hit")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("text"), text},
        });
        w.refreshLooperLanes();
    };
    w.beatGrid_->onBeatDivisionChanged = [&w](int section, int beat, int division, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.division")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("division"), division},
        });
        w.refreshLooperLanes();
    };
    w.beatGrid_->onStructureChanged = [&w] {
        if (w.chordGrid_) w.chordGrid_->refresh();
        if (w.lyricGrid_) w.lyricGrid_->refresh();
        w.refreshLooperLanes();
        w.sendSongSnapshot();
    };
    w.lyricGrid_->onStructureChanged = [&w] {
        if (w.chordGrid_) w.chordGrid_->refresh();
        if (w.beatGrid_) w.beatGrid_->refresh();
        w.sendSongSnapshot();
    };
    w.beatGrid_->onGridResized = [&w](int section, int beats, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("beat")},
            {QStringLiteral("beats"), beats},
        });
        if (w.chordGrid_) w.chordGrid_->refresh();
        if (w.lyricGrid_) w.lyricGrid_->refresh();
        w.refreshLooperLanes();
        w.regeneratePreparedMix(section);
        w.syncLooperArrangement();
    };
    w.lyricGrid_->onGridResized = w.beatGrid_->onGridResized;
    w.beatGrid_->onShrinkRequested = [&w](int section) {
        w.shrinkSectionOneBar(section);
    };
    w.lyricGrid_->onShrinkRequested = w.beatGrid_->onShrinkRequested;

    w.diagnosisLabel_ = new QLabel(QStringLiteral("Diagnosis -"), &w);
    w.diagnosisLabel_->setObjectName(QStringLiteral("StatusPill"));
    w.diagnosisLabel_->setMinimumWidth(260);
    w.diagnosisLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    w.diagnosisEvidenceLabel_ = new QLabel(
        QStringLiteral("Live diagnosis waits for connection diagnostics."), &w);
    w.diagnosisEvidenceLabel_->setWordWrap(true);
    w.diagnosisEvidenceLabel_->setObjectName(QStringLiteral("DiagnosisDetail"));
    w.performanceHome_ = new PerformanceHomeWidget(&w);
    w.performanceHome_->setSongModel(&w.chordModel_);
    w.performanceHome_->onOpenDetail = [&w](const QString& page) {
        w.openWorkspace(page);
    };
    w.performanceHome_->onPeerSelected = [&w](std::uint64_t peerId) {
        w.selectPerformancePeer(peerId);
    };
    w.performanceHome_->setTrackGainDb(w.trackController_.model().trackGainDb);
    w.performanceHome_->onTrackGainChanged = [&w](double gainDb) {
        w.trackController_.model().trackGainDb = gainDb;
        if (w.jam2_.isRunning()) w.sendPreparedTrackLevel();
    };
    w.performanceHome_->onGenerateIdea = [&w] {
        w.generatePracticeIdea();
    };
    w.performanceHome_->onBrowseIdeas = [&w] {
        w.browseCuratedIdeas();
    };
    w.performanceHome_->onContinueIdea = [&w] {
        w.continuePracticeIdea();
    };
    w.performanceHome_->onClearIdea = [&w] {
        w.clearPracticeIdea();
    };
    w.performanceHome_->onGenerateWav = [&w] {
        w.generatePracticeReferenceWavs();
    };
    w.performanceHome_->onJamTaster = [&w] {
        w.showJamTasterDialog();
    };
    w.performanceHome_->onTunerEnabledChanged = [&w](bool enabled) {
        w.setTunerEnabled(enabled);
    };
    w.performanceHome_->onJamRecordingToggle = [&w] {
        if (w.trackRecordingWorkflow_.jamRecordingActive()) w.stopJamRecording();
        else w.startJamRecording();
    };
    w.performanceHome_->onBankLaunch = [&w](int bank) {
        w.requestBankLaunch(bank);
    };
    w.performanceHome_->onAddSection = [&w] {
        w.addSongSection();
    };
    w.performanceHome_->onRemoveSection = [&w] {
        w.removeLastSongSection();
    };
    w.performanceHome_->onManageArrangement = [&w] {
        w.showArrangementDialog();
    };

    w.workspaceStack_ = new QStackedWidget(&w);
    const auto addWorkspace = [&w](const QString& key, QWidget* page) {
        w.workspacePages_.insert(key, w.workspaceStack_->addWidget(page));
    };
    addWorkspace(QStringLiteral("chords"), chordPage);
    addWorkspace(QStringLiteral("beats"), beatPage);
    addWorkspace(QStringLiteral("lyrics"), lyricPage);
    addWorkspace(QStringLiteral("metronome"), metronomePage);
    addWorkspace(QStringLiteral("looper"), trackPage);
    sessionPage->hide();

    auto* detailPanel = new QFrame(&w);
    detailPanel->setObjectName(QStringLiteral("DetailPanel"));
    auto* detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(12, 10, 12, 12);
    detailLayout->setSpacing(8);
    auto* detailHeader = new QHBoxLayout();
    w.detailIdentityPanel_ = new QWidget(detailPanel);
    w.detailIdentityPanel_->setObjectName(QStringLiteral("DetailIdentityPanel"));
    auto* detailIdentity = new QVBoxLayout(w.detailIdentityPanel_);
    detailIdentity->setContentsMargins(0, 0, 0, 0);
    detailIdentity->setSpacing(0);
    auto* detailSectionEdit = new jam2::gui::DetailSectionEdit(w.detailIdentityPanel_);
    w.detailPositionLabel_ = detailSectionEdit;
    w.detailPositionLabel_->setText(QStringLiteral("Section A"));
    w.detailPositionLabel_->setCursorPosition(0);
    w.detailPositionLabel_->setObjectName(QStringLiteral("DetailPosition"));
    jam2::gui::registerGuiControl(
        *w.detailPositionLabel_, QStringLiteral("song.section-name"),
        QStringLiteral("song.section-rename"),
        jam2::gui::GuiControlAvailability::StateGated);
    detailSectionEdit->onCommitted = [&w](const QString& name) {
        const int bank = w.viewedBankIndex_;
        if (bank < 0 || bank >= w.chordModel_.sections().size()) return;
        w.chordModel_.renameSection(bank, w.chordModel_.section(bank).label, name);
        if (w.chordGrid_) w.chordGrid_->refresh();
        if (w.beatGrid_) w.beatGrid_->refresh();
        if (w.lyricGrid_) w.lyricGrid_->refresh();
        w.refreshLooperLanes();
        w.sendSongSnapshot();
    };
    detailIdentity->addWidget(w.detailPositionLabel_);
    detailHeader->addWidget(w.detailIdentityPanel_);
    w.detailIdentityPanel_->setVisible(false);
    detailHeader->addStretch(1);
    const QList<QPair<QString, QString>> destinations{
        {QStringLiteral("Chords"), QStringLiteral("chords")},
        {QStringLiteral("Beats"), QStringLiteral("beats")},
        {QStringLiteral("Lyrics"), QStringLiteral("lyrics")},
        {QStringLiteral("Metronome"), QStringLiteral("metronome")},
        {QStringLiteral("Track"), QStringLiteral("looper")},
    };
    for (const auto& destination : destinations) {
        auto* button = new QPushButton(destination.first, detailPanel);
        button->setObjectName(QStringLiteral("DetailTab"));
        button->setProperty("workspaceKey", destination.second);
        jam2::gui::registerGuiControl(
            *button, QStringLiteral("workspace.open.%1").arg(destination.second),
            QStringLiteral("workspace.navigation"),
            jam2::gui::GuiControlAvailability::StateGated,
            QStringLiteral("workspace.open"));
        QObject::connect(button, &QPushButton::clicked, &w, [&w, key = destination.second] {
            w.openWorkspace(key);
        });
        detailHeader->addWidget(button);
    }
    auto* closeDetailButton = new QPushButton(QStringLiteral("Close"), detailPanel);
    closeDetailButton->setObjectName(QStringLiteral("CloseDetailButton"));
    jam2::gui::registerGuiControl(
        *closeDetailButton, QStringLiteral("workspace.close"),
        QStringLiteral("workspace.navigation"),
        jam2::gui::GuiControlAvailability::StateGated);
    QObject::connect(closeDetailButton, &QPushButton::clicked, &w, [&w] {
        w.openWorkspace(QStringLiteral("performance"));
    });
    detailHeader->addWidget(closeDetailButton);
    detailLayout->addLayout(detailHeader);
    detailLayout->addWidget(w.workspaceStack_, 1);

    w.performanceStageStack_ = new QStackedWidget(&w);
    w.performanceStageStack_->addWidget(w.performanceHome_);
    w.performanceStageStack_->addWidget(detailPanel);
    w.performanceStageStack_->setCurrentIndex(0);

    auto* sessionActions = new QHBoxLayout();
    sessionActions->setSpacing(6);
    auto* saveSongButton = new QPushButton(QStringLiteral("Save"), &w);
    auto* openSongButton = new QPushButton(QStringLiteral("Open"), &w);
    auto* newSongButton = new QPushButton(QStringLiteral("New"), &w);
    jam2::gui::registerGuiControl(
        *saveSongButton, QStringLiteral("session.save"),
        QStringLiteral("session.persistence"),
        jam2::gui::GuiControlAvailability::FileDialog);
    jam2::gui::registerGuiControl(
        *openSongButton, QStringLiteral("session.open"),
        QStringLiteral("session.persistence"),
        jam2::gui::GuiControlAvailability::FileDialog);
    jam2::gui::registerGuiControl(
        *newSongButton, QStringLiteral("session.new"),
        QStringLiteral("session.persistence"),
        jam2::gui::GuiControlAvailability::Modal);
    for (QPushButton* button : {saveSongButton, openSongButton, newSongButton}) {
        button->setObjectName(QStringLiteral("SessionAction"));
        sessionActions->addWidget(button);
    }
    QObject::connect(saveSongButton, &QPushButton::clicked, &w, [&w] { w.saveSong(); });
    QObject::connect(openSongButton, &QPushButton::clicked, &w, [&w] { w.openSong(); });
    QObject::connect(newSongButton, &QPushButton::clicked, &w, [&w] { w.newSong(); });
    auto* dataButton = new QPushButton(QStringLiteral("Data"), &w);
    dataButton->setObjectName(QStringLiteral("DataButton"));
    jam2::gui::registerGuiControl(
        *dataButton, QStringLiteral("application.data"),
        QStringLiteral("application.data-drawer"));
    QObject::connect(dataButton, &QPushButton::clicked, &w, [&w] {
        w.toggleDataDrawer();
    });
    sessionActions->addWidget(dataButton);
    header->addLayout(sessionActions);

    w.dataOverlay_ = new QWidget(&w);
    w.dataOverlay_->setObjectName(QStringLiteral("DataOverlay"));
    auto* overlayLayout = new QHBoxLayout(w.dataOverlay_);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    overlayLayout->addStretch(1);
    w.dataDrawer_ = new QFrame(w.dataOverlay_);
    w.dataDrawer_->setObjectName(QStringLiteral("DataDrawer"));
    w.dataDrawer_->setFixedWidth(580);
    auto* dataContent = new QWidget(w.dataDrawer_);
    auto* dataLayout = new QVBoxLayout(dataContent);
    dataLayout->setContentsMargins(18, 16, 18, 20);
    dataLayout->setSpacing(12);
    auto* dataHeader = new QHBoxLayout();
    auto* dataHeading = new QVBoxLayout();
    auto* dataTitle = new QLabel(QStringLiteral("Session data"), dataContent);
    dataTitle->setObjectName(QStringLiteral("DrawerTitle"));
    auto* dataSubtitle = new QLabel(
        QStringLiteral("LIVE AUDIO PATH / RAW MEASUREMENTS"), dataContent);
    dataSubtitle->setObjectName(QStringLiteral("MicroHeading"));
    dataHeading->addWidget(dataTitle);
    dataHeading->addWidget(dataSubtitle);
    dataHeader->addLayout(dataHeading);
    dataHeader->addStretch(1);
    auto* closeData = new QPushButton(QStringLiteral("Close"), dataContent);
    closeData->setObjectName(QStringLiteral("CloseDetailButton"));
    jam2::gui::registerGuiControl(
        *closeData, QStringLiteral("application.data.close"),
        QStringLiteral("application.data-drawer"),
        jam2::gui::GuiControlAvailability::StateGated);
    QObject::connect(closeData, &QPushButton::clicked, &w, [&w] {
        w.toggleDataDrawer();
    });
    dataHeader->addWidget(closeData);
    dataLayout->addLayout(dataHeader);

    auto* audioPathTitle = new QLabel(QStringLiteral("AUDIO PATH"), dataContent);
    audioPathTitle->setObjectName(QStringLiteral("DrawerSection"));
    dataLayout->addWidget(audioPathTitle);
    auto* metricGrid = new QGridLayout();
    metricGrid->setSpacing(7);
    const auto metricCard = [dataContent](
                                const QString& label,
                                QLabel*& value,
                                const QString& initial) {
        auto* card = new QFrame(dataContent);
        card->setObjectName(QStringLiteral("MetricCard"));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 7, 10, 8);
        layout->setSpacing(1);
        value = new QLabel(initial, card);
        value->setObjectName(QStringLiteral("MetricValue"));
        auto* caption = new QLabel(label, card);
        caption->setObjectName(QStringLiteral("MetricCaption"));
        layout->addWidget(value);
        layout->addWidget(caption);
        return card;
    };
    metricGrid->addWidget(metricCard(
        QStringLiteral("SAMPLE RATE"), w.diagnosticSampleRateValue_, QStringLiteral("-")), 0, 0);
    metricGrid->addWidget(metricCard(
        QStringLiteral("DRIFT"), w.diagnosticDriftValue_, QStringLiteral("-")), 0, 1);
    metricGrid->addWidget(metricCard(
        QStringLiteral("MISSING AUDIO"), w.diagnosticMissingAudioValue_, QStringLiteral("0 fr")), 0, 2);
    metricGrid->addWidget(metricCard(
        QStringLiteral("OUTPUT UNDERRUNS"), w.diagnosticOutputUnderrunsValue_, QStringLiteral("0")), 0, 3);
    metricGrid->addWidget(metricCard(
        QStringLiteral("PACKETS"), w.diagnosticPacketsValue_, QStringLiteral("0")), 1, 0);
    metricGrid->addWidget(metricCard(
        QStringLiteral("LATE / REORDERED"), w.diagnosticLateValue_, QStringLiteral("0")), 1, 1);
    metricGrid->addWidget(metricCard(
        QStringLiteral("LOSS EVENTS"), w.diagnosticLossEventsValue_, QStringLiteral("0")), 1, 2);
    metricGrid->addWidget(metricCard(
        QStringLiteral("PACKET BURSTS"), w.diagnosticBurstGapsValue_, QStringLiteral("0")), 1, 3);
    dataLayout->addLayout(metricGrid);
    dataLayout->addWidget(w.diagnosisLabel_);
    dataLayout->addWidget(w.diagnosisEvidenceLabel_);

    auto* peerTitle = new QLabel(QStringLiteral("DIRECT PEERS"), dataContent);
    peerTitle->setObjectName(QStringLiteral("DrawerSection"));
    dataLayout->addWidget(peerTitle);
    w.diagnosticPeerTable_ = new QTableWidget(0, 6, dataContent);
    w.diagnosticPeerTable_->setHorizontalHeaderLabels({
        QStringLiteral("Peer"),
        QStringLiteral("RTT"),
        QStringLiteral("Jitter"),
        QStringLiteral("Loss"),
        QStringLiteral("Late"),
        QStringLiteral("Drift")});
    w.diagnosticPeerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    w.diagnosticPeerTable_->setSelectionMode(QAbstractItemView::NoSelection);
    w.diagnosticPeerTable_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    w.diagnosticPeerTable_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    w.diagnosticPeerTable_->verticalHeader()->hide();
    w.diagnosticPeerTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    w.diagnosticPeerTable_->setMinimumHeight(140);
    w.diagnosticPeerTable_->setMaximumHeight(280);
    jam2::gui::excludeFromGuiControlInventory(
        *w.diagnosticPeerTable_,
        QStringLiteral("read-only diagnostic view with selection and editing disabled"));
    dataLayout->addWidget(w.diagnosticPeerTable_);

    auto* guideTitle = new QLabel(QStringLiteral("IF YOU HEAR THIS ARTIFACT"), dataContent);
    guideTitle->setObjectName(QStringLiteral("DrawerSection"));
    dataLayout->addWidget(guideTitle);
    auto addGuide = [dataContent, dataLayout, guideIndex = 0](
                              const QString& title,
                              const QString& body) mutable {
        auto* container = new QFrame(dataContent);
        container->setObjectName(QStringLiteral("GuideSection"));
        auto* sectionLayout = new QVBoxLayout(container);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(0);
        auto* toggle = new QToolButton(container);
        toggle->setObjectName(QStringLiteral("GuideToggle"));
        toggle->setText(title);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::RightArrow);
        toggle->setCheckable(true);
        jam2::gui::registerGuiControl(
            *toggle, QStringLiteral("application.data.guide.%1").arg(guideIndex++),
            QStringLiteral("application.data-guide"),
            jam2::gui::GuiControlAvailability::StateGated,
            QStringLiteral("application.data.guide"));
        auto* copy = new QLabel(body, container);
        copy->setObjectName(QStringLiteral("ArtifactGuide"));
        copy->setWordWrap(true);
        copy->setContentsMargins(13, 9, 13, 11);
        copy->hide();
        QObject::connect(toggle, &QToolButton::toggled, container, [toggle, copy](bool open) {
            toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            copy->setVisible(open);
        });
        sectionLayout->addWidget(toggle);
        sectionLayout->addWidget(copy);
        dataLayout->addWidget(container);
    };
    addGuide(
        QStringLiteral("Clicks, pops or drop-outs"),
        QStringLiteral(
            "Watch callback gaps, output underruns, packet loss and late packets. "
            "If callback gaps rise, increase the audio-device buffer. If network "
            "loss or late packets rise, test a wired path or cautiously increase prefill."));
    addGuide(
        QStringLiteral("Pitch wobble or drifting"),
        QStringLiteral(
            "Watch drift ppm, resampler ratio and drift-clamped samples. Change "
            "drift limits only when those measurements show sustained clock pressure."));
    addGuide(
        QStringLiteral("Stable audio, but too delayed"),
        QStringLiteral(
            "Compare peer RTT with the configured playout and device-buffer values "
            "in Settings or the CSV log. Physical network RTT cannot be removed by "
            "reducing a local buffer."));
    addGuide(
        QStringLiteral("One person is loud, quiet or distorted"),
        QStringLiteral(
            "Select that person in the performance rail and change their dB gain. "
            "Use input and output peak meters to separate source clipping from mix level."));
    addGuide(
        QStringLiteral("Rhythmic pulsing or repeated gaps"),
        QStringLiteral(
            "Compare packet-gap bursts with callback cadence and buffer depth. A "
            "regular callback issue points to the device path; network bursts point "
            "to the connection path."));
    auto* logTitle = new QToolButton(dataContent);
    logTitle->setObjectName(QStringLiteral("GuideToggle"));
    logTitle->setText(QStringLiteral("Technical log"));
    logTitle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    logTitle->setArrowType(Qt::RightArrow);
    logTitle->setCheckable(true);
    jam2::gui::registerGuiControl(
        *logTitle, QStringLiteral("application.data.log"),
        QStringLiteral("application.technical-log"),
        jam2::gui::GuiControlAvailability::StateGated);
    dataLayout->addWidget(logTitle);
    w.logEdit_ = new QPlainTextEdit(dataContent);
    w.logEdit_->setReadOnly(true);
    w.logEdit_->setMaximumBlockCount(2000);
    w.logEdit_->setMinimumHeight(220);
    w.logEdit_->hide();
    QObject::connect(logTitle, &QToolButton::toggled, dataContent, [logTitle, &w](bool open) {
        logTitle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        w.logEdit_->setVisible(open);
    });
    dataLayout->addWidget(w.logEdit_, 1);
    auto* dataScroll = new QScrollArea(w.dataDrawer_);
    dataScroll->setWidgetResizable(true);
    dataScroll->setFrameShape(QFrame::NoFrame);
    dataScroll->setWidget(dataContent);
    auto* drawerLayout = new QVBoxLayout(w.dataDrawer_);
    drawerLayout->setContentsMargins(0, 0, 0, 0);
    drawerLayout->addWidget(dataScroll);
    overlayLayout->addWidget(w.dataDrawer_);
    w.dataOverlay_->hide();

    auto* central = new QWidget(&w);
    auto* centralLayout = new QStackedLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setStackingMode(QStackedLayout::StackAll);
    centralLayout->addWidget(w.performanceStageStack_);
    centralLayout->addWidget(w.dataOverlay_);

    auto* transport = new QFrame(&w);
    transport->setObjectName(QStringLiteral("PerformanceTransport"));
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(12, 8, 12, 8);
    transportLayout->setSpacing(18);

    auto* localStrip = new QWidget(transport);
    auto* localLayout = new QVBoxLayout(localStrip);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(4);
    auto* localHeader = new QWidget(localStrip);
    auto* localHeaderLayout = new QHBoxLayout(localHeader);
    localHeaderLayout->setContentsMargins(0, 0, 0, 0);
    localHeaderLayout->setSpacing(5);
    w.performanceLeftTitle_ = new QLabel(QStringLiteral("YOU / LOCAL INPUT"), localHeader);
    w.performanceLeftTitle_->setObjectName(QStringLiteral("StripTitle"));
    localHeaderLayout->addWidget(w.performanceLeftTitle_);
    localHeaderLayout->addStretch(1);
    w.performanceAudioInputsButton_ = new QPushButton(
        QStringLiteral("AUDIO"), localHeader);
    w.performanceAudioInputsButton_->setObjectName(QStringLiteral("LocalAudioTag"));
    jam2::gui::registerGuiControl(
        *w.performanceAudioInputsButton_, QStringLiteral("performance.audio-inputs"),
        QStringLiteral("performance.audio-input-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    w.performanceMidiInputsButton_ = new QPushButton(
        QStringLiteral("MIDI"), localHeader);
    w.performanceMidiInputsButton_->setObjectName(QStringLiteral("LocalMidiTag"));
    jam2::gui::registerGuiControl(
        *w.performanceMidiInputsButton_, QStringLiteral("performance.midi-inputs"),
        QStringLiteral("performance.midi-input-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    w.performancePluginsButton_ = new QPushButton(
        QStringLiteral("PLUGINS"), localHeader);
    w.performancePluginsButton_->setObjectName(QStringLiteral("LocalPluginsTag"));
    jam2::gui::registerGuiControl(
        *w.performancePluginsButton_, QStringLiteral("performance.plugins"),
        QStringLiteral("performance.plugin-dialog"),
        jam2::gui::GuiControlAvailability::Modal);
    w.performancePluginBypassButton_ = new QPushButton(
        QStringLiteral("BYPASS"), localHeader);
    w.performancePluginBypassButton_->setObjectName(QStringLiteral("LocalBypassTag"));
    jam2::gui::registerGuiControl(
        *w.performancePluginBypassButton_, QStringLiteral("performance.plugin-bypass"),
        QStringLiteral("performance.plugin-bypass"),
        jam2::gui::GuiControlAvailability::StateGated);
    w.performancePluginBypassButton_->setCheckable(true);
    localHeaderLayout->addWidget(w.performanceAudioInputsButton_);
    localHeaderLayout->addWidget(w.performanceMidiInputsButton_);
    localHeaderLayout->addWidget(w.performancePluginsButton_);
    localHeaderLayout->addWidget(w.performancePluginBypassButton_);
    QObject::connect(w.performanceAudioInputsButton_, &QPushButton::clicked,
        &w, [&w] { w.showAudioInputSources(); });
    QObject::connect(w.performanceMidiInputsButton_, &QPushButton::clicked,
        &w, [&w] { w.showMidiInputSources(); });
    QObject::connect(w.performancePluginsButton_, &QPushButton::clicked,
        &w, [&w] { w.showInputPlugins(); });
    QObject::connect(w.performancePluginBypassButton_, &QPushButton::toggled,
        &w, [&w](bool bypassed) {
            for (auto& source : w.audioPluginSources_) {
                if (source.host) {
                    source.bypassed = bypassed;
                    source.host->setAudioBypassed(bypassed);
                }
            }
            for (auto& source : w.midiPluginSources_) {
                if (!source || !source->host) continue;
                source->muted = bypassed;
                if (bypassed) source->host->requestMidiReset();
                source->host->setMidiMuted(bypassed);
            }
            w.updateInputSourceButtons();
        });
    localLayout->addWidget(localHeader);
    w.performanceLocalControls_ = new QWidget(localStrip);
    auto* selfControlsLayout = new QVBoxLayout(w.performanceLocalControls_);
    selfControlsLayout->setContentsMargins(0, 0, 0, 0);
    selfControlsLayout->setSpacing(4);
    selfControlsLayout->addWidget(w.mixInputMeterRow_);
    selfControlsLayout->addWidget(w.mixSendRow_);
    auto* monitorRow = new QHBoxLayout();
    monitorRow->addWidget(w.mixMonitorEnableRow_);
    monitorRow->addWidget(w.mixMonitorRow_, 1);
    selfControlsLayout->addLayout(monitorRow);
    localLayout->addWidget(w.performanceLocalControls_);

    w.performancePeerControls_ = new QWidget(localStrip);
    auto* peerControlsLayout = new QVBoxLayout(w.performancePeerControls_);
    peerControlsLayout->setContentsMargins(0, 0, 0, 0);
    peerControlsLayout->setSpacing(5);
    auto* aggregateRow = new QHBoxLayout();
    aggregateRow->addWidget(new QLabel(QStringLiteral("Peer activity"), w.performancePeerControls_));
    aggregateRow->addWidget(w.mixRemotePeerMeter_, 1);
    peerControlsLayout->addLayout(aggregateRow);
    auto* selectedRow = new QHBoxLayout();
    w.selectedPeerNameLabel_ = new QLabel(QStringLiteral("Volume"), w.performancePeerControls_);
    w.selectedPeerNameLabel_->setMinimumWidth(72);
    w.selectedPeerGainSlider_ = new QSlider(Qt::Horizontal, w.performancePeerControls_);
    w.selectedPeerGainSlider_->setRange(-60, 12);
    w.selectedPeerGainSlider_->setValue(0);
    w.selectedPeerGainSlider_->setEnabled(false);
    w.selectedPeerGainSlider_->setMinimumWidth(240);
    applyJamSliderStyle(w.selectedPeerGainSlider_);
    jam2::gui::registerGuiControl(
        *w.selectedPeerGainSlider_, QStringLiteral("performance.peer-gain"),
        QStringLiteral("performance.peer-mix"),
        jam2::gui::GuiControlAvailability::StateGated);
    w.selectedPeerGainLabel_ = new QLabel(QStringLiteral("+0.0 dB"), w.performancePeerControls_);
    QObject::connect(w.selectedPeerGainSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        w.applySelectedPeerGain(value);
    });
    selectedRow->addWidget(w.selectedPeerNameLabel_);
    selectedRow->addWidget(w.selectedPeerGainSlider_, 1);
    selectedRow->addWidget(w.selectedPeerGainLabel_);
    peerControlsLayout->addLayout(selectedRow);
    localLayout->addWidget(w.performancePeerControls_);
    transportLayout->addWidget(localStrip, 1);

    auto* playback = new QWidget(transport);
    auto* playbackLayout = new QHBoxLayout(playback);
    playbackLayout->setContentsMargins(0, 0, 0, 0);
    playbackLayout->setSpacing(8);
    w.performanceTrackToggle_ = new QPushButton(QStringLiteral("▶"), playback);
    w.performanceTrackToggle_->setObjectName(QStringLiteral("MainTransportButton"));
    jam2::gui::registerGuiControl(
        *w.performanceTrackToggle_, QStringLiteral("performance.track-toggle"),
        QStringLiteral("performance.global-transport"),
        jam2::gui::GuiControlAvailability::StateGated);
    w.performanceTrackToggle_->setFixedSize(64, 64);
    QObject::connect(w.performanceTrackToggle_, &QPushButton::clicked, &w, [&w] {
        w.appendLog(QStringLiteral(
            "global transport button clicked: requested=%1 playing=%2 engine_running=%3 count_in=%4")
            .arg(w.trackRecordingWorkflow_.globalTransportRequestedPlaying())
            .arg(w.trackRecordingWorkflow_.globalTransportPlaying())
            .arg(w.jam2_.isRunning())
            .arg(w.performanceCountInCheck_ &&
                w.performanceCountInCheck_->isChecked()));
        if (w.trackRecordingWorkflow_.globalTransportRequestedPlaying()) {
            w.runGridLockedEngineAction(
                QStringLiteral("track.stop"),
                [&w](std::uint64_t targetFrame) { w.stopTrack(targetFrame); });
        } else {
            w.playTrack();
        }
    });
    playbackLayout->addWidget(w.performanceTrackToggle_);

    auto* tempoCard = new QFrame(playback);
    tempoCard->setObjectName(QStringLiteral("TempoCard"));
    auto* tempoLayout = new QVBoxLayout(tempoCard);
    tempoLayout->setContentsMargins(10, 5, 10, 5);
    tempoLayout->setSpacing(2);
    auto* tempoTop = new QHBoxLayout();
    tempoTop->setContentsMargins(0, 0, 0, 0);
    tempoTop->setSpacing(6);
    w.performanceMetronomeToggle_ = new QPushButton(QStringLiteral("METRONOME ON"), tempoCard);
    w.performanceMetronomeToggle_->setProperty("active", true);
    w.performanceMetronomeToggle_->setObjectName(QStringLiteral("MetronomeToggle"));
    jam2::gui::registerGuiControl(
        *w.performanceMetronomeToggle_, QStringLiteral("performance.metronome-toggle"),
        QStringLiteral("performance.metronome-audibility"),
        jam2::gui::GuiControlAvailability::StateGated);
    w.performanceMetronomeToggle_->setToolTip(QStringLiteral(
        "Choose whether the click is heard while global playback is running"));
    QObject::connect(w.performanceMetronomeToggle_, &QPushButton::clicked, &w, [&w] {
        if (w.metronomeTransport_.localRunning()) {
            w.stopTrackMetronome();
        } else {
            w.startTrackMetronome();
        }
    });
    tempoTop->addWidget(w.performanceMetronomeToggle_);
    w.performanceTempoButton_ = new QPushButton(QStringLiteral("120 BPM"), tempoCard);
    w.performanceTempoButton_->setObjectName(QStringLiteral("TempoButton"));
    jam2::gui::registerGuiControl(
        *w.performanceTempoButton_, QStringLiteral("performance.tempo"),
        QStringLiteral("workspace.navigation"));
    QObject::connect(w.performanceTempoButton_, &QPushButton::clicked, &w, [&w] {
        w.openWorkspace(QStringLiteral("metronome"));
    });
    tempoTop->addWidget(w.performanceTempoButton_);
    tempoLayout->addLayout(tempoTop);
    auto* clickLevel = new QHBoxLayout();
    clickLevel->setContentsMargins(4, 0, 4, 0);
    clickLevel->setSpacing(6);
    auto* clickLevelLabel = new QLabel(QStringLiteral("CLICK"), tempoCard);
    clickLevelLabel->setObjectName(QStringLiteral("StripTitle"));
    clickLevelLabel->setFixedWidth(38);
    clickLevel->addWidget(clickLevelLabel);
    w.metronomeLevelSlider_->setMinimumWidth(120);
    clickLevel->addWidget(w.metronomeLevelSlider_, 1);
    clickLevel->addWidget(w.mixMetronomeLevelLabel_);
    tempoLayout->addLayout(clickLevel);
    auto* countInRow = new QHBoxLayout();
    countInRow->setContentsMargins(4, 0, 4, 0);
    countInRow->setSpacing(6);
    countInRow->addSpacing(44);
    w.performanceCountInCheck_ = new QCheckBox(QStringLiteral("Count-In"), tempoCard);
    w.performanceCountInCheck_->setObjectName(QStringLiteral("PlaybackCountIn"));
    jam2::gui::registerGuiControl(
        *w.performanceCountInCheck_, QStringLiteral("performance.count-in"),
        QStringLiteral("performance.count-in"));
    w.performanceCountInCheck_->setToolTip(QStringLiteral(
        "Play one bar of the recording count-in click before global playback starts"));
    countInRow->addWidget(w.performanceCountInCheck_);
    countInRow->addStretch(1);
    tempoLayout->addLayout(countInRow);
    playbackLayout->addWidget(tempoCard);
    w.performancePositionLabel_ = new QLabel(
        QStringLiteral("1.01\nBAR / BEAT"), playback);
    w.performancePositionLabel_->setObjectName(QStringLiteral("PerformancePosition"));
    playbackLayout->addWidget(w.performancePositionLabel_);
    transportLayout->addWidget(playback);

    auto* remoteStrip = new QWidget(transport);
    auto* remoteLayout = new QVBoxLayout(remoteStrip);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    w.performanceRightTitle_ =
        new QLabel(QStringLiteral("MASTER OUTPUT"), remoteStrip);
    w.performanceRightTitle_->setObjectName(QStringLiteral("StripTitle"));
    remoteLayout->addWidget(w.performanceRightTitle_);
    w.performanceMasterOutputControls_ = new QWidget(remoteStrip);
    auto* outputControlsLayout =
        new QVBoxLayout(w.performanceMasterOutputControls_);
    outputControlsLayout->setContentsMargins(0, 0, 0, 0);
    outputControlsLayout->setSpacing(5);
    outputControlsLayout->addWidget(w.mixOutputMeterRow_);
    auto* outputLevelRow = new QHBoxLayout();
    outputLevelRow->addWidget(new QLabel(QStringLiteral("Output"), remoteStrip));
    outputLevelRow->addWidget(w.masterOutputLevelSlider_, 1);
    outputLevelRow->addWidget(w.masterOutputLevelLabel_);
    outputControlsLayout->addLayout(outputLevelRow);
    remoteLayout->addWidget(w.performanceMasterOutputControls_);
    transportLayout->addWidget(remoteStrip, 1);
    transport->setMaximumHeight(164);

    if (w.playTrackButton_) w.playTrackButton_->hide();
    if (w.stopTrackButton_) w.stopTrackButton_->hide();

    auto* layout = new QVBoxLayout(&w);
    layout->setContentsMargins(12, 8, 12, 10);
    layout->setSpacing(7);
    layout->addLayout(header);
    layout->addWidget(central, 1);
    layout->addWidget(transport);
    w.updateMixControls();
    w.openWorkspace(QStringLiteral("performance"));

    QTimer::singleShot(0, &w, [&w] {
        w.refreshDevices();
        w.refreshLoopbackSources();
        if (!w.jam2_.isRunning()) {
            w.showLocalPerformSetup();
        }
    });
}

QWidget* MainWindowPages::buildSessionPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    w.bpmSpin_ = new QSpinBox(page);
    w.bpmSpin_->setRange(1, 400);
    w.bpmSpin_->setValue(120);
    w.bpmSpin_->hide();
    jam2::gui::excludeFromGuiControlInventory(
        *w.bpmSpin_, QStringLiteral("hidden mirror of the visible metronome BPM control"));
    w.startButton_ = new QPushButton(QStringLiteral("Start Jam"), page);
    w.joinButton_ = new QPushButton(QStringLiteral("Join Jam"), page);
    w.stopButton_ = new QPushButton(QStringLiteral("End Jam"), page);
    w.refreshControlButton_ = new QPushButton(QStringLiteral("Refresh Control"), page);
    jam2::gui::excludeFromGuiControlInventory(
        *w.startButton_, QStringLiteral("hidden duplicate of the public Start Jam control"));
    jam2::gui::excludeFromGuiControlInventory(
        *w.joinButton_, QStringLiteral("hidden duplicate of the public Join Jam control"));
    jam2::gui::excludeFromGuiControlInventory(
        *w.stopButton_, QStringLiteral("hidden state mirror for the public Leave Jam control"));
    jam2::gui::excludeFromGuiControlInventory(
        *w.refreshControlButton_,
        QStringLiteral("hidden state control; refresh is exposed in active session dialogs"));
    w.stopButton_->setEnabled(false);
    w.refreshControlButton_->setEnabled(false);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(w.startButton_);
    buttons->addWidget(w.joinButton_);
    buttons->addWidget(w.stopButton_);
    buttons->addWidget(w.refreshControlButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(buttons);

    QObject::connect(w.startButton_, &QPushButton::clicked, &w, [&w] { w.showStartJamDialog(); });
    QObject::connect(w.joinButton_, &QPushButton::clicked, &w, [&w] { w.showJoinJamDialog(); });
    QObject::connect(w.stopButton_, &QPushButton::clicked, &w, [&w] {
        if (w.sessionController_.snapshot().role == SharedSessionController::Role::Creator) {
            (void)w.sessionController_.endSession();
            QTimer::singleShot(100, &w, [&w] { w.stopJam(); });
        } else {
            w.stopJam();
        }
    });
    QObject::connect(w.refreshControlButton_, &QPushButton::clicked, &w, [&w] { w.refreshControlConnection(); });
    return page;
}

QWidget* MainWindowPages::buildSongPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    w.chordGrid_ = new BeatGridWidget(&w.chordModel_, QStringLiteral("chord"), page);

    auto* generate = new QPushButton(QStringLiteral("GENERATE NEW IDEA"), page);
    auto* browse = new QPushButton(QStringLiteral("GROOVE LIBRARY"), page);
    auto* continueIdea = new QPushButton(QStringLiteral("CONTINUE IDEA"), page);
    auto* reference = new QPushButton(QStringLiteral("GENERATE WAV"), page);
    auto* jamTaster = new QPushButton(QStringLiteral("JAMTASTER"), page);
    auto* details = new QPushButton(QStringLiteral("IDEA DETAILS"), page);
    const auto registerChordAction = [](QObject& control, const char* id, const char* contract) {
        jam2::gui::registerGuiControl(
            control,
            QStringLiteral("chords.idea.") + QString::fromLatin1(id),
            QString::fromLatin1(contract),
            jam2::gui::GuiControlAvailability::Modal,
            QStringLiteral("idea.header-action"));
    };
    registerChordAction(*generate, "generate", "idea.generate");
    registerChordAction(*browse, "browse", "idea.catalog");
    registerChordAction(*continueIdea, "continue", "idea.continue");
    registerChordAction(*reference, "generate-wav", "idea.reference-wav");
    registerChordAction(*jamTaster, "jam-taster", "idea.jam-taster");
    registerChordAction(*details, "details", "idea.details");
    styleIdeaHeaderAction(generate, IdeaHeaderAction::Generate);
    styleIdeaHeaderAction(browse, IdeaHeaderAction::Browse);
    styleIdeaHeaderAction(continueIdea, IdeaHeaderAction::Continue);
    styleIdeaHeaderAction(reference, IdeaHeaderAction::Wav);
    styleIdeaHeaderAction(jamTaster, IdeaHeaderAction::Browse);
    styleIdeaHeaderAction(details, IdeaHeaderAction::Details);
    auto* top = new QHBoxLayout();
    addBankControls(w, page, top, false, "chords");
    top->addSpacing(12);
    top->addWidget(generate);
    top->addWidget(browse);
    top->addWidget(continueIdea);
    top->addWidget(reference);
    top->addWidget(jamTaster);
    top->addWidget(details);
    top->addStretch(1);
    top->addWidget(w.chordGrid_->createOverviewPagination(page));
    QObject::connect(generate, &QPushButton::clicked, &w, [&w] { w.generatePracticeIdea(); });
    QObject::connect(browse, &QPushButton::clicked, &w, [&w] { w.browseCuratedIdeas(); });
    QObject::connect(continueIdea, &QPushButton::clicked, &w, [&w] { w.continuePracticeIdea(); });
    QObject::connect(reference, &QPushButton::clicked, &w, [&w] { w.generatePracticeReferenceWavs(); });
    QObject::connect(jamTaster, &QPushButton::clicked, &w, [&w] { w.showJamTasterDialog(); });
    QObject::connect(details, &QPushButton::clicked, &w, [&w] { w.showPracticeIdeaDetails(); });

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(w.chordGrid_, 1);

    w.chordGrid_->onCellEdited = [&w](int section, const QString& lane, int beat, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
        w.refreshLooperLanes();
    };
    w.chordGrid_->onMusicalDivisionChanged = [&w](int section, int beat, int division, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("music.division")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("division"), division},
        });
        w.refreshLooperLanes();
    };
    w.chordGrid_->onMusicalStepEdited = [&w](
        int section, int beat, int step, const QString& lane, const QString& text, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("music.step")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("step"), step},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("text"), text},
        });
        w.refreshLooperLanes();
    };
    w.chordGrid_->onGridResized = [&w](int section, int beats, int revision) {
        w.sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("chord")},
            {QStringLiteral("beats"), beats},
        });
        if (w.beatGrid_) w.beatGrid_->refresh();
        if (w.lyricGrid_) w.lyricGrid_->refresh();
        w.refreshLooperLanes();
        w.regeneratePreparedMix(section);
        w.syncLooperArrangement();
    };
    w.chordGrid_->onShrinkRequested = [&w](int section) {
        w.shrinkSectionOneBar(section);
    };
    w.chordGrid_->onStructureChanged = [&w] {
        if (w.beatGrid_) w.beatGrid_->refresh();
        if (w.lyricGrid_) w.lyricGrid_->refresh();
        w.refreshLooperLanes();
        w.sendSongSnapshot();
    };

    return page;
}

QWidget* MainWindowPages::buildBeatPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    w.beatGrid_ = new BeatGridWidget(&w.beatModel_, QStringLiteral("beat"), page);
    auto* generate = new QPushButton(QStringLiteral("GENERATE NEW IDEA"), page);
    auto* browse = new QPushButton(QStringLiteral("GROOVE LIBRARY"), page);
    auto* continueIdea = new QPushButton(QStringLiteral("CONTINUE IDEA"), page);
    auto* reference = new QPushButton(QStringLiteral("GENERATE WAV"), page);
    auto* jamTaster = new QPushButton(QStringLiteral("JAMTASTER"), page);
    auto* details = new QPushButton(QStringLiteral("IDEA DETAILS"), page);
    const auto registerBeatAction = [](QObject& control, const char* id, const char* contract) {
        jam2::gui::registerGuiControl(
            control,
            QStringLiteral("beats.idea.") + QString::fromLatin1(id),
            QString::fromLatin1(contract),
            jam2::gui::GuiControlAvailability::Modal,
            QStringLiteral("idea.header-action"));
    };
    registerBeatAction(*generate, "generate", "idea.generate");
    registerBeatAction(*browse, "browse", "idea.catalog");
    registerBeatAction(*continueIdea, "continue", "idea.continue");
    registerBeatAction(*reference, "generate-wav", "idea.reference-wav");
    registerBeatAction(*jamTaster, "jam-taster", "idea.jam-taster");
    registerBeatAction(*details, "details", "idea.details");
    styleIdeaHeaderAction(generate, IdeaHeaderAction::Generate);
    styleIdeaHeaderAction(browse, IdeaHeaderAction::Browse);
    styleIdeaHeaderAction(continueIdea, IdeaHeaderAction::Continue);
    styleIdeaHeaderAction(reference, IdeaHeaderAction::Wav);
    styleIdeaHeaderAction(jamTaster, IdeaHeaderAction::Browse);
    styleIdeaHeaderAction(details, IdeaHeaderAction::Details);
    auto* top = new QHBoxLayout();
    addBankControls(w, page, top, false, "beats");
    top->addSpacing(12);
    top->addWidget(generate);
    top->addWidget(browse);
    top->addWidget(continueIdea);
    top->addWidget(reference);
    top->addWidget(jamTaster);
    top->addWidget(details);
    top->addStretch(1);
    top->addWidget(w.beatGrid_->createOverviewPagination(page));
    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(w.beatGrid_, 1);
    QObject::connect(generate, &QPushButton::clicked, &w, [&w] { w.generatePracticeIdea(); });
    QObject::connect(browse, &QPushButton::clicked, &w, [&w] { w.browseCuratedIdeas(); });
    QObject::connect(continueIdea, &QPushButton::clicked, &w, [&w] { w.continuePracticeIdea(); });
    QObject::connect(reference, &QPushButton::clicked, &w, [&w] { w.generatePracticeReferenceWavs(); });
    QObject::connect(jamTaster, &QPushButton::clicked, &w, [&w] { w.showJamTasterDialog(); });
    QObject::connect(details, &QPushButton::clicked, &w, [&w] { w.showPracticeIdeaDetails(); });
    return page;
}

QWidget* MainWindowPages::buildTrackPage(MainWindow& w)
{
    auto* page = new QWidget(&w);
    const auto registerTrack = [](
                                   QObject& control,
                                   const char* id,
                                   const char* contract,
                                   jam2::gui::GuiControlAvailability availability =
                                       jam2::gui::GuiControlAvailability::StateGated,
                                   const char* family = nullptr) {
        jam2::gui::registerGuiControl(
            control,
            QStringLiteral("looper.") + QString::fromLatin1(id),
            QString::fromLatin1(contract),
            availability,
            family ? QString::fromLatin1(family) : QString{});
    };
    w.recordingContextFrame_ = new QFrame(page);
    w.recordingContextFrame_->setObjectName(QStringLiteral("RecordingContext"));
    w.recordingContextFrame_->setStyleSheet(QStringLiteral(
        "QFrame#RecordingContext { background: #15130f; border: 1px solid #986d36; border-radius: 3px; }"
        "QLabel#RecordingContextTitle { color: #fff1d5; font-size: 15px; font-weight: 600; }"
        "QLabel#RecordingContextDetail { color: #bdc5c3; font-size: 12px; }"
        "QLabel#RecordingPeerStates { color: #f0dfbe; font-size: 12px; padding-top: 5px; border-top: 1px solid #5f482b; }"
        "QLabel#RecordingPhase { color: #e8a44a; font-size: 12px; font-weight: 600; letter-spacing: 1px; }"));
    auto* recordingContextLayout = new QVBoxLayout(w.recordingContextFrame_);
    recordingContextLayout->setContentsMargins(12, 9, 12, 9);
    recordingContextLayout->setSpacing(6);
    auto* recordingTop = new QHBoxLayout();
    auto* recordingLamp = new QLabel(QStringLiteral("●"), w.recordingContextFrame_);
    recordingLamp->setStyleSheet(QStringLiteral("color: #c92f58; font-size: 18px;"));
    recordingTop->addWidget(recordingLamp);
    auto* recordingCopy = new QVBoxLayout();
    recordingCopy->setSpacing(1);
    w.recordingContextTitle_ = new QLabel(QStringLiteral("Track armed"), w.recordingContextFrame_);
    w.recordingContextTitle_->setObjectName(QStringLiteral("RecordingContextTitle"));
    w.recordingContextDetail_ = new QLabel(w.recordingContextFrame_);
    w.recordingContextDetail_->setObjectName(QStringLiteral("RecordingContextDetail"));
    recordingCopy->addWidget(w.recordingContextTitle_);
    recordingCopy->addWidget(w.recordingContextDetail_);
    recordingTop->addLayout(recordingCopy, 1);
    w.recoverRecordingGroupButton_ = new QPushButton(
        QStringLiteral("Continue Locally"), w.recordingContextFrame_);
    w.recoverRecordingGroupButton_->setToolTip(QStringLiteral(
        "Release a stalled shared take while allowing active recordings to continue locally"));
    registerTrack(
        *w.recoverRecordingGroupButton_, "recording.recover-group",
        "looper.shared-recording-recovery");
    w.recoverRecordingGroupButton_->setStyleSheet(QStringLiteral(
        "QPushButton { background:#2a2520;color:#d9c7ab;border:1px solid #6a563c;"
        "padding:6px 10px;border-radius:3px;font-size:12px; }"
        "QPushButton:hover { background:#3a3025;color:#fff1d5;border-color:#a77a42; }"));
    w.recoverRecordingGroupButton_->hide();
    QObject::connect(w.recoverRecordingGroupButton_, &QPushButton::clicked,
        &w, [&w] { w.requestRecordingGroupRecovery(); });
    recordingTop->addWidget(w.recoverRecordingGroupButton_, 0, Qt::AlignTop);
    recordingContextLayout->addLayout(recordingTop);
    w.recordingPeerStatesLabel_ = new QLabel(w.recordingContextFrame_);
    w.recordingPeerStatesLabel_->setObjectName(QStringLiteral("RecordingPeerStates"));
    w.recordingPeerStatesLabel_->setWordWrap(true);
    recordingContextLayout->addWidget(w.recordingPeerStatesLabel_);
    w.recordingCountdownLabel_ = new QLabel(
        QStringLiteral("ARMED  ›  WAITING FOR BAR  ›  COUNT-IN  ›  RECORDING"),
        w.recordingContextFrame_);
    w.recordingCountdownLabel_->setObjectName(QStringLiteral("RecordingPhase"));
    recordingContextLayout->addWidget(w.recordingCountdownLabel_);
    w.recordingContextFrame_->hide();

    w.trackWaveform_ = nullptr;
    w.looperStack_ = new LooperLaneStackWidget(page);
    w.trackSpeedSlider_ = new QSlider(Qt::Horizontal, page);
    w.trackSpeedSlider_->setRange(10, 200);
    w.trackSpeedSlider_->setValue(100);
    applyJamSliderStyle(w.trackSpeedSlider_);
    w.trackSpeedSlider_->setMinimumWidth(220);
    w.trackSpeedSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.trackSpeedSpin_ = new QDoubleSpinBox(page);
    w.trackSpeedSpin_->setRange(0.10, 2.00);
    w.trackSpeedSpin_->setSingleStep(0.01);
    w.trackSpeedSpin_->setDecimals(2);
    w.trackSpeedSpin_->setValue(1.0);
    w.trackSpeedSpin_->setFixedWidth(92);
    applyMutedEditorStyle(w.trackSpeedSpin_);
    registerTrack(*w.trackSpeedSlider_, "speed.slider", "looper.playback-transform",
        jam2::gui::GuiControlAvailability::StateGated, "looper.speed");
    registerTrack(*w.trackSpeedSpin_, "speed.value", "looper.playback-transform",
        jam2::gui::GuiControlAvailability::StateGated, "looper.speed");
    w.trackPitchSlider_ = new QSlider(Qt::Horizontal, page);
    w.trackPitchSlider_->setRange(-12, 12);
    w.trackPitchSlider_->setValue(0);
    applyJamSliderStyle(w.trackPitchSlider_);
    w.trackPitchSlider_->setMinimumWidth(220);
    w.trackPitchSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.trackPitchSpin_ = new QSpinBox(page);
    w.trackPitchSpin_->setRange(-12, 12);
    w.trackPitchSpin_->setSingleStep(1);
    w.trackPitchSpin_->setSuffix(QStringLiteral(" semitones"));
    w.trackPitchSpin_->setFixedWidth(128);
    applyMutedEditorStyle(w.trackPitchSpin_);
    registerTrack(*w.trackPitchSlider_, "pitch.slider", "looper.playback-transform",
        jam2::gui::GuiControlAvailability::StateGated, "looper.pitch");
    registerTrack(*w.trackPitchSpin_, "pitch.value", "looper.playback-transform",
        jam2::gui::GuiControlAvailability::StateGated, "looper.pitch");
    w.focusFrequencySlider_ = new QSlider(Qt::Horizontal, page);
    w.focusFrequencySlider_->setRange(40, 8000);
    w.focusFrequencySlider_->setValue(120);
    applyJamSliderStyle(w.focusFrequencySlider_);
    w.focusFrequencySlider_->setMinimumWidth(220);
    w.focusFrequencySlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    w.focusFrequencySpin_ = new QSpinBox(page);
    w.focusFrequencySpin_->setRange(40, 8000);
    w.focusFrequencySpin_->setValue(120);
    w.focusFrequencySpin_->setSuffix(QStringLiteral(" Hz"));
    w.focusFrequencySpin_->setFixedWidth(108);
    applyMutedEditorStyle(w.focusFrequencySpin_);
    registerTrack(*w.focusFrequencySlider_, "focus.slider", "looper.focus-filter",
        jam2::gui::GuiControlAvailability::StateGated, "looper.focus-frequency");
    registerTrack(*w.focusFrequencySpin_, "focus.value", "looper.focus-filter",
        jam2::gui::GuiControlAvailability::StateGated, "looper.focus-frequency");
    w.trackGridLockCheck_ = new QCheckBox(QStringLiteral("Lock to grid"), page);
    w.trackGridLockCheck_->setChecked(w.looperProject_.gridLockEnabled());
    registerTrack(*w.trackGridLockCheck_, "grid-lock", "looper.grid-lock");
    w.captureManualStopCheck_ = new QCheckBox(QStringLiteral("Record until stopped"), page);
    w.captureManualStopCheck_->setChecked(true);
    w.captureCountInCheck_ = new QCheckBox(QStringLiteral("Count-in"), page);
    w.captureCountInCheck_->setChecked(true);
    w.captureCountInMetronomeCheck_ = new QCheckBox(QStringLiteral("Metronome during count-in"), page);
    w.captureCountInMetronomeCheck_->setChecked(true);
    w.captureKeepMetronomeCheck_ = new QCheckBox(QStringLiteral("Keep metronome on while recording"), page);
    w.captureKeepMetronomeCheck_->setChecked(false);
    registerTrack(*w.captureManualStopCheck_, "recording.manual-stop", "looper.recording-settings");
    registerTrack(*w.captureCountInCheck_, "recording.count-in", "looper.recording-settings");
    registerTrack(
        *w.captureCountInMetronomeCheck_, "recording.count-in-metronome",
        "looper.recording-settings");
    registerTrack(
        *w.captureKeepMetronomeCheck_, "recording.keep-metronome",
        "looper.recording-settings");
    w.captureCountInBarsSpin_ = new QSpinBox(page);
    w.captureCountInBarsSpin_->setRange(1, 8);
    w.captureCountInBarsSpin_->setValue(1);
    w.captureCountInBarsSpin_->setSuffix(QStringLiteral(" bars"));
    w.captureCountInBarsSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.captureCountInBarsSpin_);
    registerTrack(
        *w.captureCountInBarsSpin_, "recording.count-in-bars", "looper.recording-settings");
    w.captureDurationSpin_ = new QSpinBox(page);
    w.captureDurationSpin_->setRange(1, 128);
    w.captureDurationSpin_->setValue(8);
    w.captureDurationSpin_->setSuffix(QStringLiteral(" bars"));
    w.captureDurationSpin_->setEnabled(false);
    w.captureDurationSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(w.captureDurationSpin_);
    registerTrack(
        *w.captureDurationSpin_, "recording.duration", "looper.recording-settings");
    w.loopStartButton_ = new QPushButton(QStringLiteral("Loop Start"), page);
    w.loopEndButton_ = new QPushButton(QStringLiteral("Loop End"), page);
    w.clearLoopButton_ = new QPushButton(QStringLiteral("Clear Loop"), page);
    w.loopEnabledCheck_ = new QCheckBox(QStringLiteral("Loop whole track"), page);
    w.loopEnabledCheck_->setChecked(w.trackController_.model().loopEnabled);
    registerTrack(*w.loopStartButton_, "loop.start", "looper.loop-region",
        jam2::gui::GuiControlAvailability::StateGated, "looper.loop-region");
    registerTrack(*w.loopEndButton_, "loop.end", "looper.loop-region",
        jam2::gui::GuiControlAvailability::StateGated, "looper.loop-region");
    registerTrack(*w.clearLoopButton_, "loop.clear", "looper.loop-region",
        jam2::gui::GuiControlAvailability::StateGated, "looper.loop-region");
    registerTrack(*w.loopEnabledCheck_, "loop.enabled", "looper.loop-region");
    w.loadWavButton_ = nullptr;
    w.shareTracksButton_ = new QPushButton(QStringLiteral("Share with Jam Now"), page);
    registerTrack(*w.shareTracksButton_, "share-now", "looper.wav-sharing");
    w.shareTracksButton_->setEnabled(!w.automaticWavSharingEnabled());
    w.shareTracksButton_->setToolTip(w.automaticWavSharingEnabled()
        ? QStringLiteral("WAVs are already shared automatically with the jam")
        : QStringLiteral("Manually share the current tracks with the jam"));
    w.startArmedLaneRecordingButton_ = new QPushButton(QStringLiteral("Start Recording"), page);
    registerTrack(
        *w.startArmedLaneRecordingButton_, "recording.start-stop", "looper.recording-lifecycle");

    w.laneRecordingState_.outputPath =
        appReleaseFilePath(QStringLiteral("captures"), QStringLiteral("take.wav"));
    auto* speedControl = new QWidget(page);
    speedControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* speedLayout = new QHBoxLayout(speedControl);
    speedLayout->setContentsMargins(0, 0, 0, 0);
    speedLayout->addWidget(w.trackSpeedSlider_, 1);
    speedLayout->addWidget(w.trackSpeedSpin_);
    auto* pitchControl = new QWidget(page);
    pitchControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* pitchLayout = new QHBoxLayout(pitchControl);
    pitchLayout->setContentsMargins(0, 0, 0, 0);
    pitchLayout->addWidget(w.trackPitchSlider_, 1);
    pitchLayout->addWidget(w.trackPitchSpin_);
    auto* focusControl = new QWidget(page);
    focusControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* focusLayout = new QHBoxLayout(focusControl);
    focusLayout->setContentsMargins(0, 0, 0, 0);
    w.focusFrequencyCheck_ = new QCheckBox(page);
    w.focusPresetBox_ = new QComboBox(page);
    registerTrack(*w.focusFrequencyCheck_, "focus.enabled", "looper.focus-filter");
    registerTrack(*w.focusPresetBox_, "focus.preset", "looper.focus-filter");

    w.focusPresetBox_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    w.focusPresetBox_->addItem(QStringLiteral("Bass"), QStringLiteral("bass"));
    w.focusPresetBox_->addItem(QStringLiteral("Guitar"), QStringLiteral("guitar"));
    w.focusPresetBox_->addItem(QStringLiteral("Vocals"), QStringLiteral("vocals"));
    w.focusPresetBox_->addItem(QStringLiteral("Drums"), QStringLiteral("drums"));
    w.focusPresetBox_->setFixedWidth(108);
    applyMutedEditorStyle(w.focusPresetBox_);
    focusLayout->addWidget(w.focusFrequencyCheck_);
    focusLayout->addWidget(w.focusPresetBox_);
    focusLayout->addWidget(w.focusFrequencySlider_, 1);
    focusLayout->addWidget(w.focusFrequencySpin_);
    auto* loopOptionsControl = new QWidget(page);
    auto* loopOptionsLayout = new QHBoxLayout(loopOptionsControl);
    loopOptionsLayout->setContentsMargins(0, 0, 0, 0);
    loopOptionsLayout->setSpacing(10);
    auto* fitTimelineButton = new QPushButton(QStringLiteral("Fit"), loopOptionsControl);
    auto* zoomOutButton = new QPushButton(QStringLiteral("\u2212"), loopOptionsControl);
    auto* zoomInButton = new QPushButton(QStringLiteral("+"), loopOptionsControl);
    registerTrack(*fitTimelineButton, "timeline.fit", "looper.timeline-zoom",
        jam2::gui::GuiControlAvailability::StateGated, "looper.timeline-zoom");
    registerTrack(*zoomOutButton, "timeline.zoom-out", "looper.timeline-zoom",
        jam2::gui::GuiControlAvailability::StateGated, "looper.timeline-zoom");
    registerTrack(*zoomInButton, "timeline.zoom-in", "looper.timeline-zoom",
        jam2::gui::GuiControlAvailability::StateGated, "looper.timeline-zoom");
    fitTimelineButton->setFixedSize(42, 28);
    zoomOutButton->setFixedSize(30, 28);
    zoomInButton->setFixedSize(30, 28);
    fitTimelineButton->setToolTip(QStringLiteral("Fit the complete track timeline in the view"));
    zoomOutButton->setToolTip(QStringLiteral("Zoom out on the track timeline"));
    zoomInButton->setToolTip(QStringLiteral("Zoom in on the track timeline"));
    fitTimelineButton->setEnabled(false);
    zoomOutButton->setEnabled(false);
    loopOptionsLayout->addWidget(w.trackGridLockCheck_);
    loopOptionsLayout->addSpacing(4);
    loopOptionsLayout->addWidget(fitTimelineButton);
    loopOptionsLayout->addWidget(zoomOutButton);
    loopOptionsLayout->addWidget(zoomInButton);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("Speed"), speedControl);
    form->addRow(QStringLiteral("Pitch"), pitchControl);
    form->addRow(QStringLiteral("Focus frequency"), focusControl);

    recordingTop->addWidget(w.startArmedLaneRecordingButton_);
    w.recordingGlobalControls_ = new QWidget(w.recordingContextFrame_);
    auto* recordingControlsLayout = new QHBoxLayout(w.recordingGlobalControls_);
    recordingControlsLayout->setContentsMargins(0, 2, 0, 0);
    recordingControlsLayout->setSpacing(10);
    recordingControlsLayout->addWidget(w.captureManualStopCheck_);
    recordingControlsLayout->addWidget(w.captureDurationSpin_);
    recordingControlsLayout->addWidget(w.captureCountInCheck_);
    recordingControlsLayout->addWidget(w.captureCountInBarsSpin_);
    recordingControlsLayout->addWidget(w.captureCountInMetronomeCheck_);
    recordingControlsLayout->addWidget(w.captureKeepMetronomeCheck_);
    recordingControlsLayout->addStretch(1);
    recordingContextLayout->addWidget(w.recordingGlobalControls_);

    auto* sectionRow = new QHBoxLayout();
    auto* sectionLabel = new QLabel(QStringLiteral("SECTION"), page);
    sectionLabel->setObjectName(QStringLiteral("BankStripLabel"));
    sectionRow->addWidget(sectionLabel);
    for (int i = 0; i < jam2::application::limits::kMaximumSongSections; ++i) {
        auto* bankButton = new QPushButton(QString(QChar(QLatin1Char(static_cast<char>('A' + i)))), page);
        bankButton->setFixedWidth(34);
        bankButton->setCheckable(true);
        jam2::gui::registerGuiControl(
            *bankButton, QStringLiteral("looper.section.select.%1").arg(i),
            QStringLiteral("song.section-selection"),
            jam2::gui::GuiControlAvailability::StateGated,
            QStringLiteral("looper.section-select"));
        w.looperBankButtons_[i] = bankButton;
        sectionRow->addWidget(bankButton);
        QObject::connect(bankButton, &QPushButton::clicked, &w, [&w, i] {
            w.selectViewedBank(i);
        });
    }
    auto* removeSectionButton = new QPushButton(QStringLiteral("\u2212"), page);
    auto* addSectionButton = new QPushButton(QStringLiteral("+"), page);
    registerTrack(*removeSectionButton, "section.remove", "song.section-structure",
        jam2::gui::GuiControlAvailability::StateGated, "looper.section-structure");
    registerTrack(*addSectionButton, "section.add", "song.section-structure",
        jam2::gui::GuiControlAvailability::StateGated, "looper.section-structure");
    for (QPushButton* button : {removeSectionButton, addSectionButton}) {
        button->setFixedWidth(30);
        sectionRow->addWidget(button);
    }
    w.sectionRemoveButtons_.push_back(removeSectionButton);
    w.sectionAddButtons_.push_back(addSectionButton);
    QObject::connect(removeSectionButton, &QPushButton::clicked, &w, [&w] {
        w.removeLastSongSection();
    });
    QObject::connect(addSectionButton, &QPushButton::clicked, &w, [&w] {
        w.addSongSection();
    });
    auto* trimSectionButton = new QPushButton(QStringLiteral("TRIM SECTION"), page);
    registerTrack(*trimSectionButton, "section.trim", "song.section-structure",
        jam2::gui::GuiControlAvailability::Modal, "looper.section-structure");
    trimSectionButton->setObjectName(QStringLiteral("TrimSectionButton"));
    trimSectionButton->setStyleSheet(QStringLiteral(
        "QPushButton { color:#d7c3a4; border:1px solid #5e4c37; background:#17140f; padding:5px 9px; }"
        "QPushButton:hover { color:#ffd68a; border-color:#986d36; background:#211a12; }"
        "QPushButton:disabled { color:#586164; border-color:#303638; background:#111516; }"));
    w.sectionTrimButtons_.push_back(trimSectionButton);
    sectionRow->addWidget(trimSectionButton);
    QObject::connect(trimSectionButton, &QPushButton::clicked, &w, [&w] {
        w.trimViewedSection();
    });
    sectionRow->addSpacing(12);
    auto* generateIdeaButton = new QPushButton(
        QStringLiteral("GENERATE NEW IDEA"), page);
    auto* browseIdeasButton = new QPushButton(
        QStringLiteral("GROOVE LIBRARY"), page);
    auto* continueIdeaButton = new QPushButton(
        QStringLiteral("CONTINUE IDEA"), page);
    auto* generateWavButton = new QPushButton(
        QStringLiteral("GENERATE WAV"), page);
    auto* jamTasterButton = new QPushButton(
        QStringLiteral("JAMTASTER"), page);
    registerTrack(*generateIdeaButton, "idea.generate", "idea.generate",
        jam2::gui::GuiControlAvailability::Modal, "idea.header-action");
    registerTrack(*browseIdeasButton, "idea.browse", "idea.catalog",
        jam2::gui::GuiControlAvailability::StateGated, "idea.header-action");
    registerTrack(*continueIdeaButton, "idea.continue", "idea.continue",
        jam2::gui::GuiControlAvailability::Modal, "idea.header-action");
    registerTrack(*generateWavButton, "idea.generate-wav", "idea.reference-wav",
        jam2::gui::GuiControlAvailability::Modal, "idea.header-action");
    registerTrack(*jamTasterButton, "idea.jam-taster", "idea.jam-taster",
        jam2::gui::GuiControlAvailability::StateGated, "idea.header-action");
    styleIdeaHeaderAction(generateIdeaButton, IdeaHeaderAction::Generate);
    styleIdeaHeaderAction(browseIdeasButton, IdeaHeaderAction::Browse);
    styleIdeaHeaderAction(continueIdeaButton, IdeaHeaderAction::Continue);
    styleIdeaHeaderAction(generateWavButton, IdeaHeaderAction::Wav);
    styleIdeaHeaderAction(jamTasterButton, IdeaHeaderAction::Browse);
    sectionRow->addWidget(generateIdeaButton);
    sectionRow->addWidget(browseIdeasButton);
    sectionRow->addWidget(continueIdeaButton);
    sectionRow->addWidget(generateWavButton);
    sectionRow->addWidget(jamTasterButton);
    QObject::connect(generateIdeaButton, &QPushButton::clicked, &w, [&w] {
        w.generatePracticeIdea();
    });
    QObject::connect(browseIdeasButton, &QPushButton::clicked, &w, [&w] {
        w.browseCuratedIdeas();
    });
    QObject::connect(continueIdeaButton, &QPushButton::clicked, &w, [&w] {
        w.continuePracticeIdea();
    });
    QObject::connect(generateWavButton, &QPushButton::clicked, &w, [&w] {
        w.generatePracticeReferenceWavs();
    });
    QObject::connect(jamTasterButton, &QPushButton::clicked, &w, [&w] {
        w.showJamTasterDialog();
    });
    sectionRow->addStretch(1);
    w.launchBankButton_ = new QPushButton(QStringLiteral("Queue Section"), page);
    registerTrack(*w.launchBankButton_, "section.queue", "looper.arrangement-playback");
    QObject::connect(w.launchBankButton_, &QPushButton::clicked, &w, [&w] {
        w.requestBankLaunch(w.viewedBankIndex_);
    });
    sectionRow->addWidget(w.launchBankButton_);
    w.arrangementButton_ = new QPushButton(QStringLiteral("Arrangement..."), page);
    registerTrack(
        *w.arrangementButton_, "arrangement", "looper.arrangement-dialog",
        jam2::gui::GuiControlAvailability::Modal);
    QObject::connect(w.arrangementButton_, &QPushButton::clicked, &w, [&w] {
        w.showArrangementDialog();
    });
    sectionRow->addWidget(w.arrangementButton_);

    auto* loopRow = new QHBoxLayout();
    auto* loopLabel = new QLabel(QStringLiteral("LOOP"), page);
    loopLabel->setObjectName(QStringLiteral("BankStripLabel"));
    loopRow->addWidget(loopLabel);
    loopRow->addWidget(w.loopStartButton_);
    loopRow->addWidget(w.loopEndButton_);
    loopRow->addWidget(w.clearLoopButton_);
    loopRow->addWidget(w.loopEnabledCheck_);
    loopRow->addStretch(1);
    loopRow->addWidget(loopOptionsControl);

    auto* listeningGroup = new QWidget(page);
    auto* listeningContent = new QWidget(listeningGroup);
    listeningContent->setLayout(form);
    listeningContent->hide();
    auto* listeningLayout = new QVBoxLayout(listeningGroup);
    listeningLayout->setContentsMargins(0, 0, 0, 0);
    listeningLayout->setSpacing(0);
    auto* listeningToggle = new QPushButton(listeningGroup);
    registerTrack(*listeningToggle, "listening-tools", "looper.listening-tools");
    const auto setListeningToggleText = [listeningToggle](bool open) {
        listeningToggle->setText(QStringLiteral(
            "%1  LISTENING TOOLS     Speed 1.00× · Pitch 0 st · Focus off")
            .arg(open ? QStringLiteral("▾") : QStringLiteral("▸")));
    };
    setListeningToggleText(false);
    listeningToggle->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:12px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
        "QPushButton:hover { color:#ffd68a; }"));
    listeningLayout->addWidget(listeningToggle);
    listeningLayout->addWidget(listeningContent);
    QObject::connect(listeningToggle, &QPushButton::clicked, listeningGroup,
        [listeningContent, setListeningToggleText] {
            const bool open = !listeningContent->isVisible();
            listeningContent->setVisible(open);
            setListeningToggleText(open);
        });

    auto* sharingGroup = new QGroupBox(QStringLiteral("Sharing"), page);
    sharingGroup->setObjectName(QStringLiteral("TrackSharingCard"));
    auto* sharingLayout = new QHBoxLayout(sharingGroup);
    w.trackSharingStatusLabel_ = new QLabel(
        QStringLiteral("LANES: SYNCED  ·  WAVS: AUTOMATIC"), sharingGroup);
    w.trackSharingStatusLabel_->setObjectName(QStringLiteral("TrackSharingStatus"));
    sharingLayout->addWidget(w.trackSharingStatusLabel_, 1);
    sharingLayout->addWidget(w.shareTracksButton_);
    auto* exportButton = new QPushButton(QStringLiteral("Export..."), page);
    registerTrack(
        *exportButton, "export", "looper.export",
        jam2::gui::GuiControlAvailability::Modal,
        "looper.export-dialog");
    QObject::connect(exportButton, &QPushButton::clicked, &w, [&w] {
        w.exportLooperAudio();
    });
    sharingLayout->addWidget(exportButton);

    auto* laneScroll = new QScrollArea(page);
    laneScroll->setWidgetResizable(true);
    laneScroll->setFrameShape(QFrame::NoFrame);
    laneScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    laneScroll->setMinimumHeight(280);
    laneScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    laneScroll->setWidget(w.looperStack_);

    const auto applyTimelineZoom = [laneScroll, fitTimelineButton, zoomOutButton, zoomInButton, &w](int level) {
        w.looperStack_->setTimelineZoomLevel(level, laneScroll->viewport()->width());
        fitTimelineButton->setEnabled(level > 0);
        zoomOutButton->setEnabled(level > 0);
        zoomInButton->setEnabled(level < 6);
    };
    QObject::connect(fitTimelineButton, &QPushButton::clicked, laneScroll,
        [applyTimelineZoom] { applyTimelineZoom(0); });
    QObject::connect(zoomOutButton, &QPushButton::clicked, laneScroll,
        [applyTimelineZoom, &w] {
            applyTimelineZoom(qMax(0, w.looperStack_->timelineZoomLevel() - 1));
        });
    QObject::connect(zoomInButton, &QPushButton::clicked, laneScroll,
        [applyTimelineZoom, &w] {
            applyTimelineZoom(qMin(6, w.looperStack_->timelineZoomLevel() + 1));
        });

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(sectionRow);
    layout->addLayout(loopRow);
    layout->addWidget(w.recordingContextFrame_);
    layout->addWidget(laneScroll, 1);
    layout->addWidget(listeningGroup);
    layout->addWidget(sharingGroup);

    QObject::connect(w.trackSpeedSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        const double speed = static_cast<double>(value) / 100.0;
        w.trackController_.model().speed = speed;
        if (w.trackSpeedSpin_) {
            const QSignalBlocker blocker(w.trackSpeedSpin_);
            w.trackSpeedSpin_->setValue(speed);
        }
    });
    QObject::connect(w.trackSpeedSlider_, &QSlider::sliderReleased, &w, [&w] {
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackSpeedSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), &w, [&w](double value) {
        w.trackController_.model().speed = value;
        if (w.trackSpeedSlider_) {
            const QSignalBlocker blocker(w.trackSpeedSlider_);
            w.trackSpeedSlider_->setValue(qBound(10, qRound(value * 100.0), 200));
        }
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackPitchSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        w.trackController_.model().pitchCents = value * 100;
        if (w.trackPitchSpin_) {
            const QSignalBlocker blocker(w.trackPitchSpin_);
            w.trackPitchSpin_->setValue(value);
        }
    });
    QObject::connect(w.trackPitchSlider_, &QSlider::sliderReleased, &w, [&w] {
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackPitchSpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w](int value) {
        w.trackController_.model().pitchCents = value * 100;
        if (w.trackPitchSlider_) {
            const QSignalBlocker blocker(w.trackPitchSlider_);
            w.trackPitchSlider_->setValue(value);
        }
        w.regeneratePreparedMix();
    });
    QObject::connect(w.loopStartButton_, &QPushButton::clicked, &w, [&w] {
        w.setLoopStartAtCurrentPosition();
    });
    QObject::connect(w.loopEndButton_, &QPushButton::clicked, &w, [&w] {
        w.setLoopEndAtCurrentPosition();
    });
    QObject::connect(w.clearLoopButton_, &QPushButton::clicked, &w, [&w] { w.clearTrackLoop(); });
    QObject::connect(w.loopEnabledCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.trackController_.setLoopEnabled(checked);
        w.updateTrackControls();
        w.loadPreparedMixIntoEngine();
    });
    QObject::connect(w.focusFrequencyCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.trackController_.model().focusEnabled = checked;
        w.regeneratePreparedMix();
    });
    QObject::connect(w.focusPresetBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w](int) {
        QString key = w.focusPresetBox_->currentData().toString();
        if (key.isEmpty()) {
            key = QStringLiteral("custom");
        }
        auto& model = w.trackController_.model();
        model.focusPreset = key;
        if (!isCustomFocusPreset(key)) {
            const FocusPreset preset = focusPresetForKey(key);
            model.focusEnabled = true;
            model.focusFrequencyHz = preset.frequencyHz;
            model.focusGainDb = preset.gainDb;
            model.focusQ = preset.q;
        }
        w.updateTrackControls();
        w.regeneratePreparedMix();
    });
    QObject::connect(w.focusFrequencySlider_, &QSlider::valueChanged, &w, [&w](int value) {
        auto& model = w.trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (w.focusPresetBox_) {
            const QSignalBlocker blocker(w.focusPresetBox_);
            w.focusPresetBox_->setCurrentIndex(qMax(0, w.focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (w.focusFrequencySlider_) {
            w.focusFrequencySlider_->setEnabled(true);
        }
        if (w.focusFrequencySpin_) {
            w.focusFrequencySpin_->setEnabled(true);
        }
        if (w.focusFrequencySpin_) {
            const QSignalBlocker blocker(w.focusFrequencySpin_);
            w.focusFrequencySpin_->setValue(value);
        }
    });
    QObject::connect(w.focusFrequencySlider_, &QSlider::sliderReleased, &w, [&w] {
        w.regeneratePreparedMix();
    });
    QObject::connect(w.focusFrequencySpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w](int value) {
        auto& model = w.trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (w.focusPresetBox_) {
            const QSignalBlocker blocker(w.focusPresetBox_);
            w.focusPresetBox_->setCurrentIndex(qMax(0, w.focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (w.focusFrequencySlider_) {
            w.focusFrequencySlider_->setEnabled(true);
        }
        if (w.focusFrequencySpin_) {
            w.focusFrequencySpin_->setEnabled(true);
        }
        if (w.focusFrequencySlider_) {
            const QSignalBlocker blocker(w.focusFrequencySlider_);
            w.focusFrequencySlider_->setValue(value);
        }
        w.regeneratePreparedMix();
    });
    QObject::connect(w.trackGridLockCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        w.looperProject_.setGridLockEnabled(checked);
        w.refreshLooperLanes();
    });
    QObject::connect(w.captureManualStopCheck_, &QCheckBox::toggled, &w, [&w](bool checked) {
        (void)checked;
        updateCaptureDurationControl(w.captureManualStopCheck_, w.captureDurationSpin_);
    });

    QObject::connect(w.shareTracksButton_, &QPushButton::clicked, &w, [&w] { w.shareLocalTracks(true); });
    QObject::connect(w.startArmedLaneRecordingButton_, &QPushButton::clicked, &w, [&w] {
        if (w.trackRecordingWorkflow_.inputTakeActive() || w.loopbackRecorder_.isRunning()) {
            if (w.loopbackRecorder_.isRunning()) {
                w.loopbackRecorder_.stop();
            } else {
                w.runGridLockedEngineAction(
                    QStringLiteral("record.stop"),
                    [&w](std::uint64_t targetFrame) { w.stopInputCapture(targetFrame); },
                    true);
            }
            return;
        }
        w.startArmedLooperLaneRecording();
    });
    w.looperStack_->onSelected = [&w](int lane) { w.selectedLooperLane_ = lane; };
    w.looperStack_->onAddLane = [&w] { if (!w.sharedRecordingProtected()) w.addEmptyLooperLane(); };
    w.looperStack_->onAddWav = [&w] { if (!w.sharedRecordingProtected()) w.addLooperWavs(); };
    w.looperStack_->onWavDropped = [&w](int lane, const QString& path) {
        w.importWavIntoLooperLane(lane, path);
    };
    w.looperStack_->onMute = [&w](int lane) { if (!w.sharedRecordingProtected()) { w.selectedLooperLane_ = lane; w.toggleSelectedLooperLaneMute(); } };
    w.looperStack_->onSolo = [&w](int lane) { if (!w.sharedRecordingProtected()) { w.selectedLooperLane_ = lane; w.toggleSelectedLooperLaneSolo(); } };
    w.looperStack_->onArm = [&w](int lane) {
        if (w.sharedRecordingProtected()) return;
        w.selectedLooperLane_ = lane;
        if (w.trackRecordingWorkflow_.laneArmedAt(w.viewedBankIndex_, lane)) {
            w.trackRecordingWorkflow_.disarmLane();
            w.publishLocalTrackRecordingState(QStringLiteral("idle"));
            w.refreshLooperLanes();
            w.appendLog(QStringLiteral("disarmed lane recording"));
            return;
        }
        (void)w.armSelectedLooperLaneRecording();
    };
    w.looperStack_->onRename = [&w](int lane) { if (!w.sharedRecordingProtected()) { w.selectedLooperLane_ = lane; w.renameSelectedLooperLane(); } };
    w.looperStack_->onRemove = [&w](int lane) { if (!w.sharedRecordingProtected()) { w.selectedLooperLane_ = lane; w.removeSelectedLooperLane(); } };
    w.looperStack_->onRevealWav = [&w](int lane) { w.revealLooperLaneWav(lane); };
    w.looperStack_->onRemoveWav = [&w](int lane) { if (!w.sharedRecordingProtected()) w.removeLooperLaneWav(lane); };
    w.looperStack_->onAnalyzeWav = [&w](int lane) { w.showJamTasterDialog(lane); };
    w.looperStack_->onGainChanged = [&w](int lane, double gainDb) { if (!w.sharedRecordingProtected()) w.applyLooperLaneGain(lane, gainDb); };
    w.looperStack_->onRegionCommitted = [&w](int lane, qint64 startFrame, qint64 sourceStartFrame, qint64 sourceEndFrame) {
        if (w.sharedRecordingProtected()) return;
        w.selectedLooperLane_ = lane;
        w.applySelectedLooperLaneRegion(startFrame, sourceStartFrame, sourceEndFrame);
    };
    w.refreshLooperLanes();
    w.regeneratePreparedMix();
    w.syncLooperArrangement();

    return page;
}

void MainWindowPages::addBankControls(
    MainWindow& w,
    QWidget* owner,
    QHBoxLayout* layout,
    bool looper,
    const char* view)
{
    if (!owner || !layout) return;
    const QString viewName = QString::fromLatin1(view);
    auto* label = new QLabel(QStringLiteral("SECTION"), owner);
    label->setObjectName(QStringLiteral("BankStripLabel"));
    layout->addWidget(label);
    for (int bank = 0; bank < jam2::application::limits::kMaximumSongSections; ++bank) {
        auto* button = new QPushButton(
            QString(QChar(QLatin1Char('A').unicode() + bank)), owner);
        button->setCheckable(true);
        button->setFixedWidth(34);
        button->setProperty("bankIndex", bank);
        button->setProperty("looperStrip", looper);
        jam2::gui::registerGuiControl(
            *button,
            QStringLiteral("grid.%1.section.select.%2").arg(viewName).arg(bank),
            QStringLiteral("song.section-selection"),
            jam2::gui::GuiControlAvailability::StateGated,
            QStringLiteral("grid.section-select"));
        w.bankViewButtons_.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &w, [&w, bank] {
            w.selectViewedBank(bank);
        });
    }
    auto* removeSectionButton = new QPushButton(QStringLiteral("\u2212"), owner);
    auto* addSectionButton = new QPushButton(QStringLiteral("+"), owner);
    jam2::gui::registerGuiControl(
        *removeSectionButton, QStringLiteral("grid.%1.section.remove").arg(viewName),
        QStringLiteral("song.section-structure"),
        jam2::gui::GuiControlAvailability::StateGated,
        QStringLiteral("grid.section-structure"));
    jam2::gui::registerGuiControl(
        *addSectionButton, QStringLiteral("grid.%1.section.add").arg(viewName),
        QStringLiteral("song.section-structure"),
        jam2::gui::GuiControlAvailability::StateGated,
        QStringLiteral("grid.section-structure"));
    for (QPushButton* button : {removeSectionButton, addSectionButton}) {
        button->setFixedWidth(30);
        layout->addWidget(button);
    }
    w.sectionRemoveButtons_.push_back(removeSectionButton);
    w.sectionAddButtons_.push_back(addSectionButton);
    QObject::connect(removeSectionButton, &QPushButton::clicked, &w, [&w] {
        w.removeLastSongSection();
    });
    QObject::connect(addSectionButton, &QPushButton::clicked, &w, [&w] {
        w.addSongSection();
    });
    auto* trimSectionButton = new QPushButton(QStringLiteral("TRIM SECTION"), owner);
    jam2::gui::registerGuiControl(
        *trimSectionButton, QStringLiteral("grid.%1.section.trim").arg(viewName),
        QStringLiteral("song.section-structure"),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("grid.section-structure"));
    trimSectionButton->setObjectName(QStringLiteral("TrimSectionButton"));
    trimSectionButton->setStyleSheet(QStringLiteral(
        "QPushButton { color:#d7c3a4; border:1px solid #5e4c37; background:#17140f; padding:5px 9px; }"
        "QPushButton:hover { color:#ffd68a; border-color:#986d36; background:#211a12; }"
        "QPushButton:disabled { color:#586164; border-color:#303638; background:#111516; }"));
    w.sectionTrimButtons_.push_back(trimSectionButton);
    layout->addWidget(trimSectionButton);
    QObject::connect(trimSectionButton, &QPushButton::clicked, &w, [&w] {
        w.trimViewedSection();
    });
}

QWidget* MainWindowPages::buildMetronomePage(MainWindow& w)
{
    auto* page = new QWidget(&w);

    w.metronomeBpmSpin_ = new DoubleClickBpmSpin(page);
    w.metronomeBpmSpin_->setRange(1, 400);
    w.metronomeBpmSpin_->setValue(qBound(1, static_cast<int>(std::lround(w.trackController_.model().acceptedBpm)), 400));
    w.metronomeBpmSpin_->setToolTip(
        QStringLiteral("Use −/+ or double-click the BPM value to type a tempo"));

    w.metronomeBeatsSpin_ = new QComboBox(page);
    for (int beats = 1; beats <= 16; ++beats)
        w.metronomeBeatsSpin_->addItem(QString::number(beats), beats);
    w.metronomeBeatsSpin_->setCurrentIndex(w.metronomeBeatsSpin_->findData(4));
    w.metronomeBeatsSpin_->setFixedWidth(54);
    applyMutedEditorStyle(w.metronomeBeatsSpin_);

    w.metronomeBeatUnitBox_ = new QComboBox(page);
    w.metronomeBeatUnitBox_->addItem(QStringLiteral("2"), 2);
    w.metronomeBeatUnitBox_->addItem(QStringLiteral("4"), 4);
    w.metronomeBeatUnitBox_->addItem(QStringLiteral("8"), 8);
    w.metronomeBeatUnitBox_->addItem(QStringLiteral("16"), 16);
    w.metronomeBeatUnitBox_->setCurrentIndex(w.metronomeBeatUnitBox_->findData(4));
    w.metronomeBeatUnitBox_->setFixedWidth(54);
    w.metronomeBeatUnitBox_->setToolTip(
        QStringLiteral("The note value counted by each beat (the denominator in the time signature)"));
    applyMutedEditorStyle(w.metronomeBeatUnitBox_);

    w.metronomeTempoPulseBox_ = new QComboBox(page);
    w.metronomeTempoPulseBox_->addItem(QStringLiteral("Each beat"), 1);
    w.metronomeTempoPulseBox_->addItem(
        QStringLiteral("Dotted beat (compound)"), 3);
    w.metronomeTempoPulseBox_->setToolTip(
        QStringLiteral(
            "How many written beat units one BPM pulse spans; use 3 for dotted-quarter pulse in 6/8, 9/8, or 12/8"));
    w.metronomeTempoPulseBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(w.metronomeTempoPulseBox_);

    w.metronomeDivisionBox_ = new QComboBox(page);
    w.metronomeDivisionBox_->addItem(QStringLiteral("Quarter"), 1);
    w.metronomeDivisionBox_->addItem(QStringLiteral("Eighth"), 2);
    w.metronomeDivisionBox_->addItem(QStringLiteral("16th"), 4);
    w.metronomeDivisionBox_->addItem(QStringLiteral("Triplet"), 3);
    w.metronomeDivisionBox_->addItem(QStringLiteral("6th"), 6);
    w.metronomeDivisionBox_->addItem(QStringLiteral("32nd"), 8);
    w.metronomeDivisionBox_->setCurrentIndex(w.metronomeDivisionBox_->findData(1));
    w.metronomeDivisionBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(w.metronomeDivisionBox_);

    w.metronomeSoundBox_ = new QComboBox(page);
    w.metronomeSoundBox_->addItem(
        QStringLiteral("Classic"),
        static_cast<int>(jam2::metronome::ClickSound::Classic));
    w.metronomeSoundBox_->addItem(
        QStringLiteral("Woodblock"),
        static_cast<int>(jam2::metronome::ClickSound::Woodblock));
    w.metronomeSoundBox_->addItem(
        QStringLiteral("Rim Click"),
        static_cast<int>(jam2::metronome::ClickSound::RimClick));
    w.metronomeSoundBox_->addItem(
        QStringLiteral("Digital Tick"),
        static_cast<int>(jam2::metronome::ClickSound::DigitalTick));
    w.metronomeSoundBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    w.metronomeSoundBox_->setToolTip(
        QStringLiteral("Choose the local click character; leader-audio sends the leader's chosen sound"));
    applyMutedEditorStyle(w.metronomeSoundBox_);

    w.metronomeModeBox_ = new QComboBox(page);
    w.metronomeModeBox_->addItems({
        QStringLiteral("shared-grid"),
        QStringLiteral("leader-audio"),
        QStringLiteral("listener-compensated"),
    });
    w.metronomeModeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    applyMutedEditorStyle(w.metronomeModeBox_);

    const auto registerMetronomeControl = [](QObject& control, const char* id, const char* contract) {
        jam2::gui::registerGuiControl(
            control,
            QStringLiteral("metronome.") + QString::fromLatin1(id),
            QString::fromLatin1(contract),
            jam2::gui::GuiControlAvailability::StateGated);
    };
    registerMetronomeControl(*w.metronomeBeatsSpin_, "beats-per-bar", "metronome.pattern");
    registerMetronomeControl(*w.metronomeBeatUnitBox_, "beat-unit", "metronome.pattern");
    registerMetronomeControl(*w.metronomeTempoPulseBox_, "tempo-pulse", "metronome.pattern");
    registerMetronomeControl(*w.metronomeDivisionBox_, "division", "metronome.pattern");
    registerMetronomeControl(*w.metronomeSoundBox_, "sound", "metronome.sound");
    registerMetronomeControl(*w.metronomeModeBox_, "mode", "metronome.mode-and-epoch");

    w.metronomeCompensationButton_ = new QPushButton(QStringLiteral("Advanced"), page);
    w.metronomeCompensationButton_->setVisible(false);
    jam2::gui::registerGuiControl(
        *w.metronomeCompensationButton_,
        QStringLiteral("metronome.compensation"),
        QStringLiteral("metronome.compensation-dialog"),
        jam2::gui::GuiControlAvailability::Modal);

    w.metronomeCompensationMaxSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationMaxSpin_->setRange(0.0, 1000.0);
    w.metronomeCompensationMaxSpin_->setDecimals(1);
    w.metronomeCompensationMaxSpin_->setSuffix(QStringLiteral(" ms"));
    w.metronomeCompensationMaxSpin_->setValue(250.0);
    w.metronomeCompensationSmoothingSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationSmoothingSpin_->setRange(0.0, 10000.0);
    w.metronomeCompensationSmoothingSpin_->setDecimals(1);
    w.metronomeCompensationSmoothingSpin_->setSuffix(QStringLiteral(" ms"));
    w.metronomeCompensationSmoothingSpin_->setValue(750.0);
    w.metronomeCompensationDeadbandSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationDeadbandSpin_->setRange(0.0, 1000.0);
    w.metronomeCompensationDeadbandSpin_->setDecimals(1);
    w.metronomeCompensationDeadbandSpin_->setSuffix(QStringLiteral(" ms"));
    w.metronomeCompensationDeadbandSpin_->setValue(1.0);
    w.metronomeCompensationSlewSpin_ = new QDoubleSpinBox(page);
    w.metronomeCompensationSlewSpin_->setRange(0.0, 10000.0);
    w.metronomeCompensationSlewSpin_->setDecimals(1);
    w.metronomeCompensationSlewSpin_->setSuffix(QStringLiteral(" ms/s"));
    w.metronomeCompensationSlewSpin_->setValue(40.0);
    const auto registerCompensation = [](QObject& control, const char* id) {
        jam2::gui::registerGuiControl(
            control,
            QStringLiteral("metronome.compensation.") + QString::fromLatin1(id),
            QStringLiteral("metronome.listener-compensation"),
            jam2::gui::GuiControlAvailability::Modal,
            QStringLiteral("metronome.compensation-field"));
    };
    registerCompensation(*w.metronomeCompensationMaxSpin_, "maximum");
    registerCompensation(*w.metronomeCompensationSmoothingSpin_, "smoothing");
    registerCompensation(*w.metronomeCompensationDeadbandSpin_, "deadband");
    registerCompensation(*w.metronomeCompensationSlewSpin_, "slew");
    applyMutedEditorStyle(w.metronomeCompensationMaxSpin_);
    applyMutedEditorStyle(w.metronomeCompensationSmoothingSpin_);
    applyMutedEditorStyle(w.metronomeCompensationDeadbandSpin_);
    applyMutedEditorStyle(w.metronomeCompensationSlewSpin_);
    w.metronomeCompensationMaxSpin_->hide();
    w.metronomeCompensationSmoothingSpin_->hide();
    w.metronomeCompensationDeadbandSpin_->hide();
    w.metronomeCompensationSlewSpin_->hide();

    w.tapTrackMetronomeButton_ = new QPushButton(QStringLiteral("Tap Tempo"), page);
    w.tapTrackMetronomeButton_->setToolTip(
        QStringLiteral("Tap at least twice; pause for two seconds to begin a new tempo"));
    w.metronomeBpmSpin_->setObjectName(QStringLiteral("MetronomeBpm"));
    jam2::gui::registerGuiControl(
        *w.metronomeBpmSpin_, QStringLiteral("metronome.bpm"),
        QStringLiteral("metronome.tempo"),
        jam2::gui::GuiControlAvailability::StateGated);
    QFont bpmFont = w.songTitleEdit_ ? w.songTitleEdit_->font() : page->font();
    bpmFont.setPointSizeF(30.0);
    bpmFont.setFeature(QFont::Tag("lnum"), 1);
    bpmFont.setFeature(QFont::Tag("tnum"), 1);
    w.metronomeBpmSpin_->setFont(bpmFont);
    w.tapTrackMetronomeButton_->setObjectName(QStringLiteral("MetronomeTap"));
    jam2::gui::registerGuiControl(
        *w.tapTrackMetronomeButton_, QStringLiteral("metronome.tap"),
        QStringLiteral("metronome.tap-tempo"),
        jam2::gui::GuiControlAvailability::StateGated);

    auto* content = new QWidget(page);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(14, 12, 14, 18);
    contentLayout->setSpacing(12);

    auto* tempoCard = new QGroupBox(QStringLiteral("Tempo"), content);
    tempoCard->setObjectName(QStringLiteral("MetronomeTempoCard"));
    tempoCard->setMinimumHeight(126);
    auto* tempoStack = new QGridLayout(tempoCard);
    tempoStack->setContentsMargins(1, 1, 1, 1);
    w.metronomeNebula_ = new MetronomeNebulaWidget(tempoCard);
    w.metronomeNebula_->setBpm(w.metronomeBpmSpin_->value());
    tempoStack->addWidget(w.metronomeNebula_, 0, 0);

    auto* tempoControls = new QWidget(tempoCard);
    tempoControls->setObjectName(QStringLiteral("MetronomeTempoControls"));
    auto* tempoLayout = new QHBoxLayout(tempoControls);
    tempoLayout->setContentsMargins(18, 12, 18, 12);
    tempoLayout->setSpacing(18);
    w.tapTrackMetronomeButton_->setFixedSize(150, 42);
    tempoLayout->addWidget(w.tapTrackMetronomeButton_, 0, Qt::AlignVCenter);

    auto* bpmPanel = new QWidget(tempoCard);
    bpmPanel->setObjectName(QStringLiteral("MetronomeTempoPanel"));
    bpmPanel->setMinimumWidth(250);
    bpmPanel->setFixedHeight(42);
    auto* bpmLayout = new QVBoxLayout(bpmPanel);
    bpmLayout->setContentsMargins(0, 0, 0, 0);
    bpmLayout->setSpacing(0);
    auto* bpmControl = new QHBoxLayout();
    bpmControl->setContentsMargins(0, 0, 0, 0);
    bpmControl->setSpacing(8);
    auto* decreaseBpm = new QPushButton(QStringLiteral("−"), bpmPanel);
    auto* increaseBpm = new QPushButton(QStringLiteral("+"), bpmPanel);
    jam2::gui::registerGuiControl(
        *decreaseBpm, QStringLiteral("metronome.bpm.decrease"),
        QStringLiteral("metronome.tempo"),
        jam2::gui::GuiControlAvailability::StateGated,
        QStringLiteral("metronome.bpm-adjust"));
    jam2::gui::registerGuiControl(
        *increaseBpm, QStringLiteral("metronome.bpm.increase"),
        QStringLiteral("metronome.tempo"),
        jam2::gui::GuiControlAvailability::StateGated,
        QStringLiteral("metronome.bpm-adjust"));
    for (QPushButton* button : {decreaseBpm, increaseBpm}) {
        button->setObjectName(QStringLiteral("MetronomeBpmAdjust"));
        button->setFixedSize(40, 40);
        button->setToolTip(w.metronomeBpmSpin_->toolTip());
    }
    w.metronomeBpmSpin_->setFixedWidth(136);
    bpmControl->addWidget(decreaseBpm);
    bpmControl->addWidget(w.metronomeBpmSpin_);
    bpmControl->addWidget(increaseBpm);
    bpmLayout->addLayout(bpmControl);
    tempoLayout->addWidget(bpmPanel);

    tempoLayout->addStretch(1);

    auto* tempoFacts = new QWidget(tempoCard);
    tempoFacts->setObjectName(QStringLiteral("MetronomeTempoPanel"));
    tempoFacts->setMinimumWidth(230);
    auto* factsLayout = new QGridLayout(tempoFacts);
    factsLayout->setContentsMargins(14, 0, 0, 0);
    factsLayout->setHorizontalSpacing(20);
    auto* meterCaption = new QLabel(QStringLiteral("METER"), tempoFacts);
    meterCaption->setObjectName(QStringLiteral("MicroHeading"));
    auto* intervalCaption = new QLabel(QStringLiteral("STEP INTERVAL"), tempoFacts);
    intervalCaption->setObjectName(QStringLiteral("MicroHeading"));
    w.metronomeMeterReadout_ = new QLabel(QStringLiteral("4 / 4"), tempoFacts);
    w.metronomeIntervalReadout_ = new QLabel(QStringLiteral("500.0 ms"), tempoFacts);
    w.metronomeMeterReadout_->setObjectName(QStringLiteral("MetronomeFact"));
    w.metronomeIntervalReadout_->setObjectName(QStringLiteral("MetronomeFact"));
    factsLayout->addWidget(meterCaption, 0, 0);
    factsLayout->addWidget(intervalCaption, 0, 1);
    factsLayout->addWidget(w.metronomeMeterReadout_, 1, 0);
    factsLayout->addWidget(w.metronomeIntervalReadout_, 1, 1);
    factsLayout->setRowStretch(2, 1);
    tempoLayout->addWidget(tempoFacts);
    tempoStack->addWidget(tempoControls, 0, 0);
    contentLayout->addWidget(tempoCard);

    auto* patternCard = new QFrame(content);
    patternCard->setObjectName(QStringLiteral("MetronomeCard"));
    auto* patternLayout = new QVBoxLayout(patternCard);
    patternLayout->setContentsMargins(0, 0, 0, 8);
    patternLayout->setSpacing(0);
    auto* patternHeader = new QWidget(patternCard);
    patternHeader->setObjectName(QStringLiteral("MetronomeCardHeader"));
    auto* patternHeaderLayout = new QHBoxLayout(patternHeader);
    patternHeaderLayout->setContentsMargins(14, 9, 14, 9);
    auto* repeatingLabel = new QLabel(QStringLiteral("REPEATING PATTERN"), patternHeader);
    repeatingLabel->setObjectName(QStringLiteral("MicroHeading"));
    patternHeaderLayout->addWidget(repeatingLabel);
    patternHeaderLayout->addStretch(1);
    patternLayout->addWidget(patternHeader);

    w.metronomePatternWidget_ = new MetronomePatternWidget(patternCard);
    auto* patternScroll = new QScrollArea(patternCard);
    patternScroll->setWidgetResizable(true);
    patternScroll->setFrameShape(QFrame::NoFrame);
    patternScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    patternScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    patternScroll->setMinimumHeight(172);
    patternScroll->setWidget(w.metronomePatternWidget_);
    patternLayout->addWidget(patternScroll);
    auto* legend = new QLabel(patternCard);
    legend->setText(QStringLiteral(
        "<span style='color:#68777a'>○</span> <span style='color:#f5f2e9'>Muted</span>"
        "&nbsp;&nbsp;&nbsp;&nbsp; <span style='color:#66d4cf'>●</span> <span style='color:#f5f2e9'>Hit</span>"
        "&nbsp;&nbsp;&nbsp;&nbsp; <span style='color:#e8a44a'>◆</span> <span style='color:#f5f2e9'>Accent</span>"
        "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; <span style='color:#f5f2e9'>Left-click cycles · right-click chooses</span>"));
    legend->setTextFormat(Qt::RichText);
    legend->setObjectName(QStringLiteral("MetronomeLegend"));
    legend->setContentsMargins(14, 5, 14, 3);
    patternLayout->addWidget(legend);
    contentLayout->addWidget(patternCard);

    auto* settings = new QHBoxLayout();
    settings->setSpacing(10);
    auto* timingCard = new QGroupBox(QStringLiteral("Timing"), content);
    auto* timingForm = new QFormLayout(timingCard);
    timingForm->setHorizontalSpacing(18);
    timingForm->setVerticalSpacing(9);
    auto* meterEditor = new QWidget(timingCard);
    auto* meterEditorLayout = new QHBoxLayout(meterEditor);
    meterEditorLayout->setContentsMargins(0, 0, 0, 0);
    meterEditorLayout->setSpacing(6);
    meterEditorLayout->addWidget(w.metronomeBeatsSpin_);
    meterEditorLayout->addWidget(new QLabel(QStringLiteral("/"), meterEditor));
    meterEditorLayout->addWidget(w.metronomeBeatUnitBox_);
    meterEditor->setFixedWidth(138);
    timingForm->addRow(QStringLiteral("Meter"), meterEditor);
    timingForm->addRow(QStringLiteral("Tempo counts"), w.metronomeTempoPulseBox_);
    timingForm->addRow(QStringLiteral("Division"), w.metronomeDivisionBox_);
    settings->addWidget(timingCard, 1);

    auto* clickCard = new QGroupBox(QStringLiteral("Click"), content);
    auto* clickForm = new QFormLayout(clickCard);
    clickForm->setHorizontalSpacing(18);
    clickForm->setVerticalSpacing(9);
    clickForm->addRow(QStringLiteral("Sound"), w.metronomeSoundBox_);
    settings->addWidget(clickCard, 1);

    auto* syncCard = new QGroupBox(QStringLiteral("Sync"), content);
    auto* syncForm = new QFormLayout(syncCard);
    syncForm->setHorizontalSpacing(18);
    syncForm->setVerticalSpacing(9);
    syncForm->addRow(QStringLiteral("Mode"), w.metronomeModeBox_);
    w.metronomeModeDescription_ = new QLabel(syncCard);
    w.metronomeModeDescription_->setObjectName(QStringLiteral("MetronomeModeDescription"));
    w.metronomeModeDescription_->setWordWrap(true);
    syncForm->addRow(w.metronomeModeDescription_);
    syncForm->addRow(w.metronomeCompensationButton_);
    settings->addWidget(syncCard, 1);
    contentLayout->addLayout(settings);
    contentLayout->addStretch(1);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    QObject::connect(w.tapTrackMetronomeButton_, &QPushButton::clicked, &w, [&w] { w.tapTrackMetronomeTempo(); });
    QObject::connect(decreaseBpm, &QPushButton::clicked, w.metronomeBpmSpin_, [&w] {
        w.metronomeBpmSpin_->setValue(w.metronomeBpmSpin_->value() - 1);
    });
    QObject::connect(increaseBpm, &QPushButton::clicked, w.metronomeBpmSpin_, [&w] {
        w.metronomeBpmSpin_->setValue(w.metronomeBpmSpin_->value() + 1);
    });
    w.metronomePatternWidget_->onStepChanged = [&w](int step, bool enabled, bool accent) {
        if (step < 0 || step >= w.metronomeEnabledSteps_.size() ||
            step >= w.metronomeAccents_.size()) return;
        w.metronomeEnabledSteps_[step] = enabled;
        w.metronomeAccents_[step] = accent;
        w.rebuildMetronomePattern(false);
    };
    QObject::connect(w.metronomeBpmSpin_, qOverload<int>(&QSpinBox::valueChanged), &w, [&w] {
        if (w.metronomeNebula_) w.metronomeNebula_->setBpm(w.metronomeBpmSpin_->value());
        w.rebuildMetronomePattern(false);
        w.updateTrackMetronomeInterval();
        w.refreshLooperLanes();
    });
    QObject::connect(w.metronomeModeBox_, &QComboBox::currentTextChanged, &w, [&w] {
        w.updateMetronomeCompensationVisibility();
        w.updateJamSyncPresentation();
        w.sendMetronomeModeToJam();
    });
    QObject::connect(w.metronomeSoundBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.preferences_.metronome.sound = w.metronomeSoundBox_->currentData().toInt();
        UserPreferencesStore::save(w.preferences_);
        w.sendMetronomeSoundToJam();
    });
    QObject::connect(w.metronomeCompensationButton_, &QPushButton::clicked, &w, [&w] {
        w.showMetronomeCompensationDialog();
    });
    QObject::connect(w.metronomeBeatsSpin_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.rebuildMetronomePattern();
        w.updateTrackMetronomeInterval();
    });
    QObject::connect(w.metronomeBeatUnitBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.rebuildMetronomePattern(false);
        w.updateTrackMetronomeInterval();
    });
    QObject::connect(w.metronomeTempoPulseBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.rebuildMetronomePattern(false);
        w.updateTrackMetronomeInterval();
        w.refreshLooperLanes();
    });
    QObject::connect(w.metronomeDivisionBox_, qOverload<int>(&QComboBox::currentIndexChanged), &w, [&w] {
        w.rebuildMetronomePattern(true);
        w.updateTrackMetronomeInterval();
    });

    w.rebuildMetronomePattern();
    w.updateMetronomeCompensationVisibility();
    return page;
}

void MainWindowPages::buildAudioControls(MainWindow& w)
{
    // Some controls are subsequently reparented into the visible Performance
    // transport. Keep the remaining implementation-only controls in one
    // explicitly hidden owned container; this is not a workspace page.
    auto* page = new QWidget(&w);
    page->setObjectName(QStringLiteral("AudioControlStorage"));
    page->hide();
    auto* content = new QWidget(page);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto makeValueLabel = [page](const QString& text) {
        auto* label = new QLabel(text, page);
        label->setFrameShape(QFrame::StyledPanel);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(82);
        return label;
    };
    auto makeRow = [page](const QString& name, QSlider* slider, QLabel* valueLabel) {
        auto* row = new QWidget(page);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setMinimumWidth(120);
        rowLayout->addWidget(nameLabel);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(valueLabel);
        return row;
    };
    auto makeMeterRow = [page](const QString& name, LevelMeterWidget* meter, QLabel* valueLabel = nullptr) {
        auto* row = new QWidget(page);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setMinimumWidth(120);
        rowLayout->addWidget(nameLabel);
        rowLayout->addWidget(meter, 1);
        if (valueLabel != nullptr) {
            rowLayout->addWidget(valueLabel);
        }
        return row;
    };
    auto makeSectionLabel = [content](const QString& text) {
        auto* label = new QLabel(text, content);
        label->setStyleSheet(QStringLiteral("font-weight: 600; margin-top: 8px;"));
        return label;
    };
    w.mixInputMeter_ = new LevelMeterWidget(page);
    w.mixOutputMeter_ = new LevelMeterWidget(page);
    w.mixRemotePeerMeter_ = new LevelMeterWidget(page);
    w.mixOutputClipLabel_ = makeValueLabel(QStringLiteral("clip 0"));

    w.mixSendLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.mixSendLevelSlider_->setRange(-60, 12);
    w.mixSendLevelSlider_->setValue(0);
    applyJamSliderStyle(w.mixSendLevelSlider_);
    w.mixSendLevelSlider_->setMinimumWidth(220);
    w.mixSendLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    jam2::gui::registerGuiControl(
        *w.mixSendLevelSlider_, QStringLiteral("performance.send-level"),
        QStringLiteral("performance.local-mix"));
    w.mixSendLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    w.mixMonitorCheck_ = new QCheckBox(QStringLiteral("Monitor input"), page);
    w.mixMonitorCheck_->setChecked(false);
    jam2::gui::registerGuiControl(
        *w.mixMonitorCheck_, QStringLiteral("performance.monitor-enabled"),
        QStringLiteral("performance.local-monitor"));
    w.mixMonitorLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.mixMonitorLevelSlider_->setRange(-60, 12);
    w.mixMonitorLevelSlider_->setValue(0);
    applyJamSliderStyle(w.mixMonitorLevelSlider_);
    w.mixMonitorLevelSlider_->setMinimumWidth(220);
    w.mixMonitorLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    jam2::gui::registerGuiControl(
        *w.mixMonitorLevelSlider_, QStringLiteral("performance.monitor-level"),
        QStringLiteral("performance.local-monitor"),
        jam2::gui::GuiControlAvailability::StateGated);

    w.mixMonitorLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    w.metronomeLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.metronomeLevelSlider_->setRange(-60, 12);
    w.metronomeLevelSlider_->setValue(-10);
    applyJamSliderStyle(w.metronomeLevelSlider_);
    w.metronomeLevelSlider_->setMinimumWidth(220);
    w.metronomeLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    jam2::gui::registerGuiControl(
        *w.metronomeLevelSlider_, QStringLiteral("performance.metronome-level"),
        QStringLiteral("performance.metronome-level"));
    w.localMetronomeLevelSlider_ = w.metronomeLevelSlider_;
    w.mixMetronomeLevelSlider_ = w.metronomeLevelSlider_;
    w.mixMetronomeLevelLabel_ = makeValueLabel(QStringLiteral("-10.0 dB"));

    w.masterOutputLevelSlider_ = new QSlider(Qt::Horizontal, page);
    w.masterOutputLevelSlider_->setRange(-60, 12);
    w.masterOutputLevelSlider_->setValue(0);
    applyJamSliderStyle(w.masterOutputLevelSlider_);
    w.masterOutputLevelSlider_->setMinimumWidth(240);
    w.masterOutputLevelSlider_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Fixed);
    jam2::gui::registerGuiControl(
        *w.masterOutputLevelSlider_, QStringLiteral("performance.master-output-level"),
        QStringLiteral("performance.master-output"));
    w.masterOutputLevelLabel_ = makeValueLabel(QStringLiteral("+0.0 dB"));

    w.mixLocalInputSection_ = makeSectionLabel(QStringLiteral("Local input"));
    w.mixInputMeterRow_ = makeMeterRow(QStringLiteral("Input"), w.mixInputMeter_);
    w.mixSendRow_ = makeRow(QStringLiteral("Send"), w.mixSendLevelSlider_, w.mixSendLevelLabel_);
    layout->addWidget(w.mixLocalInputSection_);
    layout->addWidget(w.mixInputMeterRow_);
    layout->addWidget(w.mixSendRow_);

    w.mixMonitorEnableRow_ = new QWidget(page);
    auto* monitorEnableLayout = new QHBoxLayout(w.mixMonitorEnableRow_);
    monitorEnableLayout->setContentsMargins(0, 0, 0, 0);
    monitorEnableLayout->setSpacing(10);
    auto* monitorName = new QLabel(QStringLiteral("Monitoring"), w.mixMonitorEnableRow_);
    monitorName->setMinimumWidth(120);
    monitorEnableLayout->addWidget(monitorName);
    monitorEnableLayout->addWidget(w.mixMonitorCheck_, 1);
    w.mixMonitorRow_ = makeRow(QStringLiteral("Monitor"), w.mixMonitorLevelSlider_, w.mixMonitorLevelLabel_);
    layout->addWidget(w.mixMonitorEnableRow_);
    layout->addWidget(w.mixMonitorRow_);

    w.mixOutputSection_ = makeSectionLabel(QStringLiteral("Output"));
    w.mixOutputMeterRow_ = makeMeterRow(QStringLiteral("Main output"), w.mixOutputMeter_, w.mixOutputClipLabel_);

    layout->addWidget(w.mixOutputSection_);
    layout->addWidget(w.mixOutputMeterRow_);

    layout->addStretch(1);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);

    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll, 1);

    QObject::connect(w.mixSendLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixSendLevelLabel_) {
            w.mixSendLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        w.updateRuntimeControls();
    });
    QObject::connect(w.mixMonitorCheck_, &QCheckBox::toggled, &w, [&w] {
        w.updateMixControls();
        w.updateRuntimeControls();
    });
    QObject::connect(w.mixMonitorLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixMonitorLevelLabel_) {
            w.mixMonitorLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        w.updateRuntimeControls();
    });
    QObject::connect(w.metronomeLevelSlider_, &QSlider::valueChanged, &w, [&w](int value) {
        if (w.mixMetronomeLevelLabel_) {
            w.mixMetronomeLevelLabel_->setText(dbText(static_cast<double>(value)));
        }
        if (w.jam2_.isRunning()) {
            const double metronomeLevel = gainFromDb(static_cast<double>(value));
            w.submitEngineGain(
                jam2::EngineCommandType::SetMetronomeLevel,
                metronomeLevel,
                QStringLiteral("metronome level"));
        }
    });
    QObject::connect(
        w.masterOutputLevelSlider_,
        &QSlider::valueChanged,
        &w,
        [&w](int value) {
            if (w.masterOutputLevelLabel_) {
                w.masterOutputLevelLabel_->setText(dbText(static_cast<double>(value)));
            }
            if (w.jam2_.isRunning()) {
                w.submitEngineGain(
                    jam2::EngineCommandType::SetOutputLevel,
                    gainFromDb(static_cast<double>(value)),
                    QStringLiteral("master output level"));
            }
        });
    w.updateMixControls();
    w.updateMixRemotePeers();
    w.updateJamRecordingControls();
    // These controls remain the single owners of audio state used by the
    // Performance surface and runtime update path. Deleting this storage
    // container leaves MainWindow's control pointers dangling; the first
    // session snapshot then dereferences freed Qt widgets and can crash in
    // Qt6Widgets. It is deliberately not inserted into the workspace stack.
    page->hide();
    return;
}
