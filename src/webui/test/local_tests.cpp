// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of zSide's pages (local.cpp): the viewing key is shown only after the desktop asked (audit
// port-2 #3: exportshieldedviewingkey was on the page's allowlist, and the node answers it with the wallet
// locked, so a page could read the key with no question asked).

#include <test/util/setup_common.h>
#include <webui/bridge.h>
#include <webui/local.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

using namespace std::chrono_literals;

namespace {
struct Answers {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::pair<int, UniValue>> answers;
    webui::Bridge::ReplyFn Fn()
    {
        return [this](const std::string& id, int status, const std::string& json) {
            UniValue value;
            if (!json.empty()) value.read(json);
            {
                std::lock_guard lock{mutex};
                answers.emplace(id, std::pair{status, value});
            }
            cv.notify_all();
        };
    }
    std::pair<int, UniValue> Wait(const std::string& id)
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE_MESSAGE(cv.wait_for(lock, 10s, [&] { return answers.contains(id); }), "no answer for " + id);
        return answers.at(id);
    }
};

std::string Call(const std::string& method, const UniValue& params)
{
    UniValue body{UniValue::VOBJ};
    body.pushKV("kind", "host");
    body.pushKV("method", method);
    body.pushKV("params", params);
    UniValue args{UniValue::VARR};
    args.push_back(body);
    return args.write();
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_local_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(viewing_key_waits_for_the_desktop)
{
    // The page may not call the RPC method itself.
    BOOST_CHECK(!webui::local::RpcMethods().contains("exportshieldedviewingkey"));
    // What pays or signs: never the page's to call, only through the host, which confirms it on the desktop (audit 7 #1).
    for (const char* m : {"sendshielded", "shieldcoins", "unshieldcoins", "bumpshieldedfee", "meltcoins", "castcoins"}) {
        BOOST_CHECK_MESSAGE(webui::local::ConfirmedRpcMethods().contains(m), m);
        BOOST_CHECK_MESSAGE(!webui::local::RpcMethods().contains(m) && !webui::local::KeyedRpcMethods().contains(m), m);
    }

    Answers answers;
    webui::Bridge bridge{answers.Fn(), [](std::function<void()> fn) { fn(); }};
    std::atomic<int> node_asked{0};
    std::atomic<int> asked{0};
    std::atomic<bool> yes{false};
    std::string shown;
    webui::local::RegisterHost(
        bridge,
        [&]() -> interfaces::Node& {
            ++node_asked;
            // No node in this test: reaching it is what is checked.
            throw webui::HostError(-4242, "the node was asked");
        },
        [&](const webui::Window::ConfirmDialog& dialog) {
            ++asked;
            shown = dialog.message;
            return yes.load();
        },
        /*keys=*/{});

    UniValue params{UniValue::VOBJ};
    params.pushKV("wallet", "savings\n‮evil");
    // Declined on the desktop: cancelled, and the node never asked.
    bridge.Handle("1", Call("zside.viewingkey.export", params));
    const auto no{answers.Wait("1")};
    BOOST_CHECK_EQUAL(no.first, 1);
    BOOST_CHECK_EQUAL(no.second.find_value("code").getInt<int>(), webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(asked.load(), 1);
    BOOST_CHECK_EQUAL(node_asked.load(), 0);
    // The question names the wallet on one line, and says what the key shows.
    BOOST_CHECK(shown.starts_with("Wallet: savings"));
    BOOST_CHECK(shown.find("every payment") != std::string::npos);
    BOOST_CHECK_EQUAL(shown.substr(0, shown.find("\n\n")).find('\n'), std::string::npos);

    // Yes: then, and only then, the node.
    yes = true;
    bridge.Handle("2", Call("zside.viewingkey.export", params));
    const auto ok{answers.Wait("2")};
    BOOST_CHECK_EQUAL(ok.second.find_value("code").getInt<int>(), -4242);
    BOOST_CHECK_EQUAL(asked.load(), 2);
    BOOST_CHECK_EQUAL(node_asked.load(), 1);

    // No wallet named: refused before anything is asked.
    bridge.Handle("3", Call("zside.viewingkey.export", UniValue{UniValue::VOBJ}));
    BOOST_CHECK_EQUAL(answers.Wait("3").second.find_value("code").getInt<int>(), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(asked.load(), 2);
    bridge.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
