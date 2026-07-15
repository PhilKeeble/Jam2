#pragma once

#include <QJsonObject>
#include <QString>

namespace jam2::application {

bool isTrackSyncControlMessageType(const QString& type) noexcept;
bool validateControlMessage(const QJsonObject& message, QString& reason);

}
