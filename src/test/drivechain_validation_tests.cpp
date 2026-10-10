// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The drivechain parts of validation, and the node around them, where the unit tests of the
// sidechain database alone (drivechain_tests) do not reach: failures while blocks are connected
// or disconnected, the ways the sidechain database is brought back at startup, and the miner.

#include <base58.h>
#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <drivechain/db.h>
#include <drivechain/miner.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <key_io.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <node/cpuminer.h>
#include <node/utxo_snapshot.h>
#include <test/util/chainstate.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/result.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace drivechain;
using namespace std::chrono_literals;

namespace {

/** Chainstate::LoadDrivechainState, with its error in `error` (empty when it succeeded). */
bool LoadState(Chainstate& chainstate, bilingual_str& error) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const auto loaded{chainstate.LoadDrivechainState()};
    error = loaded ? bilingual_str{} : util::ErrorString(loaded);
    return bool{loaded};
}

/** A regtest chain of 100 blocks, with what the drivechain tests need on top. */
struct DrivechainChainSetup : public TestChain100Setup {
    const CScript coinbase_script{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};

    Chainstate& ActiveChainstate() { return m_node.chainman->ActiveChainstate(); }
    Database& DrivechainDB() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return *m_node.chainman->m_blockman.m_drivechain_db; }

    void Mine(int count = 1)
    {
        for (int i{0}; i < count; ++i) CreateAndProcessBlock({}, coinbase_script);
    }

    /** Activate a sidechain in `slot`: this node's miner proposes it and acks it until it activates. */
    void ActivateSidechain(SidechainId slot)
    {
        Sidechain sidechain;
        sidechain.slot = slot;
        sidechain.title = "test";
        sidechain.hash_id1 = uint256{1};
        m_node.chainman->m_drivechain_miner.AddProposal(sidechain);
        Mine(m_node.chainman->GetConsensus().drivechain.activation_period);
        BOOST_REQUIRE(WITH_LOCK(::cs_main, return ActiveChainstate().m_scdb.IsActive(slot)));
    }

    /** The first deposit to the sidechain in `slot`, paid by the coinbase of block `height` (from the first 100). */
    CMutableTransaction FirstDeposit(SidechainId slot, CAmount amount, int height)
    {
        const CTransactionRef& input{m_coinbase_txns.at(height - 1)};
        const std::vector<CTxOut> outputs{
            {amount, EscrowScript(slot)},
            {0, DestinationScript("dest")},
            {input->vout[0].nValue - amount - 10'000, coinbase_script},
        };
        return CreateValidTransaction({input}, {COutPoint{input->GetHash(), 0}}, height, {coinbaseKey}, outputs, std::nullopt, std::nullopt).first;
    }

    /** Spend the output of the tip's coinbase that is in the UTXO set: the coins no longer match the blocks. */
    std::pair<COutPoint, Coin> BreakTipCoins() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        Chainstate& chainstate{ActiveChainstate()};
        CBlock block;
        BOOST_REQUIRE(m_node.chainman->m_blockman.ReadBlock(block, *chainstate.m_chain.Tip()));
        const COutPoint out{block.vtx[0]->GetHash(), 0};
        Coin coin;
        BOOST_REQUIRE(chainstate.CoinsTip().SpendCoin(out, &coin));
        return {out, std::move(coin)};
    }

    void RepairCoins(std::pair<COutPoint, Coin>&& broken) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        ActiveChainstate().CoinsTip().AddCoin(broken.first, std::move(broken.second), /*possible_overwrite=*/false);
    }

    /** Wait for `done`, for ten seconds at most. */
    static bool WaitFor(const std::function<bool()>& done)
    {
        for (int i{0}; i < 1000; ++i) {
            if (done()) return true;
            UninterruptibleSleep(10ms);
        }
        return done();
    }
};

/** A log line containing `message` is a failure. */
DebugLogHelper Unexpected(const std::string& message)
{
    return DebugLogHelper{message, [](const std::string* line) {
                              BOOST_CHECK_MESSAGE(!line, "unexpected log line: " + (line ? *line : std::string{}));
                              return false;
                          }};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_validation_tests, DrivechainChainSetup)

BOOST_AUTO_TEST_CASE(failed_disconnect_keeps_the_sidechain_database)
{
    // When a block cannot be disconnected cleanly (the coins do not match it), the coins stay at the
    // block: so does the sidechain database, which is taken back in place and so is connected again.
    ActivateSidechain(0);
    CreateAndProcessBlock({FirstDeposit(0, COIN, 1)}, coinbase_script);
    LOCK(::cs_main);
    Chainstate& chainstate{ActiveChainstate()};
    CBlockIndex* tip{chainstate.m_chain.Tip()};
    const SidechainDB at_tip{chainstate.m_scdb};
    BOOST_REQUIRE(at_tip.GetSlot(0)->has_ctip);
    {
        auto broken{BreakTipCoins()};
        LOCK(m_node.mempool->cs);
        BlockValidationState state;
        {
            ASSERT_DEBUG_LOG("DisconnectTip(): DisconnectBlock " + tip->GetBlockHash().ToString() + " failed");
            BOOST_CHECK(!chainstate.DisconnectTip(state, nullptr));
        }
        BOOST_CHECK(!state.IsError());
        BOOST_CHECK(!m_interrupt);
        RepairCoins(std::move(broken));
    }
    BOOST_CHECK(chainstate.m_chain.Tip() == tip);
    BOOST_CHECK(chainstate.m_scdb == at_tip);

    // Undo data that takes the database somewhere the block does not connect to: the node cannot go on.
    BlockUndo undo;
    BOOST_REQUIRE(DrivechainDB().ReadBlockUndo(tip->GetBlockHash(), undo));
    BlockUndo wrong{undo};
    BOOST_REQUIRE(!wrong.slots.empty());
    for (BlockUndo::SlotUndo& slot : wrong.slots) slot.existed = false;
    DrivechainDB().WriteBlock(tip->GetBlockHash(), tip->nHeight, wrong, {}, chainstate.m_scdb);
    {
        auto broken{BreakTipCoins()};
        LOCK(m_node.mempool->cs);
        BlockValidationState state;
        {
            ASSERT_DEBUG_LOG("the sidechain database cannot be brought back to " + tip->GetBlockHash().ToString() + " (bad-dc-inactive-sidechain)");
            BOOST_CHECK(!chainstate.DisconnectTip(state, nullptr));
        }
        BOOST_CHECK(state.IsError());
        BOOST_CHECK(m_interrupt);
        RepairCoins(std::move(broken));
    }
    BOOST_CHECK(chainstate.m_chain.Tip() == tip);
    // Taken back to where the wrong undo data put it.
    BOOST_CHECK(chainstate.m_scdb.GetBlockHash() == tip->pprev->GetBlockHash());
    BOOST_CHECK(!chainstate.m_scdb.IsActive(0));
    // Put right again for the end of the test.
    BOOST_REQUIRE(m_interrupt.reset());
    DrivechainDB().WriteBlock(tip->GetBlockHash(), tip->nHeight, undo, {}, at_tip);
    chainstate.m_scdb = at_tip;
}

BOOST_AUTO_TEST_CASE(interrupted_reorg_failure_is_not_fatal)
{
    // A block that cannot be disconnected during a reorg is a fatal error, unless the node is
    // shutting down (a deep reorg derives the sidechain database, which a shutdown interrupts).
    Chainstate& chainstate{ActiveChainstate()};
    Mine();
    CBlockIndex* own{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, own));
    // (Paying elsewhere: the same coinbase at the same time would make the same block.)
    const CBlock fork{CreateAndProcessBlock({}, CScript() << OP_TRUE)};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    CBlockIndex* fork_tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    CBlockIndex* fork_index{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(fork.GetHash()))};
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork_index));
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(own);
        m_node.chainman->RecalculateBestHeader();
    }
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()) == own);

    // The longer fork is valid again; the tip cannot be disconnected, and the node is shutting down.
    auto broken{WITH_LOCK(::cs_main, return BreakTipCoins())};
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(fork_index);
        m_node.chainman->RecalculateBestHeader();
    }
    BOOST_REQUIRE(m_interrupt());
    {
        BlockValidationState reorg;
        auto fatal{Unexpected("Failed to disconnect block")};
        BOOST_CHECK(!chainstate.ActivateBestChain(reorg));
        BOOST_CHECK(!reorg.IsError());
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()) == own);
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_SUCCESS);
    BOOST_REQUIRE(m_interrupt.reset());
    WITH_LOCK(::cs_main, RepairCoins(std::move(broken)));
    BlockValidationState reorg;
    BOOST_CHECK(chainstate.ActivateBestChain(reorg));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()) == fork_tip);
}

BOOST_AUTO_TEST_CASE(connect_block_needs_the_matching_sidechain_database)
{
    const CBlock block{CreateBlock({}, coinbase_script)};
    {
        BlockValidationState state;
        const std::vector<CBlockHeader> headers{static_cast<const CBlockHeader&>(block)};
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state));
    }
    LOCK(::cs_main);
    Chainstate& chainstate{ActiveChainstate()};
    CBlockIndex* pindex{m_node.chainman->m_blockman.LookupBlockIndex(block.GetHash())};
    BOOST_REQUIRE(pindex);

    // Checking a block against the caller's database of another block is the caller's error.
    {
        SidechainDB other;
        other.SetBlockHash(uint256{1});
        CCoinsViewCache view{&chainstate.CoinsTip()};
        BlockValidationState state;
        BOOST_CHECK(!chainstate.ConnectBlock(block, state, pindex, view, /*fJustCheck=*/true, &other));
        BOOST_CHECK(state.IsError());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "the sidechain database does not match the block being checked");
        BOOST_CHECK(other.GetBlockHash() == uint256{1});
        BOOST_CHECK(!m_interrupt);
    }
    // Connecting a block to a chainstate whose database belongs elsewhere is a fatal error.
    {
        const uint256 hash{chainstate.m_scdb.GetBlockHash()};
        chainstate.m_scdb.SetBlockHash(uint256{1});
        CCoinsViewCache view{&chainstate.CoinsTip()};
        BlockValidationState state;
        {
            ASSERT_DEBUG_LOG("The sidechain database does not match the block being connected.");
            BOOST_CHECK(!chainstate.ConnectBlock(block, state, pindex, view));
        }
        BOOST_CHECK(state.IsError());
        BOOST_CHECK(m_interrupt);
        BOOST_REQUIRE(m_interrupt.reset());
        chainstate.m_scdb.SetBlockHash(hash);
    }
    // With the matching one, the block checks, and the caller's database is left updated.
    {
        SidechainDB copy{chainstate.m_scdb};
        CCoinsViewCache view{&chainstate.CoinsTip()};
        BlockValidationState state;
        BOOST_CHECK(chainstate.ConnectBlock(block, state, pindex, view, /*fJustCheck=*/true, &copy));
        BOOST_CHECK(copy.GetBlockHash() == block.GetHash());
        BOOST_CHECK(chainstate.m_scdb.GetBlockHash() == chainstate.m_chain.Tip()->GetBlockHash());
    }
}

BOOST_AUTO_TEST_CASE(loading_the_sidechain_database)
{
    // At startup the snapshot of the sidechain database is brought to the chain tip: rewound by the
    // undo data from a block that left the chain, derived from the blocks without it, and, when
    // that is interrupted, taken up next time from where it stopped.
    Chainstate& chainstate{ActiveChainstate()};
    ActivateSidechain(0);
    Mine();
    CBlockIndex* left{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    const SidechainDB at_left{WITH_LOCK(::cs_main, return chainstate.m_scdb)};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, left));
    CreateAndProcessBlock({FirstDeposit(0, COIN, 1)}, coinbase_script);
    Mine();

    LOCK(::cs_main);
    const SidechainDB expected{chainstate.m_scdb};
    BOOST_REQUIRE(expected.GetSlot(0)->has_ctip);
    const int tip_height{chainstate.m_chain.Height()};
    const std::string name{chainstate.DrivechainStateName()};
    bilingual_str error;
    const auto load{[&](const SidechainDB& snapshot) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        DrivechainDB().WriteState(name, snapshot);
        chainstate.m_scdb = SidechainDB{};
        return LoadState(chainstate, error);
    }};

    {
        auto rebuilt{Unexpected("rebuilt from the blocks")};
        ASSERT_DEBUG_LOG(strprintf("Bringing the sidechain database from height %d to the chain tip at height %d", left->nHeight - 1, tip_height));
        BOOST_CHECK(load(at_left));
    }
    BOOST_CHECK(chainstate.m_scdb == expected);

    BOOST_REQUIRE(DrivechainDB().HasBlockUndo(left->GetBlockHash()));
    DrivechainDB().EraseBlockUndo(left->GetBlockHash());
    {
        ASSERT_DEBUG_LOG("No drivechain undo data for block " + left->GetBlockHash().ToString() + "; the sidechain database is rebuilt from the blocks");
        ASSERT_DEBUG_LOG(strprintf("Bringing the sidechain database from height 0 to the chain tip at height %d", tip_height));
        BOOST_CHECK(load(at_left));
    }
    BOOST_CHECK(chainstate.m_scdb == expected);

    // A snapshot of no block of the chain: derived from the blocks; a shutdown interrupts that.
    SidechainDB unknown;
    unknown.SetBlockHash(uint256{0x42});
    BOOST_REQUIRE(m_interrupt());
    {
        ASSERT_DEBUG_LOG("Interrupted while bringing the sidechain database to the chain tip");
        BOOST_CHECK(!load(unknown));
    }
    BOOST_REQUIRE(m_interrupt.reset());
    SidechainDB partial;
    BOOST_REQUIRE(DrivechainDB().ReadState(name, partial));
    const CBlockIndex* stopped{m_node.chainman->m_blockman.LookupBlockIndex(partial.GetBlockHash())};
    BOOST_REQUIRE(stopped);
    BOOST_CHECK(chainstate.m_chain.Contains(*stopped));
    BOOST_CHECK_LT(stopped->nHeight, tip_height);
    {
        ASSERT_DEBUG_LOG(strprintf("Bringing the sidechain database from height %d to the chain tip", stopped->nHeight));
        chainstate.m_scdb = SidechainDB{};
        BOOST_CHECK(LoadState(chainstate, error));
    }
    BOOST_CHECK(chainstate.m_scdb == expected);
    BOOST_CHECK(error.empty());

    // The snapshot of the tip itself is used as it is.
    {
        auto derived{Unexpected("Bringing the sidechain database")};
        BOOST_CHECK(load(expected));
    }
    BOOST_CHECK(chainstate.m_scdb == expected);
}

BOOST_AUTO_TEST_CASE(older_database_format_is_rebuilt)
{
    // A database an older version laid out (its version record is a number alone) is wiped and
    // derived anew from the blocks.
    ActivateSidechain(0);
    LOCK(::cs_main);
    Chainstate& chainstate{ActiveChainstate()};
    const SidechainDB expected{chainstate.m_scdb};
    const uint256 fingerprint{ParamsFingerprint(m_node.chainman->GetConsensus().drivechain)};
    const auto old_database{[&](const fs::path& path) {
        {
            CDBWrapper old{DBParams{.path = path, .cache_bytes = 1 << 20}};
            old.Write(uint8_t{'v'}, uint32_t{7});
            old.Write(std::make_pair(uint8_t{'s'}, std::string{}), uint32_t{1});
        }
        return std::make_unique<Database>(DBParams{.path = path, .cache_bytes = 1 << 20}, fingerprint);
    }};
    auto current{std::move(m_node.chainman->m_blockman.m_drivechain_db)};

    m_node.chainman->m_blockman.m_drivechain_db = old_database(m_path_root / "old_drivechain");
    BOOST_CHECK(DrivechainDB().CheckFormat() == Database::Format::OTHER_VERSION);
    bilingual_str error;
    {
        ASSERT_DEBUG_LOG("The sidechain database is in an older format; it is rebuilt from the blocks");
        BOOST_CHECK(LoadState(chainstate, error));
    }
    BOOST_CHECK(DrivechainDB().IsCurrentFormat());
    BOOST_CHECK(chainstate.m_scdb == expected);

    // Without a chainstate to rebuild from (-reindex-chainstate), it is wiped and marked current at once.
    m_node.chainman->m_blockman.m_drivechain_db = old_database(m_path_root / "old_drivechain_2");
    {
        ASSERT_DEBUG_LOG("The sidechain database is in an older format; it is rebuilt from the blocks");
        BOOST_CHECK(chainstate.PrepareDrivechainDB(/*rebuild_from=*/nullptr, error));
    }
    BOOST_CHECK(DrivechainDB().IsCurrentFormat());
    SidechainDB read;
    BOOST_CHECK(!DrivechainDB().ReadState("", read));

    m_node.chainman->m_blockman.m_drivechain_db = std::move(current);
}

BOOST_AUTO_TEST_CASE(rollback_from_an_invalid_block_needs_its_data)
{
    // An active block that breaks the drivechain rules of this version is taken back at startup:
    // not without its data, and not if its coins do not match. Nothing changes then.
    Mine();
    LOCK(::cs_main);
    Chainstate& chainstate{ActiveChainstate()};
    CBlockIndex* tip{chainstate.m_chain.Tip()};
    SidechainDB at_parent;
    at_parent.SetBlockHash(tip->pprev->GetBlockHash());
    bilingual_str error;

    tip->nStatus &= ~BLOCK_HAVE_UNDO;
    {
        ASSERT_DEBUG_LOG(strprintf("no block or undo data for block %s at height %d", tip->GetBlockHash().ToString(), tip->nHeight));
        BOOST_CHECK(!chainstate.RollBackFromInvalidBlock(tip, SidechainDB{at_parent}, error));
    }
    BOOST_CHECK(error.original.find("Restart with -reindex-chainstate") != std::string::npos);
    tip->nStatus |= BLOCK_HAVE_UNDO;

    error = {};
    {
        auto broken{BreakTipCoins()};
        ASSERT_DEBUG_LOG(strprintf("failed to disconnect block %s at height %d", tip->GetBlockHash().ToString(), tip->nHeight));
        BOOST_CHECK(!chainstate.RollBackFromInvalidBlock(tip, SidechainDB{at_parent}, error));
        RepairCoins(std::move(broken));
    }
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(chainstate.m_chain.Tip() == tip);
    BOOST_CHECK(!(tip->nStatus & BLOCK_FAILED_VALID));
    BOOST_CHECK(chainstate.m_scdb.GetBlockHash() == tip->GetBlockHash());
}

BOOST_AUTO_TEST_CASE(roll_forward_needs_the_blocks)
{
    // Deriving the sidechain database stops at a block that cannot be read.
    const CBlock next{CreateBlock({}, coinbase_script)};
    {
        BlockValidationState state;
        const std::vector<CBlockHeader> headers{static_cast<const CBlockHeader&>(next)};
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state));
    }
    LOCK(::cs_main);
    Chainstate& chainstate{ActiveChainstate()};
    const CBlockIndex* header{m_node.chainman->m_blockman.LookupBlockIndex(next.GetHash())};
    BOOST_REQUIRE(header && !(header->nStatus & BLOCK_HAVE_DATA));
    SidechainDB scdb{chainstate.m_scdb};
    const auto rolled{chainstate.RollForwardSidechainDB(scdb, chainstate.m_chain.Tip(), header, header->nHeight, &chainstate.SideCache())};
    BOOST_CHECK(!rolled);
    BOOST_CHECK_EQUAL(util::ErrorString(rolled).original, "Failed to read block " + next.GetHash().ToString() + ". Restart with -reindex.");
    BOOST_CHECK(scdb == chainstate.m_scdb);
}

BOOST_AUTO_TEST_CASE(sweep_erases_undo_data_out_of_reach)
{
    // The undo data of blocks no reorg from the flushed block can reach goes: here that of a block
    // the node does not know (a crash took it out of the block index), with its deposit records.
    LOCK(::cs_main);
    Chainstate& chainstate{ActiveChainstate()};
    const uint256 stray{0x51};
    DrivechainDB().WriteBlock(stray, 3, BlockUndo{}, {}, chainstate.m_scdb);
    BOOST_REQUIRE(DrivechainDB().HasBlockUndo(stray));
    {
        ASSERT_DEBUG_LOG("Erasing the drivechain undo data of 1 blocks out of reach (1 not in the active chain)");
        chainstate.SweepDrivechainUndo(*chainstate.m_chain.Tip());
    }
    BOOST_CHECK(!DrivechainDB().HasBlockUndo(stray));
    BlockEvents events;
    BOOST_CHECK(DrivechainDB().ReadBlockEvents(stray, events));
    BOOST_CHECK(DrivechainDB().HasBlockUndo(chainstate.m_chain.Tip()->GetBlockHash()));
    // Nothing more to erase: nothing said.
    auto quiet{Unexpected("Erasing the drivechain undo data")};
    chainstate.SweepDrivechainUndo(*chainstate.m_chain.Tip());
}

BOOST_AUTO_TEST_CASE(deep_chain_without_old_undo_data)
{
    // Deeper than DRIVECHAIN_UNDO_DEPTH below the flushed block, blocks have no drivechain undo data:
    // it is erased in batches; the block check stops there; a reorg that deep derives the sidechain
    // database from the blocks, which a shutdown interrupts without changing anything.
    Chainstate& chainstate{ActiveChainstate()};
    WITH_LOCK(::cs_main, chainstate.m_drivechain_undo_erase_batch = 7);
    mineBlocks(DRIVECHAIN_UNDO_DEPTH + 40);
    {
        LOCK(::cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
        const int flushed{chainstate.GetLastFlushedBlock()->nHeight};
        BOOST_REQUIRE_EQUAL(flushed, chainstate.m_chain.Height());
        for (int height{1}; height <= chainstate.m_chain.Height(); ++height) {
            const bool kept{DrivechainDB().HasBlockUndo(chainstate.m_chain[height]->GetBlockHash())};
            BOOST_CHECK_MESSAGE(kept == (height > flushed - DRIVECHAIN_UNDO_DEPTH), strprintf("height %d", height));
        }
    }
    {
        LOCK(::cs_main);
        ASSERT_DEBUG_LOG("(drivechain undo data is kept for the last");
        BOOST_CHECK(CVerifyDB{*m_node.notifications}.VerifyDB(chainstate, m_node.chainman->GetConsensus(), chainstate.CoinsTip(), /*nCheckLevel=*/3, /*nCheckDepth=*/0) == VerifyDBResult::SUCCESS);
    }

    const CBlockIndex* top{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    const SidechainDB at_top{WITH_LOCK(::cs_main, return chainstate.m_scdb)};
    {
        LOCK2(::cs_main, m_node.mempool->cs);
        while (DrivechainDB().HasBlockUndo(chainstate.m_chain.Tip()->GetBlockHash())) {
            BlockValidationState state;
            BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
        }
        CBlockIndex* deep{chainstate.m_chain.Tip()};
        const SidechainDB at_deep{chainstate.m_scdb};
        BOOST_REQUIRE(at_deep.GetBlockHash() == deep->GetBlockHash());

        BOOST_REQUIRE(m_interrupt());
        {
            ASSERT_DEBUG_LOG("DisconnectTip(): interrupted while disconnecting block " + deep->GetBlockHash().ToString());
            BlockValidationState state;
            BOOST_CHECK(!chainstate.DisconnectTip(state, nullptr));
            BOOST_CHECK(!state.IsError());
        }
        BOOST_REQUIRE(m_interrupt.reset());
        BOOST_CHECK(chainstate.m_chain.Tip() == deep);
        BOOST_CHECK(chainstate.m_scdb == at_deep);

        {
            ASSERT_DEBUG_LOG(strprintf("No drivechain undo data for block %s at height %d: deriving the sidechain database from the blocks", deep->GetBlockHash().ToString(), deep->nHeight));
            BlockValidationState state;
            BOOST_CHECK(chainstate.DisconnectTip(state, nullptr));
        }
        BOOST_CHECK(chainstate.m_chain.Tip() == deep->pprev);
        BOOST_CHECK(chainstate.m_scdb.GetBlockHash() == deep->pprev->GetBlockHash());
        // Deriving it brought back the undo data of the blocks below, for the rest of a deep reorg.
        BOOST_CHECK(DrivechainDB().HasBlockUndo(chainstate.m_chain.Tip()->GetBlockHash()));
        BlockValidationState state;
        BOOST_CHECK(chainstate.DisconnectTip(state, nullptr));
    }
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    LOCK(::cs_main);
    BOOST_CHECK(chainstate.m_chain.Tip() == top);
    BOOST_CHECK(chainstate.m_scdb == at_top);
}

BOOST_AUTO_TEST_CASE(snapshot_chainstate_starts_with_an_empty_database)
{
    // A chainstate loaded from a UTXO snapshot (regtest only) has none of the blocks the sidechain
    // database is derived from: it starts with an empty one at its tip.
    // (Height 110 has a snapshot in the regtest parameters.)
    mineBlocks(10);
    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this));
    LOCK(::cs_main);
    Chainstate& snapshot{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(snapshot.m_from_snapshot_blockhash);
    SidechainDB stale;
    stale.SetBlockHash(uint256{0x42});
    DrivechainDB().WriteState(snapshot.DrivechainStateName(), stale);
    bilingual_str error;
    {
        ASSERT_DEBUG_LOG("No sidechain database for the chainstate loaded from a UTXO snapshot; starting with an empty one.");
        BOOST_CHECK(LoadState(snapshot, error));
    }
    BOOST_CHECK(snapshot.m_scdb.GetBlockHash() == snapshot.m_chain.Tip()->GetBlockHash());
    BOOST_CHECK(snapshot.m_scdb.GetSlots().empty());
}

BOOST_AUTO_TEST_CASE(sidechain_escrow_without_mempool)
{
    // What the wallet asks of the chain about a sidechain's escrow, with and without a mempool.
    ActivateSidechain(0);
    CreateAndProcessBlock({FirstDeposit(0, COIN, 1)}, coinbase_script);
    auto chain{interfaces::MakeChain(m_node)};
    const auto with_mempool{chain->getSidechainEscrow(0)};
    BOOST_CHECK(with_mempool.active && with_mempool.has_output);
    std::unique_ptr<CTxMemPool> mempool{std::move(m_node.mempool)};
    const auto without{chain->getSidechainEscrow(0)};
    const auto inactive{chain->getSidechainEscrow(1)};
    const bool bmm_request{chain->getMempoolBmmRequest(0).has_value()};
    const bool in_mempool{chain->isInMempool(Txid{})};
    m_node.mempool = std::move(mempool);
    BOOST_CHECK(without.active && without.has_output);
    BOOST_CHECK(without.outpoint == with_mempool.outpoint);
    BOOST_CHECK_EQUAL(without.amount, COIN);
    BOOST_CHECK(!inactive.active && !inactive.has_output);
    BOOST_CHECK(!bmm_request);
    BOOST_CHECK(!in_mempool);
}

BOOST_AUTO_TEST_CASE(cpu_miner)
{
    if (!m_node.mining) m_node.mining = interfaces::MakeMining(m_node);
    node::CpuMiner miner{m_node};
    std::string error;
    BOOST_CHECK(!miner.Start(0, coinbase_script, error));
    BOOST_CHECK_EQUAL(error, "The number of threads must be at least 1");
    {
        auto mining{std::move(m_node.mining)};
        BOOST_CHECK(!miner.Start(1, coinbase_script, error));
        BOOST_CHECK_EQUAL(error, "The node is not ready to mine");
        m_node.mining = std::move(mining);
    }
    BOOST_CHECK(!miner.GetStats().running);

    // A template that cannot be made (a coinbase too large for any block) is tried again later.
    {
        std::atomic<bool> seen{false};
        const auto callback{LogInstance().PushBackCallback([&](const std::string& line) {
            if (line.find("CPU miner: cannot create a block template") != std::string::npos) seen = true;
        })};
        const std::vector<unsigned char> huge(MAX_BLOCK_SERIALIZED_SIZE, OP_TRUE);
        BOOST_REQUIRE(miner.Start(1, CScript(huge.begin(), huge.end()), error));
        BOOST_CHECK(WaitFor([&] { return seen.load(); }));
        miner.Stop();
        LogInstance().DeleteCallback(callback);
        BOOST_CHECK_EQUAL(miner.GetStats().blocks_found, 0U);
        BOOST_CHECK_EQUAL(miner.GetStats().templates, 0U);
    }

    // A target no hash reaches: the miner builds a new template when the tip changes, and when the
    // one it has is old.
    miner.SetMaxTargetForTesting(arith_uint256{1});
    const int height{WITH_LOCK(::cs_main, return ActiveChainstate().m_chain.Height())};
    BOOST_REQUIRE(miner.Start(1, coinbase_script, error));
    BOOST_CHECK(WaitFor([&] { return miner.GetStats().templates >= 1 && miner.GetStats().hashes > 0; }));
    const uint64_t first{miner.GetStats().templates};
    Mine();
    BOOST_CHECK(WaitFor([&] { return miner.GetStats().templates > first; }));
    miner.Stop();
    miner.SetTemplateLifetimeForTesting(50ms);
    const uint64_t before{miner.GetStats().templates};
    BOOST_REQUIRE(miner.Start(2, coinbase_script, error));
    BOOST_CHECK(WaitFor([&] { return miner.GetStats().templates >= before + 6; }));
    miner.Stop();
    BOOST_CHECK_EQUAL(miner.GetStats().blocks_found, 0U);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return ActiveChainstate().m_chain.Height()), height + 1);

    // Without the test target, blocks are found.
    node::CpuMiner finder{m_node};
    BOOST_REQUIRE(finder.Start(1, coinbase_script, error));
    BOOST_CHECK(WaitFor([&] { return finder.GetStats().blocks_found >= 1; }));
    finder.Stop();
    BOOST_CHECK_GT(WITH_LOCK(::cs_main, return ActiveChainstate().m_chain.Height()), height + 1);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(drivechain_node_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(test_drivechain_params)
{
    // -testdrivechainparam sets every drivechain parameter on regtest, by name.
    const auto parse{[](const std::vector<std::string>& values) {
        auto args{std::make_unique<ArgsManager>()};
        SetupChainParamsBaseOptions(*args);
        std::vector<std::string> strings{"chains"};
        for (const std::string& value : values) strings.push_back("-testdrivechainparam=" + value);
        std::vector<const char*> argv;
        for (const std::string& s : strings) argv.push_back(s.c_str());
        std::string error;
        BOOST_REQUIRE_MESSAGE(args->ParseParameters(argv.size(), argv.data(), error), error);
        return args;
    }};
    const auto args{parse({"max_sidechains@5", "activation_period@6", "activation_max_failures@7", "replacement_period@8", "withdrawal_period@9",
                           "withdrawal_min_score@10", "max_pending_bundles@11", "idle_expiry_blocks@12", "upvote_expiry_blocks@13", "unvoted_forget_blocks@14"})};
    const auto params{CreateChainParams(*args, ChainType::REGTEST)};
    const Consensus::DrivechainParams& dc{params->GetConsensus().drivechain};
    BOOST_CHECK_EQUAL(dc.max_sidechains, 5U);
    BOOST_CHECK_EQUAL(dc.activation_period, 6);
    BOOST_CHECK_EQUAL(dc.activation_max_failures, 7);
    BOOST_CHECK_EQUAL(dc.replacement_period, 8);
    BOOST_CHECK_EQUAL(dc.withdrawal_period, 9);
    BOOST_CHECK_EQUAL(dc.withdrawal_min_score, 10);
    BOOST_CHECK_EQUAL(dc.max_pending_bundles, 11U);
    BOOST_CHECK_EQUAL(dc.idle_expiry_blocks, 12);
    BOOST_CHECK_EQUAL(dc.upvote_expiry_blocks, 13);
    BOOST_CHECK_EQUAL(dc.unvoted_forget_blocks, 14);
    // Other parameters, other fingerprint.
    BOOST_CHECK(ParamsFingerprint(dc) != ParamsFingerprint(CreateChainParams(*parse({}), ChainType::REGTEST)->GetConsensus().drivechain));

    BOOST_CHECK_EXCEPTION(CreateChainParams(*parse({"bogus@1"}), ChainType::REGTEST), std::runtime_error,
                          HasReason("Invalid name (bogus) for -testdrivechainparam=name@value."));
    BOOST_CHECK_EXCEPTION(CreateChainParams(*parse({"withdrawal_period"}), ChainType::REGTEST), std::runtime_error,
                          HasReason("Invalid format (withdrawal_period) for -testdrivechainparam=name@value."));
    BOOST_CHECK_EXCEPTION(CreateChainParams(*parse({"withdrawal_period@-1"}), ChainType::REGTEST), std::runtime_error,
                          HasReason("Invalid format (withdrawal_period@-1) for -testdrivechainparam=name@value."));
    BOOST_CHECK_EXCEPTION(CreateChainParams(*parse({"withdrawal_period@x"}), ChainType::REGTEST), std::runtime_error,
                          HasReason("Invalid format (withdrawal_period@x) for -testdrivechainparam=name@value."));
}

BOOST_AUTO_TEST_CASE(base58_of_another_kind)
{
    // A Base58Check string of the right length whose version byte is none of this network's.
    for (const unsigned char version : {0x01, 0x99, 0xfe}) {
        std::vector<unsigned char> data(21, 0x11);
        data[0] = version;
        const auto& params{Params()};
        if (params.Base58Prefix(CChainParams::PUBKEY_ADDRESS) == std::vector<unsigned char>{version} ||
            params.Base58Prefix(CChainParams::SCRIPT_ADDRESS) == std::vector<unsigned char>{version}) continue;
        std::string error;
        BOOST_CHECK(!IsValidDestination(DecodeDestination(EncodeBase58Check(data), error)));
        BOOST_CHECK_EQUAL(error, "Invalid or unsupported Base58-encoded address.");
    }
}

namespace {
struct MainSetup : public TestingSetup {
    MainSetup() : TestingSetup{ChainType::MAIN} {}
};
struct SignetSetup : public TestingSetup {
    SignetSetup() : TestingSetup{ChainType::SIGNET} {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(no_utxo_snapshots_outside_regtest, MainSetup)
{
    // A UTXO snapshot does not hold the drivechain state, nor the state of a sidechain: only regtest
    // takes one.
    TestingSetup& main{*this};
    AutoFile file{nullptr};
    node::SnapshotMetadata metadata{main.m_node.chainman->GetParams().MessageStart()};
    const auto result{main.m_node.chainman->ActivateSnapshot(file, metadata, /*in_memory=*/true)};
    BOOST_REQUIRE(!result);
    BOOST_CHECK_EQUAL(util::ErrorString(result).original, main.m_node.chainman->GetConsensus().sidechain.enabled ?
                                                                 "UTXO snapshots are not supported on a sidechain: the snapshot does not hold the sidechain state" :
                                                                 "UTXO snapshots are not supported with drivechains: the snapshot does not hold the drivechain state");
}

BOOST_FIXTURE_TEST_CASE(cpu_miner_on_signet, SignetSetup)
{
    // Blocks of a signet need the signature of its operator: the CPU miner does not mine them. (On
    // a sidechain no network's blocks are mined by it: the miners of the mainchain mine them.)
    TestingSetup& signet{*this};
    if (!signet.m_node.mining) signet.m_node.mining = interfaces::MakeMining(signet.m_node);
    node::CpuMiner miner{signet.m_node};
    std::string error;
    BOOST_CHECK(!miner.Start(1, CScript() << OP_TRUE, error));
    BOOST_CHECK_EQUAL(error, signet.m_node.chainman->GetConsensus().sidechain.enabled ?
                                 "The blocks of a sidechain are mined by the miners of the mainchain; use setbmm instead" :
                                 "Blocks of a signet need a signature; use contrib/signet/miner instead");
    BOOST_CHECK(!miner.GetStats().running);
}

BOOST_AUTO_TEST_SUITE_END()
