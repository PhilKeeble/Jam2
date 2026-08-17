#include "JamTasterDialog.hpp"
#include "JamTasterService.hpp"
#include "TestTiming.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr auto kWorkerHash = "synthetic-worker-hash";

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()),
        "fixture parent directory must be creatable");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(bytes) == bytes.size(),
        "fixture file must be writable");
}

void writeJson(const QString& path, const QJsonObject& object)
{
    writeFile(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void waitUntil(const std::function<bool()>& predicate, const std::string& message)
{
    const auto timeout = jam2::test::scaledTimeout(std::chrono::seconds(20));
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(predicate(), message);
}

template <typename Widget>
Widget* findTextWidget(QWidget& root, const QString& text, bool prefix = false)
{
    for (Widget* widget : root.findChildren<Widget*>()) {
        const QString candidate = widget->text();
        if ((!prefix && candidate == text) || (prefix && candidate.startsWith(text))) {
            return widget;
        }
    }
    throw std::runtime_error(
        QStringLiteral("missing widget text: %1").arg(text).toStdString());
}

QLabel* valueLabel(QCheckBox& check)
{
    QLabel* label = check.parentWidget()->findChild<QLabel*>();
    require(label != nullptr, "result row must own a value label");
    return label;
}

struct MessageCapture {
    bool seen = false;
    QString title;
    QString text;
};

void answerNextMessage(
    QMessageBox::StandardButton response,
    MessageCapture& capture)
{
    auto* timer = new QTimer(QApplication::instance());
    timer->setInterval(1);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, &capture, response] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) return;
        QAbstractButton* button = box->button(response);
        if (button == nullptr) return;
        timer->stop();
        timer->deleteLater();
        capture.seen = true;
        capture.title = box->windowTitle();
        capture.text = box->text();
        button->click();
    });
    timer->start();
}

void answerNextFileDialog(const QString& selection, bool& seen)
{
    QTimer::singleShot(0, [&seen, selection] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        seen = true;
        if (selection.isEmpty()) {
            dialog->reject();
        } else {
            dialog->selectFile(selection);
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        }
    });
}

struct BundleFixture {
    QString root;
    QString bundle;
    QString project;
    QString sourceOne;
    QString sourceTwo;
    QString worker;
};

BundleFixture makeBundle(const QString& sourceWorker)
{
    BundleFixture fixture;
    fixture.root = QDir::temp().absoluteFilePath(
        QStringLiteral("jam2-jamtaster-dialog-boundary"));
    QDir(fixture.root).removeRecursively();
    fixture.bundle = QDir(fixture.root).absoluteFilePath(QStringLiteral("bundle"));
    fixture.project = QDir(fixture.root).absoluteFilePath(QStringLiteral("project"));
    fixture.sourceOne = QDir(fixture.root).absoluteFilePath(QStringLiteral("source-one.wav"));
    fixture.sourceTwo = QDir(fixture.root).absoluteFilePath(QStringLiteral("source-two.wav"));
    const QString models = QDir(fixture.bundle).absoluteFilePath(QStringLiteral("models"));
    require(QDir().mkpath(models) && QDir().mkpath(fixture.project),
        "dialog fixture directories must be creatable");
    writeFile(fixture.sourceOne, "synthetic source one");
    writeFile(fixture.sourceTwo, "synthetic source two");
    static constexpr std::array<const char*, 8> modelNames{
        "beat_this.onnx", "basic_pitch.onnx", "chordmini_btc.onnx", "adtof.onnx",
        "htdemucs_ft_0.onnx", "htdemucs_ft_1.onnx",
        "htdemucs_ft_2.onnx", "htdemucs_ft_3.onnx",
    };
    for (const char* name : modelNames) {
        writeFile(QDir(models).absoluteFilePath(QString::fromLatin1(name)), "model");
    }
#ifdef Q_OS_WIN
    fixture.worker = QDir(fixture.bundle).absoluteFilePath(
        QStringLiteral("jamtaster-worker.exe"));
    writeFile(QDir(fixture.bundle).absoluteFilePath(QStringLiteral("onnxruntime.dll")),
        "runtime");
#else
    fixture.worker = QDir(fixture.bundle).absoluteFilePath(
        QStringLiteral("jamtaster-worker"));
#endif
    require(QFile::copy(sourceWorker, fixture.worker),
        "dialog test worker must be copied into its injected bundle");
    return fixture;
}

QString sourceRoot(const BundleFixture& fixture, const QString& hash)
{
    return QDir(fixture.project).absoluteFilePath(
        QStringLiteral("analysis/sources/%1").arg(hash));
}

QString writeFullAnalysis(
    const BundleFixture& fixture,
    const QString& hash,
    const QString& slug,
    bool writeManifest = true)
{
    const QString root = sourceRoot(fixture, hash);
    writeJson(QDir(root).absoluteFilePath(QStringLiteral("tempo.json")), {
        {QStringLiteral("bpm"), 123.456},
        {QStringLiteral("beats_per_bar"), 7},
    });
    writeJson(QDir(root).absoluteFilePath(QStringLiteral("stems.json")), {
        {QStringLiteral("stems"), QJsonArray{1, 2, 3, 4}},
    });
    writeJson(QDir(root).absoluteFilePath(QStringLiteral("analysis.json")), {
        {QStringLiteral("input"), QJsonObject{{QStringLiteral("sha256"), hash}}},
        {QStringLiteral("analysis"), QJsonObject{
            {QStringLiteral("chords"), QJsonArray{1, 2}},
            {QStringLiteral("drums"), QJsonArray{1, 2, 3}},
            {QStringLiteral("bass"), QJsonArray{1}},
        }},
        {QStringLiteral("selected_sections"), QJsonArray{1, 2, 3, 4}},
    });
    if (writeManifest) {
        writeJson(QDir(root).absoluteFilePath(QStringLiteral("manifest.json")), {
            {QStringLiteral("song_slug"), slug},
        });
    }
    const QString converted = QDir(root).absoluteFilePath(
        QStringLiteral("converted/%1").arg(slug));
    require(QDir().mkpath(converted), "converted song directory must be creatable");
    writeFile(QDir(converted).absoluteFilePath(
        (slug.isEmpty() ? QStringLiteral("song") : slug) + QStringLiteral(".jamjar")),
        "synthetic JamJar");
    return converted;
}

struct CallbackState {
    int quickCalls = 0;
    int convertedCalls = 0;
    int createCalls = 0;
    QJsonObject quickTempo;
    QJsonObject quickStems;
    QJsonObject options;
    QString converted;
    QString source;
    QString hash;
};

JamTasterDialog::Callbacks callbacks(CallbackState& state)
{
    JamTasterDialog::Callbacks result;
    result.applyQuick = [&state](const QJsonObject& tempo, const QJsonObject& stems,
                            const QJsonObject& options, const QString& hash) {
        ++state.quickCalls;
        state.quickTempo = tempo;
        state.quickStems = stems;
        state.options = options;
        state.hash = hash;
    };
    result.applyConverted = [&state](const QString& converted, const QJsonObject& options) {
        ++state.convertedCalls;
        state.converted = converted;
        state.options = options;
    };
    result.createSong = [&state](
                            const QString& converted,
                            const QString& source,
                            const QString& hash) {
        ++state.createCalls;
        state.converted = converted;
        state.source = source;
        state.hash = hash;
    };
    return result;
}

void verifyFullResultRows(JamTasterDialog& dialog)
{
    auto* tempo = findTextWidget<QCheckBox>(dialog, QStringLiteral("Tempo and grid"));
    auto* stems = findTextWidget<QCheckBox>(dialog, QStringLiteral("Separated stems"));
    auto* chords = findTextWidget<QCheckBox>(dialog, QStringLiteral("Chords"));
    auto* drums = findTextWidget<QCheckBox>(dialog, QStringLiteral("Drum pattern"));
    auto* bass = findTextWidget<QCheckBox>(dialog, QStringLiteral("Bass notes"));
    auto* sections = findTextWidget<QCheckBox>(
        dialog, QStringLiteral("Sections and arrangement"));
    require(tempo->isEnabled() && stems->isEnabled() && chords->isEnabled() &&
            drums->isEnabled() && bass->isEnabled() && sections->isEnabled(),
        "complete analysis enables every selectable result");
    require(valueLabel(*tempo)->text().contains(QStringLiteral("123.46")) &&
            valueLabel(*tempo)->text().contains(QStringLiteral("7/4")) &&
            valueLabel(*stems)->text() == QStringLiteral("4 stems available") &&
            valueLabel(*chords)->text() == QStringLiteral("2 regions") &&
            valueLabel(*drums)->text() == QStringLiteral("3 events") &&
            valueLabel(*bass)->text() == QStringLiteral("1 notes") &&
            valueLabel(*sections)->text() == QStringLiteral("4 sections"),
        "complete analysis presents every saved result count and meter");
}

void testSavedResultsAndSelections(const BundleFixture& fixture)
{
    const QString hash = QStringLiteral("saved-hash");
    const QString converted = writeFullAnalysis(
        fixture, hash, QStringLiteral("saved-song"));
    JamTasterService service(fixture.bundle, {});
    CallbackState state;
    JamTasterDialog dialog(service, fixture.project, fixture.sourceOne, hash,
        QStringLiteral("Saved Song"), callbacks(state));
    require(dialog.windowTitle() == QStringLiteral("JamTaster") &&
            dialog.property("jam2MaximumDialogHeight").toInt() == 760,
        "dialog exposes its title and bounded-height contract");
    auto* source = dialog.findChild<QLineEdit*>();
    require(source != nullptr && source->isReadOnly() &&
            source->text() == QFileInfo(fixture.sourceOne).absoluteFilePath(),
        "dialog presents its source as read-only absolute context");
    verifyFullResultRows(dialog);

    const std::array<QCheckBox*, 6> checks{
        findTextWidget<QCheckBox>(dialog, QStringLiteral("Tempo and grid")),
        findTextWidget<QCheckBox>(dialog, QStringLiteral("Separated stems")),
        findTextWidget<QCheckBox>(dialog, QStringLiteral("Chords")),
        findTextWidget<QCheckBox>(dialog, QStringLiteral("Drum pattern")),
        findTextWidget<QCheckBox>(dialog, QStringLiteral("Bass notes")),
        findTextWidget<QCheckBox>(dialog, QStringLiteral("Sections and arrangement")),
    };
    auto* apply = findTextWidget<QPushButton>(
        dialog, QStringLiteral("Apply Selected to Current Jam"));
    require(!apply->isEnabled(), "apply requires at least one selected result");
    for (QCheckBox* check : checks) check->setChecked(true);
    require(apply->isEnabled(), "any complete selected result enables apply");
    apply->click();
    require(state.convertedCalls == 1 && state.converted == converted &&
            state.options.value(QStringLiteral("tempo")).toBool() &&
            state.options.value(QStringLiteral("stems")).toBool() &&
            state.options.value(QStringLiteral("chords")).toBool() &&
            state.options.value(QStringLiteral("drums")).toBool() &&
            state.options.value(QStringLiteral("bass")).toBool() &&
            state.options.value(QStringLiteral("sections")).toBool() &&
            state.options.value(QStringLiteral("source_path")).toString() == fixture.sourceOne &&
            state.options.value(QStringLiteral("source_hash")).toString() == hash,
        "converted apply preserves every explicit selection and source identity");

    auto* create = findTextWidget<QPushButton>(dialog, QStringLiteral("Create New JamJar"));
    require(create->isEnabled(), "saved full analysis enables JamJar creation");
    create->click();
    require(state.createCalls == 1 && state.converted == converted &&
            state.source == fixture.sourceOne && state.hash == hash,
        "saved full analysis creates directly without repeating work");

    const QString malformedHash = QStringLiteral("malformed-hash");
    const QString malformedRoot = sourceRoot(fixture, malformedHash);
    writeFile(QDir(malformedRoot).absoluteFilePath(QStringLiteral("tempo.json")), "[]");
    writeFile(QDir(malformedRoot).absoluteFilePath(QStringLiteral("stems.json")), "not-json");
    QFile oversized(QDir(malformedRoot).absoluteFilePath(QStringLiteral("analysis.json")));
    require(oversized.open(QIODevice::WriteOnly) && oversized.resize(64 * 1024 * 1024 + 1),
        "oversized JSON fixture must be creatable");
    oversized.close();
    dialog.setSourceContext(fixture.project, fixture.sourceTwo, malformedHash,
        QStringLiteral("Malformed"));
    require(source->text() == fixture.sourceTwo && !checks.front()->isEnabled() &&
            !checks.back()->isEnabled() && !apply->isEnabled() && !create->isEnabled(),
        "missing, invalid, nonobject, and oversized saved JSON are safely ignored");

    const QString fallbackHash = QStringLiteral("fallback-hash");
    const QString fallbackConverted = writeFullAnalysis(
        fixture, fallbackHash, QStringLiteral("source-two"), false);
    dialog.setSourceContext(fixture.project, fixture.sourceTwo, fallbackHash,
        QStringLiteral("Fallback"));
    verifyFullResultRows(dialog);
    const QString expectedFallback = QDir(sourceRoot(fixture, fallbackHash)).absoluteFilePath(
        QStringLiteral("converted/source-two"));
    require(fallbackConverted == expectedFallback,
        "fixture records the source-name fallback converted directory");
    for (QCheckBox* check : checks) check->setChecked(true);
    apply->click();
    require(state.converted == expectedFallback,
        "missing manifest falls back to the selected WAV base name");

    const QString incompleteHash = QStringLiteral("incomplete-hash");
    const QString incompleteConverted = writeFullAnalysis(
        fixture, incompleteHash, QStringLiteral("incomplete-song"));
    require(QFile::remove(QDir(incompleteConverted).absoluteFilePath(
                QStringLiteral("incomplete-song.jamjar"))),
        "incomplete export fixture must remove its only JamJar");
    dialog.setSourceContext(fixture.project, fixture.sourceOne, incompleteHash,
        QStringLiteral("Incomplete export"));
    auto* incompleteTempo = findTextWidget<QCheckBox>(
        dialog, QStringLiteral("Tempo and grid"));
    auto* incompleteStems = findTextWidget<QCheckBox>(
        dialog, QStringLiteral("Separated stems"));
    auto* incompleteChords = findTextWidget<QCheckBox>(dialog, QStringLiteral("Chords"));
    require(incompleteTempo->isEnabled() && incompleteStems->isEnabled() &&
            !incompleteChords->isEnabled() && create->isEnabled(),
        "incomplete export preserves quick results but disables full-only application");
    incompleteTempo->setChecked(true);
    apply->click();
    require(state.quickCalls == 1 && state.convertedCalls == 2,
        "incomplete export routes available tempo through quick apply, not converted apply");

    auto* close = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close);
    require(close != nullptr && close->isEnabled(), "dialog exposes its Close button");
    close->click();
    require(dialog.result() == QDialog::Rejected, "Close rejects only the dialog");
}

void testChooserAndWorkerActions(const BundleFixture& fixture)
{
    JamTasterService service(fixture.bundle, {});
    CallbackState state;
    const QString missing = QDir(fixture.root).absoluteFilePath(QStringLiteral("missing.wav"));
    JamTasterDialog dialog(service, fixture.project, missing, {},
        QStringLiteral("Missing"), callbacks(state));
    auto* choose = findTextWidget<QPushButton>(dialog, QStringLiteral("Choose WAV"), true);
    auto* source = dialog.findChild<QLineEdit*>();
    bool chooserSeen = false;
    answerNextFileDialog({}, chooserSeen);
    choose->click();
    require(chooserSeen && source->text() == missing,
        "cancelling explicit WAV choice preserves source context");
    chooserSeen = false;
    answerNextFileDialog(fixture.sourceOne, chooserSeen);
    choose->click();
    require(chooserSeen && source->text() == fixture.sourceOne,
        "WAV chooser adopts an absolute selected source and clears stale identity");

    auto* bpm = findTextWidget<QPushButton>(dialog, QStringLiteral("Find BPM"));
    bpm->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "BPM action must complete through the isolated worker");
    auto* tempo = findTextWidget<QCheckBox>(dialog, QStringLiteral("Tempo and grid"));
    require(tempo->isEnabled() && tempo->isChecked() &&
            valueLabel(*tempo)->text().contains(QStringLiteral("128.25")) &&
            valueLabel(*tempo)->text().contains(QStringLiteral("7/4")),
        "BPM result refreshes, selects, and presents saved tempo");
    auto* progress = dialog.findChild<QProgressBar*>(QStringLiteral("JamTasterProgress"));
    require(progress != nullptr && progress->value() == 100,
        "completed worker progress is presented at 100 percent");
    auto* apply = findTextWidget<QPushButton>(
        dialog, QStringLiteral("Apply Selected to Current Jam"));
    apply->click();
    require(state.quickCalls == 1 &&
            state.quickTempo.value(QStringLiteral("bpm")).toDouble() == 128.25 &&
            state.hash == QString::fromLatin1(kWorkerHash),
        "quick BPM apply preserves worker evidence and computed source hash");

    dialog.setSourceContext(fixture.project, fixture.sourceOne, {},
        QStringLiteral("Reaccepted result"));
    require(tempo->isEnabled() && tempo->isChecked(),
        "matching source context reaccepts the service's retained result");

    auto* stemsButton = findTextWidget<QPushButton>(dialog, QStringLiteral("Split Stems"));
    stemsButton->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "stem action must complete through the isolated worker");
    auto* stems = findTextWidget<QCheckBox>(dialog, QStringLiteral("Separated stems"));
    require(stems->isEnabled() && stems->isChecked() && tempo->isEnabled() &&
            !tempo->isChecked(),
        "stem result selects only the new stem evidence while retaining tempo availability");
    tempo->setChecked(true);
    apply->click();
    require(state.quickCalls == 2 && !state.quickStems.isEmpty() &&
            state.options.value(QStringLiteral("tempo")).toBool() &&
            state.options.value(QStringLiteral("stems")).toBool(),
        "combined quick apply sends selected tempo and stem evidence");

    const QString converted = writeFullAnalysis(fixture,
        QString::fromLatin1(kWorkerHash), QStringLiteral("synthetic-song"));
    auto* create = findTextWidget<QPushButton>(dialog, QStringLiteral("Create New JamJar"));
    MessageCapture prompt;
    answerNextMessage(QMessageBox::Cancel, prompt);
    create->click();
    bool promptIdentityMatches =
        prompt.title == QStringLiteral("Complete JamJar analysis");
#ifdef Q_OS_MACOS
    // Native macOS alerts intentionally omit their window title. Identify the
    // same prompt by its owned message text while retaining the strict title
    // contract on platforms that present one.
    promptIdentityMatches = promptIdentityMatches || prompt.title.isEmpty();
#endif
    require(prompt.seen && promptIdentityMatches &&
            prompt.text.contains(QStringLiteral("A new JamJar needs the full timing")) &&
            state.createCalls == 0 && !service.taskActive(),
        QStringLiteral(
            "cancelling full-analysis confirmation starts no work "
            "(seen=%1 title='%2' creates=%3 active=%4)")
            .arg(prompt.seen)
            .arg(prompt.title)
            .arg(state.createCalls)
            .arg(service.taskActive())
            .toStdString());
    prompt = {};
    answerNextMessage(QMessageBox::Yes, prompt);
    create->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "confirmed full analysis must complete");
    QApplication::processEvents(QEventLoop::AllEvents);
    require(prompt.seen, "confirmed full analysis prompt must receive Yes");
    require(state.createCalls == 1,
        QStringLiteral("confirmed full analysis must create once (actual %1)")
            .arg(state.createCalls).toStdString());
    require(state.converted == converted,
        QStringLiteral("full-analysis converted path mismatch: '%1' != '%2'")
            .arg(state.converted, converted).toStdString());
    require(state.source == fixture.sourceOne,
        QStringLiteral("full-analysis source path mismatch: '%1' != '%2'")
            .arg(state.source, fixture.sourceOne).toStdString());
    require(state.hash == QString::fromLatin1(kWorkerHash),
        QStringLiteral("full-analysis hash mismatch: '%1'").arg(state.hash).toStdString());
    verifyFullResultRows(dialog);
    for (QCheckBox* check : dialog.findChildren<QCheckBox*>()) {
        require(check->isChecked(), "full worker result selects every available result");
    }
    apply->click();
    require(state.convertedCalls == 1 && state.converted == converted,
        "full worker result applies through the converted-song callback");
    create->click();
    require(state.createCalls == 2,
        "repeated JamJar creation reuses complete saved analysis without a prompt");

    auto* analyze = findTextWidget<QPushButton>(dialog, QStringLiteral("Analyse Everything"));
    analyze->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "Analyse Everything button must run the complete action directly");
    require(state.createCalls == 2,
        "ordinary complete analysis does not invent a create-song continuation");

    CallbackState foreignState;
    JamTasterDialog foreignDialog(service, fixture.project, fixture.sourceTwo, {},
        QStringLiteral("Different WAV"), callbacks(foreignState));
    auto* foreignTempo = findTextWidget<QCheckBox>(
        foreignDialog, QStringLiteral("Tempo and grid"));
    auto* foreignCreate = findTextWidget<QPushButton>(
        foreignDialog, QStringLiteral("Create New JamJar"));
    require(!foreignTempo->isEnabled() && !foreignCreate->isEnabled(),
        "a new dialog must not adopt a retained result from a different WAV");

    QString foreignError;
    require(service.startJob({
                {QStringLiteral("action"), QStringLiteral("detect_bpm")},
                {QStringLiteral("input_path"), fixture.sourceOne},
                {QStringLiteral("project_root"), fixture.project},
                {QStringLiteral("display_name"), QStringLiteral("Other source task")},
            }, foreignError),
        "foreign-source live-result fixture must start");
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "foreign-source live result must complete");
    require(!foreignTempo->isEnabled() && !foreignCreate->isEnabled(),
        "an open dialog must ignore a live result produced for a different WAV");
}

void testMissingSourceAndUnavailableBundle(const BundleFixture& fixture)
{
    JamTasterService service(fixture.bundle, {});
    CallbackState state;
    const QString missing = QDir(fixture.root).absoluteFilePath(QStringLiteral("missing.wav"));
    JamTasterDialog missingDialog(service, fixture.project, missing, {},
        QStringLiteral("Missing"), callbacks(state));
    bool chooserSeen = false;
    answerNextFileDialog({}, chooserSeen);
    findTextWidget<QPushButton>(missingDialog, QStringLiteral("Find BPM"))->click();
    require(chooserSeen && !service.taskActive(),
        "an action with no source offers the WAV chooser and stops after cancellation");

    JamTasterService unavailable(
        QDir(fixture.root).absoluteFilePath(QStringLiteral("missing-bundle")), {});
    JamTasterDialog unavailableDialog(unavailable, fixture.project, fixture.sourceOne, {},
        QStringLiteral("Unavailable"), callbacks(state));
    MessageCapture warning;
    answerNextMessage(QMessageBox::Ok, warning);
    findTextWidget<QPushButton>(unavailableDialog, QStringLiteral("Find BPM"))->click();
    bool warningTitleMatches = warning.title == QStringLiteral("JamTaster");
#ifdef Q_OS_MACOS
    warningTitleMatches = warningTitleMatches || warning.title.isEmpty();
#endif
    require(warning.seen && warningTitleMatches &&
            warning.text.contains(QStringLiteral("worker is missing")),
        "unavailable bundle reports its precise validation error without starting");

    const QString invalidProject = QDir(fixture.root).absoluteFilePath(
        QStringLiteral("project-is-a-file"));
    writeFile(invalidProject, "not a directory");
    JamTasterService failedRequest(fixture.bundle, {});
    JamTasterDialog failedDialog(failedRequest, invalidProject, fixture.sourceOne, {},
        QStringLiteral("Failed request"), callbacks(state));
    warning = {};
    answerNextMessage(QMessageBox::Ok, warning);
    findTextWidget<QPushButton>(failedDialog, QStringLiteral("Split Stems"))->click();
    require(warning.seen &&
            warning.text == QStringLiteral(
                "Could not create the project's analysis working directory.") &&
            !failedRequest.taskActive(),
        "working-directory creation failure is reported synchronously and leaves no task");
}

void testIncompleteCreateContinuation(const BundleFixture& fixture)
{
    BundleFixture isolated = fixture;
    isolated.project = QDir(fixture.root).absoluteFilePath(
        QStringLiteral("incomplete-continuation-project"));
    require(QDir().mkpath(isolated.project),
        "incomplete-continuation project must be creatable");
    JamTasterService service(isolated.bundle, {});
    CallbackState state;
    JamTasterDialog dialog(service, isolated.project, isolated.sourceOne, {},
        QStringLiteral("Incomplete continuation"), callbacks(state));
    findTextWidget<QPushButton>(dialog, QStringLiteral("Find BPM"))->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "incomplete-continuation BPM fixture must complete");
    auto* create = findTextWidget<QPushButton>(dialog, QStringLiteral("Create New JamJar"));
    require(create->isEnabled(), "quick tempo enables the full-analysis creation prompt");
    MessageCapture confirmation;
    answerNextMessage(QMessageBox::Yes, confirmation);
    create->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "incomplete full result must complete without a converted JamJar");
    require(confirmation.seen && state.createCalls == 0,
        "incomplete full result cannot invoke JamJar creation");

    const QString converted = writeFullAnalysis(
        isolated, QString::fromLatin1(kWorkerHash), QStringLiteral("synthetic-song"));
    findTextWidget<QPushButton>(dialog, QStringLiteral("Analyse Everything"))->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "ordinary recovery analysis must complete");
    require(QFileInfo(converted).isDir() && state.createCalls == 0,
        "failed create continuation must not leak into a later ordinary analysis");
}

void testBusyCancellationAndFailure(const BundleFixture& fixture)
{
    JamTasterService service(fixture.bundle, {});
    CallbackState state;
    JamTasterDialog dialog(service, fixture.project, fixture.sourceOne, {},
        QStringLiteral("Busy"), callbacks(state));
    dialog.show();
    QApplication::processEvents();
    QString error;
    require(service.startJob({
                {QStringLiteral("action"), QStringLiteral("sleep")},
                {QStringLiteral("input_path"), fixture.sourceOne},
                {QStringLiteral("project_root"), fixture.project},
                {QStringLiteral("display_name"), QStringLiteral("Sleeping")},
            }, error),
        "busy dialog fixture must start a cancellable worker");
    waitUntil([&] { return service.taskActive() && service.isBusy(); },
        "sleeping worker must become active");
    auto* source = dialog.findChild<QLineEdit*>();
    dialog.setSourceContext(fixture.project, fixture.sourceTwo, {},
        QStringLiteral("Ignored while busy"));
    require(source->text() == fixture.sourceOne,
        "source context cannot change while analysis owns the worker");
    require(!findTextWidget<QPushButton>(dialog, QStringLiteral("Analyse Everything"))->isEnabled() &&
            !findTextWidget<QPushButton>(dialog, QStringLiteral("Find BPM"))->isEnabled() &&
            !findTextWidget<QPushButton>(dialog, QStringLiteral("Split Stems"))->isEnabled(),
        "all analysis actions lock while a task is active");
    auto* cancel = findTextWidget<QPushButton>(dialog, QStringLiteral("Cancel Task"));
    require(cancel->isEnabled(), "active task enables cancellation");
    MessageCapture confirmation;
    answerNextMessage(QMessageBox::No, confirmation);
    cancel->click();
    require(confirmation.seen && service.taskActive(),
        "declining cancellation leaves the worker active");
    confirmation = {};
    answerNextMessage(QMessageBox::Yes, confirmation);
    cancel->click();
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "confirmed cancellation must stop the worker");
    require(confirmation.seen && !cancel->isEnabled() &&
            service.taskStatusText() == QStringLiteral("JamTaster task cancelled"),
        "confirmed cancellation clears dialog and service task state");
    cancel->click();

    QString failureText;
    QTimer modalCloser;
    QObject::connect(&modalCloser, &QTimer::timeout, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) return;
        failureText = box->text();
        box->done(static_cast<int>(QMessageBox::Ok));
    });
    modalCloser.start(2);
    error.clear();
    require(service.startJob({
                {QStringLiteral("action"), QStringLiteral("fail")},
                {QStringLiteral("input_path"), fixture.sourceOne},
                {QStringLiteral("project_root"), fixture.project},
                {QStringLiteral("display_name"), QStringLiteral("Failure")},
            }, error),
        "visible dialog fixture must start a failing worker");
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "failing worker must complete its visible error path");
    modalCloser.stop();
    require(failureText == QStringLiteral("synthetic worker failure") &&
            service.taskStatusText() == failureText,
        "visible worker failure is presented and retained as hard diagnostic data");

    dialog.hide();
    error.clear();
    require(service.startJob({
                {QStringLiteral("action"), QStringLiteral("root-result")},
                {QStringLiteral("input_path"), fixture.sourceOne},
                {QStringLiteral("project_root"), fixture.project},
                {QStringLiteral("display_name"), QStringLiteral("Root result")},
            }, error),
        "dialog accepts a synthetic root converted-result boundary");
    waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
        "root converted-result boundary must complete");
    require(!findTextWidget<QPushButton>(
                dialog, QStringLiteral("Create New JamJar"))->isEnabled(),
        "invalid converted result cannot enable JamJar creation");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication application(argc, argv);
        if (argc != 2) {
            throw std::runtime_error(
                "usage: jam2_jamtaster_dialog_tests <test-worker>");
        }
        const BundleFixture fixture = makeBundle(
            QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath());
        testSavedResultsAndSelections(fixture);
        testChooserAndWorkerActions(fixture);
        testMissingSourceAndUnavailableBundle(fixture);
        testIncompleteCreateContinuation(fixture);
        testBusyCancellationAndFailure(fixture);
        QDir(fixture.root).removeRecursively();
        std::cout << "JamTaster dialog boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster dialog boundary test failed: " << error.what() << '\n';
        return 1;
    }
}
