// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// A sidechain node keeps a good history: -prune below MIN_SIDECHAIN_PRUNE_TARGET refuses to start,
// manual pruning leaves that much, and pruning keeps every block a reorg can take back.

#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <node/blockmanager_args.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <test/util/setup_common.h>
#include <util/byte_units.h>
#include <util/chaintype.h>
#include <util/check.h>
#include <util/result.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <set>
#include <string>
#include <vector>

using node::BlockManager;
using node::KernelNotifications;

namespace {
/** What -prune gives on a network: the prune target, or the error. */
struct PruneArg {
    BasicTestingSetup& setup;
    util::Result<uint64_t> Apply(ChainType chain, const std::string& prune, const std::vector<std::string>& more = {})
    {
        ArgsManager args;
        args.ForceSetArg("-prune", prune);
        for (const auto& arg : more) args.ForceSetArg(arg.substr(0, arg.find('=')), arg.substr(arg.find('=') + 1));
        const auto params{CreateChainParams(args, chain)};
        KernelNotifications notifications{Assert(setup.m_node.shutdown_request), setup.m_node.exit_status, *Assert(setup.m_node.warnings)};
        BlockManager::Options opts{
            .chainparams = *params,
            .blocks_dir = setup.m_args.GetBlocksDirPath(),
            .notifications = notifications,
            .block_tree_db_params = DBParams{.path = setup.m_args.GetDataDirNet() / "blocks" / "index", .cache_bytes = 0},
        };
        if (auto applied{node::ApplyArgsManOptions(args, opts)}; !applied) return util::Error{util::ErrorString(applied)};
        return opts.prune_target;
    }
};

struct SidechainPruneSetup : public TestingSetup {
    SidechainPruneSetup() : TestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=3", "-prune=1", "-fastprune"}}} {}
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_prune_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sidechain_prune_minimum)
{
    PruneArg prune{*this};
    BOOST_CHECK_EQUAL(MIN_SIDECHAIN_PRUNE_TARGET, 10000_MiB);
    // A sidechain: every network of this chain, and regtest with a slot.
    for (const auto& [chain, more] : std::vector<std::pair<ChainType, std::vector<std::string>>>{
             {ChainType::MAIN, {}}, {ChainType::TESTNET, {}}, {ChainType::SIGNET, {}}, {ChainType::REGTEST, {"-sidechainslot=3"}}}) {
        BOOST_TEST_MESSAGE("network " << ChainTypeToString(chain));
        // Below the minimum: refused, with the reason.
        for (const std::string value : {"2", "550", "4999", "5000", "9999"}) {
            const auto refused{prune.Apply(chain, value, more)};
            BOOST_REQUIRE(!refused);
            BOOST_CHECK(util::ErrorString(refused).original.find("-prune must be at least 10000 MiB on a sidechain") != std::string::npos);
        }
        // The minimum and above: taken.
        BOOST_CHECK_EQUAL(*Assert(prune.Apply(chain, "10000", more)), 10000_MiB);
        BOOST_CHECK_EQUAL(*Assert(prune.Apply(chain, "20000", more)), 20000_MiB);
        // Off, and manual pruning (which keeps as much: below).
        BOOST_CHECK_EQUAL(*Assert(prune.Apply(chain, "0", more)), 0U);
        BOOST_CHECK_EQUAL(*Assert(prune.Apply(chain, "1", more)), BlockManager::PRUNE_TARGET_MANUAL);
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        if (more.empty()) {
            BOOST_CHECK_EQUAL(MinPruneTarget(params->GetConsensus()), MIN_SIDECHAIN_PRUNE_TARGET);
            BOOST_CHECK_EQUAL(MinBlocksToKeep(params->GetConsensus()), DRIVECHAIN_UNDO_DEPTH);
        }
    }
    // Not a sidechain (regtest without a slot, where the tests inherited from Bitcoin run): Bitcoin's.
    BOOST_CHECK_EQUAL(*Assert(prune.Apply(ChainType::REGTEST, "550")), 550_MiB);
    BOOST_CHECK(!prune.Apply(ChainType::REGTEST, "549"));
    const auto regtest{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    BOOST_CHECK_EQUAL(MinPruneTarget(regtest->GetConsensus()), MIN_DISK_SPACE_FOR_BLOCK_FILES);
    BOOST_CHECK_EQUAL(MinBlocksToKeep(regtest->GetConsensus()), static_cast<int>(MIN_BLOCKS_TO_KEEP));
}

BOOST_FIXTURE_TEST_CASE(sidechain_prune_keeps_history, SidechainPruneSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BlockManager& blockman{chainman.m_blockman};
    BOOST_REQUIRE(chainman.GetConsensus().sidechain.enabled);
    BOOST_REQUIRE(blockman.IsPruneMode());

    LOCK(::cs_main);
    // Block files (small, -fastprune) holding the heights 1 to `written`, made 1000 MiB each.
    const CBlock& block{chainman.GetParams().GenesisBlock()};
    int written{0};
    int files{0};
    while (files < 22) files = blockman.WriteBlock(block, ++written).nFile;
    for (int n{0}; n < files; ++n) {
        blockman.GetBlockFileInfo(n)->nSize = 1000_MiB;
        blockman.GetBlockFileInfo(n)->nUndoSize = 0;
    }
    const uint64_t current{blockman.GetBlockFileInfo(files)->nSize + uint64_t{blockman.GetBlockFileInfo(files)->nUndoSize}};

    // A chain whose tip is far enough above the files that every one of them may be pruned by height.
    std::vector<CBlockIndex> fake(written + DRIVECHAIN_UNDO_DEPTH + 100);
    for (size_t h{0}; h < fake.size(); ++h) {
        fake[h].nHeight = h;
        fake[h].pprev = h ? &fake[h - 1] : nullptr;
        fake[h].BuildSkip();
    }
    CBlockIndex* const genesis{chainstate.m_chain.Genesis()};
    chainstate.m_chain.SetTip(fake.back());
    const int tip{chainstate.m_chain.Height()};

    // Pruning keeps the blocks of the last DRIVECHAIN_UNDO_DEPTH heights, which a reorg can take back.
    BOOST_CHECK_EQUAL(chainstate.GetPruneRange(tip).second, tip - DRIVECHAIN_UNDO_DEPTH);
    BOOST_CHECK_EQUAL(chainstate.GetPruneRange(written).second, written);

    // Asked to prune everything: the oldest files go, as long as 10000 MiB are left.
    std::set<int> pruned;
    blockman.FindFilesToPruneManual(pruned, written, chainstate);
    const uint64_t left{blockman.CalculateCurrentUsage()};
    BOOST_CHECK_GE(left, MIN_SIDECHAIN_PRUNE_TARGET);
    BOOST_CHECK_LT(left, MIN_SIDECHAIN_PRUNE_TARGET + 1000_MiB);
    BOOST_CHECK_EQUAL(left, (files - pruned.size()) * 1000_MiB + current);
    // The oldest ones.
    BOOST_CHECK_EQUAL(pruned.size(), static_cast<size_t>(files - 10));
    for (int n{0}; n < static_cast<int>(pruned.size()); ++n) BOOST_CHECK(pruned.contains(n));
    // Asked again: nothing more goes.
    std::set<int> again;
    blockman.FindFilesToPruneManual(again, written, chainstate);
    BOOST_CHECK(again.empty());
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), left);

    chainstate.m_chain.SetTip(*genesis);
}

BOOST_AUTO_TEST_SUITE_END()
