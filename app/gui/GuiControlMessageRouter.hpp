#pragma once

class MainWindow;
class QJsonObject;
class QString;

class GuiControlMessageRouter final {
public:
    static void dispatch(
        MainWindow& window,
        const QJsonObject& message,
        const QString& sourcePeerToken);
};
