#pragma once

class MainWindow;
class QWidget;

class MainWindowPages final {
public:
    static void build(MainWindow& window);

private:
    static QWidget* buildSessionPage(MainWindow& window);
    static QWidget* buildSongPage(MainWindow& window);
    static QWidget* buildTrackPage(MainWindow& window);
    static QWidget* buildMetronomePage(MainWindow& window);
    static QWidget* buildMixPage(MainWindow& window);
};
