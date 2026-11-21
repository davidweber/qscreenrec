#pragma once

#include "Recorder.h"

#include <QCapturableWindow>
#include <QList>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QSystemTrayIcon;
class PlayerWindow;
class RegionFrame;

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void loadSettings();
    void saveSettings() const;

    void refreshScreens();
    void refreshWindows();
    void pickWindow();
    void populateFormats();
    void showPlayer(const QString &filePath = {});
    void audioFromUrl();
    void refreshAudioSources();
    bool ensureMicrophonePermission();
    void selectRegion();
    void browseOutputFolder();
    QString nextOutputPath() const;

    void toggleRecording();
    void beginCountdown();
    void startRecording();
    void onFinished(const QString &path);
    void onFailed(const QString &message);
    void updateUiState();
    void restoreWindow();

    Recorder::Mode mode() const;
    QScreen *selectedScreen() const;

    Recorder m_recorder;
    QTimer m_countdownTimer;
    int m_countdown = 0;
    qint64 m_lastDurationMs = 0;
    QRect m_region;                         // relative to the selected screen
    QList<QCapturableWindow> m_windows;
    QPointer<RegionFrame> m_regionFrame;
    QSystemTrayIcon *m_tray = nullptr;

    QRadioButton *m_fullScreenRadio = nullptr;
    QRadioButton *m_regionRadio = nullptr;
    QRadioButton *m_windowRadio = nullptr;
    QRadioButton *m_audioOnlyRadio = nullptr;
    QPushButton *m_recordingsButton = nullptr;
    QPushButton *m_urlButton = nullptr;
    QPointer<PlayerWindow> m_player;
    bool m_formatsForAudio = false;
    QComboBox *m_screenCombo = nullptr;
    QPushButton *m_selectRegionButton = nullptr;
    QLabel *m_regionLabel = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QPushButton *m_refreshWindowsButton = nullptr;
    QPushButton *m_pickWindowButton = nullptr;
    bool m_viaPortal = false;
    QComboBox *m_audioCombo = nullptr;
    QLineEdit *m_folderEdit = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QSpinBox *m_fpsSpin = nullptr;
    QSpinBox *m_delaySpin = nullptr;
    QCheckBox *m_minimizeCheck = nullptr;
    QPushButton *m_recordButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QLabel *m_statusLabel = nullptr;
};
