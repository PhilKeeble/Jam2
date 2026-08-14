#include "AutomationActionRegistry.hpp"
#include "DebugActionValidation.hpp"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSet>

#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void expectValid(QJsonObject action, const char* message)
{
    QString error;
    expect(jam2ValidateDebugAction(action, error), message);
    if (!error.isEmpty()) {
        std::cerr << "  validation error: " << error.toStdString() << '\n';
    }
}

void expectInvalid(QJsonObject action, const char* message)
{
    QString error;
    expect(!jam2ValidateDebugAction(action, error), message);
    expect(!error.isEmpty(), "invalid actions must explain their rejection");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    const auto descriptors = jam2AutomationActionDescriptors();
    QSet<QString> names;
    QSet<int> enumValues;
    for (const auto& descriptor : descriptors) {
        const QString name = QString::fromLatin1(descriptor.name);
        expect(!name.isEmpty(), "registry names must not be empty");
        expect(!names.contains(name), "registry names must be unique");
        expect(!enumValues.contains(static_cast<int>(descriptor.type)),
            "registry enum values must be unique");
        names.insert(name);
        enumValues.insert(static_cast<int>(descriptor.type));
        expect(jam2AutomationActionType(name) == descriptor.type,
            "registry name parsing must round-trip");
        expect(jam2AutomationActionName(descriptor.type) == name,
            "registry enum formatting must round-trip");
    }
    expect(static_cast<std::size_t>(jam2AutomationActionNamesJson().size()) == descriptors.size(),
        "JSON action inventory must contain every descriptor");
    expect(!jam2AutomationActionType(QStringLiteral("unknown.action")).has_value(),
        "unknown actions must not parse");

    expectValid({{QStringLiteral("type"), QStringLiteral("metronome.enabled")},
                    {QStringLiteral("enabled"), true}},
        "metronome.enabled valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("metronome.bpm")},
                    {QStringLiteral("value"), 400}},
        "metronome.bpm upper bound");
    expectValid({{QStringLiteral("type"), QStringLiteral("metronome.mode")},
                    {QStringLiteral("mode"), QStringLiteral("listener-compensated")}},
        "metronome.mode valid choice");
    expectValid({{QStringLiteral("type"), QStringLiteral("metronome.level")},
                    {QStringLiteral("value"), 4.0}},
        "metronome.level upper bound");
    expectValid({{QStringLiteral("type"), QStringLiteral("remote.level")},
                    {QStringLiteral("value"), 0.0}},
        "remote.level lower bound");
    expectValid({{QStringLiteral("type"), QStringLiteral("track.sync")},
                    {QStringLiteral("enabled"), false}},
        "track.sync valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("track.load")},
                    {QStringLiteral("path"), QStringLiteral("fixture.wav")}},
        "track.load valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("track.play")}},
        "track.play valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("track.stop")}},
        "track.stop valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("track.restart")},
                    {QStringLiteral("count_in_bars"), 8}},
        "track.restart valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("track.record-start")}},
        "track.record-start optional count-in");
    expectValid({{QStringLiteral("type"), QStringLiteral("recording.start")},
                    {QStringLiteral("path"), QStringLiteral("recording")}},
        "recording.start valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("recording.stop")}},
        "recording.stop valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("snapshot")}},
        "snapshot valid form");
    expectValid({{QStringLiteral("type"), QStringLiteral("shutdown")}},
        "shutdown valid form");

    expectInvalid({{QStringLiteral("type"), QStringLiteral("unknown.action")}},
        "unknown action rejection");
    expectInvalid({{QStringLiteral("type"), QStringLiteral("metronome.enabled")},
                      {QStringLiteral("enabled"), 1}},
        "boolean payload type rejection");
    expectInvalid({{QStringLiteral("type"), QStringLiteral("metronome.bpm")},
                      {QStringLiteral("value"), 401}},
        "integer payload bound rejection");
    expectInvalid({{QStringLiteral("type"), QStringLiteral("track.play")},
                      {QStringLiteral("value"), 1}},
        "action-specific extra field rejection");
    expectInvalid({{QStringLiteral("type"), QStringLiteral("shutdown")},
                      {QStringLiteral("apply_frame"), 10},
                      {QStringLiteral("delay_frames"), 10}},
        "mutually exclusive scheduling rejection");

    if (failures != 0) {
        std::cerr << failures << " automation contract checks failed\n";
        return 1;
    }
    std::cout << "automation contract checks passed\n";
    return 0;
}
