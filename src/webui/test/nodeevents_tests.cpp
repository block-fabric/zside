// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <webui/events.h>
#include <webui/nodeevents.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
/** A hub that delivers at once, on the thread that emits. */
struct DirectHub {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<UniValue> events;
    webui::EventHub hub{
        [this](const std::string& json) {
            UniValue batch;
            BOOST_REQUIRE(batch.read(json));
            {
                std::lock_guard lock{mutex};
                for (const UniValue& e : batch.getValues()) events.push_back(e);
            }
            cv.notify_all();
        },
        [this](std::chrono::milliseconds, std::function<void()> fn) { fn(); },
        0ms};

    /** The last event of a type, waiting for one that matches. */
    UniValue WaitFor(const std::string& type, std::function<bool(const UniValue&)> match)
    {
        std::unique_lock lock{mutex};
        UniValue found;
        cv.wait_for(lock, 10s, [&] {
            for (const UniValue& e : events) {
                if (e["type"].get_str() == type && match(e["data"])) {
                    found = e["data"];
                    return true;
                }
            }
            return false;
        });
        return found;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_nodeevents_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(sync_state_names)
{
    BOOST_CHECK_EQUAL(webui::SyncStateName(SynchronizationState::INIT_REINDEX), "reindex");
    BOOST_CHECK_EQUAL(webui::SyncStateName(SynchronizationState::INIT_DOWNLOAD), "download");
    BOOST_CHECK_EQUAL(webui::SyncStateName(SynchronizationState::POST_INIT), "synced");
}

BOOST_AUTO_TEST_CASE(tip_and_snapshot)
{
    auto node{interfaces::MakeNode(m_node)};
    DirectHub direct;
    direct.hub.SetReady(true);
    webui::NodeEvents events{*node, direct.hub};
    events.SubscribeNode();

    const UniValue snapshot{events.Snapshot()};
    BOOST_CHECK_EQUAL(snapshot["tip"]["height"].getInt<int>(), 100);
    BOOST_CHECK_EQUAL(snapshot["connections"].getInt<int>(), 0);
    BOOST_CHECK(snapshot["networkActive"].isBool());
    BOOST_CHECK(snapshot["warnings"].isArray());
    // This test node has no wallets.
    BOOST_CHECK_EQUAL(snapshot["wallets"].size(), 0U);
    BOOST_CHECK(events.Wallets().empty());

    // A new block is an event, with the new tip.
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    const UniValue tip{direct.WaitFor("tip", [](const UniValue& d) { return d["height"].getInt<int>() == 101; })};
    BOOST_REQUIRE(tip.isObject());
    BOOST_CHECK_EQUAL(tip["hash"].get_str(), WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash().GetHex()));
    BOOST_CHECK(tip["state"].isStr());
    BOOST_CHECK(tip["progress"].isNum());

    // Network activity, as the snapshot gives it (the connection manager of the tests sends no
    // notifications: it has no user interface).
    node->setNetworkActive(false);
    BOOST_CHECK(!events.Snapshot()["networkActive"].get_bool());
    node->setNetworkActive(true);
    BOOST_CHECK(events.Snapshot()["networkActive"].get_bool());

    events.Stop();
    events.Stop();
    // No more events after Stop.
    size_t before;
    {
        std::lock_guard lock{direct.mutex};
        before = direct.events.size();
    }
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    std::this_thread::sleep_for(100ms);
    std::lock_guard lock{direct.mutex};
    BOOST_CHECK_EQUAL(direct.events.size(), before);
}

BOOST_AUTO_TEST_SUITE_END()
