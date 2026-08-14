#pragma once

#include "UserPreferences.hpp"

#include <QDialog>
#include <QVariant>
#include <QVector>

#include <cstddef>
#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

class LaneRecordingDialog final : public QDialog {
public:
    struct Choice {
        QString label;
        QVariant value;
    };

    struct Input {
        QString laneName;
        QString preferredMode;
        bool engineRunning = false;
        bool leaderAudio = false;
        QVector<Choice> inputSources;
        std::optional<std::size_t> selectedInputSource;
        QVector<Choice> loopbackSources;
        QString inputOutputPath;
        QString jamMixOutputPath;
        QString loopbackOutputPath;
        InputRecordingPreference input;
        LoopbackRecordingPreference loopback;
        JamMixTrackRecordingPreference jamMix;
        QString latencySummary;
    };

    struct Result {
        QString mode;
        std::optional<std::size_t> inputSource;
        QString outputPath;
        InputRecordingPreference input;
        LoopbackRecordingPreference loopback;
        bool includeBackingTrack = false;
        bool includeMetronome = false;
    };

    using BrowseOutput = std::function<QString(const QString& current)>;
    using RefreshLoopbackSources = std::function<QVector<Choice>()>;

    LaneRecordingDialog(
        Input input,
        BrowseOutput browseOutput,
        RefreshLoopbackSources refreshLoopbackSources,
        QWidget* parent = nullptr);

    Result result();

private:
    void storeDraft();
    void loadDraft();
    void refreshMode();
    void populateLoopbackSources(const QVector<Choice>& choices);
    void selectLoopbackSource(const QString& id, const QString& name);
    void setFormRowVisible(QFormLayout* form, QWidget* field, bool visible);

    Input input_;
    BrowseOutput browseOutput_;
    RefreshLoopbackSources refreshLoopbackSources_;
    QString activeMode_;
    QString inputOutputPath_;
    QString jamMixOutputPath_;
    QString loopbackOutputPath_;
    InputRecordingPreference inputDraft_;
    LoopbackRecordingPreference loopbackDraft_;

    QFormLayout* form_ = nullptr;
    QFormLayout* advancedForm_ = nullptr;
    QComboBox* mode_ = nullptr;
    QComboBox* inputSource_ = nullptr;
    QLineEdit* output_ = nullptr;
    QComboBox* loopbackSource_ = nullptr;
    QCheckBox* includeBacking_ = nullptr;
    QCheckBox* includeMetronome_ = nullptr;
    QLabel* engineStatus_ = nullptr;
    QLabel* inputLabel_ = nullptr;
    QLabel* leaderAudioWarning_ = nullptr;
    QWidget* outputRow_ = nullptr;
    QWidget* sourceRow_ = nullptr;
    QWidget* latencyRow_ = nullptr;
    QPushButton* advancedToggle_ = nullptr;
    QWidget* advancedContent_ = nullptr;
    QSpinBox* latencyAdjustment_ = nullptr;
    QDoubleSpinBox* silenceThreshold_ = nullptr;
    QSpinBox* tailSilence_ = nullptr;
    QCheckBox* trimLeading_ = nullptr;
    QCheckBox* trimTrailing_ = nullptr;
    QPushButton* arm_ = nullptr;
};
