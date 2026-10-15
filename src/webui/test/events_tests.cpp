// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/events.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
/** A hub whose deliveries and scheduled flushes the test sees and runs. */
struct Harness {
    std::vector<UniValue> batches;
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> scheduled;
    webui::EventHub hub{
        [this](const std::string& json) {
            UniValue batch;
            BOOST_REQUIRE(batch.read(json));
            batches.push_back(batch);
        },
        [this](std::chrono::milliseconds delay, std::function<void()> fn) { scheduled.emplace_back(delay, std::move(fn)); },
        250ms};

    void RunScheduled()
    {
        auto todo{std::move(scheduled)};
        scheduled.clear();
        for (auto& [delay, fn] : todo) fn();
    }

    static UniValue Data(int n)
    {
        UniValue data{UniValue::VOBJ};
        data.pushKV("n", n);
        return data;
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(webui_events_tests)

BOOST_AUTO_TEST_CASE(kept_until_ready)
{
    Harness h;
    h.hub.Emit("a", Harness::Data(1));
    h.hub.Emit("b", Harness::Data(2));
    BOOST_CHECK(h.scheduled.empty());
    BOOST_CHECK_EQUAL(h.hub.Waiting(), 2U);
    h.hub.SetReady(true);
    BOOST_REQUIRE_EQUAL(h.scheduled.size(), 1U);
    BOOST_CHECK(h.scheduled[0].first == 0ms);
    h.RunScheduled();
    BOOST_REQUIRE_EQUAL(h.batches.size(), 1U);
    const UniValue& batch{h.batches[0]};
    BOOST_REQUIRE_EQUAL(batch.size(), 2U);
    BOOST_CHECK_EQUAL(batch[0]["type"].get_str(), "a");
    BOOST_CHECK_EQUAL(batch[0]["data"]["n"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(batch[1]["type"].get_str(), "b");
    BOOST_CHECK_EQUAL(h.hub.Waiting(), 0U);
}

BOOST_AUTO_TEST_CASE(immediate_and_throttled)
{
    Harness h;
    h.hub.SetReady(true);
    // A coalesced event waits for the throttle.
    h.hub.Emit("tip", Harness::Data(1), "tip");
    BOOST_REQUIRE_EQUAL(h.scheduled.size(), 1U);
    BOOST_CHECK(h.scheduled[0].first == 250ms);
    // More of them replace it and schedule nothing more.
    for (int i{2}; i <= 1000; ++i) h.hub.Emit("tip", Harness::Data(i), "tip");
    BOOST_CHECK_EQUAL(h.scheduled.size(), 1U);
    BOOST_CHECK_EQUAL(h.hub.Waiting(), 1U);
    // Another kind is kept apart.
    h.hub.Emit("headerTip", Harness::Data(7), "headerTip");
    BOOST_CHECK_EQUAL(h.hub.Waiting(), 2U);
    // An event that is not coalesced needs an earlier flush.
    h.hub.Emit("wallet.tx", Harness::Data(9));
    BOOST_REQUIRE_EQUAL(h.scheduled.size(), 2U);
    BOOST_CHECK(h.scheduled[1].first == 0ms);
    h.RunScheduled();
    // Everything went in the first flush, in order, with the last tip only.
    BOOST_REQUIRE_EQUAL(h.batches.size(), 1U);
    const UniValue& batch{h.batches[0]};
    BOOST_REQUIRE_EQUAL(batch.size(), 3U);
    BOOST_CHECK_EQUAL(batch[0]["type"].get_str(), "tip");
    BOOST_CHECK_EQUAL(batch[0]["data"]["n"].getInt<int>(), 1000);
    BOOST_CHECK_EQUAL(batch[1]["type"].get_str(), "headerTip");
    BOOST_CHECK_EQUAL(batch[2]["type"].get_str(), "wallet.tx");
}

BOOST_AUTO_TEST_CASE(not_coalesced_events_all_arrive)
{
    Harness h;
    h.hub.SetReady(true);
    for (int i{0}; i < 50; ++i) h.hub.Emit("wallet.tx", Harness::Data(i));
    h.RunScheduled();
    size_t total{0};
    for (const auto& b : h.batches) total += b.size();
    BOOST_CHECK_EQUAL(total, 50U);
}

BOOST_AUTO_TEST_CASE(kept_events_are_bounded)
{
    Harness h;
    for (size_t i{0}; i < webui::EventHub::MAX_KEPT + 10; ++i) h.hub.Emit("x", Harness::Data(int(i)));
    BOOST_CHECK_EQUAL(h.hub.Waiting(), webui::EventHub::MAX_KEPT);
    h.hub.SetReady(true);
    h.RunScheduled();
    BOOST_REQUIRE_EQUAL(h.batches.size(), 1U);
    // The oldest ones went.
    BOOST_CHECK_EQUAL(h.batches[0][0]["data"]["n"].getInt<int>(), 10);
}

BOOST_AUTO_TEST_CASE(not_ready_again)
{
    Harness h;
    h.hub.SetReady(true);
    h.hub.SetReady(false);
    h.hub.Emit("a", Harness::Data(1));
    BOOST_CHECK(h.scheduled.empty());
    h.hub.Flush();
    BOOST_CHECK(h.batches.empty());
}

BOOST_AUTO_TEST_SUITE_END()
