#include "GuiControlContract.hpp"

#include <QObject>
#include <QVariant>

#include <utility>

namespace {

constexpr auto kIdProperty = "jam2.test.id";
constexpr auto kContractProperty = "jam2.test.contract";
constexpr auto kAvailabilityProperty = "jam2.test.availability";
constexpr auto kFamilyProperty = "jam2.test.family";
constexpr auto kExclusionProperty = "jam2.test.exclusion";

QString propertyString(const QObject& control, const char* name)
{
    return control.property(name).toString();
}

} // namespace

namespace jam2::gui {

QString guiControlAvailabilityName(GuiControlAvailability availability)
{
    switch (availability) {
    case GuiControlAvailability::Always: return QStringLiteral("always");
    case GuiControlAvailability::StateGated: return QStringLiteral("state-gated");
    case GuiControlAvailability::Modal: return QStringLiteral("modal");
    case GuiControlAvailability::FileDialog: return QStringLiteral("file-dialog");
    case GuiControlAvailability::HardwareProfile: return QStringLiteral("hardware-profile");
    }
    return {};
}

GuiVirtualControl makeGuiVirtualControl(
    QString id,
    QString contract,
    GuiControlAvailability availability,
    QStringList operations,
    QString family,
    QVariantMap state,
    QString kind)
{
    return {
        std::move(id),
        std::move(contract),
        guiControlAvailabilityName(availability),
        std::move(family),
        std::move(kind),
        std::move(operations),
        std::move(state),
    };
}

void registerGuiControl(
    QObject& control,
    QString id,
    QString contract,
    GuiControlAvailability availability,
    QString family)
{
    control.setProperty(kIdProperty, std::move(id));
    control.setProperty(kContractProperty, std::move(contract));
    control.setProperty(kAvailabilityProperty, guiControlAvailabilityName(availability));
    control.setProperty(kFamilyProperty, std::move(family));
    control.setProperty(kExclusionProperty, QString{});
}

void excludeFromGuiControlInventory(QObject& control, QString reason)
{
    control.setProperty(kExclusionProperty, std::move(reason));
    control.setProperty(kIdProperty, QString{});
    control.setProperty(kContractProperty, QString{});
    control.setProperty(kAvailabilityProperty, QString{});
    control.setProperty(kFamilyProperty, QString{});
}

QString guiControlId(const QObject& control)
{
    return propertyString(control, kIdProperty);
}

QString guiControlContract(const QObject& control)
{
    return propertyString(control, kContractProperty);
}

QString guiControlAvailability(const QObject& control)
{
    return propertyString(control, kAvailabilityProperty);
}

QString guiControlFamily(const QObject& control)
{
    return propertyString(control, kFamilyProperty);
}

QString guiControlExclusion(const QObject& control)
{
    return propertyString(control, kExclusionProperty);
}

bool hasCompleteGuiControlContract(const QObject& control)
{
    return !guiControlId(control).isEmpty() &&
        !guiControlContract(control).isEmpty() &&
        !guiControlAvailability(control).isEmpty();
}

} // namespace jam2::gui
