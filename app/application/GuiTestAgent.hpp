#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class AutomationChannel;
class QApplication;
class MainWindow;

// A deliberately private, reactive GUI endpoint. It is reachable only through
// inherited automation handles and always drives the real widget tree.
class GuiTestAgent final : public QObject {
public:
    GuiTestAgent(MainWindow& window, QString instanceId, bool shown, QObject* parent = nullptr);
    ~GuiTestAgent() override;

    GuiTestAgent(const GuiTestAgent&) = delete;
    GuiTestAgent& operator=(const GuiTestAgent&) = delete;

    bool start(QString& error);

private:
    void enqueue(QJsonObject command);
    void drain();
    void handle(const QJsonObject& command);
    void emitEvent(QString event, QJsonObject fields = {});
    QJsonObject controlInventoryPage(int cursor, bool includeState);
    bool invokeControl(const QJsonObject& command, QString& error);
    void reject(const QJsonObject& command, const QString& reason);

    MainWindow& window_;
    QString instanceId_;
    bool shown_ = false;
    std::unique_ptr<AutomationChannel> channel_;
    std::mutex commandMutex_;
    std::deque<QJsonObject> commands_;
    std::vector<QJsonObject> pagedControls_;
    bool pagedControlsIncludeState_ = false;
    bool drainScheduled_ = false;
    bool stopping_ = false;
    std::atomic<std::uint64_t> rejectedCommands_{0};
    std::atomic<std::uint64_t> commandQueueDrops_{0};
    std::atomic<std::uint64_t> rejectedEvents_{0};
};

int jam2RunGuiTestAgent(QApplication& application, int argc, char* argv[]);
