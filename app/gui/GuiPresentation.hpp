#pragma once

#include <QString>

class QCheckBox;
class QApplication;
class QLabel;
class QSlider;
class QSpinBox;
class QWidget;

struct FocusPreset {
    double frequencyHz = 120.0;
    double gainDb = 12.0;
    double q = 6.0;
};

void installJam2Style();
void installCompactDialogPolicy(QApplication& app);
void showJamReadyInviteDialog(QWidget* parent, const QString& inviteUrl);
QString appReleaseFilePath(const QString& folder, const QString& fileName);
QString appReleaseFolderPath(const QString& folder);
FocusPreset focusPresetForKey(const QString& key);
void applyJamSliderStyle(QSlider* slider);
bool isCustomFocusPreset(const QString& key);
void applyMutedEditorStyle(QWidget* widget);
void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin);
void updateCaptureDurationControl(
    QCheckBox* manualStopCheck,
    QSpinBox* durationSpin,
    QLabel* durationLabel);
QString dbText(double db);
double gainFromDb(double db);
