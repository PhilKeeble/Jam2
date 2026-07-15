#pragma once

#include <QJsonObject>
#include <QString>

// Validate the deliberately small local debug-action contract before an
// action reaches SessionController or the engine command queue.
bool jam2ValidateDebugAction(const QJsonObject& action, QString& error);
