#pragma once

#include <QJsonObject>
#include <QProcess>
#include <QStringList>

#include <functional>

class Jam2Process : public QObject {
public:
    explicit Jam2Process(QObject* parent = nullptr);

    bool isRunning() const;
    void start(const QString& program, const QStringList& arguments);
    void stop();
    void sendLine(const QString& line);

    std::function<void(const QString&)> onOutputLine;
    std::function<void(const QString&)> onErrorLine;
    std::function<void(const QJsonObject&)> onStatus;
    std::function<void(int)> onFinished;

private:
    void drainStdout();
    void drainStderr();
    void handleLine(const QString& line);

    QProcess process_;
    QString stdoutBuffer_;
    QString stderrBuffer_;
};
