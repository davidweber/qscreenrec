#pragma once

#include <QWidget>

class QAudioOutput;
class QLabel;
class QListWidget;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QStackedWidget;
class QVideoWidget;

// Built-in player for recordings: a list of the files in the recordings folder on the left,
// video (or an audio placeholder) with transport controls on the right.
class PlayerWindow : public QWidget
{
    Q_OBJECT
public:
    explicit PlayerWindow(QWidget *parent = nullptr);

    void setFolder(const QString &folder);      // lists the recordings found there
    void play(const QString &filePath);
    void stop();

    static QStringList mediaSuffixes();

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void refreshList();
    void openFile();
    void togglePlay();
    void updatePosition(qint64 ms);
    void updateControls();

    QString m_folder;
    QString m_current;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QListWidget *m_list = nullptr;
    QStackedWidget *m_stack = nullptr;
    QVideoWidget *m_video = nullptr;
    QLabel *m_placeholder = nullptr;
    QPushButton *m_playButton = nullptr;
    QSlider *m_seek = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_title = nullptr;
    bool m_seeking = false;
};
