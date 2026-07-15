#pragma once

#include <QString>

#if defined(__APPLE__)
bool jam2EnsureMicrophonePermission(QString* errorMessage);
#else
inline bool jam2EnsureMicrophonePermission(QString*)
{
    return true;
}
#endif
