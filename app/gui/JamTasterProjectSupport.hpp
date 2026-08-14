#pragma once

#include <QJsonObject>
#include <QString>

namespace jam2::gui {

bool appendJamTasterReferenceSection(
    QJsonObject& song,
    QJsonObject lane,
    QString& error);

} // namespace jam2::gui
