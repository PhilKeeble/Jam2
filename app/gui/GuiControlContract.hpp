#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

class QObject;

namespace jam2::gui {

// Describes when a real product interaction is expected to be operable. The
// value is diagnostic test metadata only; it never changes widget behavior.
enum class GuiControlAvailability {
    Always,
    StateGated,
    Modal,
    FileDialog,
    HardwareProfile,
};

struct GuiVirtualControl {
    QString id;
    QString contract;
    QString availability;
    QString family;
    QString kind = QStringLiteral("virtual-target");
    QStringList operations;
    QVariantMap state;
};

// Custom-painted widgets expose their real hit targets through this interface.
// This keeps those interactions in the same inventory as ordinary Qt controls
// without manufacturing child buttons that would change the product UI.
class GuiVirtualControlProvider {
public:
    virtual ~GuiVirtualControlProvider() = default;
    virtual QVector<GuiVirtualControl> guiVirtualControls() const = 0;
    virtual bool invokeGuiVirtualControl(
        const QString& id,
        const QString& operation,
        const QVariant& value,
        QString& error) = 0;
};

QString guiControlAvailabilityName(GuiControlAvailability availability);
GuiVirtualControl makeGuiVirtualControl(
    QString id,
    QString contract,
    GuiControlAvailability availability,
    QStringList operations,
    QString family = {},
    QVariantMap state = {},
    QString kind = QStringLiteral("virtual-target"));

// Registers a semantic product control with the private GUI automation
// inventory. Every id is unique in a live window. Controls produced from one
// repeated UI family additionally share a family name and are covered through
// a parameterized workflow contract.
void registerGuiControl(
    QObject& control,
    QString id,
    QString contract,
    GuiControlAvailability availability = GuiControlAvailability::Always,
    QString family = {});

// Accounts for a widget-shaped state holder that is never a user interaction
// itself (for example, a hidden value mirrored by a real dialog control).
void excludeFromGuiControlInventory(QObject& control, QString reason);

QString guiControlId(const QObject& control);
QString guiControlContract(const QObject& control);
QString guiControlAvailability(const QObject& control);
QString guiControlFamily(const QObject& control);
QString guiControlExclusion(const QObject& control);
bool hasCompleteGuiControlContract(const QObject& control);

} // namespace jam2::gui
