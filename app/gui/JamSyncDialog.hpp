#pragma once

#include "JamSyncPolicy.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QWidget;

class JamSyncDialog final : public QDialog {
public:
    JamSyncDialog(
        JamSyncPolicy policy,
        bool policyLocked,
        bool leaderAudio,
        QWidget* parent = nullptr);

    JamSyncPolicy policy() const noexcept;

private:
    void updateDependencies();

    JamSyncPolicy initialPolicy_;
    bool policyLocked_ = false;
    bool leaderAudio_ = false;
    QCheckBox* trackLanes_ = nullptr;
    QCheckBox* automaticWavs_ = nullptr;
    QComboBox* generatedIdeas_ = nullptr;
    QCheckBox* globalPlayback_ = nullptr;
    QCheckBox* metronomeState_ = nullptr;
    QCheckBox* recordings_ = nullptr;
    QLabel* dependencyNote_ = nullptr;
};
