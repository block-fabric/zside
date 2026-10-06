// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/themedframe.h>

#include <qt/theme.h>

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QToolButton>
#include <QVariant>
#include <QWindow>

#include <algorithm>

namespace {
//! Width of the border, in pixels.
constexpr int EDGE{4};
//! How far from a corner the border resizes in both directions.
constexpr int CORNER{14};
const char* const EDGES_PROPERTY{"frameEdges"};
} // namespace

ThemedFrame::ThemedFrame(QMainWindow* window) : QObject(window), m_window(window)
{
    m_bar = new QWidget(window);
    m_bar->setObjectName("titleBar");
    m_bar->setAttribute(Qt::WA_StyledBackground);
    auto* layout{new QHBoxLayout(m_bar)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_icon = new QLabel(m_bar);
    m_icon->setObjectName("titleIcon");
    m_icon->setContentsMargins(8, 0, 4, 0);
    layout->addWidget(m_icon);

    m_menu_bar = new QMenuBar(m_bar);
    m_menu_bar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    layout->addWidget(m_menu_bar);

    m_title = new QLabel(m_bar);
    m_title->setObjectName("titleText");
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_title, 1);

    const auto button{[&](const QString& text, const char* name, const QString& tip) {
        auto* b{new QToolButton(m_bar)};
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        layout->addWidget(b);
        m_buttons.push_back(b);
        return b;
    }};
    connect(button(QStringLiteral("–"), "titleButton", tr("Minimize")), &QToolButton::clicked, m_window, &QWidget::showMinimized);
    m_maximize = button(QStringLiteral("□"), "titleButton", tr("Maximize"));
    connect(m_maximize, &QToolButton::clicked, this, &ThemedFrame::toggleMaximized);
    connect(button(QStringLiteral("✕"), "titleClose", tr("Close")), &QToolButton::clicked, m_window, &QWidget::close);

    window->setMenuWidget(m_bar);
    m_bar->installEventFilter(this);
    m_title->installEventFilter(this);
    m_icon->installEventFilter(this);

    // The border: a strip along each side. Near a corner a strip resizes in both directions.
    for (const Qt::Edge edge : {Qt::TopEdge, Qt::BottomEdge, Qt::LeftEdge, Qt::RightEdge}) {
        auto* strip{new QFrame(window)};
        strip->setObjectName("frameEdge");
        strip->setAttribute(Qt::WA_StyledBackground);
        strip->setProperty(EDGES_PROPERTY, static_cast<int>(edge));
        strip->setMouseTracking(true);
        strip->installEventFilter(this);
        strip->hide();
        m_edges.push_back(strip);
    }

    window->installEventFilter(this);
    connect(Theme::Changes(), &Theme::Signals::changed, this, &ThemedFrame::apply);
    apply();
}

void ThemedFrame::apply()
{
    const bool wanted{Theme::FrameWanted()};
    if (wanted != m_active) {
        m_active = wanted;
        const bool visible{m_window->isVisible()};
        const QRect geometry{m_window->geometry()};
        // Changing the flag hides the window.
        m_window->setWindowFlag(Qt::FramelessWindowHint, m_active);
        if (visible) {
            m_window->setGeometry(geometry);
            m_window->show();
        }
    }
    m_icon->setVisible(m_active);
    m_title->setVisible(m_active);
    for (QToolButton* button : m_buttons) button->setVisible(m_active);
    m_icon->setPixmap(m_window->windowIcon().pixmap(18, 18));
    showTitle();
    layoutEdges();
}

void ThemedFrame::layoutEdges()
{
    // A window that fills the screen has no border to resize it by.
    const bool border{m_active && !m_window->isMaximized() && !m_window->isFullScreen()};
    m_maximize->setText(m_window->isMaximized() ? QStringLiteral("❒") : QStringLiteral("□"));
    m_maximize->setToolTip(m_window->isMaximized() ? tr("Restore") : tr("Maximize"));
    const int margin{border ? EDGE : 0};
    if (m_window->contentsMargins().left() != margin) m_window->setContentsMargins(margin, margin, margin, margin);
    const int w{m_window->width()};
    const int h{m_window->height()};
    for (QWidget* strip : m_edges) {
        strip->setVisible(border);
        if (!border) continue;
        switch (static_cast<Qt::Edge>(strip->property(EDGES_PROPERTY).toInt())) {
        case Qt::TopEdge: strip->setGeometry(0, 0, w, EDGE); break;
        case Qt::BottomEdge: strip->setGeometry(0, h - EDGE, w, EDGE); break;
        case Qt::LeftEdge: strip->setGeometry(0, EDGE, EDGE, h - 2 * EDGE); break;
        case Qt::RightEdge: strip->setGeometry(w - EDGE, EDGE, EDGE, h - 2 * EDGE); break;
        }
        strip->raise();
    }
}

void ThemedFrame::showTitle()
{
    // What does not fit is cut off at the end, not on both sides.
    const QString title{m_title->fontMetrics().elidedText(m_window->windowTitle(), Qt::ElideRight, std::max(0, m_title->width() - 8))};
    if (m_title->text() != title) m_title->setText(title);
}

void ThemedFrame::toggleMaximized()
{
    if (m_window->isMaximized()) {
        m_window->showNormal();
    } else {
        m_window->showMaximized();
    }
}

bool ThemedFrame::eventFilter(QObject* object, QEvent* event)
{
    if (object == m_window) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::WindowStateChange:
        case QEvent::Show:
            layoutEdges();
            break;
        case QEvent::WindowTitleChange:
            showTitle();
            break;
        case QEvent::WindowIconChange:
            m_icon->setPixmap(m_window->windowIcon().pixmap(18, 18));
            break;
        default:
            break;
        }
        return false;
    }
    if (object == m_title && event->type() == QEvent::Resize) showTitle();
    if (!m_active) return false;
    if (m_edges.contains(qobject_cast<QWidget*>(object))) {
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress) return false;
        auto* strip{static_cast<QWidget*>(object)};
        auto* mouse{static_cast<QMouseEvent*>(event)};
        Qt::Edges edges{strip->property(EDGES_PROPERTY).toInt()};
        const QPoint at{strip->mapTo(m_window, mouse->position().toPoint())};
        if (at.y() < CORNER) edges |= Qt::TopEdge;
        if (at.y() >= m_window->height() - CORNER) edges |= Qt::BottomEdge;
        if (at.x() < CORNER) edges |= Qt::LeftEdge;
        if (at.x() >= m_window->width() - CORNER) edges |= Qt::RightEdge;
        if (event->type() == QEvent::MouseMove) {
            const bool vertical{edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge)};
            const bool horizontal{edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)};
            if (vertical && horizontal) {
                strip->setCursor(edges.testFlag(Qt::TopEdge) == edges.testFlag(Qt::LeftEdge) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
            } else {
                strip->setCursor(vertical ? Qt::SizeVerCursor : Qt::SizeHorCursor);
            }
            return false;
        }
        if (mouse->button() == Qt::LeftButton && m_window->windowHandle()) {
            m_window->windowHandle()->startSystemResize(edges);
            return true;
        }
        return false;
    }
    // The bar, where it is not a menu or a button, moves the window.
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && m_window->windowHandle()) {
        m_window->windowHandle()->startSystemMove();
        return true;
    }
    if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        toggleMaximized();
        return true;
    }
    return false;
}
