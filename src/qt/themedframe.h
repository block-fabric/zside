// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_THEMEDFRAME_H
#define BITCOIN_QT_THEMEDFRAME_H

#include <QList>
#include <QObject>

class QLabel;
class QMainWindow;
class QMenuBar;
class QToolButton;
class QWidget;

/**
 * Gives a main window a frame in the colours of the theme: a title bar that
 * holds the menus, the title and the buttons of the window, and a border to
 * resize it by, in place of the frame the desktop draws. With the look of the
 * desktop, or if the user would rather not, the window keeps the frame of the
 * desktop and only the menus are left of the bar.
 */
class ThemedFrame : public QObject
{
    Q_OBJECT

public:
    explicit ThemedFrame(QMainWindow* window);

    /** The menu bar of the window, which lives in the title bar. */
    QMenuBar* menuBar() const { return m_menu_bar; }
    /** Whether the frame is drawn by this class at the moment. */
    bool active() const { return m_active; }
    /** Take the frame the settings ask for. */
    void apply();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void layoutEdges();
    void showTitle();
    void toggleMaximized();

    QMainWindow* const m_window;
    QWidget* m_bar{nullptr};
    QMenuBar* m_menu_bar{nullptr};
    QLabel* m_icon{nullptr};
    QLabel* m_title{nullptr};
    QList<QToolButton*> m_buttons;
    QToolButton* m_maximize{nullptr};
    //! The border: a strip along each side, which resizes the window.
    QList<QWidget*> m_edges;
    bool m_active{false};
};

#endif // BITCOIN_QT_THEMEDFRAME_H
