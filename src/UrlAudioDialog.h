#pragma once

#include <QDialog>
#include <QProcess>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

// "Audio from URL": saves the audio track of an online video (YouTube and the many other
// sites yt-dlp supports) into the recordings folder.
//
// The download itself is done by the external tools yt-dlp and, for converting to a chosen
// format, ffmpeg. They are not bundled: the dialog finds them on the system and explains how
// to install them when they are missing.
class UrlAudioDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UrlAudioDialog(const QString &outputFolder, QWidget *parent = nullptr);
    ~UrlAudioDialog() override;

    QString savedFile() const { return m_savedFile; }

    static QString findTool(const QString &name);   // PATH plus the usual per-user install dirs

protected:
    void reject() override;

private:
    void start();
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void setBusy(bool busy);

    QString m_folder;
    QString m_ytDlp;
    QString m_ffmpeg;
    QString m_savedFile;
    QString m_lastError;
    QByteArray m_buffer;
    QProcess *m_process = nullptr;

    QLineEdit *m_urlEdit = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};
