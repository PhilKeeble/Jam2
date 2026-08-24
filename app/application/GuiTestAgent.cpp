#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "GuiTestAgent.hpp"

#include "AutomationChannel.hpp"
#include "MainWindow.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"
#include "UserPreferences.hpp"
#include "MidiInputBackend.hpp"
#include "InputPluginBackend.hpp"
#include "common.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr auto kProtocol = "jam2-gui-test-agent";
constexpr int kPageSize = 32;
constexpr qsizetype kMaximumTextBytes = 1024;
constexpr qsizetype kMaximumCommandTextBytes = 4096;

class HiddenGuiWindowFilter final : public QObject {
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event != nullptr && event->type() == QEvent::Show) {
            if (auto* messageBox = qobject_cast<QMessageBox*>(watched)) {
                // QMessageBox asks the platform to play an alert based on its
                // icon during showEvent(). Hidden GUI-agent runs should not
                // produce desktop UI or its associated system sound.
                messageBox->setIcon(QMessageBox::NoIcon);
            }
        }
        if (event != nullptr &&
            (event->type() == QEvent::Polish || event->type() == QEvent::Show)) {
            if (auto* widget = qobject_cast<QWidget*>(watched);
                widget != nullptr && widget->isWindow()) {
                widget->setAttribute(Qt::WA_DontShowOnScreen, true);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

QString boundedText(QString text)
{
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= kMaximumTextBytes) return text;
    utf8.truncate(kMaximumTextBytes);
    return QString::fromUtf8(utf8) + QStringLiteral("...");
}

bool isInteractive(QObject* object)
{
    if (!jam2::gui::guiControlExclusion(*object).isEmpty()) return false;
    const auto hasAncestor = [object](auto* typeTag) {
        using Type = std::remove_pointer_t<decltype(typeTag)>;
        for (QObject* parent = object->parent(); parent != nullptr; parent = parent->parent()) {
            if (qobject_cast<Type*>(parent) != nullptr) return true;
        }
        return false;
    };
    // Composite Qt controls own implementation widgets that are not separate
    // product interactions. Inventory the combo/spin control once rather than
    // also treating its editor and popup view as independent Jam2 controls.
    if (qobject_cast<QLineEdit*>(object) != nullptr &&
        (hasAncestor(static_cast<QAbstractSpinBox*>(nullptr)) ||
         hasAncestor(static_cast<QComboBox*>(nullptr)))) {
        return false;
    }
    if (qobject_cast<QAbstractItemView*>(object) != nullptr &&
        hasAncestor(static_cast<QComboBox*>(nullptr))) {
        return false;
    }
    // Header/corner widgets are implementation children of the owning item
    // view. Any supported sorting, selection, or resize behavior belongs to
    // that one semantic table/list contract.
    if (qobject_cast<QHeaderView*>(object) != nullptr ||
        object->objectName() == QStringLiteral("qt_tableview_cornerbutton")) {
        return false;
    }
    // Read-only text panes are views. They are audited with view/state output,
    // not advertised as invokable controls with an empty operation set.
    if (const auto* edit = qobject_cast<QPlainTextEdit*>(object);
        edit != nullptr && edit->isReadOnly()) {
        return false;
    }
    return qobject_cast<QAbstractButton*>(object) != nullptr ||
        qobject_cast<QComboBox*>(object) != nullptr ||
        qobject_cast<QSpinBox*>(object) != nullptr ||
        qobject_cast<QDoubleSpinBox*>(object) != nullptr ||
        qobject_cast<QSlider*>(object) != nullptr ||
        qobject_cast<QLineEdit*>(object) != nullptr ||
        qobject_cast<QPlainTextEdit*>(object) != nullptr ||
        qobject_cast<QTabWidget*>(object) != nullptr ||
        qobject_cast<QAbstractItemView*>(object) != nullptr ||
        qobject_cast<QAction*>(object) != nullptr;
}

QString controlKind(QObject* object)
{
    if (qobject_cast<QAbstractButton*>(object)) return QStringLiteral("button");
    if (qobject_cast<QComboBox*>(object)) return QStringLiteral("combo-box");
    if (qobject_cast<QSpinBox*>(object)) return QStringLiteral("integer-spin-box");
    if (qobject_cast<QDoubleSpinBox*>(object)) return QStringLiteral("number-spin-box");
    if (qobject_cast<QSlider*>(object)) return QStringLiteral("slider");
    if (qobject_cast<QLineEdit*>(object)) return QStringLiteral("line-edit");
    if (qobject_cast<QPlainTextEdit*>(object)) return QStringLiteral("plain-text-edit");
    if (qobject_cast<QTabWidget*>(object)) return QStringLiteral("tab-widget");
    if (qobject_cast<QAbstractItemView*>(object)) return QStringLiteral("item-view");
    if (qobject_cast<QAction*>(object)) return QStringLiteral("action");
    return QStringLiteral("unknown");
}

QJsonArray controlOperations(QObject* object)
{
    QJsonArray operations;
    if (const auto* button = qobject_cast<QAbstractButton*>(object)) {
        operations.push_back(QStringLiteral("click"));
        if (jam2::gui::guiControlAvailability(*object) == QStringLiteral("modal")) {
            operations.push_back(QStringLiteral("click-async"));
        }
        if (button->isCheckable()) operations.push_back(QStringLiteral("set-checked"));
    } else if (qobject_cast<QComboBox*>(object)) {
        operations.push_back(QStringLiteral("set-index"));
        operations.push_back(QStringLiteral("activate-index"));
    } else if (qobject_cast<QSpinBox*>(object) ||
               qobject_cast<QDoubleSpinBox*>(object) ||
               qobject_cast<QSlider*>(object)) {
        operations.push_back(QStringLiteral("set-value"));
    } else if (const auto* edit = qobject_cast<QLineEdit*>(object)) {
        if (!edit->isReadOnly()) operations.push_back(QStringLiteral("set-text"));
    } else if (const auto* edit = qobject_cast<QPlainTextEdit*>(object)) {
        if (!edit->isReadOnly()) operations.push_back(QStringLiteral("set-text"));
    } else if (qobject_cast<QListWidget*>(object)) {
        operations.push_back(QStringLiteral("set-current-row"));
    } else if (qobject_cast<QTabWidget*>(object)) {
        operations.push_back(QStringLiteral("set-index"));
    } else if (qobject_cast<QAction*>(object)) {
        operations.push_back(QStringLiteral("trigger"));
    }
    return operations;
}

QString diagnosticPath(QObject* object)
{
    QStringList segments;
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        const QString objectName = current->objectName();
        int ordinal = 0;
        if (QObject* parent = current->parent()) {
            for (QObject* sibling : parent->children()) {
                if (sibling == current) break;
                if (sibling->metaObject() == current->metaObject() &&
                    sibling->objectName() == objectName) {
                    ++ordinal;
                }
            }
        }
        segments.prepend(className + QLatin1Char('[') + QString::number(ordinal) +
            QLatin1Char(']') + (objectName.isEmpty() ? QString{} : QLatin1Char('#') + objectName));
    }
    return segments.join(QLatin1Char('/'));
}

std::vector<QObject*> interactiveControls(QWidget& window)
{
    std::vector<QObject*> controls;
    const auto objects = window.findChildren<QObject*>(QString{}, Qt::FindChildrenRecursively);
    controls.reserve(static_cast<std::size_t>(objects.size()));
    for (QObject* object : objects) {
        if (isInteractive(object)) controls.push_back(object);
    }
    std::sort(controls.begin(), controls.end(), [](QObject* left, QObject* right) {
        const QString leftId = jam2::gui::guiControlId(*left);
        const QString rightId = jam2::gui::guiControlId(*right);
        if (leftId.isEmpty() != rightId.isEmpty()) return !leftId.isEmpty();
        if (leftId != rightId) return leftId < rightId;
        return diagnosticPath(left) < diagnosticPath(right);
    });
    return controls;
}

QJsonObject controlState(QObject* object)
{
    QJsonObject state;
    if (const auto* widget = qobject_cast<QWidget*>(object)) {
        state.insert(QStringLiteral("enabled"), widget->isEnabled());
        state.insert(QStringLiteral("visible"), widget->isVisible());
    }
    if (const auto* button = qobject_cast<QAbstractButton*>(object)) {
        state.insert(QStringLiteral("text"), boundedText(button->text()));
        state.insert(QStringLiteral("checkable"), button->isCheckable());
        if (button->isCheckable()) state.insert(QStringLiteral("checked"), button->isChecked());
    } else if (const auto* combo = qobject_cast<QComboBox*>(object)) {
        state.insert(QStringLiteral("index"), combo->currentIndex());
        state.insert(QStringLiteral("text"), boundedText(combo->currentText()));
        state.insert(QStringLiteral("count"), combo->count());
    } else if (const auto* spin = qobject_cast<QSpinBox*>(object)) {
        state.insert(QStringLiteral("value"), spin->value());
        state.insert(QStringLiteral("minimum"), spin->minimum());
        state.insert(QStringLiteral("maximum"), spin->maximum());
    } else if (const auto* spin = qobject_cast<QDoubleSpinBox*>(object)) {
        state.insert(QStringLiteral("value"), spin->value());
        state.insert(QStringLiteral("minimum"), spin->minimum());
        state.insert(QStringLiteral("maximum"), spin->maximum());
    } else if (const auto* slider = qobject_cast<QSlider*>(object)) {
        state.insert(QStringLiteral("value"), slider->value());
        state.insert(QStringLiteral("minimum"), slider->minimum());
        state.insert(QStringLiteral("maximum"), slider->maximum());
    } else if (const auto* edit = qobject_cast<QLineEdit*>(object)) {
        if (edit->echoMode() == QLineEdit::Normal) {
            state.insert(QStringLiteral("text"), boundedText(edit->text()));
        } else {
            state.insert(QStringLiteral("text_length"), edit->text().size());
        }
        state.insert(QStringLiteral("read_only"), edit->isReadOnly());
    } else if (const auto* edit = qobject_cast<QPlainTextEdit*>(object)) {
        state.insert(QStringLiteral("text_length"), edit->toPlainText().size());
        state.insert(QStringLiteral("read_only"), edit->isReadOnly());
    } else if (const auto* list = qobject_cast<QListWidget*>(object)) {
        state.insert(QStringLiteral("current_row"), list->currentRow());
        state.insert(QStringLiteral("count"), list->count());
        state.insert(QStringLiteral("text"), list->currentItem()
            ? boundedText(list->currentItem()->text()) : QString{});
    } else if (const auto* tabs = qobject_cast<QTabWidget*>(object)) {
        state.insert(QStringLiteral("index"), tabs->currentIndex());
        state.insert(QStringLiteral("count"), tabs->count());
        state.insert(QStringLiteral("text"), tabs->currentIndex() >= 0
            ? boundedText(tabs->tabText(tabs->currentIndex())) : QString{});
    } else if (const auto* action = qobject_cast<QAction*>(object)) {
        state.insert(QStringLiteral("enabled"), action->isEnabled());
        state.insert(QStringLiteral("visible"), action->isVisible());
        state.insert(QStringLiteral("text"), boundedText(action->text()));
        state.insert(QStringLiteral("checkable"), action->isCheckable());
        if (action->isCheckable()) state.insert(QStringLiteral("checked"), action->isChecked());
    }
    return state;
}

struct VirtualControlRef {
    QObject* owner = nullptr;
    jam2::gui::GuiVirtualControlProvider* provider = nullptr;
    jam2::gui::GuiVirtualControl control;
};

std::vector<VirtualControlRef> virtualControls(QWidget& window)
{
    std::vector<VirtualControlRef> controls;
    const auto appendProvider = [&controls](QObject* object) {
        auto* provider = dynamic_cast<jam2::gui::GuiVirtualControlProvider*>(object);
        if (!provider) return;
        const QVector<jam2::gui::GuiVirtualControl> provided = provider->guiVirtualControls();
        controls.reserve(controls.size() + static_cast<std::size_t>(provided.size()));
        for (const auto& control : provided) {
            controls.push_back({object, provider, control});
        }
    };
    appendProvider(&window);
    const auto objects = window.findChildren<QObject*>(QString{}, Qt::FindChildrenRecursively);
    for (QObject* object : objects) appendProvider(object);
    std::sort(controls.begin(), controls.end(), [](const auto& left, const auto& right) {
        if (left.control.id != right.control.id) return left.control.id < right.control.id;
        return diagnosticPath(left.owner) < diagnosticPath(right.owner);
    });
    return controls;
}

std::vector<QJsonObject> controlInventoryItems(QWidget& window, bool includeState)
{
    std::vector<QJsonObject> items;
    const auto realControls = interactiveControls(window);
    const auto customControls = virtualControls(window);
    items.reserve(realControls.size() + customControls.size());
    for (QObject* object : realControls) {
        QJsonObject item{
            {QStringLiteral("test_id"), jam2::gui::guiControlId(*object)},
            {QStringLiteral("contract"), jam2::gui::guiControlContract(*object)},
            {QStringLiteral("availability"), jam2::gui::guiControlAvailability(*object)},
            {QStringLiteral("family"), jam2::gui::guiControlFamily(*object)},
            {QStringLiteral("classified"),
                jam2::gui::hasCompleteGuiControlContract(*object)},
            {QStringLiteral("diagnostic_path"), diagnosticPath(object)},
            {QStringLiteral("object_name"), object->objectName()},
            {QStringLiteral("class"), QString::fromLatin1(object->metaObject()->className())},
            {QStringLiteral("kind"), controlKind(object)},
            {QStringLiteral("operations"), controlOperations(object)},
        };
        if (includeState) item.insert(QStringLiteral("state"), controlState(object));
        items.push_back(std::move(item));
    }
    for (const auto& entry : customControls) {
        const auto& control = entry.control;
        QStringList operations = control.operations;
        if (control.availability == QStringLiteral("modal") &&
            operations.contains(QStringLiteral("click")) &&
            !operations.contains(QStringLiteral("click-async"))) {
            operations.append(QStringLiteral("click-async"));
        }
        const bool classified = !control.id.isEmpty() && !control.contract.isEmpty() &&
            !control.availability.isEmpty() && !operations.isEmpty();
        QJsonObject item{
            {QStringLiteral("test_id"), control.id},
            {QStringLiteral("contract"), control.contract},
            {QStringLiteral("availability"), control.availability},
            {QStringLiteral("family"), control.family},
            {QStringLiteral("classified"), classified},
            {QStringLiteral("diagnostic_path"),
                diagnosticPath(entry.owner) + QStringLiteral("/virtual#") + control.id},
            {QStringLiteral("object_name"), entry.owner->objectName()},
            {QStringLiteral("class"),
                QString::fromLatin1(entry.owner->metaObject()->className())},
            {QStringLiteral("kind"), control.kind},
            {QStringLiteral("operations"), QJsonArray::fromStringList(operations)},
        };
        if (includeState) {
            item.insert(QStringLiteral("state"), QJsonObject::fromVariantMap(control.state));
        }
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const QJsonObject& left, const QJsonObject& right) {
        const QString leftId = left.value(QStringLiteral("test_id")).toString();
        const QString rightId = right.value(QStringLiteral("test_id")).toString();
        if (leftId.isEmpty() != rightId.isEmpty()) return !leftId.isEmpty();
        if (leftId != rightId) return leftId < rightId;
        return left.value(QStringLiteral("diagnostic_path")).toString() <
            right.value(QStringLiteral("diagnostic_path")).toString();
    });
    return items;
}

bool boundedRequestId(const QJsonObject& command)
{
    if (!command.contains(QStringLiteral("id"))) return true;
    const QJsonValue value = command.value(QStringLiteral("id"));
    return value.isString() && !value.toString().isEmpty() && value.toString().toUtf8().size() <= 128;
}

bool exactInteger(const QJsonValue& value, int minimum, int maximum, int& output)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < minimum || number > maximum) return false;
    output = static_cast<int>(number);
    return true;
}

bool onlyFields(const QJsonObject& command, std::initializer_list<const char*> fields)
{
    QSet<QString> allowed;
    for (const char* field : fields) allowed.insert(QString::fromLatin1(field));
    for (auto it = command.begin(); it != command.end(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

} // namespace

GuiTestAgent::GuiTestAgent(MainWindow& window, QString instanceId, bool shown, QObject* parent)
    : QObject(parent), window_(window), instanceId_(std::move(instanceId)), shown_(shown)
{
}

GuiTestAgent::~GuiTestAgent()
{
    if (channel_) channel_->stop(false);
}

bool GuiTestAgent::start(QString& error)
{
    std::string channelError;
    channel_ = AutomationChannel::fromInheritedEnvironment(true, channelError);
    if (!channel_) {
        error = QString::fromStdString(channelError.empty()
            ? std::string("GUI test agent requires inherited automation handles")
            : channelError);
        return false;
    }
    QPointer<GuiTestAgent> self(this);
    channel_->start(
        [self](QJsonObject command) {
            if (self) self->enqueue(std::move(command));
        },
        [self](std::string disconnectError) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, disconnectError = std::move(disconnectError)] {
                if (!self || self->stopping_) return;
                std::cerr << disconnectError << '\n';
                QCoreApplication::exit(5);
            }, Qt::QueuedConnection);
        });

    const auto controls = controlInventoryItems(window_, false);
    QSet<QString> stableIds;
    int duplicateIds = 0;
    int stableCount = 0;
    int classifiedCount = 0;
    int incompleteRegisteredCount = 0;
    int excludedCount = 0;
    const auto allObjects = window_.findChildren<QObject*>(
        QString{}, Qt::FindChildrenRecursively);
    for (QObject* object : allObjects) {
        if (!jam2::gui::guiControlExclusion(*object).isEmpty()) ++excludedCount;
    }
    int virtualCount = 0;
    for (const QJsonObject& control : controls) {
        const QString id = control.value(QStringLiteral("test_id")).toString();
        if (control.value(QStringLiteral("classified")).toBool()) {
            ++classifiedCount;
        } else if (!id.isEmpty()) {
            ++incompleteRegisteredCount;
        }
        if (control.value(QStringLiteral("kind")).toString() ==
            QStringLiteral("virtual-target")) {
            ++virtualCount;
        }
        if (id.isEmpty()) continue;
        ++stableCount;
        if (stableIds.contains(id)) ++duplicateIds;
        stableIds.insert(id);
    }
    emitEvent(QStringLiteral("hello"), {
        {QStringLiteral("protocol"), QString::fromLatin1(kProtocol)},
        {QStringLiteral("instance_id"), instanceId_},
        {QStringLiteral("shown"), shown_},
        {QStringLiteral("page_size"), kPageSize},
        {QStringLiteral("control_count"), static_cast<qint64>(controls.size())},
        {QStringLiteral("stable_control_count"), stableCount},
        {QStringLiteral("unnamed_control_count"), static_cast<qint64>(controls.size()) - stableCount},
        {QStringLiteral("classified_control_count"), classifiedCount},
        {QStringLiteral("unclassified_control_count"),
            static_cast<qint64>(controls.size()) - classifiedCount},
        {QStringLiteral("incomplete_registered_control_count"), incompleteRegisteredCount},
        {QStringLiteral("excluded_control_count"), excludedCount},
        {QStringLiteral("virtual_control_count"), virtualCount},
        {QStringLiteral("duplicate_control_id_count"), duplicateIds},
        {QStringLiteral("max_frame_bytes"), static_cast<qint64>(AutomationChannel::kMaxFrameBytes)},
        {QStringLiteral("queue_capacity"), static_cast<qint64>(AutomationChannel::kQueueCapacity)},
        {QStringLiteral("commands_per_turn"), static_cast<qint64>(AutomationChannel::kCommandsPerTurn)},
    });
    return true;
}

void GuiTestAgent::enqueue(QJsonObject command)
{
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (commands_.size() >= AutomationChannel::kQueueCapacity) {
            commandQueueDrops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        commands_.push_back(std::move(command));
        if (!drainScheduled_) {
            drainScheduled_ = true;
            schedule = true;
        }
    }
    if (!schedule) return;
    QPointer<GuiTestAgent> self(this);
    QMetaObject::invokeMethod(this, [self] { if (self) self->drain(); }, Qt::QueuedConnection);
}

void GuiTestAgent::drain()
{
    std::vector<QJsonObject> commands;
    commands.reserve(AutomationChannel::kCommandsPerTurn);
    bool reschedule = false;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        while (!commands_.empty() && commands.size() < AutomationChannel::kCommandsPerTurn) {
            commands.push_back(std::move(commands_.front()));
            commands_.pop_front();
        }
        reschedule = !commands_.empty();
        drainScheduled_ = reschedule;
    }
    for (const auto& command : commands) {
        handle(command);
        if (stopping_) return;
    }
    if (reschedule) {
        QPointer<GuiTestAgent> self(this);
        QMetaObject::invokeMethod(this, [self] { if (self) self->drain(); }, Qt::QueuedConnection);
    }
}

void GuiTestAgent::handle(const QJsonObject& command)
{
    const QString type = command.value(QStringLiteral("type")).toString();
    if (!boundedRequestId(command)) {
        reject(command, QStringLiteral("id must be a bounded non-empty string"));
        return;
    }
    if (type == QStringLiteral("inventory") || type == QStringLiteral("snapshot")) {
        const bool exactControlSnapshot = type == QStringLiteral("snapshot") &&
            command.contains(QStringLiteral("control"));
        const bool fieldsOk = exactControlSnapshot
            ? onlyFields(command, {"format", "type", "id", "control"})
            : onlyFields(command, {"format", "type", "id", "cursor"});
        if (!fieldsOk) {
            reject(command, QStringLiteral("unsupported field for inventory or snapshot"));
            return;
        }
        QJsonObject page;
        if (exactControlSnapshot) {
            const QJsonValue requestedValue = command.value(QStringLiteral("control"));
            const QString requested = requestedValue.toString();
            if (!requestedValue.isString() || requested.isEmpty() ||
                requested.toUtf8().size() > 256) {
                reject(command,
                    QStringLiteral("control must be a bounded non-empty string"));
                return;
            }
            const auto controls = controlInventoryItems(window_, true);
            QJsonObject match;
            int matches = 0;
            for (const QJsonObject& control : controls) {
                if (control.value(QStringLiteral("test_id")).toString() != requested) {
                    continue;
                }
                match = control;
                ++matches;
            }
            if (matches > 1) {
                reject(command, QStringLiteral("control id is duplicated"));
                return;
            }
            page = {
                {QStringLiteral("protocol"), QString::fromLatin1(kProtocol)},
                {QStringLiteral("requested_control"), requested},
                {QStringLiteral("control_found"), matches == 1},
            };
            if (matches == 1) page.insert(QStringLiteral("control"), match);
        } else {
            int cursor = 0;
            if (command.contains(QStringLiteral("cursor")) &&
                !exactInteger(command.value(QStringLiteral("cursor")), 0,
                    std::numeric_limits<int>::max(), cursor)) {
                reject(command, QStringLiteral("cursor must be a non-negative integer"));
                return;
            }
            page = controlInventoryPage(cursor, type == QStringLiteral("snapshot"));
        }
        page.insert(QStringLiteral("id"), command.value(QStringLiteral("id")));
        if (type == QStringLiteral("snapshot")) {
            page.insert(QStringLiteral("window"), QJsonObject{
                {QStringLiteral("title"), boundedText(window_.windowTitle())},
                {QStringLiteral("visible"), window_.isVisible()},
                {QStringLiteral("enabled"), window_.isEnabled()},
                {QStringLiteral("active_modal"), QApplication::activeModalWidget() != nullptr},
            });
            page.insert(QStringLiteral("jam"), window_.automationJamSnapshot());
            page.insert(QStringLiteral("content"), window_.automationContentSnapshot());
            page.insert(QStringLiteral("performance"), window_.automationPerformanceSnapshot());
        }
        emitEvent(type, std::move(page));
        return;
    }
    if (type == QStringLiteral("invoke")) {
        if (!onlyFields(command, {"format", "type", "id", "control", "operation", "value"})) {
            reject(command, QStringLiteral("unsupported field for invoke"));
            return;
        }
        QString error;
        bool completionDeferred = false;
        if (!invokeControl(command, error, completionDeferred)) {
            reject(command, error);
            return;
        }
        if (completionDeferred) return;
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("control"), command.value(QStringLiteral("control"))},
            {QStringLiteral("operation"), command.value(QStringLiteral("operation"))},
        });
        return;
    }
    if (type == QStringLiteral("window.close")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("unsupported field for window.close"));
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
        });
        QPointer<MainWindow> window(&window_);
        QTimer::singleShot(0, this, [window] {
            if (window) window->close();
        });
        return;
    }
    if (type == QStringLiteral("midi.inject")) {
        if (!onlyFields(command, {
                "format", "type", "id", "device", "status", "data1", "data2", "size"})) {
            reject(command, QStringLiteral("unsupported field for MIDI injection"));
            return;
        }
        const QJsonValue deviceValue = command.value(QStringLiteral("device"));
        const QString device = deviceValue.toString();
        int status = 0;
        int data1 = 0;
        int data2 = 0;
        int size = 0;
        if (!deviceValue.isString() || device.isEmpty() || device.toUtf8().size() > 256 ||
            !exactInteger(command.value(QStringLiteral("status")), 0, 255, status) ||
            !exactInteger(command.value(QStringLiteral("data1")), 0, 127, data1) ||
            !exactInteger(command.value(QStringLiteral("data2")), 0, 127, data2) ||
            !exactInteger(command.value(QStringLiteral("size")), 2, 3, size)) {
            reject(command, QStringLiteral("invalid bounded MIDI injection fields"));
            return;
        }
        jam2::midi::Event event;
        event.monotonic_us = jam2::monotonic_us();
        event.status = static_cast<std::uint8_t>(status);
        event.data1 = static_cast<std::uint8_t>(data1);
        event.data2 = static_cast<std::uint8_t>(data2);
        event.size = static_cast<std::uint8_t>(size);
        std::string error;
        if (!window_.midiInputBackend_->inject(
                device.toStdString(), event, error)) {
            reject(command, QString::fromStdString(error));
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("device"), device},
            {QStringLiteral("status"), status},
        });
        return;
    }
    if (type == QStringLiteral("jam.create") || type == QStringLiteral("jam.join")) {
        const bool create = type == QStringLiteral("jam.create");
        const bool fieldsOk = create
            ? onlyFields(command, {"format", "type", "id", "port", "test_input"})
            : onlyFields(command, {
                "format", "type", "id", "port", "invite_url", "test_input"});
        int port = 0;
        const QString testInput = command.value(QStringLiteral("test_input"))
            .toString(QStringLiteral("silence"));
        Jam2TestInputMode testInputMode = Jam2TestInputMode::Silence;
        if (testInput == QStringLiteral("off")) testInputMode = Jam2TestInputMode::Off;
        else if (testInput == QStringLiteral("tone-440")) {
            testInputMode = Jam2TestInputMode::Tone440;
        } else if (testInput == QStringLiteral("pulse-1s")) {
            testInputMode = Jam2TestInputMode::Pulse1s;
        } else if (testInput == QStringLiteral("metro-pulse")) {
            testInputMode = Jam2TestInputMode::MetroPulse;
        }
        const bool validTestInput = testInput == QStringLiteral("off") ||
            testInput == QStringLiteral("silence") ||
            testInput == QStringLiteral("tone-440") ||
            testInput == QStringLiteral("pulse-1s") ||
            testInput == QStringLiteral("metro-pulse");
        if (!fieldsOk ||
            !exactInteger(command.value(QStringLiteral("port")), 1024, 65535, port) ||
            !validTestInput ||
            (!create && (!command.value(QStringLiteral("invite_url")).isString() ||
                command.value(QStringLiteral("invite_url")).toString().isEmpty() ||
                command.value(QStringLiteral("invite_url")).toString().toUtf8().size() > 4096))) {
            reject(command, QStringLiteral("invalid automation jam startup fields"));
            return;
        }
        QString error;
        if (!window_.startAutomationJam(
                create,
                port,
                command.value(QStringLiteral("invite_url")).toString(),
                testInputMode,
                error)) {
            reject(command, error);
            return;
        }
        QJsonObject fields{
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("role"), create ? QStringLiteral("creator") : QStringLiteral("joiner")},
        };
        if (create) fields.insert(QStringLiteral("invite_url"), window_.meshInviteUrl());
        emitEvent(QStringLiteral("command_applied"), std::move(fields));
        return;
    }
    if (type == QStringLiteral("jam.dialog-runtime.prepare")) {
        if (!onlyFields(command, {"format", "type", "id", "test_input"})) {
            reject(command, QStringLiteral(
                "unsupported field for dialog runtime preparation"));
            return;
        }
        const QString testInput = command.value(QStringLiteral("test_input"))
            .toString(QStringLiteral("silence"));
        Jam2TestInputMode testInputMode = Jam2TestInputMode::Silence;
        if (testInput == QStringLiteral("off")) {
            testInputMode = Jam2TestInputMode::Off;
        } else if (testInput == QStringLiteral("tone-440")) {
            testInputMode = Jam2TestInputMode::Tone440;
        } else if (testInput == QStringLiteral("pulse-1s")) {
            testInputMode = Jam2TestInputMode::Pulse1s;
        } else if (testInput == QStringLiteral("metro-pulse")) {
            testInputMode = Jam2TestInputMode::MetroPulse;
        } else if (testInput != QStringLiteral("silence")) {
            reject(command, QStringLiteral("invalid dialog runtime test input"));
            return;
        }
        QString error;
        if (!window_.prepareAutomationDialogJam(testInputMode, error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("test_input"), testInput},
        });
        return;
    }
    if (type == QStringLiteral("application.local-dialog.open")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral(
                "unsupported field for local dialog open"));
            return;
        }
        QPointer<MainWindow> window(&window_);
        QTimer::singleShot(0, &window_, [window] {
            if (window) window->showLocalPerformSetup();
        });
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
        });
        return;
    }
    if (type == QStringLiteral("audio.device-preflight")) {
        int sampleRate = 0;
        if (!onlyFields(command, {"format", "type", "id", "sample_rate"}) ||
            !exactInteger(command.value(QStringLiteral("sample_rate")),
                8000, 384000, sampleRate)) {
            reject(command, QStringLiteral("sample_rate is invalid"));
            return;
        }
        const bool supported = window_.selectedDeviceSupportsSampleRate(sampleRate);
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("sample_rate"), sampleRate},
            {QStringLiteral("supported"), supported},
            {QStringLiteral("device"), window_.selectedDeviceDescription()},
        });
        return;
    }
    if (type == QStringLiteral("application.boundary")) {
        const QString action = command.value(QStringLiteral("action")).toString();
        if (!onlyFields(command, {"format", "type", "id", "action"}) ||
            action.isEmpty() || action.toUtf8().size() > 128) {
            reject(command, QStringLiteral("boundary action is invalid"));
            return;
        }
        const auto closeNextModal = [] {
            QTimer::singleShot(0, QCoreApplication::instance(), [] {
                if (QWidget* modal = QApplication::activeModalWidget()) {
                    modal->close();
                }
            });
        };
        if (action == QStringLiteral("session-maintenance")) {
            const ConnectionDiagnosticsSnapshot diagnostics =
                window_.lastDiagnostics_.value_or(ConnectionDiagnosticsSnapshot{});
            window_.handleConnectionDiagnostics(diagnostics);
            window_.notePreAuthenticationDisconnect();
            window_.refreshControlConnection();
            window_.restoreSessionHeaderStatus();
            window_.cleanupTransientTrackWavs();
            window_.requestRecordingGroupRecovery();
            window_.handleRecordingGroupRecovery({});
            window_.revealLooperLaneWav(-1);
        } else if (action == QStringLiteral("incoming-delay-presentation")) {
            ConnectionDiagnosticsSnapshot diagnostics;
            diagnostics.received_packets = 1000;
            diagnostics.packet_gap_samples = 1000;
            Jam2PeerDiagnostics peer;
            peer.peer_id = 2;
            peer.rtt_ms = 12.602;
            peer.has_rtt = true;
            peer.incoming_audio = jam2_estimate_incoming_audio_delay(
                true, peer.rtt_ms, true, 44100.0, 1408, 49, 3072, 154, 64);
            diagnostics.peers.push_back(peer);
            window_.handleConnectionDiagnostics(diagnostics);
        } else if (action == QStringLiteral("track-reload")) {
            window_.loadTrackJson(
                window_.trackToJson(), window_.trackController_.model());
        } else if (action == QStringLiteral("file-dialog-cancels")) {
            closeNextModal();
            window_.addLooperWavs();
            closeNextModal();
            window_.openSong();
        } else if (action == QStringLiteral("jamtaster-dialog-cancel")) {
            closeNextModal();
            window_.showJamTasterDialog();
        } else if (action == QStringLiteral("jamtaster-source-disposition")) {
            closeNextModal();
            if (!window_.promptJamTasterSourceDisposition().isEmpty()) {
                reject(command, QStringLiteral(
                    "JamTaster source-disposition cancel was not preserved"));
                return;
            }
        } else if (action == QStringLiteral("jamtaster-apply")) {
            const QJsonObject tempo{
                {QStringLiteral("bpm"), 136.75},
                {QStringLiteral("project_bpm"), 137},
                {QStringLiteral("beats_per_bar"), 7},
            };
            window_.applyJamTasterTempo(tempo);
            window_.applyJamTasterQuick(
                tempo,
                {},
                QJsonObject{
                    {QStringLiteral("tempo"), true},
                    {QStringLiteral("stems"), false},
                },
                {});
            window_.showJamTasterSessionHeaderStatus();
        } else if (action == QStringLiteral("save-session-defaults")) {
            window_.saveCreateDefaults();
            window_.saveJoinDefaults();
        } else if (action == QStringLiteral("recording-loopback")) {
            window_.laneRecordingState_.outputPath = QDir(
                window_.jamAssetFolder(JamStorage::AssetKind::Recorded))
                .absoluteFilePath(QStringLiteral("automation-loopback.wav"));
            if (window_.captureManualStopCheck_) {
                window_.captureManualStopCheck_->setChecked(true);
            }
            window_.startLoopbackCapture();
        } else if (action == QStringLiteral("jamtaster-error-boundaries")) {
            const QString missing = QDir(window_.jamStorage_.rootFolder())
                .absoluteFilePath(QStringLiteral("missing-jamtaster-result"));
            closeNextModal();
            window_.applyJamTasterConverted(missing, {});
            closeNextModal();
            window_.createJamTasterSong(missing, {}, {});
            closeNextModal();
            window_.showJamRecordingImportDialog(missing);
        } else if (action == QStringLiteral("recording-schedule")) {
            (void)window_.scheduleLoopbackCountIn(1, false);
            window_.cancelLoopbackCountIn();
            closeNextModal();
            window_.startArmedLooperLaneRecordingNow(0);
            closeNextModal();
            window_.startInputCapture(0, 0);
            window_.stopInputCapture(0);
        } else if (action == QStringLiteral("failure-presentation")) {
            window_.showJamFailure(QStringLiteral(
                "automation failure presentation boundary"));
        } else {
            reject(command, QStringLiteral("unsupported boundary action"));
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("action"), action},
            {QStringLiteral("jam"), window_.automationJamSnapshot()},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
            {QStringLiteral("performance"), window_.automationPerformanceSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("jam.leave")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("unsupported field for automation jam leave"));
            return;
        }
        const QString role = window_.automationJamSnapshot()
            .value(QStringLiteral("role")).toString();
        if (role != QStringLiteral("creator") && role != QStringLiteral("joiner")) {
            reject(command, QStringLiteral("automation jam is not active"));
            return;
        }
        window_.stopJam(true);
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("jam"), window_.automationJamSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("performance.metronome")) {
        int bpm = 0;
        if (!onlyFields(command, {"format", "type", "id", "enabled", "bpm"}) ||
            !command.value(QStringLiteral("enabled")).isBool() ||
            !exactInteger(command.value(QStringLiteral("bpm")), 1, 400, bpm)) {
            reject(command, QStringLiteral("metronome performance fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationSetMetronome(
                command.value(QStringLiteral("enabled")).toBool(), bpm, error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("performance"), window_.automationPerformanceSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("performance.transport")) {
        const QString action = command.value(QStringLiteral("action")).toString();
        if (!onlyFields(command, {"format", "type", "id", "action"}) ||
            (action != QStringLiteral("play") && action != QStringLiteral("stop"))) {
            reject(command, QStringLiteral("transport performance fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationSetGlobalPlayback(action == QStringLiteral("play"), error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("performance"), window_.automationPerformanceSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("performance.recording")) {
        const QString action = command.value(QStringLiteral("action")).toString();
        if (!onlyFields(command, {"format", "type", "id", "action"}) ||
            (action != QStringLiteral("start") && action != QStringLiteral("stop"))) {
            reject(command, QStringLiteral("recording performance fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationSetJamRecording(
                action == QStringLiteral("start"), error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("performance"), window_.automationPerformanceSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("jam.policy")) {
        if (!onlyFields(command, {
                "format", "type", "id", "track_lanes", "auto_share_wavs",
                "global_playback", "generated_ideas", "metronome_state", "recordings"})) {
            reject(command, QStringLiteral("unsupported field for Jam Sync policy"));
            return;
        }
        QJsonObject request = command;
        request.remove(QStringLiteral("format"));
        request.remove(QStringLiteral("id"));
        request.insert(QStringLiteral("type"), QStringLiteral("jam.sync.request"));
        JamSyncPolicy policy;
        QString error;
        if (!jam2ParseJamSyncPolicyMessage(request, policy, error)) {
            reject(command, error);
            return;
        }
        window_.requestJamSyncPolicy(policy);
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("role"), window_.automationJamSnapshot().value(QStringLiteral("role"))},
        });
        return;
    }
    if (type == QStringLiteral("song.rename")) {
        const QJsonValue title = command.value(QStringLiteral("title"));
        if (!onlyFields(command, {"format", "type", "id", "title"}) ||
            !title.isString() || title.toString().trimmed().isEmpty() ||
            title.toString().size() > 512) {
            reject(command, QStringLiteral("song title is invalid"));
            return;
        }
        QString error;
        if (!window_.automationRenameSong(title.toString(), error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("song.cell")) {
        int section = 0;
        int beat = 0;
        const QJsonValue lane = command.value(QStringLiteral("lane"));
        const QJsonValue value = command.value(QStringLiteral("value"));
        if (!onlyFields(command, {
                "format", "type", "id", "section", "lane", "beat", "value"}) ||
            !exactInteger(command.value(QStringLiteral("section")), 0, 255, section) ||
            !exactInteger(command.value(QStringLiteral("beat")), 0, 511, beat) ||
            !lane.isString() || !value.isString() || value.toString().size() > 4096) {
            reject(command, QStringLiteral("song cell fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationEditSongCell(
                section, lane.toString(), beat, value.toString(), error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("song.resize")) {
        int section = 0;
        int beats = 0;
        if (!onlyFields(command, {"format", "type", "id", "section", "beats"}) ||
            !exactInteger(command.value(QStringLiteral("section")), 0, 255, section) ||
            !exactInteger(command.value(QStringLiteral("beats")), 4, 512, beats)) {
            reject(command, QStringLiteral("song resize fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationResizeSongSection(section, beats, error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("idea.generate")) {
        int seed = 0;
        const QString parts = command.value(QStringLiteral("parts")).toString();
        const auto selected = parts == QStringLiteral("full")
            ? jam2::practice::PracticeIdeaParts::FullArrangement
            : parts == QStringLiteral("chords")
                ? jam2::practice::PracticeIdeaParts::PitchedPartsOnly
                : jam2::practice::PracticeIdeaParts::DrumsOnly;
        if (!onlyFields(command, {"format", "type", "id", "parts", "seed"}) ||
            (parts != QStringLiteral("full") && parts != QStringLiteral("chords") &&
             parts != QStringLiteral("beats")) ||
            !exactInteger(command.value(QStringLiteral("seed")),
                0, (std::numeric_limits<int>::max)(), seed)) {
            reject(command, QStringLiteral("idea generation fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationGenerateIdea(
                selected, static_cast<std::uint32_t>(seed), error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.transfer.pause")) {
        const QString point = command.value(QStringLiteral("point")).toString();
        if (!onlyFields(command, {"format", "type", "id", "point"}) ||
            (point != QStringLiteral("offer") &&
             point != QStringLiteral("outgoing-validation") &&
             point != QStringLiteral("incoming-chunk") &&
             point != QStringLiteral("incoming-finalize"))) {
            reject(command, QStringLiteral("transfer gate fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationArmTransferPause(point, error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("transfer"), window_.automationTransferSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.transfer.release")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("transfer gate release fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationReleaseTransferPause(error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("transfer"), window_.automationTransferSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.transfer.drop-start")) {
        int count = 1;
        if (!onlyFields(command, {"format", "type", "id", "count"}) ||
            (command.contains(QStringLiteral("count")) &&
             !exactInteger(command.value(QStringLiteral("count")), 1, 4, count))) {
            reject(command, QStringLiteral("transfer start-drop fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationDropOutgoingAssetStarts(count, error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("transfer"), window_.automationTransferSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.transfer.expire-request-start")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("transfer request-start expiry fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationExpireAssetRequestStart(error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.file-workers.hold")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("file-worker hold fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationHoldFileWorkers(error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.file-workers.release")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("file-worker release fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationReleaseFileWorkers(error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("test.completion-gate.arm") ||
        type == QStringLiteral("test.completion-gate.release")) {
        const QString target = command.value(QStringLiteral("target")).toString();
        if (!onlyFields(command, {"format", "type", "id", "target"}) ||
            (target != QStringLiteral("midi-enumeration") &&
             target != QStringLiteral("input-plugin-load"))) {
            reject(command, QStringLiteral("completion gate fields are invalid"));
            return;
        }
        QString error;
        const bool applied = type.endsWith(QStringLiteral(".arm"))
            ? window_.automationArmCompletionGate(target, error)
            : window_.automationReleaseCompletionGate(target, error);
        if (!applied) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("target"), target},
            {QStringLiteral("performance"), window_.automationPerformanceSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("test.metronome.tap-at")) {
        int elapsedMs = 0;
        const bool reset = command.value(QStringLiteral("reset")).toBool(false);
        if (!onlyFields(command, {"format", "type", "id", "elapsed_ms", "reset"}) ||
            !exactInteger(command.value(QStringLiteral("elapsed_ms")),
                0, 24 * 60 * 60 * 1000, elapsedMs) ||
            (command.contains(QStringLiteral("reset")) &&
             !command.value(QStringLiteral("reset")).isBool())) {
            reject(command, QStringLiteral("deterministic tap-tempo fields are invalid"));
            return;
        }
        if (reset) window_.tapTempoTracker_.reset();
        window_.applyTapTrackMetronomeTempoAt(elapsedMs);
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("elapsed_ms"), elapsedMs},
            {QStringLiteral("performance"), window_.automationPerformanceSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.import")) {
        int lane = 0;
        const QJsonValue path = command.value(QStringLiteral("source_path"));
        if (!onlyFields(command, {"format", "type", "id", "lane", "source_path"}) ||
            !exactInteger(command.value(QStringLiteral("lane")), -1, 255, lane) ||
            !path.isString() || path.toString().isEmpty() ||
            path.toString().toUtf8().size() > kMaximumCommandTextBytes) {
            reject(command, QStringLiteral("looper import fields are invalid"));
            return;
        }
        QString error;
        if (!window_.automationImportWav(lane, path.toString(), error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("looper.share")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("unsupported field for Share Tracks"));
            return;
        }
        QString error;
        if (!window_.automationShareTracks(error)) {
            reject(command, error);
            return;
        }
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("content"), window_.automationContentSnapshot()},
        });
        return;
    }
    if (type == QStringLiteral("shutdown")) {
        if (!onlyFields(command, {"format", "type", "id"})) {
            reject(command, QStringLiteral("unsupported field for shutdown"));
            return;
        }
        stopping_ = true;
        emitEvent(QStringLiteral("command_applied"), {
            {QStringLiteral("id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("rejected_commands"), static_cast<qint64>(
                rejectedCommands_.load(std::memory_order_relaxed))},
            {QStringLiteral("command_queue_drops"), static_cast<qint64>(
                commandQueueDrops_.load(std::memory_order_relaxed))},
            {QStringLiteral("rejected_events"), static_cast<qint64>(
                rejectedEvents_.load(std::memory_order_relaxed))},
        });
        emitEvent(QStringLiteral("shutdown"), {{QStringLiteral("return_code"), 0}});
        channel_->stop(true);
        QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::exit(0); });
        return;
    }
    reject(command, QStringLiteral("unsupported GUI automation command type"));
}

void GuiTestAgent::emitEvent(QString event, QJsonObject fields)
{
    fields.insert(QStringLiteral("event"), std::move(event));
    fields.insert(QStringLiteral("instance_id"), instanceId_);
    if (channel_ && !channel_->send(std::move(fields))) {
        rejectedEvents_.fetch_add(1, std::memory_order_relaxed);
    }
}

QJsonObject GuiTestAgent::controlInventoryPage(int cursor, bool includeState)
{
    // A cursor walks one point-in-time inventory. Rebuilding the widget tree on
    // every page lets asynchronous device/UI updates insert a control between
    // pages, shifting indices and producing duplicates or omissions.
    if (cursor == 0 || pagedControls_.empty() ||
        pagedControlsIncludeState_ != includeState) {
        pagedControls_ = controlInventoryItems(window_, includeState);
        pagedControlsIncludeState_ = includeState;
    }
    const int count = static_cast<int>(std::min<std::size_t>(
        pagedControls_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int start = std::min(cursor, count);
    const int end = std::min(count, start + kPageSize);
    QJsonArray items;
    for (int index = start; index < end; ++index) {
        QJsonObject item = pagedControls_[static_cast<std::size_t>(index)];
        item.insert(QStringLiteral("index"), index);
        items.push_back(std::move(item));
    }
    QJsonObject page{
        {QStringLiteral("protocol"), QString::fromLatin1(kProtocol)},
        {QStringLiteral("cursor"), start},
        {QStringLiteral("next_cursor"), end < count ? end : -1},
        {QStringLiteral("total_controls"), count},
        {QStringLiteral("controls"), items},
    };
    if (end >= count) pagedControls_.clear();
    return page;
}

bool GuiTestAgent::invokeControl(
    const QJsonObject& command, QString& error, bool& completionDeferred)
{
    completionDeferred = false;
    const QString id = command.value(QStringLiteral("control")).toString();
    const QString operation = command.value(QStringLiteral("operation")).toString();
    if (id.isEmpty() || id.toUtf8().size() > 256 || operation.isEmpty()) {
        error = QStringLiteral("control and operation must be bounded non-empty strings");
        return false;
    }

    QObject* match = nullptr;
    for (QObject* object : interactiveControls(window_)) {
        if (jam2::gui::guiControlId(*object) != id) continue;
        if (match != nullptr) {
            error = QStringLiteral("control id is duplicated");
            return false;
        }
        match = object;
    }
    jam2::gui::GuiVirtualControlProvider* virtualMatch = nullptr;
    QPointer<QObject> virtualOwner;
    QString virtualAvailability;
    for (const auto& entry : virtualControls(window_)) {
        if (entry.control.id != id) continue;
        if (match != nullptr || virtualMatch != nullptr) {
            error = QStringLiteral("control id is duplicated");
            return false;
        }
        virtualMatch = entry.provider;
        virtualOwner = entry.owner;
        virtualAvailability = entry.control.availability;
    }
    if (match == nullptr && virtualMatch == nullptr) {
        error = QStringLiteral("control id is not registered");
        return false;
    }
    if (virtualMatch != nullptr) {
        if (operation == QStringLiteral("click-async") &&
            virtualAvailability == QStringLiteral("modal")) {
            const QVariant value = command.value(QStringLiteral("value")).toVariant();
            QTimer::singleShot(0, virtualOwner.data(), [virtualOwner, id, value] {
                if (!virtualOwner) return;
                auto* provider = dynamic_cast<jam2::gui::GuiVirtualControlProvider*>(
                    virtualOwner.data());
                if (!provider) return;
                QString ignoredError;
                (void)provider->invokeGuiVirtualControl(
                    id, QStringLiteral("click"), value, ignoredError);
            });
            return true;
        }
        return virtualMatch->invokeGuiVirtualControl(
            id, operation, command.value(QStringLiteral("value")).toVariant(), error);
    }
    if (auto* widget = qobject_cast<QWidget*>(match); widget && !widget->isEnabled()) {
        error = QStringLiteral("control is disabled");
        return false;
    }

    if (auto* button = qobject_cast<QAbstractButton*>(match)) {
        if (operation == QStringLiteral("click")) {
            button->click();
            return true;
        }
        if (operation == QStringLiteral("click-async") &&
            (jam2::gui::guiControlAvailability(*button) == QStringLiteral("modal") ||
             jam2::gui::guiControlAvailability(*button) ==
                 QStringLiteral("hardware-profile"))) {
            const QPointer<QAbstractButton> target(button);
            const QPointer<GuiTestAgent> self(this);
            QTimer::singleShot(0, this, [self, target, command] {
                if (!self) return;
                if (!target || !target->isEnabled()) {
                    self->reject(command,
                        QStringLiteral("control became unavailable before its async click"));
                    return;
                }
                // The acknowledgement is emitted from the GUI turn that performs
                // the click. The click itself follows synchronously, before any
                // command queued in response can be drained by the modal event
                // loop. This makes command_applied an ordering signal instead of
                // an acknowledgement that a zero-delay timer merely exists.
                self->emitEvent(QStringLiteral("command_applied"), {
                    {QStringLiteral("id"), command.value(QStringLiteral("id"))},
                    {QStringLiteral("control"), command.value(QStringLiteral("control"))},
                    {QStringLiteral("operation"), command.value(QStringLiteral("operation"))},
                });
                target->click();
            });
            completionDeferred = true;
            return true;
        }
        if (operation == QStringLiteral("set-checked") && button->isCheckable() &&
            command.value(QStringLiteral("value")).isBool()) {
            button->setChecked(command.value(QStringLiteral("value")).toBool());
            return true;
        }
    } else if (auto* combo = qobject_cast<QComboBox*>(match)) {
        int value = 0;
        if ((operation == QStringLiteral("set-index") ||
             operation == QStringLiteral("activate-index")) &&
            exactInteger(command.value(QStringLiteral("value")), 0,
                std::max(0, combo->count() - 1), value) && combo->count() > 0) {
            combo->setCurrentIndex(value);
            if (operation == QStringLiteral("activate-index")) {
                QMetaObject::invokeMethod(
                    combo,
                    "activated",
                    Qt::DirectConnection,
                    Q_ARG(int, value));
            }
            return true;
        }
    } else if (auto* spin = qobject_cast<QSpinBox*>(match)) {
        int value = 0;
        if (operation == QStringLiteral("set-value") &&
            exactInteger(command.value(QStringLiteral("value")), spin->minimum(), spin->maximum(), value)) {
            spin->setValue(value);
            return true;
        }
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(match)) {
        const QJsonValue requested = command.value(QStringLiteral("value"));
        const double value = requested.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (operation == QStringLiteral("set-value") && requested.isDouble() &&
            std::isfinite(value) && value >= spin->minimum() && value <= spin->maximum()) {
            spin->setValue(value);
            return true;
        }
    } else if (auto* slider = qobject_cast<QSlider*>(match)) {
        int value = 0;
        if (operation == QStringLiteral("set-value") &&
            exactInteger(command.value(QStringLiteral("value")), slider->minimum(), slider->maximum(), value)) {
            slider->setValue(value);
            return true;
        }
    } else if (auto* edit = qobject_cast<QLineEdit*>(match)) {
        const QJsonValue value = command.value(QStringLiteral("value"));
        if (operation == QStringLiteral("set-text") && !edit->isReadOnly() && value.isString() &&
            value.toString().toUtf8().size() <= kMaximumCommandTextBytes) {
            edit->setText(value.toString());
            QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
            return true;
        }
    } else if (auto* edit = qobject_cast<QPlainTextEdit*>(match)) {
        const QJsonValue value = command.value(QStringLiteral("value"));
        if (operation == QStringLiteral("set-text") && !edit->isReadOnly() && value.isString() &&
            value.toString().toUtf8().size() <= kMaximumCommandTextBytes) {
            edit->setPlainText(value.toString());
            return true;
        }
    } else if (auto* list = qobject_cast<QListWidget*>(match)) {
        int value = 0;
        if (operation == QStringLiteral("set-current-row") && list->count() > 0 &&
            exactInteger(command.value(QStringLiteral("value")), 0,
                list->count() - 1, value)) {
            list->setCurrentRow(value);
            return true;
        }
    } else if (auto* tabs = qobject_cast<QTabWidget*>(match)) {
        int value = 0;
        if (operation == QStringLiteral("set-index") && tabs->count() > 0 &&
            exactInteger(command.value(QStringLiteral("value")), 0, tabs->count() - 1, value)) {
            tabs->setCurrentIndex(value);
            return true;
        }
    } else if (auto* action = qobject_cast<QAction*>(match)) {
        if (operation == QStringLiteral("trigger") && action->isEnabled()) {
            action->trigger();
            return true;
        }
    }
    error = QStringLiteral("operation or value is invalid for the control type");
    return false;
}

void GuiTestAgent::reject(const QJsonObject& command, const QString& reason)
{
    rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
    emitEvent(QStringLiteral("command_rejected"), {
        {QStringLiteral("id"), command.value(QStringLiteral("id"))},
        {QStringLiteral("reason"), reason},
    });
}

int jam2RunGuiTestAgent(QApplication& application, int argc, char* argv[])
{
    QString instanceId = QStringLiteral("gui-peer");
    QString storageRoot;
    bool show = false;
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--show-gui") {
            show = true;
        } else if (argument == "--instance-id" && index + 1 < argc) {
            instanceId = QString::fromUtf8(argv[++index]);
            if (instanceId.isEmpty() || instanceId.toUtf8().size() > 128) {
                std::cerr << "--instance-id must be a bounded non-empty string\n";
                return 2;
            }
        } else if (argument == "--storage-root" && index + 1 < argc) {
            storageRoot = QString::fromUtf8(argv[++index]);
            if (storageRoot.isEmpty() || storageRoot.toUtf8().size() > 4096) {
                std::cerr << "--storage-root must be a bounded non-empty path\n";
                return 2;
            }
        } else {
            std::cerr << "Usage: jam2 debug gui-agent [--show-gui] "
                         "[--instance-id <id>] [--storage-root <absolute-path>]\n";
            return 2;
        }
    }

    HiddenGuiWindowFilter hiddenWindowFilter;
    if (!show) {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        application.installEventFilter(&hiddenWindowFilter);
    }

    if (!storageRoot.isEmpty()) {
        QString error;
        if (!setAppReleaseRootForTesting(storageRoot, error)) {
            std::cerr << error.toStdString() << '\n';
            return 2;
        }
        const QString preferencesPath = QDir(storageRoot).absoluteFilePath(
            QStringLiteral("config/preferences.ini"));
        if (!UserPreferencesStore::setFilePathForTesting(preferencesPath, error)) {
            std::cerr << error.toStdString() << '\n';
            return 2;
        }
    }

    MainWindow window(nullptr, jam2::application::makeSyntheticMidiInputBackend({
        {"automation-midi-0", "Automation MIDI A"},
        {"automation-midi-1", "Automation MIDI B"},
    }), jam2::application::makeSyntheticInputPluginBackend());
    window.resize(1920, 1080);
    if (!show) window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.show();

    GuiTestAgent agent(window, instanceId, show);
    QString error;
    if (!agent.start(error)) {
        std::cerr << error.toStdString() << '\n';
        return 2;
    }
    return application.exec();
}
