#include "JamTasterDialog.hpp"

#include "../jamtaster/JamTasterService.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

JamTasterDialog::JamTasterDialog(
    JamTasterService& service,
    QString projectRoot,
    QString sourcePath,
    QString sourceHash,
    QString displayName,
    Callbacks callbacks,
    QWidget* parent)
    : QDialog(parent)
    , service_(service)
    , projectRoot_(std::move(projectRoot))
    , sourcePath_(std::move(sourcePath))
    , sourceHash_(std::move(sourceHash))
    , displayName_(std::move(displayName))
    , callbacks_(std::move(callbacks))
{
    setWindowTitle(QStringLiteral("JamTaster"));
    resize(820, 720);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto* introduction = new QLabel(
        QStringLiteral(
            "Analyse a Jam2 recording or imported WAV once, then choose exactly "
            "which timing, stems, notes and sections to bring into the jam. "
            "Installation and analysis continue in the background if this window is closed."),
        content);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);

    auto* sourceBox = new QGroupBox(QStringLiteral("Source WAV"), content);
    auto* sourceLayout = new QVBoxLayout(sourceBox);
    auto* sourceRow = new QHBoxLayout();
    sourceEdit_ = new QLineEdit(sourcePath_, sourceBox);
    sourceEdit_->setReadOnly(true);
    auto* choose = new QPushButton(QStringLiteral("Choose WAV…"), sourceBox);
    sourceRow->addWidget(sourceEdit_, 1);
    sourceRow->addWidget(choose);
    sourceLayout->addLayout(sourceRow);
    auto* sourceHelp = new QLabel(
        QStringLiteral(
            "Use an existing WAV lane, import a WAV here, or record a whole song "
            "with System Loopback in Jam2's Track view. Loopback captures exactly "
            "what Jam2 is playing without needing a microphone."),
        sourceBox);
    sourceHelp->setWordWrap(true);
    sourceHelp->setProperty("muted", true);
    sourceLayout->addWidget(sourceHelp);
    connect(choose, &QPushButton::clicked, this, &JamTasterDialog::chooseSource);
    layout->addWidget(sourceBox);

    auto* componentBox = new QGroupBox(QStringLiteral("Analysis component"), content);
    auto* componentLayout = new QVBoxLayout(componentBox);
    auto* componentRow = new QHBoxLayout();
    componentLabel_ = new QLabel(componentBox);
    componentLabel_->setWordWrap(true);
    storageLabel_ = new QLabel(service_.storageSummary(), componentBox);
    storageLabel_->setWordWrap(true);
    storageLabel_->setProperty("muted", true);
    componentInstallButton_ = new QPushButton(QStringLiteral("Install"), componentBox);
    componentHealthButton_ = new QPushButton(QStringLiteral("Check health"), componentBox);
    componentRemoveButton_ = new QPushButton(QStringLiteral("Remove"), componentBox);
    componentRow->addWidget(componentLabel_, 1);
    componentRow->addWidget(componentInstallButton_);
    componentRow->addWidget(componentHealthButton_);
    componentRow->addWidget(componentRemoveButton_);
    componentLayout->addLayout(componentRow);
    componentLayout->addWidget(storageLabel_);
    connect(componentInstallButton_, &QPushButton::clicked, this, [this] {
        if (QFileInfo::exists(service_.componentRoot())) {
            service_.repair();
        } else {
            service_.install();
        }
    });
    connect(componentHealthButton_, &QPushButton::clicked,
            &service_, &JamTasterService::checkHealth);
    connect(componentRemoveButton_, &QPushButton::clicked,
            this, &JamTasterDialog::removePartialComponent);
    layout->addWidget(componentBox);

    auto* actions = new QGroupBox(QStringLiteral("Quick analysis"), content);
    auto* actionLayout = new QHBoxLayout(actions);
    analyzeButton_ = new QPushButton(QStringLiteral("Analyse Everything"), actions);
    bpmButton_ = new QPushButton(QStringLiteral("Find BPM"), actions);
    stemsButton_ = new QPushButton(QStringLiteral("Split Stems"), actions);
    actionLayout->addWidget(analyzeButton_);
    actionLayout->addWidget(bpmButton_);
    actionLayout->addWidget(stemsButton_);
    connect(analyzeButton_, &QPushButton::clicked, this,
            [this] { startAction(QStringLiteral("analyze_all")); });
    connect(bpmButton_, &QPushButton::clicked, this,
            [this] { startAction(QStringLiteral("detect_bpm")); });
    connect(stemsButton_, &QPushButton::clicked, this,
            [this] { startAction(QStringLiteral("split_stems")); });
    layout->addWidget(actions);

    progressLabel_ = new QLabel(QStringLiteral("Ready"), content);
    progressLabel_->setWordWrap(true);
    progressBar_ = new QProgressBar(content);
    progressBar_->setObjectName(QStringLiteral("JamTasterProgress"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    layout->addWidget(progressLabel_);
    layout->addWidget(progressBar_);

    auto* results = new QGroupBox(QStringLiteral("Saved results for this WAV"), content);
    auto* resultLayout = new QFormLayout(results);
    const auto resultRow = [results, resultLayout](
        const QString& name, QCheckBox*& check, QLabel*& value) {
        auto* row = new QWidget(results);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        check = new QCheckBox(name, row);
        value = new QLabel(QStringLiteral("Not analysed"), row);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(check);
        rowLayout->addWidget(value, 1);
        resultLayout->addRow(row);
    };
    resultRow(QStringLiteral("Tempo and grid"), applyTempoCheck_, tempoValue_);
    resultRow(QStringLiteral("Separated stems"), applyStemsCheck_, stemsValue_);
    resultRow(QStringLiteral("Chords"), applyChordsCheck_, chordsValue_);
    resultRow(QStringLiteral("Drum pattern"), applyDrumsCheck_, drumsValue_);
    resultRow(QStringLiteral("Bass notes"), applyBassCheck_, bassValue_);
    resultRow(QStringLiteral("Sections and arrangement"), applySectionsCheck_, sectionsValue_);
    for (QCheckBox* check : {applyTempoCheck_, applyStemsCheck_, applyChordsCheck_,
                             applyDrumsCheck_, applyBassCheck_, applySectionsCheck_}) {
        connect(check, &QCheckBox::toggled, this, [this] { refreshResultsUi(); });
    }
    layout->addWidget(results);

    auto* resultActions = new QHBoxLayout();
    applySelectedButton_ = new QPushButton(QStringLiteral("Apply Selected to Current Jam"), content);
    createSongButton_ = new QPushButton(QStringLiteral("Create New JamJar"), content);
    resultActions->addStretch(1);
    resultActions->addWidget(applySelectedButton_);
    resultActions->addWidget(createSongButton_);
    layout->addLayout(resultActions);

    connect(applySelectedButton_, &QPushButton::clicked, this, [this] {
        QJsonObject options{
            {QStringLiteral("tempo"), applyTempoCheck_->isChecked()},
            {QStringLiteral("stems"), applyStemsCheck_->isChecked()},
            {QStringLiteral("chords"), applyChordsCheck_->isChecked()},
            {QStringLiteral("drums"), applyDrumsCheck_->isChecked()},
            {QStringLiteral("bass"), applyBassCheck_->isChecked()},
            {QStringLiteral("sections"), applySectionsCheck_->isChecked()},
            {QStringLiteral("source_path"), sourcePath_},
            {QStringLiteral("source_hash"), sourceHash_},
        };
        if (!convertedSong_.isEmpty()) {
            if (callbacks_.applyConverted) callbacks_.applyConverted(convertedSong_, options);
        } else if (callbacks_.applyQuick) {
            callbacks_.applyQuick(tempoResult_, stemsResult_, options, sourceHash_);
        }
    });
    connect(createSongButton_, &QPushButton::clicked, this, [this] {
        if (!convertedSong_.isEmpty() && callbacks_.createSong) {
            callbacks_.createSong(convertedSong_, sourcePath_, sourceHash_);
            return;
        }
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("Complete JamJar analysis"),
            QStringLiteral(
                "A new JamJar needs the full timing, section and grid analysis. "
                "Run Analyse Everything now, then continue directly to naming the new JamJar?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            createSongAfterAnalysis_ = true;
            startAction(QStringLiteral("analyze_all"));
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    cancelTaskButton_ = buttons->addButton(
        QStringLiteral("Cancel Task"), QDialogButtonBox::ActionRole);
    connect(cancelTaskButton_, &QPushButton::clicked,
            this, &JamTasterDialog::confirmCancelTask);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);

    connect(&service_, &JamTasterService::componentChanged,
            this, [this] {
                refreshComponent();
                startPendingAction();
            });
    connect(&service_, &JamTasterService::storageUsageChanged,
            this, [this](const QString& summary) {
                storageLabel_->setText(summary);
            });
    connect(&service_, &JamTasterService::componentProgress,
            this, [this](const QString& message, int percent) {
                progressLabel_->setText(message);
                if (percent >= 0) progressBar_->setValue(percent);
                refreshComponent();
                refreshResultsUi();
            });
    connect(&service_, &JamTasterService::componentFailed,
            this, [this](const QString& message) {
                pendingAction_.clear();
                createSongAfterAnalysis_ = false;
                progressLabel_->setText(message);
                if (isVisible()) {
                    QMessageBox::warning(this, QStringLiteral("JamTaster"), message);
                }
            });
    connect(&service_, &JamTasterService::jobStarted,
            this, [this](const QString&) {
                progressBar_->setValue(0);
                progressLabel_->setText(QStringLiteral("Starting isolated JamTaster worker…"));
                refreshResultsUi();
            });
    connect(&service_, &JamTasterService::jobProgress,
            this, [this](const QJsonObject& event) {
                if (event.value(QStringLiteral("type")).toString() !=
                    QStringLiteral("progress")) return;
                const QString message = event.value(QStringLiteral("message")).toString();
                if (!message.isEmpty()) progressLabel_->setText(message);
                if (event.contains(QStringLiteral("percent"))) {
                    progressBar_->setValue(event.value(QStringLiteral("percent")).toInt());
                }
            });
    connect(&service_, &JamTasterService::taskStatusChanged,
            this, [this](const QString&, int, bool) {
                syncTaskState();
                refreshComponent();
                refreshResultsUi();
            });
    connect(&service_, &JamTasterService::taskCancelled,
            this, [this] {
                pendingAction_.clear();
                createSongAfterAnalysis_ = false;
                syncTaskState();
            });
    connect(&service_, &JamTasterService::jobFinished,
            this, [this](const QJsonObject& result) {
                progressBar_->setValue(100);
                progressLabel_->setText(QStringLiteral("Analysis complete"));
                acceptJobResult(result);
            });
    connect(&service_, &JamTasterService::jobFailed,
            this, [this](const QString& error) {
                createSongAfterAnalysis_ = false;
                progressLabel_->setText(error);
                refreshResultsUi();
                if (isVisible()) {
                    QMessageBox::warning(
                        this, QStringLiteral("JamTaster analysis failed"), error);
                }
            });

    refreshComponent();
    refreshSavedResults();
    if (!service_.lastJobResult().isEmpty()) {
        acceptJobResult(service_.lastJobResult());
    }
    syncTaskState();
    refreshResultsUi();
}

void JamTasterDialog::setSourceContext(
    QString projectRoot,
    QString sourcePath,
    QString sourceHash,
    QString displayName)
{
    if (service_.taskActive()) return;
    projectRoot_ = std::move(projectRoot);
    sourcePath_ = std::move(sourcePath);
    sourceHash_ = std::move(sourceHash);
    displayName_ = std::move(displayName);
    sourceEdit_->setText(sourcePath_);
    tempoResult_ = {};
    stemsResult_ = {};
    analysisResult_ = {};
    convertedSong_.clear();
    refreshSavedResults();
    if (!service_.lastJobResult().isEmpty() &&
        QFileInfo(service_.taskInputPath()).absoluteFilePath() ==
            QFileInfo(sourcePath_).absoluteFilePath()) {
        acceptJobResult(service_.lastJobResult());
    }
    syncTaskState();
    refreshResultsUi();
}

void JamTasterDialog::chooseSource()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose a WAV for JamTaster"),
        QFileInfo(sourcePath_).absolutePath(), QStringLiteral("WAV audio (*.wav)"));
    if (path.isEmpty()) return;
    sourcePath_ = QFileInfo(path).absoluteFilePath();
    sourceHash_.clear();
    displayName_ = QFileInfo(path).completeBaseName();
    sourceEdit_->setText(sourcePath_);
    tempoResult_ = {};
    stemsResult_ = {};
    analysisResult_ = {};
    convertedSong_.clear();
    refreshResultsUi();
}

void JamTasterDialog::startAction(const QString& action)
{
    if (!QFileInfo(sourcePath_).isFile()) {
        chooseSource();
        if (!QFileInfo(sourcePath_).isFile()) return;
    }
    if (!service_.isInstalled()) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("Install JamTaster"),
            QStringLiteral(
                "This analysis requires the optional private JamTaster runtime and models. "
                "Jam2 will detect compatible graphics hardware and install the correct "
                "CPU, NVIDIA, or native macOS component inside Jam2's application-data "
                "folder. Your system Python installation will not be changed. Install it now?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Yes);
        if (answer != QMessageBox::Yes) return;
        pendingAction_ = action;
        if (QFileInfo::exists(service_.componentRoot())) {
            service_.repair();
        } else {
            service_.install();
        }
        return;
    }
    pendingAction_ = action;
    startPendingAction();
}

void JamTasterDialog::startPendingAction()
{
    if (pendingAction_.isEmpty() || !service_.isInstalled() || service_.isBusy()) return;
    QJsonObject request{
        {QStringLiteral("action"), pendingAction_},
        {QStringLiteral("input_path"), sourcePath_},
        {QStringLiteral("project_root"), projectRoot_},
        {QStringLiteral("display_name"), displayName_},
        {QStringLiteral("options"), QJsonObject{}},
    };
    QString error;
    if (!service_.startJob(request, error)) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"), error);
        return;
    }
    pendingAction_.clear();
    refreshResultsUi();
}

void JamTasterDialog::refreshComponent()
{
    const QString version = service_.componentVersion();
    const bool filesExist = QFileInfo::exists(service_.componentRoot());
    componentLabel_->setText(version.isEmpty()
        ? service_.componentStatus()
        : QStringLiteral("%1 | version %2")
            .arg(service_.componentStatus(), version));
    storageLabel_->setText(service_.storageSummary());
    if (!service_.taskActive()) service_.refreshStorageUsage();
    componentInstallButton_->setText(filesExist
        ? QStringLiteral("Repair") : QStringLiteral("Install"));
    componentInstallButton_->setEnabled(!service_.taskActive());
    componentHealthButton_->setEnabled(service_.isInstalled() && !service_.taskActive());
    componentRemoveButton_->setText(
        filesExist && !service_.isInstalled()
            ? QStringLiteral("Remove Partial Installation")
            : QStringLiteral("Remove"));
    componentRemoveButton_->setEnabled(filesExist && !service_.taskActive());
    componentLabel_->setToolTip(QStringLiteral(
        "JamTaster's Python runtime and packages are isolated under this application "
        "component folder. They do not modify the system Python installation.\n\n%1")
        .arg(service_.componentRoot()));
}

void JamTasterDialog::syncTaskState()
{
    cancelTaskButton_->setEnabled(service_.taskActive());
    if (service_.taskActive() || service_.taskProgress() >= 100 ||
        service_.taskStatusText() != QStringLiteral("Ready")) {
        progressLabel_->setText(service_.taskStatusText());
        progressBar_->setRange(0, 100);
        progressBar_->setValue(service_.taskProgress());
    } else if (QFileInfo::exists(service_.componentRoot()) && !service_.isInstalled()) {
        progressLabel_->setText(QStringLiteral(
            "A partial JamTaster installation can be repaired or removed."));
        progressBar_->setValue(0);
    }
}

void JamTasterDialog::confirmCancelTask()
{
    if (!service_.taskActive()) return;
    const auto answer = QMessageBox::warning(
        this,
        QStringLiteral("Cancel JamTaster task"),
        QStringLiteral(
            "Cancel the active JamTaster task? An interrupted installation will be "
            "cleaned up, and completed song analysis results will be left intact."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer == QMessageBox::Yes) service_.cancelTask();
}

void JamTasterDialog::removePartialComponent()
{
    if (service_.taskActive() || !QFileInfo::exists(service_.componentRoot())) return;
    const bool partial = !service_.isInstalled();
    const auto answer = QMessageBox::warning(
        this,
        partial ? QStringLiteral("Remove partial JamTaster installation")
                : QStringLiteral("Remove JamTaster"),
        partial
            ? QStringLiteral(
                "Remove the incomplete private JamTaster runtime? This does not remove "
                "analysis saved inside song folders.")
            : QStringLiteral(
                "Remove the private JamTaster runtime and models? Analysis saved inside "
                "song folders will remain available."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    QString error;
    if (!service_.remove(error)) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"), error);
    }
    refreshComponent();
    syncTaskState();
}

QString JamTasterDialog::analysisSourceRoot() const
{
    if (sourceHash_.isEmpty()) return {};
    return QDir(projectRoot_).absoluteFilePath(
        QStringLiteral("analysis/sources/%1").arg(sourceHash_));
}

void JamTasterDialog::refreshSavedResults()
{
    const QString root = analysisSourceRoot();
    if (root.isEmpty()) return;
    tempoResult_ = readJson(QDir(root).absoluteFilePath(QStringLiteral("tempo.json")));
    stemsResult_ = readJson(QDir(root).absoluteFilePath(QStringLiteral("stems.json")));
    analysisResult_ = readJson(QDir(root).absoluteFilePath(QStringLiteral("analysis.json")));
    if (!analysisResult_.isEmpty()) {
        const QString slug = QFileInfo(sourcePath_).completeBaseName();
        const QJsonObject manifest = readJson(
            QDir(root).absoluteFilePath(QStringLiteral("manifest.json")));
        const QString songSlug = manifest.value(QStringLiteral("song_slug")).toString(slug);
        convertedSong_ = QDir(root).absoluteFilePath(
            QStringLiteral("converted/%1").arg(songSlug));
    }
}

void JamTasterDialog::acceptJobResult(const QJsonObject& result)
{
    const QString action = result.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("detect_bpm")) {
        tempoResult_ = result;
        sourceHash_ = result.value(QStringLiteral("source_sha256")).toString();
    } else if (action == QStringLiteral("split_stems")) {
        stemsResult_ = result;
        sourceHash_ = result.value(QStringLiteral("source_sha256")).toString();
    } else {
        convertedSong_ = result.value(QStringLiteral("converted_song")).toString();
        loadFullAnalysis(convertedSong_);
    }
    refreshSavedResults();
    refreshResultsUi();
    for (QCheckBox* check : {applyTempoCheck_, applyStemsCheck_, applyChordsCheck_,
                             applyDrumsCheck_, applyBassCheck_, applySectionsCheck_}) {
        check->setChecked(false);
    }
    if (action == QStringLiteral("detect_bpm")) {
        applyTempoCheck_->setChecked(applyTempoCheck_->isEnabled());
    } else if (action == QStringLiteral("split_stems")) {
        applyStemsCheck_->setChecked(applyStemsCheck_->isEnabled());
    } else {
        for (QCheckBox* check : {applyTempoCheck_, applyStemsCheck_, applyChordsCheck_,
                                 applyDrumsCheck_, applyBassCheck_, applySectionsCheck_}) {
            check->setChecked(check->isEnabled());
        }
    }
    refreshResultsUi();
    if (createSongAfterAnalysis_ && !convertedSong_.isEmpty()) {
        createSongAfterAnalysis_ = false;
        if (callbacks_.createSong) {
            callbacks_.createSong(convertedSong_, sourcePath_, sourceHash_);
        }
    }
}

void JamTasterDialog::loadFullAnalysis(const QString& convertedSong)
{
    if (convertedSong.isEmpty()) return;
    QDir source(QFileInfo(convertedSong).absolutePath());
    if (!source.cdUp()) return;
    analysisResult_ = readJson(source.absoluteFilePath(QStringLiteral("analysis.json")));
    tempoResult_ = readJson(source.absoluteFilePath(QStringLiteral("tempo.json")));
    stemsResult_ = readJson(source.absoluteFilePath(QStringLiteral("stems.json")));
    sourceHash_ = analysisResult_.value(QStringLiteral("input")).toObject()
        .value(QStringLiteral("sha256")).toString(sourceHash_);
}

void JamTasterDialog::refreshResultsUi()
{
    const bool running = service_.taskActive();
    analyzeButton_->setEnabled(!running);
    bpmButton_->setEnabled(!running);
    stemsButton_->setEnabled(!running);

    const bool hasTempo = !tempoResult_.isEmpty();
    const bool hasStems = !stemsResult_.isEmpty();
    const QJsonObject analysis = analysisResult_.value(QStringLiteral("analysis")).toObject();
    const int chordCount = analysis.value(QStringLiteral("chords")).toArray().size();
    const int drumCount = analysis.value(QStringLiteral("drums")).toArray().size();
    const int bassCount = analysis.value(QStringLiteral("bass")).toArray().size();
    const int sectionCount = analysisResult_.value(QStringLiteral("selected_sections")).toArray().size();

    tempoValue_->setText(hasTempo
        ? QStringLiteral("%1 BPM · %2/4")
            .arg(tempoResult_.value(QStringLiteral("bpm")).toDouble(), 0, 'f', 2)
            .arg(tempoResult_.value(QStringLiteral("beats_per_bar")).toInt())
        : QStringLiteral("Not analysed"));
    stemsValue_->setText(hasStems ? QStringLiteral("4 stems available") : QStringLiteral("Not analysed"));
    chordsValue_->setText(chordCount > 0 ? QStringLiteral("%1 regions").arg(chordCount) : QStringLiteral("Not analysed"));
    drumsValue_->setText(drumCount > 0 ? QStringLiteral("%1 events").arg(drumCount) : QStringLiteral("Not analysed"));
    bassValue_->setText(bassCount > 0 ? QStringLiteral("%1 notes").arg(bassCount) : QStringLiteral("Not analysed"));
    sectionsValue_->setText(sectionCount > 0 ? QStringLiteral("%1 sections").arg(sectionCount) : QStringLiteral("Not analysed"));

    applyTempoCheck_->setEnabled(hasTempo);
    applyStemsCheck_->setEnabled(hasStems);
    applyChordsCheck_->setEnabled(chordCount > 0);
    applyDrumsCheck_->setEnabled(drumCount > 0);
    applyBassCheck_->setEnabled(bassCount > 0);
    applySectionsCheck_->setEnabled(sectionCount > 0);
    for (QCheckBox* check : {applyTempoCheck_, applyStemsCheck_, applyChordsCheck_,
                             applyDrumsCheck_, applyBassCheck_, applySectionsCheck_}) {
        if (!check->isEnabled()) check->setChecked(false);
    }
    const bool hasSelection = applyTempoCheck_->isChecked() ||
        applyStemsCheck_->isChecked() || applyChordsCheck_->isChecked() ||
        applyDrumsCheck_->isChecked() || applyBassCheck_->isChecked() ||
        applySectionsCheck_->isChecked();
    const bool hasQuickResult = hasTempo || hasStems;
    applySelectedButton_->setEnabled(
        (!convertedSong_.isEmpty() || hasQuickResult) && hasSelection && !running);
    createSongButton_->setEnabled(
        (!convertedSong_.isEmpty() || hasQuickResult) && !running);
}

QJsonObject JamTasterDialog::readJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}
