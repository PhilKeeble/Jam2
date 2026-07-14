#pragma once

#include "cli_entry.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <thread>

class Jam2Process : public QObject {
public:
    explicit Jam2Process(QObject* parent = nullptr);
    ~Jam2Process() override;

    bool isRunning() const;
    void start(const QString& program, const QStringList& arguments, bool installEmbeddedHost = true);
    void stop();
    bool sendControl(const QByteArray& payload);
    bool updatePeers(const QStringList& peers);

    std::function<void(const QString&)> onOutputLine;
    std::function<void(const QString&)> onErrorLine;
    std::function<void(quint16, const QByteArray&)> onControlFrame;
    std::function<void(int)> onFinished;

private:
    void deliverOutput(const QString& line, bool error);

    std::atomic<bool> running_{false};
    std::thread worker_;
    Jam2CliHost host_;
};
