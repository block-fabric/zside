// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/windowframe.h>

#include <univalue.h>

#include <algorithm>
#include <array>
#include <utility>

namespace webui {
namespace {
constexpr std::array<std::pair<WindowEdge, std::string_view>, 8> EDGES{{
    {WindowEdge::TOP, "top"},
    {WindowEdge::BOTTOM, "bottom"},
    {WindowEdge::LEFT, "left"},
    {WindowEdge::RIGHT, "right"},
    {WindowEdge::TOP_LEFT, "top-left"},
    {WindowEdge::TOP_RIGHT, "top-right"},
    {WindowEdge::BOTTOM_LEFT, "bottom-left"},
    {WindowEdge::BOTTOM_RIGHT, "bottom-right"},
}};
} // namespace

std::string_view WindowEdgeName(WindowEdge edge)
{
    for (const auto& [e, name] : EDGES) {
        if (e == edge) return name;
    }
    return "";
}

std::optional<WindowEdge> ParseWindowEdge(std::string_view name)
{
    for (const auto& [e, n] : EDGES) {
        if (n == name) return e;
    }
    return std::nullopt;
}

std::string_view TitleBarModeName(TitleBarMode mode)
{
    return mode == TitleBarMode::SYSTEM ? "system" : "integrated";
}

TitleBarMode ParseTitleBarMode(const UniValue& value)
{
    return value.isStr() && value.get_str() == "system" ? TitleBarMode::SYSTEM : TitleBarMode::INTEGRATED;
}

std::optional<WindowEdge> EdgeAt(int x, int y, int width, int height, int border, int corner, bool maximized)
{
    if (maximized || width <= 0 || height <= 0 || border <= 0) return std::nullopt;
    if (x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;
    corner = std::max(corner, border);
    const bool top{y < border}, bottom{y >= height - border}, left{x < border}, right{x >= width - border};
    if (!top && !bottom && !left && !right) return std::nullopt;
    // Near a corner along either of its edges: the corner.
    const bool near_top{y < corner}, near_bottom{y >= height - corner}, near_left{x < corner}, near_right{x >= width - corner};
    if ((top || left) && near_top && near_left) return WindowEdge::TOP_LEFT;
    if ((top || right) && near_top && near_right) return WindowEdge::TOP_RIGHT;
    if ((bottom || left) && near_bottom && near_left) return WindowEdge::BOTTOM_LEFT;
    if ((bottom || right) && near_bottom && near_right) return WindowEdge::BOTTOM_RIGHT;
    if (top) return WindowEdge::TOP;
    if (bottom) return WindowEdge::BOTTOM;
    if (left) return WindowEdge::LEFT;
    return WindowEdge::RIGHT;
}

} // namespace webui
