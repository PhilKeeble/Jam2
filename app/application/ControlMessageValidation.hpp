#pragma once

#include <QJsonObject>
#include <QString>

namespace jam2::application {

enum class ControlMessageSource {
    LocalCreator,
    LocalJoiner,
    AuthenticatedPeer,
    Coordinator,
    Internal,
};

enum class ControlMessageRejection {
    None,
    Validation,
    Authorization,
};

struct ControlMessageDecision {
    bool accepted = false;
    ControlMessageRejection rejection = ControlMessageRejection::Validation;
    QString reason;
};

bool isGridControlMessageType(const QString& type) noexcept;
bool isArrangementControlMessageType(const QString& type) noexcept;
bool isTrackSyncControlMessageType(const QString& type) noexcept;
bool validateControlMessage(const QJsonObject& message, QString& reason);
ControlMessageDecision evaluateControlMessage(
    const QJsonObject& message,
    ControlMessageSource source);

}
