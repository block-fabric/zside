// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <tinyformat.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <webui/bridge.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
/** Collects the answers of a bridge. */
struct Replies {
    std::mutex mutex;
    std::condition_variable cv;
    struct Reply {
        int status;
        UniValue value;
    };
    std::multimap<std::string, Reply> replies;

    webui::Bridge::ReplyFn Fn()
    {
        return [this](const std::string& id, int status, const std::string& json) {
            UniValue value;
            BOOST_REQUIRE(json.empty() || value.read(json));
            {
                std::lock_guard lock{mutex};
                replies.emplace(id, Reply{status, value});
            }
            cv.notify_all();
        };
    }

    /** The answer of a call, waiting for it. */
    Reply Wait(const std::string& id)
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE_MESSAGE(cv.wait_for(lock, 10s, [&] { return replies.contains(id); }), "no answer for " + id);
        return replies.find(id)->second;
    }

    size_t Count(const std::string& id)
    {
        std::lock_guard lock{mutex};
        return replies.count(id);
    }
};

/** Runs the functions of the "UI thread" when the test says so. */
struct UiQueue {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    webui::Bridge::UiRunner Fn()
    {
        return [this](std::function<void()> fn) {
            std::lock_guard lock{mutex};
            queue.push_back(std::move(fn));
        };
    }
    void RunAll()
    {
        std::deque<std::function<void()>> todo;
        {
            std::lock_guard lock{mutex};
            todo.swap(queue);
        }
        for (auto& fn : todo) fn();
    }
};

std::string Request(const std::string& kind, const std::string& method, const UniValue& params = UniValue{UniValue::VARR}, const std::string& rid = "", const std::string& wallet = "")
{
    UniValue body{UniValue::VOBJ};
    if (!rid.empty()) body.pushKV("rid", rid);
    body.pushKV("kind", kind);
    body.pushKV("method", method);
    body.pushKV("params", params);
    if (!wallet.empty()) body.pushKV("wallet", wallet);
    UniValue args{UniValue::VARR};
    args.push_back(body);
    return args.write();
}

int ErrorCode(const Replies::Reply& reply)
{
    BOOST_REQUIRE_EQUAL(reply.status, 1);
    return reply.value.find_value("code").getInt<int>();
}

/** A gate that RPC calls wait at, until the test opens it. */
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open{false};
    std::atomic<int> waiting{0};
    void Wait()
    {
        std::unique_lock lock{mutex};
        ++waiting;
        cv.wait(lock, [&] { return open; });
    }
    void Open()
    {
        {
            std::lock_guard lock{mutex};
            open = true;
        }
        cv.notify_all();
    }
    void UntilWaiting(int n)
    {
        for (int i{0}; i < 1000 && waiting < n; ++i) std::this_thread::sleep_for(5ms);
        BOOST_REQUIRE_EQUAL(waiting.load(), n);
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(webui_bridge_tests)

BOOST_AUTO_TEST_CASE(percent_encoding)
{
    BOOST_CHECK_EQUAL(webui::PercentEncode("abcXYZ019-._~"), "abcXYZ019-._~");
    BOOST_CHECK_EQUAL(webui::PercentEncode("my wallet/1"), "my%20wallet%2F1");
    BOOST_CHECK_EQUAL(webui::PercentEncode("é%?"), "%C3%A9%25%3F");
    BOOST_CHECK_EQUAL(webui::WalletUri(""), "/wallet/");
    BOOST_CHECK_EQUAL(webui::WalletUri("a b"), "/wallet/a%20b");
}

BOOST_AUTO_TEST_CASE(invalid_requests)
{
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 2};
    bridge.Handle("1", "not json");
    bridge.Handle("2", "[]");
    bridge.Handle("3", R"([{"method":"getblockcount"}])");
    bridge.Handle("4", R"([{"kind":"other","method":"x"}])");
    bridge.Handle("5", R"([{"kind":"rpc"}])");
    bridge.Handle("6", R"([{"kind":"rpc","method":"x","params":"text"}])");
    bridge.Handle("7", R"([{"kind":"rpc","method":"x","wallet":3}])");
    bridge.Handle("8", R"([{"kind":"rpc","method":"x","rid":7}])");
    bridge.Handle("9", R"([{"kind":"cancel"}])");
    bridge.Handle("10", R"([{"kind":"rpc","method":"x"},{"kind":"rpc","method":"y"}])");
    for (const char* id : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}) {
        BOOST_CHECK_EQUAL(ErrorCode(replies.Wait(id)), webui::BRIDGE_INVALID_REQUEST);
    }
    BOOST_CHECK_EQUAL(bridge.Pending(), 0U);
}

BOOST_AUTO_TEST_CASE(rpc_calls)
{
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 2};

    // Before the node runs.
    bridge.Handle("early", Request("rpc", "getblockcount"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("early")), webui::BRIDGE_NOT_READY);

    std::mutex seen_mutex;
    std::vector<std::string> seen;
    bridge.SetRpc([&](const std::string& method, const UniValue& params, const std::string& uri) -> UniValue {
        {
            std::lock_guard lock{seen_mutex};
            seen.push_back(method + " " + params.write() + " " + uri);
        }
        if (method == "fails") throw JSONRPCError(RPC_INVALID_PARAMETER, "bad parameter");
        if (method == "throws") throw std::runtime_error("broken");
        if (method == "odd") throw UniValue{"just a string"};
        UniValue result{UniValue::VOBJ};
        result.pushKV("method", method);
        result.pushKV("text", "<script>alert(1)</script>\"'\n");
        return result;
    });

    UniValue params{UniValue::VARR};
    params.push_back(5);
    bridge.Handle("a", Request("rpc", "getblockhash", params));
    auto a{replies.Wait("a")};
    BOOST_CHECK_EQUAL(a.status, 0);
    BOOST_CHECK_EQUAL(a.value.find_value("method").get_str(), "getblockhash");
    // Text of the chain comes back as data, exactly.
    BOOST_CHECK_EQUAL(a.value.find_value("text").get_str(), "<script>alert(1)</script>\"'\n");

    UniValue named{UniValue::VOBJ};
    named.pushKV("verbose", true);
    bridge.Handle("b", Request("rpc", "getbalances", named, "rid-b", "my wallet"));
    BOOST_CHECK_EQUAL(replies.Wait("b").status, 0);

    bridge.Handle("c", Request("rpc", "fails"));
    auto c{replies.Wait("c")};
    BOOST_CHECK_EQUAL(ErrorCode(c), RPC_INVALID_PARAMETER);
    BOOST_CHECK_EQUAL(c.value.find_value("message").get_str(), "bad parameter");

    bridge.Handle("d", Request("rpc", "throws"));
    auto d{replies.Wait("d")};
    BOOST_CHECK_EQUAL(ErrorCode(d), webui::BRIDGE_HOST_ERROR);
    BOOST_CHECK_EQUAL(d.value.find_value("message").get_str(), "broken");

    bridge.Handle("e", Request("rpc", "odd"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("e")), webui::BRIDGE_HOST_ERROR);

    // Requests without params get an empty array.
    bridge.Handle("f", R"([{"kind":"rpc","method":"getblockcount"}])");
    BOOST_CHECK_EQUAL(replies.Wait("f").status, 0);

    std::lock_guard lock{seen_mutex};
    BOOST_CHECK(std::find(seen.begin(), seen.end(), "getblockhash [5] /") != seen.end());
    BOOST_CHECK(std::find(seen.begin(), seen.end(), R"(getbalances {"verbose":true} /wallet/my%20wallet)") != seen.end());
    BOOST_CHECK(std::find(seen.begin(), seen.end(), "getblockcount [] /") != seen.end());
}

BOOST_AUTO_TEST_CASE(host_calls)
{
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 2};
    std::atomic<bool> on_worker{false};
    const auto main_thread{std::this_thread::get_id()};
    bridge.RegisterHost("echo", [&](const UniValue& params) {
        on_worker = std::this_thread::get_id() != main_thread;
        return params;
    });
    bridge.RegisterHost("fail", [](const UniValue&) -> UniValue { throw webui::HostError(-42, "no way"); });
    bridge.RegisterHost("std", [](const UniValue&) -> UniValue { throw std::runtime_error("std error"); });
    std::atomic<int> ui_runs{0};
    bridge.RegisterHost("ui", [&](const UniValue&) {
        ++ui_runs;
        return UniValue{"from the ui"};
    }, webui::Bridge::Thread::UI);

    UniValue params{UniValue::VOBJ};
    params.pushKV("x", 1);
    bridge.Handle("1", Request("host", "echo", params));
    auto r1{replies.Wait("1")};
    BOOST_CHECK_EQUAL(r1.status, 0);
    BOOST_CHECK_EQUAL(r1.value.write(), R"({"x":1})");
    BOOST_CHECK(on_worker);

    bridge.Handle("2", Request("host", "fail"));
    auto r2{replies.Wait("2")};
    BOOST_CHECK_EQUAL(ErrorCode(r2), -42);
    BOOST_CHECK_EQUAL(r2.value.find_value("message").get_str(), "no way");

    bridge.Handle("3", Request("host", "std"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("3")), webui::BRIDGE_HOST_ERROR);

    bridge.Handle("4", Request("host", "nothing.here"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("4")), webui::BRIDGE_METHOD_NOT_FOUND);

    // A method of the UI thread runs when the UI thread runs it, not before.
    bridge.Handle("5", Request("host", "ui"));
    std::this_thread::sleep_for(20ms);
    BOOST_CHECK_EQUAL(replies.Count("5"), 0U);
    BOOST_CHECK_EQUAL(ui_runs.load(), 0);
    ui.RunAll();
    auto r5{replies.Wait("5")};
    BOOST_CHECK_EQUAL(r5.value.get_str(), "from the ui");
    BOOST_CHECK_EQUAL(bridge.Pending(), 0U);
}

BOOST_AUTO_TEST_CASE(cancel)
{
    Replies replies;
    UiQueue ui;
    // One worker: the second request waits behind the first.
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 1};
    Gate gate;
    std::atomic<int> runs{0};
    bridge.SetRpc([&](const std::string& method, const UniValue&, const std::string&) -> UniValue {
        ++runs;
        if (method == "slow") gate.Wait();
        return UniValue{method};
    });

    bridge.Handle("1", Request("rpc", "slow", UniValue{UniValue::VARR}, "r1"));
    gate.UntilWaiting(1);
    bridge.Handle("2", Request("rpc", "queued", UniValue{UniValue::VARR}, "r2"));
    BOOST_CHECK_EQUAL(bridge.Pending(), 2U);

    // Cancel the one that waits: answered at once, never run.
    bridge.Handle("c2", R"([{"kind":"cancel","rid":"r2"}])");
    BOOST_CHECK_EQUAL(replies.Wait("c2").value.get_bool(), true);
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("2")), webui::BRIDGE_CANCELLED);

    // Cancel the one that runs: answered at once, its result is dropped.
    bridge.Handle("c1", R"([{"kind":"cancel","rid":"r1"}])");
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("1")), webui::BRIDGE_CANCELLED);
    gate.Open();
    // A later request runs after the slow one finished.
    bridge.Handle("3", Request("rpc", "after", UniValue{UniValue::VARR}, "r3"));
    BOOST_CHECK_EQUAL(replies.Wait("3").value.get_str(), "after");
    BOOST_CHECK_EQUAL(replies.Count("1"), 1U);
    BOOST_CHECK_EQUAL(replies.Count("2"), 1U);
    BOOST_CHECK_EQUAL(runs.load(), 2); // slow and after; queued never ran

    // Cancelling what is not there.
    bridge.Handle("c4", R"([{"kind":"cancel","rid":"nothing"}])");
    BOOST_CHECK_EQUAL(replies.Wait("c4").value.get_bool(), false);
    BOOST_CHECK_EQUAL(bridge.Pending(), 0U);
}

BOOST_AUTO_TEST_CASE(request_gone_before_its_answer)
{
    // The answer of a request goes out only after the request left the list of pending ones: when
    // the page (or a test) has the answer, Pending() no longer counts it, on every path.
    UiQueue ui;
    webui::Bridge* bridge_ptr{nullptr};
    std::mutex mutex;
    std::map<std::string, size_t> pending_at_answer;
    std::condition_variable cv;
    webui::Bridge bridge{[&](const std::string& id, int, const std::string&) {
                             const size_t pending{bridge_ptr->Pending()};
                             {
                                 std::lock_guard lock{mutex};
                                 pending_at_answer[id] = pending;
                             }
                             cv.notify_all();
                         },
                         ui.Fn(), 2, 2};
    bridge_ptr = &bridge;
    bridge.SetRpc([](const std::string& method, const UniValue&, const std::string&) -> UniValue {
        if (method == "fails") throw JSONRPCError(RPC_MISC_ERROR, "no");
        return UniValue{method};
    });
    bridge.RegisterHost("worker", [](const UniValue&) { return UniValue{1}; });
    bridge.RegisterHost("long", [](const UniValue&) { return UniValue{2}; }, webui::Bridge::Thread::LONG);
    bridge.RegisterHost("ui", [](const UniValue&) { return UniValue{3}; }, webui::Bridge::Thread::UI);
    bridge.RegisterHost("throws", [](const UniValue&) -> UniValue { throw std::runtime_error{"boom"}; });

    const auto wait{[&](const std::string& id) {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE_MESSAGE(cv.wait_for(lock, 10s, [&] { return pending_at_answer.contains(id); }), "no answer for " + id);
        return pending_at_answer.at(id);
    }};
    // One at a time, so that nothing else is pending when an answer goes out.
    for (int round{0}; round < 50; ++round) {
        const std::string n{strprintf("%d-", round)};
        bridge.Handle(n + "a", Request("rpc", "ok", UniValue{UniValue::VARR}, "ra"));
        BOOST_CHECK_EQUAL(wait(n + "a"), 0U);
        bridge.Handle(n + "b", Request("rpc", "fails"));
        BOOST_CHECK_EQUAL(wait(n + "b"), 0U);
        bridge.Handle(n + "c", Request("host", "worker", UniValue{UniValue::VARR}, "rc"));
        BOOST_CHECK_EQUAL(wait(n + "c"), 0U);
        bridge.Handle(n + "d", Request("host", "long"));
        BOOST_CHECK_EQUAL(wait(n + "d"), 0U);
        bridge.Handle(n + "e", Request("host", "throws"));
        BOOST_CHECK_EQUAL(wait(n + "e"), 0U);
        bridge.Handle(n + "f", Request("host", "missing"));
        BOOST_CHECK_EQUAL(wait(n + "f"), 0U);
        bridge.Handle(n + "g", Request("host", "ui"));
        ui.RunAll();
        BOOST_CHECK_EQUAL(wait(n + "g"), 0U);
    }
    BOOST_CHECK_EQUAL(bridge.Pending(), 0U);
}

BOOST_AUTO_TEST_CASE(duplicate_rid)
{
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 1};
    Gate gate;
    bridge.SetRpc([&](const std::string&, const UniValue&, const std::string&) -> UniValue {
        gate.Wait();
        return UniValue{true};
    });
    bridge.Handle("1", Request("rpc", "x", UniValue{UniValue::VARR}, "same"));
    gate.UntilWaiting(1);
    bridge.Handle("2", Request("rpc", "x", UniValue{UniValue::VARR}, "same"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("2")), webui::BRIDGE_INVALID_REQUEST);
    gate.Open();
    BOOST_CHECK_EQUAL(replies.Wait("1").status, 0);
}

BOOST_AUTO_TEST_CASE(stop)
{
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 1};
    Gate gate;
    bridge.SetRpc([&](const std::string& method, const UniValue&, const std::string&) -> UniValue {
        if (method == "slow") gate.Wait();
        return UniValue{method};
    });
    bridge.Handle("1", Request("rpc", "slow", UniValue{UniValue::VARR}, "r1"));
    gate.UntilWaiting(1);
    bridge.Handle("2", Request("rpc", "queued", UniValue{UniValue::VARR}, "r2"));

    // Stop waits for the call that runs; the one that waits is answered that the app stops.
    auto stopped{std::async(std::launch::async, [&] { bridge.Stop(); })};
    std::this_thread::sleep_for(50ms);
    BOOST_CHECK(stopped.wait_for(0ms) == std::future_status::timeout);
    gate.Open();
    stopped.get();
    BOOST_CHECK_EQUAL(replies.Wait("1").value.get_str(), "slow");
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("2")), webui::BRIDGE_STOPPED);

    // After Stop, nothing runs.
    bridge.Handle("3", Request("rpc", "late"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("3")), webui::BRIDGE_STOPPED);
    bridge.Stop(); // twice is fine
}

BOOST_AUTO_TEST_CASE(many_calls_answered_once)
{
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 4};
    bridge.SetRpc([](const std::string& method, const UniValue& params, const std::string&) -> UniValue { return params; });
    constexpr int N{300};
    std::vector<std::thread> callers;
    for (int t{0}; t < 3; ++t) {
        callers.emplace_back([&, t] {
            for (int i{t}; i < N; i += 3) {
                UniValue params{UniValue::VARR};
                params.push_back(i);
                bridge.Handle(std::to_string(i), Request("rpc", "echo", params, "rid" + std::to_string(i)));
                if (i % 7 == 0) bridge.Handle("cancel" + std::to_string(i), R"([{"kind":"cancel","rid":"rid)" + std::to_string(i) + R"("}])");
            }
        });
    }
    for (auto& c : callers) c.join();
    for (int i{0}; i < N; ++i) {
        const auto reply{replies.Wait(std::to_string(i))};
        if (reply.status == 0) {
            BOOST_CHECK_EQUAL(reply.value[0].getInt<int>(), i);
        } else {
            // Only the cancelled ones may have been cancelled.
            BOOST_CHECK_EQUAL(i % 7, 0);
            BOOST_CHECK_EQUAL(ErrorCode(reply), webui::BRIDGE_CANCELLED);
        }
    }
    bridge.Stop();
    for (int i{0}; i < N; ++i) BOOST_CHECK_EQUAL(replies.Count(std::to_string(i)), 1U);
    BOOST_CHECK_EQUAL(bridge.Pending(), 0U);
}

BOOST_FIXTURE_TEST_CASE(real_node_rpc, TestChain100Setup)
{
    // The bridge in front of the in-process dispatcher of a real node, as chains-app sets it up.
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto node{interfaces::MakeNode(m_node)};
    Replies replies;
    UiQueue ui;
    webui::Bridge bridge{replies.Fn(), ui.Fn(), 2};
    bridge.SetRpc([&](const std::string& method, const UniValue& params, const std::string& uri) {
        return node->executeRpc(method, params, uri);
    });

    bridge.Handle("count", Request("rpc", "getblockcount"));
    auto count{replies.Wait("count")};
    BOOST_CHECK_EQUAL(count.status, 0);
    BOOST_CHECK_EQUAL(count.value.getInt<int>(), 100);

    UniValue params{UniValue::VARR};
    params.push_back(100);
    bridge.Handle("hash", Request("rpc", "getblockhash", params));
    BOOST_CHECK_EQUAL(replies.Wait("hash").value.get_str(), WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash().GetHex()));

    bridge.Handle("info", Request("rpc", "getblockchaininfo"));
    BOOST_CHECK_EQUAL(replies.Wait("info").value.find_value("chain").get_str(), "regtest");

    bridge.Handle("unknown", Request("rpc", "no_such_method"));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("unknown")), RPC_METHOD_NOT_FOUND);

    UniValue bad{UniValue::VARR};
    bad.push_back("not a number");
    bridge.Handle("bad", Request("rpc", "getblockhash", bad));
    BOOST_CHECK_EQUAL(ErrorCode(replies.Wait("bad")), RPC_TYPE_ERROR);
}

BOOST_AUTO_TEST_SUITE_END()
