#pragma once

class MainWindow;
class QWidget;
class QHBoxLayout;

class MainWindowPages final {
public:
    static void build(MainWindow& window);

private:
    static QWidget* buildSessionPage(MainWindow& window);
    static QWidget* buildSongPage(MainWindow& window);
    static QWidget* buildBeatPage(MainWindow& window);
    static QWidget* buildTrackPage(MainWindow& window);
    static QWidget* buildMetronomePage(MainWindow& window);
    static void buildAudioControls(MainWindow& window);
    static void addBankControls(
        MainWindow& window,
        QWidget* owner,
        QHBoxLayout* layout,
        bool looper);
};
