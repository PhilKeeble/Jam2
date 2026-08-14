#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

namespace {

void emitEvent(const QJsonObject& event)
{
    QTextStream output(stdout);
    output << QJsonDocument(event).toJson(QJsonDocument::Compact) << Qt::endl;
}

bool writeJson(const QString& path, const QJsonObject& object)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(bytes) == bytes.size();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    const int requestIndex = arguments.indexOf(QStringLiteral("--request"));
    if (requestIndex < 0 || requestIndex + 1 >= arguments.size()) return 9;
    QFile file(arguments[requestIndex + 1]);
    if (!file.open(QIODevice::ReadOnly)) return 8;
    const QJsonObject request = QJsonDocument::fromJson(file.readAll()).object();
    const QString action = request.value(QStringLiteral("action")).toString();
    const QString projectRoot = request.value(QStringLiteral("project_root")).toString();
    const QString syntheticHash = QStringLiteral("synthetic-worker-hash");
    const QString sourceRoot = QDir(projectRoot).absoluteFilePath(
        QStringLiteral("analysis/sources/%1").arg(syntheticHash));

    if (action == QStringLiteral("sleep")) {
        emitEvent({
            {QStringLiteral("protocol"), 1},
            {QStringLiteral("type"), QStringLiteral("progress")},
            {QStringLiteral("message"), QStringLiteral("Sleeping")},
            {QStringLiteral("percent"), 4},
        });
        QThread::msleep(30'000);
        return 0;
    }
    if (action == QStringLiteral("fail")) {
        emitEvent({
            {QStringLiteral("protocol"), 1},
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("message"), QStringLiteral("synthetic worker failure")},
        });
        return 1;
    }
    if (action == QStringLiteral("exit-only")) {
        QTextStream(stdout) << QStringLiteral("trailing worker detail");
        return 7;
    }

    QTextStream(stderr) << QStringLiteral(" detail one\r\ndetail two ") << Qt::endl;
    QTextStream(stdout) << QStringLiteral("unstructured worker diagnostic") << Qt::endl;
    emitEvent({
        {QStringLiteral("protocol"), 99},
        {QStringLiteral("type"), QStringLiteral("progress")},
        {QStringLiteral("message"), QStringLiteral("wrong protocol")},
    });
    emitEvent({
        {QStringLiteral("protocol"), 1},
        {QStringLiteral("type"), QStringLiteral("progress")},
        {QStringLiteral("message"), QStringLiteral("Synthetic work complete")},
        {QStringLiteral("percent"), 150},
    });
    QJsonObject result{
        {QStringLiteral("action"), action},
        {QStringLiteral("format"), QStringLiteral("synthetic-result-v1")},
    };
    if (action == QStringLiteral("detect_bpm")) {
        result.insert(QStringLiteral("format"), QStringLiteral("jamtaster-tempo-v1"));
        result.insert(QStringLiteral("source_sha256"), syntheticHash);
        result.insert(QStringLiteral("bpm"), 128.25);
        result.insert(QStringLiteral("beats_per_bar"), 7);
        if (!writeJson(QDir(sourceRoot).absoluteFilePath(QStringLiteral("tempo.json")), result)) {
            return 6;
        }
    } else if (action == QStringLiteral("split_stems")) {
        result.insert(QStringLiteral("format"), QStringLiteral("jamtaster-stems-v1"));
        result.insert(QStringLiteral("source_sha256"), syntheticHash);
        result.insert(QStringLiteral("stems"), QJsonObject{
            {QStringLiteral("drums"), QDir(sourceRoot).absoluteFilePath(
                QStringLiteral("stems/drums.wav"))},
            {QStringLiteral("bass"), QDir(sourceRoot).absoluteFilePath(
                QStringLiteral("stems/bass.wav"))},
            {QStringLiteral("other"), QDir(sourceRoot).absoluteFilePath(
                QStringLiteral("stems/other.wav"))},
            {QStringLiteral("vocals"), QDir(sourceRoot).absoluteFilePath(
                QStringLiteral("stems/vocals.wav"))},
        });
        if (!writeJson(QDir(sourceRoot).absoluteFilePath(QStringLiteral("stems.json")), result)) {
            return 6;
        }
    } else if (action == QStringLiteral("analyze_all")) {
        result.insert(QStringLiteral("format"), QStringLiteral("jamtaster-job-result-v1"));
        result.insert(QStringLiteral("converted_song"), QDir(sourceRoot).absoluteFilePath(
            QStringLiteral("converted/synthetic-song")));
    } else if (action == QStringLiteral("root-result")) {
        result.insert(QStringLiteral("converted_song"), QDir::rootPath());
    }
    emitEvent({
        {QStringLiteral("protocol"), 1},
        {QStringLiteral("type"), QStringLiteral("result")},
        {QStringLiteral("result"), result},
    });
    return 0;
}
