#pragma once

#include <QPixmap>
#include <QWidget>

class QScreen;

// Full-screen translucent overlay on one screen; the user drags out a rectangle.
// Emits selected() with a rectangle in logical pixels relative to that screen's
// top-left corner, or cancelled() on Esc / right-click / too-small selection.
class RegionSelector : public QWidget
{
    Q_OBJECT
public:
    explicit RegionSelector(QScreen *screen, const QRect &initial = {}, QWidget *parent = nullptr);

    void begin();

signals:
    void selected(const QRect &region);
    void cancelled();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QScreen *m_screen;
    QPoint m_origin;
    QRect m_selection;
    bool m_dragging = false;
    QPixmap m_snapshot;     // X11 only: frozen screenshot used instead of real translucency
};

// Thin click-through frame drawn just outside the recorded area so the user can
// see what is being captured without the frame itself ending up in the video.
class RegionFrame : public QWidget
{
    Q_OBJECT
public:
    RegionFrame(QScreen *screen, const QRect &region, QWidget *parent = nullptr);

    static bool isSupported();   // needs absolute window positioning (not available on Wayland)

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
};
