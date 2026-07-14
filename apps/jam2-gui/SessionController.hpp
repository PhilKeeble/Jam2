#pragma once

#include <QString>

class SessionController {
public:
    static QString defaultJam2Path();
    static QString defaultBindHost();
    static QString defaultPublicHost();

    // The unified executable owns TCP bootstrap here and delegates every
    // connected session to the same private direct-mesh UDP runner.
    static bool handlesNetworkCommand(int argc, char* argv[]);
    static int runNetworkCommand(int argc, char* argv[]);
};
