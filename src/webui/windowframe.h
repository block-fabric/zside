// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_WINDOWFRAME_H
#define BITCOIN_WEBUI_WINDOWFRAME_H

#include <optional>
#include <string_view>

class UniValue;

namespace webui {

/**
 * The frame of the main window: the integrated title bar (the page draws the title bar, its
 * minimize, maximize and close buttons, and the resize handles at the edges; the window has no
 * frame of the system) or the title bar of the system. What does not need a window is here, for
 * every platform and for the tests.
 */

//! An edge or a corner of the window, where a drag resizes it.
enum class WindowEdge { TOP, BOTTOM, LEFT, RIGHT, TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT };

//! The name of an edge, as the page says it: "top", "bottom", "left", "right", "top-left", "top-right", "bottom-left", "bottom-right".
std::string_view WindowEdgeName(WindowEdge edge);
//! The edge of a name, nothing for a name that is not one.
std::optional<WindowEdge> ParseWindowEdge(std::string_view name);

//! Which title bar the window has (the GUI preference "titleBar" of the app settings).
enum class TitleBarMode { INTEGRATED, SYSTEM };

std::string_view TitleBarModeName(TitleBarMode mode);
//! "integrated" or "system"; anything else (nothing kept yet) is the default, the integrated title bar.
TitleBarMode ParseTitleBarMode(const UniValue& value);

/**
 * The edge under a point of a window without a frame of the system: within `border` pixels of
 * an edge, within `corner` pixels of a corner. Nothing inside, outside, or when the window is
 * maximized (it cannot be resized then). For the hit test of a platform that asks the window
 * (WM_NCHITTEST on Windows), the page's handles doing the same elsewhere.
 */
std::optional<WindowEdge> EdgeAt(int x, int y, int width, int height, int border, int corner, bool maximized);

} // namespace webui

#endif // BITCOIN_WEBUI_WINDOWFRAME_H
