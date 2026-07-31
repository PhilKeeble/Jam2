#include "BeatGridModel.hpp"

// The experiment needs only the stable drum-lane vocabulary from
// BeatGridModel. Keeping this tiny definition local avoids linking Jam2's
// persistence/content-limit implementation into the standalone renderer.
QStringList BeatGridModel::beatLaneNames()
{
    return {
        QStringLiteral("Kick"),
        QStringLiteral("Snare"),
        QStringLiteral("Closed HH"),
        QStringLiteral("Open HH"),
        QStringLiteral("Ride"),
        QStringLiteral("Crash"),
        QStringLiteral("High Tom"),
        QStringLiteral("Mid Tom"),
        QStringLiteral("Floor Tom"),
        QStringLiteral("Cross-stick / Rim"),
        QStringLiteral("Shaker"),
        QStringLiteral("Hand Percussion"),
    };
}
