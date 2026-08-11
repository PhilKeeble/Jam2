#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class JamTasterService;

class JamTasterDialog final : public QDialog {
public:
    struct Callbacks {
        std::function<void(
            const QJsonObject&, const QJsonObject&, const QJsonObject&, const QString&)>
            applyQuick;
        std::function<void(const QString&, const QJsonObject&)> applyConverted;
        std::function<void(const QString&, const QString&, const QString&)> createSong;
    };

    JamTasterDialog(
        JamTasterService& service,
        QString projectRoot,
        QString sourcePath,
        QString sourceHash,
        QString displayName,
        Callbacks callbacks,
        QWidget* parent = nullptr);
    ~JamTasterDialog() override;

    void setSourceContext(
        QString projectRoot,
        QString sourcePath,
        QString sourceHash,
        QString displayName);

private:
    void chooseSource();
    void startAction(const QString& action);
    void startPendingAction();
    void syncTaskState();
    void confirmCancelTask();
    void refreshSavedResults();
    void acceptJobResult(const QJsonObject& result);
    void loadFullAnalysis(const QString& convertedSong);
    void refreshResultsUi();
    QString analysisSourceRoot() const;
    static QJsonObject readJson(const QString& path);

    JamTasterService& service_;
    QString projectRoot_;
    QString sourcePath_;
    QString sourceHash_;
    QString displayName_;
    QString pendingAction_;
    bool createSongAfterAnalysis_ = false;
    QString convertedSong_;
    QJsonObject tempoResult_;
    QJsonObject stemsResult_;
    QJsonObject analysisResult_;
    Callbacks callbacks_;
    std::uint64_t serviceObserverId_ = 0;

    QLineEdit* sourceEdit_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* tempoValue_ = nullptr;
    QLabel* stemsValue_ = nullptr;
    QLabel* chordsValue_ = nullptr;
    QLabel* drumsValue_ = nullptr;
    QLabel* bassValue_ = nullptr;
    QLabel* sectionsValue_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* bpmButton_ = nullptr;
    QPushButton* stemsButton_ = nullptr;
    QPushButton* applySelectedButton_ = nullptr;
    QPushButton* createSongButton_ = nullptr;
    QPushButton* cancelTaskButton_ = nullptr;
    QCheckBox* applyTempoCheck_ = nullptr;
    QCheckBox* applyStemsCheck_ = nullptr;
    QCheckBox* applyChordsCheck_ = nullptr;
    QCheckBox* applyDrumsCheck_ = nullptr;
    QCheckBox* applyBassCheck_ = nullptr;
    QCheckBox* applySectionsCheck_ = nullptr;
};
