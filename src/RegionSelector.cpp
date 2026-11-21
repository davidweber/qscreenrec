#include "RegionSelector.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>

namespace {
constexpr int kMinSize = 16;
constexpr int kFrameWidth = 3;

bool isWayland()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

bool isWaylandSession()
{
    return isWayland() || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
           || qEnvironmentVariable("XDG_SESSION_TYPE").compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0;
}

// True if the screenshot is missing or a single flat colour (what a failed grab looks like).
bool isBlank(const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return true;
    const QImage image = pixmap.toImage();
    const QRgb first = image.pixel(0, 0);
    for (int y = 0; y < image.height(); y += qMax(1, image.height() / 40)) {
        for (int x = 0; x < image.width(); x += qMax(1, image.width() / 40)) {
            if (image.pixel(x, y) != first)
                return false;
        }
    }
    return true;
}
}

RegionSelector::RegionSelector(QScreen *screen, const QRect &initial, QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , m_screen(screen)
    , m_selection(initial)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void RegionSelector::begin()
{
    // The overlay is normally a translucent window. Plain X11 without a compositing manager
    // has no translucency (the overlay would be solid black), so there a frozen screenshot is
    // shown instead. Under a Wayland session (including X11 apps running through XWayland)
    // the compositor always provides translucency, and X11 screenshots come back black, so
    // the screenshot is only used when it actually contains something.
    if (QGuiApplication::platformName() == QLatin1String("xcb") && !isWaylandSession()) {
        const QPixmap shot = m_screen->grabWindow(0);
        if (!isBlank(shot)) {
            m_snapshot = shot;
            setAttribute(Qt::WA_TranslucentBackground, false);
        }
    }
    if (isWayland()) {
        // Wayland clients cannot position themselves; ask the compositor for full screen instead.
        winId();
        if (windowHandle())
            windowHandle()->setScreen(m_screen);
        showFullScreen();
    } else {
        setGeometry(m_screen->geometry());
        show();
    }
    raise();
    activateWindow();
    setFocus();
}

void RegionSelector::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (!m_snapshot.isNull())
        p.drawPixmap(rect(), m_snapshot);
    p.fillRect(rect(), QColor(0, 0, 0, 110));

    const QString hint = tr("Drag to select the area to record  —  Esc to cancel");
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.4);
    p.setFont(f);

    if (m_selection.isEmpty()) {
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter, hint);
        return;
    }

    if (!m_snapshot.isNull()) {
        // Show the selection undimmed.
        const qreal dpr = m_snapshot.devicePixelRatio();
        p.drawPixmap(m_selection, m_snapshot,
                     QRectF(m_selection.topLeft() * dpr, QSizeF(m_selection.size()) * dpr));
    } else {
        // Punch a clear hole where the selection is.
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(m_selection, Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // A nearly invisible fill keeps the hole from becoming click-through on some platforms.
        p.fillRect(m_selection, QColor(255, 255, 255, 1));
    }

    p.setPen(QPen(QColor(255, 60, 60), 2));
    p.drawRect(m_selection.adjusted(-1, -1, 0, 0));

    const QString label = QStringLiteral("%1 × %2").arg(m_selection.width()).arg(m_selection.height());
    const QRect textRect = p.fontMetrics().boundingRect(label).adjusted(-8, -4, 8, 4);
    QPoint pos(m_selection.left(), m_selection.top() - textRect.height() - 6);
    if (pos.y() < 0)
        pos.setY(m_selection.top() + 6);
    QRect box(pos, textRect.size());
    p.fillRect(box, QColor(0, 0, 0, 180));
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, label);
}

void RegionSelector::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        emit cancelled();
        close();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_origin = event->position().toPoint();
    m_selection = QRect(m_origin, QSize());
    update();
}

void RegionSelector::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    m_selection = QRect(m_origin, event->position().toPoint()).normalized() & rect();
    update();
}

void RegionSelector::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    const QRect result = m_selection;
    if (result.width() >= kMinSize && result.height() >= kMinSize)
        emit selected(result);
    else
        emit cancelled();
    close();
}

void RegionSelector::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emit cancelled();
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------------------------------------------

RegionFrame::RegionFrame(QScreen *screen, const QRect &region, QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                          | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    const QRect global = region.translated(screen->geometry().topLeft());
    setGeometry(global.adjusted(-kFrameWidth, -kFrameWidth, kFrameWidth, kFrameWidth));
}

void RegionFrame::resizeEvent(QResizeEvent *)
{
    // Shape the window to just the border, so it never covers the recorded area even
    // where translucent windows are unavailable.
    setMask(QRegion(rect()) - QRegion(rect().adjusted(kFrameWidth, kFrameWidth, -kFrameWidth, -kFrameWidth)));
}

bool RegionFrame::isSupported()
{
    return !isWayland();
}

void RegionFrame::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QPen pen(QColor(255, 60, 60), kFrameWidth);
    pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(pen);
    const qreal h = kFrameWidth / 2.0;
    p.drawRect(QRectF(rect()).adjusted(h, h, -h, -h));
}
