#include "MainWindow.hpp"

#include "SessionController.hpp"

#include "common.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QIODevice>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

QString keyToHex(const std::array<std::uint8_t, 16>& key)
{
    return QString::fromStdString(jam2::hex_encode(key.data(), key.size()));
}

QString sessionToHex(std::uint64_t session)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[bytes.size() - 1 - i] = static_cast<std::uint8_t>((session >> (i * 8)) & 0xffU);
    }
    return QString::fromStdString(jam2::hex_encode(bytes.data(), bytes.size()));
}

QString doubleText(const QJsonObject& object, const QString& key, const QString& suffix, int precision = 1)
{
    return QString::number(object.value(key).toDouble(), 'f', precision) + suffix;
}

QSlider* makeUnitSlider(double value, QWidget* parent)
{
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(0, 100);
    slider->setValue(qRound(value * 100.0));
    slider->setMinimumWidth(160);
    return slider;
}

QString deviceId(const QString& text)
{
    const QRegularExpression re(QStringLiteral("^\\s*\\[?(\\d+)\\]?"));
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1) : text.trimmed();
}

struct WavMetadata {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataBytes = 0;
    int durationMs = 0;
    QString sha256;
};

quint16 readLe16(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset])) |
        static_cast<quint16>(static_cast<unsigned char>(data[offset + 1]) << 8);
}

quint32 readLe32(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[offset])) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 8) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

WavMetadata readWavMetadata(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("failed to open WAV: %1").arg(path).toStdString());
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        throw std::runtime_error("not a RIFF/WAVE file");
    }

    WavMetadata meta;
    meta.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    qsizetype offset = 12;
    while (offset + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(offset, 4);
        const quint32 size = readLe32(bytes, offset + 4);
        const qsizetype payload = offset + 8;
        if (payload + size > bytes.size()) {
            break;
        }
        if (id == "fmt " && size >= 16) {
            meta.channels = readLe16(bytes, payload + 2);
            meta.sampleRate = static_cast<int>(readLe32(bytes, payload + 4));
            meta.bitsPerSample = readLe16(bytes, payload + 14);
        } else if (id == "data") {
            meta.dataBytes = size;
        }
        offset = payload + size + (size % 2);
    }
    if (meta.sampleRate > 0 && meta.channels > 0 && meta.bitsPerSample > 0 && meta.dataBytes > 0) {
        const qint64 frameBytes = static_cast<qint64>(meta.channels) * meta.bitsPerSample / 8;
        if (frameBytes > 0) {
            meta.durationMs = static_cast<int>((meta.dataBytes / frameBytes) * 1000 / meta.sampleRate);
        }
    }
    return meta;
}

QJsonObject readSidecarJson(const QString& wavPath)
{
    QFile file(wavPath + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QString onOff(bool value)
{
    return value ? QStringLiteral("on") : QStringLiteral("off");
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    generateSession();
    trackPlayer_.setAudioOutput(&trackAudio_);
    trackAudio_.setVolume(0.75);
    buildUi();

    jam2_.onOutputLine = [this](const QString& line) { appendLog(line); };
    jam2_.onErrorLine = [this](const QString& line) { appendLog(QStringLiteral("stderr: ") + line); };
    jam2_.onStatus = [this](const QJsonObject& status) { handleStatus(status); };
    jam2_.onFinished = [this](int code) {
        appendLog(QStringLiteral("jam2 exited rc=%1").arg(code));
        connectionLabel_->setText(QStringLiteral("Stopped"));
        startButton_->setEnabled(true);
        stopButton_->setEnabled(false);
    };
    controlServer_.onState = [this](const QString& state) {
        connectionLabel_->setText(state);
        appendLog(QStringLiteral("control: ") + state);
    };
    controlServer_.onMessage = [this](const QJsonObject& message) { handleControlMessage(message); };
    controlClient_.onState = [this](const QString& state) {
        connectionLabel_->setText(state);
        appendLog(QStringLiteral("control: ") + state);
    };
    controlClient_.onMessage = [this](const QJsonObject& message) { handleControlMessage(message); };
    QObject::connect(&captureProcess_, &QProcess::readyReadStandardOutput, this, [this] {
        const QString text = QString::fromUtf8(captureProcess_.readAllStandardOutput());
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) {
                handleCaptureOutputLine(line.trimmed());
            }
        }
    });
    QObject::connect(&captureProcess_, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(captureProcess_.readAllStandardError());
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) {
                appendLog(QStringLiteral("capture stderr: ") + line.trimmed());
            }
        }
    });
    QObject::connect(&captureProcess_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        appendLog(QStringLiteral("capture exited rc=%1").arg(exitCode));
        if (captureButton_) {
            captureButton_->setEnabled(true);
        }
        if (loopbackCaptureButton_) {
            loopbackCaptureButton_->setEnabled(true);
        }
        if (stopCaptureButton_) {
            stopCaptureButton_->setEnabled(false);
        }
        if (importCaptureButton_) {
            importCaptureButton_->setEnabled(exitCode == 0 && QFileInfo::exists(lastCapturePath_));
        }
        if (captureProgressLabel_) {
            captureProgressLabel_->setText(exitCode == 0 ? QStringLiteral("Capture ready") : QStringLiteral("Capture failed"));
        }
    });
    QObject::connect(&trackPlayer_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (trackPositionSlider_ && !trackPositionSlider_->isSliderDown()) {
            trackPositionSlider_->setValue(static_cast<int>(qMin<qint64>(position, trackPositionSlider_->maximum())));
        }
    });
    QObject::connect(&trackPlayer_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (trackPositionSlider_) {
            trackPositionSlider_->setRange(0, static_cast<int>(qMin<qint64>(duration, std::numeric_limits<int>::max())));
        }
    });
    QObject::connect(&trackPlayer_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (!trackPlaybackLabel_) {
            return;
        }
        if (state == QMediaPlayer::PlayingState) {
            trackPlaybackLabel_->setText(QStringLiteral("Playing local track"));
        } else if (state == QMediaPlayer::PausedState) {
            trackPlaybackLabel_->setText(QStringLiteral("Track paused"));
        } else {
            trackPlaybackLabel_->setText(QStringLiteral("Track stopped"));
        }
    });
    QObject::connect(&trackPlayer_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& error) {
        if (!error.isEmpty()) {
            appendLog(QStringLiteral("track playback error: ") + error);
        }
    });
}

MainWindow::~MainWindow()
{
    stopJam();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Jam2"));
    setMinimumSize(1280, 720);

    titleLabel_ = new QLabel(QStringLiteral("Jam2"), this);
    titleLabel_->setObjectName(QStringLiteral("AppTitle"));
    connectionLabel_ = new QLabel(QStringLiteral("Idle"), this);
    connectionLabel_->setObjectName(QStringLiteral("StatusPill"));
    rttLabel_ = new QLabel(QStringLiteral("RTT -"), this);
    rttLabel_->setObjectName(QStringLiteral("StatusPill"));
    jitterLabel_ = new QLabel(QStringLiteral("Jitter -"), this);
    jitterLabel_->setObjectName(QStringLiteral("StatusPill"));
    lossLabel_ = new QLabel(QStringLiteral("Loss -"), this);
    lossLabel_->setObjectName(QStringLiteral("StatusPill"));

    auto* header = new QHBoxLayout();
    header->addWidget(titleLabel_);
    header->addStretch(1);
    header->addWidget(connectionLabel_);
    header->addWidget(rttLabel_);
    header->addWidget(jitterLabel_);
    header->addWidget(lossLabel_);

    songTitleEdit_ = new QLineEdit(songModel_.title(), this);
    songTitleEdit_->setMinimumWidth(300);
    auto* newSongButton = new QPushButton(QStringLiteral("New Song"), this);
    auto* openSongButton = new QPushButton(QStringLiteral("Open"), this);
    auto* saveSongButton = new QPushButton(QStringLiteral("Save"), this);
    auto* library = new QHBoxLayout();
    library->addWidget(new QLabel(QStringLiteral("Song"), this));
    library->addWidget(songTitleEdit_, 1);
    library->addWidget(newSongButton);
    library->addWidget(openSongButton);
    library->addWidget(saveSongButton);

    QObject::connect(songTitleEdit_, &QLineEdit::editingFinished, this, [this] {
        songModel_.setTitle(songTitleEdit_->text());
    });
    QObject::connect(newSongButton, &QPushButton::clicked, this, [this] { newSong(); });
    QObject::connect(openSongButton, &QPushButton::clicked, this, [this] { openSong(); });
    QObject::connect(saveSongButton, &QPushButton::clicked, this, [this] { saveSong(); });

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildSongPage(), QStringLiteral("Chord View"));
    beatGrid_ = new BeatGridWidget(&songModel_, QStringLiteral("beat"), this);
    tabs_->addTab(beatGrid_, QStringLiteral("Beat View"));
    lyricGrid_ = new BeatGridWidget(&songModel_, QStringLiteral("lyric"), this);
    tabs_->addTab(lyricGrid_, QStringLiteral("Lyrics"));
    tabs_->addTab(buildArrangementPage(), QStringLiteral("Arrangement"));
    tabs_->addTab(buildTrackPage(), QStringLiteral("Track"));
    tabs_->addTab(buildStatsPage(), QStringLiteral("Stats"));

    auto sendCellEdit = [this](int section, const QString& lane, int beat, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
        refreshSongViews();
    };
    beatGrid_->onCellEdited = sendCellEdit;
    lyricGrid_->onCellEdited = sendCellEdit;
    beatGrid_->onStructureChanged = [this] { refreshSongViews(); };
    lyricGrid_->onStructureChanged = [this] { refreshSongViews(); };
    beatGrid_->onGridResized = [this](int section, int beats, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beats"), beats},
        });
        refreshSongViews();
    };
    lyricGrid_->onGridResized = beatGrid_->onGridResized;

    depthLabel_ = new QLabel(QStringLiteral("Depth -"), this);
    depthLabel_->setObjectName(QStringLiteral("StatusPill"));
    underrunLabel_ = new QLabel(QStringLiteral("Underruns -"), this);
    underrunLabel_->setObjectName(QStringLiteral("StatusPill"));
    driftLabel_ = new QLabel(QStringLiteral("Drift -"), this);
    driftLabel_->setObjectName(QStringLiteral("StatusPill"));
    auto* footer = new QHBoxLayout();
    footer->addWidget(depthLabel_);
    footer->addWidget(underrunLabel_);
    footer->addWidget(driftLabel_);
    footer->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addLayout(header);
    layout->addLayout(library);
    layout->addWidget(buildSessionPage());
    layout->addWidget(tabs_, 1);

    logEdit_ = new QPlainTextEdit(this);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumBlockCount(2000);
    logEdit_->setMaximumHeight(150);
    layout->addWidget(logEdit_);
    layout->addLayout(footer);

    QTimer::singleShot(0, this, [this] {
        refreshDevices();
        refreshLoopbackSources();
    });
}

QWidget* MainWindow::buildSessionPage()
{
    auto* page = new QWidget(this);
    modeBox_ = new QComboBox(page);
    modeBox_->addItems({QStringLiteral("Listen"), QStringLiteral("Connect")});

    jam2PathEdit_ = new QLineEdit(SessionController::defaultJam2Path(), page);
    bindHostEdit_ = new QLineEdit(SessionController::defaultBindHost(), page);
    portSpin_ = new QSpinBox(page);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(49000);
    publicHostEdit_ = new QLineEdit(SessionController::defaultPublicHost(), page);
    connectUrlEdit_ = new QLineEdit(page);
    generatedUrlEdit_ = new QLineEdit(page);
    generatedUrlEdit_->setReadOnly(true);

    deviceBox_ = new QComboBox(page);
    deviceBox_->setEditable(true);
    inputChannelsEdit_ = new QLineEdit(QStringLiteral("1"), page);
    outputChannelsEdit_ = new QLineEdit(QStringLiteral("1,2"), page);
    sampleRateSpin_ = new QSpinBox(page);
    sampleRateSpin_->setRange(8000, 384000);
    sampleRateSpin_->setValue(44100);
    bufferSizeSpin_ = new QSpinBox(page);
    bufferSizeSpin_->setRange(16, 4096);
    bufferSizeSpin_->setValue(128);
    frameSizeSpin_ = new QSpinBox(page);
    frameSizeSpin_->setRange(32, 256);
    frameSizeSpin_->setValue(128);
    prefillSpin_ = new QSpinBox(page);
    prefillSpin_->setRange(0, 65536);
    prefillSpin_->setValue(1536);
    noStunCheck_ = new QCheckBox(QStringLiteral("No STUN"), page);
    metronomeCheck_ = new QCheckBox(QStringLiteral("Metronome"), page);
    metronomeCheck_->setChecked(true);
    bpmSpin_ = new QSpinBox(page);
    bpmSpin_->setRange(1, 400);
    bpmSpin_->setValue(120);
    metronomeLevelSlider_ = makeUnitSlider(0.2, page);
    remoteLevelSlider_ = makeUnitSlider(1.0, page);
    startButton_ = new QPushButton(QStringLiteral("Start Jam"), page);
    stopButton_ = new QPushButton(QStringLiteral("Stop"), page);
    stopButton_->setEnabled(false);

    auto* copyUrlButton = new QPushButton(QStringLiteral("Copy URL"), page);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh Devices"), page);
    auto* regenButton = new QPushButton(QStringLiteral("New Session"), page);

    jam2PathEdit_->setMinimumWidth(360);
    connectUrlEdit_->setMinimumWidth(420);
    generatedUrlEdit_->setMinimumWidth(420);
    deviceBox_->setEditable(false);
    deviceBox_->setMinimumWidth(280);

    auto* sessionForm = new QGridLayout();
    sessionForm->addWidget(new QLabel(QStringLiteral("Mode"), page), 0, 0);
    sessionForm->addWidget(modeBox_, 0, 1);
    sessionForm->addWidget(new QLabel(QStringLiteral("Bind"), page), 0, 2);
    sessionForm->addWidget(bindHostEdit_, 0, 3);
    sessionForm->addWidget(new QLabel(QStringLiteral("Port"), page), 0, 4);
    sessionForm->addWidget(portSpin_, 0, 5);
    sessionForm->addWidget(new QLabel(QStringLiteral("Public"), page), 0, 6);
    sessionForm->addWidget(publicHostEdit_, 0, 7);
    sessionForm->addWidget(new QLabel(QStringLiteral("Connect URL"), page), 1, 0);
    sessionForm->addWidget(connectUrlEdit_, 1, 1, 1, 4);
    sessionForm->addWidget(new QLabel(QStringLiteral("Generated URL"), page), 1, 5);
    sessionForm->addWidget(generatedUrlEdit_, 1, 6, 1, 4);
    sessionForm->addWidget(new QLabel(QStringLiteral("Binary"), page), 2, 0);
    sessionForm->addWidget(jam2PathEdit_, 2, 1, 1, 9);

    auto* audioForm = new QGridLayout();
    audioForm->addWidget(new QLabel(QStringLiteral("Device"), page), 0, 0);
    audioForm->addWidget(deviceBox_, 0, 1, 1, 3);
    audioForm->addWidget(new QLabel(QStringLiteral("In"), page), 0, 4);
    audioForm->addWidget(inputChannelsEdit_, 0, 5);
    audioForm->addWidget(new QLabel(QStringLiteral("Out"), page), 0, 6);
    audioForm->addWidget(outputChannelsEdit_, 0, 7);
    audioForm->addWidget(new QLabel(QStringLiteral("Rate"), page), 1, 0);
    audioForm->addWidget(sampleRateSpin_, 1, 1);
    audioForm->addWidget(new QLabel(QStringLiteral("Buffer"), page), 1, 2);
    audioForm->addWidget(bufferSizeSpin_, 1, 3);
    audioForm->addWidget(new QLabel(QStringLiteral("Frame"), page), 1, 4);
    audioForm->addWidget(frameSizeSpin_, 1, 5);
    audioForm->addWidget(new QLabel(QStringLiteral("Prefill"), page), 1, 6);
    audioForm->addWidget(prefillSpin_, 1, 7);

    auto* runtimeLayout = new QGridLayout();
    runtimeLayout->addWidget(metronomeCheck_, 0, 0);
    runtimeLayout->addWidget(new QLabel(QStringLiteral("BPM"), page), 0, 1);
    runtimeLayout->addWidget(bpmSpin_, 0, 2);
    runtimeLayout->addWidget(new QLabel(QStringLiteral("Metronome"), page), 0, 3);
    runtimeLayout->addWidget(metronomeLevelSlider_, 0, 4);
    runtimeLayout->addWidget(new QLabel(QStringLiteral("Remote"), page), 0, 5);
    runtimeLayout->addWidget(remoteLevelSlider_, 0, 6);

    auto* sessionBox = new QGroupBox(QStringLiteral("Connection"), page);
    sessionBox->setLayout(sessionForm);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), page);
    audioBox->setLayout(audioForm);
    auto* runtimeBox = new QGroupBox(QStringLiteral("Runtime Mix"), page);
    runtimeBox->setLayout(runtimeLayout);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(startButton_);
    buttons->addWidget(stopButton_);
    buttons->addWidget(refreshButton);
    buttons->addWidget(regenButton);
    buttons->addWidget(copyUrlButton);
    buttons->addWidget(noStunCheck_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(sessionBox);
    layout->addWidget(audioBox);
    layout->addWidget(runtimeBox);
    layout->addLayout(buttons);

    QObject::connect(startButton_, &QPushButton::clicked, this, [this] { startJam(); });
    QObject::connect(stopButton_, &QPushButton::clicked, this, [this] { stopJam(); });
    QObject::connect(refreshButton, &QPushButton::clicked, this, [this] { refreshDevices(); });
    QObject::connect(regenButton, &QPushButton::clicked, this, [this] {
        generateSession();
        generatedUrlEdit_->clear();
    });
    QObject::connect(copyUrlButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(generatedUrlEdit_->text());
    });
    QObject::connect(metronomeCheck_, &QCheckBox::toggled, this, [this] { updateRuntimeControls(); });
    QObject::connect(bpmSpin_, &QSpinBox::valueChanged, this, [this] { updateRuntimeControls(); });
    QObject::connect(metronomeLevelSlider_, &QSlider::valueChanged, this, [this] { updateRuntimeControls(); });
    QObject::connect(remoteLevelSlider_, &QSlider::valueChanged, this, [this] { updateRuntimeControls(); });

    return page;
}

QWidget* MainWindow::buildSongPage()
{
    auto* page = new QWidget(this);
    chordGrid_ = new BeatGridWidget(&songModel_, QStringLiteral("chord"), page);
    leadLabel_ = new QLabel(page);
    leadPendingLabel_ = new QLabel(page);
    updateLeadLabels();

    auto* leadButton = new QPushButton(QStringLiteral("Request Lead Swap"), page);
    QObject::connect(leadButton, &QPushButton::clicked, this, [this] { requestLeadSwap(); });

    auto* top = new QHBoxLayout();
    top->addWidget(leadLabel_);
    top->addWidget(leadPendingLabel_);
    top->addStretch(1);
    top->addWidget(leadButton);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(chordGrid_, 1);

    chordGrid_->onCellEdited = [this](int section, const QString& lane, int beat, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
        refreshSongViews();
    };
    chordGrid_->onGridResized = [this](int section, int beats, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beats"), beats},
        });
        refreshSongViews();
    };
    chordGrid_->onStructureChanged = [this] { refreshSongViews(); };

    return page;
}

QWidget* MainWindow::buildArrangementPage()
{
    auto* page = new QWidget(this);
    arrangementSectionBox_ = new QComboBox(page);
    arrangementEdit_ = new QTextEdit(page);
    arrangementEdit_->setReadOnly(true);
    auto* upButton = new QPushButton(QStringLiteral("Move Section Up"), page);
    auto* downButton = new QPushButton(QStringLiteral("Move Section Down"), page);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh Arrangement"), page);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(new QLabel(QStringLiteral("Section"), page));
    buttons->addWidget(arrangementSectionBox_);
    buttons->addWidget(upButton);
    buttons->addWidget(downButton);
    buttons->addWidget(refreshButton);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(buttons);
    layout->addWidget(arrangementEdit_, 1);

    QObject::connect(upButton, &QPushButton::clicked, this, [this] {
        const int current = arrangementSectionBox_->currentIndex();
        songModel_.moveSection(current, qMax(0, current - 1));
        refreshSongViews();
    });
    QObject::connect(downButton, &QPushButton::clicked, this, [this] {
        const int current = arrangementSectionBox_->currentIndex();
        songModel_.moveSection(current, qMin(songModel_.sections().size() - 1, current + 1));
        refreshSongViews();
    });
    QObject::connect(refreshButton, &QPushButton::clicked, this, [this] { refreshSongViews(); });
    refreshSongViews();
    return page;
}

QWidget* MainWindow::buildTrackPage()
{
    auto* page = new QWidget(this);
    trackNameLabel_ = new QLabel(QStringLiteral("Track: No track loaded"), page);
    trackAnalysisLabel_ = new QLabel(QStringLiteral("Analysis: Essentia not linked | Stretch: Signalsmith not linked"), page);

    auto* loadButton = new QPushButton(QStringLiteral("Load WAV"), page);
    auto* shareButton = new QPushButton(QStringLiteral("Share Metadata"), page);
    shareTrackFileButton_ = new QPushButton(QStringLiteral("Share WAV"), page);
    auto* analyzeButton = new QPushButton(QStringLiteral("Analyze"), page);
    analyzeButton->setEnabled(false);
    analyzeButton->setToolTip(QStringLiteral("Reserved for Essentia integration."));

    auto* speedSlider = makeUnitSlider(1.0, page);
    speedSlider->setRange(50, 100);
    auto* pitchSlider = new QSlider(Qt::Horizontal, page);
    pitchSlider->setRange(-1200, 1200);
    pitchSlider->setValue(0);
    pitchSlider->setToolTip(QStringLiteral("Stored and synced now; applied after the Signalsmith dependency is added."));
    auto* focusSlider = new QSlider(Qt::Horizontal, page);
    focusSlider->setRange(40, 8000);
    focusSlider->setValue(120);
    auto* syncBox = new QCheckBox(QStringLiteral("Sync track controls"), page);
    syncBox->setChecked(true);
    capturePathEdit_ = new QLineEdit(SessionController::defaultCapturePath(), page);
    captureOutputEdit_ = new QLineEdit(page);
    loopbackSourceBox_ = new QComboBox(page);
    loopbackSourceBox_->setEditable(true);
    loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
    captureDurationSpin_ = new QSpinBox(page);
    captureDurationSpin_->setRange(1000, 600000);
    captureDurationSpin_->setValue(30000);
    captureDurationSpin_->setSuffix(QStringLiteral(" ms"));
    captureTriggerCheck_ = new QCheckBox(QStringLiteral("Trigger on signal"), page);
    trimLeadingCheck_ = new QCheckBox(QStringLiteral("Trim leading silence"), page);
    trimTrailingCheck_ = new QCheckBox(QStringLiteral("Trim trailing silence"), page);
    trimLeadingCheck_->setChecked(true);
    trimTrailingCheck_->setChecked(true);
    triggerThresholdSpin_ = new QDoubleSpinBox(page);
    triggerThresholdSpin_->setRange(-120.0, 0.0);
    triggerThresholdSpin_->setDecimals(1);
    triggerThresholdSpin_->setValue(-45.0);
    triggerThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    tailThresholdSpin_ = new QDoubleSpinBox(page);
    tailThresholdSpin_->setRange(-120.0, 0.0);
    tailThresholdSpin_->setDecimals(1);
    tailThresholdSpin_->setValue(-50.0);
    tailThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    preRollSpin_ = new QSpinBox(page);
    preRollSpin_->setRange(0, 10000);
    preRollSpin_->setValue(250);
    preRollSpin_->setSuffix(QStringLiteral(" ms"));
    triggerHoldSpin_ = new QSpinBox(page);
    triggerHoldSpin_->setRange(1, 5000);
    triggerHoldSpin_->setValue(50);
    triggerHoldSpin_->setSuffix(QStringLiteral(" ms"));
    tailSilenceSpin_ = new QSpinBox(page);
    tailSilenceSpin_->setRange(0, 30000);
    tailSilenceSpin_->setValue(1000);
    tailSilenceSpin_->setSuffix(QStringLiteral(" ms"));
    captureProgressLabel_ = new QLabel(QStringLiteral("Capture idle"), page);
    trackPlaybackLabel_ = new QLabel(QStringLiteral("Track player idle"), page);
    playTrackButton_ = new QPushButton(QStringLiteral("Play Track"), page);
    stopTrackButton_ = new QPushButton(QStringLiteral("Stop Track"), page);
    trackPositionSlider_ = new QSlider(Qt::Horizontal, page);
    trackPositionSlider_->setRange(0, 0);
    trackLevelSlider_ = makeUnitSlider(0.75, page);
    captureButton_ = new QPushButton(QStringLiteral("Record Input"), page);
    loopbackCaptureButton_ = new QPushButton(QStringLiteral("Record Loopback"), page);
    stopCaptureButton_ = new QPushButton(QStringLiteral("Stop Capture"), page);
    stopCaptureButton_->setEnabled(false);
    importCaptureButton_ = new QPushButton(QStringLiteral("Import Capture"), page);
    importCaptureButton_->setEnabled(false);

    QDir captureDir(QDir::current());
    captureDir.mkpath(QStringLiteral("captures"));
    captureOutputEdit_->setText(captureDir.absoluteFilePath(QStringLiteral("captures/capture.wav")));
    auto* browseCaptureOutputButton = new QPushButton(QStringLiteral("Browse"), page);
    auto* refreshLoopbackButton = new QPushButton(QStringLiteral("Refresh Sources"), page);

    auto* form = new QFormLayout();
    form->addRow(trackNameLabel_);
    form->addRow(QStringLiteral("Speed"), speedSlider);
    form->addRow(QStringLiteral("Pitch cents"), pitchSlider);
    form->addRow(QStringLiteral("Track position"), trackPositionSlider_);
    form->addRow(QStringLiteral("Track level"), trackLevelSlider_);
    form->addRow(QStringLiteral("Focus frequency"), focusSlider);
    form->addRow(syncBox);
    form->addRow(trackAnalysisLabel_);
    form->addRow(QStringLiteral("jam2-capture"), capturePathEdit_);
    auto* captureOutputLayout = new QHBoxLayout();
    captureOutputLayout->addWidget(captureOutputEdit_, 1);
    captureOutputLayout->addWidget(browseCaptureOutputButton);
    form->addRow(QStringLiteral("Capture output"), captureOutputLayout);
    auto* loopbackSourceLayout = new QHBoxLayout();
    loopbackSourceLayout->addWidget(loopbackSourceBox_, 1);
    loopbackSourceLayout->addWidget(refreshLoopbackButton);
    form->addRow(QStringLiteral("Loopback source"), loopbackSourceLayout);
    form->addRow(QStringLiteral("Capture duration"), captureDurationSpin_);
    form->addRow(captureTriggerCheck_);
    form->addRow(QStringLiteral("Trigger threshold"), triggerThresholdSpin_);
    form->addRow(QStringLiteral("Trigger hold"), triggerHoldSpin_);
    form->addRow(QStringLiteral("Pre-roll"), preRollSpin_);
    form->addRow(QStringLiteral("Tail threshold"), tailThresholdSpin_);
    form->addRow(QStringLiteral("Tail silence"), tailSilenceSpin_);
    form->addRow(trimLeadingCheck_);
    form->addRow(trimTrailingCheck_);
    form->addRow(QStringLiteral("Capture status"), captureProgressLabel_);
    form->addRow(QStringLiteral("Playback status"), trackPlaybackLabel_);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(loadButton);
    buttons->addWidget(shareButton);
    buttons->addWidget(shareTrackFileButton_);
    buttons->addWidget(analyzeButton);
    buttons->addWidget(playTrackButton_);
    buttons->addWidget(stopTrackButton_);
    buttons->addWidget(captureButton_);
    buttons->addWidget(loopbackCaptureButton_);
    buttons->addWidget(stopCaptureButton_);
    buttons->addWidget(importCaptureButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(buttons);
    layout->addLayout(form);
    layout->addStretch(1);

    QObject::connect(loadButton, &QPushButton::clicked, this, [this] { loadTrackMetadata(); });
    QObject::connect(shareButton, &QPushButton::clicked, this, [this] {
        sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
    });
    QObject::connect(shareTrackFileButton_, &QPushButton::clicked, this, [this] { sendTrackFile(); });
    QObject::connect(speedSlider, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().speed = static_cast<double>(value) / 100.0;
        trackPlayer_.setPlaybackRate(trackController_.model().speed);
        sendControl(trackController_.processingMessage());
    });
    QObject::connect(pitchSlider, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().pitchCents = value;
        sendControl(trackController_.processingMessage());
    });
    QObject::connect(playTrackButton_, &QPushButton::clicked, this, [this] { playTrack(); });
    QObject::connect(stopTrackButton_, &QPushButton::clicked, this, [this] { stopTrack(); });
    QObject::connect(trackPositionSlider_, &QSlider::sliderReleased, this, [this] {
        trackPlayer_.setPosition(trackPositionSlider_->value());
    });
    QObject::connect(trackLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        trackAudio_.setVolume(static_cast<double>(value) / 100.0);
    });
    QObject::connect(focusSlider, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().focusFrequencyHz = value;
        sendControl(trackController_.processingMessage());
    });
    QObject::connect(syncBox, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().syncControls = checked;
    });
    QObject::connect(captureButton_, &QPushButton::clicked, this, [this] { startInputCapture(); });
    QObject::connect(loopbackCaptureButton_, &QPushButton::clicked, this, [this] { startLoopbackCapture(); });
    QObject::connect(stopCaptureButton_, &QPushButton::clicked, this, [this] { stopInputCapture(); });
    QObject::connect(importCaptureButton_, &QPushButton::clicked, this, [this] { importLastCapture(); });
    QObject::connect(browseCaptureOutputButton, &QPushButton::clicked, this, [this] { chooseCaptureFolder(); });
    QObject::connect(refreshLoopbackButton, &QPushButton::clicked, this, [this] { refreshLoopbackSources(); });

    return page;
}

QWidget* MainWindow::buildStatsPage()
{
    auto* page = new QWidget(this);
    auto* statsTable = new QTableWidget(6, 2, page);
    statsTable->setHorizontalHeaderLabels({QStringLiteral("Metric"), QStringLiteral("Current")});
    statsTable->verticalHeader()->setVisible(false);
    statsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    statsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QStringList metrics{
        QStringLiteral("RTT"),
        QStringLiteral("Jitter"),
        QStringLiteral("Packet loss"),
        QStringLiteral("Playback depth"),
        QStringLiteral("Missing frames"),
        QStringLiteral("Drift"),
    };
    for (int row = 0; row < metrics.size(); ++row) {
        statsTable->setItem(row, 0, new QTableWidgetItem(metrics[row]));
        statsTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("-")));
    }

    auto* layout = new QVBoxLayout(page);
    layout->addWidget(statsTable, 1);
    return page;
}

void MainWindow::startJam()
{
    if (jam2_.isRunning()) {
        return;
    }
    QStringList args;
    const bool listenMode = modeBox_->currentText() == QStringLiteral("Listen");
    try {
        if (listenMode) {
            jam2::SessionInfo info;
            info.endpoint = {publicHostEdit_->text().toStdString(), static_cast<std::uint16_t>(portSpin_->value())};
            info.session_id = sessionId_;
            info.key = sessionKey_;
            generatedUrlEdit_->setText(QString::fromStdString(jam2::make_jam_url(info)));
            args << QStringLiteral("listen")
                 << QStringLiteral("--bind") << QStringLiteral("%1:%2").arg(bindHostEdit_->text()).arg(portSpin_->value())
                 << QStringLiteral("--public-endpoint") << QStringLiteral("%1:%2").arg(publicHostEdit_->text()).arg(portSpin_->value())
                 << QStringLiteral("--session-id") << sessionHex()
                 << QStringLiteral("--session-key") << keyHex();
            if (!controlServer_.listen(static_cast<quint16>(portSpin_->value()), sessionHex(), keyHex())) {
                appendLog(QStringLiteral("control server failed: ") + controlServer_.errorString());
            }
        } else {
            const std::string url = connectUrlEdit_->text().toStdString();
            const jam2::SessionInfo info = jam2::parse_jam_url(url);
            sessionId_ = info.session_id;
            sessionKey_ = info.key;
            args << QStringLiteral("connect") << connectUrlEdit_->text();
            controlClient_.connectToHost(QString::fromStdString(info.endpoint.host), info.endpoint.port, sessionHex(), keyHex());
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QString::fromUtf8(error.what()));
        return;
    }

    args << commonJamArgs();
    jam2_.start(jam2PathEdit_->text(), args);
    appendLog(QStringLiteral("starting: %1 %2").arg(jam2PathEdit_->text(), args.join(QLatin1Char(' '))));
    startButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    connectionLabel_->setText(QStringLiteral("Starting"));
}

void MainWindow::stopJam()
{
    controlServer_.close();
    controlClient_.close();
    jam2_.stop();
    startButton_->setEnabled(true);
    stopButton_->setEnabled(false);
}

void MainWindow::refreshDevices()
{
    QProcess process;
    const QFileInfo binary(jam2PathEdit_->text());
    if (binary.exists()) {
        process.setWorkingDirectory(binary.absolutePath());
    }
    process.start(jam2PathEdit_->text(), QStringList{QStringLiteral("list-devices")});
    if (!process.waitForStarted(3000)) {
        appendLog(QStringLiteral("device refresh failed to start: %1").arg(process.errorString()));
        return;
    }
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("device refresh timed out"));
        process.kill();
        return;
    }
    deviceBox_->clear();
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('['))) {
            deviceBox_->addItem(trimmed, deviceId(trimmed));
        }
    }
    if (deviceBox_->count() == 0) {
        appendLog(error.isEmpty() ? QStringLiteral("no devices returned by jam2 list-devices") : error);
    } else {
        appendLog(QStringLiteral("loaded %1 audio devices").arg(deviceBox_->count()));
    }
}

void MainWindow::appendLog(const QString& line)
{
    if (logEdit_) {
        logEdit_->appendPlainText(line);
    }
}

void MainWindow::handleStatus(const QJsonObject& status)
{
    if (status.value(QStringLiteral("event")).toString() != QStringLiteral("status")) {
        return;
    }
    rttLabel_->setText(QStringLiteral("RTT ") + doubleText(status, QStringLiteral("rtt_avg_ms"), QStringLiteral(" ms")));
    jitterLabel_->setText(QStringLiteral("Jitter ") + doubleText(status, QStringLiteral("jitter_avg_ms"), QStringLiteral(" ms")));
    lossLabel_->setText(QStringLiteral("Loss ") + doubleText(status, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2));
    depthLabel_->setText(QStringLiteral("Depth ") + doubleText(status, QStringLiteral("playback_depth_ms"), QStringLiteral(" ms")));
    underrunLabel_->setText(QStringLiteral("Missing frames %1").arg(status.value(QStringLiteral("missing_audio_frames_inserted")).toInteger()));
    driftLabel_->setText(QStringLiteral("Drift ") + doubleText(status, QStringLiteral("drift_ppm"), QStringLiteral(" ppm")));
}

void MainWindow::handleControlMessage(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("beat.set")) {
        songModel_.setCell(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("lane")).toString(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("text")).toString());
        refreshSongViews();
    } else if (type == QStringLiteral("grid.resize")) {
        songModel_.resizeSection(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beats")).toInt(8));
        refreshSongViews();
    } else if (type == QStringLiteral("lead.change_pending")) {
        leadCue_.pendingLead = message.value(QStringLiteral("target")).toString();
        leadCue_.pendingBars = message.value(QStringLiteral("bars")).toInt(4);
        updateLeadLabels();
    } else if (type == QStringLiteral("track.offer")) {
        trackController_.model().fileName = message.value(QStringLiteral("name")).toString();
        trackController_.model().filePath = message.value(QStringLiteral("path")).toString(trackController_.model().filePath);
        trackController_.model().fileBytes = static_cast<qint64>(message.value(QStringLiteral("file_bytes")).toDouble(trackController_.model().fileBytes));
        trackController_.model().sampleRate = message.value(QStringLiteral("sample_rate")).toInt(trackController_.model().sampleRate);
        trackController_.model().durationMs = message.value(QStringLiteral("duration_ms")).toInt(trackController_.model().durationMs);
        trackController_.model().sha256 = message.value(QStringLiteral("sha256")).toString(trackController_.model().sha256);
        trackController_.model().acceptedBpm = message.value(QStringLiteral("accepted_bpm")).toDouble(120.0);
        trackController_.model().key = message.value(QStringLiteral("key")).toString(QStringLiteral("Unknown"));
        updateTrackControls();
        loadTrackIntoPlayer();
    } else if (type == QStringLiteral("track.processing")) {
        trackController_.applyProcessingMessage(message);
        trackPlayer_.setPlaybackRate(trackController_.model().speed);
        updateTrackControls();
    } else if (type == QStringLiteral("track.play")) {
        if (trackPlayer_.source().isEmpty()) {
            loadTrackIntoPlayer();
        }
        trackPlayer_.setPlaybackRate(message.value(QStringLiteral("speed")).toDouble(trackController_.model().speed));
        trackPlayer_.setPosition(message.value(QStringLiteral("position_ms")).toInteger(0));
        trackPlayer_.play();
    } else if (type == QStringLiteral("track.stop")) {
        trackPlayer_.stop();
    } else if (type == QStringLiteral("track.file.start")) {
        receiveTrackFileStart(message);
    } else if (type == QStringLiteral("track.file.chunk")) {
        receiveTrackFileChunk(message);
    } else if (type == QStringLiteral("track.file.done")) {
        receiveTrackFileDone(message);
    }
}

void MainWindow::sendControl(const QJsonObject& message)
{
    controlServer_.send(message);
    controlClient_.send(message);
}

void MainWindow::updateRuntimeControls()
{
    if (!jam2_.isRunning()) {
        return;
    }
    jam2_.sendLine(metronomeCheck_->isChecked() ? QStringLiteral("metro on") : QStringLiteral("metro off"));
    jam2_.sendLine(QStringLiteral("bpm %1").arg(bpmSpin_->value()));
    jam2_.sendLine(QStringLiteral("metro level %1").arg(static_cast<double>(metronomeLevelSlider_->value()) / 100.0, 0, 'f', 2));
    jam2_.sendLine(QStringLiteral("remote level %1").arg(static_cast<double>(remoteLevelSlider_->value()) / 100.0, 0, 'f', 2));
}

void MainWindow::requestLeadSwap()
{
    const QString target = leadCue_.currentLead == QStringLiteral("Teacher") ? QStringLiteral("Student") : QStringLiteral("Teacher");
    leadCue_.pendingLead = target;
    leadCue_.pendingBars = 4;
    updateLeadLabels();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("lead.change_pending")},
        {QStringLiteral("target"), target},
        {QStringLiteral("bars"), leadCue_.pendingBars},
    });
    QTimer::singleShot(4000, this, [this, target] {
        leadCue_.currentLead = target;
        leadCue_.pendingLead.clear();
        leadCue_.pendingBars = 0;
        updateLeadLabels();
    });
}

void MainWindow::updateLeadLabels()
{
    if (!leadLabel_) {
        return;
    }
    leadLabel_->setText(QStringLiteral("Lead: %1").arg(leadCue_.currentLead));
    leadPendingLabel_->setText(
        leadCue_.pendingLead.isEmpty()
            ? QStringLiteral("No pending lead change")
            : QStringLiteral("%1 leads in %2 bars").arg(leadCue_.pendingLead).arg(leadCue_.pendingBars));
}

void MainWindow::updateTrackControls()
{
    const auto& model = trackController_.model();
    const QString duration = model.durationMs > 0
        ? QStringLiteral(" | %1.%2 s").arg(model.durationMs / 1000).arg((model.durationMs % 1000) / 100)
        : QString();
    const QString rate = model.sampleRate > 0 ? QStringLiteral(" | %1 Hz").arg(model.sampleRate) : QString();
    const QString bytes = model.fileBytes > 0 ? QStringLiteral(" | %1 bytes").arg(model.fileBytes) : QString();
    trackNameLabel_->setText(QStringLiteral("Track: %1 | BPM %2 | Key %3%4%5%6")
        .arg(model.fileName)
        .arg(model.acceptedBpm, 0, 'f', 1)
        .arg(model.key)
        .arg(duration)
        .arg(rate)
        .arg(bytes));
}

void MainWindow::loadTrackMetadata()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load WAV"), QString(), QStringLiteral("WAV files (*.wav);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFileInfo info(path);
    try {
        const WavMetadata metadata = readWavMetadata(path);
        const QJsonObject sidecar = readSidecarJson(path);
        trackController_.model().fileName = info.fileName();
        trackController_.model().filePath = info.absoluteFilePath();
        trackController_.model().fileBytes = info.size();
        trackController_.model().sampleRate = sidecar.value(QStringLiteral("sample_rate")).toInt(metadata.sampleRate);
        trackController_.model().durationMs = sidecar.value(QStringLiteral("duration_ms")).toInt(metadata.durationMs);
        trackController_.model().sha256 = metadata.sha256;
        trackController_.model().guessedBpm = 0.0;
        trackController_.model().acceptedBpm = bpmSpin_->value();
        trackController_.model().key = QStringLiteral("Unknown");
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QString::fromUtf8(error.what()));
        return;
    }
    updateTrackControls();
    loadTrackIntoPlayer();
}

QStringList MainWindow::captureOptionArgs() const
{
    return QStringList{
        QStringLiteral("--trigger"), onOff(captureTriggerCheck_ && captureTriggerCheck_->isChecked()),
        QStringLiteral("--trigger-threshold-db"), QString::number(triggerThresholdSpin_ ? triggerThresholdSpin_->value() : -45.0, 'f', 1),
        QStringLiteral("--trigger-hold-ms"), QString::number(triggerHoldSpin_ ? triggerHoldSpin_->value() : 50),
        QStringLiteral("--pre-roll-ms"), QString::number(preRollSpin_ ? preRollSpin_->value() : 250),
        QStringLiteral("--tail-silence-db"), QString::number(tailThresholdSpin_ ? tailThresholdSpin_->value() : -50.0, 'f', 1),
        QStringLiteral("--tail-silence-ms"), QString::number(tailSilenceSpin_ ? tailSilenceSpin_->value() : 1000),
        QStringLiteral("--trim-leading-silence"), onOff(trimLeadingCheck_ && trimLeadingCheck_->isChecked()),
        QStringLiteral("--trim-trailing-silence"), onOff(trimTrailingCheck_ && trimTrailingCheck_->isChecked()),
        QStringLiteral("--summary-json"), QStringLiteral("on"),
    };
}

void MainWindow::handleCaptureOutputLine(const QString& line)
{
    appendLog(QStringLiteral("capture: ") + line);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }
    const QJsonObject object = doc.object();
    if (object.value(QStringLiteral("event")).toString() != QStringLiteral("capture.summary")) {
        return;
    }
    lastCaptureSummary_ = object;
    const QString output = object.value(QStringLiteral("output")).toString();
    if (!output.isEmpty()) {
        lastCapturePath_ = output;
        captureOutputEdit_->setText(output);
    }
    if (captureProgressLabel_) {
        captureProgressLabel_->setText(QStringLiteral("%1 frames | peak %2 | trimmed %3/%4")
            .arg(object.value(QStringLiteral("frames_written")).toInteger())
            .arg(object.value(QStringLiteral("peak")).toDouble(), 0, 'f', 4)
            .arg(object.value(QStringLiteral("trimmed_leading_frames")).toInteger())
            .arg(object.value(QStringLiteral("trimmed_trailing_frames")).toInteger()));
    }
    if (importCaptureButton_) {
        importCaptureButton_->setEnabled(QFileInfo::exists(lastCapturePath_));
    }
}

void MainWindow::chooseCaptureFolder()
{
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Capture Output"),
        captureOutputEdit_->text(),
        QStringLiteral("WAV files (*.wav);;All files (*)"));
    if (!path.isEmpty()) {
        captureOutputEdit_->setText(path);
    }
}

void MainWindow::refreshLoopbackSources()
{
    QProcess process;
    const QFileInfo binary(capturePathEdit_->text());
    if (binary.exists()) {
        process.setWorkingDirectory(binary.absolutePath());
    }
    process.start(capturePathEdit_->text(), QStringList{QStringLiteral("list-loopback-sources")});
    if (!process.waitForStarted(3000)) {
        appendLog(QStringLiteral("loopback source refresh failed to start: %1").arg(process.errorString()));
        return;
    }
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("loopback source refresh timed out"));
        process.kill();
        return;
    }

    const QString previous = loopbackSourceBox_->currentData().toString().isEmpty()
        ? loopbackSourceBox_->currentText()
        : loopbackSourceBox_->currentData().toString();
    loopbackSourceBox_->clear();
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QRegularExpression re(QStringLiteral("^\\s*\\[([^\\]]+)\\]\\s*(.*)$"));
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        const QRegularExpressionMatch match = re.match(trimmed);
        if (match.hasMatch()) {
            loopbackSourceBox_->addItem(trimmed, match.captured(1));
        }
    }
    if (loopbackSourceBox_->count() == 0) {
        loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
        const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        appendLog(error.isEmpty() ? QStringLiteral("no loopback sources returned") : error);
    } else {
        appendLog(QStringLiteral("loaded %1 loopback sources").arg(loopbackSourceBox_->count()));
    }
    const int restore = loopbackSourceBox_->findData(previous);
    if (restore >= 0) {
        loopbackSourceBox_->setCurrentIndex(restore);
    }
}

void MainWindow::startInputCapture()
{
    if (captureProcess_.state() != QProcess::NotRunning) {
        return;
    }
    if (selectedDeviceId().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Capture"), QStringLiteral("Select an audio device first."));
        return;
    }
    QDir captureDir(QDir::current());
    captureDir.mkpath(QStringLiteral("captures"));
    QString output = captureOutputEdit_->text().trimmed();
    if (output.isEmpty() || output.endsWith(QStringLiteral("capture.wav"))) {
        output = captureDir.absoluteFilePath(
            QStringLiteral("captures/capture-%1.wav").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
        captureOutputEdit_->setText(output);
    }
    lastCapturePath_ = output;
    lastCaptureSummary_ = QJsonObject{};
    QStringList args{
        QStringLiteral("record-input"),
        QStringLiteral("--audio-device"), selectedDeviceId(),
        QStringLiteral("--input-channels"), inputChannelsEdit_->text(),
        QStringLiteral("--sample-rate"), QString::number(sampleRateSpin_->value()),
        QStringLiteral("--buffer-size"), QString::number(bufferSizeSpin_->value()),
        QStringLiteral("--duration-ms"), QString::number(captureDurationSpin_->value()),
        QStringLiteral("--output"), output,
    };
    args << captureOptionArgs();
    const QFileInfo binary(capturePathEdit_->text());
    if (binary.exists()) {
        captureProcess_.setWorkingDirectory(binary.absolutePath());
    }
    appendLog(QStringLiteral("starting capture: %1 %2").arg(capturePathEdit_->text(), args.join(QLatin1Char(' '))));
    captureProcess_.start(capturePathEdit_->text(), args);
    if (!captureProcess_.waitForStarted(3000)) {
        appendLog(QStringLiteral("capture failed to start: %1").arg(captureProcess_.errorString()));
        return;
    }
    captureButton_->setEnabled(false);
    loopbackCaptureButton_->setEnabled(false);
    stopCaptureButton_->setEnabled(true);
    importCaptureButton_->setEnabled(false);
    captureProgressLabel_->setText(QStringLiteral("Capturing input"));
}

void MainWindow::startLoopbackCapture()
{
    if (captureProcess_.state() != QProcess::NotRunning) {
        return;
    }
    QDir captureDir(QDir::current());
    captureDir.mkpath(QStringLiteral("captures"));
    QString output = captureOutputEdit_->text().trimmed();
    if (output.isEmpty() || output.endsWith(QStringLiteral("capture.wav"))) {
        output = captureDir.absoluteFilePath(
            QStringLiteral("captures/loopback-%1.wav").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
        captureOutputEdit_->setText(output);
    }
    lastCapturePath_ = output;
    lastCaptureSummary_ = QJsonObject{};
    QString source = loopbackSourceBox_->currentData().toString();
    if (source.isEmpty()) {
        source = loopbackSourceBox_->currentText().trimmed();
    }
    if (source.isEmpty()) {
        source = QStringLiteral("default");
    }
    QStringList args{
        QStringLiteral("record-loopback"),
        QStringLiteral("--source"), source,
        QStringLiteral("--duration-ms"), QString::number(captureDurationSpin_->value()),
        QStringLiteral("--output"), output,
    };
    args << captureOptionArgs();
    const QFileInfo binary(capturePathEdit_->text());
    if (binary.exists()) {
        captureProcess_.setWorkingDirectory(binary.absolutePath());
    }
    appendLog(QStringLiteral("starting loopback capture: %1 %2").arg(capturePathEdit_->text(), args.join(QLatin1Char(' '))));
    captureProcess_.start(capturePathEdit_->text(), args);
    if (!captureProcess_.waitForStarted(3000)) {
        appendLog(QStringLiteral("loopback capture failed to start: %1").arg(captureProcess_.errorString()));
        return;
    }
    captureButton_->setEnabled(false);
    loopbackCaptureButton_->setEnabled(false);
    stopCaptureButton_->setEnabled(true);
    importCaptureButton_->setEnabled(false);
    captureProgressLabel_->setText(QStringLiteral("Capturing loopback"));
}

void MainWindow::stopInputCapture()
{
    if (captureProcess_.state() == QProcess::NotRunning) {
        return;
    }
    captureProcess_.write("stop\n");
    captureProcess_.closeWriteChannel();
    if (!captureProcess_.waitForFinished(3000)) {
        captureProcess_.terminate();
    }
    if (captureProcess_.state() != QProcess::NotRunning && !captureProcess_.waitForFinished(1500)) {
        captureProcess_.kill();
    }
}

void MainWindow::importLastCapture()
{
    if (!QFileInfo::exists(lastCapturePath_)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Capture"), QStringLiteral("No captured WAV is available to import."));
        return;
    }
    QFileInfo info(lastCapturePath_);
    try {
        const WavMetadata metadata = readWavMetadata(lastCapturePath_);
        const QJsonObject sidecar = readSidecarJson(lastCapturePath_);
        trackController_.model().fileName = info.fileName();
        trackController_.model().filePath = info.absoluteFilePath();
        trackController_.model().fileBytes = info.size();
        trackController_.model().sampleRate = sidecar.value(QStringLiteral("sample_rate")).toInt(metadata.sampleRate);
        trackController_.model().durationMs = lastCaptureSummary_.value(QStringLiteral("duration_ms")).toInt(
            sidecar.value(QStringLiteral("duration_ms")).toInt(metadata.durationMs));
        trackController_.model().sha256 = metadata.sha256;
        trackController_.model().acceptedBpm = bpmSpin_->value();
        trackController_.model().key = QStringLiteral("Unknown");
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Capture"), QString::fromUtf8(error.what()));
        return;
    }
    updateTrackControls();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.offer")},
        {QStringLiteral("name"), trackController_.model().fileName},
        {QStringLiteral("path"), trackController_.model().filePath},
        {QStringLiteral("file_bytes"), trackController_.model().fileBytes},
        {QStringLiteral("sample_rate"), trackController_.model().sampleRate},
        {QStringLiteral("duration_ms"), trackController_.model().durationMs},
        {QStringLiteral("sha256"), trackController_.model().sha256},
        {QStringLiteral("accepted_bpm"), trackController_.model().acceptedBpm},
        {QStringLiteral("key"), trackController_.model().key},
    });
    loadTrackIntoPlayer();
}

void MainWindow::loadTrackIntoPlayer()
{
    const QString path = trackController_.model().filePath;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (trackPlaybackLabel_) {
            trackPlaybackLabel_->setText(QStringLiteral("No local track file"));
        }
        return;
    }
    trackPlayer_.setSource(QUrl::fromLocalFile(path));
    trackPlayer_.setPlaybackRate(trackController_.model().speed);
    if (trackPlaybackLabel_) {
        trackPlaybackLabel_->setText(QStringLiteral("Track loaded locally"));
    }
}

void MainWindow::playTrack()
{
    if (trackPlayer_.source().isEmpty()) {
        loadTrackIntoPlayer();
    }
    if (trackPlayer_.source().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("No local WAV is loaded."));
        return;
    }
    trackPlayer_.play();
    if (trackController_.model().syncControls) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("track.play")},
            {QStringLiteral("position_ms"), trackPlayer_.position()},
            {QStringLiteral("speed"), trackController_.model().speed},
        });
    }
}

void MainWindow::stopTrack()
{
    trackPlayer_.stop();
    if (trackController_.model().syncControls) {
        sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("track.stop")}});
    }
}

QJsonObject MainWindow::trackMetadataMessage(const QString& type) const
{
    return QJsonObject{
        {QStringLiteral("type"), type},
        {QStringLiteral("name"), trackController_.model().fileName},
        {QStringLiteral("path"), trackController_.model().filePath},
        {QStringLiteral("file_bytes"), trackController_.model().fileBytes},
        {QStringLiteral("sample_rate"), trackController_.model().sampleRate},
        {QStringLiteral("duration_ms"), trackController_.model().durationMs},
        {QStringLiteral("sha256"), trackController_.model().sha256},
        {QStringLiteral("accepted_bpm"), trackController_.model().acceptedBpm},
        {QStringLiteral("key"), trackController_.model().key},
    };
}

void MainWindow::sendTrackFile()
{
    const QString path = trackController_.model().filePath;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("No local WAV is available to share."));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Could not open track WAV for sharing."));
        return;
    }
    const QByteArray bytes = file.readAll();
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    constexpr int chunkSize = 48 * 1024;
    sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.file.start")},
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("file_bytes"), bytes.size()},
        {QStringLiteral("sha256"), sha},
        {QStringLiteral("chunk_size"), chunkSize},
    });
    for (int offset = 0, index = 0; offset < bytes.size(); offset += chunkSize, ++index) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("track.file.chunk")},
            {QStringLiteral("index"), index},
            {QStringLiteral("data"), QString::fromLatin1(bytes.mid(offset, chunkSize).toBase64())},
        });
    }
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.file.done")},
        {QStringLiteral("chunks"), (bytes.size() + chunkSize - 1) / chunkSize},
        {QStringLiteral("sha256"), sha},
    });
    appendLog(QStringLiteral("shared track WAV: %1 bytes").arg(bytes.size()));
}

void MainWindow::receiveTrackFileStart(const QJsonObject& message)
{
    incomingTrackBytes_.clear();
    incomingTrackName_ = QFileInfo(message.value(QStringLiteral("name")).toString(QStringLiteral("track.wav"))).fileName();
    incomingTrackSha256_ = message.value(QStringLiteral("sha256")).toString();
    incomingTrackBytesExpected_ = static_cast<qint64>(message.value(QStringLiteral("file_bytes")).toDouble());
    incomingTrackNextChunk_ = 0;
    incomingTrackBytes_.reserve(static_cast<int>(qMin<qint64>(incomingTrackBytesExpected_, std::numeric_limits<int>::max())));
    appendLog(QStringLiteral("receiving track WAV: %1 bytes").arg(incomingTrackBytesExpected_));
}

void MainWindow::receiveTrackFileChunk(const QJsonObject& message)
{
    const int index = message.value(QStringLiteral("index")).toInt(-1);
    if (index != incomingTrackNextChunk_) {
        appendLog(QStringLiteral("track WAV chunk out of order: got %1 expected %2").arg(index).arg(incomingTrackNextChunk_));
        incomingTrackBytes_.clear();
        incomingTrackNextChunk_ = 0;
        return;
    }
    incomingTrackBytes_.append(QByteArray::fromBase64(message.value(QStringLiteral("data")).toString().toLatin1()));
    ++incomingTrackNextChunk_;
}

void MainWindow::receiveTrackFileDone(const QJsonObject& message)
{
    const QString expectedSha = message.value(QStringLiteral("sha256")).toString(incomingTrackSha256_);
    const QString actualSha = QString::fromLatin1(QCryptographicHash::hash(incomingTrackBytes_, QCryptographicHash::Sha256).toHex());
    if (incomingTrackBytesExpected_ != incomingTrackBytes_.size() || (!expectedSha.isEmpty() && expectedSha != actualSha)) {
        appendLog(QStringLiteral("received track WAV failed verification"));
        incomingTrackBytes_.clear();
        return;
    }
    QDir dir(QDir::current());
    dir.mkpath(QStringLiteral("received_tracks"));
    const QString output = dir.absoluteFilePath(QStringLiteral("received_tracks/%1").arg(incomingTrackName_.isEmpty() ? QStringLiteral("track.wav") : incomingTrackName_));
    QFile file(output);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("failed to write received track WAV: ") + output);
        return;
    }
    file.write(incomingTrackBytes_);
    file.close();

    try {
        const WavMetadata metadata = readWavMetadata(output);
        QFileInfo info(output);
        trackController_.model().fileName = info.fileName();
        trackController_.model().filePath = info.absoluteFilePath();
        trackController_.model().fileBytes = info.size();
        trackController_.model().sampleRate = metadata.sampleRate;
        trackController_.model().durationMs = metadata.durationMs;
        trackController_.model().sha256 = metadata.sha256;
        updateTrackControls();
        loadTrackIntoPlayer();
        appendLog(QStringLiteral("received track WAV: ") + output);
    } catch (const std::exception& error) {
        appendLog(QStringLiteral("received track WAV metadata failed: ") + QString::fromUtf8(error.what()));
    }
    incomingTrackBytes_.clear();
}

QStringList MainWindow::commonJamArgs() const
{
    QStringList args;
    args << QStringLiteral("--audio-device") << selectedDeviceId()
         << QStringLiteral("--sample-rate") << QString::number(sampleRateSpin_->value())
         << QStringLiteral("--audio-buffer-size") << QString::number(bufferSizeSpin_->value())
         << QStringLiteral("--frame-size") << QString::number(frameSizeSpin_->value())
         << QStringLiteral("--playback-prefill-frames") << QString::number(prefillSpin_->value())
         << QStringLiteral("--input-channels") << inputChannelsEdit_->text()
         << QStringLiteral("--output-channels") << outputChannelsEdit_->text()
         << QStringLiteral("--stats") << QStringLiteral("enabled")
         << QStringLiteral("--stats-interval-ms") << QStringLiteral("1000")
         << QStringLiteral("--machine-readable-startup") << QStringLiteral("on")
         << QStringLiteral("--status-format") << QStringLiteral("jsonl")
         << QStringLiteral("--metronome") << (metronomeCheck_->isChecked() ? QStringLiteral("on") : QStringLiteral("off"))
         << QStringLiteral("--bpm") << QString::number(bpmSpin_->value())
         << QStringLiteral("--metronome-level") << QString::number(static_cast<double>(metronomeLevelSlider_->value()) / 100.0, 'f', 2)
         << QStringLiteral("--remote-level") << QString::number(static_cast<double>(remoteLevelSlider_->value()) / 100.0, 'f', 2);
    if (noStunCheck_->isChecked()) {
        args << QStringLiteral("--no-stun");
    }
    return args;
}

QString MainWindow::selectedDeviceId() const
{
    const QString data = deviceBox_->currentData().toString();
    return data.isEmpty() ? deviceId(deviceBox_->currentText()) : data;
}

QJsonObject MainWindow::trackToJson() const
{
    const auto& model = trackController_.model();
    return QJsonObject{
        {QStringLiteral("file_name"), model.fileName},
        {QStringLiteral("file_path"), model.filePath},
        {QStringLiteral("file_bytes"), model.fileBytes},
        {QStringLiteral("sample_rate"), model.sampleRate},
        {QStringLiteral("duration_ms"), model.durationMs},
        {QStringLiteral("sha256"), model.sha256},
        {QStringLiteral("guessed_bpm"), model.guessedBpm},
        {QStringLiteral("accepted_bpm"), model.acceptedBpm},
        {QStringLiteral("key"), model.key},
        {QStringLiteral("speed"), model.speed},
        {QStringLiteral("pitch_cents"), model.pitchCents},
        {QStringLiteral("loop_enabled"), model.loopEnabled},
        {QStringLiteral("loop_start_seconds"), model.loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), model.loopEndSeconds},
        {QStringLiteral("sync_controls"), model.syncControls},
        {QStringLiteral("focus_frequency_hz"), model.focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), model.focusGainDb},
        {QStringLiteral("focus_q"), model.focusQ},
        {QStringLiteral("highpass_hz"), model.highpassHz},
        {QStringLiteral("lowpass_hz"), model.lowpassHz},
    };
}

void MainWindow::loadTrackJson(const QJsonObject& object)
{
    if (object.isEmpty()) {
        return;
    }
    auto& model = trackController_.model();
    model.fileName = object.value(QStringLiteral("file_name")).toString(model.fileName);
    model.filePath = object.value(QStringLiteral("file_path")).toString(model.filePath);
    model.fileBytes = static_cast<qint64>(object.value(QStringLiteral("file_bytes")).toDouble(model.fileBytes));
    model.sampleRate = object.value(QStringLiteral("sample_rate")).toInt(model.sampleRate);
    model.durationMs = object.value(QStringLiteral("duration_ms")).toInt(model.durationMs);
    model.sha256 = object.value(QStringLiteral("sha256")).toString(model.sha256);
    model.guessedBpm = object.value(QStringLiteral("guessed_bpm")).toDouble(model.guessedBpm);
    model.acceptedBpm = object.value(QStringLiteral("accepted_bpm")).toDouble(model.acceptedBpm);
    model.key = object.value(QStringLiteral("key")).toString(model.key);
    model.speed = object.value(QStringLiteral("speed")).toDouble(model.speed);
    model.pitchCents = object.value(QStringLiteral("pitch_cents")).toInt(model.pitchCents);
    model.loopEnabled = object.value(QStringLiteral("loop_enabled")).toBool(model.loopEnabled);
    model.loopStartSeconds = object.value(QStringLiteral("loop_start_seconds")).toDouble(model.loopStartSeconds);
    model.loopEndSeconds = object.value(QStringLiteral("loop_end_seconds")).toDouble(model.loopEndSeconds);
    model.syncControls = object.value(QStringLiteral("sync_controls")).toBool(model.syncControls);
    model.focusFrequencyHz = object.value(QStringLiteral("focus_frequency_hz")).toDouble(model.focusFrequencyHz);
    model.focusGainDb = object.value(QStringLiteral("focus_gain_db")).toDouble(model.focusGainDb);
    model.focusQ = object.value(QStringLiteral("focus_q")).toDouble(model.focusQ);
    model.highpassHz = object.value(QStringLiteral("highpass_hz")).toDouble(model.highpassHz);
    model.lowpassHz = object.value(QStringLiteral("lowpass_hz")).toDouble(model.lowpassHz);
    updateTrackControls();
    loadTrackIntoPlayer();
}

void MainWindow::newSong()
{
    songModel_.reset();
    trackPlayer_.stop();
    trackPlayer_.setSource(QUrl());
    trackController_ = SharedTrackController{};
    lastCaptureSummary_ = QJsonObject{};
    lastCapturePath_.clear();
    updateTrackControls();
    refreshSongViews();
}

void MainWindow::openSong()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open Jam2 Song"),
        QString(),
        QStringLiteral("Jam2 song (*.jam2song *.json);;JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("Could not open song file."));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    if (!document.isObject() || !songModel_.loadJson(root)) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("Invalid Jam2 song file."));
        return;
    }
    loadTrackJson(root.value(QStringLiteral("track")).toObject());
    refreshSongViews();
}

void MainWindow::saveSong()
{
    songModel_.setTitle(songTitleEdit_->text());
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Jam2 Song"),
        songModel_.title() + QStringLiteral(".jam2song"),
        QStringLiteral("Jam2 song (*.jam2song);;JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("Could not save song file."));
        return;
    }
    QJsonObject root = songModel_.toJson();
    root.insert(QStringLiteral("track"), trackToJson());
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MainWindow::refreshSongViews()
{
    if (songTitleEdit_) {
        songTitleEdit_->setText(songModel_.title());
    }
    if (chordGrid_) {
        chordGrid_->refresh();
    }
    if (beatGrid_) {
        beatGrid_->refresh();
    }
    if (lyricGrid_) {
        lyricGrid_->refresh();
    }
    if (arrangementEdit_) {
        QStringList parts;
        const int selected = arrangementSectionBox_ ? qMax(0, arrangementSectionBox_->currentIndex()) : 0;
        if (arrangementSectionBox_) {
            arrangementSectionBox_->clear();
        }
        for (const SongSection& section : songModel_.sections()) {
            if (arrangementSectionBox_) {
                arrangementSectionBox_->addItem(section.name + QStringLiteral(" ") + section.label);
            }
            parts << QStringLiteral("[%1 %2: %3 beats]")
                .arg(section.name)
                .arg(section.label)
                .arg(section.beats);
        }
        if (arrangementSectionBox_ && arrangementSectionBox_->count() > 0) {
            arrangementSectionBox_->setCurrentIndex(qMin(selected, arrangementSectionBox_->count() - 1));
        }
        arrangementEdit_->setPlainText(parts.join(QStringLiteral(" -> ")));
    }
}

QString MainWindow::sessionHex() const
{
    return sessionToHex(sessionId_);
}

QString MainWindow::keyHex() const
{
    return keyToHex(sessionKey_);
}

void MainWindow::generateSession()
{
    sessionId_ = jam2::random_u64();
    sessionKey_ = jam2::random_key();
}
