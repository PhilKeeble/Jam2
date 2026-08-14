#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <chrono>
#include <iostream>

namespace {

bool writeScenario(
    const QString& path,
    const QString& artifactRoot,
    const QString& operation,
    QString& error)
{
    QJsonObject scenario{
        {QStringLiteral("schema"), QStringLiteral("jam2-debug-scenario")},
        {QStringLiteral("run_id"), operation},
        {QStringLiteral("operation"), operation},
        {QStringLiteral("profile"), QStringLiteral("fast")},
        {QStringLiteral("artifacts"), QJsonObject{
            {QStringLiteral("root"), artifactRoot},
        }},
    };
    if (operation == QStringLiteral("validate.boundaries")) {
        scenario.insert(QStringLiteral("fixtures"), QJsonArray{});
    } else if (operation == QStringLiteral("validate.controller-lifecycle")) {
        scenario.insert(QStringLiteral("network"), QJsonObject{
            {QStringLiteral("heartbeat_interval_ms"), 20},
            {QStringLiteral("heartbeat_miss_limit"), 3},
        });
    } else {
        error = QStringLiteral("unsupported native validation operation");
        return false;
    }

    QFile file(path);
    const QByteArray encoded = QJsonDocument(scenario).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(encoded) != encoded.size() || !file.flush()) {
        error = QStringLiteral("could not write temporary native validation scenario");
        return false;
    }
    return true;
}

bool validateManifest(
    const QString& manifestPath,
    const QString& operation,
    QString& error)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("native validation did not publish its manifest");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject manifest = document.object();
    const QJsonObject result = manifest.value(QStringLiteral("result")).toObject();
    const QJsonArray cases = result.value(QStringLiteral("cases")).toArray();
    const int minimumCases = operation == QStringLiteral("validate.boundaries") ? 200 : 25;
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        cases.size() < minimumCases) {
        error = QStringLiteral(
            "native validation manifest failed: parse=%1 manifest_ok=%2 result_ok=%3 cases=%4")
            .arg(parseError.errorString())
            .arg(manifest.value(QStringLiteral("ok")).toBool())
            .arg(result.value(QStringLiteral("ok")).toBool())
            .arg(cases.size());
        return false;
    }
    for (const QJsonValue& value : cases) {
        const QJsonObject item = value.toObject();
        if (!item.value(QStringLiteral("ok")).toBool()) {
            error = QStringLiteral("native validation case failed: %1 (%2)")
                .arg(item.value(QStringLiteral("name")).toString(),
                     item.value(QStringLiteral("detail")).toString());
            return false;
        }
    }
    if (!manifest.value(QStringLiteral("ok")).toBool() ||
        manifest.value(QStringLiteral("return_code")).toInt(-1) != 0 ||
        !result.value(QStringLiteral("ok")).toBool()) {
        error = QStringLiteral(
            "native validation aggregate failed despite no failing case: manifest_ok=%1 result_ok=%2")
            .arg(manifest.value(QStringLiteral("ok")).toBool())
            .arg(result.value(QStringLiteral("ok")).toBool());
        return false;
    }
    std::cout << operation.toStdString() << " passed " << cases.size()
              << " native cases\n";
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::cerr << "usage: jam2_native_validation_integration <release-jam2> <operation>\n";
        return 2;
    }

    const QString executable = QString::fromLocal8Bit(argv[1]);
    const QString operation = QString::fromLocal8Bit(argv[2]);
    QTemporaryDir root;
    if (!root.isValid()) {
        std::cerr << "could not create native validation temporary root\n";
        return 1;
    }
    const auto fail = [&](const QString& reason, const QByteArray& output = {}) {
        root.setAutoRemove(false);
        std::cerr << reason.toStdString();
        if (!output.isEmpty()) std::cerr << '\n' << output.toStdString();
        std::cerr << "\nartifacts retained at " << root.path().toStdString() << '\n';
        return 1;
    };
    const QString artifactRoot = QDir(root.path()).absoluteFilePath(QStringLiteral("artifacts"));
    const QString scenarioPath = QDir(root.path()).absoluteFilePath(QStringLiteral("scenario.json"));
    QString error;
    if (!writeScenario(scenarioPath, artifactRoot, operation, error)) {
        return fail(error);
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("debug"), QStringLiteral("run"), scenarioPath});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(
            static_cast<int>(jam2::test::scaledTimeout(
                std::chrono::seconds(10)).count())) ||
        !process.waitForFinished(
            static_cast<int>(jam2::test::scaledTimeout(
                std::chrono::seconds(120)).count()))) {
        process.kill();
        process.waitForFinished(5000);
        return fail(
            QStringLiteral("native validation process failed to start or finish: ") +
                process.errorString(),
            process.readAll());
    }
    const QByteArray output = process.readAll();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString manifestError;
        (void)validateManifest(
            QDir(artifactRoot).absoluteFilePath(QStringLiteral("native-manifest.json")),
            operation,
            manifestError);
        return fail(
            QStringLiteral("native validation process failed: status=%1 code=%2\n%3")
                .arg(static_cast<int>(process.exitStatus()))
                .arg(process.exitCode())
                .arg(manifestError),
            output);
    }
    if (!validateManifest(
            QDir(artifactRoot).absoluteFilePath(QStringLiteral("native-manifest.json")),
            operation,
            error)) {
        return fail(error, output);
    }
    return 0;
}
