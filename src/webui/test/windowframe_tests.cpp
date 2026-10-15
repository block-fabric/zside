// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/windowframe.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>

using webui::WindowEdge;

BOOST_AUTO_TEST_SUITE(webui_windowframe_tests)

BOOST_AUTO_TEST_CASE(edge_names)
{
    for (const WindowEdge edge : {WindowEdge::TOP, WindowEdge::BOTTOM, WindowEdge::LEFT, WindowEdge::RIGHT,
                                  WindowEdge::TOP_LEFT, WindowEdge::TOP_RIGHT, WindowEdge::BOTTOM_LEFT, WindowEdge::BOTTOM_RIGHT}) {
        const auto name{webui::WindowEdgeName(edge)};
        BOOST_CHECK(!name.empty());
        BOOST_CHECK(webui::ParseWindowEdge(name) == edge);
    }
    BOOST_CHECK(webui::ParseWindowEdge("top-left") == WindowEdge::TOP_LEFT);
    BOOST_CHECK(!webui::ParseWindowEdge(""));
    BOOST_CHECK(!webui::ParseWindowEdge("north"));
    BOOST_CHECK(!webui::ParseWindowEdge("Top"));
}

BOOST_AUTO_TEST_CASE(title_bar_mode)
{
    using webui::TitleBarMode;
    BOOST_CHECK(webui::ParseTitleBarMode(UniValue{"system"}) == TitleBarMode::SYSTEM);
    BOOST_CHECK(webui::ParseTitleBarMode(UniValue{"integrated"}) == TitleBarMode::INTEGRATED);
    // Nothing kept yet, or something else: the integrated title bar.
    BOOST_CHECK(webui::ParseTitleBarMode(UniValue{}) == TitleBarMode::INTEGRATED);
    BOOST_CHECK(webui::ParseTitleBarMode(UniValue{"bogus"}) == TitleBarMode::INTEGRATED);
    BOOST_CHECK(webui::ParseTitleBarMode(UniValue{true}) == TitleBarMode::INTEGRATED);
    BOOST_CHECK_EQUAL(webui::TitleBarModeName(TitleBarMode::SYSTEM), "system");
    BOOST_CHECK_EQUAL(webui::TitleBarModeName(TitleBarMode::INTEGRATED), "integrated");
}

BOOST_AUTO_TEST_CASE(edge_at)
{
    // A window of 800x600, borders of 6 pixels, corners of 12.
    const auto at{[](int x, int y, bool maximized = false) { return webui::EdgeAt(x, y, 800, 600, 6, 12, maximized); }};
    BOOST_CHECK(!at(400, 300));
    BOOST_CHECK(at(400, 0) == WindowEdge::TOP);
    BOOST_CHECK(at(400, 5) == WindowEdge::TOP);
    BOOST_CHECK(!at(400, 6));
    BOOST_CHECK(at(400, 599) == WindowEdge::BOTTOM);
    BOOST_CHECK(at(0, 300) == WindowEdge::LEFT);
    BOOST_CHECK(at(799, 300) == WindowEdge::RIGHT);
    BOOST_CHECK(at(0, 0) == WindowEdge::TOP_LEFT);
    // Along an edge, near the corner: the corner.
    BOOST_CHECK(at(10, 2) == WindowEdge::TOP_LEFT);
    BOOST_CHECK(at(2, 10) == WindowEdge::TOP_LEFT);
    BOOST_CHECK(at(13, 2) == WindowEdge::TOP);
    BOOST_CHECK(at(795, 3) == WindowEdge::TOP_RIGHT);
    BOOST_CHECK(at(3, 595) == WindowEdge::BOTTOM_LEFT);
    BOOST_CHECK(at(799, 599) == WindowEdge::BOTTOM_RIGHT);
    BOOST_CHECK(at(790, 597) == WindowEdge::BOTTOM_RIGHT);
    // Outside the window, or maximized: nothing to resize.
    BOOST_CHECK(!at(-1, 300));
    BOOST_CHECK(!at(800, 300));
    BOOST_CHECK(!at(400, 600));
    BOOST_CHECK(!at(0, 0, /*maximized=*/true));
    BOOST_CHECK(!webui::EdgeAt(0, 0, 0, 0, 6, 12, false));
    BOOST_CHECK(!webui::EdgeAt(0, 0, 800, 600, 0, 12, false));
}

BOOST_AUTO_TEST_SUITE_END()
