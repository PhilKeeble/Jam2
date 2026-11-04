#pragma once

#include <QString>

namespace jam2::update {
bool macInstallerRunning();
void restartMacApplication(const QString& bundlePath);
}
