// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/quitguard.h>

#include <boost/test/unit_test.hpp>

#include <string>

using webui::QuitGuard;

BOOST_AUTO_TEST_SUITE(webui_quitguard_tests)

BOOST_AUTO_TEST_CASE(no_guard_quits)
{
    QuitGuard guard;
    BOOST_CHECK(!guard.Message(100));
    BOOST_CHECK(guard.OnQuit(100) == QuitGuard::Decision::QUIT);
    BOOST_CHECK(!guard.Asking());
    // Cleared by the page: quits.
    guard.Set("A market is still being created (x).", 100);
    guard.Set("", 101);
    BOOST_CHECK(guard.OnQuit(102) == QuitGuard::Decision::QUIT);
}

BOOST_AUTO_TEST_CASE(asks_once_then_second_quit_quits)
{
    QuitGuard guard;
    guard.Set("A market is still being created (Rain).", 1000);
    BOOST_CHECK_EQUAL(*guard.Message(1000), "A market is still being created (Rain).");
    BOOST_CHECK(guard.OnQuit(1001) == QuitGuard::Decision::ASK);
    BOOST_CHECK(guard.Asking());
    // The window closed again while the question shows: the user means it.
    BOOST_CHECK(guard.OnQuit(1002) == QuitGuard::Decision::QUIT);
    // "Keep open": the next quit asks again.
    guard.Answered();
    BOOST_CHECK(!guard.Asking());
    BOOST_CHECK(guard.OnQuit(1003) == QuitGuard::Decision::ASK);
    guard.Answered();
}

BOOST_AUTO_TEST_CASE(forgotten_unless_said_again)
{
    QuitGuard guard;
    guard.Set("work", 1000);
    BOOST_CHECK(guard.Message(1000 + QuitGuard::TTL_S));
    BOOST_CHECK(!guard.Message(1000 + QuitGuard::TTL_S + 1));
    // A page that hangs or went away holds nothing.
    BOOST_CHECK(guard.OnQuit(1000 + QuitGuard::TTL_S + 1) == QuitGuard::Decision::QUIT);
    // Said again: holds again.
    guard.Set("work", 2000);
    BOOST_CHECK(guard.OnQuit(2030) == QuitGuard::Decision::ASK);
    // A clock that went back: not trusted to hold.
    BOOST_CHECK(!guard.Message(1999));
}

BOOST_AUTO_TEST_CASE(long_text_cut_on_a_character)
{
    QuitGuard guard;
    std::string text(QuitGuard::MAX_MESSAGE - 1, 'a');
    text += "é and more";
    guard.Set(text, 10);
    const std::string shown{*guard.Message(10)};
    BOOST_CHECK(shown.size() <= QuitGuard::MAX_MESSAGE + 3);
    BOOST_CHECK(shown.ends_with("a…"));
}

BOOST_AUTO_TEST_SUITE_END()
