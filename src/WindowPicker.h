#pragma once

#include <QCapturableWindow>
#include <QDialog>
#include <QList>

class QGridLayout;
class QMediaCaptureSession;
class QToolButton;
class QVideoSink;
class QWindowCapture;

// Visual window chooser: a grid of live thumbnails, one per capturable window; click one.
// Pure Qt Multimedia, so it works wherever QWindowCapture can list windows
// (Windows, macOS, Linux/X11).
class WindowPicker : public QDialog
{
    Q_OBJECT
public:
    explicit WindowPicker(const QList<QCapturableWindow> &windows, QWidget *parent = nullptr);
    ~WindowPicker() override;

    QCapturableWindow selectedWindow() const { return m_selected; }

private:
    void grabNext();
    void finishCurrent(const QImage &image);

    QList<QCapturableWindow> m_windows;
    QList<QToolButton *> m_buttons;
    QCapturableWindow m_selected;

    // One thumbnail is grabbed at a time.
    int m_current = -1;
    QMediaCaptureSession *m_session = nullptr;
    QWindowCapture *m_capture = nullptr;
    QVideoSink *m_sink = nullptr;
    class QTimer *m_timeout = nullptr;
};
