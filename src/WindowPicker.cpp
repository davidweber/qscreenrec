#include "WindowPicker.h"

#include <QGridLayout>
#include <QLabel>
#include <QMediaCaptureSession>
#include <QPainter>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWindowCapture>

namespace {
const QSize kThumb(232, 140);
constexpr int kColumns = 3;

QPixmap placeholder(const QString &text)
{
    QPixmap pm(kThumb);
    pm.fill(QColor(30, 34, 44));
    QPainter p(&pm);
    p.setPen(QColor(140, 150, 170));
    p.drawText(pm.rect(), Qt::AlignCenter, text);
    return pm;
}

QPixmap thumbnail(const QImage &image)
{
    QPixmap pm(kThumb);
    pm.fill(QColor(18, 20, 26));
    const QImage scaled = image.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&pm);
    p.drawImage((kThumb.width() - scaled.width()) / 2, (kThumb.height() - scaled.height()) / 2, scaled);
    return pm;
}
}

WindowPicker::WindowPicker(const QList<QCapturableWindow> &windows, QWidget *parent)
    : QDialog(parent)
    , m_windows(windows)
{
    setWindowTitle(tr("Choose a window to record"));

    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    grid->setSpacing(10);
    for (int i = 0; i < m_windows.size(); ++i) {
        const QString title = m_windows.at(i).description().trimmed();
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("windowTile"));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(kThumb);
        button->setIcon(placeholder(tr("Loading…")));
        button->setFixedWidth(kThumb.width() + 20);
        button->setText(button->fontMetrics().elidedText(title.isEmpty() ? tr("(untitled window)") : title,
                                                         Qt::ElideMiddle, kThumb.width()));
        button->setToolTip(title);
        connect(button, &QToolButton::clicked, this, [this, i] {
            m_selected = m_windows.at(i);
            accept();
        });
        grid->addWidget(button, i / kColumns, i % kColumns);
        m_buttons.append(button);
    }
    if (m_windows.isEmpty())
        grid->addWidget(new QLabel(tr("No windows found.")), 0, 0);

    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Click the window you want to record.")));
    layout->addWidget(scroll, 1);

    const int rows = qMin(3, (int(m_windows.size()) + kColumns - 1) / kColumns);
    resize(kColumns * (kThumb.width() + 32) + 40, qMax(1, rows) * (kThumb.height() + 60) + 80);

    m_session = new QMediaCaptureSession(this);
    m_sink = new QVideoSink(this);
    m_session->setVideoSink(m_sink);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(1500);
    connect(m_timeout, &QTimer::timeout, this, [this] { finishCurrent({}); });
    connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (m_capture && frame.isValid())
            finishCurrent(frame.toImage());
    });
    QTimer::singleShot(0, this, &WindowPicker::grabNext);
}

WindowPicker::~WindowPicker()
{
    if (m_capture)
        m_capture->stop();
}

void WindowPicker::grabNext()
{
    ++m_current;
    if (m_current >= m_windows.size())
        return;
    m_capture = new QWindowCapture(this);
    m_capture->setWindow(m_windows.at(m_current));
    connect(m_capture, &QWindowCapture::errorOccurred, this, [this] {
        QTimer::singleShot(0, this, [this] { finishCurrent({}); });
    });
    m_session->setWindowCapture(m_capture);
    m_capture->start();
    m_timeout->start();
}

void WindowPicker::finishCurrent(const QImage &image)
{
    if (!m_capture)
        return;
    m_timeout->stop();
    m_capture->stop();
    m_session->setWindowCapture(nullptr);
    m_capture->deleteLater();
    m_capture = nullptr;

    if (m_current >= 0 && m_current < m_buttons.size())
        m_buttons.at(m_current)->setIcon(image.isNull() ? placeholder(tr("No preview")) : thumbnail(image));
    QTimer::singleShot(0, this, &WindowPicker::grabNext);
}
