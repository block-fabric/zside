// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <drivechain/miner.h>
#include <consensus/params.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <interfaces/types.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <node/mining_args.h>
#include <node/mining_types.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <tinyformat.h>
#include <txgraph.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/feefrac.h>
#include <util/log.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbits.h>

#include <algorithm>
#include <set>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace node {

int64_t GetMinimumTime(const CBlockIndex* pindexPrev, const int64_t difficulty_adjustment_interval)
{
    int64_t min_time{pindexPrev->GetMedianTimePast() + 1};
    // Height of block to be mined.
    const int height{pindexPrev->nHeight + 1};
    // Account for BIP94 timewarp rule on all networks. This makes future
    // activation safer.
    if (height % difficulty_adjustment_interval == 0) {
        min_time = std::max<int64_t>(min_time, pindexPrev->GetBlockTime() - MAX_TIMEWARP);
    }
    // Account for the BIP54 Murch-Zawy rule on all networks: the last block of
    // a difficulty adjustment period may not be earlier than its first block.
    if (height % difficulty_adjustment_interval == difficulty_adjustment_interval - 1) {
        const int first_height{height - static_cast<int>(difficulty_adjustment_interval) + 1};
        const CBlockIndex* first_block{Assert(pindexPrev->GetAncestor(first_height))};
        min_time = std::max<int64_t>(min_time, first_block->GetBlockTime());
    }
    return min_time;
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(GetMinimumTime(pindexPrev, consensusParams.DifficultyAdjustmentInterval()),
                                       TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()))};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    const CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    chainman.GenerateCoinbaseCommitment(block, prev_block);

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

BlockAssembler::BlockAssembler(Chainstate& chainstate,
                               const CTxMemPool* mempool,
                               BlockCreateOptions options)
    : chainparams{chainstate.m_chainman.GetParams()},
      m_mempool{options.use_mempool ? mempool : nullptr},
      m_chainstate{chainstate},
      m_options{[&] {
          if (auto result{CheckMiningOptions(options, /*use_argnames=*/false)}; !result) {
              throw std::runtime_error(util::ErrorString(result).original);
          }
          // Unless told otherwise, blocks are as large as the chain allows.
          const bool default_weight{!options.block_max_weight};
          BlockCreateOptions flattened{FlattenMiningOptions(std::move(options))};
          // Transactions other than the coinbase have their own weight limit on
          // top of the limit for the whole block.
          const Consensus::Params& consensus{chainstate.m_chainman.GetConsensus()};
          const uint64_t limit{std::min<uint64_t>(consensus.max_block_weight, uint64_t{consensus.max_block_tx_weight} + *flattened.block_reserved_weight)};
          if (default_weight || *flattened.block_max_weight > limit) flattened.block_max_weight = limit;
          return flattened;
      }()}
{
}

void BlockAssembler::resetBlock()
{
    // Reserve space for fixed-size block header, txs count, and coinbase tx.
    nBlockWeight = *Assert(m_options.block_reserved_weight);
    nBlockSigOpsCost = m_options.coinbase_output_max_additional_sigops;
    // On a sidechain the coinbase pays deposits and refunds as well: room for as many as a block
    // may pay, each to an address that can take a signature check (P2PKH).
    if (m_chainstate.m_chainman.GetConsensus().sidechain.enabled) {
        nBlockSigOpsCost += WITNESS_SCALE_FACTOR * sidechain::MAX_PAYOUTS_PER_BLOCK;
    }

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock()
{
    const auto time_start{SteadyClock::now()};

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction. It is skipped by the
    // getblocktemplate RPC and mining interface consumers must not use it.
    pblock->vtx.emplace_back();

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    if (m_mempool) {
        LOCK(m_mempool->cs);
        m_mempool->StartBlockBuilding();
        addChunks();
        m_mempool->StopBlockBuilding();
    }

    // Drivechain: the messages this node puts in the coinbase, and the
    // withdrawal bundles that can be paid out. Withdrawals go last, so that
    // they spend the escrow output as the deposits in the block leave it.
    ChainstateManager& chainman{m_chainstate.m_chainman};
    chainman.m_drivechain_miner.Prune(m_chainstate.m_scdb, pindexPrev->nHeight + 1);
    const drivechain::BlockAdditions drivechain_additions{chainman.m_drivechain_miner.CreateBlockAdditions(
        m_chainstate.m_scdb, chainparams.GetConsensus().drivechain, pindexPrev->GetBlockHash(),
        std::vector<CTransactionRef>{pblock->vtx.begin() + 1, pblock->vtx.end()})};
    // A template that is asked not to take transactions from the mempool gets none from here either.
    for (size_t i{0}; m_mempool && i < drivechain_additions.withdrawals.size(); ++i) {
        const CTransactionRef& withdrawal{drivechain_additions.withdrawals[i]};
        // A withdrawal that does not fit waits for the next block.
        if (nBlockWeight + GetTransactionWeight(*withdrawal) >= *Assert(m_options.block_max_weight)) continue;
        if (nBlockSigOpsCost + WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*withdrawal) >= chainparams.GetConsensus().MaxBlockSigOpsCost(nHeight)) continue;
        pblock->vtx.push_back(withdrawal);
        pblocktemplate->vTxFees.push_back(drivechain_additions.withdrawal_fees[i]);
        pblocktemplate->vTxSigOpsCost.push_back(WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*withdrawal));
        nBlockWeight += GetTransactionWeight(*withdrawal);
        nBlockSigOpsCost += pblocktemplate->vTxSigOpsCost.back();
        nFees += drivechain_additions.withdrawal_fees[i];
        ++nBlockTx;
    }

    // Sidechain: what the coinbase has to pay because of what the mainchain did
    // and of the transactions in the block, and the withdrawal bundle to commit to.
    std::vector<CTxOut> side_outputs, side_tx_outputs;
    if (const Consensus::SidechainParams& side_params{chainparams.GetConsensus().sidechain}; side_params.enabled) {
        const sidechain::Mainchain& mainchain{*Assert(chainman.m_mainchain)};
        sidechain::State side{m_chainstate.m_scdb.m_side};
        sidechain::StateUndo undo;
        std::string reject_reason;
        // The block is built for the next mainchain block to commit to.
        if (!side.ApplyMainEvents(mainchain.Height(), mainchain, nHeight, side_params, undo, side_outputs, reject_reason)) {
            throw std::runtime_error(strprintf("%s: the block does not fit the mainchain on record (%s)", __func__, reject_reason));
        }
        const bool main_pending{side.MainPending(mainchain, nHeight, side_params)};
        // Take transactions out of the block, with what spends their outputs.
        const auto drop{[&](std::set<Txid> dropped) {
            // The fee and sigop lists have no entry for the coinbase: transaction i is entry i - 1.
            std::vector<CTransactionRef> vtx{pblock->vtx[0]};
            std::vector<CAmount> fees;
            std::vector<int64_t> sigops;
            for (size_t i{1}; i < pblock->vtx.size(); ++i) {
                const CTransaction& tx{*pblock->vtx[i]};
                const bool depends{std::any_of(tx.vin.begin(), tx.vin.end(), [&](const CTxIn& in) { return dropped.contains(in.prevout.hash); })};
                if (depends) dropped.insert(tx.GetHash());
                if (dropped.contains(tx.GetHash())) {
                    nFees -= pblocktemplate->vTxFees[i - 1];
                    nBlockWeight -= GetTransactionWeight(tx);
                    nBlockSigOpsCost -= pblocktemplate->vTxSigOpsCost[i - 1];
                    --nBlockTx;
                    continue;
                }
                vtx.push_back(pblock->vtx[i]);
                fees.push_back(pblocktemplate->vTxFees[i - 1]);
                sigops.push_back(pblocktemplate->vTxSigOpsCost[i - 1]);
            }
            pblock->vtx = std::move(vtx);
            pblocktemplate->vTxFees = std::move(fees);
            pblocktemplate->vTxSigOpsCost = std::move(sigops);
        }};
        std::optional<uint256> bundle_hash;
        std::vector<COutPoint> bundled;
        const auto bundle{side.NextBundle(nHeight, pindexPrev->GetBlockHash(), side_params, &bundled, main_pending)};
        // Not a bundle the mainchain has closed already: the block would be invalid.
        if (bundle && !mainchain.ClosedHeight(bundle->GetHash().ToUint256())) {
            // A bundle takes its withdrawals out of reach of refunds. Someone who just made a
            // withdrawal can still take it back: while every withdrawal of the bundle is recent, the
            // bundle waits for refunds that may go in this block. Once one has waited longer, the
            // bundle goes first, and requests in this block to refund one of its withdrawals leave it,
            // with what depends on them -- otherwise a refund kept pending on purpose, of ever new
            // withdrawals, would hold every withdrawal back.
            const bool overdue{std::any_of(bundled.begin(), bundled.end(), [&](const COutPoint& withdrawal) {
                const auto it{side.Withdrawals().find(withdrawal)};
                return it != side.Withdrawals().end() && nHeight - it->second.height > sidechain::REFUND_GRACE_BLOCKS;
            })};
            const bool refund_pending{!overdue && m_mempool && WITH_LOCK(m_mempool->cs, return std::any_of(bundled.begin(), bundled.end(), [&](const COutPoint& withdrawal) EXCLUSIVE_LOCKS_REQUIRED(m_mempool->cs) { return m_mempool->m_refunds.contains(withdrawal); }))};
            const std::set<COutPoint> taken(bundled.begin(), bundled.end());
            std::set<Txid> dropped;
            for (size_t i{1}; !refund_pending && i < pblock->vtx.size(); ++i) {
                const CTransaction& tx{*pblock->vtx[i]};
                const bool refunds_taken{std::any_of(tx.vout.begin(), tx.vout.end(), [&](const CTxOut& out) {
                    const auto refund{sidechain::ParseRefundScript(out.scriptPubKey)};
                    return refund && taken.contains(refund->withdrawal);
                })};
                const bool depends{std::any_of(tx.vin.begin(), tx.vin.end(), [&](const CTxIn& in) { return dropped.contains(in.prevout.hash); })};
                if (refunds_taken || depends) dropped.insert(tx.GetHash());
            }
            if (!dropped.empty()) drop(std::move(dropped));
            if (!refund_pending) {
                bundle_hash = bundle->GetHash().ToUint256();
                if (!side.StartBundle(*bundle_hash, nHeight, pindexPrev->GetBlockHash(), side_params, undo, reject_reason, main_pending)) bundle_hash.reset();
            }
        }
        // A transaction of the mempool can break the rules of the sidechain in this block: a refund of a
        // withdrawal that the mainchain events above paid, say. It is left out, with what depends on it,
        // rather than making no block at all -- the mempool is only cleaned when the tip changes.
        const sidechain::State side_before_txs{side};
        const sidechain::StateUndo undo_before_txs{undo};
        for (bool applied{false}; !applied;) {
            applied = true;
            side = side_before_txs;
            undo = undo_before_txs;
            side_tx_outputs.clear();
            for (size_t i{1}; i < pblock->vtx.size(); ++i) {
                if (!side.ApplyTx(*pblock->vtx[i], nHeight, side_params, undo, side_tx_outputs, reject_reason, main_pending)) {
                    LogInfo("%s: leaving out transaction %s, which breaks the sidechain rules in this block (%s)", __func__, pblock->vtx[i]->GetHash().ToString(), reject_reason);
                    drop({pblock->vtx[i]->GetHash()});
                    applied = false;
                    break;
                }
            }
        }
        // As many of the payouts owed as a block may pay; the rest wait for the next block.
        side_outputs = side.TakePayouts(std::move(side_outputs), std::move(side_tx_outputs), undo);
        if (bundle_hash) side_outputs.emplace_back(0, sidechain::BundleCommitScript(*bundle_hash));
    }

    const auto time_1{SteadyClock::now()};

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;

    // Construct coinbase transaction struct in parallel
    CoinbaseTx& coinbase_tx{pblocktemplate->m_coinbase_tx};
    coinbase_tx.version = coinbaseTx.version;

    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL; // Make sure timelock is enforced.
    coinbase_tx.sequence = coinbaseTx.vin[0].nSequence;

    // Add an output that spends the full coinbase reward.
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = m_options.coinbase_output_script;
    // Block subsidy + fees
    const CAmount block_reward{nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus())};
    coinbaseTx.vout[0].nValue = block_reward;
    coinbase_tx.block_reward_remaining = block_reward;
    // Sidechain payouts come right after the first output, as the rules want them.
    for (const CTxOut& output : side_outputs) {
        coinbaseTx.vout.push_back(output);
    }
    // Drivechain messages. Mining clients that build their own coinbase have to include them.
    for (const CTxOut& message : drivechain_additions.coinbase_outputs) {
        coinbaseTx.vout.push_back(message);
    }

    // Start the coinbase scriptSig with the block height as required by BIP34.
    // Mining clients are expected to append extra data to this prefix, so
    // increasing its length would reduce the space they can use and may break
    // existing clients.
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight;
    // Set script_sig_prefix here, so IPC mining clients are not affected by
    // the optional scriptSig padding below. They provide their own extraNonce,
    // and in a typical setup a pool name or realistic extraNonce already makes
    // the scriptSig long enough.
    coinbase_tx.script_sig_prefix = coinbaseTx.vin[0].scriptSig;
    if (nHeight <= 16) {
        // For blocks at heights <= 16, the BIP34-encoded height alone is only
        // one byte. Consensus requires coinbase scriptSigs to be at least two
        // bytes long (bad-cb-length), so an OP_0 is always appended at those
        // heights.
        coinbaseTx.vin[0].scriptSig << OP_0;
    }
    Assert(nHeight > 0);
    coinbaseTx.nLockTime = static_cast<uint32_t>(nHeight - 1);
    coinbase_tx.lock_time = coinbaseTx.nLockTime;

    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    m_chainstate.m_chainman.GenerateCoinbaseCommitment(*pblock, pindexPrev);

    const CTransactionRef& final_coinbase{pblock->vtx[0]};
    if (final_coinbase->HasWitness()) {
        const auto& witness_stack{final_coinbase->vin[0].scriptWitness.stack};
        // Consensus requires the coinbase witness stack to have exactly one
        // element of 32 bytes.
        Assert(witness_stack.size() == 1 && witness_stack[0].size() == 32);
        coinbase_tx.witness = uint256(witness_stack[0]);
    }
    if (const int witness_index = GetWitnessCommitmentIndex(*pblock); witness_index != NO_WITNESS_COMMITMENT) {
        Assert(witness_index >= 0 && static_cast<size_t>(witness_index) < final_coinbase->vout.size());
        coinbase_tx.required_outputs.push_back(final_coinbase->vout[witness_index]);
    }
    coinbase_tx.required_outputs.insert(coinbase_tx.required_outputs.end(), drivechain_additions.coinbase_outputs.begin(), drivechain_additions.coinbase_outputs.end());

    LogDebug(BCLog::MINING, "CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce         = 0;

    if (m_options.test_block_validity) {
        if (BlockValidationState state{TestBlockValidity(m_chainstate, *pblock, /*check_pow=*/false, /*check_merkle_root=*/false)}; !state.IsValid()) {
            throw std::runtime_error(strprintf("TestBlockValidity failed: %s", state.ToString()));
        }
    }
    const auto time_2{SteadyClock::now()};

    LogDebug(BCLog::BENCH, "CreateNewBlock() chunks: %.2fms, validity: %.2fms (total %.2fms)\n",
             Ticks<MillisecondsDouble>(time_1 - time_start),
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<MillisecondsDouble>(time_2 - time_start));

    return std::move(pblocktemplate);
}

bool BlockAssembler::TestChunkBlockLimits(int64_t chunk_weight, int64_t chunk_sigops_cost) const
{
    // block_max_weight has been flattened before block assembly limit checks.
    Assert(m_options.block_max_weight);
    if (nBlockWeight + chunk_weight >= m_options.block_max_weight) {
        return false;
    }
    if (nBlockSigOpsCost + chunk_sigops_cost >= chainparams.GetConsensus().MaxBlockSigOpsCost(nHeight)) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
bool BlockAssembler::TestChunkTransactions(const std::vector<CTxMemPoolEntryRef>& txs) const
{
    for (const auto tx : txs) {
        if (!IsFinalTx(tx.get().GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
    }
    return true;
}

void BlockAssembler::AddToBlock(const CTxMemPoolEntry& entry)
{
    pblocktemplate->block.vtx.emplace_back(entry.GetSharedTx());
    pblocktemplate->vTxFees.push_back(entry.GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(entry.GetSigOpCost());
    nBlockWeight += entry.GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += entry.GetSigOpCost();
    nFees += entry.GetFee();

    if (*m_options.print_modified_fee) {
        LogInfo("fee rate %s txid %s\n",
                  CFeeRate(entry.GetModifiedFee(), entry.GetTxSize()).ToString(),
                  entry.GetTx().GetHash().ToString());
    }
}

void BlockAssembler::addChunks()
{
    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    constexpr int32_t BLOCK_FULL_ENOUGH_WEIGHT_DELTA = 4000;
    int64_t nConsecutiveFailed = 0;

    std::vector<CTxMemPoolEntry::CTxMemPoolEntryRef> selected_transactions;
    selected_transactions.reserve(MAX_CLUSTER_COUNT_LIMIT);
    FeePerWeight chunk_feerate;

    // This fills selected_transactions
    chunk_feerate = m_mempool->GetBlockBuilderChunk(selected_transactions);
    FeePerVSize chunk_feerate_vsize = ToFeePerVSize(chunk_feerate);

    while (selected_transactions.size() > 0) {
        // Check to see if min fee rate is still respected.
        if (ByRatio{chunk_feerate_vsize} < ByRatio{m_options.block_min_fee_rate->GetFeePerVSize()}) {
            // Everything else we might consider has a lower feerate
            return;
        }

        int64_t chunk_sig_ops = 0;
        int64_t chunk_weight = 0;
        for (const auto& tx : selected_transactions) {
            chunk_sig_ops += tx.get().GetSigOpCost();
            chunk_weight += tx.get().GetTxWeight();
        }

        // Check to see if this chunk will fit.
        if (!TestChunkBlockLimits(chunk_weight, chunk_sig_ops) || !TestChunkTransactions(selected_transactions)) {
            // This chunk won't fit, so we skip it and will try the next best one.
            m_mempool->SkipBuilderChunk();
            ++nConsecutiveFailed;

            // block_max_weight has been flattened before block assembly limit checks.
            Assert(m_options.block_max_weight);
            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight +
                    BLOCK_FULL_ENOUGH_WEIGHT_DELTA > *m_options.block_max_weight) {
                // Give up if we're close to full and haven't succeeded in a while
                return;
            }
        } else {
            m_mempool->IncludeBuilderChunk();

            // This chunk will fit, so add it to the block.
            nConsecutiveFailed = 0;
            for (const auto& tx : selected_transactions) {
                AddToBlock(tx);
            }
            pblocktemplate->m_package_feerates.emplace_back(chunk_feerate_vsize);
        }

        selected_transactions.clear();
        chunk_feerate = m_mempool->GetBlockBuilderChunk(selected_transactions);
        chunk_feerate_vsize = ToFeePerVSize(chunk_feerate);
    }
}

void AddMerkleRootAndCoinbase(CBlock& block, CTransactionRef coinbase, uint32_t version, uint32_t timestamp, uint32_t nonce)
{
    if (block.vtx.size() == 0) {
        block.vtx.emplace_back(coinbase);
    } else {
        block.vtx[0] = coinbase;
    }
    block.nVersion = version;
    block.nTime = timestamp;
    block.nNonce = nonce;
    block.hashMerkleRoot = BlockMerkleRoot(block);

    // Reset cached checks
    block.m_checked_witness_commitment = false;
    block.m_checked_merkle_root = false;
    block.fChecked = false;
}

namespace {
class SubmitBlockStateCatcher final : public CValidationInterface
{
public:
    uint256 m_hash;
    bool m_found{false};
    BlockValidationState m_state;

    explicit SubmitBlockStateCatcher(const uint256& hash) : m_hash{hash} {}

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        if (block->GetHash() != m_hash) return;
        // ProcessNewBlock emits BlockChecked synchronously while holding cs_main,
        // so SubmitBlock can read these fields after ProcessNewBlock returns
        // without extra synchronization.
        m_found = true;
        m_state = state;
    }
};
} // namespace

bool SubmitBlock(ChainstateManager& chainman, const std::shared_ptr<const CBlock>& block, std::string& reason, std::string& debug)
{
    reason.clear();
    debug.clear();

    // This follows the submitblock RPC's validation-state capture pattern, but
    // is intentionally kept separate from the RPC implementation. The RPC entry
    // point decodes hex, formats BIP22/JSONRPC results, and calls
    // UpdateUncommittedBlockStructures() for legacy witness handling. IPC
    // callers submit already-formed blocks and need bool + reason/debug
    // results.
    auto sc = std::make_shared<SubmitBlockStateCatcher>(block->GetHash());
    CHECK_NONFATAL(chainman.m_options.signals)->RegisterSharedValidationInterface(sc);
    bool new_block;
    bool accepted = chainman.ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    // No queue drain is needed. The BlockChecked notification used above is
    // emitted synchronously by ProcessNewBlock, unlike most validation signals.
    CHECK_NONFATAL(chainman.m_options.signals)->UnregisterSharedValidationInterface(sc);

    if (!new_block && accepted) {
        reason = "duplicate";
    } else if (!accepted && (!sc->m_found || sc->m_state.IsValid())) {
        // ProcessNewBlock can fail without a validation result, for example
        // from an activation or system error. It can also fail after a valid
        // BlockChecked result. In these cases the validation result is
        // inconclusive.
        reason = "inconclusive";
    } else if (!sc->m_found) {
        // The block was accepted but not connected, for example if it does not
        // have more work than the current tip.
        reason = "inconclusive";
    } else if (!sc->m_state.IsValid()) {
        reason = sc->m_state.GetRejectReason();
        debug = sc->m_state.GetDebugMessage();
    }
    const bool result{accepted && new_block && reason.empty()};
    CHECK_NONFATAL(result == reason.empty());
    return result;
}

void InterruptWait(KernelNotifications& kernel_notifications, bool& interrupt_wait)
{
    LOCK(kernel_notifications.m_tip_block_mutex);
    interrupt_wait = true;
    kernel_notifications.m_tip_block_cv.notify_all();
}

std::unique_ptr<CBlockTemplate> WaitAndCreateNewBlock(ChainstateManager& chainman,
                                                      KernelNotifications& kernel_notifications,
                                                      CTxMemPool* mempool,
                                                      const std::unique_ptr<CBlockTemplate>& block_template,
                                                      const BlockWaitOptions& wait_options,
                                                      const BlockCreateOptions& create_options,
                                                      bool& interrupt_wait)
{
    // Delay calculating the current template fees, just in case a new block
    // comes in before the next tick.
    CAmount current_fees = -1;

    // Alternate waiting for a new tip and checking if fees have risen.
    // The latter check is expensive so we only run it once per second.
    auto now{NodeClock::now()};
    const auto deadline = now + wait_options.timeout;
    const MillisecondsDouble tick{1000};
    const bool allow_min_difficulty{chainman.GetParams().GetConsensus().fPowAllowMinDifficultyBlocks};

    do {
        bool tip_changed{false};
        {
            WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
            // Note that wait_until() checks the predicate before waiting
            kernel_notifications.m_tip_block_cv.wait_until(lock, std::min(now + tick, deadline), [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
                AssertLockHeld(kernel_notifications.m_tip_block_mutex);
                const auto tip_block{kernel_notifications.TipBlock()};
                // We assume tip_block is set, because this is an instance
                // method on BlockTemplate and no template could have been
                // generated before a tip exists.
                tip_changed = Assume(tip_block) && tip_block != block_template->block.hashPrevBlock;
                return tip_changed || chainman.m_interrupt || interrupt_wait;
            });
            if (interrupt_wait) {
                interrupt_wait = false;
                return nullptr;
            }
        }

        if (chainman.m_interrupt) return nullptr;
        // At this point the tip changed, a full tick went by or we reached
        // the deadline.

        // Must release m_tip_block_mutex before locking cs_main, to avoid deadlocks.
        LOCK(::cs_main);

        // On test networks return a minimum difficulty block after 20 minutes
        if (!tip_changed && allow_min_difficulty) {
            const NodeClock::time_point tip_time{std::chrono::seconds{chainman.ActiveChain().Tip()->GetBlockTime()}};
            if (now > tip_time + 20min) {
                tip_changed = true;
            }
        }

        /**
         * We determine if fees increased compared to the previous template by generating
         * a fresh template. There may be more efficient ways to determine how much
         * (approximate) fees for the next block increased, perhaps more so after
         * Cluster Mempool.
         *
         * We'll also create a new template if the tip changed during this iteration.
         */
        if (wait_options.fee_threshold < MAX_MONEY || tip_changed) {
            auto new_tmpl{BlockAssembler{
                chainman.ActiveChainstate(),
                mempool,
                create_options
                }.CreateNewBlock()};

            // If the tip changed, return the new template regardless of its fees.
            if (tip_changed) return new_tmpl;

            // Calculate the original template total fees if we haven't already
            if (current_fees == -1) {
                current_fees = std::accumulate(block_template->vTxFees.begin(), block_template->vTxFees.end(), CAmount{0});
            }

            // Check if fees increased enough to return the new template
            const CAmount new_fees = std::accumulate(new_tmpl->vTxFees.begin(), new_tmpl->vTxFees.end(), CAmount{0});
            Assume(wait_options.fee_threshold != MAX_MONEY);
            if (new_fees >= current_fees + wait_options.fee_threshold) return new_tmpl;
        }

        now = NodeClock::now();
    } while (now < deadline);

    return nullptr;
}

std::optional<BlockRef> GetTip(ChainstateManager& chainman)
{
    LOCK(::cs_main);
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    if (!tip) return {};
    return BlockRef{tip->GetBlockHash(), tip->nHeight};
}

bool CooldownIfHeadersAhead(ChainstateManager& chainman, KernelNotifications& kernel_notifications, const BlockRef& last_tip, bool& interrupt_mining)
{
    uint256 last_tip_hash{last_tip.hash};

    while (const std::optional<int> remaining = chainman.BlocksAheadOfTip()) {
        const int cooldown_seconds = std::clamp(*remaining, 3, 20);
        const auto cooldown_deadline{MockableSteadyClock::now() + std::chrono::seconds{cooldown_seconds}};

        {
            WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
            kernel_notifications.m_tip_block_cv.wait_until(lock, cooldown_deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
                const auto tip_block = kernel_notifications.TipBlock();
                return chainman.m_interrupt || interrupt_mining || (tip_block && *tip_block != last_tip_hash);
            });
            if (chainman.m_interrupt || interrupt_mining) {
                interrupt_mining = false;
                return false;
            }

            // If the tip changed during the wait, extend the deadline
            const auto tip_block = kernel_notifications.TipBlock();
            if (tip_block && *tip_block != last_tip_hash) {
                last_tip_hash = *tip_block;
                continue;
            }
        }

        // No tip change and the cooldown window has expired.
        if (MockableSteadyClock::now() >= cooldown_deadline) break;
    }

    return true;
}

std::optional<BlockRef> WaitTipChanged(ChainstateManager& chainman, KernelNotifications& kernel_notifications, const uint256& current_tip, MillisecondsDouble& timeout, bool& interrupt)
{
    Assume(timeout >= 0ms); // No internal callers should use a negative timeout
    if (timeout < 0ms) timeout = 0ms;
    if (timeout > std::chrono::years{100}) timeout = std::chrono::years{100}; // Upper bound to avoid UB in std::chrono
    auto deadline{std::chrono::steady_clock::now() + timeout};
    {
        WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
        // For callers convenience, wait longer than the provided timeout
        // during startup for the tip to be non-null. That way this function
        // always returns valid tip information when possible and only
        // returns null when shutting down, not when timing out.
        kernel_notifications.m_tip_block_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return kernel_notifications.TipBlock() || chainman.m_interrupt || interrupt;
        });
        if (chainman.m_interrupt || interrupt) {
            interrupt = false;
            return {};
        }
        // At this point TipBlock is set, so continue to wait until it is
        // different then `current_tip` provided by caller.
        kernel_notifications.m_tip_block_cv.wait_until(lock, deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return Assume(kernel_notifications.TipBlock()) != current_tip || chainman.m_interrupt || interrupt;
        });
        if (chainman.m_interrupt || interrupt) {
            interrupt = false;
            return {};
        }
    }

    // Must release m_tip_block_mutex before getTip() locks cs_main, to
    // avoid deadlocks.
    return GetTip(chainman);
}

} // namespace node
