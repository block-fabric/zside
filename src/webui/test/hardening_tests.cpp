// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The fixes of the audit of chains-app (doc/webui-audit.md) on the host side of the bridge: text that
// is not valid UTF-8 (H1), long calls (M1), the RPC methods the page may call (M3), secrets wiped (L12).

#include <rpc/protocol.h>
#include <rpc/request.h>
#include <test/util/setup_common.h>
#include <webui/bridge.h>
#include <webui/events.h>
#include <webui/chainconfig.h>
#include <webui/icontint.h>
#include <webui/rpcpolicy.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct Answers {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::pair<int, std::string>> raw;

    webui::Bridge::ReplyFn Fn()
    {
        return [this](const std::string& id, int status, const std::string& json) {
            {
                std::lock_guard lock{mutex};
                raw[id] = {status, json};
            }
            cv.notify_all();
        };
    }
    std::pair<int, std::string> Wait(const std::string& id, std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE_MESSAGE(cv.wait_for(lock, timeout, [&] { return raw.contains(id); }), "no answer for " + id);
        return raw[id];
    }
    bool Has(const std::string& id)
    {
        std::lock_guard lock{mutex};
        return raw.contains(id);
    }
};

std::string Call(const std::string& kind, const std::string& method, const UniValue& params = UniValue{UniValue::VARR})
{
    UniValue body{UniValue::VOBJ};
    body.pushKV("kind", kind);
    body.pushKV("method", method);
    body.pushKV("params", params);
    UniValue args{UniValue::VARR};
    args.push_back(body);
    return args.write();
}

/** Valid UTF-8 by the rules of the standard (what WebKit asks of a script). */
bool IsValidUtf8(const std::string& s)
{
    return webui::ValidUtf8(s) == s;
}

void NoUi(std::function<void()>) {}
} // namespace

BOOST_AUTO_TEST_SUITE(webui_hardening_tests)

BOOST_AUTO_TEST_CASE(valid_utf8)
{
    const std::string fffd{"\xEF\xBF\xBD"};
    // Valid text is kept as it is.
    for (const std::string text : {"", "plain", "é ü ß", "日本語", "\xF0\x9F\x98\x80 emoji", "\x7F", "\xEF\xBF\xBF", "\xF4\x8F\xBF\xBF"}) {
        BOOST_CHECK_EQUAL(webui::ValidUtf8(text), text);
    }
    // The title of the audit: a byte that cannot start a sequence, each replaced.
    BOOST_CHECK_EQUAL(webui::ValidUtf8("Bad \xff\xfe title"), "Bad " + fffd + fffd + " title");
    // Lone continuation bytes, overlong forms, surrogates, above U+10FFFF, cut short.
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\x80"), fffd);
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\xC0\xAF"), fffd + fffd);
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\xE0\x80\xAF"), fffd + fffd + fffd);
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\xED\xA0\x80"), fffd + fffd + fffd);
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\xF4\x90\x80\x80"), fffd + fffd + fffd + fffd);
    BOOST_CHECK_EQUAL(webui::ValidUtf8("ab\xE6\x97"), "ab" + fffd);
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\xE6\x97x"), fffd + "x");
    BOOST_CHECK_EQUAL(webui::ValidUtf8("\xF0\x9F\x98"), fffd);
    // Whatever comes in, valid UTF-8 goes out.
    std::string all;
    for (int i{0}; i < 256; ++i) {
        all += static_cast<char>(i);
        all += static_cast<char>(255 - i);
    }
    const std::string fixed{webui::ValidUtf8(all)};
    BOOST_CHECK(IsValidUtf8(fixed));
    BOOST_CHECK_EQUAL(webui::ValidUtf8(fixed), fixed);
}

BOOST_AUTO_TEST_CASE(answers_and_events_are_valid_utf8)
{
    // H1: a sidechain title or a deposit destination of raw bytes from the chain reaches the page.
    Answers answers;
    webui::Bridge bridge{answers.Fn(), NoUi, 2};
    bridge.SetRpc([](const std::string& method, const UniValue&, const std::string&) -> UniValue {
        UniValue proposal{UniValue::VOBJ};
        proposal.pushKV("title", std::string{"Bad \xff\xfe title"});
        proposal.pushKV("destination", std::string{"\xC3\x28\xA0\xA1 <b>x</b>"});
        UniValue list{UniValue::VARR};
        list.push_back(proposal);
        if (method == "throws") throw JSONRPCError(RPC_MISC_ERROR, std::string{"bad \xC0 message"});
        return list;
    });
    bridge.Handle("1", Call("rpc", "listsidechainproposals"));
    const auto [status, json]{answers.Wait("1")};
    BOOST_CHECK_EQUAL(status, 0);
    BOOST_CHECK(IsValidUtf8(json));
    UniValue value;
    BOOST_REQUIRE(value.read(json));
    BOOST_CHECK_EQUAL(value[0]["title"].get_str(), "Bad \xEF\xBF\xBD\xEF\xBF\xBD title");
    BOOST_CHECK_EQUAL(value[0]["destination"].get_str(), "\xEF\xBF\xBD(\xEF\xBF\xBD\xEF\xBF\xBD <b>x</b>");
    bridge.Handle("2", Call("rpc", "throws"));
    BOOST_CHECK(IsValidUtf8(answers.Wait("2").second));

    // The events take the same road to the page.
    std::string delivered;
    webui::EventHub hub{[&](const std::string& batch) { delivered = batch; }, [](std::chrono::milliseconds, std::function<void()> fn) { fn(); }};
    hub.SetReady(true);
    UniValue data{UniValue::VOBJ};
    data.pushKV("title", std::string{"\xff title"});
    hub.Emit("sidechains", data);
    hub.Flush();
    BOOST_REQUIRE(!delivered.empty());
    BOOST_CHECK(IsValidUtf8(delivered));
    BOOST_CHECK(delivered.find("\xEF\xBF\xBD title") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(page_rpc_policy)
{
    // M3: the page calls the methods it needs, no other; the console (a host method) runs any command.
    BOOST_CHECK(webui::PageRpcPolicy("getblockchaininfo") == webui::Bridge::Thread::WORKER);
    BOOST_CHECK(webui::PageRpcPolicy("scantxoutset") == webui::Bridge::Thread::LONG);
    for (const char* refused : {"backupwallet", "dumptxoutset", "stop", "dumpwallet", "importdescriptors", "rescanblockchain", "sendtoaddress",
                                "waitfornewblock", "setnetworkactive",
                                "addnode", "setban", "pruneblockchain", "savemempool", "invalidateblock", "submitblock", "loadwallet", "unloadwallet"}) {
        BOOST_CHECK_MESSAGE(!webui::PageRpcPolicy(refused), refused);
    }
    // The methods that need the keys go through wallet.withKeys, which unlocks for the call alone.
    for (const char* keyed : {"send", "walletprocesspsbt", "signmessage"}) BOOST_CHECK(webui::KeyedWalletRpc(keyed));
    // The deposit of the mainchain, the withdrawals of a sidechain (sidechainapi_tests).
    BOOST_CHECK(webui::KeyedWalletRpc(WEBUI_SIDECHAIN ? "createwithdrawal" : "createsidechaindeposit"));
    for (const char* other : {"dumpwallet", "walletpassphrase", "sendtoaddress", "backupwallet"}) BOOST_CHECK(!webui::KeyedWalletRpc(other));

    Answers answers;
    webui::Bridge bridge{answers.Fn(), NoUi, 2};
    std::atomic<int> ran{0};
    bridge.SetRpc([&](const std::string& method, const UniValue&, const std::string&) -> UniValue {
        ++ran;
        return method;
    });
    bridge.SetRpcPolicy(webui::PageRpcPolicy);
    bridge.Handle("ok", Call("rpc", "getblockchaininfo"));
    BOOST_CHECK_EQUAL(answers.Wait("ok").first, 0);
    bridge.Handle("no", Call("rpc", "backupwallet"));
    const auto refused{answers.Wait("no")};
    BOOST_CHECK_EQUAL(refused.first, 1);
    UniValue error;
    BOOST_REQUIRE(error.read(refused.second));
    BOOST_CHECK_EQUAL(error["code"].getInt<int>(), webui::BRIDGE_METHOD_NOT_FOUND);
    BOOST_CHECK_EQUAL(ran.load(), 1);
    BOOST_CHECK_EQUAL(bridge.Pending(), 0U);
    // What the policy refused is kept, once each (test.refused: the e2e runs fail on it).
    bridge.Handle("no2", Call("rpc", "backupwallet"));
    answers.Wait("no2");
    BOOST_CHECK(bridge.Refused() == std::vector<std::string>{"backupwallet"});

    // Audit port-1 #5: a test script sets the node up with "testrpc", past the policy, and only once opened
    // (a test build with a test script); the pages' "rpc" keeps to the policy all the same.
    bridge.Handle("t1", Call("testrpc", "backupwallet"));
    const auto closed{answers.Wait("t1")};
    BOOST_CHECK_EQUAL(closed.first, 1);
    BOOST_REQUIRE(error.read(closed.second));
    BOOST_CHECK_EQUAL(error["code"].getInt<int>(), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ran.load(), 1);
    bridge.OpenTestRpc();
    bridge.Handle("t2", Call("testrpc", "backupwallet"));
    BOOST_CHECK_EQUAL(answers.Wait("t2").first, 0);
    BOOST_CHECK_EQUAL(ran.load(), 2);
    bridge.Handle("t3", Call("rpc", "dumpwallet"));
    BOOST_CHECK_EQUAL(answers.Wait("t3").first, 1);
    BOOST_CHECK_EQUAL(ran.load(), 2);
    BOOST_CHECK(bridge.Refused() == (std::vector<std::string>{"backupwallet", "dumpwallet"}));

    // The list is sorted and has no duplicates (PageRpcMethods is what the frontend test reads).
    const auto methods{webui::PageRpcMethods()};
    BOOST_CHECK(std::is_sorted(methods.begin(), methods.end()));
    BOOST_CHECK(std::adjacent_find(methods.begin(), methods.end()) == methods.end());
}

BOOST_AUTO_TEST_CASE(long_calls_do_not_hold_up_quick_ones)
{
    // M1: four calls that take long (downloads, rescans, a long command) left every other call waiting.
    Answers answers;
    webui::Bridge bridge{answers.Fn(), NoUi, /*workers=*/4, /*long_workers=*/6};
    std::mutex mutex;
    std::condition_variable cv;
    bool release{false};
    std::atomic<int> waiting{0};
    const auto slow{[&](const UniValue&) {
        std::unique_lock lock{mutex};
        ++waiting;
        cv.wait(lock, [&] { return release; });
        return UniValue{"slow"};
    }};
    bridge.RegisterHost("slow", slow, webui::Bridge::Thread::LONG);
    bridge.RegisterHost("quick", [](const UniValue&) { return UniValue{"quick"}; });
    bridge.SetRpc([&](const std::string& method, const UniValue& params, const std::string&) -> UniValue {
        if (method == "scantxoutset") return slow(params);
        return UniValue{"rpc"};
    });
    bridge.SetRpcPolicy(webui::PageRpcPolicy);
    for (int i{0}; i < 4; ++i) bridge.Handle("slow" + std::to_string(i), Call("host", "slow", UniValue{UniValue::VOBJ}));
    for (int i{0}; i < 2; ++i) bridge.Handle("scan" + std::to_string(i), Call("rpc", "scantxoutset"));
    for (int i{0}; i < 200 && waiting < 6; ++i) std::this_thread::sleep_for(5ms);
    BOOST_CHECK_EQUAL(waiting.load(), 6);
    // Every worker of the long calls is busy: the quick ones still answer at once.
    const auto start{std::chrono::steady_clock::now()};
    bridge.Handle("q1", Call("host", "quick", UniValue{UniValue::VOBJ}));
    bridge.Handle("q2", Call("rpc", "getblockcount"));
    BOOST_CHECK_EQUAL(answers.Wait("q1", 2s).first, 0);
    BOOST_CHECK_EQUAL(answers.Wait("q2", 2s).first, 0);
    BOOST_CHECK(std::chrono::steady_clock::now() - start < 1s);
    BOOST_CHECK(!answers.Has("slow0"));
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    cv.notify_all();
    for (int i{0}; i < 4; ++i) BOOST_CHECK_EQUAL(answers.Wait("slow" + std::to_string(i)).first, 0);
    bridge.Stop();
}

BOOST_AUTO_TEST_CASE(secrets_are_wiped)
{
    // L12: the passphrases of a request do not stay in the memory of its params once it is done.
    UniValue params{UniValue::VOBJ};
    params.pushKV("wallet", "w");
    params.pushKV("passphrase", "correct horse");
    params.pushKV("old", "old one");
    params.pushKV("new", "new one");
    UniValue nested{UniValue::VOBJ};
    nested.pushKV("passphrase", "deep");
    UniValue list{UniValue::VARR};
    list.push_back(nested);
    params.pushKV("calls", list);
    const char* before{params["passphrase"].get_str().data()};
    webui::CleanseSecrets(params);
    BOOST_CHECK_EQUAL(params["wallet"].get_str(), "w");
    BOOST_CHECK_EQUAL(params["passphrase"].get_str().size(), 13U);
    BOOST_CHECK(params["passphrase"].get_str() != "correct horse");
    BOOST_CHECK_EQUAL(params["passphrase"].get_str().data(), before);
    BOOST_CHECK(params["passphrase"].get_str().find_first_not_of('\0') == std::string::npos);
    BOOST_CHECK(params["old"].get_str().find_first_not_of('\0') == std::string::npos);
    BOOST_CHECK(params["new"].get_str().find_first_not_of('\0') == std::string::npos);
    BOOST_CHECK(params["calls"][0]["passphrase"].get_str().find_first_not_of('\0') == std::string::npos);

    // The bridge wipes what a host method got, once it answered.
    Answers answers;
    webui::Bridge bridge{answers.Fn(), NoUi, 2};
    std::string seen_value;
    bridge.RegisterHost("keep", [&](const UniValue& p) {
        seen_value = p["passphrase"].get_str();
        return UniValue{true};
    });
    UniValue request{UniValue::VOBJ};
    request.pushKV("passphrase", "hunter2");
    bridge.Handle("k", Call("host", "keep", request));
    BOOST_CHECK_EQUAL(answers.Wait("k").first, 0);
    BOOST_CHECK_EQUAL(seen_value, "hunter2");
    bridge.Stop();
}

BOOST_AUTO_TEST_CASE(internal_errors)
{
    // A17: an exception a host method did not expect shows the internal error dialog; a HostError does not.
    Answers answers;
    webui::Bridge bridge{answers.Fn(), NoUi, 2};
    std::mutex mutex;
    std::vector<std::string> internal;
    bridge.SetInternalError([&](const std::string& method, const std::string& what) {
        std::lock_guard lock{mutex};
        internal.push_back(method + ": " + what);
    });
    bridge.RegisterHost("expected", [](const UniValue&) -> UniValue { throw webui::HostError("an error the page shows"); });
    bridge.RegisterHost("unexpected", [](const UniValue&) -> UniValue { throw std::runtime_error("a bug"); });
    bridge.Handle("e", Call("host", "expected", UniValue{UniValue::VOBJ}));
    bridge.Handle("u", Call("host", "unexpected", UniValue{UniValue::VOBJ}));
    BOOST_CHECK_EQUAL(answers.Wait("e").first, 1);
    BOOST_CHECK_EQUAL(answers.Wait("u").first, 1);
    bridge.Stop();
    std::lock_guard lock{mutex};
    BOOST_REQUIRE_EQUAL(internal.size(), 1U);
    BOOST_CHECK_EQUAL(internal[0], "unexpected: a bug");
}

BOOST_AUTO_TEST_CASE(network_icon)
{
    // A08: the icon of the app in the colours of the network, as NetworkStyle tints it.
    // The hue of the chain (chain.json brand.iconHue) comes first.
    const int hue{webui::chain::ICON_HUE};
    BOOST_CHECK_EQUAL(webui::TintOf("main").None(), hue % 360 == 0);
    BOOST_CHECK_EQUAL(webui::TintOf("main").hue_shift, hue % 360);
    BOOST_CHECK_EQUAL(webui::TintOf("regtest").hue_shift, (160 + hue) % 360);
    BOOST_CHECK_EQUAL(webui::TintOf("test").saturation_reduction, 30);
    BOOST_CHECK_EQUAL(webui::TintOf("signet").hue_shift, (35 + hue) % 360);
    // A red pixel, a grey one, a transparent green one (RGBA).
    std::vector<unsigned char> pixels{255, 0, 0, 255, 128, 128, 128, 200, 0, 255, 0, 0};
    webui::TintPixels(pixels, 4, 3, 1, 12, webui::IconTint{});
    BOOST_CHECK(pixels[0] == 255 && pixels[1] == 0);
    webui::TintPixels(pixels, 4, 3, 1, 12, webui::IconTint{160, 30});
    // Red turned by 160 degrees: a green-cyan, less saturated; the lightness and the alpha kept.
    BOOST_CHECK(pixels[0] < 60 && pixels[1] > 200 && pixels[2] > 100 && pixels[2] < 200);
    BOOST_CHECK_EQUAL(pixels[3], 255);
    // Grey stays grey.
    BOOST_CHECK(pixels[4] == 128 && pixels[5] == 128 && pixels[6] == 128 && pixels[7] == 200);
    BOOST_CHECK_EQUAL(pixels[11], 0);
}

BOOST_AUTO_TEST_SUITE_END()
