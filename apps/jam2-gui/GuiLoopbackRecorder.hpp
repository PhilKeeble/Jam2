#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <thread>

struct GuiLoopbackOptions {
    QString source = QStringLiteral("default");
    QString outputPath;
    int durationMs = 0;
    bool trigger = false;
    double triggerThresholdDb = -45.0;
    int triggerHoldMs = 50;
    int preRollMs = 250;
    double tailSilenceDb = -50.0;
    int tailSilenceMs = 1000;
    bool trimLeadingSilence = true;
    bool trimTrailingSilence = true;
};

class GuiLoopbackRecorder {
public:
    using FinishedCallback = std::function<void(bool ok, const QString& outputPath, const QString& error)>;

    GuiLoopbackRecorder();
    ~GuiLoopbackRecorder();

    GuiLoopbackRecorder(const GuiLoopbackRecorder&) = delete;
    GuiLoopbackRecorder& operator=(const GuiLoopbackRecorder&) = delete;

    bool isRunning() const;
    bool start(const GuiLoopbackOptions& options, FinishedCallback finished, QString* error);
    void stop();

    static QStringList listSources(QString* error = nullptr);

private:
    void run(GuiLoopbackOptions options, FinishedCallback finished) noexcept;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};
