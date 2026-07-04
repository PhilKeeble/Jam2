#pragma once

#include <QString>

struct LeadCueModel {
    QString currentLead = QStringLiteral("Teacher");
    QString pendingLead;
    int pendingBars = 0;
};
