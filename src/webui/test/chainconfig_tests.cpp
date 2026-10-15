// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/chainconfig.h>
#include <webui/guard.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <policy/feerate.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_AUTO_TEST_SUITE(webui_chainconfig_tests)

// What chain.json says of the chain is what the node is: its name, its programs, its unit, and on
// each network its ports and its addresses.
BOOST_AUTO_TEST_CASE(chain_json_matches_the_node)
{
    BOOST_CHECK_EQUAL(std::string{webui::chain::NAME}, CLIENT_NAME);
    BOOST_CHECK_EQUAL(std::string{webui::chain::STEM}, CLIENT_BIN_NAME);
    BOOST_CHECK_EQUAL(std::string{webui::chain::APP}, std::string{CLIENT_BIN_NAME} + "-app");
    BOOST_CHECK_EQUAL(std::string{webui::chain::TICKER}, CURRENCY_UNIT);
    BOOST_CHECK(!webui::chain::URI_SCHEME.empty());
    ArgsManager args;
    // On regtest a node is a sidechain when given its slot, as the functional tests do.
    args.ForceSetArg("-sidechainslot", "1");
    for (const auto& network : webui::chain::NETWORKS) {
        BOOST_TEST_CONTEXT(network.id)
        {
            const auto chain{ChainTypeFromString(network.id == "test" ? "test" : std::string{network.id})};
            BOOST_REQUIRE(chain);
            const auto params{CreateChainParams(args, *chain)};
            BOOST_CHECK_EQUAL(params->GetDefaultPort(), network.p2p_port);
            BOOST_CHECK_EQUAL(params->Bech32HRP(), std::string{network.hrp});
            BOOST_CHECK_EQUAL(CreateBaseChainParams(*chain)->RPCPort(), network.rpc_port);
#if WEBUI_SIDECHAIN
            BOOST_CHECK(params->GetConsensus().sidechain.enabled);
            BOOST_CHECK_GT(network.mainchain_rpc_port, 0);
#endif
        }
    }
}

// The help links of chain.json open without asking; any other web address the page names waits for the
// desktop (audit port-1 #3).
BOOST_AUTO_TEST_CASE(own_links_open_without_asking)
{
    BOOST_CHECK_EQUAL(webui::guard::KnownLinks().size(), webui::chain::LINKS.size());
    for (const std::string_view link : webui::chain::LINKS) {
        BOOST_CHECK(link.starts_with("https://"));
        BOOST_CHECK(!webui::guard::OpenUrlConfirmation(link));
        // The same address with something added is not the link.
        BOOST_CHECK(webui::guard::OpenUrlConfirmation(std::string{link} + "?x"));
    }
    BOOST_CHECK(webui::guard::OpenUrlConfirmation("https://explorer.example/tx/1"));
}

BOOST_AUTO_TEST_SUITE_END()
