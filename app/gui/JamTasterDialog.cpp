#include "JamTasterDialog.hpp"

#include "../jamtaster/JamTasterService.hpp"
#include "../jamtaster/ModelDownloadService.hpp"

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
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

JamTasterDialog::JamTasterDialog(
    JamTasterService& service,
    QString projectRoot,
    QString sourcePath,
    QString sourceHash,
    QString displayName,
    Callbacks callbacks,
    QWidget* parent,
    QNetworkAccessManager* modelNetwork)
    : QDialog(parent)
    , service_(service)
    , projectRoot_(std::move(projectRoot))
    , sourcePath_(std::move(sourcePath))
    , sourceHash_(std::move(sourceHash))
    , displayName_(std::move(displayName))
    , callbacks_(std::move(callbacks))
{
    setWindowTitle(QStringLiteral("JamTaster"));
    setProperty("jam2MaximumDialogHeight", 760);
    resize(820, 760);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    layout->setAlignment(Qt::AlignTop);

    auto* introduction = new QLabel(
        QStringLiteral(
            "Analyse a Jam2 recording or imported WAV once, then choose exactly "
            "which timing, stems, notes and sections to bring into the jam. "
            "Analysis continues in the background if this window is closed."),
        content);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);

    modelDownload_ = std::make_unique<ModelDownloadService>(
        service_.installationRoot(), service_.modelsPath(), nullptr, modelNetwork);
    auto* modelsBox = new QGroupBox(QStringLiteral("JamTaster models"), content);
    modelsPanel_ = modelsBox;
    auto* modelsLayout = new QVBoxLayout(modelsBox);
    modelsLabel_ = new QLabel(modelsBox);
    modelsLabel_->setObjectName(QStringLiteral("JamTasterModelsStatus"));
    modelsLabel_->setWordWrap(true);
    modelsProgress_ = new QProgressBar(modelsBox);
    modelsProgress_->setObjectName(QStringLiteral("JamTasterModelProgress"));
    auto* modelButtons = new QHBoxLayout;
    downloadModelsButton_ = new QPushButton(QStringLiteral("Download models"), modelsBox);
    downloadModelsButton_->setObjectName(QStringLiteral("JamTasterDownloadModels"));
    retryModelsButton_ = new QPushButton(QStringLiteral("Retry"), modelsBox);
    retryModelsButton_->setObjectName(QStringLiteral("JamTasterRetryModels"));
    cancelModelsButton_ = new QPushButton(QStringLiteral("Cancel download"), modelsBox);
    cancelModelsButton_->setObjectName(QStringLiteral("JamTasterCancelModels"));
    modelButtons->addWidget(downloadModelsButton_);
    modelButtons->addWidget(retryModelsButton_);
    modelButtons->addWidget(cancelModelsButton_);
    modelButtons->addStretch();
    modelsLayout->addWidget(modelsLabel_);
    modelsLayout->addWidget(modelsProgress_);
    modelsLayout->addLayout(modelButtons);
    layout->addWidget(modelsBox);
    modelsBox->hide();
    connect(downloadModelsButton_, &QPushButton::clicked, this, [this] {
        const auto answer = QMessageBox::question(this, QStringLiteral("Download JamTaster models"),
            QStringLiteral("Download %1 MB of analysis models now?\n\n"
                "Models are saved inside this Jam2 folder. Your jam can keep playing. "
                "Closing JamTaster cancels the download.")
                .arg(static_cast<double>(modelDownload_->downloadBytes()) / 1'000'000.0, 0, 'f', 1),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) modelDownload_->download();
    });
    connect(retryModelsButton_, &QPushButton::clicked, this, &JamTasterDialog::checkModels);
    connect(cancelModelsButton_, &QPushButton::clicked, this, [this] { modelDownload_->cancel(); });
    modelDownload_->statusChanged = [this] { refreshModelState(); };
    connect(this, &QDialog::finished, this, [this] { modelDownload_->cancel(); });

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

    auto* progressPanel = new QWidget(content);
    progressPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* progressLayout = new QVBoxLayout(progressPanel);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(6);
    progressLabel_ = new QLabel(QStringLiteral("Ready"), progressPanel);
    progressLabel_->setWordWrap(true);
    progressLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    progressBar_ = new QProgressBar(progressPanel);
    progressBar_->setObjectName(QStringLiteral("JamTasterProgress"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressLayout->addWidget(progressLabel_);
    progressLayout->addWidget(progressBar_);
    layout->addWidget(progressPanel);

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

    auto* footer = new QWidget(this);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
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
    footerLayout->addWidget(buttons);
    outer->addWidget(footer);

    JamTasterService::Observer observer;
    observer.jobStarted = [this](const QString&) {
                progressBar_->setValue(0);
                progressLabel_->setText(QStringLiteral("Starting isolated JamTaster worker…"));
                refreshResultsUi();
            };
    observer.jobProgress = [this](const QJsonObject& event) {
                if (event.value(QStringLiteral("type")).toString() !=
                    QStringLiteral("progress")) return;
                const QString message = event.value(QStringLiteral("message")).toString();
                if (!message.isEmpty()) progressLabel_->setText(message);
                if (event.contains(QStringLiteral("percent"))) {
                    progressBar_->setValue(event.value(QStringLiteral("percent")).toInt());
                }
            };
    observer.taskStatusChanged = [this](const QString&, int, bool) {
                syncTaskState();
                refreshResultsUi();
            };
    observer.taskCancelled = [this] {
                pendingAction_.clear();
                createSongAfterAnalysis_ = false;
                syncTaskState();
            };
    observer.jobFinished = [this](const QJsonObject& result) {
                progressBar_->setValue(100);
                progressLabel_->setText(QStringLiteral("Analysis complete"));
                if (!serviceTaskMatchesSource()) {
                    refreshResultsUi();
                    return;
                }
                acceptJobResult(result);
            };
    observer.jobFailed = [this](const QString& error) {
                createSongAfterAnalysis_ = false;
                progressLabel_->setText(error);
                refreshResultsUi();
                if (isVisible()) {
                    QMessageBox::warning(
                        this, QStringLiteral("JamTaster analysis failed"), error);
                }
            };
    serviceObserverId_ = service_.addObserver(std::move(observer));

    refreshSavedResults();
    if (!service_.lastJobResult().isEmpty() && serviceTaskMatchesSource()) {
        acceptJobResult(service_.lastJobResult());
    }
    syncTaskState();
    refreshResultsUi();
}

JamTasterDialog::~JamTasterDialog()
{
    modelDownload_->statusChanged = {};
    service_.removeObserver(serviceObserverId_);
}

void JamTasterDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    QTimer::singleShot(0, this, [this] { if (isVisible()) checkModels(); });
}

void JamTasterDialog::checkModels()
{
    if (service_.taskActive() || modelDownload_->busy()) return;
    if (!QFileInfo(service_.workerPath()).isFile()) {
        modelsPanel_->show();
        modelsLabel_->setText(service_.bundleStatus() +
            QStringLiteral(" You can close JamTaster and continue the jam."));
        modelsProgress_->hide();
        downloadModelsButton_->hide();
        cancelModelsButton_->hide();
        retryModelsButton_->show();
        return;
    }
    // Existing injected development/test bundles can supply their own models
    // without a release manifest. Missing models always require pinned metadata.
    if (!QFileInfo(QDir(service_.installationRoot()).filePath(QStringLiteral("models.json"))).exists() &&
        service_.isAvailable()) return;
    modelsReady_ = false;
    modelDownload_->check();
    refreshResultsUi();
}

void JamTasterDialog::refreshModelState()
{
    using State = ModelDownloadService::State;
    const auto& status = modelDownload_->status();
    modelsReady_ = status.state == State::Ready;
    modelsPanel_->setVisible(status.state != State::Idle);
    modelsLabel_->setText(status.message);
    modelsProgress_->setVisible(status.state != State::Failed && status.state != State::Cancelled &&
        status.state != State::AwaitingApproval);
    modelsProgress_->setRange(0, status.percent < 0 ? 0 : 100);
    if (status.percent >= 0) modelsProgress_->setValue(status.percent);
    downloadModelsButton_->setVisible(status.state == State::AwaitingApproval);
    retryModelsButton_->setVisible(status.state == State::Failed || status.state == State::Cancelled);
    retryModelsButton_->setEnabled(!modelDownload_->busy());
    cancelModelsButton_->setVisible(status.state == State::Checking ||
        status.state == State::FetchingMetadata || status.state == State::Downloading ||
        status.state == State::Installing);
    cancelModelsButton_->setText(status.state == State::Downloading
        ? QStringLiteral("Cancel download") : QStringLiteral("Cancel setup"));
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
    if (!service_.lastJobResult().isEmpty() && serviceTaskMatchesSource()) {
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
    if (!modelsReady_ || modelDownload_->busy()) return;
    if (!QFileInfo(sourcePath_).isFile()) {
        chooseSource();
        if (!QFileInfo(sourcePath_).isFile()) return;
    }
    if (!service_.isAvailable()) {
        QMessageBox::warning(this, QStringLiteral("JamTaster"), service_.bundleStatus());
        return;
    }
    pendingAction_ = action;
    startPendingAction();
}

void JamTasterDialog::startPendingAction()
{
    if (pendingAction_.isEmpty() || !service_.isAvailable() || service_.isBusy()) return;
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

void JamTasterDialog::syncTaskState()
{
    cancelTaskButton_->setEnabled(service_.taskActive());
    if (service_.taskActive() || service_.taskProgress() >= 100 ||
        service_.taskStatusText() != QStringLiteral("Ready")) {
        progressLabel_->setText(service_.taskStatusText());
        progressBar_->setRange(0, 100);
        progressBar_->setValue(service_.taskProgress());
    }
}

void JamTasterDialog::confirmCancelTask()
{
    if (!service_.taskActive()) return;
    const auto answer = QMessageBox::warning(
        this,
        QStringLiteral("Cancel JamTaster task"),
        QStringLiteral(
            "Cancel the active JamTaster task? Completed song analysis results will be left intact."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer == QMessageBox::Yes) service_.cancelTask();
}

QString JamTasterDialog::analysisSourceRoot() const
{
    if (sourceHash_.isEmpty()) return {};
    return QDir(projectRoot_).absoluteFilePath(
        QStringLiteral("analysis/sources/%1").arg(sourceHash_));
}

bool JamTasterDialog::serviceTaskMatchesSource() const
{
    if (service_.taskInputPath().isEmpty() || sourcePath_.isEmpty()) return false;
    return QFileInfo(service_.taskInputPath()).absoluteFilePath() ==
        QFileInfo(sourcePath_).absoluteFilePath();
}

bool JamTasterDialog::isCompleteConvertedSong(const QString& convertedSong)
{
    if (convertedSong.isEmpty()) return false;
    const QDir converted(convertedSong);
    return converted.exists() && converted.entryList(
        {QStringLiteral("*.jamjar")}, QDir::Files, QDir::Name).size() == 1;
}

void JamTasterDialog::refreshSavedResults()
{
    const QString root = analysisSourceRoot();
    if (root.isEmpty()) return;
    convertedSong_.clear();
    tempoResult_ = readJson(QDir(root).absoluteFilePath(QStringLiteral("tempo.json")));
    stemsResult_ = readJson(QDir(root).absoluteFilePath(QStringLiteral("stems.json")));
    analysisResult_ = readJson(QDir(root).absoluteFilePath(QStringLiteral("analysis.json")));
    if (!analysisResult_.isEmpty()) {
        const QString slug = QFileInfo(sourcePath_).completeBaseName();
        const QJsonObject manifest = readJson(
            QDir(root).absoluteFilePath(QStringLiteral("manifest.json")));
        const QString songSlug = manifest.value(QStringLiteral("song_slug")).toString(slug);
        const QString candidate = QDir(root).absoluteFilePath(
            QStringLiteral("converted/%1").arg(songSlug));
        if (isCompleteConvertedSong(candidate)) convertedSong_ = candidate;
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
        const QString converted = result.value(QStringLiteral("converted_song")).toString();
        convertedSong_ = loadFullAnalysis(converted) ? converted : QString{};
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
    if (createSongAfterAnalysis_) {
        createSongAfterAnalysis_ = false;
        if (!convertedSong_.isEmpty() && callbacks_.createSong) {
            callbacks_.createSong(convertedSong_, sourcePath_, sourceHash_);
        }
    }
}

bool JamTasterDialog::loadFullAnalysis(const QString& convertedSong)
{
    if (!isCompleteConvertedSong(convertedSong)) return false;
    QDir source(QFileInfo(convertedSong).absolutePath());
    if (!source.cdUp()) return false;
    const QJsonObject analysis = readJson(
        source.absoluteFilePath(QStringLiteral("analysis.json")));
    if (analysis.isEmpty()) return false;
    analysisResult_ = analysis;
    tempoResult_ = readJson(source.absoluteFilePath(QStringLiteral("tempo.json")));
    stemsResult_ = readJson(source.absoluteFilePath(QStringLiteral("stems.json")));
    sourceHash_ = analysisResult_.value(QStringLiteral("input")).toObject()
        .value(QStringLiteral("sha256")).toString(sourceHash_);
    return true;
}

void JamTasterDialog::refreshResultsUi()
{
    const bool running = service_.taskActive();
    const bool canAnalyze = !running && modelsReady_ && !modelDownload_->busy();
    analyzeButton_->setEnabled(canAnalyze);
    bpmButton_->setEnabled(canAnalyze);
    stemsButton_->setEnabled(canAnalyze);

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
    const bool hasConvertedSong = !convertedSong_.isEmpty();
    applyChordsCheck_->setEnabled(hasConvertedSong && chordCount > 0);
    applyDrumsCheck_->setEnabled(hasConvertedSong && drumCount > 0);
    applyBassCheck_->setEnabled(hasConvertedSong && bassCount > 0);
    applySectionsCheck_->setEnabled(hasConvertedSong && sectionCount > 0);
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
        (!convertedSong_.isEmpty() || hasQuickResult) && !running &&
        (!convertedSong_.isEmpty() || canAnalyze));
}

QJsonObject JamTasterDialog::readJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}
