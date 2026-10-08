// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_ITEMVIEWS_H
#define BITCOIN_QT_ITEMVIEWS_H

#include <QAbstractItemView>
#include <QInputMethodEvent>
#include <QListWidget>
#include <QTableWidget>
#include <QTreeWidget>

#include <utility>

/**
 * Read-only item views for the pages written for Chains and its sidechains.
 *
 * Under Wayland (WSLg), an input method event that reaches an item view starts an edit, the edit
 * moves the focus, and moving the focus sends the view another such event: the program runs out
 * of stack (the crash seen on a double-click). These views never edit their items, take no input
 * method and leave input method events to their editors. Create every list, table or tree of the
 * custom pages with them (or pass a view made otherwise to MakeReadOnlyView).
 */
namespace ItemViews {

/** Makes a view read-only and keeps input methods away from it. Checkable items stay checkable. */
inline void MakeReadOnlyView(QAbstractItemView* view)
{
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAttribute(Qt::WA_InputMethodEnabled, false);
}

template <typename Base>
class ReadOnly : public Base
{
public:
    template <typename... Args>
    explicit ReadOnly(Args&&... args) : Base(std::forward<Args>(args)...)
    {
        MakeReadOnlyView(this);
    }

protected:
    void inputMethodEvent(QInputMethodEvent* event) override { event->ignore(); }
};

using Table = ReadOnly<QTableWidget>;
using List = ReadOnly<QListWidget>;
using Tree = ReadOnly<QTreeWidget>;

} // namespace ItemViews

#endif // BITCOIN_QT_ITEMVIEWS_H
