#include "Jam2Process.hpp"

#include <QCoreApplication>
#include <QJsonDocument>

Jam2Process::Jam2Process(QObject* parent)
    : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { drainStdout(); });
    QObject::connect(&process_, &QProcess::readyReadStandardError, this, [this] { drainStderr(); });
    QObject::connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        if (onFinished) {
            onFinished(exitCode);
        }
    });
}

bool Jam2Process::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

void Jam2Process::start(const QString& program, const QStringList& arguments)
{
    if (isRunning()) {
        return;
    }
    process_.setProgram(program);
    process_.setArguments(arguments);
    process_.setWorkingDirectory(QCoreApplication::applicationDirPath());
    process_.start();
}

void Jam2Process::stop()
{
    if (!isRunning()) {
        return;
    }
    sendLine(QStringLiteral("quit"));
    if (!process_.waitForFinished(1500)) {
        process_.terminate();
    }
}

void Jam2Process::sendLine(const QString& line)
{
    if (!isRunning()) {
        return;
    }
    process_.write((line + QLatin1Char('\n')).toUtf8());
}

void Jam2Process::drainStdout()
{
    stdoutBuffer_ += QString::fromUtf8(process_.readAllStandardOutput());
    int newline = -1;
    while ((newline = stdoutBuffer_.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = stdoutBuffer_.left(newline).trimmed();
        stdoutBuffer_.remove(0, newline + 1);
        handleLine(line);
    }
}

void Jam2Process::drainStderr()
{
    stderrBuffer_ += QString::fromUtf8(process_.readAllStandardError());
    int newline = -1;
    while ((newline = stderrBuffer_.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = stderrBuffer_.left(newline).trimmed();
        stderrBuffer_.remove(0, newline + 1);
        if (onErrorLine && !line.isEmpty()) {
            onErrorLine(line);
        }
    }
}

void Jam2Process::handleLine(const QString& line)
{
    if (line.isEmpty()) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
    if (doc.isObject() && onStatus) {
        onStatus(doc.object());
    }
    if (onOutputLine) {
        onOutputLine(line);
    }
}
