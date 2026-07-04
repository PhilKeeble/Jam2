#pragma once

#include "SharedTrackModel.hpp"

#include <QJsonObject>

class SharedTrackController {
public:
    SharedTrackModel& model();
    QJsonObject processingMessage() const;
    void applyProcessingMessage(const QJsonObject& message);

private:
    SharedTrackModel model_;
};
