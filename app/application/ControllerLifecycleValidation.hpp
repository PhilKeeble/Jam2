#pragma once

#include <QJsonObject>

// Deterministic cold-path validation of the real shared TCP/session controller.
// This is reached only through the explicit debug scenario operation.
QJsonObject jam2RunControllerLifecycleValidation(
    int heartbeatIntervalMs,
    int heartbeatMissLimit);
