#pragma once

#include "SharedTrackModel.hpp"

#include <QJsonObject>

class SharedTrackController {
public:
    SharedTrackModel& model();
    const SharedTrackModel& model() const;
    QJsonObject processingMessage() const;
    void applyProcessingMessage(const QJsonObject& message);

private:
    SharedTrackModel model_;
};
