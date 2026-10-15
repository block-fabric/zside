// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/rpcpolicy.h>
#include <webui/sidechainapi.h>

#include <chainparams.h>
#include <drivechain/sidechain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>

namespace {
//! A node that is never reached: what the page gives is refused before the node or the mainchain is asked.
interfaces::Node& NoNode()
{
    throw webui::HostError("no node in this test");
}

int CodeOf(const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const webui::HostError& e) {
        return e.code;
    }
    return 0;
}

UniValue Obj(std::initializer_list<std::pair<std::string, UniValue>> fields)
{
    UniValue out{UniValue::VOBJ};
    for (const auto& [k, v] : fields) out.pushKV(k, v);
    return out;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_sidechainapi_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainchain_methods_read_only)
{
    // What the pages may ask of the mainchain node: reading, and its wallet's balances and addresses. Never a
    // method that spends (the deposit has peg.deposit, which checks what it pays to), nor one that votes.
    const auto& methods{webui::SidechainApi::MainchainMethods()};
    for (const char* m : {"getdrivechaininfo", "listwithdrawalbundles", "getsidechainevents", "listsidechaindeposits", "getbalances"}) BOOST_CHECK(methods.contains(m));
    for (const char* m : {"createsidechaindeposit", "sendtoaddress", "send", "createbmmrequest", "setwithdrawalvote", "acksidechain", "stop", "walletpassphrase", "dumpprivkey"}) {
        BOOST_CHECK_MESSAGE(!methods.contains(m), m);
    }
    for (const auto& m : webui::SidechainApi::MainchainWalletMethods()) BOOST_CHECK(methods.contains(m));
}

BOOST_AUTO_TEST_CASE(page_input_refused_before_the_node)
{
    webui::SidechainApi api{NoNode, [](const webui::Window::ConfirmDialog&) { return true; }};
    // A method not on the list.
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.call", Obj({{"method", "sendtoaddress"}, {"params", UniValue{UniValue::VARR}}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.call", Obj({{"method", "getblockcount"}, {"params", "x"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.nothing", UniValue{UniValue::VOBJ}); }), webui::BRIDGE_METHOD_NOT_FOUND);

    // The deposit helper pays only to a deposit address of this sidechain, with a valid address of this chain in it.
    const uint32_t slot{Params().GetConsensus().sidechain.slot};
    const std::string own{EncodeDestination(WitnessV0KeyHash{uint160{}})};
    const std::string good{drivechain::FormatDepositAddress({slot, own})};
    const std::string other_slot{drivechain::FormatDepositAddress({slot + 1, own})};
    const std::string not_an_address{drivechain::FormatDepositAddress({slot, "hello"})};
    std::string typo{good};
    typo.back() = typo.back() == 'a' ? 'b' : 'a';
    for (const std::string& bad : {std::string{"hello"}, typo, other_slot, not_an_address}) {
        BOOST_CHECK_MESSAGE(CodeOf([&] { api.Call("peg.deposit", Obj({{"destination", bad}, {"amount", "1"}})); }) == webui::BRIDGE_INVALID_REQUEST, bad);
    }
    for (const char* amount : {"0", "-1", "abc", ""}) {
        BOOST_CHECK_MESSAGE(CodeOf([&] { api.Call("peg.deposit", Obj({{"destination", good}, {"amount", amount}})); }) == webui::BRIDGE_INVALID_REQUEST, amount);
    }
    // A good request goes on to the node (here: none).
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.deposit", Obj({{"destination", good}, {"amount", "1.5"}})); }), webui::BRIDGE_HOST_ERROR);
}

BOOST_AUTO_TEST_CASE(mainchain_wallet_spends_ask_the_desktop)
{
    // Audit port-1 #4: what spends the wallet of the mainchain node is checked by the host, then confirmed on the
    // desktop; the offers of merged mining are capped.
    std::vector<webui::Window::ConfirmDialog> asked;
    bool answer{false};
    int reached{0};
    const auto node{[&]() -> interfaces::Node& {
        ++reached;
        throw webui::HostError("no node in this test");
    }};
    webui::SidechainApi api{node, [&](const webui::Window::ConfirmDialog& d) {
        asked.push_back(d);
        return answer;
    }};
    const std::string own{EncodeDestination(WitnessV0KeyHash{uint160{}})};
    // Refused before any dialog: no address of this chain, no offer, an offer over the cap.
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", true}, {"address", "nope"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", "yes"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", true}, {"address", own}, {"always", true}, {"amount", "0"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", true}, {"address", own}, {"always", true}, {"amount", "0.01000001"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.request", Obj({{"address", own}, {"amount", "5"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK(asked.empty());
    BOOST_CHECK_EQUAL(reached, 0);
    // Declined on the desktop: the node is not reached.
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", true}, {"address", own}, {"always", true}, {"amount", "0.0001"}})); }), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK(asked[0].message.starts_with("Offer: 0.0001 coins of "));
    BOOST_CHECK(asked[0].message.find("every block") != std::string::npos);
    BOOST_CHECK(asked[0].message.ends_with(own));
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", true}, {"address", own}})); }), webui::BRIDGE_CANCELLED);
    BOOST_CHECK(asked[1].message.starts_with("Offer: 99% of the fees"));
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.request", Obj({{"address", own}, {"amount", "0.01"}})); }), webui::BRIDGE_CANCELLED);
    BOOST_CHECK(asked[2].message.starts_with("Offer: 0.01 coins of "));
    BOOST_CHECK_EQUAL(reached, 0);
    // Accepted, the node is asked; turning mining off needs no word.
    answer = true;
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.request", Obj({{"address", own}, {"amount", "0.01"}})); }), webui::BRIDGE_HOST_ERROR);
    BOOST_CHECK_EQUAL(reached, 1);
    asked.clear();
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("bmm.set", Obj({{"mine", false}})); }), webui::BRIDGE_HOST_ERROR);
    BOOST_CHECK(asked.empty());
    BOOST_CHECK_EQUAL(webui::SidechainApi::MAX_BMM_OFFER, 1'000'000);
}

BOOST_AUTO_TEST_CASE(page_rpc_policy_of_the_chain)
{
#if WEBUI_SIDECHAIN
    // A sidechain: its peg and merged mining; none of the drivechain methods of the mainchain, no solo mining.
    for (const char* m : {"getmainchaininfo", "getbmminfo", "listwithdrawals", "getwithdrawalbundle", "getdepositaddress", "createwithdrawal", "refundwithdrawal"}) {
        BOOST_CHECK_MESSAGE(webui::PageRpcPolicy(m).has_value(), m);
    }
    // What spends the wallet of the mainchain node is a host method, confirmed on the desktop (bmm.set, bmm.request).
    for (const char* m : {"acksidechain", "createsidechainproposal", "setwithdrawalvote", "createsidechaindeposit", "setgenerate", "getgenerate", "setbmm", "requestbmmblock"}) {
        BOOST_CHECK_MESSAGE(!webui::PageRpcPolicy(m).has_value(), m);
    }
    BOOST_CHECK(webui::KeyedWalletRpc("createwithdrawal"));
    BOOST_CHECK(webui::KeyedWalletRpc("refundwithdrawal"));
    BOOST_CHECK(!webui::KeyedWalletRpc("createsidechaindeposit"));
#else
    BOOST_CHECK(webui::PageRpcPolicy("acksidechain").has_value());
    BOOST_CHECK(!webui::PageRpcPolicy("setbmm").has_value());
#endif
}

BOOST_AUTO_TEST_SUITE_END()
